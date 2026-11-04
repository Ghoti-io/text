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

# the runner of the scoring interpreter's programs (tools/css/cssom_eval.py makes them)
script_runner=$root/build/css-cssom-script-runner
cc -O1 -g -D_GNU_SOURCE -o "$script_runner" "$root/tools/conformance/css_cssom_script_suite.c" \
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
# The scoring interpreter's gate (tools/conformance/css_cssom_script_suite.py): the programs of script.tsv are run by css_cssom_script_suite.c and scored against the page record of the two
# browsers (tools/oracle/css_page_record.py); CSS_CSSOM_SCRIPT_EXCEPTIONS and UNSCORED_LAYOUT are derived from the run and the record, and the gate fails on any difference.
script_py=$root/tools/conformance/css_cssom_script_suite.py
script_exceptions=$root/tools/conformance/CSS_CSSOM_SCRIPT_EXCEPTIONS
layout=$root/tools/conformance/UNSCORED_LAYOUT
pagerecord=$root/tools/conformance/css_cssom_pagerecord.jsonl
script_pages=$root/build/css-cssom-script-pages.txt
awk -F'\t' '!/^#/ { print $1 }' "$vectors/script-tests.tsv" | sort -u > "$script_pages"
if [ "$1" = "--script-pages" ]; then
	cat "$script_pages"
	exit 0
fi
run_script() { # exceptions layout [options]
	e=$1 l=$2
	shift 2
	python3 -I "$script_py" "$vectors" "$script_runner" "$e" "$l" --record "$pagerecord" --wpt "$wpt" "$@"
}
run_ledger() { # vectors elsewhere census codes [options]
	v=$1 e=$2 c=$3 r=$4
	shift 4
	python3 -I "$ledger" cssom --wpt "$wpt" --vectors "$v" --rulevec "$rulevec" --elsewhere "$e" --census "$c" --codes "$r" --layout "$layout" "$@"
}
if [ "$1" = "--regenerate-ledger" ]; then
	run_script "$script_exceptions" "$layout" --regenerate || exit 1
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
	run_script "$script_exceptions" "$layout" || exit 1
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
	expect "$1" "$2" python3 -I "$ledger" cssom --wpt "$wpt" --vectors "$3" --rulevec "$rulevec" --elsewhere "$4" --census "$5" --codes "$6" --layout "$layout"
}
python3 -I "$ledger" cssom --wpt "$wpt" --vectors "$vectors" --rulevec "$rulevec" --elsewhere "$elsewhere" --census "$census" --codes "$codes" --layout "$layout" >"$log" 2>&1 || {
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
# the scoring interpreter's gate, seen to fail: each plant edits a copy of one input
script_expect() { # name message vectors exceptions layout record [env assignments are in the environment]
	expect "$1" "$2" python3 -I "$script_py" "$3" "$script_runner" "$4" "$5" --record "$6" --wpt "$wpt"
}
python3 -I "$script_py" "$vectors" "$script_runner" "$script_exceptions" "$layout" --record "$pagerecord" --wpt "$wpt" >"$log" 2>&1 || {
	echo "the script gate does not pass unedited" >&2
	cat "$log" >&2
	exit 1
}
# the record: a page that changed, a count that differs, a page with no entry, another pin
python3 - "$pagerecord" "$scratch" <<'PY'
import json, sys
src, out = sys.argv[1], sys.argv[2]
lines = open(src, encoding='utf-8').read().split('\n')
header, rows = lines[0], [l for l in lines[1:] if l]
def write(name, hdr, body):
    open('%s/%s' % (out, name), 'w', encoding='utf-8').write(hdr + '\n' + '\n'.join(body) + '\n')
e = json.loads(rows[3]); e['sha'] = '0' * 64
write('record-stale', header, rows[:3] + [json.dumps(e, sort_keys=True)] + rows[4:])
for i, r in enumerate(rows):
    e = json.loads(r)
    if len(e['chromium'].get('tests', [])) > 1:
        e['chromium']['tests'] = e['chromium']['tests'][:-1]
        write('record-count', header, rows[:i] + [json.dumps(e, sort_keys=True)] + rows[i + 1:])
        break
write('record-missing', header, rows[:3] + rows[4:])
h = json.loads(header[1:]); h['wpt'] = '0' * 40
write('record-pin', '#' + json.dumps(h, sort_keys=True), rows)
h = json.loads(header[1:]); h['harness'] = '0' * 64
write('record-harness', '#' + json.dumps(h, sort_keys=True), rows)
h = json.loads(header[1:]); h['driver'] = {'page_driver.mjs': '0' * 64}
write('record-driver', '#' + json.dumps(h, sort_keys=True), rows)
PY
script_expect record-page-changed "changed since it was recorded" "$vectors" "$script_exceptions" "$layout" "$scratch/record-stale"
script_expect record-count-differs "COUNT:" "$vectors" "$script_exceptions" "$layout" "$scratch/record-count"
script_expect record-page-missing "has no entry" "$vectors" "$script_exceptions" "$layout" "$scratch/record-missing"
script_expect record-pin-differs "the record is of the suite" "$vectors" "$script_exceptions" "$layout" "$scratch/record-pin"
script_expect record-harness-differs "the record was made under testharness.js" "$vectors" "$script_exceptions" "$layout" "$scratch/record-harness"
script_expect record-driver-differs "the record was made by the page driver" "$vectors" "$script_exceptions" "$layout" "$scratch/record-driver"
# the exceptions: none, a stale one, a changed class, a changed message
grep '^#' "$script_exceptions" > "$scratch/exc-empty" || true
script_expect no-script-exceptions "UNEXPLAINED FAILURES" "$vectors" "$scratch/exc-empty" "$layout" "$pagerecord"
{ cat "$script_exceptions"; printf 'css/control/planted.html#0\tfails\tdefect\towner: planted\n'; } > "$scratch/exc-stale"
script_expect stale-script-exception "STALE EXCEPTIONS" "$vectors" "$scratch/exc-stale" "$layout" "$pagerecord"
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ { print; next } !done && $3 == "defect" { $3 = "wpt-wrong"; done = 1 } { print }' "$script_exceptions" > "$scratch/exc-class"
script_expect script-exception-class-changed "CLASS:" "$vectors" "$scratch/exc-class" "$layout" "$pagerecord"
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ { print; next } !done { $2 = $2 " a lie"; done = 1 } { print }' "$script_exceptions" > "$scratch/exc-message"
script_expect script-exception-message-changed "STALE EXCEPTIONS" "$vectors" "$scratch/exc-message" "$layout" "$pagerecord"
# the layout list: a test that reads a layout and is not listed, one listed that does not
grep '^#' "$layout" > "$scratch/layout-empty" || true
script_expect layout-unlisted "which needs a layout, and is not listed" "$vectors" "$script_exceptions" "$scratch/layout-empty" "$pagerecord"
{ cat "$layout"; printf 'css/control/planted.html#0\twidth\tplanted\n'; } > "$scratch/layout-extra"
script_expect layout-listed-but-does-not-read "no longer reads a layout" "$vectors" "$script_exceptions" "$scratch/layout-extra" "$pagerecord"
# the program: an expectation made a lie, an op dropped (an assertion removed from a test that fails)
mkdir -p "$scratch/prog-lie" "$scratch/prog-drop"
cp "$vectors"/* "$scratch/prog-lie/"
cp "$vectors"/* "$scratch/prog-drop/"
python3 - "$scratch/prog-lie/script.tsv" "$scratch/prog-drop/script.tsv" "$script_exceptions" <<'PY'
import sys
lie, drop, exc = sys.argv[1:4]
failing = {l.split('\t')[0].rsplit('#', 1)[0] for l in open(exc, encoding='utf-8', errors='replace').read().split('\n') if l and not l.startswith('#')}
lines = open(lie, 'rb').read().split(b'\n')
page = None
for i, l in enumerate(lines):
    if l.startswith(b'P\t'):
        page = l.split(b'\t')[1].decode('utf-8')
    elif page not in failing and l.startswith(b'O\tassert_equals\tr:') and l.split(b'\t')[3].startswith(b's:'):
        f = l.split(b'\t')
        f[3] = f[3] + b' a lie'
        lines[i] = b'\t'.join(f)
        break
open(lie, 'wb').write(b'\n'.join(lines))
first = open(exc, encoding='utf-8', errors='replace').read().split('\n')
page = next(l.split('\t')[0].rsplit('#', 1)[0] for l in first if l and not l.startswith('#'))
lines = open(drop, 'rb').read().split(b'\n')
out, inpage = [], False
for l in lines:
    if l.startswith(b'P\t'):
        inpage = l.split(b'\t')[1].decode('utf-8') == page
    if inpage and l.startswith((b'O\tassert_equals\t', b'O\tassert_not_equals\t', b'O\tassert_true\t', b'O\tassert_false\t', b'O\tassert_in_array\t')):
        continue
    out.append(l)
open(drop, 'wb').write(b'\n'.join(out))
PY
script_expect script-expectation-lie "UNEXPLAINED FAILURES" "$scratch/prog-lie" "$script_exceptions" "$layout" "$pagerecord"
script_expect script-op-dropped "STALE EXCEPTIONS" "$scratch/prog-drop" "$script_exceptions" "$layout" "$pagerecord"
# the floors and the ceilings
expect script-floor "below the floor" env CSS_CSSOM_SCRIPT_MIN_SCORED=9999999 python3 -I "$script_py" "$vectors" "$script_runner" "$script_exceptions" "$layout" --record "$pagerecord" --wpt "$wpt"
expect script-page-floor "below the floor" env CSS_CSSOM_SCRIPT_MIN_PAGES=9999999 python3 -I "$script_py" "$vectors" "$script_runner" "$script_exceptions" "$layout" --record "$pagerecord" --wpt "$wpt"
expect script-defect-ceiling "above the ceiling" env CSS_CSSOM_SCRIPT_MAX_DEFECTS=0 python3 -I "$script_py" "$vectors" "$script_runner" "$script_exceptions" "$layout" --record "$pagerecord" --wpt "$wpt"
expect script-layout-ceiling "above the ceiling" env CSS_CSSOM_SCRIPT_MAX_LAYOUT=0 python3 -I "$script_py" "$vectors" "$script_runner" "$script_exceptions" "$layout" --record "$pagerecord" --wpt "$wpt"
# the evaluator's own corpus, and the evaluator seen to fail
python3 -I "$root/tools/css/check-cssom-eval.py" --controls >"$log" 2>&1 || { echo "the evaluator corpus controls failed" >&2; cat "$log" >&2; exit 1; }
echo "control evaluator-defects: caught"
echo "css cssom gate: controls done"
