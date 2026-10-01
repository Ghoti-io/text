/**
 * max_depth, and the half of the document it was not reaching.
 *
 * The limit was counted in the stream layer, at the "[" and "{" of a flow
 * collection. Block structure never touched it, because block structure is
 * composed by the DOM parser rather than reported by the scanner - so
 *
 *     - - - - - ... x        (five thousand of them)
 *
 * parsed to a DOM five thousand deep with a limit of 256 in force. That is
 * exactly the input a nesting limit exists for, and it was the one input the
 * limit did not see.
 *
 * It surfaced through the writer: the flow-style writer turns block nesting
 * into flow nesting, which *is* counted, so the writer produced a document
 * this parser then refused. A limit that only some spellings of the same
 * document reach is not a limit.
 *
 * The old test here asked "if it failed, did it fail with E_DEPTH?", which
 * passes when nothing fails at all.
 */
#include <gtest/gtest.h>
#include <stdint.h>
#include <string>

extern "C" {
#include <ghoti.io/text/yaml.h>
}

namespace {

/* n nested block sequences: "- - - ... x". */
std::string BlockSequence(size_t n) {
	std::string s;
	for (size_t i = 0; i < n; ++i) s += "- ";
	s += "x";
	return s;
}

/* n nested block mappings, one indent step each. */
std::string BlockMapping(size_t n) {
	std::string s;
	for (size_t i = 0; i < n; ++i) s += std::string(i * 2, ' ') + "k:\n";
	return s;
}

/* n nested flow sequences. */
std::string FlowSequence(size_t n) {
	return std::string(n, '[') + "1" + std::string(n, ']');
}

GTEXT_YAML_Status ParseStatus(const std::string &in, size_t max_depth) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = max_depth;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document *doc =
		gtext_yaml_parse(in.data(), in.size(), &opts, &err);
	if (doc) {
		gtext_yaml_free(doc);
		gtext_yaml_error_free(&err);
		return GTEXT_YAML_OK;
	}
	const GTEXT_YAML_Status st = err.code;
	gtext_yaml_error_free(&err);
	return st;
}

}  // namespace

TEST(YamlDepth, TheLimitReachesBlockCollections) {
	EXPECT_EQ(ParseStatus(BlockSequence(10), 64), GTEXT_YAML_OK);
	EXPECT_EQ(ParseStatus(BlockSequence(500), 64), GTEXT_YAML_E_DEPTH);
	EXPECT_EQ(ParseStatus(BlockSequence(5000), 256), GTEXT_YAML_E_DEPTH);

	EXPECT_EQ(ParseStatus(BlockMapping(10), 64), GTEXT_YAML_OK);
	EXPECT_EQ(ParseStatus(BlockMapping(500), 64), GTEXT_YAML_E_DEPTH);
}

TEST(YamlDepth, TheLimitStillReachesFlowCollections) {
	EXPECT_EQ(ParseStatus(FlowSequence(10), 64), GTEXT_YAML_OK);
	EXPECT_EQ(ParseStatus(FlowSequence(500), 64), GTEXT_YAML_E_DEPTH);
}

/* A depth refusal is not an allocation failure, and every caller of
   stack_push() used to report one. */
TEST(YamlDepth, ItIsReportedAsDepthAndNotAsMemory) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = 4;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	const std::string in = BlockSequence(64);
	GTEXT_YAML_Document *doc =
		gtext_yaml_parse(in.data(), in.size(), &opts, &err);
	ASSERT_EQ(doc, nullptr);
	EXPECT_EQ(err.code, GTEXT_YAML_E_DEPTH);
	EXPECT_STREQ(err.message, "Maximum nesting depth exceeded");
	gtext_yaml_error_free(&err);
}

/* Zero means the library default, not "no limit".
 *
 * This test used to assert the opposite, on the stated grounds that it "is
 * what the field's documentation says". It is not. max_depth carries no
 * per-field documentation at all; the only sentence in any public header on
 * the subject is the one over GTEXT_YAML_Parse_Options itself - "All size
 * limits use 0 to denote 'use the library default'" - and json_core.h spells
 * the same rule out per field as "0 = default, e.g. 256". The behaviour the
 * test pinned was the implementation's, and the documentation it cited said
 * the reverse.
 *
 * The cost of the implementation's reading was not academic.
 * "GTEXT_YAML_Parse_Options opts = {0};" - the spelling the security page
 * recommended - removed max_depth, max_total_bytes and max_alias_expansion at
 * once, and a hundred thousand nested flow sequences then ran resolve_node()
 * off the end of the stack. Under the documented reading the same struct is
 * refused at 256.
 *
 * A caller who wants no limit still has one: SIZE_MAX. There is no spelling
 * of "the library default" that a caller can reach if zero does not mean it,
 * which is the asymmetry that settles this. */
TEST(YamlDepth, ZeroMeansTheDefaultAndNotNoLimit) {
	const GTEXT_YAML_Parse_Options defaults = gtext_yaml_parse_options_default();
	ASSERT_LT(defaults.max_depth, 2000u);

	EXPECT_EQ(ParseStatus(BlockSequence(2000), 0), GTEXT_YAML_E_DEPTH);
	EXPECT_EQ(ParseStatus(BlockSequence(2000), defaults.max_depth),
		GTEXT_YAML_E_DEPTH);

	/* ...and it is the depth that refuses it, not the options struct. */
	EXPECT_EQ(ParseStatus(BlockSequence(8), 0), GTEXT_YAML_OK);

	/* Asking for no limit is still possible, just not by saying nothing. */
	EXPECT_EQ(ParseStatus(BlockSequence(2000), SIZE_MAX), GTEXT_YAML_OK);
}

/* And the scanner had a second limit nobody set: a fixed 32-entry array for
   the flow-context stack, whose push was *dropped* when it ran out while the
   matching pop still counted down. Past 32 nested flow collections the
   scanner believed it was back in block context with the brackets still
   open, and mis-scanned what followed.

   It does not fail on every shape - plain "[[[...a: 1...]]]" forty deep comes
   back right, because one dropped push and one clamped pop cancel - which is
   why it took a fuzzer and a particular shape to surface. The array grows
   now; max_depth is the limit, and it is the only one. */
TEST(YamlDepth, TheScannerHasNoLimitOfItsOwn) {
	/* The shape the writer fuzzer produced, reduced: two runs of nested flow
	   sequences either side of a mapping, inside a mapping with an empty key.
	   At 13 it parsed and at 14 it did not, because 33 brackets are open at
	   the deepest point. */
	for (size_t n : {13, 14, 20, 40}) {
		std::string in = "[{: [{" + std::string(n, '[') + "{\":\": "
			+ std::string(n, '[') + "~" + std::string(n, ']') + "}"
			+ std::string(n, ']') + ": }]}, ~]";
		EXPECT_EQ(ParseStatus(in, 256), GTEXT_YAML_OK) << "n = " << n;
	}

	/* And plain nesting past 32 still reads back as what it is. */
	for (size_t n : {32, 33, 64}) {
		const std::string in =
			std::string(n, '[') + "a: 1" + std::string(n, ']');
		GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
		opts.max_depth = 256;
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Document *doc =
			gtext_yaml_parse(in.data(), in.size(), &opts, &err);
		ASSERT_NE(doc, nullptr) << "n = " << n << ": "
			<< (err.message ? err.message : "");
		gtext_yaml_error_free(&err);
		const GTEXT_YAML_Node *node = gtext_yaml_document_root(doc);
		size_t seen = 0;
		while (node && gtext_yaml_node_type(node) == GTEXT_YAML_SEQUENCE) {
			ASSERT_EQ(gtext_yaml_sequence_length(node), (size_t)1) << "n = " << n;
			node = gtext_yaml_sequence_get(node, 0);
			seen++;
		}
		EXPECT_EQ(seen, n) << "n = " << n;
		ASSERT_NE(node, nullptr);
		EXPECT_EQ(gtext_yaml_node_type(node), GTEXT_YAML_MAPPING) << "n = " << n;
		gtext_yaml_free(doc);
	}
}

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}

/* max_depth = SIZE_MAX is the documented way to say "no depth limit", and it
   used to be a way to end the process instead.
   
   Parsing is iterative and survives any depth, but the resolver that runs
   over the finished DOM was a recursion at about 344 bytes of C stack a
   level, so a document past roughly 24000 levels took the parse down after
   the hard part was over. The resolver keeps its stack on the heap now.
   
   40000 is chosen to be past that 24000 with room to spare - 40000 levels
   at 344 bytes is 13.8 MB against the usual 8 MB stack - and no further,
   because parsing this shape is super-linear in depth for reasons that have
   nothing to do with the resolver: at 100000 the parse takes 19.5s, 99.6% of
   it inside property_left_of_open_collection() and block_node_begins_line(),
   with the resolver not appearing in the profile at all. That is worth
   fixing and is not this test's business to pay for.
   
   Like the clone's equivalent in test-yaml-built-depth.cpp, this crashes
   rather than fails if the walk goes back to recursing, which is the honest
   way to report the thing it exists to prevent. */
TEST(YamlDepth, NoLimitDoesNotMeanNoStack) {
	const size_t n = 40000;
	const std::string in =
		std::string(n, '[') + "x" + std::string(n, ']');

	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = SIZE_MAX;
	opts.max_total_bytes = 0; /* the default, which is far more than this */

	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document *doc =
		gtext_yaml_parse(in.data(), in.size(), &opts, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");
	gtext_yaml_error_free(&err);

	/* The control: a document that is not actually this deep would have been
	   resolved by any implementation. */
	const GTEXT_YAML_Node *node = gtext_yaml_document_root(doc);
	size_t seen = 0;
	while (node && gtext_yaml_node_type(node) == GTEXT_YAML_SEQUENCE
			&& gtext_yaml_sequence_length(node) > 0) {
		node = gtext_yaml_sequence_get(node, 0);
		seen++;
	}
	EXPECT_EQ(seen, n);

	gtext_yaml_free(doc);
}

/* And the limit still binds where it is set, so moving the stack to the heap
   did not turn max_depth off for parsed documents either. */
TEST(YamlDepth, TheParsedLimitStillBinds) {
	EXPECT_EQ(ParseStatus(std::string(50, '[') + "x" + std::string(50, ']'), 256),
		GTEXT_YAML_OK);
	EXPECT_EQ(ParseStatus(std::string(500, '[') + "x" + std::string(500, ']'), 256),
		GTEXT_YAML_E_DEPTH);
}

/* The *other* walk in the resolver, which stayed a recursion when resolve_node
   stopped being one.
   
   update_alias_targets() repoints any alias whose target a merge replaced, and
   it runs over the whole document from the root - thirty lines below the
   comment in resolve_node() explaining why depth must not cost a frame.  Its
   frames are small, five callee-saved registers and a return address, so 48
   bytes a level: this document survives 173000 levels and takes the process
   down at 175000, which is 8 MiB to within a rounding error.
   
   Two things have to be true for the walk to run at all, and both are asserted
   below rather than assumed, because either one failing would make this test
   pass without ever reaching the code it is about:
   
   - a merge has to have *replaced* a mapping, not just filled it in, since
     only a replacement goes in the list the walk exists to apply.  "{<<: *r}"
     is one pair and gains two, so it cannot be merged in place.
   - the document has to really be that deep.
   
   The shape is "[deep, x]" rather than "[deep]" because with one child the
   recursion into it is in tail position and gcc -O2 eliminates it - 160000
   levels of "[[[...]]]" parse fine on the recursive walk, which is a reminder
   that a witness for a stack overflow has to defeat the optimiser before it
   can defeat the stack. */
TEST(YamlDepth, TheMergeAliasWalkDoesNotUseTheCStack) {
	const size_t n = 200000;
	std::string in = "a: &r {q: 1, w: 2}\nb: ";
	in += std::string(n, '[');
	in += "{<<: *r}";
	for (size_t i = 0; i < n; ++i) in += ",x]";

	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.max_depth = SIZE_MAX;

	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document *doc =
		gtext_yaml_parse(in.data(), in.size(), &opts, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");
	gtext_yaml_error_free(&err);

	/* Control one: the nesting is really there. */
	const GTEXT_YAML_Node *node =
		gtext_yaml_mapping_get(gtext_yaml_document_root(doc), "b");
	size_t seen = 0;
	while (node && gtext_yaml_node_type(node) == GTEXT_YAML_SEQUENCE
			&& gtext_yaml_sequence_length(node) > 0) {
		node = gtext_yaml_sequence_get(node, 0);
		seen++;
	}
	EXPECT_EQ(seen, n);

	/* Control two: the merge grew the mapping, so a replacement was recorded
	   and the walk had something to do. */
	ASSERT_NE(node, nullptr);
	ASSERT_EQ(gtext_yaml_node_type(node), GTEXT_YAML_MAPPING);
	EXPECT_EQ(gtext_yaml_mapping_size(node), 2u);
	EXPECT_NE(gtext_yaml_mapping_get(node, "q"), nullptr);
	EXPECT_NE(gtext_yaml_mapping_get(node, "w"), nullptr);

	gtext_yaml_free(doc);
}
