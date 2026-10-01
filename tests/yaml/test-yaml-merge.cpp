#include <gtest/gtest.h>
#include <string.h>

extern "C" {
#include <ghoti.io/text/yaml.h>

#include "yaml_render.h"
}

TEST(YamlMerge, SingleMapping) {
	const char *yaml =
		"defaults: &def {a: 1, b: 2}\n"
		"config: {<<: *def, b: 3, c: 4}\n";

	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");

	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	const GTEXT_YAML_Node *config = gtext_yaml_mapping_get(root, "config");
	ASSERT_NE(config, nullptr);
	ASSERT_EQ(gtext_yaml_node_type(config), GTEXT_YAML_MAPPING);
	EXPECT_EQ(gtext_yaml_mapping_size(config), 3u);

	const GTEXT_YAML_Node *a = gtext_yaml_mapping_get(config, "a");
	const GTEXT_YAML_Node *b = gtext_yaml_mapping_get(config, "b");
	const GTEXT_YAML_Node *c = gtext_yaml_mapping_get(config, "c");
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	ASSERT_NE(c, nullptr);
	EXPECT_STREQ(gtext_yaml_node_as_string(a), "1");
	EXPECT_STREQ(gtext_yaml_node_as_string(b), "3");
	EXPECT_STREQ(gtext_yaml_node_as_string(c), "4");

	gtext_yaml_free(doc);
}

TEST(YamlMerge, BlockMappingRoot) {
	const char *yaml =
		"defaults: &def {a: 1, b: 2}\n"
		"<<: *def\n"
		"b: 3\n"
		"c: 4\n";

	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");

	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	ASSERT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_MAPPING);
	EXPECT_EQ(gtext_yaml_mapping_size(root), 4u);

	const GTEXT_YAML_Node *defaults = gtext_yaml_mapping_get(root, "defaults");
	const GTEXT_YAML_Node *a = gtext_yaml_mapping_get(root, "a");
	const GTEXT_YAML_Node *b = gtext_yaml_mapping_get(root, "b");
	const GTEXT_YAML_Node *c = gtext_yaml_mapping_get(root, "c");
	ASSERT_NE(defaults, nullptr);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	ASSERT_NE(c, nullptr);
	EXPECT_STREQ(gtext_yaml_node_as_string(a), "1");
	EXPECT_STREQ(gtext_yaml_node_as_string(b), "3");
	EXPECT_STREQ(gtext_yaml_node_as_string(c), "4");

	gtext_yaml_free(doc);
}

TEST(YamlMerge, SequenceSources) {
	const char *yaml =
		"base1: &b1 {a: 1, b: 2}\n"
		"base2: &b2 {b: 3, c: 4}\n"
		"config: {<<: [*b1, *b2], d: 5}\n";

	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");

	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	const GTEXT_YAML_Node *config = gtext_yaml_mapping_get(root, "config");
	ASSERT_NE(config, nullptr);

	const GTEXT_YAML_Node *a = gtext_yaml_mapping_get(config, "a");
	const GTEXT_YAML_Node *b = gtext_yaml_mapping_get(config, "b");
	const GTEXT_YAML_Node *c = gtext_yaml_mapping_get(config, "c");
	const GTEXT_YAML_Node *d = gtext_yaml_mapping_get(config, "d");
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	ASSERT_NE(c, nullptr);
	ASSERT_NE(d, nullptr);
	EXPECT_STREQ(gtext_yaml_node_as_string(a), "1");
	EXPECT_STREQ(gtext_yaml_node_as_string(b), "3");
	EXPECT_STREQ(gtext_yaml_node_as_string(c), "4");
	EXPECT_STREQ(gtext_yaml_node_as_string(d), "5");

	gtext_yaml_free(doc);
}

TEST(YamlMerge, TaggedMergeKey) {
	const char *yaml =
		"base: &b {a: 1, b: 2}\n"
		"config: {!!merge <<: *b, b: 3}\n";

	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");

	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	const GTEXT_YAML_Node *config = gtext_yaml_mapping_get(root, "config");
	ASSERT_NE(config, nullptr);

	const GTEXT_YAML_Node *a = gtext_yaml_mapping_get(config, "a");
	const GTEXT_YAML_Node *b = gtext_yaml_mapping_get(config, "b");
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	EXPECT_STREQ(gtext_yaml_node_as_string(a), "1");
	EXPECT_STREQ(gtext_yaml_node_as_string(b), "3");

	gtext_yaml_free(doc);
}

TEST(YamlMerge, AliasSource) {
	const char *yaml =
		"base: &b {a: 1}\n"
		"config: {<<: *b, a: 2}\n";

	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");

	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	const GTEXT_YAML_Node *config = gtext_yaml_mapping_get(root, "config");
	ASSERT_NE(config, nullptr);

	const GTEXT_YAML_Node *a = gtext_yaml_mapping_get(config, "a");
	ASSERT_NE(a, nullptr);
	EXPECT_STREQ(gtext_yaml_node_as_string(a), "2");

	gtext_yaml_free(doc);
}

TEST(YamlMerge, DupkeyPolicyAllowsMergeOverride) {
	const char *yaml =
		"base1: &b1 {a: 1}\n"
		"base2: &b2 {a: 2}\n"
		"config: {<<: [*b1, *b2]}\n";

	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.dupkeys = GTEXT_YAML_DUPKEY_ERROR;

	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), &opts, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");

	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	ASSERT_NE(root, nullptr);
	const GTEXT_YAML_Node *config = gtext_yaml_mapping_get(root, "config");
	ASSERT_NE(config, nullptr);

	const GTEXT_YAML_Node *a = gtext_yaml_mapping_get(config, "a");
	ASSERT_NE(a, nullptr);
	EXPECT_STREQ(gtext_yaml_node_as_string(a), "2");

	gtext_yaml_free(doc);
}

TEST(YamlMerge, DupkeyPolicyStillErrorsOnExplicitDupes) {
	const char *yaml =
		"base: &b {a: 1}\n"
		"config: {<<: *b, a: 2, a: 3}\n";

	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.dupkeys = GTEXT_YAML_DUPKEY_ERROR;

	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), &opts, &err);
	EXPECT_EQ(doc, nullptr);
	EXPECT_EQ(err.code, GTEXT_YAML_E_DUPKEY);
}

TEST(YamlMerge, InvalidMergeValue) {
	const char *yaml = "a: {<<: [1, 2]}";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	EXPECT_EQ(doc, nullptr);
	EXPECT_EQ(err.code, GTEXT_YAML_E_INVALID);
}

/* A key is a merge key because its *contents* resolve to
   tag:yaml.org,2002:merge - and only a plain scalar is resolved by its
   contents (10.3.2). '"<<"' is the two-character string, which is what both
   PyYAML and js-yaml say, and taking it for a merge key was the usual two
   faults at once: '{"<<": 1}' was refused for a merge value that is not a
   mapping, and '{"<<": {a: 1}}' was *merged* - the key vanished and its
   contents were spliced into the mapping around it, with nothing reported.

   An explicit !!merge tag still says so whatever the style, because then it
   is the tag and not the contents doing the resolving. */
TEST(YamlMerge, AQuotedMergeKeyIsAnOrdinaryString) {
	struct Case { const char *yaml; const char *expected; };
	const Case cases[] = {
		{ "{\"<<\": 1}", "{\"<<\": 1}" },
		{ "{'<<': 1}", "{\"<<\": 1}" },
		{ "{\"<<\": {a: 1}}", "{\"<<\": {\"a\": 1}}" },
		{ "a: &x {p: 1}\nb: {\"<<\": *x}\n",
		  "{\"a\": {\"p\": 1}, \"b\": {\"<<\": {\"p\": 1}}}" },

		/* Plain, and it merges. */
		{ "a: &x {p: 1}\nb: {<<: *x}\n",
		  "{\"a\": {\"p\": 1}, \"b\": {\"p\": 1}}" },
		/* Tagged, and it merges whatever the style. */
		{ "a: &x {p: 1}\nb: {!!merge \"<<\": *x}\n",
		  "{\"a\": {\"p\": 1}, \"b\": {\"p\": 1}}" },
		/* A plain "<<" whose value is not a mapping is still an error. */
		{ "{<<: 1}", nullptr },
	};
	for (const Case &c : cases) {
		const std::string got = Render(c.yaml);
		if (c.expected) {
			EXPECT_EQ(got, std::string(c.expected))
				<< "input: " << ::testing::PrintToString(std::string(c.yaml));
		} else {
			EXPECT_EQ(got, std::string(""))
				<< "should have been refused, input: "
				<< ::testing::PrintToString(std::string(c.yaml));
		}
	}
}

/* A merge splices the source mapping's pairs in *by pointer*, so a merge whose
   value dereferences to one of the mapping's own ancestors makes the document
   contain itself.  Twenty-two bytes did that, and the process died:

       &O
       :: - <:
       <<
       - <::
       *O

   The death was in the walk that repoints merged aliases, which was a
   recursion; a one-gigabyte stack overflowed in the same place, which is how
   "deep" was ruled out and "does not terminate" ruled in.  But a cycle-safe
   walk there only moves the problem: the writer and every public accessor are
   ordinary recursions over the DOM, and a cyclic DOM handed back to a caller is
   a crash in the caller's code.  There is also no YAML to write it as - the
   writer emits a shared node inline rather than as an alias, so a cycle has no
   output at all.  So the merge is refused, which is the only answer that leaves
   a document anyone can use.

   PyYAML accepts these and builds a cyclic Python object (its repr prints
   "{...}"), which a garbage-collected language can hold and C cannot.  That is
   a deliberate divergence and is recorded in notes/text/SOAK-FINDINGS.md
   finding 8, not an oversight.

   What is *not* refused is the lower bound of this check, and it is the half
   worth testing: an alias to an ancestor is ordinary YAML and is not
   containment, and a mapping may merge from itself as long as nothing comes
   back round - "&r {a: 1, <<: *r}" splices only "a: 1", because
   merge_from_mapping skips merge keys. */
TEST(YamlMerge, AMergeMayNotMakeAMappingContainItself) {
	struct Case { const char *yaml; const char *expected; };
	const Case cases[] = {
		/* The 22-byte fuzz witness, and the shape it reduces to. */
		{ "&O\n:: - <:\n<<\n- <::\n*O", nullptr },
		{ "&r\nk: {<<: *r}\n", nullptr },
		{ "&r\nk: {<<: *r, z: 9}\n", nullptr },
		/* Depth does not help it: the ancestor is still an ancestor. */
		{ "&r\na: 1\nb:\n  c:\n    d: {<<: *r}\n", nullptr },
		/* The merge source is the mapping itself, and no pair comes back. */
		{ "&r {a: 1, <<: *r}", "{\"a\": 1}" },
		{ "&r\na: 1\nsub: &s\n  <<: *s\n", "{\"a\": 1, \"sub\": {}}" },
		/* An alias to an ancestor is not containment.  Render() prints a node
		   already on its path as "*", which is how these two terminate. */
		{ "&r\na: *r\n", "{\"a\": *}" },
		{ "&r\na: [*r]\n", "{\"a\": [*]}" },
		/* An ordinary merge from a sibling, and from a nested anchor, which
		   share a subtree with the source and contain nothing of the target. */
		{ "base: &b {a: 1}\nderived: {<<: *b, c: 2}\n",
		  "{\"base\": {\"a\": 1}, \"derived\": {\"a\": 1, \"c\": 2}}" },
		{ "&r\na: &av {x: 1}\nb: {<<: *av}\n",
		  "{\"a\": {\"x\": 1}, \"b\": {\"x\": 1}}" },
	};
	for (const Case &c : cases) {
		const std::string got = Render(c.yaml);
		if (c.expected) {
			EXPECT_EQ(got, std::string(c.expected))
				<< "input: " << ::testing::PrintToString(std::string(c.yaml));
		} else {
			EXPECT_EQ(got, std::string(""))
				<< "should have been refused, input: "
				<< ::testing::PrintToString(std::string(c.yaml));
		}
	}
}

/* The refusal is reported, not just performed: a caller who sees a null
   document needs to be able to tell this apart from a syntax error. */
TEST(YamlMerge, ASelfContainingMergeIsReportedAsInvalid) {
	const char *yaml = "&O\n:: - <:\n<<\n- <::\n*O";
	GTEXT_YAML_Error err = {};
	GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, &err);
	ASSERT_EQ(doc, nullptr);
	EXPECT_EQ(err.code, GTEXT_YAML_E_INVALID);
	ASSERT_NE(err.message, nullptr);
	EXPECT_NE(strstr(err.message, "contain itself"), nullptr) << err.message;
}

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
