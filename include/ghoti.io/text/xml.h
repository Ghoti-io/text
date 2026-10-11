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
 * Include <ghoti.io/text/xml.h> to parse XML into the markup tree or as a stream
 * of events, or to write a tree as XML: the status codes, options and errors
 * in xml_core.h, the tree parser in xml_parse.h, the push and pull event
 * parsers in xml_stream.h and the writer in xml_writer.h. The tree is
 * described in <ghoti.io/text/markup.h>, which this includes.
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
#include <ghoti.io/text/xml/xml_stream.h>
#include <ghoti.io/text/xml/xml_writer.h>

#endif // GHOTI_IO_GTEXT_XML_H
