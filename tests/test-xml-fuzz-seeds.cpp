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
 * Every tracked seed of the XML fuzzers, run through the fuzzers' own properties.
 *
 * tests/fuzz/fuzz_xml.cpp and fuzz_xml_writer.cpp are libFuzzer harnesses and
 * need clang and a minute; the properties they check - the event parser and the
 * DOM parse are one parser, an accepted tree survives five encodings, a writer
 * that says OK wrote the tree it was given - do not. This test includes the
 * harness sources as they are and calls their entry points on every file in
 * tests/fuzz/corpus/xml and tests/fuzz/corpus/xml-writer, so `make test` (and
 * test-asan, and test-tsan) replays the corpus with no fuzzer built.
 *
 * That is also where **a reproducer lives once the fuzzer has found something**:
 * a file named `reproducer-*.seed` in the corpus directory is replayed here on
 * every run, which is the cheap half of "the fix is in these bytes".
 *
 * A harness aborts when a property fails; the handler below names the file first,
 * so the failure says which input and not only which property.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <dirent.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>

// The harnesses' own includes, first, so that including them inside a namespace
// below only re-opens guarded headers.
#include "../src/markup/markup_internal.h"
#include "../src/xml/xml_internal.h"
#include "markup_test_support.h"
#include "xml_replay.h"

namespace reader_harness {
#define LLVMFuzzerTestOneInput reader_entry
#include "fuzz/fuzz_xml.cpp"
#undef LLVMFuzzerTestOneInput
} // namespace reader_harness

namespace writer_harness {
#define LLVMFuzzerTestOneInput writer_entry
#include "fuzz/fuzz_xml_writer.cpp"
#undef LLVMFuzzerTestOneInput
} // namespace writer_harness

namespace {

char g_current[512];

void name_the_input(int) {
  static const char head[] = "\ntest-xml-fuzz-seeds: the input being replayed was ";
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
    if (DIR * d = opendir((c + "/xml").c_str())) {
      closedir(d);
      return c;
    }
  }
  return "tests/fuzz/corpus";
}

class XmlFuzzSeeds : public ::testing::Test {
 protected:
  void SetUp() override {
    std::signal(SIGABRT, name_the_input);
  }
  void TearDown() override {
    std::signal(SIGABRT, SIG_DFL);
  }
};

TEST_F(XmlFuzzSeeds, EveryReaderSeedHoldsEveryProperty) {
  const std::string dir = corpus_dir() + "/xml";
  const auto files = seeds(dir);
  ASSERT_GT(files.size(), 700u) << "the seed corpus is missing from " << dir
                                << "; tools/xml/make-fuzz-seeds.py writes it";
  // The recorded finds are not generated, so nothing else notices one going missing.
  const std::string repro = dir + "/reproducer-attribute-limit-before-a-bad-byte.seed";
  ASSERT_TRUE(std::find(files.begin(), files.end(), repro) != files.end()) << repro << " is not there";
  for (const std::string & path : files) {
    std::snprintf(g_current, sizeof g_current, "%s", path.c_str());
    const std::string data = slurp(path);
    reader_harness::reader_entry(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }
}

TEST_F(XmlFuzzSeeds, EveryWriterSeedHoldsEveryProperty) {
  const std::string dir = corpus_dir() + "/xml-writer";
  const auto files = seeds(dir);
  ASSERT_GE(files.size(), 48u) << "the seed corpus is missing from " << dir
                               << "; tools/xml/make-fuzz-seeds.py writes it";
  for (const std::string & path : files) {
    std::snprintf(g_current, sizeof g_current, "%s", path.c_str());
    const std::string data = slurp(path);
    writer_harness::writer_entry(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }
}

} // namespace
