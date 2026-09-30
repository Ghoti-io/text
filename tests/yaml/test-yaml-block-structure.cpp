/**
 * Where a block collection ends, and where a key is allowed to sit.
 *
 * Two defects are pinned here. A block sequence may sit at the same column as
 * the key that owns it, and one written that way never closed: the parent's
 * next key was taken as another entry, so "a:" over "- 1" over "b: 2" gave
 * {"a": [1, {"b": 2}]} instead of {"a": [1], "b": 2}. And a key indented
 * deeper than its mapping was nested even with no key above waiting for a
 * value, which put a mapping where a key belongs: "a: 1" over an indented
 * "b: 2" gave {"a": 1, {"b": 2}: null} rather than being refused.
 *
 * Expectations are PyYAML's for the same input; the accepted ones are its
 * json.dumps output, which Render() below reproduces exactly.
 */
#include <gtest/gtest.h>
#include <string.h>
#include <string>
#include <ghoti.io/text/yaml.h>

#include "yaml_render.h"

namespace {


struct Case {
	const char *input;
	const char *expected; /* nullptr: must be refused */
};

const Case kCases[] = {
	{"a:\n- 1\nb: 2\n", "{\"a\": [1], \"b\": 2}"},
	{"a:\n  - 1\nb: 2\n", "{\"a\": [1], \"b\": 2}"},
	{"a:\n- 1\n- 2\nb: 2\n", "{\"a\": [1, 2], \"b\": 2}"},
	{"a:\n  - 1\n  - 2\nb: 2\n", "{\"a\": [1, 2], \"b\": 2}"},
	{"a:\n- 1\nb: 2\nc: 3\n", "{\"a\": [1], \"b\": 2, \"c\": 3}"},
	{"x:\n  y:\n  - 1\n  z: 2\n", "{\"x\": {\"y\": [1], \"z\": 2}}"},
	{"x:\n  y:\n    - 1\n  z: 2\n", "{\"x\": {\"y\": [1], \"z\": 2}}"},
	{"a:\n- 1\n- 2\n", "{\"a\": [1, 2]}"},
	{"a:\n- - 1\n- 2\nb: 3\n", "{\"a\": [[1], 2], \"b\": 3}"},
	{"a:\n- x: 1\n  y: 2\nb: 3\n", "{\"a\": [{\"x\": 1, \"y\": 2}], \"b\": 3}"},
	{"a:\n- x: 1\nb: 3\n", "{\"a\": [{\"x\": 1}], \"b\": 3}"},
	{"a:\n  b: 1\n", "{\"a\": {\"b\": 1}}"},
	{"a: 1\nb: 2\n", "{\"a\": 1, \"b\": 2}"},
	{"a:\n  b: 1\n  c: 2\n", "{\"a\": {\"b\": 1, \"c\": 2}}"},
	{"a:\n  b: 1\nc: 2\n", "{\"a\": {\"b\": 1}, \"c\": 2}"},
	{"a: 1\n  b: 2\n", nullptr},
	{"a: 1\n b: 2\n", nullptr},
	{"a: 1\n\n  b: 2\n", nullptr},
	{"a: {x: 1}\n  b: 2\n", nullptr},
};

} // namespace

TEST(YamlBlockStructure, MatchesReferenceImplementation) {
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

/* A scalar indented past its block mapping is that mapping's value, and only
   while a key above is still waiting for one. With every key already paired
   there is nothing for it to be, and it used to become a trailing key with a
   null value - so a block scalar followed by a line that dedents out of it,
   but not back to the mapping, produced {"a": "deep\n", "shallow": null} for
   input neither PyYAML nor js-yaml accepts. */
TEST(YamlBlockStructure, RefusesScalarsWithNoPlaceToGo) {
	EXPECT_EQ(Render("a: |\n    deep\n  shallow\n"), std::string(""));
	EXPECT_EQ(Render("a: |\n  one\n b\n"), std::string(""));
	EXPECT_EQ(Render("a: >\n    one\n  two\n"), std::string(""));
	/* A plain value does continue onto an indented line, so this one stands:
	   the rule above fires only where the value is already complete. */
	EXPECT_EQ(Render("a: 1\n  b\n"), std::string("{\"a\": \"1 b\"}"));
}

/* The column that decides it is the line's first non-space, not the scalar's
   own: a tag or anchor sits before the scalar and belongs to the same node,
   so "!!str true" as a key starts where the tag does. */
TEST(YamlBlockStructure, ATaggedKeyStartsWhereItsTagDoes) {
	EXPECT_EQ(Render("true: 1\n!!str true: 2\n"),
		std::string("{true: 1, \"true\": 2}"));
	EXPECT_EQ(Render("a: 1\n&x b: 2\n"), std::string("{\"a\": 1, \"b\": 2}"));
}

/* A key inside a block sequence entry whose scalar is already complete. */
TEST(YamlBlockStructure, RefusesAKeyInsideACompleteSequenceEntry) {
	EXPECT_EQ(Render("a:\n- 1\n  b: 2\n"), std::string(""));
}

/* The scalar before a ":" is its key, held provisionally as the previous
   key's value until the ":" claims it. With none outstanding the ":" has no
   key *on its line*, and "key: a : b" used to yield {"key": "a", "b": null} -
   the tail of a value silently turned into a pair. A ":" with no space after
   it is ordinary content and is unaffected.

   A ":" that begins its own line is the other case and is not an error:
   c-l-block-map-implicit-entry's other arm is e-node (8.2.2), so the entry's
   key is the one nobody wrote.  "a: 1" over ": 2" is {"a": 1, null: 2}, which
   yaml-test-suite case NKF9 spells out.  Both this and the line above were
   refused with the same message until the event stream asked. */
TEST(YamlBlockStructure, RefusesAColonWithNoKeyBeforeIt) {
	EXPECT_EQ(Render("key: a : b\n"), std::string(""));
	EXPECT_EQ(Render("key: a: b\n"), std::string(""));
	EXPECT_EQ(Render("key: a :b\n"), std::string("{\"key\": \"a :b\"}"));
	EXPECT_EQ(Render("a: 1\nb: 2\n"), std::string("{\"a\": 1, \"b\": 2}"));
	EXPECT_EQ(Render("? a\n: 1\nb: 2\n"), std::string("{\"a\": 1, \"b\": 2}"));
	EXPECT_EQ(Render("a: 1\n: 2\n"), std::string("{\"a\": 1, null: 2}"));
}

/* A document has one root node (3.2.1). Every place that finished a node at
   the top level simply assigned it as the root, so a second one overwrote
   the first and the first vanished - the sequence in the first case below
   was returned as {"invalid": "x"}, with nothing to say two thirds of the
   document had been dropped. */
TEST(YamlBlockStructure, RefusesASecondTopLevelNode) {
	EXPECT_EQ(Render("- a\n- b\ninvalid: x\n"), std::string(""));
	EXPECT_EQ(Render("a: 1\n- b\n"), std::string(""));
	EXPECT_EQ(Render("[a, b]\n[c]\n"), std::string(""));
	EXPECT_EQ(Render("{a: 1}\nb\n"), std::string(""));

	/* What still has to work: one root, however it is spelled. A scalar held
	   provisionally as the root and then claimed by a ":" is the ordinary
	   mapping path and must not trip this. */
	EXPECT_EQ(Render("a: 1\nb: 2\n"), std::string("{\"a\": 1, \"b\": 2}"));
	EXPECT_EQ(Render("- a\n- b\n"), std::string("[\"a\", \"b\"]"));
	EXPECT_EQ(Render("scalar\n"), std::string("\"scalar\""));
	EXPECT_EQ(Render("a:\n  b:\n    c: 1\n"),
		std::string("{\"a\": {\"b\": {\"c\": 1}}}"));
	EXPECT_EQ(Render("- - a\n- b\n"), std::string("[[\"a\"], \"b\"]"));
}

/* The same rule, reached through an explicit key, which used to walk around it.
 *
 * Where the scalar before a ":" was placed decides how it is taken back, and
 * detach_last_scalar() trusted a flag for the root case instead of checking that
 * the root was still that scalar. The flag outlives the scalar: it was left set
 * after the first key was claimed, and the paths that hold a scalar as an
 * *explicit* key never touched it. So a scalar under a "?" could arrive carrying
 * an earlier scalar's "I am the root" and clear a root that a finished collection
 * had since filled.
 *
 * " 1:" over "? 2:" is the shape: the dedent closes the indent-1 mapping onto the
 * root, and the ":" after the explicit key then discards it. The document was
 * accepted as {"1": {"2": null}} - the two collections silently folded into one,
 * which is the same data loss this whole test is about.
 *
 * The anchored spelling is what found it, and it is worse than a wrong tree: with
 * "&O" on the node that gets dropped, the alias that names it stays in the DOM
 * while its anchor leaves, so the writer emits "*O" against no "&O" anywhere and
 * produces a document its own parser refuses. A twelve-hour fuzz run over the
 * writers reached it in three and a half million executions.
 *
 * PyYAML refuses every input below and accepts every one in the block after,
 * which is where the line between the two came from.
 */
TEST(YamlBlockStructure, RefusesASecondTopLevelNodeUnderAnExplicitKey) {
	EXPECT_EQ(Render(" 1:\n? 2:\n"), std::string(""));
	EXPECT_EQ(Render(" 1:\n? 2\n"), std::string(""));
	EXPECT_EQ(Render("&O\n 1:\n? *O\n"), std::string(""));
	EXPECT_EQ(Render("&O\n 1:\n? *O\n? .:\n"), std::string(""));
	/* No trailing newline either: the dedent is the last thing in the input. */
	EXPECT_EQ(Render(" 1:\n? 2:"), std::string(""));

	/* What still has to work: an explicit key is an ordinary way to write a
	   mapping entry, and none of these may trip the rule above. A fix that
	   refused the "?" at the top level outright would pass the block above and
	   break every one of these. */
	EXPECT_EQ(Render("? a\n: 1\nb: 2\n"), std::string("{\"a\": 1, \"b\": 2}"));
	EXPECT_EQ(Render("a: 1\n? b\n: 2\n"), std::string("{\"a\": 1, \"b\": 2}"));
	EXPECT_EQ(Render("? a\n? b\n"), std::string("{\"a\": null, \"b\": null}"));
	EXPECT_EQ(Render("? a\n: 1\n"), std::string("{\"a\": 1}"));
	EXPECT_EQ(Render("&O a\n"), std::string("\"a\""));
}

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
