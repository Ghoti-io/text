/**
 * @file
 *
 * One input, several JSON texts: GTEXT_JSON_Parse_Options::records and
 * GTEXT_JSON_Write_Options::records.
 *
 * A JSON text is one value, and this parser refused anything after it. A great
 * deal of real data is a *sequence* of values - one per line in a log, an
 * export or a network stream - and two pages of this library's own
 * documentation had claimed the streaming parser was suitable for it while it
 * was not: `{"a":1}\n{"b":2}\n` delivered the first object's events and then
 * GTEXT_JSON_E_TRAILING_GARBAGE.
 *
 * There is no single specification for that format, which is why `records` is
 * an enumeration and not a flag. Three readings are in use and they disagree
 * about inputs that occur:
 *
 *   - WHITESPACE accepts any white space between values, including none, so
 *     `{"a":1}{"b":2}` and `1 2` are two records each;
 *   - LINE is NDJSON / JSON Lines - at least one LF between records, and no
 *     line end *inside* one;
 *   - SEQ is RFC 7464, where an RS (0x1E) introduces every record.
 *
 * **What the tests here are shaped by.** A mode that merely accepts more is
 * easy to assert and easy to get wrong in the direction that matters, so every
 * mode is tested in both directions: the inputs it must accept, *and* the
 * inputs it must refuse that a weaker mode accepts. Three pairs of modes are
 * compared over the same input for exactly that reason - a test that only ever
 * feeds NDJSON to NDJSON mode cannot tell LINE from WHITESPACE.
 *
 * Every records test also runs **one byte at a time**, because a separator is
 * the one thing in this feature that can be split across a feed, and two
 * defects already fixed in json_stream.c were answers that depended on where
 * the caller's chunk boundaries fell.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include <ghoti.io/text/json.h>

namespace {

/** One event, flattened to what a test wants to compare. */
struct Ev {
  GTEXT_JSON_Event_Type type;
  std::string text; // key and string payloads, and number lexemes

  bool operator==(const Ev & o) const {
    return type == o.type && text == o.text;
  }
};

struct Capture {
  std::vector<Ev> events;
};

GTEXT_JSON_Status capture_cb(
    void * user, const GTEXT_JSON_Event * evt, GTEXT_JSON_Error * err) {
  (void)err;
  Capture * c = (Capture *)user;
  Ev e{evt->type, std::string()};
  if (evt->type == GTEXT_JSON_EVT_KEY || evt->type == GTEXT_JSON_EVT_STRING) {
    e.text.assign(evt->as.str.s, evt->as.str.len);
  }
  else if (evt->type == GTEXT_JSON_EVT_NUMBER) {
    e.text.assign(evt->as.number.s, evt->as.number.len);
  }
  c->events.push_back(e);
  return GTEXT_JSON_OK;
}

/** Result of feeding one input: the statuses and the events. */
struct StreamRun {
  GTEXT_JSON_Status feed = GTEXT_JSON_OK;
  GTEXT_JSON_Status finish = GTEXT_JSON_OK;
  std::vector<Ev> events;

  /** How many records the stream said it delivered. */
  size_t records() const {
    size_t n = 0;
    for (const Ev & e : events) {
      if (e.type == GTEXT_JSON_EVT_RECORD_END)
        n++;
    }
    return n;
  }

  bool ok() const {
    return feed == GTEXT_JSON_OK && finish == GTEXT_JSON_OK;
  }
};

/**
 * Feed @p text in chunks of @p chunk bytes (0 meaning all at once).
 *
 * The chunk size is a parameter rather than a separate test because it is the
 * axis along which a separator can be split, and asserting the same answer at
 * both extremes is the only way to say the answer does not depend on it.
 */
StreamRun feed_stream(GTEXT_JSON_Records mode, const std::string & text,
    size_t chunk = 0, const GTEXT_JSON_Parse_Options * base = nullptr) {
  GTEXT_JSON_Parse_Options opts =
      base ? *base : gtext_json_parse_options_default();
  opts.records = mode;

  Capture cap;
  StreamRun out;
  GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, capture_cb, &cap);
  if (!st) {
    out.feed = GTEXT_JSON_E_OOM;
    return out;
  }

  size_t off = 0;
  while (off < text.size() && out.feed == GTEXT_JSON_OK) {
    const size_t n =
        (chunk && chunk < text.size() - off) ? chunk : text.size() - off;
    out.feed = gtext_json_stream_feed(st, text.data() + off, n, nullptr);
    off += n;
  }
  out.finish = gtext_json_stream_finish(st, nullptr);
  out.events = cap.events;
  gtext_json_stream_free(st);
  return out;
}

/** The same input through the pull reader, which wraps the same parser. */
StreamRun feed_reader(GTEXT_JSON_Records mode, const std::string & text) {
  GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
  opts.records = mode;

  StreamRun out;
  GTEXT_JSON_Reader * r = gtext_json_reader_new(&opts);
  if (!r) {
    out.feed = GTEXT_JSON_E_OOM;
    return out;
  }
  out.feed = gtext_json_reader_feed(r, text.data(), text.size(), nullptr);
  if (out.feed == GTEXT_JSON_OK) {
    out.finish = gtext_json_reader_feed(r, nullptr, 0, nullptr);
  }
  while (true) {
    GTEXT_JSON_Event evt;
    std::memset(&evt, 0, sizeof(evt));
    const GTEXT_JSON_Status s = gtext_json_reader_next(r, &evt);
    if (s != GTEXT_JSON_OK) {
      if (s != GTEXT_JSON_E_STATE && out.finish == GTEXT_JSON_OK) {
        out.finish = s;
      }
      break;
    }
    Ev e{evt.type, std::string()};
    if (evt.type == GTEXT_JSON_EVT_KEY || evt.type == GTEXT_JSON_EVT_STRING) {
      e.text.assign(evt.as.str.s, evt.as.str.len);
    }
    else if (evt.type == GTEXT_JSON_EVT_NUMBER) {
      e.text.assign(evt.as.number.s, evt.as.number.len);
    }
    out.events.push_back(e);
  }
  gtext_json_reader_free(r);
  return out;
}

/** Records read by looping gtext_json_parse_multiple(), the DOM path. */
struct DomRun {
  std::vector<std::string> records; // each written back compactly
  GTEXT_JSON_Status stopped_with = GTEXT_JSON_OK;
  bool complete = false; // the whole input was consumed
};

std::string write_compact(const GTEXT_JSON_Value * v) {
  GTEXT_JSON_Sink sink;
  if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK)
    return std::string();
  GTEXT_JSON_Write_Options o = gtext_json_write_options_default();
  const std::string out =
      gtext_json_write_value(&sink, &o, v, nullptr) == GTEXT_JSON_OK
      ? std::string(gtext_json_sink_buffer_data(&sink),
            gtext_json_sink_buffer_size(&sink))
      : std::string();
  gtext_json_sink_buffer_free(&sink);
  return out;
}

DomRun read_dom(GTEXT_JSON_Records mode, const std::string & text) {
  GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
  opts.records = mode;

  DomRun out;
  size_t off = 0;
  while (off < text.size()) {
    size_t used = 0;
    GTEXT_JSON_Error err;
    std::memset(&err, 0, sizeof(err));
    GTEXT_JSON_Value * v = gtext_json_parse_multiple(
        text.data() + off, text.size() - off, &opts, &err, &used);
    if (!v) {
      out.stopped_with = err.code;
      gtext_json_error_free(&err);
      return out;
    }
    out.records.push_back(write_compact(v));
    gtext_json_free(v);
    if (used == 0) { // would loop for ever; a defect if it happens
      out.stopped_with = GTEXT_JSON_E_INVALID;
      return out;
    }
    off += used;
  }
  out.complete = true;
  return out;
}

/** N records written through the incremental writer. */
std::string write_incremental(const GTEXT_JSON_Write_Options & opts,
    const std::vector<std::string> & keys, GTEXT_JSON_Status * out_status) {
  GTEXT_JSON_Sink sink;
  if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) {
    *out_status = GTEXT_JSON_E_OOM;
    return std::string();
  }
  GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, &opts);
  if (!w) {
    gtext_json_sink_buffer_free(&sink);
    *out_status = GTEXT_JSON_E_OOM;
    return std::string();
  }
  GTEXT_JSON_Status st = GTEXT_JSON_OK;
  for (size_t i = 0; i < keys.size() && st == GTEXT_JSON_OK; i++) {
    st = gtext_json_writer_object_begin(w);
    if (st == GTEXT_JSON_OK) {
      st = gtext_json_writer_key(w, keys[i].data(), keys[i].size());
    }
    if (st == GTEXT_JSON_OK) {
      st = gtext_json_writer_number_i64(w, (int64_t)i);
    }
    if (st == GTEXT_JSON_OK)
      st = gtext_json_writer_object_end(w);
  }
  const GTEXT_JSON_Status fi = gtext_json_writer_finish(w, nullptr);
  *out_status = (st != GTEXT_JSON_OK) ? st : fi;
  const std::string out(
      gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
  gtext_json_writer_free(w);
  gtext_json_sink_buffer_free(&sink);
  return out;
}

/** The same N records through the whole-value writer, one call each. */
std::string write_values(const GTEXT_JSON_Write_Options & opts,
    const std::vector<std::string> & keys, GTEXT_JSON_Status * out_status) {
  GTEXT_JSON_Sink sink;
  if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) {
    *out_status = GTEXT_JSON_E_OOM;
    return std::string();
  }
  GTEXT_JSON_Status st = GTEXT_JSON_OK;
  for (size_t i = 0; i < keys.size() && st == GTEXT_JSON_OK; i++) {
    GTEXT_JSON_Value * v = gtext_json_new_object();
    gtext_json_object_put(v, keys[i].data(), keys[i].size(),
        gtext_json_new_number_i64((int64_t)i));
    st = gtext_json_write_value(&sink, &opts, v, nullptr);
    gtext_json_free(v);
  }
  *out_status = st;
  const std::string out(
      gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
  gtext_json_sink_buffer_free(&sink);
  return out;
}

const char * const NDJSON = "{\"a\":1}\n{\"b\":2}\n";
const char * const RUN_TOGETHER = "{\"a\":1}{\"b\":2}";
const char * const JSON_SEQ = "\x1e{\"a\":1}\n\x1e{\"b\":2}\n";
const char * const SPANS_LINES = "{\n\"a\": 1\n}\n";

} // namespace

/**
 * @test RecordsOff.IsExactlyWhatThisParserAlwaysDid
 *
 * The default, asserted as a default rather than assumed. A feature whose
 * "off" state is not tested is a feature that changed everything.
 */
TEST(RecordsOff, IsExactlyWhatThisParserAlwaysDid) {
  EXPECT_EQ(gtext_json_parse_options_default().records, GTEXT_JSON_RECORDS_OFF);
  EXPECT_EQ(gtext_json_parse_options_json5().records, GTEXT_JSON_RECORDS_OFF);
  EXPECT_EQ(gtext_json_write_options_default().records, GTEXT_JSON_RECORDS_OFF);

  // One document still parses, and emits no record boundary at all.
  const StreamRun one = feed_stream(GTEXT_JSON_RECORDS_OFF, "{\"a\":1}");
  EXPECT_TRUE(one.ok());
  EXPECT_EQ(one.records(), 0u) << "EVT_RECORD_END must not exist when off";

  // And a second document is still refused, at both chunk extremes.
  for (size_t chunk : {size_t{0}, size_t{1}}) {
    const StreamRun two = feed_stream(GTEXT_JSON_RECORDS_OFF, NDJSON, chunk);
    EXPECT_FALSE(two.ok()) << "chunk=" << chunk;
    EXPECT_EQ(two.records(), 0u);
    // The first record's events did arrive; it is the second that is refused.
    EXPECT_EQ(two.events.size(), 4u);
  }
}

/**
 * @test RecordsOff.TheDomParserRefusesTrailingContentInEveryMode
 *
 * gtext_json_parse() returns one value and has nowhere to put a second, so it
 * refuses trailing content whatever `records` says. That is a claim in
 * json_core.h, and a claim in a comment is not a test.
 */
TEST(RecordsOff, TheDomParserRefusesTrailingContentInEveryMode) {
  for (GTEXT_JSON_Records m :
      {GTEXT_JSON_RECORDS_OFF, GTEXT_JSON_RECORDS_WHITESPACE,
          GTEXT_JSON_RECORDS_LINE, GTEXT_JSON_RECORDS_SEQ}) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.records = m;
    GTEXT_JSON_Error err;
    std::memset(&err, 0, sizeof(err));
    GTEXT_JSON_Value * v =
        gtext_json_parse(NDJSON, std::strlen(NDJSON), &opts, &err);
    EXPECT_EQ(v, nullptr) << "mode " << (int)m;
    if (v)
      gtext_json_free(v);
    gtext_json_error_free(&err);
  }
}

/**
 * @test Records.EachModeAcceptsAndRefusesTheRightInputs
 *
 * The table that distinguishes the three modes, in both directions. Each row
 * is an input and what each mode must say about it - so a mode that quietly
 * became another mode fails here rather than passing three separate tests
 * that each only feed it what it likes.
 */
TEST(Records, EachModeAcceptsAndRefusesTheRightInputs) {
  struct Row {
    const char * what;
    std::string input;
    bool whitespace_ok;
    bool line_ok;
    bool seq_ok;
    size_t records_when_ok;
  };
  const Row rows[] = {
      {"NDJSON", NDJSON, true, true, false, 2},
      {"run together, no separator", RUN_TOGETHER, true, false, false, 2},
      {"two scalars, one space", "1 2", true, false, false, 2},
      {"three scalars, LF", "1\n2\n3\n", true, true, false, 3},
      {"a value spanning lines", SPANS_LINES, true, false, false, 1},
      {"CRLF separated", "{\"a\":1}\r\n{\"b\":2}\r\n", true, true, false, 2},
      {"blank line between", "{\"a\":1}\n\n{\"b\":2}\n", true, true, false, 2},
      {"no newline at end", "{\"a\":1}\n{\"b\":2}", true, true, false, 2},
      {"json-seq", JSON_SEQ, false, false, true, 2},
      {"json-seq, no trailing LF", "\x1e{\"a\":1}\x1e{\"b\":2}", false, false,
          true, 2},
      {"one value, no framing", "{\"a\":1}", true, true, false, 1},
  };

  for (const Row & r : rows) {
    struct Case {
      GTEXT_JSON_Records mode;
      bool expect_ok;
      const char * name;
    };
    const Case cases[] = {
        {GTEXT_JSON_RECORDS_WHITESPACE, r.whitespace_ok, "WHITESPACE"},
        {GTEXT_JSON_RECORDS_LINE, r.line_ok, "LINE"},
        {GTEXT_JSON_RECORDS_SEQ, r.seq_ok, "SEQ"},
    };
    for (const Case & c : cases) {
      // Both chunk extremes: a separator is the one thing that can be split.
      for (size_t chunk : {size_t{0}, size_t{1}}) {
        const StreamRun run = feed_stream(c.mode, r.input, chunk);
        EXPECT_EQ(run.ok(), c.expect_ok)
            << r.what << " in " << c.name << " (chunk=" << chunk
            << "), feed=" << (int)run.feed << " finish=" << (int)run.finish;
        if (c.expect_ok && run.ok()) {
          EXPECT_EQ(run.records(), r.records_when_ok)
              << r.what << " in " << c.name << " (chunk=" << chunk << ")";
        }
      }
    }
  }
}

/**
 * @test Records.WhitespaceAndLineDisagreeAboutTheSameInput
 *
 * The discriminating pair, stated on its own because it is the whole reason
 * `records` is an enumeration: if these two modes answered alike, one bit
 * would have done.
 */
TEST(Records, WhitespaceAndLineDisagreeAboutTheSameInput) {
  // Accepted by the tolerant mode, refused by the one that says "per line".
  for (const char * input : {RUN_TOGETHER, "1 2", SPANS_LINES}) {
    EXPECT_TRUE(feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, input).ok())
        << input;
    EXPECT_FALSE(feed_stream(GTEXT_JSON_RECORDS_LINE, input).ok()) << input;
  }
  // And accepted by both, so the refusals above are about the separator and
  // not about the documents.
  EXPECT_TRUE(feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, NDJSON).ok());
  EXPECT_TRUE(feed_stream(GTEXT_JSON_RECORDS_LINE, NDJSON).ok());
}

/**
 * @test Records.TheEventsSayWhereEachRecordEnded
 *
 * EVT_RECORD_END exists because the other events cannot answer this: `1 2` is
 * two records and emits two NUMBER events, which is also what `[1,2]` emits
 * between its array markers. So the full event sequence is asserted, not the
 * count.
 */
TEST(Records, TheEventsSayWhereEachRecordEnded) {
  const StreamRun two_scalars =
      feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, "1 2");
  ASSERT_TRUE(two_scalars.ok());
  const std::vector<Ev> want_scalars = {
      {GTEXT_JSON_EVT_NUMBER, "1"},
      {GTEXT_JSON_EVT_RECORD_END, ""},
      {GTEXT_JSON_EVT_NUMBER, "2"},
      {GTEXT_JSON_EVT_RECORD_END, ""},
  };
  EXPECT_EQ(two_scalars.events, want_scalars);

  // The same two NUMBER events inside one array, and one record.
  const StreamRun one_array =
      feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, "[1,2]");
  ASSERT_TRUE(one_array.ok());
  const std::vector<Ev> want_array = {
      {GTEXT_JSON_EVT_ARRAY_BEGIN, ""},
      {GTEXT_JSON_EVT_NUMBER, "1"},
      {GTEXT_JSON_EVT_NUMBER, "2"},
      {GTEXT_JSON_EVT_ARRAY_END, ""},
      {GTEXT_JSON_EVT_RECORD_END, ""},
  };
  EXPECT_EQ(one_array.events, want_array);

  // Which is the point: the NUMBER events alone are identical in the middle.
  EXPECT_NE(two_scalars.events, want_array);
}

/**
 * @test Records.ChunkBoundariesDoNotChangeTheAnswer
 *
 * Every chunk size from one byte to the whole input, for an NDJSON stream and
 * a json-seq one. Two defects in this file were answers that depended on where
 * a boundary fell, and a separator is now the thing a boundary can split.
 */
TEST(Records, ChunkBoundariesDoNotChangeTheAnswer) {
  struct Case {
    GTEXT_JSON_Records mode;
    std::string input;
    size_t records;
  };
  const Case cases[] = {
      {GTEXT_JSON_RECORDS_LINE, NDJSON, 2},
      {GTEXT_JSON_RECORDS_LINE, "1\n2\n3\n4\n", 4},
      {GTEXT_JSON_RECORDS_SEQ, JSON_SEQ, 2},
      {GTEXT_JSON_RECORDS_WHITESPACE, "[1,2] {\"a\":[{}]} 3", 3},
      {GTEXT_JSON_RECORDS_LINE, "{\"s\":\"a b\"}\n{\"s\":\"c\"}\n", 2},
  };
  for (const Case & c : cases) {
    const StreamRun whole = feed_stream(c.mode, c.input, 0);
    ASSERT_TRUE(whole.ok()) << c.input << " feed=" << (int)whole.feed
                            << " finish=" << (int)whole.finish;
    ASSERT_EQ(whole.records(), c.records) << c.input;
    for (size_t chunk = 1; chunk <= c.input.size(); chunk++) {
      const StreamRun split = feed_stream(c.mode, c.input, chunk);
      EXPECT_TRUE(split.ok())
          << c.input << " chunk=" << chunk << " feed=" << (int)split.feed
          << " finish=" << (int)split.finish;
      EXPECT_EQ(split.events, whole.events)
          << c.input << " chunk=" << chunk
          << ": the events a stream delivers must not depend on how the "
             "caller split the input";
    }
  }
}

/**
 * @test Records.AnExponentSignIsOnlyLegalRightAfterTheE
 *
 * Found by tests/fuzz/fuzz_json_records.cpp at 99,856 executions, as a verdict
 * that changed with the chunk size - and it is a defect in the streaming
 * number lexer rather than in this option. Resuming a number at a chunk
 * boundary, it took any following `+` or `-` as the exponent's sign whenever an
 * `e` had been seen and no sign had, which is also true long *after* the
 * exponent's digits. So `2e9-1` became one malformed number where the
 * whole-buffer lexer reads the two values `2e9` and `-1`.
 *
 * It was invisible before this option existed because at the top level both
 * readings were a refusal - one GTEXT_JSON_E_BAD_NUMBER, the other
 * GTEXT_JSON_E_TRAILING_GARBAGE - and inside a container both are invalid
 * either way. fuzz_json.cpp compares acceptance, not status codes, so it could
 * not see it. A mode where two adjacent values are legal is what turned a
 * difference of code into a difference of answer.
 */
TEST(Records, AnExponentSignIsOnlyLegalRightAfterTheE) {
  // Two records in the tolerant mode, at every chunk size.
  struct Case {
    std::string input;
    size_t records;
  };
  const Case cases[] = {
      {"2e9-1", 2},
      {"1e2-3", 2},
      {"2e-9 1", 2}, // a real exponent sign, still one number
      {"2e+9-1", 2},
      {"1.5-2", 2},
      {"2e9", 1},
      {"2e-9", 1},
  };
  for (const Case & c : cases) {
    const StreamRun whole =
        feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, c.input, 0);
    EXPECT_TRUE(whole.ok()) << c.input;
    EXPECT_EQ(whole.records(), c.records) << c.input;
    for (size_t chunk = 1; chunk <= c.input.size(); chunk++) {
      const StreamRun split =
          feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, c.input, chunk);
      EXPECT_TRUE(split.ok()) << c.input << " chunk=" << chunk;
      EXPECT_EQ(split.records(), c.records) << c.input << " chunk=" << chunk;
    }
  }

  /* `2e9+1` is refused, and that is not this defect: a `+` opens a value only
     with allow_leading_plus, which is off by default, so `+1` is not a record.
     The row is here because it is the one that says the fix did not go the
     other way and turn every sign into a token boundary. */
  for (size_t chunk = 0; chunk <= 5; chunk++) {
    EXPECT_FALSE(
        feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, "2e9+1", chunk).ok())
        << "chunk=" << chunk;
  }
  {
    // With allow_leading_plus it is two records, at every chunk size.
    GTEXT_JSON_Parse_Options plus = gtext_json_parse_options_default();
    plus.allow_leading_plus = true;
    for (size_t chunk = 0; chunk <= 5; chunk++) {
      const StreamRun run =
          feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, "2e9+1", chunk, &plus);
      EXPECT_TRUE(run.ok()) << "chunk=" << chunk;
      if (run.ok()) {
        EXPECT_EQ(run.records(), 2u) << "chunk=" << chunk;
      }
    }
  }

  /* And the pair that says this is about the sign's *position* rather than
     about signs: an exponent sign immediately after the `e` is still read, in
     one feed and split anywhere. */
  for (size_t chunk = 0; chunk <= 6; chunk++) {
    const StreamRun run =
        feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, "1e-2", chunk);
    EXPECT_TRUE(run.ok()) << "chunk=" << chunk;
    EXPECT_EQ(run.records(), 1u) << "chunk=" << chunk;
  }

  /* With records off the same input is refused at every chunk size, which it
     always was - but for two different reasons depending on the chunking, and
     that is what hid this. */
  for (size_t chunk = 0; chunk <= 5; chunk++) {
    EXPECT_FALSE(feed_stream(GTEXT_JSON_RECORDS_OFF, "2e9-1", chunk).ok())
        << "chunk=" << chunk;
  }
}

/**
 * @test Records.ALineModeRecordMayNotSpanLines
 *
 * The rule that makes LINE a statement about the format rather than only about
 * the separator. A `\n` *escaped* inside a string is not a line end and must
 * still be accepted, which is the pair that says the check reads bytes rather
 * than looking for the two characters.
 */
TEST(Records, ALineModeRecordMayNotSpanLines) {
  /* At every chunk size, not only in one feed. The line end is skipped during
     some token's scan, and if that token comes back incomplete the loop used to
     return before the flag was collected - so `[\n548310]` was refused at chunk
     sizes 1, 2 and 9 and accepted at 3 through 8, which is exactly the shape of
     defect this file has produced twice before. The flag is collected
     immediately after every lex now, and the fuzzer found this. */
  for (const char * spanning : {SPANS_LINES, "[1,\n2]\n", "{\"a\":1}\n{\n}\n",
           "[\n548310]", "[1,\n2]", "{\n\"a\":\n1}", "[\n\n\n1]"}) {
    const size_t n = std::strlen(spanning);
    for (size_t chunk = 0; chunk <= n; chunk++) {
      EXPECT_FALSE(feed_stream(GTEXT_JSON_RECORDS_LINE, spanning, chunk).ok())
          << "[" << spanning << "] chunk=" << chunk
          << ": a record spanning lines must be refused at every chunk size";
    }
  }

  // An escaped newline inside a string is content, not a line end.
  const StreamRun escaped =
      feed_stream(GTEXT_JSON_RECORDS_LINE, "{\"s\":\"a\\nb\"}\n");
  EXPECT_TRUE(escaped.ok())
      << "feed=" << (int)escaped.feed << " finish=" << (int)escaped.finish;
  EXPECT_EQ(escaped.records(), 1u);

  // The same documents are accepted by WHITESPACE, so the refusals above are
  // the mode's rule and not the grammar's.
  EXPECT_TRUE(feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, SPANS_LINES).ok());
  EXPECT_TRUE(feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, "[1,\n2]\n").ok());
}

/**
 * @test Records.FramingBeforeTheFirstRecordIsNotPartOfIt
 *
 * Found by tests/fuzz/fuzz_json_records.cpp, in both readers at once, at 363
 * executions from an empty corpus: the bytes ahead of a sequence were being
 * counted as part of its first value, so `\n\n0` was a record that "spans
 * lines" and LINE refused it. Leading blank lines are framing, exactly as the
 * blank lines between two records are.
 */
TEST(Records, FramingBeforeTheFirstRecordIsNotPartOfIt) {
  struct Case {
    GTEXT_JSON_Records mode;
    std::string input;
    size_t records;
  };
  const Case cases[] = {
      {GTEXT_JSON_RECORDS_LINE, "\n\n0", 1},
      {GTEXT_JSON_RECORDS_LINE, "\n\n{\"a\":1}\n", 1},
      {GTEXT_JSON_RECORDS_LINE, "\n1\n2\n", 2},
      {GTEXT_JSON_RECORDS_LINE, "\r\n1\n", 1},
      {GTEXT_JSON_RECORDS_WHITESPACE, "  1 ", 1},
      {GTEXT_JSON_RECORDS_WHITESPACE, "\n\n[1]\n", 1},
      {GTEXT_JSON_RECORDS_SEQ,
          "\n\x1e"
          "1\n",
          1},
  };
  for (const Case & c : cases) {
    for (size_t chunk : {size_t{0}, size_t{1}}) {
      const StreamRun run = feed_stream(c.mode, c.input, chunk);
      EXPECT_TRUE(run.ok())
          << "mode " << (int)c.mode << " chunk=" << chunk
          << " feed=" << (int)run.feed << " finish=" << (int)run.finish;
      if (run.ok()) {
        EXPECT_EQ(run.records(), c.records)
            << "mode " << (int)c.mode << " chunk=" << chunk;
      }
    }
    // The DOM loop has to agree, since the defect was in both.
    const DomRun dom = read_dom(c.mode, c.input);
    EXPECT_TRUE(dom.complete)
        << "mode " << (int)c.mode << " stopped with " << (int)dom.stopped_with;
    EXPECT_EQ(dom.records.size(), c.records) << "mode " << (int)c.mode;
  }
}

/**
 * @test Records.EveryValueKindCanBeTheLastRecord
 *
 * Found by tests/fuzz/fuzz_json_records.cpp at 4,558 executions: `true` as the
 * last record of a stream emitted its value event and **no** EVT_RECORD_END,
 * so a consumer counting records lost the last one. A partial keyword is kept
 * in the input buffer rather than the token buffer, so it completes in a third
 * token loop in json_stream.c that the record hook had not been added to - and
 * the two kinds that happen to use the token buffer, a number and a string,
 * were both fine, which is why the first pass of tests missed it.
 *
 * So: every value kind, as the only record and as the last of several, with
 * and without a trailing separator, because the trailing separator is what
 * makes the token complete inside the feed rather than at finish().
 */
TEST(Records, EveryValueKindCanBeTheLastRecord) {
  const char * const kinds[] = {"true", "false", "null", "12", "-1.5e3",
      "\"s\"", "[]", "{}", "[1]", "{\"a\":1}"};

  for (GTEXT_JSON_Records m :
      {GTEXT_JSON_RECORDS_WHITESPACE, GTEXT_JSON_RECORDS_LINE}) {
    for (const char * k : kinds) {
      const std::string sep =
          (m == GTEXT_JSON_RECORDS_LINE) ? std::string("\n") : std::string(" ");

      // Alone, with no separator after it: the case that failed.
      const StreamRun alone = feed_stream(m, k);
      EXPECT_TRUE(alone.ok()) << "mode " << (int)m << " [" << k << "]";
      EXPECT_EQ(alone.records(), 1u) << "mode " << (int)m << " [" << k
                                     << "] alone with no trailing separator";

      // Alone, with one: completes inside the feed instead.
      const StreamRun terminated = feed_stream(m, std::string(k) + sep);
      EXPECT_TRUE(terminated.ok()) << "mode " << (int)m << " [" << k << "]";
      EXPECT_EQ(terminated.records(), 1u)
          << "mode " << (int)m << " [" << k << "]";

      // As the last of three, which is where a consumer notices the loss.
      const std::string three = std::string("1") + sep + "2" + sep + k;
      const StreamRun last = feed_stream(m, three);
      EXPECT_TRUE(last.ok()) << "mode " << (int)m << " [" << three << "]";
      EXPECT_EQ(last.records(), 3u)
          << "mode " << (int)m << " [" << three << "]";

      // And one byte at a time, since that moves which loop finishes it.
      const StreamRun split = feed_stream(m, three, 1);
      EXPECT_TRUE(split.ok()) << "mode " << (int)m << " [" << three << "]";
      EXPECT_EQ(split.records(), 3u)
          << "mode " << (int)m << " [" << three << "] one byte at a time";

      // The DOM loop counts the same, which is the differential that found it.
      const DomRun dom = read_dom(m, three);
      EXPECT_TRUE(dom.complete) << "mode " << (int)m << " [" << three << "]";
      EXPECT_EQ(dom.records.size(), 3u) << "mode " << (int)m;
    }
  }
}

/**
 * @test Records.AnRsIsOnlyFramingInSeqMode
 *
 * The other half of that fix. Skipping RS as framing in every mode would let a
 * reader asked for NDJSON quietly strip a json-seq file's framing and report
 * records it had never checked - accepting more, in the direction where
 * accepting more loses a guarantee. An RS is not JSON white space, so outside
 * GTEXT_JSON_RECORDS_SEQ it must reach the parser and be refused.
 */
TEST(Records, AnRsIsOnlyFramingInSeqMode) {
  for (GTEXT_JSON_Records m : {GTEXT_JSON_RECORDS_OFF,
           GTEXT_JSON_RECORDS_WHITESPACE, GTEXT_JSON_RECORDS_LINE}) {
    for (size_t chunk : {size_t{0}, size_t{1}}) {
      EXPECT_FALSE(feed_stream(m, JSON_SEQ, chunk).ok())
          << "mode " << (int)m << " chunk=" << chunk
          << " accepted json-seq framing";
    }
    EXPECT_FALSE(read_dom(m, JSON_SEQ).complete) << "mode " << (int)m;
  }
  // And SEQ accepts it, so the refusals above are about the mode.
  EXPECT_TRUE(feed_stream(GTEXT_JSON_RECORDS_SEQ, JSON_SEQ).ok());
  EXPECT_TRUE(read_dom(GTEXT_JSON_RECORDS_SEQ, JSON_SEQ).complete);
}

/**
 * @test Records.LineModeRefusesTheOptionsItCouldNotPolice
 *
 * Three of them. allow_unescaped_controls and allow_line_continuations each
 * let a line end reach the inside of a *string*, where the lexer's whitespace
 * skip never sees it. allow_comments admits `//`, which is terminated by a
 * line end, so a comment between two records reads as the next record spanning
 * lines - found by tests/fuzz/fuzz_json_records.cpp as a verdict that changed
 * with the chunk size, because a comment split across feeds is resumed and the
 * line end is collected at a different moment.
 *
 * All three are refused rather than half-honoured. The alternative for
 * comments would be a second comment skipper inside json_stream.c with its own
 * chunk-boundary state, which is one rule written twice - the shape of two
 * defects already fixed in that file. GTEXT_JSON_RECORDS_WHITESPACE accepts
 * comments between records and makes no claim this mode would have to break,
 * which the last block here asserts so that the refusal is a routing decision
 * rather than a loss.
 */
TEST(Records, LineModeRefusesTheOptionsItCouldNotPolice) {
  for (int which = 0; which < 3; which++) {
    GTEXT_JSON_Parse_Options base = gtext_json_parse_options_default();
    if (which == 0)
      base.allow_unescaped_controls = true;
    else if (which == 1)
      base.allow_line_continuations = true;
    else
      base.allow_comments = true;

    const StreamRun line =
        feed_stream(GTEXT_JSON_RECORDS_LINE, NDJSON, 0, &base);
    EXPECT_EQ(line.feed, GTEXT_JSON_E_INVALID) << "which=" << which;

    // Even with no bytes at all, because finish() is reached without feed().
    const StreamRun empty = feed_stream(GTEXT_JSON_RECORDS_LINE, "", 0, &base);
    EXPECT_EQ(empty.finish, GTEXT_JSON_E_INVALID) << "which=" << which;

    // The other two modes make no claim about line ends and are unaffected.
    EXPECT_TRUE(
        feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, NDJSON, 0, &base).ok())
        << "which=" << which;

    // And the DOM loop refuses the same combination, rather than one entry
    // point being stricter than the other.
    base.records = GTEXT_JSON_RECORDS_LINE;
    size_t used = 0;
    GTEXT_JSON_Error err;
    std::memset(&err, 0, sizeof(err));
    GTEXT_JSON_Value * v = gtext_json_parse_multiple(
        NDJSON, std::strlen(NDJSON), &base, &err, &used);
    EXPECT_EQ(v, nullptr) << "which=" << which;
    EXPECT_EQ(err.code, GTEXT_JSON_E_INVALID) << "which=" << which;
    if (v)
      gtext_json_free(v);
    gtext_json_error_free(&err);
  }

  /* And what the refusal routes a caller *to*: comments between records, in
     the mode that can take them, at every chunk size. A refusal that left no
     way to do the thing would be a loss rather than a decision. */
  GTEXT_JSON_Parse_Options commented = gtext_json_parse_options_default();
  commented.allow_comments = true;
  for (const char * input :
      {"//c\n1\n2\n", "/*c*/1 2", "1 //x\n2", "1 /*x*/ 2"}) {
    for (size_t chunk : {size_t{0}, size_t{1}, size_t{3}}) {
      const StreamRun run =
          feed_stream(GTEXT_JSON_RECORDS_WHITESPACE, input, chunk, &commented);
      EXPECT_TRUE(run.ok()) << input << " chunk=" << chunk;
      if (run.ok()) {
        EXPECT_EQ(run.records(), 2u) << input << " chunk=" << chunk;
      }
    }
  }
}

/**
 * @test Records.SeqModeRefusesComments
 *
 * An RS is the byte that makes RFC 7464's framing unambiguous because it cannot
 * occur inside a JSON text - but it can occur inside a *comment*, and a comment
 * is an extension to the text rather than to the framing. With both on, a `//`
 * comment running to the end of the input swallows every RS after it, so the
 * streaming parser reads the rest as one comment where a reader slicing on RS
 * reads several records. Found by tests/fuzz/fuzz_json_records.cpp as a
 * disagreement between the two readers, and refused rather than resolved
 * arbitrarily: RFC 7464's grammar has no comments in it.
 */
TEST(Records, SeqModeRefusesComments) {
  GTEXT_JSON_Parse_Options base = gtext_json_parse_options_default();
  base.allow_comments = true;

  const StreamRun seq = feed_stream(GTEXT_JSON_RECORDS_SEQ, JSON_SEQ, 0, &base);
  EXPECT_EQ(seq.feed, GTEXT_JSON_E_INVALID);

  // With no bytes at all too, since finish() is reached without feed().
  const StreamRun empty = feed_stream(GTEXT_JSON_RECORDS_SEQ, "", 0, &base);
  EXPECT_EQ(empty.finish, GTEXT_JSON_E_INVALID);

  // And the DOM loop refuses the same combination.
  base.records = GTEXT_JSON_RECORDS_SEQ;
  size_t used = 0;
  GTEXT_JSON_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_JSON_Value * v = gtext_json_parse_multiple(
      JSON_SEQ, std::strlen(JSON_SEQ), &base, &err, &used);
  EXPECT_EQ(v, nullptr);
  EXPECT_EQ(err.code, GTEXT_JSON_E_INVALID);
  if (v)
    gtext_json_free(v);
  gtext_json_error_free(&err);

  // Without comments, SEQ reads it; so the refusal is about the combination.
  EXPECT_TRUE(feed_stream(GTEXT_JSON_RECORDS_SEQ, JSON_SEQ).ok());
}

/**
 * @test Records.SeqSkipsFramingThatIntroducesNothing
 *
 * `RS RS` is an RS introducing no record, and RFC 7464 has a reader discard a
 * truncated element rather than fail on it. Both readers skip it, and they have
 * to agree about that, which is how the fuzzer found the case: run of RS bytes,
 * trailing RS at the end of input, and framing before the first record.
 */
TEST(Records, SeqSkipsFramingThatIntroducesNothing) {
  struct Case {
    std::string input;
    size_t records;
  };
  const Case cases[] = {
      {"\x1e"
       "1\x1e"
       "2\n",
          2},
      {"\x1e\x1e"
       "1\n",
          1},
      {"\x1e"
       "1\x1e\x1e",
          1},
      {"\x1e\x1e\x1e"
       "1\x1e\x1e\x1e"
       "2\x1e\x1e",
          2},
      {"\n\x1e"
       "1\n",
          1},
  };
  for (const Case & c : cases) {
    for (size_t chunk : {size_t{0}, size_t{1}}) {
      const StreamRun run = feed_stream(GTEXT_JSON_RECORDS_SEQ, c.input, chunk);
      EXPECT_TRUE(run.ok()) << "chunk=" << chunk << " feed=" << (int)run.feed
                            << " finish=" << (int)run.finish;
      if (run.ok()) {
        EXPECT_EQ(run.records(), c.records) << "chunk=" << chunk;
      }
    }
    const DomRun dom = read_dom(GTEXT_JSON_RECORDS_SEQ, c.input);
    EXPECT_TRUE(dom.complete) << "stopped with " << (int)dom.stopped_with;
    EXPECT_EQ(dom.records.size(), c.records);
  }
}

/**
 * @test Records.TheDomLoopAndTheStreamAgree
 *
 * Two entry points read this format - gtext_json_parse_multiple() in a loop and
 * the streaming parser - and they are two implementations of one rule, so they
 * are compared rather than tested separately. The count of records and the
 * verdict both have to match.
 */
TEST(Records, TheDomLoopAndTheStreamAgree) {
  struct Case {
    GTEXT_JSON_Records mode;
    std::string input;
  };
  const Case cases[] = {
      {GTEXT_JSON_RECORDS_LINE, NDJSON},
      {GTEXT_JSON_RECORDS_LINE, RUN_TOGETHER},
      {GTEXT_JSON_RECORDS_LINE, SPANS_LINES},
      {GTEXT_JSON_RECORDS_LINE, "1\n2\n3\n"},
      {GTEXT_JSON_RECORDS_LINE, "{\"a\":1}\n{\"b\":2}"},
      {GTEXT_JSON_RECORDS_WHITESPACE, NDJSON},
      {GTEXT_JSON_RECORDS_WHITESPACE, RUN_TOGETHER},
      {GTEXT_JSON_RECORDS_WHITESPACE, "1 2"},
      {GTEXT_JSON_RECORDS_WHITESPACE, SPANS_LINES},
      {GTEXT_JSON_RECORDS_SEQ, JSON_SEQ},
      {GTEXT_JSON_RECORDS_SEQ, NDJSON},
      {GTEXT_JSON_RECORDS_SEQ,
          "\x1e[1,2]\n\x1e"
          "3\n"},
  };
  for (const Case & c : cases) {
    const StreamRun stream = feed_stream(c.mode, c.input);
    const DomRun dom = read_dom(c.mode, c.input);
    EXPECT_EQ(stream.ok(), dom.complete)
        << "mode " << (int)c.mode << " on [" << c.input << "]: the stream said "
        << (stream.ok() ? "ok" : "no") << " and the DOM loop said "
        << (dom.complete ? "ok" : "no");
    if (stream.ok() && dom.complete) {
      EXPECT_EQ(stream.records(), dom.records.size())
          << "mode " << (int)c.mode << " on [" << c.input << "]";
    }
  }
}

/**
 * @test Records.ThePullReaderDeliversTheBoundaryToo
 *
 * The pull reader wraps the streaming parser and copies every event into its
 * queue, so a new event type reaches it only if the copy covers it. A test
 * here rather than trust, because the queue is a second implementation of
 * "what an event is".
 */
TEST(Records, ThePullReaderDeliversTheBoundaryToo) {
  const StreamRun pushed = feed_stream(GTEXT_JSON_RECORDS_LINE, NDJSON);
  const StreamRun pulled = feed_reader(GTEXT_JSON_RECORDS_LINE, NDJSON);
  ASSERT_TRUE(pushed.ok());
  EXPECT_EQ(pulled.feed, GTEXT_JSON_OK);
  EXPECT_EQ(pulled.events, pushed.events)
      << "the pull reader must deliver exactly what the callback saw";
  EXPECT_EQ(pulled.records(), 2u);
}

/**
 * @test RecordsWriter.ASecondTopLevelValueWasInvalidJsonAndSaidOK
 *
 * The defect this option was built on top of: the incremental writer accepted
 * a second top-level value and wrote `{"a":1}{"b":2}` - not JSON, refused by
 * this library's own parser - while every call and finish() returned
 * GTEXT_JSON_OK. With records off it is now GTEXT_JSON_E_STATE.
 */
TEST(RecordsWriter, ASecondTopLevelValueWasInvalidJsonAndSaidOK) {
  GTEXT_JSON_Sink sink;
  ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
  GTEXT_JSON_Write_Options o = gtext_json_write_options_default();
  GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, &o);
  ASSERT_NE(w, nullptr);

  ASSERT_EQ(gtext_json_writer_object_begin(w), GTEXT_JSON_OK);
  ASSERT_EQ(gtext_json_writer_key(w, "a", 1), GTEXT_JSON_OK);
  ASSERT_EQ(gtext_json_writer_number_i64(w, 1), GTEXT_JSON_OK);
  ASSERT_EQ(gtext_json_writer_object_end(w), GTEXT_JSON_OK);

  EXPECT_EQ(gtext_json_writer_object_begin(w), GTEXT_JSON_E_STATE)
      << "a second top-level value with records off must be refused";

  // What was written stays written and is the one valid document.
  const std::string out(
      gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
  EXPECT_EQ(out, "{\"a\":1}");
  gtext_json_writer_free(w);
  gtext_json_sink_buffer_free(&sink);

  // Every value kind, not only an object, since the refusal is in one place
  // and a test of one kind would not say that.
  struct Kind {
    const char * name;
    GTEXT_JSON_Status (*second)(GTEXT_JSON_Writer *);
  };
  const Kind kinds[] = {
      {"null", [](GTEXT_JSON_Writer * x) { return gtext_json_writer_null(x); }},
      {"array",
          [](GTEXT_JSON_Writer * x) {
            return gtext_json_writer_array_begin(x);
          }},
      {"object",
          [](GTEXT_JSON_Writer * x) {
            return gtext_json_writer_object_begin(x);
          }},
  };
  for (const Kind & k : kinds) {
    GTEXT_JSON_Sink s2;
    ASSERT_EQ(gtext_json_sink_buffer(&s2), GTEXT_JSON_OK);
    GTEXT_JSON_Write_Options o2 = gtext_json_write_options_default();
    GTEXT_JSON_Writer * w2 = gtext_json_writer_new(s2, &o2);
    ASSERT_NE(w2, nullptr);
    ASSERT_EQ(gtext_json_writer_null(w2), GTEXT_JSON_OK);
    EXPECT_EQ(k.second(w2), GTEXT_JSON_E_STATE) << k.name;
    gtext_json_writer_free(w2);
    gtext_json_sink_buffer_free(&s2);
  }
}

/**
 * @test RecordsWriter.BothWritersFrameRecordsIdentically
 *
 * The same differential that found the separator defect in the first place:
 * N records through gtext_json_writer_* and N through gtext_json_write_value()
 * must be the same bytes. The whole-value writer is stateless and frames each
 * call; the incremental one defers the terminator to the next record or to
 * finish(). That those two strategies agree is the property.
 */
TEST(RecordsWriter, BothWritersFrameRecordsIdentically) {
  const std::vector<std::string> keys = {"a", "b", "c"};
  for (GTEXT_JSON_Records m : {GTEXT_JSON_RECORDS_WHITESPACE,
           GTEXT_JSON_RECORDS_LINE, GTEXT_JSON_RECORDS_SEQ}) {
    for (const char * nl : {"\n", "\r\n"}) {
      GTEXT_JSON_Write_Options o = gtext_json_write_options_default();
      o.records = m;
      o.newline = nl;

      GTEXT_JSON_Status si = GTEXT_JSON_OK;
      GTEXT_JSON_Status sv = GTEXT_JSON_OK;
      const std::string inc = write_incremental(o, keys, &si);
      const std::string val = write_values(o, keys, &sv);
      EXPECT_EQ(si, GTEXT_JSON_OK) << "mode " << (int)m;
      EXPECT_EQ(sv, GTEXT_JSON_OK) << "mode " << (int)m;
      EXPECT_EQ(inc, val) << "mode " << (int)m << " newline "
                          << (nl[0] == '\r' ? "CRLF" : "LF");
    }
  }
}

/**
 * @test RecordsWriter.TheBytesAreExactlyThese
 *
 * The framing written out in full, because "both writers agree" is satisfied
 * by two writers that are wrong in the same way.
 */
TEST(RecordsWriter, TheBytesAreExactlyThese) {
  const std::vector<std::string> keys = {"a", "b"};
  struct Want {
    GTEXT_JSON_Records mode;
    const char * bytes;
  };
  const Want wants[] = {
      {GTEXT_JSON_RECORDS_WHITESPACE, "{\"a\":0}\n{\"b\":1}\n"},
      {GTEXT_JSON_RECORDS_LINE, "{\"a\":0}\n{\"b\":1}\n"},
      {GTEXT_JSON_RECORDS_SEQ, "\x1e{\"a\":0}\n\x1e{\"b\":1}\n"},
  };
  for (const Want & want : wants) {
    GTEXT_JSON_Write_Options o = gtext_json_write_options_default();
    o.records = want.mode;
    GTEXT_JSON_Status st = GTEXT_JSON_OK;
    EXPECT_EQ(write_incremental(o, keys, &st), std::string(want.bytes))
        << "mode " << (int)want.mode;
    EXPECT_EQ(st, GTEXT_JSON_OK);
  }
}

/**
 * @test RecordsWriter.ARecordsModeSubsumesTrailingNewline
 *
 * Both are requests for a line end after the last value, so honouring both
 * would end the output in a blank line - which a reader of this format reads
 * as one more separator. Asserted on the bytes.
 */
TEST(RecordsWriter, ARecordsModeSubsumesTrailingNewline) {
  const std::vector<std::string> keys = {"a"};
  for (GTEXT_JSON_Records m : {GTEXT_JSON_RECORDS_WHITESPACE,
           GTEXT_JSON_RECORDS_LINE, GTEXT_JSON_RECORDS_SEQ}) {
    GTEXT_JSON_Write_Options with = gtext_json_write_options_default();
    with.records = m;
    with.trailing_newline = true;
    GTEXT_JSON_Write_Options without = with;
    without.trailing_newline = false;

    GTEXT_JSON_Status s1 = GTEXT_JSON_OK;
    GTEXT_JSON_Status s2 = GTEXT_JSON_OK;
    EXPECT_EQ(write_incremental(with, keys, &s1),
        write_incremental(without, keys, &s2))
        << "mode " << (int)m;
    EXPECT_EQ(write_values(with, keys, &s1), write_values(without, keys, &s2))
        << "mode " << (int)m;
  }

  // With records off, trailing_newline still does what it always did.
  GTEXT_JSON_Write_Options off = gtext_json_write_options_default();
  off.trailing_newline = true;
  GTEXT_JSON_Status st = GTEXT_JSON_OK;
  EXPECT_EQ(write_incremental(off, keys, &st), "{\"a\":0}\n");
}

/**
 * @test RecordsWriter.LineModeAndPrettyAreRefused
 *
 * One record per line and a value printed across lines contradict each other,
 * and the output would be a file this library's own LINE reader could not read
 * back. Both writers refuse, rather than one of them writing it.
 */
TEST(RecordsWriter, LineModeAndPrettyAreRefused) {
  GTEXT_JSON_Write_Options o = gtext_json_write_options_default();
  o.records = GTEXT_JSON_RECORDS_LINE;
  o.pretty = true;

  GTEXT_JSON_Status st = GTEXT_JSON_OK;
  write_incremental(o, {"a"}, &st);
  EXPECT_EQ(st, GTEXT_JSON_E_INVALID);

  GTEXT_JSON_Value * v = gtext_json_new_object();
  gtext_json_object_put(v, "a", 1, gtext_json_new_number_i64(1));
  GTEXT_JSON_Sink sink;
  ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
  EXPECT_EQ(
      gtext_json_write_value(&sink, &o, v, nullptr), GTEXT_JSON_E_INVALID);
  EXPECT_EQ(gtext_json_sink_buffer_size(&sink), 0u)
      << "a refusal must leave the sink untouched";
  gtext_json_sink_buffer_free(&sink);

  // Pretty is fine in the two modes that make no claim about lines.
  for (GTEXT_JSON_Records m :
      {GTEXT_JSON_RECORDS_WHITESPACE, GTEXT_JSON_RECORDS_SEQ}) {
    GTEXT_JSON_Write_Options ok = o;
    ok.records = m;
    GTEXT_JSON_Status s2 = GTEXT_JSON_OK;
    write_incremental(ok, {"a"}, &s2);
    EXPECT_EQ(s2, GTEXT_JSON_OK) << "mode " << (int)m;
  }
  gtext_json_free(v);
}

/**
 * @test RecordsRoundTrip.WhatThisLibraryWritesItReadsBack
 *
 * The property that makes the modes worth having: write N records in a mode,
 * read them back in the same mode, and get the same N documents. StreamRun over
 * every value kind at the top level, because a record is a whole JSON text and
 * the framing must not care which one.
 */
TEST(RecordsRoundTrip, WhatThisLibraryWritesItReadsBack) {
  const char * const docs[] = {"null", "true", "false", "0", "-1.5", "\"s\"",
      "\"\"", "[]", "{}", "[1,2,3]", "{\"a\":{\"b\":[1,{}]}}",
      "\"a line\\nbreak\""};

  for (GTEXT_JSON_Records m : {GTEXT_JSON_RECORDS_WHITESPACE,
           GTEXT_JSON_RECORDS_LINE, GTEXT_JSON_RECORDS_SEQ}) {
    // Build the sequence with the value writer, one call per record.
    GTEXT_JSON_Write_Options wo = gtext_json_write_options_default();
    wo.records = m;
    GTEXT_JSON_Sink sink;
    ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
    std::vector<std::string> expect;
    for (const char * d : docs) {
      GTEXT_JSON_Value * v =
          gtext_json_parse(d, std::strlen(d), nullptr, nullptr);
      ASSERT_NE(v, nullptr) << d;
      expect.push_back(write_compact(v));
      ASSERT_EQ(gtext_json_write_value(&sink, &wo, v, nullptr), GTEXT_JSON_OK)
          << d;
      gtext_json_free(v);
    }
    const std::string bytes(
        gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
    gtext_json_sink_buffer_free(&sink);

    // Read it back through the DOM loop and compare document by document.
    const DomRun back = read_dom(m, bytes);
    EXPECT_TRUE(back.complete)
        << "mode " << (int)m << " stopped with " << (int)back.stopped_with;
    EXPECT_EQ(back.records, expect) << "mode " << (int)m;

    // And through the stream, which must agree about how many there were.
    const StreamRun streamed = feed_stream(m, bytes);
    EXPECT_TRUE(streamed.ok()) << "mode " << (int)m;
    EXPECT_EQ(streamed.records(), expect.size()) << "mode " << (int)m;
  }
}

/**
 * @test Records.DegenerateInputs
 *
 * An input with no records is not a sequence of zero records by accident: it
 * has to be refused the way a parser refuses empty input, or a caller looping
 * until the end sees success and no data.
 */
TEST(Records, DegenerateInputs) {
  for (GTEXT_JSON_Records m : {GTEXT_JSON_RECORDS_WHITESPACE,
           GTEXT_JSON_RECORDS_LINE, GTEXT_JSON_RECORDS_SEQ}) {
    for (const char * input : {"", "\n", "   ", "\n\n\n"}) {
      const StreamRun run = feed_stream(m, input);
      EXPECT_FALSE(run.ok())
          << "mode " << (int)m << " accepted [" << input << "]";
      EXPECT_EQ(run.records(), 0u);
    }
    // A truncated last record is incomplete, not a short sequence.
    const StreamRun cut = feed_stream(m, "{\"a\":1}\n{\"b\":");
    EXPECT_FALSE(cut.ok()) << "mode " << (int)m;
  }
}
