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
# the rule vectors of the pinned pages (test_valid_rule and kin) are read by the property extractor and scored here, through insertRule
rulevec=$root/build/css-cssom-rule-vectors
mkdir -p "$rulevec"
python3 -I "$root/tools/css/extract-wpt-props.py" "$wpt" "$rulevec" --unextracted "$rulevec/UNEXTRACTED" >/dev/null
python3 -I "$root/tools/css/extract-wpt-cssom.py" "$wpt" "$vectors" "$rulevec/rule.json" >/dev/null
rm -f "$vectors/UNEXTRACTED_CSSOM.raw"

suite_py=$root/tools/conformance/css_cssom_suite.py
exceptions=$root/tools/conformance/CSS_CSSOM_EXCEPTIONS
unscored=$root/tools/conformance/UNSCORED_CSSOM
# The ledger (tools/conformance/css_ledger.py): the reasons are codes of REASON_CODES, the census (CENSUS_CSSOM) is the set of pages and tests the extractor accounts for,
# every test is in one class, and a page claimed as scored by another gate is derived from that gate's own vectors (SCORED_ELSEWHERE_CSSOM). With --resolve it writes the
# attribution into $vectors/UNEXTRACTED_CSSOM, which is what the suite compares with UNSCORED_CSSOM.
ledger=$root/tools/conformance/css_ledger.py
elsewhere=$root/tools/conformance/SCORED_ELSEWHERE_CSSOM
census=$root/tools/conformance/CENSUS_CSSOM
codes=$root/tools/conformance/REASON_CODES
run_ledger() { # vectors elsewhere census codes [options]
	v=$1 e=$2 c=$3 r=$4
	shift 4
	python3 -I "$ledger" cssom --wpt "$wpt" --vectors "$v" --rulevec "$rulevec" --elsewhere "$e" --census "$c" --codes "$r" "$@"
}
if [ "$1" = "--regenerate-ledger" ]; then
	run_ledger "$vectors" "$elsewhere" "$census" "$codes" --regenerate || exit 1
	run_ledger "$vectors" "$elsewhere" "$census" "$codes" --resolve || exit 1
	{ head -1 "$unscored"; cat "$vectors/UNEXTRACTED_CSSOM"; } > "$unscored.tmp" && mv "$unscored.tmp" "$unscored"
	exit 0
fi
run_ledger "$vectors" "$elsewhere" "$census" "$codes" --resolve || exit 1

if [ "$1" = "--regenerate" ]; then
	python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored" --regenerate
	exit $?
fi
if [ "$1" = "--regenerate-unscored" ]; then
	{ head -1 "$unscored"; cat "$vectors/UNEXTRACTED_CSSOM"; } > "$unscored.tmp" && mv "$unscored.tmp" "$unscored"
	exit 0
fi

handports=$root/tools/conformance/css_cssom_handports.py
if [ "$1" != "--controls" ]; then
	python3 -I "$handports" "$wpt" --vectors "$vectors" || exit 1
	python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored"
	exit $?
fi
python3 -I "$handports" "$wpt" --vectors "$vectors" --controls || exit 1

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
expect fraction-floor "of the touched pages are scored" env CSS_CSSOM_MIN_FRACTION=10000 python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored"
{ cat "$exceptions"; printf 'css/control/defect.html#0\tfails\tdefect\towner: planted\n'; } > "$scratch/defect"
expect defect-ceiling "defects above the ceiling" env CSS_CSSOM_MAX_DEFECTS=0 python3 -I "$suite_py" "$vectors" "$runner" "$scratch/defect" "$unscored"
# the ledger, seen to fail: each plant edits a copy of one input and the ledger has to say why it refuses
raw="$vectors/UNEXTRACTED_CSSOM.raw"
ledger_expect() { # name message vectors elsewhere census codes
	expect "$1" "$2" python3 -I "$ledger" cssom --wpt "$wpt" --vectors "$3" --rulevec "$rulevec" --elsewhere "$4" --census "$5" --codes "$6"
}
python3 -I "$ledger" cssom --wpt "$wpt" --vectors "$vectors" --rulevec "$rulevec" --elsewhere "$elsewhere" --census "$census" --codes "$codes" >"$log" 2>&1 || {
	echo "the ledger does not pass unedited" >&2
	cat "$log" >&2
	exit 1
}
mkdir -p "$scratch/led"
cp "$vectors"/* "$scratch/led/"
# a page deleted from the census
sed '100d' "$census" > "$scratch/census-missing"
ledger_expect census-page-deleted "is touched and not in the census" "$vectors" "$elsewhere" "$scratch/census-missing" "$codes"
# a page in the census that the extractor does not touch
{ cat "$census"; printf 'css/control/planted.html\t1\t1\t\t\n'; } > "$scratch/census-extra"
ledger_expect census-page-extra "is in the census and not touched" "$vectors" "$elsewhere" "$scratch/census-extra" "$codes"
# a changed test total (the pin did not change)
sed 's/^# tests	.*/# tests	20293/' "$census" > "$scratch/census-total"
ledger_expect census-test-total "header pins" "$vectors" "$elsewhere" "$scratch/census-total" "$codes"
# a page counted in two classes: one of its lines twice, with another code
mkdir -p "$scratch/led-two"
cp "$vectors"/* "$scratch/led-two/"
first=$(head -1 "$raw" | cut -f1)
{ cat "$raw"; printf '%s\t1\ttiming\n' "$first"; } > "$scratch/led-two/UNEXTRACTED_CSSOM.raw"
ledger_expect page-in-two-classes "PARTITION: $first" "$scratch/led-two" "$elsewhere" "$census" "$codes"
# a page in no class: a line of the extractor's list dropped
mkdir -p "$scratch/led-none"
cp "$vectors"/* "$scratch/led-none/"
sed '5d' "$raw" > "$scratch/led-none/UNEXTRACTED_CSSOM.raw"
ledger_expect page-in-no-class "PARTITION:" "$scratch/led-none" "$elsewhere" "$census" "$codes"
# a scored-by entry with no program in the gate it names
{ cat "$elsewhere"; printf 'css/css-conditional/at-supports-001.html\tcss-conditions\t3\tplanted\n'; } > "$scratch/else-noprog"
ledger_expect scored-by-without-program "has no program in css-conditions" "$vectors" "$scratch/else-noprog" "$census" "$codes"
# a gate the ledger does not know
{ cat "$elsewhere"; printf 'css/css-conditional/at-supports-001.html\tcss-nowhere\t3\tplanted\n'; } > "$scratch/else-gate"
ledger_expect scored-by-unknown-gate "which is not one of" "$vectors" "$scratch/else-gate" "$census" "$codes"
# a claim that is larger than the vectors the gate has
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ { print; next } !done { $3 = $3 + 5; done = 1 } { print }' "$elsewhere" > "$scratch/else-over"
ledger_expect scored-by-overclaim "derived" "$vectors" "$scratch/else-over" "$census" "$codes"
# a claim dropped (the page falls back to its helper, silently, if nothing checked)
sed '2d' "$elsewhere" > "$scratch/else-dropped"
ledger_expect scored-by-dropped "derived" "$vectors" "$scratch/else-dropped" "$census" "$codes"
# a reason code outside the set
grep -v '^unmodeled:loop	' "$codes" > "$scratch/codes-short"
ledger_expect reason-code-outside-set "REASON CODE" "$vectors" "$elsewhere" "$census" "$scratch/codes-short"
# a class that moved: a pin that differs from what the gate finds (a count that shrinks or grows is a move)
awk -F'\t' 'BEGIN { OFS = "\t" } /^helper:computed-testcommon\t/ { $4 = $4 + 1 } { print }' "$codes" > "$scratch/codes-moved"
ledger_expect class-moved "MOVED" "$vectors" "$elsewhere" "$census" "$scratch/codes-moved"
echo "css cssom gate: controls done"
