/**
 * @file
 *
 * Validating a YAML document against a JSON Schema, and saying where.
 *
 * "There is no YAML schema validator" was the second of the three findings on
 * the comparison page's list of things that would block an adoption. YAML 1.2
 * section 10 defines schemas and they are about implicit typing - whether
 * `yes` is a boolean - not about validation; Kwalify and Rx were proposed and
 * neither is used. What is used is JSON Schema, by Kubernetes, OpenAPI, GitHub
 * Actions and the yaml-language-server directive, with the schemas themselves
 * usually written in YAML.
 *
 * ## What is actually new here
 *
 * The verdict was always available: convert with gtext_yaml_to_json() and call
 * gtext_json_schema_validate(). What was not is **a position in the YAML
 * file**. A JSON Schema failure names a place in the JSON instance, and the
 * JSON instance is a document the user never wrote; a message about
 * `/spec/2/env` with no line number sends them hunting through a file that
 * does not contain those names.
 *
 * So the tests that matter here are the ones about line and column. A test
 * that only checked the verdict would pass against gtext_yaml_validate()
 * reimplemented as the two-call form, which is the thing it is supposed to be
 * better than.
 *
 * ## The second thing the two-call form flattens
 *
 * A document JSON cannot represent at all has not been judged against the
 * schema. GTEXT_YAML_E_SCHEMA and GTEXT_YAML_E_INVALID keep those apart, and
 * one test is about nothing else: "fix your document" and "fix your schema"
 * are different instructions.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>
#include <gtest/gtest.h>

#include <ghoti.io/text/json.h>
#include <ghoti.io/text/yaml.h>

namespace {

struct Outcome {
	GTEXT_YAML_Status status = GTEXT_YAML_OK;
	int line = 0;
	int col = 0;
	std::string message;
};

/* Compile `schema_yaml` as YAML and validate `doc_yaml` against it. Both in
   YAML, because that is how the pair arrives in practice and because a schema
   written in YAML is the case the JSON entry point cannot serve. */
Outcome Check(const std::string & schema_yaml, const std::string & doc_yaml,
		bool coerce_keys = false, bool allow_aliases = false) {
	Outcome out;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));

	GTEXT_YAML_Document * sd =
		gtext_yaml_parse(schema_yaml.data(), schema_yaml.size(), nullptr, &err);
	if (!sd) {
		out.status = GTEXT_YAML_E_INVALID;
		out.message = err.message ? err.message : "schema did not parse";
		gtext_yaml_error_free(&err);
		return out;
	}
	gtext_yaml_error_free(&err);

	GTEXT_YAML_Validate_Options opts = gtext_yaml_validate_options_default();
	opts.convert.coerce_keys_to_strings = coerce_keys;
	opts.convert.allow_resolved_aliases = allow_aliases;

	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Schema * schema = gtext_yaml_schema_compile(sd, &opts, &err);
	if (!schema) {
		out.status = GTEXT_YAML_E_INVALID;
		out.message = err.message ? err.message : "schema did not compile";
		gtext_yaml_error_free(&err);
		gtext_yaml_free(sd);
		return out;
	}
	gtext_yaml_error_free(&err);

	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * doc =
		gtext_yaml_parse(doc_yaml.data(), doc_yaml.size(), nullptr, &err);
	gtext_yaml_error_free(&err);

	memset(&err, 0, sizeof(err));
	out.status = gtext_yaml_validate(doc, schema, &opts, &err);
	out.line = err.line;
	out.col = err.col;
	if (err.message) out.message = err.message;
	gtext_yaml_error_free(&err);

	gtext_yaml_free(doc);
	gtext_json_schema_free(schema);
	gtext_yaml_free(sd);
	return out;
}

/* A schema with one of each shape a position can be reported in. */
const char * kSchema =
	"type: object\n"
	"properties:\n"
	"  name: {type: string}\n"
	"  port: {type: integer, minimum: 1}\n"
	"  tags:\n"
	"    type: array\n"
	"    items: {type: string}\n"
	"required: [name, port]\n";

}  // namespace

TEST(YamlSchema, AValidDocumentPasses) {
	const Outcome o = Check(kSchema, "name: ghoti\nport: 8080\ntags: [a, b]\n");
	EXPECT_EQ(o.status, GTEXT_YAML_OK) << o.message;
	EXPECT_EQ(o.line, 0);
}

// ---------------------------------------------------------------------------
// The position, which is the whole point
// ---------------------------------------------------------------------------

TEST(YamlSchema, AFailureReportsTheLineOfTheOffendingScalar) {
	/* Line 2, column 7: where `not-a-number` begins. Not line 1, and not the
	   end of the document, which are the two things a validator that reports
	   "somewhere" tends to say. */
	const Outcome o = Check(kSchema, "name: ghoti\nport: not-a-number\n");
	ASSERT_EQ(o.status, GTEXT_YAML_E_SCHEMA) << o.message;
	EXPECT_EQ(o.line, 2);
	EXPECT_EQ(o.col, 7);
}

TEST(YamlSchema, AFailureInsideASequenceNamesTheEntry) {
	/* The third entry of a block sequence, on its own line. A pointer of
	   `/tags/1` has to become line 5 and not line 3 - the sequence's own line -
	   which is what reporting the parent would give. */
	const Outcome o = Check(kSchema,
		"name: ghoti\n"
		"port: 1\n"
		"tags:\n"
		"  - ok\n"
		"  - 7\n"
		"  - also\n");
	ASSERT_EQ(o.status, GTEXT_YAML_E_SCHEMA) << o.message;
	EXPECT_EQ(o.line, 5);
	EXPECT_EQ(o.col, 5);
}

TEST(YamlSchema, AConstraintOnTheValueItselfIsFoundToo) {
	/* Not a type mismatch but a `minimum`, which is a different code path in
	   the engine and must report the same place. */
	const Outcome o = Check(kSchema, "name: ghoti\nport: 0\n");
	ASSERT_EQ(o.status, GTEXT_YAML_E_SCHEMA) << o.message;
	EXPECT_EQ(o.line, 2);
	EXPECT_EQ(o.col, 7);
	EXPECT_NE(o.message.find("minimum"), std::string::npos) << o.message;
}

TEST(YamlSchema, AFailureAboutTheWholeDocumentReportsTheRoot) {
	/* `required` is about the mapping, not about the name it lacks - there is
	   no node for a missing key - so the root's own location is the honest
	   answer rather than nothing at all. */
	const Outcome o = Check(kSchema, "port: 1\n");
	ASSERT_EQ(o.status, GTEXT_YAML_E_SCHEMA) << o.message;
	EXPECT_EQ(o.line, 1);
	EXPECT_EQ(o.col, 1);
	EXPECT_NE(o.message.find("Required"), std::string::npos) << o.message;
}

TEST(YamlSchema, ACoercedKeyIsFoundByItsCoercedName) {
	/* With coerce_keys_to_strings the YAML key `1` becomes the JSON name "1",
	   so the schema spells it "1" and the pointer comes back as `/1`. Resolving
	   that against the YAML means spelling the key the same way the conversion
	   did - which is why the validator shares one function with the converter
	   rather than carrying a second reading of the rule. */
	const Outcome o = Check(
		"type: object\nproperties:\n  '1': {type: string}\n", "1: 7\n", true);
	ASSERT_EQ(o.status, GTEXT_YAML_E_SCHEMA) << o.message;
	EXPECT_EQ(o.line, 1);
	EXPECT_EQ(o.col, 4) << "the value 7, not the key";
}

TEST(YamlSchema, AnAliasReportsTheAnchorWhereTheValueIsWritten) {
	/* An alias needs allow_resolved_aliases; by default the conversion refuses
	   the document outright, which the previous version of this test tripped
	   over and is worth knowing before turning the option on in anger.

	   With it on, the conversion expands the alias, so the instance holds the
	   value twice and the YAML holds it once. The second copy's pointer has no
	   node of its own; following the alias reports the anchor, which is the
	   only place in the file the value is actually written. */
	/* `a` is *valid* and only `b` is not, which is what forces the failure
	   through the alias. The first draft of this test required a string for
	   both, so `a` - an anchor on line 1 - failed first and the assertion
	   below passed without the alias path running at all: removing the
	   alias-following code changed nothing. A test that cannot fail. */
	const char * schema =
		"type: object\n"
		"properties:\n"
		"  a: {type: integer}\n"
		"  b: {type: string}\n";
	const char * doc = "a: &x 7\nb: *x\n";

	const Outcome refused = Check(schema, doc);
	EXPECT_NE(refused.status, GTEXT_YAML_E_SCHEMA)
		<< "an alias is a conversion refusal by default, not a schema failure";

	const Outcome o = Check(schema, doc, false, true);
	ASSERT_EQ(o.status, GTEXT_YAML_E_SCHEMA) << o.message;
	EXPECT_EQ(o.line, 1) << "the anchor is on line 1";
}

TEST(YamlSchema, AFlowMappingOnOneLineStillGetsAColumn) {
	const Outcome o = Check(kSchema, "{name: ghoti, port: zero}\n");
	ASSERT_EQ(o.status, GTEXT_YAML_E_SCHEMA) << o.message;
	EXPECT_EQ(o.line, 1);
	EXPECT_GT(o.col, 14) << "the column has to be past `name: ghoti, port: `";
}

// ---------------------------------------------------------------------------
// The distinction the two-call form flattens
// ---------------------------------------------------------------------------

TEST(YamlSchema, ADocumentJsonCannotHoldIsNotASchemaFailure) {
	/* A complex mapping key. The document cannot be expressed as JSON at all,
	   so nothing has been said about the schema - and a caller that treats
	   every non-OK status alike would tell the user to fix a constraint that
	   was never checked. */
	const Outcome o = Check(kSchema, "? [a, b]\n: c\n");
	EXPECT_NE(o.status, GTEXT_YAML_OK);
	EXPECT_NE(o.status, GTEXT_YAML_E_SCHEMA)
		<< "a conversion refusal must not be reported as a schema failure";
	EXPECT_NE(o.message.find("convert"), std::string::npos) << o.message;
}

TEST(YamlSchema, AnUnconvertibleSchemaIsRefusedAtCompileTime) {
	const Outcome o = Check("? [a, b]\n: c\n", "name: x\nport: 1\n");
	EXPECT_EQ(o.status, GTEXT_YAML_E_INVALID);
	EXPECT_FALSE(o.message.empty());
}

TEST(YamlSchema, AMalformedSchemaSaysWhichKeywordIsWrong) {
	const Outcome o = Check("type: object\nproperties: 3\n", "a: 1\n");
	EXPECT_EQ(o.status, GTEXT_YAML_E_INVALID);
	EXPECT_FALSE(o.message.empty()) << "the engine's own message is carried";
}

// ---------------------------------------------------------------------------
// Arguments and defaults
// ---------------------------------------------------------------------------

TEST(YamlSchema, NullArgumentsAreRefusedRatherThanCrashing) {
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	EXPECT_EQ(gtext_yaml_validate(nullptr, nullptr, nullptr, &err),
		GTEXT_YAML_E_INVALID);
	EXPECT_NE(err.message, nullptr);
	EXPECT_EQ(gtext_yaml_schema_compile(nullptr, nullptr, &err), nullptr);
	EXPECT_NE(err.message, nullptr);
	/* And with no error structure at all, which is the shape that segfaults. */
	EXPECT_EQ(gtext_yaml_validate(nullptr, nullptr, nullptr, nullptr),
		GTEXT_YAML_E_INVALID);
	EXPECT_EQ(gtext_yaml_schema_compile(nullptr, nullptr, nullptr), nullptr);
}

TEST(YamlSchema, TheDefaultsAreTheConversionsDefaults) {
	const GTEXT_YAML_Validate_Options opts =
		gtext_yaml_validate_options_default();
	const GTEXT_YAML_To_JSON_Options convert =
		gtext_yaml_to_json_options_default();
	EXPECT_EQ(opts.convert.coerce_keys_to_strings,
		convert.coerce_keys_to_strings);
	EXPECT_EQ(opts.convert.allow_merge_keys, convert.allow_merge_keys);
	EXPECT_EQ(opts.convert.large_int_policy, convert.large_int_policy);
	EXPECT_FALSE(opts.convert.coerce_keys_to_strings)
		<< "a non-string key is refused by default rather than reshaped, "
		   "which is the conversion's choice and not this function's to make";
}

TEST(YamlSchema, NullOptionsMeanTheDefaults) {
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * sd = gtext_yaml_parse(kSchema, strlen(kSchema),
		nullptr, &err);
	ASSERT_NE(sd, nullptr);
	gtext_yaml_error_free(&err);

	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Schema * schema = gtext_yaml_schema_compile(sd, nullptr, &err);
	ASSERT_NE(schema, nullptr) << (err.message ? err.message : "?");
	gtext_yaml_error_free(&err);

	const char * doc_text = "name: ghoti\nport: 0\n";
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * doc =
		gtext_yaml_parse(doc_text, strlen(doc_text), nullptr, &err);
	ASSERT_NE(doc, nullptr);
	gtext_yaml_error_free(&err);

	memset(&err, 0, sizeof(err));
	EXPECT_EQ(gtext_yaml_validate(doc, schema, nullptr, &err),
		GTEXT_YAML_E_SCHEMA);
	EXPECT_EQ(err.line, 2);
	gtext_yaml_error_free(&err);

	gtext_yaml_free(doc);
	gtext_json_schema_free(schema);
	gtext_yaml_free(sd);
}

TEST(YamlSchema, ASchemaCompiledFromYamlOutlivesItsDocument) {
	/* gtext_json_schema_compile() clones the schema document for its own $ref
	   resolution, so the converted JSON is not kept - and neither, therefore,
	   does the YAML document have to be. Asserted because the opposite was
	   assumed while this was being written, and an API that needs a document
	   kept alive and does not say so is a use-after-free waiting to happen. */
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * sd =
		gtext_yaml_parse(kSchema, strlen(kSchema), nullptr, &err);
	ASSERT_NE(sd, nullptr);
	GTEXT_JSON_Schema * schema = gtext_yaml_schema_compile(sd, nullptr, &err);
	ASSERT_NE(schema, nullptr) << (err.message ? err.message : "?");
	gtext_yaml_error_free(&err);

	gtext_yaml_free(sd);  /* gone before anything is validated */

	const char * doc_text = "name: ghoti\nport: 1\n";
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * doc =
		gtext_yaml_parse(doc_text, strlen(doc_text), nullptr, &err);
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(gtext_yaml_validate(doc, schema, nullptr, &err), GTEXT_YAML_OK)
		<< (err.message ? err.message : "?");
	gtext_yaml_error_free(&err);

	gtext_yaml_free(doc);
	gtext_json_schema_free(schema);
}
