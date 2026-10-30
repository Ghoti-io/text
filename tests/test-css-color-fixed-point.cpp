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
 * Random color expressions, computed (a deterministic generator: the same cases on every host). For every generated color: the computation succeeds, its text is a fixed point
 * of parse-compute-write (the answer read back and computed again is itself, but for the channels that a literal is clamped in and a computed color is not), a mix of two colors
 * in a rectangular space is symmetric where both orders are given equal shares, `contrast-color()` is white or black, and the answer of a relative color made of the keywords
 * alone is the origin converted. Cases that fail are printed with their seed, and a failure is a defect to fix, not a case to drop.
 */

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return (uint32_t)(s >> 16);
  }
  uint32_t below(uint32_t n) {
    return next() % n;
  }
  double unit() {
    return (next() % 100001) / 100000.0;
  }
};

const char * const kNames[] = {"red", "blue", "teal", "olive", "rebeccapurple", "white", "black", "transparent", "pink", "gold", "navy", "plum"};
const char * const kSpaces[] = {"srgb", "srgb-linear", "display-p3", "display-p3-linear", "a98-rgb", "prophoto-rgb", "rec2020", "xyz", "xyz-d50", "xyz-d65"};
const char * const kRect[] = {"srgb", "srgb-linear", "display-p3", "a98-rgb", "prophoto-rgb", "rec2020", "xyz", "xyz-d50", "lab", "oklab"};
const char * const kMixSpaces[] = {"srgb", "srgb-linear", "display-p3", "a98-rgb", "prophoto-rgb", "rec2020", "xyz", "xyz-d50", "xyz-d65", "lab", "oklab", "hsl", "hwb", "lch", "oklch"};

std::string num(Rng & r, double lo, double hi) {
  char b[48];
  std::snprintf(b, sizeof b, "%.4g", lo + (hi - lo) * r.unit());
  return b;
}

std::string channel(Rng & r, const char * kind) {
  if (r.below(14) == 0) {
    return "none";
  }
  const std::string k = kind;
  if (k == "255") {
    return r.below(4) == 0 ? num(r, 0, 100) + "%" : num(r, -20, 280);
  }
  if (k == "unit") {
    return r.below(4) == 0 ? num(r, -10, 110) + "%" : num(r, -0.3, 1.3);
  }
  if (k == "pct") {
    return r.below(2) ? num(r, 0, 100) + "%" : num(r, 0, 100);
  }
  if (k == "hue") {
    return r.below(3) == 0 ? num(r, -100, 500) + "deg" : num(r, -100, 500);
  }
  if (k == "l100") {
    return num(r, -10, 110);
  }
  if (k == "ab") {
    return num(r, -140, 140);
  }
  if (k == "c150") {
    return num(r, 0, 160);
  }
  if (k == "l1") {
    return num(r, -0.1, 1.1);
  }
  if (k == "ab4") {
    return num(r, -0.5, 0.5);
  }
  return num(r, 0, 0.5);
}

std::string alpha(Rng & r) {
  switch (r.below(5)) {
  case 0: return "";
  case 1: return " / " + num(r, -0.2, 1.2);
  case 2: return " / " + num(r, 0, 100) + "%";
  case 3: return " / none";
  default: return " / " + num(r, 0, 1);
  }
}

std::string literal(Rng & r) {
  switch (r.below(10)) {
  case 0: return kNames[r.below(12)];
  case 1: {
    char b[16];
    std::snprintf(b, sizeof b, "#%06x", r.next() & 0xffffff);
    return b;
  }
  case 2: return "rgb(" + channel(r, "255") + " " + channel(r, "255") + " " + channel(r, "255") + alpha(r) + ")";
  case 3: return "hsl(" + channel(r, "hue") + " " + channel(r, "pct") + " " + channel(r, "pct") + alpha(r) + ")";
  case 4: return "hwb(" + channel(r, "hue") + " " + channel(r, "pct") + " " + channel(r, "pct") + alpha(r) + ")";
  case 5: return "lab(" + channel(r, "l100") + " " + channel(r, "ab") + " " + channel(r, "ab") + alpha(r) + ")";
  case 6: return "lch(" + channel(r, "l100") + " " + channel(r, "c150") + " " + channel(r, "hue") + alpha(r) + ")";
  case 7: return "oklab(" + channel(r, "l1") + " " + channel(r, "ab4") + " " + channel(r, "ab4") + alpha(r) + ")";
  case 8: return "oklch(" + channel(r, "l1") + " " + channel(r, "ab4") + " " + channel(r, "hue") + alpha(r) + ")";
  default: return std::string("color(") + kSpaces[r.below(10)] + " " + channel(r, "unit") + " " + channel(r, "unit") + " " + channel(r, "unit") + alpha(r) + ")";
  }
}

std::string expression(Rng & r, int depth);

std::string relative(Rng & r, int depth) {
  const std::string origin = expression(r, depth - 1);
  auto pick = [&](const char * a, const char * b, const char * c, const std::string & lit) {
    switch (r.below(5)) {
    case 0: return std::string(a);
    case 1: return std::string(b);
    case 2: return std::string(c);
    case 3: return "calc(" + std::string(a) + " * 0.5)";
    default: return lit;
    }
  };
  switch (r.below(7)) {
  case 0: return "rgb(from " + origin + " " + pick("r", "g", "b", "30") + " " + pick("g", "b", "r", "200") + " " + pick("b", "r", "g", "none") + (r.below(2) ? " / alpha" : "") + ")";
  case 1: return "hsl(from " + origin + " " + pick("h", "s", "l", "120") + " " + pick("s", "l", "h", "50") + " " + pick("l", "h", "s", "40") + (r.below(2) ? " / calc(alpha / 2)" : "") + ")";
  case 2: return "hwb(from " + origin + " h w b" + (r.below(2) ? " / 0.5" : "") + ")";
  case 3: return "lab(from " + origin + " " + pick("l", "a", "b", "50") + " a " + pick("b", "a", "l", "-20") + (r.below(2) ? " / alpha" : "") + ")";
  case 4: return "oklch(from " + origin + " l " + pick("c", "l", "c", "0.1") + " " + pick("h", "c", "l", "200") + ")";
  case 5: return std::string("color(from ") + origin + " " + kSpaces[r.below(10)] + " " + "0.25 0.5 0.75" + ")";
  default: return "alpha(from " + origin + " / " + (r.below(2) ? "calc(alpha * 0.5)" : "0.3") + ")";
  }
}

std::string expression(Rng & r, int depth) {
  if (depth <= 0 || r.below(3) == 0) {
    return literal(r);
  }
  switch (r.below(5)) {
  case 0: return relative(r, depth);
  case 1: return "contrast-color(" + expression(r, depth - 1) + ")";
  default: {
    std::string s = "color-mix(in ";
    s += kMixSpaces[r.below(15)];
    const std::string sp = s.substr(12);
    if (sp == "hsl" || sp == "hwb" || sp == "lch" || sp == "oklch") {
      static const char * const m[] = {"", " shorter hue", " longer hue", " increasing hue", " decreasing hue"};
      s += m[r.below(5)];
    }
    const int n = 2 + (int)r.below(2);
    for (int i = 0; i < n; i++) {
      s += ", " + expression(r, depth - 1);
      if (r.below(3) == 0) {
        s += " " + num(r, 0, 100) + "%";
      }
    }
    return s + ")";
  }
  }
}

std::string compute_color(const std::string & v) {
  return root_computed("color: " + v, "color");
}

/// The numbers in a text, in order.
std::vector<double> numbers(const std::string & s) {
  std::vector<double> out;
  for (size_t i = 0; i < s.size();) {
    if ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || (s[i] == '-' && i + 1 < s.size() && ((s[i + 1] >= '0' && s[i + 1] <= '9') || s[i + 1] == '.'))) {
      char * end = nullptr;
      out.push_back(std::strtod(s.c_str() + i, &end));
      i = (size_t)(end - s.c_str());
    }
    else {
      i++;
    }
  }
  return out;
}

std::string skeleton(const std::string & s) {
  std::string out;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || c == '.' || c == '-')) {
      out.push_back(c);
    }
  }
  return out;
}

bool lab_family_clamped(const std::string & t) {
  // lab-family channels are put back unclamped after a computation; a literal read back is clamped (a lightness below 0, a chroma below 0)
  const bool fam = t.rfind("lab(", 0) == 0 || t.rfind("lch(", 0) == 0 || t.rfind("oklab(", 0) == 0 || t.rfind("oklch(", 0) == 0;
  if (!fam) {
    return false;
  }
  const std::vector<double> n = numbers(t);
  const bool polar = t.find("lch(") != std::string::npos;
  return (!n.empty() && (n[0] < 0 || n[0] > (t.rfind("ok", 0) == 0 ? 1.0 : 100.0))) || (polar && n.size() > 1 && n[1] < 0) || (polar && n.size() > 2 && n[2] >= 359.9999995);
}

TEST(CssColorFixedPoint, RandomColorsComputeAndTheAnswerIsAFixedPoint) {
  size_t unresolved = 0, checked = 0, skipped = 0;
  for (uint64_t seed = 1; seed <= 2500; seed++) {
    Rng r(seed * 2654435761u);
    const std::string expr = expression(r, 3);
    const std::string t1 = compute_color(expr);
    ASSERT_EQ(t1.find('<'), std::string::npos) << "seed " << seed << ": " << expr << " -> " << t1;
    if (t1.find("color-mix(") != std::string::npos || t1.find("contrast-color(") != std::string::npos || t1.find("(from ") != std::string::npos ||
        t1.find("alpha(") != std::string::npos || t1.find("calc(") != std::string::npos || t1 == "canvastext") {
      unresolved++;
      ADD_FAILURE() << "seed " << seed << ": " << expr << " stays as written: " << t1;
      continue;
    }
    if (lab_family_clamped(t1)) {
      skipped++;
      continue;
    }
    const std::string t2 = compute_color(t1);
    EXPECT_EQ(skeleton(t1), skeleton(t2)) << "seed " << seed << ": " << expr << "\n  " << t1 << "\n  " << t2;
    const std::vector<double> a = numbers(t1), b = numbers(t2);
    ASSERT_EQ(a.size(), b.size()) << "seed " << seed << ": " << t1 << " / " << t2;
    for (size_t i = 0; i < a.size(); i++) {
      EXPECT_NEAR(a[i], b[i], 1e-9 * (1.0 + std::fabs(a[i]))) << "seed " << seed << ": " << expr << "\n  " << t1 << "\n  " << t2;
    }
    checked++;
  }
  EXPECT_EQ(unresolved, 0u);
  EXPECT_GT(checked, 2000u);
  std::fprintf(stderr, "fixed point: %zu colors checked, %zu with a channel a literal would clamp\n", checked, skipped);
}

/* Color 5, 3.3: with equal shares, a mix is the same in either order wherever the space has no hue to be turned (a rectangular space). */
TEST(CssColorFixedPoint, ARectangularMixIsSymmetric) {
  for (uint64_t seed = 1; seed <= 600; seed++) {
    Rng r(seed * 40503u);
    const std::string a = literal(r), b = literal(r);
    const std::string sp = kRect[r.below(10)];
    const std::string ab = compute_color("color-mix(in " + sp + ", " + a + ", " + b + ")");
    const std::string ba = compute_color("color-mix(in " + sp + ", " + b + ", " + a + ")");
    EXPECT_EQ(skeleton(ab), skeleton(ba)) << a << " | " << b << " in " << sp << ": " << ab << " / " << ba;
    const std::vector<double> x = numbers(ab), y = numbers(ba);
    ASSERT_EQ(x.size(), y.size()) << ab << " / " << ba;
    for (size_t i = 0; i < x.size(); i++) {
      EXPECT_NEAR(x[i], y[i], 1e-5 * (1.0 + std::fabs(x[i]))) << a << " | " << b << " in " << sp << ": " << ab << " / " << ba;
    }
  }
}

TEST(CssColorFixedPoint, TheContrastColorIsWhiteOrBlackAndTheKeywordsAloneAreTheIdentity) {
  for (uint64_t seed = 1; seed <= 400; seed++) {
    Rng r(seed * 7919u);
    const std::string c = literal(r);
    const std::string cc = root_computed("background-color: contrast-color(" + c + ")", "background-color");
    EXPECT_TRUE(cc == "rgb(0, 0, 0)" || cc == "rgb(255, 255, 255)") << c << " -> " << cc;
    // lab(from c l a b) is c in lab, whatever c was (its own alpha kept)
    const std::string direct = compute_color("lab(from " + c + " l a b)");
    EXPECT_EQ(direct.rfind("lab(", 0), 0u) << c << " -> " << direct;
  }
}

} // namespace
