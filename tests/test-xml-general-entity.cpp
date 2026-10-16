/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 */

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>
#include <ghoti.io/text/xml/xml_stream.h>

namespace {

using Node = GTEXT_Markup_Node;

struct Events {
  std::vector<std::string> values;
};

struct ExternalEntity {
  std::string bytes;
  size_t offset = 0;
  size_t closes = 0;
  int identity = 0;
  std::string requested_base_uri;
  GTEXT_XML_Resource_Kind kind = GTEXT_XML_RESOURCE_GENERAL_ENTITY;
};

static GTEXT_XML_Source_Read_Result read_entity(void * handle,
    unsigned char * buffer, size_t capacity, size_t * count) {
  auto * source = static_cast<ExternalEntity *>(handle);
  const size_t amount = std::min(capacity, source->bytes.size() - source->offset);
  if (amount == 0) { *count = 0; return GTEXT_XML_SOURCE_EOF; }
  std::memcpy(buffer, source->bytes.data() + source->offset, amount);
  source->offset += amount;
  *count = amount;
  return GTEXT_XML_SOURCE_DATA;
}

static void close_entity(void * handle) {
  ++static_cast<ExternalEntity *>(handle)->closes;
}

static GTEXT_XML_Resolver_Result resolve_entity(
    const GTEXT_XML_Resolve_Request * request, void * user,
    GTEXT_XML_External_Source * out) {
  auto * source = static_cast<ExternalEntity *>(user);
  if (request->kind != source->kind)
    return GTEXT_XML_RESOLVER_DECLINED;
  source->requested_base_uri = request->base_uri ? request->base_uri : "";
  *out = {source, &source->identity, "memory:entity", read_entity, close_entity};
  return GTEXT_XML_RESOLVER_OPENED;
}

static GTEXT_XML_Status collect(const GTEXT_XML_Event * event, void * user) {
  auto * events = static_cast<Events *>(user);
  if (event->type == GTEXT_XML_EVENT_TEXT)
    events->values.emplace_back("text:" + std::string(event->data, event->data_length));
  else if (event->type == GTEXT_XML_EVENT_ELEMENT_START)
    events->values.emplace_back("start:" + std::string(event->local_name));
  else if (event->type == GTEXT_XML_EVENT_ELEMENT_END)
    events->values.emplace_back("end:" + std::string(event->local_name));
  return GTEXT_XML_OK;
}

TEST(XmlGeneralEntity, ReplacementMarkupUsesTheSharedParserAndEvents) {
  const std::string input =
      "<!DOCTYPE root [<!ENTITY inner \"<child>ok</child>\">"
      "<!ENTITY outer \"a&inner;b\">]><root>&outer;</root>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_Markup_Document * document = nullptr;
  GTEXT_XML_Error error{};
  ASSERT_EQ(gtext_xml_parse(input.data(), input.size(), &options,
      &document, &error), GTEXT_XML_OK) << error.message;
  ASSERT_NE(document, nullptr);
  Node * root = gtext_markup_document_element(document);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(gtext_markup_node_child_count(root), 3u);
  Node * before = gtext_markup_node_first_child(root);
  size_t length = 0;
  const char * data = gtext_markup_node_data(before, &length);
  EXPECT_EQ(std::string(data, length), "a");
  Node * child = gtext_markup_node_next_sibling(before);
  ASSERT_EQ(gtext_markup_node_kind(child), GTEXT_MARKUP_KIND_ELEMENT);
  EXPECT_STREQ(gtext_markup_element_local_name(child), "child");
  Node * inside = gtext_markup_node_first_child(child);
  data = gtext_markup_node_data(inside, &length);
  EXPECT_EQ(std::string(data, length), "ok");
  Node * after = gtext_markup_node_next_sibling(child);
  data = gtext_markup_node_data(after, &length);
  EXPECT_EQ(std::string(data, length), "b");

  Events events;
  GTEXT_XML_Stream * stream = gtext_xml_stream_new(&options, collect, &events);
  ASSERT_NE(stream, nullptr);
  for (char byte : input) {
    const GTEXT_XML_Status status = gtext_xml_stream_feed(stream, &byte, 1, &error);
    ASSERT_EQ(status, GTEXT_XML_OK) << error.message;
  }
  ASSERT_EQ(gtext_xml_stream_finish(stream, &error), GTEXT_XML_OK) << error.message;
  EXPECT_EQ(events.values, (std::vector<std::string>{
      "start:root", "text:a", "start:child", "text:ok", "end:child", "text:b", "end:root"}));
  gtext_xml_stream_free(stream);
  gtext_markup_document_free(document);
}

TEST(XmlGeneralEntity, AttributeExpansionKeepsCharacterReferenceWhitespace) {
  const std::string input =
      "<!DOCTYPE root [<!ENTITY tab \"A&#x9;B\">"
      "<!ENTITY value \"  &tab;  \">"
      "<!ENTITY lines \"a\r\nb\">]><root a='&value;' line='&lines;'/>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_Markup_Document * document = nullptr;
  GTEXT_XML_Error error{};
  ASSERT_EQ(gtext_xml_parse(input.data(), input.size(), &options,
      &document, &error), GTEXT_XML_OK) << error.message;
  GTEXT_Markup_Attribute attribute{};
  ASSERT_TRUE(gtext_markup_element_get_attribute(
      gtext_markup_document_element(document), nullptr, "a", &attribute));
  EXPECT_EQ(std::string(attribute.value, attribute.value_length), "  A\tB  ");
  ASSERT_TRUE(gtext_markup_element_get_attribute(
      gtext_markup_document_element(document), nullptr, "line", &attribute));
  EXPECT_EQ(std::string(attribute.value, attribute.value_length), "a b");
  gtext_markup_document_free(document);
}

TEST(XmlGeneralEntity, CallerResolvedExternalReplacementUsesTheSharedParser) {
  ExternalEntity source;
  source.bytes = "<?xml version='1.0' encoding='UTF-8'?><child/>";
  const std::string input =
      "<!DOCTYPE root [<!ENTITY ext SYSTEM 'memory:entity'>]>"
      "<root>&ext;</root>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = resolve_entity;
  options.resolver_user_data = &source;
  options.base_uri = "memory:document";
  GTEXT_Markup_Document * document = nullptr;
  GTEXT_XML_Error error{};
  ASSERT_EQ(gtext_xml_parse(input.data(), input.size(), &options,
      &document, &error), GTEXT_XML_OK) << error.message;
  ASSERT_NE(document, nullptr);
  ASSERT_EQ(source.closes, 1u);
  EXPECT_EQ(source.requested_base_uri, "memory:document");
  Node * child = gtext_markup_node_first_child(
      gtext_markup_document_element(document));
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(gtext_markup_node_kind(child), GTEXT_MARKUP_KIND_ELEMENT);
  EXPECT_STREQ(gtext_markup_element_local_name(child), "child");
  GTEXT_Markup_Location location{};
  ASSERT_TRUE(gtext_markup_node_location(child, &location));
  EXPECT_GT(location.source_id, 1u);
  gtext_markup_document_free(document);
}

TEST(XmlGeneralEntity, ExternalReplacementBodyCountsAgainstExpansionBudget) {
  ExternalEntity source;
  source.bytes = "<?xml version='1.0' encoding='UTF-8'?><child/>";
  const std::string input =
      "<!DOCTYPE root [<!ENTITY ext SYSTEM 'memory:entity'>]>"
      "<root>&ext;</root>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = resolve_entity;
  options.resolver_user_data = &source;
  options.max_expanded_bytes = 3;
  GTEXT_Markup_Document * document = nullptr;
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_parse(input.data(), input.size(), &options,
      &document, &error), GTEXT_XML_E_LIMIT_EXPANSION) << error.message;
  EXPECT_EQ(document, nullptr);
  EXPECT_EQ(source.closes, 1u);
}

TEST(XmlGeneralEntity, RestrictedCharacterReferenceFollowsDocumentVersion) {
  const std::string xml10 =
      "<?xml version='1.0'?><!DOCTYPE root [<!ENTITY control '&#x1;'>]>"
      "<root>&control;</root>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_Markup_Document * document = nullptr;
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_parse(xml10.data(), xml10.size(), &options,
      &document, &error), GTEXT_XML_E_CHARACTER);
  EXPECT_EQ(document, nullptr);

  const std::string xml11 =
      "<?xml version='1.1'?><!DOCTYPE root [<!ENTITY control '&#x1;'>]>"
      "<root>&control;</root>";
  ASSERT_EQ(gtext_xml_parse(xml11.data(), xml11.size(), &options,
      &document, &error), GTEXT_XML_OK) << error.message;
  ASSERT_NE(document, nullptr);
  size_t length = 0;
  const char * text = gtext_markup_node_data(gtext_markup_node_first_child(
      gtext_markup_document_element(document)), &length);
  ASSERT_NE(text, nullptr);
  ASSERT_EQ(length, 1u);
  EXPECT_EQ(static_cast<unsigned char>(text[0]), 1u);
  gtext_markup_document_free(document);
}

TEST(XmlGeneralEntity, ExternalParameterEntityDeclarationWorksInPushStream) {
  ExternalEntity source;
  source.bytes = "<!ENTITY replacement 'streamed'>";
  source.kind = GTEXT_XML_RESOURCE_PARAMETER_ENTITY;
  const std::string input =
      "<!DOCTYPE root [<!ENTITY % declarations SYSTEM 'memory:declarations'>"
      "%declarations;]><root>&replacement;</root>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = resolve_entity;
  options.resolver_user_data = &source;
  Events events;
  GTEXT_XML_Stream * stream = gtext_xml_stream_new(&options, collect, &events);
  ASSERT_NE(stream, nullptr);
  GTEXT_XML_Error error{};
  for (char byte : input) {
    const GTEXT_XML_Status status = gtext_xml_stream_feed(
        stream, &byte, 1, &error);
    ASSERT_EQ(status, GTEXT_XML_OK) << error.message;
  }
  ASSERT_EQ(gtext_xml_stream_finish(stream, &error), GTEXT_XML_OK)
      << error.message;
  EXPECT_EQ(events.values, (std::vector<std::string>{
      "start:root", "text:streamed", "end:root"}));
  EXPECT_EQ(source.closes, 1u);
  gtext_xml_stream_free(stream);
}

TEST(XmlGeneralEntity, RecursionUnparsedUseAndAttributeMarkupFailWithoutTree) {
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  const std::vector<std::pair<std::string, GTEXT_XML_Status>> invalid = {
      {"<!DOCTYPE root [<!ENTITY loop '&loop;'>]><root>&loop;</root>",
          GTEXT_XML_E_ENTITY},
      {"<!DOCTYPE root [<!ENTITY loop '&loop;'>]><root a='&loop;'/>",
          GTEXT_XML_E_ENTITY},
      {"<!DOCTYPE root [<!NOTATION n SYSTEM 'n'>"
          "<!ENTITY image SYSTEM 'image' NDATA n>]><root>&image;</root>",
          GTEXT_XML_E_ENTITY},
      {"<!DOCTYPE root [<!ENTITY markup '<child/>'>]><root a='&markup;'/>",
          GTEXT_XML_E_SYNTAX},
  };
  for (const auto & [input, expected] : invalid) {
    GTEXT_Markup_Document * document = nullptr;
    GTEXT_XML_Error error{};
    EXPECT_EQ(gtext_xml_parse(input.data(), input.size(), &options,
        &document, &error), expected) << error.message;
    EXPECT_EQ(document, nullptr);
  }
}

TEST(XmlGeneralEntity, ExpansionDepthAndByteBudgetsFailBeforePublishingTree) {
  const std::string depth_input =
      "<!DOCTYPE root [<!ENTITY first '&second;'>"
      "<!ENTITY second 'x'>]><root a='&first;'/>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  options.max_entity_depth = 1;
  GTEXT_Markup_Document * document = nullptr;
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_parse(depth_input.data(), depth_input.size(), &options,
      &document, &error), GTEXT_XML_E_LIMIT_ENTITY_DEPTH);
  EXPECT_EQ(document, nullptr);

  const std::string bytes_input =
      "<!DOCTYPE root [<!ENTITY big '<child/>'>]><root>&big;</root>";
  options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  options.max_expanded_bytes = 2;
  EXPECT_EQ(gtext_xml_parse(bytes_input.data(), bytes_input.size(), &options,
      &document, &error), GTEXT_XML_E_LIMIT_EXPANSION);
  EXPECT_EQ(document, nullptr);
}

} // namespace
