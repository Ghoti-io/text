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
 * libFuzzer harness for the at-rule model (src/css/css_atrule*.c, css_font_rules.c, css_counter_style.c, css_keyframes.c, css_page.c, css_atrule_set.c).
 *
 * The input is style sheet text (with a few leading bytes that choose the failed allocation). The properties:
 *
 *   a status is OK, a limit, UNSUPPORTED or E_OOM for the allocation that was failed, with nothing leaked or freed that the allocator did not make - never E_INTERNAL;
 *   for every top-level at-rule the model keeps, the serialization is a fixed point: it parses back to a rule that serializes to the same bytes;
 *   the typed views (font face, feature values, palette values, color profile, counter style, keyframes) are made for every rule of their kind with a verdict, never a crash;
 *   a parse with the validity callbacks keeps exactly the at-rules the model calls valid;
 *   the finds of the set agree with a scan of its entries in cascade order: the keyframes, `@position-try` and counter style of a name are the last entry of that name, the
 *   normalized keyframes are strictly ascending, and the page matches come out in the order of the page cascade.
 *
 * Seeds: tests/fuzz/corpus/css-atrule, made by tools/css/make-fuzz-atrule-seeds.py.
 * Build with: make fuzz-css-atrule      Run: make fuzz-run-css-atrule
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "../css_cascade_test_support.h"
#include "../markup_test_support.h"

namespace css_atrulefuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_css_atrule: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
  for (unsigned char c : input) {
    std::fprintf(stderr, "%02x", c);
  }
  std::fputc('\n', stderr);
  std::abort();
}

bool allowed(GTEXT_CSS_Status st) {
  return st == GTEXT_CSS_OK || st == GTEXT_CSS_E_OOM || st == GTEXT_CSS_E_UNSUPPORTED || (st >= GTEXT_CSS_E_LIMIT_INPUT && st <= GTEXT_CSS_E_LIMIT_DESCRIPTORS);
}

std::string text_of(const GTEXT_CSS_Atrule * a, const GTEXT_Allocator * al) {
  char * buf = nullptr;
  size_t n = 0;
  if (gtext_css_atrule_serialize_buffer(a, al, &buf, &n) != GTEXT_CSS_OK) {
    return "<oom>";
  }
  std::string out(buf, n);
  gtext_css_atrule_buffer_free(al, buf);
  return out;
}

struct Run {
  const std::string & input;
  FaultAllocator & fa;
};

void typed(const GTEXT_CSS_Atrule * a, const std::string & input) {
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  switch (gtext_css_atrule_kind(a)) {
  case GTEXT_CSS_ATRULE_FONT_FACE: {
    GTEXT_CSS_Font_Face * f = nullptr;
    st = gtext_css_font_face_from_atrule(a, &f);
    if (st == GTEXT_CSS_OK) {
      for (size_t i = 1; i < f->range_count; i++) {
        if (f->ranges[i].first <= f->ranges[i - 1].last + 1) {
          fail("unicode ranges are not merged", "", input);
        }
      }
      if (f->weight_min > f->weight_max || f->width_min > f->width_max) {
        fail("a range is reversed", "", input);
      }
    }
    gtext_css_font_face_free(f);
    break;
  }
  case GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES: {
    GTEXT_CSS_Feature_Values * f = nullptr;
    st = gtext_css_feature_values_from_atrule(a, &f);
    gtext_css_feature_values_free(f);
    break;
  }
  case GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES: {
    GTEXT_CSS_Palette_Values * f = nullptr;
    st = gtext_css_palette_values_from_atrule(a, &f);
    gtext_css_palette_values_free(f);
    break;
  }
  case GTEXT_CSS_ATRULE_COLOR_PROFILE: {
    GTEXT_CSS_Color_Profile * f = nullptr;
    st = gtext_css_color_profile_from_atrule(a, &f);
    gtext_css_color_profile_free(f);
    break;
  }
  case GTEXT_CSS_ATRULE_COUNTER_STYLE: {
    GTEXT_CSS_Counter_Style * f = nullptr;
    st = gtext_css_counter_style_from_atrule(a, &f);
    gtext_css_counter_style_free(f);
    break;
  }
  case GTEXT_CSS_ATRULE_KEYFRAMES: {
    GTEXT_CSS_Keyframes * f = nullptr;
    st = gtext_css_keyframes_from_atrule(a, &f);
    if (st == GTEXT_CSS_OK) {
      for (size_t i = 1; i < f->frame_count; i++) {
        if (!(f->frames[i].offset > f->frames[i - 1].offset)) {
          fail("keyframes are not strictly ascending", "", input);
        }
      }
    }
    gtext_css_keyframes_free(f);
    break;
  }
  default:
    break;
  }
  if (!allowed(st)) {
    fail("a typed view returned an unexpected status", gtext_css_status_string(st), input);
  }
}

void pipeline(const std::string & input, FaultAllocator & fa) {
  const GTEXT_CSS_Registry * reg = cas::R();
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = &fa.alloc;
  GTEXT_CSS_Tree * tree = nullptr;
  GTEXT_CSS_Result pr;
  std::memset(&pr, 0, sizeof pr);
  GTEXT_CSS_Status st = gtext_css_parse_stylesheet(input.data(), input.size(), &po, &tree, &pr);
  gtext_css_result_free(&pr);
  if (st != GTEXT_CSS_OK || !tree) {
    gtext_css_tree_free(tree);
    if (!allowed(st)) {
      fail("the parse returned an unexpected status", gtext_css_status_string(st), input);
    }
    return;
  }
  GTEXT_CSS_Atrule_Options ao = gtext_css_atrule_options_default();
  ao.allocator = &fa.alloc;
  for (size_t i = 0; i < tree->rule_count; i++) {
    const GTEXT_CSS_Rule * r = &tree->rules[i];
    if (r->kind != GTEXT_CSS_RULE_AT) {
      continue;
    }
    GTEXT_CSS_Atrule * a = nullptr;
    GTEXT_CSS_Atrule_Result res;
    st = gtext_css_atrule_parse(reg, r, &ao, &a, &res);
    if (!allowed(st)) {
      fail("the at-rule parse returned an unexpected status", gtext_css_status_string(st), input);
    }
    if (st != GTEXT_CSS_OK) {
      gtext_css_atrule_free(a);
      continue;
    }
    if (res.valid && a) {
      typed(a, input);
      const std::string once = text_of(a, &fa.alloc);
      if (once != "<oom>") {
        css_test::Parsed again;
        again.status = gtext_css_parse_stylesheet(once.data(), once.size(), &po, &again.tree, &again.result);
        if (again.status == GTEXT_CSS_OK && again.tree && again.tree->rule_count >= 1) {
          GTEXT_CSS_Atrule * b = nullptr;
          GTEXT_CSS_Atrule_Result rb;
          const GTEXT_CSS_Status sb = gtext_css_atrule_parse(reg, &again.tree->rules[0], &ao, &b, &rb);
          if (sb == GTEXT_CSS_OK) {
            if (!rb.valid || !b) {
              fail("a serialization is not a valid rule", once, input);
            }
            const std::string twice = text_of(b, &fa.alloc);
            if (twice != "<oom>" && twice != once) {
              fail("the serialization is not a fixed point", once + "\n" + twice, input);
            }
          }
          gtext_css_atrule_free(b);
        }
      }
    }
    gtext_css_atrule_free(a);
  }
  // the validity callbacks keep exactly what the model calls valid
  GTEXT_CSS_Atrule_Validity v;
  gtext_css_atrule_validity_init(&v, reg);
  GTEXT_CSS_Parse_Options vo = po;
  vo.validity = v.validity;
  GTEXT_CSS_Tree * vt = nullptr;
  GTEXT_CSS_Result vr;
  std::memset(&vr, 0, sizeof vr);
  st = gtext_css_parse_stylesheet(input.data(), input.size(), &vo, &vt, &vr);
  gtext_css_result_free(&vr);
  if (st == GTEXT_CSS_OK && vt && v.refused == 0 && fa.fail_on == 0 && vt->rule_count != tree->rule_count) {
    fail("a parse with the validity callbacks dropped a rule although none was refused", "", input);
  }
  gtext_css_tree_free(vt);
  // the set
  GTEXT_CSS_Atrule_Set_Options so = gtext_css_atrule_set_options_default();
  so.allocator = &fa.alloc;
  GTEXT_CSS_Atrule_Set * set = nullptr;
  st = gtext_css_atrule_set_new(reg, &so, &set);
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_atrule_set_add_tree(set, tree);
    if (st == GTEXT_CSS_OK) {
      st = gtext_css_atrule_set_freeze(set);
    }
    if (!allowed(st)) {
      fail("the set returned an unexpected status", gtext_css_status_string(st), input);
    }
    if (st == GTEXT_CSS_OK) {
      // the finds equal a scan of the entries in cascade order
      const size_t n = gtext_css_atrule_set_count(set);
      for (size_t i = 0; i < n; i++) {
        GTEXT_CSS_Atrule_Entry e;
        if (!gtext_css_atrule_set_entry(set, i, &e)) {
          fail("an entry is missing", "", input);
        }
        if (i > 0) {
          GTEXT_CSS_Atrule_Entry p;
          gtext_css_atrule_set_entry(set, i - 1, &p);
          if (p.layer_rank > e.layer_rank || (p.layer_rank == e.layer_rank && p.order > e.order)) {
            fail("the entries are not in cascade order", "", input);
          }
        }
        if (gtext_css_atrule_kind(e.rule) == GTEXT_CSS_ATRULE_POSITION_TRY) {
          GTEXT_CSS_String nm;
          gtext_css_atrule_name_at(e.rule, 0, &nm, nullptr);
          const GTEXT_CSS_Atrule * want = e.rule;
          for (size_t j = i + 1; j < n; j++) {
            GTEXT_CSS_Atrule_Entry q;
            gtext_css_atrule_set_entry(set, j, &q);
            GTEXT_CSS_String qn;
            if (gtext_css_atrule_kind(q.rule) == GTEXT_CSS_ATRULE_POSITION_TRY && gtext_css_atrule_name_at(q.rule, 0, &qn, nullptr) && qn.length == nm.length &&
                std::memcmp(qn.data, nm.data, nm.length) == 0) {
              want = q.rule;
            }
          }
          if (gtext_css_atrule_set_position_try(set, nm.data, nm.length) != want) {
            fail("the @position-try find is not the last of its name", "", input);
          }
        }
        if (gtext_css_atrule_kind(e.rule) == GTEXT_CSS_ATRULE_KEYFRAMES) {
          GTEXT_CSS_String nm;
          gtext_css_atrule_name_at(e.rule, 0, &nm, nullptr);
          const GTEXT_CSS_Keyframes * k = gtext_css_atrule_set_keyframes(set, nm.data, nm.length);
          if (!k || k->frame_count > 0xFFFFFF) {
            fail("a keyframes name is not found", "", input);
          }
        }
      }
      GTEXT_CSS_Page_Context pc;
      std::memset(&pc, 0, sizeof pc);
      pc.first = true;
      GTEXT_CSS_Page_Match m[16];
      const size_t pn = gtext_css_atrule_set_page_collect(set, &pc, m, 16);
      for (size_t i = 1; i < pn && i < 16; i++) {
        if (m[i - 1].layer_rank > m[i].layer_rank) {
          fail("page matches are not in layer order", "", input);
        }
      }
    }
  }
  else if (!allowed(st)) {
    fail("the set could not be made for an unexpected reason", gtext_css_status_string(st), input);
  }
  gtext_css_atrule_set_free(set);
  gtext_css_tree_free(tree);
}

int run(const uint8_t * data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const size_t fail_at = static_cast<size_t>(data[0]) * 4 + (data[1] & 3) + 1;
  const std::string input(reinterpret_cast<const char *>(data + 2), size - 2);
  FaultAllocator fa;
  pipeline(input, fa);
  if (!fa.live.empty()) {
    fail("an allocation leaked", std::to_string(fa.live.size()) + " blocks", input);
  }
  if (fa.bad_frees) {
    fail("a block the allocator did not make was freed", "", input);
  }
  fa.arm(fail_at);
  pipeline(input, fa);
  fa.disarm();
  if (!fa.live.empty()) {
    fail("an allocation leaked after a failed allocation", std::to_string(fa.live.size()) + " blocks", input);
  }
  if (fa.bad_frees) {
    fail("a block the allocator did not make was freed after a failed allocation", "", input);
  }
  return 0;
}

}  // namespace css_atrulefuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_atrulefuzz::run(data, size);
}
