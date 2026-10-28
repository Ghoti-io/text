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
# The computed-style differential: this library against Chromium and Firefox (tools/oracle/css_style_diff.py says what is compared and what a divergence is).
#
#   tools/oracle/run-css-style-oracle.sh [--stage S] [--work DIR] [--controls]
#
# The stages are css_style_diff.py's: populate and ours need only the library and the pinned vectors; metrics and browsers need the image (containers/cssprop,
# pinned in containers/IMAGES) and a container engine; classify needs the answers of all three. With no --stage the five run in turn. The work directory
# (default build/css-style-oracle) holds the population and every reader's answers, so a run can be split across hosts: the first two stages in the build
# container, the third where the engine is, the last anywhere.
#
# CSS_STYLE_ORACLE_COUNT and CSS_STYLE_ORACLE_SEED size and seed the generated trees (the list tools/oracle/css_style_divergences.txt describes the default, 200
# and seed 1; a hunt is 100,000 or more, in chunks of documents by running with other seeds: each seed is its own population and needs its own list, so a hunt
# writes its findings with --write-list and classifies them). CSS_STYLE_ORACLE_SCOPE=full asks more values for each property. CSS_STYLE_ORACLE_MAX_OURS is the
# most keys of class `ours` the list may hold (default 0: a finished list holds none).
#
# --controls makes the gate fail on purpose and requires that it does, each for the right reason: an answer of ours changed, an engine's answer changed, a control
# misclassified, the header of an engine's answers changed, the list tampered with (a key removed, a key that does not occur, a count changed, class `ours`, a class
# that is not one, a blanket reason), a witness that does not hold, and this library's source compiled with one rule undone (tools/oracle/CSS_STYLE_PLANTS). It
# needs the browsers' answers of a run of the default population, which it makes (or reads, from CSS_STYLE_ORACLE_REPLAY) once.
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
work=${CSS_STYLE_ORACLE_WORK:-$root/build/css-style-oracle}
while [ $# -gt 0 ]; do
	case $1 in
	--stage) stage=$2; shift 2 ;;
	--work) work=$2; shift 2 ;;
	--controls) controls=1; shift ;;
	*) echo "run-css-style-oracle.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done

mkdir -p "$root/build"
ours=$root/build/css-style-ours
build_ours() {
	# build_ours <output> [extra object linked ahead of the archive]
	out=$1
	shift
	g++ -std=c++20 -D_GNU_SOURCE -O1 -Wall -Wextra -o "$out" "$root/tools/oracle/css_style_ours.cpp" "$@" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -lpthread \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
}
build_ours "$ours"

wpt=${CSS_PROPERTY_WPT:-$root/third_party/css/wpt}
vectors=$root/build/css-property-vectors
if [ ! -f "$vectors/valid.json" ] || [ -n "$CSS_STYLE_ORACLE_REEXTRACT" ]; then
	[ -f "$wpt/css/support/parsing-testcommon.js" ] || { echo "the pinned CSS property pages are not in third_party/css/wpt. Fetch them:  tools/css/fetch.sh" >&2; exit 1; }
	mkdir -p "$vectors"
	python3 -I "$root/tools/css/extract-wpt-props.py" "$wpt" "$vectors" >/dev/null
fi

diff_py="python3 $root/tools/oracle/css_style_diff.py --ours $ours --wpt $vectors"

if [ $controls = 0 ]; then
	exec $diff_py --work "$work" --stage "$stage"
fi

# ---- the controls ------------------------------------------------------------------------------------------------------

scratch=$(mktemp -d)
log=$(mktemp)
trap 'rm -rf "$scratch"; rm -f "$log"' EXIT
unset CSS_STYLE_ORACLE_COUNT CSS_STYLE_ORACLE_SEED CSS_STYLE_ORACLE_SCOPE

# the run the controls are about: the default population, the browsers asked once (or their answers read from a replay directory)
replay=${CSS_STYLE_ORACLE_REPLAY:-$scratch/base}
if [ -z "$CSS_STYLE_ORACLE_REPLAY" ]; then
	$diff_py --work "$scratch/base" --stage all >"$log" 2>&1 || { echo "the gate does not pass unedited" >&2; cat "$log" >&2; exit 1; }
else
	mkdir -p "$scratch/base"
	cp "$replay/population.jsonl" "$scratch/base/"
	$diff_py --work "$scratch/base" --stage ours --replay "$replay" >/dev/null
	$diff_py --work "$scratch/base" --stage classify --replay "$replay" >"$log" 2>&1 || { echo "the gate does not pass unedited" >&2; cat "$log" >&2; exit 1; }
fi
tail -3 "$log"
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
	# classify <extra arguments>: the gate on the base population and answers
	$diff_py --work "$base" --stage classify --replay "$replay" "$@"
}

for p in ours-writes-differently ours-has-no-value engine-writes-differently; do
	expect "plant-$p" "UNLISTED\|COUNT\|GONE\|DIGEST" classify --plant "$p"
done
expect plant-control-is-misclassified "CONTROL" classify --plant control-is-misclassified
for p in header-names-another-browser header-names-another-hash header-is-from-another-driver header-is-missing; do
	expect "plant-$p" "HEADER" classify --plant "$p"
done

list=$root/tools/oracle/css_style_divergences.txt
first=$(awk -F'\t' '!/^#/ && NF == 5 { print $1; exit }' "$list")
if [ -n "$first" ]; then
	awk -F'\t' -v k="$first" '/^#/ || NF != 5 || $1 != k { print }' "$list" > "$scratch/no-key"
	expect list-key-missing "UNLISTED" classify --list "$scratch/no-key"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 5 { print; next } !d { $2 = $2 + 1; d = 1 } { print }' "$list" > "$scratch/count"
	expect list-count-changed "COUNT" classify --list "$scratch/count"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 5 { print; next } !d { $4 = "0000000000000000"; d = 1 } { print }' "$list" > "$scratch/digest"
	expect list-digest-changed "DIGEST" classify --list "$scratch/digest"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 5 { print; next } $3 != "ours" { $3 = "ours" } { print }' "$list" > "$scratch/ours"
	expect list-class-ours "OURS" classify --list "$scratch/ours"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 5 { print; next } !d { $3 = "whatever"; d = 1 } { print }' "$list" > "$scratch/class"
	expect list-class-unknown "LIST" classify --list "$scratch/class"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 5 { print; next } !d { $5 = "browser behavior"; d = 1 } { print }' "$list" > "$scratch/blanket"
	expect list-blanket-reason "LIST" classify --list "$scratch/blanket"
	{ cat "$list"; printf 'ser:A|B|C:planted-property:x\t1\treference-old\t0000000000000000\tplanted: a key that never occurs in any population, so it is stale\n'; } > "$scratch/gone"
	expect list-key-gone "GONE" classify --list "$scratch/gone"
	grep -v "^$first	" "$root/tools/oracle/css_style_witnesses.txt" > "$scratch/no-witness" || true
	expect witness-missing "WITNESS" classify --witnesses "$scratch/no-witness"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 7 { print; next } !d { $6 = "OK=a planted answer"; d = 1 } { print }' "$root/tools/oracle/css_style_witnesses.txt" > "$scratch/bad-witness"
	expect witness-wrong "WITNESS" classify --witnesses "$scratch/bad-witness"
fi
: > "$scratch/empty-list"
expect list-empty "UNLISTED\|CONTROL" classify --list "$scratch/empty-list"

# this library's source with one rule undone, measured against browsers that did not change: the edits of one name are one defect
plants=$(awk -F'\t' '!/^#/ && NF == 4 { print $1 }' "$root/tools/oracle/CSS_STYLE_PLANTS" | awk '!seen[$0]++')
# The population has no style sheet, no var() and does not ask for a dump, so four of the defects are below it: the order of the cascade's sort criteria, the budget
# of substitution, the specificity of a presentation attribute against a rule of a sheet and the sort order of the dump are seen by the grammar gate (make check-css-grammar-plants) with the unit tests, and not here.
for name in $plants; do
	case $name in sort-criterion|substitution-budget|presentation-specificity|presentation-allocator|dump-order) continue ;; esac
	rm -rf "$scratch/p-$name"
	mkdir -p "$scratch/p-$name/src"
	objs=""
	python3 - "$root" "$scratch/p-$name" "$name" <<'PY'
import os, sys
root, work, name = sys.argv[1:4]
edits = {}
for line in open(os.path.join(root, "tools/oracle/CSS_STYLE_PLANTS"), encoding="utf-8"):
    if line.startswith("#") or not line.strip():
        continue
    n, f, old, new = line.rstrip("\n").split("\t")
    if n == name:
        edits.setdefault(f, []).append((old.replace("\\n", "\n"), new.replace("\\n", "\n")))
for f, es in edits.items():
    text = open(os.path.join(root, "src/css", f), encoding="utf-8").read()
    for old, new in es:
        if text.count(old) != 1:
            raise SystemExit("the plant %s: its text is in %s %d times, not once: %r" % (name, f, text.count(old), old))
        text = text.replace(old, new)
    open(os.path.join(work, f), "w", encoding="utf-8").write(text)
print(" ".join(sorted(edits)))
PY
	files=$(ls "$scratch/p-$name" | grep '\.c$')
	for f in $files; do
		cc -std=c17 -O1 -fPIC -DGTEXT_BUILD -I"$root/src/css" -I"$root/src" -I"$root/include" -I"$generated" $cflags -c "$scratch/p-$name/$f" -o "$scratch/p-$name/$(basename "$f" .c).o"
		objs="$objs $scratch/p-$name/$(basename "$f" .c).o"
	done
	build_ours "$scratch/p-$name/ours" $objs
	rm -rf "$scratch/plant-$name"
	mkdir -p "$scratch/plant-$name"
	cp "$base/population.jsonl" "$scratch/plant-$name/"
	cp "$base/metrics.tsv" "$scratch/plant-$name/" 2>/dev/null || true
	status=0
	python3 "$root/tools/oracle/css_style_diff.py" --ours "$scratch/p-$name/ours" --wpt "$vectors" --work "$scratch/plant-$name" --stage ours --replay "$replay" >/dev/null 2>&1 || status=$?
	[ $status = 0 ] || { echo "control $name: the planted library could not answer the population" >&2; exit 1; }
	expect "plant-library-$name" "UNLISTED\|COUNT\|GONE\|CONTROL\|WITNESS\|DIGEST" python3 "$root/tools/oracle/css_style_diff.py" --ours "$scratch/p-$name/ours" --wpt "$vectors" \
		--work "$scratch/plant-$name" --stage classify --replay "$replay"
done
echo "css style oracle: controls done"
