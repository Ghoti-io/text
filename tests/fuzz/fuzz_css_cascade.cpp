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
 * libFuzzer harness for the cascade (src/css/css_cascade.c, css_revert.c, css_layer.c).
 *
 * The input is four option bytes, then records separated by line feeds. A record is six key characters, a space, and a declaration `name: value`:
 *
 *   k0  bits 0-2 the origin (mod 5: user-agent, user, author, animation, transition), bit 3 `!important`, bit 4 a presentational hint, bit 5 element-attached
 *   k1  bits 0-2 the layer (0: none, 1 to 7: a, b, b.x, c, c.y.z, d, a.w, of the record's origin), bits 3-4 the context, bit 5 scoped
 *   k2  bits 0-1, 2-3, 4-5 the specificity (A, B, C)
 *   k3  bits 0-2 the scope distance
 *   k4  bits 0-2 the sheet, bits 3-5 the rule        k5  bits 0-2 the declaration
 *
 * Each key character is 0x40 plus its six bits (`@` to `\x7f`), so that a record is text and no key is a line feed. Combinations the cascade refuses (an
 * important animation declaration, a layered style attribute, a hint that is not author-level) are made valid before they are added.
 *
 *   o0  bit 0 keep the losers, bit 1 the parent's direction is rtl, bit 2 the parent's writing mode is vertical-rl, bit 3 compute the winners as an element
 *   o1  unused (reserved)   o2  unused (reserved)   o3  the allocation to fail (0 for none), counted over the whole run
 *
 * The properties:
 *
 *   the winners are what a **reference model** says: the model sorts by the criteria of Cascade 5, 6.1 and Cascade 6, 3.1 as the specification lists them
 *   (it compares two declarations criterion by criterion, where the library compares one word per criterion), finds the winner of a property by taking the
 *   strongest and, when that rolls back, removing what the keyword says (`revert`: every declaration of its origin level and above; `revert-layer`: down to
 *   the lowest of its layer's normal level; `revert-rule`: its rule) and starting again, and cascades the three properties of the flow first. The winners,
 *   their rollbacks and the flow must be the library's;
 *   the answer **does not depend on the order of the candidates**: the records shuffled (by a fixed shuffle of the input) give the same dump, when no two
 *   records have the same order of appearance;
 *   a cascade twice gives the same dump and the same hash; the losers (when kept) are the other candidates of the property, in order, each rolled back exactly
 *   when it sorts above the winner; the block of the winners has one item for each and no rolling keyword in it, and computes as an element or is refused
 *   as the computed-value layer says (a property it does not compute, a callback it lacks, a limit), never E_INTERNAL;
 *   **an allocation that fails is E_OOM**, with nothing returned and nothing leaked or freed that the allocator did not make.
 *
 * Seeds: tests/fuzz/corpus/css-cascade, made by tools/css/make-fuzz-cascade-seeds.py.
 * Build with: make fuzz-css-cascade      Run: make fuzz-run-css-cascade
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <ghoti.io/text/css.h>

#include "../markup_test_support.h"

namespace css_casfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_css_cascade: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
  for (unsigned char c : input) {
    if (c >= 0x20 && c < 0x7f && c != '\\') {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

const GTEXT_CSS_Registry * registry() {
  static GTEXT_CSS_Registry * r = [] {
    GTEXT_CSS_Registry * made = nullptr;
    if (gtext_css_registry_new(nullptr, &made) != GTEXT_CSS_OK || gtext_css_registry_prepare(made) != GTEXT_CSS_OK) {
      fail("the registry could not be made", "", "");
    }
    return made;
  }();
  return r;
}

bool allowed(GTEXT_CSS_Status s) {
  switch (s) {
  case GTEXT_CSS_OK:
  case GTEXT_CSS_E_OOM:
  case GTEXT_CSS_E_UNSUPPORTED:
  case GTEXT_CSS_E_INVALID:
  case GTEXT_CSS_E_LIMIT_WORK:
  case GTEXT_CSS_E_LIMIT_OUTPUT:
  case GTEXT_CSS_E_LIMIT_DEPTH:
  case GTEXT_CSS_E_LIMIT_MATH_NODES:
  case GTEXT_CSS_E_LIMIT_COLOR_NESTING:
  case GTEXT_CSS_E_LIMIT_STOPS:
  case GTEXT_CSS_E_LIMIT_FUNCTION_NODES:
  case GTEXT_CSS_E_LIMIT_TOKENS:
  case GTEXT_CSS_E_LIMIT_INPUT:
  case GTEXT_CSS_E_LIMIT_DECLARATIONS:
  case GTEXT_CSS_E_LIMIT_LIST:
  case GTEXT_CSS_E_LIMIT_ELEMENTS:
  case GTEXT_CSS_E_LIMIT_CANDIDATES:
    return true;
  default:
    return false;
  }
}

struct Rec {
  uint8_t k[6];
  std::string name, value;
};

/// The layers of an origin by index (1 to 7): a, b, b.x, c, c.y.z, d, a.w.
const char * const kLayerPaths[8] = {"", "a", "b", "b.x", "c", "c.y.z", "d", "a.w"};

/// The order the four contexts declare them in (each has a registry of its own: "Cascade layers are scoped to their origin and context"): the same seven
/// names, ordered differently.
const int kLayerOrders[4][7] = {{1, 2, 3, 4, 5, 6, 7}, {6, 4, 5, 2, 3, 1, 7}, {3, 2, 7, 1, 5, 4, 6}, {7, 5, 3, 6, 4, 2, 1}};

std::vector<Rec> parse(const std::string & text) {
  std::vector<Rec> out;
  size_t pos = 0;
  while (pos < text.size() && out.size() < 48) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    if (line.size() < 9 || line[6] != ' ') {
      continue;
    }
    const size_t colon = line.find(':', 7);
    if (colon == std::string::npos || colon > 7 + 80) {
      continue;
    }
    Rec r;
    for (int i = 0; i < 6; i++) {
      r.k[i] = (uint8_t)line[i] & 0x3f;
    }
    r.name = line.substr(7, colon - 7);
    r.value = line.substr(colon + 1);
    while (!r.name.empty() && r.name.back() == ' ') {
      r.name.pop_back();
    }
    while (!r.value.empty() && r.value.front() == ' ') {
      r.value.erase(r.value.begin());
    }
    if (r.name.empty() || r.value.size() > 200) {
      continue;
    }
    out.push_back(r);
  }
  return out;
}

/// The keys of a record, made valid.
GTEXT_CSS_Cascade_Entry keys_of(const Rec & r, GTEXT_CSS_Layer_Id ids[4][3][8]) {
  GTEXT_CSS_Cascade_Entry e;
  std::memset(&e, 0, sizeof e);
  e.origin = (GTEXT_CSS_Origin)((r.k[0] & 7) % 5);
  e.important = (r.k[0] & 8) != 0;
  e.presentational_hint = (r.k[0] & 16) != 0;
  e.element_attached = (r.k[0] & 32) != 0;
  const bool timed = e.origin >= GTEXT_CSS_ORIGIN_ANIMATION;
  if (timed) {
    e.important = e.presentational_hint = e.element_attached = false;
  }
  if (e.presentational_hint && (e.origin != GTEXT_CSS_ORIGIN_AUTHOR || e.important || e.element_attached)) {
    e.presentational_hint = false;
  }
  if (!timed && !e.element_attached && !e.presentational_hint) {
    e.layer = ids[(r.k[1] >> 3) & 3][e.origin][r.k[1] & 7];
  }
  e.context = (r.k[1] >> 3) & 3;
  e.scoped = (r.k[1] & 32) != 0;
  e.scope_distance = r.k[3] & 7;
  e.specificity.a = r.k[2] & 3;
  e.specificity.b = (r.k[2] >> 2) & 3;
  e.specificity.c = (r.k[2] >> 4) & 3;
  e.sheet = r.k[4] & 7;
  e.rule = (r.k[4] >> 3) & 7;
  e.declaration = r.k[5] & 7;
  e.rule_id = 1;
  return e;
}

/* ---- the run --------------------------------------------------------------------------------------------------- */

struct Pipe {
  const GTEXT_Allocator * alloc = nullptr;
  GTEXT_CSS_Layer_Registry * layers[4] = {nullptr, nullptr, nullptr, nullptr};
  GTEXT_CSS_Layer_Id ids[4][3][8];
  GTEXT_CSS_Candidates * set = nullptr;
  GTEXT_CSS_Cascaded * c = nullptr;
  std::vector<GTEXT_CSS_Cascade_Entry> keys;  ///< The keys of each added record, by group.
  GTEXT_CSS_Status status = GTEXT_CSS_OK;

  explicit Pipe(const GTEXT_Allocator * a) : alloc(a) {
    std::memset(ids, 0, sizeof ids);
  }
  Pipe(const Pipe &) = delete;
  Pipe & operator=(const Pipe &) = delete;
  ~Pipe() {
    gtext_css_cascaded_free(c);
    gtext_css_candidates_free(set);
    for (GTEXT_CSS_Layer_Registry * l : layers) {
      gtext_css_layer_registry_free(l);
    }
  }

  GTEXT_CSS_Status build(const std::vector<Rec> & recs, const GTEXT_CSS_Cascade_Options & base, const std::string & input) {
    GTEXT_CSS_Layer_Options lo = gtext_css_layer_options_default();
    lo.allocator = alloc;
    status = GTEXT_CSS_OK;
    for (int c = 0; status == GTEXT_CSS_OK && c < 4; c++) {
      status = gtext_css_layer_registry_new(&lo, &layers[c]);
      for (int o = 0; status == GTEXT_CSS_OK && o < 3; o++) {
        for (int k = 0; status == GTEXT_CSS_OK && k < 7; k++) {
          const int i = kLayerOrders[c][k];
          status = gtext_css_layer_declare(layers[c], (GTEXT_CSS_Origin)o, 0, kLayerPaths[i], std::strlen(kLayerPaths[i]), &ids[c][o][i], nullptr);
        }
      }
      if (status == GTEXT_CSS_OK) {
        status = gtext_css_layer_freeze(layers[c]);
      }
    }
    if (status == GTEXT_CSS_OK) {
      GTEXT_CSS_Candidate_Options co = gtext_css_candidate_options_default();
      co.allocator = alloc;
      status = gtext_css_candidates_new(registry(), layers[0], &co, &set);
    }
    for (int c = 0; status == GTEXT_CSS_OK && c < 4; c++) {
      status = gtext_css_candidates_use_layers(set, (uint32_t)c, layers[c]);
    }
    for (size_t i = 0; status == GTEXT_CSS_OK && i < recs.size(); i++) {
      GTEXT_CSS_Cascade_Entry k = keys_of(recs[i], ids);
      GTEXT_CSS_Property_Options po;
      std::memset(&po, 0, sizeof po);
      po.allocator = alloc;
      GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
      const size_t before = gtext_css_candidates_count(set);
      status = gtext_css_candidates_add_text(set, &k, recs[i].name.data(), recs[i].name.size(), recs[i].value.data(), recs[i].value.size(), &po, &outcome,
          nullptr);
      if (status == GTEXT_CSS_E_UNSUPPORTED) {
        status = GTEXT_CSS_OK;  // a value the library cannot judge: nothing was added
      }
      if (status == GTEXT_CSS_OK && gtext_css_candidates_count(set) > before) {
        keys.push_back(k);
      }
    }
    if (status != GTEXT_CSS_OK) {
      if (!allowed(status)) {
        fail("a status adding a declaration must not return", gtext_css_status_string(status), input);
      }
      return status;
    }
    GTEXT_CSS_Cascade_Options o = base;
    o.allocator = alloc;
    status = gtext_css_cascade(set, &o, &c, nullptr);
    if (!allowed(status)) {
      fail("a status a cascade must not return", gtext_css_status_string(status), input);
    }
    if (status != GTEXT_CSS_OK && c) {
      fail("a cascade that did not succeed returned a result", gtext_css_status_string(status), input);
    }
    return status;
  }
};

std::string dump_of(const GTEXT_CSS_Cascaded * c, const std::string & input) {
  std::string out;
  GTEXT_Markup_Sink sink;
  sink.user = &out;
  sink.write = [](void * u, const char * b, size_t n) -> GTEXT_Markup_Status {
    static_cast<std::string *>(u)->append(b, n);
    return GTEXT_MARKUP_OK;
  };
  const GTEXT_CSS_Status st = gtext_css_cascaded_dump(c, &sink);
  if (st != GTEXT_CSS_OK) {
    fail("a cascade could not be dumped", gtext_css_status_string(st), input);
  }
  return out;
}

/* ---- the reference model ---------------------------------------------------------------------------------------- */

struct Cand {
  GTEXT_CSS_Cascade_Entry e;  ///< From the set: the property and the value are the library's; the keys are overwritten from the record (the sanitized input).
  size_t index;
};

struct Model {
  std::map<std::tuple<int, int, GTEXT_CSS_Layer_Id>, int> index_of;  ///< (context, origin, layer id) -> the layer's index 1 to 7 of the harness's table
  long rank[4][8];                                                    ///< by context and layer index; [c][0] is the unlayered place
};

/// The place of each of the layers 1 to 7 when they are declared in `order`: the order of Cascade 5, 6.4.3, written out here and not asked of the library --
/// layers are a tree of the segments of their paths in order of first declaration, a layer's own declarations come after those of its sub-layers (a
/// post-order), and the unlayered declarations are last.
void model_ranks(const int order[7], long out[8]) {
  struct Node {
    std::string name;
    int parent;
  };
  std::vector<Node> nodes;
  int node_of[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  for (int k = 0; k < 7; k++) {
    const int idx = order[k];
    const std::string path = kLayerPaths[idx];
    int parent = -1;
    size_t pos = 0;
    while (pos <= path.size()) {
      size_t dot = path.find('.', pos);
      if (dot == std::string::npos) {
        dot = path.size();
      }
      const std::string name = path.substr(pos, dot - pos);
      int found = -1;
      for (size_t n = 0; n < nodes.size(); n++) {
        if (nodes[n].parent == parent && nodes[n].name == name) {
          found = (int)n;
        }
      }
      if (found < 0) {
        nodes.push_back({name, parent});
        found = (int)nodes.size() - 1;
      }
      parent = found;
      pos = dot + 1;
    }
    node_of[idx] = parent;
  }
  std::vector<long> place(nodes.size(), -1);
  long next = 0;
  /* a post-order: the children of a node (in order of creation) are numbered before it */
  struct Walk {
    const std::vector<Node> & nodes;
    std::vector<long> & place;
    long & next;
    void go(int node) {
      for (size_t n = 0; n < nodes.size(); n++) {
        if (nodes[n].parent == node) {
          go((int)n);
        }
      }
      if (node >= 0) {
        place[(size_t)node] = next++;
      }
    }
  } walk{nodes, place, next};
  walk.go(-1);
  out[0] = next;
  for (int i = 1; i < 8; i++) {
    out[i] = place[(size_t)node_of[i]];
  }
}

/// The model's place of a declaration's layer in its context: 8 for none (after every layer).
long model_rank(const Model & m, const GTEXT_CSS_Cascade_Entry & e) {
  if (e.layer == GTEXT_CSS_LAYER_NONE || e.origin >= GTEXT_CSS_ORIGIN_ANIMATION) {
    return m.rank[e.context & 3][0];
  }
  const auto it = m.index_of.find({(int)(e.context & 3), (int)e.origin, e.layer});
  return it == m.index_of.end() ? -2 : m.rank[e.context & 3][it->second];
}

int tier_rank(const GTEXT_CSS_Cascade_Entry & e) {
  /* ascending: normal user agent, normal user, normal presentational hints ("an independent origin ... between the regular user origin and the author
     origin", 6.5), normal author, animation, important author, important user, important user agent, transition */
  switch (e.origin) {
  case GTEXT_CSS_ORIGIN_USER_AGENT: return e.important ? 7 : 0;
  case GTEXT_CSS_ORIGIN_USER: return e.important ? 6 : 1;
  case GTEXT_CSS_ORIGIN_AUTHOR: return e.important ? 5 : e.presentational_hint ? 2 : 3;
  case GTEXT_CSS_ORIGIN_ANIMATION: return 4;
  default: return 8;
  }
}

bool ruleless(const GTEXT_CSS_Cascade_Entry & e) {
  return e.presentational_hint || e.element_attached || e.origin >= GTEXT_CSS_ORIGIN_ANIMATION;
}

/// -1, 0, 1 as a is weaker than, equal to, stronger than b, criterion by criterion.
int cmp(const Model & m, const Cand & a, const Cand & b) {
  const int ta = tier_rank(a.e), tb = tier_rank(b.e);
  if (ta != tb) {
    return ta < tb ? -1 : 1;
  }
  if (a.e.context != b.e.context) {
    /* "for normal rules the declaration from the outer context wins, and for important rules the declaration from the inner context wins" */
    const bool a_wins = a.e.important ? a.e.context > b.e.context : a.e.context < b.e.context;
    return a_wins ? 1 : -1;
  }
  if (a.e.element_attached != b.e.element_attached) {
    return a.e.element_attached ? 1 : -1;
  }
  {
    /* layers: by the layer order of the context, reversed for important; the unlayered declarations are last */
    const long ra = model_rank(m, a.e), rb = model_rank(m, b.e);
    if (ra != rb) {
      const bool a_later = ra > rb;
      return (a_later != a.e.important) ? 1 : -1;
    }
  }
  const GTEXT_CSS_Specificity za = {0, 0, 0};
  const GTEXT_CSS_Specificity sa = ruleless(a.e) ? za : a.e.specificity, sb = ruleless(b.e) ? za : b.e.specificity;
  if (const int s = gtext_css_specificity_compare(sa, sb)) {
    return s;
  }
  const bool scoped_a = a.e.scoped && !ruleless(a.e), scoped_b = b.e.scoped && !ruleless(b.e);
  if (scoped_a != scoped_b) {
    return scoped_a ? 1 : -1;
  }
  if (scoped_a && a.e.scope_distance != b.e.scope_distance) {
    return a.e.scope_distance < b.e.scope_distance ? 1 : -1;
  }
  if (a.e.sheet != b.e.sheet) {
    return a.e.sheet < b.e.sheet ? -1 : 1;
  }
  if (a.e.rule != b.e.rule) {
    return a.e.rule < b.e.rule ? -1 : 1;
  }
  if (a.e.declaration != b.e.declaration) {
    return a.e.declaration < b.e.declaration ? -1 : 1;
  }
  return a.index < b.index ? -1 : a.index > b.index ? 1 : 0;
}

bool rolls(const GTEXT_CSS_Cascade_Entry & e) {
  return e.outcome == GTEXT_CSS_PROP_RESULT_WIDE &&
      (e.wide == GTEXT_CSS_WIDE_REVERT || e.wide == GTEXT_CSS_WIDE_REVERT_LAYER || e.wide == GTEXT_CSS_WIDE_REVERT_RULE);
}

int level_of(const GTEXT_CSS_Cascade_Entry & e) {
  return e.origin == GTEXT_CSS_ORIGIN_USER_AGENT ? 0 : e.origin == GTEXT_CSS_ORIGIN_USER ? 1 : 2;
}

/// The winner among `cands` (indexes into `all`), by taking the strongest and removing what a rolling keyword says, until one does not roll.
/// Returns the index into `all` or -1; `*rollbacks` counts the rolling keywords passed.
long model_winner(const Model & m, const std::vector<Cand> & all, std::vector<size_t> cands, size_t * rollbacks) {
  *rollbacks = 0;
  for (;;) {
    if (cands.empty()) {
      return -1;
    }
    size_t best = cands[0];
    for (const size_t x : cands) {
      if (cmp(m, all[x], all[best]) > 0) {
        best = x;
      }
    }
    const Cand & c = all[best];
    if (!rolls(c.e)) {
      return (long)best;
    }
    (*rollbacks)++;
    std::vector<size_t> kept;
    GTEXT_CSS_Wide_Keyword how = c.e.wide;
    const bool timed = c.e.origin >= GTEXT_CSS_ORIGIN_ANIMATION;
    if (how == GTEXT_CSS_WIDE_REVERT_RULE && timed) {
      how = GTEXT_CSS_WIDE_REVERT_LAYER;
    }
    if (how == GTEXT_CSS_WIDE_REVERT) {
      if (level_of(c.e) == 0) {
        return -1;
      }
      for (const size_t x : cands) {
        if (level_of(all[x].e) < level_of(c.e)) {
          kept.push_back(x);
        }
      }
    }
    else if (how == GTEXT_CSS_WIDE_REVERT_LAYER) {
      for (const size_t x : cands) {
        const Cand & d = all[x];
        bool removed = false;
        if (timed) {
          removed = d.e.origin == c.e.origin;
        }
        else if (c.e.element_attached) {
          removed = d.e.element_attached && d.e.important == c.e.important && d.e.origin == c.e.origin && d.e.context == c.e.context;
          if (c.e.important) {
            removed = removed || (c.e.origin == GTEXT_CSS_ORIGIN_AUTHOR && d.e.origin == GTEXT_CSS_ORIGIN_ANIMATION) ||
                (d.e.element_attached && !d.e.important && d.e.origin == c.e.origin && d.e.context == c.e.context);
          }
        }
        else {
          /* from here down to the lowest key of the layer's normal level: d is removed if it is not above that lowest key */
          Cand low = c;
          low.e.important = false;
          low.e.element_attached = false;
          low.e.specificity = {0, 0, 0};
          low.e.scoped = false;
          low.e.sheet = low.e.rule = low.e.declaration = 0;
          low.index = 0;
          removed = cmp(m, d, low) >= 0;
        }
        if (!removed) {
          kept.push_back(x);
        }
      }
    }
    else {
      for (const size_t x : cands) {
        const Cand & d = all[x];
        /* a rule's declarations are of both importances */
        const bool same = d.e.origin == c.e.origin && d.e.presentational_hint == c.e.presentational_hint && d.e.element_attached == c.e.element_attached &&
            d.e.context == c.e.context && d.e.sheet == c.e.sheet && d.e.rule == c.e.rule;
        if (!same) {
          kept.push_back(x);
        }
      }
    }
    if (kept.size() == cands.size()) {
      fail("the model removed nothing", "a rolling keyword that rolls back nothing", "");
    }
    cands = kept;
  }
}

bool kw(const GTEXT_CSS_Value * v, const char * w) {
  if (!v || v->kind != GTEXT_CSS_VAL_KEYWORD || !v->text.data) {
    return false;
  }
  const size_t n = std::strlen(w);
  if (v->text.length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char a = v->text.data[i];
    if (a >= 'A' && a <= 'Z') {
      a = (char)(a - 'A' + 'a');
    }
    if (a != w[i]) {
      return false;
    }
  }
  return true;
}

/// The model's flow and winners, and the comparison with the library's, for the candidates of `set` (with the keys of the records).
void check_model(const Pipe & p, const GTEXT_CSS_Flow & parent, bool losers_kept, const std::string & dump, const std::string & input) {
  const GTEXT_CSS_Registry * reg = registry();
  Model m;
  for (int c = 0; c < 4; c++) {
    model_ranks(kLayerOrders[c], m.rank[c]);
    for (int o = 0; o < 3; o++) {
      for (int i = 1; i < 8; i++) {
        m.index_of[{c, o, p.ids[c][o][i]}] = i;
      }
    }
  }
  std::vector<Cand> all;
  const size_t n = gtext_css_candidates_count(p.set);
  for (size_t i = 0; i < n; i++) {
    Cand c;
    gtext_css_candidates_entry(p.set, i, &c.e);
    c.index = i;
    if (c.e.group >= p.keys.size()) {
      fail("a candidate has a group no record made", "", input);
    }
    /* the keys are the record's, as the model reads them: what the library returns for a hint's or a style attribute's specificity is not trusted */
    const GTEXT_CSS_Cascade_Entry & k = p.keys[c.e.group];
    c.e.origin = k.origin;
    c.e.important = k.important;
    c.e.presentational_hint = k.presentational_hint;
    c.e.element_attached = k.element_attached;
    c.e.layer = k.layer;
    c.e.context = k.context;
    c.e.scoped = k.scoped;
    c.e.scope_distance = k.scope_distance;
    c.e.specificity = k.specificity;
    c.e.sheet = k.sheet;
    c.e.rule = k.rule;
    c.e.declaration = k.declaration;
    all.push_back(c);
  }
  const GTEXT_CSS_Property_Id all_id = gtext_css_registry_find_property(reg, "all", 3);
  const GTEXT_CSS_Property_Id wm = gtext_css_registry_find_property(reg, "writing-mode", 12);
  const GTEXT_CSS_Property_Id dir = gtext_css_registry_find_property(reg, "direction", 9);
  const GTEXT_CSS_Property_Id to = gtext_css_registry_find_property(reg, "text-orientation", 16);
  const GTEXT_CSS_Property_Id bidi = gtext_css_registry_find_property(reg, "unicode-bidi", 12);
  std::vector<size_t> all_cands;
  for (size_t i = 0; i < all.size(); i++) {
    if (all[i].e.property == all_id) {
      all_cands.push_back(i);
    }
  }
  auto applies = [&](GTEXT_CSS_Property_Id unit) { return unit != dir && unit != bidi; };
  auto own_of = [&](GTEXT_CSS_Property_Id p_, const GTEXT_CSS_Flow * flow) {
    std::vector<size_t> out;
    for (size_t i = 0; i < all.size(); i++) {
      if (all[i].e.property == all_id || all[i].e.custom_name.data) {
        continue;
      }
      GTEXT_CSS_Property_Id unit = all[i].e.property;
      GTEXT_CSS_Property_Info info;
      if (flow && gtext_css_registry_property_info(reg, unit, &info) && info.logical_group) {
        GTEXT_CSS_Property_Id phys;
        if (gtext_css_logical_map(reg, unit, flow, &phys) == GTEXT_CSS_OK) {
          unit = phys;
        }
      }
      if (unit == p_) {
        out.push_back(i);
      }
    }
    return out;
  };
  /* the flow: the three properties, cascaded first */
  GTEXT_CSS_Flow flow = parent;
  {
    const GTEXT_CSS_Property_Id fp[3] = {wm, dir, to};
    int v[3] = {(int)parent.writing_mode, (int)parent.direction, (int)parent.text_orientation};
    for (int f = 0; f < 3; f++) {
      std::vector<size_t> cands = own_of(fp[f], nullptr);
      if (applies(fp[f])) {
        cands.insert(cands.end(), all_cands.begin(), all_cands.end());
      }
      size_t rb;
      const long w = model_winner(m, all, cands, &rb);
      if (w < 0) {
        continue;
      }
      const GTEXT_CSS_Cascade_Entry & e = all[(size_t)w].e;
      if (e.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
        v[f] = e.wide == GTEXT_CSS_WIDE_INITIAL ? 0 : v[f];
      }
      else if (e.outcome == GTEXT_CSS_PROP_RESULT_VALID) {
        const GTEXT_CSS_Value * root = gtext_css_value_set_root(e.set);
        if (f == 0) {
          v[0] = kw(root, "vertical-rl") ? 1 : kw(root, "vertical-lr") ? 2 : kw(root, "sideways-rl") ? 3 : kw(root, "sideways-lr") ? 4 : 0;
        }
        else if (f == 1) {
          v[1] = kw(root, "rtl") ? 1 : 0;
        }
        else {
          v[2] = kw(root, "upright") ? 1 : kw(root, "sideways") ? 2 : 0;
        }
      }
    }
    flow.writing_mode = (GTEXT_CSS_Writing_Mode)v[0];
    flow.direction = (GTEXT_CSS_Direction)v[1];
    flow.text_orientation = (GTEXT_CSS_Text_Orientation)v[2];
  }
  const GTEXT_CSS_Flow got = gtext_css_cascaded_flow(p.c);
  if (got.writing_mode != flow.writing_mode || got.direction != flow.direction || got.text_orientation != flow.text_orientation) {
    fail("the flow is not the model's", "", input);
  }
  /* the winners, by unit */
  struct Win {
    size_t index, rollbacks;
  };
  std::map<std::string, Win> want;
  std::map<std::string, std::vector<size_t>> units;  // the candidates of each unit, for the losers
  const size_t count = gtext_css_registry_property_count(reg);
  for (size_t id = 1; id <= count; id++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(reg, (GTEXT_CSS_Property_Id)id, &info) || (info.flags & (GTEXT_CSS_PROP_SHORTHAND | GTEXT_CSS_PROP_ALIAS | GTEXT_CSS_PROP_CUSTOM))) {
      continue;
    }
    GTEXT_CSS_Property_Id unit = (GTEXT_CSS_Property_Id)id;
    if (info.logical_group) {
      GTEXT_CSS_Property_Id phys;
      if (gtext_css_logical_map(reg, unit, &flow, &phys) == GTEXT_CSS_OK) {
        unit = phys;
      }
    }
    if (unit != (GTEXT_CSS_Property_Id)id) {
      continue;
    }
    std::vector<size_t> cands = own_of(unit, &flow);
    if (applies(unit)) {
      cands.insert(cands.end(), all_cands.begin(), all_cands.end());
    }
    if (cands.empty()) {
      continue;
    }
    size_t rb;
    const long w = model_winner(m, all, cands, &rb);
    const std::string key = "p" + std::to_string(id);
    units[key] = cands;
    if (w >= 0) {
      want[key] = {(size_t)w, rb};
    }
  }
  std::map<std::string, std::vector<size_t>> by_name;
  for (size_t i = 0; i < all.size(); i++) {
    if (all[i].e.custom_name.data) {
      by_name["c" + std::string(all[i].e.custom_name.data, all[i].e.custom_name.length)].push_back(i);
    }
  }
  for (auto & kv : by_name) {
    size_t rb;
    const long w = model_winner(m, all, kv.second, &rb);
    units[kv.first] = kv.second;
    if (w >= 0) {
      want[kv.first] = {(size_t)w, rb};
    }
  }
  std::map<std::string, Win> have;
  std::map<std::string, size_t> win_index;
  for (size_t i = 0; i < gtext_css_cascaded_count(p.c); i++) {
    GTEXT_CSS_Cascade_Entry e;
    size_t rb = 0;
    gtext_css_cascaded_at(p.c, i, &e, &rb);
    std::string key;
    if (e.custom_name.data) {
      key = "c" + std::string(e.custom_name.data, e.custom_name.length);
    }
    else {
      GTEXT_CSS_Property_Id unit = e.property;
      GTEXT_CSS_Property_Info info;
      if (gtext_css_registry_property_info(reg, unit, &info) && info.logical_group) {
        GTEXT_CSS_Property_Id phys;
        if (gtext_css_logical_map(reg, unit, &flow, &phys) == GTEXT_CSS_OK) {
          unit = phys;
        }
      }
      key = "p" + std::to_string(unit);
    }
    if (have.count(key)) {
      fail("a unit has two winners", key, input);
    }
    have[key] = {e.index, rb};
    win_index[key] = i;
  }
  for (const auto & kv : want) {
    const auto it = have.find(kv.first);
    if (it == have.end()) {
      fail("the model has a winner the cascade lacks", kv.first + " model candidate " + std::to_string(kv.second.index), input);
    }
    if (it->second.index != kv.second.index) {
      fail("the winner is not the model's", kv.first + " cascade " + std::to_string(it->second.index) + " model " + std::to_string(kv.second.index), input);
    }
    if (it->second.rollbacks != kv.second.rollbacks) {
      fail("the rollbacks are not the model's", kv.first, input);
    }
  }
  for (const auto & kv : have) {
    if (!want.count(kv.first)) {
      fail("the cascade has a winner the model lacks", kv.first, input);
    }
  }
  /* the losers, when kept */
  for (const auto & kv : win_index) {
    const size_t i = kv.second;
    const size_t lc = gtext_css_cascaded_loser_count(p.c, i);
    if (!losers_kept) {
      if (lc != 0) {
        fail("losers are kept that were not asked for", kv.first, input);
      }
      continue;
    }
    if (lc != units[kv.first].size() - 1) {
      fail("the number of losers is not the other candidates of the unit", kv.first, input);
    }
    const Cand & winner = all[want[kv.first].index];
    for (size_t l = 0; l < lc; l++) {
      GTEXT_CSS_Cascade_Entry e;
      GTEXT_CSS_Loss_Reason why;
      gtext_css_cascaded_loser(p.c, i, l, &e, &why);
      if (e.index == winner.index) {
        fail("the winner is among its losers", kv.first, input);
      }
      const bool above = cmp(m, all[e.index], winner) > 0;
      if (above != (why == GTEXT_CSS_LOSS_ROLLED_BACK)) {
        fail("a loser's reason is wrong", kv.first + " loser " + std::to_string(e.index), input);
      }
    }
  }
  /* the fields of every winner as the record gave them (the keys a hint, a style attribute and a timed declaration cannot use are not kept), and the line of the
     dump that writes them */
  std::vector<std::string> lines;
  for (size_t at = dump.find('\n') + 1; at < dump.size();) {
    size_t nl = dump.find('\n', at);
    if (nl == std::string::npos) {
      nl = dump.size();
    }
    lines.push_back(dump.substr(at, nl - at));
    at = nl + 1;
  }
  if (lines.size() != gtext_css_cascaded_count(p.c)) {
    fail("the dump does not have a line for each winner", "", input);
  }
  for (size_t i = 0; i < gtext_css_cascaded_count(p.c); i++) {
    GTEXT_CSS_Cascade_Entry e;
    size_t rb = 0;
    gtext_css_cascaded_at(p.c, i, &e, &rb);
    const GTEXT_CSS_Cascade_Entry & k = p.keys[e.group];
    const bool rl = ruleless(k);
    const bool scoped = k.scoped && !rl;
    const GTEXT_CSS_Specificity spec = rl ? GTEXT_CSS_Specificity{0, 0, 0} : k.specificity;
    const bool is_all = all[e.index].e.property == all_id;
    if (e.origin != k.origin || e.important != k.important || e.presentational_hint != k.presentational_hint || e.element_attached != k.element_attached ||
        e.context != k.context || e.layer != k.layer || e.scoped != scoped || e.scope_distance != (scoped ? k.scope_distance : 0) ||
        gtext_css_specificity_compare(e.specificity, spec) != 0 || e.sheet != k.sheet || e.rule != k.rule || e.declaration != k.declaration ||
        e.from_all != is_all) {
      fail("a winner's fields are not its record's", "winner " + std::to_string(i), input);
    }
    std::string layer = "-";
    if (k.layer != GTEXT_CSS_LAYER_NONE) {
      const auto it = m.index_of.find({(int)(k.context & 3), (int)k.origin, k.layer});
      layer = it == m.index_of.end() ? "?" : kLayerPaths[it->second];
    }
    const std::string suffix = std::string(k.important ? " !important" : "") + " [" + gtext_css_origin_name(k.origin) + (k.presentational_hint ? " hint" : "") +
        (k.element_attached ? " attached" : "") + (is_all ? " all" : "") + " layer=" + layer + " ctx=" + std::to_string(k.context) + " spec=" +
        std::to_string(spec.a) + "," + std::to_string(spec.b) + "," + std::to_string(spec.c) + " scope=" + (scoped ? std::to_string(k.scope_distance) : "-") +
        " order=" + std::to_string(k.sheet) + "." + std::to_string(k.rule) + "." + std::to_string(k.declaration) + " rb=" + std::to_string(rb) + "]";
    if (lines[i].size() < suffix.size() || lines[i].compare(lines[i].size() - suffix.size(), suffix.size(), suffix) != 0) {
      fail("the dump line of a winner does not write its fields", lines[i] + "\n  wanted ..." + suffix, input);
    }
  }
}

/* ---- the properties -------------------------------------------------------------------------------------------- */

GTEXT_CSS_Compute_Context * shared_context() {
  static GTEXT_CSS_Compute_Context * c = nullptr;
  if (!c) {
    GTEXT_CSS_Compute_Result r{};
    if (gtext_css_compute_context_new(registry(), nullptr, nullptr, &c, &r) != GTEXT_CSS_OK) {
      fail("a context could not be made", r.name, "");
    }
  }
  return c;
}

void check_block(const Pipe & p, const std::string & input) {
  GTEXT_CSS_Declaration_Block * block = nullptr;
  GTEXT_CSS_Declaration_Options bo = gtext_css_declaration_options_default();
  const GTEXT_CSS_Status st = gtext_css_cascaded_to_block(p.c, &bo, &block);
  if (st != GTEXT_CSS_OK) {
    fail("the block of the winners could not be made", gtext_css_status_string(st), input);
  }
  if (gtext_css_block_count(block) != gtext_css_cascaded_count(p.c)) {
    fail("the block does not have one item for each winner", "", input);
  }
  for (size_t i = 0; i < gtext_css_block_count(block); i++) {
    GTEXT_CSS_Declaration_Item it;
    gtext_css_block_item(block, i, &it);
    if (it.outcome == GTEXT_CSS_PROP_RESULT_WIDE && (it.wide == GTEXT_CSS_WIDE_REVERT || it.wide == GTEXT_CSS_WIDE_REVERT_LAYER || it.wide == GTEXT_CSS_WIDE_REVERT_RULE ||
            it.wide == GTEXT_CSS_WIDE_REVERT_RULE)) {
      fail("a rolling keyword is in the block of the winners", "", input);
    }
  }
  {
    GTEXT_CSS_Computed_Style * style = nullptr;
    GTEXT_CSS_Compute_Result r{};
    const GTEXT_CSS_Status cs = gtext_css_compute_style(shared_context(), block, nullptr, nullptr, &style, &r);
    if (!allowed(cs) || cs == GTEXT_CSS_E_INVALID) {
      fail("a status computing the winners must not return", std::string(gtext_css_status_string(cs)) + " " + r.name + " " + (r.detail ? r.detail : ""), input);
    }
    gtext_css_computed_free(style);
  }
  gtext_css_block_free(block);
}

void shuffle_records(std::vector<Rec> & recs, const std::string & input) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char ch : input) {
    h = (h ^ ch) * 1099511628211ULL;
  }
  std::mt19937_64 rng(h);
  std::shuffle(recs.begin(), recs.end(), rng);
}

void check(const std::vector<Rec> & recs, uint8_t o0, size_t fault, const std::string & input) {
  /* A form feed or a carriage return inside a string, or after a backslash, is a bad-string or bad-url token, which no <declaration-value> may hold; a value with
     such a token in it is nevertheless taken as valid or pending (a gap of the property parser that the computed-value fuzzer skips too), and the dump of such a
     value has a line break in it. Those inputs say nothing about the cascade. */
  for (const Rec & r : recs) {
    if (r.value.find_first_of("\n\r\f") != std::string::npos && r.value.find_first_of("\"'\\") != std::string::npos) {
      return;
    }
  }
  GTEXT_CSS_Flow parent;
  std::memset(&parent, 0, sizeof parent);
  parent.direction = (o0 & 2) ? GTEXT_CSS_DIRECTION_RTL : GTEXT_CSS_DIRECTION_LTR;
  parent.writing_mode = (o0 & 4) ? GTEXT_CSS_WRITING_MODE_VERTICAL_RL : GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB;
  GTEXT_CSS_Cascade_Options opt;
  std::memset(&opt, 0, sizeof opt);
  opt.parent_flow = &parent;
  opt.keep_losers = (o0 & 1) != 0;
  opt.hash_seed = 7;
  std::string first;
  uint64_t first_hash = 0;
  {
    FaultAllocator fa;
    Pipe p(&fa.alloc);
    const GTEXT_CSS_Status st = p.build(recs, opt, input);
    if (st != GTEXT_CSS_OK) {
      if (st == GTEXT_CSS_E_LIMIT_CANDIDATES) {
        return;
      }
      fail("a cascade of valid records did not succeed", gtext_css_status_string(st), input);
    }
    first = dump_of(p.c, input);
    check_model(p, parent, opt.keep_losers, first, input);
    if (gtext_css_cascaded_hash(p.c, &first_hash) != GTEXT_CSS_OK) {
      fail("a cascade could not be hashed", "", input);
    }
    uint64_t again = 0;
    if (gtext_css_cascaded_hash(p.c, &again) != GTEXT_CSS_OK || again != first_hash || dump_of(p.c, input) != first) {
      fail("the dump of a cascade is not stable", "", input);
    }
    if (o0 & 8) {
      check_block(p, input);
    }
    /* cascading the same candidates again (the set is sealed, not consumed) gives the same */
    GTEXT_CSS_Cascaded * second = nullptr;
    GTEXT_CSS_Cascade_Options again_options = opt;
    again_options.allocator = &fa.alloc;
    if (gtext_css_cascade(p.set, &again_options, &second, nullptr) != GTEXT_CSS_OK || dump_of(second, input) != first) {
      fail("a second cascade of the same candidates differs", "", input);
    }
    gtext_css_cascaded_free(second);
  }
  /* the order of the candidates does not matter, when no two have the same order of appearance */
  {
    std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> seen;
    bool distinct = true;
    GTEXT_CSS_Layer_Id ids[4][3][8];
    std::memset(ids, 0, sizeof ids);
    for (const Rec & r : recs) {
      const GTEXT_CSS_Cascade_Entry e = keys_of(r, ids);
      /* a shorthand and a longhand it sets are different declarations; two records with the same triple are not told apart */
      distinct = distinct && seen.insert({e.sheet, e.rule, e.declaration, 0}).second;
    }
    if (distinct && recs.size() > 1) {
      std::vector<Rec> shuffled = recs;
      shuffle_records(shuffled, input);
      FaultAllocator fa;
      Pipe p(&fa.alloc);
      if (p.build(shuffled, opt, input) != GTEXT_CSS_OK) {
        fail("a shuffled cascade did not succeed", "", input);
      }
      if (dump_of(p.c, input) != first) {
        fail("the winners depend on the order of the candidates", "", input);
      }
    }
  }
  if (!fault) {
    return;
  }
  /* an allocation failed: E_OOM and nothing leaked, or the answer of the run that was not reached */
  FaultAllocator fa;
  std::string got;
  GTEXT_CSS_Status st;
  {
    Pipe p(&fa.alloc);
    fa.arm(fault);
    st = p.build(recs, opt, input);
    if (st == GTEXT_CSS_OK && (o0 & 8)) {
      GTEXT_CSS_Declaration_Block * block = nullptr;
      GTEXT_CSS_Declaration_Options bo = gtext_css_declaration_options_default();
      bo.allocator = &fa.alloc;
      st = gtext_css_cascaded_to_block(p.c, &bo, &block);
      gtext_css_block_free(block);
    }
    if (st == GTEXT_CSS_OK) {
      std::string out;
      GTEXT_Markup_Sink sink;
      sink.user = &out;
      sink.write = [](void * u, const char * b, size_t n) -> GTEXT_Markup_Status {
        static_cast<std::string *>(u)->append(b, n);
        return GTEXT_MARKUP_OK;
      };
      st = gtext_css_cascaded_dump(p.c, &sink);
      got = out;
    }
    fa.disarm();
  }
  if (fa.failed && st != GTEXT_CSS_E_OOM) {
    fail("an allocation failed and nothing said so", gtext_css_status_string(st), input);
  }
  if (!fa.live.empty()) {
    fail("a cascade leaked", std::to_string(fa.live.size()) + " blocks", input);
  }
  if (fa.bad_frees) {
    fail("a cascade freed what it did not make", "", input);
  }
  if (st == GTEXT_CSS_OK && !fa.failed && got != first) {
    fail("the cascade under a fault allocator that did not fire differs", "", input);
  }
  if (st != GTEXT_CSS_OK && st != GTEXT_CSS_E_OOM && !allowed(st)) {
    fail("a status a cascade must not return", gtext_css_status_string(st), input);
  }
}

int entry(const uint8_t * data, size_t size) {
  if (size < 4 || size > 4096) {
    return 0;
  }
  const std::string input(reinterpret_cast<const char *>(data), size);
  const std::string text(reinterpret_cast<const char *>(data) + 4, size - 4);
  const std::vector<Rec> recs = parse(text);
  check(recs, data[0], data[3], input);
  return 0;
}

}  // namespace css_casfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_casfuzz::entry(data, size);
}
