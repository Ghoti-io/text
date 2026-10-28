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
# The style sheet and selector differential: this library against Chromium, Firefox and the Servo `selectors` crate (tools/oracle/css_sheet_diff.py says what is compared and
# what a divergence is).
#
#   tools/oracle/run-css-sheet-oracle.sh [--stage S] [--work DIR] [--controls]
#
# The stages are css_sheet_diff.py's: populate, ours and servo need only the library and the built driver (CSS_SHEET_ORACLE_SERVO, the binary of
# tools/oracle/containers/css/selectors-driver, built in a Rust image with `cargo build --release --locked`; CSS_SHEET_ORACLE_NO_SERVO=1 leaves the crate out of the run, saying
# so); browsers needs the image (containers/cssprop, pinned in containers/IMAGES) and a container engine; classify needs the answers of all. With no --stage they run in turn.
#
# CSS_SHEET_ORACLE_COUNT and CSS_SHEET_ORACLE_SEED size and seed the population (the list tools/oracle/css_sheet_divergences.txt describes the default, 200 and seed 1; a hunt
# is 2,000 or more documents of another seed, which are judged by the same list without its counts).
#
# --controls makes the gate fail on purpose and requires that it does, each for the right reason: the list tampered with (a feature removed, a count changed, a class `ours`, a
# reason too short, a feature that never occurs), and this library's source compiled with one rule undone (tools/conformance/CSS_SELECTOR_PLANTS, the names in
# tools/oracle/CSS_SHEET_ORACLE_PLANTS). It needs the browsers' answers of a run of the default population, which it makes (or reads, from CSS_SHEET_ORACLE_REPLAY) once.
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
work=${CSS_SHEET_ORACLE_WORK:-$root/build/css-sheet-oracle}
while [ $# -gt 0 ]; do
	case $1 in
	--stage) stage=$2; shift 2 ;;
	--work) work=$2; shift 2 ;;
	--controls) controls=1; shift ;;
	*) echo "run-css-sheet-oracle.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done

mkdir -p "$root/build"
ours=$root/build/css-sheet-ours
build_ours() {
	# build_ours <output> [extra object linked ahead of the archive]
	out=$1
	shift
	g++ -std=c++20 -D_GNU_SOURCE -O1 -Wall -Wextra -o "$out" "$root/tools/oracle/css_sheet_ours.cpp" "$@" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -lpthread \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io" -Wl,--allow-multiple-definition
}
build_ours "$ours"

servo=""
if [ -n "$CSS_SHEET_ORACLE_SERVO" ]; then
	servo="--servo $CSS_SHEET_ORACLE_SERVO"
elif [ -n "$CSS_SHEET_ORACLE_NO_SERVO" ]; then
	echo "css sheet oracle: the Servo selectors crate is NOT in this run (CSS_SHEET_ORACLE_NO_SERVO)" >&2
else
	echo "CSS_SHEET_ORACLE_SERVO names the built tools/oracle/containers/css/selectors-driver; or set CSS_SHEET_ORACLE_NO_SERVO=1 to leave the crate out, saying so" >&2
	exit 1
fi

diff_py="python3 $root/tools/oracle/css_sheet_diff.py --ours $ours $servo"

if [ $controls = 0 ]; then
	exec $diff_py --work "$work" --stage "$stage"
fi

# ---- the controls ------------------------------------------------------------------------------------------------------

scratch=$(mktemp -d)
log=$(mktemp)
trap 'rm -rf "$scratch"; rm -f "$log"' EXIT
unset CSS_SHEET_ORACLE_COUNT CSS_SHEET_ORACLE_SEED

# the run the controls are about: the default population, the browsers asked once (or their answers read from a replay directory)
replay=${CSS_SHEET_ORACLE_REPLAY:-$scratch/base}
if [ -z "$CSS_SHEET_ORACLE_REPLAY" ]; then
	$diff_py --work "$scratch/base" --stage all >"$log" 2>&1 || { echo "the gate does not pass unedited" >&2; cat "$log" >&2; exit 1; }
else
	mkdir -p "$scratch/base"
	cp "$replay/population.jsonl" "$scratch/base/"
	$diff_py --work "$scratch/base" --stage ours >/dev/null
	[ -z "$servo" ] || $diff_py --work "$scratch/base" --stage servo >/dev/null
	$diff_py --work "$scratch/base" --stage classify --replay "$replay" >"$log" 2>&1 || { echo "the gate does not pass unedited" >&2; cat "$log" >&2; exit 1; }
fi
tail -2 "$log"
base=$scratch/base

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
	$diff_py --work "$base" --stage classify --replay "$replay" "$@"
}

list=$root/tools/oracle/css_sheet_divergences.txt
first=$(awk -F'\t' '!/^#/ && NF == 4 { print $1; exit }' "$list")
awk -F'\t' '/^#/ || NF != 4 || $1 != k { print }' k="$first" "$list" > "$scratch/no-key"
expect list-feature-missing "UNLISTED" classify --list "$scratch/no-key"
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 4 { print; next } !d { $3 = $3 + 1; d = 1 } { print }' "$list" > "$scratch/count"
expect list-count-changed "COUNT" classify --list "$scratch/count"
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 4 { print; next } { $2 = "ours" } { print }' "$list" > "$scratch/ours"
expect list-class-ours "LIST" classify --list "$scratch/ours"
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 4 { print; next } !d { $4 = "browser behavior"; d = 1 } { print }' "$list" > "$scratch/blanket"
expect list-blanket-reason "LIST" classify --list "$scratch/blanket"
{ cat "$list"; printf 'z:never-occurs\tbrowsers\t1\tplanted: a feature that no population has, so it is stale and the gate says so\n'; } > "$scratch/gone"
expect list-feature-gone "GONE" classify --list "$scratch/gone"
: > "$scratch/empty-list"
expect list-empty "UNLISTED" classify --list "$scratch/empty-list"

# this library's source with one rule undone, measured against browsers that did not change: the edits of one name are one defect
for name in $(grep -v '^#' "$root/tools/oracle/CSS_SHEET_ORACLE_PLANTS" | awk 'NF { print $1 }'); do
	rm -rf "$scratch/p-$name"
	mkdir -p "$scratch/p-$name"
	python3 - "$root" "$scratch/p-$name" "$name" <<'PY'
import os, sys
root, work, name = sys.argv[1:4]
edits = {}
for line in open(os.path.join(root, "tools/conformance/CSS_SELECTOR_PLANTS"), encoding="utf-8"):
    if line.startswith("#") or not line.strip():
        continue
    f = line.rstrip("\n").split("\t")
    if f[0] == name:
        edits.setdefault(f[1], []).append((f[2].replace("\\n", "\n"), f[3].replace("\\n", "\n")))
if not edits:
    raise SystemExit("the plant %s is not in CSS_SELECTOR_PLANTS" % name)
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
	python3 "$root/tools/oracle/css_sheet_diff.py" --ours "$scratch/p-$name/ours" --work "$scratch/plant-$name" --stage ours >/dev/null
	expect "plant-library-$name" "OURS" python3 "$root/tools/oracle/css_sheet_diff.py" --ours "$scratch/p-$name/ours" --work "$scratch/plant-$name" --stage classify --replay "$replay"
done
echo "css sheet oracle: controls done"
