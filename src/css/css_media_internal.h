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
 * What the media query files share (css_media_parse.c, css_media_eval.c, css_media_serialize.c). Private.
 *
 * The list is a flat array of nodes: a query, a `not`, an `and` or `or` of operands (their numbers in `kids`), a parenthesized group, a media
 * feature, a `<general-enclosed>` and a `(--name)` reference. Nothing walks the tree by recursion: the parser, the evaluator and the serializer
 * keep their own stacks, so the depth of a query is bounded by a limit and not by the C stack.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_MEDIA_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_MEDIA_INTERNAL_H

#include <float.h>
#include <string.h>

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/text/css/css_math.h>
#include <ghoti.io/text/css/css_media.h>
#include <ghoti.io/text/css/css_selector.h>
#include <ghoti.io/text/macros.h>

#include "css_internal.h"
#include "css_media_tables.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQ_NONE UINT32_MAX

typedef enum {
  MQN_QUERY = 0, ///< One `<media-query>`: modifier, type and (kid 0) the condition. Its flags say which.
  MQN_NOT,       ///< `not <media-in-parens>`: kid 0.
  MQN_AND,       ///< `<media-in-parens> and ...`: the kids.
  MQN_OR,        ///< `<media-in-parens> or ...`: the kids.
  MQN_PAREN,     ///< `( <media-condition> )`: kid 0.
  MQN_FEATURE,   ///< `( <media-feature> )`.
  MQN_ENCLOSED,  ///< `<general-enclosed>`: unknown.
  MQN_CUSTOM,    ///< `(--name)`: a reference to a `@custom-media`.
  MQN_DECL,      ///< @supports `(property: value)`: `text` the property, `value` the value text, flag MQF_IMPORTANT.
  MQN_SELECTOR,  ///< `selector(...)`: `sel` the selector (NULL when it is not one: false).
  MQN_FONT_TECH, ///< `font-tech(x)`: `bits`.
  MQN_FONT_FORMAT, ///< `font-format(x)`: `bits` (0: a format nobody knows, which is false).
  MQN_AT_RULE,   ///< `at-rule(@x)`: `text` the name.
  MQN_NAMED,     ///< `named-feature(x)`: `text`.
  MQN_ENV,       ///< `env(x)`: `text`.
  MQN_TEST_MEDIA,    ///< @when `media( <media-feature> )`: kid 0 is the FEATURE (or an ENCLOSED, when the feature is not one).
  MQN_TEST_SUPPORTS, ///< @when `supports( ... )`: kid 0 is the condition; flag MQF_BARE when it is a declaration written without parentheses.
  MQN_CQ_FN,         ///< @container `style()`, `scroll-state()` or `anchored()` (`type_id` says which, MQ_SUB_*): kid 0 is the condition or, with MQF_BARE, a SUBFEATURE.
  MQN_SUBFEATURE     ///< A feature of those functions: `type_id` the function, `form` plain, boolean or range, `text` the name (a range: its first operand), `value` its value (a range: its last), `name` a range's middle operand.
} mq_node_kind;

/// The functions of a container condition whose arguments are not size features.
#define MQ_SUB_NONE 0
#define MQ_SUB_STYLE 1
#define MQ_SUB_SCROLL 2
#define MQ_SUB_ANCHORED 3

/// Flags of a query node.
#define MQF_NEGATED 0x1u  ///< `not` before the type.
#define MQF_ONLY 0x2u     ///< `only` before the type.
#define MQF_HAS_TYPE 0x4u ///< A media type was written.
#define MQF_REPLACED 0x8u ///< Invalid: replaced by `not all` (written as a negated `all`).
#define MQF_IMPORTANT 0x10u ///< A supports declaration written with `!important`.
#define MQF_FALSE 0x40u ///< An ENCLOSED of a supports condition: false and not unknown (Conditional 3: "Removed the 'unknown' value ... defining unrecognized syntaxes as 'false'").
#define MQF_BARE 0x80u ///< A function whose argument is a bare feature or declaration, not a parenthesized condition.
#define MQF_LOGICAL 0x100u ///< A container size feature in a logical axis: `inline-size` (feature WIDTH) or `block-size` (feature HEIGHT).
#define MQF_SPEC_TEXT 0x20u ///< A supports leaf whose `text` is the whole serialization (a function).

/// What a container condition asks of its container (the bits of a container QUERY node's `bits`).
#define MQC_SIZE 0x1u
#define MQC_STYLE 0x2u
#define MQC_SCROLL 0x4u
#define MQC_ANCHORED 0x8u
#define MQC_UNKNOWN 0x10u ///< An unknown or unsupported container feature: no container is selected, the condition is false.

typedef enum { MQT_ID_ALL = 0, MQT_ID_SCREEN, MQT_ID_PRINT, MQT_ID_DEPRECATED, MQT_ID_UNKNOWN } mq_type_id;
typedef enum { MQFORM_PLAIN = 0, MQFORM_BOOLEAN, MQFORM_RANGE } mq_form;
typedef enum { MQP_NONE = 0, MQP_MIN, MQP_MAX } mq_prefix;
typedef enum { MQOP_NONE = 0, MQOP_LT, MQOP_LE, MQOP_GT, MQOP_GE, MQOP_EQ } mq_op;
/// Where the feature name is in a range: `name op value`, `value op name`, `value op name op value`.
typedef enum { MQPOS_LEFT = 0, MQPOS_RIGHT, MQPOS_MIDDLE } mq_name_pos;

typedef enum { MQV_NUMBER = 0, MQV_DIMENSION, MQV_RATIO, MQV_IDENT, MQV_MATH, MQV_INFINITE } mq_value_kind;

/// A feature's value, as typed at parse time.
typedef struct {
  uint8_t kind;           ///< mq_value_kind.
  bool integer;           ///< NUMBER: written as an integer.
  GTEXT_CSS_Unit unit;    ///< DIMENSION: the unit as written.
  double a;               ///< The number; a ratio's first number.
  double b;               ///< A ratio's second number (1 when only one was written).
  uint16_t keyword;       ///< IDENT: the index into the feature's keywords.
  GTEXT_CSS_Math * math;  ///< MATH: owned by the list. RATIO: the numerator, when it is a calculation.
  GTEXT_CSS_Math * math2; ///< RATIO: the denominator, when it is a calculation.
  uint8_t math_cat;       ///< MATH: GTEXT_CSS_Math_Category.
} mq_value;

typedef struct {
  uint8_t kind;           ///< mq_node_kind.
  uint16_t flags;         ///< MQF_*.
  uint8_t type_id;        ///< QUERY: mq_type_id.
  uint8_t form;           ///< FEATURE: mq_form.
  uint8_t prefix;         ///< FEATURE: mq_prefix.
  uint8_t op1;            ///< FEATURE range: mq_op of the first comparison.
  uint8_t op2;            ///< FEATURE range: mq_op of the second (a double range).
  uint8_t pos;            ///< FEATURE range: mq_name_pos.
  int16_t feature;        ///< FEATURE: the row.
  uint32_t kid_first;     ///< Index into kids.
  uint32_t kid_count;
  uint32_t v1;            ///< FEATURE: the value (the one written, or the left one of a double range).
  uint32_t v2;            ///< FEATURE: the right value of a double range.
  const char * text;      ///< QUERY: the media type, lower case and decoded; ENCLOSED: the serialization; CUSTOM: the name (`--x`, as written).
  size_t text_length;
  const char * name;      ///< ENCLOSED that looks like a feature (`(name: value)`, `(name)`): the name, lower case; NULL otherwise.
  size_t name_length;
  const char * value;     ///< ... and its value as CSS text (NULL for the boolean form).
  size_t value_length;
  GTEXT_CSS_Span span;
  GTEXT_CSS_Selector_List * sel; ///< SELECTOR: owned by the list.
  uint32_t bits;          ///< FONT_TECH, FONT_FORMAT; a container QUERY: MQC_* bits of the kinds of feature it uses.
  uint32_t sub_kw;        ///< A scroll-state SUBFEATURE: the keyword's index.
  const GTEXT_CSS_Component_Value * src; ///< ENCLOSED while parsing: the block or function it stands for (not valid after the parse).
} mq_node;

struct GTEXT_CSS_Media_List_Internal {
  GCU_Atomic_Int refs;
  size_t serial;         ///< Never 0, never reused by another list of the process (the memo of a call is keyed by it, not by the address a freed list leaves behind).
  GTEXT_Allocator alloc;
  css_arena arena;       ///< Strings.
  mq_node * nodes;
  size_t node_count, node_cap;
  uint32_t * kids;
  size_t kid_count, kid_cap;
  mq_value * values;
  size_t value_count, value_cap;
  GTEXT_CSS_Math ** maths;
  size_t math_count, math_cap;
  uint32_t * roots;      ///< The node of each query.
  size_t root_count, root_cap;
  uint32_t dependencies; ///< Bits of 1 << css_mq_dep of every feature mentioned (known or not).
  uint64_t features_lo, features_hi; ///< Bit i: media feature row i is mentioned (rows 0..127).
};

/// The public struct (a list is its internal block; kept opaque to callers).
struct GTEXT_CSS_Media_List {
  struct GTEXT_CSS_Media_List_Internal in;
};

/// The scroll-state features of a container condition (`stuck`, `snapped`, `scrollable`, `scrolled`) and the keywords of each.
#define CSS_CQ_SCROLL_FEATURES 4
extern const char * const css_cq_scroll_names[CSS_CQ_SCROLL_FEATURES];
extern const char * const * const css_cq_scroll_keywords[CSS_CQ_SCROLL_FEATURES];
extern const size_t css_cq_scroll_keyword_count[CSS_CQ_SCROLL_FEATURES];

/// The row of the generated table.
extern const css_mq_feature_row css_mq_feature_rows[CSS_MQF_COUNT];
extern const char * const css_mq_keywords[CSS_MQ_KEYWORD_COUNT];
extern const css_mq_type_row css_mq_type_rows[];
extern const size_t css_mq_type_row_count;

/// The row of a (lower case) name, or -1.
int css_mq_find_feature(const char * name, size_t length);
/// The index of a keyword of a feature (ASCII case-insensitive), or -1.
int css_mq_find_keyword(int feature, const char * word, size_t length);
/// Whether `word` is one of the reserved words of <media-type> (ASCII case-insensitive).
bool css_mq_is_reserved(const char * word, size_t length);

/// A growable array through the allocator of a list under construction.
bool css_mq_grow(const GTEXT_Allocator * a, void ** array, size_t * cap, size_t count, size_t need, size_t elem);

/// A container's box and font, for evaluating a size feature against it in place of the viewport.
typedef struct {
  double width, height;      ///< The content box in px.
  double font_size;          ///< The container's computed font size in px (what `em` is in a feature value).
  bool inline_horizontal;    ///< The container's inline axis is the horizontal one (a horizontal writing mode).
  /** The size of the query container of the container itself on an axis (a `cqw` in a feature value is relative to the container's own query container); false: none, the small viewport. */
  bool (*cq_size)(void * user, GTEXT_CSS_Container_Axis axis, double * px);
  void * cq_user;
} mq_container_view;

/// css_mq_eval_feature() with the numbers of a container in place of the viewport's. Ignores the `supported` set (a container feature is always answered).
GTEXT_CSS_Tristate css_mq_eval_feature_view(const GTEXT_CSS_Media_Environment * e, const mq_container_view * view, const struct GTEXT_CSS_Media_List_Internal * in, const mq_node * n,
    GTEXT_CSS_Status * status, const char ** detail);

/// Serialize one node tree (a query, or any node) to the sink, without recursion.
GTEXT_CSS_Status css_mq_serialize_node(const struct GTEXT_CSS_Media_List_Internal * in, uint32_t node, const GTEXT_Markup_Sink * sink);

/// Not <math.h>: src/css does its arithmetic through the math library only. These read the bits of a double.
static inline bool mq_isnan(double x) {
  return x != x;
}
static inline bool mq_isinf(double x) {
  return x == x && (x > DBL_MAX || x < -DBL_MAX);
}
static inline bool mq_signbit(double x) {
  uint64_t b;
  memcpy(&b, &x, sizeof b);
  return (b >> 63) != 0;
}
static inline double mq_inf(void) {
  volatile double m = DBL_MAX;
  return m * 2.0;
}

/// ASCII lower case of one byte.
static inline unsigned char css_mq_lower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

#ifdef __cplusplus
}
#endif

#endif
