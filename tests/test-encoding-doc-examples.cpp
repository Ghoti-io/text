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
 * The code in the Encoding manual (documentation/formats/encoding.md), compiled and run.
 *
 * Every ```c block in the manual is a file in tests/doc-snippets/encoding/, and this test holds the two together in both
 * directions: the file is compiled here (as C++, so each is written in the common subset of the two languages), run, and its
 * output compared with what the manual says it prints; and the manual is read, and each of its ```c blocks must be one of the
 * files, byte for byte, with none left over.
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

#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

namespace snippets {
#include "doc-snippets/encoding/quick-start.c"
#include "doc-snippets/encoding/streaming.c"
#include "doc-snippets/encoding/fatal.c"
#include "doc-snippets/encoding/bom.c"
#include "doc-snippets/encoding/encode.c"
#include "doc-snippets/encoding/limits.c"
#include "doc-snippets/encoding/cjk-decode.c"
#include "doc-snippets/encoding/iso-2022-jp-encode.c"
} // namespace snippets

namespace {

struct Snippet {
  const char * file;
  void (*run)();
  const char * output;
};

const Snippet kSnippets[] = {
    {"quick-start.c", snippets::quick_start, "windows-1252\ncaf\xC3\xA9 \xE2\x82\xAC" "5 (10 bytes of UTF-8, 0 replaced)\n"},
    {"streaming.c", snippets::streaming, "[price ][\xE2\x82\xAC 5]\n"},
    {"fatal.c", snippets::fatal, "replacement mode: ok, 1 replaced\nfatal mode: malformed input at byte 5\n"},
    {"bom.c", snippets::bom, "UTF-16LE, from a byte order mark: 1, text: hi\n"},
    {"encode.c", snippets::encode,
        "ISO-8859-2: price &#8364;5 (1 written as a reference)\nwindows-1252: the euro sign is byte 0x80, 0 references\n"
        "fatal: a code point the encoding cannot represent, U+20AC at byte 6\n"},
    {"limits.c", snippets::limits, "output limit exceeded after 4 bytes of output\n"},
    {"cjk-decode.c", snippets::cjk_decode, "Shift_JIS: \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xEF\xBF\xBD end (1 replaced)\n"
        "gb18030: 5 bytes of UTF-8 for 5 bytes (U+0080 and U+20AC), 0 replaced\n"},
    {"iso-2022-jp-encode.c", snippets::iso_2022_jp_encode, "<ESC>$B$\"<ESC>(B&#233; (14 bytes, 1 written as a reference)\n"},
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

TEST(EncodingDocExamples, EveryFencedCBlockIsASnippetAndEverySnippetIsInTheManual) {
  const std::string manual_path = find_file("../documentation/formats/encoding.md");
  ASSERT_NE(manual_path, "") << "documentation/formats/encoding.md is not where the tests can find it";
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
    const std::string path = find_file(std::string("doc-snippets/encoding/") + s.file);
    ASSERT_NE(path, "") << s.file;
    const std::string source = slurp(path);
    bool found = false;
    for (const std::string & b : blocks) {
      found = found || b == source;
    }
    EXPECT_TRUE(found) << s.file << " is not in the manual, byte for byte";
  }
}

TEST(EncodingDocExamples, EverySnippetRunsAndPrintsWhatTheManualSays) {
  for (const Snippet & s : kSnippets) {
    SCOPED_TRACE(s.file);
    testing::internal::CaptureStdout();
    s.run();
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(out, s.output);
  }
}

} // namespace
