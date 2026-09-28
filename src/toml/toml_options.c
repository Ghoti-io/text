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
 * @file toml_options.c
 * @brief The TOML module's default options.
 */

#include "toml_internal.h"

GTEXT_TOML_Parse_Options gtext_toml_parse_options_default(void) {
  GTEXT_TOML_Parse_Options opts;
  opts.allocator = NULL;
  /* 256, as the YAML module's default is. Deep enough that no document a
   * person wrote reaches it, shallow enough that a hostile one is refused
   * before it has allocated much. 0 means no limit and is a caller's choice to
   * make, not a default to ship. */
  opts.max_depth = 256;
  opts.max_total_bytes = 0;
  return opts;
}
