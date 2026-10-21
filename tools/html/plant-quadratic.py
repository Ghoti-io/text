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
"""Plant a quadratic in one layer of the library and show tests/test-quadratic.cpp fails on the shape it should.

    PREFIX=... DEP_PCS=... ARCHIVE=... tools/html/plant-quadratic.py [name ...]

A ratio gate that has never failed has not been shown to be able to. Each plant below is one edit to one
file of the library, applied to a **copy** of src/ in a scratch directory (the working tree is never
touched), compiled alone and linked ahead of the library's archive into tests/test-quadratic.cpp, which is
then run. It must fail, and the failures it reports must include a shape the plant is meant to slow (named
in the plant), so that a plant that fails the program for another reason does not count. The **control** is
the same procedure with no edit, and the program must pass.

The edits make the layer's work counter grow with the depth or the position the way a real defect would
(a walk of the stack, a rescan of the input read, a lookup that lands in one bucket); the stack walk of
"reset the insertion mode" that this suite found is the first plant, in the form it had, counted. The `css-`
plants do the same to CSS Syntax: the declaration attempt of a block's contents reading the blocks it should
step over, or reading on to the end before it gives up (the two ways a literal reading of 5.5.5 is quadratic),
a tokenizer that looks back, and identities and a serializer that walk their stacks. The XML parser's walk of
its earlier DTD bindings is another plant.

Copyright 2026 by Corey Pennycuff
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# (name, file under src/, old text (exactly once), new text, [substrings of shape names that must be reported failing])
PLANTS = [
    ("tree-reset-mode-walks-the-stack", "html/html_tree_modes.c",
     "  bool last = false;\n  uint32_t id;\n  if (best >= 1) {",
     "  for (size_t w = tb->n; w > best + 1; w--) {\n    tb->work++; /* the walk down the stack that the name indexes replaced */\n  }\n  bool last = false;\n  uint32_t id;\n  if (best >= 1) {",
     ["reset the insertion mode", "reconstruction: after a table"]),
    ("tree-scope-check-walks-the-stack", "html/html_tree.c",
     "  const uint32_t j = tb->names[id].topidx;\n  if (j == NONE_IDX) {\n    return false;\n  }\n  const stack_entry * top = &tb->stack[tb->n - 1];",
     "  const uint32_t j = tb->names[id].topidx;\n  ((GTEXT_HTML_Tree *)tb)->work += tb->n; /* a scope check that looks at every open element */\n  if (j == NONE_IDX) {\n    return false;\n  }\n  const stack_entry * top = &tb->stack[tb->n - 1];",
     ["p in a deep stack", "li in a deep stack", "div"]),
    ("tree-adoption-rederives-the-whole-stack", "html/html_tree.c",
     "static void derive_range(GTEXT_HTML_Tree * tb, size_t from) {\n  for (size_t i = from; i < tb->n; i++) {",
     "static void derive_range(GTEXT_HTML_Tree * tb, size_t from) {\n  tb->work += from; /* the indexes below `from` are made again too */\n  for (size_t i = from; i < tb->n; i++) {",
     ["adoption"]),
    ("tokenizer-attribute-names-share-a-bucket", "html/html_tokenizer.c",
     "  size_t slot = name_hash(t, t->ar.data + start, len) & (t->names_capacity - 1);\n  while (t->names[slot]) {",
     "  size_t slot = 0; /* every name in one place */\n  while (t->names[slot]) {",
     ["one tag, many attributes"]),
    ("serializer-looks-up-the-ancestors", "html/html_serialize.c",
     "    s->r.nodes_visited++;\n    s->work++;\n",
     "    s->r.nodes_visited++;\n    s->work += 1 + nframes; /* each node finds its ancestors by walking them */\n",
     ["a tree 16n deep", "every option on, deep"]),
    ("decoder-reads-the-stream-again", "encoding/encoding_decode.c",
     "    call->steps++;\n    const GTEXT_Dec_Result r = codec->step(&dec->state, byte, &step);",
     "    call->steps += 1 + (i >> 6); /* a step that looks back over what was read */\n    const GTEXT_Dec_Result r = codec->step(&dec->state, byte, &step);",
     ["decode"]),
    ("css-speculation-reads-the-block", "css/css_parser.c",
     "      const size_t m = s->match[j];\n      j = m >= s->n ? s->n : m + 1;",
     "      const size_t m = s->match[j];\n      p->ctx->work += (m >= s->n ? s->n : m) - j; /* the attempt reads the block instead of stepping over it */\n      j = m >= s->n ? s->n : m + 1;",
     ["CSS a block then more"]),
    ("css-failed-declaration-reads-the-rest", "css/css_parser.c",
     "  (void)qualified_start(p, s, GTEXT_CSS_TOKEN_SEMICOLON, true);\n  return true;\n}",
     "  p->ctx->work += s->n - mark; /* the attempt read on to the end before it was given up */\n  (void)qualified_start(p, s, GTEXT_CSS_TOKEN_SEMICOLON, true);\n  return true;\n}",
     ["CSS nested style rules", "CSS nested at-rules"]),
    ("css-tokenizer-looks-back", "css/css_tokenizer.c",
     "static int32_t peek(const tz * t, int k) {\n  size_t p = t->pos;\n  t->ctx->work++;",
     "static int32_t peek(const tz * t, int k) {\n  size_t p = t->pos;\n  t->ctx->work += 1 + (t->pos >> 6); /* a look-ahead that looks back over what was read */",
     ["CSS comments", "CSS escapes"]),
    ("css-ids-walk-the-path", "css/css_parser.c",
     "  while (st.len > 0 && ok(p)) {\n    p->ctx->work++;\n    walk * w",
     "  while (st.len > 0 && ok(p)) {\n    p->ctx->work += st.len; /* each node's id found by walking up to it */\n    walk * w",
     ["CSS nested style rules"]),
    ("css-serializer-walks-the-stack", "css/css_serialize.c",
     "  while (st->len > 0 && w->status == GTEXT_CSS_OK) {\n    w->work++;",
     "  while (st->len > 0 && w->status == GTEXT_CSS_OK) {\n    w->work += st->len; /* each task looks at every task below it */",
     ["CSS serialize nested rules"]),
    ("sniffer-reads-past-the-window", "html/html_sniff.c",
     "  if (n > GTEXT_HTML_PRESCAN_BYTES) {\n    n = GTEXT_HTML_PRESCAN_BYTES; /* \"the end condition\" */\n  }\n  /* \"Prescan for UTF-16",
     "  /* \"Prescan for UTF-16",
     ["1 MiB of"]),
    # The XML parser's DTD: the walk of the earlier bindings that each declaration made, in the form it had (a unit a binding).
    ("xml-declaration-walks-the-bindings", "xml/xml_doctype.c",
     "  GTEXT_XML_Status charged = xml_charge_work(p, 1);\n",
     "  GTEXT_XML_Status charged = xml_charge_work(p, 1 + p->dtd_binding_count);\n",
     ["entity declarations", "notation declarations", "attribute-list declarations"]),
    ("xml-start-tag-walks-the-declarations", "xml/xml_parser.c",
     "    const xml_dtd_binding * first =\n",
     "    p->total_work += p->dtd_binding_count; /* every declaration, for the defaults */\n    const xml_dtd_binding * first =\n",
     ["start tags against many attribute-list declarations"]),
    ("xml-reference-walks-the-entities", "xml/xml_lexer.c",
     "      ? xml_find_general_entity(p, (const unsigned char *)name, length) : NULL;\n",
     "      ? xml_find_general_entity(p, (const unsigned char *)name, length) : NULL;\n  p->total_work += p->general_entity_count; /* a lookup that reads every entity */\n",
     ["references to many entities"]),
]


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main(argv):
    prefix, dep_pcs, archive = (os.environ.get(k) for k in ("PREFIX", "DEP_PCS", "ARCHIVE"))
    if not (prefix and dep_pcs and archive):
        sys.stderr.write("PREFIX, DEP_PCS and ARCHIVE must be set; the Makefile (check-html-quadratic-plants) passes them\n")
        return 2
    archive = os.path.abspath(archive)
    generated = os.path.join(os.path.dirname(os.path.dirname(archive)), "generated")
    env = dict(os.environ, PKG_CONFIG_PATH=os.path.join(prefix, "share", "pkgconfig"))
    cflags = sh(["pkg-config", "--cflags"] + dep_pcs.split(), env=env).stdout.split()
    libs = sh(["pkg-config", "--libs"] + dep_pcs.split(), env=env).stdout.split()
    gtest = sh(["pkg-config", "--libs", "--cflags", "gtest", "gtest_main"], env=env).stdout.split()
    names = argv[1:] or [p[0] for p in PLANTS]
    unknown = [n for n in names if n not in [p[0] for p in PLANTS] + ["control"]]
    if unknown:
        sys.stderr.write("no such plant: %s\n" % " ".join(unknown))
        return 2
    scratch = tempfile.mkdtemp(prefix="quadratic-plants-")
    try:
        # The test program is compiled once; each plant is one object linked ahead of the archive.
        test_o = os.path.join(scratch, "test-quadratic.o")
        done = sh(["g++", "-std=c++20", "-O1", "-g", "-c", "-I", os.path.join(ROOT, "include"), "-I", generated] + cflags
                  + [os.path.join(ROOT, "tests", "test-quadratic.cpp"), "-o", test_o])
        if done.returncode != 0:
            sys.stderr.write(done.stderr[-3000:])
            return 2
        failures = 0
        for name in (["control"] + [n for n in names if n != "control"]) if "control" in names or len(argv) == 1 else names:
            plant = next((p for p in PLANTS if p[0] == name), None)
            work = os.path.join(scratch, name)
            os.makedirs(work)
            objs = []
            if plant:
                _, rel, old, new, expect = plant
                shutil.copytree(os.path.join(ROOT, "src"), os.path.join(work, "src"))
                path = os.path.join(work, "src", rel)
                text = open(path).read()
                if text.count(old) != 1:
                    sys.stderr.write("plant %s: the text is in %s %d times, not once\n" % (name, rel, text.count(old)))
                    return 2
                open(path, "w").write(text.replace(old, new))
                obj = os.path.join(work, "plant.o")
                done = sh(["cc", "-std=c17", "-O2", "-fPIC", "-fvisibility=hidden", "-DGTEXT_BUILD", "-I", os.path.join(ROOT, "include"),
                           "-I", generated] + cflags + ["-c", path, "-o", obj])
                if done.returncode != 0:
                    sys.stderr.write("plant %s did not compile:\n%s" % (name, done.stderr[-3000:]))
                    return 2
                objs = [obj]
            exe = os.path.join(work, "testQuadratic")
            done = sh(["g++", "-o", exe, test_o] + objs + [archive, "-lstdc++", "-lm", "-Wl,-rpath," + os.path.join(prefix, "lib", "ghoti.io")] + libs + gtest)
            if done.returncode != 0:
                sys.stderr.write("plant %s did not link:\n%s" % (name, done.stderr[-3000:]))
                return 2
            run = sh([exe, "--gtest_filter=Quadratic.EveryShapeGrowsLinearly"], timeout=900)
            out = run.stdout
            if not plant:
                if run.returncode != 0:
                    print("control: FAILED, the unedited library does not pass the suite:\n" + out[-2000:])
                    failures += 1
                else:
                    print("control: passes")
                continue
            reported = [l for l in out.splitlines()
                        if l.startswith("[") and "] " in l and ("grew" in l or "exceeds" in l or "should not depend" in l or "did not" in l)]
            hit = [e for e in plant[4] if any(e in l for l in reported)]
            if run.returncode == 0 or not hit:
                print("plant %s: NOT CAUGHT (exit %d; reported: %d lines; wanted a failure naming one of %s)" % (name, run.returncode, len(reported), plant[4]))
                failures += 1
            else:
                print("plant %s: caught (%d failure lines, %s)" % (name, len(reported), ", ".join(hit)))
        print("quadratic plants: %s" % ("all caught, control passes" if failures == 0 else "%d problems" % failures))
        return 0 if failures == 0 else 1
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
