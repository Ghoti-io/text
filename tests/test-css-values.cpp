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
 * The typed value model (css_value.h): the units of Values 4 and 5, the keyword
 * atoms, the shape and fields of the values a match produces, the scan for
 * arbitrary substitution functions, and specified-value serialization with its
 * fixed point (serialize, match, serialize).
 *
 * Normative text: CSS Values and Units Level 4 Editor's Draft (served
 * 2026-10-22, dated 24 August 2026): section 6 for the units and "Like
 * keywords, unit identifiers are ASCII case-insensitive"; CSSOM for the
 * serialization of numbers, strings and identifiers. The draft does not say
 * which case a serialized unit takes, so the spelling table in css_units.c is
 * this library's decision (lower case, except Q, Hz and kHz).
 */

#include <cmath>
#include <cstring>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "css_values_test_support.h"
#include "markup_test_support.h"

using namespace gv;

namespace {

struct UnitRow {
  const char * name;
  GTEXT_CSS_Unit unit;
  GTEXT_CSS_Unit_Type type;
  bool absolute;
};

const UnitRow kUnitRows[] = {
    {"cm", GTEXT_CSS_UNIT_CM, GTEXT_CSS_UNIT_TYPE_LENGTH, true}, {"mm", GTEXT_CSS_UNIT_MM, GTEXT_CSS_UNIT_TYPE_LENGTH, true},
    {"Q", GTEXT_CSS_UNIT_Q, GTEXT_CSS_UNIT_TYPE_LENGTH, true}, {"in", GTEXT_CSS_UNIT_IN, GTEXT_CSS_UNIT_TYPE_LENGTH, true},
    {"pc", GTEXT_CSS_UNIT_PC, GTEXT_CSS_UNIT_TYPE_LENGTH, true}, {"pt", GTEXT_CSS_UNIT_PT, GTEXT_CSS_UNIT_TYPE_LENGTH, true},
    {"px", GTEXT_CSS_UNIT_PX, GTEXT_CSS_UNIT_TYPE_LENGTH, true},
    {"em", GTEXT_CSS_UNIT_EM, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"rem", GTEXT_CSS_UNIT_REM, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"ex", GTEXT_CSS_UNIT_EX, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"rex", GTEXT_CSS_UNIT_REX, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"cap", GTEXT_CSS_UNIT_CAP, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"rcap", GTEXT_CSS_UNIT_RCAP, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"ch", GTEXT_CSS_UNIT_CH, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"rch", GTEXT_CSS_UNIT_RCH, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"ic", GTEXT_CSS_UNIT_IC, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"ric", GTEXT_CSS_UNIT_RIC, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"lh", GTEXT_CSS_UNIT_LH, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"rlh", GTEXT_CSS_UNIT_RLH, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"vw", GTEXT_CSS_UNIT_VW, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"svw", GTEXT_CSS_UNIT_SVW, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"lvw", GTEXT_CSS_UNIT_LVW, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"dvw", GTEXT_CSS_UNIT_DVW, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"vh", GTEXT_CSS_UNIT_VH, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"svh", GTEXT_CSS_UNIT_SVH, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"lvh", GTEXT_CSS_UNIT_LVH, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"dvh", GTEXT_CSS_UNIT_DVH, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"vi", GTEXT_CSS_UNIT_VI, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"svi", GTEXT_CSS_UNIT_SVI, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"lvi", GTEXT_CSS_UNIT_LVI, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"dvi", GTEXT_CSS_UNIT_DVI, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"vb", GTEXT_CSS_UNIT_VB, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"svb", GTEXT_CSS_UNIT_SVB, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"lvb", GTEXT_CSS_UNIT_LVB, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"dvb", GTEXT_CSS_UNIT_DVB, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"vmin", GTEXT_CSS_UNIT_VMIN, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"svmin", GTEXT_CSS_UNIT_SVMIN, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"lvmin", GTEXT_CSS_UNIT_LVMIN, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"dvmin", GTEXT_CSS_UNIT_DVMIN, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"vmax", GTEXT_CSS_UNIT_VMAX, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"svmax", GTEXT_CSS_UNIT_SVMAX, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"lvmax", GTEXT_CSS_UNIT_LVMAX, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"dvmax", GTEXT_CSS_UNIT_DVMAX, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"cqw", GTEXT_CSS_UNIT_CQW, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"cqh", GTEXT_CSS_UNIT_CQH, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"cqi", GTEXT_CSS_UNIT_CQI, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"cqb", GTEXT_CSS_UNIT_CQB, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"cqmin", GTEXT_CSS_UNIT_CQMIN, GTEXT_CSS_UNIT_TYPE_LENGTH, false}, {"cqmax", GTEXT_CSS_UNIT_CQMAX, GTEXT_CSS_UNIT_TYPE_LENGTH, false},
    {"deg", GTEXT_CSS_UNIT_DEG, GTEXT_CSS_UNIT_TYPE_ANGLE, true}, {"grad", GTEXT_CSS_UNIT_GRAD, GTEXT_CSS_UNIT_TYPE_ANGLE, true},
    {"rad", GTEXT_CSS_UNIT_RAD, GTEXT_CSS_UNIT_TYPE_ANGLE, true}, {"turn", GTEXT_CSS_UNIT_TURN, GTEXT_CSS_UNIT_TYPE_ANGLE, true},
    {"s", GTEXT_CSS_UNIT_S, GTEXT_CSS_UNIT_TYPE_TIME, true}, {"ms", GTEXT_CSS_UNIT_MS, GTEXT_CSS_UNIT_TYPE_TIME, true},
    {"Hz", GTEXT_CSS_UNIT_HZ, GTEXT_CSS_UNIT_TYPE_FREQUENCY, true}, {"kHz", GTEXT_CSS_UNIT_KHZ, GTEXT_CSS_UNIT_TYPE_FREQUENCY, true},
    {"dpi", GTEXT_CSS_UNIT_DPI, GTEXT_CSS_UNIT_TYPE_RESOLUTION, true}, {"dpcm", GTEXT_CSS_UNIT_DPCM, GTEXT_CSS_UNIT_TYPE_RESOLUTION, true},
    {"dppx", GTEXT_CSS_UNIT_DPPX, GTEXT_CSS_UNIT_TYPE_RESOLUTION, true}, {"x", GTEXT_CSS_UNIT_X, GTEXT_CSS_UNIT_TYPE_RESOLUTION, true},
    {"fr", GTEXT_CSS_UNIT_FR, GTEXT_CSS_UNIT_TYPE_FLEX, true},
};

std::string upper(std::string s) {
  for (char & c : s) {
    c = (char)toupper((unsigned char)c);
  }
  return s;
}

TEST(CssUnits, EveryUnitOfValues4And5IsKnownByNameInAnyCase) {
  EXPECT_EQ(sizeof kUnitRows / sizeof kUnitRows[0], (size_t)GTEXT_CSS_UNIT_COUNT - 1);
  std::set<GTEXT_CSS_Unit> seen;
  for (const UnitRow & r : kUnitRows) {
    const std::string n = r.name;
    EXPECT_EQ(gtext_css_unit_lookup(n.data(), n.size()), r.unit) << n;
    EXPECT_EQ(gtext_css_unit_lookup(upper(n).data(), n.size()), r.unit) << n;
    std::string lowered = n;
    for (char & c : lowered) {
      c = (char)tolower((unsigned char)c);
    }
    EXPECT_EQ(gtext_css_unit_lookup(lowered.data(), n.size()), r.unit) << n;
    EXPECT_STREQ(gtext_css_unit_name(r.unit), r.name);
    EXPECT_EQ(gtext_css_unit_type(r.unit), r.type) << n;
    EXPECT_EQ(gtext_css_unit_is_absolute(r.unit), r.absolute) << n;
    EXPECT_TRUE(seen.insert(r.unit).second) << n;
  }
}

TEST(CssUnits, NamesThatAreNotUnitsAreNone) {
  for (const char * n : {"", "p", "pxx", "xx", "foo", "mS ", "kh", "dppxx", "turns"}) {
    EXPECT_EQ(gtext_css_unit_lookup(n, strlen(n)), GTEXT_CSS_UNIT_NONE) << n;
  }
  EXPECT_EQ(gtext_css_unit_lookup(nullptr, 2), GTEXT_CSS_UNIT_NONE);
  EXPECT_EQ(gtext_css_unit_name(GTEXT_CSS_UNIT_NONE), nullptr);
  EXPECT_EQ(gtext_css_unit_name(GTEXT_CSS_UNIT_COUNT), nullptr);
  EXPECT_EQ(gtext_css_unit_type(GTEXT_CSS_UNIT_NONE), GTEXT_CSS_UNIT_TYPE_NONE);
  EXPECT_EQ(gtext_css_unit_canonical(GTEXT_CSS_UNIT_NONE), GTEXT_CSS_UNIT_NONE);
  EXPECT_FALSE(gtext_css_unit_is_absolute(GTEXT_CSS_UNIT_NONE));
}

double canon(GTEXT_CSS_Unit u, double v) {
  double out = -12345;
  EXPECT_TRUE(gtext_css_unit_to_canonical(u, v, &out));
  return out;
}

TEST(CssUnits, AbsoluteUnitsConvertByTheDraftsFixedRatios) {
  // 1in = 2.54cm = 25.4mm = 101.6Q = 72pt = 6pc = 96px
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_IN, 1), 96.0);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_PC, 6), 96.0);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_PT, 72), 96.0);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_PX, 5), 5.0);
  EXPECT_NEAR(canon(GTEXT_CSS_UNIT_CM, 2.54), 96.0, 1e-12);
  EXPECT_NEAR(canon(GTEXT_CSS_UNIT_MM, 25.4), 96.0, 1e-12);
  EXPECT_NEAR(canon(GTEXT_CSS_UNIT_Q, 101.6), 96.0, 1e-12);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_CM, 0), 0.0);
  // 1turn = 360deg = 400grad = 2 pi rad
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_TURN, 1), 360.0);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_GRAD, 400), 360.0);
  EXPECT_NEAR(canon(GTEXT_CSS_UNIT_RAD, 2 * 3.14159265358979323846), 360.0, 1e-12);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_DEG, 7), 7.0);
  // 1s = 1000ms, 1kHz = 1000Hz
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_MS, 1000), 1.0);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_S, 2), 2.0);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_KHZ, 1), 1000.0);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_HZ, 440), 440.0);
  // 1dppx = 1x = 96dpi = 96/2.54 dpcm
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_DPI, 96), 1.0);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_X, 2), 2.0);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_DPPX, 2), 2.0);
  EXPECT_NEAR(canon(GTEXT_CSS_UNIT_DPCM, 96 / 2.54), 1.0, 1e-12);
  EXPECT_EQ(canon(GTEXT_CSS_UNIT_FR, 3), 3.0);
  // an infinity stays one, a NaN a NaN, and the sign of a zero is kept
  EXPECT_TRUE(std::isinf(canon(GTEXT_CSS_UNIT_IN, INFINITY)));
  EXPECT_TRUE(std::isnan(canon(GTEXT_CSS_UNIT_IN, NAN)));
  EXPECT_TRUE(std::signbit(canon(GTEXT_CSS_UNIT_PT, -0.0)));
}

TEST(CssUnits, RelativeUnitsAreNeverConverted) {
  double out = 7;
  for (const UnitRow & r : kUnitRows) {
    if (!r.absolute) {
      EXPECT_FALSE(gtext_css_unit_to_canonical(r.unit, 1, &out)) << r.name;
    }
  }
  EXPECT_EQ(out, 7.0);
  EXPECT_FALSE(gtext_css_unit_to_canonical(GTEXT_CSS_UNIT_NONE, 1, &out));
  EXPECT_FALSE(gtext_css_unit_to_canonical(GTEXT_CSS_UNIT_PX, 1, nullptr));
}

TEST(CssUnits, EachTypeHasItsCanonicalUnit) {
  EXPECT_EQ(gtext_css_unit_canonical(GTEXT_CSS_UNIT_CM), GTEXT_CSS_UNIT_PX);
  EXPECT_EQ(gtext_css_unit_canonical(GTEXT_CSS_UNIT_EM), GTEXT_CSS_UNIT_PX);
  EXPECT_EQ(gtext_css_unit_canonical(GTEXT_CSS_UNIT_TURN), GTEXT_CSS_UNIT_DEG);
  EXPECT_EQ(gtext_css_unit_canonical(GTEXT_CSS_UNIT_MS), GTEXT_CSS_UNIT_S);
  EXPECT_EQ(gtext_css_unit_canonical(GTEXT_CSS_UNIT_KHZ), GTEXT_CSS_UNIT_HZ);
  EXPECT_EQ(gtext_css_unit_canonical(GTEXT_CSS_UNIT_DPI), GTEXT_CSS_UNIT_DPPX);
  EXPECT_EQ(gtext_css_unit_canonical(GTEXT_CSS_UNIT_FR), GTEXT_CSS_UNIT_FR);
}

TEST(CssAtoms, AnAtomIsPerKeywordWhateverTheCaseAndKeepsTheFirstSpelling) {
  GTEXT_CSS_Atom_Table * t = nullptr;
  ASSERT_EQ(gtext_css_atom_table_new(nullptr, &t), GTEXT_CSS_OK);
  GTEXT_CSS_Atom a = 0, b = 0, c = 0;
  ASSERT_EQ(gtext_css_atom_intern(t, "Auto", 4, &a), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_atom_intern(t, "AUTO", 4, &b), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_atom_intern(t, "none", 4, &c), GTEXT_CSS_OK);
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(a, GTEXT_CSS_ATOM_NONE);
  EXPECT_EQ(gtext_css_atom_count(t), 2u);
  size_t len = 0;
  EXPECT_STREQ(gtext_css_atom_name(t, a, &len), "Auto");
  EXPECT_EQ(len, 4u);
  EXPECT_EQ(gtext_css_atom_lookup(t, "aUtO", 4), a);
  EXPECT_EQ(gtext_css_atom_lookup(t, "aut", 3), GTEXT_CSS_ATOM_NONE);
  EXPECT_EQ(gtext_css_atom_lookup(t, "autos", 5), GTEXT_CSS_ATOM_NONE);
  EXPECT_EQ(gtext_css_atom_name(t, GTEXT_CSS_ATOM_NONE, nullptr), nullptr);
  EXPECT_EQ(gtext_css_atom_name(t, 99, nullptr), nullptr);
  EXPECT_EQ(gtext_css_atom_count(t), 2u);  // a lookup makes none
  gtext_css_atom_table_free(t);
}

TEST(CssAtoms, ATableGrowsPastItsFirstSizesAndEveryAtomStaysItself) {
  GTEXT_CSS_Atom_Table * t = nullptr;
  ASSERT_EQ(gtext_css_atom_table_new(nullptr, &t), GTEXT_CSS_OK);
  std::vector<GTEXT_CSS_Atom> atoms;
  for (int i = 0; i < 5000; i++) {
    const std::string k = "keyword-" + std::to_string(i);
    GTEXT_CSS_Atom a;
    ASSERT_EQ(gtext_css_atom_intern(t, k.data(), k.size(), &a), GTEXT_CSS_OK);
    atoms.push_back(a);
  }
  EXPECT_EQ(gtext_css_atom_count(t), 5000u);
  EXPECT_EQ(std::set<GTEXT_CSS_Atom>(atoms.begin(), atoms.end()).size(), 5000u);
  for (int i = 0; i < 5000; i++) {
    const std::string k = "KEYWORD-" + std::to_string(i);
    EXPECT_EQ(gtext_css_atom_lookup(t, k.data(), k.size()), atoms[i]);
  }
  gtext_css_atom_table_free(t);
}

TEST(CssAtoms, BadArgumentsAreRefused) {
  GTEXT_CSS_Atom a = 77;
  GTEXT_CSS_Atom_Table * t = nullptr;
  EXPECT_EQ(gtext_css_atom_table_new(nullptr, nullptr), GTEXT_CSS_E_INVALID);
  ASSERT_EQ(gtext_css_atom_table_new(nullptr, &t), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_atom_intern(t, "", 0, &a), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_atom_intern(t, nullptr, 3, &a), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_atom_intern(nullptr, "a", 1, &a), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_atom_intern(t, "a", 1, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(a, 77u);
  EXPECT_EQ(gtext_css_atom_lookup(nullptr, "a", 1), GTEXT_CSS_ATOM_NONE);
  EXPECT_EQ(gtext_css_atom_count(nullptr), 0u);
  gtext_css_atom_table_free(nullptr);
  gtext_css_atom_table_free(t);
}

TEST(CssAtoms, AFailedAllocationIsOutOfMemoryAndLeavesTheTableUsable) {
  for (size_t n = 1; n < 40; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Atom_Table * t = nullptr;
    fa.arm(n);
    GTEXT_CSS_Status st = gtext_css_atom_table_new(&fa.alloc, &t);
    if (st == GTEXT_CSS_OK) {
      for (int i = 0; i < 30 && st == GTEXT_CSS_OK; i++) {
        const std::string k = "k" + std::to_string(i);
        GTEXT_CSS_Atom a;
        st = gtext_css_atom_intern(t, k.data(), k.size(), &a);
      }
    }
    fa.disarm();
    EXPECT_TRUE(st == GTEXT_CSS_OK || st == GTEXT_CSS_E_OOM) << n;
    if (t) {
      GTEXT_CSS_Atom a;
      EXPECT_EQ(gtext_css_atom_intern(t, "after", 5, &a), GTEXT_CSS_OK);
      gtext_css_atom_table_free(t);
    }
    EXPECT_TRUE(fa.live.empty()) << n;
    EXPECT_EQ(fa.bad_frees, 0u);
  }
}

TEST(CssValues, EachKindCarriesTheFieldsItIsDocumentedToCarry) {
  Defs d;
  auto one = [&](const char * g, const char * in, Matched & m, Grammar & gr) {
    compile_into(gr, g, &d);
    ASSERT_TRUE(gr.ok()) << g;
    match_into(m, gr, in);
    ASSERT_TRUE(m.matched) << g << " " << in;
    ASSERT_NE(m.root(), nullptr);
  };
  {
    Grammar g; Matched m;
    one("auto", "AUTO", m, g);
    const GTEXT_CSS_Value * v = m.root();
    EXPECT_EQ(v->kind, GTEXT_CSS_VAL_KEYWORD);
    EXPECT_NE(v->atom, GTEXT_CSS_ATOM_NONE);
    EXPECT_EQ(css_test::str(v->text), "auto");
    EXPECT_EQ(v->interp, GTEXT_CSS_INTERP_DISCRETE);
    EXPECT_EQ(v->child_count, 0u);
    EXPECT_EQ(v->component, m.parsed.tree->values[0].id);
    EXPECT_EQ(v->span.end, 4u);
    EXPECT_EQ(std::string(gtext_css_val_kind_name(v->kind)), "keyword");
  }
  {
    Grammar g; Matched m;
    one("<integer>", "-7", m, g);
    EXPECT_EQ(m.root()->kind, GTEXT_CSS_VAL_INTEGER);
    EXPECT_EQ(m.root()->number, -7.0);
    EXPECT_EQ(m.root()->interp, GTEXT_CSS_INTERP_NUMBER);
  }
  {
    Grammar g; Matched m;
    one("<number>", "7", m, g);
    EXPECT_EQ(m.root()->kind, GTEXT_CSS_VAL_NUMBER);
    EXPECT_NE(m.root()->flags & GTEXT_CSS_VFLAG_WRITTEN_INTEGER, 0u);
  }
  {
    Grammar g; Matched m;
    one("<percentage>", "12.5%", m, g);
    EXPECT_EQ(m.root()->kind, GTEXT_CSS_VAL_PERCENTAGE);
    EXPECT_EQ(m.root()->number, 12.5);
    EXPECT_EQ(m.root()->interp, GTEXT_CSS_INTERP_PERCENTAGE);
  }
  struct DimCase { const char * g, * in; GTEXT_CSS_Unit unit; GTEXT_CSS_Interp interp; };
  for (const DimCase & c : std::vector<DimCase>{{"<length>", "3em", GTEXT_CSS_UNIT_EM, GTEXT_CSS_INTERP_LENGTH},
           {"<angle>", "3rad", GTEXT_CSS_UNIT_RAD, GTEXT_CSS_INTERP_ANGLE}, {"<time>", "3ms", GTEXT_CSS_UNIT_MS, GTEXT_CSS_INTERP_TIME},
           {"<frequency>", "3khz", GTEXT_CSS_UNIT_KHZ, GTEXT_CSS_INTERP_DISCRETE},
           {"<resolution>", "3dpi", GTEXT_CSS_UNIT_DPI, GTEXT_CSS_INTERP_DISCRETE}, {"<flex>", "3fr", GTEXT_CSS_UNIT_FR, GTEXT_CSS_INTERP_DISCRETE}}) {
    Grammar g; Matched m;
    one(c.g, c.in, m, g);
    EXPECT_EQ(m.root()->kind, GTEXT_CSS_VAL_DIMENSION) << c.g;
    EXPECT_EQ(m.root()->unit, c.unit) << c.g;
    EXPECT_EQ(m.root()->interp, c.interp) << c.g;
    EXPECT_EQ(m.root()->number, 3.0);
  }
  {
    Grammar g; Matched m;
    one("<dimension>", "3Foo", m, g);
    EXPECT_EQ(m.root()->unit, GTEXT_CSS_UNIT_NONE);
    EXPECT_EQ(css_test::str(m.root()->text), "Foo");
  }
  {
    Grammar g; Matched m;
    one("<ratio>", "16/9", m, g);
    EXPECT_EQ(m.root()->kind, GTEXT_CSS_VAL_RATIO);
    EXPECT_EQ(m.root()->number, 16.0);
    EXPECT_EQ(m.root()->number2, 9.0);
    EXPECT_NE(m.root()->flags & GTEXT_CSS_VFLAG_RATIO_DENOMINATOR, 0u);
  }
  {
    Grammar g; Matched m;
    one("<url>", "url('a')", m, g);
    EXPECT_EQ(m.root()->kind, GTEXT_CSS_VAL_URL);
    EXPECT_EQ(css_test::str(m.root()->text), "a");
  }
  {
    Grammar g;
    compile_into(g, "<urange>", &d);
    Matched m;
    match_declaration(m, g, "unicode-range: U+1-5");
    ASSERT_TRUE(m.matched);
    EXPECT_EQ(m.root()->kind, GTEXT_CSS_VAL_UNICODE_RANGE);
    EXPECT_EQ(m.root()->range_start, 1u);
    EXPECT_EQ(m.root()->range_end, 5u);
  }
}

TEST(CssValues, AListIsInterpolatedItemByItemAndKnowsItsSeparatorAndSpan) {
  Grammar g;
  compile_into(g, "<length> , <length>");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_into(m, g, "1px,2px");
  ASSERT_TRUE(m.matched);
  EXPECT_EQ(m.root()->kind, GTEXT_CSS_VAL_LIST);
  EXPECT_EQ(m.root()->separator, GTEXT_CSS_SEP_COMMA);
  EXPECT_EQ(m.root()->interp, GTEXT_CSS_INTERP_LIST);
  EXPECT_EQ(m.root()->span.offset, 0u);
  EXPECT_EQ(m.root()->span.end, 7u);
  EXPECT_EQ(m.root()->child_count, 2u);
}

size_t count_values(const GTEXT_CSS_Value * v) {
  size_t n = 1;
  for (size_t i = 0; i < v->child_count; i++) {
    n += count_values(&v->children[i]);
  }
  return n;
}

TEST(CssValues, ASetOutlivesTheTreeTheGrammarAndTheResolver) {
  GTEXT_CSS_Value_Set * set = nullptr;
  {
    Grammar g;
    Defs d;
    d.types["t"] = "x | <ident>";
    compile_into(g, "[ <t> | <url> | <string> ]+ foo( <number> )", &d);
    ASSERT_TRUE(g.ok());
    Matched m;
    match_into(m, g, "a url(b) 'c' foo(1)");
    ASSERT_TRUE(m.matched);
    set = m.set;
    m.set = nullptr;
  }
  ASSERT_NE(set, nullptr);
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
  EXPECT_EQ(dump(root), "list/space(list/space(kw:a url:b str:c) fn:foo(num:1))");
  EXPECT_EQ(gtext_css_value_set_count(set), count_values(root));
  EXPECT_EQ(serialize(root), "a url(\"b\") \"c\" foo(1)");
  gtext_css_value_set_free(set);
  EXPECT_EQ(gtext_css_value_set_root(nullptr), nullptr);
  EXPECT_EQ(gtext_css_value_set_count(nullptr), 0u);
}

TEST(CssValues, TheCountOfAValueSetIsTheNumberOfValuesInIt) {
  Grammar g;
  compile_into(g, "[ a | b c | <length> ]* d?");
  ASSERT_TRUE(g.ok());
  for (const char * in : {"a", "a b c 1px d", "d", "b c b c"}) {
    Matched m;
    match_into(m, g, in);
    ASSERT_TRUE(m.matched) << in;
    EXPECT_EQ(gtext_css_value_set_count(m.set), count_values(m.root())) << in;
  }
}

TEST(CssValues, KindNamesAreDistinct) {
  std::set<std::string> names;
  for (int k = 0; k < GTEXT_CSS_VAL_KIND_COUNT; k++) {
    EXPECT_TRUE(names.insert(gtext_css_val_kind_name((GTEXT_CSS_Val_Kind)k)).second);
  }
  EXPECT_EQ(std::string(gtext_css_val_kind_name(GTEXT_CSS_VAL_KIND_COUNT)), "unknown");
}

bool has_substitution(const std::string & css, GTEXT_CSS_Status * st = nullptr) {
  css_test::Parsed p;
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_depth = 200000;
  o.max_tokens = 4000000;
  css_test::parse_into(p, gtext_css_parse_component_values, css, &o);
  bool found = false;
  const GTEXT_CSS_Status s = gtext_css_has_arbitrary_substitution(p.tree->values, p.tree->value_count, nullptr, &found);
  if (st) {
    *st = s;
  }
  return s == GTEXT_CSS_OK && found;
}

TEST(CssSubstitution, VarEnvAndAttrAreFoundAnywhereAtAllInAValue) {
  EXPECT_TRUE(has_substitution("var(--x)"));
  EXPECT_TRUE(has_substitution("a var(--x) b"));
  EXPECT_TRUE(has_substitution("env(x)"));
  EXPECT_TRUE(has_substitution("attr(x)"));
  EXPECT_TRUE(has_substitution("VAR(--x)"));
  EXPECT_TRUE(has_substitution("Env(x)"));
  EXPECT_TRUE(has_substitution("calc(1px + var(--x))"));
  EXPECT_TRUE(has_substitution("a(b(c(d(var(--x)))))"));
  EXPECT_TRUE(has_substitution("[ ( { var(--x) } ) ]"));
  EXPECT_TRUE(has_substitution("a, b, var(--x)"));
  EXPECT_TRUE(has_substitution("e\\nv(x)"));  // an escape is read: \\n is "n"
}

TEST(CssSubstitution, ANameThatIsNotAFunctionOrNotThoseThreeIsNot) {
  EXPECT_FALSE(has_substitution(""));
  EXPECT_FALSE(has_substitution("var"));
  EXPECT_FALSE(has_substitution("var (--x)"));
  EXPECT_FALSE(has_substitution("--var(x)"));
  EXPECT_FALSE(has_substitution("variable(--x)"));
  EXPECT_FALSE(has_substitution("a(b) c d"));
  EXPECT_FALSE(has_substitution("'var(--x)'"));
  EXPECT_FALSE(has_substitution("url(var(x))"));
}

TEST(CssSubstitution, ADeepValueIsWalkedWithoutRecursion) {
  std::string deep;
  const int n = 100000;
  for (int i = 0; i < n; i++) {
    deep += "f(";
  }
  deep += "var(--x)";
  for (int i = 0; i < n; i++) {
    deep += ")";
  }
  EXPECT_TRUE(has_substitution(deep));
  std::string none;
  for (int i = 0; i < n; i++) {
    none += "f(";
  }
  for (int i = 0; i < n; i++) {
    none += ")";
  }
  EXPECT_FALSE(has_substitution(none));
}

TEST(CssSubstitution, BadArgumentsAndAFailedAllocation) {
  bool found = true;
  EXPECT_EQ(gtext_css_has_arbitrary_substitution(nullptr, 1, nullptr, &found), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_has_arbitrary_substitution(nullptr, 0, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_has_arbitrary_substitution(nullptr, 0, nullptr, &found), GTEXT_CSS_OK);
  EXPECT_FALSE(found);
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_component_values, "a(b(c))");
  FaultAllocator fa;
  fa.arm(1);
  found = true;
  EXPECT_EQ(gtext_css_has_arbitrary_substitution(p.tree->values, p.tree->value_count, &fa.alloc, &found), GTEXT_CSS_E_OOM);
  EXPECT_TRUE(found);
  fa.arm(2);
  EXPECT_EQ(gtext_css_has_arbitrary_substitution(p.tree->values, p.tree->value_count, &fa.alloc, &found), GTEXT_CSS_OK);
  fa.disarm();
  EXPECT_TRUE(fa.live.empty());
}

TEST(CssSubstitution, APendingValueCarriesTheTextAndSpanOfTheWhole) {
  Grammar g;
  compile_into(g, "<length>");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_into(m, g, "  calc( var(--a) + 1px )  ");
  ASSERT_TRUE(m.matched);
  EXPECT_EQ(m.root()->kind, GTEXT_CSS_VAL_PENDING);
  EXPECT_EQ(m.root()->span.offset, 2u);
  EXPECT_NE(css_test::str(m.root()->text).find("var(--a)"), std::string::npos);
  Matched n;
  match_into(n, g, "var(--a)", nullptr);
  EXPECT_EQ(n.dump(), "pending:var(--a)");
}

struct SerRow {
  const char * grammar;
  const char * input;
  const char * text;
};

void check_serialization(const std::vector<SerRow> & rows, Defs * d = nullptr) {
  for (const SerRow & r : rows) {
    const std::string first = run_text(r.grammar, r.input, d);
    EXPECT_EQ(first, r.text) << r.grammar << " | " << r.input;
    // the fixed point: the text matches the same grammar and is written the same way
    EXPECT_EQ(run_text(r.grammar, first, d), first) << "second pass of: " << r.grammar << " | " << r.input;
  }
}

TEST(CssValueSerialize, NumbersAreCssomsShortestSixDecimalForm) {
  check_serialization({
      {"<number>", "1.50", "1.5"}, {"<number>", "+5", "5"}, {"<number>", "1e3", "1000"}, {"<number>", ".5", "0.5"},
      {"<number>", "-0", "0"}, {"<number>", "1e-7", "0"}, {"<number>", "0.1234567", "0.123457"},
      {"<number>", "1e21", "1000000000000000000000"}, {"<number>", "-.25", "-0.25"}, {"<number>", "0.0000005", "0.000001"},
      {"<number>", "123456.789012345", "123456.789012"}, {"<integer>", "+7", "7"}, {"<integer>", "-0", "0"},
      {"<percentage>", "50.0%", "50%"}, {"<percentage>", "+1e1%", "10%"},
      {"<length>", "1.0px", "1px"}, {"<length>", "-.5em", "-0.5em"}, {"<number>", "0.30000000000000004", "0.3"},
      {"<ratio>", "16 / 9", "16 / 9"}, {"<ratio>", "2", "2 / 1"}, {"<ratio>", "1.5/2", "1.5 / 2"},
      {"<zero>", "0.0", "0"},
  });
}

TEST(CssValueSerialize, ADimensionsUnitIsInTheCanonicalSpellingAndAnUnknownOneAsWritten) {
  check_serialization({
      {"<length>", "10PX", "10px"}, {"<length>", "1q", "1Q"}, {"<length>", "1Q", "1Q"}, {"<frequency>", "1KHZ", "1kHz"},
      {"<frequency>", "1hz", "1Hz"}, {"<angle>", "1TURN", "1turn"}, {"<length>", "0", "0px"}, {"<length>", "-0", "0px"},
      {"<dimension>", "3Foo", "3Foo"}, {"<dimension>", "3e", "3e"}, {"<resolution>", "2X", "2x"}, {"<length>", "1DVMIN", "1dvmin"},
  });
}

TEST(CssValueSerialize, StringsAndIdentifiersFollowCssom) {
  check_serialization({
      {"<string>", "'a\"b'", "\"a\\\"b\""}, {"<string>", "'a\\\\b'", "\"a\\\\b\""}, {"<string>", "'a\\a b'", "\"a\\a b\""},
      {"<string>", "''", "\"\""}, {"<string>", "'\\1F600'", "\"\xF0\x9F\x98\x80\""},
      {"<custom-ident>", "a\\ b", "a\\ b"}, {"<custom-ident>", "Foo", "Foo"}, {"<custom-ident>", "\\31 23", "\\31 23"},
      {"<dashed-ident>", "--Foo", "--Foo"}, {"<ident>", "\\-", "\\-"}, {"<ident>", "-a", "-a"},
      {"auto | none", "AUTO", "auto"}, {"sRGB | display-p3", "srgb", "sRGB"}, {"<ident>", "Auto", "Auto"},
      {"<url>", "url(a.png)", "url(\"a.png\")"}, {"<url>", "url('a b')", "url(\"a b\")"}, {"<url>", "url(\"a\\\"b\")", "url(\"a\\\"b\")"},
      {"<url>", "url('a' foo)", "url(\"a\" foo)"},
  });
}

TEST(CssValueSerialize, ListsUseTheirSeparators) {
  check_serialization({
      {"<length>{1,4}", "1px 2px", "1px 2px"}, {"<length>#", "1px,2px ,3px", "1px, 2px, 3px"},
      {"<length> / <length>", "1px/2px", "1px / 2px"}, {"<length> , <length>", "1px,2px", "1px, 2px"},
      {"<length> / <length> , <length>", "1px/2px,3px", "1px / 2px, 3px"}, {"<number>+#", "1 2,3", "1 2, 3"},
      {"a '/' b", "a / b", "a / b"}, {"a? / b", "/ b", "/ b"}, {"a b , c", "a b, c", "a b, c"},
      {"[a b?]+", "a b a", "a b a"}, {"a || b || c", "c a", "a c"},
  });
}

TEST(CssValueSerialize, FunctionsBlocksAndOpaqueValuesAreWrittenAsTheyWere) {
  Defs d;
  d.types["rgb()"] = "rgb( <number>#{3} )";
  check_serialization({
      {"<rgb()>", "RGB(1,2 ,3)", "rgb(1, 2, 3)"}, {"foo( bar( a ) )", "foo(bar(a))", "foo(bar(a))"}, {"foo()", "foo( )", "foo()"},
      {"<length>", "calc(1px+2px)", "calc(1px/**/+2px)"}, {"<length>", "CALC( 1px  +  2px )", "calc( 1px + 2px )"},
      {"<any-value>", "a  b   c", "a b c"}, {"( <number> )", "(3)", "(3)"}, {"{ <declaration-list> }", "{a:b}", "{a:b}"},
      {"<an+b>", "even", "2n"}, {"<length>", "var(--a, 1px)", "var(--a, 1px)"}, {"<ident-token>", "foo", "foo"},
  }, &d);
}

TEST(CssValueSerialize, AUnicodeRangeIsWrittenWithItsBounds) {
  Grammar g;
  compile_into(g, "<urange>#");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_declaration(m, g, "unicode-range: u+26, U+0025-00FF, u+4??");
  ASSERT_TRUE(m.matched);
  EXPECT_EQ(m.text(), "U+26, U+25-FF, U+400-4FF");
  Matched m2;
  match_declaration(m2, g, "unicode-range: " + m.text());
  ASSERT_TRUE(m2.matched);
  EXPECT_EQ(m2.text(), m.text());
}

struct Capture {
  std::string text;
  size_t fail_after = SIZE_MAX;
};

GTEXT_Markup_Status capture_write(void * user, const char * b, size_t n) {
  Capture * c = static_cast<Capture *>(user);
  if (c->text.size() + n > c->fail_after) {
    return GTEXT_MARKUP_E_WRITE;
  }
  c->text.append(b, n);
  return GTEXT_MARKUP_OK;
}

TEST(CssValueSerialize, TheOutputLimitIsExactAndTheSinkGetsAWholePrefix) {
  Grammar g;
  compile_into(g, "<string> , <length>");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_into(m, g, "'a\\1F600 b', 12px");
  ASSERT_TRUE(m.matched);
  const std::string full = m.text();
  for (size_t limit = 0; limit <= full.size(); limit++) {
    GTEXT_CSS_Serialize_Options o = gtext_css_serialize_options_default();
    o.max_output_bytes = limit ? limit : 1;
    Capture c;
    GTEXT_Markup_Sink sink = {capture_write, &c};
    const GTEXT_CSS_Status st = gtext_css_serialize_value(m.root(), &o, &sink);
    if (limit >= full.size()) {
      EXPECT_EQ(st, GTEXT_CSS_OK) << limit;
      EXPECT_EQ(c.text, full);
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_LIMIT_OUTPUT) << limit;
      EXPECT_LE(c.text.size(), o.max_output_bytes);
      EXPECT_EQ(full.compare(0, c.text.size(), c.text), 0) << "a prefix of the text";
    }
  }
}

TEST(CssValueSerialize, ASinkThatRefusesIsAWriteErrorAndTheBufferFormsAreFreedByTheirFunction) {
  Grammar g;
  compile_into(g, "a b c");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_into(m, g, "a b c");
  Capture c;
  c.fail_after = 2;
  GTEXT_Markup_Sink sink = {capture_write, &c};
  EXPECT_EQ(gtext_css_serialize_value(m.root(), nullptr, &sink), GTEXT_CSS_E_WRITE);
  char * buf = nullptr;
  size_t len = 0;
  ASSERT_EQ(gtext_css_serialize_value_buffer(m.root(), nullptr, &buf, &len), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(buf, len), "a b c");
  EXPECT_EQ(buf[len], '\0');
  gtext_css_serialize_buffer_free(nullptr, buf);
  EXPECT_EQ(gtext_css_serialize_value(nullptr, nullptr, &sink), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_serialize_value(m.root(), nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_serialize_value_buffer(m.root(), nullptr, nullptr, &len), GTEXT_CSS_E_INVALID);
}

TEST(CssValueSerialize, EveryAllocationOfTheBufferFormCanFail) {
  Grammar g;
  compile_into(g, "[ a | b c ]+ foo( <number>#{2} )");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_into(m, g, "a b c foo(1,2)");
  ASSERT_TRUE(m.matched);
  for (size_t n = 1; n < 30; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Serialize_Options o = gtext_css_serialize_options_default();
    o.allocator = &fa.alloc;
    fa.arm(n);
    char * buf = nullptr;
    size_t len = 0;
    const GTEXT_CSS_Status st = gtext_css_serialize_value_buffer(m.root(), &o, &buf, &len);
    fa.disarm();
    if (st == GTEXT_CSS_OK) {
      EXPECT_EQ(std::string(buf, len), "a b c foo(1, 2)");
      gtext_css_serialize_buffer_free(&o, buf);
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      EXPECT_EQ(buf, nullptr);
    }
    EXPECT_TRUE(fa.live.empty()) << n;
  }
}

TEST(CssValueSerialize, ADeepValueIsWrittenWithoutRecursion) {
  Defs d;
  d.types["x"] = "f( <x> ) | a";
  Grammar g;
  compile_into(g, "<x>", &d);
  ASSERT_TRUE(g.ok());
  const int depth = 20000;
  std::string in;
  for (int i = 0; i < depth; i++) {
    in += "f(";
  }
  in += "a";
  for (int i = 0; i < depth; i++) {
    in += ")";
  }
  css_test::Parsed p;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.max_depth = 100000;
  css_test::parse_into(p, gtext_css_parse_component_values, in, &po);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  Matched m;
  match_values(m, g, p.tree->values, p.tree->value_count);
  ASSERT_EQ(m.status, GTEXT_CSS_OK);
  ASSERT_TRUE(m.matched);
  EXPECT_EQ(m.text(), in);
}


TEST(CssAtoms, ATableWithALimitRefusesWhatPassesItWithALimitStatusAndKeepsWhatItHas) {
  GTEXT_CSS_Atom_Table * t = nullptr;
  ASSERT_EQ(gtext_css_atom_table_new(nullptr, &t), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_atom_table_limits(t, 3, 0), GTEXT_CSS_OK);
  GTEXT_CSS_Atom a, b, c, d = 77;
  EXPECT_EQ(gtext_css_atom_intern(t, "a", 1, &a), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_atom_intern(t, "b", 1, &b), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_atom_intern(t, "c", 1, &c), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_atom_intern(t, "d", 1, &d), GTEXT_CSS_E_LIMIT_ATOMS);
  EXPECT_EQ(d, 77u);
  GTEXT_CSS_Atom again;
  EXPECT_EQ(gtext_css_atom_intern(t, "B", 1, &again), GTEXT_CSS_OK) << "a keyword the table has is not new";
  EXPECT_EQ(again, b);
  EXPECT_EQ(gtext_css_atom_count(t), 3u);
  // bytes: each spelling and its NUL
  ASSERT_EQ(gtext_css_atom_table_limits(t, 0, 0), GTEXT_CSS_OK);
  gtext_css_atom_table_free(t);
  ASSERT_EQ(gtext_css_atom_table_new(nullptr, &t), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_atom_table_limits(t, 0, 10), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_atom_intern(t, "abcd", 4, &a), GTEXT_CSS_OK);   // 5 bytes
  EXPECT_EQ(gtext_css_atom_intern(t, "efgh", 4, &b), GTEXT_CSS_OK);   // 10 bytes
  EXPECT_EQ(gtext_css_atom_intern(t, "i", 1, &c), GTEXT_CSS_E_LIMIT_ATOMS);
  EXPECT_EQ(gtext_css_atom_intern(t, "abcdefghijklmnop", 16, &c), GTEXT_CSS_E_LIMIT_ATOMS);
  EXPECT_EQ(gtext_css_atom_count(t), 2u);
  EXPECT_STREQ(gtext_css_atom_name(t, b, nullptr), "efgh");
  // a limit beyond what a 32-bit atom and offset hold is that maximum, and 0 is the default
  EXPECT_EQ(gtext_css_atom_table_limits(t, SIZE_MAX, SIZE_MAX), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_atom_intern(t, "i", 1, &c), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_atom_table_limits(nullptr, 1, 1), GTEXT_CSS_E_INVALID);
  gtext_css_atom_table_free(t);
  EXPECT_NE(std::string(gtext_css_status_string(GTEXT_CSS_E_LIMIT_ATOMS)), "unknown status");
}

TEST(CssAtoms, EveryKeywordOfAGrammarIsInItsAtomTable) {
  std::string many = "k0";
  for (int i = 1; i < 300; i++) {
    many += " | k" + std::to_string(i);
  }
  Grammar g;
  compile_into(g, many);
  ASSERT_TRUE(g.ok());
  EXPECT_EQ(gtext_css_atom_count(gtext_css_compiled_atoms(g.c)), 300u);
}

void zero_value(GTEXT_CSS_Value * v, GTEXT_CSS_Val_Kind kind) {
  std::memset(v, 0, sizeof *v);
  v->kind = kind;
}

TEST(CssValueSerialize, AFailureOfTheIdentifierAndStringWritersIsTheStatusNotATruncatedOk) {
  Capture c;
  GTEXT_Markup_Sink sink = {capture_write, &c};
  // a text that is NULL with a length is no text: the serializers refuse it, and so does the value's
  for (GTEXT_CSS_Val_Kind k : {GTEXT_CSS_VAL_KEYWORD, GTEXT_CSS_VAL_CUSTOM_IDENT, GTEXT_CSS_VAL_DASHED_IDENT, GTEXT_CSS_VAL_STRING,
           GTEXT_CSS_VAL_URL, GTEXT_CSS_VAL_UNPARSED, GTEXT_CSS_VAL_PENDING, GTEXT_CSS_VAL_TOKEN, GTEXT_CSS_VAL_FUNCTION}) {
    GTEXT_CSS_Value v;
    zero_value(&v, k);
    v.text.data = nullptr;
    v.text.length = 5;
    c.text.clear();
    EXPECT_EQ(gtext_css_serialize_value(&v, nullptr, &sink), GTEXT_CSS_E_INVALID) << gtext_css_val_kind_name(k);
    char * buf = nullptr;
    size_t len = 0;
    EXPECT_EQ(gtext_css_serialize_value_buffer(&v, nullptr, &buf, &len), GTEXT_CSS_E_INVALID) << gtext_css_val_kind_name(k);
    EXPECT_EQ(buf, nullptr);
  }
  // and the same inside a list: what was written before it is a valid prefix, the status is the failure
  GTEXT_CSS_Value kids[2], list;
  zero_value(&kids[0], GTEXT_CSS_VAL_NUMBER);
  kids[0].number = 3;
  zero_value(&kids[1], GTEXT_CSS_VAL_STRING);
  kids[1].text.length = 2;
  zero_value(&list, GTEXT_CSS_VAL_LIST);
  list.children = kids;
  list.child_count = 2;
  c.text.clear();
  EXPECT_EQ(gtext_css_serialize_value(&list, nullptr, &sink), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(c.text.substr(0, 2), "3 ");
  // a sink that refuses in the middle of an identifier or a string is a write error
  GTEXT_CSS_Value ident, str;
  zero_value(&ident, GTEXT_CSS_VAL_KEYWORD);
  ident.text.data = "abcdef";
  ident.text.length = 6;
  zero_value(&str, GTEXT_CSS_VAL_STRING);
  str.text.data = "abcdef";
  str.text.length = 6;
  for (const GTEXT_CSS_Value * v : {&ident, &str}) {
    Capture failing;
    failing.fail_after = 3;
    GTEXT_Markup_Sink fs = {capture_write, &failing};
    EXPECT_EQ(gtext_css_serialize_value(v, nullptr, &fs), GTEXT_CSS_E_WRITE);
  }
}

TEST(CssValueSerialize, TheFixedPointHoldsWhereTheStandardWritesTheValueBackAsItWas) {
  // CSSOM: a <number> is written "rounding the value if necessary to not produce more than 6 decimals" and "scientific
  // notation is not used". A value that rounds to something else is not written as itself: 1e-7 is written 0, which is
  // not in [1e-7, infinity], and that is the standard's answer, not a defect of the writer.
  EXPECT_EQ(run_text("<number [1e-7,∞]>", "1e-7"), "0");
  EXPECT_EQ(run_text("<number [1e-7,∞]>", run_text("<number [1e-7,∞]>", "1e-7")), "no match");
  EXPECT_EQ(run("<number>", "1e-7"), "num:1e-07");
  // magnitudes the writer keeps are fixed points, at both ends
  for (const char * n : {"0.000001", "0.999999", "1", "123456.789012", "1e21", "1e100", "1.7976931348623157e308", "4.5e15", "9007199254740993",
           "0.5", "100", "-0.000001", "-1e300"}) {
    const std::string first = run_text("<number>", n);
    EXPECT_NE(first, "no match") << n;
    EXPECT_EQ(run_text("<number>", first), first) << n;
  }
  EXPECT_EQ(run_text("<number>", "1e300"), "1" + std::string(300, '0'));
  EXPECT_EQ(run_text("<number>", "1e-300"), "0");
  // an infinity is the largest double, and that is a fixed point
  const std::string inf = run_text("<number>", "1e999");
  EXPECT_EQ(inf.size(), 309u);
  EXPECT_EQ(run_text("<number>", inf), inf);
}

TEST(CssValues, AnInexactNumberCarriesTheFlagAndAnExactOneDoesNot) {
  Defs d;
  struct Case {
    const char * grammar;
    const char * inexact;
    const char * exact;
  };
  for (const Case & c : std::vector<Case>{{"<number>", "0.1", "0.5"}, {"<length>", "0.1px", "0.5px"}, {"<percentage>", "0.1%", "0.5%"},
           {"<integer>", "9007199254740993", "9007199254740992"}, {"<angle>", "0.1deg", "0.5deg"}, {"<dimension>", "0.1foo", "0.5foo"}}) {
    Grammar g;
    compile_into(g, c.grammar, &d);
    ASSERT_TRUE(g.ok());
    Matched a, b;
    match_into(a, g, c.inexact);
    match_into(b, g, c.exact);
    ASSERT_TRUE(a.matched && b.matched) << c.grammar;
    EXPECT_NE(a.root()->flags & GTEXT_CSS_VFLAG_INEXACT, 0u) << c.grammar << " " << c.inexact;
    EXPECT_EQ(b.root()->flags & GTEXT_CSS_VFLAG_INEXACT, 0u) << c.grammar << " " << c.exact;
  }
}

TEST(CssValues, EachKindHasItsInterpolationClass) {
  struct Case {
    const char * grammar;
    const char * input;
    GTEXT_CSS_Interp interp;
  };
  for (const Case & c : std::vector<Case>{
           {"<ratio>", "16/9", GTEXT_CSS_INTERP_NUMBER}, {"<zero>", "0", GTEXT_CSS_INTERP_NUMBER}, {"<number>", "1", GTEXT_CSS_INTERP_NUMBER},
           {"<integer>", "1", GTEXT_CSS_INTERP_NUMBER}, {"<percentage>", "1%", GTEXT_CSS_INTERP_PERCENTAGE},
           {"<dimension>", "3px", GTEXT_CSS_INTERP_LENGTH}, {"<dimension>", "3deg", GTEXT_CSS_INTERP_ANGLE},
           {"<dimension>", "3s", GTEXT_CSS_INTERP_TIME}, {"<dimension>", "3foo", GTEXT_CSS_INTERP_DISCRETE},
           {"<dimension>", "3hz", GTEXT_CSS_INTERP_DISCRETE}, {"<length>", "calc(1px)", GTEXT_CSS_INTERP_LENGTH},
           {"<number>", "calc(1)", GTEXT_CSS_INTERP_NUMBER}, {"<angle>", "calc(1deg)", GTEXT_CSS_INTERP_ANGLE},
           {"<time>", "calc(1s)", GTEXT_CSS_INTERP_TIME}, {"<percentage>", "calc(1%)", GTEXT_CSS_INTERP_PERCENTAGE},
           {"<string>", "'s'", GTEXT_CSS_INTERP_DISCRETE}, {"<url>", "url(a)", GTEXT_CSS_INTERP_DISCRETE},
           {"<ident>", "a", GTEXT_CSS_INTERP_DISCRETE}, {"<custom-ident>", "a", GTEXT_CSS_INTERP_DISCRETE},
           {"<length>#", "1px, 2px", GTEXT_CSS_INTERP_LIST}, {"<length>", "0", GTEXT_CSS_INTERP_LENGTH}}) {
    Grammar g;
    compile_into(g, c.grammar);
    ASSERT_TRUE(g.ok()) << c.grammar;
    Matched m;
    match_into(m, g, c.input);
    ASSERT_TRUE(m.matched) << c.grammar << " " << c.input;
    EXPECT_EQ(m.root()->interp, c.interp) << c.grammar << " | " << c.input;
  }
}

TEST(CssValues, AnIdentIsGivenTheAtomOfTheKeywordTheGrammarNamesAndNoneOtherwise) {
  Grammar g;
  compile_into(g, "<ident> | auto");
  ASSERT_TRUE(g.ok());
  const GTEXT_CSS_Atom autoatom = gtext_css_atom_lookup(gtext_css_compiled_atoms(g.c), "auto", 4);
  ASSERT_NE(autoatom, GTEXT_CSS_ATOM_NONE);
  Matched named, other;
  match_into(named, g, "AUTO");
  match_into(other, g, "other");
  ASSERT_TRUE(named.matched && other.matched);
  EXPECT_EQ(named.root()->kind, GTEXT_CSS_VAL_KEYWORD);
  EXPECT_EQ(named.root()->atom, autoatom);
  EXPECT_EQ(css_test::str(named.root()->text), "AUTO") << "(an <ident> keeps the input's spelling)";
  EXPECT_EQ(other.root()->kind, GTEXT_CSS_VAL_KEYWORD);
  EXPECT_EQ(other.root()->atom, GTEXT_CSS_ATOM_NONE);
  Grammar h;
  compile_into(h, "auto | <ident>");
  Matched k;
  match_into(k, h, "AUTO");
  EXPECT_EQ(css_test::str(k.root()->text), "auto") << "(a keyword the grammar names is in the grammar's spelling)";
}

TEST(CssUnits, ConversionRoundsTheProductBeforeTheDivisionOnEveryTarget) {
  // The expression is value * numerator / denominator with the product stored: on a target with excess precision
  // (FLT_EVAL_METHOD != 0, x87) a product kept in a register would round once instead of twice. Where the evaluation is
  // plain double, as on x86-64, the two are the same by construction; the i686 run is what shows the difference.
  const struct {
    GTEXT_CSS_Unit unit;
    double num, den;
  } rows[] = {{GTEXT_CSS_UNIT_CM, 9600.0, 254.0}, {GTEXT_CSS_UNIT_MM, 960.0, 254.0}, {GTEXT_CSS_UNIT_Q, 960.0, 1016.0},
      {GTEXT_CSS_UNIT_PT, 4.0, 3.0}, {GTEXT_CSS_UNIT_GRAD, 360.0, 400.0}, {GTEXT_CSS_UNIT_MS, 1.0, 1000.0},
      {GTEXT_CSS_UNIT_DPI, 1.0, 96.0}, {GTEXT_CSS_UNIT_DPCM, 254.0, 9600.0}, {GTEXT_CSS_UNIT_RAD, 180.0, 3.14159265358979323846}};
  for (const auto & r : rows) {
    for (int i = 1; i <= 2000; i++) {
      const double v = i * 0.37 - 111.1;
      volatile double product = v * r.num;
      const double want = product / r.den;
      double got = 0;
      ASSERT_TRUE(gtext_css_unit_to_canonical(r.unit, v, &got));
      ASSERT_EQ(got, want) << gtext_css_unit_name(r.unit) << " " << v;
    }
  }
}

} // namespace
