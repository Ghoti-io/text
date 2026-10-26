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
  testCssValuesReview    what a review of the value layer found, one test for each (the "review-" plants restore the defects)
  testCssRegistry, testCssProperty, testCssRegistryOom
                         the registry (the tables' relations, consolidation, registration, override, limits, grammars), property-level
                         parsing, and the allocation sweeps of both
  testCssGrammarFuzzSeeds, testCssValuesFuzzSeeds, testCssRegistryFuzzSeeds
                         the seed corpora of the two fuzzers, replayed through their harnesses' properties with no fuzzer built.
                         The plants of the fuzzers (tools/css/plant-fuzz.py: GRAMMAR_PLANTS, VALUES_PLANTS) are run here too, as
                         "fuzz-<name>", so that the fuzzers' own defect checks are part of `make test` on a host with no clang;
                         `make check-css-fuzz-plants` runs the same plants through libFuzzer.

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
            "edges": "test-css-grammar-edges.cpp", "grammar-seeds": "test-css-grammar-fuzz-seeds.cpp",
            "values-seeds": "test-css-values-fuzz-seeds.cpp", "review": "test-css-values-review.cpp",
            "registry-seeds": "test-css-registry-fuzz-seeds.cpp", "registry": "test-css-registry.cpp", "property": "test-css-property.cpp",
            "registry-oom": "test-css-registry-oom.cpp", "canon": "test-css-property-canon.cpp"}

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
    ("unitless-zero-is-not-a-length", "css_grammar_match.c", "      if (v->number == 0) {\n        return range_for_dimension(g, t) ? in_range(g, 0.0) : 1;", "      if (v->number == 0) {\n        return false;", ["corpus", "grammar"]),
    ("block-kind-is-not-checked", "css_grammar_match.c", "hit = v->kind == GTEXT_CSS_VALUE_BLOCK && v->type == (GTEXT_CSS_Token_Type)g->native;",
     "hit = v->kind == GTEXT_CSS_VALUE_BLOCK;", ["corpus", "grammar"]),
    ("slash-list-needs-no-slash", "css_grammar_match.c", "        if (literal_at(m, p, sep_char(g))) {\n          OKR(iset_add(m, &f->b, p + 1));",
     "        if (1) {\n          OKR(iset_add(m, &f->b, p + 1));", ["corpus", "grammar"]),
    ("integer-is-any-number", "css_grammar_match.c", "    return tok(v, GTEXT_CSS_TOKEN_NUMBER) && (v->flags & GTEXT_CSS_FLAG_INTEGER) && in_range(g, v->number);",
     "    return tok(v, GTEXT_CSS_TOKEN_NUMBER) && in_range(g, v->number);", ["corpus", "grammar"]),
    ("custom-ident-takes-css-wide-keywords", "css_grammar_match.c", "tok(v, GTEXT_CSS_TOKEN_IDENT) && !is_css_wide(&v->value);", "tok(v, GTEXT_CSS_TOKEN_IDENT);", ["corpus", "grammar"]),
    ("dashed-ident-needs-one-dash", "css_grammar_match.c", "v->value.data[0] == '-' && v->value.data[1] == '-';", "v->value.data[0] == '-';", ["corpus", "grammar"]),
    ("any-value-takes-bad-tokens", "css_grammar_match.c", "const uint32_t limit = t == NT_ANY_VALUE ? it->bad : it->badd;", "const uint32_t limit = it->end;", ["corpus", "grammar"]),
    ("ratio-takes-negatives", "css_grammar_match.c", "  if (tok(v, GTEXT_CSS_TOKEN_NUMBER)) {\n    return v->number >= 0;", "  if (tok(v, GTEXT_CSS_TOKEN_NUMBER)) {\n    return 1;", ["corpus", "grammar"]),
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
    ("an-empty-range-is-allowed", "css_grammar_link.c", "if ((r->flags & GNF_RANGE) && r->lo > r->hi) {", "if (0) {", ["edges"]),
    ("link-text-overflow-is-out-of-memory", "css_grammar_link.c", "      lfail(L, GTEXT_CSS_E_LIMIT_DEFINITION); /* the texts", "      lfail(L, GTEXT_CSS_E_OOM); /* the texts", ["edges"]),
    ("literals-are-not-checked-as-utf-8", "css_grammar_parse.c", "if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {", "if (0) {", ["edges"]),
    ("quoted-brackets-are-literals", "css_grammar_parse.c", "    if (quoted_open) {", "    if (0 && quoted_open) {", ["edges"]),
    ("any-value-does-not-look-inside", "css_grammar_match.c", "const bool inside = (item->cv->kind != GTEXT_CSS_VALUE_TOKEN) && nested[item->cview];", "const bool inside = false;", ["edges"]),
    ("ratio-takes-no-math-function", "css_grammar_match.c", "    return xi->state == X_MATH && xi->category == GTEXT_CSS_MATH_CAT_NUMBER && !xi->has_percent;",
     "    return 0;", ["edges"]),
    ("ratio-is-written-with-one-component", "css_value_serialize.c", "    puts_(w, \" / \");\n    put_number(w, v->number2, false);\n    return false;",
     "    if (v->flags & GTEXT_CSS_VFLAG_RATIO_DENOMINATOR) {\n      puts_(w, \" / \");\n      put_number(w, v->number2, false);\n    }\n    return false;", ["edges", "corpus", "values"]),
    ("if-is-not-a-substitution-function", "css_value.c", '{"var", "env", "attr", "if", "inherit", "ident", "random-item"}', '{"var", "env", "attr", "inherit", "ident", "random-item"}', ["edges"]),
    ("matcher-keeps-the-callers-allocator", "css_grammar_match.c", ["  matcher->options.allocator = NULL;\n", "  o.allocator = &mt->allocator;\n"],
     ["", "  o.allocator = mt->options.allocator;\n"], ["edges"]),
    ("opaque-text-failure-is-internal", "css_grammar_match.c", "      B->m->status = st; /* the real one", "      B->m->status = GTEXT_CSS_E_INTERNAL; /* the real one", ["edges"]),
    ("a-list-span-starts-at-its-first-child", "css_grammar_match.c", "  while (first < n && kids[first].span.line == 0) {", "  while (0 && first < n && kids[first].span.line == 0) {", ["edges"]),
    ("separators-ignore-what-can-be-empty", "css_grammar_link.c", "      good = good || !nl[kids[i]];", "      good = true;", ["edges"]),
    ("and-states-are-not-charged", "css_grammar_match.c", "          OKR(charge(m, 1)); /* (a position already a state costs a look too) */\n", "", ["edges"]),
    ("url-token-is-not-a-url", "css_grammar_match.c", "    if (tok(v, GTEXT_CSS_TOKEN_URL)) {\n      return true;\n    }\n    if (v->kind != GTEXT_CSS_VALUE_FUNCTION || !(css_ascii_ieq(&v->value, \"url\") || css_ascii_ieq(&v->value, \"src\"))) {",
     "    if (tok(v, GTEXT_CSS_TOKEN_URL)) {\n      return false;\n    }\n    if (v->kind != GTEXT_CSS_VALUE_FUNCTION || !(css_ascii_ieq(&v->value, \"url\") || css_ascii_ieq(&v->value, \"src\"))) {", ["corpus", "grammar"]),
    ("string-is-not-a-string", "css_grammar_match.c", "  case NT_STRING:\n    return tok(v, GTEXT_CSS_TOKEN_STRING);", "  case NT_STRING:\n    return false;", ["corpus", "grammar"]),
    ("percentage-ignores-its-range", "css_grammar_match.c", "    return tok(v, GTEXT_CSS_TOKEN_PERCENTAGE) && in_range(g, v->number);", "    return tok(v, GTEXT_CSS_TOKEN_PERCENTAGE);", ["corpus", "grammar"]),
    ("dimension-is-not-a-dimension", "css_grammar_match.c", "    return tok(v, GTEXT_CSS_TOKEN_DIMENSION) && unit_in_range(g, unit_of(v), v->number);", "    return false;", ["corpus", "grammar"]),
    ("flex-is-not-a-unit-type", "css_grammar_match.c", "  case NT_FLEX: return GTEXT_CSS_UNIT_TYPE_FLEX;", "  case NT_FLEX: return GTEXT_CSS_UNIT_TYPE_NONE;", ["corpus", "grammar"]),
    ("urange-is-any-range", "css_grammar_match.c", " && v->range_start <= v->range_end && v->range_end <= 0x10FFFFu;", ";", ["property"]),
    ("urange-is-not-a-token", "css_grammar_match.c", "    return tok(v, GTEXT_CSS_TOKEN_UNICODE_RANGE) && v->range_start <= v->range_end && v->range_end <= 0x10FFFFu;", "    return false;", ["corpus", "grammar"]),
    ("anb-is-never-read", "css_grammar_match.c", "if (gtext_css_anb_parse(v, (size_t)(last->orig - it->orig + 1), &a, &b)) {", "if (0) {", ["corpus", "grammar"]),
    ("open-ended-count-has-an-end", "css_grammar_parse.c", "*max = digits ? (uint32_t)b : GN_INF;", "*max = (uint32_t)b;", ["corpus", "grammar"]),
    ("relative-units-are-always-in-range", "css_grammar_match.c", "  if (n > 0) {\n    return g->hi > 0;", "  if (n > 0) {\n    return true;", ["grammar", "corpus"]),
    ("math-functions-are-not-recognized", "css_grammar_match.c", "static bool math_target(gn_native t) {", "static bool math_target(gn_native t) {\n  if (1) {\n    return false;\n  }", ["grammar", "edges"]),
    ("a-null-pool-is-offset", "css_grammar_match.c", "  return base ? (const void *)((const char *)base + index * elem) : NULL;",
     "  return (const void *)((const char *)base + index * elem);", ["edges"]),
    ("req-leaks-at-the-work-bound", "css_grammar_match.c", "  const bool added = iset_add(m, &only, f->pos); /* (it may allocate and then fail at the work bound: freed either way) */\n  const bool good = added && iset_subtract(m, &f->r, &only);\n  iset_free(m, &only);\n  OKR(good);",
     "  OKR(iset_add(m, &only, f->pos));\n  const bool good = iset_subtract(m, &f->r, &only);\n  iset_free(m, &only);\n  OKR(good);", ["edges"]),
    ("a-brought-range-meets-the-wrong-name", "css_grammar_link.c", "css_grammar_native_lookup(c->text + g.str_off, g.str_len)", "css_grammar_native_lookup(c->text, g.str_len)", ["edges"]),
    ("a-comma-keeps-its-space", "css_value_serialize.c", "    if (begins_with_comma(child)) {\n      return;\n    }", "", ["corpus"]),

    # What a review of the value layer found: each plant restores the defect, and tests/test-css-values-review.cpp must fail.
    ("review-easing-unknown-keyword-is-an-error", "css_easing.c", "    return GTEXT_CSS_OK; /* a keyword that is not an easing function: not valid, not an error */",
     "    return GTEXT_CSS_E_INVALID;", ["review"]),
    ("review-step-start-is-jump-start", "css_easing.c", "e->position_short = KW[i].kw == GTEXT_CSS_EASING_KW_STEP_START;", "e->position_short = false;", ["review"]),
    ("review-math-parse-keeps-its-results", "css_math.c", "  *valid = false;\n  *out = NULL;\n  GTEXT_Allocator alloc = *((options && options->allocator)",
     "  GTEXT_Allocator alloc = *((options && options->allocator)", ["review"]),
    ("review-color-parse-keeps-its-results", "css_color.c", "  *valid = false;\n  *out = NULL;\n  GTEXT_Allocator alloc = *((options && options->allocator)",
     "  GTEXT_Allocator alloc = *((options && options->allocator)", ["review"]),
    ("review-color-nodes-are-one", "css_color.c", "    result->nodes = nodes;", "    result->nodes = ok ? 1 : 0;", ["review"]),
    ("review-cmyk-scans-a-from", "css_color.c", "else if (f != F_CMYK && a.n >= 2 && is_ident(a.v[0], \"from\")) {", "else if (a.n >= 2 && is_ident(a.v[0], \"from\")) {", ["review"]),
    ("review-gradient-kind-by-search", "css_image.c", ["  if (css_fn_name_is(&s, \"linear-gradient\")) {", "  else if (css_fn_name_is(&s, \"radial-gradient\")) {",
                                                      "  else if (css_fn_name_is(&s, \"conic-gradient\")) {"],
     ["  if (0) {", "  else if (strstr(name->data, \"radial\")) {", "  else if (strstr(name->data, \"conic\")) {"], ["review"]),
    ("review-gradient-stray-keyword-is-a-stop", "css_image.c", "      if (ncolor > 1 || other > 0 || (color ? false : np != 1)) {", "      if (0) {", ["review"]),
    ("review-transform-length-unit-is-lost", "css_function.c", "      case GTEXT_CSS_MATH_CAT_LENGTH:\n        if (unit) {\n          *unit = u;\n        }\n        return 2;",
     "      case GTEXT_CSS_MATH_CAT_LENGTH: return 2;", ["review"]),
    ("review-transform-ignores-the-allocator", "css_transform.c", "const GTEXT_Allocator * a = allocator ? allocator : gtext_allocator_default();",
     "const GTEXT_Allocator * a = gtext_allocator_default();", ["review"]),
    ("review-progress-needs-an-element", "css_substitution.c", "  case GTEXT_CSS_SUBST_PROGRESS: case GTEXT_CSS_SUBST_IDENT: return GTEXT_CSS_NEEDS_NOTHING;",
     "  case GTEXT_CSS_SUBST_PROGRESS: case GTEXT_CSS_SUBST_IDENT: return GTEXT_CSS_NEEDS_ELEMENT;", ["review"]),
    ("review-progress-no-clamp-is-not-kept", "css_math.c", "        F->no_clamp = true;\n        return true;", "        return true;", ["review"]),
    ("review-random-key-is-dropped", "css_math.c", "F->op == GTEXT_CSS_MATH_RANDOM ? F->key : NULL", "NULL", ["review"]),
    ("review-media-progress-is-known", "css_math.c", "  \"sibling-index\", \"sibling-count\",\n};", "  \"sibling-index\", \"sibling-count\", \"media-progress\", \"container-progress\",\n};", ["review"]),
    ("review-sibling-index-takes-arguments", "css_math.c", "if (is_unresolved_number_function(&cv->value) && no_arguments(cv)) {", "if (is_unresolved_number_function(&cv->value)) {", ["review"]),
    ("review-out-of-memory-is-cached-as-a-bad-grammar", "css_function.c", "            t->status = st; /* not a defect of the grammar: nothing is cached, and the next ask tries again */",
     "            t->status = st;\n            t->state[index] = 2;", ["review"]),
    ("review-prepare-makes-nothing", "css_function.c", "      (void)gtext_css_function_types_resolve(t, tab[i].kind, tab[i].name, strlen(tab[i].name));\n", "", ["review"]),
    ("review-symbols-is-not-a-substitution-function", "css_function.c", "\"counters\", \"symbols\", \"calc-size\"};", "\"counters\", \"calc-size\"};", ["review"]),
    ("review-ratio-takes-a-percentage-calc", "css_grammar_match.c", "xi->category == GTEXT_CSS_MATH_CAT_NUMBER && !xi->has_percent;\n  }\n  return 0;",
     "xi->category == GTEXT_CSS_MATH_CAT_NUMBER;\n  }\n  return 0;", ["review"]),
    ("review-color-serialize-keeps-every-text", "css_color_serialize.c", "      gtext_allocator_free(&a, kid->text);\n      kid->text = NULL;\n", "", ["review"]),
    ("review-resolve-refuses-a-long-symbol", "css_math.c", "      if (!env || !env->symbol) {\n        ok = false;\n        break;\n      }\n      /* the name is NUL-terminated",
     "      if (!env || !env->symbol || n->text.length >= 64) {\n        ok = false;\n        break;\n      }\n      /* the name is NUL-terminated", ["review"]),
    ("review-mod-reaches-the-modulus", "css_math_simplify.c", "    return gmath_fabs(s) >= gmath_fabs(B) ? toward_zero(B) : s;", "    return s;", ["review"]),
    ("review-custom-space-is-written-raw", "css_color_serialize.c", "(c->space == GTEXT_CSS_SPACE_CUSTOM ? put_ident(b, c->name.data, c->name.length) :",
     "(c->space == GTEXT_CSS_SPACE_CUSTOM ? put(b, c->name.data, c->name.length) :", ["review", "values-seeds"]),
    ("review-legacy-alpha-is-at-full-precision", "css_color_serialize.c", "    return put_num(b, gmath_floor(quotient + 0.5) / 1000.0);", "    return put_num(b, ai / 255.0);", ["review"]),
]


PLANTS += [
    ("omit-nothing-is-required-of-an-optional-term", "css_grammar_link.c", "      req[i] = make_inner(L, GN_REQ, &seg[i], 1, 1, 1, seg[i], seg[i], nl, 0);\n      ok = req[i] != GN_NONE;",
     "      req[i] = seg[i];\n      ok = true;", ["edges"]),
    ("omit-the-comma-of-an-optional-term-is-required", "css_grammar_link.c", "        p = make_inner(L, GN_MULT, &p, 1, 0, 1, lit, seg[j], nl, 1);", "        p = make_inner(L, GN_MULT, &p, 1, 1, 1, lit, seg[j], nl, 1);", ["edges"]),
    ("omit-only-the-first-term-can-be-first", "css_grammar_link.c", "  while (ok && last_first + 1 < m && !good[last_first]) {", "  while (0 && ok && last_first + 1 < m && !good[last_first]) {", ["edges"]),
    ("omit-an-empty-segment-is-a-list", "css_grammar_link.c", "      if (run == 0) {\n        return true; /* an empty segment: not a list */\n      }", "      if (run == 0 && 0) {\n        return true; /* an empty segment: not a list */\n      }", ["edges"]),
    ("omit-a-nullable-group-is-not-nullable", "css_grammar_link.c", "      if (commas && whole && k->kind == GN_LITERAL && k->lit == ',') {", "      if (0 && commas && whole && k->kind == GN_LITERAL && k->lit == ',') {", ["edges"]),
    ("omit-the-rewritten-form-is-split-again", "css_grammar_link.c", "g.nkids < 3 || (g.flags & GNF_OMITTED)) {", "g.nkids < 3) {", ["edges"]),
    ("omit-left-recursion-is-looked-for-before-the-rewrite", "css_grammar_link.c", "    if (split_sequences(&L, nl)) {\n      find_left_recursion(&L, nl);\n    }", "    if (find_left_recursion(&L, nl)) {\n      split_sequences(&L, nl);\n    }", ["edges"]),
    ("omit-a-segment-with-all-items-optional-is-good", "css_grammar_link.c", "      good = good || !nl[kids[i]];", "      good = good || !nl[kids[i]] || nl[kids[i]];", ["edges", "grammar"]),
]

# The canonical-form rules (src/css/css_property_canon.c): each is undone once, and tests/test-css-property-canon.cpp must fail.
PLANTS += [
    ('canon-pair-keeps-both', "css_property_canon.c", 'static bool rule_pair(cb * out, const sp * t, size_t n) {\n  if (n == 2 && sp_eq(t[0], t[1])) {',
     'static bool rule_pair(cb * out, const sp * t, size_t n) {\n  if (n == 2 && !sp_eq(t[0], t[1])) {', ["canon"]),
    ('canon-alpha-percent-is-not-divided', "css_property_canon.c", '  return dec_put(b, &d, 2, 6);',
     '  return dec_put(b, &d, 0, 6);', ["canon"]),
    ('canon-percent-is-not-rounded-to-the-serializer', "css_property_canon.c", '  return dec_put(b, &d, 2, 6);',
     '  return dec_put(b, &d, 2, 0);', ["canon"]),
    ('canon-arc-flags-are-numbers', "css_property_canon.c", "    if ((cmd == 'a' || cmd == 'A') && (argi % 7 == 3 || argi % 7 == 4)) {",
     "    if (0 && (cmd == 'a' || cmd == 'A') && (argi % 7 == 3 || argi % 7 == 4)) {", ["canon"]),
    ('canon-a-hex-escape-loses-its-terminating-space', "css_property_canon.c", '        if (i + 1 < n && is_ws(t[i + 1])) {\n          i++;\n          escaped = i;',
     '        if (0 && i + 1 < n && is_ws(t[i + 1])) {\n          i++;\n          escaped = i;', ["canon"]),
    ('canon-an-escaped-space-is-trimmed', "css_property_canon.c", 'while (b > a && is_ws(t[b - 1]) && b - 1 != escaped) {',
     'while (b > a && is_ws(t[b - 1])) {', ["canon"]),
    ('canon-an-escape-does-not-take-the-next-character', "css_property_canon.c", "    else if (c == '\\\\' && i + 1 < n && t[i + 1] != '\\n'",
     "    else if (0 && c == '\\\\' && i + 1 < n && t[i + 1] != '\\n'", ["canon"]),
    ('canon-a-calc-angle-is-negated-by-a-sign', "css_property_canon.c", '    if (sign < 0 && !negatable_angle(t[3])) {\n      return false;\n    }',
     '    if (sign < 0 && 0) {\n      return false;\n    }', ["canon"]),
    ('canon-a-negated-zero-is-minus-zero', "css_property_canon.c", "  if (dec_zero(&d)) {\n    cb_putc(out, '0');\n  }\n  else {\n    const size_t skip",
     "  if (0) {\n    cb_putc(out, '0');\n  }\n  else {\n    const size_t skip", ["canon"]),
    ('canon-jump-end-steps-keeps-its-keyword', "css_property_canon.c", '(eq(args.v[1], "end") || eq(args.v[1], "jump-end"))',
     '(eq(args.v[1], "end"))', ["canon"]),
    ('canon-a-counter-default-is-one', "css_property_canon.c", '  return counter_defaults(out, t, n, "0");',
     '  return counter_defaults(out, t, n, "1");', ["canon"]),
    ('canon-margin-trim-block-is-not-written', "css_property_canon.c", '  if (n == 2 && eq(t[0], "block-start") && eq(t[1], "block-end")) {',
     '  if (0 && n == 2 && eq(t[0], "block-start") && eq(t[1], "block-end")) {', ["canon"]),
    ('canon-translate-keeps-a-zero-z', "css_property_canon.c", '  if (keep == 3 && is_zero_dim(t[2])) {\n    keep = 2;\n  }\n  if (keep == 2 && is_zero_dim(t[1])) {\n    keep = 1;\n  }\n  if (keep == n) {',
     '  if (keep == 2 && is_zero_dim(t[1])) {\n    keep = 1;\n  }\n  if (keep == n) {', ["canon"]),
    ('canon-block-flow-is-not-block', "css_property_canon.c", '  if (n < 2 && !(n == 1 && eq(t[0], "flow"))) {',
     '  if (n < 2) {', ["canon"]),
]

PLANTS += [
    ("registry-compile-takes-no-lock", "css_registry.c", "    GCU_MUTEX_LOCK(r->lock);\n    const GTEXT_CSS_Status st = compile_locked(r, lz, text, length, scope);\n    GCU_MUTEX_UNLOCK(r->lock);",
     "    const GTEXT_CSS_Status st = compile_locked(r, lz, text, length, scope);", ["registry"]),
    ("registry-compile-does-not-look-again-under-the-lock", "css_registry.c", "  if (gcu_atomic_int_load(&lz->state) != GTEXT_CSS_GRAMMAR_NOT_COMPILED) {\n    return GTEXT_CSS_OK;\n  }\n  GTEXT_CSS_Grammar_Result res;",
     "  GTEXT_CSS_Grammar_Result res;", ["registry"]),
    # Publishing the state before the grammar is complete is a memory-ordering defect with a window of nanoseconds: only ThreadSanitizer
    # (make test-tsan, in the build-host tier) sees it reliably, and a plant here would be caught "sometimes", which is not a plant.
    ("registry-a-failed-freeze-remembers-what-it-compiled", "css_registry.c", "    freeze_undo(builder->reg, work_before);", "    (void)work_before;", ["registry"]),
    ("registry-the-later-of-two-names-does-not-win-the-table", "css_registry.c", "h[i] = (uint32_t)(k + 1); /* later wins */", "/* first wins */", ["registry"]),
    ("registry-the-old-name-table-is-not-freed-on-growth", "css_registry.c", "    gtext_allocator_free(&r->alloc, r->hash);\n    r->hash = h;", "    r->hash = h;", ["registry-oom"]),
    ("registry-a-refused-name-leaks-its-folded-copy", "css_registry.c", "  if (found && !override) {\n    gtext_allocator_free(&r->alloc, lname);", "  if (found && !override) {", ["registry-oom"]),
    ("registry-the-all-of-the-caller-is-the-generated-all", "css_property.c", "return (info->flags & GTEXT_CSS_PROP_GENERATED) && info->name_length == 3", "return info->name_length == 3", ["registry"]),
    ("property-a-nested-at-rule-without-declarations-has-descriptors", "css_property.c", "gtext_css_registry_at_rule_info(rule, &info) && info.declarations) {", "gtext_css_registry_at_rule_info(rule, &info)) {", ["property"]),
    ("registry-a-shorthand-may-name-the-property-it-replaces", "css_registry.c", "    if (existing && id == existing) {\n      gtext_allocator_free(&r->alloc, lh);", "    if (0) {\n      gtext_allocator_free(&r->alloc, lh);", ["registry"]),
    ("registry-the-node-budget-is-not-held", "css_registry.c", "      if (gr.nodes > r->max_nodes - (r->grammar_nodes < r->max_nodes ? r->grammar_nodes : r->max_nodes)) {", "      if (0 && gr.nodes > r->max_nodes - (r->grammar_nodes < r->max_nodes ? r->grammar_nodes : r->max_nodes)) {", ["registry"]),
    ("registry-exists-is-not-refused", "css_registry.c", "  if (found && !override) {", "  if (found && 0 && !override) {", ["registry"]),
    ("registry-override-is-not-reported", "css_registry.c", "    r->overrides[r->noverrides].id = id;", "    r->overrides[r->noverrides].id = 0 * id;", ["registry"]),
    ("registry-caller-names-are-not-folded", "css_registry.c", "  lower_copy(lname, name, name_length);\n  r->work += name_length;", "  r->work += name_length;", ["registry"]),
    ("registry-types-of-other-properties-are-not-found", "css_registry.c", "  return others && other != (size_t)-1 && !other_ambiguous", "  return 0 && others && other != (size_t)-1 && !other_ambiguous", ["registry", "property"]),
    ("registry-own-name-in-an-override-is-the-override", "css_registry.c", "          if (memcmp(tmp, lname, nlen) == 0) {", "          if (0 && memcmp(tmp, lname, nlen) == 0) {", ["registry"]),
    ("registry-failed-definition-is-answered-by-another-source", "css_registry.c", "  if (r->rf.status != GTEXT_CSS_OK || gtext_css_function_types_status(r->ftypes) != GTEXT_CSS_OK) {\n    /* a definition of the pin could not be made",
     "  if (0) {\n    /* a definition of the pin could not be made", ["registry-oom"]),
    ("registry-out-of-memory-is-remembered-as-a-bad-grammar", "css_registry.c", "  if (st == GTEXT_CSS_E_OOM || st == GTEXT_CSS_E_INVALID) {\n    return st;\n  }\n  lz->def_state = 2;",
     "  if (st == GTEXT_CSS_E_INVALID) {\n    return st;\n  }\n  lz->def_state = 2;", ["registry-oom"]),
    ("registry-prepared-state-is-not-published", "css_registry.c", "  gcu_atomic_int_store(&lz->state, (int32_t)final_state);", "  gcu_atomic_int_store(&lz->state, (int32_t)GTEXT_CSS_GRAMMAR_NOT_COMPILED);", ["registry"]),
    ("registry-an-alias-is-not-followed", "css_registry.c", "      next = current_id(registry, (GTEXT_CSS_Property_Id)gtext_css_tbl_properties[id - 1].alias_of);", "      next = 0;", ["registry", "property"]),
    ("property-wide-keywords-are-not-recognized", "css_property.c", "  if (wide != GTEXT_CSS_WIDE_NONE) {", "  if (0) {", ["property"]),
    ("property-all-takes-anything", "css_property.c", "  if (is_all(&info)) {", "  if (0 && is_all(&info)) {", ["property"]),
    ("property-quirks-are-always-on", "css_property.c", "options->quirks && (info.flags & GTEXT_CSS_PROP_QUIRK_UNITLESS) != 0, info.name_length", "(info.flags & GTEXT_CSS_PROP_QUIRK_UNITLESS) != 0, info.name_length", ["property"]),
    ("property-quirks-are-for-every-property", "css_property.c", "options->quirks && (info.flags & GTEXT_CSS_PROP_QUIRK_UNITLESS) != 0, info.name_length", "options->quirks, info.name_length", ["property"]),
    ("property-an-alias-is-parsed-as-itself", "css_property.c", "  const GTEXT_CSS_Property_Id target = gtext_css_registry_resolve_alias(registry, named);", "  const GTEXT_CSS_Property_Id target = named;", ["property"]),
    ("property-descriptors-take-substitutions", "css_property.c", "  if (sub) {\n    out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;\n    return GTEXT_CSS_OK;\n  }\n  GTEXT_CSS_Grammar_State state;",
     "  if (0) {\n    out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;\n    return GTEXT_CSS_OK;\n  }\n  GTEXT_CSS_Grammar_State state;", ["property"]),
    ("property-an-undecidable-property-is-dropped", "css_property.c", "    v->undecided++;\n    valid = true;", "    v->undecided++;\n    valid = false;", ["property"]),
    ("property-a-limit-is-not-recorded", "css_property.c", "  else if (v->status == GTEXT_CSS_OK) {\n    v->status = st;\n  }", "  else {\n  }", ["property", "registry-oom"]),
    ("property-the-pending-grammar-is-compiled-per-call", "css_registry.c", "  const GTEXT_CSS_Status st = compile_lazy(r, &((GTEXT_CSS_Registry *)r)->pending, text, sizeof text - 1, 0, &s, compiled, NULL);",
     "  const GTEXT_CSS_Status st = compile_lazy(r, &((GTEXT_CSS_Registry *)r)->pending, \"<nosuch-type>\", 13, 0, &s, compiled, NULL);", ["property"]),
]


def fuzz_plants():
    """The plants of the grammar and values fuzzers, as plants of the seed-replay programs."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("plant_fuzz", os.path.join(HERE, "plant-fuzz.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    out = []
    for name, (rel, old, new, _seed) in module.GRAMMAR_PLANTS.items():
        out.append(("fuzz-" + name, rel, old, new, ["grammar-seeds"]))
    for name, (rel, old, new, _seed) in module.VALUES_PLANTS.items():
        out.append(("fuzz-" + name, rel, old, new, ["values-seeds"]))
    for name, (rel, old, new, _seed) in module.REGISTRY_PLANTS.items():
        out.append(("fuzz-" + name, rel, old, new, ["registry-seeds"]))
    return out


PLANTS += fuzz_plants()


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
