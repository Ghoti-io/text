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
# Score the CSS value layer against the value tests of web-platform-tests' css/css-values and css/css-color/parsing.
#
#   tools/conformance/run-css-values.sh [--controls]
#
# The pages are third_party/css/wpt, put there by tools/css/fetch.sh at the commit pinned in tools/css/WPT_COMMIT; the vectors are
# made from them by tools/css/extract-wpt-values.js (node) on every run. With the data absent this fails and says how to get it;
# it never skips. --controls makes the gate fail on purpose, each way it should, and requires it to say why: a vector that lies,
# exceptions that are missing, stale, with a different answer or without a quoted sentence, a floor that is not met, and the
# library with one rule undone.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
wpt=${CSS_VALUES_WPT:-$root/third_party/css/wpt}

if [ ! -f "$wpt/css/css-values/calc-serialization.html" ] || [ ! -f "$wpt/css/css-color/parsing/color-valid-rgb.html" ]; then
	echo "the pinned CSS value pages are not in third_party/css/wpt." >&2
	echo "Fetch them:  tools/css/fetch.sh" >&2
	exit 1
fi
command -v node >/dev/null 2>&1 || { echo "node is needed to read the pages' scripts (tools/css/extract-wpt-values.js)" >&2; exit 1; }

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-values-runner
cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_values_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

vectors=$root/build/css-values-wpt-vectors.json
node "$root/tools/css/extract-wpt-values.js" "$wpt" "$vectors" >/dev/null

suite_py=$root/tools/conformance/css_values_suite.py
exceptions=$root/tools/conformance/CSS_VALUES_EXCEPTIONS

if [ "$1" != "--controls" ]; then
	exec python3 "$suite_py" "$vectors" "$runner" "$exceptions"
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

python3 "$suite_py" "$vectors" "$runner" "$exceptions" >"$log" 2>&1 || {
	echo "the gate does not pass unedited" >&2
	cat "$log" >&2
	exit 1
}

# A vector that lies: the expected serialization of one that passes is changed.
python3 - "$vectors" "$scratch/lie.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
for v in data:
    if v["kind"] == "valid" and v["prop"] == "width" and v["expected"].startswith("calc("):
        v["expected"] = v["expected"][:-1] + " + 1px)"
        break
else:
    sys.exit("no calc() vector to lie about")
json.dump(data, open(sys.argv[2], "w", encoding="utf-8"))
PY
expect vector-expectation "UNEXPLAINED FAILURES" python3 "$suite_py" "$scratch/lie.json" "$runner" "$exceptions"

# The list is missing, has a stale entry, an entry for nothing, another answer, and a reason that quotes nothing.
: > "$scratch/empty"
expect no-exceptions "UNEXPLAINED FAILURES" python3 "$suite_py" "$vectors" "$runner" "$scratch/empty"
first=$(awk -F'\t' '!/^#/ && NF >= 3 { print $1; exit }' "$exceptions")
{ cat "$exceptions"; printf 'css/css-values/calc-serialization.html#0\tcalc(1px)\tplanted: this case passes "quoted"\n'; } > "$scratch/stale"
expect stale-exception "STALE EXCEPTIONS" python3 "$suite_py" "$vectors" "$runner" "$scratch/stale"
{ cat "$exceptions"; printf 'nothing.html#0\tINVALID\tplanted: no such case "quoted"\n'; } > "$scratch/gone"
expect exception-for-nothing "STALE EXCEPTIONS" python3 "$suite_py" "$vectors" "$runner" "$scratch/gone"
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF < 3 { print; next } { gsub(/"/, "", $3); print }' "$exceptions" > "$scratch/unquoted"
expect reason-without-quote "SUITE PROBLEMS" python3 "$suite_py" "$vectors" "$runner" "$scratch/unquoted"
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF < 3 || done { print; next } { $2 = "a planted answer"; done = 1; print }' "$exceptions" > "$scratch/answer"
expect listed-answer "STALE EXCEPTIONS" python3 "$suite_py" "$vectors" "$runner" "$scratch/answer"
expect floor "below the floor" env CSS_VALUES_MIN_SCORED=99999 python3 "$suite_py" "$vectors" "$runner" "$exceptions"

# The library with one rule undone. Each is a defect a vector must catch.
plant() {
	name=$1 file=$2 pattern=$3 replacement=$4
	base=$(basename "$file" .c)
	cp "$root/src/css/$file" "$scratch/$file"
	sed -i "s#$pattern#$replacement#g" "$scratch/$file"
	if cmp -s "$root/src/css/$file" "$scratch/$file"; then
		echo "control $name: the plant did not change $file ('$pattern' is not there)" >&2
		exit 1
	fi
	cc -std=c17 -O1 -fPIC -DGTEXT_BUILD -I"$root/src/css" -I"$root/src" -I"$root/include" -I"$generated" $cflags \
		-c "$scratch/$file" -o "$scratch/$base.o"
	cc -O1 -D_GNU_SOURCE -o "$scratch/runner-$name" "$root/tools/conformance/css_values_suite.c" "$scratch/$base.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "$name" "UNEXPLAINED FAILURES" python3 "$suite_py" "$vectors" "$scratch/runner-$name" "$exceptions"
}
plant sum-terms-are-not-sorted css_math_serialize.c 'if (n->kids\[i\]->op == GTEXT_CSS_MATH_NUMBER) {' 'if (0) {'
plant hsl-is-not-rounded css_color_serialize.c 'return gmath_floor(v + 0.5 + 1e-9);' 'return v;'
plant alpha-one-is-written css_color_serialize.c 'return v == 1.0 || (puts_(b, " / ") \&\& put_num(b, v));' 'return puts_(b, " / ") \&\& put_num(b, v);'
plant rgb-is-not-clamped css_color.c 'c->value = clamp_to(c->value, ch->lo, ch->hi);' '(void)0;'
plant type-sum-is-unchecked css_math_simplify.c 'if (exps_equal(\&t1, \&t2)) {' 'if (1) {'
plant pow-is-zero css_math_simplify.c 'return gmath_pow(A, B);' 'return 0.0;'
echo "css values gate: controls done"
