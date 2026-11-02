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
# Score the CSSOM declaration object against the pages of the pinned web-platform-tests that touch the CSSOM IDL (extract-wpt-cssom.py reads them, css_cssom_suite.c runs the
# recognized calls, css_cssom_suite.py scores), and with --controls show the gate fail: a test whose expectation is a lie, an empty exception list, a stale exception, a changed
# answer, a page that is not listed, a page listed that is read, the floors and the ceiling of defects.

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
wpt=${CSS_PROPERTY_WPT:-$root/third_party/css/wpt}

if [ ! -d "$wpt/css/cssom" ]; then
	echo "the pinned CSSOM pages are not in third_party/css/wpt." >&2
	echo "Fetch them:  tools/css/fetch.sh" >&2
	exit 1
fi
command -v python3 >/dev/null 2>&1 || { echo "python3 is needed to read the pages' calls (tools/css/extract-wpt-cssom.py)" >&2; exit 1; }

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-cssom-runner
cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_cssom_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

vectors=$root/build/css-cssom-vectors
mkdir -p "$vectors"
python3 -I "$root/tools/css/extract-wpt-cssom.py" "$wpt" "$vectors" >/dev/null

suite_py=$root/tools/conformance/css_cssom_suite.py
exceptions=$root/tools/conformance/CSS_CSSOM_EXCEPTIONS
unscored=$root/tools/conformance/UNSCORED_CSSOM

if [ "$1" = "--regenerate" ]; then
	python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored" --regenerate
	exit $?
fi
if [ "$1" = "--regenerate-unscored" ]; then
	{ head -1 "$unscored"; cat "$vectors/UNEXTRACTED_CSSOM"; } > "$unscored.tmp" && mv "$unscored.tmp" "$unscored"
	exit 0
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
import os, sys
p = os.path.join(sys.argv[1], "declaration.tsv")
lines = open(p, "rb").read().split(b"\n")
for i, l in enumerate(lines):
    if l.startswith(b"O\tassert_csstext\t"):
        f = l.split(b"\t")
        f[-1] = f[-1] + b" a lie"
        lines[i] = b"\t".join(f)
        break
else:
    sys.exit("no cssText assertion to lie about")
open(p, "wb").write(b"\n".join(lines))
PY
expect test-expectation "UNEXPLAINED FAILURES" python3 -I "$suite_py" "$scratch/lie" "$runner" "$exceptions" "$unscored"
: > "$scratch/empty"
line=$(awk -F'\t' '!/^#/ && NF == 4 { print; exit }' "$exceptions")
if [ -n "$line" ]; then
	expect no-exceptions "UNEXPLAINED FAILURES" python3 -I "$suite_py" "$vectors" "$runner" "$scratch/empty" "$unscored"
	id=$(printf '%s' "$line" | cut -f1)
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 4 { print; next } $1 == "'"$id"'" { $2 = "a planted answer" } { print }' "$exceptions" > "$scratch/answer"
	expect listed-answer "STALE EXCEPTIONS" python3 -I "$suite_py" "$vectors" "$runner" "$scratch/answer" "$unscored"
fi
{ cat "$exceptions"; printf 'css/control/planted.html#0\tfails\tserved-text\tplanted: this case does not exist "and the sentence quoted here is long enough"\n'; } > "$scratch/stale"
expect stale-exception "STALE EXCEPTIONS" python3 -I "$suite_py" "$vectors" "$runner" "$scratch/stale" "$unscored"
head -n -1 "$unscored" > "$scratch/unscored-short"
expect unscored-page-missing "UNSCORED VECTORS" python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$scratch/unscored-short"
{ cat "$unscored"; printf 'css/control/planted.html\t3\tplanted\n'; } > "$scratch/unscored-extra"
expect unscored-page-stale "UNSCORED VECTORS" python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$scratch/unscored-extra"
expect floor "below the floor" env CSS_CSSOM_MIN_SCORED=999999 python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored"
expect page-floor "below the floor" env CSS_CSSOM_MIN_PAGES=999999 python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored"
{ cat "$exceptions"; printf 'css/control/defect.html#0\tfails\tdefect\towner: planted\n'; } > "$scratch/defect"
expect defect-ceiling "defects above the ceiling" env CSS_CSSOM_MAX_DEFECTS=0 python3 -I "$suite_py" "$vectors" "$runner" "$scratch/defect" "$unscored"
echo "css cssom gate: controls done"
