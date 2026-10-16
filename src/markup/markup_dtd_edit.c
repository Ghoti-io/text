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
 */

/**
 * @file
 * @brief Checked, atomic edits to the retained internal DTD syntax.
 *
 * Candidate subsets are checked by the shared XML parser before the model is
 * replaced. This keeps programmatic DTD construction on the parser's grammar.
 */

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/markup/markup_tree.h>
#include <ghoti.io/text/xml/xml_parse.h>

#include <stdint.h>
#include <string.h>

#include "markup_internal.h"

enum { DEFAULT_DTD_EDIT_WORK = 1024 * 1024 };

typedef enum {
  DTD_EDIT_INSERT,
  DTD_EDIT_REPLACE,
  DTD_EDIT_MOVE,
  DTD_EDIT_REMOVE
} dtd_edit_action;

GTEXT_Markup_DTD_Edit_Options gtext_markup_dtd_edit_options_default(void) {
  GTEXT_Markup_DTD_Edit_Options options = {DEFAULT_DTD_EDIT_WORK};
  return options;
}

static size_t edit_work_limit(const GTEXT_Markup_DTD_Edit_Options *options) {
  return options && options->max_work
      ? options->max_work : DEFAULT_DTD_EDIT_WORK;
}

static int valid_kind(GTEXT_Markup_DTD_Syntax_Kind kind) {
  return kind >= GTEXT_MARKUP_DTD_ELEMENT
      && kind <= GTEXT_MARKUP_DTD_PARAMETER_REFERENCE;
}

static GTEXT_Markup_Status validate_candidate(GTEXT_Markup_Node *node,
    const markup_dtd_syntax *items, size_t count, size_t max_work) {
  markup_doctype *doctype = (markup_doctype *)node;
  size_t length = 0;
  size_t work = count;
  for (size_t i = 0; i < count; ++i) {
    if (items[i].length > SIZE_MAX - length
        || items[i].length > SIZE_MAX - work) {
      return GTEXT_MARKUP_E_LIMIT_WORK;
    }
    length += items[i].length;
    work += items[i].length;
  }
  const size_t prefix_length = sizeof("<!DOCTYPE ") - 1;
  const char *version_prefix = node->doc->xml_version
      && strcmp(node->doc->xml_version, "1.1") == 0
      ? "<?xml version='1.1'?>" : "";
  const size_t version_length = strlen(version_prefix);
  const size_t middle_length = sizeof(" [") - 1;
  const size_t suffix_length = sizeof("]><root/>") - 1;
  size_t name_length = strlen(doctype->name);
  size_t input_length = version_length;
  const size_t pieces[] = {prefix_length, name_length, middle_length, length,
      suffix_length};
  for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); ++i) {
    if (pieces[i] > SIZE_MAX - input_length)
      return GTEXT_MARKUP_E_LIMIT_WORK;
    input_length += pieces[i];
  }
  if (input_length > SIZE_MAX - work) return GTEXT_MARKUP_E_LIMIT_WORK;
  work += input_length;
  if (work > max_work || input_length == SIZE_MAX || count == SIZE_MAX)
    return GTEXT_MARKUP_E_LIMIT_WORK;

  char *input = markup_alloc(node->doc, input_length + 1);
  if (!input) return GTEXT_MARKUP_E_OOM;
  size_t at = 0;
  memcpy(input + at, version_prefix, version_length); at += version_length;
  memcpy(input + at, "<!DOCTYPE ", prefix_length); at += prefix_length;
  memcpy(input + at, doctype->name, name_length); at += name_length;
  memcpy(input + at, " [", middle_length); at += middle_length;
  for (size_t i = 0; i < count; ++i) {
    memcpy(input + at, items[i].source, items[i].length);
    at += items[i].length;
  }
  memcpy(input + at, "]><root/>", suffix_length); at += suffix_length;
  input[at] = 0;

  GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
  options.allocator = &node->doc->allocator;
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_DENY;
  options.max_input_bytes = input_length;
  options.max_total_work = max_work;
  options.max_declarations = count > 0 ? count + 1 : 1;
  GTEXT_Markup_Document *parsed = NULL;
  GTEXT_XML_Error error;
  GTEXT_XML_Status status = gtext_xml_parse(input, input_length, &options,
      &parsed, &error);
  markup_free(node->doc, input);
  if (status == GTEXT_XML_E_OOM) return GTEXT_MARKUP_E_OOM;
  if (status == GTEXT_XML_E_LIMIT_WORK
      || status == GTEXT_XML_E_LIMIT_DECLARATIONS
      || status == GTEXT_XML_E_LIMIT_INPUT) return GTEXT_MARKUP_E_LIMIT_WORK;
  if (status != GTEXT_XML_OK) return GTEXT_MARKUP_E_DTD_SYNTAX;

  /* Every requested item must be retained in the raw internal collection.
     PE-generated declarations live in their separate derived collection. */
  GTEXT_Markup_Node *parsed_doctype = gtext_markup_document_doctype(parsed);
  int retained = parsed_doctype
      && gtext_markup_doctype_syntax_count(parsed_doctype) == count;
  for (size_t i = 0; retained && i < count; ++i) {
    GTEXT_Markup_DTD_Syntax item;
    retained = gtext_markup_doctype_syntax(parsed_doctype, i, &item)
        && item.kind == (GTEXT_Markup_DTD_Syntax_Kind)items[i].kind;
  }
  gtext_markup_document_free(parsed);
  return retained ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_DTD_SYNTAX;
}

static GTEXT_Markup_Status edit_syntax(GTEXT_Markup_Node *node,
    dtd_edit_action action, size_t first, size_t second,
    GTEXT_Markup_DTD_Syntax_Kind kind, const char *source, size_t length,
    const GTEXT_Markup_DTD_Edit_Options *options) {
  if (!node || node->kind != GTEXT_MARKUP_KIND_DOCTYPE)
    return GTEXT_MARKUP_E_INVALID;
  if ((action == DTD_EDIT_INSERT || action == DTD_EDIT_REPLACE)
      && (!source || !valid_kind(kind))) return GTEXT_MARKUP_E_INVALID;
  markup_doctype *doctype = (markup_doctype *)node;
  const size_t old_count = doctype->syntax_count;
  if ((action == DTD_EDIT_INSERT && first > old_count)
      || ((action == DTD_EDIT_REPLACE || action == DTD_EDIT_REMOVE
              || action == DTD_EDIT_MOVE) && first >= old_count)
      || (action == DTD_EDIT_MOVE && second >= old_count))
    return GTEXT_MARKUP_E_INVALID;
  if (action == DTD_EDIT_INSERT && old_count == SIZE_MAX)
    return GTEXT_MARKUP_E_LIMIT_WORK;
  const size_t new_count = action == DTD_EDIT_INSERT ? old_count + 1
      : action == DTD_EDIT_REMOVE ? old_count - 1 : old_count;
  const size_t max_work = edit_work_limit(options);
  size_t projected = new_count;
  for (size_t i = 0; i < old_count; ++i) {
    if (action == DTD_EDIT_REMOVE && i == first) continue;
    size_t item_length = doctype->syntax[i].length;
    if (action == DTD_EDIT_REPLACE && i == first) item_length = length;
    if (item_length > SIZE_MAX - projected) return GTEXT_MARKUP_E_LIMIT_WORK;
    projected += item_length;
  }
  if (action == DTD_EDIT_INSERT) {
    if (length > SIZE_MAX - projected) return GTEXT_MARKUP_E_LIMIT_WORK;
    projected += length;
  }
  if (projected > max_work) return GTEXT_MARKUP_E_LIMIT_WORK;
  if (new_count > SIZE_MAX / sizeof(markup_dtd_syntax))
    return GTEXT_MARKUP_E_LIMIT_WORK;

  markup_dtd_syntax *staged = new_count
      ? markup_alloc(node->doc, new_count * sizeof(*staged)) : NULL;
  if (new_count && !staged) return GTEXT_MARKUP_E_OOM;
  size_t out = 0;
  if (action == DTD_EDIT_INSERT) {
    for (size_t i = 0; i < old_count + 1; ++i) {
      if (i == first) {
        staged[out++] = (markup_dtd_syntax){.kind = kind,
            .source = (char *)source, .length = length};
      }
      else staged[out++] = doctype->syntax[i - (i > first)];
    }
  }
  else if (action == DTD_EDIT_REPLACE) {
    for (size_t i = 0; i < old_count; ++i) {
      staged[out] = doctype->syntax[i];
      if (i == first) {
        staged[out].kind = kind;
        staged[out].source = (char *)source;
        staged[out].length = length;
        staged[out].parameter_entity = NULL;
        memset(&staged[out].location, 0, sizeof(staged[out].location));
      }
      ++out;
    }
  }
  else if (action == DTD_EDIT_REMOVE) {
    for (size_t i = 0; i < old_count; ++i)
      if (i != first) staged[out++] = doctype->syntax[i];
  }
  else {
    markup_dtd_syntax moving = doctype->syntax[first];
    for (size_t i = 0; i < old_count; ++i)
      if (i != first) staged[out++] = doctype->syntax[i];
    if (second < out) {
      memmove(&staged[second + 1], &staged[second],
          (out - second) * sizeof(*staged));
    }
    staged[second] = moving;
    ++out;
  }

  GTEXT_Markup_Status status = validate_candidate(node, staged, new_count,
      max_work);
  if (status != GTEXT_MARKUP_OK) {
    markup_free(node->doc, staged);
    return status;
  }
  for (size_t i = 0; i < new_count; ++i) {
    staged[i].source = markup_copy(node->doc, staged[i].source,
        staged[i].length);
    if (!staged[i].source) {
      for (size_t j = 0; j < i; ++j)
        markup_free(node->doc, staged[j].source);
      markup_free(node->doc, staged);
      return GTEXT_MARKUP_E_OOM;
    }
    staged[i].parameter_entity = NULL;
    staged[i].sequence = i;
    staged[i].external_context = 0;
    staged[i].applies_to_document = 1;
    memset(&staged[i].location, 0, sizeof(staged[i].location));
  }

  for (size_t i = 0; i < doctype->syntax_count; ++i)
    markup_free(node->doc, doctype->syntax[i].source);
  markup_free(node->doc, doctype->syntax);
  doctype->syntax = staged;
  doctype->syntax_count = new_count;
  doctype->syntax_capacity = new_count;
  for (size_t i = 0; i < doctype->generated_syntax_count; ++i) {
    markup_free(node->doc, doctype->generated_syntax[i].source);
    markup_free(node->doc, doctype->generated_syntax[i].parameter_entity);
  }
  markup_free(node->doc, doctype->generated_syntax);
  doctype->generated_syntax = NULL;
  doctype->generated_syntax_count = doctype->generated_syntax_capacity = 0;
  markup_doctype_clear_bindings(node);
  markup_free(node->doc, doctype->bindings);
  doctype->bindings = NULL;
  doctype->binding_capacity = 0;
  doctype->next_syntax_sequence = new_count;
  for (size_t i = 0; i < doctype->external_syntax_count; ++i)
    if (doctype->external_syntax[i].sequence >= doctype->next_syntax_sequence)
      doctype->next_syntax_sequence = doctype->external_syntax[i].sequence + 1;
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_doctype_insert_syntax(
    GTEXT_Markup_Node *doctype, size_t index,
    GTEXT_Markup_DTD_Syntax_Kind kind, const char *source, size_t length,
    const GTEXT_Markup_DTD_Edit_Options *options) {
  return edit_syntax(doctype, DTD_EDIT_INSERT, index, 0, kind, source, length,
      options);
}

GTEXT_Markup_Status gtext_markup_doctype_replace_syntax(
    GTEXT_Markup_Node *doctype, size_t index,
    GTEXT_Markup_DTD_Syntax_Kind kind, const char *source, size_t length,
    const GTEXT_Markup_DTD_Edit_Options *options) {
  return edit_syntax(doctype, DTD_EDIT_REPLACE, index, 0, kind, source, length,
      options);
}

GTEXT_Markup_Status gtext_markup_doctype_move_syntax(
    GTEXT_Markup_Node *doctype, size_t from_index, size_t to_index,
    const GTEXT_Markup_DTD_Edit_Options *options) {
  return edit_syntax(doctype, DTD_EDIT_MOVE, from_index, to_index,
      GTEXT_MARKUP_DTD_ELEMENT, NULL, 0, options);
}

GTEXT_Markup_Status gtext_markup_doctype_remove_syntax(
    GTEXT_Markup_Node *doctype, size_t index,
    const GTEXT_Markup_DTD_Edit_Options *options) {
  return edit_syntax(doctype, DTD_EDIT_REMOVE, index, 0,
      GTEXT_MARKUP_DTD_ELEMENT, NULL, 0, options);
}
