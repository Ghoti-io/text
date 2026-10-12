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
 * @file markup_html.h
 * @brief The HTML vocabulary: which elements are void, raw text, and so on.
 *
 * HTML Standard (WHATWG), sections 13.1.2 (elements) and 16.3 (attributes).
 * The table behind these functions records where it was taken from and when.
 *
 * Everything here answers **only for the XHTML namespace**
 * (`http://www.w3.org/1999/xhtml`, GTEXT_MARKUP_NS_HTML). `<br>` in the SVG
 * namespace is not void, and an element with no namespace is not an HTML
 * element: these functions say false for both. Names are compared exactly, and
 * the vocabulary is lower case, which is how the tree holds HTML names.
 *
 * Nothing here parses or sniffs. The vocabulary works on any tree, whatever
 * parsed it; a tree read by the XML parser from an XHTML document already
 * carries the namespace, so the questions can be asked of it with no extra
 * step. Every lookup is a binary search over static tables: O(log n), no
 * allocation, thread-safe.
 */

#ifndef GHOTI_IO_GTEXT_MARKUP_MARKUP_HTML_H
#define GHOTI_IO_GTEXT_MARKUP_MARKUP_HTML_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_tree.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Void element: it has no content and no end tag (`br`, `img`, ...). The 13
 *  the standard lists, and also the legacy `basefont`, `bgsound`, `frame`,
 *  `keygen` and `param`, which the HTML parser and serializer still treat as
 *  void. */
#define GTEXT_MARKUP_HTML_VOID 0x01u
/** Raw text element: `script` and `style`. Its content is text that no markup
 *  or reference in it is recognized. */
#define GTEXT_MARKUP_HTML_RAW_TEXT 0x02u
/** Escapable raw text element: `textarea` and `title`. Its content is text in
 *  which character references (and only they) are recognized. */
#define GTEXT_MARKUP_HTML_ESCAPABLE_RAW_TEXT 0x04u
/** The HTML parser drops a line feed that directly follows the start tag:
 *  `pre`, `textarea` and `listing`. XML keeps it, so a serializer that must
 *  agree with both cannot write such a text. */
#define GTEXT_MARKUP_HTML_DROPS_FIRST_NEWLINE 0x08u

/**
 * @brief The vocabulary flags of an element.
 *
 * @param ns    The element's namespace URI, or NULL.
 * @param local The element's local name, or NULL.
 * @return A combination of GTEXT_MARKUP_HTML_* bits; 0 when `ns` is not the
 *   XHTML namespace, when the name is not in the vocabulary, or when either
 *   argument is NULL.
 */
GTEXT_API unsigned gtext_markup_html_element_flags(
    const char * ns, const char * local);

/** @brief GTEXT_MARKUP_HTML_VOID set? See gtext_markup_html_element_flags(). */
GTEXT_API bool gtext_markup_html_is_void(const char * ns, const char * local);

/** @brief GTEXT_MARKUP_HTML_RAW_TEXT set? */
GTEXT_API bool gtext_markup_html_is_raw_text(
    const char * ns, const char * local);

/** @brief GTEXT_MARKUP_HTML_ESCAPABLE_RAW_TEXT set? */
GTEXT_API bool gtext_markup_html_is_escapable_raw_text(
    const char * ns, const char * local);

/** @brief GTEXT_MARKUP_HTML_DROPS_FIRST_NEWLINE set? */
GTEXT_API bool gtext_markup_html_drops_first_newline(
    const char * ns, const char * local);

/** In the parser's "special" category (the standard's list of elements with
 *  special parsing rules, `address` to `xmp`, and in MathML `mi mo mn ms mtext
 *  annotation-xml` and in SVG `foreignObject desc title`). Reported by
 *  gtext_markup_html_category(), not gtext_markup_html_element_flags(), because
 *  it is a property of the tree builder's stack and not of how an element is
 *  written. */
#define GTEXT_MARKUP_HTML_SPECIAL 0x10u
/** In the "formatting" category: `a b big code em font i nobr s small strike
 *  strong tt u` (the elements the list of active formatting elements holds). */
#define GTEXT_MARKUP_HTML_FORMATTING 0x20u

/**
 * @brief The parser categories of an element: GTEXT_MARKUP_HTML_SPECIAL and
 *   GTEXT_MARKUP_HTML_FORMATTING.
 *
 * Unlike the functions above this answers for the SVG and MathML namespaces as
 * well, because the standard's "special" category names elements of all three.
 *
 * @param ns    The element's namespace URI, or NULL.
 * @param local The element's local name, or NULL.
 * @return A combination of those two bits; 0 for any other element, a name in
 *   no namespace, or a NULL argument.
 */
GTEXT_API unsigned gtext_markup_html_category(
    const char * ns, const char * local);

/** @brief GTEXT_MARKUP_HTML_SPECIAL set? See gtext_markup_html_category(). */
GTEXT_API bool gtext_markup_html_is_special(const char * ns, const char * local);

/** @brief GTEXT_MARKUP_HTML_FORMATTING set? */
GTEXT_API bool gtext_markup_html_is_formatting(
    const char * ns, const char * local);

/**
 * @brief Is this a boolean attribute of this element?
 *
 * The standard's attribute index, rows whose value is "Boolean attribute":
 * `disabled` on `button` but not on `p`; the ones that row lists for "HTML
 * elements" (`autofocus`, `inert`, `itemscope`, `headingreset`) on every HTML
 * element. (`hidden` is not one: it is an enumerated attribute now.) The
 * obsolete presentational ones (`compact`, `nowrap`, ...) are not in it.
 *
 * @param ns        The element's namespace URI, or NULL.
 * @param element   The element's local name.
 * @param attribute The attribute's name (no namespace: these are all
 *   attributes in no namespace).
 * @return false unless `ns` is the XHTML namespace and the pair is listed.
 */
GTEXT_API bool gtext_markup_html_is_boolean_attribute(
    const char * ns, const char * element, const char * attribute);

/**
 * @brief Would an HTML parser read this SVG or MathML name back as written?
 *
 * An HTML parser lower-cases every element and attribute name, then restores the
 * case of the names in two tables of the standard: the 37 SVG element names
 * (`foreignObject`, `linearGradient`, ...), the 58 SVG attribute names (`viewBox`,
 * `preserveAspectRatio`, ...) and MathML's `definitionURL`. A name with an upper
 * case letter that is not in the table for its kind and namespace comes back
 * lower case, as a different name; `contentScriptType` is one (the standard
 * dropped it), and so is any name of your own. A name that is already lower case but is
 * the folded form of a table entry is restored to the entry, which is also another name.
 *
 * @param ns        The element's namespace: SVG or MathML. Any other answers false.
 * @param attribute true for the name of an attribute of an element in `ns`, false
 *   for the name of an element.
 * @param local     The local name, exactly as it is in the tree.
 * @return true when `local` is in the table for `ns` and `attribute` spelled as the table has it, or
 *   has no ASCII upper case letter and is not the lower-cased form of such an entry (`viewbox` is
 *   read back as `viewBox`, `clippath` as `clipPath`, `definitionurl` as `definitionURL`: other names).
 *   Whether the element or attribute exists in the vocabulary is not asked.
 */
GTEXT_API bool gtext_markup_html_foreign_name_survives(
    const char * ns, bool attribute, const char * local);

/** @brief gtext_markup_html_element_flags() of an element node; 0 for any
 *  other node or NULL. */
GTEXT_API unsigned gtext_markup_html_node_flags(
    const GTEXT_Markup_Node * element);

/** @brief Is this node a void HTML element? false for a non-element. */
GTEXT_API bool gtext_markup_html_node_is_void(const GTEXT_Markup_Node * element);

/** @brief Is this node a raw text HTML element? */
GTEXT_API bool gtext_markup_html_node_is_raw_text(
    const GTEXT_Markup_Node * element);

/** @brief Is this node an escapable raw text HTML element? */
GTEXT_API bool gtext_markup_html_node_is_escapable_raw_text(
    const GTEXT_Markup_Node * element);

/** @brief Does this node drop a leading line feed (`pre`, `textarea`,
 *  `listing`)? */
GTEXT_API bool gtext_markup_html_node_drops_first_newline(
    const GTEXT_Markup_Node * element);

/** @brief Is `attribute` of this element node a boolean attribute? */
GTEXT_API bool gtext_markup_html_node_is_boolean_attribute(
    const GTEXT_Markup_Node * element, const char * attribute);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_MARKUP_MARKUP_HTML_H
