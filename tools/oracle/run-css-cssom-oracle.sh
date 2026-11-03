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
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
# The CSSOM differential: this library against Chromium and Firefox (tools/oracle/css_cssom_diff.py says what is compared and what a divergence is).
#
#   tools/oracle/run-css-cssom-oracle.sh [--stage S] [--work DIR] [--controls]
#
# The stages are css_cssom_diff.py's. CSS_CSSOM_ORACLE_COUNT and CSS_CSSOM_ORACLE_SEED size and seed the population (the list describes the default, 200 and seed 1; a hunt is
# 2,000 or more documents of another seed, judged by the same list without its counts). --controls makes the gate fail on purpose and requires that it does, each for the right
# reason: the list tampered with (a feature removed, a count changed, a class `ours`... a reason too short) and an answer of this library changed.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

stage=all
controls=0
work=
while [ $# -gt 0 ]; do
	case $1 in
	--stage) stage=$2; shift 2 ;;
	--work) work=$2; shift 2 ;;
	--controls) controls=1; shift ;;
	*) echo "run-css-cssom-oracle.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done
work=${work:-${CSS_CSSOM_ORACLE_WORK:-$root/build/css-cssom-oracle}}

mkdir -p "$root/build"
ours=$root/build/css-cssom-ours
build_ours() {
	out=$1
	shift
	g++ -std=c++20 -D_GNU_SOURCE -O1 -Wall -Wextra -o "$out" "$root/tools/oracle/css_cssom_ours.cpp" \
		-I"$root/include" -I"$root/src/css" -I"$generated" $cflags "$@" "$archive" $libs -lm -lpthread \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io" -Wl,--allow-multiple-definition
}
build_ours "$ours"

diff_py="python3 $root/tools/oracle/css_cssom_diff.py --ours $ours"

if [ $controls = 0 ]; then
	exec $diff_py --work "$work" --stage "$stage"
fi

# ---- the controls ------------------------------------------------------------------------------------------------------

scratch=$(mktemp -d)
log=$(mktemp)
trap 'rm -rf "$scratch"; rm -f "$log"' EXIT
unset CSS_CSSOM_ORACLE_COUNT CSS_CSSOM_ORACLE_SEED
replay=${CSS_CSSOM_ORACLE_REPLAY:-$scratch/base}
mkdir -p "$scratch/base"
if [ -z "$CSS_CSSOM_ORACLE_REPLAY" ]; then
	$diff_py --work "$scratch/base" --stage all >"$log" 2>&1 || { echo "the gate does not pass unedited" >&2; cat "$log" >&2; exit 1; }
else
	cp "$replay/population.jsonl" "$scratch/base/"
	$diff_py --work "$scratch/base" --stage ours --replay "$replay" >/dev/null
	$diff_py --work "$scratch/base" --stage classify --replay "$replay" >"$log" 2>&1 || { echo "the gate does not pass unedited" >&2; cat "$log" >&2; exit 1; }
fi
tail -2 "$log"
base=$scratch/base
list=$root/tools/oracle/css_cssom_divergences.txt

expect() {
	what=$1 want=$2
	shift 2
	status=0
	"$@" </dev/null >"$log" 2>&1 || status=$?
	if [ "$status" != 3 ] || ! grep -q "$want" "$log"; then
		echo "control $what: the gate exited $status and did not say '$want'" >&2
		tail -20 "$log" >&2
		exit 1
	fi
	echo "control $what: caught"
}
classify() {
	$diff_py --work "$base" --stage classify "$@"
}

first=$(awk -F'\t' '!/^#/ && NF == 4 { print $1; exit }' "$list")
awk -F'\t' '/^#/ || NF != 4 || $1 != k { print }' k="$first" "$list" > "$scratch/no-key"
expect list-feature-missing "UNLISTED" classify --list "$scratch/no-key"
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 4 { print; next } !d { $3 = $3 + 1; d = 1 } { print }' "$list" > "$scratch/count"
expect list-count-changed "COUNT" classify --list "$scratch/count"
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 4 { print; next } !d { $4 = "too short"; d = 1 } { print }' "$list" > "$scratch/short"
expect list-reason-short "too short" classify --list "$scratch/short"
{ cat "$list"; printf 'I:never/occurs/here\tserved-text\t1\tthis feature does not occur in any document of the population and must be reported stale\n'; } > "$scratch/stale"
expect list-stale-feature "STALE" classify --list "$scratch/stale"
# an answer of this library changed: the first document's answer for its first operation is made one the browsers did not give
python3 - "$base" <<'PY'
import json, sys
p = sys.argv[1] + "/answers-ours.json"
rows = json.load(open(p))
rows[0]["r"][1] = "NotARealError"
json.dump(rows, open(sys.argv[1] + "/answers-ours.json", "w"))
PY
expect answer-changed "UNLISTED" classify
# the driver that asked the browsers is not the one in the repository: the header of the answers names the SHA-256 of cssom_driver.mjs (in a copy of the run, which the controls
# after this one do not use)
rm -rf "$scratch/hdr"
cp -r "$base" "$scratch/hdr"
python3 - "$scratch/hdr" <<'PY'
import json, sys
p = sys.argv[1] + "/header-chromium.json"
h = json.load(open(p))
h["mounted_driver"] = {"cssom_driver.mjs": "0" * 64}
json.dump(h, open(p, "w"))
PY
expect driver-changed "HEADER" $diff_py --work "$scratch/hdr" --stage classify
# this library's source with one rule undone, measured against browsers that did not change: the edits of one name are one defect (tools/conformance/CSS_CSSOM_PLANTS)
for name in $(grep -v '^#' "$root/tools/oracle/CSS_CSSOM_ORACLE_PLANTS" | awk 'NF { print $1 }'); do
	rm -rf "$scratch/p-$name"
	mkdir -p "$scratch/p-$name"
	python3 - "$root" "$scratch/p-$name" "$name" <<'PY'
import os, sys
root, work, name = sys.argv[1:4]
edits = {}
for line in open(os.path.join(root, "tools/conformance/CSS_CSSOM_PLANTS"), encoding="utf-8"):
    if line.startswith("#") or not line.strip():
        continue
    f = line.rstrip("\n").split("\t")
    if f[0] == name:
        edits.setdefault(f[1], []).append((f[2].replace("\\n", "\n"), f[3].replace("\\n", "\n")))
if not edits:
    raise SystemExit("the plant %s is not in CSS_CSSOM_PLANTS" % name)
for f, es in edits.items():
    text = open(os.path.join(root, "src/css", f), encoding="utf-8").read()
    for old, new in es:
        if text.count(old) != 1:
            raise SystemExit("the plant %s: its text is in %s %d times, not once" % (name, f, text.count(old)))
        text = text.replace(old, new)
    open(os.path.join(work, os.path.basename(f)), "w", encoding="utf-8").write(text)
PY
	objs=""
	for f in "$scratch/p-$name"/*.c; do
		cc -std=c17 -O1 -fPIC -DGTEXT_BUILD -w -I"$root/src/css" -I"$root/src" -I"$root/include" -I"$generated" $cflags -c "$f" -o "${f%.c}.o"
		objs="$objs ${f%.c}.o"
	done
	build_ours "$scratch/p-$name/ours" $objs
	rm -rf "$scratch/plant-$name"
	mkdir -p "$scratch/plant-$name"
	cp "$base/population.jsonl" "$scratch/plant-$name/"
	python3 "$root/tools/oracle/css_cssom_diff.py" --ours "$scratch/p-$name/ours" --work "$scratch/plant-$name" --stage ours >/dev/null
	expect "plant-library-$name" "UNLISTED\|COUNT\|STALE\|OURS" python3 "$root/tools/oracle/css_cssom_diff.py" --ours "$scratch/p-$name/ours" --work "$scratch/plant-$name" --stage classify --replay "$replay"
done
echo "css cssom oracle: controls done"
