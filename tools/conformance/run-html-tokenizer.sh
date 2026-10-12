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
# Score the HTML tokenizer against the pinned html5lib-tests tokenizer vectors.
#
#   tools/conformance/run-html-tokenizer.sh
#
# The vectors are third_party/html/html5lib-tests/tokenizer, put there by
# tools/html/fetch.sh at the commit pinned in tools/html/HTML5LIB_TESTS_COMMIT.
# They are not fetched here: a pin is the point, and a score about whatever the
# network returned today is not a measurement of this library. If the data is
# absent this fails and says how to get it; it never skips, because a run that
# scored nothing and exited 0 would say the tokenizer conforms having read none
# of the vectors.
#
# The scorer has a floor on how many runs were actually scored (7,000), so that a truncated
# corpus cannot pass by being small, whether or not this script is run through make;
# HTML_TOKENIZER_MIN_SCORED changes it.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${HTML_TOKENIZER_SUITE:-$root/third_party/html/html5lib-tests/tokenizer}

if [ ! -f "$suite/test1.test" ]; then
	echo "$suite/test1.test is not here." >&2
	echo "Fetch the pinned vectors:  tools/html/fetch.sh" >&2
	exit 1
fi

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/html-tokenizer-runner
cc -O1 -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/html_tokenizer_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

suite_py=$root/tools/conformance/html_tokenizer_suite.py
exceptions=$root/tools/conformance/HTML_TOKENIZER_EXCEPTIONS

if [ "$1" != "--controls" ]; then
	exec python3 "$suite_py" "$suite" "$runner" "$exceptions"
fi

# --controls: the gate is made to fail on purpose, and must, each time, say why. A gate whose
# assertion no longer reaches its subject prints the same clean line as one that passes.
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

# The vectors lie: one expectation is changed.
mkdir -p "$scratch/vectors"
cp "$suite"/*.test "$scratch/vectors/"
python3 - "$scratch/vectors/test1.test" <<'PY'
import json, sys
path = sys.argv[1]
data = json.load(open(path, encoding="utf-8"))
data["tests"][0]["output"] = [["Character", "a planted difference"]]
json.dump(data, open(path, "w", encoding="utf-8"))
PY
expect vector-expectation "UNEXPLAINED FAILURES" python3 "$suite_py" "$scratch/vectors" "$runner" "$exceptions"

# The list is missing an entry, and has a stale one.
: > "$scratch/empty"
expect no-exceptions "UNEXPLAINED FAILURES" python3 "$suite_py" "$suite" "$runner" "$scratch/empty"
{ cat "$exceptions"; printf 'test1.test#0\tplanted: this run passes\n'; } > "$scratch/stale"
expect stale-exception "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/stale"
{ cat "$exceptions"; printf 'nothing.test#0\tplanted: no such vector\n'; } > "$scratch/gone"
expect exception-for-nothing "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/gone"

# The runner refuses a request it does not understand, loudly, and runs nothing for it.
good="whole data - x3c613e"
n_bad=0
for bad in "bogus data - x3c613e" "whole nowhere - x3c613e" "whole data - x3c613" "whole data - xzz" \
		"whole data - 3c613e" "chunks:x data - x3c613e" "chunks: data - x3c613e" "limits:1x data - x3c613e" \
		"whole data nothex x3c613e" "whole data - x3c613e loose" "whole data - x3c613e strict extra" "whole data -" ""; do
	n_bad=$((n_bad + 1))
	printf '%s\n' "$bad" "$good" | "$runner" > "$log"
	if [ "$(grep -c '^END E_REQUEST' "$log")" != 1 ] || [ "$(grep -c '^END DONE' "$log")" != 1 ]; then
		echo "control bad-request: '$bad' was not answered E_REQUEST (and the next request DONE)" >&2
		cat "$log" >&2
		exit 1
	fi
done
echo "control bad-requests: caught ($n_bad)"

# The floor.
expect floor "below the floor" env HTML_TOKENIZER_MIN_SCORED=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions"

# The tokenizer with one rule undone. Each of these is a defect a vector must catch.
plant() {
	name=$1 heading=$2 pattern=$3 replacement=$4
	cp "$root/src/html/html_tokenizer.c" "$scratch/html_tokenizer.c"
	sed -i "s#$pattern#$replacement#g" "$scratch/html_tokenizer.c"
	if cmp -s "$root/src/html/html_tokenizer.c" "$scratch/html_tokenizer.c"; then
		echo "control $name: the plant did not change html_tokenizer.c ('$pattern' is not there)" >&2
		exit 1
	fi
	cc -std=gnu17 -O1 -fPIC -DGTEXT_BUILD -I"$root/src/html" -I"$root/include" -I"$generated" $cflags -c "$scratch/html_tokenizer.c" -o "$scratch/html_tokenizer.o"
	cc -O1 -D_GNU_SOURCE -o "$scratch/runner-$name" "$root/tools/conformance/html_tokenizer_suite.c" "$scratch/html_tokenizer.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "$name" "$heading" python3 "$suite_py" "$suite" "$scratch/runner-$name" "$exceptions"
}
fails="UNEXPLAINED FAILURES"
plant attribute-legacy-rule "$fails" 'if (in_attribute(t) \&\& last != .;.) {' 'if (0 \&\& in_attribute(t) \&\& last != 0x3b) {'
plant c1-remap "$fails" 'code = c1_remap\[code - 0x80\];' 'code = code;'
plant appropriate-end-tag "$fails" 'return t->have_last_start \&\& ' 'return 1 || '
plant comment-end-bang "$fails" 'ar_put(t, "--!", 3);' 'ar_put(t, "--", 2);'
plant double-escape-word "$fails" '"script", 6' '"scripx", 6'
plant doctype-system-keyword "$fails" '"system", 6' '"systex", 6'
plant duplicate-attribute-kept "$fails" 't->attr_dropped = true;' 't->attr_dropped = false;'
plant name-semicolon-window "$fails" 'if (n < longest \&\& p < t->in_length \&\& t->in\[p\] == .;.) {' 'if (0) {'
plant crlf-two-lines "$fails" "\\*n = s\\[p + 1\\] == '.n' ? 2 : 1;" '*n = 1;'
plant names-not-lowered "$fails" 'ar_cp(t, (uint32_t)c + 0x20);' 'ar_cp(t, (uint32_t)c);'
plant reference-number-wraps "$fails" 'return code > CODE_CAP ? CODE_CAP : code;' 'return code;'
plant surrogate-reference "$fails" 'if (code >= 0xD800 \&\& code <= 0xDFFF) {' 'if (0) {'
plant self-closing-flag "$fails" 't->token.self_closing = true;' '(void)0;'
plant script-escape-start "$fails" 'SWITCH(ST_SCRIPT_ESC_START);' 'SWITCH(ST_SCRIPT_DATA);'
plant cdata-bracket-pair "$fails" 'text_cp(t, .\].);' '(void)0;'
plant ambiguous-ampersand-error "$fails" 'perr(t, GTEXT_HTML_ERR_UNKNOWN_NAMED_CHARACTER_REFERENCE);' '(void)0;'
plant after-doctype-name-error "$fails" 'perr(t, GTEXT_HTML_ERR_INVALID_CHARACTER_SEQUENCE_AFTER_DOCTYPE_NAME);' '(void)0;'
plant numeric-error-position "$fails" 'error_at(t, code, t->line, t->column, t->base_offset + t->pos);' 'error_at(t, code, t->prev_line, t->prev_column, t->prev_offset);'
plant markup-declaration-error-position "$fails" 'perr_next(t, GTEXT_HTML_ERR_INCORRECTLY_OPENED_COMMENT);' 'error_at(t, GTEXT_HTML_ERR_INCORRECTLY_OPENED_COMMENT, t->line, t->column - 1, t->base_offset + t->pos);'
plant utf16-columns "$fails" 't->column += cp > 0xFFFF ? 2 : 1;' 't->column++;'
# The strict UTF-8 path is scored, and the tiny-limits pass: a defect only one of them sees.
plant strict-utf8-only "THE ANSWER DEPENDS ON HOW THE INPUT WAS CUT" '\*cp = (int32_t)value;' '*cp = (!t->wtf8 \&\& value == 0xFFFD) ? 0x3F : (int32_t)value;'
plant limit-drops-text "A RUN UNDER TINY LIMITS" 'fail(t, GTEXT_HTML_TOKENIZER_E_LIMIT_TEXT);' 'n = t->max_text > t->text.length ? t->max_text - t->text.length : 0;'
# The answer must not depend on the cutting: three defects that only a cut shows.
cuts="THE ANSWER DEPENDS ON HOW THE INPUT WAS CUT"
plant offset-lost-at-a-cut "$cuts" 't->base_offset += t->pos;' '(void)0;'
plant carriage-return-not-waited-for "$cuts" 'if (p + 1 >= len) {' "if (p + 1 >= len) { *cp = '\\\\n'; *n = 1; return true; } if (0) {"
# A crash is a failure, not a missing answer.
plant crash "RUNNER CRASHES" 'static bool is_noncharacter(int32_t cp) {' 'static bool is_noncharacter(int32_t cp) { if (cp == 0x7F) { *(volatile int *)0 = 0; }'
