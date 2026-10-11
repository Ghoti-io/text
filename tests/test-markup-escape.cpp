/**
 * @file
 *
 * Escaping text for XML, the lexical checks a writer needs, and XML's name
 * productions.
 *
 * What is asserted comes from XML 1.0 (Fifth Edition) - [2] Char, [4] and [4a]
 * name characters, [15] Comment, [16] PI, [18] CDSect, and the rules in section
 * 2.11 and 3.3.3 that make a raw carriage return in text and a raw tab or line
 * feed in an attribute value unsafe to write - and from Namespaces in XML 1.0
 * [4] NCName.
 *
 * The name productions are checked over every code point, against a table that
 * is the specification's own text parsed at run time ("[#xC0-#xD6]" and so on)
 * and not a second copy of the code's comparisons. A typo in either shows up as
 * a code point the two disagree on. That is still one reading of the
 * specification by one author, so the same predicate was also run against
 * libxml2 and expat over the whole range when it was written; what that found
 * is in notes/text/XML-HTML-PLAN.md, not here, because neither library is a
 * dependency of this test suite.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/markup.h>

#include "markup_test_support.h"

namespace {

// Collects what a sink is given and can be told to refuse the Nth write.
struct Collector {
  std::string text;
  std::vector<size_t> chunk_sizes;
  size_t writes = 0;
  size_t refuse_on = 0;  // 1-based; 0 = never
  GTEXT_Markup_Status refuse_with = GTEXT_MARKUP_E_WRITE;

  static GTEXT_Markup_Status write(void * user, const char * bytes, size_t len) {
    auto * self = static_cast<Collector *>(user);
    self->writes++;
    if (self->refuse_on != 0 && self->writes == self->refuse_on) {
      return self->refuse_with;
    }
    self->text.append(bytes, len);
    self->chunk_sizes.push_back(len);
    return GTEXT_MARKUP_OK;
  }
  GTEXT_Markup_Sink sink() {
    return GTEXT_Markup_Sink{&Collector::write, this};
  }
};

std::string escape(GTEXT_Markup_Escape_Mode mode, const std::string & in,
    GTEXT_Markup_Status * status = nullptr) {
  Collector c;
  const GTEXT_Markup_Sink sink = c.sink();
  const GTEXT_Markup_Status s = gtext_markup_escape(mode, in.data(), in.size(), &sink);
  if (status) {
    *status = s;
  }
  return c.text;
}

const GTEXT_Markup_Escape_Mode kText = GTEXT_MARKUP_ESCAPE_XML_TEXT;
const GTEXT_Markup_Escape_Mode kAttr = GTEXT_MARKUP_ESCAPE_XML_ATTRIBUTE;

std::string utf8(uint32_t cp) {
  std::string s;
  if (cp < 0x80) {
    s += (char)cp;
  }
  else if (cp < 0x800) {
    s += (char)(0xC0 | (cp >> 6));
    s += (char)(0x80 | (cp & 0x3F));
  }
  else if (cp < 0x10000) {
    s += (char)(0xE0 | (cp >> 12));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
  else {
    s += (char)(0xF0 | (cp >> 18));
    s += (char)(0x80 | ((cp >> 12) & 0x3F));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
  return s;
}

}  // namespace

// ---- What gets escaped -----------------------------------------------------

TEST(MarkupEscape, TextEscapesAmpersandAndBothAngleBrackets) {
  EXPECT_EQ(escape(kText, "a<b>c&d"), "a&lt;b&gt;c&amp;d");
}

TEST(MarkupEscape, TextLeavesQuotesTabsAndLineFeedsAlone) {
  EXPECT_EQ(escape(kText, "say \"hi\" 'there'\t\n"), "say \"hi\" 'there'\t\n");
}

TEST(MarkupEscape, TextWritesACarriageReturnAsAReferenceBecauseAnXmlParserNormalizesARawOne) {
  // XML 2.11: a parser turns a raw CR LF, and a lone CR, into LF before it
  // looks at anything else. Written raw, a CR would not come back as a CR.
  EXPECT_EQ(escape(kText, "a\r\nb\rc"), "a&#13;\nb&#13;c");
}

TEST(MarkupEscape, AnAttributeEscapesQuoteAndTheThreeWhitespaceCharacters) {
  // XML 3.3.3: a raw tab, LF or CR in an attribute value is read back as a
  // space, so each is written as a character reference.
  EXPECT_EQ(escape(kAttr, "a\"b\tc\nd\re"), "a&quot;b&#9;c&#10;d&#13;e");
}

TEST(MarkupEscape, AnAttributeStillEscapesAmpersandAndAngleBrackets) {
  EXPECT_EQ(escape(kAttr, "<&>"), "&lt;&amp;&gt;");
}

TEST(MarkupEscape, AnAttributeLeavesASingleQuoteAloneBecauseItIsWrittenInDoubleQuotes) {
  EXPECT_EQ(escape(kAttr, "it's"), "it's");
}

TEST(MarkupEscape, MultiByteCharactersPassThroughWhole) {
  const std::string in = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 <";
  EXPECT_EQ(escape(kText, in), "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 &lt;");
}

TEST(MarkupEscape, EmptyInputWritesNothingAtAll) {
  Collector c;
  const GTEXT_Markup_Sink sink = c.sink();
  EXPECT_EQ(gtext_markup_escape(kText, "", 0, &sink), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_escape(kText, nullptr, 0, &sink), GTEXT_MARKUP_OK);
  EXPECT_EQ(c.writes, 0u);
}

TEST(MarkupEscape, NothingToEscapeIsOneWrite) {
  Collector c;
  const GTEXT_Markup_Sink sink = c.sink();
  ASSERT_EQ(gtext_markup_escape(kText, "plain text", 10, &sink), GTEXT_MARKUP_OK);
  EXPECT_EQ(c.writes, 1u);
  EXPECT_EQ(c.text, "plain text");
}

TEST(MarkupEscape, NoWriteIsEverEmpty) {
  Collector c;
  const GTEXT_Markup_Sink sink = c.sink();
  ASSERT_EQ(gtext_markup_escape(kText, "<<a&&>>", 7, &sink), GTEXT_MARKUP_OK);
  for (size_t n : c.chunk_sizes) {
    EXPECT_GT(n, 0u);
  }
  EXPECT_EQ(c.text, "&lt;&lt;a&amp;&amp;&gt;&gt;");
}

TEST(MarkupEscape, AnEmbeddedNulIsRefusedBecauseXmlHasNoSpellingForIt) {
  GTEXT_Markup_Status s;
  escape(kText, std::string("a\0b", 3), &s);
  EXPECT_EQ(s, GTEXT_MARKUP_E_UNREPRESENTABLE);
}

// ---- What is refused, and that nothing is written when it is --------------

TEST(MarkupEscape, ARefusalHasWrittenNothingEvenWhenTheBadByteIsAtTheEnd) {
  Collector c;
  const GTEXT_Markup_Sink sink = c.sink();
  const std::string in = "a long run of fine text, then <" + utf8(0x1);
  EXPECT_EQ(gtext_markup_escape(kText, in.data(), in.size(), &sink),
      GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(c.writes, 0u);
}

TEST(MarkupEscape, EveryCharacterXml10ForbidsIsRefusedAndEveryOtherIsNot) {
  // XML 1.0 Char: #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] |
  // [#x10000-#x10FFFF]. Checked over the whole code space: everything outside
  // it is E_UNREPRESENTABLE, everything inside it is accepted. The surrogates
  // cannot be spelled in UTF-8 and are covered below.
  for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      continue;
    }
    const bool allowed = cp == 0x9 || cp == 0xA || cp == 0xD
        || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD)
        || (cp >= 0x10000 && cp <= 0x10FFFF);
    const std::string s = utf8(cp);
    const GTEXT_Markup_Status got = gtext_markup_check_xml_chars(s.data(), s.size());
    ASSERT_EQ(got, allowed ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_UNREPRESENTABLE)
        << "U+" << std::hex << cp;
  }
}

TEST(MarkupEscape, TheNoncharactersOtherThanFffeAndFfffAreAllowedInXml10) {
  // U+FDD0 and U+1FFFE are Unicode noncharacters and XML 1.0 Char admits them.
  for (uint32_t cp : {0xFDD0u, 0xFDEFu, 0x1FFFEu, 0x10FFFFu}) {
    const std::string s = utf8(cp);
    EXPECT_EQ(gtext_markup_check_xml_chars(s.data(), s.size()), GTEXT_MARKUP_OK)
        << std::hex << cp;
  }
}

TEST(MarkupEscape, BytesThatAreNotUtf8AreAnEncodingErrorNotAnUnrepresentableOne) {
  const std::vector<std::string> bad = {
      "\x80",             // a lone continuation byte
      "\xC0\x80",         // an overlong NUL
      "\xC1\xBF",         // an overlong DEL
      "\xE0\x80\x80",     // an overlong three-byte
      "\xED\xA0\x80",     // a UTF-16 surrogate, U+D800
      "\xF4\x90\x80\x80", // above U+10FFFF
      "\xF8\x88\x80\x80\x80", // a five-byte form
      "\xC3",             // truncated
      "\xE2\x82",         // truncated
      "ok\xFF"};
  for (const std::string & b : bad) {
    EXPECT_EQ(gtext_markup_check_xml_chars(b.data(), b.size()),
        GTEXT_MARKUP_E_ENCODING) << b.size() << " bytes starting " << (int)(uint8_t)b[0];
    GTEXT_Markup_Status s;
    escape(kText, b, &s);
    EXPECT_EQ(s, GTEXT_MARKUP_E_ENCODING);
  }
}

TEST(MarkupEscape, BadArgumentsAreInvalidAndWriteNothing) {
  Collector c;
  const GTEXT_Markup_Sink sink = c.sink();
  const GTEXT_Markup_Sink no_write = {nullptr, nullptr};
  EXPECT_EQ(gtext_markup_escape(kText, "a", 1, nullptr), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_escape(kText, "a", 1, &no_write), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_escape(kText, nullptr, 3, &sink), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_escape((GTEXT_Markup_Escape_Mode)77, "a", 1, &sink),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(c.writes, 0u);
}

// ---- A sink that fails -----------------------------------------------------

TEST(MarkupEscape, ASinkThatRefusesStopsTheWritingAndItsStatusComesBackUnchanged) {
  const std::string in = "a<b>c&d";
  // Find how many writes a full run makes, then refuse each in turn.
  Collector full;
  const GTEXT_Markup_Sink full_sink = full.sink();
  ASSERT_EQ(gtext_markup_escape(kText, in.data(), in.size(), &full_sink),
      GTEXT_MARKUP_OK);
  ASSERT_GT(full.writes, 3u);
  for (size_t n = 1; n <= full.writes; n++) {
    Collector c;
    c.refuse_on = n;
    c.refuse_with = GTEXT_MARKUP_E_OOM;
    const GTEXT_Markup_Sink sink = c.sink();
    EXPECT_EQ(gtext_markup_escape(kText, in.data(), in.size(), &sink),
        GTEXT_MARKUP_E_OOM) << "refused write " << n;
    EXPECT_EQ(c.writes, n) << "nothing is written after a refusal";
  }
}

// ---- Escaping into a string ------------------------------------------------

TEST(MarkupEscapeAlloc, GivesTheSameBytesAsTheSinkAndTerminatesThem) {
  const std::string in = "x<y & \"z\"\r\n";
  for (auto mode : {kText, kAttr}) {
    char * out = nullptr;
    size_t length = 0;
    ASSERT_EQ(gtext_markup_escape_alloc(mode, in.data(), in.size(), nullptr, &out,
                  &length), GTEXT_MARKUP_OK);
    EXPECT_EQ(std::string(out, length), escape(mode, in));
    EXPECT_EQ(out[length], '\0');
    gtext_markup_string_free(nullptr, out);
  }
}

TEST(MarkupEscapeAlloc, AnEmptyInputGivesAnEmptyStringNotNull) {
  char * out = nullptr;
  size_t length = 5;
  ASSERT_EQ(gtext_markup_escape_alloc(kText, "", 0, nullptr, &out, &length),
      GTEXT_MARKUP_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "");
  EXPECT_EQ(length, 0u);
  gtext_markup_string_free(nullptr, out);
}

TEST(MarkupEscapeAlloc, FailsCleanlyAtEveryAllocationAndLeavesNothingBehind) {
  const std::string in = std::string(500, 'a') + "<&>" + std::string(500, 'b');
  FaultAllocator fa;
  // Count how many allocations a good run makes, then fail each in turn.
  char * good = nullptr;
  fa.arm(0);
  ASSERT_EQ(gtext_markup_escape_alloc(kText, in.data(), in.size(), &fa.alloc, &good,
                nullptr), GTEXT_MARKUP_OK);
  const size_t total = fa.calls;
  gtext_markup_string_free(&fa.alloc, good);
  ASSERT_GE(total, 2u);
  for (size_t n = 1; n <= total; n++) {
    char * out = reinterpret_cast<char *>(0x1);
    fa.arm(n);
    EXPECT_EQ(gtext_markup_escape_alloc(kText, in.data(), in.size(), &fa.alloc, &out,
                  nullptr), GTEXT_MARKUP_E_OOM) << "allocation " << n;
    EXPECT_EQ(out, reinterpret_cast<char *>(0x1));
    EXPECT_TRUE(fa.live.empty()) << "leaked after failing allocation " << n;
  }
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(MarkupEscapeAlloc, AnOutputPointerIsRequiredAndARefusalIsPassedOn) {
  EXPECT_EQ(gtext_markup_escape_alloc(kText, "a", 1, nullptr, nullptr, nullptr),
      GTEXT_MARKUP_E_INVALID);
  char * out = nullptr;
  EXPECT_EQ(gtext_markup_escape_alloc(kText, "\x01", 1, nullptr, &out, nullptr),
      GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(out, nullptr);
}

// ---- Text that has no spelling ----------------------------------------------

TEST(MarkupCheck, ACommentMayNotContainTwoHyphensOrEndInOne) {
  // XML [15]: Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'.
  auto check = [](const std::string & s) {
    return gtext_markup_check_xml_comment(s.data(), s.size());
  };
  EXPECT_EQ(check(""), GTEXT_MARKUP_OK);
  EXPECT_EQ(check("a - b"), GTEXT_MARKUP_OK);
  EXPECT_EQ(check("-a-b"), GTEXT_MARKUP_OK);
  EXPECT_EQ(check("a--b"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("--"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("a-"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("-"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("a---"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("a\x01"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("\xFF"), GTEXT_MARKUP_E_ENCODING);
}

TEST(MarkupCheck, AProcessingInstructionsDataMayNotContainQuestionMarkGreaterThan) {
  // XML [16]: PI ::= '<?' PITarget (S (Char* - (Char* '?>' Char*)))? '?>'.
  auto check = [](const std::string & s) {
    return gtext_markup_check_xml_pi_data(s.data(), s.size());
  };
  EXPECT_EQ(check(""), GTEXT_MARKUP_OK);
  EXPECT_EQ(check("a ? b > c"), GTEXT_MARKUP_OK);
  EXPECT_EQ(check("x?"), GTEXT_MARKUP_OK);
  EXPECT_EQ(check(">"), GTEXT_MARKUP_OK);
  EXPECT_EQ(check("x?>y"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("?>"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("\x02"), GTEXT_MARKUP_E_UNREPRESENTABLE);
}

TEST(MarkupCheck, ACdataSectionMayNotContainBracketBracketGreaterThan) {
  // XML [20]: CData ::= (Char* - (Char* ']]>' Char*)).
  auto check = [](const std::string & s) {
    return gtext_markup_check_xml_cdata(s.data(), s.size());
  };
  EXPECT_EQ(check(""), GTEXT_MARKUP_OK);
  EXPECT_EQ(check("]] >"), GTEXT_MARKUP_OK);
  EXPECT_EQ(check("]>"), GTEXT_MARKUP_OK);
  EXPECT_EQ(check("<a & b>"), GTEXT_MARKUP_OK);
  EXPECT_EQ(check("a]]>b"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("]]>"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("]]]>"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("\x01"), GTEXT_MARKUP_E_UNREPRESENTABLE);
  EXPECT_EQ(check("\xFF"), GTEXT_MARKUP_E_ENCODING);
}

// ---- Names -----------------------------------------------------------------

namespace {

// The specification's own text for the two productions, so the table here is
// the text and not a second set of comparisons. Parsed below.
const char * kNameStartChar =
    "\":\" | [A-Z] | \"_\" | [a-z] | [#xC0-#xD6] | [#xD8-#xF6] | [#xF8-#x2FF] | "
    "[#x370-#x37D] | [#x37F-#x1FFF] | [#x200C-#x200D] | [#x2070-#x218F] | "
    "[#x2C00-#x2FEF] | [#x3001-#xD7FF] | [#xF900-#xFDCF] | [#xFDF0-#xFFFD] | "
    "[#x10000-#xEFFFF]";
const char * kNameCharExtra =
    "\"-\" | \".\" | [0-9] | #xB7 | [#x0300-#x036F] | [#x203F-#x2040]";

struct Range {
  uint32_t lo, hi;
};

uint32_t parse_ref(const std::string & s) {
  if (s.size() >= 2 && s[0] == '#' && s[1] == 'x') {
    return (uint32_t)std::stoul(s.substr(2), nullptr, 16);
  }
  if (s.size() == 3 && s[0] == '"') {  // a quoted character
    return (unsigned char)s[1];
  }
  return (unsigned char)s[0];  // a bare character inside [a-z]
}

std::vector<Range> parse_production(const std::string & text) {
  std::vector<Range> out;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '|')) {
      i++;
    }
    if (i >= text.size()) {
      break;
    }
    size_t j = i;
    if (text[i] == '[') {
      j = text.find(']', i);
      const std::string body = text.substr(i + 1, j - i - 1);
      const size_t dash = body.find('-', body[0] == '#' ? 0 : 1);
      const uint32_t lo = parse_ref(body.substr(0, dash));
      const uint32_t hi = parse_ref(body.substr(dash + 1));
      out.push_back({lo, hi});
      i = j + 1;
    }
    else if (text[i] == '"') {
      j = text.find('"', i + 1);
      const uint32_t c = (unsigned char)text[i + 1];
      out.push_back({c, c});
      i = j + 1;
    }
    else {  // #xB7
      j = text.find(' ', i);
      if (j == std::string::npos) {
        j = text.size();
      }
      const uint32_t c = parse_ref(text.substr(i, j - i));
      out.push_back({c, c});
      i = j;
    }
  }
  return out;
}

bool in_ranges(const std::vector<Range> & ranges, uint32_t cp) {
  for (const Range & r : ranges) {
    if (cp >= r.lo && cp <= r.hi) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(MarkupNames, TheTableParsedFromTheSpecificationHasTheRangesItShould) {
  // A guard on the test's own oracle: if the parser above dropped a range this
  // would otherwise "agree" with a predicate that dropped the same one.
  // ":", [A-Z], "_", [a-z], and the twelve numeric ranges.
  EXPECT_EQ(parse_production(kNameStartChar).size(), 16u);
  EXPECT_EQ(parse_production(kNameCharExtra).size(), 6u);
}

TEST(MarkupNames, EveryCodePointIsANameStartCharExactlyWhereTheSpecificationSays) {
  const std::vector<Range> start = parse_production(kNameStartChar);
  for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      continue;
    }
    const std::string s = utf8(cp);
    ASSERT_EQ(gtext_markup_is_xml_name(s.data(), s.size()), in_ranges(start, cp))
        << "U+" << std::hex << cp << " as a name of one character";
  }
}

TEST(MarkupNames, EveryCodePointIsANameCharExactlyWhereTheSpecificationSays) {
  const std::vector<Range> start = parse_production(kNameStartChar);
  const std::vector<Range> extra = parse_production(kNameCharExtra);
  for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      continue;
    }
    const std::string s = "a" + utf8(cp);
    ASSERT_EQ(gtext_markup_is_xml_name(s.data(), s.size()),
        in_ranges(start, cp) || in_ranges(extra, cp))
        << "U+" << std::hex << cp << " after a letter";
  }
}

TEST(MarkupNames, AnNcnameIsANameWithNoColonAnywhere) {
  const char * names[] = {"a", "a:b", ":a", "a:", "a-b.c_d", "\xC3\xA9t\xC3\xA9", "x1"};
  for (const char * n : names) {
    const bool has_colon = strchr(n, ':') != nullptr;
    EXPECT_TRUE(gtext_markup_is_xml_name(n, strlen(n))) << n;
    EXPECT_EQ(gtext_markup_is_xml_ncname(n, strlen(n)), !has_colon) << n;
  }
}

TEST(MarkupNames, WhatIsNotAName) {
  for (const char * n : {"", "1a", "-a", ".a", "a b", "a<b", "a/b", "a=b", "\xC2\xB7x"}) {
    EXPECT_FALSE(gtext_markup_is_xml_name(n, strlen(n))) << "[" << n << "]";
    EXPECT_FALSE(gtext_markup_is_xml_ncname(n, strlen(n))) << "[" << n << "]";
  }
  EXPECT_FALSE(gtext_markup_is_xml_name(nullptr, 3));
  EXPECT_FALSE(gtext_markup_is_xml_name(nullptr, 0));
  EXPECT_FALSE(gtext_markup_is_xml_name("\xFF", 1));
  EXPECT_FALSE(gtext_markup_is_xml_name("a\xFF", 2));
}

TEST(MarkupNames, TheLengthIsHonouredNotTheTerminator) {
  EXPECT_TRUE(gtext_markup_is_xml_name("ab cd", 2));
  EXPECT_FALSE(gtext_markup_is_xml_name("ab cd", 3));
  EXPECT_FALSE(gtext_markup_is_xml_name("a\0b", 3));
}
