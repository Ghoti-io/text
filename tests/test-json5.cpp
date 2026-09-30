/**
 * @file
 *
 * The JSON5 extensions, one option at a time.
 *
 * JSON5 is a dialect rather than a flag, so each of its differences from JSON
 * is a separate option here and each is tested in both directions: the form is
 * accepted when its option is on, and refused when it is off. A test that only
 * checked acceptance would pass just as well against a parser that accepted
 * the form unconditionally, which is the mistake that matters - strict JSON is
 * the default and has to stay refusing.
 *
 * Every case that can go through the streaming parser does. The two parsers
 * share the lexer but not the grammar, and the lexer's number scan carries
 * state across a chunk boundary, so a hex literal or a signed Infinity split
 * between feeds is the case most likely to be wrong.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <gtest/gtest.h>

#include <ghoti.io/text/json.h>

namespace {

GTEXT_JSON_Parse_Options number_opts(void) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.parse_int64 = true;
	opts.parse_uint64 = true;
	opts.parse_double = true;
	opts.preserve_number_lexeme = true;
	return opts;
}

// Parse and return the status, freeing whatever came back.
GTEXT_JSON_Status dom_status(
    const std::string & src, const GTEXT_JSON_Parse_Options * opts) {
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * v = gtext_json_parse(src.data(), src.size(), opts, &err);
	GTEXT_JSON_Status status = v ? GTEXT_JSON_OK : err.code;
	if (v) {
		gtext_json_free(v);
	}
	gtext_json_error_free(&err);
	return status;
}

// Feed the streaming parser in chunks of `chunk` bytes and report whether the
// whole document was accepted.
bool stream_accepts(const std::string & src,
    const GTEXT_JSON_Parse_Options * opts, size_t chunk) {
	GTEXT_JSON_Event_cb cb = [](void *, const GTEXT_JSON_Event *,
	                             GTEXT_JSON_Error *) { return GTEXT_JSON_OK; };
	GTEXT_JSON_Stream * st = gtext_json_stream_new(opts, cb, nullptr);
	if (!st) {
		return false;
	}
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Status status = GTEXT_JSON_OK;
	for (size_t i = 0; i < src.size(); i += chunk) {
		size_t n = std::min(chunk, src.size() - i);
		status = gtext_json_stream_feed(st, src.data() + i, n, &err);
		if (status != GTEXT_JSON_OK) {
			break;
		}
	}
	if (status == GTEXT_JSON_OK) {
		status = gtext_json_stream_finish(st, &err);
	}
	gtext_json_stream_free(st);
	gtext_json_error_free(&err);
	return status == GTEXT_JSON_OK;
}

// Both parsers, and the streaming one byte at a time as well as whole.
void expect_both(const std::string & src,
    const GTEXT_JSON_Parse_Options * opts, bool valid) {
	EXPECT_EQ(dom_status(src, opts) == GTEXT_JSON_OK, valid)
	    << "DOM parser, input [" << src << "]";
	/* **Every chunk size, not the two ends.** This used to try the whole input
	   and then one byte at a time, on the reasoning that those are the extremes.
	   They are, and the interesting sizes are in between: the streaming reader
	   accepted `{+Infinity:1}` at chunk 10 and above - exactly the width of
	   `{+Infinity`, the point at which the token arrives inside one feed - and
	   refused it below that. Sampling the ends found nothing because at the full
	   width both readers were wrong together. Inputs here are a few dozen bytes,
	   so sweeping is free. */
	for (size_t chunk = 1; chunk <= (src.size() ? src.size() : 1); chunk++) {
		EXPECT_EQ(stream_accepts(src, opts, chunk), valid)
		    << "streaming parser, chunk " << chunk << ", input [" << src << "]";
	}
}

// The one number in a single-element array.
const GTEXT_JSON_Value * only_number(const GTEXT_JSON_Value * doc) {
	if (!doc || gtext_json_typeof(doc) != GTEXT_JSON_ARRAY) {
		return nullptr;
	}
	return gtext_json_array_get(doc, 0);
}

} // namespace

// ===========================================================================
// Hexadecimal integers
// ===========================================================================

TEST(Json5Hex, ValuesAndSpellings) {
	struct Case {
		const char * src;
		uint64_t u64;
		int64_t i64;
		double dbl;
		bool has_u64;
	};
	const Case cases[] = {
		{"[0x0]", 0, 0, 0.0, true},
		{"[0x1F]", 31, 31, 31.0, true},
		{"[0X1f]", 31, 31, 31.0, true},
		{"[0xdeadBEEF]", 0xdeadBEEFull, 0xdeadBEEF, 3735928559.0, true},
		{"[0xff]", 255, 255, 255.0, true},
		// The sign belongs to the number, not to the digits.
		{"[-0x10]", 0, -16, -16.0, false},
	};

	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_hex_numbers = true;

	for (const Case & c : cases) {
		GTEXT_JSON_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * doc =
		    gtext_json_parse(c.src, std::strlen(c.src), &opts, &err);
		ASSERT_NE(doc, nullptr)
		    << c.src << ": " << (err.message ? err.message : "no message");
		const GTEXT_JSON_Value * n = only_number(doc);
		ASSERT_NE(n, nullptr) << c.src;
		EXPECT_EQ(gtext_json_typeof(n), GTEXT_JSON_NUMBER) << c.src;

		int64_t i64 = 0;
		EXPECT_EQ(gtext_json_get_i64(n, &i64), GTEXT_JSON_OK) << c.src;
		EXPECT_EQ(i64, c.i64) << c.src;

		uint64_t u64 = 0;
		if (c.has_u64) {
			EXPECT_EQ(gtext_json_get_u64(n, &u64), GTEXT_JSON_OK) << c.src;
			EXPECT_EQ(u64, c.u64) << c.src;
		}

		double d = 0.0;
		EXPECT_EQ(gtext_json_get_double(n, &d), GTEXT_JSON_OK) << c.src;
		EXPECT_EQ(d, c.dbl) << c.src;

		gtext_json_free(doc);
		gtext_json_error_free(&err);
	}
}

/* The lexeme is what was written, not what it means: a writer asked to
   round-trip the document has to be able to reproduce the source spelling. */
TEST(Json5Hex, TheLexemeKeepsTheSpelling) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_hex_numbers = true;

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	const char * src = "[0XdeadBEEF]";
	GTEXT_JSON_Value * doc =
	    gtext_json_parse(src, std::strlen(src), &opts, &err);
	ASSERT_NE(doc, nullptr);
	const GTEXT_JSON_Value * n = only_number(doc);
	ASSERT_NE(n, nullptr);
	const char * lexeme = nullptr;
	size_t lexeme_len = 0;
	ASSERT_EQ(gtext_json_get_number_lexeme(n, &lexeme, &lexeme_len),
	    GTEXT_JSON_OK);
	EXPECT_EQ(std::string(lexeme, lexeme_len), "0XdeadBEEF");
	gtext_json_free(doc);
	gtext_json_error_free(&err);
}

/* 'e' is a hex digit, not an exponent marker, and '.' cannot open a fraction
   in a hex literal. Both are cases where reusing the decimal scan would give a
   wrong answer rather than an error. */
TEST(Json5Hex, ELooksLikeAnExponentAndIsADigit) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_hex_numbers = true;

	/* 0x1e2 is the case that separates the two readings: 482 if 'e' is a
	   digit, 100 if it is an exponent marker. */
	struct Case {
		const char * src;
		int64_t value;
	};
	const Case cases[] = {
	    {"[0xe]", 14},
	    {"[0x1e2]", 482},
	    {"[0x1ee]", 494},
	    {"[0xEEE]", 3822},
	    {"[0x1ee5]", 7909},
	};
	for (const Case & c : cases) {
		GTEXT_JSON_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * doc =
		    gtext_json_parse(c.src, std::strlen(c.src), &opts, &err);
		ASSERT_NE(doc, nullptr)
		    << c.src << ": " << (err.message ? err.message : "no message");
		int64_t i64 = 0;
		ASSERT_EQ(gtext_json_get_i64(only_number(doc), &i64), GTEXT_JSON_OK)
		    << c.src;
		EXPECT_EQ(i64, c.value) << c.src;
		gtext_json_free(doc);
		gtext_json_error_free(&err);
	}

	// A fraction is not part of the hex grammar, so this is not a number.
	EXPECT_NE(dom_status("[0x1.8]", &opts), GTEXT_JSON_OK);
	// Neither is an exponent written the decimal way.
	EXPECT_NE(dom_status("[0x1p4]", &opts), GTEXT_JSON_OK);
}

TEST(Json5Hex, RefusedSpellings) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_hex_numbers = true;
	const char * refused[] = {
	    "[0x]",     // no digits
	    "[0xg]",    // not a hex digit
	    "[x10]",    // no leading zero
	    "[00x10]",  // two leading zeros
	    "[0x10x]",  // a second x
	    "[1x10]",   // the x is not after a zero
	};
	for (const char * src : refused) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

TEST(Json5Hex, RefusedWhenTheOptionIsOff) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	EXPECT_EQ(opts.allow_hex_numbers, false);
	for (const char * src : {"[0x10]", "[0X10]", "[-0x10]"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

TEST(Json5Hex, BothParsersAgreeAcrossChunkBoundaries) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_hex_numbers = true;
	struct Case {
		const char * src;
		bool valid;
	};
	const Case cases[] = {
	    {"[0x1F]", true},
	    {"[0xdeadBEEF,1]", true},
	    {"[-0x10]", true},
	    {"{\"a\":0xff}", true},
	    {"[0x]", false},
	    {"[0x1.8]", false},
	};
	for (const Case & c : cases) {
		expect_both(c.src, &opts, c.valid);
	}
}

// ===========================================================================
// A leading plus
// ===========================================================================

TEST(Json5LeadingPlus, NumbersAndTheirValues) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_leading_plus = true;

	struct Case {
		const char * src;
		double dbl;
	};
	const Case cases[] = {
	    {"[+1]", 1.0},
	    {"[+0]", 0.0},
	    {"[+1.5]", 1.5},
	    {"[+1.5e2]", 150.0},
	    {"[+12345]", 12345.0},
	};
	for (const Case & c : cases) {
		GTEXT_JSON_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * doc =
		    gtext_json_parse(c.src, std::strlen(c.src), &opts, &err);
		ASSERT_NE(doc, nullptr)
		    << c.src << ": " << (err.message ? err.message : "no message");
		double d = 0.0;
		EXPECT_EQ(gtext_json_get_double(only_number(doc), &d), GTEXT_JSON_OK)
		    << c.src;
		EXPECT_EQ(d, c.dbl) << c.src;
		gtext_json_free(doc);
		gtext_json_error_free(&err);
	}
}

/* The integer accessors have to see through the sign too, or a caller that
   asks for an int64 gets "no such representation" for a whole number. */
TEST(Json5LeadingPlus, TheIntegerAccessorsSeeThroughIt) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_leading_plus = true;

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	const char * src = "[+42]";
	GTEXT_JSON_Value * doc =
	    gtext_json_parse(src, std::strlen(src), &opts, &err);
	ASSERT_NE(doc, nullptr);
	const GTEXT_JSON_Value * n = only_number(doc);
	int64_t i64 = 0;
	EXPECT_EQ(gtext_json_get_i64(n, &i64), GTEXT_JSON_OK);
	EXPECT_EQ(i64, 42);
	uint64_t u64 = 0;
	EXPECT_EQ(gtext_json_get_u64(n, &u64), GTEXT_JSON_OK);
	EXPECT_EQ(u64, 42u);
	gtext_json_free(doc);
	gtext_json_error_free(&err);
}

TEST(Json5LeadingPlus, RefusedWhenTheOptionIsOff) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	EXPECT_EQ(opts.allow_leading_plus, false);
	for (const char * src : {"[+1]", "[+1.5]", "[+0]"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

TEST(Json5LeadingPlus, ASignAloneIsStillNotANumber) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_leading_plus = true;
	for (const char * src : {"[+]", "[-]", "[+.]", "[++1]", "[+-1]"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

/* JSON5's grammar puts the sign in front of the whole value, Infinity and NaN
   included. The sign is this option's; the word is allow_nonfinite_numbers'. */
TEST(Json5LeadingPlus, SignedNonfiniteNeedsBothOptions) {
	GTEXT_JSON_Parse_Options plus_only = number_opts();
	plus_only.allow_leading_plus = true;
	EXPECT_NE(dom_status("[+Infinity]", &plus_only), GTEXT_JSON_OK);
	EXPECT_NE(dom_status("[+NaN]", &plus_only), GTEXT_JSON_OK);

	GTEXT_JSON_Parse_Options nonfinite_only = number_opts();
	nonfinite_only.allow_nonfinite_numbers = true;
	EXPECT_NE(dom_status("[+Infinity]", &nonfinite_only), GTEXT_JSON_OK);
	// -Infinity and -NaN need no plus: the sign they carry is a minus.
	EXPECT_EQ(dom_status("[-Infinity]", &nonfinite_only), GTEXT_JSON_OK);
	EXPECT_EQ(dom_status("[-NaN]", &nonfinite_only), GTEXT_JSON_OK);

	GTEXT_JSON_Parse_Options both = number_opts();
	both.allow_leading_plus = true;
	both.allow_nonfinite_numbers = true;
	EXPECT_EQ(dom_status("[+Infinity]", &both), GTEXT_JSON_OK);
	EXPECT_EQ(dom_status("[+NaN]", &both), GTEXT_JSON_OK);
}

TEST(Json5LeadingPlus, SignedInfinityKeepsItsSign) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_leading_plus = true;
	opts.allow_nonfinite_numbers = true;

	struct Case {
		const char * src;
		int positive_inf;
		int negative_inf;
		int nan;
	};
	const Case cases[] = {
	    {"[+Infinity]", 1, 0, 0},
	    {"[-Infinity]", 0, 1, 0},
	    {"[Infinity]", 1, 0, 0},
	    {"[+NaN]", 0, 0, 1},
	    {"[-NaN]", 0, 0, 1},
	    {"[NaN]", 0, 0, 1},
	};
	for (const Case & c : cases) {
		GTEXT_JSON_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * doc =
		    gtext_json_parse(c.src, std::strlen(c.src), &opts, &err);
		ASSERT_NE(doc, nullptr)
		    << c.src << ": " << (err.message ? err.message : "no message");
		double d = 0.0;
		ASSERT_EQ(gtext_json_get_double(only_number(doc), &d), GTEXT_JSON_OK)
		    << c.src;
		if (c.nan) {
			EXPECT_TRUE(std::isnan(d)) << c.src;
		}
		else if (c.positive_inf) {
			EXPECT_TRUE(std::isinf(d)) << c.src;
			EXPECT_GT(d, 0.0) << c.src;
		}
		else {
			EXPECT_TRUE(std::isinf(d)) << c.src;
			EXPECT_LT(d, 0.0) << c.src;
		}
		gtext_json_free(doc);
		gtext_json_error_free(&err);
	}
}

TEST(Json5LeadingPlus, BothParsersAgreeAcrossChunkBoundaries) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_leading_plus = true;
	opts.allow_nonfinite_numbers = true;
	struct Case {
		const char * src;
		bool valid;
	};
	const Case cases[] = {
	    {"[+1]", true},
	    {"[+1.5e2,+2]", true},
	    {"[+Infinity]", true},
	    {"[+NaN]", true},
	    {"[-Infinity]", true},
	    {"[-NaN]", true},
	    {"[-NaNx]", false},
	    {"{\"a\":+7}", true},
	    {"[+]", false},
	    {"[+Infin]", false},
	};
	for (const Case & c : cases) {
		expect_both(c.src, &opts, c.valid);
	}
}

// ===========================================================================
// A decimal point at an edge
// ===========================================================================

TEST(Json5BarePoint, BothEdgesAndTheirValues) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_bare_decimal_point = true;

	struct Case {
		const char * src;
		double dbl;
	};
	const Case cases[] = {
	    {"[.5]", 0.5},
	    {"[5.]", 5.0},
	    {"[.5e2]", 50.0},
	    {"[5.e2]", 500.0},
	    {"[.0]", 0.0},
	    {"[0.]", 0.0},
	    {"[-.5]", -0.5},
	    {"[-5.]", -5.0},
	};
	for (const Case & c : cases) {
		GTEXT_JSON_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * doc =
		    gtext_json_parse(c.src, std::strlen(c.src), &opts, &err);
		ASSERT_NE(doc, nullptr)
		    << c.src << ": " << (err.message ? err.message : "no message");
		double d = 0.0;
		EXPECT_EQ(gtext_json_get_double(only_number(doc), &d), GTEXT_JSON_OK)
		    << c.src;
		EXPECT_EQ(d, c.dbl) << c.src;
		gtext_json_free(doc);
		gtext_json_error_free(&err);
	}
}

/* A document that is nothing but the number, so the token ends at the end of
   the buffer rather than at a bracket. That is a different path in the lexer:
   a number at EOF is where it decides whether more input could still arrive. */
TEST(Json5BarePoint, AtTheEndOfTheDocument) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_bare_decimal_point = true;
	for (const char * src : {".5", "5.", ".5e2"}) {
		EXPECT_EQ(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
	// And with the option off, the same inputs are not numbers.
	GTEXT_JSON_Parse_Options strict = number_opts();
	for (const char * src : {".5", "5."}) {
		EXPECT_NE(dom_status(src, &strict), GTEXT_JSON_OK) << src;
	}
}

TEST(Json5BarePoint, RefusedWhenTheOptionIsOff) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	EXPECT_EQ(opts.allow_bare_decimal_point, false);
	for (const char * src : {"[.5]", "[5.]", "[-.5]", "[.5e2]"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

/* The point may sit at an edge; it may not be the whole number. Digits on
   neither side is not a number under any setting. */
TEST(Json5BarePoint, APointAloneIsNotANumber) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_bare_decimal_point = true;
	for (const char * src : {"[.]", "[.e1]", "[..5]", "[5..]", "[-.]"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

TEST(Json5BarePoint, BothParsersAgreeAcrossChunkBoundaries) {
	GTEXT_JSON_Parse_Options opts = number_opts();
	opts.allow_bare_decimal_point = true;
	struct Case {
		const char * src;
		bool valid;
	};
	const Case cases[] = {
	    {"[.5]", true},
	    {"[5.]", true},
	    {"[.5,5.]", true},
	    {"{\"a\":.5}", true},
	    {"[.]", false},
	    {"[.e1]", false},
	};
	for (const Case & c : cases) {
		expect_both(c.src, &opts, c.valid);
	}
}

// ===========================================================================
// The options are independent
// ===========================================================================

/* Each option admits its own form and nothing else. Turning on hex must not
   quietly bring the plus with it, which is what a single "json5 mode" flag
   inside the lexer would have done. */
TEST(Json5Numbers, EachOptionAdmitsOnlyItsOwnForm) {
	GTEXT_JSON_Parse_Options hex = number_opts();
	hex.allow_hex_numbers = true;
	EXPECT_EQ(dom_status("[0x10]", &hex), GTEXT_JSON_OK);
	EXPECT_NE(dom_status("[+1]", &hex), GTEXT_JSON_OK);
	EXPECT_NE(dom_status("[.5]", &hex), GTEXT_JSON_OK);

	GTEXT_JSON_Parse_Options plus = number_opts();
	plus.allow_leading_plus = true;
	EXPECT_EQ(dom_status("[+1]", &plus), GTEXT_JSON_OK);
	EXPECT_NE(dom_status("[0x10]", &plus), GTEXT_JSON_OK);
	EXPECT_NE(dom_status("[.5]", &plus), GTEXT_JSON_OK);

	GTEXT_JSON_Parse_Options point = number_opts();
	point.allow_bare_decimal_point = true;
	EXPECT_EQ(dom_status("[.5]", &point), GTEXT_JSON_OK);
	EXPECT_NE(dom_status("[0x10]", &point), GTEXT_JSON_OK);
	EXPECT_NE(dom_status("[+1]", &point), GTEXT_JSON_OK);
}

/* And composed: a signed hex literal needs both the sign and the radix. */
TEST(Json5Numbers, PlusAndHexCompose) {
	GTEXT_JSON_Parse_Options both = number_opts();
	both.allow_hex_numbers = true;
	both.allow_leading_plus = true;

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	const char * src = "[+0x10]";
	GTEXT_JSON_Value * doc =
	    gtext_json_parse(src, std::strlen(src), &both, &err);
	ASSERT_NE(doc, nullptr)
	    << (err.message ? err.message : "no message");
	int64_t i64 = 0;
	ASSERT_EQ(gtext_json_get_i64(only_number(doc), &i64), GTEXT_JSON_OK);
	EXPECT_EQ(i64, 16);
	gtext_json_free(doc);
	gtext_json_error_free(&err);

	GTEXT_JSON_Parse_Options hex_only = number_opts();
	hex_only.allow_hex_numbers = true;
	EXPECT_NE(dom_status("[+0x10]", &hex_only), GTEXT_JSON_OK);
}

// ===========================================================================
// ECMAScript string escapes
// ===========================================================================

namespace {

// The decoded bytes of the one string in a single-element array.
std::string only_string(const std::string & src,
    const GTEXT_JSON_Parse_Options * opts, bool * ok) {
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * doc = gtext_json_parse(src.data(), src.size(), opts, &err);
	std::string out;
	*ok = false;
	if (doc) {
		const GTEXT_JSON_Value * v = gtext_json_array_get(doc, 0);
		const char * s = nullptr;
		size_t len = 0;
		if (v && gtext_json_get_string(v, &s, &len) == GTEXT_JSON_OK) {
			out.assign(s, len);
			*ok = true;
		}
		gtext_json_free(doc);
	}
	gtext_json_error_free(&err);
	return out;
}

GTEXT_JSON_Parse_Options escape_opts(void) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_ecma_escapes = true;
	return opts;
}

} // namespace

TEST(Json5Escapes, HexEscapeIsACodepointNotAByte) {
	GTEXT_JSON_Parse_Options opts = escape_opts();
	struct Case {
		const char * src;
		const char * expect;
		size_t expect_len;
	};
	// \xe9 is U+00E9, whose UTF-8 is two bytes. A decoder that wrote the byte
	// 0xE9 would produce a string that is not UTF-8 at all, and the validator
	// would then reject a document JSON5 says is valid.
	const Case cases[] = {
	    {"[\"\\x41\"]", "A", 1},
	    {"[\"\\x7F\"]", "\x7F", 1},
	    {"[\"\\xe9\"]", "\xC3\xA9", 2},
	    {"[\"\\xE9\"]", "\xC3\xA9", 2},
	    {"[\"\\xff\"]", "\xC3\xBF", 2},
	    // The length is given rather than measured: this one decodes to a NUL,
	    // which strlen() would read as an empty string.
	    {"[\"\\x00\"]", "\0", 1},
	};
	for (const Case & c : cases) {
		bool ok = false;
		std::string got = only_string(c.src, &opts, &ok);
		EXPECT_TRUE(ok) << c.src;
		EXPECT_EQ(got, std::string(c.expect, c.expect_len)) << c.src;
	}
}

TEST(Json5Escapes, HexEscapeNeedsTwoDigits) {
	GTEXT_JSON_Parse_Options opts = escape_opts();
	for (const char * src : {"[\"\\x\"]", "[\"\\x4\"]", "[\"\\xg0\"]",
	         "[\"\\x4g\"]"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

TEST(Json5Escapes, VerticalTabAndNul) {
	GTEXT_JSON_Parse_Options opts = escape_opts();
	bool ok = false;
	EXPECT_EQ(only_string("[\"a\\vb\"]", &opts, &ok), std::string("a\vb"));
	EXPECT_TRUE(ok);
	EXPECT_EQ(only_string("[\"a\\0b\"]", &opts, &ok),
	    std::string("a\0b", 3));
	EXPECT_TRUE(ok);
	// The length is what says the NUL is in there, not the terminator.
	EXPECT_EQ(only_string("[\"\\0\"]", &opts, &ok).size(), 1u);
}

/* \0 is a character; \01 would be an octal escape in a language that no longer
   has them, and \1 through \9 never were anything else. */
TEST(Json5Escapes, OctalIsRefused) {
	GTEXT_JSON_Parse_Options opts = escape_opts();
	for (const char * src : {"[\"\\01\"]", "[\"\\09\"]", "[\"\\1\"]",
	         "[\"\\7\"]", "[\"\\9\"]", "[\"\\12\"]"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

TEST(Json5Escapes, AnyOtherCharacterEscapesAsItself) {
	GTEXT_JSON_Parse_Options opts = escape_opts();
	struct Case {
		const char * src;
		const char * expect;
	};
	const Case cases[] = {
	    {"[\"\\a\"]", "a"},
	    {"[\"\\A\"]", "A"},
	    {"[\"\\'\"]", "'"},
	    {"[\"\\ \"]", " "},
	    {"[\"\\%\"]", "%"},
	    // A multi-byte character escapes as itself in full: U+00E9 here.
	    {"[\"\\\xC3\xA9\"]", "\xC3\xA9"},
	    // And the escapes JSON already had keep their meanings.
	    {"[\"\\n\"]", "\n"},
	    {"[\"\\t\"]", "\t"},
	    {"[\"\\\\\"]", "\\"},
	    {"[\"\\/\"]", "/"},
	};
	for (const Case & c : cases) {
		bool ok = false;
		std::string got = only_string(c.src, &opts, &ok);
		EXPECT_TRUE(ok) << c.src;
		EXPECT_EQ(got, std::string(c.expect)) << c.src;
	}
}

TEST(Json5Escapes, RefusedWhenTheOptionIsOff) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	EXPECT_EQ(opts.allow_ecma_escapes, false);
	for (const char * src : {"[\"\\x41\"]", "[\"\\v\"]", "[\"\\0\"]",
	         "[\"\\a\"]", "[\"\\'\"]"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

/* A backslash before a line terminator is a continuation, which is a separate
   option. Without it, the escape is an error rather than quietly meaning a
   newline - which is what the "any other character" rule would have said. */
TEST(Json5Escapes, ALineTerminatorIsNotAnIdentityEscape) {
	GTEXT_JSON_Parse_Options opts = escape_opts();
	EXPECT_EQ(opts.allow_line_continuations, false);
	for (const char * src : {"[\"a\\\nb\"]", "[\"a\\\rb\"]"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

// ===========================================================================
// Line continuations
// ===========================================================================

TEST(Json5LineContinuation, EveryTerminatorSequence) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_line_continuations = true;

	struct Case {
		const char * src;
		const char * expect;
	};
	const Case cases[] = {
	    {"[\"a\\\nb\"]", "ab"},
	    {"[\"a\\\rb\"]", "ab"},
	    // CRLF is one terminator: if only the CR were swallowed, the LF would
	    // be left as an unescaped control character and the parse would fail.
	    {"[\"a\\\r\nb\"]", "ab"},
	    {"[\"a\\\xE2\x80\xA8" "b\"]", "ab"}, // U+2028
	    {"[\"a\\\xE2\x80\xA9" "b\"]", "ab"}, // U+2029
	    {"[\"a\\\n\\\nb\"]", "ab"},
	    {"[\"\\\n\"]", ""},
	};
	for (const Case & c : cases) {
		bool ok = false;
		std::string got = only_string(c.src, &opts, &ok);
		EXPECT_TRUE(ok) << c.src << ": parse failed";
		EXPECT_EQ(got, std::string(c.expect)) << c.src;
	}
}

TEST(Json5LineContinuation, RefusedWhenTheOptionIsOff) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	for (const char * src : {"[\"a\\\nb\"]", "[\"a\\\r\nb\"]"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

/* A raw newline inside a string is still a control character. The continuation
   is the backslash's doing, not the newline's. */
TEST(Json5LineContinuation, ARawNewlineIsStillRefused) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_line_continuations = true;
	EXPECT_NE(dom_status("[\"a\nb\"]", &opts), GTEXT_JSON_OK);
	EXPECT_NE(dom_status("[\"a\rb\"]", &opts), GTEXT_JSON_OK);
}

TEST(Json5Escapes, BothParsersAgreeAcrossChunkBoundaries) {
	GTEXT_JSON_Parse_Options opts = escape_opts();
	opts.allow_line_continuations = true;
	struct Case {
		const char * src;
		bool valid;
	};
	const Case cases[] = {
	    {"[\"\\x41\"]", true},
	    {"[\"a\\vb\"]", true},
	    {"[\"a\\\r\nb\"]", true},
	    {"[\"\\a\"]", true},
	    {"{\"k\":\"\\x41\"}", true},
	    {"[\"\\x4\"]", false},
	    {"[\"\\1\"]", false},
	};
	for (const Case & c : cases) {
		expect_both(c.src, &opts, c.valid);
	}
}

// ===========================================================================
// ECMAScript whitespace between tokens
// ===========================================================================

TEST(Json5Whitespace, EveryCharacterEcmaScriptCallsWhitespace) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_ecma_whitespace = true;

	// Each of these sits between the tokens of [1,2]. The Zs members come from
	// the generated table; the rest are named by ECMAScript itself.
	const char * spaces[] = {
	    "\x0B",             // VT
	    "\x0C",             // FF
	    "\xC2\xA0",         // U+00A0 NO-BREAK SPACE (Zs)
	    "\xE1\x9A\x80",     // U+1680 OGHAM SPACE MARK (Zs)
	    "\xE2\x80\x80",     // U+2000 EN QUAD (Zs)
	    "\xE2\x80\x8A",     // U+200A HAIR SPACE (Zs)
	    "\xE2\x80\xAF",     // U+202F NARROW NO-BREAK SPACE (Zs)
	    "\xE2\x81\x9F",     // U+205F MEDIUM MATHEMATICAL SPACE (Zs)
	    "\xE3\x80\x80",     // U+3000 IDEOGRAPHIC SPACE (Zs)
	    "\xE2\x80\xA8",     // U+2028 LINE SEPARATOR
	    "\xE2\x80\xA9",     // U+2029 PARAGRAPH SEPARATOR
	    "\xEF\xBB\xBF",     // U+FEFF ZERO WIDTH NO-BREAK SPACE
	};
	for (const char * space : spaces) {
		std::string src = std::string("[1,") + space + "2]";
		EXPECT_EQ(dom_status(src, &opts), GTEXT_JSON_OK)
		    << "between elements: " << src;
		std::string around = std::string(space) + "[1,2]" + space;
		EXPECT_EQ(dom_status(around, &opts), GTEXT_JSON_OK)
		    << "around the document";
		std::string in_object =
		    std::string("{\"a\"") + space + ":" + space + "1}";
		EXPECT_EQ(dom_status(in_object, &opts), GTEXT_JSON_OK)
		    << "around a colon";
	}
}

TEST(Json5Whitespace, RefusedWhenTheOptionIsOff) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	EXPECT_EQ(opts.allow_ecma_whitespace, false);
	const char * spaces[] = {"\x0B", "\x0C", "\xC2\xA0", "\xE3\x80\x80",
	    "\xE2\x80\xA8"};
	for (const char * space : spaces) {
		std::string src = std::string("[1,") + space + "2]";
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
	// JSON's own four are unaffected either way.
	for (const char * space : {" ", "\t", "\r", "\n"}) {
		std::string src = std::string("[1,") + space + "2]";
		EXPECT_EQ(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

/* A character the table does not name is not whitespace, however space-like it
   looks. U+200B ZERO WIDTH SPACE is Cf, not Zs, and ECMAScript does not name
   it; U+180E was Zs until Unicode 6.3 moved it to Cf. */
TEST(Json5Whitespace, NotEverySpaceLikeCharacterCounts) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_ecma_whitespace = true;
	const char * not_spaces[] = {
	    "\xE2\x80\x8B", // U+200B ZERO WIDTH SPACE (Cf)
	    "\xE1\xA0\x8E", // U+180E MONGOLIAN VOWEL SEPARATOR (Cf since 6.3)
	    "\xC2\xAD",     // U+00AD SOFT HYPHEN (Cf)
	};
	for (const char * c : not_spaces) {
		std::string src = std::string("[1,") + c + "2]";
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

/* A three-byte space split between feeds. The skipper sees a truncated
   sequence, which is indistinguishable from "not whitespace" without more
   input, so the streaming parser has to wait rather than refuse. */
TEST(Json5Whitespace, MultiByteSpaceSplitBetweenFeeds) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_ecma_whitespace = true;
	const std::string src = "[1,\xE3\x80\x80" "2]"; // U+3000 between tokens
	for (size_t chunk = 1; chunk <= src.size(); chunk++) {
		EXPECT_TRUE(stream_accepts(src, &opts, chunk))
		    << "chunk size " << chunk;
	}
}

TEST(Json5Whitespace, BothParsersAgreeAcrossChunkBoundaries) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_ecma_whitespace = true;
	struct Case {
		const char * src;
		bool valid;
	};
	const Case cases[] = {
	    {"[1,\xC2\xA0" "2]", true},
	    {"\xEF\xBB\xBF" "[1,2]", true},
	    {"{\"a\"\xE2\x80\xA8:1}", true},
	    {"[1,\xE2\x80\x8B" "2]", false},
	    {"[1,\xC2" "2]", false}, // A truncated sequence is not whitespace.
	};
	for (const Case & c : cases) {
		expect_both(c.src, &opts, c.valid);
	}
}

// ===========================================================================
// Unquoted object names
// ===========================================================================

namespace {

GTEXT_JSON_Parse_Options key_opts(void) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_unquoted_keys = true;
	return opts;
}

// The names of a parsed object, in order.
std::vector<std::string> names_of(
    const std::string & src, const GTEXT_JSON_Parse_Options * opts, bool * ok) {
	std::vector<std::string> out;
	*ok = false;
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * doc = gtext_json_parse(src.data(), src.size(), opts, &err);
	if (doc) {
		*ok = true;
		size_t n = gtext_json_object_size(doc);
		for (size_t i = 0; i < n; i++) {
			size_t len = 0;
			const char * k = gtext_json_object_key(doc, i, &len);
			out.push_back(std::string(k ? k : "", len));
		}
		gtext_json_free(doc);
	}
	gtext_json_error_free(&err);
	return out;
}

// The KEY events the streaming parser emits, in order.
std::vector<std::string> stream_names(const std::string & src,
    const GTEXT_JSON_Parse_Options * opts, size_t chunk, bool * ok) {
	static std::vector<std::string> collected;
	collected.clear();
	GTEXT_JSON_Event_cb cb = [](void *, const GTEXT_JSON_Event * ev,
	                             GTEXT_JSON_Error *) {
		if (ev->type == GTEXT_JSON_EVT_KEY) {
			collected.push_back(std::string(ev->as.str.s, ev->as.str.len));
		}
		return GTEXT_JSON_OK;
	};
	GTEXT_JSON_Stream * st = gtext_json_stream_new(opts, cb, nullptr);
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Status status = GTEXT_JSON_OK;
	for (size_t i = 0; i < src.size(); i += chunk) {
		size_t n = std::min(chunk, src.size() - i);
		status = gtext_json_stream_feed(st, src.data() + i, n, &err);
		if (status != GTEXT_JSON_OK) {
			break;
		}
	}
	if (status == GTEXT_JSON_OK) {
		status = gtext_json_stream_finish(st, &err);
	}
	gtext_json_stream_free(st);
	gtext_json_error_free(&err);
	*ok = status == GTEXT_JSON_OK;
	return collected;
}

} // namespace

TEST(Json5UnquotedKeys, WhatMayBeAName) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	struct Case {
		const char * src;
		const char * name;
	};
	const Case cases[] = {
	    {"{a:1}", "a"},
	    {"{abc:1}", "abc"},
	    {"{a1:1}", "a1"},
	    {"{$:1}", "$"},          // ECMAScript's own addition
	    {"{_:1}", "_"},          // Pc, so ID_Continue but not ID_Start
	    {"{$x_1:1}", "$x_1"},
	    {"{ a : 1 }", "a"},
	    // ID_Start is not ASCII: U+00E9, and a Greek letter.
	    {"{\xC3\xA9:1}", "\xC3\xA9"},
	    {"{caf\xC3\xA9:1}", "caf\xC3\xA9"},
	    {"{\xCE\xB1:1}", "\xCE\xB1"},
	    // An astral ID_Start: U+10400 DESERET CAPITAL LETTER LONG I.
	    {"{\xF0\x90\x90\x80:1}", "\xF0\x90\x90\x80"},
	};
	for (const Case & c : cases) {
		bool ok = false;
		std::vector<std::string> names = names_of(c.src, &opts, &ok);
		ASSERT_TRUE(ok) << c.src;
		ASSERT_EQ(names.size(), 1u) << c.src;
		EXPECT_EQ(names[0], std::string(c.name)) << c.src;
	}
}

/* A \uXXXX escape is a spelling of a character, so it may appear in a name and
   is decoded before the name is used. A surrogate pair reaches an astral
   character, as it does in ECMAScript 5.1. */
TEST(Json5UnquotedKeys, EscapesInNames) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	struct Case {
		const char * src;
		const char * name;
	};
	const Case cases[] = {
	    {"{\\u0061:1}", "a"},
	    {"{\\u0061bc:1}", "abc"},
	    {"{a\\u0062c:1}", "abc"},
	    {"{\\u00e9:1}", "\xC3\xA9"},
	    {"{\\uD801\\uDC00:1}", "\xF0\x90\x90\x80"}, // U+10400
	};
	for (const Case & c : cases) {
		bool ok = false;
		std::vector<std::string> names = names_of(c.src, &opts, &ok);
		ASSERT_TRUE(ok) << c.src;
		ASSERT_EQ(names.size(), 1u) << c.src;
		EXPECT_EQ(names[0], std::string(c.name)) << c.src;
	}
}

/* An escape naming a character that may not be in a name is not a name, and
   neither is half a surrogate pair. \u{1F600} is ECMAScript 2015's spelling and
   the JSON5 specification is written against 5.1. */
TEST(Json5UnquotedKeys, RefusedEscapes) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	for (const char * src : {
	         "{\\u0020:1}",          // space
	         "{\\u002D:1}",          // hyphen-minus
	         "{a\\u0020b:1}",        // space in the middle
	         "{\\uD801:1}",          // lone high surrogate
	         "{\\uDC00:1}",          // lone low surrogate
	         "{\\uD801\\u0061:1}",   // high surrogate, then not a low one
	         "{\\u{1F600}:1}",       // ES2015 spelling
	         "{\\u00g1:1}",          // not hex
	         "{\\x41:1}",            // not a unicode escape
	         "{\\u006:1}",           // too few digits
	     }) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

/* An emoji is not an identifier: U+1F600 has neither ID_Start nor
   ID_Continue, so the table refuses it where it refuses a space. This is the
   case that separates "any character above ASCII" from the real rule. */
TEST(Json5UnquotedKeys, NotEveryNonAsciiCharacterIsAName) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	for (const char * src : {
	         "{\xF0\x9F\x98\x80:1}",     // U+1F600 GRINNING FACE
	         "{\\uD83D\\uDE00:1}",       // the same, escaped
	         "{\xE2\x82\xAC:1}",         // U+20AC EURO SIGN (Sc, like $, but
	                                     // ECMAScript names only $)
	         "{\xC2\xB1:1}",             // U+00B1 PLUS-MINUS SIGN (Sm)
	     }) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
	// A combining mark may continue a name but not start one: U+0301 is Mn,
	// so ID_Continue without ID_Start.
	EXPECT_EQ(dom_status("{a\xCC\x81:1}", &opts), GTEXT_JSON_OK);
	EXPECT_NE(dom_status("{\xCC\x81:1}", &opts), GTEXT_JSON_OK);
}

/* IdentifierName includes the reserved words, so these are names and not
   values. The lexer reads them as keyword tokens, which carry no text, so the
   parsers recover the spelling from the token type. */
TEST(Json5UnquotedKeys, AKeywordIsAName) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	struct Case {
		const char * src;
		const char * name;
	};
	const Case cases[] = {
	    {"{true:1}", "true"},
	    {"{false:1}", "false"},
	    {"{null:1}", "null"},
	    {"{NaN:1}", "NaN"},
	    {"{Infinity:1}", "Infinity"},
	};
	for (const Case & c : cases) {
		bool ok = false;
		std::vector<std::string> names = names_of(c.src, &opts, &ok);
		ASSERT_TRUE(ok) << c.src;
		ASSERT_EQ(names.size(), 1u) << c.src;
		EXPECT_EQ(names[0], std::string(c.name)) << c.src;
	}
	// And they are still values where a value belongs.
	EXPECT_EQ(dom_status("[true,false,null]", &opts), GTEXT_JSON_OK);
	// NaN and Infinity as names do not need allow_nonfinite_numbers, which is
	// about values. As values they still do.
	EXPECT_EQ(opts.allow_nonfinite_numbers, false);
	EXPECT_NE(dom_status("[NaN]", &opts), GTEXT_JSON_OK);
	// -Infinity is not an identifier: a name cannot start with a sign.
	EXPECT_NE(dom_status("{-Infinity:1}", &opts), GTEXT_JSON_OK);
}

TEST(Json5UnquotedKeys, RefusedWhenTheOptionIsOff) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	EXPECT_EQ(opts.allow_unquoted_keys, false);
	for (const char * src : {"{a:1}", "{true:1}", "{$:1}", "{\\u0061:1}"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
	// Quoted names are unaffected.
	EXPECT_EQ(dom_status("{\"a\":1}", &opts), GTEXT_JSON_OK);
}

/* A name is an object name and nothing else: an identifier where a value
   belongs is still an error. */
TEST(Json5UnquotedKeys, NotAValue) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	for (const char * src : {"[a]", "{a:b}", "a", "[1,abc]", "{a:1,b:c}"}) {
		EXPECT_NE(dom_status(src, &opts), GTEXT_JSON_OK) << src;
	}
}

/* The name is decoded before it is compared, so a quoted and an unquoted
   spelling of one name are one name. */
TEST(Json5UnquotedKeys, DuplicateDetectionComparesDecodedNames) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	EXPECT_NE(dom_status("{a:1,\"a\":2}", &opts), GTEXT_JSON_OK);
	EXPECT_NE(dom_status("{a:1,\\u0061:2}", &opts), GTEXT_JSON_OK);
	EXPECT_NE(dom_status("{\"true\":1,true:2}", &opts), GTEXT_JSON_OK);
	// Different names are still different.
	EXPECT_EQ(dom_status("{a:1,b:2}", &opts), GTEXT_JSON_OK);
}

/* With normalize_unicode a name arrives normalized however it was written, or
   the two spellings of one name would be two names when one of them is
   unquoted. */
TEST(Json5UnquotedKeys, NormalizationAppliesToUnquotedNames) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	opts.normalize_unicode = true;

	// "cafe" + U+0301 unquoted, against the composed form quoted.
	bool ok = false;
	std::vector<std::string> names =
	    names_of("{cafe\xCC\x81:1}", &opts, &ok);
	ASSERT_TRUE(ok);
	ASSERT_EQ(names.size(), 1u);
	EXPECT_EQ(names[0], std::string("caf\xC3\xA9")) << "should be composed";

	// And so the two spellings collide, as they do when both are quoted.
	EXPECT_NE(dom_status("{\"caf\xC3\xA9\":1,cafe\xCC\x81:2}", &opts),
	    GTEXT_JSON_OK);
}

TEST(Json5UnquotedKeys, TheStreamingParserEmitsTheSameNames) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	const char * sources[] = {
	    "{alpha:1,beta:2}",
	    "{\\u0061bc:1}",
	    "{true:1,null:2}",
	    "{caf\xC3\xA9:1}",
	    "{a:{b:{c:1}}}",
	};
	for (const char * src : sources) {
		bool dom_ok = false;
		std::vector<std::string> expected = names_of(src, &opts, &dom_ok);
		ASSERT_TRUE(dom_ok) << src;
		// Every chunk size, because a name that reaches the end of a chunk is
		// unfinished rather than complete and the lexer has to say so.
		for (size_t chunk = 1; chunk <= std::strlen(src); chunk++) {
			bool ok = false;
			std::vector<std::string> got = stream_names(src, &opts, chunk, &ok);
			EXPECT_TRUE(ok) << src << " at chunk " << chunk;
			// The DOM names are only the top level; compare the first.
			ASSERT_FALSE(got.empty()) << src << " at chunk " << chunk;
			EXPECT_EQ(got[0], expected[0]) << src << " at chunk " << chunk;
		}
	}
}

/* The default policy is ERROR, and both parsers refuse. A name is compared
   after escapes are decoded, quoted or not. One byte at a time, because the
   name the stream compares is a token the lexer may have finished in an
   earlier chunk. */
GTEXT_JSON_Status stream_status(const std::string & src,
    const GTEXT_JSON_Parse_Options * opts, size_t chunk) {
	GTEXT_JSON_Event_cb cb = [](void *, const GTEXT_JSON_Event *,
	                             GTEXT_JSON_Error *) { return GTEXT_JSON_OK; };
	GTEXT_JSON_Stream * st = gtext_json_stream_new(opts, cb, nullptr);
	if (!st) {
		return GTEXT_JSON_E_OOM;
	}
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Status status = GTEXT_JSON_OK;
	for (size_t i = 0; i < src.size(); i += chunk) {
		size_t n = std::min(chunk, src.size() - i);
		status = gtext_json_stream_feed(st, src.data() + i, n, &err);
		if (status != GTEXT_JSON_OK) {
			break;
		}
	}
	if (status == GTEXT_JSON_OK) {
		status = gtext_json_stream_finish(st, &err);
	}
	if (status != GTEXT_JSON_OK) {
		status = err.code;
	}
	gtext_json_stream_free(st);
	gtext_json_error_free(&err);
	return status;
}

TEST(Json5UnquotedKeys, BothParsersRefuseARepeatedName) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	EXPECT_EQ(opts.dupkeys, GTEXT_JSON_DUPKEY_ERROR);
	for (const char * src : {"{\"a\":1,\"a\":2}", "{a:1,\"a\":2}", "{a:1,a:2}",
	         "{\"a\":1,\"\\u0061\":2}", "{\"a\":{\"b\":1,\"b\":2}}"}) {
		EXPECT_EQ(dom_status(src, &opts), GTEXT_JSON_E_DUPKEY) << src;
		for (size_t chunk : {std::strlen(src), size_t(1)}) {
			EXPECT_EQ(stream_status(src, &opts, chunk), GTEXT_JSON_E_DUPKEY)
			    << src << " at chunk " << chunk;
		}
	}
}

/* FIRST_WINS keeps the first member and does not deliver the later one. The
   later value is still parsed: a broken one is an error, and so is a repeated
   name inside a value that is kept. LAST_WINS and COLLECT still deliver every
   member, because the callback has already been handed the earlier value and
   a stream cannot take it back. */
TEST(Json5UnquotedKeys, FirstWinsDoesNotDeliverTheLaterName) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	opts.dupkeys = GTEXT_JSON_DUPKEY_FIRST_WINS;
	const char * src = "{\"a\":1,\"a\":2,\"b\":3}";
	bool ok = false;
	std::vector<std::string> names = stream_names(src, &opts, 1, &ok);
	EXPECT_TRUE(ok);
	ASSERT_EQ(names.size(), 2u);
	EXPECT_EQ(names[0], "a");
	EXPECT_EQ(names[1], "b");
	EXPECT_EQ(dom_status(src, &opts), GTEXT_JSON_OK);

	EXPECT_EQ(stream_status("{\"a\":1,\"a\":[}", &opts, 1), GTEXT_JSON_E_BAD_TOKEN);
	EXPECT_EQ(dom_status("{\"a\":1,\"a\":[}", &opts), GTEXT_JSON_E_BAD_TOKEN);

	opts.dupkeys = GTEXT_JSON_DUPKEY_LAST_WINS;
	names = stream_names("{\"a\":1,\"a\":2}", &opts, 1, &ok);
	EXPECT_TRUE(ok);
	ASSERT_EQ(names.size(), 2u);
	EXPECT_EQ(names[0], "a");
	EXPECT_EQ(names[1], "a");
}

TEST(Json5UnquotedKeys, BothParsersAgreeAcrossChunkBoundaries) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	struct Case {
		const char * src;
		bool valid;
	};
	const Case cases[] = {
	    {"{a:1}", true},
	    {"{a:1,b:[2,{c:3}]}", true},
	    {"{true:1}", true},
	    {"{\\u0061:1}", true},
	    {"{1a:1}", false},
	    {"{a b:1}", false},
	    {"[a]", false},
	};
	for (const Case & c : cases) {
		expect_both(c.src, &opts, c.valid);
	}
}

/**
 * A signed nonfinite is not a name, and the sign is not silently dropped.
 *
 * Found by the fuzz soak as `{+Infinity:5}`: **accepted, as an object whose name
 * was `Infinity`** - a key one byte shorter than the document spelled, which is a
 * silently wrong answer rather than a refusal. An unquoted name is an ECMAScript
 * IdentifierName and `+` has neither ID_Start nor ID_Continue, so all four signed
 * spellings must be refused.
 *
 * The cause was that the *name* was recovered from the token type, and only
 * `-Infinity` has a type of its own: `+Infinity` is handed JSON_TOKEN_INFINITY and
 * `+NaN` and `-NaN` are handed JSON_TOKEN_NAN, so three of the four arrived wearing
 * the bare word's type and were answered with the bare word's text. The check that
 * excluded `-Infinity` was written about the sign that happened to be
 * distinguishable.
 *
 * The bare words stay valid - IdentifierName includes the reserved words - and are
 * asserted here beside the signed ones, because a fix that refused both would pass
 * a test that only listed the refusals.
 */
TEST(Json5UnquotedKeys, ASignedNonfiniteIsNotAName) {
	GTEXT_JSON_Parse_Options opts = key_opts();
	opts.allow_nonfinite_numbers = true;
	opts.allow_leading_plus = true;

	struct Case {
		const char * src;
		bool valid;
	};
	const Case cases[] = {
	    /* The bare words are names. */
	    {"{Infinity:1}", true},
	    {"{NaN:1}", true},
	    {"{Infinity:1,NaN:2}", true},
	    /* Signed, in any spelling, is not. */
	    {"{+Infinity:1}", false},
	    {"{-Infinity:1}", false},
	    {"{+NaN:1}", false},
	    {"{-NaN:1}", false},
	    /* Whitespace around the colon changed which branch was reached, so both. */
	    {"{+Infinity : 1}", false},
	    {"{ +Infinity:1}", false},
	    {"{-NaN : 1}", false},
	    /* A sign is not a name character in front of anything else either. */
	    {"{+a:1}", false},
	    {"{-a:1}", false},
	    /* And the words are still values where a value is expected. */
	    {"[+Infinity]", true},
	    {"[-Infinity]", true},
	    {"[+NaN]", true},
	};
	for (const Case & c : cases) {
		expect_both(c.src, &opts, c.valid);
	}

	/* The name that comes back is the one the document spelled. A test that only
	   checked the verdict would pass with the sign still being dropped from some
	   other spelling that was accepted. */
	bool ok = false;
	std::vector<std::string> names = names_of("{Infinity:1,NaN:2}", &opts, &ok);
	EXPECT_TRUE(ok);
	ASSERT_EQ(names.size(), 2u);
	EXPECT_EQ(names[0], "Infinity");
	EXPECT_EQ(names[1], "NaN");
}

// ===========================================================================
// The dialect as a whole
// ===========================================================================

/* The preset turns on exactly the options JSON5 names, and nothing else. Field
   by field, because "it works on a sample document" would pass just as well
   with one of them missing - or with allow_unescaped_controls quietly added. */
TEST(Json5Preset, TurnsOnTheDialectAndNothingElse) {
	GTEXT_JSON_Parse_Options json5 = gtext_json_parse_options_json5();
	GTEXT_JSON_Parse_Options strict = gtext_json_parse_options_default();

	EXPECT_TRUE(json5.allow_comments);
	EXPECT_TRUE(json5.allow_trailing_commas);
	EXPECT_TRUE(json5.allow_single_quotes);
	EXPECT_TRUE(json5.allow_nonfinite_numbers);
	EXPECT_TRUE(json5.allow_hex_numbers);
	EXPECT_TRUE(json5.allow_leading_plus);
	EXPECT_TRUE(json5.allow_bare_decimal_point);
	EXPECT_TRUE(json5.allow_ecma_escapes);
	EXPECT_TRUE(json5.allow_line_continuations);
	EXPECT_TRUE(json5.allow_ecma_whitespace);
	EXPECT_TRUE(json5.allow_unquoted_keys);

	// JSON5 permits a raw control character in a string no more than JSON does,
	// and says nothing about normalization.
	EXPECT_FALSE(json5.allow_unescaped_controls);
	EXPECT_FALSE(json5.normalize_unicode);

	// And the rest is the default's, not a second set of choices.
	EXPECT_EQ(json5.validate_utf8, strict.validate_utf8);
	EXPECT_EQ(json5.allow_leading_bom, strict.allow_leading_bom);
	EXPECT_EQ(json5.dupkeys, strict.dupkeys);
	EXPECT_EQ(json5.max_depth, strict.max_depth);
	EXPECT_EQ(json5.max_string_bytes, strict.max_string_bytes);
	EXPECT_EQ(json5.max_container_elems, strict.max_container_elems);
	EXPECT_EQ(json5.max_total_bytes, strict.max_total_bytes);
	EXPECT_EQ(json5.preserve_number_lexeme, strict.preserve_number_lexeme);
	EXPECT_EQ(json5.parse_int64, strict.parse_int64);
	EXPECT_EQ(json5.parse_uint64, strict.parse_uint64);
	EXPECT_EQ(json5.parse_double, strict.parse_double);
	EXPECT_EQ(json5.in_situ_mode, strict.in_situ_mode);
	EXPECT_EQ(json5.allocator, strict.allocator);
}

/* The example from json5.org's front page, which uses nearly every difference
   at once. Both parsers, and the streaming one at every chunk size, because
   this is where the features meet each other. */
TEST(Json5Preset, TheDocumentFromTheSpecification) {
	const std::string src =
	    "{\n"
	    "  // comments\n"
	    "  unquoted: 'and you can quote me on that',\n"
	    "  singleQuotes: 'I can use \"double quotes\" here',\n"
	    "  lineBreaks: \"Look, Mom! \\\n"
	    "No \\\\n's!\",\n"
	    "  hexadecimal: 0xdecaf,\n"
	    "  leadingDecimalPoint: .8675309, andTrailing: 8675309.,\n"
	    "  positiveSign: +1,\n"
	    "  trailingComma: 'in objects', andIn: ['arrays',],\n"
	    "  \"backwardsCompatible\": \"with JSON\",\n"
	    "}";

	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_json5();
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * doc =
	    gtext_json_parse(src.data(), src.size(), &opts, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "no message");
	gtext_json_error_free(&err);

	EXPECT_EQ(gtext_json_object_size(doc), 10u);

	const GTEXT_JSON_Value * hex = gtext_json_object_get(doc, "hexadecimal", 11);
	ASSERT_NE(hex, nullptr);
	int64_t i64 = 0;
	ASSERT_EQ(gtext_json_get_i64(hex, &i64), GTEXT_JSON_OK);
	EXPECT_EQ(i64, 0xdecaf);

	const GTEXT_JSON_Value * lead =
	    gtext_json_object_get(doc, "leadingDecimalPoint", 19);
	ASSERT_NE(lead, nullptr);
	double d = 0.0;
	ASSERT_EQ(gtext_json_get_double(lead, &d), GTEXT_JSON_OK);
	EXPECT_DOUBLE_EQ(d, .8675309);

	const GTEXT_JSON_Value * breaks = gtext_json_object_get(doc, "lineBreaks", 10);
	ASSERT_NE(breaks, nullptr);
	const char * s = nullptr;
	size_t len = 0;
	ASSERT_EQ(gtext_json_get_string(breaks, &s, &len), GTEXT_JSON_OK);
	// The continuation contributes nothing, and the \\n stays two characters.
	EXPECT_EQ(std::string(s, len), "Look, Mom! No \\n's!");

	gtext_json_free(doc);

	// And strict JSON refuses it, which is what says the preset is doing the
	// work rather than the parser having quietly relaxed.
	GTEXT_JSON_Parse_Options strict = gtext_json_parse_options_default();
	EXPECT_NE(dom_status(src, &strict), GTEXT_JSON_OK);

	// Every chunk size, because this document has a comment, a continuation and
	// a split-able name in it, and each of those is carried across a feed
	// boundary by different machinery.
	for (size_t chunk = 1; chunk <= src.size(); chunk++) {
		EXPECT_TRUE(stream_accepts(src, &opts, chunk))
		    << "streaming parser at chunk " << chunk;
	}
}

/* Valid JSON is valid JSON5, so the preset must accept everything the default
   does. A handful of documents that exercise each value type. */
TEST(Json5Preset, EveryJsonDocumentIsAJson5Document) {
	GTEXT_JSON_Parse_Options json5 = gtext_json_parse_options_json5();
	GTEXT_JSON_Parse_Options strict = gtext_json_parse_options_default();
	const char * documents[] = {
	    "{}",
	    "[]",
	    "null",
	    "true",
	    "0",
	    "-1.5e10",
	    "\"a string\"",
	    "{\"a\":[1,2,{\"b\":null}],\"c\":true}",
	    "[\"\\u00e9\",\"\\ud83d\\ude00\",\"\\t\"]",
	    "{\"nested\":{\"deeply\":{\"enough\":[[[]]]}}}",
	};
	for (const char * src : documents) {
		EXPECT_EQ(dom_status(src, &strict), GTEXT_JSON_OK) << "strict: " << src;
		EXPECT_EQ(dom_status(src, &json5), GTEXT_JSON_OK) << "json5: " << src;
	}
}
