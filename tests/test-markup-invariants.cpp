/**
 * @file
 *
 * The markup tree's invariant checker, seen to fail.
 *
 * test-markup-tree.cpp, the model test and the fuzzer all call
 * markup_check_invariants() after every operation and treat "nothing wrong" as
 * the passing answer. A checker that returned NULL for everything would pass
 * all three, so this file is the control: it builds a healthy tree, breaks one
 * thing at a time through the internal structures, and requires the checker to
 * name that thing - then puts it back and requires it to be satisfied again.
 *
 * One case per message the checker can give. A message with no case here is an
 * invariant nobody has watched fail.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/markup.h>

#include "../src/markup/markup_internal.h"
#include "markup_test_support.h"

namespace {

using Node = GTEXT_Markup_Node;

struct Fixture {
  FaultAllocator fa;
  GTEXT_Markup_Document * doc = nullptr;
  Node *doctype, *html, *head, *body, *text, *cdata, *comment, *pi, *tmpl, *content,
      *inner, *detached, *detached_kid, *detached2;

  Fixture() {
    GTEXT_Markup_Options o = gtext_markup_options_default();
    o.allocator = &fa.alloc;
    o.max_attributes = 3;
    EXPECT_EQ(gtext_markup_document_new(&o, &doc), GTEXT_MARKUP_OK);
    make();
  }
  ~Fixture() {
    gtext_markup_document_free(doc);
    EXPECT_TRUE(fa.live.empty());
  }

  Node * element(const char * ns, const char * prefix, const char * local) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_element(doc, ns, prefix, local, &n), GTEXT_MARKUP_OK);
    return n;
  }
  void make() {
    EXPECT_EQ(gtext_markup_create_doctype(doc, "html", "pub", nullptr, &doctype),
        GTEXT_MARKUP_OK);
    html = element(GTEXT_MARKUP_NS_HTML, nullptr, "html");
    head = element(GTEXT_MARKUP_NS_HTML, nullptr, "head");
    body = element(GTEXT_MARKUP_NS_HTML, "h", "body");
    gtext_markup_create_text(doc, "text", 4, &text);
    gtext_markup_create_cdata(doc, "cd", 2, &cdata);
    gtext_markup_create_comment(doc, "c", 1, &comment);
    gtext_markup_create_processing_instruction(doc, "pi", "d", 1, &pi);
    tmpl = element(GTEXT_MARKUP_NS_HTML, nullptr, "template");
    gtext_markup_element_ensure_template_content(tmpl, &content);
    inner = element(nullptr, nullptr, "inner");
    gtext_markup_element_set_attribute(body, nullptr, nullptr, "id", "b", 1);
    gtext_markup_element_set_attribute(body, GTEXT_MARKUP_NS_XLINK, "x", "href", "h", 1);
    gtext_markup_append_child(content, inner);
    Node * root = gtext_markup_document_root(doc);
    gtext_markup_append_child(root, doctype);
    gtext_markup_append_child(root, html);
    gtext_markup_append_child(html, head);
    gtext_markup_append_child(html, body);
    gtext_markup_append_child(body, text);
    gtext_markup_append_child(body, cdata);
    gtext_markup_append_child(body, comment);
    gtext_markup_append_child(body, pi);
    gtext_markup_append_child(body, tmpl);
    detached = element(nullptr, nullptr, "detached");
    detached_kid = element(nullptr, nullptr, "kid");
    gtext_markup_append_child(detached, detached_kid);
    detached2 = element(nullptr, nullptr, "detached2");
  }
  std::string check() {
    const char * p = markup_check_invariants(doc);
    return p ? p : "";
  }
  markup_element * el(Node * n) {
    return (markup_element *)n;
  }
};

// Breaks something, expects the checker to say `message`, then repairs it.
struct Case {
  const char * name;
  const char * message;
  std::function<std::function<void()>(Fixture &)> corrupt;  // returns the repair
};

template <typename T>
std::function<void()> set(T & slot, T value) {
  const T old = slot;
  slot = value;
  return [&slot, old] { slot = old; };
}

}  // namespace

TEST(MarkupInvariants, AHealthyTreeSatisfiesTheCheckerAndSoDoesAnEmptyDocument) {
  Fixture f;
  EXPECT_EQ(f.check(), "");
  GTEXT_Markup_Document * empty = nullptr;
  ASSERT_EQ(gtext_markup_document_new(nullptr, &empty), GTEXT_MARKUP_OK);
  EXPECT_EQ(markup_check_invariants(empty), nullptr);
  gtext_markup_document_free(empty);
}

TEST(MarkupInvariants, ANullDocumentIsNotHealthy) {
  EXPECT_STREQ(markup_check_invariants(nullptr), "no document");
}

TEST(MarkupInvariants, EveryInvariantTheCheckerStatesCanBeSeenToFail) {
  const std::vector<Case> cases = {
      {"table size", "the atom table's size is not a power of two",
       [](Fixture & f) { return set(f.doc->bucket_count, f.doc->bucket_count + 1); }},
      {"atom in the wrong bucket", "an atom is in the wrong bucket",
       [](Fixture & f) {
         markup_atom * a = f.el(f.html)->local;
         return set(a->hash, a->hash ^ 1);
       }},
      {"atom length", "an atom's length is not its text's length",
       [](Fixture & f) {
         markup_atom * a = f.el(f.html)->local;
         return set(a->length, a->length + 1);
       }},
      {"fewer atoms than counted", "the table holds fewer atoms than it says",
       [](Fixture & f) { return set(f.doc->atom_count, f.doc->atom_count + 1); }},
      {"more atoms than counted", "the table holds more atoms than it says",
       [](Fixture & f) { return set(f.doc->atom_count, (size_t)1); }},
      {"atom reference count", "an atom's reference count is not the number of references",
       [](Fixture & f) {
         markup_atom * a = f.el(f.html)->local;
         return set(a->refs, a->refs + 1);
       }},
      {"unreferenced atom", "an atom's reference count is not the number of references",
       [](Fixture & f) {
         markup_atom * a = f.el(f.html)->local;
         return set(a->refs, (size_t)0);
       }},
      {"root has a parent", "the root is not a parentless document",
       [](Fixture & f) {
         return set(gtext_markup_document_root(f.doc)->parent, f.html);
       }},
      {"root loses its flag", "the root is not a parentless document",
       [](Fixture & f) {
         Node * r = gtext_markup_document_root(f.doc);
         return set(r->flags, (uint8_t)0);
       }},
      {"foreign document", "a node's document is not the document it is in",
       [](Fixture & f) { return set(f.head->doc, (GTEXT_Markup_Document *)f.body->doc + 1); }},
      // A node is checked by its own visit and, before that, by its parent's
      // look at its siblings, so a defect only the node's own check names has
      // to be planted where no parent looks first: a detached top, a template's
      // content, the root.
      {"unknown kind", "a node has no valid kind",
       [](Fixture & f) { return set(f.detached2->kind, (uint8_t)99); }},
      {"text with children", "a node that cannot have children has some",
       [](Fixture & f) {
         auto a = set(f.text->first_child, f.head);
         auto b = set(f.text->last_child, f.head);
         return [a, b] { b(); a(); };
       }},
      {"first without last", "first_child and last_child disagree about being empty",
       [](Fixture & f) { return set(f.html->last_child, (Node *)nullptr); }},
      {"root flag elsewhere", "the root flag is on a node that is not the root",
       [](Fixture & f) {
         return set(f.content->flags, (uint8_t)(MARKUP_F_CONTENT | MARKUP_F_ROOT));
       }},
      {"content flag on a document", "the content flag is on something that is not a fragment",
       [](Fixture & f) {
         Node * r = gtext_markup_document_root(f.doc);
         return set(r->flags, (uint8_t)(MARKUP_F_ROOT | MARKUP_F_CONTENT));
       }},
      {"child with the wrong parent", "a child's parent is not the node whose list it is in",
       [](Fixture & f) { return set(f.head->parent, f.tmpl); }},
      {"child's prev wrong", "a child's prev link is wrong",
       [](Fixture & f) { return set(f.body->prev, (Node *)nullptr); }},
      {"content in a child list", "the root or a template's content is in a child list",
       [](Fixture & f) { return set(f.head->flags, (uint8_t)MARKUP_F_ROOT); }},
      {"kind may not be there", "a child is of a kind its parent may not hold",
       [](Fixture & f) { return set(f.text->kind, (uint8_t)GTEXT_MARKUP_KIND_DOCTYPE); }},
      {"doctype after the element", "a doctype follows the document's element",
       [](Fixture & f) {
         // Swap the first two children of the root, by value: doctype <-> html.
         Node * root = gtext_markup_document_root(f.doc);
         auto a = set(f.doctype->kind, (uint8_t)GTEXT_MARKUP_KIND_ELEMENT);
         auto b = set(f.html->kind, (uint8_t)GTEXT_MARKUP_KIND_DOCTYPE);
         (void)root;
         return [a, b] { a(); b(); };
       }},
      {"fragment in a child list", "a child is of a kind its parent may not hold",
       [](Fixture & f) { return set(f.head->kind, (uint8_t)GTEXT_MARKUP_KIND_FRAGMENT); }},
      {"last_child wrong", "last_child is not the last child",
       [](Fixture & f) { return set(f.html->last_child, f.head); }},
      {"two elements in a document", "a document has more than one element or doctype",
       [](Fixture & f) {
         auto a = set(f.doctype->kind, (uint8_t)GTEXT_MARKUP_KIND_ELEMENT);
         return a;
       }},
      {"element without a name", "an element has no local name",
       [](Fixture & f) { return set(f.el(f.head)->local, (markup_atom *)nullptr); }},
      {"content not marked", "a template's content is not marked as that element's",
       [](Fixture & f) { return set(f.content->flags, (uint8_t)0); }},
      {"attribute array too small", "an attribute array is smaller than its count",
       [](Fixture & f) { return set(f.el(f.body)->attr_capacity, (size_t)1); }},
      {"too many attributes", "an element has more attributes than the limit",
       [](Fixture & f) {
         // Lower the limit below what the element holds.
         return set(f.doc->options.max_attributes, (size_t)1);
       }},
      {"attribute without a value", "an attribute is missing its name or its terminated value",
       [](Fixture & f) { return set(f.el(f.body)->attrs[0].value, (char *)nullptr); }},
      {"duplicate attribute", "two attributes of one element have the same name",
       [](Fixture & f) {
         markup_element * e = f.el(f.body);
         auto a = set(e->attrs[1].local, e->attrs[0].local);
         auto b = set(e->attrs[1].ns, e->attrs[0].ns);
         return [a, b] { b(); a(); };
       }},
      {"unterminated text", "character data is missing or not terminated",
       [](Fixture & f) {
         // The terminator byte, not the length: a length past the buffer would
         // make the checker read past it, which is the sanitizers' business.
         markup_text * t = (markup_text *)f.text;
         return set(t->data[t->length], 'x');
       }},
      {"pi without data", "a processing instruction is missing its target or data",
       [](Fixture & f) { return set(((markup_pi *)f.pi)->data, (char *)nullptr); }},
      {"doctype without a name", "a doctype has no name",
       [](Fixture & f) { return set(((markup_doctype *)f.doctype)->name, (char *)nullptr); }},
      {"detached list prev", "a detached node's prev link is wrong",
       [](Fixture & f) {
         // The list is detached2 -> detached (most recently created first).
         return set(f.detached->prev, (Node *)nullptr);
       }},
      {"detached list holds a parented node", "the detached list holds a node that is not a detached top",
       [](Fixture & f) { return set(f.detached2->parent, f.html); }},
      {"node count", "the document's node count is not the number of nodes",
       [](Fixture & f) { return set(f.doc->node_count, f.doc->node_count + 1); }},
      {"more reachable than counted", "more nodes are reachable than the document says it holds",
       [](Fixture & f) { return set(f.doc->node_count, (size_t)3); }},
  };
  for (const Case & c : cases) {
    Fixture f;
    ASSERT_EQ(f.check(), "") << c.name << ": the fixture itself is unhealthy";
    const std::function<void()> repair = c.corrupt(f);
    const std::string got = f.check();
    repair();
    EXPECT_EQ(got, c.message) << "case: " << c.name;
    EXPECT_EQ(f.check(), "") << "case: " << c.name << " was not repaired";
  }
}

TEST(MarkupInvariants, ACycleIsReportedAndDoesNotHangTheChecker) {
  Fixture f;
  // Make body its own grandchild by pointing a leaf's first child back up.
  markup_text * t = (markup_text *)f.text;
  (void)t;
  Node * saved = f.head->first_child;
  Node * saved_last = f.head->last_child;
  f.head->first_child = f.html;
  f.head->last_child = f.html;
  const std::string got = f.check();
  f.head->first_child = saved;
  f.head->last_child = saved_last;
  EXPECT_NE(got, "") << "a cycle must not look healthy";
  EXPECT_EQ(f.check(), "");
}
