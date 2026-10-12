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
 * The HTML tree builder: WHATWG HTML, "Tree construction" (section 13.2.6),
 * driving the tokenizer of html_tokenizer_internal.h and building a
 * `GTEXT_Markup_Document`.
 *
 * ## What it is
 *
 * The standard's second stage: the stack of open elements, the list of active
 * formatting elements with the Noah's Ark clause, the head and form element
 * pointers, the frameset-ok flag, quirks mode from the doctype, every
 * insertion mode (the table modes, "in template" with the stack of template
 * insertion modes and the template contents, foster parenting in the
 * appropriate place for inserting a node), the rules for parsing tokens in
 * foreign content (SVG and MathML: the adjusted current node, the integration
 * points, the name and attribute adjustments, the breakout tags), the adoption
 * agency algorithm, the reconstruction of the active formatting elements, and
 * the HTML fragment parsing algorithm with a context element
 * (gtext_html_tree_new_fragment()). Read from the standard's text as served at
 * html.spec.whatwg.org (Living Standard, last updated 9 October 2026; fetched
 * 2026-10-11 for 3c and again 2026-10-12 for 3d and 3e, the same revision:
 * Last-Modified Fri, 09 Oct 2026 08:28:19 GMT); see
 * documentation/formats/html.md for the section list.
 *
 * ## What is not here
 *
 * There is no public HTML API, no encoding determination (3f), no
 * serialization and no script execution.
 *
 * ## How it is driven
 *
 * ```
 * tree = gtext_html_tree_new(&options);
 * for each chunk: gtext_html_tree_feed(tree, chunk, n);   // OK or a failure
 * gtext_html_tree_finish(tree);
 * doc = gtext_html_tree_take_document(tree);               // NULL after a failure
 * gtext_html_tree_free(tree);
 * ```
 *
 * or gtext_html_parse() for a whole buffer. A chunk is borrowed only for the
 * duration of feed(). **The tree and the errors are the same however the input
 * is cut** (the one exception is `max_input_bytes`, which the tokenizer
 * documents).
 *
 * ## The tree
 *
 * Elements are in the HTML namespace with their attributes in no namespace, in
 * source order (duplicates removed by the tokenizer), except the elements of foreign
 * content: those in the SVG and MathML namespaces have the names and attributes the
 * standard's tables give them (`viewBox`, `foreignObject`, `definitionURL`) and the
 * attributes `xlink:*`, `xml:lang`, `xml:space`, `xmlns` and `xmlns:xlink` are in the
 * XLink, XML and XMLNS namespaces; a doctype node, comments,
 * processing instructions (the pinned standard makes them nodes), and text with
 * adjacent runs merged. A `template` element owns a content document fragment (made with
 * the element, so it is never absent), which is where the builder inserts what the standard
 * puts "in the template"; the element has no children of its own (the adoption agency algorithm
 * cannot make a template the furthest block: `template` is a boundary of "in scope", so a
 * formatting element below one is never adopted). The document mode is set. A DOCTYPE token
 * that names no public or system identifier leaves them NULL in the node (the standard says
 * the empty string; the tree distinguishes absent from empty and the dump
 * format does not).
 *
 * ## Errors, limits, failure
 *
 * Parse errors never fail a parse. A parse fails only for a limit, the
 * allocator, or a construct that belongs to a later stage; the failure is
 * sticky, no tree is returned and nothing leaks. The open-element-stack limit
 * (`max_open_elements`, default 512, `html` and `body` included) is the
 * parser's depth limit.
 *
 * ## Cost
 *
 * Scope checks, "any other end tag", the list item and definition loops and
 * the active formatting list's lookups are O(1) per token (a per-name index of
 * the topmost open element, and for each stack entry the index of the nearest
 * scope boundary below it). What the standard makes linear stays linear:
 * reconstructing formatting elements costs the number reopened, and the
 * adoption agency costs the distance it walks and the elements above the
 * formatting element that it shifts. No code here recurses; the stack is an
 * array.
 *
 * Internal. Nothing here is public API until the HTML parser is.
 */

#ifndef GHOTI_IO_GTEXT_SRC_HTML_HTML_TREE_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_HTML_HTML_TREE_INTERNAL_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "html_tokenizer_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// A tree builder. Opaque.
typedef struct GTEXT_HTML_Tree GTEXT_HTML_Tree;

/* ---- Defaults ----------------------------------------------------------- */

/// The deepest the stack of open elements may grow (html and body count).
#define GTEXT_HTML_TREE_DEFAULT_MAX_OPEN_ELEMENTS ((size_t)512)
/// Most nodes a document may hold.
#define GTEXT_HTML_TREE_DEFAULT_MAX_NODES ((size_t)10 * 1000 * 1000)

/// The most times one token is handled again in another mode (the standard's "reprocess the token"). The
/// longest chain any input makes is 8 (an end tag `table` in a cell: close the cell, the row, the section,
/// the table; the end of the file in a template in a table cell). Nothing in a chain depends on the depth
/// of the stack: closing nested templates at the end of the file is a loop, not a chain. Exceeding the limit
/// is GTEXT_HTML_TREE_E_REPROCESS, sticky, no tree.
#define GTEXT_HTML_TREE_REPROCESS_LIMIT ((size_t)32)

/// The most work (see gtext_html_tree_work()) the tree builder does beyond the tokenizer's for `n`
/// bytes of input when the stack of open elements is limited to `limit`: each token costs at most
/// the stack depth in reopened elements, shifted entries or walked entries (8 per entry covers the
/// adoption agency's loops), plus a constant. Not a bound the standard gives; the one the fuzzer holds
/// every input to, at its own limit (64).
#define GTEXT_HTML_TREE_WORK_BOUND_AT(n, limit) \
  ((8 * (size_t)(limit) + 64) * (size_t)(n) + 4096)

/// The same with no depth to speak of (`limit` 0): the bound the unit tests hold the patterns that make
/// a naive tree builder quadratic to, deep stacks included - the work there is independent of depth.
/// An input that makes the standard's own algorithms reopen many formatting elements for every
/// character costs more, by the standard, and is bounded by the limit through the form above.
#define GTEXT_HTML_TREE_WORK_BOUND(n) GTEXT_HTML_TREE_WORK_BOUND_AT((n), 0)

/* ---- Errors ------------------------------------------------------------- */

/**
 * @brief The parse errors the tree construction stage raises.
 *
 * The standard names exactly one of them (`non-void-html-element-start-tag-
 * with-trailing-solidus`, which is the tokenizer enum's code); for the rest it
 * says only "this is a parse error". These are named for what happened, one
 * for each kind of "parse error" sentence in the algorithms.
 */
typedef enum {
  GTEXT_HTML_TREE_ERR_MISSING_DOCTYPE = 0,        ///< No DOCTYPE before the first token that is not a comment.
  GTEXT_HTML_TREE_ERR_NON_CONFORMING_DOCTYPE,     ///< A DOCTYPE that is not `<!DOCTYPE html>` (or its legacy forms).
  GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE,         ///< A DOCTYPE anywhere but the initial mode.
  GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG,       ///< A start tag the mode ignores or repairs.
  GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG,         ///< An end tag with no matching element in scope, or the mode ignores.
  GTEXT_HTML_TREE_ERR_UNEXPECTED_CHARACTER,       ///< Text where the mode has none.
  GTEXT_HTML_TREE_ERR_UNEXPECTED_NULL_CHARACTER,  ///< A U+0000 in body content.
  GTEXT_HTML_TREE_ERR_UNEXPECTED_EOF,             ///< The input ended in the "text" mode or the head's noscript.
  GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_EOF,       ///< The input ended with elements open that need an end tag.
  GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG,   ///< An end tag closed an element that had open children.
  GTEXT_HTML_TREE_ERR_MISNESTED_FORMATTING,       ///< `a` in `a`, `nobr` in `nobr`, or the adoption agency's repairs.
  GTEXT_HTML_TREE_ERR_NESTED_FORM,                ///< A `form` start tag inside a form.
  GTEXT_HTML_TREE_ERR_UNEXPECTED_ELEMENT_IN_CONTEXT, ///< `h1` in `h1`, a `button` in a button, `select` in `select`...
  GTEXT_HTML_TREE_ERR_DEPRECATED_TAG,             ///< `image` read as `img`; `</br>` read as `<br>`.
  GTEXT_HTML_TREE_ERR_HTML_IN_FOREIGN_CONTENT,    ///< An HTML start tag (or `</p>`, `</br>`) that ends foreign content.
  GTEXT_HTML_TREE_ERR_XMLNS_MISMATCH,             ///< An `xmlns` attribute that is not the element's namespace.
  GTEXT_HTML_TREE_ERR_COUNT
} GTEXT_HTML_TreeErrorCode;

/// Codes of both kinds share one number space: the tokenizer's are
/// `GTEXT_HTML_ErrorCode` values, the tree builder's are
/// `GTEXT_HTML_TREE_ERROR_BASE + GTEXT_HTML_TreeErrorCode`.
#define GTEXT_HTML_TREE_ERROR_BASE 64

/// One parse error, from either stage, in the order raised.
typedef struct {
  int code;            ///< A GTEXT_HTML_ErrorCode, or BASE + a GTEXT_HTML_TreeErrorCode.
  size_t line;         ///< 1-based (the token's, for the tree builder's).
  size_t column;       ///< 1-based, in UTF-16 code units, as the tokenizer's.
  size_t offset;       ///< Byte offset in the input as fed.
} GTEXT_HTML_TreeError;

/// The spelling of a code of either kind ("unknown" for any other).
GTEXT_INTERNAL_API const char * gtext_html_tree_error_name(int code);

/* ---- Status and options ------------------------------------------------- */

/// What a call returned. Failures are sticky.
typedef enum {
  GTEXT_HTML_TREE_OK = 0,
  GTEXT_HTML_TREE_E_INVALID,   ///< A NULL where a value is needed.
  GTEXT_HTML_TREE_E_STATE,     ///< feed() after a successful finish(); and nothing else (a failed builder repeats its failure).
  GTEXT_HTML_TREE_E_OOM,       ///< The allocator returned NULL.
  GTEXT_HTML_TREE_E_REPROCESS, ///< A token was handled again in a new mode more often than any rule can need: a defect, never content.
  GTEXT_HTML_TREE_E_LIMIT_DEPTH,      ///< More than `max_open_elements` open at once.
  GTEXT_HTML_TREE_E_LIMIT_NODES,      ///< More than `max_nodes` nodes.
  GTEXT_HTML_TREE_E_LIMIT_INPUT,      ///< The tokenizer's: more than `max_input_bytes` fed.
  GTEXT_HTML_TREE_E_LIMIT_NAME,       ///< The tokenizer's.
  GTEXT_HTML_TREE_E_LIMIT_VALUE,      ///< The tokenizer's.
  GTEXT_HTML_TREE_E_LIMIT_COMMENT,    ///< The tokenizer's.
  GTEXT_HTML_TREE_E_LIMIT_TEXT,       ///< The tokenizer's.
  GTEXT_HTML_TREE_E_LIMIT_ATTRIBUTES  ///< The tokenizer's.
} GTEXT_HTML_TreeStatus;

/// Whether `status` is one of the limit statuses.
#define GTEXT_HTML_TREE_IS_LIMIT(status) \
  ((status) >= GTEXT_HTML_TREE_E_LIMIT_DEPTH)

/**
 * @brief Options for gtext_html_tree_new(). Zero-initialising gives the
 *   defaults; 0 never means unlimited.
 */
typedef struct {
  const GTEXT_Allocator * allocator; ///< NULL for the default. Used for the document too.
  size_t max_open_elements; ///< Stack of open elements (default 512).
  size_t max_nodes;         ///< Nodes in the document (default 10,000,000).
  /// The tokenizer's limits, passed through (0 selects its defaults).
  size_t max_input_bytes;
  size_t max_name_bytes;
  size_t max_value_bytes;
  size_t max_comment_bytes;
  size_t max_text_bytes;
  size_t max_attributes;
  size_t max_errors;        ///< Parse errors kept, both stages' together; the rest are counted.
  /// The scripting flag: true parses `noscript` in the head as raw text, false
  /// as elements ("in head noscript"). Default false.
  bool scripting;
  /// Seed of the tokenizer's attribute-name hash (0: its own). Tests set it.
  uint64_t hash_seed;
} GTEXT_HTML_TreeOptions;

/* ---- Lifecycle and input ------------------------------------------------ */

/**
 * @brief Make a tree builder, with an empty document, in the "initial" mode.
 *
 * @param options Options, or NULL for the defaults. Copied.
 * @return The builder, or NULL if the allocator failed.
 */
GTEXT_INTERNAL_API GTEXT_HTML_Tree * gtext_html_tree_new(
    const GTEXT_HTML_TreeOptions * options);

/// Free the builder, the document it still holds (if take_document() did not
/// take it) and everything else. NULL is ignored.
GTEXT_INTERNAL_API void gtext_html_tree_free(GTEXT_HTML_Tree * tree);

/**
 * @brief Hand the builder some input and build as far as it determines.
 *
 * The bytes are not kept past the call. An empty chunk is fine.
 *
 * @return OK, or the failure (sticky: the same status again afterwards).
 */
GTEXT_INTERNAL_API GTEXT_HTML_TreeStatus gtext_html_tree_feed(
    GTEXT_HTML_Tree * tree, const void * data, size_t length);

/// Say that there is no more input and finish the tree.
GTEXT_INTERNAL_API GTEXT_HTML_TreeStatus gtext_html_tree_finish(
    GTEXT_HTML_Tree * tree);

/**
 * @brief Take the document, once the builder has finished successfully.
 *
 * @return The document (the caller frees it with gtext_markup_document_free()
 *   and the allocator it was made with), or NULL if finish() has not succeeded
 *   or the document was taken already. A failed builder holds no document.
 */
GTEXT_INTERNAL_API GTEXT_Markup_Document * gtext_html_tree_take_document(
    GTEXT_HTML_Tree * tree);

/// The failure, or OK.
GTEXT_INTERNAL_API GTEXT_HTML_TreeStatus gtext_html_tree_status(
    const GTEXT_HTML_Tree * tree);

/**
 * @brief Parse a whole buffer.
 *
 * @param data The bytes (may be NULL if `length` is 0).
 * @param length How many.
 * @param options Options, or NULL.
 * @param out Receives the document on OK. Written only then (left as it was otherwise).
 * @param tree_out If non-NULL, set to NULL at entry and then receives the builder, for its errors and
 *   status, which the caller frees with gtext_html_tree_free() (even on
 *   failure; NULL if even the builder could not be made).
 * @return OK or the failure.
 */
GTEXT_INTERNAL_API GTEXT_HTML_TreeStatus gtext_html_parse(const void * data,
    size_t length, const GTEXT_HTML_TreeOptions * options,
    GTEXT_Markup_Document ** out, GTEXT_HTML_Tree ** tree_out);

/* ---- Fragments ----------------------------------------------------------- */

/// The namespace of a fragment's context element.
typedef enum {
  GTEXT_HTML_NS_HTML = 0,
  GTEXT_HTML_NS_SVG,
  GTEXT_HTML_NS_MATHML
} GTEXT_HTML_Namespace;

/**
 * @brief The context element of the HTML fragment parsing algorithm (13.4), described by value.
 *
 * No tree node is needed and none is touched: the algorithm reads the context element's namespace and
 * name (the tokenizer's initial state, "reset the insertion mode appropriately", the adjusted current
 * node), its start tag's attributes (`encoding` on a MathML annotation-xml), whether it or an ancestor
 * is a form (the form element pointer) and its document's mode.
 */
typedef struct {
  GTEXT_HTML_Namespace ns;            ///< The namespace of the context element.
  const char * local_name;            ///< Its local name, as the element has it (exact case). Not NULL.
  size_t local_name_length;           ///< Bytes, at least 1.
  /// The attributes of the context element (borrowed; may be NULL when `attribute_count` is 0).
  const GTEXT_HTML_Attribute * attributes;
  size_t attribute_count;
  bool form_ancestor;                 ///< The context element is a form or has a form ancestor.
  GTEXT_Markup_Mode quirks;           ///< The mode of the context element's document.
  bool scripting;                     ///< The scripting flag (noscript as the context; noscript in the head).
} GTEXT_HTML_FragmentContext;

/**
 * @brief Make a tree builder for a fragment: the HTML fragment parsing algorithm, step by step.
 *
 * The result is used like gtext_html_tree_new(): feed(), finish(), and then
 * gtext_html_tree_take_fragment() in place of take_document(). The context is not kept past the
 * call.
 *
 * @param status If non-NULL, receives OK, E_INVALID (a bad context) or E_OOM.
 * @return The builder, or NULL.
 */
GTEXT_INTERNAL_API GTEXT_HTML_Tree * gtext_html_tree_new_fragment(
    const GTEXT_HTML_TreeOptions * options, const GTEXT_HTML_FragmentContext * context,
    GTEXT_HTML_TreeStatus * status);

/**
 * @brief Take the fragment, once a fragment builder has finished successfully.
 *
 * The children of the root `html` element are moved into the document fragment (the standard's
 * "return a DocumentFragment"). The fragment belongs to the document that is handed over with it: free
 * the document and the fragment goes too.
 *
 * @param doc_out Receives the document that owns the fragment (and the root element, now empty).
 * @return The fragment, or NULL if the builder is not a finished fragment builder or it was taken.
 */
GTEXT_INTERNAL_API GTEXT_Markup_Node * gtext_html_tree_take_fragment(
    GTEXT_HTML_Tree * tree, GTEXT_Markup_Document ** doc_out);

/**
 * @brief Parse a whole buffer as a fragment.
 *
 * `doc_out` and `fragment_out` are written only on OK. `tree_out`, if non-NULL, receives the builder for
 * its errors and status, as for gtext_html_parse().
 */
GTEXT_INTERNAL_API GTEXT_HTML_TreeStatus gtext_html_parse_fragment(
    const GTEXT_HTML_FragmentContext * context, const void * data, size_t length,
    const GTEXT_HTML_TreeOptions * options, GTEXT_Markup_Document ** doc_out,
    GTEXT_Markup_Node ** fragment_out, GTEXT_HTML_Tree ** tree_out);

/* ---- Errors and tests --------------------------------------------------- */

/// Tests: the foreign-content tables of the standard, as data. `which` is 0 for the SVG element names
/// (lower case name and the name it becomes), 1 for the SVG attribute names, 2 for the namespaced attributes
/// (the name with its prefix and the namespace's short name: "xlink", "xml" or "xmlns"). Returns the number
/// of rows for `which`, 0 for any other.
GTEXT_INTERNAL_API size_t gtext_html_tree_test_table_size(int which);
/// Tests: row `i` of a table. False past the end.
GTEXT_INTERNAL_API bool gtext_html_tree_test_table_at(
    int which, size_t i, const char ** from, const char ** to);

/// How many parse errors are kept (0 for a NULL tree).
GTEXT_INTERNAL_API size_t gtext_html_tree_error_count(const GTEXT_HTML_Tree * tree);
/// The i-th kept error, in the order raised, or NULL.
GTEXT_INTERNAL_API const GTEXT_HTML_TreeError * gtext_html_tree_error_at(
    const GTEXT_HTML_Tree * tree, size_t index);
/// Parse errors raised and not kept, because of `max_errors`.
GTEXT_INTERNAL_API size_t gtext_html_tree_errors_dropped(const GTEXT_HTML_Tree * tree);

/// Tests: units of work the tree builder did (not the tokenizer's): tokens handled, stack
/// entries walked, entries shifted, list entries visited, nodes moved.
GTEXT_INTERNAL_API size_t gtext_html_tree_work(const GTEXT_HTML_Tree * tree);
/// Tests: the tokenizer's work (gtext_html_tokenizer_work()).
GTEXT_INTERNAL_API size_t gtext_html_tree_tokenizer_work(const GTEXT_HTML_Tree * tree);

/// Tests: allow a token to be handled again in another mode only `n` times (the default is
/// GTEXT_HTML_TREE_REPROCESS_LIMIT), so that the status for exceeding it can be reached.
GTEXT_INTERNAL_API void gtext_html_tree_test_set_reprocess_limit(GTEXT_HTML_Tree * tree, size_t n);

/// Tests: the insertion mode, as its name ("initial", "in body", ...).
GTEXT_INTERNAL_API const char * gtext_html_tree_mode_name(const GTEXT_HTML_Tree * tree);

/**
 * @brief Tests: what "reset the insertion mode appropriately" picks for a stack.
 *
 * Runs the algorithm over a stack that the caller describes: the names of the
 * open elements from the root down (HTML namespace), and whether the head
 * element pointer is set. A name "template/in table" is a template element
 * whose template insertion mode (the stack of them has one per template) is
 * "in table"; a plain "template" has "in template".
 *
 * @return The mode's name, or NULL on failure.
 */
GTEXT_INTERNAL_API const char * gtext_html_tree_test_reset_mode(
    const char * const * names, size_t count, bool head_set);

/// Tests: the document mode the standard picks for a DOCTYPE with these parts
/// (`name`, `public_id` and `system_id` NULL when missing). 0, 1, 2 are
/// no-quirks, limited-quirks, quirks (GTEXT_Markup_Mode).
GTEXT_INTERNAL_API int gtext_html_tree_test_quirks_mode(const char * name,
    const char * public_id, const char * system_id, bool force_quirks);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_HTML_HTML_TREE_INTERNAL_H
