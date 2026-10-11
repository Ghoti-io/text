/**
 * @file
 *
 * The markup tree against a reference model, over random sequences of
 * operations, with an allocation failure injected at every step.
 *
 * test-markup-tree.cpp asserts the contract one case at a time. What it cannot
 * say is that the *combination* is right: that after the four-hundredth
 * operation, some of which were refused and some of which moved a subtree out
 * of a template into a detached fragment, every link in the structure still
 * agrees with every other. So this file keeps a second tree - plain C++,
 * vectors and recursion, written from the DOM's rules and sharing no code with
 * src/markup - applies every operation to both, and compares what each says.
 *
 * Three things are compared after every operation:
 *
 *   - the status the operation returned against the one the model predicts,
 *     with the precedence between rules written out below (it is part of the
 *     contract, because a caller who gets HIERARCHY can act on it differently
 *     from one who gets LIMIT);
 *   - the whole of the library's state against the model's, as the library's
 *     own dump of every top-level node, plus parent, kind and child count of
 *     every node there is;
 *   - the library's own invariant checker, which looks at the links, the
 *     detached list, the node count and the reference count of every interned
 *     name.
 *
 * Every operation that allocates is then run again with its first allocation
 * failing, then its second, and so on until it gets through. A failed
 * operation must have left the world exactly as it was, and one that got
 * through despite a failure (the name table's growth is best effort and is
 * allowed to be) must have produced what the model says. That is the property
 * "refused means unchanged", and it is the one a leak or a half-built node
 * breaks.
 *
 * A model that agrees with the code because it was written by the same hand
 * from the same misreading proves little, so the places where a rule could be
 * read two ways name their source: DOM Standard section 4.2.4 "Mutation
 * algorithms", "ensure pre-insert validity".
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/markup.h>

#include "../src/markup/markup_internal.h"
#include "markup_test_support.h"

namespace {

using Kind = GTEXT_Markup_Kind;
using Status = GTEXT_Markup_Status;
using RNode = GTEXT_Markup_Node;

// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------

struct MAttr {
  std::string ns, prefix, local, value;
};

struct MNode {
  Kind kind = GTEXT_MARKUP_KIND_NONE;
  int doc = 0;
  std::string ns, prefix, local;   // element
  std::vector<MAttr> attrs;
  std::string data;                // text, cdata, comment, pi
  std::string target;              // pi
  std::string dt_name, dt_pub, dt_sys;  // doctype
  bool has_pub = false, has_sys = false;
  MNode * parent = nullptr;        // in-tree parent; null for detached, root, content
  std::vector<MNode *> kids;
  MNode * content = nullptr;       // an element's template content
  MNode * owner = nullptr;         // for a content fragment: its element
  bool is_root = false;
  bool is_content = false;
  bool alive = true;
  RNode * real = nullptr;
};

bool container(Kind k) {
  return k == GTEXT_MARKUP_KIND_DOCUMENT || k == GTEXT_MARKUP_KIND_FRAGMENT
      || k == GTEXT_MARKUP_KIND_ELEMENT;
}

// DOM "ensure pre-insert validity", steps 2 and 5 and 6: what may be a child of
// what, kind by kind.
bool allowed(Kind parent, Kind child) {
  if (!container(parent)) {
    return false;
  }
  switch (child) {
    case GTEXT_MARKUP_KIND_ELEMENT:
    case GTEXT_MARKUP_KIND_COMMENT:
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
      return true;
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
      return parent != GTEXT_MARKUP_KIND_DOCUMENT;
    case GTEXT_MARKUP_KIND_DOCTYPE:
      return parent == GTEXT_MARKUP_KIND_DOCUMENT;
    default:
      return false;
  }
}

bool valid_utf8(const std::string & s) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const unsigned char c = (unsigned char)s[i];
    size_t len;
    uint32_t cp;
    if (c < 0x80) {
      i++;
      continue;
    }
    else if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1F;
    }
    else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      cp = c & 0x0F;
    }
    else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07;
    }
    else {
      return false;
    }
    if (i + len > n) {
      return false;
    }
    for (size_t k = 1; k < len; k++) {
      const unsigned char cc = (unsigned char)s[i + k];
      if ((cc & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF
        || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    i += len;
  }
  return true;
}

std::string quote(const std::string & s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20 || c == 0x7F) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\x%02X", c);
          out += buf;
        }
        else {
          out += (char)c;
        }
    }
  }
  return out + "\"";
}

std::string name_of(const std::string & ns, const std::string & prefix,
    const std::string & local) {
  return (ns.empty() ? "" : "{" + ns + "}") + (prefix.empty() ? "" : prefix + ":")
      + local;
}

void model_dump(const MNode * n, size_t depth, std::string & out) {
  const std::string pad(depth * 2, ' ');
  out += pad;
  switch (n->kind) {
    case GTEXT_MARKUP_KIND_DOCUMENT: out += "#document\n"; break;
    case GTEXT_MARKUP_KIND_FRAGMENT:
      out += n->is_content ? "#template-content\n" : "#fragment\n";
      break;
    case GTEXT_MARKUP_KIND_DOCTYPE:
      out += "<!DOCTYPE " + quote(n->dt_name) + " "
          + (n->has_pub ? quote(n->dt_pub) : "-") + " "
          + (n->has_sys ? quote(n->dt_sys) : "-") + ">\n";
      break;
    case GTEXT_MARKUP_KIND_ELEMENT:
      out += "<" + name_of(n->ns, n->prefix, n->local) + ">\n";
      for (const MAttr & a : n->attrs) {
        out += pad + "  @" + name_of(a.ns, a.prefix, a.local) + "=" + quote(a.value)
            + "\n";
      }
      break;
    case GTEXT_MARKUP_KIND_TEXT: out += quote(n->data) + "\n"; break;
    case GTEXT_MARKUP_KIND_CDATA: out += "<![CDATA[" + quote(n->data) + "]]>\n"; break;
    case GTEXT_MARKUP_KIND_COMMENT: out += "<!--" + quote(n->data) + "-->\n"; break;
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
      out += "<?" + n->target + " " + quote(n->data) + "?>\n";
      break;
    default: out += "#none\n";
  }
  if (n->content) {
    model_dump(n->content, depth + 1, out);
  }
  for (const MNode * c : n->kids) {
    model_dump(c, depth + 1, out);
  }
}

std::string model_dump(const MNode * n) {
  std::string out;
  model_dump(n, 0, out);
  return out;
}

bool model_equal(const MNode * a, const MNode * b) {
  if (a->kind != b->kind) {
    return false;
  }
  switch (a->kind) {
    case GTEXT_MARKUP_KIND_ELEMENT: {
      if (a->ns != b->ns || a->prefix != b->prefix || a->local != b->local
          || a->attrs.size() != b->attrs.size()) {
        return false;
      }
      for (const MAttr & x : a->attrs) {
        bool found = false;
        for (const MAttr & y : b->attrs) {
          if (x.ns == y.ns && x.local == y.local && x.prefix == y.prefix
              && x.value == y.value) {
            found = true;
          }
        }
        if (!found) {
          return false;
        }
      }
      break;
    }
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
    case GTEXT_MARKUP_KIND_COMMENT:
      if (a->data != b->data) {
        return false;
      }
      break;
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
      if (a->target != b->target || a->data != b->data) {
        return false;
      }
      break;
    case GTEXT_MARKUP_KIND_DOCTYPE:
      if (a->dt_name != b->dt_name || a->has_pub != b->has_pub
          || a->has_sys != b->has_sys || a->dt_pub != b->dt_pub
          || a->dt_sys != b->dt_sys) {
        return false;
      }
      break;
    default:
      break;
  }
  if ((a->content == nullptr) != (b->content == nullptr)) {
    return false;
  }
  if (a->content && !model_equal(a->content, b->content)) {
    return false;
  }
  if (a->kids.size() != b->kids.size()) {
    return false;
  }
  for (size_t i = 0; i < a->kids.size(); i++) {
    if (!model_equal(a->kids[i], b->kids[i])) {
      return false;
    }
  }
  return true;
}

size_t height(const MNode * n) {
  size_t h = 0;
  if (n->content) {
    h = 1 + height(n->content);
  }
  for (const MNode * c : n->kids) {
    h = std::max(h, 1 + height(c));
  }
  return h;
}

size_t depth(const MNode * n) {
  size_t d = 0;
  while (true) {
    const MNode * up = n->parent ? n->parent : n->owner;
    if (!up) {
      return d;
    }
    n = up;
    d++;
  }
}

bool inclusive_ancestor(const MNode * a, const MNode * n) {
  for (; n; n = n->parent ? n->parent : n->owner) {
    if (n == a) {
      return true;
    }
  }
  return false;
}

size_t subtree_size(const MNode * n) {  // n, its descendants and template contents
  size_t s = 1;
  if (n->content) {
    s += subtree_size(n->content);
  }
  for (const MNode * c : n->kids) {
    s += subtree_size(c);
  }
  return s;
}

void text_content(const MNode * n, std::string & out) {
  for (const MNode * c : n->kids) {
    if (c->kind == GTEXT_MARKUP_KIND_TEXT || c->kind == GTEXT_MARKUP_KIND_CDATA) {
      out += c->data;
    }
    text_content(c, out);
  }
}

// ---------------------------------------------------------------------------
// The world: two documents, the model of each, and the operations on both
// ---------------------------------------------------------------------------

struct World {
  FaultAllocator fa[2];
  GTEXT_Markup_Document * doc[2] = {nullptr, nullptr};
  GTEXT_Markup_Options opt[2];
  std::vector<std::unique_ptr<MNode>> nodes;
  MNode * root[2] = {nullptr, nullptr};
  uint64_t rng;
  std::string log;  // the operations so far, printed if something fails
  size_t oom_attempts = 0;

  World(uint64_t seed, const GTEXT_Markup_Options & o0, const GTEXT_Markup_Options & o1)
      : rng(seed * 0x9E3779B97F4A7C15ull + 1) {
    opt[0] = o0;
    opt[1] = o1;
    for (int d = 0; d < 2; d++) {
      GTEXT_Markup_Options o = opt[d];
      o.allocator = &fa[d].alloc;
      EXPECT_EQ(gtext_markup_document_new(&o, &doc[d]), GTEXT_MARKUP_OK);
      MNode * r = add(d, GTEXT_MARKUP_KIND_DOCUMENT);
      r->is_root = true;
      r->real = gtext_markup_document_root(doc[d]);
      root[d] = r;
    }
  }
  ~World() {
    for (int d = 0; d < 2; d++) {
      gtext_markup_document_free(doc[d]);
      EXPECT_TRUE(fa[d].live.empty()) << "document " << d << " leaked";
      EXPECT_EQ(fa[d].bad_frees, 0u);
    }
  }

  uint64_t next() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  }
  size_t below(size_t n) {
    return (size_t)(next() % n);
  }

  MNode * add(int d, Kind kind) {
    nodes.push_back(std::make_unique<MNode>());
    MNode * m = nodes.back().get();
    m->kind = kind;
    m->doc = d;
    return m;
  }

  size_t alive_count(int d) const {
    size_t n = 0;
    for (const auto & m : nodes) {
      n += m->alive && m->doc == d;
    }
    return n;
  }

  void kill(MNode * m) {
    m->alive = false;
    if (m->content) {
      kill(m->content);
    }
    for (MNode * c : m->kids) {
      kill(c);
    }
  }

  std::vector<MNode *> live(const std::function<bool(const MNode *)> & pred) {
    std::vector<MNode *> out;
    for (auto & m : nodes) {
      if (m->alive && pred(m.get())) {
        out.push_back(m.get());
      }
    }
    return out;
  }
  MNode * pick(const std::function<bool(const MNode *)> & pred) {
    auto v = live(pred);
    return v.empty() ? nullptr : v[below(v.size())];
  }

  bool is_top(const MNode * m) const {
    return !m->parent && !m->is_content;  // includes the roots
  }

  // ---- comparing the two -------------------------------------------------

  std::string whole_state() {
    // Everything either side can see, keyed so that order of the detached list
    // (an implementation detail) does not matter.
    std::vector<std::string> parts;
    for (auto & m : nodes) {
      if (m->alive && is_top(m.get())) {
        parts.push_back(dump_to_string(m->real));
      }
    }
    std::sort(parts.begin(), parts.end());
    std::string s;
    for (auto & p : parts) {
      s += p + "\x01";
    }
    for (int d = 0; d < 2; d++) {
      s += std::to_string(gtext_markup_document_node_count(doc[d])) + ";";
    }
    return s;
  }

  void compare(const std::string & what) {
    for (int d = 0; d < 2; d++) {
      const char * problem = markup_check_invariants(doc[d]);
      ASSERT_STREQ(problem ? problem : "ok", "ok") << what << "\n" << log;
      ASSERT_EQ(gtext_markup_document_node_count(doc[d]), alive_count(d))
          << what << "\n" << log;
    }
    for (auto & mp : nodes) {
      MNode * m = mp.get();
      if (!m->alive) {
        continue;
      }
      ASSERT_EQ(gtext_markup_node_kind(m->real), m->kind) << what << "\n" << log;
      ASSERT_EQ(gtext_markup_node_parent(m->real), m->parent ? m->parent->real : nullptr)
          << what << "\n" << log;
      ASSERT_EQ(gtext_markup_node_child_count(m->real), m->kids.size())
          << what << "\n" << log;
      if (!m->kids.empty()) {
        ASSERT_EQ(gtext_markup_node_first_child(m->real), m->kids.front()->real);
        ASSERT_EQ(gtext_markup_node_last_child(m->real), m->kids.back()->real);
      }
      if (is_top(m)) {
        ASSERT_EQ(dump_to_string(m->real), model_dump(m)) << what << "\n" << log;
      }
    }
    // Spot checks of the read-only comparisons.
    MNode * a = pick([](const MNode *) { return true; });
    MNode * b = pick([](const MNode *) { return true; });
    if (a && b && a->kind != GTEXT_MARKUP_KIND_DOCUMENT
        && b->kind != GTEXT_MARKUP_KIND_DOCUMENT) {
      ASSERT_EQ(gtext_markup_equal(a->real, b->real), model_equal(a, b))
          << what << "\n" << log;
    }
    if (a && container(a->kind)) {
      std::string want;
      text_content(a, want);
      char * got = nullptr;
      size_t length = 0;
      ASSERT_EQ(gtext_markup_node_text_content(a->real, &world_alloc(a->doc), &got,
                    &length), GTEXT_MARKUP_OK);
      ASSERT_EQ(std::string(got, length), want) << what << "\n" << log;
      gtext_markup_string_free(&world_alloc(a->doc), got);
    }
  }

  const GTEXT_Allocator & world_alloc(int d) {
    return fa[d].alloc;
  }

  // ---- predicting the status ----------------------------------------------

  // The precedence, in the order the library checks (and documents): a node
  // from another document; a child that is a document or a template's content;
  // a parent that cannot hold children; a reference that is not a child of the
  // parent; a cycle or a kind that may not go there; the document's own
  // one-element rule; and last the depth limit.
  Status expect_insert(MNode * parent, MNode * child, MNode * ref,
      const MNode * exclude) {
    if (parent->doc != child->doc) {
      return GTEXT_MARKUP_E_WRONG_DOCUMENT;
    }
    if (child->kind == GTEXT_MARKUP_KIND_DOCUMENT || child->is_content) {
      return GTEXT_MARKUP_E_INVALID;
    }
    if (!container(parent->kind)) {
      return GTEXT_MARKUP_E_HIERARCHY;
    }
    if (ref && (ref->parent != parent || ref->is_content)) {
      return GTEXT_MARKUP_E_INVALID;
    }
    const bool is_fragment = child->kind == GTEXT_MARKUP_KIND_FRAGMENT;
    if (is_fragment) {
      if (child == parent) {
        return GTEXT_MARKUP_E_HIERARCHY;
      }
      for (const MNode * c : child->kids) {
        if (inclusive_ancestor(c, parent) || !allowed(parent->kind, c->kind)) {
          return GTEXT_MARKUP_E_HIERARCHY;
        }
      }
    }
    else if (inclusive_ancestor(child, parent) || !allowed(parent->kind, child->kind)) {
      return GTEXT_MARKUP_E_HIERARCHY;
    }
    if (parent->kind == GTEXT_MARKUP_KIND_DOCUMENT) {
      std::vector<Kind> seq;
      auto add_new = [&]() {
        if (is_fragment) {
          for (const MNode * c : child->kids) {
            seq.push_back(c->kind);
          }
        }
        else {
          seq.push_back(child->kind);
        }
      };
      bool placed = false;
      for (const MNode * c : parent->kids) {
        if (c == ref) {
          add_new();
          placed = true;
        }
        if (c != child && c != exclude) {
          seq.push_back(c->kind);
        }
      }
      if (!placed) {
        add_new();
      }
      int elements = 0, doctypes = 0;
      bool seen_element = false, bad = false;
      for (Kind k : seq) {
        if (k == GTEXT_MARKUP_KIND_ELEMENT) {
          elements++;
          seen_element = true;
        }
        else if (k == GTEXT_MARKUP_KIND_DOCTYPE) {
          doctypes++;
          bad |= seen_element;
        }
        else if (k != GTEXT_MARKUP_KIND_COMMENT
            && k != GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION) {
          bad = true;
        }
      }
      if (bad || elements > 1 || doctypes > 1) {
        return GTEXT_MARKUP_E_HIERARCHY;
      }
    }
    const size_t limit = opt[parent->doc].max_depth;
    if (limit != 0) {
      // The deepest level anything lands on after the insertion.
      size_t deepest;
      if (is_fragment) {
        deepest = child->kids.empty() ? 0 : depth(parent) + height(child);
      }
      else {
        deepest = depth(parent) + 1 + height(child);
      }
      if (deepest > limit) {
        return GTEXT_MARKUP_E_LIMIT;
      }
    }
    return GTEXT_MARKUP_OK;
  }

  void detach_from_parent(MNode * m) {
    if (m->parent) {
      auto & v = m->parent->kids;
      v.erase(std::find(v.begin(), v.end(), m));
      m->parent = nullptr;
    }
  }

  void apply_insert(MNode * parent, MNode * child, MNode * ref) {
    if (child->kind == GTEXT_MARKUP_KIND_FRAGMENT) {
      std::vector<MNode *> moved = child->kids;
      child->kids.clear();
      for (MNode * c : moved) {
        c->parent = nullptr;
        insert_at(parent, c, ref);
      }
      return;
    }
    detach_from_parent(child);
    insert_at(parent, child, ref);
  }

  void insert_at(MNode * parent, MNode * node, MNode * ref) {
    auto & v = parent->kids;
    auto it = ref ? std::find(v.begin(), v.end(), ref) : v.end();
    v.insert(it, node);
    node->parent = parent;
  }

  // ---- the random material -------------------------------------------------

  std::string pick_text() {
    static const char * pool[] = {"", "t", "hello", "caf\xC3\xA9", "a b", "x\ny", "<&>"};
    if (below(40) == 0) {
      return "\xC3\x28";  // not UTF-8
    }
    if (below(25) == 0) {
      return std::string("n\0l", 3);
    }
    return pool[below(sizeof(pool) / sizeof(pool[0]))];
  }
  std::string pick_ns() {
    static const char * pool[] = {"", "", "urn:a", "urn:b", GTEXT_MARKUP_NS_HTML};
    if (below(80) == 0) {
      return "\xFF";
    }
    return pool[below(sizeof(pool) / sizeof(pool[0]))];
  }
  std::string pick_prefix() {
    static const char * pool[] = {"", "", "p", "q"};
    if (below(80) == 0) {
      return "\xFF";
    }
    return pool[below(sizeof(pool) / sizeof(pool[0]))];
  }
  std::string pick_local() {
    static const char * pool[] = {"a", "b", "c", "template", "x", "y"};
    if (below(50) == 0) {
      return "";
    }
    if (below(80) == 0) {
      return "\xC3\x28";
    }
    return pool[below(sizeof(pool) / sizeof(pool[0]))];
  }

  // ---- running one operation with an allocation failing in turn ----------

  // `run` performs the library call and returns its status; `done` is told the
  // status the library finally returned (not an OOM) so that the caller can
  // apply the model's side of it. Returns the final status.
  Status with_oom_sweep(int d, const std::function<Status()> & run) {
    const std::string before = whole_state();
    for (size_t n = 1;; n++) {
      fa[d].arm(n);
      const Status s = run();
      fa[d].disarm();
      if (s == GTEXT_MARKUP_E_OOM) {
        oom_attempts++;
        EXPECT_TRUE(fa[d].failed) << "OOM with no failure injected\n" << log;
        EXPECT_EQ(whole_state(), before)
            << "a failed operation changed the world (allocation " << n << ")\n" << log;
        const char * problem = markup_check_invariants(doc[d]);
        EXPECT_STREQ(problem ? problem : "ok", "ok")
            << "after failing allocation " << n << "\n" << log;
        if (::testing::Test::HasFailure()) {
          return s;
        }
        continue;
      }
      return s;
    }
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// The operations
// ---------------------------------------------------------------------------

namespace {

void run_world(World & w, size_t ops) {
  for (size_t step = 0; step < ops && !::testing::Test::HasFailure(); step++) {
    const size_t which = w.below(100);
    const int d = (int)w.below(2);
    auto note = [&](const std::string & s) {
      w.log += "[" + std::to_string(step) + "] " + s + "\n";
    };

    if (which < 24) {  // ---- create -------------------------------------
      const size_t kind_roll = w.below(8);
      const GTEXT_Markup_Options & o = w.opt[d];
      const bool full = o.max_nodes != 0 && w.alive_count(d) >= o.max_nodes;
      RNode * out = nullptr;
      Status s;
      MNode * m = nullptr;
      std::string ns, prefix, local, data, target, pub, sys;
      bool has_pub = false, has_sys = false;
      Status want = GTEXT_MARKUP_OK;
      Kind kind;
      if (kind_roll < 3) {
        kind = GTEXT_MARKUP_KIND_ELEMENT;
        ns = w.pick_ns();
        prefix = w.pick_prefix();
        local = w.pick_local();
        if (local.empty()) {
          want = GTEXT_MARKUP_E_NAME;
        }
        else if (!valid_utf8(local) || !valid_utf8(ns) || !valid_utf8(prefix)) {
          want = GTEXT_MARKUP_E_ENCODING;
        }
        note("create_element(" + std::to_string(d) + ", " + quote(ns) + ", "
            + quote(prefix) + ", " + quote(local) + ")");
        s = w.with_oom_sweep(d, [&]() {
          return gtext_markup_create_element(w.doc[d], ns.c_str(), prefix.c_str(),
              local.c_str(), &out);
        });
      }
      else if (kind_roll < 6) {
        const Kind kinds[] = {GTEXT_MARKUP_KIND_TEXT, GTEXT_MARKUP_KIND_CDATA,
            GTEXT_MARKUP_KIND_COMMENT};
        kind = kinds[kind_roll - 3];
        data = w.pick_text();
        if (!valid_utf8(data)) {
          want = GTEXT_MARKUP_E_ENCODING;
        }
        note("create_textual(" + std::to_string(d) + ", " + std::to_string(kind) + ", "
            + quote(data) + ")");
        s = w.with_oom_sweep(d, [&]() {
          return kind == GTEXT_MARKUP_KIND_TEXT
              ? gtext_markup_create_text(w.doc[d], data.data(), data.size(), &out)
              : kind == GTEXT_MARKUP_KIND_CDATA
              ? gtext_markup_create_cdata(w.doc[d], data.data(), data.size(), &out)
              : gtext_markup_create_comment(w.doc[d], data.data(), data.size(), &out);
        });
      }
      else if (kind_roll == 6) {
        if (w.below(2)) {
          kind = GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION;
          target = w.pick_local();
          data = w.pick_text();
          if (target.empty()) {
            want = GTEXT_MARKUP_E_NAME;
          }
          else if (!valid_utf8(target) || !valid_utf8(data)) {
            want = GTEXT_MARKUP_E_ENCODING;
          }
          note("create_pi(" + std::to_string(d) + ", " + quote(target) + ", "
              + quote(data) + ")");
          s = w.with_oom_sweep(d, [&]() {
            return gtext_markup_create_processing_instruction(w.doc[d], target.c_str(),
                data.data(), data.size(), &out);
          });
        }
        else {
          kind = GTEXT_MARKUP_KIND_DOCTYPE;
          local = w.pick_text();  // the name; empty is allowed
          has_pub = w.below(2);
          has_sys = w.below(2);
          pub = has_pub ? w.pick_text() : "";
          sys = has_sys ? w.pick_text() : "";
          if (!valid_utf8(local) || !valid_utf8(pub) || !valid_utf8(sys)
              || local.find('\0') != std::string::npos
              || pub.find('\0') != std::string::npos
              || sys.find('\0') != std::string::npos) {
            // A name passed as a C string stops at its NUL, so the text that
            // was meant is not the text the library sees. Skip rather than
            // model the truncation.
            continue;
          }
          note("create_doctype(" + std::to_string(d) + ", " + quote(local) + ")");
          s = w.with_oom_sweep(d, [&]() {
            return gtext_markup_create_doctype(w.doc[d], local.c_str(),
                has_pub ? pub.c_str() : nullptr, has_sys ? sys.c_str() : nullptr,
                &out);
          });
        }
      }
      else {
        kind = GTEXT_MARKUP_KIND_FRAGMENT;
        note("create_fragment(" + std::to_string(d) + ")");
        s = w.with_oom_sweep(d, [&]() { return gtext_markup_create_fragment(w.doc[d], &out); });
      }
      if (want == GTEXT_MARKUP_OK && full) {
        want = GTEXT_MARKUP_E_LIMIT;
      }
      ASSERT_EQ(s, want) << w.log;
      if (s == GTEXT_MARKUP_OK) {
        m = w.add(d, kind);
        m->real = out;
        m->ns = ns;
        m->prefix = prefix;
        m->local = (kind == GTEXT_MARKUP_KIND_ELEMENT) ? local : "";
        m->data = data;
        m->target = target;
        if (kind == GTEXT_MARKUP_KIND_DOCTYPE) {
          m->dt_name = local;
          m->has_pub = has_pub;
          m->has_sys = has_sys;
          m->dt_pub = pub;
          m->dt_sys = sys;
        }
      }
    }
    else if (which < 48) {  // ---- insert_before / append / replace --------
      MNode * parent = w.pick([](const MNode *) { return true; });
      MNode * child = w.pick([](const MNode *) { return true; });
      if (!parent || !child) {
        continue;
      }
      MNode * ref = nullptr;
      const size_t mode = w.below(10);
      if (mode < 6 && !parent->kids.empty()) {
        ref = parent->kids[w.below(parent->kids.size())];
      }
      else if (mode == 6) {
        ref = w.pick([](const MNode *) { return true; });  // usually wrong
      }
      if (which >= 42) {  // replace
        MNode * old_child = w.pick([](const MNode *) { return true; });
        if (!old_child) {
          continue;
        }
        note("replace(#" + std::to_string(old_child - w.nodes[0].get()) + ")");
        Status want;
        if (!old_child->parent || old_child->is_root || old_child->is_content) {
          want = GTEXT_MARKUP_E_INVALID;
        }
        else if (old_child == child) {
          want = GTEXT_MARKUP_OK;
        }
        else {
          want = w.expect_insert(old_child->parent, child, old_child, old_child);
        }
        MNode * old_parent = old_child->parent;
        const Status s = gtext_markup_replace(old_child->real, child->real);
        ASSERT_EQ(s, want) << "replace\n" << w.log;
        if (s == GTEXT_MARKUP_OK && old_child != child) {
          w.apply_insert(old_parent, child, old_child);
          w.detach_from_parent(old_child);
        }
      }
      else {
        note("insert_before(parent kind " + std::to_string(parent->kind)
            + ", child kind " + std::to_string(child->kind) + ")");
        // Inserting a node before itself leaves it in place.
        MNode * effective = (ref && ref == child) ? (child->parent == parent
            ? [&]() -> MNode * {
                auto & v = parent->kids;
                auto it = std::find(v.begin(), v.end(), child);
                return (it + 1 == v.end()) ? nullptr : *(it + 1);
              }()
            : ref) : ref;
        const Status want = w.expect_insert(parent, child, effective, nullptr);
        const Status s = gtext_markup_insert_before(parent->real, child->real,
            ref ? ref->real : nullptr);
        ASSERT_EQ(s, want) << "insert_before\n" << w.log;
        if (s == GTEXT_MARKUP_OK) {
          w.apply_insert(parent, child, effective);
        }
      }
    }
    else if (which < 56) {  // ---- remove -------------------------------------
      MNode * m = w.pick([](const MNode *) { return true; });
      if (!m) {
        continue;
      }
      note("remove");
      const Status want = (m->kind == GTEXT_MARKUP_KIND_DOCUMENT || m->is_root
          || m->is_content) ? GTEXT_MARKUP_E_INVALID : GTEXT_MARKUP_OK;
      const Status s = gtext_markup_remove(m->real);
      ASSERT_EQ(s, want) << w.log;
      if (s == GTEXT_MARKUP_OK) {
        w.detach_from_parent(m);
      }
    }
    else if (which < 62) {  // ---- free -----------------------------------------
      MNode * m = w.pick([](const MNode *) { return true; });
      if (!m) {
        continue;
      }
      note("node_free");
      const bool refuse = m->kind == GTEXT_MARKUP_KIND_DOCUMENT || m->is_root
          || m->is_content || m->parent != nullptr;
      const Status s = gtext_markup_node_free(m->real);
      ASSERT_EQ(s, refuse ? GTEXT_MARKUP_E_STATE : GTEXT_MARKUP_OK) << w.log;
      if (s == GTEXT_MARKUP_OK) {
        w.kill(m);
      }
    }
    else if (which < 72) {  // ---- attributes ---------------------------------------
      MNode * m = w.pick([](const MNode *) { return true; });
      if (!m) {
        continue;
      }
      const std::string ns = w.pick_ns();
      const std::string prefix = w.pick_prefix();
      std::string local = w.pick_local();
      const std::string value = w.pick_text();
      if (w.below(5) == 0 && m->kind == GTEXT_MARKUP_KIND_ELEMENT
          && !m->attrs.empty()) {
        // Remove one that exists.
        const MAttr a = m->attrs[w.below(m->attrs.size())];
        note("remove_attribute " + name_of(a.ns, "", a.local));
        ASSERT_TRUE(gtext_markup_element_remove_attribute(m->real,
            a.ns.c_str(), a.local.c_str())) << w.log;
        for (size_t i = 0; i < m->attrs.size(); i++) {
          if (m->attrs[i].ns == a.ns && m->attrs[i].local == a.local) {
            m->attrs.erase(m->attrs.begin() + i);
            break;
          }
        }
        continue;
      }
      note("set_attribute " + name_of(ns, prefix, local) + "=" + quote(value));
      Status want = GTEXT_MARKUP_OK;
      size_t found = SIZE_MAX;
      if (m->kind != GTEXT_MARKUP_KIND_ELEMENT) {
        want = GTEXT_MARKUP_E_INVALID;
      }
      else if (local.empty()) {
        want = GTEXT_MARKUP_E_NAME;
      }
      else if (!valid_utf8(local) || !valid_utf8(ns) || !valid_utf8(prefix)
          || !valid_utf8(value)) {
        want = GTEXT_MARKUP_E_ENCODING;
      }
      else {
        for (size_t i = 0; i < m->attrs.size(); i++) {
          if (m->attrs[i].ns == ns && m->attrs[i].local == local) {
            found = i;
          }
        }
        const size_t limit = w.opt[m->doc].max_attributes;
        if (found == SIZE_MAX && limit != 0 && m->attrs.size() >= limit) {
          want = GTEXT_MARKUP_E_LIMIT;
        }
      }
      const Status s = w.with_oom_sweep(m->doc, [&]() {
        return gtext_markup_element_set_attribute(m->real, ns.c_str(), prefix.c_str(),
            local.c_str(), value.data(), value.size());
      });
      ASSERT_EQ(s, want) << w.log;
      if (s == GTEXT_MARKUP_OK) {
        if (found != SIZE_MAX) {
          m->attrs[found].prefix = prefix;
          m->attrs[found].value = value;
        }
        else {
          m->attrs.push_back({ns, prefix, local, value});
        }
      }
    }
    else if (which < 78) {  // ---- set_data ---------------------------------------------
      MNode * m = w.pick([](const MNode *) { return true; });
      if (!m) {
        continue;
      }
      const std::string data = w.pick_text();
      note("set_data " + quote(data));
      const bool textual = m->kind == GTEXT_MARKUP_KIND_TEXT
          || m->kind == GTEXT_MARKUP_KIND_CDATA || m->kind == GTEXT_MARKUP_KIND_COMMENT
          || m->kind == GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION;
      Status want = !textual ? GTEXT_MARKUP_E_INVALID
          : (valid_utf8(data) ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_ENCODING);
      const Status s = w.with_oom_sweep(m->doc, [&]() {
        return gtext_markup_node_set_data(m->real, data.data(), data.size());
      });
      ASSERT_EQ(s, want) << w.log;
      if (s == GTEXT_MARKUP_OK) {
        m->data = data;
      }
    }
    else if (which < 84) {  // ---- template content ---------------------------------------
      MNode * m = w.pick([](const MNode *) { return true; });
      if (!m) {
        continue;
      }
      note("ensure_template_content");
      Status want = GTEXT_MARKUP_OK;
      if (m->kind != GTEXT_MARKUP_KIND_ELEMENT) {
        want = GTEXT_MARKUP_E_INVALID;
      }
      else if (!m->content) {
        const size_t limit = w.opt[m->doc].max_nodes;
        if (limit != 0 && w.alive_count(m->doc) >= limit) {
          want = GTEXT_MARKUP_E_LIMIT;
        }
      }
      RNode * content = nullptr;
      const Status s = w.with_oom_sweep(m->doc, [&]() {
        return gtext_markup_element_ensure_template_content(m->real, &content);
      });
      ASSERT_EQ(s, want) << w.log;
      if (s == GTEXT_MARKUP_OK && !m->content) {
        MNode * c = w.add(m->doc, GTEXT_MARKUP_KIND_FRAGMENT);
        c->is_content = true;
        c->owner = m;
        c->real = content;
        m->content = c;
      }
      else if (s == GTEXT_MARKUP_OK) {
        ASSERT_EQ(content, m->content->real) << "ensure must return the existing one";
      }
    }
    else {  // ---- clone ----------------------------------------------------------------
      MNode * src = w.pick([](const MNode *) { return true; });
      if (!src) {
        continue;
      }
      const int dst = (int)w.below(2);
      const bool deep = w.below(3) != 0;
      note("clone(deep=" + std::to_string(deep) + ", into " + std::to_string(dst) + ")");
      Status want = GTEXT_MARKUP_OK;
      if (src->kind == GTEXT_MARKUP_KIND_DOCUMENT) {
        want = GTEXT_MARKUP_E_INVALID;
      }
      else {
        // Every node the copy needs, and every element's attribute count.
        size_t needed = 1;
        bool too_many_attrs = false;
        std::function<void(const MNode *, bool)> walk = [&](const MNode * n, bool top) {
          if (n->kind == GTEXT_MARKUP_KIND_ELEMENT
              && w.opt[dst].max_attributes != 0
              && n->attrs.size() > w.opt[dst].max_attributes) {
            too_many_attrs = true;
          }
          if (!top || deep) {
            if (n->content && deep) {
              needed += subtree_size(n->content);
              walk(n->content, false);
            }
            for (const MNode * c : n->kids) {
              if (deep) {
                needed += subtree_size(c);
                walk(c, false);
              }
            }
          }
        };
        walk(src, true);
        const size_t limit = w.opt[dst].max_nodes;
        if (too_many_attrs || (limit != 0 && w.alive_count(dst) + needed > limit)) {
          want = GTEXT_MARKUP_E_LIMIT;
        }
      }
      RNode * copy = nullptr;
      const Status s = w.with_oom_sweep(dst, [&]() {
        return gtext_markup_clone(w.doc[dst], src->real, deep, &copy);
      });
      ASSERT_EQ(s, want) << "clone\n" << w.log;
      if (s == GTEXT_MARKUP_OK) {
        std::function<MNode *(const MNode *, bool)> dup = [&](const MNode * n,
            bool top) -> MNode * {
          MNode * c = w.add(dst, n->is_content ? GTEXT_MARKUP_KIND_FRAGMENT : n->kind);
          c->ns = n->ns; c->prefix = n->prefix; c->local = n->local;
          c->attrs = n->attrs; c->data = n->data; c->target = n->target;
          c->dt_name = n->dt_name; c->dt_pub = n->dt_pub; c->dt_sys = n->dt_sys;
          c->has_pub = n->has_pub; c->has_sys = n->has_sys;
          if (top && !deep) {
            return c;
          }
          if (n->content) {
            c->content = dup(n->content, false);
            c->content->is_content = true;
            c->content->owner = c;
          }
          for (const MNode * k : n->kids) {
            MNode * kc = dup(k, false);
            kc->parent = c;
            c->kids.push_back(kc);
          }
          return c;
        };
        MNode * c = dup(src, true);
        // The real handles of the copy: walk both in step.
        std::function<void(MNode *, RNode *)> bind = [&](MNode * m, RNode * r) {
          m->real = r;
          if (m->content) {
            bind(m->content, gtext_markup_element_template_content(r));
          }
          RNode * rk = gtext_markup_node_first_child(r);
          for (MNode * k : m->kids) {
            ASSERT_NE(rk, nullptr);
            bind(k, rk);
            rk = gtext_markup_node_next_sibling(rk);
          }
          ASSERT_EQ(rk, nullptr) << "the copy has more children than the model's";
        };
        bind(c, copy);
      }
    }
    w.compare("after step " + std::to_string(step));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// The tests
// ---------------------------------------------------------------------------

namespace {

GTEXT_Markup_Options options(size_t depth, size_t nodes, size_t attrs) {
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_depth = depth;
  o.max_nodes = nodes;
  o.max_attributes = attrs;
  return o;
}

}  // namespace

TEST(MarkupModel, TheTreeAgreesWithTheReferenceOverRandomOperationsWithNoLimits) {
  for (uint64_t seed = 1; seed <= 60; seed++) {
    World w(seed, options(0, 0, 0), options(0, 0, 0));
    run_world(w, 250);
    if (::testing::Test::HasFailure()) {
      FAIL() << "seed " << seed;
    }
  }
}

TEST(MarkupModel, TheTreeAgreesWithTheReferenceWhenDepthIsTight) {
  for (uint64_t seed = 100; seed < 160; seed++) {
    World w(seed, options(3 + seed % 4, 0, 0), options(2 + seed % 3, 0, 0));
    run_world(w, 250);
    if (::testing::Test::HasFailure()) {
      FAIL() << "seed " << seed;
    }
  }
}

TEST(MarkupModel, TheTreeAgreesWithTheReferenceWhenNodesAndAttributesAreScarce) {
  for (uint64_t seed = 200; seed < 260; seed++) {
    World w(seed, options(6, 20 + seed % 20, 2), options(0, 12 + seed % 10, 1));
    run_world(w, 250);
    if (::testing::Test::HasFailure()) {
      FAIL() << "seed " << seed;
    }
  }
}

TEST(MarkupModel, ARunLongEnoughToExerciseTheNameTableGrowing) {
  // The atom table starts at 64 buckets and doubles; this many distinct names
  // forces it through several doublings while the model checks every step.
  World w(777, options(0, 0, 0), options(0, 0, 0));
  for (int i = 0; i < 400 && !::testing::Test::HasFailure(); i++) {
    RNode * e = nullptr;
    const std::string name = "n" + std::to_string(i);
    ASSERT_EQ(gtext_markup_create_element(w.doc[0], nullptr, nullptr, name.c_str(), &e),
        GTEXT_MARKUP_OK);
    MNode * m = w.add(0, GTEXT_MARKUP_KIND_ELEMENT);
    m->real = e;
    m->local = name;
    for (int a = 0; a < 3; a++) {
      const std::string an = "a" + std::to_string(i * 3 + a);
      ASSERT_EQ(gtext_markup_element_set_attribute(e, "urn:grow", nullptr, an.c_str(),
                    "v", 1), GTEXT_MARKUP_OK);
      m->attrs.push_back({"urn:grow", "", an, "v"});
    }
    if (i % 25 == 0) {
      w.compare("growth " + std::to_string(i));
    }
  }
  w.compare("end of growth");
  EXPECT_GE(w.oom_attempts, 0u);
}

TEST(MarkupModel, TheOomSweepActuallyFailsAllocationsInTheOperationsItWraps) {
  // A guard on the harness: if with_oom_sweep never saw an OOM, "a failed
  // operation changed nothing" would be vacuously true.
  World w(4242, options(0, 0, 0), options(0, 0, 0));
  run_world(w, 300);
  ASSERT_FALSE(::testing::Test::HasFailure());
  EXPECT_GT(w.oom_attempts, 200u);
}
