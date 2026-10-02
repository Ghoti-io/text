/**
 * @file
 *
 * Where a JSON Schema validation failed, and the defect that found it.
 *
 * The engine reported *what* was wrong - "Value does not match the required
 * type" - and nothing about *where*. On a one-line instance that is enough; on
 * a configuration file it is not, and it is also what a validator for another
 * format needs in order to translate a failure into its own coordinates.
 * GTEXT_JSON_Error::schema_instance is the value it failed on, and
 * gtext_json_schema_instance_pointer() turns that into an RFC 6901 pointer
 * into the instance.
 *
 * The pointer is derived on request rather than carried in the error, and that
 * was not the first design: a string in the error structure would have to be
 * owned by it, and a schema validation allocated nothing before - so every
 * existing caller that does not free the error after a failure would silently
 * have become one that leaks. Three tests in this library did exactly that,
 * and LeakSanitizer said so.
 *
 * ## One site, not seventeen
 *
 * The engine has some fifty places that set GTEXT_JSON_E_SCHEMA and seventeen
 * recursive calls. Threading an instance path through all of them would mean
 * deciding, at each, what step it takes - and one wrong decision puts a
 * failure in the wrong place, which is worse than having no place at all. So
 * the engine records only which *value* a failing frame was applied to, the
 * deepest frame to fail being the first to return, and the pointer is rendered
 * once at the top by finding that node in the instance.
 *
 * The tests below are about the two things that design can get wrong: the
 * depth it reports, and whether a passing validation reports a location at
 * all. `anyOf`, `oneOf`, `not` and `if` all run subschemas that are allowed
 * to fail.
 *
 * ## The defect the work found
 *
 * gtext_json_new_number_i64(1) and a parse of `1` were not the same value. The
 * constructor set the int64 representation and cleared the double one, so
 * gtext_json_get_double() refused the constructed number - and the schema
 * compiler reads `minimum`, `maximum`, `exclusiveMinimum`,
 * `exclusiveMaximum` and `multipleOf` that way. A schema assembled with the
 * DOM constructors, or converted from YAML or TOML, was refused as "Invalid
 * minimum value" while byte-identical schema *text* compiled. It is in this
 * file because this is how it surfaced: gtext_yaml_validate() could not
 * compile the first schema anyone wrote for it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>
#include <gtest/gtest.h>

#include <ghoti.io/text/json.h>

namespace {

struct Verdict {
	GTEXT_JSON_Status status = GTEXT_JSON_OK;
	std::string pointer;
	int needed = -1;
	bool had_pointer = false;
	bool had_instance = false;
	std::string message;
};

/* Compile `schema_text`, validate `instance_text`, and report what came back.
   Both are parsed, so neither side is built by hand - which matters for the
   constructor defect above, and is why the tests that need a *constructed*
   schema say so explicitly. */
Verdict Validate(const std::string & schema_text,
		const std::string & instance_text) {
	Verdict v;
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));

	GTEXT_JSON_Value * sv =
		gtext_json_parse(schema_text.data(), schema_text.size(), nullptr, &err);
	if (!sv) {
		v.status = GTEXT_JSON_E_INVALID;
		v.message = err.message ? err.message : "schema did not parse";
		gtext_json_error_free(&err);
		return v;
	}
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Schema * schema = gtext_json_schema_compile(sv, &err);
	if (!schema) {
		v.status = GTEXT_JSON_E_INVALID;
		v.message = err.message ? err.message : "schema did not compile";
		gtext_json_error_free(&err);
		gtext_json_free(sv);
		return v;
	}
	gtext_json_error_free(&err);

	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * iv = gtext_json_parse(
		instance_text.data(), instance_text.size(), nullptr, &err);
	gtext_json_error_free(&err);

	memset(&err, 0, sizeof(err));
	v.status = gtext_json_schema_validate(schema, iv, &err);
	v.had_instance = err.schema_instance != nullptr;
	if (err.message) v.message = err.message;

	/* Asked for twice: once with no buffer, which is the length-only form, and
	   once with one. The two must agree, or a caller sizing a buffer from the
	   first call would get a truncated answer from the second. */
	char buf[512];
	const int length_only =
		gtext_json_schema_instance_pointer(iv, &err, nullptr, 0);
	v.needed = gtext_json_schema_instance_pointer(iv, &err, buf, sizeof(buf));
	v.had_pointer = v.needed >= 0;
	if (v.had_pointer) v.pointer = buf;
	if (length_only != v.needed) {
		v.message += " [length-only and buffered answers differ]";
	}
	gtext_json_error_free(&err);

	gtext_json_free(iv);
	gtext_json_schema_free(schema);
	gtext_json_free(sv);
	return v;
}

const char * kNested =
	"{\"type\":\"object\",\"properties\":{"
	"\"a\":{\"type\":\"object\",\"properties\":{"
	"\"b\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}}}}}";

}  // namespace

// ---------------------------------------------------------------------------
// The depth it reports
// ---------------------------------------------------------------------------

TEST(JsonSchemaLocation, APassingValidationCarriesNoLocation) {
	const Verdict v = Validate(kNested, "{\"a\":{\"b\":[1,2,3]}}");
	ASSERT_EQ(v.status, GTEXT_JSON_OK) << v.message;
	EXPECT_FALSE(v.had_instance);
	EXPECT_EQ(v.needed, -1) << "there is nothing to point at";
}

TEST(JsonSchemaLocation, ATruncatedAnswerSaysWhatItNeeded) {
	/* snprintf's contract, which is the only way a caller can size a buffer:
	   write what fits, return what was needed. A function that returned the
	   written length instead would look identical until the one case where it
	   matters. */
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * sv = gtext_json_parse(kNested, strlen(kNested), nullptr,
		&err);
	ASSERT_NE(sv, nullptr);
	gtext_json_error_free(&err);
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Schema * schema = gtext_json_schema_compile(sv, &err);
	ASSERT_NE(schema, nullptr) << (err.message ? err.message : "?");
	gtext_json_error_free(&err);

	const char * doc = "{\"a\":{\"b\":[1,\"x\",3]}}";
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * iv = gtext_json_parse(doc, strlen(doc), nullptr, &err);
	ASSERT_NE(iv, nullptr);
	gtext_json_error_free(&err);

	memset(&err, 0, sizeof(err));
	ASSERT_EQ(gtext_json_schema_validate(schema, iv, &err),
		GTEXT_JSON_E_SCHEMA);

	/* "/a/b/1" is six bytes. */
	char small[4];
	memset(small, 'Z', sizeof(small));
	const int needed =
		gtext_json_schema_instance_pointer(iv, &err, small, sizeof(small));
	EXPECT_EQ(needed, 6) << "the length needed, not the length written";
	EXPECT_EQ(strlen(small), 3u) << "three bytes and a terminator";
	EXPECT_STREQ(small, "/a/");

	/* A zero-sized buffer, and a null one, both still answer the length. */
	EXPECT_EQ(gtext_json_schema_instance_pointer(iv, &err, small, 0), 6);
	EXPECT_EQ(gtext_json_schema_instance_pointer(iv, &err, nullptr, 0), 6);

	gtext_json_error_free(&err);
	gtext_json_free(iv);
	gtext_json_schema_free(schema);
	gtext_json_free(sv);
}

TEST(JsonSchemaLocation, NullArgumentsAnswerNegativeRatherThanCrashing) {
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	char buf[8];
	EXPECT_EQ(gtext_json_schema_instance_pointer(nullptr, &err, buf,
		sizeof(buf)), -1);
	EXPECT_EQ(gtext_json_schema_instance_pointer(nullptr, nullptr, buf,
		sizeof(buf)), -1);
	EXPECT_STREQ(buf, "") << "the buffer is terminated even on a refusal";
}

TEST(JsonSchemaLocation, ThePointerNamesTheDeepestFailingValue) {
	/* Three failures in one schema at three depths. Reporting the *shallowest*
	   would be the easy mistake - the outermost frame also fails - and all
	   three would then read "/a". */
	EXPECT_EQ(Validate(kNested, "{\"a\":{\"b\":[1,\"x\",3]}}").pointer, "/a/b/1");
	EXPECT_EQ(Validate(kNested, "{\"a\":{\"b\":\"not an array\"}}").pointer, "/a/b");
	EXPECT_EQ(Validate(kNested, "{\"a\":5}").pointer, "/a");
}

TEST(JsonSchemaLocation, TheWholeInstanceIsTheEmptyPointer) {
	/* RFC 6901's pointer to the document itself is "", not "/" and not absent.
	   A test that only checked for non-NULL would pass against a bug that
	   returned "/" here. */
	const Verdict scalar = Validate("{\"type\":\"integer\"}", "\"nope\"");
	ASSERT_EQ(scalar.status, GTEXT_JSON_E_SCHEMA) << scalar.message;
	EXPECT_TRUE(scalar.had_pointer) << "a root failure still has a pointer";
	EXPECT_EQ(scalar.pointer, "");

	const Verdict required =
		Validate("{\"type\":\"object\",\"required\":[\"z\"]}", "{\"y\":1}");
	ASSERT_EQ(required.status, GTEXT_JSON_E_SCHEMA) << required.message;
	EXPECT_EQ(required.pointer, "")
		<< "`required` is about the object, not about a member it lacks - "
		   "there is no node for the missing name to point at";
}

TEST(JsonSchemaLocation, ReferenceTokensAreEscaped) {
	/* RFC 6901: `~` is `~0` and `/` is `~1`, and in that order. Doing it the
	   other way round turns a `/` into `~01`, which reads back as `~1`. */
	EXPECT_EQ(Validate(
		"{\"type\":\"object\",\"properties\":{\"a/b~c\":{\"type\":\"integer\"}}}",
		"{\"a/b~c\":\"x\"}").pointer, "/a~1b~0c");
	EXPECT_EQ(Validate(
		"{\"type\":\"object\",\"properties\":{\"~1\":{\"type\":\"integer\"}}}",
		"{\"~1\":\"x\"}").pointer, "/~01")
		<< "the name is tilde-one; its pointer must not read back as a slash";
}

TEST(JsonSchemaLocation, AnIndexIsTheArrayPositionNotTheMatchCount) {
	EXPECT_EQ(Validate("{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}",
		"[1,2,3,4,\"x\"]").pointer, "/4");
	EXPECT_EQ(Validate(
		"{\"type\":\"array\",\"items\":{\"type\":\"array\","
		"\"items\":{\"type\":\"integer\"}}}",
		"[[1],[2,\"x\"]]").pointer, "/1/1");
}

// ---------------------------------------------------------------------------
// Branches that are allowed to fail
// ---------------------------------------------------------------------------

TEST(JsonSchemaLocation, ARecoveredBranchLeavesNothingBehind) {
	/* Every one of these runs a subschema that fails and then succeeds anyway,
	   and a passing validation must report no location at all.

	   What this does *not* test is the engine's guard against a recovered
	   branch leaking a location, because nothing reaches that guard: every
	   recovering applicator passes a scratch error it frees. Removing the
	   guard fails none of these. They assert the observable property, which
	   is the right thing for a test to assert and is worth having whichever
	   mechanism delivers it - but the guard's own reachability is a separate
	   claim and is recorded where the guard is. */
	const char * cases[][2] = {
		{"{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"integer\"}]}", "7"},
		{"{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"integer\"}]}", "7"},
		{"{\"not\":{\"type\":\"string\"}}", "7"},
		{"{\"if\":{\"type\":\"string\"},\"then\":{\"maxLength\":1},"
			"\"else\":{\"type\":\"integer\"}}", "7"},
		{"{\"type\":\"object\",\"propertyNames\":{\"maxLength\":9}}", "{\"ok\":1}"},
	};
	for (const auto & c : cases) {
		const Verdict v = Validate(c[0], c[1]);
		ASSERT_EQ(v.status, GTEXT_JSON_OK) << c[0] << " on " << c[1]
			<< ": " << v.message;
		EXPECT_FALSE(v.had_instance) << c[0] << " left an instance behind";
		EXPECT_EQ(v.needed, -1) << c[0] << " left a location behind";
	}
}

TEST(JsonSchemaLocation, AFailedApplicatorReportsItsOwnLevel) {
	/* ...and when the applicator itself fails, the location is the value the
	   applicator was applied to, not whatever its last branch was looking at. */
	const Verdict v = Validate(
		"{\"type\":\"object\",\"properties\":{\"n\":{\"anyOf\":"
		"[{\"type\":\"string\"},{\"type\":\"integer\"}]}}}",
		"{\"n\":true}");
	ASSERT_EQ(v.status, GTEXT_JSON_E_SCHEMA) << v.message;
	EXPECT_EQ(v.pointer, "/n");
}

TEST(JsonSchemaLocation, APropertyNameFailureHasNoPointer) {
	/* `propertyNames` validates each name as if it were a string instance, and
	   that string is built for the check - it is not part of the instance, so
	   there is no pointer to it. The value is still handed over, which is the
	   reason both fields exist. */
	const Verdict v = Validate(
		"{\"type\":\"object\",\"propertyNames\":{\"maxLength\":2}}",
		"{\"abc\":1}");
	ASSERT_EQ(v.status, GTEXT_JSON_E_SCHEMA) << v.message;
	EXPECT_FALSE(v.had_pointer)
		<< "a synthesised name is not in the instance, so a pointer to it "
		   "would name something else";
	EXPECT_TRUE(v.had_instance)
		<< "...but the name itself is still reportable";
	EXPECT_NE(v.message.find("property name"), std::string::npos) << v.message;
}

// ---------------------------------------------------------------------------
// The constructor defect
// ---------------------------------------------------------------------------

TEST(JsonNumberRepresentations, AConstructedIntegerHasADoubleToo) {
	/* The property: a number built by a constructor and a number read from the
	   same text answer the same questions. It did not hold. */
	struct Row { int64_t value; const char * text; };
	const Row rows[] = {{0, "0"}, {1, "1"}, {-1, "-1"}, {42, "42"},
		{9007199254740991LL, "9007199254740991"}};

	for (const Row & row : rows) {
		GTEXT_JSON_Value * built = gtext_json_new_number_i64(row.value);
		ASSERT_NE(built, nullptr) << row.text;

		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value * parsed =
			gtext_json_parse(row.text, strlen(row.text), nullptr, &err);
		ASSERT_NE(parsed, nullptr) << row.text;
		gtext_json_error_free(&err);

		double from_built = 0;
		double from_parsed = 0;
		EXPECT_EQ(gtext_json_get_double(built, &from_built), GTEXT_JSON_OK)
			<< row.text << ": a constructed integer refused to be read as a "
			   "double, and a parse of the same text does not";
		EXPECT_EQ(gtext_json_get_double(parsed, &from_parsed), GTEXT_JSON_OK)
			<< row.text;
		EXPECT_EQ(from_built, from_parsed) << row.text;

		int64_t exact = 0;
		EXPECT_EQ(gtext_json_get_i64(built, &exact), GTEXT_JSON_OK) << row.text;
		EXPECT_EQ(exact, row.value) << "the exact value is still exact";

		gtext_json_free(parsed);
		gtext_json_free(built);
	}
}

TEST(JsonNumberRepresentations, AConstructedUnsignedHasADoubleToo) {
	GTEXT_JSON_Value * built = gtext_json_new_number_u64(18446744073709551615ULL);
	ASSERT_NE(built, nullptr);
	double d = 0;
	EXPECT_EQ(gtext_json_get_double(built, &d), GTEXT_JSON_OK);
	/* Approximate above 2^53, which is what a parse of the same digits gives
	   too - the point is that the two agree, not that either is exact. */
	EXPECT_DOUBLE_EQ(d, 18446744073709551615.0);
	uint64_t exact = 0;
	EXPECT_EQ(gtext_json_get_u64(built, &exact), GTEXT_JSON_OK);
	EXPECT_EQ(exact, 18446744073709551615ULL);
	gtext_json_free(built);
}

TEST(JsonNumberRepresentations, ASchemaBuiltFromTheDomCompiles) {
	/* The consequence, and how it was found. Every one of these keywords is
	   read with gtext_json_get_double(). */
	for (const char * keyword : {"minimum", "maximum", "exclusiveMinimum",
			"exclusiveMaximum", "multipleOf"}) {
		GTEXT_JSON_Value * schema = gtext_json_new_object();
		ASSERT_NE(schema, nullptr) << keyword;
		ASSERT_EQ(gtext_json_object_put(schema, keyword, strlen(keyword),
			gtext_json_new_number_i64(2)), GTEXT_JSON_OK) << keyword;

		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Schema * compiled = gtext_json_schema_compile(schema, &err);
		EXPECT_NE(compiled, nullptr)
			<< keyword << " built from the DOM was refused: "
			<< (err.message ? err.message : "?")
			<< " - byte-identical schema text compiles";
		gtext_json_schema_free(compiled);
		gtext_json_error_free(&err);
		gtext_json_free(schema);
	}
}
