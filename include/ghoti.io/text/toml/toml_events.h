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
 * @file toml_events.h
 * @brief The document as the sequence of statements it was written as.
 *
 * gtext_toml_parse() answers "what does this document mean". This answers
 * "what does this document say, in the order it says it", which is a different
 * question and the one a formatter, a linter or a diff tool has:
 *
 *   - a table may be opened, left, and reopened lower down as a sub-table of
 *     itself (`[a.b]` before `[a]`), so the tree says nothing about where in
 *     the file a key was written;
 *   - dotted keys interleave with headers;
 *   - and a comment between two keys belongs to neither of them.
 *
 * The events are emitted by the parser itself, as it parses, from the same walk
 * that builds the tree - not by a second grammar. That is the whole design: two
 * readers of one format drift, and the way not to have that happen is not to
 * have two.
 *
 * **There is no pull reader here, and that is a decision rather than an
 * omission.** The JSON, YAML and CSV modules each have one, with the same four
 * calls in the same order, because each of those parsers is genuinely
 * incremental: it can be handed a chunk, told to do what it can, and asked
 * again. This parser cannot, for two reasons that are the format's and not this
 * implementation's:
 *
 *   - TOML's refusals are whole-document. A duplicate key, a table defined
 *     twice, a `[[a]]` against a static `a` - each is decided against
 *     everything read so far, so a reader that answered before the end would be
 *     answering a different question from gtext_toml_parse(), which is two
 *     spellings of "is this a TOML document".
 *   - Nothing here is resumable. There are 39 end-of-buffer tests across the
 *     lexer and the parser, and every one of them currently means "the document
 *     ends here"; in an incremental reader each would have to mean "...or more
 *     input may follow", which is 39 places to get right and a second grammar
 *     in all but name.
 *
 * So a `feed`-shaped reader over this would accumulate the whole document and
 * parse it at the end: a streaming interface over a parser that does not
 * stream, whose caller would believe memory was bounded when it was not. The
 * callback below gives a caller everything such a reader could - including
 * stopping early, by returning anything other than GTEXT_TOML_OK - and promises
 * nothing it does not do.
 */

#ifndef GHOTI_IO_GTEXT_TOML_TOML_EVENTS_H
#define GHOTI_IO_GTEXT_TOML_TOML_EVENTS_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/toml/toml_core.h>
#include <ghoti.io/text/toml/toml_dom.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_TOML_Event_Type
 * @brief What a statement, or a piece of one, is.
 *
 * The stream is the file's own order. A GTEXT_TOML_EVT_KEY is followed by
 * exactly one value - a GTEXT_TOML_EVT_VALUE, or a balanced
 * ARRAY_BEGIN/ARRAY_END or INLINE_TABLE_BEGIN/INLINE_TABLE_END pair - and the
 * BEGIN/END pairs nest. A comment may appear anywhere a comment is legal,
 * including between a key and its value at the versions that allow one there.
 */
typedef enum {
  /** `[a.b]`: everything after this belongs to that table until the next
   *  header. The path is in GTEXT_TOML_Event::key. */
  GTEXT_TOML_EVT_TABLE,
  /** `[[a.b]]`: a new element appended to that array of tables. */
  GTEXT_TOML_EVT_ARRAY_TABLE,
  /** The key of a key-value pair, whose value follows. Inside an inline table
   *  as well as at the top level, since `{ a.b = 1 }` is a dotted key too. */
  GTEXT_TOML_EVT_KEY,
  /** A scalar: string, integer, float, boolean or date-time. The node is
   *  borrowed for the call in GTEXT_TOML_Event::value. */
  GTEXT_TOML_EVT_VALUE,
  GTEXT_TOML_EVT_ARRAY_BEGIN,        ///< `[`
  GTEXT_TOML_EVT_ARRAY_END,          ///< `]`
  GTEXT_TOML_EVT_INLINE_TABLE_BEGIN, ///< `{`
  GTEXT_TOML_EVT_INLINE_TABLE_END,   ///< `}`
  /**
   * A comment, when GTEXT_TOML_Parse_Options::retain_comments asked for one.
   *
   * This is the event with no counterpart in the tree. The DOM keeps the
   * comments it can put back - see gtext_toml_value_leading_comment() - and
   * that is fewer than the file contains: a comment inside `{ }`, or between
   * two array elements, has no node to hang from and no place in the document
   * the writer emits. Here they are all reported, where they were.
   */
  GTEXT_TOML_EVT_COMMENT
} GTEXT_TOML_Event_Type;

/**
 * @struct GTEXT_TOML_Key_Part
 * @brief One segment of a dotted key, decoded.
 *
 * Decoded, so `a."b"`, `a.b` and `a.'b'` all arrive as the same two parts - key
 * identity in TOML is the decoded string. `len` is authoritative: a key may
 * contain a NUL, since a quoted key may name one.
 */
typedef struct {
  const char * data;
  size_t len;
} GTEXT_TOML_Key_Part;

/**
 * @struct GTEXT_TOML_Key
 * @brief A dotted key, from one part upwards.
 *
 * `parts` points into the parser's own scratch and is valid only for the
 * duration of the callback; a consumer that keeps a key copies it.
 */
typedef struct {
  const GTEXT_TOML_Key_Part * parts;
  size_t count;
} GTEXT_TOML_Key;

/**
 * @struct GTEXT_TOML_Event
 * @brief One event, valid only for the duration of the callback.
 *
 * Every event carries where it came from, because that is a reason to be
 * reading events at all: `line` and `col` are 1-based and `offset` indexes the
 * buffer that was handed in, the same three numbers GTEXT_TOML_Error reports.
 */
typedef struct {
  GTEXT_TOML_Event_Type type;
  /** The path, for TABLE, ARRAY_TABLE and KEY; `count` is 0 otherwise. */
  GTEXT_TOML_Key key;
  /**
   * The scalar, for VALUE, and NULL otherwise.
   *
   * A borrowed node, readable with the ordinary accessors
   * (gtext_toml_value_type(), gtext_toml_value_integer(), ...) rather than
   * through a second union spelled out here. It belongs to the document being
   * built and must not be freed or stored.
   */
  const GTEXT_TOML_Value * value;
  /** For COMMENT: the bytes after the `#`, verbatim and without the line
   *  ending, so that `#` followed by these bytes is the line as written. */
  const char * comment;
  size_t comment_len; ///< Length of @ref comment; 0 for a bare `#`.
  /** For COMMENT: whether the comment had a line to itself, rather than
   *  following a statement, a value or a separator on the same line. */
  bool comment_own_line;
  size_t offset; ///< Byte offset of the event's first byte, 0-based.
  int line;      ///< Line, 1-based.
  int col;       ///< Column in characters, 1-based.
} GTEXT_TOML_Event;

/**
 * @brief The callback the event walk calls, once per event.
 *
 * Returning anything other than GTEXT_TOML_OK stops the walk, and that value is
 * what gtext_toml_read_events() returns - so a consumer that has found what it
 * came for stops the parse rather than reading the rest of the file. A callback
 * that stops this way may fill in @p err to say why; nothing else writes to it
 * while the walk is running.
 *
 * @param user The pointer handed to gtext_toml_read_events().
 * @param evt The event. Valid until the callback returns, and no longer.
 * @param err The caller's error struct, to fill in when stopping. May be NULL.
 * @return GTEXT_TOML_OK to continue.
 */
typedef GTEXT_TOML_Status (*GTEXT_TOML_Event_cb)(
    void * user, const GTEXT_TOML_Event * evt, GTEXT_TOML_Error * err);

/**
 * @brief Read a document, reporting every statement in the order written.
 *
 * The same refusals as gtext_toml_parse(), for the same documents, in the same
 * place - it is the same parser, and the tree is still built, because TOML's
 * duplicate-key and redefinition rules are answered against it. What is saved
 * is the tree's *lifetime*, not its cost: nothing is handed back, so a caller
 * that only wanted to look at the file has nothing to free.
 *
 * Events for a statement are emitted *after* it has been checked. A duplicate
 * key produces no KEY event, so a consumer never sees a statement the document
 * turned out not to be allowed to make.
 *
 * @param bytes The document. Must be valid UTF-8, as TOML requires.
 * @param len Its length in bytes.
 * @param opts Options, or NULL for gtext_toml_parse_options_default().
 *   GTEXT_TOML_Parse_Options::retain_comments decides whether COMMENT events
 *   are emitted; everything else applies exactly as it does to a parse.
 * @param cb The callback. NULL is GTEXT_TOML_E_INVALID: a walk with nothing
 *   watching it is gtext_toml_parse() with the result thrown away, and a caller
 *   who means that should say it.
 * @param user Passed to @p cb untouched.
 * @param err Receives the failure, or is zeroed on success. May be NULL.
 * @return GTEXT_TOML_OK, the parse failure, or whatever @p cb returned to stop
 *   the walk.
 */
GTEXT_API GTEXT_TOML_Status gtext_toml_read_events(const char * bytes,
    size_t len, const GTEXT_TOML_Parse_Options * opts, GTEXT_TOML_Event_cb cb,
    void * user, GTEXT_TOML_Error * err);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_TOML_TOML_EVENTS_H
