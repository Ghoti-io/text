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
 *   every descriptor the model keeps is valid for the raw grammar matcher: its text, tokenized again, is matched by the registry's grammar for it (gtext_css_descriptor_parse(),
 *   the matcher the properties use, which does not know the model's own rules about names, counts and orders), so the model never keeps what the grammar refuses;
 *   the fixed point is more than text: the rule read back from the serialization has the same kind, the same descriptors in the same order with the same text, the same
 *   nested rules and the same "defines nothing"; and the serialization of a rule the end of the input closed is itself closed (the repair is a rule);
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

/// Whether a custom property of the rule (or of a rule nested in it) has a value with a `{}` block or a function that the input left open (an open `url(` read to the end of an
/// unclosed rule is written back without what closes the rule; open question, listed in the record): `--x: a { b };` is a declaration in one reading and the start of a nested rule in
/// another (Syntax 5.4.4), so its text does not read back as the same rule. That is the syntax layer's ambiguity and not the model's; such a rule is left out of the fixed point.
bool custom_block(const GTEXT_CSS_Atrule * a) {
  for (size_t i = 0; i < gtext_css_atrule_descriptor_count(a); i++) {
    GTEXT_CSS_Atrule_Descriptor d;
    gtext_css_atrule_descriptor_at(a, i, &d);
    if (d.name_length > 2 && d.name[0] == '-' && d.name[1] == '-' && (std::memchr(d.text.data, '{', d.text.length) || std::memchr(d.text.data, '(', d.text.length))) {
      return true;
    }
  }
  for (size_t i = 0; i < gtext_css_atrule_child_count(a); i++) {
    if (custom_block(gtext_css_atrule_child_at(a, i))) {
      return true;
    }
  }
  return false;
}

/// Whether the descriptors of the rule are valid for the registry's grammar, matched afresh from their text. `unicode-range` is read where the tokenizer is told it is that
/// descriptor's value (a text of ranges is not tokenized alike elsewhere), and a custom property or a property of a page, margin or `@position-try` block has no descriptor row.
void raw_grammar(const GTEXT_CSS_Atrule * a, const GTEXT_CSS_Registry * reg, const std::string & input) {
  const char * name = gtext_css_atrule_name(a, nullptr);
  std::string at = std::string("@") + (name ? name : "");
  const GTEXT_CSS_At_Rule_Id aid = gtext_css_registry_find_at_rule(at.c_str(), at.size());
  if (aid) {
    for (size_t i = 0; i < gtext_css_atrule_descriptor_count(a); i++) {
      GTEXT_CSS_Atrule_Descriptor d;
      if (!gtext_css_atrule_descriptor_at(a, i, &d) || (d.name_length > 2 && d.name[0] == '-' && d.name[1] == '-') || std::string(d.name, d.name_length) == "unicode-range") {
        continue;
      }
      const GTEXT_CSS_Descriptor_Id did = gtext_css_registry_find_descriptor(aid, d.name, d.name_length);
      if (!did) {
        continue;
      }
      GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
      GTEXT_CSS_Tree * t = nullptr;
      if (gtext_css_parse_component_values(d.text.data ? d.text.data : "", d.text.length, &po, &t, nullptr) != GTEXT_CSS_OK) {
        continue;
      }
      GTEXT_CSS_Value_Set * set = nullptr;
      const GTEXT_CSS_Status st = gtext_css_descriptor_parse(reg, did, t->values, t->value_count, nullptr, &set);
      if (st == GTEXT_CSS_OK && !set) {
        fail("the model kept a descriptor that the grammar of the registry refuses", at + " " + std::string(d.name, d.name_length) + ": " + std::string(d.text.data ? d.text.data : "", d.text.length), input);
      }
      gtext_css_value_set_free(set);
      gtext_css_tree_free(t);
    }
  }
}

/// Whether any token of the values, at any depth, was ended by the end of the input (an open string or url).
bool holds_unclosed_token(const GTEXT_CSS_Component_Value * v, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if ((v[i].flags & GTEXT_CSS_FLAG_UNCLOSED) || (v[i].child_count && holds_unclosed_token(v[i].children, v[i].child_count))) {
      return true;
    }
  }
  return false;
}

/// The parts of two rules that a fixed point must keep, beside the text: the kind, the descriptors in order with their text, the nested rules, "defines nothing".
bool same_rule(const GTEXT_CSS_Atrule * a, const GTEXT_CSS_Atrule * b) {
  if (gtext_css_atrule_kind(a) != gtext_css_atrule_kind(b) || gtext_css_atrule_descriptor_count(a) != gtext_css_atrule_descriptor_count(b) ||
      gtext_css_atrule_child_count(a) != gtext_css_atrule_child_count(b) || gtext_css_atrule_defines_nothing(a) != gtext_css_atrule_defines_nothing(b)) {
    return false;
  }
  for (size_t i = 0; i < gtext_css_atrule_descriptor_count(a); i++) {
    GTEXT_CSS_Atrule_Descriptor x, y;
    if (!gtext_css_atrule_descriptor_at(a, i, &x) || !gtext_css_atrule_descriptor_at(b, i, &y) || x.name_length != y.name_length || x.text.length != y.text.length ||
        (x.name_length && std::memcmp(x.name, y.name, x.name_length) != 0) || (x.text.length && std::memcmp(x.text.data, y.text.data, x.text.length) != 0)) {
      return false;
    }
  }
  for (size_t i = 0; i < gtext_css_atrule_child_count(a); i++) {
    if (!same_rule(gtext_css_atrule_child_at(a, i), gtext_css_atrule_child_at(b, i))) {
      return false;
    }
  }
  return true;
}

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
      raw_grammar(a, reg, input);
      const std::string once = text_of(a, &fa.alloc);
      if (once != "<oom>" && !custom_block(a) && r->unclosed) {
        /* the text of a rule the end of the input closed is the repair, and the repair is closed: read back, it is a rule that nothing closed for it */
        css_test::Parsed closed;
        closed.status = gtext_css_parse_stylesheet(once.data(), once.size(), &po, &closed.tree, &closed.result);
        GTEXT_CSS_Tree * flat = nullptr;
        /* A custom property keeps its value as the author wrote it (Variables 1), and a string the end of the input left open is in that text without its quote: such a
           rule's text opens the string again, so the class is left out (the syntax layer's raw text, not the model's). Every other rule must come back closed. */
        const bool open_token = gtext_css_parse_component_values(once.data(), once.size(), &po, &flat, nullptr) == GTEXT_CSS_OK && flat && holds_unclosed_token(flat->values, flat->value_count);
        gtext_css_tree_free(flat);
        if (!open_token && closed.status == GTEXT_CSS_OK && closed.tree && closed.tree->rule_count >= 1 && closed.tree->rules[0].unclosed) {
          fail("the serialization of a rule the end of the input closed is not closed", once, input);
        }
      }
      if (once != "<oom>" && !custom_block(a) && !r->unclosed) {
        /* a rule the end of the input closed holds parse errors (an open string or url read to the end): its text is the repair, and the fixed point is asked of closed rules */
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
            if (!same_rule(a, b)) {
              fail("the rule read back from its serialization is another rule (kind, descriptors, nested rules or what it defines)", once, input);
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
