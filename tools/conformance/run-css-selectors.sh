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
# Score the selector layer against the selector parsing vectors of the pinned web-platform-tests (css_selector_suite.py), and with --controls show the gate fail:
# a vector whose expectation is a lie, a vector that is not refused, empty lists, a stale line, a reason without a quote, the floor, the unscored pages, and the
# planted defects of tools/conformance/CSS_SELECTOR_CONFORMANCE_PLANTS (each compiled alone and linked ahead of the library into a copy of the runner).

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
wpt=${CSS_PROPERTY_WPT:-$root/third_party/css/wpt}

if [ ! -f "$wpt/css/support/parsing-testcommon.js" ] || [ ! -d "$wpt/css/selectors/parsing" ]; then
	echo "the pinned CSS selector pages are not in third_party/css/wpt." >&2
	echo "Fetch them:  tools/css/fetch.sh" >&2
	exit 1
fi
command -v python3 >/dev/null 2>&1 || { echo "python3 is needed to read the pages' calls (tools/css/extract-wpt-props.py)" >&2; exit 1; }

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-selector-runner
cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_selector_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

vectors=$root/build/css-selector-vectors
mkdir -p "$vectors"
python3 -I "$root/tools/css/extract-wpt-props.py" "$wpt" "$vectors" --unextracted "$vectors/UNEXTRACTED" >/dev/null

suite_py=$root/tools/conformance/css_selector_suite.py
exceptions=$root/tools/conformance/CSS_SELECTOR_EXCEPTIONS
unscored=$root/tools/conformance/UNSCORED_SELECTORS

if [ "$1" = "--regenerate" ]; then
	python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored" --regenerate
	exit $?
fi

score() {
	python3 -I "$suite_py" "$1" "$2" "$3" "$4"
}

if [ "$1" != "--controls" ]; then
	score "$vectors" "$runner" "$exceptions" "$unscored"
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

score "$vectors" "$runner" "$exceptions" "$unscored" >"$log" 2>&1 || {
	echo "the gate does not pass unedited" >&2
	cat "$log" >&2
	exit 1
}
mkdir -p "$scratch/lie"
cp "$vectors"/*.json "$scratch/lie/"
python3 - "$scratch/lie" <<'PY'
import json, os, sys
d = sys.argv[1]
sel = json.load(open(os.path.join(d, "selector.json"), encoding="utf-8"))
for v in sel:
    if v["sub"] == "valid" and v["value"] == "[att=val]":
        v["expected"] = "[att=val]"
        v["alts"] = []
        break
else:
    sys.exit("no [att=val] vector to lie about")
sel.append({"file": "css/control/planted.html", "n": 0, "line": 1, "call": "test_invalid_selector", "kind": "selector", "sub": "invalid",
            "links": [], "nested": False, "quirks": False, "value": "div"})
json.dump(sel, open(os.path.join(d, "selector.json"), "w", encoding="utf-8"))
PY
expect vector-expectation "UNEXPLAINED FAILURES" score "$scratch/lie" "$runner" "$exceptions" "$unscored"
python3 -I "$suite_py" "$scratch/lie" "$runner" "$exceptions" "$unscored" 2>&1 | grep -q "serialize" || { echo "control vector-expectation: caught at the wrong stage" >&2; exit 1; }
python3 -I "$suite_py" "$scratch/lie" "$runner" "$exceptions" "$unscored" 2>&1 | grep -q "invalid-accepted" || { echo "control vector-refusal: a valid selector asked to be refused was not caught" >&2; exit 1; }
echo "control vector-refusal: caught"

: > "$scratch/empty"
expect no-exceptions "UNEXPLAINED FAILURES" score "$vectors" "$runner" "$scratch/empty" "$unscored"
line=$(awk -F'\t' '!/^#/ && NF == 6 { print; exit }' "$exceptions")
[ -n "$line" ] || { echo "control lists: the exception list is empty, so nothing about it can be shown" >&2; exit 1; }
{ cat "$exceptions"; printf 'css/control/planted.html#0\tdiv\tparse\tINVALID\twpt-wrong\tplanted: this case passes "quoted"\n'; } > "$scratch/stale"
expect stale-exception "STALE EXCEPTIONS" score "$vectors" "$runner" "$scratch/stale" "$unscored"
id=$(printf '%s' "$line" | cut -f1)
awk -F'\t' -v id="$id" 'BEGIN { OFS = "\t" } /^#/ || NF != 6 { print; next } $1 == id { $4 = "a planted answer" } { print }' "$exceptions" > "$scratch/answer"
expect listed-answer "STALE EXCEPTIONS" score "$vectors" "$runner" "$scratch/answer" "$unscored"
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 6 { print; next } $1 == "'"$id"'" { $3 = ($3 == "parse" ? "serialize" : "parse") } { print }' "$exceptions" > "$scratch/stage"
expect listed-stage "STALE EXCEPTIONS" score "$vectors" "$runner" "$scratch/stage" "$unscored"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF != 6 || done { print; next } $5 != "wpt-tentative" { $6 = "no quote"; done = 1 } { print }' "$exceptions" > "$scratch/noquote"
if grep -q "no quote" "$scratch/noquote"; then
	expect reason-without-quote "SUITE PROBLEMS" score "$vectors" "$runner" "$scratch/noquote" "$unscored"
fi
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF != 6 || done { print; next } { $5 = "browser-behavior"; done = 1 } { print }' "$exceptions" > "$scratch/class"
expect class-not-one-of-the-four "SUITE PROBLEMS" score "$vectors" "$runner" "$scratch/class" "$unscored"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF != 6 || done { print; next } $5 == "wpt-tentative" { $1 = "css/selectors/not-tentative.html#0"; done = 1 } { print }' "$exceptions" > "$scratch/tentative"
expect tentative-class-on-a-page-that-is-not "SUITE PROBLEMS" score "$vectors" "$runner" "$scratch/tentative" "$unscored"
expect floor "below the floor" env CSS_SELECTOR_MIN_SCORED=999999 python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored"
expect ceiling "above the ceiling" env CSS_SELECTOR_MAX_EXCEPTIONS=0 python3 -I "$suite_py" "$vectors" "$runner" "$exceptions" "$unscored"
head -n -1 "$unscored" > "$scratch/unscored-short"
expect unscored-page-missing "UNSCORED VECTORS" score "$vectors" "$runner" "$exceptions" "$scratch/unscored-short"
{ cat "$unscored"; printf 'css/control/planted.html\t3\tplanted\n'; } > "$scratch/unscored-extra"
expect unscored-page-stale "UNSCORED VECTORS" score "$vectors" "$runner" "$exceptions" "$scratch/unscored-extra"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF != 3 || done { print; next } { $2 = $2 + 1; done = 1 } { print }' "$unscored" > "$scratch/unscored-count"
expect unscored-count "UNSCORED VECTORS" score "$vectors" "$runner" "$exceptions" "$scratch/unscored-count"
printf '#!/bin/sh\nexec "%s" "$@" | sed -u "s/^INVALID$/OK\\tdiv\\tdiv/"\n' "$runner" > "$scratch/runner-accepts"
chmod +x "$scratch/runner-accepts"
expect runner-that-accepts-everything "invalid-accepted" score "$vectors" "$scratch/runner-accepts" "$exceptions" "$unscored"

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
	cc -O1 -D_GNU_SOURCE -o "$scratch/runner-$name" "$root/tools/conformance/css_selector_suite.c" "$scratch/$base.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io" -Wl,--allow-multiple-definition
	status=0
	python3 -I "$suite_py" "$vectors" "$scratch/runner-$name" "$exceptions" "$unscored" >"$log" 2>&1 || status=$?
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
done < "$root/tools/conformance/CSS_SELECTOR_CONFORMANCE_PLANTS"
echo "css selector gate: controls done"
