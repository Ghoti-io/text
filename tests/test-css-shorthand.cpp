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
 * Shorthands (css_shorthand.h): the kind of each shorthand, expansion into longhands (the generic assignment and the shorthands that have an
 * algorithm of their own), the CSS-wide keywords and `var()` on a shorthand, the declaration block, and CSSOM's re-serialization of a block's
 * longhands as the shortest shorthand.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

/// The longhands a declaration block holds, as `name=value` joined by `; ` (a wide keyword as its spelling, a pending value with `(pending)`).
std::string items_of(const Block & b) {
  std::string out;
  const size_t n = gtext_css_block_count(b.b);
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Declaration_Item it;
    EXPECT_TRUE(gtext_css_block_item(b.b, i, &it));
    GTEXT_CSS_Property_Info info;
    EXPECT_TRUE(gtext_css_registry_property_info(R(), it.property, &info));
    std::string text;
    if (it.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
      text = gtext_css_wide_keyword_name(it.wide);
    }
    else if (it.set && gtext_css_value_set_root(it.set)) {
      char * buf = nullptr;
      size_t len = 0;
      EXPECT_EQ(gtext_css_serialize_value_buffer(gtext_css_value_set_root(it.set), nullptr, &buf, &len), GTEXT_CSS_OK);
      text.assign(buf, len);
      gtext_css_serialize_buffer_free(nullptr, buf);
      if (it.outcome == GTEXT_CSS_PROP_RESULT_PENDING) {
        text += " (pending)";
      }
    }
    if (!out.empty()) {
      out += "; ";
    }
    out += std::string(info.name, info.name_length) + "=" + text;
  }
  return out;
}

/// The value of one property of a block as CSSOM's getPropertyValue answers.
std::string get(const Block & b, const std::string & property) {
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_css_block_get_property_value(b.b, find(R(), property), &out, &n, nullptr);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_css_serialize_buffer_free(nullptr, out);
  return s;
}

/// What declaring `name: value` and writing the block's cssText gives.
std::string through(const std::string & declaration) {
  Block b(R(), declaration);
  if (!b.rejected.empty()) {
    return "<rejected " + b.rejected[0] + ">";
  }
  return b.css();
}

/* ---- the kinds ------------------------------------------------------------------------------------------------------------- */

TEST(CssShorthandKinds, EveryShorthandOfThePinHasARule) {
  size_t shorthands = 0;
  std::vector<std::string> missing;
  const size_t count = gtext_css_registry_property_count(R());
  for (size_t i = 1; i <= count; i++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(R(), static_cast<GTEXT_CSS_Property_Id>(i), &info) || !(info.flags & GTEXT_CSS_PROP_SHORTHAND)) {
      continue;
    }
    shorthands++;
    const GTEXT_CSS_Shorthand_Kind kind = gtext_css_shorthand_kind(R(), info.id);
    EXPECT_STRNE(gtext_css_shorthand_kind_name(kind), "unknown") << std::string(info.name, info.name_length);
    if (kind == GTEXT_CSS_SHORTHAND_UNIMPLEMENTED) {
      missing.emplace_back(info.name, info.name_length);
    }
  }
  EXPECT_GT(shorthands, 150u);
  EXPECT_TRUE(missing.empty()) << "a shorthand with no rule is GTEXT_CSS_E_UNSUPPORTED when it is used: " << (missing.empty() ? "" : missing[0]);
}

TEST(CssShorthandKinds, TheKindsAreWhatTheTableSays) {
  struct Row {
    const char * name;
    GTEXT_CSS_Shorthand_Kind kind;
  };
  const Row rows[] = {
      {"margin", GTEXT_CSS_SHORTHAND_BOX},
      {"border-color", GTEXT_CSS_SHORTHAND_BOX},
      {"gap", GTEXT_CSS_SHORTHAND_PAIR},
      {"border", GTEXT_CSS_SHORTHAND_ANY},
      {"background", GTEXT_CSS_SHORTHAND_LAYERS},
      {"border-radius", GTEXT_CSS_SHORTHAND_BORDER_RADIUS},
      {"font", GTEXT_CSS_SHORTHAND_FONT},
      {"flex", GTEXT_CSS_SHORTHAND_FLEX},
      {"all", GTEXT_CSS_SHORTHAND_ALL},
      {"grid-area", GTEXT_CSS_SHORTHAND_GRID_LINE},
      {"grid", GTEXT_CSS_SHORTHAND_GRID},
      {"grid-template", GTEXT_CSS_SHORTHAND_GRID_TEMPLATE},
      {"offset", GTEXT_CSS_SHORTHAND_OFFSET},
      {"text-align", GTEXT_CSS_SHORTHAND_TEXT_ALIGN},
      {"font-synthesis", GTEXT_CSS_SHORTHAND_FONT_SYNTHESIS},
  };
  for (const Row & r : rows) {
    EXPECT_EQ(gtext_css_shorthand_kind(R(), find(R(), r.name)), r.kind) << r.name;
  }
  EXPECT_EQ(gtext_css_shorthand_kind(R(), find(R(), "width")), GTEXT_CSS_SHORTHAND_UNIMPLEMENTED) << "a longhand is no shorthand";
  EXPECT_EQ(gtext_css_shorthand_kind(R(), 0), GTEXT_CSS_SHORTHAND_UNIMPLEMENTED);
  EXPECT_EQ(gtext_css_shorthand_kind(nullptr, find(R(), "margin")), GTEXT_CSS_SHORTHAND_UNIMPLEMENTED);
  EXPECT_STREQ(gtext_css_shorthand_kind_name(GTEXT_CSS_SHORTHAND_BOX), "box");
  EXPECT_STREQ(gtext_css_shorthand_kind_name(GTEXT_CSS_SHORTHAND_GRID_TEMPLATE), "grid-template");
  EXPECT_STREQ(gtext_css_shorthand_kind_name(static_cast<GTEXT_CSS_Shorthand_Kind>(999)), "unknown");
  for (int k = 0; k < GTEXT_CSS_SHORTHAND_KIND_COUNT; k++) {
    EXPECT_STRNE(gtext_css_shorthand_kind_name(static_cast<GTEXT_CSS_Shorthand_Kind>(k)), "unknown") << "kind " << k << " has a name";
  }
}

/* ---- expansion ------------------------------------------------------------------------------------------------------------- */

TEST(CssShorthandExpand, OneToFourValuesAreDistributedToTheSides) {
  EXPECT_EQ(items_of(Block(R(), "margin: 1px")), "margin-top=1px; margin-right=1px; margin-bottom=1px; margin-left=1px");
  EXPECT_EQ(items_of(Block(R(), "margin: 1px 2px")), "margin-top=1px; margin-right=2px; margin-bottom=1px; margin-left=2px");
  EXPECT_EQ(items_of(Block(R(), "margin: 1px 2px 3px")), "margin-top=1px; margin-right=2px; margin-bottom=3px; margin-left=2px");
  EXPECT_EQ(items_of(Block(R(), "margin: 1px 2px 3px 4px")), "margin-top=1px; margin-right=2px; margin-bottom=3px; margin-left=4px");
  EXPECT_EQ(items_of(Block(R(), "border-color: red blue")), "border-top-color=red; border-right-color=blue; border-bottom-color=red; border-left-color=blue");
}

TEST(CssShorthandExpand, ComponentsInAnyOrderGoToTheLonghandThatAcceptsThem) {
  const std::string a = items_of(Block(R(), "border: 2px solid red"));
  EXPECT_NE(a.find("border-top-width=2px"), std::string::npos) << a;
  EXPECT_NE(a.find("border-top-style=solid"), std::string::npos) << a;
  EXPECT_NE(a.find("border-top-color=red"), std::string::npos) << a;
  EXPECT_EQ(items_of(Block(R(), "border: red 2px solid")), a) << "the order of the components does not matter";
  EXPECT_EQ(items_of(Block(R(), "border: solid red 2px")), a);
  // what is left out is reset to its initial value
  const std::string b = items_of(Block(R(), "border: solid"));
  EXPECT_NE(b.find("border-top-width=medium"), std::string::npos) << b;
  EXPECT_NE(b.find("border-top-color=currentcolor"), std::string::npos) << b;
  // the shorthand resets what it names (border resets border-image)
  EXPECT_NE(b.find("border-image-source=none"), std::string::npos) << b;
}

TEST(CssShorthandExpand, AValueNoLonghandAcceptsIsNotAValueOfTheShorthand) {
  Block b(R(), "border: 2px solid red blue");
  EXPECT_EQ(b.rejected.size(), 1u);
  Block c(R(), "margin: 1px 2px 3px 4px 5px");
  EXPECT_EQ(c.rejected.size(), 1u);
  EXPECT_EQ(gtext_css_block_count(c.b), 0u) << "all or nothing";
}

TEST(CssShorthandExpand, LayersAreCommaListsOfTheComponents) {
  EXPECT_EQ(items_of(Block(R(), "background: url(a) repeat-x, url(b) no-repeat red")),
      "background-image=url(\"a\"), url(\"b\"); background-size=auto, auto; background-repeat=repeat-x, no-repeat; background-attachment=scroll, scroll; "
      "background-origin=padding-box, padding-box; background-clip=border-box, border-box; background-color=red; background-blend-mode=normal; "
      "background-position-x=0%, 0%; background-position-y=0%, 0%");
  // the color belongs to the last layer alone
  EXPECT_EQ(Block(R(), "background: red, url(a)").rejected.size(), 1u);
}

TEST(CssShorthandExpand, ASlashSeparatesTheSizeFromThePositionAndTheOutsetFromTheWidth) {
  const std::string a = items_of(Block(R(), "background: 1px 2px / 3px 4px"));
  EXPECT_NE(a.find("background-size=3px 4px"), std::string::npos) << a;
  EXPECT_NE(a.find("background-position-x=1px"), std::string::npos) << a;
  EXPECT_NE(a.find("background-position-y=2px"), std::string::npos) << a;
  const std::string b = items_of(Block(R(), "border-image: 1 / / 1px"));
  EXPECT_NE(b.find("border-image-slice=1;"), std::string::npos) << b;
  EXPECT_NE(b.find("border-image-width=1;"), std::string::npos) << "an empty part leaves the width at its initial value: " << b;
  EXPECT_NE(b.find("border-image-outset=1px"), std::string::npos) << b;
}

TEST(CssShorthandExpand, OnePositionValueIsTheHorizontalAndTheVerticalIsCenter) {
  EXPECT_EQ(items_of(Block(R(), "background-position: 1px")), "background-position-x=1px; background-position-y=center");
  EXPECT_EQ(items_of(Block(R(), "background-position: bottom")), "background-position-x=center; background-position-y=bottom");
  EXPECT_EQ(items_of(Block(R(), "background-position: right 9%")), "background-position-x=right; background-position-y=9%");
  EXPECT_EQ(items_of(Block(R(), "background-position: bottom left")), "background-position-x=left; background-position-y=bottom");
}

TEST(CssShorthandExpand, OneBoxSetsTheOriginAndTheClip) {
  const std::string a = items_of(Block(R(), "background: content-box"));
  EXPECT_NE(a.find("background-origin=content-box; background-clip=content-box"), std::string::npos) << a;
  const std::string b = items_of(Block(R(), "background: padding-box content-box"));
  EXPECT_NE(b.find("background-origin=padding-box; background-clip=content-box"), std::string::npos) << b;
  const std::string m = items_of(Block(R(), "mask: no-clip"));
  EXPECT_NE(m.find("mask-origin=border-box"), std::string::npos) << m;
  EXPECT_NE(m.find("mask-clip=no-clip"), std::string::npos) << m;
}

TEST(CssShorthandExpand, ALayerListReadsTheCommasOfALonghandThatIsAListItself) {
  // `animation-range` is `[ <'animation-range-start'> <'animation-range-end'>? ]#` and its start is a list: the commas still separate layers
  EXPECT_EQ(items_of(Block(R(), "animation-range: entry, exit")), "animation-range-start=entry, exit; animation-range-end=entry, exit");
  EXPECT_EQ(items_of(Block(R(), "animation-range: cover 10% exit")), "animation-range-start=cover 10%; animation-range-end=exit");
  EXPECT_EQ(items_of(Block(R(), "animation-delay: 20s, 10s")), "animation-delay-start=20s, 10s; animation-delay-end=0s, 0s");
}

TEST(CssShorthandExpand, BorderRadiusTakesOneToFourRadiiAndASlash) {
  EXPECT_EQ(items_of(Block(R(), "border-radius: 1px")),
      "border-top-left-radius=1px; border-top-right-radius=1px; border-bottom-right-radius=1px; border-bottom-left-radius=1px");
  EXPECT_EQ(items_of(Block(R(), "border-radius: 1px 2px / 3px")),
      "border-top-left-radius=1px 3px; border-top-right-radius=2px 3px; border-bottom-right-radius=1px 3px; border-bottom-left-radius=2px 3px");
}

TEST(CssShorthandExpand, FlexOneValueIsTheGrowFactorOrTheBasis) {
  EXPECT_EQ(items_of(Block(R(), "flex: 2")), "flex-grow=2; flex-shrink=1; flex-basis=0%");
  EXPECT_EQ(items_of(Block(R(), "flex: 10px")), "flex-grow=1; flex-shrink=1; flex-basis=10px");
  EXPECT_EQ(items_of(Block(R(), "flex: auto")), "flex-grow=1; flex-shrink=1; flex-basis=auto");
  EXPECT_EQ(items_of(Block(R(), "flex: none")), "flex-grow=0; flex-shrink=0; flex-basis=auto");
  EXPECT_EQ(items_of(Block(R(), "flex: 0 0 0")), "flex-grow=0; flex-shrink=0; flex-basis=0px");
  EXPECT_EQ(items_of(Block(R(), "flex: calc(1) calc(2 + 1) calc(3px)")), "flex-grow=calc(1); flex-shrink=calc(3); flex-basis=calc(3px)")
      << "a math function that is a number is a flex factor, one that is a length is the basis";
}

TEST(CssShorthandExpand, FontSetsEveryPropertyItNamesAndResetsTheRest) {
  const std::string a = items_of(Block(R(), "font: italic bold 12px/1.5 serif"));
  EXPECT_NE(a.find("font-style=italic"), std::string::npos) << a;
  EXPECT_NE(a.find("font-weight=bold"), std::string::npos) << a;
  EXPECT_NE(a.find("font-size=12px"), std::string::npos) << a;
  EXPECT_NE(a.find("line-height=1.5"), std::string::npos) << a;
  EXPECT_NE(a.find("font-family=serif"), std::string::npos) << a;
  EXPECT_NE(a.find("font-variant-caps=normal"), std::string::npos) << "font resets font-variant: " << a;
  EXPECT_NE(a.find("font-width=normal"), std::string::npos) << "the pin names font-stretch font-width: " << a;
  // a system font needs the environment's font table
  Block s(R(), "font: caption");
  EXPECT_EQ(s.rejected.size(), 1u);
  EXPECT_NE(s.rejected[0].find("not supported"), std::string::npos) << s.rejected[0];
}

TEST(CssShorthandExpand, PlaceContentKeepsABaselineOutOfTheJustifyingAxis) {
  EXPECT_EQ(items_of(Block(R(), "place-content: center")), "align-content=center; justify-content=center");
  EXPECT_EQ(items_of(Block(R(), "place-content: start end")), "align-content=start; justify-content=end");
  const std::string b = items_of(Block(R(), "place-content: baseline"));
  EXPECT_NE(b.find("justify-content=start"), std::string::npos) << "justify-content has no baseline: " << b;
}

TEST(CssShorthandExpand, GridLinesCopyAnIdentifierAndOtherwiseAreAuto) {
  EXPECT_EQ(items_of(Block(R(), "grid-column: a / b")), "grid-column-start=a; grid-column-end=b");
  EXPECT_EQ(items_of(Block(R(), "grid-column: a")), "grid-column-start=a; grid-column-end=a");
  EXPECT_EQ(items_of(Block(R(), "grid-column: 3")), "grid-column-start=3; grid-column-end=auto");
  EXPECT_EQ(items_of(Block(R(), "grid-column: -10")), "grid-column-start=-10; grid-column-end=auto") << "-10 is not an identifier";
  EXPECT_EQ(items_of(Block(R(), "grid-area: a / b / c / d")),
      "grid-row-start=a; grid-column-start=b; grid-row-end=c; grid-column-end=d");
}

TEST(CssShorthandExpand, GridTemplateReadsRowsAndColumnsOrAreasWithTheirRowSizes) {
  EXPECT_EQ(items_of(Block(R(), "grid-template: none")), "grid-template-rows=none; grid-template-columns=none; grid-template-areas=none");
  EXPECT_EQ(items_of(Block(R(), "grid-template: 10px / 20%")), "grid-template-rows=10px; grid-template-columns=20%; grid-template-areas=none");
  EXPECT_EQ(items_of(Block(R(), "grid-template: [header-top] \"a a a\" [header-bottom] [main-top] \"b b b\" 1fr [main-bottom] / auto 1fr auto")),
      "grid-template-rows=[header-top] auto [header-bottom main-top] 1fr [main-bottom]; grid-template-columns=auto 1fr auto; "
      "grid-template-areas=\"a a a\" \"b b b\"");
  EXPECT_EQ(items_of(Block(R(), "grid-template: \"a\" [a] [b] \"b\"")),
      "grid-template-rows=auto [a b] auto; grid-template-columns=none; grid-template-areas=\"a\" \"b\"")
      << "line names between two rows are one list";
}

TEST(CssShorthandExpand, GridResetsTheImplicitGridOrSetsTheAutoFlowSide) {
  const std::string a = items_of(Block(R(), "grid: 1fr / 2fr"));
  EXPECT_NE(a.find("grid-template-rows=1fr; grid-template-columns=2fr"), std::string::npos) << a;
  EXPECT_NE(a.find("grid-auto-flow=row"), std::string::npos) << a;
  const std::string b = items_of(Block(R(), "grid: auto-flow dense 30px / 40px"));
  EXPECT_NE(b.find("grid-template-rows=none; grid-template-columns=40px"), std::string::npos) << b;
  EXPECT_NE(b.find("grid-auto-rows=30px"), std::string::npos) << b;
  EXPECT_NE(b.find("grid-auto-flow=row dense"), std::string::npos) << b;
  const std::string c = items_of(Block(R(), "grid: 100px / dense auto-flow 20px"));
  EXPECT_NE(c.find("grid-template-rows=100px; grid-template-columns=none"), std::string::npos) << c;
  EXPECT_NE(c.find("grid-auto-columns=20px"), std::string::npos) << c;
  EXPECT_NE(c.find("grid-auto-flow=column dense"), std::string::npos) << c;
}

TEST(CssShorthandExpand, OffsetPutsThePositionBeforeThePathAndTheDistanceAfterIt) {
  const std::string a = items_of(Block(R(), "offset: 100px"));
  EXPECT_NE(a.find("offset-position=100px"), std::string::npos) << "a length alone is the position: " << a;
  EXPECT_NE(a.find("offset-distance=0px"), std::string::npos) << a;
  const std::string b = items_of(Block(R(), "offset: none 100px reverse"));
  EXPECT_NE(b.find("offset-distance=100px"), std::string::npos) << "a length after the path is the distance: " << b;
  EXPECT_NE(b.find("offset-rotate=reverse"), std::string::npos) << b;
  EXPECT_NE(b.find("offset-position=normal"), std::string::npos) << b;
  const std::string c = items_of(Block(R(), "offset: left top path(\"M 0 0 H 1\") 5px auto 30deg / right bottom"));
  EXPECT_NE(c.find("offset-position=left top"), std::string::npos) << c;
  EXPECT_NE(c.find("offset-anchor=right bottom"), std::string::npos) << c;
  EXPECT_NE(c.find("offset-rotate=auto 30deg"), std::string::npos) << c;
}

TEST(CssShorthandExpand, WhiteSpaceKeywordsStandForTwoLonghands) {
  EXPECT_EQ(items_of(Block(R(), "white-space: pre")), "white-space-collapse=preserve; text-wrap-mode=nowrap");
  EXPECT_EQ(items_of(Block(R(), "white-space: pre-line")), "white-space-collapse=preserve-breaks; text-wrap-mode=wrap");
  EXPECT_EQ(items_of(Block(R(), "white-space: normal")), "white-space-collapse=collapse; text-wrap-mode=wrap");
  EXPECT_EQ(items_of(Block(R(), "white-space: nowrap")), "white-space-collapse=collapse; text-wrap-mode=nowrap");
  EXPECT_EQ(items_of(Block(R(), "text-spacing: none")), "text-spacing-trim=space-all; text-autospace=no-autospace");
}

TEST(CssShorthandExpand, RuleInsetsAreACapAndAJunctionOfAStartAndAnEnd) {
  EXPECT_EQ(items_of(Block(R(), "column-rule-inset: 1px")),
      "column-rule-inset-cap-start=1px; column-rule-inset-cap-end=1px; column-rule-inset-junction-start=1px; column-rule-inset-junction-end=1px");
  EXPECT_EQ(items_of(Block(R(), "row-rule-inset: 1px 2px / 3px")),
      "row-rule-inset-cap-start=1px; row-rule-inset-cap-end=2px; row-rule-inset-junction-start=3px; row-rule-inset-junction-end=3px");
  EXPECT_EQ(items_of(Block(R(), "column-rule-inset-cap: 1px 2px")), "column-rule-inset-cap-start=1px; column-rule-inset-cap-end=2px");
  EXPECT_EQ(items_of(Block(R(), "row-rule-inset-start: 3px")), "row-rule-inset-cap-start=3px; row-rule-inset-junction-start=3px");
}

TEST(CssShorthandExpand, TextDecorationSkipNoneSkipsNothingAndAutoIsTheDefault) {
  EXPECT_EQ(items_of(Block(R(), "text-decoration-skip: none")),
      "text-decoration-skip-self=no-skip; text-decoration-skip-box=none; text-decoration-skip-spaces=none; text-decoration-skip-ink=none");
  EXPECT_EQ(items_of(Block(R(), "text-decoration-skip: auto")),
      "text-decoration-skip-self=auto; text-decoration-skip-box=none; text-decoration-skip-spaces=start end; text-decoration-skip-ink=auto");
}

TEST(CssShorthandExpand, ListStyleNoneNamesTheTypeAndTheImageBothUnlessATypeIsGiven) {
  const std::string a = items_of(Block(R(), "list-style: none"));
  EXPECT_NE(a.find("list-style-type=none"), std::string::npos) << a;
  EXPECT_NE(a.find("list-style-image=none"), std::string::npos) << a;
  const std::string b = items_of(Block(R(), "list-style: none square"));
  EXPECT_NE(b.find("list-style-type=square"), std::string::npos) << a;
  EXPECT_NE(b.find("list-style-image=none"), std::string::npos) << b;
}

TEST(CssShorthandExpand, ABareZeroIsANumberWhereTheGrammarTakesBothANumberAndALength) {
  EXPECT_EQ(items_of(Block(R(), "border-image-outset: 0")), "border-image-outset=0");
  EXPECT_EQ(items_of(Block(R(), "border-image-outset: 0px")), "border-image-outset=0px");
  EXPECT_EQ(items_of(Block(R(), "line-height: 0")), "line-height=0");
  EXPECT_EQ(items_of(Block(R(), "margin: 0")), "margin-top=0px; margin-right=0px; margin-bottom=0px; margin-left=0px") << "margin takes no number";
}

TEST(CssShorthandExpand, ATryFallbackKeepsTheOrderItsTacticsAreWrittenIn) {
  EXPECT_EQ(items_of(Block(R(), "position-try: most-width flip-y flip-x, --foo")),
      "position-try-order=most-width; position-try-fallbacks=flip-y flip-x, --foo");
  EXPECT_EQ(items_of(Block(R(), "position-try: --foo, --bar")), "position-try-order=normal; position-try-fallbacks=--foo, --bar");
}

TEST(CssShorthandExpand, LegacyLineClampIsAMaxLinesWithAnEllipsis) {
  const std::string a = items_of(Block(R(), "-webkit-line-clamp: 3"));
  EXPECT_NE(a.find("max-lines=3"), std::string::npos) << a;
  EXPECT_NE(a.find("block-ellipsis=ellipsis"), std::string::npos) << a;
  EXPECT_NE(a.find("continue=-webkit-legacy"), std::string::npos) << a;
  const std::string b = items_of(Block(R(), "-webkit-line-clamp: none"));
  EXPECT_NE(b.find("max-lines=auto"), std::string::npos) << b;
}

/* ---- CSS-wide keywords, var() and all ---------------------------------------------------------------------------------------- */

TEST(CssShorthandWide, AWideKeywordIsEveryLonghandsAndWritesBackAsTheShorthand) {
  EXPECT_EQ(items_of(Block(R(), "margin: inherit")), "margin-top=inherit; margin-right=inherit; margin-bottom=inherit; margin-left=inherit");
  EXPECT_EQ(through("margin: inherit"), "margin: inherit;");
  EXPECT_EQ(through("border: initial"), "border: initial;");
  EXPECT_EQ(through("background: unset"), "background: unset;");
}

TEST(CssShorthandWide, AShorthandWithVarSetsEveryLonghandPendingAndHasNoValueUntilItIsSubstituted) {
  Block b(R(), "margin: var(--x)");
  const std::string items = items_of(b);
  EXPECT_NE(items.find("margin-top=var(--x) (pending)"), std::string::npos) << items;
  EXPECT_NE(items.find("margin-left=var(--x) (pending)"), std::string::npos) << items;
  EXPECT_EQ(get(b, "margin-top"), "") << "a pending longhand has no value to read as a specified value";
  EXPECT_EQ(b.css(), "margin: var(--x);") << "the declaration is written as it was";
  Block c(R(), "margin: var(--x); margin-top: 1px");
  EXPECT_EQ(get(c, "margin"), "") << "one longhand pending: the shorthand cannot be written as a value";
}

TEST(CssShorthandWide, AllTakesTheWideKeywordsAlone) {
  EXPECT_EQ(through("all: initial"), "all: initial;");
  EXPECT_EQ(through("all: inherit"), "all: inherit;");
  Block b(R(), "all: red");
  EXPECT_EQ(b.rejected.size(), 1u);
  Block c(R(), "all: revert");
  EXPECT_EQ(gtext_css_block_count(c.b), 1u) << "revert is kept as written for the cascade to resolve";
}

/* ---- the block --------------------------------------------------------------------------------------------------------------- */

TEST(CssShorthandBlock, TheLastDeclarationOfAPropertyCountsAndKeepsItsPlace) {
  Block b(R(), "margin: 1px; margin-top: 5px");
  EXPECT_EQ(b.css(), "margin: 5px 1px 1px;");
  EXPECT_EQ(get(b, "margin-top"), "5px");
  EXPECT_EQ(get(b, "margin"), "5px 1px 1px");
  Block c(R(), "margin-top: 5px; margin: 1px");
  EXPECT_EQ(c.css(), "margin: 1px;") << "the later shorthand wins every side";
}

TEST(CssShorthandBlock, ADeclarationThatDoesNotParseAddsNothing) {
  Block b(R(), "margin: 1px; width: banana; color: red");
  EXPECT_EQ(b.rejected.size(), 1u);
  EXPECT_EQ(b.css(), "margin: 1px; color: red;");
}

TEST(CssShorthandBlock, ACustomPropertyKeepsItsPlaceAndItsCase) {
  Block b(R());
  GTEXT_CSS_Property_Value pv;
  const char * text = " A b ";
  ASSERT_EQ(gtext_css_parse_property_text(R(), "--Foo", 5, text, std::strlen(text), nullptr, &pv), GTEXT_CSS_OK);
  ASSERT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_VALID);
  ASSERT_EQ(gtext_css_block_add_custom(b.b, "--Foo", 5, &pv, false), GTEXT_CSS_OK);
  gtext_css_property_value_free(&pv);
  GTEXT_CSS_Property_Outcome outcome;
  ASSERT_EQ(gtext_css_block_add_text(b.b, "color", 5, "red", 3, false, nullptr, &outcome, nullptr), GTEXT_CSS_OK);
  const std::string css = b.css();
  EXPECT_EQ(css.find("--Foo:"), 0u) << css;
  EXPECT_NE(css.find("color: red;"), std::string::npos) << css;
}

TEST(CssShorthandBlock, AllTheLonghandsWithTheSameImportanceMakeTheShorthand) {
  EXPECT_EQ(through("margin-top: 1px; margin-right: 1px; margin-bottom: 1px; margin-left: 1px"), "margin: 1px;");
  EXPECT_EQ(through("margin-top: 1px; margin-right: 2px; margin-bottom: 3px; margin-left: 2px"), "margin: 1px 2px 3px;");
  EXPECT_EQ(through("margin-top: 1px; margin-right: 1px; margin-bottom: 1px; margin-left: 1px !important"),
      "margin-top: 1px; margin-right: 1px; margin-bottom: 1px; margin-left: 1px !important;")
      << "one of them important: no shorthand";
  EXPECT_EQ(through("margin-top: 1px; margin-right: 1px; margin-bottom: 1px"), "margin-top: 1px; margin-right: 1px; margin-bottom: 1px;")
      << "one missing: no shorthand";
  EXPECT_EQ(through("margin: 1px !important"), "margin: 1px !important;");
}

TEST(CssShorthandBlock, TheShorthandLeavesOutWhatIsInitialAndKeepsTheCanonicalOrder) {
  EXPECT_EQ(through("border: 1px solid red"), "border: 1px solid red;");
  EXPECT_EQ(through("border: red solid 1px"), "border: 1px solid red;");
  EXPECT_EQ(through("border: medium none currentcolor"), "border: medium;") << "all initial: the first component stands for them";
  EXPECT_EQ(through("outline: none"), "outline: none;");
  EXPECT_EQ(through("outline: auto red"), "outline: red auto;");
  EXPECT_EQ(through("list-style: inside none"), "list-style: inside none;");
  EXPECT_EQ(through("list-style: outside outside"), "list-style: outside outside;") << "a type named like a position keyword needs the position";
  EXPECT_EQ(through("vertical-align: baseline"), "vertical-align: baseline;");
}

TEST(CssShorthandBlock, ASlashIsWrittenWhereTheGrammarNeedsItAndNotBefore) {
  EXPECT_EQ(through("columns: 10px / 100px"), "columns: 10px / 100px;");
  EXPECT_EQ(through("columns: 3 / 100px"), "columns: 3 / 100px;");
  EXPECT_EQ(through("columns: 10px 3"), "columns: 10px 3;");
  EXPECT_EQ(through("border-image: 1 / / 1px"), "border-image: 1 / / 1px;");
  EXPECT_EQ(through("border-image: none 1 1 1 1"), "border-image: 1;");
  EXPECT_EQ(through("container: a / size"), "container: a / size;");
  EXPECT_EQ(through("container: size"), "container: size;") << "a lone ident before the slash is the name";
  EXPECT_EQ(through("background: 1px 2px / 3px 4px"), "background: 1px 2px / 3px 4px;");
}

TEST(CssShorthandBlock, OneBoxIsWrittenOnceAndTwoAreWrittenWhenTheyDiffer) {
  EXPECT_EQ(through("background: padding-box"), "background: padding-box;");
  EXPECT_EQ(through("background: content-box content-box"), "background: content-box;");
  EXPECT_EQ(through("background: border-box content-box"), "background: border-box content-box;");
  EXPECT_EQ(through("mask: border-box content-box"), "mask: border-box content-box;");
  EXPECT_EQ(through("mask: no-clip"), "mask: no-clip;");
  EXPECT_EQ(through("mask: content-box content-box"), "mask: content-box;");
}

TEST(CssShorthandBlock, ALayerListWritesEachLayerAndKeepsTheColorForTheLast) {
  EXPECT_EQ(through("background: url(a) 1px 2px / 3px 4px, rgb(5, 6, 7) url(b)"), "background: url(\"a\") 1px 2px / 3px 4px, url(\"b\") rgb(5, 6, 7);");
  EXPECT_EQ(through("background: url(a), red"), "background: url(\"a\"), red;");
  EXPECT_EQ(through("animation: anim paused both reverse, 4 1s -3s cubic-bezier(0, -2, 1, 3)"),
      "animation: reverse both paused anim, 1s cubic-bezier(0, -2, 1, 3) -3s 4;")
      << "the name last, and the timeline (one value for all the layers) not written";
  EXPECT_EQ(through("animation-delay: 20s, 10s"), "animation-delay: 20s, 10s;");
  EXPECT_EQ(through("animation-range: entry 0% entry 100%"), "animation-range: entry;");
  EXPECT_EQ(through("animation-range: entry 0% exit 0%"), "animation-range: entry exit 0%;");
  EXPECT_EQ(through("animation-range: normal exit 50%"), "animation-range: normal exit 50%;");
  EXPECT_EQ(through("animation-range: entry normal"), "animation-range: entry normal;");
  EXPECT_EQ(through("animation-range: cover 50%"), "animation-range: cover 50%;");
  EXPECT_EQ(through("background-position: 1px"), "background-position: 1px center;");
}

TEST(CssShorthandBlock, TheGridShorthandsWriteTheShortestFormTheirLonghandsAllow) {
  EXPECT_EQ(through("grid-template: none"), "grid-template: none;");
  EXPECT_EQ(through("grid-template: none / none"), "grid-template: none;");
  EXPECT_EQ(through("grid-template: auto / auto"), "grid-template: auto / auto;");
  EXPECT_EQ(through("grid-template: \"a\" auto"), "grid-template: \"a\";");
  EXPECT_EQ(through("grid-template: [] \"a\" 10px []"), "grid-template: \"a\" 10px;");
  EXPECT_EQ(through("grid-template: \"a\" [a] [b] \"b\""), "grid-template: \"a\" [a b] \"b\";");
  EXPECT_EQ(through("grid-template: \"a\" / 20%"), "grid-template: \"a\" / 20%;");
  EXPECT_EQ(through("grid: 1fr / 1fr"), "grid: 1fr / 1fr;");
  EXPECT_EQ(through("grid: auto-flow dense 1fr / 100px"), "grid: auto-flow dense 1fr / 100px;");
  EXPECT_EQ(through("grid: 100px / dense auto-flow 100px"), "grid: 100px / auto-flow dense 100px;");
  EXPECT_EQ(through("grid: none / auto-flow 100px"), "grid: none / auto-flow 100px;");
  EXPECT_EQ(through("grid-area: span 1 i"), "grid-area: span i;");
}

TEST(CssShorthandBlock, TheGridShorthandIsEmptyWhenTheAreasAndTheRowsDoNotMatchUp) {
  // two rows' worth of areas and three row tracks cannot be written as the areas form
  Block b(R(), "grid-template-areas: \"a\" \"b\"; grid-template-rows: 1px 2px 3px; grid-template-columns: none");
  EXPECT_EQ(get(b, "grid-template"), "");
  Block c(R(), "grid-template-areas: \"a\"; grid-template-rows: repeat(2, 1px); grid-template-columns: none");
  EXPECT_EQ(get(c, "grid-template"), "") << "repeat() cannot be written beside area names";
}

TEST(CssShorthandBlock, OffsetWritesThePathOnlyWhenSomethingFollowsIt) {
  EXPECT_EQ(through("offset: 100px"), "offset: 100px center;");
  EXPECT_EQ(through("offset: normal none reverse"), "offset: none reverse;");
  EXPECT_EQ(through("offset: auto"), "offset: auto;");
  EXPECT_EQ(through("offset: path(\"M 0 0 H 1\") auto 0deg"), "offset: path(\"M 0 0 H 1\");");
  EXPECT_EQ(through("offset: right bottom / left top"), "offset: right bottom / left top;");
  EXPECT_EQ(through("offset: top right / top left"), "offset: right top / left top;");
}

TEST(CssShorthandBlock, TheKeywordShorthandsComeBackAsTheirKeywords) {
  EXPECT_EQ(through("white-space: pre-wrap"), "white-space: pre-wrap;");
  EXPECT_EQ(through("white-space: nowrap"), "white-space: nowrap;");
  EXPECT_EQ(through("text-spacing: no-autospace space-all"), "text-spacing: none;");
  EXPECT_EQ(through("text-spacing: no-autospace trim-start"), "text-spacing: trim-start no-autospace;");
  EXPECT_EQ(through("-webkit-line-clamp: 3"), "-webkit-line-clamp: 3;");
  EXPECT_EQ(through("-webkit-line-clamp: none"), "-webkit-line-clamp: none;");
  EXPECT_EQ(through("place-content: first baseline"), "place-content: baseline start;");
  EXPECT_EQ(through("contain-intrinsic-size: auto 1px auto 1px"), "contain-intrinsic-size: auto 1px;");
  EXPECT_EQ(through("contain-intrinsic-size: none 1px"), "contain-intrinsic-size: none 1px;");
  EXPECT_EQ(through("position-try: most-width none"), "position-try: most-width none;");
  EXPECT_EQ(through("position-try: normal none"), "position-try: none;");
  EXPECT_EQ(through("font-synthesis: weight style"), "font-synthesis: weight style;");
  EXPECT_EQ(through("text-align: justify-all"), "text-align: justify-all;");
  EXPECT_EQ(through("column-rule-inset: 1px 2px / 1px 2px"), "column-rule-inset: 1px 2px;");
  EXPECT_EQ(through("row-rule-inset: 1px 2px / 3px"), "row-rule-inset: 1px 2px / 3px;");
  EXPECT_EQ(through("text-decoration-skip: none"), "text-decoration-skip: none;");
  EXPECT_EQ(through("text-decoration-skip: auto"), "text-decoration-skip: auto;");
}

TEST(CssShorthandBlock, ARangeOfAnOverflowMarginIsItsBoxAndItsLengthMinusTheDefaults) {
  EXPECT_EQ(through("overflow-clip-margin: padding-box"), "overflow-clip-margin: 0px;");
  EXPECT_EQ(through("overflow-clip-margin: padding-box 10px"), "overflow-clip-margin: 10px;");
  EXPECT_EQ(through("overflow-clip-margin: 0px content-box"), "overflow-clip-margin: content-box;");
  EXPECT_EQ(through("overflow-clip-margin: border-box 0px"), "overflow-clip-margin: border-box;");
}

TEST(CssShorthandBlock, WritingTheCssTextAndReadingItBackIsTheSameBlock) {
  const char * declarations[] = {
      "margin: 1px 2px; padding: 3px; border: 1px solid red",
      "font: italic bold 12px/1.5 serif; color: blue",
      "background: url(a) 1px 2px / 3px 4px no-repeat, red",
      "grid-template: [a] \"x\" 1fr [b] / auto; offset: 5px",
      "animation: anim 2s ease-in 1s 3 alternate both paused",
      "flex: 1 1 0%; border-radius: 1px 2px / 3px; list-style: square inside",
      "mask: no-clip; white-space: pre; text-spacing: auto",
      "columns: 2 100px; container: main / inline-size; overflow-clip-margin: content-box 3px",
  };
  for (const char * d : declarations) {
    const std::string once = through(d);
    const std::string twice = through(once);
    EXPECT_EQ(once, twice) << d;
    EXPECT_EQ(once.find('<'), std::string::npos) << d << " -> " << once;
  }
}

/* ---- shorthands a caller registers --------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status pair_hook(void * user, const GTEXT_CSS_Value * value, const GTEXT_CSS_Longhand_Sink * sink) {
  int * calls = static_cast<int *>(user);
  (*calls)++;
  // `x-pair: <length>{1,2}`: the second is the first when it is left out
  const GTEXT_CSS_Value * first = value;
  const GTEXT_CSS_Value * second = value;
  if (value->kind == GTEXT_CSS_VAL_LIST && value->child_count >= 1) {
    first = &value->children[0];
    second = &value->children[value->child_count > 1 ? 1 : 0];
  }
  GTEXT_CSS_Status st = sink->emit(sink->user, 0, first);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  return sink->emit(sink->user, 1, second);
}

TEST(CssShorthandHook, ARegisteredShorthandIsSplitByItsHookAndWritesBackThroughTheGenericRule) {
  reg::Builder builder;
  GTEXT_CSS_Property_Def a{};
  a.name = "x-a";
  a.initial = "0px";
  a.grammar = "<length>";
  GTEXT_CSS_Property_Def b = a;
  b.name = "x-b";
  ASSERT_EQ(gtext_css_registry_register_property(builder.b, &a, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_registry_register_property(builder.b, &b, nullptr), GTEXT_CSS_OK);
  int calls = 0;
  const char * longhands[] = {"x-a", "x-b"};
  GTEXT_CSS_Shorthand_Def def{};
  def.name = "x-pair";
  def.grammar = "<length>{1,2}";
  def.longhands = longhands;
  def.longhand_count = 2;
  def.hook = pair_hook;
  def.hook_user = &calls;
  ASSERT_EQ(gtext_css_registry_register_shorthand(builder.b, &def, nullptr), GTEXT_CSS_OK);
  reg::Owned owned;
  ASSERT_EQ(builder.freeze(owned), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_shorthand_kind(owned.r, find(owned.r, "x-pair")), GTEXT_CSS_SHORTHAND_HOOK);
  Block block(owned.r, "x-pair: 1px 2px");
  EXPECT_TRUE(block.rejected.empty());
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(block.css().find("x-a: 1px; x-b: 2px;") == 0u || block.css() == "x-pair: 1px 2px;", true) << block.css();
  Block one(owned.r, "x-pair: 3px");
  EXPECT_EQ(one.rejected.size(), 0u);
  EXPECT_EQ(gtext_css_block_count(one.b), 2u);
}

TEST(CssShorthandHook, AHookThatFailsStopsTheDeclarationAndAddsNothing) {
  reg::Builder builder;
  GTEXT_CSS_Property_Def a{};
  a.name = "y-a";
  a.initial = "0px";
  a.grammar = "<length>";
  ASSERT_EQ(gtext_css_registry_register_property(builder.b, &a, nullptr), GTEXT_CSS_OK);
  const char * longhands[] = {"y-a"};
  GTEXT_CSS_Shorthand_Def def{};
  def.name = "y-one";
  def.grammar = "<length>";
  def.longhands = longhands;
  def.longhand_count = 1;
  def.hook = [](void *, const GTEXT_CSS_Value *, const GTEXT_CSS_Longhand_Sink *) { return GTEXT_CSS_E_UNSUPPORTED; };
  ASSERT_EQ(gtext_css_registry_register_shorthand(builder.b, &def, nullptr), GTEXT_CSS_OK);
  reg::Owned owned;
  ASSERT_EQ(builder.freeze(owned), GTEXT_CSS_OK);
  Block block(owned.r, "y-one: 1px");
  EXPECT_EQ(block.rejected.size(), 1u);
  EXPECT_EQ(gtext_css_block_count(block.b), 0u);
}

/* ---- the entry points -------------------------------------------------------------------------------------------------------- */

TEST(CssShorthandApi, ExpandNamesTheShorthandItCannotSplitAndRefusesWhatIsNotOne) {
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(R(), "margin", 6, "1px", 3, nullptr, &pv), GTEXT_CSS_OK);
  GTEXT_CSS_Longhand_List * list = nullptr;
  GTEXT_CSS_Shorthand_Result result{};
  ASSERT_EQ(gtext_css_shorthand_expand(R(), &pv, nullptr, &list, &result), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_longhand_list_count(list), 4u);
  GTEXT_CSS_Longhand lh;
  ASSERT_TRUE(gtext_css_longhand_list_at(list, 0, &lh));
  EXPECT_EQ(lh.property, find(R(), "margin-top"));
  EXPECT_EQ(lh.from, find(R(), "margin"));
  EXPECT_FALSE(lh.reset);
  EXPECT_FALSE(gtext_css_longhand_list_at(list, 4, &lh));
  gtext_css_longhand_list_free(list);
  gtext_css_property_value_free(&pv);
  // a longhand is no shorthand
  ASSERT_EQ(gtext_css_parse_property_text(R(), "width", 5, "1px", 3, nullptr, &pv), GTEXT_CSS_OK);
  list = nullptr;
  EXPECT_EQ(gtext_css_shorthand_expand(R(), &pv, nullptr, &list, &result), GTEXT_CSS_E_INVALID);
  gtext_css_property_value_free(&pv);
  EXPECT_EQ(gtext_css_shorthand_expand(R(), nullptr, nullptr, &list, &result), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_longhand_list_count(nullptr), 0u);
  gtext_css_longhand_list_free(nullptr);
}

TEST(CssShorthandApi, ARegisteredShorthandWithNoHookAndNoRuleIsUnsupportedAndNamesItself) {
  reg::Builder builder;
  GTEXT_CSS_Property_Def a{};
  a.name = "z-a";
  a.initial = "0px";
  a.grammar = "<length>";
  ASSERT_EQ(gtext_css_registry_register_property(builder.b, &a, nullptr), GTEXT_CSS_OK);
  const char * longhands[] = {"z-a"};
  GTEXT_CSS_Shorthand_Def def{};
  def.name = "z-one";
  def.grammar = "<length>";
  def.longhands = longhands;
  def.longhand_count = 1;
  ASSERT_EQ(gtext_css_registry_register_shorthand(builder.b, &def, nullptr), GTEXT_CSS_OK);
  reg::Owned owned;
  ASSERT_EQ(builder.freeze(owned), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_shorthand_kind(owned.r, find(owned.r, "z-one")), GTEXT_CSS_SHORTHAND_UNIMPLEMENTED);
  Block b(owned.r);
  GTEXT_CSS_Shorthand_Result result{};
  GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
  EXPECT_EQ(gtext_css_block_add_text(b.b, "z-one", 5, "1px", 3, false, nullptr, &outcome, &result), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(result.name, "z-one");
  EXPECT_NE(result.detail, nullptr);
  EXPECT_EQ(gtext_css_block_count(b.b), 0u);
  // a CSS-wide keyword needs no rule
  EXPECT_EQ(gtext_css_block_add_text(b.b, "z-one", 5, "inherit", 7, false, nullptr, &outcome, &result), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_block_count(b.b), 1u);
}

TEST(CssShorthandApi, TheDeclarationLimitIsHonoredAndAllOrNothing) {
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  o.max_declarations = 5;
  Block b(R(), "", &o);
  GTEXT_CSS_Property_Outcome outcome;
  ASSERT_EQ(gtext_css_block_add_text(b.b, "margin", 6, "1px", 3, false, nullptr, &outcome, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_block_count(b.b), 4u);
  // padding would be four more: nothing of it is added
  EXPECT_EQ(gtext_css_block_add_text(b.b, "padding", 7, "1px", 3, false, nullptr, &outcome, nullptr), GTEXT_CSS_E_LIMIT_DECLARATIONS);
  EXPECT_EQ(gtext_css_block_count(b.b), 4u);
}

TEST(CssShorthandApi, TheOutputLimitIsHonored) {
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  o.max_output_bytes = 8;
  Block b(R(), "margin: 1px; color: red", &o);
  char * out = nullptr;
  size_t n = 0;
  EXPECT_EQ(gtext_css_block_serialize_buffer(b.b, &out, &n), GTEXT_CSS_E_LIMIT_OUTPUT);
}

} // namespace
