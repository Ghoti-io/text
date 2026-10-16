/**
 * @file
 *
 * libFuzzer harness for the XML parsers.
 *
 * The input is an options pair and then a document. The document is parsed by
 * gtext_xml_parse(), by the event parser (gtext_xml_stream_*) fed the whole input,
 * one byte at a time and in pieces, and by the pull reader, whole and a byte at a
 * time - the shape of the soak finding that two parsers sharing one grammar
 * disagreed on a document nobody had written - and the properties are the ones
 * that make those one parser:
 *
 *     they agree: the same refusal, with the same status, message and
 *     position, or the same tree, down to the location of every node (the pull
 *     reader wraps the event parser and queues its events);
 *     an accepted tree passes the tree's own invariant checker;
 *     an accepted tree written in every output encoding and read back is the
 *     same tree, and writing that tree again gives the same bytes;
 *     an allocation that fails - the first to the six-hundredth of a parse, a
 *     stream (eager or not) or a pull reader - is OOM or the answer the parse would
 *     have given, never a leak, never anything else;
 *     the event parser's work stays linear in its input, fed a byte at a time.
 *
 * **The limits come from the first four bytes and are tiny on purpose**, so that
 * one harness reaches every limit's refusal and the paths after it: a depth of
 * 1, an attribute count of 1, a name of 3 bytes. A fuzzer left to find a
 * 10,000,000-node document would not.
 *
 * **Encodings come from the document's first bytes.** A byte order mark, a
 * UTF-16 or UTF-32 `<?`, an ISO-8859-1 declaration are all sequences a
 * coverage-guided search finds by itself from the seeds (tests/fuzz/corpus/xml,
 * the W3C suite's documents, which include a UTF-16 and a UTF-32 case of each
 * kind), and the round trip above writes every accepted document in the five
 * output encodings and reads each back, which is the converse.
 *
 * Two exceptions are allowed, named in agree() and counted: the count is printed when
 * the fuzzer exits ("N refusals reordered"). The input limit: the whole-document
 * parse knows the length before it reads a byte, and the event parser knows it when
 * the bytes arrive, so it may refuse the document first for something else, at an
 * offset inside the limit (twice the limit, because an offset counts bytes of the UTF-8
 * form of a converted document); or it reports the limit. The other: when the
 * whole-document parse reports a byte that is not valid in the document's encoding,
 * it has converted the whole input first, so it may report that before a refusal
 * (well-formedness, a limit) the event parser reached earlier in the same bytes, at or
 * before that offset. Both have refused the document, and only a refusal status
 * is accepted in place of the exact answer (tools/conformance/xmlconf_suite.py has the
 * second rule, and one suite case needs it).
 *
 * Build with: make fuzz-xml      Run: make fuzz-run-xml
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
#include <ghoti.io/text/xml.h>
}

#include "../../src/markup/markup_internal.h"
#include "../../src/xml/xml_internal.h"
#include "../markup_test_support.h"
#include "../xml_replay.h"

namespace {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_xml: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
  for (unsigned char c : input) {
    if (c >= 0x20 && c < 0x7f && c != '\\') {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

// A refusal of the document, as opposed to success, running out of memory, a bad argument
// or a call out of order.
bool is_refusal(GTEXT_XML_Status s) {
  return s >= GTEXT_XML_E_SYNTAX && s <= GTEXT_XML_E_ENCODING_MISMATCH;
}

bool is_predecode_encoding_error(GTEXT_XML_Status s) {
  return s == GTEXT_XML_E_INVALID_SEQUENCE || s == GTEXT_XML_E_UTF8;
}

struct Run {
  GTEXT_XML_Status status = GTEXT_XML_OK;
  GTEXT_XML_Error err{};
  xml_replay replay;
  size_t work = 0;
  Run() {
    xml_replay_init(&replay);
  }
  ~Run() {
    gtext_xml_error_free(&err);
    xml_replay_free(&replay);
  }
};

// The event parser over `data`, in pieces of `chunk` bytes (0 = all at once).
void stream(Run & run, const std::string & data, size_t chunk, bool eager,
    const GTEXT_XML_Parse_Options * opts) {
  GTEXT_XML_Stream * s = gtext_xml_stream_new(opts, xml_replay_event, &run.replay);
  if (!s) {
    run.status = GTEXT_XML_E_OOM;
    return;
  }
  xml_stream_set_eager(s, eager);
  size_t at = 0;
  run.status = GTEXT_XML_OK;
  if (data.empty()) {
    run.status = gtext_xml_stream_feed(s, nullptr, 0, &run.err);
  }
  while (at < data.size() && run.status == GTEXT_XML_OK) {
    size_t n = chunk == 0 ? data.size() : chunk;
    if (n > data.size() - at) {
      n = data.size() - at;
    }
    run.status = gtext_xml_stream_feed(s, data.data() + at, n, &run.err);
    at += n;
  }
  if (run.status == GTEXT_XML_OK) {
    run.status = gtext_xml_stream_finish(s, &run.err);
  }
  run.work = xml_stream_work(s);
  gtext_xml_stream_free(s);
}

// The pull reader over `data`, in pieces of `chunk` bytes (0 = all at once), drained
// after every piece. The events go through the same replay as the push parser's.
void pull(Run & run, const std::string & data, size_t chunk, const GTEXT_XML_Parse_Options * opts) {
  GTEXT_XML_Reader * r = gtext_xml_reader_new(opts);
  if (!r) {
    run.status = GTEXT_XML_E_OOM;
    return;
  }
  auto drain = [&]() {
    for (;;) {
      GTEXT_XML_Event e;
      const GTEXT_XML_Status st = gtext_xml_reader_next(r, &e);
      if (st == GTEXT_XML_OK) {
        if (xml_replay_event(&e, &run.replay) != GTEXT_XML_OK) {
          return;
        }
        continue;
      }
      return; // INCOMPLETE, the end, or the error the feed already reported
    }
  };
  size_t at = 0;
  run.status = GTEXT_XML_OK;
  while (at < data.size() && run.status == GTEXT_XML_OK) {
    size_t n = chunk == 0 ? data.size() : chunk;
    if (n > data.size() - at) {
      n = data.size() - at;
    }
    run.status = gtext_xml_reader_feed(r, data.data() + at, n, &run.err);
    at += n;
    drain();
  }
  if (run.status == GTEXT_XML_OK) {
    run.status = gtext_xml_reader_feed(r, nullptr, 0, &run.err);
    drain();
  }
  gtext_xml_reader_free(r);
}

std::string status_line(GTEXT_XML_Status st, const GTEXT_XML_Error & e) {
  return std::string(gtext_xml_status_string(st)) + " at " + std::to_string(e.byte_offset) + ":"
      + std::to_string(e.line) + ":" + std::to_string(e.column) + " "
      + (e.message ? e.message : "?");
}

int g_reordered = 0; // refusals where the allowance in the header applied

void report_reordered() {
  std::fprintf(stderr, "fuzz_xml: %d refusals reordered (the allowances in the file header)\n", g_reordered);
}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  std::atexit(report_reordered);
  return 0;
}

// The input limit counts bytes fed; an error offset counts bytes of the UTF-8 form, which is up
// to twice as many as the bytes of a Latin-1 document and fewer than that for the others.
size_t offsets_within(size_t input_limit) {
  return input_limit > SIZE_MAX / 2 ? SIZE_MAX : input_limit * 2;
}

// Whether `run`'s status is the DOM parse's, or one of the two allowed to stand in for it.
bool status_allowed(GTEXT_XML_Status dom_status, const Run & run, size_t input_limit) {
  if (run.status == dom_status) {
    return true;
  }
  if (is_predecode_encoding_error(dom_status)) {
    return is_refusal(run.status);
  }
  if (dom_status == GTEXT_XML_E_LIMIT_INPUT) {
    return is_refusal(run.status) && run.err.byte_offset < offsets_within(input_limit);
  }
  return false;
}

// The event parser says what the DOM parse says.
void agree(const char * how, const std::string & input, GTEXT_XML_Status dom_status,
    const GTEXT_XML_Error & dom_err, const GTEXT_Markup_Document * dom, Run & run, size_t input_limit) {
  if (run.status == GTEXT_XML_E_OOM) {
    fail("the event parser ran out of memory with the default allocator", how, input);
  }
  if (dom_status != GTEXT_XML_OK) {
    if (run.status == dom_status && run.err.code == dom_err.code
        && run.err.byte_offset == dom_err.byte_offset && run.err.line == dom_err.line
        && run.err.column == dom_err.column
        && std::string(run.err.message ? run.err.message : "")
            == std::string(dom_err.message ? dom_err.message : "")) {
      return;
    }
    if (is_predecode_encoding_error(dom_status) && is_refusal(run.status)
        && run.err.byte_offset <= dom_err.byte_offset) {
      g_reordered++;
      return;
    }
    // The input limit: the DOM parse knows the length before it reads a byte; the
    // event parser knows it when the bytes arrive, and refuses the document for
    // something else if something else comes first. Both have refused it.
    if (dom_status == GTEXT_XML_E_LIMIT_INPUT
        && (run.status == GTEXT_XML_E_LIMIT_INPUT
            || (is_refusal(run.status) && run.err.byte_offset < offsets_within(input_limit)))) {
      g_reordered++;
      return;
    }
    fail("the event parser and the DOM parse refused differently",
        std::string(how) + "\n  dom:    " + status_line(dom_status, dom_err) + "\n  events: "
            + status_line(run.status, run.err),
        input);
  }
  if (run.status != GTEXT_XML_OK) {
    fail("the DOM parse accepted and the event parser refused",
        std::string(how) + "\n  events: " + status_line(run.status, run.err), input);
  }
  if (run.replay.problem || !run.replay.doc || !run.replay.ended) {
    fail("the events do not make a document", std::string(how) + ": "
        + (run.replay.problem ? run.replay.problem : "no document or no end"), input);
  }
  const char * difference = xml_replay_compare(run.replay.doc, dom);
  if (difference) {
    fail("the events build a different tree from the DOM parse", std::string(how) + ": " + difference, input);
  }
  const char * problem = markup_check_invariants(run.replay.doc);
  if (problem) {
    fail("the replayed tree breaks an invariant", std::string(how) + ": " + problem, input);
  }
}

const GTEXT_XML_Output_Encoding kEncodings[] = {GTEXT_XML_OUTPUT_UTF8, GTEXT_XML_OUTPUT_UTF16LE,
    GTEXT_XML_OUTPUT_UTF16BE, GTEXT_XML_OUTPUT_ISO_8859_1, GTEXT_XML_OUTPUT_US_ASCII};
const char * kEncodingNames[] = {"utf-8", "utf-16le", "utf-16be", "iso-8859-1", "us-ascii"};

// An accepted document, written in every encoding and read back.
void round_trip(GTEXT_Markup_Document * doc, const std::string & input) {
  GTEXT_Markup_Standalone standalone;
  const char * version = gtext_markup_document_xml_version(doc, &standalone);
  if (version && std::strcmp(version, "1.1") == 0) {
    // XML 1.1 reading is covered here, but XML 1.1 writing belongs to a later
    // story. Keep the parser/event/tree properties without requiring a writer
    // profile this library does not implement yet.
    return;
  }
  for (int i = 0; i < 5; i++) {
    GTEXT_XML_Write_Options o = gtext_xml_write_options_default();
    o.encoding = kEncodings[i];
    GTEXT_XML_Write_Result r;
    char * out = nullptr;
    size_t out_len = 0;
    GTEXT_XML_Status st = gtext_xml_write_buffer(doc, &o, &out, &out_len, &r);
    if (st == GTEXT_XML_E_UNREPRESENTABLE && i >= 3) {
      continue; // a character the encoding lacks, in a name or a comment
    }
    if (st != GTEXT_XML_OK) {
      fail("a parsed document cannot be written", std::string(kEncodingNames[i]) + ": "
          + (r.message ? r.message : "?") + " at " + r.path, input);
    }
    GTEXT_Markup_Document * back = nullptr;
    GTEXT_XML_Error err;
    st = gtext_xml_parse(out, out_len, nullptr, &back, &err);
    if (st != GTEXT_XML_OK) {
      std::string why = status_line(st, err);
      gtext_xml_write_buffer_free(doc, &o, out);
      fail("what the writer wrote does not parse", std::string(kEncodingNames[i]) + ": " + why, input);
    }
    if (!gtext_markup_equal(gtext_markup_document_root(doc), gtext_markup_document_root(back))) {
      gtext_markup_document_free(back);
      gtext_xml_write_buffer_free(doc, &o, out);
      fail("what the writer wrote parses to a different tree", kEncodingNames[i], input);
    }
    char * again = nullptr;
    size_t again_len = 0;
    st = gtext_xml_write_buffer(back, &o, &again, &again_len, &r);
    if (st != GTEXT_XML_OK || again_len != out_len || std::memcmp(again, out, out_len) != 0) {
      if (again) {
        gtext_xml_write_buffer_free(back, &o, again);
      }
      gtext_markup_document_free(back);
      gtext_xml_write_buffer_free(doc, &o, out);
      fail("writing what was read is not the same bytes", kEncodingNames[i], input);
    }
    gtext_xml_write_buffer_free(back, &o, again);
    gtext_markup_document_free(back);
    gtext_xml_write_buffer_free(doc, &o, out);
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 4) {
    return 0;
  }
  // Four option bytes, then the document. Each limit has an enable bit and a two-bit index into a
  // table of values that are tiny on purpose, so no limit's value is out of reach:
  //   o1  bits 0-1 depth, 2 its enable, 3-4 attributes, 5 its enable, 6 the byte-at-a-time stream
  //       is eager, 7 the stream in pieces is eager
  //   o2  bits 0-1 attribute value bytes, 2 enable, 3-4 name bytes, 5 enable, 6 the pull reader
  //       a byte at a time, 7 the allocator-failure runs
  //   o3  bits 0-1 nodes, 2 enable, 3-4 namespaces in scope, 5 enable, 6 an input limit
  //   o4  bits 0-2 the size of the pieces (2 to 7), bits 3-7 the input limit: the document's length minus 16 plus this number
  const uint8_t o1 = data[0];
  const uint8_t o2 = data[1];
  const uint8_t o3 = data[2];
  const uint8_t o4 = data[3];
  const std::string input(reinterpret_cast<const char *>(data) + 4, size - 4);

  static const size_t depths[] = {1, 2, 3, 5};
  static const size_t attrs[] = {1, 2, 3, 5};
  static const size_t values[] = {1, 4, 16, 64};
  static const size_t names[] = {1, 3, 8, 32};
  static const size_t nodes[] = {3, 6, 20, 100};
  static const size_t spaces[] = {1, 2, 3, 5};
  GTEXT_XML_Parse_Options opts = gtext_xml_parse_options_default();
  opts.max_depth = (o1 & 0x04) ? depths[o1 & 3] : 0;
  opts.max_attributes = (o1 & 0x20) ? attrs[(o1 >> 3) & 3] : 0;
  opts.max_attribute_bytes = (o2 & 0x04) ? values[o2 & 3] : 0;
  opts.max_name_bytes = (o2 & 0x20) ? names[(o2 >> 3) & 3] : 0;
  opts.max_nodes = (o3 & 0x04) ? nodes[o3 & 3] : 0;
  opts.max_namespaces = (o3 & 0x20) ? spaces[(o3 >> 3) & 3] : 0;
  const bool tiny_limits = (o1 & 0x24) || (o2 & 0x24) || (o3 & 0x64);
  if (o3 & 0x40) {
    // Near the document's own length, from 16 under to 15 over, so that both sides are reached.
    const size_t around = input.size() + static_cast<size_t>(o4 >> 3);
    opts.max_input_bytes = around > 16 ? around - 16 : 1;
  }
  const size_t k = 2 + (o4 & 7) % 6; // the size of the pieces of the third stream

  // ---- the three parses, which must be one parser --------------------------
  GTEXT_Markup_Document * dom = nullptr;
  GTEXT_XML_Error dom_err;
  const GTEXT_XML_Status dom_status = gtext_xml_parse(input.data(), input.size(), &opts, &dom, &dom_err);
  if (dom_status == GTEXT_XML_OK) {
    const char * problem = markup_check_invariants(dom);
    if (problem) {
      fail("the parsed tree breaks an invariant", problem, input);
    }
  }
  else if (dom) {
    fail("a refused parse wrote its output", "", input);
  }
  {
    Run whole;
    stream(whole, input, 0, false, &opts);
    agree("whole input", input, dom_status, dom_err, dom, whole, opts.max_input_bytes);
    Run bytes;
    stream(bytes, input, 1, (o1 & 0x40) != 0, &opts);
    agree((o1 & 0x40) ? "one byte at a time, eager" : "one byte at a time", input, dom_status, dom_err, dom, bytes, opts.max_input_bytes);
    Run pieces;
    stream(pieces, input, k, (o1 & 0x80) != 0, &opts);
    agree("pieces", input, dom_status, dom_err, dom, pieces, opts.max_input_bytes);
    Run pulled;
    pull(pulled, input, (o2 & 0x40) ? 1 : 0, &opts);
    agree((o2 & 0x40) ? "the pull reader, a byte at a time" : "the pull reader, whole", input, dom_status, dom_err, dom, pulled, opts.max_input_bytes);
    // Fed a byte at a time the work stays linear: a construct re-scanned from
    // its start at every byte is quadratic, and a long enough input is a denial
    // of service. The constant is far above anything measured (about 12).
    if (bytes.work > 64 * input.size() + 4096) {
      fail("the event parser's work is not linear in its input", std::to_string(bytes.work)
          + " units for " + std::to_string(input.size()) + " bytes", input);
    }
  }

  // ---- encodings: what was accepted is written in five and read back --------
  if (dom_status == GTEXT_XML_OK && !tiny_limits) {
    round_trip(dom, input);
  }

  // ---- an allocation that fails ---------------------------------------------
  // With whatever limits this input chose (the answer to compare with is the one above, got
  // with the same limits), and failing the Nth allocation for N across the first six hundred,
  // which reaches the growth of the larger tables as well as the first few blocks.
  if (o2 & 0x80) {
    const size_t point = 1 + ((static_cast<size_t>(o1) << 16 | static_cast<size_t>(o2) << 8 | o3) * 2654435761u + input.size()) % 600;
    FaultAllocator fa;
    GTEXT_XML_Parse_Options fo = opts;
    fo.allocator = &fa.alloc;
    fa.arm(point);
    GTEXT_Markup_Document * fdoc = nullptr;
    GTEXT_XML_Error ferr;
    const GTEXT_XML_Status st = gtext_xml_parse(input.data(), input.size(), &fo, &fdoc, &ferr);
    const bool failed = fa.failed;
    fa.disarm();
    if (st == GTEXT_XML_E_OOM) {
      if (!failed) {
        fail("OOM reported with no allocation failed", "dom", input);
      }
      if (fdoc) {
        fail("an OOM parse wrote its output", "", input);
      }
    }
    else {
      if (st != dom_status) {
        fail("an allocation failure changed the answer", std::string(gtext_xml_status_string(st))
            + " against " + gtext_xml_status_string(dom_status), input);
      }
      if (st == GTEXT_XML_OK && fdoc && !gtext_markup_equal(gtext_markup_document_root(fdoc),
              gtext_markup_document_root(dom))) {
        fail("an allocation failure changed the tree", "", input);
      }
    }
    if (fdoc) {
      gtext_markup_document_free(fdoc);
    }
    gtext_xml_error_free(&ferr);
    if (!fa.live.empty() || fa.bad_frees != 0) {
      fail("the parse leaked or double-freed", std::to_string(fa.live.size()) + " blocks, "
          + std::to_string(fa.bad_frees) + " bad frees", input);
    }

    // The same for the event parser (fed as the harness's first stream was, eager or not) and
    // for the pull reader: OOM or the normal answer, and no leak.
    for (int which = 0; which < 2; which++) {
      FaultAllocator sa;
      GTEXT_XML_Parse_Options so = opts;
      so.allocator = &sa.alloc;
      sa.arm(1 + (point * (which + 3) + o1) % 600);
      {
        Run run;
        if (which == 0) {
          stream(run, input, (o2 & 0x40) ? 1 : 0, (o1 & 0x40) != 0, &so);
        }
        else {
          pull(run, input, (o2 & 0x40) ? 1 : 0, &so);
        }
        const bool sfailed = sa.failed;
        sa.disarm();
        if (run.status == GTEXT_XML_E_OOM) {
          if (!sfailed) {
            fail("OOM reported with no allocation failed", which ? "pull reader" : "stream", input);
          }
        }
        else if (!status_allowed(dom_status, run, opts.max_input_bytes)) {
          fail("an allocation failure changed the answer of the event parser or the pull reader",
              std::string(gtext_xml_status_string(run.status)) + " against " + gtext_xml_status_string(dom_status),
              input);
        }
      }
      if (!sa.live.empty() || sa.bad_frees != 0) {
        fail("the event parser or the pull reader leaked or double-freed", std::to_string(sa.live.size())
            + " blocks, " + std::to_string(sa.bad_frees) + " bad frees", input);
      }
    }
  }

  if (dom) {
    gtext_markup_document_free(dom);
  }
  gtext_xml_error_free(&dom_err);
  return 0;
}
