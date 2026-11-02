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
 * The lifetime contract a script binding needs (css_cssom_core.h), tested with the objects that exist before there are rules: internal and external counts, the hold
 * a held child keeps on its parent, a detached subtree, the user slot and its finalizer (once, in the order the rules give, never able to mutate), an object that
 * outlives the document handle, and freeing a very deep chain without the C stack. The parent is a double made of the same base a rule is made of.
 */

#include <functional>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

/// A parent: owns up to four children by an internal count, as a rule owns its block.
struct Node {
  css_obj obj;
  GTEXT_CSSOM_Decl * kids[4];
  Node * node_kid;
};

void node_destroy(css_obj * o) {
  Node * n = reinterpret_cast<Node *>(o);
  css_core * core = o->core;
  GTEXT_Allocator a = core->alloc;
  for (GTEXT_CSSOM_Decl * k : n->kids) {
    if (k) {
      css_obj_release_internal(&k->obj);
    }
  }
  if (n->node_kid) {
    css_obj_release_internal(&n->node_kid->obj);
  }
  gtext_allocator_free(&a, n);
}

Node * make_node(css_core * core, uint64_t internal, uint64_t external) {
  GTEXT_Allocator a = core->alloc;
  Node * n = static_cast<Node *>(gtext_allocator_calloc(&a, 1, sizeof *n));
  css_obj_init(&n->obj, core, GTEXT_CSSOM_OBJECT_RULE, internal, external, sizeof *n, node_destroy);
  return n;
}

/// A declaration the node owns.
GTEXT_CSSOM_Decl * own_decl(Node * parent, size_t slot) {
  GTEXT_CSSOM_Decl * d = nullptr;
  EXPECT_EQ(css_decl_create(parent->obj.core, GTEXT_CSSOM_DECL_RULE, 1, 0, &d), GTEXT_CSS_OK);
  parent->kids[slot] = d;
  css_obj_set_parent(&d->obj, &parent->obj);
  return d;
}

struct Fin {
  std::vector<std::pair<void *, GTEXT_CSSOM_Object_Kind>> calls;
  std::function<void(void *)> then;
  static void run(void * p, GTEXT_CSSOM_Object_Kind kind) {
    Fin * f = static_cast<Fin *>(*static_cast<void **>(p));   // the pointer points at a pointer to the recorder
    f->calls.emplace_back(p, kind);
    if (f->then) {
      f->then(p);
    }
  }
};

// The matrix: "Keep-alive: hold rule = cssRules[0], release the sheet's other references and the document handle: rule.parent_sheet is the sheet; all live until rule is released; no leak".
TEST(CssomLifetime, AHeldChildKeepsItsParentAliveAndTheParentsOwnReferencesDoNotMatter) {
  FaultAllocator fa;
  {
    Doc doc(nullptr, &fa.alloc);
    css_core * core = doc.core();
    Node * parent = make_node(core, 0, 1);
    GTEXT_CSSOM_Decl * kid = own_decl(parent, 0);
    EXPECT_EQ(parent->obj.external, 1u);
    gtext_cssom_decl_retain(kid);   // a script engine's wrapper holds the child
    EXPECT_EQ(parent->obj.external, 2u) << "the first external reference of a child holds its parent externally";
    gtext_cssom_decl_retain(kid);
    EXPECT_EQ(parent->obj.external, 2u) << "only the first";
    css_obj_release_external(&parent->obj);   // the sheet's other references go
    doc.release();                            // and the document handle
    EXPECT_EQ(parent->obj.external, 1u) << "the held child's hold remains";
    EXPECT_EQ(get(kid, "color"), "") << "the child is readable";
    EXPECT_EQ(set(kid, "color", "red").o.kind, GTEXT_CSSOM_CHANGED) << "and writable, with no document";
    gtext_cssom_decl_release(kid);
    EXPECT_EQ(parent->obj.external, 1u);
    gtext_cssom_decl_release(kid);
    // the last external reference of the child went: the hold went, the parent had no other reference, the parent died and released the child it owned
    EXPECT_TRUE(fa.live.empty()) << "the last release freed the parent, the child and the core";
  }
  EXPECT_TRUE(fa.live.empty()) << "nothing leaks";
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(CssomLifetime, ADetachedChildKeepsNoHoldOnItsFormerParent) {
  FaultAllocator fa;
  {
    Doc doc(nullptr, &fa.alloc);
    css_core * core = doc.core();
    Node * parent = make_node(core, 0, 1);
    GTEXT_CSSOM_Decl * kid = own_decl(parent, 0);
    gtext_cssom_decl_retain(kid);
    EXPECT_EQ(parent->obj.external, 2u);
    // removal from the parent: the parent stops owning it (its internal count is released by the removing code) and the weak pointer is cleared
    parent->kids[0] = nullptr;
    css_obj_set_parent(&kid->obj, nullptr);
    EXPECT_EQ(parent->obj.external, 1u) << "the hold moved away with the parent pointer";
    css_obj_release_external(&parent->obj);   // the parent dies at once, though the kid is held
    EXPECT_EQ(get(kid, "width"), "");
    EXPECT_EQ(kid->obj.internal, 1u) << "its owner's count is the removing code's to release";
    css_obj_release_internal(&kid->obj);
    EXPECT_EQ(kid->obj.internal, 0u);
    EXPECT_EQ(set(kid, "width", "1px").o.kind, GTEXT_CSSOM_CHANGED) << "a detached declaration is valid and mutable";
    gtext_cssom_decl_release(kid);
  }
  EXPECT_TRUE(fa.live.empty());
}

TEST(CssomLifetime, AHoldPassesUpTheWholeChain) {
  Doc doc;
  css_core * core = doc.core();
  Node * top = make_node(core, 0, 0);
  Node * mid = make_node(core, 1, 0);
  top->node_kid = mid;
  css_obj_set_parent(&mid->obj, &top->obj);
  GTEXT_CSSOM_Decl * leaf = own_decl(mid, 0);
  EXPECT_EQ(top->obj.external, 0u);
  gtext_cssom_decl_retain(leaf);
  EXPECT_EQ(mid->obj.external, 1u);
  EXPECT_EQ(top->obj.external, 1u) << "a held grandchild keeps the sheet";
  gtext_cssom_decl_release(leaf);
  EXPECT_EQ(mid->obj.external, 0u);
  EXPECT_EQ(top->obj.external, 0u);
  // nothing holds top: it was freed with the last release; the counters say no object is left
  GTEXT_CSSOM_Counters c;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.live_objects, 0u);
}

/* ---- the user slot ------------------------------------------------------------------------------------------------------------------------------------------ */

// The matrix: "User slot: set_user(rule, p, f); replace with (q, g); release the last reference; a mutation from a finalizer: f(p) at the replace, g(q) once at free; the mutation refused (E_REENTRANT)".
TEST(CssomLifetime, TheFinalizerRunsAtAReplaceAtAClearAndOnceAtFree) {
  Doc doc;
  Fin rec;
  void * p1 = &rec;
  void * p2 = &rec;
  void * p3 = &rec;
  Decl d(doc);
  ASSERT_EQ(gtext_cssom_object_set_user(d.d, &p1, &Fin::run), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_object_user(d.d), &p1);
  EXPECT_TRUE(rec.calls.empty());
  ASSERT_EQ(gtext_cssom_object_set_user(d.d, &p2, &Fin::run), GTEXT_CSS_OK);
  ASSERT_EQ(rec.calls.size(), 1u) << "replacing runs the old finalizer";
  EXPECT_EQ(rec.calls[0].first, &p1);
  EXPECT_EQ(rec.calls[0].second, GTEXT_CSSOM_OBJECT_DECLARATION);
  ASSERT_EQ(gtext_cssom_object_set_user(d.d, nullptr, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(rec.calls.size(), 2u) << "clearing runs it";
  EXPECT_EQ(rec.calls[1].first, &p2);
  EXPECT_EQ(gtext_cssom_object_user(d.d), nullptr);
  ASSERT_EQ(gtext_cssom_object_set_user(d.d, &p3, &Fin::run), GTEXT_CSS_OK);
  gtext_cssom_decl_release(d.d);
  d.d = nullptr;
  ASSERT_EQ(rec.calls.size(), 3u) << "and once when the object is freed";
  EXPECT_EQ(rec.calls[2].first, &p3);
  EXPECT_EQ(gtext_cssom_object_set_user(nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
}

TEST(CssomLifetime, AFinalizerCannotMutateAndMayReleaseAndSetUserOnOthers) {
  Doc doc;
  Decl keep(doc);
  GTEXT_CSSOM_Decl *other = nullptr, *victim = nullptr;
  ASSERT_EQ(gtext_cssom_decl_new(doc.d, &other), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_decl_new(doc.d, &victim), GTEXT_CSS_OK);
  Fin rec;
  void * slot = &rec;
  GTEXT_CSS_Status refused[3] = {GTEXT_CSS_OK, GTEXT_CSS_OK, GTEXT_CSS_OK};
  Fin keep_rec;
  void * keep_slot = &keep_rec;
  ASSERT_EQ(gtext_cssom_object_set_user(keep.d, &keep_slot, &Fin::run), GTEXT_CSS_OK);
  rec.then = [&](void *) {
    refused[0] = set(keep.d, "color", "red").st;
    refused[1] = remove(keep.d, "color").st;
    refused[2] = set_text(keep.d, "color: red").st;
    gtext_cssom_object_set_user(keep.d, nullptr, nullptr);   // allowed: another object's slot (its finalizer runs)
    EXPECT_EQ(get(keep.d, "color"), "") << "a read of another object is fine";
    gtext_cssom_decl_release(victim);                         // allowed: another object
  };
  ASSERT_EQ(gtext_cssom_object_set_user(other, &slot, &Fin::run), GTEXT_CSS_OK);
  gtext_cssom_object_set_user(other, nullptr, nullptr);   // the finalizer runs now, on a live object
  EXPECT_EQ(refused[0], GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(refused[1], GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(refused[2], GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(rec.calls.size(), 1u);
  EXPECT_EQ(keep_rec.calls.size(), 1u) << "the finalizer of the other object ran, from inside the first";
  rec.then = nullptr;
  EXPECT_EQ(set(keep.d, "color", "red").o.kind, GTEXT_CSSOM_CHANGED) << "outside the finalizer the mutation is allowed again";
  gtext_cssom_decl_release(other);
  GTEXT_CSSOM_Counters c;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.live_objects, 1u);
}

TEST(CssomLifetime, FinalizersOfAChainRunInTheOrderTheyDieAndEachOnce) {
  Doc doc;
  Fin rec;
  void * tags[3] = {&rec, &rec, &rec};
  Node * parent = make_node(doc.core(), 0, 1);
  GTEXT_CSSOM_Decl * a = own_decl(parent, 0);
  GTEXT_CSSOM_Decl * b = own_decl(parent, 1);
  gtext_cssom_object_set_user(parent, &tags[0], &Fin::run);
  gtext_cssom_object_set_user(a, &tags[1], &Fin::run);
  gtext_cssom_object_set_user(b, &tags[2], &Fin::run);
  css_obj_release_external(&parent->obj);
  ASSERT_EQ(rec.calls.size(), 3u);
  EXPECT_EQ(rec.calls[0].first, &tags[0]) << "the parent's finalizer first: it dies first, then it releases its children";
  EXPECT_EQ(rec.calls[0].second, GTEXT_CSSOM_OBJECT_RULE);
  std::set<void *> rest{rec.calls[1].first, rec.calls[2].first};
  EXPECT_EQ(rest, (std::set<void *>{&tags[1], &tags[2]}));
}

/* ---- the document ------------------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomLifetime, ObjectsOutliveTheDocumentHandleAndTheLastReleaseFreesEverything) {
  FaultAllocator fa;
  GTEXT_CSSOM_Decl * d = nullptr;
  {
    Doc doc(nullptr, &fa.alloc);
    ASSERT_EQ(gtext_cssom_decl_new(doc.d, &d), GTEXT_CSS_OK);
    ASSERT_EQ(set(d, "margin", "1px").o.kind, GTEXT_CSSOM_CHANGED);
    EXPECT_NE(gtext_cssom_decl_document(d), nullptr);
    doc.release();
    EXPECT_EQ(gtext_cssom_decl_document(d), nullptr);
    EXPECT_FALSE(fa.live.empty()) << "the core is alive";
  }
  EXPECT_EQ(text_of(d), "margin: 1px;");
  EXPECT_EQ(set(d, "color", "red").o.kind, GTEXT_CSSOM_CHANGED) << "a mutation acts on a graph that no document evaluates";
  EXPECT_EQ(gtext_cssom_decl_version(d), 2u);
  gtext_cssom_decl_release(d);
  EXPECT_TRUE(fa.live.empty()) << "no leak";
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(CssomLifetime, TwoDocumentsOnOneRegistryAreIndependent) {
  Doc a, b;
  Decl da(a), db(b);
  ASSERT_EQ(set(da.d, "color", "red").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(gtext_cssom_document_generation(a.d), 1u);
  EXPECT_EQ(gtext_cssom_document_generation(b.d), 0u);
  EXPECT_EQ(get(db.d, "color"), "");
  b.release();
  EXPECT_EQ(get(da.d, "color"), "red");
  EXPECT_EQ(reinterpret_cast<css_obj *>(da.d)->id, reinterpret_cast<css_obj *>(db.d)->id) << "ids are per document";
}

// "no recursion in the C stack": a chain of a million objects, each owning the next, freed by one release.
TEST(CssomLifetime, AMillionDeepChainIsFreedWithoutTheCStack) {
  Doc doc([](GTEXT_CSSOM_Config & c) { c.max_live_objects = 2000000; });
  css_core * core = doc.core();
  Node * root = make_node(core, 0, 1);
  Node * tail = root;
  for (size_t i = 0; i < 1000000; i++) {
    Node * next = make_node(core, 1, 0);
    css_obj_set_parent(&next->obj, &tail->obj);
    tail->node_kid = next;
    tail = next;
  }
  GTEXT_CSSOM_Counters c;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.live_objects, 1000001u);
  css_obj_release_external(&root->obj);
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.live_objects, 0u);
  EXPECT_EQ(c.live_bytes, 0u);
}

// A held leaf of a million-deep chain: the hold goes up the chain without recursion too.
TEST(CssomLifetime, AHoldOnTheLeafOfAMillionDeepChainGoesUpWithoutRecursion) {
  Doc doc([](GTEXT_CSSOM_Config & c) { c.max_live_objects = 2000000; });
  css_core * core = doc.core();
  Node * root = make_node(core, 0, 1);
  Node * tail = root;
  for (size_t i = 0; i < 1000000; i++) {
    Node * next = make_node(core, 1, 0);
    css_obj_set_parent(&next->obj, &tail->obj);
    tail->node_kid = next;
    tail = next;
  }
  css_obj_retain_external(&tail->obj);
  EXPECT_EQ(root->obj.external, 2u);
  css_obj_release_external(&root->obj);
  css_obj_release_external(&tail->obj);
  GTEXT_CSSOM_Counters c;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.live_objects, 0u);
}

} // namespace
