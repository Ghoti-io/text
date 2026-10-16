/*
 * Shared by the XML stream tests and the conformance runner: build a tree out
 * of events, and say whether two trees are the same.
 *
 * Plain C that is also C++, because both include it. "The same" is as strict as
 * the tree can be asked: kinds, names, namespaces, attribute order and values,
 * data, the document's version, encoding and how it was decided, and the
 * location of every node and attribute, so an event parser that builds the right
 * tree from the wrong positions is not accepted. The locations of ELEMENT_END,
 * DOCUMENT_START and DOCUMENT_END events are not in a tree and are not compared
 * here; the hand-written event-log tests check them.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GTEXT_TESTS_XML_REPLAY_H
#define GHOTI_IO_GTEXT_TESTS_XML_REPLAY_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/xml.h>

#include "../src/markup/markup_internal.h"

typedef struct {
  GTEXT_Markup_Document * doc;
  GTEXT_Markup_Node * current;
  int ended;
  int events;
  const char * problem; /* what went wrong building, or NULL */
} xml_replay;

static void xml_replay_init(xml_replay * r) {
  memset(r, 0, sizeof(*r));
}

static void xml_replay_free(xml_replay * r) {
  if (r->doc) {
    gtext_markup_document_free(r->doc);
  }
  memset(r, 0, sizeof(*r));
}

static GTEXT_XML_Status xml_replay_fail(xml_replay * r, const char * why) {
  if (!r->problem) {
    r->problem = why;
  }
  return GTEXT_XML_E_SYNTAX;
}

static GTEXT_XML_Status xml_replay_attach(xml_replay * r, GTEXT_Markup_Node * node,
    const GTEXT_Markup_Location * where) {
  gtext_markup_node_set_location(node, where);
  if (gtext_markup_append_child(r->current, node) != GTEXT_MARKUP_OK) {
    gtext_markup_node_free(node);
    return xml_replay_fail(r, "the tree refused a node");
  }
  return GTEXT_XML_OK;
}

static int xml_replay_same_text(const char * a, const char * b);

/* The callback: GTEXT_XML_Event_cb, with an xml_replay as the user data. */
static GTEXT_XML_Status xml_replay_event(const GTEXT_XML_Event * e, void * user) {
  xml_replay * r = (xml_replay *)user;
  GTEXT_Markup_Node * node = NULL;
  r->events++;
  if (r->ended) {
    return xml_replay_fail(r, "an event after DOCUMENT_END");
  }
  if (e->type == GTEXT_XML_EVENT_DOCUMENT_START) {
    if (r->doc) {
      return xml_replay_fail(r, "a second DOCUMENT_START");
    }
    GTEXT_Markup_Options options = gtext_markup_options_default();
    options.max_depth = 0;
    options.max_nodes = 0;
    options.max_attributes = 0;
    if (gtext_markup_document_new(&options, &r->doc) != GTEXT_MARKUP_OK) {
      return GTEXT_XML_E_OOM;
    }
    r->current = gtext_markup_document_root(r->doc);
    if (e->version
        && gtext_markup_document_set_xml_declaration(r->doc, e->version, e->standalone)
            != GTEXT_MARKUP_OK) {
      return xml_replay_fail(r, "set_xml_declaration");
    }
    if (gtext_markup_document_set_encoding(r->doc, e->encoding, e->encoding_source)
        != GTEXT_MARKUP_OK) {
      return xml_replay_fail(r, "set_encoding");
    }
    return GTEXT_XML_OK;
  }
  if (!r->doc) {
    return xml_replay_fail(r, "an event before DOCUMENT_START");
  }
  switch (e->type) {
    case GTEXT_XML_EVENT_DOCUMENT_START:
      break;
    case GTEXT_XML_EVENT_DOCUMENT_END:
      if (r->current != gtext_markup_document_root(r->doc)) {
        return xml_replay_fail(r, "DOCUMENT_END with an element open");
      }
      r->ended = 1;
      return GTEXT_XML_OK;
    case GTEXT_XML_EVENT_DOCTYPE:
      if (gtext_markup_create_doctype(
              r->doc, e->name, e->public_id, e->system_id, &node)
          != GTEXT_MARKUP_OK) {
        return xml_replay_fail(r, "create_doctype");
      }
      return xml_replay_attach(r, node, &e->location);
    case GTEXT_XML_EVENT_ELEMENT_START: {
      if (gtext_markup_create_element(
              r->doc, e->namespace_uri, e->prefix, e->local_name, &node)
          != GTEXT_MARKUP_OK) {
        return xml_replay_fail(r, "create_element");
      }
      for (size_t i = 0; i < e->attribute_count; i++) {
        const GTEXT_XML_Event_Attribute * a = &e->attributes[i];
        if (gtext_markup_element_set_attribute(node, a->namespace_uri, a->prefix,
                a->local_name, a->value, a->value_length)
                != GTEXT_MARKUP_OK
            || gtext_markup_element_set_attribute_location(node, i, &a->location)
                != GTEXT_MARKUP_OK) {
          gtext_markup_node_free(node);
          return xml_replay_fail(r, "set_attribute");
        }
      }
      const GTEXT_XML_Status status = xml_replay_attach(r, node, &e->location);
      if (status == GTEXT_XML_OK) {
        r->current = node;
      }
      return status;
    }
    case GTEXT_XML_EVENT_ELEMENT_END: {
      GTEXT_Markup_Node * parent = gtext_markup_node_parent(r->current);
      if (gtext_markup_node_kind(r->current) != GTEXT_MARKUP_KIND_ELEMENT || !parent) {
        return xml_replay_fail(r, "ELEMENT_END with no element open");
      }
      if (strcmp(gtext_markup_element_local_name(r->current), e->local_name) != 0
          || !xml_replay_same_text(gtext_markup_element_prefix(r->current), e->prefix)
          || !xml_replay_same_text(
              gtext_markup_element_namespace(r->current), e->namespace_uri)) {
        return xml_replay_fail(r, "ELEMENT_END names another element");
      }
      r->current = parent;
      return GTEXT_XML_OK;
    }
    case GTEXT_XML_EVENT_TEXT:
      {
        GTEXT_Markup_Node * previous =
            gtext_markup_node_last_child(r->current);
        if (previous && gtext_markup_node_kind(previous)
                == GTEXT_MARKUP_KIND_TEXT) {
          size_t old_length = 0;
          const char * old_data = gtext_markup_node_data(previous, &old_length);
          if (old_length == SIZE_MAX
              || e->data_length > SIZE_MAX - old_length - 1)
            return xml_replay_fail(r, "text size overflow");
          char * joined = (char *)malloc(old_length + e->data_length + 1);
          if (!joined) return xml_replay_fail(r, "allocate combined text");
          memcpy(joined, old_data, old_length);
          memcpy(joined + old_length, e->data, e->data_length);
          joined[old_length + e->data_length] = '\0';
          const GTEXT_Markup_Status append_status = gtext_markup_node_set_data(
              previous, joined, old_length + e->data_length);
          free(joined);
          return append_status == GTEXT_MARKUP_OK ? GTEXT_XML_OK
              : xml_replay_fail(r, "append text");
        }
      }
      if (gtext_markup_create_text(r->doc, e->data, e->data_length, &node)
          != GTEXT_MARKUP_OK) {
        return xml_replay_fail(r, "create_text");
      }
      return xml_replay_attach(r, node, &e->location);
    case GTEXT_XML_EVENT_CDATA:
      if (gtext_markup_create_cdata(r->doc, e->data, e->data_length, &node)
          != GTEXT_MARKUP_OK) {
        return xml_replay_fail(r, "create_cdata");
      }
      return xml_replay_attach(r, node, &e->location);
    case GTEXT_XML_EVENT_COMMENT:
      if (gtext_markup_create_comment(r->doc, e->data, e->data_length, &node)
          != GTEXT_MARKUP_OK) {
        return xml_replay_fail(r, "create_comment");
      }
      return xml_replay_attach(r, node, &e->location);
    case GTEXT_XML_EVENT_PROCESSING_INSTRUCTION:
      if (gtext_markup_create_processing_instruction(
              r->doc, e->name, e->data, e->data_length, &node)
          != GTEXT_MARKUP_OK) {
        return xml_replay_fail(r, "create_processing_instruction");
      }
      return xml_replay_attach(r, node, &e->location);
    case GTEXT_XML_EVENT_SKIPPED_ENTITY:
      if (gtext_markup_create_entity_reference(r->doc, e->name,
              strlen(e->name), &node) != GTEXT_MARKUP_OK) {
        return xml_replay_fail(r, "create_entity_reference");
      }
      return xml_replay_attach(r, node, &e->location);
    case GTEXT_XML_EVENT_UNREAD_RESOURCE:
      /* This event records parser knowledge, not a tree node. Retain it on the
         replay tree so stream-vs-DOM comparisons include incomplete DTD state. */
      if (!r->doc) return xml_replay_fail(r, "UNREAD_RESOURCE before document");
      {
        GTEXT_Markup_DTD_Resource_Kind kind;
        switch (e->resource_kind) {
          case GTEXT_XML_RESOURCE_EXTERNAL_SUBSET:
            kind = GTEXT_MARKUP_DTD_RESOURCE_EXTERNAL_SUBSET;
            break;
          case GTEXT_XML_RESOURCE_PARAMETER_ENTITY:
            kind = GTEXT_MARKUP_DTD_RESOURCE_PARAMETER_ENTITY;
            break;
          case GTEXT_XML_RESOURCE_GENERAL_ENTITY:
            kind = GTEXT_MARKUP_DTD_RESOURCE_GENERAL_ENTITY;
            break;
          default:
            return xml_replay_fail(r, "unknown unread resource kind");
        }
        if (markup_document_add_unread_resource(r->doc, kind, e->name,
                e->system_id, &e->location) != GTEXT_MARKUP_OK) {
          return xml_replay_fail(r, "retain unread resource");
        }
      }
      return GTEXT_XML_OK;
    case GTEXT_XML_EVENT_VALIDATION_DIAGNOSTIC:
    case GTEXT_XML_EVENT_VALIDATION_DIAGNOSTICS_TRUNCATED:
      /* Validity evidence is compared by dedicated validator tests; it is not
         part of the replayed markup tree. */
      return GTEXT_XML_OK;
  }
  return xml_replay_fail(r, "an unknown event type");
}

static int xml_replay_same_text(const char * a, const char * b) {
  if (!a || !b) {
    return a == b;
  }
  return strcmp(a, b) == 0;
}

static int xml_replay_same_location(
    const GTEXT_Markup_Node * a, const GTEXT_Markup_Node * b) {
  GTEXT_Markup_Location la;
  GTEXT_Markup_Location lb;
  const int ha = gtext_markup_node_location(a, &la);
  const int hb = gtext_markup_node_location(b, &lb);
  if (ha != hb) {
    return 0;
  }
  return !ha || (la.offset == lb.offset && la.line == lb.line && la.column == lb.column);
}

/* NULL if the two documents are the same; otherwise what differs. */
static const char * xml_replay_compare(
    const GTEXT_Markup_Document * x, const GTEXT_Markup_Document * y) {
  GTEXT_Markup_Standalone sx;
  GTEXT_Markup_Standalone sy;
  GTEXT_Markup_Encoding_Source ex;
  GTEXT_Markup_Encoding_Source ey;
  if (!xml_replay_same_text(gtext_markup_document_xml_version(x, &sx),
          gtext_markup_document_xml_version(y, &sy))
      || sx != sy) {
    return "XML declaration";
  }
  if (!xml_replay_same_text(gtext_markup_document_encoding(x, &ex),
          gtext_markup_document_encoding(y, &ey))
      || ex != ey) {
    return "encoding";
  }
  if (gtext_markup_document_dtd_knowledge_incomplete(x)
      != gtext_markup_document_dtd_knowledge_incomplete(y)) {
    return "DTD knowledge completeness";
  }
  const size_t unread_count = gtext_markup_document_dtd_unread_resource_count(x);
  if (unread_count != gtext_markup_document_dtd_unread_resource_count(y)) {
    return "unread DTD resource count";
  }
  for (size_t i = 0; i < unread_count; i++) {
    GTEXT_Markup_DTD_Unread_Resource ux;
    GTEXT_Markup_DTD_Unread_Resource uy;
    if (!gtext_markup_document_dtd_unread_resource(x, i, &ux)
        || !gtext_markup_document_dtd_unread_resource(y, i, &uy)
        || ux.kind != uy.kind
        || !xml_replay_same_text(ux.name, uy.name)
        || !xml_replay_same_text(ux.system_id, uy.system_id)
        || ux.location.offset != uy.location.offset
        || ux.location.line != uy.location.line
        || ux.location.column != uy.location.column
        || ux.location.source_id != uy.location.source_id
        || ux.location.reference_source_id != uy.location.reference_source_id) {
      return "unread DTD resource";
    }
  }
  const GTEXT_Markup_Node * rx = gtext_markup_document_root(x);
  const GTEXT_Markup_Node * ry = gtext_markup_document_root(y);
  const GTEXT_Markup_Node * a = rx;
  const GTEXT_Markup_Node * b = ry;
  while (a && b) {
    if (gtext_markup_node_kind(a) != gtext_markup_node_kind(b)) {
      return "node kind";
    }
    if (!xml_replay_same_location(a, b)) {
      return "node location";
    }
    switch (gtext_markup_node_kind(a)) {
      case GTEXT_MARKUP_KIND_ELEMENT: {
        if (!xml_replay_same_text(gtext_markup_element_namespace(a),
                gtext_markup_element_namespace(b))
            || !xml_replay_same_text(
                gtext_markup_element_prefix(a), gtext_markup_element_prefix(b))
            || !xml_replay_same_text(gtext_markup_element_local_name(a),
                gtext_markup_element_local_name(b))) {
          return "element name";
        }
        const size_t n = gtext_markup_element_attribute_count(a);
        if (n != gtext_markup_element_attribute_count(b)) {
          return "attribute count";
        }
        for (size_t i = 0; i < n; i++) {
          GTEXT_Markup_Attribute aa;
          GTEXT_Markup_Attribute ab;
          GTEXT_Markup_Location la;
          GTEXT_Markup_Location lb;
          gtext_markup_element_attribute_at(a, i, &aa);
          gtext_markup_element_attribute_at(b, i, &ab);
          if (!xml_replay_same_text(aa.ns, ab.ns)
              || !xml_replay_same_text(aa.prefix, ab.prefix)
              || !xml_replay_same_text(aa.local, ab.local)
              || aa.value_length != ab.value_length
              || memcmp(aa.value, ab.value, aa.value_length) != 0) {
            return "attribute";
          }
          const int ha = gtext_markup_element_attribute_location(a, i, &la);
          const int hb = gtext_markup_element_attribute_location(b, i, &lb);
          if (ha != hb
              || (ha
                  && (la.offset != lb.offset || la.line != lb.line
                      || la.column != lb.column))) {
            return "attribute location";
          }
        }
        break;
      }
      case GTEXT_MARKUP_KIND_TEXT:
      case GTEXT_MARKUP_KIND_CDATA:
      case GTEXT_MARKUP_KIND_COMMENT:
      case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
        size_t la = 0;
        size_t lb = 0;
        const char * da = gtext_markup_node_data(a, &la);
        const char * db = gtext_markup_node_data(b, &lb);
        if (la != lb || (la && memcmp(da, db, la) != 0)) {
          return "node data";
        }
        if (gtext_markup_node_kind(a) == GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION
            && !xml_replay_same_text(
                gtext_markup_pi_target(a), gtext_markup_pi_target(b))) {
          return "processing instruction target";
        }
        break;
      }
      case GTEXT_MARKUP_KIND_DOCTYPE:
        if (!xml_replay_same_text(
                gtext_markup_doctype_name(a), gtext_markup_doctype_name(b))
            || !xml_replay_same_text(gtext_markup_doctype_public_id(a),
                gtext_markup_doctype_public_id(b))
            || !xml_replay_same_text(gtext_markup_doctype_system_id(a),
                gtext_markup_doctype_system_id(b))) {
          return "doctype";
        }
        break;
      default:
        break;
    }
    a = gtext_markup_node_next_in_order(rx, a, false);
    b = gtext_markup_node_next_in_order(ry, b, false);
  }
  if (a || b) {
    return "node count";
  }
  return NULL;
}

#endif // GHOTI_IO_GTEXT_TESTS_XML_REPLAY_H
