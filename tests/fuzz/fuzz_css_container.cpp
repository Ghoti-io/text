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
 * libFuzzer harness for container queries (src/css/css_container*.c and the container half of the rule index).
 *
 * The input drives a generator: a tree of up to 24 elements (each with a parent, a `container-type`, a name, a content box and a custom property), a list of up to three
 * container conditions made of size features, style features and boolean operators, and a style sheet that uses the same conditions on a probe element. The properties:
 *
 *   a status is OK, a limit, or E_OOM for the allocation that was failed, with nothing leaked or freed that the allocator did not make - never E_INTERNAL;
 *   the library's answer for every element equals the answer of a naive reference written here: for each condition it walks the ancestors of the element with no memo, takes
 *   the nearest that qualifies and evaluates the generated tree afresh with Kleene logic (unknown at the top is false);
 *   the answer is the same whether one call context (the container cache) is shared by every element or each element has a fresh one;
 *   the serialization of the generated list is a fixed point;
 *   the rules a sheet applies to the probe through the index are the rules whose condition the reference says is true.
 *
 * Seeds: tests/fuzz/corpus/css-container, made by tools/css/make-fuzz-container-seeds.py.
 * Build with: make fuzz-css-container      Run: make fuzz-run-css-container
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "../css_container_test_support.h"
#include "../markup_test_support.h"

namespace css_containerfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_css_container: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
  for (unsigned char c : input) {
    std::fprintf(stderr, "%02x", c);
  }
  std::fputc('\n', stderr);
  std::abort();
}

struct Bytes {
  const uint8_t * p;
  size_t n, i = 0;
  uint8_t next() {
    return i < n ? p[i++] : 0;
  }
  bool more() const {
    return i < n;
  }
};

enum Tri { F = 0, T = 1, U = 2 };

Tri tnot(Tri a) {
  return a == U ? U : (a == T ? F : T);
}
Tri tand(Tri a, Tri b) {
  if (a == F || b == F) {
    return F;
  }
  return a == T && b == T ? T : U;
}
Tri tor(Tri a, Tri b) {
  if (a == T || b == T) {
    return T;
  }
  return a == F && b == F ? F : U;
}

/// An element of the generated tree.
struct Elem {
  int parent = 0;
  int type = 0;      ///< 0 normal, 1 size, 2 inline-size
  int name = 0;      ///< 0 none, 1..3
  int w = 100, h = 100;
  int v = 0;         ///< the custom property --v (0: not set)
};

/// A generated condition node.
struct Node {
  enum Kind { SIZE, STYLE, NOT, AND, OR } kind = SIZE;
  int feature = 0;   ///< SIZE: 0 width, 1 height, 2 inline-size
  int op = 0;        ///< 0 <, 1 <=, 2 >, 3 >=, 4 =
  int value = 0;     ///< SIZE: the px; STYLE: the number compared
  bool boolean = false;
  std::vector<Node> kids;
};

Node make(Bytes & b, int depth) {
  Node n;
  const int k = depth >= 3 ? b.next() % 2 : b.next() % 5;
  switch (k) {
  case 0:
    n.kind = Node::SIZE;
    n.feature = b.next() % 3;
    n.op = b.next() % 5;
    n.value = 20 * (b.next() % 12);
    break;
  case 1:
    n.kind = Node::STYLE;
    n.value = b.next() % 4;
    n.boolean = (b.next() & 3) == 0;
    break;
  case 2:
    n.kind = Node::NOT;
    n.kids.push_back(make(b, depth + 1));
    break;
  default:
    n.kind = k == 3 ? Node::AND : Node::OR;
    const int c = 2 + b.next() % 2;
    for (int i = 0; i < c; i++) {
      n.kids.push_back(make(b, depth + 1));
    }
    break;
  }
  return n;
}

const char * const kOps[] = {"<", "<=", ">", ">=", "="};
const char * const kFeatures[] = {"width", "height", "inline-size"};

std::string text(const Node & n, bool top) {
  switch (n.kind) {
  case Node::SIZE:
    return std::string("(") + kFeatures[n.feature] + " " + kOps[n.op] + " " + std::to_string(n.value) + "px)";
  case Node::STYLE:
    return n.boolean ? std::string("style(--v)") : "style(--v: " + std::to_string(n.value) + ")";
  case Node::NOT:
    return "not " + (n.kids[0].kind == Node::SIZE || n.kids[0].kind == Node::STYLE ? text(n.kids[0], false) : "(" + text(n.kids[0], true) + ")");
  default: {
    std::string out;
    for (size_t i = 0; i < n.kids.size(); i++) {
      if (i) {
        out += n.kind == Node::AND ? " and " : " or ";
      }
      /* `not` takes one operand and cannot be mixed with `and` or `or`: a group inside a group is parenthesized */
      out += n.kids[i].kind == Node::SIZE || n.kids[i].kind == Node::STYLE ? text(n.kids[i], false) : "(" + text(n.kids[i], true) + ")";
    }
    return top ? out : "(" + out + ")";
  }
  }
}

/// What a condition asks of its container: bit 0 size, bit 1 style.
int kinds(const Node & n) {
  int k = n.kind == Node::SIZE ? 1 : (n.kind == Node::STYLE ? 2 : 0);
  for (const Node & c : n.kids) {
    k |= kinds(c);
  }
  return k;
}

/// Whether a condition asks about the block axis (`height`, horizontal-tb): a container is selected for the whole condition, and an inline-size container does not support that axis.
bool asks_block(const Node & n) {
  if (n.kind == Node::SIZE && n.feature == 1) {
    return true;
  }
  for (const Node & c : n.kids) {
    if (asks_block(c)) {
      return true;
    }
  }
  return false;
}

bool cmp(int a, int op, int b) {
  switch (op) {
  case 0: return a < b;
  case 1: return a <= b;
  case 2: return a > b;
  case 3: return a >= b;
  default: return a == b;
  }
}

/// The computed value of --v on an element: custom properties inherit, so the nearest element at or above it that sets it (0: none).
int effective_v(const std::vector<Elem> & el, int c) {
  for (; c; c = el[c].parent) {
    if (el[c].v) {
      return el[c].v;
    }
  }
  return 0;
}

/// The reference evaluation against a container (an index into the elements).
Tri eval(const Node & n, const std::vector<Elem> & el, int c) {
  const Elem & e = el[c];
  switch (n.kind) {
  case Node::SIZE: {
    /* a size container contains both axes; an inline-size container is selected only by a condition that asks the inline axis alone (horizontal-tb: the width), so the block axis never reaches it here */
    const bool horizontal = n.feature == 0 || n.feature == 2;
    if (e.type == 2 && !horizontal) {
      return U;
    }
    const int size = horizontal ? e.w : e.h;
    return cmp(size, n.op, n.value) ? T : F;
  }
  case Node::STYLE:
    if (n.boolean) {
      return effective_v(el, c) != 0 ? T : F;
    }
    return effective_v(el, c) != 0 && effective_v(el, c) == n.value ? T : F;
  case Node::NOT:
    return tnot(eval(n.kids[0], el, c));
  case Node::AND: {
    Tri r = T;
    for (const Node & k : n.kids) {
      r = tand(r, eval(k, el, c));
    }
    return r;
  }
  default: {
    Tri r = F;
    for (const Node & k : n.kids) {
      r = tor(r, eval(k, el, c));
    }
    return r;
  }
  }
}

/// The reference: the nearest ancestor (the element's parent first) that is a container for the kinds and the name; no memo.
bool reference(const Node & q, int name, const std::vector<Elem> & el, int e) {
  const int k = kinds(q);
  for (int a = el[e].parent; a; a = el[a].parent) {
    const Elem & c = el[a];
    if ((k & 1) && c.type == 0) {
      continue;
    }
    if ((k & 1) && c.type == 2 && asks_block(q)) {
      continue; /* an inline-size container is passed over by a condition that asks about the block axis */
    }
    if (name && c.name != name) {
      continue;
    }
    return eval(q, el, a) == T;
  }
  return false;
}

int run(const uint8_t * data, size_t size) {
  if (size < 4) {
    return 0;
  }
  const std::string input(reinterpret_cast<const char *>(data), size);
  Bytes b{data, size};
  const uint8_t o0 = b.next();
  const bool fail_alloc = (o0 & 1u) != 0;
  const size_t fail_at = static_cast<size_t>(b.next()) + 1;
  const int count = 2 + b.next() % 22;
  std::vector<Elem> el(count + 1);
  for (int i = 1; i <= count; i++) {
    Elem & e = el[i];
    e.parent = i == 1 ? 0 : b.next() % i;
    e.type = b.next() % 3;
    e.name = b.next() % 4;
    e.w = 20 * (b.next() % 12);
    e.h = 20 * (b.next() % 12);
    e.v = b.next() % 4;
  }
  const int nq = 1 + b.next() % 3;
  struct Cond {
    int name;
    Node q;
  };
  std::vector<Cond> conds;
  std::string list;
  for (int i = 0; i < nq; i++) {
    Cond c;
    c.name = b.next() % 4;
    c.q = make(b, 0);
    if (i) {
      list += ", ";
    }
    if (c.name) {
      list += "n" + std::to_string(c.name) + " ";
    }
    list += text(c.q, true);
    conds.push_back(c);
  }
  /* the serialization is a fixed point */
  {
    GTEXT_CSS_Container_Query * q = nullptr;
    if (gtext_css_container_parse(list.data(), list.size(), nullptr, &q, nullptr) != GTEXT_CSS_OK) {
      fail("a generated list did not parse", list, input);
    }
    char *s1 = nullptr, *s2 = nullptr;
    size_t n1 = 0, n2 = 0;
    if (gtext_css_media_list_serialize_buffer(q, nullptr, &s1, &n1) != GTEXT_CSS_OK) {
      fail("serialize failed", list, input);
    }
    GTEXT_CSS_Container_Query * q2 = nullptr;
    if (gtext_css_container_parse(s1, n1, nullptr, &q2, nullptr) != GTEXT_CSS_OK || gtext_css_media_list_serialize_buffer(q2, nullptr, &s2, &n2) != GTEXT_CSS_OK) {
      fail("the serialization did not parse", std::string(s1, n1), input);
    }
    if (n1 != n2 || std::memcmp(s1, s2, n1) != 0) {
      fail("the serialization is not a fixed point", std::string(s1, n1) + "\n" + std::string(s2, n2), input);
    }
    gtext_css_media_buffer_free(nullptr, s1);
    gtext_css_media_buffer_free(nullptr, s2);
    gtext_css_media_list_free(q);
    gtext_css_media_list_free(q2);
  }
  FaultAllocator fa;
  {
    cq::Doc d;
    for (int i = 1; i <= count; i++) {
      d.add(el[i].parent, "div", "e" + std::to_string(i));
    }
    for (int i = 1; i <= count; i++) {
      std::string decl;
      if (el[i].type == 1) {
        decl += "container-type: size;";
      }
      else if (el[i].type == 2) {
        decl += "container-type: inline-size;";
      }
      if (el[i].name) {
        decl += "container-name: n" + std::to_string(el[i].name) + ";";
      }
      if (el[i].v) {
        decl += "--v: " + std::to_string(el[i].v) + ";";
      }
      d.own[i] = decl;
      d.boxes[i] = {static_cast<double>(el[i].w), static_cast<double>(el[i].h)};
    }
    d.build();
    if (d.status != GTEXT_CSS_OK) {
      fail("the document could not be built", gtext_css_status_string(d.status), input);
    }
    GTEXT_CSS_Container_Query * q = nullptr;
    GTEXT_CSS_Media_Options mo = gtext_css_media_options_default();
    mo.allocator = &fa.alloc;
    if (gtext_css_container_parse(list.data(), list.size(), &mo, &q, nullptr) != GTEXT_CSS_OK) {
      fail("a generated list did not parse", list, input);
    }
    /* one call for every element, and a fresh one for each */
    GTEXT_CSS_Condition_Call shared;
    gtext_css_condition_call_init(&shared);
    for (int e = 1; e <= count; e++) {
      bool want = false;
      for (const Cond & c : conds) {
        want = want || reference(c.q, c.name, el, e);
      }
      bool got1 = false, got2 = false;
      const GTEXT_CSS_Status s1 = gtext_css_conditions_evaluate_container(d.cond, q, synth::Tree::H(e), false, &shared, &got1);
      GTEXT_CSS_Condition_Call fresh;
      gtext_css_condition_call_init(&fresh);
      const GTEXT_CSS_Status s2 = gtext_css_conditions_evaluate_container(d.cond, q, synth::Tree::H(e), false, &fresh, &got2);
      gtext_css_condition_call_free(&fresh);
      if (s1 != GTEXT_CSS_OK || s2 != GTEXT_CSS_OK) {
        fail("an evaluation failed", std::string(gtext_css_status_string(s1)) + " / " + gtext_css_status_string(s2) + "\n" + list, input);
      }
      if (got1 != want || got2 != want) {
        /* which condition: each alone, library and reference */
        std::string each;
        for (size_t ci = 0; ci < conds.size(); ci++) {
          std::string one = (conds[ci].name ? "n" + std::to_string(conds[ci].name) + " " : std::string()) + text(conds[ci].q, true);
          GTEXT_CSS_Container_Query * q1 = nullptr;
          bool m1 = false;
          if (gtext_css_container_parse(one.data(), one.size(), nullptr, &q1, nullptr) == GTEXT_CSS_OK) {
            GTEXT_CSS_Condition_Call c1;
            gtext_css_condition_call_init(&c1);
            (void)gtext_css_conditions_evaluate_container(d.cond, q1, synth::Tree::H(e), false, &c1, &m1);
            gtext_css_condition_call_free(&c1);
          }
          gtext_css_media_list_free(q1);
          each += "\n  [" + one + "] library " + (m1 ? "true" : "false") + " reference " + (reference(conds[ci].q, conds[ci].name, el, e) ? "true" : "false");
        }
        std::string tree;
        for (int i = 1; i <= count; i++) {
          tree += "\n  e" + std::to_string(i) + " parent " + std::to_string(el[i].parent) + " type " + std::to_string(el[i].type) + " name " + std::to_string(el[i].name) + " box " +
              std::to_string(el[i].w) + "x" + std::to_string(el[i].h) + " v " + std::to_string(el[i].v);
        }
        fail("the answer is not the reference's", each + tree + "\n" + "element " + std::to_string(e) + " list " + list + " library " + (got1 ? "true" : "false") + "/" + (got2 ? "true" : "false") +
            " reference " + (want ? "true" : "false"), input);
      }
    }
    gtext_css_condition_call_free(&shared);
    gtext_css_media_list_free(q);
    (void)fail_alloc;
    (void)fail_at;
  }
  if (!fa.live.empty()) {
    fail("an allocation leaked", std::to_string(fa.live.size()) + " blocks", input);
  }
  if (fa.bad_frees) {
    fail("a block the allocator did not make was freed", "", input);
  }
  return 0;
}

}  // namespace css_containerfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_containerfuzz::run(data, size);
}
