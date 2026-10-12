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
 * The HTML fragment parsing algorithm: WHATWG HTML 13.4 "Parsing HTML fragments", as served at
 * html.spec.whatwg.org/multipage/parsing.html on 2026-10-12 (Living Standard, last updated 9 October 2026),
 * its steps in order:
 *
 *  - (4 to 7) a new document in the mode of the context element's document;
 *  - (9, 10, 11) the fragment context element and the scripting mode;
 *  - (12) the tokenizer's initial state from the context: RCDATA for title and textarea, RAWTEXT for style,
 *    xmp, iframe, noembed and noframes, script data for script, RAWTEXT for noscript when scripting is
 *    enabled, PLAINTEXT for plaintext. The list names HTML elements: an SVG `title` is not RCDATA (the
 *    vectors `<div></div>` in `svg title` say so, and html5lib and parse5 agree);
 *  - (13 to 15) a root `html` element, the only element on the stack of open elements;
 *  - (16, 17) a document fragment as the root insertion target. This builder inserts into the root
 *    element and moves the root's children into the fragment when the fragment is taken: the same
 *    tree (every insertion whose target parent is the first element on the stack goes to the fragment,
 *    including the foster parenting step that returns the html element), and nothing else sees the
 *    difference;
 *  - (18) "in template" pushed on the stack of template insertion modes if the context is a template;
 *  - (19) the fake start tag token of the context (only its attributes are read, for an annotation-xml);
 *  - (20) reset the insertion mode appropriately (it reads the context element);
 *  - (21) the form element pointer: the context element or an ancestor is a form. The pointer is only ever
 *    compared with the elements on the stack, which is never true for a context outside the tree, so a
 *    stand-in address is enough;
 *  - (22 to 24) the input is fed, the parser runs to the end, the fragment is returned.
 *
 * The context element is described by value (GTEXT_HTML_FragmentContext); nothing is made for it and
 * nothing of the caller's is touched.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "html_tree_private.h"

static bool name_is(const GTEXT_HTML_FragmentContext * c, const char * s) {
  const size_t n = strlen(s);
  return c->local_name_length == n && memcmp(c->local_name, s, n) == 0;
}

/* The flags the fragment context element has as an integration point: from its namespace and exact local
   name, and for an annotation-xml the `encoding` attribute of its (fake) start tag. */
static uint16_t context_flags(const GTEXT_HTML_FragmentContext * c) {
  if (c->ns == GTEXT_HTML_NS_MATHML) {
    if (name_is(c, "mi") || name_is(c, "mo") || name_is(c, "mn") || name_is(c, "ms")
        || name_is(c, "mtext")) {
      return F_MTIP;
    }
    if (name_is(c, "annotation-xml")) {
      uint16_t flags = F_ANNOT;
      for (size_t i = 0; i < c->attribute_count; i++) {
        const GTEXT_HTML_Attribute * a = &c->attributes[i];
        if (a->name_length == 8 && memcmp(a->name, "encoding", 8) == 0
            && (ht_ci_equal(a->value, a->value_length, "text/html")
                || ht_ci_equal(a->value, a->value_length, "application/xhtml+xml"))) {
          flags |= F_HIP;
        }
      }
      return flags;
    }
  }
  else if (c->ns == GTEXT_HTML_NS_SVG) {
    if (name_is(c, "foreignObject") || name_is(c, "desc") || name_is(c, "title")) {
      return F_HIP;
    }
  }
  return 0;
}

/* The context's namespace is stored as the builder's own number: the two enumerations must agree. */
_Static_assert((unsigned)GTEXT_HTML_NS_HTML == (unsigned)NS_HTML && (unsigned)GTEXT_HTML_NS_SVG == (unsigned)NS_SVG
        && (unsigned)GTEXT_HTML_NS_MATHML == (unsigned)NS_MATH,
    "GTEXT_HTML_NS_* and NS_* must be the same numbers");

static bool setup(GTEXT_HTML_Tree * tb, const GTEXT_HTML_FragmentContext * c) {
  tb->frag = true;
  tb->ctx_ns = (uint8_t)c->ns;
  tb->ctx_flags = context_flags(c);
  /* One scripting flag: the context's. The builder's option of the same name is for documents, and a
     fragment builder ignores it (the tokenizer has no such flag; the initial state below and the tree
     rules that read the flag both read this one). */
  tb->opt.scripting = c->scripting;
  gtext_markup_document_set_mode(tb->doc, c->quirks);

  /* The context's name for "reset the insertion mode appropriately". Only an HTML element can be one of the
     elements that algorithm names, and the HTML names are lower case: ASCII upper case in an HTML context's
     name is folded (so `TD` is the `td` the HTML parser would have made of it). An SVG or MathML context is
     matched by its exact name everywhere (the integration points above, the end tag rules read the stack,
     which holds the names as the standard adjusts them): its name matches no HTML name, so it has none. */
  if (c->ns == GTEXT_HTML_NS_HTML) {
    char small[64];
    char * lower = small;
    if (c->local_name_length >= sizeof(small)) {
      lower = gtext_allocator_malloc(&tb->alloc, c->local_name_length);
      if (!lower) {
        return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
      }
    }
    for (size_t i = 0; i < c->local_name_length; i++) {
      const char ch = c->local_name[i];
      lower[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    const bool found = ht_names_lookup_ns(tb, NS_HTML, lower, c->local_name_length, true, &tb->ctx_id);
    if (lower != small) {
      gtext_allocator_free(&tb->alloc, lower);
    }
    if (!found) {
      return false;
    }
  }
  else {
    tb->ctx_id = NONE_IDX;
  }

  /* Steps 13 to 15: the root html element is the stack's only node. */
  if (!ht_insert_element_into(tb, gtext_markup_document_root(tb->doc), T_HTML, NULL, 0, NULL)) {
    return false;
  }
  /* Step 16: the document fragment (the result). */
  if (!ht_tree_markup_status(tb, gtext_markup_create_fragment(tb->doc, &tb->frag_node))) {
    return false;
  }

  if (c->ns == GTEXT_HTML_NS_HTML) {
    /* Step 12: the tokenizer's state. */
    GTEXT_HTML_TokenizerState state = GTEXT_HTML_STATE_DATA;
    if (tb->ctx_id == T_TITLE || tb->ctx_id == T_TEXTAREA) {
      state = GTEXT_HTML_STATE_RCDATA;
    }
    else if (tb->ctx_id == T_STYLE || tb->ctx_id == T_XMP || tb->ctx_id == T_IFRAME
        || tb->ctx_id == T_NOEMBED || tb->ctx_id == T_NOFRAMES) {
      state = GTEXT_HTML_STATE_RAWTEXT;
    }
    else if (tb->ctx_id == T_SCRIPT) {
      state = GTEXT_HTML_STATE_SCRIPT_DATA;
    }
    else if (tb->ctx_id == T_NOSCRIPT) {
      state = c->scripting ? GTEXT_HTML_STATE_RAWTEXT : GTEXT_HTML_STATE_DATA;
    }
    else if (tb->ctx_id == T_PLAINTEXT) {
      state = GTEXT_HTML_STATE_PLAINTEXT;
    }
    if (state != GTEXT_HTML_STATE_DATA) {
      gtext_html_tokenizer_set_state(tb->tok, state);
    }
    tb->ctx_template = tb->ctx_id == T_TEMPLATE;
    tb->ctx_select = tb->ctx_id == T_SELECT;
  }
  /* Step 18. */
  if (tb->ctx_template && !ht_tmpl_push(tb, M_IN_TEMPLATE)) {
    return false;
  }
  /* Step 20. */
  tb->mode = ht_tree_reset_mode(tb);
  /* Step 21. */
  tb->form_el = NULL;
  tb->form_ctx = c->form_ancestor;
  return true;
}

GTEXT_HTML_Tree * gtext_html_tree_new_fragment(const GTEXT_HTML_TreeOptions * options,
    const GTEXT_HTML_FragmentContext * context, GTEXT_HTML_TreeStatus * status) {
  GTEXT_HTML_TreeStatus ignored;
  GTEXT_HTML_TreeStatus * st = status ? status : &ignored;
  *st = GTEXT_HTML_TREE_OK;
  if (!context || !context->local_name || context->local_name_length == 0
      || (unsigned)context->ns > (unsigned)GTEXT_HTML_NS_MATHML
      || (unsigned)context->quirks > (unsigned)GTEXT_MARKUP_MODE_QUIRKS
      || (context->attribute_count > 0 && !context->attributes)) {
    *st = GTEXT_HTML_TREE_E_INVALID;
    return NULL;
  }
  for (size_t i = 0; i < context->attribute_count; i++) {
    const GTEXT_HTML_Attribute * a = &context->attributes[i];
    if ((a->name_length > 0 && !a->name) || (a->value_length > 0 && !a->value)) {
      *st = GTEXT_HTML_TREE_E_INVALID;
      return NULL;
    }
  }
  GTEXT_HTML_Tree * tb = gtext_html_tree_new(options);
  if (!tb) {
    *st = GTEXT_HTML_TREE_E_OOM;
    return NULL;
  }
  if (!setup(tb, context)) {
    /* A step that failed without recording why can only have been memory. */
    *st = tb->status != GTEXT_HTML_TREE_OK ? tb->status : GTEXT_HTML_TREE_E_OOM;
    gtext_html_tree_free(tb);
    return NULL;
  }
  return tb;
}

GTEXT_Markup_Node * gtext_html_tree_take_fragment(
    GTEXT_HTML_Tree * tb, GTEXT_Markup_Document ** doc_out) {
  if (!tb || !doc_out || !tb->frag || !tb->finished || tb->status != GTEXT_HTML_TREE_OK
      || tb->doc_taken) {
    return NULL;
  }
  /* The root insertion target of the standard is the fragment; here the root element holds what was
     inserted, and its children are the fragment's. */
  markup_move_children_unchecked(tb->stack[0].node, tb->frag_node);
  tb->doc_taken = true;
  *doc_out = tb->doc;
  tb->doc = NULL;
  return tb->frag_node;
}

GTEXT_HTML_TreeStatus gtext_html_tree_parse_fragment(const GTEXT_HTML_FragmentContext * context,
    const void * data, size_t length, const GTEXT_HTML_TreeOptions * options,
    GTEXT_Markup_Document ** doc_out, GTEXT_Markup_Node ** fragment_out,
    GTEXT_HTML_Tree ** tree_out) {
  if (tree_out) {
    *tree_out = NULL;
  }
  if (doc_out) {
    *doc_out = NULL;
  }
  if (fragment_out) {
    *fragment_out = NULL;
  }
  if (!doc_out || !fragment_out || (!data && length != 0)) {
    return GTEXT_HTML_TREE_E_INVALID;
  }
  GTEXT_HTML_TreeStatus s = GTEXT_HTML_TREE_OK;
  GTEXT_HTML_Tree * tb = gtext_html_tree_new_fragment(options, context, &s);
  if (tree_out) {
    *tree_out = tb;
  }
  if (!tb) {
    return s;
  }
  s = gtext_html_tree_feed(tb, data, length);
  if (s == GTEXT_HTML_TREE_OK) {
    s = gtext_html_tree_finish(tb);
  }
  if (s == GTEXT_HTML_TREE_OK) {
    *fragment_out = gtext_html_tree_take_fragment(tb, doc_out);
    if (!*fragment_out) {
      /* A finished fragment builder always gives its fragment; not giving it is a defect, never success. */
      *doc_out = NULL;
      s = GTEXT_HTML_TREE_E_INVALID;
    }
  }
  if (!tree_out) {
    gtext_html_tree_free(tb);
  }
  return s;
}
