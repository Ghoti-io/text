/**
 * @file
 *
 * The markup tree's contract: what each operation returns and what it leaves
 * behind.
 *
 * The rules asserted here are the DOM's - "pre-insert validity" - which is the
 * only specification a tree shared by XML and HTML has, and they are written
 * out in this file as a table of kinds rather than read back from the code. The
 * model-based test (test-markup-model.cpp) checks the same rules a second way,
 * against a reference implementation, over random sequences of operations.
 *
 * Every test ends the same way, in the fixture: the tree's invariants hold,
 * the allocator holds nothing, and it was never asked to free a pointer it did
 * not give out. A test that only asserted what its operation returned would
 * pass for an operation that corrupted a neighbouring link.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
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

class MarkupTree : public ::testing::Test {
 protected:
  FaultAllocator fa;
  GTEXT_Markup_Document * doc = nullptr;

  GTEXT_Markup_Document * new_doc(GTEXT_Markup_Options opts) {
    opts.allocator = &fa.alloc;
    GTEXT_Markup_Document * d = nullptr;
    EXPECT_EQ(gtext_markup_document_new(&opts, &d), GTEXT_MARKUP_OK);
    return d;
  }

  void SetUp() override {
    doc = new_doc(gtext_markup_options_default());
  }

  void check() {
    const char * problem = markup_check_invariants(doc);
    EXPECT_STREQ(problem ? problem : "ok", "ok");
  }

  void TearDown() override {
    if (doc) {
      check();
      gtext_markup_document_free(doc);
    }
    EXPECT_TRUE(fa.live.empty()) << fa.live.size() << " blocks leaked";
    EXPECT_EQ(fa.bad_frees, 0u);
  }

  Node * element(const char * local, const char * ns = nullptr,
      const char * prefix = nullptr) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_element(doc, ns, prefix, local, &n),
        GTEXT_MARKUP_OK);
    return n;
  }
  Node * text(const char * s) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_text(doc, s, strlen(s), &n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * comment(const char * s) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_comment(doc, s, strlen(s), &n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * fragment() {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_fragment(doc, &n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * doctype(const char * name = "html") {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_doctype(doc, name, nullptr, nullptr, &n),
        GTEXT_MARKUP_OK);
    return n;
  }
  Node * root() {
    return gtext_markup_document_root(doc);
  }
  std::string dump(const Node * n) {
    return dump_to_string(n);
  }
  // Names of a node's children, in order, for compact assertions.
  std::string kids(const Node * parent) {
    std::string out;
    for (Node * c = gtext_markup_node_first_child(parent); c;
        c = gtext_markup_node_next_sibling(c)) {
      if (!out.empty()) {
        out += ' ';
      }
      const char * local = gtext_markup_element_local_name(c);
      out += local ? local : "#";
    }
    return out;
  }
};

}  // namespace

// ---- Documents -------------------------------------------------------------

TEST_F(MarkupTree, ANewDocumentHoldsOnlyItsRoot) {
  EXPECT_EQ(gtext_markup_node_kind(root()), GTEXT_MARKUP_KIND_DOCUMENT);
  EXPECT_EQ(gtext_markup_document_node_count(doc), 1u);
  EXPECT_EQ(gtext_markup_node_first_child(root()), nullptr);
  EXPECT_EQ(gtext_markup_document_element(doc), nullptr);
  EXPECT_EQ(gtext_markup_document_doctype(doc), nullptr);
  EXPECT_EQ(gtext_markup_node_document(root()), doc);
  EXPECT_EQ(dump(root()), "#document\n");
}

TEST_F(MarkupTree, TheDefaultOptionsAreWhatTheHeaderSays) {
  const GTEXT_Markup_Options o = gtext_markup_options_default();
  EXPECT_EQ(o.allocator, nullptr);
  EXPECT_EQ(o.max_depth, 256u);
  EXPECT_EQ(o.max_nodes, 0u);
  EXPECT_EQ(o.max_attributes, 1024u);
}

TEST_F(MarkupTree, DocumentNewRefusesANullOutputAndWritesNothingOnFailure) {
  EXPECT_EQ(gtext_markup_document_new(nullptr, nullptr), GTEXT_MARKUP_E_INVALID);
  GTEXT_Markup_Document * d = reinterpret_cast<GTEXT_Markup_Document *>(0x1);
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.allocator = &fa.alloc;
  fa.arm(1);
  EXPECT_EQ(gtext_markup_document_new(&o, &d), GTEXT_MARKUP_E_OOM);
  fa.disarm();
  EXPECT_EQ(d, reinterpret_cast<GTEXT_Markup_Document *>(0x1));
}

TEST_F(MarkupTree, DocumentNewFailsCleanlyWhenTheRootCannotBeAllocated) {
  // Two allocations: the document, then its root. Failing the second must give
  // back the first, which the fixture's leak check reports if it does not.
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.allocator = &fa.alloc;
  GTEXT_Markup_Document * d = nullptr;
  fa.arm(2);
  EXPECT_EQ(gtext_markup_document_new(&o, &d), GTEXT_MARKUP_E_OOM);
  EXPECT_EQ(d, nullptr);
  fa.disarm();
}

TEST_F(MarkupTree, FreeingNullIsIgnored) {
  gtext_markup_document_free(nullptr);
}

TEST_F(MarkupTree, TheDocumentModeStartsAsNoQuirksAndCanBeSet) {
  EXPECT_EQ(gtext_markup_document_mode(doc), GTEXT_MARKUP_MODE_NO_QUIRKS);
  gtext_markup_document_set_mode(doc, GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_EQ(gtext_markup_document_mode(doc), GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_EQ(gtext_markup_document_mode(nullptr), GTEXT_MARKUP_MODE_NO_QUIRKS);
}

TEST_F(MarkupTree, TheEncodingIsRecordedWithHowItWasDecided) {
  GTEXT_Markup_Encoding_Source source = GTEXT_MARKUP_ENCODING_CALLER;
  EXPECT_EQ(gtext_markup_document_encoding(doc, &source), nullptr);
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_UNKNOWN);

  ASSERT_EQ(gtext_markup_document_set_encoding(doc, "utf-8",
                GTEXT_MARKUP_ENCODING_BOM), GTEXT_MARKUP_OK);
  EXPECT_STREQ(gtext_markup_document_encoding(doc, &source), "utf-8");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);

  // Clearing the label clears the source with it: a source for no label is a
  // claim about nothing.
  ASSERT_EQ(gtext_markup_document_set_encoding(doc, nullptr,
                GTEXT_MARKUP_ENCODING_BOM), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_document_encoding(doc, &source), nullptr);
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_UNKNOWN);
}

TEST_F(MarkupTree, AFailedEncodingChangeKeepsTheOldLabel) {
  ASSERT_EQ(gtext_markup_document_set_encoding(doc, "utf-8",
                GTEXT_MARKUP_ENCODING_CALLER), GTEXT_MARKUP_OK);
  fa.arm(1);
  EXPECT_EQ(gtext_markup_document_set_encoding(doc, "latin-1",
                GTEXT_MARKUP_ENCODING_DECLARATION), GTEXT_MARKUP_E_OOM);
  fa.disarm();
  GTEXT_Markup_Encoding_Source source;
  EXPECT_STREQ(gtext_markup_document_encoding(doc, &source), "utf-8");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_CALLER);
  EXPECT_EQ(gtext_markup_document_set_encoding(doc, "\xff",
                GTEXT_MARKUP_ENCODING_CALLER), GTEXT_MARKUP_E_ENCODING);
}

TEST_F(MarkupTree, TheXmlDeclarationIsRecorded) {
  GTEXT_Markup_Standalone standalone = GTEXT_MARKUP_STANDALONE_YES;
  EXPECT_EQ(gtext_markup_document_xml_version(doc, &standalone), nullptr);
  EXPECT_EQ(standalone, GTEXT_MARKUP_STANDALONE_UNSPECIFIED);
  ASSERT_EQ(gtext_markup_document_set_xml_declaration(doc, "1.0",
                GTEXT_MARKUP_STANDALONE_NO), GTEXT_MARKUP_OK);
  EXPECT_STREQ(gtext_markup_document_xml_version(doc, &standalone), "1.0");
  EXPECT_EQ(standalone, GTEXT_MARKUP_STANDALONE_NO);
  ASSERT_EQ(gtext_markup_document_set_xml_declaration(doc, nullptr,
                GTEXT_MARKUP_STANDALONE_YES), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_document_xml_version(doc, &standalone), nullptr);
  EXPECT_EQ(standalone, GTEXT_MARKUP_STANDALONE_UNSPECIFIED);
}

// ---- Making nodes ----------------------------------------------------------

TEST_F(MarkupTree, ANewElementIsDetachedAndKnowsItsName) {
  Node * e = element("svg", GTEXT_MARKUP_NS_SVG, "s");
  EXPECT_EQ(gtext_markup_node_kind(e), GTEXT_MARKUP_KIND_ELEMENT);
  EXPECT_EQ(gtext_markup_node_parent(e), nullptr);
  EXPECT_STREQ(gtext_markup_element_namespace(e), GTEXT_MARKUP_NS_SVG);
  EXPECT_STREQ(gtext_markup_element_prefix(e), "s");
  EXPECT_STREQ(gtext_markup_element_local_name(e), "svg");
  EXPECT_EQ(gtext_markup_node_document(e), doc);
  EXPECT_EQ(gtext_markup_document_node_count(doc), 2u);
}

TEST_F(MarkupTree, AnEmptyNamespaceOrPrefixIsNoNamespaceOrPrefix) {
  Node * e = element("a", "", "");
  EXPECT_EQ(gtext_markup_element_namespace(e), nullptr);
  EXPECT_EQ(gtext_markup_element_prefix(e), nullptr);
  EXPECT_TRUE(gtext_markup_element_is(e, nullptr, "a"));
  EXPECT_TRUE(gtext_markup_element_is(e, "", "a"));
}

TEST_F(MarkupTree, ElementIsComparesNamespaceAndLocalNameAndIgnoresThePrefix) {
  Node * e = element("rect", GTEXT_MARKUP_NS_SVG, "x");
  EXPECT_TRUE(gtext_markup_element_is(e, GTEXT_MARKUP_NS_SVG, "rect"));
  EXPECT_FALSE(gtext_markup_element_is(e, nullptr, "rect"));
  EXPECT_FALSE(gtext_markup_element_is(e, GTEXT_MARKUP_NS_HTML, "rect"));
  EXPECT_FALSE(gtext_markup_element_is(e, GTEXT_MARKUP_NS_SVG, "Rect"));
  EXPECT_FALSE(gtext_markup_element_is(text("t"), nullptr, "rect"));
  EXPECT_FALSE(gtext_markup_element_is(nullptr, nullptr, "rect"));
  EXPECT_FALSE(gtext_markup_element_is(e, nullptr, nullptr));
}

TEST_F(MarkupTree, ANameThatIsNotXmlIsStoredBecauseHtmlHasThem) {
  // The HTML parser builds elements with names like these; refusing them here
  // would make the tree unable to hold what it exists to hold.
  for (const char * name : {"a$b", "x:y", "1st", "-", "a b", "\xE2\x82\xAC"}) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, name, &n),
        GTEXT_MARKUP_OK) << name;
    EXPECT_STREQ(gtext_markup_element_local_name(n), name);
  }
}

TEST_F(MarkupTree, ACreationFunctionRefusesBadArgumentsAndWritesNothing) {
  Node * sentinel = reinterpret_cast<Node *>(0x1);
  Node * n = sentinel;
  EXPECT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "", &n),
      GTEXT_MARKUP_E_NAME);
  EXPECT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, nullptr, &n),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_create_element(nullptr, nullptr, nullptr, "a", &n),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "a", nullptr),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "\xC3\x28", &n),
      GTEXT_MARKUP_E_ENCODING);
  EXPECT_EQ(gtext_markup_create_element(doc, "\xFF", nullptr, "a", &n),
      GTEXT_MARKUP_E_ENCODING);
  EXPECT_EQ(gtext_markup_create_element(doc, nullptr, "\xFF", "a", &n),
      GTEXT_MARKUP_E_ENCODING);
  EXPECT_EQ(gtext_markup_create_text(doc, nullptr, 3, &n), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_create_text(doc, "\xC0\x80", 2, &n),
      GTEXT_MARKUP_E_ENCODING);
  EXPECT_EQ(gtext_markup_create_processing_instruction(doc, "", "", 0, &n),
      GTEXT_MARKUP_E_NAME);
  EXPECT_EQ(gtext_markup_create_doctype(doc, nullptr, nullptr, nullptr, &n),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(n, sentinel);
  EXPECT_EQ(gtext_markup_document_node_count(doc), 1u);
}

TEST_F(MarkupTree, TextMayHoldANulAndKeepsItsLength) {
  Node * n = nullptr;
  ASSERT_EQ(gtext_markup_create_text(doc, "a\0b", 3, &n), GTEXT_MARKUP_OK);
  size_t length = 0;
  const char * data = gtext_markup_node_data(n, &length);
  ASSERT_EQ(length, 3u);
  EXPECT_EQ(std::string(data, length), std::string("a\0b", 3));
  EXPECT_EQ(data[3], '\0');
}

TEST_F(MarkupTree, EmptyTextIsAllowedAndSoIsANullPointerWithNoLength) {
  Node * n = nullptr;
  ASSERT_EQ(gtext_markup_create_text(doc, nullptr, 0, &n), GTEXT_MARKUP_OK);
  size_t length = 99;
  EXPECT_STREQ(gtext_markup_node_data(n, &length), "");
  EXPECT_EQ(length, 0u);
}

TEST_F(MarkupTree, ADoctypeTellsAnAbsentIdentifierFromAnEmptyOne) {
  Node * absent = nullptr;
  Node * empty = nullptr;
  ASSERT_EQ(gtext_markup_create_doctype(doc, "html", nullptr, nullptr, &absent),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_create_doctype(doc, "html", "", "", &empty),
      GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_doctype_public_id(absent), nullptr);
  EXPECT_EQ(gtext_markup_doctype_system_id(absent), nullptr);
  EXPECT_STREQ(gtext_markup_doctype_public_id(empty), "");
  EXPECT_STREQ(gtext_markup_doctype_system_id(empty), "");
  EXPECT_NE(dump(absent), dump(empty));
  EXPECT_FALSE(gtext_markup_equal(absent, empty));
}

TEST_F(MarkupTree, ADoctypeNameMayBeEmptyBecauseHtmlAllowsIt) {
  Node * n = nullptr;
  ASSERT_EQ(gtext_markup_create_doctype(doc, "", nullptr, nullptr, &n),
      GTEXT_MARKUP_OK);
  EXPECT_STREQ(gtext_markup_doctype_name(n), "");
}

TEST_F(MarkupTree, AProcessingInstructionHasATargetAndData) {
  Node * n = nullptr;
  ASSERT_EQ(gtext_markup_create_processing_instruction(
                doc, "xml-stylesheet", "href=\"a.css\"", 12, &n), GTEXT_MARKUP_OK);
  EXPECT_STREQ(gtext_markup_pi_target(n), "xml-stylesheet");
  size_t length = 0;
  EXPECT_EQ(std::string(gtext_markup_node_data(n, &length), 12),
      "href=\"a.css\"");
  EXPECT_EQ(gtext_markup_pi_target(text("x")), nullptr);
}

TEST_F(MarkupTree, AccessorsForTheWrongKindAnswerNullAndNotACrash) {
  Node * t = text("t");
  EXPECT_EQ(gtext_markup_element_local_name(t), nullptr);
  EXPECT_EQ(gtext_markup_element_namespace(t), nullptr);
  EXPECT_EQ(gtext_markup_doctype_name(t), nullptr);
  EXPECT_EQ(gtext_markup_node_data(element("a"), nullptr), nullptr);
  EXPECT_EQ(gtext_markup_element_attribute_count(t), 0u);
  EXPECT_EQ(gtext_markup_node_kind(nullptr), GTEXT_MARKUP_KIND_NONE);
  EXPECT_EQ(gtext_markup_node_parent(nullptr), nullptr);
  EXPECT_EQ(gtext_markup_node_first_child(nullptr), nullptr);
  EXPECT_EQ(gtext_markup_node_child_count(nullptr), 0u);
  EXPECT_EQ(gtext_markup_node_document(nullptr), nullptr);
}

TEST_F(MarkupTree, SettingDataReplacesItAndChecksIt) {
  Node * t = text("old");
  ASSERT_EQ(gtext_markup_node_set_data(t, "new data", 8), GTEXT_MARKUP_OK);
  size_t length = 0;
  const char * data = gtext_markup_node_data(t, &length);
  EXPECT_EQ(std::string(data, length), "new data");
  EXPECT_EQ(gtext_markup_node_set_data(t, "\xFF", 1), GTEXT_MARKUP_E_ENCODING);
  EXPECT_EQ(gtext_markup_node_set_data(element("e"), "x", 1),
      GTEXT_MARKUP_E_INVALID);
  fa.arm(1);
  EXPECT_EQ(gtext_markup_node_set_data(t, "again", 5), GTEXT_MARKUP_E_OOM);
  fa.disarm();
  data = gtext_markup_node_data(t, &length);
  EXPECT_EQ(std::string(data, length), "new data");
}

// ---- Which kinds may hold which -------------------------------------------

namespace {

enum Want { kOk, kHierarchy };

// The DOM's table, written from the specification and not from the code:
// "pre-insert validity" step 2 (the parent must be a document, document
// fragment or element) and 4 (a Text node cannot go in a document; a doctype
// only goes in a document), with the document's one-element rule tested
// separately.
Want expected(GTEXT_Markup_Kind parent, GTEXT_Markup_Kind child) {
  const bool container = parent == GTEXT_MARKUP_KIND_DOCUMENT
      || parent == GTEXT_MARKUP_KIND_FRAGMENT || parent == GTEXT_MARKUP_KIND_ELEMENT;
  if (!container) {
    return kHierarchy;
  }
  switch (child) {
    case GTEXT_MARKUP_KIND_ELEMENT:
    case GTEXT_MARKUP_KIND_COMMENT:
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
      return kOk;
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
      return parent == GTEXT_MARKUP_KIND_DOCUMENT ? kHierarchy : kOk;
    case GTEXT_MARKUP_KIND_DOCTYPE:
      return parent == GTEXT_MARKUP_KIND_DOCUMENT ? kOk : kHierarchy;
    case GTEXT_MARKUP_KIND_FRAGMENT:
      return kOk;  // an empty fragment inserts nothing
    default:
      return kHierarchy;
  }
}

}  // namespace

TEST_F(MarkupTree, EveryParentAndChildKindIsAllowedExactlyWhereTheDomAllowsIt) {
  const GTEXT_Markup_Kind kinds[] = {
      GTEXT_MARKUP_KIND_DOCUMENT, GTEXT_MARKUP_KIND_FRAGMENT,
      GTEXT_MARKUP_KIND_ELEMENT, GTEXT_MARKUP_KIND_TEXT, GTEXT_MARKUP_KIND_CDATA,
      GTEXT_MARKUP_KIND_COMMENT, GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION,
      GTEXT_MARKUP_KIND_DOCTYPE};
  auto make = [&](GTEXT_Markup_Kind k) -> Node * {
    Node * n = nullptr;
    switch (k) {
      case GTEXT_MARKUP_KIND_DOCUMENT: return root();
      case GTEXT_MARKUP_KIND_FRAGMENT: return fragment();
      case GTEXT_MARKUP_KIND_ELEMENT: return element("e");
      case GTEXT_MARKUP_KIND_TEXT: return text("t");
      case GTEXT_MARKUP_KIND_CDATA:
        EXPECT_EQ(gtext_markup_create_cdata(doc, "c", 1, &n), GTEXT_MARKUP_OK);
        return n;
      case GTEXT_MARKUP_KIND_COMMENT: return comment("c");
      case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
        EXPECT_EQ(gtext_markup_create_processing_instruction(
                      doc, "t", "d", 1, &n), GTEXT_MARKUP_OK);
        return n;
      case GTEXT_MARKUP_KIND_DOCTYPE: return doctype();
      default: return nullptr;
    }
  };
  for (GTEXT_Markup_Kind pk : kinds) {
    for (GTEXT_Markup_Kind ck : kinds) {
      if (ck == GTEXT_MARKUP_KIND_DOCUMENT) {
        continue;  // tested below: a document is never a child
      }
      Node * parent = make(pk);
      Node * child = make(ck);
      const std::string before = dump(parent);
      const GTEXT_Markup_Status got = gtext_markup_append_child(parent, child);
      SCOPED_TRACE(std::string("parent kind ") + std::to_string(pk)
          + ", child kind " + std::to_string(ck));
      if (expected(pk, ck) == kOk) {
        EXPECT_EQ(got, GTEXT_MARKUP_OK);
      }
      else {
        EXPECT_EQ(got, GTEXT_MARKUP_E_HIERARCHY);
        EXPECT_EQ(dump(parent), before) << "a refused insertion changed the tree";
        EXPECT_EQ(gtext_markup_node_parent(child), nullptr);
      }
      if (pk == GTEXT_MARKUP_KIND_DOCUMENT && got == GTEXT_MARKUP_OK) {
        // Leave the document empty for the next combination.
        ASSERT_EQ(gtext_markup_remove(child), GTEXT_MARKUP_OK);
      }
      check();
    }
  }
}

TEST_F(MarkupTree, ADocumentIsNeverAChildAndNeverMoves) {
  Node * e = element("e");
  EXPECT_EQ(gtext_markup_append_child(e, root()), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_remove(root()), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_node_free(root()), GTEXT_MARKUP_E_STATE);
  EXPECT_EQ(gtext_markup_replace(root(), e), GTEXT_MARKUP_E_INVALID);
}

// ---- The document's own rules ----------------------------------------------

TEST_F(MarkupTree, ADocumentHoldsOneElement) {
  ASSERT_EQ(gtext_markup_append_child(root(), element("a")), GTEXT_MARKUP_OK);
  Node * b = element("b");
  EXPECT_EQ(gtext_markup_append_child(root(), b), GTEXT_MARKUP_E_HIERARCHY);
  EXPECT_EQ(gtext_markup_node_parent(b), nullptr);
  EXPECT_EQ(kids(root()), "a");
}

TEST_F(MarkupTree, ADocumentHoldsOneDoctypeAndItComesBeforeTheElement) {
  Node * a = element("a");
  Node * d1 = doctype("one");
  Node * d2 = doctype("two");
  ASSERT_EQ(gtext_markup_append_child(root(), a), GTEXT_MARKUP_OK);
  // After the element: refused.
  EXPECT_EQ(gtext_markup_append_child(root(), d1), GTEXT_MARKUP_E_HIERARCHY);
  // Before the element: accepted.
  ASSERT_EQ(gtext_markup_insert_before(root(), d1, a), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_document_doctype(doc), d1);
  // A second doctype: refused.
  EXPECT_EQ(gtext_markup_insert_before(root(), d2, d1), GTEXT_MARKUP_E_HIERARCHY);
  EXPECT_EQ(kids(root()), "# a");
}

TEST_F(MarkupTree, CommentsAndInstructionsMayStandAnywhereInADocument) {
  Node * a = element("a");
  Node * c1 = comment("before");
  Node * c2 = comment("after");
  Node * d = doctype();
  ASSERT_EQ(gtext_markup_append_child(root(), a), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_insert_before(root(), c1, a), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(root(), c2), GTEXT_MARKUP_OK);
  // And a doctype may still go before the element but after a comment.
  ASSERT_EQ(gtext_markup_insert_before(root(), d, a), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(root()), "# # a #");
}

TEST_F(MarkupTree, ReplacingTheOnlyElementWithAnotherSucceeds) {
  Node * a = element("a");
  Node * b = element("b");
  ASSERT_EQ(gtext_markup_append_child(root(), a), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_replace(a, b), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(root()), "b");
  EXPECT_EQ(gtext_markup_node_parent(a), nullptr);
  EXPECT_EQ(gtext_markup_document_element(doc), b);
}

TEST_F(MarkupTree, ReplacingAnElementWithATextNodeUnderADocumentIsRefused) {
  Node * a = element("a");
  ASSERT_EQ(gtext_markup_append_child(root(), a), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_replace(a, text("t")), GTEXT_MARKUP_E_HIERARCHY);
  EXPECT_EQ(kids(root()), "a");
}

TEST_F(MarkupTree, ReplacingADoctypeWithAnElementPutsTheElementInItsPlace) {
  Node * d = doctype();
  ASSERT_EQ(gtext_markup_append_child(root(), d), GTEXT_MARKUP_OK);
  Node * a = element("a");
  ASSERT_EQ(gtext_markup_replace(d, a), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(root()), "a");
}

// ---- Fragments -------------------------------------------------------------

TEST_F(MarkupTree, InsertingAFragmentInsertsItsChildrenInOrderAndEmptiesIt) {
  Node * parent = element("p");
  Node * f = fragment();
  for (const char * name : {"a", "b", "c"}) {
    ASSERT_EQ(gtext_markup_append_child(f, element(name)), GTEXT_MARKUP_OK);
  }
  Node * existing = element("x");
  ASSERT_EQ(gtext_markup_append_child(parent, existing), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_insert_before(parent, f, existing), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(parent), "a b c x");
  EXPECT_EQ(gtext_markup_node_first_child(f), nullptr);
  EXPECT_EQ(gtext_markup_node_parent(f), nullptr);
}

TEST_F(MarkupTree, AFragmentWithTwoElementsCannotGoIntoADocumentAndNothingMoves) {
  Node * f = fragment();
  ASSERT_EQ(gtext_markup_append_child(f, element("a")), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(f, element("b")), GTEXT_MARKUP_OK);
  const std::string before = dump(f);
  EXPECT_EQ(gtext_markup_append_child(root(), f), GTEXT_MARKUP_E_HIERARCHY);
  EXPECT_EQ(dump(f), before);
  EXPECT_EQ(gtext_markup_node_first_child(root()), nullptr);
}

TEST_F(MarkupTree, AFragmentWithTextCannotGoIntoADocument) {
  Node * f = fragment();
  ASSERT_EQ(gtext_markup_append_child(f, text("t")), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(root(), f), GTEXT_MARKUP_E_HIERARCHY);
}

TEST_F(MarkupTree, ADoctypeAndAnElementInAFragmentGoIntoADocumentTogether) {
  Node * f = fragment();
  Node * d = doctype();
  Node * a = element("a");
  // A fragment cannot itself hold a doctype, so this goes in the other way: a
  // document's children moved through a fragment.
  EXPECT_EQ(gtext_markup_append_child(f, d), GTEXT_MARKUP_E_HIERARCHY);
  ASSERT_EQ(gtext_markup_append_child(f, a), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(root(), f), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(root()), "a");
}

TEST_F(MarkupTree, AFragmentCannotBeInsertedIntoOneOfItsOwnChildren) {
  Node * f = fragment();
  Node * a = element("a");
  ASSERT_EQ(gtext_markup_append_child(f, a), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(a, f), GTEXT_MARKUP_E_HIERARCHY);
  EXPECT_EQ(gtext_markup_append_child(f, f), GTEXT_MARKUP_E_HIERARCHY);
}

// ---- No cycles ---------------------------------------------------------------

TEST_F(MarkupTree, ANodeCannotBeInsertedIntoItselfOrIntoADescendant) {
  Node * a = element("a");
  Node * b = element("b");
  Node * c = element("c");
  ASSERT_EQ(gtext_markup_append_child(a, b), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(b, c), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(a, a), GTEXT_MARKUP_E_HIERARCHY);
  EXPECT_EQ(gtext_markup_append_child(b, a), GTEXT_MARKUP_E_HIERARCHY);
  EXPECT_EQ(gtext_markup_append_child(c, a), GTEXT_MARKUP_E_HIERARCHY);
  EXPECT_EQ(gtext_markup_append_child(c, b), GTEXT_MARKUP_E_HIERARCHY);
  EXPECT_EQ(gtext_markup_replace(c, a), GTEXT_MARKUP_E_HIERARCHY);
  EXPECT_EQ(dump(a), "<a>\n  <b>\n    <c>\n");
}

TEST_F(MarkupTree, ANodeCannotBeInsertedIntoItsOwnTemplateContent) {
  Node * t = element("template", GTEXT_MARKUP_NS_HTML);
  Node * content = nullptr;
  ASSERT_EQ(gtext_markup_element_ensure_template_content(t, &content),
      GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(content, t), GTEXT_MARKUP_E_HIERARCHY);
}

// ---- Moving, removing, replacing -------------------------------------------

TEST_F(MarkupTree, InsertingANodeThatIsInTheTreeMovesIt) {
  Node * p = element("p");
  Node * q = element("q");
  Node * x = element("x");
  ASSERT_EQ(gtext_markup_append_child(p, x), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(q, x), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(p), "");
  EXPECT_EQ(kids(q), "x");
  EXPECT_EQ(gtext_markup_node_parent(x), q);
}

TEST_F(MarkupTree, MovingANodeWithinItsParentReordersIt) {
  Node * p = element("p");
  Node * a = element("a");
  Node * b = element("b");
  Node * c = element("c");
  for (Node * n : {a, b, c}) {
    ASSERT_EQ(gtext_markup_append_child(p, n), GTEXT_MARKUP_OK);
  }
  ASSERT_EQ(gtext_markup_insert_before(p, c, a), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(p), "c a b");
  ASSERT_EQ(gtext_markup_append_child(p, c), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(p), "a b c");
  // Before itself, and before its own next sibling, leave it where it is.
  ASSERT_EQ(gtext_markup_insert_before(p, b, b), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_insert_before(p, b, c), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(p), "a b c");
}

TEST_F(MarkupTree, ARemovedNodeStaysValidAndCanGoBack) {
  Node * p = element("p");
  Node * x = element("x");
  Node * y = element("y");
  ASSERT_EQ(gtext_markup_append_child(p, x), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(x, y), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_remove(x), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(p), "");
  EXPECT_EQ(gtext_markup_node_parent(x), nullptr);
  EXPECT_EQ(gtext_markup_node_first_child(x), y);
  EXPECT_EQ(gtext_markup_node_next_sibling(x), nullptr);
  EXPECT_EQ(gtext_markup_remove(x), GTEXT_MARKUP_OK);  // already detached
  ASSERT_EQ(gtext_markup_append_child(p, x), GTEXT_MARKUP_OK);
  EXPECT_EQ(dump(p), "<p>\n  <x>\n    <y>\n");
}

TEST_F(MarkupTree, SiblingLinksAreRightAfterInsertionsAndRemovals) {
  Node * p = element("p");
  Node * a = element("a");
  Node * b = element("b");
  Node * c = element("c");
  ASSERT_EQ(gtext_markup_append_child(p, a), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(p, c), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_insert_before(p, b, c), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_node_previous_sibling(b), a);
  EXPECT_EQ(gtext_markup_node_next_sibling(b), c);
  EXPECT_EQ(gtext_markup_node_last_child(p), c);
  EXPECT_EQ(gtext_markup_node_child_count(p), 3u);
  ASSERT_EQ(gtext_markup_remove(b), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_node_next_sibling(a), c);
  EXPECT_EQ(gtext_markup_node_previous_sibling(c), a);
  ASSERT_EQ(gtext_markup_remove(a), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_remove(c), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_node_first_child(p), nullptr);
  EXPECT_EQ(gtext_markup_node_last_child(p), nullptr);
}

TEST_F(MarkupTree, ADetachedNodeReportsNoSiblingsEvenThoughItSharesTheirStorage) {
  // Detached nodes chain through the same two fields as siblings do. Two of
  // them must not look like neighbours.
  Node * a = element("a");
  Node * b = element("b");
  EXPECT_EQ(gtext_markup_node_next_sibling(a), nullptr);
  EXPECT_EQ(gtext_markup_node_previous_sibling(a), nullptr);
  EXPECT_EQ(gtext_markup_node_next_sibling(b), nullptr);
  EXPECT_EQ(gtext_markup_node_previous_sibling(b), nullptr);
}

TEST_F(MarkupTree, ReplaceFollowsTheNewNodesAcrossParents) {
  Node * p = element("p");
  Node * q = element("q");
  Node * old_child = element("old");
  Node * mover = element("mover");
  ASSERT_EQ(gtext_markup_append_child(p, old_child), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(q, mover), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_replace(old_child, mover), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(p), "mover");
  EXPECT_EQ(kids(q), "");
  EXPECT_EQ(gtext_markup_node_parent(old_child), nullptr);
}

TEST_F(MarkupTree, ReplacingANodeWithItsOwnChildPromotesTheChild) {
  Node * p = element("p");
  Node * a = element("a");
  Node * b = element("b");
  ASSERT_EQ(gtext_markup_append_child(p, a), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(a, b), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_replace(a, b), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(p), "b");
  EXPECT_EQ(kids(a), "");
}

TEST_F(MarkupTree, ReplacingANodeWithItselfDoesNothing) {
  Node * p = element("p");
  Node * a = element("a");
  ASSERT_EQ(gtext_markup_append_child(p, a), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_replace(a, a), GTEXT_MARKUP_OK);
  EXPECT_EQ(kids(p), "a");
}

TEST_F(MarkupTree, ReplaceOfADetachedNodeIsRefused) {
  EXPECT_EQ(gtext_markup_replace(element("a"), element("b")), GTEXT_MARKUP_E_INVALID);
}

TEST_F(MarkupTree, InsertingANodeBeforeItselfWhenItIsNotAChildOfTheParentIsInvalidNotAnAppend) {
  // Found by the model test. "Insert x before x" leaves x where it is when x is
  // a child of the parent, and the library once applied that adjustment first
  // and validated after, so for an x that was *not* a child the reference
  // became "the end" and the call appended x - or failed with a different
  // status for a different reason - instead of refusing the reference.
  Node * p = element("p");
  Node * q = element("q");
  Node * x = element("x");
  ASSERT_EQ(gtext_markup_append_child(q, x), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_insert_before(p, x, x), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(kids(p), "");
  EXPECT_EQ(kids(q), "x");
  // Detached, too: its next link is the detached list's, not a sibling's.
  Node * y = element("y");
  EXPECT_EQ(gtext_markup_insert_before(p, y, y), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_node_parent(y), nullptr);
  // And a node that is its own parent and its own reference is the same mistake.
  EXPECT_EQ(gtext_markup_insert_before(y, y, y), GTEXT_MARKUP_E_INVALID);
}

TEST_F(MarkupTree, AReferenceThatIsNotAChildOfTheParentIsInvalid) {
  Node * p = element("p");
  Node * q = element("q");
  Node * x = element("x");
  ASSERT_EQ(gtext_markup_append_child(q, x), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_insert_before(p, element("y"), x), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_insert_before(nullptr, x, nullptr), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_insert_before(p, nullptr, nullptr), GTEXT_MARKUP_E_INVALID);
}

// ---- Ownership -------------------------------------------------------------

TEST_F(MarkupTree, ADocumentFreesNodesThatNeverWentIntoTheTree) {
  // The fixture checks nothing leaked.
  element("never-inserted");
  Node * a = element("a");
  ASSERT_EQ(gtext_markup_append_child(a, text("kid")), GTEXT_MARKUP_OK);
}

TEST_F(MarkupTree, FreeingADetachedNodeReleasesItsWholeSubtreeAndCountsIt) {
  Node * a = element("a");
  ASSERT_EQ(gtext_markup_append_child(a, element("b")), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(a, text("t")), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_document_node_count(doc), 4u);
  ASSERT_EQ(gtext_markup_node_free(a), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_document_node_count(doc), 1u);
}

TEST_F(MarkupTree, FreeingANodeThatIsStillInTheTreeIsRefused) {
  Node * a = element("a");
  ASSERT_EQ(gtext_markup_append_child(root(), a), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_node_free(a), GTEXT_MARKUP_E_STATE);
  EXPECT_EQ(gtext_markup_node_free(nullptr), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(kids(root()), "a");
}

TEST_F(MarkupTree, ANodeFromAnotherDocumentCannotBeInserted) {
  GTEXT_Markup_Document * other = new_doc(gtext_markup_options_default());
  Node * foreign = nullptr;
  ASSERT_EQ(gtext_markup_create_element(other, nullptr, nullptr, "f", &foreign),
      GTEXT_MARKUP_OK);
  Node * mine = element("m");
  EXPECT_EQ(gtext_markup_append_child(mine, foreign), GTEXT_MARKUP_E_WRONG_DOCUMENT);
  EXPECT_EQ(gtext_markup_append_child(foreign, mine), GTEXT_MARKUP_E_WRONG_DOCUMENT);
  gtext_markup_document_free(other);
}

// ---- Attributes ------------------------------------------------------------

TEST_F(MarkupTree, AnAttributeIsAddedFoundAndReadBackWithItsLength) {
  Node * e = element("e");
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "id", "x1", 2),
      GTEXT_MARKUP_OK);
  GTEXT_Markup_Attribute a;
  ASSERT_TRUE(gtext_markup_element_get_attribute(e, nullptr, "id", &a));
  EXPECT_STREQ(a.local, "id");
  EXPECT_EQ(std::string(a.value, a.value_length), "x1");
  EXPECT_EQ(a.value[a.value_length], '\0');
  EXPECT_EQ(a.ns, nullptr);
  EXPECT_EQ(a.prefix, nullptr);
  EXPECT_EQ(gtext_markup_element_attribute_count(e), 1u);
  EXPECT_FALSE(gtext_markup_element_get_attribute(e, nullptr, "class", &a));
}

TEST_F(MarkupTree, AttributesKeepTheOrderTheyWereAddedIn) {
  Node * e = element("e");
  for (const char * n : {"c", "a", "b"}) {
    ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, n, "1", 1),
        GTEXT_MARKUP_OK);
  }
  GTEXT_Markup_Attribute a;
  std::string order;
  for (size_t i = 0; i < 3; i++) {
    ASSERT_EQ(gtext_markup_element_attribute_at(e, i, &a), GTEXT_MARKUP_OK);
    order += a.local;
  }
  EXPECT_EQ(order, "cab");
  EXPECT_EQ(gtext_markup_element_attribute_at(e, 3, &a), GTEXT_MARKUP_E_INVALID);
}

TEST_F(MarkupTree, SettingAnAttributeAgainReplacesItInPlace) {
  Node * e = element("e");
  for (const char * n : {"a", "b", "c"}) {
    ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, n, "1", 1),
        GTEXT_MARKUP_OK);
  }
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, "p", "b", "two", 3),
      GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_element_attribute_count(e), 3u);
  GTEXT_Markup_Attribute a;
  ASSERT_EQ(gtext_markup_element_attribute_at(e, 1, &a), GTEXT_MARKUP_OK);
  EXPECT_STREQ(a.local, "b");
  EXPECT_STREQ(a.prefix, "p");  // the new spelling is taken
  EXPECT_EQ(std::string(a.value, a.value_length), "two");
}

TEST_F(MarkupTree, ReplacingAnAttributesPrefixReleasesTheOldPrefix) {
  // The fixture's invariant check counts every reference to every interned
  // name; a prefix that was replaced and not released shows up there as a
  // reference count one too high.
  Node * e = element("e");
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, "old", "a", "1", 1),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, "new", "a", "2", 1),
      GTEXT_MARKUP_OK);
  GTEXT_Markup_Attribute a;
  ASSERT_TRUE(gtext_markup_element_get_attribute(e, nullptr, "a", &a));
  EXPECT_STREQ(a.prefix, "new");
  // Back to no prefix at all.
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "a", "3", 1),
      GTEXT_MARKUP_OK);
  ASSERT_TRUE(gtext_markup_element_get_attribute(e, nullptr, "a", &a));
  EXPECT_EQ(a.prefix, nullptr);
  EXPECT_EQ(markup_check_invariants(doc), nullptr);
}

TEST_F(MarkupTree, CreationFailsCleanlyAtEveryAllocationAndLeavesNothingBehind) {
  // Each creation function, with its allocations failed one at a time. A
  // namespace, a prefix and a name are interned in turn, so a failure part way
  // has to release the ones already taken.
  struct Case {
    const char * name;
    std::function<GTEXT_Markup_Status()> run;
  };
  Node * out = nullptr;
  const Case cases[] = {
      {"element", [&] {
         return gtext_markup_create_element(doc, "urn:n", "p", "e", &out);
       }},
      {"text", [&] { return gtext_markup_create_text(doc, "text", 4, &out); }},
      {"cdata", [&] { return gtext_markup_create_cdata(doc, "cd", 2, &out); }},
      {"comment", [&] { return gtext_markup_create_comment(doc, "c", 1, &out); }},
      {"pi", [&] {
         return gtext_markup_create_processing_instruction(doc, "t", "d", 1, &out);
       }},
      {"doctype", [&] {
         return gtext_markup_create_doctype(doc, "html", "pub", "sys", &out);
       }},
      {"fragment", [&] { return gtext_markup_create_fragment(doc, &out); }},
  };
  for (const Case & c : cases) {
    // How many allocations a good run makes.
    fa.arm(0);
    ASSERT_EQ(c.run(), GTEXT_MARKUP_OK) << c.name;
    const size_t total = fa.calls;
    ASSERT_GE(total, 1u) << c.name;
    ASSERT_EQ(gtext_markup_node_free(out), GTEXT_MARKUP_OK);
    const size_t nodes = gtext_markup_document_node_count(doc);
    for (size_t n = 1; n <= total; n++) {
      out = reinterpret_cast<Node *>(0x1);
      fa.arm(n);
      const GTEXT_Markup_Status s = c.run();
      fa.disarm();
      if (s == GTEXT_MARKUP_OK) {
        // The name table's growth is best effort: a failure there is absorbed.
        ASSERT_EQ(gtext_markup_node_free(out), GTEXT_MARKUP_OK);
        continue;
      }
      EXPECT_EQ(s, GTEXT_MARKUP_E_OOM) << c.name << ", allocation " << n;
      EXPECT_EQ(out, reinterpret_cast<Node *>(0x1)) << c.name;
      EXPECT_EQ(gtext_markup_document_node_count(doc), nodes) << c.name;
      check();
    }
  }
}

TEST_F(MarkupTree, TheSameLocalNameInTwoNamespacesIsTwoAttributes) {
  Node * e = element("e");
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "href", "a", 1),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_element_set_attribute(
                e, GTEXT_MARKUP_NS_XLINK, "xlink", "href", "b", 1), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_element_attribute_count(e), 2u);
  GTEXT_Markup_Attribute a;
  ASSERT_TRUE(gtext_markup_element_get_attribute(e, nullptr, "href", &a));
  EXPECT_EQ(std::string(a.value, a.value_length), "a");
  ASSERT_TRUE(gtext_markup_element_get_attribute(e, GTEXT_MARKUP_NS_XLINK, "href", &a));
  EXPECT_EQ(std::string(a.value, a.value_length), "b");
  EXPECT_TRUE(gtext_markup_element_get_attribute(e, "", "href", nullptr));
}

TEST_F(MarkupTree, AnAttributeValueMayBeEmptyOrHoldANul) {
  Node * e = element("e");
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "a", nullptr, 0),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "b", "x\0y", 3),
      GTEXT_MARKUP_OK);
  GTEXT_Markup_Attribute a;
  ASSERT_TRUE(gtext_markup_element_get_attribute(e, nullptr, "a", &a));
  EXPECT_EQ(a.value_length, 0u);
  ASSERT_TRUE(gtext_markup_element_get_attribute(e, nullptr, "b", &a));
  EXPECT_EQ(std::string(a.value, a.value_length), std::string("x\0y", 3));
}

TEST_F(MarkupTree, RemovingAnAttributeKeepsTheOthersInOrder) {
  Node * e = element("e");
  for (const char * n : {"a", "b", "c"}) {
    ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, n, n, 1),
        GTEXT_MARKUP_OK);
  }
  EXPECT_TRUE(gtext_markup_element_remove_attribute(e, nullptr, "b"));
  EXPECT_FALSE(gtext_markup_element_remove_attribute(e, nullptr, "b"));
  EXPECT_FALSE(gtext_markup_element_remove_attribute(e, nullptr, "zzz"));
  EXPECT_EQ(dump(e), "<e>\n  @a=\"a\"\n  @c=\"c\"\n");
}

TEST_F(MarkupTree, AnAttributeNameIsLookedUpWithoutInterningIt) {
  Node * e = element("e");
  const size_t nodes = gtext_markup_document_node_count(doc);
  fa.arm(1);  // any allocation at all would be the failure
  GTEXT_Markup_Attribute a;
  EXPECT_FALSE(gtext_markup_element_get_attribute(e, "urn:never-seen", "never", &a));
  EXPECT_FALSE(gtext_markup_element_remove_attribute(e, nullptr, "never"));
  EXPECT_FALSE(fa.failed);
  fa.disarm();
  EXPECT_EQ(gtext_markup_document_node_count(doc), nodes);
}

TEST_F(MarkupTree, AttributeArgumentsAreChecked) {
  Node * e = element("e");
  EXPECT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "", "v", 1),
      GTEXT_MARKUP_E_NAME);
  EXPECT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, nullptr, "v", 1),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "a", nullptr, 3),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "a", "\xFF", 1),
      GTEXT_MARKUP_E_ENCODING);
  EXPECT_EQ(gtext_markup_element_set_attribute(
                text("t"), nullptr, nullptr, "a", "v", 1), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_element_attribute_count(e), 0u);
}

TEST_F(MarkupTree, AnAttributeLocationIsRememberedPerAttribute) {
  Node * e = element("e");
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "a", "1", 1),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "b", "2", 1),
      GTEXT_MARKUP_OK);
  EXPECT_FALSE(gtext_markup_element_attribute_location(e, 0, nullptr));
  const GTEXT_Markup_Location where = {40, 3, 9, 0, 0, 0, false};
  ASSERT_EQ(gtext_markup_element_set_attribute_location(e, 1, &where), GTEXT_MARKUP_OK);
  GTEXT_Markup_Location got = {};
  ASSERT_TRUE(gtext_markup_element_attribute_location(e, 1, &got));
  EXPECT_EQ(got.offset, 40u);
  EXPECT_EQ(got.line, 3u);
  EXPECT_EQ(got.column, 9u);
  EXPECT_FALSE(gtext_markup_element_attribute_location(e, 0, nullptr));
  ASSERT_EQ(gtext_markup_element_set_attribute_location(e, 1, nullptr), GTEXT_MARKUP_OK);
  EXPECT_FALSE(gtext_markup_element_attribute_location(e, 1, nullptr));
  EXPECT_EQ(gtext_markup_element_set_attribute_location(e, 2, &where),
      GTEXT_MARKUP_E_INVALID);
}

// ---- Locations -------------------------------------------------------------

TEST_F(MarkupTree, ANodeBuiltByHandHasNoLocationAndOneSetIsKept) {
  Node * e = element("e");
  EXPECT_FALSE(gtext_markup_node_location(e, nullptr));
  const GTEXT_Markup_Location where = {1234, 7, 22, 0, 0, 0, false};
  gtext_markup_node_set_location(e, &where);
  GTEXT_Markup_Location got = {};
  ASSERT_TRUE(gtext_markup_node_location(e, &got));
  EXPECT_EQ(got.offset, 1234u);
  EXPECT_EQ(got.line, 7u);
  EXPECT_EQ(got.column, 22u);
  gtext_markup_node_set_location(e, nullptr);
  EXPECT_FALSE(gtext_markup_node_location(e, nullptr));
}

TEST_F(MarkupTree, ALineBeyondThirtyTwoBitsSaturatesAndTheOffsetDoesNot) {
  Node * e = element("e");
  // (As wide as a size_t allows: with a 64-bit one the line is far past 32 bits and saturates; with a 32-bit one no line
  // can be, and the test says only that nothing changes. suite/tools/xarch/text.sh i686 runs that arm.)
  const GTEXT_Markup_Location where = {SIZE_MAX / 3, SIZE_MAX / 5, (size_t)5, 0, 0, 0, false};
  gtext_markup_node_set_location(e, &where);
  GTEXT_Markup_Location got = {};
  ASSERT_TRUE(gtext_markup_node_location(e, &got));
  EXPECT_EQ(got.offset, SIZE_MAX / 3);
  EXPECT_EQ(got.line, (size_t)std::min<unsigned long long>(SIZE_MAX / 5, UINT32_MAX));
  if (sizeof(size_t) > 4) {
    EXPECT_EQ(got.line, (size_t)UINT32_MAX);
  }
  EXPECT_EQ(got.column, 5u);
}

// ---- Walking ---------------------------------------------------------------

namespace {

// <a><b><c/></b><d/></a> plus a sibling <e/> beside a, under a fragment.
struct Walk {
  Node *top, *a, *b, *c, *d, *e;
};

}  // namespace

TEST_F(MarkupTree, NextInOrderIsAPreOrderWalkConfinedToItsRoot) {
  Walk w;
  w.top = fragment();
  w.a = element("a");
  w.b = element("b");
  w.c = element("c");
  w.d = element("d");
  w.e = element("e");
  ASSERT_EQ(gtext_markup_append_child(w.top, w.a), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(w.a, w.b), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(w.b, w.c), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(w.a, w.d), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(w.top, w.e), GTEXT_MARKUP_OK);

  std::string order;
  for (Node * n = w.a; n; n = gtext_markup_node_next_in_order(w.a, n, false)) {
    order += gtext_markup_element_local_name(n);
  }
  EXPECT_EQ(order, "abcd") << "confined to a's subtree: e is outside it";

  order.clear();
  for (Node * n = w.top; n; n = gtext_markup_node_next_in_order(w.top, n, false)) {
    const char * l = gtext_markup_element_local_name(n);
    order += l ? l : "#";
  }
  EXPECT_EQ(order, "#abcde");
}

TEST_F(MarkupTree, NextInOrderCanSkipTheChildrenOfTheNodeItIsAt) {
  Node * a = element("a");
  Node * b = element("b");
  Node * c = element("c");
  Node * d = element("d");
  ASSERT_EQ(gtext_markup_append_child(a, b), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(b, c), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(a, d), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_node_next_in_order(a, b, true), d);
  EXPECT_EQ(gtext_markup_node_next_in_order(a, b, false), c);
  EXPECT_EQ(gtext_markup_node_next_in_order(a, d, true), nullptr);
  EXPECT_EQ(gtext_markup_node_next_in_order(a, a, true), nullptr);
  EXPECT_EQ(gtext_markup_node_next_in_order(nullptr, a, false), nullptr);
}

TEST_F(MarkupTree, TextContentJoinsTextAndCdataAndSkipsComments) {
  Node * e = element("e");
  Node * inner = element("i");
  Node * cd = nullptr;
  ASSERT_EQ(gtext_markup_create_cdata(doc, "<c>", 3, &cd), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(e, text("a")), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(e, comment("skipped")), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(e, inner), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(inner, text("b")), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(inner, cd), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(e, text("z")), GTEXT_MARKUP_OK);

  char * out = nullptr;
  size_t length = 0;
  ASSERT_EQ(gtext_markup_node_text_content(e, nullptr, &out, &length),
      GTEXT_MARKUP_OK);
  EXPECT_EQ(std::string(out, length), "ab<c>z");
  EXPECT_EQ(out[length], '\0');
  gtext_markup_string_free(&fa.alloc, out);

  // And an element with no text gives an empty string, not NULL.
  ASSERT_EQ(gtext_markup_node_text_content(element("empty"), nullptr, &out, &length),
      GTEXT_MARKUP_OK);
  EXPECT_STREQ(out, "");
  gtext_markup_string_free(&fa.alloc, out);
}

TEST_F(MarkupTree, TextContentFailsCleanlyWhenItCannotAllocate) {
  Node * e = element("e");
  ASSERT_EQ(gtext_markup_append_child(e, text("abc")), GTEXT_MARKUP_OK);
  char * out = reinterpret_cast<char *>(0x1);
  fa.arm(1);
  EXPECT_EQ(gtext_markup_node_text_content(e, nullptr, &out, nullptr),
      GTEXT_MARKUP_E_OOM);
  fa.disarm();
  EXPECT_EQ(out, reinterpret_cast<char *>(0x1));
}

// ---- Template contents -----------------------------------------------------

TEST_F(MarkupTree, ATemplatesContentIsAFragmentThatIsNotItsChild) {
  Node * t = element("template", GTEXT_MARKUP_NS_HTML);
  EXPECT_EQ(gtext_markup_element_template_content(t), nullptr);
  Node * content = nullptr;
  ASSERT_EQ(gtext_markup_element_ensure_template_content(t, &content),
      GTEXT_MARKUP_OK);
  ASSERT_NE(content, nullptr);
  EXPECT_EQ(gtext_markup_element_template_content(t), content);
  EXPECT_EQ(gtext_markup_node_kind(content), GTEXT_MARKUP_KIND_FRAGMENT);
  EXPECT_EQ(gtext_markup_node_parent(content), nullptr);
  EXPECT_EQ(gtext_markup_node_first_child(t), nullptr);

  Node * again = nullptr;
  ASSERT_EQ(gtext_markup_element_ensure_template_content(t, &again), GTEXT_MARKUP_OK);
  EXPECT_EQ(again, content) << "ensuring twice must not make a second fragment";

  ASSERT_EQ(gtext_markup_append_child(content, element("inside")), GTEXT_MARKUP_OK);
  EXPECT_EQ(dump(t), "<{http://www.w3.org/1999/xhtml}template>\n"
                     "  #template-content\n"
                     "    <inside>\n");
}

TEST_F(MarkupTree, ATemplatesContentCannotBeInsertedRemovedOrFreedOnItsOwn) {
  Node * t = element("template");
  Node * content = nullptr;
  ASSERT_EQ(gtext_markup_element_ensure_template_content(t, &content),
      GTEXT_MARKUP_OK);
  Node * p = element("p");
  EXPECT_EQ(gtext_markup_append_child(p, content), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_remove(content), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_node_free(content), GTEXT_MARKUP_E_STATE);
  EXPECT_EQ(gtext_markup_insert_before(t, element("x"), content),
      GTEXT_MARKUP_E_INVALID);
}

TEST_F(MarkupTree, TemplateContentIsReleasedWithItsElementAndCounted) {
  Node * t = element("template");
  Node * content = nullptr;
  ASSERT_EQ(gtext_markup_element_ensure_template_content(t, &content),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(content, element("a")), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_document_node_count(doc), 4u);
  ASSERT_EQ(gtext_markup_node_free(t), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_document_node_count(doc), 1u);
}

TEST_F(MarkupTree, TemplatesNestedInsideTemplatesAreReleasedWithoutRecursion) {
  // Content inside content, ten thousand deep. Each level is reached through a
  // template's content, which is not a child, so this is the shape a walk that
  // only followed children would miss and a recursive one would blow the stack
  // on.
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_depth = 0;
  gtext_markup_document_free(doc);
  doc = new_doc(o);
  Node * outer = element("template");
  Node * cur = outer;
  for (int i = 0; i < 10000; i++) {
    Node * content = nullptr;
    ASSERT_EQ(gtext_markup_element_ensure_template_content(cur, &content),
        GTEXT_MARKUP_OK);
    Node * inner = element("template");
    ASSERT_EQ(gtext_markup_append_child(content, inner), GTEXT_MARKUP_OK);
    cur = inner;
  }
  EXPECT_EQ(gtext_markup_document_node_count(doc), 1u + 1 + 10000 * 2);
  ASSERT_EQ(gtext_markup_node_free(outer), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_document_node_count(doc), 1u);
}

// ---- Cloning and comparing -------------------------------------------------

namespace {

// A tree touching every kind, attribute and a template, built into `doc`.
Node * build_sample(GTEXT_Markup_Document * doc) {
  Node *e, *t, *c, *p, *cd, *inner, *content;
  gtext_markup_create_element(doc, GTEXT_MARKUP_NS_SVG, "s", "svg", &e);
  gtext_markup_element_set_attribute(e, nullptr, nullptr, "id", "root", 4);
  gtext_markup_element_set_attribute(
      e, GTEXT_MARKUP_NS_XLINK, "xlink", "href", "#a", 2);
  const GTEXT_Markup_Location where = {7, 2, 3, 0, 0, 0, false};
  gtext_markup_element_set_attribute_location(e, 0, &where);
  gtext_markup_node_set_location(e, &where);
  gtext_markup_create_text(doc, "hello", 5, &t);
  gtext_markup_create_comment(doc, " note ", 6, &c);
  gtext_markup_create_processing_instruction(doc, "pi", "data", 4, &p);
  gtext_markup_create_cdata(doc, "<raw>", 5, &cd);
  gtext_markup_create_element(doc, nullptr, nullptr, "template", &inner);
  gtext_markup_element_ensure_template_content(inner, &content);
  Node * leaf;
  gtext_markup_create_element(doc, nullptr, nullptr, "leaf", &leaf);
  gtext_markup_append_child(content, leaf);
  for (Node * n : {t, c, p, cd, inner}) {
    gtext_markup_append_child(e, n);
  }
  return e;
}

}  // namespace

TEST_F(MarkupTree, ADeepCloneIsEqualToTheOriginalAndSharesNothingWithIt) {
  Node * original = build_sample(doc);
  Node * copy = nullptr;
  ASSERT_EQ(gtext_markup_clone(doc, original, true, &copy), GTEXT_MARKUP_OK);
  EXPECT_NE(copy, original);
  EXPECT_EQ(gtext_markup_node_parent(copy), nullptr);
  EXPECT_TRUE(gtext_markup_equal(original, copy));
  EXPECT_EQ(dump(original), dump(copy));
  // Changing the copy does not touch the original.
  ASSERT_EQ(gtext_markup_element_set_attribute(copy, nullptr, nullptr, "id", "x", 1),
      GTEXT_MARKUP_OK);
  EXPECT_FALSE(gtext_markup_equal(original, copy));
  GTEXT_Markup_Attribute a;
  ASSERT_TRUE(gtext_markup_element_get_attribute(original, nullptr, "id", &a));
  EXPECT_EQ(std::string(a.value, a.value_length), "root");
}

TEST_F(MarkupTree, ADoctypeExternalSnapshotIsReadOnlyOwnedAndCloned) {
  Node * original = doctype("root");
  GTEXT_Markup_Location where = {12, 2, 4, 0, 0, 12, true};
  ASSERT_EQ(markup_doctype_append_syntax(original, GTEXT_MARKUP_DTD_ENTITY,
      "<!ENTITY internal 'i'>", 22, &where), GTEXT_MARKUP_OK);
  ASSERT_EQ(markup_doctype_append_external_syntax(original,
      GTEXT_MARKUP_DTD_ENTITY, "<!ENTITY external 'e'>", 22, &where),
      GTEXT_MARKUP_OK);

  EXPECT_EQ(gtext_markup_doctype_syntax_count(original), 1u);
  EXPECT_EQ(gtext_markup_doctype_external_syntax_count(original), 1u);
  GTEXT_Markup_DTD_Syntax got = {};
  ASSERT_TRUE(gtext_markup_doctype_external_syntax(original, 0, &got));
  EXPECT_EQ(got.kind, GTEXT_MARKUP_DTD_ENTITY);
  EXPECT_EQ(std::string(got.source, got.source_length), "<!ENTITY external 'e'>");
  EXPECT_EQ(got.location.offset, 12u);

  GTEXT_Markup_DTD_Syntax unchanged = {};
  unchanged.source = "sentinel";
  EXPECT_FALSE(gtext_markup_doctype_external_syntax(original, 1, &unchanged));
  EXPECT_STREQ(unchanged.source, "sentinel");
  EXPECT_FALSE(gtext_markup_doctype_external_syntax(original, 0, nullptr));

  Node * copy = nullptr;
  ASSERT_EQ(gtext_markup_clone(doc, original, true, &copy), GTEXT_MARKUP_OK);
  EXPECT_TRUE(gtext_markup_equal(original, copy));
  ASSERT_TRUE(gtext_markup_doctype_external_syntax(copy, 0, &got));
  EXPECT_EQ(std::string(got.source, got.source_length), "<!ENTITY external 'e'>");
  EXPECT_EQ(got.location.source_id, where.source_id);
  EXPECT_EQ(got.location.reference_source_id, where.reference_source_id);
  EXPECT_EQ(got.location.offset, where.offset);

  Node * different = doctype("root");
  ASSERT_EQ(markup_doctype_append_external_syntax(different,
      GTEXT_MARKUP_DTD_ENTITY, "<!ENTITY external 'other'>",
      sizeof("<!ENTITY external 'other'>") - 1, &where),
      GTEXT_MARKUP_OK);
  EXPECT_FALSE(gtext_markup_equal(original, different));
  EXPECT_STREQ(got.source, "<!ENTITY external 'e'>");
}

TEST_F(MarkupTree, DtdSyntaxLocationsMustReferenceDocumentSources) {
  GTEXT_Markup_Document * invalid_doc =
      new_doc(gtext_markup_options_default());
  Node * node = nullptr;
  ASSERT_EQ(gtext_markup_create_doctype(invalid_doc, "root", nullptr, nullptr,
      &node), GTEXT_MARKUP_OK);
  GTEXT_Markup_Location where = {0, 0, 0, 7, 0, 0, false};
  ASSERT_EQ(markup_doctype_append_external_syntax(node,
      GTEXT_MARKUP_DTD_ELEMENT, "<!ELEMENT root EMPTY>",
      sizeof("<!ELEMENT root EMPTY>") - 1, &where), GTEXT_MARKUP_OK);
  EXPECT_NE(markup_check_invariants(invalid_doc), nullptr);
  gtext_markup_document_free(invalid_doc);
}

TEST_F(MarkupTree, ACloneKeepsLocationsAndTheTemplateContent) {
  Node * original = build_sample(doc);
  Node * copy = nullptr;
  ASSERT_EQ(gtext_markup_clone(doc, original, true, &copy), GTEXT_MARKUP_OK);
  GTEXT_Markup_Location got = {};
  ASSERT_TRUE(gtext_markup_node_location(copy, &got));
  EXPECT_EQ(got.line, 2u);
  ASSERT_TRUE(gtext_markup_element_attribute_location(copy, 0, &got));
  EXPECT_EQ(got.offset, 7u);
  Node * tmpl = gtext_markup_node_last_child(copy);
  ASSERT_NE(gtext_markup_element_template_content(tmpl), nullptr);
  EXPECT_NE(gtext_markup_element_template_content(tmpl),
      gtext_markup_element_template_content(gtext_markup_node_last_child(original)));
}

TEST_F(MarkupTree, AShallowCloneHasTheAttributesAndNoChildren) {
  Node * original = build_sample(doc);
  Node * copy = nullptr;
  ASSERT_EQ(gtext_markup_clone(doc, original, false, &copy), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_node_first_child(copy), nullptr);
  EXPECT_EQ(gtext_markup_element_attribute_count(copy), 2u);
  EXPECT_FALSE(gtext_markup_equal(original, copy));
}

TEST_F(MarkupTree, AShallowCloneOfATemplateDoesNotCopyItsContent) {
  Node * t = element("template");
  Node * content = nullptr;
  ASSERT_EQ(gtext_markup_element_ensure_template_content(t, &content),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(content, element("x")), GTEXT_MARKUP_OK);
  Node * copy = nullptr;
  ASSERT_EQ(gtext_markup_clone(doc, t, false, &copy), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_element_template_content(copy), nullptr);
}

TEST_F(MarkupTree, FailedMixedTextCloneDoesNotLeaveDetachedNodeBehind) {
  Node * original = text("source");
  const GTEXT_Markup_Location where{};
  ASSERT_EQ(markup_text_add_span(doc, original, 0, 6, &where),
      GTEXT_MARKUP_OK);
  const size_t nodes_before = gtext_markup_document_node_count(doc);
  Node * copy = reinterpret_cast<Node *>(0x1);

  // Text creation takes two allocations; span copying is the third.
  fa.arm(3);
  const GTEXT_Markup_Status status = gtext_markup_clone(doc, original, false,
      &copy);
  fa.disarm();

  EXPECT_EQ(status, GTEXT_MARKUP_E_OOM);
  EXPECT_EQ(copy, reinterpret_cast<Node *>(0x1));
  EXPECT_TRUE(fa.failed);
  EXPECT_EQ(gtext_markup_document_node_count(doc), nodes_before);
  EXPECT_EQ(markup_check_invariants(doc), nullptr);
}

TEST_F(MarkupTree, CloningIsHowANodeCrossesDocuments) {
  Node * original = build_sample(doc);
  GTEXT_Markup_Document * other = new_doc(gtext_markup_options_default());
  Node * copy = nullptr;
  ASSERT_EQ(gtext_markup_clone(other, original, true, &copy), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_node_document(copy), other);
  EXPECT_TRUE(gtext_markup_equal(original, copy));
  // The namespace string the copy reports is the other document's own.
  EXPECT_STREQ(gtext_markup_element_namespace(copy), GTEXT_MARKUP_NS_SVG);
  EXPECT_EQ(markup_check_invariants(other), nullptr);
  gtext_markup_document_free(other);
}

TEST_F(MarkupTree, ADocumentCannotBeCloned) {
  Node * copy = nullptr;
  EXPECT_EQ(gtext_markup_clone(doc, root(), true, &copy), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_clone(doc, nullptr, true, &copy), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_clone(nullptr, element("a"), true, &copy),
      GTEXT_MARKUP_E_INVALID);
}

TEST_F(MarkupTree, ACloneStopsAtTheDocumentsNodeLimitAndLeavesNothingBehind) {
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_nodes = 7;
  GTEXT_Markup_Document * small = new_doc(o);
  Node * copy = reinterpret_cast<Node *>(0x1);
  Node * original = build_sample(doc);  // far more than seven nodes
  EXPECT_EQ(gtext_markup_clone(small, original, true, &copy), GTEXT_MARKUP_E_LIMIT);
  EXPECT_EQ(copy, reinterpret_cast<Node *>(0x1));
  EXPECT_EQ(gtext_markup_document_node_count(small), 1u);
  EXPECT_EQ(markup_check_invariants(small), nullptr);
  gtext_markup_document_free(small);
}

TEST_F(MarkupTree, EqualityIgnoresAttributeOrderButNotPrefixesOrValues) {
  auto with = [&](std::initializer_list<std::pair<const char *, const char *>> attrs,
                 const char * prefix) {
    Node * e = element("e", nullptr, prefix);
    for (auto & kv : attrs) {
      EXPECT_EQ(gtext_markup_element_set_attribute(
                    e, nullptr, nullptr, kv.first, kv.second, strlen(kv.second)),
          GTEXT_MARKUP_OK);
    }
    return e;
  };
  Node * ab = with({{"a", "1"}, {"b", "2"}}, nullptr);
  Node * ba = with({{"b", "2"}, {"a", "1"}}, nullptr);
  Node * other_value = with({{"a", "1"}, {"b", "3"}}, nullptr);
  Node * fewer = with({{"a", "1"}}, nullptr);
  Node * prefixed = with({{"a", "1"}, {"b", "2"}}, "p");
  EXPECT_TRUE(gtext_markup_equal(ab, ba));
  EXPECT_FALSE(gtext_markup_equal(ab, other_value));
  EXPECT_FALSE(gtext_markup_equal(ab, fewer));
  EXPECT_FALSE(gtext_markup_equal(fewer, ab));
  EXPECT_FALSE(gtext_markup_equal(ab, prefixed));
}

TEST_F(MarkupTree, EqualityDistinguishesTemplateContentFromChildren) {
  Node * a = element("template");
  Node * b = element("template");
  Node * content = nullptr;
  ASSERT_EQ(gtext_markup_element_ensure_template_content(a, &content),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(content, element("x")), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(b, element("x")), GTEXT_MARKUP_OK);
  EXPECT_FALSE(gtext_markup_equal(a, b));
  EXPECT_FALSE(gtext_markup_equal(b, a));
}

TEST_F(MarkupTree, EqualityDistinguishesKindsAndData) {
  EXPECT_TRUE(gtext_markup_equal(nullptr, nullptr));
  EXPECT_FALSE(gtext_markup_equal(nullptr, text("a")));
  EXPECT_TRUE(gtext_markup_equal(text("a"), text("a")));
  EXPECT_FALSE(gtext_markup_equal(text("a"), text("b")));
  EXPECT_FALSE(gtext_markup_equal(text("a"), comment("a")));
  EXPECT_FALSE(gtext_markup_equal(text("a"), text("ab")));
}

// ---- Dump ------------------------------------------------------------------

TEST_F(MarkupTree, TheDumpOfASampleIsExactlyThis) {
  Node * e = build_sample(doc);
  EXPECT_EQ(dump(e),
      "<{http://www.w3.org/2000/svg}s:svg>\n"
      "  @id=\"root\"\n"
      "  @{http://www.w3.org/1999/xlink}xlink:href=\"#a\"\n"
      "  \"hello\"\n"
      "  <!--\" note \"-->\n"
      "  <?pi \"data\"?>\n"
      "  <![CDATA[\"<raw>\"]]>\n"
      "  <template>\n"
      "    #template-content\n"
      "      <leaf>\n");
}

TEST_F(MarkupTree, TheDumpEscapesWhatWouldMakeItAmbiguous) {
  Node * t = nullptr;
  ASSERT_EQ(gtext_markup_create_text(doc, "a\"b\\c\nd\te\rf\x01g\x7f", 14, &t),
      GTEXT_MARKUP_OK);
  EXPECT_EQ(dump(t), "\"a\\\"b\\\\c\\nd\\te\\rf\\x01g\\x7F\"\n");
}

TEST_F(MarkupTree, TheDumpOfADocumentShowsItsDoctypeWithAbsentIdentifiersAsDashes) {
  Node * d = nullptr;
  ASSERT_EQ(gtext_markup_create_doctype(doc, "html", "-//W3C//DTD", nullptr, &d),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(root(), d), GTEXT_MARKUP_OK);
  EXPECT_EQ(dump(root()), "#document\n  <!DOCTYPE \"html\" \"-//W3C//DTD\" ->\n");
}

// ---- Limits ----------------------------------------------------------------

TEST_F(MarkupTree, MaxDepthStopsAnInsertionExactlyAtTheLimit) {
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_depth = 3;
  gtext_markup_document_free(doc);
  doc = new_doc(o);
  // The document is depth 0, so the limit allows root > a > b > c.
  Node * a = element("a");
  Node * b = element("b");
  Node * c = element("c");
  Node * d = element("d");
  ASSERT_EQ(gtext_markup_append_child(root(), a), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(a, b), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(b, c), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(c, d), GTEXT_MARKUP_E_LIMIT);
  EXPECT_EQ(gtext_markup_node_parent(d), nullptr);
}

TEST_F(MarkupTree, MaxDepthCountsTheDescendantsOfASubtreeBeingMoved) {
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_depth = 3;
  gtext_markup_document_free(doc);
  doc = new_doc(o);
  Node * a = element("a");
  Node * b = element("b");
  Node * c = element("c");
  ASSERT_EQ(gtext_markup_append_child(b, c), GTEXT_MARKUP_OK);  // b > c: height 1
  ASSERT_EQ(gtext_markup_append_child(root(), a), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(a, b), GTEXT_MARKUP_OK);  // root>a>b>c = 3
  Node * x = element("x");
  ASSERT_EQ(gtext_markup_append_child(root(), x), GTEXT_MARKUP_E_HIERARCHY)
      << "(the document already has an element)";
  // Deeper: b with c under it, placed under c's depth, would be depth 5.
  Node * deep = element("deep");
  ASSERT_EQ(gtext_markup_append_child(c, deep), GTEXT_MARKUP_E_LIMIT);
  Node * sub = element("sub");
  Node * subkid = element("subkid");
  ASSERT_EQ(gtext_markup_append_child(sub, subkid), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(b, sub), GTEXT_MARKUP_E_LIMIT)
      << "sub has a child, so putting it under b (depth 2) reaches depth 4";
  EXPECT_EQ(gtext_markup_append_child(a, sub), GTEXT_MARKUP_OK)
      << "under a (depth 1) it reaches 3";
}

TEST_F(MarkupTree, MaxDepthCountsAFragmentsChildrenAtTheDepthTheyLandAt) {
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_depth = 2;
  gtext_markup_document_free(doc);
  doc = new_doc(o);
  Node * p = element("p");
  ASSERT_EQ(gtext_markup_append_child(root(), p), GTEXT_MARKUP_OK);  // depth 1
  Node * f = fragment();
  Node * kid = element("kid");
  Node * grandkid = element("grandkid");
  ASSERT_EQ(gtext_markup_append_child(f, kid), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(p, f), GTEXT_MARKUP_OK)
      << "kid lands at depth 2, which is the limit";
  Node * g = fragment();
  ASSERT_EQ(gtext_markup_append_child(g, grandkid), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(kid, g), GTEXT_MARKUP_E_LIMIT)
      << "grandkid would land at depth 3";
}

TEST_F(MarkupTree, MaxDepthZeroMeansNoLimit) {
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_depth = 0;
  gtext_markup_document_free(doc);
  doc = new_doc(o);
  Node * cur = root();
  for (int i = 0; i < 2000; i++) {
    Node * e = element("e");
    ASSERT_EQ(gtext_markup_append_child(cur, e), GTEXT_MARKUP_OK);
    cur = e;
    if (i == 0) {
      // Only one element may stand directly under the document, so the chain
      // continues beneath it.
    }
  }
}

TEST_F(MarkupTree, MaxNodesStopsCreationAndAFailedCreationCostsNothing) {
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_nodes = 3;
  gtext_markup_document_free(doc);
  doc = new_doc(o);  // the root is one
  Node * n = nullptr;
  ASSERT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "a", &n), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "b", &n), GTEXT_MARKUP_OK);
  Node * sentinel = reinterpret_cast<Node *>(0x1);
  n = sentinel;
  EXPECT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "c", &n),
      GTEXT_MARKUP_E_LIMIT);
  EXPECT_EQ(gtext_markup_create_text(doc, "t", 1, &n), GTEXT_MARKUP_E_LIMIT);
  EXPECT_EQ(gtext_markup_create_fragment(doc, &n), GTEXT_MARKUP_E_LIMIT);
  EXPECT_EQ(n, sentinel);
  EXPECT_EQ(gtext_markup_document_node_count(doc), 3u);
}

TEST_F(MarkupTree, FreeingANodeMakesRoomUnderTheNodeLimit) {
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_nodes = 2;
  gtext_markup_document_free(doc);
  doc = new_doc(o);
  Node * a = nullptr;
  ASSERT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "a", &a), GTEXT_MARKUP_OK);
  Node * b = nullptr;
  EXPECT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "b", &b),
      GTEXT_MARKUP_E_LIMIT);
  ASSERT_EQ(gtext_markup_node_free(a), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "b", &b), GTEXT_MARKUP_OK);
}

TEST_F(MarkupTree, MaxAttributesStopsAnElementFillingUpAndReplacingStillWorks) {
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_attributes = 2;
  gtext_markup_document_free(doc);
  doc = new_doc(o);
  Node * e = element("e");
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "a", "1", 1),
      GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "b", "1", 1),
      GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "c", "1", 1),
      GTEXT_MARKUP_E_LIMIT);
  EXPECT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "a", "2", 1),
      GTEXT_MARKUP_OK) << "replacing adds no attribute";
  EXPECT_EQ(gtext_markup_element_attribute_count(e), 2u);
}

// ---- Depth: nothing recurses -----------------------------------------------

TEST_F(MarkupTree, ATreeAHundredThousandDeepIsClonedComparedDumpedAndFreed) {
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_depth = 0;
  gtext_markup_document_free(doc);
  doc = new_doc(o);
  Node * top = element("e");
  Node * cur = top;
  for (int i = 0; i < 100000; i++) {
    Node * e = element("e");
    ASSERT_EQ(gtext_markup_append_child(cur, e), GTEXT_MARKUP_OK);
    cur = e;
  }
  Node * copy = nullptr;
  ASSERT_EQ(gtext_markup_clone(doc, top, true, &copy), GTEXT_MARKUP_OK);
  EXPECT_TRUE(gtext_markup_equal(top, copy));
  // Not dumped at this depth: a dump indents each line by its depth, so its
  // output is quadratic in the depth by nature (a hundred thousand levels is
  // ten billion spaces), which is a property of the format and not of the walk.
  // The walk is exercised at a depth where the output is affordable.
  Node * shallow_top = element("e");
  Node * cur2 = shallow_top;
  for (int i = 0; i < 3000; i++) {
    Node * e = element("e");
    ASSERT_EQ(gtext_markup_append_child(cur2, e), GTEXT_MARKUP_OK);
    cur2 = e;
  }
  FILE * sink = fopen("/dev/null", "w");
  ASSERT_NE(sink, nullptr);
  gtext_markup_dump(shallow_top, sink);
  fclose(sink);
  char * out = nullptr;
  ASSERT_EQ(gtext_markup_node_text_content(top, nullptr, &out, nullptr),
      GTEXT_MARKUP_OK);
  gtext_markup_string_free(&fa.alloc, out);
  ASSERT_EQ(gtext_markup_node_free(top), GTEXT_MARKUP_OK);
  // `copy` is freed by the document, which has to cope with it too.
}

TEST_F(MarkupTree, MovingASubtreeUnderMaxDepthZeroCostsNoWalk) {
  // Not a timing test: with no limit set the height of what is inserted is
  // never computed, which is what keeps a parser's reparenting linear.
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_depth = 0;
  gtext_markup_document_free(doc);
  doc = new_doc(o);
  Node * a = element("a");
  Node * b = element("b");
  Node * cur = a;
  for (int i = 0; i < 1000; i++) {
    Node * e = element("e");
    ASSERT_EQ(gtext_markup_append_child(cur, e), GTEXT_MARKUP_OK);
    cur = e;
  }
  for (int i = 0; i < 100; i++) {
    ASSERT_EQ(gtext_markup_append_child(b, a), GTEXT_MARKUP_OK);
    ASSERT_EQ(gtext_markup_remove(a), GTEXT_MARKUP_OK);
  }
}

// ---- Arguments the header promises are checked ------------------------------

TEST_F(MarkupTree, ANullDocumentIsRefusedOrAnswersNullByEachFunctionsOwnContract) {
  EXPECT_EQ(gtext_markup_document_element(nullptr), nullptr);
  EXPECT_EQ(gtext_markup_document_doctype(nullptr), nullptr);
  EXPECT_EQ(gtext_markup_document_root(nullptr), nullptr);
  EXPECT_EQ(gtext_markup_document_node_count(nullptr), 0u);
  EXPECT_EQ(gtext_markup_document_set_encoding(nullptr, "utf-8",
                GTEXT_MARKUP_ENCODING_CALLER), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_document_encoding(nullptr, nullptr), nullptr);
  EXPECT_EQ(gtext_markup_document_set_xml_declaration(nullptr, "1.0",
                GTEXT_MARKUP_STANDALONE_NO), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_document_xml_version(nullptr, nullptr), nullptr);
  Node * n = nullptr;
  EXPECT_EQ(gtext_markup_create_fragment(nullptr, &n), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_create_fragment(doc, nullptr), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_create_processing_instruction(doc, nullptr, "d", 1, &n),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_create_processing_instruction(doc, "t", nullptr, 3, &n),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_create_processing_instruction(doc, "t", "d", 1, nullptr),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_create_processing_instruction(doc, "\xFF", "d", 1, &n),
      GTEXT_MARKUP_E_ENCODING);
  EXPECT_EQ(gtext_markup_create_doctype(doc, "html", "\xFF", nullptr, &n),
      GTEXT_MARKUP_E_ENCODING);
  EXPECT_EQ(gtext_markup_create_doctype(doc, "html", nullptr, "\xFF", &n),
      GTEXT_MARKUP_E_ENCODING);
  EXPECT_EQ(gtext_markup_document_node_count(doc), 1u) << "nothing was made";
}

TEST_F(MarkupTree, ANullOrWrongKindNodeIsRefusedOrAnswersNullByEachFunctionsOwnContract) {
  Node * t = text("t");
  Node * e = element("e");
  char * out = reinterpret_cast<char *>(0x1);
  EXPECT_EQ(gtext_markup_node_data(nullptr, nullptr), nullptr);
  EXPECT_EQ(gtext_markup_node_set_data(nullptr, "x", 1), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_node_set_data(t, nullptr, 3), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_node_text_content(nullptr, nullptr, &out, nullptr),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_node_text_content(e, nullptr, nullptr, nullptr),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(out, reinterpret_cast<char *>(0x1));
  GTEXT_Markup_Attribute a;
  EXPECT_FALSE(gtext_markup_element_get_attribute(e, nullptr, nullptr, &a));
  EXPECT_FALSE(gtext_markup_element_get_attribute(t, nullptr, "id", &a));
  EXPECT_FALSE(gtext_markup_element_get_attribute(nullptr, nullptr, "id", &a));
  EXPECT_FALSE(gtext_markup_element_remove_attribute(t, nullptr, "id"));
  EXPECT_FALSE(gtext_markup_element_remove_attribute(nullptr, nullptr, "id"));
  EXPECT_EQ(gtext_markup_element_attribute_at(t, 0, &a), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_element_attribute_at(e, 0, nullptr), GTEXT_MARKUP_E_INVALID);
  EXPECT_FALSE(gtext_markup_element_attribute_location(t, 0, nullptr));
  EXPECT_FALSE(gtext_markup_node_location(nullptr, nullptr));
  EXPECT_EQ(gtext_markup_element_template_content(t), nullptr);
  EXPECT_EQ(gtext_markup_element_ensure_template_content(t, nullptr),
      GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_node_next_sibling(nullptr), nullptr);
  EXPECT_EQ(gtext_markup_node_previous_sibling(nullptr), nullptr);
  EXPECT_EQ(gtext_markup_node_last_child(nullptr), nullptr);
}

TEST_F(MarkupTree, DumpingNothingWritesNothing) {
  gtext_markup_dump(nullptr, stdout);
  gtext_markup_dump(element("e"), nullptr);
}

TEST_F(MarkupTree, ANextInOrderWalkLeavesNullWhenTheNodeIsNotInsideTheRoot) {
  // The walk is confined to `root`'s subtree. Started at a node that is not in
  // it, it must not wander off through the node's own ancestors or the
  // detached list; it ends.
  Node * a = element("a");
  Node * b = element("b");
  Node * c = element("c");
  ASSERT_EQ(gtext_markup_append_child(a, b), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_node_next_in_order(a, c, false), nullptr);
  EXPECT_EQ(gtext_markup_node_next_in_order(b, c, true), nullptr);
}

// ---- Statuses --------------------------------------------------------------

TEST(MarkupStatus, EveryStatusHasADistinctNonEmptyString) {
  const GTEXT_Markup_Status all[] = {
      GTEXT_MARKUP_OK, GTEXT_MARKUP_E_INVALID, GTEXT_MARKUP_E_OOM,
      GTEXT_MARKUP_E_LIMIT, GTEXT_MARKUP_E_HIERARCHY,
      GTEXT_MARKUP_E_WRONG_DOCUMENT, GTEXT_MARKUP_E_STATE, GTEXT_MARKUP_E_NAME,
      GTEXT_MARKUP_E_ENCODING, GTEXT_MARKUP_E_UNREPRESENTABLE,
      GTEXT_MARKUP_E_WRITE};
  std::vector<std::string> seen;
  for (GTEXT_Markup_Status s : all) {
    const std::string text = gtext_markup_status_string(s);
    EXPECT_FALSE(text.empty());
    EXPECT_NE(text, "unknown status");
    for (const std::string & other : seen) {
      EXPECT_NE(text, other);
    }
    seen.push_back(text);
  }
  EXPECT_STREQ(gtext_markup_status_string((GTEXT_Markup_Status)999), "unknown status");
}

// ---- What the markup tests do not cover, and why --------------------------
//
// With these four files (tree, model, escape, invariants) src/markup is covered
// to the line except the following, each of which was read and is a guard, not
// a promise:
//
//   markup_support.c   markup_copy(): len == SIZE_MAX          } arithmetic that
//                      markup_atom_intern(): name length       } would overflow
//                      grow_table(): doubling past SIZE_MAX    } size_t, for a
//                      markup_buf_append(): length + len       } string of
//                      markup_buf_append(): doubling           } ~2^63 bytes
//                      markup_check_utf8(): NULL with len > 0  } every caller has
//                                                              } already refused
//   markup_tree.c      set_attribute(): attribute-array growth overflow
//
// None can be reached through the public API without an allocation of 2^63
// bytes, which no allocator here would grant and no test could construct, and
// they stay because the alternative is wrapping silently.
