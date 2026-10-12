#!/usr/bin/env python3
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
"""Plant a defect in a scratch copy of the library and show the HTML tokenizer's, tree builder's and public API's fuzzers find it.

    tools/html/plant-fuzz.py [--time SECONDS] [name ...]

A fuzzer that has never failed has not been shown to be able to. Each plant here is one edit
to one place in src/html/html_tokenizer.c (or, for the `tree-` plants, src/html/html_tree*.c), applied to a **copy** of this repository in a
scratch directory (the working tree is never touched), and tests/fuzz/fuzz_html_tokenizer.cpp
is run on the copy for the time budget (60 seconds by default, the same `FUZZ_TIME` the
Makefile's fuzz-run-* targets use), or, if the defect has a recorded reproducer
(REPRODUCERS), over that file with -runs=0, which is the same every time. It must stop with
a report. The **control** is the same run with no edit, and it must not.

**Which are replayed and which are searched for.** Six plants (cr-at-a-cut, offset-at-a-cut,
stream-errors-twice, names-leak, two-byte-utf8, attribute-table) are replayed from a recorded
input, so the check is the same every time; that shows the harness *can* see the defect, not
that the search reaches it. Two (whitespace-flag, upper-case-tag) have a recorded input too
(in the corpus, replayed by `make test`) but are deliberately **searched for** from the
seeds, the way a real find would be: if the search stops finding them quickly the check
fails. The control is searched as well.

  cr-at-a-cut      a carriage return at the end of a piece is not held back for its line
                   feed: the line feed that follows is a second newline when the input is cut
                   there and not when it is whole. The answer depends on the cutting.
  offset-at-a-cut  the byte offset of the input dropped when the unread tail is moved is
                   forgotten: every offset after a cut is short by what was dropped.
  stream-errors-twice  the high-water mark that keeps the input stream's errors to one per
                   character is not kept: a character read again after a look-ahead that had
                   to wait reports its error again, and only where the input was cut.
  names-leak       the duplicate-attribute table is not freed with the tokenizer. The
                   fuzzer's counting allocator (and LeakSanitizer) see the block.
  whitespace-flag  a character token's all-white-space flag is never cleared: a run with a
                   letter in it says it is white space.
  upper-case-tag   a tag name is no longer lower-cased: a token property.
  two-byte-utf8    the trailing byte of the two-byte form of a code point is written without its
                   marker bits: a string in a token is not UTF-8.
  attribute-table  the duplicate-attribute table puts every name in one place, and a tag of
                   a few hundred attributes costs the square of that: the fuzzer's work
                   bound, fed a byte at a time. The seed `many-attributes` is a tag of 900.

The tree builder's fuzzer (tests/fuzz/fuzz_html_tree.cpp, target `html-tree`) has twenty-one, eight of
them replayed from a seed (`tree-text-not-merged`, `tree-atom-leak`, `tree-pending-text-leak`,
`tree-template-modes-leak`, `tree-text-capacity-stale`, `tree-foreign-atoms-leak`,
`tree-svg-attribute-case-not-restored`, `tree-foreign-nul-kept`) and thirteen searched for from the
seeds, with its own unedited control (`control-tree`):

  tree-text-not-merged   adjacent runs of text are not appended to one node: the fuzzer's
                   validity property (no two adjacent text nodes).
  tree-atom-leak   the namespace name the builder interned is not released when the parse ends:
                   the invariant checker's reference counts.
  tree-dyn-table-leak  the table of element names the vocabulary does not know is not freed with
                   the builder: the counting allocator and LeakSanitizer.
  tree-fmt-blocks-leak the formatting list's entries are not freed: the same.
  tree-cut-dependence  a feed of one byte clears the frameset-ok flag: the answer depends on how
                   the input was cut (a frameset after content, fed a byte at a time).
  tree-absorbed-oom  an allocation failure while a run of text is appended to a text node is
                   ignored: the tree is silently short, which the allocation-failure sweep sees
                   as an answer that is not the normal one.

and, for stage 3d, nine that break a property of the shapes the table and template modes build
(the fuzzer cannot say a tree is the *right* one; the vectors and the differential do that):

  tree-table-text-in-table   text that is not white space is left in a table instead of being
                   foster parented: the property that the text directly in a table part is
                   white space.                                   (in table text)
  tree-template-children   the appropriate place for inserting a node never uses a template's
                   content: the template element gets children of its own.        (template)
  tree-pending-text-leak   the pending table text is not freed with the builder: the counting
                   allocator and LeakSanitizer.  Replayed from `table-text-pending`.
  tree-template-modes-leak   the stack of template insertion modes is not freed: the same,
                   replayed from `nested-templates`.
  tree-text-capacity-stale   a new text node is recorded with a capacity it does not have, so
                   the next piece of text is written past the end: AddressSanitizer.  Replayed
                   from `text-in-pieces`.                      (foster parenting's two nodes)
  tree-no-implied-tr   a td in a table body is not given a tr: a td that is not in a row.
                   (in table body)
  tree-no-implied-tbody   a tr in a table is not given a tbody.                   (in table)
  tree-col-not-popped   a col in a column group stays open and takes what follows as children:
                   a col with children.                                  (in column group)
  tree-foster-into-the-wrong-template   the template that foster parenting finds is read from
                   the wrong place in the stack: AddressSanitizer.           (foster parenting)

and, for stage 3e, six (three replayed, three searched):

  tree-foreign-atoms-leak   the namespaces and prefixes interned for foreign content are not released
                   when the parse ends: the invariant checker's reference counts.  Replayed from
                   `foreign-attributes`.
  tree-svg-case-not-restored   the SVG element names the standard restores are left lower case: the
                   property that a foreign element has a name an HTML parser can have made (searched).
  tree-svg-attribute-case-not-restored   the same for attributes.  Replayed from `svg-attribute-table`.
  tree-foreign-nul-kept   a NUL in foreign content is inserted as it is and not as U+FFFD: the property
                   that no text holds a NUL.  Replayed from `nul-in-foreign`.
  tree-foreign-attribute-oom-absorbed   an allocation failure while an attribute of a foreign element is
                   added is ignored: the tree silently lacks the attribute, which the allocation-failure
                   sweep sees (searched). (Ignoring the result of an insert that records its failure in the
                   builder's status is no defect: the failure is sticky anyway.)
  tree-fragment-result-not-moved   the root element's children are not moved into the fragment: the
                   property that a fragment's document has an empty root element (searched).

The public API's fuzzer (tests/fuzz/fuzz_html.cpp, target `html`) has twelve, all replayed from a seed of
tests/fuzz/corpus/html and all in the layer over the builder (src/html/html_api.c, html_core.c), with its
own unedited control (`control-api`):

  api-bom-needs-one-feed   the byte order mark is recognized only in a feed of three bytes or more: a
                   document fed a byte at a time reads the mark as text, which the property that
                   the answer is the same however the input is cut sees.
  api-held-bytes-dropped   the bytes of a mark that turned out not to be one are never given to the
                   builder: the property that the answer is the builder's own.
  api-result-leak   the result's error list is not freed by gtext_html_result_free(): the counting
                   allocator, and LeakSanitizer.
  api-encoding-not-recorded   the document does not record its encoding: the property that it
                   records the one the result gives.
  api-count-only-ignored   the count-only option is dropped: count-only keeps no error.
  api-error-total-short   the dropped count is zero: kept plus dropped is every error.
  api-second-finish-gives-document   a second finish is not refused as a state error.
  api-input-limit-off-by-one   an input exactly at its limit is refused: the boundary is exact.
  api-fragment-bom-skipped   a fragment skips a byte order mark: the characters of a fragment are
                   already decoded and U+FEFF is text.
  api-sticky-feed-checks-arguments-first   a failed parser answers a NULL buffer with a length with the
                   argument error and not its failure.
  api-sticky-finish-checks-kind-first   a failed parser answers the wrong kind of finish with the state
                   error and not its failure.
  api-oom-absorbed-in-report   the result is returned incomplete when its error list cannot be
                   allocated: the allocation-failure sweep's property that a success is the normal answer.

The serializer's fuzzer (tests/fuzz/fuzz_html_serialize.cpp, target `html-serialize`) has eleven, eight replayed from a
seed of tests/fuzz/corpus/html-serialize and three searched for from the seeds (`serialize-nodes-not-counted`,
`serialize-solidus-space-not-checked`, `serialize-output-limit-off-by-one`), with its own unedited control
(`control-serialize`):

  serialize-nbsp-not-escaped   a no-break space in text is written as it is: the model, which writes `&nbsp;`.
  serialize-stream-drops-a-character   the token stream ignores a text of one character: the property that it
                   writes what the walk writes.
  serialize-sink-asked-again   a sink that refused is given the rest of the buffer when the walk stops: the sink is not
                   called again after a refusal.
  serialize-template-children-written   a template's own children are written in place of its contents: the model.
  serialize-xlink-prefix-lost   an attribute in the XLink namespace is written without `xlink:`: the model.
  serialize-raw-text-escaped   the text of a script is escaped like other text: the model.
  serialize-stack-leak   the stack of open elements is not freed with the serializer: the counting allocator and
                   LeakSanitizer.
  serialize-oom-absorbed-in-growth   a failed growth of an array is not recorded: a success that is not the normal answer.
  serialize-nodes-not-counted   the result's count of nodes visited stays zero: the counts.
  serialize-solidus-space-not-checked   `space_before_trailing_solidus` without `use_trailing_solidus` is accepted: an unusable combination
                   of options is refused.
  serialize-output-limit-off-by-one   an output exactly at the limit is refused: the limit is exact.

The scratch copy carries `src/`, `include/`, `tests/`, `tools/` and the Makefile, and is
built there; it has none of this tree's build, and only the tracked seeds of the libFuzzer
corpus.

Copyright 2026 by Corey Pennycuff
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WORKSPACE = os.path.dirname(os.path.dirname(ROOT))

PLANTS = {
    "tree-text-not-merged": ("html-tree", "src/html/html_tree.c",
                             "  if (last && last->kind == GTEXT_MARKUP_KIND_TEXT) {", "  if (0) {"),
    "tree-atom-leak": ("html-tree", "src/html/html_tree.c",
                       "  markup_atom ** const others[] = {&tb->ns_html, &tb->ns_svg,",
                       "  markup_atom ** const others[] = {&tb->ns_svg,"),
    "tree-dyn-table-leak": ("html-tree", "src/html/html_tree.c",
                            "  gtext_allocator_free(&alloc, tb->dyn_tab);\n", ""),
    "tree-fmt-blocks-leak": ("html-tree", "src/html/html_tree.c",
                             "    gtext_allocator_free(&alloc, tb->fmt_blocks[i]);\n", ""),
    "tree-cut-dependence": ("html-tree", "src/html/html_tree.c",
                            "  const GTEXT_HTML_TokenizerStatus s = gtext_html_tokenizer_feed(tb->tok, data, length);\n",
                            "  const GTEXT_HTML_TokenizerStatus s = gtext_html_tokenizer_feed(tb->tok, data, length);\n"
                            "  if (length == 1) {\n    tb->frameset_ok = false;\n  }\n"),
    "tree-absorbed-oom": ("html-tree", "src/html/html_tree.c",
                          "    return ht_tree_markup_status(\n        tb, markup_text_append(tb->doc, last, &cap, data, length));\n",
                          "    (void)markup_text_append(tb->doc, last, &cap, data, length);\n    return true;\n"),
    # Stage 3d. Each breaks a property the fuzzer holds the tree to (a valid shape, no leak, no heap
    # overflow); that the table and template modes build the *right* tree is the vectors' and the
    # differential's, which a property cannot say.
    "tree-table-text-in-table": ("html-tree", "src/html/html_tree_modes.c",
                                 "      if (!tb->pend_foster && ws_prefix(p, len) != len) {",
                                 "      if (!tb->pend_foster && 0) {"),
    "tree-template-children": ("html-tree", "src/html/html_tree.c",
                               "  if (tmpl) {\n    target = ((markup_element *)target)->content;\n  }\n", ""),
    "tree-pending-text-leak": ("html-tree", "src/html/html_tree.c",
                               "  gtext_allocator_free(&alloc, tb->pend);\n", ""),
    "tree-template-modes-leak": ("html-tree", "src/html/html_tree.c",
                                 "  gtext_allocator_free(&alloc, tb->tmpl);\n", ""),
    "tree-text-capacity-stale": ("html-tree", "src/markup/markup_tree.c",
                                 "    node->cap_log2 = (uint8_t)log2;\n    *capacity = cap;\n",
                                 "    node->cap_log2 = (uint8_t)(log2 + 3);\n    *capacity = cap;\n"),
    "tree-no-implied-tr": ("html-tree", "src/html/html_tree_modes.c",
                           "        HT_CK(ht_insert_element_bare(tb, T_TR, NULL));\n", "        (void)0;\n"),
    "tree-no-implied-tbody": ("html-tree", "src/html/html_tree_modes.c",
                              "          HT_CK(ht_insert_element_bare(tb, T_TBODY, NULL));\n", "          (void)0;\n"),
    "tree-col-not-popped": ("html-tree", "src/html/html_tree_modes.c",
                            "        HT_CK(ht_insert_element(tb, tk, NULL));\n        HT_CK(ht_pop_current_acknowledge(tb, tk));\n"
                            "        return R_DONE;\n      }\n      if (tk->id == T_TEMPLATE) {",
                            "        HT_CK(ht_insert_element(tb, tk, NULL));\n"
                            "        return R_DONE;\n      }\n      if (tk->id == T_TEMPLATE) {"),
    "tree-foster-into-the-wrong-template": ("html-tree", "src/html/html_tree.c",
                                            "      target = tb->stack[lt].node;\n      tmpl = true;",
                                            "      target = tb->stack[lt + tb->cap].node;\n      tmpl = true;"),
    # Stage 3e: foreign content and fragments. The properties are a valid shape (a foreign element named
    # as the standard's table names it, attributes in the three namespaces of the adjustment table, no NUL
    # in text, a fragment's root element empty), the reference counts of the invariant checker, the
    # allocation-failure sweep and the work bound; that the foreign rules build the *right* tree is the
    # vectors' and the differential's.
    "tree-foreign-atoms-leak": ("html-tree", "src/html/html_tree.c",
                                "  markup_atom ** const others[] = {&tb->ns_html, &tb->ns_svg, &tb->ns_math, &tb->ns_xlink,\n"
                                "      &tb->ns_xml, &tb->ns_xmlns, &tb->pfx_xlink, &tb->pfx_xml, &tb->pfx_xmlns};",
                                "  markup_atom ** const others[] = {&tb->ns_html};"),
    "tree-svg-case-not-restored": ("html-tree", "src/html/html_tree.c",
                                   "    s->disp = ns == NS_SVG ? ht_svg_tag_adjusted(bytes, len) : NULL;",
                                   "    s->disp = NULL;"),
    "tree-svg-attribute-case-not-restored": ("html-tree", "src/html/html_tree.c",
                                             "      adjusted = ht_svg_attribute_adjusted(name, len);",
                                             "      adjusted = NULL;"),
    "tree-foreign-nul-kept": ("html-tree", "src/html/html_tree_foreign.c",
                              "    if (!ht_insert_text(tb, \"\\xEF\\xBF\\xBD\", 3)) {",
                              "    if (!ht_insert_text(tb, \"\\0\", 1)) {"),
    "tree-foreign-attribute-oom-absorbed": ("html-tree", "src/html/html_tree.c",
                                            "    if (!ht_tree_markup_status(tb,\n            markup_element_append_attribute(tb->doc, el, NULL, NULL, name, len,\n"
                                            "                attrs[i].value, attrs[i].value_length))) {\n      return false;\n    }\n",
                                            "    (void)markup_element_append_attribute(tb->doc, el, NULL, NULL, name, len,\n"
                                            "        attrs[i].value, attrs[i].value_length);\n"),
    "tree-fragment-result-not-moved": ("html-tree", "src/html/html_tree_fragment.c",
                                       "  markup_move_children_unchecked(tb->stack[0].node, tb->frag_node);\n",
                                       ""),

    # The public API (tests/fuzz/fuzz_html.cpp). Each is in the layer over the builder.
    "api-bom-needs-one-feed": ("html", "src/html/html_api.c",
                               "  while (p->bom_open && len > 0) {", "  while (p->bom_open && len >= 3) {"),
    "api-held-bytes-dropped": ("html", "src/html/html_api.c",
                               "  const size_t n = p->held;\n  p->held = 0;", "  const size_t n = 0;\n  p->held = 0;"),
    "api-result-leak": ("html", "src/html/html_core.c",
                        "    gtext_allocator_free(&allocator, internal);\n", "    (void)allocator;\n"),
    "api-encoding-not-recorded": ("html", "src/html/html_api.c",
                                  "    if (gtext_markup_document_set_encoding(doc, \"UTF-8\",\n"
                                  "            p->has_bom ? GTEXT_MARKUP_ENCODING_BOM : p->source)\n        != GTEXT_MARKUP_OK) {",
                                  "    if (0) {"),
    "api-count-only-ignored": ("html", "src/html/html_api.c",
                               "  to.count_errors_only = o.count_errors_only || !errors_wanted;",
                               "  to.count_errors_only = !errors_wanted;"),
    "api-error-total-short": ("html", "src/html/html_core.c",
                              "  internal->dropped = dropped;", "  internal->dropped = 0;"),
    "api-second-finish-gives-document": ("html", "src/html/html_api.c",
                                         "  if (p->fragment != fragment || p->taken) {",
                                         "  if (p->fragment != fragment) {"),
    "api-input-limit-off-by-one": ("html", "src/html/html_api.c",
                                   "  if (p->fed > p->max_input || len > p->max_input - p->fed) {",
                                   "  if (p->fed > p->max_input || len >= p->max_input - p->fed) {"),
    "api-sticky-feed-checks-arguments-first": ("html", "src/html/html_api.c",
                                               "  if (p->status != GTEXT_HTML_OK) {\n    return p->status;\n  }\n  if (!data && len != 0) {\n    return GTEXT_HTML_E_INVALID;\n  }",
                                               "  if (!data && len != 0) {\n    return GTEXT_HTML_E_INVALID;\n  }\n  if (p->status != GTEXT_HTML_OK) {\n    return p->status;\n  }"),
    "api-sticky-finish-checks-kind-first": ("html", "src/html/html_api.c",
                                            "  if (p->status != GTEXT_HTML_OK) {\n    /* The sticky failure comes before",
                                            "  if (p->fragment != fragment) {\n    html_result_set_failure(result, GTEXT_HTML_E_STATE, NULL);\n    return GTEXT_HTML_E_STATE;\n  }\n  if (p->status != GTEXT_HTML_OK) {\n    /* The sticky failure comes before"),
    "api-fragment-bom-skipped": ("html", "src/html/html_api.c",
                                 "  p->bom_open = context == NULL;", "  p->bom_open = true;"),
    "api-oom-absorbed-in-report": ("html", "src/html/html_api.c",
                                   "    if (!result->internal) {", "    if (0) {"),
    # The serializer (tests/fuzz/fuzz_html_serialize.cpp). Each breaks a property it holds the serializer to - the model, the
    # token stream against the walk, the counts, the limits, the sink, the allocator.
    "serialize-nbsp-not-escaped": ("html-serialize", "src/html/html_serialize.c",
                                   '          rep = "&nbsp;";', '          rep = "\\xC2\\xA0";'),
    "serialize-stream-drops-a-character": ("html-serialize", "src/html/html_serialize.c",
                                           "  if (length == 0) {\n    return s->status;\n  }\n  if (fe_queueing(s)) {\n    if (q_byte(s, Q_CHARS)) {",
                                           "  if (length <= 1) {\n    return s->status;\n  }\n  if (fe_queueing(s)) {\n    if (q_byte(s, Q_CHARS)) {"),
    "serialize-sink-asked-again": ("html-serialize", "src/html/html_serialize.c",
                                   "  if (s->used == 0 || s->sink_failed) {", "  if (s->used == 0) {"),
    "serialize-template-children-written": ("html-serialize", "src/html/html_serialize.c",
                                            "    cursor = gtext_markup_node_first_child(content ? content : n);",
                                            "    cursor = gtext_markup_node_first_child(n);"),
    "serialize-xlink-prefix-lost": ("html-serialize", "src/html/html_serialize.c",
                                    '      lead = "xlink:";', "      lead = NULL;"),
    "serialize-raw-text-escaped": ("html-serialize", "src/html/html_serialize.c",
                                   "  return (flags & EF_RAW) || ((flags & EF_RAW_SCRIPTING) && s->o.scripting);",
                                   "  return false;"),
    "serialize-stack-leak": ("html-serialize", "src/html/html_serialize.c",
                             "  gtext_allocator_free(&alloc, s->stack);\n", ""),
    "serialize-oom-absorbed-in-growth": ("html-serialize", "src/html/html_serialize.c",
                                         "  if (!grown) {\n    oom(s);\n    return false;\n  }\n  *array = grown;",
                                         "  if (!grown) {\n    return false;\n  }\n  *array = grown;"),
    "serialize-nodes-not-counted": ("html-serialize", "src/html/html_serialize.c",
                                    "    s->r.nodes_visited++;\n", ""),
    "serialize-solidus-space-not-checked": ("html-serialize", "src/html/html_serialize.c",
                                            "  if (o->space_before_trailing_solidus && !o->use_trailing_solidus) {\n    return GTEXT_HTML_E_OPTIONS;\n  }\n",
                                            ""),
    "serialize-output-limit-off-by-one": ("html-serialize", "src/html/html_serialize.c",
                                          "  if (n > s->o.max_output_bytes - s->total) {", "  if (n >= s->o.max_output_bytes - s->total) {"),
    "cr-at-a-cut": ("html-tokenizer", "src/html/html_tokenizer.c",
                    "      if (p + 1 >= len) {\n        if (!t->final) {\n          return false;\n        }\n        *cp = '\\n';",
                    "      if (p + 1 >= len) {\n        *cp = '\\n';"),
    "offset-at-a-cut": ("html-tokenizer", "src/html/html_tokenizer.c",
                        "  t->pend_length = rest;\n  t->base_offset += t->pos;\n  t->in = t->pend;",
                        "  t->pend_length = rest;\n  t->in = t->pend;"),
    "stream-errors-twice": ("html-tokenizer", "src/html/html_tokenizer.c",
                            "    t->err_high = off + n;", "    t->err_high = 0;"),
    "names-leak": ("html-tokenizer", "src/html/html_tokenizer.c",
                   "  gtext_allocator_free(&a, t->names);\n", ""),
    "whitespace-flag": ("html-tokenizer", "src/html/html_tokenizer.c",
                        "  if (!whitespace) {\n    t->text_whitespace = false;\n  }\n", ""),
    "upper-case-tag": ("html-tokenizer", "src/html/html_tokenizer.c",
                       "        else if (is_upper(c)) {\n          ar_cp(t, (uint32_t)c + 0x20);\n        }\n        else if (c == 0) {\n          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);\n          ar_cp(t, 0xFFFD);\n        }\n        else if (c == CP_EOF) {\n          perr(t, GTEXT_HTML_ERR_EOF_IN_TAG);\n          t->eof_ready = true;\n          return RUN_TOKEN;\n        }\n        else {\n          ar_cp(t, (uint32_t)c);\n        }\n        break;\n\n      /* 13.2.5.9",
                       "        else if (c == 0) {\n          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);\n          ar_cp(t, 0xFFFD);\n        }\n        else if (c == CP_EOF) {\n          perr(t, GTEXT_HTML_ERR_EOF_IN_TAG);\n          t->eof_ready = true;\n          return RUN_TOKEN;\n        }\n        else {\n          ar_cp(t, (uint32_t)c);\n        }\n        break;\n\n      /* 13.2.5.9"),
    "two-byte-utf8": ("html-tokenizer", "src/html/html_tokenizer.c",
                      "    out[1] = (char)(0x80 | (cp & 0x3F));\n    return 2;", "    out[1] = (char)(0x00 | (cp & 0x3F));\n    return 2;"),
    "attribute-table": ("html-tokenizer", "src/html/html_tokenizer.c",
                        "  return (size_t)hash_name(t->hash_seed, s, n);", "  return 0;"),
}


def copy_tree(dest):
    skip = shutil.ignore_patterns("build", "third_party", "crash-*", "leak-*", "timeout-*", "slow-unit-*",
                                  "__pycache__", "docs", ".git")
    corpus = os.path.join(ROOT, "tests", "fuzz", "corpus")

    def ignore(directory, names):
        out = set(skip(directory, names))
        # The corpus: tracked seeds only, so the copy starts where a clean checkout does
        # (and does not race a fuzzer that is writing into the same directory).
        if os.path.dirname(directory) == corpus:
            out |= {n for n in names if not n.endswith(".seed")}
        return out

    shutil.copytree(ROOT, dest, ignore=ignore, symlinks=True, ignore_dangling_symlinks=True)


# A plant whose defect has a recorded reproducer is found by replaying that file, which is the
# same every time, and not by a time-boxed search, which is not. The others keep the search.
REPRODUCERS = {
    "cr-at-a-cut": "tests/fuzz/corpus/html-tokenizer/reproducer-carriage-return-at-a-cut.seed",
    "offset-at-a-cut": "tests/fuzz/corpus/html-tokenizer/reproducer-offset-after-a-cut.seed",
    "stream-errors-twice": "tests/fuzz/corpus/html-tokenizer/reproducer-stream-error-after-a-look-ahead.seed",
    "names-leak": "tests/fuzz/corpus/html-tokenizer/reproducer-attribute-table-leak.seed",
    "attribute-table": "tests/fuzz/corpus/html-tokenizer/many-attributes.seed",
    "two-byte-utf8": "tests/fuzz/corpus/html-tokenizer/reproducer-two-byte-utf8.seed",
    "tree-text-not-merged": "tests/fuzz/corpus/html-tree/text-in-pieces.seed",
    "tree-atom-leak": "tests/fuzz/corpus/html-tree/deep-div.seed",
    "tree-pending-text-leak": "tests/fuzz/corpus/html-tree/table-text-pending.seed",
    "tree-template-modes-leak": "tests/fuzz/corpus/html-tree/nested-templates.seed",
    "tree-text-capacity-stale": "tests/fuzz/corpus/html-tree/text-in-pieces.seed",
    "tree-foreign-atoms-leak": "tests/fuzz/corpus/html-tree/foreign-attributes.seed",
    "tree-svg-attribute-case-not-restored": "tests/fuzz/corpus/html-tree/svg-attribute-table.seed",
    "tree-foreign-nul-kept": "tests/fuzz/corpus/html-tree/nul-in-foreign.seed",
    "serialize-nbsp-not-escaped": "tests/fuzz/corpus/html-serialize/ser-nbsp.seed",
    "serialize-stream-drops-a-character": "tests/fuzz/corpus/html-serialize/ser-one-character-of-text.seed",
    "serialize-sink-asked-again": "tests/fuzz/corpus/html-serialize/ser-ampersands-sink-fails-midway.seed",
    "serialize-template-children-written": "tests/fuzz/corpus/html-serialize/ser-template.seed",
    "serialize-xlink-prefix-lost": "tests/fuzz/corpus/html-serialize/ser-svg-xlink.seed",
    "serialize-raw-text-escaped": "tests/fuzz/corpus/html-serialize/ser-raw-text.seed",
    "serialize-stack-leak": "tests/fuzz/corpus/html-serialize/ser-alloc-sweep-all-forms.seed",
    "serialize-oom-absorbed-in-growth": "tests/fuzz/corpus/html-serialize/ser-alloc-sweep-all-forms.seed",
    "api-bom-needs-one-feed": "tests/fuzz/corpus/html/api-bom-cut-byte-at-a-time.seed",
    "api-held-bytes-dropped": "tests/fuzz/corpus/html/api-bom-prefix-then-text.seed",
    "api-result-leak": "tests/fuzz/corpus/html/api-rich-document.seed",
    "api-encoding-not-recorded": "tests/fuzz/corpus/html/api-rich-document.seed",
    "api-count-only-ignored": "tests/fuzz/corpus/html/api-errors-count-only.seed",
    "api-error-total-short": "tests/fuzz/corpus/html/api-errors-cap-one.seed",
    "api-second-finish-gives-document": "tests/fuzz/corpus/html/api-rich-document.seed",
    "api-input-limit-off-by-one": "tests/fuzz/corpus/html/api-rich-document.seed",
    "api-fragment-bom-skipped": "tests/fuzz/corpus/html/api-bom-fragment-added.seed",
    "api-oom-absorbed-in-report": "tests/fuzz/corpus/html/api-rich-document.seed",
    "api-sticky-feed-checks-arguments-first": "tests/fuzz/corpus/html/api-sticky-deep.seed",
    "api-sticky-finish-checks-kind-first": "tests/fuzz/corpus/html/api-sticky-deep-fragment.seed",
}


def make_variables(prefix, names):
    """The Makefile's own values for `names`, so that nothing here is a second copy of them."""
    rules = "print-%: ; @echo $($*)\n"
    out = {}
    for name in names:
        done = subprocess.run(["make", "-s", "-C", ROOT, "-f", "Makefile", "-f", "-", "PREFIX=" + prefix,
                               "print-" + name], input=rules, capture_output=True, text=True)
        out[name] = done.stdout.strip()
        if done.returncode != 0 or not out[name]:
            raise SystemExit("could not read %s from the Makefile: %s" % (name, done.stderr.strip()[-300:]))
    return out


TIMEOUT = 3  # a multiple of the fuzz budget; a build and a run each fit well inside it
TIMED_OUT = -999


def target_of(name):
    if name == "control":
        return "html-tokenizer"
    if name == "control-tree":
        return "html-tree"
    if name == "control-api":
        return "html"
    if name == "control-serialize":
        return "html-serialize"
    return PLANTS[name][0]


def run(name, seconds):
    """(exit status, output) of one plant or of the control; the status is TIMED_OUT if it hung."""
    target = target_of(name)
    scratch = tempfile.mkdtemp(prefix="plant-fuzz-", dir=os.environ.get("TMPDIR", tempfile.gettempdir()))
    budget = seconds * TIMEOUT + 900
    try:
        copy_tree(os.path.join(scratch, "text"))
        text = os.path.join(scratch, "text")
        if not name.startswith("control"):
            _target, path, old, new = PLANTS[name]
            with open(os.path.join(text, path), encoding="utf-8") as fh:
                source = fh.read()
            if source.count(old) != 1:
                raise SystemExit("plant %s: its anchor matches %d times in %s" % (name, source.count(old), path))
            with open(os.path.join(text, path), "w", encoding="utf-8") as fh:
                fh.write(source.replace(old, new))
        prefix = os.environ.get("PREFIX") or os.path.join(WORKSPACE, ".local")
        env = dict(os.environ)
        env["PKG_CONFIG_PATH"] = os.path.join(prefix, "share", "pkgconfig")
        cap = os.path.join(WORKSPACE, "tools", "memcap")
        capped = [cap, "-m", os.environ.get("GHOTI_MEMCAP", "6G"), "--"] if os.path.exists(cap) else []
        try:
            if name in REPRODUCERS:
                built = subprocess.run(capped + ["make", "-C", text, "-j4", "fuzz-" + target, "PREFIX=" + prefix],
                                       env=env, capture_output=True, text=True, timeout=budget)
                if built.returncode != 0:
                    return built.returncode, built.stdout + built.stderr
                app = os.path.join(text, make_variables(prefix, ("FUZZ_APP_DIR",))["FUZZ_APP_DIR"], "fuzz_" + target.replace("-", "_"))
                done = subprocess.run(capped + [app, "-runs=0", os.path.join(text, REPRODUCERS[name])], env=env,
                                      capture_output=True, text=True, timeout=budget)
                return done.returncode, done.stdout + done.stderr
            cmd = capped + ["make", "-C", text, "-j4", "fuzz-run-" + target, "FUZZ_TIME=%d" % seconds,
                            "FUZZ_ARGS=-seed=1 -max_len=2048", "PREFIX=" + prefix]
            done = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=budget)
            return done.returncode, done.stdout + done.stderr
        except subprocess.TimeoutExpired as hung:
            return TIMED_OUT, "timed out after %ds\n%s" % (budget, (hung.stdout or b"")[-500:])
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


# What a fuzzer reporting a defect looks like: a sanitizer or libFuzzer banner, or one of the
# harness's own "property failed" lines. The build's command lines mention the harness names,
# and a compiler error, a kill by memcap (exit 137) or a timeout all exit non-zero too, so none
# of those counts: the run must have started (libFuzzer printed its seed) and then reported.
REPORT = re.compile(r"^(fuzz_html(_tokenizer|_tree|_serialize)?: .+|==\d+==ERROR: (AddressSanitizer|LeakSanitizer|libFuzzer).*|"
                    r".*runtime error: .*|SUMMARY: (AddressSanitizer|libFuzzer|UndefinedBehaviorSanitizer).*)$", re.M)


def found_by_fuzzer(status, out):
    if status in (0, TIMED_OUT, 137, -9) or "INFO: Seed:" not in out:
        return False
    return REPORT.search(out) is not None


def main(argv):
    seconds = 60
    names = []
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--time":
            seconds = int(args.pop(0))
        else:
            names.append(a)
    names = names or ["control", "control-tree", "control-api", "control-serialize"] + list(PLANTS)
    bad = []
    for name in names:
        if not name.startswith("control") and name not in PLANTS:
            raise SystemExit("unknown plant %r (one of control, control-tree, control-api, control-serialize, %s)" % (name, ", ".join(PLANTS)))
        status, out = run(name, seconds)
        reported = found_by_fuzzer(status, out)
        if name.startswith("control"):
            ok = status == 0
            print("%s (no edit): %s" % (name, "silent, as it must be" if ok else "FAILED (exit %d)" % status))
            if not ok:
                sys.stderr.write(out[-3000:])
        else:
            ok = reported
            line = next((l for l in out.splitlines() if REPORT.match(l)), "")
            print("%-20s %s %s" % (name, "found" if ok else "NOT FOUND in %ds" % seconds, line[:140]))
            if not ok:
                sys.stderr.write("exit %s; the end of the output:\n%s\n" % (status, out[-1500:]))
        if not ok:
            bad.append(name)
    if bad:
        sys.stderr.write("planted-defect check failed for: %s\n" % ", ".join(bad))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
