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
 * Copying, comparing and describing markup trees.
 *
 * All three walk the tree with the extended order (a template's content before
 * its children) and none recurses, so a tree of any depth is safe.
 */

#include <ghoti.io/text/macros.h>

#include <stdint.h>
#include <string.h>

#include "markup_internal.h"

typedef struct GTEXT_Markup_Node node_t;

/* ---- cloning ------------------------------------------------------------ */

static const char * opt_text(const markup_atom * a) {
  return a ? a->text : NULL;
}

/* A copy of one node, with its attributes and its location and none of its
   children, detached in `dst`. */
static GTEXT_Markup_Status clone_one(
    GTEXT_Markup_Document * dst, const node_t * src, node_t ** out) {
  node_t * copy = NULL;
  GTEXT_Markup_Status status;
  switch ((GTEXT_Markup_Kind)src->kind) {
    case GTEXT_MARKUP_KIND_ELEMENT: {
      const markup_element * e = (const markup_element *)src;
      status = gtext_markup_create_element(dst, opt_text(e->ns),
          opt_text(e->prefix), e->local->text, &copy);
      if (status != GTEXT_MARKUP_OK) {
        return status;
      }
      for (size_t i = 0; i < e->attr_count; i++) {
        const markup_attr * a = &e->attrs[i];
        status = gtext_markup_element_set_attribute(copy, opt_text(a->ns),
            opt_text(a->prefix), a->local->text, a->value, a->value_length);
        if (status != GTEXT_MARKUP_OK) {
          gtext_markup_node_free(copy);
          return status;
        }
        if (a->has_location) {
          GTEXT_Markup_Location where = {a->offset, a->line, a->column};
          gtext_markup_element_set_attribute_location(copy, i, &where);
        }
      }
      break;
    }
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
    case GTEXT_MARKUP_KIND_COMMENT: {
      const markup_text * t = (const markup_text *)src;
      status = src->kind == GTEXT_MARKUP_KIND_TEXT
          ? gtext_markup_create_text(dst, t->data, t->length, &copy)
          : src->kind == GTEXT_MARKUP_KIND_CDATA
          ? gtext_markup_create_cdata(dst, t->data, t->length, &copy)
          : gtext_markup_create_comment(dst, t->data, t->length, &copy);
      break;
    }
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      const markup_pi * p = (const markup_pi *)src;
      status = gtext_markup_create_processing_instruction(
          dst, p->target->text, p->data, p->length, &copy);
      break;
    }
    case GTEXT_MARKUP_KIND_DOCTYPE: {
      const markup_doctype * d = (const markup_doctype *)src;
      status = gtext_markup_create_doctype(
          dst, d->name, d->public_id, d->system_id, &copy);
      for (size_t i = 0; status == GTEXT_MARKUP_OK && i < d->syntax_count; i++) {
        const markup_dtd_syntax * item = &d->syntax[i];
        GTEXT_Markup_Location where = {item->offset, item->line, item->column};
        status = markup_doctype_append_syntax(copy, item->kind, item->source,
            item->length, &where);
      }
      if (status != GTEXT_MARKUP_OK && copy) gtext_markup_node_free(copy);
      break;
    }
    case GTEXT_MARKUP_KIND_FRAGMENT:
      status = gtext_markup_create_fragment(dst, &copy);
      break;
    case GTEXT_MARKUP_KIND_DOCUMENT:
    case GTEXT_MARKUP_KIND_NONE:
    default:
      return GTEXT_MARKUP_E_INVALID;
  }
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  if (src->flags & MARKUP_F_LOCATION) {
    GTEXT_Markup_Location where = {src->offset, src->line, src->column};
    gtext_markup_node_set_location(copy, &where);
  }
  *out = copy;
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_clone(GTEXT_Markup_Document * doc,
    const GTEXT_Markup_Node * node, bool deep, GTEXT_Markup_Node ** out) {
  if (!doc || !node || !out || node->kind == GTEXT_MARKUP_KIND_DOCUMENT) {
    return GTEXT_MARKUP_E_INVALID;
  }
  node_t * copy = NULL;
  GTEXT_Markup_Status status = clone_one(doc, node, &copy);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  if (deep) {
    /* Walk the source and build the copy beside it. The copy has the source's
       shape at every step, so the copy's raw parent is always the copy of the
       source's raw parent and climbing out of one climbs out of the other. A
       failure anywhere frees the copy as a whole: everything made so far hangs
       from `copy`, which is still on the detached list. */
    const node_t * s = node;
    node_t * d = copy;
    for (;;) {
      node_t * sc = markup_ext_first(s);
      if (sc) {
        node_t * dc = NULL;
        status = clone_one(doc, sc, &dc);
        if (status != GTEXT_MARKUP_OK) {
          goto fail;
        }
        if (sc->flags & MARKUP_F_CONTENT) {
          markup_set_content_unchecked(d, dc);
        }
        else {
          markup_append_unchecked(d, dc);
        }
        s = sc;
        d = dc;
        continue;
      }
      for (;;) {
        if (s == node) {
          *out = copy;
          return GTEXT_MARKUP_OK;
        }
        node_t * sn = markup_ext_next(s);
        if (sn) {
          node_t * dc = NULL;
          status = clone_one(doc, sn, &dc);
          if (status != GTEXT_MARKUP_OK) {
            goto fail;
          }
          markup_append_unchecked(d->parent, dc);
          s = sn;
          d = dc;
          break;
        }
        s = s->parent;
        d = d->parent;
      }
    }
  }
  *out = copy;
  return GTEXT_MARKUP_OK;

fail:
  markup_discard_detached(doc, copy);
  return status;
}

/* ---- comparing ---------------------------------------------------------- */

static bool same_text(const char * a, const char * b) {
  if (!a || !b) {
    return a == b;
  }
  return strcmp(a, b) == 0;
}

static bool same_bytes(const char * a, size_t alen, const char * b, size_t blen) {
  return alen == blen && (alen == 0 || memcmp(a, b, alen) == 0);
}

static bool node_equal(const node_t * a, const node_t * b) {
  if (a->kind != b->kind) {
    return false;
  }
  switch ((GTEXT_Markup_Kind)a->kind) {
    case GTEXT_MARKUP_KIND_ELEMENT: {
      const markup_element * x = (const markup_element *)a;
      const markup_element * y = (const markup_element *)b;
      if (!same_text(opt_text(x->ns), opt_text(y->ns))
          || !same_text(opt_text(x->prefix), opt_text(y->prefix))
          || !same_text(x->local->text, y->local->text)
          || x->attr_count != y->attr_count) {
        return false;
      }
      /* As a set: the DOM says attribute order is not part of equality. Names
         are unique within an element, so a count match and every attribute of
         one being in the other is equality. */
      for (size_t i = 0; i < x->attr_count; i++) {
        const markup_attr * p = &x->attrs[i];
        bool found = false;
        for (size_t j = 0; j < y->attr_count && !found; j++) {
          const markup_attr * q = &y->attrs[j];
          found = same_text(opt_text(p->ns), opt_text(q->ns))
              && same_text(p->local->text, q->local->text)
              && same_text(opt_text(p->prefix), opt_text(q->prefix))
              && same_bytes(p->value, p->value_length, q->value, q->value_length);
        }
        if (!found) {
          return false;
        }
      }
      return true;
    }
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
    case GTEXT_MARKUP_KIND_COMMENT: {
      const markup_text * x = (const markup_text *)a;
      const markup_text * y = (const markup_text *)b;
      return same_bytes(x->data, x->length, y->data, y->length);
    }
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      const markup_pi * x = (const markup_pi *)a;
      const markup_pi * y = (const markup_pi *)b;
      return same_text(x->target->text, y->target->text)
          && same_bytes(x->data, x->length, y->data, y->length);
    }
    case GTEXT_MARKUP_KIND_DOCTYPE: {
      const markup_doctype * x = (const markup_doctype *)a;
      const markup_doctype * y = (const markup_doctype *)b;
      if (!same_text(x->name, y->name) || !same_text(x->public_id, y->public_id)
          || !same_text(x->system_id, y->system_id)
          || x->syntax_count != y->syntax_count) return false;
      for (size_t i = 0; i < x->syntax_count; i++) {
        const markup_dtd_syntax * p = &x->syntax[i];
        const markup_dtd_syntax * q = &y->syntax[i];
        if (p->kind != q->kind || !same_bytes(p->source, p->length,
              q->source, q->length)) return false;
      }
      return true;
    }
    case GTEXT_MARKUP_KIND_DOCUMENT:
    case GTEXT_MARKUP_KIND_FRAGMENT:
    case GTEXT_MARKUP_KIND_NONE:
      break;
  }
  return true;
}

bool gtext_markup_equal(const GTEXT_Markup_Node * a, const GTEXT_Markup_Node * b) {
  if (!a || !b) {
    return a == b;
  }
  if (!node_equal(a, b)) {
    return false;
  }
  const node_t * x = a;
  const node_t * y = b;
  for (;;) {
    const node_t * cx = markup_ext_first(x);
    const node_t * cy = markup_ext_first(y);
    if ((cx == NULL) != (cy == NULL)) {
      return false;
    }
    if (cx) {
      /* A template's content and a real child are different things even when
         they hold the same bytes. */
      if (((cx->flags ^ cy->flags) & MARKUP_F_CONTENT) || !node_equal(cx, cy)) {
        return false;
      }
      x = cx;
      y = cy;
      continue;
    }
    for (;;) {
      if (x == a) {
        return true;
      }
      const node_t * nx = markup_ext_next(x);
      const node_t * ny = markup_ext_next(y);
      if ((nx == NULL) != (ny == NULL)) {
        return false;
      }
      if (nx) {
        if (((nx->flags ^ ny->flags) & MARKUP_F_CONTENT) || !node_equal(nx, ny)) {
          return false;
        }
        x = nx;
        y = ny;
        break;
      }
      x = x->parent;
      y = y->parent;
    }
  }
}

/* ---- describing --------------------------------------------------------- */

static void put_quoted(FILE * file, const char * bytes, size_t len) {
  fputc('"', file);
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)bytes[i];
    switch (c) {
      case '\\':
        fputs("\\\\", file);
        break;
      case '"':
        fputs("\\\"", file);
        break;
      case '\n':
        fputs("\\n", file);
        break;
      case '\t':
        fputs("\\t", file);
        break;
      case '\r':
        fputs("\\r", file);
        break;
      default:
        if (c < 0x20 || c == 0x7F) {
          fprintf(file, "\\x%02X", c);
        }
        else {
          fputc(c, file);
        }
    }
  }
  fputc('"', file);
}

static void put_name(FILE * file, const markup_atom * ns,
    const markup_atom * prefix, const markup_atom * local) {
  if (ns) {
    fprintf(file, "{%s}", ns->text);
  }
  if (prefix) {
    fprintf(file, "%s:", prefix->text);
  }
  fputs(local->text, file);
}

static void put_indent(FILE * file, size_t depth) {
  for (size_t i = 0; i < depth; i++) {
    fputs("  ", file);
  }
}

static void put_optional(FILE * file, const char * text) {
  if (text) {
    put_quoted(file, text, strlen(text));
  }
  else {
    fputc('-', file);
  }
}

static void dump_one(FILE * file, const node_t * n, size_t depth) {
  put_indent(file, depth);
  switch ((GTEXT_Markup_Kind)n->kind) {
    case GTEXT_MARKUP_KIND_DOCUMENT:
      fputs("#document\n", file);
      break;
    case GTEXT_MARKUP_KIND_FRAGMENT:
      fputs((n->flags & MARKUP_F_CONTENT) ? "#template-content\n" : "#fragment\n",
          file);
      break;
    case GTEXT_MARKUP_KIND_DOCTYPE: {
      const markup_doctype * d = (const markup_doctype *)n;
      fputs("<!DOCTYPE ", file);
      put_quoted(file, d->name, strlen(d->name));
      fputc(' ', file);
      put_optional(file, d->public_id);
      fputc(' ', file);
      put_optional(file, d->system_id);
      fputs(">\n", file);
      break;
    }
    case GTEXT_MARKUP_KIND_ELEMENT: {
      const markup_element * e = (const markup_element *)n;
      fputc('<', file);
      put_name(file, e->ns, e->prefix, e->local);
      fputs(">\n", file);
      for (size_t i = 0; i < e->attr_count; i++) {
        put_indent(file, depth + 1);
        fputc('@', file);
        put_name(file, e->attrs[i].ns, e->attrs[i].prefix, e->attrs[i].local);
        fputc('=', file);
        put_quoted(file, e->attrs[i].value, e->attrs[i].value_length);
        fputc('\n', file);
      }
      break;
    }
    case GTEXT_MARKUP_KIND_TEXT: {
      const markup_text * t = (const markup_text *)n;
      put_quoted(file, t->data, t->length);
      fputc('\n', file);
      break;
    }
    case GTEXT_MARKUP_KIND_CDATA: {
      const markup_text * t = (const markup_text *)n;
      fputs("<![CDATA[", file);
      put_quoted(file, t->data, t->length);
      fputs("]]>\n", file);
      break;
    }
    case GTEXT_MARKUP_KIND_COMMENT: {
      const markup_text * t = (const markup_text *)n;
      fputs("<!--", file);
      put_quoted(file, t->data, t->length);
      fputs("-->\n", file);
      break;
    }
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      const markup_pi * p = (const markup_pi *)n;
      fprintf(file, "<?%s ", p->target->text);
      put_quoted(file, p->data, p->length);
      fputs("?>\n", file);
      break;
    }
    case GTEXT_MARKUP_KIND_NONE:
      fputs("#none\n", file);
      break;
  }
}

void gtext_markup_dump(const GTEXT_Markup_Node * node, FILE * file) {
  if (!node || !file) {
    return;
  }
  const node_t * cur = node;
  size_t depth = 0;
  dump_one(file, cur, depth);
  for (;;) {
    const node_t * c = markup_ext_first(cur);
    if (c) {
      cur = c;
      depth++;
      dump_one(file, cur, depth);
      continue;
    }
    for (;;) {
      if (cur == node) {
        return;
      }
      const node_t * n = markup_ext_next(cur);
      if (n) {
        cur = n;
        dump_one(file, cur, depth);
        break;
      }
      cur = cur->parent;
      depth--;
    }
  }
}
