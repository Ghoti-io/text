/**
 * @file
 *
 * libFuzzer harness for the JSON *incremental* writer.
 *
 * fuzz_json.cpp writes every document it parsed, so `gtext_json_write_value()`
 * is well covered. `gtext_json_writer_*` was not reached by anything: a corpus
 * of JSON text drives a parse, and the incremental API is a sequence of calls
 * no document can produce.
 *
 * That gap had a cost. An object of two or more members written through this
 * API emitted
 *
 *     {"a":1,"b":,2}
 *
 * which is not JSON - every value kind, every nesting - and each call returned
 * GTEXT_JSON_OK, including gtext_json_writer_finish(). The one test over that
 * shape asserted that three substrings appeared in the output, which they do.
 * This harness would have found it on the first object with two members.
 *
 * So the input is not a document here; it is a **program**. Each byte selects a
 * writer call, and the structural rules the writer enforces are what keep the
 * program legal: a value where a key belongs is refused with
 * GTEXT_JSON_E_STATE, and the harness honours that by not counting a refusal
 * as output.
 *
 * The property:
 *
 *     if every call returned OK and finish() returned OK,
 *     the bytes written must parse,
 *     and must hold the structure the calls described.
 *
 * "The structure the calls described" is a shadow document built with the DOM
 * API from the same byte stream, written out with gtext_json_write_value(), and
 * compared. Two writers for one specification, so the differential is free -
 * and that comparison is what the option matrix in
 * tests/test-writer-agreement.cpp does over nine options; here it runs over
 * shapes nobody chose.
 *
 * Build with: make fuzz-json-writer     Run: make fuzz-run-json-writer
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

/** Abort with the bytes that caused it, so a crash names its own input. */
[[noreturn]] void fail(const char * what, const std::string & out) {
  std::fprintf(stderr, "%s\noutput was (%zu bytes): ", what, out.size());
  for (unsigned char c : out) {
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

/** What the next opcode asks for. */
enum Op {
  OP_OBJECT_BEGIN,
  OP_OBJECT_END,
  OP_ARRAY_BEGIN,
  OP_ARRAY_END,
  OP_KEY,
  OP_NULL,
  OP_BOOL,
  OP_I64,
  OP_U64,
  OP_DOUBLE,
  OP_STRING,
  OP_COUNT
};

/** The shadow DOM, built from the same opcodes so the two can be compared. */
struct Shadow {
  // A stack of containers under construction. The root is index 0 once set.
  std::vector<GTEXT_JSON_Value *> stack;
  GTEXT_JSON_Value * root = nullptr;
  std::string pending_key;
  bool have_pending_key = false;

  /** Attach @p v where the current position says it goes. */
  bool place(GTEXT_JSON_Value * v) {
    if (!v) return false;
    if (stack.empty()) {
      if (root) {
        gtext_json_free(v);
        return false; // a second root; the writer refuses this too
      }
      root = v;
      return true;
    }
    GTEXT_JSON_Value * top = stack.back();
    if (gtext_json_typeof(top) == GTEXT_JSON_OBJECT) {
      if (!have_pending_key) {
        gtext_json_free(v);
        return false;
      }
      const bool ok = gtext_json_object_put(
                          top, pending_key.data(), pending_key.size(), v)
          == GTEXT_JSON_OK;
      have_pending_key = false;
      pending_key.clear();
      return ok;
    }
    return gtext_json_array_push(top, v) == GTEXT_JSON_OK;
  }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 1) return 0;

  // The first byte picks the write options, so one corpus unit explores one
  // formatting. The four options the incremental writer cannot honour are left
  // out deliberately: they would make the differential below fail for a
  // documented reason rather than a defect. gtext_json_writer_new()'s header
  // names them, and test-writer-agreement.cpp asserts the list.
  const uint8_t flags = data[0];
  GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
  opts.pretty = (flags & 0x01) != 0;
  opts.indent_spaces = (flags & 0x02) ? 4 : 2;
  opts.trailing_newline = (flags & 0x04) != 0;
  opts.space_after_colon = (flags & 0x08) != 0;
  opts.space_after_comma = (flags & 0x10) != 0;
  opts.escape_solidus = (flags & 0x20) != 0;
  opts.escape_unicode = (flags & 0x40) != 0;
  opts.escape_all_non_ascii = (flags & 0x80) != 0;

  GTEXT_JSON_Sink sink;
  if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) return 0;
  GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, &opts);
  if (!w) {
    gtext_json_sink_buffer_free(&sink);
    return 0;
  }

  Shadow shadow;
  bool refused = false; // a call the writer rejected: stop comparing

  size_t i = 1;
  while (i < size && !refused) {
    const uint8_t op = data[i++] % OP_COUNT;
    GTEXT_JSON_Status st = GTEXT_JSON_OK;

    switch (op) {
    case OP_OBJECT_BEGIN: {
      st = gtext_json_writer_object_begin(w);
      if (st == GTEXT_JSON_OK) {
        GTEXT_JSON_Value * o = gtext_json_new_object();
        if (!shadow.place(o)) refused = true;
        else shadow.stack.push_back(o);
      }
      break;
    }
    case OP_OBJECT_END:
      st = gtext_json_writer_object_end(w);
      if (st == GTEXT_JSON_OK) {
        if (shadow.stack.empty()) refused = true;
        else shadow.stack.pop_back();
      }
      break;
    case OP_ARRAY_BEGIN: {
      st = gtext_json_writer_array_begin(w);
      if (st == GTEXT_JSON_OK) {
        GTEXT_JSON_Value * a = gtext_json_new_array();
        if (!shadow.place(a)) refused = true;
        else shadow.stack.push_back(a);
      }
      break;
    }
    case OP_ARRAY_END:
      st = gtext_json_writer_array_end(w);
      if (st == GTEXT_JSON_OK) {
        if (shadow.stack.empty()) refused = true;
        else shadow.stack.pop_back();
      }
      break;
    case OP_KEY: {
      // A length byte then the bytes, so a key can hold anything including a
      // quote, a backslash, a control character or invalid UTF-8 - the escaping
      // is part of what is under test.
      const size_t len = (i < size) ? (data[i++] % 8u) : 0u;
      const size_t have = (size - i < len) ? (size - i) : len;
      const std::string key((const char *)(data + i), have);
      i += have;
      st = gtext_json_writer_key(w, key.data(), key.size());
      if (st == GTEXT_JSON_OK) {
        shadow.pending_key = key;
        shadow.have_pending_key = true;
      }
      break;
    }
    case OP_NULL:
      st = gtext_json_writer_null(w);
      if (st == GTEXT_JSON_OK && !shadow.place(gtext_json_new_null())) {
        refused = true;
      }
      break;
    case OP_BOOL: {
      const bool b = (i < size) ? (data[i++] & 1) != 0 : false;
      st = gtext_json_writer_bool(w, b);
      if (st == GTEXT_JSON_OK && !shadow.place(gtext_json_new_bool(b))) {
        refused = true;
      }
      break;
    }
    case OP_I64: {
      int64_t v = 0;
      for (int k = 0; k < 8 && i < size; k++) {
        v = (int64_t)(((uint64_t)v << 8) | data[i++]);
      }
      st = gtext_json_writer_number_i64(w, v);
      if (st == GTEXT_JSON_OK && !shadow.place(gtext_json_new_number_i64(v))) {
        refused = true;
      }
      break;
    }
    case OP_U64: {
      uint64_t v = 0;
      for (int k = 0; k < 8 && i < size; k++) {
        v = (v << 8) | data[i++];
      }
      st = gtext_json_writer_number_u64(w, v);
      if (st == GTEXT_JSON_OK && !shadow.place(gtext_json_new_number_u64(v))) {
        refused = true;
      }
      break;
    }
    case OP_DOUBLE: {
      // From bytes, so NaN and the infinities are reachable - which the writer
      // has a policy for and the DOM builder has to agree with.
      double d = 0.0;
      uint64_t bits = 0;
      for (int k = 0; k < 8 && i < size; k++) {
        bits = (bits << 8) | data[i++];
      }
      std::memcpy(&d, &bits, sizeof d);
      st = gtext_json_writer_number_double(w, d);
      if (st == GTEXT_JSON_OK
          && !shadow.place(gtext_json_new_number_double(d))) {
        refused = true;
      }
      break;
    }
    case OP_STRING: {
      const size_t len = (i < size) ? (data[i++] % 16u) : 0u;
      const size_t have = (size - i < len) ? (size - i) : len;
      const std::string sv((const char *)(data + i), have);
      i += have;
      st = gtext_json_writer_string(w, sv.data(), sv.size());
      if (st == GTEXT_JSON_OK
          && !shadow.place(gtext_json_new_string(sv.data(), sv.size()))) {
        refused = true;
      }
      break;
    }
    default:
      break;
    }

    // A refusal is the writer enforcing its own structural rules, which is
    // correct behaviour and not something to compare against. Everything after
    // it is unconstrained, so the comparison stops here.
    if (st != GTEXT_JSON_OK) refused = true;
  }

  const bool finished =
      !refused && gtext_json_writer_finish(w, nullptr) == GTEXT_JSON_OK;
  const std::string out(
      gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));

  // Only a complete, unrefused program makes a claim about the bytes. A program
  // that left a container open wrote a prefix, which is not a document.
  if (finished && shadow.stack.empty() && shadow.root && !out.empty()) {
    GTEXT_JSON_Value * back =
        gtext_json_parse(out.data(), out.size(), nullptr, nullptr);
    if (!back) {
      fail("the incremental writer reported OK and wrote bytes that do not "
           "parse",
          out);
    }

    // The differential: the same opcodes through the DOM and the value writer.
    // Compared as canonical text, because that is what makes two
    // representations of one document comparable at all.
    GTEXT_JSON_Write_Options canon = gtext_json_write_options_default();
    canon.sort_object_keys = true;
    GTEXT_JSON_Sink s1;
    GTEXT_JSON_Sink s2;
    if (gtext_json_sink_buffer(&s1) == GTEXT_JSON_OK) {
      if (gtext_json_sink_buffer(&s2) == GTEXT_JSON_OK) {
        const bool a = gtext_json_write_value(&s1, &canon, shadow.root, nullptr)
            == GTEXT_JSON_OK;
        const bool b = gtext_json_write_value(&s2, &canon, back, nullptr)
            == GTEXT_JSON_OK;
        if (a && b) {
          const std::string from_dom(
              gtext_json_sink_buffer_data(&s1), gtext_json_sink_buffer_size(&s1));
          const std::string from_round(
              gtext_json_sink_buffer_data(&s2), gtext_json_sink_buffer_size(&s2));
          if (from_dom != from_round) {
            std::fprintf(stderr, "DOM says:   %s\nround says: %s\n",
                from_dom.c_str(), from_round.c_str());
            fail("the incremental writer wrote a different document from the "
                 "one its calls described",
                out);
          }
        }
        gtext_json_sink_buffer_free(&s2);
      }
      gtext_json_sink_buffer_free(&s1);
    }
    gtext_json_free(back);
  }

  if (shadow.root) gtext_json_free(shadow.root);
  gtext_json_writer_free(w);
  gtext_json_sink_buffer_free(&sink);
  return 0;
}
