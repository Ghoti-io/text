/**
 * @file
 *
 * libFuzzer harness for the XML writer.
 *
 * The XML harness (fuzz_xml.cpp) reads: every document it writes came from a
 * parse, so every tree the writer saw was one the parser makes. The writer's
 * refusals - a comment holding `--`, a name that is not a name, a prefix with no
 * declaration, a character the output encoding lacks - are all reachable only
 * through the tree API, and a corpus of documents cannot phrase them. So the
 * input here is not a document: it is a **program** that builds a tree, and
 * options for the write.
 *
 * The properties:
 *
 *     if the writer says OK, the bytes parse, and parse to the tree that was
 *     written (up to the empty and adjacent text nodes XML cannot tell apart,
 *     which the result counts), and writing that tree again gives the same
 *     bytes;
 *     the sink writer and the buffer writer write the same bytes, or refuse
 *     with the same status, and a refusal writes nothing to the sink;
 *     a refusal has one of the documented statuses;
 *     an allocation that fails anywhere is OOM or the normal answer, never a
 *     leak; a sink that fails is E_WRITE;
 *
 * and in **strict** mode, where the program uses only what XML can spell, the
 * writer is required to say OK. A writer that is too strict is as much a defect
 * as one that is too lax, and "a refusal is always a permitted answer" - the
 * rule the other writer harnesses use - would never find one.
 *
 * Wild mode draws names, comments, instruction data and prefixes from lists that
 * include the unspellable ones (`--`, `?>`, `]]>`, a carriage return, `xml`, an
 * empty name, a prefix bound two ways), and there a refusal is permitted but an
 * acceptance is held to the round trip.
 *
 * Build with: make fuzz-xml-writer     Run: make fuzz-run-xml-writer
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <ghoti.io/text/xml.h>
}

#include "../../src/markup/markup_internal.h"
#include "../markup_test_support.h"

namespace {

using Node = GTEXT_Markup_Node;

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & program) {
  std::fprintf(stderr, "fuzz_xml_writer: %s\n%s\nprogram (%zu bytes): ", what, detail.c_str(), program.size());
  for (unsigned char c : program) {
    std::fprintf(stderr, "%02x", c);
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

struct Reader {
  const uint8_t * p;
  size_t n;
  size_t i = 0;
  bool done() const {
    return i >= n;
  }
  uint8_t byte() {
    return i < n ? p[i++] : 0;
  }
};

template <size_t N>
const char * pick(Reader & in, const char * const (&table)[N]) {
  return table[in.byte() % N];
}

const char * const kNames[] = {"a", "b", "x1", "_u", "a-b", "a.b", "doc", "item", "e\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC"};
const char * const kNamesAscii[] = {"a", "b", "x1", "_u", "a-b", "a.b", "doc", "item", "A", "z9"};
const char * const kNamesWild[] = {"a", "", "1a", "a b", "a:b", "-a", "a<", "xml", "xmlns", "b", "x\xC3\xA9", "_"};
const char * const kTexts[] = {"", "t", "hello world", " ", "caf\xC3\xA9", "x\ny", "<&>\"'", "]]>", "a\rb", "\t",
    "\xF0\x9F\x98\x80", "\xC2\x85", "\xE2\x80\xA8", "&amp;", "\xEF\xBF\xBD", "l1\r\nl2", "  lead", "trail  ", "a\tb\nc"};
const char * const kTextsAscii[] = {"", "t", "hello world", " ", "x\ny", "<&>\"'", "]]>", "a\rb", "\t", "&amp;",
    "l1\r\nl2", "  lead", "trail  ", "a\tb\nc"};
const char * const kTextsWild[] = {"", "t", "\x01", "a\x1f" "b", "\xEF\xBF\xBE", "\xEF\xBF\xBF", "ok", "\x0b", "x\xC2\x80y"};
const char * const kComments[] = {"", " c ", "text", "a-b", "x y z", "\xC3\xA9", "-->x"};
const char * const kCommentsAscii[] = {"", " c ", "text", "a-b", "x y z", "9", "ab cd"};
const char * const kCommentsWild[] = {"a--b", "-", "a-", "x\ry", "--", "ok", "\x01"};
const char * const kCdata[] = {"", "x", "a<b>&c", "]]", "]", "\xC3\xA9", "line\nline", "[[]]"};
const char * const kCdataAscii[] = {"", "x", "a<b>&c", "]]", "]", "line\nline", "[[]]", "z"};
const char * const kCdataWild[] = {"a]]>b", "a\rb", "]]>", "ok", "\x02"};
const char * const kPiData[] = {"", "d", "a b", "x?y", "\xC3\xA9", "k=\"v\""};
const char * const kPiDataAscii[] = {"", "d", "a b", "x?y", "k=\"v\"", "z"};
const char * const kPiDataWild[] = {"a?>b", " lead", "\tt", "x\ry", "?>", "ok"};
const char * const kTargets[] = {"pi", "t1", "a-b", "target", "_p"};
const char * const kTargetsWild[] = {"xml", "XML", "Xml", "a:b", "", "ok", "1"};
const char * const kPub[] = {"pub id", "-//W3C//DTD X//EN", "a b"};
const char * const kSys[] = {"s.dtd", "http://x/y", "a b.dtd"};
const char * const kSysWild[] = {"a\"b", "a'b", "a\"'b", "", "ok"};

struct Prefix {
  const char * prefix;
  const char * uri;
};
const Prefix kBindings[] = {{nullptr, nullptr}, {"p", "urn:a"}, {"q", "urn:b"}};
const Prefix kBindingsWild[] = {{nullptr, nullptr}, {"p", "urn:a"}, {"q", "urn:b"}, {nullptr, "urn:d"},
    {"xml", GTEXT_MARKUP_NS_XML}, {"xmlns", GTEXT_MARKUP_NS_XMLNS}, {"p", "urn:other"}, {"r", nullptr},
    {"xml", "urn:notxml"}, {nullptr, GTEXT_MARKUP_NS_XMLNS}};

struct Builder {
  Reader & in;
  GTEXT_Markup_Document * doc;
  bool strict;
  bool ascii; // names, comments, instruction data and CDATA must fit an 8-bit encoding
  size_t budget = 60; // nodes
  // ISO-8859-1 with no XML declaration: a character from U+0080 to U+00FF cannot be read
  // back, so the writer refuses it by design (xml_writer.h). Strict mode stays on there and
  // draws its text from ASCII, instead of being switched off for the whole run.
  bool ascii_text = false;

  const char * name() {
    if (!strict && in.byte() % 4 == 0) {
      return pick(in, kNamesWild);
    }
    return ascii ? pick(in, kNamesAscii) : pick(in, kNames);
  }
  const char * text() {
    if (!strict && in.byte() % 5 == 0) {
      return pick(in, kTextsWild);
    }
    return ascii_text ? pick(in, kTextsAscii) : pick(in, kTexts);
  }
  Prefix binding() {
    return strict ? kBindings[in.byte() % 3] : kBindingsWild[in.byte() % 10];
  }

  Node * element(int depth) {
    const Prefix b = binding();
    Node * e = nullptr;
    if (gtext_markup_create_element(doc, b.uri, b.prefix, name(), &e) != GTEXT_MARKUP_OK) {
      return nullptr;
    }
    const int attrs = in.byte() % 4;
    for (int i = 0; i < attrs; i++) {
      const uint8_t kind = in.byte() % 6;
      if (kind == 0) {
        // An explicit declaration, consistent with the table in strict mode.
        const Prefix d = strict ? kBindings[1 + in.byte() % 2] : kBindingsWild[in.byte() % 10];
        const char * value = d.uri ? d.uri : "urn:x";
        if (d.prefix) {
          gtext_markup_element_set_attribute(e, GTEXT_MARKUP_NS_XMLNS, "xmlns", d.prefix, value, strlen(value));
        }
        else if (!strict) {
          gtext_markup_element_set_attribute(e, GTEXT_MARKUP_NS_XMLNS, nullptr, "xmlns", value, strlen(value));
        }
      }
      else {
        const Prefix a = binding();
        const char * v = text();
        gtext_markup_element_set_attribute(e, a.uri, a.prefix, name(), v, strlen(v));
      }
    }
    if (depth < 6) {
      const int kids = in.byte() % 5;
      for (int i = 0; i < kids && budget > 0; i++) {
        budget--;
        Node * c = child(depth);
        if (c && gtext_markup_append_child(e, c) != GTEXT_MARKUP_OK) {
          gtext_markup_node_free(c);
        }
      }
    }
    return e;
  }

  Node * child(int depth) {
    Node * n = nullptr;
    const uint8_t kind = in.byte() % 8;
    switch (kind) {
      case 0:
      case 1: {
        const char * t = text();
        gtext_markup_create_text(doc, t, strlen(t), &n);
        break;
      }
      case 2: {
        const char * t = (!strict && in.byte() % 4 == 0) ? pick(in, kCdataWild)
            : ascii ? pick(in, kCdataAscii) : pick(in, kCdata);
        gtext_markup_create_cdata(doc, t, strlen(t), &n);
        break;
      }
      case 3: {
        const char * t = (!strict && in.byte() % 4 == 0) ? pick(in, kCommentsWild)
            : ascii ? pick(in, kCommentsAscii) : pick(in, kComments);
        if (strict && !strcmp(t, "-->x")) {
          t = "x";
        }
        gtext_markup_create_comment(doc, t, strlen(t), &n);
        break;
      }
      case 4: {
        const char * target = (!strict && in.byte() % 4 == 0) ? pick(in, kTargetsWild) : pick(in, kTargets);
        const char * t = (!strict && in.byte() % 4 == 0) ? pick(in, kPiDataWild)
            : ascii ? pick(in, kPiDataAscii) : pick(in, kPiData);
        gtext_markup_create_processing_instruction(doc, target, t, strlen(t), &n);
        break;
      }
      default:
        return element(depth + 1);
    }
    return n;
  }

  void misc(Node * root_node) {
    const int count = in.byte() % 3;
    for (int i = 0; i < count; i++) {
      Node * n = nullptr;
      if (in.byte() & 1) {
        const char * t = ascii ? pick(in, kCommentsAscii) : pick(in, kComments);
        if (!strcmp(t, "-->x")) {
          t = "y";
        }
        gtext_markup_create_comment(doc, t, strlen(t), &n);
      }
      else {
        const char * t = ascii ? pick(in, kPiDataAscii) : pick(in, kPiData);
        gtext_markup_create_processing_instruction(doc, pick(in, kTargets), t, strlen(t), &n);
      }
      if (n && gtext_markup_append_child(root_node, n) != GTEXT_MARKUP_OK) {
        gtext_markup_node_free(n);
      }
    }
  }
};

// A canonical form of a tree that does not distinguish what XML cannot tell apart,
// or what the writer is allowed to add: an empty text node reads back as nothing and
// a text node next to another as one; namespace declarations the writer was asked to
// supply (declare_namespaces) are attributes the source never had. Neither the tree
// nor the document is changed to compute it.
void canon_value(std::string & out, const char * v, size_t n) {
  out += '"';
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = static_cast<unsigned char>(v[i]);
    if (c == '"' || c == '\\' || c < 0x20) {
      char h[8];
      std::snprintf(h, sizeof h, "\\x%02x", c);
      out += h;
    }
    else {
      out += static_cast<char>(c);
    }
  }
  out += '"';
}

// A run of character data into the canonical form. With `drop_blank` a run of white space
// alone is dropped: pretty printing adds exactly such runs between elements, and nothing
// else, so a tree compared modulo them is a tree compared modulo the indentation.
void flush_text(std::string & out, std::string & pending, bool & have_text, bool drop_blank) {
  if (have_text && !pending.empty()
      && !(drop_blank && pending.find_first_not_of(" \t\r\n") == std::string::npos)) {
    out += "T";
    canon_value(out, pending.data(), pending.size());
    out += "\n";
  }
  pending.clear();
  have_text = false;
}

void canon(std::string & out, const Node * node, bool drop_blank) {
  std::string pending;
  bool have_text = false;
  for (const Node * c = gtext_markup_node_first_child(node); c; c = gtext_markup_node_next_sibling(c)) {
    const GTEXT_Markup_Kind kind = gtext_markup_node_kind(c);
    size_t len = 0;
    const char * d = gtext_markup_node_data(c, &len);
    if (kind == GTEXT_MARKUP_KIND_TEXT) {
      pending.append(d, len);
      have_text = true;
      continue;
    }
    flush_text(out, pending, have_text, drop_blank);
    switch (kind) {
      case GTEXT_MARKUP_KIND_CDATA:
        out += "C";
        canon_value(out, d, len);
        out += "\n";
        break;
      case GTEXT_MARKUP_KIND_COMMENT:
        out += "M";
        canon_value(out, d, len);
        out += "\n";
        break;
      case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
        out += "P ";
        out += gtext_markup_pi_target(c);
        canon_value(out, d, len);
        out += "\n";
        break;
      }
      case GTEXT_MARKUP_KIND_DOCTYPE: {
        const char * n = gtext_markup_doctype_name(c);
        const char * p = gtext_markup_doctype_public_id(c);
        const char * s = gtext_markup_doctype_system_id(c);
        out += std::string("D ") + (n ? n : "-") + " " + (p ? p : "-") + " " + (s ? s : "-") + "\n";
        break;
      }
      case GTEXT_MARKUP_KIND_ELEMENT: {
        const char * ns = gtext_markup_element_namespace(c);
        const char * pre = gtext_markup_element_prefix(c);
        out += std::string("<{") + (ns ? ns : "") + "}" + (pre ? pre : "") + ":" + gtext_markup_element_local_name(c);
        std::vector<std::string> attrs;
        for (size_t i = 0; i < gtext_markup_element_attribute_count(c); i++) {
          GTEXT_Markup_Attribute a;
          gtext_markup_element_attribute_at(c, i, &a);
          if (a.ns && std::strcmp(a.ns, GTEXT_MARKUP_NS_XMLNS) == 0) {
            continue;
          }
          std::string one = std::string(" {") + (a.ns ? a.ns : "") + "}" + (a.prefix ? a.prefix : "") + ":" + a.local + "=";
          canon_value(one, a.value, a.value_length);
          attrs.push_back(one);
        }
        std::sort(attrs.begin(), attrs.end());
        for (const std::string & one : attrs) {
          out += one;
        }
        out += ">\n";
        canon(out, c, drop_blank);
        out += "</>\n";
        break;
      }
      default:
        break;
    }
  }
  flush_text(out, pending, have_text, drop_blank);
}

struct Collect {
  std::string bytes;
  size_t fail_after = SIZE_MAX; // refuse once this many bytes have been taken
  static GTEXT_Markup_Status write(void * user, const char * b, size_t len) {
    auto * self = static_cast<Collect *>(user);
    if (self->bytes.size() + len > self->fail_after) {
      return GTEXT_MARKUP_E_INVALID;
    }
    self->bytes.append(b, len);
    return GTEXT_MARKUP_OK;
  }
};

bool refusal_allowed(GTEXT_XML_Status s) {
  switch (s) {
    case GTEXT_XML_E_UNREPRESENTABLE:
    case GTEXT_XML_E_CHARACTER:
    case GTEXT_XML_E_NAME:
    case GTEXT_XML_E_NAMESPACE:
    case GTEXT_XML_E_DOCTYPE:
    case GTEXT_XML_E_UNSUPPORTED_VERSION:
    case GTEXT_XML_E_LIMIT_DEPTH:
    case GTEXT_XML_E_LIMIT_OUTPUT:
      return true;
    default:
      return false;
  }
}

const GTEXT_XML_Output_Encoding kEncodings[] = {GTEXT_XML_OUTPUT_UTF8, GTEXT_XML_OUTPUT_UTF16LE,
    GTEXT_XML_OUTPUT_UTF16BE, GTEXT_XML_OUTPUT_ISO_8859_1, GTEXT_XML_OUTPUT_US_ASCII};
const char * const kIndents[] = {nullptr, "", " ", "\t", "  "};
const char * const kNewlines[] = {nullptr, "\r\n", "\r", "\n\n"};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 4) {
    return 0;
  }
  const std::string program(reinterpret_cast<const char *>(data), size);
  Reader in{data, size};
  const uint8_t m1 = in.byte();
  const uint8_t m2 = in.byte();
  const uint8_t m3 = in.byte();

  GTEXT_XML_Write_Options o = gtext_xml_write_options_default();
  o.encoding = kEncodings[m1 % 5];
  o.xml_declaration = static_cast<GTEXT_XML_Declaration_Mode>((m1 >> 3) % 3);
  o.expand_empty_elements = (m1 >> 5) & 1;
  o.indent = kIndents[m2 % 5];
  o.newline = kNewlines[(m2 >> 3) % 4];
  const bool eight_bit = o.encoding == GTEXT_XML_OUTPUT_ISO_8859_1 || o.encoding == GTEXT_XML_OUTPUT_US_ASCII;
  const bool strict = (m3 & 1) != 0;
  o.declare_namespaces = strict ? true : ((m3 >> 1) & 1);
  const bool faults = (m3 & 0x20) != 0;
  const bool sink_fails = (m3 & 0x40) != 0;

  GTEXT_Markup_Document * doc = nullptr;
  if (gtext_markup_document_new(nullptr, &doc) != GTEXT_MARKUP_OK) {
    return 0;
  }
  Builder b{in, doc, strict, eight_bit};
  b.ascii_text = o.encoding == GTEXT_XML_OUTPUT_ISO_8859_1 && o.xml_declaration == GTEXT_XML_DECLARATION_NEVER;
  Node * root = gtext_markup_document_root(doc);

  if (in.byte() % 3 == 0) {
    gtext_markup_document_set_xml_declaration(doc, (!strict && in.byte() % 7 == 0) ? "1.1" : "1.0",
        static_cast<GTEXT_Markup_Standalone>(in.byte() % 3));
  }
  b.misc(root);
  if (in.byte() % 3 == 0) {
    Node * dt = nullptr;
    const char * pub = (in.byte() & 1) ? pick(in, kPub) : nullptr;
    const char * sys = pub ? pick(in, kSys) : ((in.byte() & 1) ? pick(in, kSys) : nullptr);
    if (!strict && in.byte() % 4 == 0) {
      sys = pick(in, kSysWild);
    }
    if (gtext_markup_create_doctype(doc, b.name(), pub, sys, &dt) == GTEXT_MARKUP_OK) {
      if (gtext_markup_append_child(root, dt) != GTEXT_MARKUP_OK) {
        gtext_markup_node_free(dt);
      }
    }
  }
  Node * top = b.element(0);
  if (top && gtext_markup_append_child(root, top) != GTEXT_MARKUP_OK) {
    gtext_markup_node_free(top);
  }
  b.misc(root);
  if (!strict && in.byte() % 9 == 0) {
    Node * second = b.element(3);
    if (second && gtext_markup_append_child(root, second) != GTEXT_MARKUP_OK) {
      gtext_markup_node_free(second);
    }
  }

  const char * problem = markup_check_invariants(doc);
  if (problem) {
    fail("the tree the program built breaks an invariant", problem, program);
  }

  // ---- the two writers --------------------------------------------------------
  Collect sink;
  GTEXT_Markup_Sink s{&Collect::write, &sink};
  GTEXT_XML_Write_Result r1;
  const GTEXT_XML_Status st1 = gtext_xml_write(&s, doc, &o, &r1);
  char * buf = nullptr;
  size_t buf_len = 0;
  GTEXT_XML_Write_Result r2;
  const GTEXT_XML_Status st2 = gtext_xml_write_buffer(doc, &o, &buf, &buf_len, &r2);
  if (st1 != st2) {
    fail("the sink writer and the buffer writer disagree", std::string(gtext_xml_status_string(st1)) + " against "
        + gtext_xml_status_string(st2), program);
  }
  if (strict && st1 != GTEXT_XML_OK) {
    fail("the writer refused a tree XML can spell", std::string(gtext_xml_status_string(st1)) + ": "
        + (r1.message ? r1.message : "?") + " at " + r1.path, program);
  }

  if (st1 != GTEXT_XML_OK) {
    if (!refusal_allowed(st1)) {
      fail("a refusal with a status the writer does not document", gtext_xml_status_string(st1), program);
    }
    if (!sink.bytes.empty()) {
      fail("a refused write put bytes in the sink", std::to_string(sink.bytes.size()), program);
    }
    if (buf) {
      fail("a refused write left a buffer", "", program);
    }
  }
  else {
    if (!buf || buf_len != sink.bytes.size() || std::memcmp(buf, sink.bytes.data(), buf_len) != 0) {
      fail("the sink writer and the buffer writer wrote different bytes", "", program);
    }
    GTEXT_Markup_Document * back = nullptr;
    GTEXT_XML_Error err;
    const GTEXT_XML_Status ps = gtext_xml_parse(buf, buf_len, nullptr, &back, &err);
    if (ps != GTEXT_XML_OK) {
      fail("what the writer wrote does not parse", std::string(gtext_xml_status_string(ps)) + " at "
          + std::to_string(err.byte_offset) + ": " + (err.message ? err.message : "?"), program);
    }
    {
      // The contract: the tree comes back, up to what XML cannot tell apart - and, when the
      // write was pretty printed, up to the white space that adds.
      std::string want, got;
      canon(want, gtext_markup_document_root(doc), o.indent != nullptr);
      canon(got, gtext_markup_document_root(back), o.indent != nullptr);
      if (want != got) {
        std::string hex;
        for (size_t i = 0; i < buf_len; i++) {
          char h[8];
          std::snprintf(h, sizeof h, (buf[i] >= 0x20 && buf[i] < 0x7f) ? "%c" : "\\x%02x", (unsigned char) buf[i]);
          hex += h;
        }
        fail("what the writer wrote parses to a different tree", "written: " + hex + "\nsource:\n" + want
            + "read back:\n" + got, program);
      }
      // And exactly, where the writer reports it added and merged nothing.
      if (!o.indent && r1.text_nodes_coalesced == 0 && r1.namespace_declarations_added == 0
          && !gtext_markup_equal(gtext_markup_document_root(doc), gtext_markup_document_root(back))) {
        fail("the writer reported no change and the tree is not equal", dump_to_string(gtext_markup_document_root(doc))
            + "read back:\n" + dump_to_string(gtext_markup_document_root(back)), program);
      }
      if (!o.indent) {
        char * again = nullptr;
        size_t again_len = 0;
        GTEXT_XML_Write_Result r3;
        if (gtext_xml_write_buffer(back, &o, &again, &again_len, &r3) != GTEXT_XML_OK || again_len != buf_len
            || std::memcmp(again, buf, buf_len) != 0) {
          fail("writing what was read is not the same bytes", r3.message ? r3.message : "", program);
        }
        gtext_xml_write_buffer_free(back, &o, again);
      }
    }
    gtext_markup_document_free(back);
    gtext_xml_error_free(&err);
  }
  if (buf) {
    gtext_xml_write_buffer_free(doc, &o, buf);
  }

  // ---- an allocator that fails, and a sink that does ------------------------------
  if (faults) {
    FaultAllocator fa;
    GTEXT_XML_Write_Options fo = o;
    fo.allocator = &fa.alloc;
    fa.arm(1 + (static_cast<size_t>(m1) * 3 + m2) % 30);
    char * fbuf = nullptr;
    size_t flen = 0;
    GTEXT_XML_Write_Result fr;
    const GTEXT_XML_Status fs = gtext_xml_write_buffer(doc, &fo, &fbuf, &flen, &fr);
    const bool failed = fa.failed;
    fa.disarm();
    if (fs == GTEXT_XML_E_OOM) {
      if (!failed) {
        fail("OOM reported with no allocation failed", "", program);
      }
      if (fbuf) {
        fail("an OOM write left a buffer", "", program);
      }
    }
    else if (fs != st1) {
      fail("an allocation failure changed the answer", std::string(gtext_xml_status_string(fs)) + " against "
          + gtext_xml_status_string(st1), program);
    }
    if (fbuf) {
      gtext_xml_write_buffer_free(doc, &fo, fbuf);
    }
    if (!fa.live.empty() || fa.bad_frees != 0) {
      fail("the writer leaked or double-freed", std::to_string(fa.live.size()) + " blocks, "
          + std::to_string(fa.bad_frees) + " bad frees", program);
    }
  }
  if (sink_fails && st1 == GTEXT_XML_OK && !sink.bytes.empty()) {
    // A sink that refuses after N bytes, for N across the output (and at the 4 KiB chunk the
    // writer cuts at): the write must report E_WRITE and the sink must not have been given more
    // than it accepted.
    const size_t n = sink.bytes.size();
    std::vector<size_t> offsets = {0, 1, n / 3, n / 2, n - 1, 4095, 4096, 4097};
    for (size_t at : offsets) {
      if (at >= n) {
        continue;
      }
      Collect limited;
      limited.fail_after = at;
      GTEXT_Markup_Sink ls{&Collect::write, &limited};
      GTEXT_XML_Write_Result lr;
      const GTEXT_XML_Status ls_status = gtext_xml_write(&ls, doc, &o, &lr);
      if (ls_status != GTEXT_XML_E_WRITE) {
        fail("a sink that refuses was not reported as E_WRITE", std::string(gtext_xml_status_string(ls_status))
            + " after " + std::to_string(at) + " bytes", program);
      }
      if (limited.bytes.size() > at || sink.bytes.compare(0, limited.bytes.size(), limited.bytes) != 0) {
        fail("a sink that refused was handed more than it took, or bytes the writer does not write",
            std::to_string(at), program);
      }
    }
  }
  if (faults && st1 == GTEXT_XML_OK) {
    // The sink writer under the failing allocator, as the buffer writer was above: OOM or the
    // normal bytes, never a leak, and what the sink was given is a prefix of the normal output.
    FaultAllocator sa;
    GTEXT_XML_Write_Options so = o;
    so.allocator = &sa.alloc;
    sa.arm(1 + (static_cast<size_t>(m2) * 5 + m3) % 30);
    Collect got;
    GTEXT_Markup_Sink gs{&Collect::write, &got};
    GTEXT_XML_Write_Result gr;
    const GTEXT_XML_Status gst = gtext_xml_write(&gs, doc, &so, &gr);
    const bool sfailed = sa.failed;
    sa.disarm();
    if (gst == GTEXT_XML_E_OOM) {
      if (!sfailed) {
        fail("OOM reported with no allocation failed", "sink writer", program);
      }
      if (sink.bytes.compare(0, got.bytes.size(), got.bytes) != 0) {
        fail("an OOM write put bytes in the sink that the writer does not write", "", program);
      }
    }
    else if (gst != GTEXT_XML_OK || got.bytes != sink.bytes) {
      fail("an allocation failure changed the sink writer's answer", gtext_xml_status_string(gst), program);
    }
    if (!sa.live.empty() || sa.bad_frees != 0) {
      fail("the sink writer leaked or double-freed", std::to_string(sa.live.size()) + " blocks, "
          + std::to_string(sa.bad_frees) + " bad frees", program);
    }
  }

  gtext_markup_document_free(doc);
  return 0;
}
