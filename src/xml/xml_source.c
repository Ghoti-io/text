/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "xml_internal.h"

static const unsigned char kEmptySource[1] = {0};

/*
 * The top entry describes the active source. Before pushing a child, the
 * active cursor is copied into the parent entry and marked suspended. The
 * child's cursor is then installed in xml_ctx. Pop performs the inverse.
 * Keeping this transition iterative is the key property: source depth never
 * consumes C stack, and an entity cannot close an element opened by its parent
 * (or leave an element open for its parent to close).
 */

static GTEXT_XML_Status source_reserve(xml_ctx * p, size_t needed) {
  return xml_grow(p, (void **)&p->sources, &p->source_capacity,
      sizeof(*p->sources), needed);
}

static void save_active(xml_ctx * p, xml_source_frame * f) {
  f->bytes = p->in;
  f->length = p->length;
  f->pos = p->pos;
  f->base = p->base;
  f->line = p->line;
  f->column = p->column;
  f->cursor_previous = p->cursor_previous;
}

GTEXT_XML_Status xml_source_init(xml_ctx * p, const void * identity) {
  if (p->source_count != 0) {
    return xml_fail(p, GTEXT_XML_E_STATE, "XML source stack is already initialized");
  }
  GTEXT_XML_Status status = source_reserve(p, 1);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  xml_source_frame * f = &p->sources[0];
  memset(f, 0, sizeof(*f));
  f->kind = XML_SOURCE_DOCUMENT;
  f->identity = identity;
  f->entry_element_depth = 0;
  f->entry_binding_count = 0;
  f->version = p->version;
  save_active(p, f);
  p->source_count = 1;
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_source_push(xml_ctx * p, xml_source_kind kind,
    const void * identity, const void * bytes, size_t length) {
  if (p->source_count == 0 || !identity || (!bytes && length != 0)
      || (kind != XML_SOURCE_INTERNAL_ENTITY
          && kind != XML_SOURCE_EXTERNAL_ENTITY
          && kind != XML_SOURCE_PARAMETER_ENTITY
          && kind != XML_SOURCE_EXTERNAL_SUBSET)) {
    return xml_fail(p, GTEXT_XML_E_INVALID, "invalid XML source transition");
  }
  const int external = kind != XML_SOURCE_INTERNAL_ENTITY;
  const size_t max_expanded = p->max_expanded_bytes != 0
      ? p->max_expanded_bytes : XML_DEFAULT_MAX_EXPANDED_BYTES;
  const size_t max_entity_depth = p->max_entity_depth != 0
      ? p->max_entity_depth : XML_DEFAULT_MAX_ENTITY_DEPTH;
  const size_t max_source_bytes = p->max_external_source_bytes != 0
      ? p->max_external_source_bytes : XML_DEFAULT_MAX_EXTERNAL_SOURCE_BYTES;
  const size_t max_external = p->max_external_bytes != 0
      ? p->max_external_bytes : XML_DEFAULT_MAX_EXTERNAL_BYTES;
  if (external) {
    if (length > max_source_bytes) {
      return xml_fail(p, GTEXT_XML_E_LIMIT_EXTERNAL_SOURCE,
          "external source byte limit exceeded");
    }
  }
  const int entity = kind != XML_SOURCE_EXTERNAL_SUBSET;
  if (entity && p->entity_depth >= max_entity_depth) {
    return xml_fail(p, GTEXT_XML_E_LIMIT_ENTITY_DEPTH,
        "entity nesting limit exceeded");
  }
  if (p->source_count >= p->max_source_depth) {
    return xml_fail(p, GTEXT_XML_E_LIMIT_DEPTH, "XML source nesting limit exceeded");
  }
  for (size_t i = 0; i < p->source_count; i++) {
    if (identity && p->sources[i].identity == identity) {
      return xml_fail(p, GTEXT_XML_E_ENTITY,
          "recursive XML entity source identity");
    }
  }
  /* Preflight every byte ceiling before changing any ledger counter. */
  if (kind != XML_SOURCE_EXTERNAL_SUBSET
      && (p->expanded_bytes > max_expanded
          || length > max_expanded - p->expanded_bytes)) {
    return xml_fail(p, GTEXT_XML_E_LIMIT_EXPANSION,
        "entity expansion byte limit exceeded");
  }
  if (external
      && (p->external_bytes > max_external
          || length > max_external - p->external_bytes)) {
    return xml_fail(p, GTEXT_XML_E_LIMIT_EXTERNAL_TOTAL,
        "aggregate external byte limit exceeded");
  }
  GTEXT_XML_Status status;
  if (kind != XML_SOURCE_EXTERNAL_SUBSET) {
    status = xml_charge(p, &p->expanded_bytes, max_expanded, length,
        GTEXT_XML_E_LIMIT_EXPANSION, "entity expansion byte limit exceeded");
    if (status != GTEXT_XML_OK) return status;
  }
  if (external) {
    status = xml_charge(p, &p->external_bytes, max_external, length,
        GTEXT_XML_E_LIMIT_EXTERNAL_TOTAL, "aggregate external byte limit exceeded");
    if (status != GTEXT_XML_OK) return status;
  }
  status = source_reserve(p, p->source_count + 1);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  xml_source_frame * parent = &p->sources[p->source_count - 1];
  parent->version = p->version;
  save_active(p, parent);
  parent->suspended = 1;

  xml_source_frame * child = &p->sources[p->source_count];
  memset(child, 0, sizeof(*child));
  child->kind = kind;
  child->bytes_charged = external ? length : 0;
  child->identity = identity;
  child->entry_element_depth = p->frame_count;
  child->entry_binding_count = p->binding_count;
  child->grammar_context = XML_SOURCE_GRAMMAR_CONTENT;
  child->version = p->version;
  child->bytes = bytes ? bytes : kEmptySource;
  child->length = length;
  child->line = 1;
  child->column = 1;
  p->source_count++;
  if (entity) {
    p->entity_depth++;
  }

  p->in = child->bytes;
  p->length = length;
  p->pos = 0;
  p->base = 0;
  p->line = 1;
  p->column = 1;
  p->cursor_previous = 0;
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_source_charge_external(xml_ctx * p, size_t amount) {
  if (p->source_count <= 1) {
    return xml_fail(p, GTEXT_XML_E_STATE,
        "no external XML source is active");
  }
  xml_source_frame * f = &p->sources[p->source_count - 1];
  if (f->kind == XML_SOURCE_INTERNAL_ENTITY) {
    return xml_fail(p, GTEXT_XML_E_STATE,
        "the active XML source is not external");
  }
  const size_t source_limit = p->max_external_source_bytes != 0
      ? p->max_external_source_bytes : XML_DEFAULT_MAX_EXTERNAL_SOURCE_BYTES;
  if (f->bytes_charged > source_limit
      || amount > source_limit - f->bytes_charged) {
    return xml_fail(p, GTEXT_XML_E_LIMIT_EXTERNAL_SOURCE,
        "external source byte limit exceeded");
  }
  const size_t total_limit = p->max_external_bytes != 0
      ? p->max_external_bytes : XML_DEFAULT_MAX_EXTERNAL_BYTES;
  const size_t expanded_before = p->expanded_bytes;
  /* External entity bytes are also expansion output. External subsets are
     parsed as declarations and do not contribute to replacement text. */
  if (f->kind != XML_SOURCE_EXTERNAL_SUBSET) {
    const size_t expanded_limit = p->max_expanded_bytes != 0
        ? p->max_expanded_bytes : XML_DEFAULT_MAX_EXPANDED_BYTES;
    GTEXT_XML_Status expanded = xml_charge(p, &p->expanded_bytes,
        expanded_limit, amount, GTEXT_XML_E_LIMIT_EXPANSION,
        "entity expansion byte limit exceeded");
    if (expanded != GTEXT_XML_OK) {
      return expanded;
    }
  }
  GTEXT_XML_Status status = xml_charge(p, &p->external_bytes,
      total_limit, amount, GTEXT_XML_E_LIMIT_EXTERNAL_TOTAL,
      "aggregate external byte limit exceeded");
  if (status == GTEXT_XML_OK) {
    f->bytes_charged += amount;
  }
  else if (f->kind != XML_SOURCE_EXTERNAL_SUBSET) {
    p->expanded_bytes = expanded_before;
  }
  return status;
}

GTEXT_XML_Status xml_source_pop(xml_ctx * p) {
  if (p->source_count <= 1) {
    return xml_fail(p, GTEXT_XML_E_STATE, "cannot pop the XML document source");
  }
  const xml_source_frame * child = &p->sources[p->source_count - 1];
  if (p->frame_count != child->entry_element_depth
      || p->binding_count != child->entry_binding_count) {
    return xml_fail(p, GTEXT_XML_E_MISMATCHED_TAG,
        "element nesting crosses a physical XML source boundary");
  }

  p->source_count--;
  if (child->kind != XML_SOURCE_EXTERNAL_SUBSET && p->entity_depth > 0) {
    p->entity_depth--;
  }
  xml_source_frame * parent = &p->sources[p->source_count - 1];
  p->in = parent->bytes;
  p->length = parent->length;
  p->pos = parent->pos;
  p->base = parent->base;
  p->line = parent->line;
  p->column = parent->column;
  p->cursor_previous = parent->cursor_previous;
  p->version = parent->version;
  parent->suspended = 0;
  memset(&p->sources[p->source_count], 0, sizeof(*p->sources));
  return GTEXT_XML_OK;
}

void xml_source_free(xml_ctx * p) {
  xml_release(p, p->sources);
  p->sources = NULL;
  p->source_count = 0;
  p->source_capacity = 0;
}
