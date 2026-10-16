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
 * @file
 *
 * Status strings and options of the markup tree.
 */

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/markup/markup_core.h>

const char * gtext_markup_status_string(GTEXT_Markup_Status status) {
  switch (status) {
    case GTEXT_MARKUP_OK:
      return "success";
    case GTEXT_MARKUP_E_INVALID:
      return "invalid argument";
    case GTEXT_MARKUP_E_OOM:
      return "out of memory";
    case GTEXT_MARKUP_E_LIMIT:
      return "a configured limit was exceeded";
    case GTEXT_MARKUP_E_HIERARCHY:
      return "the insertion would break the tree's rules";
    case GTEXT_MARKUP_E_WRONG_DOCUMENT:
      return "the node belongs to another document";
    case GTEXT_MARKUP_E_STATE:
      return "not valid in the node's current state";
    case GTEXT_MARKUP_E_NAME:
      return "a name that cannot be stored";
    case GTEXT_MARKUP_E_ENCODING:
      return "bytes that are not well-formed UTF-8";
    case GTEXT_MARKUP_E_UNREPRESENTABLE:
      return "the language has no spelling for this text";
    case GTEXT_MARKUP_E_DTD_SYNTAX:
      return "invalid DTD declaration syntax";
    case GTEXT_MARKUP_E_LIMIT_WORK:
      return "the DTD edit work limit was exceeded";
    case GTEXT_MARKUP_E_WRITE:
      return "a sink refused output";
  }
  /* After the switch and not a `default:` arm: -Wswitch reports an enumerator
     added to the enum and missing here only where there is no default. */
  return "unknown status";
}

GTEXT_Markup_Options gtext_markup_options_default(void) {
  GTEXT_Markup_Options opts;
  opts.allocator = NULL;
  /* 256, as the YAML and TOML modules default. Deep enough that no document a
     person wrote reaches it. Every walk in this library is iterative, so the
     limit protects the caller's own recursion, not ours. */
  opts.max_depth = 256;
  opts.max_nodes = 0;
  /* An attribute lookup scans the element's attributes, so an unbounded count
     makes building an element quadratic. Real elements have a handful. */
  opts.max_attributes = 1024;
  return opts;
}
