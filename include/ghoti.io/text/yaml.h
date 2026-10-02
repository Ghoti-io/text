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
 * @file yaml.h
 * @brief Public umbrella header for the YAML module.
 *
 * This header includes the YAML public sub-headers so callers can simply
 * include @c <ghoti.io/text/yaml.h> to access the YAML API. The sub-headers
 * provide the core types, DOM inspection API, streaming parser, and writer.
 *
 * Usage:
 * - From a C source: `#include <ghoti.io/text/yaml.h>`
 * - From C++ sources the headers are C-linkage guarded.
 */

#ifndef GHOTI_IO_GTEXT_YAML_H
#define GHOTI_IO_GTEXT_YAML_H

#include <ghoti.io/text/macros.h>

/* Core types and definitions */
#include <ghoti.io/text/yaml/yaml_core.h>

/* Public module headers */
#include <ghoti.io/text/yaml/yaml_dom.h>
#include <ghoti.io/text/yaml/yaml_events.h>
#include <ghoti.io/text/yaml/yaml_schema.h>
#include <ghoti.io/text/yaml/yaml_stream.h>
#include <ghoti.io/text/yaml/yaml_writer.h>

#endif // GHOTI_IO_GTEXT_YAML_H
