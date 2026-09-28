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
 * @file ini_internal.h
 * @brief Shared internals of the INI module.
 *
 * The ownership rule is uniform and deliberately boring: **the document owns
 * every byte in it, and every stored string is its own allocation from the
 * document's allocator.** Nothing points into the caller's input buffer, so a
 * document outlives the bytes it was parsed from, and there is no per-field
 * "is this owned" flag to get wrong.
 *
 * An entry keeps the original line in five pieces rather than as a copy, so
 * that a byte-identical rewrite costs no duplicated bytes:
 *
 *     line = pre + key + sep + value + eol
 *
 * `pre` is the leading whitespace, `sep` is everything between the key and the
 * value (the whitespace, the `=`, and the whitespace after it), and `eol` is
 * everything after the value including the line terminator. Concatenating the
 * five reproduces the input exactly; an entry a caller creates gets
 * `sep = "="` and `eol = "\n"`. That is why Desktop Entry §3's preservation
 * requirement is met by construction rather than by care.
 */

#ifndef GHOTI_IO_GTEXT_INI_INI_INTERNAL_H
#define GHOTI_IO_GTEXT_INI_INI_INTERNAL_H

#include <ghoti.io/text/ini.h>
#include <ghoti.io/text/macros.h>
#include <string.h>

/** A counted, owned string. A NULL `data` means absent, not empty. */
typedef struct {
  char * data;
  size_t len;
} ini_str;

/** One `key = value` line. */
typedef struct {
  ini_str key;     ///< As the document spelled it, postfix included.
  ini_str value;   ///< Raw: leading run after `=` removed, nothing decoded.
  ini_str pre;     ///< Leading whitespace of the line.
  ini_str sep;     ///< From the end of the key to the start of the value.
  ini_str eol;     ///< From the end of the value to the end of the line.
  ini_str comment; ///< Comment and blank lines immediately above, verbatim.
} ini_entry;

/** One `[group]` and the entries under it. */
struct GTEXT_INI_Group {
  ini_str name;
  ini_str comment;  ///< Comment and blank lines immediately above the header.
  ini_str hdr_pre;  ///< Leading whitespace before `[`.
  ini_str hdr_post; ///< From `]` to the end of the line, terminator included.
  ini_entry * entries;
  size_t count;
  size_t capacity;
  struct GTEXT_INI_Document * doc; ///< For the dialect. The document does not
                                  ///< move, so this stays valid when the
                                  ///< group array is reallocated.
};

struct GTEXT_INI_Document {
  const GTEXT_Allocator * alloc;
  GTEXT_INI_Dialect dialect;
  GTEXT_INI_Group * groups;
  size_t count;
  size_t capacity;
  ini_str leading;  ///< Before the first group header.
  ini_str trailing; ///< After the last line that belongs to an entry.
  bool synthesized; ///< True when built by gtext_ini_new() rather than parsed.
};

/** Duplicate @p len bytes, NUL-terminating one past the end. */
GTEXT_INTERNAL_API bool gtext_ini_str_set(const GTEXT_Allocator * alloc,
    ini_str * out, const char * bytes, size_t len);

/** Release a string and mark it absent. */
GTEXT_INTERNAL_API void gtext_ini_str_clear(const GTEXT_Allocator * alloc,
    ini_str * s);

/** Fill in @p err, copying no snippet when @p err is NULL. */
GTEXT_INTERNAL_API void gtext_ini_set_error(GTEXT_INI_Error * err,
    GTEXT_INI_Status code, const char * message, const char * input,
    size_t input_len, size_t offset);

/** Whether @p name is a legal group name for @p dialect. */
GTEXT_INTERNAL_API bool gtext_ini_group_name_ok(const GTEXT_INI_Dialect * dialect,
    const char * name, size_t len);

/** Whether @p key is a legal key for @p dialect, postfix included. */
GTEXT_INTERNAL_API bool gtext_ini_key_ok(const GTEXT_INI_Dialect * dialect,
    const char * key, size_t len);

/** The offset of the `[` that starts a key's locale postfix, or len. */
GTEXT_INTERNAL_API size_t gtext_ini_key_base_len(const char * key, size_t len);

/** Append an entry to a group without any duplicate or charset check. */
GTEXT_INTERNAL_API ini_entry * gtext_ini_group_push(GTEXT_INI_Group * group);

#endif // GHOTI_IO_GTEXT_INI_INI_INTERNAL_H
