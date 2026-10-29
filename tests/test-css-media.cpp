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
 * Media Queries 4 and 5: the grammar, the error rules, the three-valued evaluation, the units, the environment and the conditions object. Each test is named
 * for the rule it checks and the sentence of the served draft that decides it is quoted beside it (read 2026-10-28).
 */

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_media_rows.h"

namespace {

GTEXT_Markup_Status collect(void * user, const char * bytes, size_t len) {
  static_cast<std::string *>(user)->append(bytes, len);
  return GTEXT_MARKUP_OK;
}

struct Parsed {
  GTEXT_CSS_Media_List * list = nullptr;
  GTEXT_CSS_Media_Result result;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  explicit Parsed(const std::string & text, const GTEXT_CSS_Media_Options * o = nullptr) {
    std::memset(&result, 0, sizeof result);
    status = gtext_css_media_list_parse(text.data(), text.size(), o, &list, &result);
  }
  ~Parsed() {
    gtext_css_media_list_free(list);
    gtext_css_media_result_free(&result);
  }
  Parsed(const Parsed &) = delete;
  Parsed & operator=(const Parsed &) = delete;
  std::string text() const {
    std::string out;
    GTEXT_Markup_Sink sink{collect, &out};
    EXPECT_EQ(gtext_css_media_list_serialize(list, &sink), GTEXT_CSS_OK);
    return out;
  }
};

std::string ser(const std::string & q) {
  Parsed p(q);
  EXPECT_EQ(p.status, GTEXT_CSS_OK) << q;
  return p.list ? p.text() : std::string("<failed>");
}

GTEXT_CSS_Media_Environment base_env() {
  return gtext_css_media_environment_default(nullptr);
}

struct Cond {
  GTEXT_CSS_Conditions * c = nullptr;
  explicit Cond(const GTEXT_CSS_Media_Environment & env, GTEXT_CSS_Condition_Defs * defs = nullptr, bool custom = true) {
    GTEXT_CSS_Conditions_Options o = gtext_css_conditions_options_default();
    o.media = &env;
    o.defs = defs;
    o.custom_media = custom;
    EXPECT_EQ(gtext_css_conditions_new(nullptr, &o, &c), GTEXT_CSS_OK);
  }
  ~Cond() {
    gtext_css_conditions_free(c);
  }
  Cond(const Cond &) = delete;
  Cond & operator=(const Cond &) = delete;
};

/// matchMedia(q).matches against `env`.
bool matches(const std::string & q, const GTEXT_CSS_Media_Environment & env) {
  Cond c(env);
  bool m = false;
  GTEXT_CSS_Status st = gtext_css_conditions_matches_media(c.c, q.data(), q.size(), nullptr, &m);
  EXPECT_EQ(st, GTEXT_CSS_OK) << q;
  return m;
}

bool matches(const std::string & q) {
  return matches(q, base_env());
}

/// The three-valued answer of the first query.
GTEXT_CSS_Tristate tri(const std::string & q, const GTEXT_CSS_Media_Environment & env) {
  Parsed p(q);
  Cond c(env);
  GTEXT_CSS_Tristate t = GTEXT_CSS_TRI_UNKNOWN;
  EXPECT_EQ(gtext_css_conditions_evaluate_query(c.c, p.list, 0, nullptr, &t), GTEXT_CSS_OK) << q;
  return t;
}

GTEXT_CSS_Tristate tri(const std::string & q) {
  return tri(q, base_env());
}

/// The environment the probes of tests/css_media_rows.h are written for: every feature supported, a value of each that the probes name.
GTEXT_CSS_Media_Environment probe_env() {
  GTEXT_CSS_Media_Environment e = base_env();
  for (size_t i = 0; i < gtext_css_media_feature_count(); i++) {
    GTEXT_CSS_Media_Feature_Row r;
    gtext_css_media_feature_row(i, &r);
    gtext_css_media_environment_set_supported(&e, r.name, true);
  }
  e.viewport = {1280, 800};
  e.device = {1920, 1080};
  e.resolution = 2;
  e.color_bits = 8;
  e.color_index = 256;
  e.monochrome_bits = 0;
  e.color_gamut = GTEXT_CSS_MQ_GAMUT_P3;
  e.video_color_gamut = GTEXT_CSS_MQ_GAMUT_SRGB;
  e.dynamic_range = GTEXT_CSS_MQ_RANGE_STANDARD;
  e.video_dynamic_range = GTEXT_CSS_MQ_RANGE_STANDARD;
  e.scan = GTEXT_CSS_MQ_SCAN_INTERLACE;
  e.update = GTEXT_CSS_MQ_UPDATE_SLOW;
  e.overflow_block = GTEXT_CSS_MQ_OVERFLOW_BLOCK_PAGED;
  e.overflow_inline = GTEXT_CSS_MQ_OVERFLOW_INLINE_SCROLL;
  e.pointer = GTEXT_CSS_MQ_POINTER_COARSE;
  e.hover = GTEXT_CSS_MQ_HOVER_NONE;
  e.any_pointer = GTEXT_CSS_MQ_ANY_POINTER_COARSE | GTEXT_CSS_MQ_ANY_POINTER_FINE;
  e.any_hover = GTEXT_CSS_MQ_ANY_HOVER_HOVER;
  e.display_mode = GTEXT_CSS_MQ_DISPLAY_MODE_STANDALONE;
  e.display_state = GTEXT_CSS_MQ_DISPLAY_STATE_MAXIMIZED;
  e.shape = GTEXT_CSS_MQ_SHAPE_ROUND;
  e.environment_blending = GTEXT_CSS_MQ_BLENDING_ADDITIVE;
  e.nav_controls = GTEXT_CSS_MQ_NAV_BACK;
  e.inverted_colors = GTEXT_CSS_MQ_INVERTED_INVERTED;
  e.scripting = GTEXT_CSS_MQ_SCRIPTING_INITIAL_ONLY;
  e.forced_colors = GTEXT_CSS_MQ_FORCED_COLORS_ACTIVE;
  e.prefers_color_scheme = GTEXT_CSS_MQ_SCHEME_DARK;
  e.ua_color_scheme = GTEXT_CSS_MQ_SCHEME_DARK;
  e.prefers_contrast = GTEXT_CSS_MQ_CONTRAST_MORE;
  e.prefers_reduced_motion = GTEXT_CSS_MQ_REDUCE_REDUCE;
  e.prefers_reduced_transparency = GTEXT_CSS_MQ_REDUCE_REDUCE;
  e.prefers_reduced_data = GTEXT_CSS_MQ_REDUCE_REDUCE;
  e.horizontal_viewport_segments = 2;
  e.vertical_viewport_segments = 3;
  e.grid = true;
  e.resizable = true;
  e.transform_3d = true;
  return e;
}

GTEXT_CSS_Media_Environment sized(double w, double h) {
  GTEXT_CSS_Media_Environment e = base_env();
  e.viewport = {w, h};
  return e;
}

}  // namespace

/* ---- one probe for every feature of the tables ------------------------------------------------------------------------------------------------------ */

TEST(CssMedia, EveryFeatureOfTheTableHasAProbeThatIsTrueAndOneThatIsFalse) {
  GTEXT_CSS_Media_Environment e = probe_env();
  size_t seen = 0;
  for (const MediaProbe & p : media_probes) {
    EXPECT_TRUE(matches(p.yes, e)) << p.name << " " << p.yes;
    EXPECT_FALSE(matches(p.no, e)) << p.name << " " << p.no;
    EXPECT_EQ(ser(p.yes), p.serialized) << p.name;
    EXPECT_GE(gtext_css_media_feature_find(p.name, std::strlen(p.name)), 0) << p.name;
    seen++;
  }
  EXPECT_EQ(seen, gtext_css_media_feature_count());
}

TEST(CssMedia, EveryKeywordOfEveryKeywordFeatureIsEvaluatedFromItsField) {
  /* The environment's enumerations are the pin's keywords by position: each keyword is true for exactly the value of the field that names it. */
  struct Case {
    const char * feature;
    std::vector<const char *> words;
    void (*set)(GTEXT_CSS_Media_Environment *, int);
    bool ordered; /* each value implies the lower ones */
    int first;    /* the field value of the first keyword */
  };
  static const Case cases[] = {
    {"scan", {"interlace", "progressive"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->scan = (uint8_t)v; }, false, 1},
    {"update", {"none", "slow", "fast"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->update = (uint8_t)v; }, false, 0},
    {"overflow-block", {"none", "scroll", "paged"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->overflow_block = (uint8_t)v; }, false, 0},
    {"overflow-inline", {"none", "scroll"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->overflow_inline = (uint8_t)v; }, false, 0},
    {"color-gamut", {"srgb", "p3", "rec2020"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->color_gamut = (uint8_t)v; }, true, 1},
    {"video-color-gamut", {"srgb", "p3", "rec2020"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->video_color_gamut = (uint8_t)v; }, true, 1},
    {"dynamic-range", {"standard", "high"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->dynamic_range = (uint8_t)v; }, true, 0},
    {"video-dynamic-range", {"standard", "high"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->video_dynamic_range = (uint8_t)v; }, true, 0},
    {"pointer", {"none", "coarse", "fine"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->pointer = (uint8_t)v; }, false, 0},
    {"hover", {"none", "hover"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->hover = (uint8_t)v; }, false, 0},
    {"display-mode", {"application", "fullscreen", "standalone", "minimal-ui", "browser", "picture-in-picture"},
     [](GTEXT_CSS_Media_Environment * e, int v) { e->display_mode = (uint8_t)v; }, false, 0},
    {"display-state", {"normal", "minimized", "maximized", "fullscreen"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->display_state = (uint8_t)v; }, false, 0},
    {"shape", {"rect", "round"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->shape = (uint8_t)v; }, false, 0},
    {"environment-blending", {"opaque", "additive", "subtractive"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->environment_blending = (uint8_t)v; }, false, 0},
    {"nav-controls", {"none", "back"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->nav_controls = (uint8_t)v; }, false, 0},
    {"inverted-colors", {"none", "inverted"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->inverted_colors = (uint8_t)v; }, false, 0},
    {"scripting", {"none", "initial-only", "enabled"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->scripting = (uint8_t)v; }, false, 0},
    {"forced-colors", {"none", "active"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->forced_colors = (uint8_t)v; }, false, 0},
    {"prefers-color-scheme", {"light", "dark"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->prefers_color_scheme = (uint8_t)v; }, false, 0},
    {"ua-color-scheme", {"light", "dark"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->ua_color_scheme = (uint8_t)v; }, false, 0},
    {"prefers-contrast", {"no-preference", "less", "more", "custom"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->prefers_contrast = (uint8_t)v; }, false, 0},
    {"prefers-reduced-motion", {"no-preference", "reduce"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->prefers_reduced_motion = (uint8_t)v; }, false, 0},
    {"prefers-reduced-transparency", {"no-preference", "reduce"},
     [](GTEXT_CSS_Media_Environment * e, int v) { e->prefers_reduced_transparency = (uint8_t)v; }, false, 0},
    {"prefers-reduced-data", {"no-preference", "reduce"}, [](GTEXT_CSS_Media_Environment * e, int v) { e->prefers_reduced_data = (uint8_t)v; }, false, 0},
  };
  for (const Case & c : cases) {
    for (size_t chosen = 0; chosen < c.words.size(); chosen++) {
      GTEXT_CSS_Media_Environment e = probe_env();
      c.set(&e, c.first + (int)chosen);
      for (size_t w = 0; w < c.words.size(); w++) {
        std::string q = std::string("(") + c.feature + ": " + c.words[w] + ")";
        bool want = c.ordered ? (w <= chosen) : (w == chosen);
        EXPECT_EQ(matches(q, e), want) << q << " with the value " << chosen;
      }
    }
  }
}

/* ---- the grammar and the serializer (matchMedia parsing of the web-platform-tests, css/mediaqueries/match-media-parsing.html) ------------------------- */

TEST(CssMedia, MatchMediaParsingVectors) {
  struct V {
    const char * in;
    const char * out;
  };
  static const V v[] = {
    {"", ""}, {" ", ""}, {"all", "all"}, {" all", "all"}, {"   all   ", "all"}, {"all,all", "all, all"}, {" all , all ", "all, all"},
    {"(color)", "(color)"}, {"(color", "(color)"}, {" (color)", "(color)"}, {" ( color  )  ", "(color)"}, {" ( color   ", "(color)"},
    {"color)", "not all"}, {"  color)", "not all"}, {"  color ), ( color", "not all, (color)"}, {" foo ", "foo"}, {",", "not all, not all"},
    {" , ", "not all, not all"}, {",,", "not all, not all, not all"}, {"  ,  ,  ", "not all, not all, not all"}, {" foo,", "foo, not all"},
    {"(min-resolution: 1x)", "(min-resolution: 1dppx)"}, /* the web-platform-tests keep the unit as written; see the test of units */
  };
  for (const V & x : v) {
    if (std::string(x.in) == "(min-resolution: 1x)") {
      continue; /* decided below */
    }
    EXPECT_EQ(ser(x.in), x.out) << "'" << x.in << "'";
  }
}

TEST(CssMedia, ResolutionUnitsAreKeptAsWritten) {
  /* Web-platform-tests match-media-parsing: `(resolution: 2x)`, `2dppx`, `600dpi` and `77dpcm` serialize as written; a calculation is computed. */
  EXPECT_EQ(ser("(min-resolution: 1x)"), "(min-resolution: 1x)");
  EXPECT_EQ(ser("(resolution: 2x)"), "(resolution: 2x)");
  EXPECT_EQ(ser("(resolution: 2dppx)"), "(resolution: 2dppx)");
  EXPECT_EQ(ser("(resolution: 600dpi)"), "(resolution: 600dpi)");
  EXPECT_EQ(ser("(resolution: 77dpcm)"), "(resolution: 77dpcm)");
  EXPECT_EQ(ser("(min-resolution: calc(1x))"), "(min-resolution: calc(1dppx))");
  EXPECT_EQ(ser("(resolution: calc(1x + 2x))"), "(resolution: calc(3dppx))");
  EXPECT_EQ(ser("(resolution: calc(5x - 2x))"), "(resolution: calc(3dppx))");
  EXPECT_EQ(ser("(resolution: calc(1x * 3))"), "(resolution: calc(3dppx))");
  EXPECT_EQ(ser("(resolution: calc(6x / 2))"), "(resolution: calc(3dppx))");
}

TEST(CssMedia, RatioIsAlwaysSerializedWithBothNumbers) {
  /* Values 4, 5.7: "<ratio> is always serialized with both components"; the web-platform-tests: `(aspect-ratio: 1/3)` is `(aspect-ratio: 1 / 3)`. */
  EXPECT_EQ(ser("(aspect-ratio: 1/3)"), "(aspect-ratio: 1 / 3)");
  EXPECT_EQ(ser("(aspect-ratio: 16 / 9)"), "(aspect-ratio: 16 / 9)");
  EXPECT_EQ(ser("(aspect-ratio: 2)"), "(aspect-ratio: 2 / 1)");
  EXPECT_EQ(ser("(min-aspect-ratio: 0.5)"), "(min-aspect-ratio: 0.5 / 1)");
}

TEST(CssMedia, TheGrammarOfTheDraftSerializes) {
  EXPECT_EQ(ser("not screen and (min-WIDTH:5px) AND (max-width:40px)"), "not screen and (min-width: 5px) and (max-width: 40px)");
  EXPECT_EQ(ser("all and (color) and (color)"), "(color) and (color)") << "CSSOM: all and (color) and (color) is (color) and (color)";
  EXPECT_EQ(ser("only screen and (color)"), "only screen and (color)");
  EXPECT_EQ(ser("SCREEN"), "screen");
  EXPECT_EQ(ser("not all and (color)"), "not all and (color)");
  EXPECT_EQ(ser("(color) or (hover)"), "(color) or (hover)");
  EXPECT_EQ(ser("not (color)"), "not (color)");
  EXPECT_EQ(ser("(not (color)) or (hover)"), "(not (color)) or (hover)");
  EXPECT_EQ(ser("((color))"), "((color))");
  EXPECT_EQ(ser("(color) and ((pointer) or (hover))"), "(color) and ((pointer) or (hover))");
  EXPECT_EQ(ser("screen and not (color)"), "screen and not (color)");
}

TEST(CssMedia, RangeSyntaxKeepsItsForm) {
  /* Media Queries 4, 2.4.3: the range forms `(a < width)`, `(width <= b)`, `(a <= width <= b)`. */
  EXPECT_EQ(ser("(width>=600px)"), "(width >= 600px)");
  EXPECT_EQ(ser("(600px<=width)"), "(600px <= width)");
  EXPECT_EQ(ser("(400px <= width <= 700px)"), "(400px <= width <= 700px)");
  EXPECT_EQ(ser("(700px > width > 400px)"), "(700px > width > 400px)");
  EXPECT_EQ(ser("(width = 600px)"), "(width = 600px)");
  EXPECT_EQ(ser("(MIN-WIDTH: 10PX)"), "(min-width: 10px)");
  EXPECT_EQ(ser("(-webkit-min-device-pixel-ratio: 2)"), "(-webkit-min-device-pixel-ratio: 2)");
}

TEST(CssMedia, TheVendorRatioHasOnlyItsOwnPrefixedSpellings) {
  /* Compatibility Standard, 4.3: -webkit-min-device-pixel-ratio and -webkit-max-device-pixel-ratio exist; `min--webkit-device-pixel-ratio` does not (both browsers
   * answer false), so it is an unknown feature name and is written back as it was written. */
  EXPECT_EQ(ser("(min--webkit-device-pixel-ratio: 1)"), "(min--webkit-device-pixel-ratio: 1)");
  EXPECT_EQ(ser("(max--webkit-device-pixel-ratio: 1)"), "(max--webkit-device-pixel-ratio: 1)");
  EXPECT_FALSE(matches("(min--webkit-device-pixel-ratio: 1)"));
  EXPECT_TRUE(matches("(not (min--webkit-device-pixel-ratio: 1)) or (-webkit-min-device-pixel-ratio: 1)")) << "unknown or true";
  EXPECT_FALSE(matches("(not (min--webkit-device-pixel-ratio: 1)) or (-webkit-min-device-pixel-ratio: 2)")) << "unknown or false is unknown";
  EXPECT_TRUE(matches("(-webkit-min-device-pixel-ratio: 1)"));
}

TEST(CssMedia, ANegativeResolutionIsNotAValue) {
  /* `<resolution [0,inf]>`: `(min-resolution: -1x)` is an unknown leaf, false here and in both browsers, where a negative length is a value. */
  EXPECT_FALSE(matches("(min-resolution: -1x)"));
  EXPECT_FALSE(matches("not (min-resolution: -1x)"));
  EXPECT_EQ(ser("(min-resolution: -1x)"), "(min-resolution: -1x)");
  EXPECT_TRUE(matches("(min-resolution: -1x) or (color)"));
  EXPECT_FALSE(matches("(resolution >= -1dppx)"));
  EXPECT_TRUE(matches("(min-width: -1px)"));
}

TEST(CssMedia, RatiosAreComparedExactly) {
  /* A viewport of (2^52+1) by 2^52 against the ratio (2^52+2)/(2^52+1): the cross products are 2^104 + 2^53 + 1 and 2^104 + 2^53, which differ by one and
   * which a product rounded to a double cannot tell apart. */
  GTEXT_CSS_Media_Environment e = base_env();
  e.viewport.width = 4503599627370497.0;
  e.viewport.height = 4503599627370496.0;
  EXPECT_TRUE(matches("(aspect-ratio > 4503599627370498 / 4503599627370497)", e));
  EXPECT_FALSE(matches("(aspect-ratio <= 4503599627370498 / 4503599627370497)", e));
  EXPECT_FALSE(matches("(aspect-ratio = 4503599627370498 / 4503599627370497)", e));
  EXPECT_TRUE(matches("(aspect-ratio = 4503599627370497 / 4503599627370496)", e));
  EXPECT_TRUE(matches("(aspect-ratio = 9007199254740994 / 9007199254740992)", e)) << "equal as fractions";
  EXPECT_TRUE(matches("(aspect-ratio < 4503599627370497 / 4503599627370495)", e));
  EXPECT_TRUE(matches("(1 / 3 < aspect-ratio < 3 / 1)", e));
}

TEST(CssMedia, AUnitIsWrittenInLowerCase) {
  /* Values 4, 5.1: "Units are ASCII case-insensitive and serialize as lowercase, for example 1Q serializes as 1q" (both browsers: `matchMedia("(min-width: 256Q)").media` is
   * `(min-width: 256q)`). */
  EXPECT_EQ(ser("(min-width: 256Q)"), "(min-width: 256q)");
  EXPECT_EQ(ser("(100Q <= width <= 2E2Q)"), "(100q <= width <= 200q)");
  EXPECT_EQ(ser("(min-resolution: 2DPPX)"), "(min-resolution: 2dppx)");
  EXPECT_TRUE(matches("(min-width: 1Q)"));
}

TEST(CssMedia, ParseSerializeParseIsAFixedPoint) {
  static const char * const qs[] = {
    "not screen and (min-width: 5px) and (max-width: 40px)", "(400px <= width <= 700px), print", "((color))", "(foo: bar), (--x), not (--y)",
    "(aspect-ratio: 16/9) or (width > calc(100px + 5em))", "only screen", "foo(bar) and (color)", "(  )", "unknown and (hover)", "(example, all,), speech",
    "(min-resolution: calc(1x))", "\\{screen and (--\\(FOO: bar)", "(calc(1px) < width)",
  };
  for (const char * q : qs) {
    std::string a = ser(q);
    std::string b = ser(a);
    EXPECT_EQ(a, b) << q;
  }
}

TEST(CssMedia, EscapedIdentifiersAreSerializedEscaped) {
  /* Web-platform-tests mq-escaped-serialization.html: "Serialization of media query escapes identifiers when necessary". */
  EXPECT_EQ(ser("\\{screen and (--\\(FOO: bar)"), "\\{screen and (--\\(FOO: bar)");
}

/* ---- error handling (Media Queries 4, 3.2) ------------------------------------------------------------------------------------------------------- */

TEST(CssMedia, AnInvalidQueryIsReplacedByNotAllAndTheOthersStand) {
  /* "A media query that does not match the grammar ... must be replaced by not all during parsing. ... a grammar mismatch does not wipe out an entire media query
   * list, just the problematic media query." */
  Parsed p("&test, speech");
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  EXPECT_EQ(p.text(), "not all, speech");
  /* The draft's other example, `(example, all,), speech`, is `not all, speech` there. Since the draft allowed any value in a <general-enclosed> the block is that:
   * unknown, which is false at the top, the same truth value, and kept as written. */
  EXPECT_EQ(ser("(example, all,), speech"), "(example, all,), speech");
  EXPECT_EQ(tri("(example, all,)"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_TRUE(gtext_css_media_list_query_replaced(p.list, 0));
  EXPECT_FALSE(gtext_css_media_list_query_replaced(p.list, 1));
  EXPECT_EQ(p.result.replaced, 1u);
  EXPECT_GE(gtext_css_media_result_diagnostic_count(&p.result), 1u);
  GTEXT_CSS_Media_Diagnostic d;
  ASSERT_TRUE(gtext_css_media_result_diagnostic(&p.result, 0, &d));
  EXPECT_NE(std::string(d.reference).find("Media Queries 4"), std::string::npos);
}

TEST(CssMedia, AnAuthoredNotAllIsNotReplaced) {
  Parsed p("not all");
  EXPECT_FALSE(gtext_css_media_list_query_replaced(p.list, 0));
  EXPECT_EQ(p.result.replaced, 0u);
}

TEST(CssMedia, InvalidByTheGrammar) {
  /* Each of these is `not all`: an `only` with no type, a missing `and`, `or` beside `and`, `not` where the grammar forbids it, a reserved word as a type. */
  static const char * const bad[] = {
    "only (color)",                    /* "the only keyword can only be used before a media type" */
    "screen (color)",                  /* a missing `and` */
    "screen and",                      /* nothing after `and` */
    "screen and (a) or (b)",           /* <media-condition-without-or> */
    "(color) and (pointer) or (hover)", /* "It is invalid to mix and and or and not at the same level" */
    "not (a) and (b)",                 /* <media-not> is not followed by `and` */
    "not not (color)",
    "or and (color)",                  /* "the media query or and (color) is turned into not all" */
    "layer",                           /* "<media-type> does not include the keywords only, not, and, or, and layer" */
    "not layer",
    "and",
    "42",
    "\"string\"",
    "screen and screen",
  };
  for (const char * q : bad) {
    EXPECT_EQ(ser(q), "not all") << q;
    EXPECT_FALSE(matches(q)) << q;
  }
}


TEST(CssMedia, AKnownFeatureWrittenWrongIsOneUnknownLeaf) {
  /* Media Queries 4, 3.2: a media feature that is not valid is unknown; with the three-valued logic of 3.4 an unknown leaf makes `or` true beside a true
   * leaf and `not` of it unknown, and the text is written back as it was written, as `matchMedia` of both browsers does ("The media query (color:20example)
   * ... is therefore turned into not all" of the draft is the whole query being that one leaf, which is false either way). */
  static const char * const unknown[] = {
    "(width: 5)",                      /* a number with no unit where a length is wanted */
    "(width: 5px 6px)",
    "(width:)",
    "(color: 20example)",              /* "The media query (color:20example) ... is therefore turned into not all" */
    "(color: 1.5)",
    "(grid: 2)",                       /* "Any other integer value is invalid" */
    "(aspect-ratio: -1/2)",            /* <number [0,inf]> */
    "(aspect-ratio: 1/-2)",
    "(min-orientation: portrait)",     /* "The orientation feature does not accept prefixes" */
    "(max-grid: 1)",
    "(min-width)",                     /* "Attempting to evaluate a min/max prefixed media feature in a boolean context is invalid" */
    "(400px < width > 700px)",         /* both comparisons in the same direction */
    "(400px = width = 700px)",         /* `=` once */
    "(400px <= width = 700px)",
    "(orientation = portrait)",        /* the range forms are for range features */
    "(width < height)",
    "(min-width < 5px)",
    "(resolution: 5)",                 /* a resolution has a unit */
    "(resolution: 0)",
    "(update: 1)",
    "(pointer: coarse fine)",
  };
  for (const char * q : unknown) {
    EXPECT_EQ(ser(q), q) << q;
    EXPECT_FALSE(matches(q)) << q;
    EXPECT_TRUE(matches(std::string("(color) or ") + q)) << q;
    EXPECT_FALSE(matches(std::string("(color) and ") + q)) << q;
    EXPECT_FALSE(matches(std::string("not ") + q)) << q;
    EXPECT_TRUE(matches(std::string("not foo and ") + q)) << q;
  }
  EXPECT_EQ(ser("(color: foo) or (color)"), "(color: foo) or (color)");
  EXPECT_TRUE(matches("(min-width: foo) or (min-width: 1px)"));
  EXPECT_TRUE(matches("(color: foo), (color)"));
  EXPECT_FALSE(matches("screen and (color: foo)"));
}

TEST(CssMedia, NoPreferenceIsFalseInABooleanContext) {
  /* Media Queries 5: "no-preference evaluates to false in the boolean context" (prefers-reduced-motion, -transparency, -data and prefers-contrast; web-platform-tests
   * prefers-reduced-motion.html: `(prefers-reduced-motion)` matches exactly when `(prefers-reduced-motion: no-preference)` does not). */
  GTEXT_CSS_Media_Environment e = base_env();
  e.prefers_reduced_motion = GTEXT_CSS_MQ_REDUCE_NO_PREFERENCE;
  e.prefers_reduced_transparency = GTEXT_CSS_MQ_REDUCE_NO_PREFERENCE;
  e.prefers_reduced_data = GTEXT_CSS_MQ_REDUCE_NO_PREFERENCE;
  e.prefers_contrast = GTEXT_CSS_MQ_CONTRAST_NO_PREFERENCE;
  for (const char * f : {"prefers-reduced-motion", "prefers-reduced-transparency", "prefers-reduced-data", "prefers-contrast"}) {
    const std::string q = std::string("(") + f + ")";
    EXPECT_FALSE(matches(q.c_str(), e)) << q;
    EXPECT_TRUE(matches(("not " + q).c_str(), e)) << q;
    EXPECT_TRUE(matches((std::string("(") + f + ": no-preference)").c_str(), e)) << q;
  }
  e.prefers_reduced_motion = GTEXT_CSS_MQ_REDUCE_REDUCE;
  e.prefers_reduced_transparency = GTEXT_CSS_MQ_REDUCE_REDUCE;
  e.prefers_reduced_data = GTEXT_CSS_MQ_REDUCE_REDUCE;
  e.prefers_contrast = GTEXT_CSS_MQ_CONTRAST_MORE;
  for (const char * f : {"prefers-reduced-motion", "prefers-reduced-transparency", "prefers-reduced-data", "prefers-contrast"}) {
    EXPECT_TRUE(matches((std::string("(") + f + ")").c_str(), e)) << f;
  }
  e.prefers_contrast = GTEXT_CSS_MQ_CONTRAST_CUSTOM;
  EXPECT_TRUE(matches("(prefers-contrast)", e));
}

TEST(CssMedia, ZeroWithoutAUnitIsALength) {
  /* Values 4: a zero length may be written without its unit. */
  EXPECT_TRUE(matches("(min-width: 0)"));
  EXPECT_EQ(ser("(min-width: 0)"), "(min-width: 0)");
}

TEST(CssMedia, AnUnknownMediaTypeIsFalseAndItsNegationTrue) {
  /* "An unknown <media-type> must be treated as not matching. For example, the media query unknown is false ... But not unknown is true." */
  EXPECT_FALSE(matches("foo"));
  EXPECT_TRUE(matches("not foo"));
  EXPECT_EQ(ser("foo"), "foo");
  /* "User agents must recognize the following media types as valid, but must make them match nothing": tty tv projection handheld braille embossed aural speech. */
  for (const char * t : {"tty", "tv", "projection", "handheld", "braille", "embossed", "aural", "speech"}) {
    EXPECT_FALSE(matches(t)) << t;
    EXPECT_TRUE(matches(std::string("not ") + t)) << t;
    EXPECT_EQ(ser(t), t);
  }
}

TEST(CssMedia, MediaTypesMatchTheMedium) {
  GTEXT_CSS_Media_Environment e = base_env();
  EXPECT_TRUE(matches("all", e));
  EXPECT_TRUE(matches("screen", e));
  EXPECT_FALSE(matches("print", e));
  e.medium = GTEXT_CSS_MEDIUM_PRINT;
  EXPECT_TRUE(matches("all", e));
  EXPECT_FALSE(matches("screen", e));
  EXPECT_TRUE(matches("print", e));
  e.medium = GTEXT_CSS_MEDIUM_SPEECH;
  EXPECT_TRUE(matches("all", e));
  EXPECT_FALSE(matches("screen", e));
  EXPECT_FALSE(matches("print", e));
  EXPECT_FALSE(matches("speech", e)) << "the served draft lists speech among the deprecated types, which match nothing";
}

TEST(CssMedia, TypeAndConditionAreBothNeeded) {
  GTEXT_CSS_Media_Environment e = base_env();
  EXPECT_TRUE(matches("screen and (color)", e));
  EXPECT_FALSE(matches("print and (color)", e));
  EXPECT_FALSE(matches("screen and (monochrome)", e));
  EXPECT_TRUE(matches("not print and (color)", e));
  EXPECT_FALSE(matches("not screen and (color)", e)) << "the entire media query is negated, not just the media type";
  EXPECT_TRUE(matches("not screen and (monochrome)", e));
}

TEST(CssMedia, AnUnknownFeatureIsGeneralEnclosedAndEvaluatesUnknown) {
  /* "An unknown <mf-name> ... results in the value 'unknown'." The query (foo: bar) is unknown, which is false at the top. */
  EXPECT_EQ(tri("(foo: bar)"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_FALSE(matches("(foo: bar)"));
  EXPECT_EQ(tri("not (foo: bar)"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_FALSE(matches("not (foo: bar)"));
  EXPECT_TRUE(matches("(foo: bar) or (color)"));
  EXPECT_FALSE(matches("(foo: bar) and (color)"));
  EXPECT_EQ(ser("(foo: bar)"), "(foo: bar)");
  EXPECT_EQ(ser("(foo:bar)"), "(foo:bar)") << "<general-enclosed> is written as it was";
  /* "this media query list is turned into not all, (color)": screen and (max-weight: 3kg) and (color), (color). */
  EXPECT_TRUE(matches("screen and (max-weight: 3kg) and (color), (color)"));
  EXPECT_FALSE(matches("screen and (max-weight: 3kg) and (color)"));
}

TEST(CssMedia, GeneralEnclosedFunctionsAndEmptyBlocksAreUnknown) {
  /* <general-enclosed> = [ <function-token> <any-value>? ) ] | [ ( <any-value>? ) ]: "The result is unknown." */
  EXPECT_EQ(tri("foo(bar)"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_EQ(tri("()"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_EQ(tri("(a b c)"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_EQ(ser("foo(bar)"), "foo(bar)");
  EXPECT_EQ(ser("()"), "()");
  EXPECT_FALSE(matches("not foo(bar)"));
  EXPECT_TRUE(matches("foo(bar) or (color)"));
}

TEST(CssMedia, TheGeneralEnclosedBranchIsOnlyChosenWhenTheOthersDoNotMatch) {
  /* A parenthesized group that is neither a condition nor a feature is general-enclosed; mixing and/or inside parentheses is that, not an error of the query. */
  EXPECT_EQ(tri("((color) and (pointer) or (hover))"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_TRUE(matches("((color) and (pointer) or (hover)) or (color)"));
  EXPECT_EQ(ser("((color) and (pointer) or (hover))"), "((color) and (pointer) or (hover))");
}

TEST(CssMedia, ACustomPropertyNameIsAnUnknownFeatureNotAnError) {
  /* Web-platform-tests mq-unknown-feature-custom-property.html. */
  EXPECT_EQ(tri("(--foo: bar)"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_EQ(ser("(--foo: bar)"), "(--foo: bar)");
}

/* ---- the three-valued logic (Media Queries 4, 3.1) ------------------------------------------------------------------------------------------------ */

TEST(CssMedia, KleeneTables) {
  /* T = (color) on a color screen, F = (monochrome), U = (foo). */
  const char * const T = "(color)";
  const char * const F = "(monochrome)";
  const char * const U = "(foo)";
  const char * names[3] = {T, F, U};
  const GTEXT_CSS_Tristate vals[3] = {GTEXT_CSS_TRI_TRUE, GTEXT_CSS_TRI_FALSE, GTEXT_CSS_TRI_UNKNOWN};
  auto kleene_and = [](GTEXT_CSS_Tristate a, GTEXT_CSS_Tristate b) {
    if (a == GTEXT_CSS_TRI_FALSE || b == GTEXT_CSS_TRI_FALSE) return GTEXT_CSS_TRI_FALSE;
    if (a == GTEXT_CSS_TRI_UNKNOWN || b == GTEXT_CSS_TRI_UNKNOWN) return GTEXT_CSS_TRI_UNKNOWN;
    return GTEXT_CSS_TRI_TRUE;
  };
  auto kleene_or = [](GTEXT_CSS_Tristate a, GTEXT_CSS_Tristate b) {
    if (a == GTEXT_CSS_TRI_TRUE || b == GTEXT_CSS_TRI_TRUE) return GTEXT_CSS_TRI_TRUE;
    if (a == GTEXT_CSS_TRI_UNKNOWN || b == GTEXT_CSS_TRI_UNKNOWN) return GTEXT_CSS_TRI_UNKNOWN;
    return GTEXT_CSS_TRI_FALSE;
  };
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(tri(names[i]), vals[i]) << names[i];
    GTEXT_CSS_Tristate n = vals[i] == GTEXT_CSS_TRI_TRUE ? GTEXT_CSS_TRI_FALSE : (vals[i] == GTEXT_CSS_TRI_FALSE ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_UNKNOWN);
    EXPECT_EQ(tri(std::string("not ") + names[i]), n) << "not " << names[i] << ": the negation of unknown is unknown";
    for (int j = 0; j < 3; j++) {
      EXPECT_EQ(tri(std::string(names[i]) + " and " + names[j]), kleene_and(vals[i], vals[j])) << names[i] << " and " << names[j];
      EXPECT_EQ(tri(std::string(names[i]) + " or " + names[j]), kleene_or(vals[i], vals[j])) << names[i] << " or " << names[j];
    }
  }
}

TEST(CssMedia, UnknownAtTheTopOfAListIsFalse) {
  EXPECT_FALSE(matches("(foo)"));
  EXPECT_FALSE(matches("(foo), (monochrome)"));
  EXPECT_TRUE(matches("(foo), (color)"));
}

TEST(CssMedia, TheEmptyListIsTrue) {
  /* "This definition of <media-query-list> parsing intentionally accepts an empty list." */
  EXPECT_TRUE(matches(""));
  Parsed p("");
  EXPECT_EQ(gtext_css_media_list_query_count(p.list), 0u);
}

/* ---- the environment: support, defaults, units ----------------------------------------------------------------------------------------------------- */

TEST(CssMedia, AFeatureTheEnvironmentDoesNotSupportIsUnknown) {
  /* "If a media feature references a concept which does not exist on the device ... must always evaluate to false" for the unsupported; here it is unknown: false, and its not false. */
  GTEXT_CSS_Media_Environment e = base_env();
  gtext_css_media_environment_set_supported(&e, "prefers-reduced-data", false);
  e.prefers_reduced_data = GTEXT_CSS_MQ_REDUCE_REDUCE;
  EXPECT_EQ(tri("(prefers-reduced-data)", e), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_EQ(tri("not (prefers-reduced-data)", e), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_FALSE(matches("(prefers-reduced-data: reduce)", e));
  EXPECT_FALSE(matches("not (prefers-reduced-data: reduce)", e));
  gtext_css_media_environment_set_supported(&e, "prefers-reduced-data", true);
  EXPECT_TRUE(matches("(prefers-reduced-data: reduce)", e));
}

TEST(CssMedia, AZeroedEnvironmentSupportsNothing) {
  GTEXT_CSS_Media_Environment e;
  std::memset(&e, 0, sizeof e);
  e.size = sizeof e;
  e.viewport = {100, 100};
  e.device = {100, 100};
  e.resolution = 1;
  e.default_font_size = 16;
  for (size_t i = 0; i < gtext_css_media_feature_count(); i++) {
    GTEXT_CSS_Media_Feature_Row r;
    ASSERT_TRUE(gtext_css_media_feature_row(i, &r));
    EXPECT_FALSE(gtext_css_media_environment_is_supported(&e, r.name)) << r.name;
    EXPECT_EQ(tri(std::string("(") + r.name + ")", e), GTEXT_CSS_TRI_UNKNOWN) << r.name;
  }
}

TEST(CssMedia, TheDefaultEnvironmentIsTheScreenBaseline) {
  GTEXT_CSS_Media_Environment e = base_env();
  EXPECT_TRUE(matches("(width: 1280px) and (height: 800px) and (resolution: 1dppx) and (color: 8) and (hover) and (pointer: fine) and (prefers-color-scheme: light)", e));
  EXPECT_TRUE(matches("(prefers-contrast: no-preference) and (scripting: enabled) and (orientation: landscape)", e));
  EXPECT_EQ(tri("(display-state)", e), GTEXT_CSS_TRI_UNKNOWN) << "the baseline cannot say what a window is";
  EXPECT_EQ(gtext_css_media_environment_check(&e, nullptr), GTEXT_CSS_OK);
}

TEST(CssMedia, TheDefaultEnvironmentTakesTheViewportFromTheComputedValueEnvironment) {
  GTEXT_CSS_Environment b = gtext_css_environment_default();
  b.dynamic_viewport = {600, 900};
  b.device_pixel_ratio = 2.5;
  b.default_font_size = 20;
  b.color_scheme = GTEXT_CSS_SCHEME_DARK;
  GTEXT_CSS_Media_Environment e = gtext_css_media_environment_default(&b);
  EXPECT_TRUE(matches("(width: 600px) and (height: 900px) and (orientation: portrait) and (resolution: 2.5dppx) and (prefers-color-scheme: dark) and (min-width: 30em)", e));
}

TEST(CssMedia, EnvironmentCheckRefusesWhatIsNotAnEnvironment) {
  const char * why = nullptr;
  GTEXT_CSS_Media_Environment e = base_env();
  e.viewport.width = 0;
  EXPECT_EQ(gtext_css_media_environment_check(&e, &why), GTEXT_CSS_E_INVALID);
  EXPECT_NE(std::string(why).find("viewport"), std::string::npos);
  e = base_env();
  e.viewport.height = NAN;
  EXPECT_EQ(gtext_css_media_environment_check(&e, &why), GTEXT_CSS_E_INVALID);
  e = base_env();
  e.resolution = INFINITY;
  EXPECT_EQ(gtext_css_media_environment_check(&e, &why), GTEXT_CSS_E_INVALID);
  EXPECT_NE(std::string(why).find("resolution"), std::string::npos);
  e.resolution_infinite = true;
  EXPECT_EQ(gtext_css_media_environment_check(&e, &why), GTEXT_CSS_OK);
  e = base_env();
  e.default_font_size = -1;
  EXPECT_EQ(gtext_css_media_environment_check(&e, &why), GTEXT_CSS_E_INVALID);
  e = base_env();
  e.pointer = 7;
  EXPECT_EQ(gtext_css_media_environment_check(&e, &why), GTEXT_CSS_E_INVALID);
  EXPECT_NE(std::string(why).find("pointer"), std::string::npos);
  e = base_env();
  e.medium = (GTEXT_CSS_Medium)9;
  EXPECT_EQ(gtext_css_media_environment_check(&e, &why), GTEXT_CSS_E_INVALID);
  e = base_env();
  e.any_hover = 8;
  EXPECT_EQ(gtext_css_media_environment_check(&e, &why), GTEXT_CSS_E_INVALID);
  e = base_env();
  e.size = 8;
  EXPECT_EQ(gtext_css_media_environment_check(&e, &why), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_media_environment_check(nullptr, &why), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Conditions_Options o = gtext_css_conditions_options_default();
  e = base_env();
  e.viewport.width = -4;
  o.media = &e;
  GTEXT_CSS_Conditions * c = nullptr;
  EXPECT_EQ(gtext_css_conditions_new(nullptr, &o, &c), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(c, nullptr);
}

TEST(CssMedia, AnEnvironmentBuiltBeforeAFieldWasAddedKeepsWorking) {
  /* Size-prefixed: a caller whose struct ends at default_font_size gets zero (unsupported) for the rest, and the rest of its struct is read as it is. */
  GTEXT_CSS_Media_Environment full = base_env();
  full.size = offsetof(GTEXT_CSS_Media_Environment, default_font_size) + sizeof full.default_font_size;
  full.viewport = {500, 300};
  Cond c(full);
  bool m = false;
  ASSERT_EQ(gtext_css_conditions_matches_media(c.c, "(width: 500px)", 14, nullptr, &m), GTEXT_CSS_OK);
  EXPECT_TRUE(m);
  EXPECT_EQ(gtext_css_conditions_media_environment(c.c)->size, sizeof(GTEXT_CSS_Media_Environment));
}

TEST(CssMedia, FontRelativeUnitsUseTheEnvironmentsFontSizeNotTheDocuments) {
  /* "(min-width: 20em) ... The em value is relative to the initial value of font-size"; mq-calc-002: a :root { font-size: 30000px } does not change it. */
  GTEXT_CSS_Media_Environment e = sized(600, 800);
  e.default_font_size = 16;
  EXPECT_TRUE(matches("(min-width: 37.5em)", e)) << "37.5 x 16 is 600, and the comparison is exact";
  EXPECT_FALSE(matches("(min-width: 37.51em)", e));
  EXPECT_TRUE(matches("(width: 37.5rem)", e));
  EXPECT_TRUE(matches("(min-width: calc(1em))", e));
  EXPECT_TRUE(matches("(min-width: calc(1px + 1rem))", e));
  e.default_font_size = 20;
  EXPECT_TRUE(matches("(width: 30em)", e));
}

TEST(CssMedia, AbsoluteUnits) {
  GTEXT_CSS_Media_Environment e = sized(96, 192);
  EXPECT_TRUE(matches("(width: 1in)", e));
  EXPECT_TRUE(matches("(width: 2.54cm)", e));
  EXPECT_TRUE(matches("(width: 25.4mm)", e));
  EXPECT_TRUE(matches("(width: 72pt)", e));
  EXPECT_TRUE(matches("(width: 6pc)", e));
  EXPECT_TRUE(matches("(width: 101.6Q)", e));
  EXPECT_TRUE(matches("(height: 2in)", e));
  EXPECT_TRUE(matches("(min-width: calc(100in)) or (width: 96px)", e));
  EXPECT_FALSE(matches("(min-width: calc(100in))", e)) << "mq-calc-006";
}

TEST(CssMedia, ViewportUnitsUseTheViewportOfTheEnvironment) {
  GTEXT_CSS_Media_Environment e = sized(500, 300);
  EXPECT_TRUE(matches("(width: 100vw)", e));
  EXPECT_TRUE(matches("(width: 100dvw)", e));
  EXPECT_TRUE(matches("(height: 100vh)", e));
  EXPECT_TRUE(matches("(height: 100vmin)", e));
  EXPECT_TRUE(matches("(width: 100vmax)", e));
}

TEST(CssMedia, MathFunctionsInFeatureValues) {
  /* The matrix: (width > calc(100px + 5em)) at 200px is true (180px). */
  EXPECT_TRUE(matches("(width > calc(100px + 5em))", sized(200, 100)));
  EXPECT_FALSE(matches("(width > calc(100px + 5em))", sized(180, 100)));
  EXPECT_TRUE(matches("(width > calc(1px * (1 + sign(16px - 1rem))))", sized(2, 1))) << "mq-calc-sign-function-001";
  EXPECT_TRUE(matches("(min-aspect-ratio: calc(59/79))", sized(100, 100))) << "mq-calc-008";
  EXPECT_FALSE(matches("(width: calc(1 + 1))")) << "a number is not a length";
}

TEST(CssMedia, FontMetricUnitsNeedTheMetricCallback) {
  /* The matrix: (width > 10ex) with no font_metric is no result: GTEXT_CSS_E_UNSUPPORTED naming ex. */
  GTEXT_CSS_Media_Environment e = sized(200, 100);
  Parsed p("(width > 10ex)");
  Cond c(e);
  GTEXT_CSS_Condition_Call call;
  gtext_css_condition_call_init(&call);
  bool m = true;
  EXPECT_EQ(gtext_css_conditions_matches_list(c.c, p.list, &call, &m), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_TRUE(m) << "written only on success";
  EXPECT_STREQ(call.detail, "ex");
  gtext_css_condition_call_free(&call);
  e.font_metric = [](void *, const GTEXT_CSS_Font_Query * q, GTEXT_CSS_Font_Metric metric, double * px) {
    EXPECT_EQ(q->size_px, 16.0);
    *px = metric == GTEXT_CSS_METRIC_EX ? 8.0 : 10.0;
    return true;
  };
  EXPECT_TRUE(matches("(width > 10ex)", e));
  EXPECT_FALSE(matches("(width > 30ex)", e));
  EXPECT_TRUE(matches("(width = 20ch)", e));
}

TEST(CssMedia, LineHeightUnitsNeedNoCallback) {
  /* `normal` has no used value without a font: `lh` and `rlh` are 1.2 em of the initial font size, with no font_metric callback, so the answer is a result and
   * never E_UNSUPPORTED (`ex`, `ch`, `cap` and `ic` still are, above). */
  GTEXT_CSS_Media_Environment e = sized(192, 100); /* 1.2 * 16 = 19.2; 10lh is 192px */
  EXPECT_TRUE(matches("(width = 10lh)", e));
  EXPECT_TRUE(matches("(width = 10rlh)", e));
  EXPECT_TRUE(matches("(width > 9lh)", e));
  EXPECT_FALSE(matches("(width > 10lh)", e));
  e.default_font_size = 20; /* 24px a line: 8lh is 192px */
  EXPECT_TRUE(matches("(width = 8lh)", e));
}

/* ---- resolution, ratios, ranges ---------------------------------------------------------------------------------------------------------------------- */

TEST(CssMedia, ResolutionUnitsAndInfinite) {
  /* The matrix: (min-resolution: 2dppx), 192dpi, 2x and (resolution: infinite) at 2 dppx: true, true, true, false. */
  GTEXT_CSS_Media_Environment e = base_env();
  e.resolution = 2;
  EXPECT_TRUE(matches("(min-resolution: 2dppx)", e));
  EXPECT_TRUE(matches("(min-resolution: 192dpi)", e));
  EXPECT_TRUE(matches("(min-resolution: 2x)", e));
  EXPECT_TRUE(matches("(resolution: 2x)", e));
  EXPECT_TRUE(matches("(resolution: 192dpi)", e));
  EXPECT_TRUE(matches("(resolution: 75.5905511811dpcm) or (resolution: 2dppx)", e));
  EXPECT_FALSE(matches("(resolution: infinite)", e));
  EXPECT_FALSE(matches("(resolution > 3dppx)", e));
  /* "infinite must be treated as larger than any possible <resolution>." */
  e.resolution_infinite = true;
  EXPECT_TRUE(matches("(resolution: infinite)", e));
  EXPECT_TRUE(matches("(resolution > 1000dpi)", e));
  EXPECT_TRUE(matches("(min-resolution: 1000000dppx)", e));
  EXPECT_FALSE(matches("(max-resolution: 1000000dppx)", e));
  EXPECT_TRUE(matches("(resolution >= infinite)", e));
}

TEST(CssMedia, NegativeRangeIsFalseNotUnknown) {
  /* "The following examples result in a green background on all visual devices: not (width <= -100px), (height > -100px), not (resolution: -300dpi)".
   * The first two hold; the third does not here: `<resolution [0,inf]>` has no negative value, so `(resolution: -300dpi)` is an unknown leaf and its negation
   * unknown, as it is in both browsers (the draft's example was written before the range was). */
  EXPECT_TRUE(matches("not (width <= -100px)"));
  EXPECT_TRUE(matches("(height > -100px)"));
  EXPECT_FALSE(matches("not (resolution: -300dpi)"));
  EXPECT_FALSE(matches("(width = -100px)"));
  EXPECT_FALSE(matches("(width < -100px)"));
  EXPECT_FALSE(matches("(width <= -100px)"));
  EXPECT_TRUE(matches("(width >= -100px)"));
  EXPECT_TRUE(matches("(min-width: -100px)"));
  EXPECT_FALSE(matches("(max-width: -100px)"));
  EXPECT_EQ(tri("not (width <= -100px)"), GTEXT_CSS_TRI_TRUE);
}

TEST(CssMedia, TheRangeFormsAndThePrefixesAgree) {
  GTEXT_CSS_Media_Environment e = sized(500, 800);
  /* The matrix: (400px <= width <= 700px), width 500, then 800. */
  EXPECT_TRUE(matches("(400px <= width <= 700px)", e));
  e.viewport.width = 800;
  EXPECT_FALSE(matches("(400px <= width <= 700px)", e));
  e.viewport.width = 500;
  EXPECT_TRUE(matches("(700px >= width >= 400px)", e));
  EXPECT_TRUE(matches("(400px < width < 700px)", e));
  e.viewport.width = 400;
  EXPECT_TRUE(matches("(400px <= width <= 700px)", e));
  EXPECT_FALSE(matches("(400px < width < 700px)", e)) << "(400px < width < 1000px) is not true at 400px";
  EXPECT_TRUE(matches("(width = 400px)", e));
  EXPECT_TRUE(matches("(width: 400px)", e));
  EXPECT_TRUE(matches("(min-width: 400px)", e));
  EXPECT_TRUE(matches("(max-width: 400px)", e));
  EXPECT_FALSE(matches("(min-width: 401px)", e));
  EXPECT_TRUE(matches("(width >= 400px)", e));
  EXPECT_TRUE(matches("(width <= 400px)", e));
  EXPECT_FALSE(matches("(width > 400px)", e));
  EXPECT_FALSE(matches("(width < 400px)", e));
  EXPECT_TRUE(matches("(400px = width)", e));
  EXPECT_TRUE(matches("(399px < width)", e));
  EXPECT_TRUE(matches("(401px > width)", e));
}

TEST(CssMedia, RangeWithOneBadSideInAListLeavesTheOtherQueries) {
  /* The matrix: the list `(color), (400px < width > 700px)`: the second is an unknown leaf (false), kept as written; the list is true on a color device. */
  Parsed p("(color), (400px < width > 700px)");
  EXPECT_EQ(p.text(), "(color), (400px < width > 700px)");
  EXPECT_TRUE(matches("(color), (400px < width > 700px)"));
  EXPECT_GE(gtext_css_media_result_diagnostic_count(&p.result), 1u);
}

TEST(CssMedia, BooleanContext) {
  /* Media Queries 4, 2.4.2: true for any value other than 0, a dimension 0 or the keyword none. The matrix: (color) on color_bits 0 is false; (grid) is false. */
  GTEXT_CSS_Media_Environment e = base_env();
  EXPECT_TRUE(matches("(color)", e));
  e.color_bits = 0;
  EXPECT_FALSE(matches("(color)", e));
  EXPECT_TRUE(matches("not (color)", e));
  EXPECT_FALSE(matches("(grid)", e));
  e.grid = true;
  EXPECT_TRUE(matches("(grid)", e));
  EXPECT_TRUE(matches("(width)", e));
  EXPECT_TRUE(matches("(pointer)", e));
  e.pointer = GTEXT_CSS_MQ_POINTER_NONE;
  EXPECT_FALSE(matches("(pointer)", e));
  e.update = GTEXT_CSS_MQ_UPDATE_NONE;
  EXPECT_FALSE(matches("(update)", e));
  EXPECT_TRUE(matches("not (update)", e));
  EXPECT_TRUE(matches("(scan)", e)) << "(scan) is always true if the feature applies";
  e.color_gamut = GTEXT_CSS_MQ_GAMUT_NONE;
  EXPECT_FALSE(matches("(color-gamut)", e)) << "not (color-gamut) detects a very low-quality screen: none of its values match";
  EXPECT_TRUE(matches("(update: none) and (not (update))", e));
  EXPECT_FALSE(matches("(update: fast)", e));
}

TEST(CssMedia, AnyPointerAndAnyHoverAreSets) {
  GTEXT_CSS_Media_Environment e = base_env();
  e.any_pointer = GTEXT_CSS_MQ_ANY_POINTER_COARSE | GTEXT_CSS_MQ_ANY_POINTER_FINE;
  EXPECT_TRUE(matches("(any-pointer: coarse)", e));
  EXPECT_TRUE(matches("(any-pointer: fine)", e));
  EXPECT_FALSE(matches("(any-pointer: none)", e));
  EXPECT_TRUE(matches("(any-pointer)", e));
  e.any_pointer = GTEXT_CSS_MQ_ANY_POINTER_NONE;
  EXPECT_TRUE(matches("(any-pointer: none)", e));
  EXPECT_FALSE(matches("(any-pointer)", e));
  e.any_hover = 0;
  EXPECT_FALSE(matches("(any-hover)", e));
  EXPECT_FALSE(matches("(any-hover: none)", e));
}

TEST(CssMedia, GamutsImplyTheNarrowerOnes) {
  GTEXT_CSS_Media_Environment e = base_env();
  e.color_gamut = GTEXT_CSS_MQ_GAMUT_REC2020;
  EXPECT_TRUE(matches("(color-gamut: srgb) and (color-gamut: p3) and (color-gamut: rec2020)", e));
  e.color_gamut = GTEXT_CSS_MQ_GAMUT_SRGB;
  EXPECT_TRUE(matches("(color-gamut: srgb)", e));
  EXPECT_FALSE(matches("(color-gamut: p3)", e));
  e.dynamic_range = GTEXT_CSS_MQ_RANGE_HIGH;
  EXPECT_TRUE(matches("(dynamic-range: standard) and (dynamic-range: high)", e));
}

TEST(CssMedia, RatiosAreComparedByCrossMultiplication) {
  /* The matrix: (aspect-ratio: 16/9) at 1280 by 720: true. */
  GTEXT_CSS_Media_Environment e = sized(1280, 720);
  EXPECT_TRUE(matches("(aspect-ratio: 16/9)", e));
  EXPECT_TRUE(matches("(aspect-ratio: 16 / 9)", e));
  EXPECT_TRUE(matches("(aspect-ratio: 32/18)", e));
  EXPECT_TRUE(matches("(aspect-ratio: 1280/720)", e));
  EXPECT_TRUE(matches("(min-aspect-ratio: 16/9)", e));
  EXPECT_TRUE(matches("(max-aspect-ratio: 16/9)", e));
  EXPECT_TRUE(matches("(aspect-ratio > 3/2)", e));
  EXPECT_TRUE(matches("(3/2 < aspect-ratio < 2/1)", e));
  EXPECT_FALSE(matches("(aspect-ratio < 16/9)", e));
  EXPECT_TRUE(matches("(aspect-ratio >= 16/9)", e));
  /* 59/79 against a square viewport: 59/79 < 1. */
  EXPECT_TRUE(matches("(aspect-ratio > 59/79)", sized(100, 100)));
  EXPECT_TRUE(matches("(device-aspect-ratio: 16/9)", base_env()) == false);
}

TEST(CssMedia, DegenerateRatios) {
  /* Values 4, 5.7: "If either number in the <ratio> is 0 or infinite, it represents a degenerate ratio". A ratio with a zero denominator is larger than any ordinary one;
   * 0/0 is not ordered with anything. */
  GTEXT_CSS_Media_Environment e = sized(1280, 720);
  EXPECT_TRUE(matches("(aspect-ratio < 1/0)", e));
  EXPECT_FALSE(matches("(aspect-ratio > 1/0)", e));
  EXPECT_TRUE(matches("(aspect-ratio > 0/1)", e));
  EXPECT_FALSE(matches("(aspect-ratio = 0/0)", e));
  EXPECT_FALSE(matches("(aspect-ratio > 0/0)", e));
  EXPECT_FALSE(matches("(aspect-ratio < 0/0)", e));
  EXPECT_EQ(ser("(aspect-ratio: 0/0)"), "(aspect-ratio: 0 / 0)");
}

TEST(CssMedia, OrientationFollowsTheViewport) {
  /* Media Queries 4, 4.4: portrait when the height is greater than or equal to the width. */
  EXPECT_TRUE(matches("(orientation: portrait)", sized(500, 500)));
  EXPECT_FALSE(matches("(orientation: landscape)", sized(500, 500)));
  EXPECT_TRUE(matches("(orientation: landscape)", sized(501, 500)));
  EXPECT_TRUE(matches("(orientation: portrait)", sized(499, 500)));
}

TEST(CssMedia, TheDeviceFeaturesReadTheDevice) {
  GTEXT_CSS_Media_Environment e = base_env();
  e.device = {1920, 1080};
  EXPECT_TRUE(matches("(device-width: 1920px) and (device-height: 1080px) and (device-aspect-ratio: 16/9)", e));
  EXPECT_TRUE(matches("(width: 1280px)", e));
}

TEST(CssMedia, CaseInsensitivity) {
  /* "As per [CSS-SYNTAX-3], tokens are ASCII case-insensitive." (Web-platform-tests mq-case-insensitive-001.) */
  EXPECT_TRUE(matches("SCREEN AND (MIN-WIDTH: 100PX) AND (ORIENTATION: LANDSCAPE)"));
  EXPECT_TRUE(matches("(Color)"));
  EXPECT_TRUE(matches("NOT PRINT"));
}

TEST(CssMedia, ExtensionFeaturesAreAskedOfTheCallback) {
  GTEXT_CSS_Media_Environment e = base_env();
  e.extension_feature = [](void *, const GTEXT_CSS_String * name, const GTEXT_CSS_String * value) {
    std::string n(name->data, name->length);
    if (n == "-vendor-thing") {
      return value ? (std::string(value->data, value->length) == "5" ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE) : GTEXT_CSS_TRI_TRUE;
    }
    return GTEXT_CSS_TRI_UNKNOWN;
  };
  EXPECT_TRUE(matches("(-vendor-thing)", e));
  EXPECT_TRUE(matches("(-vendor-thing: 5)", e));
  EXPECT_FALSE(matches("(-vendor-thing: 6)", e));
  EXPECT_FALSE(matches("(-other-thing)", e));
}

/* ---- custom media (Media Queries 5, 10) --------------------------------------------------------------------------------------------------------------- */

namespace {

struct Defs {
  GTEXT_CSS_Condition_Defs * d = nullptr;
  Defs() {
    EXPECT_EQ(gtext_css_condition_defs_new(nullptr, &d), GTEXT_CSS_OK);
  }
  ~Defs() {
    gtext_css_condition_defs_free(d);
  }
  Defs(const Defs &) = delete;
  Defs & operator=(const Defs &) = delete;
  void add(const std::string & t) {
    bool added = false;
    EXPECT_EQ(gtext_css_condition_defs_add_custom_media_text(d, t.data(), t.size(), nullptr, &added), GTEXT_CSS_OK) << t;
    EXPECT_TRUE(added) << t;
  }
};

bool matches_with(Defs & defs, const std::string & q, const GTEXT_CSS_Media_Environment & env, bool custom = true) {
  Cond c(env, defs.d, custom);
  bool m = false;
  EXPECT_EQ(gtext_css_conditions_matches_media(c.c, q.data(), q.size(), nullptr, &m), GTEXT_CSS_OK) << q;
  return m;
}

}  // namespace

TEST(CssMediaCustom, TheMatrixRow) {
  /* `@custom-media --narrow (max-width: 30em); @media (--narrow)` at 400px: true; `not (--narrow)` false. */
  Defs d;
  d.add("--narrow (max-width: 30em)");
  GTEXT_CSS_Media_Environment e = sized(400, 800);
  EXPECT_TRUE(matches_with(d, "(--narrow)", e));
  EXPECT_FALSE(matches_with(d, "not (--narrow)", e));
  e.viewport.width = 600;
  EXPECT_FALSE(matches_with(d, "(--narrow)", e));
  EXPECT_TRUE(matches_with(d, "not (--narrow)", e));
}

TEST(CssMediaCustom, ItIsEvaluatedLogicallyNotAsATextualSubstitution) {
  /* "@custom-media --modern (color), (hover); @media (--modern) and (width > 1024px)" is ((color) or (hover)) and (width > 1024px), not (color), (hover) and ... */
  Defs d;
  d.add("--modern (color), (hover)");
  GTEXT_CSS_Media_Environment e = sized(500, 500);
  EXPECT_FALSE(matches_with(d, "(--modern) and (width > 1024px)", e));
  e.viewport.width = 2000;
  EXPECT_TRUE(matches_with(d, "(--modern) and (width > 1024px)", e));
}

TEST(CssMediaCustom, TrueAndFalse) {
  Defs d;
  d.add("--yes true");
  d.add("--no false");
  EXPECT_TRUE(matches_with(d, "(--yes)", base_env()));
  EXPECT_FALSE(matches_with(d, "(--no)", base_env()));
  EXPECT_TRUE(matches_with(d, "not (--no)", base_env()));
  EXPECT_TRUE(matches_with(d, "(--YES)", base_env()) == false) << "the names are case-sensitive";
}

TEST(CssMediaCustom, TheLastDefinitionAlone) {
  /* "If multiple @custom-media rules declare the same <extension-name>, the truth value is based on the last one alone". */
  Defs d;
  d.add("--x true");
  d.add("--x false");
  EXPECT_FALSE(matches_with(d, "(--x)", base_env()));
  Defs d2;
  d2.add("--x false");
  d2.add("--x true");
  EXPECT_TRUE(matches_with(d2, "(--x)", base_env()));
}

TEST(CssMediaCustom, AnUndefinedNameIsGeneralEnclosed) {
  Defs d;
  EXPECT_EQ(tri("(--nope)"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_FALSE(matches_with(d, "(--nope)", base_env()));
  EXPECT_FALSE(matches_with(d, "not (--nope)", base_env()));
}

TEST(CssMediaCustom, ACycleMakesEveryNameOnItUndefined) {
  /* "a custom media query must not be defined in terms of itself or of another ... that directly or indirectly refers to it. Any such attempt ... must cause all the
   * custom media queries in the loop to fail to be defined." The matrix: --a by (--b), --b by (--a): both unknown, found in one pass. */
  Defs d;
  d.add("--a (--b)");
  d.add("--b (--a)");
  d.add("--c (--a), (color)");
  d.add("--self (--self)");
  d.add("--ok (color)");
  Cond c(base_env(), d.d);
  ASSERT_EQ(gtext_css_conditions_cycle_count(c.c), 3u);
  EXPECT_STREQ(gtext_css_conditions_cycle_name(c.c, 0), "--a");
  EXPECT_STREQ(gtext_css_conditions_cycle_name(c.c, 1), "--b");
  EXPECT_STREQ(gtext_css_conditions_cycle_name(c.c, 2), "--self");
  EXPECT_EQ(gtext_css_conditions_cycle_name(c.c, 3), nullptr);
  auto t = [&](const char * q) {
    Parsed p(q);
    GTEXT_CSS_Tristate r;
    EXPECT_EQ(gtext_css_conditions_evaluate_query(c.c, p.list, 0, nullptr, &r), GTEXT_CSS_OK);
    return r;
  };
  EXPECT_EQ(t("(--a)"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_EQ(t("(--b)"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_EQ(t("not (--a)"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_EQ(t("(--self)"), GTEXT_CSS_TRI_UNKNOWN);
  EXPECT_EQ(t("(--ok)"), GTEXT_CSS_TRI_TRUE);
  /* --c is not on the cycle; it refers to an undefined name, which is false in its list, and (color) makes it true. */
  EXPECT_EQ(t("(--c)"), GTEXT_CSS_TRI_TRUE);
}

TEST(CssMediaCustom, ACycleIsFoundEvenWhereAShortCircuitNeverReachesIt) {
  Defs d;
  d.add("--a (color), (--b)");
  d.add("--b (--a)");
  Cond c(base_env(), d.d);
  EXPECT_EQ(gtext_css_conditions_cycle_count(c.c), 2u);
  bool m = true;
  ASSERT_EQ(gtext_css_conditions_matches_media(c.c, "(--a)", 5, nullptr, &m), GTEXT_CSS_OK);
  EXPECT_FALSE(m) << "undefined, so unknown, so false";
}

TEST(CssMediaCustom, BrowserModeIgnoresTheDefinitions) {
  /* The option custom_media off is the behavior of every shipping browser: (--narrow) is general-enclosed. */
  Defs d;
  d.add("--narrow (max-width: 30em)");
  EXPECT_FALSE(matches_with(d, "(--narrow)", sized(400, 800), false));
  EXPECT_FALSE(matches_with(d, "not (--narrow)", sized(400, 800), false));
}

TEST(CssMediaCustom, AChainOfDefinitionsIsEvaluatedOnceEach) {
  const int n = 2000;
  Defs d;
  d.add("--c0 (color)");
  for (int i = 1; i < n; i++) {
    d.add("--c" + std::to_string(i) + " (--c" + std::to_string(i - 1) + ")");
  }
  Cond c(base_env(), d.d);
  GTEXT_CSS_Condition_Call call;
  gtext_css_condition_call_init(&call);
  std::string q = "(--c" + std::to_string(n - 1) + ")";
  Parsed p(q);
  bool m = false;
  ASSERT_EQ(gtext_css_conditions_matches_list(c.c, p.list, &call, &m), GTEXT_CSS_OK);
  EXPECT_TRUE(m);
  EXPECT_EQ(call.defs_evaluated, (size_t)n);
  /* A second use in the same call context is answered from the memo. */
  ASSERT_EQ(gtext_css_conditions_matches_list(c.c, p.list, &call, &m), GTEXT_CSS_OK);
  EXPECT_EQ(call.defs_evaluated, (size_t)n);
  EXPECT_GE(call.memo_hits, 1u);
  gtext_css_condition_call_free(&call);
}

TEST(CssMediaCustom, DefinitionsAreSealedByTheFirstConditionsObject) {
  Defs d;
  d.add("--x true");
  Cond c(base_env(), d.d);
  bool added = false;
  EXPECT_EQ(gtext_css_condition_defs_add_custom_media_text(d.d, "--y true", 8, nullptr, &added), GTEXT_CSS_E_INVALID);
  EXPECT_FALSE(added);
}

TEST(CssMediaCustom, APreludeThatIsNotADefinitionIsNotAdded) {
  Defs d;
  bool added = true;
  EXPECT_EQ(gtext_css_condition_defs_add_custom_media_text(d.d, "narrow (color)", 14, nullptr, &added), GTEXT_CSS_OK);
  EXPECT_FALSE(added);
  EXPECT_EQ(gtext_css_condition_defs_add_custom_media_text(d.d, "--x", 3, nullptr, &added), GTEXT_CSS_OK);
  EXPECT_FALSE(added);
  EXPECT_EQ(gtext_css_condition_defs_add_custom_media_text(d.d, "", 0, nullptr, &added), GTEXT_CSS_OK);
  EXPECT_FALSE(added);
  EXPECT_EQ(gtext_css_condition_defs_count(d.d), 0u);
}

TEST(CssMediaCustom, UsedInAPlainOrRangeContextItIsInvalid) {
  /* "It must be used in a boolean context; using them in a normal or range context is a syntax error." (The name is unknown as a feature, so it is general-enclosed.) */
  EXPECT_FALSE(matches("(--narrow: 1)"));
  EXPECT_EQ(tri("(--narrow: 1)"), GTEXT_CSS_TRI_UNKNOWN);
}

/* ---- limits, depth, the call context ---------------------------------------------------------------------------------------------------------------- */

TEST(CssMediaLimits, TooManyQueries) {
  GTEXT_CSS_Media_Options o = gtext_css_media_options_default();
  o.max_queries = 3;
  Parsed ok("all, all, all", &o);
  EXPECT_EQ(ok.status, GTEXT_CSS_OK);
  Parsed bad("all, all, all, all", &o);
  EXPECT_EQ(bad.status, GTEXT_CSS_E_LIMIT_CONDITIONS);
  EXPECT_EQ(bad.list, nullptr);
  EXPECT_EQ(bad.result.status, GTEXT_CSS_E_LIMIT_CONDITIONS);
}

TEST(CssMediaLimits, TooManyNodes) {
  GTEXT_CSS_Media_Options o = gtext_css_media_options_default();
  o.max_nodes = 5;
  Parsed bad("(color) and (hover) and (pointer) and (scan) and (grid)", &o);
  EXPECT_EQ(bad.status, GTEXT_CSS_E_LIMIT_CONDITIONS);
}

TEST(CssMediaLimits, ConditionDepthIsALimitAndNeverAnOverflow) {
  auto nested = [](int depth) {
    std::string s;
    for (int i = 0; i < depth; i++) s += "(not ";
    s += "(color)";
    for (int i = 0; i < depth; i++) s += ")";
    return s;
  };
  GTEXT_CSS_Media_Options o = gtext_css_media_options_default();
  o.max_depth = 64;
  Parsed ok(nested(60), &o);
  EXPECT_EQ(ok.status, GTEXT_CSS_OK);
  Parsed bad(nested(80), &o);
  EXPECT_EQ(bad.status, GTEXT_CSS_E_LIMIT_CONDITION_DEPTH);
  EXPECT_EQ(bad.list, nullptr);
}

TEST(CssMediaLimits, ADeepNestingIsHandledWithoutRecursion) {
  /* 100,000 levels: parse, serialize, evaluate, free. The tokenizer's own limits are raised to match. */
  const int depth = 100000;
  std::string s;
  s.reserve(depth * 6 + 16);
  for (int i = 0; i < depth; i++) s += "(not ";
  s += "(color)";
  for (int i = 0; i < depth; i++) s += ")";
  GTEXT_CSS_Media_Options o = gtext_css_media_options_default();
  o.max_depth = 1u << 20;
  o.max_nodes = 1u << 22;
  o.parse.max_depth = 1u << 20;
  Parsed p(s, &o);
  ASSERT_EQ(p.status, GTEXT_CSS_OK) << p.result.message;
  ASSERT_NE(p.list, nullptr);
  std::string out = p.text();
  EXPECT_EQ(out, s) << "parenthesized nots serialize as written";
  Cond c(base_env());
  bool m = false;
  ASSERT_EQ(gtext_css_conditions_matches_list(c.c, p.list, nullptr, &m), GTEXT_CSS_OK);
  /* An even number of nots over (color), true on a color screen. */
  EXPECT_EQ(m, depth % 2 == 0);
}

TEST(CssMediaLimits, ALongOrListIsLinear) {
  std::string s;
  const int n = 20000;
  for (int i = 0; i < n; i++) {
    if (i) s += " or ";
    s += "(foo)";
  }
  Parsed p(s);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  Cond c(base_env());
  GTEXT_CSS_Condition_Call call;
  gtext_css_condition_call_init(&call);
  bool m = true;
  ASSERT_EQ(gtext_css_conditions_matches_list(c.c, p.list, &call, &m), GTEXT_CSS_OK);
  EXPECT_FALSE(m);
  EXPECT_LE(call.node_visits, (size_t)(2 * n + 8));
  gtext_css_condition_call_free(&call);
}

TEST(CssMediaLimits, WorkIsBounded) {
  std::string s;
  for (int i = 0; i < 200; i++) {
    if (i) s += " or ";
    s += "(foo)";
  }
  Parsed p(s);
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.max_work = 50;
  GTEXT_CSS_Conditions * c = nullptr;
  ASSERT_EQ(gtext_css_conditions_new(nullptr, &co, &c), GTEXT_CSS_OK);
  bool m = true;
  EXPECT_EQ(gtext_css_conditions_matches_list(c, p.list, nullptr, &m), GTEXT_CSS_E_LIMIT_WORK);
  EXPECT_TRUE(m);
  gtext_css_conditions_free(c);
}

TEST(CssMediaLimits, ADeepChainOfCustomMediaDoesNotUseTheCStack) {
  const int n = 50000;
  Defs d;
  d.add("--c0 (color)");
  for (int i = 1; i < n; i++) {
    d.add("--c" + std::to_string(i) + " (--c" + std::to_string(i - 1) + ")");
  }
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.defs = d.d;
  GTEXT_CSS_Conditions * c = nullptr;
  ASSERT_EQ(gtext_css_conditions_new(nullptr, &co, &c), GTEXT_CSS_OK);
  std::string q = "(--c" + std::to_string(n - 1) + ")";
  bool m = false;
  ASSERT_EQ(gtext_css_conditions_matches_media(c, q.data(), q.size(), nullptr, &m), GTEXT_CSS_OK);
  EXPECT_TRUE(m);
  gtext_css_conditions_free(c);
}

TEST(CssMediaApi, ArgumentsAreChecked) {
  GTEXT_CSS_Media_List * l = nullptr;
  EXPECT_EQ(gtext_css_media_list_parse("a", 1, nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_media_list_parse(nullptr, 3, nullptr, &l, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_media_list_parse(nullptr, 0, nullptr, &l, nullptr), GTEXT_CSS_OK);
  gtext_css_media_list_free(l);
  GTEXT_Markup_Sink bad{nullptr, nullptr};
  EXPECT_EQ(gtext_css_media_list_serialize(nullptr, &bad), GTEXT_CSS_E_INVALID);
  gtext_css_media_list_free(nullptr);
  gtext_css_media_result_free(nullptr);
  EXPECT_EQ(gtext_css_media_list_retain(nullptr), nullptr);
  EXPECT_EQ(gtext_css_media_feature_find("nope", 4), -1);
  EXPECT_FALSE(gtext_css_media_environment_set_supported(nullptr, "width", true));
  GTEXT_CSS_Media_Environment e = base_env();
  EXPECT_FALSE(gtext_css_media_environment_set_supported(&e, "nope", true));
  EXPECT_EQ(gtext_css_conditions_new(nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Media_Feature_Row row;
  EXPECT_FALSE(gtext_css_media_feature_row(gtext_css_media_feature_count(), &row));
  ASSERT_TRUE(gtext_css_media_feature_row(0, &row));
}

TEST(CssMediaApi, ParseFromValuesAndAQueryAlone) {
  GTEXT_CSS_Tree * tree = nullptr;
  GTEXT_CSS_Result r;
  std::memset(&r, 0, sizeof r);
  const char * text = "screen and (color), print";
  ASSERT_EQ(gtext_css_parse_component_values(text, std::strlen(text), nullptr, &tree, &r), GTEXT_CSS_OK);
  GTEXT_CSS_Media_List * l = nullptr;
  ASSERT_EQ(gtext_css_media_list_parse_values(tree->values, tree->value_count, nullptr, &l, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_media_list_query_count(l), 2u);
  gtext_css_media_list_free(l);
  /* A single query refuses a comma. */
  ASSERT_EQ(gtext_css_media_query_parse_values(tree->values, tree->value_count, nullptr, &l, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_media_list_query_count(l), 1u);
  EXPECT_TRUE(gtext_css_media_list_query_replaced(l, 0));
  gtext_css_media_list_free(l);
  gtext_css_tree_free(tree);
  gtext_css_result_free(&r);
}

TEST(CssMediaApi, TheListIsSharedByReference) {
  Parsed p("(color)");
  GTEXT_CSS_Media_List * again = gtext_css_media_list_retain(p.list);
  EXPECT_EQ(again, p.list);
  gtext_css_media_list_free(again);
  EXPECT_EQ(p.text(), "(color)");
}

TEST(CssMediaApi, SerializeBuffer) {
  Parsed p("screen and (color)");
  char * buf = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_css_media_list_serialize_buffer(p.list, nullptr, &buf, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(buf, n), "screen and (color)");
  EXPECT_EQ(buf[n], 0);
  gtext_css_media_buffer_free(nullptr, buf);
  Parsed e("");
  ASSERT_EQ(gtext_css_media_list_serialize_buffer(e.list, nullptr, &buf, &n), GTEXT_CSS_OK);
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(buf[0], 0);
  gtext_css_media_buffer_free(nullptr, buf);
}

TEST(CssMediaApi, DependenciesAreData) {
  Parsed p("(width > 1px) and (prefers-color-scheme: dark), (--x), (-vendor)");
  GTEXT_CSS_Condition_Dependencies d;
  gtext_css_media_list_dependencies(p.list, &d);
  EXPECT_TRUE(d.environment & (1u << 0)) << "viewport";
  EXPECT_TRUE(d.environment & (1u << 6)) << "scheme";
  EXPECT_FALSE(d.environment & (1u << 3)) << "color is not mentioned";
  EXPECT_TRUE(d.custom_media);
  EXPECT_TRUE(d.unknown_features);
  int w = gtext_css_media_feature_find("width", 5);
  ASSERT_GE(w, 0);
  EXPECT_TRUE(d.features[w / 64] >> (w % 64) & 1u);
}

TEST(CssMediaApi, TheIfConditionHookAnswersMedia) {
  /* The bridge to Values 5 `if(media(...))`: gtext_css_conditions_environment_hook() fills `condition`. */
  GTEXT_CSS_Media_Environment me = sized(500, 500);
  Cond c(me);
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  gtext_css_conditions_environment_hook(c.c, &env);
  ASSERT_NE(env.condition, nullptr);
  GTEXT_CSS_String t1{"(width: 500px)", 14};
  GTEXT_CSS_String t2{"(width: 501px)", 14};
  GTEXT_CSS_String t3{"(foo)", 5};
  EXPECT_EQ(env.condition(env.user, GTEXT_CSS_CONDITION_MEDIA, &t1, nullptr), GTEXT_CSS_TRI_TRUE);
  EXPECT_EQ(env.condition(env.user, GTEXT_CSS_CONDITION_MEDIA, &t2, nullptr), GTEXT_CSS_TRI_FALSE);
  EXPECT_EQ(env.condition(env.user, GTEXT_CSS_CONDITION_SUPPORTS, &t1, nullptr), GTEXT_CSS_TRI_UNKNOWN) << "until the supports half is here";
  EXPECT_EQ(gtext_css_conditions_condition(c.c, GTEXT_CSS_CONDITION_MEDIA, &t3), GTEXT_CSS_TRI_FALSE);
}
