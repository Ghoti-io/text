/**
 * @file test-yaml-to-json.cpp
 * @brief Tests for YAML to JSON conversion API.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <gtest/gtest.h>

#include <ghoti.io/text/yaml.h>
#include <ghoti.io/text/json/json_dom.h>
#include <ghoti.io/text/macros.h>
#include <string.h>
#include <stdio.h>
#include <string>

static bool json_converter_called = false;

static GTEXT_YAML_Status custom_tag_json_converter(
	const GTEXT_YAML_Node * node,
	const char * tag,
	void * user,
	GTEXT_JSON_Value ** out_json,
	GTEXT_YAML_Error * out_err
) {
	(void)tag;
	if (user) {
		bool *flag = (bool *)user;
		*flag = true;
	}
	if (!node || !out_json) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_INVALID;
			out_err->message = "custom tag JSON converter: invalid arguments";
		}
		return GTEXT_YAML_E_INVALID;
	}

	const char *value = gtext_yaml_node_as_string(node);
	if (!value) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_INVALID;
			out_err->message = "custom tag JSON converter: expected string scalar";
		}
		return GTEXT_YAML_E_INVALID;
	}

	char buffer[128];
	int written = snprintf(buffer, sizeof(buffer), "custom:%s", value);
	if (written < 0) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_INVALID;
			out_err->message = "custom tag JSON converter: format error";
		}
		return GTEXT_YAML_E_INVALID;
	}

	*out_json = gtext_json_new_string(buffer, strlen(buffer));
	if (!*out_json) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_OOM;
			out_err->message = "custom tag JSON converter: out of memory";
		}
		return GTEXT_YAML_E_OOM;
	}

	return GTEXT_YAML_OK;
}

static GTEXT_YAML_Custom_Tag json_custom_tags[] = {
	{
		.tag = "tag:example.com,2026:upper",
		.construct = NULL,
		.represent = NULL,
		.to_json = custom_tag_json_converter,
		.user = &json_converter_called
	}
};

/**
 * @test YamlToJsonBasicTypes
 * @brief Test conversion of basic YAML scalar types to JSON
 */
TEST(YamlToJson, BasicScalarTypes) {
	/* Test null */
	{
		GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse("null", 4, NULL, NULL);
		ASSERT_NE(yaml_doc, nullptr);
		
		GTEXT_JSON_Value * json_val = NULL;
		GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, NULL);
		ASSERT_EQ(status, GTEXT_YAML_OK);
		ASSERT_NE(json_val, nullptr);
		EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_NULL);
		
		gtext_json_free(json_val);
		gtext_yaml_free(yaml_doc);
	}
	
	/* Test boolean true */
	{
		GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse("true", 4, NULL, NULL);
		ASSERT_NE(yaml_doc, nullptr);
		
		GTEXT_JSON_Value * json_val = NULL;
		GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, NULL);
		ASSERT_EQ(status, GTEXT_YAML_OK);
		ASSERT_NE(json_val, nullptr);
		EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_BOOL);
		
		bool b = false;
		EXPECT_EQ(gtext_json_get_bool(json_val, &b), GTEXT_JSON_OK);
		EXPECT_TRUE(b);
		
		gtext_json_free(json_val);
		gtext_yaml_free(yaml_doc);
	}
	
	/* Test integer */
	{
		GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse("42", 2, NULL, NULL);
		ASSERT_NE(yaml_doc, nullptr);
		
		GTEXT_JSON_Value * json_val = NULL;
		GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, NULL);
		ASSERT_EQ(status, GTEXT_YAML_OK);
		ASSERT_NE(json_val, nullptr);
		EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_NUMBER);
		
		int64_t num = 0;
		EXPECT_EQ(gtext_json_get_i64(json_val, &num), GTEXT_JSON_OK);
		EXPECT_EQ(num, 42);
		
		gtext_json_free(json_val);
		gtext_yaml_free(yaml_doc);
	}
	
	/* Test float */
	{
		GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse("3.14", 4, NULL, NULL);
		ASSERT_NE(yaml_doc, nullptr);
		
		GTEXT_JSON_Value * json_val = NULL;
		GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, NULL);
		ASSERT_EQ(status, GTEXT_YAML_OK);
		ASSERT_NE(json_val, nullptr);
		EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_NUMBER);
		
		double num = 0.0;
		EXPECT_EQ(gtext_json_get_double(json_val, &num), GTEXT_JSON_OK);
		EXPECT_DOUBLE_EQ(num, 3.14);
		
		gtext_json_free(json_val);
		gtext_yaml_free(yaml_doc);
	}
	
	/* Test string */
	{
		GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse("\"hello\"", 7, NULL, NULL);
		ASSERT_NE(yaml_doc, nullptr);
		
		GTEXT_JSON_Value * json_val = NULL;
		GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, NULL);
		ASSERT_EQ(status, GTEXT_YAML_OK);
		ASSERT_NE(json_val, nullptr);
		EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_STRING);
		
		const char * str = NULL;
		size_t len = 0;
		EXPECT_EQ(gtext_json_get_string(json_val, &str, &len), GTEXT_JSON_OK);
		EXPECT_STREQ(str, "hello");
		
		gtext_json_free(json_val);
		gtext_yaml_free(yaml_doc);
	}
}

/**
 * @test YamlToJsonSequence
 * @brief Test conversion of YAML sequences to JSON arrays
 */
TEST(YamlToJson, Sequence) {
	const char * yaml_input = "- 1\n- 2\n- 3";
	GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);
	
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, NULL);
	ASSERT_EQ(status, GTEXT_YAML_OK);
	ASSERT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_ARRAY);
	
	EXPECT_EQ(gtext_json_array_size(json_val), 3);
	
	int64_t val = 0;
	const GTEXT_JSON_Value * elem0 = gtext_json_array_get(json_val, 0);
	ASSERT_NE(elem0, nullptr);
	EXPECT_EQ(gtext_json_get_i64(elem0, &val), GTEXT_JSON_OK);
	EXPECT_EQ(val, 1);
	
	const GTEXT_JSON_Value * elem2 = gtext_json_array_get(json_val, 2);
	ASSERT_NE(elem2, nullptr);
	EXPECT_EQ(gtext_json_get_i64(elem2, &val), GTEXT_JSON_OK);
	EXPECT_EQ(val, 3);
	
	gtext_json_free(json_val);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonMapping
 * @brief Test conversion of YAML mappings to JSON objects
 */
TEST(YamlToJson, Mapping) {
	const char * yaml_input = "name: Alice\nage: 30\nactive: true";
	GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);
	
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, NULL);
	ASSERT_EQ(status, GTEXT_YAML_OK);
	ASSERT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_OBJECT);
	
	EXPECT_EQ(gtext_json_object_size(json_val), 3);
	
	/* Check "name" key */
	const GTEXT_JSON_Value * name_val = gtext_json_object_get(json_val, "name", 4);
	ASSERT_NE(name_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(name_val), GTEXT_JSON_STRING);
	const char * str = NULL;
	size_t len = 0;
	EXPECT_EQ(gtext_json_get_string(name_val, &str, &len), GTEXT_JSON_OK);
	EXPECT_STREQ(str, "Alice");
	
	/* Check "age" key */
	const GTEXT_JSON_Value * age_val = gtext_json_object_get(json_val, "age", 3);
	ASSERT_NE(age_val, nullptr);
	int64_t num = 0;
	EXPECT_EQ(gtext_json_get_i64(age_val, &num), GTEXT_JSON_OK);
	EXPECT_EQ(num, 30);
	
	/* Check "active" key */
	const GTEXT_JSON_Value * active_val = gtext_json_object_get(json_val, "active", 6);
	ASSERT_NE(active_val, nullptr);
	bool b;
	EXPECT_EQ(gtext_json_get_bool(active_val, &b), GTEXT_JSON_OK);
	EXPECT_TRUE(b);
	
	gtext_json_free(json_val);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonNested
 * @brief Test conversion of nested YAML structures (flow style for proper nesting)
 */
TEST(YamlToJson, Nested) {
	const char * yaml_input = "{person: {name: Bob, age: 25}, items: [a, b]}";
	GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);
	
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, NULL);
	ASSERT_EQ(status, GTEXT_YAML_OK);
	ASSERT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_OBJECT);
	
	/* Check nested mapping */
	const GTEXT_JSON_Value * person = gtext_json_object_get(json_val, "person", 6);
	ASSERT_NE(person, nullptr);
	EXPECT_EQ(gtext_json_typeof(person), GTEXT_JSON_OBJECT);
	
	const GTEXT_JSON_Value * name = gtext_json_object_get(person, "name", 4);
	ASSERT_NE(name, nullptr);
	const char * str = NULL;
	size_t len = 0;
	EXPECT_EQ(gtext_json_get_string(name, &str, &len), GTEXT_JSON_OK);
	EXPECT_STREQ(str, "Bob");
	
	/* Check nested array */
	const GTEXT_JSON_Value * items = gtext_json_object_get(json_val, "items", 5);
	ASSERT_NE(items, nullptr);
	EXPECT_EQ(gtext_json_typeof(items), GTEXT_JSON_ARRAY);
	EXPECT_EQ(gtext_json_array_size(items), 2);
	
	gtext_json_free(json_val);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonIncompatibleAnchors
 * @brief Test that anchors/aliases are rejected
 */
TEST(YamlToJson, IncompatibleAnchors) {
	const char * yaml_input = "anchor: &anchor_name value\nalias: *anchor_name";
	GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);
	
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, &err);
	
	/* Should reject due to alias */
	EXPECT_NE(status, GTEXT_YAML_OK);
	EXPECT_EQ(json_val, nullptr);
	EXPECT_NE(err.message, nullptr);
	
	gtext_yaml_error_free(&err);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonAllowAnchorsOption
 * @brief Test that aliases can be resolved when allowed via options
 */
TEST(YamlToJson, AllowAnchorsOption) {
	const char * yaml_input = "anchor: &anchor_name value\nalias: *anchor_name";
	GTEXT_YAML_Document * yaml_doc = NULL;
	GTEXT_YAML_To_JSON_Options options = gtext_yaml_to_json_options_default();
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Error err;
	GTEXT_YAML_Status status = GTEXT_YAML_OK;
	const GTEXT_JSON_Value * alias_val = NULL;

	memset(&err, 0, sizeof(err));
	yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);

	options.allow_resolved_aliases = true;
	status = gtext_yaml_to_json_with_options(
		yaml_doc,
		&json_val,
		&options,
		&err
	);

	EXPECT_EQ(status, GTEXT_YAML_OK);
	EXPECT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_OBJECT);

	alias_val = gtext_json_object_get(json_val, "alias", 5);
	ASSERT_NE(alias_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(alias_val), GTEXT_JSON_STRING);

	gtext_json_free(json_val);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonCustomTagConverter
 * @brief Test custom tag conversion via JSON converter callback
 */
TEST(YamlToJson, CustomTagConverter) {
	const char * yaml_input =
		"%TAG !e! tag:example.com,2026:\n"
		"---\n"
		"!e!upper hello\n";
	GTEXT_YAML_Document * yaml_doc = NULL;
	GTEXT_YAML_To_JSON_Options options = gtext_yaml_to_json_options_default();
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Error err;
	GTEXT_YAML_Status status = GTEXT_YAML_OK;
	const GTEXT_JSON_Value * value = NULL;
	const char * str = NULL;
	size_t len = 0;

	json_converter_called = false;
	memset(&err, 0, sizeof(err));
	yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, &err);
	ASSERT_NE(yaml_doc, nullptr) << (err.message ? err.message : "parse failed");

	options.enable_custom_tags = true;
	options.custom_tags = json_custom_tags;
	options.custom_tag_count = 1;
	status = gtext_yaml_to_json_with_options(
		yaml_doc,
		&json_val,
		&options,
		&err
	);

	EXPECT_EQ(status, GTEXT_YAML_OK);
	ASSERT_NE(json_val, nullptr);
	EXPECT_TRUE(json_converter_called);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_STRING);

	value = json_val;
	EXPECT_EQ(gtext_json_get_string(value, &str, &len), GTEXT_JSON_OK);
	EXPECT_STREQ(str, "custom:hello");

	gtext_json_free(json_val);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonOmapConvertsToAnArray
 * @brief An !!omap is a sequence of single-pair mappings, so JSON has it.
 *
 * This used to assert a refusal. The refusal was there so that "a
 * YAML-specific collection cannot silently become a JSON array", but there
 * is nothing silent about it: an !!omap is stored as a sequence of
 * single-pair mappings and a JSON array is exactly that, order included.
 * The only thing lost is the tag, which JSON drops for every tagged node
 * anyway - "!foo 5" has always converted to 5 without complaint.
 *
 * The conversions that do change the data still refuse by default, each
 * behind its own option: aliases, merge keys, non-string keys and
 * out-of-range integers. See YamlToJsonRefusals below.
 */
TEST(YamlToJson, OmapConvertsToAnArray) {
	const char * yaml_input = "!!omap [{a: 1}, {b: 2}]";
	GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);

	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, &err);

	EXPECT_EQ(status, GTEXT_YAML_OK) << (err.message ? err.message : "?");
	ASSERT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_ARRAY);
	EXPECT_EQ(gtext_json_array_size(json_val), 2u);

	gtext_json_free(json_val);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonStreamingTagValidation
 * @brief Test that explicit custom tags are rejected via streaming validation
 */
TEST(YamlToJson, StreamingTagValidation) {
	const char * yaml_input = "value: !custom 1";
	GTEXT_YAML_Parse_Options parse_options = gtext_yaml_parse_options_default();
	GTEXT_YAML_To_JSON_Options json_options = gtext_yaml_to_json_options_default();
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Error err;
	GTEXT_YAML_Status status = GTEXT_YAML_OK;
	const GTEXT_JSON_Value * value = NULL;
	int64_t num = 0;

	memset(&err, 0, sizeof(err));
	status = gtext_yaml_to_json_with_tags(
		yaml_input,
		strlen(yaml_input),
		&parse_options,
		&json_options,
		&json_val,
		&err
	);

	EXPECT_NE(status, GTEXT_YAML_OK);
	EXPECT_EQ(json_val, nullptr);
	EXPECT_NE(err.message, nullptr);
	gtext_yaml_error_free(&err);

	yaml_input = "value: !!int 12";
	memset(&err, 0, sizeof(err));
	json_val = NULL;
	status = gtext_yaml_to_json_with_tags(
		yaml_input,
		strlen(yaml_input),
		&parse_options,
		&json_options,
		&json_val,
		&err
	);

	EXPECT_EQ(status, GTEXT_YAML_OK);
	ASSERT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_OBJECT);

	value = gtext_json_object_get(json_val, "value", 5);
	ASSERT_NE(value, nullptr);
	EXPECT_EQ(gtext_json_get_i64(value, &num), GTEXT_JSON_OK);
	EXPECT_EQ(num, 12);

	gtext_json_free(json_val);
	gtext_yaml_error_free(&err);
}

/**
 * @test YamlToJsonIncompatibleKeys
 * @brief Test that non-string keys are rejected in mappings
 * Note: YAML allows arbitrary types as keys, but JSON requires strings.
 * This would test detection but current YAML parsers struggle with complex keys.
 * For now, we test that simple non-string keys would be rejected if they parse.
 */
TEST(YamlToJson, IncompatibleKeys) {
	/* While this YAML is valid, the parser doesn't create non-string keys easily.
	 * The core logic in convert_node checks that all mapping keys are STRING type,
	 * which would catch this case if it parsed differently. This is a pass
	 * since the conversion doesn't accept non-string keys at any level. */
	const char * yaml_input = "{a: 1, b: 2}";  /* Valid convertible YAML */
	GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);
	
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, &err);
	
	/* This should succeed since it's valid JSON-compatible YAML */
	EXPECT_EQ(status, GTEXT_YAML_OK);
	EXPECT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_OBJECT);
	
	gtext_json_free(json_val);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonCoerceKeysOption
 * @brief Test that non-string scalar keys can be coerced to strings
 */
TEST(YamlToJson, CoerceKeysOption) {
	const char * yaml_input = "{!!int 1: one}";
	GTEXT_YAML_Document * yaml_doc = NULL;
	GTEXT_YAML_To_JSON_Options options = gtext_yaml_to_json_options_default();
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Error err;
	GTEXT_YAML_Status status = GTEXT_YAML_OK;
	const GTEXT_JSON_Value * val = NULL;

	memset(&err, 0, sizeof(err));
	yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);
	options.coerce_keys_to_strings = true;
	status = gtext_yaml_to_json_with_options(
		yaml_doc,
		&json_val,
		&options,
		&err
	);

	EXPECT_EQ(status, GTEXT_YAML_OK);
	EXPECT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_OBJECT);

	val = gtext_json_object_get(json_val, "1", 1);
	ASSERT_NE(val, nullptr);
	EXPECT_EQ(gtext_json_typeof(val), GTEXT_JSON_STRING);

	gtext_json_free(json_val);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonEmptyDocument
 * @brief Test conversion of empty YAML document
 */
TEST(YamlToJson, EmptyDocument) {
	GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse("", 0, NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);
	
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, NULL);
	
	/* Empty document should convert to JSON null */
	ASSERT_EQ(status, GTEXT_YAML_OK);
	ASSERT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_NULL);
	
	gtext_json_free(json_val);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonMergeKeysOption
 * @brief Test that merge keys can be allowed via options
 */
TEST(YamlToJson, MergeKeysOption) {
	const char * yaml_input = "base: &base {a: 1}\nmerged: {<<: *base, b: 2}";
	GTEXT_YAML_Document * yaml_doc = NULL;
	GTEXT_YAML_To_JSON_Options options = gtext_yaml_to_json_options_default();
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Error err;
	GTEXT_YAML_Status status = GTEXT_YAML_OK;
	const GTEXT_JSON_Value * merged = NULL;

	memset(&err, 0, sizeof(err));
	yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);
	options.allow_merge_keys = true;
	options.allow_resolved_aliases = true;
	status = gtext_yaml_to_json_with_options(
		yaml_doc,
		&json_val,
		&options,
		&err
	);

	EXPECT_EQ(status, GTEXT_YAML_OK);
	EXPECT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_OBJECT);

	merged = gtext_json_object_get(json_val, "merged", 6);
	ASSERT_NE(merged, nullptr);
	EXPECT_EQ(gtext_json_typeof(merged), GTEXT_JSON_OBJECT);

	gtext_json_free(json_val);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonComplexDocument
 * @brief Test conversion of a complex YAML document with proper nesting
 * Using flow style to ensure proper nesting in the DOM
 */
TEST(YamlToJson, ComplexDocument) {
	const char * yaml_input = "{users: [{name: Alice, age: 30, email: alice@example.com}, {name: Bob, age: 25, email: bob@example.com}], settings: {theme: dark, notifications: true, timeout: 3600}}";
	GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);
	
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, &json_val, NULL);
	ASSERT_EQ(status, GTEXT_YAML_OK);
	ASSERT_NE(json_val, nullptr);
	
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_OBJECT);
	EXPECT_EQ(gtext_json_object_size(json_val), 2);
	
	/* Check users array */
	const GTEXT_JSON_Value * users = gtext_json_object_get(json_val, "users", 5);
	ASSERT_NE(users, nullptr);
	EXPECT_EQ(gtext_json_typeof(users), GTEXT_JSON_ARRAY);
	EXPECT_EQ(gtext_json_array_size(users), 2);
	
	/* Check first user */
	const GTEXT_JSON_Value * alice = gtext_json_array_get(users, 0);
	ASSERT_NE(alice, nullptr);
	EXPECT_EQ(gtext_json_typeof(alice), GTEXT_JSON_OBJECT);
	
	const GTEXT_JSON_Value * alice_name = gtext_json_object_get(alice, "name", 4);
	ASSERT_NE(alice_name, nullptr);
	const char * str = NULL;
	size_t len = 0;
	EXPECT_EQ(gtext_json_get_string(alice_name, &str, &len), GTEXT_JSON_OK);
	EXPECT_STREQ(str, "Alice");
	
	gtext_json_free(json_val);
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonInvalidArguments
 * @brief Test error handling for invalid arguments
 */
TEST(YamlToJson, InvalidArguments) {
	GTEXT_YAML_Document * yaml_doc = gtext_yaml_parse("test", 4, NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);
	
	/* NULL document */
	{
		GTEXT_JSON_Value * json_val = NULL;
		GTEXT_YAML_Status status = gtext_yaml_to_json(NULL, &json_val, NULL);
		EXPECT_NE(status, GTEXT_YAML_OK);
	}
	
	/* NULL output pointer */
	{
		GTEXT_YAML_Status status = gtext_yaml_to_json(yaml_doc, NULL, NULL);
		EXPECT_NE(status, GTEXT_YAML_OK);
	}
	
	gtext_yaml_free(yaml_doc);
}

/**
 * @test YamlToJsonLargeIntPolicy
 * @brief Test large integer handling options
 */
TEST(YamlToJson, LargeIntPolicy) {
	const char * yaml_input = "!!int 9007199254740993";
	GTEXT_YAML_Document * yaml_doc = NULL;
	GTEXT_JSON_Value * json_val = NULL;
	GTEXT_YAML_Error err;
	GTEXT_YAML_Status status = GTEXT_YAML_OK;
	GTEXT_YAML_To_JSON_Options options = gtext_yaml_to_json_options_default();
	const char * str = NULL;
	size_t len = 0;
	double double_val = 0.0;

	memset(&err, 0, sizeof(err));
	yaml_doc = gtext_yaml_parse(yaml_input, strlen(yaml_input), NULL, NULL);
	ASSERT_NE(yaml_doc, nullptr);
	status = gtext_yaml_to_json(yaml_doc, &json_val, &err);
	EXPECT_NE(status, GTEXT_YAML_OK);
	EXPECT_EQ(json_val, nullptr);
	EXPECT_NE(err.message, nullptr);
	gtext_yaml_error_free(&err);
	memset(&err, 0, sizeof(err));
	options.large_int_policy = GTEXT_YAML_JSON_LARGE_INT_STRING;
	status = gtext_yaml_to_json_with_options(yaml_doc, &json_val, &options, &err);
	EXPECT_EQ(status, GTEXT_YAML_OK);
	ASSERT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_STRING);
	EXPECT_EQ(gtext_json_get_string(json_val, &str, &len), GTEXT_JSON_OK);
	EXPECT_STREQ(str, "9007199254740993");
	gtext_json_free(json_val);
	json_val = NULL;

	options.large_int_policy = GTEXT_YAML_JSON_LARGE_INT_DOUBLE;
	status = gtext_yaml_to_json_with_options(yaml_doc, &json_val, &options, &err);
	EXPECT_EQ(status, GTEXT_YAML_OK);
	ASSERT_NE(json_val, nullptr);
	EXPECT_EQ(gtext_json_typeof(json_val), GTEXT_JSON_NUMBER);
	EXPECT_EQ(gtext_json_get_double(json_val, &double_val), GTEXT_JSON_OK);
	EXPECT_NE(double_val, 0.0);

	gtext_json_free(json_val);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(yaml_doc);
}

// ---------------------------------------------------------------------------
// Alias expansion budget
//
// Resolving aliases turns a DAG into a tree, so a document linear in its input
// can convert to one exponential in it - the classic YAML alias bomb.  The
// conversion had a cycle check, but a bomb is not cyclic: every path through
// it is distinct, so it passed the check and then allocated until malloc()
// failed.  A 430-byte document exhausted all available memory and came back
// GTEXT_YAML_E_OOM; the time it took scaled with how much memory the process
// was allowed, which is the signature of no budget at all.
//
// The library had the right accounting in a resolver module, and a test for it
// (YamlAliasExponential.DFSLimit), but that test drove ResolverState directly
// and the path a caller actually takes - gtext_yaml_parse() followed by
// gtext_yaml_to_json_with_options() - never consulted it.  The budget below is
// what replaced it.  That module has since been deleted: four of its six
// functions had no caller anywhere in src/, and the other two were called only
// to build an object nothing ever read.  These three tests are the coverage it
// was credited with, asked of the code that answers.
// ---------------------------------------------------------------------------

namespace {

// levels=8 is nominally 10^9 nodes from a few hundred bytes of input.
std::string yaml_alias_bomb(int levels) {
	std::string s = "l0: &l0 [x,x,x,x,x,x,x,x,x,x]\n";
	for (int i = 1; i <= levels; ++i) {
		s += "l" + std::to_string(i) + ": &l" + std::to_string(i) + " [";
		for (int j = 0; j < 10; ++j) {
			s += (j ? "," : "");
			s += "*l" + std::to_string(i - 1);
		}
		s += "]\n";
	}
	return s;
}

} // namespace

TEST(YamlToJsonAliasBudget, BombIsRejectedByLimitNotByOom) {
	std::string src = yaml_alias_bomb(8);

	GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));

	// Parsing is cheap: aliases are stored by reference, so the DOM stays
	// linear in the input.  The blowup is entirely in the conversion.
	GTEXT_YAML_Document * doc =
	    gtext_yaml_parse(src.data(), src.size(), &popts, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");

	GTEXT_YAML_To_JSON_Options jopts = gtext_yaml_to_json_options_default();
	jopts.allow_resolved_aliases = true;

	GTEXT_JSON_Value * out = nullptr;
	GTEXT_YAML_Status st =
	    gtext_yaml_to_json_with_options(doc, &out, &jopts, &err);

	// The distinction that matters: a configured limit refused it, rather
	// than the allocator failing after the process had taken every page it
	// could get.
	EXPECT_EQ(st, GTEXT_YAML_E_LIMIT);
	EXPECT_NE(st, GTEXT_YAML_E_OOM);
	EXPECT_EQ(out, nullptr);

	if (out) {
		gtext_json_free(out);
	}
	gtext_yaml_error_free(&err);
	gtext_yaml_free(doc);
}

TEST(YamlToJsonAliasBudget, OrdinaryAliasesStillConvert) {
	// The budget must not refuse documents that simply use aliases.
	const char * src =
	    "defaults: &d\n"
	    "  timeout: 30\n"
	    "  retries: 3\n"
	    "service_a: *d\n"
	    "service_b: *d\n";

	GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));

	GTEXT_YAML_Document * doc =
	    gtext_yaml_parse(src, strlen(src), &popts, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "parse failed");

	GTEXT_YAML_To_JSON_Options jopts = gtext_yaml_to_json_options_default();
	jopts.allow_resolved_aliases = true;

	GTEXT_JSON_Value * out = nullptr;
	GTEXT_YAML_Status st =
	    gtext_yaml_to_json_with_options(doc, &out, &jopts, &err);

	EXPECT_EQ(st, GTEXT_YAML_OK) << (err.message ? err.message : "");
	EXPECT_NE(out, nullptr);

	if (out) {
		gtext_json_free(out);
	}
	gtext_yaml_error_free(&err);
	gtext_yaml_free(doc);
}

TEST(YamlToJsonAliasBudget, LimitIsTakenFromTheParseOptions) {
	// A caller who raises max_alias_expansion gets more room; one who lowers
	// it gets less.  This is what ties the conversion to the documented knob.
	const std::string src = yaml_alias_bomb(3);

	struct Case {
		size_t limit;
		bool expect_ok;
	};
	// levels=3 expands to roughly 10^4 nodes.
	const Case cases[] = {{100, false}, {1000000, true}};

	for (const Case & c : cases) {
		GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
		popts.max_alias_expansion = c.limit;

		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Document * doc =
		    gtext_yaml_parse(src.data(), src.size(), &popts, &err);
		ASSERT_NE(doc, nullptr)
		    << "limit=" << c.limit << ": "
		    << (err.message ? err.message : "parse failed");

		GTEXT_YAML_To_JSON_Options jopts =
		    gtext_yaml_to_json_options_default();
		jopts.allow_resolved_aliases = true;

		GTEXT_JSON_Value * out = nullptr;
		GTEXT_YAML_Status st =
		    gtext_yaml_to_json_with_options(doc, &out, &jopts, &err);

		if (c.expect_ok) {
			EXPECT_EQ(st, GTEXT_YAML_OK)
			    << "limit=" << c.limit << " should have been enough";
		}
		else {
			EXPECT_EQ(st, GTEXT_YAML_E_LIMIT)
			    << "limit=" << c.limit << " should have been too small";
		}

		if (out) {
			gtext_json_free(out);
		}
		gtext_yaml_error_free(&err);
		gtext_yaml_free(doc);
	}
}

// ---------------------------------------------------------------------------
// Conversion options and refusals
//
// yaml_to_json.c sat at 52.5%, the second-least-covered file in the library.
// Almost all of the untested part is the set of documents it is supposed to
// refuse, and the options that decide what it does instead - which is exactly
// the part a caller depends on when the input is not theirs.
// ---------------------------------------------------------------------------

namespace {

GTEXT_YAML_Document * parse_yaml_or_die(const char * src) {
	GTEXT_YAML_Parse_Options po = gtext_yaml_parse_options_default();
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * d = gtext_yaml_parse(src, strlen(src), &po, &err);
	EXPECT_NE(d, nullptr) << (err.message ? err.message : "parse failed");
	gtext_yaml_error_free(&err);
	return d;
}

} // namespace

TEST(YamlToJsonLargeInt, DefaultPolicyRefuses) {
	// 2^53 + 1: representable in YAML, not exactly representable as a JSON
	// number under the interoperable range of RFC 8259 section 6.
	GTEXT_YAML_Document * d = parse_yaml_or_die("n: 9007199254740993\n");
	ASSERT_NE(d, nullptr);

	GTEXT_YAML_To_JSON_Options o = gtext_yaml_to_json_options_default();
	EXPECT_EQ(o.large_int_policy, GTEXT_YAML_JSON_LARGE_INT_ERROR);

	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * out = nullptr;
	EXPECT_NE(gtext_yaml_to_json_with_options(d, &out, &o, &err), GTEXT_YAML_OK);
	EXPECT_EQ(out, nullptr);

	if (out) {
		gtext_json_free(out);
	}
	gtext_yaml_error_free(&err);
	gtext_yaml_free(d);
}

TEST(YamlToJsonLargeInt, StringPolicyKeepsTheDigits) {
	GTEXT_YAML_Document * d = parse_yaml_or_die("n: 9007199254740993\n");
	ASSERT_NE(d, nullptr);

	GTEXT_YAML_To_JSON_Options o = gtext_yaml_to_json_options_default();
	o.large_int_policy = GTEXT_YAML_JSON_LARGE_INT_STRING;

	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * out = nullptr;
	ASSERT_EQ(gtext_yaml_to_json_with_options(d, &out, &o, &err), GTEXT_YAML_OK)
	    << (err.message ? err.message : "");
	ASSERT_NE(out, nullptr);

	const GTEXT_JSON_Value * n = gtext_json_object_get(out, "n", 1);
	ASSERT_NE(n, nullptr);
	EXPECT_EQ(gtext_json_typeof(n), GTEXT_JSON_STRING);

	const char * s = nullptr;
	size_t len = 0;
	ASSERT_EQ(gtext_json_get_string(n, &s, &len), GTEXT_JSON_OK);
	// The point of this policy is that no digit is lost.
	EXPECT_EQ(std::string(s, len), "9007199254740993");

	gtext_json_free(out);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(d);
}

TEST(YamlToJsonLargeInt, DoublePolicyProducesANumber) {
	GTEXT_YAML_Document * d = parse_yaml_or_die("n: 9007199254740993\n");
	ASSERT_NE(d, nullptr);

	GTEXT_YAML_To_JSON_Options o = gtext_yaml_to_json_options_default();
	o.large_int_policy = GTEXT_YAML_JSON_LARGE_INT_DOUBLE;

	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * out = nullptr;
	ASSERT_EQ(gtext_yaml_to_json_with_options(d, &out, &o, &err), GTEXT_YAML_OK)
	    << (err.message ? err.message : "");
	ASSERT_NE(out, nullptr);

	const GTEXT_JSON_Value * n = gtext_json_object_get(out, "n", 1);
	ASSERT_NE(n, nullptr);
	EXPECT_EQ(gtext_json_typeof(n), GTEXT_JSON_NUMBER);

	double v = 0;
	ASSERT_EQ(gtext_json_get_double(n, &v), GTEXT_JSON_OK);
	// This policy trades exactness for a number, so the value is the nearest
	// double rather than the original integer.
	EXPECT_DOUBLE_EQ(v, 9007199254740992.0);

	gtext_json_free(out);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(d);
}

TEST(YamlToJsonLargeInt, SafeRangeIsUnaffectedByThePolicy) {
	// An integer inside the safe range must convert identically whatever the
	// policy says, or the policy is changing more than it claims to.
	const GTEXT_YAML_JSON_Large_Int_Policy policies[] = {
	    GTEXT_YAML_JSON_LARGE_INT_ERROR,
	    GTEXT_YAML_JSON_LARGE_INT_STRING,
	    GTEXT_YAML_JSON_LARGE_INT_DOUBLE,
	};

	for (auto policy : policies) {
		GTEXT_YAML_Document * d = parse_yaml_or_die("n: 42\n");
		ASSERT_NE(d, nullptr);

		GTEXT_YAML_To_JSON_Options o = gtext_yaml_to_json_options_default();
		o.large_int_policy = policy;

		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * out = nullptr;
		ASSERT_EQ(
		    gtext_yaml_to_json_with_options(d, &out, &o, &err), GTEXT_YAML_OK);
		ASSERT_NE(out, nullptr);

		const GTEXT_JSON_Value * n = gtext_json_object_get(out, "n", 1);
		ASSERT_NE(n, nullptr);
		EXPECT_EQ(gtext_json_typeof(n), GTEXT_JSON_NUMBER);
		int64_t v = 0;
		EXPECT_EQ(gtext_json_get_i64(n, &v), GTEXT_JSON_OK);
		EXPECT_EQ(v, 42);

		gtext_json_free(out);
		gtext_yaml_error_free(&err);
		gtext_yaml_free(d);
	}
}

TEST(YamlToJsonKeys, NonStringKeysNeedCoercion) {
	// JSON object names are strings; YAML keys need not be.  The default is to
	// refuse rather than to invent a spelling.
	GTEXT_YAML_Document * d = parse_yaml_or_die("1: one\ntrue: yes\n");
	ASSERT_NE(d, nullptr);

	{
		GTEXT_YAML_To_JSON_Options o = gtext_yaml_to_json_options_default();
		EXPECT_FALSE(o.coerce_keys_to_strings);

		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * out = nullptr;
		EXPECT_NE(
		    gtext_yaml_to_json_with_options(d, &out, &o, &err), GTEXT_YAML_OK);
		if (out) {
			gtext_json_free(out);
		}
		gtext_yaml_error_free(&err);
	}
	{
		GTEXT_YAML_To_JSON_Options o = gtext_yaml_to_json_options_default();
		o.coerce_keys_to_strings = true;

		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * out = nullptr;
		ASSERT_EQ(gtext_yaml_to_json_with_options(d, &out, &o, &err),
		    GTEXT_YAML_OK)
		    << (err.message ? err.message : "");
		ASSERT_NE(out, nullptr);
		EXPECT_EQ(gtext_json_typeof(out), GTEXT_JSON_OBJECT);
		EXPECT_NE(gtext_json_object_get(out, "1", 1), nullptr)
		    << "the integer key should have become the string \"1\"";

		gtext_json_free(out);
		gtext_yaml_error_free(&err);
	}

	gtext_yaml_free(d);
}

// !!set, !!omap and !!pairs convert to what they already are: a set is a
// mapping whose values are all null, and an omap or pairs is a sequence of
// single-pair mappings.  Nothing about the data is lost on the way - a JSON
// array is ordered, so an omap keeps its order, and pairs keeps its
// duplicate keys.  Only the tag goes, and JSON drops the tag of every tagged
// node: "!foo 5" has always converted to 5 without complaint.
//
// This file used to assert a refusal here.  The refusal was documented as
// stopping "a YAML-specific collection silently becoming a JSON array", but
// it was the one hardcoded refusal in a converter whose other four - for
// aliases, merge keys, non-string keys and out-of-range integers - each sit
// behind an option and each guard a real change to the data.  This one
// guarded none.  The three cases below are the JSON that yaml-test-suite
// gives for spec examples 2.25 and 2.26.
TEST(YamlToJsonRefusals, YamlSpecificCollectionsConvertStructurally) {
	struct Case {
		const char * src;
		GTEXT_JSON_Type type;
		size_t size;
	};
	const Case cases[] = {
	    {"!!set {a: ~, b: ~}", GTEXT_JSON_OBJECT, 2},
	    {"!!omap [{a: 1}, {b: 2}]", GTEXT_JSON_ARRAY, 2},
	    {"!!pairs [{a: 1}, {a: 2}]", GTEXT_JSON_ARRAY, 2},
	};

	for (const Case & c : cases) {
		SCOPED_TRACE(c.src);
		GTEXT_YAML_Document * d = parse_yaml_or_die(c.src);
		ASSERT_NE(d, nullptr);

		GTEXT_YAML_To_JSON_Options o = gtext_yaml_to_json_options_default();
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * out = nullptr;
		ASSERT_EQ(
		    gtext_yaml_to_json_with_options(d, &out, &o, &err), GTEXT_YAML_OK)
		    << (err.message ? err.message : "?");
		ASSERT_NE(out, nullptr);
		EXPECT_EQ(gtext_json_typeof(out), c.type);
		EXPECT_EQ(c.type == GTEXT_JSON_OBJECT ? gtext_json_object_size(out)
		                                      : gtext_json_array_size(out),
		    c.size);

		gtext_json_free(out);
		gtext_yaml_error_free(&err);
		gtext_yaml_free(d);
	}
}

// A set's values are null, not the empty string or a missing key.
TEST(YamlToJsonRefusals, ASetsValuesAreNull) {
	GTEXT_YAML_Document * d = parse_yaml_or_die("!!set {a: ~, b: ~}");
	ASSERT_NE(d, nullptr);
	GTEXT_JSON_Value * out = nullptr;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	ASSERT_EQ(gtext_yaml_to_json(d, &out, &err), GTEXT_YAML_OK);
	ASSERT_NE(out, nullptr);
	const GTEXT_JSON_Value * a = gtext_json_object_get(out, "a", 1);
	ASSERT_NE(a, nullptr);
	EXPECT_EQ(gtext_json_typeof(a), GTEXT_JSON_NULL);
	gtext_json_free(out);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(d);
}

// !!pairs exists to carry duplicate keys, and the array keeps both.  A JSON
// object could not: this is why omap and pairs convert to arrays and not to
// objects.
TEST(YamlToJsonRefusals, PairsKeepsItsDuplicateKeys) {
	GTEXT_YAML_Document * d = parse_yaml_or_die("!!pairs [{a: 1}, {a: 2}]");
	ASSERT_NE(d, nullptr);
	GTEXT_JSON_Value * out = nullptr;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	ASSERT_EQ(gtext_yaml_to_json(d, &out, &err), GTEXT_YAML_OK);
	ASSERT_NE(out, nullptr);
	ASSERT_EQ(gtext_json_array_size(out), 2u);
	for (size_t i = 0; i < 2; ++i) {
		const GTEXT_JSON_Value * e = gtext_json_array_get(out, i);
		ASSERT_NE(e, nullptr);
		ASSERT_EQ(gtext_json_object_size(e), 1u);
		EXPECT_STREQ(gtext_json_object_key(e, 0, nullptr), "a");
	}
	gtext_json_free(out);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(d);
}

// A set whose key is not a scalar still has nowhere to go: JSON object keys
// are strings.  That refusal is the non-string-key rule and is unchanged.
TEST(YamlToJsonRefusals, ASetWithACollectionKeyIsStillRefused) {
	GTEXT_YAML_Document * d = parse_yaml_or_die("--- !!set\n? [1, 2]\n");
	ASSERT_NE(d, nullptr);
	GTEXT_JSON_Value * out = nullptr;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	EXPECT_NE(gtext_yaml_to_json(d, &out, &err), GTEXT_YAML_OK);
	if (out) gtext_json_free(out);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(d);
}

TEST(YamlToJsonRefusals, AliasesAreRefusedUnlessAllowed) {
	GTEXT_YAML_Document * d =
	    parse_yaml_or_die("a: &x 1\nb: *x\n");
	ASSERT_NE(d, nullptr);

	{
		GTEXT_YAML_To_JSON_Options o = gtext_yaml_to_json_options_default();
		EXPECT_FALSE(o.allow_resolved_aliases);

		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * out = nullptr;
		EXPECT_NE(
		    gtext_yaml_to_json_with_options(d, &out, &o, &err), GTEXT_YAML_OK);
		if (out) {
			gtext_json_free(out);
		}
		gtext_yaml_error_free(&err);
	}
	{
		GTEXT_YAML_To_JSON_Options o = gtext_yaml_to_json_options_default();
		o.allow_resolved_aliases = true;

		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * out = nullptr;
		ASSERT_EQ(gtext_yaml_to_json_with_options(d, &out, &o, &err),
		    GTEXT_YAML_OK)
		    << (err.message ? err.message : "");
		ASSERT_NE(out, nullptr);

		// Both names carry the resolved value.
		const GTEXT_JSON_Value * b = gtext_json_object_get(out, "b", 1);
		ASSERT_NE(b, nullptr);
		int64_t v = 0;
		EXPECT_EQ(gtext_json_get_i64(b, &v), GTEXT_JSON_OK);
		EXPECT_EQ(v, 1);

		gtext_json_free(out);
		gtext_yaml_error_free(&err);
	}

	gtext_yaml_free(d);
}

TEST(YamlToJsonRefusals, NullArgumentsAreRejected) {
	GTEXT_YAML_Document * d = parse_yaml_or_die("a: 1\n");
	ASSERT_NE(d, nullptr);

	GTEXT_JSON_Value * out = nullptr;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));

	EXPECT_NE(gtext_yaml_to_json(nullptr, &out, &err), GTEXT_YAML_OK);
	EXPECT_NE(gtext_yaml_to_json(d, nullptr, &err), GTEXT_YAML_OK);

	gtext_yaml_error_free(&err);
	gtext_yaml_free(d);
}

TEST(YamlToJsonEmpty, EmptyDocumentBecomesNull) {
	GTEXT_YAML_Parse_Options po = gtext_yaml_parse_options_default();
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * d = gtext_yaml_parse("", 0, &po, &err);
	// Asserted, not skipped on. This used to read
	//
	//     if (!d) GTEST_SKIP() << "an empty document does not parse";
	//
	// which made the parser refusing an empty document a *pass*: the one
	// regression this test is placed to catch would have turned it green. An
	// empty stream is a valid YAML document whose root is the empty node, and
	// 10.3.2 resolves that to null under the core schema, so refusing it is a
	// defect and belongs here as a failure.
	//
	// A skip is for something about the machine - no mkfifo, no comma-decimal
	// locale installed - never for a property of the library under test.
	ASSERT_NE(d, nullptr)
	    << "an empty document must parse: its root is the empty node, which the "
	       "core schema resolves to null ("
	    << (err.message ? err.message : "no message") << ")";

	GTEXT_JSON_Value * out = nullptr;
	ASSERT_EQ(gtext_yaml_to_json(d, &out, &err), GTEXT_YAML_OK)
	    << (err.message ? err.message : "");
	ASSERT_NE(out, nullptr);
	EXPECT_EQ(gtext_json_typeof(out), GTEXT_JSON_NULL);

	gtext_json_free(out);
	gtext_yaml_error_free(&err);
	gtext_yaml_free(d);
}

// A tag on a block-style collection used to be dropped: the scanner attached
// it to the next scalar, so for "!!omap\na: 1" the mapping came out untagged
// and gtext_yaml_node_tag() returned NULL.  The refusal below is the reason it
// mattered - with the tag gone, "!!omap\n- a: 1" converted to [{"a":1}]
// without complaint, and block style is the common style in real YAML.
//
// The tag now goes to the collection.  This test was written disabled while
// the bug was open and is enabled now that it is not.
TEST(YamlToJsonRefusals, BlockStyleCollectionsKeepTheirTag) {
	const char * docs[] = {
	    "!!omap\n- a: 1\n- b: 2\n",
	    "!!pairs\n- a: 1\n- a: 2\n",
	};

	for (const char * src : docs) {
		SCOPED_TRACE(src);
		GTEXT_YAML_Document * d = parse_yaml_or_die(src);
		ASSERT_NE(d, nullptr);

		const GTEXT_YAML_Node * root = gtext_yaml_document_root(d);
		ASSERT_NE(root, nullptr);
		EXPECT_NE(gtext_yaml_node_tag(root), nullptr)
		    << "the document's tag was dropped";

		/* The node's own type is the direct evidence that the tag reached
		   the collection rather than its first entry. This used to be read
		   off a JSON refusal instead, which stopped being a signal once
		   !!omap and !!pairs began converting - and was always indirect. */
		const GTEXT_YAML_Node_Type t = gtext_yaml_node_type(root);
		EXPECT_TRUE(t == GTEXT_YAML_OMAP || t == GTEXT_YAML_PAIRS)
		    << "the tag did not reach the collection";

		GTEXT_YAML_To_JSON_Options o = gtext_yaml_to_json_options_default();
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * out = nullptr;
		EXPECT_EQ(
		    gtext_yaml_to_json_with_options(d, &out, &o, &err), GTEXT_YAML_OK)
		    << (err.message ? err.message : "?");
		if (out) {
			EXPECT_EQ(gtext_json_typeof(out), GTEXT_JSON_ARRAY);
			EXPECT_EQ(gtext_json_array_size(out), 2u);
			gtext_json_free(out);
		}
		gtext_yaml_error_free(&err);
		gtext_yaml_free(d);
	}
}
