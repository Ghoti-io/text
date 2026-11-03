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
 * Element-owned declarations (css_cssom_decl.h): the `style` attribute as the declaration's only storage. The write-back of every mutation, the validation of every call by
 * content (by the attribute's version, by the caller's tree version, by comparing), the counters that make the costs visible, the identity of the declaration of an element,
 * the element handle that is released, the failing binding, and an element of a real Markup tree.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/markup.h>
#include <ghoti.io/text/xml.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

const char * const kA = "element A";
const char * const kB = "element B";
const void * ea() {
  return kA;
}
const void * eb() {
  return kB;
}

struct Fixture {
  Attrs attrs;
  Doc doc;
  GTEXT_CSSOM_Attribute_Binding binding;
  explicit Fixture(bool with_hook = true, bool with_version = true, bool quirks = false, bool trust = false)
      : doc([&](GTEXT_CSSOM_Config & c) {
          if (with_hook) {
            c.tree_version = &Attrs::tree_version;
            c.tree_version_user = &attrs;
          }
          c.quirks = quirks;
          c.inline_trust_notifications = trust;
        }) {
    binding = attrs.binding(with_version);
  }
  GTEXT_CSSOM_Decl * style(const void * e) {
    GTEXT_CSSOM_Decl * d = nullptr;
    EXPECT_EQ(gtext_cssom_inline_style(doc.d, e, &binding, &d), GTEXT_CSS_OK);
    return d;
  }
  GTEXT_CSSOM_Counters counters() {
    GTEXT_CSSOM_Counters c;
    EXPECT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
    return c;
  }
};

// The matrix: "Inline style: getAttribute("style") after style.setProperty; setAttribute("style","color:blue") then style.getPropertyValue("color")  the serialization; blue".
TEST(CssomInline, MutationWritesTheAttributeBackAndAnExternalChangeIsSeenByContent) {
  Fixture f;
  f.attrs.change(ea(), "color: red; margin: 1px");
  Decl d(f.style(ea()));
  ASSERT_NE(d.d, nullptr);
  EXPECT_EQ(gtext_cssom_decl_owner(d.d), GTEXT_CSSOM_DECL_ELEMENT);
  EXPECT_EQ(get(d.d, "color"), "red");
  EXPECT_EQ(get(d.d, "margin-top"), "1px");
  const Result r = set(d.d, "color", "green");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_TRUE(r.o.attribute_written);
  EXPECT_EQ(f.attrs.get(ea()), "color: green; margin: 1px;") << "CSSOM: update style attribute for the declaration block";
  f.attrs.change(ea(), "color:blue");
  EXPECT_EQ(get(d.d, "color"), "blue") << "the attribute is the storage: a change behind the object's back is read by content";
  EXPECT_EQ(get(d.d, "margin-top"), "");
  EXPECT_EQ(text_of(d.d), "color: blue;");
  f.attrs.drop(ea());
  EXPECT_EQ(length(d.d), 0u) << "no attribute: an empty block";
}

// CSSOM "update style attribute for": the attribute is the serialization of the declarations, so a declaration the library cannot judge must be in them, or an edit of
// another property would take it out of the element: data loss. It survives byte for byte, in its place, whatever else is set, removed or written as the text.
TEST(CssomInline, ADeclarationTheLibraryCannotJudgeSurvivesTheWriteBack) {
  Fixture f;
  f.attrs.change(ea(), "color: red; voice-family: male !important; margin: 1px; font: caption");
  Decl d(f.style(ea()));
  EXPECT_EQ(length(d.d), 7u) << "margin is four longhands, the others are one each";
  EXPECT_EQ(set(d.d, "color", "green").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(f.attrs.get(ea()), "color: green; voice-family: male !important; margin: 1px; font: caption;");
  EXPECT_EQ(set(d.d, "width", "2px").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(f.attrs.get(ea()), "color: green; voice-family: male !important; margin: 1px; font: caption; width: 2px;");
  EXPECT_EQ(remove(d.d, "color").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(f.attrs.get(ea()), "voice-family: male !important; margin: 1px; font: caption; width: 2px;");
  // the text setter writes what it parsed, the declaration it cannot judge included
  EXPECT_EQ(set_text(d.d, "glyph-orientation-vertical: auto; color: blue").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(f.attrs.get(ea()), "glyph-orientation-vertical: auto; color: blue;");
  // a change behind its back is read again, the declaration the library cannot judge included
  f.attrs.change(ea(), "glyph-orientation-vertical: auto; width: 1px");
  EXPECT_EQ(get(d.d, "glyph-orientation-vertical"), "auto");
  EXPECT_EQ(set(d.d, "width", "3px").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(f.attrs.get(ea()), "glyph-orientation-vertical: auto; width: 3px;");
  // and it cannot be set
  EXPECT_EQ(set(d.d, "glyph-orientation-vertical", "none").st, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(f.attrs.get(ea()), "glyph-orientation-vertical: auto; width: 3px;");
}

// "update style attribute for": an empty block sets the empty string; the attribute is removed only by the specification's steps.
TEST(CssomInline, AnEmptyBlockSetsTheEmptyStringAndNeverRemovesTheAttribute) {
  Fixture f;
  f.attrs.change(ea(), "color: red");
  Decl d(f.style(ea()));
  const Result r = remove(d.d, "color");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_TRUE(r.o.attribute_written);
  EXPECT_TRUE(f.attrs.present.count(ea()));
  EXPECT_EQ(f.attrs.get(ea()), "");
  EXPECT_EQ(f.attrs.removes, 0u);
}

// The matrix: "cssText write-back: element-owned style.cssText = <the current text>; setProperty of the current value  UNCHANGED, attribute_written true, no version, no event; UNCHANGED, attribute_written false".
TEST(CssomInline, TheAttributeIsWrittenByTheCssTextSetterWhetherOrNotItChangedAnything) {
  Fixture f;
  f.attrs.change(ea(), "color: red");
  Decl d(f.style(ea()));
  const uint64_t v = gtext_cssom_decl_version(d.d);
  const size_t events = f.doc.events.size();
  const size_t sets = f.attrs.sets;
  Result r = set_text(d.d, "color: red;");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_TRUE(r.o.attribute_written) << "CSSOM step 4 is unconditional";
  EXPECT_EQ(f.attrs.sets, sets + 1);
  EXPECT_EQ(f.attrs.get(ea()), "color: red;") << "the attribute now holds the serialization";
  EXPECT_EQ(gtext_cssom_decl_version(d.d), v);
  EXPECT_EQ(f.doc.events.size(), events);
  r = set(d.d, "color", "red");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_FALSE(r.o.attribute_written) << "setProperty writes only if something was updated";
  EXPECT_EQ(f.attrs.sets, sets + 1);
  r = remove(d.d, "width");
  EXPECT_FALSE(r.o.attribute_written) << "removeProperty writes only if something was removed";
  EXPECT_EQ(f.attrs.sets, sets + 1);
  r = set_text(d.d, "color: blue");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_TRUE(r.o.attribute_written);
  ASSERT_EQ(f.doc.events.size(), events + 1);
  EXPECT_EQ(f.doc.events.back().kind, GTEXT_CSSOM_EVENT_INLINE_STYLE_CHANGED);
  EXPECT_TRUE(f.doc.events.back().attribute_written);
  EXPECT_EQ(f.doc.events.back().element, ea()) << "an inline style's event names its element";
}

// "the write-back updates the remembered string so it does not re-parse its own write (the specification's mutation guard)".
TEST(CssomInline, ADeclarationNeverParsesItsOwnWrite) {
  Fixture f;
  Decl d(f.style(ea()));
  const uint64_t reparses = f.counters().inline_reparses;
  for (int i = 0; i < 50; i++) {
    ASSERT_EQ(set(d.d, "width", std::to_string(i + 1) + "px").o.kind, GTEXT_CSSOM_CHANGED);
    ASSERT_EQ(get(d.d, "width"), std::to_string(i + 1) + "px");
  }
  const GTEXT_CSSOM_Counters c = f.counters();
  EXPECT_EQ(c.inline_reparses, reparses);
  EXPECT_EQ(c.inline_compares, 0u) << "the attribute version and the tree version say nothing changed behind the object's back";
  EXPECT_EQ(c.inline_writes, 50u);
}

// The matrix: "Item loop: an element-owned declaration of n declarations, tree version unchanged, item(0) to item(n-1): one validation, inline_compares 1 (a plant that validates per item)."
TEST(CssomInline, AnItemLoopValidatesOnceNotPerItem) {
  for (const bool with_version : {false, true}) {
    Fixture f(true, with_version);
    std::string text;
    for (int i = 0; i < 500; i++) {
      text += "--p" + std::to_string(i) + ": " + std::to_string(i) + "; ";
    }
    f.attrs.change(ea(), text);
    Decl d(f.style(ea()));
    size_t n = length(d.d);
    ASSERT_EQ(n, 500u);
    for (size_t i = 0; i < n; i++) {
      ASSERT_EQ(item(d.d, i), "--p" + std::to_string(i));
    }
    EXPECT_EQ(f.counters().inline_compares, 0u) << "tree version unchanged: nothing is compared";
    EXPECT_GE(f.counters().inline_reads, 500u);
    // the caller's tree moves once: the loop compares once, not per item
    f.attrs.tree++;
    for (size_t i = 0; i < n; i++) {
      ASSERT_EQ(item(d.d, i), "--p" + std::to_string(i));
    }
    // with an attribute version the tree version is not asked: the attribute did not change, so nothing is compared; without one the move of the tree is one comparison
    EXPECT_EQ(f.counters().inline_compares, with_version ? 0u : 1u) << "with_version=" << with_version;
    EXPECT_EQ(f.counters().inline_reparses, 0u) << "it was the same text";
    EXPECT_EQ(f.attrs.gets, 1u + f.counters().inline_compares) << "the attribute was read when the declaration was made, and once for each comparison";
  }
}

// No hook: "A config with neither the version hook nor tree_changed disables cross-call caching": every call compares.
TEST(CssomInline, WithNoHookEveryCallComparesTheAttribute) {
  Fixture f(false, false);
  f.attrs.change(ea(), "color: red");
  Decl d(f.style(ea()));
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(get(d.d, "color"), "red");
  }
  EXPECT_EQ(f.counters().inline_compares, 10u);
  f.attrs.change(ea(), "color: blue");
  EXPECT_EQ(get(d.d, "color"), "blue");
  EXPECT_EQ(f.counters().inline_reparses, 1u);
}

// The caller that notifies instead of offering a version: gtext_cssom_document_tree_changed().
TEST(CssomInline, TreeChangedStandsInForTheVersionHook) {
  Fixture f(false, false);
  f.attrs.change(ea(), "color: red");
  Decl d(f.style(ea()));
  gtext_cssom_document_tree_changed(f.doc.d);   // from here the caller notifies
  ASSERT_EQ(get(d.d, "color"), "red");
  const uint64_t compares = f.counters().inline_compares;
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(get(d.d, "color"), "red");
  }
  EXPECT_EQ(f.counters().inline_compares, compares) << "the first call after the notification compared once; the ten after it nothing";
  f.attrs.change(ea(), "color: blue");
  gtext_cssom_document_tree_changed(f.doc.d);
  EXPECT_EQ(get(d.d, "color"), "blue");
}

// "A caller whose tree notifies it of attribute changes may set the document option inline_trust_notifications ...".
TEST(CssomInline, TrustedNotificationsSkipTheComparison) {
  Fixture f(false, false, false, true);
  f.attrs.change(ea(), "color: red");
  Decl d(f.style(ea()));
  f.attrs.change(ea(), "color: blue");
  EXPECT_EQ(get(d.d, "color"), "red") << "nothing told the document";
  gtext_cssom_inline_style_attribute_changed(f.doc.d, ea());
  EXPECT_EQ(get(d.d, "color"), "blue");
  EXPECT_EQ(get(d.d, "color"), "blue");
  EXPECT_EQ(f.counters().inline_compares, 1u);
}

// The attribute's own version: when it moves the attribute is compared, and when it moves with the same text nothing is parsed.
TEST(CssomInline, TheAttributeVersionDecidesWhetherToCompare) {
  Fixture f(false, true);
  f.attrs.change(ea(), "color: red");
  Decl d(f.style(ea()));
  for (int i = 0; i < 5; i++) {
    ASSERT_EQ(get(d.d, "color"), "red");
  }
  EXPECT_EQ(f.counters().inline_compares, 0u);
  f.attrs.change(ea(), "color: red");   // the same text, a new version
  ASSERT_EQ(get(d.d, "color"), "red");
  EXPECT_EQ(f.counters().inline_compares, 1u);
  EXPECT_EQ(f.counters().inline_reparses, 0u);
  f.attrs.change(ea(), "color: blue");
  ASSERT_EQ(get(d.d, "color"), "blue");
  EXPECT_EQ(f.counters().inline_reparses, 1u);
}

// The matrix: "Inline identity: style asked twice; after the last reference is released and asked again: same object; a new one".
TEST(CssomInline, TheDeclarationOfAnElementIsTheSameObjectWhileAnyoneHoldsIt) {
  Fixture f;
  f.attrs.change(ea(), "color: red");
  GTEXT_CSSOM_Decl * a = f.style(ea());
  GTEXT_CSSOM_Decl * b = f.style(ea());
  EXPECT_EQ(a, b);
  GTEXT_CSSOM_Decl * other = f.style(eb());
  EXPECT_NE(a, other);
  EXPECT_EQ(f.counters().inline_styles, 2u);
  gtext_cssom_object_set_user(a, reinterpret_cast<void *>(0x1), nullptr);
  gtext_cssom_decl_release(b);
  EXPECT_EQ(gtext_cssom_object_user(a), reinterpret_cast<void *>(0x1)) << "still held";
  gtext_cssom_decl_release(a);   // the last
  EXPECT_EQ(f.counters().inline_styles, 1u);
  GTEXT_CSSOM_Decl * again = f.style(ea());
  EXPECT_EQ(gtext_cssom_object_user(again), nullptr) << "a new object";
  EXPECT_EQ(get(again, "color"), "red");
  gtext_cssom_decl_release(again);
  gtext_cssom_decl_release(other);
  EXPECT_EQ(f.counters().inline_styles, 0u);
  EXPECT_EQ(f.counters().live_objects, 0u);
}

// "gtext_cssom_document_element_released(): a reused address never answers for the old element."
TEST(CssomInline, AReleasedElementDetachesItsDeclarationAndANewHandleGetsItsOwn) {
  Fixture f;
  f.attrs.change(ea(), "color: red");
  Decl old(f.style(ea()));
  gtext_cssom_document_element_released(f.doc.d, ea());
  const size_t gets = f.attrs.gets;
  const size_t sets = f.attrs.sets;
  EXPECT_EQ(get(old.d, "color"), "red") << "it keeps what it held";
  EXPECT_EQ(set(old.d, "color", "blue").o.attribute_written, false);
  EXPECT_EQ(get(old.d, "color"), "blue");
  EXPECT_EQ(f.attrs.gets, gets);
  EXPECT_EQ(f.attrs.sets, sets) << "and never calls the binding again";
  f.attrs.change(ea(), "color: green");   // the address is reused by another element
  Decl fresh(f.style(ea()));
  EXPECT_NE(fresh.d, old.d);
  EXPECT_EQ(get(fresh.d, "color"), "green");
  EXPECT_EQ(get(old.d, "color"), "blue");
}

// "a failing binding callback fails the read or write ... and the declaration is as it was."
TEST(CssomInline, AFailingBindingFailsTheCallAndChangesNothing) {
  Fixture f;
  f.attrs.change(ea(), "color: red");
  Decl d(f.style(ea()));
  const std::string before = dump(d.d);
  const uint64_t gen = gtext_cssom_document_generation(f.doc.d);
  f.attrs.fail_set = GTEXT_CSS_E_WRITE;
  EXPECT_EQ(set(d.d, "color", "blue").st, GTEXT_CSS_E_WRITE);
  EXPECT_EQ(remove(d.d, "color").st, GTEXT_CSS_E_WRITE);
  EXPECT_EQ(set_text(d.d, "width: 1px").st, GTEXT_CSS_E_WRITE);
  f.attrs.fail_set = GTEXT_CSS_OK;
  EXPECT_EQ(dump(d.d), before);
  EXPECT_EQ(gtext_cssom_document_generation(f.doc.d), gen);
  EXPECT_EQ(f.attrs.get(ea()), "color: red");
  f.attrs.change(ea(), "color: green");
  f.attrs.fail_get = GTEXT_CSS_E_WRITE;
  size_t n = 0;
  EXPECT_EQ(gtext_cssom_decl_length(d.d, &n), GTEXT_CSS_E_WRITE);
  EXPECT_EQ(set(d.d, "width", "1px").st, GTEXT_CSS_E_WRITE);
  f.attrs.fail_get = GTEXT_CSS_OK;
  EXPECT_EQ(get(d.d, "color"), "green");
  // and a binding that fails at the first ask makes no declaration
  f.attrs.fail_get = GTEXT_CSS_E_WRITE;
  GTEXT_CSSOM_Decl * none = nullptr;
  EXPECT_EQ(gtext_cssom_inline_style(f.doc.d, eb(), &f.binding, &none), GTEXT_CSS_E_WRITE);
  EXPECT_EQ(none, nullptr);
  EXPECT_EQ(f.counters().inline_styles, 1u);
}

// Quirks: "quirks mode affects the declaration parse of an element-owned declaration only through the document's flag".
TEST(CssomInline, TheQuirksFlagMakesAUnitlessLengthValidInAnElementOwnedDeclarationOnly) {
  Fixture q(true, true, true);
  Decl inl(q.style(ea()));
  EXPECT_EQ(set(inl.d, "width", "10").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(get(inl.d, "width"), "10px");
  EXPECT_EQ(set(inl.d, "line-height", "10").o.kind, GTEXT_CSSOM_CHANGED) << "a number is valid there without the quirk";
  q.attrs.change(eb(), "height: 5; color: red");
  Decl other(q.style(eb()));
  EXPECT_EQ(get(other.d, "height"), "5px") << "the parse of the attribute follows the flag";
  Decl free_decl(q.doc);
  EXPECT_EQ(set(free_decl.d, "width", "10").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE) << "a free-standing declaration has no quirks";
  Fixture s;
  Decl strict(s.style(ea()));
  EXPECT_EQ(set(strict.d, "width", "10").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
}

TEST(CssomInline, ABadBindingIsInvalid) {
  Fixture f;
  GTEXT_CSSOM_Decl * d = nullptr;
  GTEXT_CSSOM_Attribute_Binding b = f.binding;
  b.set_attribute = nullptr;
  EXPECT_EQ(gtext_cssom_inline_style(f.doc.d, ea(), &b, &d), GTEXT_CSS_E_INVALID);
  b = f.binding;
  b.size = 4;
  EXPECT_EQ(gtext_cssom_inline_style(f.doc.d, ea(), &b, &d), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_inline_style(f.doc.d, nullptr, &f.binding, &d), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_inline_style(nullptr, ea(), &f.binding, &d), GTEXT_CSS_E_INVALID);
  // a binding without the optional members
  b = f.binding;
  b.size = offsetof(GTEXT_CSSOM_Attribute_Binding, remove_attribute);
  ASSERT_EQ(gtext_cssom_inline_style(f.doc.d, ea(), &b, &d), GTEXT_CSS_OK);
  gtext_cssom_decl_release(d);
}

TEST(CssomInline, TheLimitOnInlineStylesIsExact) {
  Attrs attrs;
  Doc doc([&](GTEXT_CSSOM_Config & c) { c.max_inline_styles = 2; });
  GTEXT_CSSOM_Attribute_Binding b = attrs.binding();
  GTEXT_CSSOM_Decl *x = nullptr, *y = nullptr, *z = nullptr;
  ASSERT_EQ(gtext_cssom_inline_style(doc.d, ea(), &b, &x), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_inline_style(doc.d, eb(), &b, &y), GTEXT_CSS_OK);
  static const char * c3 = "c";
  EXPECT_EQ(gtext_cssom_inline_style(doc.d, c3, &b, &z), GTEXT_CSS_E_LIMIT_CSSOM);
  GTEXT_CSSOM_Decl * again = nullptr;
  EXPECT_EQ(gtext_cssom_inline_style(doc.d, ea(), &b, &again), GTEXT_CSS_OK) << "the same element is no new declaration";
  EXPECT_EQ(again, x);
  gtext_cssom_decl_release(again);
  gtext_cssom_decl_release(x);
  ASSERT_EQ(gtext_cssom_inline_style(doc.d, c3, &b, &z), GTEXT_CSS_OK);
  gtext_cssom_decl_release(y);
  gtext_cssom_decl_release(z);
}

// The released document: the held declaration is readable and writable, but a document-level call is invalid.
TEST(CssomInline, AHeldDeclarationOutlivesTheDocumentHandle) {
  Attrs attrs;
  GTEXT_CSSOM_Decl * d = nullptr;
  {
    Doc doc([&](GTEXT_CSSOM_Config & c) {
      c.tree_version = &Attrs::tree_version;
      c.tree_version_user = &attrs;
    });
    attrs.change(ea(), "color: red");
    GTEXT_CSSOM_Attribute_Binding b = attrs.binding();
    ASSERT_EQ(gtext_cssom_inline_style(doc.d, ea(), &b, &d), GTEXT_CSS_OK);
    doc.release();
    EXPECT_EQ(gtext_cssom_decl_document(d), nullptr);
  }
  EXPECT_EQ(get(d, "color"), "red");
  EXPECT_EQ(set(d, "color", "blue").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(attrs.get(ea()), "color: blue;");
  gtext_cssom_decl_release(d);
}

/* ---- a Markup element ---------------------------------------------------------------------------------------------------------------------------------- */

struct MarkupAttrs {
  std::string scratch;
  static GTEXT_Markup_Node * node(const void * e) {
    return const_cast<GTEXT_Markup_Node *>(static_cast<const GTEXT_Markup_Node *>(e));
  }
};

// "gtext_cssom_markup_inline_binding() is the Markup one"; this is the same mechanism over an element of the Markup tree through the two calls it needs.
TEST(CssomInline, AnElementOfAMarkupTree) {
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_XML_Error err{};
  const std::string xml = "<r><a style=\"color: red; margin: 1px 2px\"/><b/></r>";
  ASSERT_EQ(gtext_xml_parse(xml.data(), xml.size(), nullptr, &doc, &err), GTEXT_XML_OK);
  const GTEXT_Markup_Node * root = gtext_markup_document_element(doc);
  const GTEXT_Markup_Node * a = gtext_markup_node_first_child(root);
  const GTEXT_Markup_Node * b = gtext_markup_node_next_sibling(a);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  struct Ctx {
    size_t gets = 0;
  } ctx;
  GTEXT_CSSOM_Attribute_Binding binding;
  std::memset(&binding, 0, sizeof binding);
  binding.size = sizeof binding;
  binding.user = &ctx;
  binding.get_attribute = [](void * u, const void * e, const char ** v, size_t * n, bool * p) -> GTEXT_CSS_Status {
    static_cast<Ctx *>(u)->gets++;
    GTEXT_Markup_Attribute at;
    *p = gtext_markup_element_get_attribute(static_cast<const GTEXT_Markup_Node *>(e), nullptr, "style", &at);
    *v = *p ? at.value : "";
    *n = *p ? at.value_length : 0;
    return GTEXT_CSS_OK;
  };
  binding.set_attribute = [](void *, const void * e, const char * v, size_t n) -> GTEXT_CSS_Status {
    return gtext_markup_element_set_attribute(MarkupAttrs::node(e), nullptr, nullptr, "style", v, n) == GTEXT_MARKUP_OK ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  };
  Doc cd;
  GTEXT_CSSOM_Decl *da = nullptr, *db = nullptr;
  ASSERT_EQ(gtext_cssom_inline_style(cd.d, a, &binding, &da), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_inline_style(cd.d, b, &binding, &db), GTEXT_CSS_OK);
  EXPECT_EQ(get(da, "margin"), "1px 2px");
  EXPECT_EQ(text_of(db), "");
  EXPECT_EQ(set(db, "width", "5px").o.attribute_written, true);
  GTEXT_Markup_Attribute at;
  ASSERT_TRUE(gtext_markup_element_get_attribute(b, nullptr, "style", &at));
  EXPECT_EQ(std::string(at.value, at.value_length), "width: 5px;");
  ASSERT_EQ(gtext_markup_element_set_attribute(MarkupAttrs::node(a), nullptr, nullptr, "style", "color: blue", 11), GTEXT_MARKUP_OK);
  EXPECT_EQ(get(da, "color"), "blue") << "setAttribute through the Markup API is seen on the next read";
  EXPECT_EQ(get(da, "margin"), "");
  gtext_cssom_decl_release(da);
  gtext_cssom_decl_release(db);
  gtext_markup_document_free(doc);
}

} // namespace
