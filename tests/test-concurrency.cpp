/**
 * @file
 *
 * The concurrency guarantee in documentation/modules/Core.md, exercised.
 *
 * That page states the rule and then argues for it: *no object in this library
 * is thread-safe, and no two threads may touch the same object at once, but
 * distinct objects share nothing and may be used concurrently* - which holds
 * "because the library keeps no mutable global state", established by searching
 * for non-const file-scope variables. The search is still true: at the time this
 * file was written `src/` had no file-scope variable that was not `const`, no
 * function-local `static`, no lazily built table and no cache.
 *
 * An argument from a search is the right kind of argument and the wrong kind of
 * gate. What breaks that property is not carelessness, it is an ordinary-looking
 * improvement - a memo on the hot path, a table built on first use, a cached
 * default - and the search has to be repeated by hand to notice. Three of the
 * four "yes" rows in that table had no test at all.
 *
 * So each is run here from several threads, and this file is also what gives
 * `make test-tsan` something to look at. The two halves do different work and
 * both are needed: under the plain build these tests catch a torn or wrong
 * *answer*, which is the consequence a user would see; under ThreadSanitizer
 * they catch the race whether or not it lost. ASan and UBSan see neither - they
 * detect nothing about data races at all.
 *
 * ## Why the first test is named to sort first
 *
 * Lazily initialised state is written exactly **once**, on the first call. Any
 * single-threaded call before the threads start performs that write, after which
 * the threads only read and there is nothing left to race. A warmed-up
 * concurrency test passes against a real defect, and passes when the defect is
 * reintroduced, so it cannot even be shown to work.
 *
 * `AaaColdStart` therefore sorts first inside this binary, makes the process's
 * first call to each entry point *inside* the threads, and compares the threads
 * against each other rather than against anything prepared in advance. The
 * reference is built after the join.
 *
 * ## The row that must not be tested
 *
 * The table's first row - the same DOM, table, parser, stream, reader or writer
 * from two threads at once - is documented as **unsafe**. A test for it would be
 * a deliberate data race: it would pass the plain build most of the time and
 * fail `make test-tsan` by design, which is a gate reporting a defect that is
 * not one. The prohibition is not testable from inside the suite and is not
 * tested here.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <atomic>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

#include <ghoti.io/text/csv.h>
#include <ghoti.io/text/ini.h>
#include <ghoti.io/text/json.h>
#include <ghoti.io/text/text.h>
#include <ghoti.io/text/toml.h>
#include <ghoti.io/text/yaml.h>

namespace {

const int kThreads = 8;
const int kRounds = 40;

/* Serialize a JSON value to bytes. Every thread makes its own sink, which is
   the point: two sinks created separately share nothing. */
std::string WriteJson(const GTEXT_JSON_Value * v) {
	GTEXT_JSON_Sink sink;
	if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) return "<sink>";
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	if (gtext_json_write_value(&sink, nullptr, v, &err) != GTEXT_JSON_OK) {
		gtext_json_sink_buffer_free(&sink);
		return "<write>";
	}
	std::string out(gtext_json_sink_buffer_data(&sink),
			gtext_json_sink_buffer_size(&sink));
	gtext_json_sink_buffer_free(&sink);
	return out;
}

/* Parse one document of each format, read something back out of each, and
   return the five answers joined. A thread that raced on shared state inside
   any of the five parsers reports a different string here.

   Done as one function so that every thread exercises all five formats
   simultaneously rather than one format at a time: a race between two
   *different* modules sharing a global would otherwise need the two threads to
   be in the same module at the same moment, which running them in lockstep
   makes unlikely. */
std::string ParseAllFiveFormats(int salt) {
	const std::string tag = std::to_string(salt);
	std::string answer;

	/* JSON. The salt is in the document, so two threads with different salts
	   must get different answers - which is what makes an equal answer across
	   equal salts evidence rather than a coincidence of constants. */
	{
		const std::string src =
			"{\"n\":" + tag + ",\"s\":\"x" + tag + "\",\"a\":[1,2,3]}";
		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * v = gtext_json_parse(src.data(), src.size(), nullptr, &err);
		answer += v ? WriteJson(v) : "<json>";
		gtext_json_free(v);
	}
	answer += "|";

	/* YAML, through its own DOM and out again as JSON, so the conversion and
	   the number formatting are on the path too. */
	{
		const std::string src = "name: ghoti" + tag + "\nitems:\n  - 1.5\n  - two\n";
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Document * doc =
			gtext_yaml_parse(src.data(), src.size(), nullptr, &err);
		if (doc) {
			const GTEXT_YAML_Node * name =
				gtext_yaml_mapping_get(gtext_yaml_document_root(doc), "name");
			const char * s = name ? gtext_yaml_node_as_string(name) : nullptr;
			answer += s ? s : "<node>";
			gtext_yaml_free(doc);
		}
		else {
			answer += "<yaml>";
		}
		gtext_yaml_error_free(&err);
	}
	answer += "|";

	/* TOML, whose float lexing goes through the locale-independent conversion
	   that was the last piece of shared-looking machinery in the library. */
	{
		const std::string src = "k = \"v" + tag + "\"\nf = 0.1\n";
		GTEXT_TOML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_TOML_Value * root =
			gtext_toml_parse(src.data(), src.size(), nullptr, &err);
		if (root) {
			const GTEXT_TOML_Value * k = gtext_toml_table_get(root, "k", 1);
			size_t slen = 0;
			const char * s = k ? gtext_toml_value_string(k, &slen) : nullptr;
			double f = 0;
			const GTEXT_TOML_Value * fv = gtext_toml_table_get(root, "f", 1);
			if (fv) gtext_toml_value_float(fv, &f);
			answer += (s ? s : "<key>");
			answer += (f == 0.1 ? "/ok" : "/bad");
			gtext_toml_free(root);
		}
		else {
			answer += "<toml>";
		}
		gtext_toml_error_free(&err);
	}
	answer += "|";

	/* INI. */
	{
		const std::string src = "[s]\nkey=val" + tag + "\n";
		GTEXT_INI_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_INI_Document * doc =
			gtext_ini_parse(src.data(), src.size(), nullptr, &err);
		if (doc) {
			const GTEXT_INI_Group * g = gtext_ini_document_group(doc, "s");
			size_t vlen = 0;
			const char * s = g ? gtext_ini_group_get(g, "key", &vlen) : nullptr;
			answer += s ? s : "<ini-key>";
			gtext_ini_free(doc);
		}
		else {
			answer += "<ini>";
		}
		gtext_ini_error_free(&err);
	}
	answer += "|";

	/* CSV. */
	{
		const std::string src = "a,b\n1,x" + tag + "\n";
		GTEXT_CSV_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_CSV_Table * t =
			gtext_csv_parse_table(src.data(), src.size(), nullptr, &err);
		if (t) {
			size_t flen = 0;
			const char * f = gtext_csv_field(t, 1, 1, &flen);
			answer += f ? f : "<field>";
			answer += "/" + std::to_string(gtext_csv_row_count(t));
			gtext_csv_free_table(t);
		}
		else {
			answer += "<csv>";
		}
		gtext_csv_error_free(&err);
	}

	return answer;
}

/* Run `body(thread_index)` on kThreads threads and return what each produced.
   Nothing is called on this thread first, so whatever `body` touches is
   touched for the first time by whichever thread gets there first. */
std::vector<std::string> Across(
		const std::function<std::string(int)> & body) {
	std::vector<std::string> results(kThreads);
	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int i = 0; i < kThreads; ++i) {
		threads.emplace_back([i, &results, &body]() { results[i] = body(i); });
	}
	for (auto & t : threads) t.join();
	return results;
}

}  // namespace

/* Row: `gtext_*_parse()` on separate inputs into separate results - yes.
 *
 * First in the binary by name, so the first call into each of the five parsers
 * is made by a thread and not by the test runner. Each thread parses documents
 * carrying its own index, so the answers are *supposed* to differ; the assertion
 * is that each thread's answer is the one its own input implies. That is
 * stronger than comparing threads to each other on identical input, where a
 * parser returning a constant would pass.
 */
TEST(AaaColdStart, FiveParsersOnSeparateInputsFromEightThreads) {
	const std::vector<std::string> got =
		Across([](int i) { return ParseAllFiveFormats(i); });

	/* The reference is computed here, after the join, on this thread. Computing
	   it before would have warmed every path the threads were meant to reach
	   first. */
	for (int i = 0; i < kThreads; ++i) {
		const std::string alone = ParseAllFiveFormats(i);
		EXPECT_EQ(got[i], alone)
			<< "thread " << i << " parsed its own input differently from a "
			   "solitary parse of the same input";
		EXPECT_EQ(got[i].find('<'), std::string::npos)
			<< "thread " << i << " failed a parse or an accessor: " << got[i];
	}
}

/* Repeated rounds, so that a race with a narrow window has more than one
 * chance. Eight threads x forty rounds x five formats is 1,600 parses with no
 * ordering between them.
 *
 * Content alone is what this asserts, which is why it is worth having outside
 * the sanitizer: a torn result is what a user of a racy build would actually
 * see. ThreadSanitizer is what makes the *absence* of a finding mean something.
 */
TEST(Concurrency, RepeatedParsesAgreeWithSolitaryOnes) {
	const std::vector<std::string> expected = [] {
		std::vector<std::string> e;
		for (int i = 0; i < kThreads; ++i) e.push_back(ParseAllFiveFormats(i));
		return e;
	}();

	std::atomic<int> disagreements{0};
	std::vector<std::string> first_bad(kThreads);

	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int i = 0; i < kThreads; ++i) {
		threads.emplace_back([i, &expected, &first_bad, &disagreements]() {
			for (int round = 0; round < kRounds; ++round) {
				const std::string got = ParseAllFiveFormats(i);
				/* The FIRST disagreement is kept and never overwritten: keeping
				   the latest would let a transient tear be erased by the next
				   good round, which is the difference between a test that can
				   fail and one that only looks like it can. */
				if (got != expected[i] && first_bad[i].empty()) {
					first_bad[i] = got;
					disagreements.fetch_add(1, std::memory_order_relaxed);
				}
			}
		});
	}
	for (auto & t : threads) t.join();

	EXPECT_EQ(disagreements.load(), 0);
	for (int i = 0; i < kThreads; ++i) {
		EXPECT_TRUE(first_bad[i].empty())
			<< "thread " << i << " got\n  " << first_bad[i] << "\nwanted\n  "
			<< expected[i];
	}
}

/* Row: the same DOM read-only from both, with no writer anywhere - yes.
 *
 * Necessarily warm: the claim is about a document that already exists, so one
 * has to be parsed before the threads start. What is cold here is the *writing*
 * side - no value is serialized before the threads, so each thread's sink is
 * the first one the process makes.
 */
TEST(Concurrency, OneDomSerializedByEightThreadsAtOnce) {
	const std::string src =
		"{\"a\":[1,2,3],\"b\":{\"c\":\"d\"},\"e\":1.5,\"f\":null,"
		"\"g\":true,\"h\":\"\\u00e9\\u20ac\"}";
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * shared =
		gtext_json_parse(src.data(), src.size(), nullptr, &err);
	ASSERT_NE(shared, nullptr) << (err.message ? err.message : "unknown");

	const std::vector<std::string> got =
		Across([shared](int) { return WriteJson(shared); });

	/* Every thread wrote the same bytes, and they are the bytes a solitary
	   write produces. Both halves matter: eight identical wrong answers would
	   pass the first on its own. */
	const std::string alone = WriteJson(shared);
	for (int i = 0; i < kThreads; ++i) {
		EXPECT_EQ(got[i], alone) << "thread " << i;
	}

	/* ...and that answer is the right one. Without this, eight identical wrong
	   answers would pass the loop above. Not compared against `src`: the
	   escapes there are input spelling, and the writer emits the characters
	   they denote as UTF-8, which is what a round trip through this library is
	   supposed to do. */
	/* Each byte in its own literal: "\xC3\xA9" in one literal is a single hex
	   escape, because a hex escape has no length limit and eats every hex
	   digit that follows it. */
	const std::string want =
		std::string("{\"a\":[1,2,3],\"b\":{\"c\":\"d\"},\"e\":1.5,")
		+ "\"f\":null,\"g\":true,\"h\":\""
		+ "\xC3" "\xA9" "\xE2" "\x82" "\xAC" + "\"}";
	EXPECT_EQ(alone, want)
		<< "the round trip itself is wrong, so the comparison above was "
		   "between eight copies of a wrong answer";

	gtext_json_free(shared);
}

/* Row: the version accessors and the `*_options_default()` functions - yes.
 *
 * tests/test.cpp has Version.StringIsThreadSafe for the string accessor, and
 * it warms up: it reads `gtext_version_string()` into `expected` before
 * starting any thread. That is the right thing for its own assertion - it needs
 * something to compare against - and it means the accessor's first call is
 * never made by a thread. This is the cold half, and it covers the five
 * `*_options_default()` functions, which that test does not reach at all.
 *
 * An options struct is returned by value and is the obvious candidate for a
 * "build the defaults once and copy them" optimisation, which is exactly the
 * shape this file exists to catch.
 */
TEST(Concurrency, AccessorsAndOptionDefaultsFromEightColdThreads) {
	const std::vector<std::string> got = Across([](int) {
		std::string local;
		for (int round = 0; round < kRounds; ++round) {
			const char * s = gtext_version_string();
			local = s ? s : "<null>";
			/* Each default is read and one field of it is spelled, so that the
			   struct has to be filled in rather than merely returned. */
			GTEXT_JSON_Parse_Options j = gtext_json_parse_options_default();
			GTEXT_YAML_Parse_Options y = gtext_yaml_parse_options_default();
			GTEXT_CSV_Parse_Options c = gtext_csv_parse_options_default();
			GTEXT_TOML_Parse_Options t = gtext_toml_parse_options_default();
			GTEXT_INI_Parse_Options i = gtext_ini_parse_options_default();
			local += "/" + std::to_string((int) j.dupkeys);
			local += "/" + std::to_string((int) y.dupkeys);
			local += "/" + std::to_string((int) c.dialect.delimiter);
			local += "/" + std::to_string((int) t.max_depth);
			local += "/" + std::to_string((int) i.max_groups);
		}
		return local;
	});

	for (int i = 1; i < kThreads; ++i) {
		EXPECT_EQ(got[i], got[0])
			<< "thread " << i << " and thread 0 disagree about the version "
			   "string or a default options struct";
	}
	EXPECT_EQ(got[0].find('<'), std::string::npos) << got[0];
	EXPECT_FALSE(got[0].empty());
}
