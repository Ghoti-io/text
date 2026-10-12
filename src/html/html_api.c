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
 * The public HTML parsers: a whole buffer, a fragment, an incremental parser and (in html_file_io.c) a
 * file, over the tree builder of html_tree_internal.h.
 *
 * Every entry point is made of the same pieces, which is how "chunking never changes the answer" holds
 * for the public layer as it does for the builder: gtext_html_parse() is a parser fed once, and the
 * fragment form is the same parser made by the other constructor. What this layer adds to the builder:
 *
 * - **Encodings**: the labels are checked (UTF-8's only, until phase 3h), a UTF-8 byte order mark at
 *   the very start is recognized and skipped, however the first bytes are cut, and the facts - UTF-8,
 *   and how it was decided - are recorded on the document and in the result. A document that starts
 *   with part of a mark that turns out not to be one gives those bytes to the builder, which reads them
 *   as it would have.
 * - **The input limit**, checked here on everything fed, so a cut input and a whole one are refused by
 *   the same rule whatever the mark holds back.
 * - **Failure** that is sticky and a status of this API's own (a tree builder's reprocess failure,
 *   which is a defect, is E_INTERNAL), and a result that is built for success and for failure alike.
 * - The fragment context, taken by value with NUL-terminated strings and given to the builder with
 *   lengths.
 */

#include <ghoti.io/text/macros.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "html_api_internal.h"

/// The incremental parser: the tree builder, a sticky failure, and the first bytes of the input while they
/// might still be a byte order mark.
struct GTEXT_HTML_Parser {
  GTEXT_Allocator allocator; ///< The caller's, copied; the parser and the builder are made with it.
  GTEXT_HTML_Tree * tree;    ///< The tree builder (a fragment builder for a fragment parser).
  GTEXT_HTML_Status status;  ///< The sticky failure; OK if none.
  bool fragment;             ///< Made by gtext_html_parser_new_fragment().
  bool finished;             ///< The builder has finished successfully.
  bool taken;                ///< ... and its document has been handed out.
  bool bom_open;             ///< The start of the input is still being matched against the mark.
  unsigned char held;        ///< Bytes of the mark seen and held back, 0 to 2.
  bool has_bom;              ///< A mark was found and skipped.
  size_t max_input;          ///< The input limit.
  size_t fed;                ///< Bytes accepted from the caller.
  GTEXT_Markup_Encoding_Source source; ///< The source of the encoding when there is no mark.
  GTEXT_HTML_Encoding_Certainty certainty; ///< How much the transport label counts for (phase 3h reads it; UTF-8 is read either way).
};

static const unsigned char k_bom[3] = {0xEF, 0xBB, 0xBF};

/* ---- encoding labels ---------------------------------------------------- */

static bool is_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/// The Encoding standard's labels of UTF-8, matched ASCII case-insensitively after trimming ASCII
/// white space as "get an encoding" does (declared in html_api_internal.h; the serializer's labels too).
bool html_label_is_utf8(const char * label) {
  static const char * const names[] = {"unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8",
      "utf8", "x-unicode20utf8"};
  while (is_ascii_space(*label)) {
    label++;
  }
  size_t n = strlen(label);
  while (n > 0 && is_ascii_space(label[n - 1])) {
    n--;
  }
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    const char * name = names[i];
    if (strlen(name) != n) {
      continue;
    }
    size_t k = 0;
    for (; k < n; k++) {
      char c = label[k];
      if (c >= 'A' && c <= 'Z') {
        c = (char)(c + 0x20);
      }
      if (c != name[k]) {
        break;
      }
    }
    if (k == n) {
      return true;
    }
  }
  return false;
}

/* ---- statuses ----------------------------------------------------------- */

/// The tree builder's status as the public one.
GTEXT_HTML_Status html_status_from_tree(GTEXT_HTML_TreeStatus status) {
  switch (status) {
    case GTEXT_HTML_TREE_OK:
      return GTEXT_HTML_OK;
    case GTEXT_HTML_TREE_E_OOM:
      return GTEXT_HTML_E_OOM;
    case GTEXT_HTML_TREE_E_STATE:
      return GTEXT_HTML_E_STATE;
    case GTEXT_HTML_TREE_E_LIMIT_DEPTH:
      return GTEXT_HTML_E_LIMIT_DEPTH;
    case GTEXT_HTML_TREE_E_LIMIT_NODES:
      return GTEXT_HTML_E_LIMIT_NODES;
    case GTEXT_HTML_TREE_E_LIMIT_INPUT:
      return GTEXT_HTML_E_LIMIT_INPUT;
    case GTEXT_HTML_TREE_E_LIMIT_NAME:
      return GTEXT_HTML_E_LIMIT_NAME;
    case GTEXT_HTML_TREE_E_LIMIT_VALUE:
      return GTEXT_HTML_E_LIMIT_VALUE;
    case GTEXT_HTML_TREE_E_LIMIT_COMMENT:
      return GTEXT_HTML_E_LIMIT_COMMENT;
    case GTEXT_HTML_TREE_E_LIMIT_TEXT:
      return GTEXT_HTML_E_LIMIT_TEXT;
    case GTEXT_HTML_TREE_E_LIMIT_ATTRIBUTES:
      return GTEXT_HTML_E_LIMIT_ATTRIBUTES;
    case GTEXT_HTML_TREE_E_INVALID:     /* every argument was checked here, so the builder's own is a defect */
    case GTEXT_HTML_TREE_E_REPROCESS:
      return GTEXT_HTML_E_INTERNAL;
  }
  return GTEXT_HTML_E_INTERNAL;
}

/* Record a failure (the first one stands) and return it. */
static GTEXT_HTML_Status parser_fail(GTEXT_HTML_Parser * p, GTEXT_HTML_Status status) {
  if (p->status == GTEXT_HTML_OK) {
    p->status = status;
  }
  return p->status;
}

static GTEXT_HTML_Status parser_fail_tree(GTEXT_HTML_Parser * p, GTEXT_HTML_TreeStatus status) {
  return parser_fail(p, html_status_from_tree(status));
}

/* ---- making a parser ---------------------------------------------------- */

/* The context taken by value, as the builder takes it. `attrs` is the caller's scratch for the
   attribute list (freed by the caller). */
static GTEXT_HTML_Status check_context(const GTEXT_HTML_Fragment_Context * c) {
  if (!c || !c->local_name || c->local_name[0] == '\0' || (unsigned)c->ns > (unsigned)GTEXT_HTML_NS_MATHML
      || (unsigned)c->quirks > (unsigned)GTEXT_MARKUP_MODE_QUIRKS
      || (c->attribute_count > 0 && !c->attributes)) {
    return GTEXT_HTML_E_INVALID;
  }
  for (size_t i = 0; i < c->attribute_count; i++) {
    if (!c->attributes[i].name || c->attributes[i].name[0] == '\0' || !c->attributes[i].value) {
      return GTEXT_HTML_E_INVALID;
    }
  }
  return GTEXT_HTML_OK;
}

/* Make a parser. `context` is NULL for a document. `errors_wanted` false keeps no parse errors. */
static GTEXT_HTML_Status parser_make(const GTEXT_HTML_Parse_Options * opts,
    const GTEXT_HTML_Fragment_Context * context, bool errors_wanted, GTEXT_HTML_Parser ** out) {
  *out = NULL;
  if (context) {
    const GTEXT_HTML_Status checked = check_context(context);
    if (checked != GTEXT_HTML_OK) {
      return checked;
    }
  }
  const GTEXT_HTML_Parse_Options o = opts ? *opts : gtext_html_parse_options_default();
  if ((unsigned)o.transport_encoding_certainty > (unsigned)GTEXT_HTML_TENTATIVE) {
    return GTEXT_HTML_E_INVALID;
  }
  if ((o.transport_encoding && !html_label_is_utf8(o.transport_encoding))
      || (o.default_encoding && !html_label_is_utf8(o.default_encoding))) {
    return GTEXT_HTML_E_UNSUPPORTED_ENCODING;
  }
  const GTEXT_Allocator * allocator = o.allocator ? o.allocator : gtext_allocator_default();
  GTEXT_HTML_Parser * p = gtext_allocator_calloc(allocator, 1, sizeof(*p));
  if (!p) {
    return GTEXT_HTML_E_OOM;
  }
  p->allocator = *allocator;
  p->fragment = context != NULL;
  p->bom_open = context == NULL;
  p->max_input = o.max_input_bytes ? o.max_input_bytes : GTEXT_HTML_TOKENIZER_DEFAULT_MAX_INPUT;
  p->certainty = o.transport_encoding ? o.transport_encoding_certainty : GTEXT_HTML_CERTAIN;
  p->source = o.transport_encoding ? GTEXT_MARKUP_ENCODING_CALLER : GTEXT_MARKUP_ENCODING_DEFAULT;

  GTEXT_HTML_TreeOptions to;
  memset(&to, 0, sizeof(to));
  to.allocator = &p->allocator;
  to.max_open_elements = o.max_open_elements;
  to.max_nodes = o.max_nodes;
  to.max_input_bytes = o.max_input_bytes;
  to.max_name_bytes = o.max_name_bytes;
  to.max_value_bytes = o.max_attribute_bytes;
  to.max_comment_bytes = o.max_comment_bytes;
  to.max_text_bytes = o.max_text_bytes;
  to.max_attributes = o.max_attributes;
  to.max_errors = o.max_errors;
  to.count_errors_only = o.count_errors_only || !errors_wanted;
  to.scripting = o.scripting;

  if (!context) {
    p->tree = gtext_html_tree_new(&to);
    if (!p->tree) {
      gtext_allocator_free(&p->allocator, p);
      return GTEXT_HTML_E_OOM;
    }
    *out = p;
    return GTEXT_HTML_OK;
  }

  GTEXT_HTML_Attribute * attrs = NULL;
  if (context->attribute_count > 0) {
    if (context->attribute_count > SIZE_MAX / sizeof(*attrs)) {
      gtext_allocator_free(&p->allocator, p);
      return GTEXT_HTML_E_OOM;
    }
    attrs = gtext_allocator_malloc(&p->allocator, context->attribute_count * sizeof(*attrs));
    if (!attrs) {
      gtext_allocator_free(&p->allocator, p);
      return GTEXT_HTML_E_OOM;
    }
    for (size_t i = 0; i < context->attribute_count; i++) {
      attrs[i].name = context->attributes[i].name;
      attrs[i].name_length = strlen(context->attributes[i].name);
      attrs[i].value = context->attributes[i].value;
      attrs[i].value_length = strlen(context->attributes[i].value);
    }
  }
  GTEXT_HTML_FragmentContext fc;
  memset(&fc, 0, sizeof(fc));
  fc.ns = context->ns;
  fc.local_name = context->local_name;
  fc.local_name_length = strlen(context->local_name);
  fc.attributes = attrs;
  fc.attribute_count = context->attribute_count;
  fc.form_ancestor = context->form_ancestor;
  fc.quirks = context->quirks;
  fc.scripting = o.scripting;
  GTEXT_HTML_TreeStatus ts = GTEXT_HTML_TREE_OK;
  p->tree = gtext_html_tree_new_fragment(&to, &fc, &ts);
  gtext_allocator_free(&p->allocator, attrs);
  if (!p->tree) {
    const GTEXT_HTML_Status s = html_status_from_tree(ts);
    gtext_allocator_free(&p->allocator, p);
    return s;
  }
  *out = p;
  return GTEXT_HTML_OK;
}

GTEXT_HTML_Status gtext_html_parser_new(
    const GTEXT_HTML_Parse_Options * opts, GTEXT_HTML_Parser ** out) {
  if (!out) {
    return GTEXT_HTML_E_INVALID;
  }
  return parser_make(opts, NULL, true, out);
}

GTEXT_HTML_Status gtext_html_parser_new_fragment(const GTEXT_HTML_Fragment_Context * context,
    const GTEXT_HTML_Parse_Options * opts, GTEXT_HTML_Parser ** out) {
  if (!out) {
    return GTEXT_HTML_E_INVALID;
  }
  if (!context) {
    *out = NULL;
    return GTEXT_HTML_E_INVALID;
  }
  return parser_make(opts, context, true, out);
}

void gtext_html_parser_free(GTEXT_HTML_Parser * p) {
  if (!p) {
    return;
  }
  const GTEXT_Allocator allocator = p->allocator;
  gtext_html_tree_free(p->tree);
  gtext_allocator_free(&allocator, p);
}

/* ---- feeding ------------------------------------------------------------ */

/* Give the builder the bytes of the mark that were held back. */
static GTEXT_HTML_Status release_held(GTEXT_HTML_Parser * p) {
  p->bom_open = false;
  const size_t n = p->held;
  p->held = 0;
  if (n > 0) {
    const GTEXT_HTML_TreeStatus ts = gtext_html_tree_feed(p->tree, k_bom, n);
    if (ts != GTEXT_HTML_TREE_OK) {
      return parser_fail_tree(p, ts);
    }
  }
  return GTEXT_HTML_OK;
}

GTEXT_HTML_Status gtext_html_parser_feed(GTEXT_HTML_Parser * p, const void * data, size_t len) {
  if (!p) {
    return GTEXT_HTML_E_INVALID;
  }
  /* A failed parser says its failure to every call, whatever the call's arguments. */
  if (p->status != GTEXT_HTML_OK) {
    return p->status;
  }
  if (!data && len != 0) {
    return GTEXT_HTML_E_INVALID;
  }
  if (p->finished || p->taken) {
    return GTEXT_HTML_E_STATE;
  }
  if (len == 0) {
    return GTEXT_HTML_OK;
  }
  if (p->fed > p->max_input || len > p->max_input - p->fed) {
    return parser_fail(p, GTEXT_HTML_E_LIMIT_INPUT);
  }
  p->fed += len;
  const unsigned char * d = data;
  while (p->bom_open && len > 0) {
    if (*d == k_bom[p->held]) {
      p->held++;
      d++;
      len--;
      if (p->held == sizeof(k_bom)) {
        p->bom_open = false;
        p->held = 0;
        p->has_bom = true;
        const GTEXT_HTML_TreeStatus ts = gtext_html_tree_skip_input(p->tree, sizeof(k_bom));
        if (ts != GTEXT_HTML_TREE_OK) {
          return parser_fail_tree(p, ts);
        }
      }
    }
    else {
      const GTEXT_HTML_Status s = release_held(p);
      if (s != GTEXT_HTML_OK) {
        return s;
      }
    }
  }
  if (len > 0) {
    const GTEXT_HTML_TreeStatus ts = gtext_html_tree_feed(p->tree, d, len);
    if (ts != GTEXT_HTML_TREE_OK) {
      return parser_fail_tree(p, ts);
    }
  }
  return GTEXT_HTML_OK;
}

/* ---- finishing ---------------------------------------------------------- */

/* Run the end of the input through the builder, once. */
static GTEXT_HTML_Status parser_complete(GTEXT_HTML_Parser * p) {
  if (p->status != GTEXT_HTML_OK) {
    return p->status;
  }
  if (p->finished) {
    return GTEXT_HTML_OK;
  }
  if (p->bom_open) {
    const GTEXT_HTML_Status s = release_held(p);
    if (s != GTEXT_HTML_OK) {
      return s;
    }
  }
  const GTEXT_HTML_TreeStatus ts = gtext_html_tree_finish(p->tree);
  if (ts != GTEXT_HTML_TREE_OK) {
    return parser_fail_tree(p, ts);
  }
  p->finished = true;
  return GTEXT_HTML_OK;
}

/* Fill the result for the parser's present state. */
static void parser_report(GTEXT_HTML_Parser * p, GTEXT_HTML_Status status, GTEXT_Markup_Document * doc,
    GTEXT_HTML_Result * result) {
  if (!result) {
    return;
  }
  html_result_clear(result);
  result->status = status;
  result->message = gtext_html_status_string(status);
  result->encoding = "UTF-8";
  result->encoding_source = p->has_bom ? GTEXT_MARKUP_ENCODING_BOM : p->source;
  result->input_bytes = p->fed;
  if (doc) {
    result->mode = gtext_markup_document_mode(doc);
    result->node_count = gtext_markup_document_node_count(doc);
  }
  /* The error list is best effort after a failure: the status stands if it cannot be built. */
  html_result_set_errors(result, &p->allocator, p->tree);
}

/* The one finish: `doc_out` for a document parser, `doc_out` and `fragment_out` for a fragment. */
static GTEXT_HTML_Status parser_finish(GTEXT_HTML_Parser * p, bool fragment,
    GTEXT_Markup_Document ** doc_out, GTEXT_Markup_Node ** fragment_out, GTEXT_HTML_Result * result) {
  html_result_clear(result);
  if (!p) {
    html_result_set_failure(result, GTEXT_HTML_E_INVALID, NULL);
    return GTEXT_HTML_E_INVALID;
  }
  if (p->status != GTEXT_HTML_OK) {
    /* The sticky failure comes before any complaint about the call (the wrong kind of finish, a NULL output). */
    parser_report(p, p->status, NULL, result);
    return p->status;
  }
  if (!doc_out || (fragment && !fragment_out)) {
    html_result_set_failure(result, GTEXT_HTML_E_INVALID, NULL);
    return GTEXT_HTML_E_INVALID;
  }
  if (p->fragment != fragment || p->taken) {
    /* The wrong kind of finish, or a second one: not a failure of the parse, and the parser stays as it is. */
    html_result_set_failure(result, GTEXT_HTML_E_STATE, NULL);
    return GTEXT_HTML_E_STATE;
  }
  GTEXT_HTML_Status status = parser_complete(p);
  if (status != GTEXT_HTML_OK) {
    parser_report(p, status, NULL, result);
    return status;
  }
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_Markup_Node * node = NULL;
  if (fragment) {
    node = gtext_html_tree_take_fragment(p->tree, &doc);
    if (!node || !doc) {
      status = parser_fail(p, GTEXT_HTML_E_INTERNAL);
      parser_report(p, status, NULL, result);
      return status;
    }
  }
  else {
    doc = gtext_html_tree_take_document(p->tree);
    if (!doc) {
      status = parser_fail(p, GTEXT_HTML_E_INTERNAL);
      parser_report(p, status, NULL, result);
      return status;
    }
    /* The encoding is recorded on the document as the XML parser records its own. */
    if (gtext_markup_document_set_encoding(doc, "UTF-8",
            p->has_bom ? GTEXT_MARKUP_ENCODING_BOM : p->source)
        != GTEXT_MARKUP_OK) {
      gtext_markup_document_free(doc);
      status = parser_fail(p, GTEXT_HTML_E_OOM);
      parser_report(p, status, NULL, result);
      return status;
    }
  }
  p->taken = true;
  if (result) {
    parser_report(p, GTEXT_HTML_OK, doc, result);
    if (!result->internal) {
      /* The error list could not be built: a success without its report is a failure of the call. */
      gtext_markup_document_free(doc);
      p->taken = false;
      p->finished = false;
      status = parser_fail(p, GTEXT_HTML_E_OOM);
      parser_report(p, status, NULL, result);
      return status;
    }
  }
  *doc_out = doc;
  if (fragment) {
    *fragment_out = node;
  }
  return GTEXT_HTML_OK;
}

GTEXT_HTML_Status gtext_html_parser_finish(
    GTEXT_HTML_Parser * p, GTEXT_Markup_Document ** out, GTEXT_HTML_Result * result) {
  return parser_finish(p, false, out, NULL, result);
}

GTEXT_HTML_Status gtext_html_parser_finish_fragment(GTEXT_HTML_Parser * p, GTEXT_Markup_Document ** doc,
    GTEXT_Markup_Node ** fragment, GTEXT_HTML_Result * result) {
  return parser_finish(p, true, doc, fragment, result);
}

/* ---- the whole-buffer forms --------------------------------------------- */

GTEXT_HTML_Status gtext_html_parse(const void * data, size_t len, const GTEXT_HTML_Parse_Options * opts,
    GTEXT_Markup_Document ** out, GTEXT_HTML_Result * result) {
  html_result_clear(result);
  if (!out || (!data && len != 0)) {
    html_result_set_failure(result, GTEXT_HTML_E_INVALID, NULL);
    return GTEXT_HTML_E_INVALID;
  }
  GTEXT_HTML_Parser * p = NULL;
  const GTEXT_HTML_Status made = parser_make(opts, NULL, result != NULL, &p);
  if (made != GTEXT_HTML_OK) {
    html_result_set_failure(result, made, NULL);
    return made;
  }
  GTEXT_HTML_Status status = gtext_html_parser_feed(p, data, len);
  if (status == GTEXT_HTML_OK) {
    status = parser_finish(p, false, out, NULL, result);
  }
  else {
    parser_report(p, status, NULL, result);
  }
  gtext_html_parser_free(p);
  return status;
}

GTEXT_HTML_Status gtext_html_parse_fragment(const GTEXT_HTML_Fragment_Context * context, const void * data,
    size_t len, const GTEXT_HTML_Parse_Options * opts, GTEXT_Markup_Document ** doc,
    GTEXT_Markup_Node ** fragment, GTEXT_HTML_Result * result) {
  html_result_clear(result);
  if (!context || !doc || !fragment || (!data && len != 0)) {
    html_result_set_failure(result, GTEXT_HTML_E_INVALID, NULL);
    return GTEXT_HTML_E_INVALID;
  }
  GTEXT_HTML_Parser * p = NULL;
  const GTEXT_HTML_Status made = parser_make(opts, context, result != NULL, &p);
  if (made != GTEXT_HTML_OK) {
    html_result_set_failure(result, made, NULL);
    return made;
  }
  GTEXT_HTML_Status status = gtext_html_parser_feed(p, data, len);
  if (status == GTEXT_HTML_OK) {
    status = parser_finish(p, true, doc, fragment, result);
  }
  else {
    parser_report(p, status, NULL, result);
  }
  gtext_html_parser_free(p);
  return status;
}
