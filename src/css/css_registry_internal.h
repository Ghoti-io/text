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

#endif /* GHOTI_IO_GTEXT_CSS_REGISTRY_INTERNAL_H */
