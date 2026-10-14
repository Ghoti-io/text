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
 * Every tracked seed of the HTML serializer's fuzzer, run through the fuzzer's own properties.
 *
 * tests/fuzz/fuzz_html_serialize.cpp is a libFuzzer harness and needs clang and a minute; the properties it checks - the
 * output is the model's, the token stream writes what the walk writes, the counts, the limits at their exact values, a sink that
 * refuses, an allocation that fails - do not. This test includes the harness source as it is and calls its entry point on every
 * file in tests/fuzz/corpus/html-serialize, so `make test` (and test-asan, and test-tsan) replays the corpus with no fuzzer
 * built.
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

namespace serialize_harness {
#define LLVMFuzzerTestOneInput serialize_entry
#include "fuzz/fuzz_html_serialize.cpp"
#undef LLVMFuzzerTestOneInput
} // namespace serialize_harness

namespace {

char g_current[512];

void name_the_input(int) {
  std::fprintf(stderr, "\ntest-html-serialize-fuzz-seeds: the input being replayed was %s\n", g_current);
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
      out.push_back(entry.path().generic_string()); // (generic: "/" on every host, so the names the test builds with "/" compare equal)
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
  candidates.push_back((fs::path(__FILE__).parent_path() / "fuzz" / "corpus").generic_string());
  candidates.push_back("tests/fuzz/corpus");
  candidates.push_back("../tests/fuzz/corpus");
  candidates.push_back("../../tests/fuzz/corpus");
  for (const std::string & c : candidates) {
    std::error_code ec;
    if (fs::is_directory(fs::path(c) / "html-serialize", ec)) {
      return c;
    }
  }
  return "tests/fuzz/corpus";
}

class HtmlSerializeFuzzSeeds : public ::testing::Test {
 protected:
  void SetUp() override {
    std::signal(SIGABRT, name_the_input);
  }
  void TearDown() override {
    std::signal(SIGABRT, SIG_DFL);
  }
};

TEST_F(HtmlSerializeFuzzSeeds, EverySeedHoldsEveryProperty) {
  const std::string dir = corpus_dir() + "/html-serialize";
  const auto files = seeds(dir);
  // The floor. tools/html/make-serialize-fuzz-seeds.py reads this very line for the lower end of the size it accepts.
  constexpr size_t kMinSeeds = 850;
  ASSERT_GE(files.size(), kMinSeeds) << "the seed corpus is missing from " << dir
                                 << "; tools/html/make-serialize-fuzz-seeds.py writes it";
  for (const char * name : {"ser-nbsp.seed", "ser-ampersands-sink-fails-midway.seed", "ser-alloc-sweep-all-forms.seed"}) {
    const std::string path = dir + "/" + name;
    ASSERT_TRUE(std::find(files.begin(), files.end(), path) != files.end()) << path << " is not there";
  }
  const auto start = std::chrono::steady_clock::now();
  for (const std::string & path : files) {
    std::snprintf(g_current, sizeof g_current, "%s", path.c_str());
    const std::string data = slurp(path);
    serialize_harness::serialize_entry(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }
  // Every seed was written to reach a serialization; a replay in which few did is checking the parser's refusals.
  EXPECT_GE(serialize_harness::g_serialized_inputs * 10, files.size() * 9)
      << serialize_harness::g_serialized_inputs << " of " << files.size() << " seeds reached the serializer";
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  RecordProperty("replay_seconds", std::to_string(seconds));
  EXPECT_LT(seconds, 60.0) << files.size() << " seeds took " << seconds << " s";
}

} // namespace
