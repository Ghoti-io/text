/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <ghoti.io/text/html.h>
#include "markup_test_support.h"

namespace {

std::string sanitize(const char *input, const GTEXT_HTML_Sanitize_Options *opts = nullptr) {
  char *out = nullptr;
  size_t length = 0;
  GTEXT_HTML_Sanitize_Result result{};
  EXPECT_EQ(gtext_html_sanitize_buffer(input, std::char_traits<char>::length(input),
      opts, &out, &length, &result), GTEXT_HTML_OK) << result.message;
  std::string value = out ? std::string(out, length) : std::string();
  gtext_html_sanitize_buffer_free(out);
  return value;
}

TEST(HtmlSanitize, KeepsOnlyDefaultFlowContentAndSafeLinks) {
  std::string out = sanitize("<p onclick=x>hi <a href='https://example.test/x' target=_blank>go</a></p>");
  EXPECT_EQ(out, "<p>hi <a href=\"https://example.test/x\" rel=\"nofollow\">go</a></p>");
}

TEST(HtmlSanitize, DropsActiveForeignAndTemplateSubtrees) {
  std::string out = sanitize("<p>a<script>bad<b>nested</b></script><svg><p>svg</p></svg>"
      "<math><mi>math</mi></math><template><b>hidden</b></template><iframe>frame</iframe>z</p>");
  EXPECT_EQ(out, "<p>a</p><p>svg</p>z<p></p>");
}

TEST(HtmlSanitize, UnwrapsBenignFormattingRecursively) {
  EXPECT_EQ(sanitize("<p>a<span class=x>b<strong>c</strong></span></p>"), "<p>ab<strong>c</strong></p>");
}

TEST(HtmlSanitize, RejectsObfuscatedUnsafeLinksAndDropsTheirAttributes) {
  std::string out = sanitize("<a href='jav&#x61;script:alert(1)' onclick=x>bad</a>"
      "<a href='mailto:user@example.test'>mail</a><a href='tel:555'>phone</a>");
  EXPECT_EQ(out, "<a>bad</a><a href=\"mailto:user@example.test\" rel=\"nofollow\">mail</a><a>phone</a>");
}

TEST(HtmlSanitize, AcceptsIpv6AndRejectsEmptyWebPorts) {
  EXPECT_EQ(sanitize("<a href='https://[2001:db8::1]/path'>v6</a>"
      "<a href='http://example.test:'>bad</a>"),
      "<a href=\"https://[2001:db8::1]/path\" rel=\"nofollow\">v6</a><a>bad</a>");
}

TEST(HtmlSanitize, ImageAndTableGroupsAreOptInAndValidated) {
  const char *input = "<img src='data:text/html,x' alt=x width=12 onerror=x>"
      "<img src='/image.png' alt=ok loading=lazy width=12><table><tr><td colspan=1001>bad</td>"
      "<td rowspan=0>ok</td></tr></table>";
  EXPECT_EQ(sanitize(input), "");
  auto options = gtext_html_sanitize_options_default();
  options.allow_images = true;
  options.allow_tables = true;
  EXPECT_EQ(sanitize(input, &options), "<img alt=\"x\" width=\"12\"><img src=\"/image.png\" alt=\"ok\" loading=\"lazy\" width=\"12\"><table><tbody><tr><td>bad</td><td rowspan=\"0\">ok</td></tr></tbody></table>");
}

TEST(HtmlSanitize, TableSpansAcceptLeadingZeroDigits) {
  auto options = gtext_html_sanitize_options_default();
  options.allow_tables = true;
  EXPECT_EQ(sanitize("<table><tr><td colspan='0002' rowspan='00'>ok</td></tr></table>", &options),
      "<table><tbody><tr><td colspan=\"0002\" rowspan=\"00\">ok</td></tr></tbody></table>");
}

TEST(HtmlSanitize, WorkAndOutputLimitsPublishNothing) {
  auto options = gtext_html_sanitize_options_default();
  options.max_work_units = 1;
  GTEXT_Markup_Document *doc = nullptr;
  GTEXT_Markup_Node *fragment = nullptr;
  GTEXT_HTML_Sanitize_Result result{};
  EXPECT_EQ(gtext_html_sanitize_fragment("<p>x</p>", 8, &options, &doc, &fragment, &result),
      GTEXT_HTML_E_LIMIT_SANITIZE_WORK);
  EXPECT_EQ(doc, nullptr);
  EXPECT_EQ(fragment, nullptr);
  options = gtext_html_sanitize_options_default();
  options.max_output_bytes = 1;
  char *out = nullptr;
  size_t length = 0;
  EXPECT_EQ(gtext_html_sanitize_buffer("<p>x</p>", 8, &options, &out, &length, &result),
      GTEXT_HTML_E_LIMIT_OUTPUT);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(length, 0u);
}

TEST(HtmlSanitize, AddedLinkRelHonorsAttributeLimit) {
  auto options = gtext_html_sanitize_options_default();
  options.parse.max_attributes = 1;
  const char input[] = "<a href='https://example.test'>x</a>";
  char *out = nullptr;
  size_t length = 0;
  GTEXT_HTML_Sanitize_Result result{};
  EXPECT_EQ(gtext_html_sanitize_buffer(input, sizeof(input) - 1,
      &options, &out, &length, &result), GTEXT_HTML_E_LIMIT_ATTRIBUTES);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(length, 0u);
}

TEST(HtmlSanitize, ParseLimitsPublishNothing) {
  struct LimitCase {
    const char * input;
    void (*set)(GTEXT_HTML_Parse_Options *);
    GTEXT_HTML_Status expected;
  };
  const LimitCase cases[] = {
      {"abcd", [](GTEXT_HTML_Parse_Options *o) { o->max_input_bytes = 3; }, GTEXT_HTML_E_LIMIT_INPUT},
      {"<p>x</p>", [](GTEXT_HTML_Parse_Options *o) { o->max_open_elements = 1; }, GTEXT_HTML_E_LIMIT_DEPTH},
      {"<p>x</p>", [](GTEXT_HTML_Parse_Options *o) { o->max_nodes = 2; }, GTEXT_HTML_E_LIMIT_NODES},
      {"<long>x</long>", [](GTEXT_HTML_Parse_Options *o) { o->max_name_bytes = 2; }, GTEXT_HTML_E_LIMIT_NAME},
      {"<p a='long'>x</p>", [](GTEXT_HTML_Parse_Options *o) { o->max_attribute_bytes = 2; }, GTEXT_HTML_E_LIMIT_VALUE},
      {"<!-- long -->", [](GTEXT_HTML_Parse_Options *o) { o->max_comment_bytes = 2; }, GTEXT_HTML_E_LIMIT_COMMENT},
      {"long text", [](GTEXT_HTML_Parse_Options *o) { o->max_text_bytes = 2; }, GTEXT_HTML_E_LIMIT_TEXT},
      {"<p a=1 b=2>x</p>", [](GTEXT_HTML_Parse_Options *o) { o->max_attributes = 1; }, GTEXT_HTML_E_LIMIT_ATTRIBUTES},
  };
  for (const auto &test : cases) {
    auto options = gtext_html_sanitize_options_default();
    test.set(&options.parse);
    GTEXT_Markup_Document *doc = nullptr;
    GTEXT_Markup_Node *fragment = nullptr;
    GTEXT_HTML_Sanitize_Result result{};
    EXPECT_EQ(gtext_html_sanitize_fragment(test.input, std::char_traits<char>::length(test.input),
        &options, &doc, &fragment, &result), test.expected) << test.input;
    EXPECT_EQ(doc, nullptr) << test.input;
    EXPECT_EQ(fragment, nullptr) << test.input;
  }
}

TEST(HtmlSanitize, EveryAllocationFailureLeavesNoPublishedOutputOrLeak) {
  const std::string input = "<p><span>a</span><a href='https://example.test'>b</a></p>";
  bool completed = false;
  for (size_t fail_on = 1; fail_on < 500; ++fail_on) {
    FaultAllocator fault;
    auto options = gtext_html_sanitize_options_default();
    options.parse.allocator = &fault.alloc;
    fault.arm(fail_on);
    GTEXT_Markup_Document *doc = nullptr;
    GTEXT_Markup_Node *fragment = nullptr;
    GTEXT_HTML_Sanitize_Result result{};
    GTEXT_HTML_Status status = gtext_html_sanitize_fragment(input.data(), input.size(),
        &options, &doc, &fragment, &result);
    fault.disarm();
    if (status == GTEXT_HTML_OK) {
      ASSERT_NE(doc, nullptr);
      ASSERT_NE(fragment, nullptr);
      gtext_markup_document_free(doc);
      completed = true;
    } else {
      EXPECT_EQ(status, GTEXT_HTML_E_OOM);
      EXPECT_EQ(doc, nullptr);
      EXPECT_EQ(fragment, nullptr);
    }
    EXPECT_EQ(fault.bad_frees, 0u);
    EXPECT_TRUE(fault.live.empty());
    if (completed) break;
  }
  EXPECT_TRUE(completed) << "the sweep reached the final allocation";
}

TEST(HtmlSanitize, EveryBufferAllocationFailureLeavesNoPublishedOutputOrLeak) {
  const std::string input = "<p><span>a</span><a href='https://example.test'>b</a></p>";
  bool completed = false;
  for (size_t fail_on = 1; fail_on < 500; ++fail_on) {
    FaultAllocator fault;
    auto options = gtext_html_sanitize_options_default();
    options.parse.allocator = &fault.alloc;
    fault.arm(fail_on);
    char *output = nullptr;
    size_t output_length = 0;
    GTEXT_HTML_Sanitize_Result result{};
    GTEXT_HTML_Status status = gtext_html_sanitize_buffer(input.data(), input.size(),
        &options, &output, &output_length, &result);
    fault.disarm();
    if (status == GTEXT_HTML_OK) {
      ASSERT_NE(output, nullptr);
      EXPECT_GT(output_length, 0u);
      gtext_html_sanitize_buffer_free(output);
      completed = true;
    } else {
      EXPECT_EQ(status, GTEXT_HTML_E_OOM);
      EXPECT_EQ(output, nullptr);
      EXPECT_EQ(output_length, 0u);
    }
    EXPECT_EQ(fault.bad_frees, 0u);
    EXPECT_TRUE(fault.live.empty());
    if (completed) break;
  }
  EXPECT_TRUE(completed) << "the sweep reached the final buffer allocation";
}

TEST(HtmlSanitize, ReturnsFreshTree) {
  GTEXT_Markup_Document *doc = nullptr;
  GTEXT_Markup_Node *fragment = nullptr;
  GTEXT_HTML_Sanitize_Result result{};
  ASSERT_EQ(gtext_html_sanitize_fragment("<b>x</b>", 8, nullptr, &doc, &fragment, &result), GTEXT_HTML_OK);
  ASSERT_NE(doc, nullptr);
  ASSERT_NE(fragment, nullptr);
  EXPECT_EQ(result.policy_version, std::string("ghoti-html-flow-1"));
  EXPECT_GT(result.work_units, 0u);
  gtext_markup_document_free(doc);
}

} // namespace
