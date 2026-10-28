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
 * @file css_markup.h
 * @brief An element interface over a Markup tree (HTML or XML), so that selectors match the tree the parsers build.
 *
 * Reference: the HTML Standard (what HTML adds to matching: the case rules, `:link`, the form states, `:dir()`, `:lang()`, `:defined`, `:open`) and the DOM Standard, as served
 * 2026-10-28.
 *
 * The adapter reads a GTEXT_Markup_Document through the library's own accessors and answers the callbacks of css_element.h with the elements of the tree as handles
 * (`const GTEXT_Markup_Node *`). The document's **mode** gives the options of the matcher (an HTML document, and quirks mode). Shadow trees are absent from a Markup tree, so
 * `containing_host`, `shadow_scope` and `assigned_slot` answer NULL; a `template`'s content is not in the tree of its element and is not reached.
 *
 * ## States
 *
 * The states that the HTML Standard defines from the markup alone are derived: `:link` and `:any-link` (`a` and `area` with `href`), `:checked` and `:default` (a checked
 * checkbox or radio button, a selected option), `:disabled` and `:enabled` (including a descendant of a disabled `fieldset`), `:required` and `:optional`, `:read-only` and
 * `:read-write`, `:placeholder-shown`, `:indeterminate` (a `progress` without a value), `:open` (`details` and `dialog` with `open`) and `:defined` (an element whose name is
 * not a valid custom element name, as no custom element is upgraded in a static tree). The dynamic states (hover, focus, active, visited, target, ...) are never set, and neither are
 * the states that need a constraint validation or a layout (`:valid`, `:invalid`, `:in-range`, ...). A caller's `state` callback in the options adds bits to what is derived.
 * `:dir()` is the element's `dir` attribute, inherited, and for `auto` the first strong character of its text by the Unicode bidirectional class; `:lang()` is the nearest `lang`
 * (or `xml:lang`) attribute.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_MARKUP_H
#define GHOTI_IO_GTEXT_CSS_CSS_MARKUP_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_match.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_tree.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The adapter (the struct is private). Single threaded, like a match context. */
typedef struct GTEXT_CSS_Markup_Adapter GTEXT_CSS_Markup_Adapter;

/** @brief Which language the document is. */
typedef enum {
  GTEXT_CSS_MARKUP_AUTO = 0, ///< An HTML document if its document element is an HTML element (the HTML parser's documents; an XHTML document read as XML is taken as HTML).
  GTEXT_CSS_MARKUP_HTML,     ///< An HTML document: type selectors and attribute names are compared ignoring case for HTML elements.
  GTEXT_CSS_MARKUP_XML       ///< An XML document: names are compared exactly.
} GTEXT_CSS_Markup_Kind;

/** @brief What an adapter may be told. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator; ///< NULL for the default.
  GTEXT_CSS_Markup_Kind kind;        ///< See above.
  /** More state bits for an element (GTEXT_CSS_STATE_*), OR-ed with the derived ones; NULL for none. Called with the node. */
  uint64_t (*state)(void * user, const GTEXT_Markup_Node * element);
  void * user;                       ///< Passed to `state`.
} GTEXT_CSS_Markup_Options;

/**
 * @brief Make an adapter over a document.
 *
 * @param document The document. Not NULL; it must outlive the adapter and not change while a match context made from the adapter is in use.
 * @param options NULL for the defaults.
 * @param out Receives the adapter. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_markup_adapter_new(const GTEXT_Markup_Document * document, const GTEXT_CSS_Markup_Options * options,
    GTEXT_CSS_Markup_Adapter ** out);

/** @brief Free an adapter; NULL is ignored. */
GTEXT_API void gtext_css_markup_adapter_free(GTEXT_CSS_Markup_Adapter * adapter);

/** @brief The element interface (valid for the life of the adapter). */
GTEXT_API const GTEXT_CSS_Element_Interface * gtext_css_markup_interface(const GTEXT_CSS_Markup_Adapter * adapter);

/** @brief The options of a match context for this document: `html_document` and `quirks` from the document and its mode, and visited states never matching. */
GTEXT_API GTEXT_CSS_Selector_Match_Options gtext_css_markup_match_options(const GTEXT_CSS_Markup_Adapter * adapter);

/** @brief A node as the handle of the element interface. */
GTEXT_API GTEXT_CSS_Element_Handle gtext_css_markup_handle(const GTEXT_Markup_Node * element);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_MARKUP_H
