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
# Score the CSS Syntax parser against the pinned css-parsing-tests vectors and
# the tables of web-platform-tests' css/css-syntax.
#
#   tools/conformance/run-css-syntax.sh [--controls]
#
# The vectors are third_party/css/, put there by tools/css/fetch.sh at the
# commits pinned in tools/css/CSS_PARSING_TESTS_COMMIT and tools/css/WPT_COMMIT.
# They are not fetched here: a pin is the point. If the data is absent this
# fails and says how to get it; it never skips. The WPT tables are turned into
# vectors by tools/css/extract-wpt.py on every run, from the pinned checkout.
#
# The scorer has a floor on how many cases were scored (CSS_SYNTAX_MIN_SCORED,
# which the Makefile sets in CSS_SYNTAX_FLOOR, the one place the number is
# written; the scorer's own default is lower), so that a truncated corpus
# cannot pass by being small.
#
# --controls makes the gate fail on purpose, each way it should, and requires
# it to say why: vectors that lie, exceptions that are missing, stale, about
# nothing or without a quoted sentence, a floor that is not met, requests the
# runner must refuse, a runner that crashes, and the library with one rule of
# the standard undone (one plant at a time, in a copy of the file, never in
# the tree).
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${CSS_SYNTAX_SUITE:-$root/third_party/css/css-parsing-tests}
wpt=${CSS_SYNTAX_WPT:-$root/third_party/css/wpt}

if [ ! -f "$suite/component_value_list.json" ] || [ ! -f "$wpt/css/css-syntax/anb-parsing.html" ]; then
	echo "the pinned CSS vectors are not in third_party/css/." >&2
	echo "Fetch them:  tools/css/fetch.sh" >&2
	exit 1
fi

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-syntax-runner
cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_syntax_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

vectors=$root/build/css-syntax-wpt-vectors.json
python3 "$root/tools/css/extract-wpt.py" "$wpt" "$vectors"

suite_py=$root/tools/conformance/css_syntax_suite.py
exceptions=$root/tools/conformance/CSS_SYNTAX_EXCEPTIONS

if [ "$1" != "--controls" ]; then
	exec python3 "$suite_py" "$suite" "$vectors" "$runner" "$exceptions"
fi

scratch=$(mktemp -d)
log=$(mktemp)
trap 'rm -rf "$scratch" "$log"' EXIT

# expect <what> <heading> <command...>: the command must fail, and say the heading.
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

# The unedited gate passes: the controls below are measured against it.
python3 "$suite_py" "$suite" "$vectors" "$runner" "$exceptions" >"$log" 2>&1 || {
	echo "the gate does not pass unedited" >&2
	cat "$log" >&2
	exit 1
}

# The vectors lie: one expectation of css-parsing-tests, and one of the WPT tables.
mkdir -p "$scratch/suite"
cp "$suite"/*.json "$scratch/suite/"
python3 - "$scratch/suite/one_component_value.json" <<'PY'
import json, sys
path = sys.argv[1]
data = json.load(open(path, encoding="utf-8"))
data[11] = ["ident", "a planted difference"]
json.dump(data, open(path, "w", encoding="utf-8"))
PY
git -C "$suite" rev-parse HEAD >/dev/null 2>&1 && git init --quiet "$scratch/suite" 2>/dev/null || true
expect vector-expectation "UNEXPLAINED FAILURES" python3 "$suite_py" "$scratch/suite" "$vectors" "$runner" "$exceptions"
python3 - "$vectors" "$scratch/wpt.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
for v in data["vectors"]:
    if v["check"]["kind"] == "anb" and v["check"]["expect"] == "2n+1":
        v["check"]["expect"] = "2n-1"
        break
json.dump(data, open(sys.argv[2], "w", encoding="utf-8"))
PY
expect wpt-expectation "UNEXPLAINED FAILURES" python3 "$suite_py" "$suite" "$scratch/wpt.json" "$runner" "$exceptions"

# The bytes' expected encoding is wrong in one case of stylesheet_bytes.json (and nothing else is).
cp "$suite"/*.json "$scratch/suite/"
python3 - "$scratch/suite/stylesheet_bytes.json" <<'PY'
import json, sys
path = sys.argv[1]
data = json.load(open(path, encoding="utf-8"))
data[1][1] = "ibm866"
json.dump(data, open(path, "w", encoding="utf-8"))
PY
expect bytes-encoding "UNEXPLAINED FAILURES" python3 "$suite_py" "$scratch/suite" "$vectors" "$runner" "$exceptions"
cp "$suite"/*.json "$scratch/suite/"

# A file of the pin that is neither scored nor out of scope.
cp "$suite"/*.json "$scratch/suite/" && printf '["a", []]\n' > "$scratch/suite/new_entry_point.json"
expect unknown-file "SUITE PROBLEMS" python3 "$suite_py" "$scratch/suite" "$vectors" "$runner" "$exceptions"

# The list is missing, has a stale entry, an entry for nothing, and a reason that quotes nothing.
: > "$scratch/empty"
expect no-exceptions "UNEXPLAINED FAILURES" python3 "$suite_py" "$suite" "$vectors" "$runner" "$scratch/empty"
{ cat "$exceptions"; printf 'one_rule.json#0\t["error","empty"]\tplanted: this case passes "quoted"\n'; } > "$scratch/stale"
expect stale-exception "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$vectors" "$runner" "$scratch/stale"
{ cat "$exceptions"; printf 'nothing.json#0\tnull\tplanted: no such case "quoted"\n'; } > "$scratch/gone"
expect exception-for-nothing "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$vectors" "$runner" "$scratch/gone"
awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF < 3 { print; next } { gsub(/"/, "", $3); print }' "$exceptions" > "$scratch/unquoted"
expect reason-without-quote "SUITE PROBLEMS" python3 "$suite_py" "$suite" "$vectors" "$runner" "$scratch/unquoted"
# A listed case whose answer is not the draft's: the listed answer is changed.
awk -F'\t' 'BEGIN { OFS = "\t"; done = 0 } /^#/ || NF < 3 || done { print; next } { $2 = "[]"; done = 1; print }' "$exceptions" > "$scratch/answer"
expect listed-answer "UNEXPLAINED FAILURES" python3 "$suite_py" "$suite" "$vectors" "$runner" "$scratch/answer"

# The floor.
expect floor "below the floor" env CSS_SYNTAX_MIN_SCORED=99999 python3 "$suite_py" "$suite" "$vectors" "$runner" "$exceptions"

# The runner refuses a request it does not understand, loudly, and runs nothing for it.
good="component_values - 61"
n_bad=0
for bad in "bogus - 61" "component_values" "component_values -" "component_values - 6" "component_values - zz" \
		"component_values nope 61" "component_values - 61 extra" ""; do
	n_bad=$((n_bad + 1))
	printf '%s\n' "$bad" "$good" | "$runner" > "$log"
	if [ "$(grep -c '^END E_REQUEST' "$log")" != 1 ] || [ "$(grep -c '^END DONE' "$log")" != 1 ]; then
		echo "control bad-request: '$bad' was not answered E_REQUEST (and the next request DONE)" >&2
		cat "$log" >&2
		exit 1
	fi
done
echo "control bad-requests: caught ($n_bad)"

# The library with one rule undone. Each is a defect a vector must catch.
plant() {
	name=$1 heading=$2 file=$3 pattern=$4 replacement=$5
	base=$(basename "$file" .c)
	cp "$root/src/css/$file" "$scratch/$file"
	sed -i "s#$pattern#$replacement#g" "$scratch/$file"
	if cmp -s "$root/src/css/$file" "$scratch/$file"; then
		echo "control $name: the plant did not change $file ('$pattern' is not there)" >&2
		exit 1
	fi
	cc -std=c17 -O1 -fPIC -DGTEXT_BUILD -I"$root/src/css" -I"$root/src" -I"$root/include" -I"$generated" $cflags \
		-c "$scratch/$file" -o "$scratch/$base.o"
	cc -O1 -D_GNU_SOURCE -o "$scratch/runner-$name" "$root/tools/conformance/css_syntax_suite.c" "$scratch/$base.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "$name" "$heading" python3 "$suite_py" "$suite" "$vectors" "$scratch/runner-$name" "$exceptions"
}
fails="UNEXPLAINED FAILURES"
trips="ROUND TRIP FAILURES"
plant hash-type-flag "$fails" css_tokenizer.c 'tok->flags |= GTEXT_CSS_FLAG_HASH_ID;' '(void)0;'
plant cdc-swallowed "$fails" css_tokenizer.c "else if (a == '-' \&\& b == '>') {" 'else if (0) {'
plant escape-seven-digits "$fails" css_tokenizer.c 'for (int i = 0; i < 5 \&\& is_hex' 'for (int i = 0; i < 6 \&\& is_hex'
# No vector escapes a surrogate (\D800): that rule is held by tests/test-css-tokenizer.cpp, not here.
plant url-whitespace "$fails" css_tokenizer.c 'while (is_ws(peek(t, 0)) \&\& is_ws(peek(t, 1))) {' 'while (0) {'
plant number-exponent "$fails" css_tokenizer.c "if (c == 'e' || c == 'E') {" 'if (0) {'
plant non-ascii-range "$fails" css_core.c '(cp >= 0x37F \&\& cp <= 0x1FFF)' '(cp >= 0x380 \&\& cp <= 0x1FFF)'
plant important-case "$fails" css_parser.c '\&\& css_ascii_ieq(\&imp->value, "important")) {' '\&\& imp->value.length == 9 \&\& memcmp(imp->value.data, "important", 9) == 0) {'
plant trailing-whitespace "$fails" css_parser.c 'while (n > 0 \&\& vstack(p, base + n - 1)->kind == GTEXT_CSS_VALUE_TOKEN' 'while (0 \&\& vstack(p, base + n - 1)->kind == GTEXT_CSS_VALUE_TOKEN'
plant custom-property-rule "$fails" css_parser.c 'if (looks_like_custom_property(p, base)) {' 'if (0) {'
plant cdo-cdc-kept "$fails" css_parser.c '|| t->type == GTEXT_CSS_TOKEN_CDO' '|| 0'
plant remnants-skipped "$fails" css_parser.c 'remnants(p, s, false);' '(void)0;'
plant anb-dagger "$fails" css_anb.c 'if (i == count || !is_token(\&values\[i\], GTEXT_CSS_TOKEN_IDENT)) {' 'i = skip_ws(values, count, i); if (i == count || !is_token(\&values[i], GTEXT_CSS_TOKEN_IDENT)) {'
plant no-comment-between-tokens "$trips" css_serialize.c 'puts_(w, "/\*\*/");' '(void)0;'
plant identifier-digit "$trips" css_serialize.c 'else if (mode != ID_UNRESTRICTED \&\& index == 0 \&\& c >= .0. \&\& c <= .9.) {' 'else if (0) {'
plant bad-string-newline "$trips" css_serialize.c 'put(w, "\\"\\n", 2);' 'put(w, "\\"", 1);'
# CSS Syntax 3.2, the bytes: a byte order mark over every label, the protocol's label over the declaration, the
# declaration's UTF-16 read as UTF-8, the environment encoding, and a stream that reports another encoding.
plant bom-over-label "$fails" css_decode.c 'if (length >= 3 \&\& b\[0\] == 0xEF \&\& b\[1\] == 0xBB \&\& b\[2\] == 0xBF) {' 'if (0) {'
plant protocol-label "$fails" css_decode.c 'if (options \&\& options->protocol_label) {' 'if (0) {'
plant charset-utf16 "$fails" css_decode.c 'if (e == GTEXT_ENCODING_UTF_16BE || e == GTEXT_ENCODING_UTF_16LE) {' 'if (0) {'
plant charset-rule "$fails" css_decode.c 'if (charset_label(b, window, \&lb, \&le)) {' 'if (0) {'
plant environment-encoding "$fails" css_decode.c 'if (options \&\& options->environment_encoding != GTEXT_ENCODING_NONE' 'if (0 \&\& options->environment_encoding != GTEXT_ENCODING_NONE'
plant stream-encoding "$fails" css_stream.c 'info.encoding.encoding = s->dec.encoding;' 'info.encoding.encoding = GTEXT_ENCODING_UTF_8;'
# A crash is a failure, not a missing answer.
plant crash "RUNNER CRASHES" css_tokenizer.c 'static bool is_non_printable(int32_t c) {' 'static bool is_non_printable(int32_t c) { if (c == 0x7F) { *(volatile int *)0 = 0; }'
