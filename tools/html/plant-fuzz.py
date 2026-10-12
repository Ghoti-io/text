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
"""Plant a defect in a scratch copy of the library and show the HTML tokenizer's and tree builder's fuzzers find it.

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

The tree builder's fuzzer (tests/fuzz/fuzz_html_tree.cpp, target `html-tree`) has fifteen, five of
them replayed from a seed (`tree-text-not-merged`, `tree-atom-leak`, `tree-pending-text-leak`,
`tree-template-modes-leak`, `tree-text-capacity-stale`) and ten searched for from the seeds, with
its own unedited control (`control-tree`):

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
                       "  if (tb->ns_html) {\n    markup_atom_release(tb->doc, tb->ns_html);\n    tb->ns_html = NULL;\n  }\n", ""),
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
REPORT = re.compile(r"^(fuzz_html_(tokenizer|tree): .+|==\d+==ERROR: (AddressSanitizer|LeakSanitizer|libFuzzer).*|"
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
    names = names or ["control", "control-tree"] + list(PLANTS)
    bad = []
    for name in names:
        if not name.startswith("control") and name not in PLANTS:
            raise SystemExit("unknown plant %r (one of control, control-tree, %s)" % (name, ", ".join(PLANTS)))
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
