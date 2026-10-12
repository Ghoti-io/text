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
 * Every tracked seed of the HTML tokenizer's fuzzer, run through the fuzzer's own properties.
 *
 * tests/fuzz/fuzz_html_tokenizer.cpp is a libFuzzer harness and needs clang and a
 * minute; the properties it checks - the answer is the same however the input is cut,
 * every token is well formed, a limit is its own status, an allocation that fails is
 * OOM or the normal answer and leaks nothing, the work is linear - do not. This test
 * includes the harness source as it is and calls its entry point on every file in
 * tests/fuzz/corpus/html-tokenizer, so `make test` (and test-asan, and test-tsan)
 * replays the corpus with no fuzzer built.
 *
 * That is also where **a reproducer lives once the fuzzer has found something**: a
 * file named `reproducer-*.seed` in the corpus directory is replayed here on every
 * run, which is the cheap half of "the fix is in these bytes".
 *
 * A harness aborts when a property fails; the handler below names the file first, so
 * the failure says which input and not only which property.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <dirent.h>
#include <unistd.h>

#include <gtest/gtest.h>

// The harness's own includes, first, so that including it inside a namespace below only
// re-opens guarded headers.
#include "../src/html/html_tokenizer_internal.h"
#include "html_tokenizer_test_support.h"
#include "markup_test_support.h"

namespace tokenizer_harness {
#define LLVMFuzzerTestOneInput tokenizer_entry
#include "fuzz/fuzz_html_tokenizer.cpp"
#undef LLVMFuzzerTestOneInput
} // namespace tokenizer_harness

namespace {

char g_current[512];

void name_the_input(int) {
  static const char head[] = "\ntest-html-tokenizer-fuzz-seeds: the input being replayed was ";
  (void) !write(2, head, sizeof(head) - 1);
  (void) !write(2, g_current, std::strlen(g_current));
  (void) !write(2, "\n", 1);
  std::signal(SIGABRT, SIG_DFL);
  std::raise(SIGABRT);
}

std::vector<std::string> seeds(const std::string & directory) {
  std::vector<std::string> out;
  DIR * d = opendir(directory.c_str());
  if (!d) {
    return out;
  }
  while (struct dirent * e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".seed") == 0) {
      out.push_back(directory + "/" + name);
    }
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

std::string slurp(const std::string & path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Where the corpus is, whatever the working directory: GTEXT_FUZZ_CORPUS if set, else the
// directory this file is in (when the compiler recorded an absolute path), the working
// directory's tests/, and the source tree above the executable (build/<os>/<mode>/apps/).
std::string corpus_dir() {
  std::vector<std::string> candidates;
  if (const char * env = std::getenv("GTEXT_FUZZ_CORPUS")) {
    candidates.push_back(env);
  }
  std::string here = __FILE__;
  const size_t slash = here.rfind('/');
  candidates.push_back((slash == std::string::npos ? std::string(".") : here.substr(0, slash)) + "/fuzz/corpus");
  candidates.push_back("tests/fuzz/corpus");
  char exe[4096];
  const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n > 0) {
    exe[n] = '\0';
    std::string path = exe;
    for (int up = 0; up < 5; up++) {
      path = path.substr(0, path.rfind('/'));
    }
    candidates.push_back(path + "/tests/fuzz/corpus");
  }
  for (const std::string & c : candidates) {
    if (DIR * d = opendir((c + "/html-tokenizer").c_str())) {
      closedir(d);
      return c;
    }
  }
  return "tests/fuzz/corpus";
}

class HtmlTokenizerFuzzSeeds : public ::testing::Test {
 protected:
  void SetUp() override {
    std::signal(SIGABRT, name_the_input);
  }
  void TearDown() override {
    std::signal(SIGABRT, SIG_DFL);
  }
};

TEST_F(HtmlTokenizerFuzzSeeds, EverySeedHoldsEveryProperty) {
  const std::string dir = corpus_dir() + "/html-tokenizer";
  const auto files = seeds(dir);
  ASSERT_GT(files.size(), 2000u) << "the seed corpus is missing from " << dir
                                 << "; tools/html/make-tokenizer-fuzz-seeds.py writes it";
  // The recorded finds are not generated, so nothing else notices one going missing.
  for (const char * name : {"reproducer-carriage-return-at-a-cut.seed", "reproducer-offset-after-a-cut.seed",
           "reproducer-stream-error-after-a-look-ahead.seed", "reproducer-attribute-table-leak.seed",
           "reproducer-whitespace-flag.seed", "reproducer-upper-case-tag-name.seed",
           "reproducer-two-byte-utf8.seed", "many-attributes.seed"}) {
    const std::string path = dir + "/" + name;
    ASSERT_TRUE(std::find(files.begin(), files.end(), path) != files.end()) << path << " is not there";
  }
  for (const std::string & path : files) {
    std::snprintf(g_current, sizeof g_current, "%s", path.c_str());
    const std::string data = slurp(path);
    tokenizer_harness::tokenizer_entry(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }
}

} // namespace
