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
#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_computed.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_match.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/css/css_selector.h>
#include <ghoti.io/text/css/css_value.h>
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
  GTEXT_CSSOM_OBJECT_MEDIA_QUERY_LIST,  ///< A media query list (css_cssom_document.h).
  GTEXT_CSSOM_OBJECT_SCOPE              ///< A tree scope (css_cssom_document.h).
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

/** @brief A rule and a sheet (the structs are private; css_cssom_rule.h and css_cssom_sheet.h). */
typedef struct GTEXT_CSSOM_Rule GTEXT_CSSOM_Rule;
typedef struct GTEXT_CSSOM_Sheet GTEXT_CSSOM_Sheet;

/** @brief What the import callback says when it gives no sheet. */
typedef enum {
  GTEXT_CSSOM_IMPORT_LOADED = 0,    ///< (A sheet is given.)
  GTEXT_CSSOM_IMPORT_PENDING,       ///< The sheet will come: gtext_cssom_import_rule_complete() gives it, or NULL if it fails. The import contributes nothing until then.
  GTEXT_CSSOM_IMPORT_FAILED         ///< There will be none. The import contributes nothing.
} GTEXT_CSSOM_Import_Outcome;

/**
 * @struct GTEXT_CSSOM_Import_Request
 * @brief What the import callback is asked: valid for the call only.
 */
typedef struct {
  size_t size;                   ///< `sizeof` this struct.
  GTEXT_CSSOM_Rule * rule;       ///< The `@import` rule. Borrowed; it is **not in the sheet yet** when the call is for an `insertRule`, and must not be mutated.
  GTEXT_CSSOM_Sheet * sheet;     ///< The sheet the rule is for (borrowed).
  const char * href;             ///< The `href` of the rule as CSSOM gives it (not parsed or joined to any base), NUL-terminated.
  size_t href_length;
  const char * base_url;         ///< The sheet's base URL (opaque), or NULL.
  const char * media;            ///< The rule's media query list as CSSOM serializes it ("" for none).
  const char * supports;         ///< The `supports()` condition text, or NULL for none.
  const char * layer;            ///< The layer name (the empty string for an anonymous layer), or NULL for no `layer`.
  size_t depth;                  ///< How many import callbacks are running above this one (a callback that makes the imported sheet makes its own requests inside this call).
} GTEXT_CSSOM_Import_Request;

/**
 * @brief The import callback: make the sheet an `@import` asks for, or say it is pending or failed.
 *
 * On a sheet (`*sheet` not NULL) the callback has made a new one (a parse of the fetched text, with the options it wants) and **gives it to the rule**: the document attaches it
 * (single-owner: gtext_cssom_import_rule_complete()) and releases the callback's reference. A sheet that is refused (it has an owner, or would make a cycle) is released and recorded as a
 * diagnostic of the sheet. With `*sheet` NULL, `*outcome` is PENDING or FAILED. While the callback runs a mutation is GTEXT_CSS_E_REENTRANT, but it may make sheets, and a sheet it makes
 * is asked for its own imports (the nesting is bounded: a request deeper than 64 is a failed import). A status other than GTEXT_CSS_OK fails the call that made the request (a sheet being
 * made is dropped; an `insertRule` changes nothing), and a sheet the callback gave is released.
 */
typedef GTEXT_CSS_Status (*GTEXT_CSSOM_Import_Fn)(void * user, const GTEXT_CSSOM_Import_Request * request, GTEXT_CSSOM_Sheet ** sheet, GTEXT_CSSOM_Import_Outcome * outcome);

/** @brief Bits of what an element's state says (GTEXT_CSSOM_Config::element_state): the three facts the first steps of `getComputedStyle()` ask. */
#define GTEXT_CSSOM_ELEMENT_CONNECTED 0x1u             ///< The element is connected.
#define GTEXT_CSSOM_ELEMENT_IN_FLAT_TREE 0x2u          ///< It is part of the flat tree.
#define GTEXT_CSSOM_ELEMENT_IN_RENDERED_CONTEXT 0x4u   ///< Its shadow-including root has a browsing context that has no container, or whose container is being rendered.
#define GTEXT_CSSOM_ELEMENT_STATE_ALL 0x7u             ///< All three: what an absent `element_state` means.

/**
 * @struct GTEXT_CSSOM_Layout_Interface
 * @brief What the consumer's layout answers for the resolved values that are used values (css_cssom_computed.h). Size-prefixed and copied; every callback is optional.
 *
 * A resolved value that CSSOM defines as the used value (a `width`, a `margin-top`, an inset, a `line-height`, a transform) is a fact about a laid out box, which this
 * library does not have. Without `used_value` such a property's resolved value is its computed value and gtext_cssom_computed_origin() says so.
 */
typedef struct {
  size_t size;      ///< `sizeof` this struct as the caller's header has it. Required.
  void * user;      ///< Passed to every callback.
  /** Whether the element (or its pseudo-element, a selector list as gtext_css_style_options says, or NULL) is being rendered: the third fact of `getComputedStyle()`'s fifth step.
   *  Optional: without it the element counts as rendered. A failing status fails the read. */
  GTEXT_CSS_Status (*is_rendered)(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, bool * rendered);
  /** The used value of a property for the element: set `*applies` false for "does not apply, use computed" (the property does not apply to the box, the element is not positioned,
   *  an inset is over-constrained, the box is not a grid container ...: the resolved value is then the computed value), else give the value in `*out`, a set the library takes (a typed
   *  value, serialized like any specified value; release with gtext_css_value_set_free() if the library does not take it, which it always does on GTEXT_CSS_OK). Any status
   *  but GTEXT_CSS_OK fails the read; GTEXT_CSS_E_UNSUPPORTED names the callback and the property (gtext_cssom_computed_failure()), never a guess. Optional. */
  GTEXT_CSS_Status (*used_value)(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, GTEXT_CSS_Property_Id property, bool * applies,
      GTEXT_CSS_Value_Set ** out);
} GTEXT_CSSOM_Layout_Interface;

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
  bool quirks;                          ///< The document is in quirks mode: an element-owned declaration reads a unitless number as a length where the properties allow it (Quirks Mode 3.4); and, when `match_options` is NULL, the rule index's `quirks`. Nothing else is affected.
  bool inline_trust_notifications;      ///< The caller calls gtext_cssom_inline_style_attribute_changed() when a `style` attribute changes, and the library does not compare the attribute with what it parsed.
  size_t max_live_objects;              ///< Most live objects (declarations, sheets, rules...). Default 16,777,216. One more is GTEXT_CSS_E_LIMIT_CSSOM.
  size_t max_live_bytes;                ///< Most bytes the objects hold (their structs and the strings they keep), and the open batch's queue with them (its array and the copies in its events). Default 1 GiB. One more is GTEXT_CSS_E_LIMIT_CSSOM; a batch that has filled the budget refuses the next mutation with nothing changed.
  size_t max_inline_styles;             ///< Most element-owned declarations alive at once. Default 1,048,576. One more is GTEXT_CSS_E_LIMIT_CSSOM.
  size_t max_declarations;              ///< Most declarations in one block. Default 1,048,576, at most 2^28. One more is GTEXT_CSS_E_LIMIT_DECLARATIONS.
  size_t max_input_bytes;               ///< Longest text a `cssText` set, a value or an attribute may be. Default 64 MiB. One more is GTEXT_CSS_E_LIMIT_INPUT.
  size_t max_output_bytes;              ///< Most bytes one serialization writes. Default 64 MiB. One more is GTEXT_CSS_E_LIMIT_OUTPUT.
  size_t max_work;                      ///< Most declarations or rules one call walks (a `cssText` set, the reload of an attribute, the parse of a sheet). Default 2^24. One more is GTEXT_CSS_E_LIMIT_WORK.
  size_t max_rules;                     ///< Most rules in a sheet, at every depth. Default 1,048,576 (the parse counts every rule it makes, those it drops included). One more is GTEXT_CSS_E_LIMIT_RULES.
  size_t max_rule_nesting;              ///< Deepest nesting of rules in a sheet (a rule in a rule is 2). Default 64, at most 4,096. One more is GTEXT_CSS_E_LIMIT_RULE_NESTING.
  size_t max_diagnostics;               ///< Most diagnostics of a sheet's parse that are kept (the rest are counted). Default 1,024.
  size_t max_cached_text_bytes;         ///< Most bytes of rule `cssText` kept for leaf rules (a rule with no rules inside it). Default 64 MiB. Past it the entries added first are dropped; it is never an error.
  /* the document (css_cssom_document.h) */
  const GTEXT_CSS_Conditions * conditions;   ///< The evaluator of media and supports conditions (retained). NULL: the document makes one of the registry's defaults.
  const GTEXT_CSS_Element_Interface * element_interface;   ///< The caller's tree, for what `getComputedStyle()` computes (not owned; it outlives the document). NULL: none.
  const GTEXT_CSS_Selector_Match_Options * match_options;  ///< How selectors are matched (copied). Its `html_document` and `quirks` are the ones the rule index is built with. NULL: neither (`quirks` above is then the index's).
  GTEXT_CSSOM_Import_Fn import;         ///< Asked for the sheet of each `@import` (see GTEXT_CSSOM_Import_Fn). NULL: the caller gives the sheets with gtext_cssom_import_rule_complete().
  void * import_user;                   ///< Passed to `import`.
  size_t max_scopes;                    ///< Most tree scopes registered with the document, its own included. Default 65,536. One more is GTEXT_CSS_E_LIMIT_CSSOM.
  size_t max_adopted;                   ///< Most sheets one scope adopts, a sheet listed twice counted twice. Default 1,048,576. One more is GTEXT_CSS_E_LIMIT_CSSOM.
  size_t max_media_query_lists;         ///< Most `MediaQueryList` objects alive at once. Default 1,048,576. One more is GTEXT_CSS_E_LIMIT_CSSOM.
  size_t max_observers;                 ///< Most observers registered at once (gtext_cssom_document_observe()). Default 1,024. One more is GTEXT_CSS_E_LIMIT_CSSOM.
  size_t max_queued_events;             ///< Most events a batch holds (gtext_cssom_document_begin_batch()). Default 1,048,576. A mutation that would queue one more is GTEXT_CSS_E_LIMIT_CSSOM and changes nothing.
  /* getComputedStyle() and resolved values (css_cssom_computed.h) */
  /** The facts of the first steps of `getComputedStyle()` for an element: GTEXT_CSSOM_ELEMENT_* bits. NULL: all three are true. */
  uint32_t (*element_state)(void * user, const void * element);
  void * element_state_user;            ///< Passed to `element_state`.
  const GTEXT_CSSOM_Layout_Interface * layout;   ///< The consumer's layout (copied); NULL: none, and a resolved value that is a used value is the computed value.
  /** The context the computed styles are made with (css_computed.h): the environment (viewport, fonts, colors) and the limits. Borrowed: it outlives the document and every computed
   *  declaration; gtext_cssom_document_set_compute_context() replaces it. NULL: the document makes one from the registry, the default environment and `element_interface`. */
  const GTEXT_CSS_Compute_Context * compute_context;
  size_t max_cached_styles;             ///< Most computed styles the document's style cache holds. Default 65,536. The oldest entry goes first; the answer never depends on the cache.
  bool computed_trust_tree_changed;     ///< The caller calls gtext_cssom_document_tree_changed() on every change of its tree (it has no `tree_version`), and styles are cached from the first read; without this and without a
                                        ///< hook, nothing is cached across reads until the first call of gtext_cssom_document_tree_changed().
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
  uint64_t snapshot_compiles;       ///< Snapshots compiled (gtext_cssom_sheet_snapshot() that did not find a current one).
  uint64_t snapshot_hits;           ///< Snapshots answered from the sheet's cache.
  uint64_t snapshot_rules_reused;   ///< Top-level rules of those compiles that were read from their own syntax tree (not changed since they were read).
  uint64_t snapshot_rules_reparsed; ///< ... and those read from the parse of their own `cssText` (changed, or with a changed rule inside, or whose syntax is not what the object model writes).
  uint64_t snapshot_rules_checked;  ///< Rules whose syntax was compared once with the syntax of their serialization to decide which of the two to read.
  uint64_t index_builds;            ///< Rule indexes built (gtext_cssom_document_index() that did not find a current one).
  uint64_t index_hits;              ///< Asks answered with the index that was current.
  uint64_t index_sheets_added;      ///< Sheets (snapshots, imports included) put into an index, over all builds.
  uint64_t index_records_rebuilt;   ///< Records of the indexes built, over all builds: the work of a rebuild.
  uint64_t index_key_sheets;        ///< Listed sheets (owner and adopted, those of other documents left out) looked at to decide whether the cached index is current, over all asks.
  uint64_t import_requests;         ///< Times the import callback was called.
  uint64_t import_refused;          ///< Sheets the callback gave that were refused (owned, a cycle), and requests refused for their nesting.
  uint64_t mql_evaluations;         ///< Media query lists evaluated by an environment change or a `matches` that found its answer stale.
  uint64_t mql_notifications;       ///< Listener calls made because an answer changed.
  uint64_t events_delivered;        ///< Events handed to the observers (an event with no observer is not built, and is not counted).
  uint64_t event_calls;             ///< Calls of observer functions.
  uint64_t events_degraded;         ///< Events whose affected-set summary could not be made (an allocation failed after the mutation was complete): delivered with `summary_complete` false.
  uint64_t events_lost;             ///< Events that could not be queued in a batch although room was reserved. Always 0; a test asserts it.
  size_t live_scopes;               ///< Tree scopes registered with the document, its own included.
  uint64_t computed_computes;       ///< Styles computed for `getComputedStyle()` (gtext_css_style_compute() calls): the chain of depth d costs d for the first read and none for the next.
  uint64_t computed_cache_hits;     ///< Styles answered from the style cache.
  uint64_t computed_cache_evictions;///< Styles the cache dropped to stay within `max_cached_styles`.
  uint64_t computed_refreshes;      ///< Computed declarations whose declarations were made again (a first read, or a read after the document or the tree moved).
  uint64_t computed_layout_calls;   ///< Calls of the layout interface's `used_value`.
  size_t cached_styles;             ///< Styles in the cache now.
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

/**
 * @brief Replace the compute context (a resize that changes the environment of the computed values, a new color scheme).
 *
 * The style cache and the styles that computed declarations hold are dropped (they belong to the context they were made with, which the caller may free after this call), and every
 * computed declaration reads its values again on its next call. NULL: back to the context the document makes itself. GTEXT_CSS_E_REENTRANT from a callback of the
 * caller's tree or layout, or of an observer, that runs while a computed declaration is being read: the read holds the context. A callback may release the document handle; the read
 * finishes on what it holds, and the document's computed state goes when it ends.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_set_compute_context(GTEXT_CSSOM_Document * document, const GTEXT_CSS_Compute_Context * context);

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
