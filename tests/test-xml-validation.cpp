/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>
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
struct ExternalPair {
  ExternalDtd subset;
  ExternalDtd entity;
  size_t opens = 0;
  size_t closes = 0;
};
struct LiveAllocator {
  GTEXT_Allocator allocator{};
  size_t blocks = 0;
  LiveAllocator() {
    allocator.ctx = this;
    allocator.malloc_fn = [](void *ctx, size_t size) {
      auto *self = static_cast<LiveAllocator *>(ctx);
      void *memory = std::malloc(size ? size : 1);
      if (memory) ++self->blocks;
      return memory;
    };
    allocator.calloc_fn = [](void *ctx, size_t count, size_t size) {
      auto *self = static_cast<LiveAllocator *>(ctx);
      if (size && count > SIZE_MAX / size) return static_cast<void *>(nullptr);
      void *memory = std::calloc(count && size ? count : 1,
          count && size ? size : 1);
      if (memory) ++self->blocks;
      return memory;
    };
    allocator.realloc_fn = [](void *ctx, void *old, size_t size) {
      auto *self = static_cast<LiveAllocator *>(ctx);
      void *memory = std::realloc(old, size ? size : 1);
      if (memory && !old) ++self->blocks;
      return memory;
    };
    allocator.free_fn = [](void *ctx, void *memory) {
      auto *self = static_cast<LiveAllocator *>(ctx);
      if (memory) {
        EXPECT_GT(self->blocks, 0u);
        --self->blocks;
      }
      std::free(memory);
    };
  }
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
GTEXT_XML_Resolver_Result open_external_pair(
    const GTEXT_XML_Resolve_Request *request, void *opaque,
    GTEXT_XML_External_Source *source) {
  auto *pair = static_cast<ExternalPair *>(opaque);
  ++pair->opens;
  ExternalDtd *selected = request->kind == GTEXT_XML_RESOURCE_GENERAL_ENTITY
      ? &pair->entity : &pair->subset;
  source->handle = selected;
  source->identity = selected;
  source->base_uri = request->kind == GTEXT_XML_RESOURCE_GENERAL_ENTITY
      ? "memory:/entity.xml" : "memory:/validation.dtd";
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
struct Events {
  size_t diagnostics = 0, content = 0, undeclared = 0, attribute = 0;
  size_t idref = 0, truncated = 0, results = 0;
  GTEXT_XML_Validation_Result result{};
};
struct OrderedEvents {
  std::vector<GTEXT_XML_Event_Type> types;
  GTEXT_XML_Validation_Result result{};
};
GTEXT_XML_Status receive_ordered(const GTEXT_XML_Event *event, void *opaque) {
  auto *events = static_cast<OrderedEvents *>(opaque);
  events->types.push_back(event->type);
  if (event->type == GTEXT_XML_EVENT_VALIDATION_RESULT)
    events->result = event->validation_result;
  return GTEXT_XML_OK;
}
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
  if (event->type == GTEXT_XML_EVENT_VALIDATION_RESULT
      && event->validation_result.verdict != GTEXT_XML_VALIDATION_NOT_RUN) {
    ++events->results;
    events->result = event->validation_result;
  }
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

TEST(XmlValidation, ReportsVerdictAndKnowledgeIndependently) {
  const auto valid = parse("<!DOCTYPE root [<!ELEMENT root EMPTY>]><root/>");
  ASSERT_EQ(valid.status, GTEXT_XML_OK) << valid.error.message;
  GTEXT_XML_Validation_Result result{};
  ASSERT_TRUE(gtext_xml_validation_result(valid.doc, &result));
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_VALID);
  EXPECT_TRUE(result.knowledge_complete);
  EXPECT_EQ(result.diagnostic_count, 0u);

  const auto no_doctype = parse("<root/>");
  ASSERT_EQ(no_doctype.status, GTEXT_XML_OK) << no_doctype.error.message;
  ASSERT_TRUE(gtext_xml_validation_result(no_doctype.doc, &result));
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_INCOMPLETE);
  EXPECT_FALSE(result.knowledge_complete);

  const auto incomplete = parse("<!DOCTYPE root SYSTEM 'urn:missing'><root/>");
  ASSERT_EQ(incomplete.status, GTEXT_XML_OK) << incomplete.error.message;
  ASSERT_TRUE(gtext_xml_validation_result(incomplete.doc, &result));
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_INCOMPLETE);
  EXPECT_FALSE(result.knowledge_complete);

  const auto invalid_incomplete = parse("<!DOCTYPE root SYSTEM 'urn:missing' ["
      "<!ELEMENT root EMPTY>]><root><child/></root>");
  ASSERT_EQ(invalid_incomplete.status, GTEXT_XML_OK)
      << invalid_incomplete.error.message;
  ASSERT_TRUE(gtext_xml_validation_result(invalid_incomplete.doc, &result));
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_INVALID);
  EXPECT_FALSE(result.knowledge_complete);
  EXPECT_EQ(result.diagnostic_count, 1u);

  const auto not_well_formed = parse("<root>");
  EXPECT_EQ(not_well_formed.status, GTEXT_XML_E_UNEXPECTED_EOF);
  EXPECT_EQ(not_well_formed.doc, nullptr);

  auto process = gtext_xml_parse_options_default();
  process.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_Markup_Document *process_doc = nullptr;
  GTEXT_XML_Error error{};
  ASSERT_EQ(gtext_xml_parse("<root/>", 7, &process, &process_doc, &error),
      GTEXT_XML_OK) << error.message;
  EXPECT_FALSE(gtext_xml_validation_result(process_doc, &result));
  gtext_markup_document_free(process_doc);
}

TEST(XmlValidation, MarksExternalKnowledgeStaleAfterInternalDtdEdit) {
  ExternalDtd dtd;
  dtd.bytes = "<!ELEMENT other EMPTY>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = open_external;
  options.resolver_user_data = &dtd;
  const std::string input = "<!DOCTYPE root SYSTEM 'urn:validation' ["
      "<!ELEMENT root EMPTY>]><root/>";
  Parsed parsed;
  parsed.status = gtext_xml_parse(input.data(), input.size(), &options,
      &parsed.doc, &parsed.error);
  ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
  GTEXT_XML_Validation_Result result{};
  ASSERT_TRUE(gtext_xml_validation_result(parsed.doc, &result));
  ASSERT_EQ(result.verdict, GTEXT_XML_VALIDATION_VALID);

  GTEXT_Markup_Node *doctype = gtext_markup_document_doctype(parsed.doc);
  ASSERT_NE(doctype, nullptr);
  const char replacement[] = "<!ELEMENT root ANY>";
  ASSERT_EQ(gtext_markup_doctype_replace_syntax(doctype, 0,
      GTEXT_MARKUP_DTD_ELEMENT, replacement, sizeof(replacement) - 1, nullptr),
      GTEXT_MARKUP_OK);
  EXPECT_TRUE(gtext_markup_document_dtd_knowledge_incomplete(parsed.doc));
  EXPECT_FALSE(gtext_xml_validation_result(parsed.doc, &result));
  GTEXT_XML_Error revalidate_error{};
  EXPECT_EQ(gtext_xml_revalidate(parsed.doc, nullptr, &result,
      &revalidate_error), GTEXT_XML_OK) << revalidate_error.message;
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_INCOMPLETE);
  EXPECT_FALSE(result.knowledge_complete);
  EXPECT_EQ(dtd.opens, 1u);
}

TEST(XmlValidation, PullNeedMoreInputDoesNotProduceAValidityResult) {
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  GTEXT_XML_Reader *reader = gtext_xml_reader_new(&options);
  ASSERT_NE(reader, nullptr);
  GTEXT_XML_Error error{};
  const std::string partial = "<!DOCTYPE root [<!ELEMENT root EMPTY>]><root";
  ASSERT_EQ(gtext_xml_reader_feed(reader, partial.data(), partial.size(), &error), GTEXT_XML_OK)
      << error.message;
  GTEXT_XML_Event event{};
  GTEXT_XML_Status status = GTEXT_XML_OK;
  while ((status = gtext_xml_reader_next(reader, &event)) == GTEXT_XML_OK) {
    EXPECT_FALSE(event.type == GTEXT_XML_EVENT_DOCUMENT_END
        && event.validation_result.verdict != GTEXT_XML_VALIDATION_NOT_RUN);
  }
  EXPECT_EQ(status, GTEXT_XML_E_INCOMPLETE);
  ASSERT_EQ(gtext_xml_reader_feed(reader, "/>" , 2, &error), GTEXT_XML_OK)
      << error.message;
  ASSERT_EQ(gtext_xml_reader_feed(reader, nullptr, 0, &error), GTEXT_XML_OK)
      << error.message;
  size_t result_count = 0;
  bool saw_validation_result = false;
  status = GTEXT_XML_OK;
  while ((status = gtext_xml_reader_next(reader, &event)) == GTEXT_XML_OK) {
    if (event.type == GTEXT_XML_EVENT_VALIDATION_RESULT) {
      ++result_count;
      EXPECT_EQ(event.validation_result.verdict, GTEXT_XML_VALIDATION_VALID);
      EXPECT_TRUE(event.validation_result.knowledge_complete);
      EXPECT_FALSE(event.validation_result.diagnostics_truncated);
      EXPECT_EQ(event.validation_result.diagnostic_count, 0u);
      saw_validation_result = true;
    }
    if (event.type == GTEXT_XML_EVENT_DOCUMENT_END) {
      EXPECT_TRUE(saw_validation_result);
      EXPECT_EQ(event.validation_result.verdict, GTEXT_XML_VALIDATION_VALID);
    }
  }
  EXPECT_EQ(status, GTEXT_XML_E_STATE);
  EXPECT_EQ(result_count, 1u);
  gtext_xml_reader_free(reader);
}

TEST(XmlValidation, PushStreamEmitsCompleteResultBeforeDocumentEnd) {
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  OrderedEvents events;
  GTEXT_XML_Stream *stream = gtext_xml_stream_new(&options, receive_ordered,
      &events);
  ASSERT_NE(stream, nullptr);
  const std::string valid =
      "<!DOCTYPE root [<!ELEMENT root EMPTY>]><root/>";
  GTEXT_XML_Error error{};
  ASSERT_EQ(gtext_xml_stream_feed(stream, valid.data(), valid.size(), &error),
      GTEXT_XML_OK) << error.message;
  ASSERT_EQ(gtext_xml_stream_finish(stream, &error), GTEXT_XML_OK)
      << error.message;
  const auto result = std::find(events.types.begin(), events.types.end(),
      GTEXT_XML_EVENT_VALIDATION_RESULT);
  const auto end = std::find(events.types.begin(), events.types.end(),
      GTEXT_XML_EVENT_DOCUMENT_END);
  ASSERT_NE(result, events.types.end());
  ASSERT_NE(end, events.types.end());
  EXPECT_LT(result, end);
  EXPECT_EQ(events.result.verdict, GTEXT_XML_VALIDATION_VALID);
  EXPECT_TRUE(events.result.knowledge_complete);
  EXPECT_FALSE(events.result.diagnostics_truncated);
  EXPECT_EQ(events.result.diagnostic_count, 0u);
  gtext_xml_stream_free(stream);

  events = {};
  stream = gtext_xml_stream_new(&options, receive_ordered, &events);
  ASSERT_NE(stream, nullptr);
  const char without_doctype[] = "<root/>";
  ASSERT_EQ(gtext_xml_stream_feed(stream, without_doctype,
      sizeof(without_doctype) - 1, &error), GTEXT_XML_OK) << error.message;
  ASSERT_EQ(gtext_xml_stream_finish(stream, &error), GTEXT_XML_OK)
      << error.message;
  EXPECT_EQ(events.result.verdict, GTEXT_XML_VALIDATION_INCOMPLETE);
  EXPECT_FALSE(events.result.knowledge_complete);
  EXPECT_FALSE(events.result.diagnostics_truncated);
  EXPECT_EQ(events.result.diagnostic_count, 0u);
  gtext_xml_stream_free(stream);
}

TEST(XmlValidation, FatalStreamFailureDoesNotPublishAValidityResult) {
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  Events events;
  GTEXT_XML_Stream *stream = gtext_xml_stream_new(&options, receive, &events);
  ASSERT_NE(stream, nullptr);
  const char malformed[] = "<root>";
  ASSERT_EQ(gtext_xml_stream_feed(stream, malformed, sizeof(malformed) - 1,
      nullptr), GTEXT_XML_OK);
  EXPECT_EQ(gtext_xml_stream_finish(stream, nullptr),
      GTEXT_XML_E_UNEXPECTED_EOF);
  EXPECT_EQ(events.results, 0u);
  gtext_xml_stream_free(stream);
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
  EXPECT_EQ(events.results, 1u);
  EXPECT_EQ(events.result.verdict, GTEXT_XML_VALIDATION_INVALID);
  EXPECT_TRUE(events.result.knowledge_complete);
  EXPECT_FALSE(events.result.diagnostics_truncated);
  EXPECT_EQ(events.result.diagnostic_count,
      gtext_xml_validation_diagnostic_count(dom.doc));
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
  GTEXT_XML_Validation_Result result{};
  ASSERT_TRUE(gtext_xml_validation_result(p.doc, &result));
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_INVALID);
  EXPECT_TRUE(result.knowledge_complete);
  EXPECT_TRUE(result.diagnostics_truncated);
  EXPECT_EQ(result.diagnostic_count, 1u);

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
  EXPECT_EQ(events.results, 1u);
  EXPECT_EQ(events.result.verdict, GTEXT_XML_VALIDATION_INVALID);
  EXPECT_TRUE(events.result.knowledge_complete);
  EXPECT_TRUE(events.result.diagnostics_truncated);
  EXPECT_EQ(events.result.diagnostic_count, 1u);
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
TEST(XmlValidation, RevalidatesCurrentTreeAndInvalidatesPreviousResult) {
  Parsed parsed = parse("<!DOCTYPE root [<!ELEMENT root EMPTY>]><root/>");
  ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
  GTEXT_XML_Validation_Result result{};
  ASSERT_TRUE(gtext_xml_validation_result(parsed.doc, &result));
  ASSERT_EQ(result.verdict, GTEXT_XML_VALIDATION_VALID);

  GTEXT_Markup_Node *root = gtext_markup_document_element(parsed.doc);
  GTEXT_Markup_Node *text = nullptr;
  ASSERT_EQ(gtext_markup_create_text(parsed.doc, "content", 7, &text),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(root, text), GTEXT_MARKUP_OK);
  EXPECT_FALSE(gtext_xml_validation_result(parsed.doc, &result));
  EXPECT_EQ(gtext_xml_validation_diagnostic_count(parsed.doc), 0u);

  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_revalidate(parsed.doc, nullptr, &result, &error),
      GTEXT_XML_OK) << error.message;
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_INVALID);
  EXPECT_EQ(result.diagnostic_count, 1u);
  EXPECT_EQ(count(parsed.doc, GTEXT_XML_VALIDATION_ELEMENT_CONTENT), 1u);
}

TEST(XmlValidation, RevalidationLimitFailurePreservesTreeAndOldResult) {
  Parsed parsed = parse("<!DOCTYPE root [<!ELEMENT root EMPTY>]><root/>");
  ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
  GTEXT_XML_Validation_Result before{};
  ASSERT_TRUE(gtext_xml_validation_result(parsed.doc, &before));
  GTEXT_XML_Revalidation_Options limits{.max_work = 1,
      .max_memory = 0, .max_diagnostics = 0};
  GTEXT_XML_Validation_Result after{};
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_revalidate(parsed.doc, &limits, &after, &error),
      GTEXT_XML_E_LIMIT_WORK);
  EXPECT_EQ(gtext_markup_node_kind(gtext_markup_document_element(parsed.doc)),
      GTEXT_MARKUP_KIND_ELEMENT);
  GTEXT_XML_Validation_Result still_current{};
  EXPECT_TRUE(gtext_xml_validation_result(parsed.doc, &still_current));
  EXPECT_EQ(still_current.verdict, before.verdict);
}

TEST(XmlValidation, RevalidationMemoryBudgetIncludesRetainedDtdReplay) {
  std::string input = "<!DOCTYPE root [<!ELEMENT root EMPTY>";
  for (unsigned i = 0; i < 32; ++i) {
    input += "<!ELEMENT item" + std::to_string(i) + " EMPTY>";
  }
  input += "]><root/>";
  Parsed parsed = parse(input);
  ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
  GTEXT_XML_Validation_Result before{};
  ASSERT_TRUE(gtext_xml_validation_result(parsed.doc, &before));
  GTEXT_XML_Revalidation_Options limits{.max_work = 0,
      .max_memory = 256, .max_diagnostics = 0};
  GTEXT_XML_Validation_Result result{};
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_revalidate(parsed.doc, &limits, &result, &error),
      GTEXT_XML_E_LIMIT_WORK);
  GTEXT_XML_Validation_Result still_current{};
  EXPECT_TRUE(gtext_xml_validation_result(parsed.doc, &still_current));
  EXPECT_EQ(still_current.verdict, before.verdict);
}

TEST(XmlValidation, RevalidationMemoryBudgetIncludesEntityRecordCapacity) {
  Parsed parsed = parse("<!DOCTYPE root [<!ENTITY e 'x'>]><root/>");
  ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;

  /* A single replayed entity causes the scratch entity table to reserve its
     initial geometric capacity; charging only the retained name would miss
     that larger allocation under this cap. */
  GTEXT_XML_Revalidation_Options limits{.max_work = 0,
      .max_memory = 1024, .max_diagnostics = 0};
  GTEXT_XML_Validation_Result result{};
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_revalidate(parsed.doc, &limits, &result, &error),
      GTEXT_XML_E_LIMIT_WORK);
}

TEST(XmlValidation, RevalidationReleasesEntityReplayAllocations) {
  LiveAllocator allocator;
  auto options = gtext_xml_parse_options_default();
  options.allocator = &allocator.allocator;
  Parsed parsed = parse("<!DOCTYPE root [<!ELEMENT root EMPTY>"
      "<!ENTITY e 'x'>]><root/>", options);
  ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
  const size_t before = allocator.blocks;

  GTEXT_XML_Validation_Result result{};
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_revalidate(parsed.doc, nullptr, &result, &error),
      GTEXT_XML_OK) << error.message;
  EXPECT_EQ(allocator.blocks, before);

  gtext_markup_document_free(parsed.doc);
  parsed.doc = nullptr;

  std::string many_children = "<!DOCTYPE root [<!ENTITY e 'x'>]><root>";
  for (size_t i = 0; i < 50; ++i) many_children += "<child/>";
  many_children += "</root>";
  Parsed limited = parse(many_children, options);
  ASSERT_EQ(limited.status, GTEXT_XML_OK) << limited.error.message;
  const size_t before_limited = allocator.blocks;
  GTEXT_XML_Revalidation_Options work_limit{.max_work = 100,
      .max_memory = 0, .max_diagnostics = 0};
  EXPECT_EQ(gtext_xml_revalidate(limited.doc, &work_limit, &result, &error),
      GTEXT_XML_E_LIMIT_WORK);
  EXPECT_EQ(allocator.blocks, before_limited);
  gtext_markup_document_free(limited.doc);
  limited.doc = nullptr;
  EXPECT_EQ(allocator.blocks, 0u);
}

TEST(XmlValidation, RevalidationNeverCallsTheOriginalResolver) {
  ExternalDtd dtd;
  dtd.bytes = "<!ELEMENT root EMPTY>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = open_external;
  options.resolver_user_data = &dtd;
  const std::string input = "<!DOCTYPE root SYSTEM 'urn:validation'><root/>";
  Parsed parsed;
  parsed.status = gtext_xml_parse(input.data(), input.size(), &options,
      &parsed.doc, &parsed.error);
  ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
  ASSERT_EQ(dtd.opens, 1u);

  GTEXT_XML_Validation_Result result{};
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_revalidate(parsed.doc, nullptr, &result, &error),
      GTEXT_XML_OK) << error.message;
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_VALID);
  EXPECT_EQ(dtd.opens, 1u);
  EXPECT_EQ(dtd.closes, 1u);
}

TEST(XmlValidation, RevalidatesStoredDefaultsAndExpandedEntityNodes) {
  Parsed parsed = parse("<!DOCTYPE root [<!ELEMENT root (item)><!ELEMENT item EMPTY>"
      "<!ATTLIST root mode CDATA 'safe'><!ENTITY kid '<item/>'>]><root>&kid;</root>");
  ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
  GTEXT_Markup_Attribute attr{};
  ASSERT_TRUE(gtext_markup_element_get_attribute(
      gtext_markup_document_element(parsed.doc), nullptr, "mode", &attr));
  EXPECT_EQ(attr.origin, GTEXT_MARKUP_ATTRIBUTE_DEFAULTED);
  EXPECT_EQ(gtext_markup_node_kind(gtext_markup_node_first_child(
      gtext_markup_document_element(parsed.doc))), GTEXT_MARKUP_KIND_ELEMENT);

  GTEXT_XML_Validation_Result result{};
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_revalidate(parsed.doc, nullptr, &result, &error),
      GTEXT_XML_OK) << error.message;
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_VALID);
}

TEST(XmlValidation, RevalidatesProgrammaticallyCreatedDocument) {
  GTEXT_Markup_Document *doc = nullptr;
  ASSERT_EQ(gtext_markup_document_new(nullptr, &doc), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node *doctype = nullptr;
  ASSERT_EQ(gtext_markup_create_doctype(doc, "root", nullptr, nullptr,
      &doctype), GTEXT_MARKUP_OK);
  const char declaration[] = "<!ELEMENT root EMPTY>";
  ASSERT_EQ(gtext_markup_doctype_insert_syntax(doctype, 0,
      GTEXT_MARKUP_DTD_ELEMENT, declaration, sizeof(declaration) - 1, nullptr),
      GTEXT_MARKUP_OK);
  GTEXT_Markup_Node *root = nullptr;
  ASSERT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "root", &root),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(doc), doctype),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(doc), root),
      GTEXT_MARKUP_OK);

  GTEXT_XML_Validation_Result result{};
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_revalidate(doc, nullptr, &result, &error), GTEXT_XML_OK)
      << error.message;
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_VALID);
  gtext_markup_document_free(doc);
}

TEST(XmlValidation, RevalidationWithoutDoctypeReportsIncompleteKnowledge) {
  GTEXT_Markup_Document *doc = nullptr;
  ASSERT_EQ(gtext_markup_document_new(nullptr, &doc), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node *root = nullptr;
  ASSERT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "root", &root),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(doc), root),
      GTEXT_MARKUP_OK);
  GTEXT_XML_Validation_Result result{};
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_revalidate(doc, nullptr, &result, &error), GTEXT_XML_OK)
      << error.message;
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_INCOMPLETE);
  EXPECT_FALSE(result.knowledge_complete);
  gtext_markup_document_free(doc);
}

TEST(XmlValidation, RevalidationRebuildsUnparsedEntityAndNotationRules) {
  const auto valid = parse("<!DOCTYPE root [<!ELEMENT root EMPTY>"
      "<!NOTATION image SYSTEM 'image/gif'>"
      "<!ENTITY icon SYSTEM 'icon.gif' NDATA image>"
      "<!ATTLIST root one ENTITY #IMPLIED many ENTITIES #IMPLIED>]><root"
      " one='icon' many='icon icon'/>");
  ASSERT_EQ(valid.status, GTEXT_XML_OK) << valid.error.message;
  GTEXT_XML_Validation_Result valid_result{};
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_revalidate(valid.doc, nullptr, &valid_result, &error),
      GTEXT_XML_OK) << error.message;
  EXPECT_EQ(valid_result.verdict, GTEXT_XML_VALIDATION_VALID);

  const auto invalid_notation = parse("<!DOCTYPE root [<!ELEMENT root EMPTY>"
      "<!ENTITY icon SYSTEM 'icon.gif' NDATA absent>]><root/>");
  ASSERT_EQ(invalid_notation.status, GTEXT_XML_OK) << invalid_notation.error.message;
  GTEXT_XML_Validation_Result invalid_result{};
  EXPECT_EQ(gtext_xml_revalidate(invalid_notation.doc, nullptr,
      &invalid_result, &error), GTEXT_XML_OK) << error.message;
  EXPECT_EQ(invalid_result.verdict, GTEXT_XML_VALIDATION_INVALID);
  EXPECT_GE(count(invalid_notation.doc,
      GTEXT_XML_VALIDATION_DTD_CROSS_REFERENCE), 1u);

  const auto invalid_attribute = parse("<!DOCTYPE root [<!ELEMENT root EMPTY>"
      "<!NOTATION image SYSTEM 'image/gif'>"
      "<!ENTITY icon SYSTEM 'icon.gif' NDATA image>"
      "<!ENTITY text 'parsed'>"
      "<!ATTLIST root one ENTITY #IMPLIED many ENTITIES #IMPLIED>]><root"
      " one='text' many='icon text'/>");
  ASSERT_EQ(invalid_attribute.status, GTEXT_XML_OK) << invalid_attribute.error.message;
  EXPECT_EQ(gtext_xml_revalidate(invalid_attribute.doc, nullptr,
      &invalid_result, &error), GTEXT_XML_OK) << error.message;
  EXPECT_EQ(invalid_result.verdict, GTEXT_XML_VALIDATION_INVALID);
  EXPECT_GE(count(invalid_attribute.doc,
      GTEXT_XML_VALIDATION_ATTRIBUTE_VALUE), 2u);
}

TEST(XmlValidation, RevalidationRetainsStandaloneExternalEntityFinding) {
  ExternalPair resources;
  resources.subset.bytes = "<!ELEMENT root ANY><!ELEMENT child EMPTY>"
      "<!ENTITY external SYSTEM 'entity.xml'>";
  resources.entity.bytes = "<child/>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = open_external_pair;
  options.resolver_user_data = &resources;
  const std::string input = "<?xml version='1.0' standalone='yes'?>"
      "<!DOCTYPE root SYSTEM 'urn:validation'><root>&external;</root>";
  Parsed parsed;
  parsed.status = gtext_xml_parse(input.data(), input.size(), &options,
      &parsed.doc, &parsed.error);
  ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
  ASSERT_EQ(resources.opens, 2u);
  EXPECT_GE(count(parsed.doc, GTEXT_XML_VALIDATION_STANDALONE), 1u);

  GTEXT_XML_Validation_Result result{};
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_revalidate(parsed.doc, nullptr, &result, &error),
      GTEXT_XML_OK) << error.message;
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_INVALID);
  EXPECT_GE(count(parsed.doc, GTEXT_XML_VALIDATION_STANDALONE), 1u);
  EXPECT_EQ(resources.opens, 2u);
}

TEST(XmlValidation, RevalidationSeesEditedEnumeratedAttribute) {
  const auto parsed = parse("<!DOCTYPE root [<!ELEMENT root EMPTY>"
      "<!ATTLIST root mode (fast|safe) #REQUIRED>]><root mode='fast'/>");
  ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
  GTEXT_XML_Validation_Result result{};
  ASSERT_TRUE(gtext_xml_validation_result(parsed.doc, &result));
  ASSERT_EQ(result.verdict, GTEXT_XML_VALIDATION_VALID);
  GTEXT_Markup_Node *document_root = gtext_markup_document_root(parsed.doc);
  GTEXT_Markup_Node *root = gtext_markup_node_first_child(document_root);
  while (root && gtext_markup_node_kind(root) != GTEXT_MARKUP_KIND_ELEMENT)
    root = gtext_markup_node_next_in_order(document_root, root, false);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(gtext_markup_element_set_attribute(root, nullptr, nullptr,
      "mode", "invalid", 7), GTEXT_MARKUP_OK);
  EXPECT_FALSE(gtext_xml_validation_result(parsed.doc, &result));
  GTEXT_XML_Error error{};
  ASSERT_EQ(gtext_xml_revalidate(parsed.doc, nullptr, &result, &error),
      GTEXT_XML_OK) << error.message;
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_INVALID);
  EXPECT_GE(count(parsed.doc, GTEXT_XML_VALIDATION_ATTRIBUTE_VALUE), 1u);
}

} // namespace
