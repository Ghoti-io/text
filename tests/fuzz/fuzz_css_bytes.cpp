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
 * libFuzzer harness for CSS from bytes (src/css/css_decode.c, css_stream.c): the
 * stylesheet decode of CSS Syntax 3.2, the byte entry point, and the incremental
 * parser, through the public API.
 *
 * The input is four option bytes and then the stylesheet's bytes:
 *
 *   o0  bits 0-1 the protocol's label (none, windows-1251, utf-16le, shift_jis),
 *       bits 2-3 the environment encoding (none, windows-1252, ISO-8859-2,
 *       gb18030), bit 4 WTF-8 input, bit 5 a text stream rather than a byte
 *       one, bit 6 a validity callback that refuses some declarations and rules,
 *       bit 7 count_errors_only;
 *   o1  a limit made tiny: bits 0-2 which (none, depth, tokens, rules,
 *       declarations, parse errors, input bytes, none), bits 3-7 its value
 *       (1 to 32);
 *   o2  the allocation to fail (0 for none);
 *   o3  the way to cut the input for the stream (the generator of the pieces).
 *
 * The properties, each a requirement of css_decode.h or css_stream.h:
 *
 *   the parse of the bytes succeeds with the default limits (or is the depth
 *   limit's, as css_fuzz_syntax allows), its tree is well formed against the
 *   bytes (ids, spans inside them, strings), the parse errors are in order and
 *   inside the bytes, and it says which encoding it read and why: the same as
 *   gtext_css_determine_encoding(), a byte order mark of 2 or 3 bytes only
 *   where the encoding is the mark's;
 *   **the positions are in the bytes**: the stylesheet decoded by the Encoding
 *   module (a byte at a time, with its source map) and parsed as text has the
 *   same tree, errors, lines and columns, and every span and error offset of
 *   the byte parse is the offset the map gives for the same place of the text
 *   (for UTF-8, the text's offset plus the byte order mark); sibling spans are
 *   in order;
 *   **the same answer at every cut**: a stream given the bytes one at a time, in
 *   random pieces with empty feeds between them, and whole, gives the same
 *   status, the same tree to every field (spans and identities included), the
 *   same serialization, the same errors with their places, the same failure and
 *   the same encoding as the one-call parse; so does a text stream for the text;
 *   each limit is exact: the input limit at the length and one below it (the
 *   feed that goes past it is the stream's failure, and finish says so), and
 *   under the tiny limit of o1 the stream and the one-call parse agree;
 *   an allocation that fails is E_OOM with no tree, or the same answer, for the
 *   one-call parse and for the stream, and never a leak or a free of a block
 *   the allocator did not make;
 *   the work is linear: at most 64 units per byte, plus 4096, for the one-call
 *   parse and (a byte at a time, with the calls counted) 96 per byte for the
 *   stream.
 *
 * Seeds: tests/fuzz/corpus/css-bytes, made by tools/css/make-fuzz-seeds.py.
 * Build with: make fuzz-css-bytes      Run: make fuzz-run-css-bytes
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The syntax harness's checks of a tree, its failure report and its helpers are this one's; its own entry
// point is renamed out of the way.
// (push and pop, because the replay test renames this harness's own entry point the same way.)
#pragma push_macro("LLVMFuzzerTestOneInput")
#undef LLVMFuzzerTestOneInput
#define LLVMFuzzerTestOneInput css_syntax_entry_not_this_harness
#include "fuzz_css_syntax.cpp"
#undef LLVMFuzzerTestOneInput
#pragma pop_macro("LLVMFuzzerTestOneInput")

#include <ghoti.io/text/encoding/encoding_decode.h>

#include "../css_tree_dump.h"

extern "C" GTEXT_CSS_Status gtext_css_parse_bytes_work(GTEXT_CSS_Tree_Kind kind, const void * bytes, size_t length,
    const GTEXT_CSS_Bytes_Options * bytes_options, const GTEXT_CSS_Parse_Options * options, size_t * work);
extern "C" GTEXT_CSS_Status gtext_css_stream_work(GTEXT_CSS_Tree_Kind kind, const void * data, size_t length, size_t chunk,
    const GTEXT_CSS_Bytes_Options * bytes_options, const GTEXT_CSS_Parse_Options * options, size_t * work);

namespace css_fuzz_bytes {

using css_fuzz::canon;
using css_fuzz::Checker;
using css_fuzz::check_errors;
using css_fuzz::fail;

const char * const kLabels[4] = {nullptr, "windows-1251", "utf-16le", "shift_jis"};
const GTEXT_Encoding kEnvironments[4] = {GTEXT_ENCODING_NONE, GTEXT_ENCODING_WINDOWS_1252, GTEXT_ENCODING_ISO_8859_2,
    GTEXT_ENCODING_GB18030};

bool refuse_decl(void *, const GTEXT_CSS_Parent *, const GTEXT_CSS_Declaration * d) {
  return !(d->name.length > 0 && d->name.data[0] == 'x');
}

bool refuse_rule(void *, const GTEXT_CSS_Parent *, const GTEXT_CSS_Rule * r) {
  for (size_t i = 0; i < r->prelude_count; i++) {
    if (r->prelude[i].type == GTEXT_CSS_TOKEN_DELIM && r->prelude[i].delim == '<') {
      return false;
    }
  }
  return true;
}

/// Everything a parse said, as text to compare.
std::string signature(GTEXT_CSS_Status st, const GTEXT_CSS_Tree * t, const GTEXT_CSS_Result & r, const std::string & input) {
  std::string s = std::string("status ") + gtext_css_status_string(st) + "\n";
  if (t) {
    s += css_test::dump_tree(t) + "\n" + css_fuzz::serialize(t, input) + "\n";
  }
  char b[256];
  for (size_t i = 0; i < gtext_css_result_error_count(&r); i++) {
    GTEXT_CSS_Error e;
    gtext_css_result_error_at(&r, i, &e);
    std::snprintf(b, sizeof b, "error %s %zu %zu:%zu\n", e.name, e.byte_offset, e.line, e.column);
    s += b;
  }
  std::snprintf(b, sizeof b, "dropped %zu failure %zu-%zu@%zu:%zu enc %d src %d bom %zu tokens %zu rules %zu decls %zu\n",
      gtext_css_result_errors_dropped(&r), r.failure.offset, r.failure.end, r.failure.line, r.failure.column, (int)r.encoding,
      (int)r.encoding_source, r.bom_length, r.token_count, r.rule_count, r.declaration_count);
  s += b;
  return s;
}

struct Outcome {
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  GTEXT_CSS_Tree * tree = nullptr;
  GTEXT_CSS_Result result{};
  GTEXT_CSS_Status fed = GTEXT_CSS_OK;
  Outcome() = default;
  Outcome(const Outcome &) = delete;
  Outcome & operator=(const Outcome &) = delete;
  ~Outcome() {
    gtext_css_tree_free(tree);
    gtext_css_result_free(&result);
  }
};

struct Settings {
  bool text_stream;
  GTEXT_CSS_Bytes_Options bytes;
  GTEXT_CSS_Parse_Options options;
};

void whole(Outcome & o, const Settings & s, const std::string & input) {
  o.status = gtext_css_parse_stylesheet_bytes(input.data(), input.size(), &s.bytes, &s.options, &o.tree, &o.result);
}

/// A stream given the input in the pieces `cut(i)` returns (0 is an empty feed).
template <class Cut>
void streamed(Outcome & o, const Settings & s, const std::string & input, Cut cut) {
  GTEXT_CSS_Stream * stream = nullptr;
  const GTEXT_CSS_Status ns = gtext_css_stream_new_bytes(GTEXT_CSS_TREE_STYLESHEET, &s.bytes, &s.options, &stream);
  if (ns != GTEXT_CSS_OK) {
    o.status = ns;
    return;
  }
  size_t at = 0, i = 0;
  while (at < input.size()) {
    size_t take = cut(i++);
    if (take > input.size() - at) {
      take = input.size() - at;
    }
    const GTEXT_CSS_Status fs = gtext_css_stream_feed(stream, input.data() + at, take);
    if (fs != GTEXT_CSS_OK && o.fed == GTEXT_CSS_OK) {
      o.fed = fs;
    }
    at += take;
  }
  o.status = gtext_css_stream_finish(stream, &o.tree, &o.result);
  gtext_css_stream_free(stream);
}

// ---- positions ------------------------------------------------------------------------------------------

/// The stylesheet decoded by the Encoding module a byte at a time: the text, and for each code point where its bytes were.
struct Decoded {
  std::string text;
  std::vector<size_t> starts, ends;
  bool ok = false;
};

bool collect(void * user, const char * bytes, size_t len, const GTEXT_Encoding_Source_Map * map) {
  Decoded * d = static_cast<Decoded *>(user);
  d->text.append(bytes, len);
  size_t i = 0;
  while (i < len) {
    const unsigned char c = (unsigned char)bytes[i];
    const size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
    d->starts.push_back(map->offsets[i]);
    d->ends.push_back(map->offsets[i] + map->widths[i]);
    i += n;
  }
  return true;
}

Decoded decode(GTEXT_Encoding enc, const std::string & input) {
  Decoded d;
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.bom_sniffing = true;
  o.track_offsets = true;
  o.max_output_bytes = SIZE_MAX;
  GTEXT_Decoder * dec = nullptr;
  if (gtext_decoder_new(enc, &o, &dec) != GTEXT_ENCODING_OK) {
    return d;
  }
  const GTEXT_Encoding_Mapped_Sink sink = {collect, &d};
  d.ok = true;
  for (size_t i = 0; i < input.size() && d.ok; i++) {
    d.ok = gtext_decoder_decode_mapped(dec, input.data() + i, 1, false, &sink, nullptr) == GTEXT_ENCODING_OK;
  }
  if (d.ok) {
    d.ok = gtext_decoder_decode_mapped(dec, "", 0, true, &sink, nullptr) == GTEXT_ENCODING_OK;
  }
  gtext_decoder_free(dec);
  return d;
}

size_t code_points_before(const std::string & text, size_t offset) {
  size_t n = 0;
  for (size_t i = 0; i < offset && i < text.size(); i++) {
    n += ((unsigned char)text[i] & 0xC0) != 0x80;
  }
  return n;
}

/// Sibling values are in order, and a value holds its children.
void check_values(const GTEXT_CSS_Component_Value * v, size_t n, const std::string & input) {
  size_t last = 0;
  for (size_t i = 0; i < n; i++) {
    if (v[i].span.offset < last) {
      fail("sibling spans out of order in the bytes", std::to_string(v[i].span.offset) + " after " + std::to_string(last), input);
    }
    last = v[i].span.end;
    for (size_t k = 0; k < v[i].child_count; k++) {
      if (v[i].children[k].span.offset < v[i].span.offset || v[i].children[k].span.end > v[i].span.end) {
        fail("a child's span is outside its parent's", "", input);
      }
    }
    check_values(v[i].children, v[i].child_count, input);
  }
}

void check_rules(const GTEXT_CSS_Rule * r, size_t n, const std::string & input) {
  size_t last = 0;
  for (size_t i = 0; i < n; i++) {
    const bool bad_prelude = r[i].prelude_count > 0
        && (r[i].prelude_span.offset < r[i].span.offset || r[i].prelude_span.end > r[i].span.end);
    if (r[i].span.offset < last || bad_prelude) {
      fail("rule spans out of order in the bytes", "", input);
    }
    last = r[i].span.end;
    check_values(r[i].prelude, r[i].prelude_count, input);
    for (size_t k = 0; k < r[i].declaration_count; k++) {
      check_values(r[i].declarations[k].value, r[i].declarations[k].value_count, input);
    }
    check_rules(r[i].rules, r[i].rule_count, input);
  }
}

// ---- the entry --------------------------------------------------------------------------------------------

int entry(const uint8_t * data, size_t size) {
  if (size < 4) {
    return 0;
  }
  const uint8_t o0 = data[0], o1 = data[1], o2 = data[2], o3 = data[3];
  const std::string input(reinterpret_cast<const char *>(data) + 4, size - 4);

  Settings s;
  s.text_stream = (o0 & 0x20) != 0;
  s.bytes = gtext_css_bytes_options_default();
  s.bytes.protocol_label = kLabels[o0 & 3];
  s.bytes.environment_encoding = kEnvironments[(o0 >> 2) & 3];
  s.options = gtext_css_parse_options_default();
  s.options.input_wtf8 = (o0 & 0x10) != 0;
  const bool validity = (o0 & 0x40) != 0;
  if (validity) {
    s.options.validity.declaration = refuse_decl;
    s.options.validity.rule = refuse_rule;
  }
  s.options.count_errors_only = (o0 & 0x80) != 0;

  // The text stream: the text parse's answer, however cut. The bytes are the text.
  if (s.text_stream) {
    Outcome want;
    want.status = gtext_css_parse_stylesheet(input.data(), input.size(), &s.options, &want.tree, &want.result);
    if (want.status != GTEXT_CSS_OK && want.status != GTEXT_CSS_E_LIMIT_DEPTH) {
      fail("a text parse under the default limits failed", gtext_css_status_string(want.status), input);
    }
    const std::string sig = signature(want.status, want.tree, want.result, input);
    uint64_t state = 0x9E3779B97F4A7C15ULL ^ o3 ^ ((uint64_t)size << 8);
    auto next = [&]() {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      return (size_t)(state >> 33);
    };
    for (int round = 0; round < 3; round++) {
      GTEXT_CSS_Stream * stream = nullptr;
      if (gtext_css_stream_new(GTEXT_CSS_TREE_STYLESHEET, &s.options, &stream) != GTEXT_CSS_OK) {
        fail("a text stream could not be made", "", input);
      }
      size_t at = 0;
      while (at < input.size()) {
        const size_t max_piece = round == 0 ? 1 : round == 1 ? 7 : 300;
        const size_t take = std::min(next() % (max_piece + 1), input.size() - at);
        if (gtext_css_stream_feed(stream, input.data() + at, take) != GTEXT_CSS_OK) {
          fail("a text stream refused input within the limit", "", input);
        }
        at += take;
        if (take == 0 && next() % 4 == 0 && gtext_css_stream_feed(stream, input.data() + at, 1) == GTEXT_CSS_OK) {
          at += 1;
        }
      }
      Outcome got;
      got.status = gtext_css_stream_finish(stream, &got.tree, &got.result);
      gtext_css_stream_free(stream);
      if (signature(got.status, got.tree, got.result, input) != sig) {
        fail("a text stream cut in pieces does not give the text parse's answer", "round " + std::to_string(round), input);
      }
    }
    return 0;
  }

  // The parse of the bytes with the default limits.
  Outcome good;
  whole(good, s, input);
  if (good.status == GTEXT_CSS_E_LIMIT_DEPTH) {
    if (good.tree) {
      fail("a depth limit that left a tree", "", input);
    }
    return 0;
  }
  if (good.status != GTEXT_CSS_OK || !good.tree || good.result.status != GTEXT_CSS_OK) {
    fail("a parse of bytes under the default limits failed", gtext_css_status_string(good.status), input);
  }
  GTEXT_CSS_Encoding_Decision dec;
  if (gtext_css_determine_encoding(input.data(), input.size(), &s.bytes, &dec) != GTEXT_CSS_OK) {
    fail("the encoding could not be decided", "", input);
  }
  if (good.result.encoding != dec.encoding || good.result.encoding_source != dec.source || good.result.bom_length != dec.bom_length
      || good.result.input_bytes != input.size()) {
    fail("the result does not say how the bytes were read", "", input);
  }
  if (dec.bom_length > 3 || (dec.bom_length != 0 && dec.source != GTEXT_CSS_ENCODING_SOURCE_BOM)
      || (dec.source == GTEXT_CSS_ENCODING_SOURCE_BOM && dec.bom_length == 0)) {
    fail("a byte order mark that is not one", "", input);
  }
  css_fuzz::Checker ck{input};
  ck.tree(good.tree);
  check_errors(good.result, input, s.options.count_errors_only ? 0 : 1024);
  check_rules(good.tree->rules, good.tree->rule_count, input);
  const std::string sig = signature(good.status, good.tree, good.result, input);
  {
    Outcome again;
    whole(again, s, input);
    if (signature(again.status, again.tree, again.result, input) != sig) {
      fail("the parse of bytes is not deterministic", "", input);
    }
  }

  // The positions are in the bytes.
  {
    GTEXT_CSS_Parse_Options to = s.options;
    Outcome text;
    std::string text_bytes;
    std::vector<size_t> starts, ends;
    bool have_map = false;
    if (dec.encoding == GTEXT_ENCODING_UTF_8) {
      text_bytes = input.substr(dec.bom_length);
    }
    else {
      Decoded d = decode(dec.encoding, input);
      if (!d.ok || code_points_before(d.text, d.text.size()) != d.starts.size()) {
        fail("the Encoding module could not decode the bytes a byte at a time", "", input);
      }
      text_bytes = d.text;
      starts = d.starts;
      ends = d.ends;
      have_map = true;
    }
    text.status = gtext_css_parse_stylesheet(text_bytes.data(), text_bytes.size(), &to, &text.tree, &text.result);
    if (text.status != GTEXT_CSS_OK) {
      fail("the decoded text did not parse as the bytes did", gtext_css_status_string(text.status), input);
    }
    if (canon(text.tree) != canon(good.tree)) {
      fail("the tree of the bytes is not the tree of their text", "", input);
    }
    auto start_of = [&](size_t d) -> size_t {
      if (d >= text_bytes.size()) {
        return input.size();
      }
      return have_map ? starts[code_points_before(text_bytes, d)] : d + dec.bom_length;
    };
    auto end_of = [&](size_t d) -> size_t {
      if (d >= text_bytes.size()) {
        return input.size();
      }
      if (d == 0) {
        return start_of(0);
      }
      return have_map ? ends[code_points_before(text_bytes, d) - 1] : d + dec.bom_length;
    };
    const std::vector<size_t> want = css_test::collect_spans(text.tree);
    const std::vector<size_t> got = css_test::collect_spans(good.tree);
    if (want.size() != got.size()) {
      fail("the trees have different numbers of spans", "", input);
    }
    for (size_t i = 0; i < want.size(); i += 2) {
      const size_t st = start_of(want[i]);
      size_t en = want[i + 1] == want[i] ? st : end_of(want[i + 1]);
      if (en < st) {
        en = st;
      }
      if (got[i] != st || got[i + 1] != en) {
        fail("a span is not at the bytes of its place in the text",
            "span " + std::to_string(i / 2) + " text " + std::to_string(want[i]) + "-" + std::to_string(want[i + 1]) + " bytes "
                + std::to_string(got[i]) + "-" + std::to_string(got[i + 1]) + " want " + std::to_string(st) + "-" + std::to_string(en),
            input);
      }
    }
    if (!s.options.count_errors_only) {
      const size_t n = gtext_css_result_error_count(&good.result);
      if (n != gtext_css_result_error_count(&text.result)
          || gtext_css_result_errors_dropped(&good.result) != gtext_css_result_errors_dropped(&text.result)) {
        // (the decode path has no invalid-utf-8 errors of its own and the text of a decoded stylesheet is well formed, so
        // the counts are the text's; UTF-8 read in place has the same errors as the text of its bytes)
        fail("the parse errors of the bytes are not those of their text", "", input);
      }
      for (size_t i = 0; i < n; i++) {
        GTEXT_CSS_Error a, b;
        gtext_css_result_error_at(&good.result, i, &a);
        gtext_css_result_error_at(&text.result, i, &b);
        if (a.code != b.code || a.line != b.line || a.column != b.column || a.byte_offset != start_of(b.byte_offset)) {
          fail("an error is not at the bytes of its place in the text", std::string(a.name), input);
        }
      }
    }
  }

  // The same answer at every cut.
  {
    Outcome one;
    streamed(one, s, input, [](size_t) -> size_t { return 1; });
    if (signature(one.status, one.tree, one.result, input) != sig) {
      fail("a stream fed a byte at a time does not give the one-call parse's answer", "", input);
    }
    Outcome all;
    streamed(all, s, input, [&](size_t) -> size_t { return input.size() ? input.size() : 1; });
    if (signature(all.status, all.tree, all.result, input) != sig) {
      fail("a stream fed whole does not give the one-call parse's answer", "", input);
    }
    uint64_t state = 0x9E3779B97F4A7C15ULL ^ o3 ^ ((uint64_t)size << 8);
    for (int round = 0; round < 2; round++) {
      const size_t bound = round ? 2000 : 12;
      Outcome cut;
      streamed(cut, s, input, [&](size_t) -> size_t {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return (size_t)((state >> 33) % (bound + 1));
      });
      if (signature(cut.status, cut.tree, cut.result, input) != sig) {
        fail("a stream cut at random does not give the one-call parse's answer", "round " + std::to_string(round), input);
      }
    }
    // The cut at bytes of the first kilobyte and a little more (every byte of a short input, about a hundred cuts of a long one, from
    // a start the input chooses: a parse of a long input per cut, at every byte, made a unit take ten seconds): the window that
    // decides the encoding.
    const size_t limit = std::min<size_t>(input.size(), 1040);
    auto cut_in_two = [&](size_t k) {
      Outcome two;
      streamed(two, s, input, [&](size_t i) -> size_t { return i == 0 ? k : (input.size() ? input.size() : 1); });
      if (signature(two.status, two.tree, two.result, input) != sig) {
        fail("a stream cut in two does not give the one-call parse's answer", "at " + std::to_string(k), input);
      }
    };
    for (size_t k = (o3 & 1) ? 0 : 1; k <= limit; k += 1 + (limit > 80 ? (o3 >> 1) % 17 : 0) + limit / 100) {
      cut_in_two(k);
    }
    for (size_t k : {size_t{1022}, size_t{1023}, size_t{1024}, size_t{1025}}) { // (the window itself, whatever the stride was)
      if (k <= input.size()) {
        cut_in_two(k);
      }
    }
  }

  // The input limit: at the length it is met, one below it is the stream's failure.
  if (!input.empty()) {
    GTEXT_CSS_Parse_Options lo = s.options;
    lo.max_input_bytes = input.size();
    Outcome ok, tight, tight_stream;
    Settings ls = s;
    ls.options = lo;
    whole(ok, ls, input);
    if (ok.status != GTEXT_CSS_OK || signature(ok.status, ok.tree, ok.result, input) != sig) {
      fail("an input limit at the length changed the answer", gtext_css_status_string(ok.status), input);
    }
    Outcome ok_stream;
    streamed(ok_stream, ls, input, [](size_t) -> size_t { return 1; });
    if (ok_stream.fed != GTEXT_CSS_OK || signature(ok_stream.status, ok_stream.tree, ok_stream.result, input) != sig) {
      fail("a stream refused input that is exactly the input limit", gtext_css_status_string(ok_stream.fed), input);
    }
    ls.options.max_input_bytes = input.size() - 1;
    if (input.size() - 1 > 0) {
      whole(tight, ls, input);
      streamed(tight_stream, ls, input, [](size_t) -> size_t { return 1; });
      for (const Outcome * o : {&tight, &tight_stream}) {
        if (o->status != GTEXT_CSS_E_LIMIT_INPUT || o->tree || o->result.failure.offset != input.size() - 1
            || o->result.failure.line != 0) {
          fail("the input limit one below the length is not its own failure at that byte", gtext_css_status_string(o->status), input);
        }
      }
      if (tight_stream.fed != GTEXT_CSS_E_LIMIT_INPUT) {
        fail("the feed that went past the input limit did not say so", "", input);
      }
    }
  }

  // A limit made tiny: the stream agrees with the one-call parse.
  if ((o1 & 7) != 0 && (o1 & 7) != 7 && (o1 & 7) != 6) {
    Settings ls = s;
    const size_t value = (size_t)(o1 >> 3) + 1;
    switch (o1 & 7) {
    case 1: ls.options.max_depth = value; break;
    case 2: ls.options.max_tokens = value; break;
    case 3: ls.options.max_rules = value; break;
    case 4: ls.options.max_declarations = value; break;
    default: ls.options.max_total_errors = value; break;
    }
    Outcome a, b;
    whole(a, ls, input);
    streamed(b, ls, input, [&](size_t i) -> size_t { return 1 + (i * 7 + o3) % 13; });
    if (signature(a.status, a.tree, a.result, input) != signature(b.status, b.tree, b.result, input)) {
      fail("a stream under a tiny limit does not give the one-call parse's answer", std::to_string(o1), input);
    }
    if (a.status != GTEXT_CSS_OK && a.tree) {
      fail("a limit that was reached left a tree", "", input);
    }
  }

  // An allocation that fails.
  if (o2) {
    for (int path = 0; path < 2; path++) {
      FaultAllocator fa;
      Settings fs = s;
      fs.options.allocator = &fa.alloc;
      {
        Outcome r;
        fa.arm(o2);
        if (path == 0) {
          whole(r, fs, input);
        }
        else {
          streamed(r, fs, input, [&](size_t i) -> size_t { return 1 + (i * 5 + o3) % 11; });
        }
        fa.disarm();
        if (r.status == GTEXT_CSS_E_OOM) {
          if (!fa.failed || r.tree) {
            fail("E_OOM with no allocation failed, or with a tree", "", input);
          }
        }
        else if (r.status != GTEXT_CSS_OK || signature(r.status, r.tree, r.result, input) != sig) {
          fail("an allocation that failed changed the answer", gtext_css_status_string(r.status), input);
        }
      }
      if (!fa.live.empty() || fa.bad_frees) {
        fail("an allocation that failed leaked a block or freed one it did not make", path ? "stream" : "one call", input);
      }
    }
  }

  // The work is linear.
  size_t work = 0;
  if (gtext_css_parse_bytes_work(GTEXT_CSS_TREE_STYLESHEET, input.data(), input.size(), &s.bytes, &s.options, &work) != GTEXT_CSS_OK
      || work > 64 * input.size() + 4096) {
    fail("the work of parsing bytes is not linear in the input", std::to_string(work), input);
  }
  size_t swork = 0;
  if (gtext_css_stream_work(GTEXT_CSS_TREE_STYLESHEET, input.data(), input.size(), 1, &s.bytes, &s.options, &swork) != GTEXT_CSS_OK
      || swork > 96 * input.size() + 8192) {
    fail("the work of a stream fed a byte at a time is not linear in the input", std::to_string(swork), input);
  }
  return 0;
}

} // namespace css_fuzz_bytes

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_fuzz_bytes::entry(data, size);
}
