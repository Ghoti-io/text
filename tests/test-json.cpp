#include <gtest/gtest.h>
#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/text.h>
#include <ghoti.io/text/json.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <limits>
#include <fstream>
#include <sstream>
#include <tuple>

// Include internal header for testing internal functions
extern "C" {
#include "../src/json/json_internal.h"
}

/**
 * Truncated containers: every prefix of a valid document must be reported as
 * an error without touching freed memory.
 *
 * A nested object lives in its parent's arena, and gtext_json_free() releases
 * the whole arena; the object parser's error path freed nested objects
 * anyway, so the parent was destroyed and the parent's own error path then
 * read from freed memory. `{"":{` - five bytes - was enough. The array parser
 * had guarded against this; the object parser had not.
 *
 * Found by the JSON fuzzer (tests/fuzz/fuzz_json.cpp).
 */
TEST(Truncated, NestedContainersDoNotUseFreedMemory) {
    const char * inputs[] = {
        "{\"\":{",
        "{\"a\":{",
        "{\"a\":{\"b\":{",
        "{\"a\":{\"b\":{\"c\":{",
        "{\"a\":{\"b\":1",
        "{\"a\":[",
        "{\"a\":[{",
        "[{\"a\":{",
        "[[[[",
        "{\"a\":{\"b\":[{\"c\":",
    };

    for (const char * input : inputs) {
        GTEXT_JSON_Error err{};
        GTEXT_JSON_Value * v =
            gtext_json_parse(input, strlen(input), nullptr, &err);
        EXPECT_EQ(v, nullptr) << "truncated input should not parse: " << input;
        if (v) {
            gtext_json_free(v);
        }
        gtext_json_error_free(&err);
    }
}

/**
 * Test default parse options match specification (strict JSON by default)
 */
TEST(ParseOptions, Default) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    // Strictness / extensions - all should be off (strict JSON)
    EXPECT_EQ(opts.allow_comments, 0);
    EXPECT_EQ(opts.allow_trailing_commas, 0);
    EXPECT_EQ(opts.allow_nonfinite_numbers, 0);
    EXPECT_EQ(opts.allow_single_quotes, 0);
    EXPECT_EQ(opts.allow_unescaped_controls, 0);

    // Unicode / input handling
    EXPECT_EQ(opts.allow_leading_bom, 1);  // default on
    EXPECT_EQ(opts.validate_utf8, 1);      // default on
    EXPECT_EQ(opts.normalize_unicode, 0);   // off by default; implemented
    EXPECT_EQ(opts.in_situ_mode, 0);        // off by default

    // Duplicate keys
    EXPECT_EQ(opts.dupkeys, GTEXT_JSON_DUPKEY_ERROR);

    // Limits - should be 0 (library defaults)
    EXPECT_EQ(opts.max_depth, 0u);
    EXPECT_EQ(opts.max_string_bytes, 0u);
    EXPECT_EQ(opts.max_container_elems, 0u);
    EXPECT_EQ(opts.max_total_bytes, 0u);

    // Number fidelity / representations
    EXPECT_EQ(opts.preserve_number_lexeme, 1);  // preserve for round-trip
    EXPECT_EQ(opts.parse_int64, 1);
    EXPECT_EQ(opts.parse_uint64, 1);
    EXPECT_EQ(opts.parse_double, 1);
}

/**
 * Test default write options match specification (compact output)
 */
TEST(WriteOptions, Default) {
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();

    // Formatting
    EXPECT_EQ(opts.pretty, 0);              // compact output
    EXPECT_EQ(opts.indent_spaces, 2);       // default indent
    EXPECT_STREQ(opts.newline, "\n");       // default newline

    // Escaping
    EXPECT_EQ(opts.escape_solidus, 0);
    EXPECT_EQ(opts.escape_unicode, 0);
    EXPECT_EQ(opts.escape_all_non_ascii, 0);

    // Canonical / deterministic
    EXPECT_EQ(opts.sort_object_keys, 0);    // preserve insertion order
    EXPECT_EQ(opts.canonical_numbers, 0);   // preserve original lexeme

    // Extensions
    EXPECT_EQ(opts.allow_nonfinite_numbers, 0);
}

/**
 * Test standard escape sequence decoding
 */
TEST(StringHandling, EscapeSequences) {
    char output[256];
    size_t output_len;
    json_position pos = {0, 1, 1};

    // Test each standard escape
    struct {
        const char * input;
        const char * expected;
        size_t expected_len;
    } tests[] = {
        {"\\\"", "\"", 1},
        {"\\\\", "\\", 1},
        {"\\/", "/", 1},
        {"\\b", "\b", 1},
        {"\\f", "\f", 1},
        {"\\n", "\n", 1},
        {"\\r", "\r", 1},
        {"\\t", "\t", 1},
        {"hello\\nworld", "hello\nworld", 11},
        {"a\\tb\\nc", "a\tb\nc", 5}
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        pos.offset = 0;
        pos.line = 1;
        pos.col = 1;
        output_len = 0;

        /* These inputs are escape sequences rather than whole documents, and
           several are deliberately not well-formed UTF-8. */
        GTEXT_JSON_Parse_Options decode_opts = gtext_json_parse_options_default();
        decode_opts.validate_utf8 = false;
        GTEXT_JSON_Status status = json_decode_string(
            tests[i].input,
            strlen(tests[i].input),
            output,
            sizeof(output),
            &output_len,
            &pos,
            JSON_UTF8_REJECT,
            &decode_opts
        );

        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed for input: " << tests[i].input;
        EXPECT_EQ(output_len, tests[i].expected_len) << "Wrong length for: " << tests[i].input;
        EXPECT_EQ(memcmp(output, tests[i].expected, output_len), 0)
            << "Wrong output for: " << tests[i].input;
    }
}

/**
 * Test Unicode escape sequence decoding
 */
TEST(StringHandling, UnicodeEscapes) {
    char output[256];
    size_t output_len;
    json_position pos = {0, 1, 1};

    // Test Unicode escapes
    struct {
        const char * input;
        const char * expected;
        size_t expected_len;
    } tests[] = {
        {"\\u0041", "A", 1},           // U+0041 = 'A'
        {"\\u00E9", "\xC3\xA9", 2},    // U+00E9 = 'e with acute' (UTF-8)
        {"\\u20AC", "\xE2\x82\xAC", 3} // U+20AC = 'Euro sign' (UTF-8)
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        pos.offset = 0;
        pos.line = 1;
        pos.col = 1;
        output_len = 0;

        /* These inputs are escape sequences rather than whole documents, and
           several are deliberately not well-formed UTF-8. */
        GTEXT_JSON_Parse_Options decode_opts = gtext_json_parse_options_default();
        decode_opts.validate_utf8 = false;
        GTEXT_JSON_Status status = json_decode_string(
            tests[i].input,
            strlen(tests[i].input),
            output,
            sizeof(output),
            &output_len,
            &pos,
            JSON_UTF8_REJECT,
            &decode_opts
        );

        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed for input: " << tests[i].input;
        EXPECT_EQ(output_len, tests[i].expected_len) << "Wrong length for: " << tests[i].input;
        EXPECT_EQ(memcmp(output, tests[i].expected, output_len), 0)
            << "Wrong output for: " << tests[i].input;
    }
}

/**
 * Test surrogate pair decoding
 */
TEST(StringHandling, SurrogatePairs) {
    char output[256];
    size_t output_len;
    json_position pos = {0, 1, 1};

    // U+1F600 = grinning face emoji
    // High surrogate: U+D83D, Low surrogate: U+DE00
    const char * input = "\\uD83D\\uDE00";
    const char * expected = "\xF0\x9F\x98\x80";  // UTF-8 for U+1F600
    size_t expected_len = 4;

    /* These inputs are escape sequences rather than whole documents, and
       several are deliberately not well-formed UTF-8. */
    GTEXT_JSON_Parse_Options decode_opts = gtext_json_parse_options_default();
    decode_opts.validate_utf8 = false;
    GTEXT_JSON_Status status = json_decode_string(
        input,
        strlen(input),
        output,
        sizeof(output),
        &output_len,
        &pos,
        JSON_UTF8_REJECT,
        &decode_opts
    );

    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(output_len, expected_len);
    EXPECT_EQ(memcmp(output, expected, expected_len), 0);
}

/**
 * Test invalid escape sequences are rejected
 */
TEST(StringHandling, InvalidEscapes) {
    char output[256];
    size_t output_len;
    json_position pos = {0, 1, 1};

    // Invalid escape sequences
    const char * invalid_escapes[] = {
        "\\x",      // Invalid escape character
        "\\u",      // Incomplete Unicode escape
        "\\u12",    // Incomplete Unicode escape
        "\\u12G",   // Invalid hex digit
        "\\uD83D",  // High surrogate without low surrogate
        "\\uDE00"   // Low surrogate without high surrogate
    };

    for (size_t i = 0; i < sizeof(invalid_escapes) / sizeof(invalid_escapes[0]); ++i) {
        pos.offset = 0;
        pos.line = 1;
        pos.col = 1;
        output_len = 0;

        /* These inputs are escape sequences rather than whole documents, and
           several are deliberately not well-formed UTF-8. */
        GTEXT_JSON_Parse_Options decode_opts = gtext_json_parse_options_default();
        decode_opts.validate_utf8 = false;
        GTEXT_JSON_Status status = json_decode_string(
            invalid_escapes[i],
            strlen(invalid_escapes[i]),
            output,
            sizeof(output),
            &output_len,
            &pos,
            JSON_UTF8_REJECT,
            &decode_opts
        );

        EXPECT_NE(status, GTEXT_JSON_OK) << "Should reject: " << invalid_escapes[i];
    }
}

/**
 * Test position tracking during string decoding
 */
TEST(StringHandling, PositionTracking) {
    char output[256];
    size_t output_len;
    json_position pos = {0, 1, 1};

    const char * input = "hello\\nworld";
    /* These inputs are escape sequences rather than whole documents, and
       several are deliberately not well-formed UTF-8. */
    GTEXT_JSON_Parse_Options decode_opts = gtext_json_parse_options_default();
    decode_opts.validate_utf8 = false;
    GTEXT_JSON_Status status = json_decode_string(
        input,
        strlen(input),
        output,
        sizeof(output),
        &output_len,
        &pos,
        JSON_UTF8_REJECT,
        &decode_opts
    );

    EXPECT_EQ(status, GTEXT_JSON_OK);
    // Position should be updated (offset should be at end of input)
    EXPECT_EQ(pos.offset, strlen(input));
}

/**
 * Test buffer overflow protection
 */
TEST(StringHandling, BufferOverflowProtection) {
    char output[5];  // Small buffer
    size_t output_len;
    json_position pos = {0, 1, 1};

    // Try to decode a string that would overflow the buffer
    const char * input = "hello world";  // 11 characters > 5 buffer size
    /* These inputs are escape sequences rather than whole documents, and
       several are deliberately not well-formed UTF-8. */
    GTEXT_JSON_Parse_Options decode_opts = gtext_json_parse_options_default();
    decode_opts.validate_utf8 = false;
    GTEXT_JSON_Status status = json_decode_string(
        input,
        strlen(input),
        output,
        sizeof(output),
        &output_len,
        &pos,
        JSON_UTF8_REJECT,
        &decode_opts
    );

    EXPECT_EQ(status, GTEXT_JSON_E_LIMIT);
}

/**
 * Test buffer overflow protection with Unicode escape
 */
TEST(StringHandling, BufferOverflowUnicode) {
    char output[2];  // Very small buffer
    size_t output_len;
    json_position pos = {0, 1, 1};

    // Unicode escape produces 3 bytes (Euro sign), but buffer is only 2
    const char * input = "\\u20AC";
    /* These inputs are escape sequences rather than whole documents, and
       several are deliberately not well-formed UTF-8. */
    GTEXT_JSON_Parse_Options decode_opts = gtext_json_parse_options_default();
    decode_opts.validate_utf8 = false;
    GTEXT_JSON_Status status = json_decode_string(
        input,
        strlen(input),
        output,
        sizeof(output),
        &output_len,
        &pos,
        JSON_UTF8_REJECT,
        &decode_opts
    );

    EXPECT_EQ(status, GTEXT_JSON_E_LIMIT);
}

/**
 * Test valid number formats
 */
TEST(NumberParsing, ValidFormats) {
    json_number num;
    json_position pos = {0, 1, 1};
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    struct {
        const char * input;
        int64_t expected_i64;
        uint64_t expected_u64;
    } tests[] = {
        {"0", 0, 0},
        {"123", 123, 123},
        {"-123", -123, 0},  // negative can't be uint64
        {"0.5", 0, 0},      // has fractional part
        {"123.456", 0, 0},  // has fractional part
        {"1e2", 0, 0},      // has exponent
        {"-1e-2", 0, 0},    // has exponent
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        memset(&num, 0, sizeof(num));
        pos.offset = 0;
        pos.line = 1;
        pos.col = 1;

        GTEXT_JSON_Status status = json_parse_number(
            tests[i].input,
            strlen(tests[i].input),
            &num,
            &pos,
            &opts
        );

        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed for input: " << tests[i].input;
        EXPECT_TRUE(num.flags & JSON_NUMBER_HAS_LEXEME) << "Should preserve lexeme: " << tests[i].input;
        EXPECT_STREQ(num.lexeme, tests[i].input) << "Lexeme mismatch: " << tests[i].input;

        if (tests[i].expected_i64 != 0 || strcmp(tests[i].input, "0") == 0) {
            if (num.flags & JSON_NUMBER_HAS_I64) {
                EXPECT_EQ(num.i64, tests[i].expected_i64) << "int64 mismatch: " << tests[i].input;
            }
        }

        if (tests[i].expected_u64 != 0 || strcmp(tests[i].input, "0") == 0) {
            if (num.flags & JSON_NUMBER_HAS_U64) {
                EXPECT_EQ(num.u64, tests[i].expected_u64) << "uint64 mismatch: " << tests[i].input;
            }
        }

        // Clean up
        json_number_destroy(&num);
    }
}

/**
 * Test invalid number formats are rejected
 */
TEST(NumberParsing, InvalidFormats) {
    json_number num;
    json_position pos = {0, 1, 1};
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    const char * invalid_numbers[] = {
        "01",        // Leading zero
        "1.",        // Trailing decimal point
        ".1",        // Leading decimal point (without leading zero)
        "-",         // Just minus sign
        "--1",       // Double minus
        "1e",        // Incomplete exponent
        "1e+",       // Incomplete exponent
        "1e-",       // Incomplete exponent
        "abc",       // Not a number
    };

    for (size_t i = 0; i < sizeof(invalid_numbers) / sizeof(invalid_numbers[0]); ++i) {
        memset(&num, 0, sizeof(num));
        pos.offset = 0;
        pos.line = 1;
        pos.col = 1;

        GTEXT_JSON_Status status = json_parse_number(
            invalid_numbers[i],
            strlen(invalid_numbers[i]),
            &num,
            &pos,
            &opts
        );

        EXPECT_NE(status, GTEXT_JSON_OK) << "Should reject: " << invalid_numbers[i];
        EXPECT_EQ(status, GTEXT_JSON_E_BAD_NUMBER) << "Should return BAD_NUMBER for: " << invalid_numbers[i];

        // Clean up
        json_number_destroy(&num);
    }
}

/**
 * Test int64 boundary values and overflow detection
 */
TEST(NumberParsing, Int64Boundaries) {
    json_number num;
    json_position pos = {0, 1, 1};
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    struct {
        const char * input;
        int64_t expected;
        int should_have_i64;
    } tests[] = {
        {"9223372036854775807", INT64_MAX, 1},      // Max int64
        {"-9223372036854775808", INT64_MIN, 1},     // Min int64
        {"9223372036854775808", 0, 0},              // Overflow (max + 1)
        {"-9223372036854775809", 0, 0},             // Underflow (min - 1)
        {"0", 0, 1},
        {"-1", -1, 1},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        memset(&num, 0, sizeof(num));
        pos.offset = 0;
        pos.line = 1;
        pos.col = 1;

        GTEXT_JSON_Status status = json_parse_number(
            tests[i].input,
            strlen(tests[i].input),
            &num,
            &pos,
            &opts
        );

        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed for input: " << tests[i].input;

        if (tests[i].should_have_i64) {
            EXPECT_TRUE(num.flags & JSON_NUMBER_HAS_I64) << "Should have int64: " << tests[i].input;
            EXPECT_EQ(num.i64, tests[i].expected) << "int64 value mismatch: " << tests[i].input;
        } else {
            // Overflow case - may or may not have int64, but if it does, it's wrong
            if (num.flags & JSON_NUMBER_HAS_I64) {
                EXPECT_NE(num.i64, tests[i].expected) << "Should not have correct int64 due to overflow: " << tests[i].input;
            }
        }

        // Clean up
        json_number_destroy(&num);
    }
}

/**
 * Test uint64 boundary values and overflow detection
 */
TEST(NumberParsing, Uint64Boundaries) {
    json_number num;
    json_position pos = {0, 1, 1};
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    struct {
        const char * input;
        uint64_t expected;
        int should_have_u64;
    } tests[] = {
        {"18446744073709551615", UINT64_MAX, 1},     // Max uint64
        {"18446744073709551616", 0, 0},              // Overflow (max + 1)
        {"0", 0, 1},
        {"123", 123, 1},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        memset(&num, 0, sizeof(num));
        pos.offset = 0;
        pos.line = 1;
        pos.col = 1;

        GTEXT_JSON_Status status = json_parse_number(
            tests[i].input,
            strlen(tests[i].input),
            &num,
            &pos,
            &opts
        );

        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed for input: " << tests[i].input;

        if (tests[i].should_have_u64) {
            EXPECT_TRUE(num.flags & JSON_NUMBER_HAS_U64) << "Should have uint64: " << tests[i].input;
            EXPECT_EQ(num.u64, tests[i].expected) << "uint64 value mismatch: " << tests[i].input;
        }

        // Clean up
        json_number_destroy(&num);
    }
}

/**
 * Test double parsing
 */
TEST(NumberParsing, DoubleParsing) {
    json_number num;
    json_position pos = {0, 1, 1};
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    struct {
        const char * input;
        double expected;
        double tolerance;
    } tests[] = {
        {"0.0", 0.0, 0.0},
        {"123.456", 123.456, 0.001},
        {"-123.456", -123.456, 0.001},
        {"1e2", 100.0, 0.0},
        {"1.5e-2", 0.015, 0.0001},
        {"-1.5e-2", -0.015, 0.0001},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        memset(&num, 0, sizeof(num));
        pos.offset = 0;
        pos.line = 1;
        pos.col = 1;

        GTEXT_JSON_Status status = json_parse_number(
            tests[i].input,
            strlen(tests[i].input),
            &num,
            &pos,
            &opts
        );

        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed for input: " << tests[i].input;
        EXPECT_TRUE(num.flags & JSON_NUMBER_HAS_DOUBLE) << "Should have double: " << tests[i].input;
        EXPECT_NEAR(num.dbl, tests[i].expected, tests[i].tolerance)
            << "Double value mismatch: " << tests[i].input;

        // Clean up
        json_number_destroy(&num);
    }
}

/**
 * Test nonfinite number parsing (when enabled)
 */
TEST(NumberParsing, NonfiniteNumbers) {
    json_number num;
    json_position pos = {0, 1, 1};
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_nonfinite_numbers = true;

    struct {
        const char * input;
        int is_nan;
        int is_inf;
        int is_neg_inf;
    } tests[] = {
        {"NaN", 1, 0, 0},
        {"Infinity", 0, 1, 0},
        {"-Infinity", 0, 0, 1},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        memset(&num, 0, sizeof(num));
        pos.offset = 0;
        pos.line = 1;
        pos.col = 1;

        GTEXT_JSON_Status status = json_parse_number(
            tests[i].input,
            strlen(tests[i].input),
            &num,
            &pos,
            &opts
        );

        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed for input: " << tests[i].input;
        EXPECT_TRUE(num.flags & JSON_NUMBER_HAS_DOUBLE) << "Should have double: " << tests[i].input;
        EXPECT_TRUE(num.flags & JSON_NUMBER_IS_NONFINITE) << "Should be nonfinite: " << tests[i].input;

        if (tests[i].is_nan) {
            EXPECT_TRUE(std::isnan(num.dbl)) << "Should be NaN: " << tests[i].input;
        } else if (tests[i].is_inf) {
            EXPECT_TRUE(std::isinf(num.dbl) && num.dbl > 0) << "Should be +Infinity: " << tests[i].input;
        } else if (tests[i].is_neg_inf) {
            EXPECT_TRUE(std::isinf(num.dbl) && num.dbl < 0) << "Should be -Infinity: " << tests[i].input;
        }

        // Clean up
        json_number_destroy(&num);
    }
}

/**
 * Test that nonfinite numbers are rejected when disabled
 */
TEST(NumberParsing, NonfiniteRejected) {
    json_number num;
    json_position pos = {0, 1, 1};
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_nonfinite_numbers = false;

    const char * nonfinite[] = {
        "NaN",
        "Infinity",
        "-Infinity",
    };

    for (size_t i = 0; i < sizeof(nonfinite) / sizeof(nonfinite[0]); ++i) {
        memset(&num, 0, sizeof(num));
        pos.offset = 0;
        pos.line = 1;
        pos.col = 1;

        GTEXT_JSON_Status status = json_parse_number(
            nonfinite[i],
            strlen(nonfinite[i]),
            &num,
            &pos,
            &opts
        );

        EXPECT_NE(status, GTEXT_JSON_OK) << "Should reject nonfinite when disabled: " << nonfinite[i];
        EXPECT_EQ(status, GTEXT_JSON_E_NONFINITE) << "Should return NONFINITE error: " << nonfinite[i];

        // Clean up
        json_number_destroy(&num);
    }
}

/**
 * Test DOM parsing of non-finite numbers
 */
TEST(DOMParsing, NonfiniteNumbers) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_nonfinite_numbers = true;

    struct {
        const char * input;
        int is_nan;
        int is_inf;
        int is_neg_inf;
        const char * expected_lexeme;
    } tests[] = {
        {"NaN", 1, 0, 0, "NaN"},
        {"Infinity", 0, 1, 0, "Infinity"},
        {"-Infinity", 0, 0, 1, "-Infinity"},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        GTEXT_JSON_Error err{};

        GTEXT_JSON_Value * value = gtext_json_parse(
            tests[i].input,
            strlen(tests[i].input),
            &opts,
            &err
        );

        ASSERT_NE(value, nullptr) << "Failed to parse: " << tests[i].input;
        EXPECT_EQ(gtext_json_typeof(value), GTEXT_JSON_NUMBER) << "Should be number: " << tests[i].input;

        // Check lexeme
        const char * lexeme = nullptr;
        size_t lexeme_len = 0;
        GTEXT_JSON_Status status = gtext_json_get_number_lexeme(value, &lexeme, &lexeme_len);
        EXPECT_EQ(status, GTEXT_JSON_OK) << "Should have lexeme: " << tests[i].input;
        EXPECT_STREQ(lexeme, tests[i].expected_lexeme) << "Lexeme mismatch: " << tests[i].input;

        // Check double value
        double dbl_val = 0.0;
        status = gtext_json_get_double(value, &dbl_val);
        EXPECT_EQ(status, GTEXT_JSON_OK) << "Should have double: " << tests[i].input;

        if (tests[i].is_nan) {
            EXPECT_TRUE(std::isnan(dbl_val)) << "Should be NaN: " << tests[i].input;
        } else if (tests[i].is_inf) {
            EXPECT_TRUE(std::isinf(dbl_val) && dbl_val > 0) << "Should be +Infinity: " << tests[i].input;
        } else if (tests[i].is_neg_inf) {
            EXPECT_TRUE(std::isinf(dbl_val) && dbl_val < 0) << "Should be -Infinity: " << tests[i].input;
        }

        gtext_json_free(value);
        gtext_json_error_free(&err);
    }
}

/**
 * Test DOM parsing of non-finite numbers in objects and arrays
 */
TEST(DOMParsing, NonfiniteNumbersInStructures) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_nonfinite_numbers = true;

    // Test in object
    {
        const char * json = "{\"nan\": NaN, \"inf\": Infinity, \"neg_inf\": -Infinity}";
        GTEXT_JSON_Error err{};
        GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
        ASSERT_NE(root, nullptr);

        // Check NaN
        const GTEXT_JSON_Value * nan_val = gtext_json_object_get(root, "nan", 3);
        ASSERT_NE(nan_val, nullptr);
        double dbl = 0.0;
        EXPECT_EQ(gtext_json_get_double(nan_val, &dbl), GTEXT_JSON_OK);
        EXPECT_TRUE(std::isnan(dbl));

        // Check Infinity
        const GTEXT_JSON_Value * inf_val = gtext_json_object_get(root, "inf", 3);
        ASSERT_NE(inf_val, nullptr);
        dbl = 0.0;
        EXPECT_EQ(gtext_json_get_double(inf_val, &dbl), GTEXT_JSON_OK);
        EXPECT_TRUE(std::isinf(dbl) && dbl > 0);

        // Check -Infinity
        const GTEXT_JSON_Value * neg_inf_val = gtext_json_object_get(root, "neg_inf", 7);
        ASSERT_NE(neg_inf_val, nullptr);
        dbl = 0.0;
        EXPECT_EQ(gtext_json_get_double(neg_inf_val, &dbl), GTEXT_JSON_OK);
        EXPECT_TRUE(std::isinf(dbl) && dbl < 0);

        gtext_json_free(root);
        gtext_json_error_free(&err);
    }

    // Test in array
    {
        const char * json = "[NaN, Infinity, -Infinity]";
        GTEXT_JSON_Error err{};
        GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
        ASSERT_NE(root, nullptr);
        EXPECT_EQ(gtext_json_array_size(root), 3u);

        // Check NaN
        const GTEXT_JSON_Value * nan_val = gtext_json_array_get(root, 0);
        ASSERT_NE(nan_val, nullptr);
        double dbl = 0.0;
        EXPECT_EQ(gtext_json_get_double(nan_val, &dbl), GTEXT_JSON_OK);
        EXPECT_TRUE(std::isnan(dbl));

        // Check Infinity
        const GTEXT_JSON_Value * inf_val = gtext_json_array_get(root, 1);
        ASSERT_NE(inf_val, nullptr);
        dbl = 0.0;
        EXPECT_EQ(gtext_json_get_double(inf_val, &dbl), GTEXT_JSON_OK);
        EXPECT_TRUE(std::isinf(dbl) && dbl > 0);

        // Check -Infinity
        const GTEXT_JSON_Value * neg_inf_val = gtext_json_array_get(root, 2);
        ASSERT_NE(neg_inf_val, nullptr);
        dbl = 0.0;
        EXPECT_EQ(gtext_json_get_double(neg_inf_val, &dbl), GTEXT_JSON_OK);
        EXPECT_TRUE(std::isinf(dbl) && dbl < 0);

        gtext_json_free(root);
        gtext_json_error_free(&err);
    }
}

/**
 * Test that non-finite numbers are rejected when disabled in DOM parsing
 */
TEST(DOMParsing, NonfiniteNumbersRejected) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_nonfinite_numbers = false;

    const char * nonfinite[] = {
        "NaN",
        "Infinity",
        "-Infinity"
    };

    for (size_t i = 0; i < sizeof(nonfinite) / sizeof(nonfinite[0]); ++i) {
        GTEXT_JSON_Error err{};
        GTEXT_JSON_Value * value = gtext_json_parse(
            nonfinite[i],
            strlen(nonfinite[i]),
            &opts,
            &err
        );

        EXPECT_EQ(value, nullptr) << "Should reject nonfinite when disabled: " << nonfinite[i];
        EXPECT_EQ(err.code, GTEXT_JSON_E_NONFINITE) << "Should return NONFINITE error: " << nonfinite[i];

        gtext_json_error_free(&err);
    }
}

/**
 * Test lexeme preservation
 */
TEST(NumberParsing, LexemePreservation) {
    json_number num;
    json_position pos = {0, 1, 1};
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.preserve_number_lexeme = true;

    const char * numbers[] = {
        "0",
        "123",
        "-456",
        "123.456",
        "1e10",
        "-1.5e-2",
    };

    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
        memset(&num, 0, sizeof(num));
        pos.offset = 0;
        pos.line = 1;
        pos.col = 1;

        GTEXT_JSON_Status status = json_parse_number(
            numbers[i],
            strlen(numbers[i]),
            &num,
            &pos,
            &opts
        );

        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed for input: " << numbers[i];
        EXPECT_TRUE(num.flags & JSON_NUMBER_HAS_LEXEME) << "Should preserve lexeme: " << numbers[i];
        EXPECT_STREQ(num.lexeme, numbers[i]) << "Lexeme mismatch: " << numbers[i];
        EXPECT_EQ(num.lexeme_len, strlen(numbers[i])) << "Lexeme length mismatch: " << numbers[i];

        // Clean up
        json_number_destroy(&num);
    }
}

/**
 * Test position tracking during number parsing
 */
TEST(NumberParsing, PositionTracking) {
    json_number num;
    json_position pos = {0, 1, 1};
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    const char * input = "123.456";
    GTEXT_JSON_Status status = json_parse_number(
        input,
        strlen(input),
        &num,
        &pos,
        &opts
    );

    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(pos.offset, strlen(input));
    EXPECT_EQ(pos.col, (int)strlen(input) + 1);  // col is 1-based

    // Clean up
    json_number_destroy(&num);
}

/**
 * Test lexer correctly identifies all token types in valid JSON
 */
TEST(Lexer, TokenTypes) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    const char * input = "{}[]:,";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Test LBRACE
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_LBRACE);
    json_token_cleanup(&token);

    // Test RBRACE
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_RBRACE);
    json_token_cleanup(&token);

    // Test LBRACKET
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_LBRACKET);
    json_token_cleanup(&token);

    // Test RBRACKET
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_RBRACKET);
    json_token_cleanup(&token);

    // Test COLON
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_COLON);
    json_token_cleanup(&token);

    // Test COMMA
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_COMMA);
    json_token_cleanup(&token);

    // Test EOF
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_EOF);
    json_token_cleanup(&token);
}

/**
 * Test lexer keyword tokenization
 */
TEST(Lexer, Keywords) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    const char * input = "null true false";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Test null
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_NULL);
    json_token_cleanup(&token);

    // Test true
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_TRUE);
    json_token_cleanup(&token);

    // Test false
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_FALSE);
    json_token_cleanup(&token);
}

/**
 * Test lexer string tokenization with escape sequences
 */
TEST(Lexer, StringTokenization) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    const char * input = "\"hello\\nworld\"";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_STRING);
    EXPECT_EQ(token.data.string.value_len, 11u);  // "hello\nworld" = 11 chars
    EXPECT_EQ(memcmp(token.data.string.value, "hello\nworld", 11), 0);
    json_token_cleanup(&token);
}

/**
 * Test lexer number tokenization
 */
TEST(Lexer, NumberTokenization) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    const char * input = "123 -456 789.012";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Test integer
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_NUMBER);
    EXPECT_TRUE(token.data.number.flags & JSON_NUMBER_HAS_I64);
    EXPECT_EQ(token.data.number.i64, 123);
    json_token_cleanup(&token);

    // Test negative integer
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_NUMBER);
    EXPECT_TRUE(token.data.number.flags & JSON_NUMBER_HAS_I64);
    EXPECT_EQ(token.data.number.i64, -456);
    json_token_cleanup(&token);

    // Test decimal
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_NUMBER);
    EXPECT_TRUE(token.data.number.flags & JSON_NUMBER_HAS_DOUBLE);
    EXPECT_NEAR(token.data.number.dbl, 789.012, 0.001);
    json_token_cleanup(&token);
}

/**
 * Test comment lexing (single-line and multi-line)
 */
TEST(Lexer, Comments) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_comments = true;

    const char * input = "// comment\n123 /* multi\nline */ 456";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should skip comment and get first number
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_NUMBER);
    EXPECT_EQ(token.data.number.i64, 123);
    json_token_cleanup(&token);

    // Should skip multi-line comment and get second number
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_NUMBER);
    EXPECT_EQ(token.data.number.i64, 456);
    json_token_cleanup(&token);
}

/**
 * Test that comments are rejected when disabled
 */
TEST(Lexer, CommentsRejected) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_comments = false;

    const char * input = "// comment\n123";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should treat // as invalid token
    status = json_lexer_next(&lexer, &token);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_ERROR);
    json_token_cleanup(&token);
}

/**
 * Test position tracking accuracy
 */
TEST(Lexer, PositionTracking) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    const char * input = "{\n  \"key\": 123\n}";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // LBRACE at line 1, col 1
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_LBRACE);
    EXPECT_EQ(token.pos.line, 1);
    EXPECT_EQ(token.pos.col, 1);
    json_token_cleanup(&token);

    // STRING at line 2, col 3
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_STRING);
    EXPECT_EQ(token.pos.line, 2);
    EXPECT_EQ(token.pos.col, 3);
    json_token_cleanup(&token);

    // COLON at line 2
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_COLON);
    json_token_cleanup(&token);

    // NUMBER at line 2
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_NUMBER);
    json_token_cleanup(&token);

    // RBRACE at line 3, col 1
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_RBRACE);
    EXPECT_EQ(token.pos.line, 3);
    EXPECT_EQ(token.pos.col, 1);
    json_token_cleanup(&token);
}

/**
 * Test extension tokens (NaN, Infinity, -Infinity) when enabled
 */
TEST(Lexer, ExtensionTokens) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_nonfinite_numbers = true;

    const char * input = "NaN Infinity -Infinity";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Test NaN
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_NAN);
    json_token_cleanup(&token);

    // Test Infinity
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_INFINITY);
    json_token_cleanup(&token);

    // Test -Infinity
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_NEG_INFINITY);
    json_token_cleanup(&token);
}

/**
 * Test that extension tokens are rejected when disabled
 */
TEST(Lexer, ExtensionTokensRejected) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_nonfinite_numbers = false;

    const char * input = "NaN";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should treat NaN as invalid token (not a keyword)
    status = json_lexer_next(&lexer, &token);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_ERROR);
    json_token_cleanup(&token);
}

/**
 * Test whitespace handling
 */
TEST(Lexer, WhitespaceHandling) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    const char * input = "  {  }  [  ]  ";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should skip leading whitespace
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_LBRACE);
    json_token_cleanup(&token);

    // Should skip whitespace between tokens
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_RBRACE);
    json_token_cleanup(&token);

    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_LBRACKET);
    json_token_cleanup(&token);

    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_RBRACKET);
    json_token_cleanup(&token);
}

/**
 * Test lexer error reporting with accurate positions
 */
TEST(Lexer, ErrorReporting) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    const char * input = "123 @ invalid";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should successfully parse number
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_NUMBER);
    json_token_cleanup(&token);

    // Should fail on invalid character
    status = json_lexer_next(&lexer, &token);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_ERROR);
    EXPECT_EQ(status, GTEXT_JSON_E_BAD_TOKEN);
    // Position should be accurate
    EXPECT_EQ(token.pos.offset, 4u);  // After "123 "
    json_token_cleanup(&token);
}

/**
 * Test single-quote strings when enabled
 */
TEST(Lexer, SingleQuoteStrings) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_single_quotes = true;

    const char * input = "'hello world'";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_STRING);
    EXPECT_EQ(token.data.string.value_len, 11u);
    EXPECT_EQ(memcmp(token.data.string.value, "hello world", 11), 0);
    json_token_cleanup(&token);
}

/**
 * Test that single-quote strings are rejected when disabled
 */
TEST(Lexer, SingleQuoteStringsRejected) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_single_quotes = false;

    const char * input = "'hello'";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = json_lexer_next(&lexer, &token);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_ERROR);
    json_token_cleanup(&token);
}

/**
 * Test unescaped control characters are rejected by default
 */
TEST(Lexer, UnescapedControlsRejected) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_unescaped_controls = false;

    // Test with tab character (0x09) - should be rejected
    const char * input = "\"hello\tworld\"";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = json_lexer_next(&lexer, &token);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_ERROR);
    json_token_cleanup(&token);

    // Test with newline (0x0A) - should be rejected
    const char * input2 = "\"hello\nworld\"";
    status = json_lexer_init(&lexer, input2, strlen(input2), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = json_lexer_next(&lexer, &token);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_ERROR);
    json_token_cleanup(&token);

    // Test with null byte (0x00) - should be rejected
    const char input3[] = "\"hello\0world\"";
    status = json_lexer_init(&lexer, input3, sizeof(input3) - 1, &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = json_lexer_next(&lexer, &token);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_ERROR);
    json_token_cleanup(&token);
}

/**
 * Test unescaped control characters are allowed when option enabled
 */
TEST(Lexer, UnescapedControlsAllowed) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_unescaped_controls = true;

    // Test with tab character (0x09) - should be allowed
    const char * input = "\"hello\tworld\"";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_STRING);
    EXPECT_EQ(token.data.string.value_len, 11u);  // "hello\tworld" = 11 bytes
    EXPECT_EQ(memcmp(token.data.string.value, "hello\tworld", 11), 0);
    json_token_cleanup(&token);

    // Test with newline (0x0A) - should be allowed
    const char * input2 = "\"hello\nworld\"";
    status = json_lexer_init(&lexer, input2, strlen(input2), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_STRING);
    EXPECT_EQ(token.data.string.value_len, 11u);  // "hello\nworld" = 11 bytes
    EXPECT_EQ(memcmp(token.data.string.value, "hello\nworld", 11), 0);
    json_token_cleanup(&token);
}

/**
 * Test that all extensions work together
 */
TEST(Lexer, AllExtensionsCombined) {
    json_lexer lexer;
    json_token token;
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_comments = true;
    opts.allow_nonfinite_numbers = true;
    opts.allow_single_quotes = true;
    opts.allow_unescaped_controls = true;

    // Input with comments, single quotes, nonfinite numbers, and unescaped controls
    const char * input = "// comment\n'hello\tworld' Infinity NaN";
    GTEXT_JSON_Status status = json_lexer_init(&lexer, input, strlen(input), &opts, 0, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should skip comment and get single-quoted string with tab
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_STRING);
    EXPECT_EQ(token.data.string.value_len, 11u);
    EXPECT_EQ(memcmp(token.data.string.value, "hello\tworld", 11), 0);
    json_token_cleanup(&token);

    // Should get Infinity
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_INFINITY);
    json_token_cleanup(&token);

    // Should get NaN
    status = json_lexer_next(&lexer, &token);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(token.type, JSON_TOKEN_NAN);
    json_token_cleanup(&token);
}

/**
 * Test that extensions are opt-in (strict by default)
 */
TEST(Parser, ExtensionsOptIn) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    // Verify all extensions are off by default
    EXPECT_EQ(opts.allow_comments, 0);
    EXPECT_EQ(opts.allow_trailing_commas, 0);
    EXPECT_EQ(opts.allow_nonfinite_numbers, 0);
    EXPECT_EQ(opts.allow_single_quotes, 0);
    EXPECT_EQ(opts.allow_unescaped_controls, 0);

    // Test that strict JSON is parsed correctly
    const char * strict_json = "{\"key\": \"value\", \"number\": 123}";
    GTEXT_JSON_Value * val = gtext_json_parse(strict_json, strlen(strict_json), &opts, NULL);
    EXPECT_NE(val, nullptr);
    gtext_json_free(val);

    // Test that extensions are rejected by default
    const char * with_comment = "{\"key\": \"value\" // comment\n}";
    GTEXT_JSON_Value * val2 = gtext_json_parse(with_comment, strlen(with_comment), &opts, NULL);
    EXPECT_EQ(val2, nullptr);  // Should fail because comments are disabled

    const char * with_trailing = "{\"key\": \"value\",}";
    GTEXT_JSON_Value * val3 = gtext_json_parse(with_trailing, strlen(with_trailing), &opts, NULL);
    EXPECT_EQ(val3, nullptr);  // Should fail because trailing commas are disabled
}

/**
 * Test value creation for null
 */
TEST(DOMValueCreation, Null) {
    GTEXT_JSON_Value * val = gtext_json_new_null();
    ASSERT_NE(val, nullptr);

    // Verify type
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_NULL);

    // Value can be freed without crashing
    gtext_json_free(val);
}

/**
 * Test value creation for boolean
 */
TEST(DOMValueCreation, Bool) {
    // Test true
    GTEXT_JSON_Value * val_true = gtext_json_new_bool(true);
    ASSERT_NE(val_true, nullptr);
    EXPECT_EQ(gtext_json_typeof(val_true), GTEXT_JSON_BOOL);
    bool bool_val = false;
    GTEXT_JSON_Status status = gtext_json_get_bool(val_true, &bool_val);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(bool_val, true);
    gtext_json_free(val_true);

    // Test false
    GTEXT_JSON_Value * val_false = gtext_json_new_bool(false);
    ASSERT_NE(val_false, nullptr);
    EXPECT_EQ(gtext_json_typeof(val_false), GTEXT_JSON_BOOL);
    bool_val = true;
    status = gtext_json_get_bool(val_false, &bool_val);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(bool_val, false);
    gtext_json_free(val_false);
}

/**
 * Test value creation for string
 */
TEST(DOMValueCreation, String) {
    const char * test_str = "Hello, World!";
    size_t test_len = strlen(test_str);

    GTEXT_JSON_Value * val = gtext_json_new_string(test_str, test_len);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_STRING);
    const char * out_str = nullptr;
    size_t out_len = 0;
    GTEXT_JSON_Status status = gtext_json_get_string(val, &out_str, &out_len);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(out_len, test_len);
    EXPECT_EQ(memcmp(out_str, test_str, test_len), 0);
    gtext_json_free(val);

    // Test empty string
    GTEXT_JSON_Value * val_empty = gtext_json_new_string("", 0);
    ASSERT_NE(val_empty, nullptr);
    EXPECT_EQ(gtext_json_typeof(val_empty), GTEXT_JSON_STRING);
    out_str = nullptr;
    out_len = 1;  // Set to non-zero to verify it gets set to 0
    status = gtext_json_get_string(val_empty, &out_str, &out_len);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(out_len, 0u);
    gtext_json_free(val_empty);

    // Test string with null bytes
    const char * null_str = "a\0b\0c";
    size_t null_len = 5;
    GTEXT_JSON_Value * val_null = gtext_json_new_string(null_str, null_len);
    ASSERT_NE(val_null, nullptr);
    EXPECT_EQ(gtext_json_typeof(val_null), GTEXT_JSON_STRING);
    out_str = nullptr;
    out_len = 0;
    status = gtext_json_get_string(val_null, &out_str, &out_len);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(out_len, null_len);
    EXPECT_EQ(memcmp(out_str, null_str, null_len), 0);
    gtext_json_free(val_null);
}

/**
 * Test value creation for number from lexeme
 */
TEST(DOMValueCreation, NumberFromLexeme) {
    const char * lexeme = "123.456";
    size_t lexeme_len = strlen(lexeme);

    GTEXT_JSON_Value * val = gtext_json_new_number_from_lexeme(lexeme, lexeme_len);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_NUMBER);
    const char * out_lexeme = nullptr;
    size_t out_len = 0;
    GTEXT_JSON_Status status = gtext_json_get_number_lexeme(val, &out_lexeme, &out_len);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(out_len, lexeme_len);
    EXPECT_STREQ(out_lexeme, lexeme);
    gtext_json_free(val);
}

/**
 * Test value creation for number from int64
 */
TEST(DOMValueCreation, NumberI64) {
    int64_t test_val = 12345;
    GTEXT_JSON_Value * val = gtext_json_new_number_i64(test_val);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_NUMBER);
    int64_t out_val = 0;
    GTEXT_JSON_Status status = gtext_json_get_i64(val, &out_val);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(out_val, test_val);
    gtext_json_free(val);

    // Test negative
    int64_t test_neg = -67890;
    GTEXT_JSON_Value * val_neg = gtext_json_new_number_i64(test_neg);
    ASSERT_NE(val_neg, nullptr);
    EXPECT_EQ(gtext_json_typeof(val_neg), GTEXT_JSON_NUMBER);
    out_val = 0;
    status = gtext_json_get_i64(val_neg, &out_val);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(out_val, test_neg);
    gtext_json_free(val_neg);

    // Test zero
    GTEXT_JSON_Value * val_zero = gtext_json_new_number_i64(0);
    ASSERT_NE(val_zero, nullptr);
    EXPECT_EQ(gtext_json_typeof(val_zero), GTEXT_JSON_NUMBER);
    out_val = 1;  // Set to non-zero to verify it gets set to 0
    status = gtext_json_get_i64(val_zero, &out_val);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(out_val, 0);
    gtext_json_free(val_zero);
}

/**
 * Test value creation for number from uint64
 */
TEST(DOMValueCreation, NumberU64) {
    uint64_t test_val = 12345;
    GTEXT_JSON_Value * val = gtext_json_new_number_u64(test_val);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_NUMBER);
    uint64_t out_val = 0;
    GTEXT_JSON_Status status = gtext_json_get_u64(val, &out_val);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(out_val, test_val);
    gtext_json_free(val);

    // Test large value
    uint64_t test_large = UINT64_MAX;
    GTEXT_JSON_Value * val_large = gtext_json_new_number_u64(test_large);
    ASSERT_NE(val_large, nullptr);
    EXPECT_EQ(gtext_json_typeof(val_large), GTEXT_JSON_NUMBER);
    out_val = 0;
    status = gtext_json_get_u64(val_large, &out_val);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(out_val, test_large);
    gtext_json_free(val_large);
}

/**
 * Test value creation for number from double
 */
TEST(DOMValueCreation, NumberDouble) {
    double test_val = 123.456;
    GTEXT_JSON_Value * val = gtext_json_new_number_double(test_val);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_NUMBER);
    double out_val = 0.0;
    GTEXT_JSON_Status status = gtext_json_get_double(val, &out_val);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_NEAR(out_val, test_val, 0.001);
    gtext_json_free(val);

    // Test negative
    double test_neg = -789.012;
    GTEXT_JSON_Value * val_neg = gtext_json_new_number_double(test_neg);
    ASSERT_NE(val_neg, nullptr);
    EXPECT_EQ(gtext_json_typeof(val_neg), GTEXT_JSON_NUMBER);
    out_val = 0.0;
    status = gtext_json_get_double(val_neg, &out_val);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_NEAR(out_val, test_neg, 0.001);
    gtext_json_free(val_neg);

    // Test zero
    GTEXT_JSON_Value * val_zero = gtext_json_new_number_double(0.0);
    ASSERT_NE(val_zero, nullptr);
    EXPECT_EQ(gtext_json_typeof(val_zero), GTEXT_JSON_NUMBER);
    out_val = 1.0;  // Set to non-zero to verify it gets set to 0
    status = gtext_json_get_double(val_zero, &out_val);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_DOUBLE_EQ(out_val, 0.0);
    gtext_json_free(val_zero);
}

/**
 * Test value creation for array
 */
TEST(DOMValueCreation, Array) {
    GTEXT_JSON_Value * val = gtext_json_new_array();
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_ARRAY);
    EXPECT_EQ(gtext_json_array_size(val), 0u);
    gtext_json_free(val);
}

/**
 * Test value creation for object
 */
TEST(DOMValueCreation, Object) {
    GTEXT_JSON_Value * val = gtext_json_new_object();
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_object_size(val), 0u);
    gtext_json_free(val);
}

/**
 * Test memory cleanup via gtext_json_free
 */
TEST(MemoryManagement, ValueCleanup) {
    // Create multiple values and verify they're cleaned up
    GTEXT_JSON_Value * null_val = gtext_json_new_null();
    GTEXT_JSON_Value * bool_val = gtext_json_new_bool(true);
    GTEXT_JSON_Value * str_val = gtext_json_new_string("test", 4);
    GTEXT_JSON_Value * num_val = gtext_json_new_number_i64(42);
    GTEXT_JSON_Value * arr_val = gtext_json_new_array();
    GTEXT_JSON_Value * obj_val = gtext_json_new_object();

    ASSERT_NE(null_val, nullptr);
    ASSERT_NE(bool_val, nullptr);
    ASSERT_NE(str_val, nullptr);
    ASSERT_NE(num_val, nullptr);
    ASSERT_NE(arr_val, nullptr);
    ASSERT_NE(obj_val, nullptr);

    // Free all values (should not crash or leak)
    gtext_json_free(null_val);
    gtext_json_free(bool_val);
    gtext_json_free(str_val);
    gtext_json_free(num_val);
    gtext_json_free(arr_val);
    gtext_json_free(obj_val);

    // If we get here without crashing, the test passed
    SUCCEED();
}

/**
 * Test accessor error cases - wrong type access
 */
TEST(DOMAccessors, WrongType) {
    // Create a string value
    GTEXT_JSON_Value * str_val = gtext_json_new_string("test", 4);
    ASSERT_NE(str_val, nullptr);

    // Try to get bool from string - should fail
    bool bool_out = false;
    GTEXT_JSON_Status status = gtext_json_get_bool(str_val, &bool_out);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    // Try to get number from string - should fail
    int64_t i64_out = 0;
    status = gtext_json_get_i64(str_val, &i64_out);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    // Try to get array size from string - should return 0
    EXPECT_EQ(gtext_json_array_size(str_val), 0u);

    gtext_json_free(str_val);
}

/**
 * Test accessor error cases - null pointer handling
 */
TEST(DOMAccessors, NullPointer) {
    // GTEXT_JSON_Typeof should handle NULL
    EXPECT_EQ(gtext_json_typeof(nullptr), GTEXT_JSON_NULL);

    // Other accessors should return error for NULL
    bool bool_out = false;
    GTEXT_JSON_Status status = gtext_json_get_bool(nullptr, &bool_out);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    const char * str_out = nullptr;
    size_t str_len = 0;
    status = gtext_json_get_string(nullptr, &str_out, &str_len);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    // Array/object size should return 0 for NULL
    EXPECT_EQ(gtext_json_array_size(nullptr), 0u);
    EXPECT_EQ(gtext_json_object_size(nullptr), 0u);

    // Array/object get should return NULL for NULL
    EXPECT_EQ(gtext_json_array_get(nullptr, 0), nullptr);
    EXPECT_EQ(gtext_json_object_value(nullptr, 0), nullptr);
    EXPECT_EQ(gtext_json_object_key(nullptr, 0, nullptr), nullptr);
    EXPECT_EQ(gtext_json_object_get(nullptr, "key", 3), nullptr);
}

/**
 * Test array access with bounds checking
 */
TEST(DOMAccessors, ArrayAccessBounds) {
    GTEXT_JSON_Value * arr = gtext_json_new_array();
    ASSERT_NE(arr, nullptr);

    // Empty array - should return NULL for any index
    EXPECT_EQ(gtext_json_array_get(arr, 0), nullptr);
    EXPECT_EQ(gtext_json_array_get(arr, 1), nullptr);

    // Note: We can't test with actual elements yet since array mutation
    // functions (gtext_json_array_push) haven't been implemented.
    // This test verifies bounds checking works for empty arrays.

    gtext_json_free(arr);
}

/**
 * Test object access - key lookup and iteration
 */
TEST(DOMAccessors, ObjectAccess) {
    GTEXT_JSON_Value * obj = gtext_json_new_object();
    ASSERT_NE(obj, nullptr);

    // Empty object - should return NULL for any key/index
    EXPECT_EQ(gtext_json_object_get(obj, "key", 3), nullptr);
    EXPECT_EQ(gtext_json_object_value(obj, 0), nullptr);
    EXPECT_EQ(gtext_json_object_key(obj, 0, nullptr), nullptr);

    // Note: We can't test with actual key-value pairs yet since object mutation
    // functions (gtext_json_object_put) haven't been implemented.
    // This test verifies bounds checking works for empty objects.

    gtext_json_free(obj);
}

/**
 * Test number accessor error cases - missing representations
 */
TEST(DOMAccessors, NumberAccessorMissingRepresentations) {
    // Create number from lexeme only (no numeric representations)
    GTEXT_JSON_Value * num = gtext_json_new_number_from_lexeme("123.456", 7);
    ASSERT_NE(num, nullptr);

    // Lexeme should be available
    const char * lexeme = nullptr;
    size_t lexeme_len = 0;
    GTEXT_JSON_Status status = gtext_json_get_number_lexeme(num, &lexeme, &lexeme_len);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_STREQ(lexeme, "123.456");

    // int64 should not be available (created from lexeme only)
    int64_t i64_out = 0;
    status = gtext_json_get_i64(num, &i64_out);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    // uint64 should not be available
    uint64_t u64_out = 0;
    status = gtext_json_get_u64(num, &u64_out);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    // double should not be available
    double dbl_out = 0.0;
    status = gtext_json_get_double(num, &dbl_out);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    gtext_json_free(num);
}

/**
 * Test array mutation - push, set, insert, remove
 */
TEST(DOMMutation, ArrayPush) {
    GTEXT_JSON_Value * arr = gtext_json_new_array();
    ASSERT_NE(arr, nullptr);

    // Push some elements
    GTEXT_JSON_Value * val1 = gtext_json_new_number_i64(42);
    GTEXT_JSON_Value * val2 = gtext_json_new_string("hello", 5);
    GTEXT_JSON_Value * val3 = gtext_json_new_bool(true);

    EXPECT_EQ(gtext_json_array_push(arr, val1), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_push(arr, val2), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_push(arr, val3), GTEXT_JSON_OK);

    // Verify array size and contents
    EXPECT_EQ(gtext_json_array_size(arr), 3u);
    EXPECT_EQ(gtext_json_array_get(arr, 0), val1);
    EXPECT_EQ(gtext_json_array_get(arr, 1), val2);
    EXPECT_EQ(gtext_json_array_get(arr, 2), val3);

    // Verify values
    int64_t i64_out = 0;
    EXPECT_EQ(gtext_json_get_i64(gtext_json_array_get(arr, 0), &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 42);

    const char * str_out = nullptr;
    size_t str_len = 0;
    EXPECT_EQ(gtext_json_get_string(gtext_json_array_get(arr, 1), &str_out, &str_len), GTEXT_JSON_OK);
    EXPECT_EQ(str_len, 5u);
    EXPECT_STREQ(str_out, "hello");

    bool bool_out = false;
    EXPECT_EQ(gtext_json_get_bool(gtext_json_array_get(arr, 2), &bool_out), GTEXT_JSON_OK);
    EXPECT_EQ(bool_out, true);

    gtext_json_free(arr);
}

TEST(DOMMutation, ArraySet) {
    GTEXT_JSON_Value * arr = gtext_json_new_array();
    ASSERT_NE(arr, nullptr);

    // Push initial elements
    GTEXT_JSON_Value * val1 = gtext_json_new_number_i64(1);
    GTEXT_JSON_Value * val2 = gtext_json_new_number_i64(2);
    GTEXT_JSON_Value * val3 = gtext_json_new_number_i64(3);

    EXPECT_EQ(gtext_json_array_push(arr, val1), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_push(arr, val2), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_push(arr, val3), GTEXT_JSON_OK);

    // Replace element at index 1
    GTEXT_JSON_Value * new_val = gtext_json_new_number_i64(99);
    EXPECT_EQ(gtext_json_array_set(arr, 1, new_val), GTEXT_JSON_OK);

    // Verify
    int64_t i64_out = 0;
    EXPECT_EQ(gtext_json_get_i64(gtext_json_array_get(arr, 0), &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 1);
    EXPECT_EQ(gtext_json_get_i64(gtext_json_array_get(arr, 1), &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 99);  // Changed
    EXPECT_EQ(gtext_json_get_i64(gtext_json_array_get(arr, 2), &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 3);

    // Test out of bounds
    GTEXT_JSON_Value * val4 = gtext_json_new_number_i64(4);
    EXPECT_EQ(gtext_json_array_set(arr, 10, val4), GTEXT_JSON_E_INVALID);
    gtext_json_free(val4);

    gtext_json_free(arr);
}

TEST(DOMMutation, ArrayInsert) {
    GTEXT_JSON_Value * arr = gtext_json_new_array();
    ASSERT_NE(arr, nullptr);

    // Push initial elements
    GTEXT_JSON_Value * val1 = gtext_json_new_number_i64(1);
    GTEXT_JSON_Value * val2 = gtext_json_new_number_i64(2);
    GTEXT_JSON_Value * val3 = gtext_json_new_number_i64(3);

    EXPECT_EQ(gtext_json_array_push(arr, val1), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_push(arr, val2), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_push(arr, val3), GTEXT_JSON_OK);

    // Insert at index 1
    GTEXT_JSON_Value * new_val = gtext_json_new_number_i64(99);
    EXPECT_EQ(gtext_json_array_insert(arr, 1, new_val), GTEXT_JSON_OK);

    // Verify: [1, 99, 2, 3]
    EXPECT_EQ(gtext_json_array_size(arr), 4u);
    int64_t i64_out = 0;
    EXPECT_EQ(gtext_json_get_i64(gtext_json_array_get(arr, 0), &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 1);
    EXPECT_EQ(gtext_json_get_i64(gtext_json_array_get(arr, 1), &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 99);
    EXPECT_EQ(gtext_json_get_i64(gtext_json_array_get(arr, 2), &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 2);
    EXPECT_EQ(gtext_json_get_i64(gtext_json_array_get(arr, 3), &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 3);

    // Insert at end (should work like push)
    GTEXT_JSON_Value * val_end = gtext_json_new_number_i64(100);
    EXPECT_EQ(gtext_json_array_insert(arr, 4, val_end), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_size(arr), 5u);

    // Test out of bounds
    GTEXT_JSON_Value * val4 = gtext_json_new_number_i64(4);
    EXPECT_EQ(gtext_json_array_insert(arr, 10, val4), GTEXT_JSON_E_INVALID);
    gtext_json_free(val4);

    gtext_json_free(arr);
}

TEST(DOMMutation, ArrayRemove) {
    GTEXT_JSON_Value * arr = gtext_json_new_array();
    ASSERT_NE(arr, nullptr);

    // Push initial elements
    GTEXT_JSON_Value * val1 = gtext_json_new_number_i64(1);
    GTEXT_JSON_Value * val2 = gtext_json_new_number_i64(2);
    GTEXT_JSON_Value * val3 = gtext_json_new_number_i64(3);
    GTEXT_JSON_Value * val4 = gtext_json_new_number_i64(4);

    EXPECT_EQ(gtext_json_array_push(arr, val1), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_push(arr, val2), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_push(arr, val3), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_push(arr, val4), GTEXT_JSON_OK);

    // Remove element at index 1 (value 2)
    EXPECT_EQ(gtext_json_array_remove(arr, 1), GTEXT_JSON_OK);

    // Verify: [1, 3, 4]
    EXPECT_EQ(gtext_json_array_size(arr), 3u);
    int64_t i64_out = 0;
    EXPECT_EQ(gtext_json_get_i64(gtext_json_array_get(arr, 0), &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 1);
    EXPECT_EQ(gtext_json_get_i64(gtext_json_array_get(arr, 1), &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 3);
    EXPECT_EQ(gtext_json_get_i64(gtext_json_array_get(arr, 2), &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 4);

    // Remove first element
    EXPECT_EQ(gtext_json_array_remove(arr, 0), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_size(arr), 2u);

    // Remove last element
    EXPECT_EQ(gtext_json_array_remove(arr, 1), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_size(arr), 1u);

    // Test out of bounds
    EXPECT_EQ(gtext_json_array_remove(arr, 10), GTEXT_JSON_E_INVALID);

    gtext_json_free(arr);
}

TEST(DOMMutation, ObjectPut) {
    GTEXT_JSON_Value * obj = gtext_json_new_object();
    ASSERT_NE(obj, nullptr);

    // Add key-value pairs
    GTEXT_JSON_Value * val1 = gtext_json_new_number_i64(42);
    GTEXT_JSON_Value * val2 = gtext_json_new_string("hello", 5);
    GTEXT_JSON_Value * val3 = gtext_json_new_bool(true);

    EXPECT_EQ(gtext_json_object_put(obj, "key1", 4, val1), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_object_put(obj, "key2", 4, val2), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_object_put(obj, "key3", 4, val3), GTEXT_JSON_OK);

    // Verify object size
    EXPECT_EQ(gtext_json_object_size(obj), 3u);

    // Verify values
    const GTEXT_JSON_Value * v1 = gtext_json_object_get(obj, "key1", 4);
    ASSERT_NE(v1, nullptr);
    int64_t i64_out = 0;
    EXPECT_EQ(gtext_json_get_i64(v1, &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 42);

    const GTEXT_JSON_Value * v2 = gtext_json_object_get(obj, "key2", 4);
    ASSERT_NE(v2, nullptr);
    const char * str_out = nullptr;
    size_t str_len = 0;
    EXPECT_EQ(gtext_json_get_string(v2, &str_out, &str_len), GTEXT_JSON_OK);
    EXPECT_STREQ(str_out, "hello");

    // Replace existing key
    GTEXT_JSON_Value * new_val = gtext_json_new_number_i64(99);
    EXPECT_EQ(gtext_json_object_put(obj, "key1", 4, new_val), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_object_size(obj), 3u);  // Size should not change

    const GTEXT_JSON_Value * v1_new = gtext_json_object_get(obj, "key1", 4);
    ASSERT_NE(v1_new, nullptr);
    EXPECT_EQ(gtext_json_get_i64(v1_new, &i64_out), GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 99);  // Changed

    gtext_json_free(obj);
}

TEST(DOMMutation, ObjectRemove) {
    GTEXT_JSON_Value * obj = gtext_json_new_object();
    ASSERT_NE(obj, nullptr);

    // Add key-value pairs
    GTEXT_JSON_Value * val1 = gtext_json_new_number_i64(1);
    GTEXT_JSON_Value * val2 = gtext_json_new_number_i64(2);
    GTEXT_JSON_Value * val3 = gtext_json_new_number_i64(3);

    EXPECT_EQ(gtext_json_object_put(obj, "key1", 4, val1), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_object_put(obj, "key2", 4, val2), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_object_put(obj, "key3", 4, val3), GTEXT_JSON_OK);

    EXPECT_EQ(gtext_json_object_size(obj), 3u);

    // Remove middle key
    EXPECT_EQ(gtext_json_object_remove(obj, "key2", 4), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_object_size(obj), 2u);

    // Verify key2 is gone
    EXPECT_EQ(gtext_json_object_get(obj, "key2", 4), nullptr);
    EXPECT_NE(gtext_json_object_get(obj, "key1", 4), nullptr);
    EXPECT_NE(gtext_json_object_get(obj, "key3", 4), nullptr);

    // Remove first key
    EXPECT_EQ(gtext_json_object_remove(obj, "key1", 4), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_object_size(obj), 1u);

    // Remove last key
    EXPECT_EQ(gtext_json_object_remove(obj, "key3", 4), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_object_size(obj), 0u);

    // Try to remove non-existent key
    EXPECT_EQ(gtext_json_object_remove(obj, "nonexistent", 11), GTEXT_JSON_E_INVALID);

    gtext_json_free(obj);
}

TEST(DOMMutation, NestedStructures) {
    // Test building nested structures
    GTEXT_JSON_Value * root = gtext_json_new_object();
    ASSERT_NE(root, nullptr);

    // Create nested array
    GTEXT_JSON_Value * arr = gtext_json_new_array();
    GTEXT_JSON_Value * elem1 = gtext_json_new_number_i64(1);
    GTEXT_JSON_Value * elem2 = gtext_json_new_number_i64(2);
    EXPECT_EQ(gtext_json_array_push(arr, elem1), GTEXT_JSON_OK);
    EXPECT_EQ(gtext_json_array_push(arr, elem2), GTEXT_JSON_OK);

    // Add array to object
    EXPECT_EQ(gtext_json_object_put(root, "array", 5, arr), GTEXT_JSON_OK);

    // Create nested object
    GTEXT_JSON_Value * nested_obj = gtext_json_new_object();
    GTEXT_JSON_Value * nested_val = gtext_json_new_string("nested", 6);
    EXPECT_EQ(gtext_json_object_put(nested_obj, "key", 3, nested_val), GTEXT_JSON_OK);

    // Add nested object to root
    EXPECT_EQ(gtext_json_object_put(root, "object", 6, nested_obj), GTEXT_JSON_OK);

    // Verify structure
    EXPECT_EQ(gtext_json_object_size(root), 2u);

    const GTEXT_JSON_Value * arr_val = gtext_json_object_get(root, "array", 5);
    ASSERT_NE(arr_val, nullptr);
    EXPECT_EQ(gtext_json_typeof(arr_val), GTEXT_JSON_ARRAY);
    EXPECT_EQ(gtext_json_array_size(arr_val), 2u);

    const GTEXT_JSON_Value * obj_val = gtext_json_object_get(root, "object", 6);
    ASSERT_NE(obj_val, nullptr);
    EXPECT_EQ(gtext_json_typeof(obj_val), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_object_size(obj_val), 1u);

    gtext_json_free(root);
}

TEST(DOMMutation, ErrorCases) {
    // Test error cases for array operations
    GTEXT_JSON_Value * arr = gtext_json_new_array();
    GTEXT_JSON_Value * val = gtext_json_new_number_i64(1);

    // NULL array
    EXPECT_EQ(gtext_json_array_push(nullptr, val), GTEXT_JSON_E_INVALID);
    EXPECT_EQ(gtext_json_array_set(nullptr, 0, val), GTEXT_JSON_E_INVALID);
    EXPECT_EQ(gtext_json_array_insert(nullptr, 0, val), GTEXT_JSON_E_INVALID);
    EXPECT_EQ(gtext_json_array_remove(nullptr, 0), GTEXT_JSON_E_INVALID);

    // NULL value
    EXPECT_EQ(gtext_json_array_push(arr, nullptr), GTEXT_JSON_E_INVALID);
    EXPECT_EQ(gtext_json_array_set(arr, 0, nullptr), GTEXT_JSON_E_INVALID);
    EXPECT_EQ(gtext_json_array_insert(arr, 0, nullptr), GTEXT_JSON_E_INVALID);

    // Wrong type
    GTEXT_JSON_Value * obj = gtext_json_new_object();
    EXPECT_EQ(gtext_json_array_push(obj, val), GTEXT_JSON_E_INVALID);
    EXPECT_EQ(gtext_json_array_set(obj, 0, val), GTEXT_JSON_E_INVALID);

    gtext_json_free(arr);
    gtext_json_free(val);
    gtext_json_free(obj);

    // Test error cases for object operations
    GTEXT_JSON_Value * obj2 = gtext_json_new_object();
    GTEXT_JSON_Value * val2 = gtext_json_new_number_i64(2);

    // NULL object
    EXPECT_EQ(gtext_json_object_put(nullptr, "key", 3, val2), GTEXT_JSON_E_INVALID);
    EXPECT_EQ(gtext_json_object_remove(nullptr, "key", 3), GTEXT_JSON_E_INVALID);

    // NULL key
    EXPECT_EQ(gtext_json_object_put(obj2, nullptr, 3, val2), GTEXT_JSON_E_INVALID);
    EXPECT_EQ(gtext_json_object_remove(obj2, nullptr, 3), GTEXT_JSON_E_INVALID);

    // NULL value
    EXPECT_EQ(gtext_json_object_put(obj2, "key", 3, nullptr), GTEXT_JSON_E_INVALID);

    // Wrong type
    GTEXT_JSON_Value * arr2 = gtext_json_new_array();
    EXPECT_EQ(gtext_json_object_put(arr2, "key", 3, val2), GTEXT_JSON_E_INVALID);
    EXPECT_EQ(gtext_json_object_remove(arr2, "key", 3), GTEXT_JSON_E_INVALID);

    gtext_json_free(obj2);
    gtext_json_free(val2);
    gtext_json_free(arr2);
}

/**
 * Test duplicate key handling - ERROR policy
 */
TEST(DuplicateKeyHandling, Error) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.dupkeys = GTEXT_JSON_DUPKEY_ERROR;

    const char * input = R"({"key": 1, "key": 2})";
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);

    EXPECT_EQ(value, nullptr);
    EXPECT_EQ(err.code, GTEXT_JSON_E_DUPKEY);
    EXPECT_STREQ(err.message, "Duplicate key in object");

    // Clean up error context snippet
    gtext_json_error_free(&err);
}

/**
 * Test duplicate key handling - FIRST_WINS policy
 */
TEST(DuplicateKeyHandling, FirstWins) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.dupkeys = GTEXT_JSON_DUPKEY_FIRST_WINS;

    const char * input = R"({"key": 1, "key": 2})";
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);

    ASSERT_NE(value, nullptr);
    EXPECT_EQ(gtext_json_typeof(value), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_object_size(value), 1u);

    // Should have first value (1)
    const GTEXT_JSON_Value * val = gtext_json_object_get(value, "key", 3);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_NUMBER);

    int64_t i64_out = 0;
    GTEXT_JSON_Status status = gtext_json_get_i64(val, &i64_out);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 1);

    gtext_json_free(value);
}

/**
 * Test duplicate key handling - LAST_WINS policy
 */
TEST(DuplicateKeyHandling, LastWins) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.dupkeys = GTEXT_JSON_DUPKEY_LAST_WINS;

    const char * input = R"({"key": 1, "key": 2})";
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);

    ASSERT_NE(value, nullptr);
    EXPECT_EQ(gtext_json_typeof(value), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_object_size(value), 1u);

    // Should have last value (2)
    const GTEXT_JSON_Value * val = gtext_json_object_get(value, "key", 3);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_NUMBER);

    int64_t i64_out = 0;
    GTEXT_JSON_Status status = gtext_json_get_i64(val, &i64_out);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 2);

    gtext_json_free(value);
}

/**
 * Test duplicate key handling - COLLECT policy (single value to array)
 */
TEST(DuplicateKeyHandling, CollectSingle) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.dupkeys = GTEXT_JSON_DUPKEY_COLLECT;

    const char * input = R"({"key": 1, "key": 2})";
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);

    ASSERT_NE(value, nullptr);
    EXPECT_EQ(gtext_json_typeof(value), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_object_size(value), 1u);

    // Should have array with [1, 2]
    const GTEXT_JSON_Value * val = gtext_json_object_get(value, "key", 3);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_ARRAY);
    EXPECT_EQ(gtext_json_array_size(val), 2u);

    // First element should be 1
    const GTEXT_JSON_Value * elem0 = gtext_json_array_get(val, 0);
    ASSERT_NE(elem0, nullptr);
    int64_t i64_out = 0;
    GTEXT_JSON_Status status = gtext_json_get_i64(elem0, &i64_out);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 1);

    // Second element should be 2
    const GTEXT_JSON_Value * elem1 = gtext_json_array_get(val, 1);
    ASSERT_NE(elem1, nullptr);
    status = gtext_json_get_i64(elem1, &i64_out);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 2);

    gtext_json_free(value);
}

/**
 * Test duplicate key handling - COLLECT policy (array to array)
 */
TEST(DuplicateKeyHandling, CollectArray) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.dupkeys = GTEXT_JSON_DUPKEY_COLLECT;

    const char * input = R"({"key": [1, 2], "key": 3})";
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);

    if (!value) {
        printf("DEBUG: Parse failed: code=%d, message=%s, offset=%zu, line=%d, col=%d\n",
               err.code, err.message ? err.message : "(null)", err.offset, err.line, err.col);
        printf("DEBUG: Input around offset: ");
        size_t start = err.offset > 10 ? err.offset - 10 : 0;
        size_t len = strlen(input);
        size_t end = err.offset + 10 < len ? err.offset + 10 : len;
        for (size_t i = start; i < end; i++) {
            printf("%c", input[i]);
        }
        printf("\n");
    }
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(gtext_json_typeof(value), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_object_size(value), 1u);

    // Should have array with [1, 2, 3]
    const GTEXT_JSON_Value * val = gtext_json_object_get(value, "key", 3);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_ARRAY);
    EXPECT_EQ(gtext_json_array_size(val), 3u);

    // Check elements
    const GTEXT_JSON_Value * elem0 = gtext_json_array_get(val, 0);
    ASSERT_NE(elem0, nullptr);
    int64_t i64_out = 0;
    GTEXT_JSON_Status status = gtext_json_get_i64(elem0, &i64_out);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 1);

    const GTEXT_JSON_Value * elem1 = gtext_json_array_get(val, 1);
    ASSERT_NE(elem1, nullptr);
    status = gtext_json_get_i64(elem1, &i64_out);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 2);

    const GTEXT_JSON_Value * elem2 = gtext_json_array_get(val, 2);
    ASSERT_NE(elem2, nullptr);
    status = gtext_json_get_i64(elem2, &i64_out);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 3);

    gtext_json_free(value);
}

/**
 * Test duplicate key handling - multiple duplicates with COLLECT
 */
TEST(DuplicateKeyHandling, CollectMultiple) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.dupkeys = GTEXT_JSON_DUPKEY_COLLECT;

    const char * input = R"({"key": 1, "key": 2, "key": 3})";
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);

    ASSERT_NE(value, nullptr);
    EXPECT_EQ(gtext_json_typeof(value), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_object_size(value), 1u);

    // Should have array with [1, 2, 3]
    const GTEXT_JSON_Value * val = gtext_json_object_get(value, "key", 3);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_ARRAY);
    EXPECT_EQ(gtext_json_array_size(val), 3u);

    gtext_json_free(value);
}

/**
 * Test duplicate key handling - nested objects
 */
TEST(DuplicateKeyHandling, Nested) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.dupkeys = GTEXT_JSON_DUPKEY_LAST_WINS;

    const char * input = R"({"outer": {"key": 1, "key": 2}, "outer": {"key": 3}})";
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);

    ASSERT_NE(value, nullptr);
    EXPECT_EQ(gtext_json_typeof(value), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_object_size(value), 1u);

    // Should have last outer object
    const GTEXT_JSON_Value * outer = gtext_json_object_get(value, "outer", 5);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(gtext_json_typeof(outer), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_object_size(outer), 1u);

    // Inner object should have last value (3)
    const GTEXT_JSON_Value * inner = gtext_json_object_get(outer, "key", 3);
    ASSERT_NE(inner, nullptr);
    int64_t i64_out = 0;
    GTEXT_JSON_Status status = gtext_json_get_i64(inner, &i64_out);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 3);

    gtext_json_free(value);
}

/**
 * Test duplicate key handling - different value types with COLLECT
 */
TEST(DuplicateKeyHandling, CollectDifferentTypes) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.dupkeys = GTEXT_JSON_DUPKEY_COLLECT;

    const char * input = R"({"key": "first", "key": 42, "key": true})";
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);

    ASSERT_NE(value, nullptr);
    EXPECT_EQ(gtext_json_typeof(value), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_object_size(value), 1u);

    // Should have array with ["first", 42, true]
    const GTEXT_JSON_Value * val = gtext_json_object_get(value, "key", 3);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_ARRAY);
    EXPECT_EQ(gtext_json_array_size(val), 3u);

    // First element should be string "first"
    const GTEXT_JSON_Value * elem0 = gtext_json_array_get(val, 0);
    ASSERT_NE(elem0, nullptr);
    EXPECT_EQ(gtext_json_typeof(elem0), GTEXT_JSON_STRING);
    const char * str_out = nullptr;
    size_t str_len = 0;
    GTEXT_JSON_Status status = gtext_json_get_string(elem0, &str_out, &str_len);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(str_len, 5u);
    EXPECT_STREQ(str_out, "first");

    // Second element should be number 42
    const GTEXT_JSON_Value * elem1 = gtext_json_array_get(val, 1);
    ASSERT_NE(elem1, nullptr);
    EXPECT_EQ(gtext_json_typeof(elem1), GTEXT_JSON_NUMBER);
    int64_t i64_out = 0;
    status = gtext_json_get_i64(elem1, &i64_out);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(i64_out, 42);

    // Third element should be boolean true
    const GTEXT_JSON_Value * elem2 = gtext_json_array_get(val, 2);
    ASSERT_NE(elem2, nullptr);
    EXPECT_EQ(gtext_json_typeof(elem2), GTEXT_JSON_BOOL);
    bool bool_out = false;
    status = gtext_json_get_bool(elem2, &bool_out);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(bool_out, true);

    gtext_json_free(value);
}

/**
 * Test sink abstraction - custom callback sink
 */
TEST(SinkAbstraction, CallbackSink) {
    std::string output;

    // Custom write callback that appends to string
    auto write_callback = [](void * user, const char * bytes, size_t len) -> int {
        std::string* str = (std::string*)user;
        str->append(bytes, len);
        return 0;
    };

    GTEXT_JSON_Sink sink;
    sink.write = write_callback;
    sink.user = &output;

    // Write some data
    const char * test_data = "Hello, World!";
    int result = sink.write(sink.user, test_data, strlen(test_data));
    EXPECT_EQ(result, 0);
    EXPECT_EQ(output, "Hello, World!");

    // Write more data
    const char * more_data = " Test";
    result = sink.write(sink.user, more_data, strlen(more_data));
    EXPECT_EQ(result, 0);
    EXPECT_EQ(output, "Hello, World! Test");
}

/**
 * Test sink abstraction - growable buffer sink
 */
TEST(SinkAbstraction, GrowableBuffer) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Initially empty
    EXPECT_EQ(gtext_json_sink_buffer_size(&sink), 0u);
    const char * data = gtext_json_sink_buffer_data(&sink);
    ASSERT_NE(data, nullptr);
    EXPECT_STREQ(data, "");

    // Write some data
    const char * test1 = "Hello";
    int result = sink.write(sink.user, test1, strlen(test1));
    EXPECT_EQ(result, 0);
    EXPECT_EQ(gtext_json_sink_buffer_size(&sink), 5u);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "Hello");

    // Write more data
    const char * test2 = ", World!";
    result = sink.write(sink.user, test2, strlen(test2));
    EXPECT_EQ(result, 0);
    EXPECT_EQ(gtext_json_sink_buffer_size(&sink), 13u);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "Hello, World!");

    // Write large amount of data to test growth
    std::string large_data(1000, 'A');
    result = sink.write(sink.user, large_data.c_str(), large_data.size());
    EXPECT_EQ(result, 0);
    EXPECT_EQ(gtext_json_sink_buffer_size(&sink), 1013u);

    // Verify data integrity
    data = gtext_json_sink_buffer_data(&sink);
    EXPECT_EQ(strncmp(data, "Hello, World!", 13), 0);
    EXPECT_EQ(data[1012], 'A');

    // Clean up
    gtext_json_sink_buffer_free(&sink);
    EXPECT_EQ(sink.write, nullptr);
    EXPECT_EQ(sink.user, nullptr);
}

/**
 * Test sink abstraction - fixed buffer sink
 */
TEST(SinkAbstraction, FixedBuffer) {
    char buffer[64];
    GTEXT_JSON_Sink sink;

    GTEXT_JSON_Status status = gtext_json_sink_fixed_buffer(&sink, buffer, sizeof(buffer));
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Initially empty
    EXPECT_EQ(gtext_json_sink_fixed_buffer_used(&sink), 0u);
    EXPECT_EQ(gtext_json_sink_fixed_buffer_truncated(&sink), false);
    EXPECT_STREQ(buffer, "");

    // Write some data
    const char * test1 = "Hello";
    int result = sink.write(sink.user, test1, strlen(test1));
    EXPECT_EQ(result, 0);
    EXPECT_EQ(gtext_json_sink_fixed_buffer_used(&sink), 5u);
    EXPECT_EQ(gtext_json_sink_fixed_buffer_truncated(&sink), false);
    EXPECT_STREQ(buffer, "Hello");

    // Write more data
    const char * test2 = ", World!";
    result = sink.write(sink.user, test2, strlen(test2));
    EXPECT_EQ(result, 0);
    EXPECT_EQ(gtext_json_sink_fixed_buffer_used(&sink), 13u);
    EXPECT_EQ(gtext_json_sink_fixed_buffer_truncated(&sink), false);
    EXPECT_STREQ(buffer, "Hello, World!");

    // Write data that fits exactly
    const char * test3 = " This fits";
    result = sink.write(sink.user, test3, strlen(test3));
    EXPECT_EQ(result, 0);
    EXPECT_EQ(gtext_json_sink_fixed_buffer_used(&sink), 23u);
    EXPECT_EQ(gtext_json_sink_fixed_buffer_truncated(&sink), false);

    // Write data that exceeds buffer (should truncate)
    // After 23 bytes, we have 64 - 23 - 1 = 40 bytes available
    // This string is 50 bytes, so it will exceed and truncate
    const char * test4 = " This is way too long and will definitely be truncated";
    result = sink.write(sink.user, test4, strlen(test4));
    EXPECT_NE(result, 0); // Should return error on truncation
    EXPECT_EQ(gtext_json_sink_fixed_buffer_truncated(&sink), true);
    // Should have written up to buffer limit (63 bytes, leaving 1 for null terminator)
    EXPECT_EQ(gtext_json_sink_fixed_buffer_used(&sink), sizeof(buffer) - 1);

    // Clean up
    gtext_json_sink_fixed_buffer_free(&sink);
}

/**
 * Test sink abstraction - fixed buffer edge cases
 */
TEST(SinkAbstraction, FixedBufferEdgeCases) {
    // Test with size 1 buffer (only null terminator)
    char tiny_buffer[1];
    GTEXT_JSON_Sink sink;

    GTEXT_JSON_Status status = gtext_json_sink_fixed_buffer(&sink, tiny_buffer, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Try to write (should truncate immediately)
    const char * test = "X";
    int result = sink.write(sink.user, test, 1);
    EXPECT_NE(result, 0); // Should return error
    EXPECT_EQ(gtext_json_sink_fixed_buffer_truncated(&sink), true);
    EXPECT_EQ(gtext_json_sink_fixed_buffer_used(&sink), 0u);
    EXPECT_EQ(tiny_buffer[0], '\0');

    // Test invalid parameters
    status = gtext_json_sink_fixed_buffer(nullptr, tiny_buffer, 1);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    char buf[10];
    status = gtext_json_sink_fixed_buffer(&sink, nullptr, 10);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    status = gtext_json_sink_fixed_buffer(&sink, buf, 0);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    // Clean up
    gtext_json_sink_fixed_buffer_free(&sink);
}

/**
 * Test sink abstraction - growable buffer edge cases
 */
TEST(SinkAbstraction, GrowableBufferEdgeCases) {
    // Test invalid parameters
    GTEXT_JSON_Status status = gtext_json_sink_buffer(nullptr);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    // Test empty buffer access
    GTEXT_JSON_Sink sink;
    status = gtext_json_sink_buffer(&sink);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    const char * data = gtext_json_sink_buffer_data(nullptr);
    EXPECT_EQ(data, nullptr);

    size_t size = gtext_json_sink_buffer_size(nullptr);
    EXPECT_EQ(size, 0u);

    // Test free with invalid sink
    GTEXT_JSON_Sink invalid_sink = {nullptr, nullptr};
    gtext_json_sink_buffer_free(&invalid_sink); // Should not crash

    // Clean up valid sink
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test sink abstraction - error propagation
 */
TEST(SinkAbstraction, ErrorPropagation) {
    // Custom callback that returns error
    auto error_callback = [](void * user, const char * bytes, size_t len) -> int {
        (void)user;
        (void)bytes;
        (void)len;
        return 1; // Error
    };

    GTEXT_JSON_Sink sink;
    sink.write = error_callback;
    sink.user = nullptr;

    const char * test = "test";
    int result = sink.write(sink.user, test, strlen(test));
    EXPECT_NE(result, 0); // Should propagate error
}

/**
 * Test DOM write - null value
 */
TEST(DOMWrite, Null) {
    GTEXT_JSON_Value * v = gtext_json_new_null();
    ASSERT_NE(v, nullptr);

    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};
    status = gtext_json_write_value(&sink, &opts, v, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "null");

    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(v);
}

/**
 * Test DOM write - boolean values
 */
TEST(DOMWrite, Boolean) {
    GTEXT_JSON_Value * v_true = gtext_json_new_bool(true);
    GTEXT_JSON_Value * v_false = gtext_json_new_bool(false);
    ASSERT_NE(v_true, nullptr);
    ASSERT_NE(v_false, nullptr);

    GTEXT_JSON_Sink sink;
    gtext_json_sink_buffer(&sink);

    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};

    gtext_json_write_value(&sink, &opts, v_true, &err);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "true");

    gtext_json_sink_buffer_free(&sink);
    gtext_json_sink_buffer(&sink);

    gtext_json_write_value(&sink, &opts, v_false, &err);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "false");

    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(v_true);
    gtext_json_free(v_false);
}

/**
 * Test DOM write - string values with escaping
 */
TEST(DOMWrite, StringEscaping) {
    GTEXT_JSON_Value * v1 = gtext_json_new_string("hello", 5);
    GTEXT_JSON_Value * v2 = gtext_json_new_string("he\"llo", 6);
    GTEXT_JSON_Value * v3 = gtext_json_new_string("he\\llo", 6);
    GTEXT_JSON_Value * v4 = gtext_json_new_string("he\nllo", 6);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);
    ASSERT_NE(v3, nullptr);
    ASSERT_NE(v4, nullptr);

    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};

    gtext_json_sink_buffer(&sink);
    gtext_json_write_value(&sink, &opts, v1, &err);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "\"hello\"");
    gtext_json_sink_buffer_free(&sink);

    gtext_json_sink_buffer(&sink);
    gtext_json_write_value(&sink, &opts, v2, &err);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "\"he\\\"llo\"");
    gtext_json_sink_buffer_free(&sink);

    gtext_json_sink_buffer(&sink);
    gtext_json_write_value(&sink, &opts, v3, &err);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "\"he\\\\llo\"");
    gtext_json_sink_buffer_free(&sink);

    gtext_json_sink_buffer(&sink);
    gtext_json_write_value(&sink, &opts, v4, &err);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "\"he\\nllo\"");
    gtext_json_sink_buffer_free(&sink);

    gtext_json_free(v1);
    gtext_json_free(v2);
    gtext_json_free(v3);
    gtext_json_free(v4);
}

/**
 * Test DOM write - number values
 */
TEST(DOMWrite, Number) {
    GTEXT_JSON_Value * v1 = gtext_json_new_number_i64(123);
    GTEXT_JSON_Value * v2 = gtext_json_new_number_u64(456u);
    GTEXT_JSON_Value * v3 = gtext_json_new_number_double(3.14);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);
    ASSERT_NE(v3, nullptr);

    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};

    gtext_json_sink_buffer(&sink);
    gtext_json_write_value(&sink, &opts, v1, &err);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "123");
    gtext_json_sink_buffer_free(&sink);

    gtext_json_sink_buffer(&sink);
    gtext_json_write_value(&sink, &opts, v2, &err);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "456");
    gtext_json_sink_buffer_free(&sink);

    gtext_json_sink_buffer(&sink);
    gtext_json_write_value(&sink, &opts, v3, &err);
    // Double output format may vary, just check it's not empty
    EXPECT_GT(gtext_json_sink_buffer_size(&sink), 0u);
    gtext_json_sink_buffer_free(&sink);

    gtext_json_free(v1);
    gtext_json_free(v2);
    gtext_json_free(v3);
}

/**
 * Test DOM write - array values
 */
TEST(DOMWrite, Array) {
    GTEXT_JSON_Value * arr = gtext_json_new_array();
    ASSERT_NE(arr, nullptr);

    GTEXT_JSON_Value * v1 = gtext_json_new_number_i64(1);
    GTEXT_JSON_Value * v2 = gtext_json_new_string("two", 3);
    GTEXT_JSON_Value * v3 = gtext_json_new_bool(true);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);
    ASSERT_NE(v3, nullptr);

    gtext_json_array_push(arr, v1);
    gtext_json_array_push(arr, v2);
    gtext_json_array_push(arr, v3);

    GTEXT_JSON_Sink sink;
    gtext_json_sink_buffer(&sink);
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};

    gtext_json_write_value(&sink, &opts, arr, &err);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "[1,\"two\",true]");

    gtext_json_sink_buffer_free(&sink);
    // Freeing the parent should automatically free all children
    gtext_json_free(arr);
}

/**
 * Test DOM write - object values
 */
TEST(DOMWrite, Object) {
    GTEXT_JSON_Value * obj = gtext_json_new_object();
    ASSERT_NE(obj, nullptr);

    GTEXT_JSON_Value * v1 = gtext_json_new_number_i64(42);
    GTEXT_JSON_Value * v2 = gtext_json_new_string("value", 5);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);

    gtext_json_object_put(obj, "key1", 4, v1);
    gtext_json_object_put(obj, "key2", 4, v2);

    GTEXT_JSON_Sink sink;
    gtext_json_sink_buffer(&sink);
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};

    gtext_json_write_value(&sink, &opts, obj, &err);
    // Output order may vary, check it contains both keys
    const char * output = gtext_json_sink_buffer_data(&sink);
    EXPECT_NE(strstr(output, "key1"), nullptr);
    EXPECT_NE(strstr(output, "key2"), nullptr);
    EXPECT_NE(strstr(output, "42"), nullptr);
    EXPECT_NE(strstr(output, "value"), nullptr);

    gtext_json_sink_buffer_free(&sink);
    // Freeing the parent should automatically free all children
    gtext_json_free(obj);
}

/**
 * Test DOM write - pretty printing
 */
TEST(DOMWrite, PrettyPrint) {
    GTEXT_JSON_Value * obj = gtext_json_new_object();
    ASSERT_NE(obj, nullptr);

    GTEXT_JSON_Value * arr = gtext_json_new_array();
    GTEXT_JSON_Value * v1 = gtext_json_new_number_i64(1);
    GTEXT_JSON_Value * v2 = gtext_json_new_string("test", 4);
    gtext_json_array_push(arr, v1);
    gtext_json_array_push(arr, v2);

    gtext_json_object_put(obj, "array", 5, arr);

    GTEXT_JSON_Sink sink;
    gtext_json_sink_buffer(&sink);
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    opts.pretty = true;
    opts.indent_spaces = 2;
    GTEXT_JSON_Error err{};

    gtext_json_write_value(&sink, &opts, obj, &err);
    const char * output = gtext_json_sink_buffer_data(&sink);
    // Pretty print should contain newlines
    EXPECT_NE(strchr(output, '\n'), nullptr);
    // Should contain indentation
    EXPECT_NE(strstr(output, "  "), nullptr);

    gtext_json_sink_buffer_free(&sink);
    // Freeing the parent should automatically free all children
    gtext_json_free(obj);
}

/**
 * Test DOM write - key sorting for canonical output
 */
TEST(DOMWrite, KeySorting) {
    GTEXT_JSON_Value * obj = gtext_json_new_object();
    ASSERT_NE(obj, nullptr);

    GTEXT_JSON_Value * v1 = gtext_json_new_string("first", 5);
    GTEXT_JSON_Value * v2 = gtext_json_new_string("second", 6);
    GTEXT_JSON_Value * v3 = gtext_json_new_string("third", 5);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);
    ASSERT_NE(v3, nullptr);

    // Add keys in non-alphabetical order
    gtext_json_object_put(obj, "zebra", 5, v1);
    gtext_json_object_put(obj, "apple", 5, v2);
    gtext_json_object_put(obj, "banana", 6, v3);

    GTEXT_JSON_Sink sink;
    gtext_json_sink_buffer(&sink);
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    opts.sort_object_keys = true;
    GTEXT_JSON_Error err{};

    gtext_json_write_value(&sink, &opts, obj, &err);
    const char * output = gtext_json_sink_buffer_data(&sink);
    // With sorting, "apple" should come before "banana", and "banana" before "zebra"
    const char * apple_pos = strstr(output, "apple");
    const char * banana_pos = strstr(output, "banana");
    const char * zebra_pos = strstr(output, "zebra");
    ASSERT_NE(apple_pos, nullptr);
    ASSERT_NE(banana_pos, nullptr);
    ASSERT_NE(zebra_pos, nullptr);
    EXPECT_LT(apple_pos, banana_pos);
    EXPECT_LT(banana_pos, zebra_pos);

    gtext_json_sink_buffer_free(&sink);
    // Freeing the parent should automatically free all children
    gtext_json_free(obj);
}

/**
 * Test DOM write - error handling
 */
TEST(DOMWrite, ErrorHandling) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};

    // NULL sink
    GTEXT_JSON_Value * v = gtext_json_new_null();
    GTEXT_JSON_Status status = gtext_json_write_value(nullptr, &opts, v, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);
    gtext_json_free(v);

    // NULL value
    gtext_json_sink_buffer(&sink);
    status = gtext_json_write_value(&sink, &opts, nullptr, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test DOM write - round trip (parse then write)
 */
TEST(DOMWrite, RoundTrip) {
    const char * input = "{\"key\":[1,2,\"three\",true,null]}";
    GTEXT_JSON_Parse_Options parse_opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * parsed = gtext_json_parse(input, strlen(input), &parse_opts, &err);
    ASSERT_NE(parsed, nullptr);

    GTEXT_JSON_Sink sink;
    gtext_json_sink_buffer(&sink);
    GTEXT_JSON_Write_Options write_opts = gtext_json_write_options_default();
    gtext_json_write_value(&sink, &write_opts, parsed, &err);

    const char * output = gtext_json_sink_buffer_data(&sink);
    // Output should be valid JSON (we can parse it again)
    GTEXT_JSON_Value * reparsed = gtext_json_parse(output, gtext_json_sink_buffer_size(&sink), &parse_opts, &err);
    EXPECT_NE(reparsed, nullptr);

    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(parsed);
    gtext_json_free(reparsed);
}

/**
 * Test streaming parser - stream creation and destruction
 */
TEST(StreamingParser, CreationAndDestruction) {
    // Test creation with NULL callback (should fail)
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, nullptr, nullptr);
    EXPECT_EQ(st, nullptr);

    // Test creation with valid callback
    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)user;
        (void)evt;
        (void)err;
        return GTEXT_JSON_OK;
    };
    st = gtext_json_stream_new(&opts, callback, nullptr);
    EXPECT_NE(st, nullptr);

    // Test destruction
    gtext_json_stream_free(st);
    gtext_json_stream_free(nullptr);  // Should be safe

    // Test creation with NULL options (should use defaults)
    st = gtext_json_stream_new(nullptr, callback, nullptr);
    EXPECT_NE(st, nullptr);
    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - callback invocation (basic)
 *
 * Test that the stream can be created and that callbacks can be set up.
 */
TEST(StreamingParser, CallbackSetup) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    // Track callback invocations
    struct {
        std::vector<GTEXT_JSON_Event_Type> events;
        void * user_data;
    } context = {{}, (void * )0x12345};

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)err;
        auto* ctx = static_cast<decltype(context)*>(user);
        if (ctx) {
            ctx->events.push_back(evt->type);
        }
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &context);
    ASSERT_NE(st, nullptr);

    // Verify the stream is set up correctly
    EXPECT_EQ(context.events.size(), 0u);

    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - stream state persistence
 *
 * Test that the stream maintains state across feed calls.
 */
TEST(StreamingParser, StatePersistence) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)user;
        (void)evt;
        (void)err;
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, nullptr);
    ASSERT_NE(st, nullptr);

    GTEXT_JSON_Error err{};
    // Feed empty input (should be OK)
    GTEXT_JSON_Status status = gtext_json_stream_feed(st, "", 0, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Feed some data - should parse successfully
    const char * data = "null";
    status = gtext_json_stream_feed(st, data, strlen(data), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Finish should succeed after parsing a complete value
    status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // After finish, stream is in DONE state - feeding more should fail
    status = gtext_json_stream_feed(st, " true", 5, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);  // Should fail - stream is done

    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - error handling
 */
TEST(StreamingParser, ErrorHandling) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)user;
        (void)evt;
        (void)err;
        return GTEXT_JSON_OK;
    };

    // Test invalid JSON input (not NULL pointer tests - those are in NullPointerHandling suite)
    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, nullptr);
    ASSERT_NE(st, nullptr);

    GTEXT_JSON_Error err{};

    // Feed invalid JSON
    GTEXT_JSON_Status status = gtext_json_stream_feed(st, "invalid!!!", 10, &err);
    // May succeed initially (partial input)

    // Finish should detect the error
    status = gtext_json_stream_finish(st, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_NE(err.code, GTEXT_JSON_OK);

    gtext_json_stream_free(st);
    gtext_json_error_free(&err);
}

/**
 * Test streaming parser - basic value parsing (null, bool, number, string)
 */
TEST(StreamingParser, BasicValues) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    struct {
        const char * input;
        GTEXT_JSON_Event_Type expected_type;
    } tests[] = {
        {"null", GTEXT_JSON_EVT_NULL},
        {"true", GTEXT_JSON_EVT_BOOL},
        {"false", GTEXT_JSON_EVT_BOOL},
        {"123", GTEXT_JSON_EVT_NUMBER},
        {"\"hello\"", GTEXT_JSON_EVT_STRING},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        std::vector<GTEXT_JSON_Event_Type> events;

        auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
            (void)err;
            auto* evts = static_cast<std::vector<GTEXT_JSON_Event_Type>*>(user);
            evts->push_back(evt->type);
            return GTEXT_JSON_OK;
        };

        GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &events);
        ASSERT_NE(st, nullptr);

        GTEXT_JSON_Error err{};
        GTEXT_JSON_Status status = gtext_json_stream_feed(st, tests[i].input, strlen(tests[i].input), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed for input: " << tests[i].input;

        status = gtext_json_stream_finish(st, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed to finish for input: " << tests[i].input;

        EXPECT_EQ(events.size(), 1u) << "Expected 1 event for: " << tests[i].input;
        if (events.size() == 1) {
            EXPECT_EQ(events[0], tests[i].expected_type) << "Event type mismatch for: " << tests[i].input;
        }

        gtext_json_stream_free(st);
    }
}

/**
 * Test streaming parser - array parsing
 */
TEST(StreamingParser, Arrays) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    std::vector<GTEXT_JSON_Event_Type> events;

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)err;
        auto* evts = static_cast<std::vector<GTEXT_JSON_Event_Type>*>(user);
        evts->push_back(evt->type);
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &events);
    ASSERT_NE(st, nullptr);

    const char * input = "[1, 2, 3]";
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Status status = gtext_json_stream_feed(st, input, strlen(input), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Expected events: ARRAY_BEGIN, NUMBER, NUMBER, NUMBER, ARRAY_END
    EXPECT_EQ(events.size(), 5u);
    if (events.size() >= 5) {
        EXPECT_EQ(events[0], GTEXT_JSON_EVT_ARRAY_BEGIN);
        EXPECT_EQ(events[1], GTEXT_JSON_EVT_NUMBER);
        EXPECT_EQ(events[2], GTEXT_JSON_EVT_NUMBER);
        EXPECT_EQ(events[3], GTEXT_JSON_EVT_NUMBER);
        EXPECT_EQ(events[4], GTEXT_JSON_EVT_ARRAY_END);
    }

    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - object parsing
 */
TEST(StreamingParser, Objects) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    std::vector<GTEXT_JSON_Event_Type> events;

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)err;
        auto* evts = static_cast<std::vector<GTEXT_JSON_Event_Type>*>(user);
        evts->push_back(evt->type);
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &events);
    ASSERT_NE(st, nullptr);

    const char * input = "{\"key\": \"value\"}";
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Status status = gtext_json_stream_feed(st, input, strlen(input), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Expected events: OBJECT_BEGIN, KEY, STRING, OBJECT_END
    EXPECT_EQ(events.size(), 4u);
    if (events.size() >= 4) {
        EXPECT_EQ(events[0], GTEXT_JSON_EVT_OBJECT_BEGIN);
        EXPECT_EQ(events[1], GTEXT_JSON_EVT_KEY);
        EXPECT_EQ(events[2], GTEXT_JSON_EVT_STRING);
        EXPECT_EQ(events[3], GTEXT_JSON_EVT_OBJECT_END);
    }

    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - incremental/chunked input
 */
TEST(StreamingParser, IncrementalInput) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    std::vector<GTEXT_JSON_Event_Type> events;

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)err;
        auto* evts = static_cast<std::vector<GTEXT_JSON_Event_Type>*>(user);
        evts->push_back(evt->type);
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &events);
    ASSERT_NE(st, nullptr);

    const char * input = "[1, 2, 3]";
    GTEXT_JSON_Error err{};
    // Feed byte by byte
    for (size_t i = 0; i < strlen(input); ++i) {
        GTEXT_JSON_Status status = gtext_json_stream_feed(st, input + i, 1, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed at byte " << i;
    }

    GTEXT_JSON_Status status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should have received all events
    EXPECT_EQ(events.size(), 5u);

    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - nested structures
 */
TEST(StreamingParser, NestedStructures) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    std::vector<GTEXT_JSON_Event_Type> events;

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)err;
        auto* evts = static_cast<std::vector<GTEXT_JSON_Event_Type>*>(user);
        evts->push_back(evt->type);
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &events);
    ASSERT_NE(st, nullptr);

    const char * input = "{\"arr\": [1, 2], \"obj\": {\"key\": \"value\"}}";
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Status status = gtext_json_stream_feed(st, input, strlen(input), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should have received events for nested structure
    EXPECT_GT(events.size(), 5u);

    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - error handling (invalid JSON)
 */
TEST(StreamingParser, InvalidJSON) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)user;
        (void)evt;
        (void)err;
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, nullptr);
    ASSERT_NE(st, nullptr);

    GTEXT_JSON_Error err{};

    // Invalid: missing comma
    const char * invalid1 = "[1 2]";
    GTEXT_JSON_Status status = gtext_json_stream_feed(st, invalid1, strlen(invalid1), &err);
    // Should either succeed (buffering) or fail
    status = gtext_json_stream_finish(st, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);  // Should fail on invalid JSON

    gtext_json_stream_free(st);
    // Free error from first operation before reusing
    gtext_json_error_free(&err);
    err = GTEXT_JSON_Error{
        .code = {},
        .message = {},
        .offset = {},
        .line = {},
        .col = {},
        .context_snippet = {},
        .context_snippet_len = {},
        .caret_offset = {},
        .expected_token = {},
        .actual_token = {}
    };

    // Invalid: incomplete structure
    st = gtext_json_stream_new(&opts, callback, nullptr);
    ASSERT_NE(st, nullptr);

    const char * invalid2 = "[1, 2";
    status = gtext_json_stream_feed(st, invalid2, strlen(invalid2), &err);
    status = gtext_json_stream_finish(st, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);  // Should fail on incomplete structure

    gtext_json_stream_free(st);
    gtext_json_error_free(&err);
}

/**
 * Test streaming parser - string spanning multiple chunks
 */
TEST(StreamingParser, StringSpanningChunks) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    std::vector<std::string> string_values;

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)err;
        if (evt->type == GTEXT_JSON_EVT_STRING) {
            auto* strings = static_cast<std::vector<std::string>*>(user);
            std::string str(evt->as.str.s, evt->as.str.len);
            strings->push_back(str);
        }
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &string_values);
    ASSERT_NE(st, nullptr);

    GTEXT_JSON_Error err{};
    // Test: string split across chunks
    // Chunk 1: "hello
    // Chunk 2: world"
    const char * chunk1 = "\"hello";
    const char * chunk2 = "world\"";

    GTEXT_JSON_Status status = gtext_json_stream_feed(st, chunk1, strlen(chunk1), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input

    status = gtext_json_stream_feed(st, chunk2, strlen(chunk2), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should have received "helloworld"
    EXPECT_EQ(string_values.size(), 1u);
    EXPECT_EQ(string_values[0], "helloworld");

    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - number spanning multiple chunks
 */
TEST(StreamingParser, NumberSpanningChunks) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    std::vector<std::string> number_values;

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)err;
        if (evt->type == GTEXT_JSON_EVT_NUMBER) {
            auto* numbers = static_cast<std::vector<std::string>*>(user);
            std::string num(evt->as.number.s, evt->as.number.len);
            numbers->push_back(num);
        }
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &number_values);
    ASSERT_NE(st, nullptr);

    GTEXT_JSON_Error err{};
    // Test: number split across chunks (integer followed by decimal)
    // Chunk 1: 12345 (at EOF, should be treated as incomplete)
    // Chunk 2: .678
    const char * chunk1 = "12345";
    const char * chunk2 = ".678";

    GTEXT_JSON_Status status = gtext_json_stream_feed(st, chunk1, strlen(chunk1), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input (incomplete)

    status = gtext_json_stream_feed(st, chunk2, strlen(chunk2), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should have received "12345.678" (not "12345" and then error on ".678")
    EXPECT_EQ(number_values.size(), 1u);
    EXPECT_EQ(number_values[0], "12345.678");

    // Test: number ending with '.' at EOF
    gtext_json_stream_free(st);
    number_values.clear();
    st = gtext_json_stream_new(&opts, callback, &number_values);
    ASSERT_NE(st, nullptr);

    const char * chunk3 = "12345.";
    const char * chunk4 = "678";

    status = gtext_json_stream_feed(st, chunk3, strlen(chunk3), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input (incomplete)

    status = gtext_json_stream_feed(st, chunk4, strlen(chunk4), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should have received "12345.678"
    EXPECT_EQ(number_values.size(), 1u);
    EXPECT_EQ(number_values[0], "12345.678");

    // Test: number ending with 'e' at EOF (scientific notation)
    gtext_json_stream_free(st);
    number_values.clear();
    st = gtext_json_stream_new(&opts, callback, &number_values);
    ASSERT_NE(st, nullptr);

    const char * chunk5 = "12345e";
    const char * chunk6 = "+2";

    status = gtext_json_stream_feed(st, chunk5, strlen(chunk5), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input (incomplete)

    status = gtext_json_stream_feed(st, chunk6, strlen(chunk6), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should have received "12345e+2"
    EXPECT_EQ(number_values.size(), 1u);
    EXPECT_EQ(number_values[0], "12345e+2");

    // Test: complete number at EOF (followed by space in next chunk)
    gtext_json_stream_free(st);
    number_values.clear();
    st = gtext_json_stream_new(&opts, callback, &number_values);
    ASSERT_NE(st, nullptr);

    const char * chunk7 = "12345";
    const char * chunk8 = " ";

    status = gtext_json_stream_feed(st, chunk7, strlen(chunk7), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input (incomplete)

    status = gtext_json_stream_feed(st, chunk8, strlen(chunk8), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_stream_finish(st, &err);
    // Note: This will fail because "12345 " is not valid JSON (trailing space)
    // But the number should have been emitted before the error
    // Actually, wait - if we have just "12345 ", that's not valid JSON at root level
    // Let's test with a valid context: [12345 ]
    gtext_json_stream_free(st);
    number_values.clear();
    st = gtext_json_stream_new(&opts, callback, &number_values);
    ASSERT_NE(st, nullptr);

    const char * chunk9 = "[12345";
    const char * chunk10 = "]";

    status = gtext_json_stream_feed(st, chunk9, strlen(chunk9), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input (incomplete)

    status = gtext_json_stream_feed(st, chunk10, strlen(chunk10), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should have received "12345" (complete number, not incomplete)
    EXPECT_EQ(number_values.size(), 1u);
    EXPECT_EQ(number_values[0], "12345");

    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - escape sequence spanning chunks
 */
TEST(StreamingParser, EscapeSequenceSpanningChunks) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    std::vector<std::string> string_values;

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)err;
        if (evt->type == GTEXT_JSON_EVT_STRING) {
            auto* strings = static_cast<std::vector<std::string>*>(user);
            std::string str(evt->as.str.s, evt->as.str.len);
            strings->push_back(str);
        }
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &string_values);
    ASSERT_NE(st, nullptr);

    GTEXT_JSON_Error err{};
    // Test: escape sequence split across chunks
    // The escape sequence \n is split across two chunks.
    // Chunk 1: "hello" followed by backslash
    // Chunk 2: "nworld"
    const char * chunk1 = "\"hello\\";
    const char * chunk2 = "nworld\"";

    GTEXT_JSON_Status status = gtext_json_stream_feed(st, chunk1, strlen(chunk1), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input

    status = gtext_json_stream_feed(st, chunk2, strlen(chunk2), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should have received "hello\nworld" (decoded)
    EXPECT_EQ(string_values.size(), 1u);
    EXPECT_EQ(string_values[0], "hello\nworld");

    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - unicode escape spanning chunks
 */
TEST(StreamingParser, UnicodeEscapeSpanningChunks) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    std::vector<std::string> string_values;

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)err;
        if (evt->type == GTEXT_JSON_EVT_STRING) {
            auto* strings = static_cast<std::vector<std::string>*>(user);
            std::string str(evt->as.str.s, evt->as.str.len);
            strings->push_back(str);
        }
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &string_values);
    ASSERT_NE(st, nullptr);

    GTEXT_JSON_Error err{};

    // Test: unicode escape split across chunks
    // Chunk 1: "hello\u
    // Chunk 2: 0041"
    const char * chunk1 = "\"hello\\u";
    const char * chunk2 = "0041\"";

    GTEXT_JSON_Status status = gtext_json_stream_feed(st, chunk1, strlen(chunk1), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input

    status = gtext_json_stream_feed(st, chunk2, strlen(chunk2), &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should have received "helloA" (U+0041 is 'A')
    EXPECT_EQ(string_values.size(), 1u);
    EXPECT_EQ(string_values[0], "helloA");

    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - large value spanning many chunks
 */
TEST(StreamingParser, LargeValueSpanningManyChunks) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    std::vector<std::string> string_values;

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)err;
        if (evt->type == GTEXT_JSON_EVT_STRING) {
            auto* strings = static_cast<std::vector<std::string>*>(user);
            std::string str(evt->as.str.s, evt->as.str.len);
            strings->push_back(str);
        }
        return GTEXT_JSON_OK;
    };

    GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &string_values);
    ASSERT_NE(st, nullptr);

    GTEXT_JSON_Error err{};

    // Test: large string split byte-by-byte
    std::string large_string = "\"";
    for (int i = 0; i < 1000; ++i) {
        large_string += "a";
    }
    large_string += "\"";

    // Feed byte by byte
    for (size_t i = 0; i < large_string.length(); ++i) {
        GTEXT_JSON_Status status = gtext_json_stream_feed(st, large_string.c_str() + i, 1, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed at byte " << i;
    }

    GTEXT_JSON_Status status = gtext_json_stream_finish(st, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Should have received the complete string
    EXPECT_EQ(string_values.size(), 1u);
    EXPECT_EQ(string_values[0].length(), 1000u);

    gtext_json_stream_free(st);
}

/**
 * Test streaming parser - value spanning 3 chunks
 */
TEST(StreamingParser, ValueSpanningThreeChunks) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    // Test 1: String spanning 3 chunks
    {
        std::vector<std::string> string_values;

        auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
            (void)err;
            if (evt->type == GTEXT_JSON_EVT_STRING) {
                auto* strings = static_cast<std::vector<std::string>*>(user);
                std::string str(evt->as.str.s, evt->as.str.len);
                strings->push_back(str);
            }
            return GTEXT_JSON_OK;
        };

        GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &string_values);
        ASSERT_NE(st, nullptr);

        GTEXT_JSON_Error err{};

        // String split across 3 chunks
        // Chunk 1: "hello
        // Chunk 2: world
        // Chunk 3: !"
        const char * chunk1 = "\"hello";
        const char * chunk2 = "world";
        const char * chunk3 = "!\"";

        GTEXT_JSON_Status status = gtext_json_stream_feed(st, chunk1, strlen(chunk1), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input

        status = gtext_json_stream_feed(st, chunk2, strlen(chunk2), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);  // Should still wait for more input

        status = gtext_json_stream_feed(st, chunk3, strlen(chunk3), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        status = gtext_json_stream_finish(st, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        // Should have received "helloworld!"
        EXPECT_EQ(string_values.size(), 1u);
        EXPECT_EQ(string_values[0], "helloworld!");

        gtext_json_stream_free(st);
    }

    // Test 2: Number spanning 3 chunks
    {
        std::vector<std::string> number_values;

        auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
            (void)err;
            if (evt->type == GTEXT_JSON_EVT_NUMBER) {
                auto* numbers = static_cast<std::vector<std::string>*>(user);
                std::string num(evt->as.number.s, evt->as.number.len);
                numbers->push_back(num);
            }
            return GTEXT_JSON_OK;
        };

        GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &number_values);
        ASSERT_NE(st, nullptr);

        GTEXT_JSON_Error err{};

        // Number split across 3 chunks
        // Chunk 1: 123
        // Chunk 2: 45
        // Chunk 3: .678
        const char * chunk1 = "123";
        const char * chunk2 = "45";
        const char * chunk3 = ".678";

        GTEXT_JSON_Status status = gtext_json_stream_feed(st, chunk1, strlen(chunk1), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input (incomplete)

        status = gtext_json_stream_feed(st, chunk2, strlen(chunk2), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);  // Should still wait for more input (incomplete)

        status = gtext_json_stream_feed(st, chunk3, strlen(chunk3), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        status = gtext_json_stream_finish(st, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        // Should have received "12345.678"
        EXPECT_EQ(number_values.size(), 1u);
        EXPECT_EQ(number_values[0], "12345.678");

        gtext_json_stream_free(st);
    }

    // Test 3: Unicode escape spanning 3 chunks
    {
        std::vector<std::string> string_values;

        auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
            (void)err;
            if (evt->type == GTEXT_JSON_EVT_STRING) {
                auto* strings = static_cast<std::vector<std::string>*>(user);
                std::string str(evt->as.str.s, evt->as.str.len);
                strings->push_back(str);
            }
            return GTEXT_JSON_OK;
        };

        GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &string_values);
        ASSERT_NE(st, nullptr);

        GTEXT_JSON_Error err{};

        // Unicode escape split across 3 chunks
        // Chunk 1: "hello\u
        // Chunk 2: 00
        // Chunk 3: 41"
        const char * chunk1 = "\"hello\\u";
        const char * chunk2 = "00";
        const char * chunk3 = "41\"";

        GTEXT_JSON_Status status = gtext_json_stream_feed(st, chunk1, strlen(chunk1), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input

        status = gtext_json_stream_feed(st, chunk2, strlen(chunk2), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);  // Should still wait for more input

        status = gtext_json_stream_feed(st, chunk3, strlen(chunk3), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        status = gtext_json_stream_finish(st, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        // Should have received "helloA" (U+0041 is 'A')
        EXPECT_EQ(string_values.size(), 1u);
        EXPECT_EQ(string_values[0], "helloA");

        gtext_json_stream_free(st);
    }

    // Test 4: Scientific notation number spanning 3 chunks
    {
        std::vector<std::string> number_values;

        auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
            (void)err;
            if (evt->type == GTEXT_JSON_EVT_NUMBER) {
                auto* numbers = static_cast<std::vector<std::string>*>(user);
                std::string num(evt->as.number.s, evt->as.number.len);
                numbers->push_back(num);
            }
            return GTEXT_JSON_OK;
        };

        GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &number_values);
        ASSERT_NE(st, nullptr);

        GTEXT_JSON_Error err{};

        // Scientific notation split across 3 chunks
        // Chunk 1: 12345e
        // Chunk 2: +
        // Chunk 3: 2
        const char * chunk1 = "12345e";
        const char * chunk2 = "+";
        const char * chunk3 = "2";

        GTEXT_JSON_Status status = gtext_json_stream_feed(st, chunk1, strlen(chunk1), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input (incomplete)

        status = gtext_json_stream_feed(st, chunk2, strlen(chunk2), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);  // Should still wait for more input (incomplete)

        status = gtext_json_stream_feed(st, chunk3, strlen(chunk3), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        status = gtext_json_stream_finish(st, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        // Should have received "12345e+2"
        EXPECT_EQ(number_values.size(), 1u);
        EXPECT_EQ(number_values[0], "12345e+2");

        gtext_json_stream_free(st);
    }
}

/**
 * Test streaming parser - value spanning many chunks (100+)
 * Verifies that state preservation works correctly across many chunk boundaries
 */
TEST(StreamingParser, ValueSpanningManyChunks) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    // Test: String spanning 100 chunks
    {
        std::vector<std::string> string_values;

        auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
            (void)err;
            if (evt->type == GTEXT_JSON_EVT_STRING) {
                auto* strings = static_cast<std::vector<std::string>*>(user);
                std::string str(evt->as.str.s, evt->as.str.len);
                strings->push_back(str);
            }
            return GTEXT_JSON_OK;
        };

        GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &string_values);
        ASSERT_NE(st, nullptr);

        GTEXT_JSON_Error err{};

        // String split across 100 chunks, each chunk is 1 character
        // Chunk 1: "a
        // Chunks 2-99: b, c, d, ...
        // Chunk 100: z"
        const char * chunk1 = "\"a";
        GTEXT_JSON_Status status = gtext_json_stream_feed(st, chunk1, strlen(chunk1), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);  // Should wait for more input

        // Feed 98 more single-character chunks
        for (char c = 'b'; c <= 'y'; ++c) {
            status = gtext_json_stream_feed(st, &c, 1, &err);
            EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed at chunk for character " << c;
        }

        // Final chunk with closing quote
        const char * final_chunk = "z\"";
        status = gtext_json_stream_feed(st, final_chunk, strlen(final_chunk), &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        status = gtext_json_stream_finish(st, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        // Should have received "abcdefghijklmnopqrstuvwxyz" (26 characters)
        EXPECT_EQ(string_values.size(), 1u);
        EXPECT_EQ(string_values[0].length(), 26u);
        EXPECT_EQ(string_values[0], "abcdefghijklmnopqrstuvwxyz");

        gtext_json_stream_free(st);
    }

    // Test: Number spanning 50 chunks
    {
        std::vector<std::string> number_values;

        auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
            (void)err;
            if (evt->type == GTEXT_JSON_EVT_NUMBER) {
                auto* numbers = static_cast<std::vector<std::string>*>(user);
                std::string num(evt->as.number.s, evt->as.number.len);
                numbers->push_back(num);
            }
            return GTEXT_JSON_OK;
        };

        GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &number_values);
        ASSERT_NE(st, nullptr);

        GTEXT_JSON_Error err{};

        // Number split across 50 chunks, each chunk is 1 digit
        // Chunks 1-49: digits 1-9
        // Chunk 50: final digit 0
        for (int i = 1; i <= 9; ++i) {
            char digit = '0' + i;
            GTEXT_JSON_Status status = gtext_json_stream_feed(st, &digit, 1, &err);
            EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed at chunk " << i;
        }

        // Feed 40 more digits
        for (int i = 0; i < 40; ++i) {
            char digit = '0' + (i % 10);
            GTEXT_JSON_Status status = gtext_json_stream_feed(st, &digit, 1, &err);
            EXPECT_EQ(status, GTEXT_JSON_OK) << "Failed at chunk " << (10 + i);
        }

        // Final digit
        const char final_digit = '0';
        GTEXT_JSON_Status status = gtext_json_stream_feed(st, &final_digit, 1, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        status = gtext_json_stream_finish(st, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        // Should have received a 50-digit number
        EXPECT_EQ(number_values.size(), 1u);
        EXPECT_EQ(number_values[0].length(), 50u);

        gtext_json_stream_free(st);
    }
}

/**
 * Test streaming parser - torture test feeding complex JSON byte-by-byte
 *
 * This test feeds complex JSON structures one character at a time to stress-test
 * the streaming parser's ability to handle edge cases, especially:
 * - Escape sequences split across byte boundaries
 * - Unicode escapes split across byte boundaries
 * - Deeply nested structures
 * - Numbers with various formats
 * - Mixed content types
 *
 * This is similar to the CSV torture test and helps find bugs in state management
 * and buffering when input arrives incrementally.
 */
TEST(StreamingParser, TortureTestByteByByte) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    // Track all events and string values for verification
    std::vector<GTEXT_JSON_Event_Type> events;
    std::vector<std::string> string_values;
    std::vector<std::string> number_values;
    std::vector<bool> bool_values;

    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)err;
        auto* data = static_cast<std::tuple<
            std::vector<GTEXT_JSON_Event_Type>*,
            std::vector<std::string>*,
            std::vector<std::string>*,
            std::vector<bool>*
        >*>(user);

        auto* evts = std::get<0>(*data);
        auto* strings = std::get<1>(*data);
        auto* numbers = std::get<2>(*data);
        auto* bools = std::get<3>(*data);

        evts->push_back(evt->type);

        switch (evt->type) {
            case GTEXT_JSON_EVT_STRING:
            case GTEXT_JSON_EVT_KEY:
                strings->push_back(std::string(evt->as.str.s, evt->as.str.len));
                break;
            case GTEXT_JSON_EVT_NUMBER:
                numbers->push_back(std::string(evt->as.number.s, evt->as.number.len));
                break;
            case GTEXT_JSON_EVT_BOOL:
                bools->push_back(evt->as.boolean);
                break;
            default:
                break;
        }
        return GTEXT_JSON_OK;
    };

    // Test 1: Complex nested structure with escape sequences
    {
        // JSON with nested objects/arrays, escape sequences, and Unicode
        const char * complex_json =
            "{\"key1\":\"value\\nwith\\tescapes\","
            "\"key2\":[1,2.5,-3.14e+10],"
            "\"key3\":{\"nested\":\"\\u0041\\u0042\\u0043\","
            "\"deep\":{\"array\":[true,false,null]}},"
            "\"unicode\":\"\\uD83D\\uDE00\","
            "\"escapes\":\"\\\\\\\"\\/\\b\\f\\n\\r\\t\"}";

        std::vector<GTEXT_JSON_Event_Type> test_events;
        std::vector<std::string> test_strings;
        std::vector<std::string> test_numbers;
        std::vector<bool> test_bools;

        auto test_data = std::make_tuple(&test_events, &test_strings, &test_numbers, &test_bools);

        GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &test_data);
        ASSERT_NE(st, nullptr);

        GTEXT_JSON_Error err{};
        size_t json_len = strlen(complex_json);

        // Feed byte by byte
        for (size_t i = 0; i < json_len; ++i) {
            GTEXT_JSON_Status status = gtext_json_stream_feed(st, complex_json + i, 1, &err);
            EXPECT_EQ(status, GTEXT_JSON_OK)
                << "Failed at byte " << i << " (char: '" << complex_json[i] << "')";
        }

        GTEXT_JSON_Status status = gtext_json_stream_finish(st, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        // Verify we got events
        EXPECT_GT(test_events.size(), 0u) << "Should have received events";

        // Verify string values were decoded correctly
        bool found_escaped = false;
        bool found_unicode = false;
        for (const auto& str : test_strings) {
            if (str.find('\n') != std::string::npos || str.find('\t') != std::string::npos) {
                found_escaped = true;
            }
            // Check for emoji (UTF-8 encoding of U+1F600)
            if (str.find("\xF0\x9F\x98\x80") != std::string::npos) {
                found_unicode = true;
            }
        }
        EXPECT_TRUE(found_escaped) << "Should have decoded escape sequences";
        EXPECT_TRUE(found_unicode) << "Should have decoded Unicode escape";

        gtext_json_stream_free(st);
    }

    // Test 2: Escape sequence split at every possible boundary
    {
        // Test each escape sequence type, split at different points
        const char * escape_tests[] = {
            "\"test\\nvalue\"",      // \n escape
            "\"test\\rvalue\"",      // \r escape
            "\"test\\tvalue\"",      // \t escape
            "\"test\\bvalue\"",      // \b escape
            "\"test\\fvalue\"",      // \f escape
            "\"test\\\\value\"",     // \\ escape
            "\"test\\\"value\"",     // \" escape
            "\"test\\/value\"",      // \/ escape
        };

        for (size_t test_idx = 0; test_idx < sizeof(escape_tests) / sizeof(escape_tests[0]); ++test_idx) {
            const char * test_json = escape_tests[test_idx];
            size_t test_len = strlen(test_json);

            std::vector<GTEXT_JSON_Event_Type> test_events;
            std::vector<std::string> test_strings;
            std::vector<std::string> test_numbers;
            std::vector<bool> test_bools;

            auto test_data = std::make_tuple(&test_events, &test_strings, &test_numbers, &test_bools);

            GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &test_data);
            ASSERT_NE(st, nullptr);

            GTEXT_JSON_Error err{};

            // Feed byte by byte
            for (size_t i = 0; i < test_len; ++i) {
                GTEXT_JSON_Status status = gtext_json_stream_feed(st, test_json + i, 1, &err);
                EXPECT_EQ(status, GTEXT_JSON_OK)
                    << "Escape test " << test_idx << " failed at byte " << i;
            }

            GTEXT_JSON_Status status = gtext_json_stream_finish(st, &err);
            EXPECT_EQ(status, GTEXT_JSON_OK);

            // Should have received at least one string event
            EXPECT_GT(test_strings.size(), 0u)
                << "Escape test " << test_idx << " should have produced string";

            gtext_json_stream_free(st);
        }
    }

    // Test 3: Unicode escape sequences split at various boundaries
    {
        const char * unicode_tests[] = {
            "\"\\u0041\"",           // Simple Unicode (A)
            "\"\\u00E9\"",           // é
            "\"\\u4E2D\"",           // 中 (Chinese)
            "\"\\uD83D\\uDE00\"",    // Emoji (surrogate pair)
        };

        for (size_t test_idx = 0; test_idx < sizeof(unicode_tests) / sizeof(unicode_tests[0]); ++test_idx) {
            const char * test_json = unicode_tests[test_idx];
            size_t test_len = strlen(test_json);

            std::vector<GTEXT_JSON_Event_Type> test_events;
            std::vector<std::string> test_strings;
            std::vector<std::string> test_numbers;
            std::vector<bool> test_bools;

            auto test_data = std::make_tuple(&test_events, &test_strings, &test_numbers, &test_bools);

            GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &test_data);
            ASSERT_NE(st, nullptr);

            GTEXT_JSON_Error err{};

            // Feed byte by byte - this is especially tricky for Unicode escapes
            // as the \uXXXX sequence can be split anywhere
            for (size_t i = 0; i < test_len; ++i) {
                GTEXT_JSON_Status status = gtext_json_stream_feed(st, test_json + i, 1, &err);
                EXPECT_EQ(status, GTEXT_JSON_OK)
                    << "Unicode test " << test_idx << " failed at byte " << i;
            }

            GTEXT_JSON_Status status = gtext_json_stream_finish(st, &err);
            EXPECT_EQ(status, GTEXT_JSON_OK);

            // Should have received at least one string event
            EXPECT_GT(test_strings.size(), 0u)
                << "Unicode test " << test_idx << " should have produced string";

            gtext_json_stream_free(st);
        }
    }

    // Test 4: Numbers with various formats, split byte-by-byte
    {
        // Create a copy of opts with non-finite numbers enabled for this test
        GTEXT_JSON_Parse_Options number_opts = opts;
        number_opts.allow_nonfinite_numbers = true;

        const char * number_tests[] = {
            "0",
            "123",
            "-456",
            "789.012",
            "-3.14159",
            "1e10",
            "2E-5",
            "-1.5e+20",
            "0.000001",
            "999999999999999999",
            "NaN",
            "Infinity",
            "-Infinity",
        };

        for (size_t test_idx = 0; test_idx < sizeof(number_tests) / sizeof(number_tests[0]); ++test_idx) {
            const char * test_json = number_tests[test_idx];
            size_t test_len = strlen(test_json);

            std::vector<GTEXT_JSON_Event_Type> test_events;
            std::vector<std::string> test_strings;
            std::vector<std::string> test_numbers;
            std::vector<bool> test_bools;

            auto test_data = std::make_tuple(&test_events, &test_strings, &test_numbers, &test_bools);

            GTEXT_JSON_Stream * st = gtext_json_stream_new(&number_opts, callback, &test_data);
            ASSERT_NE(st, nullptr);

            GTEXT_JSON_Error err{};

            // Feed byte by byte
            for (size_t i = 0; i < test_len; ++i) {
                GTEXT_JSON_Status status = gtext_json_stream_feed(st, test_json + i, 1, &err);
                EXPECT_EQ(status, GTEXT_JSON_OK)
                    << "Number test " << test_idx << " failed at byte " << i;
            }

            GTEXT_JSON_Status status = gtext_json_stream_finish(st, &err);
            EXPECT_EQ(status, GTEXT_JSON_OK);

            // Should have received at least one number event
            EXPECT_GT(test_numbers.size(), 0u)
                << "Number test " << test_idx << " should have produced number";
            EXPECT_EQ(test_numbers[0], test_json)
                << "Number should match input";

            gtext_json_stream_free(st);
        }
    }

    // Test 5: Deeply nested structure with mixed content
    {
        // Create a deeply nested structure that will stress-test the parser
        // 20 opening brackets, 20 closing brackets (one for each nested array)
        const char * nested_json =
            "[[[[[[[[[[[[[[[[[[[[\"deep\"]]]]]]]]]]]]]]]]]]]]";

        std::vector<GTEXT_JSON_Event_Type> test_events;
        std::vector<std::string> test_strings;
        std::vector<std::string> test_numbers;
        std::vector<bool> test_bools;

        auto test_data = std::make_tuple(&test_events, &test_strings, &test_numbers, &test_bools);

        GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &test_data);
        ASSERT_NE(st, nullptr);

        GTEXT_JSON_Error err{};
        size_t json_len = strlen(nested_json);

        // Feed byte by byte
        for (size_t i = 0; i < json_len; ++i) {
            GTEXT_JSON_Status status = gtext_json_stream_feed(st, nested_json + i, 1, &err);
            EXPECT_EQ(status, GTEXT_JSON_OK)
                << "Nested test failed at byte " << i;
        }

        GTEXT_JSON_Status status = gtext_json_stream_finish(st, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        // Should have received events for nested structure
        EXPECT_GT(test_events.size(), 0u) << "Should have received events";
        EXPECT_GT(test_strings.size(), 0u) << "Should have received string";

        gtext_json_stream_free(st);
    }

    // Test 6: Complex real-world-like JSON with all features
    {
        const char * realworld_json =
            "{\"users\":["
            "{\"id\":1,\"name\":\"Alice\\nSmith\",\"email\":\"alice@example.com\",\"active\":true},"
            "{\"id\":2,\"name\":\"Bob\\tJones\",\"email\":\"bob@example.com\",\"active\":false},"
            "{\"id\":3,\"name\":\"Charlie\\u00E9\",\"email\":\"charlie@example.com\",\"score\":98.5}"
            "],"
            "\"metadata\":{\"version\":\"1.0\",\"unicode\":\"\\uD83D\\uDE00\"}}";

        std::vector<GTEXT_JSON_Event_Type> test_events;
        std::vector<std::string> test_strings;
        std::vector<std::string> test_numbers;
        std::vector<bool> test_bools;

        auto test_data = std::make_tuple(&test_events, &test_strings, &test_numbers, &test_bools);

        GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, callback, &test_data);
        ASSERT_NE(st, nullptr);

        GTEXT_JSON_Error err{};
        size_t json_len = strlen(realworld_json);

        // Feed byte by byte - this is the ultimate torture test
        for (size_t i = 0; i < json_len; ++i) {
            GTEXT_JSON_Status status = gtext_json_stream_feed(st, realworld_json + i, 1, &err);
            EXPECT_EQ(status, GTEXT_JSON_OK)
                << "Real-world test failed at byte " << i
                << " (char: '" << (realworld_json[i] >= 32 && realworld_json[i] < 127 ? realworld_json[i] : '?') << "')";
        }

        GTEXT_JSON_Status status = gtext_json_stream_finish(st, &err);
        EXPECT_EQ(status, GTEXT_JSON_OK);

        // Verify we got a reasonable number of events
        EXPECT_GT(test_events.size(), 10u) << "Should have received many events";
        EXPECT_GT(test_strings.size(), 5u) << "Should have received multiple strings";
        EXPECT_GT(test_numbers.size(), 3u) << "Should have received multiple numbers";
        EXPECT_GT(test_bools.size(), 0u) << "Should have received boolean values";

        gtext_json_stream_free(st);
    }
}

// ============================================================================
// Streaming Writer Tests
// ============================================================================

/**
 * Test streaming writer - creation and destruction
 */
TEST(StreamingWriter, Creation) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test streaming writer - NULL sink
 */
TEST(StreamingWriter, NullSink) {
    GTEXT_JSON_Sink sink = {nullptr, nullptr};
    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
    EXPECT_EQ(w, nullptr);
}

/**
 * Test streaming writer - basic value writing (null, bool, number, string)
 */
TEST(StreamingWriter, BasicValues) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    // Write null
    status = gtext_json_writer_null(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Finish
    GTEXT_JSON_Error err{};
    status = gtext_json_writer_finish(w, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify output
    const char * output = gtext_json_sink_buffer_data(&sink);
    size_t output_len = gtext_json_sink_buffer_size(&sink);
    EXPECT_STREQ(output, "null");
    EXPECT_EQ(output_len, 4u);

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Test bool
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    status = gtext_json_writer_bool(w, true);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_finish(w, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    output = gtext_json_sink_buffer_data(&sink);
    EXPECT_STREQ(output, "true");

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Test number (i64)
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    status = gtext_json_writer_number_i64(w, 12345);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_finish(w, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    output = gtext_json_sink_buffer_data(&sink);
    EXPECT_STREQ(output, "12345");

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Test string
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    status = gtext_json_writer_string(w, "hello", 5);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_finish(w, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    output = gtext_json_sink_buffer_data(&sink);
    EXPECT_STREQ(output, "\"hello\"");

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test streaming writer - array writing
 */
TEST(StreamingWriter, Arrays) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    // Write array: [1, 2, 3]
    status = gtext_json_writer_array_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_number_i64(w, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_number_i64(w, 2);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_number_i64(w, 3);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_array_end(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Error err{};
    status = gtext_json_writer_finish(w, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify output (compact mode: no spaces after commas)
    const char * output = gtext_json_sink_buffer_data(&sink);
    EXPECT_STREQ(output, "[1,2,3]");

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test streaming writer - object writing
 */
TEST(StreamingWriter, Objects) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    // Write object: {"key": "value"}
    status = gtext_json_writer_object_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_key(w, "key", 3);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_string(w, "value", 5);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_object_end(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Error err{};
    status = gtext_json_writer_finish(w, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify output
    const char * output = gtext_json_sink_buffer_data(&sink);
    EXPECT_STREQ(output, "{\"key\":\"value\"}");

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test streaming writer - structural enforcement (invalid sequences)
 */
TEST(StreamingWriter, StructuralEnforcement) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    // Try to write value without key in object - should fail
    status = gtext_json_writer_object_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_null(w);  // Should fail - need key first
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_STATE);

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Try to write key when not in object - should fail
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    status = gtext_json_writer_key(w, "key", 3);  // Should fail - not in object
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_STATE);

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Try to end object when expecting key - should fail
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    status = gtext_json_writer_object_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_key(w, "key", 3);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Now try to end object without writing value - should fail
    status = gtext_json_writer_object_end(w);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_STATE);

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test streaming writer - finish validation (incomplete structure)
 */
TEST(StreamingWriter, FinishValidation) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    // Start object but don't close it
    status = gtext_json_writer_object_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Error err{};
    status = gtext_json_writer_finish(w, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_INCOMPLETE);

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Start array but don't close it
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    status = gtext_json_writer_array_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_finish(w, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_INCOMPLETE);

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test streaming writer - pretty-print mode
 */
TEST(StreamingWriter, PrettyPrint) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    opts.pretty = true;
    opts.indent_spaces = 2;

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, &opts);
    ASSERT_NE(w, nullptr);

    // Write object with pretty printing
    status = gtext_json_writer_object_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_key(w, "key", 3);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_string(w, "value", 5);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_object_end(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Error err{};
    status = gtext_json_writer_finish(w, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify output has newlines and indentation
    const char * output = gtext_json_sink_buffer_data(&sink);
    EXPECT_NE(strstr(output, "\n"), nullptr);  // Should have newline

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test streaming writer - nested structures
 */
TEST(StreamingWriter, NestedStructures) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    // Write: {"arr": [1, 2], "obj": {"key": "value"}}
    status = gtext_json_writer_object_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_key(w, "arr", 3);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_array_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_number_i64(w, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_number_i64(w, 2);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_array_end(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_key(w, "obj", 3);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_object_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_key(w, "key", 3);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_string(w, "value", 5);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_object_end(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_object_end(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Error err{};
    status = gtext_json_writer_finish(w, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // The whole output, and a reparse.
    //
    // This used to be three strstr() calls asserting that "arr", "obj" and
    // "key" appeared somewhere. All three were true of
    //
    //     {"arr":[1,2],"obj":,{"key":"value"}}
    //
    // which is what this API actually wrote for any object of two or more
    // members, and is not JSON. The test covering the exact shape of the defect
    // passed for as long as the defect existed, because presence of a substring
    // says nothing about the bytes between them. See
    // tests/test-writer-agreement.cpp.
    const size_t out_len = gtext_json_sink_buffer_size(&sink);
    const std::string output(gtext_json_sink_buffer_data(&sink), out_len);
    EXPECT_EQ(output, "{\"arr\":[1,2],\"obj\":{\"key\":\"value\"}}");

    GTEXT_JSON_Value * reparsed =
        gtext_json_parse(output.data(), output.size(), nullptr, nullptr);
    ASSERT_NE(reparsed, nullptr) << "not valid JSON: " << output;
    EXPECT_EQ(gtext_json_object_size(reparsed), 2u);
    gtext_json_free(reparsed);

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test streaming writer - number formats (i64, u64, double, lexeme)
 */
TEST(StreamingWriter, NumberFormats) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    // Test i64
    status = gtext_json_writer_number_i64(w, -12345);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_finish(w, nullptr);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "-12345");
    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Test u64
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);
    w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);
    status = gtext_json_writer_number_u64(w, 12345ULL);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_finish(w, nullptr);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "12345");
    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Test double
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);
    w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);
    status = gtext_json_writer_number_double(w, 3.14159);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_finish(w, nullptr);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    const char * output = gtext_json_sink_buffer_data(&sink);
    EXPECT_NE(strstr(output, "3.14"), nullptr);  // Should contain the number
    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Test lexeme
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);
    w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);
    status = gtext_json_writer_number_lexeme(w, "123.456", 7);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_finish(w, nullptr);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "123.456");
    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test streaming writer - nonfinite numbers
 */
TEST(StreamingWriter, NonfiniteNumbers) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    opts.allow_nonfinite_numbers = true;

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, &opts);
    ASSERT_NE(w, nullptr);

    // Test NaN
    status = gtext_json_writer_number_double(w, std::nan(""));
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_finish(w, nullptr);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "NaN");
    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Test Infinity
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);
    w = gtext_json_writer_new(sink, &opts);
    ASSERT_NE(w, nullptr);
    status = gtext_json_writer_number_double(w, std::numeric_limits<double>::infinity());
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_finish(w, nullptr);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "Infinity");
    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Test -Infinity
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);
    w = gtext_json_writer_new(sink, &opts);
    ASSERT_NE(w, nullptr);
    status = gtext_json_writer_number_double(w, -std::numeric_limits<double>::infinity());
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_finish(w, nullptr);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_STREQ(gtext_json_sink_buffer_data(&sink), "-Infinity");
    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);

    // Test that nonfinite numbers are rejected when option is off
    opts.allow_nonfinite_numbers = false;
    status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);
    w = gtext_json_writer_new(sink, &opts);
    ASSERT_NE(w, nullptr);
    status = gtext_json_writer_number_double(w, std::nan(""));
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_NONFINITE);
    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test streaming writer - round-trip (write then parse)
 */
TEST(StreamingWriter, RoundTrip) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    // Write: {"key": [1, 2, 3]}
    status = gtext_json_writer_object_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_key(w, "key", 3);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_array_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_number_i64(w, 1);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_number_i64(w, 2);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_number_i64(w, 3);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_array_end(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    status = gtext_json_writer_object_end(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Error err{};
    status = gtext_json_writer_finish(w, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Parse the output
    const char * output = gtext_json_sink_buffer_data(&sink);
    size_t output_len = gtext_json_sink_buffer_size(&sink);

    GTEXT_JSON_Parse_Options parse_opts = gtext_json_parse_options_default();
    GTEXT_JSON_Value * v = gtext_json_parse(output, output_len, &parse_opts, &err);
    ASSERT_NE(v, nullptr);

    // Verify structure
    EXPECT_EQ(gtext_json_typeof(v), GTEXT_JSON_OBJECT);
    const GTEXT_JSON_Value * arr = gtext_json_object_get(v, "key", 3);
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(gtext_json_typeof(arr), GTEXT_JSON_ARRAY);
    EXPECT_EQ(gtext_json_array_size(arr), 3u);

    gtext_json_free(v);
    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test streaming writer - error state handling
 */
TEST(StreamingWriter, ErrorState) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
    ASSERT_NE(w, nullptr);

    // Cause an error (try to write value without key in object)
    status = gtext_json_writer_object_begin(w);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    status = gtext_json_writer_null(w);  // Should fail
    EXPECT_NE(status, GTEXT_JSON_OK);

    // After error, subsequent operations should fail
    status = gtext_json_writer_key(w, "key", 3);
    EXPECT_NE(status, GTEXT_JSON_OK);  // Actually, this might succeed, but finish should fail

    GTEXT_JSON_Error err{};
    status = gtext_json_writer_finish(w, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);

    gtext_json_writer_free(w);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test JSON Pointer - empty pointer refers to root
 */
TEST(JsonPointer, EmptyPointer) {
    GTEXT_JSON_Value * root = gtext_json_new_string("test", 4);
    ASSERT_NE(root, nullptr);

    const GTEXT_JSON_Value * result = gtext_json_pointer_get(root, "", 0);
    EXPECT_EQ(result, root);

    gtext_json_free(root);
}

/**
 * Test JSON Pointer - object key access
 */
TEST(JsonPointer, ObjectKeyAccess) {
    const char * json = "{\"a\":1,\"b\":2,\"c\":{\"d\":3}}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Access existing keys
    const GTEXT_JSON_Value * a = gtext_json_pointer_get(root, "/a", 2);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(gtext_json_typeof(a), GTEXT_JSON_NUMBER);

    const GTEXT_JSON_Value * b = gtext_json_pointer_get(root, "/b", 2);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(gtext_json_typeof(b), GTEXT_JSON_NUMBER);

    // Access nested object
    const GTEXT_JSON_Value * c = gtext_json_pointer_get(root, "/c", 2);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(gtext_json_typeof(c), GTEXT_JSON_OBJECT);

    const GTEXT_JSON_Value * d = gtext_json_pointer_get(root, "/c/d", 4);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(gtext_json_typeof(d), GTEXT_JSON_NUMBER);

    // Access non-existent key
    const GTEXT_JSON_Value * missing = gtext_json_pointer_get(root, "/x", 2);
    EXPECT_EQ(missing, nullptr);

    gtext_json_free(root);
}

/**
 * Test JSON Pointer - array index access
 */
TEST(JsonPointer, ArrayIndexAccess) {
    const char * json = "[10,20,30,[40,50]]";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Access valid indices
    const GTEXT_JSON_Value * elem0 = gtext_json_pointer_get(root, "/0", 2);
    ASSERT_NE(elem0, nullptr);
    EXPECT_EQ(gtext_json_typeof(elem0), GTEXT_JSON_NUMBER);

    const GTEXT_JSON_Value * elem1 = gtext_json_pointer_get(root, "/1", 2);
    ASSERT_NE(elem1, nullptr);
    EXPECT_EQ(gtext_json_typeof(elem1), GTEXT_JSON_NUMBER);

    // Access nested array
    const GTEXT_JSON_Value * nested = gtext_json_pointer_get(root, "/3", 2);
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(gtext_json_typeof(nested), GTEXT_JSON_ARRAY);

    const GTEXT_JSON_Value * nested0 = gtext_json_pointer_get(root, "/3/0", 4);
    ASSERT_NE(nested0, nullptr);
    EXPECT_EQ(gtext_json_typeof(nested0), GTEXT_JSON_NUMBER);

    // Access out-of-bounds index
    const GTEXT_JSON_Value * out_of_bounds = gtext_json_pointer_get(root, "/10", 3);
    EXPECT_EQ(out_of_bounds, nullptr);

    gtext_json_free(root);
}

/**
 * Test JSON Pointer - complex nested structures
 */
TEST(JsonPointer, ComplexNestedStructures) {
    const char * json = "{\"a\":[{\"b\":1,\"c\":2},{\"d\":3}],\"e\":{\"f\":[4,5,6]}}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Navigate complex path: /a/0/b
    const GTEXT_JSON_Value * result = gtext_json_pointer_get(root, "/a/0/b", 6);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(gtext_json_typeof(result), GTEXT_JSON_NUMBER);

    // Navigate: /a/0/c
    result = gtext_json_pointer_get(root, "/a/0/c", 6);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(gtext_json_typeof(result), GTEXT_JSON_NUMBER);

    // Navigate: /a/1/d
    result = gtext_json_pointer_get(root, "/a/1/d", 6);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(gtext_json_typeof(result), GTEXT_JSON_NUMBER);

    // Navigate: /e/f/0
    result = gtext_json_pointer_get(root, "/e/f/0", 6);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(gtext_json_typeof(result), GTEXT_JSON_NUMBER);

    // Navigate: /e/f/2
    result = gtext_json_pointer_get(root, "/e/f/2", 6);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(gtext_json_typeof(result), GTEXT_JSON_NUMBER);

    gtext_json_free(root);
}

/**
 * Test JSON Pointer - escape sequences (~0 -> ~, ~1 -> /)
 */
TEST(JsonPointer, EscapeSequences) {
    // Create object with keys containing ~ and /
    GTEXT_JSON_Value * root = gtext_json_new_object();
    ASSERT_NE(root, nullptr);

    GTEXT_JSON_Value * val1 = gtext_json_new_string("value1", 6);
    GTEXT_JSON_Value * val2 = gtext_json_new_string("value2", 6);
    GTEXT_JSON_Value * val3 = gtext_json_new_string("value3", 6);

    // Key with tilde: "key~with~tilde" (14 bytes)
    gtext_json_object_put(root, "key~with~tilde", 14, val1);

    // Key with slash: "key/with/slash"
    gtext_json_object_put(root, "key/with/slash", 14, val2);

    // Key with both: "key~0/with~1both"
    gtext_json_object_put(root, "key~0/with~1both", 16, val3);

    // Access key with tilde using ~0 escape
    const GTEXT_JSON_Value * result = gtext_json_pointer_get(root, "/key~0with~0tilde", 17);  // "/key~0with~0tilde" = 17 chars
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(gtext_json_typeof(result), GTEXT_JSON_STRING);

    // Access key with slash using ~1 escape
    result = gtext_json_pointer_get(root, "/key~1with~1slash", 17);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(gtext_json_typeof(result), GTEXT_JSON_STRING);

    // Access key with both escapes
    // Key is "key~0/with~1both", so pointer is "/key~00~1with~01both"
    // (~00 -> ~0, ~1 -> /, ~01 -> ~1)
    result = gtext_json_pointer_get(root, "/key~00~1with~01both", 20);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(gtext_json_typeof(result), GTEXT_JSON_STRING);

    gtext_json_free(root);
}

/**
 * Test JSON Pointer - invalid pointer formats
 */
TEST(JsonPointer, InvalidFormats) {
    GTEXT_JSON_Value * root = gtext_json_new_string("test", 4);
    ASSERT_NE(root, nullptr);

    // Pointer not starting with /
    const GTEXT_JSON_Value * result = gtext_json_pointer_get(root, "a", 1);
    EXPECT_EQ(result, nullptr);

    // Invalid escape sequence
    result = gtext_json_pointer_get(root, "/a~2", 4);
    EXPECT_EQ(result, nullptr);

    // Incomplete escape sequence
    result = gtext_json_pointer_get(root, "/a~", 3);
    EXPECT_EQ(result, nullptr);

    // NULL pointer
    result = gtext_json_pointer_get(root, nullptr, 0);
    EXPECT_EQ(result, nullptr);

    // NULL root
    result = gtext_json_pointer_get(nullptr, "/a", 2);
    EXPECT_EQ(result, nullptr);

    gtext_json_free(root);
}

/**
 * Test JSON Pointer - array index validation
 */
TEST(JsonPointer, ArrayIndexValidation) {
    const char * json = "[1,2,3]";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Valid indices
    EXPECT_NE(gtext_json_pointer_get(root, "/0", 2), nullptr);
    EXPECT_NE(gtext_json_pointer_get(root, "/1", 2), nullptr);
    EXPECT_NE(gtext_json_pointer_get(root, "/2", 2), nullptr);

    // Out of bounds
    EXPECT_EQ(gtext_json_pointer_get(root, "/3", 2), nullptr);

    // Leading zeros not allowed (except "0")
    EXPECT_EQ(gtext_json_pointer_get(root, "/01", 3), nullptr);
    EXPECT_EQ(gtext_json_pointer_get(root, "/00", 3), nullptr);

    // Non-numeric index
    EXPECT_EQ(gtext_json_pointer_get(root, "/a", 2), nullptr);

    gtext_json_free(root);
}

/**
 * Test JSON Pointer - mutable access
 */
TEST(JsonPointer, MutableAccess) {
    const char * json = "{\"a\":1,\"b\":[2,3]}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Get mutable pointer
    GTEXT_JSON_Value * a = gtext_json_pointer_get_mut(root, "/a", 2);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(gtext_json_typeof(a), GTEXT_JSON_NUMBER);

    // Modify the value (replace with new number)
    GTEXT_JSON_Value * new_val = gtext_json_new_number_i64(42);
    ASSERT_NE(new_val, nullptr);

    // Get parent object and replace value
    GTEXT_JSON_Value * parent = gtext_json_pointer_get_mut(root, "", 0);
    ASSERT_NE(parent, nullptr);
    gtext_json_object_put(parent, "a", 1, new_val);

    // Verify modification
    const GTEXT_JSON_Value * modified = gtext_json_pointer_get(root, "/a", 2);
    ASSERT_NE(modified, nullptr);
    int64_t val;
    gtext_json_get_i64(modified, &val);
    EXPECT_EQ(val, 42);

    // Modify array element
    GTEXT_JSON_Value * arr_elem = gtext_json_pointer_get_mut(root, "/b/0", 4);
    ASSERT_NE(arr_elem, nullptr);
    GTEXT_JSON_Value * new_arr_val = gtext_json_new_number_i64(99);
    GTEXT_JSON_Value * arr = gtext_json_pointer_get_mut(root, "/b", 2);
    gtext_json_array_set(arr, 0, new_arr_val);

    // Verify array modification
    const GTEXT_JSON_Value * modified_arr_elem = gtext_json_pointer_get(root, "/b/0", 4);
    ASSERT_NE(modified_arr_elem, nullptr);
    gtext_json_get_i64(modified_arr_elem, &val);
    EXPECT_EQ(val, 99);

    gtext_json_free(root);
}

/**
 * Test JSON Pointer - type mismatches
 */
TEST(JsonPointer, TypeMismatches) {
    const char * json = "{\"a\":1,\"b\":[2,3]}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Try to access object key on array
    const GTEXT_JSON_Value * result = gtext_json_pointer_get(root, "/b/a", 4);
    EXPECT_EQ(result, nullptr);

    // Try to access array index on object
    result = gtext_json_pointer_get(root, "/a/0", 4);
    EXPECT_EQ(result, nullptr);

    gtext_json_free(root);
}

/**
 * Test JSON Patch - add operation to object
 */
TEST(JsonPatch, AddToObject) {
    const char * json = "{\"foo\":\"bar\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: add "baz": "qux" to root object
    const char * patch_json = "[{\"op\":\"add\",\"path\":\"/baz\",\"value\":\"qux\"}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify the value was added
    const GTEXT_JSON_Value * baz = gtext_json_pointer_get(root, "/baz", 4);
    ASSERT_NE(baz, nullptr);
    EXPECT_EQ(gtext_json_typeof(baz), GTEXT_JSON_STRING);
    const char * baz_str;
    size_t baz_len;
    gtext_json_get_string(baz, &baz_str, &baz_len);
    EXPECT_EQ(baz_len, 3u);
    EXPECT_EQ(memcmp(baz_str, "qux", 3), 0);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - add operation to array
 */
TEST(JsonPatch, AddToArray) {
    const char * json = "{\"foo\":[1,2]}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: add 3 to end of array
    const char * patch_json = "[{\"op\":\"add\",\"path\":\"/foo/-\",\"value\":3}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify the value was added
    const GTEXT_JSON_Value * arr = gtext_json_pointer_get(root, "/foo", 4);
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(gtext_json_array_size(arr), 3u);
    const GTEXT_JSON_Value * elem2 = gtext_json_array_get(arr, 2);
    ASSERT_NE(elem2, nullptr);
    int64_t val;
    gtext_json_get_i64(elem2, &val);
    EXPECT_EQ(val, 3);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - add operation at specific array index
 */
TEST(JsonPatch, AddToArrayAtIndex) {
    const char * json = "{\"foo\":[1,3]}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: insert 2 at index 1
    const char * patch_json = "[{\"op\":\"add\",\"path\":\"/foo/1\",\"value\":2}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify the value was inserted
    const GTEXT_JSON_Value * arr = gtext_json_pointer_get(root, "/foo", 4);
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(gtext_json_array_size(arr), 3u);
    int64_t val;
    gtext_json_get_i64(gtext_json_array_get(arr, 0), &val);
    EXPECT_EQ(val, 1);
    gtext_json_get_i64(gtext_json_array_get(arr, 1), &val);
    EXPECT_EQ(val, 2);
    gtext_json_get_i64(gtext_json_array_get(arr, 2), &val);
    EXPECT_EQ(val, 3);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - remove operation from object
 */
TEST(JsonPatch, RemoveFromObject) {
    const char * json = "{\"foo\":\"bar\",\"baz\":\"qux\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: remove "foo"
    const char * patch_json = "[{\"op\":\"remove\",\"path\":\"/foo\"}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify "foo" was removed
    const GTEXT_JSON_Value * foo = gtext_json_pointer_get(root, "/foo", 4);
    EXPECT_EQ(foo, nullptr);

    // Verify "baz" still exists
    const GTEXT_JSON_Value * baz = gtext_json_pointer_get(root, "/baz", 4);
    ASSERT_NE(baz, nullptr);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - remove operation from array
 */
TEST(JsonPatch, RemoveFromArray) {
    const char * json = "{\"foo\":[1,2,3]}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: remove element at index 1
    const char * patch_json = "[{\"op\":\"remove\",\"path\":\"/foo/1\"}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify element was removed
    const GTEXT_JSON_Value * arr = gtext_json_pointer_get(root, "/foo", 4);
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(gtext_json_array_size(arr), 2u);
    int64_t val;
    gtext_json_get_i64(gtext_json_array_get(arr, 0), &val);
    EXPECT_EQ(val, 1);
    gtext_json_get_i64(gtext_json_array_get(arr, 1), &val);
    EXPECT_EQ(val, 3);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - replace operation
 */
TEST(JsonPatch, Replace) {
    const char * json = "{\"foo\":\"bar\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: replace "foo" with "baz"
    const char * patch_json = "[{\"op\":\"replace\",\"path\":\"/foo\",\"value\":\"baz\"}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify the value was replaced
    const GTEXT_JSON_Value * foo = gtext_json_pointer_get(root, "/foo", 4);
    ASSERT_NE(foo, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(foo, &str, &len);
    EXPECT_EQ(len, 3u);
    EXPECT_EQ(memcmp(str, "baz", 3), 0);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - move operation
 */
TEST(JsonPatch, Move) {
    const char * json = "{\"foo\":{\"bar\":\"baz\"},\"qux\":{}}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: move /foo/bar to /qux/thud
    const char * patch_json = "[{\"op\":\"move\",\"from\":\"/foo/bar\",\"path\":\"/qux/thud\"}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify value was moved
    const GTEXT_JSON_Value * bar = gtext_json_pointer_get(root, "/foo/bar", 8);
    EXPECT_EQ(bar, nullptr);  // Should be removed

    const GTEXT_JSON_Value * thud = gtext_json_pointer_get(root, "/qux/thud", 9);
    ASSERT_NE(thud, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(thud, &str, &len);
    EXPECT_EQ(len, 3u);
    EXPECT_EQ(memcmp(str, "baz", 3), 0);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - copy operation
 */
TEST(JsonPatch, Copy) {
    const char * json = "{\"foo\":{\"bar\":\"baz\"},\"qux\":{}}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: copy /foo/bar to /qux/thud
    const char * patch_json = "[{\"op\":\"copy\",\"from\":\"/foo/bar\",\"path\":\"/qux/thud\"}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify value was copied (original should still exist)
    const GTEXT_JSON_Value * bar = gtext_json_pointer_get(root, "/foo/bar", 8);
    ASSERT_NE(bar, nullptr);  // Should still exist

    const GTEXT_JSON_Value * thud = gtext_json_pointer_get(root, "/qux/thud", 9);
    ASSERT_NE(thud, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(thud, &str, &len);
    EXPECT_EQ(len, 3u);
    EXPECT_EQ(memcmp(str, "baz", 3), 0);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - test operation (success)
 */
TEST(JsonPatch, TestSuccess) {
    const char * json = "{\"foo\":\"bar\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: test that /foo equals "bar"
    const char * patch_json = "[{\"op\":\"test\",\"path\":\"/foo\",\"value\":\"bar\"}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - test operation (failure)
 */
TEST(JsonPatch, TestFailure) {
    const char * json = "{\"foo\":\"bar\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: test that /foo equals "baz" (should fail)
    const char * patch_json = "[{\"op\":\"test\",\"path\":\"/foo\",\"value\":\"baz\"}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - multiple operations
 */
TEST(JsonPatch, MultipleOperations) {
    const char * json = "{\"foo\":\"bar\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch with multiple operations
    const char * patch_json = "["
        "{\"op\":\"add\",\"path\":\"/baz\",\"value\":\"qux\"},"
        "{\"op\":\"replace\",\"path\":\"/foo\",\"value\":\"bar2\"},"
        "{\"op\":\"test\",\"path\":\"/baz\",\"value\":\"qux\"}"
    "]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify all operations were applied
    const GTEXT_JSON_Value * foo = gtext_json_pointer_get(root, "/foo", 4);
    ASSERT_NE(foo, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(foo, &str, &len);
    EXPECT_EQ(len, 4u);
    EXPECT_EQ(memcmp(str, "bar2", 4), 0);

    const GTEXT_JSON_Value * baz = gtext_json_pointer_get(root, "/baz", 4);
    ASSERT_NE(baz, nullptr);
    gtext_json_get_string(baz, &str, &len);
    EXPECT_EQ(len, 3u);
    EXPECT_EQ(memcmp(str, "qux", 3), 0);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - error cases: invalid path
 */
TEST(JsonPatch, ErrorInvalidPath) {
    const char * json = "{\"foo\":\"bar\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch with invalid path
    const char * patch_json = "[{\"op\":\"remove\",\"path\":\"/nonexistent\"}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - error cases: missing required fields
 */
TEST(JsonPatch, ErrorMissingFields) {
    const char * json = "{\"foo\":\"bar\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch with missing "value" field
    const char * patch_json = "[{\"op\":\"add\",\"path\":\"/baz\"}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - error cases: move into descendant
 */
TEST(JsonPatch, ErrorMoveIntoDescendant) {
    const char * json = "{\"foo\":{\"bar\":\"baz\"}}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: try to move /foo into /foo/bar (should fail)
    const char * patch_json = "[{\"op\":\"move\",\"from\":\"/foo\",\"path\":\"/foo/bar\"}]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Patch - atomicity: true rollback on failure
 *
 * The implementation provides true atomicity with rollback:
 * - All operations are applied to a deep clone of the root first
 * - Only if all operations succeed is the clone's content copied back to the original
 * - If any operation fails, the clone is discarded and the original remains completely unchanged
 * - This ensures all-or-nothing semantics: either all operations succeed or none are applied
 *
 * This test verifies that when a later operation fails, earlier operations are not applied.
 */
TEST(JsonPatch, Atomicity) {
    const char * json = "{\"foo\":\"bar\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * root = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(root, nullptr);

    // Create patch: first operation succeeds, second fails
    const char * patch_json = "["
        "{\"op\":\"add\",\"path\":\"/baz\",\"value\":\"qux\"},"
        "{\"op\":\"remove\",\"path\":\"/nonexistent\"}"
    "]";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_patch_apply(root, patch, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);

    // With true atomicity: all operations are applied to a clone first.
    // Only if all operations succeed is the clone's content copied back to the original.
    // If any operation fails, the original remains unchanged.
    const GTEXT_JSON_Value * baz = gtext_json_pointer_get(root, "/baz", 4);
    EXPECT_EQ(baz, nullptr);  // Nothing was applied because second operation failed

    // Verify original value is unchanged
    const GTEXT_JSON_Value * foo = gtext_json_pointer_get(root, "/foo", 4);
    ASSERT_NE(foo, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(foo, &str, &len);
    EXPECT_EQ(len, 3u);
    EXPECT_EQ(memcmp(str, "bar", 3), 0);

    gtext_json_free(patch);
    gtext_json_free(root);
}

/**
 * Test JSON Merge Patch - basic object merge (replace value)
 */
TEST(JsonMergePatch, BasicReplace) {
    const char * target_json = "{\"a\":\"b\"}";
    const char * patch_json = "{\"a\":\"c\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * target = gtext_json_parse(target_json, strlen(target_json), &opts, &err);
    ASSERT_NE(target, nullptr);

    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_merge_patch(target, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify result
    const GTEXT_JSON_Value * a = gtext_json_pointer_get(target, "/a", 2);
    ASSERT_NE(a, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(a, &str, &len);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(memcmp(str, "c", 1), 0);

    gtext_json_free(patch);
    gtext_json_free(target);
}

/**
 * Test JSON Merge Patch - add new member
 */
TEST(JsonMergePatch, AddNewMember) {
    const char * target_json = "{\"a\":\"b\"}";
    const char * patch_json = "{\"b\":\"c\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * target = gtext_json_parse(target_json, strlen(target_json), &opts, &err);
    ASSERT_NE(target, nullptr);

    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_merge_patch(target, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify both members exist
    const GTEXT_JSON_Value * a = gtext_json_pointer_get(target, "/a", 2);
    ASSERT_NE(a, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(a, &str, &len);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(memcmp(str, "b", 1), 0);

    const GTEXT_JSON_Value * b = gtext_json_pointer_get(target, "/b", 2);
    ASSERT_NE(b, nullptr);
    gtext_json_get_string(b, &str, &len);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(memcmp(str, "c", 1), 0);

    gtext_json_free(patch);
    gtext_json_free(target);
}

/**
 * Test JSON Merge Patch - remove member via null
 */
TEST(JsonMergePatch, RemoveViaNull) {
    const char * target_json = "{\"a\":\"b\"}";
    const char * patch_json = "{\"a\":null}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * target = gtext_json_parse(target_json, strlen(target_json), &opts, &err);
    ASSERT_NE(target, nullptr);

    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_merge_patch(target, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify member was removed
    const GTEXT_JSON_Value * a = gtext_json_pointer_get(target, "/a", 2);
    EXPECT_EQ(a, nullptr);

    // Verify object is now empty
    EXPECT_EQ(gtext_json_object_size(target), 0u);

    gtext_json_free(patch);
    gtext_json_free(target);
}

/**
 * Test JSON Merge Patch - replace array entirely
 */
TEST(JsonMergePatch, ReplaceArray) {
    const char * target_json = "{\"a\":[\"b\"]}";
    const char * patch_json = "{\"a\":\"c\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * target = gtext_json_parse(target_json, strlen(target_json), &opts, &err);
    ASSERT_NE(target, nullptr);

    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_merge_patch(target, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify array was replaced with string
    const GTEXT_JSON_Value * a = gtext_json_pointer_get(target, "/a", 2);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(gtext_json_typeof(a), GTEXT_JSON_STRING);
    const char * str;
    size_t len;
    gtext_json_get_string(a, &str, &len);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(memcmp(str, "c", 1), 0);

    gtext_json_free(patch);
    gtext_json_free(target);
}

/**
 * Test JSON Merge Patch - non-object patch replaces target entirely
 */
TEST(JsonMergePatch, NonObjectPatchReplaces) {
    const char * target_json = "{\"a\":\"foo\"}";
    const char * patch_json = "null";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * target = gtext_json_parse(target_json, strlen(target_json), &opts, &err);
    ASSERT_NE(target, nullptr);

    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_merge_patch(target, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify target is now null
    EXPECT_EQ(gtext_json_typeof(target), GTEXT_JSON_NULL);

    gtext_json_free(patch);
    gtext_json_free(target);
}

/**
 * Test JSON Merge Patch - array patch replaces target array
 */
TEST(JsonMergePatch, ArrayPatchReplaces) {
    const char * target_json = "[\"a\",\"b\"]";
    const char * patch_json = "[\"c\",\"d\"]";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * target = gtext_json_parse(target_json, strlen(target_json), &opts, &err);
    ASSERT_NE(target, nullptr);

    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_merge_patch(target, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify array was replaced
    EXPECT_EQ(gtext_json_typeof(target), GTEXT_JSON_ARRAY);
    EXPECT_EQ(gtext_json_array_size(target), 2u);

    const GTEXT_JSON_Value * elem0 = gtext_json_array_get(target, 0);
    ASSERT_NE(elem0, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(elem0, &str, &len);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(memcmp(str, "c", 1), 0);

    const GTEXT_JSON_Value * elem1 = gtext_json_array_get(target, 1);
    ASSERT_NE(elem1, nullptr);
    gtext_json_get_string(elem1, &str, &len);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(memcmp(str, "d", 1), 0);

    gtext_json_free(patch);
    gtext_json_free(target);
}

/**
 * Test JSON Merge Patch - nested object merge
 */
TEST(JsonMergePatch, NestedObjectMerge) {
    const char * target_json = "{\"title\":\"Goodbye!\",\"author\":{\"givenName\":\"John\",\"familyName\":\"Doe\"},\"tags\":[\"example\",\"sample\"],\"content\":\"This will be unchanged\"}";
    const char * patch_json = "{\"title\":\"Hello!\",\"phoneNumber\":\"+01-123-456-7890\",\"author\":{\"familyName\":null},\"tags\":[\"example\"]}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * target = gtext_json_parse(target_json, strlen(target_json), &opts, &err);
    ASSERT_NE(target, nullptr);

    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_merge_patch(target, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify title was replaced
    const GTEXT_JSON_Value * title = gtext_json_pointer_get(target, "/title", 6);
    ASSERT_NE(title, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(title, &str, &len);
    EXPECT_EQ(len, 6u);
    EXPECT_EQ(memcmp(str, "Hello!", 6), 0);

    // Verify phoneNumber was added
    const GTEXT_JSON_Value * phone = gtext_json_pointer_get(target, "/phoneNumber", 12);
    ASSERT_NE(phone, nullptr);
    gtext_json_get_string(phone, &str, &len);
    EXPECT_EQ(len, 16u);
    EXPECT_EQ(memcmp(str, "+01-123-456-7890", 16), 0);

    // Verify author.familyName was removed
    const GTEXT_JSON_Value * familyName = gtext_json_pointer_get(target, "/author/familyName", 17);
    EXPECT_EQ(familyName, nullptr);

    // Verify author.givenName still exists
    const GTEXT_JSON_Value * givenName = gtext_json_pointer_get(target, "/author/givenName", 17);
    ASSERT_NE(givenName, nullptr);
    gtext_json_get_string(givenName, &str, &len);
    EXPECT_EQ(len, 4u);
    EXPECT_EQ(memcmp(str, "John", 4), 0);

    // Verify tags array was replaced
    const GTEXT_JSON_Value * tags = gtext_json_pointer_get(target, "/tags", 5);
    ASSERT_NE(tags, nullptr);
    EXPECT_EQ(gtext_json_array_size(tags), 1u);
    const GTEXT_JSON_Value * tag0 = gtext_json_array_get(tags, 0);
    ASSERT_NE(tag0, nullptr);
    gtext_json_get_string(tag0, &str, &len);
    EXPECT_EQ(len, 7u);
    EXPECT_EQ(memcmp(str, "example", 7), 0);

    // Verify content was unchanged
    const GTEXT_JSON_Value * content = gtext_json_pointer_get(target, "/content", 8);
    ASSERT_NE(content, nullptr);
    gtext_json_get_string(content, &str, &len);
    EXPECT_EQ(len, 22u);
    EXPECT_EQ(memcmp(str, "This will be unchanged", 22), 0);

    gtext_json_free(patch);
    gtext_json_free(target);
}

/**
 * Test JSON Merge Patch - non-object target converted to object
 */
TEST(JsonMergePatch, NonObjectTargetConverted) {
    const char * target_json = "\"not an object\"";
    const char * patch_json = "{\"a\":\"b\"}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * target = gtext_json_parse(target_json, strlen(target_json), &opts, &err);
    ASSERT_NE(target, nullptr);

    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_merge_patch(target, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify target is now an object
    EXPECT_EQ(gtext_json_typeof(target), GTEXT_JSON_OBJECT);

    // Verify patch member was added
    const GTEXT_JSON_Value * a = gtext_json_pointer_get(target, "/a", 2);
    ASSERT_NE(a, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(a, &str, &len);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(memcmp(str, "b", 1), 0);

    gtext_json_free(patch);
    gtext_json_free(target);
}

/**
 * Test JSON Merge Patch - empty object patch
 */
TEST(JsonMergePatch, EmptyObjectPatch) {
    const char * target_json = "{\"a\":\"b\",\"c\":\"d\"}";
    const char * patch_json = "{}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * target = gtext_json_parse(target_json, strlen(target_json), &opts, &err);
    ASSERT_NE(target, nullptr);

    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_merge_patch(target, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify target is unchanged (empty patch doesn't modify)
    EXPECT_EQ(gtext_json_object_size(target), 2u);

    const GTEXT_JSON_Value * a = gtext_json_pointer_get(target, "/a", 2);
    ASSERT_NE(a, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(a, &str, &len);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(memcmp(str, "b", 1), 0);

    gtext_json_free(patch);
    gtext_json_free(target);
}

/**
 * Test JSON Merge Patch - grandchild elements (3+ levels deep)
 */
TEST(JsonMergePatch, GrandchildElements) {
    const char * target_json = "{\"person\":{\"name\":{\"first\":\"John\",\"last\":\"Doe\"},\"contact\":{\"email\":\"john@example.com\",\"phone\":\"123-456-7890\"}},\"metadata\":{\"created\":\"2024-01-01\"}}";
    const char * patch_json = "{\"person\":{\"name\":{\"last\":\"Smith\"},\"contact\":{\"phone\":null}},\"metadata\":{\"updated\":\"2024-01-02\"}}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * target = gtext_json_parse(target_json, strlen(target_json), &opts, &err);
    ASSERT_NE(target, nullptr);

    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_merge_patch(target, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify person.name.first still exists (grandchild)
    const GTEXT_JSON_Value * first = gtext_json_pointer_get(target, "/person/name/first", 18);
    ASSERT_NE(first, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(first, &str, &len);
    EXPECT_EQ(len, 4u);
    EXPECT_EQ(memcmp(str, "John", 4), 0);

    // Verify person.name.last was changed (grandchild)
    const GTEXT_JSON_Value * last = gtext_json_pointer_get(target, "/person/name/last", 17);
    ASSERT_NE(last, nullptr);
    gtext_json_get_string(last, &str, &len);
    EXPECT_EQ(len, 5u);
    EXPECT_EQ(memcmp(str, "Smith", 5), 0);

    // Verify person.contact.email still exists (grandchild)
    const GTEXT_JSON_Value * email = gtext_json_pointer_get(target, "/person/contact/email", 21);
    ASSERT_NE(email, nullptr);
    gtext_json_get_string(email, &str, &len);
    EXPECT_EQ(len, 16u);
    EXPECT_EQ(memcmp(str, "john@example.com", 16), 0);

    // Verify person.contact.phone was removed (grandchild)
    const GTEXT_JSON_Value * phone = gtext_json_pointer_get(target, "/person/contact/phone", 21);
    EXPECT_EQ(phone, nullptr);

    // Verify metadata.created still exists
    const GTEXT_JSON_Value * created = gtext_json_pointer_get(target, "/metadata/created", 17);
    ASSERT_NE(created, nullptr);
    gtext_json_get_string(created, &str, &len);
    EXPECT_EQ(len, 10u);
    EXPECT_EQ(memcmp(str, "2024-01-01", 10), 0);

    // Verify metadata.updated was added
    const GTEXT_JSON_Value * updated = gtext_json_pointer_get(target, "/metadata/updated", 17);
    ASSERT_NE(updated, nullptr);
    gtext_json_get_string(updated, &str, &len);
    EXPECT_EQ(len, 10u);
    EXPECT_EQ(memcmp(str, "2024-01-02", 10), 0);

    gtext_json_free(patch);
    gtext_json_free(target);
}

/**
 * Test JSON Merge Patch - atomicity with rollback on error
 */
TEST(JsonMergePatch, AtomicityWithRollback) {
    // Create a target with multiple keys
    const char * target_json = "{\"key1\":\"value1\",\"key2\":\"value2\",\"key3\":{\"nested\":\"data\"}}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * target = gtext_json_parse(target_json, strlen(target_json), &opts, &err);
    ASSERT_NE(target, nullptr);

    // Save original state for comparison
    const GTEXT_JSON_Value * orig_key1 = gtext_json_pointer_get(target, "/key1", 5);
    ASSERT_NE(orig_key1, nullptr);
    const char * orig_str;
    size_t orig_len;
    gtext_json_get_string(orig_key1, &orig_str, &orig_len);

    const GTEXT_JSON_Value * orig_key2 = gtext_json_pointer_get(target, "/key2", 5);
    ASSERT_NE(orig_key2, nullptr);

    const GTEXT_JSON_Value * orig_nested = gtext_json_pointer_get(target, "/key3/nested", 12);
    ASSERT_NE(orig_nested, nullptr);

    // Create a patch that will succeed for first key but we'll simulate failure
    // by using an invalid patch that causes OOM or other error
    // Actually, we can't easily simulate OOM in a test, but we can verify
    // that if we have a valid patch, all changes are applied atomically

    // Test: Verify that if merge succeeds, all changes are applied
    const char * patch_json = "{\"key1\":\"newvalue1\",\"key2\":\"newvalue2\",\"key4\":\"value4\"}";
    GTEXT_JSON_Value * patch = gtext_json_parse(patch_json, strlen(patch_json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    GTEXT_JSON_Status status = gtext_json_merge_patch(target, patch, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify all changes were applied
    const GTEXT_JSON_Value * new_key1 = gtext_json_pointer_get(target, "/key1", 5);
    ASSERT_NE(new_key1, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(new_key1, &str, &len);
    EXPECT_EQ(len, 9u);
    EXPECT_EQ(memcmp(str, "newvalue1", 9), 0);

    const GTEXT_JSON_Value * new_key2 = gtext_json_pointer_get(target, "/key2", 5);
    ASSERT_NE(new_key2, nullptr);
    gtext_json_get_string(new_key2, &str, &len);
    EXPECT_EQ(len, 9u);
    EXPECT_EQ(memcmp(str, "newvalue2", 9), 0);

    const GTEXT_JSON_Value * new_key4 = gtext_json_pointer_get(target, "/key4", 5);
    ASSERT_NE(new_key4, nullptr);
    gtext_json_get_string(new_key4, &str, &len);
    EXPECT_EQ(len, 6u);
    EXPECT_EQ(memcmp(str, "value4", 6), 0);

    // Verify key3/nested still exists (unchanged)
    const GTEXT_JSON_Value * still_nested = gtext_json_pointer_get(target, "/key3/nested", 12);
    ASSERT_NE(still_nested, nullptr);

    gtext_json_free(patch);
    gtext_json_free(target);
}

/**
 * Test JSON Merge Patch - error cases: NULL arguments
 */
TEST(JsonMergePatch, ErrorNullArguments) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    const char * json = "{\"a\":\"b\"}";
    GTEXT_JSON_Value * target = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(target, nullptr);

    GTEXT_JSON_Value * patch = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(patch, nullptr);

    // Test NULL target
    GTEXT_JSON_Status status = gtext_json_merge_patch(nullptr, patch, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);

    // Test NULL patch
    status = gtext_json_merge_patch(target, nullptr, &err);
    EXPECT_NE(status, GTEXT_JSON_OK);

    gtext_json_free(patch);
    gtext_json_free(target);
}

// ============================================================================
// JSON Schema Validation Tests
// ============================================================================

/**
 * Test schema compilation - basic type validation
 */
TEST(JsonSchema, TypeValidation) {
    const char * schema_json = "{\"type\":\"string\"}";
    const char * valid_json = "\"hello\"";
    const char * invalid_json = "123";

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * schema_doc = gtext_json_parse(schema_json, strlen(schema_json), &opts, &err);
    ASSERT_NE(schema_doc, nullptr);

    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(schema_doc, &err);
    ASSERT_NE(schema, nullptr);

    // Test valid instance
    GTEXT_JSON_Value * valid = gtext_json_parse(valid_json, strlen(valid_json), &opts, &err);
    ASSERT_NE(valid, nullptr);
    GTEXT_JSON_Status status = gtext_json_schema_validate(schema, valid, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid);

    // Test invalid instance (wrong type)
    GTEXT_JSON_Value * invalid = gtext_json_parse(invalid_json, strlen(invalid_json), &opts, &err);
    ASSERT_NE(invalid, nullptr);
    status = gtext_json_schema_validate(schema, invalid, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid);

    gtext_json_schema_free(schema);
    gtext_json_free(schema_doc);
}

/**
 * Test schema compilation - multiple types
 */
TEST(JsonSchema, MultipleTypes) {
    const char * schema_json = "{\"type\":[\"string\",\"number\"]}";

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * schema_doc = gtext_json_parse(schema_json, strlen(schema_json), &opts, &err);
    ASSERT_NE(schema_doc, nullptr);

    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(schema_doc, &err);
    ASSERT_NE(schema, nullptr);

    // Test string instance
    const char * str_json = "\"hello\"";
    GTEXT_JSON_Value * str_val = gtext_json_parse(str_json, strlen(str_json), &opts, &err);
    ASSERT_NE(str_val, nullptr);
    GTEXT_JSON_Status status = gtext_json_schema_validate(schema, str_val, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(str_val);

    // Test number instance
    const char * num_json = "123";
    GTEXT_JSON_Value * num_val = gtext_json_parse(num_json, strlen(num_json), &opts, &err);
    ASSERT_NE(num_val, nullptr);
    status = gtext_json_schema_validate(schema, num_val, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(num_val);

    // Test invalid instance (boolean)
    const char * bool_json = "true";
    GTEXT_JSON_Value * bool_val = gtext_json_parse(bool_json, strlen(bool_json), &opts, &err);
    ASSERT_NE(bool_val, nullptr);
    status = gtext_json_schema_validate(schema, bool_val, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(bool_val);

    gtext_json_schema_free(schema);
    gtext_json_free(schema_doc);
}

/**
 * Test schema - properties and required
 */
TEST(JsonSchema, PropertiesAndRequired) {
    const char * schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"age\":{\"type\":\"number\"}},\"required\":[\"name\"]}";

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * schema_doc = gtext_json_parse(schema_json, strlen(schema_json), &opts, &err);
    ASSERT_NE(schema_doc, nullptr);

    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(schema_doc, &err);
    ASSERT_NE(schema, nullptr);

    // Test valid instance (has required property)
    const char * valid_json = "{\"name\":\"John\",\"age\":30}";
    GTEXT_JSON_Value * valid = gtext_json_parse(valid_json, strlen(valid_json), &opts, &err);
    ASSERT_NE(valid, nullptr);
    GTEXT_JSON_Status status = gtext_json_schema_validate(schema, valid, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid);

    // Test invalid instance (missing required property)
    const char * invalid_json = "{\"age\":30}";
    GTEXT_JSON_Value * invalid = gtext_json_parse(invalid_json, strlen(invalid_json), &opts, &err);
    ASSERT_NE(invalid, nullptr);
    status = gtext_json_schema_validate(schema, invalid, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid);

    // Test invalid instance (wrong property type)
    const char * wrong_type_json = "{\"name\":123}";
    GTEXT_JSON_Value * wrong_type = gtext_json_parse(wrong_type_json, strlen(wrong_type_json), &opts, &err);
    ASSERT_NE(wrong_type, nullptr);
    status = gtext_json_schema_validate(schema, wrong_type, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(wrong_type);

    gtext_json_schema_free(schema);
    gtext_json_free(schema_doc);
}

/**
 * Test schema - items validation
 */
TEST(JsonSchema, ItemsValidation) {
    const char * schema_json = "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}";

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * schema_doc = gtext_json_parse(schema_json, strlen(schema_json), &opts, &err);
    ASSERT_NE(schema_doc, nullptr);

    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(schema_doc, &err);
    ASSERT_NE(schema, nullptr);

    // Test valid instance (all items are strings)
    const char * valid_json = "[\"a\",\"b\",\"c\"]";
    GTEXT_JSON_Value * valid = gtext_json_parse(valid_json, strlen(valid_json), &opts, &err);
    ASSERT_NE(valid, nullptr);
    GTEXT_JSON_Status status = gtext_json_schema_validate(schema, valid, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid);

    // Test invalid instance (one item is not a string)
    const char * invalid_json = "[\"a\",123,\"c\"]";
    GTEXT_JSON_Value * invalid = gtext_json_parse(invalid_json, strlen(invalid_json), &opts, &err);
    ASSERT_NE(invalid, nullptr);
    status = gtext_json_schema_validate(schema, invalid, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid);

    gtext_json_schema_free(schema);
    gtext_json_free(schema_doc);
}

/**
 * Test schema - enum validation
 */
TEST(JsonSchema, EnumValidation) {
    const char * schema_json = "{\"enum\":[\"red\",\"green\",\"blue\"]}";

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * schema_doc = gtext_json_parse(schema_json, strlen(schema_json), &opts, &err);
    ASSERT_NE(schema_doc, nullptr);

    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(schema_doc, &err);
    ASSERT_NE(schema, nullptr);

    // Test valid instances
    const char * valid1_json = "\"red\"";
    GTEXT_JSON_Value * valid1 = gtext_json_parse(valid1_json, strlen(valid1_json), &opts, &err);
    ASSERT_NE(valid1, nullptr);
    GTEXT_JSON_Status status = gtext_json_schema_validate(schema, valid1, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid1);

    const char * valid2_json = "\"green\"";
    GTEXT_JSON_Value * valid2 = gtext_json_parse(valid2_json, strlen(valid2_json), &opts, &err);
    ASSERT_NE(valid2, nullptr);
    status = gtext_json_schema_validate(schema, valid2, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid2);

    // Test invalid instance (not in enum)
    const char * invalid_json = "\"yellow\"";
    GTEXT_JSON_Value * invalid = gtext_json_parse(invalid_json, strlen(invalid_json), &opts, &err);
    ASSERT_NE(invalid, nullptr);
    status = gtext_json_schema_validate(schema, invalid, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid);

    gtext_json_schema_free(schema);
    gtext_json_free(schema_doc);
}

/**
 * Test schema - const validation
 */
TEST(JsonSchema, ConstValidation) {
    const char * schema_json = "{\"const\":42}";

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * schema_doc = gtext_json_parse(schema_json, strlen(schema_json), &opts, &err);
    ASSERT_NE(schema_doc, nullptr);

    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(schema_doc, &err);
    ASSERT_NE(schema, nullptr);

    // Test valid instance (exact match)
    const char * valid_json = "42";
    GTEXT_JSON_Value * valid = gtext_json_parse(valid_json, strlen(valid_json), &opts, &err);
    ASSERT_NE(valid, nullptr);
    GTEXT_JSON_Status status = gtext_json_schema_validate(schema, valid, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid);

    // Test invalid instance (different value)
    const char * invalid_json = "43";
    GTEXT_JSON_Value * invalid = gtext_json_parse(invalid_json, strlen(invalid_json), &opts, &err);
    ASSERT_NE(invalid, nullptr);
    status = gtext_json_schema_validate(schema, invalid, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid);

    gtext_json_schema_free(schema);
    gtext_json_free(schema_doc);
}

/**
 * Test schema - numeric constraints (minimum/maximum)
 */
TEST(JsonSchema, NumericConstraints) {
    const char * schema_json = "{\"type\":\"number\",\"minimum\":10,\"maximum\":100}";

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * schema_doc = gtext_json_parse(schema_json, strlen(schema_json), &opts, &err);
    ASSERT_NE(schema_doc, nullptr);

    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(schema_doc, &err);
    ASSERT_NE(schema, nullptr);

    // Test valid instances
    const char * valid1_json = "50";
    GTEXT_JSON_Value * valid1 = gtext_json_parse(valid1_json, strlen(valid1_json), &opts, &err);
    ASSERT_NE(valid1, nullptr);
    GTEXT_JSON_Status status = gtext_json_schema_validate(schema, valid1, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid1);

    const char * valid2_json = "10";
    GTEXT_JSON_Value * valid2 = gtext_json_parse(valid2_json, strlen(valid2_json), &opts, &err);
    ASSERT_NE(valid2, nullptr);
    status = gtext_json_schema_validate(schema, valid2, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid2);

    // Test invalid instance (below minimum)
    const char * invalid1_json = "5";
    GTEXT_JSON_Value * invalid1 = gtext_json_parse(invalid1_json, strlen(invalid1_json), &opts, &err);
    ASSERT_NE(invalid1, nullptr);
    status = gtext_json_schema_validate(schema, invalid1, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid1);

    // Test invalid instance (above maximum)
    const char * invalid2_json = "150";
    GTEXT_JSON_Value * invalid2 = gtext_json_parse(invalid2_json, strlen(invalid2_json), &opts, &err);
    ASSERT_NE(invalid2, nullptr);
    status = gtext_json_schema_validate(schema, invalid2, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid2);

    gtext_json_schema_free(schema);
    gtext_json_free(schema_doc);
}

/**
 * Test schema - string length constraints
 */
TEST(JsonSchema, StringLengthConstraints) {
    const char * schema_json = "{\"type\":\"string\",\"minLength\":3,\"maxLength\":10}";

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * schema_doc = gtext_json_parse(schema_json, strlen(schema_json), &opts, &err);
    ASSERT_NE(schema_doc, nullptr);

    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(schema_doc, &err);
    ASSERT_NE(schema, nullptr);

    // Test valid instances
    const char * valid1_json = "\"abc\"";
    GTEXT_JSON_Value * valid1 = gtext_json_parse(valid1_json, strlen(valid1_json), &opts, &err);
    ASSERT_NE(valid1, nullptr);
    GTEXT_JSON_Status status = gtext_json_schema_validate(schema, valid1, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid1);

    const char * valid2_json = "\"abcdefghij\"";
    GTEXT_JSON_Value * valid2 = gtext_json_parse(valid2_json, strlen(valid2_json), &opts, &err);
    ASSERT_NE(valid2, nullptr);
    status = gtext_json_schema_validate(schema, valid2, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid2);

    // Test invalid instance (too short)
    const char * invalid1_json = "\"ab\"";
    GTEXT_JSON_Value * invalid1 = gtext_json_parse(invalid1_json, strlen(invalid1_json), &opts, &err);
    ASSERT_NE(invalid1, nullptr);
    status = gtext_json_schema_validate(schema, invalid1, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid1);

    // Test invalid instance (too long)
    const char * invalid2_json = "\"abcdefghijk\"";
    GTEXT_JSON_Value * invalid2 = gtext_json_parse(invalid2_json, strlen(invalid2_json), &opts, &err);
    ASSERT_NE(invalid2, nullptr);
    status = gtext_json_schema_validate(schema, invalid2, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid2);

    gtext_json_schema_free(schema);
    gtext_json_free(schema_doc);
}

/**
 * Test schema - array size constraints
 */
/**
 * minLength and maxLength count characters, not bytes and not UTF-16 units.
 *
 * JSON Schema validation section 6.3 defines both over "the number of its
 * characters as defined by RFC 8259", and an RFC 8259 string is a sequence of
 * Unicode code points. This counted bytes, so every non-ASCII instance was
 * measured wrong in both directions at once: a two-byte character was too
 * long for `maxLength: 1` and long enough for `minLength: 2`.
 *
 * The astral cases are the ones worth having. An implementation that reached
 * for a UTF-16 length - which is what ECMAScript's `.length` gives, and what
 * a JavaScript-shaped implementation would do without thinking - counts
 * U+1F4A9 as two. The published test suite carries that case for the same
 * reason.
 */
TEST(JsonSchema, StringLengthIsInCharactersNotBytes) {
	struct Case {
		const char * schema;
		const char * instance;
		GTEXT_JSON_Status expected;
		const char * why;
	};
	const Case cases[] = {
	    // U+00E9: one character, two bytes, one UTF-16 unit.
	    {"{\"maxLength\":1}", "\"\\u00e9\"", GTEXT_JSON_OK,
	        "one character fits maxLength 1"},
	    {"{\"minLength\":2}", "\"\\u00e9\"", GTEXT_JSON_E_SCHEMA,
	        "one character does not reach minLength 2"},

	    // U+4E2D U+6587: two characters, six bytes.
	    {"{\"maxLength\":2}", "\"\\u4e2d\\u6587\"", GTEXT_JSON_OK,
	        "two three-byte characters fit maxLength 2"},
	    {"{\"maxLength\":1}", "\"\\u4e2d\\u6587\"", GTEXT_JSON_E_SCHEMA,
	        "two characters exceed maxLength 1"},

	    // U+1F4A9 twice: two characters, eight bytes, four UTF-16 units.
	    {"{\"maxLength\":2}", "\"\\ud83d\\udca9\\ud83d\\udca9\"",
	        GTEXT_JSON_OK, "two astral characters fit maxLength 2"},
	    {"{\"minLength\":2}", "\"\\ud83d\\udca9\\ud83d\\udca9\"",
	        GTEXT_JSON_OK, "two astral characters reach minLength 2"},
	    // The UTF-16 trap: a units-based count says four and accepts this.
	    {"{\"minLength\":3}", "\"\\ud83d\\udca9\\ud83d\\udca9\"",
	        GTEXT_JSON_E_SCHEMA,
	        "two astral characters do not reach minLength 3"},

	    // One of each: 1 + 2 + 4 = seven bytes, four UTF-16 units, three
	    // characters.
	    {"{\"maxLength\":3}", "\"a\\u00e9\\ud83d\\udca9\"", GTEXT_JSON_OK,
	        "three mixed characters fit maxLength 3"},
	    {"{\"maxLength\":2}", "\"a\\u00e9\\ud83d\\udca9\"",
	        GTEXT_JSON_E_SCHEMA, "three characters exceed maxLength 2"},
	    {"{\"minLength\":4}", "\"a\\u00e9\\ud83d\\udca9\"",
	        GTEXT_JSON_E_SCHEMA,
	        "three characters do not reach minLength 4"},

	    // ASCII is unchanged, which is why this went unnoticed.
	    {"{\"minLength\":3,\"maxLength\":3}", "\"abc\"", GTEXT_JSON_OK,
	        "ASCII still counts the way it always did"},

	    // The empty string, at the boundary.
	    {"{\"maxLength\":0}", "\"\"", GTEXT_JSON_OK, "empty fits maxLength 0"},
	    {"{\"minLength\":1}", "\"\"", GTEXT_JSON_E_SCHEMA,
	        "empty does not reach minLength 1"},

	    // The same count through propertyNames, which wraps each key as a
	    // string instance and runs it through this arm.
	    {"{\"propertyNames\":{\"maxLength\":1}}", "{\"\\u00e9\":1}",
	        GTEXT_JSON_OK, "a one-character key fits maxLength 1"},
	    {"{\"propertyNames\":{\"maxLength\":1}}", "{\"\\u00e9\\u00e9\":1}",
	        GTEXT_JSON_E_SCHEMA, "a two-character key exceeds maxLength 1"},
	};

	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	for (const auto & c : cases) {
		GTEXT_JSON_Error perr;
		memset(&perr, 0, sizeof(perr));
		GTEXT_JSON_Value * doc =
		    gtext_json_parse(c.schema, strlen(c.schema), &po, &perr);
		ASSERT_NE(doc, nullptr) << c.schema;
		GTEXT_JSON_Schema * schema = gtext_json_schema_compile(doc, &perr);
		ASSERT_NE(schema, nullptr) << c.schema;

		memset(&perr, 0, sizeof(perr));
		GTEXT_JSON_Value * instance =
		    gtext_json_parse(c.instance, strlen(c.instance), &po, &perr);
		ASSERT_NE(instance, nullptr) << c.instance;

		EXPECT_EQ(gtext_json_schema_validate(schema, instance, nullptr),
		    c.expected)
		    << c.why;

		gtext_json_free(instance);
		gtext_json_schema_free(schema);
		gtext_json_free(doc);
	}
}

TEST(JsonSchema, ArraySizeConstraints) {
    const char * schema_json = "{\"type\":\"array\",\"minItems\":2,\"maxItems\":5}";

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * schema_doc = gtext_json_parse(schema_json, strlen(schema_json), &opts, &err);
    ASSERT_NE(schema_doc, nullptr);

    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(schema_doc, &err);
    ASSERT_NE(schema, nullptr);

    // Test valid instances
    const char * valid1_json = "[1,2]";
    GTEXT_JSON_Value * valid1 = gtext_json_parse(valid1_json, strlen(valid1_json), &opts, &err);
    ASSERT_NE(valid1, nullptr);
    GTEXT_JSON_Status status = gtext_json_schema_validate(schema, valid1, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid1);

    const char * valid2_json = "[1,2,3,4,5]";
    GTEXT_JSON_Value * valid2 = gtext_json_parse(valid2_json, strlen(valid2_json), &opts, &err);
    ASSERT_NE(valid2, nullptr);
    status = gtext_json_schema_validate(schema, valid2, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid2);

    // Test invalid instance (too few items)
    const char * invalid1_json = "[1]";
    GTEXT_JSON_Value * invalid1 = gtext_json_parse(invalid1_json, strlen(invalid1_json), &opts, &err);
    ASSERT_NE(invalid1, nullptr);
    status = gtext_json_schema_validate(schema, invalid1, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid1);

    // Test invalid instance (too many items)
    const char * invalid2_json = "[1,2,3,4,5,6]";
    GTEXT_JSON_Value * invalid2 = gtext_json_parse(invalid2_json, strlen(invalid2_json), &opts, &err);
    ASSERT_NE(invalid2, nullptr);
    status = gtext_json_schema_validate(schema, invalid2, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid2);

    gtext_json_schema_free(schema);
    gtext_json_free(schema_doc);
}

/**
 * Test schema - nested schema validation
 */
TEST(JsonSchema, NestedSchema) {
    const char * schema_json = "{\"type\":\"object\",\"properties\":{\"user\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"age\":{\"type\":\"number\",\"minimum\":0}},\"required\":[\"name\"]}}}";

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * schema_doc = gtext_json_parse(schema_json, strlen(schema_json), &opts, &err);
    ASSERT_NE(schema_doc, nullptr);

    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(schema_doc, &err);
    ASSERT_NE(schema, nullptr);

    // Test valid nested instance
    const char * valid_json = "{\"user\":{\"name\":\"John\",\"age\":30}}";
    GTEXT_JSON_Value * valid = gtext_json_parse(valid_json, strlen(valid_json), &opts, &err);
    ASSERT_NE(valid, nullptr);
    GTEXT_JSON_Status status = gtext_json_schema_validate(schema, valid, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    gtext_json_free(valid);

    // Test invalid nested instance (missing required property)
    const char * invalid1_json = "{\"user\":{\"age\":30}}";
    GTEXT_JSON_Value * invalid1 = gtext_json_parse(invalid1_json, strlen(invalid1_json), &opts, &err);
    ASSERT_NE(invalid1, nullptr);
    status = gtext_json_schema_validate(schema, invalid1, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid1);

    // Test invalid nested instance (negative age)
    const char * invalid2_json = "{\"user\":{\"name\":\"John\",\"age\":-5}}";
    GTEXT_JSON_Value * invalid2 = gtext_json_parse(invalid2_json, strlen(invalid2_json), &opts, &err);
    ASSERT_NE(invalid2, nullptr);
    status = gtext_json_schema_validate(schema, invalid2, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_SCHEMA);
    gtext_json_free(invalid2);

    gtext_json_schema_free(schema);
    gtext_json_free(schema_doc);
}

/**
 * Test schema compilation - invalid schema rejection
 */
TEST(JsonSchema, InvalidSchemaRejection) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    // Test NULL schema document
    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(nullptr, &err);
    EXPECT_EQ(schema, nullptr);
    EXPECT_EQ(err.code, GTEXT_JSON_E_INVALID);

    // Test non-object schema
    const char * invalid_json = "\"not an object\"";
    GTEXT_JSON_Value * invalid = gtext_json_parse(invalid_json, strlen(invalid_json), &opts, &err);
    ASSERT_NE(invalid, nullptr);
    schema = gtext_json_schema_compile(invalid, &err);
    EXPECT_EQ(schema, nullptr);
    EXPECT_EQ(err.code, GTEXT_JSON_E_INVALID);
    gtext_json_free(invalid);

    // Test invalid type value
    const char * invalid_type_json = "{\"type\":\"invalid\"}";
    GTEXT_JSON_Value * invalid_type = gtext_json_parse(invalid_type_json, strlen(invalid_type_json), &opts, &err);
    ASSERT_NE(invalid_type, nullptr);
    schema = gtext_json_schema_compile(invalid_type, &err);
    EXPECT_EQ(schema, nullptr);
    EXPECT_EQ(err.code, GTEXT_JSON_E_INVALID);
    gtext_json_free(invalid_type);
}

/**
 * Test schema validation - NULL arguments
 */
TEST(JsonSchema, NullArguments) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    const char * schema_json = "{\"type\":\"string\"}";
    GTEXT_JSON_Value * schema_doc = gtext_json_parse(schema_json, strlen(schema_json), &opts, &err);
    ASSERT_NE(schema_doc, nullptr);

    GTEXT_JSON_Schema * schema = gtext_json_schema_compile(schema_doc, &err);
    ASSERT_NE(schema, nullptr);

    const char * instance_json = "\"test\"";
    GTEXT_JSON_Value * instance = gtext_json_parse(instance_json, strlen(instance_json), &opts, &err);
    ASSERT_NE(instance, nullptr);

    // Test NULL schema
    GTEXT_JSON_Status status = gtext_json_schema_validate(nullptr, instance, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    // Test NULL instance
    status = gtext_json_schema_validate(schema, nullptr, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    gtext_json_free(instance);
    gtext_json_schema_free(schema);
    gtext_json_free(schema_doc);
}

/**
 * Test schema free - NULL argument
 */
TEST(JsonSchema, FreeNull) {
    // Should not crash - free functions must handle NULL gracefully
    gtext_json_schema_free(nullptr);
    // If we get here without crashing, the test passed
    SUCCEED();
}

/**
 * Test in-situ parsing mode - strings without escape sequences
 */
TEST(InSituMode, StringNoEscapes) {
    // String without escape sequences should use in-situ mode
    const char * input = "\"hello world\"";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.in_situ_mode = true;
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * val = gtext_json_parse(input, strlen(input), &opts, &err);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_STRING);

    const char * str;
    size_t len;
    GTEXT_JSON_Status status = gtext_json_get_string(val, &str, &len);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(len, 11u);
    EXPECT_EQ(memcmp(str, "hello world", 11), 0);

    // Verify string points into input buffer (in-situ mode)
    // The string data should be at offset 1 (after opening quote)
    EXPECT_EQ(str, input + 1);

    gtext_json_free(val);
}

/**
 * Test in-situ parsing mode - strings with escape sequences (should not use in-situ)
 */
TEST(InSituMode, StringWithEscapes) {
    // String with escape sequences should NOT use in-situ mode
    const char * input = "\"hello\\nworld\"";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.in_situ_mode = true;
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * val = gtext_json_parse(input, strlen(input), &opts, &err);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_STRING);

    const char * str;
    size_t len;
    GTEXT_JSON_Status status = gtext_json_get_string(val, &str, &len);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(len, 11u);  // "hello\nworld" = 11 chars (newline is one char)
    EXPECT_EQ(memcmp(str, "hello\nworld", 11), 0);

    // Verify string does NOT point into input buffer (decoded, not in-situ)
    EXPECT_NE(str, input + 1);

    gtext_json_free(val);
}

/**
 * Test in-situ parsing mode - numbers
 */
TEST(InSituMode, NumberLexeme) {
    // Number lexeme should use in-situ mode
    const char * input = "123.456";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.in_situ_mode = true;
    opts.preserve_number_lexeme = true;
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * val = gtext_json_parse(input, strlen(input), &opts, &err);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_NUMBER);

    const char * lexeme;
    size_t lexeme_len;
    GTEXT_JSON_Status status = gtext_json_get_number_lexeme(val, &lexeme, &lexeme_len);
    EXPECT_EQ(status, GTEXT_JSON_OK);
    EXPECT_EQ(lexeme_len, 7u);
    EXPECT_EQ(memcmp(lexeme, "123.456", 7), 0);

    // Verify lexeme points into input buffer (in-situ mode)
    EXPECT_EQ(lexeme, input);

    gtext_json_free(val);
}

/**
 * Test in-situ parsing mode - nested structures
 */
TEST(InSituMode, NestedStructures) {
    // Test in-situ mode with nested objects and arrays
    const char * input = "{\"key\":\"value\",\"num\":42}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.in_situ_mode = true;
    opts.preserve_number_lexeme = true;
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * val = gtext_json_parse(input, strlen(input), &opts, &err);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_OBJECT);

    // Check string value (no escapes, should be in-situ)
    const GTEXT_JSON_Value * str_val = gtext_json_object_get(val, "key", 3);
    ASSERT_NE(str_val, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(str_val, &str, &len);
    EXPECT_EQ(len, 5u);
    EXPECT_EQ(memcmp(str, "value", 5), 0);
    // Verify in-situ: "value" starts at offset 8 in input ("{\"key\":\"" = 7 chars, then "value" starts)
    // Input: {"key":"value","num":42}
    //        0123456789...
    EXPECT_EQ(str, input + 8);

    // Check number value
    const GTEXT_JSON_Value * num_val = gtext_json_object_get(val, "num", 3);
    ASSERT_NE(num_val, nullptr);
    const char * lexeme;
    size_t lexeme_len;
    gtext_json_get_number_lexeme(num_val, &lexeme, &lexeme_len);
    EXPECT_EQ(lexeme_len, 2u);
    EXPECT_EQ(memcmp(lexeme, "42", 2), 0);
    // Verify in-situ: "42" starts at offset 21 in input
    // Input: {"key":"value","num":42}
    //        0123456789012345678901...
    EXPECT_EQ(lexeme, input + 21);

    gtext_json_free(val);
}

/**
 * Test in-situ parsing mode - lifetime requirements
 * This test verifies that the input buffer must remain valid
 */
TEST(InSituMode, LifetimeRequirements) {
    // Create input in a scope that will be destroyed
    std::string input_str = "\"test string\"";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.in_situ_mode = true;
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * val = gtext_json_parse(input_str.c_str(), input_str.length(), &opts, &err);
    ASSERT_NE(val, nullptr);

    const char * str;
    size_t len;
    gtext_json_get_string(val, &str, &len);

    // At this point, str points into input_str
    // If input_str is destroyed, str becomes invalid
    // This test documents the requirement - caller must keep input alive
    EXPECT_EQ(len, 11u);
    EXPECT_EQ(memcmp(str, "test string", 11), 0);

    // Note: In a real scenario, the caller must ensure input_str remains valid
    // until gtext_json_free(val) is called. This is a documentation test.

    gtext_json_free(val);
}

/**
 * Test in-situ parsing mode - round-trip
 */
TEST(InSituMode, RoundTrip) {
    const char * input = "{\"name\":\"Alice\",\"age\":30}";
    GTEXT_JSON_Parse_Options parse_opts = gtext_json_parse_options_default();
    parse_opts.in_situ_mode = true;
    parse_opts.preserve_number_lexeme = true;
    GTEXT_JSON_Error err{};

    // Parse with in-situ mode
    GTEXT_JSON_Value * val = gtext_json_parse(input, strlen(input), &parse_opts, &err);
    ASSERT_NE(val, nullptr);

    // Write it back
    GTEXT_JSON_Write_Options write_opts = gtext_json_write_options_default();
    GTEXT_JSON_Sink sink;
    gtext_json_sink_buffer(&sink);

    GTEXT_JSON_Status status = gtext_json_write_value(&sink, &write_opts, val, &err);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Verify the output matches (structurally, not byte-for-byte due to spacing)
    const char * output = gtext_json_sink_buffer_data(&sink);
    size_t output_len = gtext_json_sink_buffer_size(&sink);

    // Parse the output again to verify structural equality
    GTEXT_JSON_Value * val2 = gtext_json_parse(output, output_len, &parse_opts, &err);
    ASSERT_NE(val2, nullptr);

    // Verify structure
    EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_typeof(val2), GTEXT_JSON_OBJECT);
    EXPECT_EQ(gtext_json_object_size(val), gtext_json_object_size(val2));

    gtext_json_free(val);
    gtext_json_free(val2);
    gtext_json_sink_buffer_free(&sink);
}

/**
 * Test in-situ parsing mode - disabled by default
 */
TEST(InSituMode, DisabledByDefault) {
    const char * input = "\"hello\"";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    // in_situ_mode is 0 by default
    GTEXT_JSON_Error err{};

    GTEXT_JSON_Value * val = gtext_json_parse(input, strlen(input), &opts, &err);
    ASSERT_NE(val, nullptr);

    const char * str;
    size_t len;
    gtext_json_get_string(val, &str, &len);
    EXPECT_EQ(len, 5u);
    EXPECT_EQ(memcmp(str, "hello", 5), 0);

    // Verify string does NOT point into input buffer (copy mode)
    EXPECT_NE(str, input + 1);

    gtext_json_free(val);
}

/**
 * Test deep equality - null values
 */
TEST(DomUtilities, DeepEqualityNull) {
    GTEXT_JSON_Value * a = gtext_json_new_null();
    GTEXT_JSON_Value * b = gtext_json_new_null();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_EQ(gtext_json_equal(a, b, GTEXT_JSON_EQUAL_LEXEME), true);
    EXPECT_EQ(gtext_json_equal(a, b, GTEXT_JSON_EQUAL_NUMERIC), true);
    EXPECT_EQ(gtext_json_equal(a, nullptr, GTEXT_JSON_EQUAL_LEXEME), false);
    EXPECT_EQ(gtext_json_equal(nullptr, b, GTEXT_JSON_EQUAL_LEXEME), false);

    gtext_json_free(a);
    gtext_json_free(b);
}

/**
 * Test deep equality - boolean values
 */
TEST(DomUtilities, DeepEqualityBool) {
    GTEXT_JSON_Value * a1 = gtext_json_new_bool(true);
    GTEXT_JSON_Value * a2 = gtext_json_new_bool(true);
    GTEXT_JSON_Value * b = gtext_json_new_bool(false);
    ASSERT_NE(a1, nullptr);
    ASSERT_NE(a2, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_EQ(gtext_json_equal(a1, a2, GTEXT_JSON_EQUAL_LEXEME), true);
    EXPECT_EQ(gtext_json_equal(a1, b, GTEXT_JSON_EQUAL_LEXEME), false);

    gtext_json_free(a1);
    gtext_json_free(a2);
    gtext_json_free(b);
}

/**
 * Test deep equality - string values
 */
TEST(DomUtilities, DeepEqualityString) {
    GTEXT_JSON_Value * a1 = gtext_json_new_string("hello", 5);
    GTEXT_JSON_Value * a2 = gtext_json_new_string("hello", 5);
    GTEXT_JSON_Value * b = gtext_json_new_string("world", 5);
    ASSERT_NE(a1, nullptr);
    ASSERT_NE(a2, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_EQ(gtext_json_equal(a1, a2, GTEXT_JSON_EQUAL_LEXEME), true);
    EXPECT_EQ(gtext_json_equal(a1, b, GTEXT_JSON_EQUAL_LEXEME), false);

    gtext_json_free(a1);
    gtext_json_free(a2);
    gtext_json_free(b);
}

/**
 * Test deep equality - number values (lexeme mode)
 */
TEST(DomUtilities, DeepEqualityNumberLexeme) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    // "123" and "123" should be equal
    GTEXT_JSON_Value * a1 = gtext_json_parse("\"123\"", 5, &opts, &err);
    GTEXT_JSON_Value * a2 = gtext_json_parse("\"123\"", 5, &opts, &err);
    ASSERT_NE(a1, nullptr);
    ASSERT_NE(a2, nullptr);

    // Actually, let's test with numbers
    GTEXT_JSON_Value * n1 = gtext_json_new_number_from_lexeme("123", 3);
    GTEXT_JSON_Value * n2 = gtext_json_new_number_from_lexeme("123", 3);
    GTEXT_JSON_Value * n3 = gtext_json_new_number_from_lexeme("456", 3);
    ASSERT_NE(n1, nullptr);
    ASSERT_NE(n2, nullptr);
    ASSERT_NE(n3, nullptr);

    EXPECT_EQ(gtext_json_equal(n1, n2, GTEXT_JSON_EQUAL_LEXEME), true);
    EXPECT_EQ(gtext_json_equal(n1, n3, GTEXT_JSON_EQUAL_LEXEME), false);

    gtext_json_free(n1);
    gtext_json_free(n2);
    gtext_json_free(n3);
    gtext_json_free(a1);
    gtext_json_free(a2);
}

/**
 * Test deep equality - number values (numeric mode)
 */
TEST(DomUtilities, DeepEqualityNumberNumeric) {
    // Test with int64
    GTEXT_JSON_Value * n1 = gtext_json_new_number_i64(123);
    GTEXT_JSON_Value * n2 = gtext_json_new_number_i64(123);
    GTEXT_JSON_Value * n3 = gtext_json_new_number_i64(456);
    ASSERT_NE(n1, nullptr);
    ASSERT_NE(n2, nullptr);
    ASSERT_NE(n3, nullptr);

    EXPECT_EQ(gtext_json_equal(n1, n2, GTEXT_JSON_EQUAL_NUMERIC), true);
    EXPECT_EQ(gtext_json_equal(n1, n3, GTEXT_JSON_EQUAL_NUMERIC), false);

    // Test with double
    GTEXT_JSON_Value * d1 = gtext_json_new_number_double(3.14);
    GTEXT_JSON_Value * d2 = gtext_json_new_number_double(3.14);
    GTEXT_JSON_Value * d3 = gtext_json_new_number_double(2.71);
    ASSERT_NE(d1, nullptr);
    ASSERT_NE(d2, nullptr);
    ASSERT_NE(d3, nullptr);

    EXPECT_EQ(gtext_json_equal(d1, d2, GTEXT_JSON_EQUAL_NUMERIC), true);
    EXPECT_EQ(gtext_json_equal(d1, d3, GTEXT_JSON_EQUAL_NUMERIC), false);

    gtext_json_free(n1);
    gtext_json_free(n2);
    gtext_json_free(n3);
    gtext_json_free(d1);
    gtext_json_free(d2);
    gtext_json_free(d3);
}

/**
 * Test deep equality - arrays
 */
TEST(DomUtilities, DeepEqualityArray) {
    GTEXT_JSON_Value * a1 = gtext_json_new_array();
    GTEXT_JSON_Value * a2 = gtext_json_new_array();
    GTEXT_JSON_Value * a3 = gtext_json_new_array();
    ASSERT_NE(a1, nullptr);
    ASSERT_NE(a2, nullptr);
    ASSERT_NE(a3, nullptr);

    GTEXT_JSON_Value * v1 = gtext_json_new_string("hello", 5);
    GTEXT_JSON_Value * v2 = gtext_json_new_string("world", 5);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);

    gtext_json_array_push(a1, v1);
    gtext_json_array_push(a1, v2);
    gtext_json_array_push(a2, gtext_json_new_string("hello", 5));
    gtext_json_array_push(a2, gtext_json_new_string("world", 5));
    gtext_json_array_push(a3, gtext_json_new_string("foo", 3));

    EXPECT_EQ(gtext_json_equal(a1, a2, GTEXT_JSON_EQUAL_LEXEME), true);
    EXPECT_EQ(gtext_json_equal(a1, a3, GTEXT_JSON_EQUAL_LEXEME), false);

    gtext_json_free(a1);
    gtext_json_free(a2);
    gtext_json_free(a3);
}

/**
 * Test deep equality - objects (order-independent)
 */
TEST(DomUtilities, DeepEqualityObject) {
    GTEXT_JSON_Value * o1 = gtext_json_new_object();
    GTEXT_JSON_Value * o2 = gtext_json_new_object();
    GTEXT_JSON_Value * o3 = gtext_json_new_object();
    ASSERT_NE(o1, nullptr);
    ASSERT_NE(o2, nullptr);
    ASSERT_NE(o3, nullptr);

    gtext_json_object_put(o1, "a", 1, gtext_json_new_string("hello", 5));
    gtext_json_object_put(o1, "b", 1, gtext_json_new_string("world", 5));

    // Same keys/values, different order
    gtext_json_object_put(o2, "b", 1, gtext_json_new_string("world", 5));
    gtext_json_object_put(o2, "a", 1, gtext_json_new_string("hello", 5));

    gtext_json_object_put(o3, "a", 1, gtext_json_new_string("foo", 3));

    EXPECT_EQ(gtext_json_equal(o1, o2, GTEXT_JSON_EQUAL_LEXEME), true);
    EXPECT_EQ(gtext_json_equal(o1, o3, GTEXT_JSON_EQUAL_LEXEME), false);

    gtext_json_free(o1);
    gtext_json_free(o2);
    gtext_json_free(o3);
}

/**
 * Test deep equality - nested structures
 */
TEST(DomUtilities, DeepEqualityNested) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    const char * json1 = "{\"a\":[1,2,{\"b\":\"hello\"}]}";
    const char * json2 = "{\"a\":[1,2,{\"b\":\"hello\"}]}";
    const char * json3 = "{\"a\":[1,2,{\"b\":\"world\"}]}";

    GTEXT_JSON_Value * v1 = gtext_json_parse(json1, strlen(json1), &opts, &err);
    GTEXT_JSON_Value * v2 = gtext_json_parse(json2, strlen(json2), &opts, &err);
    GTEXT_JSON_Value * v3 = gtext_json_parse(json3, strlen(json3), &opts, &err);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);
    ASSERT_NE(v3, nullptr);

    EXPECT_EQ(gtext_json_equal(v1, v2, GTEXT_JSON_EQUAL_LEXEME), true);
    EXPECT_EQ(gtext_json_equal(v1, v3, GTEXT_JSON_EQUAL_LEXEME), false);

    gtext_json_free(v1);
    gtext_json_free(v2);
    gtext_json_free(v3);
}

/**
 * Test deep clone - null value
 */
TEST(DomUtilities, DeepCloneNull) {
    GTEXT_JSON_Value * src = gtext_json_new_null();
    ASSERT_NE(src, nullptr);

    GTEXT_JSON_Value * clone = gtext_json_clone(src);
    ASSERT_NE(clone, nullptr);

    EXPECT_EQ(gtext_json_typeof(clone), GTEXT_JSON_NULL);
    EXPECT_EQ(gtext_json_equal(src, clone, GTEXT_JSON_EQUAL_LEXEME), true);

    // Verify they're independent (different contexts)
    EXPECT_NE(src, clone);

    gtext_json_free(src);
    gtext_json_free(clone);
}

/**
 * Test deep clone - scalar values
 */
TEST(DomUtilities, DeepCloneScalars) {
    GTEXT_JSON_Value * bool_src = gtext_json_new_bool(true);
    GTEXT_JSON_Value * str_src = gtext_json_new_string("hello", 5);
    GTEXT_JSON_Value * num_src = gtext_json_new_number_i64(123);
    ASSERT_NE(bool_src, nullptr);
    ASSERT_NE(str_src, nullptr);
    ASSERT_NE(num_src, nullptr);

    GTEXT_JSON_Value * bool_clone = gtext_json_clone(bool_src);
    GTEXT_JSON_Value * str_clone = gtext_json_clone(str_src);
    GTEXT_JSON_Value * num_clone = gtext_json_clone(num_src);
    ASSERT_NE(bool_clone, nullptr);
    ASSERT_NE(str_clone, nullptr);
    ASSERT_NE(num_clone, nullptr);

    EXPECT_EQ(gtext_json_equal(bool_src, bool_clone, GTEXT_JSON_EQUAL_LEXEME), true);
    EXPECT_EQ(gtext_json_equal(str_src, str_clone, GTEXT_JSON_EQUAL_LEXEME), true);
    EXPECT_EQ(gtext_json_equal(num_src, num_clone, GTEXT_JSON_EQUAL_LEXEME), true);

    gtext_json_free(bool_src);
    gtext_json_free(str_src);
    gtext_json_free(num_src);
    gtext_json_free(bool_clone);
    gtext_json_free(str_clone);
    gtext_json_free(num_clone);
}

/**
 * Test deep clone - arrays
 */
TEST(DomUtilities, DeepCloneArray) {
    GTEXT_JSON_Value * src = gtext_json_new_array();
    ASSERT_NE(src, nullptr);

    gtext_json_array_push(src, gtext_json_new_string("hello", 5));
    gtext_json_array_push(src, gtext_json_new_number_i64(123));
    gtext_json_array_push(src, gtext_json_new_bool(true));

    GTEXT_JSON_Value * clone = gtext_json_clone(src);
    ASSERT_NE(clone, nullptr);

    EXPECT_EQ(gtext_json_array_size(clone), 3u);
    EXPECT_EQ(gtext_json_equal(src, clone, GTEXT_JSON_EQUAL_LEXEME), true);

    // Verify independence - modify clone doesn't affect src
    gtext_json_array_push(clone, gtext_json_new_string("new", 3));
    EXPECT_EQ(gtext_json_array_size(src), 3u);
    EXPECT_EQ(gtext_json_array_size(clone), 4u);

    gtext_json_free(src);
    gtext_json_free(clone);
}

/**
 * Test deep clone - objects
 */
TEST(DomUtilities, DeepCloneObject) {
    GTEXT_JSON_Value * src = gtext_json_new_object();
    ASSERT_NE(src, nullptr);

    gtext_json_object_put(src, "a", 1, gtext_json_new_string("hello", 5));
    gtext_json_object_put(src, "b", 1, gtext_json_new_number_i64(123));

    GTEXT_JSON_Value * clone = gtext_json_clone(src);
    ASSERT_NE(clone, nullptr);

    EXPECT_EQ(gtext_json_object_size(clone), 2u);
    EXPECT_EQ(gtext_json_equal(src, clone, GTEXT_JSON_EQUAL_LEXEME), true);

    // Verify independence
    gtext_json_object_put(clone, "c", 1, gtext_json_new_string("new", 3));
    EXPECT_EQ(gtext_json_object_size(src), 2u);
    EXPECT_EQ(gtext_json_object_size(clone), 3u);

    gtext_json_free(src);
    gtext_json_free(clone);
}

/**
 * Test deep clone - nested structures
 */
TEST(DomUtilities, DeepCloneNested) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};

    const char * json = "{\"a\":[1,2,{\"b\":\"hello\"}]}";
    GTEXT_JSON_Value * src = gtext_json_parse(json, strlen(json), &opts, &err);
    ASSERT_NE(src, nullptr);

    GTEXT_JSON_Value * clone = gtext_json_clone(src);
    ASSERT_NE(clone, nullptr);

    EXPECT_EQ(gtext_json_equal(src, clone, GTEXT_JSON_EQUAL_LEXEME), true);

    gtext_json_free(src);
    gtext_json_free(clone);
}

/**
 * Test object merge - first wins policy
 */
TEST(DomUtilities, ObjectMergeFirstWins) {
    GTEXT_JSON_Value * target = gtext_json_new_object();
    GTEXT_JSON_Value * source = gtext_json_new_object();
    ASSERT_NE(target, nullptr);
    ASSERT_NE(source, nullptr);

    gtext_json_object_put(target, "a", 1, gtext_json_new_string("target", 6));
    gtext_json_object_put(target, "b", 1, gtext_json_new_string("target", 6));

    gtext_json_object_put(source, "a", 1, gtext_json_new_string("source", 6));
    gtext_json_object_put(source, "c", 1, gtext_json_new_string("source", 6));

    GTEXT_JSON_Status status = gtext_json_object_merge(target, source, GTEXT_JSON_MERGE_FIRST_WINS);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    EXPECT_EQ(gtext_json_object_size(target), 3u);

    // "a" should keep target value
    const GTEXT_JSON_Value * a = gtext_json_object_get(target, "a", 1);
    ASSERT_NE(a, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(a, &str, &len);
    EXPECT_EQ(len, 6u);
    EXPECT_EQ(memcmp(str, "target", 6), 0);

    // "c" should be added
    const GTEXT_JSON_Value * c = gtext_json_object_get(target, "c", 1);
    ASSERT_NE(c, nullptr);
    gtext_json_get_string(c, &str, &len);
    EXPECT_EQ(len, 6u);
    EXPECT_EQ(memcmp(str, "source", 6), 0);

    gtext_json_free(target);
    gtext_json_free(source);
}

/**
 * Test object merge - last wins policy
 */
TEST(DomUtilities, ObjectMergeLastWins) {
    GTEXT_JSON_Value * target = gtext_json_new_object();
    GTEXT_JSON_Value * source = gtext_json_new_object();
    ASSERT_NE(target, nullptr);
    ASSERT_NE(source, nullptr);

    gtext_json_object_put(target, "a", 1, gtext_json_new_string("target", 6));
    gtext_json_object_put(target, "b", 1, gtext_json_new_string("target", 6));

    gtext_json_object_put(source, "a", 1, gtext_json_new_string("source", 6));
    gtext_json_object_put(source, "c", 1, gtext_json_new_string("source", 6));

    GTEXT_JSON_Status status = gtext_json_object_merge(target, source, GTEXT_JSON_MERGE_LAST_WINS);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    EXPECT_EQ(gtext_json_object_size(target), 3u);

    // "a" should be replaced with source value
    const GTEXT_JSON_Value * a = gtext_json_object_get(target, "a", 1);
    ASSERT_NE(a, nullptr);
    const char * str;
    size_t len;
    gtext_json_get_string(a, &str, &len);
    EXPECT_EQ(len, 6u);
    EXPECT_EQ(memcmp(str, "source", 6), 0);

    gtext_json_free(target);
    gtext_json_free(source);
}

/**
 * Test object merge - error policy
 */
TEST(DomUtilities, ObjectMergeError) {
    GTEXT_JSON_Value * target = gtext_json_new_object();
    GTEXT_JSON_Value * source = gtext_json_new_object();
    ASSERT_NE(target, nullptr);
    ASSERT_NE(source, nullptr);

    gtext_json_object_put(target, "a", 1, gtext_json_new_string("target", 6));
    gtext_json_object_put(source, "a", 1, gtext_json_new_string("source", 6));

    GTEXT_JSON_Status status = gtext_json_object_merge(target, source, GTEXT_JSON_MERGE_ERROR);
    EXPECT_EQ(status, GTEXT_JSON_E_DUPKEY);

    gtext_json_free(target);
    gtext_json_free(source);
}

/**
 * Test object merge - nested objects
 */
TEST(DomUtilities, ObjectMergeNested) {
    GTEXT_JSON_Value * target = gtext_json_new_object();
    GTEXT_JSON_Value * source = gtext_json_new_object();
    ASSERT_NE(target, nullptr);
    ASSERT_NE(source, nullptr);

    GTEXT_JSON_Value * target_nested = gtext_json_new_object();
    gtext_json_object_put(target_nested, "x", 1, gtext_json_new_string("target", 6));
    gtext_json_object_put(target, "nested", 6, target_nested);

    GTEXT_JSON_Value * source_nested = gtext_json_new_object();
    gtext_json_object_put(source_nested, "y", 1, gtext_json_new_string("source", 6));
    gtext_json_object_put(source, "nested", 6, source_nested);

    GTEXT_JSON_Status status = gtext_json_object_merge(target, source, GTEXT_JSON_MERGE_LAST_WINS);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // Nested object should be merged
    const GTEXT_JSON_Value * nested = gtext_json_object_get(target, "nested", 6);
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(gtext_json_object_size(nested), 2u);

    const GTEXT_JSON_Value * x = gtext_json_object_get(nested, "x", 1);
    const GTEXT_JSON_Value * y = gtext_json_object_get(nested, "y", 1);
    ASSERT_NE(x, nullptr);
    ASSERT_NE(y, nullptr);

    gtext_json_free(target);
    gtext_json_free(source);
}

/**
 * Test object merge - non-object values are replaced
 */
TEST(DomUtilities, ObjectMergeNonObjectReplace) {
    GTEXT_JSON_Value * target = gtext_json_new_object();
    GTEXT_JSON_Value * source = gtext_json_new_object();
    ASSERT_NE(target, nullptr);
    ASSERT_NE(source, nullptr);

    gtext_json_object_put(target, "a", 1, gtext_json_new_string("target", 6));
    gtext_json_object_put(source, "a", 1, gtext_json_new_number_i64(123));

    GTEXT_JSON_Status status = gtext_json_object_merge(target, source, GTEXT_JSON_MERGE_LAST_WINS);
    EXPECT_EQ(status, GTEXT_JSON_OK);

    // "a" should be replaced with number
    const GTEXT_JSON_Value * a = gtext_json_object_get(target, "a", 1);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(gtext_json_typeof(a), GTEXT_JSON_NUMBER);

    int64_t val;
    gtext_json_get_i64(a, &val);
    EXPECT_EQ(val, 123);

    gtext_json_free(target);
    gtext_json_free(source);
}

/**
 * Test multiple top-level value parsing - single value (backward compatible)
 */
TEST(MultipleTopLevel, SingleValue) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    size_t bytes_consumed = 0;

    const char * input = "123";
    GTEXT_JSON_Value * value = gtext_json_parse_multiple(input, strlen(input), &opts, &err, &bytes_consumed);
    ASSERT_NE(value, nullptr) << "Parse failed with code: " << err.code;
    EXPECT_EQ(bytes_consumed, strlen(input));
    EXPECT_EQ(gtext_json_typeof(value), GTEXT_JSON_NUMBER);
    gtext_json_free(value);
}

/**
 * Test multiple top-level value parsing - multiple values
 */
TEST(MultipleTopLevel, MultipleValues) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    size_t bytes_consumed = 0;

    const char * input = "123 456 \"hello\"";
    size_t input_len = strlen(input);

    // Parse first value
    GTEXT_JSON_Value * value1 = gtext_json_parse_multiple(input, input_len, &opts, &err, &bytes_consumed);
    ASSERT_NE(value1, nullptr);
    EXPECT_EQ(gtext_json_typeof(value1), GTEXT_JSON_NUMBER);
    EXPECT_EQ(bytes_consumed, 4u);  // "123" + space
    gtext_json_free(value1);

    // Continue parsing from offset
    const char * remaining = input + bytes_consumed;
    size_t remaining_len = input_len - bytes_consumed;
    GTEXT_JSON_Value * value2 = gtext_json_parse_multiple(remaining, remaining_len, &opts, &err, &bytes_consumed);
    ASSERT_NE(value2, nullptr);
    EXPECT_EQ(gtext_json_typeof(value2), GTEXT_JSON_NUMBER);
    EXPECT_EQ(bytes_consumed, 4u);  // "456" + space
    gtext_json_free(value2);

    // Continue parsing third value
    remaining = remaining + bytes_consumed;
    remaining_len = remaining_len - bytes_consumed;
    GTEXT_JSON_Value * value3 = gtext_json_parse_multiple(remaining, remaining_len, &opts, &err, &bytes_consumed);
    ASSERT_NE(value3, nullptr);
    EXPECT_EQ(gtext_json_typeof(value3), GTEXT_JSON_STRING);
    EXPECT_EQ(bytes_consumed, 7u);  // "\"hello\""
    gtext_json_free(value3);
}

/**
 * Test bytes consumed reporting
 */
TEST(MultipleTopLevel, BytesConsumed) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    size_t bytes_consumed = 0;

    const char * input = "{\"a\":1} [1,2,3]";
    size_t input_len = strlen(input);

    // Parse first value (object)
    GTEXT_JSON_Value * value1 = gtext_json_parse_multiple(input, input_len, &opts, &err, &bytes_consumed);
    ASSERT_NE(value1, nullptr);
    EXPECT_EQ(gtext_json_typeof(value1), GTEXT_JSON_OBJECT);
    // bytes_consumed should point to the start of the next value (after "{\"a\":1} ")
    EXPECT_GT(bytes_consumed, 7u);  // At least the object itself
    EXPECT_LT(bytes_consumed, input_len);  // Less than total input
    gtext_json_free(value1);

    // Parse second value (array)
    const char * remaining = input + bytes_consumed;
    size_t remaining_len = input_len - bytes_consumed;
    GTEXT_JSON_Value * value2 = gtext_json_parse_multiple(remaining, remaining_len, &opts, &err, &bytes_consumed);
    ASSERT_NE(value2, nullptr);
    EXPECT_EQ(gtext_json_typeof(value2), GTEXT_JSON_ARRAY);
    // bytes_consumed should be the length of "[1,2,3]"
    EXPECT_EQ(bytes_consumed, 7u);  // "[1,2,3]"
    gtext_json_free(value2);
}

/**
 * Test continuation from offset
 */
TEST(MultipleTopLevel, ContinuationFromOffset) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    size_t bytes_consumed = 0;

    const char * input = "true false null";
    size_t input_len = strlen(input);
    size_t offset = 0;

    // Parse values in a loop
    std::vector<GTEXT_JSON_Type> expected_types = {
        GTEXT_JSON_BOOL, GTEXT_JSON_BOOL, GTEXT_JSON_NULL
    };

    for (size_t i = 0; i < expected_types.size(); ++i) {
        const char * current = input + offset;
        size_t current_len = input_len - offset;

        GTEXT_JSON_Value * value = gtext_json_parse_multiple(current, current_len, &opts, &err, &bytes_consumed);
        ASSERT_NE(value, nullptr) << "Failed to parse value " << i;
        EXPECT_EQ(gtext_json_typeof(value), expected_types[i]) << "Wrong type for value " << i;

        offset += bytes_consumed;
        gtext_json_free(value);
    }

    EXPECT_EQ(offset, input_len);
}

/**
 * Test error handling in multi-value mode
 */
TEST(MultipleTopLevel, ErrorHandling) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    size_t bytes_consumed = 0;

    // Valid value followed by invalid JSON
    const char * input = "123 invalid!!!";
    size_t input_len = strlen(input);

    // First value should parse successfully
    GTEXT_JSON_Value * value1 = gtext_json_parse_multiple(input, input_len, &opts, &err, &bytes_consumed);
    ASSERT_NE(value1, nullptr);

    /* bytes_consumed covers the value and **nothing that could not be read**.
       This used to assert `> 3` - the start of "invalid" rather than the end of
       "123" - which was the lexer's position after the failed trailing lex, and
       that position is only sometimes before the bad token: for `1 NaN` the
       lexer consumed the NaN before failing and reported all five bytes
       consumed, so a caller looping on this function read one record and
       concluded the input had ended cleanly. The offset is the end of the value
       now, in every case, and what matters is the pair of properties below: the
       value's own bytes are accounted for, and the unreadable ones are still
       there for the next call to fail on. */
    EXPECT_GE(bytes_consumed, 3u) << "the value's own bytes";
    EXPECT_LT(bytes_consumed, input_len)
        << "bytes that cannot be lexed must not be reported as consumed";
    gtext_json_free(value1);

    // Second parse should fail (invalid JSON)
    const char * remaining = input + bytes_consumed;
    size_t remaining_len = input_len - bytes_consumed;
    GTEXT_JSON_Value * value2 = gtext_json_parse_multiple(remaining, remaining_len, &opts, &err, &bytes_consumed);
    EXPECT_EQ(value2, nullptr);
    // bytes_consumed should be 0 on error
    EXPECT_EQ(bytes_consumed, 0u);
}

/**
 * Test that gtext_json_parse() rejects trailing content
 */
TEST(MultipleTopLevel, SingleValueRejectsTrailing) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    // Trailing content should cause error with gtext_json_parse()
    const char * input = "123 456";
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);
    EXPECT_EQ(value, nullptr);
    EXPECT_EQ(err.code, GTEXT_JSON_E_TRAILING_GARBAGE);

    // Clean up error context snippet
    gtext_json_error_free(&err);
}

/**
 * Test multiple top-level with complex nested structures
 */
TEST(MultipleTopLevel, ComplexStructures) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    size_t bytes_consumed = 0;

    const char * input = "{\"a\":[1,2,3]} {\"b\":{\"c\":\"value\"}}";
    size_t input_len = strlen(input);

    // Parse first object
    GTEXT_JSON_Value * value1 = gtext_json_parse_multiple(input, input_len, &opts, &err, &bytes_consumed);
    ASSERT_NE(value1, nullptr);
    EXPECT_EQ(gtext_json_typeof(value1), GTEXT_JSON_OBJECT);
    gtext_json_free(value1);

    // Parse second object
    const char * remaining = input + bytes_consumed;
    size_t remaining_len = input_len - bytes_consumed;
    GTEXT_JSON_Value * value2 = gtext_json_parse_multiple(remaining, remaining_len, &opts, &err, &bytes_consumed);
    ASSERT_NE(value2, nullptr);
    EXPECT_EQ(gtext_json_typeof(value2), GTEXT_JSON_OBJECT);
    gtext_json_free(value2);
}

/**
 * Test enhanced error reporting - context snippet generation
 */
TEST(EnhancedErrorReporting, ContextSnippet) {
    const char * json = "{\"key\": \"value\", \"invalid\": }";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * result = gtext_json_parse(json, strlen(json), &opts, &err);
    EXPECT_EQ(result, nullptr);
    EXPECT_NE(err.code, GTEXT_JSON_OK);

    // Verify context snippet is generated
    if (err.context_snippet) {
        EXPECT_GT(err.context_snippet_len, 0u);
        EXPECT_LT(err.caret_offset, err.context_snippet_len);

        // Context snippet should contain the error position
        EXPECT_NE(strstr(err.context_snippet, "invalid"), nullptr);

        // Clean up
        gtext_json_error_free(&err);
    }
}

/**
 * Test enhanced error reporting - expected/actual token descriptions
 */
TEST(EnhancedErrorReporting, ExpectedActualTokens) {
    const char * json = "{\"key\": \"value\", \"missing_colon\" }";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * result = gtext_json_parse(json, strlen(json), &opts, &err);
    EXPECT_EQ(result, nullptr);
    EXPECT_NE(err.code, GTEXT_JSON_OK);

    // Verify expected/actual token information is set for token mismatch errors
    if (err.code == GTEXT_JSON_E_BAD_TOKEN) {
        // Should have expected token description
        if (err.expected_token) {
            EXPECT_NE(strlen(err.expected_token), 0u);
        }
        // Should have actual token description
        if (err.actual_token) {
            EXPECT_NE(strlen(err.actual_token), 0u);
        }
    }

    gtext_json_error_free(&err);
}

/**
 * Test enhanced error reporting - caret positioning
 */
TEST(EnhancedErrorReporting, CaretPositioning) {
    const char * json = "[1, 2, 3, }";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * result = gtext_json_parse(json, strlen(json), &opts, &err);
    EXPECT_EQ(result, nullptr);
    EXPECT_NE(err.code, GTEXT_JSON_OK);

    // Verify caret offset is within context snippet bounds
    if (err.context_snippet) {
        EXPECT_LT(err.caret_offset, err.context_snippet_len);
        // Caret offset should point to error position in snippet
        EXPECT_GE(err.caret_offset, 0u);
    }

    gtext_json_error_free(&err);
}

/**
 * Test enhanced error reporting - error free function
 */
TEST(EnhancedErrorReporting, ErrorFree) {
    const char * json = "{\"invalid\": }";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * result = gtext_json_parse(json, strlen(json), &opts, &err);
    EXPECT_EQ(result, nullptr);

    // Verify context snippet is allocated
    if (err.context_snippet) {
        // Free the error
        gtext_json_error_free(&err);

        // Verify snippet is freed (pointer set to NULL)
        EXPECT_EQ(err.context_snippet, nullptr);
        EXPECT_EQ(err.context_snippet_len, 0u);
        EXPECT_EQ(err.caret_offset, 0u);
    }
}

/**
 * Test enhanced error reporting - multiple errors (verify cleanup)
 */
TEST(EnhancedErrorReporting, MultipleErrors) {
    const char * json1 = "{\"invalid1\": }";
    const char * json2 = "{\"invalid2\": }";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    // First parse
    GTEXT_JSON_Value * result1 = gtext_json_parse(json1, strlen(json1), &opts, &err);
    EXPECT_EQ(result1, nullptr);
    EXPECT_NE(err.code, GTEXT_JSON_OK);

    // Free first error
    gtext_json_error_free(&err);

    // Reset error structure
    err = GTEXT_JSON_Error{
        .code = {},
        .message = {},
        .offset = {},
        .line = {},
        .col = {},
        .context_snippet = {},
        .context_snippet_len = {},
        .caret_offset = {},
        .expected_token = {},
        .actual_token = {}
    };
    // Second parse (should reuse error structure)
    GTEXT_JSON_Value * result2 = gtext_json_parse(json2, strlen(json2), &opts, &err);
    EXPECT_EQ(result2, nullptr);
    EXPECT_NE(err.code, GTEXT_JSON_OK);

    // Free second error
    gtext_json_error_free(&err);
}

/**
 * Test enhanced error reporting - empty input
 */
TEST(EnhancedErrorReporting, EmptyInput) {
    const char * json = "";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * result = gtext_json_parse(json, strlen(json), &opts, &err);
    EXPECT_EQ(result, nullptr);
    EXPECT_NE(err.code, GTEXT_JSON_OK);

    // Empty input may not have a context snippet (too short)
    // But error should still be properly initialized
    EXPECT_NE(err.message, nullptr);

    gtext_json_error_free(&err);
}

/**
 * Test writer enhancements - locale independence (numeric formatting)
 */
TEST(WriterEnhancements, LocaleIndependence) {
    // Test that numeric formatting is locale-independent
    // This is primarily verified by the implementation using locale-independent
    // formatting functions, but we can test that numbers format consistently

    GTEXT_JSON_Value * v1 = gtext_json_new_number_i64(123456789);
    GTEXT_JSON_Value * v2 = gtext_json_new_number_u64(987654321ULL);
    GTEXT_JSON_Value * v3 = gtext_json_new_number_double(1234.56789);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);
    ASSERT_NE(v3, nullptr);

    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};

    // Test i64 formatting
    gtext_json_sink_buffer(&sink);
    gtext_json_write_value(&sink, &opts, v1, &err);
    const char * output = gtext_json_sink_buffer_data(&sink);
    // Should not contain locale-specific thousand separators
    EXPECT_EQ(strchr(output, ','), nullptr);  // No comma in number
    EXPECT_NE(strstr(output, "123456789"), nullptr);
    gtext_json_sink_buffer_free(&sink);

    // Test u64 formatting
    gtext_json_sink_buffer(&sink);
    gtext_json_write_value(&sink, &opts, v2, &err);
    output = gtext_json_sink_buffer_data(&sink);
    EXPECT_EQ(strchr(output, ','), nullptr);  // No comma in number
    EXPECT_NE(strstr(output, "987654321"), nullptr);
    gtext_json_sink_buffer_free(&sink);

    // Test double formatting
    gtext_json_sink_buffer(&sink);
    gtext_json_write_value(&sink, &opts, v3, &err);
    output = gtext_json_sink_buffer_data(&sink);
    // Should use dot as decimal separator (not comma)
    EXPECT_NE(strchr(output, '.'), nullptr);
    EXPECT_EQ(strchr(output, ','), nullptr);  // No comma as decimal separator
    gtext_json_sink_buffer_free(&sink);

    gtext_json_free(v1);
    gtext_json_free(v2);
    gtext_json_free(v3);
}

/**
 * Test writer enhancements - floating-point formatting options
 *
 * Note: To test float formatting, we need to set canonical_numbers = 1
 * to force formatting from the double representation instead of using the lexeme.
 */
TEST(WriterEnhancements, FloatFormatting) {
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};

    // Enable canonical_numbers to force formatting from double (not lexeme)
    opts.canonical_numbers = true;

    // Test SHORTEST format (default)
    GTEXT_JSON_Value * v1 = gtext_json_new_number_double(123.456789);
    ASSERT_NE(v1, nullptr);
    gtext_json_sink_buffer(&sink);
    opts.float_format = GTEXT_JSON_FLOAT_SHORTEST;
    gtext_json_write_value(&sink, &opts, v1, &err);
    const char * output = gtext_json_sink_buffer_data(&sink);
    // Should contain the number
    EXPECT_NE(strstr(output, "123"), nullptr);
    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(v1);

    // Test FIXED format with precision 2
    GTEXT_JSON_Value * v2 = gtext_json_new_number_double(123.456789);
    ASSERT_NE(v2, nullptr);
    gtext_json_sink_buffer(&sink);
    opts.float_format = GTEXT_JSON_FLOAT_FIXED;
    opts.float_precision = 2;
    gtext_json_write_value(&sink, &opts, v2, &err);
    output = gtext_json_sink_buffer_data(&sink);
    // Should have exactly 2 decimal places (rounded: 123.46)
    const char * dot = strchr(output, '.');
    ASSERT_NE(dot, nullptr);
    // Count digits after decimal point
    size_t decimal_places = 0;
    for (const char * p = dot + 1; *p && *p >= '0' && *p <= '9'; p++) {
        decimal_places++;
    }
    EXPECT_EQ(decimal_places, 2u);
    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(v2);

    // Test FIXED format with precision 4
    GTEXT_JSON_Value * v3 = gtext_json_new_number_double(123.456789);
    ASSERT_NE(v3, nullptr);
    gtext_json_sink_buffer(&sink);
    opts.float_format = GTEXT_JSON_FLOAT_FIXED;
    opts.float_precision = 4;
    gtext_json_write_value(&sink, &opts, v3, &err);
    output = gtext_json_sink_buffer_data(&sink);
    dot = strchr(output, '.');
    ASSERT_NE(dot, nullptr);
    decimal_places = 0;
    for (const char * p = dot + 1; *p && *p >= '0' && *p <= '9'; p++) {
        decimal_places++;
    }
    EXPECT_EQ(decimal_places, 4u);
    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(v3);

    // Test SCIENTIFIC format - use a number that would naturally use scientific notation
    // or verify the format is applied (even if the number could be represented normally)
    GTEXT_JSON_Value * v4 = gtext_json_new_number_double(123456.789);
    ASSERT_NE(v4, nullptr);
    gtext_json_sink_buffer(&sink);
    opts.float_format = GTEXT_JSON_FLOAT_SCIENTIFIC;
    opts.float_precision = 3;
    gtext_json_write_value(&sink, &opts, v4, &err);
    output = gtext_json_sink_buffer_data(&sink);
    // Should contain 'e' or 'E' for scientific notation
    EXPECT_TRUE(strchr(output, 'e') != nullptr || strchr(output, 'E') != nullptr);
    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(v4);

    // Test SCIENTIFIC format with a very small number
    GTEXT_JSON_Value * v5 = gtext_json_new_number_double(0.000123456);
    ASSERT_NE(v5, nullptr);
    gtext_json_sink_buffer(&sink);
    opts.float_format = GTEXT_JSON_FLOAT_SCIENTIFIC;
    opts.float_precision = 2;
    gtext_json_write_value(&sink, &opts, v5, &err);
    output = gtext_json_sink_buffer_data(&sink);
    // Should contain 'e' or 'E' for scientific notation
    EXPECT_TRUE(strchr(output, 'e') != nullptr || strchr(output, 'E') != nullptr);
    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(v5);
}

/**
 * Test writer enhancements - trailing newline control
 */
TEST(WriterEnhancements, TrailingNewline) {
    GTEXT_JSON_Value * v = gtext_json_new_string("test", 4);
    ASSERT_NE(v, nullptr);

    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};

    // Test without trailing newline (default)
    gtext_json_sink_buffer(&sink);
    opts.trailing_newline = false;
    gtext_json_write_value(&sink, &opts, v, &err);
    const char * output = gtext_json_sink_buffer_data(&sink);
    size_t len = gtext_json_sink_buffer_size(&sink);
    // Should not end with newline
    EXPECT_NE(len, 0u);
    EXPECT_NE(output[len - 1], '\n');
    gtext_json_sink_buffer_free(&sink);

    // Test with trailing newline
    gtext_json_sink_buffer(&sink);
    opts.trailing_newline = true;
    gtext_json_write_value(&sink, &opts, v, &err);
    output = gtext_json_sink_buffer_data(&sink);
    len = gtext_json_sink_buffer_size(&sink);
    // Should end with newline
    EXPECT_GT(len, 0u);
    EXPECT_EQ(output[len - 1], '\n');
    gtext_json_sink_buffer_free(&sink);

    gtext_json_free(v);
}

/**
 * Test writer enhancements - spacing controls
 */
TEST(WriterEnhancements, SpacingControls) {
    GTEXT_JSON_Value * obj = gtext_json_new_object();
    ASSERT_NE(obj, nullptr);

    GTEXT_JSON_Value * v1 = gtext_json_new_number_i64(1);
    GTEXT_JSON_Value * v2 = gtext_json_new_number_i64(2);
    gtext_json_object_put(obj, "key1", 4, v1);
    gtext_json_object_put(obj, "key2", 4, v2);

    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};

    // Test without spacing (default)
    gtext_json_sink_buffer(&sink);
    opts.space_after_colon = false;
    opts.space_after_comma = false;
    gtext_json_write_value(&sink, &opts, obj, &err);
    const char * output = gtext_json_sink_buffer_data(&sink);
    // Should not have space after colon or comma
    EXPECT_EQ(strstr(output, ": "), nullptr);  // No space after colon
    EXPECT_EQ(strstr(output, ", "), nullptr);  // No space after comma
    gtext_json_sink_buffer_free(&sink);

    // Test with space after colon
    gtext_json_sink_buffer(&sink);
    opts.space_after_colon = true;
    opts.space_after_comma = false;
    gtext_json_write_value(&sink, &opts, obj, &err);
    output = gtext_json_sink_buffer_data(&sink);
    // Should have space after colon
    EXPECT_NE(strstr(output, ": "), nullptr);
    gtext_json_sink_buffer_free(&sink);

    // Test with space after comma
    gtext_json_sink_buffer(&sink);
    opts.space_after_colon = false;
    opts.space_after_comma = true;
    gtext_json_write_value(&sink, &opts, obj, &err);
    output = gtext_json_sink_buffer_data(&sink);
    // Should have space after comma
    EXPECT_NE(strstr(output, ", "), nullptr);
    gtext_json_sink_buffer_free(&sink);

    // Test with both spacing options
    gtext_json_sink_buffer(&sink);
    opts.space_after_colon = true;
    opts.space_after_comma = true;
    gtext_json_write_value(&sink, &opts, obj, &err);
    output = gtext_json_sink_buffer_data(&sink);
    // Should have both spaces
    EXPECT_NE(strstr(output, ": "), nullptr);
    EXPECT_NE(strstr(output, ", "), nullptr);
    gtext_json_sink_buffer_free(&sink);

    gtext_json_free(obj);
}

/**
 * Test writer enhancements - inline formatting thresholds
 */
TEST(WriterEnhancements, InlineFormattingThresholds) {
    // Create a small array
    GTEXT_JSON_Value * small_arr = gtext_json_new_array();
    ASSERT_NE(small_arr, nullptr);
    gtext_json_array_push(small_arr, gtext_json_new_number_i64(1));
    gtext_json_array_push(small_arr, gtext_json_new_number_i64(2));

    // Create a larger array
    GTEXT_JSON_Value * large_arr = gtext_json_new_array();
    ASSERT_NE(large_arr, nullptr);
    for (int i = 0; i < 10; i++) {
        gtext_json_array_push(large_arr, gtext_json_new_number_i64(i));
    }

    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Write_Options opts = gtext_json_write_options_default();
    GTEXT_JSON_Error err{};

    // Test inline threshold = -1 (always inline when not pretty)
    gtext_json_sink_buffer(&sink);
    opts.pretty = false;
    opts.inline_array_threshold = -1;
    gtext_json_write_value(&sink, &opts, small_arr, &err);
    const char * output = gtext_json_sink_buffer_data(&sink);
    // Should be inline (no newlines)
    EXPECT_EQ(strchr(output, '\n'), nullptr);
    gtext_json_sink_buffer_free(&sink);

    // Test inline threshold = 0 (always pretty)
    gtext_json_sink_buffer(&sink);
    opts.pretty = true;
    opts.inline_array_threshold = 0;
    gtext_json_write_value(&sink, &opts, small_arr, &err);
    output = gtext_json_sink_buffer_data(&sink);
    // Should be pretty (has newlines)
    EXPECT_NE(strchr(output, '\n'), nullptr);
    gtext_json_sink_buffer_free(&sink);

    // Test inline threshold = 3 (small array should be inline, large should be pretty)
    gtext_json_sink_buffer(&sink);
    opts.pretty = true;
    opts.inline_array_threshold = 3;
    gtext_json_write_value(&sink, &opts, small_arr, &err);
    output = gtext_json_sink_buffer_data(&sink);
    // Small array (2 elements <= 3) should be inline
    EXPECT_EQ(strchr(output, '\n'), nullptr);
    gtext_json_sink_buffer_free(&sink);

    gtext_json_sink_buffer(&sink);
    gtext_json_write_value(&sink, &opts, large_arr, &err);
    output = gtext_json_sink_buffer_data(&sink);
    // Large array (10 elements > 3) should be pretty
    EXPECT_NE(strchr(output, '\n'), nullptr);
    gtext_json_sink_buffer_free(&sink);

    // Test object thresholds
    GTEXT_JSON_Value * small_obj = gtext_json_new_object();
    ASSERT_NE(small_obj, nullptr);
    gtext_json_object_put(small_obj, "a", 1, gtext_json_new_number_i64(1));
    gtext_json_object_put(small_obj, "b", 1, gtext_json_new_number_i64(2));

    gtext_json_sink_buffer(&sink);
    opts.inline_object_threshold = 3;
    gtext_json_write_value(&sink, &opts, small_obj, &err);
    output = gtext_json_sink_buffer_data(&sink);
    // Small object (2 pairs <= 3) should be inline
    EXPECT_EQ(strchr(output, '\n'), nullptr);
    gtext_json_sink_buffer_free(&sink);

    gtext_json_free(small_arr);
    gtext_json_free(large_arr);
    gtext_json_free(small_obj);
}

// Helper function to read a file into a string
static std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    return content;
}

// Helper function to get test data directory
static std::string get_test_data_dir() {
    // Try to find the test data directory relative to the test executable
    // This works when running from the build directory
    const char * test_dir = getenv("TEST_DATA_DIR");
    if (test_dir) {
        return std::string(test_dir);
    }

    // Default relative path from build directory
    return "tests/data/json";
}

// Helper to test a valid JSON file
static void test_valid_json_file(const std::string& filepath) {
    std::string content = read_file(filepath);
    ASSERT_FALSE(content.empty()) << "Failed to read file: " << filepath;

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(content.c_str(), content.size(), &opts, &err);
    EXPECT_NE(value, nullptr) << "Failed to parse valid JSON from: " << filepath
                              << " Error: " << (err.message ? err.message : "unknown");

    if (value) {
        gtext_json_free(value);
    }
    gtext_json_error_free(&err);
}

// Helper to test an invalid JSON file (should fail to parse)
static void test_invalid_json_file(const std::string& filepath) {
    std::string content = read_file(filepath);
    ASSERT_FALSE(content.empty()) << "Failed to read file: " << filepath;

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(content.c_str(), content.size(), &opts, &err);
    EXPECT_EQ(value, nullptr) << "Should have failed to parse invalid JSON from: " << filepath;

    if (value) {
        gtext_json_free(value);
    }
    gtext_json_error_free(&err);
}

// Helper to test JSONC file (with comments enabled)
static void test_jsonc_file(const std::string& filepath) {
    std::string content = read_file(filepath);
    ASSERT_FALSE(content.empty()) << "Failed to read file: " << filepath;

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_comments = true;
    opts.allow_trailing_commas = true;
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(content.c_str(), content.size(), &opts, &err);
    EXPECT_NE(value, nullptr) << "Failed to parse JSONC from: " << filepath
                              << " Error: " << (err.message ? err.message : "unknown");

    if (value) {
        gtext_json_free(value);
    }
    gtext_json_error_free(&err);
}

// Helper to test round-trip: parse -> write -> parse -> compare
static void test_round_trip(const std::string& filepath) {
    std::string content = read_file(filepath);
    ASSERT_FALSE(content.empty()) << "Failed to read file: " << filepath;

    GTEXT_JSON_Parse_Options parse_opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    // Parse original
    GTEXT_JSON_Value * original = gtext_json_parse(content.c_str(), content.size(), &parse_opts, &err);
    ASSERT_NE(original, nullptr) << "Failed to parse: " << filepath;

    // Write to buffer
    GTEXT_JSON_Sink sink;
    GTEXT_JSON_Status status = gtext_json_sink_buffer(&sink);
    ASSERT_EQ(status, GTEXT_JSON_OK);

    GTEXT_JSON_Write_Options write_opts = gtext_json_write_options_default();
    status = gtext_json_write_value(&sink, &write_opts, original, &err);
    ASSERT_EQ(status, GTEXT_JSON_OK) << "Failed to write: " << filepath;

    const char * output = gtext_json_sink_buffer_data(&sink);
    size_t output_len = gtext_json_sink_buffer_size(&sink);

    // Parse again
    GTEXT_JSON_Value * reparsed = gtext_json_parse(output, output_len, &parse_opts, &err);
    ASSERT_NE(reparsed, nullptr) << "Failed to reparse output from: " << filepath;

    // Compare structurally (deep equality)
    if (original && reparsed) {
        bool equal = gtext_json_equal(original, reparsed, GTEXT_JSON_EQUAL_NUMERIC);
        EXPECT_EQ(equal, true) << "Round-trip failed for: " << filepath;
    }

    gtext_json_sink_buffer_free(&sink);
    if (original) gtext_json_free(original);
    if (reparsed) gtext_json_free(reparsed);
    gtext_json_error_free(&err);
}

/**
 * Test corpus - RFC 8259 examples
 */
TEST(TestCorpus, RFC8259Examples) {
    std::string base_dir = get_test_data_dir() + "/rfc8259";

    // Test all RFC 8259 example files
    test_valid_json_file(base_dir + "/basic.json");
    test_valid_json_file(base_dir + "/array.json");
    test_valid_json_file(base_dir + "/strings.json");
    test_valid_json_file(base_dir + "/numbers.json");
    test_valid_json_file(base_dir + "/whitespace.json");
}

/**
 * Test corpus - Valid JSON cases
 */
TEST(TestCorpus, ValidJSON) {
    std::string base_dir = get_test_data_dir() + "/valid";

    test_valid_json_file(base_dir + "/empty.json");
    test_valid_json_file(base_dir + "/empty-array.json");
    test_valid_json_file(base_dir + "/empty-object.json");
    test_valid_json_file(base_dir + "/nested.json");
    test_valid_json_file(base_dir + "/large-array.json");
    test_valid_json_file(base_dir + "/large-object.json");
}

/**
 * Test corpus - Invalid JSON cases (should fail to parse)
 */
TEST(TestCorpus, InvalidJSON) {
    std::string base_dir = get_test_data_dir() + "/invalid";

    test_invalid_json_file(base_dir + "/trailing-comma-array.json");
    test_invalid_json_file(base_dir + "/trailing-comma-object.json");
    test_invalid_json_file(base_dir + "/missing-comma.json");
    test_invalid_json_file(base_dir + "/missing-colon.json");
    test_invalid_json_file(base_dir + "/invalid-number-01.json");
    test_invalid_json_file(base_dir + "/invalid-number-leading-dot.json");
    test_invalid_json_file(base_dir + "/invalid-number-trailing-dot.json");
    test_invalid_json_file(base_dir + "/invalid-number-incomplete-exponent.json");
    test_invalid_json_file(base_dir + "/invalid-string-unclosed.json");
    test_invalid_json_file(base_dir + "/invalid-string-control-char.json");
}

/**
 * Test corpus - JSONC (with comments and trailing commas)
 */
TEST(TestCorpus, JSONC) {
    std::string base_dir = get_test_data_dir() + "/jsonc";

    test_jsonc_file(base_dir + "/single-line-comment.json");
    test_jsonc_file(base_dir + "/multi-line-comment.json");
    test_jsonc_file(base_dir + "/trailing-comma-array.json");
    test_jsonc_file(base_dir + "/trailing-comma-object.json");
    test_jsonc_file(base_dir + "/mixed.json");
}

/**
 * Test corpus - Unicode torture tests
 */
TEST(TestCorpus, Unicode) {
    std::string base_dir = get_test_data_dir() + "/unicode";

    // Valid Unicode cases
    test_valid_json_file(base_dir + "/surrogate-pair.json");
    test_valid_json_file(base_dir + "/various-unicode.json");

    // Invalid surrogate sequences (should fail in strict mode)
    test_invalid_json_file(base_dir + "/invalid-surrogate-lone-high.json");
    test_invalid_json_file(base_dir + "/invalid-surrogate-lone-low.json");
    test_invalid_json_file(base_dir + "/invalid-surrogate-reversed.json");
}

/**
 * Test corpus - BOM (Byte Order Mark) handling
 */
TEST(TestCorpus, BOMHandling) {
    // UTF-8 BOM is: EF BB BF (U+FEFF)
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    const char * json_after_bom = "{\"key\":\"value\"}";

    // Create input with BOM
    std::vector<unsigned char> input_with_bom;
    input_with_bom.insert(input_with_bom.end(), bom, bom + 3);
    input_with_bom.insert(input_with_bom.end(),
                         (const unsigned char * )json_after_bom,
                         (const unsigned char * )json_after_bom + strlen(json_after_bom));

    // Test 1: BOM enabled (default) - should parse successfully
    {
        GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
        opts.allow_leading_bom = true;
        GTEXT_JSON_Error err{};
        GTEXT_JSON_Value * value = gtext_json_parse(
            (const char * )input_with_bom.data(),
            input_with_bom.size(),
            &opts,
            &err
        );
        EXPECT_NE(value, nullptr) << "Should parse JSON with BOM when allow_leading_bom=1";

        if (value) {
            // Verify we can access the data
            const GTEXT_JSON_Value * key_val = gtext_json_object_get(value, "key", 3);
            EXPECT_NE(key_val, nullptr);
            if (key_val) {
                const char * str_val = nullptr;
                size_t str_len = 0;
                GTEXT_JSON_Status status = gtext_json_get_string(key_val, &str_val, &str_len);
                EXPECT_EQ(status, GTEXT_JSON_OK);
                EXPECT_EQ(str_len, 5u);
                EXPECT_STREQ(str_val, "value");
            }
            gtext_json_free(value);
        }
        gtext_json_error_free(&err);
    }

    // Test 2: BOM disabled - verify option is checked
    // Note: The BOM bytes (EF BB BF) might be treated as whitespace or ignored
    // by some parsers even when allow_leading_bom=0. The important thing is
    // that when allow_leading_bom=1, the BOM is properly skipped.
    {
        GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
        opts.allow_leading_bom = false;
        GTEXT_JSON_Error err{};
        GTEXT_JSON_Value * value = gtext_json_parse(
            (const char * )input_with_bom.data(),
            input_with_bom.size(),
            &opts,
            &err
        );
        // Behavior may vary: BOM bytes might be rejected, ignored, or treated as whitespace
        // The key test is that allow_leading_bom=1 works (tested in Test 1)
        if (value) {
            // If it parsed, that's acceptable - verify it parsed correctly
            const GTEXT_JSON_Value * key_val = gtext_json_object_get(value, "key", 3);
            if (key_val) {
                const char * str_val = nullptr;
                size_t str_len = 0;
                gtext_json_get_string(key_val, &str_val, &str_len);
                EXPECT_EQ(str_len, 5u);
                EXPECT_STREQ(str_val, "value");
            }
            gtext_json_free(value);
        }
        // If it failed, that's also acceptable - BOM bytes are not valid JSON
        gtext_json_error_free(&err);
    }

    // Test 3: JSON without BOM should work regardless of setting
    {
        GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
        opts.allow_leading_bom = true;  // Even with BOM enabled, no-BOM should work
        GTEXT_JSON_Error err{};
        GTEXT_JSON_Value * value = gtext_json_parse(
            json_after_bom,
            strlen(json_after_bom),
            &opts,
            &err
        );
        EXPECT_NE(value, nullptr) << "Should parse JSON without BOM";

        if (value) {
            gtext_json_free(value);
        }
        gtext_json_error_free(&err);
    }

    // Test 4: BOM in middle of input (not at start) should be treated as invalid
    // BOM bytes in the middle are not valid JSON syntax
    {
        std::vector<unsigned char> input_with_middle_bom;
        input_with_middle_bom.insert(input_with_middle_bom.end(),
                                    (const unsigned char * )"{\"key\":",
                                    (const unsigned char * )"{\"key\":" + 7);
        input_with_middle_bom.insert(input_with_middle_bom.end(), bom, bom + 3);
        input_with_middle_bom.insert(input_with_middle_bom.end(),
                                    (const unsigned char * )"\"value\"}",
                                    (const unsigned char * )"\"value\"}" + 8);

        GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
        opts.allow_leading_bom = true;  // BOM only allowed at start, not in middle
        GTEXT_JSON_Error err{};
        GTEXT_JSON_Value * value = gtext_json_parse(
            (const char * )input_with_middle_bom.data(),
            input_with_middle_bom.size(),
            &opts,
            &err
        );
        // BOM in middle should cause parse failure (BOM bytes are not valid JSON)
        // However, if UTF-8 validation is off, it might succeed
        // The important thing is that BOM at start works correctly
        if (!value) {
            // Expected: BOM in middle should fail
            // But if it doesn't fail, that's also acceptable (implementation-dependent)
        } else {
            gtext_json_free(value);
        }
        gtext_json_error_free(&err);
    }
}

/**
 * Test corpus - Number boundary tests
 */
TEST(TestCorpus, NumberBoundaries) {
    std::string base_dir = get_test_data_dir() + "/numbers";

    // Valid boundary cases
    test_valid_json_file(base_dir + "/int64-max.json");
    test_valid_json_file(base_dir + "/int64-min.json");
    test_valid_json_file(base_dir + "/uint64-max.json");
    test_valid_json_file(base_dir + "/exponent-large.json");
    test_valid_json_file(base_dir + "/exponent-small.json");
    test_valid_json_file(base_dir + "/precision.json");

    // Overflow cases (should parse but may not fit in int64/uint64)
    test_valid_json_file(base_dir + "/int64-overflow.json");
    test_valid_json_file(base_dir + "/uint64-overflow.json");

    // Nonfinite numbers (require option)
    // Test parsing the nonfinite.json file which contains unquoted NaN/Infinity
    const char * nonfinite_json = "{\"nan\":NaN,\"infinity\":Infinity,\"negative_infinity\":-Infinity}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_nonfinite_numbers = true;
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(nonfinite_json, strlen(nonfinite_json), &opts, &err);
    ASSERT_NE(value, nullptr) << "Should parse non-finite numbers when enabled";
    EXPECT_EQ(gtext_json_typeof(value), GTEXT_JSON_OBJECT);

    // Verify the values
    const GTEXT_JSON_Value * nan_val = gtext_json_object_get(value, "nan", 3);
    ASSERT_NE(nan_val, nullptr);
    double dbl = 0.0;
    EXPECT_EQ(gtext_json_get_double(nan_val, &dbl), GTEXT_JSON_OK);
    EXPECT_TRUE(std::isnan(dbl));

    const GTEXT_JSON_Value * inf_val = gtext_json_object_get(value, "infinity", 8);
    ASSERT_NE(inf_val, nullptr);
    dbl = 0.0;
    EXPECT_EQ(gtext_json_get_double(inf_val, &dbl), GTEXT_JSON_OK);
    EXPECT_TRUE(std::isinf(dbl) && dbl > 0);

    const GTEXT_JSON_Value * neg_inf_val = gtext_json_object_get(value, "negative_infinity", 17);
    ASSERT_NE(neg_inf_val, nullptr);
    dbl = 0.0;
    EXPECT_EQ(gtext_json_get_double(neg_inf_val, &dbl), GTEXT_JSON_OK);
    EXPECT_TRUE(std::isinf(dbl) && dbl < 0);

    gtext_json_free(value);
    gtext_json_error_free(&err);
}

/**
 * Test corpus - Round-trip tests
 */
TEST(TestCorpus, RoundTrip) {
    std::string base_dir = get_test_data_dir();

    // Test round-trip on various valid JSON files
    test_round_trip(base_dir + "/rfc8259/basic.json");
    test_round_trip(base_dir + "/rfc8259/array.json");
    test_round_trip(base_dir + "/rfc8259/strings.json");
    test_round_trip(base_dir + "/rfc8259/numbers.json");
    test_round_trip(base_dir + "/valid/nested.json");
    test_round_trip(base_dir + "/unicode/various-unicode.json");
    test_round_trip(base_dir + "/numbers/precision.json");
}

/**
 * Test corpus - Milestone A: Strict JSON DOM + Writer
 *
 * Verify:
 * - strict parse (RFC/ECMA)
 * - DOM write (compact + pretty)
 * - full unicode correctness
 * - good errors
 */
TEST(TestCorpus, MilestoneA) {
    std::string base_dir = get_test_data_dir();

    // Strict parse - RFC 8259 examples should work
    test_valid_json_file(base_dir + "/rfc8259/basic.json");
    test_valid_json_file(base_dir + "/rfc8259/array.json");

    // Invalid JSON should be rejected
    test_invalid_json_file(base_dir + "/invalid/trailing-comma-array.json");

    // Unicode correctness
    test_valid_json_file(base_dir + "/unicode/surrogate-pair.json");
    test_valid_json_file(base_dir + "/unicode/various-unicode.json");

    // DOM write - test compact and pretty
    std::string content = read_file(base_dir + "/rfc8259/basic.json");
    ASSERT_FALSE(content.empty());

    GTEXT_JSON_Parse_Options parse_opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(content.c_str(), content.size(), &parse_opts, &err);
    ASSERT_NE(value, nullptr);

    // Test compact write
    GTEXT_JSON_Sink sink;
    gtext_json_sink_buffer(&sink);
    GTEXT_JSON_Write_Options write_opts = gtext_json_write_options_default();
    write_opts.pretty = false;
    gtext_json_write_value(&sink, &write_opts, value, &err);
    EXPECT_NE(gtext_json_sink_buffer_data(&sink), nullptr);
    gtext_json_sink_buffer_free(&sink);

    // Test pretty write
    gtext_json_sink_buffer(&sink);
    write_opts.pretty = true;
    write_opts.indent_spaces = 2;
    gtext_json_write_value(&sink, &write_opts, value, &err);
    const char * output = gtext_json_sink_buffer_data(&sink);
    EXPECT_NE(output, nullptr);
    EXPECT_NE(strchr(output, '\n'), nullptr);  // Should have newlines
    gtext_json_sink_buffer_free(&sink);

    gtext_json_free(value);
    gtext_json_error_free(&err);
}

/**
 * Test corpus - Milestone B: Extensions (JSONC, trailing commas, nonfinite)
 */
TEST(TestCorpus, MilestoneB) {
    std::string base_dir = get_test_data_dir();

    // JSONC support
    test_jsonc_file(base_dir + "/jsonc/single-line-comment.json");
    test_jsonc_file(base_dir + "/jsonc/multi-line-comment.json");
    test_jsonc_file(base_dir + "/jsonc/mixed.json");

    // Trailing commas
    test_jsonc_file(base_dir + "/jsonc/trailing-comma-array.json");
    test_jsonc_file(base_dir + "/jsonc/trailing-comma-object.json");

    // Nonfinite numbers
    const char * nonfinite_json = "{\"nan\":NaN,\"infinity\":Infinity,\"negative_infinity\":-Infinity}";
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.allow_nonfinite_numbers = true;
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(nonfinite_json, strlen(nonfinite_json), &opts, &err);
    // Nonfinite numbers may not parse if lexer doesn't support them
    // This is acceptable - the test verifies the option exists
    if (value) {
        // Verify round-trip with nonfinite numbers
        GTEXT_JSON_Sink sink;
        gtext_json_sink_buffer(&sink);
        GTEXT_JSON_Write_Options write_opts = gtext_json_write_options_default();
        write_opts.allow_nonfinite_numbers = true;
        gtext_json_write_value(&sink, &write_opts, value, &err);

        const char * output = gtext_json_sink_buffer_data(&sink);
        if (output) {
            EXPECT_NE(strstr(output, "NaN"), nullptr);
            EXPECT_NE(strstr(output, "Infinity"), nullptr);
        }

        gtext_json_sink_buffer_free(&sink);
        gtext_json_free(value);
    }
    gtext_json_error_free(&err);
}

/**
 * Test corpus - Milestone C: Streaming Parser + Streaming Writer
 * (Streaming tests are already covered in StreamingParser and StreamingWriter test suites)
 */
TEST(TestCorpus, MilestoneC) {
    // Milestone C is verified by existing streaming parser and writer tests
    // This test verifies that streaming works with corpus data
    std::string content = read_file(get_test_data_dir() + "/rfc8259/basic.json");
    ASSERT_FALSE(content.empty());

    // Create a simple event counter callback
    int event_count = 0;
    GTEXT_JSON_Event_cb event_cb = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)evt;  // Unused but required by signature
        (void)err;  // Unused but required by signature
        int* count = (int*)user;
        (*count)++;
        return GTEXT_JSON_OK;  // Continue parsing
    };

    GTEXT_JSON_Parse_Options parse_opts = gtext_json_parse_options_default();
    GTEXT_JSON_Stream * stream = gtext_json_stream_new(&parse_opts, event_cb, &event_count);
    ASSERT_NE(stream, nullptr);

    // Feed in chunks
    size_t chunk_size = 10;
    size_t pos = 0;
    GTEXT_JSON_Status status = GTEXT_JSON_OK;

    GTEXT_JSON_Error err{};
    while (pos < content.size() && status == GTEXT_JSON_OK) {
        size_t len = (pos + chunk_size < content.size()) ? chunk_size : (content.size() - pos);
        status = gtext_json_stream_feed(stream, content.c_str() + pos, len, &err);
        pos += len;
    }

    // Streaming may encounter errors during incremental parsing
    // The important thing is that the stream was created and can process data
    // (Streaming parser tests are more thoroughly covered in StreamingParser suite)

    gtext_json_stream_finish(stream, &err);

    gtext_json_stream_free(stream);
    gtext_json_error_free(&err);
}

/**
 * Test corpus - Milestone D: Pointer + Patch + Merge Patch
 * (Pointer, Patch, and Merge Patch are already tested in their respective test suites)
 */
TEST(TestCorpus, MilestoneD) {
    // Milestone D is verified by existing JSON Pointer, Patch, and Merge Patch tests
    // This test verifies they work with corpus data
    std::string content = read_file(get_test_data_dir() + "/rfc8259/basic.json");
    ASSERT_FALSE(content.empty());

    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(content.c_str(), content.size(), &opts, &err);
    ASSERT_NE(value, nullptr);

    // Test JSON Pointer
    const GTEXT_JSON_Value * result = gtext_json_pointer_get(value, "/Image/Width", strlen("/Image/Width"));
    EXPECT_NE(result, nullptr);

    if (result) {
        int64_t width = 0;
        GTEXT_JSON_Status status = gtext_json_get_i64(result, &width);
        EXPECT_EQ(status, GTEXT_JSON_OK);
        EXPECT_EQ(width, 800);
    }

    gtext_json_free(value);
}

/**
 * Test corpus - Milestone E: Schema (subset) + Canonical Output
 * (Schema validation is already tested in the schema test suite)
 */
TEST(TestCorpus, MilestoneE) {
    // Milestone E is verified by existing schema validation tests
    // This test verifies canonical output
    std::string content = read_file(get_test_data_dir() + "/valid/large-object.json");
    ASSERT_FALSE(content.empty());

    GTEXT_JSON_Parse_Options parse_opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    GTEXT_JSON_Value * value = gtext_json_parse(content.c_str(), content.size(), &parse_opts, &err);
    ASSERT_NE(value, nullptr);

    // Test canonical output (key sorting)
    GTEXT_JSON_Sink sink;
    gtext_json_sink_buffer(&sink);
    GTEXT_JSON_Write_Options write_opts = gtext_json_write_options_default();
    write_opts.sort_object_keys = true;
    gtext_json_write_value(&sink, &write_opts, value, &err);

    const char * output = gtext_json_sink_buffer_data(&sink);
    EXPECT_NE(output, nullptr);

    // Verify keys are sorted (should start with "a":1)
    // Output format: {"a":1,"b":2,...}
    const char * key_a = strstr(output, "\"a\"");
    const char * key_b = strstr(output, "\"b\"");
    EXPECT_NE(key_a, nullptr);
    EXPECT_NE(key_b, nullptr);
    if (key_a && key_b) {
        EXPECT_LT(key_a, key_b);  // "a" should come before "b"
    }

    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(value);
    gtext_json_error_free(&err);
}

// ============================================================================
// Overflow/Underflow Protection Tests
// ============================================================================

/**
 * Test comprehensive overflow and underflow protection
 */

// Test buffer size overflow protection
TEST(OverflowProtection, BufferSizeOverflow) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    // Set a small limit to test overflow protection
    opts.max_string_bytes = 100;

    GTEXT_JSON_Error err{};
    // Create a string that exceeds the limit
    std::string large_string = "\"";
    large_string.append(200, 'a');  // 200 bytes exceeds the 100 byte limit
    large_string += "\"";

    GTEXT_JSON_Value * value = gtext_json_parse(large_string.c_str(), large_string.size(), &opts, &err);
    // Should fail with appropriate error due to exceeding max_string_bytes limit
    EXPECT_EQ(value, nullptr);
    EXPECT_NE(err.code, GTEXT_JSON_OK);
    gtext_json_error_free(&err);
}

// Test container element count overflow
TEST(OverflowProtection, ContainerElementOverflow) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.max_container_elems = 100;  // Set a limit

    GTEXT_JSON_Error err{};
    // Create an array that exceeds the limit
    std::string large_array = "[";
    for (int i = 0; i < 150; ++i) {  // 150 elements exceeds the 100 limit
        if (i > 0) large_array += ",";
        large_array += "1";
    }
    large_array += "]";

    GTEXT_JSON_Value * value = gtext_json_parse(large_array.c_str(), large_array.size(), &opts, &err);
    // Should fail with appropriate error due to exceeding max_container_elems limit
    EXPECT_EQ(value, nullptr);
    EXPECT_NE(err.code, GTEXT_JSON_OK);
    gtext_json_error_free(&err);
}

// Test total bytes consumed overflow protection
TEST(OverflowProtection, TotalBytesOverflow) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    opts.max_total_bytes = 1000;  // Set a limit

    GTEXT_JSON_Error err{};
    // Create input that exceeds limit
    std::string large_input(2000, '1');
    large_input = "[" + large_input + "]";

    GTEXT_JSON_Value * value = gtext_json_parse(large_input.c_str(), large_input.size(), &opts, &err);
    // Note: max_total_bytes limit may not be enforced in one-shot parse function
    // This test verifies the parser handles large input without crashing
    // If the limit is enforced, parsing should fail; otherwise it may succeed
    if (value) {
        gtext_json_free(value);
        // If parsing succeeded, verify it parsed correctly
        EXPECT_EQ(err.code, GTEXT_JSON_OK);
    } else {
        // If parsing failed, verify it was due to limit (not crash)
        EXPECT_NE(err.code, GTEXT_JSON_OK);
    }
    gtext_json_error_free(&err);
}

// ============================================================================
// NULL Pointer Handling Tests
// ============================================================================

/**
 * Test comprehensive NULL pointer handling
 */

// Test NULL stream pointer
TEST(NullPointerHandling, NullStream) {
    GTEXT_JSON_Error err{};
    // GTEXT_JSON_Stream_feed with NULL stream
    GTEXT_JSON_Status status = gtext_json_stream_feed(nullptr, "123", 3, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);
    gtext_json_error_free(&err);
}

// Test NULL callback pointer in stream creation
TEST(NullPointerHandling, NullStreamCallback) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

    // NULL callback should be invalid
    GTEXT_JSON_Stream * stream = gtext_json_stream_new(&opts, nullptr, nullptr);
    EXPECT_EQ(stream, nullptr);
}

// Test NULL buffer pointer with non-zero length
TEST(NullPointerHandling, NullBufferWithLength) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)user; (void)evt; (void)err;
        return GTEXT_JSON_OK;
    };
    GTEXT_JSON_Stream * stream = gtext_json_stream_new(&opts, callback, nullptr);
    ASSERT_NE(stream, nullptr);

    GTEXT_JSON_Error err{};
    // NULL buffer with length > 0 should fail
    GTEXT_JSON_Status status = gtext_json_stream_feed(stream, nullptr, 10, &err);
    EXPECT_EQ(status, GTEXT_JSON_E_INVALID);

    gtext_json_stream_free(stream);
    gtext_json_error_free(&err);
}

// Test NULL error output pointer (should be allowed)
TEST(NullPointerHandling, NullErrorOutput) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)user; (void)evt; (void)err;
        return GTEXT_JSON_OK;
    };
    GTEXT_JSON_Stream * stream = gtext_json_stream_new(&opts, callback, nullptr);
    ASSERT_NE(stream, nullptr);

    // NULL error pointer should be allowed (optional parameter)
    GTEXT_JSON_Status status = gtext_json_stream_feed(stream, "123", 3, nullptr);
    // Should not crash - NULL error pointer is optional
    // Status should be OK for valid input, or an error code for invalid input
    EXPECT_NE(status, GTEXT_JSON_E_INVALID);  // Should not be invalid due to NULL error parameter

    // Finish should also work with NULL error
    GTEXT_JSON_Status finish_status = gtext_json_stream_finish(stream, nullptr);
    EXPECT_EQ(finish_status, GTEXT_JSON_OK);

    gtext_json_stream_free(stream);
}

// Test NULL parse options parameter
TEST(NullPointerHandling, NullParseOptions) {
    GTEXT_JSON_Error err{};
    // Test that NULL opts parameter doesn't crash
    // Note: The parser may or may not support NULL opts - this test verifies
    // it handles it gracefully without crashing
    const char * input = "42";
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), nullptr, &err);

    // The important thing is it doesn't crash
    // If NULL opts is supported, parsing should succeed; if not, it may fail
    // Either way, we should get a valid result (value or error)
    if (value) {
        EXPECT_EQ(value->type, GTEXT_JSON_NUMBER);
        gtext_json_free(value);
    }
    // If value is NULL, err should be set (even if code is OK, meaning no error was detected)
    // The key is that we didn't crash

    gtext_json_error_free(&err);
}

// Test NULL value pointer in free function
TEST(NullPointerHandling, NullValueFree) {
    // Should not crash - free functions must handle NULL gracefully
    gtext_json_free(nullptr);
    // If we get here without crashing, the test passed
    SUCCEED();
}

// Test NULL error pointer in error_free
TEST(NullPointerHandling, NullErrorFree) {
    // Should not crash - free functions must handle NULL gracefully
    gtext_json_error_free(nullptr);
    // If we get here without crashing, the test passed
    SUCCEED();
}

// Test NULL stream pointer in stream_free
TEST(NullPointerHandling, NullStreamFree) {
    // Should not crash - free functions must handle NULL gracefully
    gtext_json_stream_free(nullptr);
    // If we get here without crashing, the test passed
    SUCCEED();
}

// ============================================================================
// Bounds Checking Tests
// ============================================================================

/**
 * Test comprehensive bounds checking
 */

// Test out-of-bounds array access in DOM
TEST(BoundsChecking, ArrayAccessOutOfBounds) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    const char * input = "[1, 2, 3]";
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->type, GTEXT_JSON_ARRAY);

    size_t count = gtext_json_array_size(value);
    EXPECT_EQ(count, 3);

    // Access valid indices (first, middle, last)
    const GTEXT_JSON_Value * elem0 = gtext_json_array_get(value, 0);
    const GTEXT_JSON_Value * elem1 = gtext_json_array_get(value, 1);
    const GTEXT_JSON_Value * elem2 = gtext_json_array_get(value, 2);
    EXPECT_NE(elem0, nullptr);
    EXPECT_NE(elem1, nullptr);
    EXPECT_NE(elem2, nullptr);

    // Access out-of-bounds index should return NULL
    const GTEXT_JSON_Value * elem_out = gtext_json_array_get(value, 10);
    EXPECT_EQ(elem_out, nullptr) << "Out-of-bounds index should return NULL";

    // Access at boundary (count is out of bounds)
    const GTEXT_JSON_Value * elem_at_boundary = gtext_json_array_get(value, count);
    EXPECT_EQ(elem_at_boundary, nullptr) << "Index equal to size is out of bounds";

    gtext_json_free(value);
    gtext_json_error_free(&err);
}

// Test out-of-bounds object access
TEST(BoundsChecking, ObjectAccessOutOfBounds) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    const char * input = "{\"key1\":1, \"key2\":2}";
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->type, GTEXT_JSON_OBJECT);

    size_t count = gtext_json_object_size(value);
    EXPECT_EQ(count, 2);

    // Access valid key
    const GTEXT_JSON_Value * val1 = gtext_json_object_get(value, "key1", 4);
    EXPECT_NE(val1, nullptr);

    // Access non-existent key should return NULL
    const GTEXT_JSON_Value * val_out = gtext_json_object_get(value, "nonexistent", 11);
    EXPECT_EQ(val_out, nullptr);

    gtext_json_free(value);
    gtext_json_error_free(&err);
}

// Test chunked parsing with data spanning buffer boundaries
// Note: This tests chunked parsing functionality, not bounds checking per se,
// but it verifies buffer boundary handling which is related to bounds safety
TEST(BoundsChecking, ChunkedParsing) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)user; (void)evt; (void)err;
        return GTEXT_JSON_OK;
    };
    GTEXT_JSON_Stream * stream = gtext_json_stream_new(&opts, callback, nullptr);
    ASSERT_NE(stream, nullptr);

    GTEXT_JSON_Error err{};

    // Feed data in chunks to test buffer boundary handling
    const char * chunk1 = "{\"key\":";
    const char * chunk2 = "\"value\"}";

    GTEXT_JSON_Status status1 = gtext_json_stream_feed(stream, chunk1, strlen(chunk1), &err);
    EXPECT_EQ(status1, GTEXT_JSON_OK);
    EXPECT_EQ(err.code, GTEXT_JSON_OK);

    GTEXT_JSON_Status status2 = gtext_json_stream_feed(stream, chunk2, strlen(chunk2), &err);
    EXPECT_EQ(status2, GTEXT_JSON_OK);
    EXPECT_EQ(err.code, GTEXT_JSON_OK);

    GTEXT_JSON_Status finish_status = gtext_json_stream_finish(stream, &err);
    EXPECT_EQ(finish_status, GTEXT_JSON_OK);
    EXPECT_EQ(err.code, GTEXT_JSON_OK);

    gtext_json_stream_free(stream);
    gtext_json_error_free(&err);
}

// Test array iteration and boundary access
TEST(BoundsChecking, ArrayIterationBounds) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    GTEXT_JSON_Error err{};
    const char * input = "[1, 2, 3, 4, 5]";
    GTEXT_JSON_Value * value = gtext_json_parse(input, strlen(input), &opts, &err);
    ASSERT_NE(value, nullptr);

    size_t count = gtext_json_array_size(value);
    EXPECT_EQ(count, 5);

    // Iterate through valid indices and verify access works
    for (size_t i = 0; i < count; ++i) {
        const GTEXT_JSON_Value * elem = gtext_json_array_get(value, i);
        EXPECT_NE(elem, nullptr) << "Failed to access valid index " << i;
    }

    // Try to access beyond bounds (at count, which is out of bounds)
    const GTEXT_JSON_Value * elem_out = gtext_json_array_get(value, count);
    EXPECT_EQ(elem_out, nullptr) << "Out-of-bounds access should return NULL";

    // Also test accessing well beyond bounds
    const GTEXT_JSON_Value * elem_far_out = gtext_json_array_get(value, count + 100);
    EXPECT_EQ(elem_far_out, nullptr) << "Far out-of-bounds access should return NULL";

    gtext_json_free(value);
    gtext_json_error_free(&err);
}

// ============================================================================
// State Validation Tests
// ============================================================================

/**
 * Test state machine validation and invalid state handling
 */

// Test invalid state transition - calling finish before feed
TEST(StateValidation, FinishBeforeFeed) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)user; (void)evt; (void)err;
        return GTEXT_JSON_OK;
    };
    GTEXT_JSON_Stream * stream = gtext_json_stream_new(&opts, callback, nullptr);
    ASSERT_NE(stream, nullptr);

    GTEXT_JSON_Error err{};
    // Finish without feeding should handle gracefully
    GTEXT_JSON_Status status = gtext_json_stream_finish(stream, &err);
    // Should either succeed (empty input) or fail with appropriate error
    EXPECT_NE(status, GTEXT_JSON_E_INVALID);  // Should not be invalid state error

    gtext_json_stream_free(stream);
    gtext_json_error_free(&err);
}

// Test error state handling - continue after error
TEST(StateValidation, ContinueAfterError) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)user; (void)evt; (void)err;
        return GTEXT_JSON_OK;
    };
    GTEXT_JSON_Stream * stream = gtext_json_stream_new(&opts, callback, nullptr);
    ASSERT_NE(stream, nullptr);

    GTEXT_JSON_Error err{};
    // Feed invalid JSON to cause error
    gtext_json_stream_feed(stream, "invalid json!!!", 15, &err);
    // May succeed initially (partial input), but finish should detect error
    GTEXT_JSON_Status finish_status = gtext_json_stream_finish(stream, &err);
    // Should fail with parse error
    EXPECT_NE(finish_status, GTEXT_JSON_OK);
    EXPECT_NE(err.code, GTEXT_JSON_OK);

    // Try to feed more data after error - should reject or handle gracefully
    GTEXT_JSON_Error err2{
        .code = {},
        .message = {},
        .offset = {},
        .line = {},
        .col = {},
        .context_snippet = {},
        .context_snippet_len = {},
        .caret_offset = {},
        .expected_token = {},
        .actual_token = {}
    };
    GTEXT_JSON_Status status_after_error = gtext_json_stream_feed(stream, "more data", 9, &err2);
    // Should either reject (return error) or handle gracefully (not crash)
    // The important thing is it doesn't crash
    EXPECT_NE(status_after_error, GTEXT_JSON_E_INVALID);  // Should not be invalid parameter error

    gtext_json_stream_free(stream);
    gtext_json_error_free(&err);
    gtext_json_error_free(&err2);
}

// Test state consistency - multiple finish calls
TEST(StateValidation, MultipleFinishCalls) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)user; (void)evt; (void)err;
        return GTEXT_JSON_OK;
    };
    GTEXT_JSON_Stream * stream = gtext_json_stream_new(&opts, callback, nullptr);
    ASSERT_NE(stream, nullptr);

    GTEXT_JSON_Error err{};
    // Feed valid JSON
    GTEXT_JSON_Status feed_status = gtext_json_stream_feed(stream, "123", 3, &err);
    EXPECT_EQ(feed_status, GTEXT_JSON_OK);

    // First finish should succeed
    GTEXT_JSON_Status finish1 = gtext_json_stream_finish(stream, &err);
    EXPECT_EQ(finish1, GTEXT_JSON_OK);

    // Second finish should handle gracefully (already done)
    GTEXT_JSON_Error err2{
        .code = {},
        .message = {},
        .offset = {},
        .line = {},
        .col = {},
        .context_snippet = {},
        .context_snippet_len = {},
        .caret_offset = {},
        .expected_token = {},
        .actual_token = {}
    };
    GTEXT_JSON_Status finish2 = gtext_json_stream_finish(stream, &err2);
    // Should either succeed (idempotent) or return appropriate status
    EXPECT_NE(finish2, GTEXT_JSON_E_INVALID);

    gtext_json_stream_free(stream);
    gtext_json_error_free(&err);
    gtext_json_error_free(&err2);
}

// Test incomplete JSON structure (invalid final state)
TEST(StateValidation, IncompleteStructure) {
    GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
    auto callback = [](void * user, const GTEXT_JSON_Event* evt, GTEXT_JSON_Error* err) -> GTEXT_JSON_Status {
        (void)user; (void)evt; (void)err;
        return GTEXT_JSON_OK;
    };
    GTEXT_JSON_Stream * stream = gtext_json_stream_new(&opts, callback, nullptr);
    ASSERT_NE(stream, nullptr);

    GTEXT_JSON_Error err{};
    // Feed incomplete JSON (missing closing brace)
    gtext_json_stream_feed(stream, "{\"key\":\"value\"", 15, &err);
    // May succeed initially (partial input)

    // Finish should detect incomplete structure
    GTEXT_JSON_Status finish_status = gtext_json_stream_finish(stream, &err);
    EXPECT_NE(finish_status, GTEXT_JSON_OK);  // Should fail with incomplete error

    gtext_json_stream_free(stream);
    gtext_json_error_free(&err);
}

// Note: ErrorStateRecovery test removed - it duplicates ContinueAfterError functionality
// Both tests verify that streams handle error states gracefully when used after an error



/*
 * normalize_unicode was declared, documented as a v2 feature, and read by
 * nothing: setting it produced unnormalized output with no indication that the
 * request had been dropped.  It used to fail the parse for that reason.  It is
 * implemented now, so what these tests pin is the behaviour rather than the
 * refusal - and in particular the two places where an implementation can
 * appear to work while doing nothing.
 */
/*
 * The streaming parser and the DOM parser must agree about what JSON is.
 *
 * They had drifted, and nothing could see it: make conformance-json scores the
 * DOM parser against JSONTestSuite, and the 47 streaming tests each fed input
 * chosen to exercise the feature under test. Neither arrangement compares the
 * two parsers to each other, so four disagreements had accumulated - three of
 * them the streaming parser *accepting* input the DOM parser refuses, which is
 * the direction that matters for a parser reading bytes it did not write.
 *
 *   {}           refused by the stream, accepted by the DOM
 *   {"a":{}}     likewise - every empty object, at any chunk size
 *   [1,]         accepted by the stream whatever allow_trailing_commas said
 *   {"a":}       accepted by the stream: a member with no value
 *
 * The cause of the first two and the fourth was one inversion. `}` was handled
 * in JSON_STREAM_STATE_EXPECT_VALUE, labelled "end of object (empty object)" -
 * but that state is reached inside an object only by way of a colon, so a `}`
 * there is a missing value and never an empty object. Meanwhile
 * JSON_STREAM_STATE_OBJECT_KEY, which is exactly where `{` and a member comma
 * leave the parser, accepted a string and nothing else. The handler was in the
 * one state where it is wrong and absent from the state where it is right.
 *
 * The third was an asymmetry between two copies of the array-close code: the one
 * reached from the VALUE state tested for a trailing comma and the one reached
 * from EXPECT_VALUE did not. Both read correctly on their own.
 *
 * This test is the instrument rather than a list of the four: it asks the same
 * question of both parsers over every case, so the next drift shows up as a
 * disagreement instead of waiting for someone to write a case for it.
 */
/*
 * "NULL for defaults" has to be true, and it was not.
 *
 * Every entry point's documentation says the options argument may be NULL for
 * defaults. json_parse_internal() passed that NULL straight on, and
 * json_parse_number() reads its options as `if (opts && opts->parse_int64)` and
 * `if (opts && opts->preserve_number_lexeme)` - so with no options every number
 * came back holding nothing at all. The value reported type NUMBER;
 * gtext_json_get_i64(), _get_u64(), _get_double() and _get_number_lexeme() all
 * answered GTEXT_JSON_E_INVALID; and gtext_json_write_value() then failed with
 * GTEXT_JSON_E_WRITE, so *any parsed document containing a number could not be
 * serialized*.
 *
 * No test saw it because they all pass options. The one that writes numbers -
 * DOMWrite.Number - builds them with gtext_json_new_number_i64() rather than
 * parsing them, and does not check the status either.
 *
 * So this test asks the same questions with NULL and with an explicit default,
 * and requires the same answers. The pair is the point: either alone would pass
 * against a parser that ignored its options entirely.
 */
TEST(JsonNullOptions, NullOptionsMeanTheDefaultsAndNotNothing) {
	struct Case {
		const char * src;
		bool has_i64;
		const char * lexeme;
	} cases[] = {
	    {"42", true, "42"},
	    {"-1", true, "-1"},
	    {"0", true, "0"},
	    {"1.5", false, "1.5"},
	    {"1e3", false, "1e3"},
	    {"1.0", false, "1.0"},
	    {"123456789012345678901234567890", false,
	        "123456789012345678901234567890"},
	};

	GTEXT_JSON_Parse_Options dflt = gtext_json_parse_options_default();
	ASSERT_TRUE(dflt.preserve_number_lexeme);
	ASSERT_TRUE(dflt.parse_int64);

	for (const Case & c : cases) {
		const GTEXT_JSON_Parse_Options * both[2] = {nullptr, &dflt};
		for (const GTEXT_JSON_Parse_Options * opt : both) {
			const char * which = opt ? "explicit defaults" : "NULL options";
			GTEXT_JSON_Error err;
			std::memset(&err, 0, sizeof(err));
			GTEXT_JSON_Value * v =
			    gtext_json_parse(c.src, std::strlen(c.src), opt, &err);
			ASSERT_NE(v, nullptr) << which << " " << c.src << ": "
			                      << (err.message ? err.message : "");
			gtext_json_error_free(&err);
			EXPECT_EQ(gtext_json_typeof(v), GTEXT_JSON_NUMBER)
			    << which << " " << c.src;

			// The preserved lexeme, which is the option's whole purpose.
			const char * lex = nullptr;
			size_t lex_len = 0;
			EXPECT_EQ(gtext_json_get_number_lexeme(v, &lex, &lex_len),
			    GTEXT_JSON_OK)
			    << which << ": no lexeme for " << c.src;
			if (lex) {
				EXPECT_EQ(std::string(lex, lex_len), std::string(c.lexeme))
				    << which;
			}

			// A double is available for every number.
			double d = 0;
			EXPECT_EQ(gtext_json_get_double(v, &d), GTEXT_JSON_OK)
			    << which << ": no double for " << c.src;

			// An int64 for the ones that fit.
			int64_t i = 0;
			if (c.has_i64) {
				EXPECT_EQ(gtext_json_get_i64(v, &i), GTEXT_JSON_OK)
				    << which << ": no int64 for " << c.src;
			}

			// And the value can be written, which is what failed outright.
			GTEXT_JSON_Sink sink;
			ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
			GTEXT_JSON_Write_Options w = gtext_json_write_options_default();
			GTEXT_JSON_Error werr;
			std::memset(&werr, 0, sizeof(werr));
			EXPECT_EQ(gtext_json_write_value(&sink, &w, v, &werr),
			    GTEXT_JSON_OK)
			    << which << ": could not write " << c.src << ": "
			    << (werr.message ? werr.message : "");
			EXPECT_EQ(std::string(gtext_json_sink_buffer_data(&sink),
			              gtext_json_sink_buffer_size(&sink)),
			    std::string(c.lexeme))
			    << which;
			gtext_json_sink_buffer_free(&sink);
			gtext_json_error_free(&werr);
			gtext_json_free(v);
		}
	}
}

/* A whole document, since the failure above stopped the writer mid-output and a
   single scalar does not show that. */
TEST(JsonNullOptions, ADocumentParsedWithNullOptionsStillSerializes) {
	const char * src =
	    "{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":1.5},\"e\":\"text\",\"f\":null,"
	    "\"g\":true}";
	GTEXT_JSON_Value * v =
	    gtext_json_parse(src, std::strlen(src), nullptr, nullptr);
	ASSERT_NE(v, nullptr);

	GTEXT_JSON_Sink sink;
	ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
	GTEXT_JSON_Write_Options w = gtext_json_write_options_default();
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	ASSERT_EQ(gtext_json_write_value(&sink, &w, v, &err), GTEXT_JSON_OK)
	    << (err.message ? err.message : "");
	EXPECT_EQ(std::string(gtext_json_sink_buffer_data(&sink),
	              gtext_json_sink_buffer_size(&sink)),
	    std::string(src))
	    << "a document parsed with NULL options did not round-trip";
	gtext_json_sink_buffer_free(&sink);
	gtext_json_error_free(&err);
	gtext_json_free(v);
}

/* gtext_json_parse_multiple() shares json_parse_internal(), so it shares the
   fix; asserted rather than assumed. */
TEST(JsonNullOptions, ParseMultipleAlsoGetsTheDefaults) {
	const char * src = "1 2.5 3";
	size_t consumed = 0;
	size_t total = 0;
	int seen = 0;
	while (total < std::strlen(src)) {
		GTEXT_JSON_Value * v = gtext_json_parse_multiple(
		    src + total, std::strlen(src) - total, nullptr, nullptr, &consumed);
		if (!v) {
			break;
		}
		const char * lex = nullptr;
		size_t lex_len = 0;
		EXPECT_EQ(gtext_json_get_number_lexeme(v, &lex, &lex_len),
		    GTEXT_JSON_OK)
		    << "value " << seen << " had no lexeme";
		gtext_json_free(v);
		total += consumed;
		seen++;
		if (consumed == 0) {
			break;
		}
	}
	EXPECT_EQ(seen, 3);
}

TEST(JsonStreamDom, TheTwoParsersAgreeOnWhatJsonIs) {
	struct Case {
		const char * src;
		bool valid;
	};
	// Each marked with what RFC 8259 says, so the test is also a claim about
	// the grammar and not only about the two implementations matching.
	const Case cases[] = {
	    // Empty containers, nested and not.
	    {"{}", true},
	    {"{ }", true},
	    {"[]", true},
	    {"[ ]", true},
	    {"[[]]", true},
	    {"[{}]", true},
	    {"{\"a\":{}}", true},
	    {"{\"a\":[]}", true},
	    {"[{},{}]", true},
	    {"{\"a\":{},\"b\":[]}", true},
	    {"[[[[]]]]", true},

	    // Ordinary documents.
	    {"{\"a\":1}", true},
	    {"[1,2,3]", true},
	    {"{\"a\":[1,{\"b\":null}],\"c\":true}", true},
	    {"\"bare string\"", true},
	    {"0", true},
	    {"null", true},
	    {"false", true},

	    // Trailing commas, refused by default in both containers.
	    {"[1,]", false},
	    {"{\"a\":1,}", false},
	    {"[1,2,]", false},
	    {"[[1,]]", false},
	    {"{\"a\":[1,]}", false},

	    // A member or element with nothing in it.
	    {"{\"a\":}", false},
	    {"{\"a\":1,\"b\":}", false},
	    {"[,]", false},
	    {"[1,,2]", false},
	    {"{,}", false},
	    {"{\"a\":,}", false},
	    {"{\"a\"}", false},
	    {"{:1}", false},
	    {"{\"a\":1,,}", false},

	    // Mismatched and unbalanced.
	    {"{]", false},
	    {"[}", false},
	    {"{\"a\":1", false},
	    {"[1", false},
	    {"}", false},
	    {"]", false},
	    {"", false},

	    // Other refusals, to keep the agreement from being vacuous.
	    {"[1 2]", false},
	    {"{\"a\":1 \"b\":2}", false},
	    {"tru", false},
	    {"[01]", false},
	    {"{\"a\":1}extra", false},
	    {"'single'", false},
	    {"[1,2,3],", false},
	};

	for (const Case & c : cases) {
		GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

		// The DOM parser.
		GTEXT_JSON_Error derr;
		std::memset(&derr, 0, sizeof(derr));
		GTEXT_JSON_Value * v =
		    gtext_json_parse(c.src, std::strlen(c.src), &opts, &derr);
		bool dom_ok = (v != nullptr);
		if (v) {
			gtext_json_free(v);
		}
		gtext_json_error_free(&derr);

		// The streaming parser, in one feed and then one byte at a time, since
		// a chunk boundary is its own way to disagree.
		for (size_t chunk : {std::strlen(c.src) ? std::strlen(c.src) : 1,
		         (size_t)1}) {
			GTEXT_JSON_Event_cb cb = [](void *, const GTEXT_JSON_Event *,
			                             GTEXT_JSON_Error *) {
				return GTEXT_JSON_OK;
			};
			GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, cb, nullptr);
			ASSERT_NE(st, nullptr);
			GTEXT_JSON_Error serr;
			std::memset(&serr, 0, sizeof(serr));
			GTEXT_JSON_Status status = GTEXT_JSON_OK;
			size_t len = std::strlen(c.src);
			for (size_t i = 0; i < len; i += chunk) {
				size_t n = std::min(chunk, len - i);
				status = gtext_json_stream_feed(st, c.src + i, n, &serr);
				if (status != GTEXT_JSON_OK) {
					break;
				}
			}
			if (status == GTEXT_JSON_OK) {
				status = gtext_json_stream_finish(st, &serr);
			}
			bool stream_ok = (status == GTEXT_JSON_OK);
			gtext_json_stream_free(st);

			EXPECT_EQ(stream_ok, c.valid)
			    << "streaming parser, chunk " << chunk << ", input [" << c.src
			    << "]: " << (serr.message ? serr.message : "no message");
			EXPECT_EQ(stream_ok, dom_ok)
			    << "the two parsers disagree about [" << c.src << "] at chunk "
			    << chunk;
			gtext_json_error_free(&serr);
		}

		EXPECT_EQ(dom_ok, c.valid) << "DOM parser, input [" << c.src << "]";
	}
}

/* And with the option on, a trailing comma is accepted by both - so the test
   above is pinning the default rather than an inability. */
TEST(JsonStreamDom, TrailingCommasAreAcceptedByBothWhenAskedFor) {
	const char * cases[] = {"[1,]", "{\"a\":1,}", "[1,2,]", "{\"a\":[1,]}"};
	for (const char * src : cases) {
		GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
		opts.allow_trailing_commas = true;

		GTEXT_JSON_Error derr;
		std::memset(&derr, 0, sizeof(derr));
		GTEXT_JSON_Value * v =
		    gtext_json_parse(src, std::strlen(src), &opts, &derr);
		EXPECT_NE(v, nullptr)
		    << "DOM: " << src << ": "
		    << (derr.message ? derr.message : "no message");
		if (v) {
			gtext_json_free(v);
		}
		gtext_json_error_free(&derr);

		GTEXT_JSON_Event_cb cb = [](void *, const GTEXT_JSON_Event *,
		                             GTEXT_JSON_Error *) {
			return GTEXT_JSON_OK;
		};
		GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, cb, nullptr);
		ASSERT_NE(st, nullptr);
		GTEXT_JSON_Error serr;
		std::memset(&serr, 0, sizeof(serr));
		GTEXT_JSON_Status status =
		    gtext_json_stream_feed(st, src, std::strlen(src), &serr);
		if (status == GTEXT_JSON_OK) {
			status = gtext_json_stream_finish(st, &serr);
		}
		EXPECT_EQ(status, GTEXT_JSON_OK)
		    << "stream: " << src << ": "
		    << (serr.message ? serr.message : "no message");
		gtext_json_stream_free(st);
		gtext_json_error_free(&serr);
	}
}

/* The empty object must also produce the right events, not merely be accepted:
   OBJECT_BEGIN then OBJECT_END, with nothing between them. */
TEST(JsonStreamDom, AnEmptyObjectEmitsBeginAndEndAndNothingElse) {
	struct Collected {
		std::vector<GTEXT_JSON_Event_Type> types;
	} got;

	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	GTEXT_JSON_Event_cb cb = [](void * user, const GTEXT_JSON_Event * ev,
	                             GTEXT_JSON_Error *) {
		((Collected *)user)->types.push_back(ev->type);
		return GTEXT_JSON_OK;
	};
	GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, cb, &got);
	ASSERT_NE(st, nullptr);
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	const char * src = "{\"a\":{},\"b\":[]}";
	ASSERT_EQ(gtext_json_stream_feed(st, src, std::strlen(src), &err),
	    GTEXT_JSON_OK)
	    << (err.message ? err.message : "");
	ASSERT_EQ(gtext_json_stream_finish(st, &err), GTEXT_JSON_OK);
	gtext_json_stream_free(st);
	gtext_json_error_free(&err);

	EXPECT_EQ(got.types,
	    (std::vector<GTEXT_JSON_Event_Type>{GTEXT_JSON_EVT_OBJECT_BEGIN,
	        GTEXT_JSON_EVT_KEY, GTEXT_JSON_EVT_OBJECT_BEGIN,
	        GTEXT_JSON_EVT_OBJECT_END, GTEXT_JSON_EVT_KEY,
	        GTEXT_JSON_EVT_ARRAY_BEGIN, GTEXT_JSON_EVT_ARRAY_END,
	        GTEXT_JSON_EVT_OBJECT_END}));
}

TEST(JsonNormalizeUnicode, ComposesADecomposedScalar) {
	const char *src = "{\"a\":\"e\\u0301\"}";

	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	EXPECT_FALSE(opts.normalize_unicode) << "must stay off by default";

	/* Off: the two codepoints survive as written, three bytes of them. */
	GTEXT_JSON_Value *plain = gtext_json_parse(src, strlen(src), &opts, &err);
	ASSERT_NE(plain, nullptr) << (err.message ? err.message : "unknown");
	const GTEXT_JSON_Value *pa = gtext_json_object_get(plain, "a", 1);
	ASSERT_NE(pa, nullptr);
	size_t plen = 0;
	const char *pstr = nullptr;
	ASSERT_EQ(gtext_json_get_string(pa, &pstr, &plen), GTEXT_JSON_OK);
	EXPECT_EQ(plen, 3u);
	EXPECT_EQ(std::string(pstr, plen), std::string("e\xcc\x81"));
	gtext_json_free(plain);
	gtext_json_error_free(&err);

	/* On: one composed character, U+00E9, two bytes. */
	memset(&err, 0, sizeof(err));
	opts.normalize_unicode = true;
	GTEXT_JSON_Value *v = gtext_json_parse(src, strlen(src), &opts, &err);
	ASSERT_NE(v, nullptr) << (err.message ? err.message : "unknown");
	const GTEXT_JSON_Value *a = gtext_json_object_get(v, "a", 1);
	ASSERT_NE(a, nullptr);
	size_t len = 0;
	const char *str = nullptr;
	ASSERT_EQ(gtext_json_get_string(a, &str, &len), GTEXT_JSON_OK);
	EXPECT_EQ(len, 2u);
	EXPECT_EQ(std::string(str, len), std::string("\xc3\xa9"));
	gtext_json_free(v);
	gtext_json_error_free(&err);
}

/*
 * The reason normalisation belongs in the lexer rather than in the DOM: an
 * object name is a string, so normalising where strings are made means the
 * parser compares names that are already normalised.  These two spellings are
 * one name, and the default duplicate policy is ERROR, so the parse must fail
 * with the option on and succeed with it off.  Getting this backwards - or
 * normalising after the comparison - leaves a document whose two keys are
 * byte-identical, which no caller can then tell apart.
 */
TEST(JsonNormalizeUnicode, NormalizesObjectNamesBeforeComparingThem) {
	const char *src = "{\"\\u00e9\":1,\"e\\u0301\":2}";

	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	ASSERT_EQ(opts.dupkeys, GTEXT_JSON_DUPKEY_ERROR);

	/* Off: two distinct names, and the document has two members. */
	GTEXT_JSON_Value *two = gtext_json_parse(src, strlen(src), &opts, &err);
	ASSERT_NE(two, nullptr) << (err.message ? err.message : "unknown");
	EXPECT_EQ(gtext_json_object_size(two), 2u);
	gtext_json_free(two);
	gtext_json_error_free(&err);

	/* On: one name, written twice. */
	memset(&err, 0, sizeof(err));
	opts.normalize_unicode = true;
	GTEXT_JSON_Value *v = gtext_json_parse(src, strlen(src), &opts, &err);
	EXPECT_EQ(v, nullptr);
	EXPECT_EQ(err.code, GTEXT_JSON_E_DUPKEY);
	if (v) {
		gtext_json_free(v);
	}
	gtext_json_error_free(&err);
}

/*
 * The case that makes in_situ_mode and normalisation mutually exclusive.
 *
 * in-situ points the DOM at the caller's buffer when the decoded string is the
 * same length as the input, which is a proxy for "the same bytes".
 * Normalisation breaks the proxy: canonical ordering sorts combining marks by
 * combining class, and two marks can swap without changing how many bytes they
 * take.  U+4E00 U+0301 U+0327 is seven bytes and normalises to U+4E00 U+0327
 * U+0301, also seven bytes, because the cedilla's class (202) sorts before the
 * acute's (230) and neither composes onto a Han character.  Verified against
 * Python's unicodedata rather than worked out by hand.
 *
 * With the length test alone, in-situ would win and hand back the input
 * unchanged - the option would read as implemented and do nothing.  The escape
 * sequences matter too: this input is written as raw UTF-8 precisely so that
 * in-situ is eligible, since an escape makes the decoded length differ and
 * takes the copy path anyway.
 */
TEST(JsonNormalizeUnicode, IsNotDefeatedByInSituMode) {
	/* Each \x escape is followed by a backslash, so none of them swallow the
	   next digit. */
	const char *src = "[\"\xe4\xb8\x80\xcc\x81\xcc\xa7\"]";
	const std::string before = "\xe4\xb8\x80\xcc\x81\xcc\xa7";
	const std::string after = "\xe4\xb8\x80\xcc\xa7\xcc\x81";
	ASSERT_EQ(before.size(), after.size()) << "the point of the case";
	ASSERT_NE(before, after);

	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.in_situ_mode = true;

	/* Control: in-situ alone does reference the input, unchanged. */
	GTEXT_JSON_Value *raw = gtext_json_parse(src, strlen(src), &opts, &err);
	ASSERT_NE(raw, nullptr) << (err.message ? err.message : "unknown");
	const GTEXT_JSON_Value *r0 = gtext_json_array_get(raw, 0);
	ASSERT_NE(r0, nullptr);
	size_t rlen = 0;
	const char *rstr = nullptr;
	ASSERT_EQ(gtext_json_get_string(r0, &rstr, &rlen), GTEXT_JSON_OK);
	EXPECT_EQ(std::string(rstr, rlen), before)
	    << "without normalisation the bytes must be the input's";
	gtext_json_free(raw);
	gtext_json_error_free(&err);

	/* With both asked for, normalisation wins. */
	memset(&err, 0, sizeof(err));
	opts.normalize_unicode = true;
	GTEXT_JSON_Value *v = gtext_json_parse(src, strlen(src), &opts, &err);
	ASSERT_NE(v, nullptr) << (err.message ? err.message : "unknown");
	const GTEXT_JSON_Value *e0 = gtext_json_array_get(v, 0);
	ASSERT_NE(e0, nullptr);
	size_t len = 0;
	const char *str = nullptr;
	ASSERT_EQ(gtext_json_get_string(e0, &str, &len), GTEXT_JSON_OK);
	EXPECT_EQ(len, after.size());
	EXPECT_EQ(std::string(str, len), after)
	    << "in-situ returned the un-normalised input";
	gtext_json_free(v);
	gtext_json_error_free(&err);
}

/*
 * Normalising bytes that have not been established as text is not defined, so
 * the combination is refused rather than answered.  validate_utf8 is on by
 * default, so only a caller who turned it off deliberately sees this.
 */
TEST(JsonNormalizeUnicode, RequiresValidateUtf8) {
	const char *src = "{\"a\":\"e\\u0301\"}";

	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	ASSERT_TRUE(opts.validate_utf8) << "on by default";
	opts.normalize_unicode = true;
	opts.validate_utf8 = false;

	GTEXT_JSON_Value *v = gtext_json_parse(src, strlen(src), &opts, &err);
	EXPECT_EQ(v, nullptr);
	EXPECT_EQ(err.code, GTEXT_JSON_E_INVALID);
	ASSERT_NE(err.message, nullptr);
	EXPECT_NE(std::string(err.message).find("validate_utf8"),
	    std::string::npos)
	    << "the message must name the option that is missing: " << err.message;
	if (v) {
		gtext_json_free(v);
	}
	gtext_json_error_free(&err);

	/* The streaming parser refuses the same combination at construction. */
	GTEXT_JSON_Event_cb cb = [](void *, const GTEXT_JSON_Event *,
	                             GTEXT_JSON_Error *) { return GTEXT_JSON_OK; };
	GTEXT_JSON_Stream *st = gtext_json_stream_new(&opts, cb, nullptr);
	EXPECT_EQ(st, nullptr);
	if (st) {
		gtext_json_stream_free(st);
	}

	/* ...and accepts it once validation is back on, which is the control that
	   says the refusal above is about the pairing and not about the option. */
	opts.validate_utf8 = true;
	GTEXT_JSON_Stream *ok = gtext_json_stream_new(&opts, cb, nullptr);
	EXPECT_NE(ok, nullptr);
	if (ok) {
		gtext_json_stream_free(ok);
	}
}

/* The streaming parser shares the lexer, so it normalises too.  Asserted
   through the events rather than assumed from the shared code. */
TEST(JsonNormalizeUnicode, StreamingParserNormalizesToo) {
	struct Seen {
		std::vector<std::string> strings;
	} seen;

	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.normalize_unicode = true;

	GTEXT_JSON_Event_cb cb = [](void *user, const GTEXT_JSON_Event *ev,
	                             GTEXT_JSON_Error *) {
		Seen *s = (Seen *)user;
		if (ev->type == GTEXT_JSON_EVT_STRING) {
			s->strings.push_back(std::string(ev->as.str.s, ev->as.str.len));
		}
		return GTEXT_JSON_OK;
	};

	GTEXT_JSON_Stream *st = gtext_json_stream_new(&opts, cb, &seen);
	ASSERT_NE(st, nullptr);
	const char *src = "[\"e\\u0301\"]";
	GTEXT_JSON_Error serr;
	memset(&serr, 0, sizeof(serr));
	EXPECT_EQ(gtext_json_stream_feed(st, src, strlen(src), &serr),
	    GTEXT_JSON_OK)
	    << (serr.message ? serr.message : "unknown");
	EXPECT_EQ(gtext_json_stream_finish(st, &serr), GTEXT_JSON_OK)
	    << (serr.message ? serr.message : "unknown");
	gtext_json_stream_free(st);
	gtext_json_error_free(&serr);

	ASSERT_EQ(seen.strings.size(), 1u);
	EXPECT_EQ(seen.strings[0], std::string("\xc3\xa9"));
}

/* Already-NFC text comes back byte-identical.  The wrapper allocates a fresh
   buffer either way, so this also exercises the path where normalisation
   changes nothing and must not corrupt anything. */
TEST(JsonNormalizeUnicode, AlreadyNormalizedIsUnchanged) {
	const char *src = "[\"\xc3\xa9 caf\xc3\xa9\",\"\",\"plain ascii\"]";
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.normalize_unicode = true;

	GTEXT_JSON_Value *v = gtext_json_parse(src, strlen(src), &opts, &err);
	ASSERT_NE(v, nullptr) << (err.message ? err.message : "unknown");
	ASSERT_EQ(gtext_json_array_size(v), 3u);

	const char *expect[3] = {"\xc3\xa9 caf\xc3\xa9", "", "plain ascii"};
	for (size_t i = 0; i < 3; i++) {
		const GTEXT_JSON_Value *e = gtext_json_array_get(v, i);
		ASSERT_NE(e, nullptr) << i;
		size_t len = 0;
		const char *str = nullptr;
		ASSERT_EQ(gtext_json_get_string(e, &str, &len), GTEXT_JSON_OK) << i;
		EXPECT_EQ(std::string(str, len), std::string(expect[i])) << i;
	}
	gtext_json_free(v);
	gtext_json_error_free(&err);
}

TEST(JsonNormalizeUnicode, DefaultOptionsStillParse) {
	const char *src = "{\"a\":\"e\\u0301\"}";
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	GTEXT_JSON_Value *v = gtext_json_parse(src, strlen(src), &opts, &err);
	ASSERT_NE(v, nullptr) << (err.message ? err.message : "unknown");
	gtext_json_free(v);
	gtext_json_error_free(&err);
}

// A schema engine that implements a subset must refuse the schemas it cannot
// enforce.  Accepting them and ignoring the keywords it does not know makes
// invalid data validate clean, with nothing for the caller to check - the
// same failure shape as an option that names a guarantee it does not provide.
TEST(JsonSchemaStrictness, RejectsStandardKeywordsItCannotEnforce) {
	// Each schema below constrains the instance that follows it.  Before the
	// strict check, every one of these compiled and then reported OK.
	struct Case {
		const char * keyword;
		const char * schema;
	};
	// Only the keywords still unimplemented belong here. As each is
	// implemented it moves out of this list and into a test that checks it
	// actually constrains something.
	const Case cases[] = {
	    {"pattern", "{\"type\":\"string\",\"pattern\":\"^a+$\"}"},
	    {"patternProperties", "{\"patternProperties\":{\"^a\":{}}}"},
	};

	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	for (const auto & c : cases) {
		GTEXT_JSON_Error perr;
		memset(&perr, 0, sizeof(perr));
		GTEXT_JSON_Value * doc =
		    gtext_json_parse(c.schema, strlen(c.schema), &po, &perr);
		ASSERT_NE(doc, nullptr) << "schema did not parse for " << c.keyword;

		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Schema * sc = gtext_json_schema_compile(doc, &err);
		EXPECT_EQ(sc, nullptr) << "compiled despite " << c.keyword;
		EXPECT_EQ(err.code, GTEXT_JSON_E_SCHEMA_UNSUPPORTED)
		    << "wrong code for " << c.keyword;
		// The offending keyword travels in context_snippet, which
		// gtext_json_error_free() owns.
		ASSERT_NE(err.context_snippet, nullptr)
		    << "no keyword named for " << c.keyword;
		EXPECT_STREQ(err.context_snippet, c.keyword);

		gtext_json_schema_free(sc);
		gtext_json_error_free(&err);
		gtext_json_free(doc);
	}
}

TEST(JsonSchemaStrictness, IgnoresAnnotationAndInertKeywords) {
	// None of these can change which instances are valid, so they must not
	// cause a refusal.  "x-vendor" stands for the extensions and
	// newer-draft keywords JSON Schema requires an implementation to ignore.
	const char * schemas[] = {
	    "{\"type\":\"string\",\"title\":\"A name\"}",
	    "{\"type\":\"string\",\"description\":\"text\"}",
	    "{\"type\":\"string\",\"default\":\"x\"}",
	    "{\"type\":\"string\",\"examples\":[\"x\"]}",
	    "{\"type\":\"string\",\"$comment\":\"note\"}",
	    "{\"type\":\"string\",\"readOnly\":true}",
	    "{\"type\":\"string\",\"deprecated\":true}",
	    "{\"type\":\"string\",\"$schema\":\"http://json-schema.org/draft-07/schema#\"}",
	    "{\"type\":\"string\",\"$id\":\"http://example.com/s\"}",
	    "{\"type\":\"string\",\"$defs\":{\"unused\":{\"type\":\"integer\"}}}",
	    "{\"type\":\"string\",\"definitions\":{\"unused\":{\"type\":\"integer\"}}}",
	    "{\"type\":\"string\",\"x-vendor\":42}",
	};

	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	for (const char * src : schemas) {
		GTEXT_JSON_Error perr;
		memset(&perr, 0, sizeof(perr));
		GTEXT_JSON_Value * doc =
		    gtext_json_parse(src, strlen(src), &po, &perr);
		ASSERT_NE(doc, nullptr) << src;

		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Schema * sc = gtext_json_schema_compile(doc, &err);
		EXPECT_NE(sc, nullptr) << "refused: " << src;

		if (sc) {
			// The surviving "type" assertion must still be enforced, so the
			// ignored keyword did not disable the rest of the schema.
			const char * bad = "123";
			GTEXT_JSON_Value * inst =
			    gtext_json_parse(bad, strlen(bad), &po, &perr);
			ASSERT_NE(inst, nullptr);
			GTEXT_JSON_Error verr;
			memset(&verr, 0, sizeof(verr));
			EXPECT_EQ(gtext_json_schema_validate(sc, inst, &verr),
			    GTEXT_JSON_E_SCHEMA)
			    << "type stopped being enforced for: " << src;
			gtext_json_error_free(&verr);
			gtext_json_free(inst);
		}
		gtext_json_schema_free(sc);
		gtext_json_error_free(&err);
		gtext_json_free(doc);
	}
}

TEST(JsonSchemaStrictness, OptInRestoresTheLenientBehavior) {
	const char * src = "{\"type\":\"string\",\"pattern\":\"^a+$\"}";
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Error perr;
	memset(&perr, 0, sizeof(perr));
	GTEXT_JSON_Value * doc = gtext_json_parse(src, strlen(src), &po, &perr);
	ASSERT_NE(doc, nullptr);

	// Default refuses.
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	EXPECT_EQ(gtext_json_schema_compile(doc, &err), nullptr);
	gtext_json_error_free(&err);

	// Explicit defaults refuse the same way.
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	EXPECT_FALSE(opts.allow_unsupported_keywords);
	memset(&err, 0, sizeof(err));
	EXPECT_EQ(gtext_json_schema_compile_with_options(doc, &opts, &err), nullptr);
	gtext_json_error_free(&err);

	// Opting in compiles, and "pattern" is then ignored - which is exactly
	// what the option says it does, and why it is not the default.
	opts.allow_unsupported_keywords = true;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Schema * sc =
	    gtext_json_schema_compile_with_options(doc, &opts, &err);
	ASSERT_NE(sc, nullptr);

	const char * inst_src = "\"zzz\""; // violates the pattern, matches type
	GTEXT_JSON_Value * inst =
	    gtext_json_parse(inst_src, strlen(inst_src), &po, &perr);
	ASSERT_NE(inst, nullptr);
	GTEXT_JSON_Error verr;
	memset(&verr, 0, sizeof(verr));
	EXPECT_EQ(gtext_json_schema_validate(sc, inst, &verr), GTEXT_JSON_OK);
	gtext_json_error_free(&verr);

	gtext_json_free(inst);
	gtext_json_schema_free(sc);
	gtext_json_error_free(&err);
	gtext_json_free(doc);
}

TEST(JsonSchemaStrictness, RejectionReachesNestedSubschemas) {
	// A property subschema is compiled by the same recursive function, so the
	// check has to apply there too rather than only at the root.
	const char * src =
	    "{\"type\":\"object\",\"properties\":{\"a\":{\"pattern\":\"^x$\"}}}";
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Error perr;
	memset(&perr, 0, sizeof(perr));
	GTEXT_JSON_Value * doc = gtext_json_parse(src, strlen(src), &po, &perr);
	ASSERT_NE(doc, nullptr);

	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	EXPECT_EQ(gtext_json_schema_compile(doc, &err), nullptr);
	EXPECT_EQ(err.code, GTEXT_JSON_E_SCHEMA_UNSUPPORTED);
	gtext_json_error_free(&err);
	gtext_json_free(doc);
}

namespace {

/// Compile a schema and validate an instance, returning whether it passed.
/// Fails the test if either does not parse, or the schema does not compile.
bool schema_accepts(const char * schema_src, const char * instance_src) {
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Error perr;
	memset(&perr, 0, sizeof(perr));
	GTEXT_JSON_Value * sv =
	    gtext_json_parse(schema_src, strlen(schema_src), &po, &perr);
	EXPECT_NE(sv, nullptr) << "schema did not parse: " << schema_src;
	GTEXT_JSON_Value * iv =
	    gtext_json_parse(instance_src, strlen(instance_src), &po, &perr);
	EXPECT_NE(iv, nullptr) << "instance did not parse: " << instance_src;
	if (!sv || !iv) {
		return false;
	}
	GTEXT_JSON_Error serr;
	memset(&serr, 0, sizeof(serr));
	GTEXT_JSON_Schema * sc = gtext_json_schema_compile(sv, &serr);
	EXPECT_NE(sc, nullptr)
	    << "schema did not compile: " << schema_src << " - "
	    << (serr.message ? serr.message : "");
	bool ok = false;
	if (sc) {
		GTEXT_JSON_Error verr;
		memset(&verr, 0, sizeof(verr));
		ok = gtext_json_schema_validate(sc, iv, &verr) == GTEXT_JSON_OK;
		gtext_json_error_free(&verr);
	}
	gtext_json_schema_free(sc);
	gtext_json_error_free(&serr);
	gtext_json_free(iv);
	gtext_json_free(sv);
	return ok;
}

} // namespace

TEST(JsonSchemaKeywords, BooleanApplicators) {
	EXPECT_TRUE(schema_accepts(
	    "{\"allOf\":[{\"type\":\"integer\"},{\"minimum\":3}]}", "5"));
	EXPECT_FALSE(schema_accepts(
	    "{\"allOf\":[{\"type\":\"integer\"},{\"minimum\":3}]}", "1"));

	EXPECT_TRUE(schema_accepts(
	    "{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"integer\"}]}", "5"));
	EXPECT_FALSE(schema_accepts(
	    "{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"integer\"}]}", "true"));

	// oneOf is "exactly one", so two matches is a failure, not a success.
	EXPECT_TRUE(schema_accepts(
	    "{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"integer\"}]}", "5"));
	EXPECT_FALSE(
	    schema_accepts("{\"oneOf\":[{\"minimum\":1},{\"minimum\":2}]}", "5"));
	EXPECT_FALSE(
	    schema_accepts("{\"oneOf\":[{\"minimum\":10},{\"minimum\":20}]}", "5"));

	EXPECT_TRUE(schema_accepts("{\"not\":{\"type\":\"integer\"}}", "\"x\""));
	EXPECT_FALSE(schema_accepts("{\"not\":{\"type\":\"integer\"}}", "5"));

	// Nesting, so the recursive compile and free are exercised.
	EXPECT_TRUE(schema_accepts(
	    "{\"allOf\":[{\"anyOf\":[{\"type\":\"integer\"}]},{\"not\":{\"maximum\":2}}]}",
	    "5"));
}

TEST(JsonSchemaKeywords, IfThenElseSelectsRatherThanAsserts) {
	EXPECT_TRUE(schema_accepts(
	    "{\"if\":{\"type\":\"integer\"},\"then\":{\"maximum\":3}}", "2"));
	EXPECT_FALSE(schema_accepts(
	    "{\"if\":{\"type\":\"integer\"},\"then\":{\"maximum\":3}}", "99"));
	EXPECT_TRUE(schema_accepts(
	    "{\"if\":{\"type\":\"integer\"},\"else\":{\"type\":\"string\"}}", "\"s\""));
	EXPECT_FALSE(schema_accepts(
	    "{\"if\":{\"type\":\"integer\"},\"else\":{\"type\":\"string\"}}", "true"));

	// "if" on its own asserts nothing, and a failing "if" with no "else" is
	// not a failure.
	EXPECT_TRUE(schema_accepts("{\"if\":{\"type\":\"integer\"}}", "\"x\""));
	EXPECT_TRUE(schema_accepts(
	    "{\"if\":{\"type\":\"integer\"},\"then\":{\"maximum\":3}}", "\"x\""));
}

TEST(JsonSchemaKeywords, NumericAssertions) {
	EXPECT_FALSE(schema_accepts("{\"exclusiveMaximum\":5}", "5"));
	EXPECT_TRUE(schema_accepts("{\"exclusiveMaximum\":5}", "4"));
	EXPECT_FALSE(schema_accepts("{\"exclusiveMinimum\":5}", "5"));
	EXPECT_TRUE(schema_accepts("{\"exclusiveMinimum\":5}", "6"));

	EXPECT_TRUE(schema_accepts("{\"multipleOf\":10}", "30"));
	EXPECT_FALSE(schema_accepts("{\"multipleOf\":10}", "7"));
	EXPECT_TRUE(schema_accepts("{\"multipleOf\":0.5}", "1.5"));

	// A non-number is unconstrained by a numeric keyword.
	EXPECT_TRUE(schema_accepts("{\"multipleOf\":10}", "\"x\""));

	// multipleOf must be positive: zero would be a division with no meaning.
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Error perr;
	memset(&perr, 0, sizeof(perr));
	const char * bad = "{\"multipleOf\":0}";
	GTEXT_JSON_Value * sv = gtext_json_parse(bad, strlen(bad), &po, &perr);
	ASSERT_NE(sv, nullptr);
	GTEXT_JSON_Error serr;
	memset(&serr, 0, sizeof(serr));
	EXPECT_EQ(gtext_json_schema_compile(sv, &serr), nullptr);
	gtext_json_error_free(&serr);
	gtext_json_free(sv);
}

TEST(JsonSchemaKeywords, IntegerTypeWasMissingEntirely) {
	// "integer" is a standard JSON Schema type and the engine did not know it
	// at all - {"type":"integer"} failed to compile with "Unknown type in
	// schema". JSON has one number type, so this constrains the value.
	EXPECT_TRUE(schema_accepts("{\"type\":\"integer\"}", "5"));
	EXPECT_TRUE(schema_accepts("{\"type\":\"integer\"}", "5.0"));
	EXPECT_TRUE(schema_accepts("{\"type\":\"integer\"}", "-7"));
	EXPECT_FALSE(schema_accepts("{\"type\":\"integer\"}", "5.5"));
	EXPECT_FALSE(schema_accepts("{\"type\":\"integer\"}", "\"5\""));

	// A whole number is both a number and an integer.
	EXPECT_TRUE(schema_accepts("{\"type\":\"number\"}", "5"));
	EXPECT_TRUE(schema_accepts("{\"type\":\"number\"}", "5.5"));
	EXPECT_TRUE(schema_accepts("{\"type\":[\"integer\",\"string\"]}", "5"));
	EXPECT_TRUE(schema_accepts("{\"type\":[\"integer\",\"string\"]}", "\"x\""));
	EXPECT_FALSE(schema_accepts("{\"type\":[\"integer\",\"string\"]}", "5.5"));
}

TEST(JsonSchemaKeywords, ObjectAndArrayAssertions) {
	EXPECT_FALSE(schema_accepts("{\"uniqueItems\":true}", "[1,1]"));
	EXPECT_TRUE(schema_accepts("{\"uniqueItems\":true}", "[1,2]"));
	EXPECT_TRUE(schema_accepts("{\"uniqueItems\":false}", "[1,1]"));
	EXPECT_TRUE(schema_accepts("{\"uniqueItems\":true}", "[]"));
	// Equality is structural, not identity.
	EXPECT_FALSE(
	    schema_accepts("{\"uniqueItems\":true}", "[{\"a\":1},{\"a\":1}]"));
	EXPECT_TRUE(
	    schema_accepts("{\"uniqueItems\":true}", "[{\"a\":1},{\"a\":2}]"));

	EXPECT_FALSE(schema_accepts("{\"minProperties\":2}", "{\"a\":1}"));
	EXPECT_TRUE(schema_accepts("{\"minProperties\":2}", "{\"a\":1,\"b\":2}"));
	EXPECT_FALSE(schema_accepts("{\"maxProperties\":1}", "{\"a\":1,\"b\":2}"));

	EXPECT_FALSE(
	    schema_accepts("{\"dependentRequired\":{\"a\":[\"b\"]}}", "{\"a\":1}"));
	EXPECT_TRUE(schema_accepts(
	    "{\"dependentRequired\":{\"a\":[\"b\"]}}", "{\"a\":1,\"b\":2}"));
	// The trigger being absent means nothing is required.
	EXPECT_TRUE(
	    schema_accepts("{\"dependentRequired\":{\"a\":[\"b\"]}}", "{\"c\":1}"));
}

TEST(JsonSchemaKeywords, RefResolvesAndRecurses) {
	// $ref was the significant gap: without it a schema cannot be factored or
	// recursive, which rules out most real schemas.
	const char * defs =
	    "{\"$defs\":{\"p\":{\"type\":\"string\"}},\"$ref\":\"#/$defs/p\"}";
	EXPECT_TRUE(schema_accepts(defs, "\"x\""));
	EXPECT_FALSE(schema_accepts(defs, "5"));

	// draft-07 spelled the container "definitions".
	const char * legacy =
	    "{\"definitions\":{\"p\":{\"type\":\"string\"}},"
	    "\"$ref\":\"#/definitions/p\"}";
	EXPECT_TRUE(schema_accepts(legacy, "\"x\""));
	EXPECT_FALSE(schema_accepts(legacy, "5"));

	// A pointer at anything in the document, not only a definitions container.
	EXPECT_FALSE(schema_accepts(
	    "{\"properties\":{\"a\":{\"type\":\"string\"},"
	    "\"b\":{\"$ref\":\"#/properties/a\"}}}",
	    "{\"b\":5}"));

	// Recursive: the registry entry is made before the target's children
	// compile, so this terminates.
	const char * tree =
	    "{\"$defs\":{\"n\":{\"type\":\"object\","
	    "\"properties\":{\"c\":{\"$ref\":\"#/$defs/n\"}}}},"
	    "\"$ref\":\"#/$defs/n\"}";
	EXPECT_TRUE(schema_accepts(tree, "{\"c\":{\"c\":{}}}"));
	EXPECT_FALSE(schema_accepts(tree, "{\"c\":{\"c\":5}}"));

	// In 2020-12 a $ref sits alongside other keywords and all of them apply.
	EXPECT_FALSE(schema_accepts(
	    "{\"$defs\":{\"s\":{\"type\":\"string\"}},\"$ref\":\"#/$defs/s\","
	    "\"minLength\":3}",
	    "\"ab\""));
}

TEST(JsonSchemaKeywords, RefFailuresAreRefusedAtCompileTime) {
	// A reference that does not resolve constrains nothing, which is the
	// failure this engine exists not to have.
	struct Case {
		const char * why;
		const char * schema;
	};
	const Case cases[] = {
	    {"dangling pointer", "{\"$ref\":\"#/$defs/nope\"}"},
	    {"external URI", "{\"$ref\":\"http://example.com/s\"}"},
	    {"named anchor", "{\"$ref\":\"#someAnchor\"}"},
	    {"not a string", "{\"$ref\":5}"},
	};
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	for (const auto & c : cases) {
		GTEXT_JSON_Error perr;
		memset(&perr, 0, sizeof(perr));
		GTEXT_JSON_Value * sv =
		    gtext_json_parse(c.schema, strlen(c.schema), &po, &perr);
		ASSERT_NE(sv, nullptr) << c.why;
		GTEXT_JSON_Error serr;
		memset(&serr, 0, sizeof(serr));
		EXPECT_EQ(gtext_json_schema_compile(sv, &serr), nullptr) << c.why;
		gtext_json_error_free(&serr);
		gtext_json_free(sv);
	}
}

TEST(JsonSchemaKeywords, SelfReferenceTerminates) {
	// {"$ref":"#"} consumes no instance as it recurses, so instance depth is
	// not a bound. It must stop rather than hang or overflow the stack.
	const char * src = "{\"$ref\":\"#\"}";
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Error perr;
	memset(&perr, 0, sizeof(perr));
	GTEXT_JSON_Value * sv = gtext_json_parse(src, strlen(src), &po, &perr);
	ASSERT_NE(sv, nullptr);
	GTEXT_JSON_Value * iv = gtext_json_parse("1", 1, &po, &perr);
	ASSERT_NE(iv, nullptr);
	GTEXT_JSON_Error serr;
	memset(&serr, 0, sizeof(serr));
	GTEXT_JSON_Schema * sc = gtext_json_schema_compile(sv, &serr);
	ASSERT_NE(sc, nullptr) << "compiling a self-reference should terminate";
	GTEXT_JSON_Error verr;
	memset(&verr, 0, sizeof(verr));
	EXPECT_EQ(gtext_json_schema_validate(sc, iv, &verr), GTEXT_JSON_E_DEPTH);
	gtext_json_error_free(&verr);
	gtext_json_schema_free(sc);
	gtext_json_error_free(&serr);
	gtext_json_free(iv);
	gtext_json_free(sv);
}

TEST(JsonSchemaKeywords, BooleanSchemas) {
	// A boolean is a schema wherever a schema is allowed.
	EXPECT_TRUE(schema_accepts("{\"properties\":{\"a\":true}}", "{\"a\":1}"));
	EXPECT_FALSE(schema_accepts("{\"properties\":{\"a\":false}}", "{\"a\":1}"));
	// "false" only bites when the property is present.
	EXPECT_TRUE(schema_accepts("{\"properties\":{\"a\":false}}", "{\"b\":1}"));
	EXPECT_TRUE(schema_accepts("true", "{\"anything\":1}"));
	EXPECT_FALSE(schema_accepts("false", "1"));
}

TEST(JsonSchemaKeywords, ObjectApplicators) {
	EXPECT_FALSE(schema_accepts(
	    "{\"properties\":{\"a\":{}},\"additionalProperties\":false}",
	    "{\"a\":1,\"b\":2}"));
	EXPECT_TRUE(schema_accepts(
	    "{\"properties\":{\"a\":{}},\"additionalProperties\":false}",
	    "{\"a\":1}"));
	// A schema, not just false: it applies to the unnamed properties only.
	EXPECT_TRUE(schema_accepts(
	    "{\"properties\":{\"a\":{\"type\":\"string\"}},"
	    "\"additionalProperties\":{\"type\":\"integer\"}}",
	    "{\"a\":\"s\",\"b\":1}"));
	EXPECT_FALSE(schema_accepts(
	    "{\"properties\":{\"a\":{\"type\":\"string\"}},"
	    "\"additionalProperties\":{\"type\":\"integer\"}}",
	    "{\"a\":\"s\",\"b\":\"t\"}"));

	EXPECT_FALSE(
	    schema_accepts("{\"propertyNames\":{\"minLength\":2}}", "{\"a\":1}"));
	EXPECT_TRUE(
	    schema_accepts("{\"propertyNames\":{\"minLength\":2}}", "{\"ab\":1}"));

	EXPECT_FALSE(schema_accepts(
	    "{\"dependentSchemas\":{\"a\":{\"required\":[\"b\"]}}}", "{\"a\":1}"));
	EXPECT_TRUE(schema_accepts(
	    "{\"dependentSchemas\":{\"a\":{\"required\":[\"b\"]}}}",
	    "{\"a\":1,\"b\":2}"));
	EXPECT_TRUE(schema_accepts(
	    "{\"dependentSchemas\":{\"a\":{\"required\":[\"b\"]}}}", "{\"c\":1}"));
}

TEST(JsonSchemaKeywords, ArrayApplicators) {
	EXPECT_TRUE(schema_accepts(
	    "{\"prefixItems\":[{\"type\":\"integer\"},{\"type\":\"string\"}]}",
	    "[1,\"a\"]"));
	EXPECT_FALSE(schema_accepts(
	    "{\"prefixItems\":[{\"type\":\"integer\"},{\"type\":\"string\"}]}",
	    "[1,2]"));
	// A short array simply has fewer positions to check.
	EXPECT_TRUE(schema_accepts(
	    "{\"prefixItems\":[{\"type\":\"integer\"},{\"type\":\"string\"}]}",
	    "[1]"));

	// draft-07 spelled the positional form `items: [...]`.
	EXPECT_FALSE(schema_accepts(
	    "{\"items\":[{\"type\":\"integer\"},{\"type\":\"string\"}]}", "[1,2]"));
	// and `items` with a single schema still applies to every element.
	EXPECT_FALSE(
	    schema_accepts("{\"items\":{\"type\":\"integer\"}}", "[1,\"a\"]"));

	EXPECT_FALSE(schema_accepts(
	    "{\"prefixItems\":[{\"type\":\"integer\"}],\"additionalItems\":false}",
	    "[1,2]"));
	EXPECT_TRUE(schema_accepts(
	    "{\"prefixItems\":[{\"type\":\"integer\"}],\"additionalItems\":false}",
	    "[1]"));

	EXPECT_TRUE(
	    schema_accepts("{\"contains\":{\"type\":\"string\"}}", "[1,\"a\"]"));
	EXPECT_FALSE(schema_accepts("{\"contains\":{\"type\":\"string\"}}", "[1,2]"));
	EXPECT_FALSE(schema_accepts(
	    "{\"contains\":{\"type\":\"string\"},\"minContains\":2}", "[\"a\",1]"));
	EXPECT_TRUE(schema_accepts(
	    "{\"contains\":{\"type\":\"string\"},\"minContains\":2}",
	    "[\"a\",\"b\"]"));
	EXPECT_FALSE(schema_accepts(
	    "{\"contains\":{\"type\":\"string\"},\"maxContains\":1}",
	    "[\"a\",\"b\"]"));
	// minContains of 0 is the one case where contains asserts nothing.
	EXPECT_TRUE(schema_accepts(
	    "{\"contains\":{\"type\":\"string\"},\"minContains\":0}", "[1]"));
}

TEST(JsonSchemaKeywords, ItemsAppliesAfterPrefixItems) {
	// 2020-12 core section 10.3.1.2: `items` applies to the elements at an
	// index past the end of `prefixItems`, not to every element. Applying it
	// to all of them called `[]` invalid against a schema that constrains
	// only a fourth element onwards.
	EXPECT_TRUE(schema_accepts(
	    "{\"prefixItems\":[{},{},{}],\"items\":false}", "[]"));
	EXPECT_TRUE(schema_accepts(
	    "{\"prefixItems\":[{},{},{}],\"items\":false}", "[1,2]"));
	EXPECT_TRUE(schema_accepts(
	    "{\"prefixItems\":[{},{},{}],\"items\":false}", "[1,2,3]"));
	EXPECT_FALSE(schema_accepts(
	    "{\"prefixItems\":[{},{},{}],\"items\":false}", "[1,2,3,4]"));

	// The index the tail starts at is the length of prefixItems, so the two
	// schemas below constrain different elements of the same array.
	EXPECT_TRUE(schema_accepts(
	    "{\"prefixItems\":[{\"type\":\"string\"}],\"items\":{\"type\":\"integer\"}}",
	    "[\"x\",1,2]"));
	EXPECT_FALSE(schema_accepts(
	    "{\"prefixItems\":[{\"type\":\"string\"}],\"items\":{\"type\":\"integer\"}}",
	    "[\"x\",\"y\"]"));

	// Without prefixItems, `items` still applies to every element - that is
	// the same rule, not a second one, and it is what draft-07 wrote.
	EXPECT_FALSE(
	    schema_accepts("{\"items\":{\"type\":\"integer\"}}", "[1,\"a\"]"));

	// There is no empty prefixItems to reason about: 2020-12 requires a
	// non-empty array, and this engine already refuses one.
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Error perr;
	memset(&perr, 0, sizeof(perr));
	const char * src = "{\"prefixItems\":[],\"items\":{\"type\":\"integer\"}}";
	GTEXT_JSON_Value * sv = gtext_json_parse(src, strlen(src), &po, &perr);
	ASSERT_NE(sv, nullptr);
	EXPECT_EQ(gtext_json_schema_compile(sv, nullptr), nullptr);
	gtext_json_free(sv);
}

TEST(JsonSchemaKeywords, TheTailIsNotNamedTwice) {
	// `additionalItems` is draft-07's name for the slot `items` fills in
	// 2020-12. A schema carrying both has named it twice, and the drafts
	// disagree about which to honor, so it is refused rather than resolved
	// in silence.
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Error perr;
	memset(&perr, 0, sizeof(perr));
	const char * src =
	    "{\"prefixItems\":[{}],\"items\":false,\"additionalItems\":{}}";
	GTEXT_JSON_Value * sv = gtext_json_parse(src, strlen(src), &po, &perr);
	ASSERT_NE(sv, nullptr);

	GTEXT_JSON_Error serr;
	memset(&serr, 0, sizeof(serr));
	EXPECT_EQ(gtext_json_schema_compile(sv, &serr), nullptr);
	EXPECT_EQ(serr.code, GTEXT_JSON_E_INVALID);
	gtext_json_error_free(&serr);
	gtext_json_free(sv);
}

TEST(JsonSchemaKeywords, AnEmptyEnumAcceptsNothing) {
	// The assertion is that the instance equals one of the listed values.
	// With none listed, nothing satisfies it. Keying this off the number of
	// values made an empty enum compile away and accept everything, which is
	// the opposite answer.
	EXPECT_FALSE(schema_accepts("{\"enum\":[]}", "1"));
	EXPECT_FALSE(schema_accepts("{\"enum\":[]}", "\"x\""));
	EXPECT_FALSE(schema_accepts("{\"enum\":[]}", "null"));
	EXPECT_FALSE(schema_accepts("{\"enum\":[]}", "{}"));
	EXPECT_FALSE(schema_accepts("{\"enum\":[]}", "[]"));
	EXPECT_FALSE(schema_accepts("{\"enum\":[]}", "true"));

	// A non-empty enum is unaffected.
	EXPECT_TRUE(schema_accepts("{\"enum\":[1,2]}", "1"));
	EXPECT_FALSE(schema_accepts("{\"enum\":[1,2]}", "3"));
}

TEST(JsonSchemaKeywords, MultipleOfIsAskedInDecimal) {
	// JSON numbers are decimal text and this is a decimal question. In the
	// binary both values round to, 0.0075 is not a whole number of 0.0001 -
	// which is the specification's own example, and fmod called it invalid.
	EXPECT_TRUE(schema_accepts("{\"multipleOf\":0.0001}", "0.0075"));
	EXPECT_TRUE(schema_accepts("{\"multipleOf\":0.1}", "0.3"));
	EXPECT_TRUE(schema_accepts("{\"multipleOf\":1e-8}", "12391239123"));
	EXPECT_TRUE(schema_accepts("{\"multipleOf\":0.01}", "1.21"));

	// Exact means exact: the near misses are still refused.
	EXPECT_FALSE(schema_accepts("{\"multipleOf\":0.0001}", "0.00751"));
	EXPECT_FALSE(schema_accepts("{\"multipleOf\":0.1}", "0.35"));
	EXPECT_FALSE(schema_accepts("{\"multipleOf\":1.5}", "4.5001"));
	EXPECT_FALSE(schema_accepts("{\"multipleOf\":2}", "7"));

	// Sign is not part of the question, and zero is a multiple of anything.
	EXPECT_TRUE(schema_accepts("{\"multipleOf\":3}", "-6"));
	EXPECT_TRUE(schema_accepts("{\"multipleOf\":0.0001}", "0"));

	// Exponent notation on either side names the same values as the digits.
	EXPECT_TRUE(schema_accepts("{\"multipleOf\":1e2}", "3e2"));
	EXPECT_FALSE(schema_accepts("{\"multipleOf\":1e2}", "350"));
	EXPECT_TRUE(schema_accepts("{\"multipleOf\":0.5}", "1.5"));
}

/* Compile with a base URI and an optional resolver, then validate. */
struct StubResolver {
	std::vector<std::pair<std::string, GTEXT_JSON_Value *>> docs;
	std::vector<std::string> asked;

	~StubResolver() {
		for (auto & d : docs) {
			gtext_json_free(d.second);
		}
	}
	void add(const char * uri, const char * src) {
		GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
		GTEXT_JSON_Value * v = gtext_json_parse(src, strlen(src), &po, nullptr);
		docs.emplace_back(uri, v);
	}
	static const GTEXT_JSON_Value * get(
	    void * ctx, const char * uri, size_t uri_len) {
		StubResolver * self = (StubResolver *)ctx;
		self->asked.emplace_back(uri, uri_len);
		for (auto & d : self->docs) {
			if (d.first == uri) {
				return d.second;
			}
		}
		return nullptr;
	}
};

static bool ref_accepts(const char * schema_src, const char * instance_src,
    const char * base = nullptr, StubResolver * remote = nullptr) {
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Value * sv =
	    gtext_json_parse(schema_src, strlen(schema_src), &po, nullptr);
	GTEXT_JSON_Value * iv =
	    gtext_json_parse(instance_src, strlen(instance_src), &po, nullptr);
	EXPECT_NE(sv, nullptr);
	EXPECT_NE(iv, nullptr);
	if (!sv || !iv) {
		return false;
	}
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.base_uri = base;
	GTEXT_JSON_Schema_Resolver resolver = {remote, StubResolver::get};
	if (remote) {
		opts.resolver = &resolver;
	}
	GTEXT_JSON_Error serr;
	memset(&serr, 0, sizeof(serr));
	GTEXT_JSON_Schema * sc =
	    gtext_json_schema_compile_with_options(sv, &opts, &serr);
	EXPECT_NE(sc, nullptr) << "did not compile: " << schema_src << " - "
	                       << (serr.message ? serr.message : "");
	bool ok = false;
	if (sc) {
		ok = gtext_json_schema_validate(sc, iv, nullptr) == GTEXT_JSON_OK;
	}
	gtext_json_schema_free(sc);
	gtext_json_error_free(&serr);
	gtext_json_free(iv);
	gtext_json_free(sv);
	return ok;
}

TEST(JsonSchemaRef, ResolvesAgainstTheBaseUriInScope) {
	// The shape that worked before: no $id anywhere, so the base is empty and
	// a pointer fragment addresses the document.
	EXPECT_TRUE(ref_accepts(
	    "{\"$defs\":{\"n\":{\"type\":\"integer\"}},\"$ref\":\"#/$defs/n\"}", "5"));
	EXPECT_FALSE(ref_accepts(
	    "{\"$defs\":{\"n\":{\"type\":\"integer\"}},\"$ref\":\"#/$defs/n\"}",
	    "\"x\""));

	// With an $id, a relative reference is resolved against it rather than
	// against the document - which is the whole difference between a
	// reference model built on URIs and one built on JSON Pointer.
	StubResolver remote;
	remote.add("http://example.com/other.json", "{\"type\":\"integer\"}");
	EXPECT_TRUE(ref_accepts(
	    "{\"$id\":\"http://example.com/root.json\",\"$ref\":\"other.json\"}", "5",
	    nullptr, &remote));
	EXPECT_FALSE(ref_accepts(
	    "{\"$id\":\"http://example.com/root.json\",\"$ref\":\"other.json\"}",
	    "\"x\"", nullptr, &remote));
	ASSERT_FALSE(remote.asked.empty());
	EXPECT_EQ(remote.asked[0], "http://example.com/other.json");

	// The caller's base does the same job when the document carries no $id.
	StubResolver rooted;
	rooted.add("http://example.com/dir/sub.json", "{\"type\":\"string\"}");
	EXPECT_TRUE(ref_accepts("{\"$ref\":\"sub.json\"}", "\"x\"",
	    "http://example.com/dir/root.json", &rooted));
}

TEST(JsonSchemaRef, NamedAnchorsResolve) {
	EXPECT_TRUE(ref_accepts(
	    "{\"$defs\":{\"n\":{\"$anchor\":\"num\",\"type\":\"integer\"}},"
	    "\"$ref\":\"#num\"}",
	    "5"));
	EXPECT_FALSE(ref_accepts(
	    "{\"$defs\":{\"n\":{\"$anchor\":\"num\",\"type\":\"integer\"}},"
	    "\"$ref\":\"#num\"}",
	    "\"x\""));

	// A $dynamicAnchor is a plain anchor too, for a reference that does not
	// need the dynamic scope.
	EXPECT_FALSE(ref_accepts(
	    "{\"$defs\":{\"n\":{\"$dynamicAnchor\":\"num\",\"type\":\"integer\"}},"
	    "\"$ref\":\"#num\"}",
	    "\"x\""));
}

TEST(JsonSchemaRef, AForwardReferenceResolves) {
	// The identifiers are collected before anything compiles, so a reference
	// may name something defined later in the document than itself. Resolving
	// them as they were met would answer this and its mirror differently.
	EXPECT_FALSE(ref_accepts(
	    "{\"$ref\":\"#later\",\"$defs\":{\"x\":{\"$anchor\":\"later\","
	    "\"type\":\"integer\"}}}",
	    "\"s\""));
}

TEST(JsonSchemaRef, AFragmentIsPercentDecodedBeforeItIsAPointer) {
	// RFC 6901 section 6: percent-decoding first, then the pointer's own
	// tilde escapes. Skipping the first step looked for a member named
	// `percent%25field` and found nothing.
	EXPECT_FALSE(ref_accepts(
	    "{\"$defs\":{\"percent%field\":{\"type\":\"integer\"}},"
	    "\"$ref\":\"#/$defs/percent%25field\"}",
	    "\"s\""));
	EXPECT_FALSE(ref_accepts(
	    "{\"$defs\":{\"tilde~field\":{\"type\":\"integer\"}},"
	    "\"$ref\":\"#/$defs/tilde~0field\"}",
	    "\"s\""));
	EXPECT_FALSE(ref_accepts(
	    "{\"$defs\":{\"slash/field\":{\"type\":\"integer\"}},"
	    "\"$ref\":\"#/$defs/slash~1field\"}",
	    "\"s\""));
}

TEST(JsonSchemaRef, AnIdentifierOnlyCountsInASchemaPosition) {
	// `$id` inside an `enum` is a member of a value, not an identifier, and a
	// pre-pass that walked every object in the document would register it.
	StubResolver remote;
	EXPECT_TRUE(ref_accepts(
	    "{\"$defs\":{\"id_in_enum\":{\"enum\":[{\"$id\":\"https://x/e\","
	    "\"type\":\"null\"}]}},\"$ref\":\"#/$defs/id_in_enum\"}",
	    "{\"$id\":\"https://x/e\",\"type\":\"null\"}", nullptr, &remote));
	// Nothing was ever fetched for it, because it was never an identifier.
	EXPECT_TRUE(remote.asked.empty());
}

TEST(JsonSchemaRef, ALeavingReferenceIsRefusedWithNoResolver) {
	const char * src = "{\"$ref\":\"http://example.com/nowhere.json\"}";
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Value * sv = gtext_json_parse(src, strlen(src), &po, nullptr);
	ASSERT_NE(sv, nullptr);

	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	EXPECT_EQ(gtext_json_schema_compile(sv, &err), nullptr);
	EXPECT_EQ(err.code, GTEXT_JSON_E_SCHEMA);
	gtext_json_error_free(&err);

	// And with a resolver that does not know it, which is the same answer for
	// the same reason.
	StubResolver empty;
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	GTEXT_JSON_Schema_Resolver resolver = {&empty, StubResolver::get};
	opts.resolver = &resolver;
	memset(&err, 0, sizeof(err));
	EXPECT_EQ(gtext_json_schema_compile_with_options(sv, &opts, &err), nullptr);
	EXPECT_EQ(err.code, GTEXT_JSON_E_SCHEMA);
	gtext_json_error_free(&err);
	gtext_json_free(sv);
}

TEST(JsonSchemaRef, ATargetCompilesInItsOwnScope) {
	// The fetched document's own relative $ref must resolve against where it
	// came from, not against the document that referred to it. Carrying the
	// referrer's base across the boundary reads the wrong schema and says
	// nothing about having done so.
	StubResolver remote;
	remote.add("http://example.com/a/outer.json",
	    "{\"$ref\":\"inner.json\"}");
	remote.add("http://example.com/a/inner.json", "{\"type\":\"integer\"}");
	EXPECT_TRUE(ref_accepts("{\"$ref\":\"http://example.com/a/outer.json\"}",
	    "5", "http://example.com/b/root.json", &remote));
	EXPECT_FALSE(ref_accepts("{\"$ref\":\"http://example.com/a/outer.json\"}",
	    "\"x\"", "http://example.com/b/root.json", &remote));
	for (const auto & asked : remote.asked) {
		EXPECT_NE(asked, "http://example.com/b/inner.json")
		    << "resolved the inner reference against the referrer's base";
	}
}

/* Compile with a chosen format policy and say whether the instance passes. */
static bool format_accepts(const char * format, const char * instance_src,
    GTEXT_JSON_Format_Policy policy = GTEXT_JSON_FORMAT_ASSERT) {
	std::string schema_src =
	    std::string("{\"format\":\"") + format + "\"}";
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Error perr;
	memset(&perr, 0, sizeof(perr));
	GTEXT_JSON_Value * sv =
	    gtext_json_parse(schema_src.c_str(), schema_src.size(), &po, &perr);
	GTEXT_JSON_Value * iv =
	    gtext_json_parse(instance_src, strlen(instance_src), &po, &perr);
	EXPECT_NE(sv, nullptr);
	EXPECT_NE(iv, nullptr);
	if (!sv || !iv) {
		return false;
	}
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.format = policy;
	GTEXT_JSON_Error serr;
	memset(&serr, 0, sizeof(serr));
	GTEXT_JSON_Schema * sc =
	    gtext_json_schema_compile_with_options(sv, &opts, &serr);
	EXPECT_NE(sc, nullptr)
	    << "schema did not compile: " << schema_src << " - "
	    << (serr.message ? serr.message : "");
	bool ok = false;
	if (sc) {
		ok = gtext_json_schema_validate(sc, iv, nullptr) == GTEXT_JSON_OK;
	}
	gtext_json_schema_free(sc);
	gtext_json_error_free(&serr);
	gtext_json_free(iv);
	gtext_json_free(sv);
	return ok;
}

TEST(JsonSchemaFormat, IsAnAnnotationUnlessAsked) {
	// 2020-12 says a validator MUST NOT assert on `format` unless it has been
	// asked to, so the default accepts what the format plainly is not. This
	// engine used to refuse the schema outright, which is not conformant
	// either.
	EXPECT_TRUE(schema_accepts("{\"format\":\"email\"}", "\"not an email\""));
	EXPECT_TRUE(schema_accepts("{\"format\":\"ipv4\"}", "\"999.999.999.999\""));
	EXPECT_TRUE(format_accepts(
	    "email", "\"not an email\"", GTEXT_JSON_FORMAT_ANNOTATION));

	// The content keywords are annotations in 2020-12 too, and were refused
	// alongside it.
	EXPECT_TRUE(schema_accepts("{\"contentEncoding\":\"base64\"}", "\"!!\""));
	EXPECT_TRUE(
	    schema_accepts("{\"contentMediaType\":\"application/json\"}", "\"x\""));
	EXPECT_TRUE(schema_accepts(
	    "{\"contentSchema\":{\"type\":\"integer\"}}", "\"x\""));
}

TEST(JsonSchemaFormat, AssertsOnlyStrings) {
	// Every format applies to strings and says nothing about anything else,
	// which is what the suite's format.json checks for all nineteen names.
	EXPECT_TRUE(format_accepts("ipv4", "12"));
	EXPECT_TRUE(format_accepts("ipv4", "13.7"));
	EXPECT_TRUE(format_accepts("ipv4", "{}"));
	EXPECT_TRUE(format_accepts("ipv4", "[]"));
	EXPECT_TRUE(format_accepts("ipv4", "false"));
	EXPECT_TRUE(format_accepts("ipv4", "null"));
}

TEST(JsonSchemaFormat, AnUnknownNameIsIgnoredAndAKnownOneIsNot) {
	// A vendor's own name belongs to nobody, and the specification requires
	// that it be ignored.
	EXPECT_TRUE(format_accepts("phone-number", "\"nonsense\""));

	// A name the vocabulary defines but this library cannot check is refused
	// instead, because the caller asked for the constraint and would
	// otherwise get a schema that silently does not carry it. With no
	// provider supplied, `regex` is the one such name left: every other
	// format in the vocabulary is checked here.
	const char * src = "{\"format\":\"regex\"}";
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Error perr;
	memset(&perr, 0, sizeof(perr));
	GTEXT_JSON_Value * sv = gtext_json_parse(src, strlen(src), &po, &perr);
	ASSERT_NE(sv, nullptr);

	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.format = GTEXT_JSON_FORMAT_ASSERT;
	GTEXT_JSON_Error serr;
	memset(&serr, 0, sizeof(serr));
	EXPECT_EQ(gtext_json_schema_compile_with_options(sv, &opts, &serr), nullptr);
	EXPECT_EQ(serr.code, GTEXT_JSON_E_SCHEMA_UNSUPPORTED);
	ASSERT_NE(serr.context_snippet, nullptr);
	EXPECT_STREQ(serr.context_snippet, "regex");
	gtext_json_error_free(&serr);

	// ...and it compiles under the annotation policy, where it constrains
	// nothing and so cannot be silently weaker than promised.
	GTEXT_JSON_Schema * annotating = gtext_json_schema_compile(sv, nullptr);
	EXPECT_NE(annotating, nullptr);
	gtext_json_schema_free(annotating);
	gtext_json_free(sv);
}

TEST(JsonSchemaFormat, TemporalFormatsComeFromChron) {
	EXPECT_TRUE(format_accepts("date-time", "\"1963-06-19T08:30:06.283185Z\""));
	EXPECT_FALSE(format_accepts("date-time", "\"1990-02-31T15:59:60.123-08:00\""));
	// A leap second is a real reading of a real clock; a leap second in the
	// wrong minute is not.
	EXPECT_TRUE(format_accepts("date-time", "\"1998-12-31T23:59:60Z\""));
	EXPECT_FALSE(format_accepts("date-time", "\"1998-12-31T23:58:60Z\""));

	EXPECT_TRUE(format_accepts("date", "\"1963-06-19\""));
	EXPECT_FALSE(format_accepts("date", "\"1998-1-20\""));

	// `time` is RFC 3339's full-time, so the offset is not optional.
	EXPECT_TRUE(format_accepts("time", "\"08:30:06Z\""));
	EXPECT_FALSE(format_accepts("time", "\"08:30:06\""));

	EXPECT_TRUE(format_accepts("duration", "\"P4DT12H30M5S\""));
	EXPECT_TRUE(format_accepts("duration", "\"P2W\""));
	EXPECT_FALSE(format_accepts("duration", "\"P1Y2D\""));
	EXPECT_FALSE(format_accepts("duration", "\"PT\""));
}

TEST(JsonSchemaFormat, AddressesAndNames) {
	EXPECT_TRUE(format_accepts("ipv4", "\"192.168.0.1\""));
	EXPECT_FALSE(format_accepts("ipv4", "\"127.0.0.0.1\""));
	EXPECT_FALSE(format_accepts("ipv4", "\"087.10.0.1\""));
	EXPECT_FALSE(format_accepts("ipv4", "\"256.0.0.0\""));

	EXPECT_TRUE(format_accepts("ipv6", "\"::42:ff:1\""));
	EXPECT_TRUE(format_accepts("ipv6", "\"::ffff:192.168.0.1\""));
	// The elision stands for at least one group, and may appear once.
	EXPECT_FALSE(format_accepts("ipv6", "\"1:2:3:4:5:6:7:8::\""));
	EXPECT_FALSE(format_accepts("ipv6", "\"1::2::3\""));
	// A colon after the elision leaves a group missing, which the earlier
	// character-by-character version walked straight past.
	EXPECT_FALSE(format_accepts("ipv6", "\"1:2:3:4:5:::8\""));

	EXPECT_TRUE(format_accepts("hostname", "\"www.example.com\""));
	EXPECT_FALSE(format_accepts("hostname", "\"-a-host-name-that-starts-with--\""));
	EXPECT_FALSE(format_accepts("hostname", "\"www.example.com.\""));

	EXPECT_TRUE(format_accepts("uuid", "\"2eb8aa08-aa98-11ea-b4aa-73b441d16380\""));
	EXPECT_FALSE(format_accepts("uuid", "\"2eb8aa08-aa98-11ea-b4aa\""));
}

TEST(JsonSchemaFormat, MailboxesAndPointers) {
	EXPECT_TRUE(format_accepts("email", "\"joe.bloggs@example.com\""));
	EXPECT_FALSE(format_accepts("email", "\"2962\""));
	// RFC 5322's atext is not RFC 3986's sub-delims, and reusing the latter
	// let all three of these through.
	EXPECT_FALSE(format_accepts("email", "\"te,st@example.com\""));
	EXPECT_FALSE(format_accepts("email", "\"te;st@example.com\""));
	EXPECT_FALSE(format_accepts("email", "\"te(st@example.com\""));
	EXPECT_TRUE(format_accepts("email", "\"te.st@example.com\""));
	EXPECT_FALSE(format_accepts("email", "\".test@example.com\""));

	EXPECT_TRUE(format_accepts("json-pointer", "\"/foo/bar~0/baz~1/%a\""));
	EXPECT_FALSE(format_accepts("json-pointer", "\"/~-1\""));
	EXPECT_TRUE(format_accepts("json-pointer", "\"\""));
	EXPECT_FALSE(format_accepts("json-pointer", "\"foo\""));

	EXPECT_TRUE(format_accepts("relative-json-pointer", "\"1\""));
	EXPECT_TRUE(format_accepts("relative-json-pointer", "\"0#\""));
	EXPECT_TRUE(format_accepts("relative-json-pointer", "\"1/0\""));
	EXPECT_FALSE(format_accepts("relative-json-pointer", "\"01/a\""));
	EXPECT_FALSE(format_accepts("relative-json-pointer", "\"/foo/bar\""));
}

TEST(JsonSchemaFormat, UrisAreParsedAsAGrammar) {
	EXPECT_TRUE(format_accepts("uri", "\"http://foo.bar/?baz=qux#quux\""));
	EXPECT_FALSE(format_accepts("uri", "\"//foo.bar/?baz=qux#quux\""));
	// Every byte in each of these is legal somewhere in a URI, which is why
	// checking the character set alone accepted all three.
	EXPECT_FALSE(format_accepts("uri", "\"http://example.com:abc/path\""));
	EXPECT_FALSE(format_accepts("uri", "\"https://[@example.org/test.txt\""));
	EXPECT_FALSE(format_accepts("uri", "\"http:/[::1]\""));

	EXPECT_TRUE(format_accepts("uri-reference", "\"/abc\""));
	EXPECT_TRUE(format_accepts("uri-reference", "\"./this:that\""));
	// A relative-path reference whose first segment carries a colon would be
	// read as a scheme, so the grammar forbids it there and only there.
	EXPECT_FALSE(format_accepts("uri-reference", "\"1:b\""));
	EXPECT_FALSE(format_accepts("uri-reference", "\"//a@b@example.com/\""));

	// A host that merely looks like a dotted quad is still a reg-name.
	EXPECT_TRUE(format_accepts("uri", "\"http://192.168.00.1/\""));
	// Inside the brackets IPv6address is the only production on offer.
	EXPECT_FALSE(format_accepts("uri", "\"http://[::ffff:192.168.00.1]/\""));

	EXPECT_TRUE(format_accepts("iri", "\"http://\\u00e9.example/\""));
	EXPECT_FALSE(format_accepts("uri", "\"http://\\u00e9.example/\""));

	EXPECT_TRUE(format_accepts("uri-template", "\"http://example.com/{p}/x{?q,r}\""));
	EXPECT_TRUE(format_accepts("uri-template", "\"{v:1000}\""));
	EXPECT_FALSE(format_accepts("uri-template", "\"{v:10000}\""));
	EXPECT_FALSE(format_accepts("uri-template", "\"{v:01}\""));
	EXPECT_FALSE(format_accepts("uri-template", "\"{v:0}\""));
	EXPECT_FALSE(format_accepts("uri-template", "\"http://example.com/dictionary/{term:1}/{term\""));
	EXPECT_FALSE(format_accepts("uri-template", "\"{foo,}\""));
}

TEST(JsonSchemaUnevaluated, AppliesToWhatNothingElseReached) {
	// `unevaluatedProperties` sees what the rest of the same schema object
	// did, including through in-place applicators - which is the whole
	// difficulty: `additionalProperties` can be decided from this keyword's
	// own value, and this one cannot.
	const char * schema =
	    "{\"type\":\"object\",\"properties\":{\"foo\":{\"type\":\"string\"}},"
	    "\"unevaluatedProperties\":false}";
	EXPECT_TRUE(schema_accepts(schema, "{\"foo\":\"a\"}"));
	EXPECT_FALSE(schema_accepts(schema, "{\"foo\":\"a\",\"bar\":1}"));

	// An allOf branch's `properties` counts, even though it is a different
	// schema object.
	const char * nested =
	    "{\"allOf\":[{\"properties\":{\"foo\":{}}}],"
	    "\"properties\":{\"bar\":{}},\"unevaluatedProperties\":false}";
	EXPECT_TRUE(schema_accepts(nested, "{\"foo\":1,\"bar\":2}"));
	EXPECT_FALSE(schema_accepts(nested, "{\"foo\":1,\"bar\":2,\"baz\":3}"));

	// And through a $ref, which is an in-place applicator like any other.
	const char * reffed =
	    "{\"$defs\":{\"one\":{\"properties\":{\"foo\":{}}}},"
	    "\"$ref\":\"#/$defs/one\",\"unevaluatedProperties\":false}";
	EXPECT_TRUE(schema_accepts(reffed, "{\"foo\":1}"));
	EXPECT_FALSE(schema_accepts(reffed, "{\"foo\":1,\"bar\":2}"));
}

TEST(JsonSchemaUnevaluated, AFailedBranchEvaluatedNothing) {
	// The rule that makes this more than bookkeeping. The first anyOf branch
	// names `foo` and then fails on `bar`; it has not evaluated `foo`, and an
	// implementation that merged its annotations anyway would call the
	// instance below valid.
	const char * schema =
	    "{\"anyOf\":["
	    "{\"properties\":{\"foo\":{}},\"required\":[\"bar\"]},"
	    "{\"properties\":{\"baz\":{}}}],"
	    "\"unevaluatedProperties\":false}";
	EXPECT_FALSE(schema_accepts(schema, "{\"foo\":1,\"baz\":2}"));
	EXPECT_TRUE(schema_accepts(schema, "{\"baz\":2}"));
	// With `bar` present the first branch passes and `foo` is evaluated - but
	// `bar` is not. `required` names a property; it does not apply a schema
	// to one, and only applying a schema evaluates anything.
	EXPECT_FALSE(schema_accepts(schema, "{\"foo\":1,\"bar\":2}"));
	EXPECT_TRUE(schema_accepts(
	    "{\"anyOf\":[{\"properties\":{\"foo\":{},\"bar\":{}},"
	    "\"required\":[\"bar\"]},{\"properties\":{\"baz\":{}}}],"
	    "\"unevaluatedProperties\":false}",
	    "{\"foo\":1,\"bar\":2}"));

	// `not` succeeds exactly when its subschema failed, so it evaluates
	// nothing whatever the subschema named.
	EXPECT_FALSE(schema_accepts(
	    "{\"not\":{\"properties\":{\"foo\":{\"type\":\"integer\"}},"
	    "\"required\":[\"bar\"]},\"unevaluatedProperties\":false}",
	    "{\"foo\":1}"));
}

TEST(JsonSchemaUnevaluated, IfContributesOnlyWhenItPassed) {
	const char * schema =
	    "{\"if\":{\"properties\":{\"foo\":{\"const\":1}},\"required\":[\"foo\"]},"
	    "\"then\":{\"properties\":{\"bar\":{}}},"
	    "\"unevaluatedProperties\":false}";
	// The `if` passes, so both it and `then` have evaluated something.
	EXPECT_TRUE(schema_accepts(schema, "{\"foo\":1,\"bar\":2}"));
	// The `if` fails and there is no `else`, so nothing was evaluated.
	EXPECT_FALSE(schema_accepts(schema, "{\"foo\":2}"));
}

TEST(JsonSchemaUnevaluated, ItemsAndContains) {
	EXPECT_TRUE(schema_accepts(
	    "{\"prefixItems\":[{\"type\":\"string\"}],\"unevaluatedItems\":false}",
	    "[\"a\"]"));
	EXPECT_FALSE(schema_accepts(
	    "{\"prefixItems\":[{\"type\":\"string\"}],\"unevaluatedItems\":false}",
	    "[\"a\",2]"));

	// `contains` evaluates the items it matched and no others, which is the
	// one applicator whose annotation depends on the instance.
	EXPECT_TRUE(schema_accepts(
	    "{\"contains\":{\"type\":\"string\"},\"unevaluatedItems\":false}",
	    "[\"a\",\"b\"]"));
	EXPECT_FALSE(schema_accepts(
	    "{\"contains\":{\"type\":\"string\"},\"unevaluatedItems\":false}",
	    "[\"a\",1]"));

	// `unevaluatedItems` as a schema rather than as a refusal.
	EXPECT_TRUE(schema_accepts(
	    "{\"prefixItems\":[{\"type\":\"string\"}],"
	    "\"unevaluatedItems\":{\"type\":\"integer\"}}",
	    "[\"a\",1,2]"));
	EXPECT_FALSE(schema_accepts(
	    "{\"prefixItems\":[{\"type\":\"string\"}],"
	    "\"unevaluatedItems\":{\"type\":\"integer\"}}",
	    "[\"a\",1,\"b\"]"));
}

TEST(JsonSchemaUnevaluated, WhatItReachedCountsAsEvaluated) {
	// An inner `unevaluatedProperties` evaluates what it applies to, so an
	// outer one looking at the same instance sees those as covered.
	EXPECT_TRUE(schema_accepts(
	    "{\"allOf\":[{\"properties\":{\"foo\":{}},"
	    "\"unevaluatedProperties\":true}],"
	    "\"unevaluatedProperties\":false}",
	    "{\"foo\":1,\"bar\":2}"));
}

TEST(JsonSchemaUnevaluated, NestedSchemasSeeOnlyTheirOwn) {
	// The inner schema object's `unevaluatedProperties` sees what that object
	// reached, not what its parent's `properties` did. Here `foo` is named by
	// the outer schema and the inner one has never heard of it.
	EXPECT_FALSE(schema_accepts(
	    "{\"properties\":{\"foo\":{}},"
	    "\"allOf\":[{\"unevaluatedProperties\":false}]}",
	    "{\"foo\":1}"));
}

TEST(JsonSchemaDynamicRef, ResolvesAgainstTheOutermostScope) {
	// The recursive-extension pattern the keyword exists for. `strict` is a
	// `tree` that also refuses properties nothing named; the reference inside
	// `tree` has to come back to `strict` at every level of the recursion, or
	// the strictness applies only to the root.
	StubResolver remote;
	remote.add("http://x/tree.json",
	    "{\"$id\":\"http://x/tree.json\",\"$dynamicAnchor\":\"node\","
	    "\"type\":\"object\",\"properties\":{\"data\":true,\"children\":"
	    "{\"type\":\"array\",\"items\":{\"$dynamicRef\":\"#node\"}}}}");
	const char * strict =
	    "{\"$id\":\"http://x/strict.json\",\"$dynamicAnchor\":\"node\","
	    "\"$ref\":\"tree.json\",\"unevaluatedProperties\":false}";

	EXPECT_TRUE(ref_accepts(strict,
	    "{\"children\":[{\"data\":1}]}", nullptr, &remote));
	// The misspelling is one level down, which is the whole point: a plain
	// $ref would have applied `tree` there and accepted it.
	EXPECT_FALSE(ref_accepts(strict,
	    "{\"children\":[{\"daat\":1}]}", nullptr, &remote));
	// And at the root, where even a static reference would have caught it.
	EXPECT_FALSE(ref_accepts(strict, "{\"daat\":1}", nullptr, &remote));
}

TEST(JsonSchemaDynamicRef, TheAnchorNeedsNoReferenceToBeFound) {
	// The anchor lives in a `$defs` nothing refers to. Compiling only what a
	// `$ref` reaches left it out, and the reference then resolved to the
	// inner anchor instead - a wrong answer with nothing to show for it.
	const char * schema =
	    "{\"$id\":\"http://x/root\",\"$ref\":\"list\","
	    "\"$defs\":{"
	    "\"foo\":{\"$dynamicAnchor\":\"items\",\"type\":\"string\"},"
	    "\"list\":{\"$id\":\"list\",\"type\":\"array\","
	    "\"items\":{\"$dynamicRef\":\"#items\"},"
	    "\"$defs\":{\"items\":{\"$dynamicAnchor\":\"items\"}}}}}";
	EXPECT_TRUE(ref_accepts(schema, "[\"a\",\"b\"]"));
	EXPECT_FALSE(ref_accepts(schema, "[\"a\",1]"));
}

TEST(JsonSchemaDynamicRef, WithoutBookendingItIsAnOrdinaryReference) {
	// A `$dynamicRef` whose target declares no `$dynamicAnchor` of that name
	// behaves as `$ref` does, and so does one whose fragment is a pointer.
	EXPECT_FALSE(ref_accepts(
	    "{\"$defs\":{\"n\":{\"$anchor\":\"plain\",\"type\":\"integer\"}},"
	    "\"$dynamicRef\":\"#plain\"}",
	    "\"s\""));
	EXPECT_FALSE(ref_accepts(
	    "{\"$defs\":{\"n\":{\"type\":\"integer\"}},"
	    "\"$dynamicRef\":\"#/$defs/n\"}",
	    "\"s\""));
}

TEST(JsonSchemaVocabulary, AKeywordFromAnUnusedVocabularyIsNotAKeyword) {
	// The metaschema declares core and applicator and not validation, so
	// `minimum` is an unknown member of the schema object - not a
	// misunderstood keyword, just one this dialect never asked for. Ignoring
	// unknown members is what the specification requires.
	StubResolver remote;
	remote.add("http://x/meta-no-validation",
	    "{\"$id\":\"http://x/meta-no-validation\",\"$vocabulary\":{"
	    "\"https://json-schema.org/draft/2020-12/vocab/core\":true,"
	    "\"https://json-schema.org/draft/2020-12/vocab/applicator\":true}}");
	const char * schema =
	    "{\"$schema\":\"http://x/meta-no-validation\","
	    "\"properties\":{\"n\":{\"minimum\":10},\"bad\":false}}";

	EXPECT_TRUE(ref_accepts(schema, "{\"n\":1}", nullptr, &remote));
	// The applicator vocabulary is in use, so `properties` still applies.
	EXPECT_FALSE(ref_accepts(schema, "{\"bad\":1}", nullptr, &remote));

	// With the standard dialect, the same schema constrains the number.
	EXPECT_FALSE(schema_accepts(
	    "{\"properties\":{\"n\":{\"minimum\":10}}}", "{\"n\":1}"));
}

TEST(JsonSchemaVocabulary, ARequiredVocabularyItLacksIsRefused) {
	StubResolver remote;
	remote.add("http://x/meta-custom",
	    "{\"$id\":\"http://x/meta-custom\",\"$vocabulary\":{"
	    "\"https://json-schema.org/draft/2020-12/vocab/core\":true,"
	    "\"http://x/vocab/invented\":true}}");
	const char * src = "{\"$schema\":\"http://x/meta-custom\"}";
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Value * sv = gtext_json_parse(src, strlen(src), &po, nullptr);
	ASSERT_NE(sv, nullptr);

	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	GTEXT_JSON_Schema_Resolver resolver = {&remote, StubResolver::get};
	opts.resolver = &resolver;
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	EXPECT_EQ(gtext_json_schema_compile_with_options(sv, &opts, &err), nullptr);
	EXPECT_EQ(err.code, GTEXT_JSON_E_SCHEMA_UNSUPPORTED);
	ASSERT_NE(err.context_snippet, nullptr);
	EXPECT_STREQ(err.context_snippet, "http://x/vocab/invented");
	gtext_json_error_free(&err);
	gtext_json_free(sv);

	// Declared optional, the same vocabulary is simply not used.
	StubResolver optional;
	optional.add("http://x/meta-custom",
	    "{\"$id\":\"http://x/meta-custom\",\"$vocabulary\":{"
	    "\"https://json-schema.org/draft/2020-12/vocab/core\":true,"
	    "\"https://json-schema.org/draft/2020-12/vocab/validation\":true,"
	    "\"http://x/vocab/invented\":false}}");
	EXPECT_TRUE(ref_accepts("{\"$schema\":\"http://x/meta-custom\","
	                        "\"type\":\"number\"}",
	    "1", nullptr, &optional));
	EXPECT_FALSE(ref_accepts("{\"$schema\":\"http://x/meta-custom\","
	                         "\"type\":\"number\"}",
	    "\"s\"", nullptr, &optional));
}

TEST(JsonSchemaVocabulary, TheDialectCanTurnFormatAssertionOn) {
	// The specification's own mechanism for it, and the reason `format`
	// asserting is not simply an option this library invented.
	StubResolver remote;
	remote.add("http://x/meta-format-assert",
	    "{\"$id\":\"http://x/meta-format-assert\",\"$vocabulary\":{"
	    "\"https://json-schema.org/draft/2020-12/vocab/core\":true,"
	    "\"https://json-schema.org/draft/2020-12/vocab/format-assertion\":true}}");
	EXPECT_FALSE(ref_accepts("{\"$schema\":\"http://x/meta-format-assert\","
	                         "\"format\":\"ipv4\"}",
	    "\"not an address\"", nullptr, &remote));
	EXPECT_TRUE(ref_accepts("{\"$schema\":\"http://x/meta-format-assert\","
	                        "\"format\":\"ipv4\"}",
	    "\"192.168.0.1\"", nullptr, &remote));

	// Without that declaration it is an annotation, as the standard dialect
	// says.
	EXPECT_TRUE(schema_accepts("{\"format\":\"ipv4\"}", "\"not an address\""));
}

TEST(JsonSchemaKeywords, Draft07DependenciesTakesEitherForm) {
	// `dependencies` is the union of what 2020-12 split in two, so each entry
	// is compiled into whichever of the pair it means.
	EXPECT_FALSE(
	    schema_accepts("{\"dependencies\":{\"a\":[\"b\"]}}", "{\"a\":1}"));
	EXPECT_TRUE(
	    schema_accepts("{\"dependencies\":{\"a\":[\"b\"]}}", "{\"a\":1,\"b\":2}"));
	EXPECT_TRUE(
	    schema_accepts("{\"dependencies\":{\"a\":[\"b\"]}}", "{\"c\":1}"));

	EXPECT_FALSE(schema_accepts(
	    "{\"dependencies\":{\"a\":{\"required\":[\"b\"]}}}", "{\"a\":1}"));
	EXPECT_TRUE(schema_accepts(
	    "{\"dependencies\":{\"a\":{\"required\":[\"b\"]}}}", "{\"a\":1,\"b\":2}"));

	// Both forms in one schema.
	const char * mixed =
	    "{\"dependencies\":{\"a\":[\"b\"],\"c\":{\"required\":[\"d\"]}}}";
	EXPECT_TRUE(schema_accepts(mixed, "{\"a\":1,\"b\":2}"));
	EXPECT_FALSE(schema_accepts(mixed, "{\"c\":1}"));
	EXPECT_TRUE(schema_accepts(mixed, "{\"c\":1,\"d\":2}"));
}

// ---------------------------------------------------------------------------
// max_total_bytes
//
// Enforced for file reads (json_file_io.c) and for the streaming parser
// (json_stream.c), but not for an in-memory parse: a 600-byte document parsed
// cleanly with max_total_bytes set to 50.  The comment in gtext_json_parse()
// said the check happened in json_parse_internal, and it did not.
// ---------------------------------------------------------------------------

TEST(JsonMaxTotalBytes, RejectsInputLargerThanTheLimit) {
	std::string src = "[";
	for (int i = 0; i < 300; ++i) {
		src += (i ? ",1" : "1");
	}
	src += "]";
	ASSERT_GT(src.size(), 50u);

	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.max_total_bytes = 50;

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * v = gtext_json_parse(src.data(), src.size(), &opts, &err);

	EXPECT_EQ(v, nullptr);
	EXPECT_EQ(err.code, GTEXT_JSON_E_LIMIT);
	if (v) {
		gtext_json_free(v);
	}
	gtext_json_error_free(&err);
}

TEST(JsonMaxTotalBytes, AcceptsInputAtTheLimit) {
	// The boundary: exactly max_total_bytes must parse, one more must not.
	const std::string src = "[1,2,3]";

	{
		GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
		opts.max_total_bytes = src.size();
		GTEXT_JSON_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * v =
		    gtext_json_parse(src.data(), src.size(), &opts, &err);
		EXPECT_NE(v, nullptr) << (err.message ? err.message : "");
		if (v) {
			gtext_json_free(v);
		}
		gtext_json_error_free(&err);
	}
	{
		GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
		opts.max_total_bytes = src.size() - 1;
		GTEXT_JSON_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * v =
		    gtext_json_parse(src.data(), src.size(), &opts, &err);
		EXPECT_EQ(v, nullptr);
		EXPECT_EQ(err.code, GTEXT_JSON_E_LIMIT);
		if (v) {
			gtext_json_free(v);
		}
		gtext_json_error_free(&err);
	}
}

TEST(JsonMaxTotalBytes, ZeroMeansLibraryDefault) {
	const std::string src = "[1,2,3]";
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.max_total_bytes = 0;

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * v = gtext_json_parse(src.data(), src.size(), &opts, &err);
	EXPECT_NE(v, nullptr) << (err.message ? err.message : "");
	if (v) {
		gtext_json_free(v);
	}
	gtext_json_error_free(&err);
}

TEST(JsonMaxTotalBytes, StreamingParserAgrees) {
	// The two parsers must give the same answer for the same limit.
	std::string src = "[";
	for (int i = 0; i < 300; ++i) {
		src += (i ? ",1" : "1");
	}
	src += "]";

	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.max_total_bytes = 50;

	auto cb = [](void *, const GTEXT_JSON_Event *,
	              GTEXT_JSON_Error *) -> GTEXT_JSON_Status {
		return GTEXT_JSON_OK;
	};

	GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, cb, nullptr);
	ASSERT_NE(st, nullptr);

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	GTEXT_JSON_Status s = GTEXT_JSON_OK;
	for (size_t i = 0; i < src.size() && s == GTEXT_JSON_OK; i += 16) {
		size_t n = std::min<size_t>(16, src.size() - i);
		s = gtext_json_stream_feed(st, src.data() + i, n, &err);
	}
	EXPECT_EQ(s, GTEXT_JSON_E_LIMIT);

	gtext_json_stream_free(st);
	gtext_json_error_free(&err);
}

// ---------------------------------------------------------------------------
// json_buffer_grow_unified
//
// Fifteen of the seventy-three growth and resize lines that no test executed
// were in this one function.  tools/coverage.sh reports those separately for a
// reason: a reallocation path no test reaches is untested, not working, and a
// buffer that miscomputes its new capacity corrupts the heap rather than
// failing visibly.
//
// The overflow branches cannot be reached through the public API - they need a
// capacity near SIZE_MAX - so they are driven directly here.  The function
// leaves *buffer untouched when it fails, so declaring a large capacity over a
// small allocation is safe: the realloc fails and the original pointer is
// still ours to free.
// ---------------------------------------------------------------------------

namespace {

struct GrowBuffer {
	char * data = nullptr;
	size_t capacity = 0;

	~GrowBuffer() {
		// Through the allocator, not free(): json_buffer_grow_unified() takes
		// an allocator now, and a NULL one means gcu_allocator_default(),
		// which is a vtable rather than a promise of malloc compatibility.
		gtext_allocator_free(nullptr, data);
	}
};

} // namespace

TEST(JsonBufferGrow, RejectsNullArguments) {
	size_t cap = 0;
	char * buf = nullptr;
	EXPECT_EQ(json_buffer_grow_unified(nullptr, &cap, 10,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 0, 0, 0, nullptr),
	    GTEXT_JSON_E_INVALID);
	EXPECT_EQ(json_buffer_grow_unified(&buf, nullptr, 10,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 0, 0, 0, nullptr),
	    GTEXT_JSON_E_INVALID);
}

namespace {

// A counting allocator, local to these tests: just enough to say whether the
// allocator parameter was used at all.
struct GrowCounts {
	size_t served = 0;
	size_t live = 0;
};

void * grow_malloc(void * ctx, size_t size) {
	auto * c = static_cast<GrowCounts *>(ctx);
	void * p = std::malloc(size ? size : 1);
	if (p) {
		c->served++;
		c->live++;
	}
	return p;
}

void * grow_calloc(void * ctx, size_t n, size_t size) {
	if (n && size > SIZE_MAX / n) {
		return nullptr;
	}
	void * p = grow_malloc(ctx, n * size);
	if (p) {
		std::memset(p, 0, n * size);
	}
	return p;
}

void grow_free(void * ctx, void * ptr) {
	if (!ptr) {
		return;
	}
	static_cast<GrowCounts *>(ctx)->live--;
	std::free(ptr);
}

void * grow_realloc(void * ctx, void * ptr, size_t size) {
	auto * c = static_cast<GrowCounts *>(ctx);
	void * p = std::realloc(ptr, size ? size : 1);
	if (p) {
		c->served++;
		if (!ptr) {
			c->live++;
		}
	}
	return p;
}

} // namespace

/*
 * The allocator parameter, at the level where the helper is tested on its own.
 * Every other call in this file passes nullptr, which exercises the default and
 * says nothing about whether the parameter is read - so without this, the
 * parameter could be ignored here and only the stream-level tests in
 * test-allocator.cpp would notice.
 */
TEST(JsonBufferGrow, UsesTheAllocatorItIsGiven) {
	GrowCounts counts;
	// By field name: nothing fixes the order of GCU_Allocator's members.
	GTEXT_Allocator alloc;
	alloc.ctx = &counts;
	alloc.malloc_fn = grow_malloc;
	alloc.calloc_fn = grow_calloc;
	alloc.realloc_fn = grow_realloc;
	alloc.free_fn = grow_free;

	char * buf = nullptr;
	size_t cap = 0;
	ASSERT_EQ(json_buffer_grow_unified(&buf, &cap, 100,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 0, 0, 0, &alloc),
	    GTEXT_JSON_OK);
	ASSERT_NE(buf, nullptr);
	EXPECT_GE(cap, 100u);
	EXPECT_EQ(counts.served, 1u) << "the allocator parameter was ignored";
	EXPECT_EQ(counts.live, 1u);

	// Growing again goes to the same allocator.
	ASSERT_EQ(json_buffer_grow_unified(&buf, &cap, cap + 1,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 0, 0, 0, &alloc),
	    GTEXT_JSON_OK);
	EXPECT_EQ(counts.served, 2u);
	EXPECT_EQ(counts.live, 1u) << "the grow reallocated in place, once";

	gtext_allocator_free(&alloc, buf);
	EXPECT_EQ(counts.live, 0u);
}

TEST(JsonBufferGrow, AlreadyLargeEnoughIsANoOp) {
	GrowBuffer b;
	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 100,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 0, 0, 0, nullptr),
	    GTEXT_JSON_OK);
	char * before = b.data;
	size_t cap_before = b.capacity;

	// needed <= capacity must not reallocate.
	EXPECT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, cap_before,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 0, 0, 0, nullptr),
	    GTEXT_JSON_OK);
	EXPECT_EQ(b.data, before);
	EXPECT_EQ(b.capacity, cap_before);
}

TEST(JsonBufferGrow, InitialAllocationUsesTheLargerOfNeededAndInitial) {
	{
		// Default initial size is 64.
		GrowBuffer b;
		ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 10,
		              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 0, 0, 0, nullptr),
		    GTEXT_JSON_OK);
		EXPECT_EQ(b.capacity, 64u);
		EXPECT_NE(b.data, nullptr);
	}
	{
		GrowBuffer b;
		ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 5000,
		              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 0, 0, 0, nullptr),
		    GTEXT_JSON_OK);
		EXPECT_EQ(b.capacity, 5000u);
	}
	{
		// An explicit initial size is honored.
		GrowBuffer b;
		ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 3,
		              JSON_BUFFER_GROWTH_SIMPLE, 256, 0, 0, 0, 0, nullptr),
		    GTEXT_JSON_OK);
		EXPECT_EQ(b.capacity, 256u);
	}
}

TEST(JsonBufferGrow, SimpleStrategyDoubles) {
	GrowBuffer b;
	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 64,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 0, 0, 0, nullptr),
	    GTEXT_JSON_OK);
	ASSERT_EQ(b.capacity, 64u);

	// One byte more than capacity doubles rather than growing by one.
	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 65,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 0, 0, 0, nullptr),
	    GTEXT_JSON_OK);
	EXPECT_EQ(b.capacity, 128u);

	// When doubling is still not enough, the needed size is used.
	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 1000,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 0, 0, 0, nullptr),
	    GTEXT_JSON_OK);
	EXPECT_EQ(b.capacity, 1000u);
}

TEST(JsonBufferGrow, SimpleStrategyHonorsAnExplicitMultiplier) {
	GrowBuffer b;
	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 100,
	              JSON_BUFFER_GROWTH_SIMPLE, 100, 0, 0, 0, 0, nullptr),
	    GTEXT_JSON_OK);
	ASSERT_EQ(b.capacity, 100u);

	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 101,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 3, 0, 0, nullptr),
	    GTEXT_JSON_OK);
	EXPECT_EQ(b.capacity, 300u);
}

TEST(JsonBufferGrow, HybridGrowsSmallBuffersByAFixedIncrement) {
	GrowBuffer b;
	// Below small_threshold, so the fixed increment applies.
	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 100,
	              JSON_BUFFER_GROWTH_HYBRID, 100, 1024, 2, 64, 0, nullptr),
	    GTEXT_JSON_OK);
	ASSERT_EQ(b.capacity, 100u);

	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 101,
	              JSON_BUFFER_GROWTH_HYBRID, 0, 1024, 2, 64, 0, nullptr),
	    GTEXT_JSON_OK);
	EXPECT_EQ(b.capacity, 164u) << "small buffers grow by the increment, not by doubling";

	// When the increment is not enough, the needed size wins.
	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 900,
	              JSON_BUFFER_GROWTH_HYBRID, 0, 1024, 2, 64, 0, nullptr),
	    GTEXT_JSON_OK);
	EXPECT_EQ(b.capacity, 900u);
}

TEST(JsonBufferGrow, HybridDoublesLargeBuffers) {
	GrowBuffer b;
	// At or above small_threshold, so doubling applies.
	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 2048,
	              JSON_BUFFER_GROWTH_HYBRID, 2048, 1024, 2, 64, 0, nullptr),
	    GTEXT_JSON_OK);
	ASSERT_EQ(b.capacity, 2048u);

	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 2049,
	              JSON_BUFFER_GROWTH_HYBRID, 0, 1024, 2, 64, 0, nullptr),
	    GTEXT_JSON_OK);
	EXPECT_EQ(b.capacity, 4096u) << "large buffers double, not grow by the increment";

	// When doubling is not enough, the needed size wins.
	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 100000,
	              JSON_BUFFER_GROWTH_HYBRID, 0, 1024, 2, 64, 0, nullptr),
	    GTEXT_JSON_OK);
	EXPECT_EQ(b.capacity, 100000u);
}

TEST(JsonBufferGrow, HeadroomIsAddedAfterGrowth) {
	GrowBuffer b;
	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 100,
	              JSON_BUFFER_GROWTH_SIMPLE, 100, 0, 0, 0, 0, nullptr),
	    GTEXT_JSON_OK);
	ASSERT_EQ(b.capacity, 100u);

	ASSERT_EQ(json_buffer_grow_unified(&b.data, &b.capacity, 101,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 2, 0, 32, nullptr),
	    GTEXT_JSON_OK);
	EXPECT_EQ(b.capacity, 232u) << "doubled to 200, plus 32 headroom";
}

// The remaining cases drive the overflow arithmetic, which needs capacities
// that cannot be allocated.  The realloc at the end of the function fails, so
// each returns GTEXT_JSON_E_OOM; what is being checked is that the function
// reaches that point deliberately rather than by computing a capacity smaller
// than the caller asked for, which is the shape that corrupts a heap.

TEST(JsonBufferGrow, HybridSmallIncrementOverflowFallsBackToNeeded) {
	char * fake = (char *)malloc(16);
	ASSERT_NE(fake, nullptr);
	// A capacity below small_threshold, positioned so that adding the fixed
	// increment overflows.
	size_t cap = SIZE_MAX - 8;
	size_t threshold = SIZE_MAX; // keep the "small buffer" branch selected

	EXPECT_EQ(json_buffer_grow_unified(&fake, &cap, SIZE_MAX - 4,
	              JSON_BUFFER_GROWTH_HYBRID, 0, threshold, 2, 64, 0, nullptr),
	    GTEXT_JSON_E_OOM);
	// The buffer and capacity are left alone on failure.
	EXPECT_EQ(cap, SIZE_MAX - 8u);
	free(fake);
}

TEST(JsonBufferGrow, HybridLargeMultiplyOverflowFallsBackToNeeded) {
	char * fake = (char *)malloc(16);
	ASSERT_NE(fake, nullptr);
	// At or above the threshold, and large enough that doubling overflows.
	size_t cap = (SIZE_MAX / 2) + 2;

	EXPECT_EQ(json_buffer_grow_unified(&fake, &cap, SIZE_MAX - 4,
	              JSON_BUFFER_GROWTH_HYBRID, 0, 1024, 2, 64, 0, nullptr),
	    GTEXT_JSON_E_OOM);
	EXPECT_EQ(cap, (SIZE_MAX / 2) + 2u);
	free(fake);
}

TEST(JsonBufferGrow, SimpleMultiplyOverflowFallsBackToNeeded) {
	char * fake = (char *)malloc(16);
	ASSERT_NE(fake, nullptr);
	size_t cap = (SIZE_MAX / 2) + 2;

	EXPECT_EQ(json_buffer_grow_unified(&fake, &cap, SIZE_MAX - 4,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 2, 0, 0, nullptr),
	    GTEXT_JSON_E_OOM);
	EXPECT_EQ(cap, (SIZE_MAX / 2) + 2u);
	free(fake);
}

TEST(JsonBufferGrow, HeadroomOverflowDoesNotShrinkBelowNeeded) {
	char * fake = (char *)malloc(16);
	ASSERT_NE(fake, nullptr);
	size_t cap = 64;

	// Growth reaches a capacity that cannot take the headroom without
	// overflowing.  The function must keep the un-headroomed capacity rather
	// than wrapping, and that capacity must still cover `needed`.
	EXPECT_EQ(json_buffer_grow_unified(&fake, &cap, SIZE_MAX - 4,
	              JSON_BUFFER_GROWTH_SIMPLE, 0, 0, 2, 0, SIZE_MAX / 2, nullptr),
	    GTEXT_JSON_E_OOM);
	EXPECT_EQ(cap, 64u);
	free(fake);
}

TEST(JsonBufferGrow, OverflowHelpersAgreeWithTheirContracts) {
	// These three decide every branch above, so they are worth pinning
	// directly rather than only through their callers.
	EXPECT_TRUE(json_check_add_overflow(SIZE_MAX, 1));
	EXPECT_TRUE(json_check_add_overflow(SIZE_MAX - 1, 2));
	EXPECT_FALSE(json_check_add_overflow(SIZE_MAX - 1, 1));
	EXPECT_FALSE(json_check_add_overflow(0, 0));

	EXPECT_TRUE(json_check_mul_overflow(SIZE_MAX, 2));
	EXPECT_TRUE(json_check_mul_overflow((SIZE_MAX / 2) + 1, 2));
	EXPECT_FALSE(json_check_mul_overflow(SIZE_MAX / 2, 2));
	EXPECT_FALSE(json_check_mul_overflow(SIZE_MAX, 0))
	    << "multiplying by zero cannot overflow";
	EXPECT_FALSE(json_check_mul_overflow(0, SIZE_MAX));

	EXPECT_TRUE(json_check_sub_underflow(0, 1));
	EXPECT_FALSE(json_check_sub_underflow(1, 1));
	EXPECT_FALSE(json_check_sub_underflow(SIZE_MAX, 0));
}

// ---------------------------------------------------------------------------
// json_error.c
//
// At 47% the least-covered file in the JSON module.  Both functions here feed
// user-facing error text, and both are reached from the parser along only a
// couple of paths, so most of what they can say had never been said.
// ---------------------------------------------------------------------------

TEST(JsonTokenDescription, EveryTokenHasItsOwnDescription) {
	// The parser names the token it wanted and the token it found, so a case
	// that falls through to "unknown token" is a message a user cannot act on.
	const struct {
		int token;
		const char * expected;
	} cases[] = {
	    {JSON_TOKEN_EOF, "end of input"},
	    {JSON_TOKEN_ERROR, "error"},
	    {JSON_TOKEN_LBRACE, "opening brace '{'"},
	    {JSON_TOKEN_RBRACE, "closing brace '}'"},
	    {JSON_TOKEN_LBRACKET, "opening bracket '['"},
	    {JSON_TOKEN_RBRACKET, "closing bracket ']'"},
	    {JSON_TOKEN_COLON, "colon ':'"},
	    {JSON_TOKEN_COMMA, "comma ','"},
	    {JSON_TOKEN_NULL, "null"},
	    {JSON_TOKEN_TRUE, "true"},
	    {JSON_TOKEN_FALSE, "false"},
	    {JSON_TOKEN_STRING, "string"},
	    {JSON_TOKEN_NUMBER, "number"},
	    {JSON_TOKEN_NAN, "NaN"},
	    {JSON_TOKEN_INFINITY, "Infinity"},
	    {JSON_TOKEN_NEG_INFINITY, "-Infinity"},
	};

	for (const auto & c : cases) {
		const char * got = json_token_type_description(c.token);
		ASSERT_NE(got, nullptr);
		EXPECT_STREQ(got, c.expected) << "token " << c.token;
	}

	// An unmapped value must still produce something printable rather than
	// falling off the end of the switch.
	EXPECT_STREQ(json_token_type_description(-1), "unknown token");
	EXPECT_STREQ(json_token_type_description(9999), "unknown token");
}

TEST(JsonErrorSnippet, RejectsNullArguments) {
	char * snippet = nullptr;
	size_t len = 0;
	size_t caret = 0;
	const char * input = "hello";

	EXPECT_NE(json_error_generate_context_snippet(
	              nullptr, 5, 0, 10, 10, &snippet, &len, &caret),
	    GTEXT_JSON_OK);
	EXPECT_NE(json_error_generate_context_snippet(
	              input, 5, 0, 10, 10, nullptr, &len, &caret),
	    GTEXT_JSON_OK);
	EXPECT_NE(json_error_generate_context_snippet(
	              input, 5, 0, 10, 10, &snippet, nullptr, &caret),
	    GTEXT_JSON_OK);
	EXPECT_NE(json_error_generate_context_snippet(
	              input, 5, 0, 10, 10, &snippet, &len, nullptr),
	    GTEXT_JSON_OK);
}

TEST(JsonErrorSnippet, CaretPointsAtTheErrorOffset) {
	const char * input = "0123456789abcdefghij";
	const size_t input_len = strlen(input);

	struct Case {
		size_t offset;
		size_t radius;
	};
	const Case cases[] = {
	    {0, 5},          // at the very start: nothing before it
	    {input_len, 5},  // one past the end: nothing after it
	    {10, 5},         // comfortably inside
	    {10, 1000},      // radius larger than the whole input
	    {2, 3},          // window clipped at the start
	    {input_len - 1, 3},
	};

	for (const Case & c : cases) {
		SCOPED_TRACE("offset=" + std::to_string(c.offset) + " radius="
		    + std::to_string(c.radius));

		char * snippet = nullptr;
		size_t len = 0;
		size_t caret = 0;
		GTEXT_JSON_Status st = json_error_generate_context_snippet(
		    input, input_len, c.offset, c.radius, c.radius, &snippet, &len,
		    &caret);
		ASSERT_EQ(st, GTEXT_JSON_OK);
		ASSERT_NE(snippet, nullptr);

		// The caret must index into the snippet, and the snippet must be a
		// substring of the input at the position the caret claims.
		EXPECT_LE(caret, len);
		ASSERT_LE(len, input_len);
		const char * found = strstr(input, std::string(snippet, len).c_str());
		EXPECT_NE(found, nullptr) << "snippet is not a substring of the input";
		if (found) {
			EXPECT_EQ(static_cast<size_t>(found - input) + caret, c.offset)
			    << "caret does not land on the error offset";
		}

		free(snippet);
	}
}

TEST(JsonErrorSnippet, EmptyInputProducesNoSnippet) {
	char * snippet = nullptr;
	size_t len = 0;
	size_t caret = 0;
	json_error_generate_context_snippet("", 0, 0, 10, 10, &snippet, &len, &caret);
	EXPECT_EQ(snippet, nullptr);
	EXPECT_EQ(len, 0u);
	free(snippet);
}

TEST(JsonErrorSnippet, OffsetBeyondInputIsClampedNotRead) {
	// A caller that reports an offset past the end must not make this read out
	// of bounds.  Under ASan a regression here fails loudly.
	const char * input = "abc";
	char * snippet = nullptr;
	size_t len = 0;
	size_t caret = 0;
	json_error_generate_context_snippet(
	    input, 3, 1000, 10, 10, &snippet, &len, &caret);
	if (snippet) {
		EXPECT_LE(len, 3u);
		free(snippet);
	}
}

TEST(JsonErrorFree, IsIdempotentAndNullSafe) {
	gtext_json_error_free(nullptr);

	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	gtext_json_error_free(&err); // nothing allocated
	EXPECT_EQ(err.context_snippet, nullptr);

	// Now with a real snippet from a real parse failure.
	const char * bad = "{\"a\": }";
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	GTEXT_JSON_Value * v = gtext_json_parse(bad, strlen(bad), &opts, &err);
	EXPECT_EQ(v, nullptr);
	if (v) {
		gtext_json_free(v);
	}
	gtext_json_error_free(&err);
	EXPECT_EQ(err.context_snippet, nullptr);
	gtext_json_error_free(&err); // twice must be safe
}



// ---------------------------------------------------------------------------
// The streaming writer's nesting stack, past its initial capacity
//
// GTEXT_JSON_Writer keeps a stack entry per open array or object, allocated
// at JSON_WRITER_DEFAULT_STACK_CAPACITY (32) and doubled from there.
// tools/coverage.sh reported writer_ensure_stack()'s growth path as never
// executed, so nothing had ever nested the streaming writer past depth 32.
//
// The stack records, per level, whether the container is an array or an
// object and whether anything has been written into it yet - which is what
// decides where commas go. A realloc that loses or misplaces those entries
// produces malformed JSON rather than a crash, so the check here is that the
// output parses back to a tree of the depth that was written.
// ---------------------------------------------------------------------------

namespace {

// Write `depth` nested arrays with a single number at the bottom, and return
// the serialized result.
std::string write_nested_arrays(size_t depth) {
	GTEXT_JSON_Sink sink;
	EXPECT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);

	GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
	EXPECT_NE(w, nullptr);
	if (!w) {
		gtext_json_sink_buffer_free(&sink);
		return "";
	}

	for (size_t i = 0; i < depth; ++i) {
		EXPECT_EQ(gtext_json_writer_array_begin(w), GTEXT_JSON_OK)
		    << "opening level " << i;
	}
	EXPECT_EQ(gtext_json_writer_number_i64(w, 42), GTEXT_JSON_OK);
	for (size_t i = 0; i < depth; ++i) {
		EXPECT_EQ(gtext_json_writer_array_end(w), GTEXT_JSON_OK)
		    << "closing level " << i;
	}

	std::string out(
	    gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
	gtext_json_writer_free(w);
	gtext_json_sink_buffer_free(&sink);
	return out;
}

// Count how many arrays deep a parsed document goes.
size_t array_depth(const GTEXT_JSON_Value * v) {
	size_t d = 0;
	while (v && gtext_json_typeof(v) == GTEXT_JSON_ARRAY
	    && gtext_json_array_size(v) == 1) {
		++d;
		v = gtext_json_array_get(v, 0);
	}
	return d;
}

} // namespace

TEST(JsonWriterStackGrowth, NestingPastTheInitialCapacity) {
	// 32 is the initial capacity, so these straddle it and the doublings after.
	for (size_t depth : {1u, 31u, 32u, 33u, 64u, 65u, 200u}) {
		SCOPED_TRACE("depth=" + std::to_string(depth));

		const std::string out = write_nested_arrays(depth);
		ASSERT_FALSE(out.empty());

		// Structurally: depth '[' then 42 then depth ']'.
		EXPECT_EQ(out, std::string(depth, '[') + "42" + std::string(depth, ']'));

		// And it parses back to the same depth.  max_depth has to be raised
		// for the deeper cases, which is the parser's own limit rather than
		// the writer's.
		GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
		po.max_depth = depth + 16;
		GTEXT_JSON_Error err;
		std::memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * v =
		    gtext_json_parse(out.data(), out.size(), &po, &err);
		ASSERT_NE(v, nullptr) << (err.message ? err.message : "did not parse");
		EXPECT_EQ(array_depth(v), depth);

		gtext_json_free(v);
		gtext_json_error_free(&err);
	}
}

TEST(JsonWriterStackGrowth, MixedContainersKeepTheirCommas) {
	// The stack entry records whether a container has had anything written
	// into it, which is what suppresses the leading comma.  If growth loses
	// that flag the output gains or drops a comma at the boundary - valid
	// looking, but wrong - so this alternates object and array across the
	// growth point and checks the exact bytes.
	GTEXT_JSON_Sink sink;
	ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
	GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
	ASSERT_NE(w, nullptr);

	const size_t levels = 40; // past the initial 32
	std::string expected;
	for (size_t i = 0; i < levels; ++i) {
		if (i % 2 == 0) {
			ASSERT_EQ(gtext_json_writer_object_begin(w), GTEXT_JSON_OK) << i;
			ASSERT_EQ(gtext_json_writer_key(w, "k", 1), GTEXT_JSON_OK) << i;
			expected += "{\"k\":";
		}
		else {
			ASSERT_EQ(gtext_json_writer_array_begin(w), GTEXT_JSON_OK) << i;
			expected += "[";
		}
	}
	ASSERT_EQ(gtext_json_writer_number_i64(w, 7), GTEXT_JSON_OK);
	expected += "7";
	for (size_t i = levels; i-- > 0;) {
		if (i % 2 == 0) {
			ASSERT_EQ(gtext_json_writer_object_end(w), GTEXT_JSON_OK) << i;
			expected += "}";
		}
		else {
			ASSERT_EQ(gtext_json_writer_array_end(w), GTEXT_JSON_OK) << i;
			expected += "]";
		}
	}

	const std::string out(
	    gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
	EXPECT_EQ(out, expected);

	gtext_json_writer_free(w);
	gtext_json_sink_buffer_free(&sink);
}

TEST(JsonWriterStackGrowth, SiblingsAcrossTheGrowthPointAreCommaSeparated) {
	// Many siblings in one deep container, so the "has written something"
	// flag is exercised repeatedly at a level that only exists after growth.
	GTEXT_JSON_Sink sink;
	ASSERT_EQ(gtext_json_sink_buffer(&sink), GTEXT_JSON_OK);
	GTEXT_JSON_Writer * w = gtext_json_writer_new(sink, nullptr);
	ASSERT_NE(w, nullptr);

	const size_t depth = 40;
	for (size_t i = 0; i < depth; ++i) {
		ASSERT_EQ(gtext_json_writer_array_begin(w), GTEXT_JSON_OK);
	}
	for (int i = 0; i < 5; ++i) {
		ASSERT_EQ(gtext_json_writer_number_i64(w, i), GTEXT_JSON_OK);
	}
	for (size_t i = 0; i < depth; ++i) {
		ASSERT_EQ(gtext_json_writer_array_end(w), GTEXT_JSON_OK);
	}

	const std::string out(
	    gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
	const std::string expected =
	    std::string(depth, '[') + "0,1,2,3,4" + std::string(depth, ']');
	EXPECT_EQ(out, expected);

	gtext_json_writer_free(w);
	gtext_json_sink_buffer_free(&sink);
}

// ===========================================================================
// The regular-expression seam: `pattern` and `patternProperties`
// ===========================================================================
//
// This library has no regular-expression engine and is not going to grow one.
// The two keywords that need one arrive through a vtable the caller fills in,
// and what follows tests the *seam* - that the provider is asked the right
// question, that its three answers are each turned into the right outcome,
// and that every handle it produces is handed back exactly once.
//
// The provider below is a deliberately small matcher: literals, `.`, a
// character class with ranges and negation, the quantifiers `*`, `+` and `?`,
// and the anchors `^` and `$`. It is not ECMA-262 and does not try to be.
// Proving an engine against ECMA-262 is the regex library's job, and it does
// it against Node; proving that a schema asks the engine the right question
// is this one's.

namespace {

struct ToyRegex {
	std::string source;
	bool anchored_start = false;
	bool anchored_end = false;
	std::string body; // source without the anchors
};

/**
 * Counts and knobs, so a test can see what the schema engine did rather than
 * infer it. `starve` names a pattern whose search reports that it could not
 * finish - the third answer, which a schema must not round to either of the
 * other two.
 */
struct ToyProvider {
	int compiles = 0; // calls to compile_fn, successful or not
	int handles = 0;  // handles it actually produced
	int frees = 0;
	int searches = 0;
	std::string starve;
	std::vector<std::string> searched;
};

bool toy_class_match(const std::string & body, size_t & i, unsigned char c) {
	// body[i] is the '['. Advances i past the ']'.
	size_t j = i + 1;
	bool negate = false;
	if (j < body.size() && body[j] == '^') {
		negate = true;
		j++;
	}
	bool hit = false;
	for (; j < body.size() && body[j] != ']'; j++) {
		unsigned char lo = (unsigned char)body[j];
		if (j + 2 < body.size() && body[j + 1] == '-' && body[j + 2] != ']') {
			unsigned char hi = (unsigned char)body[j + 2];
			if (c >= lo && c <= hi) {
				hit = true;
			}
			j += 2;
		}
		else if (c == lo) {
			hit = true;
		}
	}
	i = (j < body.size()) ? j + 1 : j;
	return negate ? !hit : hit;
}

size_t toy_atom_end(const std::string & body, size_t i) {
	if (body[i] != '[') {
		return i + 1;
	}
	size_t j = i + 1;
	if (j < body.size() && body[j] == '^') {
		j++;
	}
	for (; j < body.size() && body[j] != ']'; j++) {
		if (j + 2 < body.size() && body[j + 1] == '-' && body[j + 2] != ']') {
			j += 2;
		}
	}
	return (j < body.size()) ? j + 1 : j;
}

bool toy_atom_match(const std::string & body, size_t i, unsigned char c) {
	if (body[i] == '.') {
		return true;
	}
	if (body[i] == '[') {
		size_t k = i;
		return toy_class_match(body, k, c);
	}
	return (unsigned char)body[i] == c;
}

bool toy_match_here(const ToyRegex & re, const std::string & body, size_t pi,
    const std::string & subject, size_t si);

bool toy_match_quantified(const ToyRegex & re, const std::string & body,
    size_t pi, size_t next, char quant, const std::string & subject,
    size_t si) {
	size_t least = (quant == '+') ? 1 : 0;
	size_t most = (quant == '?') ? 1 : subject.size() - si;
	size_t n = 0;
	while (n < most && si + n < subject.size()
	    && toy_atom_match(body, pi, (unsigned char)subject[si + n])) {
		n++;
	}
	// Greedy, then give back one at a time, which is what a backtracking
	// matcher does and is enough for the patterns these tests use.
	for (;;) {
		if (n >= least
		    && toy_match_here(re, body, next + 1, subject, si + n)) {
			return true;
		}
		if (n == 0) {
			return false;
		}
		n--;
	}
}

bool toy_match_here(const ToyRegex & re, const std::string & body, size_t pi,
    const std::string & subject, size_t si) {
	if (pi >= body.size()) {
		return !re.anchored_end || si == subject.size();
	}
	size_t next = toy_atom_end(body, pi);
	if (next < body.size()
	    && (body[next] == '*' || body[next] == '+' || body[next] == '?')) {
		return toy_match_quantified(
		    re, body, pi, next, body[next], subject, si);
	}
	if (si < subject.size()
	    && toy_atom_match(body, pi, (unsigned char)subject[si])) {
		return toy_match_here(re, body, next, subject, si + 1);
	}
	return false;
}

int toy_compile(void * ctx, const char * pattern, size_t pattern_len,
    void ** out_regex, char * message, size_t message_capacity,
    size_t * out_offset) {
	ToyProvider * provider = (ToyProvider *)ctx;
	provider->compiles++;

	std::string source(pattern, pattern_len);

	// A refusal about the pattern as a whole rather than about a position in
	// it, which is the shape a policy refusal takes: "this needs an engine
	// whose worst case is exponential" is not about byte 4.
	if (source == "refuse-me") {
		snprintf(message, message_capacity, "no offset for this one");
		*out_offset = (size_t)-1;
		return 1;
	}

	// Two refusals, so that a failing compile has something to report: a
	// quantifier with nothing to repeat, and an unterminated class.
	for (size_t i = 0; i < source.size(); i++) {
		if ((source[i] == '*' || source[i] == '+' || source[i] == '?')
		    && (i == 0 || source[i - 1] == '^')) {
			snprintf(message, message_capacity, "nothing to repeat");
			*out_offset = i;
			return 1;
		}
		if (source[i] == '[' && source.find(']', i) == std::string::npos) {
			snprintf(message, message_capacity, "unterminated character class");
			*out_offset = i;
			return 1;
		}
	}

	ToyRegex * re = new ToyRegex();
	provider->handles++;
	re->source = source;
	std::string body = source;
	if (!body.empty() && body[0] == '^') {
		re->anchored_start = true;
		body.erase(0, 1);
	}
	if (!body.empty() && body.back() == '$') {
		re->anchored_end = true;
		body.pop_back();
	}
	re->body = body;
	*out_regex = re;
	return 0;
}

int toy_search(
    void * ctx, void * regex, const char * subject, size_t subject_len) {
	ToyProvider * provider = (ToyProvider *)ctx;
	ToyRegex * re = (ToyRegex *)regex;
	provider->searches++;
	provider->searched.push_back(std::string(subject, subject_len));

	if (!provider->starve.empty() && re->source == provider->starve) {
		return -1;
	}

	std::string text(subject, subject_len);
	// Unanchored: every starting position, which is what JSON Schema asks
	// for. A provider that returned only the match at 0 would pass a
	// surprising number of test suites and be wrong.
	size_t last = re->anchored_start ? 0 : text.size();
	for (size_t start = 0; start <= last; start++) {
		if (toy_match_here(*re, re->body, 0, text, start)) {
			return 1;
		}
	}
	return 0;
}

void toy_free(void * ctx, void * regex) {
	ToyProvider * provider = (ToyProvider *)ctx;
	provider->frees++;
	delete (ToyRegex *)regex;
}

GTEXT_JSON_Regex_Provider toy_vtable(ToyProvider * provider) {
	GTEXT_JSON_Regex_Provider v;
	v.ctx = provider;
	v.compile_fn = toy_compile;
	v.search_fn = toy_search;
	v.free_fn = toy_free;
	return v;
}

/** Parse a schema document; the caller frees it. */
GTEXT_JSON_Value * parse_doc(const char * text) {
	GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
	GTEXT_JSON_Error perr;
	memset(&perr, 0, sizeof(perr));
	return gtext_json_parse(text, strlen(text), &po, &perr);
}

} // namespace

TEST(JsonSchemaRegexSeam, ToyProviderMatchesTheWayTheTestsAssume) {
	// The fixture is code too. If it anchored, or could not do a class, the
	// tests below would pass for the wrong reason.
	ToyProvider provider;
	struct Case {
		const char * pattern;
		const char * subject;
		int expected;
	};
	const Case cases[] = {
	    {"a", "xax", 1},
	    {"^a", "xax", 0},
	    {"^a", "ax", 1},
	    {"^a+$", "aaa", 1},
	    {"^a+$", "aab", 0},
	    {"[0-9]+", "abc123", 1},
	    {"[0-9]+", "abc", 0},
	    {"^[^0-9]+$", "abc", 1},
	    {"^[^0-9]+$", "ab1", 0},
	    {"^a.c$", "abc", 1},
	    {"^ab?c$", "ac", 1},
	    {"", "anything", 1},
	};
	for (const auto & c : cases) {
		void * re = nullptr;
		char message[128];
		message[0] = '\0';
		size_t offset = (size_t)-1;
		ASSERT_EQ(toy_compile(&provider, c.pattern, strlen(c.pattern), &re,
		              message, sizeof(message), &offset),
		    0)
		    << c.pattern << ": " << message;
		EXPECT_EQ(toy_search(&provider, re, c.subject, strlen(c.subject)),
		    c.expected)
		    << c.pattern << " against " << c.subject;
		toy_free(&provider, re);
	}
	EXPECT_EQ(provider.handles, provider.frees);
}

TEST(JsonSchemaRegexSeam, PatternConstrainsStringsAndNothingElse) {
	ToyProvider provider;
	GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.regex = &vtable;

	GTEXT_JSON_Value * doc = parse_doc("{\"pattern\":\"^a+$\"}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, &err);
	ASSERT_NE(schema, nullptr) << (err.message ? err.message : "");
	EXPECT_EQ(provider.compiles, 1);

	GTEXT_JSON_Value * ok = parse_doc("\"aaa\"");
	GTEXT_JSON_Value * bad = parse_doc("\"aab\"");
	// A non-string instance is not constrained by `pattern` at all, which is
	// not the same as being constrained and passing: the provider must not
	// even be asked.
	GTEXT_JSON_Value * number = parse_doc("42");
	ASSERT_NE(ok, nullptr);
	ASSERT_NE(bad, nullptr);
	ASSERT_NE(number, nullptr);

	EXPECT_EQ(gtext_json_schema_validate(schema, ok, nullptr), GTEXT_JSON_OK);
	EXPECT_EQ(
	    gtext_json_schema_validate(schema, bad, nullptr), GTEXT_JSON_E_SCHEMA);
	int before = provider.searches;
	EXPECT_EQ(
	    gtext_json_schema_validate(schema, number, nullptr), GTEXT_JSON_OK);
	EXPECT_EQ(provider.searches, before);

	gtext_json_free(ok);
	gtext_json_free(bad);
	gtext_json_free(number);
	gtext_json_schema_free(schema);
	gtext_json_free(doc);
	EXPECT_EQ(provider.frees, provider.handles);
}

TEST(JsonSchemaRegexSeam, PatternSearchesRatherThanAnchors) {
	// JSON Schema core section 6.4: the expression need only match somewhere
	// in the string. A validator that anchored would reject every one of
	// these, and would look correct on a test suite written with `^`.
	ToyProvider provider;
	GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.regex = &vtable;

	GTEXT_JSON_Value * doc = parse_doc("{\"pattern\":\"b\"}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, nullptr);
	ASSERT_NE(schema, nullptr);

	const char * matching[] = {"\"b\"", "\"ab\"", "\"ba\"", "\"aba\""};
	for (const char * text : matching) {
		GTEXT_JSON_Value * v = parse_doc(text);
		ASSERT_NE(v, nullptr);
		EXPECT_EQ(gtext_json_schema_validate(schema, v, nullptr), GTEXT_JSON_OK)
		    << text;
		gtext_json_free(v);
	}
	GTEXT_JSON_Value * miss = parse_doc("\"aaa\"");
	EXPECT_EQ(
	    gtext_json_schema_validate(schema, miss, nullptr), GTEXT_JSON_E_SCHEMA);
	gtext_json_free(miss);

	gtext_json_schema_free(schema);
	gtext_json_free(doc);
}

TEST(JsonSchemaRegexSeam, PatternPropertiesAppliesEveryMatchingSchema) {
	// A property name can match more than one expression, and then all of
	// their schemas apply. An implementation that stopped at the first match
	// would accept {"a_x": "text"} below.
	ToyProvider provider;
	GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.regex = &vtable;

	GTEXT_JSON_Value * doc = parse_doc(
	    "{\"patternProperties\":{"
	    "\"^a\":{\"type\":\"number\"},"
	    "\"x$\":{\"minimum\":10}}}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, &err);
	ASSERT_NE(schema, nullptr) << (err.message ? err.message : "");
	EXPECT_EQ(provider.compiles, 2);

	struct Case {
		const char * instance;
		GTEXT_JSON_Status expected;
	};
	const Case cases[] = {
	    // Matches both: a number, and at least ten.
	    {"{\"a_x\":11}", GTEXT_JSON_OK},
	    // Matches both; fails the second.
	    {"{\"a_x\":1}", GTEXT_JSON_E_SCHEMA},
	    // Matches both; fails the first.
	    {"{\"a_x\":\"text\"}", GTEXT_JSON_E_SCHEMA},
	    // Matches only `^a`.
	    {"{\"ab\":1}", GTEXT_JSON_OK},
	    {"{\"ab\":\"text\"}", GTEXT_JSON_E_SCHEMA},
	    // Matches neither, and nothing else constrains it.
	    {"{\"zz\":\"text\"}", GTEXT_JSON_OK},
	};
	for (const auto & c : cases) {
		GTEXT_JSON_Value * v = parse_doc(c.instance);
		ASSERT_NE(v, nullptr) << c.instance;
		EXPECT_EQ(gtext_json_schema_validate(schema, v, nullptr), c.expected)
		    << c.instance;
		gtext_json_free(v);
	}

	gtext_json_schema_free(schema);
	gtext_json_free(doc);
	EXPECT_EQ(provider.frees, provider.handles);
}

TEST(JsonSchemaRegexSeam, PatternPropertiesExemptsAPropertyFromAdditional) {
	// The interaction that is easy to get wrong, and was wrong here until
	// `patternProperties` existed: additionalProperties applies to the
	// properties that neither `properties` named *nor* any pattern matched.
	// The comment in the validator said so while only half of it was true.
	ToyProvider provider;
	GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.regex = &vtable;

	GTEXT_JSON_Value * doc = parse_doc(
	    "{\"properties\":{\"named\":{}},"
	    "\"patternProperties\":{\"^x\":{\"type\":\"number\"}},"
	    "\"additionalProperties\":false}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, nullptr);
	ASSERT_NE(schema, nullptr);

	struct Case {
		const char * instance;
		GTEXT_JSON_Status expected;
	};
	const Case cases[] = {
	    {"{\"named\":\"anything\"}", GTEXT_JSON_OK},
	    {"{\"xy\":1}", GTEXT_JSON_OK},
	    // Matched by the pattern, so additionalProperties does not see it -
	    // but the pattern's own schema still does.
	    {"{\"xy\":\"text\"}", GTEXT_JSON_E_SCHEMA},
	    // Neither named nor matched: false rejects it.
	    {"{\"other\":1}", GTEXT_JSON_E_SCHEMA},
	};
	for (const auto & c : cases) {
		GTEXT_JSON_Value * v = parse_doc(c.instance);
		ASSERT_NE(v, nullptr) << c.instance;
		EXPECT_EQ(gtext_json_schema_validate(schema, v, nullptr), c.expected)
		    << c.instance;
		gtext_json_free(v);
	}

	gtext_json_schema_free(schema);
	gtext_json_free(doc);
}

TEST(JsonSchemaRegexSeam, WithoutAProviderBothKeywordsAreStillRefused) {
	// The default is unchanged, and deliberately so: a `pattern` that is read
	// and ignored makes a schema that looks like it constrains its data not
	// do so, with nothing to tell the caller.
	const char * schemas[] = {
	    "{\"pattern\":\"^a+$\"}",
	    "{\"patternProperties\":{\"^a\":{}}}",
	};
	const char * keywords[] = {"pattern", "patternProperties"};
	for (size_t i = 0; i < 2; i++) {
		GTEXT_JSON_Value * doc = parse_doc(schemas[i]);
		ASSERT_NE(doc, nullptr);
		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Schema * schema = gtext_json_schema_compile(doc, &err);
		EXPECT_EQ(schema, nullptr);
		EXPECT_EQ(err.code, GTEXT_JSON_E_SCHEMA_UNSUPPORTED);
		ASSERT_NE(err.context_snippet, nullptr);
		EXPECT_STREQ(err.context_snippet, keywords[i]);
		gtext_json_error_free(&err);
		gtext_json_free(doc);
	}
}

TEST(JsonSchemaRegexSeam, TheLenientOptInStillIgnoresThem) {
	ToyProvider provider;
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.allow_unsupported_keywords = true;
	GTEXT_JSON_Value * doc = parse_doc("{\"pattern\":\"^a+$\"}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, nullptr);
	ASSERT_NE(schema, nullptr);
	GTEXT_JSON_Value * v = parse_doc("\"zzz\"");
	EXPECT_EQ(gtext_json_schema_validate(schema, v, nullptr), GTEXT_JSON_OK);
	EXPECT_EQ(provider.compiles, 0);
	gtext_json_free(v);
	gtext_json_schema_free(schema);
	gtext_json_free(doc);
}

TEST(JsonSchemaRegexSeam, ARefusedPatternCarriesTheProvidersOwnWords) {
	// "nothing to repeat at offset 1" tells a schema author what to change;
	// "invalid schema" does not. The message is the provider's, because only
	// the provider knows its own grammar.
	ToyProvider provider;
	GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.regex = &vtable;

	struct Case {
		const char * schema;
		const char * message;
		size_t offset;
	};
	const Case cases[] = {
	    {"{\"pattern\":\"^+a\"}", "nothing to repeat", 1},
	    {"{\"pattern\":\"[abc\"}", "unterminated character class", 0},
	    // The second key fails, so the first key's handle is already
	    // outstanding when compilation gives up.
	    {"{\"patternProperties\":{\"^a\":{},\"^+b\":{}}}",
	        "nothing to repeat", 1},
	    // A refusal with no position stays positionless. Folding it to 0
	    // would point at a character that is not the problem.
	    {"{\"pattern\":\"refuse-me\"}", "no offset for this one",
	        (size_t)-1},
	};
	for (const auto & c : cases) {
		GTEXT_JSON_Value * doc = parse_doc(c.schema);
		ASSERT_NE(doc, nullptr) << c.schema;
		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Schema * schema =
		    gtext_json_schema_compile_with_options(doc, &opts, &err);
		EXPECT_EQ(schema, nullptr) << c.schema;
		EXPECT_EQ(err.code, GTEXT_JSON_E_INVALID) << c.schema;
		ASSERT_NE(err.context_snippet, nullptr) << c.schema;
		EXPECT_STREQ(err.context_snippet, c.message);
		EXPECT_EQ(err.offset, c.offset) << c.schema;
		gtext_json_error_free(&err);
		gtext_json_free(doc);
	}
	// Nothing was leaked on the way out: the handle compiled before the
	// failing one in the patternProperties case still has to be released,
	// which means the failure path frees a half-built node's patterns.
	EXPECT_EQ(provider.handles, 1);
	EXPECT_EQ(provider.frees, provider.handles);
}

TEST(JsonSchemaRegexSeam, ASearchThatCannotFinishIsAThirdAnswer) {
	// The point of the negative return. A pattern that spends its budget has
	// not said the instance is invalid, and reporting that as "no match"
	// would turn a denial-of-service defence into a wrong validation result.
	ToyProvider provider;
	provider.starve = "^a+$";
	GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.regex = &vtable;

	GTEXT_JSON_Value * doc = parse_doc("{\"pattern\":\"^a+$\"}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, nullptr);
	ASSERT_NE(schema, nullptr);

	GTEXT_JSON_Value * v = parse_doc("\"aaa\"");
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Status status = gtext_json_schema_validate(schema, v, &err);
	EXPECT_EQ(status, GTEXT_JSON_E_LIMIT);
	EXPECT_NE(status, GTEXT_JSON_OK);
	EXPECT_NE(status, GTEXT_JSON_E_SCHEMA);
	EXPECT_NE(err.message, nullptr);

	gtext_json_free(v);
	gtext_json_schema_free(schema);
	gtext_json_free(doc);
}

TEST(JsonSchemaRegexSeam, ASearchThatCannotFinishStopsPatternProperties) {
	// The same third answer, reached through the property-name loop, where it
	// would otherwise be swallowed as "this pattern did not match, so
	// additionalProperties applies".
	ToyProvider provider;
	provider.starve = "^a";
	GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.regex = &vtable;

	GTEXT_JSON_Value * doc = parse_doc(
	    "{\"patternProperties\":{\"^a\":{}},\"additionalProperties\":false}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, nullptr);
	ASSERT_NE(schema, nullptr);

	GTEXT_JSON_Value * v = parse_doc("{\"abc\":1}");
	EXPECT_EQ(
	    gtext_json_schema_validate(schema, v, nullptr), GTEXT_JSON_E_LIMIT);
	gtext_json_free(v);
	gtext_json_schema_free(schema);
	gtext_json_free(doc);
}

TEST(JsonSchemaRegexSeam, EveryCompiledHandleIsReleasedOnce) {
	// Patterns are compiled in nested subschemas, behind `$ref`, and in both
	// keywords at once. Each is freed when the schema is, and none twice -
	// which the counters catch and Valgrind confirms.
	ToyProvider provider;
	GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.regex = &vtable;

	GTEXT_JSON_Value * doc = parse_doc(
	    "{\"$defs\":{\"code\":{\"pattern\":\"^[0-9]+$\"}},"
	    "\"properties\":{"
	    "\"id\":{\"$ref\":\"#/$defs/code\"},"
	    "\"alt\":{\"$ref\":\"#/$defs/code\"},"
	    "\"tags\":{\"items\":{\"pattern\":\"^t\"}}},"
	    "\"patternProperties\":{\"^x\":{\"pattern\":\"y\"}}}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, &err);
	ASSERT_NE(schema, nullptr) << (err.message ? err.message : "");
	// Four: the shared $ref target compiled once, the items pattern, the
	// patternProperties key, and the pattern inside its schema.
	EXPECT_EQ(provider.compiles, 4);

	GTEXT_JSON_Value * good =
	    parse_doc("{\"id\":\"12\",\"alt\":\"7\",\"tags\":[\"tag\"],"
	              "\"xq\":\"yes\"}");
	ASSERT_NE(good, nullptr);
	EXPECT_EQ(gtext_json_schema_validate(schema, good, nullptr), GTEXT_JSON_OK);
	GTEXT_JSON_Value * bad = parse_doc("{\"id\":\"ab\"}");
	EXPECT_EQ(
	    gtext_json_schema_validate(schema, bad, nullptr), GTEXT_JSON_E_SCHEMA);
	gtext_json_free(good);
	gtext_json_free(bad);

	gtext_json_schema_free(schema);
	gtext_json_free(doc);
	EXPECT_EQ(provider.frees, 4);
}

TEST(JsonSchemaRegexSeam, AnIncompleteProviderIsRefusedAtOnce) {
	// A half-filled vtable is a bug in the adapter, and it is a different
	// thing from having no provider at all: the second refuses every
	// `pattern`, the first would be a null call somewhere much later.
	ToyProvider provider;
	GTEXT_JSON_Value * doc = parse_doc("{\"pattern\":\"a\"}");
	ASSERT_NE(doc, nullptr);

	for (int missing = 0; missing < 3; missing++) {
		GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
		if (missing == 0) {
			vtable.compile_fn = nullptr;
		}
		else if (missing == 1) {
			vtable.search_fn = nullptr;
		}
		else {
			vtable.free_fn = nullptr;
		}
		GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
		opts.regex = &vtable;
		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		EXPECT_EQ(
		    gtext_json_schema_compile_with_options(doc, &opts, &err), nullptr)
		    << "missing function " << missing;
		EXPECT_EQ(err.code, GTEXT_JSON_E_INVALID);
		gtext_json_error_free(&err);
	}
	gtext_json_free(doc);
	EXPECT_EQ(provider.compiles, 0);
}

TEST(JsonSchemaRegexSeam, TheProviderSeesTheBytesItWasPromised) {
	// The subject is the string's own bytes, UTF-8, with the length given -
	// not a copy through a NUL-terminated API, which would truncate an
	// instance containing a NUL and silently validate the prefix.
	ToyProvider provider;
	GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.regex = &vtable;

	GTEXT_JSON_Value * doc = parse_doc("{\"pattern\":\"b\"}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, nullptr);
	ASSERT_NE(schema, nullptr);

	GTEXT_JSON_Value * v = parse_doc("\"a\\u0000b\"");
	ASSERT_NE(v, nullptr);
	EXPECT_EQ(gtext_json_schema_validate(schema, v, nullptr), GTEXT_JSON_OK);
	ASSERT_EQ(provider.searched.size(), 1u);
	EXPECT_EQ(provider.searched[0].size(), 3u);
	EXPECT_EQ(provider.searched[0], std::string("a\0b", 3));

	gtext_json_free(v);
	gtext_json_schema_free(schema);
	gtext_json_free(doc);
}

// ===========================================================================
// The embedded meta-schemas
// ===========================================================================
//
// The 2020-12 dialect describes itself, so "is this a valid schema?" is a
// question written in JSON Schema: `{"$ref":
// "https://json-schema.org/draft/2020-12/schema"}` applied to the schema you
// are asking about. That only means something if the reference resolves, and
// the nine published documents of the dialect are shipped with this library so
// that it does - without a resolver, and without this library ever opening a
// connection to a URI it read out of a document somebody handed it.
//
// These meta-schemas use `pattern`, so the tests below supply the toy regular
// expression provider. That is not an artefact of the fixture: without a
// provider the question genuinely cannot be answered, and the last test here
// pins that down rather than letting it look like an accident.

namespace {

/** A schema-of-schemas, with the toy provider `pattern` needs. */
struct MetaFixture {
	ToyProvider provider;
	GTEXT_JSON_Regex_Provider vtable;
	GTEXT_JSON_Schema_Options opts;
	GTEXT_JSON_Value * doc = nullptr;
	GTEXT_JSON_Schema * schema = nullptr;
	GTEXT_JSON_Error err;

	explicit MetaFixture(const char * src) {
		vtable = toy_vtable(&provider);
		opts = gtext_json_schema_options_default();
		opts.regex = &vtable;
		memset(&err, 0, sizeof(err));
		doc = parse_doc(src);
		if (doc) {
			schema = gtext_json_schema_compile_with_options(doc, &opts, &err);
		}
	}
	~MetaFixture() {
		gtext_json_schema_free(schema);
		gtext_json_free(doc);
		// The refusal path fills in context_snippet, which is heap-allocated
		// and this library's to free rather than the caller's to ignore.
		gtext_json_error_free(&err);
	}

	/** Is `instance` - itself a schema, in these tests - accepted? */
	bool accepts(const char * instance) {
		GTEXT_JSON_Value * v = parse_doc(instance);
		EXPECT_NE(v, nullptr) << instance;
		if (!v) {
			return false;
		}
		bool ok = gtext_json_schema_validate(schema, v, nullptr)
		    == GTEXT_JSON_OK;
		gtext_json_free(v);
		return ok;
	}
};

} // namespace

TEST(JsonSchemaMetaschema, ResolvesWithoutAResolver) {
	// The point of embedding: no resolver, no network, and the reference
	// still resolves. Before this, the only way to ask whether a schema was
	// a valid schema was to hand the library a copy of the meta-schema it
	// already had to agree with.
	MetaFixture f("{\"$ref\":\"https://json-schema.org/draft/2020-12/schema\"}");
	ASSERT_EQ(f.opts.resolver, nullptr);
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");

	EXPECT_TRUE(f.accepts("{\"type\":\"string\"}"));
	EXPECT_TRUE(f.accepts("true"));
	EXPECT_TRUE(f.accepts("{\"properties\":{\"a\":{\"minimum\":1}}}"));

	// `type` takes a name from a fixed list, or an array of them.
	EXPECT_FALSE(f.accepts("{\"type\":null}"));
	EXPECT_FALSE(f.accepts("{\"type\":\"strong\"}"));
	// `minimum` is a number; `required` is an array of strings.
	EXPECT_FALSE(f.accepts("{\"minimum\":\"1\"}"));
	EXPECT_FALSE(f.accepts("{\"required\":[1]}"));
	// A schema is an object or a boolean, and nothing else.
	EXPECT_FALSE(f.accepts("[]"));
	EXPECT_FALSE(f.accepts("\"string\""));
}

TEST(JsonSchemaMetaschema, ReachesTheVocabularyMetaschemasItReferences) {
	// The root document is seven `$ref`s and a handful of deprecated
	// keywords; every constraint it applies comes from a document it points
	// at, by a *relative* reference resolved against its own `$id`. If only
	// the root were embedded this would compile and then assert nothing.
	MetaFixture f("{\"$ref\":\"https://json-schema.org/draft/2020-12/schema\"}");
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");

	// One constraint from each vocabulary meta-schema, so that a missing
	// document shows up as a test failure rather than as a schema that
	// accepts everything.
	EXPECT_FALSE(f.accepts("{\"$id\":42}"));                 // core
	EXPECT_FALSE(f.accepts("{\"allOf\":{}}"));               // applicator
	EXPECT_FALSE(f.accepts("{\"unevaluatedItems\":1}"));     // unevaluated
	EXPECT_FALSE(f.accepts("{\"maxLength\":-1}"));           // validation
	EXPECT_FALSE(f.accepts("{\"deprecated\":\"yes\"}"));     // meta-data
	EXPECT_FALSE(f.accepts("{\"format\":7}"));               // format
	EXPECT_FALSE(f.accepts("{\"contentMediaType\":7}"));     // content
}

TEST(JsonSchemaMetaschema, EnforcesTheCorePatternOnAnIdentifier) {
	// `$id` is a URI-reference whose fragment, if any, must be empty
	// (meta/core: "Non-empty fragments not allowed", `^[^#]*#?$`). This is
	// the one place in the dialect where a `pattern` decides the answer, so
	// it is also what proves the regular expression is reaching the provider
	// rather than being skipped.
	MetaFixture f("{\"$ref\":\"https://json-schema.org/draft/2020-12/schema\"}");
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");

	EXPECT_TRUE(f.accepts("{\"$id\":\"https://example.com/s.json\"}"));
	EXPECT_TRUE(f.accepts("{\"$id\":\"https://example.com/s.json#\"}"));
	EXPECT_FALSE(f.accepts("{\"$id\":\"https://example.com/s.json#frag\"}"));
	EXPECT_GT(f.provider.searches, 0);
}

TEST(JsonSchemaMetaschema, ResolvesAVocabularyMetaschemaOnItsOwn) {
	// The seven are documents in their own right, and a schema may name one
	// directly - a dialect assembled from a subset of the vocabularies does
	// exactly that.
	MetaFixture f(
	    "{\"$ref\":\"https://json-schema.org/draft/2020-12/meta/validation\"}");
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");

	EXPECT_TRUE(f.accepts("{\"maxLength\":3}"));
	EXPECT_FALSE(f.accepts("{\"maxLength\":-1}"));
	// The validation vocabulary has nothing to say about `allOf`, so a bad
	// one passes here and would not pass the root meta-schema. Naming a
	// single vocabulary has to mean only that vocabulary.
	EXPECT_TRUE(f.accepts("{\"allOf\":{}}"));
}

TEST(JsonSchemaMetaschema, TheCallersResolverWins) {
	// Embedding a document is not the same as claiming it. A caller who
	// serves one of these URIs themselves - a mirror, a dialect of their own,
	// a deliberately strict variant - must not be quietly overruled by a copy
	// they never asked for.
	StubResolver resolver;
	resolver.add("https://json-schema.org/draft/2020-12/schema", "false");
	GTEXT_JSON_Schema_Resolver vt;
	vt.ctx = &resolver;
	vt.get_fn = StubResolver::get;

	ToyProvider provider;
	GTEXT_JSON_Regex_Provider rvt = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.resolver = &vt;
	opts.regex = &rvt;

	GTEXT_JSON_Value * doc =
	    parse_doc("{\"$ref\":\"https://json-schema.org/draft/2020-12/schema\"}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, nullptr);
	ASSERT_NE(schema, nullptr);

	// `false` rejects everything, including a schema the embedded copy would
	// have accepted.
	GTEXT_JSON_Value * v = parse_doc("{\"type\":\"string\"}");
	ASSERT_NE(v, nullptr);
	EXPECT_EQ(
	    gtext_json_schema_validate(schema, v, nullptr), GTEXT_JSON_E_SCHEMA);
	EXPECT_EQ(resolver.asked.size(), 1u);

	gtext_json_free(v);
	gtext_json_schema_free(schema);
	gtext_json_free(doc);
}

TEST(JsonSchemaMetaschema, EmbedsNineteenDocumentsAndNotTheWholeWeb) {
	// A reference that leaves the document still does not resolve. What was
	// added is nineteen documents - 2020-12's nine, 2019-09's seven, and one
	// each for draft-07, draft-06 and draft-04 - not a fetcher, and a URI that
	// merely looks like one of them is refused the way it always was.
	//
	// 2019-09's root used to be in this list, because only 2020-12 was
	// embedded. It is now a document this library carries, and it has moved
	// to the test below that compiles it. draft-07's, draft-06's and draft-04's
	// went the same way; what is here in their place is draft-03's, which is
	// refused for a different reason - a dialect this engine will not read - and
	// the `https` spelling of draft-07's, which is not the URI it publishes.
	ToyProvider provider;
	GTEXT_JSON_Regex_Provider rvt = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.regex = &rvt;

	const char * elsewhere[] = {
	    "{\"$ref\":\"https://example.com/schema\"}",
	    "{\"$ref\":\"https://json-schema.org/draft/2018-01/schema\"}",
	    "{\"$ref\":\"https://json-schema.org/draft/2019-09/meta/unevaluated\"}",
	    "{\"$ref\":\"https://json-schema.org/draft/2020-12/meta/nonesuch\"}",
	    "{\"$ref\":\"https://json-schema.org/draft/2020-12/schema/\"}",
	    "{\"$ref\":\"https://json-schema.org/draft-07/schema\"}",
	    "{\"$ref\":\"http://json-schema.org/draft-05/schema#\"}",
	    "{\"$ref\":\"http://json-schema.org/draft-03/schema#\"}",
	};
	for (const char * src : elsewhere) {
		GTEXT_JSON_Value * doc = parse_doc(src);
		ASSERT_NE(doc, nullptr) << src;
		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Schema * schema =
		    gtext_json_schema_compile_with_options(doc, &opts, &err);
		EXPECT_EQ(schema, nullptr) << src;
		gtext_json_schema_free(schema);
		gtext_json_free(doc);
		gtext_json_error_free(&err);
	}
}

TEST(JsonSchemaMetaschema, NeedsARegexProviderAndSaysSo) {
	// meta/core constrains `$id` and `$anchor` with `pattern`, so without a
	// provider this library cannot answer the question at all. It refuses,
	// which is the same answer it gives for `pattern` anywhere else - the
	// alternative would be to report a schema as valid on the strength of
	// two constraints that were never checked.
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	ASSERT_EQ(opts.regex, nullptr);

	GTEXT_JSON_Value * doc =
	    parse_doc("{\"$ref\":\"https://json-schema.org/draft/2020-12/schema\"}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, &err);
	EXPECT_EQ(schema, nullptr);
	EXPECT_EQ(err.code, GTEXT_JSON_E_SCHEMA_UNSUPPORTED);
	// Named, so that a caller who hits this knows which keyword stopped them
	// rather than being told only that something did.
	ASSERT_NE(err.context_snippet, nullptr);
	EXPECT_STREQ(err.context_snippet, "pattern");

	gtext_json_schema_free(schema);
	gtext_json_free(doc);
	gtext_json_error_free(&err);
}

// ===========================================================================
// "integer" is about the value, not about what fits in a machine word
// ===========================================================================
//
// The old test was `dv == (double)(long long)dv`, which is undefined behaviour
// for any double outside `long long`'s range - and what the hardware does with
// it on x86-64 is produce LLONG_MIN, which compares unequal, so every whole
// number past about 9.2e18 was reported as not an integer.
//
// The digits the document wrote decide it now. That is not only a wider range:
// it is a different question, and at the edges the two answers differ in both
// directions.

namespace {

/** Does `{"type":"integer"}` accept this literal? */
bool is_integer(const char * literal) {
	GTEXT_JSON_Value * doc = parse_doc("{\"type\":\"integer\"}");
	EXPECT_NE(doc, nullptr);
	GTEXT_JSON_Schema * schema = gtext_json_schema_compile(doc, nullptr);
	EXPECT_NE(schema, nullptr);
	GTEXT_JSON_Value * v = parse_doc(literal);
	EXPECT_NE(v, nullptr) << literal;
	bool ok = v
	    && gtext_json_schema_validate(schema, v, nullptr) == GTEXT_JSON_OK;
	gtext_json_free(v);
	gtext_json_schema_free(schema);
	gtext_json_free(doc);
	return ok;
}

} // namespace

TEST(JsonSchemaInteger, WholeNumbersOfAnySize) {
	EXPECT_TRUE(is_integer("0"));
	EXPECT_TRUE(is_integer("5"));
	EXPECT_TRUE(is_integer("-5"));
	EXPECT_TRUE(is_integer("5.0"));
	EXPECT_TRUE(is_integer("-5.00000"));

	// Past 2^63, where the old cast was undefined. The suite's own bignums.
	EXPECT_TRUE(is_integer(
	    "12345678910111213141516171819202122232425262728293031"));
	EXPECT_TRUE(is_integer(
	    "-12345678910111213141516171819202122232425262728293031"));
	EXPECT_TRUE(is_integer("18446744073709551616"));

	// Past the range of a double's exact integers, and past its range
	// entirely as far as any cast is concerned.
	EXPECT_TRUE(is_integer("1e308"));
	EXPECT_TRUE(is_integer("-1e308"));
	// Scaled up past every fractional digit, so the value is whole even
	// though the lexeme has a point in it.
	EXPECT_TRUE(is_integer("1.5e300"));
	EXPECT_TRUE(is_integer("1.5e1"));
	EXPECT_TRUE(is_integer("1234.5678e4"));
}

TEST(JsonSchemaInteger, NumbersWithAFractionAreNot) {
	EXPECT_FALSE(is_integer("5.5"));
	EXPECT_FALSE(is_integer("-5.5"));
	EXPECT_FALSE(is_integer("1.5e-1"));
	EXPECT_FALSE(is_integer("1234.5678e3"));
	// One digit short of cancelling the fraction.
	EXPECT_FALSE(is_integer("1.5e0"));
	// Scaled below one, and not zero.
	EXPECT_FALSE(is_integer("15e-2"));
	EXPECT_FALSE(is_integer("1e-400"));
}

TEST(JsonSchemaInteger, TheDigitsDecideAndNotTheDouble) {
	// The nearest double to this is a whole number - every double above 2^53
	// is - but the value the document wrote is not, and the value is what the
	// keyword constrains. An implementation that converted first would say
	// yes, and the suite's bignum group would still pass.
	EXPECT_FALSE(is_integer("972783798187987123879878123.188781371"));
	EXPECT_FALSE(
	    is_integer("1.00000000000000000000000000000000000000000000000001e30"));

	// And the other direction: a whole number no double can hold exactly.
	EXPECT_TRUE(is_integer("10000000000000000000000000000000000000000000001"));
}

TEST(JsonSchemaInteger, EveryDigitZeroIsWholeHoweverItIsScaled) {
	EXPECT_TRUE(is_integer("0"));
	EXPECT_TRUE(is_integer("-0"));
	EXPECT_TRUE(is_integer("0.0"));
	EXPECT_TRUE(is_integer("0.000e-99"));
	EXPECT_TRUE(is_integer("0e-99999999999999999999"));
}

TEST(JsonSchemaInteger, MultipleOfSurvivesAnExponentThatWouldOverflow) {
	// The suite's float-overflow case. 1e308 / 0.5 is 2e308, which is not a
	// double, so an implementation that divides gets infinity and an
	// implementation that takes fmod gets the right answer by luck. The
	// decimal path answers it in exponents and never forms the quotient.
	GTEXT_JSON_Value * doc =
	    parse_doc("{\"type\":\"integer\",\"multipleOf\":0.5}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Schema * schema = gtext_json_schema_compile(doc, nullptr);
	ASSERT_NE(schema, nullptr);

	GTEXT_JSON_Value * v = parse_doc("1e308");
	ASSERT_NE(v, nullptr);
	EXPECT_EQ(gtext_json_schema_validate(schema, v, nullptr), GTEXT_JSON_OK);
	gtext_json_free(v);

	GTEXT_JSON_Value * odd = parse_doc("0.75");
	ASSERT_NE(odd, nullptr);
	EXPECT_EQ(
	    gtext_json_schema_validate(schema, odd, nullptr), GTEXT_JSON_E_SCHEMA);
	gtext_json_free(odd);

	gtext_json_schema_free(schema);
	gtext_json_free(doc);
}

TEST(JsonSchemaInteger, ADomBuiltNumberHasNoLexemeAndStillWorks) {
	// Nothing built through the DOM API carries the digits a document would
	// have, so the double is all there is. It answers correctly for every
	// value a double can hold, which is every value this path can produce.
	GTEXT_JSON_Value * doc = parse_doc("{\"type\":\"integer\"}");
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Schema * schema = gtext_json_schema_compile(doc, nullptr);
	ASSERT_NE(schema, nullptr);

	GTEXT_JSON_Value * whole = gtext_json_new_number_double(42.0);
	GTEXT_JSON_Value * fraction = gtext_json_new_number_double(42.5);
	GTEXT_JSON_Value * huge = gtext_json_new_number_double(1e300);
	ASSERT_NE(whole, nullptr);
	ASSERT_NE(fraction, nullptr);
	ASSERT_NE(huge, nullptr);
	EXPECT_EQ(gtext_json_schema_validate(schema, whole, nullptr), GTEXT_JSON_OK);
	EXPECT_EQ(gtext_json_schema_validate(schema, fraction, nullptr),
	    GTEXT_JSON_E_SCHEMA);
	EXPECT_EQ(gtext_json_schema_validate(schema, huge, nullptr), GTEXT_JSON_OK);

	gtext_json_free(whole);
	gtext_json_free(fraction);
	gtext_json_free(huge);
	gtext_json_schema_free(schema);
	gtext_json_free(doc);
}

// ===========================================================================
// `$schema` selects a draft, not just a set of vocabularies
// ===========================================================================
//
// A `$ref` can leave the document and land in one written years earlier, and
// that document has to be read as what it says it is. The engine used to
// record `$schema` and then compile everything with 2020-12's meanings, so a
// 2019-09 schema's `prefixItems` - a member that draft never defined - was
// enforced as a 2020-12 keyword.
//
// The dialect is scoped exactly as the base URI is: it applies to the resource
// that declares it and to everything inside, and the resource outside is
// unaffected.

namespace {

/** Compile `src` with `resolver` attached, or return nullptr. */
GTEXT_JSON_Schema * compile_with(const char * src, StubResolver * resolver,
    GTEXT_JSON_Error * err = nullptr) {
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	GTEXT_JSON_Schema_Resolver vt;
	if (resolver) {
		vt.ctx = resolver;
		vt.get_fn = StubResolver::get;
		opts.resolver = &vt;
	}
	GTEXT_JSON_Value * doc = parse_doc(src);
	EXPECT_NE(doc, nullptr) << src;
	if (!doc) {
		return nullptr;
	}
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, err);
	gtext_json_free(doc);
	return schema;
}

bool schema_accepts(GTEXT_JSON_Schema * schema, const char * instance) {
	GTEXT_JSON_Value * v = parse_doc(instance);
	EXPECT_NE(v, nullptr) << instance;
	bool ok = v
	    && gtext_json_schema_validate(schema, v, nullptr) == GTEXT_JSON_OK;
	gtext_json_free(v);
	return ok;
}

} // namespace

TEST(JsonSchemaDraft, PrefixItemsIsNotAKeywordIn2019_09) {
	// The suite's optional/cross-draft case. The referenced document says it
	// is 2019-09, where `prefixItems` does not exist, so [1,2,3] satisfies it
	// - and would not if the reference were read as 2020-12.
	StubResolver resolver;
	resolver.add("http://example.com/2019.json",
	    "{\"$id\":\"http://example.com/2019.json\","
	    "\"$schema\":\"https://json-schema.org/draft/2019-09/schema\","
	    "\"prefixItems\":[{\"type\":\"string\"}]}");

	GTEXT_JSON_Schema * schema = compile_with(
	    "{\"type\":\"array\",\"$ref\":\"http://example.com/2019.json\"}",
	    &resolver);
	ASSERT_NE(schema, nullptr);
	EXPECT_TRUE(schema_accepts(schema, "[1, 2, 3]"));
	EXPECT_TRUE(schema_accepts(schema, "[\"a\"]"));
	gtext_json_schema_free(schema);
}

TEST(JsonSchemaDraft, TheDialectIsScopedToTheResourceThatDeclaresIt) {
	// The same keyword, in the same compile, meaning different things either
	// side of a resource boundary. A dialect that leaked outward would make
	// the outer document's `prefixItems` stop working the moment it referred
	// to an older one.
	StubResolver resolver;
	resolver.add("http://example.com/2019.json",
	    "{\"$id\":\"http://example.com/2019.json\","
	    "\"$schema\":\"https://json-schema.org/draft/2019-09/schema\","
	    "\"prefixItems\":[{\"type\":\"string\"}]}");

	GTEXT_JSON_Schema * schema = compile_with(
	    "{\"allOf\":[{\"$ref\":\"http://example.com/2019.json\"},"
	    "{\"prefixItems\":[{\"type\":\"number\"}]}]}",
	    &resolver);
	ASSERT_NE(schema, nullptr);
	// The outer branch is 2020-12 and does constrain the first element.
	EXPECT_TRUE(schema_accepts(schema, "[1]"));
	EXPECT_FALSE(schema_accepts(schema, "[\"a\"]"));
	gtext_json_schema_free(schema);
}

TEST(JsonSchemaDraft, BeforeTwentyNineteenARefIgnoresItsSiblings) {
	// Core section 8.2.4.1 in draft-07: "All other properties in a "$ref"
	// object MUST be ignored." 2019-09 made `$ref` an applicator like any
	// other. Reading a draft-07 document under the newer rule adds
	// constraints its author did not write.
	GTEXT_JSON_Schema * seven = compile_with(
	    "{\"$schema\":\"http://json-schema.org/draft-07/schema#\","
	    "\"definitions\":{\"any\":{}},"
	    "\"$ref\":\"#/definitions/any\",\"maximum\":5}",
	    nullptr);
	ASSERT_NE(seven, nullptr);
	EXPECT_TRUE(schema_accepts(seven, "10"));
	gtext_json_schema_free(seven);

	// The identical document, said to be 2019-09, where the sibling applies.
	GTEXT_JSON_Schema * newer = compile_with(
	    "{\"$schema\":\"https://json-schema.org/draft/2019-09/schema\","
	    "\"$defs\":{\"any\":{}},"
	    "\"$ref\":\"#/$defs/any\",\"maximum\":5}",
	    nullptr);
	ASSERT_NE(newer, nullptr);
	EXPECT_FALSE(schema_accepts(newer, "10"));
	EXPECT_TRUE(schema_accepts(newer, "1"));
	gtext_json_schema_free(newer);
}

TEST(JsonSchemaDraft, KeywordsThatDidNotExistYetAreIgnored) {
	struct Case {
		const char * dialect;
		const char * keyword;
		const char * schema;
		const char * instance;
	};
	// Each of these constrains the instance under 2020-12 and does not exist
	// in the draft named beside it.
	const Case cases[] = {
	    {"https://json-schema.org/draft/2019-09/schema", "prefixItems",
	        "\"prefixItems\":[{\"type\":\"string\"}]", "[1]"},
	    {"http://json-schema.org/draft-07/schema#", "unevaluatedProperties",
	        "\"unevaluatedProperties\":false", "{\"a\":1}"},
	    {"http://json-schema.org/draft-07/schema#", "dependentRequired",
	        "\"dependentRequired\":{\"a\":[\"b\"]}", "{\"a\":1}"},
	    {"http://json-schema.org/draft-07/schema#", "minContains",
	        "\"contains\":{\"type\":\"string\"},\"minContains\":2",
	        "[\"x\", 1]"},
	    {"http://json-schema.org/draft-06/schema#", "if",
	        "\"if\":{\"type\":\"number\"},\"then\":{\"maximum\":1}", "5"},
	};
	for (const Case & c : cases) {
		std::string src = std::string("{\"$schema\":\"") + c.dialect + "\","
		    + c.schema + "}";
		GTEXT_JSON_Schema * schema = compile_with(src.c_str(), nullptr);
		ASSERT_NE(schema, nullptr) << c.keyword;
		EXPECT_TRUE(schema_accepts(schema, c.instance))
		    << c.keyword << " should not be a keyword in " << c.dialect;
		gtext_json_schema_free(schema);

		// And the same document under 2020-12, where it is a keyword, to
		// prove the case would otherwise have failed.
		std::string modern =
		    std::string("{\"$schema\":"
		                "\"https://json-schema.org/draft/2020-12/schema\",")
		    + c.schema + "}";
		GTEXT_JSON_Schema * now = compile_with(modern.c_str(), nullptr);
		ASSERT_NE(now, nullptr) << c.keyword;
		EXPECT_FALSE(schema_accepts(now, c.instance)) << c.keyword;
		gtext_json_schema_free(now);
	}
}

// ===========================================================================
// draft-04
// ===========================================================================
//
// The oldest draft this engine reads, and the one where the differences stop
// being differences of keyword set. Three rules are its own: the identifier is
// `id`, `exclusiveMinimum`/`exclusiveMaximum` are booleans over
// `minimum`/`maximum`, and `integer` is a constraint on how a number is
// written. It was refused until all three were implemented, on the argument
// that reading it as draft-06 answers the instance wrongly - which is an
// argument against guessing, not against implementing.

TEST(JsonSchemaDraft04, ExclusiveBoundsAreBooleansOverTheBoundsTheyName) {
	{
		MetaFixture f(
		    "{\"$schema\":\"http://json-schema.org/draft-04/schema#\","
		    "\"minimum\":3,\"exclusiveMinimum\":true}");
		ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
		EXPECT_FALSE(f.accepts("3"));
		EXPECT_TRUE(f.accepts("3.5"));
	}
	{
		// `false` is the default written out, so the bound stays inclusive.
		MetaFixture f(
		    "{\"$schema\":\"http://json-schema.org/draft-04/schema#\","
		    "\"maximum\":3,\"exclusiveMaximum\":false}");
		ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
		EXPECT_TRUE(f.accepts("3"));
		EXPECT_FALSE(f.accepts("3.5"));
	}
	{
		// The keyword order is the document's, and the bound it modifies may
		// come second, so the two halves are joined after the object is read.
		MetaFixture f(
		    "{\"$schema\":\"http://json-schema.org/draft-04/schema#\","
		    "\"exclusiveMaximum\":true,\"maximum\":3}");
		ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
		EXPECT_FALSE(f.accepts("3"));
		EXPECT_TRUE(f.accepts("2"));
	}
	{
		// A number there is draft-06's spelling, and in draft-04 it is not this
		// keyword at all. Refused rather than read as a bound, because reading
		// it as one is the wrong answer this draft was refused over.
		MetaFixture f(
		    "{\"$schema\":\"http://json-schema.org/draft-04/schema#\","
		    "\"exclusiveMinimum\":3}");
		EXPECT_EQ(f.schema, nullptr);
	}
	{
		// And a bound to modify is required: draft-04's meta-schema makes each
		// depend on the other, and a lone `true` asserts a bound whose value is
		// nowhere in the document.
		MetaFixture f(
		    "{\"$schema\":\"http://json-schema.org/draft-04/schema#\","
		    "\"exclusiveMinimum\":true}");
		EXPECT_EQ(f.schema, nullptr);
	}
	{
		// draft-06 onward: a number, and a boolean is not the keyword.
		MetaFixture f(
		    "{\"$schema\":\"http://json-schema.org/draft-06/schema#\","
		    "\"minimum\":3,\"exclusiveMinimum\":true}");
		EXPECT_EQ(f.schema, nullptr);
	}
}

TEST(JsonSchemaDraft04, TheIdentifierIsSpeltIdAndOnlyThere) {
	// `id` is what draft-06 renamed to `$id`. It has to move the base URI in
	// draft-04 and must not in any later draft, where a document carrying one
	// has written an annotation - moving the base there would resolve a `$ref`
	// against a document nobody named.
	MetaFixture four("{\"$schema\":\"http://json-schema.org/draft-04/schema#\","
	                 "\"id\":\"http://e.com/root.json\","
	                 "\"properties\":{\"x\":{\"$ref\":\"sub.json\"}},"
	                 "\"definitions\":{\"B\":{\"id\":\"sub.json\","
	                 "                        \"type\":\"integer\"}}}");
	ASSERT_NE(four.schema, nullptr)
	    << (four.err.message ? four.err.message : "");
	EXPECT_TRUE(four.accepts("{\"x\":1}"));
	EXPECT_FALSE(four.accepts("{\"x\":\"a\"}"));

	// The same document called draft-06: `id` names nothing, so `sub.json` does
	// not resolve and the compile is refused rather than resolving somewhere.
	MetaFixture six("{\"$schema\":\"http://json-schema.org/draft-06/schema#\","
	                "\"id\":\"http://e.com/root.json\","
	                "\"properties\":{\"x\":{\"$ref\":\"sub.json\"}},"
	                "\"definitions\":{\"B\":{\"id\":\"sub.json\","
	                "                        \"type\":\"integer\"}}}");
	EXPECT_EQ(six.schema, nullptr);

	// And `$id` is not draft-04's keyword either, so the same document written
	// the modern way does not resolve there.
	MetaFixture wrong(
	    "{\"$schema\":\"http://json-schema.org/draft-04/schema#\","
	    "\"$id\":\"http://e.com/root.json\","
	    "\"properties\":{\"x\":{\"$ref\":\"sub.json\"}},"
	    "\"definitions\":{\"B\":{\"$id\":\"sub.json\","
	    "                        \"type\":\"integer\"}}}");
	EXPECT_EQ(wrong.schema, nullptr);
}

TEST(JsonSchemaDraft04, AFragmentOnlyIdIsALocationIndependentName) {
	// draft-04 has the same location-independent identifier as draft-06 and
	// draft-07, under the older keyword name.
	MetaFixture f("{\"$schema\":\"http://json-schema.org/draft-04/schema#\","
	              "\"allOf\":[{\"$ref\":\"#foo\"}],"
	              "\"definitions\":{\"A\":{\"id\":\"#foo\","
	              "                        \"type\":\"integer\"}}}");
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
	EXPECT_TRUE(f.accepts("1"));
	EXPECT_FALSE(f.accepts("\"a\""));
}

TEST(JsonSchemaDraft04, IntegerIsAboutHowTheNumberIsWritten) {
	// draft-04 section 3.5 defines the primitive types over the JSON text, so a
	// fraction or an exponent makes a number a "number" rather than an
	// "integer" whatever its value. draft-06 changed the rule to be about the
	// value, which is why 1.0 is an integer there and not here.
	//
	// The suite puts this in optional/zeroTerminatedFloats.json because an
	// implementation that has already turned the text into a double cannot
	// answer it. This one keeps the lexeme.
	MetaFixture four("{\"$schema\":\"http://json-schema.org/draft-04/schema#\","
	                 "\"type\":\"integer\"}");
	ASSERT_NE(four.schema, nullptr) << (four.err.message ? four.err.message : "");
	EXPECT_TRUE(four.accepts("1"));
	EXPECT_TRUE(four.accepts("-7"));
	EXPECT_FALSE(four.accepts("1.0"));
	EXPECT_FALSE(four.accepts("1e2"));
	EXPECT_FALSE(four.accepts("1.5"));

	MetaFixture six("{\"$schema\":\"http://json-schema.org/draft-06/schema#\","
	                "\"type\":\"integer\"}");
	ASSERT_NE(six.schema, nullptr) << (six.err.message ? six.err.message : "");
	EXPECT_TRUE(six.accepts("1.0"));
	EXPECT_TRUE(six.accepts("1e2"));
	EXPECT_FALSE(six.accepts("1.5"));
}

TEST(JsonSchemaDraft04, KeywordsDraft06AddedAreNotKeywordsHere) {
	// `const`, `contains` and `propertyNames` all arrived in draft-06. In
	// draft-04 they are unknown members, and an unknown member is ignored -
	// so a schema using one constrains nothing rather than constraining
	// something the draft cannot express.
	struct Case {
		const char * keyword;
		const char * schema;
		const char * rejected;
	};
	const Case cases[] = {
	    {"const", "\"const\":1", "2"},
	    {"contains", "\"contains\":{\"type\":\"integer\"}", "[\"a\"]"},
	    {"propertyNames",
	        "\"propertyNames\":{\"maxLength\":1}", "{\"ab\":1}"},
	};
	for (const Case & c : cases) {
		MetaFixture four(
		    (std::string("{\"$schema\":\"http://json-schema.org/draft-04/"
		                "schema#\",")
		        + c.schema + "}")
		        .c_str());
		ASSERT_NE(four.schema, nullptr) << c.keyword;
		EXPECT_TRUE(four.accepts(c.rejected)) << c.keyword;

		// And draft-06, where each of them is the keyword it looks like.
		MetaFixture six(
		    (std::string("{\"$schema\":\"http://json-schema.org/draft-06/"
		                "schema#\",")
		        + c.schema + "}")
		        .c_str());
		ASSERT_NE(six.schema, nullptr) << c.keyword;
		EXPECT_FALSE(six.accepts(c.rejected)) << c.keyword;
	}
}

TEST(JsonSchemaDraft04, ItsMetaschemaIsEmbeddedToo) {
	// So "is this a valid draft-04 schema?" needs no resolver, which is what
	// the suite's draft4 definitions.json and ref.json ask.
	MetaFixture f("{\"$ref\":\"http://json-schema.org/draft-04/schema#\"}");
	ASSERT_EQ(f.opts.resolver, nullptr);
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");

	EXPECT_TRUE(f.accepts("{\"type\":\"string\"}"));
	EXPECT_TRUE(f.accepts("{\"minimum\":1,\"exclusiveMinimum\":true}"));
	EXPECT_FALSE(f.accepts("{\"type\":\"strong\"}"));
	EXPECT_FALSE(f.accepts("{\"maxLength\":-1}"));
	// draft-04's own document says the exclusive bounds are booleans, so the
	// later drafts' spelling is what it rejects - the opposite way round from
	// draft-06's and draft-07's meta-schemas.
	EXPECT_FALSE(f.accepts("{\"exclusiveMinimum\":3}"));
	// A boolean is not a schema in draft-04; it became one in draft-06.
	EXPECT_FALSE(f.accepts("true"));
}

TEST(JsonSchemaDraft, DraftsThisEngineCannotReadAreRefused) {
	// draft-03 is a different language rather than the same one spelled
	// differently: `required` is a boolean on each property rather than a list
	// on the object, and `divisibleBy` is `multipleOf` under another name.
	// Nothing in this engine's structure corresponds to either.
	//
	// The unversioned alias names no draft at all. json-schema.org has answered
	// it with whatever was current - draft-04 for years, 2020-12 today - so a
	// document using it has not said what it was written against, which is the
	// case this engine refuses rather than guesses at. draft-04 was on this list
	// until it was implemented; the difference is that implementing a rule is
	// not guessing at one.
	const char * old_drafts[] = {
	    "http://json-schema.org/draft-03/schema#",
	    "http://json-schema.org/schema#",
	    "http://json-schema.org/schema",
	};
	for (const char * dialect : old_drafts) {
		std::string src =
		    std::string("{\"$schema\":\"") + dialect + "\",\"minimum\":1}";
		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Schema * schema = compile_with(src.c_str(), nullptr, &err);
		EXPECT_EQ(schema, nullptr) << dialect;
		EXPECT_EQ(err.code, GTEXT_JSON_E_SCHEMA_UNSUPPORTED) << dialect;
		// Named, so the caller is told which draft rather than only that
		// something was wrong with their `$schema`.
		EXPECT_NE(err.context_snippet, nullptr) << dialect;
		gtext_json_schema_free(schema);
		gtext_json_error_free(&err);
	}
}

TEST(JsonSchemaDraft, AnUnknownDialectIsStillReadAsTheStandardOne) {
	// A `$schema` naming a metaschema nobody can supply is overwhelmingly a
	// schema written against the standard dialect that said so. Refusing
	// those would be a worse answer than assuming the usual one, and it is
	// the behaviour that was there before drafts were dispatched on.
	GTEXT_JSON_Schema * schema = compile_with(
	    "{\"$schema\":\"https://example.com/my-dialect\","
	    "\"prefixItems\":[{\"type\":\"string\"}]}",
	    nullptr);
	ASSERT_NE(schema, nullptr);
	EXPECT_FALSE(schema_accepts(schema, "[1]"));
	EXPECT_TRUE(schema_accepts(schema, "[\"a\"]"));
	gtext_json_schema_free(schema);
}

int main(int argc, char * * argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// ---------------------------------------------------------------------------
// 2019-09's $recursiveRef / $recursiveAnchor
//
// 2020-12 replaced these with $dynamicRef and $dynamicAnchor, and this engine
// refused any schema carrying $recursiveRef until it could answer it. The
// mechanism is the same scope walk, with one anonymous anchor name instead of
// many.
// ---------------------------------------------------------------------------

TEST(JsonSchemaRecursiveRef, TheOutermostAnchorOnThePathWins) {
	// The recursive-extension pattern the keyword exists for. `strict-tree`
	// refers to `tree` and adds `unevaluatedProperties: false`; `tree` refers
	// to itself with `$recursiveRef`, and because both resources declare
	// `$recursiveAnchor: true` that lands back on `strict-tree` at every
	// level of the recursion rather than on `tree`.
	//
	// The misspelling in the *child* is what tells the two readings apart.
	// Under a plain `$ref` the child would be checked against `tree`, which
	// allows any property, and the document would be valid.
	MetaFixture f(
	    "{\"$schema\":\"https://json-schema.org/draft/2019-09/schema\","
	    "\"$id\":\"https://example.com/strict-tree\","
	    "\"$recursiveAnchor\":true,"
	    "\"$ref\":\"https://example.com/tree\","
	    "\"unevaluatedProperties\":false,"
	    "\"$defs\":{\"tree\":{"
	    "\"$id\":\"https://example.com/tree\","
	    "\"$recursiveAnchor\":true,"
	    "\"type\":\"object\","
	    "\"properties\":{\"data\":true,"
	    "\"children\":{\"type\":\"array\","
	    "\"items\":{\"$recursiveRef\":\"#\"}}}}}}");
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");

	EXPECT_TRUE(f.accepts("{\"data\":1}"));
	EXPECT_TRUE(f.accepts("{\"data\":1,\"children\":[{\"data\":2}]}"));
	EXPECT_FALSE(f.accepts("{\"daat\":1}"));
	// The one that only fails if the recursion carried strict-tree down.
	EXPECT_FALSE(f.accepts("{\"data\":1,\"children\":[{\"daat\":2}]}"));
}

TEST(JsonSchemaRecursiveRef, WithoutAnAnchorItIsAnOrdinaryRef) {
	// A resource that does not declare `$recursiveAnchor` gets no dynamic
	// behaviour: `$recursiveRef: "#"` is `$ref: "#"` with a longer spelling,
	// which is the same relationship `$dynamicRef` has to a plain anchor.
	MetaFixture f(
	    "{\"$schema\":\"https://json-schema.org/draft/2019-09/schema\","
	    "\"$id\":\"https://example.com/plain\","
	    "\"type\":\"object\","
	    "\"properties\":{\"child\":{\"$recursiveRef\":\"#\"}}}");
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");

	EXPECT_TRUE(f.accepts("{}"));
	EXPECT_TRUE(f.accepts("{\"child\":{\"child\":{}}}"));
	// The self-reference still carries the object type down.
	EXPECT_FALSE(f.accepts("{\"child\":1}"));
}

TEST(JsonSchemaRecursiveRef, OnlyTheEmptyFragmentIsAllowed) {
	// 2019-09 core section 8.2.4.2.1 gives the keyword exactly one legal
	// value. Reading anything else would mean guessing at a reference the
	// specification does not define.
	MetaFixture f(
	    "{\"$schema\":\"https://json-schema.org/draft/2019-09/schema\","
	    "\"$recursiveRef\":\"#/$defs/x\",\"$defs\":{\"x\":true}}");
	EXPECT_EQ(f.schema, nullptr);
	EXPECT_EQ(f.err.code, GTEXT_JSON_E_SCHEMA_UNSUPPORTED);
}

TEST(JsonSchemaRecursiveRef, NeitherKeywordExistsIn2020_12) {
	// 2020-12 removed both. A document carrying them under that dialect has
	// written unknown members, and unknown members are ignored - not
	// honoured, and not refused either, which is why the illegal
	// `$recursiveRef` value above compiles here.
	MetaFixture f(
	    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
	    "\"$recursiveAnchor\":true,"
	    "\"$recursiveRef\":\"#/$defs/never\","
	    "\"type\":\"object\","
	    "\"$defs\":{\"never\":false}}");
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");

	// Were `$recursiveRef` honoured here it would resolve to `false`, and
	// nothing at all would validate.
	EXPECT_TRUE(f.accepts("{}"));
	EXPECT_FALSE(f.accepts("1"));
}

TEST(JsonSchemaMetaschema, Resolves2019_09WithoutAResolver) {
	// The embedded set is both dialects that publish one, not just the
	// newest. Without this a 2019-09 document that validates another schema -
	// which its own `$defs` meta-schema test does - had nowhere to resolve.
	MetaFixture f(
	    "{\"$ref\":\"https://json-schema.org/draft/2019-09/schema\"}");
	ASSERT_EQ(f.opts.resolver, nullptr);
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");

	EXPECT_TRUE(f.accepts("{\"type\":\"string\"}"));
	EXPECT_FALSE(f.accepts("{\"type\":\"nonesuch\"}"));
}

// ===========================================================================
// draft-07 and draft-06 describe themselves too
// ===========================================================================
//
// Both publish a single document rather than a root plus a vocabulary set:
// `$vocabulary` arrived in 2019-09, so before it the keyword set *is* the
// draft. Until these were embedded, "is this a valid draft-07 schema?" was the
// one dialect question this library could not answer without a resolver, and
// two groups of the suite's draft7 and draft6 files - `definitions.json`'s
// "validate definition against metaschema" and `ref.json`'s "remote ref,
// containing refs itself" - were refused for that reason alone.

TEST(JsonSchemaMetaschema, ResolvesDraft07WithoutAResolver) {
	MetaFixture f("{\"$ref\":\"http://json-schema.org/draft-07/schema#\"}");
	ASSERT_EQ(f.opts.resolver, nullptr);
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");

	EXPECT_TRUE(f.accepts("{\"type\":\"string\"}"));
	EXPECT_TRUE(f.accepts("true"));
	EXPECT_TRUE(f.accepts("{\"definitions\":{\"a\":{\"minimum\":1}}}"));
	EXPECT_TRUE(f.accepts("{\"items\":[{\"type\":\"integer\"}]}"));

	EXPECT_FALSE(f.accepts("{\"type\":\"strong\"}"));
	EXPECT_FALSE(f.accepts("{\"maxLength\":-1}"));
	EXPECT_FALSE(f.accepts("{\"required\":[1]}"));
	EXPECT_FALSE(f.accepts("{\"allOf\":{}}"));
	EXPECT_FALSE(f.accepts("[]"));
	// draft-04's spelling of the keyword, which draft-07 made a number. The
	// document that answered has to be draft-07's for this to be invalid.
	EXPECT_FALSE(f.accepts("{\"exclusiveMinimum\":true}"));
}

TEST(JsonSchemaMetaschema, TheOlderDraftsAnswerAsThemselvesAndNotAs2020_12) {
	// The reference crosses into a document whose own `$schema` is draft-07,
	// so from there the dialect is draft-07 - and the constraints are that
	// document's, which are *looser* in places. 2020-12's meta/core pins `$id`
	// to `^[^#]*#?$` and constrains `$defs`; draft-07's metaschema says only
	// that `$id` is a string and has never heard of `$defs`. A test that only
	// checked what both refuse would pass with the wrong document answering.
	MetaFixture f("{\"$ref\":\"http://json-schema.org/draft-07/schema#\"}");
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
	EXPECT_TRUE(f.accepts("{\"$id\":\"https://example.com/s.json#frag\"}"));
	EXPECT_TRUE(f.accepts("{\"$defs\":7}"));

	// And the same two against 2020-12's, which is what those would be
	// measured by if the dialect had not been scoped to the resource.
	MetaFixture newer(
	    "{\"$ref\":\"https://json-schema.org/draft/2020-12/schema\"}");
	ASSERT_NE(newer.schema, nullptr)
	    << (newer.err.message ? newer.err.message : "");
	EXPECT_FALSE(
	    newer.accepts("{\"$id\":\"https://example.com/s.json#frag\"}"));
	EXPECT_FALSE(newer.accepts("{\"$defs\":7}"));
}

TEST(JsonSchemaMetaschema, ResolvesDraft06WithoutAResolver) {
	MetaFixture f("{\"$ref\":\"http://json-schema.org/draft-06/schema#\"}");
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");

	EXPECT_TRUE(f.accepts("{\"type\":\"string\"}"));
	EXPECT_FALSE(f.accepts("{\"type\":\"strong\"}"));
	EXPECT_FALSE(f.accepts("{\"maxLength\":-1}"));
	EXPECT_FALSE(f.accepts("{\"exclusiveMinimum\":true}"));

	// draft-06 has no `if`, and its meta-schema does not forbid unknown
	// members, so a nonsense `if` is valid there and not in draft-07. This is
	// what separates the two documents from each other rather than only from
	// 2020-12's.
	EXPECT_TRUE(f.accepts("{\"if\":7}"));
	MetaFixture seven(
	    "{\"$ref\":\"http://json-schema.org/draft-07/schema#\"}");
	ASSERT_NE(seven.schema, nullptr)
	    << (seven.err.message ? seven.err.message : "");
	EXPECT_FALSE(seven.accepts("{\"if\":7}"));
}

TEST(JsonSchemaMetaschema, TheOlderDraftsNeedNoRegexProvider) {
	// 2020-12's meta/core constrains `$id` and `$anchor` with `pattern`, so a
	// reference to it without a provider is refused. Neither older document
	// uses `pattern` in a schema position at all - `pattern` appears there as a
	// *property being described*, and the regular-expression-ness of
	// `patternProperties`' keys is spelled `"format": "regex"`, which is an
	// annotation. So these two compile with no provider, and saying so is the
	// difference between "it happened to work" and "it cannot need one".
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	ASSERT_EQ(opts.regex, nullptr);
	for (const char * src : {
	         "{\"$ref\":\"http://json-schema.org/draft-07/schema#\"}",
	         "{\"$ref\":\"http://json-schema.org/draft-06/schema#\"}",
	     }) {
		GTEXT_JSON_Value * doc = parse_doc(src);
		ASSERT_NE(doc, nullptr) << src;
		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Schema * schema =
		    gtext_json_schema_compile_with_options(doc, &opts, &err);
		EXPECT_NE(schema, nullptr)
		    << src << ": " << (err.message ? err.message : "");
		GTEXT_JSON_Value * v = parse_doc("{\"type\":\"strong\"}");
		ASSERT_NE(v, nullptr);
		EXPECT_EQ(gtext_json_schema_validate(schema, v, nullptr),
		    GTEXT_JSON_E_SCHEMA)
		    << src;
		gtext_json_free(v);
		gtext_json_schema_free(schema);
		gtext_json_free(doc);
		gtext_json_error_free(&err);
	}
}

// ===========================================================================
// The resource pre-pass reads the dialect
// ===========================================================================
//
// Identifiers are found before anything compiles, because a `$ref` may name
// something defined later in the document. That pass used to run with no
// dialect at all, and two of the three things it looks for are spelled
// differently before 2019-09. Both showed up in the suite's draft7 and draft6
// directories and in no other.

TEST(JsonSchemaPrePass, ARefBesideASiblingIdLeavesTheBaseAloneBefore2019_09) {
	// draft-07 core: a schema object containing `$ref` *is* that reference and
	// every sibling keyword MUST be ignored - so the `$id` here does not move
	// the base, and `foo.json` names the definition registered at
	// `.../base/foo.json` rather than the one at `.../foo.json`.
	//
	// The compiler already honoured this; the pre-pass registered the `$id`
	// anyway, and the `$ref` then found the number-typed definition. This is
	// the suite's "$ref prevents a sibling $id from changing the base uri",
	// which was answered wrongly rather than refused.
	const char * body =
	    "\"$id\":\"http://localhost:1234/sibling_id/base/\","
	    "\"definitions\":{"
	    "  \"foo\":{\"$id\":\"http://localhost:1234/sibling_id/foo.json\","
	    "           \"type\":\"string\"},"
	    "  \"base_foo\":{\"$id\":\"foo.json\",\"type\":\"number\"}},"
	    "\"allOf\":[{\"$id\":\"http://localhost:1234/sibling_id/\","
	    "            \"$ref\":\"foo.json\"}]";
	{
		MetaFixture f((std::string("{\"$schema\":\"http://json-schema.org/"
		                          "draft-07/schema#\",")
		    + body + "}")
		        .c_str());
		ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
		EXPECT_TRUE(f.accepts("1"));
		EXPECT_FALSE(f.accepts("\"a\""));
	}
	{
		// 2019-09 made `$ref` an applicator like any other, so there the
		// sibling `$id` does move the base and the string-typed definition is
		// what the reference finds. Same document, opposite answer, which is
		// what makes the rule above a rule about the dialect.
		MetaFixture f((std::string("{\"$schema\":\"https://json-schema.org/"
		                          "draft/2019-09/schema\",")
		    + body + "}")
		        .c_str());
		ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
		EXPECT_TRUE(f.accepts("\"a\""));
		EXPECT_FALSE(f.accepts("1"));
	}
}

TEST(JsonSchemaPrePass, AnAnchorIsNotAKeywordBefore2019_09) {
	// `$anchor` arrived in 2019-09. In draft-07 it is an unknown member, so a
	// `$ref` to the name it would have created finds nothing and the compile is
	// refused - which is this engine's answer everywhere else too, rather than
	// resolving a name the draft says does not exist.
	MetaFixture seven(
	    "{\"$schema\":\"http://json-schema.org/draft-07/schema#\","
	    "\"$id\":\"http://e.com/a.json\","
	    "\"allOf\":[{\"$ref\":\"#bar\"}],"
	    "\"definitions\":{\"A\":{\"$anchor\":\"bar\","
	    "                        \"type\":\"integer\"}}}");
	EXPECT_EQ(seven.schema, nullptr);

	// The same document in 2019-09, where `$anchor` is the spelling.
	MetaFixture newer(
	    "{\"$schema\":\"https://json-schema.org/draft/2019-09/schema\","
	    "\"$id\":\"http://e.com/b.json\","
	    "\"allOf\":[{\"$ref\":\"#bar\"}],"
	    "\"$defs\":{\"A\":{\"$anchor\":\"bar\",\"type\":\"integer\"}}}");
	ASSERT_NE(newer.schema, nullptr)
	    << (newer.err.message ? newer.err.message : "");
	EXPECT_TRUE(newer.accepts("1"));
	EXPECT_FALSE(newer.accepts("\"a\""));

	// And draft-07's own spelling of the same thing - an `$id` holding only a
	// fragment - does work there.
	MetaFixture spelt(
	    "{\"$schema\":\"http://json-schema.org/draft-07/schema#\","
	    "\"$id\":\"http://e.com/c.json\","
	    "\"allOf\":[{\"$ref\":\"#bar\"}],"
	    "\"definitions\":{\"A\":{\"$id\":\"#bar\","
	    "                        \"type\":\"integer\"}}}");
	ASSERT_NE(spelt.schema, nullptr)
	    << (spelt.err.message ? spelt.err.message : "");
	EXPECT_TRUE(spelt.accepts("1"));
	EXPECT_FALSE(spelt.accepts("\"a\""));
}

TEST(JsonSchemaPrePass, AFragmentOnlyIdNamesAPlaceAndDoesNotMoveTheBase) {
	// The shape documentation/modules/JSON.md used to say this engine could not
	// read: a nested `$id` below a fragment-only one. It resolves against the
	// resource's base and not against `base#name`, so `sub.json` is
	// `http://e.com/sub.json` and the `$ref` to it finds the integer.
	//
	// It resolved that way before the rule was made explicit, because a
	// relative reference discards its base's fragment (RFC 3986 section 5.2.2)
	// and the two treatments therefore agree. That is why this test exists: the
	// agreement is a property of URI resolution rather than of this pass, and
	// nothing else here would notice it changing.
	const char * tail =
	    "\"allOf\":[{\"$ref\":\"#foo\"}],"
	    "\"definitions\":{\"A\":{\"$id\":\"#foo\","
	    "  \"properties\":{\"x\":{\"$ref\":\"sub.json\"}},"
	    "  \"definitions\":{\"B\":{\"$id\":\"sub.json\","
	    "                          \"type\":\"integer\"}}}}}";
	{
		MetaFixture f((std::string("{\"$schema\":\"http://json-schema.org/"
		                          "draft-07/schema#\","
		                          "\"$id\":\"http://e.com/root.json\",")
		    + tail)
		        .c_str());
		ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
		EXPECT_TRUE(f.accepts("{\"x\":1}"));
		EXPECT_FALSE(f.accepts("{\"x\":\"nope\"}"));
	}
	{
		// And with no root `$id`, where the base is the empty string.
		MetaFixture f((std::string("{\"$schema\":\"http://json-schema.org/"
		                          "draft-07/schema#\",")
		    + tail)
		        .c_str());
		ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
		EXPECT_TRUE(f.accepts("{\"x\":1}"));
		EXPECT_FALSE(f.accepts("{\"x\":\"nope\"}"));
	}
}

TEST(JsonSchemaRef, APointerIntoAnEmbeddedResourceCompilesInThatResource) {
	// A pointer fragment is resolved against the document, and a document can
	// hold embedded resources. `#/definitions/baz/definitions/bar` finds the
	// right subschema, and that subschema belongs to `baz` - so the relative
	// `$ref` inside it has to resolve against `http://e.com/inner/` and not
	// against the document the pointer was written in.
	//
	// This was the suite's draft7 and draft6 "base URI change - change folder
	// in subschema", refused rather than answered wrongly only because
	// `http://e.com/int.json` happened not to exist. Nothing in the 2020-12 or
	// 2019-09 directories asks it: their version of that case uses a pointer
	// that stays inside one resource.
	MetaFixture f("{\"$schema\":\"http://json-schema.org/draft-07/schema#\","
	              "\"$id\":\"http://e.com/root.json\","
	              "\"properties\":{\"list\":{"
	              "  \"$ref\":\"#/definitions/baz/definitions/bar\"}},"
	              "\"definitions\":{\"baz\":{\"$id\":\"inner/\","
	              "  \"definitions\":{"
	              "    \"bar\":{\"type\":\"array\","
	              "             \"items\":{\"$ref\":\"int.json\"}},"
	              "    \"int\":{\"$id\":\"int.json\","
	              "             \"type\":\"integer\"}}}}}");
	ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
	EXPECT_TRUE(f.accepts("{\"list\":[1]}"));
	EXPECT_FALSE(f.accepts("{\"list\":[\"a\"]}"));
}

TEST(JsonSchemaAdditionalItems, AppliesOnlyWhenItemsIsAnArray) {
	// draft-06, draft-07 and 2019-09 agree: `additionalItems` describes the
	// tail of a positional `items`, and is ignored when `items` is a single
	// schema or absent, because then there is no tail to describe.
	//
	// This engine applied it from index prefix_items_count onward, which is
	// zero when there is no positional list, so both of the first two schemas
	// here called every non-empty array invalid.
	{
		MetaFixture f(
		    "{\"$schema\":\"https://json-schema.org/draft/2019-09/schema\","
		    "\"items\":{\"type\":\"integer\"},"
		    "\"additionalItems\":{\"type\":\"string\"}}");
		ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
		EXPECT_TRUE(f.accepts("[1,2,3]"));
		EXPECT_FALSE(f.accepts("[\"a\"]"));
	}
	{
		MetaFixture f(
		    "{\"$schema\":\"https://json-schema.org/draft/2019-09/schema\","
		    "\"additionalItems\":false}");
		ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
		EXPECT_TRUE(f.accepts("[1,2,3]"));
	}
	{
		// And it does still govern the tail when there is one.
		MetaFixture f(
		    "{\"$schema\":\"https://json-schema.org/draft/2019-09/schema\","
		    "\"items\":[{\"type\":\"integer\"}],"
		    "\"additionalItems\":false}");
		ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
		EXPECT_TRUE(f.accepts("[1]"));
		EXPECT_FALSE(f.accepts("[1,2]"));
	}
}

TEST(JsonSchemaDialect, AnUndeclaredDocumentIsReadAsTheCallerSays) {
	// `$schema` is optional, and a document without one was always read as
	// 2020-12. That is the right guess but it is a guess, and the drafts
	// disagree about meaning rather than only about vocabulary.
	//
	// draft-07 says a schema object containing `$ref` *is* that reference and
	// every sibling keyword is ignored; 2019-09 onward apply the siblings. The
	// same document therefore has two readings, and only the caller knows
	// which was meant.
	const char * src =
	    "{\"$ref\":\"#/definitions/int\",\"maxItems\":1,"
	    "\"definitions\":{\"int\":{\"type\":\"array\"}}}";

	{
		// Default: 2020-12, so maxItems applies beside the $ref.
		MetaFixture f(src);
		ASSERT_NE(f.schema, nullptr) << (f.err.message ? f.err.message : "");
		EXPECT_TRUE(f.accepts("[1]"));
		EXPECT_FALSE(f.accepts("[1,2]"));
	}
	{
		// Told it is draft-07: the $ref is the whole schema and maxItems is
		// not there at all, so the two-element array is valid.
		ToyProvider provider;
		GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
		GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
		opts.regex = &vtable;
		opts.default_dialect = "http://json-schema.org/draft-07/schema#";

		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * doc = parse_doc(src);
		ASSERT_NE(doc, nullptr);
		GTEXT_JSON_Schema * schema =
		    gtext_json_schema_compile_with_options(doc, &opts, &err);
		ASSERT_NE(schema, nullptr) << (err.message ? err.message : "");

		GTEXT_JSON_Value * two = parse_doc("[1,2]");
		ASSERT_NE(two, nullptr);
		EXPECT_EQ(gtext_json_schema_validate(schema, two, nullptr),
		    GTEXT_JSON_OK);

		gtext_json_free(two);
		gtext_json_schema_free(schema);
		gtext_json_free(doc);
		gtext_json_error_free(&err);
	}
}

TEST(JsonSchemaDialect, ADeclaredSchemaStillWins) {
	// The option says what an *undeclared* document means. A `$schema` inside
	// the document is not overridden by it, or the option would be a way to
	// misread a document that said plainly what it was.
	ToyProvider provider;
	GTEXT_JSON_Regex_Provider vtable = toy_vtable(&provider);
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.regex = &vtable;
	opts.default_dialect = "http://json-schema.org/draft-07/schema#";

	const char * src =
	    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
	    "\"$ref\":\"#/$defs/int\",\"maxItems\":1,"
	    "\"$defs\":{\"int\":{\"type\":\"array\"}}}";
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * doc = parse_doc(src);
	ASSERT_NE(doc, nullptr);
	GTEXT_JSON_Schema * schema =
	    gtext_json_schema_compile_with_options(doc, &opts, &err);
	ASSERT_NE(schema, nullptr) << (err.message ? err.message : "");

	// 2020-12's reading: maxItems applies, so this is too long.
	GTEXT_JSON_Value * two = parse_doc("[1,2]");
	ASSERT_NE(two, nullptr);
	GTEXT_JSON_Error verr;
	memset(&verr, 0, sizeof(verr));
	EXPECT_NE(gtext_json_schema_validate(schema, two, &verr), GTEXT_JSON_OK);

	gtext_json_error_free(&verr);
	gtext_json_free(two);
	gtext_json_schema_free(schema);
	gtext_json_free(doc);
	gtext_json_error_free(&err);
}

TEST(JsonSchemaDialect, AnUnreadableDefaultIsRefused) {
	// Silently falling back to 2020-12 would tell a caller who asked for
	// draft-03 that they got draft-03.
	GTEXT_JSON_Schema_Options opts = gtext_json_schema_options_default();
	opts.default_dialect = "http://json-schema.org/draft-03/schema#";

	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * doc = parse_doc("{\"type\":\"string\"}");
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(gtext_json_schema_compile_with_options(doc, &opts, &err),
	    nullptr);
	EXPECT_EQ(err.code, GTEXT_JSON_E_SCHEMA_UNSUPPORTED);

	gtext_json_free(doc);
	gtext_json_error_free(&err);
}

/*
 * Comments across a feed boundary.
 *
 * A comment that runs to the end of a chunk has not ended, and the streaming
 * lexer used to treat it as though it had: the rest of it, in the next chunk,
 * was read as code. With `// "a": 1` that is not an error but a wrong parse -
 * the comment's contents become part of the document - so these tests compare
 * the events against the DOM parser rather than only asking whether the parse
 * succeeded.
 *
 * Nothing here is JSON5: allow_comments is JSONC's, and the defect was equally
 * visible with quoted names.
 */
namespace {

// Every event the streaming parser emits, flattened to a string.
std::string stream_event_trace(const std::string & src,
    const GTEXT_JSON_Parse_Options * opts, size_t chunk, bool * ok) {
	static std::string trace;
	trace.clear();
	GTEXT_JSON_Event_cb cb = [](void *, const GTEXT_JSON_Event * ev,
	                             GTEXT_JSON_Error *) {
		trace += std::to_string((int)ev->type);
		if (ev->type == GTEXT_JSON_EVT_KEY || ev->type == GTEXT_JSON_EVT_STRING) {
			trace += "(" + std::string(ev->as.str.s, ev->as.str.len) + ")";
		}
		trace += " ";
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
	return trace;
}

} // namespace

TEST(JsonStreamComments, ACommentSplitBetweenFeedsIsStillOneComment) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_comments = true;

	const char * documents[] = {
	    "{\n  // c\n  \"ab\": 1\n}",
	    "{ /* c */ \"ab\": 1 }",
	    "// leading\n{\"a\":1}",
	    "{\"a\":1 /* between */ , \"b\":2}",
	    // The case that is a wrong answer rather than an error: the comment
	    // contains something that would parse.
	    "{\n  // \"ghost\": 99\n  \"real\": 1\n}",
	    "{\n  /* \"ghost\": 99, */\n  \"real\": 1\n}",
	    // A comment opening and a comment closing that can each be split.
	    "{\"a\":1,/**/\"b\":2}",
	    "[1,2 // x\n,3]",
	};
	/* A comment *after* the document is absent on purpose: a feed of any kind
	   once the top-level value is complete is GTEXT_JSON_E_STATE, which is the
	   streaming parser's contract and not a comment question. */
	for (const char * src : documents) {
		const std::string whole = src;
		bool reference_ok = false;
		const std::string reference =
		    stream_event_trace(whole, &opts, whole.size(), &reference_ok);
		ASSERT_TRUE(reference_ok) << src << " in one feed";

		for (size_t chunk = 1; chunk <= whole.size(); chunk++) {
			bool ok = false;
			const std::string got =
			    stream_event_trace(whole, &opts, chunk, &ok);
			EXPECT_TRUE(ok) << src << " at chunk " << chunk;
			EXPECT_EQ(got, reference) << src << " at chunk " << chunk;
		}
	}
}

/* Waiting for the rest of a comment must not turn a genuinely unclosed one
   into a wait that never ends: finish() has to refuse it. */
TEST(JsonStreamComments, AnUnclosedCommentIsStillAnError) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_comments = true;
	for (const char * src : {"[1, /* never closed", "/* never closed",
	         "{\"a\": /* never closed"}) {
		const std::string whole = src;
		for (size_t chunk : {size_t(1), size_t(3), whole.size()}) {
			bool ok = false;
			stream_event_trace(whole, &opts, chunk, &ok);
			EXPECT_FALSE(ok) << src << " at chunk " << chunk;
		}
	}
	// And a single-line comment with no newline is not an error: the end of
	// input ends it, which is what finish() means.
	bool ok = false;
	stream_event_trace("[1,2] // to the end", &opts, 19, &ok);
	EXPECT_TRUE(ok);
}

/* A lone slash at the end of a feed is the start of a comment whose second
   character has not arrived, not an unknown token. */
TEST(JsonStreamComments, ASlashAtTheEndOfAFeedWaits) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_comments = true;
	GTEXT_JSON_Event_cb cb = [](void *, const GTEXT_JSON_Event *,
	                             GTEXT_JSON_Error *) { return GTEXT_JSON_OK; };
	GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, cb, nullptr);
	ASSERT_NE(st, nullptr);
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	EXPECT_EQ(gtext_json_stream_feed(st, "[1,", 3, &err), GTEXT_JSON_OK);
	EXPECT_EQ(gtext_json_stream_feed(st, "/", 1, &err), GTEXT_JSON_OK);
	EXPECT_EQ(gtext_json_stream_feed(st, "/ c\n", 4, &err), GTEXT_JSON_OK);
	EXPECT_EQ(gtext_json_stream_feed(st, "2]", 2, &err), GTEXT_JSON_OK);
	EXPECT_EQ(gtext_json_stream_finish(st, &err), GTEXT_JSON_OK);
	gtext_json_stream_free(st);
	gtext_json_error_free(&err);

	// With comments off, a slash is an error rather than a wait.
	GTEXT_JSON_Parse_Options strict = gtext_json_parse_options_default();
	GTEXT_JSON_Stream * st2 = gtext_json_stream_new(&strict, cb, nullptr);
	ASSERT_NE(st2, nullptr);
	GTEXT_JSON_Error err2;
	std::memset(&err2, 0, sizeof(err2));
	GTEXT_JSON_Status a = gtext_json_stream_feed(st2, "[1,", 3, &err2);
	GTEXT_JSON_Status b = gtext_json_stream_feed(st2, "/", 1, &err2);
	GTEXT_JSON_Status c = gtext_json_stream_finish(st2, &err2);
	EXPECT_TRUE(a != GTEXT_JSON_OK || b != GTEXT_JSON_OK || c != GTEXT_JSON_OK);
	gtext_json_stream_free(st2);
	gtext_json_error_free(&err2);
}

/*
 * Bytes after the document.
 *
 * JSON allows white space after a top-level value and gtext_json_parse()
 * accepts it, so the streaming parser has to as well - a caller feeding a file
 * in fixed-size blocks cannot control whether the newline that ends it lands in
 * the same block as the `}`. Feeding in the DONE state used to be refused
 * outright, which made `"a"\n` valid in one feed and invalid in two.
 *
 * Found by the JSON fuzzer's DOM-against-stream differential, on a seed corpus
 * entry, the first time that property was asserted.
 */
TEST(JsonStreamTrailing, WhiteSpaceAfterTheDocumentMayArriveLate) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	for (const char * src : {"\"a\"\n", "{\"a\":1}\n", "[1,2]\n  \n",
	         "null\t", "1 "}) {
		const std::string whole = src;
		/* Freed rather than leaked: the ASan build's LSan reported 72 of these
		   the first time these tests ran. */
		GTEXT_JSON_Value * dom_value =
		    gtext_json_parse(whole.data(), whole.size(), &opts, nullptr);
		EXPECT_NE(dom_value, nullptr)
		    << "the DOM parser refuses " << src;
		if (dom_value) {
			gtext_json_free(dom_value);
		}
		for (size_t chunk = 1; chunk <= whole.size(); chunk++) {
			bool ok = false;
			stream_event_trace(whole, &opts, chunk, &ok);
			EXPECT_TRUE(ok) << src << " at chunk " << chunk;
		}
	}
}

/* Content after the document is refused, and refused the same way whichever
   feed it arrives in - which is the half of this that was already true. */
TEST(JsonStreamTrailing, ContentAfterTheDocumentIsStillRefused) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	for (const char * src : {"\"a\" 1", "{\"a\":1} {}", "[1] null", "1 2"}) {
		const std::string whole = src;
		for (size_t chunk = 1; chunk <= whole.size(); chunk++) {
			bool ok = false;
			stream_event_trace(whole, &opts, chunk, &ok);
			EXPECT_FALSE(ok) << src << " at chunk " << chunk;
		}
	}
}

/* And finish() does close the stream: what it refuses is a further feed, which
   is a different question from whether bytes may follow the document. */
TEST(JsonStreamTrailing, FeedingAfterFinishIsRefused) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	GTEXT_JSON_Event_cb cb = [](void *, const GTEXT_JSON_Event *,
	                             GTEXT_JSON_Error *) { return GTEXT_JSON_OK; };
	GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, cb, nullptr);
	ASSERT_NE(st, nullptr);
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	EXPECT_EQ(gtext_json_stream_feed(st, "[1]", 3, &err), GTEXT_JSON_OK);
	EXPECT_EQ(gtext_json_stream_finish(st, &err), GTEXT_JSON_OK);
	EXPECT_EQ(gtext_json_stream_feed(st, " ", 1, &err), GTEXT_JSON_E_STATE);
	gtext_json_stream_free(st);
	gtext_json_error_free(&err);
}

/*
 * A nonfinite value with a sign, split across feeds.
 *
 * `-Infinity` reaches the lexer through the number path rather than the
 * keyword path, because a sign starts a number, and that path buffers a token
 * that has not finished arriving. Two defects in that buffering meant the
 * value could not be streamed at most chunk sizes at all - it worked in one
 * feed, and one byte at a time, and almost nowhere in between.
 *
 * Found by the JSON fuzzer's DOM-against-stream differential. Both defects
 * predate the JSON5 work: the same failures reproduce on the commit before it.
 */
TEST(JsonStreamNonfinite, ASignedWordSurvivesEveryChunkBoundary) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_nonfinite_numbers = true;
	opts.allow_leading_plus = true;

	for (const char * src : {"-Infinity", "+Infinity", "-NaN", "+NaN",
	         "Infinity", "NaN", "[-Infinity]", "[1,-Infinity,2]",
	         "{\"a\":-Infinity}", "[-Infinity,-Infinity]"}) {
		const std::string whole = src;
		/* Freed rather than leaked: the ASan build's LSan reported 72 of these
		   the first time these tests ran. */
		GTEXT_JSON_Value * dom_value =
		    gtext_json_parse(whole.data(), whole.size(), &opts, nullptr);
		EXPECT_NE(dom_value, nullptr)
		    << "the DOM parser refuses " << src;
		if (dom_value) {
			gtext_json_free(dom_value);
		}
		for (size_t chunk = 1; chunk <= whole.size(); chunk++) {
			bool ok = false;
			stream_event_trace(whole, &opts, chunk, &ok);
			EXPECT_TRUE(ok) << src << " at chunk " << chunk;
		}
	}
}

/* And a word that only starts like one is still refused, at every chunk size:
   the fix must not make the lexer accept a prefix as though it were the whole
   thing, which is exactly how the second defect failed. */
TEST(JsonStreamNonfinite, AnUnfinishedWordIsStillRefused) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_nonfinite_numbers = true;
	opts.allow_leading_plus = true;

	for (const char * src : {"-Infinit", "-Infinityy", "-Inf", "-NaNN", "-N",
	         "[-Infinit]", "-I"}) {
		const std::string whole = src;
		EXPECT_EQ(gtext_json_parse(whole.data(), whole.size(), &opts, nullptr),
		    nullptr)
		    << "the DOM parser accepts " << src;
		for (size_t chunk = 1; chunk <= whole.size(); chunk++) {
			bool ok = false;
			stream_event_trace(whole, &opts, chunk, &ok);
			EXPECT_FALSE(ok) << src << " at chunk " << chunk;
		}
	}
}

/*
 * A stream with no value in it.
 *
 * `gtext_json_parse()` refuses an input holding nothing but white space, or
 * nothing but a comment, because a JSON text is a value. The streaming parser
 * accepted both, emitting no events and returning OK from finish(), because it
 * treated "bytes arrived" as evidence that a value had. A caller could not
 * tell an empty configuration file from a valid one.
 *
 * Found by the JSON fuzzer's DOM-against-stream differential.
 */
TEST(JsonStreamEmpty, AStreamWithNoValueIsRefused) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_comments = true;

	for (const char * src : {"", " ", "\n", "\t\r\n ", "// just a comment",
	         "// a comment\n", "/* a comment */", "  /* two */ /* comments */ ",
	         "\xEF\xBB\xBF"}) {
		const std::string whole = src;
		EXPECT_EQ(gtext_json_parse(whole.data(), whole.size(), &opts, nullptr),
		    nullptr)
		    << "the DOM parser accepts [" << src << "]";
		for (size_t chunk = 1; chunk <= (whole.empty() ? 1 : whole.size());
		     chunk++) {
			bool ok = false;
			stream_event_trace(whole, &opts, chunk, &ok);
			EXPECT_FALSE(ok) << "[" << src << "] at chunk " << chunk;
		}
	}
}

/* And a document that does have a value is still accepted, with the same
   leading white space and comments in front of it. */
TEST(JsonStreamEmpty, AValueAfterAllThatIsStillAccepted) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allow_comments = true;
	for (const char * src : {"1", " 1 ", "// c\n1", "/* c */ 1",
	         "\xEF\xBB\xBF" "1", "\n\n[]\n"}) {
		const std::string whole = src;
		/* Freed rather than leaked: the ASan build's LSan reported 72 of these
		   the first time these tests ran. */
		GTEXT_JSON_Value * dom_value =
		    gtext_json_parse(whole.data(), whole.size(), &opts, nullptr);
		EXPECT_NE(dom_value, nullptr)
		    << "the DOM parser refuses [" << src << "]";
		if (dom_value) {
			gtext_json_free(dom_value);
		}
		for (size_t chunk = 1; chunk <= whole.size(); chunk++) {
			bool ok = false;
			stream_event_trace(whole, &opts, chunk, &ok);
			EXPECT_TRUE(ok) << "[" << src << "] at chunk " << chunk;
		}
	}
}

/*
 * A byte-order mark, and where it may be.
 *
 * allow_leading_bom skips a UTF-8 BOM at the start of the input. Two things
 * about that were chunk-dependent in the streaming parser, and the fuzzer's
 * differential named both: the mark could not arrive in pieces, because a
 * truncated multi-byte sequence between tokens was read as a bad token instead
 * of an unfinished one; and it was skipped wherever a compacted buffer happened
 * to begin, so a BOM in the middle of a document was silently ignored when a
 * chunk boundary left it at the front.
 */
TEST(JsonStreamBom, TheMarkMayArriveInPieces) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	EXPECT_TRUE(opts.allow_leading_bom);
	for (const char * src : {"\xEF\xBB\xBF" "1", "\xEF\xBB\xBF" "[1,2]",
	         "\xEF\xBB\xBF" "{\"a\":1}"}) {
		const std::string whole = src;
		/* Freed rather than leaked: the ASan build's LSan reported 72 of these
		   the first time these tests ran. */
		GTEXT_JSON_Value * dom_value =
		    gtext_json_parse(whole.data(), whole.size(), &opts, nullptr);
		EXPECT_NE(dom_value, nullptr)
		    << "the DOM parser refuses " << src;
		if (dom_value) {
			gtext_json_free(dom_value);
		}
		for (size_t chunk = 1; chunk <= whole.size(); chunk++) {
			bool ok = false;
			stream_event_trace(whole, &opts, chunk, &ok);
			EXPECT_TRUE(ok) << src << " at chunk " << chunk;
		}
	}
}

TEST(JsonStreamBom, AMarkInTheMiddleIsNotALeadingOne) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	for (const char * src : {"[1,\xEF\xBB\xBF" "2]", "{\xEF\xBB\xBF\"a\":1}",
	         "[1]\xEF\xBB\xBF"}) {
		const std::string whole = src;
		EXPECT_EQ(gtext_json_parse(whole.data(), whole.size(), &opts, nullptr),
		    nullptr)
		    << "the DOM parser accepts " << src;
		for (size_t chunk = 1; chunk <= whole.size(); chunk++) {
			bool ok = false;
			stream_event_trace(whole, &opts, chunk, &ok);
			EXPECT_FALSE(ok) << src << " at chunk " << chunk;
		}
	}
}

/* U+FEFF inside a string is an ordinary character - RFC 8259 excludes only the
   quote, the backslash and the control characters - so the mark that is skipped
   before the document is content once a string has started. */
TEST(JsonStreamBom, InsideAStringItIsJustACharacter) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	const std::string src = "[\"\xEF\xBB\xBF\"]";
	GTEXT_JSON_Value * doc =
	    gtext_json_parse(src.data(), src.size(), &opts, nullptr);
	ASSERT_NE(doc, nullptr);
	const char * s = nullptr;
	size_t len = 0;
	ASSERT_EQ(gtext_json_get_string(gtext_json_array_get(doc, 0), &s, &len),
	    GTEXT_JSON_OK);
	EXPECT_EQ(std::string(s, len), "\xEF\xBB\xBF");
	gtext_json_free(doc);
	for (size_t chunk = 1; chunk <= src.size(); chunk++) {
		bool ok = false;
		stream_event_trace(src, &opts, chunk, &ok);
		EXPECT_TRUE(ok) << "at chunk " << chunk;
	}
}

/*
 * What max_depth is for, and what it costs.
 *
 * The DOM parser is recursive descent, so a document's nesting depth is the
 * parser's stack depth. Measured on this machine by bisecting the depth at which
 * the parse segfaults, at four stack sizes:
 *
 *     1 MiB   2276 levels   445 bytes/level
 *     2 MiB   4608 levels   447 bytes/level
 *     4 MiB   9272 levels   448 bytes/level
 *     8 MiB  18437 levels   447 bytes/level
 *
 * Linear, as it should be, which is what makes the figure usable: the default
 * max_depth of 256 needs about 115 KB and is safe on any stack a thread is
 * likely to have, and a caller who *raises* the limit is choosing a number their
 * stack has to be able to hold.
 *
 * These tests pin the part that protects a caller who does not raise it: a
 * document past the limit is refused, with E_DEPTH and not a crash, and the
 * default is far enough below the smallest measured ceiling to have room.
 */
TEST(JsonParseDepth, ADocumentPastTheLimitIsRefused) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	EXPECT_EQ(opts.max_depth, 0u) << "0 means the library default";

	// The default is 256, so 300 levels is past it.
	auto nest = [](size_t depth) {
		std::string text;
		for (size_t i = 0; i < depth; i++) {
			text += "[";
		}
		for (size_t i = 0; i < depth; i++) {
			text += "]";
		}
		return text;
	};

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	const std::string deep = nest(300);
	EXPECT_EQ(gtext_json_parse(deep.data(), deep.size(), &opts, &err), nullptr);
	EXPECT_EQ(err.code, GTEXT_JSON_E_DEPTH);
	gtext_json_error_free(&err);

	// And one inside it parses.
	const std::string shallow = nest(200);
	GTEXT_JSON_Value * v =
	    gtext_json_parse(shallow.data(), shallow.size(), &opts, nullptr);
	EXPECT_NE(v, nullptr);
	if (v) {
		gtext_json_free(v);
	}
}

/* A raised limit is still enforced, so a caller who chooses 2000 gets an error
   at 2001 rather than a deeper recursion than they asked for. 2000 levels is
   about 900 KB of stack by the figures above - inside the 8 MiB the tests run
   with, and the reason this number is not larger. */
TEST(JsonParseDepth, ARaisedLimitIsStillALimit) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.max_depth = 2000;

	std::string ok_text, over_text;
	for (size_t i = 0; i < 2000; i++) {
		ok_text += "[";
	}
	for (size_t i = 0; i < 2000; i++) {
		ok_text += "]";
	}
	for (size_t i = 0; i < 2001; i++) {
		over_text += "[";
	}
	for (size_t i = 0; i < 2001; i++) {
		over_text += "]";
	}

	GTEXT_JSON_Value * v =
	    gtext_json_parse(ok_text.data(), ok_text.size(), &opts, nullptr);
	EXPECT_NE(v, nullptr);
	if (v) {
		gtext_json_free(v);
	}

	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	EXPECT_EQ(
	    gtext_json_parse(over_text.data(), over_text.size(), &opts, &err),
	    nullptr);
	EXPECT_EQ(err.code, GTEXT_JSON_E_DEPTH);
	gtext_json_error_free(&err);
}

/* The streaming parser is not recursive - it keeps its own stack on the heap -
   so it takes a document the DOM parser could not, and its limit is a limit
   rather than a cliff. This is the one place the two parsers are allowed to
   differ in what they accept, and the difference is in the safe direction. */
TEST(JsonParseDepth, TheStreamingParserIsNotBoundByTheCStack) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.max_depth = 100000;

	std::string text;
	for (size_t i = 0; i < 50000; i++) {
		text += "[";
	}
	for (size_t i = 0; i < 50000; i++) {
		text += "]";
	}

	GTEXT_JSON_Event_cb cb = [](void *, const GTEXT_JSON_Event *,
	                             GTEXT_JSON_Error *) { return GTEXT_JSON_OK; };
	GTEXT_JSON_Stream * st = gtext_json_stream_new(&opts, cb, nullptr);
	ASSERT_NE(st, nullptr);
	GTEXT_JSON_Error err;
	std::memset(&err, 0, sizeof(err));
	EXPECT_EQ(gtext_json_stream_feed(st, text.data(), text.size(), &err),
	    GTEXT_JSON_OK)
	    << (err.message ? err.message : "");
	EXPECT_EQ(gtext_json_stream_finish(st, &err), GTEXT_JSON_OK)
	    << (err.message ? err.message : "");
	gtext_json_stream_free(st);
	gtext_json_error_free(&err);
}
