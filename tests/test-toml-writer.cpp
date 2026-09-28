/**
 * @file
 *
 * The TOML writer: the contract toml-test cannot see.
 *
 * toml-test measures the writer in two directions and both score 100% - the
 * whole corpus written back out and read again, and the corpus's tagged JSON
 * turned into TOML and handed to `tomllib`. Six deliberate writer defects were
 * planted and every one of them moved that score.
 *
 * The seventh did not, and that is what this file is for. Forcing every table
 * to a `[header]` when the caller asked for inline produces a document with
 * the same *value*, so a comparison by value - which is the only comparison
 * the corpus's tagged JSON supports - cannot see the difference. A
 * GTEXT_TOML_Table_Style that never reached its decision scored 2250 of 2250.
 * So the style option, the key and string spellings, and the float spellings
 * are pinned here as **text**, which is the only instrument that separates two
 * documents with one value.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <gtest/gtest.h>

#include <ghoti.io/text/toml.h>

namespace {

GTEXT_TOML_Value * ok(const std::string & text, size_t max_depth = 256) {
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
  opts.max_depth = max_depth;
  GTEXT_TOML_Value * root =
      gtext_toml_parse(text.data(), text.size(), &opts, &err);
  EXPECT_NE(root, nullptr) << "[" << text << "]: "
                           << (err.message ? err.message : "no message");
  gtext_toml_error_free(&err);
  return root;
}

// Write a tree, requiring success, and return the bytes.
std::string write_ok(const GTEXT_TOML_Value * root,
    const GTEXT_TOML_Write_Options * opts = nullptr) {
  GTEXT_TOML_Sink sink;
  EXPECT_EQ(gtext_toml_sink_buffer(&sink), GTEXT_TOML_OK);
  GTEXT_TOML_Status status = gtext_toml_write(root, &sink, opts);
  EXPECT_EQ(status, GTEXT_TOML_OK);
  std::string out(gtext_toml_sink_buffer_data(&sink),
      gtext_toml_sink_buffer_size(&sink));
  gtext_toml_sink_buffer_free(&sink);
  return out;
}

// Write a tree, requiring failure, and return the status.
GTEXT_TOML_Status write_refused(const GTEXT_TOML_Value * root,
    const GTEXT_TOML_Write_Options * opts = nullptr) {
  GTEXT_TOML_Sink sink;
  EXPECT_EQ(gtext_toml_sink_buffer(&sink), GTEXT_TOML_OK);
  GTEXT_TOML_Status status = gtext_toml_write(root, &sink, opts);
  EXPECT_NE(status, GTEXT_TOML_OK);
  gtext_toml_sink_buffer_free(&sink);
  return status;
}

// Parse then write, which is what the conformance runner's roundtrip mode does.
std::string reshape(
    const std::string & text, GTEXT_TOML_Table_Style style) {
  GTEXT_TOML_Value * root = ok(text);
  GTEXT_TOML_Write_Options opts = gtext_toml_write_options_default();
  opts.table_style = style;
  std::string out = write_ok(root, &opts);
  gtext_toml_free(root);
  return out;
}

// The text a one-key document writes, for the spelling tests.
std::string one(const std::string & document) {
  GTEXT_TOML_Value * root = ok(document);
  std::string out = write_ok(root);
  gtext_toml_free(root);
  return out;
}

} // namespace

/*==========================================================================*
 * The table style, which no corpus comparison can see
 *==========================================================================*/

TEST(TomlWriter, TableStyleReachesTheDecision) {
  // One document with all three shapes in it: a header table, an inline table,
  // and an array of tables.
  const std::string source =
      "top = 1\n"
      "inline = { x = 1 }\n"
      "[header]\n"
      "y = 2\n"
      "[[rows]]\n"
      "z = 3\n"
      "[[rows]]\n"
      "z = 4\n";

  // AS_READ gives each of the three back the way it arrived.
  EXPECT_EQ(reshape(source, GTEXT_TOML_TABLE_STYLE_AS_READ),
      "top = 1\n"
      "inline = { x = 1 }\n"
      "\n[header]\n"
      "y = 2\n"
      "\n[[rows]]\n"
      "z = 3\n"
      "\n[[rows]]\n"
      "z = 4\n");

  // HEADERS turns the inline table into a header too.
  EXPECT_EQ(reshape(source, GTEXT_TOML_TABLE_STYLE_HEADERS),
      "top = 1\n"
      "\n[inline]\n"
      "x = 1\n"
      "\n[header]\n"
      "y = 2\n"
      "\n[[rows]]\n"
      "z = 3\n"
      "\n[[rows]]\n"
      "z = 4\n");

  // INLINE writes no header at all, and the array of tables becomes an array
  // of inline tables.
  EXPECT_EQ(reshape(source, GTEXT_TOML_TABLE_STYLE_INLINE),
      "top = 1\n"
      "inline = { x = 1 }\n"
      "header = { y = 2 }\n"
      "rows = [{ z = 3 }, { z = 4 }]\n");
}

TEST(TomlWriter, EveryStyleIsStillReadableAsTheSameValue) {
  // The property the corpus checks, asserted here too, so that the text
  // assertions above cannot be "fixed" by breaking the value.
  const std::string source = "[a.b]\nc = [1, { d = 2 }]\n";
  for (GTEXT_TOML_Table_Style style :
      {GTEXT_TOML_TABLE_STYLE_AS_READ, GTEXT_TOML_TABLE_STYLE_HEADERS,
          GTEXT_TOML_TABLE_STYLE_INLINE}) {
    std::string written = reshape(source, style);
    GTEXT_TOML_Value * again = ok(written);
    const GTEXT_TOML_Value * a = gtext_toml_table_get(again, "a", 1);
    ASSERT_NE(a, nullptr) << written;
    const GTEXT_TOML_Value * b = gtext_toml_table_get(a, "b", 1);
    ASSERT_NE(b, nullptr) << written;
    const GTEXT_TOML_Value * c = gtext_toml_table_get(b, "c", 1);
    ASSERT_NE(c, nullptr) << written;
    EXPECT_EQ(gtext_toml_array_size(c), 2u);
    gtext_toml_free(again);
  }
}

TEST(TomlWriter, EmptyArrayOfTablesFallsBackToBrackets) {
  // Zero `[[a]]` headers would not say that `a` exists, so an empty array is
  // written `a = []` even when headers were asked for. The corpus catches this
  // one, but only in the two modes that force headers.
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Value * array = gtext_toml_new_array(nullptr);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(gtext_toml_value_set_inline(array, false), GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1, array), GTEXT_TOML_OK);
  GTEXT_TOML_Write_Options opts = gtext_toml_write_options_default();
  opts.table_style = GTEXT_TOML_TABLE_STYLE_HEADERS;
  EXPECT_EQ(write_ok(root, &opts), "a = []\n");
  gtext_toml_free(root);
}

TEST(TomlWriter, PlainKeysComeBeforeSubTables) {
  // A blank line separates sections, and there is none before the first thing
  // written - which is what makes a document of nothing but headers start at
  // `[a]` rather than at an empty line.
  // Every bare key after a `[header]` belongs to the table that header opened,
  // so a sub-table defined before a plain key has to move. The value is the
  // same either way, which is why the corpus caught this and the text is here.
  EXPECT_EQ(reshape("[a]\nx = 1\n[b]\ny = 2\n",
                GTEXT_TOML_TABLE_STYLE_AS_READ),
      "[a]\nx = 1\n\n[b]\ny = 2\n");
  // `sub` is defined first in the source and written second.
  EXPECT_EQ(one("[t]\n[t.sub]\nq = 1\n"), "[t]\n\n[t.sub]\nq = 1\n");
  EXPECT_EQ(one("[t.sub]\nq = 1\n[t]\np = 2\n"),
      "[t]\np = 2\n\n[t.sub]\nq = 1\n");
}

TEST(TomlWriter, NestedHeaderPaths) {
  EXPECT_EQ(one("[a.b.c]\nd = 1\n"), "[a]\n\n[a.b]\n\n[a.b.c]\nd = 1\n");
  // A sub-table of an array-of-tables element takes the element's path.
  EXPECT_EQ(one("[[a]]\n[a.b]\nc = 1\n"), "[[a]]\n\n[a.b]\nc = 1\n");
}

TEST(TomlWriter, EmptyDocumentWritesNothing) {
  GTEXT_TOML_Value * root = ok("");
  EXPECT_EQ(write_ok(root), "");
  gtext_toml_free(root);
  // And an empty table still gets its header, or the key would vanish.
  EXPECT_EQ(one("[a]\n"), "[a]\n");
  EXPECT_EQ(one("a = {}\n"), "a = {}\n");
  EXPECT_EQ(one("a = []\n"), "a = []\n");
}

/*==========================================================================*
 * Spellings
 *==========================================================================*/

TEST(TomlWriter, KeysAreBareWhereTomlAllowsIt) {
  EXPECT_EQ(one("abc_DEF-012 = 1\n"), "abc_DEF-012 = 1\n");
  // Every other key is quoted, and a quoted key is escaped like a string.
  EXPECT_EQ(one("\"\" = 1\n"), "\"\" = 1\n");
  EXPECT_EQ(one("'a.b' = 1\n"), "\"a.b\" = 1\n");
  EXPECT_EQ(one("'a b' = 1\n"), "\"a b\" = 1\n");
  EXPECT_EQ(one("\"\\u00e9\" = 1\n"), "\"\xc3\xa9\" = 1\n");
  // A dotted key is two keys, and comes back as a header path.
  EXPECT_EQ(one("a.b = 1\n"), "[a]\nb = 1\n");
  // A key that needs quoting inside a header path is quoted there too.
  EXPECT_EQ(one("'a.b'.c = 1\n"), "[\"a.b\"]\nc = 1\n");
}

TEST(TomlWriter, KeyWithANulIsWrittenAsAnEscape) {
  // U+0000 has an escape and so may appear in a quoted key. The writer has to
  // spell it, and the key's length rather than a NUL terminator has to be what
  // it reads.
  EXPECT_EQ(one("\"a\\u0000b\" = 1\n"), "\"a\\u0000b\" = 1\n");
  GTEXT_TOML_Value * again = ok(one("\"a\\u0000b\" = 1\n"));
  EXPECT_NE(gtext_toml_table_get(again, "a\0b", 3), nullptr);
  gtext_toml_free(again);
}

TEST(TomlWriter, StringsAreBasicStringsWithEscapes) {
  EXPECT_EQ(one("a = 'plain'\n"), "a = \"plain\"\n");
  EXPECT_EQ(one("a = \"q\\\"b\\\\c\"\n"), "a = \"q\\\"b\\\\c\"\n");
  EXPECT_EQ(one("a = \"\\b\\t\\n\\f\\r\"\n"), "a = \"\\b\\t\\n\\f\\r\"\n");
  // The controls with no short escape, and U+007F, which v1.0.0 lists among
  // the characters a basic string may not hold unescaped.
  EXPECT_EQ(one("a = \"\\u0000\\u001f\\u007f\"\n"),
      "a = \"\\u0000\\u001F\\u007F\"\n");
  // A multi-line string is not reproduced as one: the form a string was
  // written in is not retained, and a basic string can always spell it.
  EXPECT_EQ(one("a = \"\"\"x\ny\"\"\"\n"), "a = \"x\\ny\"\n");
  // Non-ASCII passes through as the UTF-8 it already is.
  EXPECT_EQ(one("a = \"\xe6\x97\xa5\"\n"), "a = \"\xe6\x97\xa5\"\n");
}

TEST(TomlWriter, FloatsKeepTheirTypeAndRoundTrip) {
  // `%g` spells 1.0 as `1`, which TOML reads back as an integer.
  EXPECT_EQ(one("a = 1.0\n"), "a = 1.0\n");
  EXPECT_EQ(one("a = 0e0\n"), "a = 0.0\n");
  // The shortest spelling that reads back as the same double, not %.17g.
  EXPECT_EQ(one("a = 0.1\n"), "a = 0.1\n");
  EXPECT_EQ(one("a = 3.141592653589793\n"), "a = 3.141592653589793\n");
  // The sign of a zero is part of the value; the corpus distinguishes them.
  EXPECT_EQ(one("a = -0.0\n"), "a = -0.0\n");
  EXPECT_EQ(one("a = +0.0\n"), "a = 0.0\n");
  // An exponent is a float spelling TOML accepts, so a large value needs no
  // fraction appended.
  EXPECT_EQ(one("a = 1e300\n"), "a = 1e+300\n");
  EXPECT_EQ(one("a = inf\n"), "a = inf\n");
  EXPECT_EQ(one("a = -inf\n"), "a = -inf\n");
  EXPECT_EQ(one("a = nan\n"), "a = nan\n");
  EXPECT_EQ(one("a = -nan\n"), "a = -nan\n");
}

TEST(TomlWriter, IntegersAtTheBounds) {
  EXPECT_EQ(one("a = 9223372036854775807\n"), "a = 9223372036854775807\n");
  EXPECT_EQ(one("a = -9223372036854775808\n"), "a = -9223372036854775808\n");
  // Every other base is written in decimal: the base a number was written in
  // is not retained, and 0x is not a different value.
  EXPECT_EQ(one("a = 0xff\n"), "a = 255\n");
  EXPECT_EQ(one("a = 1_000\n"), "a = 1000\n");
}

TEST(TomlWriter, TheFourDateTimeKinds) {
  EXPECT_EQ(one("a = 1979-05-27T07:32:00Z\n"), "a = 1979-05-27T07:32:00Z\n");
  EXPECT_EQ(one("a = 1979-05-27T07:32:00-08:00\n"),
      "a = 1979-05-27T07:32:00-08:00\n");
  EXPECT_EQ(one("a = 1979-05-27T07:32:00\n"), "a = 1979-05-27T07:32:00\n");
  EXPECT_EQ(one("a = 1979-05-27\n"), "a = 1979-05-27\n");
  EXPECT_EQ(one("a = 07:32:00\n"), "a = 07:32:00\n");
  EXPECT_EQ(one("a = 07:32:00.999999\n"), "a = 07:32:00.999999\n");
  // An unknown offset keeps the only spelling that carries the meaning. No
  // valid case in the corpus has one, and tomllib cannot represent it, so
  // nothing outside this file checks it.
  EXPECT_EQ(one("a = 1979-05-27T07:32:00-00:00\n"),
      "a = 1979-05-27T07:32:00-00:00\n");
}

TEST(TomlWriter, DateTimeOptionsReachChron) {
  GTEXT_TOML_Value * root = ok("a = 1979-05-27T07:32:00Z\n");
  GCHRON_WriteOptions dt;
  gchron_write_options_default(&dt);
  dt.space_separator = true;
  dt.zero_offset_as_numeric = true;
  GTEXT_TOML_Write_Options opts = gtext_toml_write_options_default();
  opts.datetime = &dt;
  // v1.0.0, Offset Date-Time: a space may separate the date and the time.
  EXPECT_EQ(write_ok(root, &opts), "a = 1979-05-27 07:32:00+00:00\n");
  gtext_toml_free(root);
}

/* The writer's decimal point is the format's and not the user's, which is
   asserted in tests/test-locale-numbers.cpp and not here: that file generates
   a comma locale with localedef rather than hoping one is installed, and
   *fails* when it cannot. A version of this test lived here and used
   setlocale(LC_NUMERIC, "de_DE.UTF-8") directly, which returns NULL on this
   machine - so it ran in the C locale, measured nothing, and passed. */

TEST(TomlWriter, InlineSpacing) {
  EXPECT_EQ(one("a = [1, 2, 3]\n"), "a = [1, 2, 3]\n");
  EXPECT_EQ(one("a = { b = 1, c = 2 }\n"), "a = { b = 1, c = 2 }\n");
  EXPECT_EQ(one("a = [{ b = 1 }]\n"), "a = [{ b = 1 }]\n");
  EXPECT_EQ(one("a = [[1], [2]]\n"), "a = [[1], [2]]\n");
  // v1.0.0 arrays are heterogeneous, so a table beside a scalar is legal and
  // forces the whole array inline.
  EXPECT_EQ(one("a = [1, { b = 2 }]\n"), "a = [1, { b = 2 }]\n");
}

/*==========================================================================*
 * Refusals
 *==========================================================================*/

TEST(TomlWriter, RefusesWhatItCouldNotReadBack) {
  // Invalid UTF-8 in a string. No corpus case can reach this: the corpus's
  // documents are all valid UTF-8 by the time they parse, so the only way to
  // put a bad sequence in a tree is to build one.
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Value * bad = gtext_toml_new_string(nullptr, "\xc3", 1);
  ASSERT_NE(bad, nullptr);
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1, bad), GTEXT_TOML_OK);
  EXPECT_EQ(write_refused(root), GTEXT_TOML_E_BAD_UNICODE);
  gtext_toml_free(root);

  // And in a key.
  GTEXT_TOML_Value * root2 = gtext_toml_new_table(nullptr);
  ASSERT_NE(root2, nullptr);
  GTEXT_TOML_Value * one_value = gtext_toml_new_integer(nullptr, 1);
  ASSERT_NE(one_value, nullptr);
  ASSERT_EQ(gtext_toml_table_set(root2, "\xed\xa0\x80", 3, one_value),
      GTEXT_TOML_OK);
  EXPECT_EQ(write_refused(root2), GTEXT_TOML_E_BAD_UNICODE);
  gtext_toml_free(root2);
}

TEST(TomlWriter, RefusesADateTimeChronWillNotSpell) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GCHRON_TomlValue dt;
  std::memset(&dt, 0, sizeof(dt));
  dt.kind = GCHRON_TOML_LOCAL_DATE;
  dt.civil.date.year = 1979;
  dt.civil.date.month = 13; // 1..12
  dt.civil.date.day = 1;
  GTEXT_TOML_Value * value = gtext_toml_new_datetime(nullptr, &dt);
  ASSERT_NE(value, nullptr);
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1, value), GTEXT_TOML_OK);
  EXPECT_EQ(write_refused(root), GTEXT_TOML_E_DATETIME);
  gtext_toml_free(root);
}

TEST(TomlWriter, RefusesWhatIsNotADocument) {
  GTEXT_TOML_Sink sink;
  ASSERT_EQ(gtext_toml_sink_buffer(&sink), GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_write(nullptr, &sink, nullptr), GTEXT_TOML_E_INVALID);
  GTEXT_TOML_Value * scalar = gtext_toml_new_integer(nullptr, 1);
  ASSERT_NE(scalar, nullptr);
  // v1.0.0, Table: a TOML document is a table.
  EXPECT_EQ(gtext_toml_write(scalar, &sink, nullptr), GTEXT_TOML_E_INVALID);
  gtext_toml_free(scalar);
  gtext_toml_sink_buffer_free(&sink);

  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(gtext_toml_write(root, nullptr, nullptr), GTEXT_TOML_E_INVALID);
  GTEXT_TOML_Sink empty;
  std::memset(&empty, 0, sizeof(empty));
  EXPECT_EQ(gtext_toml_write(root, &empty, nullptr), GTEXT_TOML_E_INVALID);
  gtext_toml_free(root);
}

namespace {

GTEXT_TOML_Status refuse_everything(void *, const char *, size_t) {
  return GTEXT_TOML_E_OOM;
}

int counted_calls = 0;

GTEXT_TOML_Status refuse_after_two(void *, const char *, size_t) {
  return ++counted_calls > 2 ? GTEXT_TOML_E_WRITE : GTEXT_TOML_OK;
}

} // namespace

TEST(TomlWriter, ASinksOwnStatusComesBackUnchanged) {
  // A sink that reports out of memory must not have that turned into
  // E_WRITE: the two are the distinction a caller retrying on a full
  // destination needs, and an int-returning callback would have lost it.
  GTEXT_TOML_Value * root = ok("a = 1\nb = 2\n");
  GTEXT_TOML_Sink sink;
  sink.write = refuse_everything;
  sink.user = nullptr;
  EXPECT_EQ(gtext_toml_write(root, &sink, nullptr), GTEXT_TOML_E_OOM);

  counted_calls = 0;
  sink.write = refuse_after_two;
  EXPECT_EQ(gtext_toml_write(root, &sink, nullptr), GTEXT_TOML_E_WRITE);
  gtext_toml_free(root);
}

/*==========================================================================*
 * Sinks
 *==========================================================================*/

TEST(TomlWriter, FixedBufferReportsTruncationRatherThanShortening) {
  GTEXT_TOML_Value * root = ok("a = 1\n");
  std::string want = "a = 1\n";

  char exact[16];
  GTEXT_TOML_Sink sink;
  ASSERT_EQ(gtext_toml_sink_fixed_buffer(&sink, exact, want.size()),
      GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_write(root, &sink, nullptr), GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_sink_fixed_buffer_used(&sink), want.size());
  EXPECT_FALSE(gtext_toml_sink_fixed_buffer_truncated(&sink));
  EXPECT_EQ(std::string(exact, want.size()), want);
  gtext_toml_sink_fixed_buffer_free(&sink);

  // One byte short. The count alone cannot tell this from the exact fit above,
  // which is why the flag exists and why the write fails rather than returning
  // a shorter document.
  ASSERT_EQ(gtext_toml_sink_fixed_buffer(&sink, exact, want.size() - 1),
      GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_write(root, &sink, nullptr), GTEXT_TOML_E_WRITE);
  EXPECT_TRUE(gtext_toml_sink_fixed_buffer_truncated(&sink));
  EXPECT_EQ(gtext_toml_sink_fixed_buffer_used(&sink), want.size() - 1);
  gtext_toml_sink_fixed_buffer_free(&sink);

  gtext_toml_free(root);
}

TEST(TomlWriter, SinkAccessorsAnswerOnlyForTheirOwnKind) {
  GTEXT_TOML_Sink grow;
  ASSERT_EQ(gtext_toml_sink_buffer(&grow), GTEXT_TOML_OK);
  char room[8];
  GTEXT_TOML_Sink fixed;
  ASSERT_EQ(gtext_toml_sink_fixed_buffer(&fixed, room, sizeof(room)),
      GTEXT_TOML_OK);

  // Nothing written yet: the empty string rather than NULL, so that an empty
  // document needs no special case at the call site.
  EXPECT_STREQ(gtext_toml_sink_buffer_data(&grow), "");
  EXPECT_EQ(gtext_toml_sink_buffer_size(&grow), 0u);

  // Crossed over, and on a sink the caller built themselves.
  EXPECT_EQ(gtext_toml_sink_buffer_data(&fixed), nullptr);
  EXPECT_EQ(gtext_toml_sink_buffer_size(&fixed), 0u);
  EXPECT_EQ(gtext_toml_sink_fixed_buffer_used(&grow), 0u);
  EXPECT_FALSE(gtext_toml_sink_fixed_buffer_truncated(&grow));
  GTEXT_TOML_Sink mine;
  mine.write = refuse_everything;
  mine.user = nullptr;
  EXPECT_EQ(gtext_toml_sink_buffer_data(&mine), nullptr);
  EXPECT_FALSE(gtext_toml_sink_fixed_buffer_truncated(&mine));

  // Freeing the wrong kind leaves the sink alone rather than freeing the
  // caller's buffer or the other sink's state.
  gtext_toml_sink_fixed_buffer_free(&grow);
  EXPECT_NE(grow.write, nullptr);
  gtext_toml_sink_buffer_free(&fixed);
  EXPECT_NE(fixed.write, nullptr);

  gtext_toml_sink_buffer_free(&grow);
  gtext_toml_sink_buffer_free(&grow); // twice is safe
  gtext_toml_sink_fixed_buffer_free(&fixed);
  gtext_toml_sink_fixed_buffer_free(&fixed);

  EXPECT_EQ(gtext_toml_sink_buffer(nullptr), GTEXT_TOML_E_INVALID);
  EXPECT_EQ(gtext_toml_sink_fixed_buffer(&fixed, nullptr, 8),
      GTEXT_TOML_E_INVALID);
  EXPECT_EQ(gtext_toml_sink_fixed_buffer(&fixed, room, 0),
      GTEXT_TOML_E_INVALID);
  gtext_toml_sink_buffer_free(nullptr);
  gtext_toml_sink_fixed_buffer_free(nullptr);
}

/*==========================================================================*
 * Building a document
 *==========================================================================*/

TEST(TomlBuild, EveryTypeCanBeBuiltAndWritten) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GCHRON_TomlValue dt;
  std::memset(&dt, 0, sizeof(dt));
  dt.kind = GCHRON_TOML_LOCAL_DATE;
  dt.civil.date.year = 1979;
  dt.civil.date.month = 5;
  dt.civil.date.day = 27;

  GTEXT_TOML_Value * array = gtext_toml_new_array(nullptr);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(gtext_toml_array_append(array, gtext_toml_new_integer(nullptr, 1)),
      GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_array_append(array, gtext_toml_new_float(nullptr, 2.5)),
      GTEXT_TOML_OK);

  ASSERT_EQ(gtext_toml_table_set(root, "s", 1,
                gtext_toml_new_string(nullptr, "x", 1)),
      GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(root, "i", 1,
                gtext_toml_new_integer(nullptr, -7)),
      GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(root, "f", 1,
                gtext_toml_new_float(nullptr, 1.5)),
      GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(root, "b", 1,
                gtext_toml_new_boolean(nullptr, true)),
      GTEXT_TOML_OK);
  ASSERT_EQ(
      gtext_toml_table_set(root, "d", 1, gtext_toml_new_datetime(nullptr, &dt)),
      GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1, array), GTEXT_TOML_OK);
  GTEXT_TOML_Value * sub = gtext_toml_new_table(nullptr);
  ASSERT_NE(sub, nullptr);
  ASSERT_EQ(gtext_toml_table_set(sub, "k", 1,
                gtext_toml_new_integer(nullptr, 9)),
      GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(root, "t", 1, sub), GTEXT_TOML_OK);

  // Keys in the order they were added, and a built table counts as a header
  // table, which is what AS_READ then reproduces.
  EXPECT_EQ(write_ok(root),
      "s = \"x\"\n"
      "i = -7\n"
      "f = 1.5\n"
      "b = true\n"
      "d = 1979-05-27\n"
      "a = [1, 2.5]\n"
      "\n[t]\n"
      "k = 9\n");
  gtext_toml_free(root);
}

TEST(TomlBuild, EmptyAndNulKeys) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(gtext_toml_table_set(root, "", 0,
                gtext_toml_new_integer(nullptr, 1)),
      GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(root, "a\0b", 3,
                gtext_toml_new_integer(nullptr, 2)),
      GTEXT_TOML_OK);
  // Two keys, not one: the length is what identifies a key, so "a" and
  // "a\0b" are different even though strcmp cannot tell.
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1,
                gtext_toml_new_integer(nullptr, 3)),
      GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_table_size(root), 3u);
  EXPECT_EQ(write_ok(root), "\"\" = 1\n\"a\\u0000b\" = 2\na = 3\n");
  gtext_toml_free(root);
}

TEST(TomlBuild, RefusesADuplicateKey) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1,
                gtext_toml_new_integer(nullptr, 1)),
      GTEXT_TOML_OK);
  // Not a replacement: TOML says defining a key twice is invalid, and a
  // builder that replaced would let a caller write a document differing from
  // the one they described with nothing saying so.
  GTEXT_TOML_Value * second = gtext_toml_new_integer(nullptr, 2);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(gtext_toml_table_set(root, "a", 1, second), GTEXT_TOML_E_DUPKEY);
  // The caller still owns it on failure.
  gtext_toml_free(second);
  EXPECT_EQ(write_ok(root), "a = 1\n");
  gtext_toml_free(root);
}

TEST(TomlBuild, RefusesTheShapesThatCannotBeFreedOrWritten) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Value * mid = gtext_toml_new_table(nullptr);
  ASSERT_NE(mid, nullptr);
  ASSERT_EQ(gtext_toml_table_set(root, "mid", 3, mid), GTEXT_TOML_OK);
  GTEXT_TOML_Value * leaf = gtext_toml_new_table(nullptr);
  ASSERT_NE(leaf, nullptr);
  ASSERT_EQ(gtext_toml_table_set(mid, "leaf", 4, leaf), GTEXT_TOML_OK);

  // Already in a tree: storing it again is a double free at teardown.
  EXPECT_EQ(gtext_toml_table_set(root, "again", 5, leaf), GTEXT_TOML_E_STATE);
  // Itself: a one-node cycle.
  EXPECT_EQ(gtext_toml_table_set(leaf, "self", 4, leaf), GTEXT_TOML_E_STATE);
  // An ancestor: a longer cycle, and the case the parent walk is for.
  EXPECT_EQ(gtext_toml_table_set(leaf, "up", 2, root), GTEXT_TOML_E_STATE);
  EXPECT_EQ(gtext_toml_table_set(leaf, "up", 2, mid), GTEXT_TOML_E_STATE);

  GTEXT_TOML_Value * array = gtext_toml_new_array(nullptr);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(gtext_toml_table_set(root, "arr", 3, array), GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_array_append(array, array), GTEXT_TOML_E_STATE);
  EXPECT_EQ(gtext_toml_array_append(array, root), GTEXT_TOML_E_STATE);
  EXPECT_EQ(gtext_toml_array_append(array, leaf), GTEXT_TOML_E_STATE);

  // The tree is still the shape it was, and still frees.
  EXPECT_EQ(gtext_toml_table_size(root), 2u);
  gtext_toml_free(root);
}

TEST(TomlBuild, RefusesTwoAllocatorsInOneTree) {
  // gtext_toml_free() frees the whole tree through the root's allocator, so a
  // subtree from another one would be released through the wrong allocator.
  const GTEXT_Allocator * other = gtext_allocator_default();
  GTEXT_TOML_Value * root = gtext_toml_new_table(other);
  ASSERT_NE(root, nullptr);
  // NULL and gtext_allocator_default() are one allocator spelled two ways, and
  // must not be refused for being spelled differently.
  GTEXT_TOML_Value * child = gtext_toml_new_integer(nullptr, 1);
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(gtext_toml_table_set(root, "a", 1, child), GTEXT_TOML_OK);
  gtext_toml_free(root);
}

TEST(TomlBuild, SetInlineAppliesToContainersOnly) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Value * table = gtext_toml_new_table(nullptr);
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(gtext_toml_value_set_inline(table, true), GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(table, "x", 1,
                gtext_toml_new_integer(nullptr, 1)),
      GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(root, "t", 1, table), GTEXT_TOML_OK);
  EXPECT_EQ(write_ok(root), "t = { x = 1 }\n");
  // And back again.
  ASSERT_EQ(gtext_toml_value_set_inline(table, false), GTEXT_TOML_OK);
  EXPECT_EQ(write_ok(root), "[t]\nx = 1\n");

  GTEXT_TOML_Value * array = gtext_toml_new_array(nullptr);
  ASSERT_NE(array, nullptr);
  GTEXT_TOML_Value * element = gtext_toml_new_table(nullptr);
  ASSERT_NE(element, nullptr);
  ASSERT_EQ(gtext_toml_array_append(array, element), GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_value_set_inline(array, false), GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(root, "rows", 4, array), GTEXT_TOML_OK);
  EXPECT_EQ(write_ok(root), "[t]\nx = 1\n\n[[rows]]\n");

  GTEXT_TOML_Value * scalar = gtext_toml_new_integer(nullptr, 1);
  ASSERT_NE(scalar, nullptr);
  EXPECT_EQ(gtext_toml_value_set_inline(scalar, true), GTEXT_TOML_E_STATE);
  EXPECT_EQ(gtext_toml_value_set_inline(nullptr, true), GTEXT_TOML_E_INVALID);
  gtext_toml_free(scalar);
  gtext_toml_free(root);
}

TEST(TomlBuild, ConstructorsAndSettersRefuseNonsense) {
  EXPECT_EQ(gtext_toml_new_string(nullptr, nullptr, 4), nullptr);
  EXPECT_EQ(gtext_toml_new_datetime(nullptr, nullptr), nullptr);
  // An empty string is a string, and its accessor must not answer NULL.
  GTEXT_TOML_Value * empty = gtext_toml_new_string(nullptr, nullptr, 0);
  ASSERT_NE(empty, nullptr);
  size_t len = 99;
  EXPECT_NE(gtext_toml_value_string(empty, &len), nullptr);
  EXPECT_EQ(len, 0u);
  gtext_toml_free(empty);

  GTEXT_TOML_Value * scalar = gtext_toml_new_integer(nullptr, 1);
  ASSERT_NE(scalar, nullptr);
  GTEXT_TOML_Value * other = gtext_toml_new_integer(nullptr, 2);
  ASSERT_NE(other, nullptr);
  EXPECT_EQ(gtext_toml_table_set(scalar, "a", 1, other), GTEXT_TOML_E_INVALID);
  EXPECT_EQ(gtext_toml_array_append(scalar, other), GTEXT_TOML_E_INVALID);
  EXPECT_EQ(gtext_toml_table_set(nullptr, "a", 1, other), GTEXT_TOML_E_INVALID);
  EXPECT_EQ(gtext_toml_array_append(nullptr, other), GTEXT_TOML_E_INVALID);
  GTEXT_TOML_Value * table = gtext_toml_new_table(nullptr);
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(gtext_toml_table_set(table, nullptr, 0, other), GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_table_set(table, nullptr, 3, scalar),
      GTEXT_TOML_E_INVALID);
  EXPECT_EQ(gtext_toml_table_set(table, "a", 1, nullptr),
      GTEXT_TOML_E_INVALID);
  gtext_toml_free(table);
  gtext_toml_free(scalar);
}

/*==========================================================================*
 * Depth, and the file path
 *==========================================================================*/

TEST(TomlWriter, WritesADocumentAsDeepAsTheParserAccepts) {
  // The parser walks on the heap and so does the free; a writer that recursed
  // would crash on a tree this library handed the caller itself. 100,000 deep,
  // parsed with no depth limit, written, and read again.
  const size_t depth = 100000;
  std::string document = "a = ";
  document.append(depth, '[');
  document.append(depth, ']');
  document.push_back('\n');

  GTEXT_TOML_Value * root = ok(document, 0);
  ASSERT_NE(root, nullptr);
  std::string written = write_ok(root);
  gtext_toml_free(root);

  GTEXT_TOML_Value * again = ok(written, 0);
  ASSERT_NE(again, nullptr);
  const GTEXT_TOML_Value * node = gtext_toml_table_get(again, "a", 1);
  size_t counted = 0;
  while (node && gtext_toml_value_type(node) == GTEXT_TOML_ARRAY
      && gtext_toml_array_size(node) == 1) {
    node = gtext_toml_array_get(node, 0);
    ++counted;
  }
  EXPECT_EQ(counted, depth - 1);
  gtext_toml_free(again);
}

TEST(TomlWriter, WriteFileReplacesTheFileOrLeavesIt) {
  char path[] = "/tmp/gtext-toml-writer-XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(write(fd, "previous\n", 9), 9);
  close(fd);

  GTEXT_TOML_Value * root = ok("a = 1\n[b]\nc = 2\n");
  ASSERT_EQ(gtext_toml_write_file(root, path, nullptr), GTEXT_TOML_OK);
  GTEXT_TOML_Value * again = gtext_toml_parse_file(path, nullptr, nullptr);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(gtext_toml_table_size(again), 2u);
  gtext_toml_free(again);
  gtext_toml_free(root);

  // A document the writer refuses leaves the previous file whole, and reports
  // the writer's own code rather than a disk error.
  GTEXT_TOML_Value * bad = gtext_toml_new_table(nullptr);
  ASSERT_NE(bad, nullptr);
  ASSERT_EQ(gtext_toml_table_set(bad, "a", 1,
                gtext_toml_new_string(nullptr, "\xc3", 1)),
      GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_write_file(bad, path, nullptr),
      GTEXT_TOML_E_BAD_UNICODE);
  GTEXT_TOML_Value * survivor = gtext_toml_parse_file(path, nullptr, nullptr);
  ASSERT_NE(survivor, nullptr);
  EXPECT_EQ(gtext_toml_table_size(survivor), 2u);
  gtext_toml_free(survivor);
  gtext_toml_free(bad);

  EXPECT_EQ(gtext_toml_write_file(nullptr, path, nullptr),
      GTEXT_TOML_E_INVALID);
  GTEXT_TOML_Value * empty = gtext_toml_new_table(nullptr);
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(gtext_toml_write_file(empty, nullptr, nullptr),
      GTEXT_TOML_E_INVALID);
  EXPECT_EQ(gtext_toml_write_file(empty, "/nonexistent-dir/x.toml", nullptr),
      GTEXT_TOML_E_INVALID);
  gtext_toml_free(empty);

  unlink(path);
}

TEST(TomlWriter, WriteOptionsDefaults) {
  GTEXT_TOML_Write_Options opts = gtext_toml_write_options_default();
  EXPECT_EQ(opts.allocator, nullptr);
  EXPECT_EQ(opts.table_style, GTEXT_TOML_TABLE_STYLE_AS_READ);
  EXPECT_EQ(opts.datetime, nullptr);
}
