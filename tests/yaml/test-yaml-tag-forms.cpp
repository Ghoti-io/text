/**
 * The three spellings of a tag property (5.3).
 *
 * c-ns-tag-property is a verbatim tag, a shorthand, or the non-specific tag:
 *
 *   !<tag:yaml.org,2002:str>   the URI exactly as written, no handle
 *   !!str / !foo               a shorthand, expanded through a handle
 *   !                          the non-specific tag, which is the whole
 *                              property with the node after it
 *
 * Only the shorthand worked. The stream read the token after a "!" as the
 * tag's name whichever form it was, so "! a" used its own node as the tag
 * name and came back as null - three suite cases, including spec example
 * 6.28, where "! 12" is the string "12" beside the integer 12. And the
 * scanner had no idea what "!<...>" was: the brackets are not plain
 * characters and the ":" inside a URI is not a key separator, so
 * "!<tag:yaml.org,2002:str> foo" was read as a plain scalar beginning part
 * way through the URI.
 */
#include <gtest/gtest.h>
#include <string.h>
#include <string>
#include <ghoti.io/text/yaml.h>

#include "yaml_render.h"

namespace {

struct Case {
	const char *input;
	const char *expected;
};

const Case kCases[] = {
	/* The non-specific tag stands alone and the node follows it. */
	{"! a\n", "\"a\""},
	{"---\n! a\n", "\"a\""},
	{"- \"12\"\n- 12\n- ! 12\n", "[\"12\", 12, \"12\"]"},  /* spec example 6.28 */
	{"a: ! 1\n", "{\"a\": \"1\"}"},

	/* Verbatim tags keep the URI and resolve by it. */
	{"!<tag:yaml.org,2002:str> 12\n", "\"12\""},
	{"!<tag:yaml.org,2002:int> 12\n", "12"},
	{"!<tag:x> a\n", "\"a\""},
	/* Spec example 6.24: a verbatim tag on a key and a local one on its
	   value, with colons inside the URI that are not key separators. */
	{"!<tag:yaml.org,2002:str> foo :\n  !<!bar> baz\n", "{\"foo\": \"baz\"}"},

	/* Shorthands, unchanged. */
	{"!!str 12\n", "\"12\""},
	{"!!int 12\n", "12"},
	{"!foo bar\n", "\"bar\""},
	{"a: !!str 1\n", "{\"a\": \"1\"}"},
};

}  // namespace

TEST(YamlTagForms, AllThreeSpellings) {
	for (const Case &c : kCases) {
		EXPECT_EQ(Render(c.input), std::string(c.expected))
			<< "input: " << ::testing::PrintToString(std::string(c.input));
	}
}

/* A handle has to be up against its name.
 *
 * c-ns-shorthand-tag is c-tag-handle ns-tag-char+ (5.6) with nothing allowed
 * between the two, and a space is not an ns-tag-char - so a space after "!!"
 * ends the tag and leaves the handle with no name at all.
 *
 * This file's own opening paragraph records the same defect for the single
 * "!": the stream read the token after it as the tag's name whichever form it
 * was, so "! a" used its own node as the tag name. The "!!" handle kept that
 * behaviour. stream_scan() skips whitespace, so it answers with the next
 * token whatever stands in front of it, and "!! str" glued that token on as
 * the suffix: the tag became "!!str" and the node became *empty*, with the
 * document's only value swallowed into the tag name. Accepted, and silently
 * wrong - PyYAML refuses every input below ("expected URI, but found ' '").
 *
 * "a: !! b" and "- !! x" were already refused and that hid this: the glued
 * name failed a namespace lookup afterwards, so the error named a tag nobody
 * had written rather than the malformed handle. Against the unfixed stream
 * only three of the seven refusals below actually fail - the three where the
 * glued name happens to be a *defined* tag, "!!str" - so four of them would
 * have passed while the defect was fully present, and a shorter list of
 * inputs could easily have been all four.
 *
 * Found by the yaml-writer fuzz leg on the EVO-X2 at 578,031 executions,
 * through the event pipe rather than the DOM: the streaming writer re-emitted
 * the swallowed scalar, so the round trip disagreed with itself.
 */
TEST(YamlTagForms, AShorthandHandleNeedsANameAgainstIt) {
	EXPECT_EQ(Render("!! str\n"), std::string(""));
	EXPECT_EQ(Render("!!  str\n"), std::string(""));
	EXPECT_EQ(Render("!!\n"), std::string(""));
	EXPECT_EQ(Render("!! \n"), std::string(""));
	EXPECT_EQ(Render("a: !! b\n"), std::string(""));
	EXPECT_EQ(Render("- !! x\n"), std::string(""));
	/* No trailing newline either. */
	EXPECT_EQ(Render("!! str"), std::string(""));

	/* What still has to work, and it is the whole reason this is an adjacency
	   test rather than a ban: the shorthand itself, the node that follows it,
	   and the non-specific "!" which is *defined* by standing apart from its
	   node. A fix that refused a space after any "!" would break the four
	   cases in kCases above. */
	EXPECT_EQ(Render("!!str 12\n"), std::string("\"12\""));
	EXPECT_EQ(Render("!!int 12\n"), std::string("12"));
	EXPECT_EQ(Render("!!str\n"), std::string("\"\""));
	EXPECT_EQ(Render("! str\n"), std::string("\"str\""));
	EXPECT_EQ(Render("a: !!str 1\n"), std::string("{\"a\": \"1\"}"));
}

/* A verbatim tag that never closes is an error rather than a scalar. */
TEST(YamlTagForms, AnUnterminatedVerbatimTagIsRefused) {
	EXPECT_EQ(Render("!<tag:x a\n"), std::string(""));
	EXPECT_EQ(Render("!<tag:x\n"), std::string(""));
	/* A ">" on a later line does not close it: a tag property sits on one
	   line, so this is unterminated rather than a two-line URI. */
	EXPECT_EQ(Render("!<tag:x\nfoo> a\n"), std::string(""));
}

/* Only a plain scalar is resolved by its contents. Every other style
   carries the non-specific tag "!", which for a scalar is
   tag:yaml.org,2002:str (10.3.2) - quoting something is how you say it is a
   string. The style was not consulted at all, so 'a: "12"' came back as the
   integer 12 and 'a: "null"' as null, and 1305 tests went past it. */
TEST(YamlTagForms, OnlyPlainScalarsAreResolvedByTheirContents) {
	static const Case kTyped[] = {
		{"a: \"12\"\n", "{\"a\": \"12\"}"},
		{"a: '12'\n", "{\"a\": \"12\"}"},
		{"a: \"true\"\n", "{\"a\": \"true\"}"},
		{"a: 'true'\n", "{\"a\": \"true\"}"},
		{"a: \"null\"\n", "{\"a\": \"null\"}"},
		{"a: \"1.5\"\n", "{\"a\": \"1.5\"}"},
		{"a: |\n  12\n", "{\"a\": \"12\\n\"}"},
		{"a: >\n  12\n", "{\"a\": \"12\\n\"}"},

		/* Plain scalars still are. */
		{"a: 12\n", "{\"a\": 12}"},
		{"a: true\n", "{\"a\": true}"},
		{"a: null\n", "{\"a\": null}"},
		{"a: 1.5\n", "{\"a\": 1.5}"},

		/* And an explicit tag still wins over the style. */
		{"a: !!int \"12\"\n", "{\"a\": 12}"},
		{"a: !!str 12\n", "{\"a\": \"12\"}"},
	};
	for (const Case &c : kTyped) {
		EXPECT_EQ(Render(c.input), std::string(c.expected))
			<< "input: " << ::testing::PrintToString(std::string(c.input));
	}
}

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
