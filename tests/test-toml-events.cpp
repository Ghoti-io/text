/**
 * @file
 *
 * The event walk: what a document says, in the order it says it.
 *
 * gtext_toml_read_events() is the same parser as gtext_toml_parse() with a
 * callback, which is what makes most of this file cheap to justify: the corpus
 * already scores the grammar, so what is left is the *stream* - the order,
 * the positions, the key paths, and where the walk stops.
 *
 * Three of these tests exist because of a defect the shape of the code invites,
 * rather than one found by running it:
 *
 *   - **A value's position is where the value began.** The parser's line
 *     counter moves while a multi-line string is being scanned, so the obvious
 *     implementation - read `ctx->line` when there is a value to report -
 *     names the line the string *ended* on. Every event here is therefore
 *     marked before its token is scanned, and `AMultiLineStringIsReportedWhere
 *     ItStarted` is the case that separates the two.
 *   - **A refused statement produces no event.** Events are emitted after the
 *     check that made the statement legal, so a consumer never sees a
 *     duplicate key or a redefinition as though it had happened.
 *   - **The callback's refusal is not the document's.** A callback that stops
 *     the walk gets its own status back out, and the document is not thereby
 *     invalid.
 *
 * The corpus has a `events` mode that rebuilds each valid case's document from
 * the event stream alone and scores it against the suite's expectation, which
 * is the completeness claim this file does not try to make by hand: 709 cases
 * say the stream carries everything the tree does. What it cannot say is
 * anything about the comments, which have no place in a tagged-JSON
 * expectation - those are in tests/test-toml-comments.cpp.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>
#include <vector>
#include <gtest/gtest.h>

#include <ghoti.io/text/toml.h>

namespace {

struct Seen {
  std::string compact;   // The stream as one line of text.
  std::string positions; // The same, each event with its line and column.
  size_t count = 0;
  // Stop the walk once this many events have been delivered, or never at 0.
  size_t stop_after = 0;
  GTEXT_TOML_Status stop_with = GTEXT_TOML_OK;
};

// One event as text. A key path is joined with dots, which is lossy for a key
// containing a dot and is the reason KeyPartsAreDecodedAndCountedSeparately
// looks at the parts instead of at this.
std::string spell(const GTEXT_TOML_Event * e) {
  std::string out;
  switch (e->type) {
    case GTEXT_TOML_EVT_TABLE: out = "T("; break;
    case GTEXT_TOML_EVT_ARRAY_TABLE: out = "AT("; break;
    case GTEXT_TOML_EVT_KEY: out = "K("; break;
    case GTEXT_TOML_EVT_VALUE: out = "V"; break;
    case GTEXT_TOML_EVT_ARRAY_BEGIN: return "[";
    case GTEXT_TOML_EVT_ARRAY_END: return "]";
    case GTEXT_TOML_EVT_INLINE_TABLE_BEGIN: return "{";
    case GTEXT_TOML_EVT_INLINE_TABLE_END: return "}";
    case GTEXT_TOML_EVT_COMMENT:
      out = e->comment_own_line ? "#own(" : "#end(";
      out.append(e->comment, e->comment_len);
      return out + ")";
  }
  if (e->type == GTEXT_TOML_EVT_VALUE) {
    switch (gtext_toml_value_type(e->value)) {
      case GTEXT_TOML_STRING: {
        size_t len = 0;
        const char * s = gtext_toml_value_string(e->value, &len);
        return "Vs(" + std::string(s, len) + ")";
      }
      case GTEXT_TOML_INTEGER: {
        int64_t i = 0;
        gtext_toml_value_integer(e->value, &i);
        return "Vi(" + std::to_string(i) + ")";
      }
      case GTEXT_TOML_FLOAT: return "Vf";
      case GTEXT_TOML_BOOLEAN: {
        bool b = false;
        gtext_toml_value_boolean(e->value, &b);
        return b ? "Vb(true)" : "Vb(false)";
      }
      case GTEXT_TOML_DATETIME: return "Vd";
      default: return "V?";
    }
  }
  for (size_t i = 0; i < e->key.count; ++i) {
    if (i) out += ".";
    out.append(e->key.parts[i].data, e->key.parts[i].len);
  }
  return out + ")";
}

GTEXT_TOML_Status collect(
    void * user, const GTEXT_TOML_Event * e, GTEXT_TOML_Error * err) {
  (void) err;
  Seen * seen = static_cast<Seen *>(user);
  const std::string text = spell(e);
  if (!seen->compact.empty()) {
    seen->compact += " ";
    seen->positions += " ";
  }
  seen->compact += text;
  seen->positions += text + "@" + std::to_string(e->line) + ":"
      + std::to_string(e->col);
  seen->count++;
  if (seen->stop_after && seen->count >= seen->stop_after) {
    return seen->stop_with;
  }
  return GTEXT_TOML_OK;
}

GTEXT_TOML_Parse_Options with_comments(bool on) {
  GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
  opts.retain_comments = on;
  return opts;
}

// Walk `text`, returning what was seen; `status` gets the result.
Seen walk(const std::string & text, const GTEXT_TOML_Parse_Options * opts,
    GTEXT_TOML_Status * status) {
  Seen seen;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Status got = gtext_toml_read_events(
      text.data(), text.size(), opts, collect, &seen, &err);
  if (status) *status = got;
  gtext_toml_error_free(&err);
  return seen;
}

// The stream of a document that must be read successfully.
std::string stream_of(const std::string & text) {
  GTEXT_TOML_Status status = GTEXT_TOML_E_STATE;
  GTEXT_TOML_Parse_Options opts = with_comments(true);
  Seen seen = walk(text, &opts, &status);
  EXPECT_EQ(status, GTEXT_TOML_OK) << "[" << text << "]";
  return seen.compact;
}

} // namespace

// --------------------------------------------------------------------------
// The shape of the stream
// --------------------------------------------------------------------------

TEST(TomlEvents, EveryStatementKindIsReportedInTheOrderWritten) {
  EXPECT_EQ(stream_of("a = 1\n[t]\nb = 2\n[[u]]\nc = 3\n"),
      "K(a) Vi(1) T(t) K(b) Vi(2) AT(u) K(c) Vi(3)");
}

TEST(TomlEvents, AKeyIsFollowedByExactlyOneValueHoweverNested) {
  EXPECT_EQ(stream_of("a = [1, [2, 3], {b = 4}]\n"),
      "K(a) [ Vi(1) [ Vi(2) Vi(3) ] { K(b) Vi(4) } ]");
  // An empty container is a BEGIN and an END and nothing between them, which
  // is the case a consumer that counts values has to get right.
  EXPECT_EQ(stream_of("a = []\nb = {}\n"), "K(a) [ ] K(b) { }");
}

TEST(TomlEvents, ADottedKeyArrivesAsOneKeyOfSeveralParts) {
  // Not as three keys, and not as a table event: `a.b.c = 1` defines one key.
  // A consumer told about `a` and `b` as tables could not tell this document
  // from `[a.b]` + `c = 1`, which TOML does distinguish - the second refuses a
  // later `[a]` and this one does not.
  EXPECT_EQ(stream_of("a.b.c = 1\n"), "K(a.b.c) Vi(1)");
  EXPECT_EQ(stream_of("x = { a.b = 1 }\n"), "K(x) { K(a.b) Vi(1) }");
}

TEST(TomlEvents, AHeaderCarriesItsWholePathAndNotJustItsLastPart) {
  EXPECT_EQ(stream_of("[a.b.c]\n"), "T(a.b.c)");
  EXPECT_EQ(stream_of("[[a.b]]\n"), "AT(a.b)");
}

TEST(TomlEvents, AReopenedTableIsReportedAgain) {
  // `[a]` after `[a.b]` is the specification's super-table-afterwards case:
  // the table already exists, and the file says so twice. The stream is the
  // file, so it says so twice too - a formatter that dropped the second would
  // move `x = 1` into `[a.b]`.
  EXPECT_EQ(stream_of("[a.b]\n[a]\nx = 1\n"), "T(a.b) T(a) K(x) Vi(1)");
}

TEST(TomlEvents, KeyPartsAreDecodedAndCountedSeparately) {
  Seen seen;
  GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  // Three parts: a quoted key holding a dot is one part, and an escape in a
  // quoted key is decoded before it gets here - key identity in TOML is the
  // decoded string.
  const std::string doc = "a.'b.c'.\"d\\u0000e\" = 1\n";
  struct Parts {
    std::vector<std::string> parts;
  } out;
  auto grab = [](void * user, const GTEXT_TOML_Event * e,
                  GTEXT_TOML_Error *) -> GTEXT_TOML_Status {
    if (e->type == GTEXT_TOML_EVT_KEY) {
      Parts * p = static_cast<Parts *>(user);
      for (size_t i = 0; i < e->key.count; ++i) {
        p->parts.push_back(
            std::string(e->key.parts[i].data, e->key.parts[i].len));
      }
    }
    return GTEXT_TOML_OK;
  };
  EXPECT_EQ(gtext_toml_read_events(
                doc.data(), doc.size(), &opts, grab, &out, &err),
      GTEXT_TOML_OK);
  gtext_toml_error_free(&err);
  ASSERT_EQ(out.parts.size(), 3u);
  EXPECT_EQ(out.parts[0], "a");
  EXPECT_EQ(out.parts[1], "b.c");
  // A NUL is a character like any other in a quoted key, so the length is
  // what says how long the key is. `strlen` here would say 1.
  EXPECT_EQ(out.parts[2].size(), 3u);
  EXPECT_EQ(out.parts[2], std::string("d\0e", 3));
  (void) seen;
}

// --------------------------------------------------------------------------
// Positions
// --------------------------------------------------------------------------

TEST(TomlEvents, EachEventNamesWhereItsOwnTokenStarts) {
  GTEXT_TOML_Status status = GTEXT_TOML_E_STATE;
  GTEXT_TOML_Parse_Options opts = with_comments(true);
  Seen seen = walk("a = 1\n  b = [ 2 ]\n", &opts, &status);
  EXPECT_EQ(status, GTEXT_TOML_OK);
  EXPECT_EQ(seen.positions, "K(a)@1:1 Vi(1)@1:5 K(b)@2:3 [@2:7 Vi(2)@2:9 ]@2:11");
}

TEST(TomlEvents, AMultiLineStringIsReportedWhereItStarted) {
  // The value begins on line 1 and ends on line 3, and the parser's line
  // counter is on 3 by the time the string exists. Reading the counter then
  // would put this value on line 3 and the key after it on the right line,
  // which is the combination that makes the mistake hard to notice.
  GTEXT_TOML_Status status = GTEXT_TOML_E_STATE;
  GTEXT_TOML_Parse_Options opts = with_comments(true);
  Seen seen = walk("a = \"\"\"\nx\ny\"\"\"\nb = 2\n", &opts, &status);
  EXPECT_EQ(status, GTEXT_TOML_OK);
  EXPECT_EQ(seen.positions, "K(a)@1:1 Vs(x\ny)@1:5 K(b)@4:1 Vi(2)@4:5");
}

TEST(TomlEvents, AColumnCountsCharactersAndNotBytes) {
  // The same rule GTEXT_TOML_Error uses, so that a consumer can report a
  // position the way an error does. The key is three characters and six bytes.
  GTEXT_TOML_Status status = GTEXT_TOML_E_STATE;
  GTEXT_TOML_Parse_Options opts = with_comments(true);
  Seen seen = walk("\"\xc3\xa9\xc3\xa9\xc3\xa9\" = 1\n", &opts, &status);
  EXPECT_EQ(status, GTEXT_TOML_OK);
  EXPECT_EQ(seen.positions.substr(seen.positions.find(" Vi") + 1), "Vi(1)@1:9");
}

// --------------------------------------------------------------------------
// Refusals, and stopping
// --------------------------------------------------------------------------

TEST(TomlEvents, TheWalkRefusesExactlyWhatAParseRefuses) {
  // Both directions on each document, because the two entry points are one
  // function and the way to find out whether they have drifted is to ask them
  // the same question. The list is a shape of refusal apiece: syntax, a
  // duplicate key, the redefinition rules, a bad escape, a control character,
  // and a date-time chron rejects.
  const char * documents[] = {
      "a = 1\n",
      "[a]\nb = 1\n",
      "a = \n",
      "a = 1\na = 2\n",
      "[a]\n[a]\n",
      "a = { b = 1 }\n[a.c]\nd = 2\n",
      "a = \"\\q\"\n",
      "a = \"x\ty\"\n",
      "a = 1979-02-30T00:00:00Z\n",
      "a = 07:32\n",
      "# just a comment\n",
      "",
  };
  for (const char * doc : documents) {
    GTEXT_TOML_Error perr;
    std::memset(&perr, 0, sizeof(perr));
    GTEXT_TOML_Value * root =
        gtext_toml_parse(doc, std::strlen(doc), nullptr, &perr);
    const bool parsed = root != nullptr;
    gtext_toml_free(root);

    Seen seen;
    GTEXT_TOML_Error eerr;
    std::memset(&eerr, 0, sizeof(eerr));
    GTEXT_TOML_Status walked = gtext_toml_read_events(
        doc, std::strlen(doc), nullptr, collect, &seen, &eerr);
    EXPECT_EQ(parsed, walked == GTEXT_TOML_OK) << "[" << doc << "]";
    if (!parsed) {
      // And the same refusal, not merely a refusal: the status and the
      // position both come from the one parse.
      EXPECT_EQ(perr.code, walked) << "[" << doc << "]";
      EXPECT_EQ(perr.line, eerr.line) << "[" << doc << "]";
      EXPECT_EQ(perr.col, eerr.col) << "[" << doc << "]";
    }
    gtext_toml_error_free(&perr);
    gtext_toml_error_free(&eerr);
  }
}

TEST(TomlEvents, ARefusedStatementIsNotReported) {
  // The first `a = 1` is reported and the second is not: the duplicate is
  // refused before the key event, so a consumer never sees a statement the
  // document was not allowed to make.
  GTEXT_TOML_Status status = GTEXT_TOML_OK;
  GTEXT_TOML_Parse_Options opts = with_comments(true);
  Seen seen = walk("a = 1\na = 2\n", &opts, &status);
  EXPECT_EQ(status, GTEXT_TOML_E_DUPKEY);
  EXPECT_EQ(seen.compact, "K(a) Vi(1)");
}

TEST(TomlEvents, ARefusedRedefinitionIsNotReportedEither) {
  GTEXT_TOML_Status status = GTEXT_TOML_OK;
  GTEXT_TOML_Parse_Options opts = with_comments(true);
  Seen seen = walk("[a]\nx = 1\n[a]\n", &opts, &status);
  EXPECT_EQ(status, GTEXT_TOML_E_REDEFINE);
  EXPECT_EQ(seen.compact, "T(a) K(x) Vi(1)");
}

TEST(TomlEvents, AValueRefusedAfterItsKeyLeavesTheKeyReported) {
  // The other half of the rule above, and the honest half: a statement is
  // checked in pieces, so a consumer may be told about a key whose value then
  // turns out to be malformed. The status is the verdict, not the stream.
  GTEXT_TOML_Status status = GTEXT_TOML_OK;
  GTEXT_TOML_Parse_Options opts = with_comments(true);
  Seen seen = walk("a = [1, 2\n", &opts, &status);
  EXPECT_EQ(status, GTEXT_TOML_E_BAD_TOKEN);
  EXPECT_EQ(seen.compact, "K(a) [ Vi(1) Vi(2)");
}

TEST(TomlEvents, ACallbackCanStopTheWalkAndSaysWithWhat) {
  Seen seen;
  seen.stop_after = 2;
  seen.stop_with = GTEXT_TOML_E_STATE;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  const std::string doc = "a = 1\nb = 2\nc = 3\n";
  // A perfectly good document: the status is the callback's answer, not a
  // verdict on the file.
  EXPECT_EQ(gtext_toml_read_events(
                doc.data(), doc.size(), nullptr, collect, &seen, &err),
      GTEXT_TOML_E_STATE);
  EXPECT_EQ(seen.compact, "K(a) Vi(1)");
  EXPECT_EQ(seen.count, 2u);
  // Nothing was written to the error struct, because nothing went wrong.
  EXPECT_EQ(err.code, GTEXT_TOML_OK);
  gtext_toml_error_free(&err);
}

TEST(TomlEvents, StoppingOnTheFirstEventOfEachKindIsClean) {
  // Under a sanitizer this is the test that matters: a callback that stops
  // while a container is open, or between a key and its value, leaves the
  // parser's frame stack and the half-built tree to be released on a path
  // nothing else takes.
  const std::string doc =
      "# c\n[t]\nk.j = [1, {a = \"\"\"x\ny\"\"\"}]\n[[u]]\nz = 2\n";
  for (size_t stop = 1; stop <= 16; ++stop) {
    Seen seen;
    seen.stop_after = stop;
    seen.stop_with = GTEXT_TOML_E_STATE;
    GTEXT_TOML_Parse_Options opts = with_comments(true);
    GTEXT_TOML_Error err;
    std::memset(&err, 0, sizeof(err));
    GTEXT_TOML_Status got = gtext_toml_read_events(
        doc.data(), doc.size(), &opts, collect, &seen, &err);
    // The last stop value is past the end of the stream, where the walk runs
    // to completion instead - the control that keeps this loop from passing
    // because nothing ever stopped.
    EXPECT_EQ(got, seen.count >= stop ? GTEXT_TOML_E_STATE : GTEXT_TOML_OK)
        << "stop at " << stop;
    gtext_toml_error_free(&err);
  }
}

TEST(TomlEvents, NoCallbackIsRefusedRatherThanIgnored) {
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_toml_read_events("a = 1\n", 5, nullptr, nullptr, nullptr, &err),
      GTEXT_TOML_E_INVALID);
  EXPECT_EQ(err.code, GTEXT_TOML_E_INVALID);
  gtext_toml_error_free(&err);
  // And a NULL document is refused the way a parse refuses one, rather than
  // read as an empty one.
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(
      gtext_toml_read_events(nullptr, 0, nullptr, collect, nullptr, &err),
      GTEXT_TOML_E_INVALID);
  gtext_toml_error_free(&err);
}

// --------------------------------------------------------------------------
// The options reach the walk
// --------------------------------------------------------------------------

TEST(TomlEvents, TheVersionOptionReachesTheWalk) {
  // The same three points of use as a parse, since it is the same parse. A
  // walk that read its own defaults would accept this at 1.0.0.
  const std::string doc = "a = { b = 1,\n  c = 07:32 }\n";
  GTEXT_TOML_Status status = GTEXT_TOML_OK;
  GTEXT_TOML_Parse_Options strict = gtext_toml_parse_options_default();
  walk(doc, &strict, &status);
  EXPECT_EQ(status, GTEXT_TOML_E_BAD_TOKEN);

  GTEXT_TOML_Parse_Options next = gtext_toml_parse_options_default();
  next.version = GTEXT_TOML_VERSION_1_1_0;
  Seen seen = walk(doc, &next, &status);
  EXPECT_EQ(status, GTEXT_TOML_OK);
  EXPECT_EQ(seen.compact, "K(a) { K(b) Vi(1) K(c) Vd }");
}

TEST(TomlEvents, TheDepthLimitReachesTheWalk) {
  GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
  opts.max_depth = 2;
  GTEXT_TOML_Status status = GTEXT_TOML_OK;
  walk("a = [[[1]]]\n", &opts, &status);
  EXPECT_EQ(status, GTEXT_TOML_E_DEPTH);
}

TEST(TomlEvents, CommentsAreReportedOnlyWhenAskedFor) {
  const std::string doc = "# one\na = 1 # two\n";
  EXPECT_EQ(stream_of(doc), "#own( one) K(a) Vi(1) #end( two)");
  GTEXT_TOML_Status status = GTEXT_TOML_E_STATE;
  GTEXT_TOML_Parse_Options off = with_comments(false);
  Seen seen = walk(doc, &off, &status);
  EXPECT_EQ(status, GTEXT_TOML_OK);
  EXPECT_EQ(seen.compact, "K(a) Vi(1)");
  // And the default is off, which is the setting most callers will get.
  seen = walk(doc, nullptr, &status);
  EXPECT_EQ(seen.compact, "K(a) Vi(1)");
}

TEST(TomlEvents, OwnLineIsAskedOfTheInputAndNotTrackedByTheCaller) {
  // Four positions a comment can be in, and the answer is the same question
  // each time: is everything before the `#` on this line blank. The two inside
  // the array are the pair that a flag passed down from the caller gets wrong,
  // because the caller is the same call site for both.
  EXPECT_EQ(stream_of("a = [ # after the bracket\n"
                      "  1, # after the comma\n"
                      "  # on its own\n"
                      "  2 ]\n"
                      "\t# indented, still its own\n"),
      "K(a) [ #end( after the bracket) Vi(1) #end( after the comma) "
      "#own( on its own) Vi(2) ] #own( indented, still its own)");
}

TEST(TomlEvents, AnEmptyCommentIsACommentAndNotAnAbsence) {
  // `#` with nothing after it is a comment of no characters. The distinction
  // matters on the way out: the DOM stores the empty string rather than NULL,
  // so the writer puts the `#` line back.
  EXPECT_EQ(stream_of("#\na = 1 #\n"), "#own() K(a) Vi(1) #end()");
}

TEST(TomlEvents, ACommentTextExcludesTheHashAndTheLineEnding) {
  // Verbatim otherwise - not trimmed - so that `#` followed by the text is the
  // line as written. The CR of a CRLF belongs to the newline and not to the
  // comment, which is the byte a reader most easily keeps by accident.
  EXPECT_EQ(stream_of("#   spaced   \r\na = 1\r\n"), "#own(   spaced   ) K(a) Vi(1)");
}
