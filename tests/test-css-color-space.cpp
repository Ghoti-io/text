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
 * The conversions between the color spaces of CSS Color 4 (src/css/css_color_space.h): known values, every space against every other, the analogous components
 * that a conversion carries as missing, and the epsilon of a powerless hue in each polar space. The values below were computed with the draft's sample code
 * (Color 4, "Sample code for color conversions") in Python's double arithmetic, which shares nothing with the code under test.
 */

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../src/css/css_color_space.h"

namespace {

struct Named {
  const char * name;
  GTEXT_CSS_Color_Space space;
};

/* The spaces of Color 4, 10 and 4.4: gen-color-tables.py --check requires every one of these names to be in this file. */
const std::vector<Named> kSpaces = {
  {"srgb", GTEXT_CSS_SPACE_SRGB}, {"srgb-linear", GTEXT_CSS_SPACE_SRGB_LINEAR}, {"display-p3", GTEXT_CSS_SPACE_DISPLAY_P3},
  {"display-p3-linear", GTEXT_CSS_SPACE_DISPLAY_P3_LINEAR}, {"a98-rgb", GTEXT_CSS_SPACE_A98_RGB}, {"prophoto-rgb", GTEXT_CSS_SPACE_PROPHOTO_RGB},
  {"rec2020", GTEXT_CSS_SPACE_REC2020}, {"xyz", GTEXT_CSS_SPACE_XYZ_D65}, {"xyz-d50", GTEXT_CSS_SPACE_XYZ_D50}, {"xyz-d65", GTEXT_CSS_SPACE_XYZ_D65},
  {"hsl", GTEXT_CSS_SPACE_HSL}, {"hwb", GTEXT_CSS_SPACE_HWB}, {"lab", GTEXT_CSS_SPACE_LAB}, {"lch", GTEXT_CSS_SPACE_LCH},
  {"oklab", GTEXT_CSS_SPACE_OKLAB}, {"oklch", GTEXT_CSS_SPACE_OKLCH},
};

css_cs make(GTEXT_CSS_Color_Space s, double a, double b, double c, double alpha = 1.0) {
  css_cs v{};
  v.space = s;
  v.c[0] = a;
  v.c[1] = b;
  v.c[2] = c;
  v.alpha = alpha;
  return v;
}

css_cs convert(const css_cs & in, GTEXT_CSS_Color_Space to) {
  css_cs out{};
  css_cs_convert(&in, to, &out);
  return out;
}

void expect_near(const css_cs & v, double a, double b, double c, double eps, const std::string & what) {
  EXPECT_NEAR(v.c[0], a, eps) << what;
  EXPECT_NEAR(v.c[1], b, eps) << what;
  EXPECT_NEAR(v.c[2], c, eps) << what;
}

/* Color 4, 11.2: a color is converted through XYZ, with the Bradford adaptation between D65 and D50 (Lab is against D50, Oklab against D65). */
TEST(CssColorSpace, KnownValuesOfRedAndFriends) {
  struct Case {
    const char * name;
    double rgb[3];
    double xyz[3], lab[3], oklab[3];
  };
  const Case cases[] = {
    {"red", {1, 0, 0}, {0.4123907993, 0.2126390059, 0.0193308187}, {54.29054140, 80.80492817, 69.89096477}, {0.6279553639, 0.2248630684, 0.1258462773}},
    {"rebeccapurple", {0.4, 0.2, 0.6}, {0.1241229931, 0.0749251094, 0.3093033234}, {32.39271642, 38.42299376, -47.69112551}, {0.4402717960, 0.0881767612, -0.1338643575}},
    {"white", {1, 1, 1}, {0.9504559271, 1.0, 1.0890577508}, {100.0, 0.0, 0.0}, {1.0, 0.0, 0.0}},
    {"teal", {0, 128.0 / 255.0, 128.0 / 255.0}, {0.1161470076, 0.1699601380, 0.2309117905}, {47.98582666, -30.38735841, -8.97506984}, {0.5431225691, -0.0896470588, -0.0236338253}},
    {"a color outside the sRGB gamut", {1.2, -0.1, 0.4}, {0.6459259768, 0.3249628655, 0.1544226527}, {64.83180448, 95.88607965, 35.66119359}, {0.7254601739, 0.2862472874, 0.0781574095}},
  };
  for (const Case & k : cases) {
    const css_cs in = make(GTEXT_CSS_SPACE_SRGB, k.rgb[0], k.rgb[1], k.rgb[2]);
    expect_near(convert(in, GTEXT_CSS_SPACE_XYZ_D65), k.xyz[0], k.xyz[1], k.xyz[2], 1e-9, std::string(k.name) + " in xyz-d65");
    expect_near(convert(in, GTEXT_CSS_SPACE_LAB), k.lab[0], k.lab[1], k.lab[2], 1e-6, std::string(k.name) + " in lab");
    expect_near(convert(in, GTEXT_CSS_SPACE_OKLAB), k.oklab[0], k.oklab[1], k.oklab[2], 1e-9, std::string(k.name) + " in oklab");
  }
}

/* Color 4, 10.8: "Reference electro-optical transfer function from Rec. ITU-R BT.1886 Annex 1 ... defined over the extended range, not clamped": a pure power of 2.4. */
TEST(CssColorSpace, Rec2020IsAPurePowerAndEveryRgbSpaceKeepsTheSignOfAChannel) {
  const css_cs lin = convert(make(GTEXT_CSS_SPACE_REC2020, 0.5, -0.5, 1.0), GTEXT_CSS_SPACE_XYZ_D65);
  const css_cs back = convert(lin, GTEXT_CSS_SPACE_REC2020);
  expect_near(back, 0.5, -0.5, 1.0, 1e-12, "rec2020 through xyz");
  // 0.5^2.4 against the matrix: the first column of rec2020-to-xyz scaled
  const double r = std::pow(0.5, 2.4);
  const css_cs red_only = convert(make(GTEXT_CSS_SPACE_REC2020, 0.5, 0, 0), GTEXT_CSS_SPACE_XYZ_D65);
  EXPECT_NEAR(red_only.c[0], css_cs_rec2020_to_xyz[0][0] * r, 1e-12);
  for (const Named & n : kSpaces) {
    if (css_cs_is_polar(n.space) || n.space == GTEXT_CSS_SPACE_LAB || n.space == GTEXT_CSS_SPACE_OKLAB || n.space == GTEXT_CSS_SPACE_XYZ_D50 ||
        n.space == GTEXT_CSS_SPACE_XYZ_D65) {
      continue;
    }
    const css_cs there = convert(make(n.space, -0.25, 0.5, 1.25), GTEXT_CSS_SPACE_XYZ_D65);
    expect_near(convert(there, n.space), -0.25, 0.5, 1.25, 1e-9, n.name);
  }
}

/* Color 4, 11.2 converts a color in any source space to any destination: every ordered pair of spaces, there and back, for colors in and out of gamut. */
TEST(CssColorSpace, EveryPairOfSpacesRoundTrips) {
  const double samples[][3] = {{0.2, 0.4, 0.6}, {0.9, 0.1, 0.3}, {0.5, 0.5, 0.5}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0}, {1.2, -0.1, 0.4}};
  for (const Named & a : kSpaces) {
    for (const Named & b : kSpaces) {
      for (const auto & s : samples) {
        const css_cs start = convert(make(GTEXT_CSS_SPACE_SRGB, s[0], s[1], s[2]), a.space);
        const css_cs mid = convert(start, b.space);
        css_cs back = convert(mid, a.space);
        for (int i = 0; i < 3; i++) {
          // a missing component (a powerless hue of a gray) is zero, and the hue of a gray does not matter
          if (back.none[i] || start.none[i]) {
            continue;
          }
          const double tol = (a.space == GTEXT_CSS_SPACE_HSL || a.space == GTEXT_CSS_SPACE_HWB || b.space == GTEXT_CSS_SPACE_HSL || b.space == GTEXT_CSS_SPACE_HWB) ? 1e-6 : 1e-7;
          EXPECT_NEAR(back.c[i], start.c[i], tol * (1.0 + std::fabs(start.c[i]))) << a.name << " -> " << b.name << " channel " << i << " of sample " << s[0] << " " << s[1] << " " << s[2];
        }
      }
    }
  }
}

/* Color 4, 4.4.1: "if a color is automatically produced by color space conversion, then any powerless components in the result must instead be set to missing". */
TEST(CssColorSpace, TheHueOfAGrayIsMissingWhereConversionMadeIt) {
  const GTEXT_CSS_Color_Space polar[] = {GTEXT_CSS_SPACE_HSL, GTEXT_CSS_SPACE_HWB, GTEXT_CSS_SPACE_LCH, GTEXT_CSS_SPACE_OKLCH};
  for (GTEXT_CSS_Color_Space p : polar) {
    for (double g : {0.0, 0.3, 1.0}) {
      const css_cs out = convert(make(GTEXT_CSS_SPACE_SRGB, g, g, g), p);
      EXPECT_TRUE(out.none[css_cs_hue_index(p)]) << "gray " << g << " into space " << p;
      EXPECT_FALSE(out.none[2 - css_cs_hue_index(p)] && p != GTEXT_CSS_SPACE_HWB && p != GTEXT_CSS_SPACE_HSL) << "the other channels are kept";
    }
    const css_cs colored = convert(make(GTEXT_CSS_SPACE_SRGB, 0.8, 0.2, 0.1), p);
    EXPECT_FALSE(colored.none[css_cs_hue_index(p)]) << "a color has a hue";
  }
  // white is hsl(none 0% 100%): the example of Color 4, 4.4
  const css_cs w = convert(make(GTEXT_CSS_SPACE_SRGB, 1, 1, 1), GTEXT_CSS_SPACE_HSL);
  EXPECT_TRUE(w.none[0]);
  EXPECT_EQ(w.c[1], 0.0);
  EXPECT_EQ(w.c[2], 100.0);
}

/* Color 4, 4.4.1, "Powerless hue epsilon" of each space: hsl S <= 0.001; lch C <= 0.0015; oklch C <= 0.000004; hwb W + B >= 99.999. A hue that conversion produced
   with a chroma at or below it is missing; above it, it is a number. Each threshold is probed on both sides with a color made to sit there. */
TEST(CssColorSpace, ThePowerlessEpsilonOfEachPolarSpace) {
  // hsl: a gray off by d in blue has a saturation of about 100 d percent; hwb: W + B is 100 (1 - d)
  for (double d : {0.9e-5, 1.2e-5}) {
    const css_cs gray = make(GTEXT_CSS_SPACE_SRGB, 0.5, 0.5, 0.5 + d);
    EXPECT_EQ(convert(gray, GTEXT_CSS_SPACE_HSL).none[0], d < 1.0e-5) << "hsl, d = " << d;
    EXPECT_EQ(convert(gray, GTEXT_CSS_SPACE_HWB).none[0], d < 1.0e-5) << "hwb, d = " << d;
  }
  // lch and oklch: a Lab color with b = C and a = 0 has chroma C
  for (double c : {0.0014, 0.0016}) {
    EXPECT_EQ(convert(make(GTEXT_CSS_SPACE_LAB, 50, 0, c), GTEXT_CSS_SPACE_LCH).none[2], c <= 0.0015) << "lch, C = " << c;
  }
  for (double c : {0.0000039, 0.0000041}) {
    EXPECT_EQ(convert(make(GTEXT_CSS_SPACE_OKLAB, 0.5, 0, c), GTEXT_CSS_SPACE_OKLCH).none[2], c <= 0.000004) << "oklch, C = " << c;
  }
  // a powerless hue sets a small positive chroma to zero ("this value is only set to zero when it is larger than zero but smaller or equal to the epsilon")
  const css_cs tiny = convert(make(GTEXT_CSS_SPACE_LAB, 50, 0, 0.001), GTEXT_CSS_SPACE_LCH);
  EXPECT_TRUE(tiny.none[2]);
  EXPECT_EQ(tiny.c[1], 0.0);
  // and a source with a powerless hue hands the destination's hue a missing one: hsl(200 0.001% 50%) into hwb
  const css_cs src = make(GTEXT_CSS_SPACE_HSL, 200, 0.001, 50);
  EXPECT_TRUE(convert(src, GTEXT_CSS_SPACE_HWB).none[0]);
  EXPECT_FALSE(convert(make(GTEXT_CSS_SPACE_HSL, 200, 0.0011, 50), GTEXT_CSS_SPACE_HWB).none[0]);
}

/* Color 4, 13.3: an analogous component that is missing is carried to the destination; "if every component of an analogous set ... is a missing component, they are all
   carried forward". The draft's examples: lab(50% none none) to LCH is lch(50% none none) and not lch(50% 0 0); rgb(none none none / 50%) to Oklab is oklab(none none none / 50%). */
TEST(CssColorSpace, MissingComponentsAreCarriedToAnalogousOnes) {
  css_cs lab = make(GTEXT_CSS_SPACE_LAB, 50, 0, 0);
  lab.none[1] = lab.none[2] = true;
  css_cs lch = convert(lab, GTEXT_CSS_SPACE_LCH);
  EXPECT_FALSE(lch.none[0]);
  EXPECT_TRUE(lch.none[1]);
  EXPECT_TRUE(lch.none[2]);
  EXPECT_NEAR(lch.c[0], 50.0, 1e-9);

  css_cs rgb = make(GTEXT_CSS_SPACE_SRGB, 0, 0, 0, 0.5);
  rgb.none[0] = rgb.none[1] = rgb.none[2] = true;
  const css_cs ok = convert(rgb, GTEXT_CSS_SPACE_OKLAB);
  EXPECT_TRUE(ok.none[0] && ok.none[1] && ok.none[2]);
  EXPECT_FALSE(ok.none[3]);
  EXPECT_EQ(ok.alpha, 0.5);

  // only part of an analogous set: nothing is carried (rgb(0 none 0) to oklab)
  css_cs part = make(GTEXT_CSS_SPACE_SRGB, 0.2, 0, 0.4);
  part.none[1] = true;
  const css_cs po = convert(part, GTEXT_CSS_SPACE_OKLAB);
  EXPECT_FALSE(po.none[0] || po.none[1] || po.none[2]);

  // the alpha is its own analogous component
  css_cs al = make(GTEXT_CSS_SPACE_SRGB, 0.2, 0.3, 0.4);
  al.none[3] = true;
  EXPECT_TRUE(convert(al, GTEXT_CSS_SPACE_LAB).none[3]);

  // the hue of lch is the hue of oklch: a missing hue is carried, and the lightnesses are analogous
  css_cs h = make(GTEXT_CSS_SPACE_LCH, 50, 30, 0);
  h.none[2] = true;
  const css_cs oh = convert(h, GTEXT_CSS_SPACE_OKLCH);
  EXPECT_TRUE(oh.none[2]);
  EXPECT_FALSE(oh.none[0]);
}

/* sRGB, HSL and HWB are one space written three ways: no detour through XYZ turns white into a color a hair off the gray axis. */
TEST(CssColorSpace, TheSrgbFamilyConvertsExactly) {
  const css_cs hsl = convert(make(GTEXT_CSS_SPACE_HSL, 120, 100, 50), GTEXT_CSS_SPACE_SRGB);
  EXPECT_EQ(hsl.c[0], 0.0);
  EXPECT_EQ(hsl.c[1], 1.0);
  EXPECT_EQ(hsl.c[2], 0.0);
  const css_cs hwb = convert(make(GTEXT_CSS_SPACE_HWB, 0, 20, 30), GTEXT_CSS_SPACE_SRGB);
  EXPECT_NEAR(hwb.c[0], 0.7, 1e-15);
  EXPECT_NEAR(hwb.c[1], 0.2, 1e-15);
  EXPECT_NEAR(hwb.c[2], 0.2, 1e-15);
  const css_cs white = convert(make(GTEXT_CSS_SPACE_SRGB, 1, 1, 1), GTEXT_CSS_SPACE_HWB);
  EXPECT_EQ(white.c[1], 100.0);
  EXPECT_EQ(white.c[2], 0.0);
  // Color 4, 8.2: the hue of hwb is that of rgbToHue, without the turn of a negative saturation
  const css_cs far = convert(make(GTEXT_CSS_SPACE_SRGB, 1.5, 1.2, 1.9), GTEXT_CSS_SPACE_HWB);
  const css_cs far_hsl = convert(make(GTEXT_CSS_SPACE_SRGB, 1.5, 1.2, 1.9), GTEXT_CSS_SPACE_HSL);
  EXPECT_NEAR(far.c[0], 265.7142857142857, 1e-9);
  EXPECT_NEAR(far_hsl.c[0], 85.71428571428572, 1e-9) << "a color this far out of gamut has a negative saturation in hsl, whose hue is turned half a turn and the saturation made positive";
  EXPECT_GT(far_hsl.c[1], 0.0);
}

/* Color 4, 13.4: the mix of the matrices with the draft's own: the five RGB spaces equal the sample code's matrices (checked when the table was written). */
TEST(CssColorSpace, TheSrgbMatrixIsTheDraftsRationalOne) {
  // lin_sRGB_to_XYZ: 506752 / 1228815, 87881 / 245763, 12673 / 70218 ...
  EXPECT_NEAR(css_cs_srgb_to_xyz[0][0], 506752.0 / 1228815.0, 1e-16);
  EXPECT_NEAR(css_cs_srgb_to_xyz[0][1], 87881.0 / 245763.0, 1e-16);
  EXPECT_NEAR(css_cs_srgb_to_xyz[0][2], 12673.0 / 70218.0, 1e-16);
  EXPECT_NEAR(css_cs_srgb_to_xyz[2][2], 1001167.0 / 1053270.0, 1e-16);
  // D65 to D50 (Bradford), the first row of the sample code
  EXPECT_NEAR(css_cs_d65_to_d50[0][0], 1.0479297925449969, 1e-13);
  EXPECT_NEAR(css_cs_d65_to_d50[2][2], 0.7518742814281371, 1e-13);
  // the ProPhoto matrix, from the draft's six-digit chromaticities
  EXPECT_NEAR(css_cs_prophoto_rgb_to_xyz[0][0], 0.79776664490064230, 1e-12);
  EXPECT_NEAR(css_cs_prophoto_rgb_to_xyz[1][2], 0.00008993693872564, 1e-15);
}

} // namespace
