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

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_PSEUDO_APPLY_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_PSEUDO_APPLY_H

#include <ghoti.io/text/macros.h>

#include <stdbool.h>

#include <ghoti.io/text/css/css_registry.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Whether a declaration of the property takes part in the cascade of the pseudo-element (a `GTEXT_CSS_SEL_PE_*` enumerator): false for a property that does not apply to
/// ::first-line, ::first-letter or ::marker. True for every other pseudo-element and for a custom property.
bool css_pseudo_applies(unsigned pseudo, const GTEXT_CSS_Property_Info * info);

#ifdef __cplusplus
}
#endif

#endif
