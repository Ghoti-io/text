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
 * Every tracked seed of the mutation fuzzer of the rule layer, run through the fuzzer's own properties.
 *
 * tests/fuzz/fuzz_cssom_sheet.cpp is a libFuzzer harness and needs clang; the properties it checks do not. This test includes the harness as it is and calls its entry point on every
 * file in tests/fuzz/corpus/cssom-sheet, so `make test` replays the corpus with no fuzzer built. A reproducer the fuzzer finds is kept there as `reproducer-*.seed`.
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

#define LLVMFuzzerTestOneInput cssom_sheet_entry
#include "fuzz/fuzz_cssom_sheet.cpp"
#undef LLVMFuzzerTestOneInput

namespace {

char g_current[512];

void name_the_input(int) {
  std::fprintf(stderr, "\ntest-cssom-sheet-fuzz-seeds: the input being replayed was %s\n", g_current);
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
    if (fs::is_directory(fs::path(c) / "cssom-sheet", ec)) {
      return c;
    }
  }
  return "tests/fuzz/corpus";
}

class CssomSheetFuzzSeeds : public ::testing::Test {
 protected:
  void SetUp() override {
    std::signal(SIGABRT, name_the_input);
  }
  void TearDown() override {
    std::signal(SIGABRT, SIG_DFL);
  }
};

TEST_F(CssomSheetFuzzSeeds, EverySeedHoldsEveryProperty) {
  const std::string dir = corpus_dir() + "/cssom-sheet";
  const auto files = seeds(dir);
  // The floor: tools/css/make-fuzz-sheet-cssom-seeds.py writes about 270: sheets that use every form the compiler reads, and invalid versions of each, put to trees of several shapes with the options varied.
  constexpr size_t kMinSeeds = 150;
  ASSERT_GE(files.size(), kMinSeeds) << "the seed corpus is missing from " << dir << "; tools/css/make-fuzz-sheet-cssom-seeds.py writes it";
  const auto start = std::chrono::steady_clock::now();
  for (const std::string & path : files) {
    std::snprintf(g_current, sizeof g_current, "%s", path.c_str());
    const std::string data = slurp(path);
    cssom_sheet_entry(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  RecordProperty("replay_seconds", std::to_string(seconds));
  const char * slow_env = std::getenv("GTEXT_TEST_SLOWDOWN");   // a known slow host (an emulator, Valgrind, a sanitizer) multiplies the bound, as the other seed replays do
  const double slow = slow_env && std::atof(slow_env) >= 1.0 && std::atof(slow_env) <= 1000.0 ? std::atof(slow_env) : 1.0;
  EXPECT_LT(seconds, 120.0 * slow) << files.size() << " seeds took " << seconds << " s";
}


// A seed chooses its rules and sheets by an index modulo the size of a pool, so a pool that grows changes what every seed already written means (a reproducer that failed once would stop
// reproducing, and nothing would say so). The pools the harness had when the mode of mixins was added are pinned here; new texts go in the pools of that mode.
uint64_t pool_hash() {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const char * text) {
    for (const char * c = text; *c; c++) {
      h = (h ^ static_cast<unsigned char>(*c)) * 1099511628211ull;
    }
    h = (h ^ 0xffu) * 1099511628211ull;
  };
  using namespace cssom_sheetfuzz;
  for (const char * t : kInit) mix(t);
  for (const char * t : kRules) mix(t);
  for (const char * t : kSheets) mix(t);
  for (const char * t : kSelectors) mix(t);
  for (const char * t : kNames) mix(t);
  for (const char * t : kValues) mix(t);
  for (const char * t : kMedia) mix(t);
  return h;
}

TEST(CssomSheetFuzzPools, ThePoolsASeedIndexesAreUnchanged) {
  EXPECT_EQ(sizeof cssom_sheetfuzz::kRules / sizeof *cssom_sheetfuzz::kRules, 30u);
  EXPECT_EQ(sizeof cssom_sheetfuzz::kSheets / sizeof *cssom_sheetfuzz::kSheets, 7u);
  EXPECT_EQ(pool_hash(), 0xb01bf9329477ee1bull) << "a text of a pool a seed indexes was changed, added or removed; put new texts in the pools of the mode of mixins (kRulesExt, kSheetsExt, kInitExt)";
}

// Only the seeds of the mode of mixins begin with the bytes that select it: nothing else in the corpus (a reproducer included) may, or its meaning would change.
TEST(CssomSheetFuzzPools, OnlyTheSeedsOfTheModeOfMixinsBeginWithItsMagic) {
  size_t ext = 0;
  for (const auto & entry : fs::directory_iterator(corpus_dir() + "/cssom-sheet")) {
    const std::string data = slurp(entry.path().generic_string());
    const bool magic = data.size() >= sizeof cssom_sheetfuzz::kExtMagic && std::memcmp(data.data(), cssom_sheetfuzz::kExtMagic, sizeof cssom_sheetfuzz::kExtMagic) == 0;
    const bool named = entry.path().filename().string().rfind("ext-", 0) == 0;
    EXPECT_EQ(magic, named) << entry.path().generic_string() << (magic ? " begins with the magic of the mode of mixins and is not named for it" : " is named for the mode of mixins and does not begin with its magic");
    ext += magic && named;
  }
  EXPECT_GE(ext, 100u) << "tools/css/make-fuzz-sheet-cssom-seeds.py writes the seeds of the mode of mixins";
}

} // namespace
