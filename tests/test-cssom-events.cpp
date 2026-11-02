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
 * The observers of a document and the events they receive (css_cssom_document.h, "observers and events"; the spec's C5 'Change notifications').
 *
 * A table over every public mutator (the functions of the css_cssom_*.h headers that take an outcome): each row is a scenario, the kinds of events it must send, in order. A second test
 * reads the headers and fails for a mutator the table does not list, with a control that shows the reading refuses what it should. Then the contents: the path of a rule as of its event, the
 * selector list before and after a replace with its dependency bits, the condition summary of a media rule, the changed properties of a declaration and the element of an inline style;
 * the order (the state a callback reads is the state after the mutation, the generation is the one it made); the contract of delivery (registration order, reentrancy, an observer added
 * or removed from inside a call, the first failing status); batches (deferred, in order, nested, the objects held for the delivery, the limit, a refusal that changes nothing); and what
 * happens to a batch when the document handle goes.
 */

#include <fstream>
#include <cctype>
#include <set>
#include <sstream>

#include <gtest/gtest.h>

#include "cssom_events_test_support.h"

using namespace coe;

namespace {


/* ---- the table ----------------------------------------------------------------------------------------------------------------------------------------- */

struct Row {
  const char * mutator;                          ///< The function, as the header names it.
  std::function<void(Ctx &)> setup;              ///< Before the observer is registered.
  std::function<GTEXT_CSS_Status(Ctx &)> op;
  const char * expect;                           ///< The kinds of the events, in order.
};

const std::vector<Row> & table() {
  static const std::vector<Row> rows = {
      {"gtext_cssom_decl_set_property", [](Ctx & c) { c.decl(); }, [](Ctx & c) { return set_color(c.decls[0], "red"); }, "DECLARATIONS_CHANGED"},
      {"gtext_cssom_decl_remove_property",
          [](Ctx & c) {
            c.decl();
            set_color(c.decls[0], "red");
          },
          [](Ctx & c) { return gtext_cssom_decl_remove_property(c.decls[0], "color", 5, nullptr, nullptr, nullptr); }, "DECLARATIONS_CHANGED"},
      {"gtext_cssom_decl_set_css_text", [](Ctx & c) { c.decl(); }, [](Ctx & c) { return gtext_cssom_decl_set_css_text(c.decls[0], "color: red; margin: 1px", 23, nullptr); }, "DECLARATIONS_CHANGED"},
      {"gtext_cssom_decl_idl_set", [](Ctx & c) { c.decl(); }, [](Ctx & c) { return gtext_cssom_decl_idl_set(c.decls[0], "backgroundColor", 15, "red", 3, nullptr); }, "DECLARATIONS_CHANGED"},
      {"gtext_cssom_scope_remove",
          [](Ctx & c) {
            GTEXT_CSSOM_Scope * s = c.scope(0);
            GTEXT_CSSOM_Sheet * a = c.sheet("p{}"), * b = c.sheet("q{}");
            EXPECT_EQ(gtext_cssom_scope_insert_sheet(s, a, 0, nullptr), GTEXT_CSS_OK);
            EXPECT_EQ(gtext_cssom_scope_insert_sheet(s, b, 1, nullptr), GTEXT_CSS_OK);
            GTEXT_CSSOM_Sheet * k = c.constructed("r{}");
            EXPECT_EQ(gtext_cssom_scope_adopted_insert(s, k, 0, nullptr), GTEXT_CSS_OK);
          },
          [](Ctx & c) { return gtext_cssom_scope_remove(c.scopes[0], nullptr); }, "SHEET_REMOVED,SHEET_REMOVED,ADOPTED_CHANGED"},
      {"gtext_cssom_scope_reparent",
          [](Ctx & c) {
            c.other = std::make_unique<World>();
            GTEXT_CSSOM_Scope * s = c.scope(0);
            GTEXT_CSSOM_Sheet * k = c.constructed("r{}");
            EXPECT_EQ(gtext_cssom_scope_adopted_insert(s, k, 0, nullptr), GTEXT_CSS_OK);
          },
          [](Ctx & c) { return gtext_cssom_scope_reparent(c.scopes[0], c.other->d(), nullptr, nullptr); }, "ADOPTED_CHANGED"},
      {"gtext_cssom_scope_insert_sheet", [](Ctx & c) { c.sheet("p{}"); }, [](Ctx & c) { return gtext_cssom_scope_insert_sheet(c.doc_scope(), c.sheets[0], 0, nullptr); }, "SHEET_ADDED"},
      {"gtext_cssom_scope_remove_sheet", [](Ctx & c) { c.owned("p{}"); }, [](Ctx & c) { return gtext_cssom_scope_remove_sheet(c.doc_scope(), c.sheets[0], nullptr); }, "SHEET_REMOVED"},
      {"gtext_cssom_scope_adopted_insert", [](Ctx & c) { c.constructed("p{}"); }, [](Ctx & c) { return gtext_cssom_scope_adopted_insert(c.doc_scope(), c.sheets[0], 0, nullptr); }, "ADOPTED_CHANGED"},
      {"gtext_cssom_scope_adopted_remove",
          [](Ctx & c) {
            c.constructed("p{}");
            gtext_cssom_scope_adopted_insert(c.doc_scope(), c.sheets[0], 0, nullptr);
          },
          [](Ctx & c) { return gtext_cssom_scope_adopted_remove(c.doc_scope(), 0, nullptr); }, "ADOPTED_CHANGED"},
      {"gtext_cssom_scope_adopted_set",
          [](Ctx & c) {
            c.constructed("p{}");
            c.constructed("q{}");
          },
          [](Ctx & c) {
            GTEXT_CSSOM_Sheet * list[] = {c.sheets[0], c.sheets[1]};
            return gtext_cssom_scope_adopted_set(c.doc_scope(), list, 2, nullptr);
          },
          "ADOPTED_CHANGED"},
      {"gtext_cssom_document_register_sheet", [](Ctx & c) { c.compiled.push_back(new Compiled("p{color:red}")); },
          [](Ctx & c) { return gtext_cssom_document_register_sheet(c.d(), GTEXT_CSS_ORIGIN_USER, c.compiled[0]->s, nullptr); }, "SHEET_ADDED"},
      {"gtext_cssom_document_unregister_sheet",
          [](Ctx & c) {
            c.compiled.push_back(new Compiled("p{color:red}"));
            gtext_cssom_document_register_sheet(c.d(), GTEXT_CSS_ORIGIN_USER, c.compiled[0]->s, nullptr);
          },
          [](Ctx & c) { return gtext_cssom_document_unregister_sheet(c.d(), c.compiled[0]->s, nullptr); }, "SHEET_REMOVED"},
      {"gtext_cssom_document_set_conditions", [](Ctx & c) { c.conds.push_back(new Cond(500)); },
          [](Ctx & c) { return gtext_cssom_document_set_conditions(c.d(), c.conds[0]->c, nullptr); }, "CONDITIONS_CHANGED"},
      {"gtext_cssom_media_set_text", [](Ctx & c) { c.owned("p{}"); }, [](Ctx & c) { return gtext_cssom_media_set_text(c.media_of(c.sheets[0]), "print", 5, nullptr); }, "SHEET_MEDIA_CHANGED"},
      {"gtext_cssom_media_append", [](Ctx & c) { c.owned("p{}"); }, [](Ctx & c) { return gtext_cssom_media_append(c.media_of(c.sheets[0]), "print", 5, nullptr); }, "SHEET_MEDIA_CHANGED"},
      {"gtext_cssom_media_delete",
          [](Ctx & c) {
            c.owned("p{}");
            gtext_cssom_media_set_text(c.media_of(c.sheets[0]), "print, screen", 13, nullptr);
          },
          [](Ctx & c) { return gtext_cssom_media_delete(c.media_of(c.sheets[0]), "print", 5, nullptr); }, "SHEET_MEDIA_CHANGED"},
      {"gtext_cssom_sheet_replace_sync", [](Ctx & c) { c.constructed("p{color:red}"); }, [](Ctx & c) { return gtext_cssom_sheet_replace_sync(c.sheets[0], "q{}", 3, nullptr); }, "RULE_REPLACED"},
      {"gtext_cssom_sheet_replace_complete",
          [](Ctx & c) {
            c.constructed("p{color:red}");
            EXPECT_EQ(gtext_cssom_sheet_replace_begin(c.sheets[0]), GTEXT_CSS_OK);
          },
          [](Ctx & c) { return gtext_cssom_sheet_replace_complete(c.sheets[0], "q{}", 3, nullptr); }, "RULE_REPLACED"},
      {"gtext_cssom_rule_set_selector_text", [](Ctx & c) { c.owned("p{color:red}"); c.rule_at(c.sheets[0], 0); },
          [](Ctx & c) { return gtext_cssom_rule_set_selector_text(c.rules[0], "q.x", 3, nullptr); }, "RULE_REPLACED"},
      {"gtext_cssom_rule_set_key_text",
          [](Ctx & c) {
            c.owned("@keyframes k{from{color:red}to{color:blue}}");
            c.inner_at(c.rule_at(c.sheets[0], 0), 0);
          },
          [](Ctx & c) { return gtext_cssom_rule_set_key_text(c.rules[1], "10%", 3, nullptr); }, "RULE_REPLACED"},
      {"gtext_cssom_rule_set_attribute", [](Ctx & c) { c.owned("@keyframes k{from{color:red}}"); c.rule_at(c.sheets[0], 0); },
          [](Ctx & c) { return gtext_cssom_rule_set_attribute(c.rules[0], "name", 4, "j", 1, nullptr); }, "RULE_REPLACED"},
      {"gtext_cssom_import_rule_complete",
          [](Ctx & c) {
            c.imp.table["a.css"] = "pending";
            c.owned("@import url(a.css); p{}");
            c.rule_at(c.sheets[0], 0);
            c.sheet("q{}");
          },
          [](Ctx & c) { return gtext_cssom_import_rule_complete(c.rules[0], c.sheets[1], nullptr); }, "IMPORT_COMPLETED"},
      {"gtext_cssom_rule_insert_rule", [](Ctx & c) { c.owned("@media screen{p{}}"); c.rule_at(c.sheets[0], 0); },
          [](Ctx & c) { return gtext_cssom_rule_insert_rule(c.rules[0], "q{}", 3, 0, nullptr, nullptr); }, "RULE_INSERTED"},
      {"gtext_cssom_rule_delete_rule", [](Ctx & c) { c.owned("@media screen{p{}}"); c.rule_at(c.sheets[0], 0); },
          [](Ctx & c) { return gtext_cssom_rule_delete_rule(c.rules[0], 0, nullptr); }, "RULE_REMOVED"},
      {"gtext_cssom_keyframes_append_rule", [](Ctx & c) { c.owned("@keyframes k{from{color:red}}"); c.rule_at(c.sheets[0], 0); },
          [](Ctx & c) { return gtext_cssom_keyframes_append_rule(c.rules[0], "to{color:blue}", 14, nullptr, nullptr); }, "RULE_INSERTED"},
      {"gtext_cssom_keyframes_delete_rule", [](Ctx & c) { c.owned("@keyframes k{from{color:red}to{color:blue}}"); c.rule_at(c.sheets[0], 0); },
          [](Ctx & c) { return gtext_cssom_keyframes_delete_rule(c.rules[0], "to", 2, nullptr); }, "RULE_REMOVED"},
      {"gtext_cssom_sheet_set_disabled", [](Ctx & c) { c.owned("p{}"); }, [](Ctx & c) { return gtext_cssom_sheet_set_disabled(c.sheets[0], true, nullptr); }, "SHEET_DISABLED_CHANGED"},
      {"gtext_cssom_sheet_insert_rule", [](Ctx & c) { c.owned("p{}"); }, [](Ctx & c) { return gtext_cssom_sheet_insert_rule(c.sheets[0], "q{}", 3, 0, nullptr, nullptr); }, "RULE_INSERTED"},
      {"gtext_cssom_sheet_delete_rule", [](Ctx & c) { c.owned("p{}"); }, [](Ctx & c) { return gtext_cssom_sheet_delete_rule(c.sheets[0], 0, nullptr); }, "RULE_REMOVED"},
      {"gtext_cssom_sheet_add_rule", [](Ctx & c) { c.owned("p{}"); }, [](Ctx & c) { return gtext_cssom_sheet_add_rule(c.sheets[0], "q", 1, "color:red", 9, false, 0, nullptr); }, "RULE_INSERTED"},
      {"gtext_cssom_sheet_remove_rule", [](Ctx & c) { c.owned("p{}"); }, [](Ctx & c) { return gtext_cssom_sheet_remove_rule(c.sheets[0], 0, nullptr); }, "RULE_REMOVED"},
  };
  return rows;
}

/// The mutators the table leaves out, and why.
const std::map<std::string, std::string> & excluded() {
  static const std::map<std::string, std::string> m = {
      {"gtext_cssom_document_end_batch", "it delivers the events of the mutations before it and makes none of its own (BatchesDeferDelivery...)"},
  };
  return m;
}

TEST(CssomEvents, EveryMutatorSendsTheEventsThatItsRowSays) {
  for (const Row & row : table()) {
    SCOPED_TRACE(row.mutator);
    Ctx c;
    row.setup(c);
    Rec rec(c.d());
    ASSERT_EQ(rec.status, GTEXT_CSS_OK);
    const uint64_t gen = gtext_cssom_document_generation(c.d());
    GTEXT_CSS_Status st = row.op(c);
    EXPECT_EQ(st, GTEXT_CSS_OK);
    EXPECT_EQ(rec.kinds_text(), row.expect);
    uint64_t last = gen;
    for (const Seen & s : rec.seen) {
      EXPECT_GT(s.generation, last) << "each event carries the generation of its own mutation, in order";
      last = s.generation;
      EXPECT_TRUE(s.complete);
    }
    EXPECT_EQ(gtext_cssom_document_generation(c.d()), last) << "the last event is the generation the call ended at";
  }
}

/// The names of the declarations in a header's text that take an outcome: every `GTEXT_API ... name(... GTEXT_CSSOM_Outcome ...);`.
std::vector<std::string> mutators_in(const std::string & text) {
  std::vector<std::string> out;
  size_t at = 0;
  while ((at = text.find("GTEXT_API", at)) != std::string::npos) {
    const size_t end = text.find(';', at);
    if (end == std::string::npos) {
      break;
    }
    const std::string decl = text.substr(at, end - at);
    const size_t open = decl.find('(');
    if (open != std::string::npos && decl.find("GTEXT_CSSOM_Outcome", open) != std::string::npos) {
      size_t b = open;
      while (b > 0 && (std::isalnum(static_cast<unsigned char>(decl[b - 1])) || decl[b - 1] == '_')) {
        b--;
      }
      const std::string name = decl.substr(b, open - b);
      if (name.rfind("gtext_cssom_", 0) == 0) {
        out.push_back(name);
      }
    }
    at = end;
  }
  return out;
}

TEST(CssomEvents, EveryMutatorOfTheHeadersIsInTheTableOrExcludedWithAReason) {
  const std::string here = __FILE__;
  const std::string dir = here.substr(0, here.find_last_of('/')) + "/../include/ghoti.io/text/css/";
  std::set<std::string> found;
  for (const char * name : {"css_cssom_decl.h", "css_cssom_document.h", "css_cssom_media.h", "css_cssom_rule.h", "css_cssom_sheet.h"}) {
    std::ifstream in(dir + name, std::ios::binary);
    ASSERT_TRUE(in.good()) << dir + name;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    for (const std::string & n : mutators_in(text)) {
      found.insert(n);
    }
  }
  EXPECT_GE(found.size(), 33u) << "the reading found the mutators";
  std::set<std::string> listed;
  for (const Row & row : table()) {
    EXPECT_TRUE(listed.insert(row.mutator).second) << row.mutator << " is listed twice";
  }
  for (const std::string & f : found) {
    EXPECT_TRUE(listed.count(f) || excluded().count(f)) << f << " takes an outcome and is not in the table of events";
  }
  for (const std::string & l : listed) {
    EXPECT_TRUE(found.count(l)) << l << " is in the table and no header has it";
  }
  for (const auto & e : excluded()) {
    EXPECT_TRUE(found.count(e.first)) << e.first << " is excluded and no header has it";
    EXPECT_FALSE(listed.count(e.first)) << e.first << " is both listed and excluded";
  }
  // the control: the reading, run on a header with a mutator the table does not know, finds it
  const std::string synthetic = "GTEXT_API GTEXT_CSS_Status gtext_cssom_made_up_mutator(GTEXT_CSSOM_Sheet * s,\n    GTEXT_CSSOM_Outcome * outcome);\nGTEXT_API int gtext_cssom_not_one(int x);\n";
  const std::vector<std::string> control = mutators_in(synthetic);
  ASSERT_EQ(control.size(), 1u);
  EXPECT_EQ(control[0], "gtext_cssom_made_up_mutator");
  EXPECT_EQ(listed.count(control[0]) + excluded().count(control[0]), 0u) << "and it is neither listed nor excluded, which is what fails the test above";
}

TEST(CssomEvents, AMutationThatChangesNothingSendsNothing) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  GTEXT_CSSOM_Sheet * s = c.owned("p{color:red}");
  Rec rec(c.d());
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_decl_set_property(d, "color", 5, "red", 3, "", 0, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  ASSERT_EQ(gtext_cssom_decl_set_property(d, "color", 5, "nonsense(", 9, "", 0, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  ASSERT_EQ(gtext_cssom_sheet_set_disabled(s, false, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(gtext_cssom_sheet_delete_rule(s, 5, nullptr), GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_EQ(gtext_cssom_sheet_insert_rule(s, "}}", 2, 0, nullptr, nullptr), GTEXT_CSS_E_SYNTAX);
  EXPECT_TRUE(rec.seen.empty()) << rec.kinds_text();
  EXPECT_EQ(o.generation, gtext_cssom_document_generation(c.d()));
}

TEST(CssomEvents, ARuleThatNoSheetHoldsSendsNothingAndMovesNoGeneration) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("p{color:red}");
  GTEXT_CSSOM_Rule * r = c.rule_at(s, 0);
  GTEXT_CSSOM_Decl * d = c.style_of(r);
  ASSERT_EQ(gtext_cssom_sheet_delete_rule(s, 0, nullptr), GTEXT_CSS_OK);
  Rec rec(c.d());
  const uint64_t gen = gtext_cssom_document_generation(c.d());
  ASSERT_EQ(set_color(d, "blue"), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_rule_set_selector_text(r, "q", 1, nullptr), GTEXT_CSS_OK);
  EXPECT_TRUE(rec.seen.empty());
  EXPECT_EQ(gtext_cssom_document_generation(c.d()), gen) << "nothing that evaluates the document can see it";
}

/* ---- the contents ---------------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomEventContents, ARuleInsertedHasItsPathItsSelectorsAndItsDependencies) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("a{} @media screen{ b{} c{} } d{}");
  GTEXT_CSSOM_Rule * media = c.rule_at(s, 1);
  Rec rec(c.d());
  GTEXT_CSSOM_Rule * made = nullptr;
  ASSERT_EQ(gtext_cssom_rule_insert_rule(media, "p.lead > span[title]:hover{color:red}", 37, 1, &made, nullptr), GTEXT_CSS_OK);
  c.rules.push_back(made);
  ASSERT_EQ(rec.seen.size(), 1u);
  const Seen & e = rec.seen[0];
  EXPECT_EQ(e.kind, GTEXT_CSSOM_EVENT_RULE_INSERTED);
  EXPECT_EQ(e.sheet_id, gtext_cssom_sheet_id(s));
  EXPECT_EQ(e.rule_id, gtext_cssom_rule_id(made));
  EXPECT_EQ(e.rule, made);
  EXPECT_EQ(e.sheet, s);
  EXPECT_TRUE(e.has_path);
  EXPECT_EQ(path_text(e.path), "1/1") << "the second rule of the sheet, and inside it the second";
  EXPECT_FALSE(e.detached);
  EXPECT_TRUE(e.complete);
  EXPECT_TRUE(e.has_selectors);
  EXPECT_EQ(e.selectors, "p.lead > span[title]:hover");
  EXPECT_FALSE(e.has_selectors_before);
  EXPECT_NE(e.deps & GTEXT_CSS_DEP_CLASS, 0u);
  EXPECT_NE(e.deps & GTEXT_CSS_DEP_ATTR, 0u);
  EXPECT_NE(e.deps & GTEXT_CSS_DEP_STATE, 0u);
  EXPECT_NE(e.deps & GTEXT_CSS_DEP_TREE, 0u);
  EXPECT_EQ(e.deps & GTEXT_CSS_DEP_ID, 0u) << "and nothing it does not say";
  EXPECT_EQ(join(e.attributes), "title");
  EXPECT_EQ(e.descendants, 0u);
  EXPECT_FALSE(e.has_condition) << "a style rule has none";
}

TEST(CssomEventContents, ARuleThatHoldsRulesListsNoSelectorAndSaysAnythingMayHaveChanged) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("a{}");
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(s, "@media (min-width: 600px){ b{} c{} }", 36, 0, nullptr, nullptr), GTEXT_CSS_OK);
  const Seen & e = rec.seen.at(0);
  EXPECT_EQ(path_text(e.path), "0");
  EXPECT_FALSE(e.has_selectors);
  EXPECT_EQ(e.deps, GTEXT_CSSOM_DEP_ALL);
  EXPECT_EQ(e.descendants, 2u);
  ASSERT_TRUE(e.has_condition) << "an @media rule has a condition";
  EXPECT_NE(e.cond_environment & 1u, 0u) << "bit 0 is the viewport";
  EXPECT_NE(e.cond_features0, 0u) << "and the feature row of min-width is named";
}

TEST(CssomEventContents, AStyleRuleWithRulesInsideItSaysAnythingMayHaveChangedButStillListsItsSelectors) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("a{}");
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(s, ".x{ &:hover{color:red} }", 24, 0, nullptr, nullptr), GTEXT_CSS_OK);
  const Seen & e = rec.seen.at(0);
  EXPECT_TRUE(e.has_selectors);
  EXPECT_EQ(e.selectors, ".x");
  EXPECT_EQ(e.deps, GTEXT_CSSOM_DEP_ALL) << "the rules inside read their selectors relative to this one";
  EXPECT_GE(e.descendants, 1u);
}

TEST(CssomEventContents, ARemovedRuleHasThePathItHadAndIsDetachedAtDelivery) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("a{} @media screen{ b{} c{} } d{}");
  GTEXT_CSSOM_Rule * media = c.rule_at(s, 1);
  GTEXT_CSSOM_Rule * c_rule = c.inner_at(media, 1);
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_rule_delete_rule(media, 1, nullptr), GTEXT_CSS_OK);
  const Seen & e = rec.seen.at(0);
  EXPECT_EQ(e.kind, GTEXT_CSSOM_EVENT_RULE_REMOVED);
  EXPECT_EQ(e.rule, c_rule);
  EXPECT_TRUE(e.has_path);
  EXPECT_EQ(path_text(e.path), "1/1") << "where it was";
  EXPECT_TRUE(e.detached);
  EXPECT_EQ(e.selectors, "c");
  EXPECT_EQ(e.sheet, s);
  EXPECT_EQ(e.sheet_id, gtext_cssom_sheet_id(s));
}

TEST(CssomEventContents, ASelectorTextSetListsTheSelectorsBeforeAndAfterWithTheirDependencies) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("p.a, #b{color:red}");
  GTEXT_CSSOM_Rule * r = c.rule_at(s, 0);
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_rule_set_selector_text(r, "div > [data-x]", 14, nullptr), GTEXT_CSS_OK);
  const Seen & e = rec.seen.at(0);
  EXPECT_EQ(e.kind, GTEXT_CSSOM_EVENT_RULE_REPLACED);
  EXPECT_EQ(e.selectors, "div > [data-x]");
  EXPECT_EQ(e.selectors_before, "p.a, #b");
  EXPECT_TRUE(e.has_selectors_before);
  EXPECT_NE(e.deps & GTEXT_CSS_DEP_ATTR, 0u);
  EXPECT_EQ(e.deps & GTEXT_CSS_DEP_CLASS, 0u);
  EXPECT_NE(e.deps_before & GTEXT_CSS_DEP_CLASS, 0u);
  EXPECT_NE(e.deps_before & GTEXT_CSS_DEP_ID, 0u);
  EXPECT_EQ(e.deps_before & GTEXT_CSS_DEP_ATTR, 0u);
  EXPECT_EQ(path_text(e.path), "0");
  EXPECT_TRUE(e.complete);
  // an invalid selector changes nothing and sends nothing
  rec.seen.clear();
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_rule_set_selector_text(r, "}{", 2, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_IGNORED_INVALID_SELECTOR);
  EXPECT_TRUE(rec.seen.empty());
}

TEST(CssomEventContents, TheMediaTextOfARuleGivesTheConditionOfTheRuleAfterTheChange) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("@media screen{p{}}");
  GTEXT_CSSOM_Rule * r = c.rule_at(s, 0);
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_media_set_text(c.media_of(r), "(prefers-color-scheme: dark)", 28, nullptr), GTEXT_CSS_OK);
  const Seen & e = rec.seen.at(0);
  EXPECT_EQ(e.kind, GTEXT_CSSOM_EVENT_RULE_REPLACED);
  ASSERT_TRUE(e.has_condition);
  EXPECT_NE(e.cond_environment & (1u << 6), 0u) << "bit 6 is the color scheme";
  EXPECT_EQ(e.cond_environment & 1u, 0u) << "and the viewport is no longer a part of it";
  EXPECT_EQ(e.deps, GTEXT_CSSOM_DEP_ALL);
}

TEST(CssomEventContents, TheMediaOfASheetGivesItsConditionAndAllOfItsRulesAreAffected) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("p{}");
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_media_set_text(c.media_of(s), "(min-resolution: 2dppx)", 23, nullptr), GTEXT_CSS_OK);
  const Seen & e = rec.seen.at(0);
  EXPECT_EQ(e.kind, GTEXT_CSSOM_EVENT_SHEET_MEDIA_CHANGED);
  EXPECT_EQ(e.sheet, s);
  EXPECT_FALSE(e.rule);
  EXPECT_FALSE(e.has_path);
  ASSERT_TRUE(e.has_condition);
  EXPECT_NE(e.cond_environment & (1u << 2), 0u) << "bit 2 is the resolution";
  EXPECT_EQ(e.deps, GTEXT_CSSOM_DEP_ALL);
}

TEST(CssomEventContents, ASupportsRuleGivesItsCondition) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("a{}");
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(s, "@supports (display: grid){ p{} }", 32, 0, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_TRUE(rec.seen.at(0).has_condition);
}

TEST(CssomEventContents, AReplacedSheetIsOneEventWithNoRuleAndAnythingMayHaveChanged) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.constructed("p{} q{}");
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_sheet_replace_sync(s, "r{}", 3, nullptr), GTEXT_CSS_OK);
  const Seen & e = rec.seen.at(0);
  EXPECT_EQ(e.kind, GTEXT_CSSOM_EVENT_RULE_REPLACED);
  EXPECT_EQ(e.rule, nullptr);
  EXPECT_EQ(e.rule_id, 0u);
  EXPECT_EQ(e.sheet, s);
  EXPECT_EQ(e.deps, GTEXT_CSSOM_DEP_ALL);
}

TEST(CssomEventContents, ADeclarationChangeListsTheLonghandsOfAShorthandAndTheNamesOfCustomProperties) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  Rec rec(c.d());
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_decl_set_property(d, "margin", 6, "1px 2px", 7, "", 0, &o), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_decl_set_property(d, "--b", 3, "x", 1, "", 0, &o), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_decl_set_property(d, "--a", 3, "y", 1, "", 0, &o), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_decl_set_css_text(d, "all: initial; --c: 1", 20, &o), GTEXT_CSS_OK);
  ASSERT_EQ(rec.seen.size(), 4u);
  EXPECT_EQ(rec.seen[0].props.size(), 4u) << "a shorthand contributes its longhands";
  EXPECT_FALSE(rec.seen[0].all);
  EXPECT_EQ(join(rec.seen[1].customs), "--b");
  EXPECT_EQ(join(rec.seen[2].customs), "--a");
  EXPECT_TRUE(rec.seen[3].all) << "`all` is among them";
  EXPECT_EQ(rec.seen[0].declaration, d);
  EXPECT_EQ(rec.seen[0].rule, nullptr) << "a declaration of no rule";
  EXPECT_EQ(rec.seen[0].deps, 0u) << "and no selector is concerned";
}

TEST(CssomEventContents, TheChangedPropertiesAreAscendingWithoutRepeats) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  ASSERT_EQ(gtext_cssom_decl_set_css_text(d, "margin-left: 1px; color: red; margin: 2px", 40, nullptr), GTEXT_CSS_OK);
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_decl_set_css_text(d, "border: 1px solid red; margin: 3px; color: blue", 48, nullptr), GTEXT_CSS_OK);
  const Seen & e = rec.seen.at(0);
  for (size_t i = 1; i < e.ids.size(); i++) {
    EXPECT_LT(e.ids[i - 1], e.ids[i]) << "ascending, so without repeats";
  }
  EXPECT_FALSE(e.props.empty());
}

TEST(CssomEventContents, TheDeclarationsOfAStyleRuleGiveItsSelectorsAndTheChangedProperties) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("ul > li:first-child{color:red}");
  GTEXT_CSSOM_Rule * r = c.rule_at(s, 0);
  GTEXT_CSSOM_Decl * d = c.style_of(r);
  Rec rec(c.d());
  ASSERT_EQ(set_color(d, "blue"), GTEXT_CSS_OK);
  const Seen & e = rec.seen.at(0);
  EXPECT_EQ(e.kind, GTEXT_CSSOM_EVENT_DECLARATIONS_CHANGED);
  EXPECT_EQ(e.rule, r);
  EXPECT_EQ(e.declaration, d);
  EXPECT_EQ(e.sheet, s);
  EXPECT_EQ(e.rule_id, gtext_cssom_rule_id(r));
  EXPECT_EQ(path_text(e.path), "0");
  EXPECT_EQ(e.selectors, "ul > li:first-child");
  EXPECT_NE(e.deps & GTEXT_CSS_DEP_FIRST_LAST, 0u);
  EXPECT_EQ(join(e.props), "color");
}

TEST(CssomEventContents, AnInlineStyleEventNamesTheElementAndWhetherTheAttributeWasWritten) {
  Doc doc;
  Attrs attrs;
  int element = 0;
  const GTEXT_CSSOM_Attribute_Binding binding = attrs.binding();
  GTEXT_CSSOM_Decl * d = nullptr;
  ASSERT_EQ(gtext_cssom_inline_style(doc.d, &element, &binding, &d), GTEXT_CSS_OK);
  Rec rec(doc.d);
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  ASSERT_EQ(rec.seen.size(), 1u);
  EXPECT_EQ(rec.seen[0].kind, GTEXT_CSSOM_EVENT_INLINE_STYLE_CHANGED);
  EXPECT_EQ(rec.seen[0].element, &element);
  EXPECT_TRUE(rec.seen[0].attribute_written);
  EXPECT_EQ(rec.seen[0].declaration, d);
  EXPECT_EQ(join(rec.seen[0].props), "color");
  gtext_cssom_decl_release(d);
}

TEST(CssomEventContents, TheEnvironmentAndTheListsOfSheetsAreSummarizedForWhatTheyAre) {
  Ctx c;
  GTEXT_CSSOM_Sheet * k = c.constructed("p{}");
  Rec rec(c.d());
  Cond narrow(400);
  ASSERT_EQ(gtext_cssom_document_set_conditions(c.d(), narrow.c, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_scope_adopted_insert(c.doc_scope(), k, 0, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(rec.seen.size(), 2u);
  EXPECT_EQ(rec.seen[0].kind, GTEXT_CSSOM_EVENT_CONDITIONS_CHANGED);
  EXPECT_TRUE(rec.seen[0].has_condition);
  EXPECT_EQ(rec.seen[0].cond_environment, 0x3ffu) << "which aspect differs is not known: every one may";
  EXPECT_EQ(rec.seen[0].deps, 0u) << "and no selector is concerned";
  EXPECT_EQ(rec.seen[1].kind, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED);
  EXPECT_EQ(rec.seen[1].scope, c.doc_scope());
  EXPECT_EQ(rec.seen[1].sheet, k);
  EXPECT_EQ(rec.seen[1].deps, GTEXT_CSSOM_DEP_ALL);
}

TEST(CssomEventContents, ARegisteredUserAgentSheetIsAnEventWithNoSheetObject) {
  Ctx c;
  c.compiled.push_back(new Compiled("p{}"));
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_document_register_sheet(c.d(), GTEXT_CSS_ORIGIN_USER_AGENT, c.compiled[0]->s, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(rec.seen.size(), 1u);
  EXPECT_EQ(rec.seen[0].sheet, nullptr);
  EXPECT_EQ(rec.seen[0].sheet_id, 0u);
}

TEST(CssomEventContents, AFinishedImportNamesTheRuleAndTheSheetThatHoldsIt) {
  Ctx c;
  c.imp.table["a.css"] = "pending";
  GTEXT_CSSOM_Sheet * s = c.owned("@import url(a.css); p{}");
  GTEXT_CSSOM_Rule * imp = c.rule_at(s, 0);
  GTEXT_CSSOM_Sheet * loaded = c.sheet("q{}");
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp, loaded, nullptr), GTEXT_CSS_OK);
  const Seen & e = rec.seen.at(0);
  EXPECT_EQ(e.kind, GTEXT_CSSOM_EVENT_IMPORT_COMPLETED);
  EXPECT_EQ(e.sheet, s);
  EXPECT_EQ(e.rule, imp);
  EXPECT_EQ(e.deps, GTEXT_CSSOM_DEP_ALL);
  EXPECT_EQ(path_text(e.path), "0");
}

/* ---- the order ------------------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomEventOrder, AnObserverReadsTheStateAfterTheMutationAndTheGenerationItMade) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("a{} b{}");
  Rec rec(c.d());
  rec.hook = [&](const GTEXT_CSSOM_Event & e, Seen & seen) {
    seen.note = std::to_string(gtext_cssom_sheet_rule_total(s)) + ":" + std::to_string(gtext_cssom_document_generation(c.d())) + ":" + std::to_string(gtext_cssom_sheet_version(s));
    const GTEXT_CSSOM_Rule_List * l = nullptr;
    EXPECT_EQ(gtext_cssom_sheet_rules(s, &l), GTEXT_CSS_OK);
    size_t index = 99;
    if (e.kind == GTEXT_CSSOM_EVENT_RULE_INSERTED) {
      EXPECT_TRUE(gtext_cssom_rule_list_index_of(l, e.rule, &index));
      seen.note += ":at" + std::to_string(index);
    }
    else {
      EXPECT_FALSE(gtext_cssom_rule_list_index_of(l, e.rule, &index)) << "a removed rule is out of the list when the observer is called";
      EXPECT_TRUE(gtext_cssom_rule_detached(e.rule));
    }
    return GTEXT_CSS_OK;
  };
  const uint64_t v0 = gtext_cssom_sheet_version(s);
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(s, "c{}", 3, 1, nullptr, nullptr), GTEXT_CSS_OK);
  const uint64_t g1 = gtext_cssom_document_generation(c.d());
  ASSERT_EQ(gtext_cssom_sheet_delete_rule(s, 0, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(rec.seen.size(), 2u);
  EXPECT_EQ(rec.seen[0].note, "3:" + std::to_string(g1) + ":" + std::to_string(v0 + 1) + ":at1") << "three rules, the new generation, the new version";
  EXPECT_EQ(rec.seen[0].generation, g1);
  EXPECT_EQ(rec.seen[1].note.substr(0, 2), "2:");
}

TEST(CssomEventOrder, TheOutcomeOfTheCallCarriesTheGenerationOfTheLastEvent) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("a{}");
  Rec rec(c.d());
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(s, "b{}", 3, 0, nullptr, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.generation, rec.seen.at(0).generation);
  EXPECT_EQ(o.observer_status, GTEXT_CSS_OK);
}

/* ---- delivery -------------------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomEventDelivery, ObserversRunInTheOrderTheyWereRegisteredAndEachIsCalledOncePerEvent) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  std::string order;
  Rec a(c.d()), b(c.d()), third(c.d());
  a.hook = [&](const GTEXT_CSSOM_Event &, Seen &) { order += "a"; return GTEXT_CSS_OK; };
  b.hook = [&](const GTEXT_CSSOM_Event &, Seen &) { order += "b"; return GTEXT_CSS_OK; };
  third.hook = [&](const GTEXT_CSSOM_Event &, Seen &) { order += "c"; return GTEXT_CSS_OK; };
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  ASSERT_EQ(set_color(d, "blue"), GTEXT_CSS_OK);
  EXPECT_EQ(order, "abcabc");
  EXPECT_EQ(gtext_cssom_document_observer_count(c.d()), 3u);
}

TEST(CssomEventDelivery, AMutationFromAnObserverIsReentrantAndSoIsABatch) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  GTEXT_CSSOM_Sheet * s = c.owned("a{}");
  Rec rec(c.d());
  std::vector<GTEXT_CSS_Status> got;
  rec.hook = [&](const GTEXT_CSSOM_Event &, Seen &) {
    got.push_back(set_color(d, "green"));
    got.push_back(gtext_cssom_sheet_insert_rule(s, "x{}", 3, 0, nullptr, nullptr));
    got.push_back(gtext_cssom_document_begin_batch(c.d()));
    got.push_back(gtext_cssom_document_end_batch(c.d(), nullptr));
    got.push_back(gtext_cssom_sheet_set_disabled(s, true, nullptr));
    // and a read is allowed
    size_t n = 0;
    got.push_back(gtext_cssom_decl_length(d, &n));
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  ASSERT_EQ(got.size(), 6u);
  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(got[i], GTEXT_CSS_E_REENTRANT) << i;
  }
  EXPECT_EQ(got[5], GTEXT_CSS_OK);
  EXPECT_EQ(get(d, "color"), "red") << "and nothing the observer tried changed anything";
  EXPECT_EQ(rec.seen.size(), 1u);
}

TEST(CssomEventDelivery, TheFirstFailingStatusIsTheOutcomesAndTheMutationStaysAndTheOthersAreCalled) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  Rec a(c.d()), b(c.d()), third(c.d());
  a.hook = [](const GTEXT_CSSOM_Event &, Seen &) { return GTEXT_CSS_E_WRITE; };
  b.hook = [](const GTEXT_CSSOM_Event &, Seen &) { return GTEXT_CSS_E_UNSUPPORTED; };
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_decl_set_property(d, "color", 5, "red", 3, "", 0, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(o.observer_status, GTEXT_CSS_E_WRITE) << "the first";
  EXPECT_EQ(get(d, "color"), "red");
  EXPECT_EQ(a.seen.size(), 1u);
  EXPECT_EQ(b.seen.size(), 1u);
  EXPECT_EQ(third.seen.size(), 1u) << "delivery goes on";
}

TEST(CssomEventDelivery, AnObserverRegisteredDuringADeliveryIsNotCalledForThatEvent) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  Rec first(c.d());
  std::unique_ptr<Rec> late;
  first.hook = [&](const GTEXT_CSSOM_Event &, Seen &) {
    if (!late) {
      late = std::make_unique<Rec>(c.d());
    }
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  ASSERT_TRUE(late);
  EXPECT_EQ(late->status, GTEXT_CSS_OK);
  EXPECT_TRUE(late->seen.empty()) << "it was registered after the event was made";
  ASSERT_EQ(set_color(d, "blue"), GTEXT_CSS_OK);
  EXPECT_EQ(late->seen.size(), 1u) << "and is called for the next";
  late.reset();
}

TEST(CssomEventDelivery, AnObserverRemovedDuringADeliveryIsNotCalledAfterwardNotEvenForThatEvent) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  Rec first(c.d()), second(c.d());
  first.hook = [&](const GTEXT_CSSOM_Event &, Seen &) {
    if (second.handle) {
      EXPECT_EQ(gtext_cssom_document_unobserve(c.d(), second.handle), GTEXT_CSS_OK);
      second.handle = 0;
    }
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  EXPECT_TRUE(second.seen.empty());
  EXPECT_EQ(gtext_cssom_document_observer_count(c.d()), 1u);
  ASSERT_EQ(set_color(d, "blue"), GTEXT_CSS_OK);
  EXPECT_TRUE(second.seen.empty());
  EXPECT_EQ(first.seen.size(), 2u);
}

TEST(CssomEventDelivery, AnObserverMayRemoveItselfAndTheArrayMayGrowUnderTheLoop) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  std::vector<std::unique_ptr<Rec>> more;
  Rec first(c.d());
  first.hook = [&](const GTEXT_CSSOM_Event &, Seen &) {
    for (int i = 0; i < 40; i++) {
      more.push_back(std::make_unique<Rec>(c.d()));   // the array of observers moves
    }
    EXPECT_EQ(gtext_cssom_document_unobserve(c.d(), first.handle), GTEXT_CSS_OK);
    first.handle = 0;
    return GTEXT_CSS_OK;
  };
  Rec last(c.d());
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  EXPECT_EQ(last.seen.size(), 1u) << "the observer after the one that moved the array is still called";
  for (auto & m : more) {
    EXPECT_TRUE(m->seen.empty());
  }
  ASSERT_EQ(set_color(d, "blue"), GTEXT_CSS_OK);
  EXPECT_EQ(first.seen.size(), 1u);
  EXPECT_EQ(more.back()->seen.size(), 1u);
  more.clear();
}

TEST(CssomEventDelivery, TheRegistryHasAHandleLimitAndNamesWhatItDoesNotKnow) {
  Doc doc([](GTEXT_CSSOM_Config & c) { c.max_observers = 3; });
  auto fn = [](void *, const GTEXT_CSSOM_Event *) { return GTEXT_CSS_OK; };
  GTEXT_CSSOM_Observer_Handle h[4] = {0, 0, 0, 0};
  EXPECT_EQ(gtext_cssom_document_observe(nullptr, fn, nullptr, &h[0]), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_document_observe(doc.d, nullptr, nullptr, &h[0]), GTEXT_CSS_E_INVALID);
  ASSERT_EQ(gtext_cssom_document_observe(doc.d, fn, nullptr, &h[0]), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_observe(doc.d, fn, nullptr, &h[1]), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_observe(doc.d, fn, nullptr, nullptr), GTEXT_CSS_OK) << "a handle is optional";
  EXPECT_EQ(gtext_cssom_document_observe(doc.d, fn, nullptr, &h[3]), GTEXT_CSS_E_LIMIT_CSSOM);
  EXPECT_EQ(h[3], 0u) << "written only on success";
  EXPECT_NE(h[0], h[1]);
  EXPECT_NE(h[0], 0u);
  EXPECT_EQ(gtext_cssom_document_unobserve(doc.d, h[0]), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_document_unobserve(doc.d, h[0]), GTEXT_CSS_E_NOT_FOUND) << "once";
  EXPECT_EQ(gtext_cssom_document_unobserve(doc.d, 9999), GTEXT_CSS_E_NOT_FOUND);
  EXPECT_EQ(gtext_cssom_document_unobserve(nullptr, h[1]), GTEXT_CSS_E_INVALID);
  ASSERT_EQ(gtext_cssom_document_observe(doc.d, fn, nullptr, &h[3]), GTEXT_CSS_OK) << "room again";
  EXPECT_NE(h[3], h[0]) << "and a handle is never reused";
  EXPECT_EQ(gtext_cssom_document_observer_count(doc.d), 3u);
  EXPECT_EQ(gtext_cssom_document_observer_count(nullptr), 0u);
}

TEST(CssomEventDelivery, ADocumentWithNoObserverMakesNoEventAndCountsNone) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  EXPECT_EQ(c.w->counters().events_delivered, 0u);
  EXPECT_EQ(c.w->counters().event_calls, 0u);
  Rec rec(c.d());
  ASSERT_EQ(set_color(d, "blue"), GTEXT_CSS_OK);
  EXPECT_EQ(c.w->counters().events_delivered, 1u);
  EXPECT_EQ(c.w->counters().event_calls, 1u);
  EXPECT_EQ(c.w->counters().events_lost, 0u);
}

TEST(CssomEventDelivery, TheDocumentMayBeReleasedFromInsideACallAndNothingIsCalledAfter) {
  auto doc = std::make_unique<Doc>();
  GTEXT_CSSOM_Decl * d = nullptr;
  ASSERT_EQ(gtext_cssom_decl_new(doc->d, &d), GTEXT_CSS_OK);
  Rec first(doc->d), second(doc->d);
  int calls = 0;
  first.hook = [&](const GTEXT_CSSOM_Event &, Seen &) {
    calls++;
    doc->release();   // the handle goes while the event is being delivered
    first.handle = 0;
    second.handle = 0;
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(second.seen.empty()) << "the document is gone: no observer is called after the one that released it";
  EXPECT_EQ(get(d, "color"), "red") << "and the object it held is valid";
  gtext_cssom_decl_release(d);
}

/* ---- batches --------------------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomEventBatch, ABatchDefersDeliveryToTheEndOfTheOutermostOneInTheOrderOfTheMutations) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("a{}");
  GTEXT_CSSOM_Decl * d = c.decl();
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_document_begin_batch(c.d()), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_document_batch_depth(c.d()), 1u);
  const uint64_t v0 = gtext_cssom_sheet_version(s);
  const uint64_t g0 = gtext_cssom_document_generation(c.d());
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(s, "b{}", 3, 1, nullptr, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_begin_batch(c.d()), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_document_batch_depth(c.d()), 2u);
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_sheet_set_disabled(s, true, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_sheet_version(s), v0 + 2) << "versions still bump per mutation";
  EXPECT_EQ(gtext_cssom_document_generation(c.d()), g0 + 3) << "and so does the generation";
  EXPECT_TRUE(rec.seen.empty()) << "nothing yet";
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_document_end_batch(c.d(), &o), GTEXT_CSS_OK);
  EXPECT_TRUE(rec.seen.empty()) << "the inner end delivers nothing";
  EXPECT_EQ(gtext_cssom_document_batch_depth(c.d()), 1u);
  ASSERT_EQ(gtext_cssom_document_end_batch(c.d(), &o), GTEXT_CSS_OK);
  EXPECT_EQ(rec.kinds_text(), "RULE_INSERTED,DECLARATIONS_CHANGED,SHEET_DISABLED_CHANGED");
  EXPECT_EQ(rec.seen[0].generation, g0 + 1);
  EXPECT_EQ(rec.seen[1].generation, g0 + 2);
  EXPECT_EQ(rec.seen[2].generation, g0 + 3);
  EXPECT_EQ(o.observer_status, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_document_batch_depth(c.d()), 0u);
  EXPECT_EQ(gtext_cssom_document_end_batch(c.d(), nullptr), GTEXT_CSS_E_INVALID_STATE) << "none is open";
  ASSERT_EQ(set_color(d, "blue"), GTEXT_CSS_OK);
  EXPECT_EQ(rec.seen.size(), 4u) << "and delivery is immediate again";
}

TEST(CssomEventBatch, AnEmptyBatchDeliversNothingAndAFailingObserverIsReportedAtTheEnd) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  Rec a(c.d()), b(c.d());
  ASSERT_EQ(gtext_cssom_document_begin_batch(c.d()), GTEXT_CSS_OK);
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_document_end_batch(c.d(), &o), GTEXT_CSS_OK);
  EXPECT_TRUE(a.seen.empty());
  a.hook = [](const GTEXT_CSSOM_Event &, Seen &) { return GTEXT_CSS_E_WRITE; };
  ASSERT_EQ(gtext_cssom_document_begin_batch(c.d()), GTEXT_CSS_OK);
  GTEXT_CSSOM_Outcome mut{};
  ASSERT_EQ(gtext_cssom_decl_set_property(d, "color", 5, "red", 3, "", 0, &mut), GTEXT_CSS_OK);
  EXPECT_EQ(mut.observer_status, GTEXT_CSS_OK) << "nothing has been delivered yet";
  ASSERT_EQ(set_color(d, "blue"), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_end_batch(c.d(), &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.observer_status, GTEXT_CSS_E_WRITE);
  EXPECT_EQ(b.seen.size(), 2u) << "and the others were called for every event";
}

TEST(CssomEventBatch, TheObjectsOfAnEventAreHeldForTheDeliveryEvenIfALaterEventRemovedThemAndNobodyElseHoldsThem) {
  Ctx c;
  GTEXT_CSSOM_Sheet * s = c.owned("a{} b{}");
  Rec rec(c.d());
  rec.hook = [](const GTEXT_CSSOM_Event & e, Seen & seen) {
    // the pointers are valid for the call: read through them
    size_t n = 0;
    char * text = nullptr;
    if (e.rule && gtext_cssom_rule_css_text(e.rule, &text, &n) == GTEXT_CSS_OK) {
      seen.note.assign(text, n);
      gtext_cssom_buffer_free(gtext_cssom_document_allocator(gtext_cssom_sheet_document(e.sheet)), text);
    }
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(gtext_cssom_document_begin_batch(c.d()), GTEXT_CSS_OK);
  GTEXT_CSSOM_Rule * made = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(s, "x{color:red}", 12, 0, &made, nullptr), GTEXT_CSS_OK);
  const uint64_t id = gtext_cssom_rule_id(made);
  gtext_cssom_rule_release(made);   // nobody else holds it now
  ASSERT_EQ(gtext_cssom_sheet_delete_rule(s, 0, nullptr), GTEXT_CSS_OK);   // and it is out of the sheet
  ASSERT_EQ(gtext_cssom_document_end_batch(c.d(), nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(rec.seen.size(), 2u);
  EXPECT_EQ(rec.seen[0].kind, GTEXT_CSSOM_EVENT_RULE_INSERTED);
  EXPECT_EQ(rec.seen[0].rule_id, id);
  EXPECT_EQ(rec.seen[0].note, "x { color: red; }");
  EXPECT_TRUE(rec.seen[0].detached) << "a later event of the batch removed it";
  EXPECT_TRUE(rec.seen[0].has_path);
  EXPECT_EQ(path_text(rec.seen[0].path), "0") << "the path is where it was when its own event happened";
  EXPECT_EQ(rec.seen[1].rule_id, id);
  EXPECT_EQ(path_text(rec.seen[1].path), "0");
  EXPECT_EQ(rec.seen[0].selectors, "x");
  EXPECT_EQ(c.w->counters().live_objects > 0, true);
}

TEST(CssomEventBatch, TheCopiesAMutatorsArraysAreMadeIntoSurviveTheCall) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  Rec rec(c.d());
  ASSERT_EQ(gtext_cssom_document_begin_batch(c.d()), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_decl_set_css_text(d, "margin: 1px; --zeta: 1; --alpha: 2; padding: 3px", 48, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_decl_set_css_text(d, "color: red", 10, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_end_batch(c.d(), nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(rec.seen.size(), 2u);
  EXPECT_EQ(rec.seen[0].props.size(), 8u);
  EXPECT_EQ(join(rec.seen[0].customs), "--alpha,--zeta");
  EXPECT_FALSE(rec.seen[1].props.empty());
}

TEST(CssomEventBatch, AnObserverRegisteredInsideABatchIsCalledForTheEventsAfterItOnly) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  Rec early(c.d());
  ASSERT_EQ(gtext_cssom_document_begin_batch(c.d()), GTEXT_CSS_OK);
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  Rec late(c.d());
  ASSERT_EQ(set_color(d, "blue"), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_end_batch(c.d(), nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(early.seen.size(), 2u);
  EXPECT_EQ(late.seen.size(), 1u);
  EXPECT_EQ(late.seen[0].generation, early.seen[1].generation);
}

TEST(CssomEventBatch, ABatchWithNoObserverQueuesNothing) {
  Ctx c;
  GTEXT_CSSOM_Decl * d = c.decl();
  ASSERT_EQ(gtext_cssom_document_begin_batch(c.d()), GTEXT_CSS_OK);
  for (int i = 0; i < 100; i++) {
    ASSERT_EQ(set_color(d, i % 2 ? "red" : "blue"), GTEXT_CSS_OK);
  }
  EXPECT_EQ(c.w->doc->core()->qcount, 0u);
  EXPECT_EQ(c.w->doc->core()->qcap, 0u);
  ASSERT_EQ(gtext_cssom_document_end_batch(c.d(), nullptr), GTEXT_CSS_OK);
}

TEST(CssomEventBatch, AFullBatchRefusesTheMutationAndChangesNothing) {
  Doc doc([](GTEXT_CSSOM_Config & cfg) { cfg.max_queued_events = 3; });
  GTEXT_CSSOM_Decl * d = nullptr;
  ASSERT_EQ(gtext_cssom_decl_new(doc.d, &d), GTEXT_CSS_OK);
  Rec rec(doc.d);
  ASSERT_EQ(gtext_cssom_document_begin_batch(doc.d), GTEXT_CSS_OK);
  ASSERT_EQ(set_color(d, "red"), GTEXT_CSS_OK);
  ASSERT_EQ(set_color(d, "blue"), GTEXT_CSS_OK);
  ASSERT_EQ(set_color(d, "green"), GTEXT_CSS_OK);
  const uint64_t gen = gtext_cssom_document_generation(doc.d);
  const uint64_t version = gtext_cssom_decl_version(d);
  GTEXT_CSSOM_Outcome o{};
  EXPECT_EQ(gtext_cssom_decl_set_property(d, "color", 5, "pink", 4, "", 0, &o), GTEXT_CSS_E_LIMIT_CSSOM);
  EXPECT_EQ(get(d, "color"), "green");
  EXPECT_EQ(gtext_cssom_document_generation(doc.d), gen);
  EXPECT_EQ(gtext_cssom_decl_version(d), version);
  EXPECT_EQ(doc.core()->counters.events_lost, 0u);
  ASSERT_EQ(gtext_cssom_document_end_batch(doc.d, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(rec.seen.size(), 3u);
  EXPECT_EQ(set_color(d, "pink"), GTEXT_CSS_OK) << "room again once the batch is delivered";
  gtext_cssom_decl_release(d);
}

TEST(CssomEventBatch, AMutatorThatSendsSeveralEventsReservesThemAllBeforeItChangesAnything) {
  Doc doc([](GTEXT_CSSOM_Config & cfg) { cfg.max_queued_events = 3; });
  GTEXT_CSSOM_Scope_Options so = gtext_cssom_scope_options_default();
  int handle = 0;
  so.handle = &handle;
  so.shadow = true;
  GTEXT_CSSOM_Scope * scope = nullptr;
  ASSERT_EQ(gtext_cssom_scope_new(doc.d, &so, &scope), GTEXT_CSS_OK);
  GTEXT_CSSOM_Sheet * sheets[4] = {nullptr, nullptr, nullptr, nullptr};
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(gtext_cssom_sheet_new_parsed(doc.d, "p{}", 3, nullptr, &sheets[i]), GTEXT_CSS_OK);
    ASSERT_EQ(gtext_cssom_scope_insert_sheet(scope, sheets[i], i, nullptr), GTEXT_CSS_OK);
  }
  Rec rec(doc.d);
  ASSERT_EQ(gtext_cssom_document_begin_batch(doc.d), GTEXT_CSS_OK);
  const uint64_t gen = gtext_cssom_document_generation(doc.d);
  EXPECT_EQ(gtext_cssom_scope_remove(scope, nullptr), GTEXT_CSS_E_LIMIT_CSSOM) << "four events do not fit in three places";
  EXPECT_EQ(gtext_cssom_scope_sheet_count(scope), 4u) << "and the scope is as it was";
  EXPECT_EQ(gtext_cssom_document_generation(doc.d), gen);
  EXPECT_NE(gtext_cssom_scope_document(scope), nullptr);
  ASSERT_EQ(gtext_cssom_document_end_batch(doc.d, nullptr), GTEXT_CSS_OK);
  EXPECT_TRUE(rec.seen.empty());
  ASSERT_EQ(gtext_cssom_scope_remove(scope, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(rec.seen.size(), 4u);
  for (auto * s : sheets) gtext_cssom_sheet_release(s);
  gtext_cssom_scope_release(scope);
}

TEST(CssomEventBatch, TheDocumentHandleReleasedWithABatchOpenDropsTheQueueAndLeavesNothingBehind) {
  FaultAllocator fa;
  {
    auto doc = std::make_unique<Doc>(nullptr, &fa.alloc);
    GTEXT_CSSOM_Sheet * s = nullptr;
    ASSERT_EQ(gtext_cssom_sheet_new_parsed(doc->d, "a{} b{}", 7, nullptr, &s), GTEXT_CSS_OK);
    GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(doc->d);
    ASSERT_EQ(gtext_cssom_scope_insert_sheet(scope, s, 0, nullptr), GTEXT_CSS_OK);
    Rec rec(doc->d);
    ASSERT_EQ(gtext_cssom_document_begin_batch(doc->d), GTEXT_CSS_OK);
    GTEXT_CSSOM_Rule * made = nullptr;
    ASSERT_EQ(gtext_cssom_sheet_insert_rule(s, "c{}", 3, 0, &made, nullptr), GTEXT_CSS_OK);
    ASSERT_EQ(gtext_cssom_sheet_delete_rule(s, 0, nullptr), GTEXT_CSS_OK);
    ASSERT_EQ(rec.seen.size(), 0u);
    rec.handle = 0;
    doc->release();   // the batch and its holds go with the handle
    EXPECT_TRUE(rec.seen.empty()) << "nothing is delivered after the handle went";
    // the objects the caller holds are valid and a mutation through them sends nothing anywhere
    EXPECT_EQ(gtext_cssom_sheet_delete_rule(s, 0, nullptr), GTEXT_CSS_OK);
    gtext_cssom_rule_release(made);
    gtext_cssom_sheet_release(s);
  }
  EXPECT_TRUE(fa.live.empty()) << "nothing leaks";
  EXPECT_EQ(fa.bad_frees, 0u);
}

}  // namespace
