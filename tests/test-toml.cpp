/**
 * @file
 *
 * TOML: the contract toml-test cannot see.
 *
 * The conformance suite (tools/conformance/run-toml.sh, 2316 of 2316 at TOML
 * 1.0.0 and 2362 of 2362 at 1.1.0) decides whether documents are read
 * correctly. It says nothing about
 * the API around that: what an accessor does with the wrong type, whether keys
 * come back in the order the document gave them, where an error points, or
 * whether a document nested past every reasonable depth can be freed. Those
 * are here.
 *
 * Two of the parser's checks are carried by almost nothing in the corpus -
 * measured, by breaking each and counting: a lone carriage return by one case,
 * and the up-front UTF-8 validation by two, and those two are the only ones
 * not already caught by the per-character check inside strings. Both are
 * covered directly here, because a check one corpus case away from being
 * untested is a check that will be silently deleted one day.
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

// Parse, requiring success.
GTEXT_TOML_Value * ok(const std::string & text) {
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root =
      gtext_toml_parse(text.data(), text.size(), nullptr, &err);
  EXPECT_NE(root, nullptr) << "[" << text << "]: "
                           << (err.message ? err.message : "no message");
  gtext_toml_error_free(&err);
  return root;
}

// Parse, requiring failure, and return the status.
GTEXT_TOML_Status refused(const std::string & text, int * line = nullptr,
    int * col = nullptr) {
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root =
      gtext_toml_parse(text.data(), text.size(), nullptr, &err);
  EXPECT_EQ(root, nullptr) << "[" << text << "] was accepted";
  gtext_toml_free(root);
  GTEXT_TOML_Status code = err.code;
  if (line) *line = err.line;
  if (col) *col = err.col;
  gtext_toml_error_free(&err);
  return code;
}

// A top-level key's value.
const GTEXT_TOML_Value * at(const GTEXT_TOML_Value * table, const char * key) {
  return gtext_toml_table_get(table, key, std::strlen(key));
}

std::string string_of(const GTEXT_TOML_Value * value) {
  size_t len = 0;
  const char * bytes = gtext_toml_value_string(value, &len);
  return bytes ? std::string(bytes, len) : std::string("<not a string>");
}

} // namespace

TEST(Toml, ReadsEachType) {
  GTEXT_TOML_Value * root = ok("s = \"text\"\n"
                               "i = 42\n"
                               "f = 3.5\n"
                               "b = true\n"
                               "odt = 1979-05-27T07:32:00Z\n"
                               "ldt = 1979-05-27T07:32:00\n"
                               "ld = 1979-05-27\n"
                               "lt = 07:32:00\n"
                               "arr = [1, 2]\n"
                               "[tbl]\nk = 1\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(gtext_toml_value_type(root), GTEXT_TOML_TABLE);
  EXPECT_EQ(gtext_toml_value_type(at(root, "s")), GTEXT_TOML_STRING);
  EXPECT_EQ(gtext_toml_value_type(at(root, "i")), GTEXT_TOML_INTEGER);
  EXPECT_EQ(gtext_toml_value_type(at(root, "f")), GTEXT_TOML_FLOAT);
  EXPECT_EQ(gtext_toml_value_type(at(root, "b")), GTEXT_TOML_BOOLEAN);
  EXPECT_EQ(gtext_toml_value_type(at(root, "arr")), GTEXT_TOML_ARRAY);
  EXPECT_EQ(gtext_toml_value_type(at(root, "tbl")), GTEXT_TOML_TABLE);

  // All four date-times are one type here and four kinds in chron, which is
  // the distinction TOML makes. A module that collapsed them would pass every
  // type check above and lose the difference.
  const char * names[] = {"odt", "ldt", "ld", "lt"};
  GCHRON_TomlKind want[] = {GCHRON_TOML_OFFSET_DATE_TIME,
      GCHRON_TOML_LOCAL_DATE_TIME, GCHRON_TOML_LOCAL_DATE,
      GCHRON_TOML_LOCAL_TIME};
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(gtext_toml_value_type(at(root, names[i])), GTEXT_TOML_DATETIME)
        << names[i];
    GCHRON_TomlValue dt;
    std::memset(&dt, 0, sizeof(dt));
    ASSERT_TRUE(gtext_toml_value_datetime(at(root, names[i]), &dt))
        << names[i];
    EXPECT_EQ(dt.kind, want[i]) << names[i];
  }
  gtext_toml_free(root);
}

TEST(Toml, AccessorsRefuseTheWrongType) {
  GTEXT_TOML_Value * root = ok("i = 1\n");
  ASSERT_NE(root, nullptr);
  const GTEXT_TOML_Value * i = at(root, "i");
  int64_t as_int = 0;
  double as_float = 0;
  bool as_bool = false;
  GCHRON_TomlValue as_dt;
  std::memset(&as_dt, 0, sizeof(as_dt));
  EXPECT_TRUE(gtext_toml_value_integer(i, &as_int));
  EXPECT_EQ(as_int, 1);
  EXPECT_FALSE(gtext_toml_value_float(i, &as_float));
  EXPECT_FALSE(gtext_toml_value_boolean(i, &as_bool));
  EXPECT_FALSE(gtext_toml_value_datetime(i, &as_dt));
  EXPECT_EQ(gtext_toml_value_string(i, nullptr), nullptr);
  EXPECT_EQ(gtext_toml_array_size(i), 0u);
  EXPECT_EQ(gtext_toml_table_size(i), 0u);
  EXPECT_EQ(gtext_toml_array_get(i, 0), nullptr);
  EXPECT_EQ(gtext_toml_table_value_at(i, 0), nullptr);
  EXPECT_EQ(gtext_toml_table_key_at(i, 0, nullptr), nullptr);

  // NULL everywhere, which is the other half of the same contract.
  EXPECT_FALSE(gtext_toml_value_integer(nullptr, &as_int));
  EXPECT_FALSE(gtext_toml_value_integer(i, nullptr));
  EXPECT_EQ(gtext_toml_table_get(nullptr, "i", 1), nullptr);
  EXPECT_EQ(gtext_toml_table_get(root, nullptr, 0), nullptr);
  gtext_toml_free(nullptr);
  gtext_toml_free(root);
}

TEST(Toml, KeysComeBackInDefinitionOrder) {
  // Not sorted, and not in the order a hash would give: a writer has to be
  // able to reproduce the arrangement a person chose.
  GTEXT_TOML_Value * root = ok("zebra = 1\napple = 2\nmango = 3\n");
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(gtext_toml_table_size(root), 3u);
  const char * expected[] = {"zebra", "apple", "mango"};
  for (size_t i = 0; i < 3; ++i) {
    size_t len = 0;
    const char * key = gtext_toml_table_key_at(root, i, &len);
    ASSERT_NE(key, nullptr);
    EXPECT_EQ(std::string(key, len), expected[i]);
  }
  gtext_toml_free(root);
}

TEST(Toml, KeyIdentityIsTheDecodedString) {
  // a."b", a.b and a.'b' name one key, so the second of these is a duplicate
  // rather than a second key. The check therefore cannot live in the lexer.
  EXPECT_EQ(refused("a.b = 1\na.\"b\" = 2\n"), GTEXT_TOML_E_DUPKEY);
  EXPECT_EQ(refused("\"x\" = 1\n'x' = 2\n"), GTEXT_TOML_E_DUPKEY);

  // And a dot inside a quoted key is not a separator.
  GTEXT_TOML_Value * root = ok("'a.b' = 1\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(gtext_toml_table_size(root), 1u);
  EXPECT_NE(at(root, "a.b"), nullptr);
  EXPECT_EQ(at(root, "a"), nullptr);
  gtext_toml_free(root);
}

TEST(Toml, AKeyMayContainNulAndTheLengthIsAuthoritative) {
  // U+0000 has an escape and is a legal quoted key, so keys are compared by
  // bytes and a length. strcmp would make these two keys one.
  GTEXT_TOML_Value * root = ok("\"a\\u0000b\" = 1\n\"a\" = 2\n");
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(gtext_toml_table_size(root), 2u);
  size_t len = 0;
  const char * key = gtext_toml_table_key_at(root, 0, &len);
  ASSERT_NE(key, nullptr);
  EXPECT_EQ(len, 3u);
  EXPECT_EQ(std::strlen(key), 1u); // what strcmp would have seen
  EXPECT_EQ(key[1], '\0');
  EXPECT_NE(gtext_toml_table_get(root, "a\0b", 3), nullptr);
  gtext_toml_free(root);
}

TEST(Toml, TheFourRedefinitionRulesAreFour) {
  // Each was pinned against the pinned reference rather than read off the
  // prose, and each is a different rule: collapsing them into one "already
  // defined" test gets at least one of these wrong.
  EXPECT_EQ(refused("[a]\n[a]\n"), GTEXT_TOML_E_REDEFINE);
  EXPECT_EQ(refused("[x]\na.b = 1\n[x.a]\n"), GTEXT_TOML_E_REDEFINE);
  EXPECT_EQ(refused("a = {b = 1}\na.c = 2\n"), GTEXT_TOML_E_REDEFINE);
  EXPECT_EQ(refused("a = []\n[[a]]\n"), GTEXT_TOML_E_REDEFINE);

  // And the three shapes that are allowed, which the same code has to let
  // through: a super-table afterwards, a dotted key extending an implicit
  // table, and a header below a dotted one.
  GTEXT_TOML_Value * root = ok("[a.b]\nc = 1\n[a]\nd = 2\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(gtext_toml_table_size(root), 1u);
  EXPECT_EQ(gtext_toml_table_size(at(root, "a")), 2u);
  gtext_toml_free(root);

  root = ok("[x.a.b]\nz = 1\n[x]\na.c = 1\n");
  ASSERT_NE(root, nullptr);
  gtext_toml_free(root);

  root = ok("[x]\na.b = 1\n[x.a.c]\nd = 2\n");
  ASSERT_NE(root, nullptr);
  gtext_toml_free(root);
}

TEST(Toml, ArrayOfTablesAppends) {
  GTEXT_TOML_Value * root = ok("[[a]]\nn = 1\n[[a]]\nn = 2\n[a.sub]\nk = 3\n");
  ASSERT_NE(root, nullptr);
  const GTEXT_TOML_Value * a = at(root, "a");
  ASSERT_EQ(gtext_toml_value_type(a), GTEXT_TOML_ARRAY);
  ASSERT_EQ(gtext_toml_array_size(a), 2u);
  int64_t n = 0;
  ASSERT_TRUE(gtext_toml_value_integer(at(gtext_toml_array_get(a, 1), "n"), &n));
  EXPECT_EQ(n, 2);
  // [a.sub] lands in the newest element, not the first.
  EXPECT_EQ(at(gtext_toml_array_get(a, 0), "sub"), nullptr);
  EXPECT_NE(at(gtext_toml_array_get(a, 1), "sub"), nullptr);
  gtext_toml_free(root);
}

TEST(Toml, IntegerBoundsAreExactlyInt64) {
  GTEXT_TOML_Value * root =
      ok("hi = 9223372036854775807\nlo = -9223372036854775808\n");
  ASSERT_NE(root, nullptr);
  int64_t v = 0;
  ASSERT_TRUE(gtext_toml_value_integer(at(root, "hi"), &v));
  EXPECT_EQ(v, INT64_MAX);
  ASSERT_TRUE(gtext_toml_value_integer(at(root, "lo"), &v));
  EXPECT_EQ(v, INT64_MIN);
  gtext_toml_free(root);

  // One past each end, and the two spellings of the bound are separate
  // checks: a parser that wrote one against INT64_MAX and the other against
  // INT64_MAX + 1 accepts one of these.
  EXPECT_EQ(refused("x = 9223372036854775808\n"), GTEXT_TOML_E_RANGE);
  EXPECT_EQ(refused("x = -9223372036854775809\n"), GTEXT_TOML_E_RANGE);
  EXPECT_EQ(refused("x = 0x8000000000000000\n"), GTEXT_TOML_E_RANGE);
}

TEST(Toml, FloatsIncludeTheInfinitiesAndNan) {
  GTEXT_TOML_Value * root =
      ok("a = inf\nb = -inf\nc = +inf\nd = nan\ne = -nan\n");
  ASSERT_NE(root, nullptr);
  double v = 0;
  ASSERT_TRUE(gtext_toml_value_float(at(root, "a"), &v));
  EXPECT_TRUE(std::isinf(v) && v > 0);
  ASSERT_TRUE(gtext_toml_value_float(at(root, "b"), &v));
  EXPECT_TRUE(std::isinf(v) && v < 0);
  ASSERT_TRUE(gtext_toml_value_float(at(root, "c"), &v));
  EXPECT_TRUE(std::isinf(v) && v > 0);
  ASSERT_TRUE(gtext_toml_value_float(at(root, "d"), &v));
  EXPECT_TRUE(std::isnan(v));
  ASSERT_TRUE(gtext_toml_value_float(at(root, "e"), &v));
  EXPECT_TRUE(std::isnan(v));
  gtext_toml_free(root);
}

/* Numbers do not depend on the locale: asserted in
   tests/test-locale-numbers.cpp, which generates a comma locale with localedef
   and fails if it cannot, rather than here. The test that was here called
   setlocale(LC_NUMERIC, "de_DE.UTF-8") and used whatever came back - and that
   returns NULL on this machine, so it ran in the C locale every time and
   measured nothing. */

TEST(Toml, StringFormsDecode) {
  GTEXT_TOML_Value * root = ok("a = \"x\\ty\\u0041\"\n"
                               "b = 'x\\ty'\n"
                               "c = \"\"\"\nline\"\"\"\n"
                               "d = '''\nline'''\n"
                               "e = \"\"\"a \\\n    b\"\"\"\n"
                               "f = \"\"\"x\"\"\"\"\n"
                               "g = \"\"\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(string_of(at(root, "a")), "x\tyA");
  EXPECT_EQ(string_of(at(root, "b")), "x\\ty"); // literal: no escapes at all
  EXPECT_EQ(string_of(at(root, "c")), "line");  // the first newline is trimmed
  EXPECT_EQ(string_of(at(root, "d")), "line");
  EXPECT_EQ(string_of(at(root, "e")), "a b"); // line-ending backslash
  EXPECT_EQ(string_of(at(root, "f")), "x\""); // four quotes: one is content
  EXPECT_EQ(string_of(at(root, "g")), "");
  // An empty string is a string, so its accessor returns a pointer and not the
  // NULL that means "not a string".
  EXPECT_NE(gtext_toml_value_string(at(root, "g"), nullptr), nullptr);
  gtext_toml_free(root);

  // Six quotes close at the third and leave three behind, which is a statement
  // that does not end at its line rather than a string containing quotes.
  EXPECT_EQ(refused("a = \"\"\"x\"\"\"\"\"\"\n"), GTEXT_TOML_E_BAD_TOKEN);
}

TEST(Toml, ALoneCarriageReturnIsRefusedEverywhere) {
  // One corpus case covers this; measured by breaking the check and counting.
  EXPECT_EQ(refused("a = 1\rb = 2\n"), GTEXT_TOML_E_CONTROL);
  EXPECT_EQ(refused("a = 1 # c\rb = 2\n"), GTEXT_TOML_E_CONTROL);
  EXPECT_EQ(refused("a = \"\"\"x\ry\"\"\"\n"), GTEXT_TOML_E_CONTROL);
  // CRLF is a newline and is not the same thing.
  GTEXT_TOML_Value * root = ok("a = 1\r\nb = 2\r\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(gtext_toml_table_size(root), 2u);
  gtext_toml_free(root);
}

TEST(Toml, InvalidUtf8IsRefusedOutsideStringsToo) {
  // The up-front validation and the per-character check inside strings are
  // complementary, not redundant: breaking the first leaves exactly two corpus
  // cases failing, both of them bytes no scanner looks at.
  EXPECT_EQ(refused("# \xff\na = 1\n"), GTEXT_TOML_E_BAD_UNICODE);
  EXPECT_EQ(refused("a = \"\xff\"\n"), GTEXT_TOML_E_BAD_UNICODE);
  // A surrogate, which is well-formed by byte shape and is not a character.
  EXPECT_EQ(refused("a = \"\xed\xa0\x80\"\n"), GTEXT_TOML_E_BAD_UNICODE);
  // And an escape naming one, which is a different code path to the same rule.
  EXPECT_EQ(refused("a = \"\\ud800\"\n"), GTEXT_TOML_E_BAD_UNICODE);
}

TEST(Toml, ControlCharactersAreRefusedInCommentsAsWellAsStrings) {
  EXPECT_EQ(refused("a = 1 # \x01\n"), GTEXT_TOML_E_CONTROL);
  EXPECT_EQ(refused("a = \"\x01\"\n"), GTEXT_TOML_E_CONTROL);
  // Tab is not a control character for this purpose, in either place.
  GTEXT_TOML_Value * root = ok("a = \"x\ty\" # \tc\n");
  ASSERT_NE(root, nullptr);
  gtext_toml_free(root);
}

TEST(Toml, TheOneOneZeroAdditionsAreRefusedByDefault) {
  // The default is the released version, and this is that statement made from
  // the outside: NULL options, no version named. The option itself, both arms
  // and every case where they part, is tests/test-toml-version.cpp - including
  // the two other routes to this default, which this file's `refused()` helper
  // cannot reach because it always passes NULL.
  //
  // `07:32` answers E_BAD_TOKEN and not E_DATETIME: the missing seconds are
  // now the scanner's own refusal, pointing at the place they are missing,
  // rather than chron declining a time it was handed.
  EXPECT_EQ(refused("a = \"\\e\"\n"), GTEXT_TOML_E_BAD_ESCAPE);
  EXPECT_EQ(refused("a = \"\\x41\"\n"), GTEXT_TOML_E_BAD_ESCAPE);
  EXPECT_EQ(refused("a = {b = 1,}\n"), GTEXT_TOML_E_BAD_TOKEN);
  EXPECT_EQ(refused("a = {\n  b = 1\n}\n"), GTEXT_TOML_E_BAD_TOKEN);
  EXPECT_EQ(refused("a = 07:32\n"), GTEXT_TOML_E_BAD_TOKEN);
}

TEST(Toml, ErrorsPointAtTheRightPlace) {
  int line = 0;
  int col = 0;
  // A duplicate is reported where the key began, not at the '=' the parser had
  // reached by the time it knew.
  EXPECT_EQ(refused("a = 1\nb = 2\na = 3\n", &line, &col),
      GTEXT_TOML_E_DUPKEY);
  EXPECT_EQ(line, 3);
  EXPECT_EQ(col, 1);

  // Columns count characters, so a line of multi-byte text points at the
  // character and not at the byte. The `!` is the tenth character of
  // `a = "\u00e9\u00e9" !` and the twelfth byte, so this number separates the
  // two conventions rather than merely being present.
  EXPECT_EQ(refused("a = \"\xc3\xa9\xc3\xa9\" !\n", &line, &col),
      GTEXT_TOML_E_BAD_TOKEN);
  EXPECT_EQ(line, 1);
  EXPECT_EQ(col, 10);
}

TEST(Toml, ASnippetOfTheLineComesBack) {
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  const char * text = "ok = 1\nbroken = = 2\n";
  EXPECT_EQ(gtext_toml_parse(text, std::strlen(text), nullptr, &err), nullptr);
  ASSERT_NE(err.context_snippet, nullptr);
  EXPECT_EQ(std::string(err.context_snippet, err.context_snippet_len),
      "broken = = 2");
  EXPECT_LE(err.caret_offset, err.context_snippet_len);
  gtext_toml_error_free(&err);
  // Twice, and on a zeroed struct.
  gtext_toml_error_free(&err);
  GTEXT_TOML_Error zeroed;
  std::memset(&zeroed, 0, sizeof(zeroed));
  gtext_toml_error_free(&zeroed);
  gtext_toml_error_free(nullptr);
}

TEST(Toml, MaxDepthIsEnforcedAndZeroMeansNoLimit) {
  std::string deep = "a = ";
  for (int i = 0; i < 40; ++i) deep += "[";
  for (int i = 0; i < 40; ++i) deep += "]";
  deep += "\n";

  GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
  EXPECT_EQ(opts.max_depth, 256u);
  opts.max_depth = 8;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_toml_parse(deep.data(), deep.size(), &opts, &err), nullptr);
  EXPECT_EQ(err.code, GTEXT_TOML_E_DEPTH);
  gtext_toml_error_free(&err);

  opts.max_depth = 0;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root =
      gtext_toml_parse(deep.data(), deep.size(), &opts, &err);
  EXPECT_NE(root, nullptr) << (err.message ? err.message : "");
  gtext_toml_error_free(&err);
  gtext_toml_free(root);
}

TEST(Toml, ADocumentNestedPastAnyStackIsParsedAndFreed) {
  // The reason the value parser and the teardown both work on the heap. With
  // max_depth at 0 a caller has asked for no limit, and the contract is that
  // this still does not crash - which is what notes/text/HEAP-STACK-WALKS.md
  // took the YAML module from "unless you asked for no limit" to.
  const int depth = 100000;
  std::string deep = "a = ";
  deep.append(depth, '[');
  deep.append(depth, ']');
  deep += "\n";
  GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
  opts.max_depth = 0;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root =
      gtext_toml_parse(deep.data(), deep.size(), &opts, &err);
  ASSERT_NE(root, nullptr) << (err.message ? err.message : "");
  gtext_toml_error_free(&err);
  // Walk to the bottom without recursing, to prove it is really that deep.
  const GTEXT_TOML_Value * node = at(root, "a");
  int seen = 0;
  while (node && gtext_toml_value_type(node) == GTEXT_TOML_ARRAY
      && gtext_toml_array_size(node) == 1) {
    node = gtext_toml_array_get(node, 0);
    ++seen;
  }
  EXPECT_EQ(seen, depth - 1);
  gtext_toml_free(root);
}

TEST(Toml, AnEmptyDocumentIsAnEmptyTable) {
  for (const char * text : {"", "\n", "# only a comment\n", "  \t\n\n"}) {
    GTEXT_TOML_Value * root = gtext_toml_parse(text, std::strlen(text),
        nullptr, nullptr);
    ASSERT_NE(root, nullptr) << "[" << text << "]";
    EXPECT_EQ(gtext_toml_value_type(root), GTEXT_TOML_TABLE);
    EXPECT_EQ(gtext_toml_table_size(root), 0u);
    gtext_toml_free(root);
  }
}

TEST(Toml, ABomIsRefused) {
  // A choice rather than a rule: neither 1.0.0 nor the 1.1.0 draft mentions a
  // byte order mark and the corpus has no case for a leading one. It is
  // refused because the pinned reference refuses it.
  EXPECT_EQ(refused("\xef\xbb\xbf" "a = 1\n"), GTEXT_TOML_E_BAD_TOKEN);
}

TEST(Toml, NoInputIsRefusedRatherThanCrashing) {
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_toml_parse(nullptr, 10, nullptr, &err), nullptr);
  EXPECT_EQ(err.code, GTEXT_TOML_E_INVALID);
  gtext_toml_error_free(&err);
  // And with no error struct at all, which every entry point must accept.
  EXPECT_EQ(gtext_toml_parse(nullptr, 0, nullptr, nullptr), nullptr);
}

TEST(Toml, MaxTotalBytesIsAPromise) {
  GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
  opts.max_total_bytes = 4;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  const char * text = "a = 1\n";
  EXPECT_EQ(gtext_toml_parse(text, std::strlen(text), &opts, &err), nullptr);
  EXPECT_EQ(err.code, GTEXT_TOML_E_LIMIT);
  gtext_toml_error_free(&err);
}

TEST(Toml, ParsesAFile) {
  char path[] = "/tmp/gtext-toml-testXXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  const char * text = "[section]\nkey = \"value\"\n";
  ASSERT_EQ(write(fd, text, std::strlen(text)),
      (ssize_t) std::strlen(text));
  close(fd);

  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root = gtext_toml_parse_file(path, nullptr, &err);
  ASSERT_NE(root, nullptr) << (err.message ? err.message : "");
  EXPECT_EQ(string_of(at(at(root, "section"), "key")), "value");
  gtext_toml_free(root);
  gtext_toml_error_free(&err);
  std::remove(path);

  // A path that is not there, and no path at all.
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_toml_parse_file("/nonexistent/toml", nullptr, &err), nullptr);
  EXPECT_EQ(err.code, GTEXT_TOML_E_INVALID);
  gtext_toml_error_free(&err);
  EXPECT_EQ(gtext_toml_parse_file(nullptr, nullptr, nullptr), nullptr);
}
