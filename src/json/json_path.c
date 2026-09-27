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
 * JSONPath (RFC 9535): compiling a query, and evaluating it over a DOM.
 *
 * Two halves that do not know much about each other. The compiler turns the
 * query text into an array of segments, each holding an array of selectors;
 * the evaluator walks the document applying one segment at a time to a node
 * list. Nothing is evaluated during compilation and nothing is parsed during
 * evaluation, which is what makes a compiled query reusable across documents.
 *
 * Three things about this file are decisions rather than mechanics.
 *
 * **The descendant walk is on the heap.** `$..a` visits a node and every node
 * under it, and the obvious way to write that is recursion - which makes the
 * depth of the caller's document the depth of this library's C stack. Three
 * DOM walks in the YAML module were moved off the stack for exactly that
 * reason; this one starts there. The stack is explicit, children are pushed in
 * reverse so they come off in document order, and running out of memory is a
 * status rather than a crash.
 *
 * **A node list may hold the same node twice.** `$[0,0]` selects the first
 * element twice and RFC 9535 says it does (2.3.1.2). So the evaluator appends
 * rather than unions, and a caller counting results gets the number the
 * specification gives.
 *
 * **A construct that cannot be evaluated is refused, not ignored.** Dropping a
 * filter and evaluating the rest would select every element of the array
 * instead of the ones asked for. An *ill-typed* query is GTEXT_JSON_E_PATH:
 * 2.4.2 says it is invalid, not false. match() and search() are evaluated.
 * Their pattern is I-Regexp (RFC 9485), compiled by ghoti.io-regex. A pattern
 * that is not I-Regexp is LogicalFalse, which is what RFC 9535 §2.4.6 says.
 * A budget the engine exhausts is GTEXT_JSON_E_LIMIT, because that question
 * was not answered.
 *
 * **The filter's recursion is the query's, not the document's.** The
 * expression parser and evaluator recurse over the expression tree, whose
 * depth is however many parentheses the caller wrote. Everything that walks the
 * *document* - the descendant collector, and deep equality for `==` between two
 * structured values - uses an explicit stack instead, because that depth is the
 * document's.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include "json_internal.h"

#include <ghoti.io/text/json/json_path.h>

#include <ghoti.io/regex/regex.h>

/* RFC 9535 2.3.3.1: an index or a slice bound is an integer in the
 * interchangeable range, so that every implementation agrees about it. */
#define JSON_PATH_MAX_INT 9007199254740991LL  /* 2^53 - 1 */
#define JSON_PATH_MIN_INT (-9007199254740991LL)

typedef enum {
  JSON_PATH_SEL_NAME,     /* ["a"] and .a */
  JSON_PATH_SEL_INDEX,    /* [3], [-1] */
  JSON_PATH_SEL_WILDCARD, /* [*] and .* */
  JSON_PATH_SEL_SLICE,    /* [1:5:2] */
  JSON_PATH_SEL_FILTER    /* [?<logical-expr>] */
} json_path_selector_kind;

/* ---------------------------------------------------------------------- *
 * The filter selector's expression (2.3.5)
 * ---------------------------------------------------------------------- */

typedef enum {
  JSON_PATH_EXPR_OR,
  JSON_PATH_EXPR_AND,
  JSON_PATH_EXPR_NOT,
  JSON_PATH_EXPR_TEST,    /* a query, true when it selects anything */
  JSON_PATH_EXPR_LOGICAL, /* match() or search(), a LogicalType test */
  JSON_PATH_EXPR_COMPARE  /* comparable <op> comparable */
} json_path_expr_kind;

typedef enum {
  JSON_PATH_CMP_EQ,
  JSON_PATH_CMP_NE,
  JSON_PATH_CMP_LT,
  JSON_PATH_CMP_LE,
  JSON_PATH_CMP_GT,
  JSON_PATH_CMP_GE
} json_path_cmp_op;

typedef enum {
  JSON_PATH_LIT_NUMBER,
  JSON_PATH_LIT_STRING,
  JSON_PATH_LIT_TRUE,
  JSON_PATH_LIT_FALSE,
  JSON_PATH_LIT_NULL
} json_path_literal_kind;

typedef struct {
  json_path_literal_kind kind;
  double number;
  int64_t integer;
  bool has_integer; /* so two large integers compare exactly */
  char * string;
  size_t string_len;
} json_path_literal;

/* A query inside a filter: relative to the current node (@) or to the root
 * ($). `singular` records whether every segment is a single name or index,
 * which is what 2.4.1 needs to know before a query may be compared. */
typedef struct {
  bool relative;
  bool singular;
  struct json_path_segment * segments;
  size_t segment_count;
} json_path_query;

typedef enum {
  JSON_PATH_FN_NONE,
  JSON_PATH_FN_LENGTH, /* length(ValueType) -> ValueType */
  JSON_PATH_FN_COUNT,  /* count(NodesType) -> ValueType */
  JSON_PATH_FN_VALUE,  /* value(NodesType) -> ValueType */
  JSON_PATH_FN_MATCH,  /* match(ValueType, ValueType) -> LogicalType */
  JSON_PATH_FN_SEARCH  /* search(ValueType, ValueType) -> LogicalType */
} json_path_fn;

typedef enum {
  JSON_PATH_COMPARABLE_LITERAL,
  JSON_PATH_COMPARABLE_QUERY,
  JSON_PATH_COMPARABLE_FUNCTION
} json_path_comparable_kind;

typedef struct json_path_comparable {
  json_path_comparable_kind kind;
  json_path_literal literal;
  json_path_query query;
  json_path_fn fn;
  struct json_path_comparable * arg;  /* every function's first argument */
  struct json_path_comparable * arg2; /* match() and search(): the pattern */
} json_path_comparable;

typedef struct json_path_expr {
  json_path_expr_kind kind;
  struct json_path_expr * left;
  struct json_path_expr * right;
  json_path_cmp_op op;
  json_path_comparable lhs;
  json_path_comparable rhs;
  json_path_query test;
  json_path_comparable logical; /* JSON_PATH_EXPR_LOGICAL */
} json_path_expr;

typedef struct json_path_selector {
  json_path_selector_kind kind;
  /* NAME: the decoded name, owned by the query. */
  char * name;
  size_t name_len;
  /* INDEX: the index, which may be negative. */
  int64_t index;
  /* SLICE: the three bounds, each present or not. */
  int64_t start;
  int64_t end;
  int64_t step;
  bool has_start;
  bool has_end;
  /* FILTER: the expression, owned by the selector. */
  json_path_expr * filter;
} json_path_selector;

typedef struct json_path_segment {
  bool descendant; /* ".." rather than "." or "[" */
  struct json_path_selector * selectors;
  size_t selector_count;
} json_path_segment;

struct GTEXT_JSON_Path {
  const GTEXT_Allocator * alloc;
  json_path_segment * segments;
  size_t segment_count;
};

/* ======================================================================
 * Compilation
 * ====================================================================== */

typedef struct {
  const char * text;
  size_t len;
  size_t at;
  const GTEXT_Allocator * alloc;
  GTEXT_JSON_Status status;
  const char * message;
} json_path_parser;

static bool json_path_bracket(json_path_parser * p, json_path_segment * seg);
static bool json_path_shorthand(json_path_parser * p, json_path_segment * seg);
static json_path_expr * json_path_logical_or(json_path_parser * p);
static void json_path_free_expr(
    const GTEXT_Allocator * alloc, json_path_expr * expr);
static void json_path_free_comparable(
    const GTEXT_Allocator * alloc, json_path_comparable * c);
static void json_path_free_segments(const GTEXT_Allocator * alloc,
    json_path_segment * segments, size_t count);

static void json_path_fail(json_path_parser * p, GTEXT_JSON_Status status,
    const char * message) {
  if (p->status == GTEXT_JSON_OK) { /* keep the first complaint */
    p->status = status;
    p->message = message;
  }
}

static bool json_path_at_end(const json_path_parser * p) {
  return p->at >= p->len;
}

static char json_path_peek(const json_path_parser * p) {
  return json_path_at_end(p) ? '\0' : p->text[p->at];
}

static char json_path_peek_at(const json_path_parser * p, size_t ahead) {
  return (p->at + ahead >= p->len) ? '\0' : p->text[p->at + ahead];
}

/* RFC 9535 2.1: S is blank space, and only these four characters. */
static void json_path_skip_space(json_path_parser * p) {
  while (!json_path_at_end(p)) {
    const char c = p->text[p->at];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      p->at++;
    }
    else {
      break;
    }
  }
}

static bool json_path_is_digit(char c) {
  return c >= '0' && c <= '9';
}

/* name-first / name-char (2.5.1.1): ASCII letters, "_", and every
 * non-ASCII character. A shorthand name is *not* a JSON string: it has no
 * escapes, and the bytes go through as they are. Their well-formedness as
 * UTF-8 is the document's business, not this grammar's. */
static bool json_path_name_first(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
      || c >= 0x80;
}

static bool json_path_name_char(unsigned char c) {
  return json_path_name_first(c) || (c >= '0' && c <= '9');
}

/* One hex digit's value, or -1. */
static int json_path_hex(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/* Write a codepoint as UTF-8; returns the byte count. */
static size_t json_path_utf8(uint32_t cp, char * out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/*
 * A string literal (2.3.1.1), single- or double-quoted.
 *
 * The escapes are JSON's, with one difference in each direction: a
 * double-quoted string may not hold an unescaped `"` and may hold `'`, and a
 * single-quoted string is the other way round. Everything else - \b \f \n \r
 * \t \/ \\ \uXXXX and surrogate pairs - is the same, and anything else after a
 * backslash is an error rather than the character itself.
 *
 * The decoded bytes are written into a buffer the query owns. Decoding can
 * only shrink, so the source length is a safe capacity.
 */
static char * json_path_string(json_path_parser * p, size_t * out_len) {
  const char quote = json_path_peek(p);
  if (quote != '"' && quote != '\'') {
    json_path_fail(p, GTEXT_JSON_E_PATH, "expected a quoted name");
    return NULL;
  }
  p->at++;

  const size_t capacity = p->len - p->at + 1;
  char * out = (char *)gtext_allocator_malloc(p->alloc, capacity);
  if (!out) {
    json_path_fail(p, GTEXT_JSON_E_OOM, "out of memory");
    return NULL;
  }
  size_t used = 0;

  while (!json_path_at_end(p)) {
    const unsigned char c = (unsigned char)p->text[p->at];
    if (c == (unsigned char)quote) {
      p->at++;
      out[used] = '\0';
      *out_len = used;
      return out;
    }
    if (c < 0x20) {
      json_path_fail(
          p, GTEXT_JSON_E_PATH, "unescaped control character in a name");
      break;
    }
    if (c != '\\') {
      out[used++] = (char)c;
      p->at++;
      continue;
    }

    /* An escape. */
    p->at++;
    const char esc = json_path_peek(p);
    switch (esc) {
    case 'b': out[used++] = '\b'; p->at++; continue;
    case 'f': out[used++] = '\f'; p->at++; continue;
    case 'n': out[used++] = '\n'; p->at++; continue;
    case 'r': out[used++] = '\r'; p->at++; continue;
    case 't': out[used++] = '\t'; p->at++; continue;
    case '/': out[used++] = '/'; p->at++; continue;
    case '\\': out[used++] = '\\'; p->at++; continue;
    case 'u': break; /* below */
    default:
      /* The quote that delimits *this* string may be escaped; the other one
       * may not be, because it needs no escaping and 2.3.1.1 lists only one. */
      if (esc == quote) {
        out[used++] = esc;
        p->at++;
        continue;
      }
      json_path_fail(p, GTEXT_JSON_E_PATH, "unknown escape in a name");
      gtext_allocator_free(p->alloc, out);
      return NULL;
    }

    /* \uXXXX, and a surrogate pair written as two of them. */
    p->at++; /* past the 'u' */
    uint32_t cp = 0;
    for (int i = 0; i < 4; i++) {
      const int digit = json_path_hex(json_path_peek(p));
      if (digit < 0) {
        json_path_fail(p, GTEXT_JSON_E_PATH, "a \\u escape wants four hex digits");
        gtext_allocator_free(p->alloc, out);
        return NULL;
      }
      cp = (cp << 4) | (uint32_t)digit;
      p->at++;
    }
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (json_path_peek(p) != '\\' || json_path_peek_at(p, 1) != 'u') {
        json_path_fail(p, GTEXT_JSON_E_PATH,
            "a high surrogate wants a low surrogate after it");
        gtext_allocator_free(p->alloc, out);
        return NULL;
      }
      p->at += 2;
      uint32_t low = 0;
      for (int i = 0; i < 4; i++) {
        const int digit = json_path_hex(json_path_peek(p));
        if (digit < 0) {
          json_path_fail(
              p, GTEXT_JSON_E_PATH, "a \\u escape wants four hex digits");
          gtext_allocator_free(p->alloc, out);
          return NULL;
        }
        low = (low << 4) | (uint32_t)digit;
        p->at++;
      }
      if (low < 0xDC00 || low > 0xDFFF) {
        json_path_fail(
            p, GTEXT_JSON_E_PATH, "a high surrogate wants a low surrogate");
        gtext_allocator_free(p->alloc, out);
        return NULL;
      }
      cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
    }
    else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      json_path_fail(p, GTEXT_JSON_E_PATH, "a low surrogate on its own");
      gtext_allocator_free(p->alloc, out);
      return NULL;
    }
    used += json_path_utf8(cp, out + used);
  }

  json_path_fail(p, GTEXT_JSON_E_PATH, "a name was never closed");
  gtext_allocator_free(p->alloc, out);
  return NULL;
}

/*
 * An integer (2.3.3.1 int): an optional "-", then "0" alone or a digit 1-9
 * followed by digits. So "01" and "-0" are not integers, which is a rule about
 * the *query* and has nothing to do with what JSON numbers allow.
 */
static bool json_path_int(json_path_parser * p, int64_t * out) {
  const size_t start = p->at;
  bool negative = false;
  if (json_path_peek(p) == '-') {
    negative = true;
    p->at++;
  }
  if (!json_path_is_digit(json_path_peek(p))) {
    p->at = start;
    return false;
  }
  if (json_path_peek(p) == '0') {
    p->at++;
    if (json_path_is_digit(json_path_peek(p))) {
      json_path_fail(p, GTEXT_JSON_E_PATH, "a leading zero is not an integer");
      return false;
    }
    if (negative) {
      json_path_fail(p, GTEXT_JSON_E_PATH, "-0 is not an integer");
      return false;
    }
    *out = 0;
    return true;
  }
  int64_t value = 0;
  while (json_path_is_digit(json_path_peek(p))) {
    const int digit = json_path_peek(p) - '0';
    if (value > (JSON_PATH_MAX_INT - digit) / 10) {
      json_path_fail(p, GTEXT_JSON_E_PATH,
          "an index outside the interchangeable range");
      return false;
    }
    value = value * 10 + digit;
    p->at++;
  }
  *out = negative ? -value : value;
  return true;
}

/* Room for one more selector in a segment. */
static bool json_path_segment_grow(
    json_path_parser * p, json_path_segment * seg, size_t capacity) {
  json_path_selector * grown = (json_path_selector *)gtext_allocator_realloc(
      p->alloc, seg->selectors, capacity * sizeof(json_path_selector));
  if (!grown) {
    json_path_fail(p, GTEXT_JSON_E_OOM, "out of memory");
    return false;
  }
  seg->selectors = grown;
  return true;
}

/* One selector inside brackets. */
static bool json_path_selector_parse(
    json_path_parser * p, json_path_selector * out) {
  memset(out, 0, sizeof(*out));
  out->step = 1;

  const char c = json_path_peek(p);
  if (c == '*') {
    p->at++;
    out->kind = JSON_PATH_SEL_WILDCARD;
    return true;
  }
  if (c == '"' || c == '\'') {
    out->kind = JSON_PATH_SEL_NAME;
    out->name = json_path_string(p, &out->name_len);
    return out->name != NULL;
  }
  if (c == '?') {
    p->at++;
    json_path_skip_space(p);
    out->kind = JSON_PATH_SEL_FILTER;
    out->filter = json_path_logical_or(p);
    return out->filter != NULL;
  }

  /* An index or a slice. The two are told apart by the colon, which may come
   * before the first number: "[:2]" is a slice with no start. */
  bool has_first = false;
  int64_t first = 0;
  if (c == '-' || json_path_is_digit(c)) {
    if (!json_path_int(p, &first)) {
      return false;
    }
    has_first = true;
    json_path_skip_space(p);
  }
  if (json_path_peek(p) != ':') {
    if (!has_first) {
      json_path_fail(p, GTEXT_JSON_E_PATH, "expected a selector");
      return false;
    }
    out->kind = JSON_PATH_SEL_INDEX;
    out->index = first;
    return true;
  }

  out->kind = JSON_PATH_SEL_SLICE;
  out->has_start = has_first;
  out->start = first;
  p->at++; /* the first colon */
  json_path_skip_space(p);
  if (json_path_peek(p) == '-' || json_path_is_digit(json_path_peek(p))) {
    if (!json_path_int(p, &out->end)) {
      return false;
    }
    out->has_end = true;
    json_path_skip_space(p);
  }
  if (json_path_peek(p) == ':') {
    p->at++;
    json_path_skip_space(p);
    if (json_path_peek(p) == '-' || json_path_is_digit(json_path_peek(p))) {
      if (!json_path_int(p, &out->step)) {
        return false;
      }
    }
  }
  return true;
}

/* ---------------------------------------------------------------------- *
 * The filter expression parser
 *
 * Straight recursive descent over 2.3.5's grammar, with "||" lowest, then
 * "&&", then "!" and the comparisons. The recursion here is bounded by the
 * *query text*, not by the document, so it is the C stack's - a query with a
 * thousand nested parentheses is a thousand frames, and a query is written by
 * the caller rather than arriving as data.
 * ---------------------------------------------------------------------- */

/* The segments of a query inside a filter, and whether they are all singular.
 * 2.4.1: only a singular query - every segment one name or one index - may be
 * compared, because a comparison needs a value and a query yields a list. */
static bool json_path_filter_query(json_path_parser * p, json_path_query * out) {
  memset(out, 0, sizeof(*out));
  const char c = json_path_peek(p);
  if (c == '@') {
    out->relative = true;
  }
  else if (c == '$') {
    out->relative = false;
  }
  else {
    json_path_fail(p, GTEXT_JSON_E_PATH, "expected '@' or '$'");
    return false;
  }
  p->at++;

  out->singular = true;
  size_t capacity = 0;
  for (;;) {
    /* `segments = *(S segment)` and `singular-query-segments = *(S (...))`, so
     * blank space comes before each segment: `length(@ .a .b)` is a query with
     * two segments, and the compliance suite has eight cases saying so. The
     * position is restored when what follows the space is not a segment, so
     * the space in `@.a == 1` still belongs to the operator. */
    const size_t before_space = p->at;
    json_path_skip_space(p);
    const char next = json_path_peek(p);
    if (next != '.' && next != '[') {
      p->at = before_space;
      break;
    }
    if (out->segment_count == capacity) {
      const size_t grown_capacity = capacity ? capacity * 2 : 4;
      json_path_segment * grown = (json_path_segment *)gtext_allocator_realloc(
          p->alloc, out->segments, grown_capacity * sizeof(json_path_segment));
      if (!grown) {
        json_path_fail(p, GTEXT_JSON_E_OOM, "out of memory");
        return false;
      }
      out->segments = grown;
      capacity = grown_capacity;
    }
    json_path_segment * seg = &out->segments[out->segment_count];
    memset(seg, 0, sizeof(*seg));

    bool ok;
    if (next == '[') {
      ok = json_path_bracket(p, seg);
    }
    else {
      p->at++;
      if (json_path_peek(p) == '.') {
        p->at++;
        seg->descendant = true;
        ok = (json_path_peek(p) == '[') ? json_path_bracket(p, seg)
                                        : json_path_shorthand(p, seg);
      }
      else {
        ok = json_path_shorthand(p, seg);
      }
    }
    if (!ok) {
      for (size_t j = 0; j < seg->selector_count; j++) {
        gtext_allocator_free(p->alloc, seg->selectors[j].name);
        json_path_free_expr(p->alloc, seg->selectors[j].filter);
      }
      gtext_allocator_free(p->alloc, seg->selectors);
      return false;
    }

    /* Singular means one selector, and that selector a name or an index, and
     * the segment a child rather than a descendant. */
    if (seg->descendant || seg->selector_count != 1
        || (seg->selectors[0].kind != JSON_PATH_SEL_NAME
            && seg->selectors[0].kind != JSON_PATH_SEL_INDEX)) {
      out->singular = false;
    }
    out->segment_count++;
  }
  return true;
}

/* A JSON number as a literal (2.3.5.1 number). */
static bool json_path_number_literal(
    json_path_parser * p, json_path_literal * out) {
  const size_t start = p->at;
  if (json_path_peek(p) == '-') {
    p->at++;
  }
  if (!json_path_is_digit(json_path_peek(p))) {
    json_path_fail(p, GTEXT_JSON_E_PATH, "expected a number");
    return false;
  }
  if (json_path_peek(p) == '0') {
    p->at++;
    if (json_path_is_digit(json_path_peek(p))) {
      json_path_fail(p, GTEXT_JSON_E_PATH, "a number has no leading zero");
      return false;
    }
  }
  else {
    while (json_path_is_digit(json_path_peek(p))) {
      p->at++;
    }
  }
  bool integral = true;
  if (json_path_peek(p) == '.') {
    integral = false;
    p->at++;
    if (!json_path_is_digit(json_path_peek(p))) {
      json_path_fail(p, GTEXT_JSON_E_PATH, "a fraction wants a digit");
      return false;
    }
    while (json_path_is_digit(json_path_peek(p))) {
      p->at++;
    }
  }
  if (json_path_peek(p) == 'e' || json_path_peek(p) == 'E') {
    integral = false;
    p->at++;
    if (json_path_peek(p) == '+' || json_path_peek(p) == '-') {
      p->at++;
    }
    if (!json_path_is_digit(json_path_peek(p))) {
      json_path_fail(p, GTEXT_JSON_E_PATH, "an exponent wants a digit");
      return false;
    }
    while (json_path_is_digit(json_path_peek(p))) {
      p->at++;
    }
  }

  /* The text is a JSON number, which json_parse_number() already knows how to
   * read - including keeping an exact int64 where there is one, which is what
   * lets two large integers compare by value rather than through a double. */
  const size_t len = p->at - start;
  json_number num;
  json_position pos = {0, 1, 1};
  GTEXT_JSON_Parse_Options number_opts = gtext_json_parse_options_default();
  number_opts.parse_int64 = true;
  number_opts.parse_double = true;
  number_opts.preserve_number_lexeme = false;
  if (json_parse_number(p->text + start, len, &num, &pos, &number_opts)
      != GTEXT_JSON_OK) {
    json_path_fail(p, GTEXT_JSON_E_PATH, "not a number");
    return false;
  }
  out->kind = JSON_PATH_LIT_NUMBER;
  out->number = (num.flags & JSON_NUMBER_HAS_DOUBLE) ? num.dbl : 0.0;
  out->has_integer = integral && (num.flags & JSON_NUMBER_HAS_I64) != 0;
  out->integer = out->has_integer ? num.i64 : 0;
  if (!(num.flags & JSON_NUMBER_HAS_DOUBLE) && out->has_integer) {
    out->number = (double)out->integer;
  }
  json_number_destroy(&num);
  return true;
}

static bool json_path_word(json_path_parser * p, const char * word) {
  const size_t len = strlen(word);
  if (p->at + len > p->len || memcmp(p->text + p->at, word, len) != 0) {
    return false;
  }
  p->at += len;
  return true;
}

/* A word that has to end where it ends: `true` is the literal, `trueish` is
 * not it followed by junk. The operators do not need this - nothing follows
 * "==" that could continue it - and must not have it, because what follows an
 * operator is usually a name character. */
static bool json_path_keyword(json_path_parser * p, const char * word) {
  const size_t len = strlen(word);
  if (p->at + len > p->len || memcmp(p->text + p->at, word, len) != 0) {
    return false;
  }
  if (p->at + len < p->len
      && json_path_name_char((unsigned char)p->text[p->at + len])) {
    return false;
  }
  p->at += len;
  return true;
}

static bool json_path_comparable_parse(
    json_path_parser * p, json_path_comparable * out);

static bool json_path_fn_is_logical(json_path_fn fn) {
  return fn == JSON_PATH_FN_MATCH || fn == JSON_PATH_FN_SEARCH;
}

/* ValueType: a literal, a singular query, or a function whose result is a
 * value. match() and search() return LogicalType, so they cannot stand where
 * a value is required (2.4.3). */
static bool json_path_is_value_argument(const json_path_comparable * arg) {
  if (arg->kind == JSON_PATH_COMPARABLE_LITERAL) {
    return true;
  }
  if (arg->kind == JSON_PATH_COMPARABLE_QUERY) {
    return arg->query.singular;
  }
  if (arg->kind == JSON_PATH_COMPARABLE_FUNCTION) {
    return !json_path_fn_is_logical(arg->fn);
  }
  return false;
}

/* function-expr. The five standard functions. A name that is not one of them
 * is not a well-formed query: 2.4.1 requires the name to be registered. */
static bool json_path_function(json_path_parser * p, json_path_comparable * out) {
  const size_t start = p->at;
  while (!json_path_at_end(p)
      && ((p->text[p->at] >= 'a' && p->text[p->at] <= 'z')
          || p->text[p->at] == '_')) {
    p->at++;
  }
  const size_t name_len = p->at - start;
  const char * name = p->text + start;

  if (json_path_peek(p) != '(') {
    json_path_fail(p, GTEXT_JSON_E_PATH, "expected '(' after a function name");
    return false;
  }

  json_path_fn fn = JSON_PATH_FN_NONE;
  if (name_len == 6 && memcmp(name, "length", 6) == 0) {
    fn = JSON_PATH_FN_LENGTH;
  }
  else if (name_len == 5 && memcmp(name, "count", 5) == 0) {
    fn = JSON_PATH_FN_COUNT;
  }
  else if (name_len == 5 && memcmp(name, "value", 5) == 0) {
    fn = JSON_PATH_FN_VALUE;
  }
  else if (name_len == 5 && memcmp(name, "match", 5) == 0) {
    fn = JSON_PATH_FN_MATCH;
  }
  else if (name_len == 6 && memcmp(name, "search", 6) == 0) {
    fn = JSON_PATH_FN_SEARCH;
  }
  else {
    /* 2.4.1: a function this implementation does not know is not a
     * well-formed query, because the name has to be registered. */
    json_path_fail(p, GTEXT_JSON_E_PATH, "unknown function");
    return false;
  }

  p->at++; /* '(' */
  json_path_skip_space(p);
  json_path_comparable * arg = (json_path_comparable *)gtext_allocator_calloc(
      p->alloc, 1, sizeof(json_path_comparable));
  if (!arg) {
    json_path_fail(p, GTEXT_JSON_E_OOM, "out of memory");
    return false;
  }
  if (!json_path_comparable_parse(p, arg)) {
    json_path_free_comparable(p->alloc, arg);
    gtext_allocator_free(p->alloc, arg);
    return false;
  }
  json_path_skip_space(p);

  json_path_comparable * arg2 = NULL;
  if (json_path_fn_is_logical(fn)) {
    /* match() and search() take the subject and then the I-Regexp. */
    if (json_path_peek(p) != ',') {
      json_path_fail(p, GTEXT_JSON_E_PATH,
          "match() and search() take exactly two arguments");
      json_path_free_comparable(p->alloc, arg);
      gtext_allocator_free(p->alloc, arg);
      return false;
    }
    p->at++;
    json_path_skip_space(p);
    arg2 = (json_path_comparable *)gtext_allocator_calloc(
        p->alloc, 1, sizeof(json_path_comparable));
    if (!arg2) {
      json_path_fail(p, GTEXT_JSON_E_OOM, "out of memory");
      json_path_free_comparable(p->alloc, arg);
      gtext_allocator_free(p->alloc, arg);
      return false;
    }
    if (!json_path_comparable_parse(p, arg2)) {
      json_path_free_comparable(p->alloc, arg);
      gtext_allocator_free(p->alloc, arg);
      json_path_free_comparable(p->alloc, arg2);
      gtext_allocator_free(p->alloc, arg2);
      return false;
    }
    json_path_skip_space(p);
  }

  if (json_path_peek(p) != ')') {
    /* 2.4.1 makes a wrong arity invalid. A comma after a one-argument
     * function, or a third argument to match() or search(), lands here. */
    json_path_fail(p, GTEXT_JSON_E_PATH,
        json_path_fn_is_logical(fn)
            ? "match() and search() take exactly two arguments"
            : "this function takes exactly one argument");
    json_path_free_comparable(p->alloc, arg);
    gtext_allocator_free(p->alloc, arg);
    if (arg2) {
      json_path_free_comparable(p->alloc, arg2);
      gtext_allocator_free(p->alloc, arg2);
    }
    return false;
  }
  p->at++;

  /* The type rules of 2.4.2/2.4.3, which make an ill-typed query invalid.
   * count() and value() take a node list, so their argument has to be a query.
   * length(), and both arguments of match() and search(), take a value, so a
   * query argument has to be a singular one. */
  if (fn == JSON_PATH_FN_COUNT || fn == JSON_PATH_FN_VALUE) {
    if (arg->kind != JSON_PATH_COMPARABLE_QUERY) {
      json_path_fail(p, GTEXT_JSON_E_PATH,
          "count() and value() take a query, not a value");
      json_path_free_comparable(p->alloc, arg);
      gtext_allocator_free(p->alloc, arg);
      return false;
    }
  }
  else if (json_path_fn_is_logical(fn)) {
    if (!json_path_is_value_argument(arg) || !json_path_is_value_argument(arg2)) {
      json_path_fail(p, GTEXT_JSON_E_PATH,
          "match() and search() take two values");
      json_path_free_comparable(p->alloc, arg);
      gtext_allocator_free(p->alloc, arg);
      json_path_free_comparable(p->alloc, arg2);
      gtext_allocator_free(p->alloc, arg2);
      return false;
    }
  }
  else if (!json_path_is_value_argument(arg)) {
    json_path_fail(p, GTEXT_JSON_E_PATH,
        "length() takes a value, so its query must be singular");
    json_path_free_comparable(p->alloc, arg);
    gtext_allocator_free(p->alloc, arg);
    return false;
  }

  out->kind = JSON_PATH_COMPARABLE_FUNCTION;
  out->fn = fn;
  out->arg = arg;
  out->arg2 = arg2;
  return true;
}

/* comparable = literal / singular-query / function-expr - and, where this is a
 * function's argument, a query that need not be singular. The caller checks
 * that. */
static bool json_path_comparable_parse(
    json_path_parser * p, json_path_comparable * out) {
  memset(out, 0, sizeof(*out));
  const char c = json_path_peek(p);

  if (c == '@' || c == '$') {
    out->kind = JSON_PATH_COMPARABLE_QUERY;
    return json_path_filter_query(p, &out->query);
  }
  if (c == '\'' || c == '"') {
    out->kind = JSON_PATH_COMPARABLE_LITERAL;
    out->literal.kind = JSON_PATH_LIT_STRING;
    out->literal.string = json_path_string(p, &out->literal.string_len);
    return out->literal.string != NULL;
  }
  if (c == '-' || json_path_is_digit(c)) {
    out->kind = JSON_PATH_COMPARABLE_LITERAL;
    return json_path_number_literal(p, &out->literal);
  }
  if (json_path_keyword(p, "true")) {
    out->kind = JSON_PATH_COMPARABLE_LITERAL;
    out->literal.kind = JSON_PATH_LIT_TRUE;
    return true;
  }
  if (json_path_keyword(p, "false")) {
    out->kind = JSON_PATH_COMPARABLE_LITERAL;
    out->literal.kind = JSON_PATH_LIT_FALSE;
    return true;
  }
  if (json_path_keyword(p, "null")) {
    out->kind = JSON_PATH_COMPARABLE_LITERAL;
    out->literal.kind = JSON_PATH_LIT_NULL;
    return true;
  }
  if ((c >= 'a' && c <= 'z') || c == '_') {
    return json_path_function(p, out);
  }
  json_path_fail(p, GTEXT_JSON_E_PATH, "expected a value, a query or a function");
  return false;
}

static json_path_expr * json_path_new_expr(
    json_path_parser * p, json_path_expr_kind kind) {
  json_path_expr * expr = (json_path_expr *)gtext_allocator_calloc(
      p->alloc, 1, sizeof(json_path_expr));
  if (!expr) {
    json_path_fail(p, GTEXT_JSON_E_OOM, "out of memory");
    return NULL;
  }
  expr->kind = kind;
  return expr;
}

/* basic-expr = paren-expr / comparison-expr / test-expr */
static json_path_expr * json_path_basic(json_path_parser * p) {
  bool negated = false;
  if (json_path_peek(p) == '!') {
    p->at++;
    json_path_skip_space(p);
    negated = true;
  }

  json_path_expr * inner = NULL;

  if (json_path_peek(p) == '(') {
    p->at++;
    json_path_skip_space(p);
    inner = json_path_logical_or(p);
    if (!inner) {
      return NULL;
    }
    json_path_skip_space(p);
    if (json_path_peek(p) != ')') {
      json_path_fail(p, GTEXT_JSON_E_PATH, "expected ')'");
      json_path_free_expr(p->alloc, inner);
      return NULL;
    }
    p->at++;
  }
  else {
    /* A comparable, and then either a comparison operator or nothing - in
     * which case this is a test expression and the comparable has to have been
     * a query. */
    json_path_comparable first;
    if (!json_path_comparable_parse(p, &first)) {
      return NULL;
    }
    const size_t after_first = p->at;
    json_path_skip_space(p);

    json_path_cmp_op op = JSON_PATH_CMP_EQ;
    bool have_op = true;
    if (json_path_word(p, "==")) {
      op = JSON_PATH_CMP_EQ;
    }
    else if (json_path_word(p, "!=")) {
      op = JSON_PATH_CMP_NE;
    }
    else if (json_path_word(p, "<=")) {
      op = JSON_PATH_CMP_LE;
    }
    else if (json_path_word(p, ">=")) {
      op = JSON_PATH_CMP_GE;
    }
    else if (json_path_word(p, "<")) {
      op = JSON_PATH_CMP_LT;
    }
    else if (json_path_word(p, ">")) {
      op = JSON_PATH_CMP_GT;
    }
    else {
      have_op = false;
      p->at = after_first; /* the space belonged to whatever follows */
    }

    if (!have_op) {
      /* test-expr: a query, true when it selects at least one node, or
       * match()/search(), which return LogicalType (2.4.3). A value function
       * or a literal standing here is ill-typed, which is invalid. */
      if (first.kind == JSON_PATH_COMPARABLE_QUERY) {
        inner = json_path_new_expr(p, JSON_PATH_EXPR_TEST);
        if (!inner) {
          json_path_free_comparable(p->alloc, &first);
          return NULL;
        }
        inner->test = first.query;
        /* The query's segments moved into the expression. */
        memset(&first, 0, sizeof(first));
      }
      else if (first.kind == JSON_PATH_COMPARABLE_FUNCTION
          && json_path_fn_is_logical(first.fn)) {
        inner = json_path_new_expr(p, JSON_PATH_EXPR_LOGICAL);
        if (!inner) {
          json_path_free_comparable(p->alloc, &first);
          return NULL;
        }
        inner->logical = first;
        memset(&first, 0, sizeof(first));
      }
      else {
        json_path_fail(p, GTEXT_JSON_E_PATH,
            "this expression is not a test: it has no comparison");
        json_path_free_comparable(p->alloc, &first);
        return NULL;
      }
    }
    else {
      json_path_skip_space(p);
      json_path_comparable second;
      if (!json_path_comparable_parse(p, &second)) {
        json_path_free_comparable(p->alloc, &first);
        return NULL;
      }
      /* 2.4.3: LogicalType may not be compared. match() == true is invalid,
       * not false. 2.4.1: only a singular query may be compared. */
      if ((first.kind == JSON_PATH_COMPARABLE_FUNCTION
              && json_path_fn_is_logical(first.fn))
          || (second.kind == JSON_PATH_COMPARABLE_FUNCTION
              && json_path_fn_is_logical(second.fn))) {
        json_path_fail(p, GTEXT_JSON_E_PATH,
            "match() and search() return a logical value, which cannot be compared");
        json_path_free_comparable(p->alloc, &first);
        json_path_free_comparable(p->alloc, &second);
        return NULL;
      }
      if ((first.kind == JSON_PATH_COMPARABLE_QUERY && !first.query.singular)
          || (second.kind == JSON_PATH_COMPARABLE_QUERY
              && !second.query.singular)) {
        json_path_fail(p, GTEXT_JSON_E_PATH,
            "only a singular query may be compared");
        json_path_free_comparable(p->alloc, &first);
        json_path_free_comparable(p->alloc, &second);
        return NULL;
      }
      inner = json_path_new_expr(p, JSON_PATH_EXPR_COMPARE);
      if (!inner) {
        json_path_free_comparable(p->alloc, &first);
        json_path_free_comparable(p->alloc, &second);
        return NULL;
      }
      inner->op = op;
      inner->lhs = first;
      inner->rhs = second;
    }
  }

  if (!negated) {
    return inner;
  }
  json_path_expr * not_expr = json_path_new_expr(p, JSON_PATH_EXPR_NOT);
  if (!not_expr) {
    json_path_free_expr(p->alloc, inner);
    return NULL;
  }
  not_expr->left = inner;
  return not_expr;
}

static json_path_expr * json_path_logical_and(json_path_parser * p) {
  json_path_expr * left = json_path_basic(p);
  if (!left) {
    return NULL;
  }
  for (;;) {
    const size_t before = p->at;
    json_path_skip_space(p);
    if (!json_path_word(p, "&&")) {
      p->at = before;
      return left;
    }
    json_path_skip_space(p);
    json_path_expr * right = json_path_basic(p);
    if (!right) {
      json_path_free_expr(p->alloc, left);
      return NULL;
    }
    json_path_expr * both = json_path_new_expr(p, JSON_PATH_EXPR_AND);
    if (!both) {
      json_path_free_expr(p->alloc, left);
      json_path_free_expr(p->alloc, right);
      return NULL;
    }
    both->left = left;
    both->right = right;
    left = both;
  }
}

static json_path_expr * json_path_logical_or(json_path_parser * p) {
  json_path_expr * left = json_path_logical_and(p);
  if (!left) {
    return NULL;
  }
  for (;;) {
    const size_t before = p->at;
    json_path_skip_space(p);
    if (!json_path_word(p, "||")) {
      p->at = before;
      return left;
    }
    json_path_skip_space(p);
    json_path_expr * right = json_path_logical_and(p);
    if (!right) {
      json_path_free_expr(p->alloc, left);
      return NULL;
    }
    json_path_expr * either = json_path_new_expr(p, JSON_PATH_EXPR_OR);
    if (!either) {
      json_path_free_expr(p->alloc, left);
      json_path_free_expr(p->alloc, right);
      return NULL;
    }
    either->left = left;
    either->right = right;
    left = either;
  }
}

/* A bracketed selection: at least one selector, comma-separated. */
static bool json_path_bracket(json_path_parser * p, json_path_segment * seg) {
  p->at++; /* '[' */
  size_t capacity = 0;
  for (;;) {
    json_path_skip_space(p);
    if (seg->selector_count == capacity) {
      capacity = capacity ? capacity * 2 : 4;
      if (!json_path_segment_grow(p, seg, capacity)) {
        return false;
      }
    }
    if (!json_path_selector_parse(p, &seg->selectors[seg->selector_count])) {
      return false;
    }
    seg->selector_count++;
    json_path_skip_space(p);
    if (json_path_peek(p) == ',') {
      p->at++;
      continue;
    }
    if (json_path_peek(p) == ']') {
      p->at++;
      return true;
    }
    json_path_fail(p, GTEXT_JSON_E_PATH, "expected a comma or a closing bracket");
    return false;
  }
}

/* A shorthand name or a wildcard after a dot. */
static bool json_path_shorthand(json_path_parser * p, json_path_segment * seg) {
  if (!json_path_segment_grow(p, seg, 1)) {
    return false;
  }
  json_path_selector * sel = &seg->selectors[0];
  memset(sel, 0, sizeof(*sel));
  sel->step = 1;

  if (json_path_peek(p) == '*') {
    p->at++;
    sel->kind = JSON_PATH_SEL_WILDCARD;
    seg->selector_count = 1;
    return true;
  }
  if (!json_path_name_first((unsigned char)json_path_peek(p))) {
    json_path_fail(p, GTEXT_JSON_E_PATH, "expected a name after '.'");
    return false;
  }
  const size_t start = p->at;
  while (!json_path_at_end(p)
      && json_path_name_char((unsigned char)p->text[p->at])) {
    p->at++;
  }
  const size_t name_len = p->at - start;
  char * name = (char *)gtext_allocator_malloc(p->alloc, name_len + 1);
  if (!name) {
    json_path_fail(p, GTEXT_JSON_E_OOM, "out of memory");
    return false;
  }
  memcpy(name, p->text + start, name_len);
  name[name_len] = '\0';
  sel->kind = JSON_PATH_SEL_NAME;
  sel->name = name;
  sel->name_len = name_len;
  seg->selector_count = 1;
  return true;
}

static void json_path_free_query(
    const GTEXT_Allocator * alloc, json_path_query * q) {
  json_path_free_segments(alloc, q->segments, q->segment_count);
  q->segments = NULL;
  q->segment_count = 0;
}

static void json_path_free_comparable(
    const GTEXT_Allocator * alloc, json_path_comparable * c) {
  switch (c->kind) {
  case JSON_PATH_COMPARABLE_LITERAL:
    gtext_allocator_free(alloc, c->literal.string);
    c->literal.string = NULL;
    break;
  case JSON_PATH_COMPARABLE_QUERY:
    json_path_free_query(alloc, &c->query);
    break;
  case JSON_PATH_COMPARABLE_FUNCTION:
    if (c->arg) {
      json_path_free_comparable(alloc, c->arg);
      gtext_allocator_free(alloc, c->arg);
      c->arg = NULL;
    }
    if (c->arg2) {
      json_path_free_comparable(alloc, c->arg2);
      gtext_allocator_free(alloc, c->arg2);
      c->arg2 = NULL;
    }
    break;
  }
}

static void json_path_free_expr(
    const GTEXT_Allocator * alloc, json_path_expr * expr) {
  if (!expr) {
    return;
  }
  json_path_free_expr(alloc, expr->left);
  json_path_free_expr(alloc, expr->right);
  if (expr->kind == JSON_PATH_EXPR_COMPARE) {
    json_path_free_comparable(alloc, &expr->lhs);
    json_path_free_comparable(alloc, &expr->rhs);
  }
  else if (expr->kind == JSON_PATH_EXPR_TEST) {
    json_path_free_query(alloc, &expr->test);
  }
  else if (expr->kind == JSON_PATH_EXPR_LOGICAL) {
    json_path_free_comparable(alloc, &expr->logical);
  }
  gtext_allocator_free(alloc, expr);
}

static void json_path_free_segments(const GTEXT_Allocator * alloc,
    json_path_segment * segments, size_t count) {
  if (!segments) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < segments[i].selector_count; j++) {
      gtext_allocator_free(alloc, segments[i].selectors[j].name);
      json_path_free_expr(alloc, segments[i].selectors[j].filter);
    }
    gtext_allocator_free(alloc, segments[i].selectors);
  }
  gtext_allocator_free(alloc, segments);
}

GTEXT_API GTEXT_JSON_Path * gtext_json_path_compile(const char * query,
    size_t len, const GTEXT_Allocator * alloc, GTEXT_JSON_Error * err) {
  if (err) {
    memset(err, 0, sizeof(*err));
    err->line = 1;
    err->col = 1;
  }
  if (!query) {
    if (err) {
      err->code = GTEXT_JSON_E_INVALID;
      err->message = "no query";
    }
    return NULL;
  }

  json_path_parser p;
  memset(&p, 0, sizeof(p));
  p.text = query;
  p.len = (len == SIZE_MAX) ? strlen(query) : len;
  p.alloc = alloc;
  p.status = GTEXT_JSON_OK;

  json_path_segment * segments = NULL;
  size_t count = 0;
  size_t capacity = 0;

  if (json_path_peek(&p) != '$') {
    json_path_fail(&p, GTEXT_JSON_E_PATH, "a query begins with '$'");
    goto failed;
  }
  p.at++;

  for (;;) {
    /* `segments = *(S segment)` (2.1): blank space comes *before* a segment,
     * so space at the end of a query is not part of one and the query is not
     * well-formed. The suite has `$ ` as an invalid selector for exactly this,
     * and skipping to the end and stopping accepted it. */
    const size_t before_space = p.at;
    json_path_skip_space(&p);
    if (json_path_at_end(&p)) {
      if (p.at != before_space) {
        json_path_fail(
            &p, GTEXT_JSON_E_PATH, "a query does not end with blank space");
        goto failed;
      }
      break;
    }
    const char c = json_path_peek(&p);
    if (c != '.' && c != '[') {
      json_path_fail(&p, GTEXT_JSON_E_PATH, "expected '.', '..' or '['");
      goto failed;
    }

    if (count == capacity) {
      const size_t grown_capacity = capacity ? capacity * 2 : 4;
      json_path_segment * grown = (json_path_segment *)gtext_allocator_realloc(
          alloc, segments, grown_capacity * sizeof(json_path_segment));
      if (!grown) {
        json_path_fail(&p, GTEXT_JSON_E_OOM, "out of memory");
        goto failed;
      }
      segments = grown;
      capacity = grown_capacity;
    }
    json_path_segment * seg = &segments[count];
    memset(seg, 0, sizeof(*seg));

    if (c == '[') {
      if (!json_path_bracket(&p, seg)) {
        goto failed_with_segment;
      }
    }
    else {
      p.at++; /* the first '.' */
      if (json_path_peek(&p) == '.') {
        p.at++;
        seg->descendant = true;
        if (json_path_peek(&p) == '[') {
          if (!json_path_bracket(&p, seg)) {
            goto failed_with_segment;
          }
        }
        else if (!json_path_shorthand(&p, seg)) {
          goto failed_with_segment;
        }
      }
      else if (!json_path_shorthand(&p, seg)) {
        goto failed_with_segment;
      }
    }
    count++;
    continue;

  failed_with_segment:
    /* The half-built segment has to be released before the array is, or its
     * selector array and any name in it leak. */
    for (size_t j = 0; j < seg->selector_count; j++) {
      gtext_allocator_free(alloc, seg->selectors[j].name);
    }
    gtext_allocator_free(alloc, seg->selectors);
    goto failed;
  }

  {
    GTEXT_JSON_Path * path = (GTEXT_JSON_Path *)gtext_allocator_calloc(
        alloc, 1, sizeof(GTEXT_JSON_Path));
    if (!path) {
      json_path_fail(&p, GTEXT_JSON_E_OOM, "out of memory");
      goto failed;
    }
    path->alloc = alloc;
    path->segments = segments;
    path->segment_count = count;
    return path;
  }

failed:
  json_path_free_segments(alloc, segments, count);
  if (err) {
    err->code = p.status == GTEXT_JSON_OK ? GTEXT_JSON_E_PATH : p.status;
    err->message = p.message ? p.message : "invalid query";
    err->offset = p.at;
  }
  return NULL;
}

GTEXT_API void gtext_json_path_free(GTEXT_JSON_Path * path) {
  if (!path) {
    return;
  }
  const GTEXT_Allocator * alloc = path->alloc;
  json_path_free_segments(alloc, path->segments, path->segment_count);
  gtext_allocator_free(alloc, path);
}

/* ======================================================================
 * Evaluation
 * ====================================================================== */

/*
 * A node list, optionally carrying the normalized path of each node (2.7).
 *
 * The paths are opt-in because they cost an allocation and a copy per selected
 * node, and a caller who only wants the values should not pay for them. A
 * filter's sub-queries never ask for them: they are evaluated for a truth value
 * or a single value, and nothing asks where that came from.
 */
typedef struct {
  const GTEXT_JSON_Value ** items;
  char ** paths; /* NULL unless want_paths; each entry owned by the list */
  size_t count;
  size_t capacity;
  bool want_paths;
  const GTEXT_Allocator * alloc;
} json_path_list;

/* Push a node and, where the list wants paths, the path that reached it - which
 * the list takes ownership of, including when the push fails. */
static bool json_path_list_push_path(
    json_path_list * list, const GTEXT_JSON_Value * node, char * path) {
  if (list->count == list->capacity) {
    const size_t capacity = list->capacity ? list->capacity * 2 : 8;
    const GTEXT_JSON_Value ** grown =
        (const GTEXT_JSON_Value **)gtext_allocator_realloc(list->alloc,
            (void *)list->items, capacity * sizeof(const GTEXT_JSON_Value *));
    if (!grown) {
      gtext_allocator_free(list->alloc, path);
      return false;
    }
    list->items = grown;
    if (list->want_paths) {
      char ** grown_paths = (char **)gtext_allocator_realloc(
          list->alloc, list->paths, capacity * sizeof(char *));
      if (!grown_paths) {
        gtext_allocator_free(list->alloc, path);
        return false;
      }
      list->paths = grown_paths;
    }
    list->capacity = capacity;
  }
  if (list->want_paths) {
    if (!path) {
      return false; /* the caller could not build it: out of memory */
    }
    list->paths[list->count] = path;
  }
  else {
    gtext_allocator_free(list->alloc, path);
  }
  list->items[list->count++] = node;
  return true;
}

static void json_path_list_free(json_path_list * list) {
  if (list->paths) {
    for (size_t i = 0; i < list->count; i++) {
      gtext_allocator_free(list->alloc, list->paths[i]);
    }
    gtext_allocator_free(list->alloc, list->paths);
    list->paths = NULL;
  }
  gtext_allocator_free(list->alloc, (void *)list->items);
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

/* ---------------------------------------------------------------------- *
 * Normalized paths (2.7)
 *
 * A normalized path names one node: "$" then a bracketed index or
 * single-quoted name per step, so `$['a'][0]['b c']`. It is itself a valid
 * JSONPath query, and it is the only spelling of a path the specification
 * blesses - which is what makes it useful as an identity for a result.
 * ---------------------------------------------------------------------- */

static char * json_path_path_root(const GTEXT_Allocator * alloc) {
  char * out = (char *)gtext_allocator_malloc(alloc, 2);
  if (!out) {
    return NULL;
  }
  out[0] = '$';
  out[1] = '\0';
  return out;
}

static char * json_path_path_dup(
    const GTEXT_Allocator * alloc, const char * path) {
  if (!path) {
    return NULL;
  }
  const size_t len = strlen(path);
  char * out = (char *)gtext_allocator_malloc(alloc, len + 1);
  if (!out) {
    return NULL;
  }
  memcpy(out, path, len + 1);
  return out;
}

static char * json_path_path_index(
    const GTEXT_Allocator * alloc, const char * parent, size_t index) {
  if (!parent) {
    return NULL;
  }
  char digits[24];
  const int written = snprintf(digits, sizeof(digits), "[%zu]", index);
  if (written < 0) {
    return NULL;
  }
  const size_t parent_len = strlen(parent);
  char * out =
      (char *)gtext_allocator_malloc(alloc, parent_len + (size_t)written + 1);
  if (!out) {
    return NULL;
  }
  memcpy(out, parent, parent_len);
  memcpy(out + parent_len, digits, (size_t)written + 1);
  return out;
}

/* normal-name-selector: single-quoted, with 2.7's escapes and \uXXXX in
 * lowercase hex for the control characters that have no shorter spelling. */
static char * json_path_path_name(const GTEXT_Allocator * alloc,
    const char * parent, const char * name, size_t name_len) {
  if (!parent) {
    return NULL;
  }
  const size_t parent_len = strlen(parent);
  /* Worst case per byte is six, for \uXXXX, plus the two quotes, the two
   * brackets and the terminator. */
  char * out = (char *)gtext_allocator_malloc(
      alloc, parent_len + name_len * 6 + 5);
  if (!out) {
    return NULL;
  }
  memcpy(out, parent, parent_len);
  size_t at = parent_len;
  out[at++] = '[';
  out[at++] = '\'';
  for (size_t i = 0; i < name_len; i++) {
    const unsigned char c = (unsigned char)name[i];
    switch (c) {
    case '\'':
      out[at++] = '\\';
      out[at++] = '\'';
      continue;
    case '\\':
      out[at++] = '\\';
      out[at++] = '\\';
      continue;
    case '\b':
      out[at++] = '\\';
      out[at++] = 'b';
      continue;
    case '\f':
      out[at++] = '\\';
      out[at++] = 'f';
      continue;
    case '\n':
      out[at++] = '\\';
      out[at++] = 'n';
      continue;
    case '\r':
      out[at++] = '\\';
      out[at++] = 'r';
      continue;
    case '\t':
      out[at++] = '\\';
      out[at++] = 't';
      continue;
    default:
      break;
    }
    if (c < 0x20) {
      static const char hex[] = "0123456789abcdef";
      out[at++] = '\\';
      out[at++] = 'u';
      out[at++] = '0';
      out[at++] = '0';
      out[at++] = hex[(c >> 4) & 0xF];
      out[at++] = hex[c & 0xF];
      continue;
    }
    out[at++] = (char)c;
  }
  out[at++] = '\'';
  out[at++] = ']';
  out[at] = '\0';
  return out;
}

/*
 * The bounds of a slice (2.3.4.2.2), which is Python's rule written out.
 *
 * A negative bound counts from the end; a negative step runs backwards and
 * swaps which end is inclusive. A step of zero selects nothing, which the
 * specification says explicitly rather than leaving to a division.
 */
static void json_path_slice_bounds(const json_path_selector * sel, size_t len,
    int64_t * out_lower, int64_t * out_upper) {
  const int64_t length = (int64_t)len;
  const int64_t step = sel->step;

  int64_t start = sel->has_start ? sel->start : (step >= 0 ? 0 : length - 1);
  int64_t end = sel->has_end ? sel->end : (step >= 0 ? length : -length - 1);

  if (start < 0) {
    start += length;
  }
  if (end < 0) {
    end += length;
  }

  if (step >= 0) {
    if (start < 0) {
      start = 0;
    }
    if (start > length) {
      start = length;
    }
    if (end < 0) {
      end = 0;
    }
    if (end > length) {
      end = length;
    }
  }
  else {
    if (start < -1) {
      start = -1;
    }
    if (start > length - 1) {
      start = length - 1;
    }
    if (end < -1) {
      end = -1;
    }
    if (end > length - 1) {
      end = length - 1;
    }
  }
  *out_lower = start;
  *out_upper = end;
}

/* ---------------------------------------------------------------------- *
 * Evaluating a filter expression
 * ---------------------------------------------------------------------- */

/* What a comparable evaluates to (2.4.1's ValueType): nothing at all, a node
 * from the document, or a value this evaluator made - a literal from the query,
 * or a number from length() or count(). */
typedef enum {
  JSON_PATH_FV_NOTHING,
  JSON_PATH_FV_NODE,
  JSON_PATH_FV_NUMBER,
  JSON_PATH_FV_STRING,
  JSON_PATH_FV_BOOL,
  JSON_PATH_FV_NULL
} json_path_fv_kind;

typedef struct {
  json_path_fv_kind kind;
  const GTEXT_JSON_Value * node;
  double number;
  int64_t integer;
  bool has_integer;
  const char * string;
  size_t string_len;
  bool boolean;
} json_path_fv;

/* What the evaluator needs that a node does not carry: the document's root,
 * for a filter's absolute queries, and the allocator. */
typedef struct {
  const GTEXT_JSON_Value * root;
  const GTEXT_Allocator * alloc;
  GTEXT_JSON_Status status;
} json_path_eval;

static GTEXT_JSON_Status json_path_apply_segments(json_path_eval * ev,
    const json_path_segment * segments, size_t count,
    const GTEXT_JSON_Value * start, json_path_list * out);

/* A node as a comparable value, so that a literal and a document value are
 * compared by the same code. */
static json_path_fv json_path_fv_of_node(const GTEXT_JSON_Value * node) {
  json_path_fv fv;
  memset(&fv, 0, sizeof(fv));
  if (!node) {
    fv.kind = JSON_PATH_FV_NOTHING;
    return fv;
  }
  switch (gtext_json_typeof(node)) {
  case GTEXT_JSON_NUMBER: {
    fv.kind = JSON_PATH_FV_NUMBER;
    int64_t i64 = 0;
    if (gtext_json_get_i64(node, &i64) == GTEXT_JSON_OK) {
      fv.has_integer = true;
      fv.integer = i64;
      fv.number = (double)i64;
    }
    double d = 0.0;
    if (gtext_json_get_double(node, &d) == GTEXT_JSON_OK) {
      fv.number = d;
    }
    fv.node = node;
    return fv;
  }
  case GTEXT_JSON_STRING: {
    fv.kind = JSON_PATH_FV_STRING;
    const char * s = NULL;
    size_t len = 0;
    gtext_json_get_string(node, &s, &len);
    fv.string = s;
    fv.string_len = len;
    fv.node = node;
    return fv;
  }
  case GTEXT_JSON_BOOL: {
    fv.kind = JSON_PATH_FV_BOOL;
    bool b = false;
    gtext_json_get_bool(node, &b);
    fv.boolean = b;
    fv.node = node;
    return fv;
  }
  case GTEXT_JSON_NULL:
    fv.kind = JSON_PATH_FV_NULL;
    fv.node = node;
    return fv;
  default:
    /* An array or an object: comparable only as a whole, by deep equality. */
    fv.kind = JSON_PATH_FV_NODE;
    fv.node = node;
    return fv;
  }
}

/*
 * Deep equality of two nodes (2.3.5.2.2): arrays elementwise in order, objects
 * by name with order ignored, everything else by value.
 *
 * Iterative, with a stack of pairs, for the reason the descendant walk is: the
 * depth is the caller's document's.
 */
typedef struct {
  const GTEXT_JSON_Value * a;
  const GTEXT_JSON_Value * b;
} json_path_pair;

static bool json_path_scalar_equal(
    const GTEXT_JSON_Value * a, const GTEXT_JSON_Value * b);

static bool json_path_deep_equal(json_path_eval * ev,
    const GTEXT_JSON_Value * a, const GTEXT_JSON_Value * b) {
  json_path_pair * stack = NULL;
  size_t count = 0;
  size_t capacity = 0;
  bool equal = true;

  const json_path_pair first = {a, b};
  json_path_pair * grown = (json_path_pair *)gtext_allocator_realloc(
      ev->alloc, stack, 8 * sizeof(json_path_pair));
  if (!grown) {
    ev->status = GTEXT_JSON_E_OOM;
    return false;
  }
  stack = grown;
  capacity = 8;
  stack[count++] = first;

  while (count > 0 && equal) {
    const json_path_pair pair = stack[--count];
    const GTEXT_JSON_Type ta = gtext_json_typeof(pair.a);
    const GTEXT_JSON_Type tb = gtext_json_typeof(pair.b);
    if (ta != tb) {
      equal = false;
      break;
    }
    if (ta == GTEXT_JSON_ARRAY) {
      const size_t na = gtext_json_array_size(pair.a);
      if (na != gtext_json_array_size(pair.b)) {
        equal = false;
        break;
      }
      for (size_t i = 0; i < na; i++) {
        if (count == capacity) {
          const size_t next_capacity = capacity * 2;
          json_path_pair * bigger = (json_path_pair *)gtext_allocator_realloc(
              ev->alloc, stack, next_capacity * sizeof(json_path_pair));
          if (!bigger) {
            ev->status = GTEXT_JSON_E_OOM;
            equal = false;
            break;
          }
          stack = bigger;
          capacity = next_capacity;
        }
        stack[count].a = gtext_json_array_get(pair.a, i);
        stack[count].b = gtext_json_array_get(pair.b, i);
        count++;
      }
      continue;
    }
    if (ta == GTEXT_JSON_OBJECT) {
      const size_t na = gtext_json_object_size(pair.a);
      if (na != gtext_json_object_size(pair.b)) {
        equal = false;
        break;
      }
      for (size_t i = 0; i < na; i++) {
        size_t key_len = 0;
        const char * key = gtext_json_object_key(pair.a, i, &key_len);
        const GTEXT_JSON_Value * other =
            gtext_json_object_get(pair.b, key, key_len);
        if (!other) {
          equal = false; /* a name the other object does not have */
          break;
        }
        if (count == capacity) {
          const size_t next_capacity = capacity * 2;
          json_path_pair * bigger = (json_path_pair *)gtext_allocator_realloc(
              ev->alloc, stack, next_capacity * sizeof(json_path_pair));
          if (!bigger) {
            ev->status = GTEXT_JSON_E_OOM;
            equal = false;
            break;
          }
          stack = bigger;
          capacity = next_capacity;
        }
        stack[count].a = gtext_json_object_value(pair.a, i);
        stack[count].b = other;
        count++;
      }
      continue;
    }
    if (!json_path_scalar_equal(pair.a, pair.b)) {
      equal = false;
    }
  }

  gtext_allocator_free(ev->alloc, stack);
  return equal;
}

static bool json_path_scalar_equal(
    const GTEXT_JSON_Value * a, const GTEXT_JSON_Value * b) {
  switch (gtext_json_typeof(a)) {
  case GTEXT_JSON_NULL:
    return true;
  case GTEXT_JSON_BOOL: {
    bool ba = false;
    bool bb = false;
    gtext_json_get_bool(a, &ba);
    gtext_json_get_bool(b, &bb);
    return ba == bb;
  }
  case GTEXT_JSON_STRING: {
    const char * sa = NULL;
    const char * sb = NULL;
    size_t la = 0;
    size_t lb = 0;
    gtext_json_get_string(a, &sa, &la);
    gtext_json_get_string(b, &sb, &lb);
    return la == lb && memcmp(sa, sb, la) == 0;
  }
  case GTEXT_JSON_NUMBER: {
    int64_t ia = 0;
    int64_t ib = 0;
    if (gtext_json_get_i64(a, &ia) == GTEXT_JSON_OK
        && gtext_json_get_i64(b, &ib) == GTEXT_JSON_OK) {
      return ia == ib;
    }
    double da = 0.0;
    double db = 0.0;
    if (gtext_json_get_double(a, &da) != GTEXT_JSON_OK
        || gtext_json_get_double(b, &db) != GTEXT_JSON_OK) {
      return false;
    }
    return da == db;
  }
  default:
    return false;
  }
}

/* Two comparables, compared as 2.3.5.2.2 says.
 *
 * Nothing equals only Nothing, and orders against nothing at all - which makes
 * every ordering comparison against a missing member false, both ways round.
 * Numbers compare by value, exactly where both have an int64. Strings compare
 * by their bytes, which for well-formed UTF-8 is codepoint order. Arrays and
 * objects compare only for equality. A number against a string is unequal and
 * unordered rather than an error. */
static bool json_path_compare(json_path_eval * ev, json_path_cmp_op op,
    const json_path_fv * a, const json_path_fv * b) {
  const bool a_nothing = a->kind == JSON_PATH_FV_NOTHING;
  const bool b_nothing = b->kind == JSON_PATH_FV_NOTHING;
  if (a_nothing || b_nothing) {
    switch (op) {
    case JSON_PATH_CMP_EQ:
      return a_nothing && b_nothing;
    case JSON_PATH_CMP_NE:
      return !(a_nothing && b_nothing);
    default:
      return false; /* unordered */
    }
  }

  int order = 0;      /* -1, 0, 1 when comparable */
  bool ordered = false;
  bool equal = false;

  if (a->kind == JSON_PATH_FV_NUMBER && b->kind == JSON_PATH_FV_NUMBER) {
    if (a->has_integer && b->has_integer) {
      order = (a->integer < b->integer) ? -1 : (a->integer > b->integer ? 1 : 0);
    }
    else {
      order = (a->number < b->number) ? -1 : (a->number > b->number ? 1 : 0);
    }
    ordered = true;
    equal = order == 0;
  }
  else if (a->kind == JSON_PATH_FV_STRING && b->kind == JSON_PATH_FV_STRING) {
    const size_t shortest = a->string_len < b->string_len ? a->string_len
                                                          : b->string_len;
    int cmp = shortest ? memcmp(a->string, b->string, shortest) : 0;
    if (cmp == 0) {
      cmp = (a->string_len < b->string_len)
          ? -1
          : (a->string_len > b->string_len ? 1 : 0);
    }
    order = cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
    ordered = true;
    equal = order == 0;
  }
  else if (a->kind == JSON_PATH_FV_NULL && b->kind == JSON_PATH_FV_NULL) {
    equal = true;
  }
  else if (a->kind == JSON_PATH_FV_BOOL && b->kind == JSON_PATH_FV_BOOL) {
    equal = a->boolean == b->boolean;
  }
  else if (a->kind == JSON_PATH_FV_NODE && b->kind == JSON_PATH_FV_NODE) {
    equal = json_path_deep_equal(ev, a->node, b->node);
  }

  switch (op) {
  case JSON_PATH_CMP_EQ:
    return equal;
  case JSON_PATH_CMP_NE:
    return !equal;
  case JSON_PATH_CMP_LT:
    return ordered && order < 0;
  case JSON_PATH_CMP_GT:
    return ordered && order > 0;
  /* 2.3.5.2.2 defines these as "less than or equal" rather than as an ordering
   * of their own, and that matters where two values are equal but unordered:
   * `null <= null` and `true >= true` are true, and asking for an order first
   * made them false. Six compliance cases. */
  case JSON_PATH_CMP_LE:
    return equal || (ordered && order < 0);
  case JSON_PATH_CMP_GE:
    return equal || (ordered && order > 0);
  }
  return false;
}

/* The node list a query inside a filter selects, with @ bound to `current`. */
static GTEXT_JSON_Status json_path_run_query(json_path_eval * ev,
    const json_path_query * query, const GTEXT_JSON_Value * current,
    json_path_list * out) {
  const GTEXT_JSON_Value * start = query->relative ? current : ev->root;
  return json_path_apply_segments(
      ev, query->segments, query->segment_count, start, out);
}

/* The number of Unicode scalar values in a UTF-8 string, which is what
 * length() counts for a string (2.4.4) - not its bytes. A byte that is not a
 * continuation byte begins a character. */
static size_t json_path_codepoint_count(const char * s, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    if (((unsigned char)s[i] & 0xC0) != 0x80) {
      n++;
    }
  }
  return n;
}

static json_path_fv json_path_eval_comparable(json_path_eval * ev,
    const json_path_comparable * c, const GTEXT_JSON_Value * current);

static json_path_fv json_path_eval_function(json_path_eval * ev,
    const json_path_comparable * c, const GTEXT_JSON_Value * current) {
  json_path_fv result;
  memset(&result, 0, sizeof(result));
  result.kind = JSON_PATH_FV_NOTHING;

  switch (c->fn) {
  case JSON_PATH_FN_COUNT: {
    json_path_list nodes;
    memset(&nodes, 0, sizeof(nodes));
    nodes.alloc = ev->alloc;
    const GTEXT_JSON_Status status =
        json_path_run_query(ev, &c->arg->query, current, &nodes);
    if (status != GTEXT_JSON_OK) {
      ev->status = status;
      json_path_list_free(&nodes);
      return result;
    }
    result.kind = JSON_PATH_FV_NUMBER;
    result.has_integer = true;
    result.integer = (int64_t)nodes.count;
    result.number = (double)nodes.count;
    json_path_list_free(&nodes);
    return result;
  }

  case JSON_PATH_FN_VALUE: {
    /* 2.4.8: one node gives its value, any other count gives Nothing. */
    json_path_list nodes;
    memset(&nodes, 0, sizeof(nodes));
    nodes.alloc = ev->alloc;
    const GTEXT_JSON_Status status =
        json_path_run_query(ev, &c->arg->query, current, &nodes);
    if (status != GTEXT_JSON_OK) {
      ev->status = status;
      json_path_list_free(&nodes);
      return result;
    }
    if (nodes.count == 1) {
      result = json_path_fv_of_node(nodes.items[0]);
    }
    json_path_list_free(&nodes);
    return result;
  }

  case JSON_PATH_FN_LENGTH: {
    const json_path_fv arg = json_path_eval_comparable(ev, c->arg, current);
    switch (arg.kind) {
    case JSON_PATH_FV_STRING:
      result.kind = JSON_PATH_FV_NUMBER;
      result.has_integer = true;
      result.integer =
          (int64_t)json_path_codepoint_count(arg.string, arg.string_len);
      result.number = (double)result.integer;
      return result;
    case JSON_PATH_FV_NODE: {
      const GTEXT_JSON_Type type = gtext_json_typeof(arg.node);
      if (type == GTEXT_JSON_ARRAY) {
        result.kind = JSON_PATH_FV_NUMBER;
        result.has_integer = true;
        result.integer = (int64_t)gtext_json_array_size(arg.node);
        result.number = (double)result.integer;
      }
      else if (type == GTEXT_JSON_OBJECT) {
        result.kind = JSON_PATH_FV_NUMBER;
        result.has_integer = true;
        result.integer = (int64_t)gtext_json_object_size(arg.node);
        result.number = (double)result.integer;
      }
      return result;
    }
    default:
      /* A number, a boolean, a null, or Nothing: length() is Nothing. */
      return result;
    }
  }

  case JSON_PATH_FN_MATCH:
  case JSON_PATH_FN_SEARCH:
    /* LogicalType. A comparison is refused at compile time, so evaluation of
     * these goes through json_path_logical_call, not through here. */
    break;
  case JSON_PATH_FN_NONE:
    break;
  }
  return result;
}

/* match() or search() for one node. A subject or pattern that is not a
 * string, and a pattern that is not I-Regexp, are LogicalFalse (RFC 9535
 * §2.4.6, §2.4.7). GRX_ERR_LIMIT and GRX_ERR_OOM are not a Boolean: the
 * question was not answered, and ev->status carries that out. */
static bool json_path_logical_call(json_path_eval * ev,
    const json_path_comparable * call, const GTEXT_JSON_Value * current) {
  const json_path_fv subject = json_path_eval_comparable(ev, call->arg, current);
  if (ev->status != GTEXT_JSON_OK) {
    return false;
  }
  const json_path_fv pattern =
      json_path_eval_comparable(ev, call->arg2, current);
  if (ev->status != GTEXT_JSON_OK) {
    return false;
  }
  if (subject.kind != JSON_PATH_FV_STRING || pattern.kind != JSON_PATH_FV_STRING) {
    return false;
  }

  /* match is the whole string. search is any substring. The dialect anchors
   * nothing; both questions are the same pattern and two option settings.
   * GRX_OPT_UTF is the dialect's default and is applied by the parser. */
  const uint32_t options = call->fn == JSON_PATH_FN_MATCH
      ? (uint32_t)(GRX_OPT_ANCHORED | GRX_OPT_ANCHORED_END)
      : (uint32_t)GRX_OPT_NONE;
  GRX_Regex * regex = NULL;
  const GRX_Result compiled = grx_regex_compile_with_allocator(pattern.string,
      pattern.string_len, GRX_SYNTAX_IREGEXP, options, NULL,
      (const GRX_Allocator *)ev->alloc, NULL, &regex);
  if (compiled == GRX_ERR_SYNTAX || compiled == GRX_ERR_INVALID) {
    return false;
  }
  if (compiled != GRX_OK) {
    ev->status = compiled == GRX_ERR_OOM ? GTEXT_JSON_E_OOM : GTEXT_JSON_E_LIMIT;
    return false;
  }

  int matched = 0;
  const GRX_Result searched = grx_regex_search(regex, subject.string,
      subject.string_len, 0, GRX_ENGINE_AUTO, NULL, NULL, &matched);
  grx_regex_free(regex);
  if (searched != GRX_OK) {
    ev->status = searched == GRX_ERR_OOM ? GTEXT_JSON_E_OOM : GTEXT_JSON_E_LIMIT;
    return false;
  }
  return matched != 0;
}

static json_path_fv json_path_eval_comparable(json_path_eval * ev,
    const json_path_comparable * c, const GTEXT_JSON_Value * current) {
  json_path_fv result;
  memset(&result, 0, sizeof(result));
  result.kind = JSON_PATH_FV_NOTHING;

  switch (c->kind) {
  case JSON_PATH_COMPARABLE_LITERAL:
    switch (c->literal.kind) {
    case JSON_PATH_LIT_NUMBER:
      result.kind = JSON_PATH_FV_NUMBER;
      result.number = c->literal.number;
      result.has_integer = c->literal.has_integer;
      result.integer = c->literal.integer;
      return result;
    case JSON_PATH_LIT_STRING:
      result.kind = JSON_PATH_FV_STRING;
      result.string = c->literal.string;
      result.string_len = c->literal.string_len;
      return result;
    case JSON_PATH_LIT_TRUE:
      result.kind = JSON_PATH_FV_BOOL;
      result.boolean = true;
      return result;
    case JSON_PATH_LIT_FALSE:
      result.kind = JSON_PATH_FV_BOOL;
      result.boolean = false;
      return result;
    case JSON_PATH_LIT_NULL:
      result.kind = JSON_PATH_FV_NULL;
      return result;
    }
    return result;

  case JSON_PATH_COMPARABLE_QUERY: {
    /* A singular query: one node or none. */
    json_path_list nodes;
    memset(&nodes, 0, sizeof(nodes));
    nodes.alloc = ev->alloc;
    const GTEXT_JSON_Status status =
        json_path_run_query(ev, &c->query, current, &nodes);
    if (status != GTEXT_JSON_OK) {
      ev->status = status;
      json_path_list_free(&nodes);
      return result;
    }
    if (nodes.count == 1) {
      result = json_path_fv_of_node(nodes.items[0]);
    }
    json_path_list_free(&nodes);
    return result;
  }

  case JSON_PATH_COMPARABLE_FUNCTION:
    return json_path_eval_function(ev, c, current);
  }
  return result;
}

/* Is the expression true for this node? */
static bool json_path_eval_expr(json_path_eval * ev,
    const json_path_expr * expr, const GTEXT_JSON_Value * current) {
  if (!expr || ev->status != GTEXT_JSON_OK) {
    return false;
  }
  switch (expr->kind) {
  case JSON_PATH_EXPR_OR:
    /* Short-circuiting is not observable - no side effects - but it saves the
     * work of a second query. */
    return json_path_eval_expr(ev, expr->left, current)
        || json_path_eval_expr(ev, expr->right, current);
  case JSON_PATH_EXPR_AND:
    return json_path_eval_expr(ev, expr->left, current)
        && json_path_eval_expr(ev, expr->right, current);
  case JSON_PATH_EXPR_NOT:
    return !json_path_eval_expr(ev, expr->left, current);
  case JSON_PATH_EXPR_LOGICAL:
    return json_path_logical_call(ev, &expr->logical, current);
  case JSON_PATH_EXPR_TEST: {
    json_path_list nodes;
    memset(&nodes, 0, sizeof(nodes));
    nodes.alloc = ev->alloc;
    const GTEXT_JSON_Status status =
        json_path_run_query(ev, &expr->test, current, &nodes);
    if (status != GTEXT_JSON_OK) {
      ev->status = status;
      json_path_list_free(&nodes);
      return false;
    }
    const bool any = nodes.count > 0;
    json_path_list_free(&nodes);
    return any;
  }
  case JSON_PATH_EXPR_COMPARE: {
    const json_path_fv a = json_path_eval_comparable(ev, &expr->lhs, current);
    const json_path_fv b = json_path_eval_comparable(ev, &expr->rhs, current);
    return json_path_compare(ev, expr->op, &a, &b);
  }
  }
  return false;
}

/* Apply one selector to one node, appending what it selects. */
static GTEXT_JSON_Status json_path_apply_selector(json_path_eval * ev,
    const json_path_selector * sel, const GTEXT_JSON_Value * node,
    const char * node_path, json_path_list * out) {
  const GTEXT_JSON_Type type = gtext_json_typeof(node);
  const GTEXT_Allocator * alloc = ev->alloc;
  /* Building a path is skipped entirely where the list does not want one, so
   * the common case pays nothing. */
  const bool paths = out->want_paths;

  switch (sel->kind) {
  case JSON_PATH_SEL_FILTER: {
    /* 2.3.5.2: the expression is applied to each *element* or *member value*,
     * with @ bound to it - never to the node the selector is applied to. A
     * scalar has neither, so it selects nothing. */
    if (type == GTEXT_JSON_ARRAY) {
      const size_t len = gtext_json_array_size(node);
      for (size_t i = 0; i < len; i++) {
        const GTEXT_JSON_Value * child = gtext_json_array_get(node, i);
        if (json_path_eval_expr(ev, sel->filter, child)) {
          if (!json_path_list_push_path(out, child,
                  paths ? json_path_path_index(alloc, node_path, i) : NULL)) {
            return GTEXT_JSON_E_OOM;
          }
        }
        if (ev->status != GTEXT_JSON_OK) {
          return ev->status;
        }
      }
    }
    else if (type == GTEXT_JSON_OBJECT) {
      const size_t len = gtext_json_object_size(node);
      for (size_t i = 0; i < len; i++) {
        const GTEXT_JSON_Value * child = gtext_json_object_value(node, i);
        if (json_path_eval_expr(ev, sel->filter, child)) {
          size_t key_len = 0;
          const char * key = gtext_json_object_key(node, i, &key_len);
          if (!json_path_list_push_path(out, child,
                  paths ? json_path_path_name(alloc, node_path, key, key_len)
                        : NULL)) {
            return GTEXT_JSON_E_OOM;
          }
        }
        if (ev->status != GTEXT_JSON_OK) {
          return ev->status;
        }
      }
    }
    return GTEXT_JSON_OK;
  }

  case JSON_PATH_SEL_NAME: {
    if (type != GTEXT_JSON_OBJECT) {
      return GTEXT_JSON_OK;
    }
    const GTEXT_JSON_Value * found =
        gtext_json_object_get(node, sel->name, sel->name_len);
    if (found
        && !json_path_list_push_path(out, found,
            paths ? json_path_path_name(
                        alloc, node_path, sel->name, sel->name_len)
                  : NULL)) {
      return GTEXT_JSON_E_OOM;
    }
    return GTEXT_JSON_OK;
  }

  case JSON_PATH_SEL_INDEX: {
    if (type != GTEXT_JSON_ARRAY) {
      return GTEXT_JSON_OK;
    }
    const size_t len = gtext_json_array_size(node);
    int64_t index = sel->index;
    if (index < 0) {
      index += (int64_t)len;
    }
    if (index < 0 || index >= (int64_t)len) {
      return GTEXT_JSON_OK;
    }
    const GTEXT_JSON_Value * found = gtext_json_array_get(node, (size_t)index);
    /* The path carries the *resolved* index: a normalized path has no negative
     * index, so `$[-1]` on a three-element array is `$[2]`. */
    if (found
        && !json_path_list_push_path(out, found,
            paths ? json_path_path_index(alloc, node_path, (size_t)index)
                  : NULL)) {
      return GTEXT_JSON_E_OOM;
    }
    return GTEXT_JSON_OK;
  }

  case JSON_PATH_SEL_WILDCARD: {
    if (type == GTEXT_JSON_ARRAY) {
      const size_t len = gtext_json_array_size(node);
      for (size_t i = 0; i < len; i++) {
        if (!json_path_list_push_path(out, gtext_json_array_get(node, i),
                paths ? json_path_path_index(alloc, node_path, i) : NULL)) {
          return GTEXT_JSON_E_OOM;
        }
      }
    }
    else if (type == GTEXT_JSON_OBJECT) {
      /* 2.3.2.2: the order of an object's members is implementation-defined,
       * and this DOM keeps them in document order, so that is what comes out. */
      const size_t len = gtext_json_object_size(node);
      for (size_t i = 0; i < len; i++) {
        size_t key_len = 0;
        const char * key = gtext_json_object_key(node, i, &key_len);
        if (!json_path_list_push_path(out, gtext_json_object_value(node, i),
                paths ? json_path_path_name(alloc, node_path, key, key_len)
                      : NULL)) {
          return GTEXT_JSON_E_OOM;
        }
      }
    }
    return GTEXT_JSON_OK;
  }

  case JSON_PATH_SEL_SLICE: {
    if (type != GTEXT_JSON_ARRAY) {
      return GTEXT_JSON_OK;
    }
    if (sel->step == 0) {
      return GTEXT_JSON_OK; /* 2.3.4.2.2: a zero step selects nothing. */
    }
    const size_t len = gtext_json_array_size(node);
    int64_t lower = 0;
    int64_t upper = 0;
    json_path_slice_bounds(sel, len, &lower, &upper);
    if (sel->step > 0) {
      for (int64_t i = lower; i < upper; i += sel->step) {
        if (!json_path_list_push_path(out,
                gtext_json_array_get(node, (size_t)i),
                paths ? json_path_path_index(alloc, node_path, (size_t)i)
                      : NULL)) {
          return GTEXT_JSON_E_OOM;
        }
      }
    }
    else {
      for (int64_t i = lower; i > upper; i += sel->step) {
        if (!json_path_list_push_path(out,
                gtext_json_array_get(node, (size_t)i),
                paths ? json_path_path_index(alloc, node_path, (size_t)i)
                      : NULL)) {
          return GTEXT_JSON_E_OOM;
        }
      }
    }
    return GTEXT_JSON_OK;
  }
  }
  return GTEXT_JSON_OK;
}

/*
 * Every node at or under `root`, in document order, appended to `out`.
 *
 * An explicit stack rather than recursion: the depth here is the caller's
 * document's depth, and a query is not a reason to put that on the C stack.
 * Children are pushed in reverse so they are visited front to back.
 */
static GTEXT_JSON_Status json_path_collect_descendants(json_path_eval * ev,
    const GTEXT_JSON_Value * root, const char * root_path,
    json_path_list * out) {
  const GTEXT_Allocator * alloc = out->alloc;
  const bool paths = out->want_paths;

  json_path_list stack;
  memset(&stack, 0, sizeof(stack));
  stack.alloc = alloc;
  stack.want_paths = paths;

  char * first_path = paths ? json_path_path_dup(alloc, root_path) : NULL;
  if (paths && !first_path) {
    return GTEXT_JSON_E_OOM;
  }
  if (!json_path_list_push_path(&stack, root, first_path)) {
    json_path_list_free(&stack);
    return GTEXT_JSON_E_OOM;
  }

  GTEXT_JSON_Status status = GTEXT_JSON_OK;
  while (stack.count > 0) {
    stack.count--;
    const GTEXT_JSON_Value * node = stack.items[stack.count];
    /* The path moves from the stack to the output, so the stack must not free
     * it: this is the one place a path changes owner. */
    char * node_path = paths ? stack.paths[stack.count] : NULL;
    if (paths) {
      stack.paths[stack.count] = NULL;
    }
    const GTEXT_JSON_Type type = gtext_json_typeof(node);

    /* The children are pushed before the node's own path is handed over,
     * because building theirs reads it. */
    if (type == GTEXT_JSON_ARRAY) {
      const size_t len = gtext_json_array_size(node);
      for (size_t i = len; i > 0; i--) {
        if (!json_path_list_push_path(&stack, gtext_json_array_get(node, i - 1),
                paths ? json_path_path_index(alloc, node_path, i - 1) : NULL)) {
          status = GTEXT_JSON_E_OOM;
          break;
        }
      }
    }
    else if (type == GTEXT_JSON_OBJECT) {
      const size_t len = gtext_json_object_size(node);
      for (size_t i = len; i > 0; i--) {
        size_t key_len = 0;
        const char * key = gtext_json_object_key(node, i - 1, &key_len);
        if (!json_path_list_push_path(&stack,
                gtext_json_object_value(node, i - 1),
                paths ? json_path_path_name(alloc, node_path, key, key_len)
                      : NULL)) {
          status = GTEXT_JSON_E_OOM;
          break;
        }
      }
    }

    if (status == GTEXT_JSON_OK
        && !json_path_list_push_path(out, node, node_path)) {
      status = GTEXT_JSON_E_OOM;
    }
    else if (status != GTEXT_JSON_OK) {
      gtext_allocator_free(alloc, node_path);
    }
    if (status != GTEXT_JSON_OK) {
      break;
    }
  }

  json_path_list_free(&stack);
  (void)ev;
  return status;
}

/*
 * Apply a run of segments to one starting node, one segment at a time over a
 * node list. Shared by the top-level query and by the queries inside a filter,
 * which is what makes a nested filter work at all: `$[?@.a[?@.b]]` is this
 * function calling the filter evaluator calling this function.
 */
static GTEXT_JSON_Status json_path_apply_segments(json_path_eval * ev,
    const json_path_segment * segments, size_t count,
    const GTEXT_JSON_Value * start, json_path_list * out) {
  const bool paths = out->want_paths;

  json_path_list current;
  memset(&current, 0, sizeof(current));
  current.alloc = ev->alloc;
  current.want_paths = paths;
  {
    char * root_path = paths ? json_path_path_root(ev->alloc) : NULL;
    if (paths && !root_path) {
      return GTEXT_JSON_E_OOM;
    }
    if (!json_path_list_push_path(&current, start, root_path)) {
      json_path_list_free(&current);
      return GTEXT_JSON_E_OOM;
    }
  }

  GTEXT_JSON_Status status = GTEXT_JSON_OK;
  for (size_t s = 0; s < count && status == GTEXT_JSON_OK; s++) {
    const json_path_segment * seg = &segments[s];
    json_path_list next;
    memset(&next, 0, sizeof(next));
    next.alloc = ev->alloc;
    next.want_paths = paths;

    for (size_t i = 0; i < current.count && status == GTEXT_JSON_OK; i++) {
      const char * item_path = paths ? current.paths[i] : NULL;
      if (!seg->descendant) {
        for (size_t k = 0; k < seg->selector_count; k++) {
          status = json_path_apply_selector(
              ev, &seg->selectors[k], current.items[i], item_path, &next);
          if (status != GTEXT_JSON_OK) {
            break;
          }
        }
        continue;
      }

      /* A descendant segment applies its selectors to the node and to every
       * node under it (2.5.2.2), in document order. */
      json_path_list visited;
      memset(&visited, 0, sizeof(visited));
      visited.alloc = ev->alloc;
      visited.want_paths = paths;
      status = json_path_collect_descendants(
          ev, current.items[i], item_path, &visited);
      for (size_t v = 0; v < visited.count && status == GTEXT_JSON_OK; v++) {
        for (size_t k = 0; k < seg->selector_count; k++) {
          status = json_path_apply_selector(ev, &seg->selectors[k],
              visited.items[v], paths ? visited.paths[v] : NULL, &next);
          if (status != GTEXT_JSON_OK) {
            break;
          }
        }
      }
      json_path_list_free(&visited);
    }

    json_path_list_free(&current);
    current = next;
  }

  if (status != GTEXT_JSON_OK) {
    json_path_list_free(&current);
    return status;
  }

  /* Hand the nodes - and their paths, which move rather than being copied -
   * to the caller's list. */
  for (size_t i = 0; i < current.count; i++) {
    char * moved = NULL;
    if (paths) {
      moved = current.paths[i];
      current.paths[i] = NULL;
    }
    if (!json_path_list_push_path(out, current.items[i], moved)) {
      json_path_list_free(&current);
      return GTEXT_JSON_E_OOM;
    }
  }
  json_path_list_free(&current);
  return GTEXT_JSON_OK;
}

static GTEXT_JSON_Status json_path_select_internal(
    const GTEXT_JSON_Path * path, const GTEXT_JSON_Value * root,
    GTEXT_JSON_Path_Result * out, bool want_paths) {
  if (!path || !root || !out) {
    return GTEXT_JSON_E_INVALID;
  }
  memset(out, 0, sizeof(*out));
  out->alloc = path->alloc;

  json_path_eval ev;
  memset(&ev, 0, sizeof(ev));
  ev.root = root;
  ev.alloc = path->alloc;
  ev.status = GTEXT_JSON_OK;

  json_path_list nodes;
  memset(&nodes, 0, sizeof(nodes));
  nodes.alloc = path->alloc;
  nodes.want_paths = want_paths;

  GTEXT_JSON_Status status = json_path_apply_segments(
      &ev, path->segments, path->segment_count, root, &nodes);
  if (status == GTEXT_JSON_OK && ev.status != GTEXT_JSON_OK) {
    status = ev.status;
  }
  if (status != GTEXT_JSON_OK) {
    json_path_list_free(&nodes);
    return status;
  }

  out->nodes = nodes.items;
  out->paths = nodes.paths;
  out->count = nodes.count;
  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_path_select(const GTEXT_JSON_Path * path,
    const GTEXT_JSON_Value * root, GTEXT_JSON_Path_Result * out) {
  return json_path_select_internal(path, root, out, false);
}

GTEXT_API GTEXT_JSON_Status gtext_json_path_select_paths(
    const GTEXT_JSON_Path * path, const GTEXT_JSON_Value * root,
    GTEXT_JSON_Path_Result * out) {
  return json_path_select_internal(path, root, out, true);
}

static GTEXT_JSON_Status json_path_query_internal(
    const GTEXT_JSON_Value * root, const char * query, size_t len,
    const GTEXT_Allocator * alloc, GTEXT_JSON_Path_Result * out,
    GTEXT_JSON_Error * err, bool want_paths) {
  if (!root || !query || !out) {
    return GTEXT_JSON_E_INVALID;
  }
  GTEXT_JSON_Path * path = gtext_json_path_compile(query, len, alloc, err);
  if (!path) {
    memset(out, 0, sizeof(*out));
    return err ? err->code : GTEXT_JSON_E_PATH;
  }
  const GTEXT_JSON_Status status =
      json_path_select_internal(path, root, out, want_paths);
  gtext_json_path_free(path);
  return status;
}

GTEXT_API GTEXT_JSON_Status gtext_json_path_query(const GTEXT_JSON_Value * root,
    const char * query, size_t len, const GTEXT_Allocator * alloc,
    GTEXT_JSON_Path_Result * out, GTEXT_JSON_Error * err) {
  return json_path_query_internal(root, query, len, alloc, out, err, false);
}

GTEXT_API GTEXT_JSON_Status gtext_json_path_query_paths(
    const GTEXT_JSON_Value * root, const char * query, size_t len,
    const GTEXT_Allocator * alloc, GTEXT_JSON_Path_Result * out,
    GTEXT_JSON_Error * err) {
  return json_path_query_internal(root, query, len, alloc, out, err, true);
}

GTEXT_API void gtext_json_path_result_free(GTEXT_JSON_Path_Result * result) {
  if (!result) {
    return;
  }
  if (result->paths) {
    for (size_t i = 0; i < result->count; i++) {
      gtext_allocator_free(result->alloc, result->paths[i]);
    }
    gtext_allocator_free(result->alloc, result->paths);
    result->paths = NULL;
  }
  gtext_allocator_free(result->alloc, (void *)result->nodes);
  result->nodes = NULL;
  result->count = 0;
}
