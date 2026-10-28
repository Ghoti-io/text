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
# Score the Markup queries against the selector tables of the pinned web-platform-tests' dom/nodes (css_selector_dom_suite.py), and with --controls show the gate fail.

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
wpt=${CSS_PROPERTY_WPT:-$root/third_party/css/wpt}
fixture=$wpt/dom/nodes/ParentNode-querySelector-All-content.html

if [ ! -f "$wpt/dom/nodes/selectors.js" ] || [ ! -f "$fixture" ]; then
	echo "the pinned selector tables of dom/nodes are not in third_party/css/wpt." >&2
	echo "Fetch them:  tools/css/fetch.sh" >&2
	exit 1
fi
command -v python3 >/dev/null 2>&1 || { echo "python3 is needed to read selectors.js (tools/css/extract-wpt-selectors.py)" >&2; exit 1; }
[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-selector-dom-runner
cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_selector_dom_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"
tables=$root/build/css-selector-dom-tables.json
python3 -I "$root/tools/css/extract-wpt-selectors.py" "$wpt" "$tables" >/dev/null

suite_py=$root/tools/conformance/css_selector_dom_suite.py
exceptions=$root/tools/conformance/CSS_SELECTOR_DOM_EXCEPTIONS

if [ "$1" = "--regenerate" ]; then
	python3 -I "$suite_py" "$tables" "$runner" "$exceptions" "$fixture" --regenerate
	exit $?
fi

score() {
	python3 -I "$suite_py" "$1" "$2" "$3" "$fixture"
}

if [ "$1" != "--controls" ]; then
	score "$tables" "$runner" "$exceptions"
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

score "$tables" "$runner" "$exceptions" >"$log" 2>&1 || {
	echo "the gate does not pass unedited" >&2
	cat "$log" >&2
	exit 1
}
python3 - "$tables" "$scratch/lie.json" <<'PY'
import json, sys
t = json.load(open(sys.argv[1], encoding="utf-8"))
for e in t["valid"]:
    if e["selector"] == "#universal>*" and e["expect"]:
        e["expect"] = e["expect"][1:]
        break
else:
    sys.exit("no #universal>* entry to lie about")
json.dump(t, open(sys.argv[2], "w", encoding="utf-8"))
PY
expect expectation "UNEXPLAINED FAILURES" score "$scratch/lie.json" "$runner" "$exceptions"
: > "$scratch/empty"
line=$(awk -F'\t' '!/^#/ && NF == 5 { print; exit }' "$exceptions")
if [ -n "$line" ]; then
	expect no-exceptions "UNEXPLAINED FAILURES" score "$tables" "$runner" "$scratch/empty"
	{ cat "$exceptions"; printf 'valid:99999@document\tdiv\tOK\twpt-wrong\tplanted: this case passes "quoted"\n'; } > "$scratch/stale"
	expect stale-exception "STALE EXCEPTIONS" score "$tables" "$runner" "$scratch/stale"
	id=$(printf '%s' "$line" | cut -f1)
	awk -F'\t' -v id="$id" 'BEGIN { OFS = "\t" } /^#/ || NF != 5 { print; next } $1 == id { $3 = "a planted answer" } { print }' "$exceptions" > "$scratch/answer"
	expect listed-answer "STALE EXCEPTIONS" score "$tables" "$runner" "$scratch/answer"
	awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF != 5 || done { print; next } { $5 = "no quote"; done = 1 } { print }' "$exceptions" > "$scratch/noquote"
	expect reason-without-quote "SUITE PROBLEMS" score "$tables" "$runner" "$scratch/noquote"
	awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF != 5 || done { print; next } { $4 = "browser-behavior"; done = 1 } { print }' "$exceptions" > "$scratch/class"
	expect class-not-one-of-the-four "SUITE PROBLEMS" score "$tables" "$runner" "$scratch/class"
else
	# with nothing listed, the stale line is the control that a listed case which passes is refused
	{ printf 'valid:99999@document\tdiv\tOK\twpt-wrong\tplanted: this case passes "quoted"\n'; } > "$scratch/stale"
	expect stale-exception "STALE EXCEPTIONS" score "$tables" "$runner" "$scratch/stale"
	printf 'valid:0@document\tdiv\tOK\tbrowser-behavior\tplanted\n' > "$scratch/class"
	expect class-not-one-of-the-four "SUITE PROBLEMS" score "$tables" "$runner" "$scratch/class"
	printf 'valid:0@document\tdiv\tOK\twpt-wrong\tno quote\n' > "$scratch/noquote"
	expect reason-without-quote "SUITE PROBLEMS" score "$tables" "$runner" "$scratch/noquote"
fi
expect floor "below the floor" env CSS_SELECTOR_DOM_MIN_SCORED=999999 python3 -I "$suite_py" "$tables" "$runner" "$exceptions" "$fixture"
printf '#!/bin/sh\nexec "%s" "$@" | sed -u "s/^SYNTAX$/OK\\t/"\n' "$runner" > "$scratch/runner-accepts"
chmod +x "$scratch/runner-accepts"
expect runner-that-accepts-everything "UNEXPLAINED FAILURES" score "$tables" "$scratch/runner-accepts" "$exceptions"

plant() {
	name=$1 file=$2 old=$3 new=$4
	base=$(basename "$file" .c)
	python3 - "$root/src/css/$file" "$scratch/$base.c" "$old" "$new" <<'PY'
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
		-c "$scratch/$base.c" -o "$scratch/$base.o"
	cc -O1 -D_GNU_SOURCE -o "$scratch/runner-$name" "$root/tools/conformance/css_selector_dom_suite.c" "$scratch/$base.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io" -Wl,--allow-multiple-definition
	status=0
	python3 -I "$suite_py" "$tables" "$scratch/runner-$name" "$exceptions" "$fixture" >"$log" 2>&1 || status=$?
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
done < "$root/tools/conformance/CSS_SELECTOR_DOM_PLANTS"
echo "css selector queries gate: controls done"
