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
 * YAML writer infrastructure implementation.
 *
 * This file implements the sink abstraction for writing YAML output
 * to various destinations.
 */

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/yaml/yaml_writer.h>
#include "yaml_internal.h"

static int buffer_write_fn(void * user, const char * bytes, size_t len) {
  GTEXT_YAML_Buffer_Sink * buf = (GTEXT_YAML_Buffer_Sink *)user;
  if (!buf || !bytes) {
    return 1;
  }

  if (len > SIZE_MAX - buf->used || buf->used > SIZE_MAX - len - 1) {
    return 1;
  }

  size_t needed = buf->used + len + 1;
  if (needed > buf->size) {
    size_t new_size = buf->size;
    if (new_size == 0) {
      new_size = 256;
    }
    while (new_size < needed) {
      if (new_size > SIZE_MAX / 2) {
        return 1;
      }
      new_size *= 2;
    }

    char * new_data = (char *)realloc(buf->data, new_size);
    if (!new_data) {
      return 1;
    }
    buf->data = new_data;
    buf->size = new_size;
  }

  if (buf->used + len > buf->size - 1) {
    return 1;
  }

  memcpy(buf->data + buf->used, bytes, len);
  buf->used += len;
  if (buf->used < buf->size) {
    buf->data[buf->used] = '\0';
  }

  return 0;
}

static int fixed_buffer_write_fn(void * user, const char * bytes, size_t len) {
  GTEXT_YAML_Fixed_Buffer_Sink * buf = (GTEXT_YAML_Fixed_Buffer_Sink *)user;
  if (!buf || !bytes) {
    return 1;
  }

  size_t available = 0;
  if (buf->size > buf->used) {
    if (buf->size - buf->used > 1) {
      available = buf->size - buf->used - 1;
    }
  }

  size_t to_write = len;
  int truncated = 0;

  if (to_write > available) {
    to_write = available;
    truncated = 1;
    buf->truncated = true;
  }

  if (to_write > 0 && available > 0) {
    if (buf->used <= buf->size - 1 && to_write <= buf->size - 1 - buf->used) {
      memcpy(buf->data + buf->used, bytes, to_write);
      buf->used += to_write;
      if (buf->used < buf->size) {
        buf->data[buf->used] = '\0';
      }
    } else {
      truncated = 1;
      buf->truncated = true;
    }
  }

  return truncated ? 1 : 0;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_sink_buffer(GTEXT_YAML_Sink * sink) {
  if (!sink) {
    return GTEXT_YAML_E_INVALID;
  }

  GTEXT_YAML_Buffer_Sink * buf =
      (GTEXT_YAML_Buffer_Sink *)malloc(sizeof(GTEXT_YAML_Buffer_Sink));
  if (!buf) {
    return GTEXT_YAML_E_OOM;
  }

  buf->data = NULL;
  buf->size = 0;
  buf->used = 0;

  sink->write = buffer_write_fn;
  sink->user = buf;

  return GTEXT_YAML_OK;
}

GTEXT_API const char * gtext_yaml_sink_buffer_data(
    const GTEXT_YAML_Sink * sink) {
  if (!sink || sink->write != buffer_write_fn) {
    return NULL;
  }

  GTEXT_YAML_Buffer_Sink * buf = (GTEXT_YAML_Buffer_Sink *)sink->user;
  if (!buf) {
    return NULL;
  }

  return buf->data ? buf->data : "";
}

GTEXT_API size_t gtext_yaml_sink_buffer_size(const GTEXT_YAML_Sink * sink) {
  if (!sink || sink->write != buffer_write_fn) {
    return 0;
  }

  GTEXT_YAML_Buffer_Sink * buf = (GTEXT_YAML_Buffer_Sink *)sink->user;
  if (!buf) {
    return 0;
  }

  return buf->used;
}

GTEXT_API void gtext_yaml_sink_buffer_free(GTEXT_YAML_Sink * sink) {
  if (!sink || sink->write != buffer_write_fn) {
    return;
  }

  GTEXT_YAML_Buffer_Sink * buf = (GTEXT_YAML_Buffer_Sink *)sink->user;
  if (buf) {
    free(buf->data);
    free(buf);
    sink->user = NULL;
    sink->write = NULL;
  }
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_sink_fixed_buffer(
    GTEXT_YAML_Sink * sink, char * buffer, size_t size) {
  if (!sink || !buffer || size == 0) {
    return GTEXT_YAML_E_INVALID;
  }

  GTEXT_YAML_Fixed_Buffer_Sink * buf = (GTEXT_YAML_Fixed_Buffer_Sink *)malloc(
      sizeof(GTEXT_YAML_Fixed_Buffer_Sink));
  if (!buf) {
    return GTEXT_YAML_E_OOM;
  }

  buf->data = buffer;
  buf->size = size;
  buf->used = 0;
  buf->truncated = false;

  buf->data[0] = '\0';

  sink->write = fixed_buffer_write_fn;
  sink->user = buf;

  return GTEXT_YAML_OK;
}

GTEXT_API size_t gtext_yaml_sink_fixed_buffer_used(
    const GTEXT_YAML_Sink * sink) {
  if (!sink || sink->write != fixed_buffer_write_fn) {
    return 0;
  }

  GTEXT_YAML_Fixed_Buffer_Sink * buf = (GTEXT_YAML_Fixed_Buffer_Sink *)sink->user;
  if (!buf) {
    return 0;
  }

  return buf->used;
}

GTEXT_API bool gtext_yaml_sink_fixed_buffer_truncated(
    const GTEXT_YAML_Sink * sink) {
  if (!sink || sink->write != fixed_buffer_write_fn) {
    return false;
  }

  GTEXT_YAML_Fixed_Buffer_Sink * buf = (GTEXT_YAML_Fixed_Buffer_Sink *)sink->user;
  if (!buf) {
    return false;
  }

  return buf->truncated;
}

GTEXT_API void gtext_yaml_sink_fixed_buffer_free(GTEXT_YAML_Sink * sink) {
  if (!sink || sink->write != fixed_buffer_write_fn) {
    return;
  }

  GTEXT_YAML_Fixed_Buffer_Sink * buf = (GTEXT_YAML_Fixed_Buffer_Sink *)sink->user;
  if (buf) {
    free(buf);
    sink->user = NULL;
    sink->write = NULL;
  }
}

typedef struct {
  GTEXT_YAML_Encoding encoding;
  bool emit_bom;
  bool bom_written;
  unsigned char pending_utf8[4];
  size_t pending_utf8_len;
} yaml_encoding_state;

typedef struct {
  GTEXT_YAML_Sink * sink;
  const GTEXT_YAML_Write_Options * opts;
  /* Borrowed: the streaming writer keeps its own encoder across calls and
     hands this state a view of it, so the two writers cannot drift apart on
     the half of a UTF-8 sequence that is still waiting for its other half. */
  yaml_encoding_state * encoding;
  /* 8.1.1.1 measures a block scalar's indentation indicator from the
     indentation of the node that holds it - which is -1 for the root of a
     document, and the container's own indent everywhere else.  The writer
     cannot work it out from the indent it is passed, so each caller says. */
  int block_parent_indent;
  /* 7.2 lets an empty node stand wherever a node is expected, with one
     exception: a flow sequence entry needs properties or content, so "[,]"
     is not a sequence of one empty node.  Everywhere else "a:" is how an
     empty node is spelled, and writing "a: ~" instead put a scalar into the
     document that the document never held. */
  bool empty_scalar_ok;
  /* 6.9.2 and 5.6 stop an anchor name and a tag at a flow indicator and
     nowhere else, so ':' belongs to whichever of them came last: "*b: 1"
     names the anchor "b:", "&a: 1" the anchor "a:", "!!null: 1" the tag
     "!!null:".  A key that ends in a property has to be separated from its
     colon; a key that ends in content does not. */
  bool key_absorbs_colon;
  /* A block scalar writes the line break that terminates its last line,
     because with '+' chomping that break is part of the value.  Whoever
     would have written the separator break next skips it. */
  bool line_terminated;
  /* The named tag handles a %TAG has declared for the document being
     written, as the handles themselves ("!e!").  Only the streaming writer
     can have any: the DOM writer emits no directives, so a named handle is
     undeclared there by construction and a shorthand using one cannot be
     written at all.  See write_tag(). */
  const char *const *tag_handles;
  size_t tag_handle_count;
  /* Whether the position being written to is inside a flow collection, and
     what to indent a continuation line to if it is.

     A comment runs to the end of the line (7.1), and a flow collection does
     not: "[x # note, y]" puts the ", y]" inside the comment and the bracket
     never closes.  So an inline comment written in flow context has to be
     followed by a line break, which 7.4 permits inside a flow collection and
     which is how the input that produced such a node was spelled in the first
     place.

     This is a property of the *position*, not of the node: a nested flow
     collection's own trailing comment sits inside its parent's brackets, so
     the flag is raised around a flow container's children and lowered again
     before the container's own comment is written.  Then each comment is
     judged by the context it lands in. */
  bool in_flow;
  size_t flow_indent;
  /* How deep write_node() currently is, and how deep it may go.
   *
   * max_depth is a parse option, and until now only the parser read it - so
   * it bounded a document that arrived as text and said nothing about one
   * built through the DOM API, which is the half of the library that can
   * nest without limit. write_node() recurses on the C stack at about 228
   * bytes a level, so a hand-built document a few tens of thousands deep
   * took the process down with it, on default options, with nothing having
   * asked for anything unusual.
   *
   * Taken from the document being written, because that is where a document
   * keeps the options it was made with. Zero is no limit, which after
   * gtext_yaml_parse_options_effective() means a caller who asked for none. */
  size_t depth;
  size_t max_depth;
  /* Nodes whose anchor this document has already written.
   *
   * A node can be reached twice: a merge key splices the source mapping's
   * pairs in by pointer, so one node becomes the value of two keys.  The
   * writer has no alias to emit for the second occurrence - a shared node is
   * not an alias node, and one created by a merge carries no anchor of its
   * own to point at - so it writes the subtree out again.  That is a value
   * change rather than a cosmetic one, because an anchor written twice moves
   * what a *later* alias binds to: 3.2.2.2 says an alias refers to the most
   * recent preceding node with that name, so
   *
   *     m: &s {v: &n 1}
   *     k: &n 2
   *     c: {<<: *s}
   *     r: *n          <- 2, bound where it is written
   *
   * came back with r = 1, because the copy of "&n 1" the merge put in "c"
   * was written between "&n 2" and the alias.  The second definition is
   * dropped instead, which leaves every alias binding to the definition it
   * had before.  Identity is still not preserved - two equal nodes come back
   * where one was shared - and that is a separate question from the value.
   *
   * Per document, because 3.2.2.2's anchor namespace is per document. */
  GTEXT_YAML_Node_Set anchors_written;
} yaml_writer_state;

static GTEXT_YAML_Encoding writer_encoding(const GTEXT_YAML_Write_Options *opts) {
  return opts ? opts->encoding : GTEXT_YAML_ENCODING_UTF8;
}

static bool writer_emit_bom(const GTEXT_YAML_Write_Options *opts) {
  return opts ? opts->emit_bom : false;
}

static void writer_encoding_init(
    yaml_encoding_state *state,
    const GTEXT_YAML_Write_Options *opts) {
  if (!state) {
    return;
  }
  state->encoding = writer_encoding(opts);
  state->emit_bom = writer_emit_bom(opts);
  state->bom_written = false;
  state->pending_utf8_len = 0;
}

static int writer_should_emit_bom(const yaml_encoding_state *state) {
  if (!state) {
    return 0;
  }
  if (state->emit_bom) {
    return 1;
  }
  return state->encoding != GTEXT_YAML_ENCODING_UTF8;
}

static int writer_emit_bom_bytes(
    GTEXT_YAML_Sink *sink,
    yaml_encoding_state *state) {
  if (!sink || !sink->write || !state) {
    return 1;
  }
  if (state->bom_written) {
    return 0;
  }
  if (!writer_should_emit_bom(state)) {
    state->bom_written = true;
    return 0;
  }

  const unsigned char *bom = NULL;
  size_t bom_len = 0;
  switch (state->encoding) {
    case GTEXT_YAML_ENCODING_UTF8: {
      static const unsigned char utf8_bom[] = {0xEF, 0xBB, 0xBF};
      bom = utf8_bom;
      bom_len = sizeof(utf8_bom);
      break;
    }
    case GTEXT_YAML_ENCODING_UTF16LE: {
      static const unsigned char utf16le_bom[] = {0xFF, 0xFE};
      bom = utf16le_bom;
      bom_len = sizeof(utf16le_bom);
      break;
    }
    case GTEXT_YAML_ENCODING_UTF16BE: {
      static const unsigned char utf16be_bom[] = {0xFE, 0xFF};
      bom = utf16be_bom;
      bom_len = sizeof(utf16be_bom);
      break;
    }
    case GTEXT_YAML_ENCODING_UTF32LE: {
      static const unsigned char utf32le_bom[] = {0xFF, 0xFE, 0x00, 0x00};
      bom = utf32le_bom;
      bom_len = sizeof(utf32le_bom);
      break;
    }
    case GTEXT_YAML_ENCODING_UTF32BE: {
      static const unsigned char utf32be_bom[] = {0x00, 0x00, 0xFE, 0xFF};
      bom = utf32be_bom;
      bom_len = sizeof(utf32be_bom);
      break;
    }
    default:
      return 1;
  }

  if (sink->write(sink->user, (const char *)bom, bom_len) != 0) {
    return 1;
  }
  state->bom_written = true;
  return 0;
}

static int writer_encode_codepoint(
    GTEXT_YAML_Sink *sink,
    yaml_encoding_state *state,
    uint32_t codepoint) {
  unsigned char out[4];
  size_t out_len = 0;

  if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    return 1;
  }

  switch (state->encoding) {
    case GTEXT_YAML_ENCODING_UTF16LE:
    case GTEXT_YAML_ENCODING_UTF16BE: {
      if (codepoint <= 0xFFFF) {
        uint16_t unit = (uint16_t)codepoint;
        if (state->encoding == GTEXT_YAML_ENCODING_UTF16BE) {
          out[0] = (unsigned char)((unit >> 8) & 0xFF);
          out[1] = (unsigned char)(unit & 0xFF);
        } else {
          out[0] = (unsigned char)(unit & 0xFF);
          out[1] = (unsigned char)((unit >> 8) & 0xFF);
        }
        out_len = 2;
        if (sink->write(sink->user, (const char *)out, out_len) != 0) {
          return 1;
        }
        return 0;
      }

      uint32_t value = codepoint - 0x10000;
      uint16_t high = (uint16_t)(0xD800 | ((value >> 10) & 0x3FF));
      uint16_t low = (uint16_t)(0xDC00 | (value & 0x3FF));
      if (state->encoding == GTEXT_YAML_ENCODING_UTF16BE) {
        out[0] = (unsigned char)((high >> 8) & 0xFF);
        out[1] = (unsigned char)(high & 0xFF);
        out[2] = (unsigned char)((low >> 8) & 0xFF);
        out[3] = (unsigned char)(low & 0xFF);
      } else {
        out[0] = (unsigned char)(high & 0xFF);
        out[1] = (unsigned char)((high >> 8) & 0xFF);
        out[2] = (unsigned char)(low & 0xFF);
        out[3] = (unsigned char)((low >> 8) & 0xFF);
      }
      out_len = 4;
      return sink->write(sink->user, (const char *)out, out_len) == 0 ? 0 : 1;
    }
    case GTEXT_YAML_ENCODING_UTF32LE:
    case GTEXT_YAML_ENCODING_UTF32BE: {
      uint32_t unit = codepoint;
      if (state->encoding == GTEXT_YAML_ENCODING_UTF32BE) {
        out[0] = (unsigned char)((unit >> 24) & 0xFF);
        out[1] = (unsigned char)((unit >> 16) & 0xFF);
        out[2] = (unsigned char)((unit >> 8) & 0xFF);
        out[3] = (unsigned char)(unit & 0xFF);
      } else {
        out[0] = (unsigned char)(unit & 0xFF);
        out[1] = (unsigned char)((unit >> 8) & 0xFF);
        out[2] = (unsigned char)((unit >> 16) & 0xFF);
        out[3] = (unsigned char)((unit >> 24) & 0xFF);
      }
      out_len = 4;
      return sink->write(sink->user, (const char *)out, out_len) == 0 ? 0 : 1;
    }
    default:
      return 1;
  }
}

static int utf8_decode_one(
    const unsigned char *buf,
    size_t len,
    uint32_t *out_codepoint,
    size_t *out_len) {
  if (len == 0) {
    return 0;
  }

  unsigned char c = buf[0];
  if (c < 0x80) {
    *out_codepoint = c;
    *out_len = 1;
    return 1;
  }

  if ((c & 0xE0) == 0xC0) {
    if (len < 2) return 0;
    if ((buf[1] & 0xC0) != 0x80) return -1;
    uint32_t code = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(buf[1] & 0x3F);
    if (code < 0x80) return -1;
    *out_codepoint = code;
    *out_len = 2;
    return 1;
  }

  if ((c & 0xF0) == 0xE0) {
    if (len < 3) return 0;
    if ((buf[1] & 0xC0) != 0x80 || (buf[2] & 0xC0) != 0x80) return -1;
    uint32_t code = ((uint32_t)(c & 0x0F) << 12) |
        ((uint32_t)(buf[1] & 0x3F) << 6) |
        (uint32_t)(buf[2] & 0x3F);
    if (code < 0x800) return -1;
    if (code >= 0xD800 && code <= 0xDFFF) return -1;
    *out_codepoint = code;
    *out_len = 3;
    return 1;
  }

  if ((c & 0xF8) == 0xF0) {
    if (len < 4) return 0;
    if ((buf[1] & 0xC0) != 0x80 || (buf[2] & 0xC0) != 0x80 ||
        (buf[3] & 0xC0) != 0x80) return -1;
    uint32_t code = ((uint32_t)(c & 0x07) << 18) |
        ((uint32_t)(buf[1] & 0x3F) << 12) |
        ((uint32_t)(buf[2] & 0x3F) << 6) |
        (uint32_t)(buf[3] & 0x3F);
    if (code < 0x10000 || code > 0x10FFFF) return -1;
    *out_codepoint = code;
    *out_len = 4;
    return 1;
  }

  return -1;
}

static GTEXT_YAML_Status write_encoded_bytes(
    GTEXT_YAML_Sink *sink,
    yaml_encoding_state *state,
    const char *bytes,
    size_t len) {
  if (!sink || !sink->write || !bytes || !state) {
    return GTEXT_YAML_E_INVALID;
  }

  if (writer_emit_bom_bytes(sink, state) != 0) {
    return GTEXT_YAML_E_WRITE;
  }

  if (state->encoding == GTEXT_YAML_ENCODING_UTF8) {
    return sink->write(sink->user, bytes, len) == 0
        ? GTEXT_YAML_OK
        : GTEXT_YAML_E_WRITE;
  }

  size_t i = 0;
  if (state->pending_utf8_len > 0) {
    while (state->pending_utf8_len < sizeof(state->pending_utf8) && i < len) {
      state->pending_utf8[state->pending_utf8_len++] =
          (unsigned char)bytes[i++];
    }
    uint32_t codepoint = 0;
    size_t used = 0;
    int rc = utf8_decode_one(state->pending_utf8, state->pending_utf8_len,
        &codepoint, &used);
    if (rc < 0) {
      return GTEXT_YAML_E_INVALID;
    }
    if (rc == 1) {
      if (writer_encode_codepoint(sink, state, codepoint) != 0) {
        return GTEXT_YAML_E_WRITE;
      }
      size_t remaining = state->pending_utf8_len - used;
      if (remaining > 0) {
        memmove(state->pending_utf8, state->pending_utf8 + used, remaining);
      }
      state->pending_utf8_len = remaining;
    }
  }

  while (i < len) {
    uint32_t codepoint = 0;
    size_t used = 0;
    int rc = utf8_decode_one((const unsigned char *)bytes + i, len - i,
        &codepoint, &used);
    if (rc < 0) {
      return GTEXT_YAML_E_INVALID;
    }
    if (rc == 0) {
      size_t remaining = len - i;
      if (remaining > sizeof(state->pending_utf8)) {
        return GTEXT_YAML_E_INVALID;
      }
      memcpy(state->pending_utf8, bytes + i, remaining);
      state->pending_utf8_len = remaining;
      break;
    }
    if (writer_encode_codepoint(sink, state, codepoint) != 0) {
      return GTEXT_YAML_E_WRITE;
    }
    i += used;
  }

  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status write_bytes(
    yaml_writer_state * state, const char * bytes, size_t len) {
  if (!state || !state->sink || !state->sink->write) {
    return GTEXT_YAML_E_INVALID;
  }
  return write_encoded_bytes(state->sink, state->encoding, bytes, len);
}

static GTEXT_YAML_Status write_str(
    yaml_writer_state * state, const char * str) {
  if (!str) {
    return GTEXT_YAML_E_INVALID;
  }
  return write_bytes(state, str, strlen(str));
}

/* The style the root node starts in.  Block style has to win here, because
   nothing below the root can turn flow back into block. */
static bool writer_root_is_flow(const GTEXT_YAML_Write_Options * opts) {
  if (!opts) return true;
  if (opts->flow_style == GTEXT_YAML_FLOW_STYLE_FLOW) return true;
  if (opts->flow_style == GTEXT_YAML_FLOW_STYLE_BLOCK) return false;
  return !opts->pretty;
}

/* The dialect the output is meant to be read back in.  Which texts resolve is
   what a schema and a version *are*, so every "may this go out plain" answer
   below depends on them; they default to the 1.2 core schema, which is what
   this writer emitted before it could be told. */
static GTEXT_YAML_Schema writer_schema(const GTEXT_YAML_Write_Options * opts) {
  return opts ? opts->schema : GTEXT_YAML_SCHEMA_CORE;
}

static bool writer_yaml_1_1(const GTEXT_YAML_Write_Options * opts) {
  return opts ? opts->yaml_1_1 : false;
}

/* Whether @p value, written plain, comes back as the null it is meant to be.
   A node holding the text "~" is a null where the core schema reads it and
   the string "~" where the JSON schema does. */
static bool writer_text_reads_as_null(
    const GTEXT_YAML_Write_Options * opts, const char * value, size_t len) {
  /* NULL: the writer is a separate entry point with no allocator, as in JSON
     and CSV, and this probe's scratch never reaches a caller. */
  return gtext_yaml_plain_text_classify_as(
      value, len, writer_schema(opts), writer_yaml_1_1(opts),
      NULL, NULL, NULL, NULL) == GTEXT_YAML_NULL;
}

static const char *writer_newline(const GTEXT_YAML_Write_Options * opts) {
  if (!opts || !opts->newline) {
    return "\n";
  }
  return opts->newline;
}

static int writer_indent_spaces(const GTEXT_YAML_Write_Options * opts) {
  if (!opts || opts->indent_spaces <= 0) {
    return 2;
  }
  return opts->indent_spaces;
}

static int writer_line_width(const GTEXT_YAML_Write_Options * opts) {
  if (!opts || opts->line_width <= 0) {
    return 0;
  }
  return opts->line_width;
}

/* The break that separates one node from the next - unless a block scalar
   has already written it, in which case writing another would leave a blank
   line that '+' chomping would keep as part of the value. */
static GTEXT_YAML_Status write_separator(yaml_writer_state * state) {
  if (state->line_terminated) {
    state->line_terminated = false;
    return GTEXT_YAML_OK;
  }
  return write_str(state, writer_newline(state->opts));
}

static GTEXT_YAML_Status write_indent(
    yaml_writer_state * state, size_t spaces) {
  char pad[64];
  size_t chunk = sizeof(pad);
  memset(pad, ' ', sizeof(pad));
  while (spaces > 0) {
    size_t to_write = spaces > chunk ? chunk : spaces;
    GTEXT_YAML_Status status = write_bytes(state, pad, to_write);
    if (status != GTEXT_YAML_OK) {
      return status;
    }
    spaces -= to_write;
  }
  return GTEXT_YAML_OK;
}

/* A node's tag, or NULL when it carries none.
 *
 * An empty string is none.  It used to be treated as a tag by everything that
 * asked - the node had "properties", so a scalar with nothing else to write
 * wrote nothing at all and an entry holding one disappeared from its sequence
 * - while write_tag() wrote no characters for it.  The DOM API will store
 * whatever it is given, so the check belongs where the writer reads it. */
static const char *node_tag(const GTEXT_YAML_Node *node) {
  const char *tag = NULL;
  if (!node) return NULL;
  switch (node->type) {
    case GTEXT_YAML_STRING:
    case GTEXT_YAML_BOOL:
    case GTEXT_YAML_INT:
    case GTEXT_YAML_FLOAT:
    case GTEXT_YAML_NULL:
      tag = node->as.scalar.tag;
      break;
    case GTEXT_YAML_SEQUENCE:
    case GTEXT_YAML_OMAP:
    case GTEXT_YAML_PAIRS:
      tag = node->as.sequence.tag;
      break;
    case GTEXT_YAML_MAPPING:
    case GTEXT_YAML_SET:
      tag = node->as.mapping.tag;
      break;
    default:
      return NULL;
  }
  return (tag && *tag) ? tag : NULL;
}

static const char *node_anchor(const GTEXT_YAML_Node *node) {
  if (!node) return NULL;
  switch (node->type) {
    case GTEXT_YAML_STRING:
    case GTEXT_YAML_BOOL:
    case GTEXT_YAML_INT:
    case GTEXT_YAML_FLOAT:
    case GTEXT_YAML_NULL:
      return node->as.scalar.anchor;
    case GTEXT_YAML_SEQUENCE:
    case GTEXT_YAML_OMAP:
    case GTEXT_YAML_PAIRS:
      return node->as.sequence.anchor;
    case GTEXT_YAML_MAPPING:
    case GTEXT_YAML_SET:
      return node->as.mapping.anchor;
    default:
      return NULL;
  }
}

static bool node_is_sequence_type(const GTEXT_YAML_Node *node) {
  if (!node) return false;
  return node->type == GTEXT_YAML_SEQUENCE ||
      node->type == GTEXT_YAML_OMAP ||
      node->type == GTEXT_YAML_PAIRS;
}

static bool node_is_mapping_type(const GTEXT_YAML_Node *node) {
  if (!node) return false;
  return node->type == GTEXT_YAML_MAPPING ||
      node->type == GTEXT_YAML_SET;
}

/* "key:" followed by a block collection puts the collection on the next
   line - but an empty one is written "[]" or "{}" with no indent of its own,
   which lands in column zero and ends the mapping.  An empty collection is a
   value like any other and belongs on the line it was introduced on. */
static bool node_scalar_block_style(const GTEXT_YAML_Node *node) {
  if (!node) return false;
  switch (node->type) {
    case GTEXT_YAML_STRING:
    case GTEXT_YAML_BOOL:
    case GTEXT_YAML_INT:
    case GTEXT_YAML_FLOAT:
    case GTEXT_YAML_NULL:
      return node->as.scalar.scalar_style == GTEXT_YAML_SCALAR_STYLE_LITERAL ||
        node->as.scalar.scalar_style == GTEXT_YAML_SCALAR_STYLE_FOLDED;
    default:
      return false;
  }
}

static const char *default_tag_for_type(GTEXT_YAML_Node_Type type) {
  switch (type) {
    case GTEXT_YAML_STRING:
      return "!!str";
    case GTEXT_YAML_NULL:
      return "!!null";
    case GTEXT_YAML_BOOL:
      return "!!bool";
    case GTEXT_YAML_INT:
      return "!!int";
    case GTEXT_YAML_FLOAT:
      return "!!float";
    case GTEXT_YAML_SEQUENCE:
      return "!!seq";
    case GTEXT_YAML_MAPPING:
      return "!!map";
    case GTEXT_YAML_SET:
      return "!!set";
    case GTEXT_YAML_OMAP:
      return "!!omap";
    case GTEXT_YAML_PAIRS:
      return "!!pairs";
    default:
      return NULL;
  }
}

static bool node_requires_tag(GTEXT_YAML_Node_Type type) {
  return type == GTEXT_YAML_SET ||
      type == GTEXT_YAML_OMAP ||
      type == GTEXT_YAML_PAIRS;
}

static bool tag_is_binary(const char *tag) {
  static const char yaml_prefix[] = "tag:yaml.org,2002:";
  if (!tag) return false;
  if (strcmp(tag, "!!binary") == 0) return true;
  if (strncmp(tag, yaml_prefix, sizeof(yaml_prefix) - 1) == 0) {
    return strcmp(tag + (sizeof(yaml_prefix) - 1), "binary") == 0;
  }
  return false;
}

/* A tag reaches the writer in one of two spellings: the shorthand the parser
   never had to expand ("!local", "!!str"), or the URI a %TAG directive
   resolved it to.  The second kind used to go out as bare text, so
   "tag:example.com,2000:app/foo" left the writer looking like a plain scalar -
   and came back as a mapping key, or as two nodes either side of the comma.
   5.6 gives three spellings and every tag has to go out as one of them. */
static bool tag_char_is_safe(unsigned char c, bool shorthand) {
  if (isalnum(c) || c == '-' || c == '_') return true;
  switch (c) {
    case '#': case ';': case '/': case '?': case ':': case '@':
    case '&': case '=': case '+': case '$': case '.': case '~':
    case '*': case '\'': case '(': case ')':
      return true;
    /* ns-tag-char is ns-uri-char less '!' and the flow indicators, because a
       shorthand ends where a flow collection could begin. */
    case ',': case '[': case ']': case '!':
      return !shorthand;
    default:
      return false;
  }
}

static bool tag_suffix_is_safe(const char *suffix) {
  if (!suffix || !*suffix) return false;
  for (const unsigned char *p = (const unsigned char *)suffix; *p; p++) {
    if (!tag_char_is_safe(*p, true)) return false;
  }
  return true;
}

/* "!", "!local", "!!str", "!handle!suffix".  Anything else - a space, a
   comma, a brace - has to go out verbatim instead. */
/* Whether @p tag is a shorthand this writer can spell, and if so how long
   its handle is.

   c-ns-shorthand-tag is a tag handle followed by ns-tag-char+ (6.8.2), and
   there are three handles: the primary "!", the secondary "!!", and a named
   "!" ns-word-char+ "!".  The first two are defined for every document; a
   *named* one means whatever a %TAG declared it to mean, and means nothing
   at all where none did. */
static bool tag_is_writable_shorthand(const char *tag, size_t *handle_len) {
  if (!tag || tag[0] != '!') return false;
  const unsigned char *p = (const unsigned char *)tag + 1;
  size_t hlen = 1;
  if (*p == '\0') {
    if (handle_len) *handle_len = 1;
    return true;
  }
  if (*p == '!') {
    p++;
    hlen = 2;
  } else {
    const unsigned char *scan = p;
    while (isalnum(*scan) || *scan == '-') scan++;
    if (*scan == '!') {
      p = scan + 1;
      hlen = (size_t)(p - (const unsigned char *)tag);
    }
  }
  if (handle_len) *handle_len = hlen;
  return tag_suffix_is_safe((const char *)p);
}

/* Whether @p handle is a tag handle: "!", "!!", or "!" ns-word-char+ "!"
   (6.8.2).  Not the same question as tag_is_writable_shorthand(), which
   wants a suffix after the handle and answers no for a bare one - asking it
   this meant no %TAG was ever recorded and every named shorthand was
   refused, the streaming writer's own round trip included. */
static bool tag_handle_is_writable(const char *handle) {
  if (!handle || handle[0] != '!') return false;
  if (handle[1] == '\0') return true;                       /* "!" */
  if (handle[1] == '!' && handle[2] == '\0') return true;   /* "!!" */
  const unsigned char *p = (const unsigned char *)handle + 1;
  if (!isalnum(*p) && *p != '-') return false;
  while (isalnum(*p) || *p == '-') p++;
  return p[0] == '!' && p[1] == '\0';
}

/* Whether a %TAG in this document declared @p handle, which is the whole
   handle including both "!" characters. */
static bool writer_handle_is_declared(
    const yaml_writer_state *state, const char *handle, size_t len) {
  if (!state) return false;
  for (size_t i = 0; i < state->tag_handle_count; i++) {
    const char *declared = state->tag_handles[i];
    if (declared && strlen(declared) == len
        && memcmp(declared, handle, len) == 0) {
      return true;
    }
  }
  return false;
}

static int hex_digit_value(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Whether a tag can be written between "!<" and ">" and read back as itself.

   c-verbatim-tag is "!" "<" ns-uri-char+ ">", and a verbatim tag is used
   exactly as written (5.3) - the reader does not decode its escapes, because
   there is no handle left to expand and nothing that says an escape here was
   ever an escape rather than three characters of the URI.  So the writer must
   not encode either: the bytes between the brackets are the tag.

   This used to percent-encode "%" on the way out, on the premise that the
   reader had decoded it on the way in.  The reader decodes only where a %TAG
   prefix was substituted, so the encoding had no matching decode and a tag
   holding a "%" gained a layer of escaping on every round trip: "!a%21b"
   became "!a%2521b", then "!a%252521b". */
static bool tag_is_writable_verbatim(const char *tag) {
  if (!tag || !*tag) return false;
  for (const unsigned char *p = (const unsigned char *)tag; *p; p++) {
    if (tag_char_is_safe(*p, false)) continue;
    if (*p == '%' && hex_digit_value(p[1]) >= 0 && hex_digit_value(p[2]) >= 0) {
      p += 2;
      continue;
    }
    return false;
  }
  return true;
}

static GTEXT_YAML_Status write_tag_verbatim(
    yaml_writer_state * state, const char * tag) {
  GTEXT_YAML_Status status = write_str(state, "!<");
  if (status != GTEXT_YAML_OK) return status;
  status = write_str(state, tag);
  if (status != GTEXT_YAML_OK) return status;
  return write_str(state, ">");
}

/* A tag in the "tag:yaml.org,2002:" namespace has to name a type the spec
   defines; that namespace is not the author's to extend, and the resolver
   refuses "!!bogus" on the way in whatever the options say.  The writer used
   to emit any suffix at all, so a node carrying "!!-.#" - which the DOM API
   will hold, since it validates nothing - came out as a document this very
   parser refuses.  The list is gtext_yaml_tag_is_defined_standard()'s, not a
   second copy of it. */
static bool standard_tag_suffix_is_defined(const char *suffix) {
  return suffix && *suffix && gtext_yaml_tag_is_defined_standard(suffix);
}

static GTEXT_YAML_Status write_tag(
    yaml_writer_state * state, const char * tag) {
  static const char yaml_prefix[] = "tag:yaml.org,2002:";
  if (!tag || !*tag) return GTEXT_YAML_OK;

  if (tag[0] == '!') {
    if (tag[1] == '!' && !standard_tag_suffix_is_defined(tag + 2)) {
      return GTEXT_YAML_E_INVALID;
    }
    size_t handle_len = 0;
    if (tag_is_writable_shorthand(tag, &handle_len)) {
      /* A named handle means whatever a %TAG declared it to mean, so a
         shorthand using one is only writable where this writer has declared
         it.  The DOM writer never declares any - it emits no directives - so
         "!a!3" was going out as itself and the parser refused the writer's
         own output for a handle no %TAG defined.  The streaming writer does
         declare them, and the parser's own event stream reports a tag as it
         was written (5.3), so "!e!foo" arriving beside its "%TAG !e! ..."
         still writes as it arrived.

         There is nothing to fall back on: "!<!a!3>" is a different tag, the
         literal URI rather than the handle's prefix followed by "3", and
         guessing a prefix would invent one. */
      if (handle_len > 2
          && !writer_handle_is_declared(state, tag, handle_len)) {
        return GTEXT_YAML_E_INVALID;
      }
      return write_str(state, tag);
    }
  } else if (strncmp(tag, yaml_prefix, sizeof(yaml_prefix) - 1) == 0) {
    if (!standard_tag_suffix_is_defined(tag + sizeof(yaml_prefix) - 1)) {
      return GTEXT_YAML_E_INVALID;
    }
    if (tag_suffix_is_safe(tag + sizeof(yaml_prefix) - 1)) {
      GTEXT_YAML_Status status = write_str(state, "!!");
      if (status != GTEXT_YAML_OK) return status;
      return write_str(state, tag + sizeof(yaml_prefix) - 1);
    }
  }

  /* Like an anchor name, a tag has one spelling and no fallback.  A tag that
     is not ns-uri-char+ cannot be written at all, and writing an
     approximation of it is worse than saying so. */
  if (!tag_is_writable_verbatim(tag)) return GTEXT_YAML_E_INVALID;
  return write_tag_verbatim(state, tag);
}

/* c-printable, 5.1: the characters a YAML stream is allowed to hold at all.
   Everything outside it - NUL and the rest of C0, DEL, the C1 block apart
   from NEL, the surrogates and the two non-characters at the end of the BMP -
   has no spelling in a stream except an escape, and only the double-quoted
   style has escapes.  The writer used to emit them raw, which produced a
   document its own parser refuses. */
static bool codepoint_is_printable(uint32_t cp) {
  if (cp == 0x09 || cp == 0x0A || cp == 0x0D) return true;
  if (cp >= 0x20 && cp <= 0x7E) return true;
  if (cp == 0x85) return true;
  if (cp >= 0xA0 && cp <= 0xD7FF) return true;
  if (cp >= 0xE000 && cp <= 0xFFFD) return true;
  if (cp >= 0x10000 && cp <= 0x10FFFF) return true;
  return false;
}

/* True when every character of the value may stand in a stream unescaped.
   A byte that is not valid UTF-8 counts as unprintable: it has no code point,
   so there is nothing to escape it as either, and forcing the double-quoted
   style at least keeps it inside quotes. */
static bool scalar_is_printable(const char *value, size_t len) {
  if (!value) return true;
  const unsigned char *p = (const unsigned char *)value;
  size_t i = 0;
  while (i < len) {
    uint32_t cp = 0;
    size_t n = 0;
    if (utf8_decode_one(p + i, len - i, &cp, &n) != 1) return false;
    if (!codepoint_is_printable(cp)) return false;
    i += n;
  }
  return true;
}

/* ns-anchor-name, 6.9.2:

     c-ns-anchor-property ::= "&" ns-anchor-name
     ns-anchor-name       ::= ns-anchor-char+
     ns-anchor-char       ::= ns-char - c-flow-indicator

   ns-char is a printable character that is neither white space nor a line
   break nor the byte order mark, so an anchor name holds none of those, none
   of "[]{},", and is never empty.

   The writer used to emit "&" followed by whatever string it had been handed.
   An anchor of "a b" wrote "&a b", which reads back as the anchor "a" with
   the rest of the line shifted into the value; an anchor of "a[b" wrote a
   document that does not parse at all.  There is nothing to fall back to
   here - unlike a scalar style, an anchor has exactly one spelling - so a
   name that cannot be written is refused. */
static bool anchor_name_is_writable(const char *name) {
  if (!name || !*name) return false;
  size_t len = strlen(name);
  size_t i = 0;
  while (i < len) {
    uint32_t cp = 0;
    size_t n = 0;
    if (utf8_decode_one((const unsigned char *)name + i, len - i, &cp, &n) != 1) {
      return false;
    }
    if (!codepoint_is_printable(cp)) return false;  /* c-printable */
    if (cp == 0x0A || cp == 0x0D) return false;     /* b-char */
    if (cp == 0x20 || cp == 0x09) return false;     /* s-white */
    if (cp == 0xFEFF) return false;                 /* c-byte-order-mark */
    if (cp == '[' || cp == ']' || cp == '{' || cp == '}' || cp == ',') {
      return false;                                 /* c-flow-indicator */
    }
    i += n;
  }
  return true;
}

/* Whether a comment can be written as one, and whether it stays one.
 *
 * A comment is "#" followed by nb-char* (7.1), so what is written after the
 * "#" has to be nb-char: c-printable, no line break, no byte order mark.
 * The writer emitted whatever string it was handed, which went wrong in two
 * ways, and the second is worse than the first.
 *
 *   - A character 5.1 forbids came out raw, so the writer produced a
 *     document this library refuses to read - "# a<CAN>z" over "k: v".
 *
 *   - A line break in an *inline* comment ended the comment and everything
 *     after it became content.  A mapping of one entry with the inline
 *     comment "one\nevil: yes" was written as "k: v # one" over "evil: yes"
 *     and read back with *two* entries.  The comment escaped into the
 *     document, which no error said anything about.
 *
 * There is nothing to fall back to, the same way there is nothing for an
 * anchor: a comment has one spelling and no escapes.  So a comment that
 * cannot be written is refused.
 *
 * @p allow_breaks is for a leading comment, which write_comment_lines()
 * renders as one "#" line per "\n" - a real spelling of a multi-line
 * comment.  A carriage return is not in it: nothing splits on one, so it
 * would reach the stream raw and end the line there.  An inline comment has
 * nowhere to put a second line and takes none.
 */
static bool comment_text_is_writable(const char *text, bool allow_breaks) {
  size_t len = 0;
  size_t i = 0;
  if (!text) return true;
  len = strlen(text);
  while (i < len) {
    uint32_t cp = 0;
    size_t n = 0;
    if (utf8_decode_one((const unsigned char *)text + i, len - i, &cp, &n) != 1) {
      return false;
    }
    if (cp == 0x0A) {
      if (!allow_breaks) return false;
    }
    else {
      if (!codepoint_is_printable(cp)) return false;  /* c-printable */
      if (cp == 0x0D) return false;                   /* b-char */
      if (cp == 0xFEFF) return false;                 /* c-byte-order-mark */
    }
    i += n;
  }
  return true;
}

/* Whether a value has to be quoted rather than written plain.
 *
 * This is 7.3.3's rule rather than a whitelist of safe characters. It was a
 * whitelist - alphanumerics, "_", "-", ".", "~", "+" and a ":" under a rule -
 * on the reasoning that quoting text which would have been safe plain is only
 * a matter of style, and the by-event round trip holds the writer to the text
 * of every scalar either way.
 *
 * Style is the point, though, for the one use this library is better at than
 * its alternatives: reading a configuration file, changing one value, and
 * writing it back. Under the whitelist every plain scalar containing a space
 * came back double-quoted, so `name: John Smith` became `name: "John Smith"`
 * and a one-line change produced a diff touching every line that had a space
 * in it. A space between plain characters is separation *inside* the scalar
 * (nb-ns-plain-in-line), not a reason to quote.
 *
 * Quoting is still value-preserving, and where it is not, that is decided
 * elsewhere: gtext_yaml_plain_text_resolves_to_non_string_as() asks whether
 * the target dialect would resolve this text to a number, a bool or a null,
 * and plan_scalar_style() quotes it if so. That check is what makes widening
 * this one safe - it is a separate question from whether the characters can be
 * written plain, and it was already separate. */
/* 7.3.3's ns-plain-safe(c): every ns-char, less the flow indicators where a
   flow collection is what we are inside of.  A flow indicator inside a plain
   scalar there would end the scalar rather than belong to it. */
static bool plain_safe_char(unsigned char c, bool in_flow) {
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return false;
  if (in_flow) {
    if (c == ',' || c == '[' || c == ']' || c == '{' || c == '}') return false;
  }
  return true;
}

static bool scalar_needs_quotes(const char *value, size_t len, bool in_flow) {
  if (!value || len == 0) return true;
  /* ns-plain-first admits "-" only when an ns-plain-safe character follows -
     it is a c-indicator otherwise, and a lone "-" on a line is a block
     sequence entry.  The string "-" was being written plain and read back as
     a sequence holding one empty node. */
  if (value[0] == '-' && (len == 1 || value[1] == ' ' || value[1] == '\t')) {
    return true;
  }
  /* A line of exactly "---" or "..." is c-directives-end or c-document-end
     (9.1.2), and c-forbidden keeps either out of a document's content
     wherever it stands at the start of a line and a break, white space or the
     end of input follows it (9.1.1).  A root scalar is written at the start
     of its line, so the string "---" went out plain and came back as an empty
     document - the writer had said OK and produced a document marker.

     Quoting is value-preserving here: neither text resolves to anything but a
     string.  So the whole family is quoted rather than only the positions
     where it would be fatal, and the test is written against c-forbidden
     rather than against "len == 3", which would hold only for as long as the
     whitelist below stays narrow enough to reject everything else. */
  if (len >= 3
      && (memcmp(value, "---", 3) == 0 || memcmp(value, "...", 3) == 0)
      && (len == 3 || value[3] == ' ' || value[3] == '\t'
          || value[3] == '\n' || value[3] == '\r')) {
    return true;
  }
  /* ns-plain-first(c) is an ns-char that is not a c-indicator.  Only the first
     character asks that question: "a'b" and "a|b" are plain scalars, because
     every one of those indicators is an ordinary ns-char anywhere but the
     front.  The three that keep their meaning inside a scalar - ":", "#", and
     the flow indicators - are handled in the loop.

     "?" and ":" are admitted by ns-plain-first when an ns-plain-safe character
     follows, and are refused here anyway: "? " is an explicit key and a
     leading ":" is how a block mapping writes a value with no key, so neither
     is worth the one character it saves.

     "-" is absent from this list because the check above already has it, and
     has it more precisely: ns-plain-first admits a "-" that is followed by an
     ns-plain-safe character, so "-1" and "-x" are plain scalars and only a
     lone "-" or one before white space is the sequence-entry indicator. */
  {
    const unsigned char first = (unsigned char)value[0];
    if (strchr("?:,[]{}#&*!|>'\"%@`", (int)first) != NULL) {
      return true;
    }
  }

  /* White space at either end is separation rather than content: the scanner
     takes it off, and " 3" written plain reads back as the integer 3.
     plain_style_cannot_carry() says the same thing, but it is consulted only
     for binary scalars - this function is the whole test for every other
     kind. */
  if (value[0] == ' ' || value[0] == '\t' || value[len - 1] == ' '
      || value[len - 1] == '\t') {
    return true;
  }

  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)value[i];

    /* White space *between* plain characters is separation inside the scalar
       (nb-ns-plain-in-line), so "John Smith" is one plain scalar and does not
       need quoting. */
    if (c == ' ' || c == '\t') {
      continue;
    }

    /* Three characters that this parser reads as content and another reader
       may not, so they are quoted wherever they stand.  U+0085 NEL, U+2028
       LINE SEPARATOR and U+2029 PARAGRAPH SEPARATOR are line breaks in YAML
       1.1 and ordinary nb-chars in 1.2: written raw in a plain scalar they
       round-trip here and fold to a space there.  U+FEFF is excluded from
       nb-char outright (5.2), so it has no plain spelling at all.  Inside
       double quotes each of them is unambiguous. */
    if (c == 0xC2 && i + 1 < len && (unsigned char)value[i + 1] == 0x85) {
      return true; // U+0085
    }
    if (c == 0xE2 && i + 2 < len && (unsigned char)value[i + 1] == 0x80
        && ((unsigned char)value[i + 2] == 0xA8
            || (unsigned char)value[i + 2] == 0xA9)) {
      return true; // U+2028, U+2029
    }
    if (c == 0xEF && i + 2 < len && (unsigned char)value[i + 1] == 0xBB
        && (unsigned char)value[i + 2] == 0xBF) {
      return true; // U+FEFF
    }

    /* A flow indicator inside a flow collection would end the scalar. */
    if (!plain_safe_char(c, in_flow)) {
      return true;
    }

    /* ns-plain-char admits "#" only where an ns-char precedes it: after white
       space it starts a comment and the rest of the line stops being content.
       Not in first position either, which the check above already refused. */
    if (c == '#') {
      if (i == 0 || value[i - 1] == ' ' || value[i - 1] == '\t') {
        return true;
      }
      continue;
    }

    /* ns-plain-char admits ":" where an ns-plain-safe character follows it
       (7.3.3), and the whitelist this replaced did not - so "0:0" was quoted.
       For a string that costs nothing; for anything else quoting is not
       value-preserving, and "0:0" parsed with yaml_1_1 is the sexagesimal
       integer 0.  It went out as the string.

       ": " is the other side of the same rule, and the one that matters most
       here: it ends a key, so a scalar containing it cannot be written plain
       at all. */
    if (c == ':') {
      if (i + 1 >= len
          || !plain_safe_char((unsigned char)value[i + 1], in_flow)) {
        return true;
      }
      continue;
    }
  }
  return false;
}

/* What the plain style cannot carry, whatever the whitelist above says: a
   line break folds to a space (6.5), and white space at either end is
   separation the scanner takes off before the content begins.

   Base64 is exempt from the whitelist, because "/" and "=" are ordinary in it
   and quoting every binary scalar would be noise.  It is not exempt from
   this.  A binary scalar built with a break in its text - which is how base64
   is written by hand, in short lines - went out plain across two lines and
   came back with the break folded into a space.  The bytes were the same,
   since base64 ignores white space; the text was not, and the text is what
   gtext_yaml_node_as_string() returns and what this library keeps as written
   (suite case 565N). */
static bool plain_style_cannot_carry(const char *value, size_t len) {
  if (!value || len == 0) return false;
  if (value[0] == ' ' || value[0] == '\t') return true;
  if (value[len - 1] == ' ' || value[len - 1] == '\t') return true;
  for (size_t i = 0; i < len; i++) {
    if (value[i] == '\n' || value[i] == '\r') return true;
  }
  return false;
}

static GTEXT_YAML_Status write_escaped_scalar(
    yaml_writer_state * state, const char * value, size_t len) {
  static const char hex[] = "0123456789ABCDEF";
  GTEXT_YAML_Status status = write_str(state, "\"");
  if (status != GTEXT_YAML_OK) return status;

  if (!value) len = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)value[i];
    switch (c) {
      case '\\':
        status = write_str(state, "\\\\");
        break;
      case '"':
        status = write_str(state, "\\\"");
        break;
      case '\n':
        status = write_str(state, "\\n");
        break;
      case '\r':
        status = write_str(state, "\\r");
        break;
      case '\t':
        status = write_str(state, "\\t");
        break;
      default: {
        /* Decode before deciding: what 5.1 forbids is a character, not a
           byte, so the C1 block has to be recognised through its two-byte
           UTF-8 spelling rather than by looking at 0xC2. */
        uint32_t cp = 0;
        size_t n = 0;
        int ok = utf8_decode_one(
            (const unsigned char *)value + i, len - i, &cp, &n);
        if (ok == 1 && codepoint_is_printable(cp)) {
          status = write_bytes(state, value + i, n);
          i += n - 1;
          break;
        }
        if (ok != 1) {
          /* Not UTF-8 at all, and there is no spelling for it: a YAML stream
             is a stream of characters, and the escapes of 5.7 all name a code
             point.  Writing "\xFF" for the byte 0xFF would read back as
             U+00FF - two bytes, a different value - so the byte is refused
             rather than quietly changed into a character that happens to
             share its number.  Every path that could carry one arrives here,
             because scalar_is_printable() is false for invalid UTF-8 and
             forces the double-quoted style. */
          return GTEXT_YAML_E_INVALID;
        }
        char buf[10];
        size_t blen = 0;
        buf[blen++] = '\\';
        if (cp <= 0xFF) {
          buf[blen++] = 'x';
          buf[blen++] = hex[(cp >> 4) & 0x0F];
          buf[blen++] = hex[cp & 0x0F];
        }
        else if (cp <= 0xFFFF) {
          buf[blen++] = 'u';
          for (int shift = 12; shift >= 0; shift -= 4) {
            buf[blen++] = hex[(cp >> shift) & 0x0F];
          }
        }
        else {
          buf[blen++] = 'U';
          for (int shift = 28; shift >= 0; shift -= 4) {
            buf[blen++] = hex[(cp >> shift) & 0x0F];
          }
        }
        status = write_bytes(state, buf, blen);
        i += n - 1;
        break;
      }
    }
    if (status != GTEXT_YAML_OK) return status;
  }

  return write_str(state, "\"");
}

/* Single quotes have no escapes at all, so the style can only be used for
   content it can hold verbatim: a line break inside one folds away to a
   space on the way back in, and there is no spelling for a control
   character.  Both cases go out double quoted instead. */
static bool scalar_fits_single_quotes(const char *value, size_t len) {
  if (!value) return true;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)value[i];
    if (c == '\t') continue;
    if (c < 0x20) return false;
  }
  /* Everything 5.1 forbids needs an escape, and single quotes have none. */
  return scalar_is_printable(value, len);
}

static GTEXT_YAML_Status write_single_quoted_scalar(
    yaml_writer_state * state, const char * value, size_t len) {
  GTEXT_YAML_Status status = write_str(state, "'");
  if (status != GTEXT_YAML_OK) return status;

  if (!value) len = 0;
  for (size_t i = 0; i < len; i++) {
    if (value[i] == '\'') {
      status = write_str(state, "''");
    } else {
      status = write_bytes(state, value + i, 1);
    }
    if (status != GTEXT_YAML_OK) return status;
  }

  return write_str(state, "'");
}

/* Everything a block scalar has to decide before a byte of it is written.
   The old writer decided none of it: it always wrote '|' or '>' with no
   chomping indicator and no indentation indicator, so a value with no
   trailing break came back with one, a value with several came back with
   one, a first line that began with a space lost the space, and folding
   turned every line break in the content into a space. */
typedef struct {
  bool usable;      /* false: the value has no faithful block spelling */
  bool folded;      /* '>' rather than '|' */
  char chomp;       /* '-' strip, '+' keep, '\0' clip */
  int indent_digit; /* 0 when auto-detection reads the right indentation */
  size_t body_len;  /* the value with its trailing line breaks removed */
  size_t trailing;  /* how many of those there were */
} yaml_block_plan;

static void plan_block_scalar(
    const char * value,
    size_t len,
    size_t content_indent,
    int parent_indent,
    bool folded_wanted,
    yaml_block_plan * plan) {
  memset(plan, 0, sizeof(*plan));
  if (!value || len == 0) return;

  size_t trailing = 0;
  while (trailing < len && value[len - 1 - trailing] == '\n') trailing++;
  size_t body_len = len - trailing;
  /* A value that is nothing but line breaks has no first line to set the
     indentation from, so it has no block spelling at all. */
  if (body_len == 0) return;

  for (size_t i = 0; i < body_len; i++) {
    unsigned char c = (unsigned char)value[i];
    /* A carriage return would be read back as a line break, and a control
       character has no literal spelling.  Both need quotes. */
    if (c == '\r') return;
    if (c < 0x20 && c != '\n' && c != '\t') return;
    if (c == 0x7F) return;
    /* A block scalar's lines are nb-char, and nb-char is "c-printable -
       b-char - c-byte-order-mark" (5.4), so a U+FEFF has no literal or folded
       spelling any more than it has a plain one.  scalar_needs_quotes() has
       said so for the plain style since U+0085 and U+2028 were added beside
       it; the block styles never asked, because they ask this function and
       this function only looked below U+0020.

       fuzz_yaml_writer found it: a scalar carrying one, with a block style
       stored on the node, was written straight out and this library's own
       parser answered "Byte order mark in scalar content".  Reachable through
       gtext_yaml_node_set_scalar_style() and not from any text - the only
       YAML spelling that admits a U+FEFF is a quoted one, and a parsed node
       remembers that style, as does one gtext_json_to_yaml() builds. */
    if (c == 0xEF && i + 2 < body_len
        && (unsigned char)value[i + 1] == 0xBB
        && (unsigned char)value[i + 2] == 0xBF) {
      return;
    }
  }

  plan->body_len = body_len;
  plan->trailing = trailing;
  plan->chomp = trailing == 0 ? '-' : (trailing == 1 ? '\0' : '+');

  /* 8.1.1.1: auto-detection takes the indentation from the first non-empty
     line, so a first line that begins with a space needs the indicator. */
  size_t first = 0;
  while (first < body_len && value[first] == '\n') first++;
  if (first < body_len && value[first] == ' ') {
    long digit = (long)content_indent - (long)parent_indent;
    if (digit < 1 || digit > 9) return;
    plan->indent_digit = (int)digit;
  }

  if (folded_wanted) {
    /* 8.1.3 keeps the break after a more-indented line and folds every other
       one, so a folded scalar can only hold lines that neither begin nor end
       with a space or a tab. */
    bool ok = true;
    size_t line = 0;
    while (ok && line < body_len) {
      size_t stop = line;
      while (stop < body_len && value[stop] != '\n') stop++;
      if (stop > line) {
        char head = value[line];
        char tail = value[stop - 1];
        if (head == ' ' || head == '\t' || tail == ' ' || tail == '\t') {
          ok = false;
        }
      }
      line = stop + 1;
    }
    plan->folded = ok;
  }

  plan->usable = true;
}

static bool is_space_or_tab(char c) {
  return c == ' ' || c == '\t';
}

/* A folded line may be broken at a single space, because the break folds
   back to that space.  Two spaces in a row cannot: the fold would return
   only one of them.

   Nor may the break leave white space against it on either side, and a tab
   counts.  8.1.3 keeps the break before a more-indented line rather than
   folding it, and "more indented" means beginning with a space *or a tab* -
   so breaking "s{ \t  a" at its space writes a continuation line starting
   with a tab, and the reader keeps that break as a line feed.  The space is
   then gone and a newline stands where it was.  The plan above already
   refuses a scalar whose own lines begin or end with white space; this is
   the same rule applied to the lines the writer invents. */
static GTEXT_YAML_Status write_folded_line(
    yaml_writer_state * state,
    const char * line,
    size_t len,
    size_t indent,
    int line_width) {
  size_t width = line_width > 0 ? (size_t)line_width : 0;
  size_t pos = 0;
  bool first = true;

  while (pos < len) {
    size_t cut = len;
    if (width > 0 && len - pos > width) {
      for (size_t i = 1; i < width && pos + i < len; i++) {
        if (line[pos + i] == ' ' && !is_space_or_tab(line[pos + i - 1]) &&
            pos + i + 1 < len && !is_space_or_tab(line[pos + i + 1])) {
          cut = pos + i;
        }
      }
    }
    if (!first) {
      GTEXT_YAML_Status status = write_str(state, writer_newline(state->opts));
      if (status != GTEXT_YAML_OK) return status;
    }
    GTEXT_YAML_Status status = write_indent(state, indent);
    if (status != GTEXT_YAML_OK) return status;
    status = write_bytes(state, line + pos, cut - pos);
    if (status != GTEXT_YAML_OK) return status;
    pos = cut < len ? cut + 1 : len;
    first = false;
  }
  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status write_block_scalar(
    yaml_writer_state * state,
    const char * value,
    size_t indent,
    const yaml_block_plan * plan) {
  size_t child_indent = indent + (size_t)writer_indent_spaces(state->opts);
  int line_width = writer_line_width(state->opts);

  GTEXT_YAML_Status status = write_str(state, plan->folded ? ">" : "|");
  if (status != GTEXT_YAML_OK) return status;
  if (plan->indent_digit) {
    char digit = (char)('0' + plan->indent_digit);
    status = write_bytes(state, &digit, 1);
    if (status != GTEXT_YAML_OK) return status;
  }
  if (plan->chomp) {
    status = write_bytes(state, &plan->chomp, 1);
    if (status != GTEXT_YAML_OK) return status;
  }

  size_t pos = 0;

  /* 8.1.3 folds a lone break away to a space and keeps one line feed for
     every empty line after it, so a run of k line feeds in the content is
     written as k+1 breaks.  A literal scalar keeps every break as it is, so
     there each line is simply one break. */
  if (plan->folded) {
    size_t lead = 0;
    while (pos < plan->body_len && value[pos] == '\n') {
      lead++;
      pos++;
    }
    /* Empty lines at the head of a folded scalar come before any folding and
       are taken one line feed each. */
    for (size_t i = 0; i <= lead; i++) {
      status = write_str(state, writer_newline(state->opts));
      if (status != GTEXT_YAML_OK) return status;
    }
    while (pos < plan->body_len) {
      size_t stop = pos;
      while (stop < plan->body_len && value[stop] != '\n') stop++;
      status = write_folded_line(
          state, value + pos, stop - pos, child_indent, line_width);
      if (status != GTEXT_YAML_OK) return status;
      size_t gap = 0;
      pos = stop;
      while (pos < plan->body_len && value[pos] == '\n') {
        gap++;
        pos++;
      }
      if (pos < plan->body_len) {
        for (size_t i = 0; i <= gap; i++) {
          status = write_str(state, writer_newline(state->opts));
          if (status != GTEXT_YAML_OK) return status;
        }
      }
    }
  } else {
    while (pos <= plan->body_len) {
      size_t stop = pos;
      while (stop < plan->body_len && value[stop] != '\n') stop++;

      status = write_str(state, writer_newline(state->opts));
      if (status != GTEXT_YAML_OK) return status;
      if (stop > pos) {
        status = write_indent(state, child_indent);
        if (status == GTEXT_YAML_OK) {
          status = write_bytes(state, value + pos, stop - pos);
        }
        if (status != GTEXT_YAML_OK) return status;
      }
      if (stop == plan->body_len) break;
      pos = stop + 1;
    }
  }

  /* One break terminates the last line; the rest are the value's own, kept
     by '+' and thrown away by '|' and '-' alike. */
  size_t breaks = plan->trailing > 0 ? plan->trailing : 1;
  for (size_t i = 0; i < breaks; i++) {
    status = write_str(state, writer_newline(state->opts));
    if (status != GTEXT_YAML_OK) return status;
  }
  state->line_terminated = true;
  return GTEXT_YAML_OK;
}

/* Which style a scalar can actually be written in, and the block plan that
   goes with it if the answer is a block one.

   Both writers ask this.  They used to decide it separately - the spelling
   helpers below were made common, but the choice of spelling was not - and a
   rule added to one of them did not reach the other.  Every reason to reject
   a style lives here now, in the order the reasons override each other.

   `style` is what the caller would prefer, after the options and the node's
   own remembered style have been consulted. */
/* 10.3.2's merge row: a plain "<<" in a mapping key resolves to
   tag:yaml.org,2002:merge, not to a string.  is_merge_key() in yaml_resolve.c
   is where that rule is stated and commented; this is the writer's half of it.

   Position is not consulted, so the string "<<" is quoted as a value too,
   where plain would have been fine.  That follows the "---" and "..." family
   in scalar_needs_quotes(): quoting is value-preserving for a string, so the
   whole family is quoted rather than only the positions where leaving it plain
   is fatal, and the alternative is threading "is this a key" through
   plan_scalar_style() for two characters.

   Dialect and schema are not consulted either, because merge keys are not
   theirs to decide: allow_merge_keys defaults to true and is_merge_key() asks
   neither.  Nor does allow_merge_keys = false make plain "<<" safe to write -
   it makes it an error to read. */
static bool plain_text_is_merge_key(const char *value, size_t len) {
  return len == 2 && value[0] == '<' && value[1] == '<';
}

static GTEXT_YAML_Scalar_Style plan_scalar_style(
    GTEXT_YAML_Scalar_Style style,
    const char *value,
    size_t len,
    bool is_binary,
    /* Whether the node says it is a string, as distinct from a scalar whose
       text happens to be one.  Only a string needs protecting from its own
       spelling. */
    bool is_string,
    bool canonical,
    bool in_flow,
    bool pretty,
    int line_width,
    size_t content_indent,
    int parent_indent,
    GTEXT_YAML_Schema schema,
    bool yaml_1_1,
    yaml_block_plan *plan) {
  memset(plan, 0, sizeof(*plan));

  if (canonical) return GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED;

  /* First, because it is the only question with one answer: a character 5.1
     forbids has no spelling in a stream except an escape, and the
     double-quoted style is the only one that has escapes. */
  if (!scalar_is_printable(value, len)) {
    return GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED;
  }

  if (style == GTEXT_YAML_SCALAR_STYLE_PLAIN) {
    if (is_binary ? plain_style_cannot_carry(value, len)
                  : scalar_needs_quotes(value, len, in_flow)) {
      style = GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED;
    }
    /* A string whose text spells a number, a bool or a null has to be
       quoted, or the plain spelling resolves it back to that instead.
       gtext_yaml_node_new_scalar() makes a string of whatever it is given,
       so a caller who built the string "1" and wrote it got the integer 1
       back.

       Which texts spell what is the target dialect's to say, and this used to
       ask the 1.2 core schema whatever the output was for.  1.1 resolves
       strictly more of them - "yes", "off", "012", "0:0" - so the strings
       spelling those went out plain and came back as a bool or an integer;
       the failsafe schema resolves none of them, so quoting for its sake
       protects nothing.

       A plain "<<" is the other text that does not resolve to a string, and
       it is not the schema's doing: as a mapping key it is the merge key.  A
       DOM built with the string "<<" for a key went out plain, and the output
       was then refused by this library's own parser for a merge value that is
       not a mapping - or, where the value *was* a mapping, read back with the
       key gone and its contents spliced into the mapping around it.  A fuzzer
       found the first; the second is the one that was quiet. */
    else if (is_string
             && (gtext_yaml_plain_text_resolves_to_non_string_as(
                     value, len, schema, yaml_1_1)
                 || plain_text_is_merge_key(value, len))) {
      style = GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED;
    }
    /* Long enough to wrap, and wrapping it is allowed to change how it is
       spelled but not what it says.  A folded block scalar is never a plain
       one, so 10.3.2 gives it the string tag whatever it holds - and this arm
       is reached *only* through PLAIN, which is what write_scalar_node()
       forces every non-string scalar to precisely so that no style can change
       its type.  The comment there says plan_scalar_style() upgrades PLAIN
       only "where the text cannot be written plain at all"; a number that
       does not fit the line is text that can be written plain perfectly well
       and merely does not fit, so the upgrade was outside the one exception
       the rule allows.  An int of 222 written with line_width 2 came back as
       the string "222".

       Asked of the *text* rather than of is_string, which is what makes one
       condition enough for both callers: a string spelling a number has
       already been quoted by the arm above and never arrives here, so this
       only ever fires for a scalar whose own spelling would resolve to
       something other than a string - and folding that is the value change.
       A long ordinary word resolves to a string either way and still folds.

       The consequence is that a value whose plain spelling exceeds the width
       is written over it. That is the only option available: YAML has no way
       to wrap a number, and a line_width is a preference where a value is a
       guarantee. */
    else if (!in_flow && pretty && line_width > 0 &&
             len > (size_t)line_width &&
             !gtext_yaml_plain_text_resolves_to_non_string_as(
                 value, len, schema, yaml_1_1)) {
      style = GTEXT_YAML_SCALAR_STYLE_FOLDED;
    }
  }

  /* A flow collection is one line's worth of syntax; a block scalar ends its
     own line, so there is nowhere inside one to put it. */
  if (in_flow && (style == GTEXT_YAML_SCALAR_STYLE_LITERAL ||
      style == GTEXT_YAML_SCALAR_STYLE_FOLDED)) {
    style = GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED;
  }

  /* A block style is only a spelling of the value if the value has one.  Ask
     before committing to it, rather than writing something that reads back
     as a different string. */
  if (style == GTEXT_YAML_SCALAR_STYLE_LITERAL ||
      style == GTEXT_YAML_SCALAR_STYLE_FOLDED) {
    plan_block_scalar(value, len, content_indent, parent_indent,
                      style == GTEXT_YAML_SCALAR_STYLE_FOLDED, plan);
    if (!plan->usable) style = GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED;
  }

  if (style == GTEXT_YAML_SCALAR_STYLE_SINGLE_QUOTED &&
      !scalar_fits_single_quotes(value, len)) {
    style = GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED;
  }

  return style;
}

static const char *node_leading_comment(const GTEXT_YAML_Node *node) {
  if (!node) return NULL;
  switch (node->type) {
    case GTEXT_YAML_STRING:
    case GTEXT_YAML_BOOL:
    case GTEXT_YAML_INT:
    case GTEXT_YAML_FLOAT:
    case GTEXT_YAML_NULL:
      return node->as.scalar.leading_comment;
    case GTEXT_YAML_SEQUENCE:
    case GTEXT_YAML_OMAP:
    case GTEXT_YAML_PAIRS:
      return node->as.sequence.leading_comment;
    case GTEXT_YAML_MAPPING:
    case GTEXT_YAML_SET:
      return node->as.mapping.leading_comment;
    case GTEXT_YAML_ALIAS:
      return node->as.alias.leading_comment;
    default:
      return NULL;
  }
}

static const char *node_inline_comment(const GTEXT_YAML_Node *node) {
  if (!node) return NULL;
  switch (node->type) {
    case GTEXT_YAML_STRING:
    case GTEXT_YAML_BOOL:
    case GTEXT_YAML_INT:
    case GTEXT_YAML_FLOAT:
    case GTEXT_YAML_NULL:
      return node->as.scalar.inline_comment;
    case GTEXT_YAML_SEQUENCE:
    case GTEXT_YAML_OMAP:
    case GTEXT_YAML_PAIRS:
      return node->as.sequence.inline_comment;
    case GTEXT_YAML_MAPPING:
    case GTEXT_YAML_SET:
      return node->as.mapping.inline_comment;
    case GTEXT_YAML_ALIAS:
      return node->as.alias.inline_comment;
    default:
      return NULL;
  }
}

/* The one place that decides whether a collection is written "[a, b]" or as
   indented block lines.  A container has to ask before it writes "-" and a
   line break, because a child that turns out flow after all would then start
   in column zero: "- !!map" over "{foo: bar}" is two top-level nodes, not
   one tagged mapping. */
static bool collection_is_flow(
    const yaml_writer_state * state,
    const GTEXT_YAML_Node * node,
    const char * tag_override,
    bool flow) {
  const GTEXT_YAML_Write_Options * opts = state->opts;
  bool pretty = opts ? opts->pretty : false;
  GTEXT_YAML_Flow_Style flow_style = opts
      ? opts->flow_style
      : GTEXT_YAML_FLOW_STYLE_AUTO;

  if (flow_style == GTEXT_YAML_FLOW_STYLE_FLOW) return true;
  /* Block style asked for at the root, not once inside a flow collection:
     7.3 has no block spelling there. */
  if (flow_style == GTEXT_YAML_FLOW_STYLE_BLOCK && !flow) pretty = true;
  if (flow) return true;
  if (opts && opts->canonical) return true;
  /* Properties are written in front of the node, and "&a" cannot share a
     line with block content, so an anchored or tagged collection goes out
     flow whatever was asked for. */
  if (node_anchor(node) || tag_override || node_tag(node)) return true;
  if (node_requires_tag(node->type)) return true;
  return !pretty;
}

static bool node_opens_block(
    const yaml_writer_state * state,
    const GTEXT_YAML_Node * node,
    const char * tag_override) {
  if (node_is_sequence_type(node)) {
    if (node->as.sequence.count == 0) return false;
  } else if (node_is_mapping_type(node)) {
    if (node->as.mapping.count == 0) return false;
  } else {
    return false;
  }
  return !collection_is_flow(state, node, tag_override, false);
}

/* True when this node will be written as nothing at all, so the space that
   would have separated it from its '-' or ':' has nothing left to separate. */
static bool node_writes_nothing(
    const yaml_writer_state * state,
    const GTEXT_YAML_Node * node,
    const char * tag_override) {
  if (!state->empty_scalar_ok) return false;
  if (!node || node->type != GTEXT_YAML_NULL) return false;
  if (node->as.scalar.value && node->as.scalar.value[0]) return false;
  if (!state->opts) return false;
  if (state->opts->canonical) return false;
  if (state->opts->scalar_style != GTEXT_YAML_SCALAR_STYLE_PLAIN) return false;
  if (node->as.scalar.scalar_style != GTEXT_YAML_SCALAR_STYLE_PLAIN) return false;
  if (node_anchor(node) || tag_override || node_tag(node)) return false;
  return node_inline_comment(node) == NULL;
}

static GTEXT_YAML_Status write_comment_lines(
    yaml_writer_state *state,
    const char *comment,
    size_t indent) {
  if (!comment || !state) return GTEXT_YAML_OK;
  if (!comment_text_is_writable(comment, true)) return GTEXT_YAML_E_INVALID;

  const char *line = comment;
  const char *cursor = comment;

  for (;;) {
    if (*cursor == '\n' || *cursor == '\0') {
      size_t len = (size_t)(cursor - line);
      GTEXT_YAML_Status status = write_indent(state, indent);
      if (status != GTEXT_YAML_OK) return status;
      status = write_str(state, "#");
      if (status != GTEXT_YAML_OK) return status;
      if (len > 0) {
        status = write_str(state, " ");
        if (status != GTEXT_YAML_OK) return status;
        status = write_bytes(state, line, len);
        if (status != GTEXT_YAML_OK) return status;
      }
      status = write_str(state, writer_newline(state->opts));
      if (status != GTEXT_YAML_OK) return status;
      if (*cursor == '\0') break;
      cursor++;
      line = cursor;
      continue;
    }
    cursor++;
  }

  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status write_inline_comment(
    yaml_writer_state *state,
    const char *comment) {
  if (!comment || !state) return GTEXT_YAML_OK;
  if (!comment_text_is_writable(comment, false)) return GTEXT_YAML_E_INVALID;
  GTEXT_YAML_Status status = write_str(state, " # ");
  if (status != GTEXT_YAML_OK) return status;
  status = write_str(state, comment);
  if (status != GTEXT_YAML_OK) return status;

  /* In flow context the comment has eaten the rest of the line, and the rest
     of the line is the collection: the separating "," or the closing "]".
     7.4 lets a flow collection run over a line break, so this ends the line
     and the collection carries on below it, indented past the block node that
     holds it.  Without this the writer emitted "[x # note, y]" and said OK,
     and reading that back gave "Unterminated flow collection" - on input this
     library had just parsed, because the parser attaches a comment written
     inside brackets to the entry before it. */
  if (state->in_flow) {
    status = write_str(state, writer_newline(state->opts));
    if (status != GTEXT_YAML_OK) return status;
    return write_indent(state, state->flow_indent);
  }
  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status write_node(
    yaml_writer_state * state,
    const GTEXT_YAML_Node * node,
    size_t indent,
    bool flow,
  const char * tag_override,
  bool leading_newline);

static GTEXT_YAML_Status write_properties(
    yaml_writer_state * state,
    const char * anchor,
    const char * tag,
    GTEXT_YAML_Node_Type type,
    bool trailing_space) {
  bool canonical = state->opts && state->opts->canonical;

  /* An empty tag string is no tag.  It used to be one everywhere but in
     write_tag(), which wrote nothing for it - so a node carrying "" was given
     a tag's spacing and none of its text, and an entry holding one vanished
     from the sequence it was in. */
  if (tag && !*tag) tag = NULL;

  if (!tag && node_requires_tag(type)) {
    tag = default_tag_for_type(type);
  }

  if (!tag && canonical) {
    tag = default_tag_for_type(type);
  }

  if (anchor) {
    if (!anchor_name_is_writable(anchor)) return GTEXT_YAML_E_INVALID;
    GTEXT_YAML_Status status = write_str(state, "&");
    if (status != GTEXT_YAML_OK) return status;
    status = write_str(state, anchor);
    if (status != GTEXT_YAML_OK) return status;
  }

  if (tag) {
    GTEXT_YAML_Status status = GTEXT_YAML_OK;
    if (anchor) {
      status = write_str(state, " ");
      if (status != GTEXT_YAML_OK) return status;
    }
    status = write_tag(state, tag);
    if (status != GTEXT_YAML_OK) return status;
  }

  if ((anchor || tag) && trailing_space) {
    return write_str(state, " ");
  }

  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status write_node_prefix(
    yaml_writer_state * state,
    const GTEXT_YAML_Node * node,
    const char * tag_override,
    bool trailing_space) {
  const char *anchor = node_anchor(node);
  if (anchor) {
    /* Second and later occurrences of one node write no anchor; see
       anchors_written.  The set is keyed on the node rather than on the name
       because a name may legitimately be redefined for a *different* node
       (3.2.2.2, and YamlAnchors.AnAnchorMayBeRedefined), and that definition
       has to be written. */
    bool oom = false;
    if (!gtext_yaml_node_set_add(&state->anchors_written, node, &oom)) {
      if (oom) return GTEXT_YAML_E_OOM;
      anchor = NULL;
    }
  }
  return write_properties(
      state,
      anchor,
      tag_override ? tag_override : node_tag(node),
      node->type,
      trailing_space);
}

static GTEXT_YAML_Status resolve_custom_write_tag(
    const yaml_writer_state * state,
    const GTEXT_YAML_Node * node,
    const char * tag_override,
    const char ** out_tag_override) {
  const char *tag = tag_override ? tag_override : node_tag(node);
  if (out_tag_override) {
    *out_tag_override = tag_override;
  }
  if (!state || !state->opts || !state->opts->enable_custom_tags) {
    return GTEXT_YAML_OK;
  }
  if (!state->opts->custom_tags || state->opts->custom_tag_count == 0) {
    return GTEXT_YAML_OK;
  }
  if (!tag) {
    return GTEXT_YAML_OK;
  }

  for (size_t i = 0; i < state->opts->custom_tag_count; i++) {
    const GTEXT_YAML_Custom_Tag *handler = &state->opts->custom_tags[i];
    if (!handler->tag || !handler->represent) continue;
    if (strcmp(handler->tag, tag) != 0) continue;

    const char *custom_tag = NULL;
    GTEXT_YAML_Status st = handler->represent(node, tag, handler->user, &custom_tag, NULL);
    if (st != GTEXT_YAML_OK) return st;
    if (custom_tag && out_tag_override) {
      *out_tag_override = custom_tag;
    }
    return GTEXT_YAML_OK;
  }

  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status write_scalar_node(
    yaml_writer_state * state,
    const GTEXT_YAML_Node * node,
    size_t indent,
    bool flow,
    const char * tag_override) {
  const char *resolved_tag = tag_override;
  GTEXT_YAML_Status status = resolve_custom_write_tag(
      state, node, tag_override, &resolved_tag);
  if (status != GTEXT_YAML_OK) return status;

  const char *value = node->as.scalar.value;
  /* A scalar may hold a NUL, so its length is what counts, not its
     terminator. */
  size_t value_len = value ? node->as.scalar.length : 0;
  bool is_binary = false;
  bool canonical = state->opts && state->opts->canonical;
  GTEXT_YAML_Scalar_Style style = state->opts
      ? state->opts->scalar_style
      : GTEXT_YAML_SCALAR_STYLE_PLAIN;

  if (node->type == GTEXT_YAML_STRING && node->as.scalar.has_binary) {
    is_binary = true;
  }
  if (!is_binary) {
    const char *tag = resolved_tag ? resolved_tag : node_tag(node);
    is_binary = tag_is_binary(tag);
  }

  if (canonical) {
    style = GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED;
  } else if (style == GTEXT_YAML_SCALAR_STYLE_PLAIN) {
    style = node->as.scalar.scalar_style;
  }

  /* A style is a preference, and a preference may not change what the
     document says.  Only a plain scalar is resolved by its contents
     (10.3.2), so giving any other style to a scalar that is *not* a string
     makes it one: with opts->scalar_style set, the null went out as "" and
     came back the empty string, 42 went out as "42", and true as "true".  A
     style stored on the node says the same thing through a different door -
     a node built as an int and told it was double-quoted - and is refused
     here for the same reason.

     This is the rule the writer already applies to "~" a few lines below and
     to its quoting whitelist; the option that asks for a style had simply
     never been held to it.  plan_scalar_style() still upgrades PLAIN to a
     quoted style where the text cannot be written plain at all, so nothing
     unprintable escapes through this.

     Canonical form is the exception, and needs no help: it writes "!!int"
     in front of the value, and an explicit tag carries the type whatever the
     quoting does. */
  if (!canonical && node->type != GTEXT_YAML_STRING) {
    style = GTEXT_YAML_SCALAR_STYLE_PLAIN;
  }

  /* The null spellings are asked about before any other style question.  "~"
     is not a character scalar_needs_quotes() would leave alone, and quoting
     it would put the one-character string "~" into the document in place of
     the null the document held.

     Which spellings are nulls is the schema's to say, and the writer used to
     answer for it: it wrote "~", or whatever text the node carried, without
     asking whether the reader this output is for would read either as a
     null.  Under the JSON schema neither the empty scalar nor "~" is one, so
     a null went out as "[~]" and came back as the string "~". */
  if (!canonical && style == GTEXT_YAML_SCALAR_STYLE_PLAIN &&
      node->type == GTEXT_YAML_NULL) {
    const bool empty = (!value || value[0] == '\0');
    const bool as_written =
        writer_text_reads_as_null(state->opts, value, empty ? 0 : value_len);
    /* Properties are enough to make a flow sequence entry a node, so an
       anchored or tagged empty scalar can stay empty even there. */
    const bool has_properties =
        node_anchor(node) || resolved_tag || node_tag(node);
    if (empty && as_written
        && (state->empty_scalar_ok || has_properties)) {
      status = write_node_prefix(state, node, resolved_tag, false);
      if (status != GTEXT_YAML_OK) return status;
      state->key_absorbs_colon = has_properties;
      return write_inline_comment(state, node_inline_comment(node));
    }
    status = write_node_prefix(state, node, resolved_tag, true);
    if (status != GTEXT_YAML_OK) return status;
    /* The node's own text where the target dialect reads it as a null, and
       that dialect's own spelling everywhere else - which covers two cases,
       not one.  The obvious is text the dialect does not read as a null.  The
       other is an *empty* text that it does: ns-flow-seq-entry has no empty
       alternative, so there is nowhere to put it here and the spelling has to
       be written out. */
    status = (as_written && !empty)
        ? write_bytes(state, value, value_len)
        : write_str(state,
              gtext_yaml_null_spelling_for(writer_schema(state->opts)));
    if (status != GTEXT_YAML_OK) return status;
    return write_inline_comment(state, node_inline_comment(node));
  }

  yaml_block_plan block;
  style = plan_scalar_style(
      style, value, value_len, is_binary,
      node->type == GTEXT_YAML_STRING, canonical, flow,
      state->opts && state->opts->pretty, writer_line_width(state->opts),
      indent + (size_t)writer_indent_spaces(state->opts),
      state->block_parent_indent,
      writer_schema(state->opts), writer_yaml_1_1(state->opts), &block);

  status = write_node_prefix(state, node, resolved_tag, true);
  if (status != GTEXT_YAML_OK) return status;

  switch (style) {
    case GTEXT_YAML_SCALAR_STYLE_SINGLE_QUOTED:
      status = write_single_quoted_scalar(state, value, value_len);
      break;
    case GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED:
      status = write_escaped_scalar(state, value, value_len);
      break;
    case GTEXT_YAML_SCALAR_STYLE_LITERAL:
    case GTEXT_YAML_SCALAR_STYLE_FOLDED:
      return write_block_scalar(state, value, indent, &block);
    case GTEXT_YAML_SCALAR_STYLE_PLAIN:
    default:
      status = GTEXT_YAML_OK;
      break;
  }

  if (status != GTEXT_YAML_OK) return status;

  if (style == GTEXT_YAML_SCALAR_STYLE_PLAIN) {
    status = write_bytes(state, value ? value : "", value_len);
    if (status != GTEXT_YAML_OK) return status;
  }

  return write_inline_comment(state, node_inline_comment(node));
}

/* The writer's stack, on the heap.
 *
 * This was a recursive descent: write_sequence_node() and
 * write_mapping_node() called write_node() for each child, at about 228 bytes
 * of C stack a level, so a document past roughly 37000 levels ended the
 * process. max_depth bounds that, and SIZE_MAX is the documented way to say
 * "no bound, I own the stack" - which made SIZE_MAX a way to ask for a
 * segmentation fault and get one.
 *
 * It is the hardest of the three walks to convert, because a collection
 * emits text *between* its children rather than only before or after them:
 * a separator, an indent, a "-" or a ":", and for a mapping a colon whose
 * spelling depends on what the key turned out to be. So a frame carries the
 * step it is up to, and each step emits its share and then asks for one
 * child. The emission itself is unchanged, line for line; only the control
 * flow around it moved.
 *
 * The flow-collection arms displace state->in_flow and state->flow_indent
 * for the length of their children and put them back afterwards - which the
 * recursion did with a local and a goto. Each frame holds what it displaced,
 * and an error unwinds the stack restoring them innermost first, which is
 * the order the returns used to happen in. */
typedef enum {
  WS_ENTER = 0,   /* properties, any opening bracket, per-type checks */
  WS_SEQ_ITEM,    /* one sequence child */
  WS_MAP_KEY,     /* one mapping key */
  WS_MAP_VALUE,   /* that key's value */
  WS_CLOSE        /* a flow collection's closing bracket and comment */
} write_step;

typedef struct {
  const GTEXT_YAML_Node * node;
  size_t indent;
  bool flow;                /* as handed in, before collection_is_flow() */
  const char * tag_override;
  bool leading_newline;

  write_step step;
  size_t i;

  bool coll_flow;           /* what collection_is_flow() decided */
  bool saved_flow;          /* whether the two fields below are live */
  bool was_in_flow;
  size_t was_flow_indent;
  bool explicit_key;        /* block mapping: this pair takes the "? k" form */
} write_frame;

typedef struct {
  write_frame * items;
  size_t count;
  size_t capacity;
} write_stack;

static bool write_stack_push(
    write_stack * stack, const GTEXT_YAML_Node * node, size_t indent,
    bool flow, const char * tag_override, bool leading_newline) {
  if (stack->count == stack->capacity) {
    size_t new_capacity = stack->capacity == 0 ? 32 : stack->capacity * 2;
    write_frame * items = (write_frame *)realloc(
        stack->items, new_capacity * sizeof(write_frame));
    if (!items) return false;
    stack->items = items;
    stack->capacity = new_capacity;
  }
  write_frame * f = &stack->items[stack->count++];
  f->node = node;
  f->indent = indent;
  f->flow = flow;
  f->tag_override = tag_override;
  f->leading_newline = leading_newline;
  f->step = WS_ENTER;
  f->i = 0;
  f->coll_flow = false;
  f->saved_flow = false;
  f->was_in_flow = false;
  f->was_flow_indent = 0;
  f->explicit_key = false;
  return true;
}

/* max_depth, checked where the recursion checked it.
 *
 * A node sitting at nesting level d was refused when d >= max_depth, and d is
 * how many frames are already on the stack when it is pushed - so the test
 * reads the same and fires on the same documents. A document that was parsed
 * has already been held to this once; it is here for the other half, since
 * the DOM constructors do not consult max_depth at all. */
static GTEXT_YAML_Status write_push_child(
    yaml_writer_state * state, write_stack * stack,
    const GTEXT_YAML_Node * child, size_t indent, bool flow,
    const char * tag_override, bool leading_newline) {
  if (state->max_depth > 0 && stack->count >= state->max_depth) {
    return GTEXT_YAML_E_DEPTH;
  }
  if (!write_stack_push(
          stack, child, indent, flow, tag_override, leading_newline)) {
    return GTEXT_YAML_E_OOM;
  }
  return GTEXT_YAML_OK;
}

/* Everything a sequence does before its first child. */
static GTEXT_YAML_Status write_enter_sequence(
    yaml_writer_state * state, write_stack * stack) {
  write_frame * f = &stack->items[stack->count - 1];
  const GTEXT_YAML_Node * node = f->node;
  const size_t indent = f->indent;
  const char * tag_override = f->tag_override;
  GTEXT_YAML_Status status = GTEXT_YAML_OK;

  f->coll_flow = collection_is_flow(state, node, tag_override, f->flow);

  const char *resolved_tag = tag_override;
  status = resolve_custom_write_tag(state, node, tag_override, &resolved_tag);
  if (status != GTEXT_YAML_OK) return status;
  if (!resolved_tag && !node_tag(node) &&
      (node->type == GTEXT_YAML_OMAP || node->type == GTEXT_YAML_PAIRS)) {
    resolved_tag = default_tag_for_type(node->type);
  }
  if (node->type == GTEXT_YAML_OMAP || node->type == GTEXT_YAML_PAIRS) {
    for (size_t i = 0; i < node->as.sequence.count; i++) {
      const GTEXT_YAML_Node *entry = node->as.sequence.children[i];
      if (!entry || entry->type != GTEXT_YAML_MAPPING || entry->as.mapping.count != 1) {
        return GTEXT_YAML_E_INVALID;
      }
    }
  }
  status = write_node_prefix(state, node, resolved_tag, true);
  if (status != GTEXT_YAML_OK) return status;
  if (f->coll_flow) {
    status = write_str(state, "[");
    if (status != GTEXT_YAML_OK) return status;
    /* Raised for the children only.  This collection's *own* trailing
       comment belongs to whatever context holds the collection, which for a
       nested one is the parent's flow and for the outermost is block - so the
       flag goes back before that comment is written.  The continuation indent
       is taken once, on the way into the outermost collection: anything past
       the block node that holds it is deep enough, and nesting need not make
       it deeper. */
    f->saved_flow = true;
    f->was_in_flow = state->in_flow;
    f->was_flow_indent = state->flow_indent;
    if (!state->in_flow) state->flow_indent = indent + 2;
    state->in_flow = true;
    f->step = WS_SEQ_ITEM;
    return GTEXT_YAML_OK;
  }

  if (node->as.sequence.count == 0) {
    stack->count--;
    return write_str(state, "[]");
  }
  f->step = WS_SEQ_ITEM;
  return GTEXT_YAML_OK;
}

/* One sequence child, in either spelling. */
static GTEXT_YAML_Status write_sequence_item(
    yaml_writer_state * state, write_stack * stack) {
  write_frame * f = &stack->items[stack->count - 1];
  const GTEXT_YAML_Node * node = f->node;
  const size_t indent = f->indent;
  const bool leading_newline = f->leading_newline;
  const size_t i = f->i;
  GTEXT_YAML_Status status = GTEXT_YAML_OK;

  if (i >= node->as.sequence.count) {
    if (f->coll_flow) {
      f->step = WS_CLOSE;
      return GTEXT_YAML_OK;
    }
    stack->count--;
    return GTEXT_YAML_OK;
  }
  f->i = i + 1;

  if (f->coll_flow) {
    if (i > 0) {
      status = write_str(state, ", ");
      if (status != GTEXT_YAML_OK) return status;
    }
    /* ns-flow-seq-entry has no empty alternative, so an entry with nothing
       in it and no properties has to be written "~". */
    state->empty_scalar_ok = false;
    return write_push_child(
        state, stack, node->as.sequence.children[i], indent, true, NULL,
        false);
  }

  {
    const GTEXT_YAML_Node *child = node->as.sequence.children[i];
    const char *child_comment = node_leading_comment(child);
    if (i > 0 || leading_newline) {
      status = write_separator(state);
      if (status != GTEXT_YAML_OK) return status;
    } else {
      state->line_terminated = false;
    }
    if (child_comment) {
      status = write_comment_lines(state, child_comment, indent);
      if (status != GTEXT_YAML_OK) return status;
    }
    if (status != GTEXT_YAML_OK) return status;
    status = write_indent(state, indent);
    if (status != GTEXT_YAML_OK) return status;
    status = write_str(state, "-");
    if (status != GTEXT_YAML_OK) return status;

    state->block_parent_indent = (int)indent;
    state->empty_scalar_ok = true;
    if (node_opens_block(state, child, NULL)) {
      status = write_str(state, writer_newline(state->opts));
      if (status != GTEXT_YAML_OK) return status;
      return write_push_child(
          state, stack, child,
          indent + (size_t)writer_indent_spaces(state->opts), false, NULL,
          false);
    }
    status = write_str(
        state, node_writes_nothing(state, child, NULL) ? "" : " ");
    if (status != GTEXT_YAML_OK) return status;
    return write_push_child(
        state, stack, child,
        indent + (size_t)writer_indent_spaces(state->opts),
        !node_scalar_block_style(child), NULL, false);
  }
}

/* Everything a mapping does before its first key. */
static GTEXT_YAML_Status write_enter_mapping(
    yaml_writer_state * state, write_stack * stack) {
  write_frame * f = &stack->items[stack->count - 1];
  const GTEXT_YAML_Node * node = f->node;
  const size_t indent = f->indent;
  const char * tag_override = f->tag_override;
  GTEXT_YAML_Status status = GTEXT_YAML_OK;

  f->coll_flow = collection_is_flow(state, node, tag_override, f->flow);

  const char *resolved_tag = tag_override;
  status = resolve_custom_write_tag(state, node, tag_override, &resolved_tag);
  if (status != GTEXT_YAML_OK) return status;
  if (!resolved_tag && !node_tag(node) && node->type == GTEXT_YAML_SET) {
    resolved_tag = default_tag_for_type(node->type);
  }
  if (node->type == GTEXT_YAML_SET) {
    for (size_t i = 0; i < node->as.mapping.count; i++) {
      const GTEXT_YAML_Node *value = node->as.mapping.pairs[i].value;
      if (value && value->type == GTEXT_YAML_ALIAS) {
        value = value->as.alias.target;
      }
      if (!value || value->type != GTEXT_YAML_NULL) {
        return GTEXT_YAML_E_INVALID;
      }
    }
  }
  status = write_node_prefix(state, node, resolved_tag, true);
  if (status != GTEXT_YAML_OK) return status;

  if (f->coll_flow) {
    status = write_str(state, "{");
    if (status != GTEXT_YAML_OK) return status;
    /* The same displacement, and the same reason, as a flow sequence. */
    f->saved_flow = true;
    f->was_in_flow = state->in_flow;
    f->was_flow_indent = state->flow_indent;
    if (!state->in_flow) state->flow_indent = indent + 2;
    state->in_flow = true;
    f->step = WS_MAP_KEY;
    return GTEXT_YAML_OK;
  }

  if (node->as.mapping.count == 0) {
    stack->count--;
    return write_str(state, "{}");
  }
  f->step = WS_MAP_KEY;
  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status write_mapping_key(
    yaml_writer_state * state, write_stack * stack) {
  write_frame * f = &stack->items[stack->count - 1];
  const GTEXT_YAML_Node * node = f->node;
  const size_t indent = f->indent;
  const bool leading_newline = f->leading_newline;
  const size_t i = f->i;
  GTEXT_YAML_Status status = GTEXT_YAML_OK;

  if (i >= node->as.mapping.count) {
    if (f->coll_flow) {
      f->step = WS_CLOSE;
      return GTEXT_YAML_OK;
    }
    stack->count--;
    return GTEXT_YAML_OK;
  }
  f->step = WS_MAP_VALUE;

  if (f->coll_flow) {
    if (i > 0) {
      status = write_str(state, ", ");
      if (status != GTEXT_YAML_OK) return status;
    }
    state->empty_scalar_ok = true;
    return write_push_child(
        state, stack, node->as.mapping.pairs[i].key, indent, true,
        node->as.mapping.pairs[i].key_tag, false);
  }

  {
    const GTEXT_YAML_Node *key_node = node->as.mapping.pairs[i].key;
    const char *key_comment = node_leading_comment(key_node);
    if (i > 0 || leading_newline) {
      status = write_separator(state);
      if (status != GTEXT_YAML_OK) return status;
    } else {
      state->line_terminated = false;
    }
    if (key_comment) {
      status = write_comment_lines(state, key_comment, indent);
      if (status != GTEXT_YAML_OK) return status;
    }
    if (status != GTEXT_YAML_OK) return status;
    status = write_indent(state, indent);
    if (status != GTEXT_YAML_OK) return status;

    /* A key carrying an inline comment cannot be an implicit one.  A comment
       runs to the end of the line (7.1) and an implicit key has to share its
       line with the ":" that follows it, so "k # note" and ": v" cannot both
       be there: the writer emitted "k # note: v", which reads back as a
       comment and *no entry at all* - a mapping of one going out and a
       mapping of none coming back, with no error to say so.

       7.4's explicit form is where a key and its colon are on separate lines,
       which is how the input that produced such a node was spelled:

         ? k # note
         : v

       so that is what gets written.  The condition is the key's own comment;
       one deeper inside the key - "[a # c]" - ends its line inside the
       brackets and is handled where flow collections are. */
    f->explicit_key = node_inline_comment(key_node) != NULL;
    if (f->explicit_key) {
      status = write_str(state, "? ");
      if (status != GTEXT_YAML_OK) return status;
    }
    state->empty_scalar_ok = true;
    return write_push_child(
        state, stack, key_node, indent, true,
        node->as.mapping.pairs[i].key_tag, false);
  }
}

static GTEXT_YAML_Status write_mapping_value(
    yaml_writer_state * state, write_stack * stack) {
  write_frame * f = &stack->items[stack->count - 1];
  const GTEXT_YAML_Node * node = f->node;
  const size_t indent = f->indent;
  const size_t i = f->i;
  const bool explicit_key = f->explicit_key;
  GTEXT_YAML_Status status = GTEXT_YAML_OK;

  f->i = i + 1;
  f->step = WS_MAP_KEY;

  if (f->coll_flow) {
    status = write_str(state, state->key_absorbs_colon ? " : " : ": ");
    if (status != GTEXT_YAML_OK) return status;
    state->empty_scalar_ok = true;
    return write_push_child(
        state, stack, node->as.mapping.pairs[i].value, indent, true,
        node->as.mapping.pairs[i].value_tag, false);
  }

  if (explicit_key) {
    /* The colon starts its own line, so nothing is adjacent to it and
       key_absorbs_colon has nothing to separate. */
    status = write_str(state, writer_newline(state->opts));
    if (status != GTEXT_YAML_OK) return status;
    status = write_indent(state, indent);
    if (status != GTEXT_YAML_OK) return status;
    state->key_absorbs_colon = false;
  }
  status = write_str(state, state->key_absorbs_colon ? " :" : ":");
  if (status != GTEXT_YAML_OK) return status;

  {
    const GTEXT_YAML_Node *value = node->as.mapping.pairs[i].value;
    state->block_parent_indent = (int)indent;
    state->empty_scalar_ok = true;
    if (node_opens_block(state, value, node->as.mapping.pairs[i].value_tag)) {
      status = write_str(state, writer_newline(state->opts));
      if (status != GTEXT_YAML_OK) return status;
      return write_push_child(
          state, stack, value,
          indent + (size_t)writer_indent_spaces(state->opts), false,
          node->as.mapping.pairs[i].value_tag, false);
    }
    status = write_str(
        state,
        node_writes_nothing(state, value, node->as.mapping.pairs[i].value_tag)
            ? "" : " ");
    if (status != GTEXT_YAML_OK) return status;
    return write_push_child(
        state, stack, value,
        indent + (size_t)writer_indent_spaces(state->opts),
        !node_scalar_block_style(value),
        node->as.mapping.pairs[i].value_tag, false);
  }
}

/* A flow collection's closing bracket, its restores, and its own comment. */
static GTEXT_YAML_Status write_close_flow(
    yaml_writer_state * state, write_stack * stack) {
  write_frame * f = &stack->items[stack->count - 1];
  const GTEXT_YAML_Node * node = f->node;
  const bool is_sequence = node->type == GTEXT_YAML_SEQUENCE
      || node->type == GTEXT_YAML_OMAP || node->type == GTEXT_YAML_PAIRS;
  GTEXT_YAML_Status status = write_str(state, is_sequence ? "]" : "}");

  state->in_flow = f->was_in_flow;
  state->flow_indent = f->was_flow_indent;
  f->saved_flow = false;
  stack->count--;
  if (status != GTEXT_YAML_OK) return status;
  state->key_absorbs_colon = false;
  return write_inline_comment(state, node_inline_comment(node));
}

static GTEXT_YAML_Status write_alias_node(
    yaml_writer_state * state, const GTEXT_YAML_Node * node) {
  const char *name = node->as.alias.anchor_name;
  if (!name && node->as.alias.target) {
    name = node_anchor(node->as.alias.target);
  }
  /* An alias names an anchor, so it is held to the same production. */
  if (!anchor_name_is_writable(name)) {
    return GTEXT_YAML_E_INVALID;
  }

  GTEXT_YAML_Status status = write_str(state, "*");
  if (status != GTEXT_YAML_OK) return status;
  status = write_str(state, name);
  if (status != GTEXT_YAML_OK) return status;
  state->key_absorbs_colon = true;
  return write_inline_comment(state, node_inline_comment(node));
}

/* One step of the walk: advance the top frame, which may finish it or ask
   for a child. Never holds a frame pointer across a push, because a push can
   move the array. */
static GTEXT_YAML_Status write_advance(
    yaml_writer_state * state, write_stack * stack) {
  write_frame * f = &stack->items[stack->count - 1];
  const GTEXT_YAML_Node * node = f->node;

  switch (f->step) {
    case WS_ENTER:
      if (!node) {
        stack->count--;
        return GTEXT_YAML_E_INVALID;
      }
      state->key_absorbs_colon = false;
      switch (node->type) {
        case GTEXT_YAML_STRING:
        case GTEXT_YAML_BOOL:
        case GTEXT_YAML_INT:
        case GTEXT_YAML_FLOAT:
        case GTEXT_YAML_NULL: {
          GTEXT_YAML_Status status = write_scalar_node(
              state, node, f->indent, f->flow, f->tag_override);
          stack->count--;
          return status;
        }
        case GTEXT_YAML_SEQUENCE:
        case GTEXT_YAML_OMAP:
        case GTEXT_YAML_PAIRS:
          return write_enter_sequence(state, stack);
        case GTEXT_YAML_MAPPING:
        case GTEXT_YAML_SET:
          return write_enter_mapping(state, stack);
        case GTEXT_YAML_ALIAS: {
          GTEXT_YAML_Status status = write_alias_node(state, node);
          stack->count--;
          return status;
        }
        default:
          stack->count--;
          return GTEXT_YAML_E_INVALID;
      }
    case WS_SEQ_ITEM:
      return write_sequence_item(state, stack);
    case WS_MAP_KEY:
      return write_mapping_key(state, stack);
    case WS_MAP_VALUE:
      return write_mapping_value(state, stack);
    case WS_CLOSE:
    default:
      return write_close_flow(state, stack);
  }
}

/* Write a node and everything under it.
 *
 * Callers still see the signature the recursion had; what changed is that
 * the nesting lives in a heap array rather than in C frames, so a document
 * the caller has said may be any depth costs memory instead of the process. */
static GTEXT_YAML_Status write_node(
    yaml_writer_state * state,
    const GTEXT_YAML_Node * node,
    size_t indent,
    bool flow,
    const char * tag_override,
    bool leading_newline) {
  write_stack stack = {NULL, 0, 0};
  GTEXT_YAML_Status status = write_push_child(
      state, &stack, node, indent, flow, tag_override, leading_newline);

  while (status == GTEXT_YAML_OK && stack.count > 0) {
    status = write_advance(state, &stack);
  }

  /* An error leaves frames standing, and a flow collection's frame is
     holding the in_flow state it displaced. Innermost first, which is the
     order the returns used to unwind in. */
  while (stack.count > 0) {
    write_frame * f = &stack.items[--stack.count];
    if (f->saved_flow) {
      state->in_flow = f->was_in_flow;
      state->flow_indent = f->was_flow_indent;
    }
  }
  free(stack.items);
  return status;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_write_document(
    const GTEXT_YAML_Document * doc,
    GTEXT_YAML_Sink * sink,
    const GTEXT_YAML_Write_Options * opts) {
  GTEXT_YAML_Write_Options defaults = gtext_yaml_write_options_default();
  yaml_writer_state state;
  yaml_encoding_state encoding;
  GTEXT_YAML_Status status = GTEXT_YAML_OK;
  const GTEXT_YAML_Node *root = NULL;

  /* Zeroed first: this used to set every field by hand, which is correct
     only until the next field is added.  One was, and the DOM writer read
     two uninitialised pointers off the stack.  It is also before the
     argument check, because the cleanup at the end of this function frees
     the anchor set and every exit goes through it - including the ones that
     refuse the arguments.  A zeroed set is a valid empty one. */
  memset(&state, 0, sizeof(state));

  if (!doc || !sink || !sink->write) {
    status = GTEXT_YAML_E_INVALID;
    goto done;
  }

  if (!opts) {
    opts = &defaults;
  }

  /* The document's allocator, so a write is accounted to the same place the
     document is. */
  gtext_yaml_node_set_init(
      &state.anchors_written, doc->ctx ? doc->ctx->alloc : NULL);
  state.sink = sink;
  state.opts = opts;
  /* The document's own limit, not the writer's: a document carries the parse
     options it was made with, and max_depth is one of them. */
  state.max_depth = doc->options.max_depth;
  writer_encoding_init(&encoding, opts);
  state.encoding = &encoding;
  /* s-l+block-node(-1, block-in): the root of a document sits one column to
     the left of column zero, as far as 8.1.1.1 is concerned. */
  state.block_parent_indent = -1;
  state.line_terminated = false;
  /* Nothing holds the root of a document, so an empty root has to be "~". */
  state.empty_scalar_ok = false;
  state.key_absorbs_colon = false;
  root = doc->root;

  if (!root) {
    /* A document with no content is still a document, and the empty node it
       holds resolves to null.  Writing nothing at all said "no documents"
       instead, which is a different stream. */
    status = write_str(&state, "---");
    if (status != GTEXT_YAML_OK) {
      goto done;
    }
    if (opts->trailing_newline) {
      status = write_str(&state, writer_newline(opts));
    }
    goto done;
  }

  status = write_comment_lines(&state, node_leading_comment(root), 0);
  if (status != GTEXT_YAML_OK) {
    goto done;
  }

  status = write_node(&state, root, 0, writer_root_is_flow(opts), NULL, false);
  if (status != GTEXT_YAML_OK) {
    goto done;
  }

  if (opts->trailing_newline) {
    status = write_separator(&state);
    if (status != GTEXT_YAML_OK) {
      goto done;
    }
  }
  if (encoding.pending_utf8_len != 0) {
    status = GTEXT_YAML_E_INVALID;
    goto done;
  }

  status = GTEXT_YAML_OK;
    goto done;

done:
	gtext_yaml_node_set_free(&state.anchors_written);
	return status;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_write_documents(
    GTEXT_YAML_Document * const * docs,
    size_t count,
    GTEXT_YAML_Sink * sink,
    const GTEXT_YAML_Write_Options * opts) {
  GTEXT_YAML_Write_Options defaults = gtext_yaml_write_options_default();
  yaml_writer_state state;
  yaml_encoding_state encoding;
  GTEXT_YAML_Status status = GTEXT_YAML_OK;
  bool wrote_doc = false;

  /* Before the argument check: every exit frees the anchor set, including
     the ones that refuse the arguments.  A zeroed set is a valid empty one. */
  memset(&state, 0, sizeof(state));

  if (!sink || !sink->write) {
    status = GTEXT_YAML_E_INVALID;
    goto done;
  }
  /* An empty stream is a stream.  "# comment only", "..." and a file of one
     blank line all parse to no documents at all, and refusing to write them
     made a legal round trip look like a writer failure. */
  if (count == 0) {
    status = GTEXT_YAML_OK;
    goto done;
  }
  if (!docs) {
    status = GTEXT_YAML_E_INVALID;
    goto done;
  }
  if (!opts) {
    opts = &defaults;
  }

  state.sink = sink;
  state.opts = opts;
  writer_encoding_init(&encoding, opts);
  state.encoding = &encoding;
  state.block_parent_indent = -1;
  state.line_terminated = false;
  /* Nothing holds the root of a document, so an empty root has to be "~". */
  state.empty_scalar_ok = false;
  state.key_absorbs_colon = false;

  for (size_t i = 0; i < count; i++) {
    const GTEXT_YAML_Document *doc = docs[i];
    const GTEXT_YAML_Node *root = NULL;

    if (!doc) {
      status = GTEXT_YAML_E_INVALID;
    goto done;
    }

    if (wrote_doc) {
      status = write_separator(&state);
      if (status != GTEXT_YAML_OK) goto done;
    }

    /* Per document, since each carries the options it was parsed with. */
    state.max_depth = doc->options.max_depth;
    /* And 3.2.2.2's anchor namespace is per document too, so the record of
       which anchors have been written starts empty for each one. */
    gtext_yaml_node_set_free(&state.anchors_written);
    gtext_yaml_node_set_init(
        &state.anchors_written, doc->ctx ? doc->ctx->alloc : NULL);

    state.block_parent_indent = -1;
    status = write_str(&state, "---");
    if (status != GTEXT_YAML_OK) goto done;
    status = write_str(&state, writer_newline(opts));
    if (status != GTEXT_YAML_OK) goto done;

    root = doc->root;
    if (root) {
      status = write_comment_lines(&state, node_leading_comment(root), 0);
      if (status != GTEXT_YAML_OK) goto done;
      status = write_node(&state, root, 0, writer_root_is_flow(opts), NULL, false);
      if (status != GTEXT_YAML_OK) goto done;
    }

    if (opts->trailing_newline) {
      status = write_separator(&state);
      if (status != GTEXT_YAML_OK) goto done;
    }

    wrote_doc = true;
  }

  if (encoding.pending_utf8_len != 0) {
    status = GTEXT_YAML_E_INVALID;
    goto done;
  }

  status = GTEXT_YAML_OK;
    goto done;

done:
	gtext_yaml_node_set_free(&state.anchors_written);
	return status;
}

// ============================================================================
// Streaming Writer (event -> YAML)
// ============================================================================

typedef enum {
  YAML_WRITER_STACK_SEQUENCE,
  YAML_WRITER_STACK_MAPPING
} yaml_writer_stack_type;

typedef struct {
  yaml_writer_stack_type type;
  bool flow;
  bool has_items;
  bool expecting_key;
  bool explicit_key;
  bool is_map_key;
  size_t indent;
} yaml_writer_stack_entry;

struct GTEXT_YAML_Writer {
  GTEXT_YAML_Sink sink;
  GTEXT_YAML_Write_Options opts;
  yaml_encoding_state encoding;
  /* Set when the node just written ended in an anchor, a tag or an alias
     name, all of which would swallow a ':' written straight after them. */
  bool key_absorbs_colon;
  /* Set when a block scalar has already written the break that ends its last
     line, so the next separator must not write a second one. */
  bool line_terminated;
  yaml_writer_stack_entry *stack;
  size_t stack_size;
  size_t stack_capacity;
  bool in_document;
  bool wrote_doc;
  /* Set once the line break that separates the next document from the last
     one has been written.  A directive is written before its document's
     "---", so whichever of the two comes first owes that break. */
  bool doc_separated;
  /* Set once a node has been written at the root of the document being
     written.  A document has exactly one root - l-bare-document is a single
     s-l+block-node (9.2) - and a second one has nowhere to go: the writer
     used to put it straight after the first, so an ALIAS and a SCALAR at
     document level came out as "*a:x", two nodes run together with no
     separator at all.  That is the same habit as answering an INDICATOR with
     OK: inventing structure for events that describe none. */
  bool root_written;
  /* The named tag handles this document's %TAG directives have declared.
     A %TAG applies only to the document it precedes (6.8.2), so the list is
     cleared at each DOCUMENT_END - a handle declared for one document says
     nothing about the next. */
  char **tag_handles;
  size_t tag_handle_count;
  size_t tag_handle_capacity;
  /* Whether the next byte written starts a line.  A "#" is a comment only at
     the start of a line or after white space (7.1); anywhere else it is an
     ordinary character of the scalar it is written against.  The writer had
     no idea where it was, wrote "#" straight after a scalar, and the comment
     became part of the value - "x" with the comment "mid" came out as
     "x# mid" and read back as the one scalar "x# mid".

     Every byte the streaming writer emits goes through writer_write_bytes(),
     so that is where this is kept true. */
  bool at_line_start;
  bool error;
};

/* Forget the handles the document just written declared. */
static void writer_tag_handles_clear(GTEXT_YAML_Writer *writer) {
  for (size_t i = 0; i < writer->tag_handle_count; i++) {
    free(writer->tag_handles[i]);
  }
  writer->tag_handle_count = 0;
}

/* Remember one, so a shorthand using it can be written.  A handle this fails
   to record is a handle write_tag() will refuse, which is the safe direction
   to fail in. */
static int writer_tag_handle_add(GTEXT_YAML_Writer *writer, const char *handle) {
  if (writer->tag_handle_count == writer->tag_handle_capacity) {
    size_t cap = writer->tag_handle_capacity ? writer->tag_handle_capacity * 2 : 4;
    if (cap > 4096) return 1;
    char **grown = (char **)realloc(writer->tag_handles, cap * sizeof(*grown));
    if (!grown) return 1;
    writer->tag_handles = grown;
    writer->tag_handle_capacity = cap;
  }
  size_t len = strlen(handle);
  char *copy = (char *)malloc(len + 1);
  if (!copy) return 1;
  memcpy(copy, handle, len + 1);
  writer->tag_handles[writer->tag_handle_count++] = copy;
  return 0;
}

#define YAML_WRITER_DEFAULT_STACK_CAPACITY 32

static int writer_stack_grow(GTEXT_YAML_Writer *writer) {
  if (writer->stack_size < writer->stack_capacity) {
    return 0;
  }
  size_t new_capacity = writer->stack_capacity == 0
      ? YAML_WRITER_DEFAULT_STACK_CAPACITY
      : writer->stack_capacity * 2;
  if (new_capacity < writer->stack_capacity) {
    return 1;
  }
  if (new_capacity > 1024 * 1024) {
    return 1;
  }
  size_t entry_size = sizeof(yaml_writer_stack_entry);
  if (entry_size > 0 && new_capacity > SIZE_MAX / entry_size) {
    return 1;
  }
  yaml_writer_stack_entry *new_stack = (yaml_writer_stack_entry *)realloc(
      writer->stack, new_capacity * entry_size);
  if (!new_stack) {
    return 1;
  }
  writer->stack = new_stack;
  writer->stack_capacity = new_capacity;
  return 0;
}

static yaml_writer_stack_entry *writer_stack_top(GTEXT_YAML_Writer *writer) {
  if (!writer || writer->stack_size == 0) {
    return NULL;
  }
  return &writer->stack[writer->stack_size - 1];
}

static int writer_stack_push(
    GTEXT_YAML_Writer *writer,
    yaml_writer_stack_type type,
    bool is_map_key,
    bool flow,
    size_t indent) {
  if (writer_stack_grow(writer) != 0) {
    return 1;
  }
  yaml_writer_stack_entry entry = {
      .type = type,
      .flow = flow,
      .has_items = false,
      .expecting_key = (type == YAML_WRITER_STACK_MAPPING),
      .explicit_key = false,
      .is_map_key = is_map_key,
      .indent = indent};
  writer->stack[writer->stack_size++] = entry;
  return 0;
}

static int writer_stack_pop(GTEXT_YAML_Writer *writer,
    yaml_writer_stack_entry *out_entry) {
  if (!writer || writer->stack_size == 0) {
    return 1;
  }
  writer->stack_size--;
  if (out_entry) {
    *out_entry = writer->stack[writer->stack_size];
  }
  return 0;
}

/* The two writers share a sink, a set of options and an encoder, so the
   streaming one borrows the spelling helpers above by handing them a view of
   itself rather than keeping a second copy of every rule.  The two used to
   be written out twice and had drifted: only one of them knew how to spell a
   resolved tag, and neither knew how to chomp a block scalar. */
static void writer_view(GTEXT_YAML_Writer *writer, yaml_writer_state *state) {
  memset(state, 0, sizeof(*state));
  state->sink = &writer->sink;
  state->opts = &writer->opts;
  state->encoding = &writer->encoding;
  state->block_parent_indent = -1;
  state->empty_scalar_ok = false;
  state->tag_handles = (const char *const *)writer->tag_handles;
  state->tag_handle_count = writer->tag_handle_count;
}

static int writer_write_bytes(GTEXT_YAML_Writer *writer,
    const char *bytes, size_t len) {
  if (!writer || !writer->sink.write || !bytes) {
    return 1;
  }
  GTEXT_YAML_Status status = write_encoded_bytes(
      &writer->sink, &writer->encoding, bytes, len);
  if (status != GTEXT_YAML_OK) {
    writer->error = true;
    return 1;
  }
  if (len > 0) {
    const char last = bytes[len - 1];
    writer->at_line_start = (last == '\n' || last == '\r');
  }
  return 0;
}

static int writer_write_char(GTEXT_YAML_Writer *writer, char c) {
  return writer_write_bytes(writer, &c, 1);
}

static int writer_write_string(GTEXT_YAML_Writer *writer, const char *str) {
  if (!str) {
    return 0;
  }
  return writer_write_bytes(writer, str, strlen(str));
}

static int writer_write_indent(GTEXT_YAML_Writer *writer, size_t spaces) {
  char pad[64];
  size_t chunk = sizeof(pad);
  memset(pad, ' ', sizeof(pad));
  while (spaces > 0) {
    size_t to_write = spaces > chunk ? chunk : spaces;
    if (writer_write_bytes(writer, pad, to_write) != 0) return 1;
    spaces -= to_write;
  }
  return 0;
}

/* Thin shims: the rules live in the shared helpers above, and these only
   carry the streaming writer's int-returning convention across. */
static int writer_write_prefix(
    GTEXT_YAML_Writer *writer,
    const char *anchor,
    const char *tag,
    GTEXT_YAML_Node_Type type,
    bool trailing_space) {
  yaml_writer_state view;
  writer_view(writer, &view);
  if (write_properties(&view, anchor, tag, type, trailing_space)
      != GTEXT_YAML_OK) {
    writer->error = true;
    return 1;
  }
  /* An anchor or a tag with nothing after it would swallow a ':'. */
  writer->key_absorbs_colon = !trailing_space && (anchor || tag);
  return 0;
}

/* The break that separates one node from the next - unless a block scalar
   has already written it, in which case a second would leave a blank line
   that '+' chomping would keep as part of the value. */
static int writer_write_separator(GTEXT_YAML_Writer *writer) {
  if (writer->line_terminated) {
    writer->line_terminated = false;
    return 0;
  }
  return writer_write_string(writer, writer_newline(&writer->opts));
}

/* @p empty says the node about to be written puts nothing on the page, so
   the space that would have separated it from its "-" or ":" has nothing
   left to separate. */
static int writer_prepare_scalar(
    GTEXT_YAML_Writer *writer, bool *is_key, bool empty) {
  yaml_writer_stack_entry *top = writer_stack_top(writer);
  if (!top) {
    if (writer->root_written) return 1;   /* a document has one root (9.2) */
    writer->root_written = true;
    if (is_key) *is_key = false;
    return 0;
  }

  if (top->flow) {
    if (top->type == YAML_WRITER_STACK_SEQUENCE) {
      if (top->has_items) {
        if (writer_write_string(writer, ", ") != 0) return 1;
      }
      if (is_key) *is_key = false;
      return 0;
    }
    if (top->type == YAML_WRITER_STACK_MAPPING) {
      if (top->expecting_key) {
        if (top->has_items) {
          if (writer_write_string(writer, ", ") != 0) return 1;
        }
        if (is_key) *is_key = true;
        return 0;
      }
      if (writer_write_string(writer, writer->key_absorbs_colon
              ? (empty ? " :" : " : ") : (empty ? ":" : ": ")) != 0) return 1;
      writer->key_absorbs_colon = false;
      if (is_key) *is_key = false;
      return 0;
    }
    return 1;
  }

  if (top->type == YAML_WRITER_STACK_SEQUENCE) {
    if (top->has_items) {
      if (writer_write_separator(writer) != 0) return 1;
    }
    if (writer_write_indent(writer, top->indent) != 0) return 1;
    if (writer_write_string(writer, empty ? "-" : "- ") != 0) return 1;
    if (is_key) *is_key = false;
    return 0;
  }

  if (top->type == YAML_WRITER_STACK_MAPPING) {
    if (top->expecting_key) {
      if (top->has_items) {
        if (writer_write_separator(writer) != 0) return 1;
      }
      if (writer_write_indent(writer, top->indent) != 0) return 1;
      if (is_key) *is_key = true;
      return 0;
    }

    if (top->explicit_key) {
      if (writer_write_separator(writer) != 0) return 1;
      if (writer_write_indent(writer, top->indent) != 0) return 1;
      top->explicit_key = false;
    }
    if (writer_write_string(writer, writer->key_absorbs_colon
            ? (empty ? " :" : " : ") : (empty ? ":" : ": ")) != 0) return 1;
    writer->key_absorbs_colon = false;
    if (is_key) *is_key = false;
    return 0;
  }

  return 1;
}

static void writer_finish_value(GTEXT_YAML_Writer *writer, bool is_key) {
  yaml_writer_stack_entry *top = writer_stack_top(writer);
  if (!top) {
    return;
  }
  if (top->type == YAML_WRITER_STACK_SEQUENCE) {
    top->has_items = true;
    return;
  }
  if (top->type == YAML_WRITER_STACK_MAPPING) {
    if (is_key) {
      top->expecting_key = false;
    } else {
      top->expecting_key = true;
      top->has_items = true;
      top->explicit_key = false;
    }
  }
}

static GTEXT_YAML_Status writer_emit_scalar(
    GTEXT_YAML_Writer *writer,
    const GTEXT_YAML_Event *event) {
  bool is_key = false;
  const char *value = event->data.scalar.ptr ? event->data.scalar.ptr : "";
  size_t len = event->data.scalar.len;
  GTEXT_YAML_Scalar_Style style = writer->opts.scalar_style;
  yaml_writer_stack_entry *top = writer_stack_top(writer);
  bool in_flow = top ? top->flow : false;
  size_t base_indent = top ? top->indent : 0;
  bool has_properties = event->anchor || event->tag;
  yaml_writer_state view;
  writer_view(writer, &view);

  /* An empty plain scalar is the empty node of 7.2, not the empty string:
     "a:" and "-" are how it is written, and writing "" instead put a string
     into the document that the document never held.  One position has no
     empty alternative - a flow sequence entry with no properties, which
     needs "~" - and the writer's own "---" holds an empty root. */
  bool empty_node = (len == 0)
      && !writer->opts.canonical
      && writer->opts.scalar_style == GTEXT_YAML_SCALAR_STYLE_PLAIN
      && event->scalar_style == GTEXT_YAML_SCALAR_STYLE_PLAIN;
  bool needs_tilde = empty_node && !has_properties && in_flow && top
      && top->type == YAML_WRITER_STACK_SEQUENCE;
  if (needs_tilde) empty_node = false;

  if (writer_prepare_scalar(
          writer, &is_key, empty_node && !has_properties) != 0) {
    return GTEXT_YAML_E_STATE;
  }

  if (writer_write_prefix(
          writer,
          event->anchor,
          event->tag,
          GTEXT_YAML_STRING,
          !empty_node) != 0) {
    return GTEXT_YAML_E_WRITE;
  }

  if (empty_node) {
    writer_finish_value(writer, is_key);
    return GTEXT_YAML_OK;
  }
  if (needs_tilde) {
    /* ns-flow-seq-entry has no empty alternative, so the empty node has to be
       spelled out - in whatever spelling the target dialect reads as a null,
       which is not "~" for all of them. */
    if (writer_write_string(
            writer,
            gtext_yaml_null_spelling_for(writer_schema(&writer->opts))) != 0) {
      return GTEXT_YAML_E_WRITE;
    }
    writer->key_absorbs_colon = false;
    writer_finish_value(writer, is_key);
    return GTEXT_YAML_OK;
  }
  /* 8.1.1.1 measures the indentation indicator from the node that holds the
     scalar: the container's own indent, or -1 at the root of a document. */
  view.block_parent_indent = top ? (int)top->indent : -1;

  if (!writer->opts.canonical && style == GTEXT_YAML_SCALAR_STYLE_PLAIN) {
    style = event->scalar_style;
  }

  yaml_block_plan block;
  style = plan_scalar_style(
      style, value, len,
      /* An event carries no !!binary flag of its own; the tag is the only
         thing that says so, and plan_scalar_style is asked about the value
         either way.  Nor does it say the scalar is a string: an event stream
         reports a scalar as written and leaves resolution to its consumer, so
         a plain one stays plain here and a quoted one arrives already
         quoted. */
      false, false,
      writer->opts.canonical, in_flow, writer->opts.pretty,
      writer_line_width(&writer->opts),
      base_indent + (size_t)writer_indent_spaces(&writer->opts),
      view.block_parent_indent,
      writer_schema(&writer->opts), writer_yaml_1_1(&writer->opts), &block);

  GTEXT_YAML_Status st = GTEXT_YAML_OK;
  switch (style) {
    case GTEXT_YAML_SCALAR_STYLE_SINGLE_QUOTED:
      st = write_single_quoted_scalar(&view, value, len);
      break;
    case GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED:
      st = write_escaped_scalar(&view, value, len);
      break;
    case GTEXT_YAML_SCALAR_STYLE_LITERAL:
    case GTEXT_YAML_SCALAR_STYLE_FOLDED:
      st = write_block_scalar(&view, value, base_indent, &block);
      break;
    default:
      st = write_bytes(&view, value, len);
      break;
  }
  if (st != GTEXT_YAML_OK) {
    writer->error = true;
    return GTEXT_YAML_E_WRITE;
  }
  /* A block scalar ends its own last line, so whatever writes the next
     separator has to know not to write another. */
  writer->line_terminated = view.line_terminated;
  /* Content of any kind stands between a property and a following ':'. */
  if (len > 0 || style != GTEXT_YAML_SCALAR_STYLE_PLAIN) {
    writer->key_absorbs_colon = false;
  }

  writer_finish_value(writer, is_key);
  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status writer_emit_alias(
    GTEXT_YAML_Writer *writer,
    const GTEXT_YAML_Event *event) {
  bool is_key = false;
  if (writer_prepare_scalar(writer, &is_key, false) != 0) {
    return GTEXT_YAML_E_STATE;
  }

  const char *name = event->data.alias_name;
  if (!anchor_name_is_writable(name)) {
    return GTEXT_YAML_E_INVALID;
  }
  if (writer_write_char(writer, '*') != 0) {
    return GTEXT_YAML_E_WRITE;
  }
  if (writer_write_string(writer, name) != 0) {
    return GTEXT_YAML_E_WRITE;
  }
  /* 6.9.2 stops an anchor name only at a flow indicator, so "*b: 1" names
     the anchor "b:".  The colon has to be kept off it. */
  writer->key_absorbs_colon = true;

  writer_finish_value(writer, is_key);
  return GTEXT_YAML_OK;
}

/* A directive holds one line and no white space inside any of its parts, so
   a space or a break in one would run the line into the next and change what
   the directive says. */
static bool directive_word_is_writable(const char *word) {
  if (!word || !*word) return false;
  for (const unsigned char *p = (const unsigned char *)word; *p; p++) {
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '#') {
      return false;
    }
  }
  /* And it has to be characters at all, held to 5.1 like any other part of
     the stream. */
  return scalar_is_printable(word, strlen(word));
}

/* "%YAML 1.2" and "%TAG !e! tag:example.com,2000:app/".

   These used to be dropped without a word, and that is a correctness failure
   rather than a fidelity one.  The event stream reports a tag as it was
   written (5.3), so "!e!foo" arrives still spelled with its handle; dropping
   the "%TAG !e! ..." that declared the handle leaves a document whose handle
   is undefined, which this very parser then refuses.  Feeding the parser's
   own events straight back to the writer produced a document it could not
   read. */
static GTEXT_YAML_Status writer_emit_directive(
    GTEXT_YAML_Writer *writer,
    const GTEXT_YAML_Event *event) {
  /* A directive belongs to the document it precedes (9.2); there is nowhere
     inside one to put it. */
  if (writer->in_document) return GTEXT_YAML_E_STATE;

  const char *parts[3] = {
    event->data.directive.name,
    event->data.directive.value,
    event->data.directive.value2,
  };
  if (!directive_word_is_writable(parts[0])) return GTEXT_YAML_E_INVALID;

  const char *newline = writer_newline(&writer->opts);
  if (writer->wrote_doc && !writer->doc_separated) {
    /* A directive may only follow a document that has been ended
       *explicitly*: l-yaml-stream reaches a directive document through
       l-document-suffix, which is c-document-end (9.2).  Writing only the
       break left the "%" standing after content, and the two ways that goes
       wrong are both bad.  "%TAG" after a document produced a stream this
       parser refuses - correctly, "Directive after content, with no '...' to
       close the document".  "%YAML 1.2" after a *plain* scalar was worse: it
       folded into the scalar, so "a" over "%YAML 1.2" came back as the one
       string "a %YAML 1.2" and nothing was reported at all. */
    if (writer_write_string(writer, newline) != 0) return GTEXT_YAML_E_WRITE;
    if (writer_write_string(writer, "...") != 0) return GTEXT_YAML_E_WRITE;
    if (writer_write_string(writer, newline) != 0) return GTEXT_YAML_E_WRITE;
    writer->doc_separated = true;
  }

  if (writer_write_char(writer, '%') != 0) return GTEXT_YAML_E_WRITE;
  for (int i = 0; i < 3; i++) {
    if (!parts[i] || !*parts[i]) continue;
    if (i > 0) {
      if (!directive_word_is_writable(parts[i])) return GTEXT_YAML_E_INVALID;
      if (writer_write_char(writer, ' ') != 0) return GTEXT_YAML_E_WRITE;
    }
    if (writer_write_string(writer, parts[i]) != 0) return GTEXT_YAML_E_WRITE;
  }
  if (writer_write_string(writer, newline) != 0) return GTEXT_YAML_E_WRITE;

  /* "%TAG !e! tag:example.com,2000:app/" declares "!e!" for this document,
     and a shorthand using it is writable from here until DOCUMENT_END. */
  if (strcmp(parts[0], "TAG") == 0 && tag_handle_is_writable(parts[1])
      && writer_tag_handle_add(writer, parts[1]) != 0) {
    return GTEXT_YAML_E_OOM;
  }
  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status writer_emit_comment(
    GTEXT_YAML_Writer *writer,
    const GTEXT_YAML_Event *event) {
  if (!writer || !event) return GTEXT_YAML_E_INVALID;
  const char *comment = event->data.comment.ptr;
  if (!comment) return GTEXT_YAML_OK;
  /* One event, one comment line: this writes the text straight out and
     splits nothing, so a break here has nowhere to go.  A caller wanting two
     comment lines emits two comment events. */
  if (!comment_text_is_writable(comment, false)) return GTEXT_YAML_E_INVALID;

  yaml_writer_stack_entry *top = writer_stack_top(writer);
  size_t indent = top ? top->indent : 0;

  /* Where the "#" goes depends on where the writer already is, and it used to
     be written as though that were always the start of a line.  It is not: a
     comment event arriving after a scalar lands mid-line, the indent was zero
     at the root, and "- x" then "#" then " mid" ran together into the plain
     scalar "x# mid".  The comment was not lost, which would have been the
     smaller fault - it was read back as part of the value.

     The event says which of the two a caller meant.  An inline comment ends
     the line the writer is on and needs only the white space that makes a
     "#" a comment; a comment of its own needs a line to be on, so anything
     already written on this one is ended first. */
  if (!writer->at_line_start) {
    if (event->data.comment.inline_comment) {
      if (writer_write_string(writer, " ") != 0) return GTEXT_YAML_E_WRITE;
    }
    else {
      if (writer_write_string(writer, writer_newline(&writer->opts)) != 0) {
        return GTEXT_YAML_E_WRITE;
      }
      if (writer_write_indent(writer, indent) != 0) return GTEXT_YAML_E_WRITE;
    }
  }
  else if (writer_write_indent(writer, indent) != 0) {
    return GTEXT_YAML_E_WRITE;
  }
  if (writer_write_string(writer, "#") != 0) return GTEXT_YAML_E_WRITE;
  if (comment[0] != '\0') {
    if (writer_write_string(writer, " ") != 0) return GTEXT_YAML_E_WRITE;
    if (writer_write_string(writer, comment) != 0) return GTEXT_YAML_E_WRITE;
  }
  if (writer_write_string(writer, writer_newline(&writer->opts)) != 0) {
    return GTEXT_YAML_E_WRITE;
  }

  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status writer_emit_container_start(
    GTEXT_YAML_Writer *writer,
    const GTEXT_YAML_Event *event,
    yaml_writer_stack_type type) {
  yaml_writer_stack_entry *parent = writer_stack_top(writer);
  bool is_key = false;
  bool flow = false;
  bool force_flow = false;
  size_t parent_indent = parent ? parent->indent : 0;
  size_t indent_step = (size_t)writer_indent_spaces(&writer->opts);
  size_t child_indent = parent ? parent_indent + (parent->flow ? 0 : indent_step) : 0;
  bool has_prefix = event->anchor || event->tag;

  if (writer->opts.flow_style == GTEXT_YAML_FLOW_STYLE_FLOW) {
    force_flow = true;
  } else if (writer->opts.flow_style == GTEXT_YAML_FLOW_STYLE_BLOCK) {
    force_flow = false;
  } else {
    force_flow = !writer->opts.pretty;
  }

  flow = force_flow;
  if (writer->opts.canonical || has_prefix) {
    flow = true;
  }
  if (parent && parent->flow) {
    flow = true;
  }

  if (!parent) {
    if (writer->root_written) return GTEXT_YAML_E_STATE;
    writer->root_written = true;
    if (writer_write_prefix(
            writer,
            event->anchor,
            event->tag,
            (type == YAML_WRITER_STACK_SEQUENCE)
                ? GTEXT_YAML_SEQUENCE
                : GTEXT_YAML_MAPPING,
            flow) != 0) {
      return GTEXT_YAML_E_WRITE;
    }
    if (flow) {
      char open_char = (type == YAML_WRITER_STACK_SEQUENCE) ? '[' : '{';
      if (writer_write_char(writer, open_char) != 0) return GTEXT_YAML_E_WRITE;
    } else if (has_prefix) {
      if (writer_write_string(writer, writer_newline(&writer->opts)) != 0) {
        return GTEXT_YAML_E_WRITE;
      }
    }
  } else if (parent->flow) {
    if (writer_prepare_scalar(writer, &is_key, false) != 0) {
      return GTEXT_YAML_E_STATE;
    }
    if (writer_write_prefix(
            writer,
            event->anchor,
            event->tag,
            (type == YAML_WRITER_STACK_SEQUENCE)
                ? GTEXT_YAML_SEQUENCE
                : GTEXT_YAML_MAPPING,
            true) != 0) {
      return GTEXT_YAML_E_WRITE;
    }
    char open_char = (type == YAML_WRITER_STACK_SEQUENCE) ? '[' : '{';
    if (writer_write_char(writer, open_char) != 0) return GTEXT_YAML_E_WRITE;
  } else if (parent->type == YAML_WRITER_STACK_SEQUENCE) {
    if (parent->has_items) {
      if (writer_write_separator(writer) != 0) return GTEXT_YAML_E_WRITE;
    }
    if (writer_write_indent(writer, parent_indent) != 0) return GTEXT_YAML_E_WRITE;
    if (flow) {
      if (writer_write_string(writer, "- ") != 0) return GTEXT_YAML_E_WRITE;
      if (writer_write_prefix(
              writer,
              event->anchor,
              event->tag,
              (type == YAML_WRITER_STACK_SEQUENCE)
                  ? GTEXT_YAML_SEQUENCE
                  : GTEXT_YAML_MAPPING,
              true) != 0) {
        return GTEXT_YAML_E_WRITE;
      }
      char open_char = (type == YAML_WRITER_STACK_SEQUENCE) ? '[' : '{';
      if (writer_write_char(writer, open_char) != 0) return GTEXT_YAML_E_WRITE;
    } else {
      if (has_prefix) {
        if (writer_write_string(writer, "- ") != 0) return GTEXT_YAML_E_WRITE;
        if (writer_write_prefix(
                writer,
                event->anchor,
                event->tag,
                (type == YAML_WRITER_STACK_SEQUENCE)
                    ? GTEXT_YAML_SEQUENCE
                    : GTEXT_YAML_MAPPING,
                false) != 0) {
          return GTEXT_YAML_E_WRITE;
        }
      } else {
        if (writer_write_string(writer, "-") != 0) return GTEXT_YAML_E_WRITE;
      }
      if (writer_write_string(writer, writer_newline(&writer->opts)) != 0) {
        return GTEXT_YAML_E_WRITE;
      }
    }
  } else if (parent->type == YAML_WRITER_STACK_MAPPING) {
    if (parent->expecting_key) {
      if (parent->has_items) {
        if (writer_write_separator(writer) != 0) return GTEXT_YAML_E_WRITE;
      }
      if (writer_write_indent(writer, parent_indent) != 0) return GTEXT_YAML_E_WRITE;
      if (writer_write_string(writer, "? ") != 0) return GTEXT_YAML_E_WRITE;
      parent->explicit_key = true;
      is_key = true;

      if (flow) {
        if (writer_write_prefix(
                writer,
                event->anchor,
                event->tag,
                (type == YAML_WRITER_STACK_SEQUENCE)
                    ? GTEXT_YAML_SEQUENCE
                    : GTEXT_YAML_MAPPING,
                true) != 0) {
          return GTEXT_YAML_E_WRITE;
        }
        char open_char = (type == YAML_WRITER_STACK_SEQUENCE) ? '[' : '{';
        if (writer_write_char(writer, open_char) != 0) return GTEXT_YAML_E_WRITE;
      } else {
        if (has_prefix) {
          if (writer_write_prefix(
                  writer,
                  event->anchor,
                  event->tag,
                  (type == YAML_WRITER_STACK_SEQUENCE)
                      ? GTEXT_YAML_SEQUENCE
                      : GTEXT_YAML_MAPPING,
                  false) != 0) {
            return GTEXT_YAML_E_WRITE;
          }
        }
        if (writer_write_string(writer, writer_newline(&writer->opts)) != 0) {
          return GTEXT_YAML_E_WRITE;
        }
      }
    } else {
      if (parent->explicit_key) {
        if (writer_write_separator(writer) != 0) return GTEXT_YAML_E_WRITE;
        if (writer_write_indent(writer, parent_indent) != 0) {
          return GTEXT_YAML_E_WRITE;
        }
        parent->explicit_key = false;
      }
      if (writer_write_string(writer, ":") != 0) return GTEXT_YAML_E_WRITE;
      if (flow || has_prefix) {
        if (writer_write_char(writer, ' ') != 0) return GTEXT_YAML_E_WRITE;
      }

      if (has_prefix) {
        if (writer_write_prefix(
                writer,
                event->anchor,
                event->tag,
                (type == YAML_WRITER_STACK_SEQUENCE)
                    ? GTEXT_YAML_SEQUENCE
                    : GTEXT_YAML_MAPPING,
                flow) != 0) {
          return GTEXT_YAML_E_WRITE;
        }
      }

      if (flow) {
        char open_char = (type == YAML_WRITER_STACK_SEQUENCE) ? '[' : '{';
        if (writer_write_char(writer, open_char) != 0) return GTEXT_YAML_E_WRITE;
      } else {
        if (writer_write_string(writer, writer_newline(&writer->opts)) != 0) {
          return GTEXT_YAML_E_WRITE;
        }
      }
    }
  }

  if (writer_stack_push(writer, type, is_key, flow, child_indent) != 0) {
    return GTEXT_YAML_E_OOM;
  }

  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status writer_emit_container_end(
    GTEXT_YAML_Writer *writer,
    yaml_writer_stack_type type) {
  yaml_writer_stack_entry entry;
  if (writer_stack_pop(writer, &entry) != 0) {
    return GTEXT_YAML_E_STATE;
  }
  if (entry.type != type) {
    return GTEXT_YAML_E_STATE;
  }

  if (entry.flow) {
    char close_char = (type == YAML_WRITER_STACK_SEQUENCE) ? ']' : '}';
    if (writer_write_char(writer, close_char) != 0) {
      return GTEXT_YAML_E_WRITE;
    }
  }
  writer->key_absorbs_colon = false;

  writer_finish_value(writer, entry.is_map_key);
  return GTEXT_YAML_OK;
}

GTEXT_API GTEXT_YAML_Writer * gtext_yaml_writer_new(
    GTEXT_YAML_Sink sink, const GTEXT_YAML_Write_Options * opts) {
  if (!sink.write) {
    return NULL;
  }

  GTEXT_YAML_Writer *writer = (GTEXT_YAML_Writer *)calloc(1, sizeof(*writer));
  if (!writer) {
    return NULL;
  }

  writer->sink = sink;
  writer->at_line_start = true;
  if (opts) {
    writer->opts = *opts;
  } else {
    writer->opts = gtext_yaml_write_options_default();
  }
  writer_encoding_init(&writer->encoding, &writer->opts);
  writer->stack_capacity = YAML_WRITER_DEFAULT_STACK_CAPACITY;
  writer->stack = (yaml_writer_stack_entry *)calloc(
      writer->stack_capacity, sizeof(yaml_writer_stack_entry));
  if (!writer->stack) {
    free(writer);
    return NULL;
  }

  writer->stack_size = 0;
  writer->in_document = false;
  writer->wrote_doc = false;
  writer->error = false;
  return writer;
}

GTEXT_API void gtext_yaml_writer_free(GTEXT_YAML_Writer *writer) {
  if (!writer) {
    return;
  }
  writer_tag_handles_clear(writer);
  free(writer->tag_handles);
  free(writer->stack);
  free(writer);
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_writer_event(
    GTEXT_YAML_Writer *writer, const GTEXT_YAML_Event *event) {
  if (!writer || !event) {
    return GTEXT_YAML_E_INVALID;
  }
  if (writer->error) {
    return GTEXT_YAML_E_WRITE;
  }

  switch (event->type) {
    case GTEXT_YAML_EVENT_STREAM_START:
      return GTEXT_YAML_OK;
    case GTEXT_YAML_EVENT_STREAM_END:
      return gtext_yaml_writer_finish(writer);
    case GTEXT_YAML_EVENT_DOCUMENT_START: {
      const char *newline = writer_newline(&writer->opts);
      if (writer->in_document) {
        return GTEXT_YAML_E_STATE;
      }
      if (writer->wrote_doc && !writer->doc_separated) {
        /* Through the separator, which knows whether a block scalar has
           already ended its own last line.  Writing the break unconditionally
           put a second one there, and with "+" chomping a break is part of
           the value: "|+" over "  a" over a blank line came back with one
           line feed too many.  Clip and strip chomping collapse the extra
           break, which is why this only ever showed on "+" - and only with a
           document after it to write the "---". */
        if (writer_write_separator(writer) != 0) return GTEXT_YAML_E_WRITE;
      }
      if (writer_write_string(writer, "---") != 0) return GTEXT_YAML_E_WRITE;
      if (writer_write_string(writer, newline) != 0) return GTEXT_YAML_E_WRITE;
      writer->in_document = true;
      writer->wrote_doc = true;
      writer->doc_separated = false;
      writer->root_written = false;
      return GTEXT_YAML_OK;
    }
    case GTEXT_YAML_EVENT_DOCUMENT_END: {
      if (!writer->in_document) {
        return GTEXT_YAML_E_STATE;
      }
      if (writer->opts.trailing_newline) {
        /* Through the separator, so a block scalar that has already ended
           its own last line does not gain a blank one - which "+" chomping
           would keep as part of the value. */
        if (writer_write_separator(writer) != 0) return GTEXT_YAML_E_WRITE;
      }
      writer->in_document = false;
      writer_tag_handles_clear(writer);
      return GTEXT_YAML_OK;
    }
    case GTEXT_YAML_EVENT_DIRECTIVE:
      return writer_emit_directive(writer, event);
    case GTEXT_YAML_EVENT_COMMENT:
      return writer_emit_comment(writer, event);
    case GTEXT_YAML_EVENT_SEQUENCE_START:
      return writer_emit_container_start(writer, event, YAML_WRITER_STACK_SEQUENCE);
    case GTEXT_YAML_EVENT_SEQUENCE_END:
      return writer_emit_container_end(writer, YAML_WRITER_STACK_SEQUENCE);
    case GTEXT_YAML_EVENT_MAPPING_START:
      return writer_emit_container_start(writer, event, YAML_WRITER_STACK_MAPPING);
    case GTEXT_YAML_EVENT_MAPPING_END:
      return writer_emit_container_end(writer, YAML_WRITER_STACK_MAPPING);
    case GTEXT_YAML_EVENT_SCALAR:
      return writer_emit_scalar(writer, event);
    case GTEXT_YAML_EVENT_ALIAS:
      return writer_emit_alias(writer, event);
    case GTEXT_YAML_EVENT_INDICATOR:
      /* The writer takes *composed* events: a mapping is MAPPING_START, its
         pairs, MAPPING_END.  The streaming parser does not produce those for
         block collections - it reports the ":" and the "-" as indicators and
         leaves composing to its consumer, which is what GTEXT_YAML_Event's
         own documentation says of them.  The two are therefore not a pipe,
         and this used to answer an indicator with OK and write nothing: a
         caller who joined them got no error and a document with its block
         structure gone, so "a: 1" over "b: 2" came out as "a1b2".

         There is nothing to render here and no way to guess what was meant,
         so it is refused.  A consumer that composes the stream's events -
         which is what the DOM parser is - never sends one. */
      return GTEXT_YAML_E_INVALID;
    default:
      return GTEXT_YAML_E_INVALID;
  }
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_writer_finish(GTEXT_YAML_Writer *writer) {
  if (!writer) {
    return GTEXT_YAML_E_INVALID;
  }
  if (writer->stack_size != 0) {
    return GTEXT_YAML_E_STATE;
  }
  if (writer->in_document) {
    return GTEXT_YAML_E_STATE;
  }
  if (writer->encoding.pending_utf8_len != 0) {
    return GTEXT_YAML_E_INVALID;
  }
  return writer->error ? GTEXT_YAML_E_WRITE : GTEXT_YAML_OK;
}
