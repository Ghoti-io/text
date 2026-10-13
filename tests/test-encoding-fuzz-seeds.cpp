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
 * Every tracked seed of the Encoding fuzzer, run through the fuzzer's own properties.
 *
 * tests/fuzz/fuzz_encoding.cpp is a libFuzzer harness and needs clang and a minute; the properties it checks - the answer is
 * the same however the input is cut, the output is the reference decoders', fatal mode fails exactly where replacement mode
 * replaces, the limits at their exact values, a sink that refuses, an allocation that fails, the encoder's model - do not.
 * This test includes the harness source as it is and calls its entry point on every file in tests/fuzz/corpus/encoding, so
 * `make test` (and test-asan, and test-tsan) replays the corpus with no fuzzer built.
 *
 * That is also where **a reproducer lives once the fuzzer has found something**: a file named `reproducer-*.seed` in the
 * corpus directory is replayed here on every run. A harness aborts when a property fails; the handler below names the file
 * first, so the failure says which input and not only which property.
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

#include "markup_test_support.h"

namespace encoding_harness {
#define LLVMFuzzerTestOneInput encoding_entry
#include "fuzz/fuzz_encoding.cpp"
#undef LLVMFuzzerTestOneInput
} // namespace encoding_harness

namespace {

char g_current[512];

void name_the_input(int) {
  std::fprintf(stderr, "\ntest-encoding-fuzz-seeds: the input being replayed was %s\n", g_current);
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

// Where the corpus is, whatever the working directory (see test-html-api-fuzz-seeds.cpp).
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
    if (fs::is_directory(fs::path(c) / "encoding", ec)) {
      return c;
    }
  }
  return "tests/fuzz/corpus";
}

class EncodingFuzzSeeds : public ::testing::Test {
 protected:
  void SetUp() override {
    std::signal(SIGABRT, name_the_input);
  }
  void TearDown() override {
    std::signal(SIGABRT, SIG_DFL);
  }
};

TEST_F(EncodingFuzzSeeds, EverySeedHoldsEveryProperty) {
  const std::string dir = corpus_dir() + "/encoding";
  const auto files = seeds(dir);
  // The corpus is what tools/encoding/make-fuzz-seeds.py wrote (its count is in tools/encoding/FUZZ_SEED_COUNT) and the reproducers, which it keeps.
  size_t generated = 0;
  for (const std::string & f : files) {
    if (fs::path(f).filename().string().rfind("reproducer-", 0) != 0) {
      generated++;
    }
  }
  const fs::path count_file = fs::path(dir).parent_path().parent_path().parent_path().parent_path() / "tools" / "encoding" / "FUZZ_SEED_COUNT";
  std::ifstream count_in(count_file);
  size_t expected = 0;
  ASSERT_TRUE(static_cast<bool>(count_in >> expected)) << count_file.string() << " is not there; tools/encoding/make-fuzz-seeds.py writes it";
  ASSERT_GT(expected, 0u);
  EXPECT_EQ(generated, expected) << "the generator wrote " << expected << " seeds and " << dir << " has " << generated << " that are not reproducers";
  ASSERT_GE(files.size(), expected) << "the seed corpus is missing from " << dir << "; tools/encoding/make-fuzz-seeds.py writes it";
  for (const char * name : {"utf-8-utf8-ill-formed-table.seed", "windows-1252-all-bytes.seed", "utf-16le-bom-utf16le.seed",
           "x-user-defined-utf8-euro-emoji-alloc.seed", "not-an-encoding-0.seed"}) {
    const std::string path = dir + "/" + name;
    ASSERT_TRUE(std::find(files.begin(), files.end(), path) != files.end()) << path << " is not there";
  }
  const auto start = std::chrono::steady_clock::now();
  for (const std::string & path : files) {
    std::snprintf(g_current, sizeof g_current, "%s", path.c_str());
    const std::string data = slurp(path);
    encoding_harness::encoding_entry(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }
  // Every seed was written to reach a codec (all but the values that are no encoding); a replay in which few did is
  // checking the refusals.
  EXPECT_GE(encoding_harness::g_decoded_inputs * 10, files.size() * 8)
      << encoding_harness::g_decoded_inputs << " of " << files.size() << " seeds reached a decoder";
  EXPECT_GE(encoding_harness::g_encoded_inputs * 10, files.size() * 6)
      << encoding_harness::g_encoded_inputs << " of " << files.size() << " seeds reached an encoder";
  EXPECT_EQ(encoding_harness::g_refused_values, 8u);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  RecordProperty("replay_seconds", std::to_string(seconds));
  // About five seconds for the 2,600 seeds of a plain build; a sanitizer is up to twenty times slower, and the bound is for a replay that has gone quadratic.
  // (GTEXT_TEST_FULL=1 in the environment, the switch of the other multi-byte tests for their sanitizer reductions, lifts the bound to ten times that.)
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
  const double kSecondsAllowed = std::getenv("GTEXT_TEST_FULL") ? 3000.0 : 300.0;
#else
  const double kSecondsAllowed = 60.0;
#endif
  EXPECT_LT(seconds, kSecondsAllowed) << files.size() << " seeds took " << seconds << " s";
}

} // namespace
