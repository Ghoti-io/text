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
# Score the HTML tree builder against the pinned tree-construction vectors.
#
#   tools/conformance/run-html-tree.sh [--controls]
#
# The vectors are third_party/html/wpt/html/syntax/parsing/resources/*.dat, put there by
# tools/html/fetch.sh at the commit pinned in tools/html/WPT_COMMIT (html5lib-tests keeps no
# tree-construction vectors any more). They are not fetched here: a pin is the point. If the
# data is absent this fails and says how to get it; it never skips.
#
# The scorer has two floors, so that a truncated corpus cannot pass by being small whether or
# not this script is run through make: HTML_TREE_MIN_VECTORS (vectors loaded) and
# HTML_TREE_MIN_PASS (vectors passing).
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${HTML_TREE_SUITE:-$root/third_party/html/wpt/html/syntax/parsing/resources}

if [ ! -f "$suite/tests1.dat" ]; then
	echo "$suite/tests1.dat is not here." >&2
	echo "Fetch the pinned vectors:  tools/html/fetch.sh" >&2
	exit 1
fi

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/html-tree-runner
cc -O1 -D_GNU_SOURCE $CFLAGS -o "$runner" "$root/tools/conformance/html_tree_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

suite_py=$root/tools/conformance/html_tree_suite.py
exceptions=$root/tools/conformance/HTML_TREE_EXCEPTIONS
extra=$root/tools/conformance/html_tree_extra.dat
export HTML_TREE_MIN_VECTORS=${HTML_TREE_MIN_VECTORS:-1900}
export HTML_TREE_MIN_PASS=${HTML_TREE_MIN_PASS:-1300}
export HTML_TREE_MIN_EXACT_ERRORS=${HTML_TREE_MIN_EXACT_ERRORS:-1129}

# --sanitize: the same gate with the library and the runner built with AddressSanitizer and
# UndefinedBehaviorSanitizer, the library from its sources (the archive is not instrumented). The runner
# frees each chunk the moment the builder has taken it, so a builder that kept a pointer into one is a
# use-after-free here, and every leak is reported at exit.
if [ "$1" = "--sanitize" ]; then
	san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"
	objdir=$(mktemp -d)
	trap 'rm -rf "$objdir"' EXIT
	i=0
	for src in $(find "$root/src" -name '*.c'); do
		i=$((i + 1))
		echo "cc -std=gnu17 $san -DGTEXT_BUILD -w -I\"$root/include\" -I\"$generated\" $cflags -c \"$src\" -o \"$objdir/$i.o\""
	done | xargs -P "$(nproc)" -I{} sh -c '{}'
	cc -D_GNU_SOURCE $san -o "$objdir/runner" "$root/tools/conformance/html_tree_suite.c" "$objdir"/*.o \
		-I"$root/include" -I"$generated" $cflags $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	echo "html tree gate under ASan + UBSan"
	LD_PRELOAD= ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		HTML_TREE_RUNNER_TIMEOUT=1800 \
		python3 "$root/tools/conformance/html_tree_suite.py" "$suite" "$objdir/runner" "$root/tools/conformance/HTML_TREE_EXCEPTIONS" \
		"$root/tools/conformance/html_tree_extra.dat"
	exit $?
fi

if [ "$1" != "--controls" ]; then
	exec python3 "$suite_py" "$suite" "$runner" "$exceptions" "$extra"
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

# The vectors lie: one expected tree is changed.
mkdir -p "$scratch/vectors"
cp "$suite"/*.dat "$scratch/vectors/"
python3 - "$scratch/vectors/tests1.dat" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
at = text.index("#document\n") + len("#document\n")
text = text[:at] + "| <planted>\n" + text[at:]
open(path, "w", encoding="utf-8").write(text)
PY
expect vector-expectation "UNEXPLAINED FAILURES" python3 "$suite_py" "$scratch/vectors" "$runner" "$exceptions" "$extra"

# The list is missing everything, has a stale entry, an entry for nothing, an entry in the
# wrong stage, a digest that does not match.
: > "$scratch/empty"
expect no-exceptions "UNEXPLAINED FAILURES" python3 "$suite_py" "$suite" "$runner" "$scratch/empty" "$extra"
{ cat "$exceptions"; printf 'tests1.dat#1\t3d\ttable\tplanted: this vector passes\n'; } > "$scratch/stale"
expect stale-exception "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/stale" "$extra"
{ cat "$exceptions"; printf 'nothing.dat#1\t3d\ttable\tplanted: no such vector\n'; } > "$scratch/gone"
expect exception-for-nothing "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/gone" "$extra"
awk -F'\t' 'BEGIN{OFS="\t"} !done && $2=="3d" && $3=="table" {$2="3e"; done=1} {print}' "$exceptions" > "$scratch/wrongstage"
expect exception-in-the-wrong-stage "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/wrongstage" "$extra"
awk -F'\t' 'BEGIN{OFS="\t"} $2=="none" && !done {$5="0000000000"; done=1} {print}' "$exceptions" > "$scratch/wrongdigest"
expect exception-with-another-failure "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/wrongdigest" "$extra"

# The floors.
expect floor-passing "below the floor" env HTML_TREE_MIN_PASS=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions" "$extra"
expect floor-loaded "below the floor" env HTML_TREE_MIN_VECTORS=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions" "$extra"

# The runner refuses a request it does not understand, loudly, and runs nothing for it.
good="whole 0 x3c613e"
n_bad=0
for bad in "bogus 0 x3c613e" "whole 2 x3c613e" "whole 0 x3c613" "whole 0 xzz" "whole 0 3c613e" \
		"chunks:x 0 x3c613e" "chunks: 0 x3c613e" "split:99 0 x3c613e" "split:x 0 x3c613e" "whole 0 x3c613e loose" "whole 0" ""; do
	n_bad=$((n_bad + 1))
	printf '%s\n' "$bad" "$good" | "$runner" > "$log"
	if [ "$(grep -c '^END E_REQUEST' "$log")" != 1 ] || [ "$(grep -c '^END DONE' "$log")" != 1 ]; then
		echo "control bad-request: '$bad' was not answered E_REQUEST (and the next request DONE)" >&2
		cat "$log" >&2
		exit 1
	fi
done
echo "control bad-requests: caught ($n_bad)"

# The exact-count floor, and a vector that has fewer parse errors than the list.
expect floor-exact-errors "below the floor" env HTML_TREE_MIN_EXACT_ERRORS=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions" "$extra"

# The tree builder with one rule undone. Each of these is a defect a vector must catch. The
# plant copies the whole of src/html (the tag table is in a header both files read), edits one
# file, and builds the runner against it in front of the archive.
plant() {
	name=$1 heading=$2 file=$3 pattern=$4 replacement=$5
	rm -rf "$scratch/plant"
	mkdir -p "$scratch/plant/src"
	cp -r "$root/src/html" "$root/src/markup" "$scratch/plant/src/"
	sed -i "s#$pattern#$replacement#g" "$scratch/plant/src/html/$file"
	if cmp -s "$root/src/html/$file" "$scratch/plant/src/html/$file"; then
		echo "control $name: the plant did not change $file ('$pattern' is not there)" >&2
		exit 1
	fi
	for f in html_tree.c html_tree_modes.c; do
		cc -std=gnu17 -O1 -fPIC -DGTEXT_BUILD -I"$scratch/plant/src/html" -I"$root/include" -I"$generated" $cflags \
			-c "$scratch/plant/src/html/$f" -o "$scratch/plant/$f.o"
	done
	cc -O1 -D_GNU_SOURCE -o "$scratch/runner-$name" "$root/tools/conformance/html_tree_suite.c" \
		"$scratch/plant/html_tree.c.o" "$scratch/plant/html_tree_modes.c.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "$name" "$heading" python3 "$suite_py" "$suite" "$scratch/runner-$name" "$exceptions" "$extra"
}
fails="UNEXPLAINED FAILURES"
plant noahs-ark-keeps-four "$fails" html_tree.c 'if (count >= 3) {' 'if (count >= 4) {'
plant adoption-outer-loop-once "$fails" html_tree.c 'outer < 8' 'outer < 1'
plant adoption-no-bookmark-move "$fails" html_tree.c 'if (last_node == fb_node) {' 'if (0) {'
plant adoption-inner-loop-limit "$fails" html_tree.c 'inner > 3' 'inner > 300'
plant adoption-leaves-formatting-element-in-list "$fails" html_tree.c 'ht_fmt_remove(tb, fe);' '(void)fe;'
plant reconstruct-skipped "$fails" html_tree_modes.c 'if (!ht_fmt_reconstruct(tb) || !ht_insert_text(tb, p, len)) {' 'if (!ht_insert_text(tb, p, len)) {'
plant li-does-not-close-li "$fails" html_tree_modes.c 'if (j != NONE_IDX \&\& j >= HT_STACK_TOP(tb).fl_li) {' 'if (0) {'
plant dd-dt-do-not-close "$fails" html_tree_modes.c 'if (found != NONE_IDX) {' 'if (0) {'
plant frameset-ok-never-cleared "$fails" html_tree_modes.c 'if (ws_prefix(p, len) != len) {' 'if (0) {'
plant pre-newline-kept "$fails" html_tree_modes.c 'tb->skip_lf = true;' 'tb->skip_lf = false;'
plant html-attributes-not-merged "$fails" html_tree.c 'present = true;' 'present = false;'
plant p-closed-outside-button-scope "$fails" html_tree.c 'if (ht_stack_in_scope(tb, T_P, SCOPE_BUTTON)) {' 'if (ht_stack_in_scope(tb, T_P, SCOPE_GEN)) {'
plant implied-end-tags-stop "$fails" html_tree.c 'while (tb->n > 0 \&\& (HT_STACK_TOP(tb).flags \& F_IMPLIED)' 'while (0 \&\& (HT_STACK_TOP(tb).flags \& F_IMPLIED)'
plant form-pointer-ignored "$fails" html_tree_modes.c 'if (tb->form_el) {' 'if (0) {'
plant noscript-flag-ignored "$fails" html_tree_modes.c 'if (!tb->opt.scripting) {' 'if (1) {'
plant text-not-merged "$fails" html_tree.c 'if (last \&\& last->kind == GTEXT_MARKUP_KIND_TEXT) {' 'if (0) {'
plant marquee-is-not-a-scope-boundary "$fails" html_tree_private.h 'X(T_MARQUEE, "marquee", F_SPECIAL | F_SC_GEN)' 'X(T_MARQUEE, "marquee", F_SPECIAL)'
plant button-is-not-a-button-scope-boundary "$fails" html_tree_private.h 'X(T_BUTTON, "button", F_SPECIAL | F_SC_BUTTON)' 'X(T_BUTTON, "button", F_SPECIAL)'
plant ol-is-not-a-list-scope-boundary "$fails" html_tree_private.h 'X(T_OL, "ol", F_SPECIAL | F_SC_LIST)' 'X(T_OL, "ol", F_SPECIAL)'
plant ul-is-not-a-list-scope-boundary "$fails" html_tree_private.h 'X(T_UL, "ul", F_SPECIAL | F_SC_LIST)' 'X(T_UL, "ul", F_SPECIAL)'
plant select-is-not-a-scope-boundary "$fails" html_tree_private.h 'X(T_SELECT, "select", F_SPECIAL | F_SC_GEN)' 'X(T_SELECT, "select", F_SPECIAL)'
plant any-other-end-tag-passes-special "$fails" html_tree.c 'j >= HT_STACK_TOP(tb).fl_special' 'j >= 0'
plant image-is-not-img "$fails" html_tree_modes.c 'img.id = T_IMG;' 'img.id = T_IMAGE;'
plant head-element-not-removed-after-head "$fails" html_tree_modes.c 'const int r = h_in_head(tb, tk);' 'const int r = h_in_head(tb, tk); if (r != R_FAIL) { return r; }'
plant a-in-a-not-closed "$fails" html_tree_modes.c 'HT_CK(ht_adoption_agency(tb, T_A));' '(void)0;'
plant nobr-in-nobr-not-closed "$fails" html_tree_modes.c 'HT_CK(ht_adoption_agency(tb, T_NOBR));' '(void)0;'
# A crash is a failure, not a missing answer.
plant crash "$fails" html_tree_modes.c 'static bool is_ws(char c) {' 'static bool is_ws(char c) { if (c == 0x61) { *(volatile int *)0 = 0; }'
