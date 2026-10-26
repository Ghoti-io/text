/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * This library's side of the CSS property differential (tools/oracle/css_prop_diff.py), in three modes.
 *
 *   css-prop-ours                         answer the items on standard input, one a line: `<property> TAB <value>`, both written
 *                                         with \\ for a backslash, \n for a line feed and \t for a tab. The answer is
 *                                           OK TAB <serialization>    the value is a value of the property (a CSS-wide keyword and
 *                                                                     a value with var() in it are too, as `style.setProperty`
 *                                                                     keeps them), and how this library writes it
 *                                           NO                        it is not
 *                                           UNDECIDED                 the property is known and the pin gives no grammar to judge it
 *                                           UNKNOWN                   no such property
 *   css-prop-ours --properties            one line a property of the registry: name, then SHORTHAND or LONGHAND (what the property
 *                                         is, an alias being what its target is), then DECIDED or UNDECIDED, then the property the
 *                                         alias names (or `-`), then the pin's file the definition came from (or `-`)
 *   css-prop-ours --sample SEED PER [P..] for each property (the named ones, or every decidable one), the value that is the
 *                                         cheapest sentence of its compiled grammar (tag `witness`) and PER sentences of a seeded
 *                                         random walk over the grammar (tag `walk`): the alternatives, the repetitions and the
 *                                         order of a `&&` and a `||` are chosen at random, and a number is drawn from a few common
 *                                         ones and from the boundaries of the range the grammar gives it (the bound, one past it and
 *                                         the middle), so that the edges of every range are in the population. One line:
 *                                         `<property> TAB <value> TAB <tag>`.
 *   css-prop-ours --edges LEVEL [P..]     the witness of each property with one of its numbers replaced by an edge of its range: the bound,
 *                                         one past it, the middle, zero, the extremes (level 1: six edges of up to two numbers; level 2:
 *                                         twelve edges of up to three, and the number 1 in each unit of the first number's type).
 *                                         One line: `<property> TAB <value> TAB edge`.
 *
 * The sentences are written from the compiled grammar, so they are values of the property by construction up to the places where
 * the grammar's text and the matcher disagree, which is what the differential measures. A random walk is a function of its seed
 * (xorshift64*, no libc state), so a population is reproduced from SEED and PER and the pin.
 */

#include <ghoti.io/text/css.h>

#include "../../src/css/css_grammar_internal.h"
#include "../../src/css/css_registry_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- escapes ---------------------------------------------------------------------------------- */

static char * unescape(const char * s, size_t n, size_t * out_len) {
  char * r = malloc(n + 1);
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\' && i + 1 < n) {
      i++;
      r[k++] = s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
    }
    else {
      r[k++] = s[i];
    }
  }
  r[k] = '\0';
  *out_len = k;
  return r;
}

static void escape_out(const char * s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\') {
      fputs("\\\\", stdout);
    }
    else if (s[i] == '\n') {
      fputs("\\n", stdout);
    }
    else if (s[i] == '\t') {
      fputs("\\t", stdout);
    }
    else {
      fputc(s[i], stdout);
    }
  }
}

/* ---- a growable string -------------------------------------------------------------------------- */

typedef struct {
  char * data;
  size_t len, cap;
  int failed;
} sbuf;

static void sb_put(sbuf * b, const char * s, size_t n) {
  if (b->failed) {
    return;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + n + 1) {
      cap *= 2;
    }
    char * g = realloc(b->data, cap);
    if (!g) {
      b->failed = 1;
      return;
    }
    b->data = g;
    b->cap = cap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void sb_puts(sbuf * b, const char * s) {
  sb_put(b, s, strlen(s));
}

/* ---- answering ---------------------------------------------------------------------------------- */

static void answer(const GTEXT_CSS_Registry * reg, const char * name, size_t nlen, const char * value, size_t vlen) {
  GTEXT_CSS_Property_Value pv;
  memset(&pv, 0, sizeof pv);
  const GTEXT_CSS_Status st = gtext_css_parse_property_text(reg, name, nlen, value, vlen, NULL, &pv);
  if (st == GTEXT_CSS_E_UNSUPPORTED) {
    puts("UNDECIDED");
  }
  else if (st != GTEXT_CSS_OK) {
    printf("ERR\t%s\n", gtext_css_status_string(st));
  }
  else if (pv.outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN) {
    puts("UNKNOWN");
  }
  else if (pv.outcome == GTEXT_CSS_PROP_RESULT_INVALID) {
    puts("NO");
  }
  else {
    char * text = NULL;
    size_t tlen = 0;
    if (gtext_css_property_value_serialize_buffer(&pv, NULL, &text, &tlen) != GTEXT_CSS_OK) {
      puts("ERR\tserialize");
    }
    else {
      fputs("OK\t", stdout);
      escape_out(text, tlen);
      fputc('\n', stdout);
      gtext_css_serialize_buffer_free(NULL, text);
    }
  }
  gtext_css_property_value_free(&pv);
}

static int run_answers(const GTEXT_CSS_Registry * reg) {
  char * line = NULL;
  size_t cap = 0;
  ssize_t len;
  while ((len = getline(&line, &cap, stdin)) >= 0) {
    if (len > 0 && line[len - 1] == '\n') {
      line[--len] = '\0';
    }
    char * tab = memchr(line, '\t', (size_t)len);
    if (!tab) {
      puts("BAD");
      fflush(stdout);
      continue;
    }
    size_t nlen, vlen;
    char * name = unescape(line, (size_t)(tab - line), &nlen);
    char * value = unescape(tab + 1, (size_t)len - (size_t)(tab + 1 - line), &vlen);
    answer(reg, name, nlen, value, vlen);
    free(name);
    free(value);
    fflush(stdout);
  }
  free(line);
  return 0;
}

/* ---- the properties ------------------------------------------------------------------------------ */

static int run_properties(const GTEXT_CSS_Registry * reg) {
  const size_t n = gtext_css_registry_property_count(reg);
  for (size_t i = 1; i <= n; i++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(reg, (GTEXT_CSS_Property_Id)i, &info)) {
      continue;
    }
    const GTEXT_CSS_Property_Id target = gtext_css_registry_resolve_alias(reg, (GTEXT_CSS_Property_Id)i);
    GTEXT_CSS_Property_Info tinfo;
    gtext_css_registry_property_info(reg, target, &tinfo);
    GTEXT_CSS_Grammar_State state = GTEXT_CSS_GRAMMAR_NONE;
    const GTEXT_CSS_Compiled * compiled = NULL;
    if (css_registry_property_compiled(reg, target, &state, &compiled, NULL) != GTEXT_CSS_OK) {
      fprintf(stderr, "compile %s\n", info.name);
      return 2;
    }
    printf("%s\t%s\t%s\t%s\t%s\n", info.name, (tinfo.flags & GTEXT_CSS_PROP_SHORTHAND) ? "SHORTHAND" : "LONGHAND",
        state == GTEXT_CSS_GRAMMAR_OK ? "DECIDED" : "UNDECIDED", (info.flags & GTEXT_CSS_PROP_ALIAS) ? tinfo.name : "-",
        tinfo.source ? tinfo.source : "-");
  }
  return 0;
}

/* ---- the random walk ------------------------------------------------------------------------------ */

typedef struct {
  uint64_t s;
} rng_t;

static uint64_t rnd(rng_t * r) {
  /* xorshift64* (Vigna): the state is the seed and nothing else */
  uint64_t x = r->s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  r->s = x;
  return x * UINT64_C(2685821657736338717);
}

static uint32_t below(rng_t * r, uint32_t n) {
  return n ? (uint32_t)(rnd(r) % n) : 0;
}

#define INF_SIZE (1u << 28)
#define MAX_DEPTH 24

typedef struct {
  const GTEXT_CSS_Compiled * c;
  uint32_t * size;   /**< Fewest tokens a node can be written in (INF_SIZE: it cannot be written). */
  rng_t * rng;       /**< NULL: the deterministic cheapest sentence. */
  int unsupported;
  sbuf out;
  unsigned numbers;      /**< Numeric natives written so far in this sentence. */
  int edge_at;           /**< The numeric native (counting from 0) to put an edge value in, or -1. */
  int edge_variant;      /**< Which edge value (see edge_number()); past EDGE_NUMBERS, the number 1 with the unit of that index. */
  int edge_hit;          /**< The last number written was the edge one. */
  unsigned lean;         /**< 0: any alternative; n: only alternatives not more than 2n tokens longer than the shortest (a sentence of the plain syntax). */
} walk;

static const char * const kLengthUnits[] = {"px", "em", "rem", "ex", "ch", "cm", "mm", "in", "pt", "pc", "Q", "vw", "vh", "vmin",
    "vmax", "svw", "dvh", "lvi", "cqw", "lh", "rlh", "cap", "ic", "rex"};
static const char * const kAngleUnits[] = {"deg", "grad", "rad", "turn"};
static const char * const kTimeUnits[] = {"s", "ms"};
static const char * const kFreqUnits[] = {"Hz", "kHz"};
static const char * const kResUnits[] = {"dppx", "dpi", "dpcm", "x"};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const char * const kColors[] = {"red", "#fff", "#ff00ff80", "rgb(1 2 3)", "rgba(1, 2, 3, .5)", "hsl(120 50% 50%)", "currentcolor",
    "transparent", "oklch(50% 0.1 120)", "color-mix(in srgb, red, blue)", "light-dark(red, blue)", "lab(50 20 30)", "hwb(0 10% 20%)",
    "color(srgb 1 0 0 / 50%)", "Canvas", "RED", "rgb(from red r g b)", "#abc"};
static const char * const kIdents[] = {"foo", "bar-baz", "a1", "Foo", "_x", "none-of", "x\\ y"};
static const char * const kStrings[] = {"\"a\"", "'b'", "\"\"", "\"a b\"", "\"\\\"q\\\"\"", "\"\\41 \"", "'it''s'"};
static const char * const kUrls[] = {"url(a.png)", "url(\"b c.png\")", "url('d')", "url(  e  )"};
static const char * const kAny[] = {"x", "a b", "(a)", "[b]", "{c}", "1px", "\"s\"", "f(g)", "var(--v)"};

static const char * const * units_of(gn_native t, size_t * n) {
  switch (t) {
  case NT_ANGLE: case NT_ANGLE_PERCENTAGE: *n = COUNT(kAngleUnits); return kAngleUnits;
  case NT_TIME: case NT_TIME_PERCENTAGE: *n = COUNT(kTimeUnits); return kTimeUnits;
  case NT_FREQUENCY: case NT_FREQUENCY_PERCENTAGE: *n = COUNT(kFreqUnits); return kFreqUnits;
  case NT_RESOLUTION: *n = COUNT(kResUnits); return kResUnits;
  default: *n = COUNT(kLengthUnits); return kLengthUnits;
  }
}

static const char * pick(walk * w, const char * const * list, size_t n) {
  return w->rng ? list[below(w->rng, (uint32_t)n)] : list[0];
}

static void put_double(sbuf * b, double v) {
  char buf[64];
  if (v != v) {
    sb_puts(b, "0");
    return;
  }
  if (v == (double)(long long)v && v > -1e15 && v < 1e15) {
    snprintf(buf, sizeof buf, "%lld", (long long)v);
  }
  else {
    snprintf(buf, sizeof buf, "%.9g", v);
  }
  sb_puts(b, buf);
}

/** The edge values of a numeric node, by variant: its bounds, one past them, the middle, zero and the extremes. */
#define EDGE_NUMBERS 12
static double edge_number(const gnode * n, int variant, int integer) {
  const int ranged = (n->flags & GNF_RANGE) != 0;
  const double lo = ranged ? n->lo : -1e300, hi = ranged ? n->hi : 1e300;
  const double eps = integer ? 1 : 0.001;
  double v;
  switch (variant % EDGE_NUMBERS) {
  case 0: v = lo > -1e15 ? lo : -1; break;
  case 1: v = lo > -1e15 ? lo - eps : -2; break;
  case 2: v = lo > -1e15 ? lo + eps : 0; break;
  case 3: v = hi < 1e15 ? hi : 1; break;
  case 4: v = hi < 1e15 ? hi + eps : 2; break;
  case 5: v = hi < 1e15 ? hi - eps : 3; break;
  case 6: v = (lo > -1e15 && hi < 1e15) ? (lo + hi) / 2 : 0.5; break;
  case 7: v = 0; break;
  case 8: v = integer ? 2147483647.0 : 1e20; break;
  case 9: v = integer ? -2147483648.0 : -1e20; break;
  case 10: v = integer ? 4294967296.0 : 1e-7; break;
  default: v = integer ? 1 : 123456789.123; break;
  }
  return integer ? (double)(long long)v : v;
}

/** A number for a (maybe ranged) node. */
static double pick_number(walk * w, const gnode * n, int integer) {
  w->edge_hit = 0;
  if (w->edge_at >= 0) {
    const unsigned k = w->numbers++;
    if ((int)k == w->edge_at) {
      w->edge_hit = 1;
      return w->edge_variant < EDGE_NUMBERS ? edge_number(n, w->edge_variant, integer) : (integer ? 1 : 1.0);
    }
  }
  static const double common[] = {0, 1, -1, 2, 0.5, -0.5, 10, 100, 360, 255, 1000, 0.1, 1.5, 1e-3, 65535, 2147483647.0, 2147483648.0, 1e20, -100};
  const int ranged = (n->flags & GNF_RANGE) != 0;
  double lo = ranged ? n->lo : -1e300, hi = ranged ? n->hi : 1e300;
  if (!w->rng) {
    double v = 1;
    if (ranged && v < lo) {
      v = lo;
    }
    if (ranged && v > hi) {
      v = hi;
    }
    if (v != v || v > 1e15 || v < -1e15) {
      v = 0;
    }
    return integer ? (double)(long long)v : v;
  }
  double cands[40];
  size_t nc = 0;
  for (size_t i = 0; i < COUNT(common); i++) {
    cands[nc++] = common[i];
  }
  if (ranged) {
    const double mid = (lo > -1e15 && hi < 1e15) ? (lo + hi) / 2 : 1;
    if (lo > -1e15) {
      cands[nc++] = lo;
      cands[nc++] = lo - 1;
      cands[nc++] = lo + 1;
      cands[nc++] = integer ? lo : lo - 0.001;
    }
    if (hi < 1e15) {
      cands[nc++] = hi;
      cands[nc++] = hi + 1;
      cands[nc++] = hi - 1;
      cands[nc++] = integer ? hi : hi + 0.001;
    }
    cands[nc++] = mid;
    cands[nc++] = mid;
  }
  double v = cands[below(w->rng, (uint32_t)nc)];
  if (integer) {
    v = (double)(long long)v;
  }
  /* mostly inside the range: a sentence that is outside is a boundary probe */
  if (ranged && (v < lo || v > hi) && below(w->rng, 4) != 0) {
    v = lo > -1e15 ? lo : (hi < 1e15 ? hi : 0);
  }
  return v;
}

/** The text of a native type's value: a word, or for a number the unit (the number itself is written to `w->out` first). NULL: none. */
static const char * native_word(walk * w, const gnode * n) {
  switch ((gn_native)n->native) {
  case NT_ZERO: return "0";
  case NT_STRING: case NT_TOK_STRING: return pick(w, kStrings, COUNT(kStrings));
  case NT_URL: case NT_TOK_URL: return pick(w, kUrls, COUNT(kUrls));
  case NT_IDENT: case NT_TOK_IDENT: case NT_CUSTOM_IDENT: return pick(w, kIdents, COUNT(kIdents));
  case NT_DASHED_IDENT: return w->rng && below(w->rng, 2) ? "--a-b" : "--foo";
  case NT_ANY_VALUE: case NT_DECL_VALUE: return pick(w, kAny, COUNT(kAny));
  case NT_URANGE: return w->rng && below(w->rng, 2) ? "U+??" : "U+0-7F";
  case NT_ANB: return w->rng && below(w->rng, 2) ? "odd" : "2n+1";
  case NT_DECL_LIST: case NT_RULE_LIST: return "";
  case NT_RATIO: return w->rng && below(w->rng, 3) == 0 ? "16/9" : "1 / 1";
  case NT_COLOR: return pick(w, kColors, COUNT(kColors));
  case NT_TOK_HASH: return "#zz";
  case NT_TOK_DELIM: return "/";
  case NT_TOK_NUMBER: return "1";
  case NT_TOK_PERCENTAGE: return "1%";
  case NT_TOK_DIMENSION: return "1zzunit";
  case NT_TOK_COLON: return ":";
  case NT_TOK_SEMICOLON: return ";";
  case NT_TOK_COMMA: return ",";
  case NT_INTEGER: case NT_NUMBER: {
    const int integer = n->native == NT_INTEGER;
    put_double(&w->out, pick_number(w, n, integer));
    return "";
  }
  case NT_PERCENTAGE: {
    put_double(&w->out, pick_number(w, n, 0));
    return "%";
  }
  case NT_LENGTH: case NT_ANGLE: case NT_TIME: case NT_FREQUENCY: case NT_RESOLUTION: case NT_FLEX:
  case NT_LENGTH_PERCENTAGE: case NT_ANGLE_PERCENTAGE: case NT_TIME_PERCENTAGE: case NT_FREQUENCY_PERCENTAGE:
  case NT_DIMENSION: {
    const double v = pick_number(w, n, 0);
    const int mixed = n->native == NT_LENGTH_PERCENTAGE || n->native == NT_ANGLE_PERCENTAGE || n->native == NT_TIME_PERCENTAGE ||
        n->native == NT_FREQUENCY_PERCENTAGE;
    put_double(&w->out, v);
    if (n->native == NT_FLEX) {
      return "fr";
    }
    if (mixed && w->rng && below(w->rng, 3) == 0) {
      return "%";
    }
    if (v == 0 && w->rng && below(w->rng, 3) == 0 && n->native == NT_LENGTH) {
      return ""; /* a unitless zero */
    }
    size_t un;
    const char * const * units = units_of((gn_native)n->native, &un);
    if (w->edge_hit && w->edge_variant >= EDGE_NUMBERS) {
      return units[(size_t)(w->edge_variant - EDGE_NUMBERS) % un];
    }
    return w->rng ? units[below(w->rng, (uint32_t)un)] : (mixed || n->native == NT_LENGTH ? "px" : units[0]);
  }
  default:
    return NULL;
  }
}

static int native_supported(const gnode * n) {
  switch ((gn_native)n->native) {
  case NT_TOK_FUNCTION: case NT_TOK_AT_KEYWORD: case NT_TOK_BAD_STRING: case NT_TOK_BAD_URL: case NT_TOK_WS:
  case NT_TOK_CDO: case NT_TOK_CDC: case NT_TOK_CLOSE_SQUARE: case NT_TOK_CLOSE_PAREN: case NT_TOK_CLOSE_CURLY:
  case NT_TOK_OPEN_SQUARE: case NT_TOK_OPEN_PAREN: case NT_TOK_OPEN_CURLY:
    return 0;
  default:
    return 1;
  }
}

static void compute_sizes(walk * w) {
  const GTEXT_CSS_Compiled * c = w->c;
  for (size_t i = 0; i < c->nnodes; i++) {
    w->size[i] = INF_SIZE;
  }
  for (size_t pass = 0; pass < c->nnodes + 2; pass++) {
    int changed = 0;
    for (size_t i = 0; i < c->nnodes; i++) {
      const gnode * n = &c->nodes[i];
      uint64_t s = INF_SIZE;
      switch ((gn_kind)n->kind) {
      case GN_SEQ: case GN_AND: {
        uint64_t sum = 0;
        for (uint32_t k = 0; k < n->nkids; k++) {
          sum += w->size[c->kids[n->kids + k]];
        }
        s = sum >= INF_SIZE ? INF_SIZE : sum;
        break;
      }
      case GN_ALT: case GN_OR:
        for (uint32_t k = 0; k < n->nkids; k++) {
          const uint32_t ks = w->size[c->kids[n->kids + k]];
          if (ks < s) {
            s = ks;
          }
        }
        break;
      case GN_MULT: {
        const uint32_t ks = w->size[c->kids[n->kids]];
        const uint64_t t = n->min == 0 ? 0 : (uint64_t)ks * n->min;
        s = n->min == 0 ? 0 : (t >= INF_SIZE ? INF_SIZE : t);
        break;
      }
      case GN_REQ:
        s = w->size[c->kids[n->kids]];
        break;
      case GN_KEYWORD: case GN_LITERAL:
        s = 1;
        break;
      case GN_NATIVE:
        s = native_supported(n) ? 1 : INF_SIZE;
        break;
      case GN_FUNC: case GN_BLOCK: {
        const uint32_t ks = n->nkids ? w->size[c->kids[n->kids]] : 0;
        s = ks >= INF_SIZE ? INF_SIZE : (uint64_t)ks + 2;
        break;
      }
      default:
        s = INF_SIZE;
      }
      if (s < w->size[i]) {
        w->size[i] = (uint32_t)s;
        changed = 1;
      }
    }
    if (!changed) {
      break;
    }
  }
}

static void gen(walk * w, uint32_t node, int depth);

static void sep_out(walk * w, const char * s) {
  sb_puts(&w->out, s);
}

static void gen_seq(walk * w, const gnode * n, int depth, const char * sep) {
  for (uint32_t k = 0; k < n->nkids; k++) {
    if (k) {
      sep_out(w, sep);
    }
    gen(w, w->c->kids[n->kids + k], depth + 1);
  }
}

/** The members of a `&&` or `||` group in a random order (the order written, when there is no random source). */
static void gen_group(walk * w, const gnode * n, int depth, int any_subset) {
  uint32_t order[GN_MAX_GROUP];
  uint32_t count = n->nkids > GN_MAX_GROUP ? GN_MAX_GROUP : n->nkids;
  for (uint32_t k = 0; k < count; k++) {
    order[k] = k;
  }
  if (w->rng) {
    for (uint32_t k = count; k > 1; k--) {
      const uint32_t j = below(w->rng, k);
      const uint32_t t = order[k - 1];
      order[k - 1] = order[j];
      order[j] = t;
    }
  }
  int first = 1;
  uint32_t used = 0;
  for (uint32_t k = 0; k < count; k++) {
    const uint32_t kid = w->c->kids[n->kids + order[k]];
    if (any_subset) {
      /* `||`: at least one member; the cheapest alone without a random source, a random subset with one */
      const int take = w->rng ? (below(w->rng, 2) != 0 || (k == count - 1 && used == 0)) : (k == 0);
      if (!take || w->size[kid] >= INF_SIZE) {
        continue;
      }
    }
    if (!first) {
      sb_puts(&w->out, " ");
    }
    first = 0;
    used++;
    gen(w, kid, depth + 1);
  }
}

static void gen(walk * w, uint32_t node, int depth) {
  const GTEXT_CSS_Compiled * c = w->c;
  const gnode * n = &c->nodes[node];
  if (w->out.failed || w->unsupported) {
    return;
  }
  switch ((gn_kind)n->kind) {
  case GN_SEQ:
    gen_seq(w, n, depth, n->sep == 1 ? ", " : (n->sep == 2 ? " / " : " "));
    break;
  case GN_AND:
    gen_group(w, n, depth, 0);
    break;
  case GN_OR:
    gen_group(w, n, depth, 1);
    break;
  case GN_ALT: {
    uint32_t pickk = 0, best = INF_SIZE;
    uint32_t viable[256];
    uint32_t nv = 0;
    for (uint32_t k = 0; k < n->nkids; k++) {
      const uint32_t ks = w->size[c->kids[n->kids + k]];
      if (ks < best) {
        best = ks;
        pickk = k;
      }
      if (ks < INF_SIZE && nv < 256) {
        viable[nv++] = k;
      }
    }
    if (best >= INF_SIZE) {
      w->unsupported = 1;
      return;
    }
    if (w->rng && depth < MAX_DEPTH) {
      uint32_t near[256];
      uint32_t nn = 0;
      for (uint32_t k = 0; k < nv; k++) {
        if (!w->lean || w->size[c->kids[n->kids + viable[k]]] <= best + 2 * w->lean) {
          near[nn++] = viable[k];
        }
      }
      pickk = nn ? near[below(w->rng, nn)] : viable[below(w->rng, nv)];
    }
    gen(w, c->kids[n->kids + pickk], depth + 1);
    break;
  }
  case GN_MULT: {
    uint32_t count = n->min;
    if (w->rng && depth < MAX_DEPTH) {
      uint32_t most = n->max == GN_INF ? n->min + 3 : n->max;
      if (most > n->min + 3) {
        most = n->min + 3;
      }
      count = n->min + below(w->rng, most - n->min + 1);
    }
    if (w->size[c->kids[n->kids]] >= INF_SIZE && count > 0) {
      w->unsupported = 1;
      return;
    }
    for (uint32_t k = 0; k < count; k++) {
      if (k) {
        sb_puts(&w->out, (n->flags & GNF_COMMA) ? ", " : " ");
      }
      gen(w, c->kids[n->kids], depth + 1);
    }
    break;
  }
  case GN_REQ:
    gen(w, c->kids[n->kids], depth + 1);
    break;
  case GN_KEYWORD:
    sb_put(&w->out, c->text + n->str_off, n->str_len);
    break;
  case GN_LITERAL: {
    if (n->lit < 0x80) {
      const char ch = (char)n->lit;
      sb_put(&w->out, &ch, 1);
    }
    else {
      w->unsupported = 1;
    }
    break;
  }
  case GN_NATIVE: {
    if (!native_supported(n)) {
      w->unsupported = 1;
      return;
    }
    const char * suffix = native_word(w, n);
    if (!suffix) {
      w->unsupported = 1;
      return;
    }
    sb_puts(&w->out, suffix);
    break;
  }
  case GN_FUNC: {
    sb_put(&w->out, c->text + n->str_off, n->str_len);
    sb_puts(&w->out, "(");
    if (n->nkids) {
      gen(w, c->kids[n->kids], depth + 1);
    }
    sb_puts(&w->out, ")");
    break;
  }
  case GN_BLOCK: {
    const char * open = n->native == GTEXT_CSS_TOKEN_OPEN_CURLY ? "{" : (n->native == GTEXT_CSS_TOKEN_OPEN_SQUARE ? "[" : "(");
    const char * close = n->native == GTEXT_CSS_TOKEN_OPEN_CURLY ? "}" : (n->native == GTEXT_CSS_TOKEN_OPEN_SQUARE ? "]" : ")");
    sb_puts(&w->out, open);
    if (n->nkids) {
      gen(w, c->kids[n->kids], depth + 1);
    }
    sb_puts(&w->out, close);
    break;
  }
  default:
    w->unsupported = 1;
  }
}

/** Write one sentence; returns it (malloc) or NULL when the grammar cannot be written. */
static char * sentence(walk * w) {
  memset(&w->out, 0, sizeof w->out);
  w->unsupported = 0;
  gen(w, w->c->root, 0);
  if (w->unsupported || w->out.failed) {
    free(w->out.data);
    return NULL;
  }
  if (!w->out.data) {
    sb_puts(&w->out, "");
  }
  return w->out.data;
}

static void tabless(const char * s) {
  for (; *s; s++) {
    if (*s == '\\') {
      fputs("\\\\", stdout);
    }
    else if (*s == '\t') {
      fputs("\\t", stdout);
    }
    else if (*s == '\n') {
      fputs("\\n", stdout);
    }
    else {
      fputc(*s, stdout);
    }
  }
}

static int run_sample(const GTEXT_CSS_Registry * reg, uint64_t seed, unsigned per, int nnames, char ** names) {
  const size_t n = gtext_css_registry_property_count(reg);
  rng_t rng = {seed ? seed : UINT64_C(0x9E3779B97F4A7C15)};
  for (size_t i = 1; i <= n; i++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(reg, (GTEXT_CSS_Property_Id)i, &info) || (info.flags & GTEXT_CSS_PROP_ALIAS)) {
      continue;
    }
    if (nnames) {
      int want = 0;
      for (int k = 0; k < nnames; k++) {
        want |= strcmp(names[k], info.name) == 0;
      }
      if (!want) {
        continue;
      }
    }
    GTEXT_CSS_Grammar_State state = GTEXT_CSS_GRAMMAR_NONE;
    const GTEXT_CSS_Compiled * compiled = NULL;
    if (css_registry_property_compiled(reg, (GTEXT_CSS_Property_Id)i, &state, &compiled, NULL) != GTEXT_CSS_OK) {
      fprintf(stderr, "compile %s\n", info.name);
      return 2;
    }
    if (state != GTEXT_CSS_GRAMMAR_OK || (info.flags & GTEXT_CSS_PROP_CUSTOM)) {
      continue;
    }
    walk w;
    memset(&w, 0, sizeof w);
    w.c = compiled;
    w.size = malloc((compiled->nnodes + 1) * sizeof *w.size);
    if (!w.size) {
      return 2;
    }
    compute_sizes(&w);
    char * s = sentence(&w);
    if (s) {
      printf("%s\t", info.name);
      tabless(s);
      puts("\twitness");
      free(s);
    }
    w.rng = &rng;
    /* a property gets a fixed stream of the generator, however many properties come before it: reseed from the seed and the name */
    uint64_t h = seed ^ UINT64_C(1469598103934665603);
    for (const char * p = info.name; *p; p++) {
      h = (h ^ (unsigned char)*p) * UINT64_C(1099511628211);
    }
    rng.s = h ? h : 1;
    for (unsigned k = 0; k < per; k++) {
      s = sentence(&w);
      if (!s) {
        break;
      }
      printf("%s\t", info.name);
      tabless(s);
      puts("\twalk");
      free(s);
    }
    free(w.size);
  }
  return 0;
}

/**
 * The edges of the numbers of each property's witness: for each of the first numeric types written (at most `targets`), the value
 * replaced by each edge of its range (level 1: the bounds, one past them, zero and a huge value; level 2: all twelve, and the number 1 in
 * each unit of the type, for the first target only). One line a sentence: `<property> TAB <value> TAB edge`.
 */
static int run_edges(const GTEXT_CSS_Registry * reg, int level, int nnames, char ** names) {
  const size_t n = gtext_css_registry_property_count(reg);
  static const int kLevel1[] = {0, 1, 3, 4, 7, 8};
  for (size_t i = 1; i <= n; i++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(reg, (GTEXT_CSS_Property_Id)i, &info) || (info.flags & GTEXT_CSS_PROP_ALIAS)) {
      continue;
    }
    if (nnames) {
      int want = 0;
      for (int k = 0; k < nnames; k++) {
        want |= strcmp(names[k], info.name) == 0;
      }
      if (!want) {
        continue;
      }
    }
    GTEXT_CSS_Grammar_State state = GTEXT_CSS_GRAMMAR_NONE;
    const GTEXT_CSS_Compiled * compiled = NULL;
    if (css_registry_property_compiled(reg, (GTEXT_CSS_Property_Id)i, &state, &compiled, NULL) != GTEXT_CSS_OK) {
      return 2;
    }
    if (state != GTEXT_CSS_GRAMMAR_OK || (info.flags & GTEXT_CSS_PROP_CUSTOM)) {
      continue;
    }
    walk w;
    memset(&w, 0, sizeof w);
    w.c = compiled;
    w.size = malloc((compiled->nnodes + 1) * sizeof *w.size);
    if (!w.size) {
      return 2;
    }
    compute_sizes(&w);
    /* the bases: the witness and up to three sentences of a seeded walk that hold a number (the cheapest sentence of `width` is `auto`,
     * which has none); each is written again, from the generator state it started from, with one number replaced */
    struct {
      uint64_t state;   /* 0: the witness */
      char * text;
      unsigned numbers;
    } bases[4];
    unsigned nbases = 0;
    uint64_t h = UINT64_C(1469598103934665603);
    for (const char * p = info.name; *p; p++) {
      h = (h ^ (unsigned char)*p) * UINT64_C(1099511628211);
    }
    rng_t rng = {h ? h : 1};
    w.rng = NULL;
    w.edge_at = 0x7fffffff; /* counts the numbers and replaces none */
    w.numbers = 0;
    bases[nbases].state = 0;
    bases[nbases].text = sentence(&w);
    bases[nbases].numbers = w.numbers;
    nbases += bases[nbases].text != NULL;
    for (int attempt = 0; attempt < 60 && nbases < 4; attempt++) {
      const uint64_t state = rng.s;
      w.rng = &rng;
      w.lean = 2;
      w.numbers = 0;
      char * s = sentence(&w);
      int fresh = s && w.numbers > 0;
      for (unsigned k = 0; fresh && k < nbases; k++) {
        fresh = strcmp(bases[k].text, s) != 0;
      }
      if (fresh) {
        bases[nbases].state = state;
        bases[nbases].text = s;
        bases[nbases].numbers = w.numbers;
        nbases++;
      }
      else {
        free(s);
      }
    }
    for (unsigned b = 0; b < nbases; b++) {
      for (unsigned t = 0; t < bases[b].numbers && t < (level >= 2 ? 3u : 2u); t++) {
        const int nvar = level >= 2 ? EDGE_NUMBERS + (t == 0 && b == 0 ? (int)COUNT(kLengthUnits) : 0) : (int)COUNT(kLevel1);
        for (int v = 0; v < nvar; v++) {
          rng_t again = {bases[b].state};
          w.rng = bases[b].state ? &again : NULL;
          w.lean = bases[b].state ? 2 : 0;
          w.edge_at = (int)t;
          w.numbers = 0;
          w.edge_variant = level >= 2 ? v : kLevel1[v];
          char * s = sentence(&w);
          if (s && strcmp(s, bases[b].text) != 0) {
            printf("%s\t", info.name);
            tabless(s);
            puts("\tedge");
          }
          free(s);
        }
      }
    }
    for (unsigned b = 0; b < nbases; b++) {
      free(bases[b].text);
    }
    free(w.size);
  }
  return 0;
}

int main(int argc, char ** argv) {
  GTEXT_CSS_Registry * reg = NULL;
  if (gtext_css_registry_new(NULL, &reg) != GTEXT_CSS_OK) {
    return 2;
  }
  int rc;
  if (argc > 1 && strcmp(argv[1], "--properties") == 0) {
    rc = run_properties(reg);
  }
  else if (argc > 3 && strcmp(argv[1], "--sample") == 0) {
    rc = run_sample(reg, strtoull(argv[2], NULL, 10), (unsigned)strtoul(argv[3], NULL, 10), argc - 4, argv + 4);
  }
  else if (argc > 2 && strcmp(argv[1], "--edges") == 0) {
    rc = run_edges(reg, atoi(argv[2]), argc - 3, argv + 3);
  }
  else if (argc == 1) {
    rc = run_answers(reg);
  }
  else {
    fprintf(stderr, "usage: css-prop-ours | --properties | --sample SEED PER [PROPERTY...] | --edges LEVEL [PROPERTY...]\n");
    rc = 2;
  }
  gtext_css_registry_free(reg);
  return rc;
}
