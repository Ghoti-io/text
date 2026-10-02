/**
 * @file test-yaml-arena.cpp
 * @brief Tests for YAML arena allocator
 */

#include <gtest/gtest.h>
#include <set>

extern "C" {
#include "../src/yaml/yaml_internal.h"
/* For the DOM accessors the node_count tests walk the tree with: the internal
   header alone does not declare them. */
#include <ghoti.io/text/yaml.h>
#include <stdlib.h>
#include <string.h>
}

//
// Test: Create and destroy arena
//
TEST(YamlArena, CreateDestroy) {
	yaml_arena *arena = yaml_arena_new(nullptr);
	ASSERT_NE(arena, nullptr);
	
	// Should have initial block
	EXPECT_NE(arena->first, nullptr);
	EXPECT_EQ(arena->first, arena->current);
	EXPECT_EQ(arena->block_size, 4096);  // 4KB initial
	
	yaml_arena_free(arena);
	// Valgrind will catch leaks
}

//
// Test: Simple allocation
//
TEST(YamlArena, SimpleAlloc) {
	yaml_arena *arena = yaml_arena_new(nullptr);
	ASSERT_NE(arena, nullptr);
	
	// Allocate small block
	void *p1 = yaml_arena_alloc(arena, 64, 8);
	ASSERT_NE(p1, nullptr);
	
	// Write to it (shouldn't crash)
	memset(p1, 0xAB, 64);
	
	// Allocate another
	void *p2 = yaml_arena_alloc(arena, 128, 8);
	ASSERT_NE(p2, nullptr);
	EXPECT_NE(p1, p2);  // Different pointers
	
	// Write to second
	memset(p2, 0xCD, 128);
	
	// First allocation should be unchanged
	EXPECT_EQ(((unsigned char*)p1)[0], 0xAB);
	
	yaml_arena_free(arena);
}

//
// Test: Alignment
//
TEST(YamlArena, Alignment) {
	yaml_arena *arena = yaml_arena_new(nullptr);
	ASSERT_NE(arena, nullptr);
	
	// Test various alignments
	void *p1 = yaml_arena_alloc(arena, 1, 1);
	EXPECT_EQ((uintptr_t)p1 % 1, 0);
	
	void *p2 = yaml_arena_alloc(arena, 1, 2);
	EXPECT_EQ((uintptr_t)p2 % 2, 0);
	
	void *p4 = yaml_arena_alloc(arena, 1, 4);
	EXPECT_EQ((uintptr_t)p4 % 4, 0);
	
	void *p8 = yaml_arena_alloc(arena, 1, 8);
	EXPECT_EQ((uintptr_t)p8 % 8, 0);
	
	void *p16 = yaml_arena_alloc(arena, 1, 16);
	EXPECT_EQ((uintptr_t)p16 % 16, 0);
	
	yaml_arena_free(arena);
}

//
// Test: Multiple blocks
//
TEST(YamlArena, MultipleBlocks) {
	yaml_arena *arena = yaml_arena_new(nullptr);
	ASSERT_NE(arena, nullptr);
	
	yaml_arena_block *first_block = arena->first;
	
	// Allocate enough to trigger new block
	// Initial block is 4KB, allocate 5KB to force new block
	void *p1 = yaml_arena_alloc(arena, 5 * 1024, 8);
	ASSERT_NE(p1, nullptr);
	
	// Should have created new block
	EXPECT_NE(arena->current, first_block);
	EXPECT_EQ(first_block->next, arena->current);
	
	// New block should be larger (8KB next)
	EXPECT_EQ(arena->block_size, 8192);
	
	yaml_arena_free(arena);
}

//
// Test: Exponential growth
//
TEST(YamlArena, ExponentialGrowth) {
	yaml_arena *arena = yaml_arena_new(nullptr);
	ASSERT_NE(arena, nullptr);
	
	// Start at 4KB
	EXPECT_EQ(arena->block_size, 4096);
	
	// Trigger growth to 8KB
	yaml_arena_alloc(arena, 5000, 8);
	EXPECT_EQ(arena->block_size, 8192);
	
	// Trigger growth to 16KB
	yaml_arena_alloc(arena, 9000, 8);
	EXPECT_EQ(arena->block_size, 16384);
	
	// Trigger growth to 32KB
	yaml_arena_alloc(arena, 17000, 8);
	EXPECT_EQ(arena->block_size, 32768);
	
	// Trigger growth to 64KB
	yaml_arena_alloc(arena, 33000, 8);
	EXPECT_EQ(arena->block_size, 65536);
	
	// Should cap at 64KB
	yaml_arena_alloc(arena, 65000, 8);
	EXPECT_EQ(arena->block_size, 65536);
	
	yaml_arena_free(arena);
}

//
// Test: Large allocation
//
TEST(YamlArena, LargeAlloc) {
	yaml_arena *arena = yaml_arena_new(nullptr);
	ASSERT_NE(arena, nullptr);
	
	// Allocate larger than max block size
	size_t large_size = 128 * 1024;  // 128KB
	void *p = yaml_arena_alloc(arena, large_size, 8);
	ASSERT_NE(p, nullptr);
	
	// Should work fine
	memset(p, 0xFF, large_size);
	EXPECT_EQ(((unsigned char*)p)[0], 0xFF);
	EXPECT_EQ(((unsigned char*)p)[large_size - 1], 0xFF);
	
	yaml_arena_free(arena);
}

//
// Test: Many small allocations
//
TEST(YamlArena, ManySmallAllocs) {
	yaml_arena *arena = yaml_arena_new(nullptr);
	ASSERT_NE(arena, nullptr);
	
	// Allocate 1000 small blocks
	void *ptrs[1000];
	for (int i = 0; i < 1000; i++) {
		ptrs[i] = yaml_arena_alloc(arena, 32, 8);
		ASSERT_NE(ptrs[i], nullptr);
		// Mark each allocation
		*(int*)ptrs[i] = i;
	}
	
	// Verify all allocations are distinct and intact
	for (int i = 0; i < 1000; i++) {
		EXPECT_EQ(*(int*)ptrs[i], i);
	}
	
	yaml_arena_free(arena);
}

//
// Test: Zero-size allocation
//
TEST(YamlArena, ZeroSize) {
	yaml_arena *arena = yaml_arena_new(nullptr);
	ASSERT_NE(arena, nullptr);
	
	void *p = yaml_arena_alloc(arena, 0, 8);
	EXPECT_EQ(p, nullptr);  // Should return NULL for zero size
	
	yaml_arena_free(arena);
}

//
// Test: NULL arena
//
TEST(YamlArena, NullArena) {
	void *p = yaml_arena_alloc(nullptr, 100, 8);
	EXPECT_EQ(p, nullptr);
	
	// Free NULL should be safe
	yaml_arena_free(nullptr);
}

//
// Test: Bulk free
//
TEST(YamlArena, BulkFree) {
	yaml_arena *arena = yaml_arena_new(nullptr);
	ASSERT_NE(arena, nullptr);
	
	// Allocate lots of memory
	for (int i = 0; i < 100; i++) {
		yaml_arena_alloc(arena, 1024, 8);
	}
	
	// Single free cleans up everything
	yaml_arena_free(arena);
	// Valgrind will verify no leaks
}

//
// Test: Context creation
//
TEST(YamlContext, CreateDestroy) {
	yaml_context *ctx = yaml_context_new(nullptr);
	ASSERT_NE(ctx, nullptr);
	
	EXPECT_NE(ctx->arena, nullptr);
	EXPECT_EQ(ctx->decoded_input, nullptr);
	EXPECT_EQ(ctx->decoded_input_len, 0);
	EXPECT_EQ(ctx->node_count, 0);
	
	yaml_context_free(ctx);
}

//
// Test: Context allocation
//
TEST(YamlContext, Alloc) {
	yaml_context *ctx = yaml_context_new(nullptr);
	ASSERT_NE(ctx, nullptr);
	
	void *p1 = yaml_context_alloc(ctx, 64, 8);
	ASSERT_NE(p1, nullptr);
	
	void *p2 = yaml_context_alloc(ctx, 128, 8);
	ASSERT_NE(p2, nullptr);
	EXPECT_NE(p1, p2);
	
	yaml_context_free(ctx);
}

//
// Test: Set the decoded input
//
TEST(YamlContext, SetDecodedInput) {
	yaml_context *ctx = yaml_context_new(nullptr);
	ASSERT_NE(ctx, nullptr);
	
	const char *input = "test: yaml";
	yaml_context_set_decoded_input(ctx, input, strlen(input));
	
	EXPECT_EQ(ctx->decoded_input, input);
	EXPECT_EQ(ctx->decoded_input_len, strlen(input));
	
	yaml_context_free(ctx);
	// The decoded stream is NOT freed: the scanner owns it.
}

//
// Test: NULL context safety
//
TEST(YamlContext, NullSafety) {
	yaml_context_free(nullptr);  // Should not crash
	
	void *p = yaml_context_alloc(nullptr, 100, 8);
	EXPECT_EQ(p, nullptr);
	
	yaml_context_set_decoded_input(nullptr, "test", 4);  // Should not crash
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

//
// GTEXT_YAML_Document::node_count
//
// The field was set to a literal 1 on three of the four paths that build a
// document, with a TODO beside one of them, and read by nothing - so it was
// wrong and nothing could notice. These tests are the reader it did not have.
//
// It is counted by the four node constructors in yaml_dom.c and lives on the
// context, and every path that builds a document gives that document a context
// of its own, so the figure is per document and not a running total.
//

// Nodes in the finished tree, as an independent count: node_count is
// accumulated while the document is built, and this walks it afterwards. Two
// routes to one number is the point.
//
// **Distinct nodes, by pointer, and that is not a detail.** A merge key splices
// the source mapping's pairs in by pointer, so one node is reachable twice, and
// an alias the writer resolved is reachable from both places that named it. A
// walk that adds one per visit counts *occurrences*; node_count counts
// allocations. The first version of this counted occurrences and read 15 where
// the document holds 14 nodes - which looks exactly like the count being one
// short, and is the oracle being wrong instead.
static void collect_nodes(
		const GTEXT_YAML_Node *n, std::set<const GTEXT_YAML_Node *> *seen) {
	if (!n) return;
	if (!seen->insert(n).second) return; // already counted: shared, not new
	if (gtext_yaml_node_type(n) == GTEXT_YAML_SEQUENCE) {
		size_t len = gtext_yaml_sequence_length(n);
		for (size_t i = 0; i < len; i++) {
			collect_nodes(gtext_yaml_sequence_get(n, i), seen);
		}
	}
	else if (gtext_yaml_node_type(n) == GTEXT_YAML_MAPPING) {
		size_t len = gtext_yaml_mapping_size(n);
		for (size_t i = 0; i < len; i++) {
			const GTEXT_YAML_Node *k = nullptr;
			const GTEXT_YAML_Node *v = nullptr;
			if (gtext_yaml_mapping_get_at(n, i, &k, &v)) {
				collect_nodes(k, seen);
				collect_nodes(v, seen);
			}
		}
	}
}

static size_t count_nodes(const GTEXT_YAML_Node *n) {
	std::set<const GTEXT_YAML_Node *> seen;
	collect_nodes(n, &seen);
	return seen.size();
}

TEST(YamlNodeCount, CountsEveryNodeOfASingleDocumentParse) {
	// 1 root mapping + 2 keys + 2 values, one of which is a sequence of 3:
	// 1 + 2 + 1 + (1 + 3) = 8 nodes by hand, and the walk below is the check on
	// that arithmetic rather than a second statement of it.
	const char *src = "a: 1\nb: [2, 3, 4]\n";
	GTEXT_YAML_Document *doc = gtext_yaml_parse(src, strlen(src), nullptr, nullptr);
	ASSERT_NE(doc, nullptr);

	EXPECT_EQ(doc->node_count, count_nodes(doc->root));
	// And it is not the literal 1 it used to be, for a document that plainly
	// holds more than one node. A document with exactly one node could not tell
	// the fixed state from the broken one.
	EXPECT_GT(doc->node_count, 1u);

	gtext_yaml_free(doc);
}

TEST(YamlNodeCount, CountsTheNodeAMergeKeyCreates) {
	// This is the only one of these tests that separates *where* the count is
	// taken, and it does it by subtraction rather than by a figure.
	//
	// The two documents are the same shape: one mapping of two keys, whose
	// second value is a mapping of two pairs. In `merged` the first of those
	// pairs is "<<: *b" - a key scalar and an alias node - and in `plain` it is
	// "q: 0" - a key scalar and a scalar. Equal allocations so far. What
	// `merged` allocates and `plain` does not is the one mapping node that
	// yaml_resolve_document() builds for the merge, so the difference is
	// exactly 1.
	//
	// Measured: 14 and 13. With the count taken where it used to be - beside
	// `doc->root = parser.root`, before the resolve - both read 13 and the
	// difference is 0, which is what this fails on. The other tests in this file
	// pass under either placement, so without this one the fix is unmeasured.
	const char *merged =
		"base: &b {x: 1, y: 2}\n"
		"derived:\n"
		"  <<: *b\n"
		"  z: 3\n";
	const char *plain =
		"base: &b {x: 1, y: 2}\n"
		"derived:\n"
		"  q: 0\n"
		"  z: 3\n";

	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.allow_merge_keys = true;

	GTEXT_YAML_Document *m = gtext_yaml_parse(merged, strlen(merged), &opts, nullptr);
	ASSERT_NE(m, nullptr);
	GTEXT_YAML_Document *p = gtext_yaml_parse(plain, strlen(plain), &opts, nullptr);
	ASSERT_NE(p, nullptr);

	// The premise: the merge really was expanded, so "derived" carries x, y and
	// z rather than a "<<" key. A test that asserted the count difference
	// without this would pass just as well if merge keys had stopped working.
	const GTEXT_YAML_Node *derived =
		gtext_yaml_mapping_get(m->root, "derived");
	ASSERT_NE(derived, nullptr);
	EXPECT_NE(gtext_yaml_mapping_get(derived, "x"), nullptr);
	EXPECT_NE(gtext_yaml_mapping_get(derived, "z"), nullptr);
	EXPECT_EQ(gtext_yaml_mapping_get(derived, "<<"), nullptr);

	EXPECT_EQ(m->node_count, p->node_count + 1)
		<< "the count was taken before the merge node was built";

	// And the field means allocations, not nodes in the tree: resolution leaves
	// the pre-merge mapping unreachable, so the total exceeds what a walk of the
	// finished document can find. That is what "Total nodes allocated" says, and
	// it is why the equality the other tests assert holds only for documents
	// that discard nothing.
	EXPECT_GT(m->node_count, count_nodes(m->root));

	gtext_yaml_free(m);
	gtext_yaml_free(p);
}

TEST(YamlNodeCountIsPerDocument, EachDocumentInAStreamCountsOnlyItsOwn) {
	// One context per document is what makes the figure per document. If the
	// stream shared one, the second document's count would include the first's
	// and the third's would include both - so the smaller document coming
	// second is what separates the two.
	const char *src =
		"---\n"
		"a: [1, 2, 3, 4, 5]\n"
		"---\n"
		"b: 1\n";
	size_t count = 0;
	GTEXT_YAML_Document **docs =
		gtext_yaml_parse_all(src, strlen(src), &count, nullptr, nullptr);
	ASSERT_NE(docs, nullptr);
	ASSERT_EQ(count, 2u);

	EXPECT_EQ(docs[0]->node_count, count_nodes(docs[0]->root));
	EXPECT_EQ(docs[1]->node_count, count_nodes(docs[1]->root));
	// The second document is the smaller one, so a running total would make it
	// the larger figure.
	EXPECT_LT(docs[1]->node_count, docs[0]->node_count);

	for (size_t i = 0; i < count; i++) {
		gtext_yaml_free(docs[i]);
	}
	free(docs);
}

TEST(YamlNodeCount, CountsEveryNodeOfAPartialParse) {
	const char *src = "a: 1\nb: [2, 3]\n";
	GTEXT_YAML_Document *doc = nullptr;
	GTEXT_YAML_Error *errors = nullptr;
	size_t error_count = 0;
	ASSERT_EQ(gtext_yaml_parse_partial(src, strlen(src), nullptr, &doc, &errors,
			&error_count, nullptr),
		GTEXT_YAML_OK);
	ASSERT_NE(doc, nullptr);

	EXPECT_EQ(doc->node_count, count_nodes(doc->root));
	EXPECT_GT(doc->node_count, 1u);

	for (size_t i = 0; i < error_count; i++) {
		gtext_yaml_error_free(&errors[i]);
	}
	free(errors);
	gtext_yaml_free(doc);
}

TEST(YamlNodeCount, CountsEveryNodeOfADocumentBuiltFromJson) {
	// gtext_yaml_parse_json() is the fourth path, and its count was assigned
	// before the resolve as well.
	const char *json = "{\"a\":1,\"b\":[2,3,4]}";
	GTEXT_YAML_Document *doc =
		gtext_yaml_parse_json(json, strlen(json), nullptr, nullptr);
	ASSERT_NE(doc, nullptr);

	EXPECT_EQ(doc->node_count, count_nodes(doc->root));
	EXPECT_GT(doc->node_count, 1u);

	gtext_yaml_free(doc);
}

TEST(YamlNodeCount, CountsEveryNodeOnTheJsonFastPathToo) {
	// gtext_yaml_parse() takes the same builder when the input looks like JSON
	// and enable_json_fast_path is on, which it is by default - so an ordinary
	// parse of JSON-shaped YAML reaches the fourth path rather than the first.
	// Without this the fast path is counted by nothing.
	const char *src = "{\"a\": 1, \"b\": [2, 3, 4]}";
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	ASSERT_TRUE(opts.enable_json_fast_path) << "the default stopped taking it";
	GTEXT_YAML_Document *doc = gtext_yaml_parse(src, strlen(src), &opts, nullptr);
	ASSERT_NE(doc, nullptr);

	EXPECT_EQ(doc->node_count, count_nodes(doc->root));
	EXPECT_GT(doc->node_count, 1u);

	gtext_yaml_free(doc);
}
