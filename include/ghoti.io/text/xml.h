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
 * @file xml.h
 * @brief Public umbrella header for XML.
 *
 * Include <ghoti.io/text/xml.h> to parse XML into the markup tree: the status
 * codes, options and errors in xml_core.h and the parser in xml_parse.h. The
 * tree is described in <ghoti.io/text/markup.h>, which this includes.
 */

#ifndef GHOTI_IO_GTEXT_XML_H
#define GHOTI_IO_GTEXT_XML_H

#include <ghoti.io/text/macros.h>

/* The tree the parser builds */
#include <ghoti.io/text/markup.h>

/* Core types and definitions */
#include <ghoti.io/text/xml/xml_core.h>

/* Public module headers */
#include <ghoti.io/text/xml/xml_parse.h>

#endif // GHOTI_IO_GTEXT_XML_H
