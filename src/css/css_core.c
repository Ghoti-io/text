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
 * The statuses, names, default options, result and error list of the CSS
 * module, its arena, and the parse context the tokenizer and the parser
 * share.
 */

#include <ghoti.io/text/macros.h>

#include <stdint.h>
#include <string.h>

#include "../text_number_internal.h"
#include "css_internal.h"

/* ---- names ------------------------------------------------------------ */

static const char * const status_names[GTEXT_CSS_STATUS_COUNT] = {
  [GTEXT_CSS_OK] = "success",
  [GTEXT_CSS_E_INVALID] = "invalid argument",
  [GTEXT_CSS_E_OOM] = "out of memory",
  [GTEXT_CSS_E_INTERNAL] = "internal error",
  [GTEXT_CSS_E_UNSUPPORTED] = "not supported",
  [GTEXT_CSS_E_LIMIT_INPUT] = "input longer than max_input_bytes",
  [GTEXT_CSS_E_LIMIT_TOKENS] = "more tokens than max_tokens",
  [GTEXT_CSS_E_LIMIT_DEPTH] = "blocks and functions nested deeper than max_depth",
  [GTEXT_CSS_E_LIMIT_RULES] = "more rules than max_rules",
  [GTEXT_CSS_E_LIMIT_DECLARATIONS] = "more declarations than max_declarations",
  [GTEXT_CSS_E_LIMIT_ERRORS] = "more parse errors than max_total_errors",
  [GTEXT_CSS_E_WRITE] = "the sink refused output",
  [GTEXT_CSS_E_LIMIT_OUTPUT] = "output longer than max_output_bytes",
};

const char * gtext_css_status_string(GTEXT_CSS_Status status) {
  if ((unsigned)status >= (unsigned)GTEXT_CSS_STATUS_COUNT) {
    return "unknown status";
  }
  return status_names[status];
}

static const char * const token_names[GTEXT_CSS_TOKEN_TYPE_COUNT] = {
  [GTEXT_CSS_TOKEN_IDENT] = "ident",
  [GTEXT_CSS_TOKEN_FUNCTION] = "function",
  [GTEXT_CSS_TOKEN_AT_KEYWORD] = "at-keyword",
  [GTEXT_CSS_TOKEN_HASH] = "hash",
  [GTEXT_CSS_TOKEN_STRING] = "string",
  [GTEXT_CSS_TOKEN_BAD_STRING] = "bad-string",
  [GTEXT_CSS_TOKEN_URL] = "url",
  [GTEXT_CSS_TOKEN_BAD_URL] = "bad-url",
  [GTEXT_CSS_TOKEN_DELIM] = "delim",
  [GTEXT_CSS_TOKEN_NUMBER] = "number",
  [GTEXT_CSS_TOKEN_PERCENTAGE] = "percentage",
  [GTEXT_CSS_TOKEN_DIMENSION] = "dimension",
  [GTEXT_CSS_TOKEN_UNICODE_RANGE] = "unicode-range",
  [GTEXT_CSS_TOKEN_WHITESPACE] = "whitespace",
  [GTEXT_CSS_TOKEN_CDO] = "CDO",
  [GTEXT_CSS_TOKEN_CDC] = "CDC",
  [GTEXT_CSS_TOKEN_COLON] = "colon",
  [GTEXT_CSS_TOKEN_SEMICOLON] = "semicolon",
  [GTEXT_CSS_TOKEN_COMMA] = "comma",
  [GTEXT_CSS_TOKEN_OPEN_SQUARE] = "[",
  [GTEXT_CSS_TOKEN_CLOSE_SQUARE] = "]",
  [GTEXT_CSS_TOKEN_OPEN_PAREN] = "(",
  [GTEXT_CSS_TOKEN_CLOSE_PAREN] = ")",
  [GTEXT_CSS_TOKEN_OPEN_CURLY] = "{",
  [GTEXT_CSS_TOKEN_CLOSE_CURLY] = "}",
  [GTEXT_CSS_TOKEN_EOF] = "EOF",
};

const char * gtext_css_token_type_name(GTEXT_CSS_Token_Type type) {
  if ((unsigned)type >= (unsigned)GTEXT_CSS_TOKEN_TYPE_COUNT) {
    return "unknown";
  }
  return token_names[type];
}

const char * gtext_css_error_name(GTEXT_CSS_Error_Code code) {
  switch (code) {
  case GTEXT_CSS_ERROR_INVALID_ESCAPE: return "invalid-escape";
  case GTEXT_CSS_ERROR_EOF_IN_COMMENT: return "eof-in-comment";
  case GTEXT_CSS_ERROR_EOF_IN_STRING: return "eof-in-string";
  case GTEXT_CSS_ERROR_NEWLINE_IN_STRING: return "newline-in-string";
  case GTEXT_CSS_ERROR_EOF_IN_URL: return "eof-in-url";
  case GTEXT_CSS_ERROR_INVALID_CHARACTER_IN_URL: return "invalid-character-in-url";
  case GTEXT_CSS_ERROR_INVALID_ESCAPE_IN_URL: return "invalid-escape-in-url";
  case GTEXT_CSS_ERROR_WHITESPACE_IN_URL: return "whitespace-in-url";
  case GTEXT_CSS_ERROR_EOF_IN_ESCAPE: return "eof-in-escape";
  case GTEXT_CSS_ERROR_INVALID_UTF_8: return "invalid-utf-8";
  case GTEXT_CSS_ERROR_EOF_IN_QUALIFIED_RULE: return "eof-in-qualified-rule";
  case GTEXT_CSS_ERROR_SEMICOLON_IN_QUALIFIED_RULE: return "semicolon-in-qualified-rule";
  case GTEXT_CSS_ERROR_CLOSE_BRACE_IN_QUALIFIED_RULE: return "close-brace-in-qualified-rule";
  case GTEXT_CSS_ERROR_UNMATCHED_CLOSE_BRACE: return "unmatched-close-brace";
  case GTEXT_CSS_ERROR_UNMATCHED_CLOSE_PAREN: return "unmatched-close-paren";
  case GTEXT_CSS_ERROR_UNMATCHED_CLOSE_BRACKET: return "unmatched-close-bracket";
  case GTEXT_CSS_ERROR_INVALID_DECLARATION: return "invalid-declaration";
  case GTEXT_CSS_ERROR_AT_RULE_IN_DECLARATION_LIST: return "at-rule-in-declaration-list";
  case GTEXT_CSS_ERROR_RULE_LOOKS_LIKE_CUSTOM_PROPERTY: return "rule-looks-like-custom-property";
  case GTEXT_CSS_ERROR_INVALID_RULE: return "invalid-rule";
  case GTEXT_CSS_ERROR_INVALID_DECLARATION_IN_CONTEXT: return "invalid-declaration-in-context";
  }
  return "unknown";
}

/* ---- options ---------------------------------------------------------- */

/** The default of max_input_bytes. */
#define DEFAULT_MAX_INPUT ((size_t)64 * 1024 * 1024)
/** The default of max_tokens. */
#define DEFAULT_MAX_TOKENS ((size_t)4 * 1024 * 1024)
/** The default of max_depth. */
#define DEFAULT_MAX_DEPTH ((size_t)512)
/** The default of max_rules. */
#define DEFAULT_MAX_RULES ((size_t)1024 * 1024)
/** The default of max_declarations. */
#define DEFAULT_MAX_DECLARATIONS ((size_t)4 * 1024 * 1024)
/** The default of max_total_errors. */
#define DEFAULT_MAX_TOTAL_ERRORS ((size_t)1024 * 1024)
/** The default of max_errors. */
#define DEFAULT_MAX_ERRORS ((size_t)1024)

GTEXT_CSS_Parse_Options gtext_css_parse_options_default(void) {
  GTEXT_CSS_Parse_Options o;
  memset(&o, 0, sizeof o);
  o.max_input_bytes = DEFAULT_MAX_INPUT;
  o.max_tokens = DEFAULT_MAX_TOKENS;
  o.max_depth = DEFAULT_MAX_DEPTH;
  o.max_rules = DEFAULT_MAX_RULES;
  o.max_declarations = DEFAULT_MAX_DECLARATIONS;
  o.max_total_errors = DEFAULT_MAX_TOTAL_ERRORS;
  o.max_errors = DEFAULT_MAX_ERRORS;
  return o;
}

static size_t or_default(size_t value, size_t fallback) {
  return value ? value : fallback;
}

/** Set up a context from options (css_internal.h). */
void css_ctx_init(css_ctx * ctx, const GTEXT_CSS_Parse_Options * options,
    bool keep_errors) {
  memset(ctx, 0, sizeof *ctx);
  GTEXT_CSS_Parse_Options zero;
  memset(&zero, 0, sizeof zero);
  const GTEXT_CSS_Parse_Options * o = options ? options : &zero;
  ctx->alloc = *(o->allocator ? o->allocator : gtext_allocator_default());
  ctx->max_input = or_default(o->max_input_bytes, DEFAULT_MAX_INPUT);
  ctx->max_tokens = or_default(o->max_tokens, DEFAULT_MAX_TOKENS);
  ctx->max_depth = or_default(o->max_depth, DEFAULT_MAX_DEPTH);
  ctx->max_rules = or_default(o->max_rules, DEFAULT_MAX_RULES);
  ctx->max_declarations = or_default(o->max_declarations, DEFAULT_MAX_DECLARATIONS);
  ctx->max_total_errors = or_default(o->max_total_errors, DEFAULT_MAX_TOTAL_ERRORS);
  ctx->max_errors = or_default(o->max_errors, DEFAULT_MAX_ERRORS);
  ctx->keep_errors = keep_errors && !o->count_errors_only;
  ctx->wtf8 = o->input_wtf8;
  ctx->validity = o->validity;
  ctx->status = GTEXT_CSS_OK;
  ctx->errors_on = true;
  css_arena_init(&ctx->arena, &ctx->alloc);
}

/** Free what a context holds (css_internal.h). */
void css_ctx_free(css_ctx * ctx, bool keep_arena) {
  css_tokvec_free(ctx, &ctx->toks);
  gtext_allocator_free(&ctx->alloc, ctx->match);
  ctx->match = NULL;
  gtext_allocator_free(&ctx->alloc, ctx->errs);
  ctx->errs = NULL;
  ctx->nerr = ctx->errcap = 0;
  css_buf_free(ctx, &ctx->sbuf);
  css_buf_free(ctx, &ctx->ubuf);
  css_buf_free(ctx, &ctx->nbuf);
  if (!keep_arena) {
    css_arena_free(&ctx->arena);
  }
}

/* ---- failure and parse errors ----------------------------------------- */

/** Mark the parse failed; the first failure wins (css_internal.h). */
void css_fail(css_ctx * ctx, GTEXT_CSS_Status status) {
  if (ctx->status == GTEXT_CSS_OK) {
    ctx->status = status;
  }
}

/** Mark the parse failed for a limit reached at a place (css_internal.h). */
void css_fail_at(css_ctx * ctx, GTEXT_CSS_Status status, size_t offset,
    size_t line, size_t column) {
  if (ctx->status == GTEXT_CSS_OK) {
    ctx->status = status;
    ctx->failure.offset = offset;
    ctx->failure.end = offset;
    ctx->failure.line = line;
    ctx->failure.column = column;
  }
}

/** Raise a parse error (css_internal.h). */
void css_error(css_ctx * ctx, GTEXT_CSS_Error_Code code, size_t offset,
    size_t line, size_t column) {
  if (!ctx->errors_on || ctx->status != GTEXT_CSS_OK) {
    return;
  }
  if (ctx->err_total >= ctx->max_total_errors) {
    css_fail_at(ctx, GTEXT_CSS_E_LIMIT_ERRORS, offset, line, column);
    return;
  }
  ctx->err_total++;
  if (!ctx->keep_errors) {
    return;
  }
  if (ctx->nerr == ctx->errcap) {
    const size_t cap = ctx->errcap ? ctx->errcap * 2 : 16;
    if (cap > SIZE_MAX / sizeof(css_err)) {
      css_fail(ctx, GTEXT_CSS_E_OOM);
      return;
    }
    css_err * grown = gtext_allocator_realloc(&ctx->alloc, ctx->errs, cap * sizeof(css_err));
    if (!grown) {
      css_fail(ctx, GTEXT_CSS_E_OOM);
      return;
    }
    ctx->errs = grown;
    ctx->errcap = cap;
  }
  css_err * e = &ctx->errs[ctx->nerr];
  e->code = code;
  e->offset = offset;
  e->line = line;
  e->column = column;
  e->seq = ctx->nerr;
  ctx->nerr++;
}

/* ---- buffers ---------------------------------------------------------- */

static bool buf_reserve(css_ctx * ctx, css_buf * buf, size_t more) {
  if (buf->cap - buf->len > more) {
    return true;
  }
  if (more > SIZE_MAX - buf->len - 1) {
    css_fail(ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  size_t cap = buf->cap ? buf->cap : 64;
  while (cap - buf->len <= more) {
    if (cap > SIZE_MAX / 2) {
      cap = buf->len + more + 1;
      break;
    }
    cap *= 2;
  }
  char * grown = gtext_allocator_realloc(&ctx->alloc, buf->data, cap);
  if (!grown) {
    css_fail(ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  buf->data = grown;
  buf->cap = cap;
  return true;
}

/** Append bytes to a buffer (css_internal.h). */
bool css_buf_put(css_ctx * ctx, css_buf * buf, const char * s, size_t n) {
  if (!buf_reserve(ctx, buf, n)) {
    return false;
  }
  if (n) {
    memcpy(buf->data + buf->len, s, n);
  }
  buf->len += n;
  buf->data[buf->len] = '\0';
  return true;
}

/** Append a code point as UTF-8 (css_internal.h). */
bool css_buf_put_cp(css_ctx * ctx, css_buf * buf, uint32_t cp) {
  char b[4];
  size_t n;
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    cp = 0xFFFD; /* not a scalar value: the buffer stays UTF-8 for any caller */
  }
  if (cp < 0x80) {
    b[0] = (char)cp;
    n = 1;
  }
  else if (cp < 0x800) {
    b[0] = (char)(0xC0 | (cp >> 6));
    b[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  }
  else if (cp < 0x10000) {
    b[0] = (char)(0xE0 | (cp >> 12));
    b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  }
  else {
    b[0] = (char)(0xF0 | (cp >> 18));
    b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[3] = (char)(0x80 | (cp & 0x3F));
    n = 4;
  }
  return css_buf_put(ctx, buf, b, n);
}

/** The tests' view of css_buf_put_cp() (css_internal.h). */
size_t gtext_css_encode_cp(uint32_t cp, char * out) {
  css_ctx ctx;
  css_ctx_init(&ctx, NULL, false);
  css_buf buf;
  memset(&buf, 0, sizeof buf);
  size_t n = 0;
  if (css_buf_put_cp(&ctx, &buf, cp)) {
    n = buf.len;
    memcpy(out, buf.data, n);
  }
  css_buf_free(&ctx, &buf);
  css_ctx_free(&ctx, false);
  return n;
}

/** Free a byte buffer (css_internal.h). */
void css_buf_free(css_ctx * ctx, css_buf * buf) {
  gtext_allocator_free(&ctx->alloc, buf->data);
  buf->data = NULL;
  buf->len = buf->cap = 0;
}

/** Append to a size_t vector (css_internal.h). */
bool css_sizevec_push(css_ctx * ctx, css_sizevec * vec, size_t value) {
  if (vec->len == vec->cap) {
    const size_t cap = vec->cap ? vec->cap * 2 : 32;
    if (cap > SIZE_MAX / sizeof(size_t)) {
      css_fail(ctx, GTEXT_CSS_E_OOM);
      return false;
    }
    size_t * grown = gtext_allocator_realloc(&ctx->alloc, vec->data, cap * sizeof(size_t));
    if (!grown) {
      css_fail(ctx, GTEXT_CSS_E_OOM);
      return false;
    }
    vec->data = grown;
    vec->cap = cap;
  }
  vec->data[vec->len++] = value;
  return true;
}

/* ---- the arena -------------------------------------------------------- */

/// Bytes of a block's header, padded to 16.
#define CHUNK_HEADER (((sizeof(css_arena_chunk)) + 15u) & ~(size_t)15u)
/// The size of an arena's first block.
#define CHUNK_MIN ((size_t)16 * 1024)

/** Initialize an arena (css_internal.h). */
void css_arena_init(css_arena * arena, const GTEXT_Allocator * alloc) {
  arena->alloc = *alloc;
  arena->head = NULL;
  arena->total = 0;
}

/** Bytes from an arena, aligned to 16 (css_internal.h). */
void * css_arena_alloc(css_arena * arena, size_t size) {
  if (size > SIZE_MAX - 15) {
    return NULL;
  }
  size = (size + 15u) & ~(size_t)15u;
  if (size == 0) {
    size = 16;
  }
  css_arena_chunk * c = arena->head;
  if (!c || c->cap - c->used < size) {
    size_t cap = c ? c->cap * 2 : CHUNK_MIN;
    if (cap > (size_t)8 * 1024 * 1024) {
      cap = (size_t)8 * 1024 * 1024;
    }
    if (cap < size) {
      cap = size;
    }
    if (cap > SIZE_MAX - CHUNK_HEADER) {
      return NULL;
    }
    css_arena_chunk * fresh = gtext_allocator_malloc(&arena->alloc, CHUNK_HEADER + cap);
    if (!fresh) {
      return NULL;
    }
    fresh->next = c;
    fresh->used = 0;
    fresh->cap = cap;
    arena->head = fresh;
    arena->total += CHUNK_HEADER + cap;
    c = fresh;
  }
  void * p = (unsigned char *)c + CHUNK_HEADER + c->used;
  c->used += size;
  return p;
}

/** A NUL-terminated copy in an arena (css_internal.h). */
char * css_arena_strdup(css_arena * arena, const char * s, size_t len) {
  if (len == SIZE_MAX) {
    return NULL;
  }
  char * p = css_arena_alloc(arena, len + 1);
  if (!p) {
    return NULL;
  }
  if (len) {
    memcpy(p, s, len);
  }
  p[len] = '\0';
  return p;
}

/** Free every block of an arena (css_internal.h). */
void css_arena_free(css_arena * arena) {
  css_arena_chunk * c = arena->head;
  while (c) {
    css_arena_chunk * next = c->next;
    gtext_allocator_free(&arena->alloc, c);
    c = next;
  }
  arena->head = NULL;
  arena->total = 0;
}

/* ---- small helpers ---------------------------------------------------- */

/** The value of a numeric token's spelling (css_internal.h). */
bool css_number_value(css_ctx * ctx, const char * repr, size_t len, double * out) {
  /* The conversion reads to the NUL: the contract is that repr[len] is one. */
  if (!repr || len == 0 || repr[len] != '\0') {
    css_fail(ctx, GTEXT_CSS_E_INTERNAL);
    return false;
  }
  /* The conversion can allocate (it respells the number in a locale whose
     decimal separator is not "."), and answers 0 with nothing consumed when
     it cannot; a numeric token's spelling always consumes something. */
  char * end = NULL;
  const double v = gtext_number_strtod(&ctx->alloc, repr, &end);
  if (end == repr) {
    css_fail(ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  *out = v;
  return true;
}

/** ASCII case-insensitive equality with a lower-case literal (css_internal.h). */
bool css_ascii_ieq(const GTEXT_CSS_String * s, const char * lower) {
  const size_t n = strlen(lower);
  if (!s->data || s->length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s->data[i];
    if (c >= 'A' && c <= 'Z') {
      c = (unsigned char)(c + 0x20);
    }
    if (c != (unsigned char)lower[i]) {
      return false;
    }
  }
  return true;
}

/** Whether a code point is a non-ASCII ident code point, CSS Syntax 4.2 (css_internal.h). */
bool css_is_non_ascii_ident(uint32_t cp) {
  return cp == 0xB7 || (cp >= 0xC0 && cp <= 0xD6) || (cp >= 0xD8 && cp <= 0xF6)
      || (cp >= 0xF8 && cp <= 0x37D) || (cp >= 0x37F && cp <= 0x1FFF)
      || cp == 0x200C || cp == 0x200D || cp == 0x203F || cp == 0x2040
      || (cp >= 0x2070 && cp <= 0x218F) || (cp >= 0x2C00 && cp <= 0x2FEF)
      || (cp >= 0x3001 && cp <= 0xD7FF) || (cp >= 0xF900 && cp <= 0xFDCF)
      || (cp >= 0xFDF0 && cp <= 0xFFFD) || cp >= 0x10000;
}

/* ---- the result ------------------------------------------------------- */

/* A stable merge sort of the raised errors by offset: the list is in the
   order of the input, and two errors at one place keep the order they were
   raised in (the tokenizer's before the parser's). */
static void sort_errors(css_err * a, css_err * tmp, size_t n) {
  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      size_t mid = lo + width < n ? lo + width : n;
      size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        if (a[j].offset < a[i].offset) {
          tmp[k++] = a[j++];
        }
        else {
          tmp[k++] = a[i++];
        }
      }
      while (i < mid) {
        tmp[k++] = a[i++];
      }
      while (j < hi) {
        tmp[k++] = a[j++];
      }
    }
    memcpy(a, tmp, n * sizeof *a);
  }
}

/** Fill a result from a finished context (css_internal.h). */
GTEXT_CSS_Status css_finish_result(css_ctx * ctx, GTEXT_CSS_Result * result,
    size_t input_bytes) {
  GTEXT_CSS_Status status = ctx->status;
  if (!result) {
    return status;
  }
  memset(result, 0, sizeof *result);
  result->input_bytes = input_bytes;
  result->token_count = ctx->toks.len;
  result->rule_count = ctx->rule_count;
  result->declaration_count = ctx->declaration_count;
  if (status != GTEXT_CSS_OK) {
    result->failure = ctx->failure;
  }
  size_t keep = ctx->nerr < ctx->max_errors ? ctx->nerr : ctx->max_errors;
  if (status != GTEXT_CSS_E_OOM) {
    if (ctx->nerr > 1) {
      css_err * tmp = gtext_allocator_malloc(&ctx->alloc, ctx->nerr * sizeof(css_err));
      if (!tmp) {
        status = GTEXT_CSS_E_OOM;
      }
      else {
        sort_errors(ctx->errs, tmp, ctx->nerr);
        gtext_allocator_free(&ctx->alloc, tmp);
      }
    }
  }
  if (status != GTEXT_CSS_E_OOM) {
    if (keep > (SIZE_MAX - sizeof(struct GTEXT_CSS_Result_Internal)) / sizeof(GTEXT_CSS_Error)) {
      status = GTEXT_CSS_E_OOM;
    }
  }
  if (status != GTEXT_CSS_E_OOM) {
    struct GTEXT_CSS_Result_Internal * in = gtext_allocator_malloc(&ctx->alloc,
        sizeof(struct GTEXT_CSS_Result_Internal) + keep * sizeof(GTEXT_CSS_Error));
    if (!in) {
      status = GTEXT_CSS_E_OOM;
    }
    else {
      in->allocator = ctx->alloc;
      in->count = keep;
      in->dropped = ctx->err_total - keep;
      for (size_t i = 0; i < keep; i++) {
        in->errors[i].code = ctx->errs[i].code;
        in->errors[i].name = gtext_css_error_name(ctx->errs[i].code);
        in->errors[i].line = ctx->errs[i].line;
        in->errors[i].column = ctx->errs[i].column;
        in->errors[i].byte_offset = ctx->errs[i].offset;
      }
      result->internal = in;
    }
  }
  result->status = status;
  result->message = gtext_css_status_string(status);
  return status;
}

void gtext_css_result_free(GTEXT_CSS_Result * result) {
  if (!result) {
    return;
  }
  if (result->internal) {
    GTEXT_Allocator a = result->internal->allocator;
    gtext_allocator_free(&a, result->internal);
  }
  memset(result, 0, sizeof *result);
}

size_t gtext_css_result_error_count(const GTEXT_CSS_Result * result) {
  return result && result->internal ? result->internal->count : 0;
}

size_t gtext_css_result_errors_dropped(const GTEXT_CSS_Result * result) {
  return result && result->internal ? result->internal->dropped : 0;
}

bool gtext_css_result_error_at(const GTEXT_CSS_Result * result, size_t index,
    GTEXT_CSS_Error * out) {
  if (!result || !out || !result->internal || index >= result->internal->count) {
    return false;
  }
  *out = result->internal->errors[index];
  return true;
}

void gtext_css_tree_free(GTEXT_CSS_Tree * tree) {
  if (!tree || !tree->internal) {
    return;
  }
  /* The tree struct itself is the arena's first allocation, so freeing the
     arena frees it: copy the arena out first. */
  css_arena arena = tree->internal->arena;
  css_arena_free(&arena);
}
