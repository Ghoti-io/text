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
 * @brief `CSSRule` and `CSSRuleList`: the rule object of every kind, and the ordered list that holds a sheet's or a rule's rules (CSS Object Model).
 *
 * ## One rule object, twenty-nine kinds
 *
 * CSSOM has a class for each kind of rule. A C library has one object, @ref GTEXT_CSSOM_Rule, with a **kind** (@ref GTEXT_CSSOM_Rule_Kind: the at-rule kinds of
 * css_atrule.h, `UNKNOWN` included, plus the style rule and the nested-declarations rule) and the attributes of the kinds that have them. `CSSLayerBlockRule` and
 * `CSSLayerStatementRule` are one kind, told apart by gtext_cssom_rule_has_block(). A sheet's parse never makes a rule of the kinds that are not rules (`UNKNOWN`,
 * `CHARSET`, `FEATURE_BLOCK`): they are dropped with a diagnostic (css_cssom_sheet.h).
 *
 * ## Identity, ownership and lifetime
 *
 * **Identity is stable**: asking twice for the same rule (a list's `at(i)`, a parent's child, a declaration's rule) returns the same pointer for as long as the object
 * lives, because a binding must give `sheet.cssRules[0] === sheet.cssRules[0]`. Every call that returns an object **returns it retained**: the caller releases it with
 * gtext_cssom_rule_release(). Nothing a call returns is a borrowed pointer that a later mutation can invalidate. A list (@ref GTEXT_CSSOM_Rule_List) is a **view** of its
 * owner, allocates nothing and is valid for as long as its owner is held.
 *
 * A rule is owned by the list that holds it, which is owned by a sheet or by a rule; a child's pointer to its parent is **weak**, and a held child keeps its parent
 * (and so its sheet) alive through a hold, not an owning edge, so the graph has no strong cycle (css_cssom_core.h, "Lifetime and keep-alive"). `parent_rule` is stored;
 * **`parent_sheet` is derived** by walking the parent chain, so detaching a rule is one pointer and every descendant of a detached rule reads `parent_sheet` NULL at once.
 * A detached rule is still a valid object: readable, its declaration block mutable (with no effect on any sheet), its id unchanged.
 *
 * ## Ids, sources and spans
 *
 * Every rule has an **id** from the document's counter, monotonic and never reused (so a dump is deterministic whatever the addresses are). A parsed rule also has the
 * **source span** of its text (line, column in UTF-16 code units, byte offsets) and the id of the source buffer, and says whether the position can be trusted
 * (@ref GTEXT_CSSOM_Source_Kind): a rule that was parsed from a sheet's text is `PARSED`; one that was inserted or whose text was rewritten says so.
 *
 * ## The declarations of a rule
 *
 * A style rule's `style` holds the declarations before its first nested rule; the declarations after a nested rule are nested-declarations rules with blocks of their
 * own. The block is built the first time it is asked for (a parse that holds a million style rules builds none), and mutating it is a mutation of the rule and of its sheet
 * (the rule's version, the sheet's version, a notification), unless the rule is detached, in which case it changes the rule and nothing else.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CSSOM_RULE_H
#define GHOTI_IO_GTEXT_CSS_CSS_CSSOM_RULE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_atrule.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_cssom_core.h>
#include <ghoti.io/text/css/css_cssom_decl.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A rule (the struct is private). Reference counted with a plain, non-atomic count: the document's writer thread only. */
typedef struct GTEXT_CSSOM_Rule GTEXT_CSSOM_Rule;

/** @brief The ordered list of a sheet's or a rule's rules (`CSSRuleList`; the struct is private). A view: valid while its owner is held. */
typedef struct GTEXT_CSSOM_Rule_List GTEXT_CSSOM_Rule_List;

/** @brief A style sheet (css_cssom_sheet.h). */
typedef struct GTEXT_CSSOM_Sheet GTEXT_CSSOM_Sheet;

/**
 * @brief What a rule is. The first values are the at-rule kinds of css_atrule.h (equal to them), then the two that are not at-rules.
 *
 * | kind | CSSOM interface |
 * |---|---|
 * | STYLE | `CSSStyleRule` |
 * | NESTED_DECLARATIONS | `CSSNestedDeclarations` |
 * | IMPORT, NAMESPACE, MEDIA, SUPPORTS, CONTAINER, STARTING_STYLE, SCOPE | `CSSImportRule` ... `CSSScopeRule` |
 * | LAYER | `CSSLayerBlockRule` (with a block) or `CSSLayerStatementRule` |
 * | PROPERTY, FONT_FACE, FONT_FEATURE_VALUES, FONT_PALETTE_VALUES, COLOR_PROFILE, COUNTER_STYLE, VIEW_TRANSITION, POSITION_TRY | the interface of the same name |
 * | PAGE, PAGE_MARGIN | `CSSPageRule`, `CSSMarginRule` |
 * | KEYFRAMES, KEYFRAME | `CSSKeyframesRule`, `CSSKeyframeRule` |
 * | WHEN, ELSE, CUSTOM_MEDIA, PRESERVED | no browser exposes them; the rule is generic: in `cssRules`, inactive |
 * | UNKNOWN, CHARSET, FEATURE_BLOCK | not rules: a sheet's parse never makes one |
 */
typedef enum {
  GTEXT_CSSOM_RULE_UNKNOWN = GTEXT_CSS_ATRULE_UNKNOWN,
  GTEXT_CSSOM_RULE_CHARSET = GTEXT_CSS_ATRULE_CHARSET,
  GTEXT_CSSOM_RULE_IMPORT = GTEXT_CSS_ATRULE_IMPORT,
  GTEXT_CSSOM_RULE_NAMESPACE = GTEXT_CSS_ATRULE_NAMESPACE,
  GTEXT_CSSOM_RULE_LAYER = GTEXT_CSS_ATRULE_LAYER,
  GTEXT_CSSOM_RULE_SCOPE = GTEXT_CSS_ATRULE_SCOPE,
  GTEXT_CSSOM_RULE_PROPERTY = GTEXT_CSS_ATRULE_PROPERTY,
  GTEXT_CSSOM_RULE_MEDIA = GTEXT_CSS_ATRULE_MEDIA,
  GTEXT_CSSOM_RULE_SUPPORTS = GTEXT_CSS_ATRULE_SUPPORTS,
  GTEXT_CSSOM_RULE_CONTAINER = GTEXT_CSS_ATRULE_CONTAINER,
  GTEXT_CSSOM_RULE_STARTING_STYLE = GTEXT_CSS_ATRULE_STARTING_STYLE,
  GTEXT_CSSOM_RULE_WHEN = GTEXT_CSS_ATRULE_WHEN,
  GTEXT_CSSOM_RULE_ELSE = GTEXT_CSS_ATRULE_ELSE,
  GTEXT_CSSOM_RULE_CUSTOM_MEDIA = GTEXT_CSS_ATRULE_CUSTOM_MEDIA,
  GTEXT_CSSOM_RULE_FONT_FACE = GTEXT_CSS_ATRULE_FONT_FACE,
  GTEXT_CSSOM_RULE_FONT_FEATURE_VALUES = GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES,
  GTEXT_CSSOM_RULE_FEATURE_BLOCK = GTEXT_CSS_ATRULE_FEATURE_BLOCK,
  GTEXT_CSSOM_RULE_FONT_PALETTE_VALUES = GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES,
  GTEXT_CSSOM_RULE_COLOR_PROFILE = GTEXT_CSS_ATRULE_COLOR_PROFILE,
  GTEXT_CSSOM_RULE_COUNTER_STYLE = GTEXT_CSS_ATRULE_COUNTER_STYLE,
  GTEXT_CSSOM_RULE_PAGE = GTEXT_CSS_ATRULE_PAGE,
  GTEXT_CSSOM_RULE_PAGE_MARGIN = GTEXT_CSS_ATRULE_PAGE_MARGIN,
  GTEXT_CSSOM_RULE_KEYFRAMES = GTEXT_CSS_ATRULE_KEYFRAMES,
  GTEXT_CSSOM_RULE_KEYFRAME = GTEXT_CSS_ATRULE_KEYFRAME,
  GTEXT_CSSOM_RULE_VIEW_TRANSITION = GTEXT_CSS_ATRULE_VIEW_TRANSITION,
  GTEXT_CSSOM_RULE_POSITION_TRY = GTEXT_CSS_ATRULE_POSITION_TRY,
  GTEXT_CSSOM_RULE_PRESERVED = GTEXT_CSS_ATRULE_PRESERVED,
  GTEXT_CSSOM_RULE_STYLE = GTEXT_CSS_ATRULE_KIND_COUNT,   ///< A style rule: a selector list, declarations, and rules nested in it.
  GTEXT_CSSOM_RULE_NESTED_DECLARATIONS,                   ///< The declarations that follow a nested rule (CSS Nesting).
  GTEXT_CSSOM_RULE_KIND_COUNT                             ///< The number of kinds; not a kind.
} GTEXT_CSSOM_Rule_Kind;

/** @brief The name of a kind: the at-rule kinds' names of css_atrule.h (`font-face`, `page-margin`), `style`, `nested-declarations`; "unknown" for anything else. Static. */
GTEXT_API const char * gtext_cssom_rule_kind_name(GTEXT_CSSOM_Rule_Kind kind);

/** @brief Where the text of a rule comes from, and so whether its span is a position an editor can trust. */
typedef enum {
  GTEXT_CSSOM_SOURCE_PARSED = 0,   ///< Read from the text of a sheet: the span is a position in that text.
  GTEXT_CSSOM_SOURCE_INSERTED,     ///< Inserted by `insertRule`: the span is a position in the text of that call.
  GTEXT_CSSOM_SOURCE_MODIFIED      ///< Rewritten (a setter, a compaction): the span is where the rule was, and no longer says where it is.
} GTEXT_CSSOM_Source_Kind;

/** @brief The source of a rule. */
typedef struct {
  GTEXT_CSSOM_Source_Kind kind;    ///< Whether the span can be trusted.
  uint64_t source_id;              ///< The id of the source buffer, from the document's counter; rules parsed together share it.
  GTEXT_CSS_Span span;             ///< The rule's text in that buffer: byte offsets, and the line and the column (in UTF-16 code units) of the first.
} GTEXT_CSSOM_Source_Info;

/** @brief Take another external reference. Writer thread only. @return The rule. */
GTEXT_API GTEXT_CSSOM_Rule * gtext_cssom_rule_retain(GTEXT_CSSOM_Rule * rule);

/** @brief Give one back. A held rule keeps its parents alive; the object is freed when its last reference, internal or external, goes. NULL is ignored. */
GTEXT_API void gtext_cssom_rule_release(GTEXT_CSSOM_Rule * rule);

/** @brief The kind of the rule. GTEXT_CSSOM_RULE_UNKNOWN for NULL. */
GTEXT_API GTEXT_CSSOM_Rule_Kind gtext_cssom_rule_kind(const GTEXT_CSSOM_Rule * rule);

/**
 * @brief The legacy `type` attribute: the constant CSSOM lists for the rules it lists (`STYLE_RULE` 1, `IMPORT_RULE` 3, `MEDIA_RULE` 4, `FONT_FACE_RULE` 5, `PAGE_RULE` 6,
 *   `KEYFRAMES_RULE` 7, `KEYFRAME_RULE` 8, `MARGIN_RULE` 9, `NAMESPACE_RULE` 10, `COUNTER_STYLE_RULE` 11, `SUPPORTS_RULE` 12, `FONT_FEATURE_VALUES_RULE` 14), 0 for every newer one.
 */
GTEXT_API unsigned gtext_cssom_rule_type(const GTEXT_CSSOM_Rule * rule);

/** @brief The CSSOM interface name of the rule (`CSSStyleRule`, `CSSLayerBlockRule`, ...); "CSSRule" for a kind no browser exposes. Static. */
GTEXT_API const char * gtext_cssom_rule_interface_name(const GTEXT_CSSOM_Rule * rule);

/** @brief The rule's id: from the document's counter, monotonic, never reused and never an address. 0 for NULL. */
GTEXT_API uint64_t gtext_cssom_rule_id(const GTEXT_CSSOM_Rule * rule);

/** @brief The rule's version: it increases by one with each mutation of the rule (its own declarations included). 0 for NULL. */
GTEXT_API uint64_t gtext_cssom_rule_version(const GTEXT_CSSOM_Rule * rule);

/** @brief For `@layer`: whether the rule has a block (`CSSLayerBlockRule`) or is a statement (`CSSLayerStatementRule`). False for every other kind. */
GTEXT_API bool gtext_cssom_rule_has_block(const GTEXT_CSSOM_Rule * rule);

/** @brief `parentRule`: the rule whose list holds this one, or NULL (a top-level rule, a detached one). Returned retained. */
GTEXT_API GTEXT_CSSOM_Rule * gtext_cssom_rule_parent_rule(const GTEXT_CSSOM_Rule * rule);

/** @brief `parentStyleSheet`: the sheet at the top of the chain of parents, or NULL when the chain does not end in one (the rule or an ancestor was removed). Returned retained. */
GTEXT_API GTEXT_CSSOM_Sheet * gtext_cssom_rule_parent_sheet(const GTEXT_CSSOM_Rule * rule);

/** @brief Whether the chain of parents no longer ends in a sheet. */
GTEXT_API bool gtext_cssom_rule_detached(const GTEXT_CSSOM_Rule * rule);

/**
 * @brief `cssRules`: the list of the rules inside this one.
 *
 * @return The list, valid while the rule is held; NULL for a kind that has none (it is not a grouping rule, a style rule, `@keyframes` or `@page`), and for a `@layer`
 *   statement.
 */
GTEXT_API const GTEXT_CSSOM_Rule_List * gtext_cssom_rule_css_rules(const GTEXT_CSSOM_Rule * rule);

/**
 * @brief `style`: the rule's declaration block (CSSStyleDeclaration), built the first time it is asked for. The same object every time, returned retained.
 *
 * Available for a style rule and for a nested-declarations rule. The other kinds that have a declaration block hold typed descriptors, which are not exposed as a
 * CSSStyleDeclaration by this call.
 *
 * @param rule The rule. Not NULL.
 * @param out Receives the declaration with an external reference of the caller's; written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument); GTEXT_CSS_E_UNSUPPORTED (a kind with no declaration block here); GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_CSSOM;
 *   the limits of a block (GTEXT_CSS_E_LIMIT_DECLARATIONS, GTEXT_CSS_E_LIMIT_WORK).
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_rule_style(GTEXT_CSSOM_Rule * rule, GTEXT_CSSOM_Decl ** out);

/** @brief Where the rule's text is. @return GTEXT_CSS_OK or GTEXT_CSS_E_INVALID. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_rule_source(const GTEXT_CSSOM_Rule * rule, GTEXT_CSSOM_Source_Info * out);

/**
 * @brief The text the span of a parsed rule covers in the source it was read from, in a new buffer (release it with gtext_cssom_buffer_free()).
 *
 * @param rule The rule. Not NULL.
 * @param out Receives the bytes, NUL-terminated; written only on success. Not NULL.
 * @param length Receives their number (NULL if not wanted).
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument); GTEXT_CSS_E_INVALID_STATE (the rule's source text is not kept: it was inserted, rewritten or compacted);
 *   GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_rule_source_text(const GTEXT_CSSOM_Rule * rule, char ** out, size_t * length);

/** @brief `length`: how many rules the list holds. O(1). 0 for NULL. */
GTEXT_API size_t gtext_cssom_rule_list_length(const GTEXT_CSSOM_Rule_List * list);

/** @brief `item(index)` and `list[index]`: the rule at the index, or NULL past the end. O(log n). Returned retained: the same object every time. */
GTEXT_API GTEXT_CSSOM_Rule * gtext_cssom_rule_list_at(const GTEXT_CSSOM_Rule_List * list, size_t index);

/** @brief The first rule, or NULL for an empty list. Returned retained. */
GTEXT_API GTEXT_CSSOM_Rule * gtext_cssom_rule_list_first(const GTEXT_CSSOM_Rule_List * list);

/**
 * @brief The rule after `rule` in the list, or NULL for the last (or for a rule that is not in this list). O(1) amortized over a walk. Returned retained.
 */
GTEXT_API GTEXT_CSSOM_Rule * gtext_cssom_rule_list_next(const GTEXT_CSSOM_Rule_List * list, const GTEXT_CSSOM_Rule * rule);

/** @brief The index of `rule` in the list. O(log n). @return false when the rule is not in this list. */
GTEXT_API bool gtext_cssom_rule_list_index_of(const GTEXT_CSSOM_Rule_List * list, const GTEXT_CSSOM_Rule * rule, size_t * index);

/**
 * @brief The canonical dump of a rule and everything under it (`cssom-rule 1`): ids, kinds, versions, sources and the shape of the tree, one line each; identical on every host.
 *
 * It reads and builds nothing: a declaration block that has not been built yet is written as `lazy`.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_rule_dump(const GTEXT_CSSOM_Rule * rule, const GTEXT_Markup_Sink * sink);

/** @brief The SHA-256 of the text gtext_cssom_rule_dump() writes. @param hash Receives GTEXT_CSSOM_HASH_LENGTH bytes. Not NULL. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_rule_hash(const GTEXT_CSSOM_Rule * rule, unsigned char hash[GTEXT_CSSOM_HASH_LENGTH]);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CSSOM_RULE_H
