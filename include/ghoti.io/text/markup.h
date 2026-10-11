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
 * @file markup.h
 * @brief Public umbrella header for the markup tree.
 *
 * Include <ghoti.io/text/markup.h> to reach the document model XML, XHTML and
 * HTML share: the status codes, options and vocabulary in markup_core.h, the
 * tree in markup_tree.h, and the escaping and lexical checks in
 * markup_escape.h.
 */

#ifndef GHOTI_IO_GTEXT_MARKUP_H
#define GHOTI_IO_GTEXT_MARKUP_H

#include <ghoti.io/text/macros.h>

/* Core types and definitions */
#include <ghoti.io/text/markup/markup_core.h>

/* Public module headers */
#include <ghoti.io/text/markup/markup_escape.h>
#include <ghoti.io/text/markup/markup_tree.h>

#endif // GHOTI_IO_GTEXT_MARKUP_H
