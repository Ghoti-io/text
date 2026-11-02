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
 * @brief `CSSStyleSheet`: a style sheet as a live, mutable, reference counted object, and the parse that makes one (CSS Object Model, CSS Syntax 3).
 *
 * ## What a sheet is
 *
 * A @ref GTEXT_CSSOM_Sheet holds the list of its rules (`cssRules`), the attributes CSSOM gives a sheet (`type`, `href`, `title`, `disabled`, `ownerNode`, the sheet's
 * base URL as an opaque string the library never parses) and the facts a host needs about it (`origin_clean`, `alternate`). It is an **object**: created by a document,
 * reference counted, valid for as long as anyone holds it, **including after the document handle is released**. It is a different thing from the immutable
 * @ref GTEXT_CSS_Sheet of css_sheet.h, which is the compiled form the cascade reads; the live sheet is for editing and for binding.
 *
 * ## Parsing a sheet
 *
 * gtext_cssom_sheet_new_parsed() is CSS Syntax's "parse a stylesheet" (css_parse.h) followed by CSSOM's "create a CSS style sheet": every rule that is valid where it is
 * becomes a rule object (css_cssom_rule.h), top-down, with the lazy materialization that keeps parse-and-hold linear in the source: a style rule keeps its place in the
 * syntax tree and builds its declarations the first time they are asked for.
 *
 * What is not a rule: `@charset`; an at-rule the library does not know; a rule that is invalid where it is (an `@import` or `@namespace` after the rules they must
 * precede, a style rule whose selector is not valid, a keyframe whose selector is not, an at-rule its grammar refuses). Each is dropped with a diagnostic, and with it
 * everything inside it, as CSS Syntax says an invalid rule is.
 *
 * ## One syntax tree per parse, shared by its rules
 *
 * Each parse is one reference counted syntax tree (css_core.h) and one copy of the source text with an id, which the tree's spans index; every rule keeps its tree alive.
 * **The tree lives while any rule of it is attached or held**, so deleting most of a very large sheet frees nothing until the last rule of that parse goes: the tree's bytes
 * count against the document's live-bytes limit, and gtext_cssom_sheet_compact() copies what is left into one new tree and releases the old ones.
 *
 * ## Snapshots: what the cascade reads
 *
 * A live sheet is for editing; the cascade reads a @ref GTEXT_CSS_Sheet (css_sheet.h), which is immutable, atomically counted and shareable between threads. The bridge is
 * gtext_cssom_sheet_snapshot(): it returns the compiled form of the sheet's **current** state, and the same pointer for as long as nothing in the sheet or in the sheets it
 * imports changes, so any number of readers cost one compile. A snapshot, once handed out, is never altered by a later mutation (a test holds one across 10^5 of them). A rule that
 * has not been changed since it was read, and whose syntax is what the text the object model writes for it reads back as, compiles from its own syntax tree, so its spans stay (a
 * record says which source they index); any other rule (changed, with a changed rule inside it, or one whose declarations the object model writes differently from the text, such as a
 * shorthand and its longhand, or a property written twice) compiles from the parse of its own `cssText`, which is a fixed point. Either way the dump of a snapshot equals the dump of the
 * compile of the sheet's serialization.
 *
 * ## Errors, limits and the strong guarantee
 *
 * Every limit is a status carrying what exceeded it, and the call fails whole: GTEXT_CSS_E_LIMIT_INPUT (the text), GTEXT_CSS_E_LIMIT_RULES (rules in the sheet),
 * GTEXT_CSS_E_LIMIT_RULE_NESTING (rules nested deeper than the document's `max_rule_nesting`), GTEXT_CSS_E_LIMIT_WORK, GTEXT_CSS_E_LIMIT_CSSOM (objects or bytes of the
 * document) and the parse's own limits. Nothing recurses on the C stack: a nest a hundred thousand deep is a status, not a crash. A failed call leaves nothing behind.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CSSOM_SHEET_H
#define GHOTI_IO_GTEXT_CSS_CSS_CSSOM_SHEET_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_cssom_core.h>
#include <ghoti.io/text/css/css_cssom_rule.h>
#include <ghoti.io/text/css/css_sheet.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct GTEXT_CSSOM_Sheet_Options
 * @brief What a sheet is made with. Start from gtext_cssom_sheet_options_default().
 */
typedef struct {
  size_t size;                  ///< `sizeof` this struct as the caller's header has it. Required, and first so that the struct can grow.
  const char * href;            ///< `href`: NUL-terminated, copied; NULL for none.
  const char * title;           ///< `title`: NUL-terminated, copied; NULL for none.
  const char * base_url;        ///< The sheet's base URL: opaque, copied, never parsed or joined by the library, handed to the caller's callbacks. NULL for none.
  const void * owner_node;      ///< `ownerNode`: the caller's opaque handle of the `<style>` or `<link>` element, or NULL. Never dereferenced.
  bool disabled;                ///< The `disabled` flag.
  bool alternate;               ///< The sheet is an alternate style sheet (HTML's `alternate`).
  bool origin_clean;            ///< The origin-clean flag. False for a sheet from another origin: reading `cssRules` and every mutation are then GTEXT_CSS_E_SECURITY.
  const char * media;           ///< `media`: the sheet's media query list as text (copied, kept in its canonical form); NULL for none, which matches everything.
  size_t max_import_depth;      ///< Deepest chain of imports a snapshot of this sheet follows (the sheets its `@import` rules loaded, and theirs). 0 is the default, 16; at most 64. One more is GTEXT_CSS_E_LIMIT_RULE_NESTING.
  const void * implicit_root;   ///< The root of an `@scope` rule with no prelude in this sheet: the parent of its owner node, as the interface's element handle (opaque, never dereferenced). NULL for none. Fixed for the life of the sheet: moving the element makes a new sheet.
} GTEXT_CSSOM_Sheet_Options;

/** @brief The defaults, spelled out: `size` set, no href, title, base URL or owner, not disabled, not alternate, origin-clean. */
GTEXT_API GTEXT_CSSOM_Sheet_Options gtext_cssom_sheet_options_default(void);

/** @brief Why a rule of a sheet's text is not in the sheet. */
typedef enum {
  GTEXT_CSSOM_PARSE_CHARSET = 0,          ///< `@charset`: the decoder has read the bytes by it; it is never a rule.
  GTEXT_CSSOM_PARSE_UNKNOWN_AT_RULE,      ///< An at-rule the library does not know (CSS Syntax: an unknown at-rule is invalid).
  GTEXT_CSSOM_PARSE_INVALID_AT_RULE,      ///< An at-rule whose prelude, block or place its grammar refuses.
  GTEXT_CSSOM_PARSE_INVALID_SELECTOR,     ///< A style rule whose selector list is not valid (Selectors 4: an invalid selector makes its rule invalid).
  GTEXT_CSSOM_PARSE_INVALID_KEYFRAME,     ///< A keyframe whose selector is not `from`, `to` or a percentage.
  GTEXT_CSSOM_PARSE_MISPLACED_IMPORT,     ///< An `@import` after a rule it must precede (CSS Cascade 4, "@import").
  GTEXT_CSSOM_PARSE_MISPLACED_NAMESPACE,  ///< An `@namespace` after a rule it must precede (CSS Namespaces 3, "@namespace").
  GTEXT_CSSOM_PARSE_IMPORT_REFUSED        ///< The sheet the import callback gave for an `@import` was refused: it has an owner, or it would make a cycle (the import contributes nothing); or the callbacks were nested too deep.
} GTEXT_CSSOM_Parse_Diag_Kind;

/** @brief One rule of the text that is not in the sheet. */
typedef struct {
  GTEXT_CSSOM_Parse_Diag_Kind kind;  ///< Why.
  GTEXT_CSS_Span span;               ///< Where the rule was.
  const char * reference;            ///< The sentence of the standard that decides it (static, never NULL).
} GTEXT_CSSOM_Parse_Diagnostic;

/**
 * @brief Parse the text of a style sheet into a live sheet (CSS Syntax "parse a stylesheet", CSSOM "create a CSS style sheet").
 *
 * @param document The document. Not NULL.
 * @param text The sheet's text, UTF-8 (copied); NULL with a length of 0 for an empty sheet.
 * @param length Its length in bytes. At most the document's `max_input_bytes`.
 * @param options NULL for the defaults.
 * @param out Receives the sheet with one external reference (the caller's); written only on success. Not NULL.
 * @return GTEXT_CSS_OK (also for text with rules that are dropped: see the diagnostics); GTEXT_CSS_E_INVALID (a NULL argument, or a document that was released);
 *   GTEXT_CSS_E_LIMIT_INPUT, _LIMIT_RULES, _LIMIT_RULE_NESTING, _LIMIT_WORK, _LIMIT_CSSOM and the parse's limits (css_core.h); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_new_parsed(GTEXT_CSSOM_Document * document, const char * text, size_t length, const GTEXT_CSSOM_Sheet_Options * options,
    GTEXT_CSSOM_Sheet ** out);

/** @brief Take another external reference. Writer thread only. @return The sheet. */
GTEXT_API GTEXT_CSSOM_Sheet * gtext_cssom_sheet_retain(GTEXT_CSSOM_Sheet * sheet);

/** @brief Give one back. The sheet is freed, with its rules, when its last reference, internal or external, goes; a held rule keeps its sheet. NULL is ignored. */
GTEXT_API void gtext_cssom_sheet_release(GTEXT_CSSOM_Sheet * sheet);

/** @brief The sheet's id, from the document's counter: monotonic, never reused. 0 for NULL. */
GTEXT_API uint64_t gtext_cssom_sheet_id(const GTEXT_CSSOM_Sheet * sheet);

/** @brief The sheet's version: it increases by one with each mutation of the sheet or of anything in it. 0 for NULL. */
GTEXT_API uint64_t gtext_cssom_sheet_version(const GTEXT_CSSOM_Sheet * sheet);

/** @brief The document the sheet belongs to, or NULL when its handle was released. */
GTEXT_API GTEXT_CSSOM_Document * gtext_cssom_sheet_document(const GTEXT_CSSOM_Sheet * sheet);

/**
 * @brief `cssRules`: the list of the sheet's rules.
 *
 * @param sheet The sheet. Not NULL.
 * @param out Receives the list, valid while the sheet is held; written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument); GTEXT_CSS_E_SECURITY (the sheet is not origin-clean: CSSOM's SecurityError).
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_rules(const GTEXT_CSSOM_Sheet * sheet, const GTEXT_CSSOM_Rule_List ** out);

/** @brief `type`: always `text/css`. Static. */
GTEXT_API const char * gtext_cssom_sheet_type(const GTEXT_CSSOM_Sheet * sheet);

/**
 * @brief `href`, `title` and the base URL, each in a new buffer (release it with gtext_cssom_buffer_free()); `*out` is NULL and `*length` 0 for a sheet that has none.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_href(const GTEXT_CSSOM_Sheet * sheet, char ** out, size_t * length);
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_title(const GTEXT_CSSOM_Sheet * sheet, char ** out, size_t * length);
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_base_url(const GTEXT_CSSOM_Sheet * sheet, char ** out, size_t * length);

/** @brief `ownerNode`: the caller's handle the sheet was made with, or NULL. */
GTEXT_API const void * gtext_cssom_sheet_owner_node(const GTEXT_CSSOM_Sheet * sheet);

/** @brief `disabled`. */
GTEXT_API bool gtext_cssom_sheet_disabled(const GTEXT_CSSOM_Sheet * sheet);

/**
 * @brief The `disabled` setter: a change bumps the sheet's version and sends one notification (GTEXT_CSSOM_EVENT_SHEET_DISABLED_CHANGED); setting the value the sheet
 *   has is `GTEXT_CSSOM_UNCHANGED` and changes nothing.
 *
 * @param sheet The sheet. Not NULL.
 * @param disabled The new value.
 * @param outcome NULL, or receives what the call did.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_REENTRANT (from inside a notification or a finalizer); GTEXT_CSS_E_LIMIT_CSSOM (the generation counter is spent).
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_set_disabled(GTEXT_CSSOM_Sheet * sheet, bool disabled, GTEXT_CSSOM_Outcome * outcome);

/** @brief Whether the sheet is an alternate style sheet. */
GTEXT_API bool gtext_cssom_sheet_alternate(const GTEXT_CSSOM_Sheet * sheet);

/** @brief The origin-clean flag. */
GTEXT_API bool gtext_cssom_sheet_origin_clean(const GTEXT_CSSOM_Sheet * sheet);

/** @brief Set the origin-clean flag: the caller's fact about where the text came from, not a mutation of the model (no version, no notification). */
GTEXT_API void gtext_cssom_sheet_set_origin_clean(GTEXT_CSSOM_Sheet * sheet, bool origin_clean);

/** @brief Whether the sheet was made by `new CSSStyleSheet()` (always false for a parsed sheet). */
GTEXT_API bool gtext_cssom_sheet_constructed(const GTEXT_CSSOM_Sheet * sheet);

/** @brief How many rules of the text were dropped, whatever the number kept. */
GTEXT_API size_t gtext_cssom_sheet_diagnostic_total(const GTEXT_CSSOM_Sheet * sheet);

/** @brief The `index`th diagnostic kept (at most the document's `max_diagnostics`, the earliest in the text); false past the end. */
GTEXT_API bool gtext_cssom_sheet_diagnostic(const GTEXT_CSSOM_Sheet * sheet, size_t index, GTEXT_CSSOM_Parse_Diagnostic * out);

/** @brief How many rules the sheet holds, at every depth. O(1). */
GTEXT_API size_t gtext_cssom_sheet_rule_total(const GTEXT_CSSOM_Sheet * sheet);

/**
 * @brief `ownerRule`: the `@import` rule that holds this sheet (gtext_cssom_import_rule_complete()), with an external reference of the caller's, or NULL in `*out` for a sheet no
 *   import holds.
 * @return GTEXT_CSS_OK or GTEXT_CSS_E_INVALID.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_owner_rule(const GTEXT_CSSOM_Sheet * sheet, GTEXT_CSSOM_Rule ** out);

/**
 * @brief The sheet's compiled form: an immutable, atomically counted @ref GTEXT_CSS_Sheet of its current state.
 *
 * The result is compiled by `gtext_css_sheet_compile_rules()` over the sheet's top-level rules (see the file's description) with the document's registry and allocator; the
 * limits are the document's (`max_rule_nesting` is the rule depth, `max_diagnostics` the diagnostics kept) and the sheet's `max_import_depth`. The sheets that the sheet's `@import`
 * rules hold (gtext_cssom_import_rule_complete()) are the snapshots of those sheets, one level down, and an import with no sheet contributes nothing (a diagnostic of the compile).
 *
 * **It is cached** on the sheet's composite version, the greatest change stamp among the sheet and the sheets it imports: while nothing in them changes the same pointer is returned,
 * with a new reference each time, and a change makes the next call compile once however many changes came before. A snapshot handed out earlier is not altered and stays valid for as
 * long as its holder keeps it, on any thread, including after the sheet, its document and the library's other objects are gone. Reading the cache or the sheet does not change the
 * sheet: no version, no generation, no notification. A sheet that is not origin-clean is snapshotted all the same (the flag guards the script's access to the rules, not the
 * cascade's), and so is a disabled one (whether it takes part is the document's decision).
 *
 * @param sheet The sheet. Not NULL.
 * @param out Receives the snapshot with one reference of the caller's, released with gtext_css_sheet_free(); written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument); GTEXT_CSS_E_LIMIT_RULE_NESTING (imports nested deeper than `max_import_depth`, or rules deeper than the document's
 *   `max_rule_nesting`); GTEXT_CSS_E_LIMIT_SHEET_SIZE and the limits of the compile; GTEXT_CSS_E_INTERNAL (the text of a rule does not read back as that rule: a defect, never a content
 *   error); GTEXT_CSS_E_OOM. On any failure the sheet, its cache and the document are as they were.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_snapshot(GTEXT_CSSOM_Sheet * sheet, GTEXT_CSS_Sheet ** out);

/**
 * @brief The sheet's composite version: the greatest change stamp among the sheet and the sheets it imports, through every level.
 *
 * Every change of a sheet or of anything in it (a rule inserted, deleted, replaced or edited, `disabled`, `media`, an import completed) gives the sheet a new stamp, greater than every
 * stamp given before it in the document, so the composite version of a sheet **changes (and only increases) exactly when something in it or in what it imports changed**; a sheet
 * the import holds being replaced by one that was last changed long ago is a change of the importing sheet. Equal values mean an unchanged sheet; the value is otherwise opaque.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_LIMIT_RULE_NESTING (imports nested deeper than the sheet's `max_import_depth`).
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_composite_version(const GTEXT_CSSOM_Sheet * sheet, uint64_t * out);

/**
 * @brief The text the sheet was parsed from, in a new buffer (release it with gtext_cssom_buffer_free()).
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument); GTEXT_CSS_E_INVALID_STATE (the text is no longer kept: the sheet was compacted); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_source_text(const GTEXT_CSSOM_Sheet * sheet, char ** out, size_t * length);

/**
 * @brief Copy what is left of the sheet into one new syntax tree and release the old ones, so that a sheet from which most rules were deleted gives its memory back.
 *
 * O(the size of the rules that remain). No id, identity or version changes, and nothing a caller holds is invalidated; the spans of the compacted rules become
 * `GTEXT_CSSOM_SOURCE_MODIFIED` (the text they pointed into is no longer kept). All or nothing.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_LIMIT_CSSOM; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_compact(GTEXT_CSSOM_Sheet * sheet);

/* ---- insertRule, deleteRule and the sheet's text ---------------------------------------------------------------------------------------- */

/**
 * @brief `CSSStyleSheet.insertRule(rule, index)`: parse one rule from the text and insert it at the index.
 *
 * **The order of checks** (the first error wins; the Editors' Draft of CSSOM as read, and the answers of Chromium 156 and Firefox 158 where it is silent):
 * 1. GTEXT_CSS_E_REENTRANT (called from an observer or a finalizer), GTEXT_CSS_E_LIMIT_CSSOM (the generation counter is spent);
 * 2. GTEXT_CSS_E_SECURITY (the sheet is not origin-clean);
 * 3. GTEXT_CSS_E_NOT_ALLOWED (the disallow-modification flag is set);
 * 4. GTEXT_CSS_E_SYNTAX: "parse a rule" over the whole text finds one rule that is valid, or it does not (nothing, two rules, trailing text, an unknown at-rule, a style rule whose
 *    selector is not valid, a rule that is invalid for its own grammar); so a text that is not a rule is a SyntaxError **whatever the index is**;
 * 5. GTEXT_CSS_E_SYNTAX for an `@import` into a constructed sheet;
 * 6. GTEXT_CSS_E_INDEX_SIZE (the index is greater than the number of rules);
 * 7. GTEXT_CSS_E_HIERARCHY: the rule cannot be at that position by CSS Cascade 5 and CSS Namespaces 3 (an `@import` after a style rule, a style rule before an `@import` or
 *    `@namespace`, an `@namespace` after a style rule or before an `@import`);
 * 8. GTEXT_CSS_E_INVALID_STATE: an `@namespace` into a list that holds anything but `@import` and `@namespace` rules;
 * 9. the rule is inserted.
 *
 * Both browsers answer the index check (6) before the text is looked at (4); the served text puts the parse first, and this library follows the text.
 *
 * Atomic: on any failure (an allocation or a limit included) the sheet, its versions, its ids and its observers' view are as they were. One GTEXT_CSSOM_EVENT_RULE_INSERTED is sent.
 *
 * @param sheet The sheet. Not NULL.
 * @param text The rule, UTF-8. NULL with a length of 0 is the empty text (a SyntaxError).
 * @param length Its length in bytes.
 * @param index Where to insert it: 0 to the number of rules. (The binding's default is 0.)
 * @param rule_out NULL, or receives the new rule with an external reference of the caller's; written only on success.
 * @param outcome NULL, or receives what the call did.
 * @return GTEXT_CSS_OK, or one of the statuses above; GTEXT_CSS_E_INVALID (a NULL argument); the limits (GTEXT_CSS_E_LIMIT_INPUT, _LIMIT_RULES, _LIMIT_RULE_NESTING, _LIMIT_WORK, _LIMIT_CSSOM,
 *   and the parse's); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_insert_rule(GTEXT_CSSOM_Sheet * sheet, const char * text, size_t length, size_t index, GTEXT_CSSOM_Rule ** rule_out,
    GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief `CSSStyleSheet.deleteRule(index)`: remove the rule, with the rules inside it, which stay valid objects that no sheet holds (`parentStyleSheet` and `parentRule` are NULL).
 *
 * The order: GTEXT_CSS_E_REENTRANT/_LIMIT_CSSOM, GTEXT_CSS_E_SECURITY, GTEXT_CSS_E_NOT_ALLOWED, GTEXT_CSS_E_INDEX_SIZE (the index is not less than the number of rules),
 * GTEXT_CSS_E_INVALID_STATE (an `@namespace` rule, when the list holds anything but `@import` and `@namespace` rules), then the rule is removed. One GTEXT_CSSOM_EVENT_RULE_REMOVED
 * is sent. Atomic. O(log n) in the rules of the sheet.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_delete_rule(GTEXT_CSSOM_Sheet * sheet, size_t index, GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief The legacy `addRule(selector, style, index)`: `insertRule(selector + " { " + style + " }", index)`, at the end when no index is given. The binding returns -1.
 * @param has_index Whether `index` is given.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_add_rule(GTEXT_CSSOM_Sheet * sheet, const char * selector, size_t selector_length, const char * style, size_t style_length,
    bool has_index, size_t index, GTEXT_CSSOM_Outcome * outcome);

/** @brief The legacy `removeRule(index)`: the steps of gtext_cssom_sheet_delete_rule(). (`rules` is gtext_cssom_sheet_rules().) */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_remove_rule(GTEXT_CSSOM_Sheet * sheet, size_t index, GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief Write the sheet as text: the `cssText` of each rule, each followed by a line feed. Reads the live state, fills no cache, and the sink's output is bounded by the document's
 *   `max_output_bytes` (GTEXT_CSS_E_LIMIT_OUTPUT).
 *
 * Parsing the text again gives a sheet that serializes to the same bytes (for every sheet this library's parse can make, but a rule closed by the end of input and a custom property
 * value holding `{` or `(`, which the syntax layer owns).
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_SECURITY (not origin-clean); GTEXT_CSS_E_LIMIT_OUTPUT; GTEXT_CSS_E_WRITE (the sink refused); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_serialize(const GTEXT_CSSOM_Sheet * sheet, const GTEXT_Markup_Sink * sink);

/**
 * @brief The canonical dump of a sheet and every rule in it (`cssom-sheet 1`): the sheet's attributes, then the rules in document order, one line each; identical on every
 *   host. It reads and builds nothing.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_dump(const GTEXT_CSSOM_Sheet * sheet, const GTEXT_Markup_Sink * sink);

/** @brief The SHA-256 of the text gtext_cssom_sheet_dump() writes. @param hash Receives GTEXT_CSSOM_HASH_LENGTH bytes. Not NULL. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_hash(const GTEXT_CSSOM_Sheet * sheet, unsigned char hash[GTEXT_CSSOM_HASH_LENGTH]);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CSSOM_SHEET_H
