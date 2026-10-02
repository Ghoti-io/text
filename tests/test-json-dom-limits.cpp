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

#include <cstring>
#include <string>
#include <gtest/gtest.h>

#include <ghoti.io/text/json.h>

namespace {

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

int main(int argc, char ** argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
