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
 * Typed access to the font at-rules (css_font_rules.h). Each reader parses the canonical text of the descriptors the registry already validated and serialized, so the grammar
 * it reads is the registry's own and the same on every platform; what it adds is the meaning (a weight range, merged code point ranges, the folded family key).
 */

#include <ghoti.io/unicode/case.h>

#include <stdlib.h>
#include <string.h>

#include "css_atrule_internal.h"

/// The family name a `<font-family-name>` token run reads: a string, or the idents joined by one space.
static GTEXT_CSS_String family_of(holder * h, const GTEXT_CSS_Component_Value * v, size_t n, bool * oom) {
  GTEXT_CSS_String out = {NULL, 0};
  size_t i = 0;
  const GTEXT_CSS_Component_Value * first = next_sig(v, n, &i);
  if (!first) {
    return out;
  }
  if (first->kind == GTEXT_CSS_VALUE_TOKEN && first->type == GTEXT_CSS_TOKEN_STRING) {
    return copy_string(h, &first->value, oom);
  }
  size_t total = 0;
  for (size_t k = 0; k < n; k++) {
    if (v[k].kind == GTEXT_CSS_VALUE_TOKEN && v[k].type == GTEXT_CSS_TOKEN_IDENT) {
      total += v[k].value.length + 1;
    }
  }
  char * buf = css_arena_alloc(&h->arena, total + 1);
  if (!buf) {
    *oom = true;
    return out;
  }
  size_t len = 0;
  for (size_t k = 0; k < n; k++) {
    if (v[k].kind == GTEXT_CSS_VALUE_TOKEN && v[k].type == GTEXT_CSS_TOKEN_IDENT) {
      if (len) {
        buf[len++] = ' ';
      }
      memcpy(buf + len, v[k].value.data, v[k].value.length);
      len += v[k].value.length;
    }
  }
  buf[len] = 0;
  out.data = buf;
  out.length = len;
  return out;
}

/// The family name folded by full case folding (Fonts 4, 5: "Default Caseless Matching ... without normalizing ... and without applying any language-specific tailorings").
static GTEXT_CSS_String fold_key(holder * h, const GTEXT_CSS_String * s, bool * oom) {
  GTEXT_CSS_String out = {NULL, 0};
  const size_t cap = s->length * 12 + 16;
  char * buf = css_arena_alloc(&h->arena, cap);
  if (!buf) {
    *oom = true;
    return out;
  }
  size_t n = 0;
  if (guni_case_fold_utf8(s->data ? s->data : "", s->length, GUNI_INVALID_REPLACE, false, NULL, buf, cap, &n) != GUNI_OK) {
    memcpy(buf, s->data ? s->data : "", s->length);
    n = s->length;
  }
  buf[n] = 0;
  out.data = buf;
  out.length = n;
  return out;
}

static bool oom_status(bool oom) {
  return oom;
}

/* ---- @font-face ----------------------------------------------------------------------------------------------------------------------- */

static const char * const format_names[] = {"collection", "embedded-opentype", "opentype", "svg", "truetype", "woff", "woff2"};
static const char * const tech_names[] = {"features-opentype", "features-aat", "features-graphite", "color-colrv0", "color-colrv1", "color-svg", "color-sbix", "color-cbdt",
    "variations", "palettes", "incremental"};

static uint32_t bit_of(const GTEXT_CSS_String * s, const char * const * names, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (css_ascii_ieq(s, names[i])) {
      return (uint32_t)1 << i;
    }
  }
  return 0;
}

static void read_overrides(const GTEXT_CSS_Tree * t, GTEXT_CSS_Font_Override out[2], bool allow_from_font) {
  size_t k = 0;
  for (size_t i = 0; t && i < t->value_count && k < 2;) {
    const GTEXT_CSS_Component_Value * v = next_sig(t->values, t->value_count, &i);
    if (!v) {
      break;
    }
    out[k].kind = 0;
    out[k].percent = 0;
    if (v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_PERCENTAGE) {
      out[k].kind = 2;
      out[k].percent = v->number;
    }
    else if (allow_from_font && is_ident(v, "from-font")) {
      out[k].kind = 1;
    }
    k++;
  }
  if (k == 1) {
    out[1] = out[0];
  }
}

static double weight_of(const GTEXT_CSS_Component_Value * v) {
  if (v->type == GTEXT_CSS_TOKEN_NUMBER) {
    return v->number;
  }
  if (is_ident(v, "bold")) {
    return 700;
  }
  return 400;
}

static double width_of(const GTEXT_CSS_Component_Value * v) {
  static const struct {
    const char * n;
    double pc;
  } k[] = {{"ultra-condensed", 50}, {"extra-condensed", 62.5}, {"condensed", 75}, {"semi-condensed", 87.5}, {"normal", 100}, {"semi-expanded", 112.5}, {"expanded", 125},
      {"extra-expanded", 150}, {"ultra-expanded", 200}};
  if (v->type == GTEXT_CSS_TOKEN_PERCENTAGE || v->type == GTEXT_CSS_TOKEN_NUMBER) {
    return v->number;
  }
  for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
    if (is_ident(v, k[i].n)) {
      return k[i].pc;
    }
  }
  return 100;
}

static int range_cmp(const void * a, const void * b) {
  const GTEXT_CSS_Code_Range * x = a;
  const GTEXT_CSS_Code_Range * y = b;
  if (x->first != y->first) {
    return x->first < y->first ? -1 : 1;
  }
  return x->last < y->last ? -1 : (x->last > y->last);
}

static GTEXT_CSS_Status read_settings(holder * h, const GTEXT_CSS_Tree * t, size_t max, const GTEXT_CSS_Font_Setting ** out, size_t * count, bool variation) {
  *out = NULL;
  *count = 0;
  if (!t) {
    return GTEXT_CSS_OK;
  }
  size_t groups = 1;
  for (size_t i = 0; i < t->value_count; i++) {
    groups += is_comma(&t->values[i]);
  }
  if (groups > max) {
    return GTEXT_CSS_E_LIMIT_DESCRIPTORS;
  }
  GTEXT_CSS_Font_Setting * arr = css_arena_alloc(&h->arena, groups * sizeof *arr);
  if (!arr) {
    return GTEXT_CSS_E_OOM;
  }
  size_t n = 0, i = 0;
  while (i <= t->value_count) {
    size_t j = i;
    while (j < t->value_count && !is_comma(&t->values[j])) {
      j++;
    }
    size_t k = i;
    const GTEXT_CSS_Component_Value * tag = next_sig(t->values, j, &k);
    if (tag && tag->kind == GTEXT_CSS_VALUE_TOKEN && tag->type == GTEXT_CSS_TOKEN_STRING && tag->value.length == 4) {
      GTEXT_CSS_Font_Setting * s = &arr[n++];
      memcpy(s->tag, tag->value.data, 4);
      s->tag[4] = 0;
      s->value = 1;
      const GTEXT_CSS_Component_Value * val = next_sig(t->values, j, &k);
      if (val) {
        if (val->type == GTEXT_CSS_TOKEN_NUMBER) {
          s->value = val->number;
        }
        else if (!variation && is_ident(val, "off")) {
          s->value = 0;
        }
      }
    }
    i = j + 1;
  }
  *out = arr;
  *count = n;
  return GTEXT_CSS_OK;
}

void gtext_css_font_face_free(GTEXT_CSS_Font_Face * face) {
  if (face) {
    holder * h = face->priv_;
    const GTEXT_Allocator a = h->alloc;
    holder_free(h);
    gtext_allocator_free(&a, face);
  }
}

bool gtext_css_font_face_defined(const GTEXT_CSS_Font_Face * face) {
  return face && (face->present & GTEXT_CSS_FACE_HAS_FAMILY) && (face->present & GTEXT_CSS_FACE_HAS_SRC) && face->src_count > 0;
}

GTEXT_CSS_Status gtext_css_font_face_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Font_Face ** out) {
  if (!rule || !out || gtext_css_atrule_kind(rule) != GTEXT_CSS_ATRULE_FONT_FACE) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  const GTEXT_Allocator * al = &rule->alloc;
  GTEXT_CSS_Font_Face * f = gtext_allocator_calloc(al, 1, sizeof *f);
  if (!f) {
    return GTEXT_CSS_E_OOM;
  }
  holder * h = holder_new(al);
  if (!h) {
    gtext_allocator_free(al, f);
    return GTEXT_CSS_E_OOM;
  }
  f->priv_ = h;
  f->rule = rule;
  f->weight_auto = f->width_auto = f->font_size_auto = f->named_instance_auto = f->language_override_normal = true;
  f->weight_min = f->weight_max = 400;
  f->width_min = f->width_max = 100;
  f->size_adjust = 100;
  f->style = GTEXT_CSS_FACE_STYLE_AUTO;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  bool oom = false;
  GTEXT_CSS_Tree * t = NULL;
  const size_t max = rule->max_children;
  /* font-family */
  if ((st = css_atrule_tokens(rule, "font-family", &t)) == GTEXT_CSS_OK && t) {
    f->present |= GTEXT_CSS_FACE_HAS_FAMILY;
    f->family = family_of(h, t->values, t->value_count, &oom);
    f->family_key = fold_key(h, &f->family, &oom);
  }
  gtext_css_tree_free(t);
  t = NULL;
  /* src */
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "src", &t)) == GTEXT_CSS_OK && t) {
    f->present |= GTEXT_CSS_FACE_HAS_SRC;
    size_t groups = 1;
    for (size_t i = 0; i < t->value_count; i++) {
      groups += is_comma(&t->values[i]);
    }
    if (groups > max) {
      st = GTEXT_CSS_E_LIMIT_DESCRIPTORS;
    }
    else {
      GTEXT_CSS_Font_Src * arr = css_arena_alloc(&h->arena, groups * sizeof *arr);
      if (!arr) {
        oom = true;
      }
      size_t n = 0, i = 0;
      while (arr && i <= t->value_count) {
        size_t j = i;
        while (j < t->value_count && !is_comma(&t->values[j])) {
          j++;
        }
        GTEXT_CSS_Font_Src s;
        memset(&s, 0, sizeof s);
        size_t k = i;
        const GTEXT_CSS_Component_Value * v;
        bool have = false;
        while ((v = next_sig(t->values, j, &k)) != NULL) {
          if (v->kind == GTEXT_CSS_VALUE_TOKEN && (v->type == GTEXT_CSS_TOKEN_URL || v->type == GTEXT_CSS_TOKEN_STRING)) {
            s.kind = GTEXT_CSS_FONT_SRC_URL;
            s.url = copy_string(h, &v->value, &oom);
            have = true;
          }
          else if (v->kind == GTEXT_CSS_VALUE_FUNCTION && css_ascii_ieq(&v->value, "url")) {
            s.kind = GTEXT_CSS_FONT_SRC_URL;
            for (size_t c = 0; c < v->child_count; c++) {
              if (v->children[c].kind == GTEXT_CSS_VALUE_TOKEN && v->children[c].type != GTEXT_CSS_TOKEN_WHITESPACE) {
                s.url = copy_string(h, &v->children[c].value, &oom);
                break;
              }
            }
            have = true;
          }
          else if (v->kind == GTEXT_CSS_VALUE_FUNCTION && css_ascii_ieq(&v->value, "local")) {
            s.kind = GTEXT_CSS_FONT_SRC_LOCAL;
            s.local = family_of(h, v->children, v->child_count, &oom);
            have = true;
          }
          else if (v->kind == GTEXT_CSS_VALUE_FUNCTION && css_ascii_ieq(&v->value, "format")) {
            s.has_format = true;
            size_t c = 0;
            const GTEXT_CSS_Component_Value * a = next_sig(v->children, v->child_count, &c);
            if (a) {
              s.format = copy_string(h, &a->value, &oom);
              s.format_bit = a->type == GTEXT_CSS_TOKEN_IDENT ? bit_of(&a->value, format_names, sizeof format_names / sizeof *format_names) : 0;
            }
          }
          else if (v->kind == GTEXT_CSS_VALUE_FUNCTION && css_ascii_ieq(&v->value, "tech")) {
            for (size_t c = 0; c < v->child_count; c++) {
              if (v->children[c].kind == GTEXT_CSS_VALUE_TOKEN && v->children[c].type == GTEXT_CSS_TOKEN_IDENT) {
                s.tech |= bit_of(&v->children[c].value, tech_names, sizeof tech_names / sizeof *tech_names);
              }
            }
          }
        }
        if (have) {
          arr[n++] = s;
        }
        i = j + 1;
      }
      f->src = arr;
      f->src_count = n;
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  /* font-style */
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "font-style", &t)) == GTEXT_CSS_OK && t) {
    f->present |= GTEXT_CSS_FACE_HAS_STYLE;
    size_t i = 0;
    const GTEXT_CSS_Component_Value * v = next_sig(t->values, t->value_count, &i);
    if (v && is_ident(v, "normal")) {
      f->style = GTEXT_CSS_FACE_STYLE_NORMAL;
    }
    else if (v && is_ident(v, "italic")) {
      f->style = GTEXT_CSS_FACE_STYLE_ITALIC;
    }
    else if (v && is_ident(v, "left")) {
      f->style = GTEXT_CSS_FACE_STYLE_LEFT;
    }
    else if (v && is_ident(v, "right")) {
      f->style = GTEXT_CSS_FACE_STYLE_RIGHT;
    }
    else if (v && is_ident(v, "oblique")) {
      f->style = GTEXT_CSS_FACE_STYLE_OBLIQUE;
      f->oblique_min = f->oblique_max = 14;
      const GTEXT_CSS_Component_Value * a = next_sig(t->values, t->value_count, &i);
      if (a && a->type == GTEXT_CSS_TOKEN_DIMENSION) {
        f->oblique_min = f->oblique_max = a->number;
        const GTEXT_CSS_Component_Value * b = next_sig(t->values, t->value_count, &i);
        if (b && b->type == GTEXT_CSS_TOKEN_DIMENSION) {
          f->oblique_max = b->number;
        }
      }
      if (f->oblique_min > f->oblique_max) {
        const double tmp = f->oblique_min;
        f->oblique_min = f->oblique_max;
        f->oblique_max = tmp;
      }
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  /* font-weight, font-width: auto, or one or two values (a closed range) */
  for (int pass = 0; pass < 2 && st == GTEXT_CSS_OK; pass++) {
    if ((st = css_atrule_tokens(rule, pass ? "font-width" : "font-weight", &t)) == GTEXT_CSS_OK && t) {
      f->present |= pass ? GTEXT_CSS_FACE_HAS_WIDTH : GTEXT_CSS_FACE_HAS_WEIGHT;
      size_t i = 0;
      const GTEXT_CSS_Component_Value * a = next_sig(t->values, t->value_count, &i);
      if (a && is_ident(a, "auto")) {
        (pass ? &f->width_auto : &f->weight_auto)[0] = true;
      }
      else if (a) {
        const double lo = pass ? width_of(a) : weight_of(a);
        const GTEXT_CSS_Component_Value * b = next_sig(t->values, t->value_count, &i);
        const double hi = b ? (pass ? width_of(b) : weight_of(b)) : lo;
        if (pass) {
          f->width_auto = false;
          f->width_min = lo < hi ? lo : hi;
          f->width_max = lo < hi ? hi : lo;
        }
        else {
          f->weight_auto = false;
          f->weight_min = lo < hi ? lo : hi;
          f->weight_max = lo < hi ? hi : lo;
        }
      }
    }
    gtext_css_tree_free(t);
    t = NULL;
  }
  /* unicode-range: merged, sorted, non-overlapping. The registry's canonical text spells each range `U+first-last` (wildcards expanded) or `U+code`; it is read here as text,
     because the unicode-range token exists only where the tokenizer is told it is reading that descriptor. */
  {
    GTEXT_CSS_Atrule_Descriptor ud;
    if (st == GTEXT_CSS_OK && gtext_css_atrule_descriptor_find(rule, "unicode-range", 13, &ud)) {
      f->present |= GTEXT_CSS_FACE_HAS_UNICODE_RANGE;
      size_t n = 1;
      for (size_t i = 0; i < ud.text.length; i++) {
        n += ud.text.data[i] == ',';
      }
      if (n > max) {
        st = GTEXT_CSS_E_LIMIT_DESCRIPTORS;
      }
      else {
        GTEXT_CSS_Code_Range * arr = css_arena_alloc(&h->arena, n * sizeof *arr);
        if (!arr) {
          oom = true;
        }
        else {
          size_t m = 0;
          const char * p = ud.text.data;
          const char * end = p + ud.text.length;
          while (p < end) {
            while (p < end && (*p == ' ' || *p == ',')) {
              p++;
            }
            if (p + 2 > end || (p[0] != 'U' && p[0] != 'u') || p[1] != '+') {
              break;
            }
            p += 2;
            uint32_t v[2] = {0, 0};
            int part = 0;
            for (; p < end && *p != ','; p++) {
              if (*p == '-') {
                part = 1;
                continue;
              }
              const char c = *p;
              const uint32_t d = c >= '0' && c <= '9' ? (uint32_t)(c - '0') : (c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10) : (c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10) : 16u));
              if (d < 16 && v[part] < 0x1000000u) {
                v[part] = v[part] * 16 + d;
              }
            }
            if (part == 0) {
              v[1] = v[0];
            }
            if (v[0] <= v[1]) {
              arr[m].first = v[0];
              arr[m].last = v[1];
              m++;
            }
          }
          qsort(arr, m, sizeof *arr, range_cmp);
          size_t w = 0;
          for (size_t i = 0; i < m; i++) {
            if (w && arr[i].first <= arr[w - 1].last + 1) {
              if (arr[i].last > arr[w - 1].last) {
                arr[w - 1].last = arr[i].last;
              }
            }
            else {
              arr[w++] = arr[i];
            }
          }
          f->ranges = arr;
          f->range_count = w;
        }
      }
    }
    else if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Code_Range * d = css_arena_alloc(&h->arena, sizeof *d);
      if (!d) {
        oom = true;
      }
      else {
        d->first = 0;
        d->last = 0x10FFFF;
        f->ranges = d;
        f->range_count = 1;
      }
    }
  }
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "font-feature-settings", &t)) == GTEXT_CSS_OK && t) {
    f->present |= GTEXT_CSS_FACE_HAS_FEATURE_SETTINGS;
    st = read_settings(h, t, max, &f->features, &f->feature_count, false);
  }
  gtext_css_tree_free(t);
  t = NULL;
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "font-variation-settings", &t)) == GTEXT_CSS_OK && t) {
    f->present |= GTEXT_CSS_FACE_HAS_VARIATION_SETTINGS;
    st = read_settings(h, t, max, &f->variations, &f->variation_count, true);
  }
  gtext_css_tree_free(t);
  t = NULL;
  /* font-display */
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "font-display", &t)) == GTEXT_CSS_OK && t) {
    f->present |= GTEXT_CSS_FACE_HAS_DISPLAY;
    size_t i = 0;
    const GTEXT_CSS_Component_Value * v = next_sig(t->values, t->value_count, &i);
    f->display = !v ? GTEXT_CSS_FONT_DISPLAY_AUTO : is_ident(v, "block") ? GTEXT_CSS_FONT_DISPLAY_BLOCK : is_ident(v, "swap") ? GTEXT_CSS_FONT_DISPLAY_SWAP :
        is_ident(v, "fallback") ? GTEXT_CSS_FONT_DISPLAY_FALLBACK : is_ident(v, "optional") ? GTEXT_CSS_FONT_DISPLAY_OPTIONAL : GTEXT_CSS_FONT_DISPLAY_AUTO;
  }
  gtext_css_tree_free(t);
  t = NULL;
  /* the overrides */
  static const struct {
    const char * name;
    uint32_t bit;
    size_t off;
    bool from_font;
  } ov[] = {{"ascent-override", GTEXT_CSS_FACE_HAS_ASCENT_OVERRIDE, offsetof(GTEXT_CSS_Font_Face, ascent), false},
      {"descent-override", GTEXT_CSS_FACE_HAS_DESCENT_OVERRIDE, offsetof(GTEXT_CSS_Font_Face, descent), false},
      {"line-gap-override", GTEXT_CSS_FACE_HAS_LINE_GAP_OVERRIDE, offsetof(GTEXT_CSS_Font_Face, line_gap), false},
      {"superscript-position-override", GTEXT_CSS_FACE_HAS_SUPERSCRIPT_POSITION, offsetof(GTEXT_CSS_Font_Face, superscript_position), true},
      {"subscript-position-override", GTEXT_CSS_FACE_HAS_SUBSCRIPT_POSITION, offsetof(GTEXT_CSS_Font_Face, subscript_position), true},
      {"superscript-size-override", GTEXT_CSS_FACE_HAS_SUPERSCRIPT_SIZE, offsetof(GTEXT_CSS_Font_Face, superscript_size), true},
      {"subscript-size-override", GTEXT_CSS_FACE_HAS_SUBSCRIPT_SIZE, offsetof(GTEXT_CSS_Font_Face, subscript_size), true}};
  for (size_t k = 0; k < sizeof ov / sizeof *ov && st == GTEXT_CSS_OK; k++) {
    if ((st = css_atrule_tokens(rule, ov[k].name, &t)) == GTEXT_CSS_OK && t) {
      f->present |= ov[k].bit;
      read_overrides(t, (GTEXT_CSS_Font_Override *)((char *)f + ov[k].off), ov[k].from_font);
    }
    gtext_css_tree_free(t);
    t = NULL;
  }
  /* size-adjust, font-size */
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "size-adjust", &t)) == GTEXT_CSS_OK && t) {
    f->present |= GTEXT_CSS_FACE_HAS_SIZE_ADJUST;
    size_t i = 0;
    const GTEXT_CSS_Component_Value * v = next_sig(t->values, t->value_count, &i);
    if (v && v->type == GTEXT_CSS_TOKEN_PERCENTAGE) {
      f->size_adjust = v->number;
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "font-size", &t)) == GTEXT_CSS_OK && t) {
    f->present |= GTEXT_CSS_FACE_HAS_FONT_SIZE;
    size_t i = 0;
    const GTEXT_CSS_Component_Value * a = next_sig(t->values, t->value_count, &i);
    if (a && a->type == GTEXT_CSS_TOKEN_NUMBER) {
      const GTEXT_CSS_Component_Value * b = next_sig(t->values, t->value_count, &i);
      f->font_size_auto = false;
      f->font_size_min = f->font_size_max = a->number;
      if (b && b->type == GTEXT_CSS_TOKEN_NUMBER) {
        f->font_size_max = b->number;
      }
      if (f->font_size_min > f->font_size_max) {
        const double tmp = f->font_size_min;
        f->font_size_min = f->font_size_max;
        f->font_size_max = tmp;
      }
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  for (int pass = 0; pass < 2 && st == GTEXT_CSS_OK; pass++) {
    if ((st = css_atrule_tokens(rule, pass ? "font-language-override" : "font-named-instance", &t)) == GTEXT_CSS_OK && t) {
      f->present |= pass ? GTEXT_CSS_FACE_HAS_LANGUAGE_OVERRIDE : GTEXT_CSS_FACE_HAS_NAMED_INSTANCE;
      size_t i = 0;
      const GTEXT_CSS_Component_Value * v = next_sig(t->values, t->value_count, &i);
      if (v && v->type == GTEXT_CSS_TOKEN_STRING) {
        if (pass) {
          f->language_override_normal = false;
          f->language_override = copy_string(h, &v->value, &oom);
        }
        else {
          f->named_instance_auto = false;
          f->named_instance = copy_string(h, &v->value, &oom);
        }
      }
    }
    gtext_css_tree_free(t);
    t = NULL;
  }
  if (st == GTEXT_CSS_OK && oom_status(oom)) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_font_face_free(f);
    return st;
  }
  *out = f;
  return GTEXT_CSS_OK;
}

/* ---- @font-feature-values ------------------------------------------------------------------------------------------------------------- */

void gtext_css_feature_values_free(GTEXT_CSS_Feature_Values * v) {
  if (v) {
    holder * h = v->priv_;
    const GTEXT_Allocator a = h->alloc;
    holder_free(h);
    gtext_allocator_free(&a, v);
  }
}

GTEXT_CSS_Status gtext_css_feature_values_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Feature_Values ** out) {
  if (!rule || !out || gtext_css_atrule_kind(rule) != GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  const GTEXT_Allocator * al = &rule->alloc;
  GTEXT_CSS_Feature_Values * v = gtext_allocator_calloc(al, 1, sizeof *v);
  if (!v) {
    return GTEXT_CSS_E_OOM;
  }
  holder * h = holder_new(al);
  if (!h) {
    gtext_allocator_free(al, v);
    return GTEXT_CSS_E_OOM;
  }
  v->priv_ = h;
  v->rule = rule;
  bool oom = false;
  const size_t nf = gtext_css_atrule_name_count(rule);
  GTEXT_CSS_String * fam = css_arena_alloc(&h->arena, (nf ? nf : 1) * sizeof *fam);
  GTEXT_CSS_String * keys = css_arena_alloc(&h->arena, (nf ? nf : 1) * sizeof *keys);
  if (!fam || !keys) {
    oom = true;
  }
  for (size_t i = 0; !oom && i < nf; i++) {
    GTEXT_CSS_String n;
    gtext_css_atrule_name_at(rule, i, &n, NULL);
    fam[i] = copy_string(h, &n, &oom);
    keys[i] = fold_key(h, &n, &oom);
  }
  v->families = fam;
  v->family_keys = keys;
  v->family_count = nf;
  GTEXT_CSS_Atrule_Descriptor d;
  if (gtext_css_atrule_descriptor_find(rule, "font-display", 12, &d)) {
    v->has_display = true;
    v->display = css_ascii_ieq(&d.text, "block") ? GTEXT_CSS_FONT_DISPLAY_BLOCK : css_ascii_ieq(&d.text, "swap") ? GTEXT_CSS_FONT_DISPLAY_SWAP :
        css_ascii_ieq(&d.text, "fallback") ? GTEXT_CSS_FONT_DISPLAY_FALLBACK : css_ascii_ieq(&d.text, "optional") ? GTEXT_CSS_FONT_DISPLAY_OPTIONAL : GTEXT_CSS_FONT_DISPLAY_AUTO;
  }
  static const char * const types[GTEXT_CSS_FEATURE_TYPE_COUNT] = {"stylistic", "historical-forms", "styleset", "character-variant", "swash", "ornaments", "annotation"};
  for (size_t ki = 0; !oom && ki < gtext_css_atrule_child_count(rule); ki++) {
    const GTEXT_CSS_Atrule * kid = gtext_css_atrule_child_at(rule, ki);
    size_t nl = 0;
    const char * kn = gtext_css_atrule_name(kid, &nl);
    int type = -1;
    for (int t = 0; t < GTEXT_CSS_FEATURE_TYPE_COUNT; t++) {
      if (strlen(types[t]) == nl && memcmp(types[t], kn, nl) == 0) {
        type = t;
      }
    }
    if (type < 0) {
      continue;
    }
    const size_t nd = gtext_css_atrule_descriptor_count(kid);
    GTEXT_CSS_Feature_Value * arr = css_arena_alloc(&h->arena, (nd ? nd : 1) * sizeof *arr);
    if (!arr) {
      oom = true;
      break;
    }
    size_t m = 0;
    for (size_t di = 0; di < nd; di++) {
      GTEXT_CSS_Atrule_Descriptor ds;
      gtext_css_atrule_descriptor_at(kid, di, &ds);
      uint32_t * ints = css_arena_alloc(&h->arena, (ds.text.length / 2 + 2) * sizeof *ints);
      if (!ints) {
        oom = true;
        break;
      }
      size_t ni = 0;
      uint64_t cur = 0;
      bool in = false;
      for (size_t c = 0; c <= ds.text.length; c++) {
        const char ch = c < ds.text.length ? ds.text.data[c] : ' ';
        if (ch >= '0' && ch <= '9') {
          cur = cur * 10 + (uint64_t)(ch - '0');
          in = true;
        }
        else if (in) {
          ints[ni++] = (uint32_t)cur;
          cur = 0;
          in = false;
        }
      }
      arr[m].name.data = ds.name;
      arr[m].name.length = ds.name_length;
      arr[m].values = ints;
      arr[m].value_count = ni;
      m++;
    }
    /* several blocks of one type in one rule: the later of a name wins */
    if (v->value_count[type]) {
      const size_t old = v->value_count[type];
      GTEXT_CSS_Feature_Value * merged = css_arena_alloc(&h->arena, (old + m) * sizeof *merged);
      if (!merged) {
        oom = true;
        break;
      }
      memcpy(merged, v->values[type], old * sizeof *merged);
      size_t w = old;
      for (size_t a = 0; a < m; a++) {
        bool replaced = false;
        for (size_t b = 0; b < old; b++) {
          if (merged[b].name.length == arr[a].name.length && memcmp(merged[b].name.data, arr[a].name.data, arr[a].name.length) == 0) {
            merged[b] = arr[a];
            replaced = true;
          }
        }
        if (!replaced) {
          merged[w++] = arr[a];
        }
      }
      v->values[type] = merged;
      v->value_count[type] = w;
    }
    else {
      v->values[type] = arr;
      v->value_count[type] = m;
    }
  }
  if (oom) {
    gtext_css_feature_values_free(v);
    return GTEXT_CSS_E_OOM;
  }
  *out = v;
  return GTEXT_CSS_OK;
}

/* ---- @font-palette-values -------------------------------------------------------------------------------------------------------------- */

void gtext_css_palette_values_free(GTEXT_CSS_Palette_Values * v) {
  if (v) {
    holder * h = v->priv_;
    const GTEXT_Allocator a = h->alloc;
    holder_free(h);
    gtext_allocator_free(&a, v);
  }
}

GTEXT_CSS_Status gtext_css_palette_values_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Palette_Values ** out) {
  if (!rule || !out || gtext_css_atrule_kind(rule) != GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  const GTEXT_Allocator * al = &rule->alloc;
  GTEXT_CSS_Palette_Values * v = gtext_allocator_calloc(al, 1, sizeof *v);
  if (!v) {
    return GTEXT_CSS_E_OOM;
  }
  holder * h = holder_new(al);
  if (!h) {
    gtext_allocator_free(al, v);
    return GTEXT_CSS_E_OOM;
  }
  v->priv_ = h;
  v->rule = rule;
  bool oom = false;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  GTEXT_CSS_String nm;
  if (gtext_css_atrule_name_at(rule, 0, &nm, NULL)) {
    v->name = copy_string(h, &nm, &oom);
  }
  GTEXT_CSS_Tree * t = NULL;
  if ((st = css_atrule_tokens(rule, "font-family", &t)) == GTEXT_CSS_OK && t) {
    size_t groups = 1;
    for (size_t i = 0; i < t->value_count; i++) {
      groups += is_comma(&t->values[i]);
    }
    GTEXT_CSS_String * fam = css_arena_alloc(&h->arena, groups * sizeof *fam);
    GTEXT_CSS_String * keys = css_arena_alloc(&h->arena, groups * sizeof *keys);
    if (!fam || !keys) {
      oom = true;
    }
    size_t n = 0, i = 0;
    while (!oom && i <= t->value_count) {
      size_t j = i;
      while (j < t->value_count && !is_comma(&t->values[j])) {
        j++;
      }
      fam[n] = family_of(h, t->values + i, j - i, &oom);
      keys[n] = fold_key(h, &fam[n], &oom);
      n++;
      i = j + 1;
    }
    v->families = fam;
    v->family_keys = keys;
    v->family_count = n;
  }
  gtext_css_tree_free(t);
  t = NULL;
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "base-palette", &t)) == GTEXT_CSS_OK && t) {
    size_t i = 0;
    const GTEXT_CSS_Component_Value * a = next_sig(t->values, t->value_count, &i);
    if (a && is_ident(a, "light")) {
      v->base_kind = GTEXT_CSS_BASE_PALETTE_LIGHT;
    }
    else if (a && is_ident(a, "dark")) {
      v->base_kind = GTEXT_CSS_BASE_PALETTE_DARK;
    }
    else if (a && a->type == GTEXT_CSS_TOKEN_NUMBER) {
      v->base_kind = GTEXT_CSS_BASE_PALETTE_INDEX;
      v->base_index = clamp_u32(a->number);
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "override-colors", &t)) == GTEXT_CSS_OK && t) {
    size_t groups = 1;
    for (size_t i = 0; i < t->value_count; i++) {
      groups += is_comma(&t->values[i]);
    }
    GTEXT_CSS_Palette_Override * arr = css_arena_alloc(&h->arena, groups * sizeof *arr);
    if (!arr) {
      oom = true;
    }
    size_t n = 0, i = 0;
    while (!oom && i <= t->value_count) {
      size_t j = i;
      while (j < t->value_count && !is_comma(&t->values[j])) {
        j++;
      }
      size_t k = i;
      const GTEXT_CSS_Component_Value * idx = next_sig(t->values, j, &k);
      if (idx && idx->type == GTEXT_CSS_TOKEN_NUMBER) {
        GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
        so.allocator = &rule->alloc;
        char * buf = NULL;
        size_t bl = 0;
        st = gtext_css_serialize_values_buffer(t->values + k, j - k, &so, &buf, &bl);
        if (st != GTEXT_CSS_OK) {
          break;
        }
        size_t s0 = 0;
        while (s0 < bl && buf[s0] == ' ') {
          s0++;
        }
        arr[n].index = clamp_u32(idx->number);
        arr[n].color.data = css_arena_strdup(&h->arena, buf + s0, bl - s0);
        arr[n].color.length = bl - s0;
        gtext_css_serialize_buffer_free(&so, buf);
        if (!arr[n].color.data) {
          oom = true;
        }
        n++;
      }
      i = j + 1;
    }
    v->overrides = arr;
    v->override_count = n;
  }
  gtext_css_tree_free(t);
  if (st == GTEXT_CSS_OK && oom) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_palette_values_free(v);
    return st;
  }
  *out = v;
  return GTEXT_CSS_OK;
}

/* ---- @color-profile ------------------------------------------------------------------------------------------------------------------- */

void gtext_css_color_profile_free(GTEXT_CSS_Color_Profile * v) {
  if (v) {
    holder * h = v->priv_;
    const GTEXT_Allocator a = h->alloc;
    holder_free(h);
    gtext_allocator_free(&a, v);
  }
}

GTEXT_CSS_Status gtext_css_color_profile_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Color_Profile ** out) {
  if (!rule || !out || gtext_css_atrule_kind(rule) != GTEXT_CSS_ATRULE_COLOR_PROFILE) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  const GTEXT_Allocator * al = &rule->alloc;
  GTEXT_CSS_Color_Profile * v = gtext_allocator_calloc(al, 1, sizeof *v);
  if (!v) {
    return GTEXT_CSS_E_OOM;
  }
  holder * h = holder_new(al);
  if (!h) {
    gtext_allocator_free(al, v);
    return GTEXT_CSS_E_OOM;
  }
  v->priv_ = h;
  v->rule = rule;
  bool oom = false;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  GTEXT_CSS_String nm;
  if (gtext_css_atrule_name_at(rule, 0, &nm, NULL)) {
    v->name = copy_string(h, &nm, &oom);
    v->device_cmyk = css_ascii_ieq(&nm, "device-cmyk");
  }
  GTEXT_CSS_Tree * t = NULL;
  if ((st = css_atrule_tokens(rule, "src", &t)) == GTEXT_CSS_OK && t) {
    for (size_t i = 0; i < t->value_count; i++) {
      const GTEXT_CSS_Component_Value * c = &t->values[i];
      if (c->kind == GTEXT_CSS_VALUE_TOKEN && (c->type == GTEXT_CSS_TOKEN_URL || c->type == GTEXT_CSS_TOKEN_STRING)) {
        v->src = copy_string(h, &c->value, &oom);
      }
      else if (c->kind == GTEXT_CSS_VALUE_FUNCTION) {
        for (size_t k = 0; k < c->child_count; k++) {
          if (c->children[k].kind == GTEXT_CSS_VALUE_TOKEN && c->children[k].type != GTEXT_CSS_TOKEN_WHITESPACE) {
            v->src = copy_string(h, &c->children[k].value, &oom);
            break;
          }
        }
      }
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "rendering-intent", &t)) == GTEXT_CSS_OK && t) {
    size_t i = 0;
    const GTEXT_CSS_Component_Value * a = next_sig(t->values, t->value_count, &i);
    if (a && is_ident(a, "absolute-colorimetric")) {
      v->intent = GTEXT_CSS_INTENT_ABSOLUTE_COLORIMETRIC;
    }
    else if (a && is_ident(a, "perceptual")) {
      v->intent = GTEXT_CSS_INTENT_PERCEPTUAL;
    }
    else if (a && is_ident(a, "saturation")) {
      v->intent = GTEXT_CSS_INTENT_SATURATION;
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "components", &t)) == GTEXT_CSS_OK && t) {
    GTEXT_CSS_String * arr = css_arena_alloc(&h->arena, (t->value_count + 1) * sizeof *arr);
    if (!arr) {
      oom = true;
    }
    size_t n = 0;
    for (size_t i = 0; !oom && i < t->value_count; i++) {
      if (t->values[i].kind == GTEXT_CSS_VALUE_TOKEN && t->values[i].type == GTEXT_CSS_TOKEN_IDENT) {
        arr[n++] = copy_string(h, &t->values[i].value, &oom);
      }
    }
    v->components = arr;
    v->component_count = n;
  }
  gtext_css_tree_free(t);
  if (st == GTEXT_CSS_OK && oom) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_color_profile_free(v);
    return st;
  }
  *out = v;
  return GTEXT_CSS_OK;
}
