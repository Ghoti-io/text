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
 * @file css_font_rules.h
 * @brief Typed access to `@font-face`, `@font-feature-values`, `@font-palette-values` and `@color-profile`, shaped for the `font` library: plain structs of strings, numbers and arrays.
 *
 * Normative text: CSS Fonts 4 and 5 (4.3 `@font-face`, 4.5 `unicode-range`, 6.3 `@font-feature-values`, 9 `@font-palette-values`), CSS Color 5 (`@color-profile`), as served 2026-10-29. Font matching,
 * loading, `unicode-range` coverage tests against text and the palette lookup stay `font`'s: this header only says what a rule wrote.
 *
 * A struct here is **owned** by the call that made it and released by its `_free`; the strings and arrays it points to live as long as it does. The structs name no CSS-internal type
 * beyond GTEXT_CSS_String, so a library that does not want to depend on this header can copy a face field by field into types of its own.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_FONT_RULES_H
#define GHOTI_IO_GTEXT_CSS_CSS_FONT_RULES_H

#include <ghoti.io/text/css/css_atrule.h>
#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @name Which descriptors a `@font-face` wrote
 *  The bits of GTEXT_CSS_Font_Face::present: `font` can tell `auto` from absent.
 *  @{ */
#define GTEXT_CSS_FACE_HAS_FAMILY 0x1u
#define GTEXT_CSS_FACE_HAS_SRC 0x2u
#define GTEXT_CSS_FACE_HAS_STYLE 0x4u
#define GTEXT_CSS_FACE_HAS_WEIGHT 0x8u
#define GTEXT_CSS_FACE_HAS_WIDTH 0x10u
#define GTEXT_CSS_FACE_HAS_UNICODE_RANGE 0x20u
#define GTEXT_CSS_FACE_HAS_FEATURE_SETTINGS 0x40u
#define GTEXT_CSS_FACE_HAS_VARIATION_SETTINGS 0x80u
#define GTEXT_CSS_FACE_HAS_DISPLAY 0x100u
#define GTEXT_CSS_FACE_HAS_ASCENT_OVERRIDE 0x200u
#define GTEXT_CSS_FACE_HAS_DESCENT_OVERRIDE 0x400u
#define GTEXT_CSS_FACE_HAS_LINE_GAP_OVERRIDE 0x800u
#define GTEXT_CSS_FACE_HAS_SIZE_ADJUST 0x1000u
#define GTEXT_CSS_FACE_HAS_FONT_SIZE 0x2000u
#define GTEXT_CSS_FACE_HAS_NAMED_INSTANCE 0x4000u
#define GTEXT_CSS_FACE_HAS_LANGUAGE_OVERRIDE 0x8000u
#define GTEXT_CSS_FACE_HAS_SUPERSCRIPT_POSITION 0x10000u
#define GTEXT_CSS_FACE_HAS_SUBSCRIPT_POSITION 0x20000u
#define GTEXT_CSS_FACE_HAS_SUPERSCRIPT_SIZE 0x40000u
#define GTEXT_CSS_FACE_HAS_SUBSCRIPT_SIZE 0x80000u
/** @} */

/** @brief The kind of a `src` entry. */
typedef enum { GTEXT_CSS_FONT_SRC_URL = 0, GTEXT_CSS_FONT_SRC_LOCAL } GTEXT_CSS_Font_Src_Kind;

/** @brief One entry of `src`, in order. */
typedef struct {
  GTEXT_CSS_Font_Src_Kind kind;
  GTEXT_CSS_String url;       ///< A `url()` entry: the URL as written (not resolved: the caller's).
  GTEXT_CSS_String local;     ///< A `local()` entry: the face name, decoded.
  bool has_format;            ///< `format(...)` was written.
  GTEXT_CSS_String format;    ///< ... its argument, decoded (a string or an ident).
  uint32_t format_bit;        ///< GTEXT_CSS_FONT_FORMAT_* of the argument if it names one the pin lists (css_condition.h), else 0 (a string nobody knows).
  uint32_t tech;              ///< The OR of GTEXT_CSS_FONT_TECH_* of `tech(...)`.
} GTEXT_CSS_Font_Src;

/** @brief `font-style` of a face. */
typedef enum {
  GTEXT_CSS_FACE_STYLE_AUTO = 0, GTEXT_CSS_FACE_STYLE_NORMAL, GTEXT_CSS_FACE_STYLE_ITALIC, GTEXT_CSS_FACE_STYLE_LEFT, GTEXT_CSS_FACE_STYLE_RIGHT, GTEXT_CSS_FACE_STYLE_OBLIQUE
} GTEXT_CSS_Face_Style;

/** @brief `font-display`. */
typedef enum {
  GTEXT_CSS_FONT_DISPLAY_AUTO = 0, GTEXT_CSS_FONT_DISPLAY_BLOCK, GTEXT_CSS_FONT_DISPLAY_SWAP, GTEXT_CSS_FONT_DISPLAY_FALLBACK, GTEXT_CSS_FONT_DISPLAY_OPTIONAL
} GTEXT_CSS_Font_Display;

/** @brief A code point range, inclusive. */
typedef struct {
  uint32_t first;
  uint32_t last;
} GTEXT_CSS_Code_Range;

/** @brief A feature or variation setting: a four-character tag and a value. */
typedef struct {
  char tag[5];   ///< The tag, NUL terminated.
  double value;  ///< The value (a feature's integer, a variation axis' number).
} GTEXT_CSS_Font_Setting;

/** @brief An override that is `normal`, `from-font` or a percentage. */
typedef struct {
  uint8_t kind;    ///< 0 normal, 1 from-font, 2 a percentage.
  double percent;  ///< The percentage when kind is 2.
} GTEXT_CSS_Font_Override;

/**
 * @struct GTEXT_CSS_Font_Face
 * @brief What a `@font-face` rule wrote, typed. Made by gtext_css_font_face_from_atrule(); release it with gtext_css_font_face_free().
 */
typedef struct GTEXT_CSS_Font_Face {
  uint32_t present;                        ///< GTEXT_CSS_FACE_HAS_* of the descriptors the rule wrote.
  GTEXT_CSS_String family;                 ///< `font-family`, decoded and unquoted.
  GTEXT_CSS_String family_key;             ///< The family folded by Unicode default caseless matching (Fonts 4, 5: full case folding, no normalization), what a lookup compares.
  const GTEXT_CSS_Font_Src * src;          ///< The `src` entries in order.
  size_t src_count;
  GTEXT_CSS_Face_Style style;
  double oblique_min;                      ///< `oblique`: the angle range in degrees (14 for a bare `oblique`).
  double oblique_max;
  bool weight_auto;                        ///< `font-weight: auto`.
  double weight_min;                       ///< The closed range (1 to 1000); one value is a range of one point.
  double weight_max;
  bool width_auto;                         ///< `font-width: auto`.
  double width_min;                        ///< Percentages (`condensed` is 75, `normal` 100).
  double width_max;
  const GTEXT_CSS_Code_Range * ranges;     ///< `unicode-range`, merged, sorted and non-overlapping; the default `U+0-10FFFF` when absent.
  size_t range_count;
  const GTEXT_CSS_Font_Setting * features; ///< `font-feature-settings` in order (the later of a tag wins on request).
  size_t feature_count;
  const GTEXT_CSS_Font_Setting * variations; ///< `font-variation-settings` in order.
  size_t variation_count;
  GTEXT_CSS_Font_Display display;
  GTEXT_CSS_Font_Override ascent[2];       ///< `ascent-override`: one or two values (kind 0 normal or 2 percentage); the second is the first when one was written.
  GTEXT_CSS_Font_Override descent[2];
  GTEXT_CSS_Font_Override line_gap[2];
  double size_adjust;                      ///< `size-adjust` as a percentage (100 initially).
  bool font_size_auto;                     ///< `font-size: auto`.
  double font_size_min;
  double font_size_max;
  bool named_instance_auto;
  GTEXT_CSS_String named_instance;
  bool language_override_normal;
  GTEXT_CSS_String language_override;
  GTEXT_CSS_Font_Override superscript_position[2];
  GTEXT_CSS_Font_Override subscript_position[2];
  GTEXT_CSS_Font_Override superscript_size[2];
  GTEXT_CSS_Font_Override subscript_size[2];
  const GTEXT_CSS_Atrule * rule;           ///< The rule (borrowed: it must outlive the face).
  void * priv_;                            ///< Private.
} GTEXT_CSS_Font_Face;

/** @brief Read a `@font-face` rule. @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (not a font-face rule); GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_DESCRIPTORS (more unicode ranges, sources or settings than the rule's limits). */
GTEXT_API GTEXT_CSS_Status gtext_css_font_face_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Font_Face ** out);

/** @brief Release a face; NULL is ignored. */
GTEXT_API void gtext_css_font_face_free(GTEXT_CSS_Font_Face * face);

/** @brief Whether the rule defines a face: it wrote both `font-family` and `src` ("if either of these are missing, the @font-face rule must not be considered"). */
GTEXT_API bool gtext_css_font_face_defined(const GTEXT_CSS_Font_Face * face);

/** @brief The feature types of `@font-feature-values`. */
typedef enum {
  GTEXT_CSS_FEATURE_STYLISTIC = 0, GTEXT_CSS_FEATURE_HISTORICAL_FORMS, GTEXT_CSS_FEATURE_STYLESET, GTEXT_CSS_FEATURE_CHARACTER_VARIANT, GTEXT_CSS_FEATURE_SWASH,
  GTEXT_CSS_FEATURE_ORNAMENTS, GTEXT_CSS_FEATURE_ANNOTATION, GTEXT_CSS_FEATURE_TYPE_COUNT
} GTEXT_CSS_Feature_Type;

/** @brief A named feature value: a name and its integers (`nice-style: 12` is {"nice-style", [12]}). */
typedef struct {
  GTEXT_CSS_String name;
  const uint32_t * values;
  size_t value_count;
} GTEXT_CSS_Feature_Value;

/** @brief What a `@font-feature-values` rule wrote: the families it is for, and for each feature type its name to integers. */
typedef struct GTEXT_CSS_Feature_Values {
  const GTEXT_CSS_String * families;        ///< The family names (decoded).
  const GTEXT_CSS_String * family_keys;     ///< ... folded as the face's.
  size_t family_count;
  const GTEXT_CSS_Feature_Value * values[GTEXT_CSS_FEATURE_TYPE_COUNT]; ///< Per type, in order, a later name replacing an earlier one in the same block.
  size_t value_count[GTEXT_CSS_FEATURE_TYPE_COUNT];
  GTEXT_CSS_Font_Display display;           ///< `font-display` in the rule (it has the descriptor), auto when absent.
  bool has_display;
  const GTEXT_CSS_Atrule * rule;
  void * priv_;
} GTEXT_CSS_Feature_Values;

GTEXT_API GTEXT_CSS_Status gtext_css_feature_values_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Feature_Values ** out);
GTEXT_API void gtext_css_feature_values_free(GTEXT_CSS_Feature_Values * values);

/** @brief The base palette of `@font-palette-values`. */
typedef enum { GTEXT_CSS_BASE_PALETTE_NONE = 0, GTEXT_CSS_BASE_PALETTE_LIGHT, GTEXT_CSS_BASE_PALETTE_DARK, GTEXT_CSS_BASE_PALETTE_INDEX } GTEXT_CSS_Base_Palette_Kind;

/** @brief An `override-colors` entry: a palette index and a color, as text in its declared color space (the `color` library converts it). */
typedef struct {
  uint32_t index;
  GTEXT_CSS_String color;
} GTEXT_CSS_Palette_Override;

/** @brief What a `@font-palette-values` rule wrote. */
typedef struct GTEXT_CSS_Palette_Values {
  GTEXT_CSS_String name;                    ///< The dashed name.
  const GTEXT_CSS_String * families;        ///< `font-family`: named families only.
  const GTEXT_CSS_String * family_keys;
  size_t family_count;
  GTEXT_CSS_Base_Palette_Kind base_kind;
  uint32_t base_index;                      ///< `base-palette` when it is an index.
  const GTEXT_CSS_Palette_Override * overrides;
  size_t override_count;
  const GTEXT_CSS_Atrule * rule;
  void * priv_;
} GTEXT_CSS_Palette_Values;

GTEXT_API GTEXT_CSS_Status gtext_css_palette_values_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Palette_Values ** out);
GTEXT_API void gtext_css_palette_values_free(GTEXT_CSS_Palette_Values * values);

/** @brief The rendering intent of `@color-profile`. */
typedef enum {
  GTEXT_CSS_INTENT_RELATIVE_COLORIMETRIC = 0, GTEXT_CSS_INTENT_ABSOLUTE_COLORIMETRIC, GTEXT_CSS_INTENT_PERCEPTUAL, GTEXT_CSS_INTENT_SATURATION
} GTEXT_CSS_Rendering_Intent;

/** @brief What a `@color-profile` rule wrote. */
typedef struct GTEXT_CSS_Color_Profile {
  GTEXT_CSS_String name;                    ///< The dashed name, or `device-cmyk`.
  bool device_cmyk;
  GTEXT_CSS_String src;                     ///< The URL as written (absent: data NULL).
  GTEXT_CSS_Rendering_Intent intent;
  const GTEXT_CSS_String * components;      ///< `components`: the idents.
  size_t component_count;
  const GTEXT_CSS_Atrule * rule;
  void * priv_;
} GTEXT_CSS_Color_Profile;

GTEXT_API GTEXT_CSS_Status gtext_css_color_profile_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Color_Profile ** out);
GTEXT_API void gtext_css_color_profile_free(GTEXT_CSS_Color_Profile * profile);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_FONT_RULES_H
