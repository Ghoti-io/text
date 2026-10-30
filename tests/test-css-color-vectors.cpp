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
 * The computed-color vectors of web-platform-tests that compare approximately (tests/data/css/color-fuzzy.tsv, made by tools/css/extract-wpt-color-fuzzy.py):
 * `color-mix()` in every interpolation space, relative colors in every function, `none` and powerless components. A vector passes when the answer's numbers are
 * within the page's epsilon of the expected ones and the text without the numbers is equal (css/support/color-testcommon.js). The vectors the pages state and the draft
 * contradicts are listed below with the sentence, and they are the only ones allowed to fail.
 */

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

std::string find_file(const std::string & relative_to_tests) {
  std::vector<std::string> roots;
  const std::string here = __FILE__;
  const size_t slash = here.rfind('/');
  roots.push_back(slash == std::string::npos ? "." : here.substr(0, slash));
  roots.push_back("tests");
  roots.push_back("../..");
  for (const std::string & r : roots) {
    const std::string path = r + "/" + relative_to_tests;
    std::ifstream in(path, std::ios::binary);
    if (in) {
      return path;
    }
  }
  return "";
}

/// The numbers of a color text, and the text with the digits taken out (color-testcommon.js: `getNumbers`, `getNonNumbers`).
void split(const std::string & s, std::vector<double> & numbers, std::string & text) {
  text.clear();
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || c == '.')) {
      text.push_back(c);
    }
  }
  std::string token;
  auto flush = [&]() {
    if (!token.empty()) {
      char * end = nullptr;
      const double v = std::strtod(token.c_str(), &end);
      if (end != token.c_str()) {
        numbers.push_back(v);
      }
      token.clear();
    }
  };
  for (char c : s) {
    if (c == ' ' || c == '(' || c == ',') {
      flush();
    }
    else {
      token.push_back(c);
    }
  }
  flush();
}

bool fuzzy_equal(const std::string & got, const std::string & want, double eps) {
  std::vector<double> a, b;
  std::string ta, tb;
  split(got, a, ta);
  split(want, b, tb);
  if (ta != tb || a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (!(std::fabs(a[i] - b[i]) <= eps)) {
      return false;
    }
  }
  return true;
}

struct Vector {
  std::string page, specified, expected;
  double eps;
};

std::vector<Vector> load() {
  std::vector<Vector> out;
  const std::string path = find_file("data/css/color-fuzzy.tsv");
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '\t')) {
      f.push_back(cell);
    }
    if (f.size() == 4) {
      out.push_back({f[0], f[1], f[2], std::atof(f[3].c_str())});
    }
  }
  return out;
}

TEST(CssColorVectors, EveryApproximateVectorOfThePagesIsMet) {
  const std::vector<Vector> vectors = load();
  ASSERT_GT(vectors.size(), 3000u) << "tests/data/css/color-fuzzy.tsv (tools/css/extract-wpt-color-fuzzy.py)";
  std::map<std::string, size_t> failed, total;
  size_t shown = 0;
  for (const Vector & v : vectors) {
    total[v.page]++;
    const std::string got = root_computed("color: " + v.specified, "color");
    if (!fuzzy_equal(got, v.expected, v.eps)) {
      failed[v.page]++;
      if (shown++ < 100000) {
        ADD_FAILURE() << v.page << ": " << v.specified << "\n   got  " << got << "\n   want " << v.expected;
      }
    }
  }
  for (const auto & p : total) {
    std::fprintf(stderr, "%-45s %5zu vectors, %5zu failed\n", p.first.c_str(), p.second, failed[p.first]);
  }
}

} // namespace
