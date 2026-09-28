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
 * @file toml_parser.c
 * @brief The TOML 1.0.0 document parser.
 *
 * TOML is line-oriented at the top level and nested only inside a value, so
 * this is a loop over statements with an explicit stack for the values. The
 * stack is not an optimisation: `text`'s contract is that no document crashes
 * it whatever its nesting, which a recursive-descent value parser cannot
 * promise once max_depth is 0 (notes/text/HEAP-STACK-WALKS.md).
 *
 * The redefinition rules are the hard part of TOML, not the grammar. They are
 * four rules and not one, they were pinned against the pinned reference rather
 * than read off the prose, and each is checked where it applies:
 *
 *   - a `[header]` may define a table that exists only implicitly, which is
 *     the "super-table afterwards" case, but not one another header or a
 *     dotted key defined;
 *   - a dotted key may extend an implicit or dotted table, but not one a
 *     header defined;
 *   - nothing may be added to a table written inline, by either means;
 *   - `[[a]]` appends only to an array `[[a]]` itself made.
 */

#include "toml_internal.h"
#include <string.h>

/*--------------------------------------------------------------------------*
 * Key paths
 *--------------------------------------------------------------------------*/

/** One segment of a dotted key, decoded. */
typedef struct {
  char * data;
  size_t len;
} toml_seg;

typedef struct {
  toml_seg * items;
  size_t count;
  size_t capacity;
} toml_path;

static void path_free(const GTEXT_Allocator * alloc, toml_path * path) {
  for (size_t i = 0; i < path->count; ++i) {
    gtext_allocator_free(alloc, path->items[i].data);
  }
  gtext_allocator_free(alloc, path->items);
  path->items = NULL;
  path->count = 0;
  path->capacity = 0;
}

static bool path_push(
    const GTEXT_Allocator * alloc, toml_path * path, toml_buf * segment) {
  if (path->count == path->capacity) {
    size_t want = path->capacity ? path->capacity * 2 : 4;
    if (want > SIZE_MAX / sizeof(toml_seg)) return false;
    toml_seg * grown =
        gtext_allocator_realloc(alloc, path->items, want * sizeof(toml_seg));
    if (!grown) return false;
    path->items = grown;
    path->capacity = want;
  }
  /* A zero-length key is legal - `"" = 1` - so an empty segment must still
   * carry an allocation rather than a NULL, or the difference between "no
   * key" and "the empty key" would be lost. */
  char * copy = gtext_allocator_malloc(alloc, segment->len + 1);
  if (!copy) return false;
  if (segment->len) memcpy(copy, segment->data, segment->len);
  copy[segment->len] = '\0';
  path->items[path->count].data = copy;
  path->items[path->count].len = segment->len;
  path->count++;
  return true;
}

/*--------------------------------------------------------------------------*
 * Whitespace, comments and line ends
 *--------------------------------------------------------------------------*/

static void skip_blanks(toml_ctx * ctx) {
  while (ctx->pos < ctx->len
      && (ctx->buf[ctx->pos] == ' ' || ctx->buf[ctx->pos] == '\t')) {
    ctx->pos++;
  }
}

/**
 * Consume a comment, `ctx->pos` on the `#`.
 *
 * A comment's text is text, so the control-character rule applies to it as
 * well as to strings. Checking strings and forgetting comments is the common
 * shape of this mistake, and `invalid/control/comment-*` is where a parser
 * that did it finds out.
 */
static bool skip_comment(toml_ctx * ctx) {
  ctx->pos++; /* the '#' */
  while (ctx->pos < ctx->len) {
    unsigned char c = (unsigned char) ctx->buf[ctx->pos];
    if (c == '\n') return true;
    if (c == '\r') {
      if (ctx->pos + 1 < ctx->len && ctx->buf[ctx->pos + 1] == '\n') {
        return true;
      }
      return toml_fail(ctx, GTEXT_TOML_E_CONTROL,
          "a carriage return in a comment is only allowed before a line feed");
    }
    if (c == '\t') {
      ctx->pos++;
      continue;
    }
    if (c < 0x20 || c == 0x7F) {
      return toml_fail(ctx, GTEXT_TOML_E_CONTROL,
          "a control character other than tab cannot appear in a comment");
    }
    ctx->pos++;
  }
  return true;
}

/** Consume a newline. Returns false and records an error for a lone CR. */
static bool take_newline(toml_ctx * ctx) {
  if (ctx->buf[ctx->pos] == '\r') {
    if (ctx->pos + 1 >= ctx->len || ctx->buf[ctx->pos + 1] != '\n') {
      return toml_fail(ctx, GTEXT_TOML_E_CONTROL,
          "a carriage return is only a newline when a line feed follows it");
    }
    ctx->pos++;
  }
  ctx->pos++;
  ctx->line++;
  ctx->line_start = ctx->pos;
  return true;
}

/** After a statement: blanks, an optional comment, then a newline or the end. */
static bool finish_statement(toml_ctx * ctx) {
  skip_blanks(ctx);
  if (ctx->pos >= ctx->len) return true;
  if (ctx->buf[ctx->pos] == '#') {
    if (!skip_comment(ctx)) return false;
  }
  if (ctx->pos >= ctx->len) return true;
  if (ctx->buf[ctx->pos] == '\n' || ctx->buf[ctx->pos] == '\r') {
    return take_newline(ctx);
  }
  return toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN,
      "a statement must end at the end of its line");
}

/** Blanks, comments and newlines, which an array may contain between items. */
static bool skip_array_space(toml_ctx * ctx) {
  for (;;) {
    skip_blanks(ctx);
    if (ctx->pos >= ctx->len) return true;
    char c = ctx->buf[ctx->pos];
    if (c == '#') {
      if (!skip_comment(ctx)) return false;
      continue;
    }
    if (c == '\n' || c == '\r') {
      if (!take_newline(ctx)) return false;
      continue;
    }
    return true;
  }
}

/**
 * What may separate the parts of an inline table at the version being read.
 *
 * 1.0.0 allows only blanks, so a newline is left in the stream for the caller
 * to complain about by name; 1.1.0 allows what an array allows. Returning
 * false means an error was recorded - a lone CR, or a control character in a
 * comment - and not that nothing was skipped.
 */
static bool inline_space(toml_ctx * ctx) {
  if (ctx->version == GTEXT_TOML_VERSION_1_1_0) return skip_array_space(ctx);
  skip_blanks(ctx);
  return true;
}

/*--------------------------------------------------------------------------*
 * Keys
 *--------------------------------------------------------------------------*/

/**
 * Scan a dotted key into `path`.
 *
 * Whitespace is allowed around the dots. Each segment is decoded, so
 * `a."b"`, `a.b` and `a.'b'` all produce the same path - key identity is the
 * decoded string, which is why this cannot be done by comparing source text.
 */
static bool scan_key_path(toml_ctx * ctx, toml_path * path) {
  for (;;) {
    toml_buf segment = {0};
    if (!toml_scan_key(ctx, &segment)) {
      toml_buf_free(ctx->alloc, &segment);
      return false;
    }
    bool pushed = path_push(ctx->alloc, path, &segment);
    toml_buf_free(ctx->alloc, &segment);
    if (!pushed) return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    skip_blanks(ctx);
    if (ctx->pos < ctx->len && ctx->buf[ctx->pos] == '.') {
      ctx->pos++;
      skip_blanks(ctx);
      continue;
    }
    return true;
  }
}

/*--------------------------------------------------------------------------*
 * Values
 *--------------------------------------------------------------------------*/

/** One open container while a value is being built. */
typedef struct {
  GTEXT_TOML_Value * container; ///< The array or inline table being filled.
  GTEXT_TOML_Value * target;    ///< Where the next pair goes; inline tables.
  char * key;                   ///< Pending key for that pair.
  size_t key_len;
  bool have_key;
  /**
   * Whether a comma has been consumed and the pair after it not yet read.
   *
   * WANT_KEY is reached two ways - the brace has just opened, or a comma has
   * just been read - and `{ }` is legal where `{ a = 1, }` is not. Without
   * this the two are one state and a trailing comma is accepted, which is what
   * toml-test's invalid/inline-table/trailing-comma case caught on the first
   * run this parser was ever scored.
   */
  bool after_comma;
} toml_frame;

static void frames_free(
    const GTEXT_Allocator * alloc, toml_frame * frames, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    gtext_allocator_free(alloc, frames[i].key);
  }
  gtext_allocator_free(alloc, frames);
}

/**
 * Resolve the intermediate segments of a dotted key inside a table.
 *
 * Creates what is missing as a dotted table, and refuses what TOML refuses:
 * reaching into a table a header defined, or into anything written inline.
 *
 * @param stop How many segments to resolve; the caller keeps the rest.
 * @return The table the final segment belongs in, or NULL with an error set.
 */
static GTEXT_TOML_Value * resolve_dotted(
    toml_ctx * ctx, GTEXT_TOML_Value * table, toml_path * path, size_t stop) {
  GTEXT_TOML_Value * current = table;
  for (size_t i = 0; i < stop; ++i) {
    GTEXT_TOML_Value * next =
        toml_table_find(current, path->items[i].data, path->items[i].len);
    if (!next) {
      next = toml_value_new(ctx->alloc, GTEXT_TOML_TABLE);
      if (!next) {
        toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
        return NULL;
      }
      next->origin = TOML_TABLE_DOTTED;
      if (!toml_table_insert(ctx->alloc, current, path->items[i].data,
              path->items[i].len, next)) {
        gtext_toml_free(next);
        toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
        return NULL;
      }
      current = next;
      continue;
    }
    if (next->type != GTEXT_TOML_TABLE) {
      toml_fail(ctx, GTEXT_TOML_E_REDEFINE,
          "a dotted key cannot reach through something that is not a table");
      return NULL;
    }
    if (next->origin == TOML_TABLE_INLINE) {
      toml_fail(ctx, GTEXT_TOML_E_REDEFINE,
          "a table written inline is closed; nothing may be added to it");
      return NULL;
    }
    if (next->origin == TOML_TABLE_HEADER) {
      /* Measured against the reference rather than argued from the prose:
       * `[x]` / `[x.a]` / `[x]` ... `a.c = 1` is refused, and so is this. An
       * implicit table is a different matter and is allowed through above. */
      toml_fail(ctx, GTEXT_TOML_E_REDEFINE,
          "a dotted key cannot extend a table a header defined");
      return NULL;
    }
    current = next;
  }
  return current;
}

/**
 * Parse one value, which may be a whole nest of arrays and inline tables.
 *
 * Containers are attached to their parent when they are created rather than
 * when they are finished, so the tree is connected at every moment and a
 * failure anywhere is cleaned up by freeing the root. The alternative - each
 * frame owning an unattached container - leaks one container per frame on the
 * first error path anybody forgets.
 *
 * @param root_out Receives the value on success.
 * @return false with an error recorded on failure.
 */
static bool parse_value(toml_ctx * ctx, GTEXT_TOML_Value ** root_out) {
  toml_frame * frames = NULL;
  size_t count = 0;
  size_t capacity = 0;
  GTEXT_TOML_Value * root = NULL;
  bool ok = false;

#define TOML_FRAME_PUSH(container_value)                                       \
  do {                                                                         \
    if (count == capacity) {                                                    \
      size_t want = capacity ? capacity * 2 : 8;                                \
      toml_frame * grown = gtext_allocator_realloc(                             \
          ctx->alloc, frames, want * sizeof(toml_frame));                       \
      if (!grown) {                                                             \
        toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");                       \
        goto done;                                                              \
      }                                                                         \
      frames = grown;                                                           \
      capacity = want;                                                          \
    }                                                                           \
    memset(&frames[count], 0, sizeof(toml_frame));                              \
    frames[count].container = (container_value);                                 \
    count++;                                                                    \
  } while (0)

  /* Attach a finished value to the innermost open container. */
  enum { WANT_VALUE, WANT_KEY, AFTER_VALUE } state = WANT_VALUE;

  for (;;) {
    if (state == WANT_VALUE) {
      bool in_array = count > 0
          && frames[count - 1].container->type == GTEXT_TOML_ARRAY;
      if (in_array) {
        if (!skip_array_space(ctx)) goto done;
      }
      else {
        skip_blanks(ctx);
      }
      if (ctx->pos >= ctx->len) {
        toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN, "a value was expected");
        goto done;
      }
      char c = ctx->buf[ctx->pos];

      if (in_array && c == ']') {
        /* An empty array, or the close after a trailing comma - which TOML
         * 1.0.0 allows in an array and not in an inline table. */
        ctx->pos++;
        count--;
        gtext_allocator_free(ctx->alloc, frames[count].key);
        if (count == 0) {
          ok = true;
          goto done;
        }
        state = AFTER_VALUE;
        continue;
      }

      if (c == '[' || c == '{') {
        if (ctx->max_depth && count + 1 > ctx->max_depth) {
          toml_fail(ctx, GTEXT_TOML_E_DEPTH,
              "nested deeper than max_depth allows");
          goto done;
        }
        GTEXT_TOML_Value * container = toml_value_new(ctx->alloc,
            c == '[' ? GTEXT_TOML_ARRAY : GTEXT_TOML_TABLE);
        if (!container) {
          toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
          goto done;
        }
        if (c == '[') {
          container->origin = TOML_ARRAY_STATIC;
        }
        else {
          container->origin = TOML_TABLE_INLINE;
        }
        ctx->pos++;

        if (count == 0) {
          root = container;
        }
        else if (frames[count - 1].container->type == GTEXT_TOML_ARRAY) {
          if (!toml_array_push(
                  ctx->alloc, frames[count - 1].container, container)) {
            gtext_toml_free(container);
            toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
            goto done;
          }
        }
        else {
          if (!toml_table_insert(ctx->alloc, frames[count - 1].target,
                  frames[count - 1].key, frames[count - 1].key_len,
                  container)) {
            gtext_toml_free(container);
            toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
            goto done;
          }
          gtext_allocator_free(ctx->alloc, frames[count - 1].key);
          frames[count - 1].key = NULL;
          frames[count - 1].have_key = false;
        }
        TOML_FRAME_PUSH(container);
        state = (c == '[') ? WANT_VALUE : WANT_KEY;
        continue;
      }

      GTEXT_TOML_Value * scalar = NULL;
      if (c == '"' || c == '\'') {
        toml_buf text = {0};
        if (!toml_scan_string(ctx, &text)) {
          toml_buf_free(ctx->alloc, &text);
          goto done;
        }
        scalar = toml_value_new(ctx->alloc, GTEXT_TOML_STRING);
        if (!scalar) {
          toml_buf_free(ctx->alloc, &text);
          toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
          goto done;
        }
        /* An empty string still owns an allocation, so that the accessor can
         * return a non-NULL pointer for it: NULL there would mean "not a
         * string", and `a = ""` is a string. */
        if (!text.data) {
          if (!toml_buf_append(ctx->alloc, &text, "", 0)) {
            toml_buf_free(ctx->alloc, &text);
            gtext_toml_free(scalar);
            toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
            goto done;
          }
        }
        scalar->as.string.data = text.data;
        scalar->as.string.len = text.len;
      }
      else {
        scalar = toml_scan_atom(ctx);
        if (!scalar) goto done;
      }

      if (count == 0) {
        root = scalar;
        ok = true;
        goto done;
      }
      if (frames[count - 1].container->type == GTEXT_TOML_ARRAY) {
        if (!toml_array_push(ctx->alloc, frames[count - 1].container, scalar)) {
          gtext_toml_free(scalar);
          toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
          goto done;
        }
      }
      else {
        if (!toml_table_insert(ctx->alloc, frames[count - 1].target,
                frames[count - 1].key, frames[count - 1].key_len, scalar)) {
          gtext_toml_free(scalar);
          toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
          goto done;
        }
        gtext_allocator_free(ctx->alloc, frames[count - 1].key);
        frames[count - 1].key = NULL;
        frames[count - 1].have_key = false;
      }
      state = AFTER_VALUE;
      continue;
    }

    if (state == WANT_KEY) {
      /* Inside an inline table, just after `{` or just after a comma - the
       * third and last place the version option reaches. TOML 1.0.0: "inline
       * tables are intended to appear on a single line", and a newline inside
       * the braces is invalid; 1.1.0's `inline-table-open` and
       * `inline-table-sep` are each followed by `ws-comment-newline`, so both
       * positions take a comment and a line break there. Whichever version, the
       * relaxation stops at the brace and the comma: `keyval-sep` is still
       * plain `ws`, so a pair may not be split across lines, and that is why
       * this is two different skippers and not one flag on skip_blanks(). */
      if (!inline_space(ctx)) goto done;
      if (ctx->pos >= ctx->len) {
        toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN,
            "the document ends inside an inline table");
        goto done;
      }
      char c = ctx->buf[ctx->pos];
      if (c == '}') {
        if (frames[count - 1].after_comma
            && ctx->version != GTEXT_TOML_VERSION_1_1_0) {
          /* TOML 1.0.0's inline-table production has no trailing comma; 1.1.0
           * adds one. */
          toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN,
              "a TOML 1.0.0 inline table cannot end with a comma");
          goto done;
        }
        ctx->pos++;
        count--;
        gtext_allocator_free(ctx->alloc, frames[count].key);
        if (count == 0) {
          ok = true;
          goto done;
        }
        state = AFTER_VALUE;
        continue;
      }
      if (c == '\n' || c == '\r') {
        /* Reachable only at 1.0.0, and deliberately not guarded on the version
         * as well: at 1.1.0 inline_space() consumed every newline and refused a
         * lone CR itself, so a second condition here would be a guard nothing
         * could make fire. */
        toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN,
            "a TOML 1.0.0 inline table cannot span lines");
        goto done;
      }

      toml_path path = {0};
      size_t key_start = ctx->pos;
      if (!scan_key_path(ctx, &path)) {
        path_free(ctx->alloc, &path);
        goto done;
      }
      GTEXT_TOML_Value * target = resolve_dotted(
          ctx, frames[count - 1].container, &path, path.count - 1);
      if (!target) {
        path_free(ctx->alloc, &path);
        goto done;
      }
      toml_seg * last = &path.items[path.count - 1];
      if (toml_table_find(target, last->data, last->len)) {
        toml_fail_at(ctx, key_start, GTEXT_TOML_E_DUPKEY,
            "that key is already defined");
        path_free(ctx->alloc, &path);
        goto done;
      }
      char * key = gtext_allocator_malloc(ctx->alloc, last->len + 1);
      if (!key) {
        path_free(ctx->alloc, &path);
        toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
        goto done;
      }
      if (last->len) memcpy(key, last->data, last->len);
      key[last->len] = '\0';
      frames[count - 1].target = target;
      gtext_allocator_free(ctx->alloc, frames[count - 1].key);
      frames[count - 1].key = key;
      frames[count - 1].key_len = last->len;
      frames[count - 1].have_key = true;
      frames[count - 1].after_comma = false;
      path_free(ctx->alloc, &path);

      skip_blanks(ctx);
      if (ctx->pos >= ctx->len || ctx->buf[ctx->pos] != '=') {
        toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN, "a key must be followed by '='");
        goto done;
      }
      ctx->pos++;
      state = WANT_VALUE;
      continue;
    }

    /* AFTER_VALUE: a separator or a closing bracket. */
    if (count == 0) {
      ok = true;
      goto done;
    }
    if (frames[count - 1].container->type == GTEXT_TOML_ARRAY) {
      if (!skip_array_space(ctx)) goto done;
      if (ctx->pos >= ctx->len) {
        toml_fail(
            ctx, GTEXT_TOML_E_BAD_TOKEN, "the document ends inside an array");
        goto done;
      }
      if (ctx->buf[ctx->pos] == ',') {
        ctx->pos++;
        state = WANT_VALUE;
        continue;
      }
      if (ctx->buf[ctx->pos] == ']') {
        ctx->pos++;
        count--;
        gtext_allocator_free(ctx->alloc, frames[count].key);
        if (count == 0) {
          ok = true;
          goto done;
        }
        state = AFTER_VALUE;
        continue;
      }
      toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN,
          "array items are separated by commas");
      goto done;
    }
    /* Before the comma or the closing brace, which is the other half of
     * `inline-table-sep` and of `inline-table-close`. */
    if (!inline_space(ctx)) goto done;
    if (ctx->pos >= ctx->len) {
      toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN,
          "the document ends inside an inline table");
      goto done;
    }
    if (ctx->buf[ctx->pos] == ',') {
      ctx->pos++;
      frames[count - 1].after_comma = true;
      state = WANT_KEY;
      continue;
    }
    if (ctx->buf[ctx->pos] == '}') {
      ctx->pos++;
      count--;
      gtext_allocator_free(ctx->alloc, frames[count].key);
      if (count == 0) {
        ok = true;
        goto done;
      }
      state = AFTER_VALUE;
      continue;
    }
    toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN,
        "inline table pairs are separated by commas");
    goto done;
  }

done:
  frames_free(ctx->alloc, frames, count);
  if (!ok) {
    gtext_toml_free(root);
    return false;
  }
  *root_out = root;
  return true;
#undef TOML_FRAME_PUSH
}

/*--------------------------------------------------------------------------*
 * Statements
 *--------------------------------------------------------------------------*/

/** A key-value line, in whichever table is current. */
static bool parse_key_value(toml_ctx * ctx, GTEXT_TOML_Value * current) {
  toml_path path = {0};
  size_t key_start = ctx->pos;
  if (!scan_key_path(ctx, &path)) {
    path_free(ctx->alloc, &path);
    return false;
  }
  skip_blanks(ctx);
  if (ctx->pos >= ctx->len || ctx->buf[ctx->pos] != '=') {
    path_free(ctx->alloc, &path);
    return toml_fail(
        ctx, GTEXT_TOML_E_BAD_TOKEN, "a key must be followed by '='");
  }
  ctx->pos++;

  GTEXT_TOML_Value * target =
      resolve_dotted(ctx, current, &path, path.count - 1);
  if (!target) {
    path_free(ctx->alloc, &path);
    return false;
  }
  toml_seg * last = &path.items[path.count - 1];
  if (toml_table_find(target, last->data, last->len)) {
    /* Where the key began, not where the parser has reached: a duplicate is a
     * statement about the key, and by now the parser is past the '='. The
     * first version of this said so in a comment and called toml_fail(), which
     * reports ctx->pos - so it pointed four columns to the right of the key it
     * was complaining about, and the test that asked for column 1 is what
     * found it. */
    toml_fail_at(ctx, key_start, GTEXT_TOML_E_DUPKEY,
        "that key is already defined");
    path_free(ctx->alloc, &path);
    return false;
  }

  GTEXT_TOML_Value * value = NULL;
  if (!parse_value(ctx, &value)) {
    path_free(ctx->alloc, &path);
    return false;
  }
  if (!toml_table_insert(ctx->alloc, target, last->data, last->len, value)) {
    gtext_toml_free(value);
    path_free(ctx->alloc, &path);
    return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
  }
  path_free(ctx->alloc, &path);
  return true;
}

/**
 * Walk the ancestors of a header's path, creating what is missing.
 *
 * An array grown by `[[a]]` is descended into at its last element, which is
 * what makes `[[a]]` / `[a.b]` name a table inside the newest element.
 */
static GTEXT_TOML_Value * walk_header_ancestors(
    toml_ctx * ctx, GTEXT_TOML_Value * root, toml_path * path) {
  GTEXT_TOML_Value * current = root;
  for (size_t i = 0; i + 1 < path->count; ++i) {
    GTEXT_TOML_Value * next =
        toml_table_find(current, path->items[i].data, path->items[i].len);
    if (!next) {
      next = toml_value_new(ctx->alloc, GTEXT_TOML_TABLE);
      if (!next) {
        toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
        return NULL;
      }
      next->origin = TOML_TABLE_IMPLICIT;
      if (!toml_table_insert(ctx->alloc, current, path->items[i].data,
              path->items[i].len, next)) {
        gtext_toml_free(next);
        toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
        return NULL;
      }
      current = next;
      continue;
    }
    if (next->type == GTEXT_TOML_ARRAY) {
      if (next->origin != TOML_ARRAY_OF_TABLES || next->as.array.count == 0) {
        toml_fail(ctx, GTEXT_TOML_E_REDEFINE,
            "a header cannot reach into an array that [[...]] did not build");
        return NULL;
      }
      current = next->as.array.items[next->as.array.count - 1];
      continue;
    }
    if (next->type != GTEXT_TOML_TABLE) {
      toml_fail(ctx, GTEXT_TOML_E_REDEFINE,
          "a header cannot reach through something that is not a table");
      return NULL;
    }
    if (next->origin == TOML_TABLE_INLINE) {
      toml_fail(ctx, GTEXT_TOML_E_REDEFINE,
          "a table written inline is closed; nothing may be added to it");
      return NULL;
    }
    current = next;
  }
  return current;
}

/** A `[header]` or `[[header]]` line. `current` is set to the new scope. */
static bool parse_header(
    toml_ctx * ctx, GTEXT_TOML_Value * root, GTEXT_TOML_Value ** current) {
  ctx->pos++; /* the '[' */
  bool array_of_tables = ctx->pos < ctx->len && ctx->buf[ctx->pos] == '[';
  if (array_of_tables) ctx->pos++;
  skip_blanks(ctx);

  toml_path path = {0};
  if (!scan_key_path(ctx, &path)) {
    path_free(ctx->alloc, &path);
    return false;
  }
  skip_blanks(ctx);
  if (ctx->pos >= ctx->len || ctx->buf[ctx->pos] != ']') {
    path_free(ctx->alloc, &path);
    return toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN, "a header must be closed");
  }
  ctx->pos++;
  if (array_of_tables) {
    if (ctx->pos >= ctx->len || ctx->buf[ctx->pos] != ']') {
      path_free(ctx->alloc, &path);
      return toml_fail(
          ctx, GTEXT_TOML_E_BAD_TOKEN, "an array-of-tables header needs ']]'");
    }
    ctx->pos++;
  }

  GTEXT_TOML_Value * parent = walk_header_ancestors(ctx, root, &path);
  if (!parent) {
    path_free(ctx->alloc, &path);
    return false;
  }
  toml_seg * last = &path.items[path.count - 1];
  GTEXT_TOML_Value * existing = toml_table_find(parent, last->data, last->len);

  if (array_of_tables) {
    GTEXT_TOML_Value * array = existing;
    if (!array) {
      array = toml_value_new(ctx->alloc, GTEXT_TOML_ARRAY);
      if (!array) {
        path_free(ctx->alloc, &path);
        return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
      }
      array->origin = TOML_ARRAY_OF_TABLES;
      if (!toml_table_insert(
              ctx->alloc, parent, last->data, last->len, array)) {
        gtext_toml_free(array);
        path_free(ctx->alloc, &path);
        return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
      }
    }
    else if (array->type != GTEXT_TOML_ARRAY
        || array->origin != TOML_ARRAY_OF_TABLES) {
      /* A statically written `a = []` is not an array of tables, and TOML says
       * so: "Attempting to append to a statically defined array, even if that
       * array is empty, must produce an error at parse time." */
      path_free(ctx->alloc, &path);
      return toml_fail(ctx, GTEXT_TOML_E_REDEFINE,
          "[[...]] can only append to an array it built itself");
    }
    GTEXT_TOML_Value * element = toml_value_new(ctx->alloc, GTEXT_TOML_TABLE);
    if (!element) {
      path_free(ctx->alloc, &path);
      return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    }
    element->origin = TOML_TABLE_HEADER;
    if (!toml_array_push(ctx->alloc, array, element)) {
      gtext_toml_free(element);
      path_free(ctx->alloc, &path);
      return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    }
    *current = element;
    path_free(ctx->alloc, &path);
    return true;
  }

  if (!existing) {
    GTEXT_TOML_Value * table = toml_value_new(ctx->alloc, GTEXT_TOML_TABLE);
    if (!table) {
      path_free(ctx->alloc, &path);
      return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    }
    table->origin = TOML_TABLE_HEADER;
    if (!toml_table_insert(ctx->alloc, parent, last->data, last->len, table)) {
      gtext_toml_free(table);
      path_free(ctx->alloc, &path);
      return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    }
    *current = table;
    path_free(ctx->alloc, &path);
    return true;
  }
  if (existing->type != GTEXT_TOML_TABLE) {
    path_free(ctx->alloc, &path);
    return toml_fail(ctx, GTEXT_TOML_E_REDEFINE,
        "that name is already something other than a table");
  }
  if (existing->origin != TOML_TABLE_IMPLICIT) {
    /* The one origin a header may take over is implicit - a table that exists
     * only because something below it was defined. That is the specification's
     * "you can define a super-table afterwards" case, and the three other
     * origins are each a definition that this would be a second of. */
    path_free(ctx->alloc, &path);
    return toml_fail(
        ctx, GTEXT_TOML_E_REDEFINE, "that table is already defined");
  }
  existing->origin = TOML_TABLE_HEADER;
  *current = existing;
  path_free(ctx->alloc, &path);
  return true;
}

/*--------------------------------------------------------------------------*
 * Entry point
 *--------------------------------------------------------------------------*/

GTEXT_TOML_Value * gtext_toml_parse(const char * bytes, size_t len,
    const GTEXT_TOML_Parse_Options * opts, GTEXT_TOML_Error * err) {
  if (err) {
    memset(err, 0, sizeof(*err));
  }
  GTEXT_TOML_Parse_Options effective = gtext_toml_parse_options_default();
  if (opts) effective = *opts;

  toml_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.buf = bytes;
  ctx.len = len;
  ctx.line = 1;
  ctx.alloc = effective.allocator;
  ctx.err = err;
  ctx.max_depth = effective.max_depth;
  ctx.version = effective.version;

  if (!bytes) {
    toml_fail(&ctx, GTEXT_TOML_E_INVALID, "no input");
    return NULL;
  }
  if (effective.max_total_bytes && len > effective.max_total_bytes) {
    toml_fail(&ctx, GTEXT_TOML_E_LIMIT, "longer than max_total_bytes allows");
    return NULL;
  }

  /* Validated once, up front, rather than character by character as each
   * scanner reaches it. TOML says a document must be valid UTF-8, and that is
   * a property of the whole document including its comments and the bytes
   * between its tokens - which no scanner looks at. */
  size_t bad = 0;
  if (!toml_utf8_validate(bytes, len, &bad)) {
    toml_fail_at(&ctx, bad, GTEXT_TOML_E_BAD_UNICODE,
        "not valid UTF-8; a TOML document must be");
    return NULL;
  }
  if (len >= 3 && (unsigned char) bytes[0] == 0xEF
      && (unsigned char) bytes[1] == 0xBB && (unsigned char) bytes[2] == 0xBF) {
    toml_fail_at(&ctx, 0, GTEXT_TOML_E_BAD_TOKEN,
        "a byte order mark is not part of a TOML document");
    return NULL;
  }

  GTEXT_TOML_Value * root = toml_value_new(ctx.alloc, GTEXT_TOML_TABLE);
  if (!root) {
    toml_fail(&ctx, GTEXT_TOML_E_OOM, "out of memory");
    return NULL;
  }
  root->origin = TOML_TABLE_HEADER;
  GTEXT_TOML_Value * current = root;

  for (;;) {
    skip_blanks(&ctx);
    if (ctx.pos >= ctx.len) break;
    char c = ctx.buf[ctx.pos];
    if (c == '\n' || c == '\r') {
      if (!take_newline(&ctx)) goto failed;
      continue;
    }
    if (c == '#') {
      if (!skip_comment(&ctx)) goto failed;
      continue;
    }
    if (c == '[') {
      if (!parse_header(&ctx, root, &current)) goto failed;
    }
    else {
      if (!parse_key_value(&ctx, current)) goto failed;
    }
    if (!finish_statement(&ctx)) goto failed;
  }
  return root;

failed:
  gtext_toml_free(root);
  return NULL;
}
