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
 * The code in the CSS conditions and at-rules manuals (documentation/formats/css-conditions.md and css-atrules.md), compiled and run.
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

namespace snippets {
#include "doc-snippets/css/conditions-media.c"
#include "doc-snippets/css/conditions-supports.c"
#include "doc-snippets/css/atrule-counter-style.c"
#include "doc-snippets/css/atrule-property.c"
} // namespace snippets

namespace {

struct Snippet {
  const char * manual;
  const char * file;
  void (*run)();
  const char * output;
};

const Snippet kSnippets[] = {
    {"css-conditions.md", "conditions-media.c", snippets::conditions_media,
        "(min-width: 40em): false\n(width > 700px): false\n(color: foo) or (color): true\nnot all: false\n"},
    {"css-conditions.md", "conditions-supports.c", snippets::conditions_supports,
        "(display: grid): supported\n(display: bogus): not supported\nnot (display: bogus): supported\n(--x: a;b): not supported\nselector(:is(a, b)): supported\n(foo: red}): not supported\n"},
    {"css-atrules.md", "atrule-counter-style.c", snippets::atrule_counter_style,
        "3 descriptors kept, 2 diagnostics\n@counter-style roman { system: additive; additive-symbols: 10 X, 5 V, 1 I; suffix: \". \"; }\n"},
    {"css-atrules.md", "atrule-property.c", snippets::atrule_property, "rule 1: valid\nrule 2: invalid\nrule 3: valid\nrule 4: invalid\n"},
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

struct Block {
  std::string code;
  std::string output;
};

bool read_blocks(const char * name, std::vector<Block> & blocks, std::string & manual) {
  const std::string path = find_file(std::string("../documentation/formats/") + name);
  if (path.empty()) {
    return false;
  }
  manual = slurp(path);
  size_t at = 0;
  for (;;) {
    const size_t open = manual.find("\n```c\n", at);
    if (open == std::string::npos) {
      break;
    }
    const size_t body = open + 6;
    const size_t close = manual.find("\n```\n", body - 1);
    if (close == std::string::npos) {
      return false;
    }
    Block b;
    b.code = manual.substr(body, close + 1 - body);
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
  return true;
}

TEST(CssConditionsDocExamples, EveryFencedCBlockIsASnippetAndItsOutputIsTheBlockAfterIt) {
  for (const char * name : {"css-conditions.md", "css-atrules.md"}) {
    SCOPED_TRACE(name);
    std::vector<Block> blocks;
    std::string manual;
    ASSERT_TRUE(read_blocks(name, blocks, manual)) << name << " cannot be read";
    EXPECT_EQ(manual.find("```cpp"), std::string::npos);
    size_t expected = 0;
    for (const Snippet & s : kSnippets) {
      if (std::string(s.manual) != name) {
        continue;
      }
      expected++;
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
    EXPECT_EQ(blocks.size(), expected) << "every ```c block is a snippet that is compiled and run";
  }
}

TEST(CssConditionsDocExamples, EverySnippetRunsAndPrintsWhatTheManualSays) {
  for (const Snippet & s : kSnippets) {
    SCOPED_TRACE(s.file);
    testing::internal::CaptureStdout();
    s.run();
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(out, s.output);
  }
}

} // namespace
