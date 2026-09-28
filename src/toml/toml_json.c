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
 * @file toml_json.c
 * @brief Converting between a TOML tree and a JSON one.
 *
 * Both walks are on an explicit stack, as every other walk in this module is.
 * That is not caution about these two functions in particular: `text`'s
 * contract for TOML is that no document it accepted can crash it, and a
 * conversion that recursed would put that crash back for a document this
 * library itself handed the caller.
 *
 * The one thing neither walk can promise is the *other* module's teardown.
 * gtext_json_free() recurses, so the JSON tree this builds is bounded by
 * GTEXT_TOML_To_JSON_Options::max_depth rather than by memory - the header says
 * why that limit is not a copy of the parse's.
 */

#include "../text_number_internal.h"
#include "toml_internal.h"
#include <errno.h>
#include <ghoti.io/text/toml/toml_json.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/** Record a conversion failure. Always returns @p code, so a caller can
 *  `return conv_fail(...)`. */
static GTEXT_TOML_Status conv_fail(
    GTEXT_TOML_Error * err, GTEXT_TOML_Status code, const char * message) {
  if (err && err->code == GTEXT_TOML_OK) {
    err->code = code;
    err->message = message;
    /* Zero, not one: these three name a position in an input buffer and there
     * is no input buffer here. A line of 1 would be a number a caller could
     * print and could not look at. */
    err->offset = 0;
    err->line = 0;
    err->col = 0;
  }
  return code;
}

GTEXT_TOML_To_JSON_Options gtext_toml_to_json_options_default(void) {
  GTEXT_TOML_To_JSON_Options opts;
  /* A date-time converts and a non-finite float does not, and the asymmetry is
   * the one the header argues: `"1979-05-27"` still holds the value, and no
   * JSON spelling of `nan` holds anything. */
  opts.datetime = GTEXT_TOML_JSON_DATETIME_STRING;
  opts.nonfinite = GTEXT_TOML_JSON_NONFINITE_ERROR;
  opts.max_depth = 256;
  return opts;
}

GTEXT_TOML_From_JSON_Options gtext_toml_from_json_options_default(void) {
  GTEXT_TOML_From_JSON_Options opts;
  opts.allocator = NULL;
  opts.null_values = GTEXT_TOML_JSON_NULL_ERROR;
  opts.max_depth = 256;
  return opts;
}

/*==========================================================================*
 * TOML to JSON
 *==========================================================================*/

/** One container being converted, and the container being built for it. */
typedef struct {
  const GTEXT_TOML_Value * src; ///< The TOML table or array being read.
  GTEXT_JSON_Value * dst;       ///< The JSON object or array being filled.
  size_t index;                 ///< How much of `src` has been copied.
} t2j_frame;

static bool t2j_push(t2j_frame ** stack, size_t * count, size_t * capacity,
    const GTEXT_TOML_Value * src, GTEXT_JSON_Value * dst) {
  if (*count == *capacity) {
    size_t want = *capacity ? *capacity * 2 : 16;
    if (want > SIZE_MAX / sizeof(t2j_frame)) return false;
    t2j_frame * grown =
        gtext_allocator_realloc(NULL, *stack, want * sizeof(t2j_frame));
    if (!grown) return false;
    *stack = grown;
    *capacity = want;
  }
  (*stack)[*count].src = src;
  (*stack)[*count].dst = dst;
  (*stack)[*count].index = 0;
  (*count)++;
  return true;
}

/** One TOML scalar as a JSON value, or NULL with @p status set. */
static GTEXT_JSON_Value * t2j_scalar(const GTEXT_TOML_Value * value,
    const GTEXT_TOML_To_JSON_Options * opts, GTEXT_TOML_Status * status,
    GTEXT_TOML_Error * err) {
  switch (value->type) {
    case GTEXT_TOML_STRING:
      return gtext_json_new_string(value->as.string.data, value->as.string.len);
    case GTEXT_TOML_INTEGER:
      /* Not through a double: an int64 near its bounds is not exactly a
       * double, and JSON's number type is whatever a reader makes of the
       * lexeme. `_i64` keeps the digits. */
      return gtext_json_new_number_i64(value->as.integer);
    case GTEXT_TOML_BOOLEAN:
      return gtext_json_new_bool(value->as.boolean);
    case GTEXT_TOML_FLOAT: {
      if (isfinite(value->as.floating)) {
        return gtext_json_new_number_double(value->as.floating);
      }
      switch (opts->nonfinite) {
        case GTEXT_TOML_JSON_NONFINITE_STRING: {
          const char * text = isnan(value->as.floating)
              ? (signbit(value->as.floating) ? "-nan" : "nan")
              : (value->as.floating < 0 ? "-inf" : "inf");
          return gtext_json_new_string(text, strlen(text));
        }
        case GTEXT_TOML_JSON_NONFINITE_NULL:
          return gtext_json_new_null();
        default:
          *status = conv_fail(err, GTEXT_TOML_E_UNREPRESENTABLE,
              "JSON has no infinity and no NaN; see the nonfinite policy");
          return NULL;
      }
    }
    case GTEXT_TOML_DATETIME: {
      if (opts->datetime == GTEXT_TOML_JSON_DATETIME_ERROR) {
        *status = conv_fail(err, GTEXT_TOML_E_UNREPRESENTABLE,
            "JSON has no date-time type; see the datetime policy");
        return NULL;
      }
      /* The spelling gtext_toml_write() would have used, from the same call:
       * one date-time formatter in this module, not two. */
      char text[80];
      size_t n = 0;
      if (gchron_write_toml(&value->as.datetime, NULL, text, sizeof(text), &n)
          != GCHRON_OK) {
        *status = conv_fail(err, GTEXT_TOML_E_DATETIME,
            "chron will not spell that date-time");
        return NULL;
      }
      return gtext_json_new_string(text, n);
    }
    default:
      /* A container reached here, which the caller is supposed to have taken
       * care of. */
      *status = conv_fail(
          err, GTEXT_TOML_E_STATE, "a container was converted as a scalar");
      return NULL;
  }
}

/** Store a converted child where its parent frame says it goes. */
static GTEXT_TOML_Status t2j_place(const t2j_frame * frame,
    const char * key, size_t key_len, GTEXT_JSON_Value * child,
    GTEXT_TOML_Error * err) {
  GTEXT_JSON_Status s = frame->src->type == GTEXT_TOML_ARRAY
      ? gtext_json_array_push(frame->dst, child)
      : gtext_json_object_put(frame->dst, key, key_len, child);
  if (s != GTEXT_JSON_OK) {
    gtext_json_free(child);
    /* The JSON module's own status is not this module's, and the only way
     * these calls fail on a well-formed tree is an allocation. */
    return conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building JSON");
  }
  return GTEXT_TOML_OK;
}

GTEXT_TOML_Status gtext_toml_to_json(const GTEXT_TOML_Value * root,
    const GTEXT_TOML_To_JSON_Options * opts, GTEXT_JSON_Value ** out_json,
    GTEXT_TOML_Error * err) {
  if (err) memset(err, 0, sizeof(*err));
  if (!root || !out_json) {
    return conv_fail(err, GTEXT_TOML_E_INVALID, "no document, or nowhere to "
                                                "put the result");
  }
  *out_json = NULL;
  GTEXT_TOML_To_JSON_Options effective = gtext_toml_to_json_options_default();
  if (opts) effective = *opts;

  GTEXT_TOML_Status status = GTEXT_TOML_OK;
  if (root->type != GTEXT_TOML_TABLE && root->type != GTEXT_TOML_ARRAY) {
    GTEXT_JSON_Value * scalar = t2j_scalar(root, &effective, &status, err);
    if (!scalar) {
      return status == GTEXT_TOML_OK
          ? conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building JSON")
          : status;
    }
    *out_json = scalar;
    return GTEXT_TOML_OK;
  }

  GTEXT_JSON_Value * out = root->type == GTEXT_TOML_TABLE
      ? gtext_json_new_object()
      : gtext_json_new_array();
  if (!out) {
    return conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building JSON");
  }

  t2j_frame * stack = NULL;
  size_t count = 0;
  size_t capacity = 0;
  if (!t2j_push(&stack, &count, &capacity, root, out)) {
    gtext_json_free(out);
    return conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building JSON");
  }

  while (count && status == GTEXT_TOML_OK) {
    t2j_frame * frame = &stack[count - 1];
    const bool array = frame->src->type == GTEXT_TOML_ARRAY;
    const size_t n = array ? frame->src->as.array.count
                           : frame->src->as.table.count;
    if (frame->index >= n) {
      count--;
      continue;
    }
    const char * key = NULL;
    size_t key_len = 0;
    const GTEXT_TOML_Value * child;
    if (array) {
      child = frame->src->as.array.items[frame->index];
    }
    else {
      const toml_pair * pair = &frame->src->as.table.pairs[frame->index];
      child = pair->value;
      key = pair->key;
      key_len = pair->len;
    }
    frame->index++;

    if (child->type == GTEXT_TOML_TABLE || child->type == GTEXT_TOML_ARRAY) {
      if (effective.max_depth && count + 1 > effective.max_depth) {
        status = conv_fail(err, GTEXT_TOML_E_DEPTH,
            "nested deeper than max_depth allows");
        break;
      }
      GTEXT_JSON_Value * container = child->type == GTEXT_TOML_TABLE
          ? gtext_json_new_object()
          : gtext_json_new_array();
      if (!container) {
        status = conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building JSON");
        break;
      }
      /* Attached before it is descended into, so the tree is connected at every
       * moment and one gtext_json_free(out) releases whatever was built - the
       * same discipline the value parser uses. */
      status = t2j_place(frame, key, key_len, container, err);
      if (status != GTEXT_TOML_OK) break;
      if (!t2j_push(&stack, &count, &capacity, child, container)) {
        status = conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building JSON");
        break;
      }
      continue;
    }

    GTEXT_JSON_Value * scalar = t2j_scalar(child, &effective, &status, err);
    if (!scalar) {
      if (status == GTEXT_TOML_OK) {
        status = conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building JSON");
      }
      break;
    }
    /* `frame` is still valid: nothing above reallocated the stack. */
    status = t2j_place(frame, key, key_len, scalar, err);
  }

  gtext_allocator_free(NULL, stack);
  if (status != GTEXT_TOML_OK) {
    gtext_json_free(out);
    return status;
  }
  *out_json = out;
  return GTEXT_TOML_OK;
}

/*==========================================================================*
 * JSON to TOML
 *==========================================================================*/

/** The same, in the other direction. */
typedef struct {
  const GTEXT_JSON_Value * src; ///< The JSON object or array being read.
  GTEXT_TOML_Value * dst;       ///< The TOML table or array being filled.
  size_t index;                 ///< How much of `src` has been copied.
} j2t_frame;

static bool j2t_push(const GTEXT_Allocator * alloc, j2t_frame ** stack,
    size_t * count, size_t * capacity, const GTEXT_JSON_Value * src,
    GTEXT_TOML_Value * dst) {
  if (*count == *capacity) {
    size_t want = *capacity ? *capacity * 2 : 16;
    if (want > SIZE_MAX / sizeof(j2t_frame)) return false;
    j2t_frame * grown =
        gtext_allocator_realloc(alloc, *stack, want * sizeof(j2t_frame));
    if (!grown) return false;
    *stack = grown;
    *capacity = want;
  }
  (*stack)[*count].src = src;
  (*stack)[*count].dst = dst;
  (*stack)[*count].index = 0;
  (*count)++;
  return true;
}

/**
 * One JSON number as a TOML integer or float.
 *
 * **The lexeme is the number**, for the type and for the value both. Not
 * gtext_json_get_i64() and gtext_json_get_double(): those report what
 * representations a JSON value happens to *carry*, which is not the same
 * question and gave two wrong answers on the way here.
 *
 *   - A JSON number this library built from a double carries a double and a
 *     lexeme and no int64, so every whole-valued TOML float converted to JSON
 *     and back reached the integer branch with get_i64() failing. Read as "out
 *     of range" that refused eight corpus cases holding 1000.
 *   - Falling back to the double then rounds at the bounds: -9223372036854775809
 *     is not an int64, and as a double it is exactly -2^63, so a range test on
 *     the double accepts it as INT64_MIN. That is `archive`'s defect - one bound
 *     spelled against INT64_MAX and the other against INT64_MAX + 1 - arriving
 *     from the other side.
 *
 * The text has neither problem: strtoll on the digits reports ERANGE for the
 * literal that does not fit, whichever side of zero it is on, and a lexeme with
 * a point or an exponent is a float because that is what the text says - the
 * same question TOML asks of the same characters.
 *
 * strtoll and gtext_number_strtod for the two, exactly as the TOML lexer does:
 * strtoll reads base-ten digits and no locale can move them, and strtod *is*
 * locale-sensitive, which is what gtext_number_strtod() exists to fix.
 */
static GTEXT_TOML_Value * j2t_number(const GTEXT_JSON_Value * value,
    const GTEXT_Allocator * alloc, GTEXT_TOML_Status * status,
    GTEXT_TOML_Error * err) {
  const char * lexeme = NULL;
  size_t len = 0;
  if (gtext_json_get_number_lexeme(value, &lexeme, &len) != GTEXT_JSON_OK
      || !lexeme) {
    *status = conv_fail(err, GTEXT_TOML_E_INVALID,
        "a JSON number with no lexeme; parse with preserve_number_lexeme");
    return NULL;
  }
  bool floating = false;
  for (size_t i = 0; i < len; ++i) {
    if (lexeme[i] == '.' || lexeme[i] == 'e' || lexeme[i] == 'E') {
      floating = true;
      break;
    }
  }
  /* Copied rather than used in place: a lexeme is bytes and a length, and
   * nothing promises a NUL after it. 64 is past every int64 spelling and every
   * double's shortest form; anything longer is a number neither type can hold,
   * and for the integer branch that is E_RANGE by definition. */
  char text[64];
  if (len >= sizeof(text)) {
    if (floating) {
      /* A float lexeme may legitimately be long - a caller's own
       * gtext_json_new_number_from_lexeme() can hold any digits at all - and
       * the value is what a reader makes of it, so the tail beyond the
       * precision of a double changes nothing. Truncating it would, so this
       * refuses instead of guessing. */
      *status = conv_fail(err, GTEXT_TOML_E_RANGE,
          "a number lexeme longer than any double needs");
      return NULL;
    }
    *status = conv_fail(err, GTEXT_TOML_E_RANGE,
        "an integer outside int64_t, which is what a TOML integer is");
    return NULL;
  }
  memcpy(text, lexeme, len);
  text[len] = '\0';

  if (floating) {
    char * end = NULL;
    double d = gtext_number_strtod(text, &end);
    return gtext_toml_new_float(alloc, d);
  }
  errno = 0;
  char * end = NULL;
  long long raw = strtoll(text, &end, 10);
  if (errno == ERANGE) {
    *status = conv_fail(err, GTEXT_TOML_E_RANGE,
        "an integer outside int64_t, which is what a TOML integer is");
    return NULL;
  }
  return gtext_toml_new_integer(alloc, (int64_t) raw);
}

/**
 * One JSON scalar as a TOML value, or NULL.
 *
 * @param skipped Set when the value was a `null` the policy drops, which is the
 *   one way this returns NULL without a failure. A `bool` rather than "NULL and
 *   the status is still OK", because that reading makes a failed allocation -
 *   also NULL, also nothing set - indistinguishable from a deliberate skip, and
 *   the consequence would be a silently missing key rather than an error.
 */
static GTEXT_TOML_Value * j2t_scalar(const GTEXT_JSON_Value * value,
    const GTEXT_TOML_From_JSON_Options * opts, bool in_array, bool * skipped,
    GTEXT_TOML_Status * status, GTEXT_TOML_Error * err) {
  *skipped = false;
  switch (gtext_json_typeof(value)) {
    case GTEXT_JSON_STRING: {
      const char * text = NULL;
      size_t len = 0;
      if (gtext_json_get_string(value, &text, &len) != GTEXT_JSON_OK) {
        *status = conv_fail(
            err, GTEXT_TOML_E_INVALID, "a JSON string with no bytes");
        return NULL;
      }
      return gtext_toml_new_string(opts->allocator, text, len);
    }
    case GTEXT_JSON_BOOL: {
      bool b = false;
      if (gtext_json_get_bool(value, &b) != GTEXT_JSON_OK) {
        *status = conv_fail(
            err, GTEXT_TOML_E_INVALID, "a JSON boolean with no value");
        return NULL;
      }
      return gtext_toml_new_boolean(opts->allocator, b);
    }
    case GTEXT_JSON_NUMBER:
      return j2t_number(value, opts->allocator, status, err);
    case GTEXT_JSON_NULL:
      if (opts->null_values == GTEXT_TOML_JSON_NULL_SKIP && !in_array) {
        *skipped = true;
        return NULL;
      }
      *status = conv_fail(err, GTEXT_TOML_E_UNREPRESENTABLE,
          in_array ? "a null inside an array; dropping it would shorten the "
                     "array"
                   : "TOML has no null; see the null policy");
      return NULL;
    default:
      *status = conv_fail(
          err, GTEXT_TOML_E_STATE, "a container was converted as a scalar");
      return NULL;
  }
}

static GTEXT_TOML_Status j2t_place(const j2t_frame * frame, const char * key,
    size_t key_len, GTEXT_TOML_Value * child, GTEXT_TOML_Error * err) {
  GTEXT_TOML_Status s = frame->dst->type == GTEXT_TOML_ARRAY
      ? gtext_toml_array_append(frame->dst, child)
      : gtext_toml_table_set(frame->dst, key, key_len, child);
  if (s != GTEXT_TOML_OK) {
    gtext_toml_free(child);
    /* E_DUPKEY is reachable here and is not this conversion's mistake: a JSON
     * object may hold one name twice, since RFC 8259 only SHOULD-s uniqueness
     * and this library's parser has policies that keep both. TOML has no
     * spelling for that document, so the status passes through as it is. */
    return conv_fail(err, s,
        s == GTEXT_TOML_E_DUPKEY
            ? "a JSON object holds that name twice, and TOML has one key"
            : "could not store a converted value");
  }
  return GTEXT_TOML_OK;
}

GTEXT_TOML_Status gtext_json_to_toml(const GTEXT_JSON_Value * json,
    const GTEXT_TOML_From_JSON_Options * opts, GTEXT_TOML_Value ** out_root,
    GTEXT_TOML_Error * err) {
  if (err) memset(err, 0, sizeof(*err));
  if (!json || !out_root) {
    return conv_fail(err, GTEXT_TOML_E_INVALID, "no value, or nowhere to put "
                                                "the result");
  }
  *out_root = NULL;
  GTEXT_TOML_From_JSON_Options effective =
      gtext_toml_from_json_options_default();
  if (opts) effective = *opts;

  /* v1.0.0: "a TOML document is a table". An array or a scalar at the top is a
   * perfectly good JSON value with no TOML document to be, which is what
   * E_UNREPRESENTABLE says and E_INVALID would not. */
  if (gtext_json_typeof(json) != GTEXT_JSON_OBJECT) {
    return conv_fail(err, GTEXT_TOML_E_UNREPRESENTABLE,
        "a TOML document is a table, so the JSON root must be an object");
  }

  GTEXT_TOML_Value * out =
      gtext_toml_new_table(effective.allocator);
  if (!out) {
    return conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building TOML");
  }

  j2t_frame * stack = NULL;
  size_t count = 0;
  size_t capacity = 0;
  GTEXT_TOML_Status status = GTEXT_TOML_OK;
  if (!j2t_push(
          effective.allocator, &stack, &count, &capacity, json, out)) {
    gtext_toml_free(out);
    return conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building TOML");
  }

  while (count && status == GTEXT_TOML_OK) {
    j2t_frame * frame = &stack[count - 1];
    const bool array = gtext_json_typeof(frame->src) == GTEXT_JSON_ARRAY;
    const size_t n = array ? gtext_json_array_size(frame->src)
                           : gtext_json_object_size(frame->src);
    if (frame->index >= n) {
      count--;
      continue;
    }
    const char * key = NULL;
    size_t key_len = 0;
    const GTEXT_JSON_Value * child;
    if (array) {
      child = gtext_json_array_get(frame->src, frame->index);
    }
    else {
      key = gtext_json_object_key(frame->src, frame->index, &key_len);
      child = gtext_json_object_value(frame->src, frame->index);
      if (!key) {
        status = conv_fail(
            err, GTEXT_TOML_E_INVALID, "a JSON object member with no name");
        break;
      }
    }
    frame->index++;
    if (!child) {
      status = conv_fail(
          err, GTEXT_TOML_E_INVALID, "a JSON container with a missing member");
      break;
    }

    const GTEXT_JSON_Type type = gtext_json_typeof(child);
    if (type == GTEXT_JSON_OBJECT || type == GTEXT_JSON_ARRAY) {
      if (effective.max_depth && count + 1 > effective.max_depth) {
        status = conv_fail(err, GTEXT_TOML_E_DEPTH,
            "nested deeper than max_depth allows");
        break;
      }
      GTEXT_TOML_Value * container = type == GTEXT_JSON_OBJECT
          ? gtext_toml_new_table(effective.allocator)
          : gtext_toml_new_array(effective.allocator);
      if (!container) {
        status = conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building TOML");
        break;
      }
      status = j2t_place(frame, key, key_len, container, err);
      if (status != GTEXT_TOML_OK) break;
      if (!j2t_push(effective.allocator, &stack, &count, &capacity, child,
              container)) {
        status = conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building TOML");
        break;
      }
      continue;
    }

    bool skipped = false;
    GTEXT_TOML_Value * scalar =
        j2t_scalar(child, &effective, array, &skipped, &status, err);
    if (!scalar) {
      if (skipped) continue;
      if (status == GTEXT_TOML_OK) {
        status = conv_fail(err, GTEXT_TOML_E_OOM, "out of memory building TOML");
      }
      break;
    }
    status = j2t_place(frame, key, key_len, scalar, err);
  }

  gtext_allocator_free(effective.allocator, stack);
  if (status != GTEXT_TOML_OK) {
    gtext_toml_free(out);
    return status;
  }
  *out_root = out;
  return GTEXT_TOML_OK;
}
