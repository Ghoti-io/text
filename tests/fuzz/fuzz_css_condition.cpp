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
 * libFuzzer harness for media queries (src/css/css_media_*.c, css_conditions.c).
 *
 * The input is two option bytes, then the payload:
 *
 *   o0  bit 0: generate (the payload drives a generator of queries whose truth is known) instead of being the query text;
 *       bit 1: the limits are small (depth 6, 40 nodes, 6 queries) so that every limit path is reached;
 *   o1  the allocation to fail (0 for none), counted over the one parse.
 *
 * The properties:
 *
 *   a status is OK or a limit, or E_OOM for an allocation that failed, with nothing leaked or freed that the allocator did not make - never E_INTERNAL;
 *   the serialization is a fixed point: parse, serialize, parse, serialize gives the same text, the same query count and the same three-valued answers;
 *   evaluation never crashes, and is OK, E_UNSUPPORTED or E_LIMIT_WORK;
 *   in generate mode the answer of every query equals the answer of a naive Kleene evaluation of the generated tree, written here and not shared with the library
 *   (leaves of known truth: true, false and unknown, in a fixed environment), and a list matches when some query is true.
 *
 * Seeds: tests/fuzz/corpus/css-condition, made by tools/css/make-fuzz-condition-seeds.py.
 * Build with: make fuzz-css-condition      Run: make fuzz-run-css-condition
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "../markup_test_support.h"

namespace css_condfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_css_condition: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
  for (unsigned char c : input) {
    std::fprintf(stderr, "%02x", c);
  }
  std::fputc('\n', stderr);
  std::abort();
}

GTEXT_Markup_Status collect(void * user, const char * bytes, size_t len) {
  static_cast<std::string *>(user)->append(bytes, len);
  return GTEXT_MARKUP_OK;
}

std::string write_list(const GTEXT_CSS_Media_List * l) {
  std::string out;
  GTEXT_Markup_Sink sink{collect, &out};
  if (gtext_css_media_list_serialize(l, &sink) != GTEXT_CSS_OK) {
    return "<serialize failed>";
  }
  return out;
}

using Tri = GTEXT_CSS_Tristate;

Tri k_not(Tri a) {
  return a == GTEXT_CSS_TRI_TRUE ? GTEXT_CSS_TRI_FALSE : (a == GTEXT_CSS_TRI_FALSE ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_UNKNOWN);
}

struct Gen {
  const uint8_t * p;
  size_t n, i = 0;
  size_t budget = 24;
  uint8_t next() {
    return i < n ? p[i++] : 0;
  }
  std::string ws() {
    static const char * const w[] = {" ", "  ", "\n", " /**/ ", "\t"};
    return w[next() % 5];
  }
  /// <media-in-parens>: text and truth.
  std::string paren(Tri & t, int depth) {
    uint8_t c = next();
    if (depth > 0 && budget > 0 && c % 4 == 0) {
      budget--;
      Tri inner;
      std::string s = cond(inner, depth - 1, true);
      t = inner;
      return "(" + ws() + s + ws() + ")";
    }
    static const char * const yes[] = {"(color)", "(width: 1280px)", "(min-width: 100px)", "(hover: hover)", "(400px <= width <= 2000px)", "(ORIENTATION: landscape)"};
    static const char * const no[] = {"(monochrome)", "(width: 5px)", "(pointer: none)", "(max-width: 10px)", "(width < 100px)", "(grid)"};
    static const char * const unk[] = {"(foo)", "foo(bar)", "(--zz)", "()", "(a b)", "(min-foo: 1)"};
    switch (c % 3) {
    case 0: t = GTEXT_CSS_TRI_TRUE; return yes[next() % 6];
    case 1: t = GTEXT_CSS_TRI_FALSE; return no[next() % 6];
    default: t = GTEXT_CSS_TRI_UNKNOWN; return unk[next() % 6];
    }
  }
  /// <media-condition>; `or_ok` says an `or` chain is allowed.
  std::string cond(Tri & t, int depth, bool or_ok) {
    uint8_t c = next();
    if (c % 5 == 0) {
      Tri x;
      std::string s = paren(x, depth);
      t = k_not(x);
      return "not" + ws() + s;
    }
    bool is_or = or_ok && (c % 2 == 1);
    size_t count = 1 + next() % 3;
    Tri acc = is_or ? GTEXT_CSS_TRI_FALSE : GTEXT_CSS_TRI_TRUE;
    std::string out;
    for (size_t k = 0; k < count; k++) {
      Tri x;
      std::string s = paren(x, depth);
      if (k) {
        out += ws() + (is_or ? "or" : "AND") + ws();
      }
      out += s;
      if (is_or) {
        acc = (acc == GTEXT_CSS_TRI_TRUE || x == GTEXT_CSS_TRI_TRUE) ? GTEXT_CSS_TRI_TRUE : ((acc == GTEXT_CSS_TRI_UNKNOWN || x == GTEXT_CSS_TRI_UNKNOWN) ? GTEXT_CSS_TRI_UNKNOWN : GTEXT_CSS_TRI_FALSE);
      } else {
        acc = (acc == GTEXT_CSS_TRI_FALSE || x == GTEXT_CSS_TRI_FALSE) ? GTEXT_CSS_TRI_FALSE : ((acc == GTEXT_CSS_TRI_UNKNOWN || x == GTEXT_CSS_TRI_UNKNOWN) ? GTEXT_CSS_TRI_UNKNOWN : GTEXT_CSS_TRI_TRUE);
      }
    }
    t = acc;
    return out;
  }
  /// One query: its text and its three-valued truth (unknown only below the top; the library keeps it).
  std::string query(Tri & t) {
    uint8_t c = next();
    if (c % 7 == 0) {
      static const char * const bad[] = {"only (color)", "(color) and (hover) or (pointer)", "screen (color)", "(width: 5)", "or and (color)", "(min-grid: 1)"};
      t = GTEXT_CSS_TRI_FALSE;
      return bad[next() % 6];
    }
    bool typed = c % 3 != 1;
    if (!typed) {
      return cond(t, 3, true);
    }
    static const char * const types[] = {"all", "screen", "print", "foo", "tv"};
    static const bool live[] = {true, true, false, false, false};
    int ty = next() % 5;
    uint8_t m = next() % 3; /* 0 none, 1 not, 2 only */
    Tri ct = GTEXT_CSS_TRI_TRUE;
    std::string s;
    if (m == 1) {
      s = "not ";
    } else if (m == 2) {
      s = "only ";
    }
    s += types[ty];
    if (next() % 2) {
      s += " and " + cond(ct, 3, false);
    }
    Tri tt = live[ty] ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
    Tri r = (tt == GTEXT_CSS_TRI_FALSE || ct == GTEXT_CSS_TRI_FALSE) ? GTEXT_CSS_TRI_FALSE
            : ((ct == GTEXT_CSS_TRI_UNKNOWN) ? GTEXT_CSS_TRI_UNKNOWN : GTEXT_CSS_TRI_TRUE);
    t = m == 1 ? k_not(r) : r;
    return s;
  }
};

GTEXT_CSS_Media_Environment env_for_generator() {
  GTEXT_CSS_Media_Environment e = gtext_css_media_environment_default(nullptr);
  e.viewport = {1280, 800};
  return e;
}

void check_eval(const GTEXT_CSS_Media_List * l, const std::string & input, const std::vector<Tri> * want) {
  GTEXT_CSS_Media_Environment e = env_for_generator();
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.media = &e;
  co.max_work = 100000;
  GTEXT_CSS_Conditions * c = nullptr;
  if (gtext_css_conditions_new(nullptr, &co, &c) != GTEXT_CSS_OK) {
    fail("conditions_new failed", "", input);
  }
  GTEXT_CSS_Condition_Call call;
  gtext_css_condition_call_init(&call);
  size_t nq = gtext_css_media_list_query_count(l);
  bool any = nq == 0;
  for (size_t q = 0; q < nq; q++) {
    Tri t = GTEXT_CSS_TRI_UNKNOWN;
    GTEXT_CSS_Status st = gtext_css_conditions_evaluate_query(c, l, q, &call, &t);
    if (st != GTEXT_CSS_OK && st != GTEXT_CSS_E_UNSUPPORTED && st != GTEXT_CSS_E_LIMIT_WORK) {
      fail("evaluate_query status", gtext_css_status_string(st), input);
    }
    if (st == GTEXT_CSS_OK) {
      if (want && (*want)[q] != t) {
        fail("an answer differs from the naive Kleene evaluation", write_list(l) + " query " + std::to_string(q), input);
      }
      any = any || t == GTEXT_CSS_TRI_TRUE;
    }
  }
  bool m = false;
  GTEXT_CSS_Status st = gtext_css_conditions_matches_list(c, l, &call, &m);
  if (st == GTEXT_CSS_OK && want && m != (any || nq == 0)) {
    fail("matches_list differs from some-query-true", write_list(l), input);
  }
  gtext_css_condition_call_free(&call);
  gtext_css_conditions_free(c);
}

}  // namespace css_condfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  using namespace css_condfuzz;
  if (size < 2) {
    return 0;
  }
  const uint8_t o0 = data[0], o1 = data[1];
  const uint8_t * payload = data + 2;
  const size_t psize = size - 2;
  std::string input(reinterpret_cast<const char *>(data), size);
  std::string text;
  std::vector<Tri> want;
  const bool generate = (o0 & 1) != 0;
  if (generate) {
    Gen g{payload, psize};
    size_t nq = 1 + g.next() % 3;
    for (size_t q = 0; q < nq; q++) {
      Tri t;
      if (q) {
        text += ",";
      }
      text += g.query(t);
      want.push_back(t);
    }
  } else {
    text.assign(reinterpret_cast<const char *>(payload), psize);
  }
  FaultAllocator fa;
  GTEXT_CSS_Media_Options o = gtext_css_media_options_default();
  o.allocator = &fa.alloc;
  if (o0 & 2) {
    o.max_depth = 6;
    o.max_nodes = 40;
    o.max_queries = 6;
  }
  GTEXT_CSS_Media_List * l = nullptr;
  GTEXT_CSS_Media_Result r;
  fa.arm(o1);
  GTEXT_CSS_Status st = gtext_css_media_list_parse(text.data(), text.size(), &o, &l, &r);
  const bool failed = fa.failed;
  fa.disarm();
  if (st == GTEXT_CSS_E_OOM) {
    if (!failed) {
      fail("OOM without a failed allocation", "", input);
    }
    if (l) {
      fail("OOM with a list", "", input);
    }
  } else if (st != GTEXT_CSS_OK && st != GTEXT_CSS_E_LIMIT_CONDITIONS && st != GTEXT_CSS_E_LIMIT_CONDITION_DEPTH && st != GTEXT_CSS_E_LIMIT_TOKENS &&
             st != GTEXT_CSS_E_LIMIT_DEPTH && st != GTEXT_CSS_E_LIMIT_INPUT) {
    fail("parse status", gtext_css_status_string(st), input);
  } else if (st == GTEXT_CSS_OK && failed) {
    fail("an allocation failed and the parse did not say so", "", input);
  }
  if (st == GTEXT_CSS_OK) {
    const std::string a = write_list(l);
    GTEXT_CSS_Media_Options o2 = gtext_css_media_options_default();
    o2.max_depth = 1 << 16;
    GTEXT_CSS_Media_List * l2 = nullptr;
    if (gtext_css_media_list_parse(a.data(), a.size(), &o2, &l2, nullptr) == GTEXT_CSS_OK) {
      const std::string b = write_list(l2);
      if (a != b) {
        fail("the serialization is not a fixed point", a + "\n" + b, input);
      }
      if (gtext_css_media_list_query_count(l2) != gtext_css_media_list_query_count(l)) {
        fail("the query count changed through a round trip", a, input);
      }
      if (!generate) {
        check_eval(l2, input, nullptr);
      }
    }
    gtext_css_media_list_free(l2);
    if (generate && gtext_css_media_list_query_count(l) == want.size() && !(o0 & 2)) {
      check_eval(l, input, &want);
    } else if (!generate) {
      check_eval(l, input, nullptr);
    }
  }
  gtext_css_media_list_free(l);
  gtext_css_media_result_free(&r);
  if (!fa.live.empty()) {
    fail("blocks leaked", std::to_string(fa.live.size()), input);
  }
  if (fa.bad_frees) {
    fail("a free of a block the allocator did not make", "", input);
  }
  return 0;
}
