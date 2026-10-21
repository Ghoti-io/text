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
 * The XML event parser: the incremental driver over the DOM parser's lexer.
 *
 * ## How it is built
 *
 * The lexer (xml_lexer.c, xml_parser.c, xml_doctype.c) reads a construct from a
 * contiguous buffer and fails with GTEXT_XML_E_UNEXPECTED_EOF if the buffer ends
 * inside it. It is not made resumable. Instead this driver only runs it on a
 * construct that is *complete in the buffer*, and the way it knows is a second,
 * much smaller state machine per kind of construct (`scan_ready()`): one that
 * does nothing but find where the construct ends - the `-->` of a comment, the
 * `>` of a tag outside its quotes, the `]` and `>` of a document type. That
 * machine keeps its place between feeds, so deciding "not yet" costs only the
 * bytes that are new. Every byte is therefore looked at a bounded number of
 * times by the scan and, once the construct is complete, once by the lexer:
 * the work is linear in the input however it is cut, and nothing is rescanned
 * from the start of a long comment because one more byte arrived.
 *
 * The scan is advice, not authority. If it says "complete" for something the
 * lexer finds unfinished (which only a malformed construct can cause), the
 * driver undoes the attempt, stops asking until the input ends, and the lexer
 * has the last word then - the verdict is always the lexer's, run on the same
 * bytes as the DOM parse, from the same starting state. If the scan says "not
 * yet" about a construct the lexer would already have refused, the refusal is
 * delayed, never changed.
 *
 * Character data is the one thing not scanned twice: it is consumed as it
 * arrives, and only what could still change is held back - the first bytes of a
 * multi-byte character, a carriage return (a line feed may follow it in the next
 * piece), and up to two `]` (a `>` may follow them).
 *
 * Bytes the parser has read are dropped from the buffer, keeping one byte behind
 * the cursor because the cursor's line count looks at it (CR LF is one line end).
 * `ctx->base` is how many bytes were dropped, so offsets stay the document's.
 * The names of the open elements are kept apart, since the end tag has to be
 * compared with them and the start tag is gone.
 *
 * ## Encodings
 *
 * The first bytes decide the family (xml_sniff()); for a document with no mark
 * that begins `<?xml`, that waits for the declaration to end, since it may name
 * ISO-8859-1 or US-ASCII. Anything that is not UTF-8 is converted piece by piece
 * (xml_converter), which carries a cut code unit or surrogate pair across
 * pieces, and the lexer sees UTF-8 only, as it does for the DOM parse.
 *
 * ## Events
 *
 * Wherever the DOM parser makes a node, it calls an xml_emit_* function
 * (`ctx->stream` is set), and those are here. They build an event in an arena
 * that is reused, call the callback, and keep nothing.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include <ghoti.io/text/xml/xml_stream.h>

#include "xml_internal.h"

/* ---- State -------------------------------------------------------------- */

typedef enum {
  SC_NONE = 0,
  SC_COMMENT,
  SC_PI,
  SC_CDATA,
  SC_END_TAG,
  SC_START_TAG,
  SC_DOCTYPE,
  SC_DECL,
  SC_REF
} scan_kind;

/* The states of the document type scan. */
enum {
  DT_HEAD = 0, /* before '[', or after ']': outside any literal */
  DT_HEAD_Q,   /* in a quoted literal of the head */
  DT_SUB,      /* in the internal subset, between declarations */
  DT_LT,       /* after '<' */
  DT_LT_BANG,  /* after "<!" */
  DT_LT_BANG_DASH, /* after "<!-" */
  DT_COMMENT,
  DT_PI,
  DT_DECL,
  DT_DECL_Q
};

/* Where the search for the end of the pending construct has got to. */
typedef struct {
  scan_kind kind;
  size_t off;   /* bytes of the construct already examined */
  int state;
  size_t run;   /* dashes, brackets or question marks just seen */
  unsigned char quote;
  int unreliable; /* the scan and the lexer disagreed: wait for the end */
} xml_scan;

typedef enum {
  ST_DETECT = 0, /* the encoding is not known yet */
  ST_DECL,       /* deciding whether there is an XML declaration, reading it */
  ST_PROLOG,     /* before the root element */
  ST_CONTENT,    /* inside the root element */
  ST_EPILOG,     /* after the root element */
  ST_DONE
} drive_state;

typedef struct {
  size_t local;  /* offsets into the event arena */
  size_t prefix; /* SIZE_MAX for none */
  size_t value;
} attr_offsets;

struct GTEXT_XML_Stream {
  xml_ctx p;
  GTEXT_Allocator allocator; /* a copy: the options' struct need not outlive us */
  GTEXT_XML_Event_cb callback;
  void * user;

  markup_buf input; /* what has not been read yet, and a byte before it */
  drive_state state;
  xml_scan scan;
  int final;    /* finish() was called */
  int finished; /* ...and succeeded */
  int final_prepared; /* decoder flushed after finish() */
  int document_end_sent;
  int validation_result_sent;
  int pause_requested;
  int paused;
  int stopped;  /* the callback returned an error */
  int failed;
  int last_empty; /* the start tag just read was an empty-element tag */
  int eager;      /* tests: attempt every construct whenever input arrives */
  int seen_doctype;
  int doctype_active; /* a pull reader yielded inside this declaration */

  size_t max_input;
  size_t fed;
  size_t max_nodes; /* 0: no limit */
  size_t nodes;

  int detected;
  int has_close;         /* a '>' is among the bytes held for detection */
  size_t work;           /* tests: bytes examined while deciding the encoding */
  size_t detect_scanned;
  int converting;
  xml_converter converter;

  markup_buf arena; /* the strings of the event being built */
  attr_offsets * offsets;
  size_t offsets_capacity;
  GTEXT_XML_Event_Attribute * attributes;
  size_t attributes_capacity;
};

int xml_stream_resolver_callback_active(const GTEXT_XML_Stream * stream) {
  return stream && stream->p.resolver_callback_active != 0;
}

void xml_stream_note_resolver_reentry(GTEXT_XML_Stream * stream) {
  if (stream) stream->p.resolver_reentered = 1;
}

void xml_stream_request_pause(GTEXT_XML_Stream * stream) {
  if (stream) {
    stream->pause_requested = 1;
  }
}

static const unsigned char kEmpty[1] = {0};

/* How much of a document is held waiting for the end of an XML declaration, which
   may name the encoding the rest is read in. */
#define XML_DECLARATION_WAIT ((size_t)64 * 1024)

/* ---- Small things ------------------------------------------------------- */

static void refresh(GTEXT_XML_Stream * s) {
  if (s->p.source_count > 1) {
    const xml_source_frame * f = &s->p.sources[s->p.source_count - 1];
    s->p.in = f->bytes ? f->bytes : kEmpty;
    s->p.length = f->length;
  }
  else {
    s->p.in = s->input.data ? (const unsigned char *)s->input.data : kEmpty;
    s->p.length = s->input.length;
  }
}

static size_t available(const GTEXT_XML_Stream * s) {
  return s->p.length - s->p.pos;
}

typedef enum { LOOK_MISMATCH = 0, LOOK_MATCH, LOOK_MORE } look_result;

/* Is the input at the cursor `literal`? MORE: so far it is, and the input ends
   before the literal does - when more may still come. After finish() a literal
   cut short is simply not there, as for the DOM parse. */
static look_result look(const GTEXT_XML_Stream * s, const char * literal) {
  const size_t length = strlen(literal);
  const size_t have = available(s);
  const size_t n = have < length ? have : length;
  if (memcmp(s->p.in + s->p.pos, literal, n) != 0) {
    return LOOK_MISMATCH;
  }
  if (have >= length) {
    return LOOK_MATCH;
  }
  return s->final ? LOOK_MISMATCH : LOOK_MORE;
}

/* Where, in in[from, to), a UTF-8 character that is a valid start of one cut
   short by the end begins; `to` if the end is not inside a character. */
static size_t utf8_cut_start(const unsigned char * in, size_t from, size_t to) {
  size_t i = to;
  size_t back = 0;
  while (i > from && back < 3) {
    i--;
    back++;
    const unsigned char b = in[i];
    if ((b & 0xC0) == 0x80) {
      continue;
    }
    size_t need;
    if (b >= 0xC2 && b <= 0xDF) {
      need = 2;
    }
    else if (b >= 0xE0 && b <= 0xEF) {
      need = 3;
    }
    else if (b >= 0xF0 && b <= 0xF4) {
      need = 4;
    }
    else {
      return to; /* ASCII, or a byte no character starts with */
    }
    const size_t have = to - i;
    if (have >= need) {
      return to;
    }
    if (have >= 2) {
      const unsigned char b1 = in[i + 1];
      if ((b == 0xE0 && b1 < 0xA0) || (b == 0xED && b1 > 0x9F)
          || (b == 0xF0 && b1 < 0x90) || (b == 0xF4 && b1 > 0x8F)) {
        return to;
      }
    }
    return i;
  }
  return to;
}

/* ---- The scan for the end of a construct -------------------------------- */

static void scan_begin(GTEXT_XML_Stream * s, scan_kind kind) {
  if (s->scan.kind == kind) {
    return;
  }
  memset(&s->scan, 0, sizeof(s->scan));
  s->scan.kind = kind;
  switch (kind) {
    case SC_COMMENT:
      s->scan.off = 4;
      break;
    case SC_PI:
    case SC_DECL:
    case SC_END_TAG:
      s->scan.off = 2;
      break;
    case SC_CDATA:
      s->scan.off = 9;
      break;
    case SC_DOCTYPE:
      s->scan.off = 9;
      break;
    case SC_START_TAG:
    case SC_REF:
      s->scan.off = 1;
      break;
    case SC_NONE:
      break;
  }
}

static int ref_body_byte(unsigned char x) {
  return (x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') || (x >= '0' && x <= '9')
      || x == '_' || x == '-' || x == '.' || x == ':' || x == '#' || x >= 0x80;
}

/* One byte of a document type. Returns 1 when it is the '>' that ends it. */
static int doctype_byte(xml_scan * c, unsigned char x) {
again:
  switch (c->state) {
    case DT_HEAD:
      if (x == '"' || x == '\'') {
        c->quote = x;
        c->state = DT_HEAD_Q;
      }
      else if (x == '[') {
        c->state = DT_SUB;
      }
      else if (x == '>') {
        return 1;
      }
      break;
    case DT_HEAD_Q:
      if (x == c->quote) {
        c->state = DT_HEAD;
      }
      break;
    case DT_SUB:
      if (x == ']') {
        c->state = DT_HEAD;
      }
      else if (x == '<') {
        c->state = DT_LT;
      }
      break;
    case DT_LT:
      if (x == '!') {
        c->state = DT_LT_BANG;
      }
      else if (x == '?') {
        c->state = DT_PI;
        c->run = 0;
      }
      else {
        c->state = DT_DECL;
        goto again;
      }
      break;
    case DT_LT_BANG:
      if (x == '-') {
        c->state = DT_LT_BANG_DASH;
      }
      else {
        c->state = DT_DECL;
        goto again;
      }
      break;
    case DT_LT_BANG_DASH:
      if (x == '-') {
        c->state = DT_COMMENT;
        c->run = 0;
      }
      else {
        c->state = DT_DECL;
        goto again;
      }
      break;
    case DT_COMMENT:
      if (x == '-') {
        c->run++;
      }
      else {
        if (x == '>' && c->run >= 2) {
          c->state = DT_SUB;
        }
        c->run = 0;
      }
      break;
    case DT_PI:
      if (x == '>' && c->run) {
        c->state = DT_SUB;
      }
      c->run = x == '?';
      break;
    case DT_DECL:
      if (x == '"' || x == '\'') {
        c->quote = x;
        c->state = DT_DECL_Q;
      }
      else if (x == '>') {
        c->state = DT_SUB;
      }
      break;
    case DT_DECL_Q:
      if (x == c->quote) {
        c->state = DT_DECL;
      }
      break;
  }
  return 0;
}

/* Has the pending construct, which begins at the cursor, ended in the buffer?
   Advances the scan over the bytes that are new. */
static int scan_ready(GTEXT_XML_Stream * s) {
  xml_scan * c = &s->scan;
  if (c->unreliable) {
    return 0;
  }
  const unsigned char * b = s->p.in + s->p.pos;
  const size_t n = available(s);
  for (; c->off < n; c->off++) {
    const unsigned char x = b[c->off];
    switch (c->kind) {
      case SC_COMMENT:
        if (x == '-') {
          c->run++;
        }
        else {
          if (x == '>' && c->run >= 2) {
            return 1;
          }
          c->run = 0;
        }
        break;
      case SC_PI:
        if (x == '>' && c->run) {
          return 1;
        }
        c->run = x == '?';
        break;
      case SC_DECL:
        /* The pseudo-attribute values are quoted, and a "?>" inside one does
           not end the declaration. */
        if (c->quote) {
          if (x == c->quote) {
            c->quote = 0;
            c->run = 0;
          }
        }
        else if (x == '"' || x == '\'') {
          c->quote = x;
        }
        else {
          if (x == '>' && c->run) {
            return 1;
          }
          c->run = x == '?';
        }
        break;
      case SC_CDATA:
        if (x == ']') {
          c->run++;
        }
        else {
          if (x == '>' && c->run >= 2) {
            return 1;
          }
          c->run = 0;
        }
        break;
      case SC_END_TAG:
        if (x == '>') {
          return 1;
        }
        break;
      case SC_START_TAG:
        if (c->quote) {
          if (x == c->quote) {
            c->quote = 0;
          }
        }
        else if (x == '"' || x == '\'') {
          c->quote = x;
        }
        else if (x == '>') {
          return 1;
        }
        break;
      case SC_DOCTYPE:
        if (doctype_byte(c, x)) {
          return 1;
        }
        break;
      case SC_REF:
        if (!ref_body_byte(x)) {
          return 1;
        }
        break;
      case SC_NONE:
        return 0;
    }
  }
  return 0;
}

/* ---- Running the lexer on a construct ----------------------------------- */

typedef GTEXT_XML_Status (*lexer_fn)(xml_ctx * p);

static GTEXT_XML_Status run_declaration(xml_ctx * p) {
  return xml_parse_xml_declaration(p);
}
static GTEXT_XML_Status run_comment(xml_ctx * p) {
  return xml_parse_comment(p, 1);
}
static GTEXT_XML_Status run_pi(xml_ctx * p) {
  return xml_parse_pi(p, 1);
}
static GTEXT_XML_Status run_doctype(xml_ctx * p) {
  return xml_parse_doctype(p);
}
static GTEXT_XML_Status run_cdata(xml_ctx * p) {
  return xml_parse_cdata(p);
}
static GTEXT_XML_Status run_end_tag(xml_ctx * p) {
  return xml_parse_end_tag(p);
}
static GTEXT_XML_Status run_start_tag(xml_ctx * p) {
  int is_empty = 0;
  const GTEXT_XML_Status status = xml_parse_start_tag(p, &is_empty);
  p->stream->last_empty = is_empty;
  return status;
}
static GTEXT_XML_Status run_reference(xml_ctx * p) {
  if (p->text.length == 0) {
    p->text_where = xml_here(p);
  }
  return xml_parse_reference(p, &p->text);
}

/* Run the lexer on the construct at the cursor, which the scan says is complete
   (or which the end of the input has made final). OK with *done set: it was
   read. OK with *done clear: the lexer found it unfinished after all, the
   attempt is undone, and the scan will not be trusted again until the end. Any
   other status is the verdict. */
static GTEXT_XML_Status attempt(
    GTEXT_XML_Stream * s, lexer_fn fn, int * done, int source_complete) {
  xml_ctx * p = &s->p;
  const size_t pos = p->pos;
  const size_t line = p->line;
  const size_t column = p->column;
  const uint32_t cursor_previous = p->cursor_previous;
  const GTEXT_XML_Status status = fn(p);
  if (p->dtd_yielded) {
    p->dtd_yielded = 0;
    *done = 0;
    return GTEXT_XML_OK;
  }
  if (status == GTEXT_XML_OK) {
    *done = 1;
    s->scan.kind = SC_NONE;
    return GTEXT_XML_OK;
  }
  if (!s->final && !source_complete && !s->stopped
      && p->err.code == GTEXT_XML_E_UNEXPECTED_EOF) {
    p->pos = pos;
    p->line = line;
    p->column = column;
    p->cursor_previous = cursor_previous;
    memset(&p->err, 0, sizeof(p->err));
    s->scan.unreliable = 1;
    *done = 0;
    return GTEXT_XML_OK;
  }
  return status;
}

/* The common tail of every construct: begin its scan, and if it is not complete
   yet, say so. */
static GTEXT_XML_Status construct(GTEXT_XML_Stream * s, scan_kind kind,
    lexer_fn fn, int * progress) {
  const int source_complete = s->p.source_count > 1;
  scan_begin(s, kind);
  if (!s->final && !s->eager && !scan_ready(s) && !source_complete) {
    return GTEXT_XML_OK;
  }
  int done = 0;
  const GTEXT_XML_Status status = attempt(s, fn, &done, source_complete);
  *progress = done;
  return status;
}

/* ---- The document, one step at a time ----------------------------------- */

static GTEXT_XML_Status emit_default_document_start(GTEXT_XML_Stream * s) {
  xml_ctx * p = &s->p;
  const char * name = xml_family_name(p->enc_family);
  return xml_emit_document_start(p, NULL, name, strlen(name),
      p->enc_bom ? GTEXT_MARKUP_ENCODING_BOM : GTEXT_MARKUP_ENCODING_DEFAULT,
      GTEXT_MARKUP_STANDALONE_UNSPECIFIED);
}

static GTEXT_XML_Status step_declaration(GTEXT_XML_Stream * s, int * progress) {
  xml_ctx * p = &s->p;
  xml_probe_xml11_declaration(p);
  const look_result m = look(s, "<?xml");
  if (m == LOOK_MORE) {
    return GTEXT_XML_OK;
  }
  int declared = 0;
  if (m == LOOK_MATCH) {
    if (!s->final && available(s) >= 6
        && !xml_space_bytes(p, p->pos + 5)
        && (xml_xml11_line_end_bytes(p, p->pos + 5) > 0
            || xml_at_end(p, p->pos + 5))
        && p->version != XML_VERSION_11) {
      return GTEXT_XML_OK;
    }
    if (available(s) < 6) {
      if (!s->final) {
        return GTEXT_XML_OK;
      }
    }
    else {
      declared = xml_space_bytes(p, p->pos + 5) > 0;
    }
  }
  if (declared) {
    scan_begin(s, SC_DECL);
    if (!s->final && !s->eager && !scan_ready(s)) {
      return GTEXT_XML_OK;
    }
    int done = 0;
    const GTEXT_XML_Status status = attempt(s, run_declaration, &done, 0);
    if (status != GTEXT_XML_OK || !done) {
      return status;
    }
  }
  else {
    const GTEXT_XML_Status status = emit_default_document_start(s);
    if (status != GTEXT_XML_OK) {
      return status;
    }
  }
  s->state = ST_PROLOG;
  *progress = 1;
  return GTEXT_XML_OK;
}

/* Comments, processing instructions, the document type and white space, outside
   the root element; and the root element's start tag. */
static GTEXT_XML_Status step_misc(GTEXT_XML_Stream * s, int * progress) {
  xml_ctx * p = &s->p;
  if (s->doctype_active) {
    const GTEXT_XML_Status status =
        construct(s, SC_DOCTYPE, run_doctype, progress);
    if (*progress) {
      s->doctype_active = 0;
      s->seen_doctype = 1;
    }
    return status;
  }
  const int prolog = s->state == ST_PROLOG;
  xml_skip_space(p);
  if (available(s) == 0) {
    if (!s->final) {
      return GTEXT_XML_OK;
    }
    if (prolog) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
          "the document has no root element");
    }
    const GTEXT_XML_Status validation = xml_validation_finish(p);
    if (validation != GTEXT_XML_OK) return validation;
    s->state = ST_DONE;
    return GTEXT_XML_OK;
  }
  if (p->in[p->pos] == '<') {
    const look_result comment = look(s, "<!--");
    const look_result pi = look(s, "<?");
    const look_result doctype = look(s, "<!DOCTYPE");
    if (comment == LOOK_MATCH) {
      return construct(s, SC_COMMENT, run_comment, progress);
    }
    if (pi == LOOK_MATCH) {
      return construct(s, SC_PI, run_pi, progress);
    }
    if (doctype == LOOK_MATCH) {
      if (!prolog || s->seen_doctype) {
        return xml_fail(p, GTEXT_XML_E_DOCTYPE,
            s->seen_doctype ? "a second document type declaration"
                            : "the document type declaration must come before the root element");
      }
      const GTEXT_XML_Status status =
          construct(s, SC_DOCTYPE, run_doctype, progress);
      if (*progress) {
        s->seen_doctype = 1;
      }
      else if (p->doctype_continuation.active) {
        s->doctype_active = 1;
      }
      return status;
    }
    if (comment == LOOK_MORE || pi == LOOK_MORE || doctype == LOOK_MORE) {
      return GTEXT_XML_OK;
    }
    if (!prolog) {
      return xml_unexpected(p, "content is not allowed after the root element");
    }
    if (available(s) < 2 && !s->final) {
      return GTEXT_XML_OK;
    }
    if (look(s, "</") == LOOK_MATCH || look(s, "<!") == LOOK_MATCH) {
      return xml_unexpected(p, "the root element was expected");
    }
    int started = 0;
    const GTEXT_XML_Status status =
        construct(s, SC_START_TAG, run_start_tag, &started);
    if (started) {
      s->state = p->frame_count > 0 ? ST_CONTENT : ST_EPILOG;
      *progress = 1;
    }
    return status;
  }
  /* Something that is not markup. If it is the start of a character that has
     not all arrived, the character is what it is when it has. */
  if (!s->final
      && utf8_cut_start(p->in, p->pos, p->length) == p->pos) {
    return GTEXT_XML_OK;
  }
  return xml_unexpected(p,
      prolog ? "the root element was expected"
             : "content is not allowed after the root element");
}

/* How far character data at the cursor can be taken now: not into a character
   that has not all arrived, not past a carriage return that the next piece may
   follow with a line feed, and not past a ']' or two that it may follow with the
   '>' that ends a CDATA section wrongly. */
static size_t text_safe_end(const GTEXT_XML_Stream * s) {
  const xml_ctx * p = &s->p;
  size_t end = p->length;
  if (s->final) {
    return end;
  }
  end = utf8_cut_start(p->in, p->pos, end);
  if (end > p->pos && p->in[end - 1] == '\r') {
    end--;
  }
  else {
    size_t held = 0;
    while (held < 2 && end - held > p->pos && p->in[end - 1 - held] == ']') {
      held++;
    }
    end -= held;
  }
  return end;
}

static GTEXT_XML_Status step_content(GTEXT_XML_Stream * s, int * progress) {
  xml_ctx * p = &s->p;
  if (available(s) == 0) {
    if (p->source_count > 1) {
      GTEXT_XML_Status status = xml_flush_text(p);
      if (status == GTEXT_XML_OK) {
        status = xml_source_pop(p);
      }
      if (status != GTEXT_XML_OK) {
        return status;
      }
      refresh(s);
      s->scan.kind = SC_NONE;
      *progress = 1;
      return GTEXT_XML_OK;
    }
    if (!s->final) {
      return GTEXT_XML_OK;
    }
    return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
        "the input ended inside an element");
  }
  const unsigned char c = p->in[p->pos];
  if (c == '<') {
    GTEXT_XML_Status status = xml_flush_text(p);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    if (look(s, "</") == LOOK_MATCH) {
      int done = 0;
      status = construct(s, SC_END_TAG, run_end_tag, &done);
      if (done) {
        *progress = 1;
        if (p->frame_count == 0) {
          s->state = ST_EPILOG;
        }
      }
      return status;
    }
    const look_result comment = look(s, "<!--");
    const look_result cdata = look(s, "<![CDATA[");
    if (comment == LOOK_MORE || cdata == LOOK_MORE) {
      return GTEXT_XML_OK;
    }
    if (comment == LOOK_MATCH) {
      return construct(s, SC_COMMENT, run_comment, progress);
    }
    if (cdata == LOOK_MATCH) {
      return construct(s, SC_CDATA, run_cdata, progress);
    }
    const look_result pi = look(s, "<?");
    const look_result bang = look(s, "<!");
    if (pi == LOOK_MATCH) {
      return construct(s, SC_PI, run_pi, progress);
    }
    if (bang == LOOK_MATCH) {
      return xml_fail(p, GTEXT_XML_E_SYNTAX, "not a comment or a CDATA section");
    }
    if (pi == LOOK_MORE || bang == LOOK_MORE || look(s, "</") == LOOK_MORE) {
      return GTEXT_XML_OK;
    }
    return construct(s, SC_START_TAG, run_start_tag, progress);
  }
  if (c == '&') {
    return construct(s, SC_REF, run_reference, progress);
  }
  const size_t full = p->length;
  const size_t safe = text_safe_end(s);
  if (safe <= p->pos) {
    return GTEXT_XML_OK;
  }
  size_t end = p->pos;
  /* Up to `safe` is all there is, as far as this scan can tell: what could have
     been cut short is outside it, so a character that is cut short at its end
     is followed by something that is not its continuation, and is wrong. */
  p->length = safe;
  p->partial = 0;
  const GTEXT_XML_Status status = xml_scan_text_run(p, &end);
  p->partial = !s->final;
  p->length = full;
  if (status != GTEXT_XML_OK) {
    return status;
  }
  if (p->text.length == 0) {
    p->text_where = xml_here(p);
  }
  GTEXT_XML_Status appended = xml_append_normalized(p, &p->text, p->pos, end);
  if (appended != GTEXT_XML_OK) {
    return appended;
  }
  XML_CONSUME_OR_RETURN(p, end - p->pos);
  *progress = 1;
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status step(GTEXT_XML_Stream * s, int * progress) {
  *progress = 0;
  s->p.partial = !s->final;
  switch (s->state) {
    case ST_DETECT:
      if (!s->detected) {
        return GTEXT_XML_OK;
      }
      s->state = ST_DECL;
      *progress = 1;
      return GTEXT_XML_OK;
    case ST_DECL:
      return step_declaration(s, progress);
    case ST_PROLOG:
    case ST_EPILOG:
      return step_misc(s, progress);
    case ST_CONTENT:
      return step_content(s, progress);
    case ST_DONE:
      return GTEXT_XML_OK;
  }
  return GTEXT_XML_OK;
}

/* Read everything the buffer completes. */
static GTEXT_XML_Status drive(GTEXT_XML_Stream * s) {
  if (s->paused) {
    return GTEXT_XML_OK;
  }
  for (;;) {
    if (s->p.err.code != GTEXT_XML_OK) {
      return s->p.err.code;
    }
    if (s->pause_requested) {
      s->pause_requested = 0;
      s->paused = 1;
      return GTEXT_XML_OK;
    }
    int progress = 0;
    const GTEXT_XML_Status status = step(s, &progress);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    if (s->p.err.code != GTEXT_XML_OK) {
      return s->p.err.code;
    }
    /* An emitting step can finish without consuming parser input (for example,
       flushing pending text). Honor the queue pause before the no-progress
       return so the next feed/resume cannot run ahead. */
    if (s->pause_requested) {
      s->pause_requested = 0;
      s->paused = 1;
      return GTEXT_XML_OK;
    }
    if (!progress) {
      return GTEXT_XML_OK;
    }
  }
}

/* Drop what has been read, keeping the byte before the cursor. Done only when
   at least as much has been read as is left, so the copying is paid for. */
static void compact(GTEXT_XML_Stream * s) {
  xml_ctx * p = &s->p;
  /* p->pos is relative to the active physical source, while `input` is the
     document buffer. Only compact it when the document source is active. */
  if (p->source_count > 1) {
    return;
  }
  if (p->pos < 4096 || p->pos < p->length - p->pos) {
    return;
  }
  const size_t drop = p->pos - 1;
  memmove(s->input.data, s->input.data + drop, s->input.length - drop);
  s->input.length -= drop;
  s->input.data[s->input.length] = '\0';
  p->pos -= drop;
  p->base += drop;
  refresh(s);
}

/* ---- Getting the bytes in ----------------------------------------------- */

static GTEXT_XML_Status append_input(
    GTEXT_XML_Stream * s, const void * data, size_t len) {
  return xml_append(&s->p, &s->input, (const char *)data, len);
}

/* The encoding is decided from the bytes held, and they are converted. */
static GTEXT_XML_Status detect(GTEXT_XML_Stream * s) {
  xml_ctx * p = &s->p;
  xml_family family = XML_FAMILY_UTF8;
  size_t skip = 0;
  int bom = 0;
  if (xml_sniff((const unsigned char *)s->input.data, s->input.length, &family,
          &skip, &bom)) {
    return xml_fail(p, GTEXT_XML_E_UNSUPPORTED_ENCODING,
        "EBCDIC is not supported");
  }
  p->enc_bom = bom;
  p->enc_family = family;
  s->detected = 1;
  if (family == XML_FAMILY_UTF8) {
    p->pos = skip;
    refresh(s);
    return GTEXT_XML_OK;
  }
  /* Convert what was held, from the first byte after the mark. */
  markup_buf raw = s->input;
  markup_buf_init(&s->input, &s->allocator);
  xml_converter_init(&s->converter, p, &s->input, family);
  s->converter.raw_total = skip;
  s->converting = 1;
  const GTEXT_XML_Status status = xml_converter_feed(&s->converter,
      (const unsigned char *)raw.data + skip, raw.length - skip, 0);
  markup_buf_free(&raw);
  refresh(s);
  return status;
}

static GTEXT_XML_Status ingest(
    GTEXT_XML_Stream * s, const void * data, size_t len) {
  if (s->detected) {
    if (s->converting) {
      const GTEXT_XML_Status status = xml_converter_feed(
          &s->converter, (const unsigned char *)data, len, 0);
      refresh(s);
      return status;
    }
    const GTEXT_XML_Status status = append_input(s, data, len);
    refresh(s);
    return status;
  }
  GTEXT_XML_Status status = append_input(s, data, len);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  for (; s->detect_scanned < s->input.length; s->detect_scanned++) {
    s->work++;
    if (s->input.data[s->detect_scanned] == '>') {
      s->has_close = 1;
    }
  }
  /* Whether to wait depends only on the first bytes and on whether a '>' has
     come, so only the first few are looked at: a declaration that is still
     arriving is not rescanned from its start on every feed. */
  const size_t head = s->input.length < 8 ? s->input.length : 8;
  s->work += head;
  if (xml_sniff_needs_more((const unsigned char *)s->input.data, head,
          &s->has_close)) {
    if (s->input.length > XML_DECLARATION_WAIT) {
      return xml_fail(&s->p, GTEXT_XML_E_SYNTAX,
          "the XML declaration is too long (more than 64 KiB without its end)");
    }
    refresh(s);
    return GTEXT_XML_OK;
  }
  return detect(s);
}

/* ---- Events ------------------------------------------------------------- */

static GTEXT_XML_Status deliver(xml_ctx * p, const GTEXT_XML_Event * event) {
  GTEXT_XML_Stream * s = p->stream;
  const GTEXT_XML_Status status = s->callback(event, s->user);
  if (status != GTEXT_XML_OK) {
    s->stopped = 1;
    return xml_fail_at(p, status, "stopped by the event callback", &event->location);
  }
  return GTEXT_XML_OK;
}

/* A node the DOM parse would have counted against max_nodes. */
static GTEXT_XML_Status count_node(xml_ctx * p) {
  GTEXT_XML_Stream * s = p->stream;
  if (s->max_nodes != 0 && s->nodes >= s->max_nodes) {
    return xml_fail(p, GTEXT_XML_E_LIMIT_NODES, "too many nodes");
  }
  s->nodes++;
  return GTEXT_XML_OK;
}

static void event_init(xml_ctx * p, GTEXT_XML_Event * e, GTEXT_XML_Event_Type type,
    const GTEXT_Markup_Location * at) {
  memset(e, 0, sizeof(*e));
  e->type = type;
  e->location = *at;
  if (at->source_id != 0 && at->source_id <= p->source_name_count) {
    e->source_name = p->source_names[at->source_id - 1];
  }
  const size_t retained = p->source_count > 32 ? 32 : p->source_count;
  const size_t first = p->source_count - retained;
  e->reference_trail_count = (uint8_t)retained;
  e->reference_trail_truncated = first != 0;
  for (size_t i = 0; i < retained; i++) {
    e->reference_trail[i] = p->sources[first + i].source_id;
  }
}

static GTEXT_XML_Status emit_validation_result(GTEXT_XML_Stream *s) {
  if (s->p.dtd_mode != GTEXT_XML_DTD_VALIDATE || s->validation_result_sent)
    return GTEXT_XML_OK;
  GTEXT_XML_Event event;
  const GTEXT_Markup_Location here = xml_here(&s->p);
  event_init(&s->p, &event, GTEXT_XML_EVENT_VALIDATION_RESULT, &here);
  xml_validation_result_from_context(&s->p, &event.validation_result);
  GTEXT_XML_Status status = deliver(&s->p, &event);
  if (status == GTEXT_XML_OK) s->validation_result_sent = 1;
  return status;
}

/* A NUL-terminated copy in the arena; its offset. */
static GTEXT_XML_Status arena_put(xml_ctx * p, const char * bytes, size_t length,
    size_t * offset) {
  markup_buf * a = &p->stream->arena;
  *offset = a->length;
  GTEXT_XML_Status status = xml_append(p, a, bytes, length);
  if (status == GTEXT_XML_OK) {
    status = xml_append(p, a, "", 1);
  }
  return status;
}

GTEXT_XML_Status xml_emit_document_start(xml_ctx * p, const char * version,
    const char * encoding, size_t encoding_length,
    GTEXT_Markup_Encoding_Source source, GTEXT_Markup_Standalone standalone) {
  GTEXT_XML_Stream * s = p->stream;
  s->arena.length = 0;
  size_t at = 0;
  GTEXT_XML_Status status = arena_put(p, encoding, encoding_length, &at);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  const GTEXT_Markup_Location origin = {0, 1, 1, 1, 0, 0, false};
  GTEXT_XML_Event e;
  event_init(p, &e, GTEXT_XML_EVENT_DOCUMENT_START, &origin);
  e.version = version;
  e.encoding = s->arena.data + at;
  e.encoding_source = source;
  e.standalone = standalone;
  return deliver(p, &e);
}

GTEXT_XML_Status xml_emit_doctype(xml_ctx * p, const GTEXT_Markup_Location * at,
    const char * name, const char * public_id, const char * system_id) {
  GTEXT_XML_Status status = count_node(p);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  GTEXT_XML_Event e;
  event_init(p, &e, GTEXT_XML_EVENT_DOCTYPE, at);
  e.name = name;
  e.public_id = public_id;
  e.system_id = system_id;
  return deliver(p, &e);
}

/* The element's names, copied into the arena as NUL-terminated strings. */
static GTEXT_XML_Status arena_name(xml_ctx * p, const unsigned char * name,
    size_t length, size_t colon, size_t * local, size_t * prefix) {
  const size_t local_at = colon == SIZE_MAX ? 0 : colon + 1;
  GTEXT_XML_Status status = arena_put(
      p, (const char *)name + local_at, length - local_at, local);
  *prefix = SIZE_MAX;
  if (status == GTEXT_XML_OK && colon != SIZE_MAX) {
    status = arena_put(p, (const char *)name, colon, prefix);
  }
  return status;
}

GTEXT_XML_Status xml_emit_element_start(xml_ctx * p,
    const GTEXT_Markup_Location * at, const unsigned char * name,
    size_t name_length, size_t colon, const char * element_ns) {
  GTEXT_XML_Stream * s = p->stream;
  GTEXT_XML_Status status = count_node(p);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  s->arena.length = 0;
  size_t local = 0;
  size_t prefix = SIZE_MAX;
  status = arena_name(p, name, name_length, colon, &local, &prefix);
  if (status == GTEXT_XML_OK) {
    status = xml_grow(p, (void **)&s->offsets, &s->offsets_capacity,
        sizeof(attr_offsets), p->attr_count + 1);
  }
  if (status == GTEXT_XML_OK) {
    status = xml_grow(p, (void **)&s->attributes, &s->attributes_capacity,
        sizeof(GTEXT_XML_Event_Attribute), p->attr_count + 1);
  }
  const char * pool = p->attr_pool.data ? p->attr_pool.data : "";
  for (size_t i = 0; status == GTEXT_XML_OK && i < p->attr_count; i++) {
    const xml_attr * a = &p->attrs[i];
    attr_offsets * o = &s->offsets[i];
    o->prefix = SIZE_MAX;
    if (a->kind == XML_ATTR_DEFAULT_NS_DECL) {
      status = arena_put(p, "xmlns", 5, &o->local);
    }
    else if (a->kind == XML_ATTR_PREFIX_DECL) {
      status = arena_put(p, (const char *)a->name_ptr + 6, a->name_length - 6,
          &o->local);
      if (status == GTEXT_XML_OK) {
        status = arena_put(p, "xmlns", 5, &o->prefix);
      }
    }
    else {
      status = arena_name(
          p, a->name_ptr, a->name_length, a->colon, &o->local, &o->prefix);
    }
    if (status == GTEXT_XML_OK) {
      status = arena_put(p, pool + a->value_offset, a->value_length, &o->value);
    }
  }
  if (status != GTEXT_XML_OK) {
    return status;
  }
  /* The arena has stopped growing: its addresses are final. */
  const char * base = s->arena.data;
  for (size_t i = 0; i < p->attr_count; i++) {
    const xml_attr * a = &p->attrs[i];
    const attr_offsets * o = &s->offsets[i];
    GTEXT_XML_Event_Attribute * out = &s->attributes[i];
    out->namespace_uri = a->ns;
    out->prefix = o->prefix == SIZE_MAX ? NULL : base + o->prefix;
    out->local_name = base + o->local;
    out->value = base + o->value;
    out->value_length = a->value_length;
    out->location = a->where;
    out->origin = a->defaulted ? GTEXT_MARKUP_ATTRIBUTE_DEFAULTED
                               : GTEXT_MARKUP_ATTRIBUTE_EXPLICIT;
  }
  GTEXT_XML_Event e;
  event_init(p, &e, GTEXT_XML_EVENT_ELEMENT_START, at);
  e.namespace_uri = element_ns;
  e.prefix = prefix == SIZE_MAX ? NULL : base + prefix;
  e.local_name = base + local;
  e.attributes = p->attr_count ? s->attributes : NULL;
  e.attribute_count = p->attr_count;
  return deliver(p, &e);
}

GTEXT_XML_Status xml_emit_element_end(xml_ctx * p,
    const GTEXT_Markup_Location * at, const unsigned char * name,
    size_t name_length) {
  GTEXT_XML_Stream * s = p->stream;
  const unsigned char * colon_at = memchr(name, ':', name_length);
  const size_t colon = colon_at ? (size_t)(colon_at - name) : SIZE_MAX;
  const char * ns = NULL;
  if (!xml_scope_lookup(p, colon == SIZE_MAX ? "" : (const char *)name,
          colon == SIZE_MAX ? 0 : colon, &ns)) {
    ns = NULL; /* not reachable: the start tag resolved this prefix */
  }
  s->arena.length = 0;
  size_t local = 0;
  size_t prefix = SIZE_MAX;
  const GTEXT_XML_Status status =
      arena_name(p, name, name_length, colon, &local, &prefix);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  GTEXT_XML_Event e;
  event_init(p, &e, GTEXT_XML_EVENT_ELEMENT_END, at);
  e.namespace_uri = ns;
  e.prefix = prefix == SIZE_MAX ? NULL : s->arena.data + prefix;
  e.local_name = s->arena.data + local;
  return deliver(p, &e);
}

static GTEXT_XML_Status emit_data(xml_ctx * p, GTEXT_XML_Event_Type type,
    const GTEXT_Markup_Location * at, const char * data, size_t length) {
  const GTEXT_XML_Status status = count_node(p);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  GTEXT_XML_Event e;
  event_init(p, &e, type, at);
  e.data = data;
  e.data_length = length;
  return deliver(p, &e);
}

GTEXT_XML_Status xml_emit_text(xml_ctx * p, const GTEXT_Markup_Location * at,
    const char * data, size_t length) {
  return emit_data(p, GTEXT_XML_EVENT_TEXT, at, data, length);
}

GTEXT_XML_Status xml_emit_cdata(xml_ctx * p, const GTEXT_Markup_Location * at,
    const char * data, size_t length) {
  return emit_data(p, GTEXT_XML_EVENT_CDATA, at, data, length);
}

GTEXT_XML_Status xml_emit_comment(xml_ctx * p, const GTEXT_Markup_Location * at,
    const char * data, size_t length) {
  return emit_data(p, GTEXT_XML_EVENT_COMMENT, at, data, length);
}

GTEXT_XML_Status xml_emit_pi(xml_ctx * p, const GTEXT_Markup_Location * at,
    const char * target, size_t target_length, const char * data, size_t length) {
  GTEXT_XML_Stream * s = p->stream;
  GTEXT_XML_Status status = count_node(p);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  s->arena.length = 0;
  size_t name = 0;
  status = arena_put(p, target, target_length, &name);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  GTEXT_XML_Event e;
  event_init(p, &e, GTEXT_XML_EVENT_PROCESSING_INSTRUCTION, at);
  e.name = s->arena.data + name;
  e.data = data;
  e.data_length = length;
  return deliver(p, &e);
}

GTEXT_XML_Status xml_emit_skipped_entity(xml_ctx * p,
    const GTEXT_Markup_Location * at, const char * name, size_t length) {
  GTEXT_XML_Stream * s = p->stream;
  GTEXT_XML_Status status = count_node(p);
  if (status != GTEXT_XML_OK) return status;
  s->arena.length = 0;
  size_t name_at = 0;
  status = arena_put(p, name, length, &name_at);
  if (status != GTEXT_XML_OK) return status;
  GTEXT_XML_Event e;
  event_init(p, &e, GTEXT_XML_EVENT_SKIPPED_ENTITY, at);
  e.name = s->arena.data + name_at;
  return deliver(p, &e);
}

GTEXT_XML_Status xml_emit_unread_resource(xml_ctx * p,
    const GTEXT_Markup_Location * at, GTEXT_XML_Resource_Kind kind,
    const char * name, const char * system_id) {
  GTEXT_XML_Stream * s = p->stream;
  s->arena.length = 0;
  size_t name_at = SIZE_MAX;
  size_t system_at = SIZE_MAX;
  GTEXT_XML_Status status = name
      ? arena_put(p, name, strlen(name), &name_at) : GTEXT_XML_OK;
  if (status == GTEXT_XML_OK && system_id)
    status = arena_put(p, system_id, strlen(system_id), &system_at);
  if (status != GTEXT_XML_OK) return status;
  GTEXT_XML_Event e;
  event_init(p, &e, GTEXT_XML_EVENT_UNREAD_RESOURCE, at);
  e.resource_kind = kind;
  e.name = name_at == SIZE_MAX ? NULL : s->arena.data + name_at;
  e.system_id = system_at == SIZE_MAX ? NULL : s->arena.data + system_at;
  return deliver(p, &e);
}

GTEXT_XML_Status xml_emit_validation_diagnostic(xml_ctx *p,
    GTEXT_XML_Validation_Code code, const GTEXT_Markup_Location *where,
    const char *message) {
  GTEXT_XML_Event event;
  event_init(p, &event, GTEXT_XML_EVENT_VALIDATION_DIAGNOSTIC, where);
  event.validation_diagnostic = (GTEXT_XML_Validation_Diagnostic){
      code, where ? *where : (GTEXT_Markup_Location){0}, message};
  return deliver(p, &event);
}

GTEXT_XML_Status xml_emit_validation_diagnostics_truncated(xml_ctx *p,
    const GTEXT_Markup_Location *where) {
  GTEXT_XML_Event event;
  event_init(p, &event, GTEXT_XML_EVENT_VALIDATION_DIAGNOSTICS_TRUNCATED,
      where);
  return deliver(p, &event);
}

/* ---- The public interface ----------------------------------------------- */

size_t xml_stream_work(const struct GTEXT_XML_Stream * stream) {
  return stream->work;
}

void xml_stream_set_eager(GTEXT_XML_Stream * stream, int eager) {
  stream->eager = eager;
}

GTEXT_INTERNAL_API int xml_stream_pause_pending(
    const GTEXT_XML_Stream * stream) {
  return stream && stream->pause_requested;
}

static size_t limit_or(size_t given, size_t fallback) {
  return given != 0 ? given : fallback;
}

GTEXT_API GTEXT_XML_Stream * gtext_xml_stream_new(
    const GTEXT_XML_Parse_Options * opts, GTEXT_XML_Event_cb callback,
    void * user_data) {
  return xml_stream_new_with_seams(opts, callback, user_data, NULL);
}

GTEXT_INTERNAL_API GTEXT_XML_Stream * xml_stream_new_with_seams(
    const GTEXT_XML_Parse_Options * opts, GTEXT_XML_Event_cb callback,
    void * user_data, const xml_seams * seams) {
  if (!callback) {
    return NULL;
  }
  const GTEXT_XML_Parse_Options use =
      opts ? *opts : gtext_xml_parse_options_default();
  if (xml_validate_dtd_options(&use, NULL) != GTEXT_XML_OK) {
    return NULL;
  }
  const GTEXT_Allocator * allocator =
      use.allocator ? use.allocator : gtext_allocator_default();
  GTEXT_XML_Stream * s = gtext_allocator_calloc(allocator, 1, sizeof(*s));
  if (!s) {
    return NULL;
  }
  s->allocator = *allocator;
  s->callback = callback;
  s->user = user_data;
  s->max_input = limit_or(use.max_input_bytes, XML_DEFAULT_MAX_INPUT);

  xml_ctx * p = &s->p;
  p->in = kEmpty;
  p->line = 1;
  p->column = 1;
  p->allocator = &s->allocator;
  p->stream = s;
  p->max_depth = limit_or(use.max_depth, XML_DEFAULT_MAX_DEPTH);
  p->max_attributes = limit_or(use.max_attributes, XML_DEFAULT_MAX_ATTRIBUTES);
  if (p->max_attributes > UINT32_MAX) {
    p->max_attributes = UINT32_MAX; /* Attribute indexes are 32 bits. */
  }
  p->max_attribute_bytes =
      limit_or(use.max_attribute_bytes, XML_DEFAULT_MAX_ATTRIBUTE_BYTES);
  p->max_name_bytes = limit_or(use.max_name_bytes, XML_DEFAULT_MAX_NAME_BYTES);
  p->max_namespaces = limit_or(use.max_namespaces, XML_DEFAULT_MAX_NAMESPACES);
  p->max_external_source_bytes = limit_or(use.max_external_source_bytes,
      XML_DEFAULT_MAX_EXTERNAL_SOURCE_BYTES);
  p->max_external_bytes = limit_or(use.max_external_bytes,
      XML_DEFAULT_MAX_EXTERNAL_BYTES);
  p->max_expanded_bytes = limit_or(use.max_expanded_bytes,
      XML_DEFAULT_MAX_EXPANDED_BYTES);
  p->max_amplification = limit_or(use.max_amplification,
      XML_DEFAULT_MAX_AMPLIFICATION);
  p->amplification_threshold = limit_or(use.amplification_threshold,
      XML_DEFAULT_AMPLIFICATION_THRESHOLD);
  p->max_entity_depth = limit_or(use.max_entity_depth,
      XML_DEFAULT_MAX_ENTITY_DEPTH);
  p->max_references = limit_or(use.max_references, XML_DEFAULT_MAX_REFERENCES);
  p->max_declarations = limit_or(use.max_declarations,
      XML_DEFAULT_MAX_DECLARATIONS);
  p->max_total_work = limit_or(use.max_total_work, XML_DEFAULT_MAX_TOTAL_WORK);
  p->max_validation_work = limit_or(use.max_validation_work,
      XML_DEFAULT_MAX_VALIDATION_WORK);
  p->max_validation_memory = limit_or(use.max_validation_memory,
      XML_DEFAULT_MAX_VALIDATION_MEMORY);
  p->max_validation_diagnostics = limit_or(use.max_validation_diagnostics,
      XML_DEFAULT_MAX_VALIDATION_DIAGNOSTICS);
  /* Document source + the configured entity nesting + one external subset. */
  p->max_source_depth = p->max_entity_depth >= SIZE_MAX - 2
      ? SIZE_MAX : p->max_entity_depth + 2;
  p->dtd_mode = use.dtd_mode;
  p->dtd_external_policy = use.dtd_external_policy;
  p->external_access = use.external_access;
  p->resolver = use.resolver;
  p->resolver_user_data = use.resolver_user_data;
  p->base_uri = use.base_uri;
  if (seams) {
    p->seams = *seams;
  }
  const size_t max_nodes = limit_or(use.max_nodes, XML_DEFAULT_MAX_NODES);
  s->max_nodes = max_nodes == SIZE_MAX ? 0 : max_nodes;
  s->nodes = 1; /* the document node */

  markup_buf_init(&s->input, &s->allocator);
  markup_buf_init(&s->arena, &s->allocator);
  markup_buf_init(&p->text, &s->allocator);
  markup_buf_init(&p->attr_pool, &s->allocator);
  markup_buf_init(&p->scratch, &s->allocator);
  markup_buf_init(&p->names, &s->allocator);
  if (xml_source_init(p, p->in) != GTEXT_XML_OK) {
    gtext_xml_stream_free(s);
    return NULL;
  }
  return s;
}

GTEXT_API void gtext_xml_stream_free(GTEXT_XML_Stream * s) {
  if (!s) {
    return;
  }
  xml_ctx * p = &s->p;
  if (p->resolver_callback_active) {
    p->resolver_reentered = 1;
    return;
  }
  markup_buf_free(&s->input);
  markup_buf_free(&s->arena);
  xml_doctype_continuation_reset(p);
  markup_buf_free(&p->text);
  markup_buf_free(&p->attr_pool);
  markup_buf_free(&p->scratch);
  markup_buf_free(&p->names);
  xml_release(p, p->attrs);
  xml_release(p, p->frames);
  xml_release(p, s->offsets);
  xml_release(p, s->attributes);
  xml_hset_free(p, &p->raw_names);
  xml_hset_free(p, &p->expanded_names);
  xml_scope_free(p);
  xml_source_free(p);
  xml_validation_cleanup(p);
  GTEXT_Allocator allocator = s->allocator;
  gtext_allocator_free(&allocator, s);
}

/* Leave `err` describing how a call ended. */
static GTEXT_XML_Status conclude(
    GTEXT_XML_Stream * s, GTEXT_XML_Status status, GTEXT_XML_Error * err) {
  if (status != GTEXT_XML_OK) {
    if (s->p.err.code == GTEXT_XML_OK) {
      s->p.err.code = status;
      s->p.err.message = gtext_xml_status_string(status);
    }
    s->failed = 1;
  }
  if (err) {
    if (s->failed) {
      *err = s->p.err;
    }
    else {
      memset(err, 0, sizeof(*err));
    }
  }
  return s->failed ? s->p.err.code : GTEXT_XML_OK;
}

/* A call that does not fit the state; nothing about the document. */
static GTEXT_XML_Status misuse(GTEXT_XML_Stream * s, GTEXT_XML_Status code,
    const char * message, GTEXT_XML_Error * err) {
  if (err) {
    memset(err, 0, sizeof(*err));
    err->code = code;
    err->message = message;
    err->byte_offset = s ? s->p.base + s->p.pos : 0;
    err->line = s ? s->p.line : 1;
    err->column = s ? s->p.column : 1;
  }
  return code;
}

GTEXT_API GTEXT_XML_Status gtext_xml_stream_feed(GTEXT_XML_Stream * s,
    const void * data, size_t len, GTEXT_XML_Error * err) {
  if (!s) {
    return misuse(NULL, GTEXT_XML_E_INVALID, "invalid argument", err);
  }
  if (s->p.resolver_callback_active) {
    s->p.resolver_reentered = 1;
    return misuse(s, GTEXT_XML_E_STATE,
        "resolver callback reentered the active XML parser", err);
  }
  if (s->failed) {
    return conclude(s, GTEXT_XML_OK, err);
  }
  if (!data && len != 0) {
    return misuse(s, GTEXT_XML_E_INVALID, "invalid argument", err);
  }
  if (len == 0) {
    return conclude(s, GTEXT_XML_OK, err);
  }
  if (s->final) {
    return misuse(s, GTEXT_XML_E_STATE, "input after the end of input", err);
  }
  xml_ctx * p = &s->p;
  if (len > s->max_input - s->fed) {
    /* As for the DOM parse, which refuses an over-long input before reading it:
       the refusal is of the document, so it is at the start. */
    const GTEXT_Markup_Location origin = {0, 1, 1, 1, 0, 0, false};
    return conclude(s,
        xml_fail_at(p, GTEXT_XML_E_LIMIT_INPUT, "input too long", &origin), err);
  }
  s->fed += len;
  p->input_bytes = s->fed;
  GTEXT_XML_Status status = ingest(s, data, len);
  if (status == GTEXT_XML_OK) {
    status = drive(s);
  }
  if (status == GTEXT_XML_OK) {
    compact(s);
  }
  return conclude(s, status, err);
}

GTEXT_API GTEXT_XML_Status gtext_xml_stream_finish(
    GTEXT_XML_Stream * s, GTEXT_XML_Error * err) {
  if (!s) {
    return misuse(NULL, GTEXT_XML_E_INVALID, "invalid argument", err);
  }
  if (s->p.resolver_callback_active) {
    s->p.resolver_reentered = 1;
    return misuse(s, GTEXT_XML_E_STATE,
        "resolver callback reentered the active XML parser", err);
  }
  if (s->failed || s->finished) {
    return conclude(s, GTEXT_XML_OK, err);
  }
  xml_ctx * p = &s->p;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  if (!s->final_prepared) {
    s->final = 1;
    if (!s->detected) {
      status = detect(s);
    }
    if (status == GTEXT_XML_OK && s->converting) {
      status = xml_converter_feed(&s->converter, NULL, 0, 1);
      refresh(s);
    }
    if (status == GTEXT_XML_OK) {
      s->final_prepared = 1;
    }
  }
  if (status == GTEXT_XML_OK) {
    status = drive(s);
  }
  if (status == GTEXT_XML_OK && !s->paused && s->state != ST_DONE) {
    /* Not reachable: at the end every state either finishes or says why not. */
    status = xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "the document is incomplete");
  }
  if (status == GTEXT_XML_OK && !s->paused && !s->document_end_sent) {
    status = emit_validation_result(s);
  }
  if (status == GTEXT_XML_OK && !s->paused && !s->document_end_sent) {
    GTEXT_XML_Event e;
    const GTEXT_Markup_Location here = xml_here(p);
    event_init(p, &e, GTEXT_XML_EVENT_DOCUMENT_END, &here);
    xml_validation_result_from_context(p, &e.validation_result);
    status = deliver(p, &e);
    if (status == GTEXT_XML_OK) {
      s->document_end_sent = 1;
    }
  }
  if (status == GTEXT_XML_OK && s->document_end_sent) {
    s->finished = 1;
  }
  return conclude(s, status, err);
}

GTEXT_XML_Status xml_stream_resume(GTEXT_XML_Stream * s, GTEXT_XML_Error * err) {
  if (!s) {
    return misuse(NULL, GTEXT_XML_E_INVALID, "invalid argument", err);
  }
  if (s->p.resolver_callback_active) {
    s->p.resolver_reentered = 1;
    return misuse(s, GTEXT_XML_E_STATE,
        "resolver callback reentered the active XML parser", err);
  }
  if (s->failed || s->finished) {
    return conclude(s, GTEXT_XML_OK, err);
  }
  s->paused = 0;
  GTEXT_XML_Status status = drive(s);
  if (status == GTEXT_XML_OK) {
    compact(s);
  }
  if (status == GTEXT_XML_OK && s->final && s->final_prepared && !s->paused
      && s->state != ST_DONE) {
    status = xml_fail(&s->p, GTEXT_XML_E_UNEXPECTED_EOF,
        "the document is incomplete");
  }
  if (status == GTEXT_XML_OK && s->final && s->final_prepared && !s->paused
      && !s->document_end_sent) {
    status = emit_validation_result(s);
  }
  if (status == GTEXT_XML_OK && s->final && s->final_prepared && !s->paused
      && !s->document_end_sent) {
    GTEXT_XML_Event e;
    const GTEXT_Markup_Location here = xml_here(&s->p);
    event_init(&s->p, &e, GTEXT_XML_EVENT_DOCUMENT_END, &here);
    xml_validation_result_from_context(&s->p, &e.validation_result);
    status = deliver(&s->p, &e);
    if (status == GTEXT_XML_OK) {
      s->document_end_sent = 1;
      s->finished = 1;
    }
  }
  return conclude(s, status, err);
}
