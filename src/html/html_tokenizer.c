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
 * The HTML tokenizer: WHATWG HTML, "Tokenization" (13.2.5) and "Preprocessing
 * the input stream" (13.2.3.5). The interface, and what it promises about
 * chunking, memory and errors, is html_tokenizer_internal.h; this file is the
 * machine.
 *
 * ## Source of the state machine
 *
 * The state names, the transitions and the error codes below were written
 * from the standard's text as served at https://html.spec.whatwg.org/multipage/
 * parsing.html on 2026-10-11 (the living standard's "Last Updated 9 October
 * 2026"), state by state, in its order: 13.2.5.1 is DATA, 13.2.5.84 is
 * NUMERIC_CHARACTER_REFERENCE_END. That text has processing instruction
 * states (13.2.5.72 to 13.2.5.76) which html5lib 1.1 and parse5 8.0.1 predate;
 * the vectors of the pinned html5lib-tests already carry them. The html5lib
 * vectors, the two references and this machine are compared, and where they
 * disagree the standard decides (tools/conformance/HTML_TOKENIZER_EXCEPTIONS
 * and tools/oracle/html_tokenizer_divergences.txt say each time).
 *
 * ## How it is built
 *
 * One function, run(), is the whole machine: a loop over a `switch` on the
 * state. Every state reads one character with GET() and either stays, moves, or
 * "reconsumes", which here is un-reading the character (the cursor goes back
 * to where the last consume() began) and moving. Nothing keeps a character
 * "in hand", so suspending is trivial: GET() answers CP_NEED when the bytes of
 * the next character are not all here yet and run() returns, with the state
 * and the cursor exactly where they were. The few states that look ahead
 * (match_literal(), the named character reference) un-read, look, and return
 * CP_NEED the same way. No state does work before its first GET() that would be
 * done twice by retrying it.
 *
 * The cursor is (pos, line, col). consume() is the only thing that moves it
 * forward and it is also the only place the input stream's own errors
 * (surrogate, noncharacter, control, invalid UTF-8) are raised; they are
 * raised at most once per character (err_hwm), because un-reading and
 * consuming again must not report twice.
 *
 * ## Tokens
 *
 * A tag, comment, DOCTYPE or processing instruction under construction lives in
 * one byte arena (`ar`): a tag's name, then each attribute's name and value
 * in turn (an attribute dropped for being a duplicate is truncated away); a
 * DOCTYPE's name, then its public identifier, then its system identifier.
 * Those are built strictly in that order, so each is contiguous. Characters
 * go to `text`, a separate buffer, because a run of characters is held while
 * the tag after it is built. The attribute table stores offsets; they become
 * pointers only when the token is handed out, because the arena moves as it
 * grows.
 *
 * Any helper that fails (allocation, a limit) records it in `failed` and
 * returns; the loop looks at `failed` before every state, so the states do not
 * check, and no helper does anything once it is set.
 */

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "charref_internal.h"
#include "html_tokenizer_internal.h"

/* ---- Constants ---------------------------------------------------------- */

#define CP_EOF ((int32_t)-1)
#define CP_NEED ((int32_t)-2)

/* The numbers the standard's numeric character reference end state clamps to:
   anything above this is out of range already, and the cap keeps a very long
   digit run from overflowing. */
#define CODE_CAP 0x110000u

/* The buffer the named character reference state copies the letters, digits and a
   closing `;` at the cursor into before looking them up. The longest name in the table is
   gtext_html_charref_longest (32) bytes, `;` included; the state clamps to this size
   as well, and tests/test-html-tokenizer.cpp checks the table's longest fits with room. */
#define NAME_WINDOW GTEXT_HTML_TOKENIZER_NAME_WINDOW
_Static_assert(NAME_WINDOW >= 34, "the window holds the longest name and a semicolon");

typedef enum {
  ST_DATA = 0,
  ST_RCDATA,
  ST_RAWTEXT,
  ST_SCRIPT_DATA,
  ST_PLAINTEXT,
  ST_CDATA_SECTION,
  ST_TAG_OPEN,
  ST_END_TAG_OPEN,
  ST_TAG_NAME,
  ST_RCDATA_LT,
  ST_RCDATA_END_OPEN,
  ST_RCDATA_END_NAME,
  ST_RAWTEXT_LT,
  ST_RAWTEXT_END_OPEN,
  ST_RAWTEXT_END_NAME,
  ST_SCRIPT_LT,
  ST_SCRIPT_END_OPEN,
  ST_SCRIPT_END_NAME,
  ST_SCRIPT_ESC_START,
  ST_SCRIPT_ESC_START_DASH,
  ST_SCRIPT_ESC,
  ST_SCRIPT_ESC_DASH,
  ST_SCRIPT_ESC_DASH_DASH,
  ST_SCRIPT_ESC_LT,
  ST_SCRIPT_ESC_END_OPEN,
  ST_SCRIPT_ESC_END_NAME,
  ST_SCRIPT_DBL_ESC_START,
  ST_SCRIPT_DBL_ESC,
  ST_SCRIPT_DBL_ESC_DASH,
  ST_SCRIPT_DBL_ESC_DASH_DASH,
  ST_SCRIPT_DBL_ESC_LT,
  ST_SCRIPT_DBL_ESC_END,
  ST_BEFORE_ATTR_NAME,
  ST_ATTR_NAME,
  ST_AFTER_ATTR_NAME,
  ST_BEFORE_ATTR_VALUE,
  ST_ATTR_VALUE_DQ,
  ST_ATTR_VALUE_SQ,
  ST_ATTR_VALUE_UNQ,
  ST_AFTER_ATTR_VALUE_Q,
  ST_SELF_CLOSING,
  ST_BOGUS_COMMENT,
  ST_MARKUP_DECL_OPEN,
  ST_COMMENT_START,
  ST_COMMENT_START_DASH,
  ST_COMMENT,
  ST_COMMENT_LT,
  ST_COMMENT_LT_BANG,
  ST_COMMENT_LT_BANG_DASH,
  ST_COMMENT_LT_BANG_DASH_DASH,
  ST_COMMENT_END_DASH,
  ST_COMMENT_END,
  ST_COMMENT_END_BANG,
  ST_DOCTYPE,
  ST_BEFORE_DOCTYPE_NAME,
  ST_DOCTYPE_NAME,
  ST_AFTER_DOCTYPE_NAME,
  ST_AFTER_DOCTYPE_PUBLIC_KW,
  ST_BEFORE_DOCTYPE_PUBLIC_ID,
  ST_DOCTYPE_PUBLIC_ID_DQ,
  ST_DOCTYPE_PUBLIC_ID_SQ,
  ST_AFTER_DOCTYPE_PUBLIC_ID,
  ST_BETWEEN_DOCTYPE_IDS,
  ST_AFTER_DOCTYPE_SYSTEM_KW,
  ST_BEFORE_DOCTYPE_SYSTEM_ID,
  ST_DOCTYPE_SYSTEM_ID_DQ,
  ST_DOCTYPE_SYSTEM_ID_SQ,
  ST_AFTER_DOCTYPE_SYSTEM_ID,
  ST_BOGUS_DOCTYPE,
  ST_CDATA_BRACKET,
  ST_CDATA_END,
  ST_PI_OPEN,
  ST_PI_TARGET,
  ST_AFTER_PI_TARGET,
  ST_PI_DATA,
  ST_PI_QUESTIONABLE,
  ST_CHARREF,
  ST_NAMED_CHARREF,
  ST_AMBIGUOUS_AMP,
  ST_NUMERIC_CHARREF,
  ST_HEX_CHARREF_START,
  ST_HEX_CHARREF,
  ST_DEC_CHARREF,
  ST_NUMERIC_CHARREF_END
} state_t;

typedef struct {
  char * data;
  size_t length;
  size_t capacity;
} vec;

typedef struct {
  size_t line;
  size_t column;
  size_t offset;
} where;

/* An attribute as the arena holds it. */
typedef struct {
  size_t name_offset;
  size_t name_length;
  size_t value_offset;
  size_t value_length;
  size_t slot; /* where it sits in the duplicate-detection table */
} attr_slot;

typedef enum { RUN_TOKEN, RUN_NEED, RUN_FAIL } run_result;

struct GTEXT_HTML_Tokenizer {
  GTEXT_Allocator allocator;
  size_t max_input;
  size_t max_name;
  size_t max_value;
  size_t max_comment;
  size_t max_text;
  size_t max_attributes;
  size_t max_errors;
  bool (*allow_cdata)(void *);
  void * allow_cdata_user;
  bool wtf8;
  bool keep_no_errors;

  /* Input. `in` is the caller's chunk or `pend`. */
  const uint8_t * in;
  size_t in_length;
  const uint8_t * rest; /* the part of the caller's chunk not yet visible, while `in` is `pend` */
  size_t rest_length;
  size_t pos;
  bool borrowed;
  uint8_t * pend;
  size_t pend_length;
  size_t pend_capacity;
  size_t base_offset; /* the input-as-fed offset of in[0] */
  size_t total_fed;
  bool final;

  /* The cursor, and the start of the last consume(). */
  size_t line;
  size_t column;
  size_t prev_pos;
  size_t prev_line;
  size_t prev_column;
  size_t prev_offset; /* the input-as-fed offset of the same, kept across buffer moves */
  size_t err_high; /* input-stream errors are raised for offsets >= this */

  /* The machine. */
  state_t state;
  state_t return_state;
  uint32_t code; /* the numeric character reference's value so far */
  GTEXT_HTML_CdataOverride cdata_override; /* the tree builder's word, or the callback's */

  /* Output queue: text, then a token, then EOF. */
  vec text;
  where text_where;
  bool text_whitespace;
  bool text_null;
  bool text_taken;
  bool token_ready;
  bool eof_ready;
  bool done;
  GTEXT_HTML_TokenizerStatus failed; /* the sticky failure; meaningful if has_failed */
  bool has_failed;

  /* The token under construction. */
  GTEXT_HTML_Token token;
  GTEXT_HTML_Token out;
  vec ar;
  where start; /* where the construct began: the `<` */
  where amp;   /* the `&` of the character reference in progress */
  size_t section_start;
  size_t section_max;
  GTEXT_HTML_TokenizerStatus section_status;
  size_t name_offset; /* tag/doctype/PI name in ar */
  size_t name_length;
  size_t public_offset;
  size_t public_length;
  size_t system_offset;
  size_t system_length;
  size_t data_offset; /* PI data */
  attr_slot * attrs;
  size_t attr_count;
  size_t attr_capacity;
  size_t * names;       /* open-addressed table of attribute index + 1 */
  size_t names_capacity; /* a power of two, or 0 before the first attribute */
  GTEXT_HTML_Attribute * attr_out;
  size_t attr_out_capacity;
  size_t attr_name_start;
  bool attr_open;
  bool attr_dropped;
  vec temp;       /* the standard's "temporary buffer" */
  char cr[3];     /* what a character reference has consumed so far: `&`, `#`, `x` */
  size_t cr_length;
  uint64_t hash_seed;
  vec last_start; /* the last start tag name emitted */
  bool have_last_start;

  /* Errors. */
  GTEXT_HTML_ParseError * errors;
  size_t error_count;
  size_t error_capacity;
  size_t errors_dropped;

  size_t work;
};

/* ---- Small things ------------------------------------------------------- */

static inline bool is_ws(int32_t c) {
  return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x20;
}
static inline bool is_upper(int32_t c) {
  return c >= 'A' && c <= 'Z';
}
static inline bool is_lower(int32_t c) {
  return c >= 'a' && c <= 'z';
}
static inline bool is_alpha(int32_t c) {
  return is_upper(c) || is_lower(c);
}
static inline bool is_digit(int32_t c) {
  return c >= '0' && c <= '9';
}
static inline bool is_alnum(int32_t c) {
  return is_alpha(c) || is_digit(c);
}
static inline bool is_hex(int32_t c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static size_t utf8_encode(uint32_t cp, char * out) {
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

/* ---- Failure and growth ------------------------------------------------- */

static void fail(GTEXT_HTML_Tokenizer * t, GTEXT_HTML_TokenizerStatus status) {
  if (!t->has_failed) {
    t->has_failed = true;
    t->failed = status;
  }
}

static bool vec_grow(GTEXT_HTML_Tokenizer * t, vec * v, size_t extra) {
  if (extra > SIZE_MAX - v->length - 1) {
    fail(t, GTEXT_HTML_TOKENIZER_E_OOM);
    return false;
  }
  const size_t need = v->length + extra + 1;
  if (need <= v->capacity) {
    return true;
  }
  size_t cap = v->capacity ? v->capacity : 64;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) {
      cap = need;
      break;
    }
    cap *= 2;
  }
  char * grown = gtext_allocator_realloc(&t->allocator, v->data, cap);
  if (!grown) {
    fail(t, GTEXT_HTML_TOKENIZER_E_OOM);
    return false;
  }
  v->data = grown;
  v->capacity = cap;
  return true;
}

static void vec_put(
    GTEXT_HTML_Tokenizer * t, vec * v, const char * bytes, size_t n) {
  if (t->has_failed || !vec_grow(t, v, n)) {
    return;
  }
  if (n) {
    memcpy(v->data + v->length, bytes, n);
  }
  v->length += n;
  t->work += n;
}

/* Bytes into the arena, in the current section (see section_start): the limit
   is on the section, not on the arena, which holds several. */
static void ar_put(GTEXT_HTML_Tokenizer * t, const char * bytes, size_t n) {
  if (t->has_failed) {
    return;
  }
  const size_t used = t->ar.length - t->section_start;
  if (n > t->section_max || used > t->section_max - n) {
    fail(t, t->section_status);
    return;
  }
  vec_put(t, &t->ar, bytes, n);
}

static void ar_cp(GTEXT_HTML_Tokenizer * t, uint32_t cp) {
  char b[4];
  ar_put(t, b, utf8_encode(cp, b));
}

static void section(GTEXT_HTML_Tokenizer * t, size_t max,
    GTEXT_HTML_TokenizerStatus status) {
  t->section_start = t->ar.length;
  t->section_max = max;
  t->section_status = status;
}

static void temp_put(GTEXT_HTML_Tokenizer * t, char c);

/* The temporary buffer as the double escape states use it: it is only ever compared with
   "script", so past 8 letters it stops growing (and cannot be that word). No limit applies
   to text that merely looks like a name. */
static void temp_word(GTEXT_HTML_Tokenizer * t, char c) {
  if (t->temp.length < 8) {
    temp_put(t, c);
  }
}

static void temp_put(GTEXT_HTML_Tokenizer * t, char c) {
  if (t->has_failed) {
    return;
  }
  if (t->temp.length >= t->max_name) {
    fail(t, GTEXT_HTML_TOKENIZER_E_LIMIT_NAME);
    return;
  }
  vec_put(t, &t->temp, &c, 1);
}

/* ---- Errors ------------------------------------------------------------- */

static void error_at(GTEXT_HTML_Tokenizer * t, GTEXT_HTML_ErrorCode code,
    size_t line, size_t column, size_t offset) {
  if (t->has_failed) {
    return;
  }
  if (t->keep_no_errors || t->error_count >= t->max_errors) {
    t->errors_dropped++;
    return;
  }
  if (t->error_count == t->error_capacity) {
    size_t cap = t->error_capacity ? t->error_capacity * 2 : 16;
    if (cap > t->max_errors) {
      cap = t->max_errors;
    }
    GTEXT_HTML_ParseError * grown = gtext_allocator_realloc(
        &t->allocator, t->errors, cap * sizeof(*grown));
    if (!grown) {
      fail(t, GTEXT_HTML_TOKENIZER_E_OOM);
      return;
    }
    t->errors = grown;
    t->error_capacity = cap;
  }
  GTEXT_HTML_ParseError * e = &t->errors[t->error_count++];
  e->code = code;
  e->line = line;
  e->column = column;
  e->offset = offset;
}

/* At "the current input character": the one the last consume() returned (the
   end of the input, for EOF). */
static void perr(GTEXT_HTML_Tokenizer * t, GTEXT_HTML_ErrorCode code) {
  error_at(t, code, t->prev_line, t->prev_column, t->prev_offset);
}

/* At the character after the current one: the cursor. The standard's numeric
   character reference end state and the named reference's missing semicolon
   are reported there by the vectors, because they are decided after the
   characters of the reference have been consumed. */
static void perr_next(GTEXT_HTML_Tokenizer * t, GTEXT_HTML_ErrorCode code) {
  error_at(t, code, t->line, t->column, t->base_offset + t->pos);
}

/* ---- The input stream --------------------------------------------------- */

/* Decode one character at p. Returns false if more bytes are needed to decide;
   otherwise *cp, *n (bytes used) and *bad (replaced) are set. This is the
   WHATWG UTF-8 decoder (Encoding standard, "UTF-8 decoder") plus the
   preprocessing step's newline normalization: CR and CR LF become LF. */
static bool decode_at(const GTEXT_HTML_Tokenizer * t, size_t p, int32_t * cp,
    size_t * n, bool * bad) {
  const uint8_t * s = t->in;
  const size_t len = t->in_length;
  const uint8_t b = s[p];
  *bad = false;
  if (b < 0x80) {
    if (b == '\r') {
      if (p + 1 >= len) {
        if (!t->final) {
          return false;
        }
        *cp = '\n';
        *n = 1;
        return true;
      }
      *cp = '\n';
      *n = s[p + 1] == '\n' ? 2 : 1;
      return true;
    }
    *cp = b;
    *n = 1;
    return true;
  }
  uint8_t lower = 0x80;
  uint8_t upper = 0xBF;
  size_t need;
  uint32_t value;
  if (b >= 0xC2 && b <= 0xDF) {
    need = 1;
    value = b & 0x1F;
  }
  else if (b >= 0xE0 && b <= 0xEF) {
    need = 2;
    value = b & 0x0F;
    if (b == 0xE0) {
      lower = 0xA0;
    }
    else if (b == 0xED && !t->wtf8) {
      upper = 0x9F;
    }
  }
  else if (b >= 0xF0 && b <= 0xF4) {
    need = 3;
    value = b & 0x07;
    if (b == 0xF0) {
      lower = 0x90;
    }
    else if (b == 0xF4) {
      upper = 0x8F;
    }
  }
  else {
    *cp = 0xFFFD;
    *n = 1;
    *bad = true;
    return true;
  }
  for (size_t i = 1; i <= need; i++) {
    if (p + i >= len) {
      if (!t->final) {
        return false;
      }
      *cp = 0xFFFD;
      *n = i;
      *bad = true;
      return true;
    }
    const uint8_t c = s[p + i];
    if (c < lower || c > upper) {
      *cp = 0xFFFD;
      *n = i;
      *bad = true;
      return true;
    }
    lower = 0x80;
    upper = 0xBF;
    value = (value << 6) | (c & 0x3F);
  }
  *cp = (int32_t)value;
  *n = need + 1;
  return true;
}

static bool is_noncharacter(int32_t cp) {
  return (cp >= 0xFDD0 && cp <= 0xFDEF) || ((cp & 0xFFFE) == 0xFFFE);
}

static bool is_control(int32_t cp) {
  return (cp >= 0x01 && cp <= 0x1F && cp != 0x09 && cp != 0x0A && cp != 0x0C
             && cp != 0x0D)
      || (cp >= 0x7F && cp <= 0x9F);
}

/* The next character, advancing the cursor. CP_EOF at the end of a finished
   input, CP_NEED at the end of an unfinished one or inside a character. */
static int32_t consume(GTEXT_HTML_Tokenizer * t) {
  t->work++;
  const size_t p = t->pos;
  t->prev_pos = p;
  t->prev_line = t->line;
  t->prev_column = t->column;
  t->prev_offset = t->base_offset + p;
  if (p >= t->in_length) {
    return t->final ? CP_EOF : CP_NEED;
  }
  int32_t cp;
  size_t n;
  bool bad;
  if (!decode_at(t, p, &cp, &n, &bad)) {
    return CP_NEED;
  }
  t->pos = p + n;
  if (cp == '\n') {
    t->line++;
    t->column = 1;
  }
  else {
    /* Columns count UTF-16 code units, as the vectors (and every script that
       reads a position out of a DOM) do: a character outside the BMP is 2. */
    t->column += cp > 0xFFFF ? 2 : 1;
  }
  const size_t off = t->base_offset + p;
  if (off >= t->err_high) {
    t->err_high = off + n;
    if (bad) {
      perr(t, GTEXT_HTML_ERR_INVALID_UTF8);
    }
    else if (cp >= 0xD800 && cp <= 0xDFFF) {
      perr(t, GTEXT_HTML_ERR_SURROGATE_IN_INPUT_STREAM);
    }
    else if (is_noncharacter(cp)) {
      perr(t, GTEXT_HTML_ERR_NONCHARACTER_IN_INPUT_STREAM);
    }
    else if (is_control(cp)) {
      perr(t, GTEXT_HTML_ERR_CONTROL_CHARACTER_IN_INPUT_STREAM);
    }
  }
  return cp;
}

/* "Reconsume": the character the last consume() returned is the next one
   again. At the end of the input this changes nothing. */
static void unread(GTEXT_HTML_Tokenizer * t) {
  t->pos = t->prev_pos;
  t->line = t->prev_line;
  t->column = t->prev_column;
}

/* The k-th character from the cursor, without consuming it or raising its
   errors. */
static int32_t peek(GTEXT_HTML_Tokenizer * t, size_t k) {
  size_t p = t->pos;
  for (size_t i = 0;; i++) {
    t->work++;
    if (p >= t->in_length) {
      return t->final ? CP_EOF : CP_NEED;
    }
    int32_t cp;
    size_t n;
    bool bad;
    if (!decode_at(t, p, &cp, &n, &bad)) {
      return CP_NEED;
    }
    if (i == k) {
      return cp;
    }
    p += n;
  }
}

typedef enum { LIT_NO = 0, LIT_YES = 1, LIT_NEED = 2 } lit_result;

/* Do the next characters spell `lit` (ASCII, compared case-insensitively if
   `fold`)? Decides as soon as the characters seen decide it: a mismatch at the
   second of seven is "no" without waiting for the other five. At the end of the
   input a prefix is "no". */
static lit_result match_literal(
    GTEXT_HTML_Tokenizer * t, const char * lit, size_t n, bool fold) {
  for (size_t i = 0; i < n; i++) {
    const int32_t c = peek(t, i);
    if (c == CP_NEED) {
      return LIT_NEED;
    }
    if (c == CP_EOF) {
      return LIT_NO;
    }
    int32_t want = (unsigned char)lit[i];
    int32_t got = c;
    if (fold) {
      if (is_upper(got)) {
        got += 0x20;
      }
      if (is_upper(want)) {
        want += 0x20;
      }
    }
    if (got != want) {
      return LIT_NO;
    }
  }
  return LIT_YES;
}

static void skip(GTEXT_HTML_Tokenizer * t, size_t n) {
  while (n--) {
    (void)consume(t);
  }
}

/* ---- Text (character tokens) -------------------------------------------- */

static void text_put(GTEXT_HTML_Tokenizer * t, const char * bytes, size_t n,
    const where * at, bool whitespace, bool is_null) {
  if (t->has_failed) {
    return;
  }
  if (t->text.length == 0) {
    t->text_where = *at;
    t->text_whitespace = true;
    t->text_null = false;
  }
  if (n > t->max_text || t->text.length > t->max_text - n) {
    fail(t, GTEXT_HTML_TOKENIZER_E_LIMIT_TEXT);
    return;
  }
  vec_put(t, &t->text, bytes, n);
  if (!whitespace) {
    t->text_whitespace = false;
  }
  if (is_null) {
    t->text_null = true;
  }
}

static where here(const GTEXT_HTML_Tokenizer * t) {
  where w = {t->prev_line, t->prev_column, t->prev_offset};
  return w;
}

/* The current character, as a character token. */
static void text_cp(GTEXT_HTML_Tokenizer * t, uint32_t cp) {
  char b[4];
  const where w = here(t);
  text_put(t, b, utf8_encode(cp, b), &w, is_ws((int32_t)cp), cp == 0);
}

/* ASCII characters whose position is the construct's start (the `<` of `</`). */
static void text_at(GTEXT_HTML_Tokenizer * t, const char * s, size_t n,
    const where * at) {
  text_put(t, s, n, at, false, false);
}

/* The temporary buffer, as characters: what an abandoned end tag name becomes. */
static void text_temp(GTEXT_HTML_Tokenizer * t) {
  const where w = here(t);
  /* ASCII letters only, so not white space, not NUL. */
  text_put(t, t->temp.data, t->temp.length, &w, false, false);
}

/* ---- Tokens ------------------------------------------------------------- */

static void token_begin(GTEXT_HTML_Tokenizer * t, GTEXT_HTML_TokenType type) {
  memset(&t->token, 0, sizeof(t->token));
  t->token.type = type;
  t->token.line = t->start.line;
  t->token.column = t->start.column;
  t->token.offset = t->start.offset;
  t->ar.length = 0;
  for (size_t i = 0; i < t->attr_count; i++) {
    t->names[t->attrs[i].slot] = 0;
  }
  t->attr_count = 0;
  t->attr_open = false;
  t->attr_dropped = false;
  t->name_offset = t->name_length = 0;
  t->public_offset = t->public_length = 0;
  t->system_offset = t->system_length = 0;
  t->data_offset = 0;
  section(t, t->max_name, GTEXT_HTML_TOKENIZER_E_LIMIT_NAME);
}

static void tag_begin(GTEXT_HTML_Tokenizer * t, GTEXT_HTML_TokenType type) {
  token_begin(t, type);
}

static void tag_name_done(GTEXT_HTML_Tokenizer * t) {
  t->name_offset = 0;
  t->name_length = t->ar.length;
}

static void comment_begin(GTEXT_HTML_Tokenizer * t) {
  token_begin(t, GTEXT_HTML_TOKEN_COMMENT);
  section(t, t->max_comment, GTEXT_HTML_TOKENIZER_E_LIMIT_COMMENT);
}

static void doctype_begin(GTEXT_HTML_Tokenizer * t) {
  token_begin(t, GTEXT_HTML_TOKEN_DOCTYPE);
}

static void attr_begin(GTEXT_HTML_Tokenizer * t) {
  t->attr_open = true;
  t->attr_dropped = false;
  t->attr_name_start = t->ar.length;
  section(t, t->max_name, GTEXT_HTML_TOKENIZER_E_LIMIT_NAME);
}

/* FNV-1a over the name, started from a per-tokenizer seed and finished with a mixing
   step, so that two names that collide in the table of one tokenizer do not in another's:
   a page cannot be written to make every tag quadratic for every tokenizer at once. */
static uint64_t hash_name(uint64_t seed, const char * s, size_t n) {
  uint64_t h = 1469598103934665603ULL ^ (seed * 0x9E3779B97F4A7C15ULL);
  for (size_t i = 0; i < n; i++) {
    h = (h ^ (unsigned char)s[i]) * 1099511628211ULL;
  }
  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDULL;
  h ^= h >> 33;
  h *= 0xC4CEB9FE1A85EC53ULL;
  h ^= h >> 33;
  return h;
}

uint64_t gtext_html_tokenizer_name_hash(uint64_t seed, const char * s, size_t n) {
  return hash_name(seed, s, n);
}

static size_t name_hash(const GTEXT_HTML_Tokenizer * t, const char * s, size_t n) {
  return (size_t)hash_name(t->hash_seed, s, n);
}

/* Make room in the duplicate-detection table for one more attribute, keeping it
   at most half full. Attribute names are looked up here, not by comparing with
   every earlier one, so a tag with a thousand attributes costs a thousand
   probes and not half a million comparisons. */
static bool names_reserve(GTEXT_HTML_Tokenizer * t) {
  if ((t->attr_count + 1) * 2 <= t->names_capacity) {
    return true;
  }
  size_t cap = t->names_capacity ? t->names_capacity * 2 : 16;
  size_t * grown = gtext_allocator_calloc(&t->allocator, cap, sizeof(*grown));
  if (!grown) {
    fail(t, GTEXT_HTML_TOKENIZER_E_OOM);
    return false;
  }
  for (size_t i = 0; i < t->attr_count; i++) {
    size_t slot = name_hash(t, t->ar.data + t->attrs[i].name_offset,
                      t->attrs[i].name_length) & (cap - 1);
    while (grown[slot]) {
      slot = (slot + 1) & (cap - 1);
    }
    grown[slot] = i + 1;
    t->attrs[i].slot = slot;
  }
  t->work += cap;
  gtext_allocator_free(&t->allocator, t->names);
  t->names = grown;
  t->names_capacity = cap;
  return true;
}

/* The user agent leaves the attribute name state: compare the name with the
   others on the token; a repeat is an error and the attribute is removed, but
   it stays "the current attribute" for the tokenizer's purposes, so its value
   is read and thrown away. */
static void attr_name_done(GTEXT_HTML_Tokenizer * t) {
  if (!t->attr_open || t->has_failed) {
    return;
  }
  const size_t start = t->attr_name_start;
  const size_t len = t->ar.length - start;
  if (!names_reserve(t)) {
    return;
  }
  size_t slot = name_hash(t, t->ar.data + start, len) & (t->names_capacity - 1);
  while (t->names[slot]) {
    t->work++;
    const attr_slot * other = &t->attrs[t->names[slot] - 1];
    if (other->name_length == len
        && memcmp(t->ar.data + other->name_offset, t->ar.data + start, len)
            == 0) {
      perr(t, GTEXT_HTML_ERR_DUPLICATE_ATTRIBUTE);
      t->attr_dropped = true;
      t->ar.length = start;
      t->attr_open = false;
      return;
    }
    slot = (slot + 1) & (t->names_capacity - 1);
  }
  if (t->attr_count >= t->max_attributes) {
    fail(t, GTEXT_HTML_TOKENIZER_E_LIMIT_ATTRIBUTES);
    return;
  }
  if (t->attr_count == t->attr_capacity) {
    const size_t cap = t->attr_capacity ? t->attr_capacity * 2 : 8;
    attr_slot * grown = gtext_allocator_realloc(
        &t->allocator, t->attrs, cap * sizeof(*grown));
    if (!grown) {
      fail(t, GTEXT_HTML_TOKENIZER_E_OOM);
      return;
    }
    t->attrs = grown;
    t->attr_capacity = cap;
  }
  attr_slot * a = &t->attrs[t->attr_count++];
  a->name_offset = start;
  a->name_length = len;
  a->value_offset = t->ar.length;
  a->value_length = 0;
  a->slot = slot;
  t->names[slot] = t->attr_count;
  t->attr_open = false;
  section(t, t->max_value, GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE);
}

/* Append to the current attribute's value (nothing, if it was dropped). */
static void attr_value_put(
    GTEXT_HTML_Tokenizer * t, const char * bytes, size_t n) {
  if (t->attr_dropped || t->attr_count == 0 || t->has_failed) {
    return;
  }
  ar_put(t, bytes, n);
  t->attrs[t->attr_count - 1].value_length =
      t->ar.length - t->attrs[t->attr_count - 1].value_offset;
}

static void attr_value_cp(GTEXT_HTML_Tokenizer * t, uint32_t cp) {
  char b[4];
  attr_value_put(t, b, utf8_encode(cp, b));
}

/* The current tag token is done: errors the standard attaches to emitting it,
   and the bookkeeping the tokenizer itself needs. */
static void tag_emit(GTEXT_HTML_Tokenizer * t) {
  if (t->token.type == GTEXT_HTML_TOKEN_END_TAG) {
    if (t->attr_count > 0) {
      perr(t, GTEXT_HTML_ERR_END_TAG_WITH_ATTRIBUTES);
    }
    if (t->token.self_closing) {
      perr(t, GTEXT_HTML_ERR_END_TAG_WITH_TRAILING_SOLIDUS);
    }
  }
  else {
    t->last_start.length = 0;
    vec_put(t, &t->last_start, t->ar.data, t->name_length);
    t->have_last_start = true;
  }
  t->token_ready = true;
}

/* Is the end tag being built "an appropriate end tag token"? */
static bool appropriate_end_tag(const GTEXT_HTML_Tokenizer * t) {
  return t->have_last_start && t->last_start.length == t->ar.length
      && memcmp(t->last_start.data, t->ar.data, t->ar.length) == 0;
}

/* Hand the token out: pointers where the table holds offsets. */
static bool token_finish(GTEXT_HTML_Tokenizer * t) {
  GTEXT_HTML_Token * k = &t->token;
  const char * base = t->ar.data ? t->ar.data : "";
  switch (k->type) {
    case GTEXT_HTML_TOKEN_START_TAG:
    case GTEXT_HTML_TOKEN_END_TAG:
      k->name = base;
      k->name_length = t->name_length;
      k->attribute_count = t->attr_count;
      if (t->attr_count > t->attr_out_capacity) {
        GTEXT_HTML_Attribute * grown = gtext_allocator_realloc(
            &t->allocator, t->attr_out, t->attr_count * sizeof(*grown));
        if (!grown) {
          fail(t, GTEXT_HTML_TOKENIZER_E_OOM);
          return false;
        }
        t->attr_out = grown;
        t->attr_out_capacity = t->attr_count;
      }
      for (size_t i = 0; i < t->attr_count; i++) {
        t->attr_out[i].name = base + t->attrs[i].name_offset;
        t->attr_out[i].name_length = t->attrs[i].name_length;
        t->attr_out[i].value = base + t->attrs[i].value_offset;
        t->attr_out[i].value_length = t->attrs[i].value_length;
      }
      k->attributes = t->attr_out;
      break;
    case GTEXT_HTML_TOKEN_COMMENT:
      k->data = base;
      k->data_length = t->ar.length;
      break;
    case GTEXT_HTML_TOKEN_PROCESSING_INSTRUCTION:
      k->name = base;
      k->name_length = t->name_length;
      k->data = base + t->data_offset;
      k->data_length = t->ar.length - t->data_offset;
      break;
    case GTEXT_HTML_TOKEN_DOCTYPE:
      if (k->has_name) {
        k->name = base + t->name_offset;
        k->name_length = t->name_length;
      }
      if (k->has_public_id) {
        k->public_id = base + t->public_offset;
        k->public_id_length = t->public_length;
      }
      if (k->has_system_id) {
        k->system_id = base + t->system_offset;
        k->system_id_length = t->system_length;
      }
      break;
    default:
      break;
  }
  return true;
}

/* ---- Character references ----------------------------------------------- */

static bool in_attribute(const GTEXT_HTML_Tokenizer * t) {
  return t->return_state == ST_ATTR_VALUE_DQ
      || t->return_state == ST_ATTR_VALUE_SQ
      || t->return_state == ST_ATTR_VALUE_UNQ;
}

/* "Flush code points consumed as a character reference": into the attribute
   value or out as characters. `bytes` are UTF-8 of the code points. */
static void flush_bytes(
    GTEXT_HTML_Tokenizer * t, const char * bytes, size_t n) {
  if (in_attribute(t)) {
    attr_value_put(t, bytes, n);
  }
  else {
    text_put(t, bytes, n, &t->amp, false, false);
  }
}

static void flush_cp(GTEXT_HTML_Tokenizer * t, uint32_t cp) {
  char b[4];
  const size_t n = utf8_encode(cp, b);
  if (in_attribute(t)) {
    attr_value_put(t, b, n);
  }
  else {
    text_put(t, b, n, &t->amp, is_ws((int32_t)cp), false);
  }
}

/* The standard's table in the numeric character reference end state: the
   code points 0x80 to 0x9F that are the Windows-1252 characters instead. The
   five that Windows-1252 leaves undefined (0x81, 0x8D, 0x8F, 0x90, 0x9D) have no
   row and stay as they are. Source: WHATWG HTML 13.2.5.84, as served
   2026-10-11. tests/test-html-tokenizer.cpp checks each against the
   Encoding standard's windows-1252 index. */
static const uint16_t c1_remap[32] = {
  0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, /* 80-87 */
  0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F, /* 88-8F */
  0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, /* 90-97 */
  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178, /* 98-9F */
};

/* code * base + digit, held at CODE_CAP: past it the number is out of range whatever
   follows, and a run of a million digits must not wrap around into range. */
static uint32_t accumulate(uint32_t code, uint32_t base, uint32_t digit) {
  code = code * base + digit; /* code <= CODE_CAP, so this cannot overflow */
  return code > CODE_CAP ? CODE_CAP : code;
}

/* The numeric character reference end state. */
static void numeric_end(GTEXT_HTML_Tokenizer * t) {
  uint32_t code = t->code;
  if (code == 0x00) {
    perr_next(t, GTEXT_HTML_ERR_NULL_CHARACTER_REFERENCE);
    code = 0xFFFD;
  }
  if (code > 0x10FFFF) {
    perr_next(t, GTEXT_HTML_ERR_CHARACTER_REFERENCE_OUTSIDE_UNICODE_RANGE);
    code = 0xFFFD;
  }
  if (code >= 0xD800 && code <= 0xDFFF) {
    perr_next(t, GTEXT_HTML_ERR_SURROGATE_CHARACTER_REFERENCE);
    code = 0xFFFD;
  }
  if (is_noncharacter((int32_t)code)) {
    perr_next(t, GTEXT_HTML_ERR_NONCHARACTER_CHARACTER_REFERENCE);
  }
  if (code == 0x0D || is_control((int32_t)code)) {
    perr_next(t, GTEXT_HTML_ERR_CONTROL_CHARACTER_REFERENCE);
    if (code >= 0x80 && code <= 0x9F) {
      code = c1_remap[code - 0x80];
    }
  }
  flush_cp(t, code);
}

/* ---- The machine --------------------------------------------------------- */

static state_t text_state_of(state_t s) {
  switch (s) {
    case ST_RCDATA_LT:
    case ST_RCDATA_END_OPEN:
    case ST_RCDATA_END_NAME:
      return ST_RCDATA;
    case ST_RAWTEXT_LT:
    case ST_RAWTEXT_END_OPEN:
    case ST_RAWTEXT_END_NAME:
      return ST_RAWTEXT;
    case ST_SCRIPT_LT:
    case ST_SCRIPT_END_OPEN:
    case ST_SCRIPT_END_NAME:
      return ST_SCRIPT_DATA;
    default: /* the script data escaped end tag states */
      return ST_SCRIPT_ESC;
  }
}

/* Is `bytes` the lower-case ASCII word `word`, in any case? */
static bool ascii_equals_fold(const char * bytes, size_t n, const char * word) {
  if (strlen(word) != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char ch = bytes[i];
    if (ch >= 'A' && ch <= 'Z') {
      ch = (char)(ch + 0x20);
    }
    if (ch != word[i]) {
      return false;
    }
  }
  return true;
}

static bool cdata_allowed(GTEXT_HTML_Tokenizer * t) {
  if (t->cdata_override != GTEXT_HTML_CDATA_DEFAULT) {
    return t->cdata_override == GTEXT_HTML_CDATA_ALLOW;
  }
  return t->allow_cdata ? t->allow_cdata(t->allow_cdata_user) : false;
}

#define GET() \
  do { \
    c = consume(t); \
    if (c == CP_NEED) { \
      return RUN_NEED; \
    } \
  } while (0)

#define RECONSUME(s) \
  do { \
    unread(t); \
    t->state = (s); \
  } while (0)

#define SWITCH(s) (t->state = (s))

#define MARK_LT() (t->start = here(t))

static run_result run(GTEXT_HTML_Tokenizer * t) {
  int32_t c;
  for (;;) {
    if (t->has_failed) {
      return RUN_FAIL;
    }
    switch (t->state) {
      /* 13.2.5.1 Data state */
      case ST_DATA:
        GET();
        if (c == '&') {
          t->return_state = ST_DATA;
          t->amp = here(t);
          SWITCH(ST_CHARREF);
        }
        else if (c == '<') {
          MARK_LT();
          SWITCH(ST_TAG_OPEN);
        }
        else if (c == CP_EOF) {
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          if (c == 0) {
            perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          }
          text_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.2 RCDATA state */
      case ST_RCDATA:
        GET();
        if (c == '&') {
          t->return_state = ST_RCDATA;
          t->amp = here(t);
          SWITCH(ST_CHARREF);
        }
        else if (c == '<') {
          MARK_LT();
          SWITCH(ST_RCDATA_LT);
        }
        else if (c == CP_EOF) {
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          text_cp(t, 0xFFFD);
        }
        else {
          text_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.3 RAWTEXT state, 13.2.5.4 Script data state */
      case ST_RAWTEXT:
      case ST_SCRIPT_DATA:
        GET();
        if (c == '<') {
          MARK_LT();
          SWITCH(t->state == ST_RAWTEXT ? ST_RAWTEXT_LT : ST_SCRIPT_LT);
        }
        else if (c == CP_EOF) {
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          text_cp(t, 0xFFFD);
        }
        else {
          text_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.5 PLAINTEXT state */
      case ST_PLAINTEXT:
        GET();
        if (c == CP_EOF) {
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          text_cp(t, 0xFFFD);
        }
        else {
          text_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.6 Tag open state */
      case ST_TAG_OPEN:
        GET();
        if (c == '!') {
          SWITCH(ST_MARKUP_DECL_OPEN);
        }
        else if (c == '/') {
          SWITCH(ST_END_TAG_OPEN);
        }
        else if (is_alpha(c)) {
          tag_begin(t, GTEXT_HTML_TOKEN_START_TAG);
          RECONSUME(ST_TAG_NAME);
        }
        else if (c == '?') {
          t->temp.length = 0;
          SWITCH(ST_PI_OPEN);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_BEFORE_TAG_NAME);
          text_at(t, "<", 1, &t->start);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_INVALID_FIRST_CHARACTER_OF_TAG_NAME);
          text_at(t, "<", 1, &t->start);
          RECONSUME(ST_DATA);
        }
        break;

      /* 13.2.5.7 End tag open state */
      case ST_END_TAG_OPEN:
        GET();
        if (is_alpha(c)) {
          tag_begin(t, GTEXT_HTML_TOKEN_END_TAG);
          RECONSUME(ST_TAG_NAME);
        }
        else if (c == '>') {
          perr(t, GTEXT_HTML_ERR_MISSING_END_TAG_NAME);
          SWITCH(ST_DATA);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_BEFORE_TAG_NAME);
          text_at(t, "</", 2, &t->start);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_INVALID_FIRST_CHARACTER_OF_TAG_NAME);
          comment_begin(t);
          RECONSUME(ST_BOGUS_COMMENT);
        }
        break;

      /* 13.2.5.8 Tag name state */
      case ST_TAG_NAME:
        GET();
        if (is_ws(c)) {
          tag_name_done(t);
          SWITCH(ST_BEFORE_ATTR_NAME);
        }
        else if (c == '/') {
          tag_name_done(t);
          SWITCH(ST_SELF_CLOSING);
        }
        else if (c == '>') {
          tag_name_done(t);
          SWITCH(ST_DATA);
          tag_emit(t);
          return RUN_TOKEN;
        }
        else if (is_upper(c)) {
          ar_cp(t, (uint32_t)c + 0x20);
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          ar_cp(t, 0xFFFD);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_TAG);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          ar_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.9 RCDATA less-than sign state, 13.2.5.12 RAWTEXT ... */
      case ST_RCDATA_LT:
      case ST_RAWTEXT_LT:
        GET();
        if (c == '/') {
          t->temp.length = 0;
          SWITCH(t->state == ST_RCDATA_LT ? ST_RCDATA_END_OPEN
                                          : ST_RAWTEXT_END_OPEN);
        }
        else {
          text_at(t, "<", 1, &t->start);
          RECONSUME(text_state_of(t->state));
        }
        break;

      /* 13.2.5.15 Script data less-than sign state */
      case ST_SCRIPT_LT:
        GET();
        if (c == '/') {
          t->temp.length = 0;
          SWITCH(ST_SCRIPT_END_OPEN);
        }
        else if (c == '!') {
          SWITCH(ST_SCRIPT_ESC_START);
          text_at(t, "<!", 2, &t->start);
        }
        else {
          text_at(t, "<", 1, &t->start);
          RECONSUME(ST_SCRIPT_DATA);
        }
        break;

      /* 13.2.5.10, .13, .16, .24: the end tag open states */
      case ST_RCDATA_END_OPEN:
      case ST_RAWTEXT_END_OPEN:
      case ST_SCRIPT_END_OPEN:
      case ST_SCRIPT_ESC_END_OPEN:
        GET();
        if (is_alpha(c)) {
          const state_t from = t->state;
          tag_begin(t, GTEXT_HTML_TOKEN_END_TAG);
          RECONSUME(from == ST_RCDATA_END_OPEN ? ST_RCDATA_END_NAME
                  : from == ST_RAWTEXT_END_OPEN ? ST_RAWTEXT_END_NAME
                  : from == ST_SCRIPT_END_OPEN ? ST_SCRIPT_END_NAME
                                               : ST_SCRIPT_ESC_END_NAME);
        }
        else {
          text_at(t, "</", 2, &t->start);
          RECONSUME(text_state_of(t->state));
        }
        break;

      /* 13.2.5.11, .14, .17, .25: the end tag name states */
      case ST_RCDATA_END_NAME:
      case ST_RAWTEXT_END_NAME:
      case ST_SCRIPT_END_NAME:
      case ST_SCRIPT_ESC_END_NAME:
        GET();
        if ((is_ws(c) || c == '/' || c == '>') && appropriate_end_tag(t)) {
          tag_name_done(t);
          if (is_ws(c)) {
            SWITCH(ST_BEFORE_ATTR_NAME);
          }
          else if (c == '/') {
            SWITCH(ST_SELF_CLOSING);
          }
          else {
            SWITCH(ST_DATA);
            tag_emit(t);
            return RUN_TOKEN;
          }
        }
        else if ((is_upper(c) || is_lower(c)) && t->temp.length < t->max_name) {
          ar_cp(t, is_upper(c) ? (uint32_t)c + 0x20 : (uint32_t)c);
          temp_put(t, (char)c);
        }
        else {
          /* Also the letters of a candidate longer than a name may be: the last start tag
             is no longer than a name, so such a candidate is never the appropriate end
             tag, and the standard would emit it as text at its delimiter, which is where
             the text state reads the rest of it anyway. No limit applies to text. */
          text_at(t, "</", 2, &t->start);
          text_temp(t);
          RECONSUME(text_state_of(t->state));
        }
        break;

      /* 13.2.5.18 Script data escape start state */
      case ST_SCRIPT_ESC_START:
        GET();
        if (c == '-') {
          SWITCH(ST_SCRIPT_ESC_START_DASH);
          text_cp(t, '-');
        }
        else {
          RECONSUME(ST_SCRIPT_DATA);
        }
        break;

      /* 13.2.5.19 Script data escape start dash state */
      case ST_SCRIPT_ESC_START_DASH:
        GET();
        if (c == '-') {
          SWITCH(ST_SCRIPT_ESC_DASH_DASH);
          text_cp(t, '-');
        }
        else {
          RECONSUME(ST_SCRIPT_DATA);
        }
        break;

      /* 13.2.5.20 Script data escaped state */
      case ST_SCRIPT_ESC:
        GET();
        if (c == '-') {
          SWITCH(ST_SCRIPT_ESC_DASH);
          text_cp(t, '-');
        }
        else if (c == '<') {
          MARK_LT();
          SWITCH(ST_SCRIPT_ESC_LT);
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          text_cp(t, 0xFFFD);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          text_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.21 Script data escaped dash state */
      case ST_SCRIPT_ESC_DASH:
        GET();
        if (c == '-') {
          SWITCH(ST_SCRIPT_ESC_DASH_DASH);
          text_cp(t, '-');
        }
        else if (c == '<') {
          MARK_LT();
          SWITCH(ST_SCRIPT_ESC_LT);
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          SWITCH(ST_SCRIPT_ESC);
          text_cp(t, 0xFFFD);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          SWITCH(ST_SCRIPT_ESC);
          text_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.22 Script data escaped dash dash state */
      case ST_SCRIPT_ESC_DASH_DASH:
        GET();
        if (c == '-') {
          text_cp(t, '-');
        }
        else if (c == '<') {
          MARK_LT();
          SWITCH(ST_SCRIPT_ESC_LT);
        }
        else if (c == '>') {
          SWITCH(ST_SCRIPT_DATA);
          text_cp(t, '>');
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          SWITCH(ST_SCRIPT_ESC);
          text_cp(t, 0xFFFD);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          SWITCH(ST_SCRIPT_ESC);
          text_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.23 Script data escaped less-than sign state */
      case ST_SCRIPT_ESC_LT:
        GET();
        if (c == '/') {
          t->temp.length = 0;
          SWITCH(ST_SCRIPT_ESC_END_OPEN);
        }
        else if (is_alpha(c)) {
          t->temp.length = 0;
          text_at(t, "<", 1, &t->start);
          RECONSUME(ST_SCRIPT_DBL_ESC_START);
        }
        else {
          text_at(t, "<", 1, &t->start);
          RECONSUME(ST_SCRIPT_ESC);
        }
        break;

      /* 13.2.5.26 Script data double escape start state, .31 ... end state */
      case ST_SCRIPT_DBL_ESC_START:
      case ST_SCRIPT_DBL_ESC_END:
        GET();
        if (is_ws(c) || c == '/' || c == '>') {
          const bool is_script = t->temp.length == 6
              && memcmp(t->temp.data, "script", 6) == 0;
          if (t->state == ST_SCRIPT_DBL_ESC_START) {
            SWITCH(is_script ? ST_SCRIPT_DBL_ESC : ST_SCRIPT_ESC);
          }
          else {
            SWITCH(is_script ? ST_SCRIPT_ESC : ST_SCRIPT_DBL_ESC);
          }
          text_cp(t, (uint32_t)c);
        }
        else if (is_upper(c)) {
          temp_word(t, (char)(c + 0x20));
          text_cp(t, (uint32_t)c);
        }
        else if (is_lower(c)) {
          temp_word(t, (char)c);
          text_cp(t, (uint32_t)c);
        }
        else {
          RECONSUME(t->state == ST_SCRIPT_DBL_ESC_START ? ST_SCRIPT_ESC
                                                        : ST_SCRIPT_DBL_ESC);
        }
        break;

      /* 13.2.5.27 Script data double escaped state */
      case ST_SCRIPT_DBL_ESC:
        GET();
        if (c == '-') {
          SWITCH(ST_SCRIPT_DBL_ESC_DASH);
          text_cp(t, '-');
        }
        else if (c == '<') {
          SWITCH(ST_SCRIPT_DBL_ESC_LT);
          text_cp(t, '<');
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          text_cp(t, 0xFFFD);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          text_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.28 Script data double escaped dash state */
      case ST_SCRIPT_DBL_ESC_DASH:
        GET();
        if (c == '-') {
          SWITCH(ST_SCRIPT_DBL_ESC_DASH_DASH);
          text_cp(t, '-');
        }
        else if (c == '<') {
          SWITCH(ST_SCRIPT_DBL_ESC_LT);
          text_cp(t, '<');
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          SWITCH(ST_SCRIPT_DBL_ESC);
          text_cp(t, 0xFFFD);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          SWITCH(ST_SCRIPT_DBL_ESC);
          text_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.29 Script data double escaped dash dash state */
      case ST_SCRIPT_DBL_ESC_DASH_DASH:
        GET();
        if (c == '-') {
          text_cp(t, '-');
        }
        else if (c == '<') {
          SWITCH(ST_SCRIPT_DBL_ESC_LT);
          text_cp(t, '<');
        }
        else if (c == '>') {
          SWITCH(ST_SCRIPT_DATA);
          text_cp(t, '>');
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          SWITCH(ST_SCRIPT_DBL_ESC);
          text_cp(t, 0xFFFD);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          SWITCH(ST_SCRIPT_DBL_ESC);
          text_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.30 Script data double escaped less-than sign state */
      case ST_SCRIPT_DBL_ESC_LT:
        GET();
        if (c == '/') {
          t->temp.length = 0;
          SWITCH(ST_SCRIPT_DBL_ESC_END);
          text_cp(t, '/');
        }
        else {
          RECONSUME(ST_SCRIPT_DBL_ESC);
        }
        break;

      /* 13.2.5.32 Before attribute name state */
      case ST_BEFORE_ATTR_NAME:
        GET();
        if (is_ws(c)) {
          /* Ignore. */
        }
        else if (c == '/' || c == '>' || c == CP_EOF) {
          RECONSUME(ST_AFTER_ATTR_NAME);
        }
        else if (c == '=') {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_EQUALS_SIGN_BEFORE_ATTRIBUTE_NAME);
          attr_begin(t);
          ar_cp(t, '=');
          SWITCH(ST_ATTR_NAME);
        }
        else {
          attr_begin(t);
          RECONSUME(ST_ATTR_NAME);
        }
        break;

      /* 13.2.5.33 Attribute name state */
      case ST_ATTR_NAME:
        GET();
        if (is_ws(c) || c == '/' || c == '>' || c == CP_EOF) {
          attr_name_done(t);
          RECONSUME(ST_AFTER_ATTR_NAME);
        }
        else if (c == '=') {
          attr_name_done(t);
          SWITCH(ST_BEFORE_ATTR_VALUE);
        }
        else if (is_upper(c)) {
          ar_cp(t, (uint32_t)c + 0x20);
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          ar_cp(t, 0xFFFD);
        }
        else {
          if (c == '"' || c == '\'' || c == '<') {
            perr(t, GTEXT_HTML_ERR_UNEXPECTED_CHARACTER_IN_ATTRIBUTE_NAME);
          }
          ar_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.34 After attribute name state */
      case ST_AFTER_ATTR_NAME:
        GET();
        if (is_ws(c)) {
          /* Ignore. */
        }
        else if (c == '/') {
          SWITCH(ST_SELF_CLOSING);
        }
        else if (c == '=') {
          SWITCH(ST_BEFORE_ATTR_VALUE);
        }
        else if (c == '>') {
          SWITCH(ST_DATA);
          tag_emit(t);
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_TAG);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          attr_begin(t);
          RECONSUME(ST_ATTR_NAME);
        }
        break;

      /* 13.2.5.35 Before attribute value state */
      case ST_BEFORE_ATTR_VALUE:
        GET();
        if (is_ws(c)) {
          /* Ignore. */
        }
        else if (c == '"') {
          SWITCH(ST_ATTR_VALUE_DQ);
        }
        else if (c == '\'') {
          SWITCH(ST_ATTR_VALUE_SQ);
        }
        else if (c == '>') {
          perr(t, GTEXT_HTML_ERR_MISSING_ATTRIBUTE_VALUE);
          SWITCH(ST_DATA);
          tag_emit(t);
          return RUN_TOKEN;
        }
        else {
          RECONSUME(ST_ATTR_VALUE_UNQ);
        }
        break;

      /* 13.2.5.36 and .37 Attribute value (double- and single-quoted) states */
      case ST_ATTR_VALUE_DQ:
      case ST_ATTR_VALUE_SQ:
        GET();
        if (c == (t->state == ST_ATTR_VALUE_DQ ? '"' : '\'')) {
          SWITCH(ST_AFTER_ATTR_VALUE_Q);
        }
        else if (c == '&') {
          t->return_state = t->state;
          t->amp = here(t);
          SWITCH(ST_CHARREF);
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          attr_value_cp(t, 0xFFFD);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_TAG);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          attr_value_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.38 Attribute value (unquoted) state */
      case ST_ATTR_VALUE_UNQ:
        GET();
        if (is_ws(c)) {
          SWITCH(ST_BEFORE_ATTR_NAME);
        }
        else if (c == '&') {
          t->return_state = ST_ATTR_VALUE_UNQ;
          t->amp = here(t);
          SWITCH(ST_CHARREF);
        }
        else if (c == '>') {
          SWITCH(ST_DATA);
          tag_emit(t);
          return RUN_TOKEN;
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          attr_value_cp(t, 0xFFFD);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_TAG);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          if (c == '"' || c == '\'' || c == '<' || c == '=' || c == '`') {
            perr(t, GTEXT_HTML_ERR_UNEXPECTED_CHARACTER_IN_UNQUOTED_ATTRIBUTE_VALUE);
          }
          attr_value_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.39 After attribute value (quoted) state */
      case ST_AFTER_ATTR_VALUE_Q:
        GET();
        if (is_ws(c)) {
          SWITCH(ST_BEFORE_ATTR_NAME);
        }
        else if (c == '/') {
          SWITCH(ST_SELF_CLOSING);
        }
        else if (c == '>') {
          SWITCH(ST_DATA);
          tag_emit(t);
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_TAG);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_MISSING_WHITESPACE_BETWEEN_ATTRIBUTES);
          RECONSUME(ST_BEFORE_ATTR_NAME);
        }
        break;

      /* 13.2.5.40 Self-closing start tag state */
      case ST_SELF_CLOSING:
        GET();
        if (c == '>') {
          t->token.self_closing = true;
          SWITCH(ST_DATA);
          tag_emit(t);
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_TAG);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_SOLIDUS_IN_TAG);
          RECONSUME(ST_BEFORE_ATTR_NAME);
        }
        break;

      /* 13.2.5.41 Bogus comment state */
      case ST_BOGUS_COMMENT:
        GET();
        if (c == '>') {
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          ar_cp(t, 0xFFFD);
        }
        else {
          ar_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.42 Markup declaration open state */
      case ST_MARKUP_DECL_OPEN: {
        const lit_result dashes = match_literal(t, "--", 2, false);
        const lit_result doctype = dashes == LIT_YES
            ? LIT_NO : match_literal(t, "doctype", 7, true);
        const lit_result cdata = (dashes == LIT_YES || doctype == LIT_YES)
            ? LIT_NO : match_literal(t, "[CDATA[", 7, false);
        if (dashes == LIT_YES) {
          skip(t, 2);
          comment_begin(t);
          SWITCH(ST_COMMENT_START);
        }
        else if (doctype == LIT_YES) {
          skip(t, 7);
          SWITCH(ST_DOCTYPE);
        }
        else if (cdata == LIT_YES) {
          skip(t, 7);
          if (cdata_allowed(t)) {
            SWITCH(ST_CDATA_SECTION);
          }
          else {
            perr(t, GTEXT_HTML_ERR_CDATA_IN_HTML_CONTENT);
            comment_begin(t);
            ar_put(t, "[CDATA[", 7);
            SWITCH(ST_BOGUS_COMMENT);
          }
        }
        else if (dashes == LIT_NEED || doctype == LIT_NEED
            || cdata == LIT_NEED) {
          return RUN_NEED;
        }
        else {
          /* Reported at the next character, "don't consume anything in the
             current state" notwithstanding: that is where the vectors put it,
             and the next character's own input-stream error comes first, so it
             is read (and put back) before this one is raised. */
          c = consume(t);
          if (c == CP_NEED) {
            return RUN_NEED;
          }
          unread(t);
          perr_next(t, GTEXT_HTML_ERR_INCORRECTLY_OPENED_COMMENT);
          comment_begin(t);
          SWITCH(ST_BOGUS_COMMENT);
        }
        break;
      }

      /* 13.2.5.43 Comment start state */
      case ST_COMMENT_START:
        GET();
        if (c == '-') {
          SWITCH(ST_COMMENT_START_DASH);
        }
        else if (c == '>') {
          perr(t, GTEXT_HTML_ERR_ABRUPT_CLOSING_OF_EMPTY_COMMENT);
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else {
          RECONSUME(ST_COMMENT);
        }
        break;

      /* 13.2.5.44 Comment start dash state */
      case ST_COMMENT_START_DASH:
        GET();
        if (c == '-') {
          SWITCH(ST_COMMENT_END);
        }
        else if (c == '>') {
          perr(t, GTEXT_HTML_ERR_ABRUPT_CLOSING_OF_EMPTY_COMMENT);
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_COMMENT);
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          ar_cp(t, '-');
          RECONSUME(ST_COMMENT);
        }
        break;

      /* 13.2.5.45 Comment state */
      case ST_COMMENT:
        GET();
        if (c == '<') {
          ar_cp(t, '<');
          SWITCH(ST_COMMENT_LT);
        }
        else if (c == '-') {
          SWITCH(ST_COMMENT_END_DASH);
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          ar_cp(t, 0xFFFD);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_COMMENT);
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          ar_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.46 Comment less-than sign state */
      case ST_COMMENT_LT:
        GET();
        if (c == '!') {
          ar_cp(t, '!');
          SWITCH(ST_COMMENT_LT_BANG);
        }
        else if (c == '<') {
          ar_cp(t, '<');
        }
        else {
          RECONSUME(ST_COMMENT);
        }
        break;

      /* 13.2.5.47 Comment less-than sign bang state */
      case ST_COMMENT_LT_BANG:
        GET();
        if (c == '-') {
          SWITCH(ST_COMMENT_LT_BANG_DASH);
        }
        else {
          RECONSUME(ST_COMMENT);
        }
        break;

      /* 13.2.5.48 Comment less-than sign bang dash state */
      case ST_COMMENT_LT_BANG_DASH:
        GET();
        if (c == '-') {
          SWITCH(ST_COMMENT_LT_BANG_DASH_DASH);
        }
        else {
          RECONSUME(ST_COMMENT_END_DASH);
        }
        break;

      /* 13.2.5.49 Comment less-than sign bang dash dash state */
      case ST_COMMENT_LT_BANG_DASH_DASH:
        GET();
        if (c != '>' && c != CP_EOF) {
          perr(t, GTEXT_HTML_ERR_NESTED_COMMENT);
        }
        RECONSUME(ST_COMMENT_END);
        break;

      /* 13.2.5.50 Comment end dash state */
      case ST_COMMENT_END_DASH:
        GET();
        if (c == '-') {
          SWITCH(ST_COMMENT_END);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_COMMENT);
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          ar_cp(t, '-');
          RECONSUME(ST_COMMENT);
        }
        break;

      /* 13.2.5.51 Comment end state */
      case ST_COMMENT_END:
        GET();
        if (c == '>') {
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == '!') {
          SWITCH(ST_COMMENT_END_BANG);
        }
        else if (c == '-') {
          ar_cp(t, '-');
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_COMMENT);
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          ar_put(t, "--", 2);
          RECONSUME(ST_COMMENT);
        }
        break;

      /* 13.2.5.52 Comment end bang state */
      case ST_COMMENT_END_BANG:
        GET();
        if (c == '-') {
          ar_put(t, "--!", 3);
          SWITCH(ST_COMMENT_END_DASH);
        }
        else if (c == '>') {
          perr(t, GTEXT_HTML_ERR_INCORRECTLY_CLOSED_COMMENT);
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_COMMENT);
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          ar_put(t, "--!", 3);
          RECONSUME(ST_COMMENT);
        }
        break;

      /* 13.2.5.53 DOCTYPE state */
      case ST_DOCTYPE:
        GET();
        if (is_ws(c)) {
          SWITCH(ST_BEFORE_DOCTYPE_NAME);
        }
        else if (c == '>') {
          RECONSUME(ST_BEFORE_DOCTYPE_NAME);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          doctype_begin(t);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_MISSING_WHITESPACE_BEFORE_DOCTYPE_NAME);
          RECONSUME(ST_BEFORE_DOCTYPE_NAME);
        }
        break;

      /* 13.2.5.54 Before DOCTYPE name state */
      case ST_BEFORE_DOCTYPE_NAME:
        GET();
        if (is_ws(c)) {
          /* Ignore. */
        }
        else if (c == '>') {
          perr(t, GTEXT_HTML_ERR_MISSING_DOCTYPE_NAME);
          doctype_begin(t);
          t->token.force_quirks = true;
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          doctype_begin(t);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          doctype_begin(t);
          t->token.has_name = true;
          if (is_upper(c)) {
            ar_cp(t, (uint32_t)c + 0x20);
          }
          else if (c == 0) {
            perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
            ar_cp(t, 0xFFFD);
          }
          else {
            ar_cp(t, (uint32_t)c);
          }
          SWITCH(ST_DOCTYPE_NAME);
        }
        break;

      /* 13.2.5.55 DOCTYPE name state */
      case ST_DOCTYPE_NAME:
        GET();
        if (is_ws(c)) {
          t->name_length = t->ar.length;
          SWITCH(ST_AFTER_DOCTYPE_NAME);
        }
        else if (c == '>') {
          t->name_length = t->ar.length;
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (is_upper(c)) {
          ar_cp(t, (uint32_t)c + 0x20);
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          ar_cp(t, 0xFFFD);
        }
        else if (c == CP_EOF) {
          t->name_length = t->ar.length;
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          ar_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.56 After DOCTYPE name state */
      case ST_AFTER_DOCTYPE_NAME:
        GET();
        if (is_ws(c)) {
          /* Ignore. */
        }
        else if (c == '>') {
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          /* "The six characters starting from the current input character". */
          unread(t);
          const lit_result pub = match_literal(t, "public", 6, true);
          const lit_result sys = pub == LIT_YES
              ? LIT_NO : match_literal(t, "system", 6, true);
          if (pub == LIT_YES) {
            skip(t, 6);
            SWITCH(ST_AFTER_DOCTYPE_PUBLIC_KW);
          }
          else if (sys == LIT_YES) {
            skip(t, 6);
            SWITCH(ST_AFTER_DOCTYPE_SYSTEM_KW);
          }
          else if (pub == LIT_NEED || sys == LIT_NEED) {
            return RUN_NEED;
          }
          else {
            perr(t, GTEXT_HTML_ERR_INVALID_CHARACTER_SEQUENCE_AFTER_DOCTYPE_NAME);
            t->token.force_quirks = true;
            SWITCH(ST_BOGUS_DOCTYPE);
          }
        }
        break;

      /* 13.2.5.57 After DOCTYPE public keyword state */
      case ST_AFTER_DOCTYPE_PUBLIC_KW:
        GET();
        if (is_ws(c)) {
          SWITCH(ST_BEFORE_DOCTYPE_PUBLIC_ID);
        }
        else if (c == '"' || c == '\'') {
          perr(t, GTEXT_HTML_ERR_MISSING_WHITESPACE_AFTER_DOCTYPE_PUBLIC_KEYWORD);
          t->token.has_public_id = true;
          t->public_offset = t->ar.length;
          section(t, t->max_value, GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE);
          SWITCH(c == '"' ? ST_DOCTYPE_PUBLIC_ID_DQ : ST_DOCTYPE_PUBLIC_ID_SQ);
        }
        else if (c == '>') {
          perr(t, GTEXT_HTML_ERR_MISSING_DOCTYPE_PUBLIC_IDENTIFIER);
          t->token.force_quirks = true;
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_MISSING_QUOTE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER);
          t->token.force_quirks = true;
          RECONSUME(ST_BOGUS_DOCTYPE);
        }
        break;

      /* 13.2.5.58 Before DOCTYPE public identifier state */
      case ST_BEFORE_DOCTYPE_PUBLIC_ID:
        GET();
        if (is_ws(c)) {
          /* Ignore. */
        }
        else if (c == '"' || c == '\'') {
          t->token.has_public_id = true;
          t->public_offset = t->ar.length;
          section(t, t->max_value, GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE);
          SWITCH(c == '"' ? ST_DOCTYPE_PUBLIC_ID_DQ : ST_DOCTYPE_PUBLIC_ID_SQ);
        }
        else if (c == '>') {
          perr(t, GTEXT_HTML_ERR_MISSING_DOCTYPE_PUBLIC_IDENTIFIER);
          t->token.force_quirks = true;
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_MISSING_QUOTE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER);
          t->token.force_quirks = true;
          RECONSUME(ST_BOGUS_DOCTYPE);
        }
        break;

      /* 13.2.5.59 and .60 DOCTYPE public identifier (quoted) states */
      case ST_DOCTYPE_PUBLIC_ID_DQ:
      case ST_DOCTYPE_PUBLIC_ID_SQ:
        GET();
        if (c == (t->state == ST_DOCTYPE_PUBLIC_ID_DQ ? '"' : '\'')) {
          t->public_length = t->ar.length - t->public_offset;
          SWITCH(ST_AFTER_DOCTYPE_PUBLIC_ID);
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          ar_cp(t, 0xFFFD);
        }
        else if (c == '>') {
          t->public_length = t->ar.length - t->public_offset;
          perr(t, GTEXT_HTML_ERR_ABRUPT_DOCTYPE_PUBLIC_IDENTIFIER);
          t->token.force_quirks = true;
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          t->public_length = t->ar.length - t->public_offset;
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          ar_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.61 After DOCTYPE public identifier state */
      case ST_AFTER_DOCTYPE_PUBLIC_ID:
        GET();
        if (is_ws(c)) {
          SWITCH(ST_BETWEEN_DOCTYPE_IDS);
        }
        else if (c == '>') {
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == '"' || c == '\'') {
          perr(t, GTEXT_HTML_ERR_MISSING_WHITESPACE_BETWEEN_DOCTYPE_PUBLIC_AND_SYSTEM_IDENTIFIERS);
          t->token.has_system_id = true;
          t->system_offset = t->ar.length;
          section(t, t->max_value, GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE);
          SWITCH(c == '"' ? ST_DOCTYPE_SYSTEM_ID_DQ : ST_DOCTYPE_SYSTEM_ID_SQ);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER);
          t->token.force_quirks = true;
          RECONSUME(ST_BOGUS_DOCTYPE);
        }
        break;

      /* 13.2.5.62 Between DOCTYPE public and system identifiers state */
      case ST_BETWEEN_DOCTYPE_IDS:
        GET();
        if (is_ws(c)) {
          /* Ignore. */
        }
        else if (c == '>') {
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == '"' || c == '\'') {
          t->token.has_system_id = true;
          t->system_offset = t->ar.length;
          section(t, t->max_value, GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE);
          SWITCH(c == '"' ? ST_DOCTYPE_SYSTEM_ID_DQ : ST_DOCTYPE_SYSTEM_ID_SQ);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER);
          t->token.force_quirks = true;
          RECONSUME(ST_BOGUS_DOCTYPE);
        }
        break;

      /* 13.2.5.63 After DOCTYPE system keyword state */
      case ST_AFTER_DOCTYPE_SYSTEM_KW:
        GET();
        if (is_ws(c)) {
          SWITCH(ST_BEFORE_DOCTYPE_SYSTEM_ID);
        }
        else if (c == '"' || c == '\'') {
          perr(t, GTEXT_HTML_ERR_MISSING_WHITESPACE_AFTER_DOCTYPE_SYSTEM_KEYWORD);
          t->token.has_system_id = true;
          t->system_offset = t->ar.length;
          section(t, t->max_value, GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE);
          SWITCH(c == '"' ? ST_DOCTYPE_SYSTEM_ID_DQ : ST_DOCTYPE_SYSTEM_ID_SQ);
        }
        else if (c == '>') {
          perr(t, GTEXT_HTML_ERR_MISSING_DOCTYPE_SYSTEM_IDENTIFIER);
          t->token.force_quirks = true;
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER);
          t->token.force_quirks = true;
          RECONSUME(ST_BOGUS_DOCTYPE);
        }
        break;

      /* 13.2.5.64 Before DOCTYPE system identifier state */
      case ST_BEFORE_DOCTYPE_SYSTEM_ID:
        GET();
        if (is_ws(c)) {
          /* Ignore. */
        }
        else if (c == '"' || c == '\'') {
          t->token.has_system_id = true;
          t->system_offset = t->ar.length;
          section(t, t->max_value, GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE);
          SWITCH(c == '"' ? ST_DOCTYPE_SYSTEM_ID_DQ : ST_DOCTYPE_SYSTEM_ID_SQ);
        }
        else if (c == '>') {
          perr(t, GTEXT_HTML_ERR_MISSING_DOCTYPE_SYSTEM_IDENTIFIER);
          t->token.force_quirks = true;
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER);
          t->token.force_quirks = true;
          RECONSUME(ST_BOGUS_DOCTYPE);
        }
        break;

      /* 13.2.5.65 and .66 DOCTYPE system identifier (quoted) states */
      case ST_DOCTYPE_SYSTEM_ID_DQ:
      case ST_DOCTYPE_SYSTEM_ID_SQ:
        GET();
        if (c == (t->state == ST_DOCTYPE_SYSTEM_ID_DQ ? '"' : '\'')) {
          t->system_length = t->ar.length - t->system_offset;
          SWITCH(ST_AFTER_DOCTYPE_SYSTEM_ID);
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
          ar_cp(t, 0xFFFD);
        }
        else if (c == '>') {
          t->system_length = t->ar.length - t->system_offset;
          perr(t, GTEXT_HTML_ERR_ABRUPT_DOCTYPE_SYSTEM_IDENTIFIER);
          t->token.force_quirks = true;
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          t->system_length = t->ar.length - t->system_offset;
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          ar_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.67 After DOCTYPE system identifier state */
      case ST_AFTER_DOCTYPE_SYSTEM_ID:
        GET();
        if (is_ws(c)) {
          /* Ignore. */
        }
        else if (c == '>') {
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_DOCTYPE);
          t->token.force_quirks = true;
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_CHARACTER_AFTER_DOCTYPE_SYSTEM_IDENTIFIER);
          RECONSUME(ST_BOGUS_DOCTYPE);
        }
        break;

      /* 13.2.5.68 Bogus DOCTYPE state */
      case ST_BOGUS_DOCTYPE:
        GET();
        if (c == '>') {
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == 0) {
          perr(t, GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER);
        }
        else if (c == CP_EOF) {
          t->token_ready = true;
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        break;

      /* 13.2.5.69 CDATA section state */
      case ST_CDATA_SECTION:
        GET();
        if (c == ']') {
          SWITCH(ST_CDATA_BRACKET);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_CDATA);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          text_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.70 CDATA section bracket state */
      case ST_CDATA_BRACKET:
        GET();
        if (c == ']') {
          SWITCH(ST_CDATA_END);
        }
        else {
          text_cp(t, ']');
          RECONSUME(ST_CDATA_SECTION);
        }
        break;

      /* 13.2.5.71 CDATA section end state */
      case ST_CDATA_END:
        GET();
        if (c == ']') {
          text_cp(t, ']');
        }
        else if (c == '>') {
          SWITCH(ST_DATA);
        }
        else {
          text_cp(t, ']');
          text_cp(t, ']');
          RECONSUME(ST_CDATA_SECTION);
        }
        break;

      /* 13.2.5.72 Processing instruction open state */
      case ST_PI_OPEN:
        GET();
        if (is_alpha(c) || c == '_') {
          RECONSUME(ST_PI_TARGET);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_PROCESSING_INSTRUCTION);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_INVALID_FIRST_CHARACTER_OF_PROCESSING_INSTRUCTION_TARGET);
          comment_begin(t);
          ar_put(t, "?", 1);
          ar_put(t, t->temp.data, t->temp.length);
          RECONSUME(ST_BOGUS_COMMENT);
        }
        break;

      /* 13.2.5.73 Processing instruction target state */
      case ST_PI_TARGET:
        GET();
        if (is_ws(c) || c == '?' || c == '>') {
          if (ascii_equals_fold(t->temp.data, t->temp.length, "xml")
              || ascii_equals_fold(
                  t->temp.data, t->temp.length, "xml-stylesheet")) {
            perr(t, GTEXT_HTML_ERR_DISALLOWED_PROCESSING_INSTRUCTION_TARGET);
            comment_begin(t);
            ar_put(t, "?", 1);
            ar_put(t, t->temp.data, t->temp.length);
            RECONSUME(ST_BOGUS_COMMENT);
          }
          else {
            token_begin(t, GTEXT_HTML_TOKEN_PROCESSING_INSTRUCTION);
            ar_put(t, t->temp.data, t->temp.length);
            t->name_length = t->ar.length;
            t->data_offset = t->ar.length;
            section(t, t->max_comment, GTEXT_HTML_TOKENIZER_E_LIMIT_COMMENT);
            RECONSUME(ST_AFTER_PI_TARGET);
          }
        }
        else if (is_alnum(c) || c == '-' || c == '_') {
          temp_put(t, (char)c);
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_PROCESSING_INSTRUCTION);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          perr(t, GTEXT_HTML_ERR_INVALID_PROCESSING_INSTRUCTION_TARGET);
          comment_begin(t);
          ar_put(t, "?", 1);
          ar_put(t, t->temp.data, t->temp.length);
          RECONSUME(ST_BOGUS_COMMENT);
        }
        break;

      /* 13.2.5.74 After processing instruction target state */
      case ST_AFTER_PI_TARGET:
        GET();
        if (is_ws(c)) {
          /* Ignore. */
        }
        else {
          RECONSUME(ST_PI_DATA);
        }
        break;

      /* 13.2.5.75 Processing instruction data state */
      case ST_PI_DATA:
        GET();
        if (c == '?') {
          SWITCH(ST_PI_QUESTIONABLE);
        }
        else if (c == '>') {
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_PROCESSING_INSTRUCTION);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          ar_cp(t, (uint32_t)c);
        }
        break;

      /* 13.2.5.76 Processing instruction questionable state */
      case ST_PI_QUESTIONABLE:
        GET();
        if (c == '>') {
          SWITCH(ST_DATA);
          t->token_ready = true;
          return RUN_TOKEN;
        }
        else if (c == CP_EOF) {
          perr(t, GTEXT_HTML_ERR_EOF_IN_PROCESSING_INSTRUCTION);
          t->eof_ready = true;
          return RUN_TOKEN;
        }
        else {
          ar_cp(t, '?');
          RECONSUME(ST_PI_DATA);
        }
        break;

      /* 13.2.5.77 Character reference state */
      case ST_CHARREF:
        t->cr[0] = '&';
        t->cr_length = 1;
        GET();
        if (is_alnum(c)) {
          RECONSUME(ST_NAMED_CHARREF);
        }
        else if (c == '#') {
          t->cr[t->cr_length++] = '#';
          t->code = 0;
          SWITCH(ST_NUMERIC_CHARREF);
        }
        else {
          flush_bytes(t, t->cr, t->cr_length);
          RECONSUME(t->return_state);
        }
        break;

      /* 13.2.5.78 Named character reference state */
      case ST_NAMED_CHARREF: {
        /* The window: the alphanumeric run at the cursor, and a `;` after it
           if it is short enough that a name could end there. No name is longer
           than the table's longest, so nothing past that can matter. */
        char window[NAME_WINDOW];
        size_t n = 0;
        const size_t longest = gtext_html_charref_longest < NAME_WINDOW - 1
            ? gtext_html_charref_longest : NAME_WINDOW - 2;
        size_t p = t->pos;
        while (n < longest && p < t->in_length && is_alnum(t->in[p])) {
          window[n++] = (char)t->in[p++];
        }
        t->work += n + 1;
        if (p >= t->in_length && n < longest && !t->final) {
          return RUN_NEED;
        }
        if (n < longest && p < t->in_length && t->in[p] == ';') {
          window[n++] = ';';
        }
        const GTEXT_HTML_CharRef * match =
            gtext_html_charref_longest_match(window, n);
        if (!match) {
          /* "Flush code points consumed as a character reference. Switch to
             the ambiguous ampersand state." Only the `&` was consumed. */
          flush_bytes(t, "&", 1);
          SWITCH(ST_AMBIGUOUS_AMP);
          break;
        }
        const size_t len = match->name_length;
        const char last = gtext_html_charref_name(match)[len - 1];
        if (in_attribute(t) && last != ';') {
          const size_t after = t->pos + len;
          if (after >= t->in_length && !t->final) {
            return RUN_NEED;
          }
          const int32_t next = after < t->in_length ? t->in[after] : CP_EOF;
          if (next == '=' || is_alnum(next)) {
            /* For historical reasons the reference is left as it was written. */
            char text[NAME_WINDOW + 1];
            text[0] = '&';
            memcpy(text + 1, gtext_html_charref_name(match), len);
            skip(t, len);
            flush_bytes(t, text, len + 1);
            SWITCH(t->return_state);
            break;
          }
        }
        skip(t, len);
        if (last != ';') {
          perr_next(t, GTEXT_HTML_ERR_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE);
        }
        flush_cp(t, match->first);
        if (match->code_points == 2) {
          flush_cp(t, match->second);
        }
        SWITCH(t->return_state);
        break;
      }

      /* 13.2.5.79 Ambiguous ampersand state */
      case ST_AMBIGUOUS_AMP:
        GET();
        if (is_alnum(c)) {
          if (in_attribute(t)) {
            attr_value_cp(t, (uint32_t)c);
          }
          else {
            const char ch = (char)c;
            text_put(t, &ch, 1, &t->amp, false, false);
          }
        }
        else if (c == ';') {
          perr(t, GTEXT_HTML_ERR_UNKNOWN_NAMED_CHARACTER_REFERENCE);
          RECONSUME(t->return_state);
        }
        else {
          RECONSUME(t->return_state);
        }
        break;

      /* 13.2.5.80 Numeric character reference state */
      case ST_NUMERIC_CHARREF:
        GET();
        if (c == 'x' || c == 'X') {
          t->cr[t->cr_length++] = (char)c;
          SWITCH(ST_HEX_CHARREF_START);
        }
        else if (is_digit(c)) {
          RECONSUME(ST_DEC_CHARREF);
        }
        else {
          perr(t, GTEXT_HTML_ERR_ABSENCE_OF_DIGITS_IN_NUMERIC_CHARACTER_REFERENCE);
          flush_bytes(t, t->cr, t->cr_length);
          RECONSUME(t->return_state);
        }
        break;

      /* 13.2.5.81 Hexadecimal character reference start state */
      case ST_HEX_CHARREF_START:
        GET();
        if (is_hex(c)) {
          RECONSUME(ST_HEX_CHARREF);
        }
        else {
          perr(t, GTEXT_HTML_ERR_ABSENCE_OF_DIGITS_IN_NUMERIC_CHARACTER_REFERENCE);
          flush_bytes(t, t->cr, t->cr_length);
          RECONSUME(t->return_state);
        }
        break;

      /* 13.2.5.82 Hexadecimal character reference state */
      case ST_HEX_CHARREF:
        GET();
        if (is_hex(c)) {
          const uint32_t digit = is_digit(c) ? (uint32_t)(c - '0')
              : is_upper(c) ? (uint32_t)(c - 'A' + 10)
                            : (uint32_t)(c - 'a' + 10);
          t->code = accumulate(t->code, 16, digit);
        }
        else if (c == ';') {
          SWITCH(ST_NUMERIC_CHARREF_END);
        }
        else {
          perr(t, GTEXT_HTML_ERR_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE);
          RECONSUME(ST_NUMERIC_CHARREF_END);
        }
        break;

      /* 13.2.5.83 Decimal character reference state */
      case ST_DEC_CHARREF:
        GET();
        if (is_digit(c)) {
          t->code = accumulate(t->code, 10, (uint32_t)(c - '0'));
        }
        else if (c == ';') {
          SWITCH(ST_NUMERIC_CHARREF_END);
        }
        else {
          perr(t, GTEXT_HTML_ERR_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE);
          RECONSUME(ST_NUMERIC_CHARREF_END);
        }
        break;

      /* 13.2.5.84 Numeric character reference end state */
      case ST_NUMERIC_CHARREF_END:
        numeric_end(t);
        SWITCH(t->return_state);
        break;
    }
  }
}

/* ---- The public surface ------------------------------------------------- */

static size_t limit_or(size_t given, size_t fallback) {
  return given ? given : fallback;
}

GTEXT_HTML_Tokenizer * gtext_html_tokenizer_new(
    const GTEXT_HTML_TokenizerOptions * options) {
  GTEXT_HTML_TokenizerOptions use;
  memset(&use, 0, sizeof(use));
  if (options) {
    use = *options;
  }
  const GTEXT_Allocator * allocator =
      use.allocator ? use.allocator : gtext_allocator_default();
  GTEXT_HTML_Tokenizer * t = gtext_allocator_calloc(allocator, 1, sizeof(*t));
  if (!t) {
    return NULL;
  }
  t->allocator = *allocator;
  t->max_input = limit_or(use.max_input_bytes,
      GTEXT_HTML_TOKENIZER_DEFAULT_MAX_INPUT);
  t->max_name = limit_or(use.max_name_bytes,
      GTEXT_HTML_TOKENIZER_DEFAULT_MAX_NAME);
  t->max_value = limit_or(use.max_value_bytes,
      GTEXT_HTML_TOKENIZER_DEFAULT_MAX_VALUE);
  t->max_comment = limit_or(use.max_comment_bytes,
      GTEXT_HTML_TOKENIZER_DEFAULT_MAX_COMMENT);
  t->max_text = limit_or(use.max_text_bytes,
      GTEXT_HTML_TOKENIZER_DEFAULT_MAX_TEXT);
  t->max_attributes = limit_or(use.max_attributes,
      GTEXT_HTML_TOKENIZER_DEFAULT_MAX_ATTRIBUTES);
  t->max_errors = limit_or(use.max_errors,
      GTEXT_HTML_TOKENIZER_DEFAULT_MAX_ERRORS);
  t->allow_cdata = use.allow_cdata;
  t->allow_cdata_user = use.allow_cdata_user;
  t->wtf8 = use.wtf8;
  if (use.count_errors_only) {
    t->max_errors = 0;
    t->keep_no_errors = true;
  }
  t->hash_seed = use.hash_seed;
  if (!t->hash_seed) {
    /* A different seed for each tokenizer made in the process, in the order they are made: the
       same on every run of a single-threaded program, and not the same for two tokenizers. */
    static _Atomic uint64_t instances;
    uint64_t z = (atomic_fetch_add(&instances, 1) + 1) * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    t->hash_seed = (z ^ (z >> 31)) | 1;
  }
  t->cdata_override = GTEXT_HTML_CDATA_DEFAULT;
  t->state = ST_DATA;
  t->return_state = ST_DATA;
  t->line = 1;
  t->column = 1;
  t->prev_line = 1;
  t->prev_column = 1;
  t->start.line = 1;
  t->start.column = 1;
  t->amp = t->start;
  return t;
}

void gtext_html_tokenizer_free(GTEXT_HTML_Tokenizer * t) {
  if (!t) {
    return;
  }
  GTEXT_Allocator a = t->allocator;
  gtext_allocator_free(&a, t->pend);
  gtext_allocator_free(&a, t->text.data);
  gtext_allocator_free(&a, t->ar.data);
  gtext_allocator_free(&a, t->temp.data);
  gtext_allocator_free(&a, t->last_start.data);
  gtext_allocator_free(&a, t->attrs);
  gtext_allocator_free(&a, t->names);
  gtext_allocator_free(&a, t->attr_out);
  gtext_allocator_free(&a, t->errors);
  gtext_allocator_free(&a, t);
}

/* How many bytes of a chunk are copied after a tail at a time. */
#define FEED_WINDOW 256

/* Make the next bytes of the caller's chunk (`rest`) visible. If everything before them has
   been read, the machine reads the chunk itself, in place. If a tail is waiting for the rest of
   its character or look-ahead, the tail and the first FEED_WINDOW bytes are joined in `pend`
   and read there, and the rest of the chunk waits: what is copied is bounded by the
   tail and the window, not by the chunk, and each byte is copied once. */
static void advance_input(GTEXT_HTML_Tokenizer * t) {
  const size_t tail = t->in_length - t->pos;
  if (tail == 0) {
    t->base_offset += t->pos;
    t->in = t->rest;
    t->in_length = t->rest_length;
    t->pos = 0;
    t->prev_pos = 0;
    t->rest = NULL;
    t->rest_length = 0;
    t->pend_length = 0;
    return;
  }
  const size_t take = t->rest_length < FEED_WINDOW ? t->rest_length : FEED_WINDOW;
  if (tail + take > t->pend_capacity) {
    size_t cap = t->pend_capacity ? t->pend_capacity : 512;
    while (cap < tail + take) {
      cap *= 2;
    }
    uint8_t * grown = gtext_allocator_malloc(&t->allocator, cap);
    if (!grown) {
      fail(t, GTEXT_HTML_TOKENIZER_E_OOM);
      return;
    }
    memcpy(grown, t->in + t->pos, tail);
    gtext_allocator_free(&t->allocator, t->pend);
    t->pend = grown;
    t->pend_capacity = cap;
  }
  else {
    memmove(t->pend, t->in + t->pos, tail);
  }
  memcpy(t->pend + tail, t->rest, take);
  t->work += tail + take;
  t->base_offset += t->pos;
  t->in = t->pend;
  t->in_length = tail + take;
  t->pend_length = t->in_length;
  t->pos = 0;
  t->prev_pos = 0;
  t->rest += take;
  t->rest_length -= take;
  if (t->rest_length == 0) {
    t->rest = NULL;
    t->borrowed = false;
  }
}

GTEXT_HTML_TokenizerStatus gtext_html_tokenizer_feed(
    GTEXT_HTML_Tokenizer * t, const void * data, size_t length) {
  if (!t || (!data && length)) {
    return GTEXT_HTML_TOKENIZER_E_INVALID;
  }
  if (t->has_failed) {
    return t->failed;
  }
  if (t->borrowed || t->final) {
    return GTEXT_HTML_TOKENIZER_E_STATE;
  }
  if (length > t->max_input
      || t->total_fed > t->max_input - length) {
    fail(t, GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT);
    return t->failed;
  }
  if (length == 0) {
    return GTEXT_HTML_TOKENIZER_OK;
  }
  t->total_fed += length;
  t->rest = (const uint8_t *)data;
  t->rest_length = length;
  t->borrowed = true;
  advance_input(t);
  return t->has_failed ? t->failed : GTEXT_HTML_TOKENIZER_OK;
}

void gtext_html_tokenizer_finish(GTEXT_HTML_Tokenizer * t) {
  if (t) {
    t->final = true;
  }
}

/* The machine needs more than is here: keep what it has not used, and let go
   of the caller's chunk. */
static void keep_tail(GTEXT_HTML_Tokenizer * t) {
  const size_t rest = t->in_length - t->pos;
  if (rest > 0) {
    if (rest > t->pend_capacity) {
      size_t cap = t->pend_capacity ? t->pend_capacity : 256;
      while (cap < rest) {
        cap *= 2;
      }
      uint8_t * grown = gtext_allocator_malloc(&t->allocator, cap);
      if (!grown) {
        fail(t, GTEXT_HTML_TOKENIZER_E_OOM);
        return;
      }
      memcpy(grown, t->in + t->pos, rest);
      gtext_allocator_free(&t->allocator, t->pend);
      t->pend = grown;
      t->pend_capacity = cap;
    }
    else {
      memmove(t->pend, t->in + t->pos, rest);
    }
    t->work += rest;
  }
  t->pend_length = rest;
  t->base_offset += t->pos;
  t->in = t->pend;
  t->in_length = rest;
  t->pos = 0;
  t->prev_pos = 0; /* unread() is only ever used before the machine returns */
  t->borrowed = false;
}

GTEXT_HTML_TokenizerStatus gtext_html_tokenizer_next(
    GTEXT_HTML_Tokenizer * t, const GTEXT_HTML_Token ** out) {
  if (!t || !out) {
    return GTEXT_HTML_TOKENIZER_E_INVALID;
  }
  if (t->has_failed) {
    return t->failed;
  }
  if (t->text_taken) {
    t->text.length = 0;
    t->text_taken = false;
  }
  if (t->done) {
    return GTEXT_HTML_TOKENIZER_DONE;
  }
  for (;;) {
    if (t->token_ready || t->eof_ready) {
      /* Characters first: they came before whatever ended the run. */
      if (t->text.length > 0) {
        GTEXT_HTML_Token * k = &t->out;
        memset(k, 0, sizeof(*k));
        k->type = GTEXT_HTML_TOKEN_CHARACTER;
        k->data = t->text.data;
        k->data_length = t->text.length;
        k->all_whitespace = t->text_whitespace;
        k->has_null = t->text_null;
        k->line = t->text_where.line;
        k->column = t->text_where.column;
        k->offset = t->text_where.offset;
        t->text_taken = true;
        *out = k;
        return GTEXT_HTML_TOKENIZER_TOKEN;
      }
      if (t->token_ready) {
        if (!token_finish(t)) {
          return t->failed;
        }
        t->token_ready = false;
        t->out = t->token;
        *out = &t->out;
        return GTEXT_HTML_TOKENIZER_TOKEN;
      }
      GTEXT_HTML_Token * k = &t->out;
      memset(k, 0, sizeof(*k));
      k->type = GTEXT_HTML_TOKEN_EOF;
      k->line = t->line;
      k->column = t->column;
      k->offset = t->base_offset + t->pos;
      t->eof_ready = false;
      t->done = true;
      *out = k;
      return GTEXT_HTML_TOKENIZER_TOKEN;
    }
    const run_result r = run(t);
    if (t->has_failed) {
      return t->failed;
    }
    if (r == RUN_NEED) {
      if (t->rest_length > 0) {
        advance_input(t);
        if (t->has_failed) {
          return t->failed;
        }
        continue;
      }
      keep_tail(t);
      if (t->has_failed) {
        return t->failed;
      }
      return GTEXT_HTML_TOKENIZER_NEED_INPUT;
    }
  }
}

GTEXT_HTML_TokenizerStatus gtext_html_tokenizer_set_state(
    GTEXT_HTML_Tokenizer * t, GTEXT_HTML_TokenizerState state) {
  if (!t) {
    return GTEXT_HTML_TOKENIZER_E_INVALID;
  }
  switch (state) {
    case GTEXT_HTML_STATE_DATA:
      t->state = ST_DATA;
      break;
    case GTEXT_HTML_STATE_RCDATA:
      t->state = ST_RCDATA;
      break;
    case GTEXT_HTML_STATE_RAWTEXT:
      t->state = ST_RAWTEXT;
      break;
    case GTEXT_HTML_STATE_SCRIPT_DATA:
      t->state = ST_SCRIPT_DATA;
      break;
    case GTEXT_HTML_STATE_PLAINTEXT:
      t->state = ST_PLAINTEXT;
      break;
    case GTEXT_HTML_STATE_CDATA_SECTION:
      t->state = ST_CDATA_SECTION;
      break;
    default:
      return GTEXT_HTML_TOKENIZER_E_INVALID;
  }
  return GTEXT_HTML_TOKENIZER_OK;
}

GTEXT_HTML_TokenizerState gtext_html_tokenizer_get_state(
    const GTEXT_HTML_Tokenizer * t) {
  if (!t) {
    return GTEXT_HTML_STATE_DATA;
  }
  switch (t->state) {
    case ST_RCDATA:
      return GTEXT_HTML_STATE_RCDATA;
    case ST_RAWTEXT:
      return GTEXT_HTML_STATE_RAWTEXT;
    case ST_SCRIPT_DATA:
      return GTEXT_HTML_STATE_SCRIPT_DATA;
    case ST_PLAINTEXT:
      return GTEXT_HTML_STATE_PLAINTEXT;
    case ST_CDATA_SECTION:
      return GTEXT_HTML_STATE_CDATA_SECTION;
    default:
      return GTEXT_HTML_STATE_DATA;
  }
}

GTEXT_HTML_TokenizerStatus gtext_html_tokenizer_set_last_start_tag(
    GTEXT_HTML_Tokenizer * t, const char * name, size_t length) {
  if (!t || (!name && length)) {
    return GTEXT_HTML_TOKENIZER_E_INVALID;
  }
  if (!name) {
    t->have_last_start = false;
    t->last_start.length = 0;
    return GTEXT_HTML_TOKENIZER_OK;
  }
  if (length > t->max_name) {
    return GTEXT_HTML_TOKENIZER_E_LIMIT_NAME;
  }
  t->last_start.length = 0;
  vec_put(t, &t->last_start, name, length);
  if (t->has_failed) {
    return t->failed;
  }
  t->have_last_start = true;
  return GTEXT_HTML_TOKENIZER_OK;
}

void gtext_html_tokenizer_set_allow_cdata(
    GTEXT_HTML_Tokenizer * t, GTEXT_HTML_CdataOverride mode) {
  if (t && (mode == GTEXT_HTML_CDATA_DEFAULT || mode == GTEXT_HTML_CDATA_ALLOW
          || mode == GTEXT_HTML_CDATA_REFUSE)) {
    t->cdata_override = mode;
  }
}

size_t gtext_html_tokenizer_error_count(const GTEXT_HTML_Tokenizer * t) {
  return t ? t->error_count : 0;
}

const GTEXT_HTML_ParseError * gtext_html_tokenizer_error_at(
    const GTEXT_HTML_Tokenizer * t, size_t index) {
  return t && index < t->error_count ? &t->errors[index] : NULL;
}

size_t gtext_html_tokenizer_errors_dropped(const GTEXT_HTML_Tokenizer * t) {
  return t ? t->errors_dropped : 0;
}

size_t gtext_html_tokenizer_work(const GTEXT_HTML_Tokenizer * t) {
  return t ? t->work : 0;
}
