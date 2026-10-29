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
 * @file css_sheet.h
 * @brief Style sheets, the rule index and the cascade entries of an element.
 *
 * Normative text: CSS Cascading and Inheritance 5 (the order of appearance, layers, `@import`) and 6 (`@scope` and its proximity), CSS Nesting 1, CSS Namespaces 3, CSS Scoping 1
 * and CSS Shadow Parts 1 (the contexts of the shadow trees), as served 2026-10-28.
 *
 * ## The sheet
 *
 * gtext_css_sheet_compile() turns a parsed style sheet (css_core.h) into an immutable, reference counted, thread-shareable @ref GTEXT_CSS_Sheet. Its compiled form is one record
 * for each (style rule, complex selector), with the selector (compiled once, css_selector.h), its specificity (with `&` resolved against the parent rule), the typed
 * declarations of the rule (parsed once through the registry and shared by every element they apply to), the rule's layer (a node of the sheet's own layer tree, so a sheet does
 * not depend on a document's registry), its `@scope` chain, and its place among the rules. **Nesting is resolved without textual expansion**: `&` refers to the parent's selector
 * list and the matcher evaluates it as `:is()` of it, so a rule nested d deep inside lists of k selectors costs O(d x k) to compile; a relative nested selector (or one with no
 * `&`) gets the implied `&` and its descendant combinator, with the specificity of an explicit one; a run of declarations after a nested rule (a nested declarations rule)
 * has its own place among the rules, so that a declaration after a nested rule beats the nested rule's declaration of equal specificity. `@namespace` fills the prefix map of
 * the selectors after it; a selector with a prefix it does not declare is invalid. `@layer` statements and blocks, anonymous and nested ones, are recorded. `@import` is read where
 * it may be, and the sheet asks the caller for the imported sheet: the library never fetches.
 *
 * **A sheet keeps its conditions as data.** Compiling evaluates nothing: every conditional group rule (`@media`, `@supports`, `@container`, `@starting-style`, `@when` and `@else`,
 * and the conditions of an `@import`) becomes a **condition node** of the sheet (the parsed condition, immutable, with a link to the node it is inside), and every record, layer occurrence and
 * import carries the node it is under. So the same compiled sheet serves any number of environments: a resize is a new @ref GTEXT_CSS_Conditions and a new index, and the sheet is untouched.
 * A condition that cannot be read makes a node that never applies (a media query list replaces an invalid query by `not all`, as Media Queries 4 says), with a diagnostic. Rules nested in a style rule
 * (`.a { @media (...) { color: red; & b { } } }`) are condition nodes over the nested records, a declaration directly in one being the implied `& { }`. `@else` joins the rule before it (the draft's
 * chain): it applies only if no rule earlier in its chain did. `@import ... scope()` wraps the imported sheet's rules in an implicit `@scope`, shared through a prefix link (O(1) a record).
 * The caller's import callback is asked for **every** import, with the conditions parsed in the request, so that a caller can decline to fetch what is false for its environment; the index drops
 * the imports and the records whose conditions are false.
 *
 * ## The index
 *
 * An index is built for one @ref GTEXT_CSS_Conditions (GTEXT_CSS_Rule_Index_Options::conditions, required when a sheet holds a condition node). It evaluates every node
 * **once**, as the sheet is added (a node under a node is evaluated after its parent, so n nested rules cost O(n)), drops the records and imports whose chain is false, and declares
 * the layers of the occurrences whose chain is true **before** the next occurrence, so a layer introduced by a false `@media` or a false `@import ... supports()` is never declared
 * (Cascade 5, 6.4.3), while one introduced inside an `@container` is, because the condition can differ for elements. A chain with a container node keeps its record flagged
 * element-dependent, and gtext_css_rules_collect() asks the container interface of the conditions about it for the element, after the selector has matched. Records under
 * `@starting-style` are skipped by default and take part in the cascade with GTEXT_CSS_Rules_Collect_Options::starting_style.
 *
 * A @ref GTEXT_CSS_Rule_Index holds the sheets of a document, flattened: gtext_css_rule_index_add_sheet() merges a sheet's layers into the layer registry of its context, in the
 * order the sheets are added (an imported sheet goes in before the sheet that imports it, at the place of the import), and gives every sheet and rule one order of appearance.
 * The records are in buckets by the id, a class or the local name of the subject (the first of the three that the subject compound has; the rest are in one list), so an
 * element tests the records of the keys it has and the rules with no key. The keys are compared as the match context compares them: `GTEXT_CSS_Rule_Index_Options::html_document` and
 * `::quirks` must be what the match context's options are. gtext_css_rules_collect() adds to a candidate set (css_cascade.h) one entry for each matching
 * declaration with every key the cascade defines.
 *
 * **The context number (an open item of the cascade, decided).** An entry's `context` is the number of the tree scope of the sheet that owns the rule, whatever element matched.
 * The caller registers a sheet with its scope handle and its context number, assigned in shadow-including tree order (the document is 0; a shadow root's number is greater than its
 * host's scope's). So the `:host` rules and the `::slotted()` rules of a shadow tree's sheet carry that tree's number and lose, for normal declarations, to rules of the outer tree on
 * the same host, and win for `!important` ones; a `::part()` rule of an outer sheet carries the outer number. The index checks the registrations.
 *
 * **`@scope` proximity (an open item, decided).** A rule in `@scope (start) to (end)` matches an element E when some scoping root R (an element matching `start`, with `:scope`
 * what the enclosing scope's root is) is an inclusive ancestor of E, no element from E up to but excluding R matches `end` (read as the rules inside are, relative to R), and the
 * selector, with the implied `& ` (which is `:where(:scope)` there) in front of it unless it contains `&` or `:scope`, matches with `:scope` = R. The entry is `scoped`, and `scope_distance` is the least number of generational hops from a root
 * that qualifies to E (the subject of a scoped rule is always an inclusive descendant of its root). A rule outside any `@scope` is not scoped. The innermost `@scope` counts.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_SHEET_H
#define GHOTI_IO_GTEXT_CSS_CSS_SHEET_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_cascade.h>
#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_layer.h>
#include <ghoti.io/text/css/css_match.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/css/css_selector.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A compiled style sheet (the struct is private). Immutable, reference counted, shareable between threads. */
typedef struct GTEXT_CSS_Sheet GTEXT_CSS_Sheet;

/** @brief The rules of a document's sheets, flattened and indexed (the struct is private). Frozen before it is used; then read-only and shareable. */
typedef struct GTEXT_CSS_Rule_Index GTEXT_CSS_Rule_Index;

/** @brief What an `@import` asks of the caller. The strings and the parsed conditions are valid for the call. */
typedef struct {
  GTEXT_CSS_String url;       ///< The URL as written (a `url()` token's value or a string).
  bool layered;               ///< The import has `layer` or `layer(name)`.
  bool layer_anonymous;       ///< `layer` with no name.
  GTEXT_CSS_String layer;     ///< `layer(name)`: the name as CSS text (`a.b`).
  bool has_supports;          ///< The import has `supports(...)`.
  GTEXT_CSS_String supports;  ///< Its argument as CSS text.
  bool has_media;             ///< The import has a media query list.
  GTEXT_CSS_String media;     ///< It as CSS text.
  bool has_scope;             ///< The import has `scope` or `scope(...)`.
  GTEXT_CSS_String scope;     ///< Its argument as CSS text (`(.card) to (.limit)`); empty for `scope` and `scope()`, the implicit scoping root.
  const GTEXT_CSS_Media_List * media_list;       ///< The media query list, parsed (NULL if the import has none): evaluate it with gtext_css_conditions_matches_list() to decide whether to fetch.
  const GTEXT_CSS_Supports_Condition * supports_condition; ///< The `supports()` condition, parsed (NULL if the import has none): gtext_css_conditions_evaluate_supports().
  const GTEXT_CSS_Rule * rule; ///< The `@import` rule (its span, for a diagnostic).
} GTEXT_CSS_Import_Request;

/**
 * @brief Give the compiled sheet for an `@import`, or NULL (a failed or refused fetch: it contributes nothing).
 *
 * The library never fetches. The sheet returned is **one owner given to the importing sheet** (return gtext_css_sheet_retain() of a cached one), and it must have been compiled with
 * the same registry. The call is made while the importing sheet is compiled, in the order of the imports.
 */
typedef GTEXT_CSS_Sheet * (*GTEXT_CSS_Import_Fn)(void * user, const GTEXT_CSS_Import_Request * request);

/** @brief What compiling a sheet may be told. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;  ///< NULL for the registry's.
  bool quirks;                        ///< The sheet is read in quirks mode (a unitless length in a declaration, css_property.h).
  GTEXT_CSS_Import_Fn import;         ///< The source of imported sheets; NULL: an import contributes nothing.
  void * import_user;                 ///< Passed to `import`.
  GTEXT_CSS_Selector_Options selector; ///< The limits of reading a selector (`mode` and `namespaces` are the sheet's own); NULL fields are the defaults.
  size_t max_rule_depth;              ///< Deepest nesting of rules. Default 64; at most 4,096. One more is GTEXT_CSS_E_LIMIT_RULE_NESTING.
  size_t max_compiled_bytes;          ///< Most memory of the compiled form. Default 256 MiB. One more is GTEXT_CSS_E_LIMIT_SHEET_SIZE.
  size_t max_diagnostics;             ///< Most diagnostics kept (the rest are counted). Default 1,024.
  size_t max_import_depth;            ///< Deepest chain of imports (an imported sheet's own imports); default 16.
  size_t max_conditions;              ///< Most condition nodes in a sheet. Default 1,048,576. One more is GTEXT_CSS_E_LIMIT_CONDITIONS.
  GTEXT_CSS_Media_Options condition;  ///< The limits of reading a condition (css_media.h): queries, nodes, depth, diagnostics. Zeroed fields are the defaults.
} GTEXT_CSS_Sheet_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Sheet_Options gtext_css_sheet_options_default(void);

/** @brief What was dropped, and why. */
typedef enum {
  GTEXT_CSS_SHEET_DIAG_INVALID_SELECTOR = 0, ///< A style rule's selector is not valid: the rule is dropped.
  GTEXT_CSS_SHEET_DIAG_UNKNOWN_PROPERTY,     ///< A declaration's property is not in the registry.
  GTEXT_CSS_SHEET_DIAG_INVALID_VALUE,        ///< A declaration's value is not one of the property.
  GTEXT_CSS_SHEET_DIAG_UNJUDGEABLE,          ///< The library cannot judge the property (GTEXT_CSS_E_UNSUPPORTED): the declaration is dropped.
  GTEXT_CSS_SHEET_DIAG_INVALID_RULE,         ///< An at-rule that is not valid where it is (a late `@import`, a bad `@layer` name, a bad `@scope` prelude, ...).
  GTEXT_CSS_SHEET_DIAG_IMPORT_FAILED         ///< The import callback gave no sheet.
} GTEXT_CSS_Sheet_Diag_Kind;

/** @brief A rule or declaration the sheet dropped. */
typedef struct {
  GTEXT_CSS_Sheet_Diag_Kind kind; ///< Why.
  GTEXT_CSS_Span span;            ///< Where, in the bytes of the style sheet.
  GTEXT_CSS_Id id;                ///< The C0 id of the rule or declaration.
  const char * reference;         ///< A static reference to the sentence of the draft that decides it, never NULL.
} GTEXT_CSS_Sheet_Diagnostic;

/** @brief What compiling reports. Plain data; filled whatever happens. */
typedef struct {
  GTEXT_CSS_Status status;   ///< What the call returned.
  const char * message;      ///< A static description; never NULL after a call.
  GTEXT_CSS_Span span;       ///< A limit, or the rule that made the call unsupported: where.
  char name[96];             ///< What is concerned (`conditions`, a property), cut to fit.
  const char * detail;       ///< A static sentence saying which rule was broken, or NULL.
} GTEXT_CSS_Sheet_Result;

/**
 * @brief Compile a parsed style sheet.
 *
 * @param tree A tree of a style sheet (GTEXT_CSS_TREE_STYLESHEET or _STYLESHEET_CONTENTS). Not NULL; it need not outlive the sheet (everything is copied).
 * @param registry The property registry the declarations are resolved through. Not NULL; it must outlive the sheet.
 * @param options NULL for the defaults.
 * @param out Receives the sheet (one owner). Not NULL; written only on success.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_RULE_NESTING, _LIMIT_SHEET_SIZE, _LIMIT_CONDITIONS (`max_conditions`), and the limits of selectors,
 *   layers and conditions.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_sheet_compile(const GTEXT_CSS_Tree * tree, const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Sheet_Options * options,
    GTEXT_CSS_Sheet ** out, GTEXT_CSS_Sheet_Result * result);

/** @brief Another owner of the sheet (it is freed when the last lets go). Returns `sheet`; NULL is ignored. */
GTEXT_API GTEXT_CSS_Sheet * gtext_css_sheet_retain(GTEXT_CSS_Sheet * sheet);

/** @brief One owner fewer; the last frees the sheet. NULL is ignored. */
GTEXT_API void gtext_css_sheet_free(GTEXT_CSS_Sheet * sheet);

/** @brief One record of a sheet: a complex selector of a style rule (or of a nested declarations rule) with what it carries. */
typedef struct {
  const GTEXT_CSS_Selector_List * selector; ///< The selector list the record belongs to (shared by the records of its complex selectors; owned by the sheet).
  size_t complex_index;                     ///< Which complex selector of the list.
  GTEXT_CSS_Specificity specificity;        ///< With `&` resolved against the parent rule (`&` is zero directly in `@scope`, and (0,1,0) as `:scope` at the top level).
  uint32_t rule;                            ///< The order of appearance of the rule (its run of declarations), from 0 in the sheet.
  size_t declaration_count;                 ///< Declarations of the rule that are valid.
  uint32_t layer;                           ///< The sheet's own layer node (0: unlayered); gtext_css_sheet_layer_path() writes its name.
  bool nested;                              ///< The rule is nested in a style rule (or is its nested declarations).
  size_t scope_depth;                       ///< How many `@scope` rules enclose it.
  uint32_t dependencies;                    ///< GTEXT_CSS_DEP_* of the selector.
  uint32_t condition;                       ///< The innermost condition node the rule is under (0: none); gtext_css_sheet_condition() reads it.
  GTEXT_CSS_Span span;                      ///< The rule.
} GTEXT_CSS_Sheet_Record;

/** @brief How many records the sheet has (not those of the sheets it imports). */
GTEXT_API size_t gtext_css_sheet_record_count(const GTEXT_CSS_Sheet * sheet);

/** @brief The `index`th record, in order of appearance; false past the end. */
GTEXT_API bool gtext_css_sheet_record(const GTEXT_CSS_Sheet * sheet, size_t index, GTEXT_CSS_Sheet_Record * out);

/** @brief The declarations of a record's rule as (property id, important) pairs, in order; returns how many (all of them, however many fit). A custom property has id 0. */
GTEXT_API size_t gtext_css_sheet_record_declarations(const GTEXT_CSS_Sheet * sheet, size_t index, GTEXT_CSS_Property_Id * properties, bool * important,
    size_t capacity);

/** @brief How many layers the sheet's own tree has. */
GTEXT_API size_t gtext_css_sheet_layer_count(const GTEXT_CSS_Sheet * sheet);

/** @brief The dotted path of a layer of the sheet's tree (as gtext_css_layer_path()); the length of the whole path, as snprintf. */
GTEXT_API size_t gtext_css_sheet_layer_path(const GTEXT_CSS_Sheet * sheet, uint32_t layer, char * buffer, size_t capacity);

/** @brief How many sheets the sheet imports (the ones that were found, in order). */
GTEXT_API size_t gtext_css_sheet_import_count(const GTEXT_CSS_Sheet * sheet);

/** @brief The `index`th imported sheet (not retained for you), or NULL. */
GTEXT_API const GTEXT_CSS_Sheet * gtext_css_sheet_import(const GTEXT_CSS_Sheet * sheet, size_t index);

/** @brief How many diagnostics were raised, and the `index`th kept one (the first 1,024). */
GTEXT_API size_t gtext_css_sheet_diagnostic_total(const GTEXT_CSS_Sheet * sheet);
GTEXT_API bool gtext_css_sheet_diagnostic(const GTEXT_CSS_Sheet * sheet, size_t index, GTEXT_CSS_Sheet_Diagnostic * out);

/** @brief What a condition node is. */
typedef enum {
  GTEXT_CSS_SHEET_COND_MEDIA = 1,    ///< `@media`.
  GTEXT_CSS_SHEET_COND_SUPPORTS,     ///< `@supports`.
  GTEXT_CSS_SHEET_COND_CONTAINER,    ///< `@container`: element-dependent.
  GTEXT_CSS_SHEET_COND_STARTING_STYLE, ///< `@starting-style`.
  GTEXT_CSS_SHEET_COND_WHEN,         ///< `@when`, `@else` (`else_rule`).
  GTEXT_CSS_SHEET_COND_NEVER,        ///< A rule whose condition is not valid (the diagnostic says so), or `@document`: it never applies.
  GTEXT_CSS_SHEET_COND_IMPORT        ///< The conditions of an `@import`.
} GTEXT_CSS_Sheet_Condition_Kind;

/** @brief A condition node of a sheet. The lists are owned by the sheet and valid for its life. */
typedef struct {
  GTEXT_CSS_Sheet_Condition_Kind kind;
  uint32_t parent;                   ///< The enclosing node (0: none).
  uint32_t previous;                 ///< `@when`/`@else`: the previous rule of the chain (0: it begins one).
  bool else_rule;                    ///< `@else`.
  bool always;                       ///< A bare `@else`: no condition, true unless an earlier rule of the chain applies.
  bool element_dependent;            ///< The node or one it is inside is a container query.
  bool starting_style;               ///< The node or one it is inside is `@starting-style`.
  const GTEXT_CSS_Media_List * list; ///< `@media`, `@when`, `@else`, `@container`: the parsed condition; `@supports`: the condition; an `@import`: its media query list. NULL where there is none.
  const GTEXT_CSS_Supports_Condition * supports; ///< An `@import`: its `supports()` condition (NULL: none).
  GTEXT_CSS_Span span;               ///< The rule.
} GTEXT_CSS_Sheet_Condition;

/** @brief How many condition nodes the sheet has (their ids are 1 to this number). */
GTEXT_API size_t gtext_css_sheet_condition_count(const GTEXT_CSS_Sheet * sheet);

/** @brief The node `id` (from 1); false for an id that is not one. */
GTEXT_API bool gtext_css_sheet_condition(const GTEXT_CSS_Sheet * sheet, uint32_t id, GTEXT_CSS_Sheet_Condition * out);

/** @brief The condition node of import `index` (0: none, the import is unconditional). */
GTEXT_API uint32_t gtext_css_sheet_import_condition(const GTEXT_CSS_Sheet * sheet, size_t index);

/** @brief The sheet in a canonical, versioned text (`sheet 1`, then its layers, namespaces, imports and records), the same on every platform. */
GTEXT_API GTEXT_CSS_Status gtext_css_sheet_dump(const GTEXT_CSS_Sheet * sheet, const GTEXT_Markup_Sink * sink);

/* ---- the index ------------------------------------------------------------------------------------------ */

/** @brief What the index may be told. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;  ///< NULL for the registry's.
  bool html_document;                 ///< The keys of the buckets for type selectors are lower case (an HTML document): as the match context's option.
  bool quirks;                        ///< The keys of the id and class buckets are lower case: as the match context's option.
  size_t max_records;                 ///< Most records in all. Default 16,777,216.
  const GTEXT_CSS_Conditions * conditions; ///< The evaluator the index is built for (retained): required when a sheet holds a condition node, else GTEXT_CSS_E_UNSUPPORTED naming `conditions`.
} GTEXT_CSS_Rule_Index_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Rule_Index_Options gtext_css_rule_index_options_default(void);

/** @brief What a sheet is registered with. */
typedef struct {
  GTEXT_CSS_Origin origin;                 ///< The user-agent, user or author origin (not the animation or transition origins).
  uint32_t context;                        ///< The number of the tree scope the sheet belongs to (see the file's description): the document is 0.
  const void * tree_scope;                 ///< The scope handle (the interface's `tree_scope`) of the tree the sheet is for; NULL: the document.
  bool shadow;                             ///< The scope is a shadow tree.
  const void * parent_scope;               ///< A shadow tree: the scope that holds its host (NULL: the document).
  GTEXT_CSS_Element_Handle implicit_root;  ///< The root of an `@scope` with no prelude: the parent of the `<style>` element's owner. NULL: none.
  GTEXT_CSS_Layer_Registry * layers;       ///< The layer registry of the context, shared by its sheets and not frozen; NULL: the index makes one for the context.
} GTEXT_CSS_Sheet_Registration;

/** @brief What the index calls report. */
typedef struct {
  GTEXT_CSS_Status status;   ///< What the call returned.
  const char * message;      ///< A static description; never NULL after a call.
  char name[96];             ///< What is concerned (`context order`, a callback), cut to fit.
  const char * detail;       ///< A static sentence saying which rule was broken, or NULL.
  size_t work;               ///< Units of work done (rules tested, buckets read).
} GTEXT_CSS_Rule_Index_Result;

/**
 * @brief Make an empty index.
 *
 * @param registry The property registry. Not NULL; it must outlive the index.
 * @param options NULL for the defaults.
 * @param out Receives it. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_rule_index_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Rule_Index_Options * options, GTEXT_CSS_Rule_Index ** out);

/** @brief Free an index; NULL is ignored. The sheets are released (they are retained by the index). */
GTEXT_API void gtext_css_rule_index_free(GTEXT_CSS_Rule_Index * index);

/**
 * @brief Add a sheet (and the sheets it imports, before it, in place).
 *
 * Merges the sheet's layers into the layer registry of its context, in order. The sheets of one scope have one context; a registration that contradicts an earlier one is
 * GTEXT_CSS_E_INVALID.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a frozen index, an origin without layers, a scope with two contexts); the limits of layers; GTEXT_CSS_E_LIMIT_SHEET_SIZE (too many records); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_rule_index_add_sheet(GTEXT_CSS_Rule_Index * index, GTEXT_CSS_Sheet * sheet, const GTEXT_CSS_Sheet_Registration * registration,
    GTEXT_CSS_Rule_Index_Result * result);

/**
 * @brief Freeze the layer registries and build the buckets. After it nothing is added and the index may be read by any number of threads.
 *
 * Checks the registrations: a shadow tree's context is greater than the context of the scope that holds its host, else GTEXT_CSS_E_INVALID with `name` `context order`.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_rule_index_freeze(GTEXT_CSS_Rule_Index * index, GTEXT_CSS_Rule_Index_Result * result);

/** @brief The layer registry of a context (frozen after gtext_css_rule_index_freeze()), or NULL for a context no sheet was added for. */
GTEXT_API const GTEXT_CSS_Layer_Registry * gtext_css_rule_index_layers(const GTEXT_CSS_Rule_Index * index, uint32_t context);

/** @brief How many records the index holds, and how many sheets (imports included). */
GTEXT_API size_t gtext_css_rule_index_record_count(const GTEXT_CSS_Rule_Index * index);
GTEXT_API size_t gtext_css_rule_index_sheet_count(const GTEXT_CSS_Rule_Index * index);

/** @brief What collecting may be told. A zeroed struct is the default. */
typedef struct {
  /** Records under `@starting-style`: false (the default) skips them; true lets them take part in the cascade as ordinary records, with their layer, specificity and order, which is
   *  how Transitions 2 defines the starting style. A consumer computes the style twice, once of each; interpolation is not here. */
  bool starting_style;
  /** The context of the container queries of this call: the memo that makes a walk over a tree linear (css_container.h). NULL: one private to the call. */
  GTEXT_CSS_Condition_Call * call;
} GTEXT_CSS_Rules_Collect_Options;

/**
 * @brief Add to a candidate set one entry for each declaration of each rule that matches the element (or the pseudo-element of it).
 *
 * Every entry has the keys the cascade defines: `origin`, `important` (the declaration's), `layer` (in the registry of the entry's context, which the call gives the set),
 * `context`, `scoped` and `scope_distance`, `specificity` (the highest of the complex selectors of the rule that match), `sheet` and `rule` (the order of appearance), and
 * `declaration`. The result does not depend on the order the sheets were added in beyond the order of appearance they define.
 *
 * @param index A frozen index. Not NULL.
 * @param match The match context (css_match.h) of the element interface and the options (`html_document`, `quirks`, `visited`) the document is read with. Not NULL; the call sets and
 *   restores its `:scope`, `:host` and nesting state.
 * @param element The element. Not NULL.
 * @param pseudo NULL for the element, or the pseudo-element asked for (a list of one complex selector made only of pseudo-elements, as gtext_css_selector_match() takes).
 * @param set The candidate set. Not NULL, not sealed; made on the registry the index has. It is given the layer registry of every context the index has.
 * @param options NULL for the defaults.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (an index that is not frozen, a set that is sealed, a match context whose `html_document` or `quirks` is not the index's); GTEXT_CSS_E_UNSUPPORTED (a callback is absent: `name` says which, `container.size` for a size query and a container interface without `content_box`);
 *   GTEXT_CSS_E_LIMIT_CONTAINER, _LIMIT_WORK, GTEXT_CSS_E_LIMIT_MATCH_WORK, _LIMIT_MATCH_MEMO, _LIMIT_CANDIDATES; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_rules_collect(const GTEXT_CSS_Rule_Index * index, GTEXT_CSS_Selector_Match_Context * match, GTEXT_CSS_Element_Handle element,
    const GTEXT_CSS_Selector_List * pseudo, GTEXT_CSS_Candidates * set, const GTEXT_CSS_Rules_Collect_Options * options, GTEXT_CSS_Rule_Index_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_SHEET_H
