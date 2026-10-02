/**
 * @file
 *
 * libFuzzer harness for GTEXT_JSON_Parse_Options::records - one input holding
 * several JSON texts.
 *
 * Two entry points read that format and they are two implementations of one
 * rule, so the harness needs no expectations file:
 *
 *     the streaming parser and a loop over gtext_json_parse_multiple()
 *     must agree about whether an input is a legal sequence,
 *     and about how many records it holds;
 *
 *     and at every chunk size, because a separator is the one thing a feed
 *     boundary can split - the streaming parser's two known defects were both
 *     answers that depended on where the caller's chunks fell.
 *
 * fuzz_json.cpp cannot ask this. Its differential is gtext_json_parse() - one
 * document - against the stream, which is exactly the comparison that stops
 * holding the moment an input is allowed to hold two; and its corpus is JSON
 * text, so it never produces the bytes *between* two documents, which is where
 * every rule this option adds lives.
 *
 * The third property is the round trip, and it runs in the other direction:
 * records are built, written with the framing, and read back. A mode whose
 * writer and reader disagree would be a format this library could emit and not
 * consume.
 *
 * The first byte picks the mode, so one corpus unit explores one framing, and
 * GTEXT_JSON_RECORDS_OFF is included rather than skipped: a mode that is meant
 * to change nothing is worth fuzzing as the control. It needs a *different*
 * oracle, though, which is what this harness reported on its first run - about
 * itself, at 327 executions from an empty corpus. gtext_json_parse_multiple()
 * reads a sequence whatever `records` says, and emits no record boundaries, so
 * with the option off neither its verdict nor its count is comparable to the
 * stream's. There the oracle is gtext_json_parse(): one document, one
 * document.
 *
 * Build with: make fuzz-json-records   Run: make fuzz-run-json-records
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <ghoti.io/text/json.h>
}

namespace {

[[noreturn]] void fail(const char * what, const std::string & input) {
  std::fprintf(stderr, "%s\ninput was (%zu bytes): ", what, input.size());
  for (unsigned char c : input) {
    if (c >= 0x20 && c < 0x7f) {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fputc('\n', stderr);
  std::abort();
}

size_t g_record_ends = 0;

GTEXT_JSON_Status count_cb(
    void * user, const GTEXT_JSON_Event * evt, GTEXT_JSON_Error * err) {
  (void)user;
  (void)err;
  if (evt->type == GTEXT_JSON_EVT_RECORD_END) {
    g_record_ends++;
  }
  return GTEXT_JSON_OK;
}

/** Feed through the stream in @p chunk-byte pieces. */
bool stream_reads(const char * text, size_t len,
    const GTEXT_JSON_Parse_Options * opts, size_t chunk, size_t * out_records) {
  g_record_ends = 0;
  GTEXT_JSON_Stream * st = gtext_json_stream_new(opts, count_cb, nullptr);
  if (!st)
    return false;

  GTEXT_JSON_Status s = GTEXT_JSON_OK;
  size_t off = 0;
  while (off < len && s == GTEXT_JSON_OK) {
    const size_t n = (chunk && chunk < len - off) ? chunk : len - off;
    s = gtext_json_stream_feed(st, text + off, n, nullptr);
    off += n;
  }
  const GTEXT_JSON_Status f = gtext_json_stream_finish(st, nullptr);
  gtext_json_stream_free(st);
  *out_records = g_record_ends;
  return s == GTEXT_JSON_OK && f == GTEXT_JSON_OK;
}

/** Read the whole input by looping gtext_json_parse_multiple(). */
bool dom_reads(const char * text, size_t len,
    const GTEXT_JSON_Parse_Options * opts, size_t * out_records) {
  size_t off = 0;
  size_t records = 0;
  while (off < len) {
    size_t used = 0;
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * v =
        gtext_json_parse_multiple(text + off, len - off, opts, &err, &used);
    gtext_json_error_free(&err);
    if (!v) {
      *out_records = records;
      return false;
    }
    records++;
    gtext_json_free(v);
    if (used == 0) {
      /* No progress with a value returned would spin for ever. That is a
         defect in the entry point, not an input this harness should tolerate.
       */
      gtext_json_free(nullptr);
      fail("gtext_json_parse_multiple() returned a value and consumed nothing",
          std::string(text, len));
    }
    off += used;
  }
  *out_records = records;
  return true;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2)
    return 0;

  static const GTEXT_JSON_Records modes[] = {GTEXT_JSON_RECORDS_OFF,
      GTEXT_JSON_RECORDS_WHITESPACE, GTEXT_JSON_RECORDS_LINE,
      GTEXT_JSON_RECORDS_SEQ};
  const GTEXT_JSON_Records mode = modes[data[0] & 0x03];

  GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
  opts.records = mode;
  /* One bit for comments, in the two modes that allow them. Deliberately not
     combined with GTEXT_JSON_RECORDS_LINE, nor with allow_unescaped_controls
     or allow_line_continuations in any mode, and not with
     GTEXT_JSON_RECORDS_SEQ either: each of those combinations is refused
     outright, which the unit tests assert, and a harness spending its inputs
     on a documented refusal learns nothing. That LINE and SEQ each cannot be
     combined with comments is itself something this harness found - for SEQ,
     as a `//` comment swallowing every RS after it. */
  opts.allow_comments = (data[0] & 0x04) != 0 &&
      mode != GTEXT_JSON_RECORDS_LINE && mode != GTEXT_JSON_RECORDS_SEQ;
  opts.max_total_bytes = 0;

  const char * text = reinterpret_cast<const char *>(data + 1);
  const size_t len = size - 1;

  size_t stream_records = 0;
  size_t dom_records = 0;
  const bool by_stream = stream_reads(text, len, &opts, 0, &stream_records);

  /* **The differential, and which oracle it is depends on the mode.** This
     distinction is the whole correctness of the harness, and getting it wrong
     was the first thing this harness reported - against itself.
   *
   * gtext_json_parse_multiple() is the "allow more than one value" entry point
   * whatever `records` says: with the option off it still reads a sequence, and
   * it still emits no record boundaries, so neither its verdict nor its count
   * is comparable to a stream that is refusing trailing content. The right
   * oracle there is gtext_json_parse(), one document against one document.
   *
   * With a framing named, the two *are* implementations of one rule, and both
   * the verdict and the count have to match. */
  if (mode == GTEXT_JSON_RECORDS_OFF) {
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * one = gtext_json_parse(text, len, &opts, &err);
    const bool by_dom_single = one != nullptr;
    if (one)
      gtext_json_free(one);
    gtext_json_error_free(&err);
    if (by_stream != by_dom_single) {
      std::fprintf(stderr,
          "records off: the stream said %s and gtext_json_parse() said %s\n",
          by_stream ? "yes" : "no", by_dom_single ? "yes" : "no");
      fail("with records off the stream must accept exactly one document",
          std::string(text, len));
    }
    if (stream_records != 0) {
      fail("EVT_RECORD_END was emitted with records off",
          std::string(text, len));
    }
  }
  else {
    const bool by_dom = dom_reads(text, len, &opts, &dom_records);
    if (by_stream != by_dom) {
      std::fprintf(stderr,
          "mode=%d: the stream said %s and the DOM loop said %s\n", (int)mode,
          by_stream ? "yes" : "no", by_dom ? "yes" : "no");
      fail("the two record readers disagree about this input",
          std::string(text, len));
    }
    if (by_stream && stream_records != dom_records) {
      std::fprintf(stderr,
          "mode=%d: stream counted %zu, DOM loop counted %zu\n", (int)mode,
          stream_records, dom_records);
      fail("the two record readers disagree about how many records this holds",
          std::string(text, len));
    }
  }

  /* **And the stream must agree with itself at every chunk size.** One byte at
     a time is where a separator gets split; it is quadratic, so it is spent on
     the short inputs, which is where a minimised corpus keeps them. */
  for (size_t chunk : {size_t{1}, size_t{2}, size_t{7}}) {
    if (chunk == 1 && len > 96)
      continue;
    if (chunk >= len)
      continue;
    size_t n = 0;
    if (stream_reads(text, len, &opts, chunk, &n) != by_stream) {
      std::fprintf(
          stderr, "mode=%d chunk=%zu changed the verdict\n", (int)mode, chunk);
      fail("the answer depended on where the caller's chunk boundary fell",
          std::string(text, len));
    }
    if (by_stream && n != stream_records) {
      std::fprintf(stderr, "mode=%d chunk=%zu: %zu records, not %zu\n",
          (int)mode, chunk, n, stream_records);
      fail("the record count depended on the chunk size",
          std::string(text, len));
    }
  }

  /* **The round trip, the other way round.** Whatever this input turned out to
     hold, write those records back with the same framing and read them again:
     a mode whose writer and reader disagree is a format this library can emit
     and not consume. Only for a sequence that read cleanly, since there is
     nothing to write otherwise. */
  if (by_stream && mode != GTEXT_JSON_RECORDS_OFF && dom_records > 0) {
    /* dom_records is set only in the branch above that computed it. */
    GTEXT_JSON_Write_Options wo = gtext_json_write_options_default();
    wo.records = mode;

    GTEXT_JSON_Sink sink;
    if (gtext_json_sink_buffer(&sink) == GTEXT_JSON_OK) {
      bool wrote = true;
      size_t off = 0;
      while (off < len && wrote) {
        size_t used = 0;
        GTEXT_JSON_Error err{};
        GTEXT_JSON_Value * v = gtext_json_parse_multiple(
            text + off, len - off, &opts, &err, &used);
        gtext_json_error_free(&err);
        if (!v || used == 0) {
          if (v)
            gtext_json_free(v);
          wrote = false;
          break;
        }
        wrote = gtext_json_write_value(&sink, &wo, v, nullptr) == GTEXT_JSON_OK;
        gtext_json_free(v);
        off += used;
      }
      if (wrote) {
        const std::string out(gtext_json_sink_buffer_data(&sink),
            gtext_json_sink_buffer_size(&sink));
        size_t again = 0;
        if (!stream_reads(out.data(), out.size(), &opts, 0, &again)) {
          std::fprintf(stderr, "mode=%d: wrote %zu bytes that do not read\n",
              (int)mode, out.size());
          fail("the writer framed records its own reader refuses", out);
        }
        if (again != dom_records) {
          std::fprintf(stderr, "mode=%d: wrote %zu records, read back %zu\n",
              (int)mode, dom_records, again);
          fail("a records round trip changed how many records there were", out);
        }
      }
      gtext_json_sink_buffer_free(&sink);
    }
  }

  return 0;
}
