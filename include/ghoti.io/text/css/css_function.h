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
 * @file css_function.h
 * @brief The function types of the drafts that are grammar-defined (images, gradients,
 *   transforms, easing, shapes, substitution and generated content), and the typed
 *   views of the ones that have a post-parse algorithm or a serialization of their own.
 *
 * Reference (Editor's Drafts served 2026-10-22): CSS Images 3 and 4, Transforms 1 and 2, Easing
 * Functions 1 and 2, Shapes 1, Masking 1, Values 4 and 5, Variables 1, Environment 1,
 * Lists 3, Counter Styles 3, Color 5 (the interpolation method of a gradient).
 *
 * ## Types in the engine
 *
 * Almost all of these functions are only a grammar: `<transform-function>`, `<basic-shape>`,
 * `<gradient>`, `<image>`, `<easing-function>` and the rest are Value Definition Syntax text, which
 * the engine of css_grammar.h matches into function values. The text is here, in
 * @ref GTEXT_CSS_Function_Types, a resolver for gtext_css_definition_link(): every type
 * and the two properties the shapes name (`fill-rule`, `border-radius`), looked up by the
 * name a link asks for (`image`, `linear-gradient()`, `'border-radius'`). A caller that has the
 * generated registry (a later phase) chains it first, so its grammars win. The native types of the
 * engine (`<color>`, `<length-percentage>`, `<number>` ...) are not here: they are code.
 *
 * ## Typed views
 *
 * What the draft does after parsing is code, and is given as a view of a matched value (the value
 * set stays the owner): `linear()` canonicalization and the range of `steps()` for an easing
 * function; the arguments of a transform function by name; the stops of a gradient, with their limit,
 * and the serialization that omits what is the default; the kind of a shape and its fill rule; the options of
 * `image-set()`; and what each substitution function needs (an element, the environment) before it
 * can be resolved.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_FUNCTION_H
#define GHOTI_IO_GTEXT_CSS_CSS_FUNCTION_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_grammar.h>
#include <ghoti.io/text/css/css_value.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the grammars -------------------------------------------------------- */

/** @brief The definitions of the function types (the struct is private). Immutable once made; its resolver is thread-safe only for one link at a time. */
typedef struct GTEXT_CSS_Function_Types GTEXT_CSS_Function_Types;

/** @brief The allocator and chaining of a @ref GTEXT_CSS_Function_Types. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;        ///< NULL for the default.
  GTEXT_CSS_Resolver fallback;              ///< Asked first (the generated registry), or NULL.
  void * fallback_user;                     ///< Passed to `fallback`.
  const GTEXT_CSS_Grammar_Options * grammar; ///< The options definitions are parsed under (NULL for the defaults).
} GTEXT_CSS_Function_Types_Options;

/**
 * @brief Make the definitions of the function types.
 *
 * Nothing is parsed until a type is asked for, and each is parsed once.
 *
 * @param options NULL for the defaults.
 * @param out Receives the types; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_function_types_new(const GTEXT_CSS_Function_Types_Options * options,
    GTEXT_CSS_Function_Types ** out);

/** @brief Free the types and every definition they made; NULL is ignored. Definitions handed to a link stay valid until this call. */
GTEXT_API void gtext_css_function_types_free(GTEXT_CSS_Function_Types * types);

/** @brief A GTEXT_CSS_Resolver: pass this function and the types as the `user` pointer to gtext_css_definition_link(). */
GTEXT_API const GTEXT_CSS_Definition * gtext_css_function_types_resolve(void * user, GTEXT_CSS_Ref_Kind kind,
    const char * name, size_t length);

/** @brief The number of types the library knows. */
GTEXT_API size_t gtext_css_function_type_count(void);

/**
 * @brief The `index`th type: its name, what it is and its grammar text.
 *
 * @return false when there is no such type; the pointers are static.
 */
GTEXT_API bool gtext_css_function_type_at(size_t index, const char ** name, GTEXT_CSS_Ref_Kind * kind, const char ** grammar);

/* ---- the families -------------------------------------------------------- */

/** @brief Which draft a function belongs to. */
typedef enum {
  GTEXT_CSS_FN_FAMILY_NONE = 0,
  GTEXT_CSS_FN_FAMILY_MATH,         ///< `calc()` and the others of css_math.h.
  GTEXT_CSS_FN_FAMILY_COLOR,        ///< `rgb()`, `color-mix()` ... (css_color.h).
  GTEXT_CSS_FN_FAMILY_IMAGE,        ///< `url()`, `src()`, `image()`, `image-set()`, `cross-fade()`, `element()`, `paint()`.
  GTEXT_CSS_FN_FAMILY_GRADIENT,     ///< The linear, radial and conic gradients and their repeating forms.
  GTEXT_CSS_FN_FAMILY_TRANSFORM,    ///< `matrix()` ... `perspective()`.
  GTEXT_CSS_FN_FAMILY_EASING,       ///< `linear()`, `cubic-bezier()`, `steps()`.
  GTEXT_CSS_FN_FAMILY_SHAPE,        ///< `inset()` ... `shape()`.
  GTEXT_CSS_FN_FAMILY_SUBSTITUTION  ///< `var()`, `env()`, `attr()`, `if()` ... and the generated-content functions.
} GTEXT_CSS_Function_Family;

/** @brief The family of a function name (ASCII case-insensitive); NONE for a name no draft here defines. */
GTEXT_API GTEXT_CSS_Function_Family gtext_css_function_family(const char * name, size_t length);

/* ---- easing -------------------------------------------------------------- */

/** @brief What an easing function is. */
typedef enum {
  GTEXT_CSS_EASING_LINEAR = 0,  ///< The keyword `linear`.
  GTEXT_CSS_EASING_LINEAR_POINTS, ///< `linear(...)`: `points`.
  GTEXT_CSS_EASING_CUBIC_BEZIER,  ///< A keyword (`ease` ...) or `cubic-bezier()`: `x1 y1 x2 y2`.
  GTEXT_CSS_EASING_STEPS          ///< `step-start`, `step-end`, `steps()`.
} GTEXT_CSS_Easing_Kind;

/** @brief The keyword an easing function was written as, if it was. */
typedef enum {
  GTEXT_CSS_EASING_KW_NONE = 0, ///< A function.
  GTEXT_CSS_EASING_KW_EASE, GTEXT_CSS_EASING_KW_EASE_IN, GTEXT_CSS_EASING_KW_EASE_OUT, GTEXT_CSS_EASING_KW_EASE_IN_OUT,
  GTEXT_CSS_EASING_KW_STEP_START, GTEXT_CSS_EASING_KW_STEP_END
} GTEXT_CSS_Easing_Keyword;

/** @brief The jump terms of a step easing function. */
typedef enum {
  GTEXT_CSS_STEP_JUMP_START = 0, ///< `jump-start` and `start`.
  GTEXT_CSS_STEP_JUMP_END,       ///< `jump-end` and `end`, the default.
  GTEXT_CSS_STEP_JUMP_NONE,
  GTEXT_CSS_STEP_JUMP_BOTH
} GTEXT_CSS_Step_Position;

/** @brief A control point of `linear()` after canonicalization (Easing 2, 2.1). */
typedef struct {
  double input;        ///< The input progress, 0 to 1 and not decreasing along the list.
  double output;       ///< The output progress.
  bool input_written;  ///< The input was written (serialization depends on it).
} GTEXT_CSS_Easing_Point;

/** @brief A typed easing function (the struct below; made by gtext_css_easing_from_value()). */
typedef struct GTEXT_CSS_Easing GTEXT_CSS_Easing;

/** @struct GTEXT_CSS_Easing @brief An easing function with its fixups applied. */
struct GTEXT_CSS_Easing {
  GTEXT_CSS_Easing_Kind kind;           ///< Which.
  GTEXT_CSS_Easing_Keyword keyword;     ///< The keyword it was written as, or NONE.
  double x1, y1, x2, y2;                ///< The control points of a cubic Bezier.
  int64_t steps;                        ///< The number of steps.
  GTEXT_CSS_Step_Position position;     ///< The jump term.
  bool position_written;                ///< The jump term was written.
  bool position_short;                  ///< It was written `start` or `end` (not `jump-start`, `jump-end`).
  GTEXT_CSS_Easing_Point * points;      ///< The control points of `linear()`, canonicalized (two inputs of one argument are two points).
  size_t point_count;                   ///< Their number.
  GTEXT_Allocator allocator_;           ///< Private.
};

/**
 * @brief Read an easing function from a value matched by `<easing-function>`.
 *
 * Applies the draft's parse-time checks and fixups: the integer of `steps()` is at least 1 (at least 2 with
 * `jump-none`), and the control points of `linear()` are canonicalized (the first input 0, the last 1, an input
 * below an earlier one raised to it, runs of missing inputs spaced evenly).
 *
 * @param value The matched value. Not NULL.
 * @param allocator NULL for the default.
 * @param valid Receives whether the function is valid (a `steps(0)` is not). Written on success. Not NULL.
 * @param out Receives the easing function when valid; release it with gtext_css_easing_free(). Written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID (not an easing value) or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_easing_from_value(const GTEXT_CSS_Value * value, const GTEXT_Allocator * allocator,
    bool * valid, GTEXT_CSS_Easing ** out);

/** @brief Free an easing function; NULL is ignored. */
GTEXT_API void gtext_css_easing_free(GTEXT_CSS_Easing * easing);

/** @brief Write an easing function as Easing 1 and 2 serialize it (the keywords as themselves, `steps(n)` without a `jump-end`, `linear()` by its points). */
GTEXT_API GTEXT_CSS_Status gtext_css_easing_serialize_buffer(const GTEXT_CSS_Easing * easing, const GTEXT_Allocator * allocator,
    char ** out, size_t * out_length);

/* ---- transforms ---------------------------------------------------------- */

/** @brief Which transform function. */
typedef enum {
  GTEXT_CSS_TRANSFORM_MATRIX = 0, GTEXT_CSS_TRANSFORM_TRANSLATE, GTEXT_CSS_TRANSFORM_TRANSLATE_X, GTEXT_CSS_TRANSFORM_TRANSLATE_Y,
  GTEXT_CSS_TRANSFORM_TRANSLATE_Z, GTEXT_CSS_TRANSFORM_TRANSLATE_3D, GTEXT_CSS_TRANSFORM_SCALE, GTEXT_CSS_TRANSFORM_SCALE_X,
  GTEXT_CSS_TRANSFORM_SCALE_Y, GTEXT_CSS_TRANSFORM_SCALE_Z, GTEXT_CSS_TRANSFORM_SCALE_3D, GTEXT_CSS_TRANSFORM_ROTATE,
  GTEXT_CSS_TRANSFORM_ROTATE_X, GTEXT_CSS_TRANSFORM_ROTATE_Y, GTEXT_CSS_TRANSFORM_ROTATE_Z, GTEXT_CSS_TRANSFORM_ROTATE_3D,
  GTEXT_CSS_TRANSFORM_SKEW, GTEXT_CSS_TRANSFORM_SKEW_X, GTEXT_CSS_TRANSFORM_SKEW_Y, GTEXT_CSS_TRANSFORM_MATRIX_3D,
  GTEXT_CSS_TRANSFORM_PERSPECTIVE, GTEXT_CSS_TRANSFORM_COUNT
} GTEXT_CSS_Transform_Op;

/** @brief What an argument of a transform function is. */
typedef enum {
  GTEXT_CSS_TARG_NUMBER = 0, ///< A number.
  GTEXT_CSS_TARG_PERCENT,    ///< A percentage (`value` percent).
  GTEXT_CSS_TARG_LENGTH,     ///< A length: `value` in `unit`.
  GTEXT_CSS_TARG_ANGLE,      ///< An angle, in degrees (a unitless zero is 0).
  GTEXT_CSS_TARG_NONE,       ///< The keyword `none` (`perspective(none)`).
  GTEXT_CSS_TARG_MATH        ///< A math function: `source`.
} GTEXT_CSS_Transform_Arg_Kind;

/** @brief An argument of a transform function. */
typedef struct {
  GTEXT_CSS_Transform_Arg_Kind kind;
  double value;
  GTEXT_CSS_Unit unit;
  const GTEXT_CSS_Value * source; ///< The value it was read from (owned by the value set).
} GTEXT_CSS_Transform_Arg;

/** @brief A transform function. */
typedef struct {
  GTEXT_CSS_Transform_Op op;
  size_t arg_count;
  GTEXT_CSS_Transform_Arg args[16];
} GTEXT_CSS_Transform_Function;

/**
 * @brief Read a transform function from a value matched by `<transform-function>`.
 *
 * @param value The function value. Not NULL.
 * @param out Receives it. Not NULL.
 * @return true; false when the value is not a transform function.
 */
GTEXT_API bool gtext_css_transform_function_from_value(const GTEXT_CSS_Value * value, GTEXT_CSS_Transform_Function * out);

/** @brief The name of a transform function as it is written (`translateX`, `matrix3d`). Static; NULL for a value that is not one. */
GTEXT_API const char * gtext_css_transform_op_name(GTEXT_CSS_Transform_Op op);

/* ---- gradients ----------------------------------------------------------- */

/** @brief The kind of a gradient. */
typedef enum {
  GTEXT_CSS_GRADIENT_LINEAR = 0,
  GTEXT_CSS_GRADIENT_RADIAL,
  GTEXT_CSS_GRADIENT_CONIC
} GTEXT_CSS_Gradient_Kind;

/** @brief A color stop or a color hint of a gradient. */
typedef struct {
  const GTEXT_CSS_Value * color;      ///< The stop's color (a VAL_COLOR value); NULL for a hint.
  const GTEXT_CSS_Value * position;   ///< The first position (a length, an angle, a percentage, a zero or a math function), or NULL. A hint's only position.
  const GTEXT_CSS_Value * position2;  ///< The second position of a double stop, or NULL.
} GTEXT_CSS_Gradient_Stop;

/** @brief A gradient read from a matched value. */
typedef struct {
  GTEXT_CSS_Gradient_Kind kind;         ///< Linear, radial or conic.
  bool repeating;                       ///< A `repeating-` form.
  GTEXT_CSS_Gradient_Stop * stops;      ///< The stops and hints in order.
  size_t stop_count;                    ///< Their number (hints included).
  const GTEXT_CSS_Value * value;        ///< The function value.
  GTEXT_Allocator allocator_;           ///< Private.
} GTEXT_CSS_Gradient;

/**
 * @brief Read a gradient from a value matched by `<gradient>`.
 *
 * @param value The function value. Not NULL.
 * @param max_stops The most stops and hints; 0 for 65,536. One more is GTEXT_CSS_E_LIMIT_STOPS.
 * @param allocator NULL for the default.
 * @param out Receives the gradient; release with gtext_css_gradient_free(). Written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (not a gradient); GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_STOPS.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_gradient_from_value(const GTEXT_CSS_Value * value, size_t max_stops,
    const GTEXT_Allocator * allocator, GTEXT_CSS_Gradient ** out);

/** @brief Free a gradient; NULL is ignored. */
GTEXT_API void gtext_css_gradient_free(GTEXT_CSS_Gradient * gradient);

/**
 * @brief Write a gradient as Images 4, section 8 asks: in the order of its grammar, "omitting components when
 *   possible without changing the meaning" (the default direction `to bottom`, a first position of 0%, a last of 100%).
 *
 * @param gradient The gradient. Not NULL.
 * @param allocator NULL for the default.
 * @param out Receives the text (NUL-terminated); release with gtext_css_serialize_buffer_free() given the same allocator.
 * @param out_length Receives its length.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_gradient_serialize_buffer(const GTEXT_CSS_Gradient * gradient, const GTEXT_Allocator * allocator,
    char ** out, size_t * out_length);

/* ---- images -------------------------------------------------------------- */

/** @brief What an `<image>` value is. */
typedef enum {
  GTEXT_CSS_IMAGE_NONE = 0,
  GTEXT_CSS_IMAGE_URL,        ///< `url()` or `src()`.
  GTEXT_CSS_IMAGE_IMAGE,      ///< `image(<color>)`.
  GTEXT_CSS_IMAGE_IMAGE_SET,  ///< `image-set()` or `-webkit-image-set()`.
  GTEXT_CSS_IMAGE_CROSS_FADE,
  GTEXT_CSS_IMAGE_ELEMENT,
  GTEXT_CSS_IMAGE_PAINT,
  GTEXT_CSS_IMAGE_GRADIENT
} GTEXT_CSS_Image_Kind;

/** @brief The kind of an image value. */
GTEXT_API GTEXT_CSS_Image_Kind gtext_css_image_kind(const GTEXT_CSS_Value * value);

/** @brief An option of an `image-set()`. */
typedef struct {
  const GTEXT_CSS_Value * image;   ///< The image, or the string that stands for a url.
  double resolution;               ///< In dppx (1 when not written; Images 4: "it behaves as 1x").
  bool resolution_written;
  const GTEXT_CSS_Value * type;    ///< The `type()` string value, or NULL.
} GTEXT_CSS_Image_Set_Option;

/**
 * @brief The options of an `image-set()` value, in order (an array of `*count`; release with gtext_allocator_free()).
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (not an image-set); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_image_set_options(const GTEXT_CSS_Value * value, const GTEXT_Allocator * allocator,
    GTEXT_CSS_Image_Set_Option ** options, size_t * count);

/* ---- shapes -------------------------------------------------------------- */

/** @brief The kind of a basic shape. */
typedef enum {
  GTEXT_CSS_SHAPE_NONE = 0,
  GTEXT_CSS_SHAPE_INSET, GTEXT_CSS_SHAPE_XYWH, GTEXT_CSS_SHAPE_RECT, GTEXT_CSS_SHAPE_CIRCLE, GTEXT_CSS_SHAPE_ELLIPSE,
  GTEXT_CSS_SHAPE_POLYGON, GTEXT_CSS_SHAPE_PATH, GTEXT_CSS_SHAPE_SHAPE
} GTEXT_CSS_Shape_Kind;

/** @brief A basic shape function, read from a matched value. */
typedef struct {
  GTEXT_CSS_Shape_Kind kind;
  bool even_odd;                       ///< `fill-rule: evenodd` was written (`polygon()`, `path()`, `shape()`).
  bool fill_rule_written;
  bool has_round;                      ///< `round` was written (`inset()`, `xywh()`, `rect()`, `polygon()`).
  const GTEXT_CSS_Value * path;        ///< `path()`: the string.
} GTEXT_CSS_Shape;

/** @brief Read a shape from a value matched by `<basic-shape>`; false when it is not one. */
GTEXT_API bool gtext_css_shape_from_value(const GTEXT_CSS_Value * value, GTEXT_CSS_Shape * out);

/* ---- substitution and generated content ----------------------------------- */

/** @brief Which function of the substitution family. */
typedef enum {
  GTEXT_CSS_SUBST_NONE = 0,
  GTEXT_CSS_SUBST_VAR, GTEXT_CSS_SUBST_ENV, GTEXT_CSS_SUBST_ATTR, GTEXT_CSS_SUBST_IF, GTEXT_CSS_SUBST_FIRST_VALID,
  GTEXT_CSS_SUBST_RANDOM, GTEXT_CSS_SUBST_RANDOM_ITEM, GTEXT_CSS_SUBST_SIBLING_INDEX, GTEXT_CSS_SUBST_SIBLING_COUNT,
  GTEXT_CSS_SUBST_PROGRESS, GTEXT_CSS_SUBST_INHERIT, GTEXT_CSS_SUBST_IDENT, GTEXT_CSS_SUBST_STRING,
  GTEXT_CSS_SUBST_COUNTER, GTEXT_CSS_SUBST_COUNTERS, GTEXT_CSS_SUBST_CALC_SIZE
} GTEXT_CSS_Substitution_Kind;

/** @brief What a substitution function needs before it has a value. */
typedef enum {
  GTEXT_CSS_NEEDS_NOTHING = 0,
  GTEXT_CSS_NEEDS_VARIABLES,   ///< The custom properties of the element.
  GTEXT_CSS_NEEDS_ENVIRONMENT, ///< The user agent's environment variables.
  GTEXT_CSS_NEEDS_ELEMENT,     ///< The element: its attributes, its place among its siblings, its counters, its style.
  GTEXT_CSS_NEEDS_LAYOUT       ///< Layout (the intrinsic size `calc-size()` computes with).
} GTEXT_CSS_Needs;

/** @brief The substitution function of a name, or NONE. */
GTEXT_API GTEXT_CSS_Substitution_Kind gtext_css_substitution_kind(const char * name, size_t length);

/** @brief What the function needs before it can be resolved (C2's business; stays unresolved until then). */
GTEXT_API GTEXT_CSS_Needs gtext_css_substitution_needs(GTEXT_CSS_Substitution_Kind kind);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_FUNCTION_H
