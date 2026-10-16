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
 * The UTF-8 byte order mark (EF BB BF), everywhere it can matter.
 *
 * - The writer can give it back: a document parsed from a file with a mark is
 *   written with one, byte for byte, and one without is not given one; the option
 *   has three values and each is checked, with the encodings that have no such
 *   mark.
 * - The event parsers read it however it is cut: EF | BB BF, EF BB | BF, a byte
 *   at a time, with and without an XML declaration, in the push parser (eager
 *   mode too) and the pull reader, giving the whole-input feed's events with the
 *   same encoding, source and locations (the mark is in the byte offsets and not
 *   in the columns); and a mark cut short at the end is the DOM parse's error.
 * - A mark and a declaration that disagree are refused the same way by the DOM
 *   parse, the push parser and the pull reader. XML 1.0 Appendix F: a mark says
 *   the document is UTF-8 (or UTF-16, UTF-32); 4.3.3: a declaration in an entity
 *   in one encoding that names another is an error, and the specification says
 *   a processor must not treat it as the other.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>

#include "../src/markup/markup_internal.h"
#include "../src/xml/xml_internal.h"

namespace {

using Doc = GTEXT_Markup_Document;
using Opts = GTEXT_XML_Write_Options;

const std::string kBom = "\xEF\xBB\xBF";

Doc * parse(const std::string & text, GTEXT_XML_Error * err = nullptr) {
  Doc * doc = nullptr;
  GTEXT_XML_Error local;
  const GTEXT_XML_Status st = gtext_xml_parse(text.data(), text.size(), nullptr, &doc, err ? err : &local);
  EXPECT_EQ(st, GTEXT_XML_OK);
  return st == GTEXT_XML_OK ? doc : nullptr;
}

struct Out {
  GTEXT_XML_Status status;
  std::string bytes;
};

Out write_with(const Doc * doc, const Opts & o) {
  Out out;
  char * buf = nullptr;
  size_t len = 0;
  out.status = gtext_xml_write_buffer(doc, &o, &buf, &len, nullptr);
  if (out.status == GTEXT_XML_OK) {
    out.bytes.assign(buf, len);
    gtext_xml_write_buffer_free(doc, &o, buf);
  }
  return out;
}

Opts with_bom(GTEXT_XML_Bom_Mode mode) {
  Opts o = gtext_xml_write_options_default();
  o.byte_order_mark = mode;
  return o;
}

// ---- 1. the writer ---------------------------------------------------------

TEST(XmlBom, ADocumentParsedWithAMarkIsWrittenWithOneByteForByte) {
  const std::vector<std::string> documents = {
      "<a><b/>text</a>",
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<a x=\"1\">t</a>",
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<!--c-->\n<a/>",
      "<!DOCTYPE a SYSTEM \"a.dtd\">\n<a>caf\xC3\xA9</a>",
  };
  for (const std::string & body : documents) {
    SCOPED_TRACE(body);
    Doc * doc = parse(kBom + body);
    ASSERT_NE(doc, nullptr);
    GTEXT_Markup_Encoding_Source source;
    EXPECT_STREQ(gtext_markup_document_encoding(doc, &source), "UTF-8");
    EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);
    const Out first = write_with(doc, gtext_xml_write_options_default());
    ASSERT_EQ(first.status, GTEXT_XML_OK);
    EXPECT_EQ(first.bytes, kBom + body);
    // The second write, from what was read back, is the same bytes.
    Doc * back = parse(first.bytes);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(write_with(back, gtext_xml_write_options_default()).bytes, first.bytes);
    gtext_markup_document_free(back);
    gtext_markup_document_free(doc);

    // And one without a mark is not given one.
    Doc * plain = parse(body);
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(write_with(plain, gtext_xml_write_options_default()).bytes, body);
    gtext_markup_document_free(plain);
  }
}

TEST(XmlBom, EachValueOfTheOption) {
  Doc * with = parse(kBom + "<a/>");
  Doc * without = parse("<a/>");
  EXPECT_EQ(write_with(with, with_bom(GTEXT_XML_BOM_AUTO)).bytes, kBom + "<a/>");
  EXPECT_EQ(write_with(with, with_bom(GTEXT_XML_BOM_ALWAYS)).bytes, kBom + "<a/>");
  EXPECT_EQ(write_with(with, with_bom(GTEXT_XML_BOM_NEVER)).bytes, "<a/>");
  EXPECT_EQ(write_with(without, with_bom(GTEXT_XML_BOM_AUTO)).bytes, "<a/>");
  EXPECT_EQ(write_with(without, with_bom(GTEXT_XML_BOM_ALWAYS)).bytes, kBom + "<a/>");
  EXPECT_EQ(write_with(without, with_bom(GTEXT_XML_BOM_NEVER)).bytes, "<a/>");

  // Zeroed is AUTO.
  Opts zero;
  std::memset(&zero, 0, sizeof(zero));
  EXPECT_EQ(write_with(with, zero).bytes, kBom + "<a/>");
  EXPECT_EQ(gtext_xml_write_options_default().byte_order_mark, GTEXT_XML_BOM_AUTO);

  // Written again with ALWAYS the output reads back with a mark, so AUTO keeps it.
  Doc * made = parse(write_with(without, with_bom(GTEXT_XML_BOM_ALWAYS)).bytes);
  ASSERT_NE(made, nullptr);
  EXPECT_EQ(write_with(made, with_bom(GTEXT_XML_BOM_AUTO)).bytes, kBom + "<a/>");
  gtext_markup_document_free(made);

  // Not a value.
  Opts bad = gtext_xml_write_options_default();
  bad.byte_order_mark = static_cast<GTEXT_XML_Bom_Mode>(3);
  EXPECT_EQ(write_with(with, bad).status, GTEXT_XML_E_INVALID);
  bad.byte_order_mark = static_cast<GTEXT_XML_Bom_Mode>(-1);
  EXPECT_EQ(write_with(with, bad).status, GTEXT_XML_E_INVALID);
  gtext_markup_document_free(with);
  gtext_markup_document_free(without);
}

TEST(XmlBom, OnlyUtf8HasAChoiceAndTheOtherEncodingsAreWhatTheyWere) {
  Doc * with = parse(kBom + "<a/>");
  const std::string le("\xFF\xFE<\0a\0/\0>\0", 10);
  const std::string be("\xFE\xFF\0<\0a\0/\0>", 10);
  for (GTEXT_XML_Bom_Mode mode : {GTEXT_XML_BOM_AUTO, GTEXT_XML_BOM_ALWAYS, GTEXT_XML_BOM_NEVER}) {
    Opts o = with_bom(mode);
    o.encoding = GTEXT_XML_OUTPUT_UTF16LE;
    EXPECT_EQ(write_with(with, o).bytes, le) << mode;  // its own mark, once
    o.encoding = GTEXT_XML_OUTPUT_UTF16BE;
    EXPECT_EQ(write_with(with, o).bytes, be) << mode;
    o.encoding = GTEXT_XML_OUTPUT_ISO_8859_1;
    const Out l1 = write_with(with, o);
    o.encoding = GTEXT_XML_OUTPUT_US_ASCII;
    const Out ascii = write_with(with, o);
    if (mode == GTEXT_XML_BOM_ALWAYS) {
      // There is no such mark in these encodings.
      EXPECT_EQ(l1.status, GTEXT_XML_E_INVALID);
      EXPECT_EQ(ascii.status, GTEXT_XML_E_INVALID);
    } else {
      EXPECT_EQ(l1.bytes, "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n<a/>") << mode;
      EXPECT_EQ(ascii.bytes, "<a/>") << mode;
    }
  }
  gtext_markup_document_free(with);

  // A document that came in as UTF-16 has a mark and is not UTF-8: AUTO gives
  // UTF-8 output none.
  const std::string u16 = le;
  Doc * from16 = parse(u16);
  ASSERT_NE(from16, nullptr);
  GTEXT_Markup_Encoding_Source source;
  EXPECT_STREQ(gtext_markup_document_encoding(from16, &source), "UTF-16");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);
  EXPECT_EQ(write_with(from16, with_bom(GTEXT_XML_BOM_AUTO)).bytes, "<a/>");
  EXPECT_EQ(write_with(from16, with_bom(GTEXT_XML_BOM_ALWAYS)).bytes, kBom + "<a/>");
  gtext_markup_document_free(from16);
}

TEST(XmlBom, AnyCaseOfUtf8InTheRecordedLabelCountsAndOtherSourcesDoNot) {
  const struct {
    const char * label;
    GTEXT_Markup_Encoding_Source source;
    bool bom;
  } cases[] = {
      {"UTF-8", GTEXT_MARKUP_ENCODING_BOM, true},
      {"utf-8", GTEXT_MARKUP_ENCODING_BOM, true},
      {"Utf-8", GTEXT_MARKUP_ENCODING_BOM, true},
      {"UTF-8", GTEXT_MARKUP_ENCODING_DECLARATION, false},
      {"UTF-8", GTEXT_MARKUP_ENCODING_DEFAULT, false},
      {"UTF-8", GTEXT_MARKUP_ENCODING_CALLER, false},
      {"UTF-16", GTEXT_MARKUP_ENCODING_BOM, false},
      {"UTF-8x", GTEXT_MARKUP_ENCODING_BOM, false},
      {"UTF-", GTEXT_MARKUP_ENCODING_BOM, false},
  };
  for (const auto & c : cases) {
    Doc * doc = nullptr;
    ASSERT_EQ(gtext_markup_document_new(nullptr, &doc), GTEXT_MARKUP_OK);
    GTEXT_Markup_Node * e = nullptr;
    ASSERT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "a", &e), GTEXT_MARKUP_OK);
    ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(doc), e), GTEXT_MARKUP_OK);
    ASSERT_EQ(gtext_markup_document_set_encoding(doc, c.label, c.source), GTEXT_MARKUP_OK);
    EXPECT_EQ(write_with(doc, with_bom(GTEXT_XML_BOM_AUTO)).bytes, c.bom ? kBom + "<a/>" : "<a/>")
        << c.label << " " << c.source;
    gtext_markup_document_free(doc);
  }
  // A tree made by hand with no recorded encoding at all.
  Doc * doc = nullptr;
  ASSERT_EQ(gtext_markup_document_new(nullptr, &doc), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node * e = nullptr;
  ASSERT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "a", &e), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(doc), e), GTEXT_MARKUP_OK);
  EXPECT_EQ(write_with(doc, with_bom(GTEXT_XML_BOM_AUTO)).bytes, "<a/>");
  gtext_markup_document_free(doc);
}

TEST(XmlBom, TheMarkCountsTowardTheOutputLimitAndIsRefusedWithTheRestBeforeAnythingIsWritten) {
  Doc * doc = parse("<a>0123456789</a>");
  const size_t plain = 17;
  Opts o = with_bom(GTEXT_XML_BOM_ALWAYS);
  o.max_output_bytes = plain + 3;
  EXPECT_EQ(write_with(doc, o).bytes, kBom + "<a>0123456789</a>");
  o.max_output_bytes = plain + 2;
  EXPECT_EQ(write_with(doc, o).status, GTEXT_XML_E_LIMIT_OUTPUT);
  // The same limit without the mark is enough.
  o.byte_order_mark = GTEXT_XML_BOM_NEVER;
  EXPECT_EQ(write_with(doc, o).status, GTEXT_XML_OK);
  gtext_markup_document_free(doc);
}

TEST(XmlBom, ThroughTheSinkAndThroughAFileAndPrettyPrinted) {
  Doc * doc = parse(kBom + "<a><b/></a>");
  struct Collect {
    std::string data;
    static GTEXT_Markup_Status write(void * u, const char * b, size_t n) {
      static_cast<Collect *>(u)->data.append(b, n);
      return GTEXT_MARKUP_OK;
    }
  } sink;
  const GTEXT_Markup_Sink s = {&Collect::write, &sink};
  ASSERT_EQ(gtext_xml_write(&s, doc, nullptr, nullptr), GTEXT_XML_OK);
  EXPECT_EQ(sink.data, kBom + "<a><b/></a>");

  Opts pretty = gtext_xml_write_options_default();
  pretty.indent = "  ";
  EXPECT_EQ(write_with(doc, pretty).bytes, kBom + "<a>\n  <b/>\n</a>\n");

  const std::string path = "build/xml-bom-test-" + std::to_string(::getpid()) + ".xml";
  ASSERT_EQ(gtext_xml_write_file(path.c_str(), doc, nullptr, nullptr), GTEXT_XML_OK);
  std::ifstream in(path, std::ios::binary);
  const std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(file, kBom + "<a><b/></a>");
  Doc * back = nullptr;
  ASSERT_EQ(gtext_xml_parse_file(path.c_str(), nullptr, &back, nullptr), GTEXT_XML_OK);
  GTEXT_Markup_Encoding_Source source;
  EXPECT_STREQ(gtext_markup_document_encoding(back, &source), "UTF-8");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);
  gtext_markup_document_free(back);
  std::remove(path.c_str());
  gtext_markup_document_free(doc);
}

// ---- the event parsers, however the mark is cut -------------------------------

std::string esc(const char * s, size_t n) {
  std::string out;
  char buf[8];
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x20 && c < 0x7F && c != '\\') {
      out += static_cast<char>(c);
    } else {
      std::snprintf(buf, sizeof buf, "\\x%02X", c);
      out += buf;
    }
  }
  return out;
}

std::string opt(const char * s) {
  return s ? "'" + esc(s, std::strlen(s)) + "'" : "-";
}

std::string describe(const GTEXT_XML_Event & e) {
  std::string out;
  switch (e.type) {
    case GTEXT_XML_EVENT_DOCUMENT_START:
      out = "DOCUMENT_START version=" + opt(e.version) + " encoding=" + opt(e.encoding)
          + " source=" + std::to_string(static_cast<int>(e.encoding_source))
          + " standalone=" + std::to_string(static_cast<int>(e.standalone));
      break;
    case GTEXT_XML_EVENT_DOCTYPE:
      out = "DOCTYPE " + opt(e.name);
      break;
    case GTEXT_XML_EVENT_ELEMENT_START:
      out = "START " + opt(e.local_name);
      break;
    case GTEXT_XML_EVENT_ELEMENT_END:
      out = "END " + opt(e.local_name);
      break;
    case GTEXT_XML_EVENT_TEXT:
      out = "TEXT '" + esc(e.data, e.data_length) + "'";
      break;
    case GTEXT_XML_EVENT_CDATA:
      out = "CDATA '" + esc(e.data, e.data_length) + "'";
      break;
    case GTEXT_XML_EVENT_COMMENT:
      out = "COMMENT '" + esc(e.data, e.data_length) + "'";
      break;
    case GTEXT_XML_EVENT_PROCESSING_INSTRUCTION:
      out = "PI " + opt(e.name);
      break;
    case GTEXT_XML_EVENT_DOCUMENT_END:
      out = "DOCUMENT_END";
      break;
    case GTEXT_XML_EVENT_SKIPPED_ENTITY:
      out = "SKIPPED_ENTITY " + opt(e.name);
      break;
    case GTEXT_XML_EVENT_UNREAD_RESOURCE:
      out = "UNREAD_RESOURCE "
          + std::to_string(static_cast<int>(e.resource_kind)) + " "
          + opt(e.name) + " " + opt(e.system_id);
      break;
  }
  return out + " @" + std::to_string(e.location.offset) + ":" + std::to_string(e.location.line) + ":"
      + std::to_string(e.location.column);
}

struct Trace {
  std::vector<std::string> log;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  GTEXT_XML_Error err{};
  bool have_position = false;
};

GTEXT_XML_Status record(const GTEXT_XML_Event * e, void * user) {
  static_cast<Trace *>(user)->log.push_back(describe(*e));
  return GTEXT_XML_OK;
}

// `cuts` are offsets at which the input is split; a one-byte run cuts at every byte.
Trace push(const std::string & data, std::vector<size_t> cuts, bool eager) {
  Trace run;
  GTEXT_XML_Stream * s = gtext_xml_stream_new(nullptr, record, &run);
  EXPECT_NE(s, nullptr);
  xml_stream_set_eager(s, eager);
  cuts.push_back(data.size());
  size_t at = 0;
  for (size_t cut : cuts) {
    if (cut > data.size() || cut < at) {
      continue;
    }
    if (cut > at) {
      run.status = gtext_xml_stream_feed(s, data.data() + at, cut - at, &run.err);
      at = cut;
      if (run.status != GTEXT_XML_OK) {
        run.have_position = true;
        gtext_xml_stream_free(s);
        return run;
      }
    }
  }
  run.status = gtext_xml_stream_finish(s, &run.err);
  run.have_position = run.status != GTEXT_XML_OK;
  gtext_xml_stream_free(s);
  return run;
}

Trace pull(const std::string & data, std::vector<size_t> cuts) {
  Trace run;
  GTEXT_XML_Reader * r = gtext_xml_reader_new(nullptr);
  EXPECT_NE(r, nullptr);
  auto drain = [&]() {
    for (;;) {
      GTEXT_XML_Event e;
      const GTEXT_XML_Status st = gtext_xml_reader_next(r, &e);
      if (st == GTEXT_XML_OK) {
        run.log.push_back(describe(e));
        continue;
      }
      return st;
    }
  };
  cuts.push_back(data.size());
  size_t at = 0;
  for (size_t cut : cuts) {
    if (cut > data.size() || cut <= at) {
      continue;
    }
    const GTEXT_XML_Status fed = gtext_xml_reader_feed(r, data.data() + at, cut - at, &run.err);
    at = cut;
    const GTEXT_XML_Status st = drain();
    if (fed != GTEXT_XML_OK) {
      run.status = fed;
      run.have_position = true;
      gtext_xml_reader_free(r);
      return run;
    }
    if (st != GTEXT_XML_E_INCOMPLETE) {
      run.status = st;
      gtext_xml_reader_free(r);
      return run;
    }
  }
  const GTEXT_XML_Status fed = gtext_xml_reader_feed(r, nullptr, 0, &run.err);
  const GTEXT_XML_Status st = drain();
  if (fed != GTEXT_XML_OK) {
    run.status = fed;
    run.have_position = true;
  } else {
    run.status = st == GTEXT_XML_E_STATE ? GTEXT_XML_OK : st;
  }
  gtext_xml_reader_free(r);
  return run;
}

void expect_same(const std::string & what, const Trace & want, const Trace & got) {
  EXPECT_EQ(got.status, want.status) << what;
  EXPECT_EQ(got.log, want.log) << what;
  if (want.status != GTEXT_XML_OK && want.have_position && got.have_position) {
    EXPECT_EQ(got.err.code, want.err.code) << what;
    EXPECT_STREQ(got.err.message, want.err.message) << what;
    EXPECT_EQ(got.err.byte_offset, want.err.byte_offset) << what;
    EXPECT_EQ(got.err.line, want.err.line) << what;
    EXPECT_EQ(got.err.column, want.err.column) << what;
  }
}

std::vector<size_t> every_byte(size_t n) {
  std::vector<size_t> cuts;
  for (size_t i = 1; i < n; i++) {
    cuts.push_back(i);
  }
  return cuts;
}

TEST(XmlBom, AMarkCutAnywhereGivesTheWholeInputsEventsEncodingAndLocations) {
  const std::vector<std::string> documents = {
      kBom + "<a>x<b/>\n</a>",
      kBom + "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<a>caf\xC3\xA9</a>",
      kBom + "<?xml version=\"1.0\"?><a/>",
      kBom + "\n<!--c--><a/>",
  };
  for (const std::string & doc : documents) {
    SCOPED_TRACE(doc);
    const Trace whole = push(doc, {}, false);
    ASSERT_EQ(whole.status, GTEXT_XML_OK);
    // The encoding and where it came from, in the first event.
    EXPECT_NE(whole.log[0].find("encoding='UTF-8' source=1 "), std::string::npos) << whole.log[0];
    // The mark is in the byte offsets and not in the columns: the first element
    // starts at column 1 of its line.
    for (const std::string & line : whole.log) {
      if (line.rfind("START 'a'", 0) == 0) {
        const bool first_line = doc.find('\n') == std::string::npos || doc.find("<a") < doc.find('\n');
        if (first_line) {
          EXPECT_NE(line.find(":1:"), std::string::npos) << line;
        }
      }
    }
    if (doc.compare(3, 2, "<a") == 0) {
      EXPECT_EQ(whole.log[1], "START 'a' @3:1:1");
    }

    // The DOM parse agrees about the document.
    Doc * dom = parse(doc);
    ASSERT_NE(dom, nullptr);
    GTEXT_Markup_Encoding_Source source;
    EXPECT_STREQ(gtext_markup_document_encoding(dom, &source), "UTF-8");
    EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);
    gtext_markup_document_free(dom);

    std::vector<std::vector<size_t>> shapes = {{1}, {2}, {3}, {1, 2}, {1, 3}, {2, 3}, {1, 2, 3}, {4}, {1, 2, 3, 4, 5, 6}};
    shapes.push_back(every_byte(doc.size()));
    for (const std::vector<size_t> & cuts : shapes) {
      for (bool eager : {false, true}) {
        std::string label = "push cuts=";
        for (size_t c : cuts) {
          label += std::to_string(c) + ",";
        }
        expect_same(label + (eager ? " eager" : ""), whole, push(doc, cuts, eager));
      }
      expect_same("pull", whole, pull(doc, cuts));
    }
    expect_same("pull whole", whole, pull(doc, {}));
  }
}

TEST(XmlBom, AMarkCutShortAtTheEndIsTheDomParsesError) {
  const std::vector<std::string> inputs = {
      "\xEF", "\xEF\xBB", kBom, kBom + "<", kBom + "<a", kBom + "<a>", kBom + "<?xml version=\"1.0\"",
      "\xEF\xBB<a/>", "\xEF<a/>", "\xEF\xBB\xBE<a/>", kBom + kBom + "<a/>",
  };
  for (const std::string & in : inputs) {
    SCOPED_TRACE(esc(in.data(), in.size()));
    GTEXT_XML_Error dom_err;
    Doc * dom = nullptr;
    const GTEXT_XML_Status dom_status = gtext_xml_parse(in.data(), in.size(), nullptr, &dom, &dom_err);
    EXPECT_NE(dom_status, GTEXT_XML_OK);
    Trace want;
    want.status = dom_status;
    want.err = dom_err;
    want.have_position = true;
    for (bool eager : {false, true}) {
      const Trace whole = push(in, {}, eager);
      const Trace bytes = push(in, every_byte(in.size()), eager);
      for (const Trace * run : {&whole, &bytes}) {
        EXPECT_EQ(run->status, dom_status);
        EXPECT_EQ(run->err.code, dom_err.code);
        EXPECT_STREQ(run->err.message, dom_err.message);
        EXPECT_EQ(run->err.byte_offset, dom_err.byte_offset);
        EXPECT_EQ(run->err.line, dom_err.line);
        EXPECT_EQ(run->err.column, dom_err.column);
      }
    }
    const Trace reader = pull(in, every_byte(in.size()));
    EXPECT_EQ(reader.status, dom_status);
    const Trace reader_whole = pull(in, {});
    EXPECT_EQ(reader_whole.status, dom_status);
  }
}

// ---- a mark and a declaration that disagree ------------------------------------

std::string utf16(const std::string & ascii, bool big, bool bom) {
  std::string s;
  if (bom) {
    s += big ? "\xFE\xFF" : "\xFF\xFE";
  }
  for (char c : ascii) {
    if (big) {
      s += '\0';
      s += c;
    } else {
      s += c;
      s += '\0';
    }
  }
  return s;
}

struct Conflict {
  std::string input;
  GTEXT_XML_Status status;
  const char * label;  // where the error is, if it is a label in a UTF-8 input
};

TEST(XmlBom, AMarkAndADeclarationThatDisagreeAreRefusedAlikeByTheDomPushAndPull) {
  auto declared = [](const char * label) {
    return std::string("<?xml version=\"1.0\" encoding=\"") + label + "\"?><a/>";
  };
  const std::vector<Conflict> cases = {
      {kBom + declared("UTF-8"), GTEXT_XML_OK, nullptr},
      {kBom + declared("utf-8"), GTEXT_XML_OK, nullptr},
      {kBom + "<?xml version=\"1.0\"?><a/>", GTEXT_XML_OK, nullptr},
      {kBom + declared("ISO-8859-1"), GTEXT_XML_E_ENCODING_MISMATCH, "ISO-8859-1"},
      {kBom + declared("US-ASCII"), GTEXT_XML_E_ENCODING_MISMATCH, "US-ASCII"},
      {kBom + declared("UTF-16"), GTEXT_XML_E_ENCODING_MISMATCH, "UTF-16"},
      {kBom + declared("UTF-32"), GTEXT_XML_E_ENCODING_MISMATCH, "UTF-32"},
      {utf16(declared("UTF-8"), false, true), GTEXT_XML_E_ENCODING_MISMATCH, nullptr},
      {utf16(declared("UTF-8"), true, true), GTEXT_XML_E_ENCODING_MISMATCH, nullptr},
      {utf16(declared("ISO-8859-1"), false, true), GTEXT_XML_E_ENCODING_MISMATCH, nullptr},
      {utf16(declared("UTF-32"), true, true), GTEXT_XML_E_ENCODING_MISMATCH, nullptr},
      {utf16(declared("UTF-16"), false, true), GTEXT_XML_OK, nullptr},
  };
  for (const Conflict & c : cases) {
    SCOPED_TRACE(esc(c.input.data(), c.input.size()));
    GTEXT_XML_Error dom_err;
    Doc * dom = nullptr;
    const GTEXT_XML_Status dom_status = gtext_xml_parse(c.input.data(), c.input.size(), nullptr, &dom, &dom_err);
    ASSERT_EQ(dom_status, c.status) << (dom_err.message ? dom_err.message : "");
    if (dom_status == GTEXT_XML_OK) {
      GTEXT_Markup_Encoding_Source source;
      ASSERT_NE(gtext_markup_document_encoding(dom, &source), nullptr);
      EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);
      gtext_markup_document_free(dom);
    } else if (c.label) {
      // Positioned at the label, in bytes of the input (the mark counted).
      EXPECT_EQ(dom_err.byte_offset, c.input.find(c.label));
      EXPECT_EQ(dom_err.line, 1u);
    }
    Trace want;
    want.status = dom_status;
    want.err = dom_err;
    want.have_position = true;
    const Trace reference = push(c.input, {}, false);
    for (bool eager : {false, true}) {
      for (const std::vector<size_t> & cuts : {std::vector<size_t>{}, std::vector<size_t>{1, 2, 3},
               every_byte(c.input.size())}) {
        const Trace run = push(c.input, cuts, eager);
        EXPECT_EQ(run.status, dom_status);
        if (dom_status != GTEXT_XML_OK) {
          EXPECT_EQ(run.err.code, dom_err.code);
          EXPECT_STREQ(run.err.message, dom_err.message);
          EXPECT_EQ(run.err.byte_offset, dom_err.byte_offset);
          EXPECT_EQ(run.err.line, dom_err.line);
          EXPECT_EQ(run.err.column, dom_err.column);
        } else {
          EXPECT_EQ(run.log, reference.log);
        }
      }
    }
    for (const std::vector<size_t> & cuts : {std::vector<size_t>{}, std::vector<size_t>{1, 2, 3},
             every_byte(c.input.size())}) {
      const Trace run = pull(c.input, cuts);
      EXPECT_EQ(run.status, dom_status);
      if (dom_status != GTEXT_XML_OK && run.have_position) {
        EXPECT_EQ(run.err.byte_offset, dom_err.byte_offset);
        EXPECT_EQ(run.err.column, dom_err.column);
      }
      if (dom_status == GTEXT_XML_OK) {
        EXPECT_EQ(run.log, reference.log);
      }
    }
  }
}

}  // namespace
