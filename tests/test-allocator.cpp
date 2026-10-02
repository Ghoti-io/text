/**
 * @file
 *
 * A caller-supplied allocator must see every allocation the covered code
 * makes, and every matching free.  These tests use a counting allocator to
 * assert both: that the count is non-zero, so the allocator is really being
 * used, and that it returns to zero, so nothing leaked or was freed through
 * the C library instead.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <gtest/gtest.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/csv.h>
#include <ghoti.io/text/yaml.h>
#include <ghoti.io/text/json.h>

namespace {

// A tracking allocator.  Each block is prefixed with its size so that
// outstanding bytes can be tracked, which is what catches a free() that went
// to the C library instead of here.
struct Counters {
	size_t live_blocks = 0;
	size_t total_allocations = 0;
	size_t live_bytes = 0;
	// Fail the allocation once this many have been served (0 = never).
	size_t fail_after = 0;
};

struct Header {
	size_t size;
	size_t guard;
};

const size_t kGuard = 0x5AFE5AFE5AFE5AFEull;

void * count_malloc(void * ctx, size_t size) {
	auto * c = static_cast<Counters *>(ctx);
	if (c->fail_after && c->total_allocations >= c->fail_after) {
		return nullptr;
	}
	void * raw = std::malloc(sizeof(Header) + (size ? size : 1));
	if (!raw) {
		return nullptr;
	}
	auto * h = static_cast<Header *>(raw);
	h->size = size;
	h->guard = kGuard;
	c->live_blocks++;
	c->total_allocations++;
	c->live_bytes += size;
	return static_cast<char *>(raw) + sizeof(Header);
}

void * count_calloc(void * ctx, size_t nitems, size_t size) {
	if (nitems && size > SIZE_MAX / nitems) {
		return nullptr; // overflow is an allocation failure, per the contract
	}
	size_t total = nitems * size;
	void * p = count_malloc(ctx, total ? total : 1);
	if (p) {
		std::memset(p, 0, total ? total : 1);
	}
	return p;
}

void count_free(void * ctx, void * ptr) {
	if (!ptr) {
		return;
	}
	auto * c = static_cast<Counters *>(ctx);
	auto * h = reinterpret_cast<Header *>(static_cast<char *>(ptr) - sizeof(Header));
	// If this fires, the pointer did not come from this allocator - which is
	// exactly the corruption a mismatched free() causes.
	ASSERT_EQ(h->guard, kGuard) << "freed a block this allocator never made";
	c->live_blocks--;
	c->live_bytes -= h->size;
	h->guard = 0;
	std::free(h);
}

void * count_realloc(void * ctx, void * ptr, size_t size) {
	if (!ptr) {
		return count_malloc(ctx, size);
	}
	auto * h = reinterpret_cast<Header *>(static_cast<char *>(ptr) - sizeof(Header));
	size_t old = h->size;
	void * fresh = count_malloc(ctx, size);
	if (!fresh) {
		return nullptr;
	}
	std::memcpy(fresh, ptr, old < size ? old : size);
	count_free(ctx, ptr);
	return fresh;
}

GTEXT_Allocator make_allocator(Counters * c) {
	GTEXT_Allocator a;
	a.ctx = c;
	a.malloc_fn = count_malloc;
	a.calloc_fn = count_calloc;
	a.realloc_fn = count_realloc;
	a.free_fn = count_free;
	return a;
}

} // namespace

TEST(Allocator, DefaultIsUsableAndHonorsTheZeroSizeContract) {
	const GTEXT_Allocator * d = gtext_allocator_default();
	ASSERT_NE(d, nullptr);

	// A zero-size request must return a usable pointer, so that NULL always
	// means failure.
	void * p = gtext_allocator_malloc(d, 0);
	EXPECT_NE(p, nullptr);
	gtext_allocator_free(d, p);

	void * z = gtext_allocator_calloc(d, 0, 0);
	EXPECT_NE(z, nullptr);
	gtext_allocator_free(d, z);

	// Overflow in calloc is an allocation failure, not a truncated block.
	EXPECT_EQ(gtext_allocator_calloc(d, SIZE_MAX, 2), nullptr);

	// A NULL allocator means the default everywhere.
	void * q = gtext_allocator_malloc(nullptr, 8);
	EXPECT_NE(q, nullptr);
	gtext_allocator_free(nullptr, q);

	// Freeing NULL is ignored.
	gtext_allocator_free(d, nullptr);
	gtext_allocator_free(nullptr, nullptr);
}

TEST(Allocator, JsonParseAndFreeBalanceThroughTheAllocator) {
	// Chosen to reach the paths that allocate outside the arena: object keys,
	// escaped strings needing a decode buffer, and preserved number lexemes.
	const char * src =
	    "{\"key\":\"plain\",\"esc\":\"a\\u00e9b\\n\",\"nested\":{\"deep\":[1,2,3]},"
	    "\"big\":123456789012345678901234567890,\"f\":1.5e10,\"t\":true,"
	    "\"n\":null,\"arr\":[\"x\",\"y\",{\"z\":[]}]}";

	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allocator = &alloc;

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * v = gtext_json_parse(src, std::strlen(src), &opts, &err);
	ASSERT_NE(v, nullptr) << (err.message ? err.message : "parse failed");

	// The allocator was actually used, rather than quietly bypassed.
	EXPECT_GT(c.total_allocations, 0u);
	EXPECT_GT(c.live_blocks, 0u);

	gtext_json_free(v);
	gtext_json_error_free(&err);

	// Everything came back. A free() that went to the C library instead would
	// leave live_blocks above zero here, and would have tripped the guard.
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, JsonParseBalancesOnTheErrorPath) {
	// Malformed input must not leak through the allocator either.
	const char * bad = "{\"a\":\"unterminated";
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allocator = &alloc;

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * v = gtext_json_parse(bad, std::strlen(bad), &opts, &err);
	EXPECT_EQ(v, nullptr);
	gtext_json_error_free(&err);

	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, JsonParseSurvivesAllocationFailure) {
	// An allocator that starts failing must produce a clean failure rather
	// than a crash or a leak.
	const char * src = "{\"a\":[1,2,3],\"b\":\"text\",\"c\":{\"d\":true}}";

	for (size_t budget = 1; budget <= 6; budget++) {
		Counters c;
		c.fail_after = budget;
		GTEXT_Allocator alloc = make_allocator(&c);
		GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
		opts.allocator = &alloc;

		GTEXT_JSON_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * v =
		    gtext_json_parse(src, std::strlen(src), &opts, &err);
		if (v) {
			gtext_json_free(v);
		}
		gtext_json_error_free(&err);
		EXPECT_EQ(c.live_blocks, 0u) << "leak with budget " << budget;
	}
}

TEST(Allocator, TwoParsesWithDifferentAllocatorsStaySeparate) {
	// The DOM records its allocator, so freeing one must not touch the other.
	const char * src = "{\"a\":[1,2,3]}";
	Counters c1, c2;
	GTEXT_Allocator a1 = make_allocator(&c1);
	GTEXT_Allocator a2 = make_allocator(&c2);

	GTEXT_JSON_Parse_Options o1 = gtext_json_parse_options_default();
	o1.allocator = &a1;
	GTEXT_JSON_Parse_Options o2 = gtext_json_parse_options_default();
	o2.allocator = &a2;

	GTEXT_JSON_Error e1, e2;
	std::memset(&e1, 0, sizeof(e1));
	std::memset(&e2, 0, sizeof(e2));
	GTEXT_JSON_Value * v1 = gtext_json_parse(src, std::strlen(src), &o1, &e1);
	GTEXT_JSON_Value * v2 = gtext_json_parse(src, std::strlen(src), &o2, &e2);
	ASSERT_NE(v1, nullptr);
	ASSERT_NE(v2, nullptr);

	size_t live2_before = c2.live_blocks;
	gtext_json_free(v1);
	EXPECT_EQ(c1.live_blocks, 0u);
	EXPECT_EQ(c2.live_blocks, live2_before) << "freeing one touched the other";

	gtext_json_free(v2);
	EXPECT_EQ(c2.live_blocks, 0u);
	gtext_json_error_free(&e1);
	gtext_json_error_free(&e2);
}

TEST(Allocator, NullAllocatorOptionStillParses) {
	// The default path must be unaffected by the new field.
	const char * src = "{\"a\":1}";
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	EXPECT_EQ(opts.allocator, nullptr);

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * v = gtext_json_parse(src, std::strlen(src), &opts, &err);
	ASSERT_NE(v, nullptr);
	gtext_json_free(v);
	gtext_json_error_free(&err);
}

// ---------------------------------------------------------------------------
// The wrappers with no allocator
//
// Each of gtext_allocator_malloc/_calloc/_realloc/_free accepts NULL and falls
// back to gcu_allocator_default(), which is what lets every call site pass an
// optional allocator straight through without checking it first.  That
// fallback was reached by no test for _realloc, which tools/coverage.sh
// reported among the lines no test executes.
//
// The whole point of the NULL contract is that a caller need not care, so it
// has to work for all four, not three.
// ---------------------------------------------------------------------------

TEST(Allocator, NullMeansTheDefaultForEveryWrapper) {
	// malloc
	void * p = gtext_allocator_malloc(nullptr, 64);
	ASSERT_NE(p, nullptr);
	std::memset(p, 0xAB, 64);

	// realloc, growing - the contents must survive
	p = gtext_allocator_realloc(nullptr, p, 256);
	ASSERT_NE(p, nullptr);
	for (int i = 0; i < 64; ++i) {
		EXPECT_EQ(static_cast<unsigned char *>(p)[i], 0xAB) << "byte " << i;
	}

	// realloc, shrinking
	p = gtext_allocator_realloc(nullptr, p, 32);
	ASSERT_NE(p, nullptr);
	for (int i = 0; i < 32; ++i) {
		EXPECT_EQ(static_cast<unsigned char *>(p)[i], 0xAB) << "byte " << i;
	}

	gtext_allocator_free(nullptr, p);

	// realloc from NULL behaves as malloc
	void * q = gtext_allocator_realloc(nullptr, nullptr, 48);
	ASSERT_NE(q, nullptr);
	gtext_allocator_free(nullptr, q);

	// calloc zeroes
	unsigned char * z =
	    static_cast<unsigned char *>(gtext_allocator_calloc(nullptr, 16, 4));
	ASSERT_NE(z, nullptr);
	for (int i = 0; i < 64; ++i) {
		EXPECT_EQ(z[i], 0) << "byte " << i << " was not zeroed";
	}
	gtext_allocator_free(nullptr, z);

	// free(NULL, NULL) must be a no-op rather than a crash
	gtext_allocator_free(nullptr, nullptr);
}

TEST(Allocator, ExplicitDefaultMatchesTheNullFallback) {
	// Passing gtext_allocator_default() explicitly and passing NULL must be
	// the same thing, or the fallback is a second implementation.
	const GTEXT_Allocator * def = gtext_allocator_default();
	ASSERT_NE(def, nullptr);

	void * a = gtext_allocator_malloc(def, 32);
	void * b = gtext_allocator_malloc(nullptr, 32);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);

	// Cross-free: a block from one must be releasable through the other.
	gtext_allocator_free(nullptr, a);
	gtext_allocator_free(def, b);

	void * c = gtext_allocator_malloc(def, 16);
	ASSERT_NE(c, nullptr);
	c = gtext_allocator_realloc(nullptr, c, 64);
	ASSERT_NE(c, nullptr);
	gtext_allocator_free(def, c);
}


// ---------------------------------------------------------------------------
// CSV
//
// The hazard documentation/formats/allocator-todo.md names is not a leak: a
// table whose arena came from the caller's allocator and whose structure came
// from the C library corrupts the heap when it is freed, and the caller cannot
// see it coming. The tracking allocator's guard word is what catches it - a
// free() that reached the C library instead leaves live_blocks above zero, and
// a free() of a block this allocator never made trips the guard outright.
//
// So every test below asserts both halves: that the count went up, so the
// allocator is really in the path, and that it came back to zero.
// ---------------------------------------------------------------------------

TEST(Allocator, CsvParseAndFreeBalanceThroughTheAllocator) {
	// Chosen to reach the paths that allocate: a header row and its map,
	// quoted fields needing an unescape buffer, an embedded newline, and
	// enough rows to grow past the initial capacity.
	std::string src = "name,value,note\n";
	for (int i = 0; i < 40; i++) {
		src += "row" + std::to_string(i) + ",\"a,b\",\"multi\nline\"\n";
	}

	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_CSV_Parse_Options opts = gtext_csv_parse_options_default();
	opts.allocator = &alloc;
	opts.dialect.treat_first_row_as_header = true;

	GTEXT_CSV_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_CSV_Table * t =
	    gtext_csv_parse_table(src.data(), src.size(), &opts, &err);
	ASSERT_NE(t, nullptr) << (err.message ? err.message : "parse failed");
	EXPECT_GT(c.total_allocations, 0u);
	EXPECT_GT(c.live_blocks, 0u);
	EXPECT_EQ(gtext_csv_row_count(t), 40u);

	gtext_csv_free_table(t);
	gtext_csv_error_free(&err);
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, CsvTableMutationStaysWithTheTablesAllocator) {
	// The paths allocator-todo.md calls out by name: the temporary arrays in
	// the column operations, and the clone and compact paths that build a
	// second set of structures before swapping them in. Each takes the
	// allocator of the table it works on, so none of them may move a caller's
	// data onto the C heap.
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);

	const char * headers[3] = {"a", "b", "c"};
	GTEXT_CSV_Table * t = gtext_csv_new_table_with_headers_and_allocator(
	    headers, nullptr, 3, &alloc);
	ASSERT_NE(t, nullptr);
	EXPECT_GT(c.total_allocations, 0u);

	for (int i = 0; i < 20; i++) {
		std::string v = "v" + std::to_string(i);
		const char * row[3] = {v.c_str(), "second", "third"};
		ASSERT_EQ(gtext_csv_row_append(t, row, nullptr, 3, nullptr),
		    GTEXT_CSV_OK);
	}

	// One entry per row, the header row included: csv_get_rows_to_modify()
	// counts it, so 20 data rows plus the header is 21.
	const char * col[21];
	col[0] = "d";
	for (int i = 1; i < 21; i++) {
		col[i] = "added";
	}
	EXPECT_EQ(
	    gtext_csv_column_append_with_values(t, "d", 1, col, nullptr),
	    GTEXT_CSV_OK);
	EXPECT_EQ(gtext_csv_normalize_to_max(t), GTEXT_CSV_OK);
	EXPECT_EQ(gtext_csv_table_compact(t), GTEXT_CSV_OK);

	GTEXT_CSV_Table * clone = gtext_csv_clone(t);
	ASSERT_NE(clone, nullptr) << "a clone inherits the source's allocator";

	gtext_csv_free_table(clone);
	gtext_csv_free_table(t);
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, CsvStreamBalancesThroughTheAllocator) {
	// The streaming parser's own structure and its field buffer, which grows
	// across chunk boundaries - fed one byte at a time so that it must.
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_CSV_Parse_Options opts = gtext_csv_parse_options_default();
	opts.allocator = &alloc;

	GTEXT_CSV_Event_cb cb = [](const GTEXT_CSV_Event *, void *) {
		return GTEXT_CSV_OK;
	};
	GTEXT_CSV_Stream * st = gtext_csv_stream_new(&opts, cb, nullptr);
	ASSERT_NE(st, nullptr);
	EXPECT_GT(c.total_allocations, 0u);

	std::string src = "a,\"quoted,field\",c\n";
	for (int i = 0; i < 30; i++) {
		src += "\"a long quoted field that forces the buffer to grow\",b,c\n";
	}
	GTEXT_CSV_Error err;
	std::memset(&err, 0, sizeof(err));
	for (size_t i = 0; i < src.size(); i++) {
		ASSERT_EQ(gtext_csv_stream_feed(st, src.data() + i, 1, &err),
		    GTEXT_CSV_OK)
		    << (err.message ? err.message : "feed failed") << " at " << i;
	}
	EXPECT_EQ(gtext_csv_stream_finish(st, &err), GTEXT_CSV_OK);
	gtext_csv_stream_free(st);
	gtext_csv_error_free(&err);

	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

/*
 * Empty input takes its own path - csv_create_empty_table() rather than the
 * parse - and that path was not covered until a planted defect went unnoticed
 * by every test above. Planting the exact fault allocator-todo.md describes
 * (the table structure on the C library, its arena on the caller's allocator)
 * left the suite green, because nothing here had ever parsed zero bytes.
 */
TEST(Allocator, CsvEmptyInputBalancesThroughTheAllocator) {
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_CSV_Parse_Options opts = gtext_csv_parse_options_default();
	opts.allocator = &alloc;

	GTEXT_CSV_Table * t = gtext_csv_parse_table("", 0, &opts, nullptr);
	ASSERT_NE(t, nullptr);
	EXPECT_GT(c.total_allocations, 0u) << "the allocator was bypassed";
	EXPECT_GT(c.live_blocks, 0u);
	EXPECT_EQ(gtext_csv_row_count(t), 0u);

	gtext_csv_free_table(t);
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, CsvParseBalancesOnTheErrorPath) {
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_CSV_Parse_Options opts = gtext_csv_parse_options_default();
	opts.allocator = &alloc;

	const char * bad = "a,\"unterminated";
	GTEXT_CSV_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_CSV_Table * t =
	    gtext_csv_parse_table(bad, std::strlen(bad), &opts, &err);
	if (t) {
		gtext_csv_free_table(t);
	}
	gtext_csv_error_free(&err);
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, CsvParseSurvivesAllocationFailure) {
	const char * src = "a,b,c\n1,2,3\n4,\"5,5\",6\n";
	for (size_t budget = 1; budget <= 8; budget++) {
		Counters c;
		c.fail_after = budget;
		GTEXT_Allocator alloc = make_allocator(&c);
		GTEXT_CSV_Parse_Options opts = gtext_csv_parse_options_default();
		opts.allocator = &alloc;

		GTEXT_CSV_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_CSV_Table * t =
		    gtext_csv_parse_table(src, std::strlen(src), &opts, &err);
		if (t) {
			gtext_csv_free_table(t);
		}
		gtext_csv_error_free(&err);
		EXPECT_EQ(c.live_blocks, 0u) << "leak with budget " << budget;
	}
}

TEST(Allocator, TwoCsvTablesWithDifferentAllocatorsStaySeparate) {
	// The table records its allocator, so freeing one must not touch the
	// other - and must not free through the other, which is what the guard
	// word would catch.
	const char * src = "a,b\n1,2\n";
	Counters c1, c2;
	GTEXT_Allocator a1 = make_allocator(&c1);
	GTEXT_Allocator a2 = make_allocator(&c2);

	GTEXT_CSV_Parse_Options o1 = gtext_csv_parse_options_default();
	o1.allocator = &a1;
	GTEXT_CSV_Parse_Options o2 = gtext_csv_parse_options_default();
	o2.allocator = &a2;

	GTEXT_CSV_Table * t1 =
	    gtext_csv_parse_table(src, std::strlen(src), &o1, nullptr);
	GTEXT_CSV_Table * t2 =
	    gtext_csv_parse_table(src, std::strlen(src), &o2, nullptr);
	ASSERT_NE(t1, nullptr);
	ASSERT_NE(t2, nullptr);

	size_t before = c2.live_blocks;
	gtext_csv_free_table(t1);
	EXPECT_EQ(c1.live_blocks, 0u);
	EXPECT_EQ(c2.live_blocks, before) << "freeing one touched the other";

	gtext_csv_free_table(t2);
	EXPECT_EQ(c2.live_blocks, 0u);
}

/* The pull reader copies every event's bytes into its queue, so it allocates
   where the push parser does not - and it must do so through the caller's
   allocator like everything else the parse options cover. */
TEST(Allocator, CsvPullReaderBalancesThroughTheAllocator) {
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_CSV_Parse_Options opts = gtext_csv_parse_options_default();
	opts.allocator = &alloc;

	GTEXT_CSV_Reader * r = gtext_csv_reader_new(&opts);
	ASSERT_NE(r, nullptr);
	EXPECT_GT(c.total_allocations, 0u);

	// Enough records to grow the queue past its initial capacity, and enough
	// unread events at the end that freeing has queued events to release.
	std::string src;
	for (int i = 0; i < 50; i++) {
		src += "field" + std::to_string(i) + ",\"quoted,value\",third\n";
	}
	GTEXT_CSV_Error err;
	std::memset(&err, 0, sizeof(err));
	ASSERT_EQ(gtext_csv_reader_feed(r, src.data(), src.size(), &err),
	    GTEXT_CSV_OK);
	ASSERT_EQ(gtext_csv_reader_feed(r, nullptr, 0, &err), GTEXT_CSV_OK);

	// Read only a few, so the rest are still queued at free time.
	for (int i = 0; i < 5; i++) {
		GTEXT_CSV_Event ev;
		std::memset(&ev, 0, sizeof(ev));
		EXPECT_EQ(gtext_csv_reader_next(r, &ev), GTEXT_CSV_OK);
	}

	gtext_csv_reader_free(r);
	gtext_csv_error_free(&err);
	EXPECT_EQ(c.live_blocks, 0u) << "queued events were not released";
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, CsvNullAllocatorOptionStillWorks) {
	// The default path must be unchanged: no allocator named, nothing
	// reaching a caller's allocator, and the table still correct.
	const char * src = "a,b\n1,2\n";
	GTEXT_CSV_Parse_Options opts = gtext_csv_parse_options_default();
	EXPECT_EQ(opts.allocator, nullptr) << "no allocator by default";

	GTEXT_CSV_Table * t =
	    gtext_csv_parse_table(src, std::strlen(src), &opts, nullptr);
	ASSERT_NE(t, nullptr);
	EXPECT_EQ(gtext_csv_row_count(t), 2u);
	gtext_csv_free_table(t);

	GTEXT_CSV_Table * plain = gtext_csv_new_table_with_allocator(nullptr);
	ASSERT_NE(plain, nullptr);
	gtext_csv_free_table(plain);
}


// ---------------------------------------------------------------------------
// YAML
//
// The lesson CSV taught, applied: there is one balanced test per way of
// *creating* a document, not one per way of using one. Planting the documented
// defect in CSV left the whole suite green because nothing had ever parsed zero
// bytes, and the entry point that reached it had no test of its own. YAML has
// more ways in than CSV does - parse, parse_all, parse_json, parse_partial,
// parse_safe, document_new, the streaming parser and the pull reader - so each
// gets one.
// ---------------------------------------------------------------------------

TEST(Allocator, YamlParseAndFreeBalanceThroughTheAllocator) {
	// Chosen to reach the paths that allocate outside the arena: anchors and
	// aliases with their table, tags, comments, a merge key, a block scalar,
	// and enough nesting to grow the parser's stack.
	const char * src =
	    "# leading comment\n"
	    "defaults: &defaults\n"
	    "  a: 1\n"
	    "  b: two\n"
	    "merged:\n"
	    "  <<: *defaults\n"
	    "  c: 3.5\n"
	    "block: |\n"
	    "  line one\n"
	    "  line two\n"
	    "folded: >\n"
	    "  folded text\n"
	    "tagged: !!str 42\n"
	    "nested: [[[[1, 2]]], {x: {y: {z: null}}}]\n"
	    "utf8: \"caf\xc3\xa9 \xe4\xb8\x80\"\n"
	    "alias_use: *defaults\n";

	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.allocator = &alloc;
	opts.retain_comments = true;

	GTEXT_YAML_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * doc =
	    gtext_yaml_parse(src, std::strlen(src), &opts, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");
	EXPECT_GT(c.total_allocations, 0u) << "the allocator was bypassed";
	EXPECT_GT(c.live_blocks, 0u);

	gtext_yaml_free(doc);
	gtext_yaml_error_free(&err);
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, YamlParseAllBalancesThroughTheAllocator) {
	const char * src =
	    "--- \nfirst: 1\n--- \nsecond: [1,2,3]\n--- \nthird: &a {x: 1}\n";

	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.allocator = &alloc;

	size_t count = 0;
	GTEXT_YAML_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document ** docs =
	    gtext_yaml_parse_all(src, std::strlen(src), &count, &opts, &err);
	ASSERT_NE(docs, nullptr) << (err.message ? err.message : "parse failed");
	EXPECT_EQ(count, 3u);
	EXPECT_GT(c.total_allocations, 0u);

	for (size_t i = 0; i < count; i++) {
		gtext_yaml_free(docs[i]);
	}
	// The array of pointers is on the C library, deliberately: parse_all's
	// published contract has the caller release it with free(), and routing it
	// through the allocator would make that documented call a free through the
	// wrong one. Everything of any size is in the documents.
	std::free(docs);
	gtext_yaml_error_free(&err);
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, YamlParseJsonFastPathBalancesThroughTheAllocator) {
	// The JSON fast path builds a YAML document from the JSON parser, which is
	// a different route into the same arena.
	const char * src = "{\"a\":[1,2,{\"b\":\"text\"}],\"c\":null}";

	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.allocator = &alloc;
	opts.enable_json_fast_path = true;

	GTEXT_YAML_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * doc =
	    gtext_yaml_parse(src, std::strlen(src), &opts, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");
	EXPECT_GT(c.total_allocations, 0u);
	gtext_yaml_free(doc);
	gtext_yaml_error_free(&err);
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, YamlDocumentNewBalancesThroughTheAllocator) {
	// A document built rather than parsed: the DOM constructors, which allocate
	// from the same arena by a different door.
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.allocator = &alloc;

	GTEXT_YAML_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * doc = gtext_yaml_document_new(&opts, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");
	EXPECT_GT(c.total_allocations, 0u);

	GTEXT_YAML_Node * map = gtext_yaml_node_new_mapping(doc, nullptr, nullptr);
	ASSERT_NE(map, nullptr);
	for (int i = 0; i < 40; i++) {
		std::string key = "key" + std::to_string(i);
		std::string val = "value" + std::to_string(i);
		GTEXT_YAML_Node * k =
		    gtext_yaml_node_new_scalar(doc, key.c_str(), nullptr, nullptr);
		GTEXT_YAML_Node * v =
		    gtext_yaml_node_new_scalar(doc, val.c_str(), nullptr, nullptr);
		ASSERT_NE(k, nullptr);
		ASSERT_NE(v, nullptr);
		ASSERT_NE(gtext_yaml_mapping_set(doc, map, k, v), nullptr);
	}
	EXPECT_TRUE(gtext_yaml_document_set_root(doc, map));

	// A clone takes the allocator of the document it came from.
	GTEXT_YAML_Node * copy = gtext_yaml_node_clone(doc, map);
	EXPECT_NE(copy, nullptr);

	gtext_yaml_free(doc);
	gtext_yaml_error_free(&err);
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, YamlStreamBalancesThroughTheAllocator) {
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.allocator = &alloc;

	GTEXT_YAML_Stream * st = gtext_yaml_stream_new(&opts, nullptr, nullptr);
	ASSERT_NE(st, nullptr);
	EXPECT_GT(c.total_allocations, 0u);

	// Fed a byte at a time, so the scanner's buffers must grow.
	const std::string src =
	    "a: &anchor\n  b: |\n    a long block scalar that forces a buffer\n"
	    "c: *anchor\nd: [1, 2, 3, 4, 5]\n";
	for (size_t i = 0; i < src.size(); i++) {
		ASSERT_EQ(gtext_yaml_stream_feed(st, src.data() + i, 1), GTEXT_YAML_OK)
		    << "at " << i;
	}
	EXPECT_EQ(gtext_yaml_stream_finish(st), GTEXT_YAML_OK);
	gtext_yaml_stream_free(st);

	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, YamlPullReaderBalancesThroughTheAllocator) {
	// The reader copies every event's strings into its queue, so it allocates
	// where the push parser does not - and some events are left unread at free
	// time so the queued remainder has to be released too.
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.allocator = &alloc;
	opts.retain_comments = true;

	GTEXT_YAML_Reader * r = gtext_yaml_reader_new(&opts);
	ASSERT_NE(r, nullptr);
	EXPECT_GT(c.total_allocations, 0u);

	std::string src = "%YAML 1.2\n---\n# a comment\n";
	for (int i = 0; i < 30; i++) {
		src += "key" + std::to_string(i) + ": &a" + std::to_string(i) +
		    " !!str value\n";
	}
	GTEXT_YAML_Error err;
	std::memset(&err, 0, sizeof(err));
	ASSERT_EQ(gtext_yaml_reader_feed(r, src.data(), src.size(), &err),
	    GTEXT_YAML_OK)
	    << (err.message ? err.message : "");
	ASSERT_EQ(gtext_yaml_reader_feed(r, nullptr, 0, &err), GTEXT_YAML_OK);

	for (int i = 0; i < 4; i++) {
		GTEXT_YAML_Event ev;
		std::memset(&ev, 0, sizeof(ev));
		if (gtext_yaml_reader_next(r, &ev, &err) != GTEXT_YAML_OK) {
			break;
		}
	}
	gtext_yaml_reader_free(r);
	gtext_yaml_error_free(&err);
	EXPECT_EQ(c.live_blocks, 0u) << "queued events were not released";
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, YamlToJsonBalancesThroughTheAllocator) {
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.allocator = &alloc;

	const char * src = "a: 1\nb: [1, 2, {c: text}]\nd: null\n";
	GTEXT_YAML_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * doc =
	    gtext_yaml_parse(src, std::strlen(src), &opts, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "");

	GTEXT_JSON_Value * json = nullptr;
	EXPECT_EQ(gtext_yaml_to_json(doc, &json, nullptr), GTEXT_YAML_OK);
	if (json) {
		gtext_json_free(json);
	}
	gtext_yaml_free(doc);
	gtext_yaml_error_free(&err);
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, YamlParseBalancesOnTheErrorPath) {
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.allocator = &alloc;

	// A few different ways to fail, since the unwind path differs.
	const char * bad[] = {
	    "a: [1, 2\n",          // unterminated flow
	    "a: *undefined\n",     // alias with no anchor
	    "\t- tab indent\n",    // a tab where indentation belongs
	    "a: \"unterminated\n", // unterminated quoted scalar
	};
	for (const char * src : bad) {
		GTEXT_YAML_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_YAML_Document * doc =
		    gtext_yaml_parse(src, std::strlen(src), &opts, &err);
		if (doc) {
			gtext_yaml_free(doc);
		}
		gtext_yaml_error_free(&err);
		EXPECT_EQ(c.live_blocks, 0u) << "leak after [" << src << "]";
	}
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, YamlParseSurvivesAllocationFailure) {
	const char * src = "a: &x [1, 2]\nb: *x\nc: {d: text}\n";
	for (size_t budget = 1; budget <= 10; budget++) {
		Counters c;
		c.fail_after = budget;
		GTEXT_Allocator alloc = make_allocator(&c);
		GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
		opts.allocator = &alloc;

		GTEXT_YAML_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_YAML_Document * doc =
		    gtext_yaml_parse(src, std::strlen(src), &opts, &err);
		if (doc) {
			gtext_yaml_free(doc);
		}
		gtext_yaml_error_free(&err);
		EXPECT_EQ(c.live_blocks, 0u) << "leak with budget " << budget;
	}
}

TEST(Allocator, TwoYamlDocumentsWithDifferentAllocatorsStaySeparate) {
	const char * src = "a: 1\n";
	Counters c1, c2;
	GTEXT_Allocator a1 = make_allocator(&c1);
	GTEXT_Allocator a2 = make_allocator(&c2);

	GTEXT_YAML_Parse_Options o1 = gtext_yaml_parse_options_default();
	o1.allocator = &a1;
	GTEXT_YAML_Parse_Options o2 = gtext_yaml_parse_options_default();
	o2.allocator = &a2;

	GTEXT_YAML_Document * d1 =
	    gtext_yaml_parse(src, std::strlen(src), &o1, nullptr);
	GTEXT_YAML_Document * d2 =
	    gtext_yaml_parse(src, std::strlen(src), &o2, nullptr);
	ASSERT_NE(d1, nullptr);
	ASSERT_NE(d2, nullptr);

	size_t before = c2.live_blocks;
	gtext_yaml_free(d1);
	EXPECT_EQ(c1.live_blocks, 0u);
	EXPECT_EQ(c2.live_blocks, before) << "freeing one touched the other";
	gtext_yaml_free(d2);
	EXPECT_EQ(c2.live_blocks, 0u);
}

TEST(Allocator, YamlNullAllocatorOptionStillParses) {
	const char * src = "a: 1\nb: [1,2]\n";
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	EXPECT_EQ(opts.allocator, nullptr) << "no allocator by default";
	GTEXT_YAML_Document * doc =
	    gtext_yaml_parse(src, std::strlen(src), &opts, nullptr);
	ASSERT_NE(doc, nullptr);
	gtext_yaml_free(doc);
}

// ---------------------------------------------------------------------------
// The JSON writer's working memory.
//
// GTEXT_JSON_Write_Options::allocator covers the scratch a write needs and
// releases - the frame stack the value walk carries, the sorted index array a
// sort_object_keys write builds per object, and for the incremental API the
// GTEXT_JSON_Writer handle and its stack.
//
// **The sink is deliberately not covered**, which is why these tests say
// nothing about it: a sink is created before any options are seen and outlives
// the write, so it owns its buffer. That is the line
// GTEXT_INI_Write_Options::allocator already draws, and the reason this module
// was listed as "the writer takes no allocator" for as long as it was: the
// documentation recorded writers as exempt wholesale, when only the sink is.
//
// A separate allocator is used for each so that a block crossing between them
// trips the guard word rather than merely counting wrong.
// ---------------------------------------------------------------------------

TEST(Allocator, JsonWriteValueBalancesThroughTheAllocator) {
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);

	// Parsed with the default allocator on purpose: the value is not what is
	// under test here, and keeping the two apart is what makes a stray block
	// attributable.
	// strlen, not a literal: the first version of this counted the bytes by
	// hand, got 48 for a 49-byte document, and the parse refused a truncated
	// object. A length that has to be kept in step with a string beside it is
	// a length that will not be.
	const char * doc =
		"{\"b\":[1,2,{\"d\":4,\"c\":[5,6,7]}],\"a\":{\"z\":1,\"y\":2}}";
	GTEXT_JSON_Value * v = gtext_json_parse(doc, strlen(doc), nullptr, nullptr);
	ASSERT_NE(v, nullptr);

	GTEXT_JSON_Sink sink;
	ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);

	GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
	opts.allocator = &alloc;
	// Both of these pull on the covered memory: sorting allocates an index
	// array per object, and pretty printing walks every frame.
	opts.sort_object_keys = true;
	opts.pretty = true;

	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	ASSERT_EQ(gtext_json_write_value(&sink, &opts, v, &err), GTEXT_JSON_OK)
		<< (err.message ? err.message : "");

	EXPECT_GT(c.total_allocations, 0u) << "the allocator was bypassed";
	// Everything the write took, the write gave back - there is no handle to
	// free afterwards, so the balance must already hold here.
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);

	gtext_json_sink_buffer_free(&sink);
	gtext_json_free(v);
}

TEST(Allocator, JsonWriteFrameGrowthStaysWithTheAllocator) {
	// Deeper than the inline frame array, so the heap growth path runs. The
	// inline array is what makes an ordinary write allocate nothing at all,
	// which would leave the growth path untested by every shallow document.
	std::string deep;
	const int levels = 400;
	for (int i = 0; i < levels; i++) deep += "[";
	deep += "1";
	for (int i = 0; i < levels; i++) deep += "]";

	GTEXT_JSON_Parse_Options popts = gtext_json_parse_options_default();
	popts.max_depth = levels + 8;
	GTEXT_JSON_Value * v =
		gtext_json_parse(deep.data(), deep.size(), &popts, nullptr);
	ASSERT_NE(v, nullptr);

	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_JSON_Sink sink;
	ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
	GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
	opts.allocator = &alloc;

	ASSERT_EQ(gtext_json_write_value(&sink, &opts, v, nullptr), GTEXT_JSON_OK);
	EXPECT_GT(c.total_allocations, 0u) << "the frame stack never left the inline array";
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);

	gtext_json_sink_buffer_free(&sink);
	gtext_json_free(v);
}

TEST(Allocator, JsonIncrementalWriterBalancesThroughTheAllocator) {
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);

	GTEXT_JSON_Sink sink;
	ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
	GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
	opts.allocator = &alloc;

	GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, &opts);
	ASSERT_NE(w, nullptr);
	// The handle and its stack both come from here, so a block is live before
	// anything is written. The defect this guards against is the handle coming
	// from one allocator and going back to another, which the guard word in
	// count_free() reports rather than letting it corrupt the heap quietly.
	EXPECT_GT(c.live_blocks, 0u) << "the handle did not come from the allocator";

	// Nested deeply enough to grow the writer's own stack past its default.
	const int levels = 128;
	for (int i = 0; i < levels; i++) {
		ASSERT_EQ(gtext_json_writer_array_begin(w), GTEXT_JSON_OK) << i;
	}
	for (int i = 0; i < levels; i++) {
		ASSERT_EQ(gtext_json_writer_array_end(w), GTEXT_JSON_OK) << i;
	}

	gtext_json_writer_free(w);
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);

	gtext_json_sink_buffer_free(&sink);
}

TEST(Allocator, JsonWriterWithNoAllocatorOptionStillWrites) {
	// The null fallback, which is what every existing caller passes: a write
	// that names no allocator must behave exactly as it did before the field
	// existed.
	const char * doc = "{\"a\":[1,2]}";
	GTEXT_JSON_Value * v = gtext_json_parse(doc, strlen(doc), nullptr, nullptr);
	ASSERT_NE(v, nullptr);

	GTEXT_JSON_Sink sink;
	ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
	GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
	EXPECT_EQ(opts.allocator, nullptr) << "the default must name no allocator";
	opts.sort_object_keys = true;

	ASSERT_EQ(gtext_json_write_value(&sink, &opts, v, nullptr), GTEXT_JSON_OK);
	EXPECT_GT(gtext_json_sink_buffer_size(&sink), 0u);

	gtext_json_sink_buffer_free(&sink);
	gtext_json_free(v);
}

// ---------------------------------------------------------------------------
// JSON Pointer.
//
// The walk allocates one transient buffer per token - the decoded form, since
// `~0` and `~1` mean a token is not always a span of the input - released
// before the next token is read. Nothing outlives the call, so there is no free
// function to hand the allocator back to.
//
// Neither gtext_json_pointer_get() nor its mutable twin takes options, so this
// is a separate entry point rather than a field, the shape
// gtext_csv_new_table_with_allocator() already set.
// ---------------------------------------------------------------------------

TEST(Allocator, JsonPointerBalancesThroughTheAllocator) {
	const char * doc = "{\"a\":{\"b\":[10,20,{\"c~d\":7}]}}";
	GTEXT_JSON_Value * v = gtext_json_parse(doc, strlen(doc), nullptr, nullptr);
	ASSERT_NE(v, nullptr);

	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);

	// Several tokens, and one carrying an escape so the decode is not a copy.
	const char * ptr = "/a/b/2/c~0d";
	const GTEXT_JSON_Value * found = gtext_json_pointer_get_with_allocator(
		v, ptr, strlen(ptr), &alloc);
	ASSERT_NE(found, nullptr);
	EXPECT_EQ(gtext_json_typeof(found), GTEXT_JSON_NUMBER);

	EXPECT_GT(c.total_allocations, 0u) << "the allocator was bypassed";
	// Every token buffer is released inside the walk, so the balance holds the
	// moment it returns rather than after some later free.
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);

	gtext_json_free(v);
}

TEST(Allocator, JsonPointerBalancesWhenTheWalkFails) {
	// The error paths are the ones with a buffer in hand: six of the seven
	// returns in the walk happen with `decoded` live. A pointer that resolves
	// is the one case that does not exercise them.
	const char * doc = "{\"a\":[1,2]}";
	GTEXT_JSON_Value * v = gtext_json_parse(doc, strlen(doc), nullptr, nullptr);
	ASSERT_NE(v, nullptr);

	const char * misses[] = {
		"/a/9",      // index past the end
		"/a/x",      // not an index, and the container is an array
		"/nope",     // key absent
		"/a/0/deep", // descending into a scalar
	};
	for (const char * ptr : misses) {
		Counters c;
		GTEXT_Allocator alloc = make_allocator(&c);
		EXPECT_EQ(gtext_json_pointer_get_with_allocator(v, ptr, strlen(ptr), &alloc),
			nullptr) << ptr;
		EXPECT_EQ(c.live_blocks, 0u) << ptr;
		EXPECT_EQ(c.live_bytes, 0u) << ptr;
	}

	gtext_json_free(v);
}

TEST(Allocator, JsonPointerMutableTwinUsesTheAllocatorToo) {
	const char * doc = "{\"a\":{\"b\":1}}";
	GTEXT_JSON_Value * v = gtext_json_parse(doc, strlen(doc), nullptr, nullptr);
	ASSERT_NE(v, nullptr);

	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	const char * ptr = "/a/b";
	GTEXT_JSON_Value * found =
		gtext_json_pointer_get_mut_with_allocator(v, ptr, strlen(ptr), &alloc);
	ASSERT_NE(found, nullptr);
	EXPECT_GT(c.total_allocations, 0u) << "the allocator was bypassed";
	EXPECT_EQ(c.live_blocks, 0u);

	gtext_json_free(v);
}

TEST(Allocator, JsonPointerWithoutAnAllocatorStillResolves) {
	// The delegating pair must be unchanged: same answers, and the caller's
	// allocator untouched because it was never named.
	const char * doc = "{\"a\":[1,2]}";
	GTEXT_JSON_Value * v = gtext_json_parse(doc, strlen(doc), nullptr, nullptr);
	ASSERT_NE(v, nullptr);

	// **Not a counter assertion.** The first version of this built a tracking
	// allocator, passed NULL, and checked that it served nothing - which is true
	// however the code behaves, since an allocator that is not passed cannot be
	// reached. -Werror caught it as an unused variable, which was the compiler
	// noticing the test was vacuous before I did.
	//
	// What is worth asserting is that the three spellings agree, so the
	// delegating pair is still the same walk.
	const char * ptr = "/a/1";
	const GTEXT_JSON_Value * plain = gtext_json_pointer_get(v, ptr, strlen(ptr));
	const GTEXT_JSON_Value * null_alloc =
		gtext_json_pointer_get_with_allocator(v, ptr, strlen(ptr), nullptr);
	ASSERT_NE(plain, nullptr);
	EXPECT_EQ(plain, null_alloc) << "the NULL fallback resolved differently";

	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	const GTEXT_JSON_Value * named =
		gtext_json_pointer_get_with_allocator(v, ptr, strlen(ptr), &alloc);
	EXPECT_EQ(plain, named) << "naming an allocator changed the answer";
	EXPECT_GT(c.total_allocations, 0u) << "the named allocator was bypassed";
	EXPECT_EQ(c.live_blocks, 0u);

	gtext_json_free(v);
}

/*
 * The streaming parser, and the floor is the point of these two tests.
 *
 * `live_blocks == 0` holds when the allocator serves *nothing*, and
 * `total_allocations > 0` holds from a wrapper's own structure while everything
 * beneath it bypasses - which is how gtext_json_stream_new() came to ignore
 * GTEXT_JSON_Parse_Options::allocator while src/json/json_pull_reader.c sat on
 * the `check-allocators` list and passed.  What separates the two states is
 * *which* allocations arrive, so these assert on bytes outstanding while the
 * stream is alive: the input buffer alone is JSON_TOKEN_BUFFER-independent and
 * 4096 bytes by construction, so a build that allocated it from the C library
 * cannot reach the floor no matter how the rest is counted.
 */
TEST(Allocator, JsonStreamBalancesThroughTheAllocator) {
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allocator = &alloc;

	GTEXT_JSON_Event_cb cb =
	    [](void *, const GTEXT_JSON_Event *, GTEXT_JSON_Error *) {
		    return GTEXT_JSON_OK;
	    };
	GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, cb, nullptr);
	ASSERT_NE(st, nullptr);
	// The stream's own structure and its 4096-byte input buffer, both of which
	// exist before a single byte is fed.
	EXPECT_GE(c.live_bytes, 4096u) << "the input buffer bypassed the allocator";
	EXPECT_GE(c.total_allocations, 2u);

	// Shaped to take every growth path the stream has: nesting past the
	// initial stack capacity, more keys in one object than the name array's
	// first four slots, a string longer than the token buffer's initial 64
	// bytes, and more input than the 4096-byte buffer holds.
	std::string src = "{";
	for (int i = 0; i < 40; i++) {
		src += "\"key" + std::to_string(i) + "\":[{";
	}
	src += "\"deep\":\"";
	src.append(300, 'x');
	src += "\"";
	for (int i = 0; i < 40; i++) {
		src += "}]";
		if (i + 1 < 40) {
			src += ",\"pad" + std::to_string(i) + "\":1";
		}
	}
	src += "}";
	src.append(5000, ' '); // past the input buffer, so it must grow

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	// One byte at a time, so every token crosses a chunk boundary and has to
	// be buffered rather than referenced in place.
	for (size_t i = 0; i < src.size(); i++) {
		ASSERT_EQ(gtext_json_stream_feed(st, src.data() + i, 1, &err),
		    GTEXT_JSON_OK)
		    << (err.message ? err.message : "feed failed") << " at " << i;
	}
	EXPECT_EQ(gtext_json_stream_finish(st, &err), GTEXT_JSON_OK)
	    << (err.message ? err.message : "finish failed");
	gtext_json_stream_free(st);
	gtext_json_error_free(&err);

	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, JsonStreamBalancesWhenTheInputIsRefused) {
	// The error path frees a stack that still has entries on it, each with its
	// own key-name array - the one case json_stream_free_names() is reached
	// from gtext_json_stream_free() rather than from json_stream_pop().
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allocator = &alloc;

	GTEXT_JSON_Event_cb cb =
	    [](void *, const GTEXT_JSON_Event *, GTEXT_JSON_Error *) {
		    return GTEXT_JSON_OK;
	    };
	GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, cb, nullptr);
	ASSERT_NE(st, nullptr);

	// Opens ten objects, names a key in each, then goes wrong - so the stream
	// is freed mid-document with names outstanding at every level.
	std::string src;
	for (int i = 0; i < 10; i++) {
		src += "{\"a" + std::to_string(i) + "\":";
	}
	src += "@";

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Status status =
	    gtext_json_stream_feed(st, src.data(), src.size(), &err);
	if (status == GTEXT_JSON_OK) {
		status = gtext_json_stream_finish(st, &err);
	}
	EXPECT_NE(status, GTEXT_JSON_OK) << "'@' should not parse";
	gtext_json_stream_free(st);
	gtext_json_error_free(&err);

	EXPECT_EQ(c.live_blocks, 0u) << "a refused document leaked";
	EXPECT_EQ(c.live_bytes, 0u);
}

/*
 * The pull reader, which is the test that was missing.
 *
 * tests/test-allocator.cpp had CsvStream/CsvPullReader and
 * YamlStream/YamlPullReader and neither of the JSON pair - three formats with
 * the same two-layer shape, two of them covered on both layers and one on
 * neither.  src/json/json_pull_reader.c routes its own queue and key copies
 * through the caller's allocator and then hands the same options to
 * gtext_json_stream_new(), so until the stream honoured them the larger half of
 * a reader's memory came from the C library with every gate green.
 */
TEST(Allocator, JsonPullReaderBalancesThroughTheAllocator) {
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allocator = &alloc;

	GTEXT_JSON_Reader * r = gtext_json_reader_new(&opts);
	ASSERT_NE(r, nullptr);
	// The reader's own structure is a few hundred bytes; the stream it owns
	// brings the 4096-byte input buffer with it.  Before the stream was
	// converted this floor was the assertion that failed while
	// `total_allocations > 0` still passed.
	EXPECT_GE(c.live_bytes, 4096u)
	    << "the reader's stream bypassed the allocator";

	std::string src = "[";
	for (int i = 0; i < 60; i++) {
		if (i) {
			src += ",";
		}
		src += "{\"name" + std::to_string(i) + "\":\"value\"}";
	}
	src += "]";

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	ASSERT_EQ(gtext_json_reader_feed(r, src.data(), src.size(), &err),
	    GTEXT_JSON_OK)
	    << (err.message ? err.message : "feed failed");
	ASSERT_EQ(gtext_json_reader_feed(r, nullptr, 0, &err), GTEXT_JSON_OK);

	// A few read, the rest still queued at free time.
	for (int i = 0; i < 5; i++) {
		GTEXT_JSON_Event ev;
		std::memset(&ev, 0, sizeof(ev));
		EXPECT_EQ(gtext_json_reader_next(r, &ev), GTEXT_JSON_OK);
	}

	gtext_json_reader_free(r);
	gtext_json_error_free(&err);
	EXPECT_EQ(c.live_blocks, 0u) << "queued events were not released";
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, JsonStreamWithoutAnAllocatorStillParses) {
	// The default path unchanged, and compared against the named-allocator
	// path rather than merely asserted to work: both must deliver the same
	// events in the same order.
	const char * src = "{\"a\":[1,2,{\"b\":null}],\"c\":\"d\"}";
	std::vector<int> plain;
	std::vector<int> tracked;

	GTEXT_JSON_Event_cb cb = [](void * user, const GTEXT_JSON_Event * ev,
	                             GTEXT_JSON_Error *) {
		static_cast<std::vector<int> *>(user)->push_back((int)ev->type);
		return GTEXT_JSON_OK;
	};

	GTEXT_JSON_Parse_Options plain_opts = gtext_json_parse_options_default();
	EXPECT_EQ(plain_opts.allocator, nullptr) << "no allocator by default";
	GTEXT_JSON_Stream * a = gtext_json_stream_new(&plain_opts, cb, &plain);
	ASSERT_NE(a, nullptr);
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	ASSERT_EQ(gtext_json_stream_feed(a, src, std::strlen(src), &err),
	    GTEXT_JSON_OK);
	ASSERT_EQ(gtext_json_stream_finish(a, &err), GTEXT_JSON_OK);
	gtext_json_stream_free(a);
	gtext_json_error_free(&err);

	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_JSON_Parse_Options named = gtext_json_parse_options_default();
	named.allocator = &alloc;
	GTEXT_JSON_Stream * b = gtext_json_stream_new(&named, cb, &tracked);
	ASSERT_NE(b, nullptr);
	std::memset(&err, 0, sizeof(err));
	ASSERT_EQ(gtext_json_stream_feed(b, src, std::strlen(src), &err),
	    GTEXT_JSON_OK);
	ASSERT_EQ(gtext_json_stream_finish(b, &err), GTEXT_JSON_OK);
	gtext_json_stream_free(b);
	gtext_json_error_free(&err);

	EXPECT_EQ(plain, tracked) << "the two allocators gave different events";
	EXPECT_FALSE(plain.empty());
	EXPECT_EQ(c.live_blocks, 0u);
}

/*
 * JSON Patch, which needed no API change: a patch is applied to a tree, and
 * json_context::alloc records the allocator the parse was given, so
 * gtext_json_patch_apply() can read it from its own argument.  That makes the
 * allocator the *parse* was handed the one a patch must use - which is also why
 * these tests pass one set of options and then assert on the same counters
 * across both calls.
 *
 * The token buffers only exist for a token that has to be decoded or parsed as
 * an index, so the paths below are chosen for that: `~0` and `~1` escapes, and
 * numeric array indices.  A patch over plain unescaped object keys allocates
 * nothing here and would pass whatever this code did.
 */
TEST(Allocator, JsonPatchBalancesThroughTheAllocator) {
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allocator = &alloc;

	const char * doc = R"({"a~b":1,"c/d":2,"arr":[10,20,30],"o":{"k":"v"}})";
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * root =
	    gtext_json_parse(doc, std::strlen(doc), &opts, &err);
	ASSERT_NE(root, nullptr) << (err.message ? err.message : "parse failed");

	// Every operation whose path needs decoding or index parsing.
	const char * patch_text = R"([
	  {"op":"replace","path":"/a~0b","value":11},
	  {"op":"replace","path":"/c~1d","value":22},
	  {"op":"add","path":"/arr/1","value":15},
	  {"op":"remove","path":"/arr/0"},
	  {"op":"add","path":"/arr/-","value":40},
	  {"op":"add","path":"/o/new","value":"w"},
	  {"op":"remove","path":"/o/k"}
	])";
	GTEXT_JSON_Value * patch =
	    gtext_json_parse(patch_text, std::strlen(patch_text), &opts, &err);
	ASSERT_NE(patch, nullptr) << (err.message ? err.message : "patch failed");

	size_t before = c.total_allocations;
	EXPECT_EQ(gtext_json_patch_apply(root, patch, &err), GTEXT_JSON_OK)
	    << (err.message ? err.message : "apply failed");
	EXPECT_GT(c.total_allocations, before)
	    << "the patch allocated nothing through the tree's allocator";

	gtext_json_free(patch);
	gtext_json_free(root);
	gtext_json_error_free(&err);

	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, JsonPatchBalancesWhenAnOperationFails) {
	// The error paths, which are most of the frees in this file: a token that
	// decodes and then names nothing, and an index past the end.  Each returns
	// with both buffers live, so a missed free here is invisible to a patch
	// that succeeds.
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allocator = &alloc;

	const char * patches[] = {
	    R"([{"op":"replace","path":"/no~0such","value":1}])",
	    R"([{"op":"remove","path":"/arr/99"}])",
	    R"([{"op":"add","path":"/arr/99","value":1}])",
	    R"([{"op":"remove","path":"/o/missing"}])",
	    R"([{"op":"replace","path":"/arr/notanindex","value":1}])",
	};
	for (const char * patch_text : patches) {
		const char * doc = R"({"a~b":1,"arr":[1,2],"o":{"k":"v"}})";
		GTEXT_JSON_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * root =
		    gtext_json_parse(doc, std::strlen(doc), &opts, &err);
		ASSERT_NE(root, nullptr);
		GTEXT_JSON_Value * patch =
		    gtext_json_parse(patch_text, std::strlen(patch_text), &opts, &err);
		ASSERT_NE(patch, nullptr);

		EXPECT_NE(gtext_json_patch_apply(root, patch, &err), GTEXT_JSON_OK)
		    << patch_text << " should not apply";

		gtext_json_free(patch);
		gtext_json_free(root);
		gtext_json_error_free(&err);
	}
	EXPECT_EQ(c.live_blocks, 0u) << "a refused operation leaked";
	EXPECT_EQ(c.live_bytes, 0u);
}

TEST(Allocator, JsonPatchWithoutAnAllocatorStillApplies) {
	// The default path, compared against the named-allocator path rather than
	// only asserted to work: both must produce the same document.
	const char * doc = R"({"a~b":1,"arr":[1,2,3]})";
	const char * patch_text = R"([
	  {"op":"replace","path":"/a~0b","value":9},
	  {"op":"add","path":"/arr/1","value":5}
	])";

	auto apply_with = [&](const GTEXT_JSON_Parse_Options * o) {
		GTEXT_JSON_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * root =
		    gtext_json_parse(doc, std::strlen(doc), o, &err);
		GTEXT_JSON_Value * patch =
		    gtext_json_parse(patch_text, std::strlen(patch_text), o, &err);
		std::string out;
		if (root && patch
		    && gtext_json_patch_apply(root, patch, &err) == GTEXT_JSON_OK) {
			GTEXT_JSON_Write_Options wo = gtext_json_write_options_default();
			GTEXT_JSON_Sink sink;
			if (gtext_json_sink_buffer(&sink) == GTEXT_JSON_OK) {
				if (gtext_json_write_value(&sink, &wo, root, &err)
				    == GTEXT_JSON_OK) {
					out.assign(gtext_json_sink_buffer_data(&sink),
					    gtext_json_sink_buffer_size(&sink));
				}
				gtext_json_sink_buffer_free(&sink);
			}
		}
		gtext_json_free(patch);
		gtext_json_free(root);
		gtext_json_error_free(&err);
		return out;
	};

	GTEXT_JSON_Parse_Options plain = gtext_json_parse_options_default();
	EXPECT_EQ(plain.allocator, nullptr) << "no allocator by default";
	std::string without = apply_with(&plain);

	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_JSON_Parse_Options named = gtext_json_parse_options_default();
	named.allocator = &alloc;
	std::string with = apply_with(&named);

	EXPECT_FALSE(without.empty());
	EXPECT_EQ(without, with) << "the two allocators gave different documents";
	EXPECT_GT(c.total_allocations, 0u);
	EXPECT_EQ(c.live_blocks, 0u);
}

TEST(Allocator, JsonCloneInheritsTheSourcesAllocator) {
	// gtext_json_clone() takes no options, so the only allocator it could use
	// is the source's - and it passed NULL, putting the copy in the C library
	// while src->ctx held the caller's allocator.
	Counters c;
	GTEXT_Allocator alloc = make_allocator(&c);
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allocator = &alloc;

	const char * doc = R"({"a":[1,2,3],"b":{"c":"d"},"e":"a longer string here"})";
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * root =
	    gtext_json_parse(doc, std::strlen(doc), &opts, &err);
	ASSERT_NE(root, nullptr) << (err.message ? err.message : "parse failed");

	size_t before_blocks = c.live_blocks;
	size_t before_bytes = c.live_bytes;
	GTEXT_JSON_Value * copy = gtext_json_clone(root);
	ASSERT_NE(copy, nullptr);
	// A deep copy of this document is not small; if it came from the C library
	// these would not move at all.
	EXPECT_GT(c.live_blocks, before_blocks) << "the clone bypassed the allocator";
	EXPECT_GT(c.live_bytes, before_bytes);
	EXPECT_TRUE(gtext_json_equal(root, copy, GTEXT_JSON_EQUAL_LEXEME));

	gtext_json_free(copy);
	gtext_json_free(root);
	gtext_json_error_free(&err);
	EXPECT_EQ(c.live_blocks, 0u);
	EXPECT_EQ(c.live_bytes, 0u);
}
