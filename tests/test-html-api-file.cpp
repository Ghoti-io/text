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
 * gtext_html_parse_file(): the file form of the HTML parser, and the failures of reading one.
 *
 * The mapping of the shared file layer's statuses is the one gtext_xml_parse_file(), the CSV and the
 * JSON file functions have, and so are the cases: a missing path, one that may not be read, a
 * directory (which opens and then will not read), a failed allocation while reading, a file longer
 * than the limit (applied while reading), a pipe. The messages are asserted as well as the codes,
 * because four of those arms are the same status and differ only in what they say.
 *
 * What a successful read adds: the file's bytes are parsed as gtext_html_parse() parses them (a
 * byte order mark included), and an allocation that fails anywhere - reading or parsing - is
 * GTEXT_HTML_E_OOM with no document and no block left behind.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <ghoti.io/text/html.h>

#include "html_api_test_support.h"
#include "markup_test_support.h"

using namespace html_api_test;

namespace {

class TempFile {
 public:
  explicit TempFile(const std::string & name) : path_("build/test-html-api-file-" + name) {
    std::remove(path_.c_str());
  }
  ~TempFile() {
#ifndef _WIN32
    chmod(path_.c_str(), 0600);
#endif
    std::remove(path_.c_str());
  }
  const char * c_str() const {
    return path_.c_str();
  }
  void write(const std::string & contents) const {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out << contents;
  }

 private:
  std::string path_;
};

struct FileOutcome {
  GTEXT_HTML_Status status;
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result result;
  FileOutcome() {
    std::memset(&result, 0, sizeof(result));
  }
  ~FileOutcome() {
    gtext_markup_document_free(doc);
    gtext_html_result_free(&result);
  }
};

const char * const kDocument = "\xEF\xBB\xBF<!DOCTYPE html><p>a<b>b</p>c</x>";

TEST(HtmlApiFile, TheDocumentItselfParses) {
  // The control for the rest: a failure below is about the file and not about the document.
  TempFile f("good");
  f.write(kDocument);
  FileOutcome o;
  o.status = gtext_html_parse_file(f.c_str(), nullptr, &o.doc, &o.result);
  ASSERT_EQ(o.status, GTEXT_HTML_OK);
  ASSERT_NE(o.doc, nullptr);
  EXPECT_EQ(o.result.status, GTEXT_HTML_OK);
  EXPECT_EQ(o.result.encoding_source, GTEXT_MARKUP_ENCODING_BOM);
  EXPECT_EQ(o.result.input_bytes, std::strlen(kDocument));
  // The same tree and errors as parsing the bytes.
  Outcome direct = run_call({"parse", false, false, {}}, kDocument, nullptr, nullptr);
  ASSERT_TRUE(direct.ok());
  EXPECT_EQ(dump_doc(o.doc), direct.tree);
  EXPECT_EQ(gtext_html_result_error_count(&o.result), direct.errors.size());
  for (size_t i = 0; i < direct.errors.size(); i++) {
    GTEXT_HTML_Error e;
    ASSERT_TRUE(gtext_html_result_error_at(&o.result, i, &e));
    EXPECT_EQ(e.byte_offset, direct.errors[i].offset) << "offsets are into the file";
  }
}

TEST(HtmlApiFile, ANullPathOrOutputIsRefused) {
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result r;
  EXPECT_EQ(gtext_html_parse_file(nullptr, nullptr, &doc, &r), GTEXT_HTML_E_INVALID);
  EXPECT_STREQ(r.message, "Path must not be NULL");
  gtext_html_result_free(&r);
  EXPECT_EQ(gtext_html_parse_file("x", nullptr, nullptr, &r), GTEXT_HTML_E_INVALID);
  EXPECT_STREQ(r.message, "out must not be NULL");
  gtext_html_result_free(&r);
  EXPECT_EQ(gtext_html_parse_file(nullptr, nullptr, nullptr, nullptr), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(doc, nullptr);
}

TEST(HtmlApiFile, AMissingPathSaysThereIsNothingThere) {
  GTEXT_Markup_Document * doc = reinterpret_cast<GTEXT_Markup_Document *>(0x10);
  GTEXT_HTML_Result r;
  EXPECT_EQ(gtext_html_parse_file("build/test-html-api-file-absent-on-purpose", nullptr, &doc, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(doc, reinterpret_cast<GTEXT_Markup_Document *>(0x10)) << "written only on success";
  EXPECT_EQ(r.status, GTEXT_HTML_E_INVALID);
  EXPECT_STREQ(r.message, "No such file or directory");
  EXPECT_EQ(gtext_html_result_error_count(&r), 0u);
  EXPECT_EQ(r.encoding, nullptr) << "no encoding was decided";
  gtext_html_result_free(&r);
  // A NULL result is fine for a failure too.
  EXPECT_EQ(gtext_html_parse_file("build/test-html-api-file-absent-on-purpose", nullptr, &doc, nullptr),
      GTEXT_HTML_E_INVALID);
}

TEST(HtmlApiFile, AnUnreadablePathSaysPermissionDenied) {
#ifdef _WIN32
  GTEST_SKIP() << "no POSIX permission bits";
#else
  if (geteuid() == 0) {
    GTEST_SKIP() << "running as root; the mode bits do not apply";
  }
  TempFile f("noperm");
  f.write(kDocument);
  ASSERT_EQ(chmod(f.c_str(), 0), 0) << std::strerror(errno);
  FileOutcome o;
  o.status = gtext_html_parse_file(f.c_str(), nullptr, &o.doc, &o.result);
  EXPECT_EQ(o.status, GTEXT_HTML_E_INVALID);
  EXPECT_STREQ(o.result.message, "Permission denied opening file");
  EXPECT_EQ(o.doc, nullptr);
#endif
}

TEST(HtmlApiFile, ADirectoryOpensAndThenWillNotRead) {
#ifdef _WIN32
  GTEST_SKIP() << "fopen() refuses a directory outright here";
#else
  const std::string dir = "build/test-html-api-file-isadir";
  mkdir(dir.c_str(), 0700);
  FileOutcome o;
  o.status = gtext_html_parse_file(dir.c_str(), nullptr, &o.doc, &o.result);
  rmdir(dir.c_str());
  EXPECT_EQ(o.status, GTEXT_HTML_E_INVALID);
  EXPECT_STREQ(o.result.message, "Failed to read file");
  EXPECT_EQ(o.doc, nullptr);
#endif
}

TEST(HtmlApiFile, AFileLargerThanTheLimitIsRefusedWhileReading) {
  TempFile f("big");
  std::string big;
  while (big.size() < 4096) {
    big += "<p>x</p>\n";
  }
  f.write(big);
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.max_input_bytes = 64;
  FileOutcome r;
  r.status = gtext_html_parse_file(f.c_str(), &o, &r.doc, &r.result);
  EXPECT_EQ(r.status, GTEXT_HTML_E_LIMIT_INPUT);
  EXPECT_STREQ(r.result.message, "File exceeds max_input_bytes");
  EXPECT_EQ(r.doc, nullptr);
  // The boundary is exact: the file's own size passes, one less does not.
  o.max_input_bytes = big.size();
  FileOutcome at;
  at.status = gtext_html_parse_file(f.c_str(), &o, &at.doc, &at.result);
  EXPECT_EQ(at.status, GTEXT_HTML_OK);
  o.max_input_bytes = big.size() - 1;
  FileOutcome under;
  under.status = gtext_html_parse_file(f.c_str(), &o, &under.doc, &under.result);
  EXPECT_EQ(under.status, GTEXT_HTML_E_LIMIT_INPUT);
}

TEST(HtmlApiFile, AFailedAllocationWhileReadingIsOutOfMemory) {
  TempFile f("oom-read");
  f.write(kDocument);
  FaultAllocator fa;
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.allocator = &fa.alloc;
  fa.arm(1); // the buffer the file is read into is the first allocation
  FileOutcome r;
  r.status = gtext_html_parse_file(f.c_str(), &o, &r.doc, &r.result);
  EXPECT_EQ(r.status, GTEXT_HTML_E_OOM);
  EXPECT_STREQ(r.result.message, "Out of memory reading file");
  EXPECT_EQ(r.doc, nullptr);
  EXPECT_TRUE(fa.live.empty());
}

TEST(HtmlApiFile, EveryAllocationOfTheCallCanFail) {
  TempFile f("oom-sweep");
  f.write(std::string(kDocument) + "<table><tr><td>x<template><b>y</template><svg><path/></svg>");
  Outcome baseline;
  {
    FileOutcome b;
    b.status = gtext_html_parse_file(f.c_str(), nullptr, &b.doc, &b.result);
    ASSERT_EQ(b.status, GTEXT_HTML_OK);
    baseline.tree = dump_doc(b.doc);
  }
  bool reached_the_end = false;
  size_t failures = 0;
  for (size_t n = 1; n < 3000 && !reached_the_end; n++) {
    FaultAllocator fa;
    GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
    o.allocator = &fa.alloc;
    {
      FileOutcome r;
      fa.arm(n);
      r.status = gtext_html_parse_file(f.c_str(), &o, &r.doc, &r.result);
      fa.disarm();
      if (!fa.failed) {
        ASSERT_EQ(r.status, GTEXT_HTML_OK);
        EXPECT_EQ(dump_doc(r.doc), baseline.tree);
        reached_the_end = true;
      }
      else if (r.status != GTEXT_HTML_OK) {
        failures++;
        EXPECT_EQ(r.status, GTEXT_HTML_E_OOM) << n;
        EXPECT_EQ(r.doc, nullptr);
        EXPECT_EQ(r.result.status, GTEXT_HTML_E_OOM);
      }
      else {
        EXPECT_EQ(dump_doc(r.doc), baseline.tree) << n;
      }
    }
    EXPECT_TRUE(fa.live.empty()) << n << ": " << fa.live.size() << " leaked";
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_TRUE(reached_the_end);
  EXPECT_GT(failures, 20u);
}

TEST(HtmlApiFile, EveryLimitIsExactForAFileToo) {
  // The same table as the buffer forms: for each limit, the input is written to a file, and the smallest
  // limit under which it parses passes while one less is its own status. (The input limit is the file's size.)
  for (const LimitRow & row : kLimitRows) {
    TempFile f(std::string("limit-") + row.name);
    f.write(row.input);
    FileOutcome roomy;
    roomy.status = gtext_html_parse_file(f.c_str(), nullptr, &roomy.doc, &roomy.result);
    ASSERT_EQ(roomy.status, GTEXT_HTML_OK) << row.name;
    size_t threshold = row.threshold;
    if (std::string(row.name) == "nodes") {
      threshold = roomy.result.node_count;
    }
    if (std::string(row.name) == "open_elements") {
      threshold = 4; // html, body, div, div
    }
    ASSERT_GT(threshold, 1u) << row.name;
    const std::string tree = dump_doc(roomy.doc);
    for (size_t n : {threshold - 1, threshold, threshold + 1}) {
      GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
      row.set(&o, n);
      FileOutcome got;
      got.status = gtext_html_parse_file(f.c_str(), &o, &got.doc, &got.result);
      if (n >= threshold) {
        ASSERT_EQ(got.status, GTEXT_HTML_OK) << row.name << " " << n;
        EXPECT_EQ(dump_doc(got.doc), tree);
      }
      else {
        EXPECT_EQ(got.status, row.status) << row.name << " " << n;
        EXPECT_EQ(got.doc, nullptr);
        EXPECT_EQ(got.result.status, row.status);
      }
    }
  }
}

TEST(HtmlApiFile, AnEmptyFileIsAnEmptyDocument) {
  TempFile f("empty");
  f.write("");
  FileOutcome o;
  o.status = gtext_html_parse_file(f.c_str(), nullptr, &o.doc, &o.result);
  ASSERT_EQ(o.status, GTEXT_HTML_OK);
  EXPECT_EQ(dump_doc(o.doc), "| <html>\n|   <head>\n|   <body>\n");
  EXPECT_EQ(o.result.input_bytes, 0u);
}

TEST(HtmlApiFile, TheOptionsReachTheParse) {
  TempFile f("options");
  f.write("<p>a</p><p>b</p><p>c</p>");
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.max_nodes = 4;
  FileOutcome r;
  r.status = gtext_html_parse_file(f.c_str(), &o, &r.doc, &r.result);
  EXPECT_EQ(r.status, GTEXT_HTML_E_LIMIT_NODES);
  EXPECT_STREQ(r.result.message, gtext_html_status_string(GTEXT_HTML_E_LIMIT_NODES));
  o = gtext_html_parse_options_default();
  o.default_encoding = "windows-1252";
  FileOutcome u;
  u.status = gtext_html_parse_file(f.c_str(), &o, &u.doc, &u.result);
  EXPECT_EQ(u.status, GTEXT_HTML_E_UNSUPPORTED_ENCODING);
  EXPECT_EQ(u.doc, nullptr);
}

#ifndef _WIN32
TEST(HtmlApiFile, APipeCanBeRead) {
  const std::string path = "build/test-html-api-file-fifo";
  std::remove(path.c_str());
  if (mkfifo(path.c_str(), 0600) != 0) {
    GTEST_SKIP() << "no mkfifo";
  }
  std::thread writer([&]() {
    FILE * w = std::fopen(path.c_str(), "wb"); // blocks until a reader opens the pipe
    if (w) {
      std::fputs("<!DOCTYPE html><p>piped", w);
      std::fclose(w);
    }
  });
  FileOutcome o;
  o.status = gtext_html_parse_file(path.c_str(), nullptr, &o.doc, &o.result);
  // If the library failed before it opened the pipe, the writer is still waiting for a reader: be one, so
  // that the join below returns and the test fails on its assertion instead of hanging.
  const int released = open(path.c_str(), O_RDONLY | O_NONBLOCK);
  if (released >= 0) {
    close(released);
  }
  writer.join();
  std::remove(path.c_str());
  ASSERT_EQ(o.status, GTEXT_HTML_OK);
  EXPECT_NE(dump_doc(o.doc).find("\"piped\""), std::string::npos);
}
#endif

} // namespace
