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
 *   (leaves of known truth: true, false and unknown, in a fixed environment), and a list matches when some query is true;
 *   the laws of the three-valued logic hold for any two conditions A and B the generator makes: `not (not A)` is A, `not (A and B)` is `(not A) or (not B)`, `not (A or B)` is
 *   `(not A) and (not B)`, `A and B` is `B and A`, `A or B` is `B or A`, and `A and (B or A)` is A (absorption).
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


// ---- an exact model of measurements -------------------------------------------------------------------------------------------------------------------------
//
// The environment is the one env_for_generator() makes: a 1280 by 800 viewport at 1 dppx, 8 bits of color and none of monochrome. A measurement is a number of
// hundredths with a unit, as a fraction of its base unit (px for lengths, dppx for resolutions), and two such numbers are compared as the fractions they are, in
// 64-bit integers (the products stay below 2^63), which is not how the library computes them (doubles, a table of factors) and so is a second reading.

using Int = long long;  // the largest product is about 1.4e9 x 1.3e4: 64 bits are exact

struct Frac {
  Int n, d;  // d > 0
};

Int gcd128(Int a, Int b) {
  if (a < 0) {
    a = -a;
  }
  while (b) {
    Int t = a % b;
    a = b;
    b = t;
  }
  return a;
}

Frac mul(Frac a, Frac b) {
  Frac r{a.n * b.n, a.d * b.d};
  Int g = gcd128(r.n, r.d);
  if (g > 1) {
    r.n /= g;
    r.d /= g;
  }
  return r;
}

int cmp(Frac a, Frac b) {
  const Int l = a.n * b.d, r = b.n * a.d;
  return l < r ? -1 : (l > r ? 1 : 0);
}

struct UnitRow {
  const char * name;
  Frac factor;  // of the base unit
};

// CSS Values 4, 5.2 (absolute lengths) and 6.1 (resolution): 1in = 96px = 2.54cm = 25.4mm = 101.6Q = 72pt = 6pc; 1dppx = 96dpi = 2.54dpcm / 96... 1x = 1dppx
const UnitRow kLengths[] = {{"px", {1, 1}}, {"cm", {4800, 127}}, {"mm", {480, 127}}, {"Q", {120, 127}}, {"in", {96, 1}}, {"pt", {4, 3}}, {"pc", {16, 1}}, {"em", {16, 1}}, {"rem", {16, 1}},
                            {"vw", {64, 5}}, {"vh", {8, 1}}, {"vmin", {8, 1}}, {"vmax", {64, 5}}};
const UnitRow kResolutions[] = {{"dppx", {1, 1}}, {"x", {1, 1}}, {"dpi", {1, 96}}, {"dpcm", {127, 4800}}};

struct Meas {
  std::string text;  // as written
  Frac value;        // in base units
};

const char * const kOps[] = {"<", "<=", ">", ">=", "="};

bool holds(int c, int op) {
  switch (op) {
  case 0: return c < 0;
  case 1: return c <= 0;
  case 2: return c > 0;
  case 3: return c >= 0;
  default: return c == 0;
  }
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
  bool measure_mode = false;
  /// A literal of hundredths with a unit from the table, and its exact value; sometimes the value of the viewport itself, so that equality is reached.
  Meas literal(const UnitRow * units, size_t count, Frac exact_base) {
    Meas m;
    const uint8_t pick = next();
    if (pick % 3 == 0) {
      // the environment's own value in base units, written in the unit (when its decimal expansion is finite: the base unit itself)
      m.text = std::to_string(static_cast<long long>(exact_base.n / exact_base.d)) + units[0].name;
      m.value = exact_base;
      return m;
    }
    const UnitRow & u = units[next() % count];
    const long long hundredths = (static_cast<long long>(next()) * 37 + next()) % 300000;  // 0.00 to 2999.99
    char buf[64];
    std::snprintf(buf, sizeof buf, "%lld.%02lld%s", hundredths / 100, hundredths % 100, u.name);
    m.text = buf;
    m.value = mul(Frac{hundredths, 100}, u.factor);
    return m;
  }
  /// A measurement leaf: text and exact truth (the environment is 1280 by 800 at 1 dppx, 8 bits of color).
  std::string measurement(Tri & t) {
    const uint8_t f = next() % 5;
    Frac env;
    const char * name;
    const UnitRow * units = kLengths;
    size_t count = sizeof kLengths / sizeof *kLengths;
    switch (f) {
    case 0: name = "width"; env = {1280, 1}; break;
    case 1: name = "height"; env = {800, 1}; break;
    case 2: name = "resolution"; env = {1, 1}; units = kResolutions; count = 4; break;
    case 3: name = "color"; env = {8, 1}; break;
    default: name = "aspect-ratio"; env = {8, 5}; break;
    }
    const uint8_t form = next() % 5;
    std::string text;
    bool truth;
    if (f == 3) {
      const int v = next() % 12;
      const int op = next() % 5;
      if (form % 2 == 0) {
        text = std::string("(") + name + " " + kOps[op] + " " + std::to_string(v) + ")";
        truth = holds((8 > v) - (8 < v), op);  // the feature is on the left: 8 op v
      } else {
        text = std::string("(") + std::to_string(v) + " " + kOps[op] + " " + name + ")";
        truth = holds((v > 8) - (v < 8), op);
      }
    } else if (f == 4) {
      const int a = 1 + next() % 40, b = 1 + next() % 40;
      const int op = next() % 5;
      const bool left = form % 2 == 0;
      const Frac lit{a, b};
      const int c = cmp(env, lit);
      text = left ? std::string("(") + name + " " + kOps[op] + " " + std::to_string(a) + " / " + std::to_string(b) + ")"
                  : std::string("(") + std::to_string(a) + "/" + std::to_string(b) + " " + kOps[op] + " " + name + ")";
      truth = left ? holds(c, op) : holds(-c, op);
    } else if (form == 4) {
      // a double range: lo op1 feature op2 hi, both in the same direction
      const Meas lo = literal(units, count, env), hi = literal(units, count, env);
      const int dir = next() % 2;
      const int o1 = dir ? 2 + next() % 2 : next() % 2;
      const int o2 = dir ? 2 + next() % 2 : next() % 2;
      text = "(" + lo.text + " " + kOps[o1] + " " + name + " " + kOps[o2] + " " + hi.text + ")";
      truth = holds(cmp(lo.value, env), o1) && holds(cmp(env, hi.value), o2);  // `lo op1 f` and `f op2 hi`
    } else {
      const Meas m = literal(units, count, env);
      const int c = cmp(env, m.value);
      if (form == 0) {
        text = std::string("(") + name + ": " + m.text + ")";
        truth = c == 0;
      } else if (form == 1) {
        text = std::string("(min-") + name + ": " + m.text + ")";
        truth = c >= 0;
      } else if (form == 2) {
        text = std::string("(max-") + name + ": " + m.text + ")";
        truth = c <= 0;
      } else {
        const int op = next() % 5;
        text = std::string("(") + name + " " + kOps[op] + " " + m.text + ")";
        truth = holds(c, op);
      }
    }
    t = truth ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
    return text;
  }
  /// <media-in-parens>: text and truth.
  std::string paren(Tri & t, int depth) {
    uint8_t c = next();
    if (measure_mode && c % 3 != 0 && !(depth > 0 && budget > 0 && c % 4 == 0)) {
      return measurement(t);
    }
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
      /* an invalid query is `not all` (false); a known feature written wrongly is one unknown leaf, which the library keeps as unknown below the top */
      static const char * const bad[] = {"only (color)", "(color) and (hover) or (pointer)", "screen (color)", "(width: 5)", "or and (color)", "(min-grid: 1)"};
      const uint8_t k = next() % 6;
      t = (k == 3 || k == 5) ? GTEXT_CSS_TRI_UNKNOWN : GTEXT_CSS_TRI_FALSE;
      return bad[k];
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


// ---- the laws of the three-valued logic ------------------------------------------------------------------------------------------------------------------------

Tri eval_text(GTEXT_CSS_Conditions * c, const std::string & text, const std::string & input) {
  GTEXT_CSS_Media_List * l = nullptr;
  if (gtext_css_media_list_parse(text.data(), text.size(), nullptr, &l, nullptr) != GTEXT_CSS_OK) {
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  Tri t = GTEXT_CSS_TRI_UNKNOWN;
  GTEXT_CSS_Status st = gtext_css_conditions_evaluate_query(c, l, 0, nullptr, &t);
  gtext_css_media_list_free(l);
  if (st != GTEXT_CSS_OK) {
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  (void)input;
  return t;
}

void check_laws(const std::string & a, const std::string & b, const std::string & input) {
  GTEXT_CSS_Media_Environment e = env_for_generator();
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.media = &e;
  GTEXT_CSS_Conditions * c = nullptr;
  if (gtext_css_conditions_new(nullptr, &co, &c) != GTEXT_CSS_OK) {
    fail("conditions_new failed", "", input);
  }
  struct Law {
    const char * name;
    std::string left, right;
  } laws[] = {
      {"double negation", "not (not " + a + ")", a},
      {"De Morgan (and)", "not (" + a + " and " + b + ")", "(not " + a + ") or (not " + b + ")"},
      {"De Morgan (or)", "not (" + a + " or " + b + ")", "(not " + a + ") and (not " + b + ")"},
      {"commutativity (and)", a + " and " + b, b + " and " + a},
      {"commutativity (or)", a + " or " + b, b + " or " + a},
      {"absorption", a + " and (" + b + " or " + a + ")", a},
      {"idempotence", a + " or " + a, a},
  };
  for (const Law & law : laws) {
    const Tri l = eval_text(c, law.left, input), r = eval_text(c, law.right, input);
    if (l != r) {
      fail((std::string("the three-valued logic breaks ") + law.name).c_str(), law.left + "  =>  " + std::to_string(l) + "\n" + law.right + "  =>  " + std::to_string(r), input);
    }
  }
  gtext_css_conditions_free(c);
}

// ---- @supports against a reference that parses each declaration directly ---------------------------------------------------------------------------------------

const GTEXT_CSS_Registry * registry() {
  static GTEXT_CSS_Registry * r = [] {
    GTEXT_CSS_Registry * x = nullptr;
    return gtext_css_registry_new(nullptr, &x) == GTEXT_CSS_OK ? x : nullptr;
  }();
  return r;
}

struct DeclPair {
  const char * property;
  const char * value;
};

const DeclPair kDecls[] = {
    {"display", "grid"}, {"display", "none"}, {"display", "bogus"}, {"color", "red"}, {"color", "#12"}, {"color", "rgb(1 2 3)"}, {"color", "var(--x)"}, {"color", "inherit"},
    {"width", "10px"}, {"width", "10"}, {"width", "calc(1px + 2px)"}, {"width", "calc(1px + 2)"}, {"margin", "1px 2px 3px 4px"}, {"margin", "1px 2px 3px 4px 5px"},
    {"font-family", "serif, \"a b\""}, {"font-family", "inherit a"}, {"--x", ""}, {"--x", "a b"}, {"--x", "a;b"}, {"--x", "}"}, {"--x", "var()"}, {"--x", "{a:b}"},
    {"line-height", "1.5"}, {"line-height", "-1"}, {"opacity", "50%"}, {"opacity", "2"}, {"z-index", "1.5"}, {"z-index", "3"}, {"foo", "bar"}, {"background-color", "transparent"}};

struct SupGen {
  const uint8_t * p;
  size_t n, i = 0;
  size_t budget = 16;
  bool invalid = false;  // a leaf whose contents are not an <any-value> (an unmatched `}`): the whole condition is invalid, so false (Conditional 3, <general-enclosed>)
  uint8_t next() {
    return i < n ? p[i++] : 0;
  }
  bool decl(const GTEXT_CSS_Registry * reg, std::string & text) {
    const DeclPair & d = kDecls[next() % (sizeof kDecls / sizeof *kDecls)];
    const bool important = next() % 7 == 0;
    text = std::string("(") + d.property + ": " + d.value + (important ? " !important" : "") + ")";
    invalid = invalid || std::strcmp(d.value, "}") == 0;
    // the reference: the property parser, directly (quirks off), as the cascade parses a declaration
    GTEXT_CSS_Property_Options po;
    std::memset(&po, 0, sizeof po);
    GTEXT_CSS_Property_Value pv;
    std::memset(&pv, 0, sizeof pv);
    const std::string value = d.value;
    const GTEXT_CSS_Status st = gtext_css_parse_property_text(reg, d.property, std::strlen(d.property), value.data(), value.size(), &po, &pv);
    bool ok = false;
    if (st == GTEXT_CSS_OK) {
      ok = pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE;
    }
    gtext_css_property_value_free(&pv);
    return ok;
  }
  std::string cond(const GTEXT_CSS_Registry * reg, bool & truth, int depth, bool or_ok) {
    const uint8_t c = next();
    if (c % 5 == 0) {
      bool x;
      std::string s = leaf(reg, x, depth);
      truth = !x;
      return "not " + s;
    }
    const bool is_or = or_ok && c % 2 == 1;
    const size_t count = 1 + next() % 3;
    bool acc = !is_or;
    std::string out;
    for (size_t k = 0; k < count; k++) {
      bool x;
      std::string s = leaf(reg, x, depth);
      if (k) {
        out += is_or ? " or " : " and ";
      }
      out += s;
      acc = is_or ? (acc || x) : (acc && x);
    }
    truth = acc;
    return out;
  }
  std::string leaf(const GTEXT_CSS_Registry * reg, bool & truth, int depth) {
    const uint8_t c = next();
    if (depth > 0 && budget > 0 && c % 4 == 0) {
      budget--;
      bool t;
      std::string s = cond(reg, t, depth - 1, true);
      truth = t;
      return "(" + s + ")";
    }
    if (c % 9 == 0) {
      truth = false;  // <general-enclosed> is false in a supports condition
      return c % 2 ? "(foo bar)" : "unknown(1)";
    }
    std::string text;
    truth = decl(reg, text);
    return text;
  }
};

void check_supports(const uint8_t * payload, size_t size, const std::string & input) {
  const GTEXT_CSS_Registry * reg = registry();
  if (!reg) {
    return;
  }
  SupGen g{payload, size};
  bool want = false;
  const std::string text = g.cond(reg, want, 3, true);
  if (g.invalid) {
    want = false;
  }
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.max_work = 100000;
  GTEXT_CSS_Conditions * c = nullptr;
  if (gtext_css_conditions_new(reg, &co, &c) != GTEXT_CSS_OK) {
    fail("conditions_new failed", "", input);
  }
  GTEXT_CSS_Supports_Condition * sc = nullptr;
  if (gtext_css_supports_parse(text.data(), text.size(), false, nullptr, &sc, nullptr) != GTEXT_CSS_OK) {
    gtext_css_conditions_free(c);
    return;  // a limit
  }
  Tri t = GTEXT_CSS_TRI_UNKNOWN;
  const GTEXT_CSS_Status st = gtext_css_conditions_evaluate_supports(c, sc, nullptr, &t);
  if (st == GTEXT_CSS_OK && (t == GTEXT_CSS_TRI_TRUE) != want) {
    fail("a supports condition differs from the reference that parses each declaration directly", text + "\nreference " + (want ? "true" : "false"), input);
  }
  gtext_css_media_list_free(sc);
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
  if (!generate && (o0 & 4)) {
    check_supports(payload, psize, input);
    return 0;
  }
  if (generate) {
    Gen g{payload, psize};
    g.measure_mode = (o0 & 4) != 0;
    if (!(o0 & 2)) {
      // the laws of the logic, for two conditions of the generator that the payload left over
      Gen laws{payload + psize / 2, psize - psize / 2};
      laws.measure_mode = g.measure_mode;
      Tri ta, tb;
      const std::string a = laws.paren(ta, 2), b = laws.paren(tb, 2);
      check_laws(a, b, input);
    }
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
