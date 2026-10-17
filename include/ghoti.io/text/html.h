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
 * @file html.h
 * @brief Public umbrella header for HTML.
 *
 * Include <ghoti.io/text/html.h> to parse HTML as the WHATWG standard
 * defines it into the markup tree: a whole document, a fragment in a context
 * element, a document in pieces, or a file. The status codes, options and result
 * are in html_core.h, the parse errors in html_error.h and the parsers in
 * html_parse.h, and writing a tree back out as HTML (the standard's fragment
 * serialization algorithm, to a sink, a buffer, a file or token by token) in
 * html_serialize.h. The tree is described in <ghoti.io/text/markup.h>, which
 * this includes.
 */

#ifndef GHOTI_IO_GTEXT_HTML_H
#define GHOTI_IO_GTEXT_HTML_H

#include <ghoti.io/text/macros.h>

/* The tree the parser builds */
#include <ghoti.io/text/markup.h>

/* Core types and definitions */
#include <ghoti.io/text/html/html_core.h>
#include <ghoti.io/text/html/html_error.h>

/* Public module headers */
#include <ghoti.io/text/html/html_parse.h>
#include <ghoti.io/text/html/html_serialize.h>
#include <ghoti.io/text/html/html_sanitize.h>

#endif // GHOTI_IO_GTEXT_HTML_H
