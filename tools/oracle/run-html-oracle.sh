#!/bin/sh
# Check this library's XHTML write mode against html5lib and parse5.
#
#   tools/oracle/run-html-oracle.sh [--vectors | --diff | --controls] [-- extra args for html_diff.py]
#
# Two references in one pinned container (containers/html), and three gates, in order:
#
#   1. tools/oracle/html_vectors.py - is the oracle itself right? Every tree-construction
#      vector of the pinned web-platform-tests tree that does not need script to run is put
#      to both references, and each must reproduce the expected tree or be listed in
#      tools/oracle/html_deviations.txt with a reason.
#   2. tools/oracle/html_diff.py - the differential. Trees written with `xhtml` are read by
#      both references and held to the tree that was written, under html_equivalent.h.
#      A tree with no hazards that is read differently fails the run; every divergence
#      on a tree with hazards is a line of tools/oracle/html_divergences.txt.
#
#   3. tools/oracle/html_tokenizer_diff.py - the HTML tokenizer (src/html/html_tokenizer.c) against
#      html5lib's and parse5's tokenizers, over the vectors' inputs, the tree-construction vectors'
#      documents, and generated and mutated inputs, cut every way. Every disagreement is a line of
#      tools/oracle/html_tokenizer_divergences.txt (a behaviour of a reference the standard does not
#      have) or fails the run.
#
#   4. tools/oracle/html_tree_diff.py - the HTML tree builder (src/html/html_tree*.c) against the same two
#      references over generated, mutated and vector documents restricted to the constructs it parses; every
#      divergence is a line of tools/oracle/html_tree_divergences.txt or fails the run. (--tree)
#      tools/oracle/html_tree_extra_check.py checks the hand-chosen cases of tools/conformance/
#      html_tree_extra.dat against both references.
#
# HTML_ORACLE_COUNT and HTML_ORACLE_SEED size and seed the generated part of the second
# (the list describes the default 4000 and seed 1; any other is a hunt). Needs the
# built-here image: make oracle-images.
#
# --controls makes both gates fail on purpose and requires that they do, each for the
# right reason: a gate whose assertion no longer reaches its subject prints the same clean
# line as one that passes. See check_controls below for the list and what each shows.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

mkdir -p "$root/build"
runner=$root/build/html-oracle-ours
cc -std=gnu17 -O1 -Wall -Wextra -o "$runner" "$root/tools/oracle/html_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

trunner=$root/build/html-tokenizer-runner
cc -std=gnu17 -D_GNU_SOURCE -O1 -Wall -Wextra -o "$trunner" "$root/tools/conformance/html_tokenizer_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

trree=$root/build/html-tree-runner-oracle
cc -std=gnu17 -D_GNU_SOURCE -O1 -Wall -Wextra -o "$trree" "$root/tools/conformance/html_tree_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

mode=${1:-all}
[ $# -gt 0 ] && shift
if [ "$1" = "--" ]; then
	shift
fi

run_vectors() {
	python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_vectors.py" "$@"
}
run_diff() {
	python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_diff.py" "$runner" "$@"
}

run_tree() {
	python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_tree_diff.py" "$trree" "$@" && \
	python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_tree_extra_check.py"
}

run_tokens() {
	python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_tokenizer_diff.py" "$trunner" "$@"
}

# expect <what> <heading> <detail> <command...>: the command must exit 3 and say both.
log=$(mktemp)
scratch=$(mktemp -d)
trap 'rm -f "$log"; rm -rf "$scratch"' EXIT
expect() {
	what=$1 want=$2 detail=$3
	if [ -n "$only" ] && [ "$only" != "$what" ]; then
		return 0
	fi
	shift 3
	status=0
	"$@" </dev/null >/dev/null 2>"$log" || status=$?
	if [ "$status" != 3 ] || ! grep -q "$want" "$log" || ! grep -q "$detail" "$log"; then
		echo "control $what: the gate exited $status and did not say '$want' with '$detail'" >&2
		cat "$log" >&2
		exit 1
	fi
	echo "control $what: caught"
}

# The writer defects planted in the *source*: a copy of xml_writer.c with one fix undone,
# compiled into a runner of its own (its object is linked ahead of the archive, so it
# supplies every writer symbol and the archive's copy is never pulled in). Each of these is a
# defect this differential found and the writer was repaired for; the control shows it would
# find it again.
source_plant() {
	name=$1 pattern=$2 replacement=$3 heading=$4 detail=$5
	if [ -n "$only" ] && [ "$only" != "$name" ]; then
		return 0
	fi
	cp "$root/src/xml/xml_writer.c" "$scratch/xml_writer.c"
	sed -i "s#$pattern#$replacement#" "$scratch/xml_writer.c"
	if cmp -s "$root/src/xml/xml_writer.c" "$scratch/xml_writer.c"; then
		echo "control $name: the plant did not change xml_writer.c ('$pattern' is not there)" >&2
		exit 1
	fi
	cc -std=gnu17 -O1 -fPIC -DGTEXT_BUILD -I"$root/src/xml" -I"$root/src/markup" -I"$root/src" -I"$root/include" -I"$generated" $cflags -c "$scratch/xml_writer.c" -o "$scratch/xml_writer.o"
	cc -std=gnu17 -O1 -o "$scratch/runner-$name" "$root/tools/oracle/html_ours.c" "$scratch/xml_writer.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "$name" "$heading" "$detail" \
		python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_diff.py" "$scratch/runner-$name"
}

# The tokenizer's own source with one rule undone, compiled into a runner of its own (its object is
# linked ahead of the archive, so it supplies every symbol of html_tokenizer.c and the archive's copy
# is never pulled in). The differential must fail on it, and say it found a divergence.
tokenizer_plant() {
	name=$1 pattern=$2 replacement=$3 heading=${4:-Divergences with no entry} detail=${5:-parse5}
	if [ -n "$only" ] && [ "$only" != "tok-$name" ]; then
		return 0
	fi
	cp "$root/src/html/html_tokenizer.c" "$scratch/html_tokenizer.c"
	sed -i "s#$pattern#$replacement#g" "$scratch/html_tokenizer.c"
	if cmp -s "$root/src/html/html_tokenizer.c" "$scratch/html_tokenizer.c"; then
		echo "control tok-$name: the plant did not change html_tokenizer.c ('$pattern' is not there)" >&2
		exit 1
	fi
	cc -std=gnu17 -O1 -fPIC -DGTEXT_BUILD -I"$root/src/html" -I"$root/include" -I"$generated" $cflags -c "$scratch/html_tokenizer.c" -o "$scratch/html_tokenizer.o"
	cc -std=gnu17 -D_GNU_SOURCE -O1 -o "$scratch/trunner-$name" "$root/tools/conformance/html_tokenizer_suite.c" "$scratch/html_tokenizer.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "tok-$name" "$heading" "$detail" \
		python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_tokenizer_diff.py" "$scratch/trunner-$name"
}

# The tree builder's own source with one rule undone, compiled into a runner of its own (its objects
# are linked ahead of the archive and supply every symbol of the two files). The differential must
# fail on it. The plant copies src/html and src/markup, since the tag table is in a header both read.
# tree_plant_with <sed|lit> <name> <file> <pattern> <replacement> [heading [detail]]: `sed` takes a sed
# expression, `lit` a literal text (a `\n` in it is a line feed); the way is an argument.
tree_plant_with() {
	how=$1
	shift
	name=$1 file=$2 pattern=$3 replacement=$4 heading=${5:-Divergences with no entry\\|Listed divergences whose count has changed\\|Listed divergences that are gone\\|The invariant checker failed} detail=${6:-.}
	if [ -n "$only" ] && [ "$only" != "tree-$name" ]; then
		return 0
	fi
	rm -rf "$scratch/tplant"
	mkdir -p "$scratch/tplant/src"
	cp -r "$root/src/html" "$root/src/markup" "$scratch/tplant/src/"
	if [ "$how" = lit ]; then
		# A literal text (which may span lines: \n in it is a line feed), checked to be there.
		python3 - "$scratch/tplant/src/html/$file" "$pattern" "$replacement" <<'PY'
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
		sed -i "s#$pattern#$replacement#g" "$scratch/tplant/src/html/$file"
	fi
	if cmp -s "$root/src/html/$file" "$scratch/tplant/src/html/$file"; then
		echo "control tree-$name: the plant did not change $file ('$pattern' is not there)" >&2
		exit 1
	fi
	for f in html_tree.c html_tree_modes.c html_tree_foreign.c html_tree_fragment.c; do
		cc -std=gnu17 -O1 -fPIC -DGTEXT_BUILD -I"$scratch/tplant/src/html" -I"$root/include" -I"$generated" $cflags \
			-c "$scratch/tplant/src/html/$f" -o "$scratch/tplant/$f.o"
	done
	cc -std=gnu17 -D_GNU_SOURCE -O1 -o "$scratch/trree-$name" "$root/tools/conformance/html_tree_suite.c" \
		"$scratch/tplant/html_tree.c.o" "$scratch/tplant/html_tree_modes.c.o" \
		"$scratch/tplant/html_tree_foreign.c.o" "$scratch/tplant/html_tree_fragment.c.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "tree-$name" "$heading" "$detail" \
		python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_tree_diff.py" "$scratch/trree-$name"
}

# The same with a literal text in place of a sed pattern.
tree_plant_lit() {
	tree_plant_with lit "$@"
}

tree_plant() {
	tree_plant_with sed "$@"
}

check_controls() {
	# The controls are about the default population, which the list describes, whatever size a
	# hunt asked for in the environment.
	unset HTML_ORACLE_COUNT HTML_ORACLE_SEED
	# The vector gate: the oracle must be seen to fail.
	expect vectors-expectation "Vectors a reference does not reproduce" "input" run_vectors --plant expectation
	expect vectors-answer "Vectors a reference does not reproduce" "input" run_vectors --plant answer
	expect vectors-stale "Listed deviations that are gone" "html5lib" run_vectors --plant stale
	expect vectors-signature "signature has changed" "listed" run_vectors --plant signature
	# The differential, against planted corruptions of the writer's output.
	for plant in selfclose-empty cdata-script void-end-tag; do
		expect "diff-$plant" "Trees with no hazards that an HTML parser reads differently" "wrote" run_diff --plant "$plant"
	done
	# Two corruptions change what the XML parser reads as well, and are caught on that side first.
	for plant in drop-doctype wrong-escape; do
		expect "diff-$plant" "The XML parser does not read back what was written" "wrote" run_diff --plant "$plant"
	done
	expect diff-stale-entry "Listed divergences that are gone" "hazard:" run_diff --plant stale-entry
	expect diff-change-signature "Listed divergences whose count has changed" "listed" run_diff --plant change-signature
	expect diff-no-list "Divergences with no entry" "hazard:" run_diff --plant no-list
	# The differential, against the writer itself with one repair undone. Every repair the differential led to
	# is here once, so that none can be taken out without this gate saying so. (An attribute with a colon and
	# no namespace needs no repair: the writer refuses it as not an XML name, and a test shows it.)
	defects="Trees with no hazards that an HTML parser reads differently"
	unexplained="Divergences with no entry"
	source_plant plaintext 'XC_BLOCK | XC_RAWISH | XC_SPECIAL | XC_ODD}' 'XC_BLOCK | XC_RAWISH | XC_SPECIAL}' "$defects" plaintext
	source_plant nobr 'if ((cats & XC_NOBR) && (in & HZ_NOBR))' 'if (0)' "$defects" nobr
	source_plant c1-reference 'if (w->xhtml && cp >= 0x80 && cp <= 0x9F) {' 'if (0) {' "$defects" wrote
	folded="The writer calls the folded form of a table name safe"
	source_plant foreign-element-names '!gtext_markup_html_foreign_name_survives(ns, false, local)' '0' "$folded" svg-element-folded
	# The fixed case walks every table entry, so removing either check shows there first.
	source_plant foreign-attribute-names '!gtext_markup_html_foreign_name_survives(ns, true, a.local)' '0' \
		"$folded" svg-attribute-folded
	source_plant select-depth 'if (in & HZ_SELECT) {' 'if (0) {' "$defects" wrote
	source_plant ruby 'if ((cats & XC_RUBYPART) && (in & HZ_RUBY)) {' 'if (0) {' "$unexplained" ruby
	source_plant carriage-return-in-table "else if (memchr(data, '\\\\r', len)) {" 'else if (0) {' "$unexplained" parse5
	source_plant colgroup-text 'if ((cats & XC_COLGROUP) && data' 'if (0 \&\& data' "$defects" colgroup
	source_plant head-text 'if (data ? !html_space_plain(data, len)' 'if (data ? 0' "$defects" wrote
	source_plant noscript-in-head 'strcmp(local, "noscript") == 0 && data' '0 \&\& data' "$defects" noscript
	source_plant separator-after-root 'if (any && !(w->xhtml && seen_element)' 'if (any' "$defects" wrote
	source_plant html-children-indented 'const bool own_children_plain = xe && (xcats_of(e) & XC_HTML) != 0;' 'const bool own_children_plain = 0;' "$defects" wrote
	source_plant search-in-the-li-chain '{"search", XC_BLOCK | XC_SPECIAL | XC_CONT}' '{"search", XC_BLOCK | XC_SPECIAL}' "$defects" search
	source_plant markup-in-rawish 'if ((cats & XC_RAWISH) && k != GTEXT_MARKUP_KIND_TEXT) {' 'if ((cats \& XC_RAWISH) \&\& k == GTEXT_MARKUP_KIND_ELEMENT) {' "$defects" wrote
	source_plant reference-in-rawish '|| text_needs_reference(w, data, len)' '|| 0' "$defects" wrote

	# The tokenizer's differential. Its answer corrupted five ways, the list three ways, and the
	# tokenizer itself compiled with one of its rules undone, each of which the references must
	# disagree with (the population is built to reach every state; a plant that survives it says
	# where it is thin).
	for plant in drop-token change-attribute swap-error-code move-error extra-error; do
		expect "tok-$plant" "Divergences with no entry" "parse5" run_tokens --plant "$plant"
	done
	expect tok-stale-entry "Listed divergences that are gone" "planted-gone" run_tokens --plant stale-entry
	expect tok-change-count "Listed divergences whose count has changed" "listed" run_tokens --plant change-count
	expect tok-no-list "Divergences with no entry" "parse5" run_tokens --plant no-list
	tokenizer_plant attribute-legacy-rule 'if (in_attribute(t) \&\& last != .;.) {' 'if (0 \&\& in_attribute(t) \&\& last != 0x3b) {'
	tokenizer_plant c1-remap 'code = c1_remap\[code - 0x80\];' 'code = code;'
	tokenizer_plant appropriate-end-tag 'return t->have_last_start \&\& ' 'return 1 || '
	tokenizer_plant comment-end-bang 'ar_put(t, "--!", 3);' 'ar_put(t, "--", 2);'
	tokenizer_plant double-escape-word '"script", 6' '"scripx", 6'
	tokenizer_plant doctype-system-keyword '"system", 6' '"systex", 6'
	tokenizer_plant duplicate-attribute-kept 't->attr_dropped = true;' 't->attr_dropped = false;'
	tokenizer_plant name-semicolon-window 'if (n < longest \&\& p < t->in_length \&\& t->in\[p\] == .;.) {' 'if (0) {'
	tokenizer_plant crlf-two-lines "\*n = s\[p + 1\] == '.n' ? 2 : 1;" '*n = 1;'
	tokenizer_plant names-not-lowered 'ar_cp(t, (uint32_t)c + 0x20);' 'ar_cp(t, (uint32_t)c);'
	tokenizer_plant reference-number-wraps 'return code > CODE_CAP ? CODE_CAP : code;' 'return code;'
	tokenizer_plant surrogate-reference 'if (code >= 0xD800 \&\& code <= 0xDFFF) {' 'if (0) {'
	tokenizer_plant self-closing-flag 't->token.self_closing = true;' '(void)0;'
	tokenizer_plant script-escape-start 'SWITCH(ST_SCRIPT_ESC_START);' 'SWITCH(ST_SCRIPT_DATA);'
	tokenizer_plant cdata-bracket-pair 'text_cp(t, .\].);' '(void)0;'
	tokenizer_plant ambiguous-ampersand-error 'perr(t, GTEXT_HTML_ERR_UNKNOWN_NAMED_CHARACTER_REFERENCE);' '(void)0;'
	# The scanner that says which `&name;` is in an attribute value must not be able to hide a missing
	# error: an unknown-named-character-reference error missing in text (the attribute ones are not
	# parse5's, and are removed from ours by the scanner, and only from ours) still fails the run; and
	# one missing in attributes makes the listed divergence disappear, which fails the run too.
	tokenizer_plant unknown-named-missing-in-text 'perr(t, GTEXT_HTML_ERR_UNKNOWN_NAMED_CHARACTER_REFERENCE);' 'if (in_attribute(t)) { perr(t, GTEXT_HTML_ERR_UNKNOWN_NAMED_CHARACTER_REFERENCE); }'
	tokenizer_plant unknown-named-missing-in-attribute 'perr(t, GTEXT_HTML_ERR_UNKNOWN_NAMED_CHARACTER_REFERENCE);' 'if (!in_attribute(t)) { perr(t, GTEXT_HTML_ERR_UNKNOWN_NAMED_CHARACTER_REFERENCE); }' 'Listed divergences that are gone' 'unknown-named-reference-in-attribute'
	tokenizer_plant after-doctype-name-error 'perr(t, GTEXT_HTML_ERR_INVALID_CHARACTER_SEQUENCE_AFTER_DOCTYPE_NAME);' '(void)0;'


	# The tree builder's differential: its answer corrupted two ways, the list three ways, the extra
	# cases' expectation once, and the builder compiled with one rule undone.
	for plant in ours-extra-line ours-lost-line; do
		expect "tree-$plant" "Divergences with no entry" "unexplained" run_tree_diff --plant "$plant"
	done
	expect tree-stale-entry "Listed divergences that are gone" "planted-gone" run_tree_diff --plant stale-entry
	expect tree-change-count "Listed divergences whose count has changed" "listed" run_tree_diff --plant change-count
	expect tree-no-list "Divergences with no entry" "alone" run_tree_diff --plant no-list
	expect tree-witness "A witness of a reference deviation does not hold" "planted" run_tree_diff --plant witness
	expect tree-uncorroborated "Uncorroborated documents whose count has changed" "listed" run_tree_diff --plant uncorroborated
	expect tree-extra-expectation "Extra cases a reference does not reproduce" "html_tree_extra" \
		python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_tree_extra_check.py" --plant expectation
	tree_plant noahs-ark-keeps-four html_tree.c 'if (count >= 3) {' 'if (count >= 4) {'
	tree_plant adoption-outer-loop-once html_tree.c 'outer < 8' 'outer < 1'
	tree_plant adoption-inner-loop-limit html_tree.c 'inner > 3' 'inner > 300'
	tree_plant adoption-no-bookmark-move html_tree.c 'if (last_node == fb_node) {' 'if (0) {'
	tree_plant li-does-not-close-li html_tree_modes.c 'if (j != NONE_IDX \&\& j >= HT_STACK_TOP(tb).fl_li) {' 'if (0) {'
	tree_plant reconstruct-skipped html_tree_modes.c 'if (!ht_fmt_reconstruct(tb) || !ht_insert_text(tb, p, len)) {' 'if (!ht_insert_text(tb, p, len)) {'
	tree_plant p-closed-outside-button-scope html_tree.c 'if (ht_stack_in_scope(tb, T_P, SCOPE_BUTTON)) {' 'if (ht_stack_in_scope(tb, T_P, SCOPE_GEN)) {'
	tree_plant text-not-merged html_tree.c 'if (last \&\& last->kind == GTEXT_MARKUP_KIND_TEXT) {' 'if (0) {'
	tree_plant marquee-is-not-a-scope-boundary html_tree_private.h 'X(T_MARQUEE, "marquee", F_SPECIAL | F_SC_GEN)' 'X(T_MARQUEE, "marquee", F_SPECIAL)'
	tree_plant ol-is-not-a-list-scope-boundary html_tree_private.h 'X(T_OL, "ol", F_SPECIAL | F_SC_LIST)' 'X(T_OL, "ol", F_SPECIAL)'
	tree_plant implied-end-tags-stop html_tree.c 'while (tb->n > 0 \&\& (HT_STACK_TOP(tb).flags \& F_IMPLIED)' 'while (0 \&\& (HT_STACK_TOP(tb).flags \& F_IMPLIED)'
	tree_plant frameset-ok-never-cleared html_tree_modes.c 'if (ws_prefix(p, len) != len) {' 'if (0) {'
	tree_plant pre-newline-kept html_tree_modes.c 'tb->skip_lf = true;' 'tb->skip_lf = false;'
	tree_plant html-attributes-not-merged html_tree.c 'present = true;' 'present = false;'
	# Stage 3d: a rule of each new mode undone, foster parenting four ways, the template modes five, and the quirks
	# rule of the table start tag. The population holds tables and templates (generated, and the vectors'), so a
	# plant that changes a tree there is a divergence with no entry or a changed count.
	tree_plant_lit table-stack-not-cleared-to-a-table-context html_tree_modes.c 'if (ctx == CTX_TABLE && id == T_TABLE) {' 'if (0) {'
	tree_plant_lit table-start-closes-p-in-quirks-mode html_tree_modes.c 'if (gtext_markup_document_mode(tb->doc) != GTEXT_MARKUP_MODE_QUIRKS) {' 'if (1) {'
	tree_plant_lit table-text-never-foster-parented html_tree_modes.c '!tb->pend_foster && ws_prefix(p, len) != len' '!tb->pend_foster && 0'
	tree_plant_lit caption-end-keeps-formatting html_tree_modes.c 'ht_stack_pop_until(tb, T_CAPTION);\n  ht_fmt_clear_to_marker(tb);' 'ht_stack_pop_until(tb, T_CAPTION);'
	tree_plant_lit column-group-col-not-popped html_tree_modes.c 'HT_CK(ht_insert_element(tb, tk, NULL));\n        HT_CK(ht_pop_current_acknowledge(tb, tk));\n        return R_DONE;\n      }\n      if (tk->id == T_TEMPLATE) {' 'HT_CK(ht_insert_element(tb, tk, NULL));\n        return R_DONE;\n      }\n      if (tk->id == T_TEMPLATE) {'
	tree_plant_lit table-body-context-not-cleared html_tree_modes.c 'clear_to_context(tb, CTX_BODY);' '(void)0;'
	tree_plant_lit row-context-not-cleared html_tree_modes.c 'clear_to_context(tb, CTX_ROW);' '(void)0;'
	tree_plant_lit close-cell-keeps-formatting html_tree_modes.c '  ht_fmt_clear_to_marker(tb);\n  tb->mode = M_IN_ROW;' '  tb->mode = M_IN_ROW;'
	tree_plant_lit cell-start-tag-does-not-close-the-cell html_tree_modes.c 'close_cell(tb);\n        return tb->status == GTEXT_HTML_TREE_OK ? R_REPROCESS : R_FAIL;\n      default:\n        break;\n    }\n  }\n  return h_in_body(tb, tk);' 'return IGNORE_START(tb);\n      default:\n        break;\n    }\n  }\n  return h_in_body(tb, tk);'
	tree_plant_lit foster-parented-after-the-table html_tree.c 'ref = table;' 'ref = table->next;'
	tree_plant_lit foster-parenting-prefers-the-table-to-the-template html_tree.c 'else if (lb == NONE_IDX || (lt != NONE_IDX && lt > lb)) {' 'else if (lb == NONE_IDX) {'
	tree_plant_lit adoption-agency-ignores-foster-parenting html_tree.c 'ht_place_at(tb, idx - 1, &at);' 'at.parent = tb->stack[idx - 1].node; at.ref = NULL;'
	tree_plant_lit reconstruction-ignores-foster-parenting html_tree.c '    ht_place_at(tb, tb->n - 1, &at);\n    markup_insert_unchecked(at.parent, fresh, at.ref);' '    at.parent = HT_STACK_TOP(tb).node; at.ref = NULL;\n    markup_insert_unchecked(at.parent, fresh, at.ref);'
	tree_plant_lit template-contents-not-used html_tree.c '  if (tmpl) {\n    target = ((markup_element *)target)->content;' '  if (0) {\n    target = ((markup_element *)target)->content;'
	tree_plant_lit template-mode-stack-not-replaced html_tree_modes.c '  ht_tmpl_pop(tb);\n  HT_CK(ht_tmpl_push(tb, m));' '  HT_CK(ht_tmpl_push(tb, m));'
	tree_plant_lit template-end-tag-pops-no-mode html_tree_modes.c 'ht_fmt_clear_to_marker(tb);\n  ht_tmpl_pop(tb);\n  tb->mode = ht_tree_reset_mode(tb);\n  return tb->status' 'ht_fmt_clear_to_marker(tb);\n  tb->mode = ht_tree_reset_mode(tb);\n  return tb->status'
	tree_plant_lit template-start-pushes-no-marker html_tree_modes.c 'HT_CK(ht_fmt_push_marker(tb));\n          tb->frameset_ok = false;\n          HT_CK(ht_tmpl_push' 'tb->frameset_ok = false;\n          HT_CK(ht_tmpl_push'
	tree_plant_lit template-end-of-file-ignored html_tree_modes.c 'if (tb->ntmpl > 0) {\n        return h_in_template(tb, tk);' 'if (0) {\n        return h_in_template(tb, tk);'
	tree_plant_lit body-form-start-tag-sets-the-pointer-in-a-template html_tree_modes.c 'const bool in_template = parsing_template_contents(tb);\n      if (form_pointer_set(tb)' 'const bool in_template = false;\n      if (form_pointer_set(tb)'
	tree_plant_lit html-start-tag-merges-attributes-in-a-template html_tree_modes.c 'if (template_on_stack(tb)) {\n        return R_DONE;\n      }\n      HT_CK(ht_merge_attributes(tb, tb->stack[0]' 'if (0) {\n        return R_DONE;\n      }\n      HT_CK(ht_merge_attributes(tb, tb->stack[0]'
	tree_plant_lit form-end-tag-in-a-template-asks-for-the-pointer html_tree_modes.c 'if (parsing_template_contents(tb)) {\n        if (!ht_stack_in_scope(tb, T_FORM, SCOPE_GEN)) {' 'if (0) {\n        if (!ht_stack_in_scope(tb, T_FORM, SCOPE_GEN)) {'
	tree_plant_lit reset-mode-ignores-the-template-mode html_tree_modes.c 'return tb->ntmpl > 0 ? (InsertionMode)tb->tmpl[tb->ntmpl - 1] : M_IN_TEMPLATE;' 'return M_IN_TEMPLATE;'
	# Stage 3e: a rule of foreign content or of the fragment algorithm undone, per group: the adjustments of names and
	# attributes, the integration points, the breakout list and its end tags, the foreign end tag's walk, the text rules,
	# CDATA, and the fragment algorithm (the tokenizer state, the reset from the context, the select, template and
	# frameset contexts, the integration point contexts, the result). A fragment context that is a form, a quirks mode and
	# the encoding of an annotation-xml context cannot be given to a reference, so those have no plant here.
	tree_plant_lit svg-attribute-case-not-adjusted html_tree.c 'adjusted = ht_svg_attribute_adjusted(name, len);' 'adjusted = NULL;'
	tree_plant_lit math-definitionurl-not-adjusted html_tree.c 'else if (ns == NS_MATH && len == 13 && memcmp(name, "definitionurl", 13) == 0) {' 'else if (0) {'
	tree_plant_lit foreign-attributes-not-namespaced html_tree.c 'if (ht_adjust_foreign_attribute(name, len, &which)) {' 'if (0) {'
	tree_plant_lit svg-element-case-not-adjusted html_tree.c 's->disp = ns == NS_SVG ? ht_svg_tag_adjusted(bytes, len) : NULL;' 's->disp = NULL;'
	tree_plant_lit annotation-xml-encoding-not-honoured html_tree.c 'ht_ci_equal(a->value, a->value_length, "text/html")' '0'
	tree_plant_lit foreign-elements-end-nothing-with-html-ancestors html_tree.c 'e->fl_html = e->ns == NS_HTML ? (uint32_t)i : below ? below->fl_html : 0;' 'e->fl_html = 0;'
	tree_plant_lit breakout-never html_tree_foreign.c 'if (is_breakout_start(tk)) {' 'if (0) {'
	tree_plant_lit breakout-list-misses-span html_tree_foreign.c '    case T_SPAN:\n' ''
	tree_plant_lit font-face-is-not-a-breakout-attribute html_tree_foreign.c '|| (a->name_length == 4 && memcmp(a->name, "face", 4) == 0)' ''
	tree_plant_lit end-tag-p-does-not-break-out html_tree_foreign.c "|| (t->name_length == 1 && t->name[0] == 'p')) {" ") {"
	tree_plant_lit end-tag-br-does-not-break-out html_tree_foreign.c '(t->name_length == 2 && memcmp(t->name, "br", 2) == 0)' '0'
	tree_plant_lit breakout-pops-past-integration-points html_tree_foreign.c '      && !(HT_STACK_TOP(tb).flags & (F_MTIP | F_HIP))) {' '      && 1) {'
	tree_plant_lit foreign-end-tag-pops-only-the-current-node html_tree_foreign.c '    while (tb->n > j) {\n      ht_stack_pop(tb);\n    }' '    ht_stack_pop(tb);'
	tree_plant_lit foreign-end-tag-walks-past-html-elements html_tree_foreign.c 'if (j != NONE_IDX && j > HT_STACK_TOP(tb).fl_html) {' 'if (j != NONE_IDX) {'
	tree_plant_lit foreign-end-tag-looks-only-in-svg html_tree_foreign.c 'for (unsigned ns = NS_SVG; ns <= NS_MATH; ns++) {' 'for (unsigned ns = NS_SVG; ns <= NS_SVG; ns++) {'
	tree_plant_lit svg-script-end-tag-not-popped html_tree_foreign.c '          ht_stack_pop(tb);\n          return R_DONE;' '          return R_DONE;'
	tree_plant_lit foreign-self-closing-not-popped html_tree_foreign.c '      if (tk->self_closing) {\n        /* "If the token' '      if (0) {\n        /* "If the token'
	tree_plant_lit body-svg-start-does-not-reconstruct html_tree_foreign.c 'int ht_body_start_foreign(GTEXT_HTML_Tree * tb, const Tk * tk, unsigned ns) {\n  HT_CK(ht_fmt_reconstruct(tb));' 'int ht_body_start_foreign(GTEXT_HTML_Tree * tb, const Tk * tk, unsigned ns) {'
	tree_plant_lit mathml-text-integration-points-are-foreign html_tree_foreign.c 'return F_SPECIAL | F_SC_GEN | F_MTIP;' 'return F_SPECIAL | F_SC_GEN;'
	tree_plant_lit svg-integration-points-are-foreign html_tree_foreign.c 'return F_SPECIAL | F_SC_GEN | F_HIP;' 'return F_SPECIAL | F_SC_GEN;'
	tree_plant_lit integration-points-are-not-scope-boundaries html_tree_foreign.c 'return F_SPECIAL | F_SC_GEN | F_HIP;' 'return F_SPECIAL | F_HIP;'
	tree_plant_lit mglyph-is-html-in-a-text-integration-point html_tree_foreign.c '    if (start && !(tk->tok->name_length == 6 && memcmp(tk->tok->name, "mglyph", 6) == 0)' '    if (start && !(0)'
	tree_plant_lit svg-in-annotation-xml-is-foreign html_tree_foreign.c 'if ((flags & F_ANNOT) && start && tk->id == T_SVG) {' 'if (0) {'
	tree_plant_lit characters-in-html-integration-points-are-foreign html_tree_foreign.c 'if ((flags & F_HIP) && (start || character)) {' 'if ((flags & F_HIP) && start) {'
	tree_plant_lit start-tags-in-html-integration-points-are-foreign html_tree_foreign.c 'if ((flags & F_HIP) && (start || character)) {' 'if ((flags & F_HIP) && character) {'
	tree_plant_lit nul-in-foreign-content-dropped html_tree_foreign.c 'if (!ht_insert_text(tb, "\xEF\xBF\xBD", 3)) {' 'if (0) {'
	tree_plant_lit foreign-characters-keep-frameset-ok html_tree_foreign.c '        if (!is_ws_char(p[i])) {' '        if (0) {'
	tree_plant_lit cdata-never-allowed html_tree_foreign.c 'return tb && ht_adjusted_ns(tb) != NS_HTML;' 'return false;'
	tree_plant_lit cdata-always-allowed html_tree_foreign.c 'return tb && ht_adjusted_ns(tb) != NS_HTML;' 'return true;'
	tree_plant_lit adjusted-current-node-ignores-the-fragment-context html_tree_foreign.c '  if (tb->frag && tb->n == 1) {\n    *ns = tb->ctx_ns;' '  if (0) {\n    *ns = tb->ctx_ns;'
	tree_plant_lit fragment-tokenizer-state-never-set html_tree_fragment.c '    if (state != GTEXT_HTML_STATE_DATA) {' '    if (0) {'
	tree_plant_lit fragment-title-is-not-rcdata html_tree_fragment.c 'if (tb->ctx_id == T_TITLE || tb->ctx_id == T_TEXTAREA) {' 'if (tb->ctx_id == T_TEXTAREA) {'
	tree_plant_lit fragment-mode-not-reset-from-the-context html_tree_fragment.c '  tb->mode = ht_tree_reset_mode(tb);\n  /* Step 21. */' '  tb->mode = M_IN_BODY;\n  /* Step 21. */'
	tree_plant_lit fragment-reset-ignores-the-context-element html_tree_modes.c '        id = tb->ctx_id;' '        id = tb->stack[0].id;'
	tree_plant_lit fragment-result-not-moved html_tree_fragment.c '  markup_move_children_unchecked(tb->stack[0].node, tb->frag_node);' '  (void)0;'
	tree_plant_lit fragment-select-context-allows-select html_tree_modes.c '      if (tb->ctx_select) {\n        /* The fragment context element is a select element (fragment case): "Parse error. Ignore\n           the token." */' '      if (0) {\n        /* The fragment context element is a select element (fragment case): "Parse error. Ignore\n           the token." */'
	tree_plant_lit fragment-select-context-allows-input html_tree_modes.c '      if (tb->ctx_select) {\n        /* The fragment context element is a select element (fragment case). */' '      if (0) {\n        /* The fragment context element is a select element (fragment case). */'
	tree_plant_lit fragment-template-context-is-not-template-contents html_tree_modes.c '  return template_on_stack(tb) || tb->ctx_template;' '  return template_on_stack(tb);'
	tree_plant_lit fragment-end-html-after-body-not-ignored html_tree_modes.c '        if (tb->frag) {\n          /* The fragment context element is non-null' '        if (0) {\n          /* The fragment context element is non-null'
	tree_plant_lit fragment-frameset-end-switches-mode html_tree_modes.c 'if (!tb->frag && !is_current(tb, T_FRAMESET)) {' 'if (!is_current(tb, T_FRAMESET)) {'
	tree_plant_lit fragment-integration-point-context-is-foreign html_tree_fragment.c '    if (name_is(c, "foreignObject") || name_is(c, "desc") || name_is(c, "title")) {' '    if (0) {'
	tree_plant_lit fragment-text-integration-point-context-is-foreign html_tree_fragment.c '    if (name_is(c, "mi") || name_is(c, "mo") || name_is(c, "mn") || name_is(c, "ms")\n        || name_is(c, "mtext")) {' '    if (0) {'
	tree_plant_lit fragment-context-namespace-ignored html_tree_fragment.c '  tb->ctx_ns = (uint8_t)c->ns;' '  tb->ctx_ns = NS_HTML;'
}

run_tree_diff() {
	python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_tree_diff.py" "$trree" "$@"
}

only=
case "$mode" in
	--control) only=$1; check_controls ;;
	--vectors) run_vectors "$@" ;;
	--diff) run_diff "$@" ;;
	--tokens) run_tokens "$@" ;;
	--tree) run_tree "$@" ;;
	--controls) check_controls ;;
	all) run_vectors && run_diff "$@" && run_tokens && run_tree ;;
	*) echo "usage: run-html-oracle.sh [--vectors | --diff | --tokens | --tree | --controls | --control NAME]" >&2; exit 2 ;;
esac
