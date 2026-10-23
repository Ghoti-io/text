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
"""Plant a defect in the Value Definition Syntax engine and show the grammar-corpus gate (and the engine's own tests) fail.

    PREFIX=... DEP_PCS=... ARCHIVE=... tools/css/plant-grammar.py [name ...]

A gate that has never failed has not been shown to be able to. Each plant is one edit to one file of src/css/, made in a
copy of src/ in a scratch directory (the working tree is never touched). The edited file is compiled alone and linked ahead
of the library's archive into the test programs, which are then run:

  testCssGrammarCorpus   the seed list of tools/css/grammar-corpus.txt (the gate): a plant marked "corpus" must fail it
  testCssGrammar         the construct-by-construct requirements
  testCssValues          the typed values and their serialization

The **control** is the same procedure with no edit, and all three programs must pass. Each plant says what it breaks.
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

PROGRAMS = {"corpus": "test-css-grammar-corpus.cpp", "grammar": "test-css-grammar.cpp", "values": "test-css-values.cpp"}

# (name, file under src/css/, old text (exactly once), new text, programs that must fail)
PLANTS = [
    ("and-needs-only-one", "css_grammar_match.c", "if (g->kind == GN_AND ? mask == full : mask != 0) {", "if (mask != 0) {", ["corpus", "grammar"]),
    ("or-accepts-none", "css_grammar_match.c", "if (g->kind == GN_AND ? mask == full : mask != 0) {", "if (g->kind == GN_AND ? mask == full : 1) {", ["corpus", "grammar"]),
    ("bar-drops-its-last-alternative", "css_grammar_match.c", "  while (f->i < g->nkids) {\n    ASK(kid_of(m, g, f->i), f->pos, &e);",
     "  while (f->i + 1 < g->nkids) {\n    ASK(kid_of(m, g, f->i), f->pos, &e);", ["corpus", "grammar"]),
    ("star-and-optional-need-one", "css_grammar_match.c", "    if (g->min == 0) {\n      OKR(iset_union_v(m, &f->r, f->a.v, f->a.n));",
     "    if (0) {\n      OKR(iset_union_v(m, &f->r, f->a.v, f->a.n));", ["corpus", "grammar"]),
    ("plus-takes-one-too-few", "css_grammar_match.c", "    if (kk >= g->min) {\n      OKR(iset_union_v(m, &f->r, f->b.v, f->b.n));",
     "    if (kk + 1 >= g->min) {\n      OKR(iset_union_v(m, &f->r, f->b.v, f->b.n));", ["corpus", "grammar"]),
    ("count-bound-off-by-one", "css_grammar_match.c", "if ((g->max != GN_INF && kk > g->max) || f->a.n == 0) {", "if ((g->max != GN_INF && kk > g->max + 1) || f->a.n == 0) {", ["corpus", "grammar"]),
    ("hash-separates-with-a-slash", "css_grammar_match.c", "        if (!literal_at(m, q, ',')) {\n          cursor_next(f);", "        if (!literal_at(m, q, '/')) {\n          cursor_next(f);", ["corpus", "grammar"]),
    ("bang-allows-nothing", "css_grammar_match.c", "  const bool good = iset_subtract(m, &f->r, &only);", "  const bool good = true;", ["corpus", "grammar"]),
    ("range-lower-bound-exclusive", "css_grammar_match.c", "  return v >= g->lo && v <= g->hi;", "  return v > g->lo && v <= g->hi;", ["corpus", "grammar"]),
    ("range-ignores-the-upper-bound", "css_grammar_match.c", "    return c >= g->lo && c <= g->hi;", "    return c >= g->lo;", ["corpus", "grammar"]),
    ("keywords-are-case-sensitive", "css_grammar_match.c", "    if (fold1(a[i]) != fold1(b[i])) {", "    if (a[i] != b[i]) {", ["grammar", "values"]),
    ("function-name-is-not-checked", "css_grammar_match.c",
     "        hit = v->kind == GTEXT_CSS_VALUE_FUNCTION\n            && fold_equal(v->value.data, v->value.length, m->c->text + g->str_off, g->str_len);",
     "        hit = v->kind == GTEXT_CSS_VALUE_FUNCTION;", ["corpus", "grammar"]),
    ("unitless-zero-is-not-a-length", "css_grammar_match.c", "      if (v->number == 0) {\n        return in_range(g, 0.0);", "      if (v->number == 0) {\n        return false;", ["corpus", "grammar"]),
    ("block-kind-is-not-checked", "css_grammar_match.c", "hit = v->kind == GTEXT_CSS_VALUE_BLOCK && v->type == (GTEXT_CSS_Token_Type)g->native;",
     "hit = v->kind == GTEXT_CSS_VALUE_BLOCK;", ["corpus", "grammar"]),
    ("slash-list-needs-no-slash", "css_grammar_match.c", "        if (literal_at(m, p, sep_char(g))) {\n          OKR(iset_add(m, &f->b, p + 1));",
     "        if (1) {\n          OKR(iset_add(m, &f->b, p + 1));", ["corpus", "grammar"]),
    ("integer-is-any-number", "css_grammar_match.c", "    return tok(v, GTEXT_CSS_TOKEN_NUMBER) && (v->flags & GTEXT_CSS_FLAG_INTEGER) && in_range(g, v->number);",
     "    return tok(v, GTEXT_CSS_TOKEN_NUMBER) && in_range(g, v->number);", ["corpus", "grammar"]),
    ("custom-ident-takes-css-wide-keywords", "css_grammar_match.c", "tok(v, GTEXT_CSS_TOKEN_IDENT) && !is_css_wide(&v->value);", "tok(v, GTEXT_CSS_TOKEN_IDENT);", ["corpus", "grammar"]),
    ("dashed-ident-needs-one-dash", "css_grammar_match.c", "v->value.data[0] == '-' && v->value.data[1] == '-';", "v->value.data[0] == '-';", ["corpus", "grammar"]),
    ("any-value-takes-bad-tokens", "css_grammar_match.c", "const uint32_t limit = t == NT_ANY_VALUE ? it->bad : it->badd;", "const uint32_t limit = it->end;", ["corpus", "grammar"]),
    ("ratio-takes-negatives", "css_grammar_match.c", "      if (v && tok(v, GTEXT_CSS_TOKEN_NUMBER) && v->number >= 0) {", "      if (v && tok(v, GTEXT_CSS_TOKEN_NUMBER)) {", ["corpus", "grammar"]),
    ("left-recursion-is-allowed", "css_grammar_link.c", "      if (color[to] == 1) {", "      if (0 && color[to] == 1) {", ["grammar"]),
    ("units-are-case-sensitive", "css_units.c", "lower(n[k]) == lower(name[k])", "n[k] == name[k]", ["grammar", "values"]),
    ("substitution-is-not-pending", "css_grammar_match.c", "  if (status == GTEXT_CSS_OK && pending) {", "  if (status == GTEXT_CSS_OK && pending && 0) {", ["corpus", "grammar"]),
    ("comma-list-is-written-without-a-space", "css_value_serialize.c", "    case GTEXT_CSS_SEP_COMMA: puts_(w, \", \"); return;", "    case GTEXT_CSS_SEP_COMMA: puts_(w, \",\"); return;", ["corpus", "values"]),
    ("percent-is-not-written", "css_value_serialize.c", "    put_number(w, v->number, false);\n    puts_(w, \"%\");", "    put_number(w, v->number, false);", ["corpus", "values"]),
    ("strings-are-written-unescaped", "css_value_serialize.c", "    (void)gtext_css_serialize_string(s->data ? s->data : \"\", s->length, &w->inner);",
     "    put(w, \"\\\"\", 1); put(w, s->data ? s->data : \"\", s->length); put(w, \"\\\"\", 1);", ["values"]),
    ("keywords-keep-the-inputs-spelling", "css_grammar_match.c", "    out->atom = g->atom;\n    return arena_text(B, m->c->text + g->str_off, g->str_len, &out->text) != NULL;",
     "    out->atom = g->atom;\n    return arena_text(B, v->value.data, v->value.length, &out->text) != NULL;", ["grammar", "values"]),
    ("grammar-order-is-input-order", "css_grammar_match.c", "      for (int64_t i = found - 1; b.n[i].parent != UINT32_MAX; i = b.n[i].parent) {\n        /* insert sorted",
     "      for (int64_t i = found - 1; 0 && b.n[i].parent != UINT32_MAX; i = b.n[i].parent) {\n        /* insert sorted", ["corpus", "grammar"]),
]


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main(argv):
    prefix, dep_pcs, archive = (os.environ.get(k) for k in ("PREFIX", "DEP_PCS", "ARCHIVE"))
    if not (prefix and dep_pcs and archive):
        sys.stderr.write("PREFIX, DEP_PCS and ARCHIVE must be set; the Makefile (check-css-grammar-plants) passes them\n")
        return 2
    archive = os.path.abspath(archive)
    generated = os.path.join(os.path.dirname(os.path.dirname(archive)), "generated")
    env = dict(os.environ, PKG_CONFIG_PATH=os.path.join(prefix, "share", "pkgconfig"))
    cflags = sh(["pkg-config", "--cflags"] + dep_pcs.split(), env=env).stdout.split()
    libs = sh(["pkg-config", "--libs"] + dep_pcs.split(), env=env).stdout.split()
    gtest = sh(["pkg-config", "--libs", "--cflags", "gtest", "gtest_main"], env=env).stdout.split()
    names = argv[1:] or ["control"] + [p[0] for p in PLANTS]
    unknown = [n for n in names if n not in [p[0] for p in PLANTS] + ["control"]]
    if unknown:
        sys.stderr.write("no such plant: %s\n" % " ".join(unknown))
        return 2
    scratch = tempfile.mkdtemp(prefix="css-grammar-plants-")
    try:
        objects = {}
        for key, source in PROGRAMS.items():
            obj = os.path.join(scratch, key + ".o")
            done = sh(["g++", "-std=c++20", "-O1", "-g", "-c", "-I", os.path.join(ROOT, "include"), "-I", generated] + cflags
                      + [os.path.join(ROOT, "tests", source), "-o", obj])
            if done.returncode != 0:
                sys.stderr.write(done.stderr[-3000:])
                return 2
            objects[key] = obj
        failures = 0
        for name in names:
            plant = next((p for p in PLANTS if p[0] == name), None)
            work = os.path.join(scratch, name)
            os.makedirs(work)
            objs = []
            if plant:
                _, rel, old, new, _must = plant
                shutil.copytree(os.path.join(ROOT, "src"), os.path.join(work, "src"))
                path = os.path.join(work, "src", "css", rel)
                text = open(path, encoding="utf-8").read()
                if text.count(old) != 1:
                    sys.stderr.write("plant %s: the text is in %s %d times, not once\n" % (name, rel, text.count(old)))
                    return 2
                open(path, "w", encoding="utf-8").write(text.replace(old, new))
                obj = os.path.join(work, "plant.o")
                done = sh(["cc", "-std=c17", "-O2", "-fPIC", "-fvisibility=hidden", "-DGTEXT_BUILD", "-w", "-I", os.path.join(ROOT, "include"),
                           "-I", generated, "-I", os.path.join(work, "src", "css")] + cflags + ["-c", path, "-o", obj])
                if done.returncode != 0:
                    sys.stderr.write("plant %s did not compile:\n%s" % (name, done.stderr[-3000:]))
                    return 2
                objs = [obj]
            failed = []
            for key, tobj in objects.items():
                exe = os.path.join(work, key)
                done = sh(["g++", "-o", exe, tobj] + objs + [archive, "-lstdc++", "-lm", "-Wl,-rpath," + os.path.join(prefix, "lib", "ghoti.io")]
                          + libs + gtest + ["-Wl,--allow-multiple-definition"])
                if done.returncode != 0:
                    sys.stderr.write("plant %s did not link %s:\n%s" % (name, key, done.stderr[-3000:]))
                    return 2
                run = sh([exe, "--gtest_brief=1"], timeout=900, cwd=ROOT)
                if run.returncode != 0:
                    failed.append(key)
            if not plant:
                if failed:
                    print("control: FAILED, the unedited library does not pass %s" % ", ".join(failed))
                    failures += 1
                else:
                    print("control: passes")
                continue
            missing = [k for k in plant[4] if k not in failed]
            if missing:
                print("plant %s: NOT CAUGHT by %s (failed: %s)" % (name, ", ".join(missing), ", ".join(failed) or "nothing"))
                failures += 1
            else:
                print("plant %s: caught by %s" % (name, ", ".join(failed)))
        print("grammar plants: %s" % ("all caught, control passes" if failures == 0 else "%d problems" % failures))
        return 0 if failures == 0 else 1
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
