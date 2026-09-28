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
 * @file toml.h
 * @brief Public umbrella header for the TOML module.
 *
 * Include <ghoti.io/text/toml.h> to reach the whole TOML API: the status
 * codes, error payload and options in toml_core.h, the parser, tree accessors
 * and constructors in toml_dom.h, and the writer and its sinks in
 * toml_writer.h.
 *
 * The specification is TOML v1.0.0 (2021-01-12). The four date-time types are
 * `chron` values, so toml_dom.h includes <ghoti.io/chron/chron.h> and a caller
 * needs no second header to do arithmetic on one.
 */

#ifndef GHOTI_IO_GTEXT_TOML_H
#define GHOTI_IO_GTEXT_TOML_H

#include <ghoti.io/text/macros.h>

/* Core types and definitions */
#include <ghoti.io/text/toml/toml_core.h>

/* Public module headers */
#include <ghoti.io/text/toml/toml_dom.h>
#include <ghoti.io/text/toml/toml_writer.h>

#endif // GHOTI_IO_GTEXT_TOML_H
