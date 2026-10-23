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
  testCssGrammarEdges    the edges a review found: names, ranges, literals, brackets, opaque values, math and
                         substitution functions, the matcher, separators, spans, the work that is charged

The **control** is the same procedure with no edit, and all three programs must pass. Each plant says what it breaks.
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

PROGRAMS = {"corpus": "test-css-grammar-corpus.cpp", "grammar": "test-css-grammar.cpp", "values": "test-css-values.cpp",
            "edges": "test-css-grammar-edges.cpp"}

# (name, file under src/css/, old text (exactly once), new text, programs that must fail); old and new may be
# lists of the same length, for a defect that takes more than one edit
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
    ("bang-allows-nothing", "css_grammar_match.c", "  const bool good = added && iset_subtract(m, &f->r, &only);", "  const bool good = added;", ["corpus", "grammar"]),
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
    ("ratio-takes-negatives", "css_grammar_match.c", "(tok(v, GTEXT_CSS_TOKEN_NUMBER) && v->number >= 0) || (v->kind", "tok(v, GTEXT_CSS_TOKEN_NUMBER) || (v->kind", ["corpus", "grammar"]),
    ("left-recursion-is-allowed", "css_grammar_link.c", "      if (color[to] == 1) {", "      if (0 && color[to] == 1) {", ["grammar"]),
    ("units-are-case-sensitive", "css_units.c", "lower(n[k]) == lower(name[k])", "n[k] == name[k]", ["grammar", "values"]),
    ("substitution-is-not-pending", "css_grammar_match.c", "  if (status == GTEXT_CSS_OK && pending) {", "  if (status == GTEXT_CSS_OK && pending && 0) {", ["corpus", "grammar"]),
    ("comma-list-is-written-without-a-space", "css_value_serialize.c", "    case GTEXT_CSS_SEP_COMMA: puts_(w, \", \"); return;", "    case GTEXT_CSS_SEP_COMMA: puts_(w, \",\"); return;", ["corpus", "values"]),
    ("percent-is-not-written", "css_value_serialize.c", "    put_number(w, v->number, false);\n    puts_(w, \"%\");", "    put_number(w, v->number, false);", ["corpus", "values"]),
    ("strings-are-written-unescaped", "css_value_serialize.c", "    take_status(w, gtext_css_serialize_string(s->data, s->length, &w->inner));",
     "    put(w, \"\\\"\", 1); put(w, s->data, s->length); put(w, \"\\\"\", 1);", ["values"]),
    ("keywords-keep-the-inputs-spelling", "css_grammar_match.c", "    out->atom = g->atom;\n    return arena_text(B, m->c->text + g->str_off, g->str_len, &out->text) != NULL;",
     "    out->atom = g->atom;\n    return arena_text(B, v->value.data, v->value.length, &out->text) != NULL;", ["grammar", "values"]),
    ("grammar-order-is-input-order", "css_grammar_match.c", "      for (int64_t i = found - 1; b.n[i].parent != UINT32_MAX; i = b.n[i].parent) {\n        /* insert sorted",
     "      for (int64_t i = found - 1; 0 && b.n[i].parent != UINT32_MAX; i = b.n[i].parent) {\n        /* insert sorted", ["corpus", "grammar"]),

    # ---- what a review of the first build found (tests/test-css-grammar-edges.cpp, test-css-values.cpp) ----
    ("atoms-ignore-their-count-limit", "css_value.c", "if (t->count >= t->max_atoms || length >= t->max_bytes", "if (length >= t->max_bytes", ["values"]),
    ("identifier-failure-is-dropped", "css_value_serialize.c", "    take_status(w, gtext_css_serialize_identifier(s->data, s->length, &w->inner));",
     "    (void)gtext_css_serialize_identifier(s->data, s->length, &w->inner);", ["values"]),
    ("string-failure-is-dropped", "css_value_serialize.c", "    take_status(w, gtext_css_serialize_string(s->data, s->length, &w->inner));",
     "    (void)gtext_css_serialize_string(s->data, s->length, &w->inner);", ["values"]),
    ("names-end-only-at-a-space", "css_grammar_parse.c", "!is_ws_char(P->t[P->pos]) && P->t[P->pos] != '\\''", "P->t[P->pos] != ' ' && P->t[P->pos] != '\\''", ["edges"]),
    ("range-units-are-not-checked", "css_grammar_parse.c", "if (nt != NT_COUNT && css_grammar_native_numeric(nt) && !css_grammar_range_fits(nt, joined)) {",
     "if (0) {", ["edges"]),
    ("a-brought-range-is-an-error-on-a-name", "css_grammar_link.c", "          if (own) {\n            set_name(L, r, c->text);", "          if (1) {\n            set_name(L, r, c->text);", ["edges"]),
    ("an-empty-range-is-allowed", "css_grammar_link.c", "if ((r->flags & GNF_RANGE) && r->lo > r->hi) {", "if (0) {", ["edges"]),
    ("link-text-overflow-is-out-of-memory", "css_grammar_link.c", "      lfail(L, GTEXT_CSS_E_LIMIT_DEFINITION); /* the texts", "      lfail(L, GTEXT_CSS_E_OOM); /* the texts", ["edges"]),
    ("literals-are-not-checked-as-utf-8", "css_grammar_parse.c", "if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {", "if (0) {", ["edges"]),
    ("quoted-brackets-are-literals", "css_grammar_parse.c", "    if (quoted_open) {", "    if (0 && quoted_open) {", ["edges"]),
    ("any-value-does-not-look-inside", "css_grammar_match.c", "const bool inside = (item->cv->kind != GTEXT_CSS_VALUE_TOKEN) && nested[item->cview];", "const bool inside = false;", ["edges"]),
    ("ratio-takes-no-math-function", "css_grammar_match.c", "  return (tok(v, GTEXT_CSS_TOKEN_NUMBER) && v->number >= 0) || (v->kind == GTEXT_CSS_VALUE_FUNCTION && is_math_function(&v->value));",
     "  return tok(v, GTEXT_CSS_TOKEN_NUMBER) && v->number >= 0;", ["edges"]),
    ("ratio-is-written-with-one-component", "css_value_serialize.c", "    puts_(w, \" / \");\n    put_number(w, v->number2, false);\n    return false;",
     "    if (v->flags & GTEXT_CSS_VFLAG_RATIO_DENOMINATOR) {\n      puts_(w, \" / \");\n      put_number(w, v->number2, false);\n    }\n    return false;", ["edges", "corpus", "values"]),
    ("if-is-not-a-substitution-function", "css_value.c", '{"var", "env", "attr", "if", "inherit", "ident", "random-item"}', '{"var", "env", "attr", "inherit", "ident", "random-item"}', ["edges"]),
    ("matcher-keeps-the-callers-allocator", "css_grammar_match.c", ["  matcher->options.allocator = NULL;\n", "  o.allocator = &mt->allocator;\n"],
     ["", "  o.allocator = mt->options.allocator;\n"], ["edges"]),
    ("opaque-text-failure-is-internal", "css_grammar_match.c", "      B->m->status = st; /* the real one", "      B->m->status = GTEXT_CSS_E_INTERNAL; /* the real one", ["edges"]),
    ("a-list-span-starts-at-its-first-child", "css_grammar_match.c", "  while (first < n && kids[first].span.line == 0) {", "  while (0 && first < n && kids[first].span.line == 0) {", ["edges"]),
    ("separators-ignore-what-can-be-empty", "css_grammar_link.c", "      good = good || !nl[kids[i]];", "      good = true;", ["edges"]),
    ("and-states-are-not-charged", "css_grammar_match.c", "          OKR(charge(m, 1)); /* (a position already a state costs a look too) */\n", "", ["edges"]),
    ("url-token-is-not-a-url", "css_grammar_match.c", "    if (tok(v, GTEXT_CSS_TOKEN_URL)) {\n      return true;\n    }\n    if (v->kind != GTEXT_CSS_VALUE_FUNCTION || !css_ascii_ieq(&v->value, \"url\")) {",
     "    if (tok(v, GTEXT_CSS_TOKEN_URL)) {\n      return false;\n    }\n    if (v->kind != GTEXT_CSS_VALUE_FUNCTION || !css_ascii_ieq(&v->value, \"url\")) {", ["corpus", "grammar"]),
    ("string-is-not-a-string", "css_grammar_match.c", "  case NT_STRING:\n    return tok(v, GTEXT_CSS_TOKEN_STRING);", "  case NT_STRING:\n    return false;", ["corpus", "grammar"]),
    ("percentage-ignores-its-range", "css_grammar_match.c", "    return tok(v, GTEXT_CSS_TOKEN_PERCENTAGE) && in_range(g, v->number);", "    return tok(v, GTEXT_CSS_TOKEN_PERCENTAGE);", ["corpus", "grammar"]),
    ("dimension-is-not-a-dimension", "css_grammar_match.c", "    return tok(v, GTEXT_CSS_TOKEN_DIMENSION) && unit_in_range(g, unit_of(v), v->number);", "    return false;", ["corpus", "grammar"]),
    ("flex-is-not-a-unit-type", "css_grammar_match.c", "  case NT_FLEX: return GTEXT_CSS_UNIT_TYPE_FLEX;", "  case NT_FLEX: return GTEXT_CSS_UNIT_TYPE_NONE;", ["corpus", "grammar"]),
    ("urange-is-not-a-token", "css_grammar_match.c", "  case NT_URANGE:\n    return tok(v, GTEXT_CSS_TOKEN_UNICODE_RANGE);", "  case NT_URANGE:\n    return false;", ["corpus", "grammar"]),
    ("anb-is-never-read", "css_grammar_match.c", "if (gtext_css_anb_parse(v, (size_t)(last->orig - it->orig + 1), &a, &b)) {", "if (0) {", ["corpus", "grammar"]),
    ("open-ended-count-has-an-end", "css_grammar_parse.c", "*max = digits ? (uint32_t)b : GN_INF;", "*max = (uint32_t)b;", ["corpus", "grammar"]),
    ("relative-units-are-always-in-range", "css_grammar_match.c", "  if (n > 0) {\n    return g->hi > 0;", "  if (n > 0) {\n    return true;", ["grammar", "corpus"]),
    ("math-functions-are-not-recognized", "css_grammar_match.c", "static bool is_math_function(const GTEXT_CSS_String * name) {", "static bool is_math_function(const GTEXT_CSS_String * name) {\n  if (1) {\n    return false;\n  }", ["grammar", "edges"]),
    ("a-null-pool-is-offset", "css_grammar_match.c", "  return base ? (const void *)((const char *)base + index * elem) : NULL;",
     "  return (const void *)((const char *)base + index * elem);", ["edges"]),
    ("req-leaks-at-the-work-bound", "css_grammar_match.c", "  const bool added = iset_add(m, &only, f->pos); /* (it may allocate and then fail at the work bound: freed either way) */\n  const bool good = added && iset_subtract(m, &f->r, &only);\n  iset_free(m, &only);\n  OKR(good);",
     "  OKR(iset_add(m, &only, f->pos));\n  const bool good = iset_subtract(m, &f->r, &only);\n  iset_free(m, &only);\n  OKR(good);", ["edges"]),
    ("a-brought-range-meets-the-wrong-name", "css_grammar_link.c", "css_grammar_native_lookup(c->text + g.str_off, g.str_len)", "css_grammar_native_lookup(c->text, g.str_len)", ["edges"]),
    ("a-comma-keeps-its-space", "css_value_serialize.c", "    if (begins_with_comma(child)) {\n      return;\n    }", "", ["corpus"]),
]


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", **kw)


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
                olds, news = (old, new) if isinstance(old, list) else ([old], [new])
                shutil.copytree(os.path.join(ROOT, "src"), os.path.join(work, "src"))
                path = os.path.join(work, "src", "css", rel)
                text = open(path, encoding="utf-8").read()
                for o_ in olds:
                    if text.count(o_) != 1:
                        sys.stderr.write("plant %s: the text is in %s %d times, not once\n" % (name, rel, text.count(o_)))
                        return 2
                for o_, n_ in zip(olds, news):
                    text = text.replace(o_, n_)
                open(path, "w", encoding="utf-8").write(text)
                obj = os.path.join(work, "plant.o")
                done = sh(["cc", "-std=c17", "-O2", "-fPIC", "-fvisibility=hidden", "-DGTEXT_BUILD", "-w", "-I", os.path.join(ROOT, "include"),
                           "-I", generated, "-I", os.path.join(work, "src", "css")] + cflags + ["-c", path, "-o", obj])
                if done.returncode != 0:
                    sys.stderr.write("plant %s did not compile:\n%s" % (name, done.stderr[-3000:]))
                    return 2
                objs = [obj]
            failed = []
            for key, tobj in objects.items():
                if plant and key not in plant[4]:
                    continue
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
