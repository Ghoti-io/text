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
 * Arbitrary substitution functions (css_substitute.h): the algorithm of CSS Values 5, Appendix A, as a machine over an explicit stack of frames.
 *
 * ## The shape of it
 *
 * Every value that is being replaced is a **sequence** of component values (a frame of kind SEQ). Walking it, a run of values with nothing to
 * replace in them is copied as one piece of the source text, kept byte for byte (comments and white space included, since a custom property's
 * value is "exactly as specified"); a function that is an arbitrary substitution function starts a frame of its own; a function or block that
 * only holds one (`calc(1px + var(--x))`) is a CONTAINER whose contents are a sequence. A finished frame hands its result, a rope (css_rope.h) or the
 * guaranteed-invalid value, to the frame below it, which goes on from there. A frame that needs a custom property's computed value and finds it
 * not yet computed starts a PROP frame for it, which is a sequence over the property's declared value; so a chain of custom properties is a stack
 * of frames and not of C calls.
 *
 * ## What "invalid" does to what contains it
 *
 * The guaranteed-invalid value is not removed from the sequence it is in: "replace func in values with the guaranteed-invalid value". A sequence that
 * has one in it is guaranteed-invalid whole, which is what `var(--missing)` does to `calc(1px + var(--missing))`. The rest of such a sequence is still
 * walked, because a cycle found in it makes the custom properties on the stack cyclic whatever the sequence came to.
 *
 * ## Cycles
 *
 * A custom property being computed is on the stack with its state `in progress`. A reference to it from a value it is computing (directly or
 * through others) marks every PROP frame from there to the top of the stack as cyclic, and the reference is the guaranteed-invalid value; a frame that
 * finishes marked is guaranteed-invalid whatever its text came to ("The presence of a fallback in var(--three, baz) does not affect the outcome"). A
 * range of the stack is marked once, so many references to one property of a deep chain cost what they hold and not the depth of the stack each.
 */

#include <ghoti.io/text/css/css_substitute.h>

#include <float.h>
#include <ghoti.io/math/math.h>
#include <stdlib.h>
#include <string.h>

#include "css_substitute_internal.h"

#define DEFAULT_MAX_TOKENS ((uint64_t)1048576)
#define LIMIT_MAX_TOKENS ((uint64_t)1 << 40)
#define DEFAULT_MAX_ELEMENT_TOKENS ((uint64_t)16777216)
#define LIMIT_MAX_ELEMENT_TOKENS ((uint64_t)1 << 44)
#define DEFAULT_MAX_CUSTOM ((size_t)65536)
#define LIMIT_MAX_CUSTOM ((size_t)1 << 24)
#define DEFAULT_MAX_BYTES ((size_t)16 << 20)
#define LIMIT_MAX_BYTES ((size_t)1 << 30)
#define DEFAULT_MAX_DEPTH ((size_t)100000)
#define LIMIT_MAX_DEPTH ((size_t)1 << 24)

GTEXT_CSS_Substitute_Options gtext_css_substitute_options_default(void) {
  GTEXT_CSS_Substitute_Options o;
  memset(&o, 0, sizeof o);
  o.max_substituted_tokens = DEFAULT_MAX_TOKENS;
  o.max_element_substituted_tokens = DEFAULT_MAX_ELEMENT_TOKENS;
  o.max_custom_properties = DEFAULT_MAX_CUSTOM;
  o.max_value_bytes = DEFAULT_MAX_BYTES;
  o.max_substitution_depth = DEFAULT_MAX_DEPTH;
  return o;
}

/* ---- a parsed source ------------------------------------------------------------------------------------ */

enum {
  F_SUB = 0x1,        ///< Somewhere in it (itself included) is an arbitrary substitution function.
  F_FN = 0x2,         ///< It is one.
  F_SPREAD_FN = 0x4,  ///< It is one and `...` comes right before it.
  F_HAS_SPREAD = 0x8  ///< Some function inside it (not through another substitution function) has `...` right before it.
};

typedef struct {
  GTEXT_CSS_Tree * tree;
  const char * text;
  size_t len;
  char * owned;          ///< The text, when this made it.
  uint8_t * flags;       ///< By node id.
  uint32_t * toks;       ///< By node id: the tokens it is made of (saturating).
  bool tainted;          ///< Everything made from this came from an attr().
} src_t;

static bool fn_name_is(const GTEXT_CSS_String * name, const char * lower) {
  return css_ascii_ieq(name, lower);
}

typedef enum {
  K_NONE = 0, K_VAR, K_ENV, K_ATTR, K_IF, K_INHERIT, K_IDENT, K_RANDOM_ITEM, K_RANDOM, K_SIBLING_INDEX, K_SIBLING_COUNT
} fn_kind;

static fn_kind kind_of(const GTEXT_CSS_Component_Value * v) {
  if (v->kind != GTEXT_CSS_VALUE_FUNCTION) {
    return K_NONE;
  }
  switch (v->value.length) {
  case 2:
    return fn_name_is(&v->value, "if") ? K_IF : K_NONE;
  case 3:
    return fn_name_is(&v->value, "var") ? K_VAR : fn_name_is(&v->value, "env") ? K_ENV : K_NONE;
  case 4:
    return fn_name_is(&v->value, "attr") ? K_ATTR : K_NONE;
  case 5:
    return fn_name_is(&v->value, "ident") ? K_IDENT : K_NONE;
  case 6:
    return fn_name_is(&v->value, "random") ? K_RANDOM : K_NONE;
  case 7:
    return fn_name_is(&v->value, "inherit") ? K_INHERIT : K_NONE;
  case 11:
    return fn_name_is(&v->value, "random-item") ? K_RANDOM_ITEM : K_NONE;
  case 13:
    return fn_name_is(&v->value, "sibling-index") ? K_SIBLING_INDEX : fn_name_is(&v->value, "sibling-count") ? K_SIBLING_COUNT : K_NONE;
  default:
    return K_NONE;
  }
}

static bool is_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static bool is_tok(const GTEXT_CSS_Component_Value * v, GTEXT_CSS_Token_Type t) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == t;
}

static bool is_dot(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_DELIM && v->delim == '.';
}

static uint32_t sat32(uint64_t v) {
  return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

/// Mark the spread functions of a list of siblings: a substitution function directly after three adjacent `.` delimiters.
static void mark_spread(const GTEXT_CSS_Component_Value * v, size_t n, uint8_t * flags) {
  for (size_t i = 3; i < n; i++) {
    if (kind_of(&v[i]) != K_NONE && is_dot(&v[i - 1]) && is_dot(&v[i - 2]) && is_dot(&v[i - 3]) && v[i - 3].span.end == v[i - 2].span.offset &&
        v[i - 2].span.end == v[i - 1].span.offset && v[i - 1].span.end == v[i].span.offset) {
      flags[v[i].id] |= F_SPREAD_FN;
    }
  }
}

typedef struct {
  const GTEXT_CSS_Component_Value * v;
  size_t n, i;
  uint8_t acc;     ///< F_SUB / spread seen among the finished children.
  uint64_t toks;
} info_frame;

static void src_close(const GTEXT_Allocator * alloc, src_t * s) {
  if (!s) {
    return;
  }
  GTEXT_Allocator a = *alloc;
  gtext_css_tree_free(s->tree);
  gtext_allocator_free(&a, s->flags);
  gtext_allocator_free(&a, s->toks);
  gtext_allocator_free(&a, s->owned);
  gtext_allocator_free(&a, s);
}

/// Parse a text and compute the flags and token counts of its nodes. `owned` (a malloc'd copy that this takes over, or NULL) is the text when it must be kept.
static GTEXT_CSS_Status src_open(const GTEXT_Allocator * alloc, const char * text, size_t len, char * owned, bool tainted, src_t ** out) {
  GTEXT_Allocator a = *alloc;
  src_t * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    gtext_allocator_free(&a, owned);
    return GTEXT_CSS_E_OOM;
  }
  s->owned = owned;
  s->text = owned ? owned : text;
  s->len = len;
  s->tainted = tainted;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = alloc;
  po.count_errors_only = true;
  GTEXT_CSS_Status st = gtext_css_parse_component_values(s->text, len, &po, &s->tree, NULL);
  if (st != GTEXT_CSS_OK) {
    s->tree = NULL;
    src_close(alloc, s);
    return st;
  }
  const size_t nodes = s->tree->node_count + 1;
  s->flags = gtext_allocator_calloc(&a, nodes, sizeof *s->flags);
  s->toks = gtext_allocator_calloc(&a, nodes, sizeof *s->toks);
  size_t cap = 16, depth = 0;
  info_frame * stack = gtext_allocator_malloc(&a, cap * sizeof *stack);
  if (!s->flags || !s->toks || !stack) {
    gtext_allocator_free(&a, stack);
    src_close(alloc, s);
    return GTEXT_CSS_E_OOM;
  }
  stack[depth++] = (info_frame){s->tree->values, s->tree->value_count, 0, 0, 0};
  mark_spread(s->tree->values, s->tree->value_count, s->flags);
  while (depth) {
    info_frame * f = &stack[depth - 1];
    if (f->i >= f->n) {
      /* the list is finished: its summary goes to the node it is the contents of, which is the frame below */
      const uint8_t acc = f->acc;
      const uint64_t toks = f->toks;
      depth--;
      if (depth) {
        info_frame * up = &stack[depth - 1];
        const GTEXT_CSS_Component_Value * node = &up->v[up->i - 1];
        const bool fn = kind_of(node) != K_NONE;
        uint8_t fl = (uint8_t)(s->flags[node->id] & F_SPREAD_FN);
        if (fn) {
          fl |= F_FN | F_SUB;
        }
        if (acc & F_SUB) {
          fl |= F_SUB;
        }
        if (acc & F_HAS_SPREAD) {
          fl |= F_HAS_SPREAD; /* kept on a substitution function too: its own contents need the early pass; its parent ignores it (below) */
        }
        s->flags[node->id] = fl;
        s->toks[node->id] = sat32(2 + toks);
        up->toks += s->toks[node->id];
        if (fl & F_SUB) {
          up->acc |= F_SUB;
        }
        if (((fl & F_HAS_SPREAD) && !(fl & F_FN)) || (fl & F_SPREAD_FN)) {
          up->acc |= F_HAS_SPREAD;
        }
      }
      continue;
    }
    const GTEXT_CSS_Component_Value * c = &f->v[f->i++];
    if (c->kind == GTEXT_CSS_VALUE_TOKEN) {
      uint8_t fl = (uint8_t)(s->flags[c->id] & F_SPREAD_FN);
      s->flags[c->id] = fl;
      s->toks[c->id] = 1;
      f->toks += 1;
      if (fl & F_SPREAD_FN) {
        f->acc |= F_HAS_SPREAD;
      }
      continue;
    }
    if (c->child_count == 0) {
      uint8_t fl = (uint8_t)(s->flags[c->id] & F_SPREAD_FN);
      if (kind_of(c) != K_NONE) {
        fl |= F_FN | F_SUB;
      }
      s->flags[c->id] = fl;
      s->toks[c->id] = 2;
      f->toks += 2;
      if (fl & F_SUB) {
        f->acc |= F_SUB;
      }
      if (fl & F_SPREAD_FN) {
        f->acc |= F_HAS_SPREAD;
      }
      continue;
    }
    if (depth == cap) {
      info_frame * g = gtext_allocator_realloc(&a, stack, cap * 2 * sizeof *g);
      if (!g) {
        gtext_allocator_free(&a, stack);
        src_close(alloc, s);
        return GTEXT_CSS_E_OOM;
      }
      stack = g;
      cap *= 2;
    }
    mark_spread(c->children, c->child_count, s->flags);
    stack[depth++] = (info_frame){c->children, c->child_count, 0, 0, 0};
  }
  gtext_allocator_free(&a, stack);
  *out = s;
  return GTEXT_CSS_OK;
}

/* ---- the state of one element's substitution -------------------------------------------------------------- */

typedef enum {
  FR_SEQ = 0,
  FR_CONTAINER,
  FR_PROP,
  FR_VAR,
  FR_ENV,
  FR_ATTR,
  FR_ATTRVAL,
  FR_IF,
  FR_INHERIT,
  FR_IDENT,
  FR_RANDOM_ITEM,
  FR_RANDOM,
  FR_TREE
} fr_kind;

typedef struct {
  size_t from, count;
} range_t;

/// A result slot of a frame: a rope, or the guaranteed-invalid value, once it arrived.
typedef struct {
  css_rope * rope;
  bool gi;
  bool set;
} slot_t;

typedef struct {
  fr_kind kind;
  int phase;
  bool cyclic;
  bool has_arrived;
  bool early_only;
  bool gi;                                  ///< SEQ: something in it is guaranteed-invalid.
  int wait_slot;                            ///< Where the child that was started delivers.
  slot_t arrived;
  /* the source this frame reads */
  src_t * src;
  const GTEXT_CSS_Component_Value * vals;   ///< A SEQ's range, or a function's contents after the early pass.
  size_t nvals;
  size_t pos;
  /* SEQ */
  size_t run_start;                         ///< Index of the first value of the run being copied, or SIZE_MAX.
  size_t run_from;                          ///< Byte where the run (or the trivia before the next value) begins.
  uint64_t run_toks;
  css_rope * acc;
  /* a function, a container, a property */
  const GTEXT_CSS_Component_Value * fn;
  src_t * csrc;                             ///< The source `vals` of a function index into: `src`, or `own_src` after the early pass.
  src_t * own_src;                          ///< The contents after the early pass, parsed.
  range_t * args;
  size_t nargs;
  bool has_comma;                           ///< First-comma split: there was a comma.
  slot_t res[3];
  size_t entry;                             ///< PROP: the entry of the property.
  size_t branch;                            ///< IF: the branch being looked at.
  /* the result */
  css_rope * out;
  bool out_gi;
  /* misc per kind */
  union {
    struct {
      char * name;                          ///< VAR, INHERIT, ATTR: the name asked for (owned).
      size_t name_len;
    } n;
  } u;
  void * aux;                               ///< IF: the parsed condition; ATTR: the namespace prefix as written (owned).
  size_t aux_n;
  bool has_ns;                              ///< ATTR: a namespace prefix was written.
  int mode;                                 ///< ATTR: 0 no type was given, 1 raw-string, 2 number, 3 a unit, 4 type().
  GTEXT_CSS_Unit unit;                      ///< ATTR, mode 3: the unit.
  bool pct;                                 ///< ATTR, mode 3: the unit is `%`.
  const css_compiled_syntax * csyn;         ///< ATTR, mode 4: the syntax.
  bool csyn_temp;                           ///< The syntax is the frame's own.
  bool guarded;                             ///< ATTR: the attribute is in the guard table for this frame.
} frame_t;

typedef enum {
  ENT_UNRESOLVED = 0,
  ENT_INPROGRESS,
  ENT_DONE
} ent_state;

typedef struct {
  GTEXT_CSS_String name;
  size_t item;                              ///< The index in the block of the declaration that counts.
  ent_state state;
  GTEXT_CSS_Custom_Value * value;           ///< DONE: the computed value (retained); NULL is the guaranteed-invalid value.
  long reg;                                 ///< The registered custom property's place in the context's table, or -1.
  size_t frame;                             ///< INPROGRESS: the frame computing it.
} entry_t;

struct css_sub {
  css_state * st;
  const GTEXT_CSS_Compute_Context * cc;
  GTEXT_Allocator alloc;
  entry_t * entries;
  size_t nentries;
  frame_t * frames;
  size_t nframes, capframes;
  size_t max_depth;
  uint64_t max_tokens, max_element_tokens;
  size_t max_bytes;
  uint64_t element_tokens;                  ///< Tokens charged to this element.
  uint64_t peak;                            ///< The largest expansion any value was found to have.
  size_t limited;
  size_t work;
  css_rope * comment;                       ///< The comment that goes between tokens that would run together.
  css_rope * empty;
  /* the stack range already marked cyclic */
  bool cyc_valid;
  size_t cyc_lo, cyc_hi;
  /* the guards of attribute substitution: in progress by name */
  struct {
    GTEXT_CSS_String name;
    size_t frame;
  } * aguards;
  size_t naguards, capaguards;
  /* random values */
  const char * prop;
  size_t prop_len;
  uint32_t random_index;
  GTEXT_CSS_Status failure;
};

#define SUB_FAIL(sub, status) ((sub)->failure = (sub)->failure == GTEXT_CSS_OK ? (status) : (sub)->failure)

static bool tok_ieq(const GTEXT_CSS_String * s, const char * lower) {
  return css_ascii_ieq(s, lower);
}

/* ---- ropes with the budget --------------------------------------------------------------------------------- */

/// A rope or the guaranteed-invalid value of `a` followed by `b` (a may be NULL for none yet). `*gi` is set when the budget refuses the result. The references of
/// `a` and `b` are not taken.
static GTEXT_CSS_Status rope_append(css_sub * sub, css_rope * a, css_rope * b, css_rope ** out, bool * gi) {
  *gi = false;
  *out = NULL;
  const uint64_t tokens = css_sat_add(css_rope_tokens(a), css_rope_tokens(b));
  const uint64_t bytes = css_sat_add(css_rope_bytes(a), css_rope_bytes(b));
  if (tokens > sub->peak) {
    sub->peak = tokens;
  }
  if (tokens > sub->max_tokens || bytes > sub->max_bytes) {
    sub->limited++;
    *gi = true;
    return GTEXT_CSS_OK;
  }
  css_rope * r = css_rope_cat(&sub->alloc, a ? a : sub->empty, b ? b : sub->empty, sub->comment);
  if (!r) {
    return GTEXT_CSS_E_OOM;
  }
  *out = r;
  return GTEXT_CSS_OK;
}

/// A value that is complete is checked against the budgets of the element: it is charged to it, and refused if the element has used up what it may have.
static bool charge(css_sub * sub, css_rope * r) {
  const uint64_t t = css_rope_tokens(r);
  if (t > sub->peak) {
    sub->peak = t;
  }
  if (t > sub->max_tokens) {
    sub->limited++;
    return false;
  }
  const uint64_t total = css_sat_add(sub->element_tokens, t);
  if (total > sub->max_element_tokens) {
    sub->element_tokens = total;
    sub->limited++;
    return false;
  }
  sub->element_tokens = total;
  return true;
}

/* ---- frames -------------------------------------------------------------------------------------------------- */

static void slot_clear(slot_t * s) {
  css_rope_release(s->rope);
  memset(s, 0, sizeof *s);
}

static void cond_free(const GTEXT_Allocator * alloc, void * cond);

static void frame_release(css_sub * sub, frame_t * f) {
  GTEXT_Allocator a = sub->alloc;
  css_rope_release(f->acc);
  css_rope_release(f->out);
  slot_clear(&f->arrived);
  for (int i = 0; i < 3; i++) {
    slot_clear(&f->res[i]);
  }
  src_close(&sub->alloc, f->own_src);
  gtext_allocator_free(&a, f->args);
  gtext_allocator_free(&a, f->u.n.name);
  if (f->kind == FR_IF) {
    cond_free(&sub->alloc, f->aux);
  }
  else {
    gtext_allocator_free(&a, f->aux);
  }
  if (f->csyn_temp) {
    css_compiled_syntax_free((css_compiled_syntax *)f->csyn);
  }
  memset(f, 0, sizeof *f);
}

static frame_t * frame_push(css_sub * sub, fr_kind kind, GTEXT_CSS_Status * status) {
  if (sub->nframes >= sub->max_depth) {
    *status = GTEXT_CSS_E_LIMIT_DEPTH;
    return NULL;
  }
  if (sub->nframes == sub->capframes) {
    const size_t cap = sub->capframes ? sub->capframes * 2 : 32;
    if (cap > SIZE_MAX / sizeof *sub->frames) {
      *status = GTEXT_CSS_E_OOM;
      return NULL;
    }
    GTEXT_Allocator a = sub->alloc;
    frame_t * g = gtext_allocator_realloc(&a, sub->frames, cap * sizeof *g);
    if (!g) {
      *status = GTEXT_CSS_E_OOM;
      return NULL;
    }
    sub->frames = g;
    sub->capframes = cap;
  }
  frame_t * f = &sub->frames[sub->nframes++];
  memset(f, 0, sizeof *f);
  f->kind = kind;
  f->run_start = SIZE_MAX;
  return f;
}

static void frame_pop(css_sub * sub) {
  frame_release(sub, &sub->frames[sub->nframes - 1]);
  sub->nframes--;
  if (sub->cyc_valid && sub->cyc_hi >= sub->nframes) {
    if (sub->nframes == 0 || sub->cyc_lo >= sub->nframes) {
      sub->cyc_valid = false;
    }
    else {
      sub->cyc_hi = sub->nframes - 1;
    }
  }
}

/// A cycle: every frame from `from` to the top of the stack is a participant, and finishes guaranteed-invalid.
static void mark_cyclic(css_sub * sub, size_t from) {
  const size_t top = sub->nframes - 1;
  if (sub->cyc_valid && from >= sub->cyc_lo && top <= sub->cyc_hi) {
    return;
  }
  for (size_t i = from; i <= top; i++) {
    if (sub->cyc_valid && i >= sub->cyc_lo && i <= sub->cyc_hi) {
      i = sub->cyc_hi;
      continue;
    }
    sub->frames[i].cyclic = true;
    sub->work++;
  }
  if (sub->cyc_valid && from <= sub->cyc_hi + 1 && top + 1 >= sub->cyc_lo) {
    if (from < sub->cyc_lo) {
      sub->cyc_lo = from;
    }
    if (top > sub->cyc_hi) {
      sub->cyc_hi = top;
    }
  }
  else {
    sub->cyc_valid = true;
    sub->cyc_lo = from;
    sub->cyc_hi = top;
  }
}

/// Hand the result of the top frame to the frame below it. `r` is taken (NULL with `gi`: the guaranteed-invalid value).
static void deliver(css_sub * sub, css_rope * r, bool gi) {
  frame_t * parent = &sub->frames[sub->nframes - 1];
  parent->arrived.rope = r;
  parent->arrived.gi = gi;
  parent->arrived.set = true;
  parent->has_arrived = true;
}

/* ---- sequences ------------------------------------------------------------------------------------------------ */

enum { STEP_PUSHED = 1, STEP_DONE = 2, STEP_ERROR = 3 };

/// The characters that close what the end of the input closed, from the value `cv` to the last value inside it, innermost first.
static size_t closers_of(const src_t * s, const GTEXT_CSS_Component_Value * cv, char * out, size_t cap) {
  char stack[512];
  size_t n = 0;
  const GTEXT_CSS_Component_Value * c = cv;
  while (c && (c->flags & GTEXT_CSS_FLAG_UNCLOSED) && n < sizeof stack) {
    char ch = 0;
    if (c->kind == GTEXT_CSS_VALUE_TOKEN) {
      if (c->type == GTEXT_CSS_TOKEN_STRING && c->span.offset < s->len) {
        ch = s->text[c->span.offset];
      }
      else if (c->type == GTEXT_CSS_TOKEN_URL) {
        ch = ')';
      }
    }
    else if (c->kind == GTEXT_CSS_VALUE_FUNCTION || c->type == GTEXT_CSS_TOKEN_OPEN_PAREN) {
      ch = ')';
    }
    else if (c->type == GTEXT_CSS_TOKEN_OPEN_SQUARE) {
      ch = ']';
    }
    else if (c->type == GTEXT_CSS_TOKEN_OPEN_CURLY) {
      ch = '}';
    }
    if (ch) {
      stack[n++] = ch;
    }
    c = c->kind != GTEXT_CSS_VALUE_TOKEN && c->child_count ? &c->children[c->child_count - 1] : NULL;
  }
  size_t k = 0;
  while (n && k < cap) {
    out[k++] = stack[--n];
  }
  return k;
}

/// A leaf of the bytes `[from, to)` of the source of `f`, with `first`/`last` as the classes of its ends, `last_cv` the value its end is the end of (for the
/// closers), and `tokens` its tokens.
static GTEXT_CSS_Status make_leaf(css_sub * sub, const src_t * s, size_t from, size_t to, css_tokclass first, css_tokclass last, uint64_t tokens,
    const GTEXT_CSS_Component_Value * last_cv, css_rope ** out) {
  char tail[600];
  size_t nt = 0;
  if (last_cv && to == last_cv->span.end) {
    nt = closers_of(s, last_cv, tail, sizeof tail);
  }
  css_rope * r;
  if (nt == 0) {
    r = css_rope_leaf(&sub->alloc, s->text + from, to - from, first, last, tokens, s->tainted);
  }
  else {
    GTEXT_Allocator a = sub->alloc;
    char * buf = gtext_allocator_malloc(&a, to - from + nt);
    if (!buf) {
      return GTEXT_CSS_E_OOM;
    }
    memcpy(buf, s->text + from, to - from);
    memcpy(buf + (to - from), tail, nt);
    r = css_rope_leaf(&sub->alloc, buf, to - from + nt, first, last, tokens, s->tainted);
    gtext_allocator_free(&a, buf);
  }
  if (!r) {
    return GTEXT_CSS_E_OOM;
  }
  *out = r;
  return GTEXT_CSS_OK;
}

/// Add a rope to the accumulator of a SEQ frame (poisoning it if the budget refuses).
static GTEXT_CSS_Status seq_add(css_sub * sub, size_t fi, css_rope * r) {
  frame_t * f = &sub->frames[fi];
  if (f->gi) {
    return GTEXT_CSS_OK;
  }
  css_rope * made = NULL;
  bool gi = false;
  GTEXT_CSS_Status st = rope_append(sub, f->acc, r, &made, &gi);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (gi) {
    f->gi = true;
    return GTEXT_CSS_OK;
  }
  css_rope_release(f->acc);
  f->acc = made;
  return GTEXT_CSS_OK;
}

/// End the run being copied at byte `to`: its text becomes a leaf of the accumulator.
static GTEXT_CSS_Status seq_flush(css_sub * sub, size_t fi, size_t to) {
  frame_t * f = &sub->frames[fi];
  const size_t run_from = f->run_from;
  const size_t first_idx = f->run_start;
  const size_t end_idx = f->pos; /* the values of the run are first_idx .. pos - 1 */
  const uint64_t toks = f->run_toks;
  f->run_start = SIZE_MAX;
  f->run_toks = 0;
  if (to <= run_from || f->gi) {
    return GTEXT_CSS_OK;
  }
  css_tokclass first = CSS_TC_SAFE, last = CSS_TC_SAFE;
  const GTEXT_CSS_Component_Value * last_cv = NULL;
  if (first_idx != SIZE_MAX && end_idx > first_idx) {
    const GTEXT_CSS_Component_Value * a = &f->vals[first_idx];
    last_cv = &f->vals[end_idx - 1];
    first = run_from < a->span.offset ? CSS_TC_SAFE : css_cv_first_class(a);
    last = to > last_cv->span.end ? CSS_TC_SAFE : css_cv_last_class(last_cv);
  }
  css_rope * leaf = NULL;
  GTEXT_CSS_Status st = make_leaf(sub, f->src, run_from, to, first, last, toks, last_cv, &leaf);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = seq_add(sub, fi, leaf);
  css_rope_release(leaf);
  return st;
}

/// Whether a value has something to replace in it, in the mode of the frame.
static bool needs_work(const frame_t * f, const GTEXT_CSS_Component_Value * c) {
  const uint8_t fl = f->src->flags[c->id];
  if (f->early_only) {
    return (fl & F_SPREAD_FN) || ((fl & F_HAS_SPREAD) && !(fl & F_FN));
  }
  return (fl & F_SUB) != 0;
}

static fr_kind frame_kind_of(fn_kind k) {
  switch (k) {
  case K_VAR: return FR_VAR;
  case K_ENV: return FR_ENV;
  case K_ATTR: return FR_ATTR;
  case K_IF: return FR_IF;
  case K_INHERIT: return FR_INHERIT;
  case K_IDENT: return FR_IDENT;
  case K_RANDOM_ITEM: return FR_RANDOM_ITEM;
  case K_RANDOM: return FR_RANDOM;
  default: return FR_TREE;
  }
}

/// Start a SEQ over a range of values of `src`, for the frame `parent` (`slot` is where its result goes). Returns 0 when the range is empty (the slot is filled with
/// the empty value and nothing was started), 1 when a frame was started, -1 on failure.
static int seq_start(css_sub * sub, size_t parent, int slot, src_t * src, const GTEXT_CSS_Component_Value * vals, size_t n, bool early) {
  if (n == 0) {
    frame_t * p = &sub->frames[parent];
    slot_clear(&p->res[slot]);
    p->res[slot].rope = css_rope_retain(sub->empty);
    p->res[slot].set = true;
    return 0;
  }
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  frame_t * f = frame_push(sub, FR_SEQ, &st);
  if (!f) {
    SUB_FAIL(sub, st);
    return -1;
  }
  f->src = src;
  f->vals = vals;
  f->nvals = n;
  f->early_only = early;
  f->run_from = vals[0].span.offset;
  sub->frames[parent].wait_slot = slot;
  return 1;
}

static void take_arrived(frame_t * f) {
  if (!f->has_arrived) {
    return;
  }
  slot_clear(&f->res[f->wait_slot]);
  f->res[f->wait_slot] = f->arrived;
  memset(&f->arrived, 0, sizeof f->arrived);
  f->has_arrived = false;
}

/// Start the frame of a function or container found in a sequence.
static GTEXT_CSS_Status child_frame(css_sub * sub, size_t parent, fr_kind kind, const GTEXT_CSS_Component_Value * c) {
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  src_t * src = sub->frames[parent].src;
  const bool early = sub->frames[parent].early_only;
  frame_t * f = frame_push(sub, kind, &st);
  if (!f) {
    return st;
  }
  f->src = src;
  f->fn = c;
  f->early_only = early;
  return GTEXT_CSS_OK;
}

static int step_seq(css_sub * sub, size_t fi) {
  frame_t * f = &sub->frames[fi];
  if (f->has_arrived) {
    /* what the value we were waiting for came to: it goes on the accumulator, and the copying resumes after the value */
    css_rope * r = f->arrived.rope;
    const bool gi = f->arrived.gi;
    f->arrived.rope = NULL;
    f->arrived.set = false;
    f->has_arrived = false;
    if (gi) {
      f->gi = true;
    }
    else {
      const GTEXT_CSS_Status st = seq_add(sub, fi, r);
      if (st != GTEXT_CSS_OK) {
        css_rope_release(r);
        SUB_FAIL(sub, st);
        return STEP_ERROR;
      }
    }
    css_rope_release(r);
    f = &sub->frames[fi];
  }
  while (f->pos < f->nvals) {
    const GTEXT_CSS_Component_Value * c = &f->vals[f->pos];
    sub->work++;
    if (!needs_work(f, c)) {
      if (f->run_start == SIZE_MAX) {
        f->run_start = f->pos;
      }
      f->run_toks += f->src->toks[c->id];
      f->pos++;
      continue;
    }
    /* something to do at this value: the run before it is complete */
    const bool spread = f->early_only && (f->src->flags[c->id] & F_SPREAD_FN) && f->pos >= 3;
    size_t cut = c->span.offset;
    const size_t save_pos = f->pos;
    if (spread) {
      /* the three dots before the function were copied with the run: they go with the function, not into the text */
      const GTEXT_CSS_Component_Value * d = &f->vals[f->pos - 3];
      cut = d->span.offset;
      f->run_toks -= f->src->toks[f->vals[f->pos - 1].id] + f->src->toks[f->vals[f->pos - 2].id] + f->src->toks[d->id];
      if (f->run_start != SIZE_MAX && f->run_start + 3 >= f->pos) {
        f->run_start = SIZE_MAX; /* the run was nothing but the dots */
      }
      f->pos -= 3;
    }
    const GTEXT_CSS_Status fst = seq_flush(sub, fi, cut);
    f = &sub->frames[fi];
    f->pos = save_pos + 1;
    if (fst != GTEXT_CSS_OK) {
      SUB_FAIL(sub, fst);
      return STEP_ERROR;
    }
    f->run_from = c->span.end;
    f->run_start = SIZE_MAX;
    f->run_toks = 0;
    const fn_kind k = kind_of(c);
    const GTEXT_CSS_Status st = child_frame(sub, fi, k != K_NONE ? frame_kind_of(k) : FR_CONTAINER, c);
    if (st != GTEXT_CSS_OK) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    return STEP_PUSHED;
  }
  /* the end: the last run, up to the end of the last value in it */
  if (f->run_start != SIZE_MAX && f->nvals) {
    const GTEXT_CSS_Status st = seq_flush(sub, fi, f->vals[f->nvals - 1].span.end);
    f = &sub->frames[fi];
    if (st != GTEXT_CSS_OK) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
  }
  if (f->gi) {
    f->out = NULL;
    f->out_gi = true;
  }
  else {
    f->out = f->acc ? f->acc : css_rope_retain(sub->empty);
    f->acc = NULL;
    f->out_gi = false;
  }
  return STEP_DONE;
}

/* ---- containers ---------------------------------------------------------------------------------------------- */

/// A function or block that only holds something to replace: `calc(1px + var(--x))`. Its opening and closing text are copied; what is between is a sequence.
static int step_container(css_sub * sub, size_t fi) {
  frame_t * f = &sub->frames[fi];
  const GTEXT_CSS_Component_Value * c = f->fn;
  if (f->phase == 0) {
    const GTEXT_CSS_Component_Value * first = &c->children[0];
    css_rope * open = NULL;
    GTEXT_CSS_Status st = make_leaf(sub, f->src, c->span.offset, first->span.offset, css_cv_first_class(c), CSS_TC_SAFE, 1, NULL, &open);
    if (st != GTEXT_CSS_OK) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    f->res[0].rope = open;
    f->res[0].set = true;
    f->phase = 1;
    if (seq_start(sub, fi, 1, f->src, c->children, c->child_count, f->early_only) < 0) {
      return STEP_ERROR;
    }
    return STEP_PUSHED;
  }
  take_arrived(f);
  css_rope * out = NULL;
  if (f->res[1].gi || f->res[0].gi) {
    f->out = NULL;
    f->out_gi = true;
    return STEP_DONE;
  }
  const GTEXT_CSS_Component_Value * last = &c->children[c->child_count - 1];
  css_rope * close = NULL;
  size_t from = last->span.end, to = c->span.end;
  char tail[600];
  size_t nt = 0;
  if (c->flags & GTEXT_CSS_FLAG_UNCLOSED) {
    /* the end of the input closed it: what was left after its last value is not copied, and the closer is written */
    tail[0] = c->kind == GTEXT_CSS_VALUE_FUNCTION || c->type == GTEXT_CSS_TOKEN_OPEN_PAREN ? ')' : c->type == GTEXT_CSS_TOKEN_OPEN_SQUARE ? ']' : '}';
    nt = 1;
    to = from;
  }
  {
    GTEXT_Allocator a = sub->alloc;
    const size_t body = to - from;
    char * buf = gtext_allocator_malloc(&a, body + nt + 1);
    if (!buf) {
      SUB_FAIL(sub, GTEXT_CSS_E_OOM);
      return STEP_ERROR;
    }
    memcpy(buf, f->src->text + from, body);
    memcpy(buf + body, tail, nt);
    close = css_rope_leaf(&sub->alloc, buf, body + nt, CSS_TC_SAFE, CSS_TC_SAFE, 1, f->src->tainted);
    gtext_allocator_free(&a, buf);
    if (!close) {
      SUB_FAIL(sub, GTEXT_CSS_E_OOM);
      return STEP_ERROR;
    }
  }
  css_rope * left = NULL;
  bool gi = false;
  GTEXT_CSS_Status st = rope_append(sub, f->res[0].rope, f->res[1].rope, &left, &gi);
  if (st == GTEXT_CSS_OK && !gi) {
    st = rope_append(sub, left, close, &out, &gi);
  }
  css_rope_release(left);
  css_rope_release(close);
  if (st != GTEXT_CSS_OK) {
    SUB_FAIL(sub, st);
    return STEP_ERROR;
  }
  f->out = out;
  f->out_gi = gi;
  return STEP_DONE;
}

/* ---- the contents of a function: the early pass and the arguments ----------------------------------------------- */

/// Break `vals` into arguments at the top-level tokens of type `sep` (the first only, if `first_only`), each trimmed of white space.
static GTEXT_CSS_Status split_args(css_sub * sub, frame_t * f, GTEXT_CSS_Token_Type sep, bool first_only) {
  size_t seps = 0;
  for (size_t i = 0; i < f->nvals; i++) {
    if (is_tok(&f->vals[i], sep) && (!first_only || seps == 0)) {
      seps++;
    }
  }
  GTEXT_Allocator a = sub->alloc;
  range_t * args = gtext_allocator_calloc(&a, seps + 1, sizeof *args);
  if (!args) {
    return GTEXT_CSS_E_OOM;
  }
  size_t n = 0, start = 0;
  bool first_done = false;
  for (size_t i = 0; i <= f->nvals; i++) {
    const bool end = i == f->nvals;
    if (end || (is_tok(&f->vals[i], sep) && (!first_only || !first_done))) {
      size_t b = start, e = i;
      while (b < e && is_ws(&f->vals[b])) {
        b++;
      }
      while (e > b && is_ws(&f->vals[e - 1])) {
        e--;
      }
      args[n].from = b;
      args[n].count = e - b;
      n++;
      start = i + 1;
      if (!end) {
        first_done = true;
        f->has_comma = true;
      }
    }
  }
  gtext_allocator_free(&a, f->args);
  f->args = args;
  f->nargs = n;
  return GTEXT_CSS_OK;
}

/// The text of a rope for reading as a small value (a name, an argument list): GTEXT_CSS_OK with `*text` NULL when it is longer than a name or a number can be.
static GTEXT_CSS_Status rope_text(css_sub * sub, const css_rope * r, size_t max, char ** text, size_t * len) {
  *text = NULL;
  *len = 0;
  if (css_rope_bytes(r) > max) {
    return GTEXT_CSS_OK;
  }
  return css_rope_flatten(&sub->alloc, r, max, text, len, NULL, NULL, &sub->work);
}

/// Run the common start of a function: the early pass if its contents have `...`, then the contents are `vals`. Returns STEP_PUSHED while the early pass runs, 0
/// when the contents are ready.
static int fn_prelude(css_sub * sub, size_t fi) {
  frame_t * f = &sub->frames[fi];
  if (f->phase == 0) {
    f->csrc = f->src;
    f->vals = f->fn->children;
    f->nvals = f->fn->child_count;
    if (f->nvals && (f->src->flags[f->fn->id] & F_HAS_SPREAD)) {
      f->phase = 1;
      if (seq_start(sub, fi, 2, f->src, f->vals, f->nvals, true) < 0) {
        return STEP_ERROR;
      }
      return STEP_PUSHED;
    }
    f->phase = 10;
    return 0;
  }
  if (f->phase == 1) {
    take_arrived(f);
    if (f->res[2].gi) {
      f->out = NULL;
      f->out_gi = true;
      return STEP_DONE;
    }
    char * text = NULL;
    size_t len = 0;
    GTEXT_CSS_Status st = css_rope_flatten(&sub->alloc, f->res[2].rope, sub->max_bytes, &text, &len, NULL, NULL, &sub->work);
    if (st == GTEXT_CSS_E_LIMIT_OUTPUT) {
      sub->limited++;
      f->out = NULL;
      f->out_gi = true;
      return STEP_DONE;
    }
    if (st == GTEXT_CSS_OK) {
      src_t * ns = NULL;
      st = src_open(&sub->alloc, text, len, text, f->src->tainted || css_rope_has_taint(f->res[2].rope), &ns);
      if (st == GTEXT_CSS_OK) {
        f = &sub->frames[fi];
        f->own_src = ns;
        f->csrc = ns;
        f->vals = ns->tree->values;
        f->nvals = ns->tree->value_count;
      }
    }
    if (st != GTEXT_CSS_OK) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    f->phase = 10;
    return 0;
  }
  return 0;
}

/// Start an argument as a sequence into `slot`: 0 immediate (empty), 1 started, -1 failure.
static int eval_arg(css_sub * sub, size_t fi, int slot, size_t arg) {
  frame_t * f = &sub->frames[fi];
  const range_t r = f->args[arg];
  /* an empty argument of a function with no values at all: NULL plus 0 is not allowed to be formed */
  return seq_start(sub, fi, slot, f->csrc, f->vals ? f->vals + r.from : NULL, r.count, false);
}

/// The result of a function is checked against the budget, and left in the frame.
static void fn_finish(css_sub * sub, size_t fi, css_rope * r /* taken */, bool gi) {
  frame_t * f = &sub->frames[fi];
  if (!gi && r) {
    const uint64_t t = css_rope_tokens(r);
    if (t > sub->peak) {
      sub->peak = t;
    }
    if (t > sub->max_tokens || css_rope_bytes(r) > sub->max_bytes) {
      sub->limited++;
      css_rope_release(r);
      r = NULL;
      gi = true;
    }
  }
  f->out = r;
  f->out_gi = gi || !r;
}

/* ---- small helpers for the functions ----------------------------------------------------------------------------- */

typedef struct {
  GTEXT_Allocator alloc;
  char * data;
  size_t len, cap;
  bool failed;
} dbuf;

static void dbuf_init(dbuf * b, const GTEXT_Allocator * alloc) {
  memset(b, 0, sizeof *b);
  b->alloc = *alloc;
}

static void dbuf_free(dbuf * b) {
  gtext_allocator_free(&b->alloc, b->data);
  b->data = NULL;
}

static bool dbuf_put(dbuf * b, const char * s, size_t n) {
  if (b->failed) {
    return false;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + n + 1) {
      if (cap > SIZE_MAX / 2) {
        b->failed = true;
        return false;
      }
      cap *= 2;
    }
    char * g = gtext_allocator_realloc(&b->alloc, b->data, cap);
    if (!g) {
      b->failed = true;
      return false;
    }
    b->data = g;
    b->cap = cap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return true;
}

static GTEXT_Markup_Status dbuf_sink_write(void * user, const char * data, size_t n) {
  return dbuf_put(user, data, n) ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_OOM;
}

static GTEXT_Markup_Sink dbuf_sink(dbuf * b) {
  GTEXT_Markup_Sink s;
  memset(&s, 0, sizeof s);
  s.user = b;
  s.write = dbuf_sink_write;
  return s;
}

/// Put the value of a custom property (or the guaranteed-invalid value) in a result slot of a frame.
static void slot_set(frame_t * f, int slot, const GTEXT_CSS_Custom_Value * v) {
  slot_clear(&f->res[slot]);
  f->res[slot].set = true;
  if (v) {
    f->res[slot].rope = css_rope_retain(css_cval_rope(v));
  }
  else {
    f->res[slot].gi = true;
  }
}

static void slot_gi(frame_t * f, int slot) {
  slot_clear(&f->res[slot]);
  f->res[slot].set = true;
  f->res[slot].gi = true;
}

/// Read a rope as component values, if it is short enough (`max` bytes). `*tree` is NULL when it is not. The text is released; the tree keeps what it needs.
static GTEXT_CSS_Status small_parse(css_sub * sub, const css_rope * r, size_t max, GTEXT_CSS_Tree ** tree) {
  *tree = NULL;
  char * text = NULL;
  size_t len = 0;
  GTEXT_CSS_Status st = rope_text(sub, r, max, &text, &len);
  if (st != GTEXT_CSS_OK || !text) {
    return st;
  }
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = &sub->alloc;
  po.count_errors_only = true;
  st = gtext_css_parse_component_values(text, len, &po, tree, NULL);
  gtext_allocator_free(&sub->alloc, text);
  return st;
}

/// The one non-white-space value of a tree, or NULL when there are none or several.
static const GTEXT_CSS_Component_Value * single(const GTEXT_CSS_Tree * t) {
  const GTEXT_CSS_Component_Value * found = NULL;
  for (size_t i = 0; i < t->value_count; i++) {
    if (is_ws(&t->values[i])) {
      continue;
    }
    if (found) {
      return NULL;
    }
    found = &t->values[i];
  }
  return found;
}

/// Read a rope as a `<custom-property-name>`: the name is copied into the frame (`f->u.n`). 1 if it is one, 0 if not, -1 on failure.
static int read_name(css_sub * sub, size_t fi, const css_rope * r) {
  GTEXT_CSS_Tree * t = NULL;
  const GTEXT_CSS_Status st = small_parse(sub, r, 65536, &t);
  if (st == GTEXT_CSS_E_OOM) {
    SUB_FAIL(sub, st);
    return -1;
  }
  if (st != GTEXT_CSS_OK || !t) {
    return 0; /* too long to be a name, or a limit of the parser */
  }
  const GTEXT_CSS_Component_Value * v = single(t);
  int result = 0;
  if (v && v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_IDENT && css_creg_name_ok(v->value.data, v->value.length)) {
    GTEXT_Allocator a = sub->alloc;
    char * copy = gtext_allocator_malloc(&a, v->value.length + 1);
    if (!copy) {
      SUB_FAIL(sub, GTEXT_CSS_E_OOM);
      result = -1;
    }
    else {
      memcpy(copy, v->value.data, v->value.length);
      copy[v->value.length] = '\0';
      frame_t * f = &sub->frames[fi];
      gtext_allocator_free(&a, f->u.n.name);
      f->u.n.name = copy;
      f->u.n.name_len = v->value.length;
      result = 1;
    }
  }
  gtext_css_tree_free(t);
  return result;
}

/* ---- the custom properties of the element ------------------------------------------------------------------------- */

static int entry_cmp(const void * x, const void * y) {
  const entry_t * a = x;
  const entry_t * b = y;
  const size_t n = a->name.length < b->name.length ? a->name.length : b->name.length;
  const int c = n ? memcmp(a->name.data, b->name.data, n) : 0;
  if (c) {
    return c;
  }
  if (a->name.length != b->name.length) {
    return a->name.length < b->name.length ? -1 : 1;
  }
  return a->item < b->item ? -1 : a->item > b->item ? 1 : 0;
}

static entry_t * find_entry(css_sub * sub, const char * name, size_t len) {
  size_t lo = 0, hi = sub->nentries;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    entry_t * e = &sub->entries[mid];
    const size_t n = len < e->name.length ? len : e->name.length;
    int c = n ? memcmp(name, e->name.data, n) : 0;
    if (c == 0) {
      c = len < e->name.length ? -1 : len > e->name.length ? 1 : 0;
    }
    sub->work++;
    if (c == 0) {
      return e;
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return NULL;
}

/// The value the parent has for a name (borrowed), for `inherit`, `inherit()` and a property that inherits. NULL: the guaranteed-invalid value.
static const GTEXT_CSS_Custom_Value * parent_value(css_sub * sub, const char * name, size_t len, long reg) {
  const GTEXT_CSS_Computed_Style * parent = sub->st->parent;
  if (reg >= 0) {
    return parent && parent->regvals ? parent->regvals[reg] : sub->cc->regs[reg].initial;
  }
  return parent ? css_cmap_find(parent->custom, name, len) : NULL;
}

/// The value of a name that the element does not declare.
static const GTEXT_CSS_Custom_Value * undeclared_value(css_sub * sub, const char * name, size_t len) {
  size_t reg = 0;
  if (sub->cc->nregs && css_ctx_find_reg(sub->cc, name, len, &reg)) {
    const css_ereg * e = &sub->cc->regs[reg];
    return e->creg->inherits ? parent_value(sub, name, len, (long)reg) : e->initial;
  }
  return parent_value(sub, name, len, -1);
}

/// Look a custom property up for the frame `fi`, putting its value in `slot`. 0: it is there (rope or the guaranteed-invalid value); 1: a frame was started that
/// will deliver it; -1: failure. `*in_progress` says that the property is the one being computed (a `style()` query is false then, and nothing is marked).
static int lookup_custom(css_sub * sub, size_t fi, int slot, const char * name, size_t len, bool query, bool * in_progress) {
  if (in_progress) {
    *in_progress = false;
  }
  entry_t * e = find_entry(sub, name, len);
  if (!e) {
    slot_set(&sub->frames[fi], slot, undeclared_value(sub, name, len));
    return 0;
  }
  switch (e->state) {
  case ENT_DONE:
    slot_set(&sub->frames[fi], slot, e->value);
    return 0;
  case ENT_INPROGRESS:
    if (query) {
      if (in_progress) {
        *in_progress = true;
      }
    }
    else {
      mark_cyclic(sub, e->frame);
    }
    slot_gi(&sub->frames[fi], slot);
    return 0;
  default:
    break;
  }
  const size_t ei = (size_t)(e - sub->entries);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  frame_t * p = frame_push(sub, FR_PROP, &st);
  if (!p) {
    SUB_FAIL(sub, st);
    return -1;
  }
  p->entry = ei;
  sub->frames[fi].wait_slot = slot;
  return 1;
}

/* ---- var(), inherit() --------------------------------------------------------------------------------------------- */

static int step_var(css_sub * sub, size_t fi) {
  frame_t * f = &sub->frames[fi];
  int pre = fn_prelude(sub, fi);
  if (pre) {
    return pre;
  }
  f = &sub->frames[fi];
  if (f->has_arrived) {
    take_arrived(f);
  }
  const bool inherit = f->kind == FR_INHERIT;
  switch (f->phase) {
  case 10: {
    GTEXT_CSS_Status st = split_args(sub, f, GTEXT_CSS_TOKEN_COMMA, true);
    if (st != GTEXT_CSS_OK) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    f->phase = 11;
    const int r = eval_arg(sub, fi, 0, 0);
    if (r < 0) {
      return STEP_ERROR;
    }
    if (r == 1) {
      return STEP_PUSHED;
    }
    f = &sub->frames[fi];
  }
  /* fall through */
  case 11: {
    /* the name: the first argument, replaced, read as a <custom-property-name> */
    int ok = 0;
    if (!f->res[0].gi && f->res[0].rope) {
      ok = read_name(sub, fi, f->res[0].rope);
      if (ok < 0) {
        return STEP_ERROR;
      }
    }
    f = &sub->frames[fi];
    f->phase = 12;
    if (ok == 1) {
      if (inherit) {
        size_t reg = 0;
        const bool reged = sub->cc->nregs && css_ctx_find_reg(sub->cc, f->u.n.name, f->u.n.name_len, &reg);
        slot_set(f, 1, parent_value(sub, f->u.n.name, f->u.n.name_len, reged ? (long)reg : -1));
      }
      else {
        const int r = lookup_custom(sub, fi, 1, f->u.n.name, f->u.n.name_len, false, NULL);
        if (r < 0) {
          return STEP_ERROR;
        }
        if (r == 1) {
          return STEP_PUSHED;
        }
      }
    }
    else {
      slot_gi(f, 1);
    }
    f = &sub->frames[fi];
  }
  /* fall through */
  case 12:
    if (f->res[1].gi && f->has_comma) {
      /* "If result contains the guaranteed-invalid value, and second arg was provided, set result to the result of substituting second arg." */
      f->phase = 13;
      const int r = eval_arg(sub, fi, 2, 1);
      if (r < 0) {
        return STEP_ERROR;
      }
      if (r == 1) {
        return STEP_PUSHED;
      }
      f = &sub->frames[fi];
      f->phase = 13;
    }
    else {
      css_rope * out = css_rope_retain(f->res[1].rope);
      const bool gi = f->res[1].gi;
      fn_finish(sub, fi, out, gi);
      return STEP_DONE;
    }
  /* fall through */
  default: {
    css_rope * out = css_rope_retain(f->res[2].rope);
    const bool gi = f->res[2].gi;
    fn_finish(sub, fi, out, gi);
    return STEP_DONE;
  }
  }
}

/* ---- env() -------------------------------------------------------------------------------------------------------- */

static int step_env(css_sub * sub, size_t fi) {
  int pre = fn_prelude(sub, fi);
  if (pre) {
    return pre;
  }
  frame_t * f = &sub->frames[fi];
  if (f->has_arrived) {
    take_arrived(f);
  }
  switch (f->phase) {
  case 10: {
    GTEXT_CSS_Status st = split_args(sub, f, GTEXT_CSS_TOKEN_COMMA, true);
    if (st != GTEXT_CSS_OK) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    f->phase = 11;
    const int r = eval_arg(sub, fi, 0, 0);
    if (r < 0) {
      return STEP_ERROR;
    }
    if (r == 1) {
      return STEP_PUSHED;
    }
    f = &sub->frames[fi];
  }
  /* fall through */
  case 11: {
    /* env( <custom-ident> <integer [0,∞]>* [ , fallback ]? ) */
    f->phase = 12;
    slot_gi(f, 1);
    GTEXT_CSS_Tree * t = NULL;
    if (!f->res[0].gi && f->res[0].rope) {
      const GTEXT_CSS_Status st = small_parse(sub, f->res[0].rope, 65536, &t);
      if (st == GTEXT_CSS_E_OOM) {
        SUB_FAIL(sub, st);
        return STEP_ERROR;
      }
    }
    f = &sub->frames[fi];
    if (t && sub->cc->env.env_variable) {
      GTEXT_CSS_String name = {NULL, 0};
      uint32_t idx[16];
      size_t nidx = 0;
      bool ok = true;
      for (size_t i = 0; i < t->value_count && ok; i++) {
        const GTEXT_CSS_Component_Value * v = &t->values[i];
        if (is_ws(v)) {
          continue;
        }
        if (!name.data) {
          ok = v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_IDENT;
          if (ok) {
            name = v->value;
          }
        }
        else {
          ok = v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_NUMBER && (v->flags & GTEXT_CSS_FLAG_INTEGER) && v->number >= 0 &&
              v->number <= 4294967295.0 && nidx < sizeof idx / sizeof *idx;
          if (ok) {
            idx[nidx++] = (uint32_t)v->number;
          }
        }
      }
      if (ok && name.data) {
        GTEXT_CSS_String out = {NULL, 0};
        if (sub->cc->env.env_variable(sub->cc->env.user, &name, idx, nidx, &out) && out.data) {
          css_rope * r = NULL;
          const GTEXT_CSS_Status st = css_rope_from_text(&sub->alloc, out.data, out.length, false, &r);
          if (st != GTEXT_CSS_OK) {
            gtext_css_tree_free(t);
            SUB_FAIL(sub, st);
            return STEP_ERROR;
          }
          f = &sub->frames[fi];
          slot_clear(&f->res[1]);
          f->res[1].rope = r;
          f->res[1].set = true;
        }
      }
    }
    gtext_css_tree_free(t);
    f = &sub->frames[fi];
  }
  /* fall through */
  case 12:
    if (f->res[1].gi && f->has_comma) {
      f->phase = 13;
      const int r = eval_arg(sub, fi, 2, 1);
      if (r < 0) {
        return STEP_ERROR;
      }
      if (r == 1) {
        return STEP_PUSHED;
      }
      f = &sub->frames[fi];
    }
    else {
      css_rope * out = css_rope_retain(f->res[1].rope);
      const bool gi = f->res[1].gi;
      fn_finish(sub, fi, out, gi);
      return STEP_DONE;
    }
  /* fall through */
  default: {
    css_rope * out = css_rope_retain(f->res[2].rope);
    const bool gi = f->res[2].gi;
    fn_finish(sub, fi, out, gi);
    return STEP_DONE;
  }
  }
}

/* ---- sibling-index(), sibling-count() ------------------------------------------------------------------------------- */

static int step_tree(css_sub * sub, size_t fi) {
  frame_t * f = &sub->frames[fi];
  const bool index = fn_name_is(&f->fn->value, "sibling-index");
  double n = 0;
  GTEXT_CSS_Status st = css_st_sibling(sub->st, index, &n);
  if (st != GTEXT_CSS_OK) {
    SUB_FAIL(sub, st);
    return STEP_ERROR;
  }
  char buf[340];
  const size_t len = css_cssom_number(n, true, buf);
  css_rope * r = NULL;
  st = css_rope_from_text(&sub->alloc, buf, len, false, &r);
  if (st != GTEXT_CSS_OK) {
    SUB_FAIL(sub, st);
    return STEP_ERROR;
  }
  f = &sub->frames[fi];
  f->out = r;
  f->out_gi = false;
  return STEP_DONE;
}

/* ---- ident() -------------------------------------------------------------------------------------------------------- */

static int step_ident(css_sub * sub, size_t fi) {
  int pre = fn_prelude(sub, fi);
  if (pre) {
    return pre;
  }
  frame_t * f = &sub->frames[fi];
  if (f->has_arrived) {
    take_arrived(f);
  }
  if (f->phase == 10) {
    /* ident( [ <string> | <integer> | <ident> ]+ ): one argument */
    GTEXT_Allocator a = sub->alloc;
    f->args = gtext_allocator_calloc(&a, 1, sizeof *f->args);
    if (!f->args) {
      SUB_FAIL(sub, GTEXT_CSS_E_OOM);
      return STEP_ERROR;
    }
    size_t b = 0, e = f->nvals;
    while (b < e && is_ws(&f->vals[b])) {
      b++;
    }
    while (e > b && is_ws(&f->vals[e - 1])) {
      e--;
    }
    f->args[0].from = b;
    f->args[0].count = e - b;
    f->nargs = 1;
    f->phase = 11;
    const int r = eval_arg(sub, fi, 0, 0);
    if (r < 0) {
      return STEP_ERROR;
    }
    if (r == 1) {
      return STEP_PUSHED;
    }
    f = &sub->frames[fi];
  }
  /* "Substitute arbitrary substitution functions in arg, then parse it as <ident-arg>+ ... Replace each item with its computed value ... Serialize each item, then
     concatenate ... If the result can't be represented as an ident (for example, the empty string), return the guaranteed-invalid value." */
  GTEXT_CSS_Tree * t = NULL;
  bool good = !f->res[0].gi && f->res[0].rope;
  if (good) {
    const GTEXT_CSS_Status st = small_parse(sub, f->res[0].rope, sub->max_bytes, &t);
    if (st == GTEXT_CSS_E_OOM) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    good = t != NULL;
  }
  dbuf pieces;
  dbuf_init(&pieces, &sub->alloc);
  bool taint = f->csrc->tainted || (f->res[0].rope && css_rope_has_taint(f->res[0].rope));
  size_t items = 0;
  for (size_t i = 0; good && i < t->value_count; i++) {
    const GTEXT_CSS_Component_Value * v = &t->values[i];
    if (is_ws(v)) {
      continue;
    }
    items++;
    if (v->kind != GTEXT_CSS_VALUE_TOKEN) {
      good = false;
    }
    else if (v->type == GTEXT_CSS_TOKEN_STRING || v->type == GTEXT_CSS_TOKEN_IDENT) {
      good = dbuf_put(&pieces, v->value.data ? v->value.data : "", v->value.length);
    }
    else if (v->type == GTEXT_CSS_TOKEN_NUMBER && (v->flags & GTEXT_CSS_FLAG_INTEGER)) {
      char nb[340];
      const size_t nl = css_cssom_number(v->number, true, nb);
      good = dbuf_put(&pieces, nb, nl);
    }
    else {
      good = false;
    }
  }
  css_rope * r = NULL;
  if (good && !pieces.failed && items > 0 && pieces.len > 0) {
    dbuf ident;
    dbuf_init(&ident, &sub->alloc);
    GTEXT_Markup_Sink sink = dbuf_sink(&ident);
    const GTEXT_CSS_Status st = gtext_css_serialize_identifier(pieces.data, pieces.len, &sink);
    if (st == GTEXT_CSS_OK && !ident.failed) {
      const GTEXT_CSS_Status rs = css_rope_from_text(&sub->alloc, ident.data, ident.len, taint, &r);
      if (rs != GTEXT_CSS_OK) {
        dbuf_free(&ident);
        dbuf_free(&pieces);
        gtext_css_tree_free(t);
        SUB_FAIL(sub, rs);
        return STEP_ERROR;
      }
    }
    else if (ident.failed || st == GTEXT_CSS_E_OOM) {
      dbuf_free(&ident);
      dbuf_free(&pieces);
      gtext_css_tree_free(t);
      SUB_FAIL(sub, GTEXT_CSS_E_OOM);
      return STEP_ERROR;
    }
    dbuf_free(&ident);
  }
  const bool oom = pieces.failed;
  dbuf_free(&pieces);
  gtext_css_tree_free(t);
  if (oom) {
    SUB_FAIL(sub, GTEXT_CSS_E_OOM);
    return STEP_ERROR;
  }
  fn_finish(sub, fi, r, r == NULL);
  return STEP_DONE;
}

/* ---- random(), random-item() ------------------------------------------------------------------------------------- */

typedef struct {
  bool valid;
  bool fixed;
  double fixed_value;
  char name[256];
  size_t name_len;
  bool element_scoped;
  bool property_scoped, property_index_scoped;
  char ua[256];
  size_t ua_len;
} random_key;

static uint64_t mix64(uint64_t h) {
  h ^= h >> 30;
  h *= 0xBF58476D1CE4E5B9ULL;
  h ^= h >> 27;
  h *= 0x94D049BB133111EBULL;
  h ^= h >> 31;
  return h;
}

static uint64_t fnv_bytes(uint64_t h, const void * data, size_t n) {
  const unsigned char * p = data;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ p[i]) * 0x100000001b3ULL;
  }
  return h;
}

/// Read a <random-key> from the values of a group: "auto | <random-cache-key> | fixed <number [0,1]>", where "<random-cache-key> = <dashed-ident> ||
/// element-scoped || [ property-scoped | property-index-scoped | <random-ua-ident> ]".
static void read_random_key(const GTEXT_CSS_Component_Value * v, size_t n, random_key * k) {
  memset(k, 0, sizeof *k);
  size_t items = 0;
  bool have_name = false, have_scope = false, have_el = false;
  for (size_t i = 0; i < n; i++) {
    if (is_ws(&v[i])) {
      continue;
    }
    items++;
    if (v[i].kind != GTEXT_CSS_VALUE_TOKEN) {
      return;
    }
    if (v[i].type == GTEXT_CSS_TOKEN_IDENT) {
      const GTEXT_CSS_String * s = &v[i].value;
      if (tok_ieq(s, "auto")) {
        if (items != 1) {
          return;
        }
        k->element_scoped = true;
        k->property_index_scoped = true;
        have_name = have_scope = have_el = true; /* nothing else may follow */
        k->valid = true;
        continue;
      }
      if (tok_ieq(s, "fixed")) {
        if (items != 1) {
          return;
        }
        size_t j = i + 1;
        while (j < n && is_ws(&v[j])) {
          j++;
        }
        if (j >= n || v[j].kind != GTEXT_CSS_VALUE_TOKEN || v[j].type != GTEXT_CSS_TOKEN_NUMBER || v[j].number < 0 || v[j].number > 1) {
          return;
        }
        for (size_t m = j + 1; m < n; m++) {
          if (!is_ws(&v[m])) {
            return;
          }
        }
        k->fixed = true;
        k->fixed_value = v[j].number;
        k->valid = true;
        return;
      }
      if (tok_ieq(s, "element-scoped")) {
        if (have_el) {
          return;
        }
        have_el = true;
        k->element_scoped = true;
      }
      else if (tok_ieq(s, "property-scoped") || tok_ieq(s, "property-index-scoped")) {
        if (have_scope) {
          return;
        }
        have_scope = true;
        k->property_scoped = tok_ieq(s, "property-scoped");
        k->property_index_scoped = !k->property_scoped;
      }
      else if (s->length >= 2 && s->data[0] == '-' && s->data[1] == '-') {
        if (have_name || s->length > sizeof k->name - 1) {
          return;
        }
        have_name = true;
        memcpy(k->name, s->data, s->length);
        k->name_len = s->length;
      }
      else if (s->length >= 3 && s->data[0] == 'u' && s->data[1] == 'a' && s->data[2] == '-') {
        if (have_scope || s->length > sizeof k->ua - 1) {
          return;
        }
        have_scope = true;
        memcpy(k->ua, s->data, s->length);
        k->ua_len = s->length;
      }
      else {
        return;
      }
    }
    else {
      return;
    }
  }
  k->valid = items > 0;
}

/// The random base value (0 up to but not including 1) of a key for a property, given the environment's callback and seed.
static double random_base_core(const GTEXT_CSS_Compute_Context * cc, GTEXT_CSS_Element_Handle element, const char * prop, size_t prop_len, const random_key * k,
    uint32_t index) {
  const double below_one = 1.0 - 1.1102230246251565e-16;
  if (k->fixed) {
    return k->fixed_value >= 1.0 ? below_one : k->fixed_value;
  }
  char ua[512];
  size_t ua_len = 0;
  if (k->ua_len) {
    memcpy(ua, k->ua, k->ua_len);
    ua_len = k->ua_len;
  }
  else if (k->property_scoped || k->property_index_scoped) {
    static const char pre[] = "ua-";
    memcpy(ua, pre, 3);
    ua_len = 3;
    const size_t pl = prop_len < 200 ? prop_len : 200;
    if (pl) { /* a custom property being computed names no property: prop is NULL */
      memcpy(ua + ua_len, prop, pl);
    }
    ua_len += pl;
    if (k->property_index_scoped) {
      ua[ua_len++] = '-';
      ua_len += css_cssom_number((double)index, true, ua + ua_len);
    }
  }
  GTEXT_CSS_Random_Key key;
  memset(&key, 0, sizeof key);
  if (k->name_len) {
    key.name.data = k->name;
    key.name.length = k->name_len;
  }
  if (ua_len) {
    key.ident.data = ua;
    key.ident.length = ua_len;
  }
  key.element_scoped = k->element_scoped;
  key.element = k->element_scoped ? element : NULL;
  const GTEXT_CSS_Environment * env = &cc->env;
  double base = 0;
  if (env->random_base && env->random_base(env->user, &key, &base) && base >= 0 && base < 1) {
    return base;
  }
  uint64_t h = 0xcbf29ce484222325ULL ^ env->random_seed;
  h = fnv_bytes(h, k->name, k->name_len);
  h = fnv_bytes(h, "\0", 1);
  h = fnv_bytes(h, ua, ua_len);
  h = fnv_bytes(h, "\0", 1);
  if (k->element_scoped) {
    const uintptr_t p = (uintptr_t)element;
    h = fnv_bytes(h, &p, sizeof p);
  }
  h = mix64(h ^ env->random_seed);
  return (double)(h >> 11) * (1.0 / 9007199254740992.0);
}

static double random_base_of(css_sub * sub, const random_key * k, uint32_t index) {
  return random_base_core(sub->cc, sub->st->element, sub->prop, sub->prop_len, k, index);
}

double css_random_pick(double a, double b, double step, bool has_step, double R) {
  if (b < a) {
    b = a; /* "If the maximum value is less than the minimum value, it behaves as if it's equal to the minimum value." */
  }
  if (!has_step || !(step > 0)) {
    return a + R * (b - a);
  }
  if (!(step == step) || step > DBL_MAX || step < -DBL_MAX) {
    return a; /* "If C is infinite, the result is A." */
  }
  /* 9.3: epsilon is step / 1000; N the largest integer such that min + N * step <= max, bumped by one when the next step is within epsilon of max */
  const double eps = step / 1000.0;
  double N = gmath_floor((b - a) / step);
  if (!(gmath_fabs((a + N * step) - b) <= eps) && gmath_fabs((a + (N + 1) * step) - b) <= eps) {
    N += 1;
  }
  double index = gmath_floor(R * (N + 1));
  if (index > N) {
    index = N;
  }
  const double v = a + index * step;
  return index == N && gmath_fabs(v - b) <= eps ? b : v;
}

bool css_st_random_base(css_state * st, const char * key, size_t key_length, double * base) {
  random_key k;
  if (key_length == 0) {
    memset(&k, 0, sizeof k);
    k.valid = true;
    k.element_scoped = true;
    k.property_index_scoped = true; /* auto */
  }
  else {
    GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
    po.allocator = &st->cc->alloc;
    po.count_errors_only = true;
    GTEXT_CSS_Tree * t = NULL;
    const GTEXT_CSS_Status status = gtext_css_parse_component_values(key, key_length, &po, &t, NULL);
    if (status != GTEXT_CSS_OK) {
      /* a key that could not be read is not one that is wrong: a failed allocation is that, and says so (the first failure of a computation is the one it reports) */
      css_st_fail(st, status, "random", NULL);
      return false;
    }
    else {
      read_random_key(t->values, t->value_count, &k);
      gtext_css_tree_free(t);
    }
  }
  if (!k.valid) {
    return false;
  }
  const css_pmeta * m = st->current && st->current <= st->cc->nprops ? &st->cc->meta[st->current - 1] : NULL;
  *base = random_base_core(st->cc, st->element, m ? m->name : "", m ? m->name_length : 0, &k, st->random_index++);
  return true;
}

static int step_random_item(css_sub * sub, size_t fi) {
  int pre = fn_prelude(sub, fi);
  if (pre) {
    return pre;
  }
  frame_t * f = &sub->frames[fi];
  if (f->has_arrived) {
    take_arrived(f);
  }
  if (f->phase == 10) {
    GTEXT_CSS_Status st = split_args(sub, f, GTEXT_CSS_TOKEN_COMMA, false);
    if (st != GTEXT_CSS_OK) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    f->phase = 11;
    const int r = eval_arg(sub, fi, 0, 0);
    if (r < 0) {
      return STEP_ERROR;
    }
    if (r == 1) {
      return STEP_PUSHED;
    }
    f = &sub->frames[fi];
  }
  if (f->phase == 11) {
    /* "Substitute arbitrary substitution functions in value sharing, then parse it as <random-key>. If parsing returns failure, return the guaranteed-invalid
       value ... Select one of the options ... Substitute arbitrary substitution functions in chosen option, then return the result." */
    const uint32_t index = sub->random_index++;
    GTEXT_CSS_Tree * t = NULL;
    random_key key;
    memset(&key, 0, sizeof key);
    if (!f->res[0].gi && f->res[0].rope) {
      const GTEXT_CSS_Status st = small_parse(sub, f->res[0].rope, 4096, &t);
      if (st == GTEXT_CSS_E_OOM) {
        SUB_FAIL(sub, st);
        return STEP_ERROR;
      }
      if (t) {
        read_random_key(t->values, t->value_count, &key);
        gtext_css_tree_free(t);
      }
    }
    f = &sub->frames[fi];
    if (!key.valid || f->nargs < 2) {
      fn_finish(sub, fi, NULL, true);
      return STEP_DONE;
    }
    const size_t options = f->nargs - 1;
    const double base = random_base_of(sub, &key, index);
    size_t pick = (size_t)gmath_floor(base * (double)options);
    if (pick >= options) {
      pick = options - 1;
    }
    range_t r = f->args[1 + pick];
    src_t * src = f->csrc;
    const GTEXT_CSS_Component_Value * vals = f->vals ? f->vals + r.from : NULL;
    size_t n = r.count;
    if (n == 1 && vals[0].kind == GTEXT_CSS_VALUE_BLOCK && vals[0].type == GTEXT_CSS_TOKEN_OPEN_CURLY) {
      /* a {} wrapper is the author's way of saying "the commas inside are mine": the contents are the option */
      n = vals[0].child_count;
      vals = vals[0].children;
    }
    f->phase = 12;
    const int rr = seq_start(sub, fi, 1, src, vals, n, false);
    if (rr < 0) {
      return STEP_ERROR;
    }
    if (rr == 1) {
      return STEP_PUSHED;
    }
    f = &sub->frames[fi];
  }
  css_rope * out = css_rope_retain(f->res[1].rope);
  const bool gi = f->res[1].gi;
  fn_finish(sub, fi, out, gi);
  return STEP_DONE;
}

typedef struct {
  bool literal;
  double value;
  GTEXT_CSS_Unit unit;
  bool percent;
  bool plain;
} numlit;

static void read_literal(const GTEXT_CSS_Component_Value * v, size_t n, numlit * out) {
  memset(out, 0, sizeof *out);
  const GTEXT_CSS_Component_Value * one = NULL;
  for (size_t i = 0; i < n; i++) {
    if (is_ws(&v[i])) {
      continue;
    }
    if (one) {
      return;
    }
    one = &v[i];
  }
  if (!one || one->kind != GTEXT_CSS_VALUE_TOKEN) {
    return;
  }
  if (one->type == GTEXT_CSS_TOKEN_NUMBER) {
    out->literal = out->plain = true;
    out->value = one->number;
  }
  else if (one->type == GTEXT_CSS_TOKEN_PERCENTAGE) {
    out->literal = out->percent = true;
    out->value = one->number;
  }
  else if (one->type == GTEXT_CSS_TOKEN_DIMENSION) {
    const GTEXT_CSS_Unit u = gtext_css_unit_lookup(one->unit.data, one->unit.length);
    if (u != GTEXT_CSS_UNIT_NONE) {
      out->literal = true;
      out->unit = u;
      out->value = one->number;
    }
  }
}

/// The text of a group of values as it is written back.
static GTEXT_CSS_Status group_text(css_sub * sub, const GTEXT_CSS_Component_Value * v, size_t n, dbuf * out) {
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &sub->alloc;
  char * text = NULL;
  size_t len = 0;
  const GTEXT_CSS_Status st = gtext_css_serialize_values_buffer(v, n, &so, &text, &len);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const bool ok = dbuf_put(out, text, len);
  gtext_css_serialize_buffer_free(&so, text);
  return ok ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

static int step_random(css_sub * sub, size_t fi) {
  int pre = fn_prelude(sub, fi);
  if (pre) {
    return pre;
  }
  frame_t * f = &sub->frames[fi];
  if (f->has_arrived) {
    take_arrived(f);
  }
  if (f->phase == 10) {
    GTEXT_Allocator a = sub->alloc;
    f->args = gtext_allocator_calloc(&a, 1, sizeof *f->args);
    if (!f->args) {
      SUB_FAIL(sub, GTEXT_CSS_E_OOM);
      return STEP_ERROR;
    }
    f->args[0].from = 0;
    f->args[0].count = f->nvals;
    f->nargs = 1;
    f->phase = 11;
    const int r = eval_arg(sub, fi, 0, 0);
    if (r < 0) {
      return STEP_ERROR;
    }
    if (r == 1) {
      return STEP_PUSHED;
    }
    f = &sub->frames[fi];
  }
  /* random( <random-key>? , <calc-sum>, <calc-sum>, <calc-sum>? ): the arguments are read once what is inside them is replaced */
  const uint32_t index = sub->random_index++;
  if (f->res[0].gi || !f->res[0].rope) {
    fn_finish(sub, fi, NULL, true);
    return STEP_DONE;
  }
  GTEXT_CSS_Tree * t = NULL;
  GTEXT_CSS_Status st = small_parse(sub, f->res[0].rope, sub->max_bytes, &t);
  if (st == GTEXT_CSS_E_OOM) {
    SUB_FAIL(sub, st);
    return STEP_ERROR;
  }
  f = &sub->frames[fi];
  const bool taint = f->csrc->tainted || css_rope_has_taint(f->res[0].rope);
  struct { size_t from, count; } g[5];
  size_t ng = 0;
  if (t) {
    size_t start = 0;
    for (size_t i = 0; i <= t->value_count; i++) {
      if (i == t->value_count || is_tok(&t->values[i], GTEXT_CSS_TOKEN_COMMA)) {
        if (ng < 5) {
          size_t b = start, e = i;
          while (b < e && is_ws(&t->values[b])) {
            b++;
          }
          while (e > b && is_ws(&t->values[e - 1])) {
            e--;
          }
          g[ng].from = b;
          g[ng].count = e - b;
        }
        ng++;
        start = i + 1;
      }
    }
  }
  random_key key;
  memset(&key, 0, sizeof key);
  size_t first = 0;
  css_rope * out = NULL;
  bool gi = true;
  if (t && ng >= 2 && ng <= 4) {
    bool has_key = false;
    if (ng >= 3) {
      read_random_key(t->values + g[0].from, g[0].count, &key);
      has_key = key.valid;
    }
    if (ng == 4 && !has_key) {
      gtext_css_tree_free(t);
      fn_finish(sub, fi, NULL, true);
      return STEP_DONE;
    }
    if (!has_key) {
      memset(&key, 0, sizeof key);
      key.valid = true;
      key.element_scoped = true;
      key.property_index_scoped = true; /* "auto" */
    }
    first = has_key ? 1 : 0;
    const size_t nargs = ng - first;
    if (nargs >= 2 && nargs <= 3) {
      const double R = random_base_of(sub, &key, index);
      numlit mn, mx, sp;
      read_literal(t->values + g[first].from, g[first].count, &mn);
      read_literal(t->values + g[first + 1].from, g[first + 1].count, &mx);
      memset(&sp, 0, sizeof sp);
      if (nargs == 3) {
        read_literal(t->values + g[first + 2].from, g[first + 2].count, &sp);
      }
      /* literals of one kind are computed here; anything else becomes a calculation the math layer finishes */
      bool same = mn.literal && mx.literal && (nargs == 2 || sp.literal);
      double a = mn.value, b = mx.value, stp = sp.value;
      GTEXT_CSS_Unit unit = mn.unit;
      bool percent = mn.percent, plain = mn.plain;
      if (same) {
        same = mn.percent == mx.percent && mn.plain == mx.plain && (nargs == 2 || (sp.percent == mn.percent && sp.plain == mn.plain));
      }
      if (same && !mn.percent && !mn.plain) {
        const GTEXT_CSS_Unit_Type tt = gtext_css_unit_type(mn.unit);
        if (gtext_css_unit_type(mx.unit) != tt || (nargs == 3 && gtext_css_unit_type(sp.unit) != tt)) {
          same = false;
        }
        else if (mn.unit != mx.unit || (nargs == 3 && mn.unit != sp.unit)) {
          /* different units of one type: both in the canonical unit, if they are absolute */
          double ca, cb, cs = 0;
          if (gtext_css_unit_to_canonical(mn.unit, mn.value, &ca) && gtext_css_unit_to_canonical(mx.unit, mx.value, &cb) &&
              (nargs == 2 || gtext_css_unit_to_canonical(sp.unit, sp.value, &cs))) {
            a = ca;
            b = cb;
            stp = cs;
            unit = gtext_css_unit_canonical(mn.unit);
          }
          else {
            same = false;
          }
        }
      }
      dbuf text;
      dbuf_init(&text, &sub->alloc);
      bool built = false;
      if (same) {
        const double v = css_random_pick(a, b, stp, nargs == 3, R);
        char nb[340];
        const size_t nl = css_cssom_number(v, false, nb);
        built = dbuf_put(&text, nb, nl);
        if (built && percent) {
          built = dbuf_put(&text, "%", 1);
        }
        else if (built && !plain) {
          const char * un = gtext_css_unit_name(unit);
          built = un && dbuf_put(&text, un, strlen(un));
        }
        (void)built;
      }
      else if (nargs == 2) {
        /* calc(MIN + R * (max(MIN, MAX) - MIN)) */
        char nb[340];
        const size_t nl = css_cssom_number(R, false, nb);
        built = dbuf_put(&text, "calc((", 6);
        GTEXT_CSS_Status gs = built ? group_text(sub, t->values + g[first].from, g[first].count, &text) : GTEXT_CSS_E_OOM;
        built = built && gs == GTEXT_CSS_OK && dbuf_put(&text, ") + ", 4) && dbuf_put(&text, nb, nl) && dbuf_put(&text, " * (max((", 9);
        gs = built ? group_text(sub, t->values + g[first].from, g[first].count, &text) : GTEXT_CSS_E_OOM;
        built = built && gs == GTEXT_CSS_OK && dbuf_put(&text, "), (", 4);
        gs = built ? group_text(sub, t->values + g[first + 1].from, g[first + 1].count, &text) : GTEXT_CSS_E_OOM;
        built = built && gs == GTEXT_CSS_OK && dbuf_put(&text, ")) - (", 6);
        gs = built ? group_text(sub, t->values + g[first].from, g[first].count, &text) : GTEXT_CSS_E_OOM;
        built = built && gs == GTEXT_CSS_OK && dbuf_put(&text, ")))", 3);
        if (gs != GTEXT_CSS_OK) {
          text.failed = true; /* the serialization of an argument could not be made: that is memory, and says so below */
        }
      }
      if (text.failed) {
        dbuf_free(&text);
        gtext_css_tree_free(t);
        SUB_FAIL(sub, GTEXT_CSS_E_OOM);
        return STEP_ERROR;
      }
      if (built) {
        st = css_rope_from_text(&sub->alloc, text.data, text.len, taint, &out);
        if (st != GTEXT_CSS_OK) {
          dbuf_free(&text);
          gtext_css_tree_free(t);
          SUB_FAIL(sub, st);
          return STEP_ERROR;
        }
        gi = false;
      }
      dbuf_free(&text);
    }
  }
  gtext_css_tree_free(t);
  fn_finish(sub, fi, out, gi);
  return STEP_DONE;
}

/* ---- attr() ------------------------------------------------------------------------------------------------------- */

static void guard_remove(css_sub * sub, size_t fi) {
  frame_t * f = &sub->frames[fi];
  if (!f->guarded) {
    return;
  }
  for (size_t i = sub->naguards; i-- > 0;) {
    if (sub->aguards[i].frame == fi) {
      sub->aguards[i] = sub->aguards[--sub->naguards];
      break;
    }
  }
  f->guarded = false;
}

static int attr_failure(css_sub * sub, size_t fi) {
  frame_t * f = &sub->frames[fi];
  guard_remove(sub, fi);
  if (f->has_comma) {
    /* "Substitute arbitrary substitution functions in second arg, and return the result." */
    f->phase = 15;
    const int r = eval_arg(sub, fi, 2, 1);
    if (r < 0) {
      return STEP_ERROR;
    }
    if (r == 1) {
      return STEP_PUSHED;
    }
    f = &sub->frames[fi];
    css_rope * out = css_rope_retain(f->res[2].rope);
    fn_finish(sub, fi, out, f->res[2].gi);
    return STEP_DONE;
  }
  css_rope * out = NULL;
  if (f->mode == 0) {
    /* "If second arg is null, and syntax was omitted, return an empty CSS <string>." */
    const GTEXT_CSS_Status st = css_rope_from_text(&sub->alloc, "\"\"", 2, false, &out);
    if (st != GTEXT_CSS_OK) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
  }
  fn_finish(sub, fi, out, out == NULL);
  return STEP_DONE;
}

/// The attr() result as a tainted rope from text.
static int attr_result(css_sub * sub, size_t fi, const char * text, size_t len) {
  css_rope * r = NULL;
  const GTEXT_CSS_Status st = css_rope_from_text(&sub->alloc, text, len, true, &r);
  if (st != GTEXT_CSS_OK) {
    SUB_FAIL(sub, st);
    return STEP_ERROR;
  }
  fn_finish(sub, fi, r, false);
  return STEP_DONE;
}

/// Read `<attr-name> <attr-type>?` from the replaced first argument. 1: read; 0: it is not that; -1: failure.
static int attr_read_args(css_sub * sub, size_t fi, const css_rope * r) {
  GTEXT_CSS_Tree * t = NULL;
  const GTEXT_CSS_Status st = small_parse(sub, r, 65536, &t);
  if (st == GTEXT_CSS_E_OOM) {
    SUB_FAIL(sub, st);
    return -1;
  }
  if (!t) {
    return 0;
  }
  int result = 0;
  const GTEXT_CSS_Component_Value * v = t->values;
  const size_t n = t->value_count;
  size_t i = 0;
  while (i < n && is_ws(&v[i])) {
    i++;
  }
  const GTEXT_CSS_String * ns = NULL;
  const GTEXT_CSS_String * name = NULL;
  bool ok = false;
  bool has_ns = false;
  if (i < n && is_tok(&v[i], GTEXT_CSS_TOKEN_IDENT)) {
    if (i + 2 < n && v[i].span.end == v[i + 1].span.offset && v[i + 1].span.end == v[i + 2].span.offset && v[i + 1].kind == GTEXT_CSS_VALUE_TOKEN &&
        v[i + 1].type == GTEXT_CSS_TOKEN_DELIM && v[i + 1].delim == '|' && is_tok(&v[i + 2], GTEXT_CSS_TOKEN_IDENT)) {
      ns = &v[i].value;
      has_ns = true;
      name = &v[i + 2].value;
      i += 3;
      ok = true;
    }
    else {
      name = &v[i].value;
      i++;
      ok = true;
    }
  }
  else if (i + 1 < n && v[i].kind == GTEXT_CSS_VALUE_TOKEN && v[i].type == GTEXT_CSS_TOKEN_DELIM && v[i].delim == '|' && v[i].span.end == v[i + 1].span.offset &&
      is_tok(&v[i + 1], GTEXT_CSS_TOKEN_IDENT)) {
    name = &v[i + 1].value;
    i += 2;
    ok = true; /* `|name`: no namespace, said explicitly */
  }
  int mode = 0;
  GTEXT_CSS_Unit unit = GTEXT_CSS_UNIT_NONE;
  bool pct = false;
  const css_compiled_syntax * csyn = NULL;
  bool csyn_temp = false;
  if (ok) {
    while (i < n && is_ws(&v[i])) {
      i++;
    }
    if (i < n) {
      if (v[i].kind == GTEXT_CSS_VALUE_FUNCTION && fn_name_is(&v[i].value, "type")) {
        dbuf text;
        dbuf_init(&text, &sub->alloc);
        GTEXT_CSS_Status gs = group_text(sub, v[i].children, v[i].child_count, &text);
        if (gs == GTEXT_CSS_E_OOM || text.failed) {
          dbuf_free(&text);
          gtext_css_tree_free(t);
          SUB_FAIL(sub, GTEXT_CSS_E_OOM);
          return -1;
        }
        gs = text.len ? css_ctx_syntax(sub->cc, text.data, text.len, &csyn, &csyn_temp) : GTEXT_CSS_E_GRAMMAR;
        dbuf_free(&text);
        if (gs == GTEXT_CSS_E_OOM) {
          gtext_css_tree_free(t);
          SUB_FAIL(sub, gs);
          return -1;
        }
        ok = gs == GTEXT_CSS_OK;
        if (ok && !csyn->syntax->universal) {
          /* "For this purpose, <url> is invalid as a <syntax-single-component>." */
          for (size_t c = 0; c < csyn->syntax->ncomps; c++) {
            if (csyn->syntax->comps[c].is_type && csyn->syntax->comps[c].name_len == 3 && memcmp(csyn->syntax->comps[c].name, "url", 3) == 0) {
              ok = false;
            }
          }
        }
        mode = 4;
        i++;
      }
      else if (is_tok(&v[i], GTEXT_CSS_TOKEN_IDENT)) {
        if (tok_ieq(&v[i].value, "raw-string")) {
          mode = 1;
        }
        else if (tok_ieq(&v[i].value, "number")) {
          mode = 2;
        }
        else {
          mode = 3;
          unit = gtext_css_unit_lookup(v[i].value.data, v[i].value.length);
        }
        i++;
      }
      else if (v[i].kind == GTEXT_CSS_VALUE_TOKEN && v[i].type == GTEXT_CSS_TOKEN_DELIM && v[i].delim == '%') {
        mode = 3;
        pct = true;
        i++;
      }
      else {
        ok = false;
      }
      while (ok && i < n) {
        ok = is_ws(&v[i]);
        i++;
      }
    }
  }
  if (ok) {
    GTEXT_Allocator a = sub->alloc;
    frame_t * f = &sub->frames[fi];
    char * copy = gtext_allocator_malloc(&a, name->length + 1);
    char * nscopy = has_ns ? gtext_allocator_malloc(&a, ns->length + 1) : NULL;
    if (!copy || (has_ns && !nscopy)) {
      gtext_allocator_free(&a, copy);
      gtext_allocator_free(&a, nscopy);
      if (csyn_temp) {
        css_compiled_syntax_free((css_compiled_syntax *)csyn);
      }
      gtext_css_tree_free(t);
      SUB_FAIL(sub, GTEXT_CSS_E_OOM);
      return -1;
    }
    memcpy(copy, name->data, name->length);
    copy[name->length] = '\0';
    gtext_allocator_free(&a, f->u.n.name);
    f->u.n.name = copy;
    f->u.n.name_len = name->length;
    if (has_ns) {
      memcpy(nscopy, ns->data, ns->length);
      nscopy[ns->length] = '\0';
    }
    gtext_allocator_free(&a, f->aux);
    f->aux = nscopy;
    f->aux_n = has_ns ? ns->length : 0;
    f->has_ns = has_ns;
    f->mode = mode;
    f->unit = unit;
    f->pct = pct;
    if (f->csyn_temp) {
      css_compiled_syntax_free((css_compiled_syntax *)f->csyn);
    }
    f->csyn = csyn;
    f->csyn_temp = csyn_temp;
    result = 1;
  }
  else if (csyn_temp) {
    css_compiled_syntax_free((css_compiled_syntax *)csyn);
  }
  gtext_css_tree_free(t);
  return result;
}

static int step_attr(css_sub * sub, size_t fi) {
  int pre = fn_prelude(sub, fi);
  if (pre) {
    return pre;
  }
  frame_t * f = &sub->frames[fi];
  if (f->has_arrived) {
    take_arrived(f);
  }
  GTEXT_CSS_Status st;
  switch (f->phase) {
  case 10: {
    st = split_args(sub, f, GTEXT_CSS_TOKEN_COMMA, true);
    if (st != GTEXT_CSS_OK) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    f->phase = 11;
    const int r = eval_arg(sub, fi, 0, 0);
    if (r < 0) {
      return STEP_ERROR;
    }
    if (r == 1) {
      return STEP_PUSHED;
    }
    f = &sub->frames[fi];
  }
  /* fall through */
  case 11: {
    /* "Substitute arbitrary substitution functions in first arg, then parse it as <attr-name> <attr-type>?. If that returns failure, jump to FAILURE." */
    if (f->res[0].gi || !f->res[0].rope) {
      return attr_failure(sub, fi);
    }
    const int ok = attr_read_args(sub, fi, f->res[0].rope);
    if (ok < 0) {
      return STEP_ERROR;
    }
    f = &sub->frames[fi];
    if (ok == 0) {
      return attr_failure(sub, fi);
    }
    /* "If attr name exists as an attribute on el, let attr value be its value; otherwise jump to FAILURE." */
    const GTEXT_CSS_Element_Interface * iface = sub->st->iface;
    if (!iface || !gtext_css_element_has(iface, GTEXT_CSS_ELEMENT_CB_ATTRIBUTE) || !sub->st->element) {
      st = css_st_fail(sub->st, GTEXT_CSS_E_UNSUPPORTED, !iface || !gtext_css_element_has(iface, GTEXT_CSS_ELEMENT_CB_ATTRIBUTE) ? "attribute" : "element",
          "attr() reads an attribute of the element, which the element table has no callback for (or no element was given)");
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    GTEXT_CSS_String ns = {f->aux, f->aux_n};
    GTEXT_CSS_String nm = {f->u.n.name, f->u.n.name_len};
    GTEXT_CSS_String value = {NULL, 0};
    bool found = false;
    st = gtext_css_element_get_attribute(iface, sub->st->element, f->has_ns ? &ns : NULL, &nm, &value, &found, NULL);
    if (st != GTEXT_CSS_OK) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    if (!found || !value.data) {
      return attr_failure(sub, fi);
    }
    f = &sub->frames[fi];
    if (f->mode == 0 || f->mode == 1) {
      /* "return a CSS <string> whose value is attr value. Note: No parsing or modification of any kind is performed on the value." */
      dbuf b;
      dbuf_init(&b, &sub->alloc);
      GTEXT_Markup_Sink sink = dbuf_sink(&b);
      st = gtext_css_serialize_string(value.data, value.length, &sink);
      if (st != GTEXT_CSS_OK || b.failed) {
        dbuf_free(&b);
        SUB_FAIL(sub, st == GTEXT_CSS_OK ? GTEXT_CSS_E_OOM : st);
        return STEP_ERROR;
      }
      const int r = attr_result(sub, fi, b.data, b.len);
      dbuf_free(&b);
      return r;
    }
    if (f->mode == 2 || f->mode == 3) {
      /* "If syntax is the keyword number or an <attr-unit> value, parse attr value against <attr-type>" */
      size_t b = 0, e = value.length;
      while (b < e && (value.data[b] == ' ' || value.data[b] == '\t' || value.data[b] == '\n' || value.data[b] == '\r' || value.data[b] == '\f')) {
        b++;
      }
      while (e > b && (value.data[e - 1] == ' ' || value.data[e - 1] == '\t' || value.data[e - 1] == '\n' || value.data[e - 1] == '\r' || value.data[e - 1] == '\f')) {
        e--;
      }
      GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
      po.allocator = &sub->alloc;
      po.count_errors_only = true;
      GTEXT_CSS_Tree * t = NULL;
      st = gtext_css_parse_component_values(value.data + b, e - b, &po, &t, NULL);
      if (st == GTEXT_CSS_E_OOM) {
        SUB_FAIL(sub, st);
        return STEP_ERROR;
      }
      const GTEXT_CSS_Component_Value * one = st == GTEXT_CSS_OK ? single(t) : NULL;
      int r;
      if (one && one->kind == GTEXT_CSS_VALUE_TOKEN && one->type == GTEXT_CSS_TOKEN_NUMBER && one->repr.data) {
        if (f->mode == 2) {
          r = attr_result(sub, fi, one->repr.data, one->repr.length);
        }
        else if (f->pct || f->unit != GTEXT_CSS_UNIT_NONE) {
          dbuf text;
          dbuf_init(&text, &sub->alloc);
          const char * un = f->pct ? "%" : gtext_css_unit_name(f->unit);
          if (!dbuf_put(&text, one->repr.data, one->repr.length) || !dbuf_put(&text, un, strlen(un))) {
            dbuf_free(&text);
            gtext_css_tree_free(t);
            SUB_FAIL(sub, GTEXT_CSS_E_OOM);
            return STEP_ERROR;
          }
          r = attr_result(sub, fi, text.data, text.len);
          dbuf_free(&text);
        }
        else {
          r = attr_failure(sub, fi); /* "If the <attr-unit> does not match a known CSS unit, it triggers fallback." */
        }
      }
      else {
        r = attr_failure(sub, fi);
      }
      gtext_css_tree_free(t);
      return r;
    }
    /* type(<syntax>): the attribute value is parsed as CSS, replaced under the context «"attribute", attr name», and checked against the syntax */
    for (size_t g = 0; g < sub->naguards; g++) {
      if (sub->aguards[g].name.length == f->u.n.name_len && memcmp(sub->aguards[g].name.data, f->u.n.name, f->u.n.name_len) == 0) {
        mark_cyclic(sub, sub->aguards[g].frame);
        return attr_failure(sub, fi);
      }
    }
    if (sub->naguards == sub->capaguards) {
      const size_t cap = sub->capaguards ? sub->capaguards * 2 : 8;
      GTEXT_Allocator a = sub->alloc;
      void * g = gtext_allocator_realloc(&a, sub->aguards, cap * sizeof *sub->aguards);
      if (!g) {
        SUB_FAIL(sub, GTEXT_CSS_E_OOM);
        return STEP_ERROR;
      }
      sub->aguards = g;
      sub->capaguards = cap;
    }
    if (value.length > sub->max_bytes) {
      return attr_failure(sub, fi);
    }
    GTEXT_Allocator a = sub->alloc;
    char * copy = gtext_allocator_malloc(&a, value.length + 1);
    if (!copy) {
      SUB_FAIL(sub, GTEXT_CSS_E_OOM);
      return STEP_ERROR;
    }
    memcpy(copy, value.data, value.length);
    copy[value.length] = '\0';
    src_t * ns2 = NULL;
    st = src_open(&sub->alloc, copy, value.length, copy, true, &ns2);
    if (st != GTEXT_CSS_OK) {
      if (st == GTEXT_CSS_E_OOM) {
        SUB_FAIL(sub, st);
        return STEP_ERROR;
      }
      return attr_failure(sub, fi); /* a limit of the parser: the attribute does not parse */
    }
    f = &sub->frames[fi];
    src_close(&sub->alloc, f->own_src);
    f->own_src = ns2;
    sub->aguards[sub->naguards].name.data = f->u.n.name;
    sub->aguards[sub->naguards].name.length = f->u.n.name_len;
    sub->aguards[sub->naguards].frame = fi;
    sub->naguards++;
    f->guarded = true;
    f->phase = 12;
    const int r = seq_start(sub, fi, 1, ns2, ns2->tree->values, ns2->tree->value_count, false);
    if (r < 0) {
      return STEP_ERROR;
    }
    if (r == 1) {
      return STEP_PUSHED;
    }
    f = &sub->frames[fi];
  }
  /* fall through */
  case 12: {
    guard_remove(sub, fi);
    f = &sub->frames[fi];
    if (f->cyclic || f->res[1].gi || !f->res[1].rope) {
      return attr_failure(sub, fi);
    }
    char * text = NULL;
    size_t len = 0;
    st = css_rope_flatten(&sub->alloc, f->res[1].rope, sub->max_bytes, &text, &len, NULL, NULL, &sub->work);
    if (st == GTEXT_CSS_E_LIMIT_OUTPUT) {
      sub->limited++;
      return attr_failure(sub, fi);
    }
    if (st != GTEXT_CSS_OK) {
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    size_t b = 0, e = len;
    while (b < e && (text[b] == ' ' || text[b] == '\t' || text[b] == '\n' || text[b] == '\r' || text[b] == '\f')) {
      b++;
    }
    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\n' || text[e - 1] == '\r' || text[e - 1] == '\f')) {
      e--;
    }
    bool matched = f->csyn->syntax->universal;
    if (!matched) {
      GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
      po.allocator = &sub->alloc;
      po.count_errors_only = true;
      GTEXT_CSS_Tree * t = NULL;
      st = gtext_css_parse_component_values(text + b, e - b, &po, &t, NULL);
      if (st == GTEXT_CSS_OK) {
        GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
        mo.allocator = &sub->alloc;
        css_syn_match m;
        st = css_syntax_match(f->csyn->syntax, f->csyn->compiled, t->values, t->value_count, &mo, &matched, &m);
        gtext_css_value_set_free(m.set);
        gtext_css_tree_free(t);
      }
    }
    /* the universal syntax takes any token stream that parses, and the replaced text is one */
    if (st == GTEXT_CSS_E_OOM) {
      gtext_allocator_free(&sub->alloc, text);
      SUB_FAIL(sub, st);
      return STEP_ERROR;
    }
    if (st != GTEXT_CSS_OK || !matched) {
      gtext_allocator_free(&sub->alloc, text);
      return attr_failure(sub, fi);
    }
    const int r = attr_result(sub, fi, text + b, e - b);
    gtext_allocator_free(&sub->alloc, text);
    return r;
  }
  case 15:
  default: {
    css_rope * out = css_rope_retain(f->res[2].rope);
    const bool gi = f->res[2].gi;
    fn_finish(sub, fi, out, gi);
    return STEP_DONE;
  }
  }
}

/* ---- if() ---------------------------------------------------------------------------------------------------------- */

typedef enum { T_FALSE = 0, T_TRUE = 1, T_UNKNOWN = 2 } tri;

typedef enum { N_CONST = 0, N_LEAF, N_NOT, N_AND, N_OR } ntype;

typedef struct {
  ntype type;
  tri value;        ///< N_CONST: its value; the others once evaluated.
  int a, b;         ///< Children (indexes into the nodes), or for a leaf its place in the leaves.
} cnode;

typedef enum { L_MEDIA = 0, L_SUPPORTS, L_STYLE } ltype;

typedef struct {
  ltype type;
  const GTEXT_CSS_Component_Value * name;  ///< L_STYLE: the property named.
  const GTEXT_CSS_Component_Value * vals;  ///< L_MEDIA, L_SUPPORTS: the contents; L_STYLE: the value after the colon.
  size_t count;
  bool has_value;                          ///< L_STYLE: a colon and a value were written.
  bool ranged;                             ///< L_STYLE: a comparison operator is written.
  tri result;                              ///< Once evaluated.
} cleaf;

typedef struct {
  GTEXT_Allocator alloc;
  GTEXT_CSS_Tree * tree;
  cnode * nodes;
  size_t nnodes, capnodes;
  cleaf * leaves;
  size_t nleaves, capleaves;
  size_t next_leaf;
  int root;
} cond_state;

static void cond_free(const GTEXT_Allocator * alloc, void * p) {
  cond_state * c = p;
  if (!c) {
    return;
  }
  GTEXT_Allocator a = *alloc;
  gtext_css_tree_free(c->tree);
  gtext_allocator_free(&a, c->nodes);
  gtext_allocator_free(&a, c->leaves);
  gtext_allocator_free(&a, c);
}

static int cond_node(cond_state * c, ntype type, tri value, int a, int b) {
  if (c->nnodes == c->capnodes) {
    const size_t cap = c->capnodes ? c->capnodes * 2 : 16;
    cnode * g = gtext_allocator_realloc(&c->alloc, c->nodes, cap * sizeof *g);
    if (!g) {
      return -2;
    }
    c->nodes = g;
    c->capnodes = cap;
  }
  c->nodes[c->nnodes] = (cnode){type, value, a, b};
  return (int)c->nnodes++;
}

static int cond_leaf(cond_state * c, const cleaf * leaf) {
  if (c->nleaves == c->capleaves) {
    const size_t cap = c->capleaves ? c->capleaves * 2 : 8;
    cleaf * g = gtext_allocator_realloc(&c->alloc, c->leaves, cap * sizeof *g);
    if (!g) {
      return -2;
    }
    c->leaves = g;
    c->capleaves = cap;
  }
  c->leaves[c->nleaves] = *leaf;
  const int l = (int)c->nleaves++;
  return cond_node(c, N_LEAF, T_UNKNOWN, l, 0);
}

typedef struct {
  const GTEXT_CSS_Component_Value * v;
  size_t n, i;
  int op;               ///< 0 none yet, 1 `and`, 2 `or`.
  int cur;              ///< The node of what has been read, or -1.
  bool expect_term;
  bool negated;         ///< `not` was read, so one term is wanted and nothing may follow it.
  bool style_mode;      ///< The terms are style features.
  bool bare_feature;    ///< The whole list may be one feature (the contents of `style()`).
  bool pending_child;   ///< A group was started: its node arrives in `child_node`.
} pframe;

/// Whether a list of values (in a style query) is a feature: it begins with a name that is not `not`.
static bool looks_like_feature(const GTEXT_CSS_Component_Value * v, size_t n) {
  size_t i = 0;
  while (i < n && is_ws(&v[i])) {
    i++;
  }
  return i < n && is_tok(&v[i], GTEXT_CSS_TOKEN_IDENT) && !tok_ieq(&v[i].value, "not");
}

/// A leaf for a style feature (`--name`, `--name: value`, `--name > value`).
static int make_feature(cond_state * c, const GTEXT_CSS_Component_Value * v, size_t n) {
  cleaf leaf;
  memset(&leaf, 0, sizeof leaf);
  leaf.type = L_STYLE;
  size_t i = 0;
  while (i < n && is_ws(&v[i])) {
    i++;
  }
  leaf.name = &v[i];
  i++;
  while (i < n && is_ws(&v[i])) {
    i++;
  }
  if (i < n && is_tok(&v[i], GTEXT_CSS_TOKEN_COLON)) {
    leaf.has_value = true;
    i++;
    leaf.vals = &v[i];
    leaf.count = n - i;
  }
  else if (i < n) {
    leaf.ranged = true; /* `--x > 3`, `--x = 1`: a style range, which this layer does not evaluate */
  }
  return cond_leaf(c, &leaf);
}

/// Read a boolean expression: `not <term>`, `<term> [and <term>]*`, `<term> [or <term>]*`, with `( <expr> )`, `supports()`, `media()`, `style()` and
/// <general-enclosed> (unknown) as terms. `*ok` says whether the text is one; `*root` is the node of the whole. (An explicit stack: no recursion.)
static GTEXT_CSS_Status cond_parse(cond_state * c, const GTEXT_CSS_Component_Value * v, size_t n, bool * ok, int * root) {
  *ok = false;
  GTEXT_Allocator a = c->alloc;
  size_t cap = 8, sp = 0;
  pframe * st = gtext_allocator_malloc(&a, cap * sizeof *st);
  if (!st) {
    return GTEXT_CSS_E_OOM;
  }
  st[sp++] = (pframe){v, n, 0, 0, -1, true, false, false, false, false};
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  int child_node = -1;
  while (sp && status == GTEXT_CSS_OK) {
    pframe * f = &st[sp - 1];
    int term = -1;
    bool failed = false;
    if (f->pending_child) {
      f->pending_child = false;
      term = child_node;
    }
    else {
      while (f->i < f->n && is_ws(&f->v[f->i])) {
        f->i++;
      }
      if (f->expect_term) {
        if (f->i >= f->n) {
          failed = true;
        }
        else {
          const GTEXT_CSS_Component_Value * cv = &f->v[f->i];
          if (cv->kind == GTEXT_CSS_VALUE_TOKEN && cv->type == GTEXT_CSS_TOKEN_IDENT && tok_ieq(&cv->value, "not") && f->cur == -1 && !f->negated) {
            f->negated = true;
            f->i++;
            continue;
          }
          if (f->bare_feature && f->cur == -1 && !f->negated && looks_like_feature(f->v, f->n)) {
            term = make_feature(c, f->v, f->n);
            f->i = f->n;
          }
          else if (cv->kind == GTEXT_CSS_VALUE_BLOCK && cv->type == GTEXT_CSS_TOKEN_OPEN_PAREN && f->style_mode && looks_like_feature(cv->children, cv->child_count)) {
            term = make_feature(c, cv->children, cv->child_count);
            f->i++;
          }
          else if ((cv->kind == GTEXT_CSS_VALUE_BLOCK && cv->type == GTEXT_CSS_TOKEN_OPEN_PAREN) ||
              (cv->kind == GTEXT_CSS_VALUE_FUNCTION && !f->style_mode && tok_ieq(&cv->value, "style"))) {
            /* a group: its contents are read first, in a frame of their own */
            if (sp == cap) {
              pframe * g = gtext_allocator_realloc(&a, st, cap * 2 * sizeof *g);
              if (!g) {
                status = GTEXT_CSS_E_OOM;
                break;
              }
              st = g;
              cap *= 2;
              f = &st[sp - 1];
            }
            const bool is_style = cv->kind == GTEXT_CSS_VALUE_FUNCTION;
            f->i++;
            f->pending_child = true;
            st[sp++] = (pframe){cv->children, cv->child_count, 0, 0, -1, true, false, f->style_mode || is_style, is_style, false};
            continue;
          }
          else if (cv->kind == GTEXT_CSS_VALUE_FUNCTION) {
            if (tok_ieq(&cv->value, "supports") || tok_ieq(&cv->value, "media")) {
              cleaf leaf;
              memset(&leaf, 0, sizeof leaf);
              leaf.type = tok_ieq(&cv->value, "media") ? L_MEDIA : L_SUPPORTS;
              leaf.vals = cv->children;
              leaf.count = cv->child_count;
              term = cond_leaf(c, &leaf);
            }
            else {
              term = cond_node(c, N_CONST, T_UNKNOWN, 0, 0); /* <general-enclosed> */
            }
            f->i++;
          }
          else {
            failed = true;
          }
        }
      }
      else {
        /* after a term: the end, or an operator and another term */
        if (f->i >= f->n) {
          const int node = f->cur;
          sp--;
          if (sp == 0) {
            *root = node;
            *ok = true;
          }
          else {
            child_node = node;
          }
          continue;
        }
        const GTEXT_CSS_Component_Value * cv = &f->v[f->i];
        int op = 0;
        if (cv->kind == GTEXT_CSS_VALUE_TOKEN && cv->type == GTEXT_CSS_TOKEN_IDENT) {
          op = tok_ieq(&cv->value, "and") ? 1 : tok_ieq(&cv->value, "or") ? 2 : 0;
        }
        if (!op || f->negated || (f->op && f->op != op)) {
          failed = true;
        }
        else {
          f->op = op;
          f->i++;
          f->expect_term = true;
          continue;
        }
      }
    }
    if (failed) {
      if (sp == 1) {
        break; /* not a boolean expression */
      }
      /* a group that is no boolean expression is <general-enclosed>: unknown */
      sp--;
      child_node = cond_node(c, N_CONST, T_UNKNOWN, 0, 0);
      if (child_node < 0) {
        status = GTEXT_CSS_E_OOM;
      }
      continue;
    }
    if (term < 0) {
      status = GTEXT_CSS_E_OOM; /* a node could not be made */
      break;
    }
    if (f->negated) {
      term = cond_node(c, N_NOT, T_UNKNOWN, term, 0);
      if (term < 0) {
        status = GTEXT_CSS_E_OOM;
        break;
      }
    }
    if (f->cur == -1) {
      f->cur = term;
    }
    else {
      f->cur = cond_node(c, f->op == 1 ? N_AND : N_OR, T_UNKNOWN, f->cur, term);
      if (f->cur < 0) {
        status = GTEXT_CSS_E_OOM;
        break;
      }
    }
    f->expect_term = false;
  }
  gtext_allocator_free(&a, st);
  return status;
}

/* ---- registered custom properties: parsing, computing --------------------------------------------------------------- */

enum { NEED_FONT = 1, NEED_METRICS = 2, NEED_LINE_HEIGHT = 4 };

/// What the units written in some values need of the element's computed style.
static GTEXT_CSS_Status units_needed(css_sub * sub, const GTEXT_CSS_Component_Value * values, size_t count, unsigned * need) {
  *need = 0;
  typedef struct {
    const GTEXT_CSS_Component_Value * v;
    size_t n, i;
  } fr;
  GTEXT_Allocator a = sub->alloc;
  size_t cap = 16, len = 0;
  fr * st = gtext_allocator_malloc(&a, cap * sizeof *st);
  if (!st) {
    return GTEXT_CSS_E_OOM;
  }
  if (count) {
    st[len++] = (fr){values, count, 0};
  }
  while (len) {
    fr * f = &st[len - 1];
    if (f->i >= f->n) {
      len--;
      continue;
    }
    const GTEXT_CSS_Component_Value * v = &f->v[f->i++];
    sub->work++;
    if (v->kind == GTEXT_CSS_VALUE_TOKEN) {
      if (v->type == GTEXT_CSS_TOKEN_DIMENSION) {
        const GTEXT_CSS_Unit u = gtext_css_unit_lookup(v->unit.data, v->unit.length);
        switch (u) {
        case GTEXT_CSS_UNIT_EM: case GTEXT_CSS_UNIT_REM: *need |= NEED_FONT; break;
        case GTEXT_CSS_UNIT_EX: case GTEXT_CSS_UNIT_CAP: case GTEXT_CSS_UNIT_CH: case GTEXT_CSS_UNIT_IC: case GTEXT_CSS_UNIT_REX:
        case GTEXT_CSS_UNIT_RCAP: case GTEXT_CSS_UNIT_RCH: case GTEXT_CSS_UNIT_RIC:
          *need |= NEED_FONT | NEED_METRICS;
          break;
        case GTEXT_CSS_UNIT_LH: case GTEXT_CSS_UNIT_RLH: *need |= NEED_FONT | NEED_METRICS | NEED_LINE_HEIGHT; break;
        default: break;
        }
      }
      continue;
    }
    if (v->child_count) {
      if (len == cap) {
        fr * g = gtext_allocator_realloc(&a, st, cap * 2 * sizeof *g);
        if (!g) {
          gtext_allocator_free(&a, st);
          return GTEXT_CSS_E_OOM;
        }
        st = g;
        cap *= 2;
      }
      st[len++] = (fr){v->children, v->child_count, 0};
    }
  }
  gtext_allocator_free(&a, st);
  return GTEXT_CSS_OK;
}

/// Make the computed properties a unit needs exist now. `*cycle` is set when one is being computed already (the registered value depends on it and it on the value).
static GTEXT_CSS_Status ensure_fonts(css_sub * sub, unsigned need, bool * cycle) {
  *cycle = false;
  if (!need || !sub->st->style) {
    return GTEXT_CSS_OK;
  }
  css_state * st = sub->st;
  const GTEXT_CSS_Compute_Context * cc = sub->cc;
  static const int chain[] = {CSS_P_WRITING_MODE, CSS_P_DIRECTION, CSS_P_TEXT_ORIENTATION, CSS_P_FONT_FAMILY, CSS_P_FONT_STYLE, CSS_P_FONT_WEIGHT,
                              CSS_P_FONT_WIDTH, CSS_P_FONT_SIZE, CSS_P_FONT_SIZE_ADJUST, CSS_P_LINE_HEIGHT};
  size_t upto = 0;
  if (need & NEED_LINE_HEIGHT) {
    upto = sizeof chain / sizeof *chain;
  }
  else if (need & NEED_METRICS) {
    upto = 8;
  }
  else {
    /* only the font size: it reads nothing of the element's own (tools/css/compute_order.tsv) */
    return css_st_ensure(st, cc->pid[CSS_P_FONT_SIZE], cycle);
  }
  for (size_t i = 0; i < upto; i++) {
    if (!cc->pid[chain[i]]) {
      continue;
    }
    const GTEXT_CSS_Status status = css_st_ensure(st, cc->pid[chain[i]], cycle);
    if (status != GTEXT_CSS_OK || *cycle) {
      return status;
    }
  }
  return GTEXT_CSS_OK;
}

/// A text read against the syntax of a registered custom property and computed as its type says. `*out` is the rope of the computed value as tokens, or NULL when
/// the text is not a value of the syntax (or depends on itself: `*cycle`).
static GTEXT_CSS_Status compute_typed(css_sub * sub, const css_creg * creg, const char * text, size_t len, bool tainted, css_rope ** out, bool * cycle) {
  *out = NULL;
  *cycle = false;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = &sub->alloc;
  po.count_errors_only = true;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text, len, &po, &tree, NULL);
  if (st != GTEXT_CSS_OK) {
    return st == GTEXT_CSS_E_OOM ? st : GTEXT_CSS_OK;  /* a limit of the parser: not a value of the syntax */
  }
  GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
  mo.allocator = &sub->alloc;
  bool matched = false;
  css_syn_match m;
  st = css_syntax_match(creg->syntax, creg->compiled, tree->values, tree->value_count, &mo, &matched, &m);
  if (st != GTEXT_CSS_OK) {
    gtext_css_tree_free(tree);
    return (st == GTEXT_CSS_E_OOM || st == GTEXT_CSS_E_INTERNAL) ? st : GTEXT_CSS_OK; /* the answer is not known: not a value */
  }
  if (matched) {
    unsigned need = 0;
    st = units_needed(sub, tree->values, tree->value_count, &need);
    if (st == GTEXT_CSS_OK) {
      st = ensure_fonts(sub, need, cycle);
    }
    if (st == GTEXT_CSS_OK && !*cycle && sub->cc->pid[CSS_P_COLOR_SCHEME]) {
      /* a color is computed in the element's color scheme (light-dark(), the system colors) */
      for (size_t i = 0; i < creg->syntax->ncomps; i++) {
        const css_syn_comp * c = &creg->syntax->comps[i];
        if (c->is_type && ((c->name_len == 5 && memcmp(c->name, "color", 5) == 0) || (c->name_len == 5 && memcmp(c->name, "image", 5) == 0))) {
          st = css_st_ensure(sub->st, sub->cc->pid[CSS_P_COLOR_SCHEME], cycle);
          break;
        }
      }
    }
    GTEXT_CSS_Value_Set * computed = NULL;
    if (st == GTEXT_CSS_OK && !*cycle && m.set) {
      st = css_cv_by_type(sub->st, m.set, 0, &computed);
    }
    if (st == GTEXT_CSS_OK && !*cycle) {
      const GTEXT_CSS_Value * root = computed ? gtext_css_value_set_root(computed) : NULL;
      if (root) {
        GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
        so.allocator = &sub->alloc;
        char * buf = NULL;
        size_t bl = 0;
        st = gtext_css_serialize_value_buffer(root, &so, &buf, &bl);
        if (st == GTEXT_CSS_OK) {
          st = css_rope_from_text(&sub->alloc, buf, bl, tainted, out);
          gtext_css_serialize_buffer_free(&so, buf);
        }
      }
      else {
        /* an identifier of the syntax: as specified */
        size_t b = 0, e = len;
        while (b < e && (text[b] == ' ' || text[b] == '\t' || text[b] == '\n' || text[b] == '\r' || text[b] == '\f')) {
          b++;
        }
        while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\n' || text[e - 1] == '\r' || text[e - 1] == '\f')) {
          e--;
        }
        st = css_rope_from_text(&sub->alloc, text + b, e - b, tainted, out);
      }
    }
    gtext_css_value_set_free(computed);
    gtext_css_value_set_free(m.set);
  }
  gtext_css_tree_free(tree);
  return st;
}

/* ---- style() ----------------------------------------------------------------------------------------------------- */

/// Whether two token streams are the same tokens (white space between tokens being one token however long, comments not tokens).
static GTEXT_CSS_Status same_tokens(css_sub * sub, const char * a, size_t al, const char * b, size_t bl, bool * same) {
  *same = false;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = &sub->alloc;
  po.count_errors_only = true;
  GTEXT_CSS_Tree * ta = NULL;
  GTEXT_CSS_Tree * tb = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_component_values(a, al, &po, &ta, NULL);
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_parse_component_values(b, bl, &po, &tb, NULL);
  }
  if (st == GTEXT_CSS_OK) {
    size_t an = ta->value_count, bn = tb->value_count;
    const GTEXT_CSS_Component_Value * av = ta->values;
    const GTEXT_CSS_Component_Value * bv = tb->values;
    while (an && is_ws(&av[0])) { av++; an--; }
    while (an && is_ws(&av[an - 1])) { an--; }
    while (bn && is_ws(&bv[0])) { bv++; bn--; }
    while (bn && is_ws(&bv[bn - 1])) { bn--; }
    GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
    so.allocator = &sub->alloc;
    char *sa = NULL, *sb = NULL;
    size_t la = 0, lb = 0;
    st = gtext_css_serialize_values_buffer(av, an, &so, &sa, &la);
    if (st == GTEXT_CSS_OK) {
      st = gtext_css_serialize_values_buffer(bv, bn, &so, &sb, &lb);
    }
    if (st == GTEXT_CSS_OK) {
      *same = la == lb && memcmp(sa, sb, la) == 0;
    }
    gtext_css_serialize_buffer_free(&so, sa);
    gtext_css_serialize_buffer_free(&so, sb);
  }
  gtext_css_tree_free(ta);
  gtext_css_tree_free(tb);
  return st == GTEXT_CSS_E_OOM ? st : (st == GTEXT_CSS_OK ? st : GTEXT_CSS_OK);
}

/// A style feature of a custom property: `style(--x: value)` is true when the computed value of `--x` is `value`, `style(--x)` when it is not the initial value
/// (Conditional 5, 6.2). Returns STEP_PUSHED while the property is being computed, STEP_DONE when `leaf->result` is set, STEP_ERROR on failure.
static int style_leaf(css_sub * sub, size_t fi, cleaf * leaf) {
  const GTEXT_CSS_String * nm = &leaf->name->value;
  if (leaf->ranged || !css_creg_name_ok(nm->data, nm->length)) {
    leaf->result = T_UNKNOWN;
    return STEP_DONE;
  }
  bool inprog = false;
  const int r = lookup_custom(sub, fi, 2, nm->data, nm->length, true, &inprog);
  if (r < 0) {
    return STEP_ERROR;
  }
  if (r == 1) {
    return STEP_PUSHED;
  }
  frame_t * f = &sub->frames[fi];
  if (inprog) {
    leaf->result = T_FALSE; /* "that query evaluates to false" */
    return STEP_DONE;
  }
  size_t ri = 0;
  const bool reged = sub->cc->nregs && css_ctx_find_reg(sub->cc, nm->data, nm->length, &ri);
  const css_rope * value = f->res[2].gi ? NULL : f->res[2].rope;
  const GTEXT_CSS_Custom_Value * initial_cv = reged ? sub->cc->regs[ri].initial : NULL;
  const css_rope * initial = initial_cv ? css_cval_rope(initial_cv) : NULL;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  char *vt = NULL, *it = NULL;
  size_t vl = 0, il = 0;
  if (value) {
    st = css_rope_flatten(&sub->alloc, value, sub->max_bytes, &vt, &vl, NULL, NULL, &sub->work);
  }
  if (st == GTEXT_CSS_OK && initial) {
    st = css_rope_flatten(&sub->alloc, initial, sub->max_bytes, &it, &il, NULL, NULL, &sub->work);
  }
  if (st == GTEXT_CSS_E_LIMIT_OUTPUT) {
    st = GTEXT_CSS_OK;
    leaf->result = T_FALSE;
    gtext_allocator_free(&sub->alloc, vt);
    gtext_allocator_free(&sub->alloc, it);
    return STEP_DONE;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&sub->alloc, vt);
    gtext_allocator_free(&sub->alloc, it);
    SUB_FAIL(sub, st);
    return STEP_ERROR;
  }
  bool result = false;
  if (!leaf->has_value) {
    /* "A style feature without a value evaluates to true if the computed value is different from the initial value" */
    if (!value || !initial) {
      result = (value != NULL) != (initial != NULL);
    }
    else {
      bool same = false;
      st = same_tokens(sub, vt, vl, it, il, &same);
      result = !same;
    }
  }
  else if (value) {
    dbuf want;
    dbuf_init(&want, &sub->alloc);
    st = group_text(sub, leaf->vals, leaf->count, &want);
    if (st == GTEXT_CSS_OK && !want.failed) {
      const char * wt = want.data ? want.data : "";
      size_t wl = want.len;
      /* the CSS-wide keywords, "computed with respect to the query container" */
      const GTEXT_CSS_Wide_Keyword wide = gtext_css_wide_keyword_lookup(wt, wl);
      if (wide == GTEXT_CSS_WIDE_REVERT || wide == GTEXT_CSS_WIDE_REVERT_LAYER || wide == GTEXT_CSS_WIDE_REVERT_RULE) {
        result = false; /* "Cascade-dependent keywords, such as revert and revert-layer, are invalid as values in a style feature" */
      }
      else if (wide != GTEXT_CSS_WIDE_NONE) {
        const GTEXT_CSS_Custom_Value * other = NULL;
        bool inherits = !reged || sub->cc->regs[ri].creg->inherits;
        if (wide == GTEXT_CSS_WIDE_INITIAL || (wide == GTEXT_CSS_WIDE_UNSET && !inherits)) {
          other = initial_cv;
        }
        else {
          other = parent_value(sub, nm->data, nm->length, reged ? (long)ri : -1);
        }
        if (!other) {
          result = false;
        }
        else {
          char * ot = NULL;
          size_t ol = 0;
          st = css_rope_flatten(&sub->alloc, css_cval_rope(other), sub->max_bytes, &ot, &ol, NULL, NULL, &sub->work);
          if (st == GTEXT_CSS_OK) {
            bool same = false;
            st = same_tokens(sub, vt, vl, ot, ol, &same);
            result = same;
          }
          gtext_allocator_free(&sub->alloc, ot);
        }
      }
      else if (reged && !sub->cc->regs[ri].creg->syntax->universal) {
        /* the value is read against the syntax and computed as the property's own values are */
        css_rope * cr = NULL;
        bool cycle = false;
        st = compute_typed(sub, sub->cc->regs[ri].creg, wt, wl, false, &cr, &cycle);
        if (st == GTEXT_CSS_OK && cr && !cycle) {
          char * ct = NULL;
          size_t cl = 0;
          st = css_rope_flatten(&sub->alloc, cr, sub->max_bytes, &ct, &cl, NULL, NULL, &sub->work);
          if (st == GTEXT_CSS_OK) {
            bool same = false;
            st = same_tokens(sub, vt, vl, ct, cl, &same);
            result = same;
          }
          gtext_allocator_free(&sub->alloc, ct);
        }
        css_rope_release(cr);
      }
      else {
        bool same = false;
        st = same_tokens(sub, vt, vl, wt, wl, &same);
        result = same;
      }
    }
    dbuf_free(&want);
  }
  gtext_allocator_free(&sub->alloc, vt);
  gtext_allocator_free(&sub->alloc, it);
  if (st != GTEXT_CSS_OK) {
    SUB_FAIL(sub, st);
    return STEP_ERROR;
  }
  leaf->result = result ? T_TRUE : T_FALSE;
  return STEP_DONE;
}

static tri tri_not(tri v) {
  return v == T_TRUE ? T_FALSE : v == T_FALSE ? T_TRUE : T_UNKNOWN;
}

/// Three-valued logic (CSS Values 5, Appendix B): `and` is false if either is false, true if both are, unknown otherwise; `or` the dual.
static tri tri_and(tri a, tri b) {
  return (a == T_FALSE || b == T_FALSE) ? T_FALSE : (a == T_UNKNOWN || b == T_UNKNOWN) ? T_UNKNOWN : T_TRUE;
}

static tri tri_or(tri a, tri b) {
  return (a == T_TRUE || b == T_TRUE) ? T_TRUE : (a == T_UNKNOWN || b == T_UNKNOWN) ? T_UNKNOWN : T_FALSE;
}

static tri cond_fold(cond_state * c, int root) {
  for (size_t i = 0; i < c->nnodes; i++) {
    cnode * n = &c->nodes[i];
    switch (n->type) {
    case N_CONST: break;
    case N_LEAF: n->value = c->leaves[n->a].result; break;
    case N_NOT: n->value = tri_not(c->nodes[n->a].value); break;
    case N_AND: n->value = tri_and(c->nodes[n->a].value, c->nodes[n->b].value); break;
    case N_OR: n->value = tri_or(c->nodes[n->a].value, c->nodes[n->b].value); break;
    }
  }
  return c->nodes[root].value;
}

static int step_if(css_sub * sub, size_t fi) {
  int pre = fn_prelude(sub, fi);
  if (pre) {
    return pre;
  }
  frame_t * f = &sub->frames[fi];
  if (f->has_arrived) {
    take_arrived(f);
  }
  GTEXT_CSS_Status st;
  for (;;) {
    f = &sub->frames[fi];
    switch (f->phase) {
    case 10: {
      st = split_args(sub, f, GTEXT_CSS_TOKEN_SEMICOLON, false);
      if (st != GTEXT_CSS_OK) {
        SUB_FAIL(sub, st);
        return STEP_ERROR;
      }
      /* a trailing `;` leaves an empty last branch, which the grammar allows; every other branch needs a colon ("<if-args-branch> = <declaration-value> :
         <declaration-value>?") or the whole function does not parse */
      if (f->nargs && f->args[f->nargs - 1].count == 0) {
        f->nargs--;
      }
      for (size_t b = 0; b < f->nargs; b++) {
        bool colon = false;
        for (size_t i = 0; i < f->args[b].count; i++) {
          colon = colon || is_tok(&f->vals[f->args[b].from + i], GTEXT_CSS_TOKEN_COLON);
        }
        if (!colon) {
          fn_finish(sub, fi, NULL, true);
          return STEP_DONE;
        }
      }
      f->branch = 0;
      f->phase = 11;
      continue;
    }
    case 11: {
      cond_free(&sub->alloc, f->aux);
      f->aux = NULL;
      if (f->branch >= f->nargs) {
        /* "Return nothing (an empty sequence of component values)." */
        fn_finish(sub, fi, css_rope_retain(sub->empty), false);
        return STEP_DONE;
      }
      const range_t br = f->args[f->branch];
      size_t colon = 0;
      while (!is_tok(&f->vals[br.from + colon], GTEXT_CSS_TOKEN_COLON)) {
        colon++;
      }
      /* the condition is arg 1's range, the value arg 2's: kept as offsets in the frame */
      f->aux_n = colon;
      size_t cb = br.from, ce = br.from + colon;
      while (cb < ce && is_ws(&f->vals[cb])) {
        cb++;
      }
      while (ce > cb && is_ws(&f->vals[ce - 1])) {
        ce--;
      }
      if (ce - cb == 1 && is_tok(&f->vals[cb], GTEXT_CSS_TOKEN_IDENT) && tok_ieq(&f->vals[cb].value, "else")) {
        f->phase = 14;
        continue;
      }
      f->phase = 12;
      const int r = seq_start(sub, fi, 0, f->csrc, f->vals + cb, ce - cb, false);
      if (r < 0) {
        return STEP_ERROR;
      }
      if (r == 1) {
        return STEP_PUSHED;
      }
      continue;
    }
    case 12: {
      /* "Substitute arbitrary substitution functions in the first <declaration-value> of branch, then parse the result as an <if-condition>. If parsing returns
         failure, continue" */
      GTEXT_CSS_Tree * t = NULL;
      if (!f->res[0].gi && f->res[0].rope) {
        st = small_parse(sub, f->res[0].rope, sub->max_bytes, &t);
        if (st == GTEXT_CSS_E_OOM) {
          SUB_FAIL(sub, st);
          return STEP_ERROR;
        }
      }
      f = &sub->frames[fi];
      if (!t) {
        f->branch++;
        f->phase = 11;
        continue;
      }
      GTEXT_Allocator a = sub->alloc;
      cond_state * c = gtext_allocator_calloc(&a, 1, sizeof *c);
      if (!c) {
        gtext_css_tree_free(t);
        SUB_FAIL(sub, GTEXT_CSS_E_OOM);
        return STEP_ERROR;
      }
      c->alloc = a;
      c->tree = t;
      bool ok = false;
      int root = -1;
      /* `else` may be the whole of the substituted condition too */
      const GTEXT_CSS_Component_Value * one = single(t);
      if (one && is_tok(one, GTEXT_CSS_TOKEN_IDENT) && tok_ieq(&one->value, "else")) {
        root = cond_node(c, N_CONST, T_TRUE, 0, 0);
        ok = root >= 0;
        st = root >= 0 ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
      }
      else {
        st = cond_parse(c, t->values, t->value_count, &ok, &root);
      }
      if (st != GTEXT_CSS_OK) {
        cond_free(&a, c);
        SUB_FAIL(sub, st);
        return STEP_ERROR;
      }
      f->aux = c;
      if (!ok) {
        f->branch++;
        f->phase = 11;
        continue;
      }
      c->root = root;
      f->phase = 13;
      continue;
    }
    case 13: {
      /* the tests: each is asked in turn; a style query may need a custom property computed first */
      cond_state * c = f->aux;
      while (c->next_leaf < c->nleaves) {
        cleaf * leaf = &c->leaves[c->next_leaf];
        if (leaf->type == L_STYLE) {
          const int r = style_leaf(sub, fi, leaf);
          if (r == STEP_PUSHED) {
            return STEP_PUSHED;
          }
          if (r == STEP_ERROR) {
            return STEP_ERROR;
          }
        }
        else {
          const GTEXT_CSS_Environment * env = &sub->cc->env;
          if (!env->condition) {
            st = css_st_fail(sub->st, GTEXT_CSS_E_UNSUPPORTED, "condition",
                "an if() has a media() or supports() test and the environment has no condition callback to answer it");
            SUB_FAIL(sub, st);
            return STEP_ERROR;
          }
          dbuf text;
          dbuf_init(&text, &sub->alloc);
          st = group_text(sub, leaf->vals, leaf->count, &text);
          if (st != GTEXT_CSS_OK || text.failed) {
            dbuf_free(&text);
            SUB_FAIL(sub, st == GTEXT_CSS_OK ? GTEXT_CSS_E_OOM : st);
            return STEP_ERROR;
          }
          GTEXT_CSS_String arg = {text.data ? text.data : "", text.len};
          const GTEXT_CSS_Tristate answer = env->condition(env->user, leaf->type == L_MEDIA ? GTEXT_CSS_CONDITION_MEDIA : GTEXT_CSS_CONDITION_SUPPORTS, &arg,
              sub->st->element);
          dbuf_free(&text);
          f = &sub->frames[fi];
          c = f->aux;
          leaf = &c->leaves[c->next_leaf];
          leaf->result = answer == GTEXT_CSS_TRI_TRUE ? T_TRUE : answer == GTEXT_CSS_TRI_FALSE ? T_FALSE : T_UNKNOWN;
        }
        f = &sub->frames[fi];
        c = f->aux;
        c->next_leaf++;
      }
      /* "If the result of condition is false, continue." (An unknown at the top is false.) */
      const tri truth = cond_fold(c, c->root);
      if (truth != T_TRUE) {
        f->branch++;
        f->phase = 11;
        continue;
      }
      f->phase = 14;
      continue;
    }
    case 14: {
      /* "Substitute arbitrary substitution functions in the second <declaration-value> of branch, and return the result." */
      cond_free(&sub->alloc, f->aux);
      f->aux = NULL;
      const range_t br = f->args[f->branch];
      size_t vb = br.from + f->aux_n + 1, ve = br.from + br.count;
      while (vb < ve && is_ws(&f->vals[vb])) {
        vb++;
      }
      while (ve > vb && is_ws(&f->vals[ve - 1])) {
        ve--;
      }
      f->phase = 15;
      const int r = seq_start(sub, fi, 1, f->csrc, f->vals + vb, ve - vb, false);
      if (r < 0) {
        return STEP_ERROR;
      }
      if (r == 1) {
        return STEP_PUSHED;
      }
      continue;
    }
    default: {
      css_rope * out = css_rope_retain(f->res[1].rope);
      const bool gi = f->res[1].gi;
      fn_finish(sub, fi, out, gi);
      return STEP_DONE;
    }
    }
  }
}

/* ---- a custom property ------------------------------------------------------------------------------------------- */

/// The registration of an entry, or NULL.
static const css_ereg * entry_reg(css_sub * sub, const entry_t * e) {
  return e->reg >= 0 ? &sub->cc->regs[e->reg] : NULL;
}

/// Settle an entry with a value (retained; NULL is the guaranteed-invalid value), and the frame's result.
static void prop_settle(css_sub * sub, size_t fi, GTEXT_CSS_Custom_Value * v /* taken */) {
  frame_t * f = &sub->frames[fi];
  entry_t * e = &sub->entries[f->entry];
  gtext_css_custom_value_free(e->value);
  e->value = v;
  e->state = ENT_DONE;
  f->out = v ? css_rope_retain(css_cval_rope(v)) : NULL;
  f->out_gi = v == NULL;
}

/// Whether a substituted value is only a CSS-wide keyword (white space and comments around it are nothing): GTEXT_CSS_WIDE_* in `*wide`, or 0. The rolling
/// keywords answer GTEXT_CSS_WIDE_REVERT (all three: they are alike here).
static GTEXT_CSS_Status rope_wide_keyword(css_sub * sub, css_rope * rope, int * wide) {
  *wide = 0;
  if (!rope || css_rope_bytes(rope) > 128) {
    return GTEXT_CSS_OK;
  }
  char * text = NULL;
  size_t len = 0;
  GTEXT_CSS_Status status = css_rope_flatten(&sub->alloc, rope, 128, &text, &len, NULL, NULL, &sub->work);
  if (status == GTEXT_CSS_E_LIMIT_OUTPUT) {
    return GTEXT_CSS_OK;
  }
  if (status != GTEXT_CSS_OK) {
    return status;
  }
  size_t i = 0, e = len;
  for (;;) {
    while (i < e && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' || text[i] == '\f')) {
      i++;
    }
    if (i + 1 < e && text[i] == '/' && text[i + 1] == '*') {
      const char * end = memchr(text + i + 2, '*', e - i - 2);
      while (end && end + 1 < text + e && end[1] != '/') {
        end = memchr(end + 1, '*', (size_t)(text + e - end - 1));
      }
      if (!end || end + 1 >= text + e) {
        gtext_allocator_free(&sub->alloc, text);
        return GTEXT_CSS_OK;
      }
      i = (size_t)(end + 2 - text);
      continue;
    }
    break;
  }
  for (;;) {
    while (e > i && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\n' || text[e - 1] == '\r' || text[e - 1] == '\f')) {
      e--;
    }
    if (e >= i + 4 && text[e - 1] == '/' && text[e - 2] == '*') {
      size_t k = e - 2;
      while (k > i + 1 && !(text[k - 1] == '/' && text[k] == '*')) {
        k--;
      }
      if (k > i && text[k - 1] == '/' && text[k] == '*') {
        e = k - 1;
        continue;
      }
    }
    break;
  }
  static const struct { const char * word; int kind; } words[] = {
      {"initial", GTEXT_CSS_WIDE_INITIAL}, {"inherit", GTEXT_CSS_WIDE_INHERIT}, {"unset", GTEXT_CSS_WIDE_UNSET},
      {"revert", GTEXT_CSS_WIDE_REVERT}, {"revert-layer", GTEXT_CSS_WIDE_REVERT}, {"revert-rule", GTEXT_CSS_WIDE_REVERT}};
  for (size_t k = 0; k < sizeof words / sizeof words[0]; k++) {
    const size_t wl = strlen(words[k].word);
    const GTEXT_CSS_String candidate = {text + i, e - i};
    if (e - i == wl && css_ascii_ieq(&candidate, words[k].word)) {
      *wide = words[k].kind;
      break;
    }
  }
  gtext_allocator_free(&sub->alloc, text);
  return GTEXT_CSS_OK;
}

static int step_prop(css_sub * sub, size_t fi) {
  frame_t * f = &sub->frames[fi];
  entry_t * e = &sub->entries[f->entry];
  css_state * st = sub->st;
  GTEXT_CSS_Status status;
  if (f->phase == 0) {
    GTEXT_CSS_Declaration_Item it;
    if (!gtext_css_block_item(st->block, e->item, &it)) {
      SUB_FAIL(sub, css_st_fail(st, GTEXT_CSS_E_INTERNAL, NULL, "the block changed while it was read"));
      return STEP_ERROR;
    }
    e->state = ENT_INPROGRESS;
    e->frame = fi;
    const css_ereg * reg = entry_reg(sub, e);
    if (it.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
      const GTEXT_CSS_Custom_Value * v = NULL;
      switch (it.wide) {
      case GTEXT_CSS_WIDE_INITIAL:
        v = reg ? reg->initial : NULL;
        break;
      case GTEXT_CSS_WIDE_INHERIT:
        v = parent_value(sub, e->name.data, e->name.length, e->reg);
        break;
      case GTEXT_CSS_WIDE_UNSET:
        v = !reg || reg->creg->inherits ? parent_value(sub, e->name.data, e->name.length, e->reg) : reg->initial;
        break;
      default:
        SUB_FAIL(sub, css_st_fail(st, GTEXT_CSS_E_INVALID, "--*", "revert and revert-layer are the cascade's: they are resolved before a style is computed"));
        return STEP_ERROR;
      }
      prop_settle(sub, fi, gtext_css_custom_value_retain(v));
      return STEP_DONE;
    }
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(it.set);
    const char * text = root && root->text.data ? root->text.data : "";
    const size_t len = root && root->text.data ? root->text.length : 0;
    if (len > sub->max_bytes) {
      SUB_FAIL(sub, css_st_fail(st, GTEXT_CSS_E_LIMIT_CUSTOM_VALUE, e->name.data, "a custom property value is longer than max_value_bytes"));
      return STEP_ERROR;
    }
    src_t * src = NULL;
    status = src_open(&sub->alloc, text, len, NULL, false, &src);
    if (status != GTEXT_CSS_OK) {
      SUB_FAIL(sub, status == GTEXT_CSS_E_OOM ? status : css_st_fail(st, status, e->name.data, "the value of a custom property could not be read"));
      return STEP_ERROR;
    }
    f = &sub->frames[fi];
    f->own_src = src;
    f->src = src;
    f->phase = 1;
    const int r = seq_start(sub, fi, 0, src, src->tree->values, src->tree->value_count, false);
    if (r < 0) {
      return STEP_ERROR;
    }
    if (r == 1) {
      sub->frames[sub->nframes - 1].run_from = 0; /* what comes before the first token (a comment) is part of the value */
      return STEP_PUSHED;
    }
    f = &sub->frames[fi];
  }
  take_arrived(f);
  e = &sub->entries[f->entry];
  const css_ereg * reg = entry_reg(sub, e);
  const bool typed = reg && !reg->creg->syntax->universal;
  bool gi = f->cyclic || f->res[0].gi || !f->res[0].rope;
  css_rope * final = NULL;
  const GTEXT_CSS_Custom_Value * initial_cv = typed ? reg->initial : NULL;
  if (!gi && !charge(sub, f->res[0].rope)) {
    gi = true;
  }
  if (!gi) {
    /* "If a property value, after property replacement, contains only a single CSS-wide keyword (and possibly whitespace/comments), its value is determined as if
       that keyword were its specified value all along." A custom property is a property: `--c: var(--x, unset)` is `--c: unset`. The rolling keywords cannot be
       resolved here (the cascade is over), and are invalid at computed-value time as they are for any other property. */
    int wide = 0;
    status = rope_wide_keyword(sub, f->res[0].rope, &wide);
    if (status != GTEXT_CSS_OK) {
      SUB_FAIL(sub, status);
      return STEP_ERROR;
    }
    if (wide) {
      const GTEXT_CSS_Custom_Value * wv = NULL;
      if (wide == GTEXT_CSS_WIDE_INITIAL) {
        wv = reg ? reg->initial : NULL;
      }
      else if (wide == GTEXT_CSS_WIDE_INHERIT || (wide == GTEXT_CSS_WIDE_UNSET && (!reg || reg->creg->inherits))) {
        wv = parent_value(sub, e->name.data, e->name.length, e->reg);
      }
      else if (wide == GTEXT_CSS_WIDE_UNSET) {
        wv = reg->initial;
      }
      else {
        wv = typed ? initial_cv : NULL;
      }
      prop_settle(sub, fi, gtext_css_custom_value_retain(wv));
      return STEP_DONE;
    }
  }
  if (!gi) {
    css_rope * rope = f->res[0].rope;
    const bool tainted = css_rope_has_taint(rope);
    if (typed) {
      char * text = NULL;
      size_t len = 0;
      status = css_rope_flatten(&sub->alloc, rope, sub->max_bytes, &text, &len, NULL, NULL, &sub->work);
      if (status == GTEXT_CSS_E_LIMIT_OUTPUT) {
        sub->limited++;
        gi = true;
      }
      else if (status != GTEXT_CSS_OK) {
        SUB_FAIL(sub, status);
        return STEP_ERROR;
      }
      else {
        bool cycle = false;
        status = compute_typed(sub, reg->creg, text, len, tainted, &final, &cycle);
        gtext_allocator_free(&sub->alloc, text);
        if (status != GTEXT_CSS_OK) {
          SUB_FAIL(sub, status);
          return STEP_ERROR;
        }
        if (cycle) {
          gi = true;
        }
        else if (!final) {
          gi = true; /* not a value of the syntax: invalid at computed-value time */
        }
      }
    }
    else {
      final = tainted ? css_rope_taint(&sub->alloc, rope) : css_rope_retain(rope);
      if (!final) {
        SUB_FAIL(sub, GTEXT_CSS_E_OOM);
        return STEP_ERROR;
      }
    }
  }
  f = &sub->frames[fi];
  if (f->cyclic && final) {
    /* found to be in a cycle while its type was computed (the font size it needs read it) */
    css_rope_release(final);
    final = NULL;
  }
  GTEXT_CSS_Custom_Value * v = NULL;
  if (final) {
    v = css_cval_new(&sub->alloc, final);
    css_rope_release(final);
    if (!v) {
      SUB_FAIL(sub, GTEXT_CSS_E_OOM);
      return STEP_ERROR;
    }
  }
  else if (initial_cv) {
    /* "Otherwise [a registered custom property]: the property's initial value" */
    v = gtext_css_custom_value_retain(initial_cv);
  }
  prop_settle(sub, fi, v);
  return STEP_DONE;
}

/* ---- the machine ------------------------------------------------------------------------------------------------------- */

static int step(css_sub * sub, size_t fi) {
  switch (sub->frames[fi].kind) {
  case FR_SEQ: return step_seq(sub, fi);
  case FR_CONTAINER: return step_container(sub, fi);
  case FR_PROP: return step_prop(sub, fi);
  case FR_VAR:
  case FR_INHERIT: return step_var(sub, fi);
  case FR_ENV: return step_env(sub, fi);
  case FR_ATTR: return step_attr(sub, fi);
  case FR_IF: return step_if(sub, fi);
  case FR_IDENT: return step_ident(sub, fi);
  case FR_RANDOM_ITEM: return step_random_item(sub, fi);
  case FR_RANDOM: return step_random(sub, fi);
  case FR_TREE: return step_tree(sub, fi);
  default:
    SUB_FAIL(sub, GTEXT_CSS_E_INTERNAL);
    return STEP_ERROR;
  }
}

/// Run the frames above `base` until the one at `base` is finished. Its result goes to `*out` (a rope, or NULL for the guaranteed-invalid value).
static GTEXT_CSS_Status run(css_sub * sub, size_t base, css_rope ** out) {
  *out = NULL;
  while (sub->nframes > base) {
    const size_t fi = sub->nframes - 1;
    sub->work++;
    const int r = step(sub, fi);
    if (r == STEP_ERROR || sub->failure != GTEXT_CSS_OK) {
      while (sub->nframes > base) {
        frame_pop(sub);
      }
      return sub->failure != GTEXT_CSS_OK ? sub->failure : GTEXT_CSS_E_INTERNAL;
    }
    if (r == STEP_PUSHED) {
      continue;
    }
    frame_t * f = &sub->frames[fi];
    css_rope * result = f->out;
    const bool gi = f->out_gi || !result;
    f->out = NULL;
    frame_pop(sub);
    if (sub->nframes > base) {
      deliver(sub, gi ? NULL : result, gi);
      if (gi) {
        css_rope_release(result);
      }
    }
    else {
      *out = gi ? NULL : result;
      if (gi) {
        css_rope_release(result);
      }
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- the public face to computing ------------------------------------------------------------------------------------------- */

void css_sub_free(css_sub * sub) {
  if (!sub) {
    return;
  }
  GTEXT_Allocator a = sub->alloc;
  while (sub->nframes) {
    frame_pop(sub);
  }
  for (size_t i = 0; i < sub->nentries; i++) {
    gtext_css_custom_value_free(sub->entries[i].value);
  }
  css_rope_release(sub->empty);
  css_rope_release(sub->comment);
  gtext_allocator_free(&a, sub->entries);
  gtext_allocator_free(&a, sub->frames);
  gtext_allocator_free(&a, sub->aguards);
  gtext_allocator_free(&a, sub);
}

static size_t clamp_limit(size_t v, size_t dflt, size_t maxv) {
  return v == 0 ? dflt : (v > maxv ? maxv : v);
}

static uint64_t clamp_limit64(uint64_t v, uint64_t dflt, uint64_t maxv) {
  return v == 0 ? dflt : (v > maxv ? maxv : v);
}

GTEXT_CSS_Status css_sub_new(css_state * st, css_sub ** out) {
  *out = NULL;
  const GTEXT_CSS_Compute_Context * cc = st->cc;
  const size_t nitems = st->block ? gtext_css_block_count(st->block) : 0;
  size_t ncustom = 0;
  bool pending = false;
  for (size_t i = 0; i < nitems; i++) {
    GTEXT_CSS_Declaration_Item it;
    if (!gtext_css_block_item(st->block, i, &it)) {
      continue;
    }
    if (it.property == cc->pid[CSS_P_CUSTOM] && it.custom_name.data) {
      ncustom++;
    }
    pending = pending || it.outcome == GTEXT_CSS_PROP_RESULT_PENDING;
  }
  if (!ncustom && !pending) {
    return GTEXT_CSS_OK;
  }
  GTEXT_Allocator a = cc->alloc;
  css_sub * sub = gtext_allocator_calloc(&a, 1, sizeof *sub);
  if (!sub) {
    return GTEXT_CSS_E_OOM;
  }
  sub->st = st;
  sub->cc = cc;
  sub->alloc = a;
  sub->max_depth = clamp_limit(cc->sub.max_substitution_depth, DEFAULT_MAX_DEPTH, LIMIT_MAX_DEPTH);
  sub->max_tokens = clamp_limit64(cc->sub.max_substituted_tokens, DEFAULT_MAX_TOKENS, LIMIT_MAX_TOKENS);
  sub->max_element_tokens = clamp_limit64(cc->sub.max_element_substituted_tokens, DEFAULT_MAX_ELEMENT_TOKENS, LIMIT_MAX_ELEMENT_TOKENS);
  sub->max_bytes = clamp_limit(cc->sub.max_value_bytes, DEFAULT_MAX_BYTES, LIMIT_MAX_BYTES);
  sub->empty = css_rope_leaf(&a, "", 0, CSS_TC_NONE, CSS_TC_NONE, 0, false);
  sub->comment = css_rope_leaf(&a, "/**/", 4, CSS_TC_SAFE, CSS_TC_SAFE, 0, false);
  if (!sub->empty || !sub->comment) {
    css_sub_free(sub);
    return GTEXT_CSS_E_OOM;
  }
  if (ncustom) {
    const size_t max_custom = clamp_limit(cc->sub.max_custom_properties, DEFAULT_MAX_CUSTOM, LIMIT_MAX_CUSTOM);
    entry_t * entries = gtext_allocator_calloc(&a, ncustom, sizeof *entries);
    if (!entries) {
      css_sub_free(sub);
      return GTEXT_CSS_E_OOM;
    }
    size_t n = 0;
    for (size_t i = 0; i < nitems; i++) {
      GTEXT_CSS_Declaration_Item it;
      if (gtext_css_block_item(st->block, i, &it) && it.property == cc->pid[CSS_P_CUSTOM] && it.custom_name.data) {
        entries[n].name = it.custom_name;
        entries[n].item = i;
        entries[n].reg = -1;
        n++;
      }
    }
    qsort(entries, n, sizeof *entries, entry_cmp);
    /* the last declaration of a name is the one that counts */
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
      if (i + 1 < n && entries[i].name.length == entries[i + 1].name.length && memcmp(entries[i].name.data, entries[i + 1].name.data, entries[i].name.length) == 0) {
        continue;
      }
      entries[m++] = entries[i];
    }
    sub->entries = entries;
    sub->nentries = m;
    st->work += n;
    if (m > max_custom) {
      css_st_fail(st, GTEXT_CSS_E_LIMIT_CUSTOM_PROPERTIES, NULL, "an element declares more custom properties than max_custom_properties");
      css_sub_free(sub);
      return GTEXT_CSS_E_LIMIT_CUSTOM_PROPERTIES;
    }
    for (size_t i = 0; i < m; i++) {
      size_t reg = 0;
      if (cc->nregs && css_ctx_find_reg(cc, entries[i].name.data, entries[i].name.length, &reg)) {
        entries[i].reg = (long)reg;
      }
    }
  }
  *out = sub;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_sub_customs(css_sub * sub) {
  css_state * st = sub->st;
  GTEXT_CSS_Computed_Style * style = st->style;
  for (size_t i = 0; i < sub->nentries; i++) {
    if (sub->entries[i].state != ENT_UNRESOLVED) {
      continue;
    }
    GTEXT_CSS_Status status = GTEXT_CSS_OK;
    const size_t base = sub->nframes;
    frame_t * f = frame_push(sub, FR_PROP, &status);
    if (!f) {
      return status;
    }
    f->entry = i;
    css_rope * r = NULL;
    status = run(sub, base, &r);
    css_rope_release(r);
    if (status != GTEXT_CSS_OK) {
      return status;
    }
  }
  /* install them: the registered ones in the style's table, the others in its map */
  GTEXT_Allocator a = sub->alloc;
  for (size_t i = 0; i < sub->nentries; i++) {
    entry_t * e = &sub->entries[i];
    if (e->reg >= 0) {
      gtext_css_custom_value_free(style->regvals[e->reg]);
      style->regvals[e->reg] = gtext_css_custom_value_retain(e->value);
      continue;
    }
    const bool hides = css_cmap_find(style->custom, e->name.data, e->name.length) != NULL;
    if (!e->value && !hides) {
      continue; /* the guaranteed-invalid value is what the name has already */
    }
    css_cmap * next = NULL;
    const GTEXT_CSS_Status set = css_cmap_set(&a, style->custom, e->name.data, e->name.length, e->value, sub->cc->options.custom_hash_seed, &next, &st->work);
    if (set != GTEXT_CSS_OK) {
      return css_st_fail(st, set, NULL, NULL);
    }
    css_cmap_release(style->custom);
    style->custom = next;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_sub_substitute(css_sub * sub, const char * prop, size_t prop_length, const GTEXT_CSS_Value_Set * pending, css_rope ** out) {
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(pending);
  return css_sub_substitute_text(sub, prop, prop_length, root && root->text.data ? root->text.data : "", root && root->text.data ? root->text.length : 0, out);
}

GTEXT_CSS_Status css_sub_substitute_text(css_sub * sub, const char * prop, size_t prop_length, const char * text, size_t len, css_rope ** out) {
  *out = NULL;
  sub->prop = prop;
  sub->prop_len = prop_length;
  sub->random_index = 0;
  if (len > sub->max_bytes) {
    sub->limited++;
    return GTEXT_CSS_OK;
  }
  src_t * src = NULL;
  GTEXT_CSS_Status status = src_open(&sub->alloc, text, len, NULL, false, &src);
  if (status != GTEXT_CSS_OK) {
    if (status == GTEXT_CSS_E_OOM) {
      return status;
    }
    return GTEXT_CSS_OK; /* a limit of the parser: not a value */
  }
  css_rope * result = NULL;
  if (src->tree->value_count == 0) {
    result = css_rope_retain(sub->empty);
  }
  else {
    const size_t base = sub->nframes;
    frame_t * f = frame_push(sub, FR_SEQ, &status);
    if (!f) {
      src_close(&sub->alloc, src);
      return status;
    }
    f->src = src;
    f->vals = src->tree->values;
    f->nvals = src->tree->value_count;
    f->run_from = f->vals[0].span.offset;
    status = run(sub, base, &result);
  }
  src_close(&sub->alloc, src);
  if (status != GTEXT_CSS_OK) {
    return status;
  }
  if (result && !charge(sub, result)) {
    css_rope_release(result);
    result = NULL;
  }
  *out = result;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_sub_cycle(css_sub * sub, const GTEXT_CSS_Value_Set * pending, bool * is_cycle, bool * invalid, css_cycle_arg ** args, size_t * nargs) {
  *is_cycle = false;
  *invalid = false;
  *args = NULL;
  *nargs = 0;
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(pending);
  if (!root || !root->text.data || root->text.length < 6) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_String head = {root->text.data, 5};
  if (!css_ascii_ieq(&head, "cycle") || root->text.data[5] != '(') {
    return GTEXT_CSS_OK;
  }
  src_t * src = NULL;
  GTEXT_CSS_Status st = src_open(&sub->alloc, root->text.data, root->text.length, NULL, false, &src);
  if (st != GTEXT_CSS_OK) {
    return st == GTEXT_CSS_E_OOM ? st : GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Component_Value * only = single(src->tree);
  if (!only || only->kind != GTEXT_CSS_VALUE_FUNCTION || !fn_name_is(&only->value, "cycle")) {
    src_close(&sub->alloc, src);
    return GTEXT_CSS_OK;
  }
  *is_cycle = true;
  GTEXT_Allocator a = sub->alloc;
  size_t commas = 0;
  for (size_t i = 0; i < only->child_count; i++) {
    commas += is_tok(&only->children[i], GTEXT_CSS_TOKEN_COMMA);
  }
  css_cycle_arg * list = gtext_allocator_calloc(&a, commas + 1, sizeof *list);
  if (!list) {
    src_close(&sub->alloc, src);
    return GTEXT_CSS_E_OOM;
  }
  size_t n = 0, start = 0;
  for (size_t i = 0; i <= only->child_count && st == GTEXT_CSS_OK; i++) {
    if (i == only->child_count || is_tok(&only->children[i], GTEXT_CSS_TOKEN_COMMA)) {
      size_t b = start, e = i;
      while (b < e && is_ws(&only->children[b])) {
        b++;
      }
      while (e > b && is_ws(&only->children[e - 1])) {
        e--;
      }
      if (e == b) {
        *invalid = true; /* "cycle( <whole-value># )": every argument is a value */
      }
      else {
        const size_t from = only->children[b].span.offset, to = only->children[e - 1].span.end;
        list[n].text = gtext_allocator_malloc(&a, to - from + 1);
        if (!list[n].text) {
          st = GTEXT_CSS_E_OOM;
          break;
        }
        memcpy(list[n].text, src->text + from, to - from);
        list[n].text[to - from] = '\0';
        list[n].length = to - from;
      }
      n++;
      start = i + 1;
    }
  }
  /* "However, it is not allowed to be nested, nor may it contain attr() or calc() notations; declarations containing such constructs are invalid." */
  if (st == GTEXT_CSS_OK && !*invalid) {
    typedef struct {
      const GTEXT_CSS_Component_Value * v;
      size_t n, i;
    } fr;
    size_t cap = 16, len = 0;
    fr * stack = gtext_allocator_malloc(&a, cap * sizeof *stack);
    if (!stack) {
      st = GTEXT_CSS_E_OOM;
    }
    else {
      stack[len++] = (fr){only->children, only->child_count, 0};
      while (len && !*invalid) {
        fr * f = &stack[len - 1];
        if (f->i >= f->n) {
          len--;
          continue;
        }
        const GTEXT_CSS_Component_Value * c = &f->v[f->i++];
        if (c->kind == GTEXT_CSS_VALUE_FUNCTION && (fn_name_is(&c->value, "cycle") || fn_name_is(&c->value, "attr") || fn_name_is(&c->value, "calc"))) {
          *invalid = true;
        }
        if (c->kind != GTEXT_CSS_VALUE_TOKEN && c->child_count) {
          if (len == cap) {
            fr * g = gtext_allocator_realloc(&a, stack, cap * 2 * sizeof *g);
            if (!g) {
              st = GTEXT_CSS_E_OOM;
              break;
            }
            stack = g;
            cap *= 2;
          }
          stack[len++] = (fr){c->children, c->child_count, 0};
        }
      }
      gtext_allocator_free(&a, stack);
    }
  }
  src_close(&sub->alloc, src);
  if (st != GTEXT_CSS_OK) {
    css_cycle_args_free(sub, list, n);
    return st;
  }
  *args = list;
  *nargs = n;
  return GTEXT_CSS_OK;
}

void css_cycle_args_free(css_sub * sub, css_cycle_arg * args, size_t nargs) {
  GTEXT_Allocator a = sub->alloc;
  for (size_t i = 0; i < nargs; i++) {
    gtext_allocator_free(&a, args[i].text);
  }
  gtext_allocator_free(&a, args);
}

GTEXT_CSS_Status css_sub_flatten(css_sub * sub, const css_rope * rope, char ** text, size_t * length, css_range ** taint, size_t * ntaint) {
  *text = NULL;
  const GTEXT_CSS_Status st = css_rope_flatten(&sub->alloc, rope, sub->max_bytes, text, length, taint, ntaint, &sub->work);
  if (st == GTEXT_CSS_E_LIMIT_OUTPUT) {
    sub->limited++;
    return GTEXT_CSS_OK;
  }
  return st;
}

void css_sub_report(const css_sub * sub, GTEXT_CSS_Compute_Result * result) {
  if (!sub || !result) {
    return;
  }
  result->substituted_tokens = sub->element_tokens;
  result->substitution_limited = sub->limited;
  result->substitution_peak = sub->peak;
  result->work += sub->work;
}

GTEXT_CSS_Status css_st_sibling(css_state * st, bool index, double * out) {
  const GTEXT_CSS_Element_Interface * iface = st->iface;
  const GTEXT_CSS_Element_Callback cb = index ? GTEXT_CSS_ELEMENT_CB_SIBLING_INDEX : GTEXT_CSS_ELEMENT_CB_SIBLING_COUNT;
  if (!iface || !gtext_css_element_has(iface, cb) || !st->element) {
    return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, index ? "sibling_index" : "sibling_count",
        "the element table has no callback that counts the siblings (or no element was given)");
  }
  size_t n = 0;
  const GTEXT_CSS_Status status = index ? gtext_css_element_get_sibling_index(iface, st->element, &n, NULL)
                                        : gtext_css_element_get_sibling_count(iface, st->element, &n, NULL);
  if (status != GTEXT_CSS_OK) {
    return css_st_fail(st, status, index ? "sibling_index" : "sibling_count", NULL);
  }
  *out = (double)n;
  return GTEXT_CSS_OK;
}
