/**
 * @file
 *
 * What the streaming parser does about a name that appears twice.
 *
 * GTEXT_JSON_Parse_Options::dupkeys has four values, and before
 * GTEXT_JSON_Event::repeated_key existed the streaming parser could honour
 * only two of them. ERROR refuses the document and FIRST_WINS parses the
 * repeated member without delivering it - both decisions it can take before
 * the callback has seen anything. LAST_WINS and COLLECT it could not honour at
 * all: last-wins means replacing a value already handed over and collect means
 * wrapping one after the fact, an event cannot be retracted, and buffering an
 * object until it closes is not streaming.
 *
 * So under those two modes it delivered every member and said nothing, which
 * is a silent disagreement with gtext_json_parse() on the same option and was
 * recorded as an adoption blocker on the comparison page. It still delivers
 * every member - that part is structural - but it now says which of them are
 * repeats, and that turns out to be exactly enough.
 *
 * ## What these tests assert, and why the last one is the real one
 *
 * The flag itself is easy to assert and easy to get wrong in the direction
 * that matters: a parser that set it on every key, or on none, would pass a
 * test that only counted events. So the load-bearing test here is a
 * differential. It consumes the events, applies the policy *in the callback*
 * the way a real caller would - overwrite for last-wins, append for collect -
 * and compares what it ends up holding against what gtext_json_parse() built
 * from the same bytes under the same option. If the flag were wrong in either
 * direction that comparison fails, whatever the counts say.
 *
 * There was no streaming duplicate-key test of any kind before this file.
 * tests/test-json.cpp covers all four modes against the DOM parser and none
 * against the stream.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <gtest/gtest.h>

#include <ghoti.io/text/json.h>

namespace {

/* One event, flattened to what these tests compare. */
struct Seen {
	GTEXT_JSON_Event_Type type;
	std::string text;
	bool repeated;
};

struct Capture {
	std::vector<Seen> events;
	/* The policy applied as a real caller would, in the callback, from the
	   flag alone. `order` keeps insertion order so that a comparison against
	   the DOM is about order too and not only about contents. */
	std::vector<std::string> order;
	std::map<std::string, std::vector<std::string>> members;
	std::string pending_key;
	bool pending_repeat = false;
	/* An explicit flag, not `!pending_key.empty()`. The empty name is a real
	   name: the first draft of this helper used the string's emptiness as
	   "there is no pending key" and reconstructed nothing at all from
	   `{"":9,"":2,"":1}`, which is the input the fuzz harness produced. The
	   library was right and the helper was wrong - and `len == 0` is a
	   separate branch inside json_stream_name_seen() too, so it is a shape
	   worth getting right on both sides. */
	bool have_key = false;
	GTEXT_JSON_Dupkey_Mode mode = GTEXT_JSON_DUPKEY_ERROR;
	int depth = 0;
};

std::string EventText(const GTEXT_JSON_Event * evt) {
	switch (evt->type) {
		case GTEXT_JSON_EVT_KEY:
		case GTEXT_JSON_EVT_STRING:
			return std::string(evt->as.str.s ? evt->as.str.s : "",
					evt->as.str.len);
		case GTEXT_JSON_EVT_NUMBER:
			return std::string(evt->as.number.s ? evt->as.number.s : "",
					evt->as.number.len);
		case GTEXT_JSON_EVT_BOOL:
			return evt->as.boolean ? "true" : "false";
		default:
			return "";
	}
}

GTEXT_JSON_Status Collect(
		void * user, const GTEXT_JSON_Event * evt, GTEXT_JSON_Error * err) {
	(void)err;
	Capture * c = static_cast<Capture *>(user);
	c->events.push_back({evt->type, EventText(evt), evt->repeated_key});

	switch (evt->type) {
		case GTEXT_JSON_EVT_OBJECT_BEGIN:
		case GTEXT_JSON_EVT_ARRAY_BEGIN:
			c->depth++;
			break;
		case GTEXT_JSON_EVT_OBJECT_END:
		case GTEXT_JSON_EVT_ARRAY_END:
			c->depth--;
			break;
		case GTEXT_JSON_EVT_KEY:
			/* Only the top-level object's members are reconstructed; a nested
			   one would need a stack, and the nesting is asserted separately
			   by the flag itself rather than through this reconstruction. */
			if (c->depth == 1) {
				c->pending_key = EventText(evt);
				c->pending_repeat = evt->repeated_key;
				c->have_key = true;
			}
			break;
		default:
			if (c->depth == 1 && c->have_key) {
				const std::string value = EventText(evt);
				if (!c->pending_repeat) {
					c->order.push_back(c->pending_key);
					c->members[c->pending_key] = {value};
				}
				else if (c->mode == GTEXT_JSON_DUPKEY_LAST_WINS) {
					/* "overwrite what you stored under this name" */
					c->members[c->pending_key] = {value};
				}
				else if (c->mode == GTEXT_JSON_DUPKEY_COLLECT) {
					/* "append to it" */
					c->members[c->pending_key].push_back(value);
				}
				c->pending_key.clear();
				c->pending_repeat = false;
				c->have_key = false;
			}
			break;
	}
	return GTEXT_JSON_OK;
}

/* Feed `src` to a stream in `chunk`-byte pieces (0 means all at once) and
   report the status of the whole parse. */
GTEXT_JSON_Status Feed(const std::string & src, GTEXT_JSON_Dupkey_Mode mode,
		Capture * out, size_t chunk = 0) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.dupkeys = mode;
	out->mode = mode;

	GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, Collect, out);
	if (!st) return GTEXT_JSON_E_OOM;

	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Status status = GTEXT_JSON_OK;
	const size_t step = chunk ? chunk : src.size();
	for (size_t i = 0; i < src.size() && status == GTEXT_JSON_OK; i += step) {
		const size_t n = std::min(step, src.size() - i);
		status = gtext_json_stream_feed(st, src.data() + i, n, &err);
	}
	if (status == GTEXT_JSON_OK) {
		status = gtext_json_stream_finish(st, &err);
	}
	gtext_json_stream_free(st);
	/* The snippet in a reported error belongs to the caller. Half these tests
	   expect a refusal, so forgetting this leaks on exactly the paths they are
	   about - which is how it was found: by LeakSanitizer, not by a failing
	   assertion. */
	gtext_json_error_free(&err);
	return status;
}

/* The same reconstruction, read out of a DOM parse instead - the other side of
   the differential. */
bool DomMembers(const std::string & src, GTEXT_JSON_Dupkey_Mode mode,
		std::vector<std::string> * order,
		std::map<std::string, std::vector<std::string>> * members) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.dupkeys = mode;
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * root =
		gtext_json_parse(src.data(), src.size(), &opts, &err);
	if (!root) {
		gtext_json_error_free(&err);
		return false;
	}

	for (size_t i = 0; i < gtext_json_object_size(root); i++) {
		size_t klen = 0;
		const char * k = gtext_json_object_key(root, i, &klen);
		const GTEXT_JSON_Value * v = gtext_json_object_value(root, i);
		const std::string key(k ? k : "", klen);
		order->push_back(key);

		/* Under COLLECT the DOM holds an array where a name repeated; under
		   LAST_WINS it holds the single surviving value. Flattened to the same
		   shape as the stream reconstruction so the two are comparable. */
		std::vector<std::string> flat;
		auto spell = [](const GTEXT_JSON_Value * x) {
			size_t len = 0;
			const char * s = nullptr;
			if (gtext_json_typeof(x) == GTEXT_JSON_NUMBER
					&& gtext_json_get_number_lexeme(x, &s, &len)
						== GTEXT_JSON_OK) {
				return std::string(s ? s : "", len);
			}
			if (gtext_json_typeof(x) == GTEXT_JSON_STRING
					&& gtext_json_get_string(x, &s, &len) == GTEXT_JSON_OK) {
				return std::string(s ? s : "", len);
			}
			if (gtext_json_typeof(x) == GTEXT_JSON_BOOL) {
				bool b = false;
				gtext_json_get_bool(x, &b);
				return std::string(b ? "true" : "false");
			}
			return std::string();
		};
		if (mode == GTEXT_JSON_DUPKEY_COLLECT
				&& gtext_json_typeof(v) == GTEXT_JSON_ARRAY) {
			for (size_t j = 0; j < gtext_json_array_size(v); j++) {
				flat.push_back(spell(gtext_json_array_get(v, j)));
			}
		}
		else {
			flat.push_back(spell(v));
		}
		(*members)[key] = flat;
	}
	gtext_json_free(root);
	gtext_json_error_free(&err);
	return true;
}

size_t CountKeys(const Capture & c) {
	size_t n = 0;
	for (const Seen & s : c.events) {
		if (s.type == GTEXT_JSON_EVT_KEY) n++;
	}
	return n;
}

size_t CountRepeats(const Capture & c) {
	size_t n = 0;
	for (const Seen & s : c.events) {
		if (s.repeated) n++;
	}
	return n;
}

const char * kFlat = "{\"a\":1,\"b\":2,\"a\":3,\"c\":4,\"a\":5}";

}  // namespace

// ---------------------------------------------------------------------------
// The two modes the stream could always honour on its own
// ---------------------------------------------------------------------------

TEST(JsonStreamDupkeys, ErrorRefusesAndMatchesTheDomParser) {
	Capture c;
	EXPECT_NE(Feed(kFlat, GTEXT_JSON_DUPKEY_ERROR, &c), GTEXT_JSON_OK);

	std::vector<std::string> order;
	std::map<std::string, std::vector<std::string>> members;
	EXPECT_FALSE(DomMembers(kFlat, GTEXT_JSON_DUPKEY_ERROR, &order, &members))
		<< "the DOM parser accepted what the stream refused";

	EXPECT_EQ(CountRepeats(c), 0u)
		<< "ERROR never delivers a key event for a repeated name, so it has "
		   "nothing to set the flag on";
}

TEST(JsonStreamDupkeys, FirstWinsDeliversOneMemberPerDistinctName) {
	Capture c;
	ASSERT_EQ(Feed(kFlat, GTEXT_JSON_DUPKEY_FIRST_WINS, &c), GTEXT_JSON_OK);

	/* Three distinct names out of five members. */
	EXPECT_EQ(CountKeys(c), 3u);
	EXPECT_EQ(CountRepeats(c), 0u)
		<< "FIRST_WINS does not deliver the repeated member at all, so the "
		   "flag has nothing to be true on";

	std::vector<std::string> order;
	std::map<std::string, std::vector<std::string>> members;
	ASSERT_TRUE(
		DomMembers(kFlat, GTEXT_JSON_DUPKEY_FIRST_WINS, &order, &members));
	EXPECT_EQ(c.order, order);
	EXPECT_EQ(c.members, members);
}

// ---------------------------------------------------------------------------
// The two it could not, and now reports
// ---------------------------------------------------------------------------

TEST(JsonStreamDupkeys, LastWinsDeliversEveryMemberAndFlagsTheRepeats) {
	Capture c;
	ASSERT_EQ(Feed(kFlat, GTEXT_JSON_DUPKEY_LAST_WINS, &c), GTEXT_JSON_OK);

	/* Every member, which is the structural fact, not a defect. */
	EXPECT_EQ(CountKeys(c), 5u);
	/* ...and exactly the two that are repeats. Both halves are needed: a
	   parser that flagged every key would pass the first assertion and a
	   parser that flagged none would pass it too. */
	EXPECT_EQ(CountRepeats(c), 2u);

	std::vector<bool> flags;
	for (const Seen & s : c.events) {
		if (s.type == GTEXT_JSON_EVT_KEY) flags.push_back(s.repeated);
	}
	const std::vector<bool> want = {false, false, true, false, true};
	EXPECT_EQ(flags, want) << "a, b, a, c, a - the third and fifth are repeats";
}

TEST(JsonStreamDupkeys, CollectFlagsTheSameRepeats) {
	Capture c;
	ASSERT_EQ(Feed(kFlat, GTEXT_JSON_DUPKEY_COLLECT, &c), GTEXT_JSON_OK);
	EXPECT_EQ(CountKeys(c), 5u);
	EXPECT_EQ(CountRepeats(c), 2u);
}

/* The load-bearing test. The callback applies the policy from the flag, and
 * what it ends up holding is compared against what gtext_json_parse() built
 * from the same bytes under the same option - insertion order included.
 *
 * This is what makes the flag a fix rather than a disclosure: if it were set
 * on every key, the reconstruction would drop the first occurrence of every
 * name; if on none, it would keep the first and lose the rest under LAST_WINS
 * and hold single values where the DOM holds arrays under COLLECT. Both were
 * run against this test and both fail it.
 */
TEST(JsonStreamDupkeys, TheFlagIsEnoughToReproduceTheDomParsersAnswer) {
	for (const GTEXT_JSON_Dupkey_Mode mode :
			{GTEXT_JSON_DUPKEY_LAST_WINS, GTEXT_JSON_DUPKEY_COLLECT}) {
		const char * label = mode == GTEXT_JSON_DUPKEY_LAST_WINS
			? "LAST_WINS" : "COLLECT";
		for (const std::string & src : {
				std::string(kFlat),
				std::string("{\"x\":\"one\",\"x\":\"two\"}"),
				std::string("{\"k\":true,\"k\":false,\"k\":true}"),
				std::string("{\"only\":1}"),
				std::string("{}")}) {
			Capture c;
			ASSERT_EQ(Feed(src, mode, &c), GTEXT_JSON_OK) << label << " " << src;

			std::vector<std::string> order;
			std::map<std::string, std::vector<std::string>> members;
			ASSERT_TRUE(DomMembers(src, mode, &order, &members))
				<< label << " " << src;

			EXPECT_EQ(c.order, order) << label << " " << src;
			EXPECT_EQ(c.members, members) << label << " " << src;
		}
	}
}

// ---------------------------------------------------------------------------
// Where the flag must not fire
// ---------------------------------------------------------------------------

TEST(JsonStreamDupkeys, ANameIsOnlyRepeatedWithinItsOwnObject) {
	/* "a" appears in both objects and is a first occurrence in each; "b"
	   repeats inside the inner one only. A parser tracking names per *stream*
	   rather than per object would flag the second "a". */
	const char * src = "{\"a\":1,\"inner\":{\"a\":2,\"b\":3,\"b\":4},\"b\":5}";
	Capture c;
	ASSERT_EQ(Feed(src, GTEXT_JSON_DUPKEY_LAST_WINS, &c), GTEXT_JSON_OK);

	std::vector<std::pair<std::string, bool>> keys;
	for (const Seen & s : c.events) {
		if (s.type == GTEXT_JSON_EVT_KEY) keys.push_back({s.text, s.repeated});
	}
	const std::vector<std::pair<std::string, bool>> want = {
		{"a", false}, {"inner", false}, {"a", false}, {"b", false},
		{"b", true}, {"b", false}};
	EXPECT_EQ(keys, want);
}

TEST(JsonStreamDupkeys, TheFlagIsFalseOnEverythingThatIsNotAKey) {
	const char * src = "{\"a\":[1,{\"a\":2}],\"a\":null,\"s\":\"t\"}";
	Capture c;
	ASSERT_EQ(Feed(src, GTEXT_JSON_DUPKEY_LAST_WINS, &c), GTEXT_JSON_OK);
	ASSERT_GT(c.events.size(), 8u);
	for (const Seen & s : c.events) {
		if (s.type != GTEXT_JSON_EVT_KEY) {
			EXPECT_FALSE(s.repeated)
				<< "event type " << (int) s.type << " carried the flag";
		}
	}
}

TEST(JsonStreamDupkeys, TwoSiblingObjectsEachStartFresh) {
	const char * src = "{\"o1\":{\"k\":1,\"k\":2},\"o2\":{\"k\":3}}";
	Capture c;
	ASSERT_EQ(Feed(src, GTEXT_JSON_DUPKEY_LAST_WINS, &c), GTEXT_JSON_OK);
	EXPECT_EQ(CountRepeats(c), 1u)
		<< "the k in o2 is a first occurrence; the name array is popped with "
		   "its object";
}

// ---------------------------------------------------------------------------
// The flag has to survive the things a stream does
// ---------------------------------------------------------------------------

TEST(JsonStreamDupkeys, TheFlagIsTheSameAtEveryChunkSize) {
	const std::string src = kFlat;
	Capture whole;
	ASSERT_EQ(Feed(src, GTEXT_JSON_DUPKEY_LAST_WINS, &whole), GTEXT_JSON_OK);

	for (size_t chunk = 1; chunk <= src.size(); chunk++) {
		Capture c;
		ASSERT_EQ(Feed(src, GTEXT_JSON_DUPKEY_LAST_WINS, &c, chunk),
			GTEXT_JSON_OK) << "chunk " << chunk;
		ASSERT_EQ(c.events.size(), whole.events.size()) << "chunk " << chunk;
		for (size_t i = 0; i < c.events.size(); i++) {
			EXPECT_EQ(c.events[i].type, whole.events[i].type)
				<< "chunk " << chunk << " event " << i;
			EXPECT_EQ(c.events[i].text, whole.events[i].text)
				<< "chunk " << chunk << " event " << i;
			EXPECT_EQ(c.events[i].repeated, whole.events[i].repeated)
				<< "chunk " << chunk << " event " << i
				<< ": a key split across a chunk boundary lost its flag";
		}
	}
}

TEST(JsonStreamDupkeys, ThePullReaderCarriesTheFlag) {
	/* The reader copies each event into a queue, which is a second place the
	   field has to be carried - and the copy has two early returns, so a field
	   set in the wrong one is dropped for exactly the event type it matters on.
	 */
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.dupkeys = GTEXT_JSON_DUPKEY_LAST_WINS;
	GTEXT_JSON_Reader * reader = gtext_json_reader_new(&opts);
	ASSERT_NE(reader, nullptr);

	const std::string src = kFlat;
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	ASSERT_EQ(gtext_json_reader_feed(reader, src.data(), src.size(), &err),
		GTEXT_JSON_OK);
	/* NULL with length 0 is how this reader is told the input ended. */
	ASSERT_EQ(gtext_json_reader_feed(reader, nullptr, 0, &err), GTEXT_JSON_OK);
	gtext_json_error_free(&err);

	std::vector<bool> flags;
	GTEXT_JSON_Event evt;
	memset(&evt, 0, sizeof(evt));
	while (gtext_json_reader_next(reader, &evt) == GTEXT_JSON_OK) {
		if (evt.type == GTEXT_JSON_EVT_KEY) flags.push_back(evt.repeated_key);
	}
	gtext_json_reader_free(reader);

	const std::vector<bool> want = {false, false, true, false, true};
	EXPECT_EQ(flags, want);
}

TEST(JsonStreamDupkeys, EscapedAndPlainSpellingsAreTheSameName) {
	/* Names are compared after decoding, as the DOM parser compares them, so
	   "a" and "\u0061" are one name and the second is a repeat. */
	const char * src = "{\"a\":1,\"\\u0061\":2}";
	Capture c;
	ASSERT_EQ(Feed(src, GTEXT_JSON_DUPKEY_LAST_WINS, &c), GTEXT_JSON_OK);
	EXPECT_EQ(CountKeys(c), 2u);
	EXPECT_EQ(CountRepeats(c), 1u);

	Capture e;
	EXPECT_NE(Feed(src, GTEXT_JSON_DUPKEY_ERROR, &e), GTEXT_JSON_OK)
		<< "and ERROR refuses it, which is the same judgement";
}

// ---------------------------------------------------------------------------
// Promoted from the fuzz harness
// ---------------------------------------------------------------------------

/* The two minimal witnesses tests/fuzz/fuzz_json_dupkeys.cpp produced against
 * the planted versions of this feature, kept as tests rather than as corpus
 * files - which is this repository's rule for a reproducer.
 *
 * `{"":9,"":2,"":1}` under COLLECT was the shortest input on which a flag set
 * on every key reordered the reconstruction. The empty name is the part worth
 * keeping: json_stream_name_seen() compares `len == 0 ||` before the memcmp,
 * so a zero-length name takes a different branch from every other name, and
 * nothing in the hand-written tests above reached it.
 */
TEST(JsonStreamDupkeys, TheEmptyNameRepeatsLikeAnyOther) {
	const char * src = "{\"\":9,\"\":2,\"\":1}";

	Capture c;
	ASSERT_EQ(Feed(src, GTEXT_JSON_DUPKEY_COLLECT, &c), GTEXT_JSON_OK);
	EXPECT_EQ(CountKeys(c), 3u);
	EXPECT_EQ(CountRepeats(c), 2u);

	std::vector<std::string> order;
	std::map<std::string, std::vector<std::string>> members;
	ASSERT_TRUE(DomMembers(src, GTEXT_JSON_DUPKEY_COLLECT, &order, &members));
	EXPECT_EQ(c.order, order);
	EXPECT_EQ(c.members, members);

	Capture e;
	EXPECT_NE(Feed(src, GTEXT_JSON_DUPKEY_ERROR, &e), GTEXT_JSON_OK);
}

/* `\{"a"` - a key with no colon, no value and no closing brace. The second
 * witness, and the shape that separates "this name is a repeat" from "this
 * member is complete": the parse fails, but a key event may already have been
 * delivered, and its flag has to be right anyway. */
TEST(JsonStreamDupkeys, ATruncatedMemberStillReportsHonestly) {
	for (const GTEXT_JSON_Dupkey_Mode mode : {GTEXT_JSON_DUPKEY_ERROR,
			GTEXT_JSON_DUPKEY_FIRST_WINS, GTEXT_JSON_DUPKEY_LAST_WINS,
			GTEXT_JSON_DUPKEY_COLLECT}) {
		Capture c;
		c.mode = mode;
		EXPECT_NE(Feed("{\"a\"", mode, &c), GTEXT_JSON_OK)
			<< "mode " << (int) mode;
		EXPECT_EQ(CountRepeats(c), 0u)
			<< "mode " << (int) mode << ": one occurrence of one name";
	}

	/* ...and a truncated *repeat* is reported as a repeat by the two modes
	   that deliver it, even though the member never completes. */
	Capture c;
	EXPECT_NE(Feed("{\"a\":1,\"a\"", GTEXT_JSON_DUPKEY_LAST_WINS, &c),
		GTEXT_JSON_OK);
	EXPECT_EQ(CountRepeats(c), 1u);
}
