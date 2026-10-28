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
 * css_decode.h, the incremental parser in css_stream.h, the typed values and
 * their serialization in css_value.h, and the Value Definition Syntax engine
 * that decides which component values a grammar accepts in css_grammar.h, the math functions in css_math.h and the colors in css_color.h, and the registry of every property, descriptor and type of the specifications with property-level parsing in css_registry.h and css_property.h, the caller's tree and environment in css_element.h, shorthands and declaration blocks in css_shorthand.h, logical properties in css_logical.h, computed values in css_computed.h, cascade layers in css_layer.h, the cascade in css_cascade.h, custom property values in css_custom.h, `@property` rules in css_at_property.h the limits of `var()` substitution in css_substitute.h, the presentation attributes in css_presentation.h and the call from an element to its computed style in css_style.h, and the selectors (parse, serialize, specificity) in css_selector.h. The format page
 * is documentation/formats/css.md.
 */

#ifndef GHOTI_IO_GTEXT_CSS_H
#define GHOTI_IO_GTEXT_CSS_H

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/css/css_at_property.h>
#include <ghoti.io/text/css/css_cascade.h>
#include <ghoti.io/text/css/css_color.h>
#include <ghoti.io/text/css/css_computed.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_custom.h>
#include <ghoti.io/text/css/css_decode.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_function.h>
#include <ghoti.io/text/css/css_grammar.h>
#include <ghoti.io/text/css/css_layer.h>
#include <ghoti.io/text/css/css_logical.h>
#include <ghoti.io/text/css/css_math.h>
#include <ghoti.io/text/css/css_parse.h>
#include <ghoti.io/text/css/css_presentation.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/css/css_selector.h>
#include <ghoti.io/text/css/css_serialize.h>
#include <ghoti.io/text/css/css_shorthand.h>
#include <ghoti.io/text/css/css_stream.h>
#include <ghoti.io/text/css/css_style.h>
#include <ghoti.io/text/css/css_substitute.h>
#include <ghoti.io/text/css/css_value.h>

#endif // GHOTI_IO_GTEXT_CSS_H
