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
 * Every tracked seed of the transitions fuzzer, run through the fuzzer's own
 * properties.
 *
 * tests/fuzz/fuzz_css_transition.cpp is a libFuzzer harness and needs clang; the properties it checks - an update is OK or OOM and leaves the set as it was when it is OOM, no
 * property is both running and completed, a running transition ends no earlier than it starts, a style made from the same set at the same time twice is the same - do not. This test
 * includes the harness as it is and calls its entry point on every file in tests/fuzz/corpus/css-transition, so `make test` replays the corpus with no fuzzer built.
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

#include "css_test_support.h"
#include "markup_test_support.h"

#define LLVMFuzzerTestOneInput css_transition_entry
#include "fuzz/fuzz_css_transition.cpp"
#undef LLVMFuzzerTestOneInput

namespace {

char g_current[512];

void name_the_input(int) {
  std::fprintf(stderr, "\ntest-css-transition-fuzz-seeds: the input being replayed was %s\n", g_current);
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
    if (fs::is_directory(fs::path(c) / "css-transition", ec)) {
      return c;
    }
  }
  return "tests/fuzz/corpus";
}

class CssTransitionFuzzSeeds : public ::testing::Test {
 protected:
  void SetUp() override {
    std::signal(SIGABRT, name_the_input);
  }
  void TearDown() override {
    std::signal(SIGABRT, SIG_DFL);
  }
};

TEST_F(CssTransitionFuzzSeeds, EverySeedHoldsEveryProperty) {
  const std::string dir = corpus_dir() + "/css-transition";
  const auto files = seeds(dir);
  // The floor: tools/css/make-fuzz-transition-seeds.py writes 65.
  constexpr size_t kMinSeeds = 60;
  ASSERT_GE(files.size(), kMinSeeds) << "the seed corpus is missing from " << dir << "; tools/css/make-fuzz-transition-seeds.py writes it";
  const auto start = std::chrono::steady_clock::now();
  for (const std::string & path : files) {
    std::snprintf(g_current, sizeof g_current, "%s", path.c_str());
    const std::string data = slurp(path);
    css_transition_entry(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  RecordProperty("replay_seconds", std::to_string(seconds));
  EXPECT_LT(seconds, 60.0 * css_test::slowdown()) << files.size() << " seeds took " << seconds << " s";
}

} // namespace
