/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The HTML serializer when something goes wrong, and at the edges.
 *
 * - The sink refuses the n-th call, for every n a large document makes: GTEXT_HTML_E_WRITE (or GTEXT_HTML_E_OOM when
 *   it says so), the sink has received exactly the chunks before it, and nothing is left behind.
 * - The allocator refuses the n-th allocation, for every n up to the last one a call makes, through every entry point
 *   and with the options that allocate (sorted attributes, the held head of inject_meta_charset, a deep tree): E_OOM,
 *   no leak, never a free of a block that is not its own, and the same bytes as the run that failed nothing when the
 *   failed allocation was one it could do without.
 * - The output limit at every value from 1 to the size of the output, the depth limit at every value around the depth
 *   of the tree, a tree 100,000 deep (no C stack), and a tree too deep for the default.
 * - The work is linear in the size of the tree, counted: nodes visited, allocations made, calls to the sink.
 * - The file form, atomically.
 * - Independent calls on one tree from many threads.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/html.h>

#include "html_serialize_test_support.h"

namespace {

using namespace hs;

constexpr GTEXT_HTML_Serialize_Scope CHILDREN = GTEXT_HTML_SERIALIZE_CHILDREN;
constexpr GTEXT_HTML_Serialize_Scope OUTER = GTEXT_HTML_SERIALIZE_OUTER;

// A document with every kind of thing in it, and many of them: some 60 KiB of output, 15 chunks of the sink.
std::string big_html() {
  std::string h = "<!DOCTYPE html><html lang=en><head><meta charset=ascii><title>t</title></head><body class=\"a b\">";
  for (int i = 0; i < 600; i++) {
    h += "<div id=d" + std::to_string(i) + " title='x\"y' data-n=" + std::to_string(i) + ">text &amp; more<b>bold</b>"
         "<ul><li>one<li>two</ul><table><tr><td>cell<td>cell</table><!--c" + std::to_string(i) + "-->"
         "<script>if (a<b) {}</script><br><img src=x alt=\"\"></div>";
  }
  return h + "</body></html>";
}

Opts everything_on() {
  Opts o = defaults();
  o.omit_optional_tags = true;
  o.minimize_boolean_attributes = true;
  o.quote_attr_values = GTEXT_HTML_QUOTE_WHEN_NEEDED;
  o.use_best_quote_char = true;
  o.alphabetical_attributes = true;
  o.inject_meta_charset = true;
  o.encoding = "utf-8";
  o.doctype_identifiers = true;
  o.strip_whitespace = true;
  return o;
}

// ---- the sink fails ------------------------------------------------------------------------------------

TEST(HtmlSerializeFaults, TheSinkRefusesEveryCallAndGetsExactlyTheChunksBefore) {
  Parsed p = parse_document(big_html());
  for (const Opts & o : {defaults(), everything_on()}) {
    Collect whole;
    GTEXT_Markup_Sink s = whole.sink();
    GTEXT_HTML_Serialize_Result full_result;
    ASSERT_EQ(gtext_html_serialize(p.top(), CHILDREN, &o, &s, &full_result), GTEXT_HTML_OK);
    const std::string full = whole.data;
    ASSERT_GT(full.size(), 20u * 4096u);
    const size_t calls = whole.calls;
    EXPECT_EQ(calls, (full.size() + 4095) / 4096);
    EXPECT_EQ(full_result.bytes_written, full.size());
    for (size_t n = 1; n <= calls; n++) {
      Collect c;
      c.fail_on = n;
      GTEXT_Markup_Sink cs = c.sink();
      GTEXT_HTML_Serialize_Result r;
      EXPECT_EQ(gtext_html_serialize(p.top(), CHILDREN, &o, &cs, &r), GTEXT_HTML_E_WRITE) << n;
      EXPECT_EQ(r.status, GTEXT_HTML_E_WRITE);
      EXPECT_STREQ(r.message, "output refused");
      EXPECT_EQ(c.calls, n); // nothing is written after a refusal
      EXPECT_EQ(c.data, full.substr(0, (n - 1) * 4096)) << n;
      EXPECT_EQ(r.bytes_written, c.data.size());
      // A sink out of memory is an out of memory.
      Collect m;
      m.fail_on = n;
      m.fail_with = GTEXT_MARKUP_E_OOM;
      GTEXT_Markup_Sink ms = m.sink();
      EXPECT_EQ(gtext_html_serialize(p.top(), CHILDREN, &o, &ms, &r), GTEXT_HTML_E_OOM) << n;
    }
  }
}

TEST(HtmlSerializeFaults, ASinkRefusalLeaksNothingAndAStreamIsStickyAfterIt) {
  FaultAllocator fa;
  Parsed p = parse_document(big_html());
  Opts o = everything_on();
  o.allocator = &fa.alloc;
  for (size_t n : {size_t(1), size_t(2), size_t(7)}) {
    Collect c;
    c.fail_on = n;
    GTEXT_Markup_Sink cs = c.sink();
    EXPECT_EQ(gtext_html_serialize(p.top(), CHILDREN, &o, &cs, nullptr), GTEXT_HTML_E_WRITE);
    EXPECT_TRUE(fa.live.empty());
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  // The token stream: after the failure every call answers with it.
  Collect c;
  c.fail_on = 1;
  GTEXT_Markup_Sink cs = c.sink();
  GTEXT_HTML_Serializer * ser = nullptr;
  ASSERT_EQ(gtext_html_serializer_new(&o, &cs, &ser), GTEXT_HTML_OK);
  GTEXT_HTML_Status st = GTEXT_HTML_OK;
  const std::string chunk(5000, 'x');
  for (int i = 0; i < 3 && st == GTEXT_HTML_OK; i++) {
    st = gtext_html_serializer_characters(ser, chunk.data(), chunk.size());
  }
  EXPECT_EQ(st, GTEXT_HTML_E_WRITE);
  EXPECT_EQ(gtext_html_serializer_characters(ser, "x", 1), GTEXT_HTML_E_WRITE);
  EXPECT_EQ(gtext_html_serializer_start_tag(ser, nullptr, "a", nullptr, 0), GTEXT_HTML_E_WRITE);
  GTEXT_HTML_Serialize_Result r;
  EXPECT_EQ(gtext_html_serializer_finish(ser, &r), GTEXT_HTML_E_WRITE);
  EXPECT_EQ(r.status, GTEXT_HTML_E_WRITE);
  gtext_html_serializer_free(ser);
  EXPECT_TRUE(fa.live.empty());
  EXPECT_EQ(fa.bad_frees, 0u);
}

// ---- the allocator fails --------------------------------------------------------------------------------

// What a run does with the allocator in the options: it is told what to build and returns the status and bytes.
struct Runner {
  const char * name;
  std::function<GTEXT_HTML_Status(const Node *, const Opts &, std::string *)> run;
};

GTEXT_HTML_Status via_sink(const Node * n, const Opts & o, std::string * out) {
  Collect c;
  GTEXT_Markup_Sink s = c.sink();
  const GTEXT_HTML_Status st = gtext_html_serialize(n, CHILDREN, &o, &s, nullptr);
  if (st == GTEXT_HTML_OK) {
    *out = c.data;
  }
  return st;
}

GTEXT_HTML_Status via_buffer(const Node * n, const Opts & o, std::string * out) {
  char * buf = nullptr;
  size_t len = 0;
  const GTEXT_HTML_Status st = gtext_html_serialize_buffer(n, OUTER, &o, &buf, &len, nullptr);
  if (st == GTEXT_HTML_OK) {
    out->assign(buf, len);
    gtext_html_serialize_buffer_free(gtext_markup_node_document(n), &o, buf);
  }
  else if (buf) {
    ADD_FAILURE() << "a failed call left a buffer";
  }
  return st;
}

// The token stream, fed the tokens of a small document with every token kind.
GTEXT_HTML_Status via_stream(const Node *, const Opts & o, std::string * out) {
  Collect c;
  GTEXT_Markup_Sink s = c.sink();
  GTEXT_HTML_Serializer * ser = nullptr;
  GTEXT_HTML_Status st = gtext_html_serializer_new(&o, &s, &ser);
  if (st != GTEXT_HTML_OK) {
    return st;
  }
  GTEXT_Markup_Attribute a[3];
  std::memset(a, 0, sizeof(a));
  a[0].local = "zeta";
  a[0].value = "1";
  a[0].value_length = 1;
  a[1].local = "alpha";
  a[1].value = "2";
  a[1].value_length = 1;
  a[2].local = "charset";
  a[2].value = "ascii";
  a[2].value_length = 5;
  auto step = [&](GTEXT_HTML_Status r) {
    if (st == GTEXT_HTML_OK) {
      st = r;
    }
  };
  step(gtext_html_serializer_doctype(ser, "html", "p", "s"));
  step(gtext_html_serializer_start_tag(ser, nullptr, "html", nullptr, 0));
  step(gtext_html_serializer_start_tag(ser, nullptr, "head", nullptr, 0));
  step(gtext_html_serializer_empty_tag(ser, nullptr, "meta", a + 2, 1));
  step(gtext_html_serializer_comment(ser, "c", 1));
  step(gtext_html_serializer_end_tag(ser, nullptr, "head"));
  step(gtext_html_serializer_start_tag(ser, nullptr, "body", nullptr, 0));
  step(gtext_html_serializer_start_tag(ser, nullptr, "div", a, 2));
  step(gtext_html_serializer_characters(ser, "a  b", 4));
  step(gtext_html_serializer_processing_instruction(ser, "t", "d", 1));
  step(gtext_html_serializer_end_tag(ser, nullptr, "div"));
  step(gtext_html_serializer_end_tag(ser, nullptr, "body"));
  step(gtext_html_serializer_end_tag(ser, nullptr, "html"));
  const GTEXT_HTML_Status fs = gtext_html_serializer_finish(ser, nullptr);
  if (st == GTEXT_HTML_OK) {
    st = fs;
  }
  gtext_html_serializer_free(ser);
  if (st == GTEXT_HTML_OK) {
    *out = c.data;
  }
  return st;
}

TEST(HtmlSerializeFaults, EveryAllocationCanFailAndNothingLeaks) {
  // A tree with a deep part (the stack grows several times), many attributes (the sort buffer), a head with a
  // meta to find (the held tokens), and a template.
  std::string h = "<!DOCTYPE html><html><head><meta name=x><title>t</title></head><body>";
  for (int i = 0; i < 40; i++) {
    h += "<div a=1 c=3 b=2>";
  }
  h += "<template><p>x</template>deep";
  for (int i = 0; i < 40; i++) {
    h += "</div>";
  }
  h += "</body></html>";
  Parsed p = parse_document(h);
  const Runner runs[] = {{"sink", via_sink}, {"buffer", via_buffer}, {"stream", via_stream}};
  const Opts variants[] = {defaults(), everything_on()};
  for (const Runner & run : runs) {
    for (const Opts & base : variants) {
      SCOPED_TRACE(run.name);
      std::string baseline;
      Opts o = base;
      ASSERT_EQ(run.run(run.name == std::string("buffer") ? find(p.top(), "body") : p.top(), o, &baseline), GTEXT_HTML_OK);
      ASSERT_FALSE(baseline.empty());
      size_t oom_runs = 0;
      bool finished = false;
      for (size_t n = 1; n <= 400 && !finished; n++) {
        FaultAllocator fa;
        o = base;
        o.allocator = &fa.alloc;
        std::string got;
        fa.arm(n);
        const GTEXT_HTML_Status st =
            run.run(run.name == std::string("buffer") ? find(p.top(), "body") : p.top(), o, &got);
        fa.disarm();
        if (!fa.failed) {
          EXPECT_EQ(st, GTEXT_HTML_OK);
          EXPECT_EQ(got, baseline);
          finished = true;
        }
        else {
          EXPECT_EQ(st, GTEXT_HTML_E_OOM) << "allocation " << n;
          oom_runs++;
        }
        EXPECT_TRUE(fa.live.empty()) << "allocation " << n << " leaked " << fa.live.size() << " blocks";
        EXPECT_EQ(fa.bad_frees, 0u);
      }
      EXPECT_TRUE(finished) << "the call makes more than 400 allocations";
      EXPECT_GE(oom_runs, 2u);
    }
  }
}

TEST(HtmlSerializeFaults, TheFileFormFailsCleanlyOnEveryAllocation) {
  Parsed p = parse_document("<!DOCTYPE html><html><head></head><body><p>x<p>y</body></html>");
  const std::string path = "/tmp/ghoti-html-serialize-oom-" + std::to_string(getpid()) + ".html";
  std::remove(path.c_str());
  Opts o = everything_on();
  size_t oom_runs = 0;
  bool finished = false;
  for (size_t n = 1; n <= 100 && !finished; n++) {
    FaultAllocator fa;
    o.allocator = &fa.alloc;
    fa.arm(n);
    const GTEXT_HTML_Status st = gtext_html_serialize_file(p.top(), CHILDREN, path.c_str(), &o, nullptr);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_HTML_OK);
      finished = true;
    }
    else {
      // The file layer reports its own allocation failure as out of memory too.
      EXPECT_TRUE(st == GTEXT_HTML_E_OOM || st == GTEXT_HTML_E_WRITE) << st;
      oom_runs++;
    }
    EXPECT_TRUE(fa.live.empty()) << n;
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_TRUE(finished);
  EXPECT_GE(oom_runs, 1u);
  std::remove(path.c_str());
}

// ---- the limits -----------------------------------------------------------------------------------------

TEST(HtmlSerializeLimits, TheOutputLimitIsExactAndTheSinkGetsAPrefix) {
  Parsed p = parse_document("<!DOCTYPE html><html><head></head><body><p class=\"a\">text &amp; more<br><!--c--></p></body></html>");
  Opts o = defaults();
  Out full = serialize_with(p.top(), CHILDREN, o);
  ASSERT_EQ(full.status, GTEXT_HTML_OK);
  const size_t n = full.bytes.size();
  for (size_t limit = 1; limit <= n + 1; limit++) {
    o.max_output_bytes = limit;
    Collect c;
    GTEXT_Markup_Sink s = c.sink();
    GTEXT_HTML_Serialize_Result r;
    const GTEXT_HTML_Status st = gtext_html_serialize(p.top(), CHILDREN, &o, &s, &r);
    if (limit >= n) {
      EXPECT_EQ(st, GTEXT_HTML_OK) << limit;
      EXPECT_EQ(c.data, full.bytes);
    }
    else {
      EXPECT_EQ(st, GTEXT_HTML_E_LIMIT_OUTPUT) << limit;
      EXPECT_EQ(r.status, GTEXT_HTML_E_LIMIT_OUTPUT);
      EXPECT_STREQ(r.message, "output too long");
      EXPECT_LE(c.data.size(), limit);
      EXPECT_EQ(c.data, full.bytes.substr(0, c.data.size())) << limit;
      EXPECT_EQ(r.bytes_written, c.data.size());
    }
  }
  // The buffer form leaves nothing; zero is the default and not "none".
  o.max_output_bytes = n - 1;
  char * buf = reinterpret_cast<char *>(0x1);
  EXPECT_EQ(gtext_html_serialize_buffer(p.top(), CHILDREN, &o, &buf, nullptr, nullptr), GTEXT_HTML_E_LIMIT_OUTPUT);
  EXPECT_EQ(buf, reinterpret_cast<char *>(0x1));
  o.max_output_bytes = 0;
  EXPECT_EQ(serialize_with(p.top(), CHILDREN, o).bytes, full.bytes);
  // Not just a walk: a long text, and a head held for the meta, are counted too.
  Opts q = defaults();
  q.max_output_bytes = 100;
  Build b;
  Node * d = b.el(nullptr, "div");
  b.text(d, std::string(1000, 'x'));
  Collect c;
  GTEXT_Markup_Sink s = c.sink();
  EXPECT_EQ(gtext_html_serialize(d, OUTER, &q, &s, nullptr), GTEXT_HTML_E_LIMIT_OUTPUT);
  EXPECT_LE(c.data.size(), 100u);
  q.inject_meta_charset = true;
  q.encoding = "utf-8";
  Build h;
  Node * head = h.el(h.el(nullptr, "html"), "head");
  h.text(head, std::string(1000, 'y'));
  Collect c2;
  GTEXT_Markup_Sink s2 = c2.sink();
  EXPECT_EQ(gtext_html_serialize(head, OUTER, &q, &s2, nullptr), GTEXT_HTML_E_LIMIT_OUTPUT);
  EXPECT_LE(c2.data.size(), 100u);
}

TEST(HtmlSerializeLimits, TheTokensHeldForAMetaAreNotCountedAsOutput) {
  // inject_meta_charset keeps a head's tokens until its end tag, and the tokens are bigger than the text they make. A head
  // whose output fits the limit exactly must not be refused for that (a find of the fuzzer).
  Parsed p = parse_document("<head><title>t</title><meta name=x content=y><!--c--></head>");
  Opts o = defaults();
  o.inject_meta_charset = true;
  o.encoding = "utf-8";
  o.omit_optional_tags = true;
  Out full = serialize_with(find(p.top(), "html"), CHILDREN, o);
  ASSERT_EQ(full.status, GTEXT_HTML_OK);
  for (size_t limit = 1; limit <= full.bytes.size() + 1; limit++) {
    o.max_output_bytes = limit;
    Collect c;
    GTEXT_Markup_Sink s = c.sink();
    const GTEXT_HTML_Status st = gtext_html_serialize(find(p.top(), "html"), CHILDREN, &o, &s, nullptr);
    if (limit >= full.bytes.size()) {
      EXPECT_EQ(st, GTEXT_HTML_OK) << limit;
      EXPECT_EQ(c.data, full.bytes);
    }
    else {
      EXPECT_EQ(st, GTEXT_HTML_E_LIMIT_OUTPUT) << limit;
      EXPECT_LE(c.data.size(), limit);
      EXPECT_EQ(c.data, full.bytes.substr(0, c.data.size()));
    }
  }
  // A head of twenty thousand one-character texts holds tokens of a few bytes each, and is bounded by the limit all the same.
  Build b;
  Node * head = b.el(b.el(nullptr, "html"), "head");
  for (int i = 0; i < 20000; i++) {
    b.text(head, "x");
  }
  o.max_output_bytes = 1000;
  Collect c;
  GTEXT_Markup_Sink s = c.sink();
  EXPECT_EQ(gtext_html_serialize(head, OUTER, &o, &s, nullptr), GTEXT_HTML_E_LIMIT_OUTPUT);
  EXPECT_LE(c.data.size(), 1000u);
}

// A chain of `depth` nested divs with a text at the bottom.
Node * chain(Build & b, size_t depth, Node * parent = nullptr) {
  Node * cur = parent;
  for (size_t i = 0; i < depth; i++) {
    cur = b.el(cur, "div");
  }
  return cur;
}

TEST(HtmlSerializeLimits, TheDepthLimitIsExactAroundTheDepthOfTheTree) {
  for (size_t depth = 1; depth <= 40; depth++) {
    Build b;
    Node * bottom = chain(b, depth);
    b.text(bottom, "t");
    for (size_t limit : {depth - 1, depth, depth + 1}) {
      if (limit == 0) {
        continue; // zero is the default, which is deeper than these trees
      }
      Opts o = defaults();
      o.max_depth = limit;
      Collect c;
      GTEXT_Markup_Sink s = c.sink();
      GTEXT_HTML_Serialize_Result r;
      const GTEXT_HTML_Status st = gtext_html_serialize(b.root(), CHILDREN, &o, &s, &r);
      if (limit >= depth) {
        EXPECT_EQ(st, GTEXT_HTML_OK) << depth << "/" << limit;
        EXPECT_EQ(c.data.size(), depth * 11 + 1);
      }
      else {
        EXPECT_EQ(st, GTEXT_HTML_E_LIMIT_DEPTH) << depth << "/" << limit;
        EXPECT_STREQ(r.message, "elements nested too deeply");
        // The prefix: the start tags down to the limit.
        std::string want;
        for (size_t i = 0; i < limit; i++) {
          want += "<div>";
        }
        EXPECT_EQ(c.data, want);
      }
    }
  }
  // Void elements, text, comments and a template's own level: only elements that stay open count.
  Opts o = defaults();
  o.max_depth = 2;
  Parsed p = parse_fragment("<div><br><img><!--c-->t<div><wbr></div></div>");
  EXPECT_EQ(serialize_with(p.top(), CHILDREN, o).status, GTEXT_HTML_OK);
  Parsed t = parse_fragment("<template><div><template><div></div></template></div></template>");
  o.max_depth = 4;
  EXPECT_EQ(serialize_with(t.top(), CHILDREN, o).status, GTEXT_HTML_OK);
  o.max_depth = 3;
  EXPECT_EQ(serialize_with(t.top(), CHILDREN, o).status, GTEXT_HTML_E_LIMIT_DEPTH);
}

TEST(HtmlSerializeLimits, ATreeOneHundredThousandDeepNeedsNoCStack) {
  const size_t depth = 100000;
  Build b;
  Node * bottom = chain(b, depth);
  b.text(bottom, "t");
  Opts o = defaults();
  o.max_depth = depth;
  Out out = serialize_with(b.root(), CHILDREN, o);
  ASSERT_EQ(out.status, GTEXT_HTML_OK);
  EXPECT_EQ(out.bytes.size(), depth * 11 + 1);
  EXPECT_EQ(out.bytes.substr(0, 10), "<div><div>");
  EXPECT_EQ(out.bytes.substr(out.bytes.size() - 12), "</div></div>");
  EXPECT_EQ(out.bytes[depth * 5], 't');
  EXPECT_EQ(out.r.nodes_visited, depth + 1);
  // ... in both scopes, with every option that keeps state on, and through a template at every level too.
  o = everything_on();
  o.max_depth = depth;
  out = serialize_with(b.root(), CHILDREN, o);
  ASSERT_EQ(out.status, GTEXT_HTML_OK);
  EXPECT_EQ(out.r.nodes_visited, depth + 1);
  out = serialize_with(gtext_markup_node_first_child(b.root()), OUTER, o);
  EXPECT_EQ(out.status, GTEXT_HTML_OK);
  // The default refuses it, with the prefix the sink got, and the limit is a value of the options.
  Collect c;
  GTEXT_Markup_Sink s = c.sink();
  GTEXT_HTML_Serialize_Result r;
  EXPECT_EQ(gtext_html_serialize(b.root(), CHILDREN, nullptr, &s, &r), GTEXT_HTML_E_LIMIT_DEPTH);
  EXPECT_EQ(c.data.size(), 1024u * 5);
  EXPECT_EQ(r.bytes_written, 1024u * 5);
  o = defaults();
  o.max_depth = depth - 1;
  EXPECT_EQ(serialize_with(b.root(), CHILDREN, o).status, GTEXT_HTML_E_LIMIT_DEPTH);
  // Wide as well as deep: a hundred thousand siblings is a loop and not a stack.
  Build w;
  Node * top = w.el(nullptr, "ul");
  for (int i = 0; i < 100000; i++) {
    w.text(w.el(top, "li"), "x");
  }
  out = serialize_with(w.root(), CHILDREN, defaults());
  ASSERT_EQ(out.status, GTEXT_HTML_OK);
  EXPECT_EQ(out.bytes.size(), 100000u * 10 + 9);
  // A tree the parser builds with its defaults fits the serializer's.
  std::string deep;
  for (int i = 0; i < 480; i++) {
    deep += "<div>";
  }
  Parsed parsed = parse_fragment(deep + "x");
  EXPECT_EQ(serialize_with(parsed.top(), CHILDREN, defaults()).status, GTEXT_HTML_OK);
}

// ---- the work is linear -----------------------------------------------------------------------------------

struct Counted {
  GTEXT_HTML_Status status;
  size_t bytes;
  size_t nodes_visited;
  size_t sink_calls;
  size_t allocations;
};

Counted counted(const Node * n, const Opts & base, GTEXT_HTML_Serialize_Scope scope = CHILDREN) {
  FaultAllocator fa;
  Opts o = base;
  o.allocator = &fa.alloc;
  fa.arm(0);
  Collect c;
  GTEXT_Markup_Sink s = c.sink();
  GTEXT_HTML_Serialize_Result r;
  Counted out;
  out.status = gtext_html_serialize(n, scope, &o, &s, &r);
  out.bytes = c.data.size();
  out.nodes_visited = r.nodes_visited;
  out.sink_calls = c.calls;
  out.allocations = fa.calls;
  EXPECT_TRUE(fa.live.empty());
  return out;
}

TEST(HtmlSerializeLimits, TheWorkIsLinearInTheSizeOfTheTree) {
  // Wide: n children. Deep: a chain of n. Both: nodes visited is the number of nodes, the sink is called once per 4 KiB,
  // and the allocations are a constant plus the doublings of two arrays, never one per node.
  size_t previous_allocations = 0;
  for (size_t n : {1000u, 4000u, 16000u, 64000u}) {
    Build wide;
    Node * ul = wide.el(nullptr, "ul");
    for (size_t i = 0; i < n; i++) {
      Node * li = wide.el(ul, "li");
      wide.attr(li, "a", "1");
      wide.text(li, "x");
    }
    const size_t nodes = gtext_markup_document_node_count(wide.doc) - 1; // not the document
    Opts o = defaults();
    Counted w = counted(wide.root(), o);
    ASSERT_EQ(w.status, GTEXT_HTML_OK);
    EXPECT_EQ(w.nodes_visited, nodes);
    EXPECT_EQ(w.sink_calls, (w.bytes + 4095) / 4096);
    EXPECT_LE(w.allocations, 12u) << n;   // the serializer, the stack, the walk frames, once or doubled
    // With the options that keep tokens or sort: still not one per node.
    Counted all = counted(wide.root(), everything_on());
    ASSERT_EQ(all.status, GTEXT_HTML_OK);
    EXPECT_EQ(all.nodes_visited, nodes);
    EXPECT_LE(all.allocations, 24u) << n;

    Build deep;
    Node * bottom = chain(deep, n);
    deep.text(bottom, "t");
    Opts d = defaults();
    d.max_depth = n;
    Counted c = counted(deep.root(), d);
    ASSERT_EQ(c.status, GTEXT_HTML_OK);
    EXPECT_EQ(c.nodes_visited, n + 1);
    EXPECT_EQ(c.bytes, n * 11 + 1);
    EXPECT_LE(c.allocations, 12u + 2 * 17) << n; // two arrays grown by doubling to 2^17 at most
    // Doubling the tree adds a constant number of allocations, not a multiple.
    if (previous_allocations != 0) {
      EXPECT_LE(c.allocations, previous_allocations + 4) << n;
    }
    previous_allocations = c.allocations;
  }
}

TEST(HtmlSerializeLimits, ManyAttributesAreOneSortAndNotOneEach) {
  Build b;
  Node * e = b.el(nullptr, "div");
  for (int i = 0; i < 20000; i++) {
    b.attr(e, ("a" + std::to_string(20000 - i)).c_str(), "v");
  }
  Opts o = defaults();
  o.alphabetical_attributes = true;
  Counted c = counted(b.root(), o);
  ASSERT_EQ(c.status, GTEXT_HTML_OK);
  EXPECT_LE(c.allocations, 12u);
  Out out = serialize_with(b.root(), CHILDREN, o);
  EXPECT_EQ(out.bytes.rfind("<div a1=\"v\" a10=\"v\" a100=\"v\" a1000=\"v\" a10000=\"v\" a10001=\"v\"", 0), 0u);
}

// ---- the file form --------------------------------------------------------------------------------------------

std::string slurp(const std::string & path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TEST(HtmlSerializeFile, WritesAtomicallyAndAFailureLeavesThePreviousFile) {
  Parsed p = parse_document("<!DOCTYPE html><html><head></head><body><p class=a>x &amp; y</body></html>");
  const std::string path = "/tmp/ghoti-html-serialize-file-" + std::to_string(getpid()) + ".html";
  std::remove(path.c_str());
  GTEXT_HTML_Serialize_Result r;
  ASSERT_EQ(gtext_html_serialize_file(p.top(), CHILDREN, path.c_str(), nullptr, &r), GTEXT_HTML_OK);
  EXPECT_EQ(slurp(path), "<!DOCTYPE html><html><head></head><body><p class=\"a\">x &amp; y</p></body></html>");
  EXPECT_EQ(r.bytes_written, slurp(path).size());
  EXPECT_EQ(r.status, GTEXT_HTML_OK);
  // The outer scope, of an element.
  ASSERT_EQ(gtext_html_serialize_file(find(p.top(), "p"), OUTER, path.c_str(), nullptr, nullptr), GTEXT_HTML_OK);
  EXPECT_EQ(slurp(path), "<p class=\"a\">x &amp; y</p>");
  // A failure of the serializer leaves the file as it was.
  Opts o = defaults();
  o.max_output_bytes = 5;
  EXPECT_EQ(gtext_html_serialize_file(p.top(), CHILDREN, path.c_str(), &o, &r), GTEXT_HTML_E_LIMIT_OUTPUT);
  EXPECT_EQ(r.status, GTEXT_HTML_E_LIMIT_OUTPUT);
  EXPECT_EQ(r.bytes_written, 0u); // nothing was committed, so nothing is counted
  EXPECT_EQ(slurp(path), "<p class=\"a\">x &amp; y</p>");
  o = defaults();
  o.max_depth = 1;
  EXPECT_EQ(gtext_html_serialize_file(p.top(), CHILDREN, path.c_str(), &o, &r), GTEXT_HTML_E_LIMIT_DEPTH);
  EXPECT_EQ(slurp(path), "<p class=\"a\">x &amp; y</p>");
  // A bad option is refused before anything is created, and so is the other bad request.
  const std::string fresh = path + ".new";
  o = defaults();
  o.space_before_trailing_solidus = true;
  EXPECT_EQ(gtext_html_serialize_file(p.top(), CHILDREN, fresh.c_str(), &o, &r), GTEXT_HTML_E_OPTIONS);
  EXPECT_EQ(r.status, GTEXT_HTML_E_OPTIONS);
  EXPECT_NE(access(fresh.c_str(), F_OK), 0);
  o = defaults();
  o.encoding = "no-such-encoding";
  EXPECT_EQ(gtext_html_serialize_file(p.top(), CHILDREN, fresh.c_str(), &o, &r), GTEXT_HTML_E_UNSUPPORTED_ENCODING);
  EXPECT_NE(access(fresh.c_str(), F_OK), 0);
  EXPECT_EQ(gtext_html_serialize_file(nullptr, CHILDREN, fresh.c_str(), nullptr, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serialize_file(p.top(), CHILDREN, nullptr, nullptr, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(r.status, GTEXT_HTML_E_INVALID);
  // A path that cannot be opened is an invalid argument, as for the other formats.
  EXPECT_EQ(gtext_html_serialize_file(p.top(), CHILDREN, "/nonexistent-directory/x.html", nullptr, &r), GTEXT_HTML_E_INVALID);
  EXPECT_NE(r.message, nullptr);
  // A destination that cannot be replaced (a directory) fails the commit: an error status, no counts, no new file.
  std::memset(&r, 0xAB, sizeof(r));
  const GTEXT_HTML_Status dst = gtext_html_serialize_file(p.top(), CHILDREN, "/tmp", nullptr, &r);
  EXPECT_NE(dst, GTEXT_HTML_OK);
  EXPECT_EQ(r.status, dst);
  EXPECT_EQ(r.bytes_written, 0u);
  EXPECT_EQ(r.nodes_visited, 0u);
  EXPECT_NE(r.message, nullptr);
  std::remove(path.c_str());
  std::remove(fresh.c_str());
}

// ---- arguments of the entry points -----------------------------------------------------------------------------

TEST(HtmlSerializeArguments, TheEntryPointsRefuseWhatTheyCannotUse) {
  Parsed p = parse_fragment("<p>x</p>");
  Collect c;
  GTEXT_Markup_Sink s = c.sink();
  GTEXT_Markup_Sink nowrite = {nullptr, nullptr};
  GTEXT_HTML_Serialize_Result r;
  EXPECT_EQ(gtext_html_serialize(nullptr, CHILDREN, nullptr, &s, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(r.status, GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serialize(p.top(), CHILDREN, nullptr, nullptr, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serialize(p.top(), CHILDREN, nullptr, &nowrite, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serialize(p.top(), (GTEXT_HTML_Serialize_Scope)2, nullptr, &s, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serialize(p.top(), (GTEXT_HTML_Serialize_Scope)-1, nullptr, &s, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(c.calls, 0u);
  char * buf = reinterpret_cast<char *>(0x1);
  EXPECT_EQ(gtext_html_serialize_buffer(nullptr, CHILDREN, nullptr, &buf, nullptr, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serialize_buffer(p.top(), CHILDREN, nullptr, nullptr, nullptr, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(buf, reinterpret_cast<char *>(0x1));
  // NULL result is always allowed, and an empty output is a buffer with its NUL.
  Parsed e = parse_fragment("");
  size_t len = 99;
  ASSERT_EQ(gtext_html_serialize_buffer(e.top(), CHILDREN, nullptr, &buf, &len, nullptr), GTEXT_HTML_OK);
  EXPECT_EQ(len, 0u);
  EXPECT_EQ(buf[0], '\0');
  gtext_html_serialize_buffer_free(e.doc, nullptr, buf);
  // Free: NULL buffer, NULL doc with an allocator, and neither.
  gtext_html_serialize_buffer_free(nullptr, nullptr, nullptr);
  FaultAllocator fa;
  Opts o = defaults();
  o.allocator = &fa.alloc;
  ASSERT_EQ(gtext_html_serialize_buffer(p.top(), CHILDREN, &o, &buf, &len, nullptr), GTEXT_HTML_OK);
  EXPECT_EQ(fa.live.size(), 1u);
  // The buffer carries its allocator, so it is returned correctly whatever doc and options the free is given.
  gtext_html_serialize_buffer_free(nullptr, nullptr, buf);
  EXPECT_TRUE(fa.live.empty());
  // A node of no kind.
  EXPECT_EQ(gtext_html_serialize(p.top(), CHILDREN, nullptr, &s, nullptr), GTEXT_HTML_OK);
}

// ---- threads -------------------------------------------------------------------------------------------------------

TEST(HtmlSerializeConcurrency, IndependentCallsOnOneTreeAgree) {
  Parsed p = parse_document(big_html());
  const Opts variants[] = {defaults(), everything_on()};
  std::string want[2];
  for (int i = 0; i < 2; i++) {
    want[i] = serialize_with(p.top(), CHILDREN, variants[i]).bytes;
  }
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 12; i++) {
        const int v = (t + i) % 2;
        Out out = serialize_with(p.top(), (i % 3 == 0) ? OUTER : CHILDREN, variants[v]);
        if (out.status != GTEXT_HTML_OK || out.bytes != want[v]) {
          wrong++;
        }
        // A stream of its own.
        Collect c;
        GTEXT_Markup_Sink s = c.sink();
        GTEXT_HTML_Serializer * ser = nullptr;
        if (gtext_html_serializer_new(&variants[v], &s, &ser) != GTEXT_HTML_OK) {
          wrong++;
          continue;
        }
        gtext_html_serializer_start_tag(ser, nullptr, "p", nullptr, 0);
        gtext_html_serializer_characters(ser, "x<y", 3);
        gtext_html_serializer_end_tag(ser, nullptr, "p");
        gtext_html_serializer_finish(ser, nullptr);
        gtext_html_serializer_free(ser);
        if (c.data.find("x&lt;y") == std::string::npos) {
          wrong++;
        }
      }
    });
  }
  for (std::thread & th : threads) {
    th.join();
  }
  EXPECT_EQ(wrong.load(), 0);
}

} // namespace
