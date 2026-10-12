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
 * Shared by the HTML tokenizer's tests and its fuzz harness's replay: run the
 * tokenizer over a byte string, cut up any way, and write down what it did as
 * text, so that "the same" is a string comparison and a failure shows both.
 *
 * The rendering is the point. It is not the vectors' JSON (the conformance
 * gate reads that); it is meant to be read in a test's expectation:
 *
 *   C"text"[w][n]       characters (w: white space only, n: holds a NUL)
 *   S(name a=v b=)/     start tag, attributes in order, `/` if self-closing
 *   E(name a=v)         end tag
 *   M(data)             comment
 *   D(name|public|system)  DOCTYPE; `-` for a missing part, `!` after if quirks
 *   P(target|data)      processing instruction
 *   F                   end of file
 *
 * Bytes outside printable ASCII are written `\xHH`.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GTEXT_TESTS_HTML_TOKENIZER_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_HTML_TOKENIZER_TEST_SUPPORT_H

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../src/html/html_tokenizer_internal.h"

namespace tokenizer_support {

inline std::string show(const char * s, size_t n) {
  std::string out;
  static const char * digits = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    if (c >= 0x20 && c < 0x7F && c != '\\') {
      out += (char)c;
    }
    else if (c == '\n') {
      out += "\\n";
    }
    else if (c == '\\') {
      out += "\\\\";
    }
    else {
      out += "\\x";
      out += digits[c >> 4];
      out += digits[c & 15];
    }
  }
  return out;
}

inline std::string render(const GTEXT_HTML_Token * t) {
  auto str = [](const char * p, size_t n) { return show(p, n); };
  std::string out;
  switch (t->type) {
    case GTEXT_HTML_TOKEN_CHARACTER:
      out = "C\"" + str(t->data, t->data_length) + "\"";
      if (t->all_whitespace) {
        out += "[w]";
      }
      if (t->has_null) {
        out += "[n]";
      }
      break;
    case GTEXT_HTML_TOKEN_START_TAG:
    case GTEXT_HTML_TOKEN_END_TAG:
      out = t->type == GTEXT_HTML_TOKEN_START_TAG ? "S(" : "E(";
      out += str(t->name, t->name_length);
      for (size_t i = 0; i < t->attribute_count; i++) {
        out += " " + str(t->attributes[i].name, t->attributes[i].name_length)
            + "=" + str(t->attributes[i].value, t->attributes[i].value_length);
      }
      out += ")";
      if (t->self_closing) {
        out += "/";
      }
      break;
    case GTEXT_HTML_TOKEN_COMMENT:
      out = "M(" + str(t->data, t->data_length) + ")";
      break;
    case GTEXT_HTML_TOKEN_DOCTYPE:
      out = "D(";
      out += t->has_name ? str(t->name, t->name_length) : "-";
      out += "|";
      out += t->has_public_id ? str(t->public_id, t->public_id_length) : "-";
      out += "|";
      out += t->has_system_id ? str(t->system_id, t->system_id_length) : "-";
      out += ")";
      if (t->force_quirks) {
        out += "!";
      }
      break;
    case GTEXT_HTML_TOKEN_PROCESSING_INSTRUCTION:
      out = "P(" + str(t->name, t->name_length) + "|"
          + str(t->data, t->data_length) + ")";
      break;
    case GTEXT_HTML_TOKEN_EOF:
      out = "F";
      break;
  }
  return out;
}

inline std::string render_error(const GTEXT_HTML_ParseError & e) {
  return std::string(gtext_html_error_code_name(e.code)) + "@"
      + std::to_string(e.line) + ":" + std::to_string(e.column);
}

inline const char * status_name(GTEXT_HTML_TokenizerStatus s) {
  switch (s) {
    case GTEXT_HTML_TOKENIZER_TOKEN: return "TOKEN";
    case GTEXT_HTML_TOKENIZER_NEED_INPUT: return "NEED_INPUT";
    case GTEXT_HTML_TOKENIZER_DONE: return "DONE";
    case GTEXT_HTML_TOKENIZER_OK: return "OK";
    case GTEXT_HTML_TOKENIZER_E_INVALID: return "E_INVALID";
    case GTEXT_HTML_TOKENIZER_E_STATE: return "E_STATE";
    case GTEXT_HTML_TOKENIZER_E_OOM: return "E_OOM";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT: return "E_LIMIT_INPUT";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_NAME: return "E_LIMIT_NAME";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE: return "E_LIMIT_VALUE";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_COMMENT: return "E_LIMIT_COMMENT";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_TEXT: return "E_LIMIT_TEXT";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_ATTRIBUTES: return "E_LIMIT_ATTRIBUTES";
  }
  return "E_UNKNOWN";
}

/// What a run did. `tokens` and `errors` are rendered; `error_offsets` are the
/// byte offsets of the errors, which the rendering leaves out so that
/// expectations stay short; tests that care compare them.
struct Outcome {
  std::vector<std::string> tokens;
  std::vector<std::string> errors;
  std::vector<size_t> error_offsets;
  std::vector<size_t> token_lines;
  std::vector<size_t> token_columns;
  std::vector<size_t> token_offsets;
  GTEXT_HTML_TokenizerStatus status = GTEXT_HTML_TOKENIZER_NEED_INPUT;
  size_t dropped = 0;
  size_t work = 0;

  std::string tokens_text() const {
    std::string out;
    for (const std::string & t : tokens) {
      out += (out.empty() ? "" : " ") + t;
    }
    return out;
  }
  std::string errors_text() const {
    std::string out;
    for (const std::string & e : errors) {
      out += (out.empty() ? "" : " ") + e;
    }
    return out;
  }
  bool same_as(const Outcome & other) const {
    return tokens == other.tokens && errors == other.errors
        && error_offsets == other.error_offsets
        && token_lines == other.token_lines
        && token_columns == other.token_columns
        && token_offsets == other.token_offsets && status == other.status
        && dropped == other.dropped;
  }
  std::string everything() const {
    return tokens_text() + " | " + errors_text() + " | "
        + status_name(status) + " dropped " + std::to_string(dropped);
  }
};

/// How to set the tokenizer up. A zero-initialised options struct is the
/// defaults.
struct Config {
  GTEXT_HTML_TokenizerOptions options = {};
  GTEXT_HTML_TokenizerState state = GTEXT_HTML_STATE_DATA;
  bool has_last = false;
  std::string last;
  // Called with each token before the next is asked for: the tree builder's seat.
  void (*on_token)(GTEXT_HTML_Tokenizer *, const GTEXT_HTML_Token *) = nullptr;
};

inline void collect(GTEXT_HTML_Tokenizer * tok, Outcome * run, const Config & setup,
    GTEXT_HTML_TokenizerStatus * status) {
  const GTEXT_HTML_Token * token = nullptr;
  for (;;) {
    *status = gtext_html_tokenizer_next(tok, &token);
    if (*status != GTEXT_HTML_TOKENIZER_TOKEN) {
      return;
    }
    run->tokens.push_back(render(token));
    run->token_lines.push_back(token->line);
    run->token_columns.push_back(token->column);
    run->token_offsets.push_back(token->offset);
    if (setup.on_token) {
      setup.on_token(tok, token);
    }
  }
}

/// Feed `input` in pieces of the sizes in `cuts` (repeating the last; an empty
/// list is one piece), each piece a fresh heap block that is overwritten and
/// freed the moment the tokenizer has asked for more.
inline Outcome tokenize(const std::string & input,
    const std::vector<size_t> & cuts = {}, const Config & setup = Config()) {
  Outcome run;
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(&setup.options);
  if (!tok) {
    run.status = GTEXT_HTML_TOKENIZER_E_OOM;
    return run;
  }
  gtext_html_tokenizer_set_state(tok, setup.state);
  if (setup.has_last) {
    gtext_html_tokenizer_set_last_start_tag(
        tok, setup.last.data(), setup.last.size());
  }
  GTEXT_HTML_TokenizerStatus status = GTEXT_HTML_TOKENIZER_NEED_INPUT;
  collect(tok, &run, setup, &status);
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
    std::unique_ptr<char[]> chunk(new char[take]);
    std::memcpy(chunk.get(), input.data() + at, take);
    at += take;
    const GTEXT_HTML_TokenizerStatus fed =
        gtext_html_tokenizer_feed(tok, chunk.get(), take);
    if (fed != GTEXT_HTML_TOKENIZER_OK) {
      status = fed;
    }
    else {
      collect(tok, &run, setup, &status);
    }
    std::memset(chunk.get(), 0xAA, take); // the tokenizer must not be reading this
  }
  if (status == GTEXT_HTML_TOKENIZER_NEED_INPUT) {
    gtext_html_tokenizer_finish(tok);
    collect(tok, &run, setup, &status);
  }
  run.status = status;
  for (size_t i = 0; i < gtext_html_tokenizer_error_count(tok); i++) {
    const GTEXT_HTML_ParseError * e = gtext_html_tokenizer_error_at(tok, i);
    run.errors.push_back(render_error(*e));
    run.error_offsets.push_back(e->offset);
  }
  run.dropped = gtext_html_tokenizer_errors_dropped(tok);
  run.work = gtext_html_tokenizer_work(tok);
  gtext_html_tokenizer_free(tok);
  return run;
}

} // namespace tokenizer_support

#endif // GHOTI_IO_GTEXT_TESTS_HTML_TOKENIZER_TEST_SUPPORT_H
