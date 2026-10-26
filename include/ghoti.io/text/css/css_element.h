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
 * @file css_element.h
 * @brief The caller's tree and the caller's environment: what computing a style needs to know that the library cannot read.
 *
 * ## The element interface
 *
 * A @ref GTEXT_CSS_Element_Interface is a table of callbacks over the caller's own tree (a Markup tree through an adapter, an SVG
 * document, a browser's DOM). The library never owns, copies or walks a tree except through it. An element is an opaque
 * @ref GTEXT_CSS_Element_Handle; NULL is "no element". The table's first field is its own size, so a caller built against an older
 * header passes a smaller table and one built against a newer header a larger one: a callback outside the size the caller gave is
 * the same as a NULL one.
 *
 * A callback that a computation needs and the table does not have is never worked around: the call returns GTEXT_CSS_E_UNSUPPORTED
 * and the result names the callback (gtext_css_element_callback_name()).
 *
 * `inheritance_parent` is a callback of its own, apart from `parent`. Inheritance does not follow the tree parent where the tree is
 * not the whole story: an SVG `<use>` clone inherits from the `<use>` element, and the elements of a shadow tree inherit through the
 * flat tree. When the table has no `inheritance_parent`, the inheritance parent is the parent.
 *
 * ## The environment
 *
 * A @ref GTEXT_CSS_Environment is everything else that a computed value depends on and the document does not say: the viewport sizes
 * (small, large and dynamic), the size that `medium` is, the device pixel ratio, the color scheme, the language, the base URL, and
 * callbacks for the quantities that only a font, a layout or a color engine knows (the font-relative lengths `ex ch cap ic` and
 * their root forms, the `normal` line height, container query lengths, and colors that need conversion). **Nothing reads the
 * process, the clock or the network.** A style is a function of the declarations, the parent's style and this struct.
 *
 * The callbacks are optional. A computation that needs one the environment lacks fails with GTEXT_CSS_E_UNSUPPORTED naming the unit
 * or the quantity (`ex`, `cqw`, `lh`); it never guesses and never returns part of a style. The color callback is the one that is not
 * an error: a color the library cannot compute without conversion stays what it was written, flagged
 * @ref GTEXT_CSS_VFLAG_COLOR_UNRESOLVED, because `text` does not convert colors (that is the `color` library's).
 *
 * Callbacks are called on the caller's thread, from inside the computation, and must not call back into the same computation.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_ELEMENT_H
#define GHOTI_IO_GTEXT_CSS_CSS_ELEMENT_H

#include <ghoti.io/text/css/css_color.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_value.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An element of the caller's tree. Opaque to the library; NULL is none. */
typedef const void * GTEXT_CSS_Element_Handle;

/** @brief The direction of text, for the `direction` callback and for the logical mappings. */
typedef enum {
  GTEXT_CSS_DIRECTION_LTR = 0, ///< Left to right.
  GTEXT_CSS_DIRECTION_RTL      ///< Right to left.
} GTEXT_CSS_Direction;

/** @name Element state flags
 *  Bits of the value the `state` callback returns: the dynamic pseudo-classes (Selectors 4) that the caller owns.
 *  @{ */
#define GTEXT_CSS_STATE_HOVER 0x1u
#define GTEXT_CSS_STATE_ACTIVE 0x2u
#define GTEXT_CSS_STATE_FOCUS 0x4u
#define GTEXT_CSS_STATE_FOCUS_VISIBLE 0x8u
#define GTEXT_CSS_STATE_FOCUS_WITHIN 0x10u
#define GTEXT_CSS_STATE_TARGET 0x20u
#define GTEXT_CSS_STATE_LINK 0x40u
#define GTEXT_CSS_STATE_VISITED 0x80u
#define GTEXT_CSS_STATE_CHECKED 0x100u
#define GTEXT_CSS_STATE_INDETERMINATE 0x200u
#define GTEXT_CSS_STATE_DISABLED 0x400u
#define GTEXT_CSS_STATE_ENABLED 0x800u
#define GTEXT_CSS_STATE_REQUIRED 0x1000u
#define GTEXT_CSS_STATE_OPTIONAL 0x2000u
#define GTEXT_CSS_STATE_READ_ONLY 0x4000u
#define GTEXT_CSS_STATE_READ_WRITE 0x8000u
#define GTEXT_CSS_STATE_PLACEHOLDER_SHOWN 0x10000u
#define GTEXT_CSS_STATE_DEFAULT 0x20000u
#define GTEXT_CSS_STATE_VALID 0x40000u
#define GTEXT_CSS_STATE_INVALID 0x80000u
#define GTEXT_CSS_STATE_IN_RANGE 0x100000u
#define GTEXT_CSS_STATE_OUT_OF_RANGE 0x200000u
#define GTEXT_CSS_STATE_FULLSCREEN 0x400000u
#define GTEXT_CSS_STATE_MODAL 0x800000u
#define GTEXT_CSS_STATE_OPEN 0x1000000u
#define GTEXT_CSS_STATE_DEFINED 0x2000000u
/** @} */

/** @brief The callbacks of the element interface, by number (for the errors that name one). */
typedef enum {
  GTEXT_CSS_ELEMENT_CB_PARENT = 0,
  GTEXT_CSS_ELEMENT_CB_INHERITANCE_PARENT,
  GTEXT_CSS_ELEMENT_CB_LOCAL_NAME,
  GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI,
  GTEXT_CSS_ELEMENT_CB_ID,
  GTEXT_CSS_ELEMENT_CB_CLASS_COUNT,
  GTEXT_CSS_ELEMENT_CB_CLASS_AT,
  GTEXT_CSS_ELEMENT_CB_ATTRIBUTE,
  GTEXT_CSS_ELEMENT_CB_PREVIOUS_SIBLING,
  GTEXT_CSS_ELEMENT_CB_NEXT_SIBLING,
  GTEXT_CSS_ELEMENT_CB_SIBLING_INDEX,
  GTEXT_CSS_ELEMENT_CB_SIBLING_COUNT,
  GTEXT_CSS_ELEMENT_CB_COMPARE_ORDER,
  GTEXT_CSS_ELEMENT_CB_STATE,
  GTEXT_CSS_ELEMENT_CB_LANG,
  GTEXT_CSS_ELEMENT_CB_DIRECTION,
  GTEXT_CSS_ELEMENT_CB_COUNT ///< The number of callbacks; not one.
} GTEXT_CSS_Element_Callback;

/**
 * @struct GTEXT_CSS_Element_Interface
 * @brief The table of callbacks over the caller's tree. Zero it, then set `size` and what the tree can answer.
 *
 * Every string a callback returns must stay valid until the next call of a callback of the same table on the same thread, which is
 * long enough for the library to copy or compare it.
 */
typedef struct {
  /** `sizeof` this struct as the caller's header has it. Required, and first so that the struct can grow. */
  size_t size;
  /** Passed to every callback. */
  void * user;
  /** The tree parent, or NULL for the root. */
  GTEXT_CSS_Element_Handle (*parent)(void * user, GTEXT_CSS_Element_Handle element);
  /** The element that `element` inherits from, or NULL for the root of inheritance. When the table has none, the parent. */
  GTEXT_CSS_Element_Handle (*inheritance_parent)(void * user, GTEXT_CSS_Element_Handle element);
  /** The local name (`div`, `rect`). Returns false if the element has none. */
  bool (*local_name)(void * user, GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out);
  /** The namespace URI, or false for none. */
  bool (*namespace_uri)(void * user, GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out);
  /** The id attribute; false if none. */
  bool (*id)(void * user, GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out);
  /** How many classes the element has. */
  size_t (*class_count)(void * user, GTEXT_CSS_Element_Handle element);
  /** The `index`th class, `index` below the count; false if there is none. */
  bool (*class_at)(void * user, GTEXT_CSS_Element_Handle element, size_t index, GTEXT_CSS_String * out);
  /** The value of an attribute. `ns` is NULL for no namespace. false if the element has no such attribute. */
  bool (*attribute)(void * user, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_String * ns, const GTEXT_CSS_String * name,
      GTEXT_CSS_String * out);
  /** The previous sibling element, or NULL. */
  GTEXT_CSS_Element_Handle (*previous_sibling)(void * user, GTEXT_CSS_Element_Handle element);
  /** The next sibling element, or NULL. */
  GTEXT_CSS_Element_Handle (*next_sibling)(void * user, GTEXT_CSS_Element_Handle element);
  /** The element's position among its siblings, counting from 1 (`sibling-index()`). */
  size_t (*sibling_index)(void * user, GTEXT_CSS_Element_Handle element);
  /** How many sibling elements it has, itself included (`sibling-count()`). */
  size_t (*sibling_count)(void * user, GTEXT_CSS_Element_Handle element);
  /** Document order: negative if `a` precedes `b`, positive if it follows, 0 if they are the same element. */
  int (*compare_order)(void * user, GTEXT_CSS_Element_Handle a, GTEXT_CSS_Element_Handle b);
  /** The element's dynamic state: GTEXT_CSS_STATE_* bits. */
  uint64_t (*state)(void * user, GTEXT_CSS_Element_Handle element);
  /** The language of the element (the `lang` of it or its ancestors, resolved by the caller); false if unknown. */
  bool (*lang)(void * user, GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out);
  /** The directionality of the element (`:dir()`), resolved by the caller. */
  GTEXT_CSS_Direction (*direction)(void * user, GTEXT_CSS_Element_Handle element);
} GTEXT_CSS_Element_Interface;

/** @brief What a call that uses the table reports. Plain data; filled whatever happens. */
typedef struct {
  GTEXT_CSS_Status status;   ///< What the call returned.
  const char * message;      ///< A static description; never NULL after a call.
  const char * callback;     ///< For GTEXT_CSS_E_UNSUPPORTED: the name of the callback that was needed and absent (static); else NULL.
} GTEXT_CSS_Element_Result;

/** @brief The name of a callback ("parent", "inheritance_parent", ...); "unknown" for a number that is not one. Static. */
GTEXT_API const char * gtext_css_element_callback_name(GTEXT_CSS_Element_Callback callback);

/** @brief Whether the table has a callback: it is there, its `size` reaches the field and the field is not NULL. NULL has none. */
GTEXT_API bool gtext_css_element_has(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Callback callback);

/**
 * @brief Check the table: it is there, its `size` holds at least `size` and `user`, and the callbacks named are present.
 *
 * `inheritance_parent` is satisfied by a `parent`.
 *
 * @param iface The table.
 * @param required The callbacks a computation will use; NULL with a count of 0.
 * @param count Their number.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (no table, a size too small, a number that is not a callback); GTEXT_CSS_E_UNSUPPORTED (a
 *   required callback is absent; the result names the first).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_element_check(const GTEXT_CSS_Element_Interface * iface,
    const GTEXT_CSS_Element_Callback * required, size_t count, GTEXT_CSS_Element_Result * result);

/** @name Calling the table
 *  Each is the callback guarded: GTEXT_CSS_E_UNSUPPORTED naming it when it is absent, GTEXT_CSS_E_INVALID for a NULL output, GTEXT_CSS_OK
 *  otherwise. The output is written only on GTEXT_CSS_OK. A string output is false-valued (`*found` false, `out->data` NULL) when the
 *  element has no such thing. `result` may be NULL.
 *  @{ */
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_parent(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    GTEXT_CSS_Element_Handle * out, GTEXT_CSS_Element_Result * result);
/** The inheritance parent: the `inheritance_parent` callback, else `parent`; E_UNSUPPORTED naming "parent" when the table has neither. */
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_inheritance_parent(const GTEXT_CSS_Element_Interface * iface,
    GTEXT_CSS_Element_Handle element, GTEXT_CSS_Element_Handle * out, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_local_name(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    GTEXT_CSS_String * out, bool * found, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_namespace_uri(const GTEXT_CSS_Element_Interface * iface,
    GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out, bool * found, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_id(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    GTEXT_CSS_String * out, bool * found, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_class_count(const GTEXT_CSS_Element_Interface * iface,
    GTEXT_CSS_Element_Handle element, size_t * out, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_class_at(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    size_t index, GTEXT_CSS_String * out, bool * found, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_attribute(const GTEXT_CSS_Element_Interface * iface,
    GTEXT_CSS_Element_Handle element, const GTEXT_CSS_String * ns, const GTEXT_CSS_String * name, GTEXT_CSS_String * out,
    bool * found, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_previous_sibling(const GTEXT_CSS_Element_Interface * iface,
    GTEXT_CSS_Element_Handle element, GTEXT_CSS_Element_Handle * out, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_next_sibling(const GTEXT_CSS_Element_Interface * iface,
    GTEXT_CSS_Element_Handle element, GTEXT_CSS_Element_Handle * out, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_sibling_index(const GTEXT_CSS_Element_Interface * iface,
    GTEXT_CSS_Element_Handle element, size_t * out, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_sibling_count(const GTEXT_CSS_Element_Interface * iface,
    GTEXT_CSS_Element_Handle element, size_t * out, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_compare_order(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle a,
    GTEXT_CSS_Element_Handle b, int * out, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_state(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    uint64_t * out, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_lang(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    GTEXT_CSS_String * out, bool * found, GTEXT_CSS_Element_Result * result);
GTEXT_API GTEXT_CSS_Status gtext_css_element_get_direction(const GTEXT_CSS_Element_Interface * iface,
    GTEXT_CSS_Element_Handle element, GTEXT_CSS_Direction * out, GTEXT_CSS_Element_Result * result);
/** @} */

/* ---- the environment ------------------------------------------------------------------------------ */

/** @brief The color scheme the user prefers (for `light-dark()` and for system colors). */
typedef enum {
  GTEXT_CSS_SCHEME_LIGHT = 0, ///< `light`
  GTEXT_CSS_SCHEME_DARK       ///< `dark`
} GTEXT_CSS_Color_Scheme;

/** @brief A size in CSS pixels. */
typedef struct {
  double width;  ///< Horizontal extent, positive and finite.
  double height; ///< Vertical extent, positive and finite.
} GTEXT_CSS_Size;

/** @brief A font-relative quantity that only a font knows. */
typedef enum {
  GTEXT_CSS_METRIC_EX = 0, ///< The x-height (`ex`, `rex`).
  GTEXT_CSS_METRIC_CH,     ///< The advance of "0" (`ch`, `rch`).
  GTEXT_CSS_METRIC_CAP,    ///< The cap height (`cap`, `rcap`).
  GTEXT_CSS_METRIC_IC      ///< The advance of U+6C34 (`ic`, `ric`).
} GTEXT_CSS_Font_Metric;

/**
 * @struct GTEXT_CSS_Font_Query
 * @brief The font a metric is asked of: the computed values of the font properties of the element (or of the root, for the `r` units).
 */
typedef struct {
  GTEXT_CSS_Element_Handle element; ///< The element the style is for (NULL for the root's query when the caller gave none).
  const GTEXT_CSS_Value * family;   ///< The computed `font-family` (a value of css_value.h), or NULL when it has none.
  double size_px;                   ///< The computed `font-size`.
  double weight;                    ///< The computed `font-weight`.
  double width_percent;             ///< The computed `font-width`, a percentage (100 is normal).
  bool italic;                      ///< `font-style` is `italic` or `oblique`.
  double oblique_degrees;           ///< The angle of `oblique`, else 0.
  bool vertical;                    ///< The writing mode is vertical.
} GTEXT_CSS_Font_Query;

/** @brief The axis a container query length is asked for. */
typedef enum {
  GTEXT_CSS_CONTAINER_WIDTH = 0, ///< `cqw`
  GTEXT_CSS_CONTAINER_HEIGHT,    ///< `cqh`
  GTEXT_CSS_CONTAINER_INLINE,    ///< `cqi`
  GTEXT_CSS_CONTAINER_BLOCK      ///< `cqb`
} GTEXT_CSS_Container_Axis;

/**
 * @struct GTEXT_CSS_Environment
 * @brief What a style depends on that no document says. Initialize it with gtext_css_environment_default(), then set what differs.
 *
 * Lengths are CSS pixels. The struct is copied by the computation context, so it may change or go away afterwards, but `user` and the
 * strings must outlive the context.
 */
typedef struct {
  size_t size;                       ///< `sizeof` this struct as the caller's header has it; set by gtext_css_environment_default().
  GTEXT_CSS_Size small_viewport;     ///< `svw`, `svh`: the viewport with the dynamic toolbars shown. Default 1280 by 800.
  GTEXT_CSS_Size large_viewport;     ///< `vw`, `vh`, `lvw`, `lvh`: the viewport with them hidden. Default 1280 by 800.
  GTEXT_CSS_Size dynamic_viewport;   ///< `dvw`, `dvh`: the viewport as it is now. Default 1280 by 800.
  double default_font_size;          ///< What `medium` is, in px. Default 16.
  double device_pixel_ratio;         ///< Device pixels per CSS pixel (a border width is snapped to whole ones). Default 1.
  GTEXT_CSS_Color_Scheme color_scheme; ///< Chooses a side of `light-dark()`. Default light.
  const char * language;             ///< The language of the document (BCP 47), or NULL for none.
  const char * base_url;             ///< The URL that a relative `url()` is resolved against, or NULL for none (then a relative URL stays one).
  const char * default_font_family;  ///< The initial value of `font-family`, as CSS text; NULL for `serif`. (The standard leaves it to the user agent.)
  void * user;                       ///< Passed to every callback below.
  /** The size of the query container for an axis, in px, or false if the element has none. NULL: a container query length is
   *  GTEXT_CSS_E_UNSUPPORTED naming its unit. */
  bool (*container_size)(void * user, GTEXT_CSS_Element_Handle element, GTEXT_CSS_Container_Axis axis, double * px);
  /** A metric of a font, in px at the query's size. Returns false if the font has none. NULL: `ex`, `ch`, `cap`, `ic` and the `r` forms are
   *  GTEXT_CSS_E_UNSUPPORTED naming the unit. */
  bool (*font_metric)(void * user, const GTEXT_CSS_Font_Query * query, GTEXT_CSS_Font_Metric metric, double * px);
  /** The used `line-height` of `normal`, in px. Returns false if the font has none. NULL: `lh` and `rlh` of a `normal` line height are
   *  GTEXT_CSS_E_UNSUPPORTED naming the unit. */
  bool (*normal_line_height)(void * user, const GTEXT_CSS_Font_Query * query, double * px);
  /** Resolve a color that needs conversion, or the system color, to sRGB: red, green and blue 0 to 255, alpha 0 to 1. Called for system colors,
   *  `color-mix()`, relative colors, `contrast-color()`, `alpha()` and `device-cmyk()`, after `light-dark()` is resolved. Returns false
   *  if the color cannot be resolved. NULL: such a color stays as written, flagged GTEXT_CSS_VFLAG_COLOR_UNRESOLVED. */
  bool (*resolve_color)(void * user, const GTEXT_CSS_Color * color, GTEXT_CSS_Color_Scheme scheme, double rgba[4]);
  /** Resolve a relative URL against `base_url` as the caller's URL parser does (the library's own resolution is RFC 3986's). Return a new string
   *  in `out` valid until the next call; false to keep the URL as it was. NULL: the library's own. */
  bool (*resolve_url)(void * user, const char * base, size_t base_length, const char * url, size_t url_length, GTEXT_CSS_String * out);
} GTEXT_CSS_Environment;

/** The size of GTEXT_CSS_Environment through its `resolve_url` field, which is the one a caller who sets that field has. */
#define GTEXT_CSS_ENVIRONMENT_SIZE (sizeof(GTEXT_CSS_Environment))

/** @brief The defaults, spelled out (a 1280 by 800 viewport, 16px `medium`, a device pixel ratio of 1, light, no callbacks). */
GTEXT_API GTEXT_CSS_Environment gtext_css_environment_default(void);

/**
 * @brief Check an environment: sizes positive and finite, a default font size positive and finite, a device pixel ratio positive and finite.
 *
 * @param env The environment. Not NULL.
 * @param detail NULL, or receives a static sentence naming the field that is wrong.
 * @return GTEXT_CSS_OK, or GTEXT_CSS_E_INVALID.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_environment_check(const GTEXT_CSS_Environment * env, const char ** detail);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_ELEMENT_H
