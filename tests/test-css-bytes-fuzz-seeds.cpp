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
 * Every tracked seed of the CSS bytes fuzzer, run through the fuzzer's own
 * properties.
 *
 * tests/fuzz/fuzz_css_bytes.cpp is a libFuzzer harness and needs clang; the
 * properties it checks - positions in the bytes, the same answer at every cut,
 * the input limit exact, a failed allocation is OOM or the same answer and
 * leaks nothing, the work is linear - do not. This test includes the harness as it is and calls
 * its entry point on every file in tests/fuzz/corpus/css-bytes, so `make
 * test` (and test-asan, and test-tsan) replays the corpus with no fuzzer
 * built. A reproducer the fuzzer finds is kept there as `reproducer-*.seed`
 * and is replayed here on every run.
 *
 * A harness aborts when a property fails; the handler names the file first.
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

#include "../tools/conformance/css_tree_canon.h"
#include "markup_test_support.h"

#define LLVMFuzzerTestOneInput css_bytes_entry
#include "fuzz/fuzz_css_bytes.cpp"
#undef LLVMFuzzerTestOneInput

namespace {

char g_current[512];

void name_the_input(int) {
  std::fprintf(stderr, "\ntest-css-bytes-fuzz-seeds: the input being replayed was %s\n", g_current);
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
      out.push_back(entry.path().generic_string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string slurp(const std::string & path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string corpus_dir() {
  std::vector<std::string> candidates;
  if (const char * env = std::getenv("GTEXT_FUZZ_CORPUS")) {
    candidates.push_back(env);
  }
  candidates.push_back((fs::path(__FILE__).parent_path() / "fuzz" / "corpus").generic_string());
  candidates.push_back("tests/fuzz/corpus");
  candidates.push_back("../tests/fuzz/corpus");
  candidates.push_back("../../tests/fuzz/corpus");
  for (const std::string & c : candidates) {
    std::error_code ec;
    if (fs::is_directory(fs::path(c) / "css-bytes", ec)) {
      return c;
    }
  }
  return "tests/fuzz/corpus";
}

// Whether a sanitizer or valgrind runs this program (valgrind preloads its own library into the process).
bool slow_tooling() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
  return true;
#else
  const char * preload = std::getenv("LD_PRELOAD");
  return preload != nullptr && std::strstr(preload, "vgpreload") != nullptr;
#endif
}

class CssBytesFuzzSeeds : public ::testing::Test {
 protected:
  void SetUp() override {
    std::signal(SIGABRT, name_the_input);
  }
  void TearDown() override {
    std::signal(SIGABRT, SIG_DFL);
  }
};

TEST_F(CssBytesFuzzSeeds, EverySeedHoldsEveryProperty) {
  const std::string dir = corpus_dir() + "/css-bytes";
  const auto files = seeds(dir);
  // The floor: tools/css/make-fuzz-bytes-seeds.py writes 556 from the pinned suite and its own list.
  constexpr size_t kMinSeeds = 500;
  ASSERT_GE(files.size(), kMinSeeds) << "the seed corpus is missing from " << dir << "; tools/css/make-fuzz-bytes-seeds.py writes it";
  for (const char * name : {"own-cp1251-charset.seed", "own-iso2022jp-trailing-shift-oom-3.seed", "own-charset-ends-1024.seed",
           "own-utf16le-long-cut-1.seed", "stylesheet-bytes-005.seed", "reproducer-iso2022jp-zero-width-replacement.seed"}) { // (what check-css-fuzz-plants replays is among them)
    const std::string path = dir + "/" + name;
    ASSERT_TRUE(std::find(files.begin(), files.end(), path) != files.end()) << path << " is not there";
  }
  const auto start = std::chrono::steady_clock::now();
  for (const std::string & path : files) {
    std::snprintf(g_current, sizeof g_current, "%s", path.c_str());
    const std::string data = slurp(path);
    css_bytes_entry(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  RecordProperty("replay_seconds", std::to_string(seconds));
  // (The bound is for a hang or a quadratic step. A sanitizer or valgrind makes the same work tens of times slower: 95 s under
  // ASan and 176 s under TSan on a 32-thread host, where the release build takes a few seconds.)
  EXPECT_LT(seconds, slow_tooling() ? 3600.0 : 60.0) << files.size() << " seeds took " << seconds << " s";
}

} // namespace
