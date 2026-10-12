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
 * The markup tree: documents, node creation, reading, attributes and the
 * operations that change the shape of the tree.
 *
 * Every operation that can fail decides whether it will before it changes
 * anything, or undoes what it did. The insertion functions are the clearest
 * case: all of the rules are checked, and every limit, before the first link
 * moves, and the moves themselves allocate nothing - so a failed insertion has
 * nothing to roll back.
 */

#include <ghoti.io/text/macros.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "markup_internal.h"

/* The byte `cap_log2` is in the padding after `flags`, not a new field: the 32-bit `line` that follows
   stays at offset 4, so no node got bigger. */
_Static_assert(offsetof(struct GTEXT_Markup_Node, cap_log2) == 2 && offsetof(struct GTEXT_Markup_Node, line) == 4,
    "cap_log2 must fit in the padding between flags and line");

typedef struct GTEXT_Markup_Node node_t;

/* ---- the detached list -------------------------------------------------- */

static void detached_push(GTEXT_Markup_Document * doc, node_t * node) {
  node->prev = NULL;
  node->next = doc->detached;
  if (doc->detached) {
    doc->detached->prev = node;
  }
  doc->detached = node;
}

static void detached_remove(GTEXT_Markup_Document * doc, node_t * node) {
  if (node->prev) {
    node->prev->next = node->next;
  }
  else {
    doc->detached = node->next;
  }
  if (node->next) {
    node->next->prev = node->prev;
  }
  node->prev = NULL;
  node->next = NULL;
}

/* ---- allocating and freeing one node ------------------------------------ */

static size_t size_of_kind(GTEXT_Markup_Kind kind) {
  switch (kind) {
    case GTEXT_MARKUP_KIND_ELEMENT:
      return sizeof(markup_element);
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
    case GTEXT_MARKUP_KIND_COMMENT:
      return sizeof(markup_text);
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
      return sizeof(markup_pi);
    case GTEXT_MARKUP_KIND_DOCTYPE:
      return sizeof(markup_doctype);
    case GTEXT_MARKUP_KIND_DOCUMENT:
    case GTEXT_MARKUP_KIND_FRAGMENT:
    case GTEXT_MARKUP_KIND_NONE:
      break;
  }
  return sizeof(node_t);
}

/* A zeroed node of this kind, counted against the limit and on the detached
   list - except the root, which is the document's own and is on no list. */
static GTEXT_Markup_Status node_new(GTEXT_Markup_Document * doc,
    GTEXT_Markup_Kind kind, node_t ** out) {
  if (doc->options.max_nodes != 0 && doc->node_count >= doc->options.max_nodes) {
    return GTEXT_MARKUP_E_LIMIT;
  }
  node_t * node = markup_alloc(doc, size_of_kind(kind));
  if (!node) {
    return GTEXT_MARKUP_E_OOM;
  }
  node->kind = (uint8_t)kind;
  node->doc = doc;
  doc->node_count++;
  *out = node;
  return GTEXT_MARKUP_OK;
}

/* Release one node's own memory and count. Nothing below it, nothing around it:
   the caller has already dealt with its children and its place in the tree. */
static void node_release(GTEXT_Markup_Document * doc, node_t * node) {
  switch ((GTEXT_Markup_Kind)node->kind) {
    case GTEXT_MARKUP_KIND_ELEMENT: {
      markup_element * e = (markup_element *)node;
      markup_atom_release(doc, e->ns);
      markup_atom_release(doc, e->prefix);
      markup_atom_release(doc, e->local);
      for (size_t i = 0; i < e->attr_count; i++) {
        markup_atom_release(doc, e->attrs[i].ns);
        markup_atom_release(doc, e->attrs[i].prefix);
        markup_atom_release(doc, e->attrs[i].local);
        markup_free(doc, e->attrs[i].value);
      }
      markup_free(doc, e->attrs);
      break;
    }
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
    case GTEXT_MARKUP_KIND_COMMENT:
      markup_free(doc, ((markup_text *)node)->data);
      break;
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      markup_pi * p = (markup_pi *)node;
      markup_atom_release(doc, p->target);
      markup_free(doc, p->data);
      break;
    }
    case GTEXT_MARKUP_KIND_DOCTYPE: {
      markup_doctype * d = (markup_doctype *)node;
      markup_free(doc, d->name);
      markup_free(doc, d->public_id);
      markup_free(doc, d->system_id);
      break;
    }
    case GTEXT_MARKUP_KIND_DOCUMENT:
    case GTEXT_MARKUP_KIND_FRAGMENT:
    case GTEXT_MARKUP_KIND_NONE:
      break;
  }
  markup_free(doc, node);
  doc->node_count--;
}

/* Free a subtree, whatever its depth, without recursion. The subtree's top
   must already be out of every list; everything inside it is freed with it,
   template contents included. A leaf is detached from its parent's bookkeeping
   before it is released, so the parent's "first extended child" always names
   something that still exists. */
void markup_free_subtree(GTEXT_Markup_Document * doc, node_t * top) {
  node_t * cur = top;
  for (;;) {
    node_t * c;
    while ((c = markup_ext_first(cur)) != NULL) {
      cur = c;
    }
    if (cur == top) {
      node_release(doc, cur);
      return;
    }
    node_t * parent = cur->parent;
    if (cur->flags & MARKUP_F_CONTENT) {
      ((markup_element *)parent)->content = NULL;
    }
    else {
      parent->first_child = cur->next;
    }
    node_release(doc, cur);
    cur = parent;
  }
}

/* ---- documents ---------------------------------------------------------- */

GTEXT_Markup_Status gtext_markup_document_new(
    const GTEXT_Markup_Options * opts, GTEXT_Markup_Document ** out) {
  if (!out) {
    return GTEXT_MARKUP_E_INVALID;
  }
  const GTEXT_Markup_Options use =
      opts ? *opts : gtext_markup_options_default();
  const GTEXT_Allocator * allocator =
      use.allocator ? use.allocator : gtext_allocator_default();

  GTEXT_Markup_Document * doc = gtext_allocator_calloc(
      allocator, 1, sizeof(*doc));
  if (!doc) {
    return GTEXT_MARKUP_E_OOM;
  }
  doc->allocator = *allocator;
  doc->options = use;
  doc->options.allocator = &doc->allocator;
  /* See hash_bytes() in markup_support.c for what this is and is not. */
  int stack_marker = 0;
  doc->seed = (size_t)(uintptr_t)doc * 0x9E3779B97F4A7C15ull
      ^ (size_t)(uintptr_t)&stack_marker;
  doc->mode = GTEXT_MARKUP_MODE_NO_QUIRKS;
  doc->encoding_source = GTEXT_MARKUP_ENCODING_UNKNOWN;
  doc->standalone = GTEXT_MARKUP_STANDALONE_UNSPECIFIED;

  node_t * root = NULL;
  const GTEXT_Markup_Status status =
      node_new(doc, GTEXT_MARKUP_KIND_DOCUMENT, &root);
  if (status != GTEXT_MARKUP_OK) {
    gtext_allocator_free(allocator, doc);
    return status;
  }
  root->flags = MARKUP_F_ROOT;
  doc->root = root;
  *out = doc;
  return GTEXT_MARKUP_OK;
}

void gtext_markup_document_free(GTEXT_Markup_Document * doc) {
  if (!doc) {
    return;
  }
  while (doc->detached) {
    node_t * node = doc->detached;
    detached_remove(doc, node);
    markup_free_subtree(doc, node);
  }
  markup_free_subtree(doc, doc->root);
  markup_free(doc, doc->buckets);
  markup_free(doc, doc->encoding);
  markup_free(doc, doc->xml_version);
  const GTEXT_Allocator allocator = doc->allocator;
  gtext_allocator_free(&allocator, doc);
}

GTEXT_Markup_Node * gtext_markup_document_root(const GTEXT_Markup_Document * doc) {
  return doc ? doc->root : NULL;
}

static node_t * find_child_of_kind(const GTEXT_Markup_Document * doc,
    GTEXT_Markup_Kind kind) {
  if (!doc) {
    return NULL;
  }
  for (node_t * c = doc->root->first_child; c; c = c->next) {
    if (c->kind == kind) {
      return c;
    }
  }
  return NULL;
}

GTEXT_Markup_Node * gtext_markup_document_element(
    const GTEXT_Markup_Document * doc) {
  return find_child_of_kind(doc, GTEXT_MARKUP_KIND_ELEMENT);
}

GTEXT_Markup_Node * gtext_markup_document_doctype(
    const GTEXT_Markup_Document * doc) {
  return find_child_of_kind(doc, GTEXT_MARKUP_KIND_DOCTYPE);
}

size_t gtext_markup_document_node_count(const GTEXT_Markup_Document * doc) {
  return doc ? doc->node_count : 0;
}

GTEXT_Markup_Mode gtext_markup_document_mode(const GTEXT_Markup_Document * doc) {
  return doc ? doc->mode : GTEXT_MARKUP_MODE_NO_QUIRKS;
}

void gtext_markup_document_set_mode(
    GTEXT_Markup_Document * doc, GTEXT_Markup_Mode mode) {
  if (doc) {
    doc->mode = mode;
  }
}

/* Replace an owned string with a copy of `value`, or clear it. The old string
   survives a failure. */
static GTEXT_Markup_Status replace_string(GTEXT_Markup_Document * doc,
    char ** slot, const char * value) {
  if (!value) {
    markup_free(doc, *slot);
    *slot = NULL;
    return GTEXT_MARKUP_OK;
  }
  const size_t len = strlen(value);
  GTEXT_Markup_Status status = markup_check_utf8(value, len);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  char * copy = markup_copy(doc, value, len);
  if (!copy) {
    return GTEXT_MARKUP_E_OOM;
  }
  markup_free(doc, *slot);
  *slot = copy;
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_document_set_encoding(
    GTEXT_Markup_Document * doc, const char * label,
    GTEXT_Markup_Encoding_Source source) {
  if (!doc) {
    return GTEXT_MARKUP_E_INVALID;
  }
  const GTEXT_Markup_Status status = replace_string(doc, &doc->encoding, label);
  if (status == GTEXT_MARKUP_OK) {
    doc->encoding_source = label ? source : GTEXT_MARKUP_ENCODING_UNKNOWN;
  }
  return status;
}

const char * gtext_markup_document_encoding(const GTEXT_Markup_Document * doc,
    GTEXT_Markup_Encoding_Source * source) {
  if (!doc) {
    return NULL;
  }
  if (source) {
    *source = doc->encoding_source;
  }
  return doc->encoding;
}

GTEXT_Markup_Status gtext_markup_document_set_xml_declaration(
    GTEXT_Markup_Document * doc, const char * version,
    GTEXT_Markup_Standalone standalone) {
  if (!doc) {
    return GTEXT_MARKUP_E_INVALID;
  }
  const GTEXT_Markup_Status status =
      replace_string(doc, &doc->xml_version, version);
  if (status == GTEXT_MARKUP_OK) {
    doc->standalone = version ? standalone : GTEXT_MARKUP_STANDALONE_UNSPECIFIED;
  }
  return status;
}

const char * gtext_markup_document_xml_version(
    const GTEXT_Markup_Document * doc, GTEXT_Markup_Standalone * standalone) {
  if (!doc) {
    return NULL;
  }
  if (standalone) {
    *standalone = doc->standalone;
  }
  return doc->xml_version;
}

/* ---- creating nodes ----------------------------------------------------- */

/* Intern a possibly-absent name. An empty string is the same as none, for a
   namespace and a prefix: XML's xmlns="" undeclares a namespace and the DOM
   reads that as null, so a tree that kept both spellings would have two
   "no namespace"s that compare unequal. */
static GTEXT_Markup_Status check_optional_name(const char * name) {
  return name ? markup_check_utf8(name, strlen(name)) : GTEXT_MARKUP_OK;
}

static markup_atom * intern_optional(
    GTEXT_Markup_Document * doc, const char * name, int * failed) {
  if (!name || !*name) {
    return NULL;
  }
  markup_atom * atom = markup_atom_intern(doc, name, strlen(name));
  if (!atom) {
    *failed = 1;
  }
  return atom;
}

static void new_node_discard(GTEXT_Markup_Document * doc, node_t * node) {
  /* A node_new() that did not get as far as being linked anywhere. */
  node_release(doc, node);
}

GTEXT_Markup_Status gtext_markup_create_element(GTEXT_Markup_Document * doc,
    const char * ns, const char * prefix, const char * local,
    GTEXT_Markup_Node ** out) {
  if (!doc || !out || !local) {
    return GTEXT_MARKUP_E_INVALID;
  }
  if (!*local) {
    return GTEXT_MARKUP_E_NAME;
  }
  GTEXT_Markup_Status status = markup_check_utf8(local, strlen(local));
  if (status == GTEXT_MARKUP_OK) {
    status = check_optional_name(ns);
  }
  if (status == GTEXT_MARKUP_OK) {
    status = check_optional_name(prefix);
  }
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  node_t * node = NULL;
  status = node_new(doc, GTEXT_MARKUP_KIND_ELEMENT, &node);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  markup_element * e = (markup_element *)node;
  int failed = 0;
  e->ns = intern_optional(doc, ns, &failed);
  e->prefix = failed ? NULL : intern_optional(doc, prefix, &failed);
  e->local = failed ? NULL : markup_atom_intern(doc, local, strlen(local));
  if (failed || !e->local) {
    new_node_discard(doc, node);
    return GTEXT_MARKUP_E_OOM;
  }
  detached_push(doc, node);
  *out = node;
  return GTEXT_MARKUP_OK;
}

static GTEXT_Markup_Status create_textual(GTEXT_Markup_Document * doc,
    GTEXT_Markup_Kind kind, const char * data, size_t length,
    GTEXT_Markup_Node ** out) {
  if (!doc || !out || (!data && length != 0)) {
    return GTEXT_MARKUP_E_INVALID;
  }
  GTEXT_Markup_Status status = markup_check_utf8(data, length);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  node_t * node = NULL;
  status = node_new(doc, kind, &node);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  markup_text * t = (markup_text *)node;
  t->data = markup_copy(doc, data, length);
  if (!t->data) {
    new_node_discard(doc, node);
    return GTEXT_MARKUP_E_OOM;
  }
  t->length = length;
  detached_push(doc, node);
  *out = node;
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_create_text(GTEXT_Markup_Document * doc,
    const char * data, size_t length, GTEXT_Markup_Node ** out) {
  return create_textual(doc, GTEXT_MARKUP_KIND_TEXT, data, length, out);
}

GTEXT_Markup_Status gtext_markup_create_cdata(GTEXT_Markup_Document * doc,
    const char * data, size_t length, GTEXT_Markup_Node ** out) {
  return create_textual(doc, GTEXT_MARKUP_KIND_CDATA, data, length, out);
}

GTEXT_Markup_Status gtext_markup_create_comment(GTEXT_Markup_Document * doc,
    const char * data, size_t length, GTEXT_Markup_Node ** out) {
  return create_textual(doc, GTEXT_MARKUP_KIND_COMMENT, data, length, out);
}

GTEXT_Markup_Status gtext_markup_create_processing_instruction(
    GTEXT_Markup_Document * doc, const char * target, const char * data,
    size_t length, GTEXT_Markup_Node ** out) {
  if (!doc || !out || !target || (!data && length != 0)) {
    return GTEXT_MARKUP_E_INVALID;
  }
  if (!*target) {
    return GTEXT_MARKUP_E_NAME;
  }
  GTEXT_Markup_Status status = markup_check_utf8(target, strlen(target));
  if (status == GTEXT_MARKUP_OK) {
    status = markup_check_utf8(data, length);
  }
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  node_t * node = NULL;
  status = node_new(doc, GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION, &node);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  markup_pi * p = (markup_pi *)node;
  p->target = markup_atom_intern(doc, target, strlen(target));
  p->data = p->target ? markup_copy(doc, data, length) : NULL;
  if (!p->target || !p->data) {
    new_node_discard(doc, node);
    return GTEXT_MARKUP_E_OOM;
  }
  p->length = length;
  detached_push(doc, node);
  *out = node;
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_create_doctype(GTEXT_Markup_Document * doc,
    const char * name, const char * public_id, const char * system_id,
    GTEXT_Markup_Node ** out) {
  if (!doc || !out || !name) {
    return GTEXT_MARKUP_E_INVALID;
  }
  GTEXT_Markup_Status status = markup_check_utf8(name, strlen(name));
  if (status == GTEXT_MARKUP_OK) {
    status = check_optional_name(public_id);
  }
  if (status == GTEXT_MARKUP_OK) {
    status = check_optional_name(system_id);
  }
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  node_t * node = NULL;
  status = node_new(doc, GTEXT_MARKUP_KIND_DOCTYPE, &node);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  markup_doctype * d = (markup_doctype *)node;
  d->name = markup_copy(doc, name, strlen(name));
  d->public_id = public_id ? markup_copy(doc, public_id, strlen(public_id)) : NULL;
  d->system_id = system_id ? markup_copy(doc, system_id, strlen(system_id)) : NULL;
  if (!d->name || (public_id && !d->public_id) || (system_id && !d->system_id)) {
    new_node_discard(doc, node);
    return GTEXT_MARKUP_E_OOM;
  }
  detached_push(doc, node);
  *out = node;
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_create_fragment(
    GTEXT_Markup_Document * doc, GTEXT_Markup_Node ** out) {
  if (!doc || !out) {
    return GTEXT_MARKUP_E_INVALID;
  }
  node_t * node = NULL;
  const GTEXT_Markup_Status status =
      node_new(doc, GTEXT_MARKUP_KIND_FRAGMENT, &node);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  detached_push(doc, node);
  *out = node;
  return GTEXT_MARKUP_OK;
}

/* ---- reading a node ----------------------------------------------------- */

GTEXT_Markup_Kind gtext_markup_node_kind(const GTEXT_Markup_Node * node) {
  return node ? (GTEXT_Markup_Kind)node->kind : GTEXT_MARKUP_KIND_NONE;
}

GTEXT_Markup_Document * gtext_markup_node_document(
    const GTEXT_Markup_Node * node) {
  return node ? node->doc : NULL;
}

/* A node's public parent: the raw one, except that a template's content hangs
   from its element without being its child. */
static node_t * public_parent(const node_t * node) {
  return (node->flags & MARKUP_F_CONTENT) ? NULL : node->parent;
}

GTEXT_Markup_Node * gtext_markup_node_parent(const GTEXT_Markup_Node * node) {
  return node ? public_parent(node) : NULL;
}

GTEXT_Markup_Node * gtext_markup_node_first_child(const GTEXT_Markup_Node * node) {
  return node ? node->first_child : NULL;
}

GTEXT_Markup_Node * gtext_markup_node_last_child(const GTEXT_Markup_Node * node) {
  return node ? node->last_child : NULL;
}

GTEXT_Markup_Node * gtext_markup_node_previous_sibling(
    const GTEXT_Markup_Node * node) {
  return (node && public_parent(node)) ? node->prev : NULL;
}

GTEXT_Markup_Node * gtext_markup_node_next_sibling(
    const GTEXT_Markup_Node * node) {
  return (node && public_parent(node)) ? node->next : NULL;
}

size_t gtext_markup_node_child_count(const GTEXT_Markup_Node * node) {
  size_t n = 0;
  if (node) {
    for (const node_t * c = node->first_child; c; c = c->next) {
      n++;
    }
  }
  return n;
}

GTEXT_Markup_Node * gtext_markup_node_next_in_order(
    const GTEXT_Markup_Node * root, const GTEXT_Markup_Node * node,
    bool skip_children) {
  if (!root || !node) {
    return NULL;
  }
  if (!skip_children && node->first_child) {
    return node->first_child;
  }
  while (node != root) {
    if (public_parent(node) && node->next) {
      return node->next;
    }
    node = public_parent(node);
    if (!node) {
      return NULL;
    }
  }
  return NULL;
}

const char * gtext_markup_node_data(
    const GTEXT_Markup_Node * node, size_t * length) {
  if (!node) {
    return NULL;
  }
  switch ((GTEXT_Markup_Kind)node->kind) {
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
    case GTEXT_MARKUP_KIND_COMMENT: {
      const markup_text * t = (const markup_text *)node;
      if (length) {
        *length = t->length;
      }
      return t->data;
    }
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      const markup_pi * p = (const markup_pi *)node;
      if (length) {
        *length = p->length;
      }
      return p->data;
    }
    case GTEXT_MARKUP_KIND_ELEMENT:
    case GTEXT_MARKUP_KIND_DOCTYPE:
    case GTEXT_MARKUP_KIND_DOCUMENT:
    case GTEXT_MARKUP_KIND_FRAGMENT:
    case GTEXT_MARKUP_KIND_NONE:
      break;
  }
  return NULL;
}

GTEXT_Markup_Status gtext_markup_node_set_data(
    GTEXT_Markup_Node * node, const char * data, size_t length) {
  if (!node || (!data && length != 0)) {
    return GTEXT_MARKUP_E_INVALID;
  }
  char ** slot = NULL;
  size_t * len_slot = NULL;
  switch ((GTEXT_Markup_Kind)node->kind) {
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
    case GTEXT_MARKUP_KIND_COMMENT:
      slot = &((markup_text *)node)->data;
      len_slot = &((markup_text *)node)->length;
      break;
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
      slot = &((markup_pi *)node)->data;
      len_slot = &((markup_pi *)node)->length;
      break;
    case GTEXT_MARKUP_KIND_ELEMENT:
    case GTEXT_MARKUP_KIND_DOCTYPE:
    case GTEXT_MARKUP_KIND_DOCUMENT:
    case GTEXT_MARKUP_KIND_FRAGMENT:
    case GTEXT_MARKUP_KIND_NONE:
      return GTEXT_MARKUP_E_INVALID;
  }
  GTEXT_Markup_Status status = markup_check_utf8(data, length);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  char * copy = markup_copy(node->doc, data, length);
  if (!copy) {
    return GTEXT_MARKUP_E_OOM;
  }
  markup_free(node->doc, *slot);
  *slot = copy;
  *len_slot = length;
  node->cap_log2 = 0;
  return GTEXT_MARKUP_OK;
}

void gtext_markup_string_free(const GTEXT_Allocator * allocator, char * string) {
  gtext_allocator_free(allocator, string);
}

GTEXT_Markup_Status gtext_markup_node_text_content(
    const GTEXT_Markup_Node * node, const GTEXT_Allocator * allocator,
    char ** out, size_t * length) {
  if (!node || !out) {
    return GTEXT_MARKUP_E_INVALID;
  }
  markup_buf buf;
  markup_buf_init(&buf, allocator ? allocator : &node->doc->allocator);
  GTEXT_Markup_Status status = markup_buf_append(&buf, "", 0);
  for (const node_t * n = node; n && status == GTEXT_MARKUP_OK;
      n = gtext_markup_node_next_in_order(node, n, false)) {
    if (n->kind == GTEXT_MARKUP_KIND_TEXT || n->kind == GTEXT_MARKUP_KIND_CDATA) {
      const markup_text * t = (const markup_text *)n;
      status = markup_buf_append(&buf, t->data, t->length);
    }
  }
  if (status != GTEXT_MARKUP_OK) {
    markup_buf_free(&buf);
    return status;
  }
  *out = buf.data;
  if (length) {
    *length = buf.length;
  }
  return GTEXT_MARKUP_OK;
}

static const markup_element * as_element(const GTEXT_Markup_Node * node) {
  return (node && node->kind == GTEXT_MARKUP_KIND_ELEMENT)
      ? (const markup_element *)node : NULL;
}

const char * gtext_markup_element_namespace(const GTEXT_Markup_Node * element) {
  const markup_element * e = as_element(element);
  return (e && e->ns) ? e->ns->text : NULL;
}

const char * gtext_markup_element_prefix(const GTEXT_Markup_Node * element) {
  const markup_element * e = as_element(element);
  return (e && e->prefix) ? e->prefix->text : NULL;
}

const char * gtext_markup_element_local_name(const GTEXT_Markup_Node * element) {
  const markup_element * e = as_element(element);
  return e ? e->local->text : NULL;
}

bool gtext_markup_element_is(const GTEXT_Markup_Node * element, const char * ns,
    const char * local) {
  const markup_element * e = as_element(element);
  if (!e || !local) {
    return false;
  }
  /* An empty namespace is no namespace, as everywhere else in this API. */
  const char * want = (ns && *ns) ? ns : NULL;
  const char * have = e->ns ? e->ns->text : NULL;
  if ((want == NULL) != (have == NULL) || (want && strcmp(want, have) != 0)) {
    return false;
  }
  return strcmp(e->local->text, local) == 0;
}

const char * gtext_markup_pi_target(const GTEXT_Markup_Node * node) {
  return (node && node->kind == GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION)
      ? ((const markup_pi *)node)->target->text : NULL;
}

const char * gtext_markup_doctype_name(const GTEXT_Markup_Node * node) {
  return (node && node->kind == GTEXT_MARKUP_KIND_DOCTYPE)
      ? ((const markup_doctype *)node)->name : NULL;
}

const char * gtext_markup_doctype_public_id(const GTEXT_Markup_Node * node) {
  return (node && node->kind == GTEXT_MARKUP_KIND_DOCTYPE)
      ? ((const markup_doctype *)node)->public_id : NULL;
}

const char * gtext_markup_doctype_system_id(const GTEXT_Markup_Node * node) {
  return (node && node->kind == GTEXT_MARKUP_KIND_DOCTYPE)
      ? ((const markup_doctype *)node)->system_id : NULL;
}

static void store_location(uint8_t * flags, size_t * offset, uint32_t * line,
    uint32_t * column, const GTEXT_Markup_Location * location) {
  if (!location) {
    *flags = (uint8_t)(*flags & ~MARKUP_F_LOCATION);
    return;
  }
  *flags = (uint8_t)(*flags | MARKUP_F_LOCATION);
  *offset = location->offset;
  *line = location->line > UINT32_MAX ? UINT32_MAX : (uint32_t)location->line;
  *column = location->column > UINT32_MAX ? UINT32_MAX : (uint32_t)location->column;
}

bool gtext_markup_node_location(
    const GTEXT_Markup_Node * node, GTEXT_Markup_Location * out) {
  if (!node || !(node->flags & MARKUP_F_LOCATION)) {
    return false;
  }
  if (out) {
    out->offset = node->offset;
    out->line = node->line;
    out->column = node->column;
  }
  return true;
}

void gtext_markup_node_set_location(
    GTEXT_Markup_Node * node, const GTEXT_Markup_Location * location) {
  if (node) {
    store_location(&node->flags, &node->offset, &node->line, &node->column,
        location);
  }
}

/* ---- attributes --------------------------------------------------------- */

size_t gtext_markup_element_attribute_count(const GTEXT_Markup_Node * element) {
  const markup_element * e = as_element(element);
  return e ? e->attr_count : 0;
}

static void view_of(const markup_attr * a, GTEXT_Markup_Attribute * out) {
  out->ns = a->ns ? a->ns->text : NULL;
  out->prefix = a->prefix ? a->prefix->text : NULL;
  out->local = a->local->text;
  out->value = a->value;
  out->value_length = a->value_length;
}

GTEXT_Markup_Status gtext_markup_element_attribute_at(
    const GTEXT_Markup_Node * element, size_t index,
    GTEXT_Markup_Attribute * out) {
  const markup_element * e = as_element(element);
  if (!e || !out || index >= e->attr_count) {
    return GTEXT_MARKUP_E_INVALID;
  }
  view_of(&e->attrs[index], out);
  return GTEXT_MARKUP_OK;
}

/* The index of the attribute with this name, or SIZE_MAX. Looking a name up
   interns nothing: a name nobody has interned cannot be on any attribute. */
static size_t find_attribute(const markup_element * e, const char * ns,
    const char * local) {
  if (!local) {
    return SIZE_MAX;
  }
  const markup_atom * want_local =
      markup_atom_find(e->base.doc, local, strlen(local));
  if (!want_local) {
    return SIZE_MAX;
  }
  const markup_atom * want_ns = NULL;
  if (ns && *ns) {
    want_ns = markup_atom_find(e->base.doc, ns, strlen(ns));
    if (!want_ns) {
      return SIZE_MAX;
    }
  }
  for (size_t i = 0; i < e->attr_count; i++) {
    if (e->attrs[i].local == want_local && e->attrs[i].ns == want_ns) {
      return i;
    }
  }
  return SIZE_MAX;
}

bool gtext_markup_element_get_attribute(const GTEXT_Markup_Node * element,
    const char * ns, const char * local, GTEXT_Markup_Attribute * out) {
  const markup_element * e = as_element(element);
  if (!e) {
    return false;
  }
  const size_t i = find_attribute(e, ns, local);
  if (i == SIZE_MAX) {
    return false;
  }
  if (out) {
    view_of(&e->attrs[i], out);
  }
  return true;
}

GTEXT_Markup_Status gtext_markup_element_set_attribute(
    GTEXT_Markup_Node * element, const char * ns, const char * prefix,
    const char * local, const char * value, size_t value_length) {
  markup_element * e = (markup_element *)as_element(element);
  if (!e || !local || (!value && value_length != 0)) {
    return GTEXT_MARKUP_E_INVALID;
  }
  if (!*local) {
    return GTEXT_MARKUP_E_NAME;
  }
  GTEXT_Markup_Document * doc = element->doc;
  GTEXT_Markup_Status status = markup_check_utf8(local, strlen(local));
  if (status == GTEXT_MARKUP_OK) {
    status = check_optional_name(ns);
  }
  if (status == GTEXT_MARKUP_OK) {
    status = check_optional_name(prefix);
  }
  if (status == GTEXT_MARKUP_OK) {
    status = markup_check_utf8(value, value_length);
  }
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }

  const size_t existing = find_attribute(e, ns, local);
  char * copy = markup_copy(doc, value, value_length);
  if (!copy) {
    return GTEXT_MARKUP_E_OOM;
  }

  if (existing != SIZE_MAX) {
    markup_attr * a = &e->attrs[existing];
    int failed = 0;
    markup_atom * new_prefix = intern_optional(doc, prefix, &failed);
    if (failed) {
      markup_free(doc, copy);
      return GTEXT_MARKUP_E_OOM;
    }
    markup_atom_release(doc, a->prefix);
    markup_free(doc, a->value);
    a->prefix = new_prefix;
    a->value = copy;
    a->value_length = value_length;
    return GTEXT_MARKUP_OK;
  }

  if (doc->options.max_attributes != 0
      && e->attr_count >= doc->options.max_attributes) {
    markup_free(doc, copy);
    return GTEXT_MARKUP_E_LIMIT;
  }
  if (e->attr_count == e->attr_capacity) {
    const size_t cap = e->attr_capacity ? e->attr_capacity * 2 : 4;
    if (cap > SIZE_MAX / sizeof(markup_attr)) {
      markup_free(doc, copy);
      return GTEXT_MARKUP_E_OOM;
    }
    markup_attr * grown = markup_realloc(doc, e->attrs, cap * sizeof(markup_attr));
    if (!grown) {
      markup_free(doc, copy);
      return GTEXT_MARKUP_E_OOM;
    }
    e->attrs = grown;
    e->attr_capacity = cap;
  }
  int failed = 0;
  markup_atom * a_ns = intern_optional(doc, ns, &failed);
  markup_atom * a_prefix = failed ? NULL : intern_optional(doc, prefix, &failed);
  markup_atom * a_local =
      failed ? NULL : markup_atom_intern(doc, local, strlen(local));
  if (failed || !a_local) {
    markup_atom_release(doc, a_ns);
    markup_atom_release(doc, a_prefix);
    markup_free(doc, copy);
    return GTEXT_MARKUP_E_OOM;
  }
  markup_attr * a = &e->attrs[e->attr_count++];
  memset(a, 0, sizeof(*a));
  a->ns = a_ns;
  a->prefix = a_prefix;
  a->local = a_local;
  a->value = copy;
  a->value_length = value_length;
  return GTEXT_MARKUP_OK;
}

bool gtext_markup_element_remove_attribute(
    GTEXT_Markup_Node * element, const char * ns, const char * local) {
  markup_element * e = (markup_element *)as_element(element);
  if (!e) {
    return false;
  }
  const size_t i = find_attribute(e, ns, local);
  if (i == SIZE_MAX) {
    return false;
  }
  GTEXT_Markup_Document * doc = element->doc;
  markup_atom_release(doc, e->attrs[i].ns);
  markup_atom_release(doc, e->attrs[i].prefix);
  markup_atom_release(doc, e->attrs[i].local);
  markup_free(doc, e->attrs[i].value);
  memmove(&e->attrs[i], &e->attrs[i + 1],
      (e->attr_count - i - 1) * sizeof(markup_attr));
  e->attr_count--;
  return true;
}

GTEXT_Markup_Status gtext_markup_element_set_attribute_location(
    GTEXT_Markup_Node * element, size_t index,
    const GTEXT_Markup_Location * location) {
  markup_element * e = (markup_element *)as_element(element);
  if (!e || index >= e->attr_count) {
    return GTEXT_MARKUP_E_INVALID;
  }
  markup_attr * a = &e->attrs[index];
  uint8_t flags = a->has_location ? MARKUP_F_LOCATION : 0;
  store_location(&flags, &a->offset, &a->line, &a->column, location);
  a->has_location = (flags & MARKUP_F_LOCATION) ? 1 : 0;
  return GTEXT_MARKUP_OK;
}

bool gtext_markup_element_attribute_location(const GTEXT_Markup_Node * element,
    size_t index, GTEXT_Markup_Location * out) {
  const markup_element * e = as_element(element);
  if (!e || index >= e->attr_count || !e->attrs[index].has_location) {
    return false;
  }
  if (out) {
    out->offset = e->attrs[index].offset;
    out->line = e->attrs[index].line;
    out->column = e->attrs[index].column;
  }
  return true;
}

/* ---- template contents -------------------------------------------------- */

GTEXT_Markup_Node * gtext_markup_element_template_content(
    const GTEXT_Markup_Node * element) {
  const markup_element * e = as_element(element);
  return e ? e->content : NULL;
}

GTEXT_Markup_Status gtext_markup_element_ensure_template_content(
    GTEXT_Markup_Node * element, GTEXT_Markup_Node ** out) {
  markup_element * e = (markup_element *)as_element(element);
  if (!e) {
    return GTEXT_MARKUP_E_INVALID;
  }
  if (!e->content) {
    node_t * frag = NULL;
    const GTEXT_Markup_Status status =
        node_new(element->doc, GTEXT_MARKUP_KIND_FRAGMENT, &frag);
    if (status != GTEXT_MARKUP_OK) {
      return status;
    }
    frag->flags = MARKUP_F_CONTENT;
    frag->parent = element;
    e->content = frag;
  }
  if (out) {
    *out = e->content;
  }
  return GTEXT_MARKUP_OK;
}

/* ---- changing the tree -------------------------------------------------- */

/* Take a node out of its parent's lists. It is left with no parent and no
   siblings and is on no list: the caller puts it somewhere. */
static void unlink_child(node_t * node) {
  node_t * parent = node->parent;
  if (node->prev) {
    node->prev->next = node->next;
  }
  else {
    parent->first_child = node->next;
  }
  if (node->next) {
    node->next->prev = node->prev;
  }
  else {
    parent->last_child = node->prev;
  }
  node->parent = NULL;
  node->prev = NULL;
  node->next = NULL;
}

/* Put a node, which is on no list, into `parent` before `reference`, or at the
   end. */
static void link_before(node_t * parent, node_t * node, node_t * reference) {
  node->parent = parent;
  if (reference) {
    node->prev = reference->prev;
    node->next = reference;
    if (reference->prev) {
      reference->prev->next = node;
    }
    else {
      parent->first_child = node;
    }
    reference->prev = node;
  }
  else {
    node->prev = parent->last_child;
    node->next = NULL;
    if (parent->last_child) {
      parent->last_child->next = node;
    }
    else {
      parent->first_child = node;
    }
    parent->last_child = node;
  }
}

static bool is_container(GTEXT_Markup_Kind kind) {
  return kind == GTEXT_MARKUP_KIND_DOCUMENT || kind == GTEXT_MARKUP_KIND_FRAGMENT
      || kind == GTEXT_MARKUP_KIND_ELEMENT;
}

/* What may sit directly under what. */
static bool kind_allowed(GTEXT_Markup_Kind parent, GTEXT_Markup_Kind child) {
  switch (child) {
    case GTEXT_MARKUP_KIND_ELEMENT:
    case GTEXT_MARKUP_KIND_COMMENT:
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
      return is_container(parent);
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
      return parent == GTEXT_MARKUP_KIND_ELEMENT
          || parent == GTEXT_MARKUP_KIND_FRAGMENT;
    case GTEXT_MARKUP_KIND_DOCTYPE:
      return parent == GTEXT_MARKUP_KIND_DOCUMENT;
    case GTEXT_MARKUP_KIND_DOCUMENT:
    case GTEXT_MARKUP_KIND_FRAGMENT:
    case GTEXT_MARKUP_KIND_NONE:
      break;
  }
  return false;
}

/* Is `ancestor` `node` or something above it? Raw parent links, so a template's
   content counts as being inside its element.

   A node with nothing inside it - no children and no template content - can be
   an ancestor of nothing but itself, so that case answers without climbing.
   It is the common one (a parser appends leaves) and the climb is the whole
   cost of an insertion: without this, building a chain n deep by appending at
   the bottom is quadratic. */
static bool is_inclusive_ancestor(const node_t * ancestor, const node_t * node) {
  if (ancestor == node) {
    return true;
  }
  if (!markup_ext_first(ancestor)) {
    return false;
  }
  for (; node; node = node->parent) {
    if (node == ancestor) {
      return true;
    }
  }
  return false;
}

static size_t depth_of(const node_t * node) {
  size_t depth = 0;
  for (; node->parent; node = node->parent) {
    depth++;
  }
  return depth;
}

/* How many levels there are below `node`, template contents counted. */
static size_t height_of(const node_t * node) {
  size_t best = 0;
  size_t depth = 0;
  const node_t * cur = node;
  for (;;) {
    const node_t * c = markup_ext_first(cur);
    if (c) {
      cur = c;
      depth++;
      if (depth > best) {
        best = depth;
      }
      continue;
    }
    for (;;) {
      if (cur == node) {
        return best;
      }
      const node_t * n = markup_ext_next(cur);
      if (n) {
        cur = n;
        break;
      }
      cur = cur->parent;
      depth--;
    }
  }
}

/* The rules for a document's own children that depend on *order*: at most one
   element, at most one doctype, the doctype before the element. Which kinds may
   be there at all - no text under a document - is kind_allowed()'s, and is not
   repeated here: a rule written in two places is a rule that can be changed in
   one. So this sees only kinds that have already been allowed, and everything
   it is not interested in is a comment or a processing instruction.

   It applies them to the sequence the document would have after an insertion:
   the existing children with the new ones spliced in at `reference`, skipping
   `child` (which may be moving within this very parent) and `exclude` (which a
   replacement removes). */
typedef struct {
  int elements;
  int doctypes;
  int seen_element;
  int bad;
} sequence_state;

static void sequence_visit(sequence_state * s, GTEXT_Markup_Kind kind) {
  if (kind == GTEXT_MARKUP_KIND_ELEMENT) {
    s->elements++;
    s->seen_element = 1;
  }
  else if (kind == GTEXT_MARKUP_KIND_DOCTYPE) {
    s->doctypes++;
    if (s->seen_element) {
      s->bad = 1;
    }
  }
}

static void sequence_visit_new(sequence_state * s, const node_t * child) {
  if (child->kind == GTEXT_MARKUP_KIND_FRAGMENT) {
    for (const node_t * c = child->first_child; c; c = c->next) {
      sequence_visit(s, (GTEXT_Markup_Kind)c->kind);
    }
  }
  else {
    sequence_visit(s, (GTEXT_Markup_Kind)child->kind);
  }
}

static bool document_sequence_ok(const node_t * parent, const node_t * child,
    const node_t * reference, const node_t * exclude) {
  sequence_state s = {0, 0, 0, 0};
  int placed = 0;
  for (const node_t * c = parent->first_child; c; c = c->next) {
    if (c == reference) {
      sequence_visit_new(&s, child);
      placed = 1;
    }
    if (c == child || c == exclude) {
      continue;
    }
    sequence_visit(&s, (GTEXT_Markup_Kind)c->kind);
  }
  if (!placed) {
    sequence_visit_new(&s, child);
  }
  return !s.bad && s.elements <= 1 && s.doctypes <= 1;
}

/* Everything that can stop `child` going into `parent` before `reference`,
   checked before anything moves. `exclude` is a child of `parent` that is about
   to leave, or NULL. */
static GTEXT_Markup_Status validate_insert(node_t * parent, node_t * child,
    node_t * reference, const node_t * exclude) {
  if (!parent || !child) {
    return GTEXT_MARKUP_E_INVALID;
  }
  if (parent->doc != child->doc) {
    return GTEXT_MARKUP_E_WRONG_DOCUMENT;
  }
  if (child->kind == GTEXT_MARKUP_KIND_DOCUMENT
      || (child->flags & (MARKUP_F_ROOT | MARKUP_F_CONTENT))) {
    return GTEXT_MARKUP_E_INVALID;
  }
  if (!is_container((GTEXT_Markup_Kind)parent->kind)) {
    return GTEXT_MARKUP_E_HIERARCHY;
  }
  if (reference
      && (reference->parent != parent || (reference->flags & MARKUP_F_CONTENT))) {
    return GTEXT_MARKUP_E_INVALID;
  }

  const int is_fragment = child->kind == GTEXT_MARKUP_KIND_FRAGMENT;
  if (is_fragment) {
    if (child == parent) {
      return GTEXT_MARKUP_E_HIERARCHY;
    }
    for (const node_t * c = child->first_child; c; c = c->next) {
      if (is_inclusive_ancestor(c, parent)
          || !kind_allowed((GTEXT_Markup_Kind)parent->kind,
              (GTEXT_Markup_Kind)c->kind)) {
        return GTEXT_MARKUP_E_HIERARCHY;
      }
    }
  }
  else if (is_inclusive_ancestor(child, parent)
      || !kind_allowed((GTEXT_Markup_Kind)parent->kind,
          (GTEXT_Markup_Kind)child->kind)) {
    return GTEXT_MARKUP_E_HIERARCHY;
  }

  if (parent->kind == GTEXT_MARKUP_KIND_DOCUMENT
      && !document_sequence_ok(parent, child, reference, exclude)) {
    return GTEXT_MARKUP_E_HIERARCHY;
  }

  const size_t max_depth = parent->doc->options.max_depth;
  if (max_depth != 0) {
    /* A fragment's children go in at the fragment's own depth. */
    const size_t below = height_of(child);
    const size_t top = depth_of(parent) + (is_fragment ? 0 : 1);
    if (below > 0 || !is_fragment) {
      if (top + below > max_depth) {
        return GTEXT_MARKUP_E_LIMIT;
      }
    }
  }
  return GTEXT_MARKUP_OK;
}

/* Move a validated node (or a fragment's children) into place. Allocates
   nothing and cannot fail. */
static void commit_insert(
    node_t * parent, node_t * child, node_t * reference) {
  GTEXT_Markup_Document * doc = parent->doc;
  if (child->kind == GTEXT_MARKUP_KIND_FRAGMENT) {
    node_t * c;
    while ((c = child->first_child) != NULL) {
      unlink_child(c);
      link_before(parent, c, reference);
    }
    return;
  }
  if (child->parent) {
    unlink_child(child);
  }
  else {
    detached_remove(doc, child);
  }
  link_before(parent, child, reference);
}

GTEXT_Markup_Status gtext_markup_insert_before(GTEXT_Markup_Node * parent,
    GTEXT_Markup_Node * child, GTEXT_Markup_Node * reference) {
  /* Inserting a node before itself leaves it where it is - when it is a child
     of `parent` to begin with. Otherwise the reference is not a child of
     `parent` at all and validation refuses it; adjusting it first would turn
     that mistake into an append. */
  if (reference && reference == child && parent && child->parent == parent) {
    reference = child->next;
  }
  const GTEXT_Markup_Status status =
      validate_insert(parent, child, reference, NULL);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  commit_insert(parent, child, reference);
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_append_child(
    GTEXT_Markup_Node * parent, GTEXT_Markup_Node * child) {
  return gtext_markup_insert_before(parent, child, NULL);
}

GTEXT_Markup_Status gtext_markup_remove(GTEXT_Markup_Node * node) {
  if (!node || node->kind == GTEXT_MARKUP_KIND_DOCUMENT
      || (node->flags & (MARKUP_F_ROOT | MARKUP_F_CONTENT))) {
    return GTEXT_MARKUP_E_INVALID;
  }
  if (!node->parent) {
    return GTEXT_MARKUP_OK;
  }
  unlink_child(node);
  detached_push(node->doc, node);
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_replace(
    GTEXT_Markup_Node * old_child, GTEXT_Markup_Node * new_child) {
  if (!old_child || !new_child || !old_child->parent
      || (old_child->flags & (MARKUP_F_ROOT | MARKUP_F_CONTENT))) {
    return GTEXT_MARKUP_E_INVALID;
  }
  if (old_child == new_child) {
    return GTEXT_MARKUP_OK;
  }
  node_t * parent = old_child->parent;
  const GTEXT_Markup_Status status =
      validate_insert(parent, new_child, old_child, old_child);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  commit_insert(parent, new_child, old_child);
  unlink_child(old_child);
  detached_push(old_child->doc, old_child);
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_node_free(GTEXT_Markup_Node * node) {
  if (!node) {
    return GTEXT_MARKUP_E_INVALID;
  }
  if (node->kind == GTEXT_MARKUP_KIND_DOCUMENT
      || (node->flags & (MARKUP_F_ROOT | MARKUP_F_CONTENT)) || node->parent) {
    return GTEXT_MARKUP_E_STATE;
  }
  GTEXT_Markup_Document * doc = node->doc;
  detached_remove(doc, node);
  markup_free_subtree(doc, node);
  return GTEXT_MARKUP_OK;
}

/* ---- for the code that copies trees ------------------------------------- */

void markup_append_unchecked(node_t * parent, node_t * child) {
  detached_remove(parent->doc, child);
  link_before(parent, child, NULL);
}

void markup_set_content_unchecked(node_t * element, node_t * fragment) {
  detached_remove(element->doc, fragment);
  fragment->flags = (uint8_t)(fragment->flags | MARKUP_F_CONTENT);
  fragment->parent = element;
  ((markup_element *)element)->content = fragment;
}

void markup_discard_detached(GTEXT_Markup_Document * doc, node_t * node) {
  detached_remove(doc, node);
  markup_free_subtree(doc, node);
}

/* ---- for the HTML tree builder ------------------------------------------ */

/* The tree builder makes tens of thousands of elements from names it has
   already interned and attributes the tokenizer has already deduplicated and
   checked as UTF-8, so these skip the lookups and the checks the public calls
   make. Each does the same allocation work as its public twin and leaves
   nothing behind on failure. */

GTEXT_Markup_Status markup_element_create_atoms(GTEXT_Markup_Document * doc,
    markup_atom * ns, markup_atom * local, node_t ** out) {
  node_t * node = NULL;
  const GTEXT_Markup_Status status =
      node_new(doc, GTEXT_MARKUP_KIND_ELEMENT, &node);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  markup_element * e = (markup_element *)node;
  if (ns) {
    ns->refs++;
  }
  local->refs++;
  e->ns = ns;
  e->local = local;
  detached_push(doc, node);
  *out = node;
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status markup_element_append_attribute(
    GTEXT_Markup_Document * doc, node_t * element, markup_atom * ns,
    markup_atom * prefix, const char * local, size_t local_length,
    const char * value, size_t value_length) {
  markup_element * e = (markup_element *)element;
  if (e->attr_count == e->attr_capacity) {
    const size_t cap = e->attr_capacity ? e->attr_capacity * 2 : 4;
    if (cap > SIZE_MAX / sizeof(markup_attr)) {
      return GTEXT_MARKUP_E_OOM;
    }
    markup_attr * grown = markup_realloc(doc, e->attrs, cap * sizeof(markup_attr));
    if (!grown) {
      return GTEXT_MARKUP_E_OOM;
    }
    e->attrs = grown;
    e->attr_capacity = cap;
  }
  char * copy = markup_copy(doc, value, value_length);
  if (!copy) {
    return GTEXT_MARKUP_E_OOM;
  }
  markup_atom * a_local = markup_atom_intern(doc, local, local_length);
  if (!a_local) {
    markup_free(doc, copy);
    return GTEXT_MARKUP_E_OOM;
  }
  if (ns) {
    ns->refs++;
  }
  if (prefix) {
    prefix->refs++;
  }
  markup_attr * a = &e->attrs[e->attr_count++];
  memset(a, 0, sizeof(*a));
  a->ns = ns;
  a->prefix = prefix;
  a->local = a_local;
  a->value = copy;
  a->value_length = value_length;
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status markup_element_clone_shallow(
    GTEXT_Markup_Document * doc, const node_t * src, node_t ** out) {
  const markup_element * s = (const markup_element *)src;
  node_t * copy = NULL;
  GTEXT_Markup_Status status =
      markup_element_create_atoms(doc, s->ns, s->local, &copy);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  markup_element * d = (markup_element *)copy;
  if (s->prefix) {
    s->prefix->refs++;
    d->prefix = s->prefix;
  }
  if (s->attr_count > 0) {
    d->attrs = markup_alloc(doc, s->attr_count * sizeof(markup_attr));
    if (!d->attrs) {
      markup_discard_detached(doc, copy);
      return GTEXT_MARKUP_E_OOM;
    }
    d->attr_capacity = s->attr_count;
    for (size_t i = 0; i < s->attr_count; i++) {
      const markup_attr * a = &s->attrs[i];
      char * value = markup_copy(doc, a->value, a->value_length);
      if (!value) {
        markup_discard_detached(doc, copy);
        return GTEXT_MARKUP_E_OOM;
      }
      markup_attr * b = &d->attrs[i];
      *b = *a;
      b->value = value;
      if (b->ns) {
        b->ns->refs++;
      }
      if (b->prefix) {
        b->prefix->refs++;
      }
      b->local->refs++;
      d->attr_count = i + 1;
    }
  }
  if (src->flags & MARKUP_F_LOCATION) {
    copy->flags = (uint8_t)(copy->flags | MARKUP_F_LOCATION);
    copy->offset = src->offset;
    copy->line = src->line;
    copy->column = src->column;
  }
  *out = copy;
  return GTEXT_MARKUP_OK;
}

void markup_insert_unchecked(node_t * parent, node_t * child, node_t * reference) {
  if (child->parent) {
    unlink_child(child);
  }
  else {
    detached_remove(child->doc, child);
  }
  link_before(parent, child, reference);
}

void markup_detach_unchecked(node_t * node) {
  if (node->parent) {
    unlink_child(node);
    detached_push(node->doc, node);
  }
}

void markup_move_children_unchecked(node_t * from, node_t * to) {
  node_t * first = from->first_child;
  if (!first) {
    return;
  }
  for (node_t * c = first; c; c = c->next) {
    c->parent = to;
  }
  first->prev = to->last_child;
  if (to->last_child) {
    to->last_child->next = first;
  }
  else {
    to->first_child = first;
  }
  to->last_child = from->last_child;
  from->first_child = NULL;
  from->last_child = NULL;
}

GTEXT_Markup_Status markup_text_append(GTEXT_Markup_Document * doc, node_t * node,
    size_t * capacity, const char * bytes, size_t length) {
  markup_text * t = (markup_text *)node;
  if (length > SIZE_MAX - t->length - 1) {
    return GTEXT_MARKUP_E_OOM;
  }
  const size_t need = t->length + length + 1;
  /* What the buffer holds: the size the node recorded when it last grew, else exactly the data and its NUL
     (how every path that makes or replaces the data leaves it). The caller's `*capacity` is only an output:
     a size it remembered would go stale when the data is replaced. */
  const size_t have = node->cap_log2 ? ((size_t)1 << node->cap_log2) : t->length + 1;
  if (have < need) {
    size_t cap = 16;
    unsigned log2 = 4;
    while (cap < need && cap <= SIZE_MAX / 2) {
      cap *= 2;
      log2++;
    }
    if (cap < need) {
      cap = need;
      log2 = 0;
    }
    char * grown = markup_realloc(doc, t->data, cap);
    if (!grown) {
      return GTEXT_MARKUP_E_OOM;
    }
    t->data = grown;
    node->cap_log2 = (uint8_t)log2;
    *capacity = cap;
  }
  else {
    *capacity = have;
  }
  memcpy(t->data + t->length, bytes, length);
  t->length += length;
  t->data[t->length] = '\0';
  return GTEXT_MARKUP_OK;
}
