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
tree_plant() {
	name=$1 file=$2 pattern=$3 replacement=$4 heading=${5:-Divergences with no entry\\|Listed divergences whose count has changed\\|Listed divergences that are gone\\|The invariant checker failed} detail=${6:-.}
	if [ -n "$only" ] && [ "$only" != "tree-$name" ]; then
		return 0
	fi
	rm -rf "$scratch/tplant"
	mkdir -p "$scratch/tplant/src"
	cp -r "$root/src/html" "$root/src/markup" "$scratch/tplant/src/"
	sed -i "s#$pattern#$replacement#g" "$scratch/tplant/src/html/$file"
	if cmp -s "$root/src/html/$file" "$scratch/tplant/src/html/$file"; then
		echo "control tree-$name: the plant did not change $file ('$pattern' is not there)" >&2
		exit 1
	fi
	for f in html_tree.c html_tree_modes.c; do
		cc -std=gnu17 -O1 -fPIC -DGTEXT_BUILD -I"$scratch/tplant/src/html" -I"$root/include" -I"$generated" $cflags \
			-c "$scratch/tplant/src/html/$f" -o "$scratch/tplant/$f.o"
	done
	cc -std=gnu17 -D_GNU_SOURCE -O1 -o "$scratch/trree-$name" "$root/tools/conformance/html_tree_suite.c" \
		"$scratch/tplant/html_tree.c.o" "$scratch/tplant/html_tree_modes.c.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "tree-$name" "$heading" "$detail" \
		python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_tree_diff.py" "$scratch/trree-$name"
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
