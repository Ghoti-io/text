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
 * The code in the CSS custom properties manual (documentation/formats/css-variables.md), compiled and run.
 *
 * Every ```c block in the manual is a file in tests/doc-snippets/css/, and this test holds the two together
 * in both directions: each file is compiled here (as C++, so each is written in the common subset of the two
 * languages), run, and its output compared with what the manual says it prints; and the manual is read, and
 * each of its ```c blocks must be one of the files, byte for byte, with none left over, and the block after
 * each must be the output this test expects.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

// each snippet is a program of its own, with helpers of its own that another may name the same
namespace quick_start {
#include "doc-snippets/css/variables-quick-start.c"
}
namespace registered {
#include "doc-snippets/css/variables-registered.c"
}
namespace property_rule {
#include "doc-snippets/css/variables-property-rule.c"
}
namespace budget {
#include "doc-snippets/css/variables-budget.c"
}

namespace {

struct Snippet {
  const char * file;
  void (*run)();
  const char * output;
};

const Snippet kSnippets[] = {
    {"variables-quick-start.c", quick_start::variables_quick_start,
        "--pad: 4px 8px 2px\n--cycle-a: (the guaranteed-invalid value)\nmargin-top: 4px\nmargin-right: 8px\nwidth: 10px\nheight: 3px\n"},
    {"variables-registered.c", registered::variables_registered,
        "--len: 40px\n--len: 0px\n"},
    {"variables-property-rule.c", property_rule::variables_property_rule,
        "1 rule registered\nignored --broken: no-syntax\n--size: syntax <length>, initial 16px\nwidth: 16px\n"},
    {"variables-budget.c", budget::variables_budget,
        "--v19 has a value\n--v20 is guaranteed-invalid\n--v30 is guaranteed-invalid\nvalues stopped by the budget: 1\nwork under 10000: yes\n"},
};

std::string slurp(const std::string & path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

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

TEST(CssVariablesDocExamples, EveryFencedCBlockIsASnippetAndItsOutputIsTheBlockAfterIt) {
  const std::string manual_path = find_file("../documentation/formats/css-variables.md");
  ASSERT_NE(manual_path, "") << "documentation/formats/css-variables.md is not where the tests can find it";
  const std::string manual = slurp(manual_path);
  struct Block {
    std::string code;
    std::string output;
  };
  std::vector<Block> blocks;
  size_t at = 0;
  for (;;) {
    const size_t open = manual.find("\n```c\n", at);
    if (open == std::string::npos) {
      break;
    }
    const size_t body = open + 6;
    const size_t close = manual.find("\n```\n", body - 1);
    ASSERT_NE(close, std::string::npos) << "a ```c block is never closed";
    Block b;
    b.code = manual.substr(body, close + 1 - body);
    // The output: the next plain ``` block, after "prints".
    const size_t out_open = manual.find("\n```\n", close + 4);
    const size_t prints = manual.find("prints", close);
    if (out_open != std::string::npos && prints != std::string::npos && prints < out_open) {
      const size_t out_body = out_open + 5;
      const size_t out_close = manual.find("\n```\n", out_body - 1);
      if (out_close != std::string::npos) {
        b.output = manual.substr(out_body, out_close + 1 - out_body);
      }
    }
    blocks.push_back(b);
    at = close + 4;
  }
  EXPECT_EQ(manual.find("```cpp"), std::string::npos);
  EXPECT_EQ(blocks.size(), sizeof(kSnippets) / sizeof(kSnippets[0])) << "every ```c block is a snippet that is compiled and run";
  for (const Snippet & s : kSnippets) {
    const std::string path = find_file(std::string("doc-snippets/css/") + s.file);
    ASSERT_NE(path, "") << s.file;
    const std::string source = slurp(path);
    bool found = false;
    for (const Block & b : blocks) {
      if (b.code == source) {
        found = true;
        EXPECT_EQ(b.output, s.output) << s.file << ": the manual says it prints something else";
      }
    }
    EXPECT_TRUE(found) << s.file << " is not in the manual, byte for byte";
  }
}

TEST(CssVariablesDocExamples, EverySnippetRunsAndPrintsWhatTheManualSays) {
  for (const Snippet & s : kSnippets) {
    SCOPED_TRACE(s.file);
    testing::internal::CaptureStdout();
    s.run();
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(out, s.output);
  }
}

} // namespace
