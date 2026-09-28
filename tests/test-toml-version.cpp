/**
 * @file
 *
 * The TOML version option, at its three points of use.
 *
 * Phase 2's finding was that a corpus comparing by *value* is structurally
 * blind to a writer option that only changes spelling, and this file exists
 * because the version option has the same shape of blindness in a different
 * place. toml-test has two manifests and each arm is only ever asked the cases
 * its own manifest decides, so a `version` field that nothing reads scores
 * 100% on both lists. The suite's `crossed` mode closes most of that - it runs
 * each arm over the fifteen cases the other manifest drops and requires the
 * wrong answer - and what is left for here is everything the corpus has no
 * case for at all:
 *
 *   - the three ways a caller can end up with the default, of which the
 *     conformance runner uses one;
 *   - the *value* a 1.1.0-only spelling produces, not merely that it was
 *     accepted. `\xf8` is U+00F8 and so two bytes of UTF-8; a reader that
 *     wrote one Latin-1 byte would be accepted by the crossed mode, which asks
 *     only whether the document was refused;
 *   - where 1.1.0's relaxation *stops*. `inline-table-sep` and
 *     `inline-table-open` are followed by `ws-comment-newline`, and
 *     `keyval-sep` is still plain `ws`, so `{a\n= 1}` is invalid at 1.1.0 as
 *     well. **The corpus has no case either way for that**, in either
 *     manifest: a parser that relaxed the whole inline table to
 *     skip_array_space() scores 100% on both lists and on the crossed mode
 *     too. It is pinned here because nothing else can see it;
 *   - an unrecognised version value, which must read as the strict arm;
 *   - that the writer needs no version option, stated as a round trip rather
 *     than as a claim in a comment.
 *
 * What each channel catches, measured by planting eight defects in the option
 * and running all four - the 1.0.0 manifest, the 1.1.0 manifest, the suite's
 * crossed mode, and this file. A dash is "silent"; a number is cases:
 *
 *   defect                                     1.0.0  1.1.0  crossed  here
 *   the inline-table skipper relaxed at both        4      -        4   yes
 *   \e and \xHH accepted at both                   1      -        3   yes
 *   an omitted second assumed at both              3      -        4   yes
 *   the option stored and never put on the ctx     -     11       15   yes
 *   \xHH appended as one byte, not a scalar        -      2        -   yes
 *   a lone CR admitted at 1.0.0                    -      -        2   yes
 *   a newline between an inline key and its =      -      -        -   ONLY
 *   a newline between an inline = and its value    -      -        -   ONLY
 *
 * So the corpus caught six of the eight and the crossed mode was the only
 * corpus channel to see one of those six; two are here or nowhere. The control
 * is the unmutated tree, which is silent in all four.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>
#include <gtest/gtest.h>

#include <ghoti.io/text/toml.h>

namespace {

GTEXT_TOML_Parse_Options at(GTEXT_TOML_Version version) {
  GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
  opts.version = version;
  return opts;
}

// Parse under `opts`, requiring success.
GTEXT_TOML_Value * ok(
    const std::string & text, const GTEXT_TOML_Parse_Options * opts) {
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root =
      gtext_toml_parse(text.data(), text.size(), opts, &err);
  EXPECT_NE(root, nullptr) << "[" << text << "]: "
                           << (err.message ? err.message : "no message");
  gtext_toml_error_free(&err);
  return root;
}

// Parse under `opts`, requiring failure, and return the status.
GTEXT_TOML_Status refused(
    const std::string & text, const GTEXT_TOML_Parse_Options * opts) {
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root =
      gtext_toml_parse(text.data(), text.size(), opts, &err);
  EXPECT_EQ(root, nullptr) << "[" << text << "] was accepted";
  gtext_toml_free(root);
  GTEXT_TOML_Status code = err.code;
  gtext_toml_error_free(&err);
  return code;
}

GTEXT_TOML_Value * ok_1_0(const std::string & text) {
  GTEXT_TOML_Parse_Options opts = at(GTEXT_TOML_VERSION_1_0_0);
  return ok(text, &opts);
}

GTEXT_TOML_Value * ok_1_1(const std::string & text) {
  GTEXT_TOML_Parse_Options opts = at(GTEXT_TOML_VERSION_1_1_0);
  return ok(text, &opts);
}

GTEXT_TOML_Status refused_1_0(const std::string & text) {
  GTEXT_TOML_Parse_Options opts = at(GTEXT_TOML_VERSION_1_0_0);
  return refused(text, &opts);
}

GTEXT_TOML_Status refused_1_1(const std::string & text) {
  GTEXT_TOML_Parse_Options opts = at(GTEXT_TOML_VERSION_1_1_0);
  return refused(text, &opts);
}

const GTEXT_TOML_Value * key(const GTEXT_TOML_Value * table, const char * k) {
  return gtext_toml_table_get(table, k, std::strlen(k));
}

std::string string_of(const GTEXT_TOML_Value * value) {
  size_t len = 0;
  const char * bytes = gtext_toml_value_string(value, &len);
  return bytes ? std::string(bytes, len) : std::string("<not a string>");
}

// The string value of a top-level key, at 1.1.0.
std::string string_1_1(const std::string & document, const char * k) {
  GTEXT_TOML_Value * root = ok_1_1(document);
  if (!root) return "<refused>";
  std::string out = string_of(key(root, k));
  gtext_toml_free(root);
  return out;
}

// A date-time's every field, so a test can assert the value and not only that
// something was accepted. The kind is spelled by name: it is a 1-based enum,
// and a test written against the numbers says nothing a reader can check.
std::string datetime_of(const GTEXT_TOML_Value * value) {
  GCHRON_TomlValue dt;
  std::memset(&dt, 0, sizeof(dt));
  if (!gtext_toml_value_datetime(value, &dt)) return "<not a date-time>";
  const char * kind = "?";
  switch (dt.kind) {
    case GCHRON_TOML_NONE: kind = "none"; break;
    case GCHRON_TOML_OFFSET_DATE_TIME: kind = "odt"; break;
    case GCHRON_TOML_LOCAL_DATE_TIME: kind = "ldt"; break;
    case GCHRON_TOML_LOCAL_DATE: kind = "ld"; break;
    case GCHRON_TOML_LOCAL_TIME: kind = "lt"; break;
  }
  char text[160];
  std::snprintf(text, sizeof(text),
      "%s %04d-%02d-%02d %02d:%02d:%02d.%09d %d%s", kind,
      (int) dt.civil.date.year, (int) dt.civil.date.month,
      (int) dt.civil.date.day, (int) dt.civil.time.hour,
      (int) dt.civil.time.minute, (int) dt.civil.time.second,
      (int) dt.civil.time.nsec, (int) dt.offset_sec,
      dt.offset_unknown ? " unknown" : "");
  return std::string(text);
}

std::string datetime_1_1(const std::string & document, const char * k) {
  GTEXT_TOML_Value * root = ok_1_1(document);
  if (!root) return "<refused>";
  std::string out = datetime_of(key(root, k));
  gtext_toml_free(root);
  return out;
}

} // namespace

// -------------------------------------------------------------------------
// Which version a caller gets without asking
// -------------------------------------------------------------------------

TEST(TomlVersion, ThreeWaysToTheDefaultAllGiveOneZeroZero) {
  // The released version, by every route a caller can reach it: the documented
  // constructor, a zeroed struct, and no options at all. The conformance
  // runner uses the first of those only, so a default that came out as 1.1.0
  // through one of the other two would score 100% and ship.
  const std::string only_1_1 = "a = \"\\e\"\n";

  GTEXT_TOML_Parse_Options constructed = gtext_toml_parse_options_default();
  EXPECT_EQ(constructed.version, GTEXT_TOML_VERSION_1_0_0);
  EXPECT_EQ(refused(only_1_1, &constructed), GTEXT_TOML_E_BAD_ESCAPE);

  GTEXT_TOML_Parse_Options zeroed;
  std::memset(&zeroed, 0, sizeof(zeroed));
  EXPECT_EQ(zeroed.version, GTEXT_TOML_VERSION_1_0_0);
  EXPECT_EQ(refused(only_1_1, &zeroed), GTEXT_TOML_E_BAD_ESCAPE);

  EXPECT_EQ(refused(only_1_1, nullptr), GTEXT_TOML_E_BAD_ESCAPE);
}

TEST(TomlVersion, AnUnrecognisedVersionReadsAsTheStrictArm) {
  // The switch is written as "is this 1.1.0" and not as "is this 1.0.0", so a
  // value from a later revision of this enum - or a struct a caller filled in
  // from a configuration file - gets the released grammar rather than a draft
  // one. Asserted because the alternative spelling is one character away and
  // fails in the direction that accepts more.
  GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
  opts.version = (GTEXT_TOML_Version) 7;
  EXPECT_EQ(refused("a = \"\\e\"\n", &opts), GTEXT_TOML_E_BAD_ESCAPE);
  EXPECT_EQ(refused("a = {b = 1,}\n", &opts), GTEXT_TOML_E_BAD_TOKEN);
  EXPECT_EQ(refused("a = 07:32\n", &opts), GTEXT_TOML_E_BAD_TOKEN);
}

// -------------------------------------------------------------------------
// Point of use 1: the escape scanner
// -------------------------------------------------------------------------

TEST(TomlVersion, EscEscapeIsOneOneZeroOnly) {
  EXPECT_EQ(refused_1_0("a = \"\\e\"\n"), GTEXT_TOML_E_BAD_ESCAPE);
  EXPECT_EQ(string_1_1("a = \"\\e\"\n", "a"), std::string("\x1B"));
  // And in a multi-line basic string, which is a different scanner path.
  EXPECT_EQ(string_1_1("a = \"\"\"x\\ey\"\"\"\n", "a"), std::string("x\x1By"));
}

TEST(TomlVersion, HexEscapeNamesAScalarValueAndNotAByte) {
  EXPECT_EQ(refused_1_0("a = \"\\x41\"\n"), GTEXT_TOML_E_BAD_ESCAPE);
  EXPECT_EQ(string_1_1("a = \"\\x41\"\n", "a"), std::string("A"));
  EXPECT_EQ(string_1_1("a = \"\\x00\"\n", "a"), std::string("\0", 1));
  EXPECT_EQ(string_1_1("a = \"\\x7f\"\n", "a"), std::string("\x7F"));
  // The case that separates a scalar from a byte: 1.1.0 says "all TOML strings
  // are sequences of Unicode characters, _not_ byte sequences", so \xf8 is
  // U+00F8 and two bytes of UTF-8. A reader that appended one 0xF8 byte would
  // pass the crossed mode, which asks only whether the document was accepted,
  // and would then emit a document that is not UTF-8.
  EXPECT_EQ(string_1_1("a = \"S\\xf8rmirb\\xe6ren\"\n", "a"),
      std::string("S\xC3\xB8rmirb\xC3\xA6ren"));
  EXPECT_EQ(string_1_1("a = \"\\xff\"\n", "a"), std::string("\xC3\xBF"));
}

TEST(TomlVersion, AMalformedHexEscapeIsStillRefusedAtOneOneZero) {
  // The relaxation is "this escape exists", not "anything after \x is fine".
  EXPECT_EQ(refused_1_1("a = \"\\xZZ\"\n"), GTEXT_TOML_E_BAD_ESCAPE);
  EXPECT_EQ(refused_1_1("a = \"\\x4\"\n"), GTEXT_TOML_E_BAD_ESCAPE);
  EXPECT_EQ(refused_1_1("a = \"\\x\"\n"), GTEXT_TOML_E_BAD_ESCAPE);
  // Two digits and no more: the third is content, not part of the escape.
  EXPECT_EQ(string_1_1("a = \"\\x414\"\n", "a"), std::string("A4"));
}

TEST(TomlVersion, NeitherNewEscapeAppliesInALiteralString) {
  // A literal string has no escapes at either version, so these are backslash
  // and letters. The corpus covers it at 1.1.0 only; the point is that the
  // version option did not reach a scanner it has no business in.
  EXPECT_EQ(string_1_1("a = '\\x20'\n", "a"), std::string("\\x20"));
  EXPECT_EQ(string_1_1("a = '\\e'\n", "a"), std::string("\\e"));
  GTEXT_TOML_Value * root = ok_1_0("a = '\\x20'\nb = '\\e'\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(string_of(key(root, "a")), std::string("\\x20"));
  EXPECT_EQ(string_of(key(root, "b")), std::string("\\e"));
  gtext_toml_free(root);
}

TEST(TomlVersion, EveryOtherEscapeIsStillRefusedAtBothVersions) {
  // 1.1.0 adds two escapes and reserves the rest, so the default arm of the
  // escape scanner must not have become "accept it" for either version.
  for (const char * bad : {"a = \"\\q\"\n", "a = \"\\y\"\n", "a = \"\\ \"\n",
           "a = \"\\1\"\n", "a = \"\\X41\"\n", "a = \"\\E\"\n"}) {
    EXPECT_EQ(refused_1_0(bad), GTEXT_TOML_E_BAD_ESCAPE) << bad;
    EXPECT_EQ(refused_1_1(bad), GTEXT_TOML_E_BAD_ESCAPE) << bad;
  }
}

// -------------------------------------------------------------------------
// Point of use 2: the inline-table states of the value parser
// -------------------------------------------------------------------------

TEST(TomlVersion, InlineTableNewlinesAndTrailingCommaAreOneOneZeroOnly) {
  struct Case {
    const char * document;
    const char * why;
  };
  const Case cases[] = {
      {"a = {b = 1,}\n", "a trailing comma"},
      {"a = {\n  b = 1\n}\n", "a newline after the brace"},
      {"a = {b = 1\n}\n", "a newline before the closing brace"},
      {"a = {b = 1,\n  c = 2}\n", "a newline after the comma"},
      {"a = {b = 1\n  ,c = 2}\n", "a newline before the comma"},
      {"a = {# why\n  b = 1}\n", "a comment after the brace"},
      {"a = {b = 1, # why\n  c = 2}\n", "a comment before the comma... "},
      {"a = {\n}\n", "an empty inline table spanning lines"},
      {"a = {b = 1,\n}\n", "both at once"},
  };
  for (const Case & c : cases) {
    EXPECT_EQ(refused_1_0(c.document), GTEXT_TOML_E_BAD_TOKEN) << c.why;
    GTEXT_TOML_Value * root = ok_1_1(c.document);
    EXPECT_NE(root, nullptr) << c.why;
    gtext_toml_free(root);
  }
}

TEST(TomlVersion, OneOneZeroRelaxesTheSeparatorsAndNotTheKeyvalItself) {
  // Where the relaxation stops, and the reason this file exists: 1.1.0's
  // `inline-table-open` and `inline-table-sep` are each followed by
  // `ws-comment-newline`, while `keyval-sep` is still plain `ws`. So a line
  // break may sit next to a brace or a comma and may not sit inside a pair.
  //
  // **Neither manifest has a case for this**, and that was measured rather than
  // assumed: relaxing the key-to-`=` gap and the `=`-to-value gap at 1.1.0 were
  // planted as two separate defects, and each left both manifests at 100% in
  // every mode, the crossed mode included, with only this test failing. Every
  // case that exists breaks the line at a separator.
  //
  // The sloppier version of that mutation *is* caught, which is the trap:
  // relaxing the `=`-to-value gap without restricting it to an inline table
  // also relaxes a top-level `key =` line, and `invalid/key/newline-06` refuses
  // that. A mutation that leaks outside the code under test measures the leak.
  for (const char * bad : {"a = {b\n= 1}\n", "a = {b =\n1}\n",
           "a = {b\n.c = 1}\n", "a = {b.\nc = 1}\n", "a = {b # why\n= 1}\n"}) {
    EXPECT_NE(refused_1_1(bad), GTEXT_TOML_OK) << bad;
    EXPECT_NE(refused_1_0(bad), GTEXT_TOML_OK) << bad;
  }
  // The same break one character earlier or later is legal at 1.1.0, which is
  // what makes the pair above a statement about `keyval-sep` rather than about
  // newlines in general.
  GTEXT_TOML_Value * root = ok_1_1("a = {\n  b = 1\n  ,\n  c = 2\n}\n");
  ASSERT_NE(root, nullptr);
  const GTEXT_TOML_Value * inner = key(root, "a");
  EXPECT_EQ(gtext_toml_table_size(inner), (size_t) 2);
  gtext_toml_free(root);
}

TEST(TomlVersion, AnInlineTableIsStillClosedAndStillRefusesADoubleComma) {
  // 1.1.0 permits *a* trailing comma, not any number of commas, and says
  // nothing about the rules that make an inline table self-contained. Both arms
  // refuse these, so the relaxation did not turn the comma into whitespace.
  for (const char * bad : {"a = {b = 1,,}\n", "a = {,}\n", "a = {, b = 1}\n",
           "a = {b = 1 c = 2}\n", "a = {b = 1\n\nc = 2}\n"}) {
    EXPECT_NE(refused_1_1(bad), GTEXT_TOML_OK) << bad;
    EXPECT_NE(refused_1_0(bad), GTEXT_TOML_OK) << bad;
  }
  // And a table written inline stays closed at 1.1.0: the four redefinition
  // rules are not part of the version switch.
  EXPECT_EQ(refused_1_1("a = {b = 1}\na.c = 2\n"), GTEXT_TOML_E_REDEFINE);
  EXPECT_EQ(refused_1_1("[t]\na = {b = 1}\n[t.a]\nc = 2\n"),
      GTEXT_TOML_E_REDEFINE);
}

TEST(TomlVersion, ACommentInsideAnInlineTableObeysTheControlCharacterRule) {
  // Allowing comments where 1.0.0 allowed none must not have allowed them
  // through a path that skips the checks a top-level comment gets. A lone CR
  // and a control character are refused in both places.
  EXPECT_EQ(refused_1_1("a = {b = 1 # \x01\n}\n"), GTEXT_TOML_E_CONTROL);
  EXPECT_EQ(refused_1_1("a = {b = 1\r  ,c = 2}\n"), GTEXT_TOML_E_CONTROL);
  EXPECT_EQ(refused_1_1("a = {# \x7F\n}\n"), GTEXT_TOML_E_CONTROL);
  // A CRLF is a newline, and is fine in both positions.
  GTEXT_TOML_Value * root = ok_1_1("a = {\r\n  b = 1,\r\n}\r\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(gtext_toml_table_size(key(root, "a")), (size_t) 1);
  gtext_toml_free(root);
}

TEST(TomlVersion, TheLineNumberSurvivesANewlineInsideAnInlineTable) {
  // The inline-table skipper is the array's, which counts lines; a skipper
  // that consumed newlines without counting them would report every later
  // error against the line the table opened on.
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Parse_Options opts = at(GTEXT_TOML_VERSION_1_1_0);
  const std::string text = "a = {\n  b = 1,\n  c = 2,\n}\nd = ?\n";
  GTEXT_TOML_Value * root =
      gtext_toml_parse(text.data(), text.size(), &opts, &err);
  EXPECT_EQ(root, nullptr);
  EXPECT_EQ(err.line, 5);
  gtext_toml_free(root);
  gtext_toml_error_free(&err);
}

// -------------------------------------------------------------------------
// Point of use 3: the time scanner
// -------------------------------------------------------------------------

TEST(TomlVersion, SecondsAreOptionalOnlyAtOneOneZero) {
  // 1.0.0's refusal is the scanner's own and names the version, rather than
  // being chron declining a time it was handed: the position is the point where
  // the seconds are missing, which is more use to a caller than the start of
  // the value.
  EXPECT_EQ(refused_1_0("a = 07:32\n"), GTEXT_TOML_E_BAD_TOKEN);
  EXPECT_EQ(refused_1_0("a = 1979-05-27T07:32\n"), GTEXT_TOML_E_BAD_TOKEN);
  EXPECT_EQ(refused_1_0("a = 1979-05-27 07:32Z\n"), GTEXT_TOML_E_BAD_TOKEN);
  EXPECT_EQ(refused_1_0("a = 1979-05-27T07:32-07:00\n"),
      GTEXT_TOML_E_BAD_TOKEN);
}

TEST(TomlVersion, AnOmittedSecondIsZeroAndTheKindIsUnchanged) {
  // The value, and not merely that it was accepted: 1.1.0 says "`:00` will be
  // assumed", so the second is zero and the kind is whatever the presence of a
  // date and an offset already decided. `GCHRON_TOML_*` are 0..3 in the order
  // offset-date-time, local-date-time, local-date, local-time.
  EXPECT_EQ(datetime_1_1("a = 07:32\n", "a"),
      datetime_1_1("a = 07:32:00\n", "a"));
  EXPECT_EQ(datetime_1_1("a = 1979-05-27T07:32\n", "a"),
      datetime_1_1("a = 1979-05-27T07:32:00\n", "a"));
  EXPECT_EQ(datetime_1_1("a = 1979-05-27 07:32Z\n", "a"),
      datetime_1_1("a = 1979-05-27T07:32:00Z\n", "a"));
  EXPECT_EQ(datetime_1_1("a = 1979-05-27T07:32-07:00\n", "a"),
      datetime_1_1("a = 1979-05-27T07:32:00-07:00\n", "a"));
  // Spelled out once, so the pairs above cannot both be wrong in the same way.
  EXPECT_EQ(datetime_1_1("a = 07:32\n", "a"),
      std::string("lt 1970-01-01 07:32:00.000000000 0"));
  EXPECT_EQ(datetime_1_1("a = 1979-05-27 07:32-07:00\n", "a"),
      std::string("odt 1979-05-27 07:32:00.000000000 -25200"));
  // `-00:00` is "offset unknown" and stays so with the seconds omitted.
  EXPECT_EQ(datetime_1_1("a = 1979-05-27T07:32-00:00\n", "a"),
      std::string("odt 1979-05-27 07:32:00.000000000 0 unknown"));
  EXPECT_EQ(datetime_1_1("a = 1979-05-27T07:32\n", "a"),
      std::string("ldt 1979-05-27 07:32:00.000000000 0"));
}

TEST(TomlVersion, AFractionStillBelongsToTheSecondsAtOneOneZero) {
  // The fraction hangs off the seconds in both grammars, so omitting the
  // seconds omits the fraction too: `07:32.5` is not a time. Refused at both
  // versions, and by the statement parser rather than by the time scanner -
  // the scanner stops at the `.`, which is where any other trailing byte is
  // refused as well.
  EXPECT_NE(refused_1_1("a = 07:32.5\n"), GTEXT_TOML_OK);
  EXPECT_NE(refused_1_0("a = 07:32.5\n"), GTEXT_TOML_OK);
  EXPECT_NE(refused_1_1("a = 1979-05-27T07:32.5\n"), GTEXT_TOML_OK);
  // With the seconds written, the fraction is read at both versions - which is
  // why the printed form carries the nanoseconds: without them this assertion
  // would hold just as well against a parser that dropped the fraction.
  EXPECT_EQ(datetime_1_1("a = 07:32:00.5\n", "a"),
      std::string("lt 1970-01-01 07:32:00.500000000 0"));
}

TEST(TomlVersion, AnIncompleteTimeIsRefusedAtOneOneZeroToo) {
  // Optional seconds are the only thing that moved. An hour alone, a minute of
  // one digit, and an offset with no time are refused under both arms.
  for (const char * bad : {"a = 07:\n", "a = 07:3\n", "a = 07:32:\n",
           "a = 07:32:0\n", "a = 1979-05-27T07:32:\n",
           "a = 1979-05-27T\n"}) {
    EXPECT_NE(refused_1_1(bad), GTEXT_TOML_OK) << bad;
    EXPECT_NE(refused_1_0(bad), GTEXT_TOML_OK) << bad;
  }
  // And chron still decides what a time means: 25:00 is refused by it, with
  // the seconds present or omitted.
  EXPECT_EQ(refused_1_1("a = 25:00\n"), GTEXT_TOML_E_DATETIME);
  EXPECT_EQ(refused_1_1("a = 25:00:00\n"), GTEXT_TOML_E_DATETIME);
  EXPECT_EQ(refused_1_1("a = 1979-05-27T07:60\n"), GTEXT_TOML_E_DATETIME);
}

TEST(TomlVersion, ADateAloneIsNotAffectedByTheOption) {
  // A local date has no time to omit seconds from, and the space after it is
  // still not a separator unless a time follows. Both arms, same answers.
  for (GTEXT_TOML_Version v :
      {GTEXT_TOML_VERSION_1_0_0, GTEXT_TOML_VERSION_1_1_0}) {
    GTEXT_TOML_Parse_Options opts = at(v);
    GTEXT_TOML_Value * root =
        ok("a = 1979-05-27 # a comment\nb = 1979-05-27\n", &opts);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(datetime_of(key(root, "a")),
        std::string("ld 1979-05-27 00:00:00.000000000 0"));
    EXPECT_EQ(datetime_of(key(root, "b")), datetime_of(key(root, "a")));
    gtext_toml_free(root);
    EXPECT_NE(refused("a = 1979-05\n", &opts), GTEXT_TOML_OK);
  }
}

// -------------------------------------------------------------------------
// What the version switch does not touch
// -------------------------------------------------------------------------

TEST(TomlVersion, TheTwoThingsOneOneZeroForbidsAreRefusedAtBothVersions) {
  // 1.1.0 settles a question 1.0.0 left contradictory: its prose permits a
  // carriage return inside a multi-line string ("the control characters other
  // than tab, line feed, and carriage return") and its ABNF does not, since
  // `mlb-unescaped` omits %x0D and `newline` is LF or CRLF. toml-test leaves
  // both cases out of the 1.0.0 manifest for exactly that reason, so this
  // module takes the ABNF's reading under both arms and neither list scores it.
  // Written down because "the corpus is green" is not a statement about a case
  // the corpus does not contain.
  const std::string mlb = "a = \"\"\"x\ry\"\"\"\n";
  const std::string mll = "a = '''x\ry'''\n";
  EXPECT_EQ(refused_1_0(mlb), GTEXT_TOML_E_CONTROL);
  EXPECT_EQ(refused_1_1(mlb), GTEXT_TOML_E_CONTROL);
  EXPECT_EQ(refused_1_0(mll), GTEXT_TOML_E_CONTROL);
  EXPECT_EQ(refused_1_1(mll), GTEXT_TOML_E_CONTROL);
  // An escaped one is a different thing and is content at 1.1.0, where \x0d
  // spells it - the corpus's valid/string/hex-escape depends on this.
  EXPECT_EQ(string_1_1("a = \"\"\"\\x0d\\x0a\"\"\"\n", "a"),
      std::string("\r\n"));
}

TEST(TomlVersion, AMultiLineStringIsStillNotAKeyAtEitherVersion) {
  // 1.1.0 spells this out where 1.0.0 only implied it, so the rule is the same
  // and the two arms must agree.
  EXPECT_EQ(refused_1_0("\"\"\"k\"\"\" = 1\n"), GTEXT_TOML_E_BAD_TOKEN);
  EXPECT_EQ(refused_1_1("\"\"\"k\"\"\" = 1\n"), GTEXT_TOML_E_BAD_TOKEN);
  EXPECT_EQ(refused_1_1("a = {\n  \"\"\"k\"\"\" = 1,\n}\n"),
      GTEXT_TOML_E_BAD_TOKEN);
  // An empty quoted key is legal at both, and is what valid/key/empty-05 is
  // really about once its newlines are allowed.
  GTEXT_TOML_Value * root = ok_1_1("a = {\n  \"\" = 1,\n}\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(gtext_toml_table_size(key(root, "a")), (size_t) 1);
  gtext_toml_free(root);
}

TEST(TomlVersion, BareKeysAreStillAsciiAtOneOneZero) {
  // 1.1.0 adds a note pointing at the ABNF for "the exact ranges of allowed
  // code points" and keeps the prose at `A-Za-z0-9_-`. The note reads like a
  // relaxation and is not one, so both arms refuse a non-ASCII bare key.
  EXPECT_NE(refused_1_1("caf\xC3\xA9 = 1\n"), GTEXT_TOML_OK);
  EXPECT_NE(refused_1_0("caf\xC3\xA9 = 1\n"), GTEXT_TOML_OK);
}

// -------------------------------------------------------------------------
// The writer, which has no version option
// -------------------------------------------------------------------------

TEST(TomlVersion, AOneOneZeroDocumentWritesAsTomlOneZeroZeroAccepts) {
  // Why GTEXT_TOML_Write_Options has no version field, as a measurement rather
  // than a remark: every spelling 1.0.0 defines is still a 1.1.0 spelling, and
  // nothing 1.1.0 adds survives into the writer's output. So a document only
  // 1.1.0 can read, written back, is readable by the strict arm - the seconds
  // are written, the escapes come back as \uXXXX or as the character, the
  // inline table is one line, and no trailing comma is emitted.
  //
  // A version field on the write options would be an axis with no point of
  // use, which is the same defect as a table_style that never reaches its
  // decision - and that one scored 2250 of 2250.
  const std::string source = "t = {\n"
                             "  s = \"\\e\\x41\\xf8\",\n"
                             "  lt = 07:32,\n"
                             "  ldt = 1979-05-27T07:32,\n"
                             "  odt = 1979-05-27 07:32-07:00,\n"
                             "}\n";
  EXPECT_NE(refused_1_0(source), GTEXT_TOML_OK);

  GTEXT_TOML_Value * root = ok_1_1(source);
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Sink sink;
  ASSERT_EQ(gtext_toml_sink_buffer(&sink), GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_write(root, &sink, nullptr), GTEXT_TOML_OK);
  const std::string written(
      gtext_toml_sink_buffer_data(&sink), gtext_toml_sink_buffer_size(&sink));
  gtext_toml_sink_buffer_free(&sink);

  GTEXT_TOML_Value * strict = ok_1_0(written);
  ASSERT_NE(strict, nullptr) << written;
  const GTEXT_TOML_Value * a = key(root, "t");
  const GTEXT_TOML_Value * b = key(strict, "t");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(string_of(key(a, "s")), string_of(key(b, "s")));
  EXPECT_EQ(string_of(key(b, "s")), std::string("\x1B" "A\xC3\xB8"));
  for (const char * k : {"lt", "ldt", "odt"}) {
    EXPECT_EQ(datetime_of(key(a, k)), datetime_of(key(b, k))) << k;
  }
  gtext_toml_free(strict);
  gtext_toml_free(root);
}

// -------------------------------------------------------------------------
// The write-side spelling option
//
// The other direction from the test above. That one says the writer needs no
// *version* option, because v1.0.0's spellings are all v1.1.0 spellings too.
// GTEXT_TOML_Write_Options::spellings is for the spellings v1.1.0 *adds*, and
// every one of them makes the output unreadable to a v1.0.0 reader - which is
// what makes the option measurable, and what these tests assert each time.
//
// Three properties for every bit, because two of them can pass while the bit
// does nothing:
//
//   - the bytes are what was asked for, character by character. A bit that
//     reaches nothing leaves the v1.0.0 spelling and the next two still hold;
//   - the 1.1.0 arm reads the result back with the same values, which is what
//     says the shorter spelling means the same thing;
//   - the 1.0.0 arm refuses it, which is what says the bit reached a point of
//     use at all.
//
// The corpus asks the last two over 218 valid cases (`roundtrip 1.1.0
// spellings` and `spellings reach`, 52 of which are spelt differently). What is
// here and nowhere else is the first: the exact bytes, the interactions between
// bits, and the places each bit must NOT reach.
// -------------------------------------------------------------------------

namespace {

// Write `root` with `spellings`, requiring success.
std::string write_with(
    const GTEXT_TOML_Value * root, unsigned spellings,
    const GCHRON_WriteOptions * datetime = nullptr) {
  GTEXT_TOML_Write_Options opts = gtext_toml_write_options_default();
  opts.spellings = spellings;
  opts.datetime = datetime;
  GTEXT_TOML_Sink sink;
  if (gtext_toml_sink_buffer(&sink) != GTEXT_TOML_OK) return "<no sink>";
  GTEXT_TOML_Status st = gtext_toml_write(root, &sink, &opts);
  std::string out = st == GTEXT_TOML_OK
      ? std::string(gtext_toml_sink_buffer_data(&sink),
            gtext_toml_sink_buffer_size(&sink))
      : std::string("<refused ") + std::to_string((int) st) + ">";
  gtext_toml_sink_buffer_free(&sink);
  return out;
}

// Read `document` at 1.1.0, write it with `spellings`, and return the bytes.
std::string spelt(const std::string & document, unsigned spellings) {
  GTEXT_TOML_Value * root = ok_1_1(document);
  if (!root) return "<refused>";
  std::string out = write_with(root, spellings);
  gtext_toml_free(root);
  return out;
}

// What both arms make of some bytes: "1.0.0 and 1.1.0", "1.1.0 only", or
// "neither". Written as a string so a failure names the arm rather than
// printing a bool.
std::string reads_as(const std::string & text) {
  GTEXT_TOML_Parse_Options strict = at(GTEXT_TOML_VERSION_1_0_0);
  GTEXT_TOML_Parse_Options draft = at(GTEXT_TOML_VERSION_1_1_0);
  GTEXT_TOML_Value * a = gtext_toml_parse(
      text.data(), text.size(), &strict, nullptr);
  GTEXT_TOML_Value * b = gtext_toml_parse(
      text.data(), text.size(), &draft, nullptr);
  std::string out = a ? (b ? "1.0.0 and 1.1.0" : "1.0.0 only")
                      : (b ? "1.1.0 only" : "neither");
  gtext_toml_free(a);
  gtext_toml_free(b);
  return out;
}

} // namespace

TEST(TomlSpellings, TheDefaultIsEveryReadersSpelling) {
  EXPECT_EQ(gtext_toml_write_options_default().spellings,
      (unsigned) GTEXT_TOML_SPELL_1_0_0_ONLY);
  EXPECT_EQ((unsigned) GTEXT_TOML_SPELL_1_0_0_ONLY, 0u);

  // A zeroed struct, the default, and a NULL options pointer are the three ways
  // a caller reaches the writer without choosing, and all three have to spell
  // v1.0.0 - the same three-way check the version option gets above.
  const std::string document = "s = \"\\u001B\"\nlt = 07:32:00\n"
                               "t = { a = 1 }\n";
  GTEXT_TOML_Value * root = ok_1_1(document);
  ASSERT_NE(root, nullptr);

  GTEXT_TOML_Write_Options zeroed;
  std::memset(&zeroed, 0, sizeof(zeroed));
  GTEXT_TOML_Sink sink;
  ASSERT_EQ(gtext_toml_sink_buffer(&sink), GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_write(root, &sink, &zeroed), GTEXT_TOML_OK);
  const std::string from_zeroed(
      gtext_toml_sink_buffer_data(&sink), gtext_toml_sink_buffer_size(&sink));
  gtext_toml_sink_buffer_free(&sink);

  ASSERT_EQ(gtext_toml_sink_buffer(&sink), GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_write(root, &sink, nullptr), GTEXT_TOML_OK);
  const std::string from_null(
      gtext_toml_sink_buffer_data(&sink), gtext_toml_sink_buffer_size(&sink));
  gtext_toml_sink_buffer_free(&sink);

  const std::string expected = "s = \"\\u001B\"\nlt = 07:32:00\n"
                               "t = { a = 1 }\n";
  EXPECT_EQ(from_zeroed, expected);
  EXPECT_EQ(from_null, expected);
  EXPECT_EQ(write_with(root, GTEXT_TOML_SPELL_1_0_0_ONLY), expected);
  EXPECT_EQ(reads_as(expected), std::string("1.0.0 and 1.1.0"));
  gtext_toml_free(root);
}

TEST(TomlSpellings, EscapeEIsTheOneCharacterItNames) {
  // U+001B and nothing else. U+0001 has no short escape at either version and
  // has to keep the long one, which is what says the bit did not turn into
  // "escape things differently".
  const std::string document = "s = \"\\u001B\\u0001\\u007F\"\n";
  EXPECT_EQ(spelt(document, GTEXT_TOML_SPELL_1_1_0_ESCAPE_E),
      std::string("s = \"\\e\\u0001\\u007F\"\n"));
  EXPECT_EQ(reads_as(spelt(document, GTEXT_TOML_SPELL_1_1_0_ESCAPE_E)),
      std::string("1.1.0 only"));
  EXPECT_EQ(string_1_1(spelt(document, GTEXT_TOML_SPELL_1_1_0_ESCAPE_E), "s"),
      std::string("\x1B\x01\x7F"));
}

TEST(TomlSpellings, EscapeXReachesEveryEscapedControlAndNothingElse) {
  const std::string document = "s = \"\\u001B\\u0001\\u007F\"\n";
  EXPECT_EQ(spelt(document, GTEXT_TOML_SPELL_1_1_0_ESCAPE_X),
      std::string("s = \"\\x1B\\x01\\x7F\"\n"));
  EXPECT_EQ(reads_as(spelt(document, GTEXT_TOML_SPELL_1_1_0_ESCAPE_X)),
      std::string("1.1.0 only"));
  EXPECT_EQ(string_1_1(spelt(document, GTEXT_TOML_SPELL_1_1_0_ESCAPE_X), "s"),
      std::string("\x1B\x01\x7F"));

  // A character with a short escape of its own keeps it: `\t` does not become
  // `\x09`. And a character v1.0.0 writes through as itself keeps being written
  // through - U+00F8 is two bytes of UTF-8 and `\xf8` would be a spelling no
  // rule asks for, so a document holding one still reads at 1.0.0.
  const std::string mixed = "s = \"\\t\\u00F8\"\n";
  EXPECT_EQ(spelt(mixed, GTEXT_TOML_SPELL_1_1_0_ESCAPE_X),
      std::string("s = \"\\t\xC3\xB8\"\n"));
  EXPECT_EQ(reads_as(spelt(mixed, GTEXT_TOML_SPELL_1_1_0_ESCAPE_X)),
      std::string("1.0.0 and 1.1.0"));
}

TEST(TomlSpellings, EscapeEWinsOverEscapeXForTheOneTheyShare) {
  const std::string document = "s = \"\\u001B\\u0001\"\n";
  const unsigned both = GTEXT_TOML_SPELL_1_1_0_ESCAPE_E
      | GTEXT_TOML_SPELL_1_1_0_ESCAPE_X;
  EXPECT_EQ(spelt(document, both), std::string("s = \"\\e\\x01\"\n"));
}

TEST(TomlSpellings, SecondsComeOffOnlyWhereTheyMeanNothing) {
  const std::string document =
      "lt = 07:32:00\n"
      "lt_sec = 07:32:01\n"
      "lt_frac = 07:32:00.25\n"
      "ldt = 1979-05-27T07:32:00\n"
      "odt = 1979-05-27T07:32:00-08:00\n"
      "ld = 1979-05-27\n";
  const std::string out =
      spelt(document, GTEXT_TOML_SPELL_1_1_0_TIME_NO_SECONDS);
  EXPECT_EQ(out,
      "lt = 07:32\n"
      "lt_sec = 07:32:01\n"
      /* .250, not .25: chron's default fraction rounds the digit count up to
         3, 6 or 9 - the widths an RFC 3339 reader expects. The seconds stay
         either way, which is what this line is here for. */
      "lt_frac = 07:32:00.250\n"
      "ldt = 1979-05-27T07:32\n"
      "odt = 1979-05-27T07:32-08:00\n"
      "ld = 1979-05-27\n");
  EXPECT_EQ(reads_as(out), std::string("1.1.0 only"));

  // The same instants, which is the whole claim: `:00` omitted "will be
  // assumed", so the shorter text has to read back to the value the longer one
  // did.
  GTEXT_TOML_Value * before = ok_1_1(document);
  GTEXT_TOML_Value * after = ok_1_1(out);
  ASSERT_NE(before, nullptr);
  ASSERT_NE(after, nullptr);
  for (const char * k :
      {"lt", "lt_sec", "lt_frac", "ldt", "odt", "ld"}) {
    EXPECT_EQ(datetime_of(key(before, k)), datetime_of(key(after, k))) << k;
  }
  gtext_toml_free(before);
  gtext_toml_free(after);
}

TEST(TomlSpellings, AWrittenFractionKeepsTheSecondsInFrontOfIt) {
  // `fraction_digits` 3 writes `.000` for a zero nanosecond, and the seconds
  // cannot come out from in front of a fraction. The value says they are
  // droppable and the text says they are not, and the text wins - the
  // alternative is `07:32.000`, which is not a TOML time at all.
  const std::string document = "lt = 07:32:00\n";
  GTEXT_TOML_Value * root = ok_1_1(document);
  ASSERT_NE(root, nullptr);

  GCHRON_WriteOptions dt;
  gchron_write_options_default(&dt);
  dt.fraction_digits = 3;
  EXPECT_EQ(write_with(root, GTEXT_TOML_SPELL_1_1_0_TIME_NO_SECONDS, &dt),
      std::string("lt = 07:32:00.000\n"));

  dt.fraction_digits = GCHRON_FRACTION_DIGITS_AUTO;
  EXPECT_EQ(write_with(root, GTEXT_TOML_SPELL_1_1_0_TIME_NO_SECONDS, &dt),
      std::string("lt = 07:32\n"));
  gtext_toml_free(root);
}

TEST(TomlSpellings, InlineNewlinesBreakTablesAndLeaveArraysAlone) {
  const std::string document =
      "t = { a = 1, b = { c = 2 } }\n"
      "arr = [1, 2]\n"
      "empty = {}\n";
  const std::string out =
      spelt(document, GTEXT_TOML_SPELL_1_1_0_INLINE_NEWLINES);
  EXPECT_EQ(out,
      "t = {\n"
      "  a = 1,\n"
      "  b = {\n"
      "    c = 2\n"
      "  }\n"
      "}\n"
      "arr = [1, 2]\n"
      "empty = {}\n");
  EXPECT_EQ(reads_as(out), std::string("1.1.0 only"));

  // Two spaces for each *table* that encloses the line, which is why an array
  // in between adds none: only a table breaks its lines, so there is no
  // indentation of its own for the tables inside it to be measured against.
  const std::string nested = "t = { d = [1, { e = 3 }] }\n";
  EXPECT_EQ(spelt(nested, GTEXT_TOML_SPELL_1_1_0_INLINE_NEWLINES),
      "t = {\n"
      "  d = [1, {\n"
      "    e = 3\n"
      "  }]\n"
      "}\n");
}

TEST(TomlSpellings, ATrailingCommaFollowsAPairAndNeverNothing) {
  const std::string document = "t = { a = 1, b = 2 }\nempty = {}\n"
                               "arr = [1, 2]\n";
  const std::string out =
      spelt(document, GTEXT_TOML_SPELL_1_1_0_INLINE_TRAILING_COMMA);
  // An empty table has no last pair for a comma to follow, and `{,}` is not a
  // table at either version. An array's trailing comma is legal at v1.0.0
  // already, so it is not this bit's business and none is emitted.
  EXPECT_EQ(out, "t = { a = 1, b = 2, }\nempty = {}\narr = [1, 2]\n");
  EXPECT_EQ(reads_as(out), std::string("1.1.0 only"));
  EXPECT_EQ(reads_as("empty = {}\narr = [1, 2]\n"),
      std::string("1.0.0 and 1.1.0"));
}

TEST(TomlSpellings, TheTwoInlineBitsAreIndependent) {
  const std::string document = "t = { a = 1, b = 2 }\n";
  const unsigned both = GTEXT_TOML_SPELL_1_1_0_INLINE_NEWLINES
      | GTEXT_TOML_SPELL_1_1_0_INLINE_TRAILING_COMMA;
  EXPECT_EQ(spelt(document, both),
      "t = {\n"
      "  a = 1,\n"
      "  b = 2,\n"
      "}\n");
  EXPECT_EQ(spelt(document, GTEXT_TOML_SPELL_1_1_0_INLINE_NEWLINES),
      "t = {\n"
      "  a = 1,\n"
      "  b = 2\n"
      "}\n");
  EXPECT_EQ(spelt(document, GTEXT_TOML_SPELL_1_1_0_INLINE_TRAILING_COMMA),
      "t = { a = 1, b = 2, }\n");
}

TEST(TomlSpellings, AllIsTheFiveBitsAndAnUnknownBitIsIgnored) {
  EXPECT_EQ((unsigned) GTEXT_TOML_SPELL_1_1_0_ALL,
      (unsigned) (GTEXT_TOML_SPELL_1_1_0_ESCAPE_E
          | GTEXT_TOML_SPELL_1_1_0_ESCAPE_X
          | GTEXT_TOML_SPELL_1_1_0_TIME_NO_SECONDS
          | GTEXT_TOML_SPELL_1_1_0_INLINE_NEWLINES
          | GTEXT_TOML_SPELL_1_1_0_INLINE_TRAILING_COMMA));

  // A bit this build does not know is ignored rather than refused, for the
  // reason an unrecognised `version` reads as the strict arm: a field filled in
  // from a configuration file must not be able to reach a grammar nobody chose.
  const std::string document = "t = { a = 1 }\n";
  EXPECT_EQ(spelt(document, 1u << 20), std::string("t = { a = 1 }\n"));
  EXPECT_EQ(spelt(document, (1u << 20) | GTEXT_TOML_SPELL_1_1_0_ALL),
      spelt(document, GTEXT_TOML_SPELL_1_1_0_ALL));
}

TEST(TomlSpellings, EveryBitTogetherStillMeansTheSameDocument) {
  const std::string document =
      "s = \"\\u001B\\u0001\"\n"
      "lt = 07:32:00\n"
      "t = { a = 1, b = { c = 2 } }\n"
      "arr = [1, 2]\n";
  const std::string out = spelt(document, GTEXT_TOML_SPELL_1_1_0_ALL);
  EXPECT_EQ(out,
      "s = \"\\e\\x01\"\n"
      "lt = 07:32\n"
      "t = {\n"
      "  a = 1,\n"
      "  b = {\n"
      "    c = 2,\n"
      "  },\n"
      "}\n"
      "arr = [1, 2]\n");
  EXPECT_EQ(reads_as(out), std::string("1.1.0 only"));

  GTEXT_TOML_Value * before = ok_1_1(document);
  GTEXT_TOML_Value * after = ok_1_1(out);
  ASSERT_NE(before, nullptr);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(string_of(key(before, "s")), string_of(key(after, "s")));
  EXPECT_EQ(datetime_of(key(before, "lt")), datetime_of(key(after, "lt")));
  const GTEXT_TOML_Value * tb = key(before, "t");
  const GTEXT_TOML_Value * ta = key(after, "t");
  ASSERT_NE(tb, nullptr);
  ASSERT_NE(ta, nullptr);
  EXPECT_EQ(gtext_toml_table_size(tb), gtext_toml_table_size(ta));
  gtext_toml_free(before);
  gtext_toml_free(after);
}
