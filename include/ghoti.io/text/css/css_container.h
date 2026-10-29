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
 * @file css_container.h
 * @brief Container queries (`@container`): the condition, the interface the caller supplies, and the query container of an element.
 *
 * Normative text: CSS Conditional 5 sections 5 and 6 (container queries, the size, style and scroll-state features), CSS Anchor Positioning 2 (the `anchored()` query),
 * CSS Values 4 (`cq*` units), as served 2026-10-28.
 *
 * ## What the library does and what it asks
 *
 * The library lays nothing out. A container query is a function of the **query container** of an element, and everything about the container that is not a computed style
 * is the caller's: its content box, its scroll state, the fallback applied to it. The caller gives them in a @ref GTEXT_CSS_Container_Interface in the options of the
 * conditions object (css_condition.h); every callback is optional, and a query that needs one the caller did not give is GTEXT_CSS_E_UNSUPPORTED naming it (`container.size`),
 * never a guess. A document without layout (a caller that supplies only `computed_style`) has no size containers: a size query is then false, because the
 * `container-type` of nothing is `size` (a style query still resolves, as any element is a style container).
 *
 * ## The query container (Conditional 5, 5.4)
 *
 * "For each element, the query container to be queried is selected from among the element's ancestor query containers that are established as a valid query container for all the
 * container features in the `<container-query>`. If the `<container-query>` contains unknown or unsupported container features, no query container will be selected for that
 * `<container-condition>`. The `<container-name>` filters the set of query containers considered to just those with a matching query container name." The ancestors are the
 * **flat tree** ancestors (the element interface's `inheritance_parent`) of the element; for a pseudo-element other than `::slotted()` and `::part()` they begin with the element
 * the pseudo-element belongs to. An element is a query container for size features if its computed `container-type` is `size` or `inline-size`, for scroll-state features if it
 * has `scroll-state`, for style features always ("by default, all elements are query containers for the purpose of container style queries"), and for a query of several kinds
 * if it is a container for each. **Container names are not tree-scoped** ("Container names are not tree-scoped names", Conditional 5, 5.2): a name matches by its identifier.
 * Where no ancestor qualifies the condition is unknown, so false at the top. Each `<container-condition>` of a comma-separated list selects its own container and the list is
 * true if any is.
 *
 * Resolution is memoized in the caller's @ref GTEXT_CSS_Condition_Call (css_condition.h), so that a deep chain of containers is not a quadratic walk: the container of an element
 * for a request (a name and the kinds of feature asked) is its parent's answer when the parent does not qualify, one entry for each (element, request).
 * **A call is the memo of one snapshot of the tree**: when the tree, a computed style or a size changes, make a new call (or gtext_css_condition_call_reset()).
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CONTAINER_H
#define GHOTI_IO_GTEXT_CSS_CSS_CONTAINER_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_computed.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_media.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A parsed `<container-query-list>`: the same immutable AST as a media query list, one root for each `<container-condition>`. Free it with gtext_css_media_list_free(). */
typedef GTEXT_CSS_Media_List GTEXT_CSS_Container_Query;

/**
 * @brief Parse the prelude of an `@container` rule: `<container-condition>#`.
 *
 * `[ <container-name>? <container-query>? ]!` for each entry; a name only is valid and is true for a container of that name. The keywords `none`, `and`, `or`, `not` and the
 * CSS-wide keywords are not container names. **A condition that does not match the grammar makes the whole rule invalid**: the result is a list of one query that is `not all`
 * (it parses, serializes as `not all`, never matches), `result->replaced` is 1 and the result holds a diagnostic. An unknown or invalid feature (`(asdf)`, `(width == 1px)`,
 * `foo(width)`) is `<general-enclosed>`: unknown, and it selects no container. Size features are `width`, `height`, `inline-size`, `block-size`, `aspect-ratio` and `orientation`
 * in the plain, boolean and range forms, with `min-` and `max-` on the range ones.
 *
 * @return GTEXT_CSS_OK (also when the rule is invalid); GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; the limits of the parse.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_container_parse_values(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Container_Query ** out, GTEXT_CSS_Media_Result * result);

/** @brief gtext_css_container_parse_values() from text. */
GTEXT_API GTEXT_CSS_Status gtext_css_container_parse(const char * text, size_t length, const GTEXT_CSS_Media_Options * options, GTEXT_CSS_Container_Query ** out,
    GTEXT_CSS_Media_Result * result);

/** @brief How many `<container-condition>`s the list has (0 for an invalid rule's `not all` is 1: see gtext_css_media_list_query_replaced()). */
GTEXT_API size_t gtext_css_container_query_count(const GTEXT_CSS_Container_Query * query);

/** @brief The container name of the `index`th condition (NULL: none), and its length through `length`. Valid for the life of the list. */
GTEXT_API const char * gtext_css_container_query_name(const GTEXT_CSS_Container_Query * query, size_t index, size_t * length);

/** @name What a condition asks of its container
 *  The bits gtext_css_container_query_kinds() returns.
 *  @{ */
#define GTEXT_CSS_CONTAINER_ASKS_SIZE 0x1u        ///< A size feature.
#define GTEXT_CSS_CONTAINER_ASKS_STYLE 0x2u       ///< `style()`.
#define GTEXT_CSS_CONTAINER_ASKS_SCROLL_STATE 0x4u ///< `scroll-state()`.
#define GTEXT_CSS_CONTAINER_ASKS_ANCHORED 0x8u    ///< `anchored()`.
#define GTEXT_CSS_CONTAINER_ASKS_UNKNOWN 0x10u    ///< An unknown or unsupported feature: no container is selected.
/** @} */

/** @brief The kinds of feature the `index`th condition asks of its container (GTEXT_CSS_CONTAINER_ASKS_*; 0 for a name alone). */
GTEXT_API uint32_t gtext_css_container_query_kinds(const GTEXT_CSS_Container_Query * query, size_t index);

/* ---- the interface ---------------------------------------------------------------------------------------- */

/** @brief The content box of a query container, in CSS pixels. */
typedef struct {
  double width;  ///< The width of the content box (not the border box).
  double height; ///< Its height.
} GTEXT_CSS_Container_Metrics;

/** @name Physical edges and axes
 *  The bits of GTEXT_CSS_Container_Scroll_State. The library maps the logical keywords of a query to these by the container's writing mode and direction.
 *  @{ */
#define GTEXT_CSS_EDGE_TOP 0x1u
#define GTEXT_CSS_EDGE_RIGHT 0x2u
#define GTEXT_CSS_EDGE_BOTTOM 0x4u
#define GTEXT_CSS_EDGE_LEFT 0x8u
#define GTEXT_CSS_SNAP_AXIS_X 0x1u
#define GTEXT_CSS_SNAP_AXIS_Y 0x2u
/** @} */

/** @brief The scroll state of a query container (Conditional 5, 6.3), at the last snapshot of post-layout state. */
typedef struct {
  bool sticky;            ///< The container is sticky positioned (`stuck: none` matches only a container that is).
  bool scroll_container;  ///< The container is a scroll container (`scrollable` and `scrolled` match only one that is).
  bool snap_target;       ///< The container is a snap target of its scroll container.
  uint32_t stuck;         ///< GTEXT_CSS_EDGE_*: the edges it is stuck to.
  uint32_t snapped;       ///< GTEXT_CSS_SNAP_AXIS_*: the axes it is snapped in.
  uint32_t scrollable;    ///< GTEXT_CSS_EDGE_*: the edges past which it has scrollable overflow.
  uint32_t scrolled;      ///< GTEXT_CSS_EDGE_*: the direction of its most recent relative scroll (none, or one edge).
} GTEXT_CSS_Container_Scroll_State;

/**
 * @struct GTEXT_CSS_Container_Interface
 * @brief What the caller knows about its containers. Size-prefixed; every callback is optional.
 *
 * Ancestors are computed before their descendants (inheritance requires it), so `computed_style` never forces a cycle: it returns the **already computed style** of an
 * element, or NULL for one that has none yet (which is then not a container). The library never asks about the element being styled.
 */
typedef struct {
  size_t size;  ///< `sizeof` this struct as the caller's header has it.
  void * user;  ///< Passed to the callbacks.
  /** The computed style of an element, from which `container-type`, `container-name`, `writing-mode`, `direction`, `font-size` and the custom properties are read. NULL: nothing is a container. */
  const GTEXT_CSS_Computed_Style * (*computed_style)(void * user, GTEXT_CSS_Element_Handle element);
  /** The compute context the caller's styles were computed with. A style query computes the queried value in it, as a declaration of the container (a child of the container's style, so `em` is the
   *  container's font size and `var()` reads its custom properties), and compares it with the container's own computed value; a style belongs to the context that made it, so only the caller can say which.
   *  NULL: a style query that needs a computation (anything but the presence of a custom property) is GTEXT_CSS_E_UNSUPPORTED naming `container.compute_context`. */
  const GTEXT_CSS_Compute_Context * compute_context;
  /** The content box of a container. Returns false if it has no principal box (the size features are then unknown). NULL: a size query is GTEXT_CSS_E_UNSUPPORTED naming `container.size`. */
  bool (*content_box)(void * user, GTEXT_CSS_Element_Handle container, GTEXT_CSS_Container_Metrics * out);
  /** The scroll state of a container. NULL: a scroll-state query is GTEXT_CSS_E_UNSUPPORTED naming `container.scroll_state`. */
  bool (*scroll_state)(void * user, GTEXT_CSS_Element_Handle container, GTEXT_CSS_Container_Scroll_State * out);
  /** Whether the `position-try-fallbacks` option applied to an anchored container matches the value of an `anchored(fallback: value)` query (`value` is the query's
   *  text, normalized; `none` asks for no fallback applied). The position-area comparison, which depends on writing modes, is the caller's. NULL: GTEXT_CSS_E_UNSUPPORTED naming `container.anchored_fallback`. */
  bool (*anchored_fallback)(void * user, GTEXT_CSS_Element_Handle container, const GTEXT_CSS_String * value, bool * matches);
} GTEXT_CSS_Container_Interface;

/** @name The kinds of container a computed style makes
 *  The bits of GTEXT_CSS_Container_Info::types.
 *  @{ */
#define GTEXT_CSS_CONTAINER_TYPE_SIZE 0x1u
#define GTEXT_CSS_CONTAINER_TYPE_INLINE_SIZE 0x2u
#define GTEXT_CSS_CONTAINER_TYPE_SCROLL_STATE 0x4u
#define GTEXT_CSS_CONTAINER_TYPE_ANCHORED 0x8u
/** @} */

/** @brief What a computed style says about the element as a container. */
typedef struct {
  uint32_t types;                          ///< GTEXT_CSS_CONTAINER_TYPE_* of the computed `container-type` (0 for `normal`).
  size_t name_count;                       ///< The names of the computed `container-name` (0 for `none`).
  const GTEXT_CSS_Computed_Style * style;  ///< The style (for gtext_css_container_info_name()).
} GTEXT_CSS_Container_Info;

/**
 * @brief Read `container-type` and `container-name` from a computed style.
 *
 * @param style The style. Not NULL.
 * @param info Receives the answer. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument); GTEXT_CSS_E_UNSUPPORTED (a registry without the two properties).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_container_info(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Container_Info * info);

/** @brief The `index`th name of an info (valid for the life of the style); false past the end. */
GTEXT_API bool gtext_css_container_info_name(const GTEXT_CSS_Container_Info * info, size_t index, GTEXT_CSS_String * name);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CONTAINER_H
