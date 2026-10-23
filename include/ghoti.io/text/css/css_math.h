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
 * @file css_math.h
 * @brief CSS math functions: the calculation tree, its type system, its
 *   simplification and its serialization.
 *
 * Reference: CSS Values and Units Module Level 4, Editor's Draft of 24 August 2026
 * (served 2026-10-22), section 10 (Mathematical Expressions), and Level 5 for the
 * unresolved numeric functions that may stand in a calculation.
 *
 * ## What a calculation is
 *
 * `calc() min() max() clamp() round() mod() rem() sin() cos() tan() asin() acos()
 * atan() atan2() pow() sqrt() hypot() log() exp() abs() sign()` and the constants
 * `e pi infinity -infinity NaN` parse into an immutable tree of
 * @ref GTEXT_CSS_Math_Node. Each node has a type in the draft's sense: a map from
 * the base types (length, angle, time, frequency, resolution, flex and percent) to
 * integer exponents, and a percent hint. A node whose type cannot be formed (a
 * length added to a time, `sin()` of a length) makes the whole function invalid:
 * gtext_css_math_parse() reports `*valid == false` and makes nothing, and a
 * grammar match of such a function is "no match". A function that is valid is
 * simplified as it is built, by the draft's algorithm (10.10.1): absolute units are
 * converted to their canonical unit, sums combine terms of one unit, products
 * of numbers and one dimension fold, `min()` and `max()` combine what they can compare,
 * and every function whose arguments are all numbers or absolute quantities is
 * computed.
 *
 * ## Arithmetic
 *
 * All arithmetic is IEEE double, and every function of the C library's `<math.h>`
 * is replaced by the `math` library's (`gmath_sin`, `gmath_atan2` ...), so a result
 * is the same on every host. `asin()` and `acos()` are `atan2` of the argument
 * and `sqrt((1-x)(1+x))`, which is accurate to a few units in the last place (the
 * `math` library has no correctly rounded one yet). `mod()` and `rem()` use an exact
 * remainder (a shift-and-subtract over `gmath_frexp` and `gmath_scalbn`). A result that
 * is not finite, or that is out of the range a property allows, is **kept** by the
 * calculation and clamped only when a value is computed (gtext_css_math_clamp()),
 * as the draft says (10.12, 10.9.2).
 *
 * ## Limits
 *
 * Nesting deeper than @ref GTEXT_CSS_Math_Options::max_depth is
 * GTEXT_CSS_E_LIMIT_DEPTH, with the place in @ref GTEXT_CSS_Math_Result; more
 * nodes than `max_nodes` is GTEXT_CSS_E_LIMIT_MATH_NODES. Nothing recurses.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_MATH_H
#define GHOTI_IO_GTEXT_CSS_CSS_MATH_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_serialize.h>
#include <ghoti.io/text/css/css_value.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The base types of the draft's type system (the index into
 *  GTEXT_CSS_Math_Type::exponent). */
typedef enum {
  GTEXT_CSS_MATH_BASE_LENGTH = 0, ///< `length`
  GTEXT_CSS_MATH_BASE_ANGLE,      ///< `angle`
  GTEXT_CSS_MATH_BASE_TIME,       ///< `time`
  GTEXT_CSS_MATH_BASE_FREQUENCY,  ///< `frequency`
  GTEXT_CSS_MATH_BASE_RESOLUTION, ///< `resolution`
  GTEXT_CSS_MATH_BASE_FLEX,       ///< `flex`
  GTEXT_CSS_MATH_BASE_PERCENT,    ///< `percent`
  GTEXT_CSS_MATH_BASE_COUNT       ///< The number of base types; not one.
} GTEXT_CSS_Math_Base;

/**
 * @struct GTEXT_CSS_Math_Type
 * @brief The type of a calculation (Values 4, 10.9): exponents of the base
 *   types, and a percent hint.
 *
 * `{}` (every exponent 0) is a `<number>`; `{length: 1}` a `<length>`;
 * `{percent: 1}` a `<percentage>`; `{length: 2}` an area, which no property
 * accepts (a function of that type is invalid) but which an intermediate result may have.
 */
typedef struct {
  int8_t exponent[GTEXT_CSS_MATH_BASE_COUNT]; ///< Indexed by @ref GTEXT_CSS_Math_Base.
  uint8_t percent_hint; ///< 0 for null, otherwise a GTEXT_CSS_Math_Base plus one.
} GTEXT_CSS_Math_Type;

/** @brief What a function resolves to, from its type (Values 4, 10.9). */
typedef enum {
  GTEXT_CSS_MATH_CAT_NONE = 0, ///< No type a property can take.
  GTEXT_CSS_MATH_CAT_NUMBER,
  GTEXT_CSS_MATH_CAT_PERCENTAGE,
  GTEXT_CSS_MATH_CAT_LENGTH,
  GTEXT_CSS_MATH_CAT_ANGLE,
  GTEXT_CSS_MATH_CAT_TIME,
  GTEXT_CSS_MATH_CAT_FREQUENCY,
  GTEXT_CSS_MATH_CAT_RESOLUTION,
  GTEXT_CSS_MATH_CAT_FLEX
} GTEXT_CSS_Math_Category;

/** @brief What a node of the calculation tree is. */
typedef enum {
  GTEXT_CSS_MATH_NUMBER = 0, ///< A leaf: `value`, no unit.
  GTEXT_CSS_MATH_DIMENSION,  ///< A leaf: `value` in `unit` (an absolute unit is already in its canonical one).
  GTEXT_CSS_MATH_PERCENT,    ///< A leaf: `value` percent.
  GTEXT_CSS_MATH_SUM,        ///< `kids`, added.
  GTEXT_CSS_MATH_PRODUCT,    ///< `kids`, multiplied.
  GTEXT_CSS_MATH_NEGATE,     ///< The one kid, negated.
  GTEXT_CSS_MATH_INVERT,     ///< One over the one kid.
  GTEXT_CSS_MATH_MIN,        ///< `min(kids)`
  GTEXT_CSS_MATH_MAX,        ///< `max(kids)`
  GTEXT_CSS_MATH_CLAMP,      ///< `clamp(minimum, value, maximum)`: three kids (a `none` side has become `min()` or `max()`).
  GTEXT_CSS_MATH_ROUND,      ///< `round(strategy, A, B)`: `round` the strategy, one kid (B omitted) or two.
  GTEXT_CSS_MATH_MOD,        ///< `mod(A, B)`
  GTEXT_CSS_MATH_REM,        ///< `rem(A, B)`
  GTEXT_CSS_MATH_SIN,        ///< `sin(A)`
  GTEXT_CSS_MATH_COS,        ///< `cos(A)`
  GTEXT_CSS_MATH_TAN,        ///< `tan(A)`
  GTEXT_CSS_MATH_ASIN,       ///< `asin(A)`
  GTEXT_CSS_MATH_ACOS,       ///< `acos(A)`
  GTEXT_CSS_MATH_ATAN,       ///< `atan(A)`
  GTEXT_CSS_MATH_ATAN2,      ///< `atan2(A, B)`
  GTEXT_CSS_MATH_POW,        ///< `pow(A, B)`
  GTEXT_CSS_MATH_SQRT,       ///< `sqrt(A)`
  GTEXT_CSS_MATH_HYPOT,      ///< `hypot(kids)`
  GTEXT_CSS_MATH_LOG,        ///< `log(A)` or `log(A, B)`
  GTEXT_CSS_MATH_EXP,        ///< `exp(A)`
  GTEXT_CSS_MATH_ABS,        ///< `abs(A)`
  GTEXT_CSS_MATH_SIGN,       ///< `sign(A)`
  GTEXT_CSS_MATH_PROGRESS,   ///< `progress(value, start, end)` (Values 5): `no_clamp` is `no-clamp`.
  GTEXT_CSS_MATH_RANDOM,     ///< `random(key?, min, max, step?)` (Values 5): not computed here; `text` is the key as written.
  GTEXT_CSS_MATH_SYMBOL,     ///< A keyword the context defined (a color channel): `text`, no value yet.
  GTEXT_CSS_MATH_UNRESOLVED, ///< A numeric function that is not a math function (`sibling-index()`): `text` is its serialization.
  GTEXT_CSS_MATH_OP_COUNT    ///< The number of ops; not one.
} GTEXT_CSS_Math_Op;

/** @brief The strategy of `round()`. */
typedef enum {
  GTEXT_CSS_MATH_ROUND_NEAREST = 0, ///< `nearest`, the default
  GTEXT_CSS_MATH_ROUND_UP,          ///< `up`
  GTEXT_CSS_MATH_ROUND_DOWN,        ///< `down`
  GTEXT_CSS_MATH_ROUND_TO_ZERO,     ///< `to-zero`
  GTEXT_CSS_MATH_ROUND_LINE_WIDTH   ///< `line-width`
} GTEXT_CSS_Math_Round;

/** @brief A node (the struct below). */
typedef struct GTEXT_CSS_Math_Node GTEXT_CSS_Math_Node;

/**
 * @struct GTEXT_CSS_Math_Node
 * @brief A node of a calculation tree: immutable, owned by the @ref GTEXT_CSS_Math
 *   (or the value set) that holds it.
 */
struct GTEXT_CSS_Math_Node {
  GTEXT_CSS_Math_Op op;                      ///< What it is.
  GTEXT_CSS_Math_Round round;                ///< ROUND: the strategy (written or the default).
  bool strategy_written;                     ///< ROUND: the strategy was written (it is serialized only then).
  bool no_clamp;                             ///< PROGRESS: `no-clamp` was written.
  GTEXT_CSS_Unit unit;                       ///< DIMENSION: the unit.
  double value;                              ///< The leaves' number.
  GTEXT_CSS_Math_Type type;                  ///< This node's type.
  const GTEXT_CSS_Math_Node * const * kids;  ///< The operands.
  size_t kid_count;                          ///< Their number.
  GTEXT_CSS_String text;                     ///< SYMBOL and UNRESOLVED; RANDOM: the key as written (empty when there is none).
  GTEXT_CSS_Span span;                       ///< The source of a leaf or a function (the zero span for a made node).
};

/**
 * @struct GTEXT_CSS_Math
 * @brief A parsed math function: the tree and what it resolves to.
 */
struct GTEXT_CSS_Math {
  const GTEXT_CSS_Math_Node * root;  ///< The simplified tree.
  GTEXT_CSS_Math_Type type;          ///< Its type (the root's).
  GTEXT_CSS_Math_Category category;  ///< What it resolves to.
  bool has_percent;                  ///< A percentage was written somewhere in it, even one that simplified away: a slot that does not resolve percentages refuses it.
  bool has_symbols;                  ///< It contains a context keyword or an unresolved function, so it is not a number yet.
  size_t node_count;                 ///< Nodes made, those simplified away included.
  void * owner_;                     ///< Private.
};

/** @brief A keyword a context makes usable in a calculation (Values 4, 10.7.3). */
typedef struct {
  const char * name;                 ///< ASCII lower case, NUL-terminated (`r`, `alpha`).
  GTEXT_CSS_Math_Type type;          ///< Its type (a `<number>` is the zero type).
} GTEXT_CSS_Math_Keyword;

/** @brief What a calculation is read in. A zeroed struct is the default: no extra keywords. */
typedef struct {
  const GTEXT_CSS_Math_Keyword * keywords; ///< The extra keywords, or NULL.
  size_t keyword_count;                    ///< Their number.
} GTEXT_CSS_Math_Context;

/** @brief The allocator and limits of reading a calculation. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator; ///< NULL for the default.
  size_t max_depth;                  ///< Nesting of functions and parentheses. Default 256; at most 65,536. One more is GTEXT_CSS_E_LIMIT_DEPTH.
  size_t max_nodes;                  ///< Nodes made for one function. Default 65,536; at most 2^24. One more is GTEXT_CSS_E_LIMIT_MATH_NODES.
} GTEXT_CSS_Math_Options;

/** @brief What reading a calculation reports. Plain data. */
typedef struct {
  GTEXT_CSS_Status status; ///< What the call returned.
  const char * message;    ///< A static description.
  GTEXT_CSS_Span where;    ///< A limit: the component value where it was reached.
  size_t nodes;            ///< Nodes made.
  size_t work;             ///< Units of work done (component values looked at, nodes made and examined).
} GTEXT_CSS_Math_Result;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Math_Options gtext_css_math_options_default(void);

/** @brief Whether a function name is a math function of Values 4 (ASCII case-insensitive). */
GTEXT_API bool gtext_css_math_is_function(const char * name, size_t length);

/**
 * @brief Read a math function.
 *
 * @param function A function component value: `calc(`, `min(` ... Not NULL.
 * @param context NULL for none.
 * @param options NULL for the defaults.
 * @param valid Receives whether it is a valid math function (its syntax and its types).
 *   Written on success.
 * @param out Receives the calculation when it is valid; NULL otherwise. Written only on success. Not NULL.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK (valid or not); GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM;
 *   GTEXT_CSS_E_LIMIT_DEPTH; GTEXT_CSS_E_LIMIT_MATH_NODES.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_math_parse(const GTEXT_CSS_Component_Value * function,
    const GTEXT_CSS_Math_Context * context, const GTEXT_CSS_Math_Options * options,
    bool * valid, GTEXT_CSS_Math ** out, GTEXT_CSS_Math_Result * result);

/** @brief Free a calculation made by gtext_css_math_parse(); NULL is ignored. A
 *  calculation held by a value set is freed with the set. */
GTEXT_API void gtext_css_math_free(GTEXT_CSS_Math * math);

/**
 * @brief Write a calculation as a specified value is serialized (Values 4, 10.13).
 *
 * `calc(1px + 2px)` is `calc(3px)`, `calc(20px + 2em)` is `calc(2em + 20px)`, an
 * infinite or NaN result is `calc(infinity * 1px)`, a root that is a function is
 * written as that function (`min(20px, 10%)`).
 *
 * @param math The calculation. Not NULL.
 * @param options NULL for the defaults.
 * @param sink Where the text goes. Not NULL, with a `write`.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_WRITE; GTEXT_CSS_E_LIMIT_OUTPUT.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_math_serialize(const GTEXT_CSS_Math * math,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink);

/** @brief gtext_css_math_serialize() to a new buffer (NUL-terminated; release with gtext_css_serialize_buffer_free()). */
GTEXT_API GTEXT_CSS_Status gtext_css_math_serialize_buffer(const GTEXT_CSS_Math * math,
    const GTEXT_CSS_Serialize_Options * options, char ** out, size_t * out_length);

/**
 * @brief Whether the whole calculation is one number, and which.
 *
 * True when the simplified root is a leaf: a number, a dimension in a unit that has
 * no environment to resolve (a canonical absolute unit, or a relative one), or a percentage.
 *
 * @param math The calculation.
 * @param value Receives its value (may be infinite or NaN; -0 is kept). Not NULL.
 * @param unit Receives the unit (GTEXT_CSS_UNIT_NONE for a number or a percentage). NULL if not wanted.
 * @param percent Receives whether it is a percentage. NULL if not wanted.
 * @return Whether the root is a leaf; false leaves the outputs alone.
 */
GTEXT_API bool gtext_css_math_numeric(const GTEXT_CSS_Math * math, double * value,
    GTEXT_CSS_Unit * unit, bool * percent);

/** @brief The environment a calculation is resolved in. A zeroed struct resolves nothing that is relative. */
typedef struct {
  /** The size of one unit of a relative unit, in the canonical unit of its type (px for a length), or false for
   *  one this environment does not know. NULL knows none. */
  bool (*unit_size)(void * user, GTEXT_CSS_Unit unit, double * canonical);
  /** The value of a context keyword, or false. NULL knows none. */
  bool (*symbol)(void * user, const char * name, double * value);
  void * user;               ///< Passed to the callbacks.
  bool has_percent_basis;    ///< The percentage resolves against `percent_basis`.
  double percent_basis;      ///< In the canonical unit of the calculation's type.
} GTEXT_CSS_Math_Env;

/**
 * @brief Compute a calculation in an environment (Values 4, 10.11): the value of the whole in the canonical unit of its type.
 *
 * Nothing is clamped: the result may be infinite or NaN (gtext_css_math_clamp() censors it).
 *
 * @param math The calculation.
 * @param env NULL, or the environment.
 * @param allocator NULL for the default; used for the walk's stack.
 * @param value Receives the value; written only when `*resolved`. Not NULL.
 * @param resolved Receives whether everything in the tree could be resolved. Written on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_math_resolve(const GTEXT_CSS_Math * math, const GTEXT_CSS_Math_Env * env,
    const GTEXT_Allocator * allocator, double * value, bool * resolved);

/**
 * @brief The range check of a top-level calculation (Values 4, 10.9.2, 10.12): NaN becomes 0, an infinity
 *   becomes the nearest bound, and a value out of [lo, hi] the nearer bound.
 *
 * Signed zeros are not kept (a result of -0 is 0).
 */
GTEXT_API double gtext_css_math_clamp(double value, double lo, double hi);

/** @brief The type of a category (a number is `{}`). */
GTEXT_API GTEXT_CSS_Math_Type gtext_css_math_category_type(GTEXT_CSS_Math_Category category);

/** @brief The name of an op ("sum", "min", "sin" ...). Static; "unknown" for a value that is not one. */
GTEXT_API const char * gtext_css_math_op_name(GTEXT_CSS_Math_Op op);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_MATH_H
