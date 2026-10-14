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
 * Every tracked seed of the public HTML API's fuzzer, run through the fuzzer's own properties.
 *
 * tests/fuzz/fuzz_html.cpp is a libFuzzer harness and needs clang and a minute; the
 * properties it checks - the answer is the same however the input is cut, the tree is a valid
 * HTML tree and passes the invariant checker, a limit is its own status, an allocation that
 * fails is OOM or the normal answer and leaks nothing, the work is linear - do not. This test
 * includes the harness source as it is and calls its entry point on every file in
 * tests/fuzz/corpus/html, so `make test` (and test-asan, and test-tsan) replays the corpus
 * with no fuzzer built.
 *
 * That is also where **a reproducer lives once the fuzzer has found something**: a file named
 * `reproducer-*.seed` in the corpus directory is replayed here on every run.
 *
 * A harness aborts when a property fails; the handler below names the file first, so the
 * failure says which input and not only which property.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// The harness's own includes, first, so that including it inside a namespace below only
// re-opens guarded headers.
#include "../src/html/html_tree_internal.h"
#include "../src/markup/markup_internal.h"
#include "../tools/conformance/html_tree_dump.h"
#include "html_api_test_support.h"
#include "markup_test_support.h"

namespace api_harness {
#define LLVMFuzzerTestOneInput api_entry
#include "fuzz/fuzz_html.cpp"
#undef LLVMFuzzerTestOneInput
} // namespace api_harness

namespace {

char g_current[512];

void name_the_input(int) {
  std::fprintf(stderr, "\ntest-html-api-fuzz-seeds: the input being replayed was %s\n", g_current);
  std::fflush(stderr);
  std::signal(SIGABRT, SIG_DFL);
  std::raise(SIGABRT);
}

namespace fs = std::filesystem;

std::vector<std::string> seeds(const std::string & directory) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto & entry : fs::directory_iterator(directory, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".seed") == 0) {
      out.push_back(entry.path().string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string slurp(const std::string & path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Where the corpus is, whatever the working directory: GTEXT_FUZZ_CORPUS if set, else the directory this
// file is in (when the compiler recorded a path that exists), the working directory's tests/, and the
// source tree above the working directory (build/<os>/<mode>/apps/ and the like).
std::string corpus_dir() {
  std::vector<std::string> candidates;
  if (const char * env = std::getenv("GTEXT_FUZZ_CORPUS")) {
    candidates.push_back(env);
  }
  candidates.push_back((fs::path(__FILE__).parent_path() / "fuzz" / "corpus").string());
  candidates.push_back("tests/fuzz/corpus");
  candidates.push_back("../tests/fuzz/corpus");
  candidates.push_back("../../tests/fuzz/corpus");
  for (const std::string & c : candidates) {
    std::error_code ec;
    if (fs::is_directory(fs::path(c) / "html", ec)) {
      return c;
    }
  }
  return "tests/fuzz/corpus";
}

class HtmlApiFuzzSeeds : public ::testing::Test {
 protected:
  void SetUp() override {
    std::signal(SIGABRT, name_the_input);
  }
  void TearDown() override {
    std::signal(SIGABRT, SIG_DFL);
  }
};

TEST_F(HtmlApiFuzzSeeds, EverySeedHoldsEveryProperty) {
  const std::string dir = corpus_dir() + "/html";
  const auto files = seeds(dir);
  // The floor. tools/html/make-api-fuzz-seeds.py reads this very line for the lower end of the size it accepts
  // (its upper end, 1,100, is its own), so the two cannot disagree.
  constexpr size_t kMinSeeds = 850;
  ASSERT_GE(files.size(), kMinSeeds) << "the seed corpus is missing from " << dir
                                 << "; tools/html/make-api-fuzz-seeds.py writes it";
  // The seeds of the API's own are not derived from the tree builder's, and the recorded finds are
  // not generated, so nothing else notices one going missing.
  for (const char * name : {"api-rich-document.seed", "api-bom-cut-byte-at-a-time.seed", "api-errors-beyond-the-cap.seed"}) {
    const std::string path = dir + "/" + name;
    ASSERT_TRUE(std::find(files.begin(), files.end(), path) != files.end()) << path << " is not there";
  }
  const auto start = std::chrono::steady_clock::now();
  for (const std::string & path : files) {
    std::snprintf(g_current, sizeof g_current, "%s", path.c_str());
    const std::string data = slurp(path);
    api_harness::api_entry(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }
  // The replay is part of `make test`, `test-asan` and `test-tsan`, and has to stay small in all three: about
  // a third of a second normally, and a few seconds under a sanitizer, on the development machine. A minute is
  // the bound; a seed that makes it slow is a find to be fixed or trimmed, not a limit to be raised.
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  RecordProperty("replay_seconds", std::to_string(seconds));
  EXPECT_LT(seconds, 60.0) << files.size() << " seeds took " << seconds << " s";
}

} // namespace
