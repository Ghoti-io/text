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
 * The XML writer: a markup tree to XML 1.0 bytes.
 *
 * XML 1.0 (Fifth Edition) [2] Char, [15] Comment, [16] PI, [18] CDSect, [28]
 * doctypedecl, [10] AttValue, [14] CharData and 2.11 on line ends; Namespaces
 * in XML 1.0 (Third Edition) on declarations and the reserved prefixes.
 *
 * ## Shape
 *
 * One iterative walk of the tree (parent and sibling links; no recursion, so
 * any depth), run **twice** over the same code: first with `dry` set, which
 * checks everything and counts what it would write but sends nothing, then for
 * real. That is what makes a refusal write nothing, and it makes the output
 * size limit a refusal rather than a truncation. The cost is a second walk.
 *
 * The open elements are a heap array of frames. Namespace declarations in scope
 * are a stack of bindings with a hash table from prefix to its innermost
 * binding, so a lookup costs the same at any depth and an adversarial tree of
 * many distinct prefixes is not quadratic.
 *
 * Output goes through one narrow door (out_raw) that counts bytes against the
 * limit and batches them for the sink; characters go through enc_cp, which is
 * where the output encoding lives and where a character that has no byte in it
 * becomes a numeric reference or a refusal, according to where it is.
 */

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/markup/markup_html.h>
#include <ghoti.io/unicode/utf.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "xml_internal.h"

#define NS_XML GTEXT_MARKUP_NS_XML
#define NS_XMLNS GTEXT_MARKUP_NS_XMLNS
#define W_BUFFER 4096
#define W_PATH_CAP 252

/* ---- state -------------------------------------------------------------- */

enum {
  F_INDENT = 1,   /* the children are on lines of their own */
  F_NOINDENT = 2, /* nothing at or below this is indented */
  F_PRESERVE = 4  /* xml:space="preserve" on it or above */
};

/* What an open XHTML element means for the elements inside it (xhtml mode). */
enum {
  HZ_A = 1,       /* inside an a */
  HZ_FORM = 2,    /* inside a form */
  HZ_P = 4,       /* inside a p */
  HZ_LI = 8,      /* an li is reached from here before a special element */
  HZ_DD = 16,     /* likewise a dd or dt */
  HZ_BUTTON = 32, /* inside a button */
  HZ_FOREIGN = 64, /* directly in SVG or MathML content that HTML would build
                      as such: reached from an svg or math element, and not in
                      an integration point */
  HZ_NOBR = 128,  /* inside a nobr */
  HZ_SELECT = 256, /* inside a select, not in a template inside it */
  HZ_RUBY = 512   /* inside a ruby (the scope boundaries are not modelled) */
};

typedef struct {
  size_t mark;         /* bindings in scope outside this element */
  unsigned char flags; /* F_* */
  unsigned hz;         /* HZ_*, for the elements below */
} wframe;

typedef struct {
  const char * key; /* NULL: the slot is free */
  size_t klen;
  size_t hash;
  size_t head; /* 1 + index of the innermost binding, 0 for none */
} wentry;

typedef struct {
  const char * key;
  size_t klen;
  const char * uri;
  size_t ulen;
  size_t prev; /* the entry's head before this binding */
} wbinding;

typedef struct {
  const char * key;
  size_t klen;
  const char * uri; /* NULL: xmlns="" */
  size_t ulen;
} wadd;

typedef struct {
  const char * key;
  size_t klen;
} wuse;

typedef struct {
  const GTEXT_Allocator * alloc;
  const GTEXT_Markup_Document * doc;
  GTEXT_Markup_Sink sink;
  GTEXT_XML_Write_Options o; /* resolved: no zeros left to mean a default */
  const char * indent;
  size_t indent_len;
  const char * newline;
  size_t newline_len;
  bool pretty;
  GTEXT_XML_Write_Result * r;
  GTEXT_XML_Status status;
  bool dry;
  bool bom;      /* UTF-8 output starts with a byte order mark */
  bool declared; /* the XML declaration is being written in this pass */
  bool xml11;    /* version profile selected by the document */
  bool xhtml;    /* GTEXT_XML_Write_Options::xhtml */
  bool declare_now; /* xhtml: the root element's XHTML namespace is declared
                       here if it is not already */
  size_t seed;

  /* output */
  size_t total; /* bytes this pass has produced, against the limit */
  size_t sent;  /* bytes the sink has accepted */
  size_t used;
  unsigned char buf[W_BUFFER];

  /* where we are, for an error */
  const GTEXT_Markup_Node * at;
  int at_attr;

  wframe * frames;
  size_t nframes, framecap;

  wentry * table;
  size_t tablecap, tablecount;
  wbinding * bind;
  size_t nbind, bindcap;
  wadd * adds;
  size_t nadds, addcap;
  wuse * uses; /* prefixes this element's names rely on (declare mode) */
  size_t nuses, usecap;
} wctx;

#define COUNT(w, field) \
  do { \
    if ((w)->dry) { \
      (w)->r->field++; \
    } \
  } while (0)

/* ---- failing ------------------------------------------------------------ */

static void build_path(const GTEXT_Markup_Node * node, int attr, char * out);
static GTEXT_XML_Status check_raw_data(
    wctx * w, const char * data, size_t len, const char * what);

static GTEXT_XML_Status fail(
    wctx * w, GTEXT_XML_Status code, const char * message) {
  if (w->status == GTEXT_XML_OK) {
    w->status = code;
    w->r->code = code;
    w->r->message = message;
    if (w->at) {
      build_path(w->at, w->at_attr, w->r->path);
    }
  }
  return w->status;
}

static GTEXT_XML_Status fail_name(wctx * w, GTEXT_XML_Status code,
    const char * message, const char * name, size_t len) {
  if (w->status == GTEXT_XML_OK && name) {
    if (len > sizeof(w->r->name) - 1) {
      len = sizeof(w->r->name) - 1;
    }
    memcpy(w->r->name, name, len);
    w->r->name[len] = '\0';
  }
  return fail(w, code, message);
}

static GTEXT_XML_Status oom(wctx * w) {
  return fail(w, GTEXT_XML_E_OOM, "out of memory");
}

/* ---- the path to a node ------------------------------------------------- */

typedef struct {
  char * tmp;
  size_t pos;
  int cut;
} pathbuf;

static void path_prepend(pathbuf * p, const char * s, size_t n) {
  if (p->cut) {
    return;
  }
  if (n > p->pos) {
    p->cut = 1;
    return;
  }
  p->pos -= n;
  memcpy(p->tmp + p->pos, s, n);
}

/* Do two siblings have the same label, as far as an index is concerned? */
static int same_label(
    const GTEXT_Markup_Node * a, const GTEXT_Markup_Node * b) {
  const GTEXT_Markup_Kind ka = gtext_markup_node_kind(a);
  if (ka != gtext_markup_node_kind(b)) {
    return 0;
  }
  if (ka == GTEXT_MARKUP_KIND_ELEMENT) {
    const char * pa = gtext_markup_element_prefix(a);
    const char * pb = gtext_markup_element_prefix(b);
    return strcmp(gtext_markup_element_local_name(a),
               gtext_markup_element_local_name(b)) == 0
        && ((pa == NULL) == (pb == NULL)) && (!pa || strcmp(pa, pb) == 0);
  }
  if (ka == GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION) {
    return strcmp(gtext_markup_pi_target(a), gtext_markup_pi_target(b)) == 0;
  }
  return 1;
}

static void label_add(char * label, size_t * len, size_t cap, const char * s,
    size_t n) {
  if (n > cap - *len) {
    n = cap - *len;
  }
  memcpy(label + *len, s, n);
  *len += n;
}

static void path_node(pathbuf * p, const GTEXT_Markup_Node * n) {
  char label[200];
  size_t len = 0;
  const size_t cap = sizeof(label);
  switch (gtext_markup_node_kind(n)) {
    case GTEXT_MARKUP_KIND_ELEMENT: {
      const char * prefix = gtext_markup_element_prefix(n);
      if (prefix) {
        label_add(label, &len, cap, prefix, strlen(prefix));
        label_add(label, &len, cap, ":", 1);
      }
      const char * local = gtext_markup_element_local_name(n);
      label_add(label, &len, cap, local, strlen(local));
      break;
    }
    case GTEXT_MARKUP_KIND_TEXT:
      label_add(label, &len, cap, "text()", 6);
      break;
    case GTEXT_MARKUP_KIND_CDATA:
      label_add(label, &len, cap, "cdata()", 7);
      break;
    case GTEXT_MARKUP_KIND_COMMENT:
      label_add(label, &len, cap, "comment()", 9);
      break;
    case GTEXT_MARKUP_KIND_DOCTYPE:
      label_add(label, &len, cap, "doctype()", 9);
      break;
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      const char * target = gtext_markup_pi_target(n);
      label_add(label, &len, cap, "processing-instruction(", 23);
      label_add(label, &len, cap - 1, target, strlen(target));
      label_add(label, &len, cap, ")", 1);
      break;
    }
    default:
      label_add(label, &len, cap, "node()", 6);
      break;
  }
  /* The index among siblings that look the same, when there is more than one. */
  size_t before = 0;
  size_t after = 0;
  for (const GTEXT_Markup_Node * s = gtext_markup_node_previous_sibling(n); s;
      s = gtext_markup_node_previous_sibling(s)) {
    before += same_label(s, n) ? 1 : 0;
  }
  for (const GTEXT_Markup_Node * s = gtext_markup_node_next_sibling(n); s;
      s = gtext_markup_node_next_sibling(s)) {
    after += same_label(s, n) ? 1 : 0;
  }
  if (before + after > 0) {
    char index[32];
    size_t at = sizeof(index);
    size_t v = before + 1;
    index[--at] = ']';
    do {
      index[--at] = (char)('0' + v % 10);
      v /= 10;
    } while (v);
    index[--at] = '[';
    path_prepend(p, index + at, sizeof(index) - at);
  }
  path_prepend(p, label, len);
  path_prepend(p, "/", 1);
}

static void build_path(const GTEXT_Markup_Node * node, int attr, char * out) {
  char tmp[W_PATH_CAP];
  pathbuf p = {tmp, W_PATH_CAP, 0};
  if (attr >= 0) {
    GTEXT_Markup_Attribute a;
    if (gtext_markup_element_attribute_at(node, (size_t)attr, &a)
        == GTEXT_MARKUP_OK) {
      path_prepend(&p, a.local, strlen(a.local));
      if (a.prefix) {
        path_prepend(&p, ":", 1);
        path_prepend(&p, a.prefix, strlen(a.prefix));
      }
      path_prepend(&p, "/@", 2);
    }
  }
  for (const GTEXT_Markup_Node * n = node; n && !p.cut; ) {
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(n);
    if (k == GTEXT_MARKUP_KIND_DOCUMENT || k == GTEXT_MARKUP_KIND_NONE) {
      break;
    }
    path_node(&p, n);
    n = gtext_markup_node_parent(n);
  }
  size_t n = 0;
  if (p.cut) {
    memcpy(out, "...", 3);
    n = 3;
  }
  const size_t tail = W_PATH_CAP - p.pos;
  if (tail == 0 && !p.cut) {
    out[0] = '/';
    out[1] = '\0';
    return;
  }
  memcpy(out + n, tmp + p.pos, tail);
  out[n + tail] = '\0';
}

/* ---- output ------------------------------------------------------------- */

static GTEXT_XML_Status flush(wctx * w) {
  if (w->used == 0) {
    return GTEXT_XML_OK;
  }
  const GTEXT_Markup_Status st =
      w->sink.write(w->sink.user, (const char *)w->buf, w->used);
  if (st != GTEXT_MARKUP_OK) {
    w->at = NULL;
    return st == GTEXT_MARKUP_E_OOM ? oom(w)
                                    : fail(w, GTEXT_XML_E_WRITE, "output refused");
  }
  w->sent += w->used;
  w->used = 0;
  return GTEXT_XML_OK;
}

/* Count n bytes against the limit. */
static GTEXT_XML_Status account(wctx * w, size_t n) {
  if (n > w->o.max_output_bytes - w->total) {
    return fail(w, GTEXT_XML_E_LIMIT_OUTPUT, "output too long");
  }
  w->total += n;
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status out_raw(wctx * w, const void * p, size_t n) {
  if (w->status != GTEXT_XML_OK) {
    return w->status;
  }
  if (account(w, n) != GTEXT_XML_OK) {
    return w->status;
  }
  if (w->dry || n == 0) {
    return GTEXT_XML_OK;
  }
  const unsigned char * b = p;
  while (n > 0) {
    size_t room = W_BUFFER - w->used;
    const size_t k = n < room ? n : room;
    memcpy(w->buf + w->used, b, k);
    w->used += k;
    b += k;
    n -= k;
    if (w->used == W_BUFFER && flush(w) != GTEXT_XML_OK) {
      return w->status;
    }
  }
  return GTEXT_XML_OK;
}

static bool is_utf16(const wctx * w) {
  return w->o.encoding == GTEXT_XML_OUTPUT_UTF16LE
      || w->o.encoding == GTEXT_XML_OUTPUT_UTF16BE;
}

/* Structural text: ASCII, whatever the encoding. */
static GTEXT_XML_Status put_ascii(wctx * w, const char * s, size_t n) {
  if (!is_utf16(w)) {
    return out_raw(w, s, n);
  }
  const bool le = w->o.encoding == GTEXT_XML_OUTPUT_UTF16LE;
  for (size_t i = 0; i < n; i++) {
    unsigned char two[2];
    two[le ? 0 : 1] = (unsigned char)s[i];
    two[le ? 1 : 0] = 0;
    if (out_raw(w, two, 2) != GTEXT_XML_OK) {
      return w->status;
    }
  }
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status put_str(wctx * w, const char * s) {
  return put_ascii(w, s, strlen(s));
}

/* `times` copies of an ASCII string. A dry pass does not loop: the depth of a
   tree and the length of an indent multiply, and an adversarial pair would make
   the check itself the quadratic part. */
static GTEXT_XML_Status put_repeat(
    wctx * w, const char * s, size_t n, size_t times) {
  const size_t unit = is_utf16(w) ? n * 2 : n;
  if (unit != 0 && times > SIZE_MAX / unit) {
    return fail(w, GTEXT_XML_E_LIMIT_OUTPUT, "output too long");
  }
  if (w->dry) {
    return account(w, unit * times);
  }
  for (size_t i = 0; i < times; i++) {
    if (put_ascii(w, s, n) != GTEXT_XML_OK) {
      return w->status;
    }
  }
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status newline_indent(wctx * w, size_t levels) {
  if (put_ascii(w, w->newline, w->newline_len) != GTEXT_XML_OK) {
    return w->status;
  }
  return put_repeat(w, w->indent, w->indent_len, levels);
}

/* One character in the output encoding. `ref_ok`: a reference may stand in for
   it (text and attribute values); otherwise it is a refusal. */
static GTEXT_XML_Status enc_cp(wctx * w, uint32_t cp, bool ref_ok) {
  unsigned char b[4];
  switch (w->o.encoding) {
    case GTEXT_XML_OUTPUT_UTF8: {
      size_t n;
      if (cp < 0x80) {
        b[0] = (unsigned char)cp;
        n = 1;
      } else if (cp < 0x800) {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
      } else if (cp < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
      } else {
        b[0] = (unsigned char)(0xF0 | (cp >> 18));
        b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
      }
      return out_raw(w, b, n);
    }
    case GTEXT_XML_OUTPUT_UTF16LE:
    case GTEXT_XML_OUTPUT_UTF16BE: {
      const bool le = w->o.encoding == GTEXT_XML_OUTPUT_UTF16LE;
      uint32_t units[2];
      size_t count = 1;
      if (cp < 0x10000) {
        units[0] = cp;
      } else {
        cp -= 0x10000;
        units[0] = 0xD800 + (cp >> 10);
        units[1] = 0xDC00 + (cp & 0x3FF);
        count = 2;
      }
      for (size_t i = 0; i < count; i++) {
        b[le ? 0 : 1] = (unsigned char)(units[i] & 0xFF);
        b[le ? 1 : 0] = (unsigned char)(units[i] >> 8);
        if (out_raw(w, b, 2) != GTEXT_XML_OK) {
          return w->status;
        }
      }
      return GTEXT_XML_OK;
    }
    case GTEXT_XML_OUTPUT_ISO_8859_1:
    case GTEXT_XML_OUTPUT_US_ASCII: {
      const uint32_t limit =
          w->o.encoding == GTEXT_XML_OUTPUT_ISO_8859_1 ? 0xFF : 0x7F;
      if (w->xhtml && cp >= 0x80 && cp <= 0x9F) {
        /* HTML reads &#x80;-&#x9F; as the Windows-1252 characters at those codes, and
           reads a document labelled ISO-8859-1 as Windows-1252, so the raw bytes 0x80-0x9F
           mean other characters too: a C1 control character cannot be spelled either way. */
        return fail(w, GTEXT_XML_E_XHTML,
            "an HTML parser reads a C1 control character, as a numeric reference or as a "
            "byte of an ISO-8859-1 document, as the Windows-1252 character at that code");
      }
      if (cp <= limit) {
        if (cp >= 0x80 && !w->declared) {
          /* A byte that is not UTF-8, in a document that does not say so:
             a reader would take it for an error or for something else. */
          return fail(w, GTEXT_XML_E_UNREPRESENTABLE,
              "ISO-8859-1 output with non-ASCII characters needs the XML "
              "declaration, which this document is not given");
        }
        b[0] = (unsigned char)cp;
        return out_raw(w, b, 1);
      }
      if (!ref_ok) {
        return fail(w, GTEXT_XML_E_UNREPRESENTABLE,
            "a character the output encoding cannot spell, where no "
            "character reference can stand in for it");
      }
      char ref[16];
      size_t n = 0;
      ref[n++] = '&';
      ref[n++] = '#';
      ref[n++] = 'x';
      char digits[8];
      int d = 0;
      while (cp) {
        digits[d++] = "0123456789ABCDEF"[cp & 0xF];
        cp >>= 4;
      }
      while (d > 0) {
        ref[n++] = digits[--d];
      }
      ref[n++] = ';';
      COUNT(w, references_written);
      return put_ascii(w, ref, n);
    }
  }
  return fail(w, GTEXT_XML_E_INVALID, "unknown output encoding");
}

static bool xml11_char(uint32_t cp) {
  return (cp >= 0x1 && cp <= 0xD7FF)
      || (cp >= 0xE000 && cp <= 0xFFFD)
      || (cp >= 0x10000 && cp <= 0x10FFFF);
}

static bool xml11_restricted(uint32_t cp) {
  return (cp >= 0x1 && cp <= 0x8) || cp == 0xB || cp == 0xC
      || (cp >= 0xE && cp <= 0x1F) || (cp >= 0x7F && cp <= 0x84)
      || (cp >= 0x86 && cp <= 0x9F);
}

static GTEXT_XML_Status put_char_ref(wctx * w, uint32_t cp) {
  char ref[16];
  size_t n = 0;
  ref[n++] = '&'; ref[n++] = '#'; ref[n++] = 'x';
  char digits[8];
  size_t d = 0;
  do { digits[d++] = "0123456789ABCDEF"[cp & 0xF]; cp >>= 4; } while (cp);
  while (d) ref[n++] = digits[--d];
  ref[n++] = ';';
  COUNT(w, references_written);
  return put_ascii(w, ref, n);
}

static GTEXT_XML_Status put_escaped_xml11(wctx * w,
    GTEXT_Markup_Escape_Mode mode, const char * text, size_t len) {
  size_t i = 0;
  while (i < len) {
    uint32_t cp = 0;
    bool valid = false;
    size_t used = guni_utf8_decode(text + i, len - i, &cp, &valid);
    if (!valid || !used) return fail(w, GTEXT_XML_E_UTF8, "invalid UTF-8");
    if (!xml11_char(cp)) return fail(w, GTEXT_XML_E_CHARACTER, "character not allowed in XML 1.1");
    const char * escaped = NULL;
    if (cp == '&') escaped = "&amp;";
    else if (cp == '<') escaped = "&lt;";
    else if (cp == '>') escaped = "&gt;";
    else if (mode == GTEXT_MARKUP_ESCAPE_XML_ATTRIBUTE && cp == '"') escaped = "&quot;";
    else if (cp == '\r' || (mode == GTEXT_MARKUP_ESCAPE_XML_ATTRIBUTE && (cp == '\t' || cp == '\n'))
        || cp == 0x85 || cp == 0x2028 || xml11_restricted(cp)) {
      if (put_char_ref(w, cp) != GTEXT_XML_OK) return w->status;
      i += used;
      continue;
    }
    if (escaped) {
      if (put_str(w, escaped) != GTEXT_XML_OK) return w->status;
    } else if (enc_cp(w, cp, true) != GTEXT_XML_OK) return w->status;
    i += used;
  }
  return GTEXT_XML_OK;
}

/* Text that is already valid UTF-8 and already escaped. */
static GTEXT_XML_Status put_text(
    wctx * w, const char * bytes, size_t len, bool ref_ok) {
  if (w->status != GTEXT_XML_OK) {
    return w->status;
  }
  if (w->o.encoding == GTEXT_XML_OUTPUT_UTF8) {
    return out_raw(w, bytes, len);
  }
  size_t i = 0;
  const bool one_byte = !is_utf16(w);
  while (i < len) {
    if (one_byte && (unsigned char)bytes[i] < 0x80) {
      size_t j = i;
      while (j < len && (unsigned char)bytes[j] < 0x80) {
        j++;
      }
      if (out_raw(w, bytes + i, j - i) != GTEXT_XML_OK) {
        return w->status;
      }
      i = j;
      continue;
    }
    uint32_t cp = 0;
    bool valid = false;
    const size_t used = guni_utf8_decode(bytes + i, len - i, &cp, &valid);
    if (!valid || used == 0) {
      return fail(w, GTEXT_XML_E_UTF8, "invalid UTF-8");
    }
    if (enc_cp(w, cp, ref_ok) != GTEXT_XML_OK) {
      return w->status;
    }
    i += used;
  }
  return GTEXT_XML_OK;
}

static GTEXT_Markup_Status escape_sink(
    void * user, const char * bytes, size_t len) {
  wctx * w = user;
  return put_text(w, bytes, len, true) == GTEXT_XML_OK ? GTEXT_MARKUP_OK
                                                       : GTEXT_MARKUP_E_WRITE;
}

static GTEXT_XML_Status put_escaped(wctx * w, GTEXT_Markup_Escape_Mode mode,
    const char * text, size_t len) {
  if (w->status != GTEXT_XML_OK) {
    return w->status;
  }
  if (w->xml11) return put_escaped_xml11(w, mode, text, len);
  const GTEXT_Markup_Sink sink = {escape_sink, w};
  const GTEXT_Markup_Status st = gtext_markup_escape(mode, text, len, &sink);
  if (st == GTEXT_MARKUP_OK) {
    return GTEXT_XML_OK;
  }
  if (w->status != GTEXT_XML_OK) {
    return w->status;
  }
  switch (st) {
    case GTEXT_MARKUP_E_UNREPRESENTABLE:
      return fail(w, GTEXT_XML_E_CHARACTER, "character not allowed in XML");
    case GTEXT_MARKUP_E_ENCODING:
      return fail(w, GTEXT_XML_E_UTF8, "invalid UTF-8");
    default:
      return fail(w, GTEXT_XML_E_INVALID, "text could not be escaped");
  }
}

/* A qualified name, whose parts were checked. */
static GTEXT_XML_Status put_qname(
    wctx * w, const char * prefix, const char * local) {
  if (prefix) {
    if (put_text(w, prefix, strlen(prefix), false) != GTEXT_XML_OK
        || put_ascii(w, ":", 1) != GTEXT_XML_OK) {
      return w->status;
    }
  }
  return put_text(w, local, strlen(local), false);
}

/* ---- namespace scope ---------------------------------------------------- */

static size_t hash_key(const wctx * w, const char * k, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)w->seed;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)k[i];
    h *= 0x100000001b3ull;
  }
  h ^= h >> 32;
  h *= 0x9E3779B97F4A7C15ull;
  h ^= h >> 29;
  return (size_t)h;
}

static wentry * table_probe(
    const wctx * w, const char * k, size_t n, size_t hash) {
  const size_t mask = w->tablecap - 1;
  for (size_t i = hash & mask;; i = (i + 1) & mask) {
    wentry * e = &w->table[i];
    if (!e->key || (e->hash == hash && e->klen == n
                       && (n == 0 || memcmp(e->key, k, n) == 0))) {
      return e;
    }
  }
}

static GTEXT_XML_Status table_grow(wctx * w) {
  const size_t cap = w->tablecap ? w->tablecap * 2 : 32;
  if (cap > SIZE_MAX / sizeof(wentry)) {
    return oom(w);
  }
  wentry * table = gtext_allocator_calloc(w->alloc, cap, sizeof(wentry));
  if (!table) {
    return oom(w);
  }
  wentry * old = w->table;
  const size_t old_cap = w->tablecap;
  w->table = table;
  w->tablecap = cap;
  for (size_t i = 0; i < old_cap; i++) {
    if (old[i].key) {
      *table_probe(w, old[i].key, old[i].klen, old[i].hash) = old[i];
    }
  }
  gtext_allocator_free(w->alloc, old);
  return GTEXT_XML_OK;
}

static const wbinding * scope_lookup(
    const wctx * w, const char * k, size_t n) {
  if (!w->tablecap) {
    return NULL;
  }
  const wentry * e = table_probe(w, k, n, hash_key(w, k, n));
  return e->key && e->head ? &w->bind[e->head - 1] : NULL;
}

static GTEXT_XML_Status scope_bind(wctx * w, const char * k, size_t n,
    const char * uri, size_t ulen) {
  if ((w->tablecount + 1) * 2 > w->tablecap
      && table_grow(w) != GTEXT_XML_OK) {
    return w->status;
  }
  if (w->nbind == w->bindcap) {
    const size_t cap = w->bindcap ? w->bindcap * 2 : 16;
    if (cap > SIZE_MAX / sizeof(wbinding)) {
      return oom(w);
    }
    wbinding * bind =
        gtext_allocator_realloc(w->alloc, w->bind, cap * sizeof(wbinding));
    if (!bind) {
      return oom(w);
    }
    w->bind = bind;
    w->bindcap = cap;
  }
  const size_t hash = hash_key(w, k, n);
  wentry * e = table_probe(w, k, n, hash);
  if (!e->key) {
    e->key = k;
    e->klen = n;
    e->hash = hash;
    e->head = 0;
    w->tablecount++;
  }
  wbinding * b = &w->bind[w->nbind++];
  b->key = k;
  b->klen = n;
  b->uri = uri;
  b->ulen = ulen;
  b->prev = e->head;
  e->head = w->nbind;
  return GTEXT_XML_OK;
}

static void scope_release(wctx * w, size_t mark) {
  while (w->nbind > mark) {
    const wbinding * b = &w->bind[--w->nbind];
    wentry * e = table_probe(w, b->key, b->klen, hash_key(w, b->key, b->klen));
    e->head = b->prev;
  }
}

static void scope_reset(wctx * w) {
  if (w->table) {
    memset(w->table, 0, w->tablecap * sizeof(wentry));
  }
  w->tablecount = 0;
  w->nbind = 0;
}

static bool uri_equal(
    const char * a, size_t alen, const char * b, size_t blen) {
  return alen == blen && (alen == 0 || memcmp(a, b, alen) == 0);
}

static bool is_lit(const char * s, size_t n, const char * lit) {
  const size_t m = strlen(lit);
  return n == m && memcmp(s, lit, m) == 0;
}

/* ---- names and namespaces of one element -------------------------------- */

static bool declaring(const wctx * w) {
  return w->o.declare_namespaces || w->declare_now;
}

static GTEXT_XML_Status note_used(wctx * w, const char * k, size_t n) {
  if (!declaring(w)) {
    return GTEXT_XML_OK;
  }
  if (w->nuses == w->usecap) {
    const size_t cap = w->usecap ? w->usecap * 2 : 8;
    if (cap > SIZE_MAX / sizeof(wuse)) {
      return oom(w);
    }
    wuse * uses = gtext_allocator_realloc(w->alloc, w->uses, cap * sizeof(wuse));
    if (!uses) {
      return oom(w);
    }
    w->uses = uses;
    w->usecap = cap;
  }
  w->uses[w->nuses].key = k;
  w->uses[w->nuses].klen = n;
  w->nuses++;
  return GTEXT_XML_OK;
}

static bool was_used(const wctx * w, const char * k, size_t n) {
  for (size_t i = 0; i < w->nuses; i++) {
    if (w->uses[i].klen == n && (n == 0 || memcmp(w->uses[i].key, k, n) == 0)) {
      return true;
    }
  }
  return false;
}

/* The name `key` (a prefix, "" for the default namespace) must mean `ns` here.
   `mark` is where this element's own declarations begin. */
static GTEXT_XML_Status need_binding(wctx * w, const char * key, size_t klen,
    const char * ns, size_t mark) {
  const size_t nlen = ns ? strlen(ns) : 0;
  const wbinding * b = scope_lookup(w, key, klen);
  const char * have = b && b->ulen ? b->uri : NULL;
  const size_t hlen = b ? b->ulen : 0;
  if (uri_equal(have ? have : "", hlen, ns ? ns : "", nlen)) {
    return note_used(w, key, klen);
  }
  const bool own = b && (size_t)(b - w->bind) >= mark;
  if (own) {
    return fail_name(w, GTEXT_XML_E_NAMESPACE,
        "an xmlns attribute contradicts the namespace of the name that uses it",
        key, klen);
  }
  if (!declaring(w)) {
    return fail_name(w, GTEXT_XML_E_NAMESPACE,
        b ? (klen ? "the prefix is bound to a different namespace here"
                  : "the default namespace here is not this element's")
          : "no xmlns declaration is in scope for this prefix",
        key, klen);
  }
  if (was_used(w, key, klen)) {
    return fail_name(w, GTEXT_XML_E_NAMESPACE,
        "the prefix is already relied on by this element for another "
        "namespace, so it cannot be redeclared",
        key, klen);
  }
  if (w->nadds == w->addcap) {
    const size_t cap = w->addcap ? w->addcap * 2 : 4;
    if (cap > SIZE_MAX / sizeof(wadd)) {
      return oom(w);
    }
    wadd * adds = gtext_allocator_realloc(w->alloc, w->adds, cap * sizeof(wadd));
    if (!adds) {
      return oom(w);
    }
    w->adds = adds;
    w->addcap = cap;
  }
  if (scope_bind(w, key, klen, ns ? ns : "", nlen) != GTEXT_XML_OK) {
    return w->status;
  }
  wadd * a = &w->adds[w->nadds++];
  a->key = key;
  a->klen = klen;
  a->uri = ns;
  a->ulen = nlen;
  COUNT(w, namespace_declarations_added);
  return note_used(w, key, klen);
}

static bool ncname(const char * s) {
  return gtext_markup_is_xml_ncname(s, strlen(s));
}

static GTEXT_XML_Status bad_name(wctx * w, const char * s) {
  return fail_name(w, GTEXT_XML_E_NAME, "not an XML name", s, strlen(s));
}

/* An attribute's own shape; and, for a declaration, the binding it makes. */
static GTEXT_XML_Status check_attribute_shape(
    wctx * w, const GTEXT_Markup_Attribute * a) {
  const bool decl = a->ns && strcmp(a->ns, NS_XMLNS) == 0;
  if (!decl) {
    if (!ncname(a->local)) {
      return bad_name(w, a->local);
    }
    if (!a->ns) {
      if (a->prefix) {
        return fail_name(w, GTEXT_XML_E_NAMESPACE,
            "an attribute has a prefix and no namespace", a->prefix,
            strlen(a->prefix));
      }
      if (strcmp(a->local, "xmlns") == 0) {
        return fail_name(w, GTEXT_XML_E_NAMESPACE,
            "an attribute named xmlns outside the xmlns namespace would be "
            "read as a declaration",
            a->local, strlen(a->local));
      }
      return GTEXT_XML_OK;
    }
    if (!a->prefix) {
      return fail_name(w, GTEXT_XML_E_NAMESPACE,
          "an attribute in a namespace has no prefix, and a default namespace "
          "does not apply to attributes",
          a->local, strlen(a->local));
    }
    if (!ncname(a->prefix)) {
      return bad_name(w, a->prefix);
    }
    if (strcmp(a->prefix, "xmlns") == 0) {
      return fail_name(w, GTEXT_XML_E_NAMESPACE,
          "the prefix xmlns is reserved for declarations", a->prefix, 5);
    }
    if (strcmp(a->ns, NS_XML) == 0 ? strcmp(a->prefix, "xml") != 0
                                   : strcmp(a->prefix, "xml") == 0) {
      return fail_name(w, GTEXT_XML_E_NAMESPACE,
          "the prefix xml and the XML namespace go together and with nothing "
          "else",
          a->prefix, strlen(a->prefix));
    }
    return GTEXT_XML_OK;
  }
  /* A declaration. */
  if (!a->prefix && strcmp(a->local, "xmlns") == 0) {
    if (is_lit(a->value, a->value_length, NS_XML)
        || is_lit(a->value, a->value_length, NS_XMLNS)) {
      return fail(w, GTEXT_XML_E_NAMESPACE,
          "the default namespace cannot be a reserved namespace");
    }
    return scope_bind(w, "", 0, a->value, a->value_length);
  }
  if (a->prefix && strcmp(a->prefix, "xmlns") == 0) {
    if (!ncname(a->local)) {
      return bad_name(w, a->local);
    }
    if (strcmp(a->local, "xmlns") == 0) {
      return fail_name(w, GTEXT_XML_E_NAMESPACE,
          "the prefix xmlns cannot be declared", a->local, 5);
    }
    if (a->value_length == 0 && !w->xml11) {
      return fail_name(w, GTEXT_XML_E_NAMESPACE,
          "a prefix cannot be undeclared (Namespaces in XML 1.0)", a->local,
          strlen(a->local));
    }
    if (strcmp(a->local, "xml") == 0) {
      if (!is_lit(a->value, a->value_length, NS_XML)) {
        return fail_name(w, GTEXT_XML_E_NAMESPACE,
            "the prefix xml can only be bound to the XML namespace", a->local,
            3);
      }
      return GTEXT_XML_OK;
    }
    if (is_lit(a->value, a->value_length, NS_XML)
        || is_lit(a->value, a->value_length, NS_XMLNS)) {
      return fail_name(w, GTEXT_XML_E_NAMESPACE,
          "a reserved namespace can only be bound to its own prefix", a->local,
          strlen(a->local));
    }
    return scope_bind(w, a->local, strlen(a->local), a->value, a->value_length);
  }
  return fail_name(w, GTEXT_XML_E_NAMESPACE,
      "an attribute in the xmlns namespace that is not a declaration", a->local,
      strlen(a->local));
}

static GTEXT_XML_Status check_element_name(wctx * w,
    const GTEXT_Markup_Node * e, size_t mark) {
  const char * ns = gtext_markup_element_namespace(e);
  const char * prefix = gtext_markup_element_prefix(e);
  const char * local = gtext_markup_element_local_name(e);
  if (!ncname(local)) {
    return bad_name(w, local);
  }
  if (prefix && !ncname(prefix)) {
    return bad_name(w, prefix);
  }
  if (prefix && !ns) {
    return fail_name(w, GTEXT_XML_E_NAMESPACE,
        "an element has a prefix and no namespace", prefix, strlen(prefix));
  }
  if (prefix && strcmp(prefix, "xmlns") == 0) {
    return fail_name(w, GTEXT_XML_E_NAMESPACE,
        "the prefix xmlns cannot be used on an element", prefix, 5);
  }
  if (ns && strcmp(ns, NS_XMLNS) == 0) {
    return fail(w, GTEXT_XML_E_NAMESPACE,
        "an element cannot be in the xmlns namespace");
  }
  if (ns && strcmp(ns, NS_XML) == 0) {
    if (!prefix || strcmp(prefix, "xml") != 0) {
      return fail(w, GTEXT_XML_E_NAMESPACE,
          "the XML namespace can only be used through the prefix xml");
    }
    return GTEXT_XML_OK;
  }
  if (prefix && strcmp(prefix, "xml") == 0) {
    return fail_name(w, GTEXT_XML_E_NAMESPACE,
        "the prefix xml can only mean the XML namespace", prefix, 3);
  }
  if (prefix) {
    return need_binding(w, prefix, strlen(prefix), ns, mark);
  }
  return need_binding(w, "", 0, ns, mark);
}

/* ---- the XML declaration and the document type -------------------------- */

static const char * encoding_label(GTEXT_XML_Output_Encoding enc) {
  switch (enc) {
    case GTEXT_XML_OUTPUT_UTF16LE:
    case GTEXT_XML_OUTPUT_UTF16BE:
      return "UTF-16";
    case GTEXT_XML_OUTPUT_ISO_8859_1:
      return "ISO-8859-1";
    case GTEXT_XML_OUTPUT_US_ASCII:
      return "US-ASCII";
    default:
      return "UTF-8";
  }
}

static GTEXT_XML_Status write_declaration(wctx * w) {
  GTEXT_Markup_Standalone standalone = GTEXT_MARKUP_STANDALONE_UNSPECIFIED;
  const char * version = gtext_markup_document_xml_version(w->doc, &standalone);
  w->at = NULL;
  /* Whatever the declaration mode: a document that says it is not XML 1.0 is
     not one this writer can write, and leaving the declaration out would
     hide that rather than fix it. */
  if (version && strcmp(version, "1.0") != 0 && strcmp(version, "1.1") != 0) {
    return fail_name(w, GTEXT_XML_E_UNSUPPORTED_VERSION,
        "only XML 1.0 and 1.1 can be written", version, strlen(version));
  }
  bool declare;
  switch (w->o.xml_declaration) {
    case GTEXT_XML_DECLARATION_ALWAYS:
      declare = true;
      break;
    case GTEXT_XML_DECLARATION_NEVER:
      declare = false;
      break;
    default:
      /* ISO-8859-1 output can hold bytes that are not UTF-8, which only the
         declaration tells a reader about. US-ASCII output is UTF-8. */
      declare = version != NULL
          || standalone != GTEXT_MARKUP_STANDALONE_UNSPECIFIED
          || w->o.encoding == GTEXT_XML_OUTPUT_ISO_8859_1;
      break;
  }
  w->declared = declare;
  if (!declare) {
    if (w->xml11) {
      return fail(w, GTEXT_XML_E_INVALID,
          "XML 1.1 output needs its version declaration so readers select the "
          "same XML profile");
    }
    return GTEXT_XML_OK;
  }
  if (put_str(w, w->xml11 ? "<?xml version=\"1.1\" encoding=\"" : "<?xml version=\"1.0\" encoding=\"") != GTEXT_XML_OK
      || put_str(w, encoding_label(w->o.encoding)) != GTEXT_XML_OK
      || put_str(w, "\"") != GTEXT_XML_OK) {
    return w->status;
  }
  if (standalone == GTEXT_MARKUP_STANDALONE_YES
      && put_str(w, " standalone=\"yes\"") != GTEXT_XML_OK) {
    return w->status;
  }
  if (standalone == GTEXT_MARKUP_STANDALONE_NO
      && put_str(w, " standalone=\"no\"") != GTEXT_XML_OK) {
    return w->status;
  }
  return put_str(w, "?>");
}

static bool pubid_char(unsigned char c) {
  return c == 0x20 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
      || (c >= '0' && c <= '9')
      || (c != 0 && strchr("-'()+,./:=?;!*#@$_%", c) != NULL);
}

/* A literal in quotes it can stand in. */
static GTEXT_XML_Status put_literal(
    wctx * w, const char * text, size_t len, bool pubid) {
  char quote = '"';
  if (!pubid) {
    if (memchr(text, '"', len)) {
      quote = '\'';
      if (memchr(text, '\'', len)) {
        return fail(w, GTEXT_XML_E_DOCTYPE,
            "a system identifier with both kinds of quote has no spelling");
      }
    }
  }
  if (put_ascii(w, &quote, 1) != GTEXT_XML_OK
      || put_text(w, text, len, false) != GTEXT_XML_OK
      || put_ascii(w, &quote, 1) != GTEXT_XML_OK) {
    return w->status;
  }
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status write_doctype(wctx * w, const GTEXT_Markup_Node * n) {
  w->at = n;
  w->at_attr = -1;
  const char * name = gtext_markup_doctype_name(n);
  const char * pub = gtext_markup_doctype_public_id(n);
  const char * sys = gtext_markup_doctype_system_id(n);
  if (!name || !gtext_markup_is_xml_name(name, strlen(name))) {
    return bad_name(w, name ? name : "");
  }
  if (pub) {
    const size_t pl = strlen(pub);
    for (size_t i = 0; i < pl; i++) {
      if (!pubid_char((unsigned char)pub[i])) {
        return fail(w, GTEXT_XML_E_DOCTYPE,
            "character not allowed in a public identifier");
      }
    }
    if ((pl > 0 && (pub[0] == ' ' || pub[pl - 1] == ' '))
        || strstr(pub, "  ")) {
      return fail(w, GTEXT_XML_E_DOCTYPE,
          "a public identifier is read back with its white space collapsed; "
          "this one is not");
    }
    if (!sys) {
      return fail(w, GTEXT_XML_E_DOCTYPE,
          "a public identifier needs a system identifier in a document type");
    }
  }
  if (sys) {
    const size_t sl = strlen(sys);
    if (w->xml11) {
      if (check_raw_data(w, sys, sl,
              "a line end or restricted character in a system identifier has "
              "no character-reference spelling") != GTEXT_XML_OK) return w->status;
    } else {
      const GTEXT_Markup_Status st = gtext_markup_check_xml_chars(sys, sl);
      if (st != GTEXT_MARKUP_OK) {
        return st == GTEXT_MARKUP_E_ENCODING
            ? fail(w, GTEXT_XML_E_UTF8, "invalid UTF-8")
            : fail(w, GTEXT_XML_E_CHARACTER, "character not allowed in XML");
      }
      if (memchr(sys, '\r', sl)) {
        return fail(w, GTEXT_XML_E_DOCTYPE,
            "a carriage return in a system identifier would be read back as a "
            "line feed");
      }
    }
  }
  if (put_str(w, "<!DOCTYPE ") != GTEXT_XML_OK
      || put_text(w, name, strlen(name), false) != GTEXT_XML_OK) {
    return w->status;
  }
  if (pub) {
    if (put_str(w, " PUBLIC ") != GTEXT_XML_OK
        || put_literal(w, pub, strlen(pub), true) != GTEXT_XML_OK
        || put_str(w, " ") != GTEXT_XML_OK) {
      return w->status;
    }
    if (put_literal(w, sys, strlen(sys), false) != GTEXT_XML_OK) {
      return w->status;
    }
  } else if (sys) {
    if (put_str(w, " SYSTEM ") != GTEXT_XML_OK
        || put_literal(w, sys, strlen(sys), false) != GTEXT_XML_OK) {
      return w->status;
    }
  }
  if (w->dry) {
    w->r->doctype_without_subset = true;
  }
  return put_str(w, ">");
}

/* ---- leaves ------------------------------------------------------------- */

/* Valid characters, no carriage return: for data that has no reference to hide
   a carriage return behind, and that XML would read back with a line feed. */
static GTEXT_XML_Status check_raw_data(
    wctx * w, const char * data, size_t len, const char * what) {
  if (w->xml11) {
    size_t i = 0;
    while (i < len) {
      uint32_t cp = 0; bool valid = false;
      size_t used = guni_utf8_decode(data + i, len - i, &cp, &valid);
      if (!valid || !used) return fail(w, GTEXT_XML_E_UTF8, "invalid UTF-8");
      if (!xml11_char(cp)) return fail(w, GTEXT_XML_E_CHARACTER, "character not allowed in XML 1.1");
      if (xml11_restricted(cp) || cp == 0x85 || cp == 0x2028 || cp == '\r')
        return fail(w, GTEXT_XML_E_UNREPRESENTABLE, what);
      i += used;
    }
    return GTEXT_XML_OK;
  }
  const GTEXT_Markup_Status st = gtext_markup_check_xml_chars(data, len);
  if (st == GTEXT_MARKUP_E_ENCODING) {
    return fail(w, GTEXT_XML_E_UTF8, "invalid UTF-8");
  }
  if (st != GTEXT_MARKUP_OK) {
    return fail(w, GTEXT_XML_E_CHARACTER, "character not allowed in XML");
  }
  if (len > 0 && memchr(data, '\r', len)) {
    return fail(w, GTEXT_XML_E_UNREPRESENTABLE, what);
  }
  return GTEXT_XML_OK;
}

static bool has_seq(const char * s, size_t n, const char * seq, size_t m) {
  for (size_t i = 0; i + m <= n; i++) {
    if (memcmp(s + i, seq, m) == 0) {
      return true;
    }
  }
  return false;
}

/* ---- XHTML-compatible output -------------------------------------------- */

/* What the HTML parser's tree construction does with an element of this name
   (XHTML namespace only). Sorted by name; a name not here has no category. */
enum {
  XC_BLOCK = 1u << 0,
  XC_P = 1u << 1,
  XC_A = 1u << 2,
  XC_FORM = 1u << 3,
  XC_LI = 1u << 4,
  XC_DTDD = 1u << 5,
  XC_OPTION = 1u << 6,
  XC_OPTGROUP = 1u << 7,
  XC_HEADING = 1u << 8,
  XC_TABLE = 1u << 9,
  XC_SECTION = 1u << 10,
  XC_ROW = 1u << 11,
  XC_CELL = 1u << 12,
  XC_COL = 1u << 13,
  XC_TABLE_CHILD = 1u << 14,
  XC_TABLE_OK = 1u << 15,
  XC_RAWISH = 1u << 16,
  XC_SPECIAL = 1u << 17,
  XC_CONT = 1u << 18,
  XC_HTML = 1u << 19,
  XC_HEAD = 1u << 20,
  XC_BODY = 1u << 21,
  XC_COLGROUP = 1u << 22,
  XC_CAPTION = 1u << 23,
  XC_BUTTON = 1u << 24,
  XC_SELECT = 1u << 25,
  XC_TEMPLATE = 1u << 26,
  XC_ODD = 1u << 27,
  XC_HEADCONTENT = 1u << 28,
  XC_NOBR = 1u << 29,
};

/* Ruby: a start tag of one of its parts, with a ruby open, ends an open dd, dt, li,
   optgroup, option, p or ruby part (rtc is not ended by rt and rp). */
#define XC_RUBY (1u << 30)
#define XC_RUBYPART (1u << 31)

typedef struct {
  const char * name;
  unsigned cats;
} xcat;

static const xcat k_xcats[] = {
    {"a", XC_A},
    {"address", XC_BLOCK | XC_SPECIAL | XC_CONT},
    {"applet", XC_SPECIAL},
    {"area", XC_SPECIAL},
    {"article", XC_BLOCK | XC_SPECIAL},
    {"aside", XC_BLOCK | XC_SPECIAL},
    {"base", XC_SPECIAL | XC_HEADCONTENT},
    {"basefont", XC_SPECIAL | XC_HEADCONTENT},
    {"bgsound", XC_SPECIAL | XC_HEADCONTENT},
    {"blockquote", XC_BLOCK | XC_SPECIAL},
    {"body", XC_SPECIAL | XC_BODY},
    {"br", XC_SPECIAL},
    {"button", XC_SPECIAL | XC_BUTTON},
    {"caption", XC_TABLE_CHILD | XC_SPECIAL | XC_CAPTION},
    {"center", XC_BLOCK | XC_SPECIAL},
    {"col", XC_COL | XC_SPECIAL},
    {"colgroup", XC_TABLE_CHILD | XC_SPECIAL | XC_COLGROUP},
    {"dd", XC_BLOCK | XC_DTDD | XC_SPECIAL},
    {"details", XC_BLOCK | XC_SPECIAL},
    {"dialog", XC_BLOCK},
    {"dir", XC_BLOCK | XC_SPECIAL},
    {"div", XC_BLOCK | XC_SPECIAL | XC_CONT},
    {"dl", XC_BLOCK | XC_SPECIAL},
    {"dt", XC_BLOCK | XC_DTDD | XC_SPECIAL},
    {"embed", XC_SPECIAL},
    {"fieldset", XC_BLOCK | XC_SPECIAL},
    {"figcaption", XC_BLOCK | XC_SPECIAL},
    {"figure", XC_BLOCK | XC_SPECIAL},
    {"footer", XC_BLOCK | XC_SPECIAL},
    {"form", XC_BLOCK | XC_FORM | XC_SPECIAL},
    {"frame", XC_SPECIAL | XC_ODD},
    {"frameset", XC_SPECIAL | XC_ODD},
    {"h1", XC_BLOCK | XC_HEADING | XC_SPECIAL},
    {"h2", XC_BLOCK | XC_HEADING | XC_SPECIAL},
    {"h3", XC_BLOCK | XC_HEADING | XC_SPECIAL},
    {"h4", XC_BLOCK | XC_HEADING | XC_SPECIAL},
    {"h5", XC_BLOCK | XC_HEADING | XC_SPECIAL},
    {"h6", XC_BLOCK | XC_HEADING | XC_SPECIAL},
    {"head", XC_SPECIAL | XC_HEAD},
    {"header", XC_BLOCK | XC_SPECIAL},
    {"hgroup", XC_BLOCK | XC_SPECIAL},
    {"hr", XC_BLOCK | XC_SPECIAL},
    {"html", XC_SPECIAL | XC_HTML},
    {"iframe", XC_RAWISH | XC_SPECIAL},
    {"image", XC_ODD},
    {"img", XC_SPECIAL},
    {"input", XC_SPECIAL},
    {"isindex", XC_ODD},
    {"keygen", XC_SPECIAL},
    {"li", XC_BLOCK | XC_LI | XC_SPECIAL},
    {"link", XC_SPECIAL | XC_HEADCONTENT},
    {"listing", XC_BLOCK | XC_SPECIAL},
    {"main", XC_BLOCK | XC_SPECIAL},
    {"marquee", XC_SPECIAL},
    {"math", XC_ODD},
    {"menu", XC_BLOCK | XC_SPECIAL},
    {"meta", XC_SPECIAL | XC_HEADCONTENT},
    {"nav", XC_BLOCK | XC_SPECIAL},
    {"nobr", XC_NOBR},
    {"noembed", XC_RAWISH | XC_SPECIAL},
    {"noframes", XC_RAWISH | XC_SPECIAL},
    {"noscript", XC_RAWISH | XC_SPECIAL | XC_HEADCONTENT},
    {"object", XC_SPECIAL},
    {"ol", XC_BLOCK | XC_SPECIAL},
    {"optgroup", XC_OPTGROUP},
    {"option", XC_OPTION},
    {"p", XC_BLOCK | XC_P | XC_SPECIAL | XC_CONT},
    {"param", XC_SPECIAL},
    /* Odd because it never ends: HTML reads everything after its start tag, the
       end tags the writer puts out included, as its text. */
    {"plaintext", XC_BLOCK | XC_RAWISH | XC_SPECIAL | XC_ODD},
    {"pre", XC_BLOCK | XC_SPECIAL},
    {"rb", XC_RUBYPART},
    {"rp", XC_RUBYPART},
    {"rt", XC_RUBYPART},
    {"rtc", XC_RUBYPART},
    {"ruby", XC_RUBY},
    {"script", XC_TABLE_OK | XC_SPECIAL | XC_HEADCONTENT},
    /* Special in the standard, which makes it end the chain an li, dt or dd is
       found through; html5lib 1.1 and parse5 8.0.1 predate that and read the
       chain through it, so it is not allowed to stop the chain here (XC_CONT):
       a hazard under either reading. */
    {"search", XC_BLOCK | XC_SPECIAL | XC_CONT},
    {"section", XC_BLOCK | XC_SPECIAL},
    {"select", XC_SPECIAL | XC_SELECT},
    {"source", XC_SPECIAL},
    {"style", XC_TABLE_OK | XC_SPECIAL | XC_HEADCONTENT},
    {"summary", XC_BLOCK | XC_SPECIAL},
    {"svg", XC_ODD},
    {"table", XC_BLOCK | XC_TABLE | XC_SPECIAL},
    {"tbody", XC_SECTION | XC_TABLE_CHILD | XC_SPECIAL},
    {"td", XC_CELL | XC_SPECIAL},
    {"template", XC_TABLE_OK | XC_SPECIAL | XC_TEMPLATE | XC_HEADCONTENT},
    {"textarea", XC_SPECIAL},
    {"tfoot", XC_SECTION | XC_TABLE_CHILD | XC_SPECIAL},
    {"th", XC_CELL | XC_SPECIAL},
    {"thead", XC_SECTION | XC_TABLE_CHILD | XC_SPECIAL},
    {"title", XC_SPECIAL | XC_HEADCONTENT},
    {"tr", XC_ROW | XC_SPECIAL},
    {"track", XC_SPECIAL},
    {"ul", XC_BLOCK | XC_SPECIAL},
    {"wbr", XC_SPECIAL},
    {"xmp", XC_BLOCK | XC_RAWISH | XC_SPECIAL},
};

static unsigned xcats_of(const GTEXT_Markup_Node * e) {
  if (!e || gtext_markup_node_kind(e) != GTEXT_MARKUP_KIND_ELEMENT) {
    return 0;
  }
  const char * ns = gtext_markup_element_namespace(e);
  if (!ns || strcmp(ns, GTEXT_MARKUP_NS_HTML) != 0) {
    return 0;
  }
  const char * local = gtext_markup_element_local_name(e);
  size_t lo = 0;
  size_t hi = sizeof(k_xcats) / sizeof(k_xcats[0]);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = strcmp(local, k_xcats[mid].name);
    if (c == 0) {
      return k_xcats[mid].cats;
    }
    if (c < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return 0;
}

static bool is_xhtml_element(const GTEXT_Markup_Node * e) {
  const char * ns = gtext_markup_element_namespace(e);
  return ns && strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0;
}

/* A refusal of xhtml mode: the tree is XML, but not one an HTML parser would
   read the same way. */
static GTEXT_XML_Status refuse_x(
    wctx * w, const GTEXT_Markup_Node * n, const char * why) {
  w->at = n;
  w->at_attr = -1;
  return fail(w, GTEXT_XML_E_XHTML, why);
}

/* A hazard: counted (once, in the checking pass), and a refusal when strict. */
static GTEXT_XML_Status hazard(wctx * w, const GTEXT_Markup_Node * n,
    GTEXT_XML_Xhtml_Hazard kind, const char * why) {
  if (w->dry) {
    if (w->r->xhtml_hazards++ == 0) {
      w->r->xhtml_hazard_kind = kind;
      build_path(n, -1, w->r->xhtml_hazard_path);
    }
  }
  if (w->o.xhtml_strict) {
    return refuse_x(w, n, why);
  }
  return GTEXT_XML_OK;
}

/* White space in the sense of the standard's "ASCII whitespace" and no carriage
   return. A carriage return is written as &#13;, and a character that comes from a
   reference is not white space to every HTML parser (parse5 8.0.1 treats it as
   any other character, which in a table, a colgroup or a head ends the mode the
   standard keeps), so where white space and other text differ it is not trusted. */
static bool html_space_plain(const char * d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (d[i] != ' ' && d[i] != '\t' && d[i] != '\n' && d[i] != '\f') {
      return false;
    }
  }
  return true;
}

static bool html_space_only(const char * d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (d[i] != ' ' && d[i] != '\t' && d[i] != '\n' && d[i] != '\r'
        && d[i] != '\f') {
      return false;
    }
  }
  return true;
}

/* Whether writing `data` would need a character reference in this encoding. */
static bool text_needs_reference(const wctx * w, const char * data, size_t len) {
  if (w->o.encoding != GTEXT_XML_OUTPUT_US_ASCII
      && w->o.encoding != GTEXT_XML_OUTPUT_ISO_8859_1) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)data[i];
    if (c >= 0xC4 || (c >= 0x80 && w->o.encoding == GTEXT_XML_OUTPUT_US_ASCII)) {
      return true; /* U+0100 and up, or anything beyond ASCII */
    }
  }
  return false;
}

static bool has_upper(const char * s) {
  for (; *s; s++) {
    if (*s >= 'A' && *s <= 'Z') {
      return true;
    }
  }
  return false;
}

static bool is_foreign_ns(const char * ns) {
  return ns
      && (strcmp(ns, GTEXT_MARKUP_NS_SVG) == 0
          || strcmp(ns, GTEXT_MARKUP_NS_MATHML) == 0);
}

/* The elements inside which HTML parses start tags as HTML again. */
static bool is_integration_point(const GTEXT_Markup_Node * e) {
  const char * ns = gtext_markup_element_namespace(e);
  const char * local = gtext_markup_element_local_name(e);
  if (!ns) {
    return false;
  }
  if (strcmp(ns, GTEXT_MARKUP_NS_SVG) == 0) {
    return strcmp(local, "foreignObject") == 0 || strcmp(local, "desc") == 0
        || strcmp(local, "title") == 0;
  }
  if (strcmp(ns, GTEXT_MARKUP_NS_MATHML) == 0) {
    if (strcmp(local, "annotation-xml") == 0) {
      GTEXT_Markup_Attribute enc;
      if (gtext_markup_element_get_attribute(e, NULL, "encoding", &enc)) {
        return (enc.value_length == 9 && strncasecmp(enc.value, "text/html", 9) == 0)
            || (enc.value_length == 21
                && strncasecmp(enc.value, "application/xhtml+xml", 21) == 0);
      }
      return false;
    }
    return strcmp(local, "mi") == 0 || strcmp(local, "mo") == 0
        || strcmp(local, "mn") == 0 || strcmp(local, "ms") == 0
        || strcmp(local, "mtext") == 0;
  }
  return false;
}

/* HTML start tags that end foreign content (the standard's list). */
static bool is_breakout(const GTEXT_Markup_Node * e) {
  static const char * const names[] = {"b", "big", "blockquote", "body", "br",
      "center", "code", "dd", "div", "dl", "dt", "em", "embed", "h1", "h2",
      "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing", "menu",
      "meta", "nobr", "ol", "p", "pre", "ruby", "s", "small", "span", "strong",
      "strike", "sub", "sup", "table", "tt", "u", "ul", "var"};
  const char * local = gtext_markup_element_local_name(e);
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strcmp(local, names[i]) == 0) {
      return true;
    }
  }
  if (strcmp(local, "font") == 0) {
    GTEXT_Markup_Attribute a;
    return gtext_markup_element_get_attribute(e, NULL, "color", &a)
        || gtext_markup_element_get_attribute(e, NULL, "face", &a)
        || gtext_markup_element_get_attribute(e, NULL, "size", &a);
  }
  return false;
}

/* A table part that is not in the kind of parent HTML accepts it in is ignored
   (the cases HTML repairs by inserting a part are the parent's to report). */
static bool table_part_misplaced(unsigned cats, unsigned pc) {
  if (pc & XC_TEMPLATE) {
    return false;
  }
  if (cats & XC_ROW) {
    return !(pc & (XC_SECTION | XC_TABLE | XC_ROW | XC_CELL));
  }
  if (cats & XC_CELL) {
    return !(pc & (XC_ROW | XC_SECTION | XC_TABLE | XC_CELL));
  }
  if (cats & XC_SECTION) {
    return !(pc & (XC_TABLE | XC_ROW | XC_CELL | XC_SECTION));
  }
  if (cats & (XC_CAPTION | XC_COLGROUP)) {
    return !(pc & XC_TABLE);
  }
  if (cats & XC_COL) {
    return !(pc & (XC_COLGROUP | XC_TABLE));
  }
  return false;
}

#define HAZ(w, node, kind, why) \
  do { \
    if (hazard((w), (node), (kind), (why)) != GTEXT_XML_OK) { \
      return (w)->status; \
    } \
  } while (0)

/* Attributes of an XHTML element: names HTML would fold, and namespaces it
   does not have. */
static GTEXT_XML_Status xhtml_check_attributes(
    wctx * w, const GTEXT_Markup_Node * e, bool is_root) {
  const size_t count = gtext_markup_element_attribute_count(e);
  for (size_t i = 0; i < count; i++) {
    GTEXT_Markup_Attribute a;
    gtext_markup_element_attribute_at(e, i, &a);
    if (has_upper(a.local)) {
      return refuse_x(w, e,
          "an HTML parser lower-cases attribute names, so one with upper case "
          "letters is read as a different attribute");
    }
    if (!a.ns) {
      continue;
    }
    if (strcmp(a.ns, NS_XMLNS) == 0) {
      const bool same_default = !a.prefix
          && a.value_length == strlen(GTEXT_MARKUP_NS_HTML)
          && memcmp(a.value, GTEXT_MARKUP_NS_HTML, a.value_length) == 0;
      if (!is_root && !same_default) {
        HAZ(w, e, GTEXT_XML_XHTML_HAZARD_NAMESPACED_ATTRIBUTE,
            "an HTML parser reads a namespace declaration here as an "
            "attribute and keeps the namespace of the enclosing element");
      }
      continue;
    }
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_NAMESPACED_ATTRIBUTE,
        "an HTML parser reads an attribute with a namespace as a plain "
        "attribute with its prefixed name");
  }
  return GTEXT_XML_OK;
}

/* The attributes of an element in SVG or MathML namespace. HTML lower-cases an
   attribute name and restores the case of the ones in its tables; it reads
   `xlink:href` and the other six xlink names, and `xml:lang` and `xml:space`, as
   attributes in those namespaces, and every other prefixed name as a plain
   attribute with a colon in it. The prefix has to be the literal one: the
   standard's tables match the text of the name. */
static GTEXT_XML_Status xhtml_check_foreign_attributes(
    wctx * w, const GTEXT_Markup_Node * e, const char * ns) {
  static const char * const xlink_names[] = {"actuate", "arcrole", "href", "role",
      "show", "title", "type"};
  const size_t count = gtext_markup_element_attribute_count(e);
  for (size_t i = 0; i < count; i++) {
    GTEXT_Markup_Attribute a;
    gtext_markup_element_attribute_at(e, i, &a);
    if (!a.ns) {
      if (!gtext_markup_html_foreign_name_survives(ns, true, a.local)) {
        HAZ(w, e, GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT,
            "an HTML parser lower-cases an attribute name here and does not "
            "restore this one");
      }
      /* (Two names that differ only in case are one attribute to HTML, which keeps the first. They cannot
         both pass the test above: one of them has an upper case letter that is not in a table, or is the
         folded form of one that is.) */
      continue;
    }
    if (strcmp(a.ns, NS_XMLNS) == 0) {
      continue; /* a declaration: kept as an attribute of that name, which is the known difference */
    }
    bool known = false;
    if (strcmp(a.ns, GTEXT_MARKUP_NS_XLINK) == 0 && a.prefix
        && strcmp(a.prefix, "xlink") == 0) {
      for (size_t k = 0; k < sizeof(xlink_names) / sizeof(xlink_names[0]); k++) {
        known |= strcmp(a.local, xlink_names[k]) == 0;
      }
    }
    else if (strcmp(a.ns, NS_XML) == 0 && a.prefix && strcmp(a.prefix, "xml") == 0) {
      known = strcmp(a.local, "lang") == 0 || strcmp(a.local, "space") == 0;
    }
    if (!known) {
      HAZ(w, e, GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT,
          "an HTML parser reads this namespaced attribute as a plain one with "
          "a colon in its name");
    }
  }
  return GTEXT_XML_OK;
}

/* Element in SVG or MathML namespace, or in none of the known ones. */
static GTEXT_XML_Status xhtml_check_foreign(wctx * w,
    const GTEXT_Markup_Node * e, unsigned in_hz, unsigned * out_hz) {
  const char * ns = gtext_markup_element_namespace(e);
  const char * local = gtext_markup_element_local_name(e);
  const GTEXT_Markup_Node * parent = gtext_markup_node_parent(e);
  bool legit = false;
  if (!is_foreign_ns(ns)) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT,
        "an HTML parser only makes elements in the HTML, SVG and MathML "
        "namespaces");
  }
  else {
    legit = true;
    if (!gtext_markup_html_foreign_name_survives(ns, false, local)) {
      HAZ(w, e, GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT,
          "an HTML parser lower-cases an element name here and does not "
          "restore this one");
    }
    if (xhtml_check_foreign_attributes(w, e, ns) != GTEXT_XML_OK) {
      return w->status;
    }
    const bool is_root_name = (strcmp(ns, GTEXT_MARKUP_NS_SVG) == 0
                                  && strcmp(local, "svg") == 0)
        || (strcmp(ns, GTEXT_MARKUP_NS_MATHML) == 0 && strcmp(local, "math") == 0);
    if (gtext_markup_element_prefix(e)) {
      legit = false;
      HAZ(w, e, GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT,
          "an HTML parser reads a prefixed name as a name with a colon in it");
    }
    if (in_hz & HZ_FOREIGN) {
      const char * pns = gtext_markup_element_namespace(parent);
      if (!pns || strcmp(pns, ns) != 0) {
        legit = false;
        HAZ(w, e, GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT,
            "an HTML parser gives an element the namespace of its foreign "
            "parent, not its own");
      }
      else if (is_breakout(e)) {
        legit = false;
        HAZ(w, e, GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT,
            "this start tag ends foreign content in an HTML parser");
      }
    }
    else if (!is_root_name) {
      legit = false;
      HAZ(w, e, GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT,
          "an HTML parser makes SVG and MathML elements only inside an svg or "
          "math element");
    }
  }
  *out_hz = (in_hz & ~(unsigned)HZ_FOREIGN)
      | ((legit && !is_integration_point(e)) ? HZ_FOREIGN : 0u);
  return GTEXT_XML_OK;
}

/* What an HTML parser would do to this element and what it holds directly.
   `hz` is what the open elements above it are; it becomes what they are for the
   children. The element is in the XHTML namespace when `xe`. */
static GTEXT_XML_Status xhtml_enter(wctx * w, const GTEXT_Markup_Node * e,
    bool xe, bool has_content, unsigned * hz) {
  const GTEXT_Markup_Node * parent = gtext_markup_node_parent(e);
  const bool is_root = gtext_markup_node_kind(parent) == GTEXT_MARKUP_KIND_DOCUMENT;
  const char * local = gtext_markup_element_local_name(e);
  if (!xe) {
    if (*hz & HZ_SELECT) {
      HAZ(w, e, GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT,
          "an HTML parser ignores most elements inside a select");
    }
    if (is_root) {
      HAZ(w, e, GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE,
          "an HTML parser makes the root of a document an html element");
    }
    return xhtml_check_foreign(w, e, *hz, hz);
  }

  const unsigned fl = gtext_markup_html_node_flags(e);
  const unsigned cats = xcats_of(e);
  const unsigned pc = xcats_of(parent);
  const unsigned in = *hz;
  if (gtext_markup_element_prefix(e)) {
    return refuse_x(w, e,
        "an HTML parser reads a prefixed name as a different element, so an "
        "element in the XHTML namespace cannot have a prefix");
  }
  if (has_upper(local)) {
    return refuse_x(w, e,
        "an HTML parser lower-cases element names, so one with upper case "
        "letters is read as a different element");
  }
  if (!((local[0] >= 'a' && local[0] <= 'z'))) {
    return refuse_x(w, e,
        "an HTML parser only starts an element name with an ASCII letter");
  }
  if ((fl & GTEXT_MARKUP_HTML_VOID) && has_content) {
    return refuse_x(w, e, "a void element cannot have children");
  }
  if (xhtml_check_attributes(w, e, is_root) != GTEXT_XML_OK) {
    return w->status;
  }

  /* Where it stands. */
  const size_t hazards_before = w->r->xhtml_hazards;
  if (is_root && !(cats & XC_HTML)) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE,
        "an HTML parser makes the root of a document an html element");
  }
  if (!is_root && (cats & XC_HTML)) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE,
        "an HTML parser merges a nested html element into the first");
  }
  if ((cats & (XC_HEAD | XC_BODY)) && !(pc & XC_HTML)) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE,
        "an HTML parser only has a head or body directly in html");
  }
  if ((cats & XC_A) && (in & HZ_A)) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_NESTED_A,
        "an a inside an a: an HTML parser ends the first one");
  }
  if ((cats & XC_FORM) && (in & HZ_FORM)) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_NESTED_FORM,
        "a form inside a form: an HTML parser ignores the inner one");
  }
  if ((cats & XC_BLOCK) && (in & HZ_P)) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_P_BLOCK,
        "a block-level element inside a p: an HTML parser ends the p");
  }
  const bool implied_end = ((cats & XC_LI) && (in & HZ_LI))
      || ((cats & XC_DTDD) && (in & HZ_DD))
      || ((cats & XC_OPTION) && (pc & XC_OPTION))
      || ((cats & XC_OPTGROUP) && (pc & (XC_OPTION | XC_OPTGROUP)))
      || ((cats & XC_HEADING) && (pc & XC_HEADING))
      || ((cats & XC_ROW) && (pc & (XC_ROW | XC_CELL)))
      || ((cats & XC_CELL) && (pc & XC_CELL))
      || ((cats & XC_SECTION) && (pc & (XC_ROW | XC_CELL | XC_SECTION)));
  if (implied_end) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_IMPLIED_END,
        "an HTML parser ends an open element where this one starts");
  }
  if ((cats & XC_ODD)) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT,
        "an HTML parser rewrites or specially treats this element");
  }
  if ((cats & XC_RUBYPART) && (in & HZ_RUBY)) {
    /* A start tag of a ruby part generates implied end tags first: an open dd,
       dt, li, optgroup, option, p, rb, rp, rt or rtc is ended (an rtc is not,
       by rt and rp). */
    const bool part_parent = (pc & XC_RUBYPART) != 0;
    const char * pl = gtext_markup_element_local_name(parent);
    const bool parent_is_rtc = part_parent && strcmp(pl, "rtc") == 0;
    const bool ends_on_rt = strcmp(local, "rt") == 0 || strcmp(local, "rp") == 0;
    const bool ended = (pc & (XC_DTDD | XC_LI | XC_OPTGROUP | XC_OPTION | XC_P))
        || (part_parent && !(ends_on_rt && parent_is_rtc));
    if (ended) {
      HAZ(w, e, GTEXT_XML_XHTML_HAZARD_IMPLIED_END,
          "an HTML parser ends the open element where a ruby part starts");
    }
  }
  if (in & HZ_SELECT) {
    /* HTML's "in select" mode ignores every start tag but these, wherever it
       is below the select (an element inside an option is dropped and its
       text kept). */
    const bool allowed = (cats & XC_TEMPLATE) != 0 || strcmp(local, "script") == 0
        || ((pc & XC_SELECT)
            && ((cats & (XC_OPTION | XC_OPTGROUP)) != 0 || strcmp(local, "hr") == 0))
        || ((pc & XC_OPTGROUP) && (cats & XC_OPTION));
    /* One hazard to an element: when another rule above found this one already, the
       select adds nothing to the count. */
    if (!allowed && w->r->xhtml_hazards == hazards_before) {
      HAZ(w, e, GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT,
          "an HTML parser ignores most elements inside a select");
    }
  }
  if ((cats & XC_NOBR) && (in & HZ_NOBR)) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_IMPLIED_END,
        "a nobr inside a nobr: an HTML parser ends the first one");
  }
  if ((cats & XC_BUTTON) && (in & HZ_BUTTON)) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT,
        "a button inside a button: an HTML parser ends the first one");
  }
  if (table_part_misplaced(cats, pc)) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT,
        "an HTML parser ignores a table part outside a table");
  }
  if (in & HZ_FOREIGN) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT,
        "an HTML parser makes an HTML tag inside SVG or MathML content a "
        "foreign element or ends the content");
  }
  if ((cats & XC_TEMPLATE) && has_content) {
    HAZ(w, e, GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT,
        "an HTML parser puts the children of a template into a content "
        "fragment");
  }

  /* What it holds. */
  const bool in_table = (cats & (XC_TABLE | XC_SECTION | XC_ROW)) != 0;
  const bool newline_first = (fl & GTEXT_MARKUP_HTML_DROPS_FIRST_NEWLINE) != 0;
  const bool raw = (fl & (GTEXT_MARKUP_HTML_RAW_TEXT
                         | GTEXT_MARKUP_HTML_ESCAPABLE_RAW_TEXT))
      != 0;
  const bool scan = has_content
      && (in_table || newline_first || raw
          || (cats & (XC_RAWISH | XC_HEAD | XC_COLGROUP)));
  bool seen_head = false;
  bool seen_body = false;
  if (scan) {
    bool first = true;
    for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(e); c;
        c = gtext_markup_node_next_sibling(c)) {
      const GTEXT_Markup_Kind k = gtext_markup_node_kind(c);
      size_t len = 0;
      const char * data = NULL;
      if (k == GTEXT_MARKUP_KIND_TEXT) {
        data = gtext_markup_node_data(c, &len);
        if (len == 0) {
          continue; /* XML reads it back as nothing */
        }
      }
      if (first && newline_first && data && data[0] == '\n') {
        return refuse_x(w, c,
            "an HTML parser drops a line feed that starts a pre, textarea or "
            "listing, and an XML parser keeps it");
      }
      first = false;
      if (raw && k != GTEXT_MARKUP_KIND_TEXT) {
        HAZ(w, c, GTEXT_XML_XHTML_HAZARD_RAW_TEXT_CONTENT,
            "an HTML parser reads markup inside this element as text");
      }
      if ((cats & XC_RAWISH) && k != GTEXT_MARKUP_KIND_TEXT) {
        HAZ(w, c, GTEXT_XML_XHTML_HAZARD_RAW_TEXT_CONTENT,
            "an HTML parser reads markup inside this element as text");
      }
      if ((cats & XC_RAWISH) && data
          && (memchr(data, '<', len) || memchr(data, '&', len)
              || memchr(data, '>', len) || memchr(data, '\r', len)
              || text_needs_reference(w, data, len))) {
        HAZ(w, c, GTEXT_XML_XHTML_HAZARD_RAW_TEXT_CONTENT,
            "an HTML parser reads this text without its escapes");
      }
      if ((cats & XC_RAWISH) && (pc & XC_HEAD) && strcmp(local, "noscript") == 0 && data
          && !html_space_plain(data, len)) {
        /* With scripting off HTML parses a noscript in the head, and text that
           is not white space ends the head. */
        HAZ(w, c, GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE,
            "an HTML parser ends the head at text inside a noscript in it");
      }
      if ((cats & XC_COLGROUP) && data && !html_space_plain(data, len)) {
        HAZ(w, c, GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT,
            "an HTML parser ends a colgroup at text that is not white space");
      }
      if (cats & XC_HEAD) {
        if (data ? !html_space_plain(data, len)
                 : (k == GTEXT_MARKUP_KIND_ELEMENT
                       && !(xcats_of(c) & XC_HEADCONTENT))) {
          HAZ(w, c, GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE,
              "an HTML parser ends the head at this");
        }
      }
      if ((cats & XC_COLGROUP) && k == GTEXT_MARKUP_KIND_ELEMENT
          && !(xcats_of(c) & (XC_COL | XC_TEMPLATE))) {
        HAZ(w, c, GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT,
            "an HTML parser ends a colgroup at anything but a col");
      }
      if (!in_table) {
        continue;
      }
      if (data) {
        if (!html_space_only(data, len)) {
          HAZ(w, c, GTEXT_XML_XHTML_HAZARD_TABLE_FOSTER_PARENT,
              "an HTML parser moves text in a table to before the table");
        }
        else if (memchr(data, '\r', len)) {
          /* A carriage return is written as &#13;, and a character from a
             reference is not white space to every HTML parser (parse5 8.0.1
             moves it out of the table; the standard keeps it in). */
          HAZ(w, c, GTEXT_XML_XHTML_HAZARD_TABLE_FOSTER_PARENT,
              "an HTML parser may take a carriage return written as a "
              "reference for text that is not white space, and move it out of "
              "the table");
        }
        continue;
      }
      if (k != GTEXT_MARKUP_KIND_ELEMENT) {
        continue;
      }
      const unsigned cc = xcats_of(c);
      if (cc & XC_TABLE_OK) {
        continue;
      }
      bool implied = false;
      bool foster = false;
      if (cats & XC_TABLE) {
        if (cc & (XC_ROW | XC_CELL | XC_COL)) {
          implied = true;
        }
        else if (!(cc & XC_TABLE_CHILD)) {
          foster = true;
        }
      }
      else if (cats & XC_SECTION) {
        if (cc & XC_CELL) {
          implied = true;
        }
        else if (!(cc & (XC_ROW | XC_SECTION))) {
          foster = true;
        }
      }
      else if (!(cc & (XC_CELL | XC_ROW | XC_SECTION))) {
        foster = true;
      }
      if (implied) {
        HAZ(w, c, GTEXT_XML_XHTML_HAZARD_TABLE_IMPLIED_PART,
            "an HTML parser puts a tbody, tr or colgroup around this");
      }
      if (foster) {
        HAZ(w, c, GTEXT_XML_XHTML_HAZARD_TABLE_FOSTER_PARENT,
            "an HTML parser moves this out of the table");
      }
    }
  }
  if (cats & XC_HTML) {
    /* Exactly a head and then a body, nothing else but comments. */
    for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(e); c;
        c = gtext_markup_node_next_sibling(c)) {
      const GTEXT_Markup_Kind k = gtext_markup_node_kind(c);
      if (k == GTEXT_MARKUP_KIND_COMMENT) {
        continue;
      }
      if (k == GTEXT_MARKUP_KIND_TEXT) {
        size_t len = 0;
        gtext_markup_node_data(c, &len);
        if (len == 0) {
          continue;
        }
        HAZ(w, c, GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE,
            "an HTML parser drops or moves text directly in html");
        continue;
      }
      const unsigned cc = xcats_of(c);
      if (k == GTEXT_MARKUP_KIND_ELEMENT && (cc & XC_HEAD) && !seen_head
          && !seen_body) {
        seen_head = true;
      }
      else if (k == GTEXT_MARKUP_KIND_ELEMENT && (cc & XC_BODY) && !seen_body
          && seen_head) {
        seen_body = true;
      }
      else {
        HAZ(w, c, GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE,
            "an HTML parser makes html hold one head and then one body");
      }
    }
    if (!seen_head || !seen_body) {
      HAZ(w, e, GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE,
          "an HTML parser inserts the head and body that html lacks");
    }
  }

  /* What the children inherit. */
  unsigned out = in & ~(unsigned)HZ_FOREIGN;
  if (cats & XC_A) {
    out |= HZ_A;
  }
  if (cats & XC_FORM) {
    out |= HZ_FORM;
  }
  if (cats & XC_P) {
    out |= HZ_P;
  }
  if (cats & XC_BUTTON) {
    out |= HZ_BUTTON;
  }
  if (cats & XC_NOBR) {
    out |= HZ_NOBR;
  }
  if (cats & XC_SELECT) {
    out |= HZ_SELECT;
  }
  if (cats & XC_RUBY) {
    out |= HZ_RUBY;
  }
  if (cats & XC_TEMPLATE) {
    out &= ~(unsigned)HZ_SELECT;
  }
  const bool stops = (cats & XC_SPECIAL) && !(cats & XC_CONT);
  if (cats & XC_LI) {
    out |= HZ_LI;
  }
  else if (stops) {
    out &= ~(unsigned)HZ_LI;
  }
  if (cats & XC_DTDD) {
    out |= HZ_DD;
  }
  else if (stops) {
    out &= ~(unsigned)HZ_DD;
  }
  *hz = out;
  return GTEXT_XML_OK;
}

/* The text of a script or style, which no escape can reach in HTML. */
static GTEXT_XML_Status xhtml_raw_text(
    wctx * w, const GTEXT_Markup_Node * n, const char * data, size_t len) {
  const GTEXT_Markup_Status st = gtext_markup_check_xml_chars(data, len);
  if (st == GTEXT_MARKUP_E_ENCODING) {
    return fail(w, GTEXT_XML_E_UTF8, "invalid UTF-8");
  }
  if (st != GTEXT_MARKUP_OK) {
    return fail(w, GTEXT_XML_E_CHARACTER, "character not allowed in XML");
  }
  if (len > 0 && (memchr(data, '<', len) || memchr(data, '&', len)
                     || memchr(data, '\r', len)
                     || has_seq(data, len, "]]>", 3))) {
    return refuse_x(w, n,
        "script and style content with <, &, ]]> or a carriage return is read "
        "differently by an HTML parser and an XML parser");
  }
  return put_text(w, data, len, false);
}

static GTEXT_XML_Status write_leaf(wctx * w, const GTEXT_Markup_Node * n) {
  w->at = n;
  w->at_attr = -1;
  size_t len = 0;
  const char * data = gtext_markup_node_data(n, &len);
  if (!data) {
    data = "";
  }
  switch (gtext_markup_node_kind(n)) {
    case GTEXT_MARKUP_KIND_TEXT:
      if (w->xhtml) {
        const GTEXT_Markup_Node * parent = gtext_markup_node_parent(n);
        if (parent && gtext_markup_html_node_is_raw_text(parent)) {
          return xhtml_raw_text(w, n, data, len);
        }
      }
      return put_escaped(w, GTEXT_MARKUP_ESCAPE_XML_TEXT, data, len);
    case GTEXT_MARKUP_KIND_CDATA:
      if (w->xhtml
          && !(w->nframes > 0 && (w->frames[w->nframes - 1].hz & HZ_FOREIGN))) {
        return refuse_x(w, n,
            "an HTML parser reads a CDATA section outside SVG and MathML "
            "content as a comment");
      }
      if (check_raw_data(w, data, len,
              "a carriage return in a CDATA section would be read back as a "
              "line feed")
          != GTEXT_XML_OK) {
        return w->status;
      }
      if (has_seq(data, len, "]]>", 3)) {
        return fail(w, GTEXT_XML_E_UNREPRESENTABLE,
            "a CDATA section cannot contain ]]>");
      }
      if (put_str(w, "<![CDATA[") != GTEXT_XML_OK
          || put_text(w, data, len, false) != GTEXT_XML_OK) {
        return w->status;
      }
      return put_str(w, "]]>");
    case GTEXT_MARKUP_KIND_COMMENT:
      if (check_raw_data(w, data, len,
              "a carriage return in a comment would be read back as a line "
              "feed")
          != GTEXT_XML_OK) {
        return w->status;
      }
      if (has_seq(data, len, "--", 2) || (len > 0 && data[len - 1] == '-')) {
        return fail(w, GTEXT_XML_E_UNREPRESENTABLE,
            "a comment cannot contain -- or end in -");
      }
      /* The HTML parser also ends a comment at <!--> and <!--->, and a comment
         that ends in <!- is covered by the rule above (it ends in -). */
      if (w->xhtml && len > 0
          && (data[0] == '>' || (len > 1 && data[0] == '-' && data[1] == '>'))) {
        return refuse_x(w, n,
            "an HTML parser ends a comment that starts with > or ->");
      }
      if (put_str(w, "<!--") != GTEXT_XML_OK
          || put_text(w, data, len, false) != GTEXT_XML_OK) {
        return w->status;
      }
      return put_str(w, "-->");
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      if (w->xhtml) {
        return refuse_x(w, n,
            "an HTML parser reads a processing instruction as a comment");
      }
      const char * target = gtext_markup_pi_target(n);
      if (!target || !ncname(target)) {
        return bad_name(w, target ? target : "");
      }
      if (strlen(target) == 3 && (target[0] | 0x20) == 'x'
          && (target[1] | 0x20) == 'm' && (target[2] | 0x20) == 'l') {
        return fail_name(w, GTEXT_XML_E_NAME,
            "the processing instruction target xml is reserved", target, 3);
      }
      if (check_raw_data(w, data, len,
              "a carriage return in a processing instruction would be read "
              "back as a line feed")
          != GTEXT_XML_OK) {
        return w->status;
      }
      if (has_seq(data, len, "?>", 2)) {
        return fail(w, GTEXT_XML_E_UNREPRESENTABLE,
            "processing instruction data cannot contain ?>");
      }
      if (len > 0 && (data[0] == ' ' || data[0] == '\t' || data[0] == '\n')) {
        return fail(w, GTEXT_XML_E_UNREPRESENTABLE,
            "processing instruction data that starts with white space would "
            "be read back without it");
      }
      if (put_str(w, "<?") != GTEXT_XML_OK
          || put_text(w, target, strlen(target), false) != GTEXT_XML_OK) {
        return w->status;
      }
      if (len > 0
          && (put_ascii(w, " ", 1) != GTEXT_XML_OK
              || put_text(w, data, len, false) != GTEXT_XML_OK)) {
        return w->status;
      }
      return put_str(w, "?>");
    }
    default:
      return fail(w, GTEXT_XML_E_UNREPRESENTABLE,
          "this kind of node has no place in an XML element");
  }
}

/* ---- elements ----------------------------------------------------------- */

static GTEXT_XML_Status push_frame(
    wctx * w, size_t mark, unsigned char flags, unsigned hz) {
  if (w->nframes == w->framecap) {
    const size_t cap = w->framecap ? w->framecap * 2 : 32;
    if (cap > SIZE_MAX / sizeof(wframe)) {
      return oom(w);
    }
    wframe * frames =
        gtext_allocator_realloc(w->alloc, w->frames, cap * sizeof(wframe));
    if (!frames) {
      return oom(w);
    }
    w->frames = frames;
    w->framecap = cap;
  }
  w->frames[w->nframes].mark = mark;
  w->frames[w->nframes].flags = flags;
  w->frames[w->nframes].hz = hz;
  w->nframes++;
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status open_element(
    wctx * w, const GTEXT_Markup_Node * e, bool * descend) {
  w->at = e;
  w->at_attr = -1;
  *descend = false;
  if (gtext_markup_element_template_content(e)) {
    return fail(w, GTEXT_XML_E_UNREPRESENTABLE,
        "template content is not a child and has no XML spelling");
  }
  if (w->nframes >= w->o.max_depth) {
    return fail(w, GTEXT_XML_E_LIMIT_DEPTH, "nesting too deep");
  }

  /* What is inside, which decides how it is laid out. */
  bool has_text = false;
  bool has_content = false;
  bool run = false;
  for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(e); c;
      c = gtext_markup_node_next_sibling(c)) {
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(c);
    if (k == GTEXT_MARKUP_KIND_TEXT) {
      size_t len = 0;
      gtext_markup_node_data(c, &len);
      if (len == 0) {
        COUNT(w, text_nodes_coalesced);
        continue;
      }
      has_text = true;
      has_content = true;
      if (run) {
        COUNT(w, text_nodes_coalesced);
      }
      run = true;
    } else {
      run = false;
      has_content = true;
      if (k == GTEXT_MARKUP_KIND_CDATA) {
        has_text = true;
      }
    }
  }
  const unsigned char inherited =
      w->nframes ? w->frames[w->nframes - 1].flags : 0;
  GTEXT_Markup_Attribute space;
  const bool preserve_attr =
      gtext_markup_element_get_attribute(e, NS_XML, "space", &space)
      && is_lit(space.value, space.value_length, "preserve");
  unsigned char flags = inherited & (F_NOINDENT | F_PRESERVE);
  if (preserve_attr) {
    flags |= F_PRESERVE | F_NOINDENT;
  }

  /* XHTML-compatible output: what an HTML parser would do to this element. */
  const bool xe = w->xhtml && is_xhtml_element(e);
  unsigned hz = w->nframes ? w->frames[w->nframes - 1].hz : 0;
  if (w->xhtml) {
    if (xhtml_enter(w, e, xe, has_content, &hz) != GTEXT_XML_OK) {
      return w->status;
    }
    if (gtext_markup_html_node_flags(e)
        & (GTEXT_MARKUP_HTML_RAW_TEXT | GTEXT_MARKUP_HTML_ESCAPABLE_RAW_TEXT
            | GTEXT_MARKUP_HTML_DROPS_FIRST_NEWLINE)) {
      /* White space in these is content in HTML. */
      flags |= F_PRESERVE | F_NOINDENT;
    }
  }
  if (has_text) {
    flags |= F_NOINDENT;
  }
  /* Its own children, and not what is below them: an HTML parser drops white space
     before the head, puts it in the html element after the head, and puts it in the
     body after the body, so indenting the html element's children is a different tree
     to HTML and a plain one to XML. */
  const bool own_children_plain = xe && (xcats_of(e) & XC_HTML) != 0;
  if (w->pretty && has_content) {
    if (flags & F_PRESERVE) {
      COUNT(w, elements_skipped_preserve);
    } else if ((flags & F_NOINDENT) || own_children_plain) {
      COUNT(w, elements_skipped_mixed);
    } else {
      COUNT(w, elements_indented);
    }
  }
  if (w->pretty && has_content && !(flags & F_NOINDENT) && !own_children_plain) {
    flags |= F_INDENT;
  }

  /* Names, namespaces. */
  const size_t mark = w->nbind;
  w->nadds = 0;
  w->nuses = 0;
  const size_t count = gtext_markup_element_attribute_count(e);
  for (size_t i = 0; i < count; i++) {
    GTEXT_Markup_Attribute a;
    gtext_markup_element_attribute_at(e, i, &a);
    w->at_attr = (int)i;
    if (check_attribute_shape(w, &a) != GTEXT_XML_OK) {
      return w->status;
    }
  }
  w->at_attr = -1;
  /* An XHTML root element is useless to an HTML parser and to a reader of the
     file without its namespace: declare it, as declare_namespaces would. */
  w->declare_now = xe && w->nframes == 0 && !gtext_markup_element_prefix(e);
  const GTEXT_XML_Status named = check_element_name(w, e, mark);
  w->declare_now = false;
  if (named != GTEXT_XML_OK) {
    return w->status;
  }
  for (size_t i = 0; i < count; i++) {
    GTEXT_Markup_Attribute a;
    gtext_markup_element_attribute_at(e, i, &a);
    if (!a.ns || strcmp(a.ns, NS_XMLNS) == 0 || strcmp(a.ns, NS_XML) == 0) {
      continue;
    }
    w->at_attr = (int)i;
    if (need_binding(w, a.prefix, strlen(a.prefix), a.ns, mark)
        != GTEXT_XML_OK) {
      return w->status;
    }
  }
  w->at_attr = -1;

  /* The start tag. */
  if (put_str(w, "<") != GTEXT_XML_OK
      || put_qname(w, gtext_markup_element_prefix(e),
             gtext_markup_element_local_name(e)) != GTEXT_XML_OK) {
    return w->status;
  }
  for (size_t i = 0; i < count; i++) {
    GTEXT_Markup_Attribute a;
    gtext_markup_element_attribute_at(e, i, &a);
    w->at_attr = (int)i;
    if (put_str(w, " ") != GTEXT_XML_OK
        || put_qname(w, a.prefix, a.local) != GTEXT_XML_OK
        || put_str(w, "=\"") != GTEXT_XML_OK
        || put_escaped(w, GTEXT_MARKUP_ESCAPE_XML_ATTRIBUTE, a.value,
               a.value_length) != GTEXT_XML_OK
        || put_str(w, "\"") != GTEXT_XML_OK) {
      return w->status;
    }
  }
  w->at_attr = -1;
  for (size_t i = 0; i < w->nadds; i++) {
    const wadd * a = &w->adds[i];
    if (put_str(w, a->klen ? " xmlns:" : " xmlns") != GTEXT_XML_OK
        || put_text(w, a->key, a->klen, false) != GTEXT_XML_OK
        || put_str(w, "=\"") != GTEXT_XML_OK
        || put_escaped(w, GTEXT_MARKUP_ESCAPE_XML_ATTRIBUTE,
               a->uri ? a->uri : "", a->ulen) != GTEXT_XML_OK
        || put_str(w, "\"") != GTEXT_XML_OK) {
      return w->status;
    }
  }
  if (!has_content) {
    if (xe && gtext_markup_html_node_is_void(e)) {
      if (put_str(w, " />") != GTEXT_XML_OK) {
        return w->status;
      }
    } else if (xe || w->o.expand_empty_elements) {
      if (put_str(w, "></") != GTEXT_XML_OK
          || put_qname(w, gtext_markup_element_prefix(e),
                 gtext_markup_element_local_name(e)) != GTEXT_XML_OK
          || put_str(w, ">") != GTEXT_XML_OK) {
        return w->status;
      }
    } else if (put_str(w, "/>") != GTEXT_XML_OK) {
      return w->status;
    }
    scope_release(w, mark);
    return GTEXT_XML_OK;
  }
  if (put_str(w, ">") != GTEXT_XML_OK) {
    return w->status;
  }
  *descend = true;
  return push_frame(w, mark, flags, hz);
}

static GTEXT_XML_Status close_element(wctx * w, const GTEXT_Markup_Node * e) {
  w->at = e;
  w->at_attr = -1;
  const wframe f = w->frames[w->nframes - 1];
  if ((f.flags & F_INDENT) && newline_indent(w, w->nframes - 1) != GTEXT_XML_OK) {
    return w->status;
  }
  if (put_str(w, "</") != GTEXT_XML_OK
      || put_qname(w, gtext_markup_element_prefix(e),
             gtext_markup_element_local_name(e)) != GTEXT_XML_OK
      || put_str(w, ">") != GTEXT_XML_OK) {
    return w->status;
  }
  scope_release(w, f.mark);
  w->nframes--;
  return GTEXT_XML_OK;
}

/* Write the element `root` and everything under it, without recursion. */
static GTEXT_XML_Status write_element_tree(
    wctx * w, const GTEXT_Markup_Node * root) {
  const GTEXT_Markup_Node * n = root;
  for (;;) {
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(n);
    bool skip = false;
    if (k == GTEXT_MARKUP_KIND_TEXT) {
      size_t len = 0;
      gtext_markup_node_data(n, &len);
      skip = len == 0;
    }
    if (!skip) {
      if (n != root && (w->frames[w->nframes - 1].flags & F_INDENT)) {
        w->at = n;
        if (newline_indent(w, w->nframes) != GTEXT_XML_OK) {
          return w->status;
        }
      }
      if (k == GTEXT_MARKUP_KIND_ELEMENT) {
        bool descend = false;
        if (open_element(w, n, &descend) != GTEXT_XML_OK) {
          return w->status;
        }
        if (descend) {
          n = gtext_markup_node_first_child(n);
          continue;
        }
      } else if (write_leaf(w, n) != GTEXT_XML_OK) {
        return w->status;
      }
    }
    for (;;) {
      if (n == root) {
        return GTEXT_XML_OK;
      }
      const GTEXT_Markup_Node * next = gtext_markup_node_next_sibling(n);
      if (next) {
        n = next;
        break;
      }
      n = gtext_markup_node_parent(n);
      if (close_element(w, n) != GTEXT_XML_OK) {
        return w->status;
      }
    }
  }
}

/* ---- the document ------------------------------------------------------- */

static GTEXT_XML_Status run_pass(wctx * w) {
  w->status = GTEXT_XML_OK;
  w->declared = false;
  w->total = 0;
  w->used = 0;
  w->nframes = 0;
  scope_reset(w);
  w->at = NULL;
  w->at_attr = -1;

  const GTEXT_Markup_Node * top = gtext_markup_document_root(w->doc);
  if (!gtext_markup_document_element(w->doc)) {
    return fail(w, GTEXT_XML_E_UNREPRESENTABLE,
        "the document has no root element");
  }
  if (w->bom) {
    static const unsigned char mark[3] = {0xEF, 0xBB, 0xBF};
    if (out_raw(w, mark, 3) != GTEXT_XML_OK) {
      return w->status;
    }
  }
  if (is_utf16(w)) {
    static const unsigned char le[2] = {0xFF, 0xFE};
    static const unsigned char be[2] = {0xFE, 0xFF};
    if (out_raw(w, w->o.encoding == GTEXT_XML_OUTPUT_UTF16LE ? le : be, 2)
        != GTEXT_XML_OK) {
      return w->status;
    }
  }
  const size_t before = w->total;
  if (write_declaration(w) != GTEXT_XML_OK) {
    return w->status;
  }
  bool any = w->total != before;
  bool seen_element = false;
  bool seen_doctype = false;
  for (const GTEXT_Markup_Node * n = gtext_markup_node_first_child(top); n;
      n = gtext_markup_node_next_sibling(n)) {
    /* After the root element an HTML parser reads white space as text in the
       body, so a separator written there would be a difference in the tree. */
    if (any && !(w->xhtml && seen_element)
        && put_ascii(w, w->newline, w->newline_len) != GTEXT_XML_OK) {
      return w->status;
    }
    any = true;
    switch (gtext_markup_node_kind(n)) {
      case GTEXT_MARKUP_KIND_DOCTYPE:
        if (seen_element || seen_doctype) {
          w->at = n;
          return fail(w, GTEXT_XML_E_DOCTYPE,
              "a document type must come once, before the root element");
        }
        seen_doctype = true;
        if (w->xhtml) {
          const char * dn = gtext_markup_doctype_name(n);
          if (!dn || strcmp(dn, "html") != 0 || gtext_markup_doctype_public_id(n)
              || gtext_markup_doctype_system_id(n)) {
            w->at = n;
            if (hazard(w, n, GTEXT_XML_XHTML_HAZARD_DOCTYPE,
                    "an HTML parser derives quirks mode from the document "
                    "type and keeps its identifiers; only <!DOCTYPE html> "
                    "is read back as written")
                != GTEXT_XML_OK) {
              return w->status;
            }
          }
        }
        if (write_doctype(w, n) != GTEXT_XML_OK) {
          return w->status;
        }
        break;
      case GTEXT_MARKUP_KIND_ELEMENT:
        if (seen_element) {
          w->at = n;
          return fail(w, GTEXT_XML_E_UNREPRESENTABLE,
              "a document has one root element");
        }
        seen_element = true;
        if (write_element_tree(w, n) != GTEXT_XML_OK) {
          return w->status;
        }
        break;
      case GTEXT_MARKUP_KIND_COMMENT:
      case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
        if (write_leaf(w, n) != GTEXT_XML_OK) {
          return w->status;
        }
        break;
      default:
        /* The tree does not let a document hold anything else; a writer that
           trusted that and met it would write something that is not XML. */
        w->at = n;
        return fail(w, GTEXT_XML_E_UNREPRESENTABLE,
            "this kind of node has no place directly in a document");
    }
  }
  w->at = NULL;
  if (w->pretty && !(w->xhtml && seen_element)
      && put_ascii(w, w->newline, w->newline_len) != GTEXT_XML_OK) {
    return w->status;
  }
  return w->dry ? GTEXT_XML_OK : flush(w);
}

/* ---- entry points ------------------------------------------------------- */

/* "UTF-8" in any case, which is how the parser records what a mark and a
   declaration agreed on. */
static bool label_is_utf8(const char * label) {
  static const char want[] = "utf-8";
  for (size_t i = 0; i < sizeof(want); i++) {
    char c = label[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c + 32);
    }
    if (c != want[i]) {
      return false;
    }
  }
  return true;
}

static GTEXT_XML_Status refuse(GTEXT_XML_Write_Result * r, const char * why) {
  r->code = GTEXT_XML_E_INVALID;
  r->message = why;
  return GTEXT_XML_E_INVALID;
}

static bool only_of(const char * s, const char * allowed) {
  for (; *s; s++) {
    if (!strchr(allowed, *s)) {
      return false;
    }
  }
  return true;
}

static GTEXT_XML_Status write_impl(const GTEXT_Markup_Sink * sink,
    const GTEXT_Markup_Document * doc, const GTEXT_XML_Write_Options * opts,
    GTEXT_XML_Write_Result * result, bool check_only) {
  GTEXT_XML_Write_Result local;
  GTEXT_XML_Write_Result * r = result ? result : &local;
  memset(r, 0, sizeof(*r));
  if (!doc || (!check_only && (!sink || !sink->write))) {
    return refuse(r, "sink and document must not be NULL");
  }
  GTEXT_XML_Write_Options o =
      opts ? *opts : gtext_xml_write_options_default();
  if ((int)o.encoding < GTEXT_XML_OUTPUT_UTF8
      || o.encoding > GTEXT_XML_OUTPUT_US_ASCII) {
    return refuse(r, "unknown output encoding");
  }
  if ((int)o.xml_declaration < GTEXT_XML_DECLARATION_AUTO
      || o.xml_declaration > GTEXT_XML_DECLARATION_NEVER) {
    return refuse(r, "unknown XML declaration mode");
  }
  if ((int)o.byte_order_mark < GTEXT_XML_BOM_AUTO
      || o.byte_order_mark > GTEXT_XML_BOM_NEVER) {
    return refuse(r, "unknown byte order mark mode");
  }
  if (o.byte_order_mark == GTEXT_XML_BOM_ALWAYS
      && (o.encoding == GTEXT_XML_OUTPUT_ISO_8859_1
          || o.encoding == GTEXT_XML_OUTPUT_US_ASCII)) {
    return refuse(r, "ISO-8859-1 and US-ASCII have no byte order mark");
  }
  if (o.indent && !only_of(o.indent, " \t")) {
    return refuse(r, "the indent must be spaces and tabs");
  }
  if (o.newline && (!*o.newline || !only_of(o.newline, "\r\n"))) {
    return refuse(r, "the newline must be line feeds and carriage returns");
  }
  if (o.xhtml_strict && !o.xhtml) {
    return refuse(r, "xhtml_strict needs xhtml");
  }
  if (o.max_depth == 0) {
    o.max_depth = XML_WRITE_DEFAULT_MAX_DEPTH;
  }
  if (o.max_output_bytes == 0) {
    o.max_output_bytes = XML_WRITE_DEFAULT_MAX_OUTPUT;
  }
  const GTEXT_Allocator * alloc =
      o.allocator ? o.allocator : &doc->allocator;
  wctx * w = gtext_allocator_calloc(alloc, 1, sizeof(*w));
  if (!w) {
    r->code = GTEXT_XML_E_OOM;
    r->message = "out of memory";
    return GTEXT_XML_E_OOM;
  }
  w->alloc = alloc;
  w->doc = doc;
  const char * doc_version = gtext_markup_document_xml_version(doc, NULL);
  w->xml11 = doc_version && strcmp(doc_version, "1.1") == 0;
  if (sink) {
    w->sink = *sink;
  }
  w->o = o;
  w->xhtml = o.xhtml;
  w->pretty = o.indent != NULL;
  w->indent = o.indent ? o.indent : "";
  w->indent_len = strlen(w->indent);
  w->newline = o.newline ? o.newline : "\n";
  w->newline_len = strlen(w->newline);
  w->r = r;
  w->seed = (size_t)(uintptr_t)w ^ (size_t)(uintptr_t)&local;
  w->at_attr = -1;

  if (o.encoding == GTEXT_XML_OUTPUT_UTF8) {
    GTEXT_Markup_Encoding_Source source = GTEXT_MARKUP_ENCODING_UNKNOWN;
    const char * label = gtext_markup_document_encoding(doc, &source);
    w->bom = o.byte_order_mark == GTEXT_XML_BOM_ALWAYS
        || (o.byte_order_mark == GTEXT_XML_BOM_AUTO
            && source == GTEXT_MARKUP_ENCODING_BOM && label
            && label_is_utf8(label));
  }
  w->dry = true;
  GTEXT_XML_Status status = run_pass(w);
  if (status == GTEXT_XML_OK && !check_only) {
    w->dry = false;
    status = run_pass(w);
  }
  r->bytes_written = w->sent;

  if (status != GTEXT_XML_OK && r->code == GTEXT_XML_OK) {
    r->code = status;
  }
  gtext_allocator_free(alloc, w->frames);
  gtext_allocator_free(alloc, w->table);
  gtext_allocator_free(alloc, w->bind);
  gtext_allocator_free(alloc, w->adds);
  gtext_allocator_free(alloc, w->uses);
  gtext_allocator_free(alloc, w);
  return status;
}

GTEXT_XML_Status gtext_xml_write(const GTEXT_Markup_Sink * sink,
    const GTEXT_Markup_Document * doc, const GTEXT_XML_Write_Options * opts,
    GTEXT_XML_Write_Result * result) {
  return write_impl(sink, doc, opts, result, false);
}

GTEXT_XML_Status xml_write_check(const GTEXT_Markup_Document * doc,
    const GTEXT_XML_Write_Options * opts, GTEXT_XML_Write_Result * result) {
  return write_impl(NULL, doc, opts, result, true);
}

/* ---- to a buffer -------------------------------------------------------- */

typedef struct {
  const GTEXT_Allocator * alloc;
  char * data;
  size_t length;
  size_t capacity;
} wbuf;

static GTEXT_Markup_Status wbuf_write(
    void * user, const char * bytes, size_t len) {
  wbuf * b = user;
  if (len > SIZE_MAX - b->length - 1) {
    return GTEXT_MARKUP_E_OOM;
  }
  if (b->length + len + 1 > b->capacity) {
    size_t cap = b->capacity ? b->capacity : 256;
    while (cap < b->length + len + 1) {
      if (cap > SIZE_MAX / 2) {
        return GTEXT_MARKUP_E_OOM;
      }
      cap *= 2;
    }
    char * grown = gtext_allocator_realloc(b->alloc, b->data, cap);
    if (!grown) {
      return GTEXT_MARKUP_E_OOM;
    }
    b->data = grown;
    b->capacity = cap;
  }
  memcpy(b->data + b->length, bytes, len);
  b->length += len;
  b->data[b->length] = '\0';
  return GTEXT_MARKUP_OK;
}

GTEXT_XML_Status gtext_xml_write_buffer(const GTEXT_Markup_Document * doc,
    const GTEXT_XML_Write_Options * opts, char ** out, size_t * out_len,
    GTEXT_XML_Write_Result * result) {
  GTEXT_XML_Write_Result local;
  GTEXT_XML_Write_Result * r = result ? result : &local;
  if (!doc || !out) {
    memset(r, 0, sizeof(*r));
    return refuse(r, "document and out must not be NULL");
  }
  wbuf b = {opts && opts->allocator ? opts->allocator : &doc->allocator, NULL, 0, 0};
  const GTEXT_Markup_Sink sink = {wbuf_write, &b};
  const GTEXT_XML_Status status = gtext_xml_write(&sink, doc, opts, r);
  if (status != GTEXT_XML_OK) {
    gtext_allocator_free(b.alloc, b.data);
    return status;
  }
  *out = b.data;
  if (out_len) {
    *out_len = b.length;
  }
  return GTEXT_XML_OK;
}

void gtext_xml_write_buffer_free(const GTEXT_Markup_Document * doc,
    const GTEXT_XML_Write_Options * opts, char * buffer) {
  const GTEXT_Allocator * alloc = opts && opts->allocator
      ? opts->allocator : (doc ? &doc->allocator : NULL);
  if (!buffer || !alloc) {
    return;
  }
  gtext_allocator_free(alloc, buffer);
}
