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

static GTEXT_XML_Status source_name_reserve(xml_ctx * p, size_t needed) {
  return xml_grow(p, (void **)&p->source_names, &p->source_name_capacity,
      sizeof(*p->source_names), needed);
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
  status = source_name_reserve(p, 1);
  if (status != GTEXT_XML_OK) return status;
  p->source_names[0] = NULL;
  p->source_name_count = 1;
  xml_source_frame * f = &p->sources[0];
  memset(f, 0, sizeof(*f));
  f->kind = XML_SOURCE_DOCUMENT;
  f->identity = identity;
  f->base_uri = p->base_uri;
  f->source_id = 1;
  p->next_source_id = 1;
  if (p->doc && markup_source_add(p->doc, 1, 0,
          GTEXT_MARKUP_SOURCE_DOCUMENT, NULL) != GTEXT_MARKUP_OK) {
    return xml_fail(p, GTEXT_XML_E_OOM, "out of memory recording XML source");
  }
  f->entry_element_depth = 0;
  f->entry_binding_count = 0;
  f->version = p->version;
  save_active(p, f);
  p->source_count = 1;
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_source_push(xml_ctx * p, xml_source_kind kind,
    const void * identity, const char * name, size_t name_length,
    const void * bytes, size_t length) {
  if (p->source_count == 0 || !identity || (!bytes && length != 0)
      || (kind != XML_SOURCE_INTERNAL_ENTITY
          && kind != XML_SOURCE_EXTERNAL_ENTITY
          && kind != XML_SOURCE_PARAMETER_ENTITY
          && kind != XML_SOURCE_INTERNAL_PARAMETER_ENTITY
          && kind != XML_SOURCE_EXTERNAL_SUBSET)) {
    return xml_fail(p, GTEXT_XML_E_INVALID, "invalid XML source transition");
  }
  const int external = kind == XML_SOURCE_EXTERNAL_ENTITY
      || kind == XML_SOURCE_PARAMETER_ENTITY
      || kind == XML_SOURCE_EXTERNAL_SUBSET;
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
  if (p->next_source_id == UINT32_MAX) {
    return xml_fail(p, GTEXT_XML_E_LIMIT_REFERENCES,
        "physical source identifier limit exceeded");
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
    status = xml_charge_expansion(p, length);
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
  const uint32_t source_id = p->next_source_id + 1;
  status = source_name_reserve(p, source_id);
  if (status != GTEXT_XML_OK) return status;
  char * owned_name = NULL;
  if (name) {
    owned_name = xml_alloc(p, name_length + 1);
    if (!owned_name) {
      return xml_fail(p, GTEXT_XML_E_OOM,
          "could not retain XML source name");
    }
    memcpy(owned_name, name, name_length);
    owned_name[name_length] = '\0';
  }
  if (name && p->doc) {
    const GTEXT_Markup_Source_Kind public_kind =
        kind == XML_SOURCE_INTERNAL_ENTITY ? GTEXT_MARKUP_SOURCE_INTERNAL_ENTITY
        : kind == XML_SOURCE_EXTERNAL_ENTITY ? GTEXT_MARKUP_SOURCE_EXTERNAL_ENTITY
        : (kind == XML_SOURCE_PARAMETER_ENTITY
            || kind == XML_SOURCE_INTERNAL_PARAMETER_ENTITY)
            ? GTEXT_MARKUP_SOURCE_PARAMETER_ENTITY
        : GTEXT_MARKUP_SOURCE_EXTERNAL_SUBSET;
    const GTEXT_Markup_Status recorded = markup_source_add(p->doc, source_id,
        p->sources[p->source_count - 1].source_id, public_kind, owned_name);
    if (recorded != GTEXT_MARKUP_OK) {
      xml_release(p, owned_name);
      return xml_fail(p, recorded == GTEXT_MARKUP_E_OOM
          ? GTEXT_XML_E_OOM : GTEXT_XML_E_LIMIT_REFERENCES,
          "could not retain XML source descriptor");
    }
  }
  p->source_names[source_id - 1] = owned_name;
  if (p->source_name_count < source_id) p->source_name_count = source_id;
  xml_source_frame * parent = &p->sources[p->source_count - 1];
  parent->version = p->version;
  save_active(p, parent);
  parent->suspended = 1;

  xml_source_frame * child = &p->sources[p->source_count];
  memset(child, 0, sizeof(*child));
  child->kind = kind;
  child->external_source = (unsigned char)external;
  child->bytes_charged = external ? length : 0;
  child->identity = identity;
  child->base_uri = parent->base_uri;
  child->owned_name = owned_name;
  child->source_id = ++p->next_source_id;
  child->reference_source_id = parent->source_id;
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
  const size_t total_limit = p->max_external_bytes != 0
      ? p->max_external_bytes : XML_DEFAULT_MAX_EXTERNAL_BYTES;
  const int source_exceeded = f->bytes_charged > source_limit
      || amount > source_limit - f->bytes_charged;
  const int total_exceeded = p->external_bytes > total_limit
      || amount > total_limit - p->external_bytes;
  /* A chunk has already left the caller's source when this function runs.
     Record even a rejected chunk, saturating counters rather than wrapping. */
  f->bytes_charged = amount > SIZE_MAX - f->bytes_charged
      ? SIZE_MAX : f->bytes_charged + amount;
  p->external_bytes = amount > SIZE_MAX - p->external_bytes
      ? SIZE_MAX : p->external_bytes + amount;
  /* Parameter-entity bytes count as replacement output as they are read.
     General entities are charged once their decoded body and text declaration
     boundary are known; external subsets are declarations, not replacement. */
  if (f->kind != XML_SOURCE_EXTERNAL_SUBSET
      && f->kind != XML_SOURCE_EXTERNAL_ENTITY) {
    const size_t expanded_limit = p->max_expanded_bytes != 0
        ? p->max_expanded_bytes : XML_DEFAULT_MAX_EXPANDED_BYTES;
    const int expanded_exceeded = p->expanded_bytes > expanded_limit
        || amount > expanded_limit - p->expanded_bytes;
    p->expanded_bytes = amount > SIZE_MAX - p->expanded_bytes
        ? SIZE_MAX : p->expanded_bytes + amount;
    if (expanded_exceeded) {
      return xml_fail(p, GTEXT_XML_E_LIMIT_EXPANSION,
          "entity expansion byte limit exceeded");
    }
  }
  if (source_exceeded) {
    return xml_fail(p, GTEXT_XML_E_LIMIT_EXTERNAL_SOURCE,
        "external source byte limit exceeded");
  }
  if (total_exceeded) {
    return xml_fail(p, GTEXT_XML_E_LIMIT_EXTERNAL_TOTAL,
        "aggregate external byte limit exceeded");
  }
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_source_pop(xml_ctx * p) {
  if (p->source_count <= 1) {
    return xml_fail(p, GTEXT_XML_E_STATE, "cannot pop the XML document source");
  }
  xml_source_frame * child = &p->sources[p->source_count - 1];
  if (p->frame_count != child->entry_element_depth
      || p->binding_count != child->entry_binding_count) {
    return xml_fail(p, GTEXT_XML_E_MISMATCHED_TAG,
        "element nesting crosses a physical XML source boundary");
  }

  if (child->external_close) {
    p->resolver_reentered = 0;
    p->resolver_callback_active = 1;
    child->external_close(child->external_handle);
    p->resolver_callback_active = 0;
    child->external_close = NULL;
    child->external_handle = NULL;
    if (p->resolver_reentered)
      return xml_fail(p, GTEXT_XML_E_STATE,
          "resolver callback reentered the active XML parser");
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
  xml_release(p, child->owned_bytes);
  xml_release(p, child->owned_decoded);
  xml_release(p, child->owned_base_uri);
  memset(&p->sources[p->source_count], 0, sizeof(*p->sources));
  return GTEXT_XML_OK;
}

void xml_source_free(xml_ctx * p) {
  xml_release(p, p->dtd_name_token);
  p->dtd_name_token = NULL;
  p->dtd_name_token_length = p->dtd_name_token_capacity = 0;
  xml_release(p, p->dtd_external_modes);
  p->dtd_external_modes = NULL;
  p->dtd_external_depth = p->dtd_external_capacity = 0;
  p->dtd_external_active = 0;
  for (size_t i = 1; i < p->source_count; ++i) {
    xml_source_frame * f = &p->sources[i];
    if (f->external_close) f->external_close(f->external_handle);
    xml_release(p, f->owned_bytes);
    xml_release(p, f->owned_decoded);
    xml_release(p, f->owned_base_uri);
  }
  for (size_t i = 0; i < p->parameter_entity_count; i++) {
    xml_release(p, p->parameter_entities[i].name);
    xml_release(p, p->parameter_entities[i].replacement);
    xml_release(p, p->parameter_entities[i].public_id);
    xml_release(p, p->parameter_entities[i].system_id);
  }
  for (size_t i = 0; i < p->general_entity_count; i++) {
    xml_general_entity * item = &p->general_entities[i];
    xml_release(p, item->name); xml_release(p, item->replacement);
    xml_release(p, item->public_id); xml_release(p, item->system_id);
    xml_release(p, item->base_uri);
    xml_release(p, item->notation_name);
  }
  xml_release(p, p->general_entities);
  p->general_entities = NULL;
  p->general_entity_count = p->general_entity_capacity = 0;
  xml_name_index_free(p, &p->general_entity_index);
  xml_release(p, p->parameter_entities);
  p->parameter_entities = NULL;
  p->parameter_entity_count = 0;
  p->parameter_entity_capacity = 0;
  xml_name_index_free(p, &p->parameter_entity_index);
  for (size_t i = 0; i < p->dtd_binding_count; ++i) {
    xml_release(p, p->dtd_bindings[i].element_name);
    xml_release(p, p->dtd_bindings[i].name);
    xml_release(p, p->dtd_bindings[i].attribute_default_value);
    xml_release(p, p->dtd_bindings[i].attribute_enumeration);
  }
  xml_release(p, p->dtd_bindings);
  xml_validation_memory_release(p, p->validation_dtd_key_bytes);
  p->validation_dtd_key_bytes = 0;
  p->dtd_bindings = NULL;
  p->dtd_binding_count = p->dtd_binding_capacity = 0;
  xml_name_index_free(p, &p->dtd_binding_index);
  for (size_t i = 0; i < p->source_name_count; i++) {
    xml_release(p, p->source_names[i]);
  }
  xml_release(p, p->source_names);
  p->source_names = NULL;
  p->source_name_count = 0;
  p->source_name_capacity = 0;
  xml_release(p, p->sources);
  p->sources = NULL;
  p->source_count = 0;
  p->source_capacity = 0;
}

static GTEXT_XML_Status record_unread_resource(xml_ctx * p,
    GTEXT_XML_Resource_Kind kind, const char * name, const char * system_id) {
  p->dtd_knowledge_incomplete = 1;
  const GTEXT_Markup_Location at = xml_here(p);
  if (p->doc) {
    GTEXT_Markup_DTD_Resource_Kind markup_kind;
    switch (kind) {
      case GTEXT_XML_RESOURCE_EXTERNAL_SUBSET:
        markup_kind = GTEXT_MARKUP_DTD_RESOURCE_EXTERNAL_SUBSET;
        break;
      case GTEXT_XML_RESOURCE_PARAMETER_ENTITY:
        markup_kind = GTEXT_MARKUP_DTD_RESOURCE_PARAMETER_ENTITY;
        break;
      case GTEXT_XML_RESOURCE_GENERAL_ENTITY:
        markup_kind = GTEXT_MARKUP_DTD_RESOURCE_GENERAL_ENTITY;
        break;
      default:
        return xml_fail(p, GTEXT_XML_E_INVALID,
            "invalid unread external resource kind");
    }
    const GTEXT_Markup_Status recorded = markup_document_add_unread_resource(
        p->doc, markup_kind, name, system_id, &at);
    if (recorded != GTEXT_MARKUP_OK) return xml_from_markup(p, recorded);
  }
  if (p->stream)
    return xml_emit_unread_resource(p, &at, kind, name, system_id);
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_resolve_external_resource(xml_ctx * p,
    GTEXT_XML_Resource_Kind resource_kind, xml_source_kind source_kind,
    const char * source_name, const char * public_id, const char * system_id,
    const char * declaration_base_uri, xml_resolved_resource * out) {
  if (!p || !out || (source_kind != XML_SOURCE_EXTERNAL_SUBSET
          && source_kind != XML_SOURCE_PARAMETER_ENTITY
          && source_kind != XML_SOURCE_EXTERNAL_ENTITY))
    return xml_fail(p, GTEXT_XML_E_INVALID, "invalid external resource request");
  memset(out, 0, sizeof(*out));
  out->source_kind = source_kind;
  markup_buf_init(&out->bytes, p->allocator);
  if (p->dtd_mode == GTEXT_XML_DTD_OFF || !system_id) return GTEXT_XML_OK;
  if (p->external_access != GTEXT_XML_EXTERNAL_ACCESS_ALLOW || !p->resolver) {
    out->declined = 1;
    return record_unread_resource(p, resource_kind, source_name, system_id);
  }
  if (p->resolver_callback_active) {
    p->resolver_reentered = 1;
    return xml_fail(p, GTEXT_XML_E_STATE,
        "resolver callback reentered the active XML parser");
  }

  /* Keep resolver ancestry available while the callback runs. Like event
     provenance, the public trail retains the nearest 32 physical sources. */
  const char * reference_trail[32];
  size_t reference_trail_count = 0;
  const size_t first_trail_frame = p->source_count > 33
      ? p->source_count - 32 : 1;
  for (size_t i = first_trail_frame; i < p->source_count; ++i) {
    const uint32_t source_id = p->sources[i].source_id;
    if (source_id == 0 || source_id > p->source_name_count) continue;
    const char * label = p->source_names[source_id - 1];
    if (label) reference_trail[reference_trail_count++] = label;
  }
  const GTEXT_XML_Resolve_Request request = {
    .kind = resource_kind,
    .public_id = public_id,
    .system_id = system_id,
    .base_uri = declaration_base_uri ? declaration_base_uri
        : p->source_count != 0 ? p->sources[p->source_count - 1].base_uri
                               : p->base_uri,
    .reference_trail = reference_trail_count ? reference_trail : NULL,
    .reference_trail_count = reference_trail_count
  };
  GTEXT_XML_External_Source * source = &out->source;
  p->resolver_callback_active = 1;
  const GTEXT_XML_Resolver_Result opened = p->resolver(&request,
      p->resolver_user_data, source);
  p->resolver_callback_active = 0;
  if (p->resolver_reentered) {
    /* OPENED transfers a close obligation even when the callback also tried
       to reenter the parser before returning. */
    if (opened == GTEXT_XML_RESOLVER_OPENED && source->close) {
      p->resolver_reentered = 0;
      p->resolver_callback_active = 1;
      source->close(source->handle);
      p->resolver_callback_active = 0;
    }
    return xml_fail(p, GTEXT_XML_E_STATE,
        "resolver callback reentered the active XML parser");
  }
  if (opened == GTEXT_XML_RESOLVER_DECLINED) {
    out->declined = 1;
    return record_unread_resource(p, resource_kind, source_name, system_id);
  }
  if (opened != GTEXT_XML_RESOLVER_OPENED) {
    return xml_fail(p, GTEXT_XML_E_RESOLVER,
        "external resource resolver failed");
  }

  out->opened = 1;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  static const unsigned char empty[1] = {0};
  if (!source->close) {
    status = xml_fail(p, GTEXT_XML_E_RESOLVER,
        "opened external source has no close callback");
    goto fail;
  }
  if (!source->identity || !source->base_uri || !source->read) {
    status = xml_fail(p, GTEXT_XML_E_RESOLVER,
        "opened external source is missing identity, base URI or read callback");
    goto fail;
  }
  const char * retained_name = source_name ? source_name : source->base_uri;
  status = xml_source_push(p, source_kind, source->identity, retained_name,
      strlen(retained_name), empty, 0);
  if (status != GTEXT_XML_OK) {
    goto fail;
  }
  out->frame_pushed = 1;
  const size_t base_uri_length = strlen(source->base_uri);
  char * retained_base_uri = xml_alloc(p, base_uri_length + 1);
  if (!retained_base_uri) {
    status = xml_fail(p, GTEXT_XML_E_OOM,
        "could not retain external source base URI");
    goto fail;
  }
  memcpy(retained_base_uri, source->base_uri, base_uri_length + 1);
  p->sources[p->source_count - 1].owned_base_uri = retained_base_uri;
  p->sources[p->source_count - 1].base_uri = retained_base_uri;

  for (;;) {
    unsigned char chunk[4096];
    size_t count = 0;
    p->resolver_reentered = 0;
    p->resolver_callback_active = 1;
    const GTEXT_XML_Source_Read_Result result = source->read(source->handle,
        chunk, sizeof(chunk), &count);
    p->resolver_callback_active = 0;
    if (p->resolver_reentered) {
      status = xml_fail(p, GTEXT_XML_E_STATE,
          "resolver callback reentered the active XML parser");
      break;
    }
    if (count > sizeof(chunk)) {
      status = xml_fail(p, GTEXT_XML_E_RESOLVER,
          "external source returned more bytes than requested");
      break;
    }
    if (count != 0) {
      status = xml_source_charge_external(p, count);
      if (status != GTEXT_XML_OK) {
        break;
      }
    }
    if (result == GTEXT_XML_SOURCE_EOF) {
      if (count != 0) {
        status = xml_fail(p, GTEXT_XML_E_RESOLVER,
            "external source returned bytes with EOF");
      }
      break;
    }
    if (result == GTEXT_XML_SOURCE_CANCELLED) {
      status = xml_fail(p, GTEXT_XML_E_CANCELLED,
          "external resource read cancelled");
      break;
    }
    if (result == GTEXT_XML_SOURCE_ERROR) {
      status = xml_fail(p, GTEXT_XML_E_RESOLVER,
          "external resource read failed");
      break;
    }
    if (result != GTEXT_XML_SOURCE_DATA || count == 0) {
      status = xml_fail(p, GTEXT_XML_E_RESOLVER,
          "external source returned an invalid read result");
      break;
    }
    status = xml_charge_work(p, count);
    if (status != GTEXT_XML_OK) {
      break;
    }
    if (markup_buf_append(&out->bytes, (const char *)chunk, count)
        != GTEXT_MARKUP_OK) {
      status = xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
      break;
    }
  }
  if (status != GTEXT_XML_OK) return status;
  return GTEXT_XML_OK;

fail:
  return status;
}

GTEXT_XML_Status xml_finish_external_resource(xml_ctx * p,
    xml_resolved_resource * resource, GTEXT_XML_Status status) {
  if (!p || !resource) return GTEXT_XML_E_INVALID;
  if (resource->frame_pushed && p->source_count > 1
      && p->sources[p->source_count - 1].identity == resource->source.identity) {
    const GTEXT_XML_Status popped = xml_source_pop(p);
    if (status == GTEXT_XML_OK) status = popped;
  }
  resource->frame_pushed = 0;
  if (resource->opened && resource->source.close) {
    p->resolver_reentered = 0;
    p->resolver_callback_active = 1;
    resource->source.close(resource->source.handle);
    p->resolver_callback_active = 0;
    if (p->resolver_reentered && status == GTEXT_XML_OK) {
      status = xml_fail(p, GTEXT_XML_E_STATE,
          "resolver callback reentered the active XML parser");
    }
  }
  resource->opened = 0;
  markup_buf_free(&resource->bytes);
  return status;
}

GTEXT_XML_Status xml_resolve_external_subset(xml_ctx * p,
    GTEXT_Markup_Node * doctype, const char * public_id,
    const char * system_id) {
  xml_resolved_resource resource;
  GTEXT_XML_Status status = xml_resolve_external_resource(p,
      GTEXT_XML_RESOURCE_EXTERNAL_SUBSET, XML_SOURCE_EXTERNAL_SUBSET,
      NULL, public_id, system_id, NULL, &resource);
  if (status == GTEXT_XML_OK && resource.opened) {
    static const unsigned char empty[1] = {0};
    status = xml_parse_external_subset(p, doctype,
        resource.bytes.data ? (const unsigned char *)resource.bytes.data : empty,
        resource.bytes.length, resource.source.base_uri);
    if (status == GTEXT_XML_OK && p->dtd_yielded) {
      xml_source_frame * frame = NULL;
      for (size_t i = 1; i < p->source_count; ++i) {
        if (p->sources[i].kind == XML_SOURCE_EXTERNAL_SUBSET
            && p->sources[i].identity == resource.source.identity) {
          frame = &p->sources[i];
          break;
        }
      }
      if (!frame) {
        status = xml_fail(p, GTEXT_XML_E_STATE,
            "resolved external subset source frame was lost");
        return xml_finish_external_resource(p, &resource, status);
      }
      frame->owned_bytes = resource.bytes.data;
      resource.bytes.data = NULL;
      resource.bytes.length = resource.bytes.capacity = 0;
      frame->external_close = resource.source.close;
      frame->external_handle = resource.source.handle;
      resource.source.close = NULL;
      resource.opened = 0;
      resource.frame_pushed = 0;
      return GTEXT_XML_OK;
    }
  }
  return xml_finish_external_resource(p, &resource, status);
}
