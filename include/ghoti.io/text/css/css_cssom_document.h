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
 * @brief The CSSOM document as the cascade sees it: tree scopes and their `styleSheets`, adopted style sheets, the frozen rule index, imports and `MediaQueryList`
 *   (CSS Object Model, Constructable Stylesheets, Media Queries 4, CSS Cascade 5 and 6).
 *
 * ## The index
 *
 * **The first thing to know about an index: it is borrowed.** gtext_cssom_document_index() returns the document's frozen @ref GTEXT_CSS_Rule_Index and keeps the owner;
 * the pointer is valid until the next ask that has to build a new one, or until the document is released. A caller that holds it across an ask retains it
 * (gtext_css_rule_index_retain()), and a thread that computes styles while the writer goes on takes its own owner with gtext_cssom_document_index_retained(): the count is
 * atomic and the index, with the snapshots it holds, goes with the last.
 *
 * The index is built from what the document holds: the user-agent and user sheets the caller registered (compiled, as given), then the author sheets: for each tree scope in
 * order of its context number, the owner sheets of its `styleSheets` in list order, then its adopted sheets. A sheet takes part when it is not `disabled`, its media query list
 * is true for the document's conditions, and it belongs to the document (an adopted sheet's constructor document is the scope's document; an owner sheet's too). Each takes
 * part as its snapshot (gtext_cssom_sheet_snapshot()), registered with its origin, the scope's context number, handle, shadow flag and parent scope, and the sheet's implicit
 * root (the parent of its owner node), so the checks of the index itself (a shadow tree's context greater than its host's) are the checks of the registrations. A sheet that
 * is listed more than once in a scope's adopted list takes part once, at its **last** position.
 *
 * The index is **cached** on the tuple of its inputs: the greatest change stamp among every sheet the scopes list, with the sheets they import (the composite versions of
 * css_cssom_sheet.h), the document's structure version (a scope, a `styleSheets` list or an adopted list changed), its registration version and the identity of its
 * conditions object. The same pointer is returned while the tuple is unchanged; any change builds one new index, lazily, at the next ask, however many changes came before, so n
 * mutations and one ask cost one build (O(the records of the document)). Replacing the environment with the same conditions object changes nothing, and replacing it with
 * another builds a new index whose **snapshots are the same objects**: the compiled sheets do not depend on the environment, so a resize recompiles nothing.
 *
 * ## Tree scopes
 *
 * A @ref GTEXT_CSSOM_Scope is a tree scope: the document's, or a shadow tree's. The document makes scope 0 itself. Each scope has an ordered list of owner sheets (`document.styleSheets`,
 * `shadowRoot.styleSheets`: what the caller puts there in tree order of the owner nodes, since the library does not see the caller's tree) and an ordered list of adopted sheets
 * (`adoptedStyleSheets`, constructed sheets only, each checked to be the document's own). A scope moved to another document (gtext_cssom_scope_reparent()) **keeps both lists
 * unchanged**; the new document's index takes the sheets that were made by that document and the others stay listed, take part nowhere and take part again if the scope returns.
 *
 * ## Imports
 *
 * The library never fetches. When a sheet is made from text, or an `@import` rule is inserted, the document's import callback is asked for the sheet; it answers with a new
 * sheet (one owner given to the rule), or says the import is **pending** (nothing contributes until gtext_cssom_import_rule_complete()) or **failed**. Imports are single-owner
 * (css_cssom_rule.h), a refusal of a sheet that has an owner or would make a cycle is a diagnostic of the sheet and the import contributes nothing.
 *
 * ## MediaQueryList
 *
 * gtext_cssom_match_media() makes the object `matchMedia()` returns. gtext_cssom_document_set_conditions() is the environment change and runs "evaluate media queries and report
 * changes": every list that has a listener is evaluated once (lists of the same text once between them), and a list whose answer changed calls its listeners in registration order,
 * after the change is complete. A list with no listener is evaluated when `matches` asks.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CSSOM_DOCUMENT_H
#define GHOTI_IO_GTEXT_CSS_CSS_CSSOM_DOCUMENT_H

#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_cssom_core.h>
#include <ghoti.io/text/css/css_cssom_media.h>
#include <ghoti.io/text/css/css_cssom_rule.h>
#include <ghoti.io/text/css/css_cssom_sheet.h>
#include <ghoti.io/text/css/css_layer.h>
#include <ghoti.io/text/css/css_sheet.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- tree scopes ---------------------------------------------------------------------------------------------------------------------------- */

/** @brief A tree scope: the document's or a shadow tree's (the struct is private). Reference counted with a plain count: the document's writer thread only. */
typedef struct GTEXT_CSSOM_Scope GTEXT_CSSOM_Scope;

/**
 * @struct GTEXT_CSSOM_Scope_Options
 * @brief What a tree scope is made with. Start from gtext_cssom_scope_options_default().
 */
typedef struct {
  size_t size;                    ///< `sizeof` this struct as the caller's header has it. Required, and first so that the struct can grow.
  const void * handle;            ///< The scope's handle, the interface's `tree_scope` (opaque, never dereferenced). Required for a shadow scope; the document's is NULL.
  bool shadow;                    ///< The scope is a shadow tree.
  GTEXT_CSSOM_Scope * parent;     ///< The scope that holds the host element. NULL: the document's scope. Kept alive by this one.
  uint32_t context;               ///< The context number, assigned in shadow-including tree order (css_sheet.h). 0 takes the next number in the order the scopes are made, which is right when a host's scope is made before its shadow trees.
  const void * implicit_root;     ///< The root of an `@scope` rule with no prelude in a sheet adopted by this scope: the shadow host. NULL for none.
} GTEXT_CSSOM_Scope_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSSOM_Scope_Options gtext_cssom_scope_options_default(void);

/**
 * @brief Register a tree scope with the document.
 *
 * @param document The document. Not NULL.
 * @param options NULL for the defaults (a scope with no handle, which is not a shadow scope).
 * @param out Receives the scope with one external reference of the caller's, which the document also holds until gtext_cssom_scope_remove(); written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, a shadow scope with no handle, a parent of another document or one that is not registered); GTEXT_CSS_E_LIMIT_CSSOM
 *   (`max_scopes`, or the objects); GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_scope_new(GTEXT_CSSOM_Document * document, const GTEXT_CSSOM_Scope_Options * options, GTEXT_CSSOM_Scope ** out);

/** @brief The document's own scope (context 0), borrowed: it lives as long as the document. NULL for NULL or a released document. */
GTEXT_API GTEXT_CSSOM_Scope * gtext_cssom_document_scope(GTEXT_CSSOM_Document * document);

/** @brief Take another external reference. Writer thread only. @return The scope. */
GTEXT_API GTEXT_CSSOM_Scope * gtext_cssom_scope_retain(GTEXT_CSSOM_Scope * scope);

/** @brief Give one back. NULL is ignored. A registered scope is held by its document as well. */
GTEXT_API void gtext_cssom_scope_release(GTEXT_CSSOM_Scope * scope);

/** @brief The scope's id, from the document's counter. 0 for NULL. */
GTEXT_API uint64_t gtext_cssom_scope_id(const GTEXT_CSSOM_Scope * scope);

/** @brief The scope's context number (0 for the document's), its handle, whether it is a shadow tree, and its parent (borrowed; NULL for the document's scope and for a child of it). */
GTEXT_API uint32_t gtext_cssom_scope_context(const GTEXT_CSSOM_Scope * scope);
GTEXT_API const void * gtext_cssom_scope_handle(const GTEXT_CSSOM_Scope * scope);
GTEXT_API bool gtext_cssom_scope_shadow(const GTEXT_CSSOM_Scope * scope);
GTEXT_API GTEXT_CSSOM_Scope * gtext_cssom_scope_parent(const GTEXT_CSSOM_Scope * scope);

/** @brief The document the scope belongs to now, or NULL for a scope that was removed or whose document was released. */
GTEXT_API GTEXT_CSSOM_Document * gtext_cssom_scope_document(const GTEXT_CSSOM_Scope * scope);

/**
 * @brief Take the scope out of its document: its owner sheets and adopted sheets no longer take part (one `SHEET_REMOVED` for each owner sheet, whose `owner_node` becomes NULL, and one
 *   `ADOPTED_CHANGED` if it adopted any), and the scope is a valid, empty object that belongs to no document.
 *
 * @return GTEXT_CSS_OK (`UNCHANGED` for a scope that is already removed); GTEXT_CSS_E_INVALID (NULL, or the document's own scope); GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_LIMIT_CSSOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_scope_remove(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief Move the scope to another document (a shadow root adopted by a template, a node adopted into another document), with `parent` (a scope of that document) or NULL for its own scope as the
 *   scope that holds the host.
 *
 * Both lists are kept (see the file's description). The old document and the new one each send one `ADOPTED_CHANGED` if the scope has any sheet to move out of or into their cascade. Moving
 * a scope to the document it is in with the parent it has is `UNCHANGED`.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (NULL, the document's own scope, a parent that is not a registered scope of `document`, or a scope that was removed); GTEXT_CSS_E_REENTRANT;
 *   GTEXT_CSS_E_LIMIT_CSSOM (`max_scopes` of the new document); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_scope_reparent(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Scope * parent, GTEXT_CSSOM_Outcome * outcome);

/* ---- `styleSheets` -------------------------------------------------------------------------------------------------------------------------- */

/** @brief How many owner sheets the scope lists. 0 for NULL. */
GTEXT_API size_t gtext_cssom_scope_sheet_count(const GTEXT_CSSOM_Scope * scope);

/** @brief `styleSheets[index]`: the sheet at the index, retained, or NULL past the end. */
GTEXT_API GTEXT_CSSOM_Sheet * gtext_cssom_scope_sheet_at(const GTEXT_CSSOM_Scope * scope, size_t index);

/**
 * @brief Put an owner sheet into `styleSheets` at the index (the caller's tree order of the owner nodes).
 *
 * The scope holds the sheet. A sheet that is constructed, that an `@import` holds, that is in a scope already, or that belongs to another document is refused. One `SHEET_ADDED`.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INDEX_SIZE; GTEXT_CSS_E_INVALID (NULL, a sheet of another document); GTEXT_CSS_E_INVALID_STATE (constructed, owned by an import, or already in a scope);
 *   GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_LIMIT_CSSOM; GTEXT_CSS_E_OOM. Nothing changes on a failure.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_scope_insert_sheet(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Sheet * sheet, size_t index, GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief Take an owner sheet out of `styleSheets`: it leaves the cascade, **stays a valid, mutable object** with `owner_node` NULL that keeps its rules, and one `SHEET_REMOVED` is sent.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_NOT_FOUND (the sheet is not in this scope); GTEXT_CSS_E_INVALID; GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_LIMIT_CSSOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_scope_remove_sheet(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Sheet * sheet, GTEXT_CSSOM_Outcome * outcome);

/* ---- `adoptedStyleSheets` ------------------------------------------------------------------------------------------------------------------- */

/** @brief How many sheets the scope adopted, a sheet listed twice counted twice. 0 for NULL. */
GTEXT_API size_t gtext_cssom_scope_adopted_count(const GTEXT_CSSOM_Scope * scope);

/** @brief `adoptedStyleSheets[index]`, retained, or NULL past the end. */
GTEXT_API GTEXT_CSSOM_Sheet * gtext_cssom_scope_adopted_at(const GTEXT_CSSOM_Scope * scope, size_t index);

/**
 * @brief Insert a constructed sheet into the scope's adopted list at the index. O(the length of the list).
 *
 * The sheet must be constructed by the document the scope belongs to now (GTEXT_CSS_E_NOT_ALLOWED: CSSOM's NotAllowedError for a sheet that is not constructed or was constructed by
 * another document). The same sheet may be listed more than once; it takes part once, at its last position. One `ADOPTED_CHANGED`. The list is bounded by the document's `max_adopted`.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_NOT_ALLOWED; GTEXT_CSS_E_INDEX_SIZE; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_LIMIT_CSSOM; GTEXT_CSS_E_OOM. Nothing changes on a failure.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_scope_adopted_insert(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Sheet * sheet, size_t index, GTEXT_CSSOM_Outcome * outcome);

/** @brief Remove the entry at the index. One `ADOPTED_CHANGED`. @return GTEXT_CSS_OK; GTEXT_CSS_E_INDEX_SIZE; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_LIMIT_CSSOM. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_scope_adopted_remove(GTEXT_CSSOM_Scope * scope, size_t index, GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief The `adoptedStyleSheets` setter: replace the whole list. Every sheet is checked first (GTEXT_CSS_E_NOT_ALLOWED), and the list is left as it was on any failure. A list equal to the
 *   current one is `UNCHANGED`.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_scope_adopted_set(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Sheet * const * sheets, size_t count, GTEXT_CSSOM_Outcome * outcome);

/* ---- the user-agent and user sheets, the environment ---------------------------------------------------------------------------------------- */

/**
 * @brief Register a compiled sheet of the user-agent or user origin (they are immutable and the caller's: no CSSOM object stands for them). The index takes them first, in order of
 *   registration. The sheet is retained. One `SHEET_ADDED` with no sheet object.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (NULL, an origin that is neither); GTEXT_CSS_E_INVALID_STATE (the sheet is registered already); GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_LIMIT_CSSOM;
 *   GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_register_sheet(GTEXT_CSSOM_Document * document, GTEXT_CSS_Origin origin, GTEXT_CSS_Sheet * sheet, GTEXT_CSSOM_Outcome * outcome);

/** @brief Take a registered sheet out. @return GTEXT_CSS_OK; GTEXT_CSS_E_NOT_FOUND; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_LIMIT_CSSOM. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_unregister_sheet(GTEXT_CSSOM_Document * document, GTEXT_CSS_Sheet * sheet, GTEXT_CSSOM_Outcome * outcome);

/** @brief The conditions object the document evaluates media and supports conditions with, borrowed. NULL for NULL. */
GTEXT_API const GTEXT_CSS_Conditions * gtext_cssom_document_conditions(const GTEXT_CSSOM_Document * document);

/**
 * @brief The environment change (a resize, a scheme switch): the document retains the new conditions object, the generation increases (one `CONDITIONS_CHANGED`), the next ask for the index
 *   builds a new one, and "evaluate media queries and report changes" runs: see the file's description.
 *
 * The evaluation of the lists with listeners is done **before** anything changes, so a failure of it (a unit the environment cannot answer, a limit, memory) leaves the document as it was.
 * The same object as the document has is `UNCHANGED`. Listeners run after the change is complete, may read any object and may not mutate (GTEXT_CSS_E_REENTRANT); the first status one of them
 * returns is `outcome->observer_status`, and delivery goes on.
 *
 * @param conditions The new conditions object, made with the document's registry. Not NULL; retained.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_LIMIT_CSSOM; the statuses of evaluating a media query list; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_set_conditions(GTEXT_CSSOM_Document * document, const GTEXT_CSS_Conditions * conditions, GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief The document's frozen rule index (see the file's description). **Borrowed**: valid until the next ask that builds a new one, and until the document is released.
 *
 * @param document The document. Not NULL.
 * @param out Receives the index; written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_UNSUPPORTED (a unit of a media query that the environment cannot answer); GTEXT_CSS_E_INVALID with the registrations contradicting
 *   each other (a shadow tree's context not greater than its host's); the limits of a snapshot and of the index; GTEXT_CSS_E_OOM. On any failure the previous index is kept and nothing changed.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_index(GTEXT_CSSOM_Document * document, const GTEXT_CSS_Rule_Index ** out);

/** @brief The same, with an owner for the caller: release it with gtext_css_rule_index_free(). This is what a reader thread takes while the writer goes on. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_index_retained(GTEXT_CSSOM_Document * document, GTEXT_CSS_Rule_Index ** out);

/* ---- imports ---------------------------------------------------------------------------------------------------------------------------- */

/* The import callback (GTEXT_CSSOM_Import_Fn, GTEXT_CSSOM_Import_Request, GTEXT_CSSOM_Import_Outcome) is declared in css_cssom_core.h, with the config that names it. */

/** @brief How many `@import` rules of the sheet's own list are pending: what a `replace()` promise (or a load event) waits on. 0 for NULL. */
GTEXT_API size_t gtext_cssom_sheet_pending_imports(const GTEXT_CSSOM_Sheet * sheet);

/** @brief Whether the `@import` rule is pending (the callback said so and gtext_cssom_import_rule_complete() has not come). */
GTEXT_API bool gtext_cssom_import_rule_pending(const GTEXT_CSSOM_Rule * rule);

/* ---- MediaQueryList --------------------------------------------------------------------------------------------------------------------- */

/** @brief A `MediaQueryList` (the struct is private). Reference counted like every object: the document's writer thread only. */
typedef struct GTEXT_CSSOM_Media_Query_List GTEXT_CSSOM_Media_Query_List;

/**
 * @brief A listener: called with the list and its new `matches` when the environment changed the answer. It runs after the change, may read any object and may not mutate.
 * @return GTEXT_CSS_OK, or a status that is reported as the call's `observer_status`.
 */
typedef GTEXT_CSS_Status (*GTEXT_CSSOM_Media_Listener)(void * user, GTEXT_CSSOM_Media_Query_List * list, bool matches);

/**
 * @brief `matchMedia(text)`: parse the text as a media query list (an invalid query is `not all`, the empty text matches) and return the object, with one external reference of the caller's.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_LIMIT_CSSOM (`max_media_query_lists`, the objects); the limits of the parse; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_match_media(GTEXT_CSSOM_Document * document, const char * text, size_t length, GTEXT_CSSOM_Media_Query_List ** out);

/** @brief Take another external reference. @return The list. */
GTEXT_API GTEXT_CSSOM_Media_Query_List * gtext_cssom_media_query_list_retain(GTEXT_CSSOM_Media_Query_List * list);

/** @brief Give one back. NULL is ignored. */
GTEXT_API void gtext_cssom_media_query_list_release(GTEXT_CSSOM_Media_Query_List * list);

/** @brief `media`: the query list as CSSOM serializes it, in a new buffer (gtext_cssom_buffer_free()). */
GTEXT_API GTEXT_CSS_Status gtext_cssom_media_query_list_media(const GTEXT_CSSOM_Media_Query_List * list, char ** out, size_t * length);

/**
 * @brief `matches`: the list evaluated for the document's conditions, **cached per conditions object**: asking again without an environment change evaluates nothing.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_UNSUPPORTED (a unit the environment cannot answer); the limits of the evaluation; GTEXT_CSS_E_OOM. After the document handle was released the last answer.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_media_query_list_matches(GTEXT_CSSOM_Media_Query_List * list, bool * matches);

/**
 * @brief `addListener` / `addEventListener('change')`: add a listener. The same function and user pair is added once (UNCHANGED). Allowed from inside a listener: the new one is not called for the
 *   event being delivered. @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_UNSUPPORTED and the other statuses of an evaluation; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_media_query_list_add_listener(GTEXT_CSSOM_Media_Query_List * list, GTEXT_CSSOM_Media_Listener fn, void * user);

/** @brief `removeListener`: remove the pair; one that is not there is ignored. Allowed from inside a listener: a listener removed is not called for the event being delivered. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_media_query_list_remove_listener(GTEXT_CSSOM_Media_Query_List * list, GTEXT_CSSOM_Media_Listener fn, void * user);

/** @brief How many listeners the list has. 0 for NULL. */
GTEXT_API size_t gtext_cssom_media_query_list_listener_count(const GTEXT_CSSOM_Media_Query_List * list);

/* ---- CSS.supports() ----------------------------------------------------------------------------------------------------------------------- */

/**
 * @brief `CSS.supports(property, value)` over the document's conditions object (css_condition.h): whether the property is supported with the value, with the document's support policy.
 * @param property The property name, NUL-terminated (a custom property is supported with any value). @param value The value text, NUL-terminated.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument); the limits of the evaluation; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_supports(const GTEXT_CSSOM_Document * document, const char * property, const char * value, bool * result);

/** @brief `CSS.supports(conditionText)` over the document's conditions object: the text is a `<supports-condition>` (a bare declaration is allowed). */
GTEXT_API GTEXT_CSS_Status gtext_cssom_supports_text(const GTEXT_CSSOM_Document * document, const char * text, size_t length, bool * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CSSOM_DOCUMENT_H
