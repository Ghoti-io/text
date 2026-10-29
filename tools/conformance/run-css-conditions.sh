#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Text.
#
# Ghoti.io Text is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
#
# Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
# for more details.
#
# You should have received a copy of the GNU Lesser General Public License
#
# Score the media query layer against the media query vectors of the pinned web-platform-tests (css_conditions_suite.py), and with --controls show the gate fail:
# a vector whose expectation is a lie, an empty exception list, a stale exception, a changed answer, a page that is not listed, the floor.

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
wpt=${CSS_PROPERTY_WPT:-$root/third_party/css/wpt}

if [ ! -d "$wpt/css/mediaqueries" ]; then
	echo "the pinned media query pages are not in third_party/css/wpt." >&2
	echo "Fetch them:  tools/css/fetch.sh" >&2
	exit 1
fi
command -v python3 >/dev/null 2>&1 || { echo "python3 is needed to read the pages' calls (tools/css/extract-wpt-conditions.py)" >&2; exit 1; }

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-conditions-runner
cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_conditions_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

vectors=$root/build/css-conditions-vectors
mkdir -p "$vectors"
python3 -I "$root/tools/css/extract-wpt-conditions.py" "$wpt" "$vectors" >/dev/null

suite_py=$root/tools/conformance/css_conditions_suite.py
exceptions=$root/tools/conformance/CSS_CONDITIONS_EXCEPTIONS
unscored=$root/tools/conformance/UNSCORED_CONDITIONS

if [ "$1" = "--regenerate" ]; then
	python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored" --regenerate
	exit $?
fi

if [ "$1" != "--controls" ]; then
	python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored"
	exit $?
fi

scratch=$(mktemp -d)
log=$(mktemp)
trap 'rm -rf "$scratch" "$log"' EXIT

expect() {
	what=$1 want=$2
	shift 2
	status=0
	"$@" </dev/null >"$log" 2>&1 || status=$?
	if [ "$status" = 0 ] || ! grep -q "$want" "$log"; then
		echo "control $what: the gate exited $status and did not say '$want'" >&2
		tail -30 "$log" >&2
		exit 1
	fi
	echo "control $what: caught"
}

python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored" >"$log" 2>&1 || {
	echo "the gate does not pass unedited" >&2
	cat "$log" >&2
	exit 1
}
mkdir -p "$scratch/lie"
cp "$vectors"/* "$scratch/lie/"
python3 - "$scratch/lie" <<'PY'
import json, os, sys
p = os.path.join(sys.argv[1], "media.json")
v = json.load(open(p, encoding="utf-8"))
for x in v:
    if x["kind"] == "serialize" and x["query"] == "all":
        x["expected"] = "not all"
        break
else:
    sys.exit("no 'all' vector to lie about")
json.dump(v, open(p, "w", encoding="utf-8"))
PY
expect vector-expectation "UNEXPLAINED FAILURES" python3 -I "$suite_py" "$scratch/lie" "$runner" "$exceptions" "$unscored"
: > "$scratch/empty"
line=$(awk -F'\t' '!/^#/ && NF == 5 { print; exit }' "$exceptions")
if [ -n "$line" ]; then
	expect no-exceptions "UNEXPLAINED FAILURES" python3 -I "$suite_py" "$vectors" "$runner" "$scratch/empty" "$unscored"
	id=$(printf '%s' "$line" | cut -f1)
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 5 { print; next } $1 == "'"$id"'" { $3 = "a planted answer" } { print }' "$exceptions" > "$scratch/answer"
	expect listed-answer "STALE EXCEPTIONS" python3 -I "$suite_py" "$vectors" "$runner" "$scratch/answer" "$unscored"
fi
{ cat "$exceptions"; printf 'css/control/planted.html#0\tdiv\tser:div\tdraft-newer\tplanted: this case passes "and the sentence quoted here is long"\n'; } > "$scratch/stale"
expect stale-exception "STALE EXCEPTIONS" python3 -I "$suite_py" "$vectors" "$runner" "$scratch/stale" "$unscored"
head -n -1 "$unscored" > "$scratch/unscored-short"
expect unscored-page-missing "UNSCORED VECTORS" python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$scratch/unscored-short"
{ cat "$unscored"; printf 'css/control/planted.html\t3\tplanted\n'; } > "$scratch/unscored-extra"
expect unscored-page-stale "UNSCORED VECTORS" python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$scratch/unscored-extra"
expect floor "below the floor" env CSS_CONDITIONS_MIN_SCORED=999999 python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored"
echo "css conditions gate: controls done"
