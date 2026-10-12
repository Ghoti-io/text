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
 * libFuzzer harness for the HTML tokenizer (src/html/html_tokenizer.c).
 *
 * The input is five option bytes and then bytes for the tokenizer. The same bytes
 * are tokenized whole, a byte at a time, in pieces of a size the options choose and
 * in pieces of random sizes, and the properties are the ones that make those one
 * tokenizer:
 *
 *     the token stream, the error list (code, line, column and byte offset of every
 *     error), the positions of the tokens and the status are the same however the
 *     bytes were cut - the input limit excepted, which is a limit on what has been
 *     *fed*, so a cut input reaches it after the tokens before it and a whole one
 *     before any (the status is the same, and only the status is compared);
 *     every string a token holds is UTF-8 (or WTF-8, if the options ask for it:
 *     a lone surrogate is three bytes), a tag name is lower case, no tag has two
 *     attributes of one name, a character token is not empty and its flags say
 *     what its bytes are, tokens are in order of position, and a stream that is
 *     DONE has ended with one end-of-file token and no other;
 *     errors are in order of byte offset, there are no more kept than asked, and
 *     an input stream error (surrogate, noncharacter, control, invalid UTF-8) is
 *     raised once for a character however often the machine reads it;
 *     a limit that is exceeded is its own status, sticky, and the tokens before
 *     it are a prefix of the ones a roomier run produced;
 *     an allocation that fails - the first to the nine-hundredth of a run, whole
 *     or in pieces - is OOM or the answer the run would have given, with the
 *     tokens before it a prefix of the normal ones, and never a leak;
 *     the work done stays linear in the input, fed a byte at a time.
 *
 * **The limits come from the options and are tiny on purpose**: a name of 1 to 8
 * bytes, a value of 1 to 32, an attribute count of 1 to 4. A fuzzer left to find
 * a 16 MiB comment would not. The initial state (data, RCDATA, RAWTEXT, script
 * data, PLAINTEXT, CDATA section) and the last start tag come from them too, so
 * one harness reaches the states a tree builder sets, and the tree builder's
 * answer about CDATA sections is an option.
 *
 * The seeds (tests/fuzz/corpus/html-tokenizer) are the html5lib-tests tokenizer
 * vectors' inputs, made by tools/html/make-tokenizer-fuzz-seeds.py.
 *
 * Build with: make fuzz-html-tokenizer      Run: make fuzz-run-html-tokenizer
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

#include "../../src/html/html_tokenizer_internal.h"
#include "../html_tokenizer_test_support.h"
#include "../markup_test_support.h"

namespace {

using namespace tokenizer_support;

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_html_tokenizer: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
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

bool is_utf8(const char * s, size_t n, bool allow_surrogates) {
  size_t i = 0;
  while (i < n) {
    const unsigned char b = (unsigned char)s[i];
    size_t need;
    uint32_t cp;
    if (b < 0x80) {
      i++;
      continue;
    }
    if (b >= 0xC2 && b <= 0xDF) {
      need = 1;
      cp = b & 0x1F;
    }
    else if (b >= 0xE0 && b <= 0xEF) {
      need = 2;
      cp = b & 0x0F;
    }
    else if (b >= 0xF0 && b <= 0xF4) {
      need = 3;
      cp = b & 0x07;
    }
    else {
      return false;
    }
    for (size_t k = 1; k <= need; k++) {
      if (i + k >= n || ((unsigned char)s[i + k] & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    }
    if ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF))) {
      return false;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF && !allow_surrogates) {
      return false;
    }
    i += need + 1;
  }
  return true;
}

struct Seen {
  std::vector<std::string> tokens;
  std::vector<size_t> offsets;
  std::vector<GTEXT_HTML_TokenType> types;
  bool bad = false;
  std::string why;
};

// The properties of one token by itself.
void check_token(Seen & seen, const GTEXT_HTML_Token * t, bool wtf8) {
  auto bad = [&](const std::string & why) {
    if (!seen.bad) {
      seen.bad = true;
      seen.why = why;
    }
  };
  auto text = [&](const char * p, size_t n, const char * what) {
    if (n && !p) {
      bad(std::string("a NULL string with a length: ") + what);
    }
    else if (!is_utf8(p, n, wtf8)) {
      bad(std::string("not UTF-8: ") + what);
    }
  };
  switch (t->type) {
    case GTEXT_HTML_TOKEN_CHARACTER: {
      if (t->data_length == 0) {
        bad("an empty character token");
        break;
      }
      text(t->data, t->data_length, "character data");
      bool ws = true;
      bool nul = false;
      for (size_t i = 0; i < t->data_length; i++) {
        const char c = t->data[i];
        if (c != '\t' && c != '\n' && c != '\f' && c != ' ') {
          ws = false;
        }
        if (c == 0) {
          nul = true;
        }
      }
      if (ws != t->all_whitespace) {
        bad("all_whitespace is wrong");
      }
      if (nul != t->has_null) {
        bad("has_null is wrong");
      }
      break;
    }
    case GTEXT_HTML_TOKEN_START_TAG:
    case GTEXT_HTML_TOKEN_END_TAG: {
      text(t->name, t->name_length, "a tag name");
      if (t->name_length == 0) {
        bad("a tag with no name");
      }
      for (size_t i = 0; i < t->name_length; i++) {
        if (t->name[i] >= 'A' && t->name[i] <= 'Z') {
          bad("a tag name with an upper case letter");
        }
      }
      for (size_t i = 0; i < t->attribute_count; i++) {
        const GTEXT_HTML_Attribute & a = t->attributes[i];
        text(a.name, a.name_length, "an attribute name");
        text(a.value, a.value_length, "an attribute value");
        for (size_t k = 0; k < a.name_length; k++) {
          if (a.name[k] >= 'A' && a.name[k] <= 'Z') {
            bad("an attribute name with an upper case letter");
          }
        }
        for (size_t j = 0; j < i; j++) {
          const GTEXT_HTML_Attribute & b = t->attributes[j];
          if (b.name_length == a.name_length && std::memcmp(b.name, a.name, a.name_length) == 0) {
            bad("two attributes with one name");
          }
        }
      }
      break;
    }
    case GTEXT_HTML_TOKEN_COMMENT:
      text(t->data, t->data_length, "a comment");
      break;
    case GTEXT_HTML_TOKEN_PROCESSING_INSTRUCTION:
      text(t->name, t->name_length, "a target");
      text(t->data, t->data_length, "instruction data");
      if (t->name_length == 0) {
        bad("an instruction with no target");
      }
      break;
    case GTEXT_HTML_TOKEN_DOCTYPE:
      if (t->has_name) {
        text(t->name, t->name_length, "a DOCTYPE name");
      }
      if (t->has_public_id) {
        text(t->public_id, t->public_id_length, "a public identifier");
      }
      if (t->has_system_id) {
        text(t->system_id, t->system_id_length, "a system identifier");
      }
      if (!t->has_name && t->name_length) {
        bad("a missing DOCTYPE name with a length");
      }
      break;
    case GTEXT_HTML_TOKEN_EOF:
      break;
  }
}

// What a run of the tokenizer did, with the properties of the tokens checked on the way.
struct Result {
  Outcome out;
  Seen seen;
};

Result go(const std::string & input, const std::vector<size_t> & cuts, const Config & config) {
  Result r;
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(&config.options);
  if (!tok) {
    r.out.status = GTEXT_HTML_TOKENIZER_E_OOM;
    return r;
  }
  gtext_html_tokenizer_set_state(tok, config.state);
  if (config.has_last) {
    gtext_html_tokenizer_set_last_start_tag(tok, config.last.data(), config.last.size());
  }
  auto drain = [&](GTEXT_HTML_TokenizerStatus * status) {
    const GTEXT_HTML_Token * token = nullptr;
    for (;;) {
      *status = gtext_html_tokenizer_next(tok, &token);
      if (*status != GTEXT_HTML_TOKENIZER_TOKEN) {
        return;
      }
      r.out.tokens.push_back(render(token));
      r.out.token_lines.push_back(token->line);
      r.out.token_columns.push_back(token->column);
      r.out.token_offsets.push_back(token->offset);
      r.seen.types.push_back(token->type);
      check_token(r.seen, token, config.options.wtf8);
    }
  };
  GTEXT_HTML_TokenizerStatus status = GTEXT_HTML_TOKENIZER_NEED_INPUT;
  drain(&status);
  size_t at = 0;
  size_t cut = 0;
  while (status == GTEXT_HTML_TOKENIZER_NEED_INPUT && at < input.size()) {
    size_t take = input.size() - at;
    if (!cuts.empty()) {
      take = cuts[cut < cuts.size() ? cut : cuts.size() - 1];
      cut++;
      if (take == 0) {
        take = 1;
      }
      if (take > input.size() - at) {
        take = input.size() - at;
      }
    }
    std::vector<char> chunk(input.begin() + at, input.begin() + at + take);
    at += take;
    const GTEXT_HTML_TokenizerStatus fed = gtext_html_tokenizer_feed(tok, chunk.data(), take);
    if (fed != GTEXT_HTML_TOKENIZER_OK) {
      status = fed;
    }
    else {
      drain(&status);
    }
    std::memset(chunk.data(), 0xAA, chunk.size()); // the tokenizer must not be reading this
  }
  if (status == GTEXT_HTML_TOKENIZER_NEED_INPUT) {
    gtext_html_tokenizer_finish(tok);
    drain(&status);
  }
  r.out.status = status;
  size_t last_offset = 0;
  for (size_t i = 0; i < gtext_html_tokenizer_error_count(tok); i++) {
    const GTEXT_HTML_ParseError * e = gtext_html_tokenizer_error_at(tok, i);
    r.out.errors.push_back(render_error(*e));
    r.out.error_offsets.push_back(e->offset);
    if (e->offset < last_offset) {
      r.seen.bad = true;
      r.seen.why = "errors out of order of byte offset";
    }
    last_offset = e->offset;
    if (e->line == 0 || e->column == 0) {
      r.seen.bad = true;
      r.seen.why = "an error at line or column 0";
    }
    // The input stream's own errors are raised once per character, however often the machine reads it.
    if (e->code == GTEXT_HTML_ERR_SURROGATE_IN_INPUT_STREAM || e->code == GTEXT_HTML_ERR_NONCHARACTER_IN_INPUT_STREAM
        || e->code == GTEXT_HTML_ERR_CONTROL_CHARACTER_IN_INPUT_STREAM || e->code == GTEXT_HTML_ERR_INVALID_UTF8) {
      for (size_t j = 0; j < i; j++) {
        const GTEXT_HTML_ParseError * other = gtext_html_tokenizer_error_at(tok, j);
        if (other->offset == e->offset && other->code == e->code) {
          r.seen.bad = true;
          r.seen.why = "an input stream error raised twice for one character";
        }
      }
    }
  }
  r.out.dropped = gtext_html_tokenizer_errors_dropped(tok);
  r.out.work = gtext_html_tokenizer_work(tok);
  gtext_html_tokenizer_free(tok);
  return r;
}

bool is_prefix(const std::vector<std::string> & shorter, const std::vector<std::string> & longer) {
  if (shorter.size() > longer.size()) {
    return false;
  }
  for (size_t i = 0; i < shorter.size(); i++) {
    if (shorter[i] != longer[i]) {
      return false;
    }
  }
  return true;
}

bool is_limit(GTEXT_HTML_TokenizerStatus s) {
  return GTEXT_HTML_TOKENIZER_IS_LIMIT(s);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 5) {
    return 0;
  }
  // Five option bytes, then the bytes to tokenize.
  //   o1  bits 0-1 name bytes (1, 2, 4, 8), 2 enable, 3-4 value bytes (1, 4, 16, 32), 5 enable,
  //       6 wtf8, 7 the tree builder allows CDATA
  //   o2  bits 0-1 comment bytes (1, 4, 16, 64), 2 enable, 3-4 text bytes (1, 3, 9, 40), 5 enable,
  //       6-7 attributes (0: the default, else 1, 2, 4)
  //   o3  bits 0-1 errors kept (1, 2, 5, 20), 2 enable, 3-5 the initial state, 6 the allocator
  //       failure runs, 7 an input limit
  //   o4  bits 0-2 the size of the pieces (2 to 7), bits 3-7 the input limit: the input's length
  //       minus 16 plus this number
  //   o5  bits 0-3 the last start tag emitted: none, or one of the names in `lasts` below
  const uint8_t o1 = data[0];
  const uint8_t o2 = data[1];
  const uint8_t o3 = data[2];
  const uint8_t o4 = data[3];
  const uint8_t o5 = data[4];
  const std::string input(reinterpret_cast<const char *>(data) + 5, size - 5);

  static const size_t names[] = {1, 2, 4, 8};
  static const size_t values[] = {1, 4, 16, 32};
  static const size_t comments[] = {1, 4, 16, 64};
  static const size_t texts[] = {1, 3, 9, 40};
  static const size_t attributes[] = {0, 1, 2, 4};
  static const size_t errors[] = {1, 2, 5, 20};
  static const GTEXT_HTML_TokenizerState states[] = {GTEXT_HTML_STATE_DATA, GTEXT_HTML_STATE_RCDATA,
      GTEXT_HTML_STATE_RAWTEXT, GTEXT_HTML_STATE_SCRIPT_DATA, GTEXT_HTML_STATE_PLAINTEXT,
      GTEXT_HTML_STATE_CDATA_SECTION};
  static const char * lasts[] = {nullptr, "title", "textarea", "style", "xmp", "iframe", "noembed",
      "noframes", "noscript", "script", "plaintext", "a"};

  Config config;
  config.options.max_name_bytes = (o1 & 0x04) ? names[o1 & 3] : 0;
  config.options.max_value_bytes = (o1 & 0x20) ? values[(o1 >> 3) & 3] : 0;
  config.options.wtf8 = (o1 & 0x40) != 0;
  if (o1 & 0x80) {
    config.options.allow_cdata = [](void *) { return true; };
  }
  config.options.max_comment_bytes = (o2 & 0x04) ? comments[o2 & 3] : 0;
  config.options.max_text_bytes = (o2 & 0x20) ? texts[(o2 >> 3) & 3] : 0;
  config.options.max_attributes = attributes[(o2 >> 6) & 3];
  config.options.max_errors = (o3 & 0x04) ? errors[o3 & 3] : 0;
  config.state = states[((o3 >> 3) & 7) % 6];
  if (const char * last = lasts[(o5 & 15) % (sizeof lasts / sizeof lasts[0])]) {
    config.has_last = true;
    config.last = last;
  }
  const bool input_limited = (o3 & 0x80) != 0;
  if (input_limited) {
    const size_t around = input.size() + static_cast<size_t>(o4 >> 3);
    config.options.max_input_bytes = around > 16 ? around - 16 : 1;
  }
  const bool tiny = (o1 & 0x24) || (o2 & 0x24) || (o2 & 0xC0) || input_limited;
  const size_t k = 2 + (o4 & 7) % 6;

  // ---- the four ways of cutting, which must be one tokenizer -----------------
  const Result whole = go(input, {}, config);
  if (whole.seen.bad) {
    fail("a token breaks a property", whole.seen.why, input);
  }
  if (whole.out.status == GTEXT_HTML_TOKENIZER_E_OOM) {
    fail("the tokenizer ran out of memory with the default allocator", "", input);
  }
  if (whole.out.status != GTEXT_HTML_TOKENIZER_DONE && !is_limit(whole.out.status)) {
    fail("the tokenizer stopped with a status that is neither the end nor a limit",
        status_name(whole.out.status), input);
  }
  if (whole.out.status == GTEXT_HTML_TOKENIZER_DONE) {
    size_t eofs = 0;
    for (GTEXT_HTML_TokenType t : whole.seen.types) {
      eofs += t == GTEXT_HTML_TOKEN_EOF;
    }
    if (eofs != 1 || whole.seen.types.back() != GTEXT_HTML_TOKEN_EOF) {
      fail("a finished stream does not end with exactly one end-of-file token", "", input);
    }
  }
  for (size_t i = 1; i < whole.out.token_offsets.size(); i++) {
    if (whole.out.token_offsets[i] < whole.out.token_offsets[i - 1]) {
      fail("tokens out of order of byte offset", std::to_string(i), input);
    }
  }
  const size_t kept = config.options.max_errors ? config.options.max_errors : 1024;
  if (whole.out.errors.size() > kept) {
    fail("more errors kept than asked", std::to_string(whole.out.errors.size()), input);
  }

  // The input limit is on the bytes fed: a whole input meets it before it reads a byte, a cut one
  // after the tokens before it, and a limit on a name or a text run may come first.
  const bool order_dependent = input_limited && is_limit(whole.out.status);

  uint64_t seed = 0x9E3779B97F4A7C15ULL ^ (uint64_t(o1) << 40 | uint64_t(o2) << 32 | uint64_t(o3) << 24 | uint64_t(o4) << 16) ^ input.size();
  for (unsigned char c : input) {
    seed = (seed ^ c) * 1099511628211ULL;
  }
  std::vector<size_t> random_cuts;
  for (size_t at = 0; at < input.size();) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    const size_t take = 1 + (seed >> 33) % 13;
    random_cuts.push_back(take);
    at += take;
  }
  const struct {
    const char * how;
    std::vector<size_t> cuts;
  } ways[] = {{"a byte at a time", {1}}, {"pieces", {k}}, {"random pieces", random_cuts}};
  Result bytes;
  for (const auto & way : ways) {
    Result r = go(input, way.cuts, config);
    if (r.seen.bad) {
      fail("a token breaks a property (cut up)", std::string(way.how) + ": " + r.seen.why, input);
    }
    if (order_dependent) {
      // A limit on what has been *fed* is reached, by the whole input, before any token; by a cut input,
      // after the tokens before it - and a content limit (a name, a text run) may come first. All that
      // is asked is that the cut run stopped at a limit too.
      if (!is_limit(r.out.status)) {
        fail("the input limit is not reached by every cutting", std::string(way.how) + ": "
            + status_name(r.out.status) + " against " + status_name(whole.out.status), input);
      }
      continue;
    }
    if (!whole.out.same_as(r.out)) {
      fail("the answer depends on how the input was cut", std::string(way.how) + "\n  whole: "
          + whole.out.everything() + "\n  cut:   " + r.out.everything(), input);
    }
    if (std::string(way.how) == "a byte at a time") {
      bytes = std::move(r);
    }
  }
  // Fed a byte at a time the work stays linear: a construct re-scanned from its start at every
  // byte is quadratic, and a long enough input is a denial of service. The bound is the one the
  // unit tests and the documentation use.
  if (bytes.out.work > GTEXT_HTML_TOKENIZER_WORK_BOUND(input.size())) {
    fail("the tokenizer's work is not linear in its input", std::to_string(bytes.out.work) + " units for "
        + std::to_string(input.size()) + " bytes", input);
  }

  // ---- limits: what a roomier run did first ---------------------------------
  if (tiny && is_limit(whole.out.status)) {
    Config roomy;
    roomy.options.wtf8 = config.options.wtf8;
    roomy.options.allow_cdata = config.options.allow_cdata;
    roomy.options.max_errors = 1u << 20;
    roomy.state = config.state;
    roomy.has_last = config.has_last;
    roomy.last = config.last;
    const Result big = go(input, {}, roomy);
    if (big.out.status != GTEXT_HTML_TOKENIZER_DONE) {
      fail("the roomy run did not finish", status_name(big.out.status), input);
    }
    if (!order_dependent && !is_prefix(whole.out.tokens, big.out.tokens)) {
      fail("the tokens before a limit are not a prefix of the roomy run's", whole.out.everything(), input);
    }
  }

  // ---- an allocation that fails ---------------------------------------------
  // Failing the Nth allocation for N across the first nine hundred, whole or cut: OOM, with the tokens
  // before it the normal ones, or the normal answer; and nothing leaked.
  if (o3 & 0x40) {
    for (int which = 0; which < 2; which++) {
      const size_t point = 1 + ((static_cast<size_t>(o1) << 16 | static_cast<size_t>(o2) << 8 | o3) * 2654435761u
          + input.size() * (which + 1)) % 900;
      FaultAllocator fa;
      Config fc = config;
      fc.options.allocator = &fa.alloc;
      fa.arm(point);
      const Result f = go(input, which ? ways[0].cuts : std::vector<size_t>{}, fc);
      const bool failed = fa.failed;
      fa.disarm();
      if (f.out.status == GTEXT_HTML_TOKENIZER_E_OOM) {
        if (!failed) {
          fail("OOM reported with no allocation failed", which ? "cut" : "whole", input);
        }
        if (!order_dependent && !is_prefix(f.out.tokens, whole.out.tokens)) {
          fail("the tokens before an allocation failure are not the normal ones", f.out.everything(), input);
        }
      }
      else if (!order_dependent && !whole.out.same_as(f.out)) {
        fail("an allocation failure changed the answer", f.out.everything() + "\n  against " + whole.out.everything(), input);
      }
      if (!fa.live.empty() || fa.bad_frees != 0) {
        fail("the tokenizer leaked or double-freed", std::to_string(fa.live.size()) + " blocks, "
            + std::to_string(fa.bad_frees) + " bad frees", input);
      }
    }
  }
  return 0;
}
