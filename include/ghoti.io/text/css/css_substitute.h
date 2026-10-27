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
 * @file css_substitute.h
 * @brief Arbitrary substitution functions: `var()`, `env()`, `attr()`, `if()`, `inherit()`, `ident()`, `random-item()`, `random()` and the tree counting
 *   functions, resolved when a style is computed, with the budget that keeps them from being used to amplify a few bytes into billions of tokens.
 *
 * Reference: CSS Values and Units Level 5, Appendix A ("Arbitrary Substitution Functions": substitution contexts, spread syntax, resolving in properties,
 * invalid substitution, substitution in shorthands, "Safely Handling Overly-Long Substitution") and 8.3 to 8.8, 9 (`if()`, `cycle()`, `var()`, `inherit()`,
 * `attr()`, `ident()`, `random()`, `random-item()`); CSS Custom Properties for Cascading Variables Level 1; CSS Environment Variables Level 1; CSS Conditional
 * Level 5 (`style()`), all as served 2026-10-27. Each rule is quoted in the test that checks it.
 *
 * ## What happens when
 *
 * A declaration that holds one of these functions is **pending** from the moment it is parsed (css_property.h): valid, and meaningless until a style is
 * computed. gtext_css_compute_style() (css_computed.h) is where they are replaced, and nothing else needs to be called:
 *
 * 1. The custom properties the element declares are resolved first, since any other property may refer to them: each one's value has its functions replaced
 *    (with a cycle among them making every property in it the guaranteed-invalid value), and a registered one (css_at_property.h, css_registry.h) is checked against
 *    its syntax and computed by the rules of its type.
 * 2. A pending ordinary property has its functions replaced; if that left the guaranteed-invalid value, or text that is not a value of the property, it is **invalid
 *    at computed-value time**, which is `unset` ("the property's inherited value or its initial value ... as if the property's value had been specified as the unset
 *    keyword"). Otherwise the text is parsed as the property's value and computed as any value is. A shorthand that held a function is replaced and parsed as a whole
 *    and then expanded to its longhands (css_shorthand.h).
 *
 * ## The order of replacement
 *
 * Algorithm and contexts follow Appendix A step by step. The arguments of a function are divided before anything in them is replaced ("the contents are
 * divided into arguments based only on the values literally present inside the function"), except that a function written directly after `...` (no white space
 * between) is replaced first, in the contents, and the division is made on what that gave. A replacement that leaves a guaranteed-invalid value in its place makes
 * what contains it guaranteed-invalid. Nothing recurses: a chain of a hundred thousand custom properties is a hundred thousand frames of an explicit stack.
 *
 * ## The budget
 *
 * "UAs must impose a UA-defined limit on the allowed length of the token stream that an arbitrary substitution function expands into. If an arbitrary substitution
 * function would expand into a longer token stream than this limit, it instead is replaced with the guaranteed-invalid value." Substituted values here are ropes
 * (css_custom.h): memory is linear in the text written, and the length each would have is a saturating counter. A value that would have more tokens than
 * `max_substituted_tokens` is the guaranteed-invalid value, and so is any value once the values substituted for one element have used `max_element_substituted_tokens`
 * between them. The result of a computation reports the counters (GTEXT_CSS_Compute_Result::substituted_tokens and ::substitution_limited), and the quadratic suite asserts on
 * them and not on the clock. Also bounded, each with a status of its own: the custom properties one element declares, the bytes of one value, and the depth
 * of nested replacement.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_SUBSTITUTE_H
#define GHOTI_IO_GTEXT_CSS_CSS_SUBSTITUTE_H

#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct GTEXT_CSS_Substitute_Options
 * @brief The limits of substitution. A field left at 0 selects its default; 0 never means unlimited. A zeroed struct is the default.
 */
typedef struct {
  /** Most tokens one substituted value may expand into (CSS Values 5 recommends "relatively high"). Default 1,048,576; at most 2^40. A value that would have more is
   *  the guaranteed-invalid value, and is counted in GTEXT_CSS_Compute_Result::substitution_limited. */
  uint64_t max_substituted_tokens;
  /** Most tokens that all the values substituted for one element may have between them (each custom property's computed value, and each ordinary property's value
   *  with its functions replaced, counted once). Default 16,777,216; at most 2^44. */
  uint64_t max_element_substituted_tokens;
  /** Most custom properties one element may declare. Default 65,536; at most 2^24. One more is GTEXT_CSS_E_LIMIT_CUSTOM_PROPERTIES. */
  size_t max_custom_properties;
  /** Longest a value may be once its functions are replaced, in bytes (a declared value as well). Default 16 MiB; at most 2^30. A longer substituted value is the
   *  guaranteed-invalid value, counted like one that passes the token budget; a declared value longer than this is GTEXT_CSS_E_LIMIT_CUSTOM_VALUE. */
  size_t max_value_bytes;
  /** Deepest nesting of replacement: functions inside functions, and custom properties that refer to custom properties. Default 100,000; at most 2^24. One more is
   *  GTEXT_CSS_E_LIMIT_DEPTH. */
  size_t max_substitution_depth;
} GTEXT_CSS_Substitute_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Substitute_Options gtext_css_substitute_options_default(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_SUBSTITUTE_H
