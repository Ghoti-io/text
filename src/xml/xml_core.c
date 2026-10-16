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
 * Status names, default options and error handling for the XML parser.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "xml_internal.h"

const char * gtext_xml_status_string(GTEXT_XML_Status status) {
  switch (status) {
    case GTEXT_XML_OK:
      return "ok";
    case GTEXT_XML_E_INVALID:
      return "invalid argument";
    case GTEXT_XML_E_OOM:
      return "out of memory";
    case GTEXT_XML_E_SYNTAX:
      return "not well-formed";
    case GTEXT_XML_E_UNEXPECTED_EOF:
      return "unexpected end of input";
    case GTEXT_XML_E_UTF8:
      return "invalid UTF-8";
    case GTEXT_XML_E_CHARACTER:
      return "character not allowed in XML";
    case GTEXT_XML_E_MISMATCHED_TAG:
      return "end tag does not match its start tag";
    case GTEXT_XML_E_DUPLICATE_ATTRIBUTE:
      return "duplicate attribute";
    case GTEXT_XML_E_ENTITY:
      return "undefined entity";
    case GTEXT_XML_E_NAMESPACE:
      return "namespace error";
    case GTEXT_XML_E_DOCTYPE:
      return "malformed document type declaration";
    case GTEXT_XML_E_UNSUPPORTED_ENCODING:
      return "unsupported encoding";
    case GTEXT_XML_E_UNSUPPORTED_VERSION:
      return "unsupported XML version";
    case GTEXT_XML_E_LIMIT_INPUT:
      return "input too long";
    case GTEXT_XML_E_LIMIT_DEPTH:
      return "nesting too deep";
    case GTEXT_XML_E_LIMIT_ATTRIBUTES:
      return "too many attributes";
    case GTEXT_XML_E_LIMIT_VALUE:
      return "attribute value too long";
    case GTEXT_XML_E_LIMIT_NAME:
      return "name too long";
    case GTEXT_XML_E_LIMIT_NODES:
      return "too many nodes";
    case GTEXT_XML_E_LIMIT_NAMESPACES:
      return "too many namespace declarations in scope";
    case GTEXT_XML_E_INVALID_SEQUENCE:
      return "invalid byte sequence for the document's encoding";
    case GTEXT_XML_E_ENCODING_MISMATCH:
      return "declared encoding does not match the document";
    case GTEXT_XML_E_INCOMPLETE:
      return "more input is needed";
    case GTEXT_XML_E_STATE:
      return "wrong state for this call";
    case GTEXT_XML_E_WRITE:
      return "output refused";
    case GTEXT_XML_E_UNREPRESENTABLE:
      return "no XML spelling for this";
    case GTEXT_XML_E_NAME:
      return "not an XML name";
    case GTEXT_XML_E_LIMIT_OUTPUT:
      return "output too long";
    case GTEXT_XML_E_XHTML:
      return "not the same tree for an HTML parser";
    case GTEXT_XML_E_UNSUPPORTED_MODE:
      return "requested XML mode is not implemented";
    case GTEXT_XML_E_LIMIT_EXTERNAL_SOURCE:
      return "external source too large";
    case GTEXT_XML_E_LIMIT_EXTERNAL_TOTAL:
      return "external input too large in total";
    case GTEXT_XML_E_LIMIT_EXPANSION:
      return "entity expansion too large";
    case GTEXT_XML_E_LIMIT_ENTITY_DEPTH:
      return "entity nesting too deep";
    case GTEXT_XML_E_LIMIT_REFERENCES:
      return "too many XML references";
    case GTEXT_XML_E_LIMIT_DECLARATIONS:
      return "too many DTD declarations";
    case GTEXT_XML_E_LIMIT_WORK:
      return "XML parser work limit exceeded";
    case GTEXT_XML_E_RESOLVER:
      return "external resource resolver failed";
    case GTEXT_XML_E_CANCELLED:
      return "external resource read cancelled";
    case GTEXT_XML_E_UNAVAILABLE_RESOURCE:
      return "external entity resource unavailable";
  }
  return "unknown status";
}

GTEXT_XML_Status xml_validate_dtd_options(
    const GTEXT_XML_Parse_Options * options, GTEXT_XML_Error * err) {
  const GTEXT_XML_DTD_Mode mode = options ? options->dtd_mode : GTEXT_XML_DTD_OFF;
  const GTEXT_XML_DTD_External_Policy policy = options
      ? options->dtd_external_policy : GTEXT_XML_DTD_EXTERNAL_DENY;
  const GTEXT_XML_External_Access access = options
      ? options->external_access : GTEXT_XML_EXTERNAL_ACCESS_DENY;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  const char * message = NULL;

  if (mode != GTEXT_XML_DTD_OFF && mode != GTEXT_XML_DTD_PROCESS
      && mode != GTEXT_XML_DTD_VALIDATE) {
    status = GTEXT_XML_E_INVALID;
    message = "invalid DTD mode";
  }
  else if (policy != GTEXT_XML_DTD_EXTERNAL_DENY
      && policy != GTEXT_XML_DTD_EXTERNAL_SKIP) {
    status = GTEXT_XML_E_INVALID;
    message = "invalid external entity policy";
  }
  else if (access != GTEXT_XML_EXTERNAL_ACCESS_DENY
      && access != GTEXT_XML_EXTERNAL_ACCESS_ALLOW) {
    status = GTEXT_XML_E_INVALID;
    message = "invalid external access policy";
  }
  else if (access == GTEXT_XML_EXTERNAL_ACCESS_ALLOW
      && mode == GTEXT_XML_DTD_OFF) {
    status = GTEXT_XML_E_INVALID;
    message = "external access requires DTD processing mode";
  }
  else if (policy == GTEXT_XML_DTD_EXTERNAL_SKIP
      && mode != GTEXT_XML_DTD_PROCESS) {
    status = GTEXT_XML_E_INVALID;
    message = "external entity skip policy requires DTD processing mode";
  }
  if (err && status != GTEXT_XML_OK) {
    memset(err, 0, sizeof(*err));
    err->code = status;
    err->message = message;
    err->line = 1;
    err->column = 1;
  }
  return status;
}

GTEXT_XML_Parse_Options gtext_xml_parse_options_default(void) {
  GTEXT_XML_Parse_Options opts;
  memset(&opts, 0, sizeof(opts));
  opts.max_input_bytes = XML_DEFAULT_MAX_INPUT;
  opts.max_depth = XML_DEFAULT_MAX_DEPTH;
  opts.max_attributes = XML_DEFAULT_MAX_ATTRIBUTES;
  opts.max_attribute_bytes = XML_DEFAULT_MAX_ATTRIBUTE_BYTES;
  opts.max_name_bytes = XML_DEFAULT_MAX_NAME_BYTES;
  opts.max_nodes = XML_DEFAULT_MAX_NODES;
  opts.max_namespaces = XML_DEFAULT_MAX_NAMESPACES;
  opts.max_external_source_bytes = XML_DEFAULT_MAX_EXTERNAL_SOURCE_BYTES;
  opts.max_external_bytes = XML_DEFAULT_MAX_EXTERNAL_BYTES;
  opts.max_expanded_bytes = XML_DEFAULT_MAX_EXPANDED_BYTES;
  opts.max_entity_depth = XML_DEFAULT_MAX_ENTITY_DEPTH;
  opts.max_references = XML_DEFAULT_MAX_REFERENCES;
  opts.max_declarations = XML_DEFAULT_MAX_DECLARATIONS;
  opts.max_total_work = XML_DEFAULT_MAX_TOTAL_WORK;
  opts.max_validation_work = XML_DEFAULT_MAX_VALIDATION_WORK;
  opts.max_validation_memory = XML_DEFAULT_MAX_VALIDATION_MEMORY;
  opts.max_validation_diagnostics = XML_DEFAULT_MAX_VALIDATION_DIAGNOSTICS;
  opts.max_pull_queue_bytes = XML_DEFAULT_MAX_PULL_QUEUE_BYTES;
  opts.max_pull_queue_events = XML_DEFAULT_MAX_PULL_QUEUE_EVENTS;
  return opts;
}

void gtext_xml_error_free(GTEXT_XML_Error * err) {
  if (err) {
    memset(err, 0, sizeof(*err));
  }
}

/* ---- failing ------------------------------------------------------------ */

GTEXT_XML_Status xml_fail_at(xml_ctx * p, GTEXT_XML_Status code,
    const char * message, const GTEXT_Markup_Location * where) {
  /* The first failure wins: an error path that unwinds through callers must
     not replace the position of the thing that actually went wrong. */
  if (p->err.code == GTEXT_XML_OK) {
    p->err.code = code;
    p->err.message = message;
    p->err.byte_offset = where->offset;
    p->err.line = where->line;
    p->err.column = where->column;
    p->err.source_id = where->source_id;
    p->err.reference_source_id = where->reference_source_id;
    p->err.raw_byte_offset = where->raw_offset;
    p->err.has_raw_byte_offset = where->has_raw_offset;
    const size_t retained = p->source_count > 32 ? 32 : p->source_count;
    const size_t first = p->source_count - retained;
    p->err.reference_trail_count = (uint8_t)retained;
    p->err.reference_trail_truncated = first != 0;
    for (size_t i = 0; i < retained; i++) {
      p->err.reference_trail[i] = p->sources[first + i].source_id;
    }
  }
  return p->err.code;
}

GTEXT_Markup_Location xml_here(const xml_ctx * p) {
  GTEXT_Markup_Location here;
  memset(&here, 0, sizeof(here));
  here.offset = p->base + p->pos;
  here.line = p->line;
  here.column = p->column;
  if (p->source_count != 0) {
    const xml_source_frame * source = &p->sources[p->source_count - 1];
    here.source_id = source->source_id;
    here.reference_source_id = source->reference_source_id;
  }
  else {
    here.source_id = 1;
  }
  return here;
}

GTEXT_XML_Status xml_fail(
    xml_ctx * p, GTEXT_XML_Status code, const char * message) {
  const GTEXT_Markup_Location here = xml_here(p);
  return xml_fail_at(p, code, message, &here);
}

GTEXT_XML_Status xml_unexpected(xml_ctx * p, const char * message) {
  uint32_t cp = 0;
  size_t bytes = 0;
  switch (xml_decode(p, p->pos, &cp, &bytes)) {
    case XML_CHAR_EOF:
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    case XML_CHAR_BAD_UTF8:
    {
      GTEXT_Markup_Location where = xml_here(p);
      /* UTF-8 decoding preserves byte positions, including the BOM base. */
      where.raw_offset = where.offset;
      where.has_raw_offset = true;
      return xml_fail_at(p, GTEXT_XML_E_UTF8, "invalid UTF-8", &where);
    }
    case XML_CHAR_FORBIDDEN:
    case XML_CHAR_RESTRICTED:
      return xml_fail(p, GTEXT_XML_E_CHARACTER, "character not allowed in XML");
    case XML_CHAR_OK:
      break;
  }
  return xml_fail(p, GTEXT_XML_E_SYNTAX, message);
}

GTEXT_XML_Status xml_from_markup(xml_ctx * p, GTEXT_Markup_Status status) {
  switch (status) {
    case GTEXT_MARKUP_OK:
      return GTEXT_XML_OK;
    case GTEXT_MARKUP_E_OOM:
      return xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
    case GTEXT_MARKUP_E_LIMIT:
      return xml_fail(p, GTEXT_XML_E_LIMIT_NODES, "too many nodes");
    default:
      /* Not reachable: the parser checks everything the tree checks before it
         asks. If one ever is, it is a defect here, and the honest report is
         that the input could not be turned into a tree. */
      return xml_fail(p, GTEXT_XML_E_INVALID, "the tree refused a node");
  }
}

GTEXT_XML_Status xml_charge(xml_ctx * p, size_t * counter, size_t limit,
    size_t amount, GTEXT_XML_Status code, const char * message) {
  if (*counter > limit || amount > SIZE_MAX - *counter
      || amount > limit - *counter) {
    return xml_fail(p, code, message);
  }
  *counter += amount;
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_charge_work(xml_ctx * p, size_t amount) {
  const size_t limit = p->max_total_work != 0
      ? p->max_total_work : XML_DEFAULT_MAX_TOTAL_WORK;
  return xml_charge(p, &p->total_work, limit, amount,
      GTEXT_XML_E_LIMIT_WORK, "XML parser work limit exceeded");
}

GTEXT_XML_Write_Options gtext_xml_write_options_default(void) {
  GTEXT_XML_Write_Options opts;
  memset(&opts, 0, sizeof(opts));
  opts.xml_declaration = GTEXT_XML_DECLARATION_AUTO;
  opts.encoding = GTEXT_XML_OUTPUT_UTF8;
  opts.max_depth = XML_WRITE_DEFAULT_MAX_DEPTH;
  opts.max_output_bytes = XML_WRITE_DEFAULT_MAX_OUTPUT;
  opts.byte_order_mark = GTEXT_XML_BOM_AUTO;
  return opts;
}
