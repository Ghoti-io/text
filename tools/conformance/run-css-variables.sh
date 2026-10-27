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
# Score custom properties, arbitrary substitution and @property against the css-variables and css-properties-values-api tests of
# web-platform-tests.
#
#   tools/conformance/run-css-variables.sh [--controls]
#
# The pages are third_party/css/wpt, put there by tools/css/fetch.sh at the commit pinned in tools/css/WPT_COMMIT; the vectors are made from them
# by tools/css/extract-wpt-variables.py (python3; it reads the pages as data and evaluates nothing) on every run. With the data absent this fails
# and says how to get it; it never skips. The scoring is tools/conformance/css_variables_suite.py and says what it scores.
#
# --controls makes the gate fail on purpose, each way it should, and requires it to say why: a vector that lies, a registration that is asked to be
# refused though it is valid, lists that are missing, stale, about nothing, with another answer or another stage or no quoted sentence, a defect
# listed as an exception, a floor and a ceiling that are not met, a list of unread pages that is short or has a page that is read, and the library
# with a rule undone (tools/conformance/CSS_VARIABLES_PLANTS).
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
wpt=${CSS_VARIABLES_WPT:-$root/third_party/css/wpt}

if [ ! -d "$wpt/css/css-variables" ] || [ ! -d "$wpt/css/css-properties-values-api" ]; then
	echo "the pinned css-variables pages are not in third_party/css/wpt." >&2
	echo "Fetch them:  tools/css/fetch.sh" >&2
	exit 1
fi
command -v python3 >/dev/null 2>&1 || { echo "python3 is needed to read the pages (tools/css/extract-wpt-variables.py); install it, or run make with TEST_GATES set to leave this gate out" >&2; exit 1; }

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-variables-runner
cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_variables_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

vectors=$root/build/css-variables-vectors
mkdir -p "$vectors"
python3 -I "$root/tools/css/extract-wpt-variables.py" "$wpt" "$vectors" --unextracted "$vectors/UNEXTRACTED_VARIABLES" >/dev/null

suite_py=$root/tools/conformance/css_variables_suite.py
exceptions=$root/tools/conformance/CSS_VARIABLES_EXCEPTIONS
defects=$root/tools/conformance/CSS_VARIABLES_DEFECTS
unextracted=$root/tools/css/UNEXTRACTED_VARIABLES

score() {
	# score <vectors> <runner> <exceptions> <defects> <unextracted listed>
	python3 -I "$suite_py" "$1" "$2" "$3" "$4" --unextracted "$5" "$vectors/UNEXTRACTED_VARIABLES"
}

if [ "$1" != "--controls" ]; then
	score "$vectors" "$runner" "$exceptions" "$defects" "$unextracted"
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

score "$vectors" "$runner" "$exceptions" "$defects" "$unextracted" >"$log" 2>&1 || {
	echo "the gate does not pass unedited" >&2
	cat "$log" >&2
	exit 1
}
gate() {
	# gate <vectors> <exceptions> <defects> <unextracted>: the gate with some inputs replaced
	score "$1" "$runner" "$2" "$3" "$4"
}

# A vector that lies: a registration the library accepts is asked to be refused, a valid one is asked to be refused, a reftest's reference is another style,
# a cycle is asked to be valid.
mkdir -p "$scratch/lie"
cp "$vectors"/*.json "$scratch/lie/"
python3 - "$scratch/lie" <<'PY'
import json, os, sys
d = sys.argv[1]
reg = json.load(open(os.path.join(d, "registration.json"), encoding="utf-8"))
reg.append({"file": "css/control/planted.html", "n": 0, "line": 1, "call": "assert_invalid", "valid": False, "syntax": "<length>", "initial": "5px"})
reg.append({"file": "css/control/planted.html", "n": 1, "line": 2, "call": "assert_valid", "valid": True, "syntax": "<length>", "initial": "banana"})
json.dump(reg, open(os.path.join(d, "registration.json"), "w", encoding="utf-8"))
ref = json.load(open(os.path.join(d, "reftest.json"), encoding="utf-8"))
v = next(x for x in ref if x["file"].endswith("variable-declaration-01.html"))
v = json.loads(json.dumps(v))
v["file"] = "css/control/planted-reftest.html"
v["page"]["sheets"] = ["p { color: red; }"]
ref.append(v)
json.dump(ref, open(os.path.join(d, "reftest.json"), "w", encoding="utf-8"))
cyc = json.load(open(os.path.join(d, "cycles.json"), encoding="utf-8"))
cyc.append({"file": "css/control/planted-cycle.html", "n": 0, "line": 1, "declarations": ["--a:var(--a)"], "invalid": [], "valid": ["--a"], "description": "planted"})
json.dump(cyc, open(os.path.join(d, "cycles.json"), "w", encoding="utf-8"))
PY
cp "$vectors/UNEXTRACTED_VARIABLES" "$scratch/lie/"
python3 -I "$suite_py" "$scratch/lie" "$runner" "$exceptions" "$defects" > "$log" 2>&1 && { echo "control vector-lies: the gate passed" >&2; exit 1; }
grep -q "UNEXPLAINED FAILURES" "$log" || { echo "control vector-lies: the gate did not say UNEXPLAINED FAILURES" >&2; cat "$log" >&2; exit 1; }
grep -q "planted.html#0  invalid-accepted" "$log" || { echo "control vector-lies: an invalid registration that was accepted was not caught as invalid-accepted" >&2; cat "$log" >&2; exit 1; }
grep -q "planted.html#1  register" "$log" || { echo "control vector-lies: a valid registration that was refused was not caught at register" >&2; cat "$log" >&2; exit 1; }
grep -q "planted-reftest.html#0  style" "$log" || { echo "control vector-lies: a reftest whose reference differs was not caught at style" >&2; cat "$log" >&2; exit 1; }
grep -q "planted-cycle.html#0  cycle" "$log" || { echo "control vector-lies: a cycle asked to be valid was not caught at cycle" >&2; cat "$log" >&2; exit 1; }
echo "control vector-lies: caught"

# The lists: missing, stale, for nothing, another answer, another stage, no quoted sentence, a bad class, both lists, no owner.
: > "$scratch/empty"
expect no-lists "UNEXPLAINED FAILURES" gate "$vectors" "$scratch/empty" "$scratch/empty" "$unextracted"
first=$(awk -F'\t' '!/^#/ && NF >= 6 { print $1; exit }' "$exceptions")
[ -n "$first" ] || { echo "control lists: the exception list is empty, so nothing about it can be shown" >&2; exit 1; }
{ cat "$exceptions"; printf 'css/control/planted.html#0\tplanted\tstyle\tx\tneeds-environment\tplanted: a case that passes "quoted sentence of the draft here"\n'; } > "$scratch/stale"
expect stale-exception "STALE EXCEPTIONS" gate "$vectors" "$scratch/stale" "$defects" "$unextracted"
awk -F'\t' -v id="$first" 'BEGIN { OFS = "\t" } /^#/ || NF < 6 { print; next } $1 == id { $4 = "a planted answer" } { print }' "$exceptions" > "$scratch/answer"
expect listed-answer "STALE EXCEPTIONS" gate "$vectors" "$scratch/answer" "$defects" "$unextracted"
awk -F'\t' -v id="$first" 'BEGIN { OFS = "\t" } /^#/ || NF < 6 { print; next } $1 == id { $3 = ($3 == "register" ? "style" : "register") } { print }' "$exceptions" > "$scratch/stage"
expect listed-stage "STALE EXCEPTIONS" gate "$vectors" "$scratch/stage" "$defects" "$unextracted"
sed -E 's/"[^"]*"/"x"/g' "$exceptions" > "$scratch/word-quote"
expect reason-quotes-only-a-word "SUITE PROBLEMS" gate "$vectors" "$scratch/word-quote" "$defects" "$unextracted"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF < 6 || done { print; next } { $5 = "browser-behavior"; done = 1 } { print }' "$exceptions" > "$scratch/class"
expect class-not-one-of-the-four "SUITE PROBLEMS" gate "$vectors" "$scratch/class" "$defects" "$unextracted"
{ cat "$defects"; printf '%s\tplanted\tstyle\tx\t\tshort\n' "$first"; } > "$scratch/owner"
expect defect-without-owner "SUITE PROBLEMS" gate "$vectors" "$exceptions" "$scratch/owner" "$unextracted"
{ cat "$defects"; awk -F'\t' '!/^#/ && NF >= 6 { print; exit }' "$exceptions" | awk -F'\t' 'BEGIN { OFS = "\t" } { $5 = "C9z"; $6 = "planted: the library lacks something long enough to be said"; print }'; } > "$scratch/both"
expect defect-also-an-exception "SUITE PROBLEMS" gate "$vectors" "$exceptions" "$scratch/both" "$unextracted"
expect floor "below the floor" env CSS_VARIABLES_MIN_SCORED=999999 python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$defects"
{ cat "$defects"; printf 'css/control/planted.html#0\tplanted\tstyle\tx\tC9z\tplanted: the library lacks something long enough to be said\n'; } > "$scratch/ceiling"
expect ceiling "above the ceiling" env CSS_VARIABLES_MAX_DEFECTS=0 python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$scratch/ceiling"

# The pages the extractor does not read in full.
head -n -1 "$unextracted" > "$scratch/unext-short"
expect unextracted-page-missing "UNEXTRACTED PAGES" gate "$vectors" "$exceptions" "$defects" "$scratch/unext-short"
{ cat "$unextracted"; printf 'css/control/planted.html\t3\tplanted: a page that is read in full\n'; } > "$scratch/unext-extra"
expect unextracted-page-stale "UNEXTRACTED PAGES" gate "$vectors" "$exceptions" "$defects" "$scratch/unext-extra"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF < 3 || done { print; next } { $2 = $2 + 1; done = 1 } { print }' "$unextracted" > "$scratch/unext-count"
expect unextracted-count-differs "UNEXTRACTED PAGES" gate "$vectors" "$exceptions" "$defects" "$scratch/unext-count"

# The library with a rule undone (tools/conformance/CSS_VARIABLES_PLANTS): the gate has to fail, naming a case the rule decides.
plants=$root/tools/conformance/CSS_VARIABLES_PLANTS
status=0
awk -F'\t' '!/^#/ && NF >= 5' "$plants" > "$scratch/plant-list"
while IFS='	' read -r name rel old new expect; do
	work=$scratch/plant-$name
	mkdir -p "$work"
	python3 - "$root/src/css/$rel" "$work/$(basename "$rel")" "$old" "$new" <<'PY'
import sys
src, dst, old, new = sys.argv[1:5]
text = open(src, encoding="utf-8").read()
olds = old.encode().decode("unicode_escape").split("\x1f")
news = new.encode().decode("unicode_escape").split("\x1f")
if len(olds) != len(news):
    sys.exit("plant: %d old texts and %d new" % (len(olds), len(news)))
for o, n in zip(olds, news):
    if text.count(o) != 1:
        sys.exit("plant: the text is in %s %d times, not once" % (src, text.count(o)))
    text = text.replace(o, n)
open(dst, "w", encoding="utf-8").write(text)
PY
	cc -std=c17 -O2 -fPIC -fvisibility=hidden -DGTEXT_BUILD -I"$root/src" -I"$root/include" -I"$generated" -I"$root/src/css" $cflags \
		-c "$work/$(basename "$rel")" -o "$work/plant.o" || { echo "control $name: the plant did not compile" >&2; exit 1; }
	# linked ahead of the archive: the archive's member for the same file is not pulled in
	cc -O1 -g -D_GNU_SOURCE -o "$work/runner" "$root/tools/conformance/css_variables_suite.c" -I"$root/include" -I"$generated" $cflags \
		"$work/plant.o" "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io" || { echo "control $name: the plant did not link" >&2; exit 1; }
	pstatus=0
	python3 -I "$suite_py" "$vectors" "$work/runner" "$exceptions" "$defects" --unextracted "$unextracted" "$vectors/UNEXTRACTED_VARIABLES" >"$log" 2>&1 || pstatus=$?
	if [ "$pstatus" = 0 ] || ! grep -q "$expect" "$log"; then
		echo "control plant $name: the gate exited $pstatus and did not name '$expect'" >&2
		tail -20 "$log" >&2
		status=1
	else
		echo "control plant $name: caught"
	fi
done < "$scratch/plant-list"
exit $status
