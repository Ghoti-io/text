/**
 * @file
 *
 * Comments in the tree, and out of it again.
 *
 * TOML's data model has no comments in it, so nothing in toml-test can score
 * this: the suite's expectations are the *values* of a document, and two files
 * differing only in their comments have the same expectation. The corpus does
 * carry one measurement, added with this - a `comments` mode that reads every
 * valid case with comments retained, writes it, reads it again, and requires
 * the same comments to come back - and that mode is a round trip, so it is
 * blind in exactly one way: a defect that dropped a comment on the way *in*
 * and another that dropped it on the way out agree with each other. This file
 * is where each half is looked at on its own.
 *
 * What is being fixed by these tests rather than merely described:
 *
 *   - **The tree keeps the comments the writer can put back, and no others.**
 *     A comment inside `{ }` or between two array elements has no statement to
 *     hang from; keeping it would promise a caller something the write side
 *     cannot deliver. So it is not kept, and the event walk is where it is
 *     found - which is why `ACommentInsideAValueIsNotInTheTreeAndIsInTheStream`
 *     asserts both halves at once, in one place, instead of trusting a sentence
 *     in a header.
 *   - **A comment's text is what followed the `#`.** Not trimmed. `#` plus the
 *     text is the line, which is what makes read-write an identity on comment
 *     lines, and the assertion is the bytes rather than "a comment is present".
 *   - **The writer refuses what it cannot spell.** Four refusals, one per
 *     thing: a control character, bytes that are not UTF-8, a line break in a
 *     comment that has to end a line, and a comment on a value going inside
 *     braces. Each is reachable only through the DOM setters or through the
 *     inline table style, so no document can find them and no fuzzer of
 *     documents alone would either.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>
#include <gtest/gtest.h>

#include <ghoti.io/text/toml.h>

namespace {

GTEXT_TOML_Parse_Options kept() {
  GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
  opts.retain_comments = true;
  return opts;
}

GTEXT_TOML_Value * read_kept(const std::string & text) {
  GTEXT_TOML_Parse_Options opts = kept();
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root =
      gtext_toml_parse(text.data(), text.size(), &opts, &err);
  EXPECT_NE(root, nullptr) << "[" << text
                           << "]: " << (err.message ? err.message : "");
  gtext_toml_error_free(&err);
  return root;
}

const GTEXT_TOML_Value * key(const GTEXT_TOML_Value * table, const char * k) {
  return gtext_toml_table_get(table, k, std::strlen(k));
}

// A node's three comments as one string, so that an assertion says all of them
// at once: absent is "-", which no comment text can be mistaken for here.
std::string comments_of(const GTEXT_TOML_Value * value) {
  const char * lead = gtext_toml_value_leading_comment(value);
  const char * in = gtext_toml_value_inline_comment(value);
  const char * trail = gtext_toml_value_trailing_comment(value);
  return std::string("lead[") + (lead ? lead : "-") + "] in[" + (in ? in : "-")
      + "] trail[" + (trail ? trail : "-") + "]";
}

// Write `root`, requiring success, and return the bytes.
std::string written(
    const GTEXT_TOML_Value * root, const GTEXT_TOML_Write_Options * opts) {
  GTEXT_TOML_Sink sink;
  EXPECT_EQ(gtext_toml_sink_buffer(&sink), GTEXT_TOML_OK);
  GTEXT_TOML_Status status = gtext_toml_write(root, &sink, opts);
  EXPECT_EQ(status, GTEXT_TOML_OK);
  std::string out(
      gtext_toml_sink_buffer_data(&sink), gtext_toml_sink_buffer_size(&sink));
  gtext_toml_sink_buffer_free(&sink);
  return out;
}

// Write `root`, requiring failure, and return the status.
GTEXT_TOML_Status refused_write(
    const GTEXT_TOML_Value * root, const GTEXT_TOML_Write_Options * opts) {
  GTEXT_TOML_Sink sink;
  EXPECT_EQ(gtext_toml_sink_buffer(&sink), GTEXT_TOML_OK);
  GTEXT_TOML_Status status = gtext_toml_write(root, &sink, opts);
  gtext_toml_sink_buffer_free(&sink);
  return status;
}

} // namespace

// --------------------------------------------------------------------------
// What the tree keeps
// --------------------------------------------------------------------------

TEST(TomlComments, AStatementTakesTheCommentsAroundIt) {
  GTEXT_TOML_Value * root = read_kept("# above\na = 1 # beside\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(comments_of(key(root, "a")), "lead[ above] in[ beside] trail[-]");
  gtext_toml_free(root);
}

TEST(TomlComments, AHeaderSCommentsBelongToTheTableItOpened) {
  GTEXT_TOML_Value * root = read_kept("# above\n[t] # beside\nx = 1\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(comments_of(key(root, "t")), "lead[ above] in[ beside] trail[-]");
  // And not to the key inside it, which is the off-by-one a pending-comment
  // buffer makes available: `# above` is consumed by the header.
  EXPECT_EQ(comments_of(key(key(root, "t"), "x")), "lead[-] in[-] trail[-]");
  gtext_toml_free(root);
}

TEST(TomlComments, ConsecutiveOwnLineCommentsAreOneCommentOfSeveralLines) {
  GTEXT_TOML_Value * root = read_kept("# one\n# two\n# three\na = 1\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(std::string(gtext_toml_value_leading_comment(key(root, "a"))),
      " one\n two\n three");
  gtext_toml_free(root);
}

TEST(TomlComments, ABlankLineDoesNotSeparateTwoLeadingComments) {
  // There is no rule in TOML by which it would, and a rule invented here would
  // have to be written on both sides: the writer has no way to put the blank
  // line back, so a reader that treated it as a separator would lose a comment
  // on every round trip.
  GTEXT_TOML_Value * root = read_kept("# one\n\n# two\na = 1\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(
      std::string(gtext_toml_value_leading_comment(key(root, "a"))), " one\n two");
  gtext_toml_free(root);
}

TEST(TomlComments, TheCommentsAfterTheLastStatementBelongToTheDocument) {
  GTEXT_TOML_Value * root = read_kept("a = 1\n# the end\n# really\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(comments_of(root), "lead[-] in[-] trail[ the end\n really]");
  // On the root, and not as the leading comment of anything: there is nothing
  // after them. The first version of this put them in the root's *leading*
  // slot, where the writer emitted them above the first statement.
  EXPECT_EQ(comments_of(key(root, "a")), "lead[-] in[-] trail[-]");
  gtext_toml_free(root);
}

TEST(TomlComments, ADocumentOfNothingButCommentsKeepsThem) {
  GTEXT_TOML_Value * root = read_kept("# only this\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(comments_of(root), "lead[-] in[-] trail[ only this]");
  EXPECT_EQ(gtext_toml_table_size(root), 0u);
  gtext_toml_free(root);
}

TEST(TomlComments, TheTextIsVerbatimAfterTheHash) {
  GTEXT_TOML_Value * root = read_kept("#no space\na = 1 #\tand a tab\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(comments_of(key(root, "a")),
      "lead[no space] in[\tand a tab] trail[-]");
  gtext_toml_free(root);
}

TEST(TomlComments, AnEmptyCommentIsStoredRatherThanTreatedAsAbsent) {
  GTEXT_TOML_Value * root = read_kept("#\na = 1 #\n");
  ASSERT_NE(root, nullptr);
  // Empty strings, not NULLs: `#` is a comment line, and the writer has to put
  // it back. `lead[-]` here would mean the line had vanished.
  EXPECT_EQ(comments_of(key(root, "a")), "lead[] in[] trail[-]");
  gtext_toml_free(root);
}

TEST(TomlComments, NothingIsKeptUnlessAsked) {
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  const std::string doc = "# above\na = 1 # beside\n# the end\n";
  GTEXT_TOML_Value * root =
      gtext_toml_parse(doc.data(), doc.size(), nullptr, &err);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(comments_of(key(root, "a")), "lead[-] in[-] trail[-]");
  EXPECT_EQ(comments_of(root), "lead[-] in[-] trail[-]");
  // And the document is the same document: a comment is not part of TOML's
  // data model, so this option cannot change what was read.
  int64_t got = 0;
  EXPECT_TRUE(gtext_toml_value_integer(key(root, "a"), &got));
  EXPECT_EQ(got, 1);
  gtext_toml_free(root);
  gtext_toml_error_free(&err);
}

TEST(TomlComments, ACommentInsideAValueIsNotInTheTreeAndIsInTheStream) {
  // The division of labour, asserted from both sides in one place. The tree
  // keeps what the writer can put back; `# inside` is not that, because TOML
  // 1.0.0 has no line inside `{ }` to put it on and the writer does not break
  // an array across lines.
  const std::string doc = "a = [1, # inside\n  2]\nb = { c = 1 }\n";
  GTEXT_TOML_Value * root = read_kept(doc);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(comments_of(key(root, "a")), "lead[-] in[-] trail[-]");
  const GTEXT_TOML_Value * first = gtext_toml_array_get(key(root, "a"), 0);
  EXPECT_EQ(comments_of(first), "lead[-] in[-] trail[-]");
  const GTEXT_TOML_Value * second = gtext_toml_array_get(key(root, "a"), 1);
  EXPECT_EQ(comments_of(second), "lead[-] in[-] trail[-]");
  gtext_toml_free(root);

  int seen = 0;
  auto count = [](void * user, const GTEXT_TOML_Event * e,
                   GTEXT_TOML_Error *) -> GTEXT_TOML_Status {
    if (e->type == GTEXT_TOML_EVT_COMMENT) {
      EXPECT_EQ(std::string(e->comment, e->comment_len), " inside");
      EXPECT_FALSE(e->comment_own_line);
      ++*static_cast<int *>(user);
    }
    return GTEXT_TOML_OK;
  };
  GTEXT_TOML_Parse_Options opts = kept();
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_toml_read_events(
                doc.data(), doc.size(), &opts, count, &seen, &err),
      GTEXT_TOML_OK);
  gtext_toml_error_free(&err);
  EXPECT_EQ(seen, 1);
}

// --------------------------------------------------------------------------
// Writing them back
// --------------------------------------------------------------------------

TEST(TomlComments, ACommentedDocumentIsWrittenBackAsItself) {
  // Byte for byte, which is only possible because the stored text is what
  // followed the `#`: a writer that normalised `#one` to `# one` would come
  // close and never be an identity.
  const std::string doc =
      "#one\n# two\na = 1 # beside a\n"
      "\n"
      "# above t\n[t] # beside t\nb = 2\n"
      "# the end\n";
  GTEXT_TOML_Value * root = read_kept(doc);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(written(root, nullptr), doc);
  gtext_toml_free(root);
}

TEST(TomlComments, ARoundTripKeepsTheSameCommentsTheSecondTimeAsTheFirst) {
  const std::string doc =
      "# head\nx = 1 # beside\n[[a]] # first\nk = 1\n[[a]] # second\nk = 2\n"
      "# trailing\n";
  GTEXT_TOML_Value * once = read_kept(doc);
  ASSERT_NE(once, nullptr);
  const std::string out = written(once, nullptr);
  GTEXT_TOML_Value * twice = read_kept(out);
  ASSERT_NE(twice, nullptr);
  EXPECT_EQ(written(twice, nullptr), out);
  EXPECT_EQ(comments_of(twice), comments_of(once));
  EXPECT_EQ(comments_of(key(twice, "x")), comments_of(key(once, "x")));
  gtext_toml_free(once);
  gtext_toml_free(twice);
}

TEST(TomlComments, ALeadingCommentGoesAfterTheBlankLineBeforeItsHeader) {
  // Before the blank line it would read as belonging to whatever came above,
  // and the next parse would attach it there - a comment moving up a table on
  // every trip.
  GTEXT_TOML_Value * root = read_kept("a = 1\n# about t\n[t]\nb = 2\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(written(root, nullptr), "a = 1\n\n# about t\n[t]\nb = 2\n");
  gtext_toml_free(root);
}

TEST(TomlComments, ACallerSCommentIsWrittenWithNothingAddedToIt) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Value * one = gtext_toml_new_integer(nullptr, 1);
  ASSERT_NE(one, nullptr);
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1, one), GTEXT_TOML_OK);
  // Two lines from one call, which is the inverse of how a parse joins them.
  EXPECT_EQ(gtext_toml_value_set_leading_comment(one, " first\n second"),
      GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_value_set_inline_comment(one, " beside"), GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_value_set_trailing_comment(root, " at the end"),
      GTEXT_TOML_OK);
  EXPECT_EQ(written(root, nullptr),
      "# first\n# second\na = 1 # beside\n# at the end\n");
  gtext_toml_free(root);
}

TEST(TomlComments, SettingAgainReplacesAndNullRemoves) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(gtext_toml_value_set_leading_comment(root, " one"), GTEXT_TOML_OK);
  EXPECT_EQ(std::string(gtext_toml_value_leading_comment(root)), " one");
  EXPECT_EQ(gtext_toml_value_set_leading_comment(root, " two"), GTEXT_TOML_OK);
  EXPECT_EQ(std::string(gtext_toml_value_leading_comment(root)), " two");
  EXPECT_EQ(gtext_toml_value_set_leading_comment(root, nullptr), GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_value_leading_comment(root), nullptr);
  gtext_toml_free(root);
}

TEST(TomlComments, RemovingACommentFromANodeThatHasNoneIsNotAFailure) {
  // And allocates nothing: clearing what may or may not be there is how a
  // caller writes this, and it has no reason to be able to fail.
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(gtext_toml_value_set_inline_comment(root, nullptr), GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_value_set_trailing_comment(root, nullptr), GTEXT_TOML_OK);
  gtext_toml_free(root);
}

TEST(TomlComments, TheAccessorsAndSettersRefuseNullTheWayEverythingElseDoes) {
  EXPECT_EQ(gtext_toml_value_leading_comment(nullptr), nullptr);
  EXPECT_EQ(gtext_toml_value_inline_comment(nullptr), nullptr);
  EXPECT_EQ(gtext_toml_value_trailing_comment(nullptr), nullptr);
  EXPECT_EQ(gtext_toml_value_set_leading_comment(nullptr, " x"),
      GTEXT_TOML_E_INVALID);
  EXPECT_EQ(
      gtext_toml_value_set_inline_comment(nullptr, " x"), GTEXT_TOML_E_INVALID);
  EXPECT_EQ(gtext_toml_value_set_trailing_comment(nullptr, " x"),
      GTEXT_TOML_E_INVALID);
}

// --------------------------------------------------------------------------
// What the writer refuses
// --------------------------------------------------------------------------

TEST(TomlComments, AControlCharacterInACommentIsRefused) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Value * one = gtext_toml_new_integer(nullptr, 1);
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1, one), GTEXT_TOML_OK);
  // The rule the parser applies to a comment it reads, applied to one a caller
  // wrote: tab is allowed and nothing else below U+0020 is.
  EXPECT_EQ(gtext_toml_value_set_leading_comment(one, " bell\a"),
      GTEXT_TOML_OK);
  EXPECT_EQ(refused_write(root, nullptr), GTEXT_TOML_E_CONTROL);
  EXPECT_EQ(gtext_toml_value_set_leading_comment(one, " delete\x7f"),
      GTEXT_TOML_OK);
  EXPECT_EQ(refused_write(root, nullptr), GTEXT_TOML_E_CONTROL);
  EXPECT_EQ(gtext_toml_value_set_leading_comment(one, " tab\there"),
      GTEXT_TOML_OK);
  EXPECT_EQ(written(root, nullptr), "# tab\there\na = 1\n");
  gtext_toml_free(root);
}

TEST(TomlComments, ACommentThatIsNotUtf8IsRefused) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Value * one = gtext_toml_new_integer(nullptr, 1);
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1, one), GTEXT_TOML_OK);
  // The same refusal the writer already makes for a string that is not UTF-8,
  // and for the same reason: a document nothing can read, written without
  // saying so.
  EXPECT_EQ(gtext_toml_value_set_inline_comment(one, " \xff\xfe"),
      GTEXT_TOML_OK);
  EXPECT_EQ(refused_write(root, nullptr), GTEXT_TOML_E_BAD_UNICODE);
  gtext_toml_free(root);
}

TEST(TomlComments, ALineBreakInAnInlineCommentIsRefusedRatherThanSplit) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Value * one = gtext_toml_new_integer(nullptr, 1);
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1, one), GTEXT_TOML_OK);
  // Splitting it would put half a comment on a line of its own, where the next
  // parse reads it as the *next* statement's comment. A leading comment is
  // allowed to be several lines, because there it means what it says.
  EXPECT_EQ(gtext_toml_value_set_inline_comment(one, " one\n two"),
      GTEXT_TOML_OK);
  EXPECT_EQ(refused_write(root, nullptr), GTEXT_TOML_E_UNREPRESENTABLE);
  EXPECT_EQ(gtext_toml_value_set_inline_comment(one, nullptr), GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_value_set_leading_comment(one, " one\n two"),
      GTEXT_TOML_OK);
  EXPECT_EQ(written(root, nullptr), "# one\n# two\na = 1\n");
  gtext_toml_free(root);
}

TEST(TomlComments, ACommentOnAValueInsideBracesIsRefused) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Value * inner = gtext_toml_new_table(nullptr);
  ASSERT_NE(inner, nullptr);
  ASSERT_EQ(gtext_toml_value_set_inline(inner, true), GTEXT_TOML_OK);
  GTEXT_TOML_Value * one = gtext_toml_new_integer(nullptr, 1);
  ASSERT_EQ(gtext_toml_table_set(inner, "b", 1, one), GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1, inner), GTEXT_TOML_OK);
  EXPECT_EQ(written(root, nullptr), "a = { b = 1 }\n");
  // `b`'s comment would have to go inside the braces, and there is no line
  // there to put it on. Refused rather than dropped: a caller who set it is
  // told it cannot be written, which is the difference between this and a
  // writer that quietly produces a different document.
  EXPECT_EQ(gtext_toml_value_set_leading_comment(one, " about b"),
      GTEXT_TOML_OK);
  EXPECT_EQ(refused_write(root, nullptr), GTEXT_TOML_E_UNREPRESENTABLE);
  gtext_toml_free(root);
}

TEST(TomlComments, ACommentOnAnArrayElementIsRefusedForTheSameReason) {
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Value * array = gtext_toml_new_array(nullptr);
  ASSERT_NE(array, nullptr);
  GTEXT_TOML_Value * one = gtext_toml_new_integer(nullptr, 1);
  ASSERT_EQ(gtext_toml_array_append(array, one), GTEXT_TOML_OK);
  ASSERT_EQ(gtext_toml_table_set(root, "a", 1, array), GTEXT_TOML_OK);
  EXPECT_EQ(gtext_toml_value_set_inline_comment(one, " about one"),
      GTEXT_TOML_OK);
  EXPECT_EQ(refused_write(root, nullptr), GTEXT_TOML_E_UNREPRESENTABLE);
  gtext_toml_free(root);
}

TEST(TomlComments, TheInlineTableStyleRefusesADocumentWhoseCommentsItCannotPlace) {
  // The one refusal a *document* can reach, and the reason the corpus's
  // comments mode asks it of the as-read and header styles only. Nothing is
  // wrong with the document or with the style; the two cannot be had together,
  // and the caller is told which.
  GTEXT_TOML_Value * root = read_kept("[t]\n# about b\nb = 1\n");
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Write_Options opts = gtext_toml_write_options_default();
  opts.table_style = GTEXT_TOML_TABLE_STYLE_INLINE;
  EXPECT_EQ(refused_write(root, &opts), GTEXT_TOML_E_UNREPRESENTABLE);
  // The same document with the comment on the statement that stays a statement
  // writes perfectly well, so it is the comment's position and not the style
  // that decides.
  opts.table_style = GTEXT_TOML_TABLE_STYLE_HEADERS;
  EXPECT_EQ(written(root, &opts), "[t]\n# about b\nb = 1\n");
  gtext_toml_free(root);
}

TEST(TomlComments, ATableWrittenInlineMayStillCarryItsOwnComments) {
  // Its comments are the statement's - the `a = { ... }` line - so they are
  // writable, and only its *children's* are not. The distinction is the whole
  // rule, and getting it wrong in the safe direction refuses documents that
  // are fine.
  GTEXT_TOML_Value * root = read_kept("# about a\na = { b = 1 } # beside a\n");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(written(root, nullptr), "# about a\na = { b = 1 } # beside a\n");
  gtext_toml_free(root);
}

TEST(TomlComments, ATrailingCommentOnASubTableIsWrittenAfterItsBlock) {
  // Set by a caller, since a parse only ever puts one on the root. Written
  // after everything that table contains - and documented as not surviving the
  // trip, because the next parse reads it as the following header's leading
  // comment, which is what it now is.
  GTEXT_TOML_Value * root = read_kept("[t]\nb = 1\n[u]\nc = 2\n");
  ASSERT_NE(root, nullptr);
  GTEXT_TOML_Value * t = const_cast<GTEXT_TOML_Value *>(key(root, "t"));
  ASSERT_EQ(gtext_toml_value_set_trailing_comment(t, " end of t"),
      GTEXT_TOML_OK);
  const std::string out = written(root, nullptr);
  EXPECT_EQ(out, "[t]\nb = 1\n# end of t\n\n[u]\nc = 2\n");
  GTEXT_TOML_Value * again = read_kept(out);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(gtext_toml_value_trailing_comment(key(again, "t")), nullptr);
  EXPECT_EQ(std::string(gtext_toml_value_leading_comment(key(again, "u"))),
      " end of t");
  gtext_toml_free(again);
  gtext_toml_free(root);
}
