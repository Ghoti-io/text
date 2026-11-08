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
 * Every tracked input of the INI writer's fuzzer, run through the fuzzer's own property.
 *
 * tests/fuzz/fuzz_ini_writer.cpp is a libFuzzer harness and needs clang; the property it checks (tests/ini_writer_model.h: what the document model accepts, the writer refuses
 * or writes so that it reads back as it was given) does not. This test includes the harness as it is and calls its entry point on every file in tests/fuzz/corpus/ini-writer,
 * so `make test` (and test-asan, and test-tsan) replays the corpus with no fuzzer built. A reproducer the fuzzer finds is kept there as `reproducer-*.seed` and is replayed here
 * on every run: until this test existed the INI corpus was replayed only by whoever ran the fuzzer.
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

#define LLVMFuzzerTestOneInput ini_writer_entry
#include "fuzz/fuzz_ini_writer.cpp"
#undef LLVMFuzzerTestOneInput

namespace {

char g_current[512];

void name_the_input(int) {
  std::fprintf(stderr, "\ntest-ini-fuzz-seeds: the input being replayed was %s\n", g_current);
  std::fflush(stderr);
  std::signal(SIGABRT, SIG_DFL);
  std::raise(SIGABRT);
}

namespace fs = std::filesystem;

std::vector<std::string> seeds(const std::string & directory) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto & entry : fs::directory_iterator(directory, ec)) {
    if (entry.is_regular_file(ec)) {
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
    if (fs::is_directory(fs::path(c) / "ini-writer", ec)) {
      return c;
    }
  }
  return "tests/fuzz/corpus";
}

class IniWriterFuzzSeeds : public ::testing::Test {
 protected:
  void SetUp() override {
    std::signal(SIGABRT, name_the_input);
  }
  void TearDown() override {
    std::signal(SIGABRT, SIG_DFL);
  }
};

TEST_F(IniWriterFuzzSeeds, EverySeedHoldsEveryProperty) {
  const std::string dir = corpus_dir() + "/ini-writer";
  const auto files = seeds(dir);
  // The floor: the reproducers of the findings are tracked as `reproducer-*.seed` and are what a cross build copies (the corpus libFuzzer grows is left behind); a directory that has
  // gone missing, or lost them, must not read as a replay that passed.
  constexpr size_t kMinSeeds = 20;
  size_t reproducers = 0;
  for (const std::string & f : files) {
    if (f.size() > 5 && f.compare(f.size() - 5, 5, ".seed") == 0) reproducers++;
  }
  ASSERT_GE(reproducers, kMinSeeds) << "the reproducers are missing from " << dir;
  const auto start = std::chrono::steady_clock::now();
  for (const std::string & path : files) {
    std::snprintf(g_current, sizeof g_current, "%s", path.c_str());
    const std::string data = slurp(path);
    ini_writer_entry(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  RecordProperty("replay_seconds", std::to_string(seconds));
  EXPECT_LT(seconds, 120.0) << files.size() << " seeds took " << seconds << " s";
}

} // namespace
