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
 * Syntax strings of registered custom properties (CSS Properties and Values API 1, 5; served 2026-10-27). Private.
 *
 * A syntax string is a restricted front end to the Value Definition Syntax: data type names in angle brackets, the multipliers `+` and `#`,
 * `|` between components, an identifier standing for itself, and `*` alone for any token stream. Parsing one is the draft's "consume a syntax
 * definition"; a registration compiles each component with the grammar engine (a data type, with its multiplier) and matches a value against the
 * components in written order, the first that matches the whole value deciding it ("the syntax components are matched in the order specified").
 *
 * An identifier component is matched here and not by the grammar engine, because the draft compares it code point by code point ("`Red` ... red,
 * RED, and any other casing variants are not matched by this") and the grammar engine's keywords are ASCII case-insensitive.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_SYNTAX_STRING_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_SYNTAX_STRING_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// One component of a syntax definition.
typedef struct {
  bool is_type;      ///< A data type name (`<length>`), else an identifier.
  char mult;         ///< 0, '+' (a space-separated list) or '#' (a comma-separated list).
  char * name;       ///< The data type's name without the brackets (`length-percentage`; `transform-list` is `transform-function` with `+`), or the identifier (unescaped). NUL-terminated.
  size_t name_len;
} css_syn_comp;

/// A syntax definition: the universal one, or a list of components.
typedef struct {
  GTEXT_Allocator alloc;
  bool universal;
  css_syn_comp * comps;
  size_t ncomps;
} css_syntax;

/**
 * Consume a syntax definition from a string.
 *
 * @return GTEXT_CSS_OK with `*out` set; GTEXT_CSS_E_GRAMMAR (the string is not a syntax definition; `*offset`, if not NULL, is the byte where it
 *   went wrong); GTEXT_CSS_E_OOM; or a limit of the tokenizer.
 */
GTEXT_CSS_Status css_syntax_parse(const GTEXT_Allocator * alloc, const char * text, size_t length, css_syntax ** out, size_t * offset);

void css_syntax_free(css_syntax * syntax);

/// The grammar of component `i` in the Value Definition Syntax (`<length>#`), as a new NUL-terminated string (release with the allocator). A component that is an
/// identifier has none (NULL, status OK).
GTEXT_CSS_Status css_syntax_component_grammar(const css_syntax * syntax, size_t i, char ** out, size_t * length);

/// A matched value of a syntax: the component that decided, and the typed value (NULL for an identifier component).
typedef struct {
  size_t component;
  GTEXT_CSS_Value_Set * set;
} css_syn_match;

/**
 * Match a list of component values against the definition. `compiled[i]` is the compiled grammar of component `i` (NULL for an identifier). The universal
 * definition matches nothing here: it takes any token stream and the caller checks that.
 *
 * @return GTEXT_CSS_OK (`*matched` says whether one component matched whole; if so `m` is filled, `m->set` owned by the caller); GTEXT_CSS_E_OOM and the limits
 *   of the match.
 */
GTEXT_CSS_Status css_syntax_match(const css_syntax * syntax, GTEXT_CSS_Compiled * const * compiled, const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Match_Options * options, bool * matched, css_syn_match * m);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_SYNTAX_STRING_H
