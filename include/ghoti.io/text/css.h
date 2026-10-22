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
 * @file css.h
 * @brief Public umbrella header for CSS.
 *
 * Include <ghoti.io/text/css.h> to read CSS as CSS Syntax Module Level 3
 * defines it: the tokenizer, every parser entry point with CSS Nesting's
 * block contents, the An+B microsyntax, and a serializer whose output reads
 * back as the same tree. The statuses, options, result and tree are in
 * css_core.h, the entry points and parse errors in css_parse.h, the
 * serializer in css_serialize.h, a stylesheet's bytes and their encoding in
 * css_decode.h, and the incremental parser in css_stream.h. The format page
 * is documentation/formats/css.md.
 */

#ifndef GHOTI_IO_GTEXT_CSS_H
#define GHOTI_IO_GTEXT_CSS_H

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_decode.h>
#include <ghoti.io/text/css/css_parse.h>
#include <ghoti.io/text/css/css_serialize.h>
#include <ghoti.io/text/css/css_stream.h>

#endif // GHOTI_IO_GTEXT_CSS_H
