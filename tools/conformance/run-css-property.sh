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
# Score the CSS property layer against the property parsing tests of web-platform-tests' css/.
#
#   tools/conformance/run-css-property.sh [--controls]
#
# The pages are third_party/css/wpt, put there by tools/css/fetch.sh at the commit pinned in tools/css/WPT_COMMIT; the vectors are
# made from them by tools/css/extract-wpt-props.py (python3; it reads the harness calls as data and evaluates nothing) on every
# run. With the data absent this fails and says how to get it; it never skips. The scoring is tools/conformance/css_property_suite.py
# and says what it scores and what it leaves to later phases.
#
# --controls makes the gate fail on purpose, each way it should, and requires it to say why: a vector that lies, a vector that
# asks for a refusal of a valid value, lists that are missing, stale, about nothing, with another answer, another stage or no quoted
# sentence, a defect listed as an exception, a floor and a ceiling that are not met, a list of unreadable calls and a list of
# absent properties that differ, a ledger that regressed, and the library with one rule undone
# (tools/conformance/CSS_PROPERTY_PLANTS).
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
wpt=${CSS_PROPERTY_WPT:-$root/third_party/css/wpt}

if [ ! -f "$wpt/css/support/parsing-testcommon.js" ] || [ ! -d "$wpt/css/css-grid/parsing" ]; then
	echo "the pinned CSS property pages are not in third_party/css/wpt." >&2
	echo "Fetch them:  tools/css/fetch.sh" >&2
	exit 1
fi
command -v python3 >/dev/null 2>&1 || { echo "python3 is needed to read the pages' calls (tools/css/extract-wpt-props.py); install it, or run make with TEST_GATES set to leave this gate out" >&2; exit 1; }

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-property-runner
cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_property_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

vectors=$root/build/css-property-vectors
mkdir -p "$vectors"
python3 -I "$root/tools/css/extract-wpt-props.py" "$wpt" "$vectors" --unextracted "$vectors/UNEXTRACTED" >/dev/null

suite_py=$root/tools/conformance/css_property_suite.py
exceptions=$root/tools/conformance/CSS_PROPERTY_EXCEPTIONS
defects=$root/tools/conformance/CSS_PROPERTY_DEFECTS
absent=$root/tools/conformance/CSS_PROPERTY_ABSENT
unextracted=$root/tools/css/UNEXTRACTED
ledger=$root/tools/css/depth.tsv
depth_floor=$root/tools/css/DEPTH_FLOOR

score() {
	# score <vectors> <runner> <exceptions> <defects> <absent> <unextracted> <ledger> <depth floor>
	python3 -I "$suite_py" "$1" "$2" "$3" "$4" "$5" --unextracted "$6" "$vectors/UNEXTRACTED" --depth "$7" --depth-floor "$8"
}

if [ "$1" != "--controls" ]; then
	score "$vectors" "$runner" "$exceptions" "$defects" "$absent" "$unextracted" "$ledger" "$depth_floor"
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

score "$vectors" "$runner" "$exceptions" "$defects" "$absent" "$unextracted" "$ledger" "$depth_floor" >"$log" 2>&1 || {
	echo "the gate does not pass unedited" >&2
	cat "$log" >&2
	exit 1
}
base="$vectors $runner"
gate() {
	# gate <vectors> <exceptions> <defects> <absent> <unextracted> <ledger> <floor>: the gate with some inputs replaced
	score "$1" "$runner" "$2" "$3" "$4" "$5" "$6" "$7"
}
cp "$ledger" "$scratch/ledger"
cp "$depth_floor" "$scratch/floor"
cp -r "$vectors" "$scratch/vectors"
good() { gate "$vectors" "$exceptions" "$defects" "$absent" "$unextracted" "$ledger" "$depth_floor"; }

# A vector that lies: the expected serialization of one that passes is changed; and a value that is valid is asked to be refused.
mkdir -p "$scratch/lie"
cp "$vectors"/*.json "$scratch/lie/"
cp "$vectors/UNEXTRACTED" "$scratch/lie/"
python3 - "$scratch/lie" <<'PY'
import json, os, sys
d = sys.argv[1]
valid = json.load(open(os.path.join(d, "valid.json"), encoding="utf-8"))
for v in valid:
    if v["prop"] == "margin-top" and v["value"] == "10px" and v["file"].endswith("margin-valid.html"):
        v["expected"] = "11px"
        break
else:
    sys.exit("no margin-top: 10px vector to lie about")
json.dump(valid, open(os.path.join(d, "valid.json"), "w", encoding="utf-8"))
invalid = json.load(open(os.path.join(d, "invalid.json"), encoding="utf-8"))
invalid.append({"file": "css/control/planted.html", "n": 0, "line": 1, "call": "test_invalid_value", "kind": "invalid", "sub": "invalid",
                "links": [], "nested": False, "quirks": False, "prop": "margin-top", "value": "10px"})
json.dump(invalid, open(os.path.join(d, "invalid.json"), "w", encoding="utf-8"))
PY
expect vector-expectation "UNEXPLAINED FAILURES" gate "$scratch/lie" "$exceptions" "$defects" "$absent" "$unextracted" "$ledger" "$depth_floor"
python3 -I "$suite_py" "$scratch/lie" "$runner" "$exceptions" "$defects" "$absent" 2>&1 | grep -q "serialize: margin-top" || {
	echo "control vector-expectation: the lie was caught at the wrong stage" >&2; exit 1; }
python3 -I "$suite_py" "$scratch/lie" "$runner" "$exceptions" "$defects" "$absent" 2>&1 | grep -q "invalid-accepted: margin-top" || {
	echo "control vector-refusal: a valid value asked to be refused was not caught as invalid-accepted" >&2; exit 1; }
echo "control vector-refusal: caught"

# The lists: missing, stale, for nothing, another answer, another stage, no quoted sentence, a bad class, both lists, no owner.
: > "$scratch/empty"
expect no-lists "UNEXPLAINED FAILURES" gate "$vectors" "$scratch/empty" "$scratch/empty" "$absent" "$unextracted" "$ledger" "$depth_floor"
# (the gate's own passing: with no exceptions the defects alone do not explain the exceptions' cases)
first=$(awk -F'\t' '!/^#/ && NF >= 7 { print $1; exit }' "$exceptions")
firstd=$(awk -F'\t' '!/^#/ && NF >= 7 { print $1; exit }' "$defects")
[ -n "$first" ] && [ -n "$firstd" ] || { echo "control lists: the exception and defect lists are empty, so nothing about them can be shown" >&2; exit 1; }
for kind in exception defect; do
	if [ "$kind" = exception ]; then list=$exceptions; other=$defects; else list=$defects; other=$exceptions; fi
	line=$(awk -F'\t' '!/^#/ && NF >= 7 { print; exit }' "$list")
	id=$(printf '%s' "$line" | cut -f1)
	prop=$(printf '%s' "$line" | cut -f2)
	{ cat "$list"; printf 'css/control/planted.html#0\t%s\t10px\tparse\tINVALID\tdraft-newer\tplanted: this case passes "quoted"\n' "$prop"; } > "$scratch/stale-$kind"
	if [ "$kind" = exception ]; then
		expect stale-$kind "STALE EXCEPTIONS" gate "$vectors" "$scratch/stale-$kind" "$defects" "$absent" "$unextracted" "$ledger" "$depth_floor"
	else
		sed 's/\tdraft-newer\t.*/\tC1d\tplanted: a case that does not exist at all/' "$scratch/stale-$kind" > "$scratch/stale-$kind.2"
		expect stale-$kind "STALE EXCEPTIONS" gate "$vectors" "$exceptions" "$scratch/stale-$kind.2" "$absent" "$unextracted" "$ledger" "$depth_floor"
	fi
	awk -F'\t' -v id="$id" 'BEGIN { OFS = "\t" } /^#/ || NF < 7 { print; next } $1 == id { $5 = "a planted answer" } { print }' "$list" > "$scratch/answer-$kind"
	if [ "$kind" = exception ]; then
		expect listed-answer-$kind "STALE EXCEPTIONS" gate "$vectors" "$scratch/answer-$kind" "$defects" "$absent" "$unextracted" "$ledger" "$depth_floor"
	else
		expect listed-answer-$kind "STALE EXCEPTIONS" gate "$vectors" "$exceptions" "$scratch/answer-$kind" "$absent" "$unextracted" "$ledger" "$depth_floor"
	fi
	awk -F'\t' -v id="$id" 'BEGIN { OFS = "\t" } /^#/ || NF < 7 { print; next } $1 == id { $4 = ($4 == "parse" ? "serialize" : "parse") } { print }' "$list" > "$scratch/stage-$kind"
	if [ "$kind" = exception ]; then
		expect listed-stage-$kind "STALE EXCEPTIONS" gate "$vectors" "$scratch/stage-$kind" "$defects" "$absent" "$unextracted" "$ledger" "$depth_floor"
	else
		expect listed-stage-$kind "STALE EXCEPTIONS" gate "$vectors" "$exceptions" "$scratch/stage-$kind" "$absent" "$unextracted" "$ledger" "$depth_floor"
	fi
done
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF < 7 { print; next } { gsub(/"/, "", $7) } { print }' "$exceptions" > "$scratch/unquoted"
expect reason-without-quote "SUITE PROBLEMS" gate "$vectors" "$scratch/unquoted" "$defects" "$absent" "$unextracted" "$ledger" "$depth_floor"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF < 7 || done { print; next } { $6 = "browser-behavior"; done = 1 } { print }' "$exceptions" > "$scratch/class"
expect class-not-one-of-the-four "SUITE PROBLEMS" gate "$vectors" "$scratch/class" "$defects" "$absent" "$unextracted" "$ledger" "$depth_floor"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF < 7 || done { print; next } { $6 = ""; done = 1 } { print }' "$defects" > "$scratch/owner"
expect defect-without-owner "SUITE PROBLEMS" gate "$vectors" "$exceptions" "$scratch/owner" "$absent" "$unextracted" "$ledger" "$depth_floor"
{ cat "$exceptions"; awk -F'\t' '!/^#/ && NF >= 7 { print; exit }' "$defects"; } > "$scratch/both"
expect defect-also-an-exception "SUITE PROBLEMS" gate "$vectors" "$scratch/both" "$defects" "$absent" "$unextracted" "$ledger" "$depth_floor"
expect floor "below the floor" env CSS_PROPERTY_MIN_SCORED=999999 python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$defects" "$absent"
expect ceiling "above the ceiling" env CSS_PROPERTY_MAX_DEFECTS=0 python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$defects" "$absent"

# The calls the extractor cannot read, and the properties the pin does not have.
head -n -1 "$unextracted" > "$scratch/unext-short"
expect unextracted-file-missing "UNEXTRACTED CALLS" gate "$vectors" "$exceptions" "$defects" "$absent" "$scratch/unext-short" "$ledger" "$depth_floor"
{ cat "$unextracted"; printf 'css/control/planted.html\t3\tplanted: a file that is read in full\n'; } > "$scratch/unext-extra"
expect unextracted-file-stale "UNEXTRACTED CALLS" gate "$vectors" "$exceptions" "$defects" "$absent" "$scratch/unext-extra" "$ledger" "$depth_floor"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF < 3 || done { print; next } { $2 = $2 + 1; done = 1 } { print }' "$unextracted" > "$scratch/unext-count"
expect unextracted-count "UNEXTRACTED CALLS" gate "$vectors" "$exceptions" "$defects" "$absent" "$scratch/unext-count" "$ledger" "$depth_floor"
awk -F'\t' '/^#/ || NF < 3 { print; next } !done { done = 1; next } { print }' "$absent" > "$scratch/absent-short"
expect absent-unlisted "UNLISTED ABSENT PROPERTIES" gate "$vectors" "$exceptions" "$defects" "$scratch/absent-short" "$unextracted" "$ledger" "$depth_floor"
{ cat "$absent"; printf 'planted-property\t1\tplanted: the pin has this property after all, so the line is stale\n'; } > "$scratch/absent-stale"
expect absent-stale "STALE EXCEPTIONS" gate "$vectors" "$exceptions" "$defects" "$scratch/absent-stale" "$unextracted" "$ledger" "$depth_floor"

# The depth ledger: a property whose scored vectors passed and now do not is a regression, and a column that is not what the scored set
# says is out of date. The ledger is edited a row at a time, and the gate has to notice each.
python3 - "$ledger" "$scratch/ledger-regressed" "$scratch/ledger-stale" <<'PY'
import sys
src, regressed, stale = sys.argv[1:4]
lines = open(src, encoding="utf-8").read().split("\n")
def edit(path, want, to):
    done = False
    out = []
    for line in lines:
        c = line.split("\t")
        if not done and len(c) == 7 and c[0] == "property" and c[5] == want:
            c[5] = to
            done = True
            line = "\t".join(c)
        out.append(line)
    if not done:
        sys.exit("no property row with wpt %s" % want)
    open(path, "w", encoding="utf-8").write("\n".join(out))
edit(regressed, "0", "1")   # the ledger claims a property that does not pass: the set says 0, the ledger 1
edit(stale, "1", "0")       # the ledger does not claim a property that passes
PY
expect ledger-regression "REGRESSION" gate "$vectors" "$exceptions" "$defects" "$absent" "$unextracted" "$scratch/ledger-regressed" "$depth_floor"
expect ledger-out-of-date "OUT OF DATE" gate "$vectors" "$exceptions" "$defects" "$absent" "$unextracted" "$scratch/ledger-stale" "$depth_floor"
echo 9999 9999 0 99999 > "$scratch/floor-high"
expect depth-floor "BELOW THE FLOOR" gate "$vectors" "$exceptions" "$defects" "$absent" "$unextracted" "$ledger" "$scratch/floor-high"

# The library with one rule undone. Each is a defect a vector must catch.
plant() {
	name=$1 file=$2 old=$3 new=$4
	base=$(basename "$file" .c)
	python3 - "$root/src/css/$file" "$scratch/$file" "$old" "$new" <<'PY'
import sys
src, dst, old, new = sys.argv[1:5]
old = old.replace("\\n", "\n")
new = new.replace("\\n", "\n")
text = open(src).read()
if text.count(old) != 1:
    raise SystemExit("the plant's text is in %s %d times, not once: %r" % (src, text.count(old), old))
open(dst, "w").write(text.replace(old, new))
PY
	cc -std=c17 -O1 -fPIC -DGTEXT_BUILD -I"$root/src/css" -I"$root/src" -I"$root/include" -I"$generated" $cflags \
		-c "$scratch/$file" -o "$scratch/$base.o"
	cc -O1 -D_GNU_SOURCE -o "$scratch/runner-$name" "$root/tools/conformance/css_property_suite.c" "$scratch/$base.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	status=0
	python3 -I "$suite_py" "$vectors" "$scratch/runner-$name" "$exceptions" "$defects" "$absent" >"$log" 2>&1 || status=$?
	if [ "$status" = 0 ] || ! grep -q "UNEXPLAINED FAILURES\|STALE EXCEPTIONS" "$log"; then
		echo "control $name: the gate exited $status and did not say UNEXPLAINED FAILURES or STALE EXCEPTIONS" >&2
		tail -20 "$log" >&2
		exit 1
	fi
	echo "control $name: caught"
}
while IFS="$(printf '\t')" read -r name file old new; do
	case $name in ''|'#'*) continue ;; esac
	plant "$name" "$file" "$old" "$new"
done < "$root/tools/conformance/CSS_PROPERTY_PLANTS"
echo "css property gate: controls done"
