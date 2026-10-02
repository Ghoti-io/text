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
 * @brief Default options for the YAML module.
 *
 * Minimal stub implementation used to wire defaults into the test harness.
 */

#include <stdlib.h>

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/yaml/yaml_core.h>
#include "yaml_internal.h"

GTEXT_API GTEXT_YAML_Parse_Options gtext_yaml_parse_options_default(void)
{
  GTEXT_YAML_Parse_Options opts = {0};
  opts.mode = GTEXT_YAML_MODE_DEFAULT;
  opts.dupkeys = GTEXT_YAML_DUPKEY_ERROR;
  opts.schema = GTEXT_YAML_SCHEMA_CORE;
  /* Defaults: 0 means use library default; here we supply concrete defaults */
  opts.max_depth = 256; /* default max nesting */
  opts.max_total_bytes = 64 * 1024 * 1024; /* 64 MiB */
  opts.max_alias_expansion = 10000; /* total alias-expanded nodes limit */
  opts.validate_utf8 = true;
  opts.resolve_tags = true;
  opts.retain_comments = false;
  opts.yaml_1_1 = false;
  opts.enable_custom_tags = false;
  opts.allow_nonstandard_tags = true;
  opts.allow_aliases = true;
  opts.allow_merge_keys = true;
  opts.allow_complex_keys = true;
  opts.require_string_keys = false;
  opts.enable_json_fast_path = true;
  opts.custom_tags = NULL;
  opts.custom_tag_count = 0;
  opts.warning_callback = NULL;
  opts.warning_user_data = NULL;
  opts.warnings_as_errors = false;
  opts.warning_mask = 0;
  return opts;
}

/* "All size limits use 0 to denote 'use the library default'" - the contract
 * GTEXT_YAML_Parse_Options states in yaml_core.h, and which nothing
 * implemented. Every check on these three is written "if (limit > 0 && ...)",
 * so a zero did not select the default, it removed the limit; and the header's
 * own suggested spelling, "GTEXT_YAML_Parse_Options opts = {0};", removed all
 * three at once. A hundred thousand nested flow sequences then ran
 * resolve_node() off the end of the stack.
 *
 * YAML was alone in this. json_get_limit() and csv_get_limit() have always
 * mapped zero to the default for their formats, so the same field in the same
 * shape of struct meant opposite things in three sibling parsers.
 *
 * YamlLimits.ZeroMeansDefault has asserted this since long before it was
 * true. It feeds "[1, 2, 3, 4, 5]", which is accepted under every limit and
 * under none, so the test named for the contract could not tell the two
 * apart. */
static size_t yaml_get_limit(size_t configured, size_t fallback) {
  return configured > 0 ? configured : fallback;
}

GTEXT_INTERNAL_API GTEXT_YAML_Parse_Options gtext_yaml_parse_options_effective(
  const GTEXT_YAML_Parse_Options *opts
) {
  GTEXT_YAML_Parse_Options effective = opts
    ? *opts
    : gtext_yaml_parse_options_default();

  const GTEXT_YAML_Parse_Options defaults = gtext_yaml_parse_options_default();
  effective.max_depth =
    yaml_get_limit(effective.max_depth, defaults.max_depth);
  effective.max_total_bytes =
    yaml_get_limit(effective.max_total_bytes, defaults.max_total_bytes);
  effective.max_alias_expansion =
    yaml_get_limit(effective.max_alias_expansion, defaults.max_alias_expansion);

  if (effective.mode == GTEXT_YAML_MODE_CONFIG) {
    effective.schema = GTEXT_YAML_SCHEMA_FAILSAFE;
    effective.allow_complex_keys = false;
    effective.require_string_keys = true;
    effective.enable_json_fast_path = false;
  }

  return effective;
}

GTEXT_API GTEXT_YAML_Parse_Options gtext_yaml_parse_options_safe(void)
{
  GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
  opts.max_depth = 64;
  opts.max_total_bytes = 16 * 1024 * 1024;
  opts.max_alias_expansion = 1000;
  opts.enable_custom_tags = false;
  opts.allow_nonstandard_tags = false;
  opts.allow_aliases = false;
  opts.allow_merge_keys = false;
  opts.allow_complex_keys = false;
  opts.require_string_keys = true;
  opts.enable_json_fast_path = true;
  return opts;
}

GTEXT_API GTEXT_YAML_Write_Options gtext_yaml_write_options_default(void)
{
  GTEXT_YAML_Write_Options opts = {0};
  opts.pretty = false;
  opts.indent_spaces = 2;
  opts.line_width = 0;
  opts.newline = "\n";
  opts.trailing_newline = false;
  opts.canonical = false;
  opts.scalar_style = GTEXT_YAML_SCALAR_STYLE_PLAIN;
  opts.flow_style = GTEXT_YAML_FLOW_STYLE_AUTO;
  opts.encoding = GTEXT_YAML_ENCODING_UTF8;
  opts.emit_bom = false;
  opts.enable_custom_tags = false;
  opts.custom_tags = NULL;
  opts.custom_tag_count = 0;
  /* The dialect the output is meant to be read back in.  These are the 1.2
     core schema, which is what the writer has always emitted and what
     gtext_yaml_parse_options_default() reads, so the default output does not
     change. */
  opts.schema = GTEXT_YAML_SCHEMA_CORE;
  opts.yaml_1_1 = false;
  /* The C library, which is what a write has always used. */
  opts.allocator = NULL;
  return opts;
}

GTEXT_API void gtext_yaml_error_free(GTEXT_YAML_Error *err)
{
  if (!err) {
    return;
  }

  if (err->context_snippet) {
    free(err->context_snippet); // allocator-exempt: see the option's docs
    err->context_snippet = NULL;
    err->context_snippet_len = 0;
  }
}

GTEXT_API void gtext_yaml_free(GTEXT_YAML_Document *doc)
{
  if (!doc) return;
  
  /* Free context (which frees arena, resolver, and all nodes) */
  yaml_context_free(doc->ctx);
  
  /* Note: doc itself was allocated from the arena, so it's freed by
   * yaml_context_free(). We don't need to free it separately. */
}
