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
 * @brief The CSSOM object model: the document, the objects and their lifetime, the outcome of a mutation (CSS Object Model).
 *
 * "CSSOM" for a C library with no script engine means the **data model and the algorithms** of the CSSOM specification as a native, reference counted, mutable
 * object graph. A script engine, Web IDL, wrapper objects, events and promises are the host binding's, and the API is shaped for one: object identity is stable,
 * every object has a user-data slot with a finalizer, the specification's exceptions are status codes, and nothing a call returns is a pointer that a later
 * mutation can invalidate.
 *
 * ## The document and its objects
 *
 * A @ref GTEXT_CSSOM_Document is the unit of ownership. It holds the registry (not owned: it outlives the document), the limits, the counters (the
 * **generation**, which every mutation of any object the document owns increments, and the ids) and the caller's element and attribute interface. Declarations
 * (css_cssom_decl.h) are **objects**: created by the document, reference counted with a plain, non-atomic count (`retain` and `release`; the types are private),
 * valid for as long as anyone holds them, **including after the document handle has been released**. Everything crossing from an object to the engine side is
 * immutable and atomically counted (a typed value, a snapshot); the objects themselves are **single-writer**: all calls on objects of one document are serialized
 * by the caller, as a DOM's are, and no object has a lock. The registry, element interface and callbacks the document points at must outlive the last object.
 *
 * ## Lifetime and keep-alive
 *
 * Every object has an **internal** count (owning edges: a rule's block, a document's sheet) and an **external** count (`retain` and `release` by callers). The
 * external retain of a child adds one external hold on its parent, up the chain, released when the child's external count returns to zero: a held rule keeps its
 * sheet alive. An object is freed when both counts are zero. There is no strong cycle: a child's parent pointer is weak, and a detached subtree keeps no hold on
 * its former parent.
 *
 * gtext_cssom_object_set_user() stores the binding's wrapper. Replacing the pair runs the old finalizer first; clearing it runs it; the finalizer also runs once
 * when the object is freed, called with `(pointer, kind)`. A finalizer must not call any `gtext_cssom_*` function on the dying object. It may release and
 * `set_user` other objects and free its own memory; a mutation attempted from it, or from an observer, is refused with GTEXT_CSS_E_REENTRANT. The objects of a graph that dies together are freed parent first, so the finalizer of a rule must not call into the sheet or the rule that held it: they may be gone. Counts are
 * non-atomic: `retain`, `release`, `set_user` and every finalizer run on the document's writer thread, and a binding whose collector finalizes elsewhere
 * queues the release to the writer. Every error is a GTEXT_CSS_Status; nothing is communicated through a global, `errno` or a longjmp.
 *
 * ## Mutations, outcomes and exceptions
 *
 * A mutator returns GTEXT_CSS_OK or the status of a failure and fills a @ref GTEXT_CSSOM_Outcome. Where the specification **ignores** an operation (a value that
 * does not parse, a property that is not supported, a priority that is not `important`) the call returns GTEXT_CSS_OK and the outcome says which ignore it was, so
 * that an editor can show it and a binding can discard it. Where the specification **throws**, the status is the exception's: GTEXT_CSS_E_SYNTAX,
 * _HIERARCHY, _INDEX_SIZE, _INVALID_STATE, _NOT_ALLOWED, _NO_MODIFICATION, _NOT_FOUND and _SECURITY, whose DOMException names gtext_cssom_exception_name()
 * gives. GTEXT_CSS_E_INVALID is a misuse of the API (a NULL or unusable argument) and is never a content error. Every mutator has the **strong guarantee**: on any
 * failure, GTEXT_CSS_E_OOM and every limit included, the object, its versions and the document's counters are as they were.
 *
 * A mutation that leaves the affected declaration byte-identical changes no version and fires no notification (outcome UNCHANGED). Whether an element's `style`
 * attribute was written is a separate fact, `attribute_written` of the outcome.
 *
 * ## Determinism
 *
 * Every operation is a function of the sequence of calls, the registry and the callbacks' answers: no clock, no locale, no hardware, no address-derived value
 * (ids, order, dumps), no global state, and no allocation outside the supplied allocator. Nothing recurses on the C stack.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CSSOM_CORE_H
#define GHOTI_IO_GTEXT_CSS_CSS_CSSOM_CORE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A document: the unit of ownership of the CSSOM objects (the struct is private). */
typedef struct GTEXT_CSSOM_Document GTEXT_CSSOM_Document;

/** @brief What an object is. The kind a finalizer is called with. */
typedef enum {
  GTEXT_CSSOM_OBJECT_NONE = 0,
  GTEXT_CSSOM_OBJECT_SHEET,             ///< A style sheet (css_cssom_sheet.h).
  GTEXT_CSSOM_OBJECT_RULE,              ///< A rule (css_cssom_rule.h).
  GTEXT_CSSOM_OBJECT_DECLARATION,       ///< A declaration block (css_cssom_decl.h).
  GTEXT_CSSOM_OBJECT_MEDIA_LIST,        ///< A media list (css_cssom_media.h).
  GTEXT_CSSOM_OBJECT_MEDIA_QUERY_LIST   ///< A media query list (css_cssom_document.h).
} GTEXT_CSSOM_Object_Kind;

/** @brief What a mutator did, besides its status. */
typedef enum {
  GTEXT_CSSOM_UNCHANGED = 0,               ///< Nothing is different: the declaration was byte-identical, or there was nothing to remove.
  GTEXT_CSSOM_CHANGED,                     ///< The object changed: its version moved, and one notification went out.
  GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY,    ///< The property is not one this registry knows (decided before the value is looked at).
  GTEXT_CSSOM_IGNORED_INVALID_VALUE,       ///< The value is not valid for the property, or holds `!important`.
  GTEXT_CSSOM_IGNORED_BAD_PRIORITY,        ///< The priority is neither empty nor `important` (ASCII case-insensitive).
  GTEXT_CSSOM_IGNORED_INVALID_SELECTOR,    ///< A selector, key or media text that did not parse (css_cssom_rule.h).
  GTEXT_CSSOM_IGNORED_NOT_ALLOWED_HERE     ///< The text is valid but not allowed where it was given (css_cssom_rule.h).
} GTEXT_CSSOM_Outcome_Kind;

/**
 * @struct GTEXT_CSSOM_Outcome
 * @brief What a mutator did. Every mutator takes a pointer to one, or NULL; it is written whatever the status.
 */
typedef struct {
  GTEXT_CSSOM_Outcome_Kind kind;       ///< What happened.
  bool attribute_written;              ///< An element-owned declaration wrote the element's `style` attribute (changed or not): what a binding needs to queue the attribute's mutation record.
  GTEXT_CSS_Status observer_status;    ///< GTEXT_CSS_OK, or the first status an observer callback failed with; the mutation is never undone and delivery to the others continues.
  GTEXT_CSS_Property_Id property;      ///< For an ignored or unsupported property, the property if the registry has it; else 0.
  const char * detail;                 ///< A static sentence saying why, or NULL.
  uint64_t generation;                 ///< The document's generation after the call.
} GTEXT_CSSOM_Outcome;

/** @brief The kind of a notification (css_cssom_core.h names them; the registry of observers is css_cssom_document.h's). */
typedef enum {
  GTEXT_CSSOM_EVENT_SHEET_ADDED = 1,
  GTEXT_CSSOM_EVENT_SHEET_REMOVED,
  GTEXT_CSSOM_EVENT_SHEET_DISABLED_CHANGED,
  GTEXT_CSSOM_EVENT_SHEET_MEDIA_CHANGED,
  GTEXT_CSSOM_EVENT_RULE_INSERTED,
  GTEXT_CSSOM_EVENT_RULE_REMOVED,
  GTEXT_CSSOM_EVENT_RULE_REPLACED,       ///< A selector, condition or descriptor text changed.
  GTEXT_CSSOM_EVENT_DECLARATIONS_CHANGED,
  GTEXT_CSSOM_EVENT_INLINE_STYLE_CHANGED,
  GTEXT_CSSOM_EVENT_ADOPTED_CHANGED,
  GTEXT_CSSOM_EVENT_IMPORT_COMPLETED,
  GTEXT_CSSOM_EVENT_CONDITIONS_CHANGED
} GTEXT_CSSOM_Event_Kind;

/** @brief The finalizer of a user-data pair: called with the pointer and the kind of the object that held it. It must not call any `gtext_cssom_*` function on that object. */
typedef void (*GTEXT_CSSOM_Finalizer)(void * pointer, GTEXT_CSSOM_Object_Kind kind);

/**
 * @struct GTEXT_CSSOM_Config
 * @brief What a document is made with. Start from gtext_cssom_config_default(); a field left at 0 selects its default, and 0 never means unlimited.
 */
typedef struct {
  size_t size;                          ///< `sizeof` this struct as the caller's header has it. Required, and first so that the struct can grow.
  const GTEXT_CSS_Registry * registry;  ///< The registry. Required; it outlives every object of the document. Shared documents may share it: it is read-only.
  const GTEXT_Allocator * allocator;    ///< Every allocation of the objects and of the buffers they return; copied. NULL for the registry's.
  uint64_t first_generation;            ///< The generation counter starts here (default 0). A test sets it near 2^64 to reach GTEXT_CSS_E_LIMIT_CSSOM.
  uint64_t first_id;                    ///< The id counters (sheets, rules, sources) start here (default 1).
  /** The caller's tree is an input of the document: a counter the caller advances on any change the element interface or the attribute binding could show.
   *  NULL: the caller calls gtext_cssom_document_tree_changed() instead, or neither, and no cache is kept across calls. */
  uint64_t (*tree_version)(void * user);
  void * tree_version_user;             ///< Passed to `tree_version`.
  bool quirks;                          ///< The document is in quirks mode: an element-owned declaration reads a unitless number as a length where the properties allow it (Quirks Mode 3.4). Only element-owned declarations are affected.
  bool inline_trust_notifications;      ///< The caller calls gtext_cssom_inline_style_attribute_changed() when a `style` attribute changes, and the library does not compare the attribute with what it parsed.
  size_t max_live_objects;              ///< Most live objects (declarations, sheets, rules...). Default 16,777,216. One more is GTEXT_CSS_E_LIMIT_CSSOM.
  size_t max_live_bytes;                ///< Most bytes the objects hold (their structs and the strings they keep). Default 1 GiB. One more is GTEXT_CSS_E_LIMIT_CSSOM.
  size_t max_inline_styles;             ///< Most element-owned declarations alive at once. Default 1,048,576. One more is GTEXT_CSS_E_LIMIT_CSSOM.
  size_t max_declarations;              ///< Most declarations in one block. Default 1,048,576, at most 2^28. One more is GTEXT_CSS_E_LIMIT_DECLARATIONS.
  size_t max_input_bytes;               ///< Longest text a `cssText` set, a value or an attribute may be. Default 64 MiB. One more is GTEXT_CSS_E_LIMIT_INPUT.
  size_t max_output_bytes;              ///< Most bytes one serialization writes. Default 64 MiB. One more is GTEXT_CSS_E_LIMIT_OUTPUT.
  size_t max_work;                      ///< Most declarations or rules one call walks (a `cssText` set, the reload of an attribute, the parse of a sheet). Default 2^24. One more is GTEXT_CSS_E_LIMIT_WORK.
  size_t max_rules;                     ///< Most rules in a sheet, at every depth. Default 1,048,576 (the parse counts every rule it makes, those it drops included). One more is GTEXT_CSS_E_LIMIT_RULES.
  size_t max_rule_nesting;              ///< Deepest nesting of rules in a sheet (a rule in a rule is 2). Default 64, at most 4,096. One more is GTEXT_CSS_E_LIMIT_RULE_NESTING.
  size_t max_diagnostics;               ///< Most diagnostics of a sheet's parse that are kept (the rest are counted). Default 1,024.
  size_t max_cached_text_bytes;         ///< Most bytes of rule `cssText` kept for leaf rules (a rule with no rules inside it). Default 64 MiB. Past it the entries added first are dropped; it is never an error.
} GTEXT_CSSOM_Config;

/** @brief The defaults, spelled out: `size` set, the registry and allocator NULL, the counters at 0 and 1, the limits as documented. */
GTEXT_API GTEXT_CSSOM_Config gtext_cssom_config_default(void);

/**
 * @struct GTEXT_CSSOM_Counters
 * @brief The costs the document has counted, for the tests of cost and for a caller that watches them.
 */
typedef struct {
  uint64_t inline_reads;      ///< Validations of an element-owned declaration (one per API call that reads or edits it).
  uint64_t inline_compares;   ///< Of those, the ones that compared the attribute with what the declaration last parsed or wrote (length, then bytes).
  uint64_t inline_reparses;   ///< Of those, the ones that found it different and parsed it again.
  uint64_t inline_writes;     ///< Writes of the `style` attribute through the binding.
  size_t live_objects;        ///< Objects alive (any count above zero).
  size_t live_bytes;          ///< The bytes the live objects account.
  size_t inline_styles;       ///< Element-owned declarations alive.
  size_t tree_bytes;          ///< The bytes of the syntax trees the rules hold (and the copies of the text they were read from): the part of `live_bytes` that deleting rules does not free until the last rule of a tree goes.
  uint64_t unjudgeable_preserved; ///< Declarations a parse kept as written because the library cannot judge the property (GTEXT_CSS_E_UNSUPPORTED): a property with no grammar here, or a shorthand it cannot split.
  size_t cached_text_bytes;   ///< The bytes of `cssText` kept for leaf rules (also in `live_bytes`).
  uint64_t cache_fills;       ///< Texts put in that cache.
  uint64_t cache_hits;        ///< Reads answered from it.
  uint64_t rule_inserts;      ///< Rules made by `insertRule` and `appendRule` that were linked (with the rules inside them), the work counter of the insertion cost tests.
} GTEXT_CSSOM_Counters;

/**
 * @brief Make a document.
 *
 * @param config The config. Not NULL; `size` and `registry` are required.
 * @param out Receives the document; written only on success. Not NULL. Release it with gtext_cssom_document_release().
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, a missing registry, a `size` that is too small); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_new(const GTEXT_CSSOM_Config * config, GTEXT_CSSOM_Document ** out);

/**
 * @brief End the caller's handle on a document.
 *
 * Observers are dropped and the document's own references are released, but **objects that anyone still holds stay valid and readable**: the document's core
 * (its allocator, registry pointer, limits and counters) is alive until the last of them is released. A mutation through such an object acts on a graph that no
 * document evaluates, and a call that needs the document (gtext_cssom_inline_style()) is GTEXT_CSS_E_INVALID. NULL is ignored.
 */
GTEXT_API void gtext_cssom_document_release(GTEXT_CSSOM_Document * document);

/** @brief The document's generation: it increases by one with every mutation of any object the document owns that changed something. @return 0 for NULL. */
GTEXT_API uint64_t gtext_cssom_document_generation(const GTEXT_CSSOM_Document * document);

/** @brief The caller's notice that its tree changed (a class, an attribute, a move): the caches the document keeps for elements are stale. For a caller with no `tree_version` hook. */
GTEXT_API void gtext_cssom_document_tree_changed(GTEXT_CSSOM_Document * document);

/**
 * @brief The caller's notice that an element handle is about to be freed or reused.
 *
 * Drops that element's inline-style entry: an element-owned declaration that is still held becomes **detached** from the element (it keeps what it last held and
 * calls the binding no more), and a new handle at the same address gets a declaration of its own.
 */
GTEXT_API void gtext_cssom_document_element_released(GTEXT_CSSOM_Document * document, const void * element);

/** @brief The counters. @return GTEXT_CSS_OK or GTEXT_CSS_E_INVALID. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_counters(const GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Counters * out);

/** @brief The allocator the document's buffers come from (for gtext_cssom_buffer_free()). Valid for the life of the document. NULL for NULL. */
GTEXT_API const GTEXT_Allocator * gtext_cssom_document_allocator(const GTEXT_CSSOM_Document * document);

/** @brief Release a buffer a call returned (a property value, a `cssText`, a name): the allocator is the document's. NULL is ignored. */
GTEXT_API void gtext_cssom_buffer_free(const GTEXT_Allocator * allocator, char * buffer);

/**
 * @brief Store the binding's wrapper in an object, with the function that releases it.
 *
 * Replacing the pair runs the old finalizer first (with the old pointer); `set_user(object, NULL, NULL)` clears it and runs it; the finalizer also runs once when the
 * object is freed. A finalizer may release and `set_user` other objects and free its own memory; it must not call a `gtext_cssom_*` function on the dying object, and a
 * mutation attempted from it is refused with GTEXT_CSS_E_REENTRANT. Writer thread only.
 *
 * @param object Any CSSOM object (declaration, sheet, rule, media list, media query list). Not NULL.
 * @param pointer What to store.
 * @param finalizer Called with `pointer`, or NULL.
 * @return GTEXT_CSS_OK or GTEXT_CSS_E_INVALID.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_object_set_user(void * object, void * pointer, GTEXT_CSSOM_Finalizer finalizer);

/** @brief The pointer stored by gtext_cssom_object_set_user(); NULL if none or if the object is NULL. */
GTEXT_API void * gtext_cssom_object_user(const void * object);

/** @brief What an object is; GTEXT_CSSOM_OBJECT_NONE for NULL. */
GTEXT_API GTEXT_CSSOM_Object_Kind gtext_cssom_object_kind(const void * object);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CSSOM_CORE_H
