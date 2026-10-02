/**
 * @file
 *
 * Where a format has two writers, they must agree.
 *
 * JSON, CSV and YAML each offer a whole-value writer and an incremental one.
 * They are two implementations of one specification, and nothing in the suite
 * compared them until an object of two or more members written through
 * gtext_json_writer_* turned out to be **invalid JSON**:
 *
 *     {"a":1,"b":,2}
 *
 * every value type, every nesting, from a sequence of calls that each returned
 * GTEXT_JSON_OK with gtext_json_writer_finish() reporting OK as well.
 *
 * It survived because the one test covering that exact shape asserted that
 * three substrings appeared in the output, which is true of the malformed bytes
 * too. **So every test here asserts the whole output, or a reparse, or both** -
 * never that something is present somewhere in it.
 *
 * The CSV pair disagreed on two options the streaming writer never read:
 * `trailing_newline` and `trim_trailing_empty_fields`. Both are properties of a
 * record's position in the file, which a streaming writer cannot know when the
 * record closes, so it now defers.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>
#include <vector>
#include <gtest/gtest.h>

#include <ghoti.io/text/csv.h>
#include <ghoti.io/text/json.h>

namespace {

/** One string value through the incremental writer, for comparison. */
std::string json_write_incremental_string(
    const GTEXT_JSON_Write_Options & opts, const char * utf8) {
  GTEXT_JSON_Sink sink;
  if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK)
    return std::string();
  GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, &opts);
  if (!w) {
    gtext_json_sink_buffer_free(&sink);
    return std::string();
  }
  std::string out;
  if (gtext_json_writer_string(w, utf8, std::strlen(utf8)) == GTEXT_JSON_OK &&
      gtext_json_writer_finish(w, nullptr) == GTEXT_JSON_OK) {
    out.assign(
        gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
  }
  gtext_json_writer_free(w);
  gtext_json_sink_buffer_free(&sink);
  return out;
}

std::string json_write_value_with(
    const GTEXT_JSON_Write_Options & opts, const GTEXT_JSON_Value * v) {
  GTEXT_JSON_Sink sink;
  EXPECT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_write_value(&sink, &opts, v, nullptr), GTEXT_JSON_OK);
  std::string out(
      gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
  gtext_json_sink_buffer_free(&sink);
  return out;
}

/** The same document the incremental writer builds below, as text. */
const char * kJsonDoc = "{\"b\":\"a/\\u00e9\",\"a\":[1,2],\"n\":1.50}";

/** That document through the incremental API, under @p opts. */
std::string json_write_incremental_with(const GTEXT_JSON_Write_Options & opts) {
  GTEXT_JSON_Sink sink;
  EXPECT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
  GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, &opts);
  EXPECT_NE(w, nullptr);
  EXPECT_EQ(gtext_json_writer_object_begin(w), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_writer_key(w, "b", 1), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_writer_string(w, "a/\xc3\xa9", 4), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_writer_key(w, "a", 1), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_writer_array_begin(w), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_writer_number_i64(w, 1), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_writer_number_i64(w, 2), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_writer_array_end(w), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_writer_key(w, "n", 1), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_writer_number_lexeme(w, "1.50", 4), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_writer_object_end(w), GTEXT_JSON_OK);
  EXPECT_EQ(gtext_json_writer_finish(w, nullptr), GTEXT_JSON_OK);
  std::string out(
      gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
  gtext_json_writer_free(w);
  gtext_json_sink_buffer_free(&sink);
  return out;
}

} // namespace

TEST(JsonWriterAgreement, BothWritersAgreeOnEveryApplicableOption) {
  GTEXT_JSON_Value * v =
      gtext_json_parse(kJsonDoc, std::strlen(kJsonDoc), nullptr, nullptr);
  ASSERT_NE(v, nullptr);

  // Each entry sets one option away from the defaults. The four this API
  // cannot honour are in the test below, named and explained, rather than
  // silently left out of this list - a list that omits them is a list nobody
  // can tell from an incomplete one.
  struct Case {
    const char * name;
    void (*set)(GTEXT_JSON_Write_Options *);
  };
  const Case cases[] = {
      {"defaults", [](GTEXT_JSON_Write_Options *) {}},
      {"pretty", [](GTEXT_JSON_Write_Options * o) { o->pretty = true; }},
      {"indent_spaces",
          [](GTEXT_JSON_Write_Options * o) {
            o->pretty = true;
            o->indent_spaces = 4;
          }},
      {"trailing_newline",
          [](GTEXT_JSON_Write_Options * o) { o->trailing_newline = true; }},
      {"space_after_colon",
          [](GTEXT_JSON_Write_Options * o) { o->space_after_colon = true; }},
      {"space_after_comma",
          [](GTEXT_JSON_Write_Options * o) { o->space_after_comma = true; }},
      {"escape_solidus",
          [](GTEXT_JSON_Write_Options * o) { o->escape_solidus = true; }},
      {"escape_unicode",
          [](GTEXT_JSON_Write_Options * o) { o->escape_unicode = true; }},
      {"escape_all_non_ascii",
          [](GTEXT_JSON_Write_Options * o) { o->escape_all_non_ascii = true; }},
  };

  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    c.set(&opts);

    const std::string from_value = json_write_value_with(opts, v);
    const std::string from_incremental = json_write_incremental_with(opts);

    // Byte for byte, not "both contain the keys".
    EXPECT_EQ(from_value, from_incremental);
    // And both are JSON. The defect this file exists for produced output that
    // no parser accepts, so the equality above is not enough on its own: two
    // writers could agree on the same malformed bytes.
    for (const std::string * s : {&from_value, &from_incremental}) {
      GTEXT_JSON_Value * back =
          gtext_json_parse(s->data(), s->size(), nullptr, nullptr);
      EXPECT_NE(back, nullptr) << "not valid JSON: " << *s;
      if (back) gtext_json_free(back);
    }
  }

  gtext_json_free(v);
}

TEST(JsonWriterAgreement, TheFourOptionsTheIncrementalWriterCannotHonour) {
  // Asserted rather than omitted, so that the list in gtext_json_writer_new()'s
  // documentation is checked against the code. Each of these needs to see a
  // whole value before its first byte goes out, which is what the incremental
  // API gives up; if one of them ever becomes honourable, this test fails and
  // the documentation gets corrected with it.
  GTEXT_JSON_Value * v =
      gtext_json_parse(kJsonDoc, std::strlen(kJsonDoc), nullptr, nullptr);
  ASSERT_NE(v, nullptr);

  struct Case {
    const char * name;
    void (*set)(GTEXT_JSON_Write_Options *);
  };
  const Case cases[] = {
      {"sort_object_keys",
          [](GTEXT_JSON_Write_Options * o) { o->sort_object_keys = true; }},
      {"canonical_numbers",
          [](GTEXT_JSON_Write_Options * o) { o->canonical_numbers = true; }},
      {"inline_array_threshold",
          [](GTEXT_JSON_Write_Options * o) {
            o->pretty = true;
            o->inline_array_threshold = 8;
          }},
      {"inline_object_threshold",
          [](GTEXT_JSON_Write_Options * o) {
            o->pretty = true;
            o->inline_object_threshold = 8;
          }},
  };

  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    c.set(&opts);
    GTEXT_JSON_Write_Options plain = gtext_json_write_options_default();
    // pretty is not one of the four; it is set above only to make the two
    // threshold options observable, so the baseline has to match.
    plain.pretty = opts.pretty;

    // The incremental writer ignores the option: its output is what it would
    // have written without it.
    EXPECT_EQ(json_write_incremental_with(opts),
        json_write_incremental_with(plain));
    // And the value writer does not ignore it, which is what makes this a
    // statement about the incremental writer rather than about the option.
    EXPECT_NE(json_write_value_with(opts, v), json_write_value_with(plain, v))
        << "the option changed nothing even for the value writer, so this "
           "test no longer shows that the incremental writer ignores it";
    // Ignored, never malformed.
    const std::string out = json_write_incremental_with(opts);
    GTEXT_JSON_Value * back =
        gtext_json_parse(out.data(), out.size(), nullptr, nullptr);
    EXPECT_NE(back, nullptr) << "not valid JSON: " << out;
    if (back) gtext_json_free(back);
  }

  gtext_json_free(v);
}

TEST(JsonWriterAgreement, EveryObjectSizeAndValueKindIsValidJson) {
  // The defect was invisible at one member and present at two, so the sizes
  // either side of it are what this walks. Every value kind, because the
  // separator rule is shared by all of them.
  enum Kind { kNull, kBool, kI64, kU64, kDouble, kLexeme, kString, kArray,
      kObject, kKindCount };
  const char * kind_name[] = {"null", "bool", "i64", "u64", "double", "lexeme",
      "string", "array", "object"};

  for (int members = 0; members <= 4; members++) {
    for (int k = 0; k < kKindCount; k++) {
      SCOPED_TRACE(std::string("members=") + std::to_string(members)
          + " kind=" + kind_name[k]);
      GTEXT_JSON_Sink sink;
      ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
      GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
      ASSERT_NE(w, nullptr);
      ASSERT_EQ(gtext_json_writer_object_begin(w), GTEXT_JSON_OK);
      for (int i = 0; i < members; i++) {
        const std::string key = "k" + std::to_string(i);
        ASSERT_EQ(gtext_json_writer_key(w, key.data(), key.size()),
            GTEXT_JSON_OK);
        switch (k) {
        case kNull: ASSERT_EQ(gtext_json_writer_null(w), GTEXT_JSON_OK); break;
        case kBool:
          ASSERT_EQ(gtext_json_writer_bool(w, i % 2 == 0), GTEXT_JSON_OK);
          break;
        case kI64:
          ASSERT_EQ(gtext_json_writer_number_i64(w, i), GTEXT_JSON_OK);
          break;
        case kU64:
          ASSERT_EQ(gtext_json_writer_number_u64(w, (uint64_t)i),
              GTEXT_JSON_OK);
          break;
        case kDouble:
          ASSERT_EQ(gtext_json_writer_number_double(w, i + 0.5),
              GTEXT_JSON_OK);
          break;
        case kLexeme:
          ASSERT_EQ(gtext_json_writer_number_lexeme(w, "1.50", 4),
              GTEXT_JSON_OK);
          break;
        case kString:
          ASSERT_EQ(gtext_json_writer_string(w, "v", 1), GTEXT_JSON_OK);
          break;
        case kArray:
          ASSERT_EQ(gtext_json_writer_array_begin(w), GTEXT_JSON_OK);
          ASSERT_EQ(gtext_json_writer_number_i64(w, 1), GTEXT_JSON_OK);
          ASSERT_EQ(gtext_json_writer_array_end(w), GTEXT_JSON_OK);
          break;
        case kObject:
          ASSERT_EQ(gtext_json_writer_object_begin(w), GTEXT_JSON_OK);
          ASSERT_EQ(gtext_json_writer_key(w, "i", 1), GTEXT_JSON_OK);
          ASSERT_EQ(gtext_json_writer_number_i64(w, 1), GTEXT_JSON_OK);
          ASSERT_EQ(gtext_json_writer_object_end(w), GTEXT_JSON_OK);
          break;
        default: break;
        }
      }
      ASSERT_EQ(gtext_json_writer_object_end(w), GTEXT_JSON_OK);
      ASSERT_EQ(gtext_json_writer_finish(w, nullptr), GTEXT_JSON_OK);

      const std::string out(gtext_json_sink_buffer_data(&sink),
          gtext_json_sink_buffer_size(&sink));
      GTEXT_JSON_Value * back =
          gtext_json_parse(out.data(), out.size(), nullptr, nullptr);
      EXPECT_NE(back, nullptr) << "not valid JSON: " << out;
      if (back) {
        // The structure survived, not just the syntax: the right number of
        // members came back.
        EXPECT_EQ(gtext_json_typeof(back), GTEXT_JSON_OBJECT);
        EXPECT_EQ(gtext_json_object_size(back), (size_t)members) << out;
        gtext_json_free(back);
      }
      gtext_json_writer_free(w);
      gtext_json_sink_buffer_free(&sink);
    }
  }
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------

namespace {

using CsvRows = std::vector<std::vector<std::string>>;

/** @p rows through gtext_csv_write_table(), with the status it returned. */
std::string csv_from_table(const GTEXT_CSV_Write_Options & opts,
    const CsvRows & rows, GTEXT_CSV_Status * out_status) {
  GTEXT_CSV_Table * t = gtext_csv_new_table();
  EXPECT_NE(t, nullptr);
  for (const auto & row : rows) {
    std::vector<const char *> p;
    std::vector<size_t> l;
    for (const auto & f : row) {
      p.push_back(f.data());
      l.push_back(f.size());
    }
    EXPECT_EQ(
        gtext_csv_row_append(t, p.data(), l.data(), p.size(), nullptr),
        GTEXT_CSV_OK);
  }
  GTEXT_CSV_Sink sink;
  EXPECT_EQ(gtext_csv_sink_buffer(&sink), GTEXT_CSV_OK);
  *out_status = gtext_csv_write_table(&sink, &opts, t);
  std::string out(
      gtext_csv_sink_buffer_data(&sink), gtext_csv_sink_buffer_size(&sink));
  gtext_csv_sink_buffer_free(&sink);
  gtext_csv_free_table(t);
  return out;
}

/** The same rows through the streaming writer, stopping at the first error. */
std::string csv_from_stream(const GTEXT_CSV_Write_Options & opts,
    const CsvRows & rows, GTEXT_CSV_Status * out_status) {
  GTEXT_CSV_Sink sink;
  EXPECT_EQ(gtext_csv_sink_buffer(&sink), GTEXT_CSV_OK);
  GTEXT_CSV_Writer * w = gtext_csv_writer_new(&sink, &opts);
  EXPECT_NE(w, nullptr);
  GTEXT_CSV_Status st = GTEXT_CSV_OK;
  for (const auto & row : rows) {
    if (st != GTEXT_CSV_OK) break;
    st = gtext_csv_writer_record_begin(w);
    for (const auto & f : row) {
      if (st != GTEXT_CSV_OK) break;
      st = gtext_csv_writer_field(w, f.data(), f.size());
    }
    if (st == GTEXT_CSV_OK) st = gtext_csv_writer_record_end(w);
  }
  const GTEXT_CSV_Status fi = gtext_csv_writer_finish(w);
  *out_status = (st != GTEXT_CSV_OK) ? st : fi;
  std::string out(
      gtext_csv_sink_buffer_data(&sink), gtext_csv_sink_buffer_size(&sink));
  gtext_csv_writer_free(w);
  gtext_csv_sink_buffer_free(&sink);
  return out;
}

} // namespace

TEST(CsvWriterAgreement, BothWritersAgreeOverTheOptionAndShapeMatrix) {
  // 352 combinations. The two that disagreed:
  //
  //   trailing_newline=false            table "a,b"    streaming "a,b\n"
  //   trim_trailing_empty_fields=true   table "a"      streaming "a,\"\",\"\""
  //
  // Both are about a record's position in the file, which the streaming writer
  // cannot know when the record closes - so it defers the newline to the next
  // record_begin() or to finish(), and counts trailing empty fields instead of
  // writing them. The streaming writer had never read either option; the source
  // said the first was "typically handled by the caller".
  const std::vector<std::pair<const char *, CsvRows>> shapes = {
      {"no-rows", {}},
      {"single-field", {{"x"}}},
      {"one-row", {{"a", "b"}}},
      {"two-rows", {{"a", "b"}, {"c", "d"}}},
      {"three-rows", {{"a", "b"}, {"c", "d"}, {"e", "f"}}},
      {"trailing-empties", {{"a", "", ""}}},
      {"all-empty-row", {{"", ""}}},
      {"leading-empty", {{"", "a"}}},
      {"empty-in-the-middle", {{"a", "", "b"}}},
      {"empties-across-rows", {{"a", "", ""}, {"", "b", ""}, {"", "", ""}}},
      {"needs-quoting", {{"a,b", "c\"d", "e\nf"}}},
  };

  // A row whose field count differs from the table's is refused by
  // gtext_csv_row_append(), so a ragged shape would quietly give the two
  // writers different content. The first version of this comparison had one,
  // and read as 32 library disagreements that were one harness bug. Checked
  // here, once, rather than inside the comparison.
  for (const auto & [shape_name, rows] : shapes) {
    SCOPED_TRACE(shape_name);
    for (const auto & row : rows) {
      ASSERT_EQ(row.size(), rows.front().size())
          << "ragged shape: every row must have the same field count";
    }
  }

  size_t combinations = 0;
  for (const auto & [shape_name, rows] : shapes) {
    for (int tn = 0; tn < 2; tn++) {
      for (int trim = 0; trim < 2; trim++) {
        for (int qe = 0; qe < 2; qe++) {
          for (int q = 0; q < 4; q++) {
            GTEXT_CSV_Write_Options o = gtext_csv_write_options_default();
            o.trailing_newline = (tn != 0);
            o.trim_trailing_empty_fields = (trim != 0);
            o.quote_empty_fields = (qe != 0);
            o.quoting = (GTEXT_CSV_Quoting)q;
            SCOPED_TRACE(std::string(shape_name) + " trailing_newline="
                + std::to_string(tn) + " trim=" + std::to_string(trim)
                + " quote_empty=" + std::to_string(qe)
                + " quoting=" + std::to_string(q));

            GTEXT_CSV_Status ts = GTEXT_CSV_OK;
            GTEXT_CSV_Status ss = GTEXT_CSV_OK;
            const std::string from_table = csv_from_table(o, rows, &ts);
            const std::string from_stream = csv_from_stream(o, rows, &ss);

            EXPECT_EQ(from_table, from_stream);
            // The verdict as well as the bytes: one writer refusing where the
            // other accepts is a disagreement even when both emit nothing.
            // This is what caught the streaming writer emitting its deferred
            // newline after a record whose field had been refused.
            EXPECT_EQ(ts == GTEXT_CSV_OK, ss == GTEXT_CSV_OK)
                << "table=" << (int)ts << " stream=" << (int)ss;
            combinations++;
          }
        }
      }
    }
  }
  EXPECT_EQ(combinations, 352u) << "the matrix changed size";
}

TEST(CsvWriterAgreement, StreamingWriterHonoursTrailingNewline) {
  // Stated directly as well as through the matrix, because this is the option
  // whose absence was written into the source as a decision.
  for (bool trailing : {false, true}) {
    SCOPED_TRACE(trailing ? "trailing_newline=true" : "trailing_newline=false");
    GTEXT_CSV_Write_Options o = gtext_csv_write_options_default();
    o.trailing_newline = trailing;
    GTEXT_CSV_Status st = GTEXT_CSV_OK;
    const std::string out =
        csv_from_stream(o, {{"a", "b"}, {"c", "d"}}, &st);
    EXPECT_EQ(st, GTEXT_CSV_OK);
    // Records are always separated; only the last one's terminator is optional.
    EXPECT_EQ(out, trailing ? "a,b\nc,d\n" : "a,b\nc,d");
  }
}

TEST(CsvWriterAgreement, StreamingWriterHonoursTrimTrailingEmptyFields) {
  GTEXT_CSV_Write_Options o = gtext_csv_write_options_default();
  o.trim_trailing_empty_fields = true;
  GTEXT_CSV_Status st = GTEXT_CSV_OK;

  // Trailing empties dropped, an interior one kept: the option is about
  // position, so an empty field is held back until something follows it.
  EXPECT_EQ(csv_from_stream(o, {{"a", "", ""}}, &st), "a\n");
  EXPECT_EQ(st, GTEXT_CSV_OK);
  EXPECT_EQ(csv_from_stream(o, {{"a", "", "b"}}, &st), "a,\"\",b\n");
  EXPECT_EQ(st, GTEXT_CSV_OK);
  // A record of nothing but empty fields writes no field at all, which is what
  // the table writer does for the same row.
  EXPECT_EQ(csv_from_stream(o, {{"", ""}}, &st), "\n");
  EXPECT_EQ(st, GTEXT_CSV_OK);
}

/**
 * @test JsonWriterUnicode.EscapeUnicodeEscapesCodepointsNotBytes
 *
 * `escape_unicode` and `escape_all_non_ascii` used to escape each **byte** of
 * a UTF-8 sequence on its own, under a comment saying a more sophisticated
 * implementation would decode the UTF-8. So `é` (C3 A9) came out as
 * `\u00C3\u00A9` - which is valid JSON holding the two characters `Ã©`. Every
 * non-ASCII string was silently changed by either option, and the output
 * reparsed cleanly as the wrong value.
 *
 * Nothing caught it because no test in the suite had ever set either option on
 * a string that was not ASCII: a round trip over ASCII is a round trip these
 * options do not touch. So this test is written as the round trip over the
 * characters that distinguish the cases - two, three and four byte sequences -
 * and it asserts the bytes as well, because "it round-trips" is also true of
 * not escaping at all.
 */
TEST(JsonWriterUnicode, EscapeUnicodeEscapesCodepointsNotBytes) {
  struct Case {
    const char * utf8;    // the string's content
    const char * escaped; // what \uXXXX escaping must produce
    const char * what;
  };
  const Case cases[] = {
      {"\u00e9", "\"\\u00E9\"", "e-acute, two bytes"},
      {"\u20ac", "\"\\u20AC\"", "euro sign, three bytes"},
      {"\U0001f600", "\"\\uD83D\\uDE00\"", "emoji, four bytes, a pair"},
      {"a\u00e9b", "\"a\\u00E9b\"", "mixed with ASCII"},
      {"abc", "\"abc\"", "ASCII is untouched"},
      {"\u007f", "\"\\u007F\"", "DEL is ASCII but not printable"},
  };

  for (const Case & c : cases) {
    GTEXT_JSON_Value * v = gtext_json_new_string(c.utf8, std::strlen(c.utf8));
    ASSERT_NE(v, nullptr) << c.what;

    for (int which = 0; which < 2; which++) {
      GTEXT_JSON_Write_Options o = gtext_json_write_options_default();
      if (which == 0)
        o.escape_unicode = true;
      else
        o.escape_all_non_ascii = true;

      const std::string out = json_write_value_with(o, v);
      if (std::strcmp(c.utf8, "\u007f") != 0) {
        EXPECT_EQ(out, std::string(c.escaped))
            << c.what << " (option " << which << ")";
      }

      // And the value survives, which the byte-wise escaping did not do.
      GTEXT_JSON_Value * back =
          gtext_json_parse(out.data(), out.size(), nullptr, nullptr);
      ASSERT_NE(back, nullptr) << c.what << ": " << out;
      const char * got = nullptr;
      size_t got_len = 0;
      ASSERT_EQ(gtext_json_get_string(back, &got, &got_len), GTEXT_JSON_OK);
      EXPECT_EQ(std::string(got, got_len), std::string(c.utf8))
          << c.what << " (option " << which << "): wrote " << out;
      gtext_json_free(back);
    }

    // The two writers agree about it, which is the property this file is for.
    GTEXT_JSON_Write_Options o = gtext_json_write_options_default();
    o.escape_unicode = true;
    EXPECT_EQ(
        json_write_value_with(o, v), json_write_incremental_string(o, c.utf8))
        << c.what;
    gtext_json_free(v);
  }
}

/**
 * @test JsonWriterUnicode.AStringThatIsNotUtf8IsRefused
 *
 * Both writers used to emit invalid UTF-8 verbatim and report GTEXT_JSON_OK,
 * so the output was bytes this library's own parser refuses. A parse validates
 * UTF-8, so such a string can only reach a writer through the DOM builders or
 * gtext_json_writer_string() - which is exactly where it was never checked.
 *
 * Found by tests/fuzz/fuzz_json_writer.cpp. The agreement differential in this
 * file could not have found it: both writers share one escaper, so they were
 * wrong identically and agreed perfectly.
 */
TEST(JsonWriterUnicode, AStringThatIsNotUtf8IsRefused) {
  struct Case {
    std::string bytes;
    const char * what;
  };
  const Case cases[] = {
      {std::string("a\xff"
                   "b"),
          "0xff is never a UTF-8 byte"},
      {std::string("\xed\xa0\x80"), "a lone high surrogate, encoded"},
      {std::string("\xe2\x82"), "a truncated three-byte sequence"},
      {std::string("\xc0\x80"), "an overlong encoding of NUL"},
      {std::string("\x80"), "a stray continuation byte"},
      {std::string("\xf5\x80\x80\x80"), "past U+10FFFF"},
  };

  for (const Case & c : cases) {
    // The whole-value writer.
    GTEXT_JSON_Value * v =
        gtext_json_new_string(c.bytes.data(), c.bytes.size());
    ASSERT_NE(v, nullptr) << c.what;
    GTEXT_JSON_Sink sink;
    ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
    GTEXT_JSON_Write_Options o = gtext_json_write_options_default();
    GTEXT_JSON_Error err;
    std::memset(&err, 0, sizeof(err));
    EXPECT_EQ(
        gtext_json_write_value(&sink, &o, v, &err), GTEXT_JSON_E_BAD_UNICODE)
        << c.what;
    EXPECT_EQ(err.code, GTEXT_JSON_E_BAD_UNICODE) << c.what;
    gtext_json_error_free(&err);
    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(v);

    // The incremental writer, as a value and as a key.
    for (int as_key = 0; as_key < 2; as_key++) {
      GTEXT_JSON_Sink s2;
      ASSERT_EQ(gtext_json_sink_buffer(&s2), GTEXT_JSON_OK);
      GTEXT_JSON_Writer * w = gtext_json_writer_new(s2, &o);
      ASSERT_NE(w, nullptr);
      GTEXT_JSON_Status st;
      if (as_key) {
        ASSERT_EQ(gtext_json_writer_object_begin(w), GTEXT_JSON_OK);
        st = gtext_json_writer_key(w, c.bytes.data(), c.bytes.size());
      }
      else {
        st = gtext_json_writer_string(w, c.bytes.data(), c.bytes.size());
      }
      EXPECT_EQ(st, GTEXT_JSON_E_BAD_UNICODE)
          << c.what << (as_key ? " as a key" : " as a value");
      gtext_json_writer_free(w);
      gtext_json_sink_buffer_free(&s2);
    }
  }

  // And valid UTF-8 is written, so the check is about the bytes and not about
  // being non-ASCII at all.
  const char * valid = "\u00e9\u20ac";
  GTEXT_JSON_Value * ok = gtext_json_new_string(valid, std::strlen(valid));
  ASSERT_NE(ok, nullptr);
  GTEXT_JSON_Write_Options o = gtext_json_write_options_default();
  EXPECT_EQ(json_write_value_with(o, ok), "\"\u00e9\u20ac\"");
  gtext_json_free(ok);
}

/**
 * @test JsonWriterAgreement.PrettyDeclinesSpaceAfterComma
 *
 * `space_after_comma` is a compact-mode option. In pretty mode the comma is
 * followed by an indent that begins with a newline, so a space there is
 * trailing white space at the end of every line: it changes nothing a reader
 * sees and many tools object to it.
 *
 * The incremental writer had always declined it when pretty - the space is
 * inside its own compact-mode branch - and gtext_json_write_value() had not,
 * so the two produced different bytes for the same document whenever both
 * options were set. The agreement test above could not see it, because it
 * compared the two writers' **reparsed values**, and white space is exactly
 * what a reparse normalises away. tests/fuzz/fuzz_json_writer.cpp found it
 * once the records comparison began comparing bytes.
 */
TEST(JsonWriterAgreement, PrettyDeclinesSpaceAfterComma) {
  const char * const docs[] = {"[null,null,true]", "{\"a\":1,\"b\":2}",
      "[[1,2],[3,4]]", "{\"a\":[1,2],\"b\":{\"c\":3}}"};

  for (const char * d : docs) {
    GTEXT_JSON_Value * v =
        gtext_json_parse(d, std::strlen(d), nullptr, nullptr);
    ASSERT_NE(v, nullptr) << d;

    GTEXT_JSON_Write_Options pretty = gtext_json_write_options_default();
    pretty.pretty = true;
    pretty.space_after_comma = true;
    GTEXT_JSON_Write_Options plain = pretty;
    plain.space_after_comma = false;

    // In pretty mode the option makes no difference at all.
    const std::string with = json_write_value_with(pretty, v);
    EXPECT_EQ(with, json_write_value_with(plain, v)) << d;

    // And no line ends in a space, which is the reason.
    size_t start = 0;
    while (start < with.size()) {
      size_t nl = with.find('\n', start);
      const size_t end = (nl == std::string::npos) ? with.size() : nl;
      if (end > start) {
        EXPECT_NE(with[end - 1], ' ') << d << ": a line ends in a space:\n"
                                      << with;
      }
      if (nl == std::string::npos)
        break;
      start = nl + 1;
    }

    // Compact mode still honours it, so the option is not simply ignored.
    GTEXT_JSON_Write_Options compact = gtext_json_write_options_default();
    compact.space_after_comma = true;
    const std::string compact_out = json_write_value_with(compact, v);
    GTEXT_JSON_Write_Options compact_off = compact;
    compact_off.space_after_comma = false;
    EXPECT_NE(compact_out, json_write_value_with(compact_off, v))
        << d << ": compact mode must still add the space";

    gtext_json_free(v);
  }
}
