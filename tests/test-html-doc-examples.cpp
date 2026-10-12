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
 * The code in the HTML manual (documentation/formats/html.md), compiled and run.
 *
 * Every ```c block in the manual is a file in tests/doc-snippets/html/, and this test holds the two
 * together in both directions: the file is compiled here (as C++, so each is written in the common subset
 * of the two languages), run, and its output compared with what the manual says it prints; and the manual
 * is read, and each of its ```c blocks must be one of the files, byte for byte, with none left over. A
 * snippet cannot be edited in the manual without the test failing until the file says the same, and a
 * block cannot be added to the manual without a file and an expected output here.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

namespace snippets {
#include "doc-snippets/html/quick-start.c"
#include "doc-snippets/html/errors.c"
#include "doc-snippets/html/incremental.c"
#include "doc-snippets/html/fragment.c"
#include "doc-snippets/html/file.c"
#include "doc-snippets/html/limits.c"
#include "doc-snippets/html/encoding.c"
#include "doc-snippets/html/scripting.c"
} // namespace snippets

namespace {

struct Snippet {
  const char * file;
  void (*run)();
  const char * output;
};

const Snippet kSnippets[] = {
    {"quick-start.c", snippets::quick_start, "no-quirks mode, 1 parse errors, read as UTF-8\ntitle: Hi\n"},
    {"errors.c", snippets::errors,
        "1:1 (byte 0) missing-doctype\n1:13 (byte 12) open-elements-at-end-tag\n2:1 (byte 17) unexpected-end-tag\n"
        "2:13 (byte 29) duplicate-attribute\nand 1 more\n"},
    {"incremental.c", snippets::incremental, "9 nodes, 45 bytes\n"},
    {"fragment.c", snippets::fragment, "2 children\nthe cell now has 2 children\n"},
    {"file.c", snippets::file, "read 40 bytes\ninvalid argument: No such file or directory\n"},
    {"limits.c", snippets::limits, "refused: too many elements open at once\naccepted under the default limit\n"},
    {"encoding.c", snippets::encoding, "UTF-8, from a byte order mark: 1\nunsupported encoding\n"},
    {"scripting.c", snippets::scripting, "scripting 0: the noscript holds a <p> element\nscripting 1: the noscript holds the text \"<p>enable scripts</p>\"\n"},
};

std::string slurp(const std::string & path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The directory this file is in, whatever the working directory.
std::vector<std::string> roots() {
  std::vector<std::string> out;
  std::string here = __FILE__;
  const size_t slash = here.rfind('/');
  out.push_back(slash == std::string::npos ? "." : here.substr(0, slash));
  out.push_back("tests");
  out.push_back("../..");
  return out;
}

std::string find_file(const std::string & relative_to_tests) {
  for (const std::string & r : roots()) {
    const std::string path = r + "/" + relative_to_tests;
    std::ifstream in(path, std::ios::binary);
    if (in) {
      return path;
    }
  }
  return "";
}

TEST(HtmlDocExamples, EveryFencedCBlockIsASnippetAndEverySnippetIsInTheManual) {
  const std::string manual_path = find_file("../documentation/formats/html.md");
  ASSERT_NE(manual_path, "") << "documentation/formats/html.md is not where the tests can find it";
  const std::string manual = slurp(manual_path);

  // The ```c blocks of the manual, in order.
  std::vector<std::string> blocks;
  size_t at = 0;
  for (;;) {
    const size_t open = manual.find("\n```c\n", at);
    if (open == std::string::npos) {
      break;
    }
    const size_t body = open + 6;
    const size_t close = manual.find("\n```\n", body - 1);
    ASSERT_NE(close, std::string::npos) << "a ```c block is never closed";
    blocks.push_back(manual.substr(body, close + 1 - body));
    at = close + 4;
  }
  // No fence of another spelling that is C in disguise.
  EXPECT_EQ(manual.find("```C\n"), std::string::npos);
  EXPECT_EQ(manual.find("```cpp"), std::string::npos);

  EXPECT_EQ(blocks.size(), sizeof(kSnippets) / sizeof(kSnippets[0]))
      << "every ```c block in the manual is a snippet that is compiled and run";
  for (const Snippet & s : kSnippets) {
    const std::string path = find_file(std::string("doc-snippets/html/") + s.file);
    ASSERT_NE(path, "") << s.file;
    const std::string source = slurp(path);
    bool found = false;
    for (const std::string & b : blocks) {
      found = found || b == source;
    }
    EXPECT_TRUE(found) << s.file << " is not in the manual, byte for byte";
  }
}

TEST(HtmlDocExamples, EverySnippetRunsAndPrintsWhatTheManualSays) {
  for (const Snippet & s : kSnippets) {
    SCOPED_TRACE(s.file);
    testing::internal::CaptureStdout();
    s.run();
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(out, s.output);
  }
  // The snippet that writes a file takes it away again.
  std::ifstream left("doc-example.html");
  EXPECT_FALSE(left.good());
}

} // namespace
