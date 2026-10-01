/**
 * @file test-yaml-writer-contract.cpp
 * @brief What a writer must never do: claim success and produce something
 *        this library cannot read back.
 *
 * Running yaml-test-suite backwards measures the writers over documents that
 * came from parsing, and that is not the writers' domain.  The DOM API takes
 * any `char *` a caller hands it, so a value the parser would have refused is
 * ordinary on the way out - and a corpus of YAML text can never carry one,
 * because the parser refused it on the way in.  Every defect below was found
 * by building documents rather than by reading them.
 *
 *   - a character 5.1 forbids written raw, so the parser refused the writer's
 *     own output (34 code points);
 *   - an anchor name written without being checked, so "a b" wrote "&a b" and
 *     read back as the anchor "a" with the value shifted into it;
 *   - a tag percent-encoded on the way out but decoded on the way in only
 *     where a %TAG prefix applied, so "%" gained a layer of escaping on every
 *     round trip;
 *   - the streaming writer dropping the %YAML and %TAG directives, leaving a
 *     document whose tag handles were undefined.
 *
 * tests/fuzz/fuzz_yaml_writer.cpp searches the same space without a list.
 */

#include <gtest/gtest.h>
#include <string.h>

#include <string>

extern "C" {
#include <ghoti.io/text/yaml.h>
}

namespace {

struct Written {
	GTEXT_YAML_Status status;
	std::string text;
};

Written write_doc(const GTEXT_YAML_Document *doc, bool block = false) {
	Written w;
	GTEXT_YAML_Sink sink;
	EXPECT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
	GTEXT_YAML_Write_Options opts = gtext_yaml_write_options_default();
	if (block) {
		opts.pretty = true;
		opts.flow_style = GTEXT_YAML_FLOW_STYLE_BLOCK;
	}
	w.status = gtext_yaml_write_document(doc, &sink, &opts);
	if (w.status == GTEXT_YAML_OK) {
		w.text.assign(gtext_yaml_sink_buffer_data(&sink),
			gtext_yaml_sink_buffer_size(&sink));
	}
	gtext_yaml_sink_buffer_free(&sink);
	return w;
}

/* The value that comes back, read with its length so an embedded NUL is
 * visible. */
bool read_back_scalar(const std::string &yaml, std::string *value) {
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	GTEXT_YAML_Document *doc =
		gtext_yaml_parse(yaml.data(), yaml.size(), &opts, &err);
	gtext_yaml_error_free(&err);
	if (!doc) return false;
	const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
	const char *s = gtext_yaml_node_as_string(root);
	value->assign(s ? s : "", gtext_yaml_node_scalar_length(root));
	gtext_yaml_free(doc);
	return true;
}

std::string utf8(uint32_t cp) {
	std::string s;
	if (cp < 0x80) { s.push_back((char)cp); }
	else if (cp < 0x800) {
		s.push_back((char)(0xC0 | (cp >> 6)));
		s.push_back((char)(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		s.push_back((char)(0xE0 | (cp >> 12)));
		s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		s.push_back((char)(0x80 | (cp & 0x3F)));
	} else {
		s.push_back((char)(0xF0 | (cp >> 18)));
		s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
		s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		s.push_back((char)(0x80 | (cp & 0x3F)));
	}
	return s;
}

}  // namespace

/* 5.1's c-printable is the set of characters a stream may hold.  Everything
   outside it needs an escape, and only the double-quoted style has escapes;
   the writer used to emit them raw and the parser - correctly - refused what
   it had just produced.  "\x92" was refused for a second reason: the scanner
   wrote the raw byte 0x92 rather than encoding U+0092, and a lone
   continuation byte is not UTF-8. */
TEST(YamlWriterContract, EveryCodePointSurvivesOrIsRefused) {
	/* The whole of C0 and C1, DEL, the two non-characters at the end of the
	   BMP, and enough ordinary neighbours to show the line is in the right
	   place. */
	for (uint32_t cp = 1; cp <= 0x10FFFF; cp++) {
		if (cp > 0xA5 && cp < 0xD7F0) cp = 0xD7F0;
		if (cp == 0xD800) cp = 0xE000;
		if (cp > 0xE005 && cp < 0xFFF0) cp = 0xFFF0;
		if (cp > 0x10005 && cp < 0x10FFFE) cp = 0x10FFFE;

		const std::string value = "a" + utf8(cp) + "b";
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		ASSERT_NE(doc, nullptr);
		GTEXT_YAML_Node *root = gtext_yaml_node_new_scalar_n(
			doc, value.data(), value.size(), nullptr, nullptr);
		ASSERT_NE(root, nullptr);
		gtext_yaml_document_set_root(doc, root);

		Written w = write_doc(doc);
		gtext_yaml_free(doc);
		ASSERT_EQ(w.status, GTEXT_YAML_OK) << "U+" << std::hex << cp;

		std::string back;
		ASSERT_TRUE(read_back_scalar(w.text, &back))
			<< "U+" << std::hex << cp << " wrote " << w.text
			<< " which this parser refuses";
		EXPECT_EQ(back, value) << "U+" << std::hex << cp;
	}
}

/* The four that were written raw, kept by name so a regression says which
   rule broke rather than only that some code point did. */
TEST(YamlWriterContract, TheCharactersThatWereWrittenRaw) {
	struct Case { uint32_t cp; const char *spelling; };
	const Case cases[] = {
		{ 0x00, "\"a\\x00b\"" },   /* NUL: reachable only through the length API */
		{ 0x7F, "\"a\\x7Fb\"" },   /* DEL */
		{ 0x92, "\"a\\x92b\"" },   /* a C1 control */
		{ 0x85, "\"a\xc2\x85" "b\"" },  /* NEL *is* printable and stays raw */
		{ 0xFFFE, "\"a\\uFFFEb\"" },
	};
	for (const Case &c : cases) {
		const std::string value = "a" + utf8(c.cp) + "b";
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *root = gtext_yaml_node_new_scalar_n(
			doc, value.data(), value.size(), nullptr, nullptr);
		gtext_yaml_document_set_root(doc, root);
		Written w = write_doc(doc);
		gtext_yaml_free(doc);
		ASSERT_EQ(w.status, GTEXT_YAML_OK);
		EXPECT_EQ(w.text, c.spelling) << "U+" << std::hex << c.cp;
		std::string back;
		ASSERT_TRUE(read_back_scalar(w.text, &back));
		EXPECT_EQ(back, value) << "U+" << std::hex << c.cp;
	}
}

/* Quoting a scalar changes its value, not only its spelling: only a plain
   scalar is resolved by its contents (10.3.2). So the writer may quote for
   style wherever it likes except where the plain text would have resolved to
   something other than a string - and it was doing exactly that for the two
   spellings whose characters its whitelist had left out. "~" is the null the
   10.3.2 table gives first; "+" leads the core schema's integer and float
   rows. Neither is a c-indicator and neither needed quoting at all. */
TEST(YamlWriterContract, AResolvableScalarIsNotQuotedIntoAString) {
	struct Case { const char *yaml; const char *json; };
	const Case cases[] = {
		{ "~",      "null" },
		{ "+1",     "1" },
		{ "+1.5",   "1.5" },
		{ "-1",     "-1" },
		{ ".inf",   "null" },   /* JSON has no infinity; both sides agree */
		{ "null",   "null" },
		{ "true",   "true" },
	};
	for (const Case &c : cases) {
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
		GTEXT_YAML_Document *doc =
			gtext_yaml_parse(c.yaml, strlen(c.yaml), &popts, &err);
		ASSERT_NE(doc, nullptr) << c.yaml;
		gtext_yaml_error_free(&err);

		for (int block = 0; block < 2; block++) {
			Written w = write_doc(doc, block != 0);
			ASSERT_EQ(w.status, GTEXT_YAML_OK) << c.yaml;
			GTEXT_YAML_Document *back = gtext_yaml_parse(
				w.text.data(), w.text.size(), &popts, &err);
			ASSERT_NE(back, nullptr) << c.yaml << " wrote " << w.text;
			gtext_yaml_error_free(&err);
			const GTEXT_YAML_Node *root = gtext_yaml_document_root(back);
			/* The point is the node's *type*: a quoted "~" is a string. */
			EXPECT_EQ(gtext_yaml_node_type(root),
				gtext_yaml_node_type(gtext_yaml_document_root(doc)))
				<< c.yaml << " wrote " << w.text
				<< " which reads back as a different kind of scalar";
			gtext_yaml_free(back);
		}
		gtext_yaml_free(doc);
	}
}

/* gtext_yaml_node_new_scalar() used to make a *string* of whatever it was
   given, which is a default rather than an assertion, and it made both halves
   wrong: gtext_yaml_node_type() said "string" of a node holding "1", and the
   writer, told it was a string, wrote it plain - so it came back as the
   integer 1, and there was no way through the DOM API to write a string that
   looks like a number.

   The plain constructor takes the type from the text now, as a parse of the
   same characters would, and gtext_yaml_node_new_scalar_typed() is where a
   caller says otherwise. A string the text would not have produced goes out
   quoted, because only a plain scalar is resolved by its contents (10.3.2).
   The writer asks the resolver's own predicate, not a second copy of the
   10.3.2 tables. */
TEST(YamlWriterContract, AStringThatSpellsANumberIsQuoted) {
	const char *strings[] = {
		"1", "-1", "+1", "1.5", "0x1f", "0o17", ".inf", ".nan", "-.inf",
		"true", "False", "NULL", "~", "",
	};
	for (const char *value : strings) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *root = gtext_yaml_node_new_scalar_typed(
			doc, value, strlen(value), GTEXT_YAML_STRING, nullptr, nullptr);
		ASSERT_NE(root, nullptr) << value;
		ASSERT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_STRING) << value;
		gtext_yaml_document_set_root(doc, root);

		for (int block = 0; block < 2; block++) {
			Written w = write_doc(doc, block != 0);
			ASSERT_EQ(w.status, GTEXT_YAML_OK) << value;
			GTEXT_YAML_Error err;
			memset(&err, 0, sizeof(err));
			GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
			GTEXT_YAML_Document *back =
				gtext_yaml_parse(w.text.data(), w.text.size(), &opts, &err);
			ASSERT_NE(back, nullptr) << value << " wrote " << w.text;
			gtext_yaml_error_free(&err);
			const GTEXT_YAML_Node *r = gtext_yaml_document_root(back);
			EXPECT_EQ(gtext_yaml_node_type(r), GTEXT_YAML_STRING)
				<< "the string <<" << value << ">> wrote " << w.text
				<< " which reads back as something else";
			const char *got = gtext_yaml_node_as_string(r);
			EXPECT_STREQ(got ? got : "", value) << "wrote " << w.text;
			gtext_yaml_free(back);
		}
		gtext_yaml_free(doc);
	}
}

/* And the plain constructor agrees with a parse of the same characters. */
TEST(YamlWriterContract, ABuiltScalarHasTheTypeItsTextWouldParseAs) {
	struct Case { const char *text; GTEXT_YAML_Node_Type type; };
	const Case cases[] = {
		{ "1", GTEXT_YAML_INT },       { "-1", GTEXT_YAML_INT },
		{ "0x1f", GTEXT_YAML_INT },    { "1.5", GTEXT_YAML_FLOAT },
		{ ".inf", GTEXT_YAML_FLOAT },  { "true", GTEXT_YAML_BOOL },
		{ "False", GTEXT_YAML_BOOL },  { "null", GTEXT_YAML_NULL },
		{ "~", GTEXT_YAML_NULL },      { "", GTEXT_YAML_NULL },
		{ "x", GTEXT_YAML_STRING },    { "01", GTEXT_YAML_STRING },
		{ "0b101", GTEXT_YAML_STRING },/* 1.1, and this is 1.2 */
		{ "1_000", GTEXT_YAML_STRING },
	};
	for (const Case &c : cases) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *node =
			gtext_yaml_node_new_scalar(doc, c.text, nullptr, nullptr);
		ASSERT_NE(node, nullptr) << c.text;
		EXPECT_EQ(gtext_yaml_node_type(node), c.type) << c.text;

		/* And a parse of the same characters says the same thing. */
		if (*c.text) {
			GTEXT_YAML_Error err;
			memset(&err, 0, sizeof(err));
			GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
			GTEXT_YAML_Document *parsed =
				gtext_yaml_parse(c.text, strlen(c.text), &opts, &err);
			ASSERT_NE(parsed, nullptr) << c.text;
			gtext_yaml_error_free(&err);
			EXPECT_EQ(gtext_yaml_node_type(gtext_yaml_document_root(parsed)),
				c.type) << c.text;
			gtext_yaml_free(parsed);
		}
		gtext_yaml_free(doc);
	}
}

/* White space at either end of a built scalar's text makes it a string, and
   the reason is worth stating: a plain scalar's content has none - ns-plain
   begins and ends with an ns-char (7.3.3) - so no plain spelling of such text
   exists, only a quoted one, and quoted is string (10.3.2).

   strtoll() skips leading white space, which is how " 3", "\t3" and "\n3"
   came to answer "the integer 3" for a node the writer then quoted and the
   reader read back as a string. The trailing end was already right, which is
   why only half of this ever showed. It is not a question the corpus can put:
   a parse of " 3" is the integer 3, because the scanner takes the space off
   before any of this is asked - the text only reaches here through the DOM
   API, and the writer fuzzer is what put it there. */
TEST(YamlWriterContract, OuterWhiteSpaceMakesABuiltScalarAString) {
	const char *texts[] = {
		" 3", "\t3", "\n3", "3 ", "3\n", "\ntrue", " ~", " ", "\n",
		/* And the middle, which the ends-only test left. This code consumes
		   the sign itself and hands strtoll() what follows, and strtoll()
		   skips leading white space - so "+\n1" was the integer 1 and
		   "0x\n10" was 16. Not one row of the 10.3.2 table contains white
		   space anywhere, so the rule is simply that text carrying any is a
		   string, and the helper now wants a digit straight after the sign
		   on its own account as well. */
		"+\n1", "+ 1", "-\n1", "0x\n10", "1 1", "1\n1", "+\n\n1",
		/* And the two white-space characters YAML does not have. The vertical
		   tab and the form feed are not c-printable (5.1), so no parsed
		   scalar holds one - but the DOM API takes any char *, and strtod()
		   and strtoll() skip them exactly as they skip a space. "\v6662."
		   was answering "the float 6662", and the writer then escaped it into
		   a quoted scalar the reader read back as a string. */
		"\v6662.", "\v1", "\f1", "\v.5", "+\v1", "\vtrue",
	};
	for (const char *text : texts) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *node =
			gtext_yaml_node_new_scalar(doc, text, nullptr, nullptr);
		ASSERT_NE(node, nullptr) << ::testing::PrintToString(std::string(text));
		EXPECT_EQ(gtext_yaml_node_type(node), GTEXT_YAML_STRING)
			<< ::testing::PrintToString(std::string(text));

		/* And what the writer produces reads back as the same string - which
		   is the property the fuzzer checks, stated for these nine. */
		gtext_yaml_document_set_root(doc, node);
		Written w = write_doc(doc);
		ASSERT_EQ(w.status, GTEXT_YAML_OK)
			<< ::testing::PrintToString(std::string(text));
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
		GTEXT_YAML_Document *back =
			gtext_yaml_parse(w.text.c_str(), w.text.size(), &opts, &err);
		ASSERT_NE(back, nullptr) << "wrote " << w.text;
		gtext_yaml_error_free(&err);
		const GTEXT_YAML_Node *r = gtext_yaml_document_root(back);
		EXPECT_EQ(gtext_yaml_node_type(r), GTEXT_YAML_STRING) << "wrote " << w.text;
		const char *got = gtext_yaml_node_as_string(r);
		EXPECT_STREQ(got ? got : "", text) << "wrote " << w.text;
		gtext_yaml_free(back);
		gtext_yaml_free(doc);
	}
}

/* And a NUL, which is the same mistake pointing the other way.
 *
 * White space makes strtoll() and strtod() read *past* where a row of 10.3.2
 * ends; a NUL makes them stop *before* the text does. "42\0x" was handed to
 * strtoll(), which saw "42" and answered the integer 42 - so the DOM API
 * built an integer whose text is not one, and the writer then quoted it and
 * the reader read it back as the string it always was.
 *
 * The bool and null rows compare with lengths and were right the whole time,
 * which is the tell: "true\0" was a string while "42\0" was a number. And a
 * quoted "42\0" *parses* to a string, so the constructor and the parser
 * disagreed about the same characters. No row of 10.3.2 holds a NUL, and a
 * NUL is not c-printable either. */
TEST(YamlWriterContract, ANulMakesABuiltScalarAString) {
	const std::string texts[] = {
		std::string("42\0", 3),
		std::string("42\0x", 4),
		std::string("1.5\0", 4),
		std::string("\0" "42", 3),
		std::string("4\0" "2", 3),
		std::string("\0", 1),
		std::string("+\0" "1", 3),
		std::string("0x1\0", 4),
	};
	for (const std::string &text : texts) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *node = gtext_yaml_node_new_scalar_n(
			doc, text.data(), text.size(), nullptr, nullptr);
		ASSERT_NE(node, nullptr) << ::testing::PrintToString(text);
		EXPECT_EQ(gtext_yaml_node_type(node), GTEXT_YAML_STRING)
			<< ::testing::PrintToString(text);

		/* And the parser has to agree about the same characters, which is
		   what made this visible: the writer quoted the node and the reader
		   answered "string" where the DOM had said "int". */
		gtext_yaml_document_set_root(doc, node);
		Written w = write_doc(doc);
		ASSERT_EQ(w.status, GTEXT_YAML_OK) << ::testing::PrintToString(text);
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
		GTEXT_YAML_Document *back =
			gtext_yaml_parse(w.text.data(), w.text.size(), &opts, &err);
		ASSERT_NE(back, nullptr) << "wrote " << w.text << ": "
			<< (err.message ? err.message : "");
		gtext_yaml_error_free(&err);
		const GTEXT_YAML_Node *r = gtext_yaml_document_root(back);
		EXPECT_EQ(gtext_yaml_node_type(r), GTEXT_YAML_STRING) << "wrote " << w.text;
		std::string got;
		if (read_back_scalar(w.text, &got)) {
			EXPECT_EQ(got, text) << "wrote " << w.text;
		}
		gtext_yaml_free(back);
		gtext_yaml_free(doc);
	}
}

/* A line of exactly "---" or "..." is c-directives-end or c-document-end
   (9.1.2), and c-forbidden keeps either out of a document's content wherever
   it begins a line with a break, white space or end of input after it
   (9.1.1). The whitelist is about characters and every one of these is "-" or
   ".", all of which it admits, so the string "---" went out plain - and the
   writer had then produced a document marker and called it OK. The reader
   agreed with the bytes and handed back an empty document.

   Quoting is value-preserving here, since neither text resolves to anything
   but a string, so the whole family is quoted and not just the positions
   where it would be fatal. "----" and "---x" stay plain: c-forbidden wants a
   break or white space after the three characters, and both references read
   those two as the strings they are. */
TEST(YamlWriterContract, ADocumentMarkerIsNotWrittenAsAPlainScalar) {
	struct Case { const char *text; bool quoted; };
	const Case cases[] = {
		{ "---", true },   { "...", true },
		{ "--- x", true }, { "... x", true },
		{ "----", false }, { "---x", false },
		{ "...x", false }, { "..", false },
		{ "--", false },
	};
	for (const Case &c : cases) {
		for (int block = 0; block < 2; ++block) {
			GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
			GTEXT_YAML_Node *node = gtext_yaml_node_new_scalar_typed(
				doc, c.text, strlen(c.text), GTEXT_YAML_STRING,
				nullptr, nullptr);
			ASSERT_NE(node, nullptr) << c.text;
			gtext_yaml_document_set_root(doc, node);
			Written w = write_doc(doc, block != 0);
			ASSERT_EQ(w.status, GTEXT_YAML_OK) << c.text;
			EXPECT_EQ(w.text.find('"') != std::string::npos, c.quoted)
				<< "wrote " << w.text << " for <<" << c.text << ">>";

			/* Whatever it chose, the bytes have to read back as this string
			   and not as a marker - which is the property that failed. */
			GTEXT_YAML_Error err;
			memset(&err, 0, sizeof(err));
			GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
			GTEXT_YAML_Document *back =
				gtext_yaml_parse(w.text.c_str(), w.text.size(), &opts, &err);
			ASSERT_NE(back, nullptr) << "wrote " << w.text;
			gtext_yaml_error_free(&err);
			const GTEXT_YAML_Node *r = gtext_yaml_document_root(back);
			ASSERT_NE(r, nullptr) << "wrote " << w.text
				<< " which reads back as an empty document";
			const char *got = gtext_yaml_node_as_string(r);
			EXPECT_STREQ(got ? got : "", c.text) << "wrote " << w.text;
			gtext_yaml_free(back);
			gtext_yaml_free(doc);
		}
	}
}

/* But a scalar the parser resolved to a number stays plain: quoting it would
   make a string of it, which is the same fault in the other direction. */
TEST(YamlWriterContract, AResolvedScalarIsNotQuotedIntoAString) {
	const char *documents[] = { "1", "-1", "1.5", "true", "null", "~", ".inf" };
	for (const char *src : documents) {
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
		GTEXT_YAML_Document *doc = gtext_yaml_parse(src, strlen(src), &opts, &err);
		ASSERT_NE(doc, nullptr) << src;
		gtext_yaml_error_free(&err);
		const GTEXT_YAML_Node_Type want =
			gtext_yaml_node_type(gtext_yaml_document_root(doc));

		for (int block = 0; block < 2; block++) {
			Written w = write_doc(doc, block != 0);
			ASSERT_EQ(w.status, GTEXT_YAML_OK) << src;
			memset(&err, 0, sizeof(err));
			GTEXT_YAML_Document *back =
				gtext_yaml_parse(w.text.data(), w.text.size(), &opts, &err);
			ASSERT_NE(back, nullptr) << src << " wrote " << w.text;
			gtext_yaml_error_free(&err);
			EXPECT_EQ(gtext_yaml_node_type(gtext_yaml_document_root(back)), want)
				<< src << " wrote " << w.text;
			gtext_yaml_free(back);
		}
		gtext_yaml_free(doc);
	}
}

/* A scalar has to be written in a style that reads back as a scalar.
   ns-plain-first admits "-" only when an ns-plain-safe character follows it;
   a lone "-" on a line is a block sequence entry, so the string "-" written
   plain came back as a sequence holding one empty node. */
TEST(YamlWriterContract, AScalarIsNeverWrittenAsSomethingElse) {
	const char *values[] = { "-", "- ", "-\tx", "- x" };
	for (const char *value : values) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *root =
			gtext_yaml_node_new_scalar(doc, value, nullptr, nullptr);
		gtext_yaml_document_set_root(doc, root);
		for (int block = 0; block < 2; block++) {
			Written w = write_doc(doc, block != 0);
			ASSERT_EQ(w.status, GTEXT_YAML_OK) << value;
			std::string back;
			ASSERT_TRUE(read_back_scalar(w.text, &back))
				<< "<<" << value << ">> wrote " << w.text;
			EXPECT_EQ(back, value) << "wrote " << w.text;
		}
		gtext_yaml_free(doc);
	}

	/* "-1" and "-x" are ordinary plain scalars: something plain-safe follows
	   the "-", which is the whole of the rule. */
	const char *plain[] = { "-1", "-x", "-.inf" };
	for (const char *value : plain) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *root =
			gtext_yaml_node_new_scalar(doc, value, nullptr, nullptr);
		gtext_yaml_document_set_root(doc, root);
		Written w = write_doc(doc);
		gtext_yaml_free(doc);
		ASSERT_EQ(w.status, GTEXT_YAML_OK) << value;
		EXPECT_EQ(w.text, std::string(value)) << "quoted unnecessarily";
	}
}

/* A line width may change how a value is spelled, not what it says.
 *
 * write_scalar_node() forces every non-string scalar to PLAIN, precisely so no
 * style can change its type, and its comment allows plan_scalar_style() to
 * upgrade PLAIN only "where the text cannot be written plain at all". The
 * width-driven fold sat inside that exception without belonging to it: a
 * number that does not fit the line is text that can be written plain
 * perfectly well and merely does not fit. A folded block scalar is never a
 * plain one, so 10.3.2 gives it the string tag whatever it holds.
 *
 * So an int of 222 written with line_width 2 came back as the string "222",
 * and the fuzzer's case was a 32-digit float that came back as a 32-character
 * string. Across a sweep of five types x five requested styles x six widths x
 * both dialects x the core and JSON schemas, 200 of 840 combinations changed
 * the type before the fix and none after it.
 *
 * The bound is not "tiny widths". It is any width the plain spelling exceeds,
 * which is why the long float below is tested at 8 as well as at 2 - it fails
 * at 8 where "222" does not.
 *
 * Found by the yaml-writer fuzz leg on the EVO-X2 at 4,153,232 executions,
 * through the built-DOM arm: a corpus of YAML text cannot reach this, because
 * a parsed number already has a spelling that fits whatever line it arrived
 * on. It needed a document built through the API with a width set.
 */
TEST(YamlWriterContract, ALineWidthDoesNotFoldANonStringIntoAString) {
	struct Case {
		const char *text;
		GTEXT_YAML_Node_Type type;
		GTEXT_YAML_Node_Type expect;
	};
	const Case cases[] = {
		{"222", GTEXT_YAML_INT, GTEXT_YAML_INT},
		{"2.5", GTEXT_YAML_FLOAT, GTEXT_YAML_FLOAT},
		{"true", GTEXT_YAML_BOOL, GTEXT_YAML_BOOL},
		{"null", GTEXT_YAML_NULL, GTEXT_YAML_NULL},
		{"222222222222222222222223.2222", GTEXT_YAML_FLOAT, GTEXT_YAML_FLOAT},
		/* A string stays a string, and that is not the interesting half: it
		   must still be *folded*, which the block below checks. */
		{"aaaa bbbb cccc dddd", GTEXT_YAML_STRING, GTEXT_YAML_STRING},
	};
	const int widths[] = {1, 2, 4, 8, 80};
	/* Every requested style, because the rule is about the node's type and not
	   about what the caller asked for - and because a fix that only looked at
	   opts->scalar_style would pass with PLAIN still folding. */
	for (int style = 0; style < 5; style++) {
		for (int width : widths) {
			for (const Case &c : cases) {
				GTEXT_YAML_Document *doc =
					gtext_yaml_document_new(nullptr, nullptr);
				ASSERT_NE(doc, nullptr);
				GTEXT_YAML_Node *root = gtext_yaml_node_new_scalar_typed(
					doc, c.text, strlen(c.text), c.type, nullptr, nullptr);
				ASSERT_NE(root, nullptr) << c.text;
				gtext_yaml_node_set_scalar_style(
					root, (GTEXT_YAML_Scalar_Style)style);
				gtext_yaml_document_set_root(doc, root);

				GTEXT_YAML_Sink sink;
				ASSERT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
				GTEXT_YAML_Write_Options opts =
					gtext_yaml_write_options_default();
				opts.pretty = true;
				opts.indent_spaces = 2;
				opts.line_width = width;
				opts.scalar_style = (GTEXT_YAML_Scalar_Style)style;
				const GTEXT_YAML_Status st =
					gtext_yaml_write_document(doc, &sink, &opts);
				std::string text;
				if (st == GTEXT_YAML_OK) {
					text.assign(gtext_yaml_sink_buffer_data(&sink),
						gtext_yaml_sink_buffer_size(&sink));
				}
				gtext_yaml_sink_buffer_free(&sink);
				gtext_yaml_free(doc);
				ASSERT_EQ(st, GTEXT_YAML_OK) << c.text << " width " << width;

				GTEXT_YAML_Error err;
				memset(&err, 0, sizeof(err));
				GTEXT_YAML_Parse_Options popts =
					gtext_yaml_parse_options_default();
				GTEXT_YAML_Document *back =
					gtext_yaml_parse(text.data(), text.size(), &popts, &err);
				gtext_yaml_error_free(&err);
				ASSERT_NE(back, nullptr)
					<< "style " << style << " width " << width << " <<"
					<< c.text << ">> wrote <<" << text << ">>";
				EXPECT_EQ(gtext_yaml_node_type(gtext_yaml_document_root(back)),
					c.expect)
					<< "style " << style << " width " << width << " <<"
					<< c.text << ">> wrote <<" << text << ">>";
				gtext_yaml_free(back);
			}
		}
	}

	/* The other half: a long *string* must still be folded when the width asks
	   for it. Without this the test would pass with the fold removed outright,
	   which would preserve every type and silently drop the feature. */
	{
		const char *value = "aaaa bbbb cccc dddd";
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *root = gtext_yaml_node_new_scalar_typed(
			doc, value, strlen(value), GTEXT_YAML_STRING, nullptr, nullptr);
		ASSERT_NE(root, nullptr);
		gtext_yaml_document_set_root(doc, root);
		GTEXT_YAML_Sink sink;
		ASSERT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
		GTEXT_YAML_Write_Options opts = gtext_yaml_write_options_default();
		opts.pretty = true;
		opts.indent_spaces = 2;
		opts.line_width = 8;
		const GTEXT_YAML_Status st =
			gtext_yaml_write_document(doc, &sink, &opts);
		std::string text(gtext_yaml_sink_buffer_data(&sink),
			gtext_yaml_sink_buffer_size(&sink));
		gtext_yaml_sink_buffer_free(&sink);
		gtext_yaml_free(doc);
		ASSERT_EQ(st, GTEXT_YAML_OK);
		EXPECT_NE(text.find('>'), std::string::npos)
			<< "a long string should still fold: <<" << text << ">>";
		std::string back;
		ASSERT_TRUE(read_back_scalar(text, &back)) << text;
		EXPECT_EQ(back, value);
	}
}

/* A YAML stream is a stream of *characters*, and every escape of 5.7 names a
   code point, so a byte that is not part of any UTF-8 sequence has no
   spelling at all. Writing "\\xFF" for the byte 0xFF would read back as
   U+00FF - two bytes, a different value - so it is refused rather than
   quietly changed into the character that happens to share its number. The
   writer fuzzer found this one. */
TEST(YamlWriterContract, AScalarThatIsNotUtf8IsRefused) {
	const std::string bad[] = {
		std::string("\xff", 1),
		std::string("a\xff" "b", 3),
		std::string("\xc2", 1),          /* a lead byte with no continuation */
		std::string("\xe0\x80", 2),      /* truncated three-byte sequence */
		std::string("\xed\xa0\x80", 3),  /* a surrogate, which UTF-8 excludes */
	};
	for (const std::string &value : bad) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *root = gtext_yaml_node_new_scalar_n(
			doc, value.data(), value.size(), nullptr, nullptr);
		gtext_yaml_document_set_root(doc, root);
		Written w = write_doc(doc);
		gtext_yaml_free(doc);
		EXPECT_NE(w.status, GTEXT_YAML_OK)
			<< "wrote " << w.text << " for " << value.size()
			<< " bytes that are not UTF-8";
	}
}

/* 6.9.2: ns-anchor-char is ns-char minus c-flow-indicator.  The writer used
   to emit "&" and whatever string it held, which for "a b" produced "&a b" -
   read back as the anchor "a" with the rest of the line as the value. */
TEST(YamlWriterContract, AnAnchorNameIsCheckedBeforeItIsWritten) {
	const char *refused[] = { "", "a b", "a\nb", "a\tb", "a[b", "a]b",
		"a{b", "a}b", "a,b", "a\x7f" "b" };
	for (const char *name : refused) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *root =
			gtext_yaml_node_new_scalar(doc, "v", nullptr, name);
		gtext_yaml_document_set_root(doc, root);
		EXPECT_NE(write_doc(doc).status, GTEXT_YAML_OK)
			<< "anchor <<" << name << ">> was written anyway";
		gtext_yaml_free(doc);
	}

	/* And everything the production does allow is written and read back.
	   Nine of these - ! " # % & ' * | > - were refused by the *parser* at the
	   first position of a name until the writer started producing them. */
	const char *accepted[] = { "a", "a!b", "!x", "\"x", "#x", "%x", "&x",
		"'x", "*x", "|x", ">x", ":x", "-x", "?x", "@x", "a:b" };
	for (const char *name : accepted) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *root =
			gtext_yaml_node_new_scalar(doc, "v", nullptr, name);
		gtext_yaml_document_set_root(doc, root);
		Written w = write_doc(doc);
		gtext_yaml_free(doc);
		ASSERT_EQ(w.status, GTEXT_YAML_OK) << "anchor <<" << name << ">>";

		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
		GTEXT_YAML_Document *back =
			gtext_yaml_parse(w.text.data(), w.text.size(), &opts, &err);
		ASSERT_NE(back, nullptr) << "anchor <<" << name << ">> wrote "
			<< w.text << " which this parser refuses: "
			<< (err.message ? err.message : "");
		gtext_yaml_error_free(&err);
		const char *got = gtext_yaml_node_anchor(gtext_yaml_document_root(back));
		EXPECT_STREQ(got ? got : "", name);
		gtext_yaml_free(back);
	}
}

/* A verbatim tag is used exactly as written (5.3), so the reader does not
   decode its escapes and the writer must not add any.  It used to encode "%"
   unconditionally, and with no matching decode a tag grew a layer of escaping
   on every round trip: "!a%21b" -> "!a%2521b" -> "!a%252521b". */
TEST(YamlWriterContract, ATagDoesNotGrowOnEveryRoundTrip) {
	const char *tags[] = {
		"!local", "!!str", "tag:example.com,2000:app/foo",
		"!a%21b", "!a%2521b", "tag:e.com,2000:100%25",
	};
	for (const char *tag : tags) {
		std::string current = tag;
		for (int pass = 0; pass < 3; pass++) {
			GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
			GTEXT_YAML_Node *root =
				gtext_yaml_node_new_scalar(doc, "v", current.c_str(), nullptr);
			gtext_yaml_document_set_root(doc, root);
			Written w = write_doc(doc);
			gtext_yaml_free(doc);
			ASSERT_EQ(w.status, GTEXT_YAML_OK) << tag;

			GTEXT_YAML_Error err;
			memset(&err, 0, sizeof(err));
			GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
			GTEXT_YAML_Document *back =
				gtext_yaml_parse(w.text.data(), w.text.size(), &opts, &err);
			ASSERT_NE(back, nullptr) << tag << " wrote " << w.text;
			gtext_yaml_error_free(&err);
			const char *got = gtext_yaml_node_tag(gtext_yaml_document_root(back));
			ASSERT_NE(got, nullptr) << tag;
			EXPECT_EQ(std::string(got), std::string(tag))
				<< "pass " << pass << " wrote " << w.text;
			current = got;
			gtext_yaml_free(back);
		}
	}
}

/* A tag that is not ns-uri-char+ has no spelling at all, so it is refused
   rather than approximated - and so is one in the "tag:yaml.org,2002:"
   namespace naming a type the spec does not define, since that namespace is
   not the author's to extend and the parser refuses "!!bogus" whatever the
   options say. The DOM API validates neither, so both reach the writer. */
TEST(YamlWriterContract, ATagThatCannotBeSpelledIsRefused) {
	const char *refused[] = { "!a b", "!a\nb", "tag:e.com,2000:caf\xc3\xa9",
		"!a\"b", "!a%zzb", "!a{b",
		"!!bogus", "!!-.#", "!!", "tag:yaml.org,2002:bogus" };
	for (const char *tag : refused) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *root =
			gtext_yaml_node_new_scalar(doc, "v", tag, nullptr);
		gtext_yaml_document_set_root(doc, root);
		EXPECT_NE(write_doc(doc).status, GTEXT_YAML_OK)
			<< "tag <<" << tag << ">> was written anyway";
		gtext_yaml_free(doc);
	}
}

/* "\0" is an escape 5.7 defines, so a scalar may hold a NUL.  The DOM has
   always kept a length; until gtext_yaml_node_scalar_length() there was no
   way to read past the NUL, and no constructor that could put one there. */
TEST(YamlWriterContract, AScalarMayHoldANul) {
	const std::string value("a\0b\0", 4);
	GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
	GTEXT_YAML_Node *root = gtext_yaml_node_new_scalar_n(
		doc, value.data(), value.size(), nullptr, nullptr);
	ASSERT_NE(root, nullptr);
	gtext_yaml_document_set_root(doc, root);
	EXPECT_EQ(gtext_yaml_node_scalar_length(root), value.size());

	Written w = write_doc(doc);
	gtext_yaml_free(doc);
	ASSERT_EQ(w.status, GTEXT_YAML_OK);
	std::string back;
	ASSERT_TRUE(read_back_scalar(w.text, &back)) << w.text;
	EXPECT_EQ(back, value);
}

namespace {

/* The path a caller of the event API uses: the streaming parser straight into
 * the streaming writer.  Every other measurement of the writers reaches them
 * through a DOM, and a DOM walk reports no directives, no comments, and a tag
 * already resolved - so nothing could see the directives being dropped. */
struct Pipe {
	GTEXT_YAML_Writer *writer;
	GTEXT_YAML_Status status;
};

GTEXT_YAML_Status pipe_event(GTEXT_YAML_Stream *s, const void *ev, void *user) {
	(void)s;
	Pipe *p = (Pipe *)user;
	if (p->status != GTEXT_YAML_OK) return p->status;
	p->status = gtext_yaml_writer_event(p->writer, (const GTEXT_YAML_Event *)ev);
	return p->status;
}

bool pipe_through(const std::string &in, std::string *out) {
	GTEXT_YAML_Sink sink;
	if (gtext_yaml_sink_buffer(&sink) != GTEXT_YAML_OK) return false;
	GTEXT_YAML_Write_Options wopts = gtext_yaml_write_options_default();
	Pipe pipe{ gtext_yaml_writer_new(sink, &wopts), GTEXT_YAML_OK };
	if (!pipe.writer) { gtext_yaml_sink_buffer_free(&sink); return false; }

	GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
	GTEXT_YAML_Stream *st = gtext_yaml_stream_new(&popts, pipe_event, &pipe);
	GTEXT_YAML_Status status = gtext_yaml_stream_feed(st, in.data(), in.size());
	if (status == GTEXT_YAML_OK) status = gtext_yaml_stream_finish(st);
	if (status == GTEXT_YAML_OK) status = pipe.status;
	if (status == GTEXT_YAML_OK) status = gtext_yaml_writer_finish(pipe.writer);
	if (status == GTEXT_YAML_OK) {
		out->assign(gtext_yaml_sink_buffer_data(&sink),
			gtext_yaml_sink_buffer_size(&sink));
	}
	gtext_yaml_stream_free(st);
	gtext_yaml_writer_free(pipe.writer);
	gtext_yaml_sink_buffer_free(&sink);
	return status == GTEXT_YAML_OK;
}

}  // namespace

/* The streaming writer used to answer a DIRECTIVE event with OK and write
   nothing.  The event stream reports a tag as it was written rather than
   resolved, so "!e!foo" arrives still spelled with its handle; dropping the
   "%TAG !e! ..." that declared the handle left a document this very parser
   refuses. */
TEST(YamlWriterContract, TheStreamingWriterKeepsTheDirectives) {
	const std::string in =
		"%TAG !e! tag:example.com,2000:app/\n"
		"---\n"
		"!e!foo v\n";
	std::string out;
	ASSERT_TRUE(pipe_through(in, &out));
	EXPECT_NE(out.find("%TAG !e! tag:example.com,2000:app/"), std::string::npos)
		<< "wrote: " << out;

	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	GTEXT_YAML_Document *back =
		gtext_yaml_parse(out.data(), out.size(), &opts, &err);
	ASSERT_NE(back, nullptr) << "wrote " << out << " which this parser refuses: "
		<< (err.message ? err.message : "");
	gtext_yaml_error_free(&err);
	const char *tag = gtext_yaml_node_tag(gtext_yaml_document_root(back));
	ASSERT_NE(tag, nullptr);
	EXPECT_STREQ(tag, "tag:example.com,2000:app/foo");
	gtext_yaml_free(back);
}

/* ...and a named handle is writable only where this writer declared it.

   c-ns-shorthand-tag is a handle followed by ns-tag-char+ (6.8.2). The
   primary "!" and the secondary "!!" are defined for every document; a named
   "!e!" means whatever a %TAG declared and means nothing at all where none
   did. The DOM writer emits no directives, so a named handle is undeclared
   there by construction - and "!a!3" was going out as itself, leaving a
   document this parser refuses for a handle no %TAG defined.

   There is nothing to fall back on. "!<!a!3>" is a *different* tag - the
   literal URI, not the handle's prefix followed by "3" - and a prefix the
   writer invented would be worse than a refusal. So it is refused, the way an
   unwritable anchor and "!!bogus" already are. The test above is the other
   half of this rule: declare the handle and the same shorthand writes. */
TEST(YamlWriterContract, ANamedTagHandleIsRefusedWhereNoTagDeclaredIt) {
	struct Case { const char *tag; bool writable; };
	const Case cases[] = {
		{ "!a!3", false },   /* the fuzzer's find */
		{ "!a!3x", false },
		{ "!e!foo", false },
		{ "!", true },       /* the primary handle, always defined */
		{ "!local", true },
		{ "!!str", true },   /* the secondary, likewise */
	};
	for (const Case &c : cases) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *node =
			gtext_yaml_node_new_scalar(doc, "v", c.tag, nullptr);
		ASSERT_NE(node, nullptr) << c.tag;
		gtext_yaml_document_set_root(doc, node);
		Written w = write_doc(doc);
		EXPECT_EQ(w.status == GTEXT_YAML_OK, c.writable)
			<< "tag " << c.tag << " wrote " << w.text;

		/* And what it does write has to read back, which is the property
		   that failed: the bytes were fine, the handle was not. */
		if (w.status == GTEXT_YAML_OK) {
			GTEXT_YAML_Error err;
			memset(&err, 0, sizeof(err));
			GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
			GTEXT_YAML_Document *back =
				gtext_yaml_parse(w.text.c_str(), w.text.size(), &opts, &err);
			EXPECT_NE(back, nullptr) << "tag " << c.tag << " wrote " << w.text
				<< " which this parser refuses: "
				<< (err.message ? err.message : "");
			gtext_yaml_error_free(&err);
			if (back) gtext_yaml_free(back);
		}
		gtext_yaml_free(doc);
	}
}

/* A %TAG declares its handle for the document it precedes and no further
   (6.8.2), so the same shorthand in a second document is undeclared again. */
TEST(YamlWriterContract, ADeclaredHandleDoesNotCarryToTheNextDocument) {
	std::string out;
	ASSERT_TRUE(pipe_through(
		"%TAG !e! tag:example.com,2000:app/\n---\n!e!foo v\n", &out));
	EXPECT_NE(out.find("%TAG !e!"), std::string::npos) << "wrote: " << out;

	/* Two documents, the handle declared only for the first: the second's
	   shorthand has no declaration and the writer must not write it. */
	std::string ignored;
	EXPECT_FALSE(pipe_through(
		"%TAG !e! tag:example.com,2000:app/\n---\n!e!foo v\n"
		"--- !e!bar w\n", &ignored))
		<< "wrote: " << ignored;
}

/* A type claim has to be true of the text, with or without a tag.
 *
 * gtext_yaml_node_new_scalar_typed() checked the claim only when the node
 * also carried a tag, on the reasoning that a tag is an assertion that can be
 * false. True, and beside the point: the tag is not what makes the claim
 * checkable, the *type* is. "NO" declared null is exactly as false with a tag
 * as without one, and only the tagged spelling was refused.
 *
 * The node that got through could not be written by anything - canonical form
 * emitted '!!null "NO"', which this library refuses to read, and plain form
 * emitted NO, which reads back as the string. The contradiction was in the
 * node.
 *
 * gtext_yaml_node_new_scalar() cannot build one, because it takes the type
 * from the text rather than from a caller. This is the only door. */
TEST(YamlWriterContract, ATypeClaimHasToBeTrueOfTheTextWithoutATagToo) {
	struct Case { const char *text; GTEXT_YAML_Node_Type type; bool ok; };
	const Case cases[] = {
		/* The claim the write-up was opened on. */
		{ "NO", GTEXT_YAML_NULL,  false },
		{ "NO", GTEXT_YAML_BOOL,  false },
		{ "NO", GTEXT_YAML_INT,   false },
		{ "NO", GTEXT_YAML_FLOAT, false },
		/* Any text is a string, so a string is never refused. */
		{ "NO", GTEXT_YAML_STRING, true },
		{ "",   GTEXT_YAML_STRING, true },
		/* The texts that made the conversion undefined: a float spelling
		   claimed as an integer. */
		{ ".INF",  GTEXT_YAML_INT, false },
		{ "-.INF", GTEXT_YAML_INT, false },
		{ ".nan",  GTEXT_YAML_INT, false },
		{ "1.9",   GTEXT_YAML_INT, false },
		/* ...and the same texts claimed as what they are. */
		{ ".INF",  GTEXT_YAML_FLOAT, true },
		{ "-.INF", GTEXT_YAML_FLOAT, true },
		{ ".nan",  GTEXT_YAML_FLOAT, true },
		{ "1.9",   GTEXT_YAML_FLOAT, true },
		/* An integer spelling is a float spelling too: 10.3.2's float row
		   makes the fraction optional. Not the other way about. */
		{ "12",   GTEXT_YAML_INT,   true },
		{ "12",   GTEXT_YAML_FLOAT, true },
		{ "0x10", GTEXT_YAML_INT,   true },
		/* Text that resolves to no number at all is neither. */
		{ "1e400",                   GTEXT_YAML_FLOAT, false },
		{ "99999999999999999999999", GTEXT_YAML_INT,   false },
		/* The null and bool spellings 1.2 core does have. */
		{ "~",     GTEXT_YAML_NULL, true },
		{ "null",  GTEXT_YAML_NULL, true },
		{ "",      GTEXT_YAML_NULL, true },
		{ "true",  GTEXT_YAML_BOOL, true },
		{ "false", GTEXT_YAML_BOOL, true },
		{ "yes",   GTEXT_YAML_BOOL, false },
	};
	for (const Case &c : cases) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *node = gtext_yaml_node_new_scalar_typed(
			doc, c.text, strlen(c.text), c.type, nullptr, nullptr);
		EXPECT_EQ(node != nullptr, c.ok)
			<< "typed(" << (int)c.type << ") of <<" << c.text << ">>";
		if (node) {
			EXPECT_EQ(gtext_yaml_node_type(node), c.type) << c.text;
		}
		gtext_yaml_free(doc);
	}
}

/* The same claim through both doors gets the same answer.
 *
 * This is the property the gate was hiding rather than a second list: what
 * the typed constructor accepts for a type is what this library's own parser
 * accepts behind the tag that names it. A case table can be made to agree
 * with a mistake; this cannot, because the parser is not the code under
 * test. */
TEST(YamlWriterContract, TheTypedConstructorAgreesWithTheTagOnTheWayIn) {
	struct Case { const char *text; GTEXT_YAML_Node_Type type; const char *tag; };
	const Case cases[] = {
		{ ".INF",  GTEXT_YAML_INT,   "!!int" },
		{ ".INF",  GTEXT_YAML_FLOAT, "!!float" },
		{ "1.9",   GTEXT_YAML_INT,   "!!int" },
		{ "1.9",   GTEXT_YAML_FLOAT, "!!float" },
		{ "12",    GTEXT_YAML_INT,   "!!int" },
		{ "12",    GTEXT_YAML_FLOAT, "!!float" },
		{ "NO",    GTEXT_YAML_INT,   "!!int" },
		{ "NO",    GTEXT_YAML_NULL,  "!!null" },
		{ "NO",    GTEXT_YAML_BOOL,  "!!bool" },
		{ "~",     GTEXT_YAML_NULL,  "!!null" },
		{ "true",  GTEXT_YAML_BOOL,  "!!bool" },
		{ "1e400", GTEXT_YAML_FLOAT, "!!float" },
		{ "99999999999999999999999", GTEXT_YAML_INT, "!!int" },
	};
	for (const Case &c : cases) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *built = gtext_yaml_node_new_scalar_typed(
			doc, c.text, strlen(c.text), c.type, nullptr, nullptr);

		const std::string source = std::string(c.tag) + " " + c.text;
		GTEXT_YAML_Document *parsed =
			gtext_yaml_parse(source.c_str(), source.size(), nullptr, nullptr);
		const GTEXT_YAML_Node *root =
			parsed ? gtext_yaml_document_root(parsed) : nullptr;
		const bool parser_took_it =
			root && gtext_yaml_node_type(root) == c.type;

		EXPECT_EQ(built != nullptr, parser_took_it)
			<< "<<" << source << ">>: constructor "
			<< (built ? "built" : "refused") << ", parser "
			<< (parser_took_it ? "took it" : "did not");

		if (parsed) gtext_yaml_free(parsed);
		gtext_yaml_free(doc);
	}
}

/* An omap is an ordered mapping, and both halves of that have to hold.
 *
 * !!omap takes a sequence of single-pair mappings whose keys are unique. The
 * resolver holds a parsed one to both rules; the DOM appenders held it to
 * neither, so an omap with the key "a" twice was built happily, written as
 * "!!omap [{a: 1}, {a: 2}]", and refused by this library's own parser -
 * "omap keys must be unique". The writer fuzzer built exactly that.
 *
 * !!pairs is the type that takes duplicate keys; that is what distinguishes
 * the two, so it is deliberately not checked. */
TEST(YamlWriterContract, AnOmapWillNotTakeAKeyItAlreadyHas) {
	auto pair = [](GTEXT_YAML_Document *doc, const char *k, const char *v) {
		GTEXT_YAML_Node *m = gtext_yaml_node_new_mapping(doc, nullptr, nullptr);
		return gtext_yaml_mapping_set(doc, m,
			gtext_yaml_node_new_scalar(doc, k, nullptr, nullptr),
			gtext_yaml_node_new_scalar(doc, v, nullptr, nullptr));
	};

	/* Both appenders, because both could build it. */
	for (int insert = 0; insert < 2; ++insert) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *omap = gtext_yaml_node_new_omap(doc, nullptr, nullptr);
		omap = gtext_yaml_sequence_append(doc, omap, pair(doc, "a", "1"));
		ASSERT_NE(omap, nullptr);

		/* The same key again is refused... */
		GTEXT_YAML_Node *again = insert
			? gtext_yaml_sequence_insert(doc, omap, 0, pair(doc, "a", "2"))
			: gtext_yaml_sequence_append(doc, omap, pair(doc, "a", "2"));
		EXPECT_EQ(again, nullptr) << (insert ? "insert" : "append");

		/* ...a different one is not, so this is a rule and not a refusal to
		   grow. */
		GTEXT_YAML_Node *other = insert
			? gtext_yaml_sequence_insert(doc, omap, 0, pair(doc, "b", "2"))
			: gtext_yaml_sequence_append(doc, omap, pair(doc, "b", "2"));
		ASSERT_NE(other, nullptr) << (insert ? "insert" : "append");

		/* An entry that is not a single-pair mapping cannot go in either:
		   the resolver refuses one, so the constructor has to as well. */
		EXPECT_EQ(gtext_yaml_sequence_append(doc, other,
			gtext_yaml_node_new_scalar(doc, "loose", nullptr, nullptr)),
			nullptr);
		GTEXT_YAML_Node *two = gtext_yaml_node_new_mapping(doc, nullptr, nullptr);
		two = gtext_yaml_mapping_set(doc, two,
			gtext_yaml_node_new_scalar(doc, "c", nullptr, nullptr),
			gtext_yaml_node_new_scalar(doc, "3", nullptr, nullptr));
		two = gtext_yaml_mapping_set(doc, two,
			gtext_yaml_node_new_scalar(doc, "d", nullptr, nullptr),
			gtext_yaml_node_new_scalar(doc, "4", nullptr, nullptr));
		EXPECT_EQ(gtext_yaml_sequence_append(doc, other, two), nullptr)
			<< "a two-pair mapping is not an omap entry";

		/* What was built is what this library reads back. */
		gtext_yaml_document_set_root(doc, other);
		Written w = write_doc(doc);
		ASSERT_EQ(w.status, GTEXT_YAML_OK);
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
		GTEXT_YAML_Document *back =
			gtext_yaml_parse(w.text.data(), w.text.size(), &popts, &err);
		ASSERT_NE(back, nullptr) << "wrote <<" << w.text << ">>: "
			<< (err.message ? err.message : "");
		gtext_yaml_error_free(&err);
		EXPECT_EQ(gtext_yaml_sequence_length(gtext_yaml_document_root(back)), 2u)
			<< "wrote <<" << w.text << ">>";
		gtext_yaml_free(back);
		gtext_yaml_free(doc);
	}

	/* !!pairs exists to hold what an omap will not, and still does. */
	GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
	GTEXT_YAML_Node *pairs = gtext_yaml_node_new_pairs(doc, nullptr, nullptr);
	pairs = gtext_yaml_sequence_append(doc, pairs, pair(doc, "a", "1"));
	pairs = gtext_yaml_sequence_append(doc, pairs, pair(doc, "a", "2"));
	ASSERT_NE(pairs, nullptr) << "!!pairs refused a duplicate key";
	gtext_yaml_document_set_root(doc, pairs);
	Written w = write_doc(doc);
	ASSERT_EQ(w.status, GTEXT_YAML_OK);
	GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
	GTEXT_YAML_Document *back =
		gtext_yaml_parse(w.text.data(), w.text.size(), &popts, nullptr);
	ASSERT_NE(back, nullptr) << "wrote <<" << w.text << ">>";
	EXPECT_EQ(gtext_yaml_sequence_length(gtext_yaml_document_root(back)), 2u);
	gtext_yaml_free(back);
	gtext_yaml_free(doc);
}

/* Quoting is a free choice only for a string.
 *
 * scalar_needs_quotes() admits a small whitelist and quotes everything else,
 * and the note above it says why that is safe: "quoting is value-preserving
 * here: neither text resolves to anything but a string". True of a string,
 * and of nothing else. ":" is not on the whitelist; "1:30" is nonetheless a
 * legal plain scalar - 7.3.3 admits ":" where an ns-plain-safe character
 * follows it - and parsed with yaml_1_1 it is the sexagesimal integer 90. It
 * was written "1:30" in quotes and came back the string.
 *
 * The witness was "0:0" until the resolver was corrected to 10.3.2's actual
 * 1.1 int row, "[-+]? [1-9] [0-9_]* (: [0-5]? [0-9])+", whose leading "[1-9]"
 * makes "0:0" a string in 1.1 as well as in 1.2 - which PyYAML agrees with.
 * A string witness would have left every assertion below passing and nothing
 * being measured, since each one asks whether a *non-string* survived.
 *
 * The writer fuzzer found it through the event pipe, where the scalar arrives
 * plain and has to leave plain, because plainness is what carries the type. */
TEST(YamlWriterContract, AColonDoesNotForceQuotesOntoANonString) {
	/* 7.3.3 admits ":" mid-scalar when what follows is ns-plain-safe, and in
	   flow context the flow indicators are not. The first position is left
	   alone deliberately: ":" there is how a block mapping writes a value
	   with no key. */
	struct Case { const char *text; bool needs_quotes; };
	const Case cases[] = {
		{ "1:30",    false }, { "1:30:00", false },
		/* A string in both dialects now, and still a legal plain scalar: the
		   whitelist question is about plainness and not about type. */
		{ "0:0",     false },
		{ "a:b",     false }, { "x:1",     false },
		/* A trailing colon has nothing safe after it. */
		{ "a:",      true  },
		/* A leading one is left quoted whatever follows. */
		{ ":a",      true  }, { "::a",     true  },
		/* A space between plain characters is separation inside the scalar,
		   and 7.3.3 admits "#" where an ns-char precedes it - both were
		   quoted while this test read a whitelist rather than the rule. */
		{ "word",    false }, { "a b",     false }, { "a#b",     false },
		/* And the two shapes that are not the rule: white space at an edge is
		   separation the scanner removes, and a "#" after white space starts
		   a comment. */
		{ " a",      true  }, { "a ",      true  }, { "a #b",    true  },
	};

	for (const Case &c : cases) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *root = gtext_yaml_node_new_scalar_typed(
			doc, c.text, strlen(c.text), GTEXT_YAML_STRING, nullptr, nullptr);
		ASSERT_NE(root, nullptr) << c.text;
		gtext_yaml_document_set_root(doc, root);
		Written w = write_doc(doc, true);
		ASSERT_EQ(w.status, GTEXT_YAML_OK) << c.text;
		EXPECT_EQ(w.text.find('"') != std::string::npos, c.needs_quotes)
			<< "<<" << c.text << ">> was written <<" << w.text << ">>";
		gtext_yaml_free(doc);
	}

	/* And the property that makes it matter: a 1.1 sexagesimal integer keeps
	   its value across a round trip, in every position a scalar can stand and
	   in both styles. The oracle is the parser, not the quoting rule. */
	const char *inputs[] = {
		"1:30", "k: 1:30", "1:30: v", "[1:30]", "{k: 1:30}" };
	for (const char *input : inputs) {
		for (int block = 0; block < 2; ++block) {
			GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
			popts.yaml_1_1 = true;
			GTEXT_YAML_Document *doc =
				gtext_yaml_parse(input, strlen(input), &popts, nullptr);
			ASSERT_NE(doc, nullptr) << input;

			GTEXT_YAML_Sink sink;
			ASSERT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
			GTEXT_YAML_Write_Options wopts = gtext_yaml_write_options_default();
			wopts.yaml_1_1 = true;
			if (block) {
				wopts.pretty = true;
				wopts.flow_style = GTEXT_YAML_FLOW_STYLE_BLOCK;
			}
			ASSERT_EQ(gtext_yaml_write_document(doc, &sink, &wopts),
				GTEXT_YAML_OK) << input;
			const std::string out(gtext_yaml_sink_buffer_data(&sink),
				gtext_yaml_sink_buffer_size(&sink));
			gtext_yaml_sink_buffer_free(&sink);

			GTEXT_YAML_Document *back =
				gtext_yaml_parse(out.data(), out.size(), &popts, nullptr);
			ASSERT_NE(back, nullptr) << input << " wrote <<" << out << ">>";

			/* Find the scalar that was "1:30" in each shape and check it is
			   still the integer it parsed as. */
			const GTEXT_YAML_Node *r1 = gtext_yaml_document_root(doc);
			const GTEXT_YAML_Node *r2 = gtext_yaml_document_root(back);
			ASSERT_EQ(gtext_yaml_node_type(r1), gtext_yaml_node_type(r2))
				<< input << " wrote <<" << out << ">>";
			if (gtext_yaml_node_type(r1) == GTEXT_YAML_INT) {
				int64_t a = -1;
				int64_t b = -2;
				EXPECT_TRUE(gtext_yaml_node_as_int(r1, &a));
				EXPECT_TRUE(gtext_yaml_node_as_int(r2, &b));
				EXPECT_EQ(a, b) << input << " wrote <<" << out << ">>";
			}
			else if (gtext_yaml_node_type(r1) == GTEXT_YAML_MAPPING) {
				EXPECT_EQ(gtext_yaml_mapping_size(r1),
					gtext_yaml_mapping_size(r2))
					<< input << " wrote <<" << out << ">>";
			}
			else if (gtext_yaml_node_type(r1) == GTEXT_YAML_SEQUENCE) {
				ASSERT_EQ(gtext_yaml_sequence_length(r2), 1u)
					<< input << " wrote <<" << out << ">>";
				EXPECT_EQ(
					gtext_yaml_node_type(gtext_yaml_sequence_get(r2, 0)),
					GTEXT_YAML_INT) << input << " wrote <<" << out << ">>";
			}
			gtext_yaml_free(back);
			gtext_yaml_free(doc);
		}
	}
}

/* The writer has to be told which dialect its output is for.
 *
 * Only a plain scalar is resolved by its contents (10.3.2), so whether a
 * scalar may go out plain is a question about what the *reader* resolves -
 * and the writer had no way to be told, so it answered with the 1.2 core
 * schema always. Every other dialect then read back something that was not
 * written. Under 1.1 the damage is the quiet kind: the string "yes" went out
 * plain and came back as the bool true, and "012" as the integer 10.
 *
 * The oracle is this library's own parser, told the same dialect. That is the
 * point of the option - not that some table of spellings is right, but that
 * the two ends agree. */
TEST(YamlWriterContract, AStringSurvivesTheDialectItIsWrittenFor) {
	/* Spellings 1.1 resolves and 1.2 does not, plus controls that neither
	   does and that both do. */
	const char *texts[] = {
		"yes", "no", "on", "off", "y", "n", "Yes", "OFF",
		"012", "0b101", "1_000", "1:30", "1:30:00",
		/* Both dialects resolve these; both have to quote them. */
		"true", "null", "~", "12", "1.5", "0x1f",
		/* Neither does: these may stay plain in either. */
		"word", "x12y", "a.b",
	};

	struct Dialect { GTEXT_YAML_Schema schema; bool v11; const char *name; };
	const Dialect dialects[] = {
		{ GTEXT_YAML_SCHEMA_CORE,     false, "1.2 core" },
		{ GTEXT_YAML_SCHEMA_CORE,     true,  "1.1" },
		{ GTEXT_YAML_SCHEMA_JSON,     false, "JSON" },
		{ GTEXT_YAML_SCHEMA_FAILSAFE, false, "failsafe" },
	};

	for (const Dialect &d : dialects) {
		for (const char *text : texts) {
			GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
			GTEXT_YAML_Node *root = gtext_yaml_node_new_scalar_typed(
				doc, text, strlen(text), GTEXT_YAML_STRING, nullptr, nullptr);
			ASSERT_NE(root, nullptr) << text;
			gtext_yaml_document_set_root(doc, root);

			GTEXT_YAML_Sink sink;
			ASSERT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
			GTEXT_YAML_Write_Options wopts = gtext_yaml_write_options_default();
			wopts.schema = d.schema;
			wopts.yaml_1_1 = d.v11;
			ASSERT_EQ(gtext_yaml_write_document(doc, &sink, &wopts),
				GTEXT_YAML_OK) << d.name << " " << text;
			const std::string out(gtext_yaml_sink_buffer_data(&sink),
				gtext_yaml_sink_buffer_size(&sink));
			gtext_yaml_sink_buffer_free(&sink);

			GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
			popts.schema = d.schema;
			popts.yaml_1_1 = d.v11;
			GTEXT_YAML_Document *back =
				gtext_yaml_parse(out.data(), out.size(), &popts, nullptr);
			ASSERT_NE(back, nullptr) << d.name << ": wrote <<" << out << ">>";
			const GTEXT_YAML_Node *node = gtext_yaml_document_root(back);
			EXPECT_EQ(gtext_yaml_node_type(node), GTEXT_YAML_STRING)
				<< d.name << ": the string <<" << text << ">> was written <<"
				<< out << ">> and came back something else";
			EXPECT_STREQ(gtext_yaml_node_as_string(node), text)
				<< d.name << ": wrote <<" << out << ">>";
			gtext_yaml_free(back);
			gtext_yaml_free(doc);
		}
	}
}

/* ...and a null has to be spelled the way its reader spells one.
 *
 * "~" is a null in the core schema and in 1.1, and in neither of the other
 * two. The writer wrote it regardless, so a null went out as "[~]" and came
 * back under the JSON schema as the *string* "~" - which is the case this
 * library's own format page recorded as the open question.
 *
 * The failsafe schema is the exception, and not a fixable one: it resolves
 * nothing, so no null can survive a round trip through it whatever is
 * written. That is what asking for the failsafe schema means. */
TEST(YamlWriterContract, ANullIsSpelledTheWayItsSchemaSpellsOne) {
	struct Case { GTEXT_YAML_Schema schema; bool v11; bool survives; const char *name; };
	const Case cases[] = {
		{ GTEXT_YAML_SCHEMA_CORE,     false, true,  "1.2 core" },
		{ GTEXT_YAML_SCHEMA_CORE,     true,  true,  "1.1" },
		{ GTEXT_YAML_SCHEMA_JSON,     false, true,  "JSON" },
		{ GTEXT_YAML_SCHEMA_FAILSAFE, false, false, "failsafe" },
	};

	/* Both positions: a sequence entry, where 7.2 has no empty alternative
	   and the spelling has to be written out, and a mapping value, where the
	   empty node is a spelling of its own. */
	for (const Case &c : cases) {
		for (int in_sequence = 0; in_sequence < 2; ++in_sequence) {
			/* And both ways the text can arrive: an empty scalar, and one
			   carrying a spelling of its own. */
			for (const char *text : { "", "~", "null", "NULL" }) {
				GTEXT_YAML_Document *doc =
					gtext_yaml_document_new(nullptr, nullptr);
				GTEXT_YAML_Node *null_node = gtext_yaml_node_new_scalar_typed(
					doc, text, strlen(text), GTEXT_YAML_NULL, nullptr, nullptr);
				ASSERT_NE(null_node, nullptr) << text;
				GTEXT_YAML_Node *root = nullptr;
				if (in_sequence) {
					root = gtext_yaml_node_new_sequence(doc, nullptr, nullptr);
					root = gtext_yaml_sequence_append(doc, root, null_node);
				}
				else {
					root = gtext_yaml_node_new_mapping(doc, nullptr, nullptr);
					GTEXT_YAML_Node *k =
						gtext_yaml_node_new_scalar(doc, "k", nullptr, nullptr);
					root = gtext_yaml_mapping_set(doc, root, k, null_node);
				}
				ASSERT_NE(root, nullptr);
				gtext_yaml_document_set_root(doc, root);

				GTEXT_YAML_Sink sink;
				ASSERT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
				GTEXT_YAML_Write_Options wopts =
					gtext_yaml_write_options_default();
				wopts.schema = c.schema;
				wopts.yaml_1_1 = c.v11;
				ASSERT_EQ(gtext_yaml_write_document(doc, &sink, &wopts),
					GTEXT_YAML_OK) << c.name;
				const std::string out(gtext_yaml_sink_buffer_data(&sink),
					gtext_yaml_sink_buffer_size(&sink));
				gtext_yaml_sink_buffer_free(&sink);

				GTEXT_YAML_Parse_Options popts =
					gtext_yaml_parse_options_default();
				popts.schema = c.schema;
				popts.yaml_1_1 = c.v11;
				GTEXT_YAML_Document *back =
					gtext_yaml_parse(out.data(), out.size(), &popts, nullptr);
				ASSERT_NE(back, nullptr)
					<< c.name << ": wrote <<" << out << ">>";
				const GTEXT_YAML_Node *r = gtext_yaml_document_root(back);
				const GTEXT_YAML_Node *got = in_sequence
					? gtext_yaml_sequence_get(r, 0)
					: gtext_yaml_mapping_get(r, "k");
				ASSERT_NE(got, nullptr)
					<< c.name << ": wrote <<" << out << ">>";
				EXPECT_EQ(gtext_yaml_node_type(got) == GTEXT_YAML_NULL,
					c.survives)
					<< c.name << (in_sequence ? " sequence" : " mapping")
					<< " <<" << text << ">> wrote <<" << out << ">>";
				gtext_yaml_free(back);
				gtext_yaml_free(doc);
			}
		}
	}
}

/* The streaming writer's comment event has to end up being a comment.
 *
 * "#" starts one only at the start of a line or after white space (7.1).
 * Anywhere else it is an ordinary character of whatever plain scalar it is
 * written against - and the writer emitted the indent for a fresh line, which
 * is nothing at all at the root, straight after the scalar it had just
 * written. So a sequence of "x" and "y" with the comment "mid" between them
 * came out as "- x# mid", and read back as the one scalar "x# mid".
 *
 * That is worse than losing the comment: the *value* changed, and the
 * document parsed cleanly afterwards, so nothing anywhere said so. The event
 * carries a flag for which kind of comment a caller meant, and the writer now
 * reads both it and where it actually is. */
TEST(YamlWriterContract, AStreamedCommentDoesNotBecomePartOfAValue) {
	struct Case { GTEXT_YAML_Flow_Style style; bool inlined; };
	const Case cases[] = {
		{ GTEXT_YAML_FLOW_STYLE_BLOCK, true  },
		{ GTEXT_YAML_FLOW_STYLE_BLOCK, false },
		{ GTEXT_YAML_FLOW_STYLE_FLOW,  true  },
		{ GTEXT_YAML_FLOW_STYLE_FLOW,  false },
	};

	for (const Case &c : cases) {
		GTEXT_YAML_Sink sink;
		ASSERT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
		GTEXT_YAML_Write_Options opts = gtext_yaml_write_options_default();
		opts.flow_style = c.style;
		GTEXT_YAML_Writer *writer = gtext_yaml_writer_new(sink, &opts);
		ASSERT_NE(writer, nullptr);

		auto send = [&](GTEXT_YAML_Event_Type type, const char *text) {
			GTEXT_YAML_Event e;
			memset(&e, 0, sizeof(e));
			e.type = type;
			if (type == GTEXT_YAML_EVENT_SCALAR) {
				e.data.scalar.ptr = text;
				e.data.scalar.len = strlen(text);
			}
			if (type == GTEXT_YAML_EVENT_COMMENT) {
				e.data.comment.ptr = text;
				e.data.comment.len = strlen(text);
				e.data.comment.inline_comment = c.inlined;
			}
			EXPECT_EQ(gtext_yaml_writer_event(writer, &e), GTEXT_YAML_OK);
		};

		send(GTEXT_YAML_EVENT_STREAM_START, nullptr);
		send(GTEXT_YAML_EVENT_DOCUMENT_START, nullptr);
		send(GTEXT_YAML_EVENT_SEQUENCE_START, nullptr);
		send(GTEXT_YAML_EVENT_SCALAR, "x");
		send(GTEXT_YAML_EVENT_COMMENT, "mid");
		send(GTEXT_YAML_EVENT_SCALAR, "y");
		send(GTEXT_YAML_EVENT_SEQUENCE_END, nullptr);
		send(GTEXT_YAML_EVENT_DOCUMENT_END, nullptr);
		send(GTEXT_YAML_EVENT_STREAM_END, nullptr);
		ASSERT_EQ(gtext_yaml_writer_finish(writer), GTEXT_YAML_OK);

		const std::string out(gtext_yaml_sink_buffer_data(&sink),
			gtext_yaml_sink_buffer_size(&sink));
		gtext_yaml_writer_free(writer);
		gtext_yaml_sink_buffer_free(&sink);

		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
		GTEXT_YAML_Document *back =
			gtext_yaml_parse(out.data(), out.size(), &popts, &err);
		ASSERT_NE(back, nullptr) << "wrote <<" << out << ">>: "
			<< (err.message ? err.message : "");
		gtext_yaml_error_free(&err);

		const GTEXT_YAML_Node *root = gtext_yaml_document_root(back);
		ASSERT_NE(root, nullptr) << out;
		ASSERT_EQ(gtext_yaml_sequence_length(root), 2u) << "wrote <<" << out << ">>";
		/* The values, which is what the "#" had been swallowed into. */
		EXPECT_STREQ(gtext_yaml_node_as_string(gtext_yaml_sequence_get(root, 0)),
			"x") << "wrote <<" << out << ">>";
		EXPECT_STREQ(gtext_yaml_node_as_string(gtext_yaml_sequence_get(root, 1)),
			"y") << "wrote <<" << out << ">>";
		/* ...and the comment is still written, as a comment. */
		EXPECT_NE(out.find("# mid"), std::string::npos)
			<< "wrote <<" << out << ">>";
		gtext_yaml_free(back);
	}
}

/* An inline comment may only be written where nothing follows it on the line.
 *
 * A comment is "#" to the end of the line (7.1). A flow collection is not
 * over at the end of a line, so "[x # note, y]" puts the ", y]" inside the
 * comment and the bracket never closes - the writer said OK and reading its
 * own output back gave "Unterminated flow collection". 7.4 lets a flow
 * collection run over a line break, which is how the input that produced such
 * a node was spelled, so the writer ends the line and carries on below it.
 *
 * This is not only reachable through the DOM API. The parser attaches a
 * comment written inside brackets to the entry before it, so every case here
 * is a document this library reads, writes, and could not read back. */
TEST(YamlWriterContract, AnInlineCommentInFlowDoesNotSwallowTheCollection) {
	struct Case { const char *input; size_t items; const char *kept; };
	const Case cases[] = {
		/* On the first entry: the "," is what gets eaten. */
		{ "[ x, # note\n  y ]\n", 2, "note" },
		/* On the last: the "]" is. */
		{ "[ x,\n  y # note\n]\n", 2, "note" },
		/* On every one of them. */
		{ "[ a, # one\n  b, # two\n  c # three\n]\n", 3, "three" },
		/* A nested collection's own comment sits inside the outer brackets,
		   so the context is the position and not the node. */
		{ "[ [a] # inner\n, b]\n", 2, "inner" },
		/* A flow mapping, where the eaten text is a key rather than a value. */
		{ "{ a: 1, # note\n  b: 2 }\n", 2, "note" },
	};

	for (const Case &c : cases) {
		GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
		popts.retain_comments = true;
		GTEXT_YAML_Document *doc =
			gtext_yaml_parse(c.input, strlen(c.input), &popts, nullptr);
		ASSERT_NE(doc, nullptr) << "input <<" << c.input << ">>";

		Written w = write_doc(doc);
		ASSERT_EQ(w.status, GTEXT_YAML_OK) << c.input;

		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Document *back =
			gtext_yaml_parse(w.text.data(), w.text.size(), &popts, &err);
		ASSERT_NE(back, nullptr) << "wrote <<" << w.text << ">>: "
			<< (err.message ? err.message : "");
		gtext_yaml_error_free(&err);

		/* The collection has to come back whole. Counting is what catches
		   this: the old output parsed as a *shorter* collection or not at
		   all, never as a wrong value. */
		const GTEXT_YAML_Node *root = gtext_yaml_document_root(back);
		ASSERT_NE(root, nullptr) << w.text;
		const size_t got =
			gtext_yaml_node_type(root) == GTEXT_YAML_MAPPING
				? gtext_yaml_mapping_size(root)
				: gtext_yaml_sequence_length(root);
		EXPECT_EQ(got, c.items) << "wrote <<" << w.text << ">>";

		/* And the comment is still there, which is what separates this from
		   dropping it. */
		EXPECT_NE(w.text.find(c.kept), std::string::npos)
			<< "wrote <<" << w.text << ">> without " << c.kept;

		gtext_yaml_free(back);
		gtext_yaml_free(doc);
	}
}

/* The same rule, where what follows the comment is a colon.
 *
 * An implicit key shares its line with the ":" that follows it, so a key
 * carrying an inline comment cannot be one: the writer emitted "k # note: v"
 * and the whole entry vanished into the comment. A mapping of one went out
 * and a mapping of *none* came back, with no error to say so - the quietest
 * shape this defect has.
 *
 * 7.4's explicit form puts the key and its colon on separate lines, which is
 * both valid and what the input spelled, so that is what gets written. */
TEST(YamlWriterContract, ACommentedKeyDoesNotSwallowItsOwnEntry) {
	/* Built through the DOM, and parsed from the spelling that produces the
	   same node - "? k # note" is read back with the comment on the key. */
	for (int from_text = 0; from_text < 2; ++from_text) {
		GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
		popts.retain_comments = true;

		GTEXT_YAML_Document *doc = nullptr;
		if (from_text) {
			const char *input = "? k # note\n: v\n";
			doc = gtext_yaml_parse(input, strlen(input), &popts, nullptr);
		}
		else {
			doc = gtext_yaml_document_new(nullptr, nullptr);
			GTEXT_YAML_Node *map =
				gtext_yaml_node_new_mapping(doc, nullptr, nullptr);
			GTEXT_YAML_Node *k =
				gtext_yaml_node_new_scalar(doc, "k", nullptr, nullptr);
			GTEXT_YAML_Node *v =
				gtext_yaml_node_new_scalar(doc, "v", nullptr, nullptr);
			gtext_yaml_node_set_inline_comment(doc, k, "note");
			map = gtext_yaml_mapping_set(doc, map, k, v);
			gtext_yaml_document_set_root(doc, map);
		}
		ASSERT_NE(doc, nullptr) << from_text;

		/* Both styles: block reaches the explicit-key form, flow reaches the
		   line break inside the braces. Neither may lose the entry. */
		for (int block = 0; block < 2; ++block) {
			Written w = write_doc(doc, block != 0);
			ASSERT_EQ(w.status, GTEXT_YAML_OK) << from_text << "/" << block;

			GTEXT_YAML_Error err;
			memset(&err, 0, sizeof(err));
			GTEXT_YAML_Document *back =
				gtext_yaml_parse(w.text.data(), w.text.size(), &popts, &err);
			ASSERT_NE(back, nullptr) << "wrote <<" << w.text << ">>: "
				<< (err.message ? err.message : "");
			gtext_yaml_error_free(&err);

			const GTEXT_YAML_Node *root = gtext_yaml_document_root(back);
			ASSERT_NE(root, nullptr) << w.text;
			EXPECT_EQ(gtext_yaml_node_type(root), GTEXT_YAML_MAPPING) << w.text;
			EXPECT_EQ(gtext_yaml_mapping_size(root), 1u)
				<< "wrote <<" << w.text << ">>";

			const GTEXT_YAML_Node *key = nullptr;
			const GTEXT_YAML_Node *value = nullptr;
			if (gtext_yaml_mapping_get_at(root, 0, &key, &value)) {
				EXPECT_STREQ(gtext_yaml_node_as_string(key), "k") << w.text;
				EXPECT_STREQ(gtext_yaml_node_as_string(value), "v") << w.text;
			}
			EXPECT_NE(w.text.find("note"), std::string::npos) << w.text;

			gtext_yaml_free(back);
		}
		gtext_yaml_free(doc);
	}
}

/* A comment the writer cannot spell is refused, not written anyway.
 *
 * A comment is "#" followed by nb-char* (7.1). The writer emitted whatever
 * string it was handed, and that went wrong twice over. A character 5.1
 * forbids came out raw, so the writer produced a document this library
 * refuses to read. Worse, a line break in an *inline* comment ended the
 * comment and made content of the rest: a mapping of one entry with the
 * inline comment "one\nevil: yes" was written as "k: v # one" over
 * "evil: yes" and read back with two entries. Nothing reported either.
 *
 * A comment has one spelling and no escapes, so there is nothing to fall
 * back to - the same position the writer already takes for an anchor name.
 * A leading comment is the one exception, and only for "\n": it is rendered
 * as one "#" line per break, which is a real spelling of a multi-line
 * comment. */
TEST(YamlWriterContract, AnUnwritableCommentIsRefused) {
	struct Case { const char *comment; bool leading_ok; bool inline_ok; };
	const Case cases[] = {
		{ "fine",             true,  true  },
		{ "",                 true,  true  },
		{ "with\ttab",        true,  true  },
		/* A break has a spelling as a leading comment and none inline. */
		{ "one\ntwo",         true,  false },
		{ "one\nevil: yes",   true,  false },
		/* Nothing splits on a carriage return, so it would reach the stream
		   raw and end the line there. */
		{ "one\rtwo",         false, false },
		/* Not c-printable in any position. */
		{ "a\x18z",           false, false },
		{ "a\x01z",           false, false },
		{ "\x7f",             false, false },
	};

	for (const Case &c : cases) {
		for (int inlined = 0; inlined < 2; ++inlined) {
			GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
			GTEXT_YAML_Node *map = gtext_yaml_node_new_mapping(doc, nullptr, nullptr);
			GTEXT_YAML_Node *k = gtext_yaml_node_new_scalar(doc, "k", nullptr, nullptr);
			GTEXT_YAML_Node *v = gtext_yaml_node_new_scalar(doc, "v", nullptr, nullptr);
			map = gtext_yaml_mapping_set(doc, map, k, v);
			ASSERT_NE(map, nullptr);
			if (inlined) {
				gtext_yaml_node_set_inline_comment(doc, v, c.comment);
			}
			else {
				gtext_yaml_node_set_leading_comment(doc, map, c.comment);
			}
			gtext_yaml_document_set_root(doc, map);

			const bool want_ok = inlined ? c.inline_ok : c.leading_ok;
			Written w = write_doc(doc, true);
			EXPECT_EQ(w.status == GTEXT_YAML_OK, want_ok)
				<< (inlined ? "inline" : "leading") << " comment <<"
				<< c.comment << ">> wrote " << w.text;

			/* Where it did write, the document has to come back the shape it
			   went in - one entry, not two. */
			if (w.status == GTEXT_YAML_OK) {
				GTEXT_YAML_Error err;
				memset(&err, 0, sizeof(err));
				GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
				GTEXT_YAML_Document *back =
					gtext_yaml_parse(w.text.data(), w.text.size(), &popts, &err);
				ASSERT_NE(back, nullptr) << "wrote " << w.text << ": "
					<< (err.message ? err.message : "");
				gtext_yaml_error_free(&err);
				const GTEXT_YAML_Node *r = gtext_yaml_document_root(back);
				ASSERT_NE(r, nullptr);
				EXPECT_EQ(gtext_yaml_mapping_size(r), 1u)
					<< "the comment escaped into the document: " << w.text;
				gtext_yaml_free(back);
			}
			gtext_yaml_free(doc);
		}
	}
}

/* A preferred scalar style may not change what the document says.
 *
 * `GTEXT_YAML_Write_Options::scalar_style` is a preference, and it was being
 * applied to every scalar whatever its type. Only a plain scalar is resolved
 * by its contents (10.3.2), so any other style makes a scalar a *string*: the
 * null went out as `""` and came back the empty string, the integer 42 came
 * back "42", and true came back "true". Four of the five styles did it, to
 * four of the five types, and the writer fuzzer found it within ninety
 * seconds of being allowed to set the option at all.
 *
 * Canonical form is exempt and has to stay that way: it writes `!!int` in
 * front of the value, and an explicit tag carries the type whatever the
 * quoting does. */
TEST(YamlWriterContract, APreferredStyleDoesNotChangeWhatTheScalarIs) {
	struct Case { const char *text; GTEXT_YAML_Node_Type type; };
	const Case cases[] = {
		{ "",     GTEXT_YAML_NULL },
		{ "null", GTEXT_YAML_NULL },
		{ "~",    GTEXT_YAML_NULL },
		{ "true", GTEXT_YAML_BOOL },
		{ "42",   GTEXT_YAML_INT },
		{ "1.5",  GTEXT_YAML_FLOAT },
		{ "abc",  GTEXT_YAML_STRING },
	};
	const GTEXT_YAML_Scalar_Style styles[] = {
		GTEXT_YAML_SCALAR_STYLE_PLAIN,
		GTEXT_YAML_SCALAR_STYLE_SINGLE_QUOTED,
		GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED,
		GTEXT_YAML_SCALAR_STYLE_LITERAL,
		GTEXT_YAML_SCALAR_STYLE_FOLDED,
	};

	for (const Case &c : cases) {
		for (GTEXT_YAML_Scalar_Style style : styles) {
			for (int canonical = 0; canonical < 2; ++canonical) {
				GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
				GTEXT_YAML_Node *root =
					gtext_yaml_node_new_scalar(doc, c.text, nullptr, nullptr);
				ASSERT_NE(root, nullptr) << c.text;
				ASSERT_EQ(gtext_yaml_node_type(root), c.type) << c.text;
				gtext_yaml_document_set_root(doc, root);

				GTEXT_YAML_Sink sink;
				ASSERT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
				GTEXT_YAML_Write_Options opts = gtext_yaml_write_options_default();
				opts.scalar_style = style;
				opts.canonical = canonical != 0;
				ASSERT_EQ(gtext_yaml_write_document(doc, &sink, &opts),
					GTEXT_YAML_OK) << c.text;
				const std::string text(gtext_yaml_sink_buffer_data(&sink),
					gtext_yaml_sink_buffer_size(&sink));
				gtext_yaml_sink_buffer_free(&sink);

				GTEXT_YAML_Error err;
				memset(&err, 0, sizeof(err));
				GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
				GTEXT_YAML_Document *back =
					gtext_yaml_parse(text.data(), text.size(), &popts, &err);
				ASSERT_NE(back, nullptr) << "style " << (int)style
					<< " of <<" << c.text << ">> wrote " << text << ": "
					<< (err.message ? err.message : "");
				gtext_yaml_error_free(&err);
				const GTEXT_YAML_Node *r = gtext_yaml_document_root(back);
				ASSERT_NE(r, nullptr) << "wrote " << text;
				EXPECT_EQ(gtext_yaml_node_type(r), c.type)
					<< "style " << (int)style << (canonical ? " canonical" : "")
					<< " turned <<" << c.text << ">> into " << text;
				gtext_yaml_free(back);
				gtext_yaml_free(doc);
			}
		}
	}
}

/* The preference is still a preference where honouring it costs nothing: a
   string is a string in every style, so the option has to reach it. Without
   this the fix above could be "ignore scalar_style" and still pass. */
TEST(YamlWriterContract, APreferredStyleStillReachesAString) {
	struct Case { GTEXT_YAML_Scalar_Style style; char mark; };
	const Case cases[] = {
		{ GTEXT_YAML_SCALAR_STYLE_SINGLE_QUOTED, '\'' },
		{ GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED, '"' },
	};
	for (const Case &c : cases) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *root = gtext_yaml_node_new_scalar_typed(
			doc, "abc", 3, GTEXT_YAML_STRING, nullptr, nullptr);
		ASSERT_NE(root, nullptr);
		gtext_yaml_document_set_root(doc, root);

		GTEXT_YAML_Sink sink;
		ASSERT_EQ(gtext_yaml_sink_buffer(&sink), GTEXT_YAML_OK);
		GTEXT_YAML_Write_Options opts = gtext_yaml_write_options_default();
		opts.scalar_style = c.style;
		ASSERT_EQ(gtext_yaml_write_document(doc, &sink, &opts), GTEXT_YAML_OK);
		const std::string text(gtext_yaml_sink_buffer_data(&sink),
			gtext_yaml_sink_buffer_size(&sink));
		gtext_yaml_sink_buffer_free(&sink);
		EXPECT_NE(text.find(c.mark), std::string::npos)
			<< "asked for style " << (int)c.style << " and got " << text;
		gtext_yaml_free(doc);
	}
}

/* The tag has to agree with the *declared* type, not only with the text.
 *
 * gtext_yaml_node_new_scalar_typed() takes two claims - a type and a tag -
 * and only the text was ever checked against the tag. The check skipped
 * every string-typed node, on the reasoning that "!!str" takes any text and
 * needs no checking. That is true of "!!str"; the exemption was written for
 * the tag and applied to the *type*. So a node tagged "!!int" and declared a
 * string was built happily, the writer put it out as '!!int "abc"', and this
 * library refused to read its own output. The writer fuzzer found it by
 * building exactly that.
 *
 * The plain constructor refuses the same pairs and always has, which is what
 * makes this an inconsistency rather than a policy: one door checked and the
 * other did not. */
TEST(YamlWriterContract, ATagHasToAgreeWithTheDeclaredTypeToo) {
	struct Case { const char *tag; const char *text; bool ok; };
	const Case cases[] = {
		/* A tag naming a type that is not string, on a node declared one. */
		{ "!!int",   "abc", false }, { "!!int",   "42",  false },
		{ "!!bool",  "abc", false }, { "!!float", "abc", false },
		{ "!!null",  "abc", false }, { "!!null",  "",    false },
		/* The tags that do name a string, and the ones this library does not
		   resolve - a custom tag leaves the type to the caller, which is the
		   point of one. */
		{ "!!str",   "abc", true },  { "!",       "abc", true },
		{ "!custom", "abc", true },  { "!custom", "42",  true },
	};
	for (const Case &c : cases) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *node = gtext_yaml_node_new_scalar_typed(
			doc, c.text, strlen(c.text), GTEXT_YAML_STRING, c.tag, nullptr);
		EXPECT_EQ(node != nullptr, c.ok)
			<< "typed(STRING) with " << c.tag << " <<" << c.text << ">>";

		/* And the two constructors have to give the same answer, which is
		   the property that failed. */
		GTEXT_YAML_Document *other = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *plain =
			gtext_yaml_node_new_scalar(other, c.text, c.tag, nullptr);
		const bool plain_is_string =
			plain && gtext_yaml_node_type(plain) == GTEXT_YAML_STRING;
		EXPECT_EQ(node != nullptr, plain_is_string)
			<< c.tag << " <<" << c.text
			<< ">>: the typed and plain constructors disagree";
		gtext_yaml_free(other);

		/* Whatever it built has to be writable and readable again. */
		if (node) {
			gtext_yaml_document_set_root(doc, node);
			Written w = write_doc(doc);
			ASSERT_EQ(w.status, GTEXT_YAML_OK) << c.tag << " <<" << c.text << ">>";
			GTEXT_YAML_Error err;
			memset(&err, 0, sizeof(err));
			GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
			GTEXT_YAML_Document *back =
				gtext_yaml_parse(w.text.data(), w.text.size(), &opts, &err);
			EXPECT_NE(back, nullptr) << "wrote " << w.text
				<< " which this parser refuses: "
				<< (err.message ? err.message : "");
			gtext_yaml_error_free(&err);
			if (back) gtext_yaml_free(back);
		}
		gtext_yaml_free(doc);
	}
}

/* A tag names the type whose syntax 10.3.2 defines, and the constructor is
   where a caller's claim about it is checked - the parser checks the same
   claim on the way in and refuses '!!int ""'. Without this the writer put
   such a node out as '!!int ""' and this parser refused the writer's own
   output, which is how the fuzzer found it.

   Two of these rows were the parser's answer being wrong rather than the
   constructor's being absent. "!!float 12" is a float of 12: 10.3.2's float
   row makes its fraction optional, so "12" is in it, and implicit resolution
   answers *int* there only because the int row is tried first. And "!!null x"
   is not a null - the null row is "~ | null | Null | NULL | <empty>" and
   nothing else. Both references agree on the first; js-yaml on the second,
   with PyYAML lax because 1.1 is. */
TEST(YamlWriterContract, ATaggedScalarHasToBeWhatItsTagSays) {
	struct Case { const char *tag; const char *text; bool ok; };
	const Case cases[] = {
		{ "!!int", "", false },     { "!!int", "abc", false },
		{ "!!int", "12", true },
		{ "!!bool", "", false },    { "!!bool", "12", false },
		{ "!!bool", "true", true },
		{ "!!float", "", false },   { "!!float", "abc", false },
		{ "!!float", "1.5", true },
		/* An integer spelling is a float spelling too. */
		{ "!!float", "12", true },
		/* A float spelling is not an integer one, and these two in
		   particular have no integer to be converted to at all. */
		{ "!!int", ".INF", false }, { "!!int", "-.inf", false },
		{ "!!int", ".nan", false }, { "!!int", "1.5", false },
		{ "!!float", ".INF", true },{ "!!float", ".nan", true },
		{ "!!null", "", true },     { "!!null", "~", true },
		{ "!!null", "NULL", true }, { "!!null", "x", false },
		/* A string takes any text at all, which is what makes it the
		   failsafe. */
		{ "!!str", "", true },      { "!!str", "abc", true },
		{ "!!str", "12", true },
	};
	for (const Case &c : cases) {
		GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
		GTEXT_YAML_Node *node =
			gtext_yaml_node_new_scalar(doc, c.text, c.tag, nullptr);
		EXPECT_EQ(node != nullptr, c.ok)
			<< c.tag << " <<" << c.text << ">>";

		/* And the parser has to agree, on the node's own spelling. */
		if (node) {
			gtext_yaml_document_set_root(doc, node);
			Written w = write_doc(doc);
			ASSERT_EQ(w.status, GTEXT_YAML_OK) << c.tag << " <<" << c.text << ">>";
			GTEXT_YAML_Error err;
			memset(&err, 0, sizeof(err));
			GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
			GTEXT_YAML_Document *back =
				gtext_yaml_parse(w.text.c_str(), w.text.size(), &opts, &err);
			EXPECT_NE(back, nullptr) << "wrote " << w.text
				<< " which this parser refuses: "
				<< (err.message ? err.message : "");
			gtext_yaml_error_free(&err);
			if (back) gtext_yaml_free(back);
		}
		gtext_yaml_free(doc);
	}

	/* The parser's own answers, which two of the rows above depend on. */
	struct Parse { const char *yaml; bool ok; GTEXT_YAML_Node_Type type; };
	const Parse parses[] = {
		{ "!!float 12", true, GTEXT_YAML_FLOAT },
		{ "!!float 1.5", true, GTEXT_YAML_FLOAT },
		{ "!!null x", false, GTEXT_YAML_NULL },
		{ "!!null", true, GTEXT_YAML_NULL },
		{ "!!int 12", true, GTEXT_YAML_INT },
	};
	for (const Parse &pc : parses) {
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
		GTEXT_YAML_Document *doc =
			gtext_yaml_parse(pc.yaml, strlen(pc.yaml), &opts, &err);
		EXPECT_EQ(doc != nullptr, pc.ok) << pc.yaml << ": "
			<< (err.message ? err.message : "");
		gtext_yaml_error_free(&err);
		if (doc) {
			EXPECT_EQ(gtext_yaml_node_type(gtext_yaml_document_root(doc)),
				pc.type) << pc.yaml;
			gtext_yaml_free(doc);
		}
	}
}

/* A document has exactly one root - l-bare-document is a single
   s-l+block-node (9.2) - and a second one has nowhere to go.

   The writer used to put it straight after the first. "*a: x" is not a
   mapping: 6.9.2 stops an alias name only at a flow indicator, so the name is
   "a:" and the "x" is a second node at document level - which is why the DOM
   parser refuses the document. The streaming parser reports what is written
   and leaves composing to its consumer, so the writer saw an ALIAS and then a
   SCALAR, wrote both, and produced "*a:x" - two nodes run together with no
   separator at all, and a document nothing can read.

   That is the same habit as answering an INDICATOR with OK: inventing
   structure for events that describe none. A second root is refused now. */
TEST(YamlWriterContract, ADocumentTakesOneRootNode) {
	std::string out;
	EXPECT_FALSE(pipe_through("*a: x\n", &out)) << "wrote: " << out;

	/* One root still writes, and so does one per document. */
	EXPECT_TRUE(pipe_through("a\n", &out));
	EXPECT_NE(out.find('a'), std::string::npos) << "wrote: " << out;
	EXPECT_TRUE(pipe_through("--- a\n--- b\n", &out));
	EXPECT_NE(out.find('b'), std::string::npos) << "wrote: " << out;
}

/* A block scalar ends its own last line, and with "+" chomping that break is
   part of the value (8.1.1.2). The break DOCUMENT_START writes before the
   next "---" was written unconditionally, so it landed on top of the one the
   block had already written and the value gained a line feed.

   Clip and strip chomping collapse a trailing break, which is why this only
   ever showed on "+" - and only where a second document follows, since with
   nothing after it there is no "---" to separate from. Both halves are why
   no corpus of single documents could ask. */
TEST(YamlWriterContract, AKeptTrailingBreakIsNotDoubledByTheSeparator) {
	struct Case { const char *in; const char *first; };
	const Case cases[] = {
		{ "|+\n a\n\n---\nx\n", "a\n\n" },
		{ "|+\n\n|\n\n---\nx\n", "\n|\n\n" },
		{ ">+\n a\n\n---\nx\n", "a\n\n" },
		/* Clip and strip were right before and stay right. */
		{ "|\n a\n---\nx\n", "a\n" },
		{ "|-\n a\n\n---\nx\n", "a" },
	};
	for (const Case &c : cases) {
		std::string out;
		ASSERT_TRUE(pipe_through(c.in, &out))
			<< "input: " << ::testing::PrintToString(std::string(c.in));
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
		GTEXT_YAML_Document *back =
			gtext_yaml_parse(out.data(), out.size(), &opts, &err);
		ASSERT_NE(back, nullptr) << "wrote " << out << ": "
			<< (err.message ? err.message : "");
		gtext_yaml_error_free(&err);
		const char *got =
			gtext_yaml_node_as_string(gtext_yaml_document_root(back));
		EXPECT_STREQ(got ? got : "", c.first) << "wrote "
			<< ::testing::PrintToString(out);
		gtext_yaml_free(back);
	}
}

/* %YAML travels the same way, and has to survive the trip rather than being
   quietly dropped. */
TEST(YamlWriterContract, TheStreamingWriterKeepsTheVersionDirective) {
	std::string out;
	ASSERT_TRUE(pipe_through("%YAML 1.2\n---\nv\n", &out));
	EXPECT_NE(out.find("%YAML 1.2"), std::string::npos) << "wrote: " << out;

	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	GTEXT_YAML_Document *back =
		gtext_yaml_parse(out.data(), out.size(), &opts, &err);
	ASSERT_NE(back, nullptr) << "wrote " << out;
	gtext_yaml_error_free(&err);
	gtext_yaml_free(back);
}

/* A directive may only follow a document that has been ended *explicitly*:
   l-yaml-stream reaches a directive document through l-document-suffix, which
   is c-document-end (9.2). The writer wrote only the line break, which leaves
   the "%" standing after content.

   Both ways that goes wrong are bad, and the quieter one is worse. A "%TAG"
   there produces a stream this parser refuses - correctly, "Directive after
   content, with no '...' to close the document". A "%YAML 1.2" after a plain
   scalar *folds into the scalar*: "a" over "%YAML 1.2" comes back as the one
   string "a %YAML 1.2", with nothing reported at all. */
TEST(YamlWriterContract, ADirectiveClosesTheDocumentBeforeIt) {
	std::string out;
	ASSERT_TRUE(pipe_through("a\n...\n%YAML 1.2\n---\nb\n", &out));
	EXPECT_NE(out.find("..."), std::string::npos)
		<< "wrote " << out << " which leaves the directive after content";

	/* And the values survive, which is the half that failed silently. */
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	GTEXT_YAML_Document *back =
		gtext_yaml_parse(out.data(), out.size(), &opts, &err);
	ASSERT_NE(back, nullptr) << "wrote " << out << ": "
		<< (err.message ? err.message : "");
	gtext_yaml_error_free(&err);
	const char *first = gtext_yaml_node_as_string(gtext_yaml_document_root(back));
	EXPECT_STREQ(first ? first : "", "a") << "wrote " << out;
	gtext_yaml_free(back);

	/* A %TAG there is the loud half: the handle it declares has to reach its
	   own document, and the stream has to be readable at all. */
	ASSERT_TRUE(pipe_through(
		"a\n...\n%TAG !e! tag:x,2000:\n---\n!e!f b\n", &out));
	memset(&err, 0, sizeof(err));
	back = gtext_yaml_parse(out.data(), out.size(), &opts, &err);
	EXPECT_NE(back, nullptr) << "wrote " << out << ": "
		<< (err.message ? err.message : "");
	gtext_yaml_error_free(&err);
	if (back) gtext_yaml_free(back);

	/* A directive with no document before it still needs no "...". */
	ASSERT_TRUE(pipe_through("%YAML 1.2\n---\na\n", &out));
	EXPECT_EQ(out.find("..."), std::string::npos) << "wrote " << out;
}

/* The two event APIs look like two ends of a pipe and are not one.
 *
 * gtext_yaml_writer_event() takes *composed* events - MAPPING_START, the
 * pairs, MAPPING_END - which is what gtext_yaml_stream_walk() produces from a
 * parsed document. The streaming *parser* reports structure as it is written
 * instead: the ":" of a block mapping, the "-" of a block sequence and even
 * the "," between two flow entries arrive as GTEXT_YAML_EVENT_INDICATOR, and
 * composing them is left to the consumer - which is what the DOM parser is.
 * A lone scalar is the whole of what crosses unchanged.
 *
 * The writer used to answer an indicator with OK and write nothing, so a
 * caller who joined the two got no error and a document with its block
 * structure gone: "a: 1" over "b: 2" came back as the one scalar "a1b2".
 * There is nothing to render and no way to guess what was meant, so it is
 * refused - loudly, at the first event that cannot be written, rather than
 * quietly at every one of them. */
TEST(YamlWriterContract, AnIndicatorEventIsRefusedRatherThanIgnored) {
	std::string out;
	EXPECT_FALSE(pipe_through("a: 1\nb: 2\n", &out))
		<< "the block mapping was accepted and written as: " << out;
	EXPECT_FALSE(pipe_through("- a\n- b\n", &out))
		<< "the block sequence was accepted and written as: " << out;

	/* A flow collection does open with SEQUENCE_START, but its separators
	   are indicators too, so it gets no further. */
	EXPECT_FALSE(pipe_through("[a, b]\n", &out))
		<< "the flow sequence was accepted and written as: " << out;

	/* A lone scalar carries no indicator, which is the whole of what does go
	   through - and why the directive tests above are written with one. */
	ASSERT_TRUE(pipe_through("v\n", &out));
	EXPECT_NE(out.find("v"), std::string::npos) << "wrote: " << out;
}

/* 5.1 again, from the other side.
 *
 * The gate that enforces c-printable runs over the decoded character stream
 * as bytes arrive, and a sequence a feed cut in half is held rather than
 * judged from its first byte - which is right while more input may come. At
 * the end of the stream it was still being held, and skipped, on the grounds
 * that gtext_utf8_validate() would catch it when the scalar was assembled.
 * Bytes on a directive or a comment line never become a scalar, so nothing
 * ever did: "%" followed by a lone 0xC2 was a document, and the same bytes
 * with a line break after them were not.
 *
 * Found by feeding the parser's own events to the writer: the writer emitted
 * the directive back, and the parser then refused what it had just accepted.
 *
 * spec-1.2.2.corpus cannot hold this case - it spells "\xNN" as a code point,
 * which is the whole difference here - so it lives with the writer. */
TEST(YamlWriterContract, ATruncatedSequenceIsNotACharacter) {
	struct Case { const char *label; const char *bytes; size_t len; };
	const Case cases[] = {
		{ "a directive line",            "%\xc2",      2 },
		{ "a directive line, terminated", "%\xc2\n",   3 },
		{ "a named directive",           "%YAML\xc2",  6 },
		{ "a comment line",              "#\xc2",      2 },
		{ "a comment line, terminated",  "#\xc2\n",    3 },
		{ "a plain scalar",              "a\xc2",      2 },
		{ "a stray continuation byte",   "%\x80",      2 },
		{ "an invalid lead byte",        "%\xff",      2 },
	};
	for (const Case &c : cases) {
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
		GTEXT_YAML_Document *doc =
			gtext_yaml_parse(c.bytes, c.len, &opts, &err);
		EXPECT_EQ(doc, nullptr) << c.label << " was accepted";
		if (doc) gtext_yaml_free(doc);
		gtext_yaml_error_free(&err);
	}
}

/* The style a node remembers is a *preference*, and the writer has to refuse
   one the text cannot be spelled in.  EveryCodePointSurvivesOrIsRefused above
   asks this question with the default options, which are flow - and a flow
   collection has no block scalar in it, so every awkward character there ends
   up double-quoted and the block styles were never asked about any of them.

   U+FEFF is the character that separates the two: nb-char is "c-printable -
   b-char - c-byte-order-mark" (5.4), so it has no plain *or block* spelling,
   and scalar_needs_quotes() had said so for the plain style since U+0085 and
   U+2028 were added beside it.  A folded scalar carrying one went out as
   itself and this library's own parser answered "Byte order mark in scalar
   content".  fuzz_yaml_writer found it; the reproducer is
   corpus/yaml-writer/bom-in-a-block-scalar.seed.

   U+0085, U+2028 and U+2029 are here as the controls: each is an ordinary
   nb-char in 1.2, so each *does* have a block spelling, and a fix that
   refused the whole neighbourhood would pass this test only by accident. */
TEST(YamlWriterContract, AStoredBlockStyleIsRefusedForTextThatHasNoBlockSpelling) {
	struct Case {
		const char *label;
		uint32_t cp;
	} const cases[] = {
		{"U+FEFF byte order mark", 0xFEFF},
		{"U+0085 next line", 0x0085},
		{"U+2028 line separator", 0x2028},
		{"U+2029 paragraph separator", 0x2029},
	};
	const GTEXT_YAML_Scalar_Style styles[] = {
		GTEXT_YAML_SCALAR_STYLE_PLAIN,
		GTEXT_YAML_SCALAR_STYLE_LITERAL,
		GTEXT_YAML_SCALAR_STYLE_FOLDED,
		GTEXT_YAML_SCALAR_STYLE_SINGLE_QUOTED,
		GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED,
	};
	for (const Case &c : cases) {
		const std::string value = "x" + utf8(c.cp) + "y";
		for (GTEXT_YAML_Scalar_Style style : styles) {
			GTEXT_YAML_Document *doc = gtext_yaml_document_new(nullptr, nullptr);
			ASSERT_NE(doc, nullptr);
			GTEXT_YAML_Node *root = gtext_yaml_node_new_scalar_n(
				doc, value.data(), value.size(), nullptr, nullptr);
			ASSERT_NE(root, nullptr);
			EXPECT_TRUE(gtext_yaml_node_set_scalar_style(root, style));
			gtext_yaml_document_set_root(doc, root);

			Written w = write_doc(doc, /*block=*/true);
			gtext_yaml_free(doc);
			ASSERT_EQ(w.status, GTEXT_YAML_OK) << c.label << " style " << style;

			std::string back;
			ASSERT_TRUE(read_back_scalar(w.text, &back))
				<< c.label << " style " << style << " wrote [" << w.text
				<< "] which this parser refuses";
			EXPECT_EQ(back, value) << c.label << " style " << style;
		}
	}
}
