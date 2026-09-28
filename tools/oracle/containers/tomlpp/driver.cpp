/*
 * Read a batch of TOML documents with toml++, and write what each one means as
 * toml-test tagged JSON.
 *
 * Runs *inside* the pinned image; tools/oracle/toml_1_1_diff.py on the host
 * generates the documents, asks this library the same questions through the
 * toml-test runner, and compares. Tagged JSON is the encoding both sides speak
 * already, and it is the one that tells an integer from a float and a date-time
 * from a string.
 *
 * `TOML_ENABLE_UNRELEASED_FEATURES` is what makes this a second reader for the
 * relaxations v1.1.0 makes. toml++ 3.4.0's set is not v1.1.0's - see SOURCES and
 * ../IMAGES - so the comparison is kept to the constructs both target, on the
 * host side, by name.
 *
 * The framing and the first line are `toml_ask.py`'s, so that the two references
 * answer the same protocol: a decimal byte count on its own line, then that many
 * bytes; one `ok <json>` or `err <text>` line per document, in order. A byte
 * count rather than a line, because a TOML document contains newlines.
 *
 * GTEXT_DRIVER_SHA is this file's own SHA-256, put in by the Dockerfile. It is in
 * the version line because an image carrying a driver compiled from an older copy
 * of this file would answer confidently and wrongly.
 *
 * Copyright 2026 by Corey Pennycuff
 */
#define TOML_ENABLE_UNRELEASED_FEATURES 1
#define TOML_EXCEPTIONS 0
#include "toml.hpp"
#include <cstdio>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

#ifndef GTEXT_DRIVER_SHA
#define GTEXT_DRIVER_SHA "unknown"
#endif

static void json_string(std::ostream & out, std::string_view text) {
  out << '"';
  for (unsigned char c : text) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20 || c == 0x7F) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04X", c);
          out << buf;
        } else {
          out << (char) c;
        }
    }
  }
  out << '"';
}

static void tagged(std::ostream & out, const char * type, const std::string & value) {
  out << "{\"type\":\"" << type << "\",\"value\":";
  json_string(out, value);
  out << '}';
}

static void emit(std::ostream & out, const toml::node & node);

static void emit(std::ostream & out, const toml::node & node) {
  if (auto t = node.as_table()) {
    out << '{';
    bool first = true;
    for (auto && [k, v] : *t) {
      if (!first) out << ',';
      first = false;
      json_string(out, std::string_view(k));
      out << ':';
      emit(out, v);
    }
    out << '}';
    return;
  }
  if (auto a = node.as_array()) {
    out << '[';
    bool first = true;
    for (auto && v : *a) {
      if (!first) out << ',';
      first = false;
      emit(out, v);
    }
    out << ']';
    return;
  }
  if (auto s = node.as_string()) {
    tagged(out, "string", s->get());
    return;
  }
  if (auto i = node.as_integer()) {
    tagged(out, "integer", std::to_string(i->get())); return;
  }
  if (auto f = node.as_floating_point()) {
    char buf[64];
    double d = f->get();
    if (d != d) std::snprintf(buf, sizeof(buf), "nan");
    else if (d == std::numeric_limits<double>::infinity()) std::snprintf(buf, sizeof(buf), "inf");
    else if (d == -std::numeric_limits<double>::infinity()) std::snprintf(buf, sizeof(buf), "-inf");
    else std::snprintf(buf, sizeof(buf), "%.17g", d);
    tagged(out, "float", buf); return;
  }
  if (auto b = node.as_boolean()) {
    tagged(out, "bool", b->get() ? "true" : "false"); return;
  }
  std::ostringstream text;
  if (auto d = node.as_date()) { text << d->get(); tagged(out, "date-local", text.str()); return; }
  if (auto t = node.as_time()) { text << t->get(); tagged(out, "time-local", text.str()); return; }
  if (auto dt = node.as_date_time()) {
    text << dt->get();
    tagged(out, dt->get().offset.has_value() ? "datetime" : "datetime-local", text.str());
    return;
  }
  out << "null";
}

static std::string what_i_am(void) {
  std::ostringstream out;
  out << "toml++ " << TOML_LIB_MAJOR << "." << TOML_LIB_MINOR << "."
      << TOML_LIB_PATCH << ", unreleased features on, driver "
      << GTEXT_DRIVER_SHA;
  return out.str();
}

int main(int argc, char ** argv) {
  /* `--version` writes the claim and nothing else, which is what the pin check
   * reads. The batch below prefixes it with `version ` as the protocol's first
   * line; asking for it that way would put the word into the version. */
  if (argc > 1 && std::string(argv[1]) == "--version") {
    std::cout << what_i_am() << "\n";
    return 0;
  }
  std::string all((std::istreambuf_iterator<char>(std::cin)),
                  std::istreambuf_iterator<char>());
  std::cout << "version " << what_i_am() << "\n";
  size_t pos = 0;
  while (pos < all.size()) {
    size_t nl = all.find('\n', pos);
    if (nl == std::string::npos) break;
    size_t count = (size_t) std::stoull(all.substr(pos, nl - pos));
    pos = nl + 1;
    if (pos + count > all.size()) { std::cout << "err short read\n"; break; }
    std::string_view doc(all.data() + pos, count);
    pos += count;
    auto res = toml::parse(doc);
    if (!res) {
      std::ostringstream why;
      why << res.error().description();
      std::string text = why.str();
      for (char & c : text) if (c == '\n') c = ' ';
      std::cout << "err " << text << "\n";
      continue;
    }
    std::ostringstream out;
    emit(out, res.table());
    std::cout << "ok " << out.str() << "\n";
  }
  return 0;
}
