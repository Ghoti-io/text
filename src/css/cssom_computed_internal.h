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
 * What `getComputedStyle()` shares between its files (css_cssom_computed.h): the document's session (the compute context, the match context of the caller's tree, the style cache
 * and the registry of computed declarations) and the state of one computed declaration. Private.
 *
 * **One state, three clocks.** Everything the session holds is valid for one *pair* (the document's generation, the caller's tree version) and, with a tree version that comes
 * from `tree_changed` calls, the epoch of those. When a read finds the pair moved, the style cache and the match context (a snapshot of the tree) are dropped whole; they are
 * rebuilt on demand, so the answer never depends on what the cache happened to hold.
 *
 * **Who holds what.** The session holds a style in the cache with one count, dropped when the pair moves, when the document handle is released and when the compute context is
 * replaced (a style belongs to the context that made it). A computed declaration holds the *values* of its block, which are counted and outlive any style.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSSOM_COMPUTED_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSSOM_COMPUTED_INTERNAL_H

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_cssom.h>
#include <ghoti.io/text/macros.h>

#include "cssom_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The length of a name in a fixed buffer.
static inline size_t css_name_length(const char * name, size_t cap) {
  size_t n = 0;
  while (n < cap && name[n]) {
    n++;
  }
  return n;
}

/// The categories of a resolved value (tools/css/resolved_value.tsv, generated into cssom_resolved_tables.h).
typedef enum {
  CSS_RESOLVED_COMPUTED = 0,       ///< The computed value.
  CSS_RESOLVED_USED_COLOR,         ///< The used value of a color: the computed value with `currentcolor` replaced.
  CSS_RESOLVED_USED_IF_APPLIES,    ///< The used value if the property applies and `display` is neither `none` nor `contents`, else the computed value.
  CSS_RESOLVED_USED_IF_POSITIONED, ///< The same for a positioned element whose property is not over-constrained.
  CSS_RESOLVED_LINE_HEIGHT,        ///< `normal` if the computed value is, else the used value.
  CSS_RESOLVED_USED,               ///< The property's own specification says the used value (a transform as a matrix).
  CSS_RESOLVED_ANIMATION_DURATION, ///< `auto` is `0s` when `animation-timeline` is `auto` (Animations 2).
  CSS_RESOLVED_MIN_SIZE_AUTO,      ///< `auto` is `0px` for the boxes of the CSS2 display types when `aspect-ratio` is `auto` (Sizing 3).
  CSS_RESOLVED_BORDER_WIDTH,       ///< `0px` when the border style of the same side is `none` or `hidden` (Backgrounds 3: the used width is 0, and the resolved value is the used value).
  CSS_RESOLVED_COUNT
} css_resolved_category;

/// The resolved category of a longhand by name (lower case): CSS_RESOLVED_COMPUTED for a name the table does not list.
css_resolved_category css_resolved_lookup(const char * name, size_t length);

/// The row count of the table, for the tests of the generator.
size_t css_resolved_row_count(void);

/// One element's record: its cached style and the computed declarations of it.
typedef struct css_rec css_rec;
struct css_rec {
  const void * element;
  GTEXT_CSS_Computed_Style * style;   ///< The cached style (one count), or NULL.
  css_rec * hnext;                    ///< The next record of the same bucket.
  css_rec * fprev, * fnext;           ///< The records that hold a style, the one cached first at the head.
  bool in_fifo;
  struct GTEXT_CSSOM_Decl * decls;    ///< The computed declarations of this element, linked through their `comp`.
};

/// The state of a computed declaration.
typedef struct css_comp {
  const void * element;
  GTEXT_CSS_Selector_List * pseudo;   ///< The pseudo-element (owned), or NULL for the element.
  bool null_obj;                      ///< `obj` is null: the pseudo-element argument was `::slotted()`, `::part()` or not a pseudo-element selector. Always empty.
  bool valid;                         ///< The declarations are those of `style` for the pair below.
  bool empty;                         ///< The first steps made the list empty.
  bool detached;                      ///< The element was released or the document was: the declarations are frozen as they were.
  uint64_t gen, tree, epoch, cepoch;  ///< The pair (and the epochs) the declarations were made for.
  uint8_t * origins;                  ///< GTEXT_CSSOM_Origin by property id (`norigins` entries).
  size_t norigins;
  char * position;                    ///< The computed `background-position` as the computed layer writes it (two offsets from the top left), made with the declarations; NULL where there is none (the declarations are the longhands').
  size_t position_length;
  struct GTEXT_CSSOM_Decl * rec_prev, * rec_next;   ///< The declarations of one element.
  struct GTEXT_CSSOM_Decl * all_prev, * all_next;   ///< Every computed declaration of the document.
  css_rec * rec;                      ///< The record of the element (while the declaration is linked in it).
} css_comp;

/// The document's session for `getComputedStyle()`.
typedef struct css_cmps {
  GTEXT_CSS_Compute_Context * own;    ///< The context the document made for itself (with the registrations of `core->props` at the time).
  bool own_exported;                  ///< gtext_cssom_document_container_interface() handed `own` out: a conditions object may hold it, so a replacement keeps it (`retired`) until the document goes.
  struct css_retired * retired;       ///< Contexts replaced after they were handed out, with the sets they borrow, oldest first.
  size_t nretired, retired_cap;
  const GTEXT_CSS_Compute_Context * ctx;   ///< The context in use: the caller's, or `own`.
  GTEXT_CSS_Selector_Match_Context * match;
  GTEXT_CSS_Condition_Call call;
  bool call_init;
  bool pair_valid;
  uint64_t pgen, ptree, pepoch, pcepoch;
  uint64_t cepoch;                    ///< Advances when the compute context is replaced.
  css_rec ** buckets;
  size_t nbuckets, nrecs;
  css_rec * fifo_head, * fifo_tail;
  size_t nstyles;
  struct GTEXT_CSSOM_Decl * all_head;
  const void ** released;             ///< Elements released from a callback of a read, whose records go when the outermost read ends.
  size_t nreleased, released_cap;
  bool released_lost;                 ///< One of them could not be written down: the whole computed state goes when the read ends.
  GTEXT_CSSOM_Computed_Failure failure;
} css_cmps;

/// A compute context that a registration change replaced after the document had handed it out, and the registrations it borrows.
typedef struct css_retired {
  GTEXT_CSS_Compute_Context * ctx;
  GTEXT_CSS_At_Property_Set * props;
} css_retired;

/// The session, made on first use; NULL on memory.
css_cmps * css_cmps_get(css_core * core);

/// The computed `background-position` of a computed declaration, as the computed layer writes it (valid until the declaration is refreshed); false where the declaration has none.
bool css_computed_position_text(const struct GTEXT_CSSOM_Decl * decl, const char ** text, size_t * length);

/// The first read or a later one found the pair moved: make the declarations again. A no-op for a declaration whose state is current.
GTEXT_CSS_Status css_computed_refresh(struct GTEXT_CSSOM_Decl * decl);

/// A computed declaration is being freed.
void css_computed_forget(struct GTEXT_CSSOM_Decl * decl);

/// The element handle is about to be freed or reused. From a callback of a read the records go when the outermost read ends (the read walks them).
void css_computed_element_released(css_core * core, const void * element);

/// The document handle was released: every style goes, every declaration is frozen, the session is torn down.
void css_computed_teardown(css_core * core);
/// The document handle is gone: tear the computed state down now, or when the read that is running (one of whose callbacks released the handle) ends.
void css_computed_teardown_soon(css_core * core);

/// The registrations of the `@property` rules of the sheets changed (cssom_index.c, when it builds an index): `props` (taken; NULL for none) replaces `core->props`. The compute context the
/// document made with the old ones, the styles of the cache and the computed declarations read before are dropped; a caller's compute context keeps its own registrations.
void css_computed_properties_changed(css_core * core, GTEXT_CSS_At_Property_Set * props);

/// The session's memory (at the last release of the core).
void css_computed_free(css_core * core);

/// How many records the cache holds (for the counters).
size_t css_computed_cached_styles(const css_core * core);

/// The document's index for the pair (cssom_index.c).
GTEXT_CSS_Status css_index_current(css_core * core, const GTEXT_CSS_Rule_Index ** out);

/* ---- the resolved values (cssom_resolved.c) --------------------------------------------------------------------------------------------------- */

/// What making the resolved declarations of one element needs from the session.
typedef struct {
  css_core * core;
  css_cmps * cs;
  const void * element;
  const GTEXT_CSS_Selector_List * pseudo;
  const GTEXT_CSS_Rule_Index * index;                 ///< The index the styles were computed with.
  const GTEXT_CSS_Computed_Style * element_style;     ///< For a pseudo-element: the style of its originating element.
} css_resolve_in;

/// Whether an ancestor of the element (or, for a pseudo-element, the originating element itself) has `display: none`: the element generates no box.
GTEXT_CSS_Status css_computed_in_display_none(const css_resolve_in * in, bool * yes);

/// The style of the parent of the element a block is made for (the originating element, for a pseudo-element): `*out` has one count of the caller's, and is NULL at the root.
GTEXT_CSS_Status css_computed_parent_style(const css_resolve_in * in, GTEXT_CSS_Computed_Style ** out);

/// Make the block of the resolved values of a style: every supported longhand in lexicographic order, then the custom properties. `*origins` is an array indexed by property id.
GTEXT_CSS_Status css_resolved_build(const css_resolve_in * in, const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Declaration_Block ** block, uint8_t ** origins, size_t * norigins);

/// Note why a read failed (for gtext_cssom_computed_failure()).
void css_computed_note_failure(css_core * core, GTEXT_CSS_Status status, const char * callback, size_t callback_length, GTEXT_CSS_Property_Id property);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSSOM_COMPUTED_INTERNAL_H
