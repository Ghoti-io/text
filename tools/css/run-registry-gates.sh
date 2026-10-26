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
#
# The CSS registry gates.
#
#   tools/css/run-registry-gates.sh tables             the committed tables are what the generator writes, twice the same
#   tools/css/run-registry-gates.sh tables-controls    ...and the gate is seen to fail on a hand edit and on a changed pin
#   tools/css/run-registry-gates.sh grammars           every grammar of the pin compiles, or is listed (UNRESOLVED_REFERENCES)
#   tools/css/run-registry-gates.sh grammars-controls  ...and the gate is seen to fail (list tampered, library defects planted)
#   tools/css/run-registry-gates.sh depth              the depth ledger (tools/css/depth.tsv) has no regression
#   tools/css/run-registry-gates.sh depth-controls     ...and the gate is seen to fail
#
# The Makefile passes PREFIX, DEP_PCS and ARCHIVE (CONFORMANCE_ENV), as for the other gates built on the archive.

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
mode=$1

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

expect() {
	what=$1 want=$2
	shift 2
	status=0
	"$@" </dev/null >"$scratch/log" 2>&1 || status=$?
	if [ "$status" = 0 ] || ! grep -q "$want" "$scratch/log"; then
		echo "control $what: the gate exited $status and did not say '$want'" >&2
		tail -30 "$scratch/log" >&2
		exit 1
	fi
	echo "control $what: caught"
}

# ---- the tables ----------------------------------------------------------------------------------

tables() {
	command -v python3 >/dev/null 2>&1 || { echo "python3 is needed (tools/css/gen-css-tables.py)" >&2; exit 1; }
	[ -d third_party/css/webref/ed/css ] || { echo "the pinned webref data is not in third_party/css/webref. Fetch it:  tools/css/fetch.sh" >&2; exit 1; }
	mkdir -p "$scratch/a" "$scratch/b"
	python3 tools/css/gen-css-tables.py --out "$scratch/a" >"$scratch/gen.log" || { cat "$scratch/gen.log" >&2; exit 1; }
	python3 tools/css/gen-css-tables.py --out "$scratch/b" >/dev/null
	if ! diff -r "$scratch/a" "$scratch/b" >/dev/null; then
		echo "the generator is not deterministic: two runs differ" >&2
		diff -r "$scratch/a" "$scratch/b" | head -20 >&2
		exit 1
	fi
	if ! diff -ru "${TABLES_DIR:-src/css/tables}" "$scratch/a" >"$scratch/diff" 2>&1; then
		printf '\n### The committed CSS registry tables are stale ###\n' >&2
		head -40 "$scratch/diff" >&2
		printf '\nRegenerate with:\n  tools/css/gen-css-tables.py\n' >&2
		exit 1
	fi
	printf 'The CSS registry tables are byte-identical to the generator output (%s)\n' "$(cat "$scratch/gen.log")"
}

tables_controls() {
	tables >/dev/null
	cp -r src/css/tables "$scratch/hand"
	printf '/* a hand edit */\n' >> "$scratch/hand/css_tables_types.c"
	expect hand-edited-table "stale" env TABLES_DIR="$scratch/hand" "$root/tools/css/run-registry-gates.sh" tables
	rm "$scratch/hand/css_tables_misc.c"
	expect dropped-table "stale" env TABLES_DIR="$scratch/hand" "$root/tools/css/run-registry-gates.sh" tables
	# a changed pin: one file of a copy of the data differs from the manifest
	mkdir -p "$scratch/data"
	cp -r third_party/css/webref/ed "$scratch/data/ed"
	printf ' ' >> "$scratch/data/ed/css/css-box.json"
	expect changed-pin-file "css-box.json" python3 tools/css/gen-css-tables.py --data "$scratch/data" --out "$scratch/c"
	rm "$scratch/data/ed/css/css-box.json"
	expect missing-pin-file "css-box.json" python3 tools/css/gen-css-tables.py --data "$scratch/data" --out "$scratch/c"
	cp third_party/css/webref/ed/css/css-box.json "$scratch/data/ed/css/css-box.json"
	printf '{}' > "$scratch/data/ed/css/extra.json"
	expect extra-pin-file "extra.json" python3 tools/css/gen-css-tables.py --data "$scratch/data" --out "$scratch/c"
	rm "$scratch/data/ed/css/extra.json"
	# fetch.sh's verifier names the file too
	printf ' ' >> "$scratch/data/ed/css/css-box.json"
	expect verifier-names-file "css-box.json" tools/css/webref-verify.sh "$scratch/data" tools/css/WEBREF_SHA256 -
	echo "control tables: done"
}

# ---- the report ----------------------------------------------------------------------------------

build_report() {
	# $1: where the tool goes; the rest: extra objects linked ahead of the archive (a planted copy of a source)
	out=$1
	shift
	[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
	[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
	. "$root/tools/conformance/lib.sh"
	conformance_library
	cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
	libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
	cc -std=c17 -O1 -g -D_GNU_SOURCE -o "$out" "$root/tools/css/css_registry_report.c" "$@" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -lpthread \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
}

report() {
	mkdir -p "$root/build"
	build_report "$root/build/css-registry-report"
	"$root/build/css-registry-report" > "$root/build/css-registry-report.tsv" 2>"$root/build/css-registry-report.err"
}

FLOORS=${CSS_GRAMMAR_FLOORS:-700,100,440}

grammars() {
	command -v python3 >/dev/null 2>&1 || { echo "python3 is needed (tools/css/check-grammars.py)" >&2; exit 1; }
	report
	python3 tools/css/check-grammars.py build/css-registry-report.tsv tools/css/UNRESOLVED_REFERENCES --floors "$FLOORS"
}

plant() {
	name=$1 file=$2 pattern=$3 replacement=$4
	base=$(basename "$file" .c)
	cp "$root/src/css/$file" "$scratch/$file"
	sed -i "s#$pattern#$replacement#g" "$scratch/$file"
	if cmp -s "$root/src/css/$file" "$scratch/$file"; then
		echo "control $name: the plant did not change $file ('$pattern' is not there)" >&2
		exit 1
	fi
	. "$root/tools/conformance/lib.sh"
	conformance_library
	cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
	cc -std=c17 -O1 -fPIC -DGTEXT_BUILD -I"$root/src/css" -I"$root/src" -I"$root/include" -I"$generated" $cflags \
		-c "$scratch/$file" -o "$scratch/$base.o"
	build_report "$scratch/tool-$name" "$scratch/$base.o"
	"$scratch/tool-$name" > "$scratch/report-$name.tsv" 2>/dev/null || true
}

grammars_controls() {
	report
	rep=build/css-registry-report.tsv
	check="python3 tools/css/check-grammars.py"
	list=tools/css/UNRESOLVED_REFERENCES
	$check "$rep" "$list" --floors "$FLOORS" >"$scratch/log" 2>&1 || { echo "the grammar gate does not pass unedited" >&2; cat "$scratch/log" >&2; exit 1; }

	awk -F'\t' '/^#/ || NF < 5 { print; next } !done && $1 == "reference" { done = 1; next } { print }' "$list" > "$scratch/unlisted"
	expect unlisted-reference "UNLISTED" $check "$rep" "$scratch/unlisted" --floors "$FLOORS"
	{ cat "$list"; printf 'reference\t<nosuch-type>\tproperty color\t<nosuch-type>\tplanted: a reference that nothing needs, so the entry is stale\n'; } > "$scratch/stale"
	expect stale-reference "STALE ENTRIES" $check "$rep" "$scratch/stale" --floors "$FLOORS"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF < 5 { print; next } !done && $1 == "grammar" { $4 = $4 " planted"; done = 1 } { print }' "$list" > "$scratch/text"
	expect changed-grammar-text "STALE ENTRIES" $check "$rep" "$scratch/text" --floors "$FLOORS"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF < 5 { print; next } !done && $1 == "initial" { $5 = "x"; done = 1 } { print }' "$list" > "$scratch/reason"
	expect missing-reason "LIST PROBLEMS" $check "$rep" "$scratch/reason" --floors "$FLOORS"
	awk -F'\t' '/^#/ || NF < 5 { print; next } !done && $1 == "initial" { done = 1; next } { print }' "$list" > "$scratch/initial"
	expect unlisted-initial "UNLISTED" $check "$rep" "$scratch/initial" --floors "$FLOORS"
	expect floor "below the floor" $check "$rep" "$list" --floors 99999,100,440

	# library defects, planted in a copy of the source and linked ahead of the archive
	plant types-of-other-properties-are-not-found css_registry.c 'return others \&\& other != (size_t)-1' 'return 0 \&\& other != (size_t)-1'
	expect other-properties-types "UNLISTED" $check "$scratch/report-types-of-other-properties-are-not-found.tsv" "$list" --floors "$FLOORS"
	plant function-types-are-not-chained css_registry.c 'const GTEXT_CSS_Definition \* d = gtext_css_function_types_resolve(r->ftypes, kind, name, length);' 'const GTEXT_CSS_Definition * d = pin_resolve(r, kind, name, length, false);'
	expect function-types-not-chained "UNLISTED" $check "$scratch/report-function-types-are-not-chained.tsv" "$list" --floors "$FLOORS"
	plant global-types-are-not-found css_registry.c 'if (global != (size_t)-1) {' 'if (0) {'
	expect global-types "UNLISTED" $check "$scratch/report-global-types-are-not-found.tsv" "$list" --floors "$FLOORS"
	plant aliases-are-not-resolved css_registry.c 'next = (GTEXT_CSS_Property_Id)gtext_css_tbl_properties\[id - 1\].alias_of;' 'next = 0;'
	expect aliases-not-resolved "UNLISTED" $check "$scratch/report-aliases-are-not-resolved.tsv" "$list" --floors "$FLOORS"
	echo "grammar controls: done"
}

# ---- the depth ledger ------------------------------------------------------------------------------

depth() {
	command -v python3 >/dev/null 2>&1 || { echo "python3 is needed (tools/css/gen-depth.py)" >&2; exit 1; }
	report
	base=
	if git -C "$root" ls-files --error-unmatch tools/css/depth.tsv >/dev/null 2>&1 && git -C "$root" show HEAD:libs/text/tools/css/depth.tsv >"$scratch/baseline" 2>/dev/null; then
		base="--baseline $scratch/baseline"
	elif git -C "$root" show HEAD:tools/css/depth.tsv >"$scratch/baseline" 2>/dev/null; then
		base="--baseline $scratch/baseline"
	fi
	python3 tools/css/gen-depth.py build/css-registry-report.tsv tools/css/depth.tsv --check $base
}

depth_controls() {
	report
	rep=build/css-registry-report.tsv
	led=tools/css/depth.tsv
	gen="python3 tools/css/gen-depth.py"
	$gen "$rep" "$led" --check >"$scratch/log" 2>&1 || { echo "the depth gate does not pass unedited" >&2; cat "$scratch/log" >&2; exit 1; }
	# a property that is 0 in the measurement but 1 in the ledger is a regression
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ { print; next } !done && $1 == "property" && $3 == 0 && $7 != "" && $7 !~ /^alias/ { $3 = 1; done = 1 } { print }' "$led" > "$scratch/regress"
	cmp -s "$led" "$scratch/regress" && { echo "control regression: no property that fails to parse to claim" >&2; exit 1; }
	expect parses-regression "REGRESSION" $gen "$rep" "$scratch/regress" --check
	awk -F'\t' '/^#/ { print; next } !done && $1 == "property" { done = 1; next } { print }' "$led" > "$scratch/new"
	expect property-absent-from-ledger "NEW" $gen "$rep" "$scratch/new" --check
	{ cat "$led"; printf 'property\tnosuch-property\t1\t1\t0\t0\t\n'; } > "$scratch/stale"
	expect row-for-nothing "STALE" $gen "$rep" "$scratch/stale" --check
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ { print; next } !done && $1 == "property" && $3 == 1 { $3 = 0; done = 1 } { print }' "$led" > "$scratch/lag"
	expect ledger-out-of-date "OUT OF DATE" $gen "$rep" "$scratch/lag" --check
	# the columns the layers above fill in cannot go from 1 to 0
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ { print; next } !done && $1 == "property" { $5 = 1; done = 1 } { print }' "$led" > "$scratch/baseline"
	expect computes-regression "REGRESSION" $gen "$rep" "$led" --check --baseline "$scratch/baseline"
	# a library defect that makes the witnesses stop parsing: the CSS-wide keywords are not recognized
	plant wide-keywords-are-not-recognized css_property.c 'if (wide != GTEXT_CSS_WIDE_NONE) {' 'if (0) {'
	expect wide-keywords "REGRESSION" $gen "$scratch/report-wide-keywords-are-not-recognized.tsv" "$led" --check
	echo "depth controls: done"
}

case "$mode" in
tables) tables ;;
tables-controls) tables_controls ;;
grammars) grammars ;;
grammars-controls) grammars_controls ;;
depth) depth ;;
depth-controls) depth_controls ;;
*) echo "usage: $0 tables|tables-controls|grammars|grammars-controls|depth|depth-controls" >&2; exit 2 ;;
esac
