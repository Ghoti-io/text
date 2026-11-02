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
 * The namespaces a style rule's selectors are read and written in (cssom_rule_internal.h): an immutable, shared list of the prefixes and the default namespace that the
 * `@namespace` rules of a sheet declared, so that a rule keeps its text, `x|a` or `*|b`, after it has left the sheet.
 */

#include <string.h>

#include "css_internal.h"
#include "cssom_rule_internal.h"

/// Copy `n` bytes and a NUL to the cursor and advance it past them; the copy's address.
static char * put_string(char ** cursor, const char * src, size_t n) {
  char * at = *cursor;
  if (n) {
    memcpy(at, src, n);
  }
  at[n] = '\0';
  *cursor = at + n + 1;
  return at;
}

css_ns_env * css_ns_env_retain(css_ns_env * env) {
  if (env) {
    env->refs++;
  }
  return env;
}

void css_ns_env_release(css_ns_env * env) {
  if (!env || --env->refs != 0) {
    return;
  }
  css_core * core = env->core;
  GTEXT_Allocator a = core->alloc;
  core->counters.live_bytes -= env->bytes;
  gtext_allocator_free(&a, env->entries);
  gtext_allocator_free(&a, env->strings);
  gtext_allocator_free(&a, env);
  css_core_release(core);
}

void css_ns_env_map(const css_ns_env * env, GTEXT_CSS_Namespace_Map * map) {
  map->entries = env ? env->entries : NULL;
  map->count = env ? env->count : 0;
  map->default_uri = env ? env->default_uri : NULL;
}

GTEXT_CSS_Status css_ns_env_new(css_core * core, const GTEXT_CSS_Namespace * entries, size_t count, const char * default_uri, css_ns_env ** out) {
  GTEXT_Allocator a = core->alloc;
  size_t chars = 0;
  for (size_t i = 0; i < count; i++) {
    chars += strlen(entries[i].prefix) + 1 + strlen(entries[i].uri) + 1;
  }
  if (default_uri) {
    chars += strlen(default_uri) + 1;
  }
  const size_t bytes = sizeof(css_ns_env) + count * sizeof(GTEXT_CSS_Namespace) + chars;
  GTEXT_CSS_Status st = css_core_room_bytes(core, bytes);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  css_ns_env * env = gtext_allocator_calloc(&a, 1, sizeof *env);
  GTEXT_CSS_Namespace * copy = count ? gtext_allocator_calloc(&a, count, sizeof *copy) : NULL;
  char * strings = gtext_allocator_malloc(&a, chars ? chars : 1);
  if (!env || (count && !copy) || !strings) {
    gtext_allocator_free(&a, env);
    gtext_allocator_free(&a, copy);
    gtext_allocator_free(&a, strings);
    return GTEXT_CSS_E_OOM;
  }
  char * w = strings;
  for (size_t i = 0; i < count; i++) {
    copy[i].prefix = put_string(&w, entries[i].prefix, strlen(entries[i].prefix));
    copy[i].uri = put_string(&w, entries[i].uri, strlen(entries[i].uri));
  }
  env->default_uri = default_uri ? put_string(&w, default_uri, strlen(default_uri)) : NULL;
  env->refs = 1;
  env->core = core;
  env->entries = copy;
  env->count = count;
  env->strings = strings;
  env->bytes = bytes;
  core->counters.live_bytes += bytes;
  css_core_retain(core);
  *out = env;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_ns_env_with(css_core * core, const css_ns_env * base, const char * prefix, const char * uri, css_ns_env ** out) {
  GTEXT_Allocator a = core->alloc;
  const size_t base_count = base ? base->count : 0;
  GTEXT_CSS_Namespace * tmp = gtext_allocator_calloc(&a, base_count + 1, sizeof *tmp);
  if (!tmp) {
    return GTEXT_CSS_E_OOM;
  }
  size_t count = 0;
  bool replaced = false;
  for (size_t i = 0; i < base_count; i++) {
    tmp[count] = base->entries[i];
    if (prefix && strcmp(base->entries[i].prefix, prefix) == 0) {
      tmp[count].uri = uri;
      replaced = true;
    }
    count++;
  }
  if (prefix && !replaced) {
    tmp[count].prefix = prefix;
    tmp[count].uri = uri;
    count++;
  }
  const GTEXT_CSS_Status st = css_ns_env_new(core, tmp, count, prefix ? (base ? base->default_uri : NULL) : uri, out);
  gtext_allocator_free(&a, tmp);
  return st;
}
