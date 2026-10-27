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
 * What the computed-value files share: the context, the style, the state of one computation, the value builder, and the interfaces between the
 * walker over value types (css_compute_value.c), the property algorithms (css_compute_algo.c), the units (css_compute_units.c) and the driver
 * (css_compute.c). Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_COMPUTE_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_COMPUTE_INTERNAL_H

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_computed.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_logical.h>
#include <ghoti.io/text/css/css_shorthand.h>
#include <ghoti.io/text/macros.h>

#include "css_compute_tables.h"
#include "css_custom.h"

#include "css_internal.h"
#include "css_math_internal.h"
#include "css_registry_internal.h"
#include "css_tables.h"
#include "css_value_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- logical properties (css_logical.c) ------------------------------------------------------------------ */

/// What the name of a member of a logical group says: its group's kind, whether it is flow-relative, and its slot (physical) or descriptor (flow-relative:
/// sides 0 block-start, 1 block-end, 2 inline-start, 3 inline-end; corners 0 start-start, 1 start-end, 2 end-start, 3 end-end; axes 0 block, 1 inline).
typedef struct {
  uint8_t kind;     ///< GTEXT_CSS_Logical_Kind.
  uint8_t logical;  ///< Flow-relative.
  uint8_t desc;     ///< See above.
  uint8_t slot;     ///< A physical member's slot.
} css_lmember;

/// Read a member's name in its group; false when the group or the name is not one of the rules'.
bool css_logical_describe(const char * group, const char * name, size_t length, css_lmember * out);
/// The slot a member is in this flow.
size_t css_logical_slot_of(const css_lmember * m, const GTEXT_CSS_Flow * flow);

/* ---- the value builder ------------------------------------------------------------------------ */

/// A set being made: values go in its arena, sets its values point into are kept alive by it.
typedef struct {
  GTEXT_CSS_Value_Set * set;   ///< The set under construction; NULL after css_vb_finish().
  GTEXT_Allocator alloc;
  GTEXT_CSS_Status status;     ///< OK until something failed (the first failure wins).
  size_t nodes;                ///< Values made.
  size_t max_nodes;            ///< The limit.
} css_vb;

/// Begin a set. False (and `vb->status`) when memory ran out.
bool css_vb_begin(css_vb * vb, const GTEXT_Allocator * alloc, size_t max_nodes);
/// One zeroed value of the set's arena, counted against the limit; NULL (with the status) on failure.
GTEXT_CSS_Value * css_vb_value(css_vb * vb);
/// An array of `n` zeroed values; NULL (with the status) on failure or past the limit.
GTEXT_CSS_Value * css_vb_values(css_vb * vb, size_t n);
/// A NUL-terminated copy of `n` bytes in the arena.
const char * css_vb_str(css_vb * vb, const char * s, size_t n);
/// Keep another set alive for as long as this one lives (a value of this one points into it).
bool css_vb_dep(css_vb * vb, const GTEXT_CSS_Value_Set * other);
/// Make the set whole with `root` (a value of its arena, or a value of a set it depends on) and give it up; the builder is spent.
GTEXT_CSS_Value_Set * css_vb_finish(css_vb * vb, const GTEXT_CSS_Value * root);
/// Release a set that was not finished.
void css_vb_abort(css_vb * vb);
/// Fail the builder.
void css_vb_fail(css_vb * vb, GTEXT_CSS_Status status);

/// Fill `v` (zeroed) as a keyword with the given text (not interned: its atom is 0).
void css_v_keyword(css_vb * vb, GTEXT_CSS_Value * v, const char * text);
void css_v_number(GTEXT_CSS_Value * v, double number);
void css_v_integer(GTEXT_CSS_Value * v, double number);
void css_v_percentage(GTEXT_CSS_Value * v, double number);
void css_v_dimension(GTEXT_CSS_Value * v, double number, GTEXT_CSS_Unit unit);
/// A list whose children array is `kids` (already in the arena or in a set the builder depends on).
void css_v_list(GTEXT_CSS_Value * v, const GTEXT_CSS_Value * kids, size_t n, GTEXT_CSS_List_Separator sep);
/// A function whose name is `name` (copied) and whose arguments are `kids`.
void css_v_function(css_vb * vb, GTEXT_CSS_Value * v, const char * name, const GTEXT_CSS_Value * kids, size_t n);

/* ---- the context ------------------------------------------------------------------------------ */

/// The properties the algorithms read by name (their ids in the registry).
enum {
  CSS_P_WRITING_MODE = 0, CSS_P_DIRECTION, CSS_P_TEXT_ORIENTATION, CSS_P_FONT_FAMILY, CSS_P_FONT_STYLE, CSS_P_FONT_WEIGHT,
  CSS_P_FONT_WIDTH, CSS_P_FONT_SIZE, CSS_P_FONT_SIZE_ADJUST, CSS_P_LINE_HEIGHT, CSS_P_FLOAT, CSS_P_POSITION, CSS_P_DISPLAY,
  CSS_P_COLOR, CSS_P_MATH_DEPTH, CSS_P_MATH_STYLE, CSS_P_TEXT_ALIGN_ALL, CSS_P_ALL, CSS_P_UNICODE_BIDI, CSS_P_COLOR_SCHEME, CSS_P_CUSTOM,
  CSS_P_COUNT
};

/// What the context knows of one property (an index is the id minus one).
typedef struct {
  uint8_t klass;                 ///< GTEXT_CSS_Compute_Class.
  uint8_t algo;                  ///< GTEXT_CSS_Compute_Algorithm.
  uint8_t flags;                 ///< CSS_TBL_COMPUTE_*.
  bool inherited;                ///< Inherited (for `unset` and for a property without a declaration).
  bool shorthand;                ///< Has longhands (or is `all`).
  bool generated;                ///< From the pin.
  GTEXT_CSS_Property_Id alias_of;///< For an alias: the target, followed.
  int32_t group;                 ///< The logical group (index into the context's groups), or -1.
  uint8_t member_kind;           ///< For a group member: 0 physical, 1 flow-relative.
  uint8_t member_slot;           ///< For a physical member: its slot.
  GTEXT_CSS_Property_Id partner; ///< A border width: the style property of the same side; else 0.
  GTEXT_CSS_Value_Set * initial; ///< The computed initial value, shared by every style; NULL when `initial_dependent` or there is none.
  GTEXT_CSS_Value_Set * initial_spec; ///< The specified initial value (parsed); NULL when the property has none.
  bool initial_dependent;        ///< The computed initial value depends on the element: it is computed for each style.
  const char * name;             ///< The property's name (owned by the registry).
  size_t name_length;
} css_pmeta;

/// A logical property group: its kind, and for each slot the physical property.
typedef struct {
  const char * name;
  GTEXT_CSS_Logical_Kind kind;
  size_t nmembers;
  GTEXT_CSS_Property_Id members[8];
  GTEXT_CSS_Property_Id physical[4];  ///< The physical member of each slot.
} css_group;

struct GTEXT_CSS_Compute_Context {
  const GTEXT_CSS_Registry * registry;
  GTEXT_Allocator alloc;
  GTEXT_CSS_Compute_Options options;
  GTEXT_CSS_Environment env;
  size_t nprops;
  css_pmeta * meta;                         ///< `nprops` of them.
  css_group * groups;
  size_t ngroups;
  GTEXT_CSS_Property_Id pid[CSS_P_COUNT];   ///< The properties the algorithms read by name; 0 where the registry has none.
  GTEXT_CSS_Property_Id * order;            ///< The properties computed first (ids), then all the others.
  size_t norder;
  size_t nfirst;                            ///< How many of `order` come from the table.
  bool building;                            ///< The initial values are being made (single thread, inside gtext_css_compute_context_new()).
  char * base_url;                          ///< A copy of the environment's base URL, or NULL.
  char * language;                          ///< A copy of the environment's language, or NULL.
};

/// The class row of a property in the generated table, following an override to the property of the same name in the pin; NULL for a caller's own.
const css_tbl_compute * css_compute_row(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id, GTEXT_CSS_Property_Id * generated_id);

/* ---- a style ------------------------------------------------------------------------------------ */

/// What a style remembers of the root of inheritance, for `rem`, `rlh` and the root forms of the metrics.
typedef struct {
  bool valid;                   ///< The style has a root (every style of a chain does; a root's is its own).
  double font_px;               ///< The computed font-size of the root.
  GTEXT_CSS_Value_Set * family; ///< Its font-family (retained).
  double weight, width_percent, oblique_degrees;
  bool italic, vertical;
  GTEXT_CSS_Value_Set * line_height; ///< Its line-height (retained).
} css_root_info;

struct GTEXT_CSS_Computed_Style {
  const GTEXT_CSS_Compute_Context * cc;
  GTEXT_Allocator alloc;
  size_t n;                            ///< Properties.
  GTEXT_CSS_Value_Set ** vals;         ///< A set (retained) for every property; NULL where none.
  GTEXT_CSS_Flow flow;
  struct css_cmap * custom;           ///< The custom properties (css_custom.h): the parent's map with this element's declarations set, shared with it where nothing is declared.
  css_root_info root;
  GTEXT_CSS_Element_Handle element;
};

/* ---- one computation ---------------------------------------------------------------------------- */

typedef struct {
  const GTEXT_CSS_Compute_Context * cc;
  const GTEXT_CSS_Environment * env;   ///< The environment the values are computed in (the context's, or one in the other color scheme while the initial values are made).
  const GTEXT_CSS_Declaration_Block * block;
  const GTEXT_CSS_Computed_Style * parent;
  GTEXT_CSS_Computed_Style * style;
  GTEXT_CSS_Element_Handle element;
  GTEXT_CSS_Compute_Result * res;
  unsigned char * done;           ///< Per property: its value is in the style.
  uint32_t * own;                 ///< Per property: the index + 1 of the last declaration of it in the block, or 0.
  uint32_t all_pos;               ///< The index + 1 of the last `all` declaration, or 0.
  GTEXT_CSS_Property_Id current;  ///< The property being computed.
  bool probing;                   ///< Making the initial values: reads of the style are not traps.
  bool used_context;              ///< Set when a value was computed from something that varies by element (the initial values' dependence).
  GTEXT_CSS_Status status;
  size_t work;
} css_state;

/// Fail a computation: sets the result (status, property, name, detail) once. Returns the status.
GTEXT_CSS_Status css_st_fail(css_state * st, GTEXT_CSS_Status status, const char * name, const char * detail);

/// The computed value of a property of the element, which must be computed already (reading one that is not is an internal error).
const GTEXT_CSS_Value_Set * css_st_get(css_state * st, GTEXT_CSS_Property_Id id);

/// The color scheme the element uses (CSS Color Adjust 1, 2.3 "To determine the used color scheme"): its computed `color-scheme` and the environment's preference.
GTEXT_CSS_Color_Scheme css_st_used_scheme(css_state * st);
/// The root of that value, or NULL.
const GTEXT_CSS_Value * css_st_root(css_state * st, GTEXT_CSS_Property_Id id);
/// The same for the parent's style (or the initial value when there is no parent).
const GTEXT_CSS_Value_Set * css_st_parent_get(css_state * st, GTEXT_CSS_Property_Id id);
const GTEXT_CSS_Value * css_st_parent_root(css_state * st, GTEXT_CSS_Property_Id id);

/// The computed font-size, in px, that `em` stands for in the property being computed: the parent's (or the default) for `font-size` itself,
/// the element's for the others. Marks the computation as element-dependent.
double css_st_em(css_state * st);
/// The font-size of the parent in px (the default font size without a parent).
double css_st_parent_font_px(css_state * st);
/// The px of one relative unit for the property being computed; false (and the failure set) when the environment cannot say.
bool css_st_unit_px(css_state * st, GTEXT_CSS_Unit unit, double * px);
/// The environment's font query for this element (its computed font properties so far), or the root's.
void css_st_font_query(css_state * st, bool root, GTEXT_CSS_Font_Query * q);

/// The inherited value of a property (the parent's set retained; the initial value when there is no parent).
GTEXT_CSS_Status css_st_inherit(css_state * st, GTEXT_CSS_Property_Id id, GTEXT_CSS_Value_Set ** out);

/// Whether a property's keyword value equals `word` (ASCII case-insensitive): the value must be a keyword.
bool css_val_is_keyword(const GTEXT_CSS_Value * v, const char * word);

/* ---- the walker over value types (css_compute_value.c) ------------------------------------------------ */

/// Compute a value by its types. `*out` is `src` (retained) when nothing changed, or a new set. `property` is the property whose slot text
/// says whether a calculation is an integer; 0 for none.
GTEXT_CSS_Status css_cv_by_type(css_state * st, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Property_Id property,
    GTEXT_CSS_Value_Set ** out);

/// css_cv_by_type() with options.
#define CV_MATH_NUMBERS_ONLY 0x2u  ///< Only a calculation of numbers or percentages is simplified (the computed value of a property that is "as specified").
#define CV_NUMBERS_TO_PX 0x1u  ///< A number is a length in px (SVG 2: "numbers converted to absolute lengths first").
GTEXT_CSS_Status css_cv_by_type_ex(css_state * st, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Property_Id property, unsigned flags,
    GTEXT_CSS_Value_Set ** out);

/// A canonical absolute number of the unit of a dimension value, for the algorithms: false for a value that is not one.
bool css_cv_length_px(css_state * st, const GTEXT_CSS_Value * v, double * px);

/* ---- the algorithms (css_compute_algo.c) ----------------------------------------------------------------- */

/// Compute a declared value by the property's algorithm. `src` is the specified value's set (VALID), `*out` the computed value (a new set or
/// `src` retained). The parent's and the element's earlier values are read through css_st_get().
GTEXT_CSS_Status css_algo_compute(css_state * st, GTEXT_CSS_Property_Id id, const css_pmeta * meta, const GTEXT_CSS_Value_Set * src,
    GTEXT_CSS_Value_Set ** out);

/// The registry a block was made for.
const GTEXT_CSS_Registry * css_block_registry(const GTEXT_CSS_Declaration_Block * block);

/* ---- numbers and text -------------------------------------------------------------------------------- */

/// RFC 3986 reference resolution (5.2). Returns 0 when the reference stays as it is (it is absolute, empty or a fragment, or there is no absolute
/// base), 1 with a new NUL-terminated string in `*out` (release with the allocator) and its length, or -1 when memory ran out.
int css_resolve_url(const GTEXT_Allocator * alloc, const char * base, size_t base_len, const char * ref, size_t ref_len, char ** out,
    size_t * out_len);

/// The font query of a style at a size (the computed font properties of the style, as far as they are computed).
void css_st_font_query_of(const GTEXT_CSS_Computed_Style * style, double size_px, GTEXT_CSS_Font_Query * q);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_COMPUTE_INTERNAL_H
