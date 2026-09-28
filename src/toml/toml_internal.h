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
 * @file toml_internal.h
 * @brief Shared internals of the TOML module. Not installed.
 */

#ifndef GHOTI_IO_GTEXT_TOML_INTERNAL_H
#define GHOTI_IO_GTEXT_TOML_INTERNAL_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/toml/toml_core.h>
#include <ghoti.io/text/toml/toml_dom.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * How a table or array came to exist.
 *
 * This is the whole of TOML's redefinition rules, and it is a *state* rather
 * than a boolean because the specification gives four different answers
 * depending on it. `[a]` twice is invalid; `[a]` after `[a.b]` is fine; a
 * dotted key may not redefine a table a header defined, nor the other way
 * round; and nothing at all may be added to a table written inline.
 *
 * Keeping it as flags on the node is what lets each rule be checked where it
 * applies. A single "defined" boolean would collapse three of the four.
 */
typedef enum {
  /** Created only as the ancestor of something else: `a` in `[a.b]`. A later
   *  `[a]` is allowed, and is the case the specification calls defining a
   *  super-table afterwards. */
  TOML_TABLE_IMPLICIT = 0,
  /** Created by its own `[header]`. A second header for it is invalid, and so
   *  is a dotted key that would reach into it from another scope. */
  TOML_TABLE_HEADER = 1,
  /** Created by a dotted key inside a key-value line: `a` in `a.b = 1`. More
   *  dotted keys may extend it; a `[a]` header may not. */
  TOML_TABLE_DOTTED = 2,
  /** Written as `{ }`. Closed: nothing may be added by any means. */
  TOML_TABLE_INLINE = 3
} toml_table_origin;

/**
 * How an array came to exist, which decides whether `[[a]]` may append to it.
 */
typedef enum {
  /** Written as `[ ]`. A `[[a]]` header may not append to it. */
  TOML_ARRAY_STATIC = 0,
  /** Grown by `[[a]]` headers, and only they may add to it. */
  TOML_ARRAY_OF_TABLES = 1
} toml_array_origin;

/** One entry of a table, in definition order. */
typedef struct {
  char * key;   ///< Decoded key bytes, NUL-terminated for convenience.
  size_t len;   ///< Key length; authoritative, since a key may contain NUL.
  GTEXT_TOML_Value * value;
} toml_pair;

struct GTEXT_TOML_Value {
  GTEXT_TOML_Type type;
  /**
   * The allocator this node came from, NULL meaning the default.
   *
   * On every node rather than on the root alone, the way the JSON module keeps
   * its context pointer. gtext_toml_free() reads the root's, and it has to be
   * stored somewhere: freeing a tree built with a caller's allocator through
   * the default one corrupts the heap, which is the failure `make
   * check-allocators` exists to make impossible and which no test could see.
   */
  const GTEXT_Allocator * alloc;
  /** toml_table_origin for a table, toml_array_origin for an array. Unused
   *  for scalars. */
  unsigned char origin;
  union {
    struct {
      char * data;
      size_t len;
    } string;
    int64_t integer;
    double floating;
    bool boolean;
    GCHRON_TomlValue datetime;
    struct {
      GTEXT_TOML_Value ** items;
      size_t count;
      size_t capacity;
    } array;
    struct {
      toml_pair * pairs;
      size_t count;
      size_t capacity;
    } table;
  } as;
};

/** A growable byte buffer for decoding strings and joining keys. */
typedef struct {
  char * data;
  size_t len;
  size_t capacity;
} toml_buf;

/** Parser state, threaded through every scanner. */
typedef struct {
  const char * buf;
  size_t len;
  size_t pos;
  int line;          ///< 1-based.
  size_t line_start; ///< Byte offset of the current line's first byte.
  const GTEXT_Allocator * alloc;
  GTEXT_TOML_Error * err; ///< May be NULL.
  size_t max_depth;       ///< 0 for no limit.
} toml_ctx;

/*--------------------------------------------------------------------------*
 * toml_error.c
 *--------------------------------------------------------------------------*/

/**
 * Record a failure at the current position and return false.
 *
 * Always returns false so a scanner can `return toml_fail(...)`. Fills line
 * and column from `ctx`, and attaches a snippet of the offending line.
 */
bool toml_fail(toml_ctx * ctx, GTEXT_TOML_Status code, const char * message);

/** As toml_fail(), but reporting a position other than `ctx->pos`. */
bool toml_fail_at(toml_ctx * ctx, size_t offset, GTEXT_TOML_Status code,
    const char * message);

/*--------------------------------------------------------------------------*
 * toml_dom.c
 *--------------------------------------------------------------------------*/

GTEXT_TOML_Value * toml_value_new(const GTEXT_Allocator * alloc,
    GTEXT_TOML_Type type);

/** Append to an array. Takes ownership of `item` on success only. */
bool toml_array_push(const GTEXT_Allocator * alloc, GTEXT_TOML_Value * array,
    GTEXT_TOML_Value * item);

/** Find a direct key of a table, or NULL. */
GTEXT_TOML_Value * toml_table_find(const GTEXT_TOML_Value * table,
    const char * key, size_t key_len);

/**
 * Add a key to a table, copying the key bytes. The caller has already
 * established that the key is absent.
 */
bool toml_table_insert(const GTEXT_Allocator * alloc, GTEXT_TOML_Value * table,
    const char * key, size_t key_len, GTEXT_TOML_Value * value);

/*--------------------------------------------------------------------------*
 * toml_lexer.c
 *--------------------------------------------------------------------------*/

bool toml_buf_reserve(const GTEXT_Allocator * alloc, toml_buf * buf,
    size_t extra);
bool toml_buf_append(const GTEXT_Allocator * alloc, toml_buf * buf,
    const char * bytes, size_t len);
bool toml_buf_append_byte(const GTEXT_Allocator * alloc, toml_buf * buf,
    char byte);
/** Encode one scalar value as UTF-8. The value has already been checked. */
bool toml_buf_append_utf8(const GTEXT_Allocator * alloc, toml_buf * buf,
    uint32_t scalar);
void toml_buf_free(const GTEXT_Allocator * alloc, toml_buf * buf);

/**
 * Decode one UTF-8 sequence at `pos`.
 *
 * @param scalar Receives the scalar value. May be NULL.
 * @param width Receives the number of bytes consumed.
 * @return false for an ill-formed sequence, an overlong encoding, a surrogate
 *   or a value above U+10FFFF - the four ways UTF-8 goes wrong that a
 *   byte-counting loop cannot see.
 */
bool toml_utf8_next(const char * buf, size_t len, size_t pos, uint32_t * scalar,
    size_t * width);

/** Whether the whole buffer is well-formed UTF-8; `bad` gets the offset. */
bool toml_utf8_validate(const char * buf, size_t len, size_t * bad);

/** Scan a quoted or bare key at ctx->pos into `out`. */
bool toml_scan_key(toml_ctx * ctx, toml_buf * out);

/** Scan a string value (any of the four forms) at ctx->pos. */
bool toml_scan_string(toml_ctx * ctx, toml_buf * out);

/**
 * Scan a number, boolean or date-time at ctx->pos into a fresh value.
 *
 * One function because the three are told apart by looking at the same bytes:
 * `1979-05-27` begins like an integer and is a date, and `inf` begins like a
 * bare key and is a float.
 */
GTEXT_TOML_Value * toml_scan_atom(toml_ctx * ctx);

#endif // GHOTI_IO_GTEXT_TOML_INTERNAL_H
