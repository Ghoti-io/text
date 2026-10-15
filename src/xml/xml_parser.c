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
 * The XML parser: the prolog, elements, content, and the entry point.
 *
 * XML 1.0 (Fifth Edition) [1] document, [22] prolog, [23] XMLDecl, [27] Misc,
 * [39] element, [40] STag, [41] Attribute, [42] ETag, [43] content, [14]
 * CharData, [15] Comment, [16] PI, [18] CDSect, [10] AttValue; and Namespaces
 * in XML 1.0 for the names.
 *
 * Normalization is done as the bytes are read: CR LF and CR become LF wherever
 * the text is kept, and a tab or line feed written literally in an attribute
 * value becomes a space (a character reference to one does not).
 */

#include <ghoti.io/text/macros.h>

#include <stdlib.h>
#include <string.h>

#include "xml_internal.h"

/* ---- small helpers ------------------------------------------------------ */

static void buf_reset(markup_buf * buf) {
  buf->length = 0;
  if (buf->data) {
    buf->data[0] = '\0';
  }
}

void xml_attach(GTEXT_Markup_Node * parent, GTEXT_Markup_Node * node,
    const GTEXT_Markup_Location * where) {
  gtext_markup_node_set_location(node, where);
  markup_append_unchecked(parent, node);
}

static GTEXT_Markup_Node * current_parent(const xml_ctx * p) {
  return p->frame_count > 0 ? p->frames[p->frame_count - 1].node
                            : (p->doc ? p->doc->root : NULL);
}

/* The name of an open element, as written. A tree parse points into its input,
   which it keeps; an event parse does not keep its input, so it kept the name. */
static const unsigned char * frame_name(const xml_ctx * p, const xml_frame * f) {
  return f->name ? f->name
                 : (const unsigned char *)p->names.data + f->name_offset;
}

/* ---- comments ----------------------------------------------------------- */

GTEXT_XML_Status xml_parse_comment(xml_ctx * p, int keep) {
  const GTEXT_Markup_Location at = xml_here(p);
  xml_consume(p, 4); /* "<!--" */
  const size_t start = p->pos;
  size_t i = start;
  int found = 0;
  while (i < p->length) {
    const unsigned char b = p->in[i];
    if (b == '-') {
      if (i + 1 < p->length && p->in[i + 1] == '-') {
        if (i + 2 < p->length && p->in[i + 2] == '>') {
          found = 1;
          break;
        }
        if (i + 2 >= p->length) {
          break; /* input ends right after "--": unterminated, not malformed */
        }
        xml_consume(p, i - p->pos);
        return xml_fail(p, GTEXT_XML_E_SYNTAX,
            "\"--\" is not allowed inside a comment");
      }
      i++;
    }
    else if ((b >= 0x20 && (b < 0x7F || (b == 0x7F && p->version == XML_VERSION_10))) || b == '\t' || b == '\n' || b == '\r') {
      i++;
    }
    else {
      uint32_t cp = 0;
      size_t n = 0;
      if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
        xml_consume(p, i - p->pos);
        return xml_unexpected(p, "invalid character");
      }
      i += n;
    }
  }
  if (!found) {
    return xml_fail_at(p, GTEXT_XML_E_UNEXPECTED_EOF, "unterminated comment", &at);
  }
  if (keep) {
    buf_reset(&p->scratch);
    GTEXT_XML_Status status = xml_append(p, &p->scratch, "", 0);
    if (status == GTEXT_XML_OK) {
      status = xml_append_normalized(p, &p->scratch, start, i);
    }
    if (status != GTEXT_XML_OK) {
      return status;
    }
    if (p->stream) {
      status = xml_emit_comment(p, &at, p->scratch.data, p->scratch.length);
      if (status != GTEXT_XML_OK) {
        return status;
      }
    }
    else {
      GTEXT_Markup_Node * node = NULL;
      status = xml_from_markup(p,
          gtext_markup_create_comment(
              p->doc, p->scratch.data, p->scratch.length, &node));
      if (status != GTEXT_XML_OK) {
        return status;
      }
      xml_attach(current_parent(p), node, &at);
    }
  }
  xml_consume(p, i + 3 - p->pos);
  return GTEXT_XML_OK;
}

/* ---- processing instructions -------------------------------------------- */

GTEXT_XML_Status xml_parse_pi(xml_ctx * p, int keep) {
  const GTEXT_Markup_Location at = xml_here(p);
  xml_consume(p, 2); /* "<?" */
  size_t target_length = 0;
  GTEXT_XML_Status status = xml_scan_name(p, &target_length);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  const unsigned char * target = p->in + p->pos;
  if (memchr(target, ':', target_length)) {
    return xml_fail(p, GTEXT_XML_E_NAMESPACE,
        "a processing instruction target cannot contain a colon");
  }
  if (target_length == 3 && (target[0] | 0x20) == 'x' && (target[1] | 0x20) == 'm'
      && (target[2] | 0x20) == 'l') {
    return xml_fail(p, GTEXT_XML_E_SYNTAX,
        "the target xml is reserved; an XML declaration can only be first");
  }
  xml_consume(p, target_length);

  size_t data_start = p->pos;
  size_t end = p->pos;
  if (!xml_looking_at(p, "?>", 2)) {
    if (xml_cut_literal(p, "?>", 2)) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    }
    if (xml_skip_space(p) == 0) {
      return xml_unexpected(p,
          "white space is required between a processing instruction's target and data");
    }
    data_start = p->pos;
    size_t i = data_start;
    int found = 0;
    while (i < p->length) {
      const unsigned char b = p->in[i];
      if (b == '?' && i + 1 < p->length && p->in[i + 1] == '>') {
        found = 1;
        break;
      }
      if ((b >= 0x20 && (b < 0x7F || (b == 0x7F && p->version == XML_VERSION_10))) || b == '\t' || b == '\n' || b == '\r') {
        i++;
        continue;
      }
      uint32_t cp = 0;
      size_t n = 0;
      if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
        xml_consume(p, i - p->pos);
        return xml_unexpected(p, "invalid character");
      }
      i += n;
    }
    if (!found) {
      return xml_fail_at(p, GTEXT_XML_E_UNEXPECTED_EOF,
          "unterminated processing instruction", &at);
    }
    end = i;
  }
  if (keep && p->stream) {
    /* The data is normalized into the scratch buffer, which the target does not
       use: the target is a slice of the input. */
    buf_reset(&p->scratch);
    status = xml_append(p, &p->scratch, "", 0);
    if (status == GTEXT_XML_OK) {
      status = xml_append_normalized(p, &p->scratch, data_start, end);
    }
    if (status == GTEXT_XML_OK) {
      status = xml_emit_pi(p, &at, (const char *)target, target_length,
          p->scratch.data, p->scratch.length);
    }
    if (status != GTEXT_XML_OK) {
      return status;
    }
  }
  else if (keep) {
    markup_buf name;
    markup_buf data;
    markup_buf_init(&name, p->allocator);
    markup_buf_init(&data, p->allocator);
    status = xml_append(p, &name, (const char *)target, target_length);
    if (status == GTEXT_XML_OK) {
      status = xml_append(p, &data, "", 0);
    }
    if (status == GTEXT_XML_OK) {
      status = xml_append_normalized(p, &data, data_start, end);
    }
    GTEXT_Markup_Node * node = NULL;
    if (status == GTEXT_XML_OK) {
      status = xml_from_markup(p,
          gtext_markup_create_processing_instruction(
              p->doc, name.data, data.data, data.length, &node));
    }
    markup_buf_free(&name);
    markup_buf_free(&data);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    xml_attach(current_parent(p), node, &at);
  }
  xml_consume(p, end + 2 - p->pos);
  return GTEXT_XML_OK;
}

/* ---- CDATA sections and character data ---------------------------------- */

GTEXT_XML_Status xml_flush_text(xml_ctx * p) {
  if (p->text.length == 0) {
    return GTEXT_XML_OK;
  }
  if (p->stream) {
    const GTEXT_XML_Status emitted =
        xml_emit_text(p, &p->text_where, p->text.data, p->text.length);
    if (emitted != GTEXT_XML_OK) {
      return emitted;
    }
    buf_reset(&p->text);
    return GTEXT_XML_OK;
  }
  GTEXT_Markup_Node * node = NULL;
  const GTEXT_XML_Status status = xml_from_markup(p,
      gtext_markup_create_text(p->doc, p->text.data, p->text.length, &node));
  if (status != GTEXT_XML_OK) {
    return status;
  }
  xml_attach(current_parent(p), node, &p->text_where);
  buf_reset(&p->text);
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_parse_cdata(xml_ctx * p) {
  const GTEXT_Markup_Location at = xml_here(p);
  xml_consume(p, 9); /* "<![CDATA[" */
  const size_t start = p->pos;
  size_t i = start;
  int found = 0;
  while (i < p->length) {
    const unsigned char b = p->in[i];
    if (b == ']' && i + 2 < p->length && p->in[i + 1] == ']'
        && p->in[i + 2] == '>') {
      found = 1;
      break;
    }
    if ((b >= 0x20 && (b < 0x7F || (b == 0x7F && p->version == XML_VERSION_10))) || b == '\t' || b == '\n' || b == '\r') {
      i++;
      continue;
    }
    uint32_t cp = 0;
    size_t n = 0;
    if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
      xml_consume(p, i - p->pos);
      return xml_unexpected(p, "invalid character");
    }
    i += n;
  }
  if (!found) {
    return xml_fail_at(p, GTEXT_XML_E_UNEXPECTED_EOF,
        "unterminated CDATA section", &at);
  }
  buf_reset(&p->scratch);
  GTEXT_XML_Status status = xml_append(p, &p->scratch, "", 0);
  if (status == GTEXT_XML_OK) {
    status = xml_append_normalized(p, &p->scratch, start, i);
  }
  if (status != GTEXT_XML_OK) {
    return status;
  }
  if (p->stream) {
    status = xml_emit_cdata(p, &at, p->scratch.data, p->scratch.length);
    if (status != GTEXT_XML_OK) {
      return status;
    }
  }
  else {
    GTEXT_Markup_Node * node = NULL;
    status = xml_from_markup(p,
        gtext_markup_create_cdata(
            p->doc, p->scratch.data, p->scratch.length, &node));
    if (status != GTEXT_XML_OK) {
      return status;
    }
    xml_attach(current_parent(p), node, &at);
  }
  xml_consume(p, i + 3 - p->pos);
  return GTEXT_XML_OK;
}

/* ---- attributes --------------------------------------------------------- */

typedef struct {
  const unsigned char * name;
  size_t length;
} name_key;

static int same_raw_name(const xml_ctx * p, uint32_t existing, const void * key) {
  const name_key * k = key;
  const xml_attr * a = &p->attrs[existing];
  return a->name_length == k->length && memcmp(a->name_ptr, k->name, k->length) == 0;
}

typedef struct {
  const char * ns;
  const unsigned char * local;
  size_t local_length;
} expanded_key;

static const unsigned char * local_part(const xml_attr * a, size_t * length) {
  if (a->colon == SIZE_MAX) {
    *length = a->name_length;
    return a->name_ptr;
  }
  *length = a->name_length - a->colon - 1;
  return a->name_ptr + a->colon + 1;
}

static int same_expanded_name(
    const xml_ctx * p, uint32_t existing, const void * key) {
  const expanded_key * k = key;
  const xml_attr * a = &p->attrs[existing];
  size_t length = 0;
  const unsigned char * local = local_part(a, &length);
  return a->ns && strcmp(a->ns, k->ns) == 0 && length == k->local_length
      && memcmp(local, k->local, length) == 0;
}

/* Collapse runs of spaces and trim, in place: what a tokenized attribute type
   adds to CDATA normalization. */
static void normalize_tokenized(markup_buf * pool, size_t start) {
  char * d = pool->data + start;
  const size_t n = pool->length - start;
  size_t out = 0;
  int pending = 0;
  for (size_t i = 0; i < n; i++) {
    if (d[i] == ' ') {
      pending = out > 0;
    }
    else {
      if (pending) {
        d[out++] = ' ';
      }
      pending = 0;
      d[out++] = d[i];
    }
  }
  pool->length = start + out;
  pool->data[pool->length] = '\0';
}

/* The value at the cursor, quotes and all, into the pool. */
static GTEXT_XML_Status parse_attribute_value(xml_ctx * p,
    const GTEXT_Markup_Location * attribute_at, size_t * value_offset,
    size_t * value_length) {
  if (p->pos >= p->length || (p->in[p->pos] != '"' && p->in[p->pos] != '\'')) {
    return xml_unexpected(p, "an attribute value must be quoted");
  }
  const unsigned char quote = p->in[p->pos];
  xml_consume(p, 1);
  const size_t start = p->attr_pool.length;
  GTEXT_XML_Status status = xml_append(p, &p->attr_pool, "", 0);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  for (;;) {
    size_t i = p->pos;
    while (i < p->length) {
      const unsigned char b = p->in[i];
      if (b == quote || b == '<' || b == '&' || b == '\t' || b == '\n'
          || b == '\r') {
        break;
      }
      /* The limit is met at the byte that crosses it, wherever the run ends.
         A run is cut at the end of what has arrived, and the event parser is fed
         in pieces, so a limit checked only after the run would be reported or
         not depending on where the pieces ended whenever a byte that is not a
         character lies further along the same run. (Found by fuzz_xml.) */
      if (p->attr_pool.length - start + (i - p->pos) > p->max_attribute_bytes) {
        break;
      }
      if (b >= 0x20 && (b < 0x7F || (b == 0x7F && p->version == XML_VERSION_10))) {
        i++;
        continue;
      }
      uint32_t cp = 0;
      size_t n = 0;
      if (b >= 0x80 && xml_decode(p, i, &cp, &n) == XML_CHAR_OK) {
        if (p->version == XML_VERSION_11 && (cp == 0x85 || cp == 0x2028)) {
          break;
        }
        i += n;
        continue;
      }
      xml_consume(p, i - p->pos);
      return xml_unexpected(p, "invalid character");
    }
    if (i > p->pos) {
      status = xml_append(p, &p->attr_pool, (const char *)p->in + p->pos,
          i - p->pos);
      if (status != GTEXT_XML_OK) {
        return status;
      }
      xml_consume(p, i - p->pos);
    }
    if (p->attr_pool.length - start > p->max_attribute_bytes) {
      return xml_fail_at(p, GTEXT_XML_E_LIMIT_VALUE,
          "attribute value too long", attribute_at);
    }
    if (p->pos >= p->length) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
          "unterminated attribute value");
    }
    const unsigned char b = p->in[p->pos];
    if (b == quote) {
      xml_consume(p, 1);
      break;
    }
    if (b == '<') {
      return xml_fail(p, GTEXT_XML_E_SYNTAX,
          "'<' is not allowed in an attribute value");
    }
    const size_t line_end = xml_line_end_bytes(p, p->pos);
    if (b == '&') {
      status = xml_parse_reference(p, &p->attr_pool);
    }
    else if (b == '\t' || line_end > 0) {
      /* Literal XML whitespace and normalized line ends become one space. */
      status = xml_append(p, &p->attr_pool, " ", 1);
      xml_consume(p, line_end > 0 ? line_end : 1);
    }
    if (status != GTEXT_XML_OK) {
      return status;
    }
  }
  *value_offset = start;
  *value_length = p->attr_pool.length - start;
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status push_attribute(xml_ctx * p, xml_attr ** out) {
  const GTEXT_XML_Status status = xml_grow(p, (void **)&p->attrs,
      &p->attr_capacity, sizeof(xml_attr), p->attr_count + 1);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  xml_attr * a = &p->attrs[p->attr_count++];
  memset(a, 0, sizeof(*a));
  a->colon = SIZE_MAX;
  *out = a;
  return GTEXT_XML_OK;
}

/* ---- elements ----------------------------------------------------------- */

/* Name, optional prefix, as C strings in the scratch buffer, for the tree. The
   pointers are valid until the scratch buffer is next written. */
static GTEXT_XML_Status split_name(xml_ctx * p, const unsigned char * name,
    size_t length, size_t colon, const char ** prefix, const char ** local) {
  buf_reset(&p->scratch);
  const size_t local_at = colon == SIZE_MAX ? 0 : colon + 1;
  GTEXT_XML_Status status = xml_append(
      p, &p->scratch, (const char *)name + local_at, length - local_at);
  if (status == GTEXT_XML_OK) {
    status = xml_append(p, &p->scratch, "", 1);
  }
  if (status == GTEXT_XML_OK && colon != SIZE_MAX) {
    status = xml_append(p, &p->scratch, (const char *)name, colon);
  }
  if (status != GTEXT_XML_OK) {
    return status;
  }
  *local = p->scratch.data;
  *prefix = colon == SIZE_MAX ? NULL : p->scratch.data + (length - local_at) + 1;
  return GTEXT_XML_OK;
}

static int is_literal(const unsigned char * s, size_t n, const char * literal) {
  return n == strlen(literal) && memcmp(s, literal, n) == 0;
}

GTEXT_XML_Status xml_parse_start_tag(xml_ctx * p, int * is_empty) {
  GTEXT_Markup_Node * parent = current_parent(p);
  const GTEXT_Markup_Location at = xml_here(p);
  xml_consume(p, 1); /* '<' */

  size_t name_length = 0;
  GTEXT_XML_Status status = xml_scan_name(p, &name_length);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  const unsigned char * name = p->in + p->pos;
  const GTEXT_Markup_Location name_at = xml_here(p);
  size_t colon = SIZE_MAX;
  status = xml_check_qname(p, name, name_length, &colon, &name_at);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  if (p->frame_count + 1 > p->max_depth) {
    return xml_fail_at(p, GTEXT_XML_E_LIMIT_DEPTH, "elements nested too deeply", &at);
  }
  xml_consume(p, name_length);

  p->attr_count = 0;
  buf_reset(&p->attr_pool);
  int names_begun = 0;
  *is_empty = 0;
  for (;;) {
    const size_t space = xml_skip_space(p);
    if (p->pos >= p->length) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unterminated start tag");
    }
    const unsigned char c = p->in[p->pos];
    if (c == '>') {
      xml_consume(p, 1);
      break;
    }
    if (c == '/') {
      p->tag_end_at = xml_here(p);
      status = xml_expect(p, "/>", 2, "'/' must be followed by '>'");
      if (status != GTEXT_XML_OK) {
        return status;
      }
      *is_empty = 1;
      break;
    }
    if (space == 0) {
      return xml_unexpected(p, "white space is required between attributes");
    }

    const GTEXT_Markup_Location attribute_at = xml_here(p);
    size_t length = 0;
    status = xml_scan_name(p, &length);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    if (p->attr_count >= p->max_attributes) {
      return xml_fail_at(p, GTEXT_XML_E_LIMIT_ATTRIBUTES,
          "too many attributes on one element", &attribute_at);
    }
    xml_attr * a = NULL;
    status = push_attribute(p, &a);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    a->name_ptr = p->in + p->pos;
    a->name_length = length;
    a->where = attribute_at;
    status = xml_check_qname(p, a->name_ptr, length, &a->colon, &attribute_at);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    xml_consume(p, length);

    xml_skip_space(p);
    status = xml_expect(p, "=", 1, "'=' was expected after the attribute name");
    if (status != GTEXT_XML_OK) {
      return status;
    }
    xml_skip_space(p);
    status = parse_attribute_value(
        p, &attribute_at, &a->value_offset, &a->value_length);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    a = &p->attrs[p->attr_count - 1];

    if (p->seams.attribute_type
        && p->seams.attribute_type(p->seams.user, (const char *)name,
            name_length, (const char *)a->name_ptr, a->name_length)) {
      normalize_tokenized(&p->attr_pool, a->value_offset);
      a->value_length = p->attr_pool.length - a->value_offset;
    }

    if (!names_begun) {
      status = xml_hset_begin(p, &p->raw_names, 0);
      if (status != GTEXT_XML_OK) {
        return status;
      }
      names_begun = 1;
    }
    name_key key = {a->name_ptr, a->name_length};
    a->raw_hash = xml_hash(a->name_ptr, a->name_length, 0);
    const int added = xml_hset_insert(p, &p->raw_names, a->raw_hash,
        (uint32_t)(p->attr_count - 1), same_raw_name, &key);
    if (added < 0) {
      return GTEXT_XML_E_OOM;
    }
    if (added > 0) {
      return xml_fail_at(p, GTEXT_XML_E_DUPLICATE_ATTRIBUTE,
          "the same attribute appears twice on one element", &attribute_at);
    }
  }

  /* Defaults a declaration would supply: none, until a DTD is processed. */
  if (p->seams.default_attributes) {
    const xml_default_attribute * list = NULL;
    const size_t n = p->seams.default_attributes(
        p->seams.user, (const char *)name, name_length, &list);
    for (size_t k = 0; k < n; k++) {
      const size_t dl = strlen(list[k].name);
      name_key key = {(const unsigned char *)list[k].name, dl};
      int present = 0;
      for (size_t j = 0; j < p->attr_count && !present; j++) {
        present = same_raw_name(p, (uint32_t)j, &key);
      }
      if (present) {
        continue;
      }
      if (p->attr_count >= p->max_attributes) {
        return xml_fail_at(p, GTEXT_XML_E_LIMIT_ATTRIBUTES,
            "too many attributes on one element", &at);
      }
      xml_attr * a = NULL;
      status = push_attribute(p, &a);
      if (status != GTEXT_XML_OK) {
        return status;
      }
      a->name_ptr = key.name;
      a->name_length = dl;
      a->where = at;
      status = xml_check_qname(p, a->name_ptr, dl, &a->colon, &at);
      if (status != GTEXT_XML_OK) {
        return status;
      }
      a->value_offset = p->attr_pool.length;
      a->value_length = strlen(list[k].value);
      status = xml_append(p, &p->attr_pool, list[k].value, a->value_length);
      if (status != GTEXT_XML_OK) {
        return status;
      }
    }
  }

  /* Namespaces. Declarations first, all of them, because they apply to the
     element and to every attribute of this tag whatever the order written. */
  const size_t mark = xml_scope_mark(p);
  const char * pool = p->attr_pool.data ? p->attr_pool.data : "";
  for (size_t i = 0; i < p->attr_count; i++) {
    xml_attr * a = &p->attrs[i];
    if (is_literal(a->name_ptr, a->name_length, "xmlns")) {
      a->kind = XML_ATTR_DEFAULT_NS_DECL;
      status = xml_scope_declare(p, "", 0, pool + a->value_offset,
          a->value_length, &a->where);
    }
    else if (a->colon != SIZE_MAX && a->colon == 5
        && memcmp(a->name_ptr, "xmlns", 5) == 0) {
      a->kind = XML_ATTR_PREFIX_DECL;
      status = xml_scope_declare(p, (const char *)a->name_ptr + 6,
          a->name_length - 6, pool + a->value_offset, a->value_length,
          &a->where);
    }
    else {
      continue;
    }
    if (status != GTEXT_XML_OK) {
      xml_scope_release(p, mark);
      return status;
    }
  }

  const char * element_ns = NULL;
  {
    const char * prefix = "";
    size_t prefix_length = 0;
    if (colon != SIZE_MAX) {
      prefix = (const char *)name;
      prefix_length = colon;
    }
    if (is_literal((const unsigned char *)prefix, prefix_length, "xmlns")) {
      xml_scope_release(p, mark);
      return xml_fail_at(p, GTEXT_XML_E_NAMESPACE,
          "the prefix xmlns cannot be used on an element", &name_at);
    }
    if (!xml_scope_lookup(p, prefix, prefix_length, &element_ns)) {
      xml_scope_release(p, mark);
      return xml_fail_at(p, GTEXT_XML_E_NAMESPACE,
          "the element's prefix is not bound to a namespace", &name_at);
    }
  }

  int expanded_begun = 0;
  for (size_t i = 0; i < p->attr_count; i++) {
    xml_attr * a = &p->attrs[i];
    if (a->kind == XML_ATTR_DEFAULT_NS_DECL || a->kind == XML_ATTR_PREFIX_DECL) {
      a->ns = GTEXT_MARKUP_NS_XMLNS;
      continue;
    }
    if (a->colon == SIZE_MAX) {
      a->kind = XML_ATTR_PLAIN;
      continue;
    }
    a->kind = XML_ATTR_PREFIXED;
    if (!xml_scope_lookup(p, (const char *)a->name_ptr, a->colon, &a->ns)) {
      xml_scope_release(p, mark);
      return xml_fail_at(p, GTEXT_XML_E_NAMESPACE,
          "the attribute's prefix is not bound to a namespace", &a->where);
    }
    if (!expanded_begun) {
      status = xml_hset_begin(p, &p->expanded_names, 0);
      if (status != GTEXT_XML_OK) {
        xml_scope_release(p, mark);
        return status;
      }
      expanded_begun = 1;
    }
    size_t local_length = 0;
    const unsigned char * local = local_part(a, &local_length);
    expanded_key key = {a->ns, local, local_length};
    const uint32_t hash = xml_hash(local, local_length,
        xml_hash(a->ns, strlen(a->ns), 0));
    const int added = xml_hset_insert(p, &p->expanded_names, hash, (uint32_t)i,
        same_expanded_name, &key);
    if (added < 0) {
      xml_scope_release(p, mark);
      return GTEXT_XML_E_OOM;
    }
    if (added > 0) {
      xml_scope_release(p, mark);
      return xml_fail_at(p, GTEXT_XML_E_DUPLICATE_ATTRIBUTE,
          "two attributes have the same namespace and local name", &a->where);
    }
  }

  if (p->stream) {
    /* Events, not a tree. */
    status = xml_emit_element_start(p, &at, name, name_length, colon, element_ns);
    if (status == GTEXT_XML_OK && *is_empty) {
      status = xml_emit_element_end(p, &p->tag_end_at, name, name_length);
    }
    if (status != GTEXT_XML_OK || *is_empty) {
      xml_scope_release(p, mark);
      return status;
    }
    const size_t name_offset = p->names.length;
    status = xml_append(p, &p->names, (const char *)name, name_length);
    if (status == GTEXT_XML_OK) {
      status = xml_grow(p, (void **)&p->frames, &p->frame_capacity,
          sizeof(xml_frame), p->frame_count + 1);
    }
    if (status != GTEXT_XML_OK) {
      xml_scope_release(p, mark);
      return status;
    }
    xml_frame * f = &p->frames[p->frame_count++];
    f->node = NULL;
    f->name = NULL;
    f->name_offset = name_offset;
    f->name_length = name_length;
    f->binding_mark = mark;
    return GTEXT_XML_OK;
  }

  /* The tree. */
  const char * prefix = NULL;
  const char * local = NULL;
  status = split_name(p, name, name_length, colon, &prefix, &local);
  GTEXT_Markup_Node * element = NULL;
  if (status == GTEXT_XML_OK) {
    status = xml_from_markup(p,
        gtext_markup_create_element(p->doc, element_ns, prefix, local, &element));
  }
  if (status != GTEXT_XML_OK) {
    xml_scope_release(p, mark);
    return status;
  }
  xml_attach(parent, element, &at);
  pool = p->attr_pool.data ? p->attr_pool.data : "";
  for (size_t i = 0; i < p->attr_count; i++) {
    const xml_attr * a = &p->attrs[i];
    const char * a_prefix = NULL;
    const char * a_local = NULL;
    if (a->kind == XML_ATTR_DEFAULT_NS_DECL) {
      a_local = "xmlns";
    }
    else if (a->kind == XML_ATTR_PREFIX_DECL) {
      a_prefix = "xmlns";
      status = split_name(p, a->name_ptr, a->name_length, a->colon, &a_prefix, &a_local);
    }
    else {
      status = split_name(p, a->name_ptr, a->name_length, a->colon, &a_prefix, &a_local);
    }
    if (status == GTEXT_XML_OK) {
      status = xml_from_markup(p,
          gtext_markup_element_set_attribute(element, a->ns, a_prefix, a_local,
              pool + a->value_offset, a->value_length));
    }
    if (status == GTEXT_XML_OK) {
      status = xml_from_markup(p,
          gtext_markup_element_set_attribute_location(element, i, &a->where));
    }
    if (status != GTEXT_XML_OK) {
      xml_scope_release(p, mark);
      return status;
    }
  }

  if (*is_empty) {
    xml_scope_release(p, mark);
    return GTEXT_XML_OK;
  }
  status = xml_grow(p, (void **)&p->frames, &p->frame_capacity,
      sizeof(xml_frame), p->frame_count + 1);
  if (status != GTEXT_XML_OK) {
    xml_scope_release(p, mark);
    return status;
  }
  xml_frame * f = &p->frames[p->frame_count++];
  f->node = element;
  f->name = name;
  f->name_length = name_length;
  f->binding_mark = mark;
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_parse_end_tag(xml_ctx * p) {
  const GTEXT_Markup_Location at = xml_here(p);
  if (p->source_count > 1
      && p->frame_count <= p->sources[p->source_count - 1].entry_element_depth) {
    return xml_fail_at(p, GTEXT_XML_E_MISMATCHED_TAG,
        "an entity source cannot close an element opened by its parent source", &at);
  }
  xml_consume(p, 2); /* "</" */
  size_t length = 0;
  GTEXT_XML_Status status = xml_scan_name(p, &length);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  const xml_frame * f = &p->frames[p->frame_count - 1];
  const unsigned char * open_name = frame_name(p, f);
  if (length != f->name_length || memcmp(p->in + p->pos, open_name, length) != 0) {
    return xml_fail_at(p, GTEXT_XML_E_MISMATCHED_TAG,
        "end tag does not match the start tag", &at);
  }
  xml_consume(p, length);
  xml_skip_space(p);
  status = xml_expect(p, ">", 1, "'>' was expected to end the end tag");
  if (status != GTEXT_XML_OK) {
    return status;
  }
  if (p->stream) {
    status = xml_emit_element_end(p, &at, open_name, f->name_length);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    p->names.length = f->name_offset;
  }
  xml_scope_release(p, f->binding_mark);
  p->frame_count--;
  return GTEXT_XML_OK;
}

/* The root element and everything inside it. */
static GTEXT_XML_Status parse_element(xml_ctx * p) {
  int is_empty = 0;
  GTEXT_XML_Status status = xml_parse_start_tag(p, &is_empty);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  while (p->frame_count > 0) {
    if (p->pos >= p->length) {
      if (p->source_count > 1) {
        status = xml_flush_text(p);
        if (status != GTEXT_XML_OK) {
          return status;
        }
        status = xml_source_pop(p);
        if (status != GTEXT_XML_OK) {
          return status;
        }
        continue;
      }
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
          "the input ended inside an element");
    }
    const unsigned char c = p->in[p->pos];
    if (c == '<') {
      status = xml_flush_text(p);
      if (status != GTEXT_XML_OK) {
        return status;
      }
      if (xml_looking_at(p, "</", 2)) {
        status = xml_parse_end_tag(p);
      }
      else if (xml_looking_at(p, "<!--", 4)) {
        status = xml_parse_comment(p, 1);
      }
      else if (xml_looking_at(p, "<![CDATA[", 9)) {
        status = xml_parse_cdata(p);
      }
      else if (xml_looking_at(p, "<?", 2)) {
        status = xml_parse_pi(p, 1);
      }
      else if (xml_looking_at(p, "<!", 2)) {
        status = xml_fail(p, GTEXT_XML_E_SYNTAX,
            "not a comment or a CDATA section");
      }
      else {
        status = xml_parse_start_tag(p, &is_empty);
      }
    }
    else if (c == '&') {
      if (p->text.length == 0) {
        p->text_where = xml_here(p);
      }
      status = xml_parse_reference(p, &p->text);
    }
    else {
      size_t end = p->pos;
      status = xml_scan_text_run(p, &end);
      if (status == GTEXT_XML_OK) {
        if (p->text.length == 0) {
          p->text_where = xml_here(p);
        }
        status = xml_append_normalized(p, &p->text, p->pos, end);
        if (status == GTEXT_XML_OK) {
          xml_consume(p, end - p->pos);
        }
      }
    }
    if (status != GTEXT_XML_OK) {
      return status;
    }
  }
  return GTEXT_XML_OK;
}

/* ---- the prolog and the document ---------------------------------------- */

/* A quoted pseudo-attribute value of the XML declaration: `name`, Eq, quote,
   value, quote. The cursor is on `name`. */
static GTEXT_XML_Status pseudo_attribute(xml_ctx * p, const char * name,
    size_t * value_at, size_t * value_length, GTEXT_Markup_Location * where) {
  GTEXT_XML_Status status = xml_expect(p, name, strlen(name),
      "an XML declaration's pseudo-attribute was expected");
  if (status != GTEXT_XML_OK) {
    return status;
  }
  xml_skip_space(p);
  status = xml_expect(p, "=", 1, "'=' was expected");
  if (status != GTEXT_XML_OK) {
    return status;
  }
  xml_skip_space(p);
  if (p->pos >= p->length || (p->in[p->pos] != '"' && p->in[p->pos] != '\'')) {
    return xml_unexpected(p, "a quoted value was expected");
  }
  const unsigned char quote = p->in[p->pos];
  xml_consume(p, 1);
  *where = xml_here(p);
  *value_at = p->pos;
  size_t i = p->pos;
  while (i < p->length && p->in[i] != quote) {
    const unsigned char b = p->in[i];
    if (b < 0x20 || b >= 0x80) {
      /* Nothing legal here is outside printable ASCII; report why. */
      xml_consume(p, i - p->pos);
      return xml_unexpected(p, "character not allowed in an XML declaration");
    }
    i++;
  }
  if (i >= p->length) {
    xml_consume(p, i - p->pos);
    return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unterminated XML declaration");
  }
  *value_length = i - p->pos;
  xml_consume(p, i + 1 - p->pos);
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_parse_xml_declaration(xml_ctx * p) {
  xml_consume(p, 5); /* "<?xml" */
  size_t at = 0;
  size_t length = 0;
  GTEXT_Markup_Location where;
  GTEXT_XML_Status status;

  if (xml_skip_space(p) == 0) {
    return xml_unexpected(p, "white space is required before version");
  }
  status = pseudo_attribute(p, "version", &at, &length, &where);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  /* VersionNum ::= '1.' [0-9]+ */
  int grammatical = length >= 3 && p->in[at] == '1' && p->in[at + 1] == '.';
  for (size_t i = 2; grammatical && i < length; i++) {
    grammatical = p->in[at + i] >= '0' && p->in[at + i] <= '9';
  }
  if (!grammatical) {
    return xml_fail_at(p, GTEXT_XML_E_SYNTAX,
        "the version must be 1. followed by digits", &where);
  }
  const int version_10 = length == 3 && p->in[at + 2] == '0';
  const int version_11 = length == 3 && p->in[at + 2] == '1';
  if (!version_10 && !version_11) {
    return xml_fail_at(p, GTEXT_XML_E_UNSUPPORTED_VERSION,
        "only XML 1.0 and XML 1.1 are supported", &where);
  }

  const char * label = NULL;
  size_t label_at = 0;
  size_t label_length = 0;
  GTEXT_Markup_Standalone standalone = GTEXT_MARKUP_STANDALONE_UNSPECIFIED;
  size_t space = xml_skip_space(p);
  if (xml_cut_literal(p, "encoding", 8)) {
    return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
  }
  if (xml_looking_at(p, "encoding", 8)) {
    if (space == 0) {
      return xml_unexpected(p, "white space is required before encoding");
    }
    status = pseudo_attribute(p, "encoding", &label_at, &label_length, &where);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    /* EncName ::= [A-Za-z] ([A-Za-z0-9._] | '-')* */
    int name_ok = label_length > 0
        && ((p->in[label_at] | 0x20) >= 'a' && (p->in[label_at] | 0x20) <= 'z');
    for (size_t i = 1; name_ok && i < label_length; i++) {
      const unsigned char b = p->in[label_at + i];
      name_ok = ((b | 0x20) >= 'a' && (b | 0x20) <= 'z') || (b >= '0' && b <= '9')
          || b == '.' || b == '_' || b == '-';
    }
    if (!name_ok) {
      return xml_fail_at(p, GTEXT_XML_E_SYNTAX, "not an encoding name", &where);
    }
    status = xml_check_label(p, p->in + label_at, label_length, &where);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    label = (const char *)p->in + label_at;
    space = xml_skip_space(p);
  }
  if (xml_cut_literal(p, "standalone", 10)) {
    return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
  }
  if (xml_looking_at(p, "standalone", 10)) {
    if (space == 0) {
      return xml_unexpected(p, "white space is required before standalone");
    }
    status = pseudo_attribute(p, "standalone", &at, &length, &where);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    if (length == 3 && memcmp(p->in + at, "yes", 3) == 0) {
      standalone = GTEXT_MARKUP_STANDALONE_YES;
    }
    else if (length == 2 && memcmp(p->in + at, "no", 2) == 0) {
      standalone = GTEXT_MARKUP_STANDALONE_NO;
    }
    else {
      return xml_fail_at(p, GTEXT_XML_E_SYNTAX,
          "standalone must be \"yes\" or \"no\"", &where);
    }
    xml_skip_space(p);
  }
  status = xml_expect(p, "?>", 2, "'?>' was expected to end the XML declaration");
  if (status != GTEXT_XML_OK) {
    return status;
  }

  /* Select the profile only after the complete declaration is known valid. */
  p->version = version_11 ? XML_VERSION_11 : XML_VERSION_10;
  const char * version = version_11 ? "1.1" : "1.0";

  if (p->stream) {
    return xml_emit_document_start(p, version,
        label ? label : xml_family_name(p->enc_family),
        label ? label_length : strlen(xml_family_name(p->enc_family)),
        p->enc_bom ? GTEXT_MARKUP_ENCODING_BOM
                   : (label ? GTEXT_MARKUP_ENCODING_DECLARATION
                            : GTEXT_MARKUP_ENCODING_DEFAULT),
        standalone);
  }
  status = xml_from_markup(p,
      gtext_markup_document_set_xml_declaration(p->doc, version, standalone));
  if (status != GTEXT_XML_OK) {
    return status;
  }
  /* The label as written, so that a writer can say what the document said. */
  markup_buf copy;
  markup_buf_init(&copy, p->allocator);
  status = label ? xml_append(p, &copy, label, label_length)
                 : xml_append(p, &copy, xml_family_name(p->enc_family),
                       strlen(xml_family_name(p->enc_family)));
  if (status == GTEXT_XML_OK) {
    status = xml_from_markup(p,
        gtext_markup_document_set_encoding(p->doc, copy.data,
            p->enc_bom ? GTEXT_MARKUP_ENCODING_BOM
                       : (label ? GTEXT_MARKUP_ENCODING_DECLARATION
                             : GTEXT_MARKUP_ENCODING_DEFAULT)));
  }
  markup_buf_free(&copy);
  return status;
}

/* Comments, processing instructions and white space, where they may occur
   outside the root element. Returns OK having stopped at something else. */
static GTEXT_XML_Status parse_misc(xml_ctx * p, int allow_doctype, int * seen_doctype) {
  for (;;) {
    xml_skip_space(p);
    if (p->pos >= p->length) {
      return GTEXT_XML_OK;
    }
    GTEXT_XML_Status status;
    if (xml_looking_at(p, "<!--", 4)) {
      status = xml_parse_comment(p, 1);
    }
    else if (xml_looking_at(p, "<?", 2)) {
      status = xml_parse_pi(p, 1);
    }
    else if (xml_looking_at(p, "<!DOCTYPE", 9)) {
      if (!allow_doctype || *seen_doctype) {
        return xml_fail(p, GTEXT_XML_E_DOCTYPE,
            *seen_doctype ? "a second document type declaration"
                          : "the document type declaration must come before the root element");
      }
      *seen_doctype = 1;
      status = xml_parse_doctype(p);
    }
    else {
      return GTEXT_XML_OK;
    }
    if (status != GTEXT_XML_OK) {
      return status;
    }
  }
}

static GTEXT_XML_Status parse_document(xml_ctx * p) {
  GTEXT_XML_Status status = xml_detect_encoding(p);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  status = xml_source_init(p, p->in);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  xml_probe_xml11_declaration(p);
  int declared = 0;
  if (xml_looking_at(p, "<?xml", 5) && p->pos + 5 < p->length
      && xml_space_bytes(p, p->pos + 5) > 0) {
    status = xml_parse_xml_declaration(p);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    declared = 1;
  }
  if (!declared) {
    status = xml_from_markup(p,
        gtext_markup_document_set_encoding(p->doc,
            xml_family_name(p->enc_family),
            p->enc_bom ? GTEXT_MARKUP_ENCODING_BOM : GTEXT_MARKUP_ENCODING_DEFAULT));
    if (status != GTEXT_XML_OK) {
      return status;
    }
  }

  int seen_doctype = 0;
  status = parse_misc(p, 1, &seen_doctype);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  if (p->pos >= p->length) {
    return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
        "the document has no root element");
  }
  if (p->in[p->pos] != '<' || xml_looking_at(p, "</", 2)
      || xml_looking_at(p, "<!", 2)) {
    return xml_unexpected(p, "the root element was expected");
  }
  status = parse_element(p);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  status = parse_misc(p, 0, &seen_doctype);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  if (p->pos < p->length) {
    return xml_unexpected(p, "content is not allowed after the root element");
  }
  return GTEXT_XML_OK;
}

/* ---- the entry point ---------------------------------------------------- */

static size_t limit_or(size_t given, size_t fallback) {
  return given != 0 ? given : fallback;
}

static void cleanup(xml_ctx * p) {
  markup_buf_free(&p->decoded);
  markup_buf_free(&p->text);
  markup_buf_free(&p->attr_pool);
  markup_buf_free(&p->scratch);
  xml_release(p, p->attrs);
  xml_release(p, p->frames);
  xml_hset_free(p, &p->raw_names);
  xml_hset_free(p, &p->expanded_names);
  xml_scope_free(p);
  xml_source_free(p);
}

GTEXT_XML_Status xml_parse_with_seams(const void * data, size_t len,
    const GTEXT_XML_Parse_Options * opts, const xml_seams * seams,
    GTEXT_Markup_Document ** out, GTEXT_XML_Error * err) {
  if (err) {
    memset(err, 0, sizeof(*err));
  }
  if (!out || (!data && len != 0)) {
    if (err) {
      err->code = GTEXT_XML_E_INVALID;
      err->message = "invalid argument";
      err->line = 1;
      err->column = 1;
    }
    return GTEXT_XML_E_INVALID;
  }

  GTEXT_XML_Status option_status = xml_validate_dtd_options(opts, err);
  if (option_status != GTEXT_XML_OK) {
    return option_status;
  }

  GTEXT_XML_Parse_Options use = opts ? *opts : gtext_xml_parse_options_default();
  const size_t max_input = limit_or(use.max_input_bytes, XML_DEFAULT_MAX_INPUT);
  const GTEXT_Allocator * allocator =
      use.allocator ? use.allocator : gtext_allocator_default();

  xml_ctx p;
  memset(&p, 0, sizeof(p));
  static const unsigned char empty[1] = {0};
  p.in = data ? (const unsigned char *)data : empty;
  p.length = len;
  p.line = 1;
  p.column = 1;
  p.max_depth = limit_or(use.max_depth, XML_DEFAULT_MAX_DEPTH);
  p.max_attributes = limit_or(use.max_attributes, XML_DEFAULT_MAX_ATTRIBUTES);
  if (p.max_attributes > UINT32_MAX) {
    p.max_attributes = UINT32_MAX; /* Attribute indexes are 32 bits. */
  }
  p.max_attribute_bytes =
      limit_or(use.max_attribute_bytes, XML_DEFAULT_MAX_ATTRIBUTE_BYTES);
  p.max_name_bytes = limit_or(use.max_name_bytes, XML_DEFAULT_MAX_NAME_BYTES);
  p.max_namespaces = limit_or(use.max_namespaces, XML_DEFAULT_MAX_NAMESPACES);
  p.max_source_depth = XML_DEFAULT_MAX_SOURCE_DEPTH;
  p.dtd_mode = use.dtd_mode;
  p.dtd_external_policy = use.dtd_external_policy;
  if (seams) {
    p.seams = *seams;
  }

  GTEXT_XML_Status status = GTEXT_XML_OK;
  GTEXT_Markup_Document * doc = NULL;
  if (len > max_input) {
    status = xml_fail(&p, GTEXT_XML_E_LIMIT_INPUT, "input too long");
    goto failed;
  }

  GTEXT_Markup_Options markup = gtext_markup_options_default();
  markup.allocator = allocator;
  const size_t max_nodes = limit_or(use.max_nodes, XML_DEFAULT_MAX_NODES);
  markup.max_nodes = max_nodes == SIZE_MAX ? 0 : max_nodes;
  /* The deepest node is the text inside the deepest element: one level below
     it, the document being level 0. */
  markup.max_depth = p.max_depth >= SIZE_MAX - 1 ? 0 : p.max_depth + 1;
  markup.max_attributes = p.max_attributes == SIZE_MAX ? 0 : p.max_attributes;
  if (gtext_markup_document_new(&markup, &doc) != GTEXT_MARKUP_OK) {
    status = xml_fail(&p, GTEXT_XML_E_OOM, "out of memory");
    goto failed;
  }
  p.doc = doc;
  p.allocator = &doc->allocator;
  markup_buf_init(&p.decoded, p.allocator);
  markup_buf_init(&p.text, p.allocator);
  markup_buf_init(&p.attr_pool, p.allocator);
  markup_buf_init(&p.scratch, p.allocator);

  status = parse_document(&p);
  cleanup(&p);
  if (status == GTEXT_XML_OK) {
    *out = doc;
    return GTEXT_XML_OK;
  }
  gtext_markup_document_free(doc);
  doc = NULL;

failed:
  if (p.err.code == GTEXT_XML_OK) {
    p.err.code = status;
    p.err.message = gtext_xml_status_string(status);
  }
  if (err) {
    *err = p.err;
  }
  return p.err.code;
}

GTEXT_XML_Status gtext_xml_parse(const void * data, size_t len,
    const GTEXT_XML_Parse_Options * opts, GTEXT_Markup_Document ** out,
    GTEXT_XML_Error * err) {
  return xml_parse_with_seams(data, len, opts, NULL, out, err);
}
