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
 * The CSS parser: CSS Syntax section 5, every entry point, with nesting.
 *
 * **No recursion.** The standard's algorithms call each other recursively in
 * two ways: a component value contains component values (blocks and
 * functions), and a rule's block contains rules. Both are written here as
 * loops over explicit stacks: cv_build() keeps a stack of open blocks and
 * functions, and the rule machine (run_rules()) a stack of open rule blocks,
 * so the depth of the input costs memory, which `max_depth` bounds, and
 * never the C stack.
 *
 * **Speculation is bounded.** "Consume a block's contents" tries each item as
 * a declaration and, if that returns nothing, reads it again as a qualified
 * rule. Done literally that is quadratic: `a{b{c{...}}}` reads every inner
 * block once for each block around it. The standard's implementation note
 * lists the early exits that avoid it, and they are what spec_decl() does:
 * it decides whether "consume a declaration" would return something by
 * looking only at the top level of the value, stepping over each block and
 * function by the bracket matching the tokenizer computed, and it stops at
 * the first token that settles the answer. Only a declaration that will be
 * returned is then built. What this changes is the work, never the answer:
 * the predicate is exactly the one of 5.5.6 (a custom property is always a
 * declaration; otherwise a top-level {}-block must be the whole value, an
 * `!important` after it aside), and building checks that it agrees.
 *
 * **The nodes are values in arrays.** Children are built on a shared stack
 * and copied into the tree's arena when their parent closes, so a node's
 * children are one contiguous array and nothing is linked. Identities are
 * given in a last pass, in document order, so that they are a function of
 * the tree alone.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "css_internal.h"

/* ---- vectors ---------------------------------------------------------- */

/// A growable array of elements of one size.
typedef struct {
  unsigned char * data; ///< The elements.
  size_t len;           ///< In use.
  size_t cap;           ///< Allocated.
  size_t size;          ///< Bytes of one element.
} vec;

static void vec_init(vec * v, size_t size) {
  v->data = NULL;
  v->len = v->cap = 0;
  v->size = size;
}

static void * vec_push(css_ctx * ctx, vec * v) {
  if (v->len == v->cap) {
    const size_t cap = v->cap ? v->cap * 2 : 32;
    if (cap > SIZE_MAX / v->size) {
      css_fail(ctx, GTEXT_CSS_E_OOM);
      return NULL;
    }
    unsigned char * grown = gtext_allocator_realloc(&ctx->alloc, v->data, cap * v->size);
    if (!grown) {
      css_fail(ctx, GTEXT_CSS_E_OOM);
      return NULL;
    }
    v->data = grown;
    v->cap = cap;
  }
  void * slot = v->data + v->len * v->size;
  memset(slot, 0, v->size);
  v->len++;
  return slot;
}

static void * vec_at(const vec * v, size_t i) {
  /* An empty vector has no array: the place of its (empty) end is no pointer at all. */
  return v->data ? v->data + i * v->size : NULL;
}

static void vec_free(css_ctx * ctx, vec * v) {
  gtext_allocator_free(&ctx->alloc, v->data);
  v->data = NULL;
  v->len = v->cap = 0;
}

/* ---- token streams (5.3) ---------------------------------------------- */

/// A token stream (5.3): the tokens and the index of the next one.
typedef struct {
  const css_tok * toks; ///< The tokens.
  size_t n;             ///< Their number.
  size_t i;             ///< The next token.
  const size_t * match; ///< NULL for a stream read again (a unicode-range value).
  const css_tok * eof;  ///< The end-of-file token, past the last.
} tstream;

static const css_tok * ts_at(const tstream * s, size_t j) {
  return j < s->n ? &s->toks[j] : s->eof;
}

static const css_tok * ts_peek(const tstream * s) {
  return ts_at(s, s->i);
}

static void ts_discard(tstream * s) {
  if (s->i < s->n) {
    s->i++;
  }
}

static void ts_skip_ws(tstream * s) {
  while (ts_peek(s)->type == GTEXT_CSS_TOKEN_WHITESPACE) {
    s->i++;
  }
}

static bool is_opener(uint8_t type) {
  return type == GTEXT_CSS_TOKEN_FUNCTION || type == GTEXT_CSS_TOKEN_OPEN_PAREN
      || type == GTEXT_CSS_TOKEN_OPEN_SQUARE || type == GTEXT_CSS_TOKEN_OPEN_CURLY;
}

static uint8_t mirror(uint8_t type) {
  switch (type) {
  case GTEXT_CSS_TOKEN_OPEN_SQUARE: return GTEXT_CSS_TOKEN_CLOSE_SQUARE;
  case GTEXT_CSS_TOKEN_OPEN_CURLY: return GTEXT_CSS_TOKEN_CLOSE_CURLY;
  default: return GTEXT_CSS_TOKEN_CLOSE_PAREN;
  }
}

/* ---- the parser ------------------------------------------------------- */

enum {
  RF_BASE_SHEET,  ///< Consume a stylesheet's contents.
  RF_BASE_DECLS,  ///< Parse a list of declarations.
  RF_BASE_BLOCK,  ///< Consume a block's contents, over the whole input.
  RF_BASE_SINGLE, ///< Parse a rule.
  RF_BLOCK        ///< A rule's block being read.
};

/// A frame of the rule machine: the base context of an entry point, or a rule's block.
typedef struct {
  int kind;             ///< RF_*.
  bool nested;          ///< RF_BLOCK: the rule was consumed with nested set.
  bool discard;         ///< RF_BLOCK: the block is consumed and the rule dropped.
  GTEXT_CSS_Rule rule;  ///< RF_BLOCK: the rule, its prelude read.
  size_t item_base;     ///< Where this frame's items begin in `items`.
  size_t decl_base;     ///< Where its current run of declarations begins.
  int phase;            ///< RF_BASE_SINGLE: 0 before the rule, 1 after.
  bool have_rule;       ///< RF_BASE_SINGLE: a rule was returned.
} rframe;

/// An item of a block's contents while the block is read.
typedef struct {
  int kind;                       ///< GTEXT_CSS_ITEM_*.
  GTEXT_CSS_Rule rule;            ///< For a rule.
  GTEXT_CSS_Declaration * decls;  ///< For a run (arena).
  size_t ndecls;                  ///< Its length.
} pitem;

/// An open block or function while a component value is read.
typedef struct {
  size_t base;    ///< Where the frame's children begin on the value stack.
  size_t tok;     ///< The opening token's index.
  const css_tok * open; ///< The opening token.
} cvframe;

/// The parser: the context and its stacks.
typedef struct {
  css_ctx * ctx;   ///< The parse context.
  tstream ts;      ///< The input's tokens.
  vec values;      ///< GTEXT_CSS_Component_Value: the value stack.
  vec cvframes;    ///< cvframe.
  vec decls;       ///< GTEXT_CSS_Declaration.
  vec items;       ///< pitem.
  vec rframes;     ///< rframe.
} parser;

static bool ok(const parser * p) {
  return p->ctx->status == GTEXT_CSS_OK;
}

static GTEXT_CSS_Span tok_span(const css_tok * t) {
  GTEXT_CSS_Span s = {t->start, t->end, t->line, t->column};
  return s;
}

static GTEXT_CSS_Span point_span(const css_tok * t) {
  GTEXT_CSS_Span s = {t->start, t->start, t->line, t->column};
  return s;
}

static void err_at(parser * p, GTEXT_CSS_Error_Code code, const css_tok * t) {
  css_error(p->ctx, code, t->start, t->line, t->column);
}

static void node_from_tok(GTEXT_CSS_Component_Value * v, const css_tok * t) {
  memset(v, 0, sizeof *v);
  v->kind = GTEXT_CSS_VALUE_TOKEN;
  v->type = (GTEXT_CSS_Token_Type)t->type;
  v->flags = t->flags;
  v->span = tok_span(t);
  v->value = t->value;
  v->unit = t->unit;
  v->repr = t->repr;
  v->number = t->number;
  if (t->type == GTEXT_CSS_TOKEN_DELIM) {
    v->delim = t->delim;
  }
  if (t->type == GTEXT_CSS_TOKEN_UNICODE_RANGE) {
    v->range_start = t->delim;
    v->range_end = t->range_end;
  }
}

static GTEXT_CSS_Component_Value * vstack(parser * p, size_t i) {
  return (GTEXT_CSS_Component_Value *)vec_at(&p->values, i);
}

/* A closing token that becomes a token: always a parse error (5.2). */
static void preserved_closer_error(parser * p, const css_tok * t) {
  switch (t->type) {
  case GTEXT_CSS_TOKEN_CLOSE_CURLY: err_at(p, GTEXT_CSS_ERROR_UNMATCHED_CLOSE_BRACE, t); break;
  case GTEXT_CSS_TOKEN_CLOSE_PAREN: err_at(p, GTEXT_CSS_ERROR_UNMATCHED_CLOSE_PAREN, t); break;
  case GTEXT_CSS_TOKEN_CLOSE_SQUARE: err_at(p, GTEXT_CSS_ERROR_UNMATCHED_CLOSE_BRACKET, t); break;
  default: break;
  }
}

static bool push_token(parser * p, const css_tok * t) {
  GTEXT_CSS_Component_Value * v = vec_push(p->ctx, &p->values);
  if (!v) {
    return false;
  }
  node_from_tok(v, t);
  return true;
}

/* Copy the value stack from `base` into the arena and pop it. */
static bool take_values(parser * p, size_t base, const GTEXT_CSS_Component_Value ** out, size_t * n) {
  const size_t count = p->values.len - base;
  *n = count;
  *out = NULL;
  p->ctx->work += count;
  if (count == 0) {
    return true;
  }
  if (count > SIZE_MAX / sizeof(GTEXT_CSS_Component_Value)) {
    css_fail(p->ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  GTEXT_CSS_Component_Value * a = css_arena_alloc(&p->ctx->arena, count * sizeof *a);
  if (!a) {
    css_fail(p->ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  memcpy(a, vstack(p, base), count * sizeof *a);
  p->values.len = base;
  *out = a;
  return true;
}

/*
 * 5.5.8 consume a component value, 5.5.9 a simple block and 5.5.10 a
 * function, as one loop. With `keep` the value is pushed on the value stack;
 * without it (the remnants of a bad declaration) nothing is built, but the
 * parse errors are the same.
 */
static void cv_build(parser * p, tstream * s, bool keep) {
  const css_tok * t = ts_peek(s);
  if (!is_opener(t->type)) {
    s->i++;
    preserved_closer_error(p, t);
    if (keep) {
      (void)push_token(p, t);
    }
    return;
  }
  const size_t frames_base = p->cvframes.len;
  {
    cvframe * f = vec_push(p->ctx, &p->cvframes);
    if (!f) {
      return;
    }
    f->base = p->values.len;
    f->tok = s->i;
    f->open = t;
    s->i++;
  }
  while (p->cvframes.len > frames_base && ok(p)) {
    p->ctx->work++;
    cvframe * f = (cvframe *)vec_at(&p->cvframes, p->cvframes.len - 1);
    t = ts_peek(s);
    const bool closes = t->type == mirror(f->open->type);
    if (t->type == GTEXT_CSS_TOKEN_EOF || closes) {
      const cvframe fr = *f;
      p->cvframes.len--;
      if (closes) {
        s->i++;
      }
      if (!keep) {
        continue;
      }
      GTEXT_CSS_Component_Value node;
      memset(&node, 0, sizeof node);
      if (fr.open->type == GTEXT_CSS_TOKEN_FUNCTION) {
        node.kind = GTEXT_CSS_VALUE_FUNCTION;
        node.value = fr.open->value;
      }
      else {
        node.kind = GTEXT_CSS_VALUE_BLOCK;
      }
      node.type = (GTEXT_CSS_Token_Type)fr.open->type;
      node.span = tok_span(fr.open);
      node.span.end = closes ? t->end : t->start;
      if (!closes) {
        node.flags |= GTEXT_CSS_FLAG_UNCLOSED;
      }
      if (!take_values(p, fr.base, &node.children, &node.child_count)) {
        return;
      }
      GTEXT_CSS_Component_Value * v = vec_push(p->ctx, &p->values);
      if (!v) {
        return;
      }
      *v = node;
      continue;
    }
    if (is_opener(t->type)) {
      cvframe * nf = vec_push(p->ctx, &p->cvframes);
      if (!nf) {
        return;
      }
      nf->base = p->values.len;
      nf->tok = s->i;
      nf->open = t;
      s->i++;
      continue;
    }
    s->i++;
    preserved_closer_error(p, t);
    if (keep && !push_token(p, t)) {
      return;
    }
  }
  p->cvframes.len = frames_base;
}

/* 5.5.7 consume a list of component values, onto the value stack. `stop` is
   GTEXT_CSS_TOKEN_EOF for none. */
static void consume_list(parser * p, tstream * s, uint8_t stop, bool nested, bool keep) {
  while (ok(p)) {
    p->ctx->work++;
    const css_tok * t = ts_peek(s);
    if (t->type == GTEXT_CSS_TOKEN_EOF || (stop != GTEXT_CSS_TOKEN_EOF && t->type == stop)) {
      return;
    }
    if (t->type == GTEXT_CSS_TOKEN_CLOSE_CURLY) {
      if (nested) {
        return;
      }
      err_at(p, GTEXT_CSS_ERROR_UNMATCHED_CLOSE_BRACE, t);
      s->i++;
      if (keep && !push_token(p, t)) {
        return;
      }
      continue;
    }
    cv_build(p, s, keep);
  }
}

/* ---- declarations (5.5.6) --------------------------------------------- */

static bool is_custom_name(const GTEXT_CSS_String * name) {
  return name->length >= 2 && name->data[0] == '-' && name->data[1] == '-';
}

static bool is_delim(const css_tok * t, uint32_t cp) {
  return t->type == GTEXT_CSS_TOKEN_DELIM && t->delim == cp;
}

/*
 * Whether "consume a declaration" would return a declaration (before the
 * validity check) for the tokens at s->i, nested or not, looking only at the
 * top level of the value. See the file comment.
 */
static bool spec_decl(parser * p, const tstream * s, bool nested) {
  size_t j = s->i;
  const css_tok * t = ts_at(s, j);
  if (t->type != GTEXT_CSS_TOKEN_IDENT) {
    return false;
  }
  const bool custom = is_custom_name(&t->value);
  j++;
  while (ts_at(s, j)->type == GTEXT_CSS_TOKEN_WHITESPACE) {
    j++;
  }
  if (ts_at(s, j)->type != GTEXT_CSS_TOKEN_COLON) {
    return false;
  }
  if (custom) {
    return true;
  }
  j++;
  size_t nonws = 0;
  bool first_block = false;
  int tail = 0; /* after a leading block: 0 nothing, 1 "!", 2 "! important" */
  for (;;) {
    p->ctx->work++;
    t = ts_at(s, j);
    if (t->type == GTEXT_CSS_TOKEN_EOF || t->type == GTEXT_CSS_TOKEN_SEMICOLON
        || (nested && t->type == GTEXT_CSS_TOKEN_CLOSE_CURLY)) {
      break;
    }
    if (t->type == GTEXT_CSS_TOKEN_WHITESPACE) {
      j++;
      continue;
    }
    const bool block = t->type == GTEXT_CSS_TOKEN_OPEN_CURLY;
    nonws++;
    if (nonws == 1) {
      first_block = block;
    }
    else if (first_block) {
      if (tail == 0 && is_delim(t, '!')) {
        tail = 1;
      }
      else if (tail == 1 && t->type == GTEXT_CSS_TOKEN_IDENT && css_ascii_ieq(&t->value, "important")) {
        tail = 2;
      }
      else {
        return false;
      }
    }
    else if (block) {
      return false;
    }
    if (is_opener(t->type)) {
      const size_t m = s->match[j];
      j = m >= s->n ? s->n : m + 1;
    }
    else {
      j++;
    }
  }
  return !first_block || tail != 1;
}

static GTEXT_CSS_Parent parent_of(parser * p, bool nested);

static bool decl_valid(parser * p, const GTEXT_CSS_Declaration * d, bool nested) {
  if (!p->ctx->validity.declaration) {
    return true;
  }
  const GTEXT_CSS_Parent parent = parent_of(p, nested);
  return p->ctx->validity.declaration(p->ctx->validity.user, &parent, d);
}

/* The value of a unicode-range declaration (5.5.11): bytes `begin` to `end`
   of the input tokenized again with unicode ranges allowed, and consumed as
   a list of component values, onto the value stack. */
static bool unicode_range_value(parser * p, size_t begin, size_t end, size_t line, size_t column) {
  css_ctx * ctx = p->ctx;
  css_tokvec toks = {NULL, 0, 0};
  css_tok eof;
  memset(&eof, 0, sizeof eof);
  if (!css_tokenize(ctx, begin, end, line, column, true, false, &toks, &eof)) {
    css_tokvec_free(ctx, &toks);
    return false;
  }
  eof.type = GTEXT_CSS_TOKEN_EOF;
  tstream s = {toks.data, toks.len, 0, NULL, &eof};
  /* These tokens are the declaration's own, already read once: their parse
     errors have been reported. */
  const bool was = ctx->errors_on;
  ctx->errors_on = false;
  consume_list(p, &s, GTEXT_CSS_TOKEN_EOF, false, true);
  ctx->errors_on = was;
  /* The values point at the arena's strings, not at the vector. */
  css_tokvec_free(ctx, &toks);
  return ok(p);
}

static bool count_declaration(parser * p, const css_tok * at) {
  if (p->ctx->declaration_count >= p->ctx->max_declarations) {
    css_fail_at(p->ctx, GTEXT_CSS_E_LIMIT_DECLARATIONS, at->start, at->line, at->column);
    return false;
  }
  p->ctx->declaration_count++;
  return true;
}

/*
 * Consume a declaration the speculation said is one: the name, the colon, the
 * value, `!important`, the trailing white space, the original text of a
 * custom property and the unicode-range value. Returns false if the
 * declaration turned out not to be one (a defect: the speculation must agree)
 * or the parse failed.
 */
static bool build_decl(parser * p, tstream * s, bool nested, GTEXT_CSS_Declaration * d) {
  memset(d, 0, sizeof *d);
  const css_tok * name = ts_peek(s);
  if (name->type != GTEXT_CSS_TOKEN_IDENT) {
    return false;
  }
  d->name = name->value;
  d->name_span = tok_span(name);
  d->span = tok_span(name);
  s->i++;
  ts_skip_ws(s);
  const css_tok * colon = ts_peek(s);
  if (colon->type != GTEXT_CSS_TOKEN_COLON) {
    return false;
  }
  d->span.end = colon->end;
  s->i++;
  ts_skip_ws(s);
  const size_t first = s->i;
  const size_t base = p->values.len;
  consume_list(p, s, GTEXT_CSS_TOKEN_SEMICOLON, nested, true);
  if (!ok(p)) {
    return false;
  }
  const size_t list_end = s->i;
  const css_tok * stop = ts_at(s, list_end);
  size_t n = p->values.len - base;

  /* The last two values that are not white space: "!" "important". */
  size_t k1 = SIZE_MAX, k2 = SIZE_MAX;
  for (size_t k = n; k-- > 0;) {
    p->ctx->work++;
    if (vstack(p, base + k)->type != GTEXT_CSS_TOKEN_WHITESPACE
        || vstack(p, base + k)->kind != GTEXT_CSS_VALUE_TOKEN) {
      if (k1 == SIZE_MAX) {
        k1 = k;
      }
      else {
        k2 = k;
        break;
      }
    }
  }
  if (k1 != SIZE_MAX) {
    d->span.end = vstack(p, base + k1)->span.end;
  }
  if (k2 != SIZE_MAX) {
    const GTEXT_CSS_Component_Value * bang = vstack(p, base + k2);
    const GTEXT_CSS_Component_Value * imp = vstack(p, base + k1);
    if (bang->kind == GTEXT_CSS_VALUE_TOKEN && bang->type == GTEXT_CSS_TOKEN_DELIM && bang->delim == '!'
        && imp->kind == GTEXT_CSS_VALUE_TOKEN && imp->type == GTEXT_CSS_TOKEN_IDENT
        && css_ascii_ieq(&imp->value, "important")) {
      /* Remove the two; what was between them and after them stays. */
      GTEXT_CSS_Component_Value * a = vstack(p, base);
      memmove(&a[k1], &a[k1 + 1], (n - k1 - 1) * sizeof *a);
      n--;
      memmove(&a[k2], &a[k2 + 1], (n - k2 - 1) * sizeof *a);
      n--;
      p->values.len = base + n;
      d->important = true;
    }
  }
  while (n > 0 && vstack(p, base + n - 1)->kind == GTEXT_CSS_VALUE_TOKEN
      && vstack(p, base + n - 1)->type == GTEXT_CSS_TOKEN_WHITESPACE) {
    n--;
    p->values.len--;
  }

  if (is_custom_name(&d->name)) {
    if (n > 0) {
      if (!css_filtered_text(p->ctx, vstack(p, base)->span.offset,
              vstack(p, base + n - 1)->span.end, &d->original_text)) {
        return false;
      }
    }
    else {
      char * empty = css_arena_strdup(&p->ctx->arena, "", 0);
      if (!empty) {
        css_fail(p->ctx, GTEXT_CSS_E_OOM);
        return false;
      }
      d->original_text.data = empty;
      d->original_text.length = 0;
    }
  }
  else {
    bool block = false;
    size_t nonws = 0;
    for (size_t k = 0; k < n; k++) {
      p->ctx->work++;
      const GTEXT_CSS_Component_Value * v = vstack(p, base + k);
      if (v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE) {
        continue;
      }
      nonws++;
      if (v->kind == GTEXT_CSS_VALUE_BLOCK && v->type == GTEXT_CSS_TOKEN_OPEN_CURLY) {
        block = true;
      }
    }
    if (block && nonws > 1) {
      p->values.len = base;
      return false;
    }
    if (css_ascii_ieq(&d->name, "unicode-range")) {
      p->values.len = base;
      if (list_end > first) {
        const css_tok * ft = ts_at(s, first);
        if (!unicode_range_value(p, ft->start, ts_at(s, list_end - 1)->end, ft->line, ft->column)) {
          return false;
        }
      }
      n = p->values.len - base;
    }
  }
  if (n > 0) {
    const GTEXT_CSS_Component_Value * fv = vstack(p, base);
    d->value_span = fv->span;
    d->value_span.end = vstack(p, base + n - 1)->span.end;
  }
  else {
    d->value_span = point_span(stop);
  }
  if (!take_values(p, base, &d->value, &d->value_count)) {
    return false;
  }
  return count_declaration(p, name);
}

/* 5.5.6, "consume the remnants of a bad declaration". */
static void remnants(parser * p, tstream * s, bool nested) {
  while (ok(p)) {
    const css_tok * t = ts_peek(s);
    if (t->type == GTEXT_CSS_TOKEN_EOF) {
      return;
    }
    if (t->type == GTEXT_CSS_TOKEN_SEMICOLON) {
      s->i++;
      return;
    }
    if (t->type == GTEXT_CSS_TOKEN_CLOSE_CURLY) {
      if (nested) {
        return;
      }
      s->i++;
      continue;
    }
    cv_build(p, s, false);
  }
}

/* ---- rules ------------------------------------------------------------ */

static rframe * top_frame(parser * p) {
  return (rframe *)vec_at(&p->rframes, p->rframes.len - 1);
}

static GTEXT_CSS_Parent parent_of(parser * p, bool nested) {
  GTEXT_CSS_Parent parent;
  memset(&parent, 0, sizeof parent);
  parent.nested = nested;
  size_t depth = 0;
  p->ctx->work += p->rframes.len;
  for (size_t i = 0; i < p->rframes.len; i++) {
    if (((rframe *)vec_at(&p->rframes, i))->kind == RF_BLOCK) {
      depth++;
    }
  }
  parent.depth = depth;
  rframe * f = top_frame(p);
  if (f->kind == RF_BLOCK) {
    parent.top_level = false;
    parent.kind = f->rule.kind;
    parent.name = f->rule.name;
    parent.prelude = f->rule.prelude;
    parent.prelude_count = f->rule.prelude_count;
  }
  else {
    parent.top_level = true;
  }
  return parent;
}

static bool count_rule(parser * p, const GTEXT_CSS_Span * at) {
  if (p->ctx->rule_count >= p->ctx->max_rules) {
    css_fail_at(p->ctx, GTEXT_CSS_E_LIMIT_RULES, at->offset, at->line, at->column);
    return false;
  }
  p->ctx->rule_count++;
  return true;
}

/* The current run of declarations of the top frame, as an item. */
static bool flush_decls(parser * p) {
  rframe * f = top_frame(p);
  const size_t n = p->decls.len - f->decl_base;
  p->ctx->work += n;
  if (n == 0) {
    return true;
  }
  GTEXT_CSS_Declaration * a = css_arena_alloc(&p->ctx->arena, n * sizeof *a);
  if (!a) {
    css_fail(p->ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  memcpy(a, vec_at(&p->decls, f->decl_base), n * sizeof *a);
  p->decls.len = f->decl_base;
  pitem * it = vec_push(p->ctx, &p->items);
  if (!it) {
    return false;
  }
  it->kind = GTEXT_CSS_ITEM_DECLARATIONS;
  it->decls = a;
  it->ndecls = n;
  return true;
}

/* A rule a consume algorithm returned, handed to the frame it belongs to. */
static void deliver_rule(parser * p, GTEXT_CSS_Rule * rule, bool nested) {
  if (!count_rule(p, &rule->span)) {
    return;
  }
  rframe * f = top_frame(p);
  const bool in_block = f->kind == RF_BLOCK || f->kind == RF_BASE_BLOCK;
  if (p->ctx->validity.rule) {
    const GTEXT_CSS_Parent parent = parent_of(p, nested);
    if (!p->ctx->validity.rule(p->ctx->validity.user, &parent, rule)) {
      css_error(p->ctx, GTEXT_CSS_ERROR_INVALID_RULE, rule->span.offset, rule->span.line, rule->span.column);
      if (rule->kind == GTEXT_CSS_RULE_QUALIFIED && in_block) {
        (void)flush_decls(p); /* the invalid rule error */
      }
      return;
    }
  }
  switch (f->kind) {
  case RF_BASE_DECLS:
    css_error(p->ctx, GTEXT_CSS_ERROR_AT_RULE_IN_DECLARATION_LIST, rule->span.offset, rule->span.line,
        rule->span.column);
    return;
  case RF_BASE_SINGLE:
    f->have_rule = true;
    break;
  default:
    break;
  }
  if (in_block && !flush_decls(p)) {
    return;
  }
  pitem * it = vec_push(p->ctx, &p->items);
  if (!it) {
    return;
  }
  it->kind = GTEXT_CSS_ITEM_RULE;
  it->rule = *rule;
}

static bool push_block_frame(parser * p, const GTEXT_CSS_Rule * rule, bool nested, bool discard,
    const css_tok * open) {
  rframe * f = vec_push(p->ctx, &p->rframes);
  if (!f) {
    return false;
  }
  f->kind = RF_BLOCK;
  f->nested = nested;
  f->discard = discard;
  f->rule = *rule;
  f->rule.has_block = true;
  f->rule.block_span = tok_span(open);
  f->item_base = p->items.len;
  f->decl_base = p->decls.len;
  return true;
}

static void set_prelude_span(GTEXT_CSS_Rule * r, const css_tok * ender) {
  if (r->prelude_count) {
    r->prelude_span = r->prelude[0].span;
    r->prelude_span.end = r->prelude[r->prelude_count - 1].span.end;
  }
  else {
    r->prelude_span = point_span(ender);
  }
}

/* 5.5.2 consume an at-rule, up to its block (which run_rules() reads). */
static void at_rule_start(parser * p, tstream * s, bool nested) {
  const css_tok * kw = ts_peek(s);
  GTEXT_CSS_Rule rule;
  memset(&rule, 0, sizeof rule);
  rule.kind = GTEXT_CSS_RULE_AT;
  rule.name = kw->value;
  rule.span = tok_span(kw);
  s->i++;
  const size_t base = p->values.len;
  while (ok(p)) {
    const css_tok * t = ts_peek(s);
    if (t->type == GTEXT_CSS_TOKEN_SEMICOLON || t->type == GTEXT_CSS_TOKEN_EOF
        || (nested && t->type == GTEXT_CSS_TOKEN_CLOSE_CURLY)) {
      if (!take_values(p, base, &rule.prelude, &rule.prelude_count)) {
        return;
      }
      set_prelude_span(&rule, t);
      if (rule.prelude_count) {
        rule.span.end = rule.prelude_span.end;
      }
      if (t->type == GTEXT_CSS_TOKEN_SEMICOLON) {
        rule.span.end = t->end;
        s->i++;
      }
      deliver_rule(p, &rule, nested);
      return;
    }
    if (t->type == GTEXT_CSS_TOKEN_CLOSE_CURLY) {
      err_at(p, GTEXT_CSS_ERROR_UNMATCHED_CLOSE_BRACE, t);
      s->i++;
      if (!push_token(p, t)) {
        return;
      }
      continue;
    }
    if (t->type == GTEXT_CSS_TOKEN_OPEN_CURLY) {
      if (!take_values(p, base, &rule.prelude, &rule.prelude_count)) {
        return;
      }
      set_prelude_span(&rule, t);
      if (push_block_frame(p, &rule, nested, false, t)) {
        s->i++;
      }
      return;
    }
    cv_build(p, s, true);
  }
}

/* The first two values of a prelude that are not white space: an ident
   beginning "--" and a colon (5.5.3). */
static bool looks_like_custom_property(parser * p, size_t base) {
  size_t seen = 0;
  for (size_t k = base; k < p->values.len; k++) {
    p->ctx->work++;
    const GTEXT_CSS_Component_Value * v = vstack(p, k);
    if (v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE) {
      continue;
    }
    if (seen == 0) {
      if (v->kind != GTEXT_CSS_VALUE_TOKEN || v->type != GTEXT_CSS_TOKEN_IDENT || !is_custom_name(&v->value)) {
        return false;
      }
      seen = 1;
      continue;
    }
    return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_COLON;
  }
  return false;
}

/* 5.5.3 consume a qualified rule, up to its block. `stop` is
   GTEXT_CSS_TOKEN_EOF for none. Returns whether something was started or
   returned (false: "return nothing"). */
static bool qualified_start(parser * p, tstream * s, uint8_t stop, bool nested) {
  const css_tok * first = ts_peek(s);
  GTEXT_CSS_Rule rule;
  memset(&rule, 0, sizeof rule);
  rule.kind = GTEXT_CSS_RULE_QUALIFIED;
  rule.span = tok_span(first);
  const size_t base = p->values.len;
  while (ok(p)) {
    const css_tok * t = ts_peek(s);
    if (t->type == GTEXT_CSS_TOKEN_EOF || (stop != GTEXT_CSS_TOKEN_EOF && t->type == stop)) {
      err_at(p, t->type == GTEXT_CSS_TOKEN_EOF ? GTEXT_CSS_ERROR_EOF_IN_QUALIFIED_RULE
                                               : GTEXT_CSS_ERROR_SEMICOLON_IN_QUALIFIED_RULE, t);
      p->values.len = base;
      return false;
    }
    if (t->type == GTEXT_CSS_TOKEN_CLOSE_CURLY) {
      err_at(p, GTEXT_CSS_ERROR_CLOSE_BRACE_IN_QUALIFIED_RULE, t);
      if (nested) {
        p->values.len = base;
        return false;
      }
      s->i++;
      if (!push_token(p, t)) {
        return false;
      }
      continue;
    }
    if (t->type == GTEXT_CSS_TOKEN_OPEN_CURLY) {
      if (looks_like_custom_property(p, base)) {
        css_error(p->ctx, GTEXT_CSS_ERROR_RULE_LOOKS_LIKE_CUSTOM_PROPERTY, first->start, first->line,
            first->column);
        p->values.len = base;
        if (nested) {
          remnants(p, s, true);
          return false;
        }
        if (push_block_frame(p, &rule, nested, true, t)) {
          s->i++;
        }
        return false;
      }
      if (!take_values(p, base, &rule.prelude, &rule.prelude_count)) {
        return false;
      }
      set_prelude_span(&rule, t);
      if (push_block_frame(p, &rule, nested, false, t)) {
        s->i++;
      }
      return true;
    }
    cv_build(p, s, true);
  }
  return false;
}

/* The block of the top frame has ended at `t` (`}` or EOF): materialize the
   rule (5.5.3, 5.5.2) and hand it on. */
static void finish_block(parser * p, tstream * s, const css_tok * t) {
  if (!flush_decls(p)) {
    return;
  }
  rframe f = *top_frame(p);
  p->rframes.len--;
  if (t->type == GTEXT_CSS_TOKEN_CLOSE_CURLY) {
    f.rule.block_span.end = t->end;
    s->i++;
  }
  else {
    f.rule.block_span.end = t->start;
    f.rule.unclosed = true;
  }
  f.rule.span.end = f.rule.block_span.end;
  const size_t nitems = p->items.len - f.item_base;
  p->ctx->work += nitems;
  pitem * items = (pitem *)vec_at(&p->items, f.item_base);
  if (f.discard) {
    p->items.len = f.item_base;
    return;
  }
  GTEXT_CSS_Rule * rule = &f.rule;
  if (rule->kind == GTEXT_CSS_RULE_QUALIFIED) {
    size_t from = 0;
    if (nitems > 0 && items[0].kind == GTEXT_CSS_ITEM_DECLARATIONS) {
      rule->declarations = items[0].decls;
      rule->declaration_count = items[0].ndecls;
      from = 1;
    }
    const size_t nrules = nitems - from;
    if (nrules) {
      GTEXT_CSS_Rule * rules = css_arena_alloc(&p->ctx->arena, nrules * sizeof *rules);
      if (!rules) {
        css_fail(p->ctx, GTEXT_CSS_E_OOM);
        return;
      }
      for (size_t k = 0; k < nrules; k++) {
        pitem * it = &items[from + k];
        if (it->kind == GTEXT_CSS_ITEM_RULE) {
          rules[k] = it->rule;
          continue;
        }
        GTEXT_CSS_Rule * nd = &rules[k];
        memset(nd, 0, sizeof *nd);
        nd->kind = GTEXT_CSS_RULE_NESTED_DECLARATIONS;
        nd->declarations = it->decls;
        nd->declaration_count = it->ndecls;
        nd->span = it->decls[0].span;
        nd->span.end = it->decls[it->ndecls - 1].span.end;
        if (!count_rule(p, &nd->span)) {
          return;
        }
      }
      rule->rules = rules;
      rule->rule_count = nrules;
    }
  }
  else {
    size_t nrules = 0;
    for (size_t k = 0; k < nitems; k++) {
      nrules += items[k].kind == GTEXT_CSS_ITEM_RULE;
    }
    GTEXT_CSS_Rule * rules = NULL;
    if (nrules) {
      rules = css_arena_alloc(&p->ctx->arena, nrules * sizeof *rules);
      if (!rules) {
        css_fail(p->ctx, GTEXT_CSS_E_OOM);
        return;
      }
    }
    GTEXT_CSS_Block_Item * out = NULL;
    if (nitems) {
      out = css_arena_alloc(&p->ctx->arena, nitems * sizeof *out);
      if (!out) {
        css_fail(p->ctx, GTEXT_CSS_E_OOM);
        return;
      }
    }
    size_t r = 0;
    for (size_t k = 0; k < nitems; k++) {
      memset(&out[k], 0, sizeof out[k]);
      out[k].kind = (GTEXT_CSS_Item_Kind)items[k].kind;
      if (items[k].kind == GTEXT_CSS_ITEM_RULE) {
        rules[r] = items[k].rule;
        out[k].rule = &rules[r];
        r++;
      }
      else {
        out[k].declarations = items[k].decls;
        out[k].declaration_count = items[k].ndecls;
      }
    }
    rule->contents = out;
    rule->content_count = nitems;
  }
  p->items.len = f.item_base;
  deliver_rule(p, rule, f.nested);
}

/* A declaration a consume algorithm returned, handed to the top frame. */
static bool deliver_decl(parser * p, const GTEXT_CSS_Declaration * d) {
  GTEXT_CSS_Declaration * slot = vec_push(p->ctx, &p->decls);
  if (!slot) {
    return false;
  }
  *slot = *d;
  return true;
}

/* One item of "consume a block's contents" (5.5.5). Returns false at the
   end of the block. */
static bool block_step(parser * p, tstream * s) {
  p->ctx->work++;
  rframe * f = top_frame(p);
  const css_tok * t = ts_peek(s);
  switch (t->type) {
  case GTEXT_CSS_TOKEN_WHITESPACE:
  case GTEXT_CSS_TOKEN_SEMICOLON:
    s->i++;
    return true;
  case GTEXT_CSS_TOKEN_EOF:
  case GTEXT_CSS_TOKEN_CLOSE_CURLY:
    if (f->kind == RF_BASE_BLOCK) {
      return false;
    }
    finish_block(p, s, t);
    return true;
  case GTEXT_CSS_TOKEN_AT_KEYWORD:
    if (flush_decls(p)) {
      at_rule_start(p, s, true);
    }
    return true;
  default:
    break;
  }
  const size_t mark = s->i;
  if (spec_decl(p, s, true)) {
    GTEXT_CSS_Declaration d;
    if (!build_decl(p, s, true, &d)) {
      if (ok(p)) {
        css_fail(p->ctx, GTEXT_CSS_E_INTERNAL);
      }
      return true;
    }
    if (decl_valid(p, &d, true)) {
      (void)deliver_decl(p, &d);
      return true;
    }
    css_error(p->ctx, GTEXT_CSS_ERROR_INVALID_DECLARATION_IN_CONTEXT, d.span.offset, d.span.line, d.span.column);
    s->i = mark;
  }
  (void)qualified_start(p, s, GTEXT_CSS_TOKEN_SEMICOLON, true);
  return true;
}

/* "Consume a declaration" in a context that is not nested: what parse a list
   of declarations and parse a declaration call. Returns whether one was
   returned, into *d. */
static bool declaration_flat(parser * p, tstream * s, GTEXT_CSS_Declaration * d) {
  const css_tok * start = ts_peek(s);
  if (spec_decl(p, s, false)) {
    if (!build_decl(p, s, false, d)) {
      if (ok(p)) {
        css_fail(p->ctx, GTEXT_CSS_E_INTERNAL);
      }
      return false;
    }
    if (decl_valid(p, d, false)) {
      return true;
    }
    css_error(p->ctx, GTEXT_CSS_ERROR_INVALID_DECLARATION_IN_CONTEXT, d->span.offset, d->span.line,
        d->span.column);
    return false;
  }
  css_error(p->ctx, GTEXT_CSS_ERROR_INVALID_DECLARATION, start->start, start->line, start->column);
  remnants(p, s, false);
  return false;
}

/* The loop over the rule frames. */
static void run_rules(parser * p, tstream * s, int base_kind) {
  rframe * base = vec_push(p->ctx, &p->rframes);
  if (!base) {
    return;
  }
  base->kind = base_kind;
  while (ok(p)) {
    rframe * f = top_frame(p);
    if (f->kind == RF_BLOCK || f->kind == RF_BASE_BLOCK) {
      if (!block_step(p, s)) {
        break;
      }
      continue;
    }
    const css_tok * t = ts_peek(s);
    if (f->kind == RF_BASE_SHEET) {
      if (t->type == GTEXT_CSS_TOKEN_EOF) {
        break;
      }
      if (t->type == GTEXT_CSS_TOKEN_WHITESPACE || t->type == GTEXT_CSS_TOKEN_CDO
          || t->type == GTEXT_CSS_TOKEN_CDC) {
        s->i++;
      }
      else if (t->type == GTEXT_CSS_TOKEN_AT_KEYWORD) {
        at_rule_start(p, s, false);
      }
      else {
        (void)qualified_start(p, s, GTEXT_CSS_TOKEN_EOF, false);
      }
      continue;
    }
    if (f->kind == RF_BASE_DECLS) {
      ts_skip_ws(s);
      t = ts_peek(s);
      if (t->type == GTEXT_CSS_TOKEN_EOF) {
        break;
      }
      if (t->type == GTEXT_CSS_TOKEN_SEMICOLON) {
        s->i++;
      }
      else if (t->type == GTEXT_CSS_TOKEN_AT_KEYWORD) {
        at_rule_start(p, s, false);
      }
      else {
        GTEXT_CSS_Declaration d;
        if (declaration_flat(p, s, &d)) {
          (void)deliver_decl(p, &d);
        }
      }
      continue;
    }
    /* RF_BASE_SINGLE: parse a rule (5.4.6) */
    if (f->phase == 1) {
      break;
    }
    f->phase = 1;
    if (t->type == GTEXT_CSS_TOKEN_AT_KEYWORD) {
      at_rule_start(p, s, false);
    }
    else {
      (void)qualified_start(p, s, GTEXT_CSS_TOKEN_EOF, false);
    }
  }
}

/* ---- identities ------------------------------------------------------- */

enum { W_RULES, W_DECLS, W_ITEMS, W_VALUES, W_LISTS };

/// A step of the identity walk: an array and how far into it.
typedef struct {
  int kind;   ///< W_*.
  void * ptr; ///< The array.
  size_t n;   ///< Its length.
  size_t i;   ///< The next element.
} walk;

static bool walk_push(parser * p, vec * st, int kind, const void * ptr, size_t n) {
  if (n == 0) {
    return true;
  }
  walk * w = vec_push(p->ctx, st);
  if (!w) {
    return false;
  }
  w->kind = kind;
  w->ptr = (void *)(uintptr_t)ptr;
  w->n = n;
  w->i = 0;
  return true;
}

static bool walk_rule(parser * p, vec * st, GTEXT_CSS_Rule * r, GTEXT_CSS_Id * next) {
  r->id = (*next)++;
  /* pushed in reverse of the order they are visited in */
  if (r->kind == GTEXT_CSS_RULE_AT) {
    if (!walk_push(p, st, W_ITEMS, r->contents, r->content_count)) {
      return false;
    }
  }
  else if (!walk_push(p, st, W_RULES, r->rules, r->rule_count)) {
    return false;
  }
  if (!walk_push(p, st, W_DECLS, r->declarations, r->declaration_count)) {
    return false;
  }
  return walk_push(p, st, W_VALUES, r->prelude, r->prelude_count);
}

/* Give every node an id, from 1, in document order, without recursion. */
static size_t assign_ids(parser * p, GTEXT_CSS_Tree * tree) {
  vec st;
  vec_init(&st, sizeof(walk));
  GTEXT_CSS_Id next = 1;
  /* the tree's own lists, pushed in reverse (only one is ever non-empty) */
  if (!walk_push(p, &st, W_LISTS, tree->lists, tree->list_count)
      || !walk_push(p, &st, W_VALUES, tree->values, tree->value_count)
      || !walk_push(p, &st, W_ITEMS, tree->items, tree->item_count)
      || !walk_push(p, &st, W_DECLS, tree->declarations, tree->declaration_count)
      || !walk_push(p, &st, W_RULES, tree->rules, tree->rule_count)) {
    vec_free(p->ctx, &st);
    return 0;
  }
  while (st.len > 0 && ok(p)) {
    p->ctx->work++;
    walk * w = (walk *)vec_at(&st, st.len - 1);
    if (w->i == w->n) {
      st.len--;
      continue;
    }
    const size_t i = w->i++;
    const int kind = w->kind;
    void * ptr = w->ptr;
    switch (kind) {
    case W_RULES:
      (void)walk_rule(p, &st, &((GTEXT_CSS_Rule *)ptr)[i], &next);
      break;
    case W_DECLS: {
      GTEXT_CSS_Declaration * d = &((GTEXT_CSS_Declaration *)ptr)[i];
      d->id = next++;
      (void)walk_push(p, &st, W_VALUES, d->value, d->value_count);
      break;
    }
    case W_ITEMS: {
      GTEXT_CSS_Block_Item * it = &((GTEXT_CSS_Block_Item *)ptr)[i];
      if (it->kind == GTEXT_CSS_ITEM_RULE) {
        (void)walk_rule(p, &st, (GTEXT_CSS_Rule *)(uintptr_t)it->rule, &next);
      }
      else {
        (void)walk_push(p, &st, W_DECLS, it->declarations, it->declaration_count);
      }
      break;
    }
    case W_VALUES: {
      GTEXT_CSS_Component_Value * v = &((GTEXT_CSS_Component_Value *)ptr)[i];
      v->id = next++;
      (void)walk_push(p, &st, W_VALUES, v->children, v->child_count);
      break;
    }
    default: {
      GTEXT_CSS_Value_List * l = &((GTEXT_CSS_Value_List *)ptr)[i];
      (void)walk_push(p, &st, W_VALUES, l->values, l->count);
      break;
    }
    }
  }
  vec_free(p->ctx, &st);
  return (size_t)(next - 1);
}

/* ---- entry points ----------------------------------------------------- */

/// The entry points.
typedef enum {
  E_STYLESHEET,
  E_STYLESHEET_CONTENTS,
  E_DECLARATION_LIST,
  E_BLOCK_CONTENTS,
  E_RULE,
  E_DECLARATION,
  E_COMPONENT_VALUE,
  E_COMPONENT_VALUES,
  E_COMMA_LIST
} entry;

static GTEXT_CSS_Tree_Kind tree_kind(entry e) {
  switch (e) {
  case E_STYLESHEET: return GTEXT_CSS_TREE_STYLESHEET;
  case E_STYLESHEET_CONTENTS: return GTEXT_CSS_TREE_STYLESHEET_CONTENTS;
  case E_DECLARATION_LIST: return GTEXT_CSS_TREE_DECLARATION_LIST;
  case E_BLOCK_CONTENTS: return GTEXT_CSS_TREE_BLOCK_CONTENTS;
  case E_RULE: return GTEXT_CSS_TREE_RULE;
  case E_DECLARATION: return GTEXT_CSS_TREE_DECLARATION;
  case E_COMPONENT_VALUE: return GTEXT_CSS_TREE_COMPONENT_VALUE;
  case E_COMPONENT_VALUES: return GTEXT_CSS_TREE_COMPONENT_VALUE_LIST;
  default: return GTEXT_CSS_TREE_COMMA_LIST;
  }
}

/* Copy the items of the base frame out as the tree's rules (for a stylesheet
   and a single rule) or items (for a block's contents). */
static bool base_rules(parser * p, GTEXT_CSS_Tree * tree) {
  const size_t n = p->items.len;
  if (n == 0) {
    return true;
  }
  GTEXT_CSS_Rule * rules = css_arena_alloc(&p->ctx->arena, n * sizeof *rules);
  if (!rules) {
    css_fail(p->ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  for (size_t k = 0; k < n; k++) {
    rules[k] = ((pitem *)vec_at(&p->items, k))->rule;
  }
  tree->rules = rules;
  tree->rule_count = n;
  return true;
}

static bool base_items(parser * p, GTEXT_CSS_Tree * tree) {
  if (!flush_decls(p)) {
    return false;
  }
  const size_t n = p->items.len;
  if (n == 0) {
    return true;
  }
  size_t nrules = 0;
  for (size_t k = 0; k < n; k++) {
    nrules += ((pitem *)vec_at(&p->items, k))->kind == GTEXT_CSS_ITEM_RULE;
  }
  GTEXT_CSS_Rule * rules = nrules ? css_arena_alloc(&p->ctx->arena, nrules * sizeof *rules) : NULL;
  GTEXT_CSS_Block_Item * out = css_arena_alloc(&p->ctx->arena, n * sizeof *out);
  if ((nrules && !rules) || !out) {
    css_fail(p->ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  size_t r = 0;
  for (size_t k = 0; k < n; k++) {
    const pitem * it = (pitem *)vec_at(&p->items, k);
    memset(&out[k], 0, sizeof out[k]);
    out[k].kind = (GTEXT_CSS_Item_Kind)it->kind;
    if (it->kind == GTEXT_CSS_ITEM_RULE) {
      rules[r] = it->rule;
      out[k].rule = &rules[r++];
    }
    else {
      out[k].declarations = it->decls;
      out[k].declaration_count = it->ndecls;
    }
  }
  tree->items = out;
  tree->item_count = n;
  return true;
}

static bool base_decls(parser * p, GTEXT_CSS_Tree * tree) {
  const size_t n = p->decls.len;
  if (n == 0) {
    return true;
  }
  GTEXT_CSS_Declaration * a = css_arena_alloc(&p->ctx->arena, n * sizeof *a);
  if (!a) {
    css_fail(p->ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  memcpy(a, p->decls.data, n * sizeof *a);
  tree->declarations = a;
  tree->declaration_count = n;
  return true;
}

/* The parse proper, after tokenization. Fills the tree's lists. */
static void parse_entry(parser * p, entry e, GTEXT_CSS_Tree * tree) {
  tstream * s = &p->ts;
  switch (e) {
  case E_STYLESHEET:
  case E_STYLESHEET_CONTENTS:
    run_rules(p, s, RF_BASE_SHEET);
    if (ok(p)) {
      (void)base_rules(p, tree);
    }
    return;
  case E_DECLARATION_LIST:
    run_rules(p, s, RF_BASE_DECLS);
    if (ok(p)) {
      (void)base_decls(p, tree);
    }
    return;
  case E_BLOCK_CONTENTS:
    run_rules(p, s, RF_BASE_BLOCK);
    if (ok(p)) {
      (void)base_items(p, tree);
    }
    return;
  case E_RULE: {
    ts_skip_ws(s);
    if (ts_peek(s)->type == GTEXT_CSS_TOKEN_EOF) {
      tree->syntax_error = GTEXT_CSS_SYNTAX_EMPTY;
      return;
    }
    run_rules(p, s, RF_BASE_SINGLE);
    if (!ok(p)) {
      return;
    }
    const rframe * f = (const rframe *)vec_at(&p->rframes, 0);
    if (!f->have_rule || p->items.len != 1) {
      tree->syntax_error = GTEXT_CSS_SYNTAX_INVALID;
      return;
    }
    ts_skip_ws(s);
    if (ts_peek(s)->type != GTEXT_CSS_TOKEN_EOF) {
      tree->syntax_error = GTEXT_CSS_SYNTAX_EXTRA_INPUT;
      return;
    }
    (void)base_rules(p, tree);
    return;
  }
  case E_DECLARATION: {
    ts_skip_ws(s);
    if (ts_peek(s)->type == GTEXT_CSS_TOKEN_EOF) {
      tree->syntax_error = GTEXT_CSS_SYNTAX_EMPTY;
      return;
    }
    rframe * base = vec_push(p->ctx, &p->rframes);
    if (!base) {
      return;
    }
    base->kind = RF_BASE_DECLS;
    GTEXT_CSS_Declaration d;
    if (!declaration_flat(p, s, &d)) {
      if (ok(p)) {
        tree->syntax_error = GTEXT_CSS_SYNTAX_INVALID;
      }
      return;
    }
    ts_skip_ws(s);
    if (ts_peek(s)->type != GTEXT_CSS_TOKEN_EOF) {
      tree->syntax_error = GTEXT_CSS_SYNTAX_EXTRA_INPUT;
      return;
    }
    if (deliver_decl(p, &d)) {
      (void)base_decls(p, tree);
    }
    return;
  }
  case E_COMPONENT_VALUE: {
    ts_skip_ws(s);
    if (ts_peek(s)->type == GTEXT_CSS_TOKEN_EOF) {
      tree->syntax_error = GTEXT_CSS_SYNTAX_EMPTY;
      return;
    }
    cv_build(p, s, true);
    if (!ok(p)) {
      return;
    }
    ts_skip_ws(s);
    if (ts_peek(s)->type != GTEXT_CSS_TOKEN_EOF) {
      tree->syntax_error = GTEXT_CSS_SYNTAX_EXTRA_INPUT;
      p->values.len = 0;
      return;
    }
    (void)take_values(p, 0, &tree->values, &tree->value_count);
    return;
  }
  case E_COMPONENT_VALUES:
    consume_list(p, s, GTEXT_CSS_TOKEN_EOF, false, true);
    if (ok(p)) {
      (void)take_values(p, 0, &tree->values, &tree->value_count);
    }
    return;
  case E_COMMA_LIST: {
    vec lists;
    vec_init(&lists, sizeof(GTEXT_CSS_Value_List));
    while (ok(p) && ts_peek(s)->type != GTEXT_CSS_TOKEN_EOF) {
      const css_tok * at = ts_peek(s);
      consume_list(p, s, GTEXT_CSS_TOKEN_COMMA, false, true);
      GTEXT_CSS_Value_List * l = vec_push(p->ctx, &lists);
      if (!l) {
        break;
      }
      if (!take_values(p, 0, &l->values, &l->count)) {
        break;
      }
      if (l->count) {
        l->span = l->values[0].span;
        l->span.end = l->values[l->count - 1].span.end;
      }
      else {
        l->span = point_span(at);
      }
      ts_discard(s);
    }
    if (ok(p) && lists.len) {
      GTEXT_CSS_Value_List * a = css_arena_alloc(&p->ctx->arena, lists.len * sizeof *a);
      if (!a) {
        css_fail(p->ctx, GTEXT_CSS_E_OOM);
      }
      else {
        memcpy(a, lists.data, lists.len * sizeof *a);
        tree->lists = a;
        tree->list_count = lists.len;
      }
    }
    vec_free(p->ctx, &lists);
    return;
  }
  }
}

/* Where the byte `offset` is: its line and column. */
static void position_of(css_ctx * ctx, size_t offset, size_t * line, size_t * column) {
  size_t l = 1, c = 1;
  size_t p = 0;
  while (p < offset && p < ctx->in_len) {
    const unsigned char b = ctx->in[p];
    if (b == '\n' || b == 0x0C) {
      l++;
      c = 1;
      p++;
    }
    else if (b == '\r') {
      l++;
      c = 1;
      p += (p + 1 < ctx->in_len && ctx->in[p + 1] == '\n') ? 2 : 1;
    }
    else if (b < 0x80 || b >= 0xC0) {
      c += b >= 0xF0 ? 2 : 1;
      p++;
    }
    else {
      p++; /* a continuation byte: counted with its lead */
    }
  }
  *line = l;
  *column = c;
}

static GTEXT_CSS_Status parse(entry e, const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Tree ** out,
    GTEXT_CSS_Result * result, const GTEXT_CSS_Grammar * grammar, bool * matched,
    size_t * matched_count, size_t * work) {
  if (result) {
    memset(result, 0, sizeof *result);
  }
  if (!out || (!input && length > 0)) {
    if (result) {
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
    }
    return GTEXT_CSS_E_INVALID;
  }
  css_ctx ctx;
  css_ctx_init(&ctx, options, result != NULL);
  ctx.in = (const unsigned char *)(input ? input : "");
  ctx.in_len = length;
  parser p;
  memset(&p, 0, sizeof p);
  p.ctx = &ctx;
  vec_init(&p.values, sizeof(GTEXT_CSS_Component_Value));
  vec_init(&p.cvframes, sizeof(cvframe));
  vec_init(&p.decls, sizeof(GTEXT_CSS_Declaration));
  vec_init(&p.items, sizeof(pitem));
  vec_init(&p.rframes, sizeof(rframe));

  GTEXT_CSS_Tree * tree = NULL;
  if (length > ctx.max_input) {
    size_t line, column;
    position_of(&ctx, ctx.max_input, &line, &column);
    css_fail_at(&ctx, GTEXT_CSS_E_LIMIT_INPUT, ctx.max_input, line, column);
  }
  else if (css_tokenize(&ctx, 0, length, 1, 1, false, true, &ctx.toks, &ctx.eof)) {
    ctx.eof.type = GTEXT_CSS_TOKEN_EOF;
    p.ts.toks = ctx.toks.data;
    p.ts.n = ctx.toks.len;
    p.ts.i = 0;
    p.ts.match = ctx.match;
    p.ts.eof = &ctx.eof;
    tree = css_arena_alloc(&ctx.arena, sizeof *tree);
    struct GTEXT_CSS_Tree_Internal * internal = css_arena_alloc(&ctx.arena, sizeof *internal);
    if (!tree || !internal) {
      css_fail(&ctx, GTEXT_CSS_E_OOM);
    }
    else {
      memset(tree, 0, sizeof *tree);
      tree->internal = internal;
      tree->kind = tree_kind(e);
      if (e == E_STYLESHEET && options && options->location) {
        char * loc = css_arena_strdup(&ctx.arena, options->location, strlen(options->location));
        if (!loc) {
          css_fail(&ctx, GTEXT_CSS_E_OOM);
        }
        tree->location = loc;
      }
      if (ok(&p)) {
        parse_entry(&p, e, tree);
      }
      if (ok(&p)) {
        tree->node_count = assign_ids(&p, tree);
      }
    }
  }
  vec_free(&ctx, &p.values);
  vec_free(&ctx, &p.cvframes);
  vec_free(&ctx, &p.decls);
  vec_free(&ctx, &p.items);
  vec_free(&ctx, &p.rframes);

  GTEXT_CSS_Status status = ctx.status;
  if (status == GTEXT_CSS_OK && grammar) {
    if (matched) {
      *matched = grammar->match(grammar->user, 0, tree->values, tree->value_count);
    }
    if (matched_count) {
      size_t m = 0;
      for (size_t i = 0; i < tree->list_count; i++) {
        m += grammar->match(grammar->user, i, tree->lists[i].values, tree->lists[i].count) ? 1 : 0;
      }
      *matched_count = m;
    }
  }
  status = css_finish_result(&ctx, result, length > ctx.max_input ? 0 : length);
  if (work) {
    *work = ctx.work;
  }
  if (status == GTEXT_CSS_OK) {
    tree->internal->arena = ctx.arena;
    *out = tree;
    css_ctx_free(&ctx, true);
  }
  else {
    css_ctx_free(&ctx, false);
  }
  return status;
}

GTEXT_CSS_Status gtext_css_parse_stylesheet(const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  return parse(E_STYLESHEET, input, length, options, out, result, NULL, NULL, NULL, NULL);
}

GTEXT_CSS_Status gtext_css_parse_stylesheet_contents(const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  return parse(E_STYLESHEET_CONTENTS, input, length, options, out, result, NULL, NULL, NULL, NULL);
}

GTEXT_CSS_Status gtext_css_parse_declaration_list(const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  return parse(E_DECLARATION_LIST, input, length, options, out, result, NULL, NULL, NULL, NULL);
}

GTEXT_CSS_Status gtext_css_parse_block_contents(const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  return parse(E_BLOCK_CONTENTS, input, length, options, out, result, NULL, NULL, NULL, NULL);
}

GTEXT_CSS_Status gtext_css_parse_rule(const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  return parse(E_RULE, input, length, options, out, result, NULL, NULL, NULL, NULL);
}

GTEXT_CSS_Status gtext_css_parse_declaration(const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  return parse(E_DECLARATION, input, length, options, out, result, NULL, NULL, NULL, NULL);
}

GTEXT_CSS_Status gtext_css_parse_component_value(const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  return parse(E_COMPONENT_VALUE, input, length, options, out, result, NULL, NULL, NULL, NULL);
}

GTEXT_CSS_Status gtext_css_parse_component_values(const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  return parse(E_COMPONENT_VALUES, input, length, options, out, result, NULL, NULL, NULL, NULL);
}

GTEXT_CSS_Status gtext_css_parse_comma_list(const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  return parse(E_COMMA_LIST, input, length, options, out, result, NULL, NULL, NULL, NULL);
}

static bool grammar_unusable(const GTEXT_CSS_Grammar * g, GTEXT_CSS_Result * result) {
  if (g && g->match) {
    return false;
  }
  if (result) {
    memset(result, 0, sizeof *result);
    result->status = GTEXT_CSS_E_INVALID;
    result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
  }
  return true;
}

GTEXT_CSS_Status gtext_css_parse_grammar(const char * input, size_t length,
    const GTEXT_CSS_Grammar * grammar, const GTEXT_CSS_Parse_Options * options,
    bool * matched, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  if (grammar_unusable(grammar, result) || !matched) {
    if (result && grammar && grammar->match) {
      memset(result, 0, sizeof *result);
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
    }
    return GTEXT_CSS_E_INVALID;
  }
  return parse(E_COMPONENT_VALUES, input, length, options, out, result, grammar, matched, NULL, NULL);
}

GTEXT_CSS_Status gtext_css_parse_grammar_list(const char * input, size_t length,
    const GTEXT_CSS_Grammar * grammar, const GTEXT_CSS_Parse_Options * options,
    size_t * matched_count, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  if (grammar_unusable(grammar, result) || !matched_count) {
    if (result && grammar && grammar->match) {
      memset(result, 0, sizeof *result);
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
    }
    return GTEXT_CSS_E_INVALID;
  }
  /* "If input contains only whitespace-tokens, return an empty list": a
     comma-separated list of such an input would be one empty group, so the
     list is parsed and an all-white-space input's groups are dropped. */
  GTEXT_CSS_Status st = parse(E_COMMA_LIST, input, length, options, out, result, NULL, NULL, NULL, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Tree * tree = *out;
  bool only_ws = true;
  for (size_t i = 0; i < tree->list_count && only_ws; i++) {
    for (size_t k = 0; k < tree->lists[i].count; k++) {
      if (tree->lists[i].values[k].type != GTEXT_CSS_TOKEN_WHITESPACE
          || tree->lists[i].values[k].kind != GTEXT_CSS_VALUE_TOKEN) {
        only_ws = false;
        break;
      }
    }
  }
  if (tree->list_count > 1) {
    only_ws = false; /* a comma is not white space */
  }
  if (only_ws) {
    tree->lists = NULL;
    tree->list_count = 0;
    tree->node_count = 0;
    *matched_count = 0;
    return GTEXT_CSS_OK;
  }
  size_t m = 0;
  for (size_t i = 0; i < tree->list_count; i++) {
    m += grammar->match(grammar->user, i, tree->lists[i].values, tree->lists[i].count) ? 1 : 0;
  }
  *matched_count = m;
  return GTEXT_CSS_OK;
}

bool gtext_css_match_grammar(const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Grammar * grammar) {
  if (!grammar || !grammar->match || (!values && count > 0)) {
    return false;
  }
  return grammar->match(grammar->user, 0, values, count);
}

size_t gtext_css_match_grammar_list(const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Grammar * grammar, size_t * group_count) {
  if (group_count) {
    *group_count = 0;
  }
  if (!grammar || !grammar->match || (!values && count > 0)) {
    return 0;
  }
  bool only_ws = true;
  for (size_t k = 0; k < count; k++) {
    if (values[k].kind != GTEXT_CSS_VALUE_TOKEN || values[k].type != GTEXT_CSS_TOKEN_WHITESPACE) {
      only_ws = false;
      break;
    }
  }
  if (only_ws) {
    return 0;
  }
  size_t groups = 0, matched = 0, start = 0;
  for (size_t k = 0; k <= count; k++) {
    if (k == count || (values[k].kind == GTEXT_CSS_VALUE_TOKEN && values[k].type == GTEXT_CSS_TOKEN_COMMA)) {
      if (k == count && start == count && groups > 0) {
        break; /* a trailing comma ends the list, as 5.4.10's loop does */
      }
      matched += grammar->match(grammar->user, groups, values + start, k - start) ? 1 : 0;
      groups++;
      start = k + 1;
    }
  }
  if (group_count) {
    *group_count = groups;
  }
  return matched;
}

/** The quadratic suite's view of a parse (css_internal.h). */
GTEXT_CSS_Status gtext_css_parse_work(GTEXT_CSS_Tree_Kind kind, const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, size_t * work) {
  entry e;
  switch (kind) {
  case GTEXT_CSS_TREE_STYLESHEET: e = E_STYLESHEET; break;
  case GTEXT_CSS_TREE_STYLESHEET_CONTENTS: e = E_STYLESHEET_CONTENTS; break;
  case GTEXT_CSS_TREE_DECLARATION_LIST: e = E_DECLARATION_LIST; break;
  case GTEXT_CSS_TREE_BLOCK_CONTENTS: e = E_BLOCK_CONTENTS; break;
  case GTEXT_CSS_TREE_RULE: e = E_RULE; break;
  case GTEXT_CSS_TREE_DECLARATION: e = E_DECLARATION; break;
  case GTEXT_CSS_TREE_COMPONENT_VALUE: e = E_COMPONENT_VALUE; break;
  case GTEXT_CSS_TREE_COMPONENT_VALUE_LIST: e = E_COMPONENT_VALUES; break;
  default: e = E_COMMA_LIST; break;
  }
  GTEXT_CSS_Tree * tree = NULL;
  size_t w = 0;
  const GTEXT_CSS_Status st = parse(e, input, length, options, &tree, NULL, NULL, NULL, NULL, &w);
  gtext_css_tree_free(st == GTEXT_CSS_OK ? tree : NULL);
  if (work) {
    *work = w;
  }
  return st;
}
