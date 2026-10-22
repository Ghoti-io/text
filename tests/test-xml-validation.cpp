/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <ostream>
#include <vector>
#include <unistd.h>
#include <gtest/gtest.h>
#include <ghoti.io/security/sha256.h>
#include <ghoti.io/text/xml.h>
#include <ghoti.io/text/xml/xml_parse.h>
#include <ghoti.io/text/xml/xml_stream.h>

#include "../src/xml/xml_internal.h"

namespace {
struct LiveAllocator;
struct ExternalDtd {
  std::string bytes;
  size_t offset = 0;
  size_t opens = 0;
  size_t closes = 0;
  GTEXT_XML_Resolver_Result open_result = GTEXT_XML_RESOLVER_OPENED;
  GTEXT_XML_Source_Read_Result read_result = GTEXT_XML_SOURCE_DATA;
  LiveAllocator *fail_allocator = nullptr;
};
struct ExternalPair {
  ExternalDtd subset;
  ExternalDtd entity;
  ExternalDtd leaf;
  size_t opens = 0;
  size_t closes = 0;
};
struct LiveAllocator {
  GTEXT_Allocator allocator{};
  size_t blocks = 0;
  size_t allocation_calls = 0;
  size_t fail_after = SIZE_MAX;
  LiveAllocator() {
    allocator.ctx = this;
    allocator.malloc_fn = [](void *ctx, size_t size) -> void * {
      auto *self = static_cast<LiveAllocator *>(ctx);
      if (self->allocation_calls++ >= self->fail_after) return nullptr;
      void *memory = std::malloc(size ? size : 1);
      if (memory) ++self->blocks;
      return memory;
    };
    allocator.calloc_fn = [](void *ctx, size_t count, size_t size) -> void * {
      auto *self = static_cast<LiveAllocator *>(ctx);
      if (self->allocation_calls++ >= self->fail_after) return nullptr;
      if (size && count > SIZE_MAX / size) return static_cast<void *>(nullptr);
      void *memory = std::calloc(count && size ? count : 1,
          count && size ? size : 1);
      if (memory) ++self->blocks;
      return memory;
    };
    allocator.realloc_fn = [](void *ctx, void *old, size_t size) -> void * {
      auto *self = static_cast<LiveAllocator *>(ctx);
      if (self->allocation_calls++ >= self->fail_after) return nullptr;
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
  if (source->read_result != GTEXT_XML_SOURCE_DATA) {
    *count = 0;
    return source->read_result;
  }
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
  if (dtd->fail_allocator)
    dtd->fail_allocator->fail_after = dtd->fail_allocator->allocation_calls;
  dtd->offset = 0;
  if (dtd->open_result != GTEXT_XML_RESOLVER_OPENED)
    return dtd->open_result;
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
  const char *system_id = request->system_id ? request->system_id : "";
  ExternalDtd *selected = std::strcmp(system_id, "urn:inner") == 0
      ? &pair->entity : std::strcmp(system_id, "urn:leaf") == 0
      ? &pair->leaf
      : request->kind == GTEXT_XML_RESOURCE_GENERAL_ENTITY
          ? &pair->entity : &pair->subset;
  selected->offset = 0;
  source->handle = selected;
  source->identity = selected;
  source->base_uri = selected == &pair->subset ? "memory:/validation.dtd"
      : selected == &pair->entity ? "memory:/inner.pe" : "memory:/leaf.pe";
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

TEST(XmlValidation, ExplicitRefreshUsesEditedInternalParameterEntity) {
  ExternalDtd dtd;
  dtd.bytes = "<!ELEMENT root EMPTY>%decl;";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = open_external;
  options.resolver_user_data = &dtd;
  const std::string input = "<!DOCTYPE root SYSTEM 'urn:validation' ["
      "<!ENTITY % decl '<!ATTLIST root mode (old|new) #IMPLIED>'>]><root mode='old'/>";
  Parsed parsed;
  parsed.status = gtext_xml_parse(input.data(), input.size(), &options,
      &parsed.doc, &parsed.error);
  ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
  ASSERT_EQ(dtd.closes, 1u);

  auto *doctype = gtext_markup_document_doctype(parsed.doc);
  GTEXT_Markup_DTD_Edit_Options edit{};
  size_t entity_index = SIZE_MAX;
  for (size_t i = 0; i < gtext_markup_doctype_syntax_count(doctype); ++i) {
    GTEXT_Markup_DTD_Syntax syntax{};
    ASSERT_TRUE(gtext_markup_doctype_syntax(doctype, i, &syntax));
    if (syntax.kind == GTEXT_MARKUP_DTD_ENTITY
        && std::string(syntax.source, syntax.source_length).find("% decl") != std::string::npos) {
      entity_index = i;
      break;
    }
  }
  ASSERT_NE(entity_index, SIZE_MAX);
  const char replacement[] = "<!ENTITY % decl '<!ATTLIST root mode (new) #IMPLIED>'>";
  ASSERT_EQ(gtext_markup_doctype_replace_syntax(doctype, entity_index,
      GTEXT_MARKUP_DTD_ENTITY, replacement, sizeof(replacement) - 1, &edit),
      GTEXT_MARKUP_OK);
  EXPECT_TRUE(gtext_markup_document_dtd_knowledge_incomplete(parsed.doc));

  GTEXT_XML_Validation_Result result{};
  GTEXT_XML_Revalidation_Options revalidation{};
  ASSERT_EQ(gtext_xml_revalidate(parsed.doc, &revalidation, &result,
      &parsed.error), GTEXT_XML_OK);
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_INCOMPLETE);
  EXPECT_EQ(dtd.opens, 1u); // Revalidation never invokes the resolver.

  GTEXT_XML_External_DTD_Refresh_Options refresh{};
  refresh.resolver = open_external;
  refresh.resolver_user_data = &dtd;
  ASSERT_EQ(gtext_xml_refresh_external_dtd(parsed.doc, &refresh, &result,
      &parsed.error), GTEXT_XML_OK) << parsed.error.message;
  EXPECT_EQ(result.verdict, GTEXT_XML_VALIDATION_INVALID);
  EXPECT_EQ(dtd.opens, 2u);
  EXPECT_EQ(dtd.closes, 2u);
  auto *refreshed_doctype = gtext_markup_document_doctype(parsed.doc);
  GTEXT_Markup_DTD_Syntax external{};
  ASSERT_GT(gtext_markup_doctype_external_syntax_count(refreshed_doctype), 0u);
  ASSERT_TRUE(gtext_markup_doctype_external_syntax(refreshed_doctype, 0,
      &external));
  GTEXT_Markup_Source source{};
  ASSERT_TRUE(gtext_markup_document_source(parsed.doc,
      external.location.source_id, &source));
  EXPECT_STREQ(source.base_uri, "memory:/validation.dtd");
  EXPECT_STREQ(source.encoding, "UTF-8");
  EXPECT_TRUE(source.has_resolver_identity_token);
  EXPECT_EQ(source.resolver_identity_token, reinterpret_cast<uintptr_t>(&dtd));
  EXPECT_EQ(source.byte_count, dtd.bytes.size());
  EXPECT_TRUE(source.has_sha256);
  unsigned char expected_sha256[32];
  ASSERT_EQ(gsec_sha256(dtd.bytes.data(), dtd.bytes.size(), expected_sha256),
      GSEC_OK);
  EXPECT_EQ(std::memcmp(source.sha256, expected_sha256,
      sizeof(expected_sha256)), 0);
  GTEXT_Markup_Document *clone_doc = nullptr;
  ASSERT_EQ(gtext_markup_document_new(nullptr, &clone_doc), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node *clone_root = nullptr;
  ASSERT_EQ(gtext_markup_clone(clone_doc,
      gtext_markup_document_doctype(parsed.doc), true, &clone_root), GTEXT_MARKUP_OK);
  EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_doctype(parsed.doc), clone_root));
  GTEXT_Markup_Node *clone_doctype = clone_root;
  ASSERT_GT(gtext_markup_doctype_external_syntax_count(clone_doctype), 0u);
  GTEXT_Markup_DTD_Syntax cloned_external{};
  ASSERT_TRUE(gtext_markup_doctype_external_syntax(clone_doctype, 0,
      &cloned_external));
  GTEXT_Markup_Source cloned_source{};
  ASSERT_TRUE(gtext_markup_document_source(clone_doc,
      cloned_external.location.source_id, &cloned_source));
  EXPECT_STREQ(cloned_source.base_uri, source.base_uri);
  EXPECT_STREQ(cloned_source.encoding, source.encoding);
  EXPECT_EQ(cloned_source.byte_count, source.byte_count);
  EXPECT_EQ(cloned_source.has_sha256, source.has_sha256);
  EXPECT_EQ(cloned_source.has_resolver_identity_token,
      source.has_resolver_identity_token);
  EXPECT_EQ(cloned_source.resolver_identity_token,
      source.resolver_identity_token);
  EXPECT_EQ(std::memcmp(cloned_source.sha256, source.sha256,
      sizeof(source.sha256)), 0);
  gtext_markup_document_free(clone_doc);
}

TEST(XmlValidation, ExplicitRefreshFailuresPreserveSnapshotAndCloseSources) {
  const std::string input = "<!DOCTYPE root SYSTEM 'urn:validation' ["
      "<!ENTITY % decl '<!ATTLIST root mode (old|new) #IMPLIED>'>]><root mode='old'/>";
  auto make_stale = [&](ExternalDtd &dtd,
      const GTEXT_Allocator *allocator = nullptr) {
    dtd.bytes = "<!ELEMENT root EMPTY>%decl;";
    auto options = gtext_xml_parse_options_default();
    options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
    options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
    options.resolver = open_external;
    options.resolver_user_data = &dtd;
    options.allocator = allocator;
    Parsed parsed;
    parsed.status = gtext_xml_parse(input.data(), input.size(), &options,
        &parsed.doc, &parsed.error);
    if (parsed.status != GTEXT_XML_OK) return parsed;
    auto *doctype = gtext_markup_document_doctype(parsed.doc);
    size_t index = SIZE_MAX;
    for (size_t i = 0; i < gtext_markup_doctype_syntax_count(doctype); ++i) {
      GTEXT_Markup_DTD_Syntax syntax{};
      if (gtext_markup_doctype_syntax(doctype, i, &syntax)
          && syntax.kind == GTEXT_MARKUP_DTD_ENTITY
          && std::string(syntax.source, syntax.source_length).find("% decl")
              != std::string::npos) { index = i; break; }
    }
    if (index == SIZE_MAX) {
      parsed.status = GTEXT_XML_E_STATE;
      return parsed;
    }
    const char changed[] = "<!ENTITY % decl '<!ATTLIST root mode (new) #IMPLIED>'>";
    if (gtext_markup_doctype_replace_syntax(doctype, index,
            GTEXT_MARKUP_DTD_ENTITY, changed, sizeof(changed) - 1, nullptr)
        != GTEXT_MARKUP_OK) parsed.status = GTEXT_XML_E_STATE;
    dtd.opens = dtd.closes = 0;
    return parsed;
  };
  auto assert_unchanged = [&](Parsed &parsed, ExternalDtd &dtd,
      GTEXT_XML_External_DTD_Refresh_Options refresh, bool opened,
      GTEXT_XML_Status expected = GTEXT_XML_E_STATE) {
    ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
    GTEXT_Markup_Document *snapshot = nullptr;
    ASSERT_EQ(gtext_markup_document_new(nullptr, &snapshot), GTEXT_MARKUP_OK);
    GTEXT_Markup_Node *copy = nullptr;
    ASSERT_EQ(gtext_markup_clone(snapshot,
        gtext_markup_document_doctype(parsed.doc), true, &copy), GTEXT_MARKUP_OK);
    GTEXT_Markup_Node *element_copy = nullptr;
    ASSERT_EQ(gtext_markup_clone(snapshot,
        gtext_markup_document_element(parsed.doc), true, &element_copy),
        GTEXT_MARKUP_OK);
    GTEXT_XML_Validation_Result result{};
    GTEXT_XML_Error error{};
    const GTEXT_XML_Status refresh_status = gtext_xml_refresh_external_dtd(
        parsed.doc, &refresh, &result, &error);
    if (expected != GTEXT_XML_E_STATE) EXPECT_EQ(refresh_status, expected);
    else EXPECT_NE(refresh_status, GTEXT_XML_OK);
    if (expected == GTEXT_XML_E_UNAVAILABLE_RESOURCE) {
      EXPECT_EQ(error.code, GTEXT_XML_E_UNAVAILABLE_RESOURCE);
      EXPECT_STREQ(error.message,
          "external DTD evidence is incomplete after refresh");
    }
    EXPECT_TRUE(gtext_markup_document_dtd_knowledge_incomplete(parsed.doc));
    EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_doctype(parsed.doc), copy));
    EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_element(parsed.doc),
        element_copy));
    EXPECT_EQ(dtd.closes, opened && dtd.opens ? 1u : 0u);
    gtext_markup_document_free(snapshot);
  };

  {
    ExternalDtd dtd;
    Parsed parsed = make_stale(dtd);
    dtd.open_result = GTEXT_XML_RESOLVER_DECLINED;
    GTEXT_XML_External_DTD_Refresh_Options refresh{};
    refresh.resolver = open_external;
    refresh.resolver_user_data = &dtd;
    assert_unchanged(parsed, dtd, refresh, false,
        GTEXT_XML_E_UNAVAILABLE_RESOURCE);
  }
  {
    ExternalDtd dtd;
    Parsed parsed = make_stale(dtd);
    dtd.open_result = GTEXT_XML_RESOLVER_FAILED;
    GTEXT_XML_External_DTD_Refresh_Options refresh{};
    refresh.resolver = open_external;
    refresh.resolver_user_data = &dtd;
    assert_unchanged(parsed, dtd, refresh, false, GTEXT_XML_E_RESOLVER);
  }
  {
    ExternalDtd dtd;
    Parsed parsed = make_stale(dtd);
    dtd.bytes = "<!ELEMENT root (";
    GTEXT_XML_External_DTD_Refresh_Options refresh{};
    refresh.resolver = open_external;
    refresh.resolver_user_data = &dtd;
    assert_unchanged(parsed, dtd, refresh, true);
  }
  {
    ExternalDtd dtd;
    Parsed parsed = make_stale(dtd);
    dtd.read_result = GTEXT_XML_SOURCE_CANCELLED;
    GTEXT_XML_External_DTD_Refresh_Options refresh{};
    refresh.resolver = open_external;
    refresh.resolver_user_data = &dtd;
    assert_unchanged(parsed, dtd, refresh, true);
  }

  using LimitSetter = void (*)(GTEXT_XML_External_DTD_Refresh_Options &);
  const std::vector<LimitSetter> limits = {
    [](auto &o) { o.max_external_source_bytes = 1; },
    [](auto &o) { o.max_external_bytes = 1; },
    [](auto &o) { o.max_declarations = 1; },
    [](auto &o) { o.max_total_work = 1; },
    [](auto &o) { o.max_validation_work = 1; },
    [](auto &o) { o.max_validation_memory = 1; }
  };
  for (size_t limit_index = 0; limit_index < limits.size(); ++limit_index) {
    SCOPED_TRACE(limit_index);
    ExternalDtd dtd;
    Parsed parsed = make_stale(dtd);
    GTEXT_XML_External_DTD_Refresh_Options refresh{};
    refresh.resolver = open_external;
    refresh.resolver_user_data = &dtd;
    limits[limit_index](refresh);
    assert_unchanged(parsed, dtd, refresh, true);
  }

  for (bool depth_limit : {true, false}) {
    SCOPED_TRACE(depth_limit ? "nested depth" : "nested references");
    ExternalPair pair;
    pair.subset.bytes = "<!ENTITY % inner SYSTEM 'urn:inner'>%inner;";
    pair.entity.bytes = "<!ENTITY % leaf SYSTEM 'urn:leaf'>%leaf;";
    pair.leaf.bytes = "<!ELEMENT root EMPTY>";
    const std::string nested_input = "<!DOCTYPE root SYSTEM 'urn:validation' ["
        "<!ATTLIST root mode CDATA #IMPLIED>]><root/>";
    auto parse_options = gtext_xml_parse_options_default();
    parse_options.dtd_mode = GTEXT_XML_DTD_VALIDATE;
    parse_options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
    parse_options.resolver = open_external_pair;
    parse_options.resolver_user_data = &pair;
    Parsed parsed;
    parsed.status = gtext_xml_parse(nested_input.data(), nested_input.size(),
        &parse_options, &parsed.doc, &parsed.error);
    ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
    auto *doctype = gtext_markup_document_doctype(parsed.doc);
    const char replacement[] = "<!ATTLIST root mode (old|new) #IMPLIED>";
    ASSERT_EQ(gtext_markup_doctype_replace_syntax(doctype, 0,
        GTEXT_MARKUP_DTD_ATTLIST, replacement, sizeof(replacement) - 1,
        nullptr), GTEXT_MARKUP_OK);
    pair.opens = pair.closes = 0;
    pair.subset.closes = pair.entity.closes = pair.leaf.closes = 0;
    GTEXT_Markup_Document *snapshot = nullptr;
    ASSERT_EQ(gtext_markup_document_new(nullptr, &snapshot), GTEXT_MARKUP_OK);
    GTEXT_Markup_Node *copy = nullptr;
    ASSERT_EQ(gtext_markup_clone(snapshot, doctype, true, &copy),
        GTEXT_MARKUP_OK);
    GTEXT_Markup_Node *element_copy = nullptr;
    ASSERT_EQ(gtext_markup_clone(snapshot,
        gtext_markup_document_element(parsed.doc), true, &element_copy),
        GTEXT_MARKUP_OK);
    GTEXT_XML_External_DTD_Refresh_Options refresh{};
    refresh.resolver = open_external_pair;
    refresh.resolver_user_data = &pair;
    const auto expected = depth_limit ? GTEXT_XML_E_LIMIT_ENTITY_DEPTH
                                     : GTEXT_XML_E_LIMIT_REFERENCES;
    if (depth_limit) refresh.max_entity_depth = 1;
    else refresh.max_references = 1;
    GTEXT_XML_Validation_Result result{};
    GTEXT_XML_Error error{};
    EXPECT_EQ(gtext_xml_refresh_external_dtd(parsed.doc, &refresh, &result,
        &error), expected) << error.message;
    EXPECT_TRUE(gtext_markup_document_dtd_knowledge_incomplete(parsed.doc));
    EXPECT_TRUE(gtext_markup_equal(doctype, copy));
    EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_element(parsed.doc),
        element_copy));
    EXPECT_EQ(pair.opens, pair.subset.closes + pair.entity.closes
        + pair.leaf.closes);
    gtext_markup_document_free(snapshot);
  }

  /* Fail the first document allocation after the resolver has opened. The
     refresh must close that source and leave the old DTD evidence intact. */
  {
    LiveAllocator allocator;
    {
    ExternalDtd dtd;
    Parsed parsed = make_stale(dtd, &allocator.allocator);
    ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
    GTEXT_Markup_Document *snapshot = nullptr;
    ASSERT_EQ(gtext_markup_document_new(nullptr, &snapshot), GTEXT_MARKUP_OK);
    GTEXT_Markup_Node *snapshot_doctype = nullptr;
    ASSERT_EQ(gtext_markup_clone(snapshot,
        gtext_markup_document_doctype(parsed.doc), true, &snapshot_doctype),
        GTEXT_MARKUP_OK);
    GTEXT_Markup_Node *snapshot_element = nullptr;
    ASSERT_EQ(gtext_markup_clone(snapshot,
        gtext_markup_document_element(parsed.doc), true, &snapshot_element),
        GTEXT_MARKUP_OK);
    const size_t blocks_before = allocator.blocks;
    dtd.fail_allocator = &allocator;
    GTEXT_XML_External_DTD_Refresh_Options refresh{};
    refresh.resolver = open_external;
    refresh.resolver_user_data = &dtd;
    GTEXT_XML_Validation_Result result{};
    GTEXT_XML_Error error{};
    EXPECT_EQ(gtext_xml_refresh_external_dtd(parsed.doc, &refresh, &result,
        &error), GTEXT_XML_E_OOM);
    EXPECT_EQ(dtd.opens, 1u);
    EXPECT_EQ(dtd.closes, 1u);
    EXPECT_TRUE(gtext_markup_document_dtd_knowledge_incomplete(parsed.doc));
    EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_doctype(parsed.doc),
        snapshot_doctype));
    EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_element(parsed.doc),
        snapshot_element));
    EXPECT_EQ(allocator.blocks, blocks_before);
    dtd.fail_allocator = nullptr;
    gtext_markup_document_free(snapshot);
    /* The parsed document still owns its original allocations until this
       nested scope ends. */
    (void)parsed;
    }
  }
  {
    LiveAllocator allocator;
    ExternalDtd dtd;
    Parsed parsed = make_stale(dtd, &allocator.allocator);
    ASSERT_EQ(parsed.status, GTEXT_XML_OK) << parsed.error.message;
    GTEXT_Markup_Document *snapshot = nullptr;
    ASSERT_EQ(gtext_markup_document_new(nullptr, &snapshot), GTEXT_MARKUP_OK);
    GTEXT_Markup_Node *snapshot_element = nullptr;
    ASSERT_EQ(gtext_markup_clone(snapshot,
        gtext_markup_document_element(parsed.doc), true, &snapshot_element),
        GTEXT_MARKUP_OK);
    GTEXT_Markup_Node *snapshot_doctype = nullptr;
    ASSERT_EQ(gtext_markup_clone(snapshot,
        gtext_markup_document_doctype(parsed.doc), true, &snapshot_doctype),
        GTEXT_MARKUP_OK);
    const size_t blocks_before = allocator.blocks;
    allocator.fail_after = allocator.allocation_calls;
    GTEXT_XML_External_DTD_Refresh_Options refresh{};
    refresh.resolver = open_external;
    refresh.resolver_user_data = &dtd;
    GTEXT_XML_Validation_Result result{};
    GTEXT_XML_Error error{};
    EXPECT_EQ(gtext_xml_refresh_external_dtd(parsed.doc, &refresh, &result,
        &error), GTEXT_XML_E_OOM);
    EXPECT_EQ(dtd.opens, 0u);
    EXPECT_EQ(dtd.closes, 0u);
    EXPECT_TRUE(gtext_markup_document_dtd_knowledge_incomplete(parsed.doc));
    EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_element(parsed.doc),
        snapshot_element));
    EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_doctype(parsed.doc),
        snapshot_doctype));
    EXPECT_EQ(allocator.blocks, blocks_before);
    allocator.fail_after = SIZE_MAX;
    gtext_markup_document_free(snapshot);
  }
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

// ---- validation state across a retry -------------------------------------------------------------------

// What a validating parse said: its status, and (when it parsed) the verdict and every finding in order.
struct ValidationAnswer {
  GTEXT_XML_Status status = GTEXT_XML_OK;
  size_t error_offset = 0;
  GTEXT_XML_Validation_Result result{};
  std::vector<std::string> findings;
  bool operator==(const ValidationAnswer & o) const {
    return status == o.status && error_offset == o.error_offset && result.verdict == o.result.verdict
        && result.knowledge_complete == o.result.knowledge_complete
        && result.diagnostics_truncated == o.result.diagnostics_truncated
        && result.diagnostic_count == o.result.diagnostic_count && findings == o.findings;
  }
};

std::ostream & operator<<(std::ostream & os, const ValidationAnswer & a) {
  os << gtext_xml_status_string(a.status) << " at " << a.error_offset << ", verdict " << a.result.verdict << " complete "
     << a.result.knowledge_complete << " count " << a.result.diagnostic_count << "\n";
  for (const std::string & f : a.findings) os << "    " << f << "\n";
  return os;
}

std::string finding_text(const GTEXT_XML_Validation_Diagnostic & d) {
  return std::to_string(d.code) + " @" + std::to_string(d.location.offset) + ":" + std::to_string(d.location.line) + ":"
      + std::to_string(d.location.column) + " src" + std::to_string(d.location.source_id) + " " + (d.message ? d.message : "");
}

GTEXT_XML_Parse_Options validating() {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  return o;
}

ValidationAnswer validation_dom(const std::string & text, const GTEXT_XML_Parse_Options & o) {
  ValidationAnswer a;
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_XML_Error err{};
  a.status = gtext_xml_parse(text.data(), text.size(), &o, &doc, &err);
  if (a.status != GTEXT_XML_OK) {
    a.error_offset = err.byte_offset;
    return a;
  }
  gtext_xml_validation_result(doc, &a.result);
  for (size_t i = 0; i < gtext_xml_validation_diagnostic_count(doc); ++i) {
    GTEXT_XML_Validation_Diagnostic d{};
    EXPECT_TRUE(gtext_xml_validation_diagnostic(doc, i, &d));
    a.findings.push_back(finding_text(d));
  }
  gtext_markup_document_free(doc);
  return a;
}

void take_validation_event(const GTEXT_XML_Event * e, ValidationAnswer * a) {
  if (e->type == GTEXT_XML_EVENT_VALIDATION_DIAGNOSTIC) a->findings.push_back(finding_text(e->validation_diagnostic));
  else if (e->type == GTEXT_XML_EVENT_VALIDATION_RESULT) a->result = e->validation_result;
}

ValidationAnswer validation_stream(const std::string & text, const GTEXT_XML_Parse_Options & o, size_t chunk, bool eager) {
  ValidationAnswer a;
  GTEXT_XML_Stream * s = gtext_xml_stream_new(&o,
      [](const GTEXT_XML_Event * e, void * user) {
        take_validation_event(e, static_cast<ValidationAnswer *>(user));
        return GTEXT_XML_OK;
      },
      &a);
  xml_stream_set_eager(s, eager);
  GTEXT_XML_Error err{};
  for (size_t at = 0; at < text.size() && a.status == GTEXT_XML_OK;) {
    const size_t n = std::min(chunk ? chunk : text.size(), text.size() - at);
    a.status = gtext_xml_stream_feed(s, text.data() + at, n, &err);
    at += n;
  }
  if (a.status == GTEXT_XML_OK) a.status = gtext_xml_stream_finish(s, &err);
  if (a.status != GTEXT_XML_OK) {
    a.error_offset = err.byte_offset;
    a.findings.clear();
    a.result = {};
  }
  gtext_xml_stream_free(s);
  return a;
}

ValidationAnswer validation_reader(const std::string & text, const GTEXT_XML_Parse_Options & o, size_t chunk) {
  ValidationAnswer a;
  GTEXT_XML_Reader * r = gtext_xml_reader_new(&o);
  GTEXT_XML_Error err{};
  auto drain = [&]() {
    GTEXT_XML_Event e;
    while (gtext_xml_reader_next(r, &e) == GTEXT_XML_OK) take_validation_event(&e, &a);
  };
  for (size_t at = 0; at < text.size() && a.status == GTEXT_XML_OK;) {
    const size_t n = std::min(chunk ? chunk : text.size(), text.size() - at);
    a.status = gtext_xml_reader_feed(r, text.data() + at, n, &err);
    at += n;
    drain();
  }
  if (a.status == GTEXT_XML_OK) {
    a.status = gtext_xml_reader_feed(r, nullptr, 0, &err);
    drain();
  }
  if (a.status != GTEXT_XML_OK) {
    a.error_offset = err.byte_offset;
    a.findings.clear();
    a.result = {};
  }
  gtext_xml_reader_free(r);
  return a;
}

void expect_one_validation(const char * what, const std::string & text, const GTEXT_XML_Parse_Options & o) {
  const ValidationAnswer dom = validation_dom(text, o);
  struct Way { const char * name; size_t chunk; bool eager; };
  for (const Way & w : {Way{"whole", 0, false}, Way{"whole, eager", 0, true}, Way{"a byte at a time", 1, false},
           Way{"a byte at a time, eager", 1, true}, Way{"three bytes at a time, eager", 3, true}}) {
    EXPECT_EQ(validation_stream(text, o, w.chunk, w.eager), dom) << what << ": stream, " << w.name << "\n dom: " << dom;
  }
  EXPECT_EQ(validation_reader(text, o, 0), dom) << what << ": reader, whole\n dom: " << dom;
  EXPECT_EQ(validation_reader(text, o, 1), dom) << what << ": reader, a byte at a time\n dom: " << dom;
}

// The event parser reads a construct again when its first reading found it unfinished, and the document type is one construct from
// its "<!DOCTYPE" to its closing bracket. A declaration read twice must not be declared twice: the element-declaration records,
// the entity, notation and attribute-list tables, the ID tables and the findings must be the DOM parse's, in the same order and at
// the same places, with an invalid duplicate element declaration reported once.
TEST(XmlValidationRetry, AReadAgainSubsetValidatesAsTheDomParseDoes) {
  const std::string subset =
      "<!DOCTYPE r [\n"
      "<!ELEMENT r (a|b)*>\n"
      "<!ELEMENT a (#PCDATA)>\n"
      "<!ELEMENT a ANY>\n"          // genuinely invalid: a second declaration of a
      "<!ELEMENT b EMPTY>\n"
      "<!ATTLIST a id ID #IMPLIED ref IDREF #IMPLIED>\n"
      "<!ATTLIST b n NMTOKEN \"x\" k CDATA #REQUIRED>\n"
      "<!ENTITY e \"txt\">\n"
      "<!ENTITY u SYSTEM \"u.bin\" NDATA nt>\n"
      "<!NOTATION nt SYSTEM \"x\">\n"
      "<!ENTITY % p \"<!-- c -->\">\n"
      "%p;\n"
      "]>\n";
  const std::string body = "<r><a id=\"i1\">&e;</a><a ref=\"i1\"/><b/><a ref=\"missing\"/><a id=\"i1\"/><c/></r>";
  GTEXT_XML_Parse_Options o = validating();
  const ValidationAnswer dom = validation_dom(subset + body, o);
  ASSERT_EQ(dom.status, GTEXT_XML_OK);
  ASSERT_EQ(dom.result.verdict, GTEXT_XML_VALIDATION_INVALID);
  // The duplicate declaration, the missing required attribute, the dangling IDREF, the repeated ID and the undeclared element.
  EXPECT_GE(dom.findings.size(), 5u) << dom;
  expect_one_validation("invalid document", subset + body, o);

  // The same subset, with the duplicate removed and the uses made valid: valid, and a retry does not make it otherwise.
  std::string clean = subset;
  clean.erase(clean.find("<!ELEMENT a ANY>\n"), std::string("<!ELEMENT a ANY>\n").size());
  const std::string valid_body = "<r><a id=\"i1\">&e;</a><a ref=\"i1\"/><b k=\"v\"/></r>";
  const ValidationAnswer valid = validation_dom(clean + valid_body, o);
  EXPECT_EQ(valid.result.verdict, GTEXT_XML_VALIDATION_VALID) << valid;
  expect_one_validation("valid document", clean + valid_body, o);

  // A diagnostic cap reached inside the subset's findings, and a duplicate that the cap would hide: counted once each.
  GTEXT_XML_Parse_Options capped = o;
  capped.max_validation_diagnostics = 2;
  expect_one_validation("capped", subset + body, capped);

  // The subset cut off before it closes: the first failure is the end of the input, whatever validation did on the way.
  expect_one_validation("unterminated", subset.substr(0, subset.size() - 3), o);
  // And a subset whose duplicate is the last thing before a syntax error.
  expect_one_validation("duplicate then a syntax error",
      "<!DOCTYPE r [<!ELEMENT r ANY><!ELEMENT r EMPTY><!ELEMENT r ( ]><r/>", o);
}


// The budgets of validation count the document's DTD, and a subset read again must not be charged twice. The reference here is the
// event parser fed the whole input, and not the DOM parse: the DOM parse also rebuilds its indexes from the retained syntax after the
// subset, which the event parser has no tree for, so its least budget is a little higher (a difference of the two parsers' accounting
// that has nothing to do with reading again). Every cut of the same stream parser, eager or not, and the reader, must accept the least
// budget the whole-input stream accepts, and refuse one less with the same answer.
TEST(XmlValidationRetry, ABudgetThatAcceptsTheWholeInputAcceptsEveryCut) {
  const std::string text =
      "<!DOCTYPE r [<!ELEMENT r (a|b)*><!ELEMENT a (#PCDATA)><!ELEMENT a ANY><!ELEMENT b EMPTY>"
      "<!ATTLIST a id ID #IMPLIED ref IDREF #IMPLIED><!ENTITY e \"txt\"><!NOTATION nt SYSTEM \"x\">]>"
      "<r><a id=\"i1\">&e;</a><a ref=\"i1\"/><b/></r>";
  using Field = size_t GTEXT_XML_Parse_Options::*;
  for (Field field : {&GTEXT_XML_Parse_Options::max_validation_work, &GTEXT_XML_Parse_Options::max_validation_memory}) {
    GTEXT_XML_Parse_Options o = validating();
    size_t low = 1, high = (size_t)1 << 24;
    while (low < high) {
      const size_t mid = low + (high - low) / 2;
      o.*field = mid;
      if (validation_stream(text, o, 0, false).status == GTEXT_XML_OK) high = mid; else low = mid + 1;
    }
    ASSERT_LT(low, (size_t)1 << 24);
    for (const size_t budget : {low, low - 1}) {
      o.*field = budget;
      const ValidationAnswer whole = validation_stream(text, o, 0, false);
      ASSERT_EQ(whole.status == GTEXT_XML_OK, budget == low) << budget;
      struct Way { const char * name; size_t chunk; bool eager; };
      for (const Way & w : {Way{"whole, eager", 0, true}, Way{"a byte at a time", 1, false}, Way{"a byte at a time, eager", 1, true},
               Way{"three bytes at a time, eager", 3, true}}) {
        EXPECT_EQ(validation_stream(text, o, w.chunk, w.eager), whole) << "budget " << budget << ", " << w.name;
      }
      EXPECT_EQ(validation_reader(text, o, 0), whole) << "budget " << budget << ", reader whole";
      EXPECT_EQ(validation_reader(text, o, 1), whole) << "budget " << budget << ", reader a byte at a time";
    }
  }
}

} // namespace
