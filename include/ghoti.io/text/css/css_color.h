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
 * @file css_color.h
 * @brief CSS colors as typed values: the declared space and channels, with
 *   `none` kept as missing, and the serialization of the declared value.
 *
 * Reference: CSS Color Module Level 4, Editor's Draft (served 2026-10-22), sections
 * 4 to 9 for the syntaxes and 16 for serialization; CSS Color Module Level 5, Editor's
 * Draft of 19 August 2026 (`color-mix()`, relative color syntax, `light-dark()`,
 * `contrast-color()`, `alpha()`, `device-cmyk()`, custom color spaces) and its section 11.
 *
 * ## What this holds and what it does not
 *
 * A @ref GTEXT_CSS_Color is **what was written**: the function or keyword, the
 * declared color space, the channels as numbers, percentages, angles, math functions or
 * (in a relative color) channel keywords, and the alpha. Nothing is converted
 * to another space and nothing is gamut mapped; mixing and relative colors are not computed.
 * Those belong to the `color` library, which `text` does not depend on (CSS plan, decision 5).
 * The one conversion here is the one the draft's serialization of the legacy syntaxes
 * needs: `hsl()` and `hwb()` to sRGB, in IEEE double through the `math` library
 * (and rounded to 8 bits, which the draft allows: "at least sufficient to round-trip
 * eight bit values").
 *
 * ## Forms
 *
 * `named` colors, system colors, `transparent`, `currentcolor`; `#rgb #rgba #rrggbb
 * #rrggbbaa`; `rgb() rgba() hsl() hsla() hwb() lab() lch() oklab() oklch()`
 * and `color()` with every predefined space (`xyz` is `xyz-d65`) and a custom
 * space (`--name`); `color-mix()`, `light-dark()`, `contrast-color()`,
 * `alpha(from ...)` and `device-cmyk()`; relative color syntax (`from <color>`, channel keywords,
 * usable inside math); the legacy comma syntax of `rgb()` and `hsl()` and the modern one.
 * `color-layers()` is a CSS Color 6 function and is not in the served Level 5.
 *
 * ## Serialization
 *
 * As Color 4 section 16 and Color 5 section 11 write the *declared value*: a named color
 * stays its keyword; a hex color, `rgb()`, `hsl()` and `hwb()` become `rgb(r, g, b)` or
 * `rgba(r, g, b, a)` unless a channel is `none` (`color(srgb ...)`, or `hsl()`/`hwb()`
 * with `none` kept) or not yet a number (a math function that needs the environment, or
 * a channel keyword: then the function is written with its channels as specified);
 * `lab() lch() oklab() oklch()` and `color()` keep their function, with percentages
 * converted to numbers by their reference ranges, hue angles in degrees from 0 up to 360, and
 * math functions written in their simplified form; alpha of 1 is omitted.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_COLOR_H
#define GHOTI_IO_GTEXT_CSS_CSS_COLOR_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_math.h>
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

/** @brief What a color is. */
typedef enum {
  GTEXT_CSS_COLOR_KEYWORD = 0, ///< `red`, `transparent`, `currentcolor`, a system color.
  GTEXT_CSS_COLOR_HEX,         ///< `#rgb` and its three kin.
  GTEXT_CSS_COLOR_FUNCTION,    ///< `rgb()`, `hsl()`, `hwb()`, `lab()`, `lch()`, `oklab()`, `oklch()`, `color()`.
  GTEXT_CSS_COLOR_MIX,         ///< `color-mix()`: `items`, `percents` and the method.
  GTEXT_CSS_COLOR_LIGHT_DARK,  ///< `light-dark()`: `items`, two.
  GTEXT_CSS_COLOR_CONTRAST,    ///< `contrast-color()`: `origin`.
  GTEXT_CSS_COLOR_ALPHA,       ///< `alpha(from <color> / <alpha>)`: `origin` and `alpha`.
  GTEXT_CSS_COLOR_DEVICE_CMYK  ///< `device-cmyk()`: four components.
} GTEXT_CSS_Color_Kind;

/** @brief What a keyword color is. */
typedef enum {
  GTEXT_CSS_COLOR_KW_NONE = 0,    ///< Not a keyword color.
  GTEXT_CSS_COLOR_KW_NAMED,       ///< A named color (`red`): `components` hold its sRGB value.
  GTEXT_CSS_COLOR_KW_TRANSPARENT, ///< `transparent`.
  GTEXT_CSS_COLOR_KW_CURRENTCOLOR, ///< `currentcolor`.
  GTEXT_CSS_COLOR_KW_SYSTEM,      ///< A system color (`Canvas`).
  GTEXT_CSS_COLOR_KW_DEPRECATED   ///< A deprecated system color (`ThreeDFace`).
} GTEXT_CSS_Color_Keyword;

/** @brief The function a function-form color was written with. */
typedef enum {
  GTEXT_CSS_COLOR_FN_NONE = 0,
  GTEXT_CSS_COLOR_FN_RGB, ///< `rgb()` and `rgba()`
  GTEXT_CSS_COLOR_FN_HSL, ///< `hsl()` and `hsla()`
  GTEXT_CSS_COLOR_FN_HWB,
  GTEXT_CSS_COLOR_FN_LAB,
  GTEXT_CSS_COLOR_FN_LCH,
  GTEXT_CSS_COLOR_FN_OKLAB,
  GTEXT_CSS_COLOR_FN_OKLCH,
  GTEXT_CSS_COLOR_FN_COLOR ///< `color()`
} GTEXT_CSS_Color_Function;

/** @brief A color space (`xyz` is XYZ_D65). */
typedef enum {
  GTEXT_CSS_SPACE_SRGB = 0,
  GTEXT_CSS_SPACE_SRGB_LINEAR,
  GTEXT_CSS_SPACE_DISPLAY_P3,
  GTEXT_CSS_SPACE_DISPLAY_P3_LINEAR,
  GTEXT_CSS_SPACE_A98_RGB,
  GTEXT_CSS_SPACE_PROPHOTO_RGB,
  GTEXT_CSS_SPACE_REC2020,
  GTEXT_CSS_SPACE_XYZ_D50,
  GTEXT_CSS_SPACE_XYZ_D65,
  GTEXT_CSS_SPACE_LAB,
  GTEXT_CSS_SPACE_LCH,
  GTEXT_CSS_SPACE_OKLAB,
  GTEXT_CSS_SPACE_OKLCH,
  GTEXT_CSS_SPACE_HSL,
  GTEXT_CSS_SPACE_HWB,
  GTEXT_CSS_SPACE_CUSTOM ///< A `--name` space: the name is in `GTEXT_CSS_Color::name`.
} GTEXT_CSS_Color_Space;

/** @brief What a channel is. */
typedef enum {
  GTEXT_CSS_COMP_NUMBER = 0, ///< A number.
  GTEXT_CSS_COMP_PERCENT,    ///< A percentage (`value` percent).
  GTEXT_CSS_COMP_ANGLE,      ///< A hue given as an angle: `value` in degrees.
  GTEXT_CSS_COMP_NONE,       ///< `none`: a missing component.
  GTEXT_CSS_COMP_MATH,       ///< A math function (`math`), kept as written (simplified).
  GTEXT_CSS_COMP_SYMBOL      ///< A channel keyword of a relative color (`text`: `r`, `alpha` ...).
} GTEXT_CSS_Color_Comp_Kind;

/** @brief A channel of a color. */
typedef struct {
  GTEXT_CSS_Color_Comp_Kind kind;  ///< What it is.
  double value;                    ///< NUMBER, PERCENT, ANGLE.
  const GTEXT_CSS_Math * math;     ///< MATH.
  GTEXT_CSS_String text;           ///< SYMBOL.
} GTEXT_CSS_Color_Component;

/** @brief The hue interpolation method of a `color-mix()`. */
typedef enum {
  GTEXT_CSS_HUE_UNSPECIFIED = 0,
  GTEXT_CSS_HUE_SHORTER,
  GTEXT_CSS_HUE_LONGER,
  GTEXT_CSS_HUE_INCREASING,
  GTEXT_CSS_HUE_DECREASING
} GTEXT_CSS_Hue_Method;

/**
 * @struct GTEXT_CSS_Color
 * @brief A color as written (immutable; owned by the value set or the call that made it).
 */
struct GTEXT_CSS_Color {
  GTEXT_CSS_Color_Kind kind;              ///< What it is.
  GTEXT_CSS_Color_Keyword keyword;        ///< KEYWORD.
  GTEXT_CSS_Color_Function function;      ///< FUNCTION.
  GTEXT_CSS_Color_Space space;            ///< The declared space (the space of a FUNCTION, a HEX or a named color is sRGB).
  bool legacy;                            ///< Written with the comma syntax of `rgb()` or `hsl()`.
  bool alpha_written;                     ///< An alpha was written (`alpha` is then it; otherwise 1).
  bool has_missing;                       ///< Some channel or the alpha is `none`.
  bool has_unresolved;                    ///< Some channel needs an environment: a channel keyword or a math function that is not yet a number.
  GTEXT_CSS_String name;                  ///< KEYWORD: the keyword in lower case; a custom space: its name.
  GTEXT_CSS_Color_Component * components; ///< The channels (3, 4 for `device-cmyk()`, any number for a custom space); sRGB
                                          ///< values in 0 to 255 for a named color and a hex color.
  size_t component_count;                 ///< Their number.
  GTEXT_CSS_Color_Component alpha;        ///< The alpha.
  const GTEXT_CSS_Color * origin;         ///< `from <color>` of a relative color; the argument of `contrast-color()`.
  const GTEXT_CSS_Color * const * items;  ///< `color-mix()` and `light-dark()`: the colors.
  size_t item_count;                      ///< Their number.
  GTEXT_CSS_Color_Component * percents;   ///< `color-mix()`: one for each item; NONE when omitted.
  bool mix_has_space;                     ///< `color-mix()` wrote `in <space>`.
  GTEXT_CSS_Color_Space mix_space;        ///< The space it mixes in (OKLAB when not written).
  GTEXT_CSS_String mix_custom;            ///< A custom mixing space's name.
  GTEXT_CSS_Hue_Method hue_method;        ///< The hue method written, if any.
  GTEXT_CSS_Span span;                    ///< The component value it was read from.
  void * owner_;                          ///< Private.
};

/** @brief The allocator and limits of reading a color. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator; ///< NULL for the default.
  size_t max_nesting;                ///< Deepest nesting of colors in colors. Default 32; at most 4096. One more is GTEXT_CSS_E_LIMIT_COLOR_NESTING.
  size_t max_math_depth;             ///< For the math functions in channels (see GTEXT_CSS_Math_Options). 0: the default.
  size_t max_math_nodes;             ///< 0: the default.
} GTEXT_CSS_Color_Options;

/** @brief What reading a color reports. */
typedef struct {
  GTEXT_CSS_Status status; ///< What the call returned.
  const char * message;    ///< A static description.
  GTEXT_CSS_Span where;    ///< A limit: the component value where it was reached.
  size_t nodes;            ///< Colors made.
  size_t work;             ///< Units of work done.
} GTEXT_CSS_Color_Result;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Color_Options gtext_css_color_options_default(void);

/**
 * @brief Read a color from one component value.
 *
 * A color is always one component value: an identifier, a hash, or a function.
 *
 * @param value The component value. Not NULL.
 * @param options NULL for the defaults.
 * @param valid Receives whether it is a `<color>`. Written on success. Not NULL.
 * @param out Receives the color when valid; written only on success. Not NULL.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK (valid or not); GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM;
 *   GTEXT_CSS_E_LIMIT_COLOR_NESTING; GTEXT_CSS_E_LIMIT_DEPTH; GTEXT_CSS_E_LIMIT_MATH_NODES.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_color_parse(const GTEXT_CSS_Component_Value * value,
    const GTEXT_CSS_Color_Options * options, bool * valid, GTEXT_CSS_Color ** out, GTEXT_CSS_Color_Result * result);

/** @brief Free a color made by gtext_css_color_parse(); NULL is ignored. */
GTEXT_API void gtext_css_color_free(GTEXT_CSS_Color * color);

/**
 * @brief Write the declared value of a color (see the file's description).
 *
 * @param color The color. Not NULL.
 * @param options NULL for the defaults.
 * @param sink Where the text goes. Not NULL, with a `write`.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_WRITE; GTEXT_CSS_E_LIMIT_OUTPUT.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_color_serialize(const GTEXT_CSS_Color * color,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink);

/** @brief gtext_css_color_serialize() to a new buffer (NUL-terminated; release with gtext_css_serialize_buffer_free()). */
GTEXT_API GTEXT_CSS_Status gtext_css_color_serialize_buffer(const GTEXT_CSS_Color * color,
    const GTEXT_CSS_Serialize_Options * options, char ** out, size_t * out_length);

/**
 * @brief The sRGB value of a color that is one: a named color, a hex color, and `rgb()`, `hsl()` and `hwb()` without
 *   `none` and without anything that needs an environment.
 *
 * @param color The color.
 * @param rgba Receives red, green, blue (0 to 255; `hsl()` and `hwb()` rounded to 8 bits) and alpha (0 to 1). Not NULL.
 * @return Whether the color is such a value; false leaves `rgba` alone.
 */
GTEXT_API bool gtext_css_color_srgb(const GTEXT_CSS_Color * color, double rgba[4]);

/** @brief The name of a color space as `color()` and `in` write it ("srgb", "display-p3", "xyz-d65" ...). Static. */
GTEXT_API const char * gtext_css_color_space_name(GTEXT_CSS_Color_Space space);

/** @brief The number of named colors (148, `rebeccapurple` included). */
GTEXT_API size_t gtext_css_color_named_count(void);

/** @brief The name and the sRGB value (as 0xRRGGBB) of the `index`th named color, in alphabetical order; false when there is none. */
GTEXT_API bool gtext_css_color_named_at(size_t index, const char ** name, uint32_t * rgb);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_COLOR_H
