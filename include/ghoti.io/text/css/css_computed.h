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
 * @file css_computed.h
 * @brief Computed values: from the declarations that won the cascade, the parent's style and the environment, to the computed value of
 *   every property of an element.
 *
 * Reference: CSS Cascade Level 5 ("Resolving the CSS-wide keywords", "Defaulting"), CSS Values and Units Level 4 and 5 (computed values of
 * each value type, section 6 and 10), CSS Writing Modes 4, CSS Logical Properties 1, CSS Fonts 4, CSS Color 4 and 5, and the specification
 * of each property that the registry's `computedValue` sentence points at (every sentence of the pin is classified or listed in
 * `tools/css/computed_rules.tsv`, with the sentence quoted).
 *
 * ## What is computed from what
 *
 * gtext_css_compute_style() takes the winners of the cascade for one element (a @ref GTEXT_CSS_Declaration_Block in ascending priority:
 * C2b builds it; a style attribute or a presentation attribute is one the caller builds), the computed style of the element it inherits from
 * (the **inheritance parent**: not necessarily the tree parent, see css_element.h), and the environment, and makes the style:
 *
 * 1. Each property is the declared value, or if there is none, the parent's computed value when the property is inherited and its initial
 *    value when it is not. `initial`, `inherit` and `unset` (inherit if inherited, else initial) are applied here; `all` (every property except `direction`,
 *    `unicode-bidi` and the custom properties) is a declaration at its place in the block's order, so that a later declaration of a property
 *    beats it and an earlier one does not. `revert` and `revert-layer` are cascade results (C2b) and are refused as inputs. A **pending** value (one that holds `var()`, `env()`, `attr()`, `if()`,
 *    `inherit()`, `ident()`, `random-item()`, or is a `cycle()`) is substituted here, the custom properties first (css_substitute.h, css_custom.h): a value that
 *    comes to the guaranteed-invalid value, or to text that is not a value of the property, is invalid at computed-value time, which is `unset`. `env()`, `attr()`,
 *    `if()` and the tree counting functions ask the callbacks of the environment and of the element interface (css_element.h): GTEXT_CSS_E_UNSUPPORTED naming the
 *    callback when a value needs one that is not there. A registered custom property (css_registry.h, css_at_property.h) is computed by the rules of its syntax. The importance of a declaration (`!important`) is the cascade's and is
 *    not looked at: the block is in ascending priority, and the last declaration of a property is the one that counts.
 * 2. The order of computation is fixed (`tools/css/compute_order.tsv`): `writing-mode`, `direction`, `text-orientation`, the font properties,
 *    `font-size`, `line-height`, the border styles, `position`, `float` (none under an absolutely positioned box), `display`, and then every other property. A property reads only what is
 *    above it, or the parent's style.
 * 3. A logical property is mapped to its physical slot by the element's own computed flow (css_logical.h): of the declarations of a group that
 *    map to one slot, the last in the block wins ("cascaded together as one"), and every member of the group that maps to the slot has its value.
 *    Inheritance of a flow-relative property is from the property of the same name on the parent.
 * 4. The value is computed by its **class** (gtext_css_compute_class()): the specified value as it is (`identity`); by the types the value is made
 *    of (`by-type`): absolute lengths to `px`, font-relative lengths against the computed `font-size` of the element (`rem` against the root's), viewport
 *    lengths against the environment's, container lengths by the environment's callback, angles to `deg`, times to `s`, frequencies to `Hz`, resolutions
 *    to `dppx`, numbers and integers as they are, percentages kept, math functions simplified (and clamped to the range of their slot when they
 *    resolve to one number), `light-dark()` resolved from the color scheme, `currentcolor` kept as the keyword, relative URLs resolved against the base
 *    URL, lists item by item; or by the property's own **algorithm** (the font-size keywords, `bolder`, border widths under `none`, blockification
 *    of `display`, ...). A class `unimplemented` is GTEXT_CSS_E_UNSUPPORTED naming the property when a declaration needs it computed; never a guess.
 *
 * ## Values are shared
 *
 * A computed value is an immutable @ref GTEXT_CSS_Value_Set. A style holds one for every property of the registry; the ones it inherits or takes from
 * the initial values are the very sets of the parent and of the context (retained), and a value that needs no change is the declared set
 * itself. So a style costs one pointer a property plus what it computed, a long chain of inheritance shares what it inherits, and a value
 * outlives the style that made it for as long as anyone holds it (gtext_css_value_set_retain()).
 *
 * ## Colors
 *
 * `text` does not convert colors. A computed color is the declared color with `light-dark()` resolved, a math function in a channel simplified, and
 * `currentcolor` kept. A color that cannot be computed without conversion (a system color, `color-mix()`, a relative color, `contrast-color()`,
 * `alpha()`, `device-cmyk()`) is resolved to sRGB by the environment's color callback if there is one; with none it is returned as it was written,
 * flagged GTEXT_CSS_VFLAG_COLOR_UNRESOLVED, and never guessed.
 *
 * ## Limits and reentrancy
 *
 * The depth of an element chain (gtext_css_compute_element()) and the length of a list value are limits, each its own status. Nothing recurses
 * in the library's code; a context is immutable after it is made and may be shared by any number of threads, each computing its own styles; a style
 * is immutable and may be read by any number of threads.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_COMPUTED_H
#define GHOTI_IO_GTEXT_CSS_CSS_COMPUTED_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_at_property.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_custom.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_logical.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/css/css_shorthand.h>
#include <ghoti.io/text/css/css_substitute.h>
#include <ghoti.io/text/css/css_value.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How a property's computed value is made (tools/css/computed_phrases.tsv and computed_rules.tsv; the generator reads the enumerators
 *   from this header, in this order).
 */
typedef enum {
  GTEXT_CSS_COMPUTE_UNIMPLEMENTED = 0, ///< Not implemented: a declaration that needs it computed is GTEXT_CSS_E_UNSUPPORTED.
  GTEXT_CSS_COMPUTE_IDENTITY,          ///< The computed value is the specified value.
  GTEXT_CSS_COMPUTE_BY_TYPE,           ///< Each value type computes as Values 4 says (see the file's description).
  GTEXT_CSS_COMPUTE_ALGORITHM,         ///< The property's own algorithm (gtext_css_compute_class() says which).
  GTEXT_CSS_COMPUTE_SHORTHAND,         ///< A shorthand: it has no value, its longhands do.
  GTEXT_CSS_COMPUTE_ALIAS,             ///< A legacy name: it is its target.
  GTEXT_CSS_COMPUTE_CUSTOM,            ///< The custom property pattern `--*`: carried as the tokens that were declared.
  GTEXT_CSS_COMPUTE_CLASS_COUNT        ///< The number of classes; not one.
} GTEXT_CSS_Compute_Class;

/**
 * @brief The algorithms of the properties whose computed value the specification writes as prose (the enumerators are in the order of the rows of
 *   tools/css/computed_rules.tsv by name; the generator reads them from this header).
 */
typedef enum {
  GTEXT_CSS_ALGO_NONE = 0,
  GTEXT_CSS_ALGO_FONT_SIZE,        ///< `font-size`: the keywords, `larger`, `smaller`, a percentage or `em` against the parent's size.
  GTEXT_CSS_ALGO_FONT_WEIGHT,      ///< `font-weight`: `normal`, `bold`, `bolder`, `lighter` to a number.
  GTEXT_CSS_ALGO_FONT_WIDTH,       ///< `font-width`: the keywords to a percentage.
  GTEXT_CSS_ALGO_FONT_STYLE,       ///< `font-style`: `oblique` with its angle in degrees.
  GTEXT_CSS_ALGO_LINE_HEIGHT,      ///< `line-height`: a number kept, a length or percentage to px.
  GTEXT_CSS_ALGO_FONT_SIZE_ADJUST, ///< `font-size-adjust`: a lone number gets `ex-height`.
  GTEXT_CSS_ALGO_BORDER_WIDTH,     ///< Border and outline widths: keywords to px, 0 under `none` and `hidden`, snapped to device pixels.
  GTEXT_CSS_ALGO_RULE_WIDTH,       ///< `column-rule-width`, `row-rule-width`: a list of the above.
  GTEXT_CSS_ALGO_LINE_WIDTH,       ///< `<line-width>` keywords and numbers to px (`stroke-width`, `-webkit-text-stroke-width`).
  GTEXT_CSS_ALGO_DISPLAY,          ///< `display`: blockification under `float`, `position`, the root and a flex or grid parent (an `inline flow-root` and a layout-internal box become `block`).
  GTEXT_CSS_ALGO_FLOAT,            ///< `float`: `none` under `position: absolute` and `fixed`.
  GTEXT_CSS_ALGO_COLOR,            ///< `color`: `currentcolor` is `inherit`.
  GTEXT_CSS_ALGO_OPACITY,          ///< Opacities: a number or percentage clamped to 0 to 1, as a number.
  GTEXT_CSS_ALGO_ZOOM,             ///< `zoom`: a percentage as a number.
  GTEXT_CSS_ALGO_MATH_DEPTH,       ///< `math-depth`: `auto-add`, `add()`.
  GTEXT_CSS_ALGO_TEXT_ALIGN,       ///< `text-align-all`, `text-align-last`: `match-parent`.
  GTEXT_CSS_ALGO_VOICE_PITCH,      ///< `voice-pitch`, `voice-range`: a lone keyword is itself and a frequency with `absolute` is that frequency; an offset needs a table the environment lacks.
  GTEXT_CSS_ALGO_VOICE_BALANCE,    ///< `voice-balance`: a number between -100 and 100.
  GTEXT_CSS_ALGO_HYPHENATE_CHARS,  ///< `hyphenate-limit-chars`: always three values.
  GTEXT_CSS_ALGO_IMAGE_ORIENTATION, ///< `image-orientation`: the angle rounded to a quarter turn and normalized.
  GTEXT_CSS_ALGO_PAIR_DUP,         ///< `border-spacing`: one length is two.
  GTEXT_CSS_ALGO_BOX4,             ///< `border-image-outset`, `-slice`, `-width`: one to four values are four.
  GTEXT_CSS_ALGO_REPEAT,           ///< `background-repeat` and kin: `repeat-x` and one keyword are a pair.
  GTEXT_CSS_ALGO_POSITION,         ///< A `<position>`: two offsets from the top left, as lengths or percentages.
  GTEXT_CSS_ALGO_POSITION_AXIS,    ///< `background-position-x` and kin: an offset and its origin.
  GTEXT_CSS_ALGO_BG_SIZE,          ///< `background-size` and kin: one value is a pair, `auto` filled in.
  GTEXT_CSS_ALGO_TRANSLATE,        ///< `translate`: x, y and z.
  GTEXT_CSS_ALGO_SCALE,            ///< `scale`: x, y and z as numbers.
  GTEXT_CSS_ALGO_ROTATE,           ///< `rotate`: an axis and an angle.
  GTEXT_CSS_ALGO_SHADOW,           ///< `text-shadow`: each shadow with its four lengths and its color.
  GTEXT_CSS_ALGO_WRAP_FLOW,        ///< `wrap-flow`: `auto` on a float.
  GTEXT_CSS_ALGO_NUMBER_LENGTH,    ///< `stroke-dasharray`, `stroke-dashoffset`: numbers are lengths in px.
  GTEXT_CSS_ALGO_CUSTOM,           ///< `--*`.
  GTEXT_CSS_ALGO_COUNT             ///< The number of algorithms; not one.
} GTEXT_CSS_Compute_Algorithm;

/** @brief The name of a class ("identity", "by-type" ...). Static; "unknown" for a value that is not one. */
GTEXT_API const char * gtext_css_compute_class_name(GTEXT_CSS_Compute_Class klass);

/** @brief The name of an algorithm ("FONT_SIZE" ...). Static; "unknown" for a value that is not one. */
GTEXT_API const char * gtext_css_compute_algorithm_name(GTEXT_CSS_Compute_Algorithm algorithm);

/**
 * @brief How a property's computed value is made.
 *
 * @param registry The registry. Not NULL.
 * @param property A property of it.
 * @param algorithm Receives the algorithm (GTEXT_CSS_ALGO_NONE unless the class is ALGORITHM or CUSTOM); NULL if not wanted.
 * @param partial Receives whether the algorithm leaves something of the specification undone (the row says what); NULL if not wanted.
 * @return The class; GTEXT_CSS_COMPUTE_UNIMPLEMENTED for a property the registry lacks. A property the caller registered is BY_TYPE, or
 *   IDENTITY for GTEXT_CSS_COMPUTED_AS_SPECIFIED; a caller's override of a generated property has the class of the generated one.
 */
GTEXT_API GTEXT_CSS_Compute_Class gtext_css_compute_class(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property,
    GTEXT_CSS_Compute_Algorithm * algorithm, bool * partial);

/** @brief How many properties are computed before all others (the table of tools/css/compute_order.tsv). */
GTEXT_API size_t gtext_css_compute_order_count(void);

/** @brief The `index`th property of that table, by name; NULL past the end. Static. */
GTEXT_API const char * gtext_css_compute_order_at(size_t index);

/** @brief Whether a property must be computed (inherited, or an SVG 2 presentation attribute): never UNIMPLEMENTED. */
GTEXT_API bool gtext_css_compute_required(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property);

/* ---- the context ---------------------------------------------------------------------------------- */

/** @brief Everything computing needs besides the declarations: the registry, the environment and the initial values (the struct is private). */
typedef struct GTEXT_CSS_Compute_Context GTEXT_CSS_Compute_Context;

/** @brief A computed style: the computed value of every property of one element (the struct is private). */
typedef struct GTEXT_CSS_Computed_Style GTEXT_CSS_Computed_Style;

/** @brief The allocator and limits of computing. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator; ///< NULL for the registry's.
  size_t max_element_depth;          ///< Deepest chain of inheritance parents that gtext_css_compute_element() follows. Default 100,000; at most 2^24. One more is GTEXT_CSS_E_LIMIT_ELEMENTS.
  size_t max_list_items;             ///< Most items of any list a computation reads or makes. Default 65,536; at most 2^24. One more is GTEXT_CSS_E_LIMIT_LIST.
  size_t max_value_nodes;            ///< Most values one computed value may have. Default 1,048,576; at most 2^28. One more is GTEXT_CSS_E_LIMIT_DECLARATIONS.
  uint64_t custom_hash_seed;         ///< Keys the balance of the custom properties' map, which no choice of names an author makes may defeat. 0 (the default): a seed drawn from
                                     ///< the addresses of the context (unpredictable under address randomization). A fixed seed makes a run reproducible, not safer.
  GTEXT_CSS_Substitute_Options substitute; ///< The limits of substituting `var()` and the other functions (css_substitute.h). A zeroed struct is the default.
  const GTEXT_CSS_Element_Interface * elements; ///< The caller's tree, for the functions that read it (`attr()`, `sibling-index()`, `sibling-count()`, `style()`). Copied; its
                                     ///< `user` must outlive the context. NULL: a computation that needs one is GTEXT_CSS_E_UNSUPPORTED, naming the callback. (gtext_css_compute_element() is
                                     ///< given a table of its own and uses that.)
  const GTEXT_CSS_At_Property_Set * property_rules; ///< The registrations that `@property` rules made (css_at_property.h), or NULL. Borrowed: it must outlive the context and not change.
} GTEXT_CSS_Compute_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Compute_Options gtext_css_compute_options_default(void);

/** @brief What a compute call reports. Plain data; filled whatever happens. */
typedef struct {
  GTEXT_CSS_Status status;      ///< What the call returned.
  const char * message;         ///< A static description; never NULL after a call.
  GTEXT_CSS_Property_Id property; ///< The property being computed when it failed, or 0.
  char name[96];                ///< The thing concerned, cut to fit: for GTEXT_CSS_E_UNSUPPORTED the unit (`ex`, `cqw`, `lh`), the property or the callback that was needed.
  const char * detail;          ///< A static sentence saying which rule was broken, or NULL.
  size_t depth;                 ///< gtext_css_compute_element(): the depth of the element it was computing.
  size_t work;                  ///< Units of work done (properties computed, value nodes visited and made), for the quadratic suite.
  uint64_t substituted_tokens;  ///< The tokens that the values substituted for this element would have as text, summed (saturating): the counter the budget is held to. Not the time.
  size_t substitution_limited;  ///< How many values the budget (or the byte limit) made the guaranteed-invalid value.
  uint64_t substitution_peak;   ///< The most tokens any one value was found to have, refused ones included (at most twice the budget of a value: it stops there, not at what it would have grown to).
} GTEXT_CSS_Compute_Result;

/**
 * @brief Make a context.
 *
 * The initial value of every property is parsed and computed here once; a property whose computed initial value depends on the element (it is
 * `medium`, or has an `em`) is computed again for each style.
 *
 * @param registry The registry. Not NULL; it must outlive the context and every style.
 * @param env The environment, copied (a struct from an older header is read up to its `size`); NULL for gtext_css_environment_default(). Its
 *   strings and `user` must outlive the context.
 * @param options NULL for the defaults.
 * @param out Receives the context; written only on success. Not NULL.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, an environment that does not pass gtext_css_environment_check(), the result says which
 *   field); GTEXT_CSS_E_OOM; GTEXT_CSS_E_INTERNAL (the generated tables and the registry disagree).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_compute_context_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Environment * env,
    const GTEXT_CSS_Compute_Options * options, GTEXT_CSS_Compute_Context ** out, GTEXT_CSS_Compute_Result * result);

/** @brief Free a context; NULL is ignored. Every style made with it must be freed first. */
GTEXT_API void gtext_css_compute_context_free(GTEXT_CSS_Compute_Context * context);

/** @brief The registry of a context. */
GTEXT_API const GTEXT_CSS_Registry * gtext_css_compute_context_registry(const GTEXT_CSS_Compute_Context * context);

/* ---- computing a style ---------------------------------------------------------------------------- */

/**
 * @brief Compute the style of one element.
 *
 * @param context The context. Not NULL.
 * @param block The declarations that won the cascade, in ascending priority; NULL for none. Its registry must be the context's.
 * @param parent The computed style of the inheritance parent, or NULL for the root of inheritance (the document element): its inherited
 *   properties are initial, `inherit` is `initial`, and `rem` is its own `font-size`.
 * @param element The element, for the callbacks of the environment that take one (container sizes); NULL for none.
 * @param out Receives the style; written only on success. Free it with gtext_css_computed_free(). Not NULL.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument; a `revert` or a pending value in the block; the registries differ); GTEXT_CSS_E_UNSUPPORTED
 *   (a declared property is `unimplemented`, or a callback the environment lacks was needed: the result names it; no style is made); GTEXT_CSS_E_OOM;
 *   GTEXT_CSS_E_LIMIT_LIST; GTEXT_CSS_E_LIMIT_DECLARATIONS.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_compute_style(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Declaration_Block * block,
    const GTEXT_CSS_Computed_Style * parent, GTEXT_CSS_Element_Handle element, GTEXT_CSS_Computed_Style ** out,
    GTEXT_CSS_Compute_Result * result);

/** @brief Where gtext_css_compute_element() asks for the declarations of an element. */
typedef struct {
  void * user; ///< Passed to `declarations`.
  /** Gives the block of an element (borrowed: it must stay valid until the call returns) or NULL for none. Return anything but GTEXT_CSS_OK to stop. */
  GTEXT_CSS_Status (*declarations)(void * user, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_Declaration_Block ** block);
} GTEXT_CSS_Declaration_Source;

/**
 * @brief Compute the style of an element and, on the way, of every element it inherits from, from the root down.
 *
 * The chain of inheritance parents (css_element.h) is followed iteratively up to `max_element_depth`, and each style is computed from its
 * parent's, so the work is linear in the depth. Only the style of `element` is returned; the others are freed.
 *
 * @param context The context. Not NULL.
 * @param iface The element table; it needs `inheritance_parent` or `parent`. Not NULL.
 * @param source The declarations of an element. Not NULL.
 * @param element The element. Not NULL.
 * @param out Receives its style. Not NULL.
 * @param result NULL, or filled (its `depth` is where the failure was).
 * @return As gtext_css_compute_style(); GTEXT_CSS_E_UNSUPPORTED naming `parent` when the table has neither parent callback;
 *   GTEXT_CSS_E_LIMIT_ELEMENTS (the chain is deeper than `max_element_depth`); whatever `declarations` returned.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_compute_element(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Element_Interface * iface,
    const GTEXT_CSS_Declaration_Source * source, GTEXT_CSS_Element_Handle element, GTEXT_CSS_Computed_Style ** out,
    GTEXT_CSS_Compute_Result * result);

/**
 * @brief Let go of a style: the last owner frees it and releases its values (a value retained from it outlives it); NULL is ignored.
 * A style is reference counted (with atomic counts, so any thread may hold and release it); one a call returns has one owner.
 */
GTEXT_API void gtext_css_computed_free(GTEXT_CSS_Computed_Style * style);

/** @brief Another owner of the style, to be released with gtext_css_computed_free(). Thread-safe. NULL is returned as NULL. */
GTEXT_API GTEXT_CSS_Computed_Style * gtext_css_computed_retain(GTEXT_CSS_Computed_Style * style);

/**
 * @brief The computed value of a property.
 *
 * @param style The style. Not NULL.
 * @param property A property of the context's registry (an alias is followed; a shorthand has no value).
 * @param set Receives the value, owned by the style (retain it to keep it); NULL if not wanted. A property whose class is `unimplemented`
 *   and that no declaration touched has the value it inherits or its initial value if that exists.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (an id that is not a property, or a shorthand); GTEXT_CSS_E_UNSUPPORTED (the property has no value in
 *   this style: it is `unimplemented` and has no initial value).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_computed_value(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property,
    const GTEXT_CSS_Value_Set ** set);

/** @brief The root value of the computed value of a property (gtext_css_value_set_root() of the above); NULL where there is none. */
GTEXT_API const GTEXT_CSS_Value * gtext_css_computed_root(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property);

/** @brief The element's flow: its computed `writing-mode`, `direction` and `text-orientation`. */
GTEXT_API GTEXT_CSS_Flow gtext_css_computed_flow(const GTEXT_CSS_Computed_Style * style);

/** @brief How many custom properties the style has a value for. NULL is 0. */
GTEXT_API size_t gtext_css_computed_custom_count(const GTEXT_CSS_Computed_Style * style);

/**
 * @brief The `index`th custom property that has a value, in the order of their names.
 *
 * A custom property whose value is the guaranteed-invalid value is not counted and not returned (css_custom.h). A registered custom property
 * (css_registry.h, css_at_property.h) is, with its initial value when nothing sets it.
 *
 * @param style The style.
 * @param index Which one.
 * @param name Receives the name, owned by the style; NULL if not wanted.
 * @param value Receives its computed value, owned by the style (retain it to keep it); NULL if not wanted.
 * @return true, or false past the end.
 */
GTEXT_API bool gtext_css_computed_custom_at(const GTEXT_CSS_Computed_Style * style, size_t index, GTEXT_CSS_String * name,
    const GTEXT_CSS_Custom_Value ** value);

/**
 * @brief The computed value of a custom property, by name (case-sensitive).
 *
 * @param style The style. Not NULL.
 * @param name The name, `--` and the rest.
 * @param length Its length.
 * @param value Receives the value, owned by the style; NULL if not wanted.
 * @return true if the property has a value; false for the guaranteed-invalid value (an unset property, a cycle, a missing variable, a value too long).
 */
GTEXT_API bool gtext_css_computed_custom_find(const GTEXT_CSS_Computed_Style * style, const char * name, size_t length, const GTEXT_CSS_Custom_Value ** value);

/** @brief Write the computed value of a property as a value is serialized (css_value.h). @return As gtext_css_serialize_value(); GTEXT_CSS_E_UNSUPPORTED if there is no value. */
GTEXT_API GTEXT_CSS_Status gtext_css_computed_serialize(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink);

/** @brief The version of the text gtext_css_computed_dump() writes. It changes with any change of the format, and the golden files of the tests are regenerated in the same commit. */
#define GTEXT_CSS_COMPUTED_DUMP_VERSION 1

/**
 * @brief Write the whole style in a canonical, versioned text.
 * The first line is `css-computed 1` (GTEXT_CSS_COMPUTED_DUMP_VERSION), the second `registry ` and the commit of the pin the registry's generated
 * properties come from, then one line `name: value` for every property that has a value, **in the byte order of the names** (the custom properties, whose
 * names begin with `--`, sort among the others by the same rule), each value written as a computed value is serialized (gtext_css_computed_serialize()).
 * Which declaration a value came from is not in it: two cascades with equal results give equal text, and the text of a style is the same on every host.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_WRITE.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_computed_dump(const GTEXT_CSS_Computed_Style * style, const GTEXT_Markup_Sink * sink);

/** @brief The length of a hash, GTEXT_CSS_COMPUTED_HASH_LENGTH bytes: SHA-256. */
#define GTEXT_CSS_COMPUTED_HASH_LENGTH 32u

/**
 * @brief The SHA-256 of the text gtext_css_computed_dump() writes (through the security library), for a document dump that wants one number for a style.
 * @param hash Receives GTEXT_CSS_COMPUTED_HASH_LENGTH bytes. Not NULL.
 * @return OK, E_INVALID or E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_computed_hash(const GTEXT_CSS_Computed_Style * style, unsigned char hash[GTEXT_CSS_COMPUTED_HASH_LENGTH]);

/* ---- reads that name what they could not give ------------------------------------------------------------ */

/**
 * @brief The computed value of a property, as gtext_css_computed_value() gives it, with the property named when the style cannot give one.
 * @param style The style. Not NULL.
 * @param property A property of the context's registry (an alias is followed).
 * @param set Receives the value, owned by the style; NULL if not wanted.
 * @param result NULL, or filled: for GTEXT_CSS_E_UNSUPPORTED its `name` is the property the engine cannot compute and `property` its id.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (not a property, or a shorthand); GTEXT_CSS_E_UNSUPPORTED (the engine cannot compute it and it has no initial value).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_computed_get(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property,
    const GTEXT_CSS_Value_Set ** set, GTEXT_CSS_Compute_Result * result);

/** @brief gtext_css_computed_get() by name (ASCII case-insensitive for a property of the registry; a custom property is not one: GTEXT_CSS_E_INVALID, use gtext_css_computed_custom_find()). */
GTEXT_API GTEXT_CSS_Status gtext_css_computed_get_by_name(const GTEXT_CSS_Computed_Style * style, const char * name, size_t length,
    const GTEXT_CSS_Value_Set ** set, GTEXT_CSS_Compute_Result * result);

/**
 * @brief Walk the properties that have a computed value, in the order of the registry (the sorted order of the pin's names, the caller's after).
 * @param style The style. Not NULL.
 * @param cursor Where to continue: set it to 0 to begin; it is advanced.
 * @param property Receives the property; NULL if not wanted.
 * @param set Receives the value, owned by the style; NULL if not wanted.
 * @return true, or false when there are no more.
 */
GTEXT_API bool gtext_css_computed_next(const GTEXT_CSS_Computed_Style * style, size_t * cursor, GTEXT_CSS_Property_Id * property,
    const GTEXT_CSS_Value_Set ** set);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_COMPUTED_H
