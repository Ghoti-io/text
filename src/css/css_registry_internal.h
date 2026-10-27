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
 * What css_property.c needs of the registry beyond its public header. Private.
 */

#ifndef GHOTI_IO_GTEXT_CSS_REGISTRY_INTERNAL_H
#define GHOTI_IO_GTEXT_CSS_REGISTRY_INTERNAL_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/css/css_registry.h>

#include "css_creg.h"

/**
 * @brief The compiled grammar of a property (an alias's target's), compiling it first if need be.
 *
 * @param state Receives the state (GTEXT_CSS_GRAMMAR_OK when `compiled` is set).
 * @param compiled Receives the grammar, or NULL when there is none to match.
 * @param grammar_result NULL, or receives the report of the compile.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (an id the registry does not have); GTEXT_CSS_E_OOM.
 */
GTEXT_CSS_Status css_registry_property_compiled(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id,
    GTEXT_CSS_Grammar_State * state, const GTEXT_CSS_Compiled ** compiled, GTEXT_CSS_Grammar_Result * grammar_result);

/** @brief As css_registry_property_compiled(), for a descriptor of the generated layer. */
GTEXT_CSS_Status css_registry_descriptor_compiled(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Descriptor_Id id,
    GTEXT_CSS_Grammar_State * state, const GTEXT_CSS_Compiled ** compiled, GTEXT_CSS_Grammar_Result * grammar_result);

/** @brief The grammar `<declaration-value>?` compiled, which a value with a substitution function in it is matched against. */
GTEXT_CSS_Status css_registry_pending_grammar(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Compiled ** compiled);

/**
 * @brief Whether a grammar reference resolves to a definition (and that definition can be parsed): the registry's pin first, then the
 *   function types, in the scope of a property (0 for none). The native types of the engine are not asked.
 */
bool css_registry_resolvable(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Ref_Kind kind, const char * name, size_t length,
    GTEXT_CSS_Property_Id scope);

/** @brief Units of work the registrations (and the lookups made through css_registry_find_counted()) have done: hash probes, binary search
 *  steps, and the bytes of names hashed, folded and copied. For the quadratic-nesting suite, which holds it to a ratio. */
size_t css_registry_work(const GTEXT_CSS_Registry * registry);

/** @brief gtext_css_registry_find_property() that adds its work to `*work`. */
GTEXT_CSS_Property_Id css_registry_find_counted(const GTEXT_CSS_Registry * registry, const char * name, size_t length, size_t * work);

/** @brief The allocator the registry was made with (a copy lives in the registry). */
const GTEXT_Allocator * css_registry_allocator(const GTEXT_CSS_Registry * registry);

/** @brief The match options the registry validates under (allocator unset). */
const GTEXT_CSS_Match_Options * css_registry_match_options(const GTEXT_CSS_Registry * registry);


/**
 * @brief Make a custom property registration, validated as CSS.registerProperty() validates (see gtext_css_registry_register_custom()), without adding it to the
 *   registry. `syntax` NULL is `*`; `initial` NULL is none. The name is checked as a custom property name string; a caller that has a dashed identifier has one.
 *   The grammars are compiled under the registry's resolver.
 */
GTEXT_CSS_Status css_registry_make_creg(const GTEXT_CSS_Registry * registry, const char * name, size_t name_length, const char * syntax, size_t syntax_length,
    bool inherits, const char * initial, size_t initial_length, css_creg ** out, GTEXT_CSS_Registry_Result * result, css_creg_why * why);

/** @brief The registration of the caller with this name, or NULL. `work` (may be NULL) counts the probes. */
const css_creg * css_registry_find_creg(const GTEXT_CSS_Registry * registry, const char * name, size_t length, size_t * work);

/** @brief The `index`th registration of the caller (in the order they were made), or NULL. */
const css_creg * css_registry_creg_at(const GTEXT_CSS_Registry * registry, size_t index);


/**
 * @brief Compile a syntax string for matching (every type component, under the registry's resolver), outside any registration. GTEXT_CSS_E_GRAMMAR when the text
 *   is not a syntax string; GTEXT_CSS_E_OOM; GTEXT_CSS_E_INTERNAL when a component does not compile.
 */
GTEXT_CSS_Status css_registry_compile_syntax(const GTEXT_CSS_Registry * registry, const char * text, size_t length, css_compiled_syntax ** out);

#endif /* GHOTI_IO_GTEXT_CSS_REGISTRY_INTERNAL_H */
