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
 * Namespace scope for the XML parser.
 *
 * Namespaces in XML 1.0 (Third Edition): [4] NCName, [7] QName, the reserved
 * prefixes `xml` and `xmlns`, and the constraints "Prefix Declared", "No Prefix
 * Undeclaring", "Reserved Prefixes and Namespace Names".
 *
 * The scope is a stack of declarations. An element remembers the height of the
 * stack when it opened and drops back to it when it closes, so a declaration is
 * visible exactly in the element that made it and everything inside.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "xml_internal.h"

#define NS_XML GTEXT_MARKUP_NS_XML
#define NS_XMLNS GTEXT_MARKUP_NS_XMLNS

static int is_literal(
    const char * s, size_t length, const char * literal, size_t literal_length) {
  return length == literal_length && memcmp(s, literal, length) == 0;
}

size_t xml_scope_mark(const xml_ctx * p) {
  return p->binding_count;
}

void xml_scope_release(xml_ctx * p, size_t mark) {
  while (p->binding_count > mark) {
    p->binding_count--;
    xml_release(p, p->bindings[p->binding_count].data);
    p->bindings[p->binding_count].data = NULL;
  }
}

void xml_scope_free(xml_ctx * p) {
  xml_scope_release(p, 0);
  xml_release(p, p->bindings);
  p->bindings = NULL;
  p->binding_capacity = 0;
}

GTEXT_XML_Status xml_scope_declare(xml_ctx * p, const char * prefix,
    size_t prefix_length, const char * uri, size_t uri_length,
    const GTEXT_Markup_Location * where) {
  if (is_literal(prefix, prefix_length, "xmlns", 5)) {
    return xml_fail_at(p, GTEXT_XML_E_NAMESPACE,
        "the prefix xmlns cannot be declared", where);
  }
  const int is_xml_prefix = is_literal(prefix, prefix_length, "xml", 3);
  const int is_xml_uri =
      is_literal(uri, uri_length, NS_XML, sizeof(NS_XML) - 1);
  if (is_xml_prefix && !is_xml_uri) {
    return xml_fail_at(p, GTEXT_XML_E_NAMESPACE,
        "the prefix xml can only be bound to the XML namespace", where);
  }
  if (is_xml_uri && !is_xml_prefix) {
    return xml_fail_at(p, GTEXT_XML_E_NAMESPACE,
        "the XML namespace can only be bound to the prefix xml", where);
  }
  if (is_literal(uri, uri_length, NS_XMLNS, sizeof(NS_XMLNS) - 1)) {
    return xml_fail_at(p, GTEXT_XML_E_NAMESPACE,
        "the xmlns namespace cannot be declared", where);
  }
  if (prefix_length > 0 && uri_length == 0) {
    return xml_fail_at(p, GTEXT_XML_E_NAMESPACE,
        "a prefix cannot be undeclared (Namespaces in XML 1.0)", where);
  }
  if (p->binding_count >= p->max_namespaces) {
    return xml_fail_at(p, GTEXT_XML_E_LIMIT_NAMESPACES,
        "too many namespace declarations in scope", where);
  }
  GTEXT_XML_Status status = xml_grow(p, (void **)&p->bindings,
      &p->binding_capacity, sizeof(xml_binding), p->binding_count + 1);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  char * data = xml_alloc(p, prefix_length + uri_length + 2);
  if (!data) {
    return xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
  }
  memcpy(data, prefix, prefix_length);
  memcpy(data + prefix_length + 1, uri, uri_length);
  xml_binding * b = &p->bindings[p->binding_count++];
  b->data = data;
  b->prefix_length = prefix_length;
  b->uri_length = uri_length;
  return GTEXT_XML_OK;
}

int xml_scope_lookup(const xml_ctx * p, const char * prefix,
    size_t prefix_length, const char ** uri) {
  if (is_literal(prefix, prefix_length, "xml", 3)) {
    *uri = NS_XML;
    return 1;
  }
  for (size_t i = p->binding_count; i > 0; i--) {
    const xml_binding * b = &p->bindings[i - 1];
    if (b->prefix_length == prefix_length
        && memcmp(b->data, prefix, prefix_length) == 0) {
      *uri = b->uri_length ? b->data + b->prefix_length + 1 : NULL;
      return 1;
    }
  }
  *uri = NULL;
  /* The default namespace is "none" until declared; a prefix is an error. */
  return prefix_length == 0;
}

GTEXT_XML_Status xml_check_qname(xml_ctx * p, const unsigned char * bytes,
    size_t name_length, size_t * colon, const GTEXT_Markup_Location * where) {
  const char * name = (const char *)bytes;
  const char * first = memchr(name, ':', name_length);
  if (!first) {
    *colon = SIZE_MAX;
    return GTEXT_XML_OK;
  }
  const size_t at = (size_t)(first - name);
  /* A name that scanned as a Name has only colons that the Name production
     allows; Namespaces in XML allows exactly one, with an NCName each side. */
  if (at == 0 || at + 1 == name_length
      || memchr(name + at + 1, ':', name_length - at - 1)
      || !gtext_markup_is_xml_ncname(name, at)
      || !gtext_markup_is_xml_ncname(name + at + 1, name_length - at - 1)) {
    return xml_fail_at(p, GTEXT_XML_E_NAMESPACE,
        "not a qualified name: at most one colon, with a name on each side",
        where);
  }
  *colon = at;
  return GTEXT_XML_OK;
}
