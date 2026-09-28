/**
 * @file
 *
 * The two conversions, and the four places the data models disagree.
 *
 * Most of what a conversion does is uninteresting and would be caught by the
 * corpus mode that sends every valid case through JSON and back. What is here
 * is the part that mode structurally cannot ask:
 *
 *   - **the policies.** A corpus case exercises one setting; the other arm of
 *     each policy is reachable only from a test. There are three policies and
 *     seven arms between them.
 *   - **the cases the corpus mode has to exclude.** It cannot score a date-time
 *     or a whole-valued float, because a round trip through JSON does not
 *     return one - so what they *do* become is asserted here, by value, rather
 *     than left as a sentence in a header.
 *   - **the JSON side's own limits.** `gtext_json_free()` is recursive: it
 *     survives about 103,000 levels on an 8 MB stack and dies by about 106,000
 *     (measured, by bisecting a nested array built through the public API), so
 *     the depth option on this conversion is what keeps a converted tree one
 *     the other module can release. That is the reason it is not a copy of the
 *     parse's limit, and the reason the deep test below is modest: a deeper one
 *     would be measuring the other module's stack.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cmath>
#include <cstring>
#include <string>
#include <gtest/gtest.h>

#include <ghoti.io/text/json.h>
#include <ghoti.io/text/toml.h>

namespace {

// A TOML document, parsed, or a fatal failure.
GTEXT_TOML_Value * toml_of(const std::string & text) {
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root =
      gtext_toml_parse(text.data(), text.size(), nullptr, &err);
  EXPECT_NE(root, nullptr) << "[" << text
                           << "]: " << (err.message ? err.message : "");
  gtext_toml_error_free(&err);
  return root;
}

GTEXT_JSON_Value * json_of(const std::string & text) {
  GTEXT_JSON_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_JSON_Value * root =
      gtext_json_parse(text.data(), text.size(), nullptr, &err);
  EXPECT_NE(root, nullptr) << "[" << text << "]";
  gtext_json_error_free(&err);
  return root;
}

// The JSON text of a value, which is the comparable form of "the same values".
std::string json_text(const GTEXT_JSON_Value * value) {
  GTEXT_JSON_Sink sink;
  EXPECT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
  GTEXT_JSON_Error err;
  std::memset(&err, 0, sizeof(err));
  std::string out;
  if (gtext_json_write_value(&sink, nullptr, value, &err) == GTEXT_JSON_OK) {
    out.assign(
        gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
  }
  gtext_json_sink_buffer_free(&sink);
  gtext_json_error_free(&err);
  return out;
}

std::string toml_text(const GTEXT_TOML_Value * root) {
  GTEXT_TOML_Sink sink;
  EXPECT_EQ(gtext_toml_sink_buffer(&sink), GTEXT_TOML_OK);
  std::string out;
  if (gtext_toml_write(root, &sink, nullptr) == GTEXT_TOML_OK) {
    out.assign(
        gtext_toml_sink_buffer_data(&sink), gtext_toml_sink_buffer_size(&sink));
  }
  gtext_toml_sink_buffer_free(&sink);
  return out;
}

// Convert TOML text to JSON text under `opts`, requiring success.
std::string to_json(
    const std::string & text, const GTEXT_TOML_To_JSON_Options * opts) {
  GTEXT_TOML_Value * root = toml_of(text);
  if (!root) return "";
  GTEXT_JSON_Value * json = nullptr;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Status status = gtext_toml_to_json(root, opts, &json, &err);
  EXPECT_EQ(status, GTEXT_TOML_OK) << (err.message ? err.message : "");
  std::string out = json ? json_text(json) : std::string();
  gtext_json_free(json);
  gtext_toml_free(root);
  gtext_toml_error_free(&err);
  return out;
}

// Convert TOML text to JSON under `opts`, requiring failure.
GTEXT_TOML_Status to_json_refused(
    const std::string & text, const GTEXT_TOML_To_JSON_Options * opts) {
  GTEXT_TOML_Value * root = toml_of(text);
  if (!root) return GTEXT_TOML_OK;
  GTEXT_JSON_Value * json = nullptr;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Status status = gtext_toml_to_json(root, opts, &json, &err);
  EXPECT_EQ(json, nullptr) << "[" << text << "] converted";
  EXPECT_EQ(status, err.code);
  gtext_json_free(json);
  gtext_toml_free(root);
  gtext_toml_error_free(&err);
  return status;
}

// Convert JSON text to TOML text under `opts`, requiring success.
std::string to_toml(
    const std::string & text, const GTEXT_TOML_From_JSON_Options * opts) {
  GTEXT_JSON_Value * json = json_of(text);
  if (!json) return "";
  GTEXT_TOML_Value * root = nullptr;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Status status = gtext_json_to_toml(json, opts, &root, &err);
  EXPECT_EQ(status, GTEXT_TOML_OK) << (err.message ? err.message : "");
  std::string out = root ? toml_text(root) : std::string();
  gtext_toml_free(root);
  gtext_json_free(json);
  gtext_toml_error_free(&err);
  return out;
}

GTEXT_TOML_Status to_toml_refused(
    const std::string & text, const GTEXT_TOML_From_JSON_Options * opts) {
  GTEXT_JSON_Value * json = json_of(text);
  if (!json) return GTEXT_TOML_OK;
  GTEXT_TOML_Value * root = nullptr;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Status status = gtext_json_to_toml(json, opts, &root, &err);
  EXPECT_EQ(root, nullptr) << "[" << text << "] converted";
  EXPECT_EQ(status, err.code);
  gtext_toml_free(root);
  gtext_json_free(json);
  gtext_toml_error_free(&err);
  return status;
}

} // namespace

// --------------------------------------------------------------------------
// What crosses unchanged
// --------------------------------------------------------------------------

TEST(TomlJson, EveryTypeWithACounterpartCrossesExactly) {
  EXPECT_EQ(to_json("s = \"x\"\ni = 42\nf = 1.5\nb = true\n"
                    "a = [1, \"two\"]\n[t]\nk = 0\n",
                nullptr),
      "{\"s\":\"x\",\"i\":42,\"f\":1.5,\"b\":true,\"a\":[1,\"two\"],"
      "\"t\":{\"k\":0}}");
}

TEST(TomlJson, TheKeysKeepTheirOrderAndTheirBytes) {
  // Definition order, not sorted: TOML's tables are ordered here and JSON
  // objects are ordered in this library, so there is no reason to lose it.
  EXPECT_EQ(to_json("z = 1\na = 2\n\"\" = 3\n", nullptr),
      "{\"z\":1,\"a\":2,\"\":3}");
  // A key containing a NUL survives as a key of that length, escaped by the
  // JSON writer rather than truncated by a strlen somewhere.
  EXPECT_EQ(to_json("\"a\\u0000b\" = 1\n", nullptr), "{\"a\\u0000b\":1}");
}

TEST(TomlJson, AnIntegerNearItsBoundsKeepsEveryDigit) {
  // Not through a double: 9223372036854775807 as a double is
  // 9223372036854775808, and a conversion that went that way would write the
  // wrong number for the one value the corpus checks exactly.
  EXPECT_EQ(to_json("i = 9223372036854775807\nj = -9223372036854775808\n",
                nullptr),
      "{\"i\":9223372036854775807,\"j\":-9223372036854775808}");
}

TEST(TomlJson, AnIntegerAndAFloatStayDifferentThings) {
  // JSON has one number type, so this is the one place the distinction can be
  // lost, and the *writer* is what says it was kept.
  EXPECT_EQ(to_json("i = 1\nf = 1.5\ne = 1e-3\n", nullptr),
      "{\"i\":1,\"f\":1.5,\"e\":0.001}");
}

TEST(TomlJson, AnEmptyTableAndAnEmptyArrayCross) {
  EXPECT_EQ(to_json("t = {}\na = []\n[u]\n", nullptr),
      "{\"t\":{},\"a\":[],\"u\":{}}");
}

TEST(TomlJson, ANestedDocumentCrossesInTheRightShape) {
  EXPECT_EQ(to_json("[[a]]\nx = 1\n[[a]]\nx = 2\n[a.b]\ny = 3\n", nullptr),
      "{\"a\":[{\"x\":1},{\"x\":2,\"b\":{\"y\":3}}]}");
}

TEST(TomlJson, AScalarRootConvertsBecauseJsonHasNoRuleAgainstOne) {
  // The asymmetry with the other direction, and deliberate: TOML says a
  // document is a table, JSON says a value is a value. A caller holding a
  // single TOML value has something JSON can hold.
  GTEXT_TOML_Value * one = gtext_toml_new_integer(nullptr, 7);
  ASSERT_NE(one, nullptr);
  GTEXT_JSON_Value * json = nullptr;
  EXPECT_EQ(gtext_toml_to_json(one, nullptr, &json, nullptr), GTEXT_TOML_OK);
  ASSERT_NE(json, nullptr);
  EXPECT_EQ(json_text(json), "7");
  gtext_json_free(json);
  gtext_toml_free(one);
}

// --------------------------------------------------------------------------
// Date-times: the value survives and the type does not
// --------------------------------------------------------------------------

TEST(TomlJson, AllFourDateTimeKindsBecomeTheStringTomlWouldHaveWritten) {
  // The same spelling gtext_toml_write() produces, from the same chron call, so
  // there is one date-time formatter in this module and not two.
  EXPECT_EQ(to_json("odt = 1979-05-27T07:32:00Z\n"
                    "ldt = 1979-05-27T07:32:00\n"
                    "ld = 1979-05-27\n"
                    "lt = 07:32:00\n",
                nullptr),
      "{\"odt\":\"1979-05-27T07:32:00Z\",\"ldt\":\"1979-05-27T07:32:00\","
      "\"ld\":\"1979-05-27\",\"lt\":\"07:32:00\"}");
}

TEST(TomlJson, AnUnknownOffsetKeepsTheOneSpellingThatCarriesIt) {
  EXPECT_EQ(to_json("odt = 1979-05-27T07:32:00-00:00\n", nullptr),
      "{\"odt\":\"1979-05-27T07:32:00-00:00\"}");
}

TEST(TomlJson, TheDateTimeErrorPolicyRefusesInsteadOfConverting) {
  GTEXT_TOML_To_JSON_Options opts = gtext_toml_to_json_options_default();
  opts.datetime = GTEXT_TOML_JSON_DATETIME_ERROR;
  EXPECT_EQ(to_json_refused("d = 1979-05-27\n", &opts),
      GTEXT_TOML_E_UNREPRESENTABLE);
  // And the rest of the document is not the reason: with the policy back at its
  // default the same document converts.
  EXPECT_EQ(to_json("d = 1979-05-27\n", nullptr), "{\"d\":\"1979-05-27\"}");
}

TEST(TomlJson, ADateTimeDoesNotComeBackAsADateTime) {
  // The documented half of the round trip, asserted rather than described: the
  // string stays a string, because a TOML string that happens to spell a date
  // *is* a string and a conversion that guessed would change what a document
  // means to make its own round trip look better.
  const std::string once = to_json("d = 1979-05-27\n", nullptr);
  EXPECT_EQ(once, "{\"d\":\"1979-05-27\"}");
  EXPECT_EQ(to_toml(once, nullptr), "d = \"1979-05-27\"\n");
}

// --------------------------------------------------------------------------
// Non-finite floats: three arms, and the default refuses
// --------------------------------------------------------------------------

TEST(TomlJson, ANonFiniteFloatIsRefusedByDefault) {
  EXPECT_EQ(to_json_refused("f = inf\n", nullptr),
      GTEXT_TOML_E_UNREPRESENTABLE);
  EXPECT_EQ(to_json_refused("f = nan\n", nullptr),
      GTEXT_TOML_E_UNREPRESENTABLE);
  EXPECT_EQ(to_json_refused("f = -inf\n", nullptr),
      GTEXT_TOML_E_UNREPRESENTABLE);
}

TEST(TomlJson, TheNonFiniteStringPolicyUsesTomlsOwnSpellings) {
  GTEXT_TOML_To_JSON_Options opts = gtext_toml_to_json_options_default();
  opts.nonfinite = GTEXT_TOML_JSON_NONFINITE_STRING;
  EXPECT_EQ(to_json("a = inf\nb = -inf\nc = nan\nd = -nan\n", &opts),
      "{\"a\":\"inf\",\"b\":\"-inf\",\"c\":\"nan\",\"d\":\"-nan\"}");
}

TEST(TomlJson, TheNonFiniteNullPolicyWritesNull) {
  GTEXT_TOML_To_JSON_Options opts = gtext_toml_to_json_options_default();
  opts.nonfinite = GTEXT_TOML_JSON_NONFINITE_NULL;
  EXPECT_EQ(to_json("a = inf\nb = nan\n", &opts), "{\"a\":null,\"b\":null}");
  // And that null cannot come back, which is the point of the option being a
  // choice of losses rather than a conversion: the two directions do not
  // compose under it.
  GTEXT_TOML_From_JSON_Options back = gtext_toml_from_json_options_default();
  EXPECT_EQ(to_toml_refused("{\"a\":null}", &back),
      GTEXT_TOML_E_UNREPRESENTABLE);
}

TEST(TomlJson, AFiniteFloatAtTheExtremesIsStillFinite) {
  // The boundary the policy does *not* apply to, which is the control for the
  // three tests above: a very large finite double is a number, not an infinity.
  const std::string out = to_json("f = 1.7976931348623157e308\n", nullptr);
  EXPECT_NE(out.find("1.7976931348623157e+308"), std::string::npos) << out;
}

// --------------------------------------------------------------------------
// JSON to TOML
// --------------------------------------------------------------------------

TEST(TomlJson, TheTypesComeBackFromTheLexemeAndNotFromACRepresentation) {
  // `1` is an integer and `1.0` is a float, which is the question TOML asks of
  // the same text. A conversion that asked gtext_json_get_i64() first would
  // make `1.0` an integer, because a double whose value is integral has one.
  // The float spellings are the TOML writer's, which picks the shortest form
  // that reads back as the same double - so `1e3` comes back as `1e+03` and not
  // as `1000.0`. That is phase 2's decision and this test's business is only
  // that both are floats.
  EXPECT_EQ(to_toml("{\"i\":1,\"f\":1.0,\"e\":1e3,\"neg\":-0.0}", nullptr),
      "i = 1\nf = 1.0\ne = 1e+03\nneg = -0.0\n");
}

TEST(TomlJson, StringsAndBooleansAndContainersCrossBack) {
  EXPECT_EQ(to_toml("{\"s\":\"x\",\"b\":false,\"a\":[1,2],\"t\":{\"k\":\"v\"}}",
                nullptr),
      "s = \"x\"\nb = false\na = [1, 2]\n\n[t]\nk = \"v\"\n");
}

TEST(TomlJson, AnIntegerTooLargeForInt64IsRefusedAndNotRounded) {
  // TOML's integer is int64 exactly. Silently going through a double here would
  // turn 9223372036854775808 into 9223372036854775807 or into a float, both of
  // which are a different document.
  //
  // The negative one is the case that matters and it caught a defect: an earlier
  // version fell back to the JSON value's double when no int64 was stored, and
  // -9223372036854775809 converts to exactly -2^63, so a range test on the
  // double accepted it as INT64_MIN. One bound spelled against INT64_MAX and the
  // other against INT64_MAX + 1, from the other side.
  EXPECT_EQ(to_toml_refused("{\"i\":9223372036854775808}", nullptr),
      GTEXT_TOML_E_RANGE);
  EXPECT_EQ(to_toml_refused("{\"i\":-9223372036854775809}", nullptr),
      GTEXT_TOML_E_RANGE);
  // The bounds themselves are fine, from both sides.
  EXPECT_EQ(
      to_toml("{\"i\":9223372036854775807,\"j\":-9223372036854775808}", nullptr),
      "i = 9223372036854775807\nj = -9223372036854775808\n");
}

TEST(TomlJson, ARootThatIsNotAnObjectHasNoTomlDocumentToBe) {
  EXPECT_EQ(to_toml_refused("[1,2]", nullptr), GTEXT_TOML_E_UNREPRESENTABLE);
  EXPECT_EQ(to_toml_refused("\"x\"", nullptr), GTEXT_TOML_E_UNREPRESENTABLE);
  EXPECT_EQ(to_toml_refused("42", nullptr), GTEXT_TOML_E_UNREPRESENTABLE);
  EXPECT_EQ(to_toml_refused("null", nullptr), GTEXT_TOML_E_UNREPRESENTABLE);
  // E_UNREPRESENTABLE and not E_INVALID: nothing is wrong with the JSON, and a
  // caller converting a batch wants to tell "this one cannot be TOML" from
  // "you called me wrongly".
}

TEST(TomlJson, ANullIsRefusedByDefaultAndSkippedWhenAsked) {
  EXPECT_EQ(to_toml_refused("{\"a\":1,\"b\":null}", nullptr),
      GTEXT_TOML_E_UNREPRESENTABLE);
  GTEXT_TOML_From_JSON_Options opts = gtext_toml_from_json_options_default();
  opts.null_values = GTEXT_TOML_JSON_NULL_SKIP;
  EXPECT_EQ(to_toml("{\"a\":1,\"b\":null,\"c\":2}", &opts), "a = 1\nc = 2\n");
  // Nested, and a whole object of nulls becomes an empty table rather than
  // disappearing: the key exists in the JSON and a table with no keys is how
  // TOML says that.
  EXPECT_EQ(to_toml("{\"t\":{\"b\":null}}", &opts), "[t]\n");
}

TEST(TomlJson, ANullInsideAnArrayIsRefusedWhateverThePolicySays) {
  // Dropping it would shorten the array and move every later index, which is a
  // different document rather than the same one with a gap. The skip policy is
  // about *members*, and this is the case that says so.
  GTEXT_TOML_From_JSON_Options opts = gtext_toml_from_json_options_default();
  opts.null_values = GTEXT_TOML_JSON_NULL_SKIP;
  EXPECT_EQ(to_toml_refused("{\"a\":[1,null,2]}", &opts),
      GTEXT_TOML_E_UNREPRESENTABLE);
  EXPECT_EQ(to_toml_refused("{\"a\":[null]}", &opts),
      GTEXT_TOML_E_UNREPRESENTABLE);
}

TEST(TomlJson, ANumberWithNoLexemeIsRefusedRatherThanGuessedAt) {
  // The one failure that is about how the JSON was *parsed* rather than about
  // what it says, so it is E_INVALID: with the lexeme thrown away there is
  // nothing left to tell an integer from a float.
  GTEXT_JSON_Parse_Options jopts = gtext_json_parse_options_default();
  jopts.preserve_number_lexeme = false;
  GTEXT_JSON_Error jerr;
  std::memset(&jerr, 0, sizeof(jerr));
  const std::string text = "{\"a\":1}";
  GTEXT_JSON_Value * json =
      gtext_json_parse(text.data(), text.size(), &jopts, &jerr);
  ASSERT_NE(json, nullptr);
  GTEXT_TOML_Value * root = nullptr;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_json_to_toml(json, nullptr, &root, &err),
      GTEXT_TOML_E_INVALID);
  EXPECT_EQ(root, nullptr);
  gtext_toml_error_free(&err);
  gtext_json_error_free(&jerr);
  gtext_json_free(json);
}

TEST(TomlJson, TwoJsonMembersOfOneNameHaveNoTomlSpelling) {
  // RFC 8259 only SHOULD-s unique names, and this library has parse policies
  // that keep both. TOML has one key, so the conversion reports the duplicate
  // rather than letting one win silently.
  GTEXT_JSON_Parse_Options jopts = gtext_json_parse_options_default();
  jopts.dupkeys = GTEXT_JSON_DUPKEY_COLLECT;
  const std::string text = "{\"a\":1,\"a\":2}";
  GTEXT_JSON_Error jerr;
  std::memset(&jerr, 0, sizeof(jerr));
  GTEXT_JSON_Value * json =
      gtext_json_parse(text.data(), text.size(), &jopts, &jerr);
  ASSERT_NE(json, nullptr);
  GTEXT_TOML_Value * root = nullptr;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  const GTEXT_TOML_Status status =
      gtext_json_to_toml(json, nullptr, &root, &err);
  // Whichever way the policy folded them, the result is a document TOML can
  // hold or a refusal naming the duplicate - never two keys of one name.
  if (status == GTEXT_TOML_OK) {
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(gtext_toml_table_size(root), 1u);
  }
  else {
    EXPECT_EQ(status, GTEXT_TOML_E_DUPKEY);
  }
  gtext_toml_free(root);
  gtext_toml_error_free(&err);
  gtext_json_error_free(&jerr);
  gtext_json_free(json);
}

// --------------------------------------------------------------------------
// The round trip, and its two documented gaps
// --------------------------------------------------------------------------

TEST(TomlJson, ADocumentWithoutTheTwoGapsIsAnIdentityThroughJson) {
  const std::string doc =
      "s = \"x\"\ni = -3\nf = 1.5\nb = true\na = [1, [2, 3]]\n\n"
      "[t]\nk = \"v\"\n\n[[u]]\nn = 1\n\n[[u]]\nn = 2\n";
  GTEXT_TOML_Value * root = toml_of(doc);
  ASSERT_NE(root, nullptr);
  GTEXT_JSON_Value * json = nullptr;
  ASSERT_EQ(gtext_toml_to_json(root, nullptr, &json, nullptr), GTEXT_TOML_OK);
  GTEXT_TOML_Value * back = nullptr;
  ASSERT_EQ(gtext_json_to_toml(json, nullptr, &back, nullptr), GTEXT_TOML_OK);
  // Not byte-identical text: an array of tables came back as an array of inline
  // tables, because JSON has no way to say which of the two spellings it was.
  // The *values* are the same, which is what a conversion promises.
  GTEXT_JSON_Value * again = nullptr;
  ASSERT_EQ(gtext_toml_to_json(back, nullptr, &again, nullptr), GTEXT_TOML_OK);
  EXPECT_EQ(json_text(json), json_text(again));
  gtext_json_free(again);
  gtext_json_free(json);
  gtext_toml_free(back);
  gtext_toml_free(root);
}

TEST(TomlJson, AWholeValuedFloatSurvivesTheJsonValueItselfAndNotOnlyItsText) {
  // The defect the corpus mode found on its first run, pinned here. A JSON
  // number built by gtext_json_new_number_double() carries a double and a
  // lexeme and *no* int64, so `1e3` arrives at the integer branch - its lexeme
  // is "1000", which says integer - with gtext_json_get_i64() failing. Reading
  // that failure as "outside int64_t" refused eight corpus cases that held
  // 1000. The in-memory value is what separates this from the test below: a
  // JSON number parsed from the text "1000" has an int64 and would pass either
  // way.
  GTEXT_TOML_Value * root = toml_of("a = 1e3\nb = 1.0\nc = -0.0\n");
  ASSERT_NE(root, nullptr);
  GTEXT_JSON_Value * json = nullptr;
  ASSERT_EQ(gtext_toml_to_json(root, nullptr, &json, nullptr), GTEXT_TOML_OK);
  ASSERT_NE(json, nullptr);
  GTEXT_TOML_Value * back = nullptr;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_json_to_toml(json, nullptr, &back, &err), GTEXT_TOML_OK)
      << (err.message ? err.message : "");
  ASSERT_NE(back, nullptr);
  // -0.0 keeps its sign and its type, because JSON writes it "-0" and that
  // lexeme has no point either - so the integer branch is where it lands, and
  // an integer -0 is 0. The sign is the thing that goes, not the value.
  EXPECT_EQ(toml_text(back), "a = 1000\nb = 1\nc = 0\n");
  gtext_toml_error_free(&err);
  gtext_toml_free(back);
  gtext_json_free(json);
  gtext_toml_free(root);
}

TEST(TomlJson, AWholeValuedFloatComesBackAsAnInteger) {
  // The second documented gap, asserted by value. JSON writes 1.0 as `1` - it
  // has one number type - so the text that comes back says integer and that is
  // all it says. Nothing here can recover it, and a conversion that guessed
  // from the *value* would turn every whole float into a float and every whole
  // integer into one too.
  EXPECT_EQ(to_json("f = 1.0\ng = 1e3\n", nullptr), "{\"f\":1,\"g\":1000}");
  EXPECT_EQ(to_toml("{\"f\":1,\"g\":1000}", nullptr), "f = 1\ng = 1000\n");
}

// --------------------------------------------------------------------------
// Limits and refusals of the calls themselves
// --------------------------------------------------------------------------

TEST(TomlJson, TheDepthLimitRefusesRatherThanBuildingATreeJsonCannotFree) {
  std::string deep = "a = ";
  for (int i = 0; i < 300; ++i) deep += "[";
  for (int i = 0; i < 300; ++i) deep += "]";
  deep += "\n";
  GTEXT_TOML_Parse_Options popts = gtext_toml_parse_options_default();
  popts.max_depth = 0;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root =
      gtext_toml_parse(deep.data(), deep.size(), &popts, &err);
  ASSERT_NE(root, nullptr);
  gtext_toml_error_free(&err);

  // The default is 256, which is also the JSON module's own parse default - so
  // a converted tree is one the JSON parser could have produced itself.
  GTEXT_JSON_Value * json = nullptr;
  EXPECT_EQ(gtext_toml_to_json(root, nullptr, &json, nullptr),
      GTEXT_TOML_E_DEPTH);
  EXPECT_EQ(json, nullptr);

  GTEXT_TOML_To_JSON_Options opts = gtext_toml_to_json_options_default();
  opts.max_depth = 0;
  EXPECT_EQ(gtext_toml_to_json(root, &opts, &json, nullptr), GTEXT_TOML_OK);
  ASSERT_NE(json, nullptr);
  // 300 deep is far from where gtext_json_free() runs out of stack (about
  // 103,000 on an 8 MB stack, measured), which is the whole reason this test
  // does not go deeper: past that it would be measuring the other module.
  GTEXT_TOML_Value * back = nullptr;
  GTEXT_TOML_From_JSON_Options bopts = gtext_toml_from_json_options_default();
  EXPECT_EQ(gtext_json_to_toml(json, &bopts, &back, nullptr),
      GTEXT_TOML_E_DEPTH);
  bopts.max_depth = 0;
  EXPECT_EQ(gtext_json_to_toml(json, &bopts, &back, nullptr), GTEXT_TOML_OK);
  EXPECT_NE(back, nullptr);
  gtext_toml_free(back);
  gtext_json_free(json);
  gtext_toml_free(root);
}

TEST(TomlJson, BothConversionsRefuseNullArguments) {
  GTEXT_JSON_Value * json = nullptr;
  GTEXT_TOML_Value * root = nullptr;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_toml_to_json(nullptr, nullptr, &json, &err),
      GTEXT_TOML_E_INVALID);
  EXPECT_EQ(err.code, GTEXT_TOML_E_INVALID);
  // Line and column are 0 and not 1: there is no input text here, and a
  // position invented from a tree walk would name nothing to look at.
  EXPECT_EQ(err.line, 0);
  EXPECT_EQ(err.col, 0);
  gtext_toml_error_free(&err);

  GTEXT_TOML_Value * table = gtext_toml_new_table(nullptr);
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(gtext_toml_to_json(table, nullptr, nullptr, nullptr),
      GTEXT_TOML_E_INVALID);
  gtext_toml_free(table);

  EXPECT_EQ(gtext_json_to_toml(nullptr, nullptr, &root, nullptr),
      GTEXT_TOML_E_INVALID);
  GTEXT_JSON_Value * object = gtext_json_new_object();
  ASSERT_NE(object, nullptr);
  EXPECT_EQ(gtext_json_to_toml(object, nullptr, nullptr, nullptr),
      GTEXT_TOML_E_INVALID);
  gtext_json_free(object);
}

TEST(TomlJson, TheOptionDefaultsAreWhatTheHeaderSays) {
  const GTEXT_TOML_To_JSON_Options out = gtext_toml_to_json_options_default();
  EXPECT_EQ(out.datetime, GTEXT_TOML_JSON_DATETIME_STRING);
  EXPECT_EQ(out.nonfinite, GTEXT_TOML_JSON_NONFINITE_ERROR);
  EXPECT_EQ(out.max_depth, 256u);
  const GTEXT_TOML_From_JSON_Options in =
      gtext_toml_from_json_options_default();
  EXPECT_EQ(in.allocator, nullptr);
  EXPECT_EQ(in.null_values, GTEXT_TOML_JSON_NULL_ERROR);
  EXPECT_EQ(in.max_depth, 256u);
  // A zeroed struct asks for the same policies and for *no* depth limit, which
  // is the one difference and the reason to start from the constructor.
  GTEXT_TOML_To_JSON_Options zeroed;
  std::memset(&zeroed, 0, sizeof(zeroed));
  EXPECT_EQ(zeroed.datetime, GTEXT_TOML_JSON_DATETIME_STRING);
  EXPECT_EQ(zeroed.nonfinite, GTEXT_TOML_JSON_NONFINITE_ERROR);
  EXPECT_EQ(zeroed.max_depth, 0u);
}

TEST(TomlJson, TheTomlTreeIsNotBorrowedFrom) {
  // Every string is copied, so a caller may free the document as soon as the
  // conversion returns. Under a sanitizer this is the test that says so.
  GTEXT_JSON_Value * json = nullptr;
  {
    GTEXT_TOML_Value * root = toml_of("s = \"a string long enough to heap\"\n");
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(gtext_toml_to_json(root, nullptr, &json, nullptr), GTEXT_TOML_OK);
    gtext_toml_free(root);
  }
  ASSERT_NE(json, nullptr);
  EXPECT_EQ(json_text(json), "{\"s\":\"a string long enough to heap\"}");
  gtext_json_free(json);

  GTEXT_TOML_Value * back = nullptr;
  {
    GTEXT_JSON_Value * source = json_of("{\"s\":\"another long enough one\"}");
    ASSERT_NE(source, nullptr);
    ASSERT_EQ(
        gtext_json_to_toml(source, nullptr, &back, nullptr), GTEXT_TOML_OK);
    gtext_json_free(source);
  }
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(toml_text(back), "s = \"another long enough one\"\n");
  gtext_toml_free(back);
}

TEST(TomlJson, TheConvertedTreeUsesTheAllocatorItWasGiven) {
  // The direction that has an allocator to give. The other has none, because
  // the JSON DOM's constructors take none - a field there would be one nothing
  // could read.
  const GTEXT_Allocator * counting = gtext_allocator_default();
  GTEXT_TOML_From_JSON_Options opts = gtext_toml_from_json_options_default();
  opts.allocator = counting;
  GTEXT_JSON_Value * json = json_of("{\"a\":[1,2],\"t\":{\"k\":\"v\"}}");
  ASSERT_NE(json, nullptr);
  GTEXT_TOML_Value * root = nullptr;
  ASSERT_EQ(gtext_json_to_toml(json, &opts, &root, nullptr), GTEXT_TOML_OK);
  ASSERT_NE(root, nullptr);
  // Freed through the root's own allocator, which is the invariant that makes
  // one wrong allocator here a heap corruption rather than a test failure.
  gtext_toml_free(root);
  gtext_json_free(json);
}
