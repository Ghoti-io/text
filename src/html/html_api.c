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
 * The public HTML parsers: a whole buffer, a fragment, an incremental parser and (in html_file_io.c) a
 * file, over the tree builder of html_tree_internal.h.
 *
 * Every entry point is made of the same pieces, which is how "chunking never changes the answer" holds
 * for the public layer as it does for the builder: gtext_html_parse() is a parser fed once, and the
 * fragment form is the same parser made by the other constructor. What this layer adds to the builder:
 *
 * - **Encodings.** The bytes of a document are not UTF-8 until this layer has made them so. It holds back
 *   the first 1024 bytes (or the whole input, if it is shorter) and runs the standard's encoding sniffing
 *   algorithm over them (html_sniff.c): a byte order mark, the caller's certain label, the `<meta>`
 *   prescan, the caller's guess. UTF-8, the encoding nearly every document has, is then read by the
 *   tokenizer itself, byte for byte, as it has always been; every other encoding goes through the Encoding
 *   module's decoder in its replacement mode, which tells the tokenizer where in the original bytes each
 *   code point began (gtext_decoder_decode_mapped(), gtext_html_tokenizer_feed_mapped()), so that errors and
 *   locations are offsets of the bytes the caller gave. A test forces UTF-8 through that second route too
 *   and compares (html_parser_force_transcode()).
 * - **Change the encoding.** While the encoding is tentative the layer keeps the bytes it was given, until
 *   the tree builder has left the head, so that a `<meta>` which declares another encoding (the builder
 *   fails with GTEXT_HTML_TREE_E_ENCODING) restarts the parse from the first byte in the declared one: a
 *   new builder and decoder, every kept byte again. The restart is invisible except in `reparsed`: it
 *   produces what a parse that began in that encoding produces. A whole-buffer parse keeps nothing, it has
 *   the caller's buffer.
 * - **The input limit**, checked here on everything fed, so a cut input and a whole one are refused by
 *   the same rule whatever the first window holds back, and counted in the bytes the caller gave.
 * - **Failure** that is sticky and a status of this API's own (a tree builder's reprocess failure,
 *   which is a defect, is E_INTERNAL), and a result that is built for success and for failure alike.
 * - The fragment context, taken by value with NUL-terminated strings and given to the builder with
 *   lengths. A fragment is characters: none of the above applies to it.
 */

#include <ghoti.io/text/macros.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "html_api_internal.h"
#include "html_sniff_internal.h"

/// The default of `max_reparse_bytes`.
#define GTEXT_HTML_DEFAULT_MAX_REPARSE_BYTES ((size_t)4 * 1024 * 1024)

/// What the layer is doing with the input.
typedef enum { PH_SNIFF = 0, PH_RUN } Phase;

/// The internal verdict of one pass of bytes through the pipeline: an API status, or that the tree builder changed the encoding.
enum { ST_RESTART = -1 };

/// The incremental parser: the tree builder, a sticky failure, the first bytes of the input, and what is needed to read them in the
/// encoding that is decided.
struct GTEXT_HTML_Parser {
  GTEXT_Allocator allocator; ///< The caller's, copied; the parser and the builder are made with it.
  GTEXT_HTML_Tree * tree;    ///< The tree builder (a fragment builder for a fragment parser). NULL until the encoding is decided.
  GTEXT_HTML_Status status;  ///< The sticky failure; OK if none.
  bool fragment;             ///< Made by gtext_html_parser_new_fragment().
  bool finished;             ///< The builder has finished successfully.
  bool taken;                ///< ... and its document has been handed out.
  size_t max_input;          ///< The input limit.
  size_t fed;                ///< Bytes accepted from the caller.
  GTEXT_HTML_TreeOptions topts; ///< What the builder is made with (the allocator is this parser's), but for the encoding.

  /* The caller's options, resolved. */
  GTEXT_Encoding transport;  ///< `transport_encoding`: the encoding, or NONE (none given, or none recognised).
  bool transport_certain;    ///< `transport_encoding_certainty` is certain.
  bool transport_ignored;    ///< Given and recognised as nothing.
  GTEXT_Encoding fallback;   ///< `default_encoding`: the encoding, or NONE.
  bool fallback_ignored;     ///< `default_encoding` was given and names no encoding.
  bool ignore_meta;          ///< `ignore_meta`: no declaration is read, and no restart happens.
  GTEXT_Markup_Encoding_Source fragment_source; ///< A fragment's: CALLER if a recognised label was given, else DEFAULT.

  /* Reading. */
  Phase phase;               ///< Collecting the first bytes, or running.
  unsigned char win[GTEXT_HTML_PRESCAN_BYTES]; ///< The first bytes, until there are `need` of them or the input ends.
  size_t win_len;            ///< Bytes of `win` in use.
  size_t need;               ///< Bytes the decision reads: 1024 for the prescan; 3 (the longest byte order mark) when nothing is prescanned.
  const unsigned char * whole; ///< The caller's whole input, when it was given in one piece and stays valid (gtext_html_parse()): nothing is copied.
  unsigned char * keep;      ///< Every byte fed before the one being processed, while a restart is possible (not the first window, which `win` has).
  size_t keep_len;           ///< Bytes of `keep` in use.
  size_t keep_cap;           ///< Bytes of `keep` allocated.
  size_t max_reparse;        ///< `max_reparse_bytes`, resolved (at least the prescan's window).
  bool reparse_abandoned;    ///< The bytes for a restart were let go.
  bool keep_active;          ///< A restart is possible: the encoding is tentative and the builder is still in the head.

  /* The decision, and what follows from it. */
  bool decided;              ///< The encoding is decided (the first bytes have been looked at).
  GTEXT_Encoding enc;        ///< The encoding in use.
  GTEXT_Markup_Encoding_Source source; ///< How it was decided.
  GTEXT_HTML_Encoding_Certainty confidence; ///< How sure.
  size_t bom_length;         ///< Bytes of the byte order mark, or 0.
  bool reparsed;             ///< A restart has happened.
  bool force_transcode;      ///< Tests: read UTF-8 through the decoder too.
  bool use_decoder;          ///< The bytes go through `decoder` (and carry a source map) instead of straight to the tokenizer.
  GTEXT_Decoder * decoder;   ///< The decoder of `enc`, when `use_decoder`.
  size_t * flagged;          ///< Scratch: a chunk's source map with the replacement flag in it, the form the tokenizer takes.
  size_t stream_pos;         ///< Bytes of the input the present builder has been given.
  GTEXT_HTML_TreeStatus sink_status; ///< What the builder said inside the decoder's sink, if it said no.
};

/* ---- encoding labels ---------------------------------------------------- */

/// The Encoding standard's labels of UTF-8, trimmed and matched ASCII case-insensitively (declared in html_api_internal.h).
bool html_label_is_utf8(const char * label) {
  if (!label) {
    return false;
  }
  return gtext_encoding_for_label(label, strlen(label)) == GTEXT_ENCODING_UTF_8;
}

/* ---- statuses ----------------------------------------------------------- */

/// The tree builder's status as the public one.
GTEXT_HTML_Status html_status_from_tree(GTEXT_HTML_TreeStatus status) {
  switch (status) {
    case GTEXT_HTML_TREE_OK:
      return GTEXT_HTML_OK;
    case GTEXT_HTML_TREE_E_OOM:
      return GTEXT_HTML_E_OOM;
    case GTEXT_HTML_TREE_E_STATE:
      return GTEXT_HTML_E_STATE;
    case GTEXT_HTML_TREE_E_LIMIT_DEPTH:
      return GTEXT_HTML_E_LIMIT_DEPTH;
    case GTEXT_HTML_TREE_E_LIMIT_NODES:
      return GTEXT_HTML_E_LIMIT_NODES;
    case GTEXT_HTML_TREE_E_LIMIT_INPUT:
      return GTEXT_HTML_E_LIMIT_INPUT;
    case GTEXT_HTML_TREE_E_LIMIT_NAME:
      return GTEXT_HTML_E_LIMIT_NAME;
    case GTEXT_HTML_TREE_E_LIMIT_VALUE:
      return GTEXT_HTML_E_LIMIT_VALUE;
    case GTEXT_HTML_TREE_E_LIMIT_COMMENT:
      return GTEXT_HTML_E_LIMIT_COMMENT;
    case GTEXT_HTML_TREE_E_LIMIT_TEXT:
      return GTEXT_HTML_E_LIMIT_TEXT;
    case GTEXT_HTML_TREE_E_LIMIT_ATTRIBUTES:
      return GTEXT_HTML_E_LIMIT_ATTRIBUTES;
    case GTEXT_HTML_TREE_E_INVALID:     /* every argument was checked here, so the builder's own is a defect */
    case GTEXT_HTML_TREE_E_REPROCESS:
    case GTEXT_HTML_TREE_E_ENCODING:    /* handled where it can happen; anywhere else it is a defect */
      return GTEXT_HTML_E_INTERNAL;
  }
  return GTEXT_HTML_E_INTERNAL;
}

/* Record a failure (the first one stands) and return it. */
static GTEXT_HTML_Status parser_fail(GTEXT_HTML_Parser * p, GTEXT_HTML_Status status) {
  if (p->status == GTEXT_HTML_OK) {
    p->status = status;
  }
  return p->status;
}

static GTEXT_HTML_Status parser_fail_tree(GTEXT_HTML_Parser * p, GTEXT_HTML_TreeStatus status) {
  return parser_fail(p, html_status_from_tree(status));
}

/* ---- the decision (the standard's encoding sniffing algorithm) --------------- */

/// The answer of the algorithm.
typedef struct {
  GTEXT_Encoding encoding;                  ///< The encoding picked.
  GTEXT_Markup_Encoding_Source source;      ///< How.
  GTEXT_HTML_Encoding_Certainty confidence; ///< How sure.
  size_t bom_length;                        ///< Bytes of the byte order mark, or 0.
} Decision;

/// "Determining the character encoding" (13.2.3.2) over the first `n` bytes (at most 1024) of a document. `transport` is NONE for no label; `fallback` is
/// NONE for UTF-8. Never looks at more than the standard says, and is the same function for the parser and for gtext_html_sniff_encoding().
static void decide_encoding(GTEXT_Encoding transport, bool transport_certain, GTEXT_Encoding fallback, bool ignore_meta,
    const unsigned char * d, size_t n, Decision * out) {
  size_t bom = 0;
  /* "If the result of BOM sniffing is an encoding, return that encoding with confidence certain." */
  const GTEXT_Encoding from_bom = html_bom_sniff(d, n, &bom);
  if (from_bom != GTEXT_ENCODING_NONE) {
    out->encoding = from_bom;
    out->source = GTEXT_MARKUP_ENCODING_BOM;
    out->confidence = GTEXT_HTML_CERTAIN;
    out->bom_length = bom;
    return;
  }
  out->bom_length = 0;
  /* "If the transport layer specifies a character encoding, and it is supported, return that encoding with the confidence certain." */
  if (transport != GTEXT_ENCODING_NONE && transport_certain) {
    out->encoding = transport;
    out->source = GTEXT_MARKUP_ENCODING_CALLER;
    out->confidence = GTEXT_HTML_CERTAIN;
    return;
  }
  /* "Optionally, prescan the byte stream to determine its encoding ... If it returns a character encoding, then return the same encoding, with
     confidence tentative." */
  if (!ignore_meta) {
    const GTEXT_Encoding declared = html_prescan(d, n < GTEXT_HTML_PRESCAN_BYTES ? n : GTEXT_HTML_PRESCAN_BYTES);
    if (declared != GTEXT_ENCODING_NONE) {
      out->encoding = declared;
      out->source = GTEXT_MARKUP_ENCODING_DECLARATION;
      out->confidence = GTEXT_HTML_TENTATIVE;
      return;
    }
  }
  /* A tentative label from the caller is the caller's guess ("information on the likely encoding"); then "an implementation-defined or
     user-specified default character encoding, with the confidence tentative". */
  out->confidence = GTEXT_HTML_TENTATIVE;
  if (transport != GTEXT_ENCODING_NONE) {
    out->encoding = transport;
    out->source = GTEXT_MARKUP_ENCODING_CALLER_DEFAULT;
  }
  else if (fallback != GTEXT_ENCODING_NONE) {
    out->encoding = fallback;
    out->source = GTEXT_MARKUP_ENCODING_CALLER_DEFAULT;
  }
  else {
    out->encoding = GTEXT_ENCODING_UTF_8;
    out->source = GTEXT_MARKUP_ENCODING_DEFAULT;
  }
}

GTEXT_HTML_Status gtext_html_sniff_encoding(const void * data, size_t len, const GTEXT_HTML_Parse_Options * opts,
    GTEXT_HTML_Sniff_Result * result) {
  if (!result) {
    return GTEXT_HTML_E_INVALID;
  }
  memset(result, 0, sizeof(*result));
  const GTEXT_HTML_Parse_Options o = opts ? *opts : gtext_html_parse_options_default();
  if ((!data && len != 0) || (unsigned)o.transport_encoding_certainty > (unsigned)GTEXT_HTML_TENTATIVE) {
    result->status = GTEXT_HTML_E_INVALID;
    return GTEXT_HTML_E_INVALID;
  }
  const GTEXT_Encoding transport = html_label_encoding(o.transport_encoding);
  const GTEXT_Encoding fallback = html_label_encoding(o.default_encoding);
  Decision dec;
  decide_encoding(transport, o.transport_encoding_certainty == GTEXT_HTML_CERTAIN, fallback, o.ignore_meta, data,
      len < GTEXT_HTML_PRESCAN_BYTES ? len : GTEXT_HTML_PRESCAN_BYTES, &dec);
  result->status = GTEXT_HTML_OK;
  result->encoding = dec.encoding;
  result->encoding_name = gtext_encoding_name(dec.encoding);
  result->source = dec.source;
  result->confidence = dec.confidence;
  result->bom_length = dec.bom_length;
  result->transport_encoding_ignored = o.transport_encoding && transport == GTEXT_ENCODING_NONE;
  result->default_encoding_ignored = o.default_encoding && fallback == GTEXT_ENCODING_NONE;
  return GTEXT_HTML_OK;
}

/* ---- making a parser ---------------------------------------------------- */

/* The context taken by value, as the builder takes it. `attrs` is the caller's scratch for the
   attribute list (freed by the caller). */
static GTEXT_HTML_Status check_context(const GTEXT_HTML_Fragment_Context * c) {
  if (!c || !c->local_name || c->local_name[0] == '\0' || (unsigned)c->ns > (unsigned)GTEXT_HTML_NS_MATHML
      || (unsigned)c->quirks > (unsigned)GTEXT_MARKUP_MODE_QUIRKS
      || (c->attribute_count > 0 && !c->attributes)) {
    return GTEXT_HTML_E_INVALID;
  }
  for (size_t i = 0; i < c->attribute_count; i++) {
    if (!c->attributes[i].name || c->attributes[i].name[0] == '\0' || !c->attributes[i].value) {
      return GTEXT_HTML_E_INVALID;
    }
  }
  return GTEXT_HTML_OK;
}

/* Make a parser. `context` is NULL for a document. `errors_wanted` false keeps no parse errors. */
static GTEXT_HTML_Status parser_make(const GTEXT_HTML_Parse_Options * opts,
    const GTEXT_HTML_Fragment_Context * context, bool errors_wanted, GTEXT_HTML_Parser ** out) {
  *out = NULL;
  if (context) {
    const GTEXT_HTML_Status checked = check_context(context);
    if (checked != GTEXT_HTML_OK) {
      return checked;
    }
  }
  const GTEXT_HTML_Parse_Options o = opts ? *opts : gtext_html_parse_options_default();
  if ((unsigned)o.transport_encoding_certainty > (unsigned)GTEXT_HTML_TENTATIVE) {
    return GTEXT_HTML_E_INVALID;
  }
  const GTEXT_Allocator * allocator = o.allocator ? o.allocator : gtext_allocator_default();
  GTEXT_HTML_Parser * p = gtext_allocator_calloc(allocator, 1, sizeof(*p));
  if (!p) {
    return GTEXT_HTML_E_OOM;
  }
  p->allocator = *allocator;
  p->fragment = context != NULL;
  p->max_input = o.max_input_bytes ? o.max_input_bytes : GTEXT_HTML_TOKENIZER_DEFAULT_MAX_INPUT;
  p->transport = html_label_encoding(o.transport_encoding);
  p->transport_ignored = o.transport_encoding && p->transport == GTEXT_ENCODING_NONE;
  p->transport_certain = o.transport_encoding_certainty == GTEXT_HTML_CERTAIN;
  p->fallback = html_label_encoding(o.default_encoding);
  p->fallback_ignored = o.default_encoding && p->fallback == GTEXT_ENCODING_NONE;
  p->ignore_meta = o.ignore_meta;
  p->max_reparse = o.max_reparse_bytes == 0 ? GTEXT_HTML_DEFAULT_MAX_REPARSE_BYTES
      : (o.max_reparse_bytes < GTEXT_HTML_PRESCAN_BYTES ? GTEXT_HTML_PRESCAN_BYTES : o.max_reparse_bytes);
  p->fragment_source = p->transport != GTEXT_ENCODING_NONE ? GTEXT_MARKUP_ENCODING_CALLER : GTEXT_MARKUP_ENCODING_DEFAULT;
  p->phase = context ? PH_RUN : PH_SNIFF;
  /* The decision reads the whole window only when the prescan can answer; a byte order mark and a certain label need the first three bytes
     (or the whole input, if it is shorter), so a caller that knows the encoding is not made to wait for 1024. */
  p->need = (p->ignore_meta || (p->transport != GTEXT_ENCODING_NONE && p->transport_certain)) ? 3 : GTEXT_HTML_PRESCAN_BYTES;

  GTEXT_HTML_TreeOptions * to = &p->topts;
  memset(to, 0, sizeof(*to));
  to->allocator = &p->allocator;
  to->max_open_elements = o.max_open_elements;
  to->max_nodes = o.max_nodes;
  to->max_input_bytes = o.max_input_bytes;
  to->max_name_bytes = o.max_name_bytes;
  to->max_value_bytes = o.max_attribute_bytes;
  to->max_comment_bytes = o.max_comment_bytes;
  to->max_text_bytes = o.max_text_bytes;
  to->max_attributes = o.max_attributes;
  to->max_errors = o.max_errors;
  to->count_errors_only = o.count_errors_only || !errors_wanted;
  to->scripting = o.scripting;

  if (!context) {
    *out = p;
    return GTEXT_HTML_OK;
  }

  GTEXT_HTML_Attribute * attrs = NULL;
  if (context->attribute_count > 0) {
    if (context->attribute_count > SIZE_MAX / sizeof(*attrs)) {
      gtext_allocator_free(&p->allocator, p);
      return GTEXT_HTML_E_OOM;
    }
    attrs = gtext_allocator_malloc(&p->allocator, context->attribute_count * sizeof(*attrs));
    if (!attrs) {
      gtext_allocator_free(&p->allocator, p);
      return GTEXT_HTML_E_OOM;
    }
    for (size_t i = 0; i < context->attribute_count; i++) {
      attrs[i].name = context->attributes[i].name;
      attrs[i].name_length = strlen(context->attributes[i].name);
      attrs[i].value = context->attributes[i].value;
      attrs[i].value_length = strlen(context->attributes[i].value);
    }
  }
  GTEXT_HTML_FragmentContext fc;
  memset(&fc, 0, sizeof(fc));
  fc.ns = context->ns;
  fc.local_name = context->local_name;
  fc.local_name_length = strlen(context->local_name);
  fc.attributes = attrs;
  fc.attribute_count = context->attribute_count;
  fc.form_ancestor = context->form_ancestor;
  fc.quirks = context->quirks;
  fc.scripting = o.scripting;
  GTEXT_HTML_TreeStatus ts = GTEXT_HTML_TREE_OK;
  p->tree = gtext_html_tree_new_fragment(to, &fc, &ts);
  gtext_allocator_free(&p->allocator, attrs);
  if (!p->tree) {
    const GTEXT_HTML_Status s = html_status_from_tree(ts);
    gtext_allocator_free(&p->allocator, p);
    return s;
  }
  *out = p;
  return GTEXT_HTML_OK;
}

GTEXT_HTML_Status gtext_html_parser_new(
    const GTEXT_HTML_Parse_Options * opts, GTEXT_HTML_Parser ** out) {
  if (!out) {
    return GTEXT_HTML_E_INVALID;
  }
  return parser_make(opts, NULL, true, out);
}

GTEXT_HTML_Status gtext_html_parser_new_fragment(const GTEXT_HTML_Fragment_Context * context,
    const GTEXT_HTML_Parse_Options * opts, GTEXT_HTML_Parser ** out) {
  if (!out) {
    return GTEXT_HTML_E_INVALID;
  }
  if (!context) {
    *out = NULL;
    return GTEXT_HTML_E_INVALID;
  }
  return parser_make(opts, context, true, out);
}

/// Tests: read UTF-8 through the decoder and the source map too (declared in html_api_internal.h).
GTEXT_HTML_Status html_parser_force_transcode(GTEXT_HTML_Parser * p) {
  if (!p) {
    return GTEXT_HTML_E_INVALID;
  }
  if (p->fed != 0 || p->decided || p->status != GTEXT_HTML_OK) {
    return GTEXT_HTML_E_STATE; /* (it is a choice of how the first byte is read) */
  }
  p->force_transcode = true;
  return GTEXT_HTML_OK;
}

/// Tests: the bytes the parser is keeping for a restart (declared in html_api_internal.h).
size_t html_parser_kept_bytes(const GTEXT_HTML_Parser * p) {
  return p ? p->win_len * (p->phase == PH_SNIFF ? 1 : 0) + p->keep_len : 0;
}

/* Drop the builder and the decoder (a restart makes new ones; the parser's free ends with it). */
static void pipeline_free(GTEXT_HTML_Parser * p) {
  gtext_html_tree_free(p->tree);
  p->tree = NULL;
  gtext_decoder_free(p->decoder);
  p->decoder = NULL;
  gtext_allocator_free(&p->allocator, p->flagged);
  p->flagged = NULL;
}

void gtext_html_parser_free(GTEXT_HTML_Parser * p) {
  if (!p) {
    return;
  }
  const GTEXT_Allocator allocator = p->allocator;
  pipeline_free(p);
  gtext_allocator_free(&allocator, p->keep);
  gtext_allocator_free(&allocator, p);
}

/* ---- the pipeline: bytes to the builder ------------------------------------ */

/// Start reading in the encoding in `enc`: a builder (tentative or not as `confidence` says) and, unless the tokenizer reads the bytes itself, a decoder.
static GTEXT_HTML_Status pipeline_start(GTEXT_HTML_Parser * p) {
  GTEXT_HTML_TreeOptions to = p->topts;
  to.encoding = (int)p->enc;
  to.encoding_tentative = p->confidence == GTEXT_HTML_TENTATIVE && !p->ignore_meta;
  p->use_decoder = p->enc != GTEXT_ENCODING_UTF_8 || p->force_transcode;
  if (p->use_decoder) {
    to.max_input_bytes = SIZE_MAX; /* the tokenizer counts what the decoder wrote; the input limit is on the bytes given, and is this layer's */
  }
  p->stream_pos = 0;
  p->tree = gtext_html_tree_new(&to);
  if (!p->tree) {
    return parser_fail(p, GTEXT_HTML_E_OOM);
  }
  if (p->use_decoder) {
    GTEXT_Decoder_Options dopts = gtext_decoder_options_default();
    dopts.allocator = &p->allocator;
    dopts.bom_sniffing = true; /* "decode": a byte order mark chooses the encoding over the one asked for (and the sniff has found the same one) */
    dopts.track_offsets = true;
    dopts.max_output_bytes = SIZE_MAX;
    const GTEXT_Encoding_Status ds = gtext_decoder_new(p->enc, &dopts, &p->decoder);
    if (ds != GTEXT_ENCODING_OK) {
      return parser_fail(p, ds == GTEXT_ENCODING_E_OOM ? GTEXT_HTML_E_OOM : GTEXT_HTML_E_INTERNAL);
    }
    p->flagged = gtext_allocator_malloc(&p->allocator, GTEXT_ENCODING_MAX_CHUNK * sizeof(size_t));
    if (!p->flagged) {
      return parser_fail(p, GTEXT_HTML_E_OOM);
    }
  }
  return GTEXT_HTML_OK;
}

/// The decoder's sink: the builder gets the chunk with its source map.
static bool sink_write(void * user, const char * bytes, size_t len, const GTEXT_Encoding_Source_Map * map) {
  GTEXT_HTML_Parser * p = user;
  if (len > GTEXT_ENCODING_MAX_CHUNK) { /* (the decoder never gives more: the scratch is sized by its constant) */
    p->sink_status = GTEXT_HTML_TREE_E_INVALID;
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    if (map->offsets[i] > GTEXT_HTML_SOURCE_START_MASK) { /* (a 32-bit build, an input of hundreds of megabytes) */
      p->sink_status = GTEXT_HTML_TREE_E_LIMIT_INPUT;
      return false;
    }
    p->flagged[i] = map->offsets[i] | ((size_t)map->widths[i] << GTEXT_HTML_SOURCE_WIDTH_SHIFT)
        | (map->replaced[i] ? GTEXT_HTML_SOURCE_REPLACED : (size_t)0);
  }
  const GTEXT_HTML_TreeStatus ts = gtext_html_tree_feed_mapped(p->tree, bytes, len, p->flagged);
  if (ts != GTEXT_HTML_TREE_OK) {
    p->sink_status = ts;
    return false;
  }
  return true;
}

/// A builder status after a pass: OK, the restart, or a failure of the parser.
static int tree_verdict(GTEXT_HTML_Parser * p, GTEXT_HTML_TreeStatus ts) {
  if (ts == GTEXT_HTML_TREE_OK) {
    return GTEXT_HTML_OK;
  }
  if (ts == GTEXT_HTML_TREE_E_ENCODING) {
    return ST_RESTART;
  }
  return (int)parser_fail_tree(p, ts);
}

/// Give the present builder `n` bytes (and the end of the input if `final`). ST_RESTART when it asks to change the encoding.
static int process(GTEXT_HTML_Parser * p, const unsigned char * bytes, size_t n, bool final) {
  if (p->use_decoder) {
    if (n > 0 || final) {
      const GTEXT_Encoding_Mapped_Sink sink = {sink_write, p};
      p->sink_status = GTEXT_HTML_TREE_OK;
      const GTEXT_Encoding_Status ds = gtext_decoder_decode_mapped(p->decoder, bytes, n, final, &sink, NULL);
      if (ds == GTEXT_ENCODING_E_WRITE) {
        return tree_verdict(p, p->sink_status);
      }
      if (ds != GTEXT_ENCODING_OK) {
        return (int)parser_fail(p, ds == GTEXT_ENCODING_E_OOM ? GTEXT_HTML_E_OOM : GTEXT_HTML_E_INTERNAL);
      }
    }
    p->stream_pos += n;
    if (final) {
      return tree_verdict(p, gtext_html_tree_finish_mapped(p->tree, p->fed));
    }
    return GTEXT_HTML_OK;
  }
  /* The tokenizer reads UTF-8 itself. A byte order mark is skipped, but counted in every offset and in the limit. */
  if (p->stream_pos == 0 && p->bom_length > 0 && n >= p->bom_length) {
    const GTEXT_HTML_TreeStatus ts = gtext_html_tree_skip_input(p->tree, p->bom_length);
    if (ts != GTEXT_HTML_TREE_OK) {
      return tree_verdict(p, ts);
    }
    bytes += p->bom_length;
    n -= p->bom_length;
    p->stream_pos = p->bom_length;
  }
  if (n > 0) {
    const GTEXT_HTML_TreeStatus ts = gtext_html_tree_feed(p->tree, bytes, n);
    if (ts != GTEXT_HTML_TREE_OK) {
      return tree_verdict(p, ts);
    }
  }
  p->stream_pos += n;
  if (final) {
    return tree_verdict(p, gtext_html_tree_finish(p->tree));
  }
  return GTEXT_HTML_OK;
}

/// The restart of "change the encoding": the parse begins again, in the encoding the `<meta>` declared, with certainty.
static GTEXT_HTML_Status restart(GTEXT_HTML_Parser * p) {
  p->enc = (GTEXT_Encoding)gtext_html_tree_declared_encoding(p->tree);
  p->source = GTEXT_MARKUP_ENCODING_DECLARATION;
  p->confidence = GTEXT_HTML_CERTAIN;
  p->bom_length = 0;
  p->reparsed = true;
  pipeline_free(p);
  return pipeline_start(p);
}

/// Process `cur` (and the end of the input if `final`); on a restart, again from the first byte: the kept bytes `prev` and then `cur`.
static GTEXT_HTML_Status run_bytes(GTEXT_HTML_Parser * p, const unsigned char * prev, size_t prev_len,
    const unsigned char * cur, size_t cur_len, bool final) {
  int st = process(p, cur, cur_len, final);
  if (st == ST_RESTART) {
    const GTEXT_HTML_Status r = restart(p);
    if (r != GTEXT_HTML_OK) {
      return r;
    }
    if (p->whole) { /* the caller's buffer is every byte there is */
      st = process(p, p->whole, p->fed, final);
      return st == ST_RESTART ? parser_fail(p, GTEXT_HTML_E_INTERNAL) : (GTEXT_HTML_Status)st;
    }
    if (prev_len > 0) {
      st = process(p, prev, prev_len, false);
      if (st != GTEXT_HTML_OK) { /* a certain encoding does not ask to change */
        return st == ST_RESTART ? parser_fail(p, GTEXT_HTML_E_INTERNAL) : (GTEXT_HTML_Status)st;
      }
    }
    st = process(p, cur, cur_len, final);
    if (st == ST_RESTART) {
      return parser_fail(p, GTEXT_HTML_E_INTERNAL);
    }
  }
  return (GTEXT_HTML_Status)st;
}

static bool keep_append(GTEXT_HTML_Parser * p, const unsigned char * d, size_t n) {
  if (n == 0) {
    return true;
  }
  if (n > SIZE_MAX - p->keep_len) {
    return false;
  }
  if (p->keep_len + n > p->keep_cap) {
    size_t cap = p->keep_cap ? p->keep_cap : 4096;
    while (cap < p->keep_len + n) {
      if (cap > SIZE_MAX / 2) {
        cap = p->keep_len + n;
        break;
      }
      cap *= 2;
    }
    unsigned char * grown = gtext_allocator_realloc(&p->allocator, p->keep, cap);
    if (!grown) {
      return false;
    }
    p->keep = grown;
    p->keep_cap = cap;
  }
  memcpy(p->keep + p->keep_len, d, n);
  p->keep_len += n;
  return true;
}

/// After a pass over `cur`: keep it for a restart, or let go of everything kept if no restart can come any more.
static GTEXT_HTML_Status keep_after(GTEXT_HTML_Parser * p, const unsigned char * cur, size_t cur_len) {
  if (!p->keep_active) {
    return GTEXT_HTML_OK;
  }
  if (gtext_html_tree_head_left(p->tree) || !gtext_html_tree_encoding_tentative(p->tree)) {
    p->keep_active = false;
    gtext_allocator_free(&p->allocator, p->keep);
    p->keep = NULL;
    p->keep_len = 0;
    p->keep_cap = 0;
    return GTEXT_HTML_OK;
  }
  if (!keep_append(p, cur, cur_len)) {
    return parser_fail(p, GTEXT_HTML_E_OOM);
  }
  return GTEXT_HTML_OK;
}

/// Let go of the bytes kept for a restart: from here a `<meta>` is an ordinary element.
static void reparse_abandon(GTEXT_HTML_Parser * p) {
  p->keep_active = false;
  p->reparse_abandoned = true;
  gtext_allocator_free(&p->allocator, p->keep);
  p->keep = NULL;
  p->keep_len = 0;
  p->keep_cap = 0;
  gtext_html_tree_stop_reparse(p->tree);
}

/// The first bytes of the input are all here (a full window, or the input has ended): decide the encoding and start reading.
static GTEXT_HTML_Status decide_and_start(GTEXT_HTML_Parser * p, const unsigned char * window, size_t window_len) {
  Decision d;
  decide_encoding(p->transport, p->transport_certain, p->fallback, p->ignore_meta, window, window_len, &d);
  p->enc = d.encoding;
  p->source = d.source;
  p->confidence = d.confidence;
  p->bom_length = d.bom_length;
  p->decided = true;
  p->phase = PH_RUN;
  p->keep_active = !p->whole && d.confidence == GTEXT_HTML_TENTATIVE && !p->ignore_meta;
  return pipeline_start(p);
}

/* ---- feeding ------------------------------------------------------------ */

GTEXT_HTML_Status gtext_html_parser_feed(GTEXT_HTML_Parser * p, const void * data, size_t len) {
  if (!p) {
    return GTEXT_HTML_E_INVALID;
  }
  /* A failed parser says its failure to every call, whatever the call's arguments. */
  if (p->status != GTEXT_HTML_OK) {
    return p->status;
  }
  if (!data && len != 0) {
    return GTEXT_HTML_E_INVALID;
  }
  if (p->finished || p->taken) {
    return GTEXT_HTML_E_STATE;
  }
  if (len == 0) {
    return GTEXT_HTML_OK;
  }
  if (p->fed > p->max_input || len > p->max_input - p->fed) {
    return parser_fail(p, GTEXT_HTML_E_LIMIT_INPUT);
  }
  p->fed += len;
  if (p->fragment) {
    const GTEXT_HTML_TreeStatus ts = gtext_html_tree_feed(p->tree, data, len);
    return ts == GTEXT_HTML_TREE_OK ? GTEXT_HTML_OK : parser_fail_tree(p, ts);
  }
  const unsigned char * d = data;
  if (p->phase == PH_SNIFF) {
    if (p->whole) {
      /* The caller's buffer is all of it, here for the whole call: the window is its start, and nothing is copied. */
      if (p->fed < p->need) {
        return GTEXT_HTML_OK; /* the end of the input decides */
      }
      GTEXT_HTML_Status st = decide_and_start(p, p->whole, p->need);
      if (st != GTEXT_HTML_OK) {
        return st;
      }
      return run_bytes(p, NULL, 0, p->whole, p->fed, false);
    }
    const size_t room = p->need - p->win_len;
    const size_t take = len < room ? len : room;
    memcpy(p->win + p->win_len, d, take);
    p->win_len += take;
    if (p->win_len < p->need) {
      return GTEXT_HTML_OK;
    }
    d += take;
    len -= take;
    GTEXT_HTML_Status st = decide_and_start(p, p->win, p->win_len);
    if (st != GTEXT_HTML_OK) {
      return st;
    }
    st = run_bytes(p, NULL, 0, p->win, p->win_len, false);
    if (st != GTEXT_HTML_OK) {
      return st;
    }
    st = keep_after(p, p->win, p->win_len);
    if (st != GTEXT_HTML_OK || len == 0) {
      return st;
    }
  }
  if (p->keep_active && len > p->max_reparse - p->keep_len) {
    /* The input has gone on past what may be kept for a restart. The bytes up to the limit are processed as the keeping parser processes them (so a
       `<meta>` that ends within them restarts however the input was cut), and then the rest is processed with the bytes let go. */
    const size_t room = p->max_reparse - p->keep_len;
    if (room > 0) {
      GTEXT_HTML_Status st = run_bytes(p, p->keep, p->keep_len, d, room, false);
      if (st != GTEXT_HTML_OK) {
        return st;
      }
      st = keep_after(p, d, room);
      if (st != GTEXT_HTML_OK) {
        return st;
      }
      d += room;
      len -= room;
    }
    if (p->keep_active) {
      reparse_abandon(p);
    }
    return run_bytes(p, NULL, 0, d, len, false);
  }
  const GTEXT_HTML_Status st = run_bytes(p, p->keep, p->keep_len, d, len, false);
  if (st != GTEXT_HTML_OK) {
    return st;
  }
  return keep_after(p, d, len);
}

/* ---- finishing ---------------------------------------------------------- */

/* Run the end of the input through the builder, once. */
static GTEXT_HTML_Status parser_complete(GTEXT_HTML_Parser * p) {
  if (p->status != GTEXT_HTML_OK) {
    return p->status;
  }
  if (p->finished) {
    return GTEXT_HTML_OK;
  }
  if (p->fragment) {
    const GTEXT_HTML_TreeStatus ts = gtext_html_tree_finish(p->tree);
    if (ts != GTEXT_HTML_TREE_OK) {
      return parser_fail_tree(p, ts);
    }
    p->finished = true;
    return GTEXT_HTML_OK;
  }
  GTEXT_HTML_Status st;
  if (p->phase == PH_SNIFF) {
    /* The input ended before there were `need` bytes to look at: what there is, is the window, and all of the document. */
    const unsigned char * all = p->whole ? p->whole : p->win;
    const size_t n = p->whole ? p->fed : p->win_len;
    st = decide_and_start(p, all, n);
    if (st != GTEXT_HTML_OK) {
      return st;
    }
    st = run_bytes(p, NULL, 0, all, n, true);
  }
  else {
    st = run_bytes(p, p->keep, p->keep_len, NULL, 0, true);
  }
  if (st != GTEXT_HTML_OK) {
    return st;
  }
  p->finished = true;
  return GTEXT_HTML_OK;
}

/* Fill the result for the parser's present state. */
static void parser_report(GTEXT_HTML_Parser * p, GTEXT_HTML_Status status, GTEXT_Markup_Document * doc,
    GTEXT_HTML_Result * result) {
  if (!result) {
    return;
  }
  html_result_clear(result);
  result->status = status;
  result->message = gtext_html_status_string(status);
  if (p->fragment) {
    result->encoding = "UTF-8";
    result->encoding_source = p->fragment_source;
    result->encoding_confidence = GTEXT_HTML_IRRELEVANT;
  }
  else {
    Decision d;
    if (p->decided) {
      d.encoding = p->enc;
      d.source = p->source;
      d.confidence = p->confidence;
      d.bom_length = p->bom_length;
    }
    else {
      /* A failure before the encoding was decided: what the bytes so far say. */
      decide_encoding(p->transport, p->transport_certain, p->fallback, p->ignore_meta, p->whole ? p->whole : p->win,
          p->whole ? (p->fed < p->need ? p->fed : p->need) : p->win_len, &d);
    }
    result->encoding = gtext_encoding_name(d.encoding);
    result->encoding_source = d.source;
    result->encoding_confidence = d.confidence;
    result->bom_length = d.bom_length;
    result->reparsed = p->reparsed;
    result->reparse_abandoned = p->reparse_abandoned;
    /* A declaration the builder met and agreed with, or a restart, made it certain: the builder knows. */
    if (p->decided && p->tree && p->confidence == GTEXT_HTML_TENTATIVE && !p->ignore_meta
        && !gtext_html_tree_encoding_tentative(p->tree)) {
      result->encoding_confidence = GTEXT_HTML_CERTAIN;
    }
  }
  result->transport_encoding_ignored = p->transport_ignored;
  result->default_encoding_ignored = p->fallback_ignored;
  result->input_bytes = p->fed;
  if (doc) {
    result->mode = gtext_markup_document_mode(doc);
    result->node_count = gtext_markup_document_node_count(doc);
  }
  /* The error list is best effort after a failure: the status stands if it cannot be built. */
  html_result_set_errors(result, &p->allocator, p->tree);
}

/* The one finish: `doc_out` for a document parser, `doc_out` and `fragment_out` for a fragment. */
static GTEXT_HTML_Status parser_finish(GTEXT_HTML_Parser * p, bool fragment,
    GTEXT_Markup_Document ** doc_out, GTEXT_Markup_Node ** fragment_out, GTEXT_HTML_Result * result) {
  html_result_clear(result);
  if (!p) {
    html_result_set_failure(result, GTEXT_HTML_E_INVALID, NULL);
    return GTEXT_HTML_E_INVALID;
  }
  if (p->status != GTEXT_HTML_OK) {
    /* The sticky failure comes before any complaint about the call (the wrong kind of finish, a NULL output). */
    parser_report(p, p->status, NULL, result);
    return p->status;
  }
  if (!doc_out || (fragment && !fragment_out)) {
    html_result_set_failure(result, GTEXT_HTML_E_INVALID, NULL);
    return GTEXT_HTML_E_INVALID;
  }
  if (p->fragment != fragment || p->taken) {
    /* The wrong kind of finish, or a second one: not a failure of the parse, and the parser stays as it is. */
    html_result_set_failure(result, GTEXT_HTML_E_STATE, NULL);
    return GTEXT_HTML_E_STATE;
  }
  GTEXT_HTML_Status status = parser_complete(p);
  if (status != GTEXT_HTML_OK) {
    parser_report(p, status, NULL, result);
    return status;
  }
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_Markup_Node * node = NULL;
  if (fragment) {
    node = gtext_html_tree_take_fragment(p->tree, &doc);
    if (!node || !doc) {
      status = parser_fail(p, GTEXT_HTML_E_INTERNAL);
      parser_report(p, status, NULL, result);
      return status;
    }
  }
  else {
    doc = gtext_html_tree_take_document(p->tree);
    if (!doc) {
      status = parser_fail(p, GTEXT_HTML_E_INTERNAL);
      parser_report(p, status, NULL, result);
      return status;
    }
    /* The encoding is recorded on the document as the XML parser records its own. */
    if (gtext_markup_document_set_encoding(doc, gtext_encoding_name(p->enc), p->source) != GTEXT_MARKUP_OK) {
      gtext_markup_document_free(doc);
      status = parser_fail(p, GTEXT_HTML_E_OOM);
      parser_report(p, status, NULL, result);
      return status;
    }
  }
  p->taken = true;
  if (result) {
    parser_report(p, GTEXT_HTML_OK, doc, result);
    if (!result->internal) {
      /* The error list could not be built: a success without its report is a failure of the call. */
      gtext_markup_document_free(doc);
      p->taken = false;
      p->finished = false;
      status = parser_fail(p, GTEXT_HTML_E_OOM);
      parser_report(p, status, NULL, result);
      return status;
    }
  }
  *doc_out = doc;
  if (fragment) {
    *fragment_out = node;
  }
  return GTEXT_HTML_OK;
}

GTEXT_HTML_Status gtext_html_parser_finish(
    GTEXT_HTML_Parser * p, GTEXT_Markup_Document ** out, GTEXT_HTML_Result * result) {
  return parser_finish(p, false, out, NULL, result);
}

GTEXT_HTML_Status gtext_html_parser_finish_fragment(GTEXT_HTML_Parser * p, GTEXT_Markup_Document ** doc,
    GTEXT_Markup_Node ** fragment, GTEXT_HTML_Result * result) {
  return parser_finish(p, true, doc, fragment, result);
}

/* ---- the whole-buffer forms --------------------------------------------- */

GTEXT_HTML_Status gtext_html_parse(const void * data, size_t len, const GTEXT_HTML_Parse_Options * opts,
    GTEXT_Markup_Document ** out, GTEXT_HTML_Result * result) {
  html_result_clear(result);
  if (!out || (!data && len != 0)) {
    html_result_set_failure(result, GTEXT_HTML_E_INVALID, NULL);
    return GTEXT_HTML_E_INVALID;
  }
  GTEXT_HTML_Parser * p = NULL;
  const GTEXT_HTML_Status made = parser_make(opts, NULL, result != NULL, &p);
  if (made != GTEXT_HTML_OK) {
    html_result_set_failure(result, made, NULL);
    return made;
  }
  p->whole = data; /* valid for the whole call: a restart reads it again, and nothing is copied for one */
  GTEXT_HTML_Status status = gtext_html_parser_feed(p, data, len);
  if (status == GTEXT_HTML_OK) {
    status = parser_finish(p, false, out, NULL, result);
  }
  else {
    parser_report(p, status, NULL, result);
  }
  gtext_html_parser_free(p);
  return status;
}

GTEXT_HTML_Status gtext_html_parse_fragment(const GTEXT_HTML_Fragment_Context * context, const void * data,
    size_t len, const GTEXT_HTML_Parse_Options * opts, GTEXT_Markup_Document ** doc,
    GTEXT_Markup_Node ** fragment, GTEXT_HTML_Result * result) {
  html_result_clear(result);
  if (!context || !doc || !fragment || (!data && len != 0)) {
    html_result_set_failure(result, GTEXT_HTML_E_INVALID, NULL);
    return GTEXT_HTML_E_INVALID;
  }
  GTEXT_HTML_Parser * p = NULL;
  const GTEXT_HTML_Status made = parser_make(opts, context, result != NULL, &p);
  if (made != GTEXT_HTML_OK) {
    html_result_set_failure(result, made, NULL);
    return made;
  }
  GTEXT_HTML_Status status = gtext_html_parser_feed(p, data, len);
  if (status == GTEXT_HTML_OK) {
    status = parser_finish(p, true, doc, fragment, result);
  }
  else {
    parser_report(p, status, NULL, result);
  }
  gtext_html_parser_free(p);
  return status;
}
