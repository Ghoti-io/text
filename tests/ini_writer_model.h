/**
 * @file
 *
 * What the INI writer promises, as one oracle that the fuzzer and the sweep share.
 *
 *     A group or a key the document model accepts, written by gtext_ini_write(), is either refused by the writer with
 *     GTEXT_INI_E_UNREPRESENTABLE or read back, under the dialect it was written for, as the group and the key and the value that were
 *     given (the value of the occurrence the dialect's repeated-key rule selects), up to the normalizations the dialect itself defines.
 *
 * The normalizations the dialect defines, and the only ones: the joined form of a value that continues by indentation (configparser:
 * the indentation of a continuation line is part of the stored text, and the joined form is the value), the lower case of a key under
 * a dialect that folds (git, EditorConfig, configparser), the lower case of the section of a git subsection spelled the old way, the
 * blanks around a header name under a dialect that trims it (win32), and the merge of repeated headers under a dialect that merges
 * them (a lookup goes through the document). Everything else is the library's defect.
 *
 * tests/fuzz/fuzz_ini_writer.cpp draws its steps from the bytes of its input, and tests/test-ini-sweep.cpp from a table of boundary
 * strings. Both call check() and nothing else, so a rule learned from one is the other's too and the two cannot disagree about what the
 * writer owes.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_TEXT_TESTS_INI_WRITER_MODEL_H
#define GHOTI_TEXT_TESTS_INI_WRITER_MODEL_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <ghoti.io/text/ini.h>
}

namespace ini_model {

struct DialectEntry {
  const char * name;
  GTEXT_INI_Dialect (*make)(void);
};

/** The seven dialects, in the order the fuzzer's first byte selects them. */
inline const std::vector<DialectEntry> & dialects() {
  static const std::vector<DialectEntry> table = {
      {"generic", gtext_ini_dialect_generic},
      {"git", gtext_ini_dialect_git_config},
      {"desktop-entry", gtext_ini_dialect_desktop_entry},
      {"systemd", gtext_ini_dialect_systemd},
      {"editorconfig", gtext_ini_dialect_editorconfig},
      {"win32", gtext_ini_dialect_win32},
      {"configparser", gtext_ini_dialect_configparser},
  };
  return table;
}

/** One call of the document model: a group added, or a key set, or a key appended. `name` is the group's name or the key, and the model reads it as the C string the API takes. */
struct Step {
  enum Kind { AddGroup, Set, Add };
  Kind kind;
  std::string name;
  std::string value;
};

/** The dialect and the options of the write. */
struct Config {
  size_t dialect = 0;
  bool crlf = false;
  bool emit_comments = true;
  bool normalize = false;
};

struct Result {
  /** Empty when the promise held. Otherwise the property that did not hold. */
  std::string failure;
  /** What the property says about the instance: which name or key, how many bytes. */
  std::string detail;
  /** The bytes the writer produced (empty when it refused). */
  std::string out;
  /** Whether the writer refused (GTEXT_INI_E_UNREPRESENTABLE) what the model accepted. */
  bool writer_refused = false;
  /** Whether the document model refused a step. */
  size_t steps_refused = 0;
  size_t steps_accepted = 0;
  /** Whether the bytes were read back and compared. */
  bool compared = false;
};

/**
 * Whether a value read back is the value that was set.
 *
 * A dialect whose lines continue by indentation (configparser) stores the document's own bytes: the writer puts every line of a value after its first
 * behind the indentation that makes it a continuation, and a reader keeps that indentation in the stored value ("the joined form is the canonical form",
 * documentation/formats/ini.md), so `\n)` is written as `k=` and ` )` and read back as `\n )`. What is the same is the joined form of the two,
 * which gtext_ini_unescape() makes. Every other dialect keeps the bytes.
 */
inline bool same_value(const GTEXT_INI_Dialect & dialect, const char * read, size_t read_len, const std::string & set) {
  if (dialect.continuation != GTEXT_INI_CONTINUATION_INDENT) {
    return read_len == set.size() && (read_len == 0 || std::memcmp(read, set.data(), read_len) == 0);
  }
  // The joined form of what was read is the value, and it is compared with what was set **as it is**: the joined form of the set value as well would make `" x"` and `"x"` one
  // value, because the joined form strips the blanks around each line, and a writer that dropped a leading blank would pass.
  char * joined = nullptr;
  size_t n = 0;
  if (gtext_ini_unescape(&dialect, read, read_len, nullptr, &joined, &n) != GTEXT_INI_OK) return false;
  const bool same = n == set.size() && (n == 0 || std::memcmp(joined, set.data(), n) == 0);
  gtext_ini_string_free(nullptr, joined);
  return same;
}

/** Whether two keys are one key to the dialect: a dialect that folds case (git, EditorConfig, configparser) takes `A` and `a` for one, and set() replaces it (ASCII only: so does the library). */
inline bool same_key(bool folds, const std::string & x, const std::string & y) {
  if (x.size() != y.size()) return false;
  for (size_t k = 0; k < x.size(); k++) {
    unsigned char p = (unsigned char)x[k], q = (unsigned char)y[k];
    if (folds) {
      if (p >= 'A' && p <= 'Z') p = (unsigned char)(p - 'A' + 'a');
      if (q >= 'A' && q <= 'Z') q = (unsigned char)(q - 'A' + 'a');
    }
    if (p != q) return false;
  }
  return true;
}

/** One entry the model asked for and the document accepted. */
struct Entry {
  std::string key;
  std::string value;
  /** The name a lookup answers to: the tree's canonical key. A key that holds a continuation (systemd: `Env\<LF>iron`) is written as it is and read back under its joined name (`Env iron`), which is the
      name a caller has; a dialect that folds answers to the folded one. The spelling is not what a lookup compares. */
  std::string canon;
};
struct Group {
  std::string name;
  /** The name a lookup answers to: for git the folded `section.subsection`, because `[a.V]` is the group `a.v` and is reachable by no other spelling (git does the same; documentation/formats/ini.md), and the header spelling for every other dialect. */
  std::string canon;
  std::vector<Entry> entries;
};

/** The name a lookup answers to once a dialect that trims has read it back: the blanks around it are gone (`[\t.]` and `[.]` are two headers of one name). */
inline std::string trimmed_name(bool trims, std::string name) {
  if (!trims) return name;
  const size_t b = name.find_first_not_of(" \t\v\f");
  if (b == std::string::npos) return std::string();
  return name.substr(b, name.find_last_not_of(" \t\v\f") - b + 1);
}

/**
 * The @p nth group of the document whose header names @p name.
 *
 * By name through gtext_ini_document_group() for every name but the empty one.
 * The empty name is the **preamble** for that lookup (`[]` then `k=v` with no
 * preamble answers nothing for `("", "k")`, measured against the reference
 * and pinned in tests/test-ini.cpp), so a `[]` group the DOM accepted and the
 * writer wrote is a real group that no lookup by name reaches. The model
 * asked for it and so cannot lose it, and it finds it the way a caller must:
 * by position, among the groups that are not the preamble.
 */
inline const GTEXT_INI_Group * find_group(const GTEXT_INI_Document * doc, const std::string & name, const std::string & canon, size_t & nth) {
  if (name.find_first_not_of(" \t\v\f") != std::string::npos) {
    return gtext_ini_document_group(doc, canon.c_str());
  }
  // The empty name, and a name of blanks only: a dialect that trims reads `[\t]` as `[]`, and the lookup trims the query the same way, so no lookup by name reaches the group; a dialect
  // that does not trim keeps `[\f\f]` as the name it is, but it is one of the blank groups all the same, and an exact lookup of it would not count the place it holds among them (the `[]`
  // written after it is then found as the first). So both are found by place, below.
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

/** The statuses the document model documents for a refusal of a step. */
inline bool documented_refusal(GTEXT_INI_Status st) {
  return st == GTEXT_INI_E_BAD_GROUP || st == GTEXT_INI_E_BAD_KEY || st == GTEXT_INI_E_DUPGROUP || st == GTEXT_INI_E_DUPKEY;
}

inline std::string show(const std::string & s);

inline std::string fmt(const char * f, size_t a, size_t b = 0) {
  char buf[160];
  std::snprintf(buf, sizeof buf, f, a, b);
  return buf;
}

/**
 * Build the document from the steps, write it, read the bytes back, and compare.
 *
 * What is asserted, and what is not: a step the document model refuses (with a status it documents) is not recorded and the round trip makes no claim about it; a write the writer
 * refuses with GTEXT_INI_E_UNREPRESENTABLE makes no claim either. Any other status is a failure, as is a write that reports OK and gives bytes that do not parse, or a group, key or
 * value that does not come back.
 */
inline Result check(const Config & cfg, const std::vector<Step> & steps) {
  Result r;
  const DialectEntry & de = dialects()[cfg.dialect % dialects().size()];
  GTEXT_INI_Parse_Options popts = gtext_ini_parse_options_default();
  popts.dialect = de.make();
  popts.max_total_bytes = 0;

  GTEXT_INI_Write_Options wopts = gtext_ini_write_options_default();
  wopts.crlf = cfg.crlf;
  wopts.emit_comments = cfg.emit_comments;
  wopts.normalize = cfg.normalize;

  GTEXT_INI_Document * doc = gtext_ini_new(&popts);
  if (!doc) {
    r.failure = "gtext_ini_new failed";
    return r;
  }

  // Build groups and entries from the steps, keeping only what the DOM accepts.
  // A refusal is the dialect's key or name rule doing its job, so the shadow
  // records nothing for it and the round trip below makes no claim about it.
  std::vector<Group> shadow;
  GTEXT_INI_Group * current = nullptr;

  for (const Step & step : steps) {
    // the DOM takes a C string, so a NUL ends the name or the key it is given (the shadow records what the DOM was handed, not the bytes that were drawn)
    const std::string a(step.name.c_str());
    if (step.kind == Step::AddGroup) {
      GTEXT_INI_Group * g = nullptr;
      const GTEXT_INI_Status st = gtext_ini_document_add_group(doc, a.c_str(), &g);
      if (st == GTEXT_INI_OK && g) {
        current = g;
        size_t clen = 0;
        const char * cn = gtext_ini_group_canonical_name(g, &clen);
        shadow.push_back(Group{a, cn ? std::string(cn, clen) : a, {}});
        r.steps_accepted++;
      }
      else if (st == GTEXT_INI_OK || !documented_refusal(st)) {
        r.failure = "add_group answered a status that is not a documented refusal or success";
        r.detail = fmt("status %zu, name of %zu bytes", (size_t)st, a.size());
        gtext_ini_free(doc);
        return r;
      }
      else {
        r.steps_refused++;
      }
      continue;
    }
    if (!current) continue;   // an entry before any group: nothing to put it in
    const GTEXT_INI_Status st = step.kind == Step::Set ? gtext_ini_group_set(current, a.c_str(), step.value.data(), step.value.size())
                                                       : gtext_ini_group_add(current, a.c_str(), step.value.data(), step.value.size());
    if (st != GTEXT_INI_OK) {
      if (!documented_refusal(st)) {
        r.failure = "set or add answered a status that is not a documented refusal or success";
        r.detail = fmt("status %zu, key of %zu bytes", (size_t)st, a.size());
        gtext_ini_free(doc);
        return r;
      }
      r.steps_refused++;
      continue;
    }
    r.steps_accepted++;
    size_t at = 0;
    bool replaced = false;
    if (step.kind == Step::Set) {
      // set() replaces, so the shadow must replace too rather than append.
      for (Entry & e : shadow.back().entries) {
        if (same_key(popts.dialect.fold_case, e.key, a)) {
          e.value = step.value;
          replaced = true;
          break;
        }
        at++;
      }
    }
    if (!replaced) {
      shadow.back().entries.push_back(Entry{a, step.value, a});
      at = shadow.back().entries.size() - 1;
    }
    // the shadow holds the entries in the order the group does, so the place in one is the place in the other
    size_t clen = 0;
    const char * ck = gtext_ini_group_canonical_key_at(current, at, &clen);
    if (ck) shadow.back().entries[at].canon.assign(ck, clen);
  }

  GTEXT_INI_Sink sink;
  if (gtext_ini_sink_buffer(&sink) != GTEXT_INI_OK) {
    gtext_ini_free(doc);
    r.failure = "the buffer sink could not be made";
    return r;
  }
  const GTEXT_INI_Status st = gtext_ini_write(doc, &sink, &wopts);
  const std::string out(gtext_ini_sink_buffer_data(&sink), gtext_ini_sink_buffer_size(&sink));
  gtext_ini_sink_buffer_free(&sink);

  if (st == GTEXT_INI_E_UNREPRESENTABLE) {
    r.writer_refused = true;
    gtext_ini_free(doc);
    return r;
  }
  if (st != GTEXT_INI_OK) {
    r.failure = "the writer answered a status that is neither success nor GTEXT_INI_E_UNREPRESENTABLE";
    r.detail = fmt("status %zu", (size_t)st);
    r.out = out;
    gtext_ini_free(doc);
    return r;
  }
  r.out = out;

  if (!shadow.empty()) {
    r.compared = true;
    // `crlf` is an option of the writer and not of the tree (tests/test-ini.cpp, IniWrite.CrlfIsAnOptionOfTheWriterNotOfTheTree), and a dialect that does not accept a CR
    // before an LF (Desktop Entry: "Desktop Entry §3 specifies linefeed") cannot read what it was asked to write; the reader is told to accept it, which changes nothing
    // else about the dialect.
    GTEXT_INI_Parse_Options back_opts = popts;
    if (cfg.crlf) back_opts.dialect.accept_crlf = true;
    GTEXT_INI_Document * back = gtext_ini_parse(out.data(), out.size(), &back_opts, nullptr);
    if (!back) {
      r.failure = "the writer reported OK and wrote bytes that do not parse back under the same dialect";
      gtext_ini_free(doc);
      return r;
    }

    const bool last = popts.dialect.dupkey == GTEXT_INI_DUPKEY_LAST_WINS || popts.dialect.dupkey == GTEXT_INI_DUPKEY_COLLECT;
    const bool trims = popts.dialect.trim_group_name;
    // Every group the DOM accepted must come back, with its entries. Looked up by name rather than by index, because a writer is allowed to order the preamble and the groups as it sees fit.
    size_t empty_seen = 0;
    for (size_t gi = 0; gi < shadow.size() && r.failure.empty(); gi++) {
      const Group & g = shadow[gi];
      // A dialect that keeps a repeated header as a group of its own (win32: "a key in a second `[a]` is retrievable by no name at all", pinned in tests/test-ini.cpp)
      // answers a lookup by name with the first, so the second is found as the group it is: the one with its spelling at its place among the groups of that spelling.
      // The header is the same by the name it is read back as, not its spelling: win32 trims the blanks around a name, so `[\t.]` and `[.]` are two headers of one name.
      const std::string read_as = trimmed_name(trims, g.canon);
      size_t same_before = 0;
      for (size_t k = 0; k < gi; k++) {
        if (trimmed_name(trims, shadow[k].canon) == read_as) same_before++;
      }
      const GTEXT_INI_Group * got = nullptr;
      const bool merged = popts.dialect.merge_duplicate_groups && !read_as.empty();
      if (merged) {
        // A dialect that merges repeated headers ("A duplicate group is accepted, and a lookup merges the two": ini.h; documentation/formats/ini.md) keeps the groups apart in the
        // tree and merges them in the lookup by name, gtext_ini_document_get(), which is where the promise is made: a key set in the second `[a]` is read through the document and
        // not through the first group object. What the lookup answers for a key that two of the groups hold is the dialect's repeated-key rule (the last occurrence for last-wins and
        // for collect, the first otherwise), so the model expects the value of that occurrence among the groups of the name, in the order it added them.
        got = find_group(back, g.name, g.canon, empty_seen);
      }
      else if (same_before > 0 && !read_as.empty()) {   // a blank name is found by its place among the blank ones
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
        r.failure = "a group the DOM accepted did not survive the round trip";
        r.detail = fmt("group of %zu bytes, number %zu", g.name.size(), gi);
        break;
      }
      for (const Entry & e : g.entries) {
        // the value of the occurrence the lookup answers, among the groups it looks in (every group of the name when they merge, this one otherwise)
        const std::string * want = nullptr;
        for (size_t k = 0; k < shadow.size(); k++) {
          if (merged ? trimmed_name(trims, shadow[k].canon) != read_as : k != gi) continue;
          for (const Entry & other : shadow[k].entries) {
            if (other.canon == e.canon && (last || !want)) want = &other.value;
          }
        }
        size_t len = 0;
        const char * v = merged ? gtext_ini_document_get(back, g.canon.c_str(), e.canon.c_str(), &len) : gtext_ini_group_get(got, e.canon.c_str(), &len);
        if (!v) {
          r.failure = "a key the DOM accepted did not survive the round trip";
          r.detail = fmt("key of %zu bytes in a group of %zu bytes", e.key.size(), g.name.size());
          break;
        }
        if (!want || !same_value(popts.dialect, v, len, *want)) {
          r.failure = "a value changed across the round trip";
          r.detail = fmt("wrote %zu bytes, read %zu", want ? want->size() : 0, len);
          break;
        }
      }
    }
    gtext_ini_free(back);
  }
  gtext_ini_free(doc);
  return r;
}

/**
 * The other half of the promise: the writer refuses what would not read back, and **only** that.
 *
 * "The writer must refuse exactly the values that would read back differently, and no others, or the caller cannot rewrite a file it just read" (src/ini/ini_writer.c).
 * For a document of one group and one entry that the writer refused, this writes the lines the writer would have written by hand (`[group]`, the entry with the dialect's
 * first separator, the terminator of the write) and reads them. It answers true when the bytes parse and give the group, the key and the value that were asked for: the
 * writer refused something the reader reads as given, and a caller who holds such a document cannot write it.
 *
 * The answer is a fact about the library and not a verdict. A refusal may be conservative on purpose (the writer cannot see what will be written after a value, for one), and
 * the sweep names the ones that are, so that a new one fails the sweep until it is looked at.
 */
inline bool reads_as_asked(const Config & cfg, const std::string & group, const std::string & key, const std::string & value) {
  const DialectEntry & de = dialects()[cfg.dialect % dialects().size()];
  GTEXT_INI_Parse_Options popts = gtext_ini_parse_options_default();
  popts.dialect = de.make();
  popts.max_total_bytes = 0;
  if (cfg.crlf) popts.dialect.accept_crlf = true;
  const std::string eol = cfg.crlf ? "\r\n" : "\n";
  const std::string text = "[" + group + "]" + eol + key + std::string(1, de.make().separators[0]) + value + eol;
  GTEXT_INI_Document * back = gtext_ini_parse(text.data(), text.size(), &popts, nullptr);
  if (!back) return false;
  bool same = false;
  if (gtext_ini_document_group_count(back) == 1) {
    const GTEXT_INI_Group * g = gtext_ini_document_group_at(back, 0);
    size_t gl = 0, kl = 0, vl = 0;
    const char * gn = gtext_ini_group_name(g, &gl);
    const char * kn = gtext_ini_group_key_at(g, 0, &kl);
    const char * vn = gtext_ini_group_value_at(g, 0, &vl);
    same = gtext_ini_group_entry_count(g) == 1 && gn && kn && vn && std::string(gn, gl) == group && std::string(kn, kl) == key && same_value(popts.dialect, vn, vl, value);
  }
  gtext_ini_free(back);
  return same;
}

/** What a document holds, in the terms a reader can ask: the groups in order, each with its name, whether it is the preamble, and its entries (key, whether it has a value, the value). */
struct Snapshot {
  struct E {
    std::string key;
    bool has_value;
    std::string value;
  };
  struct G {
    std::string name;
    bool preamble;
    std::vector<E> entries;
  };
  std::vector<G> groups;
};

inline Snapshot snapshot(const GTEXT_INI_Document * doc) {
  Snapshot s;
  for (size_t g = 0; g < gtext_ini_document_group_count(doc); g++) {
    const GTEXT_INI_Group * grp = gtext_ini_document_group_at(doc, g);
    size_t nl = 0;
    const char * nm = gtext_ini_group_name(grp, &nl);
    Snapshot::G out{std::string(nm ? nm : "", nl), gtext_ini_group_is_preamble(grp), {}};
    for (size_t e = 0; e < gtext_ini_group_entry_count(grp); e++) {
      size_t kl = 0, vl = 0;
      const char * k = gtext_ini_group_key_at(grp, e, &kl);
      const char * v = gtext_ini_group_value_at(grp, e, &vl);
      out.entries.push_back({std::string(k ? k : "", kl), gtext_ini_group_value_present_at(grp, e), std::string(v ? v : "", vl)});
    }
    s.groups.push_back(out);
  }
  return s;
}

/** The first difference between two snapshots, or empty. A value is compared as the dialect compares one that was read back (same_value): the bytes, or for an indent-continued dialect the joined form. */
inline std::string difference(const GTEXT_INI_Dialect & dialect, const Snapshot & a, const Snapshot & b) {
  if (a.groups.size() != b.groups.size()) return fmt("%zu groups were written and %zu read", a.groups.size(), b.groups.size());
  for (size_t g = 0; g < a.groups.size(); g++) {
    const Snapshot::G & x = a.groups[g];
    const Snapshot::G & y = b.groups[g];
    if (x.name != y.name || x.preamble != y.preamble) return fmt("group %zu was renamed (%zu bytes)", g, x.name.size()) + " to [" + show(y.name) + "]";
    if (x.entries.size() != y.entries.size()) return fmt("group %zu has %zu entries", g, x.entries.size()) + " and then " + std::to_string(y.entries.size());
    for (size_t e = 0; e < x.entries.size(); e++) {
      if (x.entries[e].key != y.entries[e].key || x.entries[e].has_value != y.entries[e].has_value) return fmt("entry %zu of group %zu changed its key or its having a value", e, g);
      if (x.entries[e].has_value) {
        // the first value is the tree's raw one, which the joined form of an indent dialect compares as it does the second
        char * j = nullptr;
        size_t n = 0;
        std::string want = x.entries[e].value;
        if (dialect.continuation == GTEXT_INI_CONTINUATION_INDENT && gtext_ini_unescape(&dialect, want.data(), want.size(), nullptr, &j, &n) == GTEXT_INI_OK) {
          want.assign(j, n);
          gtext_ini_string_free(nullptr, j);
        }
        if (!same_value(dialect, y.entries[e].value.data(), y.entries[e].value.size(), want)) return fmt("entry %zu of group %zu changed its value", e, g);
      }
    }
  }
  return std::string();
}

/**
 * The same promise for a document that was **read**, and the writer's other one: a document this module read can be written back.
 *
 * @p text is parsed under the dialect; a text the dialect refuses is not a case (`parsed` stays false). With the default write (LF, comments, no normalizing) the bytes written
 * are the bytes read, and the writer does not refuse: "Desktop Entry §3 requires that a rewrite preserve fields the implementation does not understand", and a document that
 * cannot be written back after it has been read is the failure that made the writer's first version refuse every CR. With any other option the writer may refuse
 * (GTEXT_INI_E_UNREPRESENTABLE) or write, and what it writes parses to the same groups and entries.
 */
inline Result check_document(const Config & cfg, const std::string & text, bool * parsed) {
  Result r;
  *parsed = false;
  const DialectEntry & de = dialects()[cfg.dialect % dialects().size()];
  GTEXT_INI_Parse_Options popts = gtext_ini_parse_options_default();
  popts.dialect = de.make();
  popts.max_total_bytes = 0;
  GTEXT_INI_Document * doc = gtext_ini_parse(text.data(), text.size(), &popts, nullptr);
  if (!doc) return r;
  *parsed = true;
  const Snapshot first = snapshot(doc);

  GTEXT_INI_Write_Options wopts = gtext_ini_write_options_default();
  wopts.crlf = cfg.crlf;
  wopts.emit_comments = cfg.emit_comments;
  wopts.normalize = cfg.normalize;
  GTEXT_INI_Sink sink;
  if (gtext_ini_sink_buffer(&sink) != GTEXT_INI_OK) {
    gtext_ini_free(doc);
    r.failure = "the buffer sink could not be made";
    return r;
  }
  const GTEXT_INI_Status st = gtext_ini_write(doc, &sink, &wopts);
  r.out.assign(gtext_ini_sink_buffer_data(&sink), gtext_ini_sink_buffer_size(&sink));
  gtext_ini_sink_buffer_free(&sink);
  gtext_ini_free(doc);

  const bool plain = !cfg.crlf && !cfg.normalize && cfg.emit_comments;
  if (st == GTEXT_INI_E_UNREPRESENTABLE) {
    r.writer_refused = true;
    if (plain) r.failure = "a document that was read could not be written back unchanged";
    return r;
  }
  if (st != GTEXT_INI_OK) {
    r.failure = "the writer answered a status that is neither success nor GTEXT_INI_E_UNREPRESENTABLE";
    return r;
  }
  if (plain && r.out != text) {
    r.failure = "a document that was read was written back as other bytes";
    r.detail = "wrote " + std::to_string(r.out.size()) + " bytes for " + std::to_string(text.size());
    return r;
  }
  r.compared = true;
  GTEXT_INI_Parse_Options back_opts = popts;
  if (cfg.crlf) back_opts.dialect.accept_crlf = true;
  GTEXT_INI_Document * back = gtext_ini_parse(r.out.data(), r.out.size(), &back_opts, nullptr);
  if (!back) {
    r.failure = "the writer reported OK and wrote bytes that do not parse back under the same dialect";
    return r;
  }
  const std::string d = difference(popts.dialect, first, snapshot(back));
  gtext_ini_free(back);
  if (!d.empty()) {
    r.failure = "a document that was read and written is not the document that was read";
    r.detail = d;
  }
  return r;
}

/** Whether @p s is well-formed UTF-8 (the shortest form, no surrogates, nothing above U+10FFFF). */
inline bool valid_utf8(const std::string & s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = (unsigned char)s[i];
    size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
    if (!n || i + n > s.size()) return false;
    for (size_t k = 1; k < n; k++) {
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
    }
    if (n == 3 && ((c == 0xE0 && (unsigned char)s[i + 1] < 0xA0) || (c == 0xED && (unsigned char)s[i + 1] >= 0xA0))) return false;
    if (n == 4 && ((c == 0xF0 && (unsigned char)s[i + 1] < 0x90) || (c == 0xF4 && (unsigned char)s[i + 1] >= 0x90))) return false;
    i += n;
  }
  return true;
}

/**
 * The value codec is an entry point of the writer too: gtext_ini_escape() is "the function a caller needs before gtext_ini_group_set()" (ini_value.h), so a text it encodes
 * has to be a value the writer writes, and what the reader gives back has to decode to the text. For @p text, as the value of one key in one group:
 *
 *   - escape() refuses it (GTEXT_INI_E_UNREPRESENTABLE: the dialect has no way to spell a byte of it), and nothing more is asked; or
 *   - unescape(escape(text)) is the text, the document model takes the encoded value, the writer writes it (it does not refuse what the codec made for it), and the value read
 *     back from the bytes decodes to the text.
 */
inline Result check_escaped(const Config & cfg, const std::string & text) {
  Result r;
  const DialectEntry & de = dialects()[cfg.dialect % dialects().size()];
  GTEXT_INI_Parse_Options popts = gtext_ini_parse_options_default();
  popts.dialect = de.make();
  popts.max_total_bytes = 0;
  char * enc = nullptr;
  size_t enc_len = 0;
  const GTEXT_INI_Status es = gtext_ini_escape(&popts.dialect, text.data(), text.size(), nullptr, &enc, &enc_len);
  if (es == GTEXT_INI_E_UNREPRESENTABLE) {
    // A refusal is the answer for a text the dialect cannot spell; it is not one for a text the dialect carries as it is, which the reader reads back as the text and so the writer
    // would have written. The text is that when it is the value of a one-entry document that reads as asked, and decoding it gives itself.
    char * dec = nullptr;
    size_t dn = 0;
    if (reads_as_asked(Config{cfg.dialect, false, true, false}, "g", "k", text) &&
        gtext_ini_unescape(&popts.dialect, text.data(), text.size(), nullptr, &dec, &dn) == GTEXT_INI_OK) {
      const bool itself = dn == text.size() && (dn == 0 || std::memcmp(dec, text.data(), dn) == 0);
      gtext_ini_string_free(nullptr, dec);
      if (itself) r.failure = "escape refused a text that the dialect reads back as it is";
    }
    // And a text that the dialect has a spelling for: where the escape set has a letter for every byte that needs one (a line feed, a carriage return, a tab, a backslash, and a blank, the way
    // Desktop Entry and systemd have), any text without a NUL has an encoding, as long as the dialect decodes UTF-8 only when the text is; where the dialect quotes (git), any text without a
    // NUL or a carriage return does (git has no escape for a CR, and a CR before an LF is part of the terminator).
    if (r.failure.empty()) {
      const char * esc = popts.dialect.escapes ? popts.dialect.escapes : "";
      bool full_set = true;
      for (const char * need = "nrt\\s"; *need; need++) {
        if (!std::strchr(esc, *need)) full_set = false;
      }
      const bool has_nul = text.find('\0') != std::string::npos;
      const bool has_cr = text.find('\r') != std::string::npos;
      if (full_set && !has_nul && (!popts.dialect.utf8_values || valid_utf8(text))) {
        r.failure = "escape refused a text that the dialect's escape set can spell";
      }
      else if (popts.dialect.quoted_values && !has_nul && !has_cr) {
        r.failure = "escape refused a text that the dialect's quotes can carry";
      }
    }
    return r;
  }
  if (es != GTEXT_INI_OK) {
    r.failure = "escape answered a status that is neither success nor GTEXT_INI_E_UNREPRESENTABLE";
    r.detail = fmt("status %zu", (size_t)es);
    return r;
  }
  const std::string encoded(enc, enc_len);
  gtext_ini_string_free(nullptr, enc);
  r.compared = true;
  auto decode = [&](const GTEXT_INI_Dialect & d, const std::string & raw, std::string & out) {
    char * dec = nullptr;
    size_t n = 0;
    const GTEXT_INI_Status st = gtext_ini_unescape(&d, raw.data(), raw.size(), nullptr, &dec, &n);
    if (st == GTEXT_INI_OK) {
      out.assign(dec, n);
      gtext_ini_string_free(nullptr, dec);
    }
    return st;
  };
  std::string back;
  if (decode(popts.dialect, encoded, back) != GTEXT_INI_OK || back != text) {
    r.failure = "unescape does not undo escape";
    r.detail = fmt("%zu bytes encoded as %zu", text.size(), encoded.size());
    return r;
  }
  GTEXT_INI_Document * doc = gtext_ini_new(&popts);
  GTEXT_INI_Group * g = nullptr;
  if (!doc || gtext_ini_document_add_group(doc, "g", &g) != GTEXT_INI_OK || gtext_ini_group_set(g, "k", encoded.data(), encoded.size()) != GTEXT_INI_OK) {
    r.failure = "the document model refused a group g and a key k";
    if (doc) gtext_ini_free(doc);
    return r;
  }
  GTEXT_INI_Write_Options wopts = gtext_ini_write_options_default();
  wopts.crlf = cfg.crlf;
  wopts.emit_comments = cfg.emit_comments;
  wopts.normalize = cfg.normalize;
  GTEXT_INI_Sink sink;
  if (gtext_ini_sink_buffer(&sink) != GTEXT_INI_OK) {
    gtext_ini_free(doc);
    r.failure = "the buffer sink could not be made";
    return r;
  }
  const GTEXT_INI_Status st = gtext_ini_write(doc, &sink, &wopts);
  r.out.assign(gtext_ini_sink_buffer_data(&sink), gtext_ini_sink_buffer_size(&sink));
  gtext_ini_sink_buffer_free(&sink);
  gtext_ini_free(doc);
  if (st != GTEXT_INI_OK) {
    r.writer_refused = st == GTEXT_INI_E_UNREPRESENTABLE;
    r.failure = "the writer refused a value that escape made for it";
    r.detail = fmt("status %zu, encoded value of %zu bytes", (size_t)st, encoded.size());
    return r;
  }
  GTEXT_INI_Parse_Options back_opts = popts;
  if (cfg.crlf) back_opts.dialect.accept_crlf = true;
  GTEXT_INI_Document * read = gtext_ini_parse(r.out.data(), r.out.size(), &back_opts, nullptr);
  if (!read) {
    r.failure = "the writer reported OK and wrote bytes that do not parse back under the same dialect";
    return r;
  }
  size_t vl = 0;
  const char * v = gtext_ini_document_get(read, "g", "k", &vl);
  std::string decoded;
  if (!v) {
    r.failure = "a key the DOM accepted did not survive the round trip";
  }
  else if (decode(back_opts.dialect, std::string(v, vl), decoded) != GTEXT_INI_OK || decoded != text) {
    r.failure = "the value read back does not decode to the text that was encoded";
    r.detail = fmt("%zu bytes set, %zu read", encoded.size(), vl);
  }
  gtext_ini_free(read);
  return r;
}

/** The bytes of @p s for a report: printable ASCII as it is, anything else as \xNN. */
inline std::string show(const std::string & s) {
  std::string t;
  for (unsigned char c : s) {
    if (c >= 0x20 && c < 0x7f && c != '\\') {
      t += (char)c;
    }
    else {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\x%02x", c);
      t += buf;
    }
  }
  return t;
}

}   // namespace ini_model

#endif
