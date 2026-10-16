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
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>

#include "../src/markup/markup_internal.h"
#include "../src/xml/xml_internal.h"

namespace {

struct MemorySubset {
  std::string bytes;
  size_t at = 0;
  size_t closes = 0;
  int identity = 0;
};

static GTEXT_XML_Source_Read_Result read_subset(void *handle,
    unsigned char *buffer, size_t capacity, size_t *read) {
  auto *source = static_cast<MemorySubset *>(handle);
  const size_t amount = std::min(capacity, source->bytes.size() - source->at);
  if (!amount) { *read = 0; return GTEXT_XML_SOURCE_EOF; }
  std::memcpy(buffer, source->bytes.data() + source->at, amount);
  source->at += amount;
  *read = amount;
  return GTEXT_XML_SOURCE_DATA;
}

static void close_subset(void *handle) {
  ++static_cast<MemorySubset *>(handle)->closes;
}

static GTEXT_XML_Resolver_Result resolve_subset(
    const GTEXT_XML_Resolve_Request *request, void *user,
    GTEXT_XML_External_Source *out) {
  if (request->kind != GTEXT_XML_RESOURCE_EXTERNAL_SUBSET)
    return GTEXT_XML_RESOLVER_DECLINED;
  auto *source = static_cast<MemorySubset *>(user);
  *out = {source, &source->identity, "memory:external.dtd", read_subset, close_subset};
  return GTEXT_XML_RESOLVER_OPENED;
}

static const markup_doctype *doctype(const GTEXT_Markup_Document *doc) {
  return reinterpret_cast<const markup_doctype *>(
      gtext_markup_document_doctype(doc));
}

static const markup_dtd_binding *binding(const markup_doctype *d,
    int kind, const char *element, const char *name) {
  for (size_t i = 0; i < d->binding_count; ++i) {
    const auto &candidate = d->bindings[i];
    if (candidate.kind == kind && candidate.name == std::string(name)
        && ((!element && !candidate.element_name)
            || (element && candidate.element_name
                && candidate.element_name == std::string(element))))
      return &candidate;
  }
  return nullptr;
}

TEST(XmlDtdBinding, FirstEntityAndElementBindingsWinAndSyntaxStaysVisible) {
  const std::string input =
      "<!DOCTYPE root ["
      "<!ENTITY % decl \"<!ENTITY item 'from-pe'>\">%decl;"
      "<!ENTITY item 'shadow'>"
      "<!ELEMENT root EMPTY><!ELEMENT root ANY>"
      "]><root/>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_Markup_Document *doc = nullptr;
  GTEXT_XML_Error error{};
  ASSERT_EQ(gtext_xml_parse(input.data(), input.size(), &options, &doc, &error),
      GTEXT_XML_OK) << error.message;
  ASSERT_NE(doc, nullptr);
  const auto *d = doctype(doc);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->syntax_count, 5u);
  EXPECT_EQ(d->generated_syntax_count, 1u);
  ASSERT_EQ(d->binding_count, 3u);
  const auto *parameter = binding(d, 4, nullptr, "decl");
  ASSERT_NE(parameter, nullptr);
  const auto *entity = binding(d, GTEXT_MARKUP_DTD_ENTITY, nullptr, "item");
  ASSERT_NE(entity, nullptr);
  EXPECT_NE(std::string(entity->source, entity->source_length).find("'from-pe'"),
      std::string::npos);
  GTEXT_Markup_DTD_Syntax generated{};
  ASSERT_TRUE(gtext_markup_doctype_generated_syntax(
      gtext_markup_document_doctype(doc), 0, &generated));
  EXPECT_STREQ(generated.parameter_entity, "decl");
  const auto *element = binding(d, GTEXT_MARKUP_DTD_ELEMENT, nullptr, "root");
  ASSERT_NE(element, nullptr);
  EXPECT_NE(std::string(element->source, element->source_length).find("EMPTY"),
      std::string::npos);
  EXPECT_EQ(markup_check_invariants(doc), nullptr);
  gtext_markup_document_free(doc);
}

TEST(XmlDtdBinding, ExternalBindingsFollowInternalPrecedenceAndAttlistsMerge) {
  MemorySubset external{
      "<!ENTITY item 'external'><!ELEMENT root ANY>"
      "<!ATTLIST root a CDATA 'external-a' b CDATA 'external-b'>"
      "<!NOTATION image SYSTEM 'external-image'>"};
  const std::string input =
      "<!DOCTYPE root SYSTEM 'memory:external.dtd' ["
      "<!ENTITY item 'internal'><!ELEMENT root EMPTY>"
      "<!ATTLIST root b CDATA 'internal-b'>"
      "<!NOTATION image SYSTEM 'internal-image'>"
      "]><root/>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = resolve_subset;
  options.resolver_user_data = &external;
  GTEXT_Markup_Document *doc = nullptr;
  GTEXT_XML_Error error{};
  ASSERT_EQ(gtext_xml_parse(input.data(), input.size(), &options, &doc, &error),
      GTEXT_XML_OK) << error.message;
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(external.closes, 1u);
  const auto *d = doctype(doc);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->syntax_count, 4u);
  EXPECT_EQ(d->external_syntax_count, 4u);
  ASSERT_EQ(d->binding_count, 5u);
  const auto *entity = binding(d, GTEXT_MARKUP_DTD_ENTITY, nullptr, "item");
  ASSERT_NE(entity, nullptr);
  EXPECT_FALSE(entity->external_context);
  EXPECT_NE(std::string(entity->source, entity->source_length).find("'internal'"),
      std::string::npos);
  const auto *element = binding(d, GTEXT_MARKUP_DTD_ELEMENT, nullptr, "root");
  ASSERT_NE(element, nullptr);
  EXPECT_FALSE(element->external_context);
  EXPECT_NE(std::string(element->source, element->source_length).find("EMPTY"),
      std::string::npos);
  const auto *attr_a = binding(d, GTEXT_MARKUP_DTD_ATTLIST, "root", "a");
  const auto *attr_b = binding(d, GTEXT_MARKUP_DTD_ATTLIST, "root", "b");
  ASSERT_NE(attr_a, nullptr);
  ASSERT_NE(attr_b, nullptr);
  EXPECT_TRUE(attr_a->external_context);
  EXPECT_FALSE(attr_b->external_context);
  EXPECT_NE(std::string(attr_b->source, attr_b->source_length).find("'internal-b'"),
      std::string::npos);
  const auto *notation = binding(d, GTEXT_MARKUP_DTD_NOTATION, nullptr, "image");
  ASSERT_NE(notation, nullptr);
  EXPECT_FALSE(notation->external_context);
  EXPECT_NE(std::string(notation->source, notation->source_length).find("internal-image"),
      std::string::npos);
  auto *root = gtext_markup_node_first_child(gtext_markup_document_root(doc));
  while (root && gtext_markup_node_kind(root) != GTEXT_MARKUP_KIND_ELEMENT)
    root = gtext_markup_node_next_sibling(root);
  ASSERT_NE(root, nullptr);
  GTEXT_Markup_Attribute attr{};
  ASSERT_TRUE(gtext_markup_element_get_attribute(root, nullptr, "a", &attr));
  EXPECT_STREQ(attr.value, "external-a");
  EXPECT_EQ(attr.origin, GTEXT_MARKUP_ATTRIBUTE_DEFAULTED);
  ASSERT_TRUE(gtext_markup_element_get_attribute(root, nullptr, "b", &attr));
  EXPECT_STREQ(attr.value, "internal-b");
  EXPECT_EQ(attr.origin, GTEXT_MARKUP_ATTRIBUTE_DEFAULTED);

  auto binding_snapshot = [](const markup_doctype *doctype) {
    std::vector<std::string> snapshot;
    for (size_t i = 0; i < doctype->binding_count; ++i) {
      const auto &item = doctype->bindings[i];
      snapshot.push_back(std::to_string(item.kind) + "|"
          + (item.element_name ? item.element_name : "") + "|" + item.name
          + "|" + std::string(item.source, item.source_length) + "|"
          + (item.external_context ? "external" : "internal"));
    }
    std::sort(snapshot.begin(), snapshot.end());
    return snapshot;
  };
  const auto before_rebuild = binding_snapshot(d);
  xml_ctx rebuild{};
  rebuild.allocator = &doc->allocator;
  rebuild.max_total_work = XML_DEFAULT_MAX_TOTAL_WORK;
  ASSERT_EQ(xml_doctype_rebuild_indexes(&rebuild,
      gtext_markup_document_doctype(doc)), GTEXT_XML_OK) << rebuild.err.message;
  EXPECT_EQ(rebuild.dtd_binding_count, 5u);
  EXPECT_EQ(d->binding_count, 5u);
  EXPECT_EQ(binding_snapshot(d), before_rebuild);
  EXPECT_EQ(markup_check_invariants(doc), nullptr);
  xml_source_free(&rebuild);
  gtext_markup_document_free(doc);
}

TEST(XmlDtdBinding, DeclaredDefaultsAndTokenizedValuesPrecedeNamespaceBinding) {
  const std::string input =
      "<!DOCTYPE root ["
      "<!ENTITY ent 'entity-value'>"
      "<!ATTLIST root xmlns CDATA 'urn:default' xmlns:p CDATA 'urn:p' "
      "id ID '  a   b  ' explicit ID 'default-id' "
      "fixed CDATA #FIXED 'fixed-value' token NMTOKEN #IMPLIED "
      "char CDATA 'x&#x20;y' entity CDATA '&ent;' empty CDATA '' "
      "p:code CDATA 'default-code' required CDATA #REQUIRED "
      "implied CDATA #IMPLIED>"
      "]><root explicit='  custom  ' token='  x  y  '/>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_Markup_Document *doc = nullptr;
  GTEXT_XML_Error error{};
  ASSERT_EQ(gtext_xml_parse(input.data(), input.size(), &options, &doc, &error),
      GTEXT_XML_OK) << error.message;
  ASSERT_NE(doc, nullptr);
  auto *root = gtext_markup_node_next_sibling(
      gtext_markup_node_first_child(gtext_markup_document_root(doc)));
  ASSERT_NE(root, nullptr);
  EXPECT_STREQ(gtext_markup_element_namespace(root), "urn:default");
  GTEXT_Markup_Attribute attr{};
  ASSERT_TRUE(gtext_markup_element_get_attribute(root, nullptr, "id", &attr));
  EXPECT_STREQ(attr.value, "a b");
  EXPECT_EQ(attr.origin, GTEXT_MARKUP_ATTRIBUTE_DEFAULTED);
  ASSERT_TRUE(gtext_markup_element_get_attribute(root, nullptr, "explicit", &attr));
  EXPECT_STREQ(attr.value, "custom");
  EXPECT_EQ(attr.origin, GTEXT_MARKUP_ATTRIBUTE_EXPLICIT);
  ASSERT_TRUE(gtext_markup_element_get_attribute(root, nullptr, "char", &attr));
  EXPECT_STREQ(attr.value, "x y");
  ASSERT_TRUE(gtext_markup_element_get_attribute(root, nullptr, "entity", &attr));
  EXPECT_STREQ(attr.value, "entity-value");
  ASSERT_TRUE(gtext_markup_element_get_attribute(root, nullptr, "empty", &attr));
  EXPECT_STREQ(attr.value, "");
  EXPECT_FALSE(gtext_markup_element_get_attribute(root, nullptr, "required", &attr));
  EXPECT_FALSE(gtext_markup_element_get_attribute(root, nullptr, "implied", &attr));
  bool saw_code = false;
  for (size_t i = 0; i < gtext_markup_element_attribute_count(root); i++) {
    GTEXT_Markup_Attribute candidate{};
    ASSERT_EQ(gtext_markup_element_attribute_at(root, i, &candidate), GTEXT_MARKUP_OK);
    if (std::strcmp(candidate.local, "code") == 0) {
      saw_code = true;
      EXPECT_STREQ(candidate.ns, "urn:p");
      EXPECT_STREQ(candidate.value, "default-code");
      EXPECT_EQ(candidate.origin, GTEXT_MARKUP_ATTRIBUTE_DEFAULTED);
    }
  }
  EXPECT_TRUE(saw_code);
  ASSERT_TRUE(gtext_markup_element_get_attribute(root, nullptr, "fixed", &attr));
  EXPECT_STREQ(attr.value, "fixed-value");
  EXPECT_EQ(attr.origin, GTEXT_MARKUP_ATTRIBUTE_DEFAULTED);
  ASSERT_TRUE(gtext_markup_element_get_attribute(root, nullptr, "token", &attr));
  EXPECT_STREQ(attr.value, "x y");
  EXPECT_EQ(attr.origin, GTEXT_MARKUP_ATTRIBUTE_EXPLICIT);
  GTEXT_Markup_Attribute xmlns{};
  ASSERT_TRUE(gtext_markup_element_get_attribute(root,
      GTEXT_MARKUP_NS_XMLNS, "xmlns", &xmlns));
  EXPECT_STREQ(xmlns.value, "urn:default");
  GTEXT_Markup_Document *clone_doc = nullptr;
  ASSERT_EQ(gtext_markup_document_new(nullptr, &clone_doc), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node *clone = nullptr;
  ASSERT_EQ(gtext_markup_clone(clone_doc, root, true, &clone), GTEXT_MARKUP_OK);
  EXPECT_TRUE(gtext_markup_equal(root, clone));
  GTEXT_Markup_Attribute cloned_attr{};
  ASSERT_TRUE(gtext_markup_element_get_attribute(clone, nullptr, "id", &cloned_attr));
  EXPECT_EQ(cloned_attr.origin, GTEXT_MARKUP_ATTRIBUTE_DEFAULTED);
  gtext_markup_document_free(clone_doc);
  gtext_markup_document_free(doc);

  GTEXT_XML_Reader *reader = gtext_xml_reader_new(&options);
  ASSERT_NE(reader, nullptr);
  for (size_t i = 0; i < input.size(); i++)
    ASSERT_EQ(gtext_xml_reader_feed(reader, input.data() + i, 1, &error),
        GTEXT_XML_OK) << error.message;
  ASSERT_EQ(gtext_xml_reader_feed(reader, nullptr, 0, &error), GTEXT_XML_OK)
      << error.message;
  bool saw_root = false;
  GTEXT_XML_Event event{};
  while (gtext_xml_reader_next(reader, &event) == GTEXT_XML_OK) {
    if (event.type != GTEXT_XML_EVENT_ELEMENT_START
        || std::strcmp(event.local_name, "root") != 0) continue;
    saw_root = true;
    EXPECT_STREQ(event.namespace_uri, "urn:default");
    bool saw_defaulted_id = false;
    bool saw_explicit_token = false;
    for (size_t i = 0; i < event.attribute_count; i++) {
      const auto &attribute = event.attributes[i];
      if (std::strcmp(attribute.local_name, "id") == 0) {
        saw_defaulted_id = true;
        EXPECT_STREQ(attribute.value, "a b");
        EXPECT_EQ(attribute.origin, GTEXT_MARKUP_ATTRIBUTE_DEFAULTED);
      }
      if (std::strcmp(attribute.local_name, "token") == 0) {
        saw_explicit_token = true;
        EXPECT_STREQ(attribute.value, "x y");
        EXPECT_EQ(attribute.origin, GTEXT_MARKUP_ATTRIBUTE_EXPLICIT);
      }
    }
    EXPECT_TRUE(saw_defaulted_id);
    EXPECT_TRUE(saw_explicit_token);
  }
  EXPECT_TRUE(saw_root);
  gtext_xml_reader_free(reader);

  options.max_attributes = 1;
  doc = nullptr;
  EXPECT_EQ(gtext_xml_parse(input.data(), input.size(), &options, &doc, &error),
      GTEXT_XML_E_LIMIT_ATTRIBUTES);
  EXPECT_EQ(doc, nullptr);
}

TEST(XmlDtdBinding, DefaultedAttributesAreCheckedAfterNamespaceBinding) {
  const std::string input =
      "<!DOCTYPE root [<!ATTLIST root p:a CDATA 'one' q:a CDATA 'two'>]>"
      "<root xmlns:p='urn:same' xmlns:q='urn:same'/>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_Markup_Document *doc = nullptr;
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_parse(input.data(), input.size(), &options, &doc, &error),
      GTEXT_XML_E_DUPLICATE_ATTRIBUTE);
  EXPECT_EQ(doc, nullptr);
}

TEST(XmlDtdBinding, UnresolvedDefaultReferencesFailWithoutPublishingATree) {
  const std::string input =
      "<!DOCTYPE root [<!ATTLIST root value CDATA '&missing;'>]><root/>";
  auto options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_Markup_Document *doc = nullptr;
  GTEXT_XML_Error error{};
  EXPECT_EQ(gtext_xml_parse(input.data(), input.size(), &options, &doc, &error),
      GTEXT_XML_E_ENTITY);
  EXPECT_EQ(doc, nullptr);
  EXPECT_EQ(error.byte_offset, input.find("<!ATTLIST"));
}

} // namespace
