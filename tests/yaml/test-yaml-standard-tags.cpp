#include <gtest/gtest.h>
#include <string.h>
#include <string>

extern "C" {
#include <ghoti.io/text/yaml.h>
}

TEST(YamlStandardTags, TimestampValidDate) {
	const char *yaml = "!!timestamp 2025-02-14";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	EXPECT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");
	gtext_yaml_free(doc);
}

TEST(YamlStandardTags, TimestampValidDateTime) {
	const char *yaml = "!!timestamp 2025-02-14T10:30:45Z";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");
	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ(gtext_yaml_node_as_string(root), "2025-02-14T10:30:45Z");
	gtext_yaml_free(doc);
}

TEST(YamlStandardTags, TimestampInvalid) {
	const char *yaml = "!!timestamp 2025-13-40";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	EXPECT_EQ(doc, nullptr);
	EXPECT_EQ(err.code, GTEXT_YAML_E_INVALID);
}

TEST(YamlStandardTags, TimestampNormalizesOffset) {
	const char *yaml = "!!timestamp 2025-02-14 10:30:00+02";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");
	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ(gtext_yaml_node_as_string(root), "2025-02-14T10:30:00+02:00");
	gtext_yaml_free(doc);
}

/**
 * The seconds are not optional.
 *
 * This case was written as `10:30+02` and expected to pass, which it did while
 * this library parsed timestamps with a parser of its own. YAML 1.1's
 * expression is `:[0-9][0-9] :[0-9][0-9]` - two colon-separated pairs, neither
 * optional - so `2025-02-14 10:30+02` is a `!!str`, and PyYAML resolves it as
 * one. The test asserted what the code did rather than what the specification
 * says, which is the failure mode of a test that follows its code; it now asserts
 * both halves.
 */
TEST(YamlStandardTags, TimestampRequiresSeconds) {
	const char *yaml = "!!timestamp 2025-02-14 10:30+02";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	EXPECT_EQ(doc, nullptr);
	EXPECT_EQ(err.code, GTEXT_YAML_E_INVALID);
	gtext_yaml_free(doc);
}

/**
 * The spellings YAML 1.1 permits that the previous parser refused.
 *
 * Each of these is a conformant `!!timestamp` that this library rejected until
 * the grammar moved to chron, and each was found by running the type
 * repository's own expression - through PyYAML - rather than by reading the
 * code again.
 */
TEST(YamlStandardTags, TimestampAcceptsTheRelaxedSpellings) {
	struct Case {
		const char *text;
		const char *normalised;
	};
	const Case cases[] = {
		/* A one- or two-digit month, day and hour, but only with a time. */
		{"2001-12-4T21:59:43Z", "2001-12-04T21:59:43Z"},
		{"2001-1-4T2:59:43Z", "2001-01-04T02:59:43Z"},
		/* One *or more* spaces or tabs in place of `T`. */
		{"2001-12-14   21:59:43Z", "2001-12-14T21:59:43Z"},
		{"2001-12-14\t21:59:43Z", "2001-12-14T21:59:43Z"},
		/* Whitespace before the zone. */
		{"2001-12-14T21:59:43 Z", "2001-12-14T21:59:43Z"},
		{"2001-12-14T21:59:43 -05:00", "2001-12-14T21:59:43-05:00"},
		/* An offset may write one digit of hour, or omit its minutes. */
		{"2001-12-14T21:59:43-5", "2001-12-14T21:59:43-05:00"},
		{"2001-12-14T21:59:43+05", "2001-12-14T21:59:43+05:00"},
		/* A fraction may carry no digits at all. */
		{"2001-12-14T21:59:43.Z", "2001-12-14T21:59:43Z"},
	};

	for (const Case &c : cases) {
		std::string yaml = std::string("!!timestamp \"") + c.text + "\"";
		GTEXT_YAML_Error err = {};
		GTEXT_YAML_Document *doc =
			gtext_yaml_parse(yaml.c_str(), yaml.size(), NULL, &err);
		ASSERT_NE(doc, nullptr)
			<< c.text << ": " << (err.message ? err.message : "parse failed");
		const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
		ASSERT_NE(root, nullptr) << c.text;
		EXPECT_STREQ(gtext_yaml_node_as_string(root), c.normalised) << c.text;
		gtext_yaml_free(doc);
	}
}

/**
 * `z` is not `Z`, which is the reverse of RFC 3339.
 *
 * YAML's expression writes `[Tt]` for the separator and a bare `Z` for the
 * zone. The previous parser accepted both cases of both, carrying RFC 3339's
 * case-insensitivity (RFC 5234 section 2.3) into a grammar that does not have
 * it.
 */
TEST(YamlStandardTags, TimestampSeparatorIsCaseInsensitiveAndZoneIsNot) {
	const char *lower_t = "!!timestamp 2001-12-14t21:59:43Z";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc =
		gtext_yaml_parse(lower_t, strlen(lower_t), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");
	EXPECT_STREQ(gtext_yaml_node_as_string(gtext_yaml_document_root(doc)),
		"2001-12-14T21:59:43Z");
	gtext_yaml_free(doc);

	const char *lower_z = "!!timestamp 2001-12-14T21:59:43z";
	GTEXT_YAML_Error err2 = {};
	GTEXT_YAML_Document *doc2 =
		gtext_yaml_parse(lower_z, strlen(lower_z), NULL, &err2);
	EXPECT_EQ(doc2, nullptr);
	EXPECT_EQ(err2.code, GTEXT_YAML_E_INVALID);
	gtext_yaml_free(doc2);
}

/**
 * A date with no time needs two digits; one carrying a time does not.
 *
 * YAML's first alternative is `YYYY-MM-DD` with both fields exactly two
 * digits, and only the second relaxes them. It reads as an inconsistency and
 * is what the type repository says.
 */
TEST(YamlStandardTags, TimestampDateOnlyNeedsTwoDigits) {
	const char *narrow = "!!timestamp 2001-12-4";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc =
		gtext_yaml_parse(narrow, strlen(narrow), NULL, &err);
	EXPECT_EQ(doc, nullptr);
	EXPECT_EQ(err.code, GTEXT_YAML_E_INVALID);
	gtext_yaml_free(doc);
}

/**
 * The three shapes reach the caller as the chron value they were read as.
 *
 * In particular a timestamp with no zone is **not** UTC: the document did not
 * say which zone it meant, and a reader that decided for it would be inventing
 * the missing half.
 */
TEST(YamlStandardTags, TimestampReportsItsShape) {
	struct Case {
		const char *text;
		GCHRON_YamlKind kind;
	};
	const Case cases[] = {
		{"2001-12-14", GCHRON_YAML_DATE},
		{"2001-12-14T21:59:43", GCHRON_YAML_DATE_TIME},
		{"2001-12-14T21:59:43Z", GCHRON_YAML_OFFSET_DATE_TIME},
		{"2001-12-14T21:59:43-05:00", GCHRON_YAML_OFFSET_DATE_TIME},
	};

	for (const Case &c : cases) {
		std::string yaml = std::string("!!timestamp \"") + c.text + "\"";
		GTEXT_YAML_Error err = {};
		GTEXT_YAML_Document *doc =
			gtext_yaml_parse(yaml.c_str(), yaml.size(), NULL, &err);
		ASSERT_NE(doc, nullptr) << c.text;
		const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
		GCHRON_YamlValue value;
		ASSERT_TRUE(gtext_yaml_node_timestamp_value(root, &value)) << c.text;
		EXPECT_EQ(value.kind, c.kind) << c.text;
		gtext_yaml_free(doc);
	}
}

/**
 * A `:60` second is kept exactly as the document wrote it.
 *
 * The grammar permits it and chron holds it as `:59` of the same minute -
 * there is nowhere else for a sixtieth second to go. Re-emitting the value
 * would move the reading a second earlier with nothing to show for it, so the
 * scalar keeps its own text and the flag says why the fields disagree with it.
 */
TEST(YamlStandardTags, TimestampKeepsALeapSecondVerbatim) {
	const char *yaml = "!!timestamp 1998-12-31T23:59:60Z";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");
	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ(gtext_yaml_node_as_string(root), "1998-12-31T23:59:60Z");
	EXPECT_TRUE(gtext_yaml_node_timestamp_is_leap_second(root));

	GTEXT_YAML_Timestamp view;
	ASSERT_TRUE(gtext_yaml_node_as_timestamp(root, &view));
	EXPECT_EQ(view.second, 59);
	EXPECT_TRUE(view.leap_second);
	gtext_yaml_free(doc);
}

TEST(YamlStandardTags, TimestampNormalizesFraction) {
	const char *yaml = "!!timestamp 2025-02-14T10:30:45.5000Z";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");
	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ(gtext_yaml_node_as_string(root), "2025-02-14T10:30:45.5Z");
	gtext_yaml_free(doc);
}

TEST(YamlStandardTags, SetValid) {
	const char *yaml = "!!set {a: ~, b: ~}";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");
	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_SET);
	gtext_yaml_free(doc);
}

TEST(YamlStandardTags, SetInvalidValue) {
	const char *yaml = "!!set {a: 1}";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	EXPECT_EQ(doc, nullptr);
	EXPECT_EQ(err.code, GTEXT_YAML_E_INVALID);
}

TEST(YamlStandardTags, OmapValid) {
	const char *yaml = "!!omap [ {a: 1}, {b: 2} ]";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");
	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_OMAP);
	gtext_yaml_free(doc);
}

TEST(YamlStandardTags, OmapInvalidEntry) {
	const char *yaml = "!!omap [ {a: 1, b: 2} ]";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	EXPECT_EQ(doc, nullptr);
	EXPECT_EQ(err.code, GTEXT_YAML_E_INVALID);
}

TEST(YamlStandardTags, OmapDuplicateKey) {
	const char *yaml = "!!omap [ {a: 1}, {a: 2} ]";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	EXPECT_EQ(doc, nullptr);
	EXPECT_EQ(err.code, GTEXT_YAML_E_DUPKEY);
}

TEST(YamlStandardTags, PairsValid) {
	const char *yaml = "!!pairs [ {a: 1}, {a: 2} ]";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");
	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_PAIRS);
	gtext_yaml_free(doc);
}

TEST(YamlStandardTags, PairsInvalidEntry) {
	const char *yaml = "!!pairs [ {a: 1, b: 2} ]";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	EXPECT_EQ(doc, nullptr);
	EXPECT_EQ(err.code, GTEXT_YAML_E_INVALID);
}

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}

// ---------------------------------------------------------------------------
// Where a tag attaches
//
// A tag applies to the node that follows it.  The scanner can only attach a
// pending tag to the next scalar, because in block context nothing yet says
// whether the node starting there is that scalar or a collection whose first
// key or item it is.  Tags on block collections were therefore dropped: the
// tag stayed on the key or the first item and the collection came out
// untagged, for standard and application tags alike, at the document root and
// nested.
//
// The two shapes that decide it differ in nothing but a line break:
//
//     !custom a: 1     tags the key
//     !custom
//     a: 1             tags the mapping
//
// so GTEXT_YAML_Event carries the line the tag was written on, and the parser
// moves an own-line tag onto the collection it turns out to begin.
//
// Every expectation below was taken from PyYAML's composer on the same input.
// ---------------------------------------------------------------------------

namespace {

const GTEXT_YAML_Node * tag_first_key(const GTEXT_YAML_Node *map) {
	const GTEXT_YAML_Node *k = nullptr;
	if (!gtext_yaml_mapping_get_at(map, 0, &k, nullptr)) {
		return nullptr;
	}
	return k;
}

const char *tag_or_dash(const GTEXT_YAML_Node *n) {
	if (!n) {
		return "<null>";
	}
	const char *t = gtext_yaml_node_tag(n);
	return t ? t : "-";
}

GTEXT_YAML_Document *parse_or_null(const char *src, GTEXT_YAML_Error *err) {
	return gtext_yaml_parse(src, strlen(src), NULL, err);
}

} // namespace

TEST(YamlTagPlacement, OwnLineTagGoesToTheBlockMapping) {
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = parse_or_null("!custom\na: 1\n", &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");

	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_MAPPING);
	EXPECT_STREQ(tag_or_dash(root), "!custom");
	// and not left behind on the key
	EXPECT_STREQ(tag_or_dash(tag_first_key(root)), "-");

	gtext_yaml_free(doc);
	gtext_yaml_error_free(&err);
}

TEST(YamlTagPlacement, SameLineTagStaysOnTheKey) {
	// The counterpart, and the reason the line matters: identical events,
	// different meaning.
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = parse_or_null("!custom a: 1\n", &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");

	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_MAPPING);
	EXPECT_STREQ(tag_or_dash(root), "-") << "the mapping took a key's tag";
	EXPECT_STREQ(tag_or_dash(tag_first_key(root)), "!custom");

	gtext_yaml_free(doc);
	gtext_yaml_error_free(&err);
}

TEST(YamlTagPlacement, OwnLineTagGoesToTheBlockSequence) {
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = parse_or_null("!custom\n- 1\n", &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");

	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_SEQUENCE);
	EXPECT_STREQ(tag_or_dash(root), "!custom");
	ASSERT_EQ(gtext_yaml_sequence_length(root), 1u);
	EXPECT_STREQ(tag_or_dash(gtext_yaml_sequence_get(root, 0)), "-")
	    << "the tag was left on the first item";

	gtext_yaml_free(doc);
	gtext_yaml_error_free(&err);
}

TEST(YamlTagPlacement, NestedBlockCollectionsTakeTheirTag) {
	{
		GTEXT_YAML_Error err = {};
		GTEXT_YAML_Document *doc = parse_or_null("x: !custom\n  - 1\n", &err);
		ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");
		const GTEXT_YAML_Node *v =
		    gtext_yaml_mapping_get(gtext_yaml_document_root(doc), "x");
		ASSERT_NE(v, nullptr);
		EXPECT_EQ(gtext_yaml_node_type(v), GTEXT_YAML_SEQUENCE);
		EXPECT_STREQ(tag_or_dash(v), "!custom");
		gtext_yaml_free(doc);
		gtext_yaml_error_free(&err);
	}
	{
		GTEXT_YAML_Error err = {};
		GTEXT_YAML_Document *doc = parse_or_null("x: !custom\n  a: 1\n", &err);
		ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");
		const GTEXT_YAML_Node *v =
		    gtext_yaml_mapping_get(gtext_yaml_document_root(doc), "x");
		ASSERT_NE(v, nullptr);
		EXPECT_EQ(gtext_yaml_node_type(v), GTEXT_YAML_MAPPING);
		EXPECT_STREQ(tag_or_dash(v), "!custom");
		EXPECT_STREQ(tag_or_dash(tag_first_key(v)), "-");
		gtext_yaml_free(doc);
		gtext_yaml_error_free(&err);
	}
}

TEST(YamlTagPlacement, ScalarTagsAreUnaffected) {
	// Both spellings already worked and must keep working: a tag on the same
	// line as its scalar, and a tag on its own line before a plain scalar.
	{
		GTEXT_YAML_Error err = {};
		GTEXT_YAML_Document *doc = parse_or_null("x: !custom 5\n", &err);
		ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");
		const GTEXT_YAML_Node *v =
		    gtext_yaml_mapping_get(gtext_yaml_document_root(doc), "x");
		EXPECT_STREQ(tag_or_dash(v), "!custom");
		gtext_yaml_free(doc);
		gtext_yaml_error_free(&err);
	}
	{
		GTEXT_YAML_Error err = {};
		GTEXT_YAML_Document *doc = parse_or_null("!custom\nhello\n", &err);
		ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");
		const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
		EXPECT_STREQ(tag_or_dash(root), "!custom");
		gtext_yaml_free(doc);
		gtext_yaml_error_free(&err);
	}
}

TEST(YamlTagPlacement, BlockAndFlowAgreeOnTheStandardCollectionTags) {
	// The tags that gtext_yaml_to_json() refuses.  Before the fix the block
	// spellings resolved to a plain sequence or mapping, so the refusal did
	// not happen for them.
	struct Case {
		const char *flow;
		const char *block;
		GTEXT_YAML_Node_Type type;
		const char *tag;
	};
	const Case cases[] = {
	    {"!!omap [{a: 1}]", "!!omap\n- a: 1\n", GTEXT_YAML_OMAP, "!!omap"},
	    {"!!pairs [{a: 1}]", "!!pairs\n- a: 1\n", GTEXT_YAML_PAIRS, "!!pairs"},
	};

	for (const Case &c : cases) {
		SCOPED_TRACE(c.tag);
		for (const char *src : {c.flow, c.block}) {
			SCOPED_TRACE(src);
			GTEXT_YAML_Error err = {};
			GTEXT_YAML_Document *doc = parse_or_null(src, &err);
			ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");
			const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
			ASSERT_NE(root, nullptr);
			EXPECT_EQ(gtext_yaml_node_type(root), c.type);
			EXPECT_STREQ(tag_or_dash(root), c.tag);
			gtext_yaml_free(doc);
			gtext_yaml_error_free(&err);
		}
	}
}
