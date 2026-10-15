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
 * The XML event parsers: GTEXT_XML_Stream (push) and GTEXT_XML_Reader (pull).
 *
 * The claim is that they are gtext_xml_parse() delivered in pieces: the same
 * verdicts, positions, limits, namespaces and encodings, and an event sequence
 * that does not depend on where the input was cut. So almost everything here is
 * a comparison with the DOM parse, over input the generator makes:
 *
 *  - a generated document, cut every way the test can think of - whole, a byte
 *    at a time, at every single offset, at random sizes - gives the same events
 *    (compared as text, locations included) and, replayed into a tree, a tree
 *    equal to gtext_xml_parse()'s;
 *  - the same documents damaged by one byte, and every proper prefix of them,
 *    give the DOM parse's status, offset, line, column and message, however cut;
 *  - the same documents in UTF-16, UTF-32, ISO-8859-1 and US-ASCII;
 *  - and the things only an incremental parser can get wrong: a CR LF cut
 *    between the two, a `]]>` cut, a multi-byte character cut, a surrogate pair
 *    cut, an XML declaration still arriving when the encoding has to be known,
 *    and work that is quadratic in the length of a construct.
 *
 * The generator is seeded, so a failure names its seed and reproduces.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>

#include "../src/markup/markup_internal.h"
#include "../src/xml/xml_internal.h"
#include "markup_test_support.h"
#include "xml_replay.h"

namespace {

// ---- what an event looks like as text -------------------------------------

std::string escape(const char * s, size_t n) {
  std::string out;
  char buf[8];
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    if (c == '\\') {
      out += "\\\\";
    }
    else if (c >= 0x20 && c < 0x7F) {
      out += (char)c;
    }
    else {
      std::snprintf(buf, sizeof(buf), "\\x%02X", c);
      out += buf;
    }
  }
  return out;
}

std::string opt(const char * s) {
  return s ? "'" + escape(s, std::strlen(s)) + "'" : "-";
}

std::string where(const GTEXT_Markup_Location & l) {
  return std::to_string(l.offset) + ":" + std::to_string(l.line) + ":" + std::to_string(l.column);
}

std::string describe(const GTEXT_XML_Event & e) {
  std::string out;
  switch (e.type) {
    case GTEXT_XML_EVENT_DOCUMENT_START:
      out = "DOCUMENT_START version=" + opt(e.version) + " encoding=" + opt(e.encoding)
          + " source=" + std::to_string((int)e.encoding_source)
          + " standalone=" + std::to_string((int)e.standalone);
      break;
    case GTEXT_XML_EVENT_DOCTYPE:
      out = "DOCTYPE " + opt(e.name) + " " + opt(e.public_id) + " " + opt(e.system_id);
      break;
    case GTEXT_XML_EVENT_ELEMENT_START:
      out = "START {" + opt(e.namespace_uri) + "}" + opt(e.prefix) + ":" + opt(e.local_name);
      for (size_t i = 0; i < e.attribute_count; i++) {
        const GTEXT_XML_Event_Attribute & a = e.attributes[i];
        out += " @{" + opt(a.namespace_uri) + "}" + opt(a.prefix) + ":" + opt(a.local_name) + "='"
            + escape(a.value, a.value_length) + "'@" + where(a.location);
        EXPECT_EQ(a.value[a.value_length], '\0');
      }
      break;
    case GTEXT_XML_EVENT_ELEMENT_END:
      out = "END {" + opt(e.namespace_uri) + "}" + opt(e.prefix) + ":" + opt(e.local_name);
      break;
    case GTEXT_XML_EVENT_TEXT:
      out = "TEXT '" + escape(e.data, e.data_length) + "'";
      EXPECT_EQ(e.data[e.data_length], '\0');
      break;
    case GTEXT_XML_EVENT_CDATA:
      out = "CDATA '" + escape(e.data, e.data_length) + "'";
      break;
    case GTEXT_XML_EVENT_COMMENT:
      out = "COMMENT '" + escape(e.data, e.data_length) + "'";
      break;
    case GTEXT_XML_EVENT_PROCESSING_INSTRUCTION:
      out = "PI " + opt(e.name) + " '" + escape(e.data, e.data_length) + "'";
      break;
    case GTEXT_XML_EVENT_DOCUMENT_END:
      out = "DOCUMENT_END";
      break;
  }
  return out + " @" + where(e.location);
}

// ---- one run through the push parser --------------------------------------

struct Recorder {
  std::vector<std::string> log;
  xml_replay replay;
  bool replaying = true;
  size_t stop_after = SIZE_MAX; // return an error after this many events
  GTEXT_XML_Status stop_with = GTEXT_XML_E_INVALID;
  Recorder() { xml_replay_init(&replay); }
  ~Recorder() { xml_replay_free(&replay); }
  Recorder(const Recorder &) = delete;
  Recorder & operator=(const Recorder &) = delete;
};

GTEXT_XML_Status record(const GTEXT_XML_Event * e, void * user) {
  auto * r = static_cast<Recorder *>(user);
  if (r->log.size() >= r->stop_after) {
    return r->stop_with;
  }
  r->log.push_back(describe(*e));
  if (r->replaying) {
    return xml_replay_event(e, &r->replay);
  }
  return GTEXT_XML_OK;
}

struct Parsed {
  GTEXT_XML_Status status = GTEXT_XML_OK;
  GTEXT_XML_Error err{};
  Recorder rec;
  size_t delivered = 0; // events delivered by feed() alone, before finish()
};

// The sizes of the pieces: it is cycled until the input is used up. Zero-length
// entries are legal and feed nothing.
using Chunks = std::vector<size_t>;

void run_stream(Parsed & run, const std::string & data, const Chunks & chunks,
    const GTEXT_XML_Parse_Options * opts = nullptr, bool eager = false) {
  GTEXT_XML_Stream * s = gtext_xml_stream_new(opts, record, &run.rec);
  ASSERT_NE(s, nullptr);
  xml_stream_set_eager(s, eager);
  size_t at = 0;
  size_t k = 0;
  run.status = GTEXT_XML_OK;
  while (at < data.size() && run.status == GTEXT_XML_OK) {
    size_t n = chunks.empty() ? data.size() : chunks[k++ % chunks.size()];
    if (n > data.size() - at) {
      n = data.size() - at;
    }
    run.status = gtext_xml_stream_feed(s, data.data() + at, n, &run.err);
    at += n;
  }
  run.delivered = run.rec.log.size();
  if (run.status == GTEXT_XML_OK) {
    run.status = gtext_xml_stream_finish(s, &run.err);
  }
  gtext_xml_stream_free(s);
}

struct Dom {
  GTEXT_XML_Status status = GTEXT_XML_OK;
  GTEXT_XML_Error err{};
  GTEXT_Markup_Document * doc = nullptr;
  ~Dom() {
    if (doc) {
      gtext_markup_document_free(doc);
    }
  }
};

void run_dom(Dom & dom, const std::string & data, const GTEXT_XML_Parse_Options * opts = nullptr) {
  dom.status = gtext_xml_parse(data.data(), data.size(), opts, &dom.doc, &dom.err);
}

// The event parser says what the DOM parse says: the same refusal, with the same
// message and position, or the same tree.
void expect_agrees(const std::string & what, const Parsed & run, const Dom & dom) {
  ASSERT_EQ(run.status, dom.status) << what;
  if (dom.status != GTEXT_XML_OK) {
    EXPECT_EQ(run.err.code, dom.err.code) << what;
    EXPECT_STREQ(run.err.message, dom.err.message) << what;
    EXPECT_EQ(run.err.byte_offset, dom.err.byte_offset) << what;
    EXPECT_EQ(run.err.line, dom.err.line) << what;
    EXPECT_EQ(run.err.column, dom.err.column) << what;
    return;
  }
  ASSERT_EQ(run.rec.replay.problem, nullptr) << what;
  ASSERT_NE(run.rec.replay.doc, nullptr) << what;
  EXPECT_TRUE(run.rec.replay.ended) << what;
  const char * difference = xml_replay_compare(run.rec.replay.doc, dom.doc);
  EXPECT_EQ(difference, nullptr) << what << ": " << (difference ? difference : "");
  const char * problem = markup_check_invariants(run.rec.replay.doc);
  EXPECT_STREQ(problem ? problem : "ok", "ok") << what;
}

std::vector<std::string> drain(GTEXT_XML_Reader * r, GTEXT_XML_Status * last);

std::string show(const std::string & data) {
  return "\"" + escape(data.data(), data.size()) + "\"";
}

std::string utf16(const std::string & utf8, bool big, bool bom);
std::string utf32(const std::string & utf8, bool big, bool bom);

// ---- a generator of documents ---------------------------------------------

class Gen {
 public:
  Gen(uint32_t seed, bool narrow = false, bool declaration = true)
      : rng(seed), narrow(narrow), with_declaration(declaration) {}

  std::string make() {
    out.clear();
    if (with_declaration && chance(50)) {
      out += "<?xml version=";
      const char q = chance(50) ? '"' : '\'';
      out += q;
      out += "1.0";
      out += q;
      if (chance(40)) {
        out += " encoding=";
        out += q;
        out += "UTF-8";
        out += q;
      }
      if (chance(40)) {
        out += " standalone=";
        out += q;
        out += chance(50) ? "yes" : "no";
        out += q;
      }
      out += chance(50) ? "?>" : " ?>";
    }
    misc();
    if (chance(35)) {
      doctype();
      misc();
    }
    scope_depth = 0;
    element(0, true);
    misc();
    return out;
  }

 private:
  std::mt19937 rng;
  std::string out;
  bool narrow;
  bool with_declaration;
  int scope_depth = 0;
  std::vector<bool> has_r; // is the prefix r bound, per open element

  bool chance(int percent) { return static_cast<int>(rng() % 100) < percent; }
  size_t below(size_t n) { return rng() % n; }

  void space(bool required) {
    static const char * const forms[] = {" ", "  ", "\n", "\r\n", "\t", "\r", " \n "};
    if (required || chance(30)) {
      out += forms[below(7)];
    }
  }

  void misc() {
    while (chance(30)) {
      switch (below(3)) {
        case 0:
          out += chance(50) ? "\n" : "\r\n";
          break;
        case 1:
          comment();
          break;
        default:
          pi();
          break;
      }
    }
  }

  void comment() {
    out += "<!--";
    std::string body;
    const size_t n = below(12);
    for (size_t i = 0; i < n; i++) {
      static const char * const bits = "ab -> ?]<&;\n\r";
      const char c = bits[below(std::strlen(bits))];
      if (c == '-' && (body.empty() || body.back() == '-')) {
        body += 'x';
      }
      else {
        body += c;
      }
    }
    if (!body.empty() && body.back() == '-') {
      body += 'x';
    }
    if (chance(25)) {
      body += "\xC3\xA9";
    }
    out += body;
    out += "-->";
  }

  void pi() {
    static const char * const targets[] = {"pi", "xsl-x", "a.b", "p_q"};
    out += "<?";
    out += targets[below(4)];
    if (chance(70)) {
      out += chance(50) ? " " : "\r\n";
      const size_t n = below(8);
      for (size_t i = 0; i < n; i++) {
        static const char * const bits = "ab ?>\"'&<]\n";
        char c = bits[below(std::strlen(bits))];
        // "?>" ends it.
        if (c == '>' && !out.empty() && out.back() == '?') {
          c = '.';
        }
        out += c;
      }
      if (!out.empty() && out.back() == '?') {
        out += ' ';
      }
    }
    out += "?>";
  }

  void doctype() {
    out += "<!DOCTYPE root";
    switch (below(4)) {
      case 0:
        break;
      case 1:
        out += " SYSTEM \"a.dtd\"";
        break;
      case 2:
        out += " PUBLIC \"-//X//Y z\" 'b.dtd'";
        break;
      default:
        break;
    }
    if (chance(60)) {
      out += " [";
      const size_t n = below(5);
      for (size_t i = 0; i < n; i++) {
        space(false);
        switch (below(7)) {
          case 0:
            out += "<!ENTITY e \"a>b'c]\">";
            break;
          case 1:
            out += "<!-- ] > <!ENTITY --> ";
            break;
          case 2:
            out += "<?pi x ] > ?>";
            break;
          case 3:
            out += "<!ELEMENT root ANY>";
            break;
          case 4:
            out += "<!ATTLIST root a CDATA 'x>y' b CDATA \"]\">";
            break;
          case 5:
            out += "<!ENTITY % pe 'v'>";
            break;
          default:
            out += "%pe;";
            break;
        }
      }
      space(false);
      out += "]";
      space(false);
    }
    out += ">";
  }

  void add_text_char(std::string & s, bool in_attribute, char quote) {
    static const char * const plain =
        "abc xyz 012 .,;:!?()[]{}|~^*+=-_/\\%$#@ \t\n";
    const size_t pick = below(24);
    switch (pick) {
      case 0:
        s += "&amp;";
        return;
      case 1:
        s += "&lt;";
        return;
      case 2:
        s += "&gt;";
        return;
      case 3:
        s += in_attribute && quote == '"' ? "&quot;" : "\"";
        return;
      case 4:
        s += in_attribute && quote == '\'' ? "&apos;" : "'";
        return;
      case 5:
        s += "&#65;";
        return;
      case 6:
        s += "&#x42;";
        return;
      case 7:
        s += narrow ? "&#xE9;" : "&#x1F600;";
        return;
      case 8:
        s += "&#13;";
        return;
      case 9:
        s += "&#10;";
        return;
      case 10:
        s += "\r\n";
        return;
      case 11:
        s += "\r";
        return;
      case 12:
        s += "\xC3\xA9";
        return;
      case 13:
        if (!narrow) {
          s += "\xE6\x97\xA5";
        }
        return;
      case 14:
        if (!narrow) {
          s += "\xF0\x9F\x98\x80";
        }
        return;
      case 15:
        s += "]";
        return;
      case 16:
        // "]]" is fine; "]]>" is not, so a ">" follows only a single "]".
        if (!(s.size() >= 1 && s.back() == ']')) {
          s += "]]";
        }
        return;
      case 17:
        if (!(s.size() >= 2 && s[s.size() - 1] == ']' && s[s.size() - 2] == ']')) {
          s += ">";
        }
        return;
      case 18:
        if (in_attribute) {
          s += "\t";
          return;
        }
        break;
      default:
        break;
    }
    s += plain[below(std::strlen(plain))];
  }

  void text() {
    std::string s;
    const size_t n = 1 + below(10);
    for (size_t i = 0; i < n; i++) {
      add_text_char(s, false, 0);
    }
    out += s;
  }

  void cdata() {
    out += "<![CDATA[";
    const size_t n = below(8);
    std::string body;
    for (size_t i = 0; i < n; i++) {
      static const char * const bits = "ab <>&]\n\r;\"'";
      body += bits[below(std::strlen(bits))];
      if (body.size() >= 3 && body.compare(body.size() - 3, 3, "]]>") == 0) {
        body.back() = ' ';
      }
    }
    if (chance(20)) {
      body += narrow ? "\xC3\xA9" : "\xC3\xA9\xE6\x97\xA5";
    }
    out += body;
    out += "]]>";
  }

  void attribute_value() {
    const char q = chance(50) ? '"' : '\'';
    out += q;
    std::string s;
    const size_t n = below(8);
    for (size_t i = 0; i < n; i++) {
      add_text_char(s, true, q);
    }
    // Neither "<" nor the quote may appear raw; add_text_char never writes "<".
    out += s;
    out += q;
  }

  void element(int depth, bool root) {
    static const char * const names[] = {"a", "item", "x1", "n-m", "e.f", "\xC3\xA9t\xC3\xA9"};
    static const char * const attrs[] = {"id", "class", "p:at", "q:at", "xml:lang", "k1", "k-2", "r:at"};
    std::string name = names[below(narrow ? 5 : 6)];
    if (narrow && name.find('\xC3') != std::string::npos) {
      name = "a";
    }
    bool bind_r = chance(20);
    has_r.push_back(has_r.empty() ? bind_r : (bind_r || has_r.back()));
    const char * prefix = nullptr;
    switch (below(5)) {
      case 0:
        prefix = "p";
        break;
      case 1:
        prefix = "q";
        break;
      case 2:
        if (has_r.back()) {
          prefix = "r";
        }
        break;
      default:
        break;
    }
    out += "<";
    if (prefix) {
      out += prefix;
      out += ":";
    }
    const std::string written = name;
    out += written;
    std::string full = (prefix ? std::string(prefix) + ":" : std::string()) + written;
    std::vector<std::string> used;
    // A namespace declaration has a fixed value, so a prefix never comes to
    // stand for two names that are equal.
    auto attr = [&](const std::string & n, const char * fixed = nullptr) {
      for (const std::string & u : used) {
        if (u == n) {
          return;
        }
      }
      used.push_back(n);
      space(true);
      out += n;
      space(false);
      out += "=";
      space(false);
      if (fixed) {
        const char q = chance(50) ? '\'' : '"';
        out += q;
        out += fixed;
        out += q;
      }
      else {
        attribute_value();
      }
    };
    if (root) {
      attr("xmlns:p", "urn:p");
      attr("xmlns:q", "urn:q");
      if (chance(50)) {
        attr("xmlns", "urn:default");
      }
    }
    if (bind_r) {
      attr("xmlns:r", chance(50) ? "urn:r1" : "urn:r2");
    }
    if (!root && chance(15)) {
      attr("xmlns", chance(50) ? "" : "urn:inner");
    }
    const size_t n = below(4);
    for (size_t i = 0; i < n; i++) {
      const std::string a = attrs[below(8)];
      if (a == "r:at" && !has_r.back()) {
        continue;
      }
      attr(a);
    }
    if (depth >= 5 || chance(25)) {
      space(false);
      out += "/>";
      has_r.pop_back();
      return;
    }
    space(false);
    out += ">";
    const size_t kids = below(5);
    for (size_t i = 0; i < kids; i++) {
      switch (below(7)) {
        case 0:
        case 1:
          text();
          break;
        case 2:
        case 3:
          element(depth + 1, false);
          break;
        case 4:
          comment();
          break;
        case 5:
          pi();
          break;
        default:
          cdata();
          break;
      }
    }
    out += "</";
    out += full;
    space(false);
    out += ">";
    has_r.pop_back();
  }
};

// ---- ways to cut an input -------------------------------------------------

Chunks random_chunks(std::mt19937 & rng, size_t max_size) {
  Chunks c;
  for (int i = 0; i < 64; i++) {
    c.push_back(rng() % (max_size + 1));
  }
  c.push_back(1);
  return c;
}

// ---- the matrix, one row at a time ----------------------------------------

TEST(XmlStream, TheEventsOfAKnownDocument) {
  const std::string doc =
      "<?xml version='1.0' encoding='utf-8' standalone=\"yes\"?>\n"
      "<!DOCTYPE r PUBLIC \"pub\" 'sys' [ <!ENTITY e \"x\"> ]>\n"
      "<!-- c -->"
      "<r xmlns='urn:d' xmlns:p=\"urn:p\" a='1' p:b=\"2\">"
      "t&amp;u<p:e/><![CDATA[<c>]]><?pi data?></r>"
      "<?after ?>";
  Parsed run;
  run_stream(run, doc, {});
  ASSERT_EQ(run.status, GTEXT_XML_OK);
  // Pin the sequence by kind and by the fields that are easy to get wrong,
  // rather than by every offset (which the DOM comparison below checks).
  std::vector<std::string> kinds;
  for (const std::string & line : run.rec.log) {
    kinds.push_back(line.substr(0, line.find(' ')));
  }
  EXPECT_EQ(kinds,
      (std::vector<std::string>{"DOCUMENT_START", "DOCTYPE", "COMMENT", "START", "TEXT",
          "START", "END", "CDATA", "PI", "END", "PI", "DOCUMENT_END"}));
  EXPECT_EQ(run.rec.log[0].substr(0, 70),
      "DOCUMENT_START version='1.0' encoding='utf-8' source=3 standalone=2 @0");
  EXPECT_NE(run.rec.log[1].find("DOCTYPE 'r' 'pub' 'sys' @"), std::string::npos);
  EXPECT_NE(run.rec.log[2].find("COMMENT ' c ' @"), std::string::npos);
  EXPECT_NE(run.rec.log[3].find("START {'urn:d'}-:'r' @{'http://www.w3.org/2000/xmlns/'}-:'xmlns'='urn:d'@"),
      std::string::npos);
  EXPECT_NE(run.rec.log[3].find("@{'http://www.w3.org/2000/xmlns/'}'xmlns':'p'='urn:p'@"), std::string::npos);
  EXPECT_NE(run.rec.log[3].find("@{-}-:'a'='1'@"), std::string::npos);
  EXPECT_NE(run.rec.log[3].find("@{'urn:p'}'p':'b'='2'@"), std::string::npos);
  EXPECT_NE(run.rec.log[4].find("TEXT 't&u' @"), std::string::npos);
  EXPECT_NE(run.rec.log[5].find("START {'urn:p'}'p':'e' @"), std::string::npos);
  EXPECT_NE(run.rec.log[6].find("END {'urn:p'}'p':'e' @"), std::string::npos);
  EXPECT_NE(run.rec.log[7].find("CDATA '<c>' @"), std::string::npos);
  EXPECT_NE(run.rec.log[8].find("PI 'pi' 'data' @"), std::string::npos);
  EXPECT_NE(run.rec.log[9].find("END {'urn:d'}-:'r' @"), std::string::npos);
  EXPECT_NE(run.rec.log[10].find("PI 'after' '' @"), std::string::npos);
  Dom dom;
  run_dom(dom, doc);
  expect_agrees("known document", run, dom);
}

TEST(XmlStream, Xml11ProfileAndCharacterReferencesSurviveChunking) {
  const std::string doc = "<?xml version='1.1'?><a>&#x1;&#x85;</a>";
  Parsed run;
  run_stream(run, doc, Chunks{1});
  ASSERT_EQ(run.status, GTEXT_XML_OK);
  ASSERT_FALSE(run.rec.log.empty());
  EXPECT_NE(run.rec.log[0].find("DOCUMENT_START version='1.1'"), std::string::npos);
  EXPECT_NE(run.rec.log[2].find("TEXT '\\x01\\xC2\\x85'"), std::string::npos);

  Parsed bad;
  run_stream(bad, "<?xml version='1.1'?><a>\xC2\x86</a>", Chunks{1});
  EXPECT_EQ(bad.status, GTEXT_XML_E_CHARACTER);
  EXPECT_EQ(bad.err.byte_offset, 24u);

  for (const std::string & body : {
           std::string("<a v='\x7F'/>"), std::string("<a><!--\x7F--></a>"),
           std::string("<a><?p \x7F?></a>"),
           std::string("<a><![CDATA[\x7F]]></a>")}) {
    Parsed restricted;
    run_stream(restricted, "<?xml version='1.1'?>" + body, Chunks{1});
    EXPECT_EQ(restricted.status, GTEXT_XML_E_CHARACTER);
  }
}

TEST(XmlStream, Xml11LineEndsAgreeAcrossDomPushAndPullReaders) {
  const std::string doc =
      "<?xml version='1.1'?><a v='q\r\n\r\xC2\x85\xE2\x80\xA8z'>"
      "x\r\ny\r\xC2\x85z\xC2\x85w\xE2\x80\xA8v&#x85;"
      "<!--c\xC2\x85" "d\xE2\x80\xA8" "e--><?p x\xC2\x85y\xE2\x80\xA8z?>"
      "<![CDATA[m\xC2\x85n\xE2\x80\xA8o]]></a>";
  Dom dom;
  run_dom(dom, doc);
  ASSERT_EQ(dom.status, GTEXT_XML_OK) << dom.err.message;

  Parsed push;
  run_stream(push, doc, Chunks{1});
  ASSERT_EQ(push.status, GTEXT_XML_OK) << push.err.message;
  expect_agrees("XML 1.1 line ends", push, dom);

  GTEXT_XML_Reader * reader = gtext_xml_reader_new(nullptr);
  ASSERT_NE(reader, nullptr);
  std::vector<std::string> pulled;
  GTEXT_XML_Status last = GTEXT_XML_OK;
  for (size_t at = 0; at < doc.size(); at++) {
    ASSERT_EQ(gtext_xml_reader_feed(reader, doc.data() + at, 1, nullptr), GTEXT_XML_OK);
    for (const std::string & event : drain(reader, &last)) {
      pulled.push_back(event);
    }
    ASSERT_EQ(last, GTEXT_XML_E_INCOMPLETE);
  }
  ASSERT_EQ(gtext_xml_reader_feed(reader, nullptr, 0, nullptr), GTEXT_XML_OK);
  for (const std::string & event : drain(reader, &last)) {
    pulled.push_back(event);
  }
  EXPECT_EQ(pulled, push.rec.log);
  gtext_xml_reader_free(reader);

  for (const std::string & prolog : {
           std::string("<!--a\r\xC2\x85" "b-->"),
           std::string("<?p a\r\xC2\x85" "b?>"),
           std::string("<!DOCTYPE a [<!--a\r\xC2\x85" "b-->] >"),
           std::string("<!DOCTYPE a [<!ELEMENT a (#PCDATA)\r\xC2\x85>]>")}) {
    const std::string malformed_after_construct =
        "<?xml version='1.1'?>" + prolog + "\n<a></b>";
    Dom construct_dom;
    run_dom(construct_dom, malformed_after_construct);
    ASSERT_EQ(construct_dom.status, GTEXT_XML_E_MISMATCHED_TAG)
        << construct_dom.err.message << " in " << show(prolog);
    EXPECT_EQ(construct_dom.err.line, 3u);
    EXPECT_EQ(construct_dom.err.column, 4u);
    Parsed construct_push;
    run_stream(construct_push, malformed_after_construct, Chunks{1});
    expect_agrees("XML 1.1 CR/NEL in a complete prolog construct", construct_push, construct_dom);
  }

  const std::string malformed = "<?xml version='1.1'?><a>x\r\xC2\x85\xE2x</a>";
  Dom bad_dom;
  run_dom(bad_dom, malformed);
  EXPECT_EQ(bad_dom.status, GTEXT_XML_E_UTF8);
  EXPECT_EQ(bad_dom.doc, nullptr);
  EXPECT_EQ(bad_dom.err.line, 2u);
  Parsed bad_push;
  run_stream(bad_push, malformed, Chunks{1});
  EXPECT_EQ(bad_push.status, bad_dom.status);
  EXPECT_EQ(bad_push.err.byte_offset, bad_dom.err.byte_offset);
  EXPECT_EQ(bad_push.err.line, bad_dom.err.line);
  EXPECT_EQ(bad_push.err.column, bad_dom.err.column);

  const std::string malformed_prefix = "<?xml version='1.1'?><a>x";
  for (const std::string & line_end : {std::string("\xC2\x85"), std::string("\xE2\x80\xA8")}) {
    const std::string malformed_after_line_end =
        malformed_prefix + line_end + "\xE2x</a>";
    Dom line_end_dom;
    run_dom(line_end_dom, malformed_after_line_end);
    ASSERT_EQ(line_end_dom.status, GTEXT_XML_E_UTF8);
    EXPECT_EQ(line_end_dom.doc, nullptr);
    EXPECT_EQ(line_end_dom.err.byte_offset, malformed_prefix.size() + line_end.size());
    EXPECT_EQ(line_end_dom.err.line, 2u);
    EXPECT_EQ(line_end_dom.err.column, 1u);
    Parsed line_end_push;
    run_stream(line_end_push, malformed_after_line_end, Chunks{1});
    EXPECT_EQ(line_end_push.status, line_end_dom.status);
    EXPECT_EQ(line_end_push.err.byte_offset, line_end_dom.err.byte_offset);
    EXPECT_EQ(line_end_push.err.line, line_end_dom.err.line);
    EXPECT_EQ(line_end_push.err.column, line_end_dom.err.column);

    GTEXT_XML_Reader * error_reader = gtext_xml_reader_new(nullptr);
    ASSERT_NE(error_reader, nullptr);
    GTEXT_XML_Error pull_error{};
    GTEXT_XML_Status pull_status = GTEXT_XML_OK;
    for (size_t at = 0; at < malformed_after_line_end.size(); at++) {
      pull_status = gtext_xml_reader_feed(
          error_reader, malformed_after_line_end.data() + at, 1, &pull_error);
      if (pull_status != GTEXT_XML_OK) {
        break;
      }
    }
    if (pull_status == GTEXT_XML_OK) {
      pull_status = gtext_xml_reader_feed(error_reader, nullptr, 0, &pull_error);
    }
    EXPECT_EQ(pull_status, line_end_dom.status);
    EXPECT_EQ(pull_error.byte_offset, line_end_dom.err.byte_offset);
    EXPECT_EQ(pull_error.line, line_end_dom.err.line);
    EXPECT_EQ(pull_error.column, line_end_dom.err.column);
    gtext_xml_reader_free(error_reader);
  }

  const std::string declaration_space =
      "<?xml\xC2\x85version='1.1'?><a\xE2\x80\xA8x='1'/>";
  Dom declaration_dom;
  run_dom(declaration_dom, declaration_space);
  ASSERT_EQ(declaration_dom.status, GTEXT_XML_OK) << declaration_dom.err.message;
  Parsed declaration_stream;
  run_stream(declaration_stream, declaration_space, Chunks{1});
  ASSERT_EQ(declaration_stream.status, GTEXT_XML_OK) << declaration_stream.err.message;
  expect_agrees("XML 1.1 declaration and markup whitespace", declaration_stream,
      declaration_dom);

  for (const std::string & encoded : {
           utf16("<?xml version='1.1' encoding='UTF-16'?><a>\r\xC2\x85\xE2\x80\xA8</a>", false, true),
           utf16("<?xml version='1.1' encoding='UTF-16'?><a>\r\xC2\x85\xE2\x80\xA8</a>", true, true),
           utf32("<?xml version='1.1' encoding='UTF-32'?><a>\r\xC2\x85\xE2\x80\xA8</a>", false, true),
           utf32("<?xml version='1.1' encoding='UTF-32'?><a>\r\xC2\x85\xE2\x80\xA8</a>", true, true)}) {
    Dom encoded_dom;
    run_dom(encoded_dom, encoded);
    ASSERT_EQ(encoded_dom.status, GTEXT_XML_OK) << encoded_dom.err.message;
    Parsed encoded_stream;
    run_stream(encoded_stream, encoded, Chunks{1});
    ASSERT_EQ(encoded_stream.status, GTEXT_XML_OK) << encoded_stream.err.message;
    expect_agrees("XML 1.1 encoded line ends", encoded_stream, encoded_dom);
  }

  for (const std::string & encoded : {
           utf16("<?xml\xC2\x85version='1.1' encoding='UTF-16'?><a/>", false, true),
           utf16("<?xml\xC2\x85version='1.1' encoding='UTF-16'?><a/>", true, true),
           utf32("<?xml\xC2\x85version='1.1' encoding='UTF-32'?><a/>", false, true),
           utf32("<?xml\xC2\x85version='1.1' encoding='UTF-32'?><a/>", true, true)}) {
    Dom declaration_dom;
    run_dom(declaration_dom, encoded);
    ASSERT_EQ(declaration_dom.status, GTEXT_XML_OK) << declaration_dom.err.message;
    Parsed declaration_push;
    run_stream(declaration_push, encoded, Chunks{1});
    ASSERT_EQ(declaration_push.status, GTEXT_XML_OK) << declaration_push.err.message;
    expect_agrees("XML 1.1 encoded declaration line end", declaration_push, declaration_dom);
  }

  std::string malformed_utf16 = utf16(
      "<?xml version='1.1' encoding='UTF-16'?><a>\xC2\x85", false, true);
  malformed_utf16 += std::string("\x3D\xD8", 2); // unpaired high surrogate
  Dom malformed_utf16_dom;
  run_dom(malformed_utf16_dom, malformed_utf16);
  ASSERT_EQ(malformed_utf16_dom.status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(malformed_utf16_dom.doc, nullptr);
  EXPECT_EQ(malformed_utf16_dom.err.line, 2u);
  EXPECT_EQ(malformed_utf16_dom.err.column, 1u);
  Parsed malformed_utf16_stream;
  run_stream(malformed_utf16_stream, malformed_utf16, Chunks{1});
  EXPECT_EQ(malformed_utf16_stream.status, malformed_utf16_dom.status);
  EXPECT_EQ(malformed_utf16_stream.err.byte_offset, malformed_utf16_dom.err.byte_offset);
  EXPECT_EQ(malformed_utf16_stream.err.line, malformed_utf16_dom.err.line);
  EXPECT_EQ(malformed_utf16_stream.err.column, malformed_utf16_dom.err.column);

  for (const bool big : {false, true}) {
    std::string malformed_utf32 = utf32(
        "<?xml version='1.1' encoding='UTF-32'?><a>\xC2\x85", big, true);
    malformed_utf32 += big ? std::string("\x00\x11\x00\x00", 4)
                            : std::string("\x00\x00\x11\x00", 4);
    Dom malformed_utf32_dom;
    run_dom(malformed_utf32_dom, malformed_utf32);
    ASSERT_EQ(malformed_utf32_dom.status, GTEXT_XML_E_INVALID_SEQUENCE);
    EXPECT_EQ(malformed_utf32_dom.doc, nullptr);
    EXPECT_EQ(malformed_utf32_dom.err.line, 2u);
    EXPECT_EQ(malformed_utf32_dom.err.column, 1u);
    Parsed malformed_utf32_stream;
    run_stream(malformed_utf32_stream, malformed_utf32, Chunks{1});
    EXPECT_EQ(malformed_utf32_stream.status, malformed_utf32_dom.status);
    EXPECT_EQ(malformed_utf32_stream.err.byte_offset, malformed_utf32_dom.err.byte_offset);
    EXPECT_EQ(malformed_utf32_stream.err.line, malformed_utf32_dom.err.line);
    EXPECT_EQ(malformed_utf32_stream.err.column, malformed_utf32_dom.err.column);
  }

  std::string malformed_after_decl_eol = utf16(
      "<?xml\xC2\x85version='1.1' encoding='UTF-16'?><a>\xC2\x85", false, true);
  malformed_after_decl_eol += std::string("\x3D\xD8", 2);
  Dom declaration_eol_dom;
  run_dom(declaration_eol_dom, malformed_after_decl_eol);
  ASSERT_EQ(declaration_eol_dom.status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(declaration_eol_dom.doc, nullptr);
  EXPECT_EQ(declaration_eol_dom.err.line, 3u);
  EXPECT_EQ(declaration_eol_dom.err.column, 1u);
  Parsed declaration_eol_stream;
  run_stream(declaration_eol_stream, malformed_after_decl_eol, Chunks{1});
  EXPECT_EQ(declaration_eol_stream.status, declaration_eol_dom.status);
  EXPECT_EQ(declaration_eol_stream.err.byte_offset, declaration_eol_dom.err.byte_offset);
  EXPECT_EQ(declaration_eol_stream.err.line, declaration_eol_dom.err.line);
  EXPECT_EQ(declaration_eol_stream.err.column, declaration_eol_dom.err.column);
}

TEST(XmlStream, TheEmptyElementTagIsAStartAndAnEnd) {
  Parsed run;
  run_stream(run, "<a><b x='1'/>\n</a>", {});
  ASSERT_EQ(run.status, GTEXT_XML_OK);
  ASSERT_EQ(run.rec.log.size(), 7u);
  EXPECT_EQ(run.rec.log[2], "START {-}-:'b' @{-}-:'x'='1'@6:1:7 @3:1:4");
  // The end of an empty-element tag is where its '/' is.
  EXPECT_EQ(run.rec.log[3], "END {-}-:'b' @11:1:12");
  EXPECT_EQ(run.rec.log[4], "TEXT '\\x0A' @13:1:14");
  EXPECT_EQ(run.rec.log[5], "END {-}-:'a' @14:2:1");
  EXPECT_EQ(run.rec.log[6], "DOCUMENT_END @18:2:5");
}

TEST(XmlStream, CharacterDataIsOneEventHoweverItArrives) {
  const std::string doc = "<a>x&amp;y&#65;z\r\nw\rv\n]]&gt;u]<!--c-->t2</a>";
  for (size_t cut = 0; cut <= doc.size(); cut++) {
    Parsed run;
    run_stream(run, doc, {cut, doc.size()});
    ASSERT_EQ(run.status, GTEXT_XML_OK) << cut;
    std::vector<std::string> texts;
    for (const std::string & line : run.rec.log) {
      if (line.compare(0, 4, "TEXT") == 0) {
        texts.push_back(line.substr(0, line.find(" @")));
      }
    }
    EXPECT_EQ(texts, (std::vector<std::string>{"TEXT 'x&yAz\\x0Aw\\x0Av\\x0A]]>u]'", "TEXT 't2'"})) << cut;
  }
  // Adjacent character data around a CDATA section is three nodes, as in a tree.
  Parsed run;
  run_stream(run, "<a>1<![CDATA[2]]>3</a>", Chunks{1});
  ASSERT_EQ(run.status, GTEXT_XML_OK);
  EXPECT_EQ(run.rec.log.size(), 7u);
}

TEST(XmlStream, NamespacesAreResolvedAsInTheTree) {
  const std::string doc =
      "<a xmlns='urn:1' xmlns:p='urn:p'><b xmlns=''><p:c p:x='1' x='2' xml:lang='en'/>"
      "<d xmlns:p='urn:p2'><p:e/></d></b><p:f/></a>";
  Parsed run;
  run_stream(run, doc, Chunks{1});
  ASSERT_EQ(run.status, GTEXT_XML_OK);
  Dom dom;
  run_dom(dom, doc);
  expect_agrees("namespaces", run, dom);
  bool saw_e = false;
  bool saw_c = false;
  for (const std::string & line : run.rec.log) {
    saw_e = saw_e || line.find("START {'urn:p2'}'p':'e'") == 0;
    saw_c = saw_c || line.find("START {'urn:p'}'p':'c' @{'urn:p'}'p':'x'='1'") == 0;
  }
  EXPECT_TRUE(saw_e);
  EXPECT_TRUE(saw_c);
  // The end tag carries the names of its start.
  size_t ends = 0;
  for (const std::string & line : run.rec.log) {
    if (line.find("END {'urn:p2'}'p':'e'") == 0) {
      ends++;
    }
  }
  EXPECT_EQ(ends, 1u);
  // An unbound prefix is the same error at the same place.
  Parsed bad;
  run_stream(bad, "<a><p:b/></a>", Chunks{1});
  Dom bad_dom;
  run_dom(bad_dom, "<a><p:b/></a>");
  expect_agrees("unbound", bad, bad_dom);
  EXPECT_EQ(bad.status, GTEXT_XML_E_NAMESPACE);
}

TEST(XmlStream, Xml11InternalSubsetWhitespaceAgreesAcrossReaders) {
  const std::string doc = "<?xml version='1.1'?><!DOCTYPE a [<!ELEMENT\xC2\x85" "a EMPTY>"
      "<!ENTITY %\xE2\x80\xA8" "p 'value'>]><a/>";
  Dom dom;
  run_dom(dom, doc);
  ASSERT_EQ(dom.status, GTEXT_XML_OK) << dom.err.message;

  Parsed push;
  run_stream(push, doc, Chunks{1});
  ASSERT_EQ(push.status, GTEXT_XML_OK) << push.err.message;
  expect_agrees("XML 1.1 internal-subset whitespace", push, dom);

  for (const std::string & invalid : {
           std::string("<?xml version='1.1'?><!DOCTYPE a SYSTEM '\x7F'><a/>"),
           std::string("<?xml version='1.1'?><!DOCTYPE a [<!ENTITY e '\x7F'>]><a/>")}) {
    Dom failed_dom;
    run_dom(failed_dom, invalid);
    EXPECT_EQ(failed_dom.status, GTEXT_XML_E_CHARACTER);
    EXPECT_EQ(failed_dom.doc, nullptr);
    Parsed failed_stream;
    run_stream(failed_stream, invalid, Chunks{1});
    EXPECT_EQ(failed_stream.status, failed_dom.status);
    EXPECT_EQ(failed_stream.err.byte_offset, failed_dom.err.byte_offset);
  }
}

TEST(XmlStream, Xml11PrefixUndeclarationsMatchDomOneByteAtATime) {
  const std::string valid =
      "<?xml version='1.1'?><p:a xmlns:p='u'><b xmlns:p=''>"
      "<c xmlns:p='v'><p:d/></c></b><p:f/></p:a>";
  Parsed run;
  run_stream(run, valid, Chunks{1});
  ASSERT_EQ(run.status, GTEXT_XML_OK);
  Dom dom;
  run_dom(dom, valid);
  expect_agrees("XML 1.1 prefix undeclare/rebind", run, dom);

  for (const char * invalid : {
           "<?xml version='1.1'?><p:a xmlns:p='u'><b xmlns:p=''><p:c/>"
           "</b></p:a>",
           "<?xml version='1.1'?><a xmlns:p='u'><b xmlns:p='' p:x='1'/>"
           "</a>"}) {
    Parsed failed;
    run_stream(failed, invalid, Chunks{1});
    Dom failed_dom;
    run_dom(failed_dom, invalid);
    expect_agrees("XML 1.1 unbound tombstone prefix", failed, failed_dom);
    EXPECT_EQ(failed.status, GTEXT_XML_E_NAMESPACE);
  }
}

// ---- generated documents, cut every way ------------------------------------

TEST(XmlStream, GeneratedDocumentsGiveOneEventSequenceHoweverCut) {
  for (uint32_t seed = 1; seed <= 250; seed++) {
    Gen g(seed);
    const std::string doc = g.make();
    Dom dom;
    run_dom(dom, doc);
    ASSERT_EQ(dom.status, GTEXT_XML_OK) << "seed " << seed << ": the generator made " << show(doc);
    Parsed whole;
    run_stream(whole, doc, {});
    expect_agrees("whole, seed " + std::to_string(seed), whole, dom);
    Parsed bytes;
    run_stream(bytes, doc, Chunks{1});
    expect_agrees("bytes, seed " + std::to_string(seed), bytes, dom);
    EXPECT_EQ(bytes.rec.log, whole.rec.log) << "seed " << seed << ": " << show(doc);
    // Nothing waits for the end of the input but the end of the document: every
    // construct was delivered when its last byte arrived. (A construct the scan
    // for its end got wrong would be correct but late, and only this sees it.)
    EXPECT_EQ(whole.delivered + 1, whole.rec.log.size()) << "seed " << seed << ": " << show(doc);
    EXPECT_EQ(bytes.delivered + 1, bytes.rec.log.size()) << "seed " << seed << ": " << show(doc);
    std::mt19937 rng(seed * 7919u);
    for (int k = 0; k < 4; k++) {
      Parsed cut;
      run_stream(cut, doc, random_chunks(rng, k == 0 ? 3 : 40));
      EXPECT_EQ(cut.rec.log, whole.rec.log) << "seed " << seed << " random cut " << k;
      expect_agrees("random, seed " + std::to_string(seed), cut, dom);
    }
  }
}

TEST(XmlStream, EverySplitOffsetGivesTheSameEvents) {
  for (uint32_t seed = 300; seed < 340; seed++) {
    Gen g(seed);
    const std::string doc = g.make();
    Parsed whole;
    run_stream(whole, doc, {});
    ASSERT_EQ(whole.status, GTEXT_XML_OK) << seed;
    for (size_t cut = 0; cut <= doc.size(); cut++) {
      Parsed run;
      run_stream(run, doc, {cut, doc.size()});
      ASSERT_EQ(run.status, GTEXT_XML_OK) << "seed " << seed << " cut " << cut;
      ASSERT_EQ(run.rec.log, whole.rec.log) << "seed " << seed << " cut " << cut << " of " << show(doc);
    }
  }
}

TEST(XmlStream, EveryPairOfSplitOffsetsOfAHardDocument) {
  // Constructs whose end depends on the next byte, and bytes that depend on the
  // previous one: all in a document short enough to cut in three every way.
  const std::string doc =
      "<?xml version='1.0'?><a b='>\"'>\r\n&#x41;]]&gt;]<![CDATA[]]]]><!--x-->\xC3\xA9\r</a>";
  Parsed whole;
  run_stream(whole, doc, {});
  ASSERT_EQ(whole.status, GTEXT_XML_OK);
  for (size_t i = 0; i <= doc.size(); i++) {
    for (size_t j = i; j <= doc.size(); j++) {
      Parsed run;
      run_stream(run, doc, {i, j - i, doc.size()});
      ASSERT_EQ(run.status, GTEXT_XML_OK) << i << "," << j;
      ASSERT_EQ(run.rec.log, whole.rec.log) << i << "," << j;
    }
  }
}

TEST(XmlStream, ALineEndCutBetweenItsTwoBytesIsOneLineEnd) {
  const std::string doc = "<a>\r\n\r\n</a>";
  Parsed whole;
  run_stream(whole, doc, {});
  Parsed cut;
  run_stream(cut, doc, Chunks{4, 1, 5});
  EXPECT_EQ(cut.rec.log, whole.rec.log);
  EXPECT_EQ(whole.rec.log[2], "TEXT '\\x0A\\x0A' @3:1:4");
  EXPECT_EQ(whole.rec.log.back(), "DOCUMENT_END @11:3:5");
  // The same between two attributes, in a prolog, and in an end tag, where the
  // line count is all that is kept.
  const std::string lines = "<?xml version='1.0'?>\r\n<!--c-->\r\n<a\r\nx='1'\r\n></a\r\n>\r\n";
  Parsed a;
  run_stream(a, lines, {});
  Parsed b;
  run_stream(b, lines, Chunks{1});
  EXPECT_EQ(a.rec.log, b.rec.log);
  Dom dom;
  run_dom(dom, lines);
  expect_agrees("line ends", b, dom);
}

TEST(XmlStream, TheByteBeforeTheCursorSurvivesDroppingWhatWasRead) {
  // Once enough has been read the parser drops it, keeping one byte, because the
  // line count looks at the byte before a line feed. Cut a CR LF between its two
  // bytes after that has happened: still one line end.
  std::string doc;
  for (int i = 0; i < 3000; i++) {
    doc += "\r\n";
  }
  const size_t cut = doc.size() - 1; // between the last CR and its LF
  doc += "<a/>";
  Dom dom;
  run_dom(dom, doc);
  ASSERT_EQ(dom.status, GTEXT_XML_OK);
  for (size_t c : {cut, cut - 2000, size_t(4097), size_t(4096), size_t(4095)}) {
    Parsed run;
    run_stream(run, doc, {c, doc.size()});
    expect_agrees("cut " + std::to_string(c), run, dom);
  }
  // The same inside a long run of comments, whose line ends are counted too.
  std::string comments;
  for (int i = 0; i < 1500; i++) {
    comments += "<!--x-->\r\n";
  }
  comments += "<a/>";
  Dom dom2;
  run_dom(dom2, comments);
  Parsed run2;
  run_stream(run2, comments, Chunks{4999, 1});
  expect_agrees("comments", run2, dom2);
}

TEST(XmlStream, ACdataEndCutAcrossFeedsIsStillRefusedInText) {
  // "]]>" in character data is an error wherever it is cut, and the position is
  // that of the first bracket.
  const std::string doc = "<a>x]]>y</a>";
  Dom dom;
  run_dom(dom, doc);
  ASSERT_EQ(dom.status, GTEXT_XML_E_SYNTAX);
  for (size_t i = 0; i <= doc.size(); i++) {
    for (size_t j = i; j <= doc.size(); j++) {
      Parsed run;
      run_stream(run, doc, {i, j - i, doc.size()});
      expect_agrees("cut " + std::to_string(i) + "," + std::to_string(j), run, dom);
    }
  }
}

// ---- not well-formed ------------------------------------------------------

const char * const kBroken[] = {
    "",
    " ",
    "<",
    "<a",
    "<a>",
    "<a></b>",
    "<a><b></a></b>",
    "<a/><b/>",
    "<a/>text",
    "text<a/>",
    "<a b=c/>",
    "<a b='1' b='2'/>",
    "<a b='<'/>",
    "<a b='&nope;'/>",
    "<a>&nope;</a>",
    "<a>&#0;</a>",
    "<a>&#xD800;</a>",
    "<a>&amp</a>",
    "<a>&</a>",
    "<a>]]></a>",
    "<a><!-- -- --></a>",
    "<a><!--></a>",
    "<a><!- x --></a>",
    "<a><![CDATA[x</a>",
    "<a><![CDAT[x]]></a>",
    "<a><? ?></a>",
    "<a><?xml ?></a>",
    "<a><?p x</a>",
    "<p:a/>",
    "<a:b:c/>",
    "<a xmlns:p='x' p:q='1' p:q='2'/>",
    "<a xmlns:xmlns='u'/>",
    "<a xmlns:xml='u'/>",
    "<?xml version='2.0'?><a/>",
    "<?xml version='1.0' encoding='EBCDIC-X'?><a/>",
    "<?xml version='1.0' encoding='UTF-16'?><a/>",
    "<?xml version='1.0' standalone='maybe'?><a/>",
    "<?xml version='1.0'><a/>",
    "<?xml  ?><a/>",
    "<!DOCTYPE a><!DOCTYPE a><a/>",
    "<a/><!DOCTYPE a>",
    "<!DOCTYPE a [<!ENTITY x 'y'>",
    "<!DOCTYPE a [ ] ><a/>",
    "<!DOCTYPE a [ <x> ]><a/>",
    "<!DOCTYPE a SYSTEM><a/>",
    "<a>\xC3</a>",
    "<a>\xC3(</a>",
    "<a>\xE2\x82</a>",
    "<a>\xFF</a>",
    "<a>\x01</a>",
    "<a b='\xC3'/>",
    "<a \xC3/>",
    "<a><!--\xE2\x82-->",
    "<\xC3\xA9",
    "<a>\xEF\xBF\xBE</a>",
    "<a>\xED\xA0\x80</a>",
    "\xEF\xBB\xBF",
    "\xEF\xBB",
    "\xEF",
    "<a x='1'y='2'/>",
    "<a x= '1' y ='2'>text</a x>",
    "</a>",
    "<!--c-->",
    "<?p?>",
    "<![CDATA[x]]>",
    "<!x>",
    "<a><!x></a>",
    "<a>&#x;</a>",
    "<a>&#;</a>",
    "<a>&#12a;</a>",
};

TEST(XmlStream, NotWellFormedDocumentsAreRefusedAsTheDomParseRefusesThem) {
  for (const char * text : kBroken) {
    const std::string doc = text;
    Dom dom;
    run_dom(dom, doc);
    Parsed whole;
    run_stream(whole, doc, {});
    expect_agrees("whole " + show(doc), whole, dom);
    Parsed bytes;
    run_stream(bytes, doc, Chunks{1});
    expect_agrees("bytes " + show(doc), bytes, dom);
    EXPECT_EQ(bytes.rec.log, whole.rec.log) << show(doc);
    for (size_t cut = 0; cut <= doc.size(); cut++) {
      Parsed run;
      run_stream(run, doc, {cut, doc.size()});
      expect_agrees("cut " + std::to_string(cut) + " of " + show(doc), run, dom);
      EXPECT_EQ(run.rec.log, whole.rec.log) << show(doc) << " cut " << cut;
    }
  }
}

TEST(XmlStream, DamagedDocumentsAreRefusedAsTheDomParseRefusesThem) {
  static const char alphabet[] = "<>&\"'/]-?!\r\n;#:= \t\x80\xC3\xFF[";
  size_t refused = 0;
  size_t accepted = 0;
  for (uint32_t seed = 1; seed <= 400; seed++) {
    Gen g(seed);
    std::string doc = g.make();
    std::mt19937 rng(seed * 104729u);
    const size_t damage = 1 + rng() % 2;
    for (size_t d = 0; d < damage && !doc.empty(); d++) {
      const size_t at = rng() % doc.size();
      switch (rng() % 3) {
        case 0:
          doc[at] = alphabet[rng() % (sizeof(alphabet) - 1)];
          break;
        case 1:
          doc.erase(at, 1);
          break;
        default:
          doc.insert(at, 1, alphabet[rng() % (sizeof(alphabet) - 1)]);
          break;
      }
    }
    Dom dom;
    run_dom(dom, doc);
    (dom.status == GTEXT_XML_OK ? accepted : refused)++;
    Parsed whole;
    run_stream(whole, doc, {});
    expect_agrees("whole, seed " + std::to_string(seed) + " " + show(doc), whole, dom);
    Parsed bytes;
    run_stream(bytes, doc, Chunks{1});
    expect_agrees("bytes, seed " + std::to_string(seed) + " " + show(doc), bytes, dom);
    EXPECT_EQ(bytes.rec.log, whole.rec.log) << "seed " << seed << " " << show(doc);
    Parsed cut;
    run_stream(cut, doc, random_chunks(rng, 9));
    expect_agrees("random, seed " + std::to_string(seed) + " " + show(doc), cut, dom);
    EXPECT_EQ(cut.rec.log, whole.rec.log) << "seed " << seed << " " << show(doc);
  }
  // The comparison means something only if it saw both kinds.
  EXPECT_GT(refused, 100u);
  EXPECT_GT(accepted, 10u);
}

TEST(XmlStream, TheLexerGivesTheDomVerdictForABufferThatEndsAnywhere) {
  // The driver runs the lexer on a construct only when a scan says it is
  // complete, so the lexer is normally shown a buffer that ends at the end of a
  // construct. Here it is shown every other: in "eager" mode it is run after every
  // byte, on whatever has arrived, and must say "the input ended" and not
  // invent a syntax error where more input would have made the document
  // well-formed - or the verdicts below differ. It matters for a malformed
  // construct, where the scan and the lexer can disagree about where it ends.
  static const char alphabet[] = "<>&\"'/]-?!\r\n;#:= \t\x80\xC3\xFF[%";
  size_t refused = 0;
  size_t accepted = 0;
  for (uint32_t seed = 1; seed <= 600; seed++) {
    Gen g(seed);
    std::string doc = g.make();
    std::mt19937 rng(seed * 15485863u);
    for (size_t d = 0, damage = rng() % 3; d < damage && !doc.empty(); d++) {
      const size_t at = rng() % doc.size();
      switch (rng() % 3) {
        case 0:
          doc[at] = alphabet[rng() % (sizeof(alphabet) - 1)];
          break;
        case 1:
          doc.erase(at, 1);
          break;
        default:
          doc.insert(at, 1, alphabet[rng() % (sizeof(alphabet) - 1)]);
          break;
      }
    }
    Dom dom;
    run_dom(dom, doc);
    (dom.status == GTEXT_XML_OK ? accepted : refused)++;
    Parsed eager;
    run_stream(eager, doc, Chunks{1}, nullptr, true);
    expect_agrees("eager bytes, seed " + std::to_string(seed) + " " + show(doc), eager, dom);
    Parsed reference;
    run_stream(reference, doc, {});
    EXPECT_EQ(eager.rec.log, reference.rec.log) << "seed " << seed << " " << show(doc);
  }
  for (const char * text : kBroken) {
    const std::string doc = text;
    Dom dom;
    run_dom(dom, doc);
    Parsed eager;
    run_stream(eager, doc, Chunks{1}, nullptr, true);
    expect_agrees("eager " + show(doc), eager, dom);
  }
  // Declarations and document types whose quotes hide what looks like their end.
  for (const char * text : {"<?xml version='1.0?>'?><a/>", "<?xml version=\"1.0\" encoding='utf?>8'?><a/>",
           "<?xml version='1.0' standalone='yes?>no'?><a/>", "<?xml version='1.0'?><!DOCTYPE a [<!ENTITY e '>'>]><a/>",
           "<?xml version='1.0'?><!DOCTYPE a SYSTEM 'x>y' [ <!-- ]> --> ]><a/>",
           "<?xml version='1.0'?><!DOCTYPE a PUBLIC 'x' 'y'><a/>", "<a><?pi ?>?></a>", "<a x='\xC3\xA9'/>"}) {
    const std::string doc = text;
    Dom dom;
    run_dom(dom, doc);
    Parsed eager;
    run_stream(eager, doc, Chunks{1}, nullptr, true);
    expect_agrees("eager " + show(doc), eager, dom);
    Parsed bytes;
    run_stream(bytes, doc, Chunks{1});
    expect_agrees("bytes " + show(doc), bytes, dom);
  }
  EXPECT_GT(refused, 200u);
  EXPECT_GT(accepted, 20u);
}

TEST(XmlStream, EveryPrefixOfADocumentIsJudgedAsTheDomParseJudgesIt) {
  // Truncation: the input ends in every possible place, and finish() says what
  // the DOM parse says - UNEXPECTED_EOF for a document that is merely short.
  size_t eof = 0;
  for (uint32_t seed = 500; seed < 530; seed++) {
    Gen g(seed);
    const std::string doc = g.make();
    for (size_t n = 0; n < doc.size(); n++) {
      const std::string prefix = doc.substr(0, n);
      Dom dom;
      run_dom(dom, prefix);
      Parsed bytes;
      run_stream(bytes, prefix, Chunks{1});
      expect_agrees("prefix " + std::to_string(n) + " seed " + std::to_string(seed), bytes, dom);
      if (dom.status == GTEXT_XML_E_UNEXPECTED_EOF) {
        eof++;
      }
      if (n % 7 == 0) {
        Parsed whole;
        run_stream(whole, prefix, {});
        expect_agrees("whole prefix " + std::to_string(n), whole, dom);
      }
    }
  }
  EXPECT_GT(eof, 500u);
}

TEST(XmlStream, TruncationInsideAConstructIsUnexpectedEofFromFinish) {
  // The refusal comes from finish(): until then the construct might yet end.
  const char * const open[] = {"<a><!-- x", "<a><![CDATA[x", "<a><?p x", "<a b='x", "<a b=",
      "<a><b", "<a>&amp", "<a>text", "<!DOCTYPE a [ <!ENTITY e 'x'>", "<a></a", "<?xml version='1.0'"};
  for (const char * text : open) {
    const std::string doc = text;
    GTEXT_XML_Stream * s = gtext_xml_stream_new(nullptr, [](const GTEXT_XML_Event *, void *) { return GTEXT_XML_OK; }, nullptr);
    ASSERT_NE(s, nullptr);
    GTEXT_XML_Error err;
    for (char c : doc) {
      ASSERT_EQ(gtext_xml_stream_feed(s, &c, 1, &err), GTEXT_XML_OK) << doc;
    }
    EXPECT_EQ(gtext_xml_stream_finish(s, &err), GTEXT_XML_E_UNEXPECTED_EOF) << doc;
    EXPECT_EQ(err.code, GTEXT_XML_E_UNEXPECTED_EOF);
    gtext_xml_stream_free(s);
  }
}

// ---- encodings ------------------------------------------------------------

std::vector<uint32_t> code_points(const std::string & utf8) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < utf8.size();) {
    const unsigned char b = (unsigned char)utf8[i];
    uint32_t cp;
    size_t n;
    if (b < 0x80) { cp = b; n = 1; }
    else if (b < 0xE0) { cp = b & 0x1F; n = 2; }
    else if (b < 0xF0) { cp = b & 0x0F; n = 3; }
    else { cp = b & 0x07; n = 4; }
    for (size_t k = 1; k < n; k++) {
      cp = (cp << 6) | ((unsigned char)utf8[i + k] & 0x3F);
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}

void put16(std::string & s, uint32_t u, bool big) {
  s += (char)(big ? u >> 8 : u & 0xFF);
  s += (char)(big ? u & 0xFF : u >> 8);
}

std::string utf16(const std::string & utf8, bool big, bool bom) {
  std::string out;
  if (bom) {
    put16(out, 0xFEFF, big);
  }
  for (uint32_t cp : code_points(utf8)) {
    if (cp >= 0x10000) {
      cp -= 0x10000;
      put16(out, 0xD800 + (cp >> 10), big);
      put16(out, 0xDC00 + (cp & 0x3FF), big);
    }
    else {
      put16(out, cp, big);
    }
  }
  return out;
}

std::string utf32(const std::string & utf8, bool big, bool bom) {
  std::string out;
  auto put = [&](uint32_t u) {
    for (int k = 0; k < 4; k++) {
      out += (char)(big ? u >> (24 - 8 * k) : u >> (8 * k));
    }
  };
  if (bom) {
    put(0xFEFF);
  }
  for (uint32_t cp : code_points(utf8)) {
    put(cp);
  }
  return out;
}

std::string single_byte(const std::string & utf8) {
  std::string out;
  for (uint32_t cp : code_points(utf8)) {
    out += (char)cp;
  }
  return out;
}

std::string declared(const std::string & body, const char * encoding) {
  return std::string("<?xml version=\"1.0\" encoding=\"") + encoding + "\"?>" + body;
}

TEST(XmlStream, EveryEncodingGivesTheDomParsesTreeHoweverItIsCut) {
  size_t checked = 0;
  for (uint32_t seed = 700; seed < 760; seed++) {
    Gen wide(seed, false, false);
    const std::string body = wide.make();
    Gen narrow(seed, true, false);
    const std::string small = narrow.make();
    struct Variant {
      const char * name;
      std::string bytes;
    };
    const std::vector<Variant> variants = {
        {"utf16le bom", utf16(declared(body, "UTF-16"), false, true)},
        {"utf16be bom", utf16(declared(body, "UTF-16"), true, true)},
        {"utf16le bare", utf16(declared(body, "UTF-16"), false, false)},
        {"utf16be bare", utf16(declared(body, "UTF-16"), true, false)},
        {"utf16le no decl", utf16(body, false, true)},
        {"utf32le bom", utf32(declared(body, "UTF-32"), false, true)},
        {"utf32be bom", utf32(declared(body, "UTF-32"), true, true)},
        {"utf32le bare", utf32(declared(body, "UTF-32"), false, false)},
        {"utf32be bare", utf32(declared(body, "UTF-32"), true, false)},
        {"utf8 bom", std::string("\xEF\xBB\xBF") + declared(body, "UTF-8")},
        {"latin1", single_byte(declared(small, "ISO-8859-1"))},
        {"ascii", declared(std::string("<a>x</a>"), "US-ASCII")},
    };
    for (const Variant & v : variants) {
      Dom dom;
      run_dom(dom, v.bytes);
      Parsed whole;
      run_stream(whole, v.bytes, {});
      expect_agrees(std::string(v.name) + " whole, seed " + std::to_string(seed), whole, dom);
      Parsed bytes;
      run_stream(bytes, v.bytes, Chunks{1});
      expect_agrees(std::string(v.name) + " bytes, seed " + std::to_string(seed), bytes, dom);
      EXPECT_EQ(bytes.rec.log, whole.rec.log) << v.name << " seed " << seed;
      std::mt19937 rng(seed);
      Parsed cut;
      run_stream(cut, v.bytes, random_chunks(rng, 7));
      EXPECT_EQ(cut.rec.log, whole.rec.log) << v.name << " seed " << seed;
      if (dom.status == GTEXT_XML_OK) {
        checked++;
      }
    }
  }
  EXPECT_GT(checked, 600u);
}

TEST(XmlStream, AUnitCutInTheMiddleIsCarriedToTheNextFeed) {
  // A surrogate pair, a four-byte UTF-8 character and a UTF-32 unit, cut at
  // every byte.
  const std::string body = "<a>\xF0\x9F\x98\x80 \xE6\x97\xA5</a>";
  const std::vector<std::string> docs = {
      utf16(declared(body, "UTF-16"), false, true), utf16(declared(body, "UTF-16"), true, true),
      utf32(declared(body, "UTF-32"), false, true), utf32(declared(body, "UTF-32"), true, true),
      declared(body, "UTF-8")};
  for (const std::string & doc : docs) {
    Parsed whole;
    run_stream(whole, doc, {});
    ASSERT_EQ(whole.status, GTEXT_XML_OK);
    bool found = false;
    for (const std::string & line : whole.rec.log) {
      found = found || line.find("TEXT '\\xF0\\x9F\\x98\\x80 \\xE6\\x97\\xA5'") == 0;
    }
    EXPECT_TRUE(found);
    for (size_t cut = 0; cut <= doc.size(); cut++) {
      Parsed run;
      run_stream(run, doc, {cut, doc.size()});
      EXPECT_EQ(run.rec.log, whole.rec.log) << "cut " << cut;
    }
  }
}

TEST(XmlStream, BytesThatAreNotValidInTheEncodingAreRefusedAsTheDomParseRefusesThem) {
  const std::string good = utf16(declared("<a>\xF0\x9F\x98\x80</a>", "UTF-16"), false, true);
  std::vector<std::string> bad;
  std::string lone_high = good;                       // a high surrogate then '<'
  const size_t pair = good.find(std::string("\x3D\xD8", 2));
  ASSERT_NE(pair, std::string::npos);
  lone_high.replace(pair + 2, 2, std::string("\x3C\x00", 2));
  bad.push_back(lone_high);
  std::string lone_low = good;                        // a low surrogate first
  lone_low.replace(pair, 4, std::string("\x00\xDE\x00\xDE", 4));
  bad.push_back(lone_low);
  bad.push_back(good.substr(0, good.size() - 1));    // ends inside a unit
  bad.push_back(good + std::string("\x3D\xD8", 2));  // ends after a high surrogate
  bad.push_back(good + std::string("\x3D", 1));
  std::string past = utf32(declared("<a/>", "UTF-32"), false, true);
  past.replace(past.size() - 4, 4, std::string("\x00\x00\x11\x00", 4));
  bad.push_back(past);
  std::string in_pair = utf32(declared("<a/>", "UTF-32"), true, true);
  in_pair.replace(in_pair.size() - 4, 4, std::string("\x00\x00\xD8\x00", 4));
  bad.push_back(in_pair);
  bad.push_back(utf32(declared("<a/>", "UTF-32"), false, true) + "xy");
  bad.push_back(declared("<a>\xE9</a>", "US-ASCII"));
  for (const std::string & doc : bad) {
    Dom dom;
    run_dom(dom, doc);
    ASSERT_EQ(dom.status, GTEXT_XML_E_INVALID_SEQUENCE) << show(doc);
    Parsed whole;
    run_stream(whole, doc, {});
    expect_agrees("whole " + show(doc), whole, dom);
    Parsed bytes;
    run_stream(bytes, doc, Chunks{1});
    expect_agrees("bytes " + show(doc), bytes, dom);
    for (size_t cut = 0; cut <= doc.size(); cut++) {
      Parsed run;
      run_stream(run, doc, {cut, doc.size()});
      expect_agrees("cut " + std::to_string(cut) + " " + show(doc), run, dom);
    }
  }
}

TEST(XmlStream, DetectionWaitsForADeclarationThatMayNameLatin1) {
  const std::string doc = declared("<a>caf\xE9</a>", "ISO-8859-1");
  GTEXT_XML_Stream * s = nullptr;
  Recorder rec;
  rec.replaying = false;
  s = gtext_xml_stream_new(nullptr, record, &rec);
  ASSERT_NE(s, nullptr);
  const size_t declaration_end = doc.find("?>") + 2;
  for (size_t i = 0; i < doc.size(); i++) {
    ASSERT_EQ(gtext_xml_stream_feed(s, doc.data() + i, 1, nullptr), GTEXT_XML_OK) << i;
    // Nothing is delivered until the declaration, which decides how the
    // rest is read, has all arrived.
    if (i + 1 < declaration_end) {
      EXPECT_TRUE(rec.log.empty()) << i;
    }
  }
  ASSERT_EQ(gtext_xml_stream_finish(s, nullptr), GTEXT_XML_OK);
  gtext_xml_stream_free(s);
  bool found = false;
  for (const std::string & line : rec.log) {
    found = found || line.find("TEXT 'caf\\xC3\\xA9'") == 0;
  }
  EXPECT_TRUE(found);

  // An input shorter than the four bytes the first decision needs is decided by
  // finish().
  for (const char * text : {"", "<", "<a", "<a/", "\xFF\xFE", "\xEF\xBB\xBF"}) {
    Dom dom;
    run_dom(dom, text);
    Parsed run;
    run_stream(run, text, Chunks{1});
    expect_agrees(show(text), run, dom);
  }
  // A document with no declaration is delivered as it arrives.
  Recorder rec2;
  GTEXT_XML_Stream * t = gtext_xml_stream_new(nullptr, record, &rec2);
  ASSERT_NE(t, nullptr);
  ASSERT_EQ(gtext_xml_stream_feed(t, "<a><b/>", 7, nullptr), GTEXT_XML_OK);
  EXPECT_EQ(rec2.log.size(), 4u);
  gtext_xml_stream_free(t);
}

// ---- the state rules -------------------------------------------------------

TEST(XmlStream, TheCallingRules) {
  EXPECT_EQ(gtext_xml_stream_new(nullptr, nullptr, nullptr), nullptr);
  gtext_xml_stream_free(nullptr);
  gtext_xml_reader_free(nullptr);
  GTEXT_XML_Error err;
  EXPECT_EQ(gtext_xml_stream_feed(nullptr, "x", 1, &err), GTEXT_XML_E_INVALID);
  EXPECT_EQ(gtext_xml_stream_finish(nullptr, &err), GTEXT_XML_E_INVALID);

  Recorder rec;
  GTEXT_XML_Stream * s = gtext_xml_stream_new(nullptr, record, &rec);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(gtext_xml_stream_feed(s, nullptr, 0, &err), GTEXT_XML_OK);
  EXPECT_EQ(err.code, GTEXT_XML_OK);
  EXPECT_EQ(gtext_xml_stream_feed(s, nullptr, 3, &err), GTEXT_XML_E_INVALID);
  EXPECT_EQ(gtext_xml_stream_feed(s, "<a/>", 4, &err), GTEXT_XML_OK);
  EXPECT_EQ(gtext_xml_stream_finish(s, &err), GTEXT_XML_OK);
  // Finishing twice is accepted; input after the end is a state error and is not
  // a failure of the stream.
  EXPECT_EQ(gtext_xml_stream_finish(s, &err), GTEXT_XML_OK);
  EXPECT_EQ(gtext_xml_stream_feed(s, "<b/>", 4, &err), GTEXT_XML_E_STATE);
  EXPECT_EQ(err.code, GTEXT_XML_E_STATE);
  EXPECT_EQ(gtext_xml_stream_finish(s, &err), GTEXT_XML_OK);
  gtext_xml_stream_free(s);

  // A failed stream says so to every later call, with the same position.
  Recorder rec2;
  s = gtext_xml_stream_new(nullptr, record, &rec2);
  ASSERT_NE(s, nullptr);
  GTEXT_XML_Error first;
  EXPECT_EQ(gtext_xml_stream_feed(s, "<a></b>", 7, &first), GTEXT_XML_E_MISMATCHED_TAG);
  for (int i = 0; i < 3; i++) {
    GTEXT_XML_Error again;
    EXPECT_EQ(gtext_xml_stream_feed(s, "<c/>", 4, &again), GTEXT_XML_E_MISMATCHED_TAG);
    EXPECT_EQ(again.byte_offset, first.byte_offset);
    EXPECT_EQ(again.line, first.line);
    EXPECT_EQ(again.column, first.column);
    EXPECT_EQ(gtext_xml_stream_finish(s, &again), GTEXT_XML_E_MISMATCHED_TAG);
  }
  gtext_xml_stream_free(s);
  gtext_xml_error_free(&first);
}

TEST(XmlStream, AnErrorFromTheCallbackStopsTheParse) {
  const std::string doc = "<?xml version='1.0'?><a x='1'>t<b/><!--c--></a>";
  Parsed all;
  run_stream(all, doc, {});
  ASSERT_EQ(all.status, GTEXT_XML_OK);
  const size_t events = all.rec.log.size();
  ASSERT_GE(events, 7u);
  for (size_t stop = 0; stop <= events; stop++) {
    for (const Chunks & chunks : {Chunks{}, Chunks{1}, Chunks{5}}) {
      Parsed run;
      run.rec.stop_after = stop;
      run.rec.stop_with = GTEXT_XML_E_ENTITY; // any status the callback likes
      run_stream(run, doc, chunks);
      if (stop >= events) {
        EXPECT_EQ(run.status, GTEXT_XML_OK) << stop;
        continue;
      }
      EXPECT_EQ(run.status, GTEXT_XML_E_ENTITY) << stop;
      EXPECT_EQ(run.err.code, GTEXT_XML_E_ENTITY);
      EXPECT_EQ(run.rec.log.size(), stop);
      // Nothing is delivered after the callback said stop.
      for (size_t i = 0; i < stop; i++) {
        EXPECT_EQ(run.rec.log[i], all.rec.log[i]);
      }
    }
  }
  // The stream is finished: the error comes back from every call.
  Recorder rec;
  rec.stop_after = 1;
  rec.stop_with = GTEXT_XML_E_LIMIT_NODES;
  GTEXT_XML_Stream * s = gtext_xml_stream_new(nullptr, record, &rec);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(gtext_xml_stream_feed(s, doc.data(), doc.size(), nullptr), GTEXT_XML_E_LIMIT_NODES);
  EXPECT_EQ(gtext_xml_stream_feed(s, "x", 1, nullptr), GTEXT_XML_E_LIMIT_NODES);
  EXPECT_EQ(gtext_xml_stream_finish(s, nullptr), GTEXT_XML_E_LIMIT_NODES);
  EXPECT_EQ(rec.log.size(), 1u);
  gtext_xml_stream_free(s);
  // A status that the parser itself uses for "short of input" is still the
  // callback's to return, and still stops.
  Parsed eof;
  eof.rec.stop_after = 2;
  eof.rec.stop_with = GTEXT_XML_E_UNEXPECTED_EOF;
  run_stream(eof, doc, Chunks{1});
  EXPECT_EQ(eof.status, GTEXT_XML_E_UNEXPECTED_EOF);
  EXPECT_EQ(eof.rec.log.size(), 2u);
}

// ---- limits ---------------------------------------------------------------

TEST(XmlStream, LimitsAreTheDomParsesLimits) {
  struct Case {
    std::string doc;
    GTEXT_XML_Parse_Options opts;
  };
  std::vector<Case> cases;
  auto with = [](auto set) {
    GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
    set(o);
    return o;
  };
  for (size_t depth = 1; depth <= 5; depth++) {
    cases.push_back({"<a><b><c><d/></c></b></a>", with([&](auto & o) { o.max_depth = depth; })});
    cases.push_back({"<a><b>t</b></a>", with([&](auto & o) { o.max_depth = depth; })});
  }
  for (size_t n = 1; n <= 4; n++) {
    cases.push_back({"<a x='1' y='2' z='3' xmlns:p='u'/>", with([&](auto & o) { o.max_attributes = n; })});
  }
  for (size_t n : {1u, 5u, 6u, 7u}) {
    cases.push_back({"<a b='123456'/>", with([&](auto & o) { o.max_attribute_bytes = n; })});
  }
  for (size_t n : {1u, 3u, 4u, 5u, 9u}) {
    cases.push_back({"<abcd e='1'><ffff/></abcd>", with([&](auto & o) { o.max_name_bytes = n; })});
    cases.push_back({"<a>&amp;<?pitarget d?></a>", with([&](auto & o) { o.max_name_bytes = n; })});
  }
  for (size_t n = 1; n <= 4; n++) {
    cases.push_back({"<a xmlns:p='1' xmlns:q='2'><b xmlns:r='3'/></a>",
        with([&](auto & o) { o.max_namespaces = n; })});
  }
  // The node limit counts what the tree would hold, so it refuses the same
  // documents at the same places.
  const std::string nodes = "<!DOCTYPE a><!--c--><a>t<b/><![CDATA[x]]><?p d?><b>u</b></a><!--d-->";
  for (size_t n = 1; n <= 14; n++) {
    cases.push_back({nodes, with([&](auto & o) { o.max_nodes = n; })});
  }
  for (const Case & c : cases) {
    Dom dom;
    run_dom(dom, c.doc, &c.opts);
    Parsed whole;
    run_stream(whole, c.doc, {}, &c.opts);
    expect_agrees("whole " + show(c.doc), whole, dom);
    Parsed bytes;
    run_stream(bytes, c.doc, Chunks{1}, &c.opts);
    expect_agrees("bytes " + show(c.doc), bytes, dom);
  }
}

TEST(XmlStream, TheInputLimitCountsEveryByteFed) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  const std::string text = "<a>12345</a>";
  o.max_input_bytes = text.size();
  Parsed ok;
  run_stream(ok, text, Chunks{1}, &o);
  EXPECT_EQ(ok.status, GTEXT_XML_OK);
  o.max_input_bytes = text.size() - 1;
  Parsed over;
  run_stream(over, text, Chunks{1}, &o);
  EXPECT_EQ(over.status, GTEXT_XML_E_LIMIT_INPUT);
  EXPECT_EQ(over.err.byte_offset, 0u);
  // Whole, the check is on the length first, as for the DOM parse.
  Parsed garbage;
  o.max_input_bytes = 1;
  run_stream(garbage, std::string(100, '\xFF'), {}, &o);
  EXPECT_EQ(garbage.status, GTEXT_XML_E_LIMIT_INPUT);
}

// ---- the pull reader ------------------------------------------------------

std::vector<std::string> drain(GTEXT_XML_Reader * r, GTEXT_XML_Status * last) {
  std::vector<std::string> out;
  for (;;) {
    GTEXT_XML_Event e;
    *last = gtext_xml_reader_next(r, &e);
    if (*last != GTEXT_XML_OK) {
      return out;
    }
    out.push_back(describe(e));
  }
}

TEST(XmlStream, ThePullReaderGivesThePushParsersEvents) {
  for (uint32_t seed = 900; seed < 960; seed++) {
    Gen g(seed);
    const std::string doc = g.make();
    Parsed push;
    run_stream(push, doc, {});
    ASSERT_EQ(push.status, GTEXT_XML_OK);
    GTEXT_XML_Reader * r = gtext_xml_reader_new(nullptr);
    ASSERT_NE(r, nullptr);
    std::vector<std::string> got;
    GTEXT_XML_Status last;
    std::mt19937 rng(seed);
    size_t at = 0;
    while (at < doc.size()) {
      const size_t n = std::min<size_t>(1 + rng() % 11, doc.size() - at);
      ASSERT_EQ(gtext_xml_reader_feed(r, doc.data() + at, n, nullptr), GTEXT_XML_OK);
      at += n;
      for (const std::string & line : drain(r, &last)) {
        got.push_back(line);
      }
      EXPECT_EQ(last, GTEXT_XML_E_INCOMPLETE);
    }
    ASSERT_EQ(gtext_xml_reader_feed(r, nullptr, 0, nullptr), GTEXT_XML_OK);
    for (const std::string & line : drain(r, &last)) {
      got.push_back(line);
    }
    EXPECT_EQ(last, GTEXT_XML_E_STATE);
    EXPECT_EQ(got, push.rec.log) << seed;
    GTEXT_XML_Event after;
    EXPECT_EQ(gtext_xml_reader_next(r, &after), GTEXT_XML_E_STATE);
    gtext_xml_reader_free(r);
  }
}

TEST(XmlStream, ThePullReaderCopiesWhatItHandsOut) {
  GTEXT_XML_Reader * r = gtext_xml_reader_new(nullptr);
  ASSERT_NE(r, nullptr);
  const std::string doc = "<a xmlns:p='urn:p' p:x='1'>text<!--c--></a>";
  GTEXT_XML_Event e;
  EXPECT_EQ(gtext_xml_reader_next(r, &e), GTEXT_XML_E_INCOMPLETE);
  // The input buffer is not kept: scribble over it after the feed.
  std::string scratch = doc;
  ASSERT_EQ(gtext_xml_reader_feed(r, scratch.data(), scratch.size(), nullptr), GTEXT_XML_OK);
  std::fill(scratch.begin(), scratch.end(), '#');
  ASSERT_EQ(gtext_xml_reader_next(r, &e), GTEXT_XML_OK);
  EXPECT_EQ(e.type, GTEXT_XML_EVENT_DOCUMENT_START);
  ASSERT_EQ(gtext_xml_reader_next(r, &e), GTEXT_XML_OK);
  ASSERT_EQ(e.type, GTEXT_XML_EVENT_ELEMENT_START);
  EXPECT_STREQ(e.local_name, "a");
  ASSERT_EQ(e.attribute_count, 2u);
  EXPECT_STREQ(e.attributes[0].local_name, "p");
  EXPECT_STREQ(e.attributes[1].value, "1");
  // Everything up to the end tag has been completed; only the end of the
  // document waits for the end of the input. Take the rest.
  GTEXT_XML_Status last;
  const std::vector<std::string> rest = drain(r, &last);
  EXPECT_EQ(last, GTEXT_XML_E_INCOMPLETE);
  ASSERT_EQ(rest.size(), 3u);
  EXPECT_EQ(rest[0].find("TEXT 'text'"), 0u);
  EXPECT_EQ(rest[1].find("COMMENT 'c'"), 0u);
  EXPECT_EQ(rest[2].find("END "), 0u);
  GTEXT_XML_Error err;
  ASSERT_EQ(gtext_xml_reader_feed(r, nullptr, 0, &err), GTEXT_XML_OK);
  EXPECT_EQ(gtext_xml_reader_feed(r, nullptr, 0, &err), GTEXT_XML_OK); // twice is accepted
  EXPECT_EQ(gtext_xml_reader_feed(r, "x", 1, &err), GTEXT_XML_E_STATE);
  const std::vector<std::string> tail = drain(r, &last);
  EXPECT_EQ(last, GTEXT_XML_E_STATE);
  ASSERT_EQ(tail.size(), 1u);
  EXPECT_EQ(tail[0].find("DOCUMENT_END"), 0u);
  gtext_xml_reader_free(r);
}

TEST(XmlStream, ThePullReaderRepeatsAParseError) {
  GTEXT_XML_Reader * r = gtext_xml_reader_new(nullptr);
  ASSERT_NE(r, nullptr);
  GTEXT_XML_Error err;
  EXPECT_EQ(gtext_xml_reader_feed(r, "<a><b></a>", 10, &err), GTEXT_XML_E_MISMATCHED_TAG);
  EXPECT_EQ(err.byte_offset, 6u);
  // What came before the error is delivered first, then the error, again and
  // again - never E_STATE, which would read as a clean end.
  GTEXT_XML_Status last;
  const std::vector<std::string> events = drain(r, &last);
  EXPECT_EQ(events.size(), 3u);
  EXPECT_EQ(last, GTEXT_XML_E_MISMATCHED_TAG);
  GTEXT_XML_Event e;
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(gtext_xml_reader_next(r, &e), GTEXT_XML_E_MISMATCHED_TAG);
  }
  GTEXT_XML_Error again;
  EXPECT_EQ(gtext_xml_reader_feed(r, "x", 1, &again), GTEXT_XML_E_MISMATCHED_TAG);
  EXPECT_EQ(again.byte_offset, 6u);
  EXPECT_EQ(gtext_xml_reader_feed(r, nullptr, 0, &again), GTEXT_XML_E_MISMATCHED_TAG);
  gtext_xml_reader_free(r);

  // A document cut short: the error is the end of input's.
  r = gtext_xml_reader_new(nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(gtext_xml_reader_feed(r, "<a>", 3, nullptr), GTEXT_XML_OK);
  EXPECT_EQ(gtext_xml_reader_feed(r, nullptr, 0, &err), GTEXT_XML_E_UNEXPECTED_EOF);
  drain(r, &last);
  EXPECT_EQ(last, GTEXT_XML_E_UNEXPECTED_EOF);
  EXPECT_EQ(gtext_xml_reader_next(nullptr, &e), GTEXT_XML_E_INVALID);
  gtext_xml_reader_free(r);
}

// ---- allocation failure ----------------------------------------------------

struct OomSubject {
  std::string name;
  std::string doc;
  Chunks chunks;
};

void sweep_push(const OomSubject & subject) {
  FaultAllocator fa;
  GTEXT_XML_Parse_Options opts = gtext_xml_parse_options_default();
  opts.allocator = &fa.alloc;
  size_t failures = 0;
  for (size_t n = 1;; n++) {
    fa.arm(n);
    {
      Recorder rec;
      rec.replaying = false;
      GTEXT_XML_Stream * s = gtext_xml_stream_new(&opts, record, &rec);
      GTEXT_XML_Status status = GTEXT_XML_OK;
      if (!s) {
        EXPECT_TRUE(fa.failed) << subject.name << " " << n;
        status = GTEXT_XML_E_OOM;
      }
      else {
        size_t at = 0;
        size_t k = 0;
        while (at < subject.doc.size() && status == GTEXT_XML_OK) {
          size_t len = subject.chunks.empty() ? subject.doc.size() : subject.chunks[k++ % subject.chunks.size()];
          len = std::min(len, subject.doc.size() - at);
          GTEXT_XML_Error err;
          status = gtext_xml_stream_feed(s, subject.doc.data() + at, len, &err);
          if (status != GTEXT_XML_OK) {
            EXPECT_EQ(err.code, status);
          }
          at += len;
        }
        if (status == GTEXT_XML_OK) {
          status = gtext_xml_stream_finish(s, nullptr);
        }
        if (status != GTEXT_XML_OK) {
          // A failed stream stays failed, and can be freed.
          EXPECT_EQ(gtext_xml_stream_feed(s, "x", 1, nullptr), status);
        }
        gtext_xml_stream_free(s);
      }
      if (fa.failed) {
        failures++;
        EXPECT_EQ(status, GTEXT_XML_E_OOM) << subject.name << " failing allocation " << n;
      }
      else {
        EXPECT_EQ(status, GTEXT_XML_OK) << subject.name << " " << n;
      }
    }
    EXPECT_TRUE(fa.live.empty()) << subject.name << ": leak after failing allocation " << n;
    EXPECT_EQ(fa.bad_frees, 0u);
    if (!fa.failed) {
      break; // the budget outlasted the work
    }
    ASSERT_LT(n, 100000u);
  }
  EXPECT_GT(failures, 5u) << subject.name;
}

TEST(XmlStream, AllocationFailureAtAnyPointIsOomAndLeaksNothing) {
  const std::string body =
      "<!DOCTYPE r [<!ENTITY e 'x'>]><!--c--><r xmlns='urn:d' xmlns:p='urn:p' a='1' p:b='2&amp;'>"
      "t&amp;u<p:e/><![CDATA[<c>]]><?pi data?><x y='z'>\xC3\xA9</x></r><?after ?>";
  const std::string with_decl = "<?xml version='1.0' encoding='UTF-8'?>" + body;
  for (const Chunks & chunks : {Chunks{}, Chunks{1}, Chunks{7}}) {
    sweep_push({"utf-8", with_decl, chunks});
    sweep_push({"utf-16", utf16(declared(body, "UTF-16"), false, true), chunks});
    sweep_push({"utf-32", utf32(declared(body, "UTF-32"), true, true), chunks});
    sweep_push({"latin-1", declared("<a>caf\xE9</a>", "ISO-8859-1"), chunks});
  }
}

TEST(XmlStream, TheReaderSurvivesAllocationFailureAtAnyPoint) {
  const std::string doc = "<?xml version='1.0'?><a xmlns:p='urn:p' p:x='1'>t<!--c--><b/><![CDATA[d]]></a>";
  FaultAllocator fa;
  GTEXT_XML_Parse_Options opts = gtext_xml_parse_options_default();
  opts.allocator = &fa.alloc;
  size_t failures = 0;
  for (size_t n = 1;; n++) {
    fa.arm(n);
    GTEXT_XML_Reader * r = gtext_xml_reader_new(&opts);
    GTEXT_XML_Status status = GTEXT_XML_OK;
    if (!r) {
      EXPECT_TRUE(fa.failed);
      status = GTEXT_XML_E_OOM;
    }
    else {
      for (size_t at = 0; at < doc.size() && status == GTEXT_XML_OK; at += 5) {
        status = gtext_xml_reader_feed(r, doc.data() + at, std::min<size_t>(5, doc.size() - at), nullptr);
        GTEXT_XML_Event e;
        while (gtext_xml_reader_next(r, &e) == GTEXT_XML_OK) {
        }
      }
      if (status == GTEXT_XML_OK) {
        status = gtext_xml_reader_feed(r, nullptr, 0, nullptr);
      }
      GTEXT_XML_Event e;
      while (gtext_xml_reader_next(r, &e) == GTEXT_XML_OK) {
      }
      if (status != GTEXT_XML_OK) {
        EXPECT_EQ(gtext_xml_reader_next(r, &e), status);
      }
      gtext_xml_reader_free(r);
    }
    if (fa.failed) {
      failures++;
      EXPECT_EQ(status, GTEXT_XML_E_OOM) << n;
    }
    else {
      EXPECT_EQ(status, GTEXT_XML_OK) << n;
    }
    EXPECT_TRUE(fa.live.empty()) << "leak after failing allocation " << n;
    EXPECT_EQ(fa.bad_frees, 0u);
    if (!fa.failed) {
      break;
    }
    ASSERT_LT(n, 100000u);
  }
  EXPECT_GT(failures, 5u);
}

// ---- time and memory ------------------------------------------------------

double seconds_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

GTEXT_XML_Status ignore(const GTEXT_XML_Event *, void *) {
  return GTEXT_XML_OK;
}

// Feed `doc` one byte at a time and return how long it took, having checked it
// was accepted.
double time_by_bytes(const std::string & doc) {
  GTEXT_XML_Stream * s = gtext_xml_stream_new(nullptr, ignore, nullptr);
  EXPECT_NE(s, nullptr);
  const auto start = std::chrono::steady_clock::now();
  GTEXT_XML_Status status = GTEXT_XML_OK;
  for (size_t i = 0; i < doc.size() && status == GTEXT_XML_OK; i++) {
    status = gtext_xml_stream_feed(s, doc.data() + i, 1, nullptr);
  }
  if (status == GTEXT_XML_OK) {
    status = gtext_xml_stream_finish(s, nullptr);
  }
  const double elapsed = seconds_since(start);
  EXPECT_EQ(status, GTEXT_XML_OK);
  gtext_xml_stream_free(s);
  return elapsed;
}

TEST(XmlStream, WorkIsLinearInTheLengthOfAConstructFedAByteAtATime) {
  // The bound: every byte is examined a constant number of times by the scan
  // for the end of a construct and once by the lexer, however the input is cut.
  // A parser that tried the construct again on every byte would do work
  // proportional to the square of its length - for a megabyte, about 10^12
  // steps, hours - so a generous wall-clock bound separates the two without
  // being a benchmark. Each document is a megabyte or a quarter of one.
  const size_t mega = 1024 * 1024;
  const size_t quarter = mega / 4;
  struct Shape {
    const char * name;
    std::string doc;
  };
  std::string many_gt = "<a><!--";
  for (size_t i = 0; i < quarter; i++) {
    many_gt += "->";
  }
  many_gt += "--></a>";
  std::string subset = "<!DOCTYPE a [";
  for (size_t i = 0; i < quarter / 16; i++) {
    subset += "<!ENTITY e \"a>]b\">";
  }
  subset += "]><a/>";
  std::string attributes = "<a";
  for (size_t i = 0; i < 1000; i++) {
    attributes += " k" + std::to_string(i) + "='" + std::string(quarter / 1000, 'v') + "'";
  }
  attributes += "/>";
  const std::vector<Shape> shapes = {
      {"comment", "<a><!--" + std::string(mega, 'c') + "--></a>"},
      {"text run", "<a>" + std::string(mega, 't') + "</a>"},
      {"text with brackets", "<a>" + std::string(quarter, ']') + "</a>"},
      {"text with line ends", "<a>" + [&] { std::string s; for (size_t i = 0; i < quarter / 2; i++) s += "\r\n"; return s; }() + "</a>"},
      {"cdata", "<a><![CDATA[" + std::string(mega, 'd') + "]]></a>"},
      {"processing instruction", "<a><?p " + std::string(mega, 'x') + "?></a>"},
      {"comment of >", "<a><!--" + std::string(quarter, '>') + "--></a>"},
      {"comment of ->", many_gt},
      {"attribute value", "<a b='" + std::string(mega, 'v') + "'/>"},
      {"many attributes", attributes},
      {"white space in a tag", "<a" + std::string(quarter, ' ') + "/>"},
      {"white space in an end tag", "<a></a" + std::string(quarter, ' ') + ">"},
      {"white space before the root", std::string(quarter, '\n') + "<a/>"},
      {"internal subset", subset},
      {"reference digits", "<a>&#" + std::string(quarter, '0') + "65;</a>"},
      {"one reference after another", "<a>" + [&] { std::string s; for (size_t i = 0; i < quarter / 5; i++) s += "&amp;"; return s; }() + "</a>"},
  };
  double worst = 0;
  for (const Shape & shape : shapes) {
    const double t = time_by_bytes(shape.doc);
    worst = std::max(worst, t);
    EXPECT_LT(t, 15.0) << shape.name << " took " << t << "s for " << shape.doc.size() << " bytes";
  }
  RecordProperty("slowest_shape_seconds", std::to_string(worst));
  // And the ratio, which is what "linear" is: four times the comment costs
  // about four times, not sixteen. (A wide margin, since small times are noisy.)
  const double small = time_by_bytes("<a><!--" + std::string(mega / 4, 'c') + "--></a>");
  const double large = time_by_bytes("<a><!--" + std::string(mega, 'c') + "--></a>");
  EXPECT_LT(large, std::max(small * 12.0, 0.5)) << small << "s then " << large << "s";
}

struct CountingAllocator {
  GTEXT_Allocator alloc;
  size_t live = 0;
  size_t peak = 0;
  std::vector<std::pair<void *, size_t>> sizes; // small; searched linearly from the back
  CountingAllocator() {
    alloc.ctx = this;
    alloc.malloc_fn = [](void * c, size_t n) { return static_cast<CountingAllocator *>(c)->take(nullptr, n, false); };
    alloc.calloc_fn = [](void * c, size_t k, size_t n) { return static_cast<CountingAllocator *>(c)->take(nullptr, k * n, true); };
    alloc.realloc_fn = [](void * c, void * p, size_t n) { return static_cast<CountingAllocator *>(c)->take(p, n, false); };
    alloc.free_fn = [](void * c, void * p) { static_cast<CountingAllocator *>(c)->give(p); };
  }
  void * take(void * old, size_t n, bool zero) {
    void * p = zero ? std::calloc(1, n ? n : 1) : std::malloc(n ? n : 1);
    if (!p) {
      return nullptr;
    }
    if (old) {
      for (size_t i = sizes.size(); i > 0; i--) {
        if (sizes[i - 1].first == old) {
          std::memcpy(p, old, std::min(sizes[i - 1].second, n));
          break;
        }
      }
      give(old);
    }
    sizes.push_back({p, n});
    live += n;
    peak = std::max(peak, live);
    return p;
  }
  void give(void * p) {
    if (!p) {
      return;
    }
    for (size_t i = sizes.size(); i > 0; i--) {
      if (sizes[i - 1].first == p) {
        live -= sizes[i - 1].second;
        sizes.erase(sizes.begin() + (long)(i - 1));
        break;
      }
    }
    std::free(p);
  }
};

TEST(XmlStream, AnXmlDeclarationStillArrivingIsNotRescannedOnEveryFeed) {
  // The encoding is decided from the declaration, so bytes are held until it
  // ends. Counting the bytes examined (not time): a long one fed a byte at a
  // time costs a small multiple of its length, and past 64 KiB it is refused.
  const size_t n = 60000;
  const std::string head = "<?xml version='1.0'" + std::string(n, ' ');
  GTEXT_XML_Stream * s = gtext_xml_stream_new(nullptr, ignore, nullptr);
  ASSERT_NE(s, nullptr);
  for (char c : head) {
    ASSERT_EQ(gtext_xml_stream_feed(s, &c, 1, nullptr), GTEXT_XML_OK);
  }
  EXPECT_LT(xml_stream_work(s), head.size() * 12) << "work was " << xml_stream_work(s);
  const std::string rest = "?><a/>";
  ASSERT_EQ(gtext_xml_stream_feed(s, rest.data(), rest.size(), nullptr), GTEXT_XML_OK);
  EXPECT_EQ(gtext_xml_stream_finish(s, nullptr), GTEXT_XML_OK);
  gtext_xml_stream_free(s);

  s = gtext_xml_stream_new(nullptr, ignore, nullptr);
  ASSERT_NE(s, nullptr);
  const std::string too_long = "<?xml version='1.0'" + std::string(70000, ' ');
  GTEXT_XML_Error err;
  EXPECT_EQ(gtext_xml_stream_feed(s, too_long.data(), too_long.size(), &err), GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(err.code, GTEXT_XML_E_SYNTAX);
  gtext_xml_stream_free(s);
}

TEST(XmlStream, MemoryIsBoundedByTheConstructAndNotByTheDocument) {
  // Twenty megabytes of small elements, fed in pieces of 4 KiB: what is held at
  // any moment is a piece and the construct in progress.
  CountingAllocator counting;
  GTEXT_XML_Parse_Options opts = gtext_xml_parse_options_default();
  opts.allocator = &counting.alloc;
  GTEXT_XML_Stream * s = gtext_xml_stream_new(&opts, ignore, nullptr);
  ASSERT_NE(s, nullptr);
  std::string unit;
  for (int i = 0; i < 100; i++) {
    unit += "<item id='" + std::to_string(i) + "' xmlns:p='urn:p'>text &amp; more <p:e/><!-- c --></item>\n";
  }
  const std::string head = "<root>";
  ASSERT_EQ(gtext_xml_stream_feed(s, head.data(), head.size(), nullptr), GTEXT_XML_OK);
  size_t fed = 0;
  while (fed < 20u * 1024 * 1024) {
    // The unit is fed in pieces of 4 KiB at most, cut anywhere.
    for (size_t at = 0; at < unit.size(); at += 4096) {
      const size_t n = std::min<size_t>(4096, unit.size() - at);
      ASSERT_EQ(gtext_xml_stream_feed(s, unit.data() + at, n, nullptr), GTEXT_XML_OK);
    }
    fed += unit.size();
  }
  const std::string tail = "</root>";
  ASSERT_EQ(gtext_xml_stream_feed(s, tail.data(), tail.size(), nullptr), GTEXT_XML_OK);
  ASSERT_EQ(gtext_xml_stream_finish(s, nullptr), GTEXT_XML_OK);
  gtext_xml_stream_free(s);
  EXPECT_EQ(counting.live, 0u);
  EXPECT_LT(counting.peak, 256u * 1024) << "the peak was " << counting.peak << " bytes for " << fed << " of input";
}

TEST(XmlStream, TheEventsOfAHugeDocumentFedInPiecesAreTheTreesOfTheWhole) {
  // Many levels and many siblings, in pieces that cut everywhere.
  std::string doc = "<?xml version='1.0'?><root>";
  for (int i = 0; i < 2000; i++) {
    doc += "<g n='" + std::to_string(i) + "'>x<h/>\r\n&#x41;<!--c-->]]&gt;</g>";
  }
  doc += "</root>";
  Dom dom;
  run_dom(dom, doc);
  ASSERT_EQ(dom.status, GTEXT_XML_OK);
  std::mt19937 rng(7);
  Parsed run;
  run_stream(run, doc, random_chunks(rng, 61));
  expect_agrees("many siblings", run, dom);
}


// Found by fuzz_xml. An attribute value's run of ordinary bytes was checked against
// the limit only when the run ended, and a run ends where the input that has arrived
// ends: so a value over the limit, with a byte that is not a character further along
// the same run, was LIMIT_VALUE when the bytes came in pieces that ended before the
// bad byte and an invalid-character refusal when they came whole. One document, two
// answers, by how it was cut. The limit is met at the byte that crosses it.
//
// The other limits that look at a run were audited for the same shape and need no
// change: a name's is checked after every character (name_at()), and the depth, the
// attribute count, the namespaces in scope and the node count are counted per element
// or node and not per run of bytes. Character data has no byte limit.
struct LimitCase {
  std::string doc;
  GTEXT_XML_Status want;
  const char * what;
};

// The pull reader over `doc`, in pieces of `chunk` bytes: the status and the error the feed reported.
GTEXT_XML_Status pull_status(const std::string & doc, size_t chunk, const GTEXT_XML_Parse_Options & opts,
    GTEXT_XML_Error * err) {
  GTEXT_XML_Reader * r = gtext_xml_reader_new(&opts);
  EXPECT_NE(r, nullptr);
  GTEXT_XML_Status st = GTEXT_XML_OK;
  size_t at = 0;
  while (at < doc.size() && st == GTEXT_XML_OK) {
    const size_t n = std::min(chunk == 0 ? doc.size() : chunk, doc.size() - at);
    st = gtext_xml_reader_feed(r, doc.data() + at, n, err);
    at += n;
    GTEXT_XML_Status last;
    drain(r, &last);
  }
  if (st == GTEXT_XML_OK) {
    st = gtext_xml_reader_feed(r, nullptr, 0, err);
  }
  gtext_xml_reader_free(r);
  return st;
}

void check_limit_cases(const std::vector<LimitCase> & cases, const GTEXT_XML_Parse_Options & opts) {
  for (const LimitCase & c : cases) {
    Dom dom;
    run_dom(dom, c.doc, &opts);
    ASSERT_EQ(dom.status, c.want) << c.what << ": " << show(c.doc);
    for (size_t chunk : {1u, 2u, 3u, 5u, 8u, 13u, 64u}) {
      Parsed run;
      run_stream(run, c.doc, Chunks{chunk}, &opts);
      expect_agrees(std::string(c.what) + ", pieces of " + std::to_string(chunk), run, dom);
      GTEXT_XML_Error err{};
      EXPECT_EQ(pull_status(c.doc, chunk, opts, &err), dom.status) << c.what << ", pull reader, pieces of " << chunk;
      if (dom.status != GTEXT_XML_OK) {
        EXPECT_EQ(err.byte_offset, dom.err.byte_offset) << c.what << ", pull reader, pieces of " << chunk;
        EXPECT_EQ(err.line, dom.err.line) << c.what;
        EXPECT_EQ(err.column, dom.err.column) << c.what;
      }
    }
    Parsed whole;
    run_stream(whole, c.doc, Chunks{}, &opts);
    expect_agrees(std::string(c.what) + ", whole", whole, dom);
  }
}

TEST(XmlStream, ALimitAndABadByteInOneRunAreReportedInInputOrderHoweverTheInputIsCut) {
  GTEXT_XML_Parse_Options opts = gtext_xml_parse_options_default();
  opts.max_attribute_bytes = 8;
  check_limit_cases({
      // Over the limit at the ninth byte; the bad byte is the twentieth.
      {"<a v=\"0123456789012345678\xE2\xA4 \"/>", GTEXT_XML_E_LIMIT_VALUE, "limit first"},
      // The bad byte is the third; the limit would be crossed at the ninth.
      {"<a v=\"01\xE2\xA4 3456789012345678\"/>", GTEXT_XML_E_UTF8, "bad byte first"},
      // Exactly at the limit, then the bad byte: the limit is not crossed.
      {"<a v=\"01234567\xE2\xA4 \"/>", GTEXT_XML_E_UTF8, "bad byte at the limit"},
      // One past the limit, then the bad byte: the limit is crossed first.
      {"<a v=\"012345678\xE2\xA4 \"/>", GTEXT_XML_E_LIMIT_VALUE, "bad byte one past the limit"},
      // A reference before the run: it is one byte of the pool, and the run starts at 1.
      {"<a v=\"&amp;0123456789\xE2\xA4 \"/>", GTEXT_XML_E_LIMIT_VALUE, "a reference, then the limit"},
      {"<a v=\"&amp;01\xE2\xA4 23456789\"/>", GTEXT_XML_E_UTF8, "a reference, then the bad byte"},
      {"<a v=\"&amp;0123456\xE2\xA4 \"/>", GTEXT_XML_E_UTF8, "a reference and the pool exactly at the limit"},
      // Three-byte characters: the limit is crossed by the third of them, in the middle of nothing
      // and, in pieces of one or two bytes, in the middle of a character's bytes.
      {"<a v=\"\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC\"/>", GTEXT_XML_E_LIMIT_VALUE, "multibyte, over"},
      {"<a v=\"\xE2\x82\xAC\xE2\x82\xAC\"/>", GTEXT_XML_OK, "multibyte, under"},
  }, opts);
}

TEST(XmlStream, AValueLimitOfZeroIsTheDefaultAndNotNoValue) {
  GTEXT_XML_Parse_Options opts = gtext_xml_parse_options_default();
  opts.max_attribute_bytes = 0;
  // 0 selects the default (16 MiB): a long value is accepted, and a bad byte after it is the bad byte.
  check_limit_cases({
      {"<a v=\"" + std::string(100, 'x') + "\"/>", GTEXT_XML_OK, "a hundred bytes"},
      {"<a v=\"" + std::string(100, 'x') + "\xE2\xA4 \"/>", GTEXT_XML_E_UTF8, "a hundred bytes and a bad byte"},
  }, opts);
}

TEST(XmlStream, TheValueLimitCountsBytesOfTheConvertedValueInUtf16AndLatin1) {
  GTEXT_XML_Parse_Options opts = gtext_xml_parse_options_default();
  opts.max_attribute_bytes = 8;
  auto utf16 = [](const std::string & ascii) {
    std::string out = "\xFF\xFE";
    for (char c : ascii) {
      out += c;
      out += '\0';
    }
    return out;
  };
  // UTF-16: the limit is on the UTF-8 form, so nine ASCII units cross it and eight do not.
  check_limit_cases({
      {utf16("<a v=\"012345678\"/>"), GTEXT_XML_E_LIMIT_VALUE, "UTF-16, nine bytes"},
      {utf16("<a v=\"01234567\"/>"), GTEXT_XML_OK, "UTF-16, eight bytes"},
      // ISO-8859-1: each of these letters is two bytes in UTF-8, so five cross the limit, four do not.
      {"<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a v=\"\xE9\xE9\xE9\xE9\xE9\"/>", GTEXT_XML_E_LIMIT_VALUE,
          "Latin-1, ten bytes"},
      {"<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a v=\"\xE9\xE9\xE9\xE9\"/>", GTEXT_XML_OK, "Latin-1, eight bytes"},
  }, opts);
}

} // namespace
