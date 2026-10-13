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
 * The Encoding Standard's label table (section 4.2) and the facts about each encoding: every one of the 228 labels
 * in the pinned encodings.json resolves to its encoding however it is cased or padded, nothing else does, and the
 * names, the ASCII-compatibility, the output encodings and the support flags are what the standard and this release
 * say.
 *
 * The expectations are read from third_party/encoding/encodings.json with this library's JSON parser, so they are
 * not the generated table checked against itself.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "encoding_test_support.h"

#include <set>

using namespace enctest;

namespace {

GTEXT_Encoding lookup(const std::string & s) {
  return gtext_encoding_for_label(s.data(), s.size());
}

std::string upper(std::string s) {
  for (auto & c : s) {
    c = char(std::toupper((unsigned char)c));
  }
  return s;
}

std::string alternate(std::string s) {
  for (size_t i = 0; i < s.size(); i++) {
    s[i] = char((i % 2) ? std::toupper((unsigned char)s[i]) : std::tolower((unsigned char)s[i]));
  }
  return s;
}

} // namespace

TEST(EncodingLabels, TheTableIsTheStandardsAndTheEnumFollowsIt) {
  const auto rows = load_encodings();
  ASSERT_EQ(rows.size(), 40u);
  ASSERT_EQ(size_t(GTEXT_ENCODING_COUNT), rows.size() + 1);
  size_t labels = 0;
  for (size_t i = 0; i < rows.size(); i++) {
    EXPECT_STREQ(gtext_encoding_name(GTEXT_Encoding(i + 1)), rows[i].name.c_str());
    EXPECT_EQ(encoding_by_name(rows[i].name), GTEXT_Encoding(i + 1)) << rows[i].name;
    labels += rows[i].labels.size();
  }
  EXPECT_EQ(labels, 228u);
  EXPECT_EQ(gtext_encoding_label_count(), labels);
}

TEST(EncodingLabels, EveryLabelResolvesWhateverItsCaseOrPadding) {
  const auto rows = load_encodings();
  ASSERT_FALSE(rows.empty());
  const char ws[] = {' ', '\t', '\n', '\f', '\r'};
  size_t checked = 0;
  for (size_t i = 0; i < rows.size(); i++) {
    const GTEXT_Encoding want = GTEXT_Encoding(i + 1);
    for (const std::string & label : rows[i].labels) {
      EXPECT_EQ(lookup(label), want) << label;
      EXPECT_EQ(lookup(upper(label)), want) << label;
      EXPECT_EQ(lookup(alternate(label)), want) << label;
      for (char w : ws) {
        EXPECT_EQ(lookup(std::string(1, w) + label), want) << label;
        EXPECT_EQ(lookup(label + w), want) << label;
        EXPECT_EQ(lookup(std::string(1, w) + label + w), want) << label;
        EXPECT_EQ(lookup(std::string(3, w) + upper(label) + std::string(2, w)), want) << label;
      }
      EXPECT_EQ(lookup(" \t\r\n\f" + label + "\f\n\r\t "), want) << label;
      checked++;
    }
  }
  EXPECT_EQ(checked, 228u);
}

TEST(EncodingLabels, TheStandardsExamples) {
  EXPECT_EQ(lookup(" UTF8 "), GTEXT_ENCODING_UTF_8);
  EXPECT_EQ(lookup("latin1"), GTEXT_ENCODING_WINDOWS_1252);
  EXPECT_EQ(lookup("ISO-8859-1"), GTEXT_ENCODING_WINDOWS_1252);
  EXPECT_EQ(lookup("ascii"), GTEXT_ENCODING_WINDOWS_1252);
  EXPECT_EQ(lookup("us-ascii"), GTEXT_ENCODING_WINDOWS_1252);
  EXPECT_EQ(lookup("x-sjis"), GTEXT_ENCODING_SHIFT_JIS);
  EXPECT_EQ(lookup("utf-16"), GTEXT_ENCODING_UTF_16LE);
  EXPECT_EQ(lookup("unicodefffe"), GTEXT_ENCODING_UTF_16BE);
  EXPECT_EQ(lookup("iso-8859-8-i"), GTEXT_ENCODING_ISO_8859_8_I);
  EXPECT_EQ(lookup("logical"), GTEXT_ENCODING_ISO_8859_8_I);
  EXPECT_EQ(lookup("visual"), GTEXT_ENCODING_ISO_8859_8);
  EXPECT_EQ(lookup("iso-8859-9"), GTEXT_ENCODING_WINDOWS_1254);
  EXPECT_EQ(lookup("tis-620"), GTEXT_ENCODING_WINDOWS_874);
  EXPECT_EQ(lookup("x-user-defined"), GTEXT_ENCODING_X_USER_DEFINED);
  EXPECT_EQ(lookup("gb2312"), GTEXT_ENCODING_GBK);
  EXPECT_EQ(lookup("big5-hkscs"), GTEXT_ENCODING_BIG5);
  EXPECT_EQ(lookup("ks_c_5601-1987"), GTEXT_ENCODING_EUC_KR);
  EXPECT_EQ(lookup("csiso2022jp"), GTEXT_ENCODING_ISO_2022_JP);
  for (const char * l : {"replacement", "hz-gb-2312", "iso-2022-cn", "iso-2022-cn-ext", "iso-2022-kr", "csiso2022kr"}) {
    EXPECT_EQ(lookup(l), GTEXT_ENCODING_REPLACEMENT) << l;
  }
}

TEST(EncodingLabels, NothingElseIsALabel) {
  const auto rows = load_encodings();
  std::set<std::string> labels;
  for (const auto & r : rows) {
    labels.insert(r.labels.begin(), r.labels.end());
  }
  ASSERT_EQ(labels.size(), 228u);
  size_t tried = 0;
  for (const std::string & label : labels) {
    const std::string variants[] = {
        label + "x",
        label + "-",
        label + " x",
        "x" + label,
        label.substr(0, label.size() - 1),
        label.substr(1),
        label + std::string(1, '\0'),
        std::string(1, '\0') + label,
        label + "\v",
        "\v" + label,
        label + "\xC2\xA0",     // NBSP is not ASCII white space
        "\xC2\xA0" + label,
        label + "\xE2\x80\x83", // EM SPACE
        label + "\x1F",
        label + "\x7F",
        label + "\xFF",
        "\xEF\xBB\xBF" + label, // a byte order mark is not white space
        label.substr(0, label.size() / 2) + " " + label.substr(label.size() / 2),
    };
    for (const std::string & v : variants) {
      if (labels.count(v) != 0) {
        continue; // a prefix of one label can be another label
      }
      EXPECT_EQ(lookup(v), GTEXT_ENCODING_NONE) << show(v);
      tried++;
    }
  }
  EXPECT_GT(tried, 3000u);
  EXPECT_EQ(lookup(""), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup(" \t\n\f\r "), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup(std::string(1000, 'a')), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup(std::string(63, ' ') + "utf-8" + std::string(5000, ' ')), GTEXT_ENCODING_UTF_8);
  EXPECT_EQ(gtext_encoding_for_label(nullptr, 5), GTEXT_ENCODING_NONE);
  EXPECT_EQ(gtext_encoding_for_label(nullptr, 0), GTEXT_ENCODING_NONE);
  EXPECT_EQ(gtext_encoding_for_label("utf-8", 0), GTEXT_ENCODING_NONE);
  EXPECT_EQ(gtext_encoding_for_label("utf-8!", 5), GTEXT_ENCODING_UTF_8); // the length decides, not a NUL
  // The Kelvin sign and the dotless forms do not fold to ASCII letters.
  EXPECT_EQ(lookup("\xE2\x84\xAAoi8-r"), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup("\xC4\xB1so-8859-1"), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup("\xC5\xBF" "hift_jis"), GTEXT_ENCODING_NONE);
  // A name is a label once lowercased.
  EXPECT_EQ(lookup("UTF-16LE"), GTEXT_ENCODING_UTF_16LE);
  EXPECT_EQ(lookup("Shift_JIS"), GTEXT_ENCODING_SHIFT_JIS);
  EXPECT_EQ(lookup("Shift-JIS"), GTEXT_ENCODING_SHIFT_JIS); // "shift-jis" is a label of its own
  EXPECT_EQ(lookup("Shift JIS"), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup("utf8-"), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup("utf 8"), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup("utf_8"), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup("utf-32"), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup("utf-7"), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup("cp437"), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup("iso-8859-12"), GTEXT_ENCODING_NONE);
  EXPECT_EQ(lookup("iso-8859-1:1987"), GTEXT_ENCODING_NONE);
}

TEST(EncodingLabels, TheListingIsSortedCompleteAndConsistent) {
  const auto rows = load_encodings();
  std::map<std::string, GTEXT_Encoding> want;
  for (size_t i = 0; i < rows.size(); i++) {
    for (const auto & l : rows[i].labels) {
      want[l] = GTEXT_Encoding(i + 1);
    }
  }
  std::string previous;
  const size_t n = gtext_encoding_label_count();
  ASSERT_EQ(n, want.size());
  for (size_t i = 0; i < n; i++) {
    const char * label = nullptr;
    size_t len = 0;
    GTEXT_Encoding e = GTEXT_ENCODING_NONE;
    ASSERT_TRUE(gtext_encoding_label_at(i, &label, &len, &e));
    const std::string s(label, len);
    EXPECT_LT(previous, s) << "not ascending at " << i;
    previous = s;
    ASSERT_TRUE(want.count(s)) << s;
    EXPECT_EQ(e, want[s]) << s;
    EXPECT_EQ(lookup(s), e) << s;
  }
  EXPECT_FALSE(gtext_encoding_label_at(n, nullptr, nullptr, nullptr));
  EXPECT_FALSE(gtext_encoding_label_at(SIZE_MAX, nullptr, nullptr, nullptr));
  const char * untouched = "x";
  EXPECT_FALSE(gtext_encoding_label_at(n, &untouched, nullptr, nullptr));
  EXPECT_STREQ(untouched, "x");
  EXPECT_TRUE(gtext_encoding_label_at(0, nullptr, nullptr, nullptr)); // every output is optional
}

TEST(EncodingLabels, TheLowercasedNameIsAlwaysALabel) {
  // "For each encoding, ASCII-lowercasing its name yields one of its labels."
  for (int e = 1; e < GTEXT_ENCODING_COUNT; e++) {
    std::string n = gtext_encoding_name(GTEXT_Encoding(e));
    EXPECT_EQ(lookup(n), GTEXT_Encoding(e)) << n;
    for (auto & c : n) {
      c = char(std::tolower((unsigned char)c));
    }
    EXPECT_EQ(lookup(n), GTEXT_Encoding(e)) << n;
  }
}

TEST(EncodingFacts, NamesAreTheStandardsAndOnlyForEncodings) {
  EXPECT_STREQ(gtext_encoding_name(GTEXT_ENCODING_UTF_8), "UTF-8");
  EXPECT_STREQ(gtext_encoding_name(GTEXT_ENCODING_SHIFT_JIS), "Shift_JIS");
  EXPECT_STREQ(gtext_encoding_name(GTEXT_ENCODING_GB18030), "gb18030");
  EXPECT_STREQ(gtext_encoding_name(GTEXT_ENCODING_X_USER_DEFINED), "x-user-defined");
  EXPECT_STREQ(gtext_encoding_name(GTEXT_ENCODING_ISO_8859_8_I), "ISO-8859-8-I");
  EXPECT_STREQ(gtext_encoding_name(GTEXT_ENCODING_MACINTOSH), "macintosh");
  EXPECT_EQ(gtext_encoding_name(GTEXT_ENCODING_NONE), nullptr);
  EXPECT_EQ(gtext_encoding_name(GTEXT_ENCODING_COUNT), nullptr);
  EXPECT_EQ(gtext_encoding_name(GTEXT_Encoding(62)), nullptr);
  EXPECT_EQ(gtext_encoding_name(GTEXT_Encoding(63)), nullptr);
  std::set<std::string> seen;
  for (int e = 1; e < GTEXT_ENCODING_COUNT; e++) {
    ASSERT_NE(gtext_encoding_name(GTEXT_Encoding(e)), nullptr);
    EXPECT_TRUE(seen.insert(gtext_encoding_name(GTEXT_Encoding(e))).second);
  }
}

TEST(EncodingFacts, AsciiCompatibility) {
  std::set<GTEXT_Encoding> not_ascii = {GTEXT_ENCODING_UTF_16BE, GTEXT_ENCODING_UTF_16LE, GTEXT_ENCODING_ISO_2022_JP,
      GTEXT_ENCODING_REPLACEMENT};
  for (int e = 1; e < GTEXT_ENCODING_COUNT; e++) {
    EXPECT_EQ(gtext_encoding_is_ascii_compatible(GTEXT_Encoding(e)), not_ascii.count(GTEXT_Encoding(e)) == 0)
        << gtext_encoding_name(GTEXT_Encoding(e));
  }
  EXPECT_FALSE(gtext_encoding_is_ascii_compatible(GTEXT_ENCODING_NONE));
  EXPECT_FALSE(gtext_encoding_is_ascii_compatible(GTEXT_ENCODING_COUNT));
  EXPECT_FALSE(gtext_encoding_is_ascii_compatible(GTEXT_Encoding(61)));
}

TEST(EncodingFacts, OutputEncodings) {
  for (int e = 1; e < GTEXT_ENCODING_COUNT; e++) {
    const GTEXT_Encoding enc = GTEXT_Encoding(e);
    const bool utf8 = enc == GTEXT_ENCODING_REPLACEMENT || enc == GTEXT_ENCODING_UTF_16BE
        || enc == GTEXT_ENCODING_UTF_16LE;
    EXPECT_EQ(gtext_encoding_output_encoding(enc), utf8 ? GTEXT_ENCODING_UTF_8 : enc) << gtext_encoding_name(enc);
  }
  EXPECT_EQ(gtext_encoding_output_encoding(GTEXT_ENCODING_NONE), GTEXT_ENCODING_NONE);
  EXPECT_EQ(gtext_encoding_output_encoding(GTEXT_ENCODING_COUNT), GTEXT_ENCODING_NONE);
}

TEST(EncodingFacts, SupportIsEverythingButTheMultiByteEncodings) {
  const std::set<GTEXT_Encoding> multi = {GTEXT_ENCODING_GBK, GTEXT_ENCODING_GB18030, GTEXT_ENCODING_BIG5,
      GTEXT_ENCODING_EUC_JP, GTEXT_ENCODING_ISO_2022_JP, GTEXT_ENCODING_SHIFT_JIS, GTEXT_ENCODING_EUC_KR};
  size_t supported = 0;
  for (int e = 1; e < GTEXT_ENCODING_COUNT; e++) {
    const bool want = multi.count(GTEXT_Encoding(e)) == 0;
    EXPECT_EQ(gtext_encoding_is_supported(GTEXT_Encoding(e)), want) << gtext_encoding_name(GTEXT_Encoding(e));
    supported += want;
  }
  EXPECT_EQ(supported, 33u);
  EXPECT_FALSE(gtext_encoding_is_supported(GTEXT_ENCODING_NONE));
  EXPECT_FALSE(gtext_encoding_is_supported(GTEXT_Encoding(60)));
}

TEST(EncodingFacts, StatusStringsAreDistinct) {
  std::set<std::string> seen;
  for (int s = GTEXT_ENCODING_OK; s <= GTEXT_ENCODING_E_INTERNAL; s++) {
    const char * m = gtext_encoding_status_string(GTEXT_Encoding_Status(s));
    ASSERT_NE(m, nullptr);
    EXPECT_STRNE(m, "unknown status");
    EXPECT_TRUE(seen.insert(m).second) << m;
  }
  EXPECT_STREQ(gtext_encoding_status_string(GTEXT_Encoding_Status(15)), "unknown status");
}
