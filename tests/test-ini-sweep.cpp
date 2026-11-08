/**
 * @file
 *
 * The INI writer's promise, asked of every dialect, every option of the write and every entry point of the document model, over a table of boundary strings.
 *
 * The promise is tests/ini_writer_model.h: what the document model accepts, the writer refuses (GTEXT_INI_E_UNREPRESENTABLE) or writes so that it reads back as
 * what was given. Whether a name, a key or a value is writable was decided by hand for each dialect, and each round of the libFuzzer harness found the next
 * dialect's gap (a carriage return in a win32 group name, a line end in a systemd key, a lone carriage return as a Desktop Entry value written with CRLF). The
 * harness draws its input at random; this test does not leave the choice to chance. The strings are the ones a rule of any dialect is about: the line ends and
 * their pairs, blanks of every kind at either end and inside, the quote and escape characters, the comment, separator, continuation and header characters of
 * the dialect being swept, an empty string, a lone separator, a byte order mark, bytes that are not UTF-8, and a long one. Every one of them, alone and joined
 * to every other, is a group name, a key and a value, through add_group(), set() and add(), under every dialect, with and without CRLF, normalization and
 * comments; the entries come before and after others, because a rule can depend on the byte that follows a value. The comparison is the model's, shared with the
 * fuzzer, so the two cannot disagree about what the writer owes.
 *
 * A failure is printed as the dialect, the options and the steps that make it, so it can be pasted into a test. The table is a population and not a proof: the
 * assertions below it say how many of its cases the model accepted, wrote and compared in each dialect, so that a sweep that has gone blind (every case refused, a
 * dialect the table does not reach) fails and does not pass.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ini_writer_model.h"

namespace {

using ini_model::Config;
using ini_model::Result;
using ini_model::Step;

std::string repeat(const std::string & s, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; i++) out += s;
  return out;
}

/** The boundary strings for one dialect: the same table for each, plus the characters the dialect's own grammar is made of. */
std::vector<std::string> alphabet(const GTEXT_INI_Dialect & d) {
  using namespace std::string_literals;
  std::vector<std::string> a = {
      ""s,       "a"s,     "A"s,       "k"s,        "ab"s,       "a b"s,     " "s,         "\t"s,       "\v"s,       "\f"s,    "\r"s,
      "\n"s,     "\r\n"s,  "\n\r"s,    "\\"s,       "\\\n"s,     "\\\r\n"s,  "\\\r"s,      "\""s,       "'"s,        "="s,     ":"s,
      ";"s,      "#"s,     "["s,       "]"s,        "[]"s,       "[a]"s,     "."s,         "a.b"s,      "\\\""s,     "a\\"s,   "\\n"s,
      "\\\\"s,   "\xEF\xBB\xBF"s, "\xC3\xA9"s, "\xFF"s, "\xC0\x80"s, "\x01"s, "\x7f"s,     "%"s,        "@"s,        "a@b"s,   "k[de]"s,
      "k[]"s,    "k["s,    "a \"b\""s, "\"a"s,      "\"a\""s,    " \"a\""s,  "a ;c"s,      "a #c"s,     "a;"s,       "/"s,     "{"s,
      " a"s,     "a "s,    "\ta"s,     "a\t"s,      "a\r"s,      "\ra"s,     "a\n"s,       "\na"s,      "a=b"s,      "=a"s,    "a="s,
      "a:b"s,    "\v a"s,  "a \f"s,    "\xEF\xBB\xBF" "a"s, " \xEF\xBB\xBF"s, "a\xEF\xBB\xBF"s, "a\r\nb"s, "a\\\nb"s, "a\\\r\nb"s, "a\\\rb"s, "a\0b"s,
  };
  // the characters this dialect's grammar is made of, alone and beside a letter
  std::string special = d.separators ? d.separators : "";
  if (d.escapes) special += d.escapes;
  special += "\\";
  if (d.comment_hash) special += '#';
  if (d.comment_semicolon) special += ';';
  for (char c : special) {
    const std::string s(1, c);
    a.push_back(s);
    a.push_back("a" + s);
    a.push_back(s + "a");
    a.push_back(" " + s);
    a.push_back(s + " ");
  }
  a.push_back(std::string("\0", 1));
  a.push_back(repeat("x", 300));
  a.push_back(repeat(" ", 300));
  a.push_back("a" + repeat(" ", 300));
  a.push_back(repeat("\\\n", 50));
  std::set<std::string> seen;
  std::vector<std::string> out;
  for (auto & s : a) {
    if (seen.insert(s).second) out.push_back(s);
  }
  return out;
}

/** What follows a value on the next line, or a group after it: the byte that comes after a value is part of the rule for a carriage return at its end. */
const std::vector<std::string> kFollowers = {"", "x", "\r", "[g]", " ", "\\"};

struct Tally {
  size_t cases = 0, refused_by_dom = 0, writer_refused = 0, compared = 0;
};

struct Sweep {
  size_t dialect;
  std::map<std::string, Tally> tally;          // by entry point
  /** The failures by the entry point and the property that failed: how many, and the shortest few cases. */
  struct Bucket {
    size_t count = 0;
    std::vector<std::string> shortest;
  };
  std::map<std::string, Bucket> buckets;
  /** The refusals of the writer that the reader would not have needed, by the entry point. */
  std::map<std::string, Bucket> over_refusals;
  size_t failure_count = 0;
  size_t cases = 0;

  static std::string render(const Config & cfg, const std::vector<Step> & steps) {
    std::string s = std::string(ini_model::dialects()[cfg.dialect].name) + (cfg.crlf ? " crlf" : "") + (cfg.normalize ? " normalize" : "") +
                    (cfg.emit_comments ? "" : " no-comments") + ":";
    for (const Step & st : steps) {
      s += st.kind == Step::AddGroup ? " add_group(\"" : st.kind == Step::Set ? " set(\"" : " add(\"";
      s += ini_model::show(st.name);
      if (st.kind != Step::AddGroup) s += "\", \"" + ini_model::show(st.value);
      s += "\")";
    }
    return s;
  }

  static std::string render_document(const Config & cfg, const std::string & text) {
    return std::string(ini_model::dialects()[cfg.dialect].name) + (cfg.crlf ? " crlf" : "") + (cfg.normalize ? " normalize" : "") + (cfg.emit_comments ? "" : " no-comments") +
           ": text \"" + ini_model::show(text) + "\"";
  }

  void run(const char * entry, const Config & cfg, const std::vector<Step> & steps) {
    const Result r = ini_model::check(cfg, steps);
    Tally & t = tally[entry];
    t.cases++;
    cases++;
    if (r.steps_refused) t.refused_by_dom++;
    if (r.writer_refused) t.writer_refused++;
    if (r.compared) t.compared++;
    // a refusal of one group with one entry that the reader would have read as asked
    if (r.writer_refused && steps.size() == 2 && steps[0].kind == Step::AddGroup && steps[1].kind != Step::AddGroup && r.steps_refused == 0 &&
        ini_model::reads_as_asked(cfg, steps[0].name.c_str(), steps[1].name.c_str(), steps[1].value)) {
      Bucket & b = over_refusals[std::string(entry)];
      b.count++;
      b.shortest.push_back(render(cfg, steps));
      std::sort(b.shortest.begin(), b.shortest.end(), [](const std::string & x, const std::string & y) { return x.size() < y.size(); });
      if (b.shortest.size() > 6) b.shortest.resize(6);
    }
    if (!r.failure.empty()) {
      failure_count++;
      Bucket & b = buckets[std::string(entry) + ": " + r.failure];
      b.count++;
      b.shortest.push_back(render(cfg, steps) + "\n    " + r.detail + "; wrote: " + ini_model::show(r.out));
      std::sort(b.shortest.begin(), b.shortest.end(), [](const std::string & x, const std::string & y) { return x.size() < y.size(); });
      if (b.shortest.size() > 4) b.shortest.resize(4);
    }
  }
};

/** Every option of the write: the line ending, normalization, and comments. */
std::vector<Config> configs(size_t dialect) {
  std::vector<Config> out;
  for (int bits = 0; bits < 8; bits++) {
    Config c;
    c.dialect = dialect;
    c.crlf = (bits & 1) != 0;
    c.normalize = (bits & 2) != 0;
    c.emit_comments = (bits & 4) == 0;
    out.push_back(c);
  }
  return out;
}

bool full() {
  const char * e = std::getenv("GTEXT_TEST_FULL");
  return e && *e && *e != '0';
}

void sweep_dialect(size_t dialect, Sweep & sw) {
  const GTEXT_INI_Dialect d = ini_model::dialects()[dialect].make();
  const std::vector<std::string> alpha = alphabet(d);
  // Joined strings: every one of the table beside every other, for a name, a key and a value, so a rule that depends on what is around a byte meets it
  // (`\` then LF then `]`, a blank then a byte order mark, a quote and a comment character). The default sweep joins the strings of the short half of the table; GTEXT_TEST_FULL joins all.
  std::vector<std::string> shorts;
  for (const std::string & s : alpha) {
    if (s.size() <= 8) shorts.push_back(s);
  }
  std::vector<std::string> pairs;
  const std::vector<std::string> & pool = full() ? alpha : shorts;
  for (const std::string & x : pool) {
    for (const std::string & y : pool) pairs.push_back(x + y);
  }
  std::sort(pairs.begin(), pairs.end());
  pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());

  std::vector<std::string> names = alpha;
  names.insert(names.end(), pairs.begin(), pairs.end());
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());

  for (const Config & cfg : configs(dialect)) {
    // a group, alone, and a group and one beside it (the header of one is followed by the header of the next)
    for (const std::string & n : names) {
      sw.run("add_group", cfg, {{Step::AddGroup, n, ""}});
    }
    for (const std::string & n : alpha) {
      for (const std::string & f : kFollowers) {
        sw.run("add_group+add_group", cfg, {{Step::AddGroup, n, ""}, {Step::AddGroup, f, ""}});
        sw.run("add_group+set+add_group", cfg, {{Step::AddGroup, n, ""}, {Step::Set, "k", "v"}, {Step::AddGroup, f, ""}, {Step::Set, "l", "w"}});
      }
    }
    // a key and a value, each of the table, through set() and add()
    for (const std::string & k : alpha) {
      for (const std::string & v : alpha) {
        sw.run("set", cfg, {{Step::AddGroup, "g", ""}, {Step::Set, k, v}});
        sw.run("add", cfg, {{Step::AddGroup, "g", ""}, {Step::Add, k, v}});
      }
    }
    // a key that is a name of the table joined to another, with an ordinary value; and the same for a value
    for (const std::string & k : pairs) {
      sw.run("set(joined key)", cfg, {{Step::AddGroup, "g", ""}, {Step::Set, k, "v"}});
      sw.run("add(joined key)", cfg, {{Step::AddGroup, "g", ""}, {Step::Add, k, "v"}});
    }
    for (const std::string & v : pairs) {
      sw.run("set(joined value)", cfg, {{Step::AddGroup, "g", ""}, {Step::Set, "k", v}});
      sw.run("add(joined value)", cfg, {{Step::AddGroup, "g", ""}, {Step::Add, "k", v}});
    }
    // a value, then what comes after it: another entry, another group, and the same key again
    for (const std::string & v : alpha) {
      for (const std::string & f : kFollowers) {
        sw.run("set+set", cfg, {{Step::AddGroup, "g", ""}, {Step::Set, "k", v}, {Step::Set, "l", f}});
        sw.run("set+add_group", cfg, {{Step::AddGroup, "g", ""}, {Step::Set, "k", v}, {Step::AddGroup, f, ""}, {Step::Set, "k", "w"}});
        sw.run("set+add(repeat)", cfg, {{Step::AddGroup, "g", ""}, {Step::Set, "k", v}, {Step::Add, "k", f}});
        sw.run("add+set", cfg, {{Step::AddGroup, "g", ""}, {Step::Add, "k", v}, {Step::Set, "l", f}});
      }
    }
    // a key, then what comes after it in a group of its own name (the same name twice, and a name that differs by case, or by its blanks)
    for (const std::string & n : alpha) {
      sw.run("add_group(repeat)", cfg, {{Step::AddGroup, n, ""}, {Step::Set, "k", "v"}, {Step::AddGroup, n, ""}, {Step::Set, "l", "w"}});
      sw.run("add_group(blanks)", cfg, {{Step::AddGroup, n, ""}, {Step::Set, "k", "v"}, {Step::AddGroup, " " + n + "\t", ""}, {Step::Set, "k", "w"}});
    }
  }
}


/** The lines of a document that mean something to some dialect: comments, headers, entries with every kind of value, a continuation, a byte order mark, an unfinished quote. */
const std::vector<std::string> kLines = {
    "",           " ",          "; c",        "# c",        "[g]",        "[ g ]",      "[g] x",      "[g",         "[]",         "k=v",        "k = v",      " k=v",
    "k",          "k=",         "=v",         "k:v",        "k=v ;c",     "k=v #c",     "k=\"v\"",    "k=\"v",      "k=v\\",      "k=a\\",      " cont",      "\tcont",
    "k[de]=v",    "k=v ",       "k=\xEF\xBB\xBF" "v", "\xEF\xBB\xBF" "k=v", "[a \"b\"]", "[a.b]",     "k\\",        "k=\\",       "\\",         "k=v\r",      "k=\r",       "k==v",
    " \xEF\xBB\xBF" "k=v", "[\xEF\xBB\xBF" "g]", "[g]]",       "k=\"a\\\"b\"", "k=a\\nb",    "k=%x",       "\tk=v",      "\vk=v",      "k=\fv",      "[g]\\",      "[g\\",       "k=a b c",
};
/** The lines of the three-line texts: a subset, because the number of texts is its cube. */
const std::vector<std::string> kShortLines = {"", "; c", "[g]", "k=v", "k", "k=", " cont", "k=v\\", "k=\"v", "[g", "k=v ;c", "k[de]=v", "k=\r", "k=\xEF\xBB\xBF" "v"};
const std::vector<std::string> kEols = {"\n", "\r\n", "\r"};

void sweep_documents(size_t dialect, Sweep & sw) {
  std::vector<std::string> bodies;
  auto join = [&](const std::vector<const std::string *> & lines, const std::string & eol, bool final_eol) {
    std::string t;
    for (size_t i = 0; i < lines.size(); i++) {
      t += *lines[i];
      if (i + 1 < lines.size() || final_eol) t += eol;
    }
    return t;
  };
  std::set<std::string> texts;
  for (const std::string & eol : kEols) {
    for (bool final_eol : {false, true}) {
      for (const std::string & a : kLines) {
        texts.insert(join({&a}, eol, final_eol));
        texts.insert("\xEF\xBB\xBF" + join({&a}, eol, final_eol));
        for (const std::string & b : kLines) {
          texts.insert(join({&a, &b}, eol, final_eol));
        }
      }
      for (const std::string & a : kShortLines) {
        for (const std::string & b : kShortLines) {
          for (const std::string & c : kShortLines) texts.insert(join({&a, &b, &c}, eol, final_eol));
        }
      }
    }
  }
  for (const Config & cfg : configs(dialect)) {
    for (const std::string & t : texts) {
      bool parsed = false;
      const Result r = ini_model::check_document(cfg, t, &parsed);
      Tally & tl = sw.tally["document"];
      tl.cases++;
      sw.cases++;
      if (!parsed) {
        tl.refused_by_dom++;
        continue;
      }
      if (r.writer_refused) tl.writer_refused++;
      if (r.compared) tl.compared++;
      if (!r.failure.empty()) {
        sw.failure_count++;
        Sweep::Bucket & b = sw.buckets[std::string("document: ") + r.failure];
        b.count++;
        b.shortest.push_back(Sweep::render_document(cfg, t) + "\n    " + r.detail + "; wrote: " + ini_model::show(r.out));
        std::sort(b.shortest.begin(), b.shortest.end(), [](const std::string & x, const std::string & y) { return x.size() < y.size(); });
        if (b.shortest.size() > 4) b.shortest.resize(4);
      }
    }
  }
}

class IniSweep : public ::testing::TestWithParam<size_t> {};

TEST_P(IniSweep, WhatTheDocumentModelAcceptsIsRefusedByTheWriterOrReadsBackAsItWas) {
  Sweep sw;
  sw.dialect = GetParam();
  sweep_dialect(GetParam(), sw);
  std::string report;
  for (const auto & [what, b] : sw.buckets) {
    report += what + " (" + std::to_string(b.count) + " cases); the shortest:\n";
    for (const std::string & f : b.shortest) report += "  " + f + "\n";
  }
  EXPECT_EQ(sw.failure_count, 0u) << sw.failure_count << " of " << sw.cases << " cases failed:\n" << report;

  if (std::getenv("GTEXT_INI_SWEEP_TALLY")) {
    for (const auto & [what, b] : sw.over_refusals) {
      std::printf("OVER-REFUSAL %s %s: %zu\n", ini_model::dialects()[GetParam()].name, what.c_str(), b.count);
      for (const std::string & f : b.shortest) std::printf("    %s\n", f.c_str());
    }
  }
  // The sweep must be able to see. A table that a dialect refuses wholesale (every name unwritable, every key refused) passes the property and proves nothing, so each
  // entry point is required to have been written and compared in a share of its cases (git names its groups from a narrow set of bytes, which is why the share is low), and the whole sweep to have been large.
  EXPECT_GT(sw.cases, 20000u) << "the sweep is smaller than the table makes it";
  for (const auto & [entry, t] : sw.tally) {
    if (std::getenv("GTEXT_INI_SWEEP_TALLY")) {
      std::printf("%-14s %-24s cases %7zu  model-refused %7zu  writer-refused %7zu  compared %7zu\n", ini_model::dialects()[GetParam()].name, entry.c_str(), t.cases, t.refused_by_dom,
          t.writer_refused, t.compared);
    }
    EXPECT_GE(t.compared, 100u) << entry << ": fewer than a hundred cases were written and read back";
    EXPECT_GT(t.compared * 50, t.cases) << entry << ": fewer than a fiftieth of " << t.cases << " cases were written and read back (" << t.refused_by_dom << " refused by the model, "
                                        << t.writer_refused << " by the writer)";
  }
}

TEST_P(IniSweep, ADocumentThatWasReadIsWrittenBackAsItWasAndNeverRefusedAndWhateverIsWrittenParsesToTheSameDocument) {
  Sweep sw;
  sw.dialect = GetParam();
  sweep_documents(GetParam(), sw);
  std::string report;
  for (const auto & [what, b] : sw.buckets) {
    report += what + " (" + std::to_string(b.count) + " cases); the shortest:\n";
    for (const std::string & f : b.shortest) report += "  " + f + "\n";
  }
  EXPECT_EQ(sw.failure_count, 0u) << sw.failure_count << " of " << sw.cases << " cases failed:\n" << report;
  const Tally & t = sw.tally["document"];
  if (std::getenv("GTEXT_INI_SWEEP_TALLY")) {
    std::printf("%-14s documents                cases %7zu  not-parsed %7zu  writer-refused %7zu  compared %7zu\n", ini_model::dialects()[GetParam()].name, t.cases, t.refused_by_dom,
        t.writer_refused, t.compared);
  }
  EXPECT_GT(t.compared, 1000u) << "too few of the texts were read, written and read again";
}

TEST_P(IniSweep, WhatTheValueCodecEncodesIsWrittenAndReadsBackDecodedAsTheText) {
  Sweep sw;
  sw.dialect = GetParam();
  const GTEXT_INI_Dialect d = ini_model::dialects()[GetParam()].make();
  std::vector<std::string> texts = alphabet(d);
  const size_t singles = texts.size();
  for (size_t i = 0; i < singles; i++) {
    if (texts[i].size() > 8) continue;
    for (size_t j = 0; j < singles; j++) {
      if (texts[j].size() <= 8) texts.push_back(texts[i] + texts[j]);
    }
  }
  std::sort(texts.begin(), texts.end());
  texts.erase(std::unique(texts.begin(), texts.end()), texts.end());
  for (const Config & cfg : configs(GetParam())) {
    for (const std::string & t : texts) {
      const Result r = ini_model::check_escaped(cfg, t);
      Tally & tl = sw.tally["escape"];
      tl.cases++;
      sw.cases++;
      if (r.compared) tl.compared++;
      if (!r.failure.empty()) {
        sw.failure_count++;
        Sweep::Bucket & b = sw.buckets[std::string("escape: ") + r.failure];
        b.count++;
        b.shortest.push_back(Sweep::render_document(cfg, "set(k, escape(" + t + "))") + "\n    " + r.detail + "; wrote: " + ini_model::show(r.out));
        std::sort(b.shortest.begin(), b.shortest.end(), [](const std::string & x, const std::string & y) { return x.size() < y.size(); });
        if (b.shortest.size() > 4) b.shortest.resize(4);
      }
    }
  }
  std::string report;
  for (const auto & [what, b] : sw.buckets) {
    report += what + " (" + std::to_string(b.count) + " cases); the shortest:\n";
    for (const std::string & f : b.shortest) report += "  " + f + "\n";
  }
  EXPECT_EQ(sw.failure_count, 0u) << sw.failure_count << " of " << sw.cases << " cases failed:\n" << report;
  const Tally & t = sw.tally["escape"];
  if (std::getenv("GTEXT_INI_SWEEP_TALLY")) {
    std::printf("%-14s escape                   cases %7zu  encoded %7zu\n", ini_model::dialects()[GetParam()].name, t.cases, t.compared);
  }
  EXPECT_GT(t.compared, 1000u) << "too few of the texts could be encoded";
}

std::string dialect_name(const ::testing::TestParamInfo<size_t> & info) {
  std::string n = ini_model::dialects()[info.param].name;
  for (char & c : n) {
    if (c == '-') c = '_';
  }
  return n;
}

INSTANTIATE_TEST_SUITE_P(Dialects, IniSweep, ::testing::Range<size_t>(0, 7), dialect_name);

}   // namespace
