/**
 * max_depth, for a document nobody parsed.
 *
 * max_depth was a parser limit. The parser is iterative - it composes block
 * structure on an explicit stack - so parsing never ran out of C stack, and
 * the limit bounded every document that arrived as text.
 *
 * It said nothing about a document built through the DOM API. The
 * constructors do not consult it and cannot cheaply: there are no parent
 * pointers, so asking a node how deep it sits means a walk, and a walk per
 * append is quadratic. So the depth of a built document was bounded by
 * nothing, and the walks over it - the DOM writer, and the clone - recurse:
 *
 *     ~228 bytes a level in write_node()
 *     ~113 bytes a level in clone_node()
 *
 * On an 8 MiB stack that is about 37,000 and 74,000 levels. Past those the
 * process died with SIGSEGV, on gtext_yaml_parse_options_default(), with
 * nothing anywhere having asked for anything unusual. gtext_yaml_to_json()
 * survived only by accident: its node budget bounds depth as a side effect,
 * since depth can never exceed node count.
 *
 * The limit is spent on the way back out, where the depth is already known.
 *
 * These tests use depths in the hundreds. They are not trying to reach the
 * stack - a test that did would be slow, would depend on the machine's
 * ulimit, and would fail by crashing rather than by reporting. They check
 * that the limit is consulted at all, which is the thing that was missing.
 */
#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <string>

extern "C" {
#include <ghoti.io/text/json.h>
#include <ghoti.io/text/yaml.h>
}

namespace {

/* n nested sequences. gtext_yaml_sequence_append() returns the NEW parent
   rather than the child, so gaining a level means wrapping from the inside
   out - appending in a loop builds a flat sequence instead, which is a
   mistake worth naming here because it makes a depth test silently measure
   nothing. */
GTEXT_YAML_Document *BuildNested(size_t n, const GTEXT_YAML_Parse_Options *opts) {
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document *doc = gtext_yaml_document_new(opts, &err);
	if (!doc) return nullptr;

	GTEXT_YAML_Node *inner = gtext_yaml_node_new_scalar(doc, "x", nullptr, nullptr);
	for (size_t i = 0; i < n; i++) {
		GTEXT_YAML_Node *outer = gtext_yaml_node_new_sequence(doc, nullptr, nullptr);
		if (!outer) return nullptr;
		GTEXT_YAML_Node *joined = gtext_yaml_sequence_append(doc, outer, inner);
		if (!joined) return nullptr;
		inner = joined;
	}
	gtext_yaml_document_set_root(doc, inner);
	return doc;
}

/* What the document actually is, rather than what it was asked to be. */
size_t MeasureDepth(const GTEXT_YAML_Node *n) {
	size_t d = 0;
	while (n && gtext_yaml_node_type(n) == GTEXT_YAML_SEQUENCE
			&& gtext_yaml_sequence_length(n) > 0) {
		n = gtext_yaml_sequence_get(n, 0);
		d++;
	}
	return d;
}

GTEXT_YAML_Status ToJsonIt(const GTEXT_YAML_Document *doc) {
	GTEXT_JSON_Value *json = nullptr;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Status st = gtext_yaml_to_json(doc, &json, &err);
	if (json) gtext_json_free(json);
	return st;
}

GTEXT_YAML_Status WriteIt(const GTEXT_YAML_Document *doc) {
	GTEXT_YAML_Sink sink;
	EXPECT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
	GTEXT_YAML_Write_Options opts = gtext_yaml_write_options_default();
	GTEXT_YAML_Status st = gtext_yaml_write_document(doc, &sink, &opts);
	gtext_yaml_sink_buffer_free(&sink);
	return st;
}

} // namespace

/* The builder really does nest, so everything below is measuring depth. */
TEST(YamlBuiltDepth, TheFixtureIsAsDeepAsItClaims) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = 0;  /* the default, 256 */
	GTEXT_YAML_Document *doc = BuildNested(40, &opts);
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(MeasureDepth(gtext_yaml_document_root(doc)), 40u);
	gtext_yaml_free(doc);
}

TEST(YamlBuiltDepth, TheWriterHoldsABuiltDocumentToMaxDepth) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = 100;

	GTEXT_YAML_Document *shallow = BuildNested(50, &opts);
	ASSERT_NE(shallow, nullptr);
	ASSERT_EQ(MeasureDepth(gtext_yaml_document_root(shallow)), 50u);
	EXPECT_EQ(WriteIt(shallow), GTEXT_YAML_OK);
	gtext_yaml_free(shallow);

	GTEXT_YAML_Document *deep = BuildNested(500, &opts);
	ASSERT_NE(deep, nullptr);
	ASSERT_EQ(MeasureDepth(gtext_yaml_document_root(deep)), 500u);
	EXPECT_EQ(WriteIt(deep), GTEXT_YAML_E_DEPTH);
	gtext_yaml_free(deep);
}

TEST(YamlBuiltDepth, TheCloneHoldsABuiltDocumentToMaxDepth) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = 100;

	GTEXT_YAML_Document *shallow = BuildNested(50, &opts);
	ASSERT_NE(shallow, nullptr);
	EXPECT_NE(gtext_yaml_node_clone(shallow, gtext_yaml_document_root(shallow)),
		nullptr);
	gtext_yaml_free(shallow);

	GTEXT_YAML_Document *deep = BuildNested(500, &opts);
	ASSERT_NE(deep, nullptr);
	EXPECT_EQ(gtext_yaml_node_clone(deep, gtext_yaml_document_root(deep)),
		nullptr);
	gtext_yaml_free(deep);
}

/* The default is a real limit here too, not just on the parse path: a caller
   who sets nothing at all still gets 256. */
TEST(YamlBuiltDepth, TheDefaultBindsADocumentNobodyParsed) {
	const GTEXT_YAML_Parse_Options defaults = gtext_yaml_parse_options_default();
	ASSERT_LT(defaults.max_depth, 1000u);

	GTEXT_YAML_Document *doc = BuildNested(1000, nullptr);
	ASSERT_NE(doc, nullptr);
	ASSERT_EQ(MeasureDepth(gtext_yaml_document_root(doc)), 1000u);

	EXPECT_EQ(WriteIt(doc), GTEXT_YAML_E_DEPTH);
	EXPECT_EQ(gtext_yaml_node_clone(doc, gtext_yaml_document_root(doc)), nullptr);
	gtext_yaml_free(doc);
}

/* SIZE_MAX still means no limit. It is the one spelling that hands the caller
   the C stack, and it has to keep working or there is no way to ask for a
   genuinely deep document at all. 2000 is far below any plausible stack and
   far above the default, so this separates "unlimited" from "default". */
TEST(YamlBuiltDepth, SizeMaxStillMeansNoLimit) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = SIZE_MAX;

	GTEXT_YAML_Document *doc = BuildNested(2000, &opts);
	ASSERT_NE(doc, nullptr);
	ASSERT_EQ(MeasureDepth(gtext_yaml_document_root(doc)), 2000u);

	EXPECT_EQ(WriteIt(doc), GTEXT_YAML_OK);
	EXPECT_NE(gtext_yaml_node_clone(doc, gtext_yaml_document_root(doc)), nullptr);
	gtext_yaml_free(doc);
}

/* What "no limit" costs, now that it does not cost the C stack.
   
   SIZE_MAX is the documented way to say "no bound, I own the stack". On a
   recursive walk that made it a way to ask for a segmentation fault: the
   clone recursed at about 113 bytes a level, so a little over 74000 levels
   took the process down on the usual 8 MiB. The walk keeps its stack on the
   heap now, so the depth a caller may ask for is bounded by memory rather
   than by a frame size.
   
   200000 is chosen to be comfortably past that 74000, and this test crashes
   rather than fails if the walk goes back to recursing - which is the honest
   way for it to report, since that is exactly the failure being prevented. */
TEST(YamlBuiltDepth, AnUnboundedCloneDoesNotUseTheCStack) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = SIZE_MAX;

	const size_t depth = 200000;
	GTEXT_YAML_Document *doc = BuildNested(depth, &opts);
	ASSERT_NE(doc, nullptr);
	/* The control. A document that is not actually this deep would pass the
	   clone below for the wrong reason. */
	ASSERT_EQ(MeasureDepth(gtext_yaml_document_root(doc)), depth);

	GTEXT_YAML_Node *copy =
		gtext_yaml_node_clone(doc, gtext_yaml_document_root(doc));
	ASSERT_NE(copy, nullptr);
	EXPECT_EQ(MeasureDepth(copy), depth)
		<< "the clone came back shallower than what it copied";

	gtext_yaml_free(doc);
}

/* The writer's half of the same guarantee.
   
   write_node() recursed at about 228 bytes a level, so a built document past
   roughly 37000 levels took the process down on the usual 8 MiB - and the
   writer is the half a caller reaches without parsing anything.
   
   Written in flow style on purpose: block style indents two spaces a level,
   so 100000 levels of it is some ten gigabytes of output, and this test would
   be measuring the sink rather than the walk. */
TEST(YamlBuiltDepth, AnUnboundedWriteDoesNotUseTheCStack) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = SIZE_MAX;

	const size_t depth = 100000;
	GTEXT_YAML_Document *doc = BuildNested(depth, &opts);
	ASSERT_NE(doc, nullptr);
	ASSERT_EQ(MeasureDepth(gtext_yaml_document_root(doc)), depth);

	GTEXT_YAML_Sink sink;
	ASSERT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
	GTEXT_YAML_Write_Options wopts = gtext_yaml_write_options_default();
	wopts.flow_style = GTEXT_YAML_FLOW_STYLE_FLOW;
	EXPECT_EQ(gtext_yaml_write_document(doc, &sink, &wopts), GTEXT_YAML_OK);

	/* The control: the output really is as deep as the document, so a walk
	   that stopped early would not pass this by writing something short. */
	const std::string out(
		gtext_yaml_sink_buffer_data(&sink), gtext_yaml_sink_buffer_size(&sink));
	EXPECT_EQ(std::count(out.begin(), out.end(), '['), (long)depth);
	gtext_yaml_sink_buffer_free(&sink);

	gtext_yaml_free(doc);
}

/* And the limit still binds when there is one, at the same place, so the
   heap stack did not quietly turn max_depth off. */
TEST(YamlBuiltDepth, TheCloneStillRefusesPastTheLimitItIsGiven) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = 100;

	GTEXT_YAML_Document *shallow = BuildNested(50, &opts);
	ASSERT_NE(shallow, nullptr);
	EXPECT_NE(gtext_yaml_node_clone(shallow, gtext_yaml_document_root(shallow)),
		nullptr);
	gtext_yaml_free(shallow);

	GTEXT_YAML_Document *deep = BuildNested(500, &opts);
	ASSERT_NE(deep, nullptr);
	EXPECT_EQ(gtext_yaml_node_clone(deep, gtext_yaml_document_root(deep)),
		nullptr);
	gtext_yaml_free(deep);
}

/* A parsed document is bounded as it always was, and by the same number, so
   widening the limit's reach did not narrow it. */
TEST(YamlBuiltDepth, AParsedDocumentIsUnchanged) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = 100;

	const std::string ok(50, '[') ;
	const std::string ok_doc = ok + "x" + std::string(50, ']') + "\n";
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document *doc =
		gtext_yaml_parse(ok_doc.data(), ok_doc.size(), &opts, &err);
	EXPECT_NE(doc, nullptr) << (err.message ? err.message : "?");
	if (doc) gtext_yaml_free(doc);

	const std::string deep_doc =
		std::string(500, '[') + "x" + std::string(500, ']') + "\n";
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document *refused =
		gtext_yaml_parse(deep_doc.data(), deep_doc.size(), &opts, &err);
	EXPECT_EQ(refused, nullptr);
	if (refused) gtext_yaml_free(refused);
}

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}

/* And the conversion to JSON, which is the third recursive descent over a
   built document and the one whose guard was an accident.
   
   convert_node() looked guarded, and was, but not against depth: what stopped
   it was max_nodes, taken from max_alias_expansion, which exists for the alias
   *bomb* - a DAG that resolves to an exponentially larger tree, where "every
   path is distinct" defeats the cycle check.  Depth was bounded only as a side
   effect of bounding node count, so lifting that budget - the one documented
   knob for "this document is big and I know it" - took the depth protection
   with it and the process died at some tens of thousands of levels.
   
   A guard that holds for a reason unrelated to what it is holding is a guard
   that someone fixing something else will remove. */
TEST(YamlBuiltDepth, TheConversionHoldsABuiltDocumentToMaxDepth) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = 100;

	GTEXT_YAML_Document *shallow = BuildNested(50, &opts);
	ASSERT_NE(shallow, nullptr);
	ASSERT_EQ(MeasureDepth(gtext_yaml_document_root(shallow)), 50u);
	EXPECT_EQ(ToJsonIt(shallow), GTEXT_YAML_OK);
	gtext_yaml_free(shallow);

	GTEXT_YAML_Document *deep = BuildNested(500, &opts);
	ASSERT_NE(deep, nullptr);
	ASSERT_EQ(MeasureDepth(gtext_yaml_document_root(deep)), 500u);
	EXPECT_EQ(ToJsonIt(deep), GTEXT_YAML_E_DEPTH);
	gtext_yaml_free(deep);
}

/* The one that matters, and the one that was crashing: the depth limit holds
   with the node budget lifted.
   
   max_alias_expansion = SIZE_MAX is what a caller sets for a big document.
   Before, that was also what turned off the only thing bounding the recursion;
   now it bounds nothing but node count, and E_DEPTH comes from max_depth.
   This crashes rather than fails if the two are confused again. */
TEST(YamlBuiltDepth, LiftingTheAliasBudgetDoesNotLiftTheDepthLimit) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = 256;
	opts.max_alias_expansion = SIZE_MAX;

	const size_t depth = 200000;
	GTEXT_YAML_Document *doc = BuildNested(depth, &opts);
	ASSERT_NE(doc, nullptr);
	/* The control: a document that is not actually this deep would be refused
	   by the depth limit for the wrong reason, or not refused at all. */
	ASSERT_EQ(MeasureDepth(gtext_yaml_document_root(doc)), depth);

	EXPECT_EQ(ToJsonIt(doc), GTEXT_YAML_E_DEPTH);
	gtext_yaml_free(doc);
}
