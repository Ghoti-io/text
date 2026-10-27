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
 * The cascade against a reference written from the specification and sharing nothing with the library but the public types: 20,000 random elements
 * (seeded, so a failure is reproduced by its number), each a handful of declarations of one property with random origins, importance, contexts, layers (three
 * contexts that order the same layer names differently, each with a registry of its own), element-attached and hint declarations, specificity, scope, order, and
 * every rolling keyword. Nothing is left out of the population: every combination the generator makes is compared.
 *
 * The reference sorts two declarations criterion by criterion as Cascade 5, 6.1 and Cascade 6, 3.1 list them (the presentational hint origin being its own
 * origin "between the regular user origin and the author origin", 6.5) and resolves a rolling keyword by taking out what the keyword says and starting again:
 *
 *   revert        "Rolls back the cascaded value to the previous origin": every declaration of the origin level and above (the hint, animation and transition
 *                 origins being part of the author origin for it); a user-agent revert is unset;
 *   revert-layer  "as if no rules were specified in the current cascade layer - or between its normal and important levels in the cascade": every declaration not
 *                 above the lowest declaration of the layer's normal level; for important element-attached styles "only the element-attached styles and the
 *                 intervening animation origin"; the animation origin is its own layer;
 *   revert-rule   "as if the current style rule had not been present at all": every declaration of the rule, of either importance.
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_cascade_test_support.h"

using namespace cas;

namespace {

/// The layers of each context, as declared (the order of declaration), and the place each has in the order the specification gives: a post-order of the tree of
/// the names, children before their parent, in the order the names were first declared; the unlayered declarations last.
struct ContextLayers {
  std::vector<const char *> declared;  // paths, in order of declaration
  std::vector<int> rank;               // the place of each, from 0
};

// context 0: a, b, c.   context 1: c, b, a.   context 2: b, a, b.x, c (b.x comes before b, whose own declarations are after its sub-layers).
const ContextLayers kContexts[3] = {
    {{"a", "b", "c"}, {0, 1, 2}},
    {{"c", "b", "a"}, {0, 1, 2}},
    {{"b", "a", "b.x", "c"}, {1, 2, 0, 3}},
};

int unlayered_rank(int context) {
  return (int)kContexts[context].declared.size();
}

struct Rule {
  int origin = 2;  // 0 user agent, 1 user, 2 author, 3 animation, 4 transition
  int ctx = 0;
  int layer = -1;  // an index into the context's declared layers, or -1
  int kind = 0;    // 0 a style rule, 1 a presentational hint, 2 element-attached
  int sa = 0, sb = 0, sc = 0;
  bool scoped = false;
  int dist = 0;
};

struct Decl {
  Rule r;
  int sheet = 0, rule = 0, decl = 0;
  bool important = false;
  int wide = 0;  // 0 none, 1 revert, 2 revert-layer, 3 revert-rule
};

bool ruleless(const Decl & d) {
  return d.r.kind != 0 || d.r.origin >= 3;
}

/// The origin of Cascade 5, 6.1 and 6.5 in ascending precedence: 0 normal user agent, 1 normal user, 2 normal presentational hints, 3 normal author, 4 animation,
/// 5 important author, 6 important user, 7 important user agent, 8 transition.
int origin_rank(const Decl & d) {
  switch (d.r.origin) {
  case 0: return d.important ? 7 : 0;
  case 1: return d.important ? 6 : 1;
  case 2: return d.important ? 5 : d.r.kind == 1 ? 2 : 3;
  case 3: return 4;
  default: return 8;
  }
}

int sgn(long a, long b) {
  return a < b ? -1 : a > b ? 1 : 0;
}

/// Positive when x is stronger than y; the last criterion is the position in the list, so that no two are equal.
int stronger(const Decl & x, const Decl & y, int xi, int yi) {
  int d;
  if ((d = sgn(origin_rank(x), origin_rank(y)))) {
    return d;
  }
  /* context: "for normal rules the declaration from the outer context wins, and for important rules the declaration from the inner context wins" */
  if ((d = sgn(x.r.ctx, y.r.ctx))) {
    return x.important ? d : -d;
  }
  /* "declarations that are attached directly to an element ... take precedence over declarations the same importance that are mapped via style rule" */
  if ((d = sgn(x.r.kind == 2, y.r.kind == 2))) {
    return d;
  }
  /* layers: "for normal rules the declaration whose cascade layer is latest in the layer order wins, and for important rules ... earliest" */
  auto place = [](const Decl & c) {
    if (c.r.origin >= 3 || c.r.origin < 2 || c.r.kind != 0 || c.r.layer < 0) {
      return unlayered_rank(c.r.ctx);
    }
    return kContexts[c.r.ctx].rank[(size_t)c.r.layer];
  };
  if ((d = sgn(place(x), place(y)))) {
    return x.important ? -d : d;
  }
  const bool rx = ruleless(x), ry = ruleless(y);
  if ((d = sgn(rx ? 0 : x.r.sa, ry ? 0 : y.r.sa))) return d;
  if ((d = sgn(rx ? 0 : x.r.sb, ry ? 0 : y.r.sb))) return d;
  if ((d = sgn(rx ? 0 : x.r.sc, ry ? 0 : y.r.sc))) return d;
  const bool sx = !rx && x.r.scoped, sy = !ry && y.r.scoped;
  if (sx != sy) {
    return sx ? 1 : -1;
  }
  if (sx && (d = sgn(x.r.dist, y.r.dist))) {
    return -d;
  }
  if ((d = sgn(x.sheet, y.sheet))) return d;
  if ((d = sgn(x.rule, y.rule))) return d;
  if ((d = sgn(x.decl, y.decl))) return d;
  return sgn(xi, yi);
}

int level(const Decl & d) {
  return d.r.origin == 0 ? 0 : d.r.origin == 1 ? 1 : 2;
}

/// The winner among `v`, or -1 for none, by taking the strongest and removing what a rolling keyword says until one does not roll.
int reference(const std::vector<Decl> & v) {
  std::vector<int> live;
  for (size_t i = 0; i < v.size(); i++) {
    live.push_back((int)i);
  }
  for (;;) {
    int best = -1;
    for (int i : live) {
      if (best < 0 || stronger(v[(size_t)i], v[(size_t)best], i, best) > 0) {
        best = i;
      }
    }
    if (best < 0) {
      return -1;
    }
    const Decl & c = v[(size_t)best];
    if (!c.wide) {
      return best;
    }
    int how = c.wide;
    if (how == 3 && c.r.origin >= 3) {
      how = 2; /* "The revert-rule keyword behaves like revert-layer in the animation origin." */
    }
    if (how == 1 && level(c) == 0) {
      return -1; /* "user-agent origin: Equivalent to unset." */
    }
    std::vector<int> kept;
    for (int i : live) {
      const Decl & d = v[(size_t)i];
      bool removed = false;
      if (how == 1) {
        removed = level(d) >= level(c);
      }
      else if (how == 3) {
        removed = d.r.origin == c.r.origin && d.r.kind == c.r.kind && d.r.ctx == c.r.ctx && d.sheet == c.sheet && d.rule == c.rule;
      }
      else if (c.r.origin >= 3) {
        removed = d.r.origin == c.r.origin; /* the animation (and transition) origin is its own layer */
      }
      else if (c.r.kind == 2) {
        const bool same_attached = d.r.kind == 2 && d.r.origin == c.r.origin && d.r.ctx == c.r.ctx;
        removed = same_attached && d.important == c.important;
        if (c.important) {
          removed = removed || (same_attached && !d.important) || (c.r.origin == 2 && d.r.origin == 3);
        }
      }
      else {
        /* from the declaration down to the lowest declaration of the layer's normal level */
        Decl low = c;
        low.important = false;
        low.r.kind = c.r.kind == 1 ? 1 : 0;
        low.r.sa = low.r.sb = low.r.sc = 0;
        low.r.scoped = false;
        low.sheet = low.rule = low.decl = 0;
        removed = stronger(d, low, i, -1) >= 0;
      }
      if (!removed) {
        kept.push_back(i);
      }
    }
    if (kept.size() == live.size()) {
      ADD_FAILURE() << "the reference removed nothing";
      return -2;
    }
    live = kept;
  }
}

TEST(CssCascadeReference, TwentyThousandRandomElementsAgreeWithTheSpecification) {
  const char * const seed_env = std::getenv("GTEXT_CASCADE_SEED");
  std::mt19937_64 g(seed_env ? (uint64_t)std::atoll(seed_env) : 20261027ULL);
  auto rnd = [&](int n) { return (int)(g() % (uint64_t)n); };
  size_t compared = 0, none = 0, rolled = 0;
  for (int iter = 0; iter < 20000; iter++) {
    SCOPED_TRACE("iteration " + std::to_string(iter));
    Rule table[2][3];
    for (auto & row : table) {
      for (Rule & r : row) {
        r.origin = rnd(5);
        r.ctx = rnd(3);
        r.kind = r.origin == 2 ? (rnd(4) == 0 ? 1 : rnd(4) == 0 ? 2 : 0) : (r.origin <= 1 && rnd(6) == 0 ? 2 : 0);
        r.layer = -1;
        if (r.origin == 2 && r.kind == 0) {
          r.layer = rnd((int)kContexts[r.ctx].declared.size() + 1) - 1;
        }
        r.sa = rnd(2);
        r.sb = rnd(3);
        r.sc = rnd(3);
        r.scoped = rnd(3) == 0;
        r.dist = rnd(3);
      }
    }
    std::vector<Decl> v;
    std::vector<std::array<int, 3>> used;
    const int n = 2 + rnd(13);
    for (int k = 0; k < n; k++) {
      Decl d;
      d.sheet = rnd(2);
      d.rule = rnd(3);
      d.decl = rnd(3);
      const std::array<int, 3> key{d.sheet, d.rule, d.decl};
      bool dup = false;
      for (const auto & u : used) {
        dup = dup || u == key;
      }
      if (dup) {
        continue;
      }
      used.push_back(key);
      d.r = table[d.sheet][d.rule % 3];
      /* a rule's attributes are the rule's: the table is by (sheet, rule) */
      d.important = d.r.origin <= 2 && d.r.kind != 1 && rnd(3) == 0;
      d.wide = rnd(3) == 0 ? 1 + rnd(3) : 0;
      v.push_back(d);
    }
    /* the library */
    Layers lay[3];
    GTEXT_CSS_Layer_Id ids[3][4] = {};
    for (int c = 0; c < 3; c++) {
      for (size_t i = 0; i < kContexts[c].declared.size(); i++) {
        ids[c][i] = lay[c].declare(kContexts[c].declared[i]);
      }
      lay[c].frozen();
    }
    Set set;
    for (int c = 0; c < 3; c++) {
      ASSERT_EQ(gtext_css_candidates_use_layers(set.s, (uint32_t)c, lay[c].r), GTEXT_CSS_OK);
    }
    static const char * const words[] = {"", "revert", "revert-layer", "revert-rule"};
    for (size_t i = 0; i < v.size(); i++) {
      const Decl & d = v[i];
      K k;
      k.e.origin = (GTEXT_CSS_Origin)d.r.origin;
      k.e.important = d.important;
      k.e.presentational_hint = d.r.kind == 1;
      k.e.element_attached = d.r.kind == 2;
      k.e.context = (uint32_t)d.r.ctx;
      if (d.r.layer >= 0) {
        k.e.layer = ids[d.r.ctx][d.r.layer];
      }
      k.e.specificity = {(uint32_t)d.r.sa, (uint32_t)d.r.sb, (uint32_t)d.r.sc};
      k.e.scoped = d.r.scoped;
      k.e.scope_distance = (uint32_t)d.r.dist;
      k.e.sheet = (uint32_t)d.sheet;
      k.e.rule = (uint32_t)d.rule;
      k.e.declaration = (uint32_t)d.decl;
      k.e.rule_id = i + 1;
      set.add("z-index", d.wide ? words[d.wide] : std::to_string(i), k);
      ASSERT_EQ(set.status, GTEXT_CSS_OK);
    }
    Out out(set);
    ASSERT_EQ(out.status, GTEXT_CSS_OK);
    const int want = reference(v);
    ASSERT_NE(want, -2);
    GTEXT_CSS_Cascade_Entry e;
    int got = -1;
    if (gtext_css_cascaded_find(out.c, find(R(), "z-index"), nullptr, 0, &e)) {
      got = (int)e.rule_id - 1;
    }
    compared++;
    none += want < 0;
    for (const Decl & d : v) {
      rolled += d.wide != 0;
    }
    if (got != want) {
      std::string dump;
      for (size_t i = 0; i < v.size(); i++) {
        const Decl & d = v[i];
        char line[200];
        std::snprintf(line, sizeof line, "  #%zu origin %d ctx %d layer %d kind %d imp %d spec %d.%d.%d scope %d/%d wide %d order %d.%d.%d\n", i, d.r.origin,
            d.r.ctx, d.r.layer, d.r.kind, d.important, d.r.sa, d.r.sb, d.r.sc, d.r.scoped, d.r.dist, d.wide, d.sheet, d.rule, d.decl);
        dump += line;
      }
      FAIL() << "the library chose #" << got << " and the reference #" << want << "\n" << dump;
    }
  }
  EXPECT_EQ(compared, 20000u);
  EXPECT_GT(none, 500u) << "some elements end with no declared value";
  EXPECT_GT(rolled, 10000u) << "and many have rolling declarations";
}

} // namespace
