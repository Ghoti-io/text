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
 * @file stream.c
 * @brief Minimal streaming parser that wraps the scanner and emits events.
 *
 * This is a skeleton implementation that converts scanner tokens into the
 * streaming event callback. It supports emitting scalar and indicator events.
 */

#define _POSIX_C_SOURCE 200809L  /* for strdup */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include <ghoti.io/text/macros.h>
#include "yaml_internal.h"
#include <ghoti.io/text/yaml/yaml_stream.h>

/* Forward-declare scanner type in case internal header isn't visible at this
  inclusion point due to include-path differences. */
typedef struct GTEXT_YAML_Scanner GTEXT_YAML_Scanner;

/* The white space a directive's parts are separated by is s-separate-in-line
   (6.8), which is s-white+ - and s-white is s-space *or s-tab* (5.5).  This
   split recognised only the space, so "%YAML<tab>1.2" and "%TAG !e!<tab>pfx"
   were read as one run-on word and refused. */
static bool directive_is_space(char c) {
  return c == ' ' || c == '\t';
}

static void directive_split(
  const char *line,
  size_t len,
  char *name,
  size_t name_cap,
  char *arg1,
  size_t arg1_cap,
  char *arg2,
  size_t arg2_cap
) {
  size_t i = 0;
  size_t out = 0;

  if (name_cap > 0) name[0] = '\0';
  if (arg1_cap > 0) arg1[0] = '\0';
  if (arg2_cap > 0) arg2[0] = '\0';

  while (i < len && directive_is_space(line[i])) i++;

  out = 0;
  while (i < len && !directive_is_space(line[i])) {
    if (out + 1 < name_cap) name[out++] = line[i];
    i++;
  }
  if (name_cap > 0) name[out < name_cap ? out : name_cap - 1] = '\0';

  while (i < len && directive_is_space(line[i])) i++;

  out = 0;
  while (i < len && !directive_is_space(line[i])) {
    if (out + 1 < arg1_cap) arg1[out++] = line[i];
    i++;
  }
  if (arg1_cap > 0) arg1[out < arg1_cap ? out : arg1_cap - 1] = '\0';

  while (i < len && directive_is_space(line[i])) i++;

  out = 0;
  while (i < len && !directive_is_space(line[i])) {
    if (out + 1 < arg2_cap) arg2[out++] = line[i];
    i++;
  }
  if (arg2_cap > 0) arg2[out < arg2_cap ? out : arg2_cap - 1] = '\0';
}

struct GTEXT_YAML_Stream {
  GTEXT_YAML_Scanner *scanner;
  GTEXT_YAML_Event_Callback cb;
  void *user;
  GTEXT_YAML_Parse_Options opts;
  size_t total_bytes_consumed;
  size_t current_depth;
  size_t alias_expansion_count;
  char *pending_anchor;  /* Anchor name to attach to next node (malloc'd, NULL if none) */
  char *pending_tag;  /* Tag to attach to next node (malloc'd, NULL if none) */
  int pending_tag_line; /* 1-based line pending_tag was written on */
  int pending_anchor_line; /* 1-based line pending_anchor was written on */
  /* An earlier property that a second one has displaced, still waiting to
     learn which node it belongs to.  See stream_defer_property(). */
  char *outer_anchor;
  int outer_anchor_line;
  char *outer_tag;
  int outer_tag_line;
  /* Where the pending anchor or tag was written. An empty node made out of
     them has to be reported there and not at the token that proved them
     unclaimed, or the parser measures its indentation from the wrong line
     and closes the collection the node belongs to. */
  size_t pending_prop_offset;
  int pending_prop_line;
  int pending_prop_col;
  /* The leftmost property recorded for the node being built, and the line it
     was written on.  Several properties may apply to one node ("&a !!str x",
     or an anchor and a tag on separate lines); the one furthest left is the
     one that has to clear the open collection's indentation. */
  int pending_prop_min_col;
  int pending_prop_min_line;
  /* Where the property's line began, and whether the property was the first
     thing on it. Together these say whether a later line has left the
     position the property was written in. */
  int pending_prop_line_start;
  bool pending_prop_opens_line;
  bool pending_prop_line_dash;
  /* The line the stream is on, the column its first token stood at, whether
     everything on it so far has been a property or a document marker, and
     whether it opened with a "-". A line of nothing but properties
     introduces whatever follows it, however that is indented; a "-" line
     is a sequence entry, which matters because a block sequence may sit at
     its owning key's column but not at a sibling entry's. */
  int cur_line;
  /* A property indicator taken off the scanner whose name or node had not
     arrived yet.  The scanner has no pushback - the "!" is gone by the time
     that is known and there is nowhere to put it back - so the stream holds
     it here and starts from it again on the next feed.  Without this, "---"
     over "! a" lost its tag: the "a" is an ordinary block scalar and needs a
     line of lookahead, the scan came back E_INCOMPLETE, and the "!" went
     with it. */
  bool held_property;
  GTEXT_YAML_Token held_property_tok;
  int cur_line_start;
  bool cur_line_only_props;
  bool cur_line_opens_with_dash;
  bool pending_alias; /* True if alias indicator seen and name is pending */
  bool sync_mode; /* If true, call scanner_finish after each feed */
  bool document_started; /* True if we've emitted DOCUMENT_START */
  bool document_closed; /* True if current document is closed */
  /* What the scanner said when it last refused a token. The status alone
     travels back to the caller through every return in the token loops, and
     the message the scanner wrote went with the stack frame it was written
     in, so callers reported "Parse error" for faults the scanner had already
     described exactly. Keep it here and let them ask. */
  GTEXT_YAML_Error last_error;
};

/**
 * @brief Read the next token, keeping the message if the scan fails.
 *
 * Every scan goes through here. The scanner fills in the code, the message
 * and the position and leaves the rest of the struct it is handed alone, so
 * only those five fields are worth keeping - copying a context_snippet out of
 * a local nobody wrote would hand the caller a pointer into dead stack.
 *
 * An incomplete token is not a failure: it means the input ran out mid-token
 * and more may still arrive.
 */
static GTEXT_YAML_Status stream_scan(
    GTEXT_YAML_Stream *s, GTEXT_YAML_Token *tok) {
  GTEXT_YAML_Error err;
  err.code = GTEXT_YAML_OK;
  err.message = NULL;
  err.offset = 0;
  err.line = 0;
  err.col = 0;
  GTEXT_YAML_Status st = gtext_yaml_scanner_next(s->scanner, tok, &err);
  if (st != GTEXT_YAML_OK && st != GTEXT_YAML_E_INCOMPLETE) {
    /* The scanner's code and the status it returns are the same value on
       every path it takes, and the status is what the caller sees, so record
       that one. */
    s->last_error.code = st;
    s->last_error.message = err.message;
    s->last_error.offset = err.offset;
    s->last_error.line = err.line;
    s->last_error.col = err.col;
  }
  return st;
}

static GTEXT_YAML_Status stream_apply_alias_limit(GTEXT_YAML_Stream *s) {
  if (!s) return GTEXT_YAML_E_INVALID;
  if (s->opts.max_alias_expansion > 0) {
    if (s->alias_expansion_count + 1 > s->opts.max_alias_expansion) {
      return GTEXT_YAML_E_LIMIT;
    }
  }
  s->alias_expansion_count++;
  return GTEXT_YAML_OK;
}

/**
 * @brief Emit the empty node that an unclaimed anchor or tag belongs to.
 *
 * Properties without a node are properties of the empty node (7.2, e-node),
 * which resolves to null - or to the empty string where the tag says str.
 * The stream held them until something arrived that could take them, so
 * "- !!str" lost its entry entirely and "a: &anchor" over "b: *anchor"
 * handed the anchor to b, which then aliased to itself.
 *
 * @p at is the token that proved the properties had no node of their own,
 * and is used only for the position on the event.
 */
/**
 * @brief Whether the pending properties belong to a node that never arrived.
 *
 * A property that opened its own line introduces whatever follows, however
 * that is indented. One written part way along a line belongs to the position
 * it stands in, and a line that begins no further right has left that
 * position:
 *
 *     x: !custom      the "-" line begins further right, so the tag is the
 *       - 1           sequence's
 *
 *     a: !!str        the "b" line begins at the same column, so a's value
 *     b: 1            was never written and the tag is its
 */
static bool stream_props_left_behind(
  const GTEXT_YAML_Stream *s,
  const GTEXT_YAML_Token *tok
) {
  if (!s->pending_anchor && !s->pending_tag) return false;
  /* Inside a flow collection neither the line nor the indentation says
     anything, so the token settles it: a ",", a ":" or the collection's own
     closing bracket cannot be the node the properties name, which means that
     node is empty and they belong to it.  "{a: !!str}" is a's value being
     the empty string, "[&x]" an anchored null entry, and "{!!str : bar}" an
     empty key (spec example 7.2, suite case WZ62).  None of them reached the
     flush: the line tests below all say "same line" for a flow collection
     written on one, so the properties were carried past the end of the
     collection instead. */
  if (s->current_depth > 0 && tok->type == GTEXT_YAML_TOKEN_INDICATOR
      && (tok->u.c == ',' || tok->u.c == ':'
       || tok->u.c == ']' || tok->u.c == '}')) {
    return true;
  }
  /* A ":" on the property's own line, with nothing between the two, is a
     key nobody wrote. c-ns-properties may stand on its own and the node it
     names is then the empty node (7.2), so "!!str : 1" is {"": 1} and
     "&a : 1" is a null key carrying an anchor. Both references agree. The
     same-line test just below would instead carry the properties past the
     ":" to the value, which left the ":" with no key in front of it and the
     document refused outright.

     A property at the *end* of a line is the other thing entirely: it
     introduces whatever the next line holds, so "top3: &node3" over
     "  *alias1 : scalar3" anchors the nested mapping rather than an empty
     key (suite case 26DV). That ":" is on a later line and never reaches
     here; the indentation test below is what protects it. */
  if (tok->type == GTEXT_YAML_TOKEN_INDICATOR && tok->u.c == ':'
      && tok->line == s->pending_prop_line) {
    return true;
  }
  if (tok->line == s->pending_prop_line) return false;
  if (s->pending_prop_opens_line) return false;
  if (s->cur_line_start > s->pending_prop_line_start) return false;
  /* A block sequence may stand at the column of the key that owns it, so a
     "-" there is the value position rather than a sibling - unless the
     property's own line was a sequence entry, in which case it is the next
     entry and the one before it was empty:

         sequence: !!seq      the "-" is sequence's value, so the tag is
         - entry              the sequence's

         - &a                 the "-" is the next entry, so the anchor
         - b                  belongs to the empty one above it */
  if (tok->type == GTEXT_YAML_TOKEN_INDICATOR && tok->u.c == '-'
      && s->cur_line_start == s->pending_prop_line_start
      && !s->pending_prop_line_dash) {
    return false;
  }
  return true;
}

/**
 * @brief Note that a "-" has taken the pending properties for its sequence.
 *
 * From here the properties introduce that sequence, so nothing later on the
 * line can leave them behind - without this the entry's own scalar looked
 * like a line that had, and "sequence: !!seq" over "- entry" gained an empty
 * first entry.
 */
static void stream_props_claimed_by_sequence(GTEXT_YAML_Stream *s) {
  if (!s->pending_anchor && !s->pending_tag) return;
  if (s->cur_line_start != s->pending_prop_line_start) return;
  if (s->pending_prop_line_dash) return;
  s->pending_prop_opens_line = true;
}

/**
 * @brief Record an error the stream itself found, rather than the scanner.
 *
 * stream_scan() keeps what the scanner reports; this keeps what the loop
 * works out for itself, so both reach the caller the same way.
 */
static void stream_fail(
  GTEXT_YAML_Stream *s,
  const GTEXT_YAML_Token *tok,
  const char *msg
) {
  s->last_error.code = GTEXT_YAML_E_INVALID;
  s->last_error.message = msg;
  s->last_error.offset = tok ? tok->offset : 0;
  s->last_error.line = tok ? tok->line : 0;
  s->last_error.col = tok ? tok->col : 0;
}

/**
 * @brief Set a pending property aside when a second one of its kind arrives.
 *
 * A node carries at most one anchor and at most one tag (c-ns-properties,
 * 7.1), so a second one turning up while the first is still pending means one
 * of two things.
 *
 * Written on the same line there is nothing between them, so they name one
 * node and the document is in error:
 *
 *     a: &x &y 1
 *
 * Written on different lines a block collection may open between them, and
 * then they name two different nodes - the collection and its first key:
 *
 *     top1: &node1        &node1 is the mapping's,
 *       &k1 key1: val1    &k1 the key's
 *
 * Whether that collection opens is not known until the token *after* the
 * node, so the first property is set aside here and both travel to the
 * parser, which is where the answer arrives.  If no collection opens, the two
 * named one node after all and the parser refuses it - suite case 4JVG:
 *
 *     top2: &node2
 *       &v2 val2
 *
 * A third has nowhere to go under either reading.
 *
 * Only properties on a line the first has not been left behind by get this
 * far: the caller flushes a left-behind property as its own empty node first,
 * which is what makes one spare slot enough.
 */
static GTEXT_YAML_Status stream_defer_property(
  GTEXT_YAML_Stream *s,
  const GTEXT_YAML_Token *tok,
  char **slot,
  int *slot_line,
  char **outer,
  int *outer_line,
  const char *two_on_one_node
) {
  if (!*slot) return GTEXT_YAML_OK;
  if (*slot_line == tok->line || *outer) {
    stream_fail(s, tok, two_on_one_node);
    return GTEXT_YAML_E_INVALID;
  }
  *outer = *slot;
  *outer_line = *slot_line;
  *slot = NULL;
  *slot_line = 0;
  return GTEXT_YAML_OK;
}

/**
 * @brief Hang the pending properties on an event about to be reported.
 */
static void stream_attach_pending(
  GTEXT_YAML_Stream *s,
  GTEXT_YAML_Event *ev
) {
  ev->anchor = s->pending_anchor;
  ev->anchor_line = s->pending_anchor ? s->pending_anchor_line : 0;
  ev->tag = s->pending_tag;
  ev->tag_line = s->pending_tag ? s->pending_tag_line : 0;
  ev->outer_anchor = s->outer_anchor;
  ev->outer_anchor_line = s->outer_anchor ? s->outer_anchor_line : 0;
  ev->outer_tag = s->outer_tag;
  ev->outer_tag_line = s->outer_tag ? s->outer_tag_line : 0;
  ev->prop_line = s->pending_prop_min_line;
  ev->prop_col = s->pending_prop_min_col;
}

/* Zero an event, and then undo the one field zeroing gets wrong.
 *
 * "No properties here" is spelled -1, because column 0 and line 0 are real
 * places a property could be written. Every event was being cleared with
 * memset and six of the seven sites left prop_line and prop_col at 0 - which
 * reads as "a property, at the very start of the document".
 *
 * That was not visible as a wrong answer, because the one consumer that
 * could have been fooled walks the whole parse stack to decide and every
 * level compares >= against a line number of 0, so it always fell through to
 * "no". It was visible as *time*: property_left_of_open_collection() ran that
 * walk for every property-less event, which on a deeply nested document is
 * O(depth) per event and quadratic overall - 200,010,000 stack steps to parse
 * 20000 nested flow sequences, all of them to compute false.
 *
 * One place to spell the sentinel, so a new event type cannot get it wrong.
 */
static void stream_event_init(
    GTEXT_YAML_Event * ev, GTEXT_YAML_Event_Type type) {
  memset(ev, 0, sizeof(*ev));
  ev->type = type;
  ev->prop_line = -1;
  ev->prop_col = -1;
}

/**
 * @brief Drop the properties an event has just taken.
 *
 * The outer slots go too: a node that took the inner property is the one the
 * outer was waiting on, and from here it is the parser's to place.
 */
static void stream_clear_pending(GTEXT_YAML_Stream *s) {
  gtext_allocator_free(s->opts.allocator, s->pending_anchor);
  s->pending_anchor = NULL;
  s->pending_anchor_line = 0;
  gtext_allocator_free(s->opts.allocator, s->pending_tag);
  s->pending_tag = NULL;
  s->pending_tag_line = 0;
  gtext_allocator_free(s->opts.allocator, s->outer_anchor);
  s->outer_anchor = NULL;
  s->outer_anchor_line = 0;
  gtext_allocator_free(s->opts.allocator, s->outer_tag);
  s->outer_tag = NULL;
  s->outer_tag_line = 0;
  s->pending_prop_min_col = -1;
  s->pending_prop_min_line = -1;
}

static GTEXT_YAML_Status stream_flush_empty_node(
  GTEXT_YAML_Stream *s,
  const GTEXT_YAML_Token *at
) {
  if (!s->pending_anchor && !s->pending_tag) return GTEXT_YAML_OK;

  GTEXT_YAML_Event ev;
  stream_event_init(&ev, GTEXT_YAML_EVENT_SCALAR);
  ev.data.scalar.ptr = "";
  ev.data.scalar.len = 0;
  ev.scalar_style = GTEXT_YAML_SCALAR_STYLE_PLAIN;
  stream_attach_pending(s, &ev);
  ev.offset = s->pending_prop_offset;
  ev.line = s->pending_prop_line;
  ev.col = s->pending_prop_col;
  (void)at;

  GTEXT_YAML_Status rc = GTEXT_YAML_OK;
  if (s->cb) rc = s->cb(s, &ev, s->user);

  stream_clear_pending(s);
  return rc;
}

static GTEXT_YAML_Status stream_emit_alias(GTEXT_YAML_Stream *s, GTEXT_YAML_Token *tok) {
  if (!s || !tok) return GTEXT_YAML_E_INVALID;
  if (tok->type != GTEXT_YAML_TOKEN_SCALAR) {
    return GTEXT_YAML_E_BAD_TOKEN;
  }

  /* Borrowed from the scanner, valid until the next token is requested. */
  const char *name = tok->u.scalar.ptr;
  size_t namelen = tok->u.scalar.len;
  char buf[256];
  if (namelen >= sizeof(buf)) namelen = sizeof(buf) - 1;
  memcpy(buf, name, namelen);
  buf[namelen] = '\0';

  /* An alias node carries no properties of its own: it is "*" and a name and
     nothing else (c-ns-alias-node, 7.1).  So a property still pending when
     one arrives names some other node, and which one its line says.

     On the alias's own line there is nothing else for it to name, and the
     document is in error - the tag used to be dropped here without a word
     and the anchor carried on to whatever came next:

         b: &y *x         &y has no node; "*x" is not one it may have

     On an earlier line it introduces whatever this line opens, and the alias
     is only the first thing inside it:

         top3: &node3          &node3 is the nested mapping's, and the alias
           *alias1 : scalar3   is its key (suite case 26DV)

     Which of the two is not known until the token after the alias, so it
     travels to the parser in the outer slot - the same journey, and the same
     arbiter, as a property a second one displaces.

     **But first the same question every other token kind asks**, and this path
     was the one place that did not ask it. A property is only the alias's
     business at all if the alias's line has not *left it behind*; where it has,
     the property names an empty node and the alias is an unrelated sibling:

         a: &OO        the "*OO" line begins at the same column, so a's value
         *OO :         was never written - &OO is its, and the alias is the
                       next key, carrying nothing

     Deferring unconditionally sent &OO to the parser as an outer property, and
     nothing there could claim it: no collection opens on that line, because
     "*OO" is a key in the mapping already open. check_outer_property_claimed()
     then refused the document with "An alias node may not carry an anchor",
     which is the right message for the case above it and the wrong answer here.
     PyYAML reads it as {'a': None, None: None}.

     Found by the fuzz soak, through the yaml writer's round trip: the writer
     emitted exactly this shape from a tree that was correct, and the parser
     would not read its own output back. Five other token kinds - indicator,
     anchor, non-specific tag, tag, scalar - already paired this check with the
     flush; the alias was the sixth and had only half of it. */
  if (stream_props_left_behind(s, tok)) {
    GTEXT_YAML_Status flush = stream_flush_empty_node(s, tok);
    if (flush != GTEXT_YAML_OK) return flush;
  }
  GTEXT_YAML_Status defer = stream_defer_property(
    s, tok, &s->pending_anchor, &s->pending_anchor_line,
    &s->outer_anchor, &s->outer_anchor_line,
    "An alias node may not carry an anchor");
  if (defer != GTEXT_YAML_OK) return defer;
  defer = stream_defer_property(
    s, tok, &s->pending_tag, &s->pending_tag_line,
    &s->outer_tag, &s->outer_tag_line,
    "An alias node may not carry a tag");
  if (defer != GTEXT_YAML_OK) return defer;

  GTEXT_YAML_Event alias_ev;
  stream_event_init(&alias_ev, GTEXT_YAML_EVENT_ALIAS);
  alias_ev.data.alias_name = buf;
  stream_attach_pending(s, &alias_ev);
  alias_ev.offset = tok->offset;
  alias_ev.line = tok->line;
  alias_ev.col = tok->col;

  GTEXT_YAML_Status alias_limit = stream_apply_alias_limit(s);
  if (alias_limit != GTEXT_YAML_OK) {
    return alias_limit;
  }

  if (s->cb) {
    GTEXT_YAML_Status cb_rc = s->cb(s, &alias_ev, s->user);
    if (cb_rc != GTEXT_YAML_OK) return cb_rc;
  }

  stream_clear_pending(s);
  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status stream_emit_document_start(
  GTEXT_YAML_Stream *s,
  const GTEXT_YAML_Token *tok
) {
  if (!s) return GTEXT_YAML_E_INVALID;

  GTEXT_YAML_Event ev;
  stream_event_init(&ev, GTEXT_YAML_EVENT_DOCUMENT_START);
  if (tok) {
    ev.offset = tok->offset;
    ev.line = tok->line;
    ev.col = tok->col;
    /* Taken from the token rather than from a flag at each call site: the
       marker was written exactly when the token that produced this event is
       the marker itself.  Every other caller hands over the content token
       that implied the boundary, or nothing at all at end of input. */
    ev.explicit_marker = tok->type == GTEXT_YAML_TOKEN_DOCUMENT_START;
  }

  if (s->cb) {
    GTEXT_YAML_Status rc = s->cb(s, &ev, s->user);
    if (rc != GTEXT_YAML_OK) return rc;
  }

  s->document_started = true;
  s->document_closed = false;
  return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status stream_emit_document_end(
  GTEXT_YAML_Stream *s,
  const GTEXT_YAML_Token *tok
) {
  if (!s) return GTEXT_YAML_E_INVALID;

  GTEXT_YAML_Event ev;
  stream_event_init(&ev, GTEXT_YAML_EVENT_DOCUMENT_END);
  if (tok) {
    ev.offset = tok->offset;
    ev.line = tok->line;
    ev.col = tok->col;
    /* Taken from the token rather than from a flag at each call site: the
       marker was written exactly when the token that produced this event is
       the marker itself.  Every other caller hands over the content token
       that implied the boundary, or nothing at all at end of input. */
    ev.explicit_marker = tok->type == GTEXT_YAML_TOKEN_DOCUMENT_END;
  }

  if (s->cb) {
    GTEXT_YAML_Status rc = s->cb(s, &ev, s->user);
    if (rc != GTEXT_YAML_OK) return rc;
  }

  s->document_closed = true;
  return GTEXT_YAML_OK;
}

/**
 * @brief Whether a "!" stands alone as the non-specific tag.
 *
 * "!" on its own is a whole tag property (5.3, c-non-specific-tag) and the
 * node follows it; "!foo" is a shorthand whose name begins at the very next
 * byte. The stream read the token after the "!" as the name either way, so
 * "! a" used the node as its own tag and came back as null.
 */
static bool stream_tag_is_non_specific(
  const GTEXT_YAML_Token *bang,
  const GTEXT_YAML_Token *next
) {
  if (next->offset != bang->offset + 1) return true;
  /* ns-tag-char is ns-uri-char less "!" and the flow indicators, so a "!"
     up against one of those - or against the end of the stream - has no name
     after it and is the whole property: "[!]" is a sequence of one empty
     node, not a tag named "]". */
  if (next->type == GTEXT_YAML_TOKEN_EOF) return true;
  if (next->type == GTEXT_YAML_TOKEN_INDICATOR && next->u.c != '!') return true;
  return false;
}

/**
 * @brief Note which line a token opened, and whether the line is properties
 *        alone.
 *
 * The loop below does this for every token it scans - but a bare "!" hands
 * the token after it straight to the processing step, so that one needs the
 * bookkeeping done for it or the line stays recorded as the "!"'s own.
 */
static void stream_note_line(GTEXT_YAML_Stream *s, const GTEXT_YAML_Token *tok) {
  if (tok->line != s->cur_line) {
    s->cur_line = tok->line;
    s->cur_line_start = tok->col;
    s->cur_line_only_props = true;
    s->cur_line_opens_with_dash =
      tok->type == GTEXT_YAML_TOKEN_INDICATOR && tok->u.c == '-';
  }
  if (!(tok->type == GTEXT_YAML_TOKEN_DOCUMENT_START
      || tok->type == GTEXT_YAML_TOKEN_DOCUMENT_END
      || tok->type == GTEXT_YAML_TOKEN_COMMENT
      || (tok->type == GTEXT_YAML_TOKEN_INDICATOR
          && (tok->u.c == '&' || tok->u.c == '!')))) {
    s->cur_line_only_props = false;
  }
}

static GTEXT_YAML_Status stream_ensure_document_started(
  GTEXT_YAML_Stream *s,
  const GTEXT_YAML_Token *tok
) {
  if (!s->document_started || s->document_closed) {
    return stream_emit_document_start(s, tok);
  }
  return GTEXT_YAML_OK;
}

GTEXT_API GTEXT_YAML_Stream * gtext_yaml_stream_new(
  const GTEXT_YAML_Parse_Options * opts,
  GTEXT_YAML_Event_Callback cb,
  void * user
) {
  /* Read before the structure is allocated: it is the first thing the
     caller's allocator has to own, and the options are where it is named. */
  const GTEXT_Allocator *alloc = opts ? opts->allocator : NULL;
  GTEXT_YAML_Stream *s = (GTEXT_YAML_Stream *)gtext_allocator_malloc(alloc, sizeof(*s));
  if (!s) return NULL;
  memset(s, 0, sizeof(*s));
  s->cb = cb;
  s->user = user;
  s->opts = gtext_yaml_parse_options_effective(opts);
  s->total_bytes_consumed = 0;
  s->current_depth = 0;
  s->alias_expansion_count = 0;
  /* No property pending: column 0 and line 0 are real places, so "none" has
     to be -1 rather than the zero memset() left here.
     
     Both of them. This said exactly this and then set only the column, which
     is how an event with no properties came to report prop_col = -1 beside
     prop_line = 0 - half a sentinel. The two are assigned together in every
     place that records a property, and they have to be cleared together
     too. */
  s->pending_prop_min_col = -1;
  s->pending_prop_min_line = -1;
  s->scanner = gtext_yaml_scanner_new(alloc);
  if (!s->scanner) { gtext_allocator_free(alloc, s); return NULL; }
  return s;
}

GTEXT_API void gtext_yaml_stream_free(GTEXT_YAML_Stream * s)
{
  if (!s) return;
  /* Read before the structure it lives in is released. */
  const GTEXT_Allocator *alloc = s->opts.allocator;
  if (s->scanner) gtext_yaml_scanner_free(s->scanner);
  if (s->pending_anchor) gtext_allocator_free(alloc, s->pending_anchor);
  if (s->pending_tag) gtext_allocator_free(alloc, s->pending_tag);
  if (s->outer_anchor) gtext_allocator_free(alloc, s->outer_anchor);
  if (s->outer_tag) gtext_allocator_free(alloc, s->outer_tag);
  gtext_allocator_free(alloc, s);
}

GTEXT_INTERNAL_API bool gtext_yaml_stream_last_error(
  const GTEXT_YAML_Stream * s,
  GTEXT_YAML_Error * out
) {
  if (!s || !out || s->last_error.code == GTEXT_YAML_OK) return false;
  /* Only the fields stream_scan() kept. The caller's snippet and token
     strings are its own business and are left as it had them. */
  out->code = s->last_error.code;
  out->message = s->last_error.message;
  out->offset = s->last_error.offset;
  out->line = s->last_error.line;
  out->col = s->last_error.col;
  return true;
}

GTEXT_INTERNAL_API void gtext_yaml_stream_retain_decoded_input(
  GTEXT_YAML_Stream *s
) {
  if (s && s->scanner) gtext_yaml_scanner_retain_decoded(s->scanner);
}

GTEXT_INTERNAL_API bool gtext_yaml_stream_decoded_input(
  const GTEXT_YAML_Stream *s,
  const char **data,
  size_t *len
) {
  if (!s) return false;
  return gtext_yaml_scanner_decoded(s->scanner, data, len);
}

/* Internal: Set synchronous mode (for use by gtext_yaml_parse) */
GTEXT_INTERNAL_API void gtext_yaml_stream_set_sync_mode(
  GTEXT_YAML_Stream *s,
  bool sync
) {
  if (s) s->sync_mode = sync;
}

/**
 * @brief Turn every token the scanner can hand over now into events.
 *
 * gtext_yaml_stream_feed() and gtext_yaml_stream_finish() both need this, and
 * each used to carry its own copy of it - four hundred lines, twice - which
 * had drifted a long way apart.  The finish() copy had no %YAML or %TAG
 * handling at all, none of the E_INCOMPLETE guards that keep a property from
 * being taken and then dropped, and none of the tag fixes: "---" over "! a"
 * lost its tag whenever the "!" was still in hand when the input ended, which
 * for the pull reader is every time.
 *
 * Returns GTEXT_YAML_E_INCOMPLETE when it stopped for want of input rather
 * than at the end of the stream, because finish() closes the open document
 * after the one and not after the other.
 */
static GTEXT_YAML_Status stream_drain(GTEXT_YAML_Stream *s) {
  GTEXT_YAML_Token tok;
  if (s->held_property) {
    s->held_property = false;
    tok = s->held_property_tok;
    goto process_token;
  }
  for (;;) {
    GTEXT_YAML_Status st = stream_scan(s, &tok);
    if (st == GTEXT_YAML_E_INCOMPLETE) return GTEXT_YAML_E_INCOMPLETE;
    if (st != GTEXT_YAML_OK) return st;
    if (tok.type == GTEXT_YAML_TOKEN_EOF) {
      GTEXT_YAML_Status flush = stream_flush_empty_node(s, &tok);
      if (flush != GTEXT_YAML_OK) return flush;
      break;
    }
    stream_note_line(s, &tok);

process_token:
    if (s->pending_alias) {
      if (tok.type != GTEXT_YAML_TOKEN_SCALAR) {
        return GTEXT_YAML_E_BAD_TOKEN;
      }
      s->pending_alias = false;
      GTEXT_YAML_Status doc_rc = stream_ensure_document_started(s, &tok);
      if (doc_rc != GTEXT_YAML_OK) return doc_rc;
      GTEXT_YAML_Status alias_rc = stream_emit_alias(s, &tok);
      if (alias_rc != GTEXT_YAML_OK) return alias_rc;
      continue;
    }

    GTEXT_YAML_Event ev;
    /* The type is decided further down; every arm sets it. */
    stream_event_init(&ev, GTEXT_YAML_EVENT_SCALAR);
    ev.offset = tok.offset;
    ev.line = tok.line;
    ev.col = tok.col;

    if (tok.type == GTEXT_YAML_TOKEN_DOCUMENT_START) {
      {
        GTEXT_YAML_Status flush = stream_flush_empty_node(s, &tok);
        if (flush != GTEXT_YAML_OK) return flush;
      }
      if (s->document_started && !s->document_closed) {
        GTEXT_YAML_Status rc = stream_emit_document_end(s, &tok);
        if (rc != GTEXT_YAML_OK) return rc;
      }
      GTEXT_YAML_Status rc = stream_emit_document_start(s, &tok);
      if (rc != GTEXT_YAML_OK) return rc;
      continue;
    }

    if (tok.type == GTEXT_YAML_TOKEN_DOCUMENT_END) {
      {
        GTEXT_YAML_Status flush = stream_flush_empty_node(s, &tok);
        if (flush != GTEXT_YAML_OK) return flush;
      }
      /* A "..." with no document open closes nothing: l-document-suffix
         stands on its own in a stream (9.2), and "..." by itself is a
         stream with no documents in it. Opening one here so that it could
         be closed gave every such suffix a null document of its own - a
         bare "..." parsed as one null, and one between two documents put a
         third between them. */
      if (!s->document_started || s->document_closed) {
        continue;
      }
      GTEXT_YAML_Status rc = stream_emit_document_end(s, &tok);
      if (rc != GTEXT_YAML_OK) return rc;
      continue;
    }

    if (tok.type != GTEXT_YAML_TOKEN_DIRECTIVE && tok.type != GTEXT_YAML_TOKEN_COMMENT) {
      GTEXT_YAML_Status doc_rc = stream_ensure_document_started(s, &tok);
      if (doc_rc != GTEXT_YAML_OK) return doc_rc;
    }

    if (tok.type == GTEXT_YAML_TOKEN_COMMENT) {
      if (s->opts.retain_comments) {
        ev.type = GTEXT_YAML_EVENT_COMMENT;
        ev.data.comment.ptr = tok.u.comment.ptr;
        ev.data.comment.len = tok.u.comment.len;
        ev.data.comment.inline_comment = tok.u.comment.inline_comment;
        if (s->cb) {
          GTEXT_YAML_Status rc = s->cb(s, &ev, s->user);
          if (rc != GTEXT_YAML_OK) return rc;
        } else {
        }
      } else {
      }
      continue;
    }

    if (tok.type == GTEXT_YAML_TOKEN_DIRECTIVE) {
      char name[64];
      char arg1[128];
      char arg2[128];

      directive_split(tok.u.scalar.ptr, tok.u.scalar.len, name, sizeof(name), arg1, sizeof(arg1), arg2, sizeof(arg2));
      ev.type = GTEXT_YAML_EVENT_DIRECTIVE;
      ev.data.directive.name = name[0] ? name : NULL;
      ev.data.directive.value = arg1[0] ? arg1 : NULL;
      ev.data.directive.value2 = arg2[0] ? arg2 : NULL;

      if (s->cb) {
        GTEXT_YAML_Status rc = s->cb(s, &ev, s->user);
        if (rc != GTEXT_YAML_OK) return rc;
      }
      continue;
    }

    if (tok.type == GTEXT_YAML_TOKEN_INDICATOR) {
      /* These end the node position rather than filling it, so properties
         still waiting belong to the empty node - but only where they were
         inside the position this indicator closes. A tag on a line of its
         own introduces the collection that follows it:

             !!seq        the tag belongs to the sequence
             - a

             - &a         the anchor belongs to the first entry, which is
             - b          empty, and the second "-" is what proves it

         The two are told apart by where their lines begin. A property that
         opened its own line introduces whatever follows, however that is
         indented. One written part way along a line belongs to the position
         it stands in, and a later line that begins no further right has
         left that position:

             x: !custom      the "-" line begins further right, so the tag
               - 1           is the sequence's

             a: !!str        the "b" line begins at the same column, so a's
             b: 1            value was never written and the tag is its */
      if ((tok.u.c == '-' || tok.u.c == ':' || tok.u.c == '?'
           || tok.u.c == ',' || tok.u.c == ']' || tok.u.c == '}')
          && (s->pending_anchor || s->pending_tag)
          && stream_props_left_behind(s, &tok)) {
        GTEXT_YAML_Status flush = stream_flush_empty_node(s, &tok);
        if (flush != GTEXT_YAML_OK) return flush;
      }
      if (tok.u.c == '-') stream_props_claimed_by_sequence(s);
      ev.type = GTEXT_YAML_EVENT_INDICATOR;
      ev.data.indicator = tok.u.c;
      /* Properties still looking for their node when an indicator arrives:
         where they were written travels with the indicator, so a consumer
         that is about to decide what this indicator opens can see that
         something is already waiting to name it. */
      if (s->pending_anchor || s->pending_tag) {
        ev.prop_line = s->pending_prop_line;
        ev.prop_col = s->pending_prop_col;
      }
      /* else: stream_event_init() already said "no properties". */
      /* indicator event */
      /* Adjust depth for simple flow indicators and enforce max_depth */
      if (tok.u.c == '[' || tok.u.c == '{') {
        s->current_depth++;
        if (s->opts.max_depth > 0 && s->current_depth > s->opts.max_depth) {
          return GTEXT_YAML_E_DEPTH;
        }
        /* Emit collection START event instead of INDICATOR */
        GTEXT_YAML_Event start_ev;
        stream_event_init(&start_ev, GTEXT_YAML_EVENT_SEQUENCE_START);
        start_ev.type = (tok.u.c == '[')
          ? GTEXT_YAML_EVENT_SEQUENCE_START
          : GTEXT_YAML_EVENT_MAPPING_START;
        stream_attach_pending(s, &start_ev);
        start_ev.offset = tok.offset;
        start_ev.line = tok.line;
        start_ev.col = tok.col;
        
        if (s->cb) {
          GTEXT_YAML_Status rc = s->cb(s, &start_ev, s->user);
          if (rc != GTEXT_YAML_OK) return rc;
        }
        
        stream_clear_pending(s);
        continue;
      } else if (tok.u.c == ']' || tok.u.c == '}') {
        if (s->current_depth > 0) s->current_depth--;
        /* Emit collection END event instead of INDICATOR */
        GTEXT_YAML_Event end_ev;
        stream_event_init(&end_ev, GTEXT_YAML_EVENT_SEQUENCE_END);
        end_ev.type = (tok.u.c == ']')
          ? GTEXT_YAML_EVENT_SEQUENCE_END
          : GTEXT_YAML_EVENT_MAPPING_END;
        end_ev.offset = tok.offset;
        end_ev.line = tok.line;
        end_ev.col = tok.col;
        
        if (s->cb) {
          GTEXT_YAML_Status rc = s->cb(s, &end_ev, s->user);
          if (rc != GTEXT_YAML_OK) return rc;
        }
        continue;
      } else if (tok.u.c == '&') {
        /* Anchor definition: read anchor name and store it.
           The next token (handled by subsequent iteration) will pick it up. */
        GTEXT_YAML_Token name_tok;
        GTEXT_YAML_Status nst = stream_scan(s, &name_tok);
        if (nst == GTEXT_YAML_E_INCOMPLETE) {
          s->held_property = true;
          s->held_property_tok = tok;
          return GTEXT_YAML_E_INCOMPLETE;
        }
        if (nst != GTEXT_YAML_OK) return nst;
        if (name_tok.type != GTEXT_YAML_TOKEN_SCALAR) return GTEXT_YAML_E_BAD_TOKEN;
        
        size_t namelen = name_tok.u.scalar.len;
        char buf[256];
        if (namelen >= sizeof(buf)) namelen = sizeof(buf)-1;
        memcpy(buf, name_tok.u.scalar.ptr, namelen);
        buf[namelen] = '\0';
        
        /* Store anchor name - it will be attached to the next node event */
        /* A property whose node was never written belongs to an empty one, and
           is reported as that before this one takes its place. */
        if (stream_props_left_behind(s, &tok)) {
          GTEXT_YAML_Status flush = stream_flush_empty_node(s, &tok);
          if (flush != GTEXT_YAML_OK) return flush;
        }
        GTEXT_YAML_Status defer = stream_defer_property(
          s, &tok, &s->pending_anchor, &s->pending_anchor_line,
          &s->outer_anchor, &s->outer_anchor_line,
          "Node has more than one anchor");
        if (defer != GTEXT_YAML_OK) return defer;
        s->pending_anchor = gtext_yaml_strdup(buf, s->opts.allocator);
        s->pending_anchor_line = tok.line;
        s->pending_prop_offset = tok.offset;
        s->pending_prop_line = tok.line;
        s->pending_prop_col = tok.col;
        if (s->pending_prop_min_col < 0 || tok.col < s->pending_prop_min_col) {
          s->pending_prop_min_col = tok.col;
          s->pending_prop_min_line = tok.line;
        }
        s->pending_prop_line_start = s->cur_line_start;
        s->pending_prop_opens_line = s->cur_line_only_props;
        s->pending_prop_line_dash = s->cur_line_opens_with_dash;
        
        continue;
      } else if (tok.u.c == '!') {
        GTEXT_YAML_Token tag_tok;
        GTEXT_YAML_Status nst = stream_scan(s, &tag_tok);
        if (nst == GTEXT_YAML_E_INCOMPLETE) {
          s->held_property = true;
          s->held_property_tok = tok;
          return GTEXT_YAML_E_INCOMPLETE;
        }
        if (nst != GTEXT_YAML_OK) return nst;

        char buf[256];
        size_t tag_len = 0;

        if (stream_tag_is_non_specific(&tok, &tag_tok)) {
          /* The "!" was the whole property. Record it and go round again
             with the token just read, which is the node it applies to. */
          /* A property whose node was never written belongs to an empty one, and
             is reported as that before this one takes its place. */
          if (stream_props_left_behind(s, &tok)) {
            GTEXT_YAML_Status flush = stream_flush_empty_node(s, &tok);
            if (flush != GTEXT_YAML_OK) return flush;
          }
          GTEXT_YAML_Status defer = stream_defer_property(
            s, &tok, &s->pending_tag, &s->pending_tag_line,
            &s->outer_tag, &s->outer_tag_line,
            "Node has more than one tag");
          if (defer != GTEXT_YAML_OK) return defer;
          s->pending_tag = gtext_yaml_strdup("!", s->opts.allocator);
          s->pending_tag_line = tok.line;
          /* The non-specific tag is a property like any other and has to
             record where it was written, or nothing downstream can tell
             whether its node was ever written: "a: !" over "b: 2" carried
             the "!" past the empty value it belonged to and hung it on the
             next key instead. */
          s->pending_prop_offset = tok.offset;
          s->pending_prop_line = tok.line;
          s->pending_prop_col = tok.col;
          if (s->pending_prop_min_col < 0 || tok.col < s->pending_prop_min_col) {
            s->pending_prop_min_col = tok.col;
            s->pending_prop_min_line = tok.line;
          }
          s->pending_prop_line_start = s->cur_line_start;
          s->pending_prop_opens_line = s->cur_line_only_props;
          s->pending_prop_line_dash = s->cur_line_opens_with_dash;
          tok = tag_tok;
          if (tok.type == GTEXT_YAML_TOKEN_EOF) {
            GTEXT_YAML_Status flush = stream_flush_empty_node(s, &tok);
            if (flush != GTEXT_YAML_OK) return flush;
            break;
          }
          stream_note_line(s, &tok);
          goto process_token;
        }

        if (tag_tok.type == GTEXT_YAML_TOKEN_INDICATOR && tag_tok.u.c == '!') {
          GTEXT_YAML_Token name_tok;
          nst = stream_scan(s, &name_tok);
          if (nst != GTEXT_YAML_OK) return nst;
          /* c-ns-shorthand-tag is c-tag-handle ns-tag-char+ (5.6), with
             nothing allowed between the handle and the name: the "!!" has to
             be up against its suffix, and a space is not an ns-tag-char, so
             one ends the tag and leaves the handle with no name at all.
             stream_scan() skips whitespace, so it answers with the next token
             whatever stands before it - and "!! str" glued that token on as
             the suffix, yielding the tag "!!str" on an *empty* node with the
             scalar swallowed into the tag name. The document was accepted and
             its only value silently moved; PyYAML refuses it outright
             ("expected URI, but found ' '").
             This is the adjacency test stream_tag_is_non_specific() opens
             with, thirty lines above, for the single-"!" spelling of the same
             question. Both handles need it; only one had it. "a: !! b" and
             "- !! x" looked handled because the glued name failed a namespace
             lookup afterwards, which refused them for the wrong reason and
             named a tag nobody wrote. */
          if (name_tok.type != GTEXT_YAML_TOKEN_SCALAR
              || name_tok.offset != tag_tok.offset + 1) {
            stream_fail(s, &tok, "Tag handle \"!!\" with no name after it");
            return GTEXT_YAML_E_INVALID;
          }
          tag_len = name_tok.u.scalar.len;
          if (tag_len > sizeof(buf) - 3) tag_len = sizeof(buf) - 3;
          buf[0] = '!';
          buf[1] = '!';
          memcpy(buf + 2, name_tok.u.scalar.ptr, tag_len);
          tag_len += 2;
          buf[tag_len] = '\0';
        } else if (tag_tok.type == GTEXT_YAML_TOKEN_SCALAR
            && tag_tok.u.scalar.len >= 2
            && tag_tok.u.scalar.ptr[0] == '<'
            && tag_tok.u.scalar.ptr[tag_tok.u.scalar.len - 1] == '>') {
          /* A verbatim tag: "!<X>" is the tag X exactly as written, with no
             handle to expand and no escapes to decode (5.3).
             The brackets are kept, and they are what says so.  Stripping them
             here left a verbatim tag indistinguishable from a shorthand by
             the time anything looked at it, and a URI beginning "!" is a
             legal one - "!<!a!>" was read as a shorthand naming the handle
             "!a!", and refused for a %TAG nobody had written; with a
             "%TAG ! ..." in force, "!<!a>" was expanded by it.  Both are
             wrong: 5.3 says a verbatim tag is used as written.
             gtext_yaml_node_tag() still answers the bare URI - the resolver
             takes the brackets off once it has seen them. */
          tag_len = tag_tok.u.scalar.len;
          if (tag_len > sizeof(buf) - 1) tag_len = sizeof(buf) - 1;
          memcpy(buf, tag_tok.u.scalar.ptr, tag_len);
          buf[tag_len] = '\0';
        } else if (tag_tok.type == GTEXT_YAML_TOKEN_SCALAR) {
          tag_len = tag_tok.u.scalar.len;
          if (tag_len > sizeof(buf) - 2) tag_len = sizeof(buf) - 2;
          buf[0] = '!';
          memcpy(buf + 1, tag_tok.u.scalar.ptr, tag_len);
          tag_len += 1;
          buf[tag_len] = '\0';
        } else {
          return GTEXT_YAML_E_BAD_TOKEN;
        }

        /* A property whose node was never written belongs to an empty one, and
           is reported as that before this one takes its place. */
        if (stream_props_left_behind(s, &tok)) {
          GTEXT_YAML_Status flush = stream_flush_empty_node(s, &tok);
          if (flush != GTEXT_YAML_OK) return flush;
        }
        GTEXT_YAML_Status defer = stream_defer_property(
          s, &tok, &s->pending_tag, &s->pending_tag_line,
          &s->outer_tag, &s->outer_tag_line,
          "Node has more than one tag");
        if (defer != GTEXT_YAML_OK) return defer;
        s->pending_tag = gtext_yaml_strdup(buf, s->opts.allocator);
        /* tok is the '!' that introduced the tag. */
        s->pending_tag_line = tok.line;
        s->pending_prop_offset = tok.offset;
        s->pending_prop_line = tok.line;
        s->pending_prop_col = tok.col;
        if (s->pending_prop_min_col < 0 || tok.col < s->pending_prop_min_col) {
          s->pending_prop_min_col = tok.col;
          s->pending_prop_min_line = tok.line;
        }
        s->pending_prop_line_start = s->cur_line_start;
        s->pending_prop_opens_line = s->cur_line_only_props;
        s->pending_prop_line_dash = s->cur_line_opens_with_dash;
        continue;
      } else if (tok.u.c == '*') {
        /* Process alias immediately; if name incomplete, defer to next feed */
        GTEXT_YAML_Token next_tok;
        GTEXT_YAML_Status nst = stream_scan(s, &next_tok);
        if (nst == GTEXT_YAML_E_INCOMPLETE) {
          s->pending_alias = true;
          return GTEXT_YAML_OK;
        }
        if (nst != GTEXT_YAML_OK) return nst;
        GTEXT_YAML_Status doc_rc = stream_ensure_document_started(s, &next_tok);
        if (doc_rc != GTEXT_YAML_OK) return doc_rc;
        /* "continue", not "return": an alias is one node among however many
           are left to read.  This used to end the pass, and the rest of the
           document was only read because finish() ran a second copy of this
           loop that did not - so "{ &a [a, &b b]: *b, *a : [c, *b, d]}" was
           read to its first alias and no further. */
        GTEXT_YAML_Status alias_rc = stream_emit_alias(s, &next_tok);
        if (alias_rc != GTEXT_YAML_OK) return alias_rc;
        continue;
      }
      /* Emit remaining indicators (commas, colons, etc.) */
      if (s->cb) {
        GTEXT_YAML_Status rc = s->cb(s, &ev, s->user);
        if (rc != GTEXT_YAML_OK) return rc;
      }
      continue;
    }

    if (tok.type == GTEXT_YAML_TOKEN_DIRECTIVE) {
      char name[64];
      char arg1[128];
      char arg2[128];

      directive_split(tok.u.scalar.ptr, tok.u.scalar.len, name, sizeof(name), arg1, sizeof(arg1), arg2, sizeof(arg2));
      ev.type = GTEXT_YAML_EVENT_DIRECTIVE;
      ev.data.directive.name = name[0] ? name : NULL;
      ev.data.directive.value = arg1[0] ? arg1 : NULL;
      ev.data.directive.value2 = arg2[0] ? arg2 : NULL;

      if (s->cb) {
        GTEXT_YAML_Status rc = s->cb(s, &ev, s->user);
        if (rc != GTEXT_YAML_OK) return rc;
      }
      continue;
    }

    if (tok.type == GTEXT_YAML_TOKEN_SCALAR) {
      /* scalar event */
      if (stream_props_left_behind(s, &tok)) {
        GTEXT_YAML_Status flush = stream_flush_empty_node(s, &tok);
        if (flush != GTEXT_YAML_OK) return flush;
      }
      ev.type = GTEXT_YAML_EVENT_SCALAR;
      ev.data.scalar.ptr = tok.u.scalar.ptr;
      ev.data.scalar.len = tok.u.scalar.len;
      ev.scalar_style = tok.scalar_style;
      stream_attach_pending(s, &ev);
      if (s->cb) {
        GTEXT_YAML_Status rc = s->cb(s, &ev, s->user);
        if (rc != GTEXT_YAML_OK) {
          return rc;
        }
      }
      stream_clear_pending(s);
      continue;
    }
  }

  return GTEXT_YAML_OK;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_stream_feed(
  GTEXT_YAML_Stream * s,
  const char * data,
  size_t len
) {
  if (!s) return GTEXT_YAML_E_INVALID;
  /* Enforce total-bytes limit if set (0 means use library default already applied in opts) */
  if (s->opts.max_total_bytes > 0) {
    if (s->total_bytes_consumed + len > s->opts.max_total_bytes) return GTEXT_YAML_E_LIMIT;
    s->total_bytes_consumed += len;
  }

  if (!gtext_yaml_scanner_feed(s->scanner, data, len)) return GTEXT_YAML_E_OOM;
  
  /* In sync mode, mark scanner as finished immediately so we can process aliases */
  if (s->sync_mode) {
    gtext_yaml_scanner_finish(s->scanner);
  }

  GTEXT_YAML_Status st = stream_drain(s);
  return st == GTEXT_YAML_E_INCOMPLETE ? GTEXT_YAML_OK : st;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_stream_finish(GTEXT_YAML_Stream * s)
{
  if (!s) return GTEXT_YAML_E_INVALID;
  if (!s->scanner) return GTEXT_YAML_OK;
  gtext_yaml_scanner_finish(s->scanner);

  GTEXT_YAML_Status st = stream_drain(s);
  if (st == GTEXT_YAML_E_INCOMPLETE) return GTEXT_YAML_OK;
  if (st != GTEXT_YAML_OK) return st;

  if (s->document_started && !s->document_closed) {
    GTEXT_YAML_Status rc = stream_emit_document_end(s, NULL);
    if (rc != GTEXT_YAML_OK) return rc;
  }

  return GTEXT_YAML_OK;
}
