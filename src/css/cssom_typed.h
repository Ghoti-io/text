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
 * What the setters of a rule and the declaration block of a typed rule share (cssom_setters.c, cssom_typed.c). Private.
 *
 * A rule whose syntax is immutable (a selector list, a typed at-rule) is changed **as a whole**: the setter writes the candidate text of the rule's own part (its prelude and
 * its own declarations, never the rules inside it), reads it again in the place the rule stands in, and swaps the rule's syntax for what that gives if, and only if, it is a rule
 * of the same kind that holds what was set. The rule's identity, its place, the rules inside it and its object ids do not change.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSSOM_TYPED_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSSOM_TYPED_H

#include <ghoti.io/text/macros.h>

#include "css_atrule_internal.h"
#include "css_shorthand_internal.h"
#include "cssom_internal.h"
#include "cssom_rule_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// The typed model of a rule (cssom_rule_serialize.c). `shallow`: without what is inside its block.
GTEXT_CSS_Status css_rule_typed(css_core * core, const css_rule * rule, bool shallow, GTEXT_CSS_Atrule ** out);
/// A style rule's selector list as `selectorText` writes it, appended to `out`.
GTEXT_CSS_Status css_rule_selector_text(css_rule * rule, tbuf * out);
/// Where the rule stands, for reading a rule of its kind again (cssom_insert.c). `synthetic` backs `ctx->parent_node` for a rule that is in no list.
void css_rule_own_ctx(const css_rule * rule, css_insert_ctx * ctx, GTEXT_CSSOM_Rule_Kind * parent_kind, GTEXT_CSS_Rule * synthetic);

/// A candidate reading of a rule: the syntax tree of the text, its source, and its rule node. Nothing is changed until css_own_commit().
typedef struct {
  GTEXT_CSS_Tree * tree;
  css_source * source;
  const GTEXT_CSS_Rule * node;
  uint64_t saved_id;   ///< The document's id counter before the source took one: a candidate that is not kept gives it back.
} css_own;

/// Read `text` as one rule of the kind and in the place of `rule`. `*invalid` is true (and `own` empty) when it is not, or is a rule of another kind. Resource failures are the status.
GTEXT_CSS_Status css_own_prepare(css_rule * rule, const char * text, size_t length, css_own * own, bool * invalid);
void css_own_discard(css_own * own);
/// Swap the rule's syntax for the candidate's: infallible; the rule becomes `MODIFIED` and its version moves. The sheet's version and the event are the caller's.
void css_own_commit(css_rule * rule, css_own * own);
/// The same without moving the rule's version (the declaration block's mutation moves it once, through the owner).
void css_own_swap(css_rule * rule, css_own * own);
/// The typed model of the candidate (as css_rule_typed() gives it for the rule), for reading what the text came to.
GTEXT_CSS_Status css_own_typed(css_rule * rule, const css_own * own, bool shallow, GTEXT_CSS_Atrule ** out);

/// The body of an `@font-feature-values` rule as text: its descriptors and one block per feature type (cssom_feature_map.c).
GTEXT_CSS_Status css_feature_values_body(css_rule * rule, tbuf * out);

/// The header of the rule as text, up to and not including its block: `@page :first`, `0%, 100%`, a style rule's selectors, `@counter-style foo`.
GTEXT_CSS_Status css_rule_header_text(css_rule * rule, tbuf * out);

/// Replace the rule's own part with `header { body }` (a statement rule, an `@import`, with `header;`): read again in the rule's place, swapped in if it is a rule of the same kind and
/// `check` (optional) says it holds what was set. `*ignored` is true when it does not; nothing has changed then. On success the event is sent.
typedef GTEXT_CSS_Status (*own_check)(css_rule * rule, const css_own * own, void * user, bool * ok);
GTEXT_CSS_Status css_rule_replace_own(css_rule * rule, const char * header, size_t header_length, const char * body, size_t body_length, bool statement, own_check check, void * user,
    bool * ignored, GTEXT_CSSOM_Outcome * outcome);

/// A rule changed as a whole: the sheet's version moves and the event is sent. `outcome` may be NULL.
/// The one event of a rule whose own text changed. `before` (taken) is its selector list before the change, if the caller could read it; `before_failed`: it could not.
void css_rule_replaced(css_rule * rule, GTEXT_CSS_Selector_List * before, bool before_failed, GTEXT_CSSOM_Outcome * outcome);

/* ---- media lists (cssom_media.c) ---------------------------------------------------------------------------------------------------------- */

/// The owner of a media list is going: the list is released and no longer reaches it. A NULL list is a no-op.
/// The canonical text of a media query list (a text always parses: a bad query is `not all`), appended to `out`.
GTEXT_CSS_Status css_media_canonical(css_core * core, const char * text, size_t length, tbuf * out);

/// The media query list of an `@import` or `@media` rule as text, in a new buffer, without making the `MediaList` object (which would take an id).
GTEXT_CSS_Status css_rule_media_text(css_rule * rule, char ** out, size_t * length);
void css_media_owner_gone(struct GTEXT_CSSOM_Media_List ** list);

/* ---- the declaration block of a typed rule (cssom_typed.c) ------------------------------------------------------------------------------------ */

bool css_typed_kind(GTEXT_CSSOM_Rule_Kind kind);
/// The block of a typed rule, built from its typed model; `rule->style` is set.
GTEXT_CSS_Status css_typed_build_style(css_rule * rule);
/// The view of a typed rule that has not been installed: a block and the descriptors, owned by whoever holds it. `block` is NULL when there is nothing to install.
typedef struct {
  GTEXT_CSS_Declaration_Block * block;
  css_desc * descs;
  size_t ndescs, capdescs;
} css_typed_pending;
/// The view of the rule as the candidate `own` would make it, built while the call can still fail with nothing changed. Empty (`block` NULL) when the rule's block was never built.
GTEXT_CSS_Status css_typed_pending_of_own(css_rule * rule, const css_own * own, css_typed_pending * pending);
/// Install the view in the rule's block, after css_own_commit(): infallible. An empty view changes nothing.
void css_typed_pending_install(css_rule * rule, css_typed_pending * pending);
/// Let go of a view that was not installed.
void css_typed_pending_discard(css_core * core, css_typed_pending * pending);
void css_typed_free(GTEXT_CSSOM_Decl * decl);
/// The view of a typed rule, to be installed in a declaration object: a block and the descriptors (both owned by the caller).
GTEXT_CSS_Status css_typed_view(css_rule * rule, GTEXT_CSS_Declaration_Block ** block, css_desc ** descs, size_t * ndescs);

/// Whether the IDL attribute exists on the block of this typed rule: a descriptor of the rule, or a property the rule takes (a page has no `cssFloat`).
bool css_typed_has_attribute(const GTEXT_CSSOM_Decl * decl, const char * attribute, size_t length);
bool css_typed_is_descriptor(const GTEXT_CSSOM_Decl * decl, const char * name, size_t length);
size_t css_typed_desc_count(const GTEXT_CSSOM_Decl * decl);
const css_desc * css_typed_desc_find(const GTEXT_CSSOM_Decl * decl, const char * name, size_t length);
/// cssom_decl.c: send the one event of a change that is complete, as every mutator does.
void css_decl_finish_change(GTEXT_CSSOM_Decl * d, GTEXT_CSS_Property_Id * ids, size_t id_count, const GTEXT_CSS_String * customs, size_t custom_count, bool attribute_written,
    GTEXT_CSSOM_Outcome * out);
/// The edits (cssom_decl.c calls them with the mutator already begun).
GTEXT_CSS_Status css_typed_set_descriptor(GTEXT_CSSOM_Decl * decl, const char * name, size_t length, const char * value, size_t value_length, bool important, GTEXT_CSSOM_Outcome * out);
GTEXT_CSS_Status css_typed_remove_descriptor(GTEXT_CSSOM_Decl * decl, const char * name, size_t length, GTEXT_CSSOM_Outcome * out);
/// A property edit made on `work` (a clone of the block) is accepted if the rule reads again with what `work` says. Takes `work` over. `*accepted` false: it was refused.
GTEXT_CSS_Status css_typed_commit_block(GTEXT_CSSOM_Decl * decl, GTEXT_CSS_Declaration_Block * work, bool * accepted, GTEXT_CSSOM_Outcome * out);
GTEXT_CSS_Status css_typed_set_css_text(GTEXT_CSSOM_Decl * decl, const char * text, size_t length, GTEXT_CSSOM_Outcome * out);
/// The descriptors as `name: value;` text appended to `out` (a space before each when `out` holds text).
GTEXT_CSS_Status css_typed_desc_text(const GTEXT_CSSOM_Decl * decl, tbuf * out);
/// The dashed property name an IDL attribute of a typed block stands for (`pageOrientation` -> `page-orientation`), in `buf` of `cap` bytes; false if it does not fit.
bool css_typed_idl_name(const char * attribute, size_t length, char * buf, size_t cap, size_t * n);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSSOM_TYPED_H
