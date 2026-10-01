/**
 * @file test-yaml-11-mode.cpp
 * @brief Tests for YAML 1.1 compatibility mode.
 */

#include <gtest/gtest.h>
#include <string.h>
#include <string>

extern "C" {
#include <ghoti.io/text/yaml.h>
}

static GTEXT_YAML_Document *parse_yaml(const char *yaml, GTEXT_YAML_Parse_Options *opts) {
	GTEXT_YAML_Error err = {};
	return gtext_yaml_parse(yaml, strlen(yaml), opts, &err);
}

TEST(Yaml11Mode, DirectiveEnablesBooleans) {
	const char *yaml = "%YAML 1.1\n---\nyes\n";
	GTEXT_YAML_Document *doc = parse_yaml(yaml, NULL);
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(gtext_yaml_node_type(gtext_yaml_document_root(doc)), GTEXT_YAML_BOOL);
	gtext_yaml_free(doc);
}

TEST(Yaml11Mode, DirectiveEnablesOctal) {
	const char *yaml = "%YAML 1.1\n---\n0755\n";
	GTEXT_YAML_Document *doc = parse_yaml(yaml, NULL);
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(gtext_yaml_node_type(gtext_yaml_document_root(doc)), GTEXT_YAML_INT);
	gtext_yaml_free(doc);
}

TEST(Yaml11Mode, DefaultTreatsOctalAsString) {
	const char *yaml = "0755";
	GTEXT_YAML_Document *doc = parse_yaml(yaml, NULL);
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(gtext_yaml_node_type(gtext_yaml_document_root(doc)), GTEXT_YAML_STRING);
	gtext_yaml_free(doc);
}

TEST(Yaml11Mode, DirectiveEnablesSexagesimalInt) {
	const char *yaml = "%YAML 1.1\n---\n190:20:30\n";
	GTEXT_YAML_Document *doc = parse_yaml(yaml, NULL);
	ASSERT_NE(doc, nullptr);
	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	EXPECT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_INT);
	/* The type on its own says nothing about whether the digits arrived.
	   190*3600 + 20*60 + 30. */
	int64_t value = 0;
	EXPECT_TRUE(gtext_yaml_node_as_int(root, &value));
	EXPECT_EQ(value, 685230);
	gtext_yaml_free(doc);
}

TEST(Yaml11Mode, DirectiveEnablesSexagesimalFloat) {
	const char *yaml = "%YAML 1.1\n---\n1:20:30.5\n";
	GTEXT_YAML_Document *doc = parse_yaml(yaml, NULL);
	ASSERT_NE(doc, nullptr);
	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	EXPECT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_FLOAT);
	double value = 0.0;
	EXPECT_TRUE(gtext_yaml_node_as_float(root, &value));
	EXPECT_DOUBLE_EQ(value, 1 * 3600 + 20 * 60 + 30.5);
	gtext_yaml_free(doc);
}

/* A sexagesimal whole number past what int64_t holds has no integer to be
   converted to, and converting it anyway is undefined - 6.3.1.4 - which on
   x86-64 means INT64_MIN, and that is what came back.
   "1:99999999999999999999999999999999" resolved to the integer
   -9223372036854775808.

   A decimal too large has always been handled: strtoll() reports ERANGE,
   parse_int_value() gives up, and the scalar stays the string it was written
   as. The sexagesimal rows are built in floating point and never asked. They
   answer the same way now - and the same way as each other, which is the
   half of this that a type check alone would not have caught. */
TEST(Yaml11Mode, ASexagesimalTooLargeForTheTypeIsNotAnInt) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.yaml_1_1 = true;

	const char *too_big[] = {
		"1:99999999999999999999999999999999",
		"-1:99999999999999999999999999999999",
		"99999999999999999999999999999999:0",
	};
	for (const char *text : too_big) {
		GTEXT_YAML_Document *doc = parse_yaml(text, &opts);
		ASSERT_NE(doc, nullptr) << text;
		const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
		ASSERT_NE(root, nullptr) << text;
		EXPECT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_STRING)
			<< text << " resolved to a number it cannot hold";
		int64_t value = 0;
		EXPECT_FALSE(gtext_yaml_node_as_int(root, &value)) << text;
		gtext_yaml_free(doc);

		/* And the decimal it is being made to match. */
		GTEXT_YAML_Document *dec = parse_yaml("99999999999999999999999", &opts);
		ASSERT_NE(dec, nullptr);
		EXPECT_EQ(gtext_yaml_node_type(gtext_yaml_document_root(dec)),
			GTEXT_YAML_STRING);
		gtext_yaml_free(dec);
	}
}

/* Where the tag says int, there is no string to fall back to, so it is
   refused - which is what "!!int 99999999999999999999999" already does. */
TEST(Yaml11Mode, AnExplicitIntTagRefusesASexagesimalTooLargeForTheType) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.yaml_1_1 = true;

	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	const char *text = "!!int 1:99999999999999999999999999999999";
	GTEXT_YAML_Document *doc =
		gtext_yaml_parse(text, strlen(text), &opts, &err);
	EXPECT_EQ(doc, nullptr);
	if (doc) gtext_yaml_free(doc);
	else EXPECT_EQ(err.code, GTEXT_YAML_E_INVALID);
	gtext_yaml_error_free(&err);

	/* One that does fit still resolves, so this is a bound and not a ban. */
	memset(&err, 0, sizeof(err));
	const char *ok = "!!int 1:30";
	doc = gtext_yaml_parse(ok, strlen(ok), &opts, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");
	int64_t value = 0;
	EXPECT_TRUE(gtext_yaml_node_as_int(gtext_yaml_document_root(doc), &value));
	EXPECT_EQ(value, 90);
	gtext_yaml_free(doc);
	gtext_yaml_error_free(&err);
}

TEST(Yaml11Mode, OptionForcesCompatibility) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.yaml_1_1 = true;

	GTEXT_YAML_Document *doc = parse_yaml("on", &opts);
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(gtext_yaml_node_type(gtext_yaml_document_root(doc)), GTEXT_YAML_BOOL);
	gtext_yaml_free(doc);
}

TEST(Yaml11Mode, ExplicitTagOverridesImplicit) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.yaml_1_1 = true;

	GTEXT_YAML_Document *doc = parse_yaml("!!str yes", &opts);
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(gtext_yaml_node_type(gtext_yaml_document_root(doc)), GTEXT_YAML_STRING);
	gtext_yaml_free(doc);
}

/* A 1.1 sexagesimal has to have every digit its row asks for.
 *
 * 10.3.2's int row is "[-+]? [1-9] [0-9_]* (: [0-5]? [0-9])+" and its float
 * row "[-+]? [0-9] [0-9_]* (: [0-5]? [0-9])+ \. [0-9_]*", so a colon with
 * nothing after it is not part of either, and neither is a final segment made
 * only of punctuation.  parse_sexagesimal_value() said so for an interior
 * empty segment - "1::2" was already a string - and its loop condition ended
 * the walk before the guard could be reached for a trailing one.  "-4:" was
 * the integer -4, "1:2:" was 3720, and "-4:." was -240.
 *
 * The pair is what makes a wrong type into a lost value.  A plain scalar
 * ending in ":" has nothing ns-plain-safe after the colon, so the writer must
 * quote it, and a quoted scalar is a string: a document holding the "integer"
 * -4: came back holding the string "-4:".  Both halves were behaving; the int
 * was never a number.  The yaml-writer fuzzer reported it against the writer,
 * because a round trip can only say that the two disagreed.
 *
 * A trailing dot is the other half of the same reading.  The float row's
 * fraction digits are optional, so "1:5." is the float 65 - counting fraction
 * digits instead of noticing the dot made it the integer 65, and the dot then
 * had nowhere to go in the output. */
namespace {

/* A plain scalar whose text ends in ":" has no block-context spelling - there
   the colon is the mapping indicator - so these are built in a flow mapping,
   where 7.3.3 admits ":" inside the scalar when an ns-plain-safe character
   follows it and the last colon on the line is the indicator.

   The text is asserted and not assumed.  This test asks what the resolver
   makes of a given spelling, and a scanner that handed it a different one
   would turn it into a test of something else without saying so. */
const GTEXT_YAML_Node *plain_scalar_in_flow(
	GTEXT_YAML_Document **doc_out, const char *text, bool yaml_1_1
) {
	std::string input = std::string("[k\t: ") + text + ":\n]";
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.yaml_1_1 = yaml_1_1;
	opts.schema = GTEXT_YAML_SCHEMA_CORE;
	GTEXT_YAML_Document *doc =
		gtext_yaml_parse(input.data(), input.size(), &opts, nullptr);
	*doc_out = doc;
	if (!doc) return nullptr;
	const GTEXT_YAML_Node *seq = gtext_yaml_document_root(doc);
	if (!seq || gtext_yaml_node_type(seq) != GTEXT_YAML_SEQUENCE) return nullptr;
	const GTEXT_YAML_Node *map = gtext_yaml_sequence_get(seq, 0);
	if (!map || gtext_yaml_node_type(map) != GTEXT_YAML_MAPPING) return nullptr;
	const GTEXT_YAML_Node *key = nullptr, *value = nullptr;
	gtext_yaml_mapping_get_at(map, 0, &key, &value);
	return value;
}

}  // namespace

TEST(Yaml11Mode, ASexagesimalNeedsEveryDigitItsRowAsksFor) {
	struct Case { const char *text; GTEXT_YAML_Node_Type type; };
	const Case cases[] = {
		/* The shapes that are not numbers.  Every one of these is a string in
		   PyYAML, which is a 1.1 implementation, and in 10.3.2 read directly. */
		{ "-4:",   GTEXT_YAML_STRING },
		{ "4:",    GTEXT_YAML_STRING },
		{ "1:",    GTEXT_YAML_STRING },
		{ "+4:",   GTEXT_YAML_STRING },
		{ "1:2:",  GTEXT_YAML_STRING },
		{ "-4:.",  GTEXT_YAML_STRING },
		{ "1:.",   GTEXT_YAML_STRING },
		/* Already a string before this, and here so that a fix which turns
		   the guard off rather than reaching it cannot pass. */
		{ "1::2",  GTEXT_YAML_STRING },
		{ ":30",   GTEXT_YAML_STRING },
		/* And the ones that are numbers, so that refusing the family outright
		   cannot pass either. */
		{ "1:2",       GTEXT_YAML_INT },
		{ "-1:30",     GTEXT_YAML_INT },
		{ "1:2:3",     GTEXT_YAML_INT },
		{ "190:20:30", GTEXT_YAML_INT },
		{ "1:5.5",     GTEXT_YAML_FLOAT },
		/* The float row's fraction digits are optional. */
		{ "1:5.",      GTEXT_YAML_FLOAT },
	};

	for (const Case &c : cases) {
		GTEXT_YAML_Document *doc = nullptr;
		const GTEXT_YAML_Node *value = plain_scalar_in_flow(&doc, c.text, true);
		ASSERT_NE(value, nullptr) << c.text;
		/* The instrument first: this only says anything if the scalar the
		   resolver saw is the one the row is about. */
		const char *got = gtext_yaml_node_as_string(value);
		ASSERT_NE(got, nullptr) << c.text;
		ASSERT_STREQ(got, c.text) << "the scanner handed the resolver other text";
		EXPECT_EQ(gtext_yaml_node_type(value), c.type) << c.text;
		gtext_yaml_free(doc);

		/* 1.2 has no sexagesimal row at all, so every one of them is a string
		   there - the control that says the 1.1 answers above are the dialect
		   and not the spelling. */
		GTEXT_YAML_Document *d12 = nullptr;
		const GTEXT_YAML_Node *v12 = plain_scalar_in_flow(&d12, c.text, false);
		ASSERT_NE(v12, nullptr) << c.text;
		EXPECT_EQ(gtext_yaml_node_type(v12), GTEXT_YAML_STRING) << c.text;
		gtext_yaml_free(d12);
	}
}

/* And the property the fuzzer was checking: the type survives being written
   and read back.  It cannot, for a text the writer has to quote, so this is
   the assertion that the resolver stopped producing one. */
TEST(Yaml11Mode, ASexagesimalKeepsItsTypeAcrossARoundTrip) {
	const char *texts[] = {
		"-4:", "4:", "1:2:", "-4:.", "1:5.", "1:2", "-1:30", "1:2:3", "1:5.5",
	};
	for (const char *text : texts) {
		GTEXT_YAML_Document *doc = nullptr;
		const GTEXT_YAML_Node *value = plain_scalar_in_flow(&doc, text, true);
		ASSERT_NE(value, nullptr) << text;
		const GTEXT_YAML_Node_Type before = gtext_yaml_node_type(value);

		GTEXT_YAML_Sink sink;
		ASSERT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
		GTEXT_YAML_Write_Options wopts = gtext_yaml_write_options_default();
		wopts.yaml_1_1 = true;
		wopts.schema = GTEXT_YAML_SCHEMA_CORE;
		ASSERT_EQ(gtext_yaml_write_document(doc, &sink, &wopts), GTEXT_YAML_OK)
			<< text;
		std::string out(gtext_yaml_sink_buffer_data(&sink),
			gtext_yaml_sink_buffer_size(&sink));
		gtext_yaml_sink_buffer_free(&sink);
		gtext_yaml_free(doc);

		GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
		popts.yaml_1_1 = true;
		popts.schema = GTEXT_YAML_SCHEMA_CORE;
		GTEXT_YAML_Document *back =
			gtext_yaml_parse(out.data(), out.size(), &popts, nullptr);
		ASSERT_NE(back, nullptr) << text << " wrote <<" << out << ">>";
		const GTEXT_YAML_Node *seq = gtext_yaml_document_root(back);
		ASSERT_NE(seq, nullptr) << text;
		const GTEXT_YAML_Node *map = gtext_yaml_sequence_get(seq, 0);
		ASSERT_NE(map, nullptr) << text;
		const GTEXT_YAML_Node *k = nullptr, *v = nullptr;
		gtext_yaml_mapping_get_at(map, 0, &k, &v);
		ASSERT_NE(v, nullptr) << text;
		EXPECT_EQ(gtext_yaml_node_type(v), before)
			<< "<<" << text << ">> was written <<" << out << ">>";
		gtext_yaml_free(back);
	}
}

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
