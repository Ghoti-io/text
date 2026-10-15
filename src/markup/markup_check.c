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
 * The invariant checker for the markup tree.
 *
 * Not part of the public API. The tree's tests and its fuzzer call it after
 * every operation, successful or refused, because the property that matters -
 * a refused operation changes nothing, and an accepted one leaves a tree every
 * rule still holds for - is a property of the whole structure, and a test that
 * looks at the one node it touched cannot see a link left pointing at a freed
 * neighbour.
 */

#include <ghoti.io/text/macros.h>

#include <stdint.h>
#include <string.h>

#include "markup_internal.h"

typedef struct GTEXT_Markup_Node node_t;

static void count_atom(markup_atom * a) {
  if (a) {
    a->scratch++;
  }
}

static bool source_id_known(const GTEXT_Markup_Document * doc, uint32_t id) {
  if (id == 0) return true;
  for (size_t i = 0; i < doc->source_count; i++) {
    if (doc->sources[i].view.id == id) return true;
  }
  return false;
}

static bool kind_valid(uint8_t kind) {
  return kind >= GTEXT_MARKUP_KIND_DOCUMENT
      && kind <= GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION;
}

static bool child_allowed(GTEXT_Markup_Kind parent, GTEXT_Markup_Kind child) {
  const bool container = parent == GTEXT_MARKUP_KIND_DOCUMENT
      || parent == GTEXT_MARKUP_KIND_FRAGMENT || parent == GTEXT_MARKUP_KIND_ELEMENT;
  switch (child) {
    case GTEXT_MARKUP_KIND_ELEMENT:
    case GTEXT_MARKUP_KIND_COMMENT:
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
      return container;
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

/* Checks one node and its direct children's links. Returns NULL or a message. */
static const char * check_node(const GTEXT_Markup_Document * doc, node_t * n) {
  if (n->doc != doc) {
    return "a node's document is not the document it is in";
  }
  if (!kind_valid(n->kind)) {
    return "a node has no valid kind";
  }
  const bool container = n->kind == GTEXT_MARKUP_KIND_DOCUMENT
      || n->kind == GTEXT_MARKUP_KIND_FRAGMENT || n->kind == GTEXT_MARKUP_KIND_ELEMENT;
  if (!container && (n->first_child || n->last_child)) {
    return "a node that cannot have children has some";
  }
  if ((n->first_child == NULL) != (n->last_child == NULL)) {
    return "first_child and last_child disagree about being empty";
  }
  if ((n->flags & MARKUP_F_ROOT) && n != doc->root) {
    return "the root flag is on a node that is not the root";
  }
  if ((n->flags & MARKUP_F_CONTENT) && n->kind != GTEXT_MARKUP_KIND_FRAGMENT) {
    return "the content flag is on something that is not a fragment";
  }

  /* Children: links, parents, kinds. */
  const node_t * prev = NULL;
  int elements = 0;
  int doctypes = 0;
  int seen_element = 0;
  for (node_t * c = n->first_child; c; c = c->next) {
    if (c->parent != n) {
      return "a child's parent is not the node whose list it is in";
    }
    if (c->prev != prev) {
      return "a child's prev link is wrong";
    }
    if (c->flags & (MARKUP_F_ROOT | MARKUP_F_CONTENT)) {
      return "the root or a template's content is in a child list";
    }
    if (!kind_valid(c->kind)
        || !child_allowed((GTEXT_Markup_Kind)n->kind, (GTEXT_Markup_Kind)c->kind)) {
      return "a child is of a kind its parent may not hold";
    }
    if (c->kind == GTEXT_MARKUP_KIND_ELEMENT) {
      elements++;
      seen_element = 1;
    }
    else if (c->kind == GTEXT_MARKUP_KIND_DOCTYPE) {
      doctypes++;
      if (seen_element) {
        return "a doctype follows the document's element";
      }
    }
    prev = c;
  }
  if (n->last_child != prev) {
    return "last_child is not the last child";
  }
  if (n->kind == GTEXT_MARKUP_KIND_DOCUMENT && (elements > 1 || doctypes > 1)) {
    return "a document has more than one element or doctype";
  }

  switch ((GTEXT_Markup_Kind)n->kind) {
    case GTEXT_MARKUP_KIND_ELEMENT: {
      markup_element * e = (markup_element *)n;
      if (!e->local || e->local->length == 0) {
        return "an element has no local name";
      }
      count_atom(e->ns);
      count_atom(e->prefix);
      count_atom(e->local);
      if (e->content
          && (!(e->content->flags & MARKUP_F_CONTENT) || e->content->parent != n
              || e->content->kind != GTEXT_MARKUP_KIND_FRAGMENT)) {
        return "a template's content is not marked as that element's";
      }
      if (e->attr_count > e->attr_capacity || (e->attr_count && !e->attrs)) {
        return "an attribute array is smaller than its count";
      }
      const size_t max = doc->options.max_attributes;
      if (max != 0 && e->attr_count > max) {
        return "an element has more attributes than the limit";
      }
      for (size_t i = 0; i < e->attr_count; i++) {
        const markup_attr * a = &e->attrs[i];
        if (!a->local || !a->value || a->local->length == 0
            || a->value[a->value_length] != '\0') {
          return "an attribute is missing its name or its terminated value";
        }
        count_atom(a->ns);
        count_atom(a->prefix);
        count_atom(a->local);
        for (size_t j = i + 1; j < e->attr_count; j++) {
          if (e->attrs[j].local == a->local && e->attrs[j].ns == a->ns) {
            return "two attributes of one element have the same name";
          }
        }
      }
      break;
    }
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
    case GTEXT_MARKUP_KIND_COMMENT: {
      const markup_text * t = (const markup_text *)n;
      if (!t->data || t->data[t->length] != '\0') {
        return "character data is missing or not terminated";
      }
      break;
    }
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      markup_pi * p = (markup_pi *)n;
      if (!p->target || !p->data || p->data[p->length] != '\0') {
        return "a processing instruction is missing its target or data";
      }
      count_atom(p->target);
      break;
    }
    case GTEXT_MARKUP_KIND_DOCTYPE: {
      const markup_doctype * d = (const markup_doctype *)n;
      if (!d->name) {
        return "a doctype has no name";
      }
      if (d->syntax_count > d->syntax_capacity
          || (d->syntax_count && !d->syntax)) {
        return "a doctype's syntax array is inconsistent";
      }
      for (size_t i = 0; i < d->syntax_count; i++) {
        const markup_dtd_syntax * item = &d->syntax[i];
        if (!item->source || item->source[item->length] != '\0'
            || item->kind < GTEXT_MARKUP_DTD_ELEMENT
            || item->kind > GTEXT_MARKUP_DTD_PARAMETER_REFERENCE
            || !source_id_known(doc, item->location.source_id)
            || !source_id_known(doc, item->location.reference_source_id)) {
          return "a doctype has an invalid syntax record";
        }
      }
      if (d->external_syntax_count > d->external_syntax_capacity
          || (d->external_syntax_count && !d->external_syntax)) {
        return "a doctype's external syntax array is inconsistent";
      }
      for (size_t i = 0; i < d->external_syntax_count; i++) {
        const markup_dtd_syntax * item = &d->external_syntax[i];
        if (!item->source || item->source[item->length] != '\0'
            || item->kind < GTEXT_MARKUP_DTD_ELEMENT
            || item->kind > GTEXT_MARKUP_DTD_PARAMETER_REFERENCE
            || !source_id_known(doc, item->location.source_id)
            || !source_id_known(doc, item->location.reference_source_id)) {
          return "a doctype has an invalid external syntax record";
        }
      }
      break;
    }
    case GTEXT_MARKUP_KIND_DOCUMENT:
    case GTEXT_MARKUP_KIND_FRAGMENT:
    case GTEXT_MARKUP_KIND_NONE:
      break;
  }
  return NULL;
}

/* Every node reachable from `top` in the extended order. */
static const char * check_forest(const GTEXT_Markup_Document * doc, node_t * top,
    size_t * counted) {
  node_t * cur = top;
  for (;;) {
    if (++*counted > doc->node_count) {
      return "more nodes are reachable than the document says it holds";
    }
    const char * problem = check_node(doc, cur);
    if (problem) {
      return problem;
    }
    node_t * c = markup_ext_first(cur);
    if (c) {
      cur = c;
      continue;
    }
    for (;;) {
      if (cur == top) {
        return NULL;
      }
      node_t * n = markup_ext_next(cur);
      if (n) {
        cur = n;
        break;
      }
      cur = cur->parent;
    }
  }
}

const char * markup_check_invariants(const GTEXT_Markup_Document * doc) {
  if (!doc || !doc->root) {
    return "no document";
  }
  if (doc->bucket_count & (doc->bucket_count - 1)) {
    return "the atom table's size is not a power of two";
  }

  size_t atoms = 0;
  for (size_t i = 0; i < doc->bucket_count; i++) {
    for (markup_atom * a = doc->buckets[i]; a; a = a->next) {
      a->scratch = 0;
      atoms++;
      if ((a->hash & (doc->bucket_count - 1)) != i) {
        return "an atom is in the wrong bucket";
      }
      if (strlen(a->text) != a->length) {
        return "an atom's length is not its text's length";
      }
      if (atoms > doc->atom_count) {
        return "the table holds more atoms than it says";
      }
    }
  }
  if (atoms != doc->atom_count) {
    return "the table holds fewer atoms than it says";
  }

  if (doc->root->parent || doc->root->kind != GTEXT_MARKUP_KIND_DOCUMENT
      || !(doc->root->flags & MARKUP_F_ROOT)) {
    return "the root is not a parentless document";
  }
  size_t counted = 0;
  const char * problem = check_forest(doc, doc->root, &counted);
  if (problem) {
    return problem;
  }

  const node_t * last = NULL;
  for (node_t * d = doc->detached; d; d = d->next) {
    if (d->prev != last) {
      return "a detached node's prev link is wrong";
    }
    if (d->parent || (d->flags & (MARKUP_F_ROOT | MARKUP_F_CONTENT))
        || d->kind == GTEXT_MARKUP_KIND_DOCUMENT) {
      return "the detached list holds a node that is not a detached top";
    }
    problem = check_forest(doc, d, &counted);
    if (problem) {
      return problem;
    }
    last = d;
  }
  if (counted != doc->node_count) {
    return "the document's node count is not the number of nodes";
  }

  for (size_t i = 0; i < doc->bucket_count; i++) {
    for (markup_atom * a = doc->buckets[i]; a; a = a->next) {
      if (a->refs == 0 || a->refs != a->scratch) {
        return "an atom's reference count is not the number of references";
      }
    }
  }
  return NULL;
}
