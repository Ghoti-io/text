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
 * Every tracked seed of the fuzzer of the document layer, run through the fuzzer's own properties.
 *
 * tests/fuzz/fuzz_cssom_document.cpp is a libFuzzer harness and needs clang; the properties it checks do not. This test includes the harness as it is and calls its entry point on every
 * file in tests/fuzz/corpus/cssom-document, so `make test` replays the corpus with no fuzzer built. A reproducer the fuzzer finds is kept there as `reproducer-*.seed`.
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

#include "markup_test_support.h"

#define LLVMFuzzerTestOneInput cssom_document_entry
#include "fuzz/fuzz_cssom_document.cpp"
#undef LLVMFuzzerTestOneInput

namespace {

char g_current[512];

void name_the_input(int) {
  std::fprintf(stderr, "\ntest-cssom-document-fuzz-seeds: the input being replayed was %s\n", g_current);
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
    if (fs::is_directory(fs::path(c) / "cssom-document", ec)) {
      return c;
    }
  }
  return "tests/fuzz/corpus";
}

class CssomDocumentFuzzSeeds : public ::testing::Test {
 protected:
  void SetUp() override {
    std::signal(SIGABRT, name_the_input);
  }
  void TearDown() override {
    std::signal(SIGABRT, SIG_DFL);
  }
};

TEST_F(CssomDocumentFuzzSeeds, EverySeedHoldsEveryProperty) {
  const std::string dir = corpus_dir() + "/cssom-document";
  const auto files = seeds(dir);
  // The floor: tools/css/make-fuzz-document-cssom-seeds.py writes about 270: sheets that use every form the compiler reads, and invalid versions of each, put to trees of several shapes with the options varied.
  constexpr size_t kMinSeeds = 60;
  ASSERT_GE(files.size(), kMinSeeds) << "the seed corpus is missing from " << dir << "; tools/css/make-fuzz-document-cssom-seeds.py writes it";
  const auto start = std::chrono::steady_clock::now();
  for (const std::string & path : files) {
    std::snprintf(g_current, sizeof g_current, "%s", path.c_str());
    const std::string data = slurp(path);
    cssom_document_entry(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  RecordProperty("replay_seconds", std::to_string(seconds));
  const char * slow_env = std::getenv("GTEXT_TEST_SLOWDOWN");   // a known slow host (an emulator, Valgrind, a sanitizer) multiplies the bound, as the other seed replays do
  const double slow = slow_env && std::atof(slow_env) >= 1.0 && std::atof(slow_env) <= 1000.0 ? std::atof(slow_env) : 1.0;
  EXPECT_LT(seconds, 120.0 * slow) << files.size() << " seeds took " << seconds << " s";
}

} // namespace
