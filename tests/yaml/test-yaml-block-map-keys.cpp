/**
 * @file test-yaml-block-map-keys.cpp
 * @brief What may stand where a block mapping's key belongs.
 *
 * A block mapping entry is
 *
 *     c-l-block-map-implicit-entry(n) ::=
 *       ( ns-s-block-map-implicit-key | e-node )
 *       c-l-block-map-implicit-value(n)
 *
 * and ns-s-block-map-implicit-key is c-s-implicit-json-key or
 * ns-s-implicit-yaml-key (8.2.2).  Three of those four spellings were
 * refused: the empty key, the flow collection, and a compact collection
 * standing as an explicit key on the "?"'s own line.
 *
 * None of it was visible while the conformance harness checked only values
 * and refusals.  A mapping with an empty key has a null key, which JSON
 * cannot write, so yaml-test-suite carries an event stream for those cases
 * and no JSON - and the harness skipped exactly the cases that were failing.
 * Every case named below is one of those.
 *
 * Neither PyYAML nor js-yaml accepts an empty key, so the references do not
 * settle these; the grammar and the suite do.
 */
#include <gtest/gtest.h>
#include <string>
#include <cstring>
#include <ghoti.io/text/yaml.h>

#include "yaml_render.h"

namespace {

struct Case {
	const char *input;
	const char *expected; /* nullptr: must be refused */
};

const Case kCases[] = {
	/* The empty key (suite cases NHX8, UKK6, NKF9, 2JQS, S3PD). */
	{": a\n", "{null: \"a\"}"},
	{":\n", "{null: null}"},
	{"- :\n", "[{null: null}]"},
	{"key: value\n: empty key\n", "{\"key\": \"value\", null: \"empty key\"}"},
	{"plain key: in-line value\n: \n\"quoted key\":\n- entry\n",
	 "{\"plain key\": \"in-line value\", null: null, "
	 "\"quoted key\": [\"entry\"]}"},

	/* A flow collection is c-s-implicit-json-key (suite cases LX3P, Q9WF). */
	{"[flow]: block\n", "{[\"flow\"]: \"block\"}"},
	{"{a: 1}: b\n", "{{\"a\": 1}: \"b\"}"},
	{"- [a]: b\n", "[{[\"a\"]: \"b\"}]"},
	{"{ first: Sammy, last: Sosa }:\n  hr: 65\n",
	 "{{\"first\": \"Sammy\", \"last\": \"Sosa\"}: {\"hr\": 65}}"},

	/* "?" takes s-l+block-indented, which includes a compact collection on
	 * the "?"'s own line (suite cases V9D5, M2N8, KK5P). */
	{"- sun: yellow\n- ? earth: blue\n  : moon: white\n",
	 "[{\"sun\": \"yellow\"}, {{\"earth\": \"blue\"}: {\"moon\": \"white\"}}]"},
	{"- ? : x\n", "[{{null: \"x\"}: null}]"},
	{"complex:\n  ? - a\n", "{\"complex\": {[\"a\"]: null}}"},
	{"complex:\n  ? - a\n  : b\n", "{\"complex\": {[\"a\"]: \"b\"}}"},

	/* A "?" key may be a sequence at the mapping's own column: s-l+block-node
	 * reaches one through seq-spaces(n,block-out), which is n-1 (8.2.1).
	 * Suite case 6PBE. */
	{"---\n?\n- a\n- b\n:\n- c\n- d\n", "{[\"a\", \"b\"]: [\"c\", \"d\"]}"},

	/* And the shapes that are still not entries with no key.  A ":" with
	 * anything but indentation in front of it on the line belongs to that
	 * thing, and where that thing is not a key the document is in error. */
	{"a:\n- 1\n  b: 2\n", nullptr},
	{"key: a : b\n", nullptr},
	{"key: a: b\n", nullptr},
};

} // namespace

TEST(YamlBlockMapKeys, WhatMayStandWhereAKeyBelongs) {
	for (const Case &c : kCases) {
		const std::string got = Render(c.input);
		if (c.expected) {
			EXPECT_EQ(got, std::string(c.expected))
				<< "input: " << ::testing::PrintToString(std::string(c.input));
		} else {
			EXPECT_EQ(got, std::string(""))
				<< "should have been refused, input: "
				<< ::testing::PrintToString(std::string(c.input));
		}
	}
}

/* An explicit entry that got its key but never a ":" has a value all the
 * same - c-l-block-map-explicit-entry's second arm is e-node (8.2.2).  The
 * key was claimed by the "?" rather than by a ":", and the check that asks
 * whether a ":" claimed it used to call the key a scalar nobody wanted. */
TEST(YamlBlockMapKeys, AnExplicitEntryWithNoColonStillHasAValue) {
	EXPECT_EQ(Render("? - a\n"), std::string("{[\"a\"]: null}"));
	EXPECT_EQ(Render("complex1:\n  ? - a\ncomplex2:\n  ? - a\n  : b\n"),
		std::string("{\"complex1\": {[\"a\"]: null}, "
			"\"complex2\": {[\"a\"]: \"b\"}}"));
}

/* A property written on an earlier line is still looking for its node, and
 * the node it would get is the mapping this ":" opens - "&a" over ": 1"
 * anchors the mapping the way it does in "&a" over "a: 1" (8.2).  There is
 * nowhere to put such a property when no key carries it in, so the entry
 * stays refused rather than being accepted with the property moved quietly
 * onto the value.  The same gap is why suite cases 26DV and 6BFJ still fail.
 */
TEST(YamlBlockMapKeys, AnOwnLinePropertyBeforeAnEmptyKeyIsStillRefused) {
	const char *inputs[] = {"!!str\n: 1\n", "&a\n: 1\n", "!custom\n: 1\n"};
	for (const char *input : inputs) {
		EXPECT_EQ(Render(input), std::string(""))
			<< "input: " << ::testing::PrintToString(std::string(input));
	}
}

/* An alias may stand where a key does, and then be left without a value.
 *
 * ns-s-block-map-implicit-key is ns-s-implicit-yaml-key, which is
 * ns-flow-yaml-node, whose first alternative is c-ns-alias-node (8.2.2,
 * 7.1) - so "*a : b" is a mapping keyed by whatever &a named, and "*a :" is
 * that key with the empty node for its value.
 *
 * The second of those was dropped.  A block mapping's children are collected
 * as one alternating key, value, key, value list, and a key whose value is
 * absent has to be given the null it stands for or the list is left one
 * short.  Three kinds of node arrive at the point where that is decided -
 * scalars, aliases, and flow collections - and only two of them asked:
 * mapping_supply_null_value() was reached from the SCALAR event and from the
 * four places a finished flow collection is added, and never from ALIAS.
 *
 * So the refusal landed on the *next* entry, not on the one at fault: its
 * ":" found an even-length list in front of it and reported a key missing
 * where there were two.  It took two aliases to see, because with an
 * ordinary scalar in the following entry the scalar's own copy of the rule
 * supplied the value the alias had not.  The writer fuzzer found it, on a
 * document of its own making that this library then refused to read back.
 *
 * One column is the whole measurement: an ALIAS event's column is the first
 * character of the anchor name, and the "*" that introduces it stands one to
 * its left.  Taken at face value every alias looked one column deeper than
 * it is, which is the difference between "at the key's column, so the next
 * key" and "indented, so the previous key's value" - the last case below is
 * what holds that line. */
TEST(YamlBlockMapKeys, AnAliasKeyWithNoValueStillHasOne) {
	static const Case kCases[] = {
		/* The find, and the same shape with both values absent. */
		{"a: &O v\nb: &P w\n*O :\n*P : y\n",
		 "{\"a\": \"v\", \"b\": \"w\", \"v\": null, \"w\": \"y\"}"},
		{"a: &O v\nb: &P w\n*O :\n*P :\n",
		 "{\"a\": \"v\", \"b\": \"w\", \"v\": null, \"w\": null}"},

		/* Nested, where the mapping's indentation is not 1. */
		{"outer:\n  a: &O v\n  b: &P w\n  *O :\n  *P : y\n",
		 "{\"outer\": {\"a\": \"v\", \"b\": \"w\", "
		 "\"v\": null, \"w\": \"y\"}}"},

		/* An absent value the entry below supplies as a collection. */
		{"a: &O v\nb: &P w\n*O :\n  x: 1\n*P : 2\n",
		 "{\"a\": \"v\", \"b\": \"w\", \"v\": {\"x\": 1}, \"w\": 2}"},

		/* The two that already worked, and would go on working if the rule
		   were never reached: a scalar follows the alias key in the first,
		   and nothing follows it in the second. */
		{"a: &O v\n*O :\nb: 2\n",
		 "{\"a\": \"v\", \"v\": null, \"b\": 2}"},
		{"a: &O v\nb: &P w\n*P : y\n*O :\n",
		 "{\"a\": \"v\", \"b\": \"w\", \"w\": \"y\", \"v\": null}"},

		/* Values, not keys.  A rule applied one column too far left turns
		   each of these into a second entry keyed by the alias. */
		{"a: &O v\nb: *O\n", "{\"a\": \"v\", \"b\": \"v\"}"},
		{"a: &O v\nb:\n  *O\n", "{\"a\": \"v\", \"b\": \"v\"}"},
		{"a: &O v\nb:\n- *O\n", "{\"a\": \"v\", \"b\": [\"v\"]}"},
	};

	for (const Case &c : kCases) {
		const std::string got = Render(c.input);
		if (c.expected) {
			EXPECT_EQ(got, std::string(c.expected))
				<< "input: " << ::testing::PrintToString(std::string(c.input));
		} else {
			EXPECT_EQ(got, std::string(""))
				<< "should have been refused, input: "
				<< ::testing::PrintToString(std::string(c.input));
		}
	}
}

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
