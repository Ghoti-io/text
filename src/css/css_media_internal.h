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
  MQN_CUSTOM     ///< `(--name)`: a reference to a `@custom-media`.
} mq_node_kind;

/// Flags of a query node.
#define MQF_NEGATED 0x1u  ///< `not` before the type.
#define MQF_ONLY 0x2u     ///< `only` before the type.
#define MQF_HAS_TYPE 0x4u ///< A media type was written.
#define MQF_REPLACED 0x8u ///< Invalid: replaced by `not all` (written as a negated `all`).

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
  uint8_t flags;          ///< QUERY: MQF_*.
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
  const GTEXT_CSS_Component_Value * src; ///< ENCLOSED while parsing: the block or function it stands for (not valid after the parse).
} mq_node;

struct GTEXT_CSS_Media_List_Internal {
  GCU_Atomic_Int refs;
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
