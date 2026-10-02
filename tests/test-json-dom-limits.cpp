/**
 * @file
 *
 * What the JSON DOM's own walks can be handed, from the side the parser does
 * not guard.
 *
 * `max_depth` is a *parser* limit. json_core.h documents it carefully - the
 * parser is a recursive descent at about 448 bytes a level, the default 256 is
 * safe anywhere, and raising it "is choosing a number rather than removing
 * one" - and all of that is about a document that arrived as text. A value
 * built through the DOM API arrived no such way, and nothing bounded it.
 *
 * Two shapes get in that way, and both ended the process:
 *
 * - a value that contains itself, which four bytes of API misuse made and
 *   nothing reported;
 * - a value a few tens of thousands deep, which every walk over it - free,
 *   write, equality - recursed into.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <pthread.h>

#include <cstring>
#include <functional>
#include <string>
#include <gtest/gtest.h>

#include <ghoti.io/text/json.h>

namespace {

/* n nested arrays, assembled inside-out: each new array wraps the previous
   one, so the *child* of every push is the big half.  That is the shape the
   cycle check is quadratic in if it has no shortcut, which is why the tests
   below are written this way round rather than top-down. */
GTEXT_JSON_Value * BuildNested(size_t n) {
	GTEXT_JSON_Value * inner = gtext_json_new_null();
	if (!inner) return nullptr;
	for (size_t i = 0; i < n; i++) {
		GTEXT_JSON_Value * outer = gtext_json_new_array();
		if (!outer) return nullptr;
		if (gtext_json_array_push(outer, inner) != GTEXT_JSON_OK) return nullptr;
		inner = outer;
	}
	return inner;
}

/* What the value actually is, rather than what it was asked to be. */
size_t MeasureDepth(const GTEXT_JSON_Value * v) {
	size_t d = 0;
	while (v && gtext_json_typeof(v) == GTEXT_JSON_ARRAY
			&& gtext_json_array_size(v) > 0) {
		v = gtext_json_array_get(v, 0);
		d++;
	}
	return d;
}

/* Run @p fn on a thread with a deliberately small stack.
 *
 * The walks under test die in proportion to depth, and on the main thread's
 * 8 MiB that takes a *lot* of depth: bisected on this machine, free survived
 * 104535 levels and died at 104925, equality survived 74532 and died at 74922,
 * and the writer survived 32450 and died at 32839 - about 80, 112 and 257 bytes
 * a level.
 *
 * Reaching those depths is not the problem; paying for them is.  Every level of
 * a value built through the DOM API is a separate context with an arena of its
 * own, so depth costs about 3 KB a level of real memory - and a shared 200000
 * for all three tests came to 1.3 GB, which under ASan's redzones was enough to
 * have the run killed by the OOM killer.  That is a bad test however it
 * finishes.
 *
 * So the stack is made small instead of the value large.  The property is the
 * same one - this walk's stack use is proportional to the depth of what it
 * walks - and asking it with 512 KB costs megabytes rather than gigabytes.  A
 * recursive walk still takes the whole process down, which is the honest way
 * for it to report. */
void RunOnSmallStack(const std::function<void()> & fn) {
	pthread_attr_t attr;
	ASSERT_EQ(pthread_attr_init(&attr), 0);
	ASSERT_EQ(pthread_attr_setstacksize(&attr, 512 * 1024), 0);
	pthread_t thread;
	auto trampoline = [](void * arg) -> void * {
		(*static_cast<const std::function<void()> *>(arg))();
		return nullptr;
	};
	ASSERT_EQ(pthread_create(&thread, &attr, trampoline,
		const_cast<std::function<void()> *>(&fn)), 0);
	ASSERT_EQ(pthread_join(thread, nullptr), 0);
	pthread_attr_destroy(&attr);
}

} // namespace

/* The witness, and it is four bytes of ordinary API use:
 *
 *     GTEXT_JSON_Value *a = gtext_json_new_array();
 *     gtext_json_array_push(a, a);   // returned GTEXT_JSON_OK
 *     gtext_json_free(a);            // SIGSEGV
 *
 * Nothing reported anything until the process died.  A cyclic value is not
 * merely unfreeable: it has no JSON to be written as, and every DOM walk here
 * - free, write, equality, clone, merge-patch, JSONPath, schema compilation -
 * is a walk over containment, so the place to refuse it is the door it comes in
 * by rather than a dozen walks in turn.  The YAML half refuses the equivalent
 * in apply_merge_keys() for the same reason. */
TEST(JsonDomLimits, AValueMayNotBeStoredInsideItself) {
	GTEXT_JSON_Value * a = gtext_json_new_array();
	ASSERT_NE(a, nullptr);
	EXPECT_EQ(gtext_json_array_push(a, a), GTEXT_JSON_E_INVALID);
	/* Refused means nothing happened, not "half happened". */
	EXPECT_EQ(gtext_json_array_size(a), 0u);
	gtext_json_free(a);

	GTEXT_JSON_Value * o = gtext_json_new_object();
	ASSERT_NE(o, nullptr);
	EXPECT_EQ(gtext_json_object_put(o, "k", 1, o), GTEXT_JSON_E_INVALID);
	EXPECT_EQ(gtext_json_object_size(o), 0u);
	gtext_json_free(o);

	/* set() and insert() are the same door with a different handle, and both
	   are checked before the element they would replace is freed - so a refused
	   call leaves the value it was called on exactly as it was. */
	GTEXT_JSON_Value * s = gtext_json_new_array();
	ASSERT_NE(s, nullptr);
	ASSERT_EQ(gtext_json_array_push(s, gtext_json_new_null()), GTEXT_JSON_OK);
	EXPECT_EQ(gtext_json_array_set(s, 0, s), GTEXT_JSON_E_INVALID);
	EXPECT_EQ(gtext_json_array_insert(s, 0, s), GTEXT_JSON_E_INVALID);
	EXPECT_EQ(gtext_json_array_size(s), 1u);
	EXPECT_EQ(gtext_json_typeof(gtext_json_array_get(s, 0)), GTEXT_JSON_NULL);
	gtext_json_free(s);
}

/* Identity is the cheap case and not the whole question: the cycle may close
   through any number of values.  These are the cases the `contained` shortcut
   must *not* skip, which is what makes them worth writing down separately - a
   check that only compared the two pointers would pass the test above and fail
   every one of these. */
TEST(JsonDomLimits, ACycleIsRefusedHoweverLongItIs) {
	{
		GTEXT_JSON_Value * a = gtext_json_new_array();
		GTEXT_JSON_Value * b = gtext_json_new_array();
		ASSERT_NE(a, nullptr);
		ASSERT_NE(b, nullptr);
		ASSERT_EQ(gtext_json_array_push(b, a), GTEXT_JSON_OK);
		EXPECT_EQ(gtext_json_array_push(a, b), GTEXT_JSON_E_INVALID);
		EXPECT_EQ(gtext_json_array_size(a), 0u);
		gtext_json_free(b);
	}
	{
		GTEXT_JSON_Value * a = gtext_json_new_array();
		GTEXT_JSON_Value * b = gtext_json_new_array();
		GTEXT_JSON_Value * c = gtext_json_new_array();
		ASSERT_NE(a, nullptr);
		ASSERT_NE(b, nullptr);
		ASSERT_NE(c, nullptr);
		ASSERT_EQ(gtext_json_array_push(c, b), GTEXT_JSON_OK);
		ASSERT_EQ(gtext_json_array_push(b, a), GTEXT_JSON_OK);
		EXPECT_EQ(gtext_json_array_push(a, c), GTEXT_JSON_E_INVALID);
		EXPECT_EQ(gtext_json_array_size(a), 0u);
		gtext_json_free(c);
	}
	{
		/* Through objects, and through a mix of the two. */
		GTEXT_JSON_Value * o = gtext_json_new_object();
		GTEXT_JSON_Value * p = gtext_json_new_object();
		ASSERT_NE(o, nullptr);
		ASSERT_NE(p, nullptr);
		ASSERT_EQ(gtext_json_object_put(p, "k", 1, o), GTEXT_JSON_OK);
		EXPECT_EQ(gtext_json_object_put(o, "k", 1, p), GTEXT_JSON_E_INVALID);
		EXPECT_EQ(gtext_json_object_size(o), 0u);
		gtext_json_free(p);
	}
	{
		GTEXT_JSON_Value * arr = gtext_json_new_array();
		GTEXT_JSON_Value * obj = gtext_json_new_object();
		ASSERT_NE(arr, nullptr);
		ASSERT_NE(obj, nullptr);
		ASSERT_EQ(gtext_json_object_put(obj, "a", 1, arr), GTEXT_JSON_OK);
		EXPECT_EQ(gtext_json_array_push(arr, obj), GTEXT_JSON_E_INVALID);
		EXPECT_EQ(gtext_json_array_size(arr), 0u);
		gtext_json_free(obj);
	}
}

/* The lower bound, which is the half a refusal can get wrong in the other
   direction: an ordinary insertion of a value that has children of its own,
   and of a value that is already a child somewhere else, are both fine. */
TEST(JsonDomLimits, AnOrdinaryInsertionIsNotRefused) {
	GTEXT_JSON_Value * root = gtext_json_new_array();
	GTEXT_JSON_Value * branch = gtext_json_new_array();
	GTEXT_JSON_Value * leaf = gtext_json_new_array();
	ASSERT_NE(root, nullptr);
	ASSERT_NE(branch, nullptr);
	ASSERT_NE(leaf, nullptr);

	ASSERT_EQ(gtext_json_array_push(branch, leaf), GTEXT_JSON_OK);
	EXPECT_EQ(gtext_json_array_push(root, branch), GTEXT_JSON_OK);
	EXPECT_EQ(gtext_json_array_size(root), 1u);
	/* Two steps, not three: MeasureDepth stops at the empty array at the
	   bottom, which has no element 0 to follow. */
	EXPECT_EQ(MeasureDepth(root), 2u);

	/* A sibling that is not an ancestor, so the walk has somewhere to go and
	   still has to say yes. */
	GTEXT_JSON_Value * other = gtext_json_new_array();
	ASSERT_NE(other, nullptr);
	EXPECT_EQ(gtext_json_array_push(other, root), GTEXT_JSON_OK);
	EXPECT_EQ(MeasureDepth(other), 3u);
	gtext_json_free(other);
}

/* And the depth the DOM API can reach, which the parser's limit says nothing
   about.  Each of these three walks recursed; each takes the process down if it
   goes back to recursing, which is the honest way to report the thing it exists
   to prevent.
   
   See RunOnSmallStack() for why the stack is small rather than the value deep,
   and for the depths at which each of them died on the main thread's 8 MiB.
   Every depth here is at least twice what 512 KB can hold of that walk's
   frames. */
TEST(JsonDomLimits, FreeingADeepValueDoesNotUseTheCStack) {
	const size_t depth = 15000;   /* 512 KB holds about 6550 of its frames */
	GTEXT_JSON_Value * v = BuildNested(depth);
	ASSERT_NE(v, nullptr);
	/* The control: a value that is not actually this deep would be freed by any
	   implementation. */
	ASSERT_EQ(MeasureDepth(v), depth);
	RunOnSmallStack([v] { gtext_json_free(v); });
}

TEST(JsonDomLimits, WritingADeepValueDoesNotUseTheCStack) {
	const size_t depth = 6000;    /* 512 KB holds about 2040 of its frames */
	GTEXT_JSON_Value * v = BuildNested(depth);
	ASSERT_NE(v, nullptr);
	ASSERT_EQ(MeasureDepth(v), depth);

	GTEXT_JSON_Sink sink;
	ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
	GTEXT_JSON_Status status = GTEXT_JSON_E_INVALID;
	RunOnSmallStack([&] {
		status = gtext_json_write_value(&sink, nullptr, v, nullptr);
	});
	EXPECT_EQ(status, GTEXT_JSON_OK);
	/* The control: one "[" and one "]" per level, and "null" at the bottom.  A
	   walk that stopped early would not crash either. */
	EXPECT_EQ(gtext_json_sink_buffer_size(&sink), 2 * depth + 4);
	gtext_json_sink_buffer_free(&sink);
	gtext_json_free(v);
}

TEST(JsonDomLimits, ComparingDeepValuesDoesNotUseTheCStack) {
	const size_t depth = 12000;   /* 512 KB holds about 4680 of its frames */
	GTEXT_JSON_Value * a = BuildNested(depth);
	GTEXT_JSON_Value * b = BuildNested(depth);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	ASSERT_EQ(MeasureDepth(a), depth);
	ASSERT_EQ(MeasureDepth(b), depth);

	bool same = false;
	RunOnSmallStack([&] { same = gtext_json_equal(a, b, GTEXT_JSON_EQUAL_LEXEME); });
	EXPECT_TRUE(same);

	/* And the comparison really went to the bottom: a walk that gave up early
	   would answer "equal" for these two as well. */
	GTEXT_JSON_Value * c = BuildNested(depth);
	ASSERT_NE(c, nullptr);
	GTEXT_JSON_Value * tail = c;
	for (size_t i = 0; i + 1 < depth; i++) {
		tail = const_cast<GTEXT_JSON_Value *>(gtext_json_array_get(tail, 0));
		ASSERT_NE(tail, nullptr);
	}
	ASSERT_EQ(gtext_json_array_set(tail, 0, gtext_json_new_bool(true)),
		GTEXT_JSON_OK);
	bool differ = true;
	RunOnSmallStack([&] { differ = !gtext_json_equal(a, c, GTEXT_JSON_EQUAL_LEXEME); });
	EXPECT_TRUE(differ);

	gtext_json_free(a);
	gtext_json_free(b);
	gtext_json_free(c);
}

int main(int argc, char ** argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
