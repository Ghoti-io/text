/**
 * @file
 *
 * libFuzzer harness for the INI writer.
 *
 * fuzz_ini.cpp parses, so `gtext_ini_write()` is reached only with documents a
 * parse produced - and that is the bound that has mattered most here, because
 * a parse has already refused everything the writer would have to refuse. The
 * group names and keys reachable only through `gtext_ini_new()`,
 * `gtext_ini_document_add_group()` and `gtext_ini_group_set()` are the ones
 * worth asking about: a name holding `]`, a newline, a NUL or bytes that are
 * not UTF-8, and a value holding a line break.
 *
 * The property:
 *
 *     if the writer says OK, the bytes it wrote must parse under the same
 *     dialect, and must hold the same groups, keys and values.
 *
 * Which is the round trip stated over documents nobody chose. The DOM API's own
 * refusals are the other half of what is under test: `gtext_ini_group_set()`
 * enforces the dialect's key rules, so a name the writer could not round-trip
 * should be refused on the way in rather than written and lost - and this
 * harness fails either way round, because it only asserts the round trip for
 * what the DOM accepted.
 *
 * Seven dialects, from the first byte, because a key or a group name legal in
 * one is not legal in all of them and the writer's quoting and escaping differ.
 *
 * Build with: make fuzz-ini-writer      Run: make fuzz-run-ini-writer
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <ghoti.io/text/ini.h>
}

namespace {

[[noreturn]] void fail(const char * what, const std::string & out) {
  std::fprintf(stderr, "%s\noutput was (%zu bytes): ", what, out.size());
  for (unsigned char c : out) {
    if (c >= 0x20 && c < 0x7f) {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fputc('\n', stderr);
  std::abort();
}

/** One entry the harness asked for and the DOM accepted. */
struct Entry {
  std::string key;
  std::string value;
};
struct Group {
  std::string name;
  std::vector<Entry> entries;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) return 0;

  GTEXT_INI_Dialect (*const dialects[])(void) = {
      gtext_ini_dialect_generic,
      gtext_ini_dialect_git_config,
      gtext_ini_dialect_desktop_entry,
      gtext_ini_dialect_systemd,
      gtext_ini_dialect_editorconfig,
      gtext_ini_dialect_win32,
      gtext_ini_dialect_configparser,
  };
  const size_t ndialects = sizeof dialects / sizeof dialects[0];

  GTEXT_INI_Parse_Options popts = gtext_ini_parse_options_default();
  popts.dialect = dialects[data[0] % ndialects]();
  popts.max_total_bytes = 0;

  GTEXT_INI_Write_Options wopts = gtext_ini_write_options_default();
  const uint8_t wflags = data[1];
  wopts.crlf = (wflags & 0x01) != 0;
  wopts.emit_comments = (wflags & 0x02) != 0;
  wopts.normalize = (wflags & 0x04) != 0;

  GTEXT_INI_Document * doc = gtext_ini_new(&popts);
  if (!doc) return 0;

  // Build groups and entries from the input, keeping only what the DOM accepts.
  // A refusal is the dialect's key or name rule doing its job, so the shadow
  // records nothing for it and the round trip below makes no claim about it.
  std::vector<Group> shadow;
  size_t i = 2;
  GTEXT_INI_Group * current = nullptr;
  std::string current_name;

  while (i < size) {
    const uint8_t op = data[i++];
    const size_t len = (i < size) ? (data[i++] % 8u) : 0u;
    const size_t have = (size - i < len) ? (size - i) : len;
    const std::string a((const char *)(data + i), have);
    i += have;

    if ((op & 1) == 0) {
      // A new group.
      GTEXT_INI_Group * g = nullptr;
      if (gtext_ini_document_add_group(doc, a.c_str(), &g) == GTEXT_INI_OK
          && g) {
        current = g;
        current_name = a;
        shadow.push_back(Group{a, {}});
      }
    }
    else {
      // An entry in the current group, if there is one.
      const size_t vlen = (i < size) ? (data[i++] % 8u) : 0u;
      const size_t vhave = (size - i < vlen) ? (size - i) : vlen;
      const std::string v((const char *)(data + i), vhave);
      i += vhave;
      if (current
          && gtext_ini_group_set(current, a.c_str(), v.data(), v.size())
              == GTEXT_INI_OK) {
        // set() replaces, so the shadow must replace too rather than append.
        bool replaced = false;
        for (Entry & e : shadow.back().entries) {
          if (e.key == a) {
            e.value = v;
            replaced = true;
            break;
          }
        }
        if (!replaced) shadow.back().entries.push_back(Entry{a, v});
      }
    }
  }

  GTEXT_INI_Sink sink;
  if (gtext_ini_sink_buffer(&sink) != GTEXT_INI_OK) {
    gtext_ini_free(doc);
    return 0;
  }
  const GTEXT_INI_Status st = gtext_ini_write(doc, &sink, &wopts);
  const std::string out(
      gtext_ini_sink_buffer_data(&sink), gtext_ini_sink_buffer_size(&sink));

  if (st == GTEXT_INI_OK && !shadow.empty()) {
    GTEXT_INI_Document * back =
        gtext_ini_parse(out.data(), out.size(), &popts, nullptr);
    if (!back) {
      fail("the writer reported OK and wrote bytes that do not parse back "
           "under the same dialect",
          out);
    }

    // Every group the DOM accepted must come back, with its entries. Looked up
    // by name rather than by index, because a writer is allowed to order the
    // preamble and the groups as it sees fit.
    for (const Group & g : shadow) {
      const GTEXT_INI_Group * got = gtext_ini_document_group(back, g.name.c_str());
      if (!got) {
        std::fprintf(stderr, "group lost: %zu bytes\n", g.name.size());
        fail("a group the DOM accepted did not survive the round trip", out);
      }
      for (const Entry & e : g.entries) {
        size_t len = 0;
        const char * v = gtext_ini_group_get(got, e.key.c_str(), &len);
        if (!v) {
          std::fprintf(stderr, "key lost in group %s\n", g.name.c_str());
          fail("a key the DOM accepted did not survive the round trip", out);
        }
        if (len != e.value.size()
            || (len && std::memcmp(v, e.value.data(), len) != 0)) {
          std::fprintf(stderr, "value changed: wrote %zu bytes, read %zu\n",
              e.value.size(), len);
          fail("a value changed across the round trip", out);
        }
      }
    }
    gtext_ini_free(back);
  }

  gtext_ini_sink_buffer_free(&sink);
  gtext_ini_free(doc);
  return 0;
}
