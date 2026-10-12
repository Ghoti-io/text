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

# The same vectors through the public API (include/ghoti.io/text/html.h): the layer over the builder is held to
# the same standard, with the same scorer and the same exceptions.
api_runner=$root/build/html-api-runner
cc -O1 -D_GNU_SOURCE $CFLAGS -o "$api_runner" "$root/tools/conformance/html_api_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

suite_py=$root/tools/conformance/html_tree_suite.py
exceptions=$root/tools/conformance/HTML_TREE_EXCEPTIONS
extra=$root/tools/conformance/html_tree_extra.dat
export HTML_TREE_MIN_VECTORS=${HTML_TREE_MIN_VECTORS:-2050}
export HTML_TREE_MIN_PASS=${HTML_TREE_MIN_PASS:-2085}
export HTML_TREE_MIN_EXACT_ERRORS=${HTML_TREE_MIN_EXACT_ERRORS:-1792}

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
	cc -D_GNU_SOURCE $san -o "$objdir/api-runner" "$root/tools/conformance/html_api_suite.c" "$objdir"/*.o \
		-I"$root/include" -I"$generated" $cflags $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	for which in runner api-runner; do
		echo "html tree gate under ASan + UBSan ($which)"
		LD_PRELOAD= ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
			HTML_TREE_RUNNER_TIMEOUT=1800 \
			python3 "$root/tools/conformance/html_tree_suite.py" "$suite" "$objdir/$which" "$root/tools/conformance/HTML_TREE_EXCEPTIONS" \
			"$root/tools/conformance/html_tree_extra.dat" || exit $?
	done
	exit 0
fi

# --control NAME runs one control of the list below and no other (a plant is not even built for the others).
only=
if [ "$1" = "--control" ]; then
	only=$2
	[ -n "$only" ] || { echo "usage: run-html-tree.sh --control NAME" >&2; exit 2; }
elif [ "$1" != "--controls" ]; then
	echo "through the tree builder:"
	python3 "$suite_py" "$suite" "$runner" "$exceptions" "$extra" || exit $?
	echo "through the public API:"
	exec python3 "$suite_py" "$suite" "$api_runner" "$exceptions" "$extra"
fi

# --controls: the gate is made to fail on purpose, and must, each time, say why. A gate whose
# assertion no longer reaches its subject prints the same clean line as one that passes.
scratch=$(mktemp -d)
ran=0
log=$(mktemp)
trap 'rm -rf "$scratch" "$log"' EXIT

# expect <what> <heading> <command...>: the command must fail, and say the heading.
expect() {
	what=$1 want=$2
	shift 2
	if [ -n "$only" ] && [ "$only" != "$what" ]; then
		return 0
	fi
	ran=$((ran + 1))
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
# A listed construct that parses now (a foreign content or fragment vector that stage 3e removed from the list):
# the entry is stale.
{ cat "$exceptions"; printf 'svg.dat#1\t3e\tforeign\tplanted: foreign content parses\n'; } > "$scratch/wrongstage"
expect exception-for-a-construct-that-parses "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/wrongstage" "$extra"
awk -F'\t' 'BEGIN{OFS="\t"} $2=="none" && !done {$5="0000000000"; done=1} {print}' "$exceptions" > "$scratch/wrongdigest"
expect exception-with-another-failure "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/wrongdigest" "$extra"

# The floors.
expect floor-passing "below the floor" env HTML_TREE_MIN_PASS=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions" "$extra"
expect floor-loaded "below the floor" env HTML_TREE_MIN_VECTORS=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions" "$extra"

# The runner refuses a request it does not understand, loudly, and runs nothing for it.
if [ -z "$only" ] || [ "$only" = bad-requests ]; then
	good="whole 0 x3c613e"
	n_bad=0
	for bad in "bogus 0 x3c613e" "whole 2 x3c613e" "whole 0 x3c613" "whole 0 xzz" "whole 0 3c613e" \
			"chunks:x 0 x3c613e" "chunks: 0 x3c613e" "split:99 0 x3c613e" "split:x 0 x3c613e" "whole 0 x3c613e loose" "whole 0" "" \
			"whole 0 x3c613e ctx:xzz" "whole 0 x3c613e ctx:" "whole 0 x3c613e ctx:x" "whole 0 x3c613e form" "whole 0 x3c613e quirks" \
			"whole 0 x3c613e ctx:x6469 ctx:x6469" "whole 0 x3c613e ctx:x6469 loose" "whole 0 x3c613e ctx:x6469 quirks form quirks2" \
			"whole 0 x3c613e ctx:x73766720" "whole 0 x3c613e ctx:x6d61746820"; do
		n_bad=$((n_bad + 1))
		for which in "$runner" "$api_runner"; do
			printf '%s\n' "$bad" "$good" | "$which" > "$log"
			if [ "$(grep -c '^END E_REQUEST' "$log")" != 1 ] || [ "$(grep -c '^END DONE' "$log")" != 1 ]; then
				echo "control bad-request: '$bad' was not answered E_REQUEST (and the next request DONE) by $which" >&2
				cat "$log" >&2
				exit 1
			fi
		done
	done
	# A context name with a NUL in it cannot be given to the public API (a C string), so its runner refuses it
	# rather than parse a shorter name; the builder's runner takes lengths and runs it.
	printf '%s\n' "whole 0 x3c613e ctx:x640064" "$good" | "$api_runner" > "$log"
	if [ "$(grep -c '^END E_REQUEST' "$log")" != 1 ] || [ "$(grep -c '^END DONE' "$log")" != 1 ]; then
		echo "control bad-request: a context name with a NUL was not refused by the public API's runner" >&2
		cat "$log" >&2
		exit 1
	fi
	n_bad=$((n_bad + 1))
	echo "control bad-requests: caught ($n_bad)"
	ran=$((ran + 1))
fi

# The exact-count floor, and a vector that has fewer parse errors than the list.
expect floor-exact-errors "below the floor" env HTML_TREE_MIN_EXACT_ERRORS=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions" "$extra"

# The tree builder with one rule undone. Each of these is a defect a vector must catch. The
# plant copies the whole of src/html (the tag table is in a header both files read), edits one
# file, and builds the runner against it in front of the archive.
# plant_with <sed|lit> <name> <heading> <file> <pattern> <replacement>: with `sed` the pattern is a sed
# expression and every match is replaced; with `lit` it is a literal text (a `\n` in it is a line feed) that must
# be in the file once or more. The way is an argument, not a variable of the environment.
plant_with() {
	how=$1
	shift
	name=$1 heading=$2 file=$3 pattern=$4 replacement=$5
	if [ -n "$only" ] && [ "$only" != "$name" ]; then
		return 0
	fi
	rm -rf "$scratch/plant"
	mkdir -p "$scratch/plant/src"
	cp -r "$root/src/html" "$root/src/markup" "$scratch/plant/src/"
	if [ "$how" = lit ]; then
		# A literal replacement (the text may span lines: \n in it is a line feed), checked to be there.
		python3 - "$scratch/plant/src/html/$file" "$pattern" "$replacement" <<'PY'
import sys
path, old, new = sys.argv[1:4]
# The only escape is \n (a line feed); a backslash is otherwise itself.
old = old.replace("\\n", "\n")
new = new.replace("\\n", "\n")
text = open(path).read()
if old not in text:
    raise SystemExit("the plant's text is not in %s: %r" % (path, old))
open(path, "w").write(text.replace(old, new))
PY
	else
		sed -i "s#$pattern#$replacement#g" "$scratch/plant/src/html/$file"
	fi
	if cmp -s "$root/src/html/$file" "$scratch/plant/src/html/$file"; then
		echo "control $name: the plant did not change $file ('$pattern' is not there)" >&2
		exit 1
	fi
	for f in html_tree.c html_tree_modes.c html_tree_foreign.c html_tree_fragment.c html_api.c html_core.c; do
		cc -std=gnu17 -O1 -fPIC -DGTEXT_BUILD -I"$scratch/plant/src/html" -I"$root/include" -I"$generated" $cflags \
			-c "$scratch/plant/src/html/$f" -o "$scratch/plant/$f.o"
	done
	# A plant in the public layer (html_api.c, html_core.c) is scored by the runner that drives that layer.
	runner_src=html_tree_suite.c
	case $file in html_api.c|html_core.c) runner_src=html_api_suite.c ;; esac
	cc -O1 -D_GNU_SOURCE -o "$scratch/runner-$name" "$root/tools/conformance/$runner_src" \
		"$scratch/plant/html_tree.c.o" "$scratch/plant/html_tree_modes.c.o" \
		"$scratch/plant/html_tree_foreign.c.o" "$scratch/plant/html_tree_fragment.c.o" \
		"$scratch/plant/html_api.c.o" "$scratch/plant/html_core.c.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "$name" "$heading" python3 "$suite_py" "$suite" "$scratch/runner-$name" "$exceptions" "$extra"
}
# The same with a literal text (which may span lines) in place of a sed pattern.
plant_lit() {
	plant_with lit "$@"
}

plant() {
	plant_with sed "$@"
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
plant_lit body-form-start-tag-ignored-for-an-open-form "$fails" html_tree_modes.c 'const bool in_template = parsing_template_contents(tb);\n      if (form_pointer_set(tb) && !in_template) {' 'const bool in_template = parsing_template_contents(tb);\n      if (0) {'
plant_lit table-form-start-tag-ignored-for-an-open-form "$fails" html_tree_modes.c 'if (form_pointer_set(tb) && !in_template) {\n            return R_DONE;' 'if (0) {\n            return R_DONE;'
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
# Stage 3d: each new mode, foster parenting and the template modes, one rule undone. (The template start tag's
# marker, the end tag form in a template and the pointer a form in a table in a template does not set have no
# plant here: no vector reopens a formatting element inside a template, ends a form in one or opens two forms in one
# table, so none sees those rules go (the two forms are only a unit test: both references ignore them).
# tests/test-html-tree.cpp does, with a document html5lib 1.1 reads differently from parse5 and this library,
# and the differential's plants do.)
plant_lit stack-not-cleared-to-a-table-context "$fails" html_tree_modes.c 'if (ctx == CTX_TABLE && id == T_TABLE) {' 'if (0) {'
plant_lit no-implied-colgroup-for-col "$fails" html_tree_modes.c 'HT_CK(ht_insert_element_bare(tb, T_COLGROUP, NULL));' '(void)0;'
plant_lit no-implied-tbody "$fails" html_tree_modes.c 'HT_CK(ht_insert_element_bare(tb, T_TBODY, NULL));' '(void)0;'
plant_lit no-implied-tr "$fails" html_tree_modes.c 'HT_CK(ht_insert_element_bare(tb, T_TR, NULL));' '(void)0;'
plant_lit table-start-closes-p-in-quirks-mode "$fails" html_tree_modes.c 'if (gtext_markup_document_mode(tb->doc) != GTEXT_MARKUP_MODE_QUIRKS) {' 'if (1) {'
plant_lit table-start-never-closes-p "$fails" html_tree_modes.c 'if (gtext_markup_document_mode(tb->doc) != GTEXT_MARKUP_MODE_QUIRKS) {' 'if (0) {'
plant_lit table-text-white-space-always-foster-parented "$fails" html_tree_modes.c '!tb->pend_foster && ws_prefix(p, len) != len' '!tb->pend_foster && 1'
plant_lit table-text-never-foster-parented "$fails" html_tree_modes.c '!tb->pend_foster && ws_prefix(p, len) != len' '!tb->pend_foster && 0'
plant_lit pending-white-space-dropped "$fails" html_tree_modes.c 'if (!tb->pend_foster && tb->pend_len > 0) {\n    HT_CK(ht_insert_text' 'if (0) {\n    HT_CK(ht_insert_text'
plant_lit caption-mode-never-entered "$fails" html_tree_modes.c 'tb->mode = M_IN_CAPTION;' 'tb->mode = M_IN_TABLE;'
plant_lit caption-end-keeps-formatting "$fails" html_tree_modes.c 'ht_stack_pop_until(tb, T_CAPTION);\n  ht_fmt_clear_to_marker(tb);' 'ht_stack_pop_until(tb, T_CAPTION);'
plant_lit column-group-mode-never-entered "$fails" html_tree_modes.c 'tb->mode = M_IN_COLUMN_GROUP;' 'tb->mode = M_IN_TABLE;'
plant_lit col-not-popped "$fails" html_tree_modes.c 'HT_CK(ht_insert_element(tb, tk, NULL));\n        HT_CK(ht_pop_current_acknowledge(tb, tk));\n        return R_DONE;\n      }\n      if (tk->id == T_TEMPLATE) {' 'HT_CK(ht_insert_element(tb, tk, NULL));\n        return R_DONE;\n      }\n      if (tk->id == T_TEMPLATE) {'
plant_lit table-body-context-not-cleared "$fails" html_tree_modes.c 'clear_to_context(tb, CTX_BODY);' '(void)0;'
plant_lit row-context-not-cleared "$fails" html_tree_modes.c 'clear_to_context(tb, CTX_ROW);' '(void)0;'
plant_lit cell-start-pushes-no-marker "$fails" html_tree_modes.c 'tb->mode = M_IN_CELL;\n        HT_CK(ht_fmt_push_marker(tb));' 'tb->mode = M_IN_CELL;'
plant_lit cell-end-keeps-formatting "$fails" html_tree_modes.c 'ht_stack_pop_until(tb, tk->id);\n        ht_fmt_clear_to_marker(tb);\n        tb->mode = M_IN_ROW;' 'ht_stack_pop_until(tb, tk->id);\n        tb->mode = M_IN_ROW;'
plant_lit close-cell-keeps-formatting "$fails" html_tree_modes.c '  ht_fmt_clear_to_marker(tb);\n  tb->mode = M_IN_ROW;' '  tb->mode = M_IN_ROW;'
plant_lit cell-start-tag-does-not-close-the-cell "$fails" html_tree_modes.c 'close_cell(tb);\n        return tb->status == GTEXT_HTML_TREE_OK ? R_REPROCESS : R_FAIL;\n      default:\n        break;\n    }\n  }\n  return h_in_body(tb, tk);' 'return IGNORE_START(tb);\n      default:\n        break;\n    }\n  }\n  return h_in_body(tb, tk);'
plant_lit form-in-table-keeps-the-form-open "$fails" html_tree_modes.c 'HT_CK(ht_insert_element(tb, tk, in_template ? NULL : &tb->form_el));\n          ht_stack_pop(tb);' 'HT_CK(ht_insert_element(tb, tk, in_template ? NULL : &tb->form_el));'
plant_lit hidden-input-in-table-foster-parented "$fails" html_tree_modes.c 'if (!has_attribute(tk, "type") || !attribute_is(tk, "type", "hidden")) {\n            break;' 'if (1) {\n            break;'
plant_lit table-end-tag-ignores-the-scope "$fails" html_tree_modes.c 'if (!in_table_scope(tb, T_TABLE)) {\n            return IGNORE_END(tb);' 'if (0) {\n            return IGNORE_END(tb);'
plant_lit foster-parenting-never-enabled "$fails" html_tree_modes.c 'tb->foster = true;' 'tb->foster = false;'
plant_lit foster-parented-after-the-table "$fails" html_tree.c 'ref = table;' 'ref = table->next;'
plant_lit foster-parenting-prefers-the-table-to-the-template "$fails" html_tree.c 'else if (lb == NONE_IDX || (lt != NONE_IDX && lt > lb)) {' 'else if (lb == NONE_IDX) {'
plant_lit adoption-agency-ignores-foster-parenting "$fails" html_tree.c 'ht_place_at(tb, idx - 1, &at);' 'at.parent = tb->stack[idx - 1].node; at.ref = NULL;'
plant_lit reconstruction-ignores-foster-parenting "$fails" html_tree.c '    ht_place_at(tb, tb->n - 1, &at);\n    markup_insert_unchecked(at.parent, fresh, at.ref);' '    at.parent = HT_STACK_TOP(tb).node; at.ref = NULL;\n    markup_insert_unchecked(at.parent, fresh, at.ref);'
plant_lit text-merges-with-the-wrong-neighbour "$fails" html_tree.c 'HtNode * last = at.ref ? at.ref->prev : at.parent->last_child;' 'HtNode * last = at.parent->last_child;'
plant_lit template-contents-not-used "$fails" html_tree.c '  if (tmpl) {\n    target = ((markup_element *)target)->content;' '  if (0) {\n    target = ((markup_element *)target)->content;'
plant_lit template-mode-stack-not-replaced "$fails" html_tree_modes.c '  ht_tmpl_pop(tb);\n  HT_CK(ht_tmpl_push(tb, m));' '  HT_CK(ht_tmpl_push(tb, m));'
plant_lit template-end-tag-pops-no-mode "$fails" html_tree_modes.c 'ht_fmt_clear_to_marker(tb);\n  ht_tmpl_pop(tb);\n  tb->mode = ht_tree_reset_mode(tb);\n  return tb->status' 'ht_fmt_clear_to_marker(tb);\n  tb->mode = ht_tree_reset_mode(tb);\n  return tb->status'
# Two of the 3d plants below are **error-count-only defects**: no tree differs when they are in, so only the `#errors`
# floors can see them (their heading is the floor's): the end tag template popping the cell, row and section without
# an error (the tree is the same whether the implied end tags are thorough or not), and the table end tag's error for
# the elements it pops. Their exact error lists are pinned by tests/test-html-tree.cpp instead
# (ATemplateEndTagClosesTheTableStuffInsideIt, ErrorsInTablesAreNamedAndPositioned).
plant_lit template-end-tag-implied-tags-not-thorough "below the floor" html_tree_modes.c 'ht_generate_all_implied_end_tags_thoroughly(tb);' 'ht_generate_implied_end_tags(tb, NONE_IDX);'
plant_lit template-end-of-file-ignored "$fails" html_tree_modes.c 'if (tb->ntmpl > 0) {\n        return h_in_template(tb, tk);' 'if (0) {\n        return h_in_template(tb, tk);'
plant_lit html-start-tag-merges-attributes-in-a-template "$fails" html_tree_modes.c 'if (template_on_stack(tb)) {\n        return R_DONE;\n      }\n      HT_CK(ht_merge_attributes(tb, tb->stack[0]' 'if (0) {\n        return R_DONE;\n      }\n      HT_CK(ht_merge_attributes(tb, tb->stack[0]'
plant_lit body-form-start-tag-sets-the-pointer-in-a-template "$fails" html_tree_modes.c 'const bool in_template = parsing_template_contents(tb);\n      if (form_pointer_set(tb)' 'const bool in_template = false;\n      if (form_pointer_set(tb)'
plant_lit body-start-tag-ignores-the-template "$fails" html_tree_modes.c '|| template_on_stack(tb)) {' ') {'
plant_lit reset-mode-ignores-the-template-mode "$fails" html_tree_modes.c 'return tb->ntmpl > 0 ? (InsertionMode)tb->tmpl[tb->ntmpl - 1] : M_IN_TEMPLATE;' 'return M_IN_TEMPLATE;'
plant_lit open-element-error-for-the-table-end-tag-dropped "UNEXPLAINED ERROR COUNTS" html_tree_modes.c '            ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);\n          }\n          ht_stack_pop_until(tb, T_TABLE);\n          tb->mode = ht_tree_reset_mode(tb);\n          return R_DONE;' '          }\n          ht_stack_pop_until(tb, T_TABLE);\n          tb->mode = ht_tree_reset_mode(tb);\n          return R_DONE;'
# Stage 3e: foreign content and fragments, one rule undone each. The groups are the ones the stage was scored in
# (the adjustments of names and attributes; the integration points, the breakout and the end tag rules; the fragment
# algorithm). Rules that no vector reaches have no plant here and are listed in documentation/formats/html.md: the form
# element pointer of a context with a form ancestor, the quirks mode of the context's document and the encoding attribute
# of an annotation-xml context cannot be written in the vectors' format (tests/test-html-tree-foreign.cpp has them, and
# a hand-made plant of each was seen to fail it), and the seeding of the template mode stack by a template context has no
# observable effect (the reset falls back to "in template", and the first start tag replaces the mode).
plant_lit svg-attribute-case-not-adjusted "$fails" html_tree.c 'adjusted = ht_svg_attribute_adjusted(name, len);' 'adjusted = NULL;'
plant_lit math-definitionurl-not-adjusted "$fails" html_tree.c 'else if (ns == NS_MATH && len == 13 && memcmp(name, "definitionurl", 13) == 0) {' 'else if (0) {'
plant_lit foreign-attributes-not-namespaced "$fails" html_tree.c 'if (ht_adjust_foreign_attribute(name, len, &which)) {' 'if (0) {'
plant_lit svg-element-case-not-adjusted "$fails" html_tree.c 's->disp = ns == NS_SVG ? ht_svg_tag_adjusted(bytes, len) : NULL;' 's->disp = NULL;'
plant_lit svg-tag-name-table-misses-an-entry "$fails" html_tree_foreign.c '    {"foreignobject", "foreignObject"},\n' ''
plant_lit svg-attribute-name-table-misses-an-entry "$fails" html_tree_foreign.c '    {"viewbox", "viewBox"},\n' ''
plant_lit foreign-attribute-table-misses-xml-lang "$fails" html_tree_foreign.c '    {"xml:lang", 2},\n' ''
plant_lit annotation-xml-encoding-not-honoured "$fails" html_tree.c 'ht_ci_equal(a->value, a->value_length, "text/html")' '0'
plant_lit annotation-xml-xhtml-encoding-not-honoured "$fails" html_tree.c 'ht_ci_equal(a->value, a->value_length, "application/xhtml+xml")' '0'
plant_lit foreign-elements-end-nothing-with-html-ancestors "$fails" html_tree.c 'e->fl_html = e->ns == NS_HTML ? (uint32_t)i : below ? below->fl_html : 0;' 'e->fl_html = 0;'
plant_lit breakout-never "$fails" html_tree_foreign.c 'if (is_breakout_start(tk)) {' 'if (0) {'
plant_lit breakout-list-misses-span "$fails" html_tree_foreign.c '    case T_SPAN:\n' ''
plant_lit breakout-list-misses-table "$fails" html_tree_foreign.c '    case T_TABLE:\n' ''
plant_lit breakout-list-misses-p "$fails" html_tree_foreign.c '    case T_P:\n' ''
plant_lit font-face-is-not-a-breakout-attribute "$fails" html_tree_foreign.c '|| (a->name_length == 4 && memcmp(a->name, "face", 4) == 0)' ''
plant_lit font-without-attributes-breaks-out "$fails" html_tree_foreign.c '    case T_FONT:\n      for (size_t i = 0; i < tk->nattrs; i++) {' '    case T_FONT:\n      return true;\n      for (size_t i = 0; i < tk->nattrs; i++) {'
plant_lit end-tag-p-does-not-break-out "$fails" html_tree_foreign.c "|| (t->name_length == 1 && t->name[0] == 'p')) {" ") {"
plant_lit end-tag-br-does-not-break-out "$fails" html_tree_foreign.c '(t->name_length == 2 && memcmp(t->name, "br", 2) == 0)' '0'
plant_lit breakout-pops-past-integration-points "$fails" html_tree_foreign.c '      && !(HT_STACK_TOP(tb).flags & (F_MTIP | F_HIP))) {' '      && 1) {'
plant_lit breakout-leaves-foreign-elements-open "$fails" html_tree_foreign.c '    ht_stack_pop(tb);\n  }\n  if (tb->status != GTEXT_HTML_TREE_OK) {\n    return R_FAIL;\n  }\n  return ht_html_mode_token(tb, tk);' '    break;\n  }\n  if (tb->status != GTEXT_HTML_TREE_OK) {\n    return R_FAIL;\n  }\n  return ht_html_mode_token(tb, tk);'
plant_lit foreign-end-tag-pops-only-the-current-node "$fails" html_tree_foreign.c '    while (tb->n > j) {\n      ht_stack_pop(tb);\n    }' '    ht_stack_pop(tb);'
plant_lit foreign-end-tag-walks-past-html-elements "$fails" html_tree_foreign.c 'if (j != NONE_IDX && j > HT_STACK_TOP(tb).fl_html) {' 'if (j != NONE_IDX) {'
plant_lit foreign-end-tag-never-reaches-the-html-rules "$fails" html_tree_foreign.c '  /* 7. Otherwise, process the token according to the rules of the current insertion mode. */\n  return ht_html_mode_token(tb, tk);' '  return R_DONE;'
plant_lit foreign-end-tag-looks-only-in-svg "$fails" html_tree_foreign.c 'for (unsigned ns = NS_SVG; ns <= NS_MATH; ns++) {' 'for (unsigned ns = NS_SVG; ns <= NS_SVG; ns++) {'
plant_lit svg-script-end-tag-not-popped "$fails" html_tree_foreign.c '          ht_stack_pop(tb);\n          return R_DONE;' '          return R_DONE;'
plant_lit foreign-self-closing-not-popped "$fails" html_tree_foreign.c '      if (tk->self_closing) {\n        /* "If the token' '      if (0) {\n        /* "If the token'
plant_lit body-svg-self-closing-not-popped "$fails" html_tree_foreign.c '  if (tk->self_closing) {\n    HT_CK(ht_pop_current_acknowledge(tb, tk));\n  }\n  return R_DONE;\n}' '  return R_DONE;\n}'
plant_lit body-svg-start-does-not-reconstruct "$fails" html_tree_foreign.c 'int ht_body_start_foreign(GTEXT_HTML_Tree * tb, const Tk * tk, unsigned ns) {\n  HT_CK(ht_fmt_reconstruct(tb));' 'int ht_body_start_foreign(GTEXT_HTML_Tree * tb, const Tk * tk, unsigned ns) {'
plant_lit mathml-text-integration-points-are-foreign "$fails" html_tree_foreign.c 'return F_SPECIAL | F_SC_GEN | F_MTIP;' 'return F_SPECIAL | F_SC_GEN;'
plant_lit svg-integration-points-are-foreign "$fails" html_tree_foreign.c 'return F_SPECIAL | F_SC_GEN | F_HIP;' 'return F_SPECIAL | F_SC_GEN;'
plant_lit integration-points-are-not-scope-boundaries "$fails" html_tree_foreign.c 'return F_SPECIAL | F_SC_GEN | F_HIP;' 'return F_SPECIAL | F_HIP;'
plant_lit integration-points-are-not-special "$fails" html_tree_foreign.c 'return F_SPECIAL | F_SC_GEN | F_MTIP;' 'return F_SC_GEN | F_MTIP;'
plant_lit mglyph-is-html-in-a-text-integration-point "$fails" html_tree_foreign.c '    if (start && !(tk->tok->name_length == 6 && memcmp(tk->tok->name, "mglyph", 6) == 0)' '    if (start && !(0)'
plant_lit malignmark-is-html-in-a-text-integration-point "$fails" html_tree_foreign.c '        && !(tk->tok->name_length == 10 && memcmp(tk->tok->name, "malignmark", 10) == 0)) {' '        && !(0)) {'
plant_lit svg-in-annotation-xml-is-foreign "$fails" html_tree_foreign.c 'if ((flags & F_ANNOT) && start && tk->id == T_SVG) {' 'if (0) {'
plant_lit characters-in-html-integration-points-are-foreign "$fails" html_tree_foreign.c 'if ((flags & F_HIP) && (start || character)) {' 'if ((flags & F_HIP) && start) {'
plant_lit start-tags-in-html-integration-points-are-foreign "$fails" html_tree_foreign.c 'if ((flags & F_HIP) && (start || character)) {' 'if ((flags & F_HIP) && character) {'
plant_lit characters-in-text-integration-points-are-foreign "$fails" html_tree_foreign.c '  if (flags & F_MTIP) {\n    if (character) {\n      return false;\n    }' '  if (flags & F_MTIP) {'
plant_lit end-of-file-handled-by-foreign-rules "$fails" html_tree_foreign.c '  if (!character && tk->type == TK_EOF) {\n    return false;\n  }' ''
plant_lit nul-in-foreign-content-dropped "$fails" html_tree_foreign.c 'if (!ht_insert_text(tb, "\xEF\xBF\xBD", 3)) {' 'if (0) {'
plant_lit foreign-characters-keep-frameset-ok "$fails" html_tree_foreign.c '        if (!is_ws_char(p[i])) {' '        if (0) {'
plant_lit cdata-never-allowed "$fails" html_tree_foreign.c 'return tb && ht_adjusted_ns(tb) != NS_HTML;' 'return false;'
plant_lit cdata-always-allowed "$fails" html_tree_foreign.c 'return tb && ht_adjusted_ns(tb) != NS_HTML;' 'return true;'
plant_lit foreign-elements-take-the-html-namespace-name-slots "$fails" html_tree_foreign.c '      const unsigned ns = ht_adjusted_ns(tb);\n      uint32_t id = 0;' '      const unsigned ns = NS_SVG;\n      uint32_t id = 0;'
plant_lit adjusted-current-node-ignores-the-fragment-context "$fails" html_tree_foreign.c '  if (tb->frag && tb->n == 1) {\n    *ns = tb->ctx_ns;' '  if (0) {\n    *ns = tb->ctx_ns;'
plant_lit dispatcher-ignores-foreign-characters "$fails" html_tree_modes.c '    if (ht_use_foreign_rules(tb, NULL, true)) {' '    if (0) {'
# Fragments.
plant_lit fragment-tokenizer-state-never-set "$fails" html_tree_fragment.c '    if (state != GTEXT_HTML_STATE_DATA) {' '    if (0) {'
plant_lit fragment-title-is-not-rcdata "$fails" html_tree_fragment.c 'if (tb->ctx_id == T_TITLE || tb->ctx_id == T_TEXTAREA) {' 'if (tb->ctx_id == T_TEXTAREA) {'
plant_lit fragment-script-is-not-script-data "$fails" html_tree_fragment.c 'else if (tb->ctx_id == T_SCRIPT) {' 'else if (0) {'
plant_lit fragment-plaintext-is-not-plaintext "$fails" html_tree_fragment.c 'else if (tb->ctx_id == T_PLAINTEXT) {' 'else if (0) {'
plant_lit fragment-noscript-ignores-scripting "$fails" html_tree_fragment.c 'state = c->scripting ? GTEXT_HTML_STATE_RAWTEXT : GTEXT_HTML_STATE_DATA;' 'state = GTEXT_HTML_STATE_DATA;'
plant_lit fragment-mode-not-reset-from-the-context "$fails" html_tree_fragment.c '  tb->mode = ht_tree_reset_mode(tb);\n  /* Step 21. */' '  tb->mode = M_IN_BODY;\n  /* Step 21. */'
plant_lit fragment-reset-ignores-the-context-element "$fails" html_tree_modes.c '        id = tb->ctx_id;' '        id = tb->stack[0].id;'
plant_lit fragment-result-not-moved "$fails" html_tree_fragment.c '  markup_move_children_unchecked(tb->stack[0].node, tb->frag_node);' '  (void)0;'
plant_lit fragment-select-context-allows-select "$fails" html_tree_modes.c '      if (tb->ctx_select) {\n        /* The fragment context element is a select element (fragment case): "Parse error. Ignore\n           the token." */' '      if (0) {\n        /* The fragment context element is a select element (fragment case): "Parse error. Ignore\n           the token." */'
plant_lit fragment-select-context-allows-input "$fails" html_tree_modes.c '      if (tb->ctx_select) {\n        /* The fragment context element is a select element (fragment case). */' '      if (0) {\n        /* The fragment context element is a select element (fragment case). */'
plant_lit fragment-template-context-is-not-template-contents "$fails" html_tree_modes.c '  return template_on_stack(tb) || tb->ctx_template;' '  return template_on_stack(tb);'
plant_lit fragment-end-html-after-body-not-ignored "$fails" html_tree_modes.c '        if (tb->frag) {\n          /* The fragment context element is non-null' '        if (0) {\n          /* The fragment context element is non-null'
plant_lit fragment-frameset-end-switches-mode "$fails" html_tree_modes.c 'if (!tb->frag && !is_current(tb, T_FRAMESET)) {' 'if (!is_current(tb, T_FRAMESET)) {'
plant_lit fragment-integration-point-context-is-foreign "$fails" html_tree_fragment.c '    if (name_is(c, "foreignObject") || name_is(c, "desc") || name_is(c, "title")) {' '    if (0) {'
plant_lit fragment-text-integration-point-context-is-foreign "$fails" html_tree_fragment.c '    if (name_is(c, "mi") || name_is(c, "mo") || name_is(c, "mn") || name_is(c, "ms")\n        || name_is(c, "mtext")) {' '    if (0) {'
plant_lit fragment-context-namespace-ignored "$fails" html_tree_fragment.c '  tb->ctx_ns = (uint8_t)c->ns;' '  tb->ctx_ns = NS_HTML;'
# The public layer over the builder (html_api.c, html_core.c), scored through the public API.
plant_lit api-scripting-flag-dropped "$fails" html_api.c '  to.scripting = o.scripting;' '  to.scripting = false;'
plant_lit api-fragment-context-name-cut-short "$fails" html_api.c '  fc.local_name_length = strlen(context->local_name);' '  fc.local_name_length = 1;'
plant_lit api-fragment-namespace-dropped "$fails" html_api.c '  fc.ns = context->ns;' '  fc.ns = GTEXT_HTML_NS_HTML;'
plant_lit api-fragment-scripting-dropped "$fails" html_api.c '  fc.scripting = o.scripting;' '  fc.scripting = false;'
plant_lit api-feed-loses-its-last-byte "$fails" html_api.c '    const GTEXT_HTML_TreeStatus ts = gtext_html_tree_feed(p->tree, d, len);' '    const GTEXT_HTML_TreeStatus ts = gtext_html_tree_feed(p->tree, d, len > 1 ? len - 1 : len);'
plant_lit api-finish-does-not-finish "$fails" html_api.c '  const GTEXT_HTML_TreeStatus ts = gtext_html_tree_finish(p->tree);\n  if (ts != GTEXT_HTML_TREE_OK) {\n    return parser_fail_tree(p, ts);\n  }\n  p->finished = true;' '  const GTEXT_HTML_TreeStatus ts = GTEXT_HTML_TREE_OK;\n  p->finished = true;'
plant_lit api-result-mode-lies "$fails" html_api.c '    result->mode = gtext_markup_document_mode(doc);' '    result->mode = GTEXT_MARKUP_MODE_NO_QUIRKS;'
plant_lit api-error-list-is-empty "below the floor" html_core.c '  const size_t count = gtext_html_tree_error_count(tree);' '  const size_t count = 0;'
# A crash is a failure, not a missing answer.
plant crash "$fails" html_tree_modes.c 'static bool is_ws(char c) {' 'static bool is_ws(char c) { if (c == 0x61) { *(volatile int *)0 = 0; }'
if [ -n "$only" ] && [ "$ran" = 0 ]; then
	echo "no control is named $only" >&2
	exit 1
fi
