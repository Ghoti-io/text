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

#include <ghoti.io/unicode/utf.h>
#include <stdint.h>
#include <string.h>

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

typedef struct {
  size_t mark;         /* bindings in scope outside this element */
  unsigned char flags; /* F_* */
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
  bool declared; /* the XML declaration is being written in this pass */
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

static GTEXT_XML_Status note_used(wctx * w, const char * k, size_t n) {
  if (!w->o.declare_namespaces) {
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
  if (!w->o.declare_namespaces) {
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
    if (a->value_length == 0) {
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
  if (version && strcmp(version, "1.0") != 0) {
    return fail_name(w, GTEXT_XML_E_UNSUPPORTED_VERSION,
        "only XML 1.0 can be written", version, strlen(version));
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
    return GTEXT_XML_OK;
  }
  if (put_str(w, "<?xml version=\"1.0\" encoding=\"") != GTEXT_XML_OK
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
      return put_escaped(w, GTEXT_MARKUP_ESCAPE_XML_TEXT, data, len);
    case GTEXT_MARKUP_KIND_CDATA:
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
      if (put_str(w, "<!--") != GTEXT_XML_OK
          || put_text(w, data, len, false) != GTEXT_XML_OK) {
        return w->status;
      }
      return put_str(w, "-->");
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
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

static GTEXT_XML_Status push_frame(wctx * w, size_t mark, unsigned char flags) {
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
  if (has_text) {
    flags |= F_NOINDENT;
  }
  if (w->pretty && has_content) {
    if (flags & F_PRESERVE) {
      COUNT(w, elements_skipped_preserve);
    } else if (flags & F_NOINDENT) {
      COUNT(w, elements_skipped_mixed);
    } else {
      COUNT(w, elements_indented);
    }
  }
  if (w->pretty && has_content && !(flags & F_NOINDENT)) {
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
  if (check_element_name(w, e, mark) != GTEXT_XML_OK) {
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
    if (w->o.expand_empty_elements) {
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
  return push_frame(w, mark, flags);
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
    if (any && put_ascii(w, w->newline, w->newline_len) != GTEXT_XML_OK) {
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
  if (w->pretty && put_ascii(w, w->newline, w->newline_len) != GTEXT_XML_OK) {
    return w->status;
  }
  return w->dry ? GTEXT_XML_OK : flush(w);
}

/* ---- entry points ------------------------------------------------------- */

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
  if (o.indent && !only_of(o.indent, " \t")) {
    return refuse(r, "the indent must be spaces and tabs");
  }
  if (o.newline && (!*o.newline || !only_of(o.newline, "\r\n"))) {
    return refuse(r, "the newline must be line feeds and carriage returns");
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
  if (sink) {
    w->sink = *sink;
  }
  w->o = o;
  w->pretty = o.indent != NULL;
  w->indent = o.indent ? o.indent : "";
  w->indent_len = strlen(w->indent);
  w->newline = o.newline ? o.newline : "\n";
  w->newline_len = strlen(w->newline);
  w->r = r;
  w->seed = (size_t)(uintptr_t)w ^ (size_t)(uintptr_t)&local;
  w->at_attr = -1;

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
