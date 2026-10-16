/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <unistd.h>
#include <gtest/gtest.h>
#include <ghoti.io/text/xml.h>
#include <ghoti.io/text/xml/xml_parse.h>
#include <ghoti.io/text/xml/xml_stream.h>

namespace {
struct ExternalDtd {
  std::string bytes;
  size_t offset = 0;
  size_t opens = 0;
  size_t closes = 0;
};
GTEXT_XML_Source_Read_Result read_external(void *opaque, unsigned char *buffer,
    size_t capacity, size_t *count) {
  auto *source = static_cast<ExternalDtd *>(opaque);
  *count = std::min(capacity, source->bytes.size() - source->offset);
  if (*count == 0) return GTEXT_XML_SOURCE_EOF;
  std::memcpy(buffer, source->bytes.data() + source->offset, *count);
  source->offset += *count;
  return GTEXT_XML_SOURCE_DATA;
}
void close_external(void *opaque) {
  ++static_cast<ExternalDtd *>(opaque)->closes;
}
GTEXT_XML_Resolver_Result open_external(
    const GTEXT_XML_Resolve_Request *, void *opaque,
    GTEXT_XML_External_Source *source) {
  auto *dtd = static_cast<ExternalDtd *>(opaque);
  ++dtd->opens;
  source->handle = dtd;
  source->identity = dtd;
  source->base_uri = "memory:/validation.dtd";
  source->read = read_external;
  source->close = close_external;
  return GTEXT_XML_RESOLVER_OPENED;
}
struct Parsed {
  GTEXT_Markup_Document *doc = nullptr;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  GTEXT_XML_Error error{};
  Parsed() = default;
  Parsed(const Parsed &) = delete;
  Parsed &operator=(const Parsed &) = delete;
  Parsed(Parsed &&other) noexcept
      : doc(other.doc), status(other.status), error(other.error) {
    other.doc = nullptr;
  }
  Parsed &operator=(Parsed &&other) noexcept {
    if (this != &other) {
      gtext_markup_document_free(doc);
      doc = other.doc; other.doc = nullptr;
      status = other.status; error = other.error;
    }
    return *this;
  }
  ~Parsed() { gtext_markup_document_free(doc); }
};
Parsed parse(const std::string &input, GTEXT_XML_Parse_Options options =
    gtext_xml_parse_options_default()) {
  Parsed out;
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  out.status = gtext_xml_parse(input.data(), input.size(), &options,
      &out.doc, &out.error);
  return out;
}
size_t count(const Parsed &p, GTEXT_XML_Validation_Code code) {
  size_t result = 0;
  for (size_t i = 0; i < gtext_xml_validation_diagnostic_count(p.doc); ++i) {
    GTEXT_XML_Validation_Diagnostic d{};
    if (gtext_xml_validation_diagnostic(p.doc, i, &d) && d.code == code) ++result;
  }
  return result;
}
size_t count(GTEXT_Markup_Document *doc, GTEXT_XML_Validation_Code code) {
  size_t result = 0;
  for (size_t i = 0; i < gtext_xml_validation_diagnostic_count(doc); ++i) {
    GTEXT_XML_Validation_Diagnostic d{};
    if (gtext_xml_validation_diagnostic(doc, i, &d) && d.code == code) ++result;
  }
  return result;
}
struct Events { size_t diagnostics = 0; size_t content = 0; size_t undeclared = 0; size_t attribute = 0; size_t idref = 0; size_t truncated = 0; };
GTEXT_XML_Status receive(const GTEXT_XML_Event *event, void *opaque) {
  auto *events = static_cast<Events *>(opaque);
  if (event->type == GTEXT_XML_EVENT_VALIDATION_DIAGNOSTIC) {
    ++events->diagnostics;
    if (event->validation_diagnostic.code == GTEXT_XML_VALIDATION_ELEMENT_CONTENT)
      ++events->content;
    if (event->validation_diagnostic.code == GTEXT_XML_VALIDATION_UNDECLARED_ELEMENT)
      ++events->undeclared;
    if (event->validation_diagnostic.code == GTEXT_XML_VALIDATION_ATTRIBUTE_VALUE)
      ++events->attribute;
    if (event->validation_diagnostic.code == GTEXT_XML_VALIDATION_UNRESOLVED_IDREF)
      ++events->idref;
  }
  if (event->type == GTEXT_XML_EVENT_VALIDATION_DIAGNOSTICS_TRUNCATED)
    ++events->truncated;
  return GTEXT_XML_OK;
}

TEST(XmlValidation, AcceptsElementOnlyAndSingleParticleSequence) {
  const auto p = parse("<!DOCTYPE root [<!ELEMENT root (item)><!ELEMENT item EMPTY>]><root><item/></root>");
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  EXPECT_EQ(gtext_xml_validation_diagnostic_count(p.doc), 0u);
}

TEST(XmlValidation, ChecksRequiredAndEnumeratedAttributes) {
  const auto p = parse("<!DOCTYPE root [<!ELEMENT root EMPTY>"
      "<!ATTLIST root mode (fast|safe) #REQUIRED>]><root mode='unknown'/>");
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  EXPECT_EQ(count(p, GTEXT_XML_VALIDATION_ATTRIBUTE_VALUE), 1u);
  const auto missing = parse("<!DOCTYPE root [<!ELEMENT root EMPTY>"
      "<!ATTLIST root mode (fast|safe) #REQUIRED>]><root/>");
  ASSERT_EQ(missing.status, GTEXT_XML_OK) << missing.error.message;
  EXPECT_EQ(count(missing, GTEXT_XML_VALIDATION_ATTRIBUTE_VALUE), 1u);
}

TEST(XmlValidation, ChecksFixedAndIdentityAttributesAcrossDocument) {
  const auto p = parse("<!DOCTYPE root [<!ELEMENT root (item*)>"
      "<!ELEMENT item EMPTY><!ATTLIST item id ID #IMPLIED ref IDREF #IMPLIED"
      " mode CDATA #FIXED 'safe'>]><root><item id='same' mode='unsafe'/>"
      "<item id='same' ref='missing'/></root>");
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  EXPECT_EQ(count(p, GTEXT_XML_VALIDATION_ATTRIBUTE_VALUE), 1u);
  EXPECT_EQ(count(p, GTEXT_XML_VALIDATION_DUPLICATE_ID), 1u);
  EXPECT_EQ(count(p, GTEXT_XML_VALIDATION_UNRESOLVED_IDREF), 1u);
}

TEST(XmlValidation, ResolvesIdrefsAtDocumentEndForPushReaders) {
  const std::string input = "<!DOCTYPE root [<!ELEMENT root EMPTY>"
      "<!ATTLIST root ref IDREF #IMPLIED>]><root ref='absent'/>";
  const auto dom = parse(input);
  ASSERT_EQ(dom.status, GTEXT_XML_OK) << dom.error.message;
  ASSERT_EQ(count(dom, GTEXT_XML_VALIDATION_UNRESOLVED_IDREF), 1u);
  Events events;
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  auto *stream = gtext_xml_stream_new(&options, receive, &events);
  ASSERT_NE(stream, nullptr);
  GTEXT_XML_Error error{};
  GTEXT_XML_Status status = GTEXT_XML_OK;
  for (char c : input) {
    status = gtext_xml_stream_feed(stream, &c, 1, &error);
    if (status != GTEXT_XML_OK) break;
  }
  if (status == GTEXT_XML_OK) status = gtext_xml_stream_finish(stream, &error);
  EXPECT_EQ(status, GTEXT_XML_OK) << error.message;
  EXPECT_EQ(events.idref, 1u);
  gtext_xml_stream_free(stream);
}

TEST(XmlValidation, ReportsExternalNormalizationInStandaloneDocument) {
  ExternalDtd dtd;
  dtd.bytes = "<!ELEMENT root EMPTY><!ATTLIST root words NMTOKENS #IMPLIED>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = open_external;
  options.resolver_user_data = &dtd;
  const std::string input = "<?xml version='1.0' standalone='yes'?>"
      "<!DOCTYPE root SYSTEM 'urn:validation'><root words=' a  b '/>";
  Parsed result;
  result.status = gtext_xml_parse(input.data(), input.size(), &options,
      &result.doc, &result.error);
  ASSERT_EQ(result.status, GTEXT_XML_OK) << result.error.message;
  EXPECT_GE(count(result, GTEXT_XML_VALIDATION_STANDALONE), 1u);
}

TEST(XmlValidation, ChecksEntityAndNotationAttributeReferences) {
  const std::string prefix = "<!DOCTYPE root [<!ELEMENT root EMPTY>"
      "<!NOTATION gif SYSTEM 'image/gif'>"
      "<!ENTITY logo SYSTEM 'logo.gif' NDATA gif>"
      "<!ATTLIST root image ENTITY #IMPLIED format NOTATION (gif) #IMPLIED>]><root ";
  const auto valid = parse(prefix + "image='logo' format='gif'/>");
  ASSERT_EQ(valid.status, GTEXT_XML_OK) << valid.error.message;
  EXPECT_EQ(count(valid, GTEXT_XML_VALIDATION_DTD_CROSS_REFERENCE), 0u);
  const auto invalid = parse(prefix + "image='missing' format='unknown'/>");
  ASSERT_EQ(invalid.status, GTEXT_XML_OK) << invalid.error.message;
  EXPECT_EQ(count(invalid, GTEXT_XML_VALIDATION_ATTRIBUTE_VALUE), 2u);
}

TEST(XmlValidation, ChecksUnparsedEntityNotationCrossReference) {
  const std::string input = "<!DOCTYPE root [<!ELEMENT root EMPTY>"
      "<!ENTITY image SYSTEM 'image.bin' NDATA missing>]><root/>";
  const auto p = parse(input);
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  ASSERT_EQ(count(p, GTEXT_XML_VALIDATION_DTD_CROSS_REFERENCE), 1u);
  for (size_t i = 0; i < gtext_xml_validation_diagnostic_count(p.doc); ++i) {
    GTEXT_XML_Validation_Diagnostic diagnostic{};
    ASSERT_TRUE(gtext_xml_validation_diagnostic(p.doc, i, &diagnostic));
    if (diagnostic.code == GTEXT_XML_VALIDATION_DTD_CROSS_REFERENCE) {
      EXPECT_EQ(diagnostic.location.offset,
          input.find("missing"));
    }
  }
}

TEST(XmlValidation, ResolvesForwardIdrefToLaterElement) {
  const auto p = parse("<!DOCTYPE root [<!ELEMENT root (item,item)>"
      "<!ELEMENT item EMPTY><!ATTLIST item id ID #IMPLIED ref IDREF #IMPLIED>]><root>"
      "<item ref='later'/><item id='later'/></root>");
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  EXPECT_EQ(count(p, GTEXT_XML_VALIDATION_UNRESOLVED_IDREF), 0u);
}

TEST(XmlValidation, ReportsEntityFromExternalDeclarationInStandaloneContent) {
  ExternalDtd dtd;
  dtd.bytes = "<!ELEMENT root (#PCDATA)><!ENTITY ext 'replacement'>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = open_external;
  options.resolver_user_data = &dtd;
  const std::string input = "<?xml version='1.0' standalone='yes'?>"
      "<!DOCTYPE root SYSTEM 'urn:validation'><root>&ext;</root>";
  Parsed result;
  result.status = gtext_xml_parse(input.data(), input.size(), &options,
      &result.doc, &result.error);
  ASSERT_EQ(result.status, GTEXT_XML_OK) << result.error.message;
  EXPECT_EQ(count(result, GTEXT_XML_VALIDATION_STANDALONE), 1u);
}

TEST(XmlValidation, ChecksRootUndeclaredAndContent) {
  const auto p = parse("<!DOCTYPE expected [<!ELEMENT expected (known)><!ELEMENT known EMPTY>]><expected><unknown>bad</unknown></expected>");
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  EXPECT_EQ(count(p, GTEXT_XML_VALIDATION_ROOT_TYPE), 0u);
  EXPECT_EQ(count(p, GTEXT_XML_VALIDATION_UNDECLARED_ELEMENT), 1u);
  EXPECT_GE(count(p, GTEXT_XML_VALIDATION_ELEMENT_CONTENT), 2u);
}

TEST(XmlValidation, ReportsRootTypeMismatch) {
  const std::string input = "<!DOCTYPE expected [<!ELEMENT expected EMPTY>]><actual/>";
  const auto p = parse(input);
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  EXPECT_EQ(count(p, GTEXT_XML_VALIDATION_ROOT_TYPE), 1u);
  GTEXT_XML_Validation_Diagnostic diagnostic{};
  ASSERT_TRUE(gtext_xml_validation_diagnostic(p.doc, 0, &diagnostic));
  EXPECT_EQ(diagnostic.location.offset, input.find("<actual"));
}

TEST(XmlValidation, LocatesMissingRequiredChildAtEndTag) {
  const std::string input = "<!DOCTYPE root [<!ELEMENT root (a,b)>"
      "<!ELEMENT a EMPTY><!ELEMENT b EMPTY>]><root><a/></root>";
  const auto p = parse(input);
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  ASSERT_EQ(count(p, GTEXT_XML_VALIDATION_ELEMENT_CONTENT), 1u);
  for (size_t i = 0; i < gtext_xml_validation_diagnostic_count(p.doc); ++i) {
    GTEXT_XML_Validation_Diagnostic diagnostic{};
    ASSERT_TRUE(gtext_xml_validation_diagnostic(p.doc, i, &diagnostic));
    if (diagnostic.code == GTEXT_XML_VALIDATION_ELEMENT_CONTENT) {
      EXPECT_EQ(diagnostic.location.offset, input.find("</root>"));
    }
  }
}

TEST(XmlValidation, UsesResolverSuppliedExternalSubset) {
  ExternalDtd dtd;
  dtd.bytes = "<!ELEMENT root (required)><!ELEMENT required EMPTY>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = open_external;
  options.resolver_user_data = &dtd;
  const std::string input = "<!DOCTYPE root SYSTEM 'urn:validation'><root/>";
  GTEXT_Markup_Document *doc = nullptr;
  GTEXT_XML_Error error{};
  ASSERT_EQ(gtext_xml_parse(input.data(), input.size(), &options, &doc, &error),
      GTEXT_XML_OK) << error.message;
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(dtd.opens, 1u);
  EXPECT_EQ(dtd.closes, 1u);
  EXPECT_EQ(count(doc, GTEXT_XML_VALIDATION_ELEMENT_CONTENT), 1u);
  gtext_markup_document_free(doc);
}

TEST(XmlValidation, PreservesInternalElementDeclarationPrecedence) {
  const std::string input = "<!DOCTYPE root SYSTEM 'urn:validation' ["
      "<!ELEMENT root (child)><!ELEMENT child EMPTY>]><root><child/></root>";
  ExternalDtd dtd;
  dtd.bytes = "<!ELEMENT root EMPTY><!ELEMENT child EMPTY>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = open_external;
  options.resolver_user_data = &dtd;
  Parsed result;
  result.status = gtext_xml_parse(input.data(), input.size(), &options,
      &result.doc, &result.error);
  ASSERT_EQ(result.status, GTEXT_XML_OK) << result.error.message;
  EXPECT_EQ(count(result, GTEXT_XML_VALIDATION_ELEMENT_CONTENT), 0u);
  EXPECT_EQ(count(result, GTEXT_XML_VALIDATION_DUPLICATE_ELEMENT_DECLARATION), 2u);
}

TEST(XmlValidation, MatchesNestedQuantifiedChildrenModels) {
  const auto valid = parse("<!DOCTYPE root [<!ELEMENT root ((a,b?)|c)+>"
      "<!ELEMENT a EMPTY><!ELEMENT b EMPTY><!ELEMENT c EMPTY>]><root>"
      "<a/><b/><c/><a/></root>");
  ASSERT_EQ(valid.status, GTEXT_XML_OK) << valid.error.message;
  EXPECT_EQ(gtext_xml_validation_diagnostic_count(valid.doc), 0u);
  const auto invalid = parse("<!DOCTYPE root [<!ELEMENT root ((a,b?)|c)+>"
      "<!ELEMENT a EMPTY><!ELEMENT b EMPTY><!ELEMENT c EMPTY>]><root>"
      "<a/><c/><b/></root>");
  ASSERT_EQ(invalid.status, GTEXT_XML_OK) << invalid.error.message;
  EXPECT_GE(count(invalid, GTEXT_XML_VALIDATION_ELEMENT_CONTENT), 1u);
}

TEST(XmlValidation, EmitsEquivalentFindingsFromPushStream) {
  const std::string input = "<!DOCTYPE root [<!ELEMENT root (child)><!ELEMENT child EMPTY>]><root><other/></root>";
  const auto dom = parse(input);
  ASSERT_EQ(dom.status, GTEXT_XML_OK) << dom.error.message;
  GTEXT_XML_Validation_Diagnostic first{};
  ASSERT_TRUE(gtext_xml_validation_diagnostic(dom.doc, 0, &first));
  EXPECT_EQ(first.location.offset, input.find("<other"));
  EXPECT_EQ(first.location.line, 1u);
  Events events;
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  GTEXT_XML_Stream *stream = gtext_xml_stream_new(&options, receive, &events);
  ASSERT_NE(stream, nullptr);
  GTEXT_XML_Error error{};
  GTEXT_XML_Status status = GTEXT_XML_OK;
  for (char byte : input) {
    status = gtext_xml_stream_feed(stream, &byte, 1, &error);
    if (status != GTEXT_XML_OK) break;
  }
  if (status == GTEXT_XML_OK) status = gtext_xml_stream_finish(stream, &error);
  EXPECT_EQ(status, GTEXT_XML_OK) << error.message;
  EXPECT_EQ(events.diagnostics, gtext_xml_validation_diagnostic_count(dom.doc));
  EXPECT_EQ(events.content, count(dom, GTEXT_XML_VALIDATION_ELEMENT_CONTENT));
  EXPECT_EQ(events.undeclared, count(dom, GTEXT_XML_VALIDATION_UNDECLARED_ELEMENT));
  gtext_xml_stream_free(stream);

  GTEXT_XML_Reader *reader = gtext_xml_reader_new(&options);
  ASSERT_NE(reader, nullptr);
  ASSERT_EQ(gtext_xml_reader_feed(reader, input.data(), input.size(), &error),
      GTEXT_XML_OK);
  ASSERT_EQ(gtext_xml_reader_feed(reader, nullptr, 0, &error), GTEXT_XML_OK);
  size_t pulled = 0;
  size_t pulled_content = 0, pulled_undeclared = 0;
  GTEXT_XML_Event event{};
  GTEXT_XML_Status read_status;
  while ((read_status = gtext_xml_reader_next(reader, &event)) == GTEXT_XML_OK) {
    if (event.type == GTEXT_XML_EVENT_VALIDATION_DIAGNOSTIC) {
      ++pulled;
      if (event.validation_diagnostic.code == GTEXT_XML_VALIDATION_ELEMENT_CONTENT)
        ++pulled_content;
      if (event.validation_diagnostic.code == GTEXT_XML_VALIDATION_UNDECLARED_ELEMENT)
        ++pulled_undeclared;
    }
  }
  EXPECT_EQ(read_status, GTEXT_XML_E_STATE);
  EXPECT_EQ(pulled, gtext_xml_validation_diagnostic_count(dom.doc));
  EXPECT_EQ(pulled_content, count(dom, GTEXT_XML_VALIDATION_ELEMENT_CONTENT));
  EXPECT_EQ(pulled_undeclared, count(dom, GTEXT_XML_VALIDATION_UNDECLARED_ELEMENT));
  gtext_xml_reader_free(reader);

  char path[] = "/tmp/gtext-xml-validation-XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file.good());
    file.write(input.data(), static_cast<std::streamsize>(input.size()));
    ASSERT_TRUE(file.good());
  }
  GTEXT_Markup_Document *file_doc = nullptr;
  ASSERT_EQ(gtext_xml_parse_file(path, &options, &file_doc, &error), GTEXT_XML_OK)
      << error.message;
  EXPECT_EQ(gtext_xml_validation_diagnostic_count(file_doc),
      gtext_xml_validation_diagnostic_count(dom.doc));
  gtext_markup_document_free(file_doc);
  unlink(path);
}

TEST(XmlValidation, WhitespaceInCdataIsAllowedInElementContent) {
  const auto p = parse("<!DOCTYPE root [<!ELEMENT root (child)><!ELEMENT child EMPTY>]><root><![CDATA[ \n\t ]]><child/></root>");
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  EXPECT_EQ(gtext_xml_validation_diagnostic_count(p.doc), 0u);
}

TEST(XmlValidation, ValidatesAnyAndMixedContentModels) {
  const auto any = parse("<!DOCTYPE root [<!ELEMENT root ANY><!ELEMENT child EMPTY>]><root>text<child/></root>");
  ASSERT_EQ(any.status, GTEXT_XML_OK) << any.error.message;
  EXPECT_EQ(gtext_xml_validation_diagnostic_count(any.doc), 0u);

  const auto mixed = parse("<!DOCTYPE root [<!ELEMENT root (#PCDATA|child)*><!ELEMENT child EMPTY>]><root>before<child/>after</root>");
  ASSERT_EQ(mixed.status, GTEXT_XML_OK) << mixed.error.message;
  EXPECT_EQ(gtext_xml_validation_diagnostic_count(mixed.doc), 0u);
}

TEST(XmlValidation, RejectsCharacterAndElementContentInEmptyModel) {
  const auto p = parse("<!DOCTYPE root [<!ELEMENT root EMPTY><!ELEMENT child EMPTY>]><root>text<child/></root>");
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  EXPECT_GE(count(p, GTEXT_XML_VALIDATION_ELEMENT_CONTENT), 2u);
}

TEST(XmlValidation, ReportsNondeterminismEvenWhenDocumentTakesOtherBranch) {
  const auto p = parse("<!DOCTYPE root [<!ELEMENT root ((a,b)|(a,c)|(d,e))><!ELEMENT a EMPTY><!ELEMENT b EMPTY><!ELEMENT c EMPTY><!ELEMENT d EMPTY><!ELEMENT e EMPTY>]><root><d/><e/></root>");
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  EXPECT_EQ(count(p, GTEXT_XML_VALIDATION_NONDETERMINISTIC_MODEL), 1u);
  GTEXT_XML_Validation_Diagnostic diagnostic{};
  ASSERT_TRUE(gtext_xml_validation_diagnostic(p.doc, 0, &diagnostic));
  EXPECT_EQ(diagnostic.location.offset,
      std::string("<!DOCTYPE root [<!ELEMENT root ((a,b)|(a,c)|(d,e))>").find("<!ELEMENT"));
}

TEST(XmlValidation, PreservesXml11CharacterRulesInContentValidation) {
  const auto p = parse("<?xml version='1.1'?><!DOCTYPE root [<!ELEMENT root EMPTY>]><root>&#x1;</root>");
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  EXPECT_EQ(count(p, GTEXT_XML_VALIDATION_ELEMENT_CONTENT), 1u);
}

TEST(XmlValidation, WorkAndDiagnosticBudgetsAreDistinct) {
  GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
  options.max_validation_diagnostics = 1;
  const auto p = parse("<!DOCTYPE root [<!ELEMENT root EMPTY>]><wrong><x/></wrong>", options);
  ASSERT_EQ(p.status, GTEXT_XML_OK) << p.error.message;
  EXPECT_TRUE(gtext_xml_validation_diagnostics_truncated(p.doc));

  Events events;
  auto truncated_options = gtext_xml_parse_options_default();
  truncated_options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  truncated_options.max_validation_diagnostics = 1;
  GTEXT_XML_Stream *stream = gtext_xml_stream_new(&truncated_options, receive,
      &events);
  ASSERT_NE(stream, nullptr);
  GTEXT_XML_Error stream_error{};
  const std::string invalid = "<!DOCTYPE root [<!ELEMENT root EMPTY>]><wrong><child/></wrong>";
  ASSERT_EQ(gtext_xml_stream_feed(stream, invalid.data(), invalid.size(),
      &stream_error), GTEXT_XML_OK) << stream_error.message;
  ASSERT_EQ(gtext_xml_stream_finish(stream, &stream_error), GTEXT_XML_OK)
      << stream_error.message;
  EXPECT_EQ(events.truncated, 1u);
  gtext_xml_stream_free(stream);

  options = gtext_xml_parse_options_default();
  options.max_validation_work = 1;
  Parsed limited;
  const std::string input = "<!DOCTYPE root [<!ELEMENT root (a,b,c)><!ELEMENT a EMPTY><!ELEMENT b EMPTY><!ELEMENT c EMPTY>]><root><a/><b/><c/></root>";
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  limited.status = gtext_xml_parse(input.data(), input.size(), &options,
      &limited.doc, &limited.error);
  EXPECT_EQ(limited.status, GTEXT_XML_E_LIMIT_WORK);
  EXPECT_EQ(limited.doc, nullptr);

  options = gtext_xml_parse_options_default();
  options.max_validation_memory = 16;
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  const std::string model = "<!DOCTYPE root [<!ELEMENT root (a,b)><!ELEMENT a EMPTY><!ELEMENT b EMPTY>]><root><a/><b/></root>";
  limited.status = gtext_xml_parse(model.data(), model.size(), &options,
      &limited.doc, &limited.error);
  EXPECT_EQ(limited.status, GTEXT_XML_E_LIMIT_WORK);
  EXPECT_EQ(limited.doc, nullptr);
}
} // namespace
