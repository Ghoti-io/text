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
# Score the computed values of this library against the computed-value tests of web-platform-tests' css/.
#
#   tools/conformance/run-css-computed.sh [--controls | --regenerate]
#
# The pages are third_party/css/wpt, put there by tools/css/fetch.sh at the commit pinned in tools/css/WPT_COMMIT. The vectors are made from them by
# tools/css/extract-wpt-props.py and the documents the vectors are asked in by tools/css/extract-wpt-computed-pages.py (python3; neither evaluates anything), on every
# run. The scoring is tools/conformance/css_computed_suite.py and says what it scores and what it lists.
#
# --regenerate writes CSS_COMPUTED_EXCEPTIONS, CSS_COMPUTED_DEFECTS and UNSCORED_COMPUTED from what the library answers now (tools/conformance/classify-computed.py and
# computed_classes.tsv decide how each failure is listed); a failure the table does not name stops it. --controls makes the gate fail on purpose, each way it should, and
# requires it to say why: a vector that lies, lists that are missing, stale, about nothing, with another answer or no quoted sentence, a defect listed as an exception, a
# floor and a ceiling that are not met, an unscored page that is not listed and one that is listed and is scored, and the library with one rule undone
# (tools/conformance/CSS_COMPUTED_PLANTS).
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
wpt=${CSS_PROPERTY_WPT:-$root/third_party/css/wpt}

if [ ! -f "$wpt/css/support/computed-testcommon.js" ] || [ ! -d "$wpt/css/css-grid/parsing" ]; then
	echo "the pinned CSS property pages are not in third_party/css/wpt." >&2
	echo "Fetch them:  tools/css/fetch.sh" >&2
	exit 1
fi
command -v python3 >/dev/null 2>&1 || { echo "python3 is needed to read the pages' calls; install it, or run make with TEST_GATES set to leave this gate out" >&2; exit 1; }

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-computed-runner
build_runner() {
	# build_runner <output> [extra object linked ahead of the archive]
	out=$1
	shift
	g++ -std=c++20 -O1 -g -D_GNU_SOURCE -o "$out" "$root/tools/conformance/css_computed_suite.cpp" "$@" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -lpthread \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
}
build_runner "$runner"

vectors=$root/build/css-property-vectors
mkdir -p "$vectors"
python3 -I "$root/tools/css/extract-wpt-props.py" "$wpt" "$vectors" --unextracted "$vectors/UNEXTRACTED" >/dev/null
python3 -I "$root/tools/css/extract-wpt-computed-pages.py" "$wpt" "$vectors" "$vectors/computed_pages.json" >/dev/null

suite_py=$root/tools/conformance/css_computed_suite.py
exceptions=$root/tools/conformance/CSS_COMPUTED_EXCEPTIONS
defects=$root/tools/conformance/CSS_COMPUTED_DEFECTS
unscored=$root/tools/conformance/UNSCORED_COMPUTED
pages=$vectors/computed_pages.json

score() {
	# score <vectors> <pages> <runner> <exceptions> <defects> <unscored>
	python3 -I "$suite_py" "$1" "$2" "$3" "$4" "$5" "$6"
}

if [ "$1" = "--regenerate" ]; then
	scratch=$(mktemp -d)
	trap 'rm -rf "$scratch"' EXIT
	: > "$scratch/empty"
	CSS_COMPUTED_DUMP="$scratch/failures.json" CSS_COMPUTED_UNSCORED_DUMP="$scratch/unscored" score "$vectors" "$pages" "$runner" "$scratch/empty" "$scratch/empty" "$scratch/empty" >/dev/null 2>&1 || true
	[ -f "$scratch/failures.json" ] || { echo "the gate wrote no failures" >&2; exit 1; }
	python3 -I "$root/tools/conformance/classify-computed.py" "$scratch/failures.json" "$root/tools/conformance/computed_classes.tsv" "$exceptions" "$defects"
	{
		printf '# The vectors of the computed-value gate that are not scored, by page and reason: a page this cannot model (a rule or a script that needs selectors or a browser, C3), a call\n'
		printf '# that is not a comparison of a computed value, a pseudo-element, a property the pin does not have. Written by run-css-computed.sh --regenerate; the gate fails on a page\n'
		printf '# that is not listed, a count that differs, and a listed page that is scored now. Columns: page, vectors, reason.\n'
		cat "$scratch/unscored"
	} > "$unscored"
	exit 0
fi

if [ "$1" != "--controls" ]; then
	score "$vectors" "$pages" "$runner" "$exceptions" "$defects" "$unscored"
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

score "$vectors" "$pages" "$runner" "$exceptions" "$defects" "$unscored" >"$log" 2>&1 || {
	echo "the gate does not pass unedited" >&2
	cat "$log" >&2
	exit 1
}
gate() {
	# gate <vectors> <exceptions> <defects> <unscored>: the gate with some inputs replaced
	score "$1" "$pages" "$runner" "$2" "$3" "$4"
}

# A vector that lies: the expected computed value of one that passes is changed.
mkdir -p "$scratch/lie"
cp "$vectors"/*.json "$scratch/lie/"
python3 - "$scratch/lie" <<'PY'
import json, os, sys
d = sys.argv[1]
computed = json.load(open(os.path.join(d, "computed.json"), encoding="utf-8"))
for v in computed:
    if v["call"] == "test_computed_value" and v["prop"] == "font-weight" and v["value"] == "bold" and v["file"].endswith("font-weight-computed.html"):
        v["expected"] = "701"
        break
else:
    sys.exit("no font-weight: bold vector to lie about")
json.dump(computed, open(os.path.join(d, "computed.json"), "w", encoding="utf-8"))
PY
expect vector-expectation "UNEXPLAINED FAILURES" gate "$scratch/lie" "$exceptions" "$defects" "$unscored"

# The lists: missing, stale, about nothing, another answer, no quoted sentence, a bad class, in both lists, no owner.
: > "$scratch/empty"
expect no-lists "UNEXPLAINED FAILURES" gate "$vectors" "$scratch/empty" "$scratch/empty" "$unscored"
for kind in exception defect; do
	if [ "$kind" = exception ]; then list=$exceptions; else list=$defects; fi
	line=$(awk -F'\t' '!/^#/ && NF >= 7 { print; exit }' "$list")
	[ -n "$line" ] || { echo "control lists: the $kind list is empty, so nothing about it can be shown" >&2; exit 1; }
	id=$(printf '%s' "$line" | cut -f1)
	prop=$(printf '%s' "$line" | cut -f2)
	{ cat "$list"; printf 'css/control/planted.html#0\t%s\t10px\tcompute\tplanted\t%s\tplanted: this case passes "quoted" in a page that is not there\n' "$prop" "$( [ "$kind" = exception ] && echo resolved-value || echo C2a )"; } > "$scratch/stale-$kind"
	if [ "$kind" = exception ]; then
		expect stale-$kind "STALE EXCEPTIONS" gate "$vectors" "$scratch/stale-$kind" "$defects" "$unscored"
	else
		expect stale-$kind "STALE EXCEPTIONS" gate "$vectors" "$exceptions" "$scratch/stale-$kind" "$unscored"
	fi
	awk -F'\t' -v id="$id" 'BEGIN { OFS = "\t" } /^#/ || NF < 7 { print; next } $1 == id { $5 = "a planted answer" } { print }' "$list" > "$scratch/answer-$kind"
	if [ "$kind" = exception ]; then
		expect listed-answer-$kind "STALE EXCEPTIONS" gate "$vectors" "$scratch/answer-$kind" "$defects" "$unscored"
	else
		expect listed-answer-$kind "STALE EXCEPTIONS" gate "$vectors" "$exceptions" "$scratch/answer-$kind" "$unscored"
	fi
done
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF < 7 { print; next } { gsub(/"/, "", $7) } { print }' "$exceptions" > "$scratch/unquoted"
expect reason-without-quote "SUITE PROBLEMS" gate "$vectors" "$scratch/unquoted" "$defects" "$unscored"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF < 7 || done { print; next } { $6 = "browser-behavior"; done = 1 } { print }' "$exceptions" > "$scratch/class"
expect class-not-one-of-the-five "SUITE PROBLEMS" gate "$vectors" "$scratch/class" "$defects" "$unscored"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF < 7 || done { print; next } { $6 = ""; done = 1 } { print }' "$defects" > "$scratch/owner"
expect defect-without-owner "SUITE PROBLEMS" gate "$vectors" "$exceptions" "$scratch/owner" "$unscored"
{ cat "$exceptions"; awk -F'\t' '!/^#/ && NF >= 7 { print; exit }' "$defects"; } > "$scratch/both"
expect defect-also-an-exception "SUITE PROBLEMS" gate "$vectors" "$scratch/both" "$defects" "$unscored"
expect floor "below the floor" env CSS_COMPUTED_MIN_SCORED=999999 python3 -I "$suite_py" "$vectors" "$pages" "$runner" "$exceptions" "$defects" "$unscored"
expect ceiling "above the ceiling" env CSS_COMPUTED_MAX_DEFECTS=0 python3 -I "$suite_py" "$vectors" "$pages" "$runner" "$exceptions" "$defects" "$unscored"

# The pages that are not scored.
awk -F'\t' '/^#/ || NF < 3 { print; next } !done { done = 1; next } { print }' "$unscored" > "$scratch/unscored-short"
expect unscored-page-unlisted "UNSCORED PAGES" gate "$vectors" "$exceptions" "$defects" "$scratch/unscored-short"
{ cat "$unscored"; printf 'css/control/planted.html\t3\tplanted: a page that is scored in full or does not exist at all\n'; } > "$scratch/unscored-stale"
expect unscored-page-stale "UNSCORED PAGES" gate "$vectors" "$exceptions" "$defects" "$scratch/unscored-stale"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF < 3 || done { print; next } { $2 = $2 + 1; done = 1 } { print }' "$unscored" > "$scratch/unscored-count"
expect unscored-page-count "UNSCORED PAGES" gate "$vectors" "$exceptions" "$defects" "$scratch/unscored-count"

# The library with the edits of tools/conformance/CSS_COMPUTED_PLANTS undone (each a defect a vector must catch).
plants=$(awk -F'\t' '!/^#/ && NF == 4 { print $1 }' "$root/tools/conformance/CSS_COMPUTED_PLANTS" | awk '!seen[$0]++')
for name in $plants; do
	rm -rf "$scratch/p-$name"
	mkdir -p "$scratch/p-$name"
	python3 - "$root" "$scratch/p-$name" "$name" <<'PY'
import os, sys
root, work, name = sys.argv[1:4]
edits = {}
for line in open(os.path.join(root, "tools/conformance/CSS_COMPUTED_PLANTS"), encoding="utf-8"):
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
PY
	objs=""
	for f in $(ls "$scratch/p-$name" | grep '\.c$'); do
		cc -std=c17 -O1 -fPIC -DGTEXT_BUILD -I"$root/src/css" -I"$root/src" -I"$root/include" -I"$generated" $cflags -c "$scratch/p-$name/$f" -o "$scratch/p-$name/$(basename "$f" .c).o"
		objs="$objs $scratch/p-$name/$(basename "$f" .c).o"
	done
	build_runner "$scratch/p-$name/runner" $objs
	status=0
	python3 -I "$suite_py" "$vectors" "$pages" "$scratch/p-$name/runner" "$exceptions" "$defects" "$unscored" >"$log" 2>&1 || status=$?
	if [ "$status" = 0 ] || ! grep -q "UNEXPLAINED FAILURES\|STALE EXCEPTIONS" "$log"; then
		echo "control plant-$name: the gate exited $status and did not say UNEXPLAINED FAILURES or STALE EXCEPTIONS" >&2
		tail -20 "$log" >&2
		exit 1
	fi
	echo "control plant-$name: caught"
done
echo "css computed gate: controls done"
