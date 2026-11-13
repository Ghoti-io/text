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
 * @brief The Markup binding of the CSSOM: the owner sheets of `<style>` and `<link rel=stylesheet>` elements, and the `style` attribute of a Markup element (HTML Standard, "update a
 *   style block", "link type stylesheet"; CSS Object Model, "update a style attribute").
 *
 * ## The owner sheets
 *
 * A @ref GTEXT_CSSOM_Markup_Binding connects one Markup document to one tree scope of a CSSOM document. gtext_cssom_markup_sync() reads the document and makes the scope's `styleSheets`
 * say what the document's elements say, with the HTML Standard's rules:
 *
 * - an element is a style element if it is an HTML `style` (options `html`) or an SVG `style` (options `svg`) that is **connected** (in the document's tree: an element in a `<template>`'s
 *   content, or one not yet inserted, has no sheet), and its `type` attribute is absent, empty or an ASCII case-insensitive match for `text/css` (the whole value: `text/css; charset=x` and
 *   ` text/css` are not). Its sheet is made from the **child text content** (the text and CDATA children, in order; a comment is skipped, an element child is ignored) with the element's
 *   `media` as the sheet's media query list, its `title`, its parent as the implicit root and the element as `ownerNode`. An empty `<style>` has an empty sheet.
 * - an HTML `link` whose `rel` has the keyword `stylesheet`, a `type` as above and a non-empty `href` is asked of the **link callback**: the library never fetches. The callback gives the
 *   text, or says the link is pending (gtext_cssom_markup_link_complete() gives the text later) or failed. The keyword `alternate` makes the sheet alternate and disabled; the `disabled`
 *   attribute disables it.
 * - the sheets are listed in the scope in the tree order of their elements, at the start of its list, and the binding keeps them there; a sheet the binding did not make follows them.
 *
 * What a change does, in the terms of the HTML Standard: a change of a `<style>` element's **text** removes the sheet and makes a new one (a new `sheet_id`; the rules the object model had
 * added or removed are gone, and the element's text is not changed by them); an element **moved** is removed and inserted, which is a new sheet; one removed from the tree leaves `styleSheets`
 * and the cascade with `SHEET_REMOVED`, and its sheet **stays a valid, mutable object** whose `owner_node` is NULL and which keeps its rules; an element that qualifies again (its `type`
 * made right, re-inserted) has a new sheet. A change of the `media` attribute sets the sheet's media list in place, and a change of `title` its title (Chromium keeps the sheet for both,
 * Firefox makes a new one: both were probed, Chromium 156 and Firefox 158). Text set to the value it already had changes nothing.
 *
 * ## Cheap when nothing changed
 *
 * gtext_cssom_markup_sync() returns at once, with no walk, when the Markup document's generation (gtext_markup_document_generation()) is the one of the last sync. Otherwise it walks the
 * tree once, compares each style element's text with the text its sheet was made from (length, then bytes), and keeps every sheet whose element is unchanged: the snapshot of a kept sheet is
 * the same object, so the document's index is rebuilt only if something did change. A caller that is told which element changed calls gtext_cssom_markup_style_element_changed() and
 * pays for that element, not for the document.
 *
 * ## Failure
 *
 * Each step (a sheet made, a sheet removed, a sheet put in the list) is a CSSOM mutation with the strong guarantee, and **the sync as a whole is not atomic**: a call that fails (memory, a
 * limit, a failing callback) stops with the document consistent and the binding knowing exactly what it has done, and the next call goes on from there. Observers receive the events of one
 * sync as one batch.
 *
 * ## The `style` attribute
 *
 * gtext_cssom_markup_inline_binding() is the attribute binding of css_cssom_decl.h for a Markup element: `element` is a `GTEXT_Markup_Node *`. gtext_cssom_markup_tree_version() is the
 * config's `tree_version` hook for a Markup document (`tree_version_user` is the document), so the declaration of an element notices a `setAttribute('style', ...)` made through the
 * Markup API on its next call. A write the declaration makes advances the Markup document's generation like any other change of the tree.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CSSOM_MARKUP_H
#define GHOTI_IO_GTEXT_CSS_CSS_CSSOM_MARKUP_H

#include <ghoti.io/text/css/css_cssom_document.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_tree.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The binding of a Markup document and a tree scope (the struct is private). */
typedef struct GTEXT_CSSOM_Markup_Binding GTEXT_CSSOM_Markup_Binding;

/** @brief What the link callback says when it gives no text. */
typedef enum {
  GTEXT_CSSOM_LINK_LOADED = 0,   ///< (Text is given.)
  GTEXT_CSSOM_LINK_PENDING,      ///< The text will come: gtext_cssom_markup_link_complete() gives it. The element has no sheet until then.
  GTEXT_CSSOM_LINK_FAILED        ///< There will be none. The element has no sheet.
} GTEXT_CSSOM_Link_Outcome;

/**
 * @struct GTEXT_CSSOM_Link_Request
 * @brief What the link callback is asked: valid for the call only.
 */
typedef struct {
  size_t size;                   ///< `sizeof` this struct.
  const GTEXT_Markup_Node * element;   ///< The `link` element.
  const char * href;             ///< Its `href` as written, NUL-terminated.
  size_t href_length;
  const char * rel;              ///< Its `rel` as written ("" if absent).
  const char * media;            ///< Its `media` ("" if absent).
  const char * title;            ///< Its `title` ("" if absent).
  bool alternate;                ///< `rel` has the keyword `alternate`.
  bool disabled;                 ///< It has the `disabled` attribute.
} GTEXT_CSSOM_Link_Request;

/** @brief What a link's text comes with: filled by the callback, valid until it returns (the sheet copies what it needs). */
typedef struct {
  size_t size;                   ///< `sizeof` this struct.
  const char * text;             ///< The sheet's text (UTF-8). NULL with a length of 0 for an empty sheet.
  size_t length;
  const char * base_url;         ///< The sheet's base URL (opaque), or NULL.
  const char * href;             ///< The sheet's `href` as `StyleSheet.href` reports it (the resolved address), or NULL: the element's `href`.
  bool origin_clean;             ///< The origin-clean flag (false: reading the rules and every mutation are GTEXT_CSS_E_SECURITY).
} GTEXT_CSSOM_Link_Text;

/**
 * @brief The link callback: make the text of the sheet a `<link rel=stylesheet>` names, or say it is pending or failed. A status other than GTEXT_CSS_OK fails the sync that asked.
 */
typedef GTEXT_CSS_Status (*GTEXT_CSSOM_Link_Fn)(void * user, const GTEXT_CSSOM_Link_Request * request, GTEXT_CSSOM_Link_Text * text, GTEXT_CSSOM_Link_Outcome * outcome);

/**
 * @struct GTEXT_CSSOM_Markup_Options
 * @brief What a binding is made with. Start from gtext_cssom_markup_options_default().
 */
typedef struct {
  size_t size;                   ///< `sizeof` this struct as the caller's header has it. Required, and first so that the struct can grow.
  bool html;                     ///< Honor the HTML `style` and `link` elements. Default true.
  bool svg;                      ///< Honor the SVG `style` element. Default true.
  const char * base_url;         ///< The document's base URL for the sheets of `<style>` elements (opaque, copied, never parsed); NULL for none.
  GTEXT_CSSOM_Link_Fn link;      ///< Asked for the text of each `<link rel=stylesheet>`. NULL: a link has no sheet.
  void * link_user;              ///< Passed to `link`.
  /** The root of the tree the binding reads and the one a style element must be in to be **connected**: a shadow root's fragment for the binding of a shadow tree's scope. NULL: the document's
   *  root. The binding of a scope reads one tree (a shadow tree is its own, its host's tree does not hold it), and the Markup document generation it shortcuts on is the whole document's. */
  const GTEXT_Markup_Node * root;
} GTEXT_CSSOM_Markup_Options;

/** @brief The defaults, spelled out: `size` set, `html` and `svg` true, no base URL, no link callback, the document's root. */
GTEXT_API GTEXT_CSSOM_Markup_Options gtext_cssom_markup_options_default(void);

/**
 * @struct GTEXT_CSSOM_Markup_Counters
 * @brief What a binding has done, for the tests of cost and for a caller that watches.
 */
typedef struct {
  uint64_t syncs;                ///< Calls of gtext_cssom_markup_sync().
  uint64_t shortcuts;            ///< Of those, the ones that found the generation unchanged and walked nothing.
  uint64_t walks;                ///< Walks of the Markup tree.
  uint64_t nodes_visited;        ///< Nodes the walks visited.
  uint64_t texts_compared;       ///< Style elements whose text was compared with the sheet's.
  uint64_t bytes_compared;       ///< Bytes compared in that.
  uint64_t sheets_made;          ///< Sheets made for elements.
  uint64_t sheets_removed;       ///< Sheets taken out of the scope.
  uint64_t link_requests;        ///< Calls of the link callback.
  uint64_t element_calls;        ///< Calls of gtext_cssom_markup_style_element_changed().
  size_t entries;                ///< Elements the binding has a record of now.
} GTEXT_CSSOM_Markup_Counters;

/**
 * @brief Make a binding of a Markup document and a tree scope. The scope is retained; nothing is read until the first sync.
 *
 * @param document The CSSOM document the scope belongs to. Not NULL.
 * @param scope The scope whose `styleSheets` the binding keeps. Not NULL; the document's own scope or a registered one.
 * @param markup The Markup document. Not NULL; it outlives the binding.
 * @param options NULL for the defaults.
 * @param out Receives the binding; written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, a scope of another document or one that was removed, an options struct that is too small); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_markup_binding_new(GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Scope * scope, const GTEXT_Markup_Document * markup,
    const GTEXT_CSSOM_Markup_Options * options, GTEXT_CSSOM_Markup_Binding ** out);

/** @brief Free a binding. The sheets it made stay in the scope, valid, and are the caller's to take out. NULL is ignored. */
GTEXT_API void gtext_cssom_markup_binding_free(GTEXT_CSSOM_Markup_Binding * binding);

/**
 * @brief Make the scope's `styleSheets` say what the Markup document's elements say (see the file's description).
 *
 * `outcome->kind` is UNCHANGED if nothing was different, CHANGED otherwise; `observer_status` is the first failure of an observer of the batch the sync's events were delivered in.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (the document's handle was released, or the scope was removed from it); GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_CSSOM; the limits of the
 *   parse of a sheet; a status the link callback returned. A failure leaves everything consistent and the next sync goes on (see the file's description).
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_markup_sync(GTEXT_CSSOM_Markup_Binding * binding, GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief Tell the binding that one element changed (its text, its `media`, `title`, `type`, `rel`, `href` or `disabled`, or that it was inserted or removed): the work is that element's.
 *
 * The element is read now as the HTML Standard reads it. A style element that is new, or that stopped being one, is put into the list or taken out at the place the elements before it
 * say. This does not look at any other element, and does not change the generation the next sync compares with, so a sync after it still walks if the document changed.
 *
 * @param element Any node of the Markup document. Not NULL; a node that is not a style or link element, or is not connected, is one that has no sheet.
 * @return As gtext_cssom_markup_sync().
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_markup_style_element_changed(GTEXT_CSSOM_Markup_Binding * binding, const GTEXT_Markup_Node * element, GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief Give a pending `<link>` its text. The element must be one the binding asked about and is still waiting for; a change of the element since the request (its `href`, say) makes the
 *   answer stale and it is ignored (`UNCHANGED`).
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_NOT_FOUND (no pending link of that element); GTEXT_CSS_E_INVALID; the statuses of making and listing a sheet.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_markup_link_complete(GTEXT_CSSOM_Markup_Binding * binding, const GTEXT_Markup_Node * element, const GTEXT_CSSOM_Link_Text * text,
    GTEXT_CSSOM_Outcome * outcome);

/** @brief The sheet the binding made for the element (retained), or NULL if it has none. */
GTEXT_API GTEXT_CSSOM_Sheet * gtext_cssom_markup_sheet_of(const GTEXT_CSSOM_Markup_Binding * binding, const GTEXT_Markup_Node * element);

/** @brief The counters. @return GTEXT_CSS_OK or GTEXT_CSS_E_INVALID. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_markup_binding_counters(const GTEXT_CSSOM_Markup_Binding * binding, GTEXT_CSSOM_Markup_Counters * out);

/**
 * @brief The attribute binding for the `style` attribute of a Markup element (`element` is a `GTEXT_Markup_Node *`, `user` is not used). It has no `attribute_version`: the tree version
 *   of gtext_cssom_markup_tree_version() tells a declaration when to compare.
 */
GTEXT_API GTEXT_CSSOM_Attribute_Binding gtext_cssom_markup_inline_binding(void);

/** @brief The config's `tree_version` hook for a Markup document: `user` is the `GTEXT_Markup_Document *`. */
GTEXT_API uint64_t gtext_cssom_markup_tree_version(void * user);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CSSOM_MARKUP_H
