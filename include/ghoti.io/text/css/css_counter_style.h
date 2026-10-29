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
 * @file css_counter_style.h
 * @brief `@counter-style`, typed: the descriptors a rule wrote and the resolved `extends` and `fallback` chains. The generation algorithm is not here (a layout concern).
 *
 * Normative text: CSS Counter Styles 3 (the descriptors; 2.1 `extends`: "If a @counter-style uses the extends system, it must not contain a symbols or additive-symbols descriptor", and
 * "If the value of the extends descriptor ... forms a cycle, ... the counter styles in the cycle must instead extend the decimal counter style"), as served 2026-10-29.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_COUNTER_STYLE_H
#define GHOTI_IO_GTEXT_CSS_CSS_COUNTER_STYLE_H

#include <ghoti.io/text/css/css_atrule.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The `system` of a counter style. */
typedef enum {
  GTEXT_CSS_COUNTER_CYCLIC = 0, GTEXT_CSS_COUNTER_NUMERIC, GTEXT_CSS_COUNTER_ALPHABETIC, GTEXT_CSS_COUNTER_SYMBOLIC, GTEXT_CSS_COUNTER_FIXED, GTEXT_CSS_COUNTER_ADDITIVE,
  GTEXT_CSS_COUNTER_EXTENDS
} GTEXT_CSS_Counter_System;

/** @brief A symbol: a string (decoded), an identifier, or an image (as text). */
typedef struct {
  GTEXT_CSS_String text;  ///< The string decoded, or the identifier, or the serialized image.
  bool string;            ///< A string (else an identifier or an image).
} GTEXT_CSS_Counter_Symbol;

/** @brief One `range` pair; an `infinite` bound is flagged. */
typedef struct {
  bool lower_infinite;
  bool upper_infinite;
  int64_t lower;
  int64_t upper;
} GTEXT_CSS_Counter_Range;

/** @brief An additive tuple: a weight and a symbol. */
typedef struct {
  int64_t weight;
  GTEXT_CSS_Counter_Symbol symbol;
} GTEXT_CSS_Counter_Tuple;

/** @brief What a `@counter-style` rule wrote, with the chains resolved by the set it was found in. */
typedef struct GTEXT_CSS_Counter_Style {
  GTEXT_CSS_String name;                     ///< The counter style name (decoded).
  GTEXT_CSS_Counter_System system;
  int64_t fixed_first;                       ///< `fixed`'s first symbol value (1 when not written).
  GTEXT_CSS_String extends_name;             ///< `extends <name>` (data NULL: none).
  bool has_negative;
  GTEXT_CSS_Counter_Symbol negative_prefix;  ///< Initially `"-"`.
  GTEXT_CSS_Counter_Symbol negative_suffix;
  bool has_prefix;
  GTEXT_CSS_Counter_Symbol prefix;
  bool has_suffix;
  GTEXT_CSS_Counter_Symbol suffix;           ///< Initially `". "`.
  bool range_auto;                           ///< `range: auto` or not written.
  const GTEXT_CSS_Counter_Range * ranges;
  size_t range_count;
  uint32_t pad_length;
  GTEXT_CSS_Counter_Symbol pad_symbol;
  GTEXT_CSS_String fallback;                 ///< The fallback style name (`decimal` when not written).
  const GTEXT_CSS_Counter_Symbol * symbols;
  size_t symbol_count;
  const GTEXT_CSS_Counter_Tuple * additive;
  size_t additive_count;
  GTEXT_CSS_String speak_as;                 ///< The `speak-as` value as text (`auto` when not written).
  bool valid;                                ///< The symbol counts and weights are valid for the system ("defines nothing" otherwise).
  /* set by gtext_css_atrule_set_counter_style(): */
  GTEXT_CSS_Counter_System resolved_system;  ///< The system after following `extends` to a base (a cycle: decimal's `numeric`).
  const struct GTEXT_CSS_Counter_Style * base;  ///< The style that supplies the symbols (NULL: this one, or the built-in decimal on a cycle or an unknown base).
  bool extends_cycle;                        ///< `extends` forms a cycle: the style extends `decimal`.
  bool fallback_cycle;                       ///< The `fallback` chain returns to a style already in it: cut, as the draft says, to `decimal`.
  const struct GTEXT_CSS_Counter_Style * fallback_style;  ///< The style `fallback` names (NULL: the built-in `decimal`, or none found).
  const GTEXT_CSS_Atrule * rule;
  void * priv_;
} GTEXT_CSS_Counter_Style;

/** @brief Read a `@counter-style` rule; the chain fields are zero. @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_DESCRIPTORS. */
GTEXT_API GTEXT_CSS_Status gtext_css_counter_style_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Counter_Style ** out);

/** @brief Release a style made by gtext_css_counter_style_from_atrule(); NULL is ignored. Styles found in a set belong to it. */
GTEXT_API void gtext_css_counter_style_free(GTEXT_CSS_Counter_Style * style);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_COUNTER_STYLE_H
