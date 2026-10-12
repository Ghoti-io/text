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
 * The HTML serializer: the WHATWG "serializing HTML fragments" algorithm as a token stream with a
 * one-token lookahead, and a tree walker that feeds it.
 *
 * ## Shape
 *
 * Three layers, each small.
 *
 * 1. **The front** (`fe_*`, the public `gtext_html_serializer_*` functions): checks arguments and, with
 *    `inject_meta_charset`, holds the tokens of a `head` back until its end tag, because whether a `meta`
 *    has to be added depends on the whole `head`. The held tokens are kept as bytes in one growing arena
 *    and replayed through the next layer.
 * 2. **The stream** (`s2_*`): the options that change tokens (white space), the stack of open elements
 *    (the parent of a text, of an end tag), and the lookahead. A start or end tag that the HTML syntax may
 *    leave out is not written when it arrives: it is *pending*, with the little that is needed to write it
 *    later, and the next token (or the end of the stream) decides. One slot is enough because a tag is
 *    only ever pending while the token after it is awaited, and the token after it can only be asked about
 *    its kind, its name and whether its text starts with white space - never about anything that is still
 *    to come. That is the whole of the lookahead.
 * 3. **The writer** (`out`, `put_*`): one narrow door that counts bytes against the limit and batches them
 *    for the sink; the escapes; the attributes.
 *
 * The tree walk is an explicit stack of (element, next sibling) pairs, so any depth is a heap array and not
 * the C stack. The stack of open elements in the stream is a second array of two bytes per element. Both
 * are bounded by `max_depth`.
 *
 * ## What is not here
 *
 * No second pass and no refusal: HTML has a spelling for every tree (html_serialize.h). Memory is the
 * output buffer, the two arrays above, and with `inject_meta_charset` the tokens of one `head`.
 *
 * Nothing here is mutable outside the objects of one call; there are no statics but constant tables.
 */

#include <ghoti.io/text/macros.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../markup/markup_internal.h"
#include "html_api_internal.h"

/// The size of the buffer the sink is given pieces of.
#define SER_BUFFER 4096
/// The default depth limit (twice the parser's default of open elements).
#define SER_DEFAULT_MAX_DEPTH 1024
/// The default output limit, 256 MiB.
#define SER_DEFAULT_MAX_OUTPUT ((size_t)256 * 1024 * 1024)

/* ---- the elements the rules name ----------------------------------------- */

/* Sorted by strcmp: the lookup is a binary search, and html_serialize_table_sorted() holds the order. */
enum {
  I_NONE = 0,
  I_A, I_ADDRESS, I_ARTICLE, I_ASIDE, I_AUDIO, I_BLOCKQUOTE, I_BODY, I_CAPTION, I_COL, I_COLGROUP,
  I_DD, I_DEL, I_DETAILS, I_DIALOG, I_DIV, I_DL, I_DT, I_FIELDSET, I_FIGCAPTION, I_FIGURE, I_FOOTER,
  I_FORM, I_H1, I_H2, I_H3, I_H4, I_H5, I_H6, I_HEAD, I_HEADER, I_HGROUP, I_HR, I_HTML, I_IFRAME,
  I_INS, I_LI, I_LINK, I_MAIN, I_MAP, I_MENU, I_META, I_NAV, I_NOEMBED, I_NOFRAMES, I_NOSCRIPT, I_OL,
  I_OPTGROUP, I_OPTION, I_P, I_PLAINTEXT, I_PRE, I_RP, I_RT, I_SCRIPT, I_SEARCH, I_SECTION, I_STYLE,
  I_TABLE, I_TBODY, I_TD, I_TEMPLATE, I_TEXTAREA, I_TFOOT, I_TH, I_THEAD, I_TR, I_UL, I_VIDEO, I_XMP,
  I_COUNT
};

enum {
  EF_RAW = 1,           /* text inside is written as it is */
  EF_RAW_SCRIPTING = 2, /* ... only with the scripting flag (noscript) */
  EF_PRESERVE = 4,      /* strip_whitespace leaves the text inside alone */
  EF_PNEXT = 8,         /* a p's end tag may be omitted when this start tag follows */
  EF_NOP = 16           /* a p's end tag stays at the end of this parent (a, audio, del, ins, map, noscript, video) */
};

/// A row of the table of elements the rules name.
typedef struct {
  const char * name; ///< The name, lower case.
  uint8_t flags;     ///< EF_*.
} ElementInfo;

static const ElementInfo k_elements[] = {
  {"a", EF_NOP}, {"address", EF_PNEXT}, {"article", EF_PNEXT}, {"aside", EF_PNEXT}, {"audio", EF_NOP},
  {"blockquote", EF_PNEXT}, {"body", 0}, {"caption", 0}, {"col", 0}, {"colgroup", 0},
  {"dd", 0}, {"del", EF_NOP}, {"details", EF_PNEXT}, {"dialog", EF_PNEXT}, {"div", EF_PNEXT},
  {"dl", EF_PNEXT}, {"dt", 0}, {"fieldset", EF_PNEXT}, {"figcaption", EF_PNEXT}, {"figure", EF_PNEXT},
  {"footer", EF_PNEXT}, {"form", EF_PNEXT}, {"h1", EF_PNEXT}, {"h2", EF_PNEXT}, {"h3", EF_PNEXT},
  {"h4", EF_PNEXT}, {"h5", EF_PNEXT}, {"h6", EF_PNEXT}, {"head", 0}, {"header", EF_PNEXT},
  {"hgroup", EF_PNEXT}, {"hr", EF_PNEXT}, {"html", 0}, {"iframe", EF_RAW | EF_PRESERVE},
  {"ins", EF_NOP}, {"li", 0}, {"link", 0}, {"main", EF_PNEXT}, {"map", EF_NOP}, {"menu", EF_PNEXT},
  {"meta", 0}, {"nav", EF_PNEXT}, {"noembed", EF_RAW | EF_PRESERVE}, {"noframes", EF_RAW | EF_PRESERVE},
  {"noscript", EF_RAW_SCRIPTING | EF_PRESERVE | EF_NOP}, {"ol", EF_PNEXT}, {"optgroup", 0},
  {"option", 0}, {"p", EF_PNEXT}, {"plaintext", EF_RAW | EF_PRESERVE}, {"pre", EF_PNEXT | EF_PRESERVE},
  {"rp", 0}, {"rt", 0}, {"script", EF_RAW | EF_PRESERVE}, {"search", EF_PNEXT}, {"section", EF_PNEXT},
  {"style", EF_RAW | EF_PRESERVE}, {"table", EF_PNEXT}, {"tbody", 0}, {"td", 0}, {"template", 0},
  {"textarea", EF_PRESERVE}, {"tfoot", 0}, {"th", 0}, {"thead", 0}, {"tr", 0}, {"ul", EF_PNEXT},
  {"video", EF_NOP}, {"xmp", EF_RAW | EF_PRESERVE}
};

_Static_assert(sizeof(k_elements) / sizeof(k_elements[0]) == I_COUNT - 1, "one table row for every id");

/// Is the element table in strcmp order (what the binary search needs)? For the tests.
bool html_serialize_table_sorted(void);
bool html_serialize_table_sorted(void) {
  for (size_t i = 1; i < sizeof(k_elements) / sizeof(k_elements[0]); i++) {
    if (strcmp(k_elements[i - 1].name, k_elements[i].name) >= 0) {
      return false;
    }
  }
  return true;
}

static unsigned element_id(const char * name) {
  size_t lo = 0;
  size_t hi = sizeof(k_elements) / sizeof(k_elements[0]);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = strcmp(name, k_elements[mid].name);
    if (c == 0) {
      return (unsigned)mid + 1;
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return I_NONE;
}

static const char * id_name(unsigned id) {
  return k_elements[id - 1].name;
}

static unsigned id_flags(unsigned id) {
  return id == I_NONE ? 0u : k_elements[id - 1].flags;
}

/* An autonomous custom element's name (the standard's "valid custom element name", the common part of the
   rule: it starts with a lower case ASCII letter, has a hyphen, has no upper case ASCII letter, white space
   or `/`, `>` or NUL, and is not one of the names the standard reserves). */
static bool is_custom_name(const char * n) {
  static const char * const reserved[] = {"annotation-xml", "color-profile", "font-face", "font-face-format",
      "font-face-name", "font-face-src", "font-face-uri", "missing-glyph"};
  if (!(n[0] >= 'a' && n[0] <= 'z')) {
    return false;
  }
  bool hyphen = false;
  for (const char * p = n; *p; p++) {
    const char c = *p;
    if (c == '-') {
      hyphen = true;
    }
    else if ((c >= 'A' && c <= 'Z') || c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == '/'
        || c == '>') {
      return false;
    }
  }
  if (!hyphen) {
    return false;
  }
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
    if (strcmp(n, reserved[i]) == 0) {
      return false;
    }
  }
  return true;
}

/* ---- state --------------------------------------------------------------- */

enum {
  FR_HTML = 1,  /* an HTML-namespace element */
  FR_CUSTOM = 2, /* ... that is an autonomous custom element */
  FR_PRESERVE = 4 /* it raised the white space preservation count, and lowers it when it closes */
};

/// An open element, as far as the rules care.
typedef struct {
  uint8_t id;    ///< I_*, or I_NONE.
  uint8_t flags; ///< FR_*.
} Frame;

enum { PEND_NONE = 0, PEND_START, PEND_END };
enum { N_NONE = 0, N_START, N_END, N_TEXT, N_COMMENT, N_OTHER };
enum { IJ_PRE = 0, IJ_IN_HEAD, IJ_POST };

/// What the lookahead asks of the token after a held tag.
typedef struct {
  uint8_t kind;  ///< N_*.
  uint8_t id;    ///< N_START: the HTML element's I_*, else I_NONE.
  bool white;    ///< N_TEXT: starts with ASCII white space.
} Next;

/// The serializer: the options, the sink and its buffer, the stack of open elements, the one held tag, and the queue of
/// the tokens of a head.
struct GTEXT_HTML_Serializer {
  GTEXT_Allocator alloc;           ///< The allocator, copied.
  GTEXT_HTML_Serialize_Options o;  ///< The options, resolved: no zeros left to mean a default.
  GTEXT_Markup_Sink sink;          ///< Where the bytes go.
  GTEXT_HTML_Status status;        ///< The sticky failure, or OK.
  const char * message;            ///< Its static description.
  bool finished;                   ///< finish has been called.
  bool sink_failed;                ///< The sink has refused: it is not asked again.
  bool started;                    ///< A token has been given.
  bool inject;                     ///< inject_meta_charset with an encoding.
  char label[32];                  ///< The encoding label, trimmed (the longest UTF-8 label is 17 bytes).

  GTEXT_HTML_Serialize_Result r;   ///< The counts.

  size_t total;                    ///< Bytes produced, against the limit.
  size_t sent;                     ///< Bytes the sink accepted.
  size_t used;                     ///< Bytes in `buf`.
  unsigned char buf[SER_BUFFER];   ///< Output waiting for the sink.

  Frame * stack;                   ///< The open elements.
  size_t nstack;                   ///< How many.
  size_t capstack;                 ///< How many fit.
  bool ctx_set;                    ///< The stream says what element it is the contents of.
  Frame ctx;                       ///< That element.

  uint8_t pend_kind;               ///< PEND_*: the tag being held.
  uint8_t pend_id;                 ///< Its I_*.
  bool pend_parent_nop;            ///< PEND_END: the parent forbids the omission at "no more content" for a p.
  uint8_t last_end_omitted;        ///< The I_* of the end tag omitted just before the token being processed.

  size_t preserve;                 ///< strip_whitespace: depth inside an element whose white space stays.
  bool in_space_run;               ///< strip_whitespace: the last character written was a collapsed space.

  uint8_t ij_state;                ///< inject_meta_charset: IJ_*.
  bool meta_found;                 ///< ... a meta with the encoding has been seen (or added).
  bool replaying;                  ///< ... the queue is being given to the stream.
  bool ij_context_head;            ///< ... the context is a head: there is no head start tag in the queue.
  unsigned char * q;               ///< ... the tokens of the head.
  size_t qlen;                     ///< ... bytes in the queue.
  size_t qcap;                     ///< ... bytes allocated.

  GTEXT_Markup_Attribute * scratch; ///< Scratch for replayed attributes.
  size_t scratch_cap;               ///< Its size.
  void * sortbuf;                   ///< Scratch for sorted attributes.
  size_t sort_cap;                  ///< Its size.
};

/// Where the attributes of a tag are: an array, or an element of a tree.
typedef struct {
  const GTEXT_Markup_Attribute * arr; ///< The array, or NULL.
  const GTEXT_Markup_Node * el;       ///< The element when there is no array.
  size_t n;                           ///< How many.
} AttrSource;

/* ---- failing ------------------------------------------------------------- */

static GTEXT_HTML_Status flush(GTEXT_HTML_Serializer * s);

/* The first failure is the serializer's status for good. Whatever bytes it had produced are given to the sink then and
   there, so that the walk and the token stream hand over the same prefix (everything but a tag the lookahead was still
   holding); a sink that has refused is not asked again. */
static GTEXT_HTML_Status fail(GTEXT_HTML_Serializer * s, GTEXT_HTML_Status status, const char * message) {
  if (s->status == GTEXT_HTML_OK) {
    s->status = status;
    s->message = message;
    (void)flush(s);
  }
  return s->status;
}

static GTEXT_HTML_Status oom(GTEXT_HTML_Serializer * s) {
  return fail(s, GTEXT_HTML_E_OOM, "out of memory");
}

/* ---- output -------------------------------------------------------------- */

static GTEXT_HTML_Status flush(GTEXT_HTML_Serializer * s) {
  if (s->used == 0 || s->sink_failed) {
    return s->status;
  }
  const GTEXT_Markup_Status st = s->sink.write(s->sink.user, (const char *)s->buf, s->used);
  if (st != GTEXT_MARKUP_OK) {
    s->sink_failed = true; /* a sink that has refused is not asked again */
    return st == GTEXT_MARKUP_E_OOM ? oom(s) : fail(s, GTEXT_HTML_E_WRITE, "output refused");
  }
  s->sent += s->used;
  s->used = 0;
  return GTEXT_HTML_OK;
}

static GTEXT_HTML_Status out(GTEXT_HTML_Serializer * s, const void * p, size_t n) {
  if (s->status != GTEXT_HTML_OK) {
    return s->status;
  }
  if (n > s->o.max_output_bytes - s->total) {
    return fail(s, GTEXT_HTML_E_LIMIT_OUTPUT, "output too long");
  }
  s->total += n;
  const unsigned char * b = p;
  while (n > 0) {
    const size_t room = SER_BUFFER - s->used;
    const size_t k = n < room ? n : room;
    memcpy(s->buf + s->used, b, k);
    s->used += k;
    b += k;
    n -= k;
    if (s->used == SER_BUFFER && flush(s) != GTEXT_HTML_OK) {
      return s->status;
    }
  }
  return GTEXT_HTML_OK;
}

static GTEXT_HTML_Status outs(GTEXT_HTML_Serializer * s, const char * text) {
  return out(s, text, strlen(text));
}

static GTEXT_HTML_Status outc(GTEXT_HTML_Serializer * s, char c) {
  return out(s, &c, 1);
}

/* Text or an attribute value, escaped as the standard says: `&`, U+00A0, `<`, `>`, and in an attribute
   value written between quotes the quote itself (`"` as &quot;, `'` as &#39;, which is what html5lib's
   vector for quote_char defines). `quote` is 0 for text and for an unquoted value. */
static GTEXT_HTML_Status put_escaped(GTEXT_HTML_Serializer * s, const char * p, size_t n, char quote) {
  size_t run = 0;
  size_t i = 0;
  while (i < n) {
    const unsigned char c = (unsigned char)p[i];
    const char * rep = NULL;
    size_t skip = 1;
    switch (c) {
      case '&':
        rep = "&amp;";
        break;
      case '<':
        rep = "&lt;";
        break;
      case '>':
        rep = "&gt;";
        break;
      case '"':
        if (quote == '"') {
          rep = "&quot;";
        }
        break;
      case '\'':
        if (quote == '\'') {
          rep = "&#39;";
        }
        break;
      case 0xC2:
        if (i + 1 < n && (unsigned char)p[i + 1] == 0xA0) {
          rep = "&nbsp;";
          skip = 2;
        }
        break;
      default:
        break;
    }
    if (rep) {
      if (i > run && out(s, p + run, i - run) != GTEXT_HTML_OK) {
        return s->status;
      }
      if (outs(s, rep) != GTEXT_HTML_OK) {
        return s->status;
      }
      i += skip;
      run = i;
    }
    else {
      i++;
    }
  }
  return n > run ? out(s, p + run, n - run) : s->status;
}

static bool is_white(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/* ---- growing arrays ------------------------------------------------------ */

static bool grow(GTEXT_HTML_Serializer * s, void ** array, size_t * cap, size_t need, size_t unit) {
  if (need <= *cap) {
    return true;
  }
  size_t c = *cap ? *cap : 16;
  while (c < need) {
    if (c > SIZE_MAX / 2) {
      oom(s);
      return false;
    }
    c *= 2;
  }
  if (c > SIZE_MAX / unit) {
    oom(s);
    return false;
  }
  void * grown = gtext_allocator_realloc(&s->alloc, *array, c * unit);
  if (!grown) {
    oom(s);
    return false;
  }
  *array = grown;
  *cap = c;
  return true;
}

/* ---- attributes ---------------------------------------------------------- */

static bool is_html_ns(const char * ns) {
  return !ns || strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0;
}

static bool equal_nocase(const char * a, size_t an, const char * b, size_t bn) {
  if (an != bn) {
    return false;
  }
  for (size_t i = 0; i < an; i++) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') {
      x = (char)(x + 32);
    }
    if (y >= 'A' && y <= 'Z') {
      y = (char)(y + 32);
    }
    if (x != y) {
      return false;
    }
  }
  return true;
}

static GTEXT_Markup_Attribute attr_get(const AttrSource * as, size_t i) {
  if (as->arr) {
    return as->arr[i];
  }
  GTEXT_Markup_Attribute a;
  memset(&a, 0, sizeof(a));
  (void)gtext_markup_element_attribute_at(as->el, i, &a);
  return a;
}

/* The attribute's serialized name, by the standard's table. */
static GTEXT_HTML_Status put_attr_name(GTEXT_HTML_Serializer * s, const GTEXT_Markup_Attribute * a) {
  const char * lead = NULL;
  if (a->ns) {
    if (strcmp(a->ns, GTEXT_MARKUP_NS_XML) == 0) {
      lead = "xml:";
    }
    else if (strcmp(a->ns, GTEXT_MARKUP_NS_XMLNS) == 0) {
      if (strcmp(a->local, "xmlns") != 0) {
        lead = "xmlns:";
      }
    }
    else if (strcmp(a->ns, GTEXT_MARKUP_NS_XLINK) == 0) {
      lead = "xlink:";
    }
    else if (a->prefix) {
      if (outs(s, a->prefix) != GTEXT_HTML_OK || outc(s, ':') != GTEXT_HTML_OK) {
        return s->status;
      }
    }
  }
  if (lead && outs(s, lead) != GTEXT_HTML_OK) {
    return s->status;
  }
  return outs(s, a->local);
}

static GTEXT_HTML_Status put_attr(GTEXT_HTML_Serializer * s, bool html, const char * element_local,
    const GTEXT_Markup_Attribute * a, bool quote_anyway) {
  if (outc(s, ' ') != GTEXT_HTML_OK || put_attr_name(s, a) != GTEXT_HTML_OK) {
    return s->status;
  }
  const char * v = a->value ? a->value : "";
  const size_t n = a->value_length;
  if (s->o.minimize_boolean_attributes && html && !a->ns && element_local
      && gtext_markup_html_is_boolean_attribute(GTEXT_MARKUP_NS_HTML, element_local, a->local)
      && (n == 0 || equal_nocase(v, n, a->local, strlen(a->local)))) {
    s->r.minimized_attributes++;
    return s->status;
  }
  if (outc(s, '=') != GTEXT_HTML_OK) {
    return s->status;
  }
  bool has_dq = false;
  bool has_sq = false;
  bool trigger = n == 0;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)v[i];
    if (c == '"') {
      has_dq = true;
      trigger = true;
    }
    else if (c == '\'') {
      has_sq = true;
      trigger = true;
    }
    else if (is_white(c) || c == '=' || c == '>' || c == '`') {
      trigger = true;
    }
  }
  if (s->o.quote_attr_values == GTEXT_HTML_QUOTE_ALWAYS || trigger || quote_anyway) {
    char q = s->o.quote_char ? s->o.quote_char : '"';
    if (s->o.use_best_quote_char) {
      if (has_sq && !has_dq) {
        q = '"';
      }
      else if (has_dq && !has_sq) {
        q = '\'';
      }
    }
    if (outc(s, q) != GTEXT_HTML_OK || put_escaped(s, v, n, q) != GTEXT_HTML_OK) {
      return s->status;
    }
    return outc(s, q);
  }
  return put_escaped(s, v, n, 0);
}

/// An attribute being sorted, with its place so that the sort is stable.
typedef struct {
  GTEXT_Markup_Attribute a; ///< The attribute.
  size_t index;             ///< Its place before the sort.
} SortEntry;

static int attr_compare(const void * x, const void * y) {
  const SortEntry * a = x;
  const SortEntry * b = y;
  const char * na = a->a.ns ? a->a.ns : "";
  const char * nb = b->a.ns ? b->a.ns : "";
  int c = strcmp(na, nb);
  if (c == 0) {
    c = strcmp(a->a.local, b->a.local);
  }
  if (c == 0) {
    return a->index < b->index ? -1 : a->index > b->index;
  }
  return c;
}

/* `last_quoted`: the last attribute is quoted whatever the quoting mode says. A solidus right after an unquoted value
   is part of the value, so `<br class=a/>` has the class `a/`; the HTML syntax asks for a space between them, and a
   writer that is not asked for one quotes the value instead. */
static GTEXT_HTML_Status put_attrs(GTEXT_HTML_Serializer * s, bool html, const char * element_local,
    const AttrSource * as, bool last_quoted) {
  if (as->n > 1 && s->o.alphabetical_attributes) {
    if (!grow(s, &s->sortbuf, &s->sort_cap, as->n, sizeof(SortEntry))) {
      return s->status;
    }
    SortEntry * e = s->sortbuf;
    for (size_t i = 0; i < as->n; i++) {
      e[i].a = attr_get(as, i);
      e[i].index = i;
    }
    qsort(e, as->n, sizeof(SortEntry), attr_compare);
    for (size_t i = 0; i < as->n; i++) {
      if (put_attr(s, html, element_local, &e[i].a, last_quoted && i + 1 == as->n) != GTEXT_HTML_OK) {
        return s->status;
      }
    }
    return s->status;
  }
  for (size_t i = 0; i < as->n; i++) {
    const GTEXT_Markup_Attribute a = attr_get(as, i);
    if (put_attr(s, html, element_local, &a, last_quoted && i + 1 == as->n) != GTEXT_HTML_OK) {
      return s->status;
    }
  }
  return s->status;
}

/* ---- the stack of open elements ------------------------------------------ */

static Frame frame_of(const char * ns, const char * prefix, const char * local) {
  Frame f = {I_NONE, 0};
  if (is_html_ns(ns)) {
    f.flags = FR_HTML;
    if (!prefix) {
      f.id = (uint8_t)element_id(local);
      if (f.id == I_NONE && is_custom_name(local)) {
        f.flags |= FR_CUSTOM;
      }
    }
  }
  return f;
}

static GTEXT_HTML_Status push_frame(GTEXT_HTML_Serializer * s, Frame f) {
  if (s->nstack >= s->o.max_depth) {
    return fail(s, GTEXT_HTML_E_LIMIT_DEPTH, "elements nested too deeply");
  }
  if (!grow(s, (void **)&s->stack, &s->capstack, s->nstack + 1, sizeof(Frame))) {
    return s->status;
  }
  s->stack[s->nstack++] = f;
  return GTEXT_HTML_OK;
}

/* The parent of what is being written: the innermost open element, or the context of the stream. */
static bool parent_frame(const GTEXT_HTML_Serializer * s, Frame * f) {
  if (s->nstack > 0) {
    *f = s->stack[s->nstack - 1];
    return true;
  }
  if (s->ctx_set) {
    *f = s->ctx;
    return true;
  }
  return false;
}

static bool parent_is_raw(const GTEXT_HTML_Serializer * s) {
  if (s->o.escape_rcdata) {
    return false;
  }
  Frame f;
  if (!parent_frame(s, &f) || !(f.flags & FR_HTML) || f.id == I_NONE) {
    return false;
  }
  const unsigned flags = id_flags(f.id);
  return (flags & EF_RAW) || ((flags & EF_RAW_SCRIPTING) && s->o.scripting);
}

/* ---- the lookahead: should a pending tag be written? ---------------------- */

static bool no_more(const Next * n) {
  return n->kind == N_NONE || n->kind == N_END;
}

static bool start_is(const Next * n, unsigned a) {
  return n->kind == N_START && n->id == a;
}

static bool start_is2(const Next * n, unsigned a, unsigned b) {
  return n->kind == N_START && (n->id == a || n->id == b);
}

static bool white_or_comment(const Next * n) {
  return n->kind == N_COMMENT || (n->kind == N_TEXT && n->white);
}

/* The standard's "optional tags" (13.1.2.4), a start tag with no attributes. */
static bool can_omit_start(const GTEXT_HTML_Serializer * s, unsigned id, const Next * n) {
  switch (id) {
    case I_HTML:
      return n->kind != N_COMMENT;
    case I_HEAD:
      return no_more(n) || n->kind == N_START;
    case I_BODY:
      if (no_more(n)) {
        return true;
      }
      if (white_or_comment(n)) {
        return false;
      }
      return !(n->kind == N_START
          && (n->id == I_META || n->id == I_NOSCRIPT || n->id == I_LINK || n->id == I_SCRIPT || n->id == I_STYLE
              || n->id == I_TEMPLATE));
    case I_COLGROUP:
      return start_is(n, I_COL) && s->last_end_omitted != I_COLGROUP;
    case I_TBODY:
      return start_is(n, I_TR) && s->last_end_omitted != I_TBODY && s->last_end_omitted != I_THEAD
          && s->last_end_omitted != I_TFOOT;
    default:
      return false;
  }
}

static bool can_omit_end(unsigned id, const Next * n, bool parent_nop) {
  switch (id) {
    case I_HTML:
    case I_BODY:
      return n->kind != N_COMMENT;
    case I_HEAD:
    case I_COLGROUP:
    case I_CAPTION:
      return !white_or_comment(n);
    case I_LI:
      return start_is(n, I_LI) || no_more(n);
    case I_DT:
      return start_is2(n, I_DT, I_DD);
    case I_DD:
      return start_is2(n, I_DD, I_DT) || no_more(n);
    case I_P:
      if (n->kind == N_START && (id_flags(n->id) & EF_PNEXT)) {
        return true;
      }
      return no_more(n) && !parent_nop;
    case I_RT:
    case I_RP:
      return start_is2(n, I_RT, I_RP) || no_more(n);
    case I_OPTGROUP:
      return start_is2(n, I_OPTGROUP, I_HR) || no_more(n);
    case I_OPTION:
      return start_is2(n, I_OPTION, I_OPTGROUP) || start_is(n, I_HR) || no_more(n);
    case I_THEAD:
      return start_is2(n, I_TBODY, I_TFOOT);
    case I_TBODY:
      return start_is2(n, I_TBODY, I_TFOOT) || no_more(n);
    case I_TFOOT:
      return no_more(n);
    case I_TR:
      return start_is(n, I_TR) || no_more(n);
    case I_TD:
    case I_TH:
      return start_is2(n, I_TD, I_TH) || no_more(n);
    default:
      return false;
  }
}

/* Decide the pending tag, now that the token after it is known, and write it if it stays. */
static GTEXT_HTML_Status resolve_pending(GTEXT_HTML_Serializer * s, const Next * n) {
  if (s->pend_kind == PEND_NONE) {
    return s->status;
  }
  const unsigned kind = s->pend_kind;
  const unsigned id = s->pend_id;
  const bool parent_nop = s->pend_parent_nop;
  s->pend_kind = PEND_NONE;
  if (s->status != GTEXT_HTML_OK) {
    return s->status;
  }
  if (kind == PEND_START) {
    const bool omit = can_omit_start(s, id, n);
    s->last_end_omitted = I_NONE;
    if (omit) {
      s->r.omitted_start_tags++;
      return s->status;
    }
    if (outc(s, '<') != GTEXT_HTML_OK || outs(s, id_name(id)) != GTEXT_HTML_OK) {
      return s->status;
    }
    return outc(s, '>');
  }
  const bool omit = can_omit_end(id, n, parent_nop);
  if (omit) {
    s->r.omitted_end_tags++;
    s->last_end_omitted = (uint8_t)id;
    return s->status;
  }
  s->last_end_omitted = I_NONE;
  if (outs(s, "</") != GTEXT_HTML_OK || outs(s, id_name(id)) != GTEXT_HTML_OK) {
    return s->status;
  }
  return outc(s, '>');
}

static Next next_of_start(unsigned id) {
  Next n = {N_START, (uint8_t)id, false};
  return n;
}

/* ---- the stream: tokens after the front ----------------------------------- */

static GTEXT_HTML_Status put_tag_name(GTEXT_HTML_Serializer * s, const char * prefix, const char * local) {
  if (prefix && (outs(s, prefix) != GTEXT_HTML_OK || outc(s, ':') != GTEXT_HTML_OK)) {
    return s->status;
  }
  return outs(s, local);
}

static bool is_void_name(bool html, const char * prefix, const char * local) {
  return html && !prefix && gtext_markup_html_is_void(GTEXT_MARKUP_NS_HTML, local);
}

/* A start tag, or an empty tag (`empty`: no end tag follows and no content). */
static GTEXT_HTML_Status s2_start(GTEXT_HTML_Serializer * s, const char * ns, const char * prefix,
    const char * local, const AttrSource * as, bool empty) {
  Frame f = frame_of(ns, prefix, local);
  const bool html = (f.flags & FR_HTML) != 0;
  const bool is_void = is_void_name(html, prefix, local);
  const bool no_end = empty || is_void;
  Next n = next_of_start(f.id);
  if (resolve_pending(s, &n) != GTEXT_HTML_OK) {
    return s->status;
  }
  if (!no_end && s->nstack >= s->o.max_depth) {
    /* Before the tag is written: the sink has the start tags down to the limit and not one more. */
    return fail(s, GTEXT_HTML_E_LIMIT_DEPTH, "elements nested too deeply");
  }
  if (!no_end && (s->preserve > 0 || (f.id != I_NONE && (id_flags(f.id) & EF_PRESERVE)))) {
    s->preserve++;
    f.flags |= FR_PRESERVE;
  }
  s->in_space_run = false;
  if (!no_end && s->o.omit_optional_tags && html && as->n == 0
      && (f.id == I_HTML || f.id == I_HEAD || f.id == I_BODY || f.id == I_COLGROUP || f.id == I_TBODY)) {
    if (push_frame(s, f) != GTEXT_HTML_OK) {
      return s->status;
    }
    s->pend_kind = PEND_START;
    s->pend_id = f.id;
    return s->status; /* last_end_omitted stays: the held tag asks about it */
  }
  s->last_end_omitted = I_NONE;
  if (outc(s, '<') != GTEXT_HTML_OK || put_tag_name(s, prefix, local) != GTEXT_HTML_OK
      || put_attrs(s, html, local, as, is_void && s->o.use_trailing_solidus && !s->o.space_before_trailing_solidus)
          != GTEXT_HTML_OK) {
    return s->status;
  }
  if (is_void && s->o.use_trailing_solidus) {
    if (s->o.space_before_trailing_solidus && outc(s, ' ') != GTEXT_HTML_OK) {
      return s->status;
    }
    if (outc(s, '/') != GTEXT_HTML_OK) {
      return s->status;
    }
  }
  if (outc(s, '>') != GTEXT_HTML_OK) {
    return s->status;
  }
  return no_end ? s->status : push_frame(s, f);
}

static GTEXT_HTML_Status s2_end(
    GTEXT_HTML_Serializer * s, const char * ns, const char * prefix, const char * local) {
  const Frame f = frame_of(ns, prefix, local);
  const bool html = (f.flags & FR_HTML) != 0;
  Next n = {N_END, I_NONE, false};
  if (resolve_pending(s, &n) != GTEXT_HTML_OK) {
    return s->status;
  }
  s->in_space_run = false;
  if (is_void_name(html, prefix, local)) {
    s->last_end_omitted = I_NONE;
    return s->status; /* a void element has no end tag */
  }
  /* An end tag closes the innermost open element if it is that element: an element of the same kind of namespace
     (HTML or not) and, among those the rules name, of the same name. An end tag with nothing open, or for another
     element, closes nothing: it is written (or left out by the rules, judged by the element it names) in the place
     it is given and the stack, and the white space count, are as they were. */
  if (s->nstack > 0) {
    const Frame top = s->stack[s->nstack - 1];
    if (((top.flags ^ f.flags) & FR_HTML) == 0 && top.id == f.id) {
      s->nstack--;
      if ((top.flags & FR_PRESERVE) && s->preserve > 0) {
        s->preserve--;
      }
    }
  }
  if (s->o.omit_optional_tags && html && f.id != I_NONE) {
    switch (f.id) {
      case I_HTML: case I_HEAD: case I_BODY: case I_LI: case I_DT: case I_DD: case I_P: case I_RT: case I_RP:
      case I_OPTGROUP: case I_OPTION: case I_COLGROUP: case I_CAPTION: case I_THEAD: case I_TBODY:
      case I_TFOOT: case I_TR: case I_TD: case I_TH: {
        Frame parent;
        const bool known = parent_frame(s, &parent);
        s->pend_kind = PEND_END;
        s->pend_id = f.id;
        s->pend_parent_nop = known && (parent.flags & FR_CUSTOM ? true
            : ((parent.flags & FR_HTML) && parent.id != I_NONE && (id_flags(parent.id) & EF_NOP)));
        return s->status;
      }
      default:
        break;
    }
  }
  s->last_end_omitted = I_NONE;
  if (outs(s, "</") != GTEXT_HTML_OK || put_tag_name(s, prefix, local) != GTEXT_HTML_OK) {
    return s->status;
  }
  return outc(s, '>');
}

static GTEXT_HTML_Status s2_characters(GTEXT_HTML_Serializer * s, const char * data, size_t length) {
  if (length == 0) {
    return s->status;
  }
  const bool collapse = s->o.strip_whitespace && s->preserve == 0 && !parent_is_raw(s);
  Next n = {N_TEXT, I_NONE, false};
  n.white = is_white((unsigned char)data[0]);
  if (resolve_pending(s, &n) != GTEXT_HTML_OK) {
    return s->status;
  }
  s->last_end_omitted = I_NONE;
  if (parent_is_raw(s)) {
    s->in_space_run = false;
    return out(s, data, length);
  }
  if (!collapse) {
    s->in_space_run = false;
    return put_escaped(s, data, length, 0);
  }
  size_t i = 0;
  while (i < length) {
    if (is_white((unsigned char)data[i])) {
      if (!s->in_space_run && outc(s, ' ') != GTEXT_HTML_OK) {
        return s->status;
      }
      s->in_space_run = true;
      i++;
      continue;
    }
    size_t j = i;
    while (j < length && !is_white((unsigned char)data[j])) {
      j++;
    }
    s->in_space_run = false;
    if (put_escaped(s, data + i, j - i, 0) != GTEXT_HTML_OK) {
      return s->status;
    }
    i = j;
  }
  return s->status;
}

static GTEXT_HTML_Status s2_comment(GTEXT_HTML_Serializer * s, const char * data, size_t length) {
  Next n = {N_COMMENT, I_NONE, false};
  if (resolve_pending(s, &n) != GTEXT_HTML_OK) {
    return s->status;
  }
  s->last_end_omitted = I_NONE;
  s->in_space_run = false;
  if (outs(s, "<!--") != GTEXT_HTML_OK || out(s, data, length) != GTEXT_HTML_OK) {
    return s->status;
  }
  return outs(s, "-->");
}

static GTEXT_HTML_Status s2_pi(
    GTEXT_HTML_Serializer * s, const char * target, const char * data, size_t length) {
  Next n = {N_OTHER, I_NONE, false};
  if (resolve_pending(s, &n) != GTEXT_HTML_OK) {
    return s->status;
  }
  s->last_end_omitted = I_NONE;
  s->in_space_run = false;
  if (outs(s, "<?") != GTEXT_HTML_OK || outs(s, target) != GTEXT_HTML_OK || outc(s, ' ') != GTEXT_HTML_OK
      || out(s, data, length) != GTEXT_HTML_OK) {
    return s->status;
  }
  return outs(s, "?>");
}

static GTEXT_HTML_Status put_quoted_id(GTEXT_HTML_Serializer * s, const char * id) {
  const char q = strchr(id, '"') ? '\'' : '"';
  if (outc(s, q) != GTEXT_HTML_OK || outs(s, id) != GTEXT_HTML_OK) {
    return s->status;
  }
  return outc(s, q);
}

static GTEXT_HTML_Status s2_doctype(
    GTEXT_HTML_Serializer * s, const char * name, const char * pub, const char * sys) {
  Next n = {N_OTHER, I_NONE, false};
  if (resolve_pending(s, &n) != GTEXT_HTML_OK) {
    return s->status;
  }
  s->last_end_omitted = I_NONE;
  s->in_space_run = false;
  /* An identifier is written between quotes and ends at the first one like them, so one that holds both kinds has no
     spelling. It is the one tree this serializer refuses, and only when it is asked to write identifiers (the standard
     writes the name alone, which every tree has). */
  if (s->o.doctype_identifiers && ((pub && strchr(pub, '"') && strchr(pub, '\''))
          || (sys && strchr(sys, '"') && strchr(sys, '\'')))) {
    return fail(s, GTEXT_HTML_E_INVALID, "a document type identifier with both kinds of quote cannot be written");
  }
  if (outs(s, "<!DOCTYPE ") != GTEXT_HTML_OK || outs(s, name ? name : "") != GTEXT_HTML_OK) {
    return s->status;
  }
  if (s->o.doctype_identifiers) {
    const bool has_pub = pub && *pub;
    const bool has_sys = sys && *sys;
    if (has_pub) {
      if (outs(s, " PUBLIC ") != GTEXT_HTML_OK || put_quoted_id(s, pub) != GTEXT_HTML_OK) {
        return s->status;
      }
    }
    else if (has_sys && outs(s, " SYSTEM") != GTEXT_HTML_OK) {
      return s->status;
    }
    if (has_sys && (outc(s, ' ') != GTEXT_HTML_OK || put_quoted_id(s, sys) != GTEXT_HTML_OK)) {
      return s->status;
    }
  }
  return outc(s, '>');
}

/* ---- the front: inject_meta_charset ------------------------------------- */

/* The queue is a sequence of records. A string is a length (a varint: 0 for NULL, else the length and one), the bytes and
   a NUL after them, so that what is replayed is a C string again. The varints keep the queue to a small multiple of the
   output the tokens make (a one-character text is four bytes), which is what the bound below rests on. */
enum { Q_START = 1, Q_EMPTY, Q_END, Q_CHARS, Q_COMMENT, Q_DOCTYPE, Q_PI };

/* The most the queue may hold: eight times the output limit and a little more. The queue is not output, and a head of
   an output that fits the limit must not be refused because its tokens are bigger than its text. */
static size_t queue_limit(const GTEXT_HTML_Serializer * s) {
  const size_t m = s->o.max_output_bytes;
  return m > (SIZE_MAX - 4096) / 8 ? SIZE_MAX : m * 8 + 4096;
}

static bool q_put(GTEXT_HTML_Serializer * s, const void * p, size_t n) {
  if (n > queue_limit(s) - s->qlen) {
    fail(s, GTEXT_HTML_E_LIMIT_OUTPUT, "output too long");
    return false;
  }
  if (!grow(s, (void **)&s->q, &s->qcap, s->qlen + n, 1)) {
    return false;
  }
  memcpy(s->q + s->qlen, p, n);
  s->qlen += n;
  return true;
}

static bool q_byte(GTEXT_HTML_Serializer * s, unsigned char b) {
  return q_put(s, &b, 1);
}

static bool q_varint(GTEXT_HTML_Serializer * s, size_t v) {
  unsigned char b[10];
  size_t k = 0;
  do {
    b[k] = (unsigned char)(v & 0x7F);
    v >>= 7;
    if (v) {
      b[k] |= 0x80;
    }
    k++;
  } while (v);
  return q_put(s, b, k);
}

static bool q_str(GTEXT_HTML_Serializer * s, const char * p, size_t n) {
  if (!q_varint(s, p ? n + 1 : 0)) {
    return false;
  }
  if (!p) {
    return true;
  }
  return q_put(s, p, n) && q_byte(s, 0);
}

/* A namespace is one byte for the ones this library knows (an element in the HTML namespace is the common case, and its URI
   would otherwise be most of the record), else 255 and the string. */
static const char * const k_ns_known[] = {NULL, GTEXT_MARKUP_NS_HTML, GTEXT_MARKUP_NS_SVG, GTEXT_MARKUP_NS_MATHML,
    GTEXT_MARKUP_NS_XML, GTEXT_MARKUP_NS_XMLNS, GTEXT_MARKUP_NS_XLINK, ""};

static bool q_cstr(GTEXT_HTML_Serializer * s, const char * p);

static bool q_ns(GTEXT_HTML_Serializer * s, const char * ns) {
  for (unsigned char i = 0; i < sizeof(k_ns_known) / sizeof(k_ns_known[0]); i++) {
    if ((ns == NULL && k_ns_known[i] == NULL) || (ns && k_ns_known[i] && strcmp(ns, k_ns_known[i]) == 0)) {
      return q_byte(s, i);
    }
  }
  return q_byte(s, 255) && q_cstr(s, ns);
}

static bool q_cstr(GTEXT_HTML_Serializer * s, const char * p) {
  return q_str(s, p, p ? strlen(p) : 0);
}

static bool q_attrs(GTEXT_HTML_Serializer * s, const AttrSource * as, const char * replace_name,
    const char * replace_value) {
  const size_t n = as->n;
  if (!q_varint(s, n)) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    const GTEXT_Markup_Attribute a = attr_get(as, i);
    const bool replaced = replace_name && !a.ns && strcmp(a.local, replace_name) == 0;
    if (!q_ns(s, a.ns) || !q_cstr(s, a.prefix) || !q_cstr(s, a.local)) {
      return false;
    }
    if (replaced) {
      if (!q_cstr(s, replace_value)) {
        return false;
      }
      replace_name = NULL; /* the first one only */
    }
    else if (!q_str(s, a.value ? a.value : "", a.value_length)) {
      return false;
    }
  }
  return true;
}

/// A read position in the queue of held tokens.
typedef struct {
  const unsigned char * p;   ///< The next byte.
  const unsigned char * end; ///< The end of the queue.
} QCursor;

static size_t qc_varint(QCursor * c) {
  size_t v = 0;
  unsigned shift = 0;
  for (;;) {
    const unsigned char b = *c->p++;
    v |= (size_t)(b & 0x7F) << shift;
    if (!(b & 0x80)) {
      return v;
    }
    shift += 7;
  }
}

static const char * qc_str(QCursor * c, size_t * len);

static const char * qc_ns(QCursor * c) {
  const unsigned char code = *c->p++;
  if (code == 255) {
    return qc_str(c, NULL);
  }
  return k_ns_known[code];
}

static const char * qc_str(QCursor * c, size_t * len) {
  const size_t coded = qc_varint(c);
  if (coded == 0) {
    if (len) {
      *len = 0;
    }
    return NULL;
  }
  const size_t n = coded - 1;
  const char * str = (const char *)c->p;
  c->p += n + 1;
  if (len) {
    *len = n;
  }
  return str;
}

/* Replay the queued tokens from `c` to the end (or just the first, with `one`). */
static GTEXT_HTML_Status q_replay(GTEXT_HTML_Serializer * s, QCursor * c, bool one) {
  while (c->p < c->end && s->status == GTEXT_HTML_OK) {
    const unsigned char type = *c->p++;
    switch (type) {
      case Q_START:
      case Q_EMPTY:
      case Q_END: {
        const char * ns = qc_ns(c);
        const char * prefix = qc_str(c, NULL);
        const char * local = qc_str(c, NULL);
        if (type == Q_END) {
          s2_end(s, ns, prefix, local);
          break;
        }
        const size_t n = qc_varint(c);
        if (!grow(s, (void **)&s->scratch, &s->scratch_cap, n ? n : 1, sizeof(GTEXT_Markup_Attribute))) {
          return s->status;
        }
        for (size_t i = 0; i < n; i++) {
          GTEXT_Markup_Attribute * a = &s->scratch[i];
          a->ns = qc_ns(c);
          a->prefix = qc_str(c, NULL);
          a->local = qc_str(c, NULL);
          a->value = qc_str(c, &a->value_length);
        }
        const AttrSource as = {s->scratch, NULL, n};
        s2_start(s, ns, prefix, local, &as, type == Q_EMPTY);
        break;
      }
      case Q_CHARS:
      case Q_COMMENT: {
        size_t n;
        const char * data = qc_str(c, &n);
        if (type == Q_CHARS) {
          s2_characters(s, data, n);
        }
        else {
          s2_comment(s, data, n);
        }
        break;
      }
      case Q_DOCTYPE: {
        const char * name = qc_str(c, NULL);
        const char * pub = qc_str(c, NULL);
        const char * sys = qc_str(c, NULL);
        s2_doctype(s, name, pub, sys);
        break;
      }
      default: { /* Q_PI */
        const char * target = qc_str(c, NULL);
        size_t n;
        const char * data = qc_str(c, &n);
        s2_pi(s, target, data, n);
        break;
      }
    }
    if (one) {
      break;
    }
  }
  return s->status;
}

/* The end of the head (or of the stream inside one): write the head's tokens, with the meta first among them
   if none was found. */
static GTEXT_HTML_Status ij_flush(GTEXT_HTML_Serializer * s) {
  s->replaying = true;
  QCursor c = {s->q, s->q + s->qlen};
  if (s->qlen > 0 && !s->ij_context_head) {
    q_replay(s, &c, true); /* the head's start tag */
  }
  if (!s->meta_found && s->status == GTEXT_HTML_OK && (s->qlen > 0 || s->ij_context_head)) {
    GTEXT_Markup_Attribute a;
    memset(&a, 0, sizeof(a));
    a.local = "charset";
    a.value = s->label;
    a.value_length = strlen(s->label);
    const AttrSource as = {&a, NULL, 1};
    s2_start(s, NULL, NULL, "meta", &as, true);
    s->r.meta_charset_injected = 1;
  }
  q_replay(s, &c, false);
  s->replaying = false;
  s->qlen = 0;
  s->ij_state = IJ_POST;
  s->meta_found = true;
  return s->status;
}

/* A `meta` in the head: which attribute, if any, holds the encoding, and what it is to be replaced with
   (`content` is the scratch for the second form). */
static bool meta_plan(const GTEXT_HTML_Serializer * s, const AttrSource * as, const char ** name,
    const char ** value, char * content, size_t content_cap) {
  bool http_equiv_ct = false;
  const char * content_name = NULL;
  for (size_t i = 0; i < as->n; i++) {
    const GTEXT_Markup_Attribute a = attr_get(as, i);
    if (a.ns) {
      continue;
    }
    /* Names are matched without regard to case (a parser lower-cases them; a tree made by hand need not). */
    if (equal_nocase(a.local, strlen(a.local), "charset", 7)) {
      *name = a.local;
      *value = s->label;
      return true;
    }
    if (equal_nocase(a.local, strlen(a.local), "http-equiv", 10) && a.value
        && equal_nocase(a.value, a.value_length, "content-type", 12)) {
      http_equiv_ct = true;
    }
    if (!content_name && equal_nocase(a.local, strlen(a.local), "content", 7)) {
      content_name = a.local;
    }
  }
  if (http_equiv_ct && content_name) {
    snprintf(content, content_cap, "text/html; charset=%s", s->label);
    *name = content_name;
    *value = content;
    return true;
  }
  return false;
}

static GTEXT_HTML_Status fe_start(GTEXT_HTML_Serializer * s, const char * ns, const char * prefix,
    const char * local, const AttrSource * as, bool empty) {
  if (!s->inject || s->replaying) {
    return s2_start(s, ns, prefix, local, as, empty);
  }
  const Frame f = frame_of(ns, prefix, local);
  if (!empty && f.id == I_HEAD && s->ij_state == IJ_PRE) {
    s->ij_state = IJ_IN_HEAD; /* a second head, after the first has ended, is not looked into again */
  }
  if (s->ij_state != IJ_IN_HEAD) {
    return s2_start(s, ns, prefix, local, as, empty);
  }
  const char * replace_name = NULL;
  const char * replace_value = NULL;
  char content[96];
  const bool replacing = f.id == I_META && meta_plan(s, as, &replace_name, &replace_value, content, sizeof(content));
  if (q_byte(s, empty ? Q_EMPTY : Q_START) && q_ns(s, ns) && q_cstr(s, prefix) && q_cstr(s, local)
      && q_attrs(s, as, replace_name, replace_value) && replacing) {
    s->meta_found = true; /* counted when the record is in the queue, not before */
    s->r.meta_charset_replaced++;
  }
  return s->status;
}

static GTEXT_HTML_Status fe_end(
    GTEXT_HTML_Serializer * s, const char * ns, const char * prefix, const char * local) {
  if (!s->inject || s->replaying || s->ij_state != IJ_IN_HEAD) {
    return s2_end(s, ns, prefix, local);
  }
  const Frame f = frame_of(ns, prefix, local);
  if (f.id == I_HEAD) {
    if (ij_flush(s) != GTEXT_HTML_OK) {
      return s->status;
    }
    return s2_end(s, ns, prefix, local);
  }
  if (!q_byte(s, Q_END) || !q_ns(s, ns) || !q_cstr(s, prefix) || !q_cstr(s, local)) {
    return s->status;
  }
  return s->status;
}

static bool fe_queueing(const GTEXT_HTML_Serializer * s) {
  return s->inject && !s->replaying && s->ij_state == IJ_IN_HEAD;
}

/* ---- creating -------------------------------------------------------------- */

static GTEXT_HTML_Status check_options_of(const GTEXT_HTML_Serialize_Options * o) {
  if (o->encoding && !html_label_is_utf8(o->encoding)) {
    return GTEXT_HTML_E_UNSUPPORTED_ENCODING;
  }
  if ((int)o->quote_attr_values < GTEXT_HTML_QUOTE_ALWAYS || o->quote_attr_values > GTEXT_HTML_QUOTE_WHEN_NEEDED) {
    return GTEXT_HTML_E_OPTIONS;
  }
  if (o->quote_char != 0 && o->quote_char != '"' && o->quote_char != '\'') {
    return GTEXT_HTML_E_OPTIONS;
  }
  if (o->use_best_quote_char && o->quote_char != 0) {
    return GTEXT_HTML_E_OPTIONS;
  }
  if (o->space_before_trailing_solidus && !o->use_trailing_solidus) {
    return GTEXT_HTML_E_OPTIONS;
  }
  if ((o->context_name && !*o->context_name) || (o->context_namespace && !o->context_name)) {
    return GTEXT_HTML_E_OPTIONS;
  }
  return GTEXT_HTML_OK;
}

/// See html_api_internal.h.
GTEXT_HTML_Status html_serialize_check_options(const GTEXT_HTML_Serialize_Options * opts) {
  return opts ? check_options_of(opts) : GTEXT_HTML_OK;
}

GTEXT_HTML_Serialize_Options gtext_html_serialize_options_default(void) {
  GTEXT_HTML_Serialize_Options o;
  memset(&o, 0, sizeof(o));
  o.max_depth = SER_DEFAULT_MAX_DEPTH;
  o.max_output_bytes = SER_DEFAULT_MAX_OUTPUT;
  return o;
}

static void set_context(GTEXT_HTML_Serializer * s, Frame f);

static GTEXT_HTML_Status create(const GTEXT_Allocator * alloc, const GTEXT_HTML_Serialize_Options * opts,
    const GTEXT_Markup_Sink * sink, GTEXT_HTML_Serializer ** out_ser, const char ** message) {
  *out_ser = NULL;
  *message = NULL;
  if (!sink || !sink->write) {
    *message = "sink must not be NULL";
    return GTEXT_HTML_E_INVALID;
  }
  GTEXT_HTML_Serialize_Options o = opts ? *opts : gtext_html_serialize_options_default();
  const GTEXT_HTML_Status st = check_options_of(&o);
  if (st != GTEXT_HTML_OK) {
    *message = st == GTEXT_HTML_E_OPTIONS ? "unusable combination of options"
                                          : "the encoding is not one this release writes";
    return st;
  }
  if (o.max_depth == 0) {
    o.max_depth = SER_DEFAULT_MAX_DEPTH;
  }
  if (o.max_output_bytes == 0) {
    o.max_output_bytes = SER_DEFAULT_MAX_OUTPUT;
  }
  GTEXT_HTML_Serializer * s = gtext_allocator_calloc(alloc, 1, sizeof(*s));
  if (!s) {
    *message = "out of memory";
    return GTEXT_HTML_E_OOM;
  }
  s->alloc = *alloc;
  s->o = o;
  s->o.encoding = NULL; /* the label is kept in s->label: the caller's string need not outlive the call */
  s->o.context_name = NULL; /* ... and the context is a frame, set below */
  s->o.context_namespace = NULL;
  s->sink = *sink;
  s->inject = o.inject_meta_charset && o.encoding != NULL;
  if (o.encoding) {
    const char * b = o.encoding;
    while (is_white((unsigned char)*b)) {
      b++;
    }
    size_t n = strlen(b);
    while (n > 0 && is_white((unsigned char)b[n - 1])) {
      n--;
    }
    memcpy(s->label, b, n < sizeof(s->label) - 1 ? n : sizeof(s->label) - 1);
  }
  s->meta_found = !s->inject;
  if (o.context_name) {
    set_context(s, frame_of(o.context_namespace, NULL, o.context_name));
  }
  *out_ser = s;
  return GTEXT_HTML_OK;
}

void gtext_html_serializer_free(GTEXT_HTML_Serializer * s) {
  if (!s) {
    return;
  }
  const GTEXT_Allocator alloc = s->alloc;
  gtext_allocator_free(&alloc, s->stack);
  gtext_allocator_free(&alloc, s->q);
  gtext_allocator_free(&alloc, s->scratch);
  gtext_allocator_free(&alloc, s->sortbuf);
  gtext_allocator_free(&alloc, s);
}

GTEXT_HTML_Status gtext_html_serializer_new(const GTEXT_HTML_Serialize_Options * opts,
    const GTEXT_Markup_Sink * sink, GTEXT_HTML_Serializer ** out) {
  if (!out) {
    return GTEXT_HTML_E_INVALID;
  }
  const GTEXT_Allocator * alloc = opts && opts->allocator ? opts->allocator : gtext_allocator_default();
  const char * message;
  return create(alloc, opts, sink, out, &message);
}

/* ---- the token API -------------------------------------------------------- */

/* The contents of this element are what is being written: its text is raw if it is, white space stays if
   it is `pre`, and inside a `head` the encoding is declared as if its start tag had been seen. */
static void set_context(GTEXT_HTML_Serializer * s, Frame f) {
  s->ctx = f;
  s->ctx_set = true;
  s->preserve = (f.id != I_NONE && (f.flags & FR_HTML) && (id_flags(f.id) & EF_PRESERVE)) ? 1 : 0;
  if (s->inject && f.id == I_HEAD && (f.flags & FR_HTML)) {
    s->ij_state = IJ_IN_HEAD;
    s->ij_context_head = true;
  }
}

static GTEXT_HTML_Status token_enter(GTEXT_HTML_Serializer * s) {
  if (!s) {
    return GTEXT_HTML_E_INVALID;
  }
  if (s->status != GTEXT_HTML_OK) {
    return s->status;
  }
  if (s->finished) {
    return GTEXT_HTML_E_STATE;
  }
  return GTEXT_HTML_OK;
}

static bool attrs_usable(const GTEXT_Markup_Attribute * attrs, size_t count) {
  if (count > 0 && !attrs) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    if (!attrs[i].local || !*attrs[i].local || (!attrs[i].value && attrs[i].value_length > 0)) {
      return false;
    }
  }
  return true;
}

static GTEXT_HTML_Status tag_call(GTEXT_HTML_Serializer * s, const char * ns, const char * name,
    const GTEXT_Markup_Attribute * attrs, size_t count, bool empty) {
  const GTEXT_HTML_Status st = token_enter(s);
  if (st != GTEXT_HTML_OK) {
    return st;
  }
  if (!name || !*name || !attrs_usable(attrs, count)) {
    return GTEXT_HTML_E_INVALID;
  }
  s->started = true; /* the arguments were usable: this is a token */
  const AttrSource as = {attrs, NULL, count};
  return fe_start(s, ns, NULL, name, &as, empty);
}

GTEXT_HTML_Status gtext_html_serializer_start_tag(GTEXT_HTML_Serializer * s, const char * ns,
    const char * name, const GTEXT_Markup_Attribute * attrs, size_t count) {
  return tag_call(s, ns, name, attrs, count, false);
}

GTEXT_HTML_Status gtext_html_serializer_empty_tag(GTEXT_HTML_Serializer * s, const char * ns,
    const char * name, const GTEXT_Markup_Attribute * attrs, size_t count) {
  return tag_call(s, ns, name, attrs, count, true);
}

GTEXT_HTML_Status gtext_html_serializer_end_tag(
    GTEXT_HTML_Serializer * s, const char * ns, const char * name) {
  const GTEXT_HTML_Status st = token_enter(s);
  if (st != GTEXT_HTML_OK) {
    return st;
  }
  if (!name || !*name) {
    return GTEXT_HTML_E_INVALID;
  }
  s->started = true;
  return fe_end(s, ns, NULL, name);
}

GTEXT_HTML_Status gtext_html_serializer_characters(
    GTEXT_HTML_Serializer * s, const char * data, size_t length) {
  const GTEXT_HTML_Status st = token_enter(s);
  if (st != GTEXT_HTML_OK) {
    return st;
  }
  if (!data && length > 0) {
    return GTEXT_HTML_E_INVALID;
  }
  s->started = true;
  if (length == 0) {
    return s->status;
  }
  if (fe_queueing(s)) {
    if (q_byte(s, Q_CHARS)) {
      q_str(s, data, length);
    }
    return s->status;
  }
  return s2_characters(s, data, length);
}

GTEXT_HTML_Status gtext_html_serializer_comment(
    GTEXT_HTML_Serializer * s, const char * data, size_t length) {
  const GTEXT_HTML_Status st = token_enter(s);
  if (st != GTEXT_HTML_OK) {
    return st;
  }
  if (!data && length > 0) {
    return GTEXT_HTML_E_INVALID;
  }
  s->started = true;
  if (fe_queueing(s)) {
    if (q_byte(s, Q_COMMENT)) {
      q_str(s, data ? data : "", length);
    }
    return s->status;
  }
  return s2_comment(s, data ? data : "", length);
}

GTEXT_HTML_Status gtext_html_serializer_processing_instruction(
    GTEXT_HTML_Serializer * s, const char * target, const char * data, size_t length) {
  const GTEXT_HTML_Status st = token_enter(s);
  if (st != GTEXT_HTML_OK) {
    return st;
  }
  if (!target || (!data && length > 0)) {
    return GTEXT_HTML_E_INVALID;
  }
  s->started = true;
  if (fe_queueing(s)) {
    if (q_byte(s, Q_PI) && q_cstr(s, target)) {
      q_str(s, data ? data : "", length);
    }
    return s->status;
  }
  return s2_pi(s, target, data ? data : "", length);
}

GTEXT_HTML_Status gtext_html_serializer_doctype(
    GTEXT_HTML_Serializer * s, const char * name, const char * public_id, const char * system_id) {
  const GTEXT_HTML_Status st = token_enter(s);
  if (st != GTEXT_HTML_OK) {
    return st;
  }
  s->started = true;
  if (fe_queueing(s)) {
    if (q_byte(s, Q_DOCTYPE) && q_cstr(s, name ? name : "") && q_cstr(s, public_id)) {
      q_cstr(s, system_id);
    }
    return s->status;
  }
  return s2_doctype(s, name, public_id, system_id);
}

static void result_fill(const GTEXT_HTML_Serializer * s, GTEXT_HTML_Serialize_Result * result) {
  if (!result) {
    return;
  }
  *result = s->r;
  result->status = s->status;
  result->message = s->message ? s->message : gtext_html_status_string(s->status);
  result->bytes_written = s->sent;
}

GTEXT_HTML_Status gtext_html_serializer_finish(
    GTEXT_HTML_Serializer * s, GTEXT_HTML_Serialize_Result * result) {
  if (!s) {
    return GTEXT_HTML_E_INVALID;
  }
  if (s->status == GTEXT_HTML_OK && s->finished) {
    return GTEXT_HTML_E_STATE;
  }
  if (!s->finished && s->status == GTEXT_HTML_OK) {
    s->finished = true;
    if (s->inject && s->ij_state == IJ_IN_HEAD) {
      ij_flush(s);
    }
    const Next n = {N_NONE, I_NONE, false};
    resolve_pending(s, &n);
    flush(s);
  }
  result_fill(s, result);
  return s->status;
}

GTEXT_HTML_Status gtext_html_serializer_context(
    GTEXT_HTML_Serializer * s, const char * ns, const char * name) {
  if (!s || !name || !*name) {
    return GTEXT_HTML_E_INVALID;
  }
  if (s->status != GTEXT_HTML_OK) {
    return s->status;
  }
  if (s->finished || s->started) {
    return GTEXT_HTML_E_STATE;
  }
  set_context(s, frame_of(ns, NULL, name));
  return GTEXT_HTML_OK;
}

/* ---- the tree walk --------------------------------------------------------- */

/// An element the tree walk is inside of.
typedef struct {
  const GTEXT_Markup_Node * element; ///< The element, for its end tag.
  const GTEXT_Markup_Node * next;    ///< Its next sibling, where the walk goes on.
} WalkFrame;

static bool kind_usable(GTEXT_Markup_Kind k) {
  switch (k) {
    case GTEXT_MARKUP_KIND_DOCUMENT:
    case GTEXT_MARKUP_KIND_FRAGMENT:
    case GTEXT_MARKUP_KIND_DOCTYPE:
    case GTEXT_MARKUP_KIND_ELEMENT:
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
    case GTEXT_MARKUP_KIND_COMMENT:
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
      return true;
    default:
      return false;
  }
}

/* The name an element is written under: the local name in the HTML, SVG and MathML namespaces, the
   qualified name in any other. Returns the prefix to write before the local name (or NULL). */
static const char * tag_prefix(const char * ns, const char * prefix) {
  if (!prefix || !ns) {
    return NULL;
  }
  if (strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0 || strcmp(ns, GTEXT_MARKUP_NS_SVG) == 0
      || strcmp(ns, GTEXT_MARKUP_NS_MATHML) == 0) {
    return NULL;
  }
  return prefix;
}

static GTEXT_HTML_Status walk_leaf(GTEXT_HTML_Serializer * s, const GTEXT_Markup_Node * n) {
  size_t len = 0;
  switch (gtext_markup_node_kind(n)) {
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA: {
      const char * d = gtext_markup_node_data(n, &len);
      if (len == 0) {
        return s->status;
      }
      if (fe_queueing(s)) {
        if (q_byte(s, Q_CHARS)) {
          q_str(s, d, len);
        }
        return s->status;
      }
      return s2_characters(s, d, len);
    }
    case GTEXT_MARKUP_KIND_COMMENT: {
      const char * d = gtext_markup_node_data(n, &len);
      if (fe_queueing(s)) {
        if (q_byte(s, Q_COMMENT)) {
          q_str(s, d ? d : "", len);
        }
        return s->status;
      }
      return s2_comment(s, d ? d : "", len);
    }
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      const char * d = gtext_markup_node_data(n, &len);
      const char * t = gtext_markup_pi_target(n);
      if (fe_queueing(s)) {
        if (q_byte(s, Q_PI) && q_cstr(s, t ? t : "")) {
          q_str(s, d ? d : "", len);
        }
        return s->status;
      }
      return s2_pi(s, t ? t : "", d ? d : "", len);
    }
    case GTEXT_MARKUP_KIND_DOCTYPE: {
      const char * name = gtext_markup_doctype_name(n);
      const char * pub = gtext_markup_doctype_public_id(n);
      const char * sys = gtext_markup_doctype_system_id(n);
      if (fe_queueing(s)) {
        if (q_byte(s, Q_DOCTYPE) && q_cstr(s, name ? name : "") && q_cstr(s, pub)) {
          q_cstr(s, sys);
        }
        return s->status;
      }
      return s2_doctype(s, name, pub, sys);
    }
    default:
      return s->status;
  }
}

static void walk_context(GTEXT_HTML_Serializer * s, const GTEXT_Markup_Node * element) {
  if (element && gtext_markup_node_kind(element) == GTEXT_MARKUP_KIND_ELEMENT) {
    const char * ns = gtext_markup_element_namespace(element);
    set_context(s, frame_of(ns ? ns : "", gtext_markup_element_prefix(element), gtext_markup_element_local_name(element)));
  }
}

static GTEXT_HTML_Status walk(GTEXT_HTML_Serializer * s, const GTEXT_Markup_Node * node,
    GTEXT_HTML_Serialize_Scope scope) {
  WalkFrame * frames = NULL;
  size_t nframes = 0, capframes = 0;
  const GTEXT_Markup_Node * cursor = NULL;
  bool single = false;
  const GTEXT_Markup_Kind top = gtext_markup_node_kind(node);

  if (top == GTEXT_MARKUP_KIND_DOCUMENT || top == GTEXT_MARKUP_KIND_FRAGMENT) {
    cursor = gtext_markup_node_first_child(node);
  }
  else if (scope == GTEXT_HTML_SERIALIZE_OUTER || top != GTEXT_MARKUP_KIND_ELEMENT) {
    /* The node itself: a leaf is only ever written as itself (CHILDREN of one was refused before the walk began). */
    if (scope == GTEXT_HTML_SERIALIZE_CHILDREN) {
      return fail(s, GTEXT_HTML_E_INVALID, "a leaf has no children to write");
    }
    cursor = node;
    single = true;
    walk_context(s, gtext_markup_node_parent(node));
  }
  else {
    /* The children of an element. */
    if (gtext_markup_html_node_is_void(node)) {
      return s->status;
    }
    if (gtext_markup_element_is(node, GTEXT_MARKUP_NS_HTML, "template")) {
      cursor = gtext_markup_node_first_child(gtext_markup_element_template_content(node));
    }
    else {
      cursor = gtext_markup_node_first_child(node);
      walk_context(s, node);
    }
  }

  GTEXT_HTML_Status status = GTEXT_HTML_OK;
  for (;;) {
    while (!cursor) {
      if (nframes == 0) {
        goto done;
      }
      const WalkFrame wf = frames[--nframes];
      cursor = wf.next;
      const char * element_ns = gtext_markup_element_namespace(wf.element);
      if (fe_end(s, element_ns ? element_ns : "", tag_prefix(element_ns, gtext_markup_element_prefix(wf.element)),
              gtext_markup_element_local_name(wf.element))
          != GTEXT_HTML_OK) {
        status = s->status;
        goto done;
      }
    }
    const GTEXT_Markup_Node * n = cursor;
    cursor = single ? NULL : gtext_markup_node_next_sibling(n);
    single = false; /* the node to write is the first one only: below it the walk goes on as usual */
    s->r.nodes_visited++;
    const GTEXT_Markup_Kind kind = gtext_markup_node_kind(n);
    if (kind != GTEXT_MARKUP_KIND_ELEMENT) {
      if (walk_leaf(s, n) != GTEXT_HTML_OK) {
        status = s->status;
        goto done;
      }
      continue;
    }
    /* An element in no namespace is not an HTML element (the token API's NULL means HTML). */
    const char * element_ns = gtext_markup_element_namespace(n);
    const char * ns = element_ns ? element_ns : "";
    const char * prefix = tag_prefix(element_ns, gtext_markup_element_prefix(n));
    const char * local = gtext_markup_element_local_name(n);
    const AttrSource as = {NULL, n, gtext_markup_element_attribute_count(n)};
    if (fe_start(s, ns, prefix, local, &as, false) != GTEXT_HTML_OK) {
      status = s->status;
      goto done;
    }
    const bool html = element_ns && strcmp(element_ns, GTEXT_MARKUP_NS_HTML) == 0;
    if (is_void_name(html, prefix, local)) {
      continue; /* a void element has no end tag, and its children are not written */
    }
    if (nframes >= s->o.max_depth) {
      status = fail(s, GTEXT_HTML_E_LIMIT_DEPTH, "elements nested too deeply");
      goto done;
    }
    if (nframes == capframes) {
      if (!grow(s, (void **)&frames, &capframes, nframes + 1, sizeof(WalkFrame))) {
        status = s->status;
        goto done;
      }
    }
    frames[nframes].element = n;
    frames[nframes].next = cursor;
    nframes++;
    const GTEXT_Markup_Node * content =
        html && strcmp(local, "template") == 0 ? gtext_markup_element_template_content(n) : NULL;
    cursor = gtext_markup_node_first_child(content ? content : n);
    /* A template with no content (built by hand) has no children to write, whatever else it holds. */
    if (html && strcmp(local, "template") == 0 && !content) {
      cursor = NULL;
    }
  }
done:
  gtext_allocator_free(&s->alloc, frames);
  return status;
}

/* ---- entry points ----------------------------------------------------------- */

static void fail_result(GTEXT_HTML_Serialize_Result * r, GTEXT_HTML_Status status, const char * message) {
  if (r) {
    memset(r, 0, sizeof(*r));
    r->status = status;
    r->message = message ? message : gtext_html_status_string(status);
  }
}

static GTEXT_HTML_Status serialize_impl(const GTEXT_Markup_Node * node, GTEXT_HTML_Serialize_Scope scope,
    const GTEXT_HTML_Serialize_Options * opts, const GTEXT_Markup_Sink * sink,
    GTEXT_HTML_Serialize_Result * result) {
  if (!node || !kind_usable(gtext_markup_node_kind(node))) {
    fail_result(result, GTEXT_HTML_E_INVALID, "node must not be NULL");
    return GTEXT_HTML_E_INVALID;
  }
  if ((int)scope < GTEXT_HTML_SERIALIZE_CHILDREN || scope > GTEXT_HTML_SERIALIZE_OUTER) {
    fail_result(result, GTEXT_HTML_E_INVALID, "unknown scope");
    return GTEXT_HTML_E_INVALID;
  }
  if (scope == GTEXT_HTML_SERIALIZE_CHILDREN) {
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(node);
    if (k != GTEXT_MARKUP_KIND_DOCUMENT && k != GTEXT_MARKUP_KIND_FRAGMENT && k != GTEXT_MARKUP_KIND_ELEMENT) {
      fail_result(result, GTEXT_HTML_E_INVALID, "a text, comment, processing instruction or document type has no children to write");
      return GTEXT_HTML_E_INVALID;
    }
  }
  const GTEXT_Markup_Document * doc = gtext_markup_node_document(node);
  const GTEXT_Allocator * alloc = opts && opts->allocator ? opts->allocator : (doc ? &doc->allocator : NULL);
  if (!alloc) {
    fail_result(result, GTEXT_HTML_E_INVALID, "the node belongs to no document and no allocator was given");
    return GTEXT_HTML_E_INVALID;
  }
  GTEXT_HTML_Serializer * s = NULL;
  const char * message = NULL;
  GTEXT_HTML_Status status = create(alloc, opts, sink, &s, &message);
  if (status != GTEXT_HTML_OK) {
    fail_result(result, status, message);
    return status;
  }
  status = walk(s, node, scope);
  if (status == GTEXT_HTML_OK) {
    status = gtext_html_serializer_finish(s, result);
  }
  else {
    result_fill(s, result);
  }
  gtext_html_serializer_free(s);
  return status;
}

GTEXT_HTML_Status gtext_html_serialize(const GTEXT_Markup_Node * node, GTEXT_HTML_Serialize_Scope scope,
    const GTEXT_HTML_Serialize_Options * opts, const GTEXT_Markup_Sink * sink,
    GTEXT_HTML_Serialize_Result * result) {
  return serialize_impl(node, scope, opts, sink, result);
}

/* ---- to a buffer -------------------------------------------------------------- */

/// The size of the header in front of a buffer, which holds the allocator it was made with (so that the buffer can be
/// released without the options or the document that made it). A multiple of the strictest alignment.
#define BUF_HEADER 64

_Static_assert(sizeof(GTEXT_Allocator) <= BUF_HEADER, "the allocator fits in the buffer's header");

/// The buffer of gtext_html_serialize_buffer().
typedef struct {
  const GTEXT_Allocator * alloc; ///< Where it is allocated (copied into the header when it is).
  unsigned char * base;          ///< The header, then the bytes and a NUL; NULL until there are bytes.
  size_t length;                 ///< Bytes written.
  size_t capacity;               ///< Bytes allocated after the header.
} WBuf;

static GTEXT_Markup_Status wbuf_write(void * user, const char * bytes, size_t len) {
  WBuf * b = user;
  if (len > SIZE_MAX - b->length - 1 - BUF_HEADER) {
    return GTEXT_MARKUP_E_OOM;
  }
  if (b->length + len + 1 > b->capacity) {
    size_t cap = b->capacity ? b->capacity : 256;
    while (cap < b->length + len + 1) {
      if (cap > (SIZE_MAX - BUF_HEADER) / 2) {
        return GTEXT_MARKUP_E_OOM;
      }
      cap *= 2;
    }
    unsigned char * grown = gtext_allocator_realloc(b->alloc, b->base, BUF_HEADER + cap);
    if (!grown) {
      return GTEXT_MARKUP_E_OOM;
    }
    if (!b->base) {
      memcpy(grown, b->alloc, sizeof(GTEXT_Allocator));
    }
    b->base = grown;
    b->capacity = cap;
  }
  memcpy(b->base + BUF_HEADER + b->length, bytes, len);
  b->length += len;
  b->base[BUF_HEADER + b->length] = '\0';
  return GTEXT_MARKUP_OK;
}

GTEXT_HTML_Status gtext_html_serialize_buffer(const GTEXT_Markup_Node * node,
    GTEXT_HTML_Serialize_Scope scope, const GTEXT_HTML_Serialize_Options * opts, char ** out, size_t * out_len,
    GTEXT_HTML_Serialize_Result * result) {
  if (!node || !out || !kind_usable(gtext_markup_node_kind(node))) {
    fail_result(result, GTEXT_HTML_E_INVALID, "node and out must not be NULL, and the node must be a node");
    return GTEXT_HTML_E_INVALID;
  }
  const GTEXT_Markup_Document * doc = gtext_markup_node_document(node);
  const GTEXT_Allocator * alloc = opts && opts->allocator ? opts->allocator : (doc ? &doc->allocator : NULL);
  if (!alloc) {
    fail_result(result, GTEXT_HTML_E_INVALID, "the node belongs to no document and no allocator was given");
    return GTEXT_HTML_E_INVALID;
  }
  WBuf b = {alloc, NULL, 0, 0};
  const GTEXT_Markup_Sink sink = {wbuf_write, &b};
  const GTEXT_HTML_Status status = serialize_impl(node, scope, opts, &sink, result);
  if (status != GTEXT_HTML_OK) {
    gtext_allocator_free(alloc, b.base);
    return status;
  }
  if (!b.base) { /* an empty output still has its NUL */
    wbuf_write(&b, "", 0);
    if (!b.base) {
      fail_result(result, GTEXT_HTML_E_OOM, "out of memory");
      return GTEXT_HTML_E_OOM;
    }
  }
  *out = (char *)b.base + BUF_HEADER;
  if (out_len) {
    *out_len = b.length;
  }
  return GTEXT_HTML_OK;
}

void gtext_html_serialize_buffer_free(const GTEXT_Markup_Document * doc,
    const GTEXT_HTML_Serialize_Options * opts, char * buffer) {
  (void)doc;
  (void)opts;
  if (!buffer) {
    return;
  }
  unsigned char * base = (unsigned char *)buffer - BUF_HEADER;
  GTEXT_Allocator alloc;
  memcpy(&alloc, base, sizeof(alloc));
  gtext_allocator_free(&alloc, base);
}
