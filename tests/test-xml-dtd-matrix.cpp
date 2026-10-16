/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>

#include "xml_replay.h"

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace {

struct Resolver {
  std::string bytes = "<!ENTITY remote 'external'>";
  std::unordered_map<std::string, std::string> responses;
  size_t opens = 0;
  size_t closes = 0;
  bool declined = false;
};

struct ResolverSource {
  Resolver * resolver;
  std::string bytes;
  std::string base_uri;
  size_t offset = 0;
};

GTEXT_XML_Source_Read_Result read_source(void * user, unsigned char * buffer,
    size_t capacity, size_t * count) {
  auto * source = static_cast<ResolverSource *>(user);
  *count = std::min(capacity, std::min(size_t(2),
      source->bytes.size() - source->offset));
  if (*count == 0) return GTEXT_XML_SOURCE_EOF;
  std::memcpy(buffer, source->bytes.data() + source->offset, *count);
  source->offset += *count;
  return GTEXT_XML_SOURCE_DATA;
}

void resolver_close(void * user) {
  auto * source = static_cast<ResolverSource *>(user);
  source->resolver->closes++;
  delete source;
}

GTEXT_XML_Resolver_Result resolver_open(const GTEXT_XML_Resolve_Request * request,
    void * user, GTEXT_XML_External_Source * source) {
  auto * resolver = static_cast<Resolver *>(user);
  resolver->opens++;
  if (resolver->declined) return GTEXT_XML_RESOLVER_DECLINED;
  auto * opened = new (std::nothrow) ResolverSource;
  if (!opened) return GTEXT_XML_RESOLVER_FAILED;
  opened->resolver = resolver;
  opened->base_uri = request && request->system_id
      ? std::string("memory:/") + request->system_id
      : "memory:/resolved.dtd";
  const auto response = request && request->system_id
      ? resolver->responses.find(request->system_id) : resolver->responses.end();
  opened->bytes = response == resolver->responses.end()
      ? resolver->bytes : response->second;
  source->handle = opened;
  source->identity = opened;
  source->base_uri = opened->base_uri.c_str();
  source->read = read_source;
  source->close = resolver_close;
  return GTEXT_XML_RESOLVER_OPENED;
}

struct TempInput {
  std::string path;
  FILE * file = nullptr;
  TempInput() {
#ifdef _WIN32
    char dir[MAX_PATH + 1]{};
    char name[MAX_PATH + 1]{};
    if (GetTempPathA(sizeof dir, dir) != 0
        && GetTempFileNameA(dir, "gtd", 0, name) != 0) {
      path = name;
      file = std::fopen(path.c_str(), "wb");
    }
#else
    char name[] = "/tmp/ghoti-xml-dtd-matrix-XXXXXX";
    const int fd = mkstemp(name);
    if (fd >= 0) {
      path = name;
      file = fdopen(fd, "wb");
      if (!file) {
        close(fd);
        std::remove(path.c_str());
        path.clear();
      }
    }
#endif
  }
  ~TempInput() {
    if (file) std::fclose(file);
    if (!path.empty()) std::remove(path.c_str());
  }
  bool write(const std::string & bytes) {
    if (!file) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (std::fclose(file) != 0) return false;
    file = nullptr;
    return ok;
  }
};

std::string event_text(const GTEXT_XML_Event & event);

GTEXT_XML_Status drain_reader(GTEXT_XML_Reader * reader,
    xml_replay * replay, std::vector<std::string> * events) {
  GTEXT_XML_Event event{};
  for (;;) {
    const GTEXT_XML_Status status = gtext_xml_reader_next(reader, &event);
    if (status == GTEXT_XML_E_STATE || status == GTEXT_XML_E_INCOMPLETE)
      return GTEXT_XML_OK;
    if (status != GTEXT_XML_OK) return status;
    events->push_back(event_text(event));
    const GTEXT_XML_Status replay_status = xml_replay_event(&event, replay);
    if (replay_status != GTEXT_XML_OK) return replay_status;
  }
}

struct Case {
  const char * name;
  const char * document;
  GTEXT_XML_DTD_Mode dtd_mode;
  bool external_subset;
  bool declined_subset;
};

void configure_resolver(Resolver & resolver, const Case & test) {
  resolver.declined = test.declined_subset;
  if (std::strcmp(test.name, "nested-external-parameter-chain") == 0) {
    resolver.responses["urn:matrix:subset"] =
        "<!ENTITY % outer SYSTEM 'urn:matrix:outer'>%outer;";
    resolver.responses["urn:matrix:outer"] =
        "<!ENTITY % inner SYSTEM 'urn:matrix:inner'>%inner;";
    resolver.responses["urn:matrix:inner"] = "<!ENTITY remote 'nested'>";
  }
}

size_t expected_resolver_opens(const Case & test) {
  return std::strcmp(test.name, "nested-external-parameter-chain") == 0
      ? 3u : test.external_subset ? 1u : 0u;
}

std::string event_text(const GTEXT_XML_Event & event) {
  std::string result = std::to_string(static_cast<int>(event.type)) + ":"
      + std::to_string(event.location.offset) + ":"
      + std::to_string(event.location.line) + ":"
      + std::to_string(event.location.column) + ":"
      + std::to_string(event.location.source_id) + ":"
      + std::to_string(event.location.reference_source_id);
  const auto append_optional = [&result](const char * value) {
    result += value ? ":" + std::string(value) : ":-";
  };
  append_optional(event.name);
  append_optional(event.public_id);
  append_optional(event.system_id);
  append_optional(event.version);
  append_optional(event.encoding);
  append_optional(event.namespace_uri);
  append_optional(event.prefix);
  append_optional(event.local_name);
  result += ":" + std::to_string(static_cast<int>(event.standalone));
  result += ":" + std::to_string(static_cast<int>(event.encoding_source));
  if (event.type == GTEXT_XML_EVENT_UNREAD_RESOURCE) {
    result += ":" + std::to_string(static_cast<int>(event.resource_kind));
  }
  if (event.data)
    result.append(":").append(event.data, event.data_length);
  result += event.reference_trail_truncated ? ":trail+" : ":trail=";
  for (size_t i = 0; i < event.reference_trail_count; i++)
    result += std::to_string(event.reference_trail[i]) + ",";
  for (size_t i = 0; i < event.attribute_count; i++) {
    result += ":@" + std::string(event.attributes[i].local_name) + "="
        + std::string(event.attributes[i].value,
            event.attributes[i].value_length);
  }
  return result;
}

struct Capture {
  std::vector<std::string> events;
  xml_replay replay;
  Capture() { xml_replay_init(&replay); }
  ~Capture() { xml_replay_free(&replay); }
};

GTEXT_XML_Status capture_event(const GTEXT_XML_Event * event, void * user) {
  auto * capture = static_cast<Capture *>(user);
  capture->events.push_back(event_text(*event));
  return xml_replay_event(event, &capture->replay);
}

TEST(XmlDtdMatrix, DomFilePushAndPullAgreeAcrossDtdModesAndVersions) {
  const Case cases[] = {
      {"dtd-off-10", "<!DOCTYPE root><root>plain</root>",
          GTEXT_XML_DTD_OFF, false, false},
      {"dtd-process-10",
          "<?xml version='1.0'?><!DOCTYPE root [<!ENTITY e 'expanded'>"
          "<!ATTLIST root token CDATA 'default'>]><root>&e;tail</root>",
          GTEXT_XML_DTD_PROCESS, false, false},
      {"dtd-process-11",
          "<?xml version='1.1'?><!DOCTYPE root [<!ENTITY e '&#x1;'>]>"
          "<root>&e;tail</root>", GTEXT_XML_DTD_PROCESS, false, false},
      {"resolved-external-subset",
          "<!DOCTYPE root SYSTEM 'urn:matrix:subset'><root>&remote;</root>",
          GTEXT_XML_DTD_PROCESS, true, false},
      {"resolved-external-parameter-entity",
          "<!DOCTYPE root [<!ENTITY % ext SYSTEM 'urn:matrix:parameter'>"
          "%ext;]><root>&remote;</root>",
          GTEXT_XML_DTD_PROCESS, true, false},
      {"nested-external-parameter-chain",
          "<!DOCTYPE root SYSTEM 'urn:matrix:subset'><root>&remote;</root>",
          GTEXT_XML_DTD_PROCESS, true, false},
      {"declined-external-subset",
          "<!DOCTYPE root SYSTEM 'urn:matrix:subset'><root/>",
          GTEXT_XML_DTD_PROCESS, true, true}};

  for (const Case & test : cases) {
    SCOPED_TRACE(test.name);
    GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
    options.dtd_mode = test.dtd_mode;
    Resolver dom_resolver;
    configure_resolver(dom_resolver, test);
    if (test.external_subset) {
      options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
      options.resolver = resolver_open;
      options.resolver_user_data = &dom_resolver;
    }

    GTEXT_Markup_Document * dom = nullptr;
    GTEXT_XML_Error error{};
    const std::string document = test.document;
    ASSERT_EQ(gtext_xml_parse(document.data(), document.size(), &options,
        &dom, &error), GTEXT_XML_OK) << error.message;
    ASSERT_NE(dom, nullptr);
    EXPECT_EQ(dom_resolver.opens, expected_resolver_opens(test));
    EXPECT_EQ(dom_resolver.closes,
        !test.declined_subset ? expected_resolver_opens(test) : 0u);

    TempInput input;
    ASSERT_FALSE(input.path.empty());
    ASSERT_TRUE(input.write(document));
    Resolver file_resolver;
    configure_resolver(file_resolver, test);
    if (test.external_subset) options.resolver_user_data = &file_resolver;
    GTEXT_Markup_Document * from_file = nullptr;
    ASSERT_EQ(gtext_xml_parse_file(input.path.c_str(), &options, &from_file,
        &error), GTEXT_XML_OK) << error.message;
    ASSERT_NE(from_file, nullptr);
    EXPECT_EQ(file_resolver.opens, expected_resolver_opens(test));
    EXPECT_EQ(file_resolver.closes,
        !test.declined_subset ? expected_resolver_opens(test) : 0u);
    EXPECT_EQ(xml_replay_compare(dom, from_file), nullptr);

    for (const std::vector<size_t> & cuts : {std::vector<size_t>{1},
             std::vector<size_t>{2, 7, 1, 3, 11}}) {
      Resolver push_resolver;
      configure_resolver(push_resolver, test);
      if (test.external_subset) options.resolver_user_data = &push_resolver;
      Capture pushed;
      GTEXT_XML_Stream * stream = gtext_xml_stream_new(
          &options, capture_event, &pushed);
      ASSERT_NE(stream, nullptr);
      size_t offset = 0;
      size_t cut = 0;
      GTEXT_XML_Status status = GTEXT_XML_OK;
      while (offset < document.size() && status == GTEXT_XML_OK) {
        const size_t amount = std::min(cuts[cut++ % cuts.size()],
            document.size() - offset);
        status = gtext_xml_stream_feed(stream, document.data() + offset,
            amount, &error);
        offset += amount;
      }
      if (status == GTEXT_XML_OK) status = gtext_xml_stream_finish(stream, &error);
      ASSERT_EQ(status, GTEXT_XML_OK) << error.message;
      EXPECT_EQ(push_resolver.opens, expected_resolver_opens(test));
      EXPECT_EQ(push_resolver.closes,
          !test.declined_subset ? expected_resolver_opens(test) : 0u);
      ASSERT_NE(pushed.replay.doc, nullptr);
      const char * push_difference = xml_replay_compare(dom, pushed.replay.doc);
      EXPECT_EQ(push_difference, nullptr)
          << (push_difference ? push_difference : "");
      gtext_xml_stream_free(stream);

      Resolver pull_resolver;
      configure_resolver(pull_resolver, test);
      if (test.external_subset) options.resolver_user_data = &pull_resolver;
      options.max_pull_queue_events = 2;
      options.max_pull_queue_bytes = 4096;
      GTEXT_XML_Reader * reader = gtext_xml_reader_new(&options);
      ASSERT_NE(reader, nullptr);
      xml_replay replay{};
      xml_replay_init(&replay);
      std::vector<std::string> pull_events;
      offset = 0;
      cut = 0;
      status = GTEXT_XML_OK;
      while (offset < document.size() && status == GTEXT_XML_OK) {
        const size_t amount = std::min(cuts[cut++ % cuts.size()],
            document.size() - offset);
        status = gtext_xml_reader_feed(reader, document.data() + offset,
            amount, &error);
        offset += amount;
        if (status == GTEXT_XML_OK)
          status = drain_reader(reader, &replay, &pull_events);
      }
      if (status == GTEXT_XML_OK) {
        status = gtext_xml_reader_feed(reader, nullptr, 0, &error);
      }
      if (status == GTEXT_XML_OK)
        status = drain_reader(reader, &replay, &pull_events);
      ASSERT_EQ(status, GTEXT_XML_OK) << error.message;
      EXPECT_EQ(pull_resolver.opens, expected_resolver_opens(test));
      EXPECT_EQ(pull_resolver.closes,
          !test.declined_subset ? expected_resolver_opens(test) : 0u);
      ASSERT_NE(replay.doc, nullptr);
      const char * pull_difference = xml_replay_compare(dom, replay.doc);
      EXPECT_EQ(pull_difference, nullptr)
          << (pull_difference ? pull_difference : "");
      EXPECT_EQ(pull_events, pushed.events);
      xml_replay_free(&replay);
      gtext_xml_reader_free(reader);
    }

    gtext_markup_document_free(from_file);
    gtext_markup_document_free(dom);
  }
}

TEST(XmlDtdMatrix, MalformedExternalSubsetFailsEquallyAndClosesEveryResolver) {
  const std::string document =
      "<!DOCTYPE root SYSTEM 'urn:matrix:malformed'><root/>";
  GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = resolver_open;
  GTEXT_XML_Error error{};

  Resolver dom_resolver;
  dom_resolver.bytes = "not-a-declaration";
  options.resolver_user_data = &dom_resolver;
  GTEXT_Markup_Document * dom = nullptr;
  const GTEXT_XML_Status expected = gtext_xml_parse(document.data(),
      document.size(), &options, &dom, &error);
  ASSERT_NE(expected, GTEXT_XML_OK);
  EXPECT_EQ(dom, nullptr);
  EXPECT_EQ(dom_resolver.opens, 1u);
  EXPECT_EQ(dom_resolver.closes, 1u);

  TempInput input;
  ASSERT_FALSE(input.path.empty());
  ASSERT_TRUE(input.write(document));
  Resolver file_resolver;
  file_resolver.bytes = dom_resolver.bytes;
  options.resolver_user_data = &file_resolver;
  GTEXT_Markup_Document * from_file = nullptr;
  EXPECT_EQ(gtext_xml_parse_file(input.path.c_str(), &options, &from_file,
      &error), expected);
  EXPECT_EQ(from_file, nullptr);
  EXPECT_EQ(file_resolver.opens, 1u);
  EXPECT_EQ(file_resolver.closes, 1u);

  Resolver push_resolver;
  push_resolver.bytes = dom_resolver.bytes;
  options.resolver_user_data = &push_resolver;
  Capture pushed;
  GTEXT_XML_Stream * stream = gtext_xml_stream_new(
      &options, capture_event, &pushed);
  ASSERT_NE(stream, nullptr);
  GTEXT_XML_Status push_status = gtext_xml_stream_feed(stream,
      document.data(), document.size(), &error);
  if (push_status == GTEXT_XML_OK)
    push_status = gtext_xml_stream_finish(stream, &error);
  EXPECT_EQ(push_status, expected);
  EXPECT_EQ(push_resolver.opens, 1u);
  EXPECT_EQ(push_resolver.closes, 1u);
  gtext_xml_stream_free(stream);

  Resolver pull_resolver;
  pull_resolver.bytes = dom_resolver.bytes;
  options.resolver_user_data = &pull_resolver;
  GTEXT_XML_Reader * reader = gtext_xml_reader_new(&options);
  ASSERT_NE(reader, nullptr);
  GTEXT_XML_Status pull_status = gtext_xml_reader_feed(reader,
      document.data(), document.size(), &error);
  if (pull_status == GTEXT_XML_OK)
    pull_status = gtext_xml_reader_feed(reader, nullptr, 0, &error);
  EXPECT_EQ(pull_status, expected);
  EXPECT_EQ(pull_resolver.opens, 1u);
  EXPECT_EQ(pull_resolver.closes, 1u);
  gtext_xml_reader_free(reader);
}

TEST(XmlDtdMatrix, WholeInputAndStreamReadersKeepTheirErrorPriorities) {
  std::string bytes = "<a></b>";
  bytes.push_back(static_cast<char>(0xFF));
  GTEXT_XML_Error error{};
  GTEXT_Markup_Document * document = nullptr;
  EXPECT_EQ(gtext_xml_parse(bytes.data(), bytes.size(), nullptr,
      &document, &error), GTEXT_XML_E_UTF8);
  EXPECT_EQ(document, nullptr);

  TempInput input;
  ASSERT_FALSE(input.path.empty());
  ASSERT_TRUE(input.write(bytes));
  document = nullptr;
  EXPECT_EQ(gtext_xml_parse_file(input.path.c_str(), nullptr, &document,
      &error), GTEXT_XML_E_UTF8);
  EXPECT_EQ(document, nullptr);

  Capture push;
  GTEXT_XML_Stream * stream = gtext_xml_stream_new(
      nullptr, capture_event, &push);
  ASSERT_NE(stream, nullptr);
  GTEXT_XML_Status status = GTEXT_XML_OK;
  size_t consumed = 0;
  while (consumed < bytes.size() && status == GTEXT_XML_OK) {
    status = gtext_xml_stream_feed(stream, bytes.data() + consumed, 1, &error);
    consumed++;
  }
  EXPECT_EQ(status, GTEXT_XML_E_MISMATCHED_TAG);
  EXPECT_LT(consumed, bytes.size());
  gtext_xml_stream_free(stream);

  GTEXT_XML_Reader * reader = gtext_xml_reader_new(nullptr);
  ASSERT_NE(reader, nullptr);
  status = GTEXT_XML_OK;
  consumed = 0;
  while (consumed < bytes.size() && status == GTEXT_XML_OK) {
    status = gtext_xml_reader_feed(reader, bytes.data() + consumed, 1, &error);
    consumed++;
  }
  EXPECT_EQ(status, GTEXT_XML_E_MISMATCHED_TAG);
  EXPECT_LT(consumed, bytes.size());
  gtext_xml_reader_free(reader);

  const std::vector<std::string> encoding_edges = {
      std::string("<a/>") + "\xE2",
      std::string("<?xml version='1.0' encoding='UTF-8") + "\xFF'?><a/>"};
  for (const std::string & malformed : encoding_edges) {
    SCOPED_TRACE(malformed.size());
    document = nullptr;
    EXPECT_EQ(gtext_xml_parse(malformed.data(), malformed.size(), nullptr,
        &document, &error), GTEXT_XML_E_UTF8);
    EXPECT_EQ(document, nullptr);

    Capture encoding_push;
    stream = gtext_xml_stream_new(nullptr, capture_event, &encoding_push);
    ASSERT_NE(stream, nullptr);
    status = GTEXT_XML_OK;
    consumed = 0;
    while (consumed < malformed.size() && status == GTEXT_XML_OK) {
      status = gtext_xml_stream_feed(stream, malformed.data() + consumed, 1,
          &error);
      consumed++;
    }
    if (status == GTEXT_XML_OK)
      status = gtext_xml_stream_finish(stream, &error);
    EXPECT_EQ(status, GTEXT_XML_E_UTF8);
    gtext_xml_stream_free(stream);

    reader = gtext_xml_reader_new(nullptr);
    ASSERT_NE(reader, nullptr);
    status = GTEXT_XML_OK;
    consumed = 0;
    while (consumed < malformed.size() && status == GTEXT_XML_OK) {
      status = gtext_xml_reader_feed(reader, malformed.data() + consumed, 1,
          &error);
      consumed++;
    }
    if (status == GTEXT_XML_OK)
      status = gtext_xml_reader_feed(reader, nullptr, 0, &error);
    EXPECT_EQ(status, GTEXT_XML_E_UTF8);
    gtext_xml_reader_free(reader);
  }
}

} // namespace
