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
 * Memory for the INI module: the allocator that refuses the n-th allocation, for every n a call makes, under every dialect, through the entry points that allocate - the parse,
 * building a document (the constructor, add_group(), set(), add()), the write, and the value codec (escape and unescape, whose escape makes several candidates and decodes each,
 * so that its own allocations are many and a failure of one of them must end the call as an out-of-memory and not as a refusal of a text it could have spelled).
 *
 * The property of each call is the same: it answers what it answers without the failure, or it answers GTEXT_INI_E_OOM with nothing returned; nothing leaks; nothing is freed
 * through the wrong allocator; and a document that a failed step was made on is still a document (it is written, and read, and freed).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "markup_test_support.h"

#include <ghoti.io/text/ini.h>

namespace {

using ini_oom_fault = FaultAllocator;

GTEXT_INI_Dialect (*const kDialects[])(void) = {
    gtext_ini_dialect_generic, gtext_ini_dialect_git_config, gtext_ini_dialect_desktop_entry, gtext_ini_dialect_systemd,
    gtext_ini_dialect_editorconfig, gtext_ini_dialect_win32, gtext_ini_dialect_configparser,
};

/** Run @p body with the n-th allocation failing, for n = 1, 2, ... until a run fails none. The body frees what it made. Returns the number of allocations the failure-free run made. */
size_t sweep(ini_oom_fault & fa, const std::function<void()> & body) {
  for (size_t n = 1; n < 2000; n++) {
    fa.arm(n);
    body();
    const bool failed = fa.failed;
    fa.disarm();
    EXPECT_TRUE(fa.live.empty()) << "leak after failing allocation " << n << ": " << fa.live.size();
    EXPECT_EQ(fa.bad_frees, 0u) << "a free of what this allocator did not make, after failing allocation " << n;
    if (!failed) {
      return n - 1;
    }
  }
  ADD_FAILURE() << "a call that never stops allocating";
  return 0;
}

}   // namespace

TEST(IniOom, EveryAllocationOfTheValueCodecMayFail) {
  ini_oom_fault fa;
  const std::vector<std::string> texts = {"", "plain", " lead", "a \"b\" ; c", "line one\nline two", "tab\there \\ back", std::string("nul\0in", 6), "\xff", "trailing "};
  for (auto make : kDialects) {
    const GTEXT_INI_Dialect d = make();
    for (const std::string & text : texts) {
      // the answers with the default allocator are the ones every successful run under failure must give
      char * ref = nullptr;
      size_t ref_len = 0;
      const GTEXT_INI_Status want = gtext_ini_escape(&d, text.data(), text.size(), nullptr, &ref, &ref_len);
      ASSERT_TRUE(want == GTEXT_INI_OK || want == GTEXT_INI_E_UNREPRESENTABLE);
      const std::string reference = ref ? std::string(ref, ref_len) : std::string();
      gtext_ini_string_free(nullptr, ref);
      const size_t calls = sweep(fa, [&] {
        char * out = nullptr;
        size_t n = 0;
        const GTEXT_INI_Status st = gtext_ini_escape(&d, text.data(), text.size(), &fa.alloc, &out, &n);
        if (st == GTEXT_INI_OK) {
          EXPECT_EQ(want, GTEXT_INI_OK);
          EXPECT_EQ(std::string(out, n), reference);
          gtext_ini_string_free(&fa.alloc, out);
        }
        else if (st == GTEXT_INI_E_OOM) {
          EXPECT_EQ(out, nullptr);
        }
        else {
          // a refusal is a refusal of the text, and it is the answer the failure-free call gives: running out of memory is not a reason to refuse
          EXPECT_EQ(st, want) << "the status of a call that ran out of memory partway is neither the success nor the out-of-memory";
          EXPECT_EQ(want, GTEXT_INI_E_UNREPRESENTABLE);
          EXPECT_EQ(out, nullptr);
        }
      });
      EXPECT_GE(calls, 1u);
      // and unescape, of the encoding when there is one and of the text as it is
      for (const std::string & raw : {reference, text}) {
        char * dref = nullptr;
        size_t dref_len = 0;
        const GTEXT_INI_Status dwant = gtext_ini_unescape(&d, raw.data(), raw.size(), nullptr, &dref, &dref_len);
        const std::string dreference = dref ? std::string(dref, dref_len) : std::string();
        gtext_ini_string_free(nullptr, dref);
        sweep(fa, [&] {
          char * out = nullptr;
          size_t n = 0;
          const GTEXT_INI_Status st = gtext_ini_unescape(&d, raw.data(), raw.size(), &fa.alloc, &out, &n);
          if (st == GTEXT_INI_OK) {
            EXPECT_EQ(dwant, GTEXT_INI_OK);
            EXPECT_EQ(std::string(out, n), dreference);
            gtext_ini_string_free(&fa.alloc, out);
          }
          else if (st != GTEXT_INI_E_OOM) {
            EXPECT_EQ(st, dwant);
          }
        });
      }
    }
  }
}

TEST(IniOom, EveryAllocationOfAParseMayFail) {
  ini_oom_fault fa;
  const std::string text = "[g]\nk=v\n# a comment\nl=w\n[h]\nk=1\n";
  for (auto make : kDialects) {
    GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
    opts.dialect = make();
    opts.allocator = &fa.alloc;
    size_t failures = 0;
    const size_t calls = sweep(fa, [&] {
      GTEXT_INI_Error err;
      std::memset(&err, 0, sizeof(err));
      GTEXT_INI_Document * doc = gtext_ini_parse(text.data(), text.size(), &opts, &err);
      if (doc) {
        EXPECT_EQ(gtext_ini_document_group_count(doc), 2u);
        gtext_ini_free(doc);
      }
      else {
        EXPECT_EQ(err.code, GTEXT_INI_E_OOM);
        failures++;
      }
      gtext_ini_error_free(&err);
    });
    EXPECT_GE(calls, 3u);
    EXPECT_EQ(failures, calls);
  }
}

TEST(IniOom, EveryAllocationOfBuildingAndWritingADocumentMayFail) {
  ini_oom_fault fa;
  for (auto make : kDialects) {
    GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
    opts.dialect = make();
    opts.allocator = &fa.alloc;
    GTEXT_INI_Write_Options wopts = gtext_ini_write_options_default();
    wopts.allocator = &fa.alloc;
    const size_t calls = sweep(fa, [&] {
      GTEXT_INI_Document * doc = gtext_ini_new(&opts);
      if (!doc) return;
      // every step may fail and none of them may leave the document unusable: a step that failed is simply not there
      GTEXT_INI_Group * g = nullptr;
      if (gtext_ini_document_add_group(doc, "G", &g) == GTEXT_INI_OK) {
        for (int i = 0; i < 12; i++) {
          const std::string key = "key" + std::to_string(i);
          const GTEXT_INI_Status st = i % 2 ? gtext_ini_group_add(g, key.c_str(), "value", 5) : gtext_ini_group_set(g, key.c_str(), "value", 5);
          EXPECT_TRUE(st == GTEXT_INI_OK || st == GTEXT_INI_E_OOM || st == GTEXT_INI_E_BAD_KEY) << st;
        }
        EXPECT_TRUE(gtext_ini_group_set(g, "key0", "again", 5) == GTEXT_INI_OK || fa.failed);
      }
      GTEXT_INI_Group * h = nullptr;
      gtext_ini_document_add_group(doc, "H", &h);
      GTEXT_INI_Sink sink;
      if (gtext_ini_sink_buffer(&sink) == GTEXT_INI_OK) {
        const GTEXT_INI_Status st = gtext_ini_write(doc, &sink, &wopts);
        EXPECT_TRUE(st == GTEXT_INI_OK || st == GTEXT_INI_E_OOM || st == GTEXT_INI_E_UNREPRESENTABLE) << st;
        gtext_ini_sink_buffer_free(&sink);
      }
      gtext_ini_free(doc);
    });
    EXPECT_GE(calls, 5u);
  }
}
