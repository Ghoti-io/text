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
 * @file ini.h
 * @brief INI parsing and serialization, by named dialect.
 *
 * This header serves as the umbrella header for INI functionality.
 *
 * There is no INI specification. What this module implements is a named
 * dialect, chosen by the caller: the freedesktop.org **Desktop Entry
 * Specification 1.5 (2020-04-27)** by default, and a documented derivation
 * from it for the long tail. See @ref format_ini for the dialect table, the
 * deviations from each reference implementation, and the measured scores.
 *
 * For internal implementations that only need core types, use
 * <ghoti.io/text/ini/ini_core.h> instead to reduce compile-time dependencies.
 */

#ifndef GHOTI_IO_GTEXT_INI_H
#define GHOTI_IO_GTEXT_INI_H

#include <ghoti.io/text/macros.h>

// Include core types and definitions
#include <ghoti.io/text/ini/ini_core.h>

// INI module headers
#include <ghoti.io/text/ini/ini_dom.h>
#include <ghoti.io/text/ini/ini_value.h>
#include <ghoti.io/text/ini/ini_writer.h>

#endif // GHOTI_IO_GTEXT_INI_H
