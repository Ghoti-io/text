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

/**
 * Whether a value read back is the value that was set.
 *
 * A dialect whose lines continue by indentation (configparser) stores the document's own bytes: the writer puts every line of a value after its first
 * behind the indentation that makes it a continuation, and a reader keeps that indentation in the stored value ("the joined form is the canonical form",
 * documentation/formats/ini.md), so `\n)` is written as `k=` and ` )` and read back as `\n )`. What is the same is the joined form of the two,
 * which gtext_ini_unescape() makes. Every other dialect keeps the bytes.
 */
bool same_value(const GTEXT_INI_Dialect & dialect, const char * read, size_t read_len, const std::string & set) {
  if (dialect.continuation != GTEXT_INI_CONTINUATION_INDENT) {
    return read_len == set.size() && (read_len == 0 || std::memcmp(read, set.data(), read_len) == 0);
  }
  std::string a, b;
  const std::string raw[2] = {std::string(read, read_len), set};
  std::string * dst[2] = {&a, &b};
  for (int k = 0; k < 2; k++) {
    char * out = nullptr;
    size_t n = 0;
    if (gtext_ini_unescape(&dialect, raw[k].data(), raw[k].size(), nullptr, &out, &n) != GTEXT_INI_OK) {
      return raw[0] == raw[1];
    }
    dst[k]->assign(out, n);
    gtext_ini_string_free(nullptr, out);
  }
  return a == b;
}

/** One entry the harness asked for and the DOM accepted. */
struct Entry {
  std::string key;
  std::string value;
};
struct Group {
  std::string name;
  /** The name a lookup answers to: for git the folded `section.subsection`, because `[a.V]` is the group `a.v` and is reachable by no other spelling (git does the same; documentation/formats/ini.md), and the header spelling for every other dialect. */
  std::string canon;
  std::vector<Entry> entries;
};

/**
 * The @p nth group of the document whose header names @p name.
 *
 * By name through gtext_ini_document_group() for every name but the empty one.
 * The empty name is the **preamble** for that lookup (`[]` then `k=v` with no
 * preamble answers nothing for `("", "k")`, measured against the reference
 * and pinned in tests/test-ini.cpp), so a `[]` group the DOM accepted and the
 * writer wrote is a real group that no lookup by name reaches. The harness
 * asked for it and so cannot lose it, and it finds it the way a caller must:
 * by position, among the groups that are not the preamble.
 */
/** The name a lookup answers to once a dialect that trims has read it back: the blanks around it are gone (`[\t.]` and `[.]` are two headers of one name). */
std::string trimmed_name(bool trims, std::string name) {
  if (!trims) return name;
  const size_t b = name.find_first_not_of(" \t\v\f");
  if (b == std::string::npos) return std::string();
  return name.substr(b, name.find_last_not_of(" \t\v\f") - b + 1);
}

const GTEXT_INI_Group * find_group(const GTEXT_INI_Document * doc,
    const std::string & name, const std::string & canon, size_t & nth) {
  if (!name.empty()) {
    const GTEXT_INI_Group * g = gtext_ini_document_group(doc, canon.c_str());
    if (g) return g;
    // a name of blanks only is trimmed to the empty name by a dialect that trims (`[\t]` is `[]`), and the lookup trims the query the same way: the same group
    // that no lookup by name reaches
    if (name.find_first_not_of(" \t\v\f") != std::string::npos) return nullptr;
  }
  const size_t n = gtext_ini_document_group_count(doc);
  size_t skip = nth;
  for (size_t i = 0; i < n; i++) {
    const GTEXT_INI_Group * g = gtext_ini_document_group_at(doc, i);
    if (gtext_ini_group_is_preamble(g)) continue;
    size_t len = 0;
    const char * nm = gtext_ini_group_name(g, &len);
    const bool blank = len == 0 || std::string(nm, len).find_first_not_of(" \t\v\f") == std::string::npos;
    if (blank && skip-- == 0) {
      nth++;
      return g;
    }
  }
  return nullptr;
}

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
    // the DOM takes a C string, so a NUL ends the name or the key it is given (the shadow records what the DOM was handed, not the bytes that were drawn)
    const std::string drawn((const char *)(data + i), have);
    const std::string a(drawn.c_str());
    i += have;

    if ((op & 1) == 0) {
      // A new group.
      GTEXT_INI_Group * g = nullptr;
      if (gtext_ini_document_add_group(doc, a.c_str(), &g) == GTEXT_INI_OK
          && g) {
        current = g;
        current_name = a;
        size_t clen = 0;
        const char * cn = gtext_ini_group_canonical_name(g, &clen);
        shadow.push_back(Group{a, cn ? std::string(cn, clen) : a, {}});
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
    // `crlf` is an option of the writer and not of the tree (tests/test-ini.cpp, IniWrite.CrlfIsAnOptionOfTheWriterNotOfTheTree), and a dialect that does not accept a CR
    // before an LF (Desktop Entry: "Desktop Entry §3 specifies linefeed") cannot read what it was asked to write; the reader is told to accept it, which changes nothing
    // else about the dialect.
    GTEXT_INI_Parse_Options back_opts = popts;
    if (wopts.crlf) back_opts.dialect.accept_crlf = true;
    GTEXT_INI_Document * back =
        gtext_ini_parse(out.data(), out.size(), &back_opts, nullptr);
    if (!back) {
      fail("the writer reported OK and wrote bytes that do not parse back "
           "under the same dialect",
          out);
    }

    // Every group the DOM accepted must come back, with its entries. Looked up
    // by name rather than by index, because a writer is allowed to order the
    // preamble and the groups as it sees fit.
    size_t empty_seen = 0;
    for (size_t gi = 0; gi < shadow.size(); gi++) {
      const Group & g = shadow[gi];
      // A dialect that keeps a repeated header as a group of its own (win32: "a key in a second `[a]` is retrievable by no name at all", pinned in tests/test-ini.cpp)
      // answers a lookup by name with the first, so the second is found as the group it is: the one with its spelling at its place among the groups of that spelling.
      // The header is the same by the name it is read back as, not its spelling: win32 trims the blanks around a name, so `[\t.]` and `[.]` are two headers of one name.
      const bool trims = popts.dialect.trim_group_name;
      const std::string read_as = trimmed_name(trims, g.canon);
      size_t same_before = 0;
      for (size_t k = 0; k < gi; k++) {
        if (trimmed_name(trims, shadow[k].canon) == read_as) same_before++;
      }
      const GTEXT_INI_Group * got = nullptr;
      if (popts.dialect.merge_duplicate_groups && !read_as.empty()) {
        // A dialect that merges repeated headers ("A duplicate group is accepted, and a lookup merges the two": ini.h; documentation/formats/ini.md) keeps the groups apart in the
        // tree and merges them in the lookup by name, gtext_ini_document_get(), which is where the promise is made: a key set in the second `[a]` is read through the document and
        // not through the first group object. What the lookup answers for a key that two of the groups hold is the dialect's repeated-key rule (the last occurrence for last-wins and
        // for collect, the first otherwise), so the harness expects the value of that occurrence among the groups of the name, in the order it added them.
        got = find_group(back, g.name, g.canon, empty_seen);
        if (!got) {
          std::fprintf(stderr, "group lost: %zu bytes\n", g.name.size());
          fail("a group the DOM accepted did not survive the round trip", out);
        }
        const bool last = popts.dialect.dupkey == GTEXT_INI_DUPKEY_LAST_WINS || popts.dialect.dupkey == GTEXT_INI_DUPKEY_COLLECT;
        for (const Entry & e : g.entries) {
          const std::string * want = nullptr;
          for (size_t k = 0; k < shadow.size(); k++) {
            if (trimmed_name(trims, shadow[k].canon) != read_as) continue;
            for (const Entry & other : shadow[k].entries) {
              if (other.key == e.key && (last || !want)) want = &other.value;
            }
          }
          size_t len = 0;
          const char * v = gtext_ini_document_get(back, g.canon.c_str(), e.key.c_str(), &len);
          if (!v) {
            std::fprintf(stderr, "key lost in group %s\n", g.name.c_str());
            fail("a key the DOM accepted did not survive the round trip", out);
          }
          if (!same_value(popts.dialect, v, len, *want)) {
            std::fprintf(stderr, "value changed: wrote %zu bytes, read %zu\n", want->size(), len);
            fail("a value changed across the round trip", out);
          }
        }
        continue;
      }
      if (same_before > 0 && !popts.dialect.merge_duplicate_groups && !read_as.empty()) {   // a blank name is found by its place among the blank ones, below
        size_t skip = same_before;
        for (size_t k = 0; k < gtext_ini_document_group_count(back) && !got; k++) {
          const GTEXT_INI_Group * cand = gtext_ini_document_group_at(back, k);
          size_t len = 0;
          const char * nm = gtext_ini_group_canonical_name(cand, &len);
          if (!gtext_ini_group_is_preamble(cand) && trimmed_name(trims, std::string(nm ? nm : "", len)) == read_as && skip-- == 0) got = cand;
        }
      }
      else {
        got = find_group(back, g.name, g.canon, empty_seen);
      }
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
        if (!same_value(popts.dialect, v, len, e.value)) {
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
