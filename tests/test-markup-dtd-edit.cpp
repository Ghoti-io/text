/**
 * @file
 * Checked and atomic internal DTD edit API tests.
 */

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/markup.h>
#include <ghoti.io/text/xml/xml_parse.h>

#include "../src/markup/markup_internal.h"
#include "markup_test_support.h"

namespace {

struct Fixture {
  FaultAllocator allocator;
  GTEXT_Markup_Document *doc = nullptr;
  GTEXT_Markup_Node *doctype = nullptr;

  Fixture() {
    GTEXT_Markup_Options options = gtext_markup_options_default();
    options.allocator = &allocator.alloc;
    EXPECT_EQ(gtext_markup_document_new(&options, &doc), GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_create_doctype(doc, "root", nullptr, nullptr,
        &doctype), GTEXT_MARKUP_OK);
  }
  ~Fixture() {
    gtext_markup_document_free(doc);
    EXPECT_TRUE(allocator.live.empty());
    EXPECT_EQ(allocator.bad_frees, 0u);
  }
};

std::string syntax_at(GTEXT_Markup_Node *doctype, size_t index) {
  GTEXT_Markup_DTD_Syntax syntax;
  if (!gtext_markup_doctype_syntax(doctype, index, &syntax)) return {};
  return std::string(syntax.source, syntax.source_length);
}

TEST(MarkupDtdEdit, ChecksTypedDeclarationsAndPreservesTheirOrder) {
  Fixture f;
  EXPECT_EQ(gtext_markup_doctype_insert_syntax(f.doctype, 0,
      GTEXT_MARKUP_DTD_ELEMENT, "<!ELEMENT root ANY>", 19, nullptr),
      GTEXT_MARKUP_OK);
  const char entity[] = "<!ENTITY label 'name'>";
  EXPECT_EQ(gtext_markup_doctype_insert_syntax(f.doctype, 1,
      GTEXT_MARKUP_DTD_ENTITY, entity, sizeof(entity) - 1, nullptr),
      GTEXT_MARKUP_OK);
  const char attlist[] = "<!ATTLIST root title CDATA #IMPLIED>";
  EXPECT_EQ(gtext_markup_doctype_insert_syntax(f.doctype, 1,
      GTEXT_MARKUP_DTD_ATTLIST, attlist, sizeof(attlist) - 1, nullptr),
      GTEXT_MARKUP_OK);

  ASSERT_EQ(gtext_markup_doctype_syntax_count(f.doctype), 3u);
  GTEXT_Markup_DTD_Syntax first;
  ASSERT_TRUE(gtext_markup_doctype_syntax(f.doctype, 0, &first));
  EXPECT_EQ(first.kind, GTEXT_MARKUP_DTD_ELEMENT);
  EXPECT_EQ(syntax_at(f.doctype, 1), attlist);
  EXPECT_EQ(syntax_at(f.doctype, 2), entity);

  EXPECT_EQ(gtext_markup_doctype_move_syntax(f.doctype, 2, 0, nullptr),
      GTEXT_MARKUP_OK);
  EXPECT_EQ(syntax_at(f.doctype, 0), entity);
  EXPECT_EQ(gtext_markup_doctype_remove_syntax(f.doctype, 1, nullptr),
      GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_doctype_replace_syntax(f.doctype, 1,
      GTEXT_MARKUP_DTD_COMMENT, "<!-- retained -->", 17, nullptr),
      GTEXT_MARKUP_OK);
  EXPECT_EQ(syntax_at(f.doctype, 1), "<!-- retained -->");
}

TEST(MarkupDtdEdit, AcceptsEveryRetainedInternalSyntaxKind) {
  Fixture f;
  const struct {
    GTEXT_Markup_DTD_Syntax_Kind kind;
    const char * source;
  } items[] = {
      {GTEXT_MARKUP_DTD_ELEMENT, "<!ELEMENT root ANY>"},
      {GTEXT_MARKUP_DTD_ATTLIST, "<!ATTLIST root title CDATA #IMPLIED>"},
      {GTEXT_MARKUP_DTD_ENTITY, "<!ENTITY label 'name'>"},
      {GTEXT_MARKUP_DTD_NOTATION, "<!NOTATION image SYSTEM 'image/type'>"},
      {GTEXT_MARKUP_DTD_COMMENT, "<!-- retained -->"},
      {GTEXT_MARKUP_DTD_PROCESSING_INSTRUCTION, "<?dtd-note retained?>"},
      {GTEXT_MARKUP_DTD_ENTITY,
          "<!ENTITY % declarations '<!ELEMENT child EMPTY>'>"},
      {GTEXT_MARKUP_DTD_PARAMETER_REFERENCE, "%declarations;"},
  };
  for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
    EXPECT_EQ(gtext_markup_doctype_insert_syntax(f.doctype, i, items[i].kind,
        items[i].source, std::strlen(items[i].source), nullptr),
        GTEXT_MARKUP_OK) << items[i].source;
  }
  ASSERT_EQ(gtext_markup_doctype_syntax_count(f.doctype),
      sizeof(items) / sizeof(items[0]));
  for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
    GTEXT_Markup_DTD_Syntax syntax;
    ASSERT_TRUE(gtext_markup_doctype_syntax(f.doctype, i, &syntax));
    EXPECT_EQ(syntax.kind, items[i].kind);
    EXPECT_EQ(std::string(syntax.source, syntax.source_length), items[i].source);
  }
}

TEST(MarkupDtdEdit, InvalidGrammarAndWorkCeilingLeaveTheModelUnchanged) {
  Fixture f;
  const char declaration[] = "<!ENTITY label 'before'>";
  ASSERT_EQ(gtext_markup_doctype_insert_syntax(f.doctype, 0,
      GTEXT_MARKUP_DTD_ENTITY, declaration, sizeof(declaration) - 1, nullptr),
      GTEXT_MARKUP_OK);
  const std::string before = syntax_at(f.doctype, 0);

  const char malformed[] = "<!ELEMENT root (a|)>";
  EXPECT_EQ(gtext_markup_doctype_replace_syntax(f.doctype, 0,
      GTEXT_MARKUP_DTD_ELEMENT, malformed, sizeof(malformed) - 1, nullptr),
      GTEXT_MARKUP_E_DTD_SYNTAX);
  EXPECT_EQ(gtext_markup_doctype_syntax_count(f.doctype), 1u);
  EXPECT_EQ(syntax_at(f.doctype, 0), before);

  GTEXT_Markup_DTD_Edit_Options limits = {1};
  EXPECT_EQ(gtext_markup_doctype_remove_syntax(f.doctype, 0, &limits),
      GTEXT_MARKUP_E_LIMIT_WORK);
  EXPECT_EQ(syntax_at(f.doctype, 0), before);
}

TEST(MarkupDtdEdit, AllocationFailureDoesNotPublishAPartialReplacement) {
  const char declaration[] = "<!ENTITY label 'before'>";
  const char replacement[] = "<!ENTITY label 'after'>";
  bool reached_success = false;
  for (size_t fail_at = 1; fail_at < 128 && !reached_success; ++fail_at) {
    Fixture f;
    ASSERT_EQ(gtext_markup_doctype_insert_syntax(f.doctype, 0,
        GTEXT_MARKUP_DTD_ENTITY, declaration, sizeof(declaration) - 1, nullptr),
        GTEXT_MARKUP_OK);
    const std::string before = syntax_at(f.doctype, 0);
    f.allocator.arm(fail_at);
    GTEXT_Markup_Status status = gtext_markup_doctype_replace_syntax(
        f.doctype, 0, GTEXT_MARKUP_DTD_ENTITY, replacement,
        sizeof(replacement) - 1, nullptr);
    if (f.allocator.failed) {
      EXPECT_EQ(status, GTEXT_MARKUP_E_OOM) << "allocation " << fail_at;
      EXPECT_EQ(gtext_markup_doctype_syntax_count(f.doctype), 1u);
      EXPECT_EQ(syntax_at(f.doctype, 0), before);
    }
    else {
      EXPECT_EQ(status, GTEXT_MARKUP_OK);
      EXPECT_EQ(syntax_at(f.doctype, 0), replacement);
      reached_success = true;
    }
    f.allocator.disarm();
  }
  EXPECT_TRUE(reached_success);
}

TEST(MarkupDtdEdit, SuccessfulEditsInvalidateDerivedBindings) {
  const char document[] =
      "<!DOCTYPE root [<!ENTITY label 'value'>]><root/>";
  GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_Markup_Document *parsed = nullptr;
  GTEXT_XML_Error error;
  ASSERT_EQ(gtext_xml_parse(document, sizeof(document) - 1, &options, &parsed,
      &error), GTEXT_XML_OK) << error.message;
  GTEXT_Markup_Node *doctype = gtext_markup_document_doctype(parsed);
  ASSERT_NE(doctype, nullptr);
  ASSERT_GT(reinterpret_cast<markup_doctype *>(doctype)->binding_count, 0u);

  const char comment[] = "<!-- added -->";
  EXPECT_EQ(gtext_markup_doctype_insert_syntax(doctype, 1,
      GTEXT_MARKUP_DTD_COMMENT, comment, sizeof(comment) - 1, nullptr),
      GTEXT_MARKUP_OK);
  EXPECT_EQ(reinterpret_cast<markup_doctype *>(doctype)->binding_count, 0u);
  gtext_markup_document_free(parsed);
}

} // namespace
