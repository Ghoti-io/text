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
 * @file css_media.h
 * @brief Media Queries 4 and 5: the media environment, the parser of media query lists, and the CSSOM serializer.
 *
 * Normative text: the served Editor's Drafts of Media Queries 4 and 5, CSS Values 4 (ratios, resolutions, lengths) and CSSOM ("serialize a media query
 * list"), read 2026-10-28. The sentences that the tests quote are in tests/test-css-media.cpp. Another implementation's code is not read.
 *
 * ## The parts
 *
 * - @ref GTEXT_CSS_Media_Environment: what a media feature is evaluated against. Plain data, **size-prefixed** (a caller built before a field was
 *   added keeps working), nothing read from the process, the clock, the locale or the hardware.
 * - @ref GTEXT_CSS_Media_List: the parsed form of a `<media-query-list>`: immutable, atomically reference counted, shareable between threads. It
 *   is a condition **AST**, not a truth value; it is evaluated against an environment by a @ref GTEXT_CSS_Conditions (css_condition.h), which
 *   is what makes a resize a new environment and not a new sheet.
 * - gtext_css_media_list_serialize(): CSSOM's "serialize a media query list", canonical; a parse of its output is the same list.
 *
 * ## Error handling (Media Queries 4, 3.2)
 *
 * **An invalid media query is an answer, not a failure**: it is replaced by `not all` (it parses, serializes as `not all` and never matches), the
 * others of its list stand, and the result holds a diagnostic with a span and the sentence that decides it. A failure is an argument, memory or a
 * limit. Which things are invalid and which are unknown:
 *
 * | Input | Result |
 * |---|---|
 * | an empty query, `only` with no type, a missing `and`, `or` beside `and`, `not` where the grammar forbids it, a reserved word as a type | invalid: `not all` |
 * | a known feature with a value of the wrong kind (`(width: 5)`, `(color: 20example)`), a range with its comparisons in two directions, a prefix on a discrete feature | invalid: `not all` |
 * | an unknown media type (`foo`) | valid, false; `not foo` is true |
 * | an unknown feature name (`(foo: bar)`), or a parenthesized thing the grammar does not read | `<general-enclosed>`: kept, evaluates **unknown** |
 *
 * ## Evaluation
 *
 * Three-valued (Kleene): `GTEXT_CSS_Tristate`. `not unknown` is unknown, `false and unknown` is false, `true or unknown` is true, and **unknown at
 * the top is false**. A feature is evaluated only if the environment says it supports it (`supported`): any other is unknown.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_MEDIA_H
#define GHOTI_IO_GTEXT_CSS_CSS_MEDIA_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_parse.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the environment -------------------------------------------------- */

/** @brief The medium of the environment (the media type that matches; `all` always does). */
typedef enum {
  GTEXT_CSS_MEDIUM_SCREEN = 0, ///< `screen` matches.
  GTEXT_CSS_MEDIUM_PRINT,      ///< `print` matches.
  GTEXT_CSS_MEDIUM_SPEECH      ///< Neither matches. (The served text lists `speech` among the deprecated types, which match nothing.)
} GTEXT_CSS_Medium;

/** @name Keyword values of the media features
 *  Each feature's field holds one of these (0 is the first, or "none"). A value of a field that is outside its enum is refused by
 *  gtext_css_media_environment_check().
 *  @{ */
typedef enum { GTEXT_CSS_MQ_SCAN_NONE = 0, GTEXT_CSS_MQ_SCAN_INTERLACE, GTEXT_CSS_MQ_SCAN_PROGRESSIVE } GTEXT_CSS_Mq_Scan;
typedef enum { GTEXT_CSS_MQ_UPDATE_NONE = 0, GTEXT_CSS_MQ_UPDATE_SLOW, GTEXT_CSS_MQ_UPDATE_FAST } GTEXT_CSS_Mq_Update;
typedef enum { GTEXT_CSS_MQ_OVERFLOW_BLOCK_NONE = 0, GTEXT_CSS_MQ_OVERFLOW_BLOCK_SCROLL, GTEXT_CSS_MQ_OVERFLOW_BLOCK_PAGED } GTEXT_CSS_Mq_Overflow_Block;
typedef enum { GTEXT_CSS_MQ_OVERFLOW_INLINE_NONE = 0, GTEXT_CSS_MQ_OVERFLOW_INLINE_SCROLL } GTEXT_CSS_Mq_Overflow_Inline;
/** The widest gamut the device supports; each implies the narrower ones (`NONE` matches no keyword). */
typedef enum { GTEXT_CSS_MQ_GAMUT_NONE = 0, GTEXT_CSS_MQ_GAMUT_SRGB, GTEXT_CSS_MQ_GAMUT_P3, GTEXT_CSS_MQ_GAMUT_REC2020 } GTEXT_CSS_Mq_Gamut;
/** `high` implies `standard`. */
typedef enum { GTEXT_CSS_MQ_RANGE_STANDARD = 0, GTEXT_CSS_MQ_RANGE_HIGH } GTEXT_CSS_Mq_Dynamic_Range;
typedef enum { GTEXT_CSS_MQ_POINTER_NONE = 0, GTEXT_CSS_MQ_POINTER_COARSE, GTEXT_CSS_MQ_POINTER_FINE } GTEXT_CSS_Mq_Pointer;
typedef enum { GTEXT_CSS_MQ_HOVER_NONE = 0, GTEXT_CSS_MQ_HOVER_HOVER } GTEXT_CSS_Mq_Hover;
/** Bits of `any_pointer`. */
#define GTEXT_CSS_MQ_ANY_POINTER_NONE 0x1u
#define GTEXT_CSS_MQ_ANY_POINTER_COARSE 0x2u
#define GTEXT_CSS_MQ_ANY_POINTER_FINE 0x4u
/** Bits of `any_hover`. */
#define GTEXT_CSS_MQ_ANY_HOVER_NONE 0x1u
#define GTEXT_CSS_MQ_ANY_HOVER_HOVER 0x2u
typedef enum {
  GTEXT_CSS_MQ_DISPLAY_MODE_APPLICATION = 0, GTEXT_CSS_MQ_DISPLAY_MODE_FULLSCREEN, GTEXT_CSS_MQ_DISPLAY_MODE_STANDALONE,
  GTEXT_CSS_MQ_DISPLAY_MODE_MINIMAL_UI, GTEXT_CSS_MQ_DISPLAY_MODE_BROWSER, GTEXT_CSS_MQ_DISPLAY_MODE_PICTURE_IN_PICTURE
} GTEXT_CSS_Mq_Display_Mode;
typedef enum {
  GTEXT_CSS_MQ_DISPLAY_STATE_NORMAL = 0, GTEXT_CSS_MQ_DISPLAY_STATE_MINIMIZED, GTEXT_CSS_MQ_DISPLAY_STATE_MAXIMIZED,
  GTEXT_CSS_MQ_DISPLAY_STATE_FULLSCREEN
} GTEXT_CSS_Mq_Display_State;
typedef enum { GTEXT_CSS_MQ_SHAPE_RECT = 0, GTEXT_CSS_MQ_SHAPE_ROUND } GTEXT_CSS_Mq_Shape;
typedef enum { GTEXT_CSS_MQ_BLENDING_OPAQUE = 0, GTEXT_CSS_MQ_BLENDING_ADDITIVE, GTEXT_CSS_MQ_BLENDING_SUBTRACTIVE } GTEXT_CSS_Mq_Blending;
typedef enum { GTEXT_CSS_MQ_NAV_NONE = 0, GTEXT_CSS_MQ_NAV_BACK } GTEXT_CSS_Mq_Nav_Controls;
typedef enum { GTEXT_CSS_MQ_INVERTED_NONE = 0, GTEXT_CSS_MQ_INVERTED_INVERTED } GTEXT_CSS_Mq_Inverted_Colors;
typedef enum { GTEXT_CSS_MQ_SCRIPTING_NONE = 0, GTEXT_CSS_MQ_SCRIPTING_INITIAL_ONLY, GTEXT_CSS_MQ_SCRIPTING_ENABLED } GTEXT_CSS_Mq_Scripting;
typedef enum { GTEXT_CSS_MQ_FORCED_COLORS_NONE = 0, GTEXT_CSS_MQ_FORCED_COLORS_ACTIVE } GTEXT_CSS_Mq_Forced_Colors;
typedef enum { GTEXT_CSS_MQ_SCHEME_LIGHT = 0, GTEXT_CSS_MQ_SCHEME_DARK } GTEXT_CSS_Mq_Scheme;
typedef enum {
  GTEXT_CSS_MQ_CONTRAST_NO_PREFERENCE = 0, GTEXT_CSS_MQ_CONTRAST_LESS, GTEXT_CSS_MQ_CONTRAST_MORE, GTEXT_CSS_MQ_CONTRAST_CUSTOM
} GTEXT_CSS_Mq_Contrast;
typedef enum { GTEXT_CSS_MQ_REDUCE_NO_PREFERENCE = 0, GTEXT_CSS_MQ_REDUCE_REDUCE } GTEXT_CSS_Mq_Reduce;
/** @} */

/**
 * @struct GTEXT_CSS_Media_Environment
 * @brief What media features are evaluated against. Initialize it with gtext_css_media_environment_default(), then set what differs.
 *
 * Lengths are CSS pixels, a resolution is in dppx. **A feature is evaluated only if the environment says it supports it**: `supported` has one bit
 * for each media feature of the generated table (use gtext_css_media_environment_set_supported(), not bit arithmetic), and a feature outside it is a
 * feature the "user agent does not support", which Media Queries 4 makes unknown. A zeroed struct supports nothing.
 */
typedef struct {
  size_t size;                       ///< `sizeof` this struct as the caller's header has it; set by gtext_css_media_environment_default().
  uint32_t supported[4];             ///< The features the environment can answer (128 bits; see gtext_css_media_environment_set_supported()).
  GTEXT_CSS_Medium medium;           ///< Which media type matches.
  GTEXT_CSS_Size viewport;           ///< `width`, `height`, `aspect-ratio`, `orientation`; the viewport units of a feature value. Default 1280 by 800.
  GTEXT_CSS_Size device;             ///< `device-width`, `device-height`, `device-aspect-ratio`. Default 1280 by 800.
  double resolution;                 ///< `resolution` and `-webkit-device-pixel-ratio`, in dppx. Default 1.
  bool resolution_infinite;          ///< `resolution` is `infinite` (an output with no physical constraint); `resolution` is then ignored.
  double default_font_size;          ///< What `em` and `rem` are in a feature value (Media Queries 4, 1.3: the initial `font-size`, never the document's). Default 16.
  uint32_t color_bits;               ///< `color`: bits per color component, 0 for a device with no color. Default 8.
  uint32_t color_index;              ///< `color-index`: entries of the color lookup table, 0 for none.
  uint32_t monochrome_bits;          ///< `monochrome`: bits per pixel of a monochrome device, 0 for a color device.
  uint32_t horizontal_viewport_segments; ///< `horizontal-viewport-segments`. Default 1.
  uint32_t vertical_viewport_segments;   ///< `vertical-viewport-segments`. Default 1.
  uint8_t color_gamut;               ///< GTEXT_CSS_Mq_Gamut.
  uint8_t video_color_gamut;         ///< GTEXT_CSS_Mq_Gamut.
  uint8_t dynamic_range;             ///< GTEXT_CSS_Mq_Dynamic_Range.
  uint8_t video_dynamic_range;       ///< GTEXT_CSS_Mq_Dynamic_Range.
  uint8_t scan;                      ///< GTEXT_CSS_Mq_Scan.
  uint8_t update;                    ///< GTEXT_CSS_Mq_Update.
  uint8_t overflow_block;            ///< GTEXT_CSS_Mq_Overflow_Block.
  uint8_t overflow_inline;           ///< GTEXT_CSS_Mq_Overflow_Inline.
  uint8_t pointer;                   ///< The primary pointing device: GTEXT_CSS_Mq_Pointer.
  uint8_t hover;                     ///< The primary pointing device: GTEXT_CSS_Mq_Hover.
  uint8_t any_pointer;               ///< A bit set of GTEXT_CSS_MQ_ANY_POINTER_*.
  uint8_t any_hover;                 ///< A bit set of GTEXT_CSS_MQ_ANY_HOVER_*.
  uint8_t display_mode;              ///< GTEXT_CSS_Mq_Display_Mode.
  uint8_t display_state;             ///< GTEXT_CSS_Mq_Display_State.
  uint8_t shape;                     ///< GTEXT_CSS_Mq_Shape.
  uint8_t environment_blending;      ///< GTEXT_CSS_Mq_Blending.
  uint8_t nav_controls;              ///< GTEXT_CSS_Mq_Nav_Controls.
  uint8_t inverted_colors;           ///< GTEXT_CSS_Mq_Inverted_Colors.
  uint8_t scripting;                 ///< GTEXT_CSS_Mq_Scripting.
  uint8_t forced_colors;             ///< GTEXT_CSS_Mq_Forced_Colors.
  uint8_t prefers_color_scheme;      ///< GTEXT_CSS_Mq_Scheme.
  uint8_t ua_color_scheme;           ///< GTEXT_CSS_Mq_Scheme.
  uint8_t prefers_contrast;          ///< GTEXT_CSS_Mq_Contrast.
  uint8_t prefers_reduced_motion;    ///< GTEXT_CSS_Mq_Reduce.
  uint8_t prefers_reduced_transparency; ///< GTEXT_CSS_Mq_Reduce.
  uint8_t prefers_reduced_data;      ///< GTEXT_CSS_Mq_Reduce.
  bool grid;                         ///< `grid`: a grid-based device (a terminal).
  bool resizable;                    ///< `resizable`.
  bool transform_3d;                 ///< `-webkit-transform-3d`.
  void * user;                       ///< Passed to the callbacks.
  /** A metric of the initial font, in px, for `ex`, `ch`, `cap`, `ic` and their `r` forms in a feature value. Returns false if the font has none. NULL: such
   *  a unit is GTEXT_CSS_E_UNSUPPORTED naming it. The query's size is `default_font_size`. */
  bool (*font_metric)(void * user, const GTEXT_CSS_Font_Query * query, GTEXT_CSS_Font_Metric metric, double * px);
  /** The value of a feature the caller defines (a vendor or experimental feature the table does not list: `(-moz-foo)`): `name` is lower case. Returns
   *  GTEXT_CSS_TRI_UNKNOWN for a feature it does not know. `value` is the written value as CSS text, or NULL for the boolean form. NULL: every such feature is
   *  unknown. */
  GTEXT_CSS_Tristate (*extension_feature)(void * user, const GTEXT_CSS_String * name, const GTEXT_CSS_String * value);
} GTEXT_CSS_Media_Environment;

/**
 * @brief The defaults: a screen baseline (a 1280 by 800 viewport and device, 1 dppx, 24-bit color, sRGB, `hover` and a fine pointer, light, no preference,
 *   scripting enabled, progressive scan), with every feature the baseline fills **supported**.
 *
 * @param env The computed-value environment to take the one source of truth from: the viewport (`dynamic_viewport`), `default_font_size`, the device
 *   pixel ratio (as `resolution`) and `color_scheme` (as `prefers_color_scheme`). NULL: the defaults of gtext_css_environment_default().
 */
GTEXT_API GTEXT_CSS_Media_Environment gtext_css_media_environment_default(const GTEXT_CSS_Environment * env);

/**
 * @brief Check an environment: a size or resolution that is not finite and positive, a keyword outside its enumeration, a font size that is not finite and
 *   positive, or a `size` too small for the header are refused.
 *
 * @param env The environment. Not NULL.
 * @param detail NULL, or receives a static sentence naming the field that is wrong.
 * @return GTEXT_CSS_OK, or GTEXT_CSS_E_INVALID.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_media_environment_check(const GTEXT_CSS_Media_Environment * env, const char ** detail);

/** @brief Say that the environment supports (or no longer supports) the media feature `name` (ASCII case-insensitive). Returns false for a name that is not a feature of the table. */
GTEXT_API bool gtext_css_media_environment_set_supported(GTEXT_CSS_Media_Environment * env, const char * name, bool supported);

/** @brief Whether the environment supports the media feature `name`; false for a name that is not a feature of the table. */
GTEXT_API bool gtext_css_media_environment_is_supported(const GTEXT_CSS_Media_Environment * env, const char * name);

/** @brief The kind of a media feature of the generated table. */
typedef enum { GTEXT_CSS_MEDIA_FEATURE_RANGE = 0, GTEXT_CSS_MEDIA_FEATURE_DISCRETE } GTEXT_CSS_Media_Feature_Kind;

/** @brief A row of the generated table of media features (tools/css/media-features.tsv). */
typedef struct {
  const char * name;                    ///< The feature, lower case.
  GTEXT_CSS_Media_Feature_Kind kind;    ///< Range or discrete.
  int level;                            ///< 4 or 5 (Media Queries), or 0 for another module (Window Management, Round Display, the Compatibility Standard).
  size_t keyword_count;                 ///< A keyword feature's keywords.
  bool implemented;                     ///< Evaluated here (false: parsed and kept, always unknown).
} GTEXT_CSS_Media_Feature_Row;

/** @brief The number of media features of the table. */
GTEXT_API size_t gtext_css_media_feature_count(void);

/** @brief Row `index` of the table (the bit of `supported`). false past the end. */
GTEXT_API bool gtext_css_media_feature_row(size_t index, GTEXT_CSS_Media_Feature_Row * out);

/** @brief The row of a feature by name (ASCII case-insensitive), or -1. */
GTEXT_API int gtext_css_media_feature_find(const char * name, size_t length);

/* ---- the media query list --------------------------------------------- */

/** @brief The parsed form of a `<media-query-list>`: immutable, reference counted, shareable. See the struct below. */
typedef struct GTEXT_CSS_Media_List GTEXT_CSS_Media_List;

/** @brief The kinds of diagnostic a parse keeps. */
typedef enum {
  GTEXT_CSS_MQDIAG_SYNTAX = 0,   ///< The query does not match the grammar: replaced by `not all`.
  GTEXT_CSS_MQDIAG_EMPTY,        ///< An empty query (a comma with nothing on one side): replaced by `not all`.
  GTEXT_CSS_MQDIAG_TYPE,         ///< A reserved word as a media type (`or and (color)`): replaced by `not all`.
  GTEXT_CSS_MQDIAG_FEATURE,      ///< A known feature written wrongly (a prefix on a discrete feature, a range of a discrete feature, a mixed range): replaced by `not all`.
  GTEXT_CSS_MQDIAG_VALUE,        ///< A known feature with a value its grammar does not allow: replaced by `not all`.
  GTEXT_CSS_MQDIAG_MIXED,        ///< `and` beside `or` without parentheses: replaced by `not all`.
  GTEXT_CSS_MQDIAG_UNKNOWN       ///< An unknown feature or a parenthesized thing the grammar does not read: kept as `<general-enclosed>`, evaluates unknown (not an error).
} GTEXT_CSS_Media_Diag_Kind;

/** @brief One diagnostic. */
typedef struct {
  GTEXT_CSS_Media_Diag_Kind kind; ///< What.
  GTEXT_CSS_Span span;            ///< Where, in the input.
  const char * reference;         ///< A static reference to the sentence of the draft that decides it ("Media Queries 4, 3.2"), never NULL.
} GTEXT_CSS_Media_Diagnostic;

/** @brief What a parse of media queries may do. Initialize it with gtext_css_media_options_default(). */
typedef struct {
  const GTEXT_Allocator * allocator; ///< NULL for the default; it makes the list and the result.
  size_t max_queries;                ///< Most queries in a list. Default 65,536; GTEXT_CSS_E_LIMIT_CONDITIONS.
  size_t max_nodes;                  ///< Most condition nodes in a list. Default 1,048,576; GTEXT_CSS_E_LIMIT_CONDITIONS.
  size_t max_depth;                  ///< Deepest nesting of conditions (`not (not (...))`, parentheses). Default 256; at most 2^24. GTEXT_CSS_E_LIMIT_CONDITION_DEPTH. The parser, the evaluator and the serializer do not recurse in the C stack at any depth.
  size_t max_diagnostics;            ///< Most diagnostics kept (the rest are counted). Default 1,024.
  GTEXT_CSS_Parse_Options parse;     ///< For the text entry point: the limits of tokenizing (css_core.h).
} GTEXT_CSS_Media_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Media_Options gtext_css_media_options_default(void);

/** @brief What a parse reports. Zero it before the first use; free it with gtext_css_media_result_free(). */
typedef struct {
  GTEXT_CSS_Status status;     ///< What the call returned.
  const char * message;        ///< A static description; never NULL after a call.
  GTEXT_CSS_Span failure;      ///< For a limit: where it was reached.
  size_t replaced;             ///< Queries replaced by `not all`.
  size_t diagnostic_total;     ///< Every diagnostic raised (more than the kept ones when there were many).
  struct GTEXT_CSS_Media_Result_Internal * internal; ///< Private: the kept diagnostics.
} GTEXT_CSS_Media_Result;

/** @brief Release what a result holds and reset it. Safe on NULL and on a zeroed result. */
GTEXT_API void gtext_css_media_result_free(GTEXT_CSS_Media_Result * result);

/** @brief How many diagnostics the result keeps. NULL is 0. */
GTEXT_API size_t gtext_css_media_result_diagnostic_count(const GTEXT_CSS_Media_Result * result);

/** @brief The `index`th kept diagnostic, in the order raised; false past the end. */
GTEXT_API bool gtext_css_media_result_diagnostic(const GTEXT_CSS_Media_Result * result, size_t index, GTEXT_CSS_Media_Diagnostic * out);

/**
 * @brief Parse a `<media-query-list>` from text: `matchMedia()`'s argument, the `media` attribute, an `@import` tail.
 *
 * "Parse a comma-separated list of component values, then parse each entry as a `<media-query>`." The empty text is the empty list, which is true.
 * An unclosed `(` ends at the end of the input, as CSS Syntax says (`(color` is `(color)`).
 *
 * @param text UTF-8; need not be terminated. NULL only with a `length` of 0.
 * @param length Its length in bytes.
 * @param options NULL for the defaults.
 * @param out Receives the list (one owner); written only on success. Not NULL.
 * @param result NULL, or filled whatever happens.
 * @return GTEXT_CSS_OK (also when queries were replaced by `not all`); GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; the limits of the tokenizer, GTEXT_CSS_E_LIMIT_CONDITIONS and
 *   GTEXT_CSS_E_LIMIT_CONDITION_DEPTH.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_media_list_parse(const char * text, size_t length, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Media_List ** out, GTEXT_CSS_Media_Result * result);

/**
 * @brief Parse a media query list from component values: the prelude of an `@media` rule, the media of an `@import`, the argument of `media()`.
 *
 * The values must outlive the call only; everything the list keeps is copied. @return As gtext_css_media_list_parse().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_media_list_parse_values(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Media_List ** out, GTEXT_CSS_Media_Result * result);

/**
 * @brief Parse one `<media-query>` (the `media()` of `@when`, or a single entry): a comma is a syntax error, and the query is the whole list.
 *
 * @return As gtext_css_media_list_parse(). The list holds exactly one query.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_media_query_parse_values(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Media_List ** out, GTEXT_CSS_Media_Result * result);

/** @brief Another owner of the list (it is freed when the last lets go). Returns `list`; NULL is ignored. */
GTEXT_API GTEXT_CSS_Media_List * gtext_css_media_list_retain(const GTEXT_CSS_Media_List * list);

/** @brief One owner fewer; the last frees the list. NULL is ignored. */
GTEXT_API void gtext_css_media_list_free(const GTEXT_CSS_Media_List * list);

/** @brief The number of queries (0 for the empty list). */
GTEXT_API size_t gtext_css_media_list_query_count(const GTEXT_CSS_Media_List * list);

/** @brief Whether query `index` was replaced by `not all` because it was invalid (an authored `not all` is not). */
GTEXT_API bool gtext_css_media_list_query_replaced(const GTEXT_CSS_Media_List * list, size_t index);

/** @brief The number of condition nodes in the list (queries included): the size the limit counts. */
GTEXT_API size_t gtext_css_media_list_node_count(const GTEXT_CSS_Media_List * list);

/**
 * @brief CSSOM's "serialize a media query list": the queries joined by `, `, the empty list as the empty string.
 *
 * Feature names and keywords are lower case, a prefixed name stays prefixed, a range keeps its range form, a ratio is `n / d` with both numbers, a math
 * function is serialized as a specified value is (`calc(3dppx)`), and `<general-enclosed>` as written. The output of a serialization parses to a list
 * that serializes to the same text.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_WRITE.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_media_list_serialize(const GTEXT_CSS_Media_List * list, const GTEXT_Markup_Sink * sink);

/** @brief gtext_css_media_list_serialize() to a new NUL-terminated buffer; release it with gtext_css_media_buffer_free(). */
GTEXT_API GTEXT_CSS_Status gtext_css_media_list_serialize_buffer(const GTEXT_CSS_Media_List * list, const GTEXT_Allocator * allocator, char ** out, size_t * out_length);

/** @brief Release a buffer from gtext_css_media_list_serialize_buffer(). */
GTEXT_API void gtext_css_media_buffer_free(const GTEXT_Allocator * allocator, char * buffer);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_MEDIA_H
