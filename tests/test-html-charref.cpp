/**
 * @file
 *
 * The HTML named character reference table and its two lookups.
 *
 * The table is generated (tools/html/gen_charrefs.py) and committed, and
 * `make check-html-tables` proves the committed file is what the generator
 * produces. What that cannot say is whether the generator is *right*: a gate
 * that compares a file to the output of the thing that wrote it agrees with
 * itself by construction. So the table is checked here against the data it came
 * from by a different route - entities.json read by this library's own JSON
 * parser, not by the generator's - and against the properties the tokenizer
 * relies on, which are not in the data at all.
 *
 * What is asserted comes from the WHATWG HTML specification's "Named character
 * references" table and the tokenizer's rule for matching one, not from what the
 * code happens to return. Where a test pins a choice, its name says so.
 *
 * The entities.json this reads is the pinned one (tools/html/fetch.sh). It is
 * not tracked, so a clone that has not fetched it fails here and says what to
 * run, rather than skipping: a skip would report the table checked when nothing
 * had been compared.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/json.h>

#include "../src/html/charref_internal.h"

namespace {

std::string name_of(const GTEXT_HTML_CharRef & entry) {
  return std::string(gtext_html_charref_name(&entry), entry.name_length);
}

const GTEXT_HTML_CharRef & entry_at(size_t i) {
  return gtext_html_charref_table[i];
}

// The names once, as strings, for the linear-scan references below: building
// 2,231 of them on every probe is what made those references take seconds.
const std::vector<std::string> & all_names() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (size_t i = 0; i < gtext_html_charref_count; i++) {
      v.push_back(name_of(entry_at(i)));
    }
    return v;
  }();
  return names;
}

// Reference implementations: deliberately the slowest possible reading of the
// specification's sentence, sharing no code with the binary search or the
// alphanumeric-run shortcut they check.
const GTEXT_HTML_CharRef * oracle_find(const std::string & name) {
  const auto & names = all_names();
  for (size_t i = 0; i < names.size(); i++) {
    if (names[i] == name) {
      return &entry_at(i);
    }
  }
  return nullptr;
}

const GTEXT_HTML_CharRef * oracle_longest(const std::string & text) {
  const GTEXT_HTML_CharRef * best = nullptr;
  const auto & names = all_names();
  for (size_t i = 0; i < names.size(); i++) {
    const std::string & name = names[i];
    if (name.size() <= text.size() && text.compare(0, name.size(), name) == 0
        && (!best || name.size() > best->name_length)) {
      best = &entry_at(i);
    }
  }
  return best;
}

bool same(const GTEXT_HTML_CharRef * a, const GTEXT_HTML_CharRef * b) {
  return a == b;
}

std::string expansion(const GTEXT_HTML_CharRef & entry) {
  char buf[32];
  snprintf(buf, sizeof(buf), "U+%04X", entry.first);
  std::string s = buf;
  if (entry.code_points == 2) {
    snprintf(buf, sizeof(buf), " U+%04X", entry.second);
    s += buf;
  }
  return s;
}

}  // namespace

// ---- The table -----------------------------------------------------------

TEST(HtmlCharref, HoldsTheTwoThousandTwoHundredAndThirtyOneNamesTheSpecificationLists) {
  // The WHATWG table at the pinned entities.json. A different number means the
  // pin moved, which should be a deliberate change to this line.
  EXPECT_EQ(gtext_html_charref_count, 2231u);
}

TEST(HtmlCharref, IsSortedStrictlyByTheBytesOfTheNameSoBinarySearchIsSound) {
  for (size_t i = 1; i < gtext_html_charref_count; i++) {
    const std::string a = name_of(entry_at(i - 1));
    const std::string b = name_of(entry_at(i));
    EXPECT_LT(a, b) << "entry " << i << ": " << a << " then " << b;
  }
}

TEST(HtmlCharref, EveryNameIsLettersAndDigitsThenAtMostOneSemicolon) {
  // gtext_html_charref_longest_match() reads its starting length off this, so
  // it is a property the table must have, not an observation about it.
  for (size_t i = 0; i < gtext_html_charref_count; i++) {
    const std::string name = name_of(entry_at(i));
    size_t run = 0;
    while (run < name.size() && isalnum((unsigned char)name[run])
        && (unsigned char)name[run] < 0x80) {
      run++;
    }
    EXPECT_GE(run, 1u) << name;
    EXPECT_TRUE(run == name.size() || (run + 1 == name.size() && name[run] == ';'))
        << name;
  }
}

TEST(HtmlCharref, TheShortestAndLongestNamesAreWhatTheConstantsSay) {
  size_t shortest = SIZE_MAX, longest = 0;
  for (size_t i = 0; i < gtext_html_charref_count; i++) {
    shortest = std::min<size_t>(shortest, entry_at(i).name_length);
    longest = std::max<size_t>(longest, entry_at(i).name_length);
  }
  EXPECT_EQ(shortest, gtext_html_charref_shortest);
  EXPECT_EQ(longest, gtext_html_charref_longest);
  // The spec's longest name, 31 letters and the semicolon.
  EXPECT_EQ(name_of(*gtext_html_charref_find("CounterClockwiseContourIntegral;", 32)),
      "CounterClockwiseContourIntegral;");
}

TEST(HtmlCharref, EveryExpansionIsOneOrTwoScalarValues) {
  for (size_t i = 0; i < gtext_html_charref_count; i++) {
    const GTEXT_HTML_CharRef & e = entry_at(i);
    SCOPED_TRACE(name_of(e));
    ASSERT_TRUE(e.code_points == 1 || e.code_points == 2);
    EXPECT_LE(e.first, 0x10FFFFu);
    EXPECT_FALSE(e.first >= 0xD800 && e.first <= 0xDFFF);
    EXPECT_NE(e.first, 0u);
    if (e.code_points == 2) {
      EXPECT_LE(e.second, 0x10FFFFu);
      EXPECT_FALSE(e.second >= 0xD800 && e.second <= 0xDFFF);
      EXPECT_NE(e.second, 0u);
    }
    else {
      EXPECT_EQ(e.second, 0u);
    }
  }
}

TEST(HtmlCharref, ExactlyOneHundredAndSixNamesExistWithoutTheirSemicolon) {
  // The legacy names. HTML 4 browsers accepted `&amp` and `&copy`; the
  // specification keeps exactly those spellings and no others.
  size_t without = 0;
  for (size_t i = 0; i < gtext_html_charref_count; i++) {
    const std::string name = name_of(entry_at(i));
    if (name.back() != ';') {
      without++;
    }
  }
  EXPECT_EQ(without, 106u);
}

TEST(HtmlCharref, EveryNameWithoutASemicolonExpandsLikeTheNameWithOne) {
  for (size_t i = 0; i < gtext_html_charref_count; i++) {
    const std::string name = name_of(entry_at(i));
    if (name.back() == ';') {
      continue;
    }
    const GTEXT_HTML_CharRef * with = oracle_find(name + ";");
    ASSERT_NE(with, nullptr) << name << " has no semicolon form";
    EXPECT_EQ(with->first, entry_at(i).first) << name;
    EXPECT_EQ(with->second, entry_at(i).second) << name;
    EXPECT_EQ(with->code_points, entry_at(i).code_points) << name;
  }
}

TEST(HtmlCharref, TheTableAgreesWithEntitiesJsonReadByTheLibrarysOwnParser) {
  GTEXT_JSON_Error err;
  memset(&err, 0, sizeof(err));
  GTEXT_JSON_Value * root =
      gtext_json_parse_file("third_party/html/entities.json", nullptr, &err);
  ASSERT_NE(root, nullptr)
      << "third_party/html/entities.json is not here; run tools/html/fetch.sh";

  const size_t n = gtext_json_object_size(root);
  EXPECT_EQ(n, gtext_html_charref_count);

  std::set<std::string> seen;
  for (size_t i = 0; i < n; i++) {
    size_t key_len = 0;
    const char * key = gtext_json_object_key(root, i, &key_len);
    ASSERT_NE(key, nullptr);
    ASSERT_GE(key_len, 2u);
    ASSERT_EQ(key[0], '&');
    const std::string name(key + 1, key_len - 1);
    SCOPED_TRACE(name);
    seen.insert(name);

    const GTEXT_HTML_CharRef * e = gtext_html_charref_find(name.data(), name.size());
    ASSERT_NE(e, nullptr);

    const GTEXT_JSON_Value * cps =
        gtext_json_object_get(
            gtext_json_object_value(root, i), "codepoints", 10);
    ASSERT_NE(cps, nullptr);
    ASSERT_EQ(gtext_json_array_size(cps), (size_t)e->code_points);
    int64_t v = 0;
    ASSERT_EQ(gtext_json_get_i64(gtext_json_array_get(cps, 0), &v), GTEXT_JSON_OK);
    EXPECT_EQ((int64_t)e->first, v);
    if (e->code_points == 2) {
      ASSERT_EQ(gtext_json_get_i64(gtext_json_array_get(cps, 1), &v), GTEXT_JSON_OK);
      EXPECT_EQ((int64_t)e->second, v);
    }
  }
  // Same count, every JSON name found, and the table has no duplicate: so the
  // table holds no name the JSON does not.
  EXPECT_EQ(seen.size(), gtext_html_charref_count);
  gtext_json_free(root);
}

// ---- Facts the specification states --------------------------------------

TEST(HtmlCharref, TheFiveReferencesXmlDefinesAreInTheTableBothWays) {
  struct { const char * name; uint32_t cp; } five[] = {
    {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''}};
  for (const auto & f : five) {
    const GTEXT_HTML_CharRef * e = gtext_html_charref_find(f.name, strlen(f.name));
    ASSERT_NE(e, nullptr) << f.name;
    EXPECT_EQ(e->first, f.cp) << f.name;
    EXPECT_EQ(e->code_points, 1);
  }
  // `apos` is the one of the five HTML 4 never defined, so it has no legacy
  // spelling; the other four do.
  EXPECT_NE(gtext_html_charref_find("amp", 3), nullptr);
  EXPECT_NE(gtext_html_charref_find("lt", 2), nullptr);
  EXPECT_NE(gtext_html_charref_find("gt", 2), nullptr);
  EXPECT_NE(gtext_html_charref_find("quot", 4), nullptr);
  EXPECT_EQ(gtext_html_charref_find("apos", 4), nullptr);
}

TEST(HtmlCharref, SomeNamesExpandToTwoCodePoints) {
  // NotEqualTilde; is U+2242 U+0338 - a base character and a combining one,
  // which is the reason the table cannot hold one code point per name.
  const GTEXT_HTML_CharRef * e = gtext_html_charref_find("NotEqualTilde;", 14);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(expansion(*e), "U+2242 U+0338");
}

TEST(HtmlCharref, CaseMattersBecauseTheTableHasNamesThatDifferOnlyInIt) {
  const GTEXT_HTML_CharRef * lower = gtext_html_charref_find("aacute;", 7);
  const GTEXT_HTML_CharRef * upper = gtext_html_charref_find("Aacute;", 7);
  ASSERT_NE(lower, nullptr);
  ASSERT_NE(upper, nullptr);
  EXPECT_NE(lower->first, upper->first);
  EXPECT_EQ(lower->first, 0xE1u);
  EXPECT_EQ(upper->first, 0xC1u);
}

// ---- Exact lookup ----------------------------------------------------------

TEST(HtmlCharrefFind, FindsEveryEntryByItsOwnName) {
  for (size_t i = 0; i < gtext_html_charref_count; i++) {
    const std::string name = name_of(entry_at(i));
    EXPECT_TRUE(same(gtext_html_charref_find(name.data(), name.size()), &entry_at(i)))
        << name;
  }
}

TEST(HtmlCharrefFind, AgreesWithALinearScanOnNearMissesOfEveryName) {
  // For every name: one byte shorter, one byte longer, and one byte changed in
  // each position. Binary search is easy to get right on hits and wrong at the
  // edges of the ordering; these are the inputs that sit on those edges.
  for (size_t i = 0; i < gtext_html_charref_count; i++) {
    const std::string name = name_of(entry_at(i));
    std::vector<std::string> probes;
    probes.push_back(name.substr(0, name.size() - 1));
    probes.push_back(name + "a");
    probes.push_back(name + ";");
    for (size_t k = 0; k < name.size(); k++) {
      std::string changed = name;
      changed[k] = changed[k] == 'z' ? 'y' : changed[k] + 1;
      probes.push_back(changed);
    }
    for (const std::string & p : probes) {
      EXPECT_TRUE(same(gtext_html_charref_find(p.data(), p.size()), oracle_find(p)))
          << "probe " << p << " near " << name;
    }
  }
}

TEST(HtmlCharrefFind, RefusesWhatCannotBeAName) {
  EXPECT_EQ(gtext_html_charref_find(nullptr, 3), nullptr);
  EXPECT_EQ(gtext_html_charref_find("", 0), nullptr);
  EXPECT_EQ(gtext_html_charref_find("a", 1), nullptr);   // shorter than any name
  const std::string too_long(gtext_html_charref_longest + 1, 'a');
  EXPECT_EQ(gtext_html_charref_find(too_long.data(), too_long.size()), nullptr);
  EXPECT_EQ(gtext_html_charref_find("&amp;", 5), nullptr);  // the & is not part of it
}

TEST(HtmlCharrefFind, DoesNotReadPastTheLengthItWasGiven) {
  // An exact-size heap buffer, so ASan (the sanitizer builds run this file too)
  // reports a read of one byte more than was offered.
  const char text[] = {'a', 'm', 'p', ';'};
  std::vector<char> exact(text, text + 3);
  EXPECT_NE(gtext_html_charref_find(exact.data(), exact.size()), nullptr);
}

// ---- Longest match -------------------------------------------------------

TEST(HtmlCharrefLongest, ConsumesTheLongestNameNotTheLongestRun) {
  // The specification's own example: `&notit;` is the character `not` followed
  // by the text `it;`, because `notit;` is not a name.
  const GTEXT_HTML_CharRef * e = gtext_html_charref_longest_match("notit;", 6);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(name_of(*e), "not");
  EXPECT_EQ(e->first, 0xACu);
  // And `&notin;` is a name, so the longer one wins.
  e = gtext_html_charref_longest_match("notin;", 6);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(name_of(*e), "notin;");
}

TEST(HtmlCharrefLongest, PrefersTheSemicolonFormWhenThereIsOne) {
  const GTEXT_HTML_CharRef * e = gtext_html_charref_longest_match("amp;rest", 8);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(name_of(*e), "amp;");
  e = gtext_html_charref_longest_match("ampx", 4);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(name_of(*e), "amp");
}

TEST(HtmlCharrefLongest, FindsNothingWhereNoNameIsAPrefix) {
  EXPECT_EQ(gtext_html_charref_longest_match("", 0), nullptr);
  EXPECT_EQ(gtext_html_charref_longest_match(";", 1), nullptr);
  EXPECT_EQ(gtext_html_charref_longest_match("zzz;", 4), nullptr);
  EXPECT_EQ(gtext_html_charref_longest_match(nullptr, 5), nullptr);
  // `apos` has no legacy form, so without its semicolon it is not a reference.
  EXPECT_EQ(gtext_html_charref_longest_match("apos ", 5), nullptr);
}

TEST(HtmlCharrefLongest, StopsAtTheLengthItWasGiven) {
  // `amp;` offered as three bytes is `amp`, the legacy form; offered as two it
  // is `am`, which is nothing.
  const GTEXT_HTML_CharRef * e = gtext_html_charref_longest_match("amp;", 3);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(name_of(*e), "amp");
  EXPECT_EQ(gtext_html_charref_longest_match("amp;", 2), nullptr);
}

TEST(HtmlCharrefLongest, DoesNotWalkAMegabyteOfLettersLookingForTheEnd) {
  // The alphanumeric run is measured only as far as the longest name can
  // reach. This is a correctness check on the answer; the cost it protects is
  // the reason the loop is bounded, and is read off the code.
  const std::string big = "amp" + std::string(1 << 20, 'x');
  const GTEXT_HTML_CharRef * e = gtext_html_charref_longest_match(big.data(), big.size());
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(name_of(*e), "amp");
}

TEST(HtmlCharrefLongest, AgreesWithALinearScanForEveryNameAndEveryPrefixOfIt) {
  // The shortcut that starts at the alphanumeric run is the part of this file
  // most likely to be wrong in a way no hand-picked example finds, so it is
  // checked against a reading that has no shortcut: every name, followed by
  // each of several tails, offered at every length.
  const char * tails[] = {"", ";", "a", "=", " ", "9", ";;", "&amp;"};
  for (size_t i = 0; i < gtext_html_charref_count; i++) {
    const std::string name = name_of(entry_at(i));
    for (const char * tail : tails) {
      const std::string text = name + tail;
      for (size_t len = 0; len <= text.size(); len++) {
        const std::string offered = text.substr(0, len);
        ASSERT_TRUE(same(
            gtext_html_charref_longest_match(offered.data(), offered.size()),
            oracle_longest(offered)))
            << "offered \"" << offered << "\"";
      }
    }
  }
}

TEST(HtmlCharrefLongest, AgreesWithALinearScanOnPseudorandomText) {
  // Half the probes are letters, digits and the bytes that end or continue a
  // name, drawn at random; the other half start from a real name and damage it
  // - truncate it, append to it, change a byte - because random letters hit a
  // name about once in five hundred and the interesting inputs are the ones a
  // few edits away from one.
  uint64_t state = 0x9E3779B97F4A7C15ull;
  auto next = [&]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  const std::string alphabet = "aAbBcdeEgilmnopqrstuxyzNOT019;&= ";
  const auto & names = all_names();
  size_t hits = 0;
  const size_t rounds = 40000;
  for (size_t round = 0; round < rounds; round++) {
    std::string s;
    if (round % 2 == 0) {
      const size_t len = next() % 12;
      for (size_t k = 0; k < len; k++) {
        s.push_back(alphabet[next() % alphabet.size()]);
      }
    }
    else {
      s = names[next() % names.size()];
      switch (next() % 4) {
        case 0: s.resize(next() % (s.size() + 1)); break;
        case 1: s += alphabet[next() % alphabet.size()]; break;
        case 2: s[next() % s.size()] = alphabet[next() % alphabet.size()]; break;
        default: s += names[next() % names.size()]; break;
      }
    }
    const GTEXT_HTML_CharRef * got =
        gtext_html_charref_longest_match(s.data(), s.size());
    ASSERT_TRUE(same(got, oracle_longest(s))) << "text \"" << s << "\"";
    hits += got != nullptr;
  }
  // The sweep must be able to see a match, or agreement on "nothing" proves
  // nothing. The generator is seeded, so the count is not variance: it is
  // 11,155 of 40,000 (28%) at the pinned table. The floor is a third of that,
  // there to flag a generator that stopped producing names.
  EXPECT_GT(hits, rounds / 10);
}
