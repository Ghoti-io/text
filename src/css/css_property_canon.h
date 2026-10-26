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
 * The canonical form of a specified value (css_property_canon.c): private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_PROPERTY_CANON_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_PROPERTY_CANON_H

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The rule set of a property (lower case name), a small number that is never 0, or 0 when the property has none. */
GTEXT_INTERNAL_API uint8_t css_canon_rules_of(const char * name, size_t length);

/**
 * The canonical form of `text`, the serialization of a value of a property with rule set `id`. `*out` is a new NUL-terminated string
 * (release with the allocator); a text the rule does not recognize comes back as it was. GTEXT_CSS_E_OOM, or GTEXT_CSS_E_INVALID for an `id`
 * that is not one.
 */
GTEXT_INTERNAL_API GTEXT_CSS_Status css_canon_apply(uint8_t id, const GTEXT_Allocator * allocator, const char * text, size_t length,
    char ** out, size_t * out_length);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GTEXT_SRC_CSS_CSS_PROPERTY_CANON_H */
