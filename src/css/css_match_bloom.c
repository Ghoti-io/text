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
 * The ancestor Bloom filter of the matcher (css_match.h): a counting filter of the ids, classes and local names of the ancestors of the element being visited, kept by
 * gtext_css_selector_match_walk(). A selector whose descendant or child combinators name an id, a class or a type that no ancestor has cannot match, and is rejected without
 * matching it. The filter may say "maybe" for something that is not there and never "no" for something that is: a false reject would change an answer. A counter that
 * reaches its maximum stays there (it is never decremented), which is more "maybes" and never fewer.
 */

#include <string.h>

#include "css_match_internal.h"

#define BLOOM_BITS 13
#define BLOOM_SIZE ((size_t)1 << BLOOM_BITS)

enum { KEY_NAME = 1, KEY_ID = 2, KEY_CLASS = 3 };

static uint32_t key_hash(unsigned kind, const char * s, size_t n, bool fold) {
  uint32_t h = 2166136261u ^ kind;
  h *= 16777619u;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (fold) {
      c = css_sel_lower(c);
    }
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

static void bloom_adjust(struct GTEXT_CSS_Selector_Match_Context * ctx, uint32_t h, int delta) {
  const size_t i1 = h & (BLOOM_SIZE - 1);
  const size_t i2 = ((h >> 13) ^ (h * 2654435761u >> 11)) & (BLOOM_SIZE - 1);
  const size_t at[2] = {i1, i2};
  for (int k = 0; k < 2; k++) {
    uint8_t * c = &ctx->bloom[at[k]];
    if (delta > 0) {
      if (*c != 255) {
        (*c)++;
      }
    }
    else if (*c != 255 && *c != 0) {
      (*c)--;
    }
  }
}

static bool bloom_has(const struct GTEXT_CSS_Selector_Match_Context * ctx, uint32_t h) {
  const size_t i1 = h & (BLOOM_SIZE - 1);
  const size_t i2 = ((h >> 13) ^ (h * 2654435761u >> 11)) & (BLOOM_SIZE - 1);
  return ctx->bloom[i1] != 0 && ctx->bloom[i2] != 0;
}

void css_bloom_clear(struct GTEXT_CSS_Selector_Match_Context * ctx) {
  if (ctx->bloom) {
    memset(ctx->bloom, 0, BLOOM_SIZE);
  }
  ctx->bloom_live = false;
  ctx->bloom_for = NULL;
  ctx->bloom_depth = 0;
}

static bool bloom_element(struct GTEXT_CSS_Selector_Match_Context * ctx, El element, int delta) {
  if (!ctx->bloom) {
    ctx->bloom = gtext_allocator_calloc(&ctx->alloc, BLOOM_SIZE, 1);
    if (!ctx->bloom) {
      return css_match_fail(ctx, GTEXT_CSS_E_OOM);
    }
  }
  GTEXT_CSS_String s;
  if ((ctx->avail >> GTEXT_CSS_ELEMENT_CB_LOCAL_NAME & 1u) && ctx->iface.local_name(ctx->iface.user, element, &s) && s.data) {
    bloom_adjust(ctx, key_hash(KEY_NAME, s.data, s.length, ctx->opt.html_document), delta);
  }
  if ((ctx->avail >> GTEXT_CSS_ELEMENT_CB_ID & 1u) && ctx->iface.id(ctx->iface.user, element, &s) && s.data) {
    bloom_adjust(ctx, key_hash(KEY_ID, s.data, s.length, ctx->opt.quirks), delta);
  }
  if ((ctx->avail >> GTEXT_CSS_ELEMENT_CB_CLASS_COUNT & 1u) && (ctx->avail >> GTEXT_CSS_ELEMENT_CB_CLASS_AT & 1u)) {
    const size_t n = ctx->iface.class_count(ctx->iface.user, element);
    for (size_t i = 0; i < n; i++) {
      if (ctx->iface.class_at(ctx->iface.user, element, i, &s) && s.data) {
        bloom_adjust(ctx, key_hash(KEY_CLASS, s.data, s.length, ctx->opt.quirks), delta);
      }
    }
  }
  return true;
}

bool css_bloom_push(struct GTEXT_CSS_Selector_Match_Context * ctx, El element) {
  ctx->bloom_depth++;
  return bloom_element(ctx, element, +1);
}

bool css_bloom_pop(struct GTEXT_CSS_Selector_Match_Context * ctx, El element) {
  if (ctx->bloom_depth) {
    ctx->bloom_depth--;
  }
  return bloom_element(ctx, element, -1);
}

bool css_bloom_may_match(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Complex * cx) {
  if (!ctx->bloom || cx->compound_count < 2) {
    return true;
  }
  const GTEXT_CSS_Compound * last = &cx->compounds[cx->compound_count - 1];
  if (last->first_pseudo_element < last->simple_count) {
    const GTEXT_CSS_Simple * pe = &last->simples[last->first_pseudo_element];
    if (pe->pseudo == GTEXT_CSS_SEL_PE_SLOTTED_FN || pe->pseudo == GTEXT_CSS_SEL_PE_PART_FN) {
      return true; // the ancestors are in another tree
    }
  }
  for (size_t i = cx->compound_count - 1; i > 0; i--) {
    const GTEXT_CSS_Combinator c = cx->compounds[i].combinator;
    if (c != GTEXT_CSS_COMBINATOR_DESCENDANT && c != GTEXT_CSS_COMBINATOR_CHILD) {
      break;
    }
    const GTEXT_CSS_Compound * anc = &cx->compounds[i - 1];
    if (css_compound_names_host(anc)) {
      break; // matched against the host of a shadow tree, which is no ancestor of the element in the filter
    }
    for (size_t k = 0; k < anc->simple_count; k++) {
      const GTEXT_CSS_Simple * s = &anc->simples[k];
      if (s->never) {
        continue;
      }
      uint32_t h = 0;
      bool have = false;
      if (s->kind == GTEXT_CSS_SIMPLE_ID && (ctx->avail >> GTEXT_CSS_ELEMENT_CB_ID & 1u)) {
        h = key_hash(KEY_ID, s->name.data, s->name.length, ctx->opt.quirks);
        have = true;
      }
      else if (s->kind == GTEXT_CSS_SIMPLE_CLASS && (ctx->avail >> GTEXT_CSS_ELEMENT_CB_CLASS_AT & 1u)) {
        h = key_hash(KEY_CLASS, s->name.data, s->name.length, ctx->opt.quirks);
        have = true;
      }
      else if (s->kind == GTEXT_CSS_SIMPLE_TYPE && (ctx->avail >> GTEXT_CSS_ELEMENT_CB_LOCAL_NAME & 1u)) {
        h = key_hash(KEY_NAME, s->name.data, s->name.length, ctx->opt.html_document);
        have = true;
      }
      if (have && !bloom_has(ctx, h)) {
        ctx->ctr.bloom_rejects++;
        return false;
      }
    }
  }
  return true;
}
