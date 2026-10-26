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
"""Plant a defect in a copy of a CSS source file and show the CSS Syntax and CSS bytes fuzzers find it.

    PREFIX=... tools/css/plant-fuzz.py [--time SECONDS] [name ...]

A fuzzer that has never failed has not been shown to be able to. Each plant is one edit to one file of
src/css/, made in a scratch copy of that file (the working tree is never touched). The copy is compiled
with the fuzz build's flags and linked into the harness the plant is for (tests/fuzz/fuzz_css_syntax.cpp, or
fuzz_css_bytes.cpp for the plants of BYTES_PLANTS) ahead of the fuzz build's other objects, so a plant costs one
compilation, not a build. The fuzz builds themselves (`make fuzz-css-syntax fuzz-css-bytes`) are made first, in
this tree's build directory.

A plant with a seed that shows its defect is found by replaying that seed (-runs=0), which is the same
every time; one with no seed is searched for from the corpus for the time budget (--time, 60 seconds by
default) with a fixed libFuzzer seed, and so is a plant whose seed does not show its defect any more (the
code the seed reached has moved), which is reported as such ("SEARCHED, seed ... is stale"): finding it
that way passes, and is a sign to record a new seed. Either way the fuzzer must stop with a report of
its own (a property of the harness, a sanitizer, libFuzzer), and the **control**, the unedited file, must
not.

  ids-skip-one              ids are given with a gap: the harness's id walk.
  span-before-start         a token's span ends before it starts.
  original-text-raw         a custom property's original text is copied raw, not preprocessed: a NUL
                            or ill-formed UTF-8 in it.
  no-comment-between-tokens the serializer never separates two tokens: the round trip.
  whitespace-not-collapsed  the serializer writes two whitespace tokens: the second serialization.
  token-limit-off-by-one    the token limit lets one more through: the limits at what was counted.
  depth-limit-off-by-one    the same for depth: the depth of the tree.
  oom-ignored               an allocation that fails while values are taken is not a failure: the
                            answer changes. Which allocation is the one depends on the input, so the
                            seed is a recorded find (reproducer-oom-while-taking-values.seed: the
                            19th allocation of a wide declaration block, the harness's third byte);
                            the search from the corpus is the second path, not the first.
  arena-leak                the tree's last block of memory is not freed: the allocator.
  speculation-disagrees     the declaration attempt calls a value with a block after a token a
                            declaration: building it then disagrees, which is an internal error.
  cdc-after-dashes          the serializer does not separate the ident "--" from a ">": CDC on reading back (a find
                            of the fuzzer, kept as reproducer-cdc-after-dashes-ident.seed).
  anb-plus-sign             9.1 writes "-" where it means "+": An+B read back.

The plants of fuzz_css_bytes.cpp (BYTES_PLANTS; each is found by replaying its seed from tests/fuzz/corpus/css-bytes):

  map-end-past-code-point   a span's end is mapped to the byte after the one that ends the code point: the
                            positions are in the bytes.
  shift-forgotten           UTF-8 with a byte order mark is read in place without moving its offsets past
                            the mark.
  window-short              a stream decides the encoding one byte early: the answer depends on the cut.
  run-ignores-gap           the map joins code points that the source has bytes between (a redundant shift
                            sequence between two characters of the same width).
  limit-off-by-one          the stream refuses the byte that reaches the input limit.
  flush-forgotten           a stream never ends its decoder: a sequence cut off at the end is not replaced.
  block-index-off           the map's block index sends every lookup past the first 4 KiB to the first runs.

The plants of fuzz_css_values.cpp (VALUES_PLANTS; each is found by replaying its seed from tests/fuzz/corpus/css-values):

  math-node-bound-off-by-one the node bound refuses one node early: the bound is exact.
  math-leaks-its-arena       a calculation's memory is not freed: the allocator.
  color-leaks-its-arena      the same for a color.
  alpha-dropped              the legacy serializer writes a small alpha that is not an 8-bit one as 1: the color read back is another color.
  linear-points-unsorted     linear() does not raise an input below an earlier one: the points are in order.

The plants of fuzz_css_registry.cpp (REGISTRY_PLANTS; each is found by replaying its seed from tests/fuzz/corpus/css-registry):

  property-value-leaks       gtext_css_property_value_free() does not free the typed value: the allocator.
  all-takes-substitution     `all` accepts a value with var() in it: it takes the CSS-wide keywords only.
  descriptor-takes-substitution a descriptor accepts var(): a descriptor is not a property of an element.
  initial-not-validated      a registration does not match the initial value against the grammar: it must parse as a valid value.
  registry-leaks-initial     freeing a registry leaves the initial values of the registrations: the allocator.
  override-cannot-name-itself the grammar of an override naming its own property is left recursion: it extends the one it replaces.
  failed-definition-answered-by-another-source an allocation that fails while a definition is made lets a later source answer: the status.

The plants of fuzz_css_compute.cpp (COMPUTE_PLANTS; each is found by replaying its seed from tests/fuzz/corpus/css-compute):

  keyword-shorthand-wrong-form  `white-space: pre` is written as `pre-wrap`: the cssText is read back as other longhands.
  grid-template-names-dropped   the areas form of grid-template is written without its line names: the same.
  pending-longhand-written-empty a longhand declared with var() or attr() is written with no value: the cssText cannot be read back.
  context-leaks-its-groups      freeing a compute context leaves the table of logical groups: the allocator.

The plants of fuzz_css_grammar.cpp (GRAMMAR_PLANTS; each is found by replaying its seed from tests/fuzz/corpus/css-grammar):

  work-bound-off-by-one     a match stops one unit of work early: the bounds are exact.
  memo-bound-off-by-one     the same for the memo entries.
  value-count-wrong         a value set does not count one of its values.
  matcher-forgets-the-set   the grammar hook answers and keeps nothing: it disagrees with the direct call.
  match-leaks-its-pool      the match does not free its memo's intervals: the allocator.
  limit-status-is-internal  the work bound reports an internal error instead of its own status.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "css-syntax")
BYTES_CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "css-bytes")
GRAMMAR_CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "css-grammar")
VALUES_CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "css-values")
REGISTRY_CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "css-registry")
COMPUTE_CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "css-compute")

# name: (file under src/css/, old text (exactly once), new text, seed to replay or None to search)
PLANTS = {
    "ids-skip-one": ("css_parser.c", "      v->id = next++;\n", "      v->id = next++;\n      next += v->type == GTEXT_CSS_TOKEN_COLON;\n",
                     "own-nesting.seed"),
    "span-before-start": ("css_tokenizer.c", "  tok->end = t->pos;\n  return ok", "  tok->end = t->pos ? t->pos - 1 : 0;\n  tok->start = t->pos;\n  return ok",
                          "own-every-token.seed"),
    "original-text-raw": ("css_tokenizer.c", "    if (!css_buf_put_cp(ctx, sb, (uint32_t)cp)) {\n      return false;\n    }\n  }\n  char * s",
                          "    (void)cp;\n    if (!css_buf_put(ctx, sb, (const char *)ctx->in + t.pos - n, n)) {\n      return false;\n    }\n  }\n  char * s",
                          "own-custom-nul.seed"),
    "no-comment-between-tokens": ("css_serialize.c", '    puts_(w, "/**/");\n', "    (void)0;\n", "component-value-list-001.seed"),
    "whitespace-not-collapsed": ("css_serialize.c", "&& w->last == R_WS) {", "&& w->last == R_WS && 0) {", "own-two-whitespace.seed"),
    "token-limit-off-by-one": ("css_tokenizer.c", "if (out->len >= ctx->max_tokens) {", "if (out->len > ctx->max_tokens) {", "own-nesting.seed"),
    "depth-limit-off-by-one": ("css_tokenizer.c", "if (open.len >= ctx->max_depth) {", "if (open.len > ctx->max_depth) {", "own-nesting.seed"),
    "oom-ignored": ("css_parser.c", "  GTEXT_CSS_Component_Value * a = css_arena_alloc(&p->ctx->arena, count * sizeof *a);\n  if (!a) {\n    css_fail(p->ctx, GTEXT_CSS_E_OOM);\n    return false;",
                    "  GTEXT_CSS_Component_Value * a = css_arena_alloc(&p->ctx->arena, count * sizeof *a);\n  if (!a) {\n    p->values.len = base;\n    return true;",
                    "reproducer-oom-while-taking-values.seed"),
    "arena-leak": ("css_core.c", "    gtext_allocator_free(&arena->alloc, c);\n", "    if (next) {\n      gtext_allocator_free(&arena->alloc, c);\n    }\n",
                   "own-wide.seed"),
    "speculation-disagrees": ("css_parser.c", "    else if (block) {\n      return false;\n    }\n", "", "own-decl-block.seed"),
    "cdc-after-dashes": ("css_serialize.c", "    case '>': return C_GT;\n", "", "reproducer-cdc-after-dashes-ident.seed"),
    "anb-plus-sign": ("css_anb.c", "      out[n++] = '+';\n", "      out[n++] = '-';\n", "AnplusB-002.seed"),
}


# The same shape, for tests/fuzz/fuzz_css_bytes.cpp; the seeds are in tests/fuzz/corpus/css-bytes.
BYTES_PLANTS = {
    "map-end-past-code-point": ("css_core.c", "  return r->src + ((dec - 1 - r->dec) / r->dlen) * r->width + r->width;\n",
                                "  return r->src + ((dec - 1 - r->dec) / r->dlen) * r->width + r->width + 1;\n", "own-cp1251-charset.seed"),
    "shift-forgotten": ("css_decode.c", "    shift.shift = dec.bom_length;\n", "    shift.shift = 0;\n", "own-utf8-bom.seed"),
    "window-short": ("css_stream.c", "s->raw_len >= GTEXT_CSS_ENCODING_WINDOW)", "s->raw_len >= GTEXT_CSS_ENCODING_WINDOW - 1)",
                     "own-charset-ends-1024.seed"),
    "run-ignores-gap": ("css_decode.c", "        && last->src + (size_t)last->count * width == src) {", "        ) {", "own-iso2022jp-redundant-shifts.seed"),
    "limit-off-by-one": ("css_stream.c", "  if (length > s->max_input - s->fed) {", "  if (length >= s->max_input - s->fed) {", "own-utf8.seed"),
    "flush-forgotten": ("css_stream.c", "    if (!css_decode_feed(&s->d, NULL, 0, true)) {", "    if (0) {", "own-sjis-truncated.seed"),
    "block-index-off": ("css_decode.c", "    m->blocks[b] = r;\n", "    m->blocks[b] = b ? 0 : r;\n", "own-cp1251-huge.seed"),
}


# The same shape, for tests/fuzz/fuzz_css_grammar.cpp; the seeds are in tests/fuzz/corpus/css-grammar.
GRAMMAR_PLANTS = {
    "work-bound-off-by-one": ("css_grammar_match.c", "  if (m->work > m->max_work) {", "  if (m->work >= m->max_work) {", "own-bounds.seed"),
    "memo-bound-off-by-one": ("css_grammar_match.c", "  if (n > m->max_memo - m->memo) {", "  if (n >= m->max_memo - m->memo) {", "own-bounds.seed"),
    "value-count-wrong": ("css_grammar_match.c", "  B->vals[B->nvals++] = *v;\n  B->set->count++;", "  B->vals[B->nvals++] = *v;", "own-bounds.seed"),
    "matcher-forgets-the-set": ("css_grammar_match.c", "  gtext_css_value_set_free(mt->sets[index]);\n  mt->sets[index] = set;",
                                "  gtext_css_value_set_free(mt->sets[index]);\n  gtext_css_value_set_free(set);\n  mt->sets[index] = NULL;", "own-hook.seed"),
    "match-leaks-its-pool": ("css_grammar_match.c", "  gtext_allocator_free(&m->alloc, m->pool);\n", "", "own-completes-under-the-fault-allocator.seed"),
    "limit-status-is-internal": ("css_grammar_match.c", "    if (m->status == GTEXT_CSS_OK) {\n      m->status = GTEXT_CSS_E_LIMIT_WORK;\n    }\n    return false;\n  }\n  return true;\n}\n\nstatic void fail_oom",
                                 "    if (m->status == GTEXT_CSS_OK) {\n      m->status = GTEXT_CSS_E_INTERNAL;\n    }\n    return false;\n  }\n  return true;\n}\n\nstatic void fail_oom", "own-bounds.seed"),
}


# The same shape, for tests/fuzz/fuzz_css_values.cpp; the seeds are in tests/fuzz/corpus/css-values.
VALUES_PLANTS = {
    "math-node-bound-off-by-one": ("css_math_simplify.c", "  if (mb->nodes >= mb->max_nodes) {", "  if (mb->nodes > mb->max_nodes) {", "own-nodes-exact.seed"),
    "math-leaks-its-arena": ("css_math.c", "  GTEXT_Allocator alloc = ow->alloc;\n  css_arena_free(&ow->arena);\n  gtext_allocator_free(&alloc, ow);\n}\n\n",
                             "  GTEXT_Allocator alloc = ow->alloc;\n  gtext_allocator_free(&alloc, ow);\n}\n\n", "own-math-no-fault.seed"),
    "color-leaks-its-arena": ("css_color.c", "  css_arena_free(&ow->arena);\n  gtext_allocator_free(&alloc, ow);\n}", "  gtext_allocator_free(&alloc, ow);\n}",
                              "own-color-no-fault.seed"),
    "alpha-dropped": ("css_color_serialize.c", "    return put_num(b, gmath_floor(quotient + 0.5) / 1000.0);\n  }\n  return put_num(b, a);\n}",
                      "    return put_num(b, gmath_floor(quotient + 0.5) / 1000.0);\n  }\n  return put_num(b, a < 0.5 ? 1.0 : a);\n}", "own-alpha.seed"),
    "linear-points-unsorted": ("css_easing.c", "if (!first && pts[i].input < run_max) {", "if (!first && pts[i].input < run_max && 0) {",
                               "own-linear-unsorted.seed"),
}


# The same shape, for tests/fuzz/fuzz_css_registry.cpp; the seeds are in tests/fuzz/corpus/css-registry (tools/css/make-fuzz-registry-seeds.py).
REGISTRY_PLANTS = {
    "property-value-leaks": ("css_property.c", "  gtext_css_value_set_free(value->set);\n  memset(value, 0, sizeof *value);",
                             "  memset(value, 0, sizeof *value);", "own-valid-margin.seed"),
    "all-takes-substitution": ("css_property.c", "  if (is_all(&info)) {", "  if (0 && is_all(&info)) {", "own-all-var.seed"),
    "descriptor-takes-substitution": ("css_property.c", "  if (sub) {\n    out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;\n    return GTEXT_CSS_OK;\n  }\n  GTEXT_CSS_Grammar_State state;",
                                      "  if (0) {\n    out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;\n    return GTEXT_CSS_OK;\n  }\n  GTEXT_CSS_Grammar_State state;", "own-descriptor-var.seed"),
    "initial-not-validated": ("css_registry.c", "      result = st != GTEXT_CSS_OK ? st : (matched ? GTEXT_CSS_OK : GTEXT_CSS_E_INVALID);",
                              "      result = st != GTEXT_CSS_OK ? st : GTEXT_CSS_OK;", "own-register-bad-initial.seed"),
    "registry-leaks-initial": ("css_registry.c", "    gtext_allocator_free(&a, c->initial);\n    gtext_allocator_free(&a, c->longhands);\n  }\n  gtext_css_function_types_free(r->ftypes);",
                               "    gtext_allocator_free(&a, c->longhands);\n  }\n  gtext_css_function_types_free(r->ftypes);", "own-register-valid.seed"),
    "override-cannot-name-itself": ("css_registry.c", "          if (memcmp(tmp, lname, nlen) == 0) {", "          if (0 && memcmp(tmp, lname, nlen) == 0) {",
                                    "own-register-override-self.seed"),
    "failed-definition-answered-by-another-source": ("css_registry.c", "  if (r->rf.status != GTEXT_CSS_OK || gtext_css_function_types_status(r->ftypes) != GTEXT_CSS_OK) {\n    /* a definition of the pin could not be made",
                                                      "  if (0) {\n    /* a definition of the pin could not be made", None),
}


# The same shape, for tests/fuzz/fuzz_css_compute.cpp; the seeds are in tests/fuzz/corpus/css-compute (tools/css/make-fuzz-compute-seeds.py).
COMPUTE_PLANTS = {
    "keyword-shorthand-wrong-form": ("css_shorthand_serialize.c", '    {"white-space", "white-space-collapse", "preserve", "text-wrap-mode", "nowrap", "pre"},',
                                     '    {"white-space", "white-space-collapse", "preserve", "text-wrap-mode", "nowrap", "pre-wrap"},', "own-white-space-pre.seed"),
    "grid-template-names-dropped": ("css_grid_text.c", "      if (any) {\n        if (!first) {\n          tb_put(&b, \" \", 1);\n        }\n        tb_put(&b, \"[\", 1);",
                                    "      if (0) {\n        if (!first) {\n          tb_put(&b, \" \", 1);\n        }\n        tb_put(&b, \"[\", 1);", "own-grid-template-names.seed"),
    "pending-longhand-written-empty": ("css_shorthand_serialize.c", "    if (it->outcome == GTEXT_CSS_PROP_RESULT_PENDING && it->shorthand) {",
                                       "    if (it->outcome == GTEXT_CSS_PROP_RESULT_PENDING) {", "own-pending-longhand.seed"),
    "context-leaks-its-groups": ("css_compute.c", "  gtext_allocator_free(&a, cc->groups);\n", "", "own-context-fails-early.seed"),
}


def make_variables(prefix, names):
    """The Makefile's own values for `names`, so that nothing here is a second copy of them."""
    rules = "print-%: ; @echo $($*)\n"
    out = {}
    for name in names:
        done = subprocess.run(["make", "-s", "-C", ROOT, "-f", "Makefile", "-f", "-", "PREFIX=" + prefix, "print-" + name],
                              input=rules, capture_output=True, text=True)
        out[name] = done.stdout.strip()
        if done.returncode != 0:
            raise SystemExit("could not read %s from the Makefile: %s" % (name, done.stderr.strip()[-300:]))
    return out


REPORT = re.compile(r"^(fuzz_css_(syntax|bytes|grammar|values|registry|compute): .+|==\d+==ERROR: (AddressSanitizer|LeakSanitizer|libFuzzer).*|"
                    r".*runtime error: .*|SUMMARY: (AddressSanitizer|libFuzzer|UndefinedBehaviorSanitizer|LeakSanitizer).*)$", re.M)


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
    CONTROLS = ("control", "control-bytes", "control-grammar", "control-values", "control-registry", "control-compute")
    names = names or list(CONTROLS) + list(PLANTS) + list(BYTES_PLANTS) + list(GRAMMAR_PLANTS) + list(VALUES_PLANTS) + list(REGISTRY_PLANTS) + list(COMPUTE_PLANTS)
    for n in names:
        if n not in CONTROLS and n not in PLANTS and n not in BYTES_PLANTS and n not in GRAMMAR_PLANTS and n not in VALUES_PLANTS and n not in REGISTRY_PLANTS and n not in COMPUTE_PLANTS:
            raise SystemExit("unknown plant %r (one of %s, %s)"
                             % (n, ", ".join(CONTROLS), ", ".join(list(PLANTS) + list(BYTES_PLANTS) + list(GRAMMAR_PLANTS) + list(VALUES_PLANTS) + list(REGISTRY_PLANTS) + list(COMPUTE_PLANTS))))
    prefix = os.environ.get("PREFIX")
    if not prefix:
        raise SystemExit("PREFIX must be set; the Makefile (check-css-fuzz-plants) passes it")
    env = dict(os.environ, PKG_CONFIG_PATH=os.path.join(prefix, "share", "pkgconfig"))
    built = subprocess.run(["make", "-C", ROOT, "-j4", "fuzz-css-syntax", "fuzz-css-bytes", "fuzz-css-grammar", "fuzz-css-values", "fuzz-css-registry", "fuzz-css-compute", "PREFIX=" + prefix], env=env, capture_output=True, text=True)
    if built.returncode != 0:
        sys.stderr.write(built.stdout[-2000:] + built.stderr[-2000:])
        raise SystemExit("the fuzz build failed")
    v = make_variables(prefix, ("FUZZ_CC", "FUZZ_CXX", "FUZZ_LIB_FLAGS", "FUZZ_BIN_FLAGS", "INCLUDE", "FUZZ_OBJECTS", "DEP_LIBS", "FUZZ_RPATH",
                                "FUZZ_OBJ_DIR"))
    objects = [o if os.path.isabs(o) else os.path.join(ROOT, o) for o in v["FUZZ_OBJECTS"].split()]
    include = ["-I" + os.path.join(ROOT, f[2:]) if f.startswith("-I") and not os.path.isabs(f[2:]) else f
               for f in v["INCLUDE"].replace("-I ", "-I").split()]
    bad = []
    scratch = tempfile.mkdtemp(prefix="css-plant-fuzz-", dir=os.environ.get("TMPDIR", tempfile.gettempdir()))
    try:
        for name in names:
            work = os.path.join(scratch, name)
            os.makedirs(work)
            objs = list(objects)
            seed = None
            bytes_harness = name == "control-bytes" or name in BYTES_PLANTS
            grammar_harness = name == "control-grammar" or name in GRAMMAR_PLANTS
            values_harness = name == "control-values" or name in VALUES_PLANTS
            registry_harness = name == "control-registry" or name in REGISTRY_PLANTS
            compute_harness = name == "control-compute" or name in COMPUTE_PLANTS
            harness = "fuzz_css_compute" if compute_harness else "fuzz_css_registry" if registry_harness else "fuzz_css_values" if values_harness else "fuzz_css_grammar" if grammar_harness else "fuzz_css_bytes" if bytes_harness else "fuzz_css_syntax"
            corpus_source = COMPUTE_CORPUS if compute_harness else REGISTRY_CORPUS if registry_harness else VALUES_CORPUS if values_harness else GRAMMAR_CORPUS if grammar_harness else BYTES_CORPUS if bytes_harness else CORPUS
            if name not in CONTROLS:
                rel, old, new, seed = COMPUTE_PLANTS[name] if compute_harness else REGISTRY_PLANTS[name] if registry_harness else VALUES_PLANTS[name] if values_harness else GRAMMAR_PLANTS[name] if grammar_harness else BYTES_PLANTS[name] if bytes_harness else PLANTS[name]
                src = os.path.join(ROOT, "src", "css", rel)
                text = open(src, encoding="utf-8").read()
                if text.count(old) != 1:
                    raise SystemExit("plant %s: its anchor matches %d times in %s" % (name, text.count(old), rel))
                # The copy sits beside the original's includes: a directory of the same shape.
                os.makedirs(os.path.join(work, "src", "css"))
                for f in os.listdir(os.path.join(ROOT, "src", "css")):
                    if f.endswith(".h"):
                        shutil.copy(os.path.join(ROOT, "src", "css", f), os.path.join(work, "src", "css", f))
                shutil.copy(os.path.join(ROOT, "src", "text_number_internal.h"), os.path.join(work, "src"))
                planted = os.path.join(work, "src", "css", rel)
                open(planted, "w", encoding="utf-8").write(text.replace(old, new))
                obj = os.path.join(work, rel[:-2] + ".o")
                done = subprocess.run([v["FUZZ_CC"]] + v["FUZZ_LIB_FLAGS"].split() + ["-std=c17", "-w"] + include + ["-c", planted, "-o", obj],
                                      env=env, capture_output=True, text=True)
                if done.returncode != 0:
                    raise SystemExit("plant %s did not compile:\n%s" % (name, done.stderr[-2000:]))
                original = os.path.join(ROOT, v["FUZZ_OBJ_DIR"], "css", rel[:-2] + ".o")
                objs = [obj if os.path.abspath(o) == os.path.abspath(original) else o for o in objs]
                if obj not in objs:
                    raise SystemExit("plant %s: %s is not among the fuzz objects" % (name, original))
            app = os.path.join(work, harness)
            done = subprocess.run([v["FUZZ_CXX"]] + v["FUZZ_BIN_FLAGS"].split() + ["-std=c++20", "-w"] + include
                                  + ["-o", app, os.path.join(ROOT, "tests", "fuzz", harness + ".cpp")] + objs
                                  + v["DEP_LIBS"].split() + v["FUZZ_RPATH"].split(), env=env, capture_output=True, text=True)
            if done.returncode != 0:
                raise SystemExit("plant %s did not link:\n%s" % (name, done.stderr[-2000:]))
            corpus = os.path.join(work, "corpus")
            shutil.copytree(corpus_source, corpus, ignore=lambda d, ns: [n for n in ns if not n.endswith(".seed")])
            search = [app, corpus, "-max_total_time=%d" % seconds, "-seed=1", "-max_len=2048", "-rss_limit_mb=2048"]

            def run_fuzzer(cmd):
                try:
                    run = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=seconds * 3 + 300, cwd=work)
                    return run.returncode, run.stdout + run.stderr
                except subprocess.TimeoutExpired:
                    return -1, "timed out"

            how = "replayed %s" % seed if seed else "searched"
            if seed:
                if not os.path.exists(os.path.join(corpus, seed)):
                    raise SystemExit("plant %s: its seed %s is not in %s" % (name, seed, corpus_source))
                status, out = run_fuzzer([app, "-runs=0", os.path.join(corpus, seed)])
                if status in (0, -1) or not any(REPORT.match(l) for l in out.splitlines()):
                    sys.stderr.write("plant %s: its seed %s no longer shows the defect; searching\n" % (name, seed))
                    status, out = run_fuzzer(search)
                    how = "SEARCHED, seed %s is stale" % seed
            elif name in CONTROLS:
                status, out = run_fuzzer([app, "-runs=0", corpus])
            else:
                status, out = run_fuzzer(search)
            if name in CONTROLS:
                ok = status == 0
                print("%s (no edit): %s" % (name, "silent over the whole corpus, as it must be" if ok else "FAILED (exit %d)" % status))
                if not ok:
                    sys.stderr.write(out[-3000:])
            else:
                line = next((l for l in out.splitlines() if REPORT.match(l)), "")
                ok = status not in (0, -1) and bool(line)
                print("%-26s %s %s" % (name, ("found (%s)" % how) if ok else "NOT FOUND", line[:120]))
                if not ok:
                    sys.stderr.write("exit %s; the end of the output:\n%s\n" % (status, out[-1500:]))
            if not ok:
                bad.append(name)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    if bad:
        sys.stderr.write("planted-defect check failed for: %s\n" % ", ".join(bad))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
