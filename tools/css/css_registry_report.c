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
 * Measures the generated registry against the library and writes one line for each property, descriptor and type:
 * what became of its grammar, whether a value derived from the grammar parses and round-trips, and whether the
 * initial value parses. tools/css/check-grammars.py and tools/css/gen-depth.py read it.
 *
 * Output (tab separated; the first column says what the line is):
 *
 *   P name state missing parses serializes witness initial-state initial-text
 *   D at-rule name state missing parses serializes witness initial-state initial-text
 *   W property NAME | descriptor AT NAME  witnesses matched round-trip-equal   (how many distinct witnesses were derived, how many of them
 *     parse, and how many of those parse to the same typed values when written and read again)
 *   T index name scope state missing
 *
 * The witness is derived from the compiled grammar (the engine's own node graph): a value is written for the first
 * alternative that is cheapest, then for others, and every witness that matches is serialized, parsed again and
 * serialized again. `parses` is 1 if some witness matched; `serializes` is 1 if there was one and every witness that
 * matched read back as the same text and the same number of values.
 *
 * Usage: css_registry_report [--no-witness]
 */

#include <ghoti.io/text/css.h>

#include "../../src/css/css_grammar_internal.h"
#include "../../src/css/css_registry_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a growable string ----------------------------------------------------------------------- */

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

/* ---- the witness ------------------------------------------------------------------------------ */

#define INF_SIZE (1u << 28)
#define MAX_DEPTH 40

typedef struct {
  const GTEXT_CSS_Compiled * c;
  uint32_t * size;   ///< Fewest tokens a node can be written in, or INF_SIZE if it cannot be (an unsupported native).
  unsigned variant;
  int unsupported;
  sbuf out;
} witness;

static const char * native_text(const gnode * n, char * buf, size_t cap) {
  /* a number in the node's range, in the canonical unit, written with the unit of the type */
  double lo = n->lo, hi = n->hi;
  double v = 1;
  if (n->flags & GNF_RANGE) {
    if (v < lo) {
      v = lo;
    }
    if (v > hi) {
      v = hi;
    }
    if (!isfinite(v)) {
      v = 0;
    }
  }
  const char * unit = "";
  switch ((gn_native)n->native) {
  case NT_INTEGER:
    v = floor(v);
    snprintf(buf, cap, "%.0f", v);
    return buf;
  case NT_NUMBER:
    snprintf(buf, cap, "%g", v);
    return buf;
  case NT_PERCENTAGE:
    unit = "%";
    break;
  case NT_LENGTH:
  case NT_LENGTH_PERCENTAGE:
    unit = "px";
    break;
  case NT_ANGLE:
  case NT_ANGLE_PERCENTAGE:
    unit = "deg";
    break;
  case NT_TIME:
  case NT_TIME_PERCENTAGE:
    unit = "s";
    break;
  case NT_FREQUENCY:
  case NT_FREQUENCY_PERCENTAGE:
    unit = "Hz";
    break;
  case NT_RESOLUTION:
    unit = "dppx";
    break;
  case NT_FLEX:
    unit = "fr";
    break;
  default:
    return NULL;
  }
  if ((n->flags & GNF_RANGE) && n->rclass == RC_PERCENT && (gn_native)n->native != NT_PERCENTAGE) {
    unit = "%";
  }
  snprintf(buf, cap, "%g%s", v, unit);
  return buf;
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

static const char * native_word(gn_native t, char * buf, size_t cap, const gnode * n) {
  switch (t) {
  case NT_ZERO: return "0";
  case NT_STRING: case NT_TOK_STRING: return "\"a\"";
  case NT_URL: case NT_TOK_URL: return "url(a)";
  case NT_IDENT: case NT_TOK_IDENT: return "zzident";
  case NT_CUSTOM_IDENT: return "zzcustom";
  case NT_DASHED_IDENT: return "--zzdashed";
  case NT_ANY_VALUE: case NT_DECL_VALUE: return "x";
  case NT_URANGE: return "U+0-7F";
  case NT_ANB: return "2n+1";
  case NT_DECL_LIST: case NT_RULE_LIST: return "";
  case NT_RATIO: return "1 / 1";
  case NT_COLOR: return "red";
  case NT_DIMENSION: case NT_TOK_DIMENSION: return "1zzunit";
  case NT_TOK_HASH: return "#zz";
  case NT_TOK_DELIM: return "/";
  case NT_TOK_NUMBER: return "1";
  case NT_TOK_PERCENTAGE: return "1%";
  case NT_TOK_COLON: return ":";
  case NT_TOK_SEMICOLON: return ";";
  case NT_TOK_COMMA: return ",";
  default: return native_text(n, buf, cap);
  }
}

/** Size of every node, by relaxation; a node that has no finite size cannot be written. */
static void compute_sizes(witness * w) {
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
      case GN_ALT: case GN_OR: {
        for (uint32_t k = 0; k < n->nkids; k++) {
          const uint32_t ks = w->size[c->kids[n->kids + k]];
          if (ks < s) {
            s = ks;
          }
        }
        break;
      }
      case GN_MULT: {
        const uint32_t ks = w->size[c->kids[n->kids]];
        const uint64_t t = n->min == 0 ? 0 : (uint64_t)ks * n->min;
        s = t >= INF_SIZE ? INF_SIZE : t;
        if (n->min == 0) {
          s = 0;
        }
        break;
      }
      case GN_REQ:
        s = w->size[c->kids[n->kids]];
        break;
      case GN_KEYWORD: case GN_LITERAL:
        s = 1;
        break;
      case GN_NATIVE: {
        char buf[64];
        s = (native_supported(n) && native_word((gn_native)n->native, buf, sizeof buf, n)) ? 1 : INF_SIZE;
        break;
      }
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

static void emit_sep(witness * w, int need_space) {
  if (w->out.len && need_space) {
    sb_puts(&w->out, " ");
  }
}

static void gen(witness * w, uint32_t node, int depth);

static void gen_kids(witness * w, const gnode * n, int depth, const char * sep) {
  for (uint32_t k = 0; k < n->nkids; k++) {
    if (k && sep) {
      sb_puts(&w->out, sep);
    }
    gen(w, w->c->kids[n->kids + k], depth + 1);
  }
}

static void gen(witness * w, uint32_t node, int depth) {
  const GTEXT_CSS_Compiled * c = w->c;
  const gnode * n = &c->nodes[node];
  if (w->out.failed) {
    return;
  }
  switch ((gn_kind)n->kind) {
  case GN_SEQ:
    gen_kids(w, n, depth, n->sep == 1 ? ", " : (n->sep == 2 ? " / " : " "));
    break;
  case GN_AND:
    gen_kids(w, n, depth, " ");
    break;
  case GN_ALT: case GN_OR: {
    /* the cheapest alternative, or (for the variants, while the depth allows) another one */
    uint32_t pick = 0, best = INF_SIZE;
    for (uint32_t k = 0; k < n->nkids; k++) {
      const uint32_t ks = w->size[c->kids[n->kids + k]];
      if (ks < best) {
        best = ks;
        pick = k;
      }
    }
    if (depth < MAX_DEPTH && w->variant && n->nkids > 1) {
      const uint32_t alt = (uint32_t)((w->variant + node) % n->nkids);
      if (w->size[c->kids[n->kids + alt]] < INF_SIZE) {
        pick = alt;
      }
    }
    if (best >= INF_SIZE) {
      w->unsupported = 1;
      return;
    }
    gen(w, c->kids[n->kids + pick], depth + 1);
    break;
  }
  case GN_MULT: {
    uint32_t count = n->min;
    if (count == 0 && !(w->variant & 1) && depth < MAX_DEPTH && w->size[c->kids[n->kids]] < INF_SIZE) {
      count = 1;
    }
    if (n->max != GN_INF && count > n->max) {
      count = n->max;
    }
    if (depth >= MAX_DEPTH) {
      count = n->min;
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
    char one[8];
    one[0] = (char)n->lit; /* ASCII literals only; a wider one is encoded by the grammar's own text */
    if (n->lit < 0x80) {
      sb_put(&w->out, one, 1);
    }
    else {
      w->unsupported = 1;
    }
    break;
  }
  case GN_NATIVE: {
    char buf[64];
    const char * t = native_supported(n) ? native_word((gn_native)n->native, buf, sizeof buf, n) : NULL;
    if (!t) {
      w->unsupported = 1;
      return;
    }
    sb_puts(&w->out, t);
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
  (void)emit_sep;
}

/* ---- parsing a witness ------------------------------------------------------------------------ */

typedef struct {
  unsigned witnesses, matched, roundtrip_ok;
} wstats;

/** Whether two typed values are the same value: the same kind, keyword, number, unit, text and parts. Where they came from (spans,
 * component ids, the grammar node that matched) is not part of the value. A math function or a color is compared by its serialization. */
static int values_equal(const GTEXT_CSS_Value * a, const GTEXT_CSS_Value * b) {
  if (!a || !b) {
    return a == b;
  }
  if (a->kind != b->kind || a->atom != b->atom || a->unit != b->unit || a->separator != b->separator || a->flags != b->flags ||
      a->child_count != b->child_count || a->range_start != b->range_start || a->range_end != b->range_end) {
    return 0;
  }
  if (!(a->number == b->number || (a->number != a->number && b->number != b->number)) || !(a->number2 == b->number2 || (a->number2 != a->number2 && b->number2 != b->number2))) {
    return 0;
  }
  if (a->text.length != b->text.length || (a->text.length && memcmp(a->text.data, b->text.data, a->text.length) != 0)) {
    return 0;
  }
  if ((a->math != NULL) != (b->math != NULL) || (a->color != NULL) != (b->color != NULL)) {
    return 0;
  }
  for (size_t i = 0; i < a->child_count; i++) {
    if (!values_equal(&a->children[i], &b->children[i])) {
      return 0;
    }
  }
  return 1;
}

/** Parse "name: value" as a declaration and the value as a property (or a descriptor of at_rule). */
static GTEXT_CSS_Status try_value(const GTEXT_CSS_Registry * reg, const char * name, const char * at_rule, const char * value,
    GTEXT_CSS_Property_Outcome * outcome, char ** ser, size_t * nvalues, GTEXT_CSS_Value_Set ** keep, char ** raw) {
  size_t nl = strlen(name), vl = strlen(value);
  char * decl = malloc(nl + vl + 8);
  if (!decl) {
    return GTEXT_CSS_E_OOM;
  }
  memcpy(decl, name, nl);
  memcpy(decl + nl, ": ", 2);
  memcpy(decl + nl + 2, value, vl + 1);
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_declaration_list(decl, nl + 2 + vl, NULL, &tree, NULL);
  free(decl);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.at_rule = at_rule;
  GTEXT_CSS_Property_Value pv;
  *outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
  *ser = NULL;
  if (tree->declaration_count != 1) {
    *outcome = GTEXT_CSS_PROP_RESULT_INVALID;
    gtext_css_tree_free(tree);
    return GTEXT_CSS_OK;
  }
  st = gtext_css_parse_property_declaration(reg, &tree->declarations[0], &po, &pv);
  if (st == GTEXT_CSS_OK) {
    /* the text entry point must agree with the declaration route (unicode-range reads its own tokens) */
    GTEXT_CSS_Property_Value tv;
    const GTEXT_CSS_Status ts = gtext_css_parse_property_text(reg, name, nl, value, vl, &po, &tv);
    if (ts != GTEXT_CSS_OK || tv.outcome != pv.outcome) {
      if (pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
        fprintf(stderr, "text route: %s: '%s' declaration route %d, text route %d (status %d)\n", name, value, (int)pv.outcome, (int)tv.outcome, (int)ts);
        pv.outcome = GTEXT_CSS_PROP_RESULT_INVALID;
      }
    }
    gtext_css_property_value_free(&tv);
    *outcome = pv.outcome;
    if (pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
      size_t n = 0;
      st = gtext_css_property_value_serialize_buffer(&pv, NULL, ser, &n);
      if (raw) {
        /* the same typed value written without the canonical form (css_property_canon.c), to tell whether a rule changed the text */
        size_t rn = 0;
        const GTEXT_CSS_Value * root = pv.set ? gtext_css_value_set_root(pv.set) : NULL;
        *raw = NULL;
        if (root && gtext_css_serialize_value_buffer(root, NULL, raw, &rn) != GTEXT_CSS_OK) {
          *raw = NULL;
        }
      }
      *nvalues = pv.set ? gtext_css_value_set_count(pv.set) : 0;
      if (keep) {
        *keep = pv.set; /* the caller compares the typed values */
        pv.set = NULL;
      }
    }
  }
  gtext_css_property_value_free(&pv);
  gtext_css_tree_free(tree);
  return st;
}

static void measure_witnesses(const GTEXT_CSS_Registry * reg, const GTEXT_CSS_Compiled * c, const char * name, const char * at_rule,
    int * parses, int * serializes, char * first, size_t first_cap, wstats * stats) {
  *parses = 0;
  memset(stats, 0, sizeof *stats);
  *serializes = 0;
  first[0] = '\0';
  witness w;
  memset(&w, 0, sizeof w);
  w.c = c;
  w.size = malloc((c->nnodes + 1) * sizeof *w.size);
  if (!w.size) {
    return;
  }
  compute_sizes(&w);
  unsigned matched = 0, good = 0;
  char * seen[16];
  size_t nseen = 0;
  for (unsigned v = 0; v < 16; v++) {
    memset(&w.out, 0, sizeof w.out);
    w.variant = v;
    w.unsupported = 0;
    gen(&w, c->root, 0);
    if (!w.out.data) {
      sb_puts(&w.out, "");
    }
    if (w.unsupported || w.out.failed) {
      free(w.out.data);
      continue;
    }
    int dup = 0;
    for (size_t k = 0; k < nseen; k++) {
      if (strcmp(seen[k], w.out.data) == 0) {
        dup = 1;
      }
    }
    if (dup) {
      free(w.out.data);
      continue;
    }
    seen[nseen++] = w.out.data;
    GTEXT_CSS_Property_Outcome oc;
    char * s1 = NULL;
    char * raw1 = NULL;
    size_t nv1 = 0;
    GTEXT_CSS_Value_Set * set1 = NULL;
    stats->witnesses++;
    GTEXT_CSS_Status st = try_value(reg, name, at_rule, w.out.data, &oc, &s1, &nv1, &set1, &raw1);
    if (st != GTEXT_CSS_OK || (oc != GTEXT_CSS_PROP_RESULT_VALID && oc != GTEXT_CSS_PROP_RESULT_PENDING && oc != GTEXT_CSS_PROP_RESULT_WIDE)) {
      fprintf(stderr, "witness not matched: %s%s%s: '%s'\n", at_rule ? at_rule : "", at_rule ? " " : "", name, w.out.data);
      free(s1);
      gtext_css_value_set_free(set1);
      continue;
    }
    matched++;
    stats->matched++;
    if (!first[0]) {
      snprintf(first, first_cap, "%s", w.out.data);
    }
    char * s2 = NULL;
    char * raw2 = NULL;
    size_t nv2 = 0;
    GTEXT_CSS_Value_Set * set2 = NULL;
    const GTEXT_CSS_Property_Outcome oc1 = oc;
    st = try_value(reg, name, at_rule, s1, &oc, &s2, &nv2, &set2, &raw2);
    /* the typed values are equal (the acceptance criterion), not only their text. Except where a canonical-form rule wrote the value shorter
     * (`block flow` as `block`): the text is then another spelling of the same value, a different tree, and the text must be a fixed point. */
    int canonical = raw1 && strcmp(raw1, s1) != 0;
    if (canonical) {
      /* A waiver of tree equality is not a waiver of every check: the canonical text must be accepted as the same kind of value as the
       * witness was, and the spelling of the tree it parses to (raw2) must read and be written as the same canonical text again. */
      char * s3 = NULL;
      size_t nv3 = 0;
      GTEXT_CSS_Property_Outcome oc3;
      const GTEXT_CSS_Status st3 = raw2 ? try_value(reg, name, at_rule, raw2, &oc3, &s3, &nv3, NULL, NULL) : GTEXT_CSS_E_INVALID;
      canonical = oc == oc1 && st3 == GTEXT_CSS_OK && oc3 == oc1 && s3 && strcmp(s3, s1) == 0;
      free(s3);
      if (!canonical) {
        fprintf(stderr, "canonical form: %s: '%s' -> '%s' is not stable under its own tree ('%s')\n", name, w.out.data, s1, raw2 ? raw2 : "-");
      }
    }
    if (st == GTEXT_CSS_OK && oc == oc1 && s2 && strcmp(s1, s2) == 0 &&
        (canonical || (((!raw1 || strcmp(raw1, s1) == 0) && nv1 == nv2) &&
        values_equal(set1 ? gtext_css_value_set_root(set1) : NULL, set2 ? gtext_css_value_set_root(set2) : NULL)))) {
      good++;
      stats->roundtrip_ok++;
    }
    else {
      fprintf(stderr, "round trip: %s: '%s' -> '%s' (%zu values) -> '%s' (%zu values, status %d outcome %d)\n", name, w.out.data, s1,
          nv1, s2 ? s2 : "-", nv2, (int)st, (int)oc);
    }
    free(s1);
    free(raw1);
    free(raw2);
    free(s2);
    gtext_css_value_set_free(set1);
    gtext_css_value_set_free(set2);
  }
  for (size_t k = 0; k < nseen; k++) {
    free(seen[k]);
  }
  free(w.size);
  *parses = matched > 0;
  *serializes = matched > 0 && good == matched;
}

/* ---- the report ------------------------------------------------------------------------------- */

static const char * state_name(GTEXT_CSS_Grammar_State s) {
  switch (s) {
  case GTEXT_CSS_GRAMMAR_NOT_COMPILED: return "not-compiled";
  case GTEXT_CSS_GRAMMAR_OK: return "ok";
  case GTEXT_CSS_GRAMMAR_NONE: return "none";
  case GTEXT_CSS_GRAMMAR_UNRESOLVED: return "unresolved";
  case GTEXT_CSS_GRAMMAR_FAILED: return "failed";
  }
  return "?";
}

/** Whether an initial value text is a value of the property (the declaration route, so that a descriptor's tokens are right). */
static const char * initial_state(const GTEXT_CSS_Registry * reg, const char * name, const char * at_rule, const char * text) {
  if (!text) {
    return "none";
  }
  GTEXT_CSS_Property_Outcome oc;
  char * s = NULL;
  size_t nv = 0;
  const GTEXT_CSS_Status st = try_value(reg, name, at_rule, text, &oc, &s, &nv, NULL, NULL);
  free(s);
  if (st == GTEXT_CSS_E_UNSUPPORTED) {
    return "unsupported";
  }
  if (st != GTEXT_CSS_OK) {
    return "error";
  }
  return oc == GTEXT_CSS_PROP_RESULT_VALID ? "ok" : (oc == GTEXT_CSS_PROP_RESULT_WIDE ? "wide" : (oc == GTEXT_CSS_PROP_RESULT_PENDING ? "pending" : "invalid"));
}

/** What went wrong with a grammar, for the report: the missing reference, or the status and where. */
static void detail_out(const GTEXT_CSS_Grammar_Result * gr);

static void tabs_out(const char * s) {
  /* a field never holds a tab or a newline (the pin's texts do not); replace them rather than break the file */
  for (; s && *s; s++) {
    putchar(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s);
  }
}

static void detail_out(const GTEXT_CSS_Grammar_Result * gr) {
  if (gr->name[0]) {
    tabs_out(gr->name);
  }
  else if (gr->status != GTEXT_CSS_OK) {
    printf("%s@%zu", gtext_css_status_string(gr->status), gr->offset);
  }
}

/* ---- the references no definition answers ------------------------------------------------------- */

typedef struct {
  char * ref;          ///< `<name>` or `<'name'>` as written, without a range.
  char * kind;
  char * name;
  char * grammar;
} missing;

static missing * g_missing;
static size_t g_nmissing, g_capmissing;

static void note_missing(const char * ref, const char * kind, const char * name, const char * grammar) {
  for (size_t i = 0; i < g_nmissing; i++) {
    if (strcmp(g_missing[i].ref, ref) == 0) {
      return;
    }
  }
  if (g_nmissing == g_capmissing) {
    g_capmissing = g_capmissing ? g_capmissing * 2 : 64;
    g_missing = realloc(g_missing, g_capmissing * sizeof *g_missing);
  }
  missing * m = &g_missing[g_nmissing++];
  m->ref = strdup(ref);
  m->kind = strdup(kind);
  m->name = strdup(name);
  m->grammar = strdup(grammar);
}

/** Every `<...>` reference of a grammar text that nothing answers. */
static void scan_references(const GTEXT_CSS_Registry * reg, const char * kind, const char * owner, const char * grammar,
    GTEXT_CSS_Property_Id scope) {
  const char * p = grammar;
  while (*p) {
    if (*p == '\'') { /* a quoted literal */
      const char * q = strchr(p + 1, '\'');
      p = q ? q + 1 : p + strlen(p);
      continue;
    }
    if (*p != '<') {
      p++;
      continue;
    }
    const char * start = p + 1;
    int prop = 0;
    if (*start == '\'') {
      prop = 1;
      start++;
    }
    const char * q = start;
    while (*q && *q != '>' && *q != ' ' && *q != '[' && *q != '\'') {
      q++;
    }
    size_t n = (size_t)(q - start);
    const char * end = strchr(p, '>');
    p = end ? end + 1 : p + strlen(p);
    if (n == 0) {
      continue;
    }
    if (!prop && css_grammar_native_lookup(start, n) != NT_COUNT) {
      continue;
    }
    int ok;
    if (prop) {
      const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(reg, start, n);
      ok = id != 0 && css_registry_resolvable(reg, GTEXT_CSS_REF_PROPERTY, start, n, scope);
    }
    else {
      ok = css_registry_resolvable(reg, GTEXT_CSS_REF_TYPE, start, n, scope);
    }
    if (!ok) {
      /* a definition the pin does have, whose text cannot be linked, is that grammar's failure and is listed as one */
      GTEXT_CSS_Type_Info ti;
      if (!prop && gtext_css_registry_find_type(start, n, scope, &ti) && ti.grammar) {
        continue;
      }
      char ref[160];
      snprintf(ref, sizeof ref, prop ? "<'%.*s'>" : "<%.*s>", (int)n, start);
      note_missing(ref, kind, owner, grammar);
    }
  }
}

int main(int argc, char ** argv) {
  const int witnesses = !(argc > 1 && strcmp(argv[1], "--no-witness") == 0);
  GTEXT_CSS_Registry * reg = NULL;
  if (gtext_css_registry_new(NULL, &reg) != GTEXT_CSS_OK) {
    fprintf(stderr, "registry\n");
    return 2;
  }
  if (gtext_css_registry_prepare(reg) != GTEXT_CSS_OK) {
    fprintf(stderr, "prepare\n");
    return 2;
  }
  const size_t nprops = gtext_css_registry_property_count(reg);
  for (size_t i = 1; i <= nprops; i++) {
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(reg, (GTEXT_CSS_Property_Id)i, &info);
    if (info.grammar) {
      scan_references(reg, "property", info.name, info.grammar, (GTEXT_CSS_Property_Id)i);
    }
  }
  for (GTEXT_CSS_Descriptor_Id d = 1; d <= gtext_css_registry_descriptor_count(); d++) {
    GTEXT_CSS_Descriptor_Info info;
    GTEXT_CSS_At_Rule_Info ai;
    gtext_css_registry_descriptor_info(d, &info);
    gtext_css_registry_at_rule_info(info.at_rule, &ai);
    if (info.kind != GTEXT_CSS_DESC_AT_RULE && info.grammar) {
      char owner[256];
      snprintf(owner, sizeof owner, "%s %s", ai.name, info.name);
      scan_references(reg, "descriptor", owner, info.grammar, 0);
    }
  }
  for (size_t i = 0; i < gtext_css_registry_type_count(); i++) {
    GTEXT_CSS_Type_Info ti;
    gtext_css_registry_type_at(i, &ti);
    if (ti.grammar && !(ti.flags & GTEXT_CSS_TYPE_NATIVE)) {
      scan_references(reg, "type", ti.name, ti.grammar, ti.scope);
    }
  }
  for (size_t i = 1; i <= nprops; i++) {
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(reg, (GTEXT_CSS_Property_Id)i, &info);
    GTEXT_CSS_Grammar_State state;
    GTEXT_CSS_Grammar_Result gr;
    const GTEXT_CSS_Compiled * compiled = NULL;
    GTEXT_CSS_Property_Id target = gtext_css_registry_resolve_alias(reg, (GTEXT_CSS_Property_Id)i);
    GTEXT_CSS_Property_Info tinfo;
    gtext_css_registry_property_info(reg, target, &tinfo);
    memset(&gr, 0, sizeof gr);
    if (css_registry_property_compiled(reg, target, &state, &compiled, &gr) != GTEXT_CSS_OK) {
      fprintf(stderr, "compile %s\n", info.name);
      return 2;
    }
    int parses = 0, serializes = 0;
    wstats ws;
    memset(&ws, 0, sizeof ws);
    char witness_text[512] = "";
    if (witnesses && state == GTEXT_CSS_GRAMMAR_OK && !(info.flags & GTEXT_CSS_PROP_CUSTOM)) {
      measure_witnesses(reg, compiled, info.name, NULL, &parses, &serializes, witness_text, sizeof witness_text, &ws);
    }
    else if (witnesses && state == GTEXT_CSS_GRAMMAR_OK) {
      measure_witnesses(reg, compiled, "--zz-custom", NULL, &parses, &serializes, witness_text, sizeof witness_text, &ws);
    }
    const char * init = (info.flags & GTEXT_CSS_PROP_ALIAS) ? "alias" : initial_state(reg, info.name, NULL, tinfo.initial);
    if (state != GTEXT_CSS_GRAMMAR_OK) {
      init = tinfo.initial ? "skipped" : "none";
    }
    printf("P\t%s\t%s\t", info.name, (info.flags & GTEXT_CSS_PROP_ALIAS) ? "alias" : state_name(state));
    if (info.flags & GTEXT_CSS_PROP_ALIAS) {
      printf("%s%s", tinfo.name, state == GTEXT_CSS_GRAMMAR_OK ? "" : " (its grammar does not compile)");
    }
    else {
      detail_out(&gr);
    }
    printf("\t%d\t%d\t", parses, serializes);
    tabs_out(witness_text);
    printf("\t%s\t", init);
    tabs_out((info.flags & GTEXT_CSS_PROP_ALIAS) ? "" : tinfo.initial);
    printf("\t%u\n", (unsigned)info.flags);
    if (ws.witnesses) {
      printf("W\tproperty %s\t%u\t%u\t%u\n", info.name, ws.witnesses, ws.matched, ws.roundtrip_ok);
    }
    if (state == GTEXT_CSS_GRAMMAR_NONE || (state == GTEXT_CSS_GRAMMAR_FAILED && gr.name[0] == '\0')) {
      printf("G\tproperty %s\t", info.name);
      tabs_out(tinfo.grammar);
      printf("\n");
    }
  }
  for (GTEXT_CSS_Descriptor_Id d = 1; d <= gtext_css_registry_descriptor_count(); d++) {
    GTEXT_CSS_Descriptor_Info info;
    gtext_css_registry_descriptor_info(d, &info);
    GTEXT_CSS_At_Rule_Info ai;
    gtext_css_registry_at_rule_info(info.at_rule, &ai);
    GTEXT_CSS_Grammar_State state;
    GTEXT_CSS_Grammar_Result gr;
    memset(&gr, 0, sizeof gr);
    if (gtext_css_registry_descriptor_state(reg, d, &state, &gr) != GTEXT_CSS_OK) {
      return 2;
    }
    int parses = 0, serializes = 0;
    wstats ws;
    memset(&ws, 0, sizeof ws);
    char witness_text[512] = "";
    if (witnesses && state == GTEXT_CSS_GRAMMAR_OK && info.kind != GTEXT_CSS_DESC_AT_RULE) {
      const GTEXT_CSS_Compiled * compiled;
      GTEXT_CSS_Grammar_State s2;
      css_registry_descriptor_compiled(reg, d, &s2, &compiled, NULL);
      measure_witnesses(reg, compiled, info.name, ai.name, &parses, &serializes, witness_text, sizeof witness_text, &ws);
    }
    const char * init = "none";
    if (info.kind == GTEXT_CSS_DESC_AT_RULE) {
      init = "n/a";
    }
    else if (state != GTEXT_CSS_GRAMMAR_OK) {
      init = info.initial ? "skipped" : "none";
    }
    else if (info.initial) {
      init = initial_state(reg, info.name, ai.name, info.initial);
    }
    printf("D\t%s\t%s\t%s\t", ai.name, info.name, state_name(state));
    detail_out(&gr);
    printf("\t%d\t%d\t", parses, serializes);
    tabs_out(witness_text);
    printf("\t%s\t", init);
    tabs_out(info.initial);
    printf("\t%u\n", (unsigned)info.kind);
    if (ws.witnesses) {
      printf("W\tdescriptor %s %s\t%u\t%u\t%u\n", ai.name, info.name, ws.witnesses, ws.matched, ws.roundtrip_ok);
    }
    if (state == GTEXT_CSS_GRAMMAR_FAILED && gr.name[0] == '\0') {
      printf("G\tdescriptor %s %s\t", ai.name, info.name);
      tabs_out(info.grammar);
      printf("\n");
    }
  }
  for (size_t i = 0; i < gtext_css_registry_type_count(); i++) {
    GTEXT_CSS_Type_Info ti;
    gtext_css_registry_type_at(i, &ti);
    GTEXT_CSS_Grammar_State state;
    GTEXT_CSS_Grammar_Result gr;
    memset(&gr, 0, sizeof gr);
    if (gtext_css_registry_type_state(reg, i, &state, &gr) != GTEXT_CSS_OK) {
      return 2;
    }
    GTEXT_CSS_Property_Info si;
    const char * scope = "";
    if (ti.scope && gtext_css_registry_property_info(reg, ti.scope, &si)) {
      scope = si.name;
    }
    printf("T\t%s\t%s\t%s\t", ti.name, scope, (ti.flags & GTEXT_CSS_TYPE_NATIVE) ? "native" : state_name(state));
    detail_out(&gr);
    printf("\t%u\n", (unsigned)ti.flags);
    if (state == GTEXT_CSS_GRAMMAR_FAILED && gr.name[0] == '\0') {
      printf("G\ttype %s%s%s\t", scope, scope[0] ? "/" : "", ti.name);
      tabs_out(ti.grammar);
      printf("\n");
    }
  }
  for (size_t i = 0; i < g_nmissing; i++) {
    printf("R\t%s\t%s\t", g_missing[i].ref, g_missing[i].kind);
    tabs_out(g_missing[i].name);
    printf("\t");
    tabs_out(g_missing[i].grammar);
    printf("\n");
  }
  gtext_css_registry_free(reg);
  return 0;
}
