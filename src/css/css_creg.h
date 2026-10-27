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
 * A custom property registration (CSS Properties and Values API 1, 2.1): its name, syntax, inherit flag and initial value, with the syntax parsed
 * and compiled once. Private. The registry holds the ones a caller registers; a set of `@property` rules (css_at_property.h) holds the ones a
 * stylesheet makes; a compute context reads both through this struct.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_CREG_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_CREG_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/css/css_at_property.h>

#include "css_syntax_string.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Why a registration was refused as invalid (GTEXT_CSS_E_INVALID), for the diagnostics of an `@property` rule.
typedef enum {
  CSS_CREG_OK = 0,
  CSS_CREG_BAD_NAME,
  CSS_CREG_BAD_SYNTAX,
  CSS_CREG_NO_INITIAL,         ///< A syntax other than `*` with no initial value.
  CSS_CREG_BAD_INITIAL,        ///< The initial value is not a value of the syntax (or not a token stream).
  CSS_CREG_SUBSTITUTION,       ///< The initial value has a substitution function.
  CSS_CREG_NOT_INDEPENDENT,    ///< The initial value needs the element.
  CSS_CREG_OTHER
} css_creg_why;

typedef struct css_creg {
  GTEXT_Allocator alloc;
  char * name;                     ///< `--name`, case kept, NUL-terminated.
  size_t name_len;
  char * syntax_text;              ///< The syntax string exactly as it was given (`*` for none).
  size_t syntax_len;
  css_syntax * syntax;
  GTEXT_CSS_Compiled ** compiled;  ///< `syntax->ncomps` entries; NULL for an identifier component. NULL for the universal syntax.
  bool inherits;
  bool has_initial;                ///< An initial value was given (else it is the guaranteed-invalid value).
  char * initial;                  ///< Its text, white space trimmed (NUL-terminated; empty for an empty value).
  size_t initial_len;
} css_creg;

/// A syntax string compiled for matching, outside any registration (the `type()` of an `attr()`).
typedef struct {
  GTEXT_Allocator alloc;
  char * text;
  size_t len;
  css_syntax * syntax;
  GTEXT_CSS_Compiled ** compiled;  ///< `syntax->ncomps` entries (NULL for an identifier component); NULL for the universal syntax.
} css_compiled_syntax;

void css_compiled_syntax_free(css_compiled_syntax * s);

/// Free a registration; NULL is ignored.
void css_creg_free(css_creg * c);

/// A table of registrations by name (case-sensitive): an open-addressing hash that owns them.
typedef struct {
  GTEXT_Allocator alloc;
  css_creg ** items;   ///< In the order they were added.
  size_t count, cap;
  size_t * slots;      ///< index + 1, 0 empty.
  size_t slot_cap;
} css_creg_store;

void css_creg_store_init(css_creg_store * s, const GTEXT_Allocator * alloc);
void css_creg_store_free(css_creg_store * s);
/// The registration with this name, or NULL. `work` (may be NULL) is increased by the probes made.
const css_creg * css_creg_store_find(const css_creg_store * s, const char * name, size_t length, size_t * work);
/// Add one (the store owns it on success). GTEXT_CSS_OK, GTEXT_CSS_E_EXISTS (the name is there; not added) or GTEXT_CSS_E_OOM.
GTEXT_CSS_Status css_creg_store_add(css_creg_store * s, css_creg * c);

/// Put `c` in the place of the registration of the same name, freeing that one (the store owns `c`); or add it when there is none. `*replaced` says which.
/// GTEXT_CSS_OK or GTEXT_CSS_E_OOM (nothing changed).
GTEXT_CSS_Status css_creg_store_put(css_creg_store * s, css_creg * c, bool * replaced);

/// Whether a typed initial value can be turned into a computed value from the value alone and information that CSS cannot change (Properties and Values 4.1:
/// "5px is computationally independent ... 1in is ... On the other hand, 3em is not ... Neither is a value with a var() function"): no font-relative or
/// container-relative unit, no `currentcolor`, no color that depends on the element, no substitution.
GTEXT_CSS_Status css_creg_independent(const GTEXT_Allocator * alloc, const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Value_Set * typed,
    bool * independent);

/// Whether a string is a custom property name string: it begins `--` and has something after.
bool css_creg_name_ok(const char * name, size_t length);

/// The registrations of the rules of a set (NULL for NULL), for the compute context.
const css_creg_store * css_at_property_store(const GTEXT_CSS_At_Property_Set * set);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_CREG_H
