# CSS at-rules: the typed model, the typed views and the set

Headers: `css_atrule.h`, `css_font_rules.h`, `css_counter_style.h`, `css_keyframes.h`, `css_page.h`, `css_atrule_set.h`.

The generic parser (`css_core.h`) returns every at-rule as a rule with a name, a prelude and a block. This layer reads the at-rules the pinned drafts list into a typed, immutable,
reference counted `GTEXT_CSS_Atrule`, and builds from them the structures a renderer asks for. Nothing here lays out a page, loads a font or generates a counter.

## The model

`gtext_css_atrule_parse(registry, rule, options, &out, &result)` reads one rule. The result has a `kind` (`gtext_css_atrule_kind_name()` names it), the decoded names of the
prelude, the **descriptors** (or, for a keyframe, a page or `@position-try`, the **declarations**), and the nested rules (keyframe blocks, margin rules, feature blocks). Every
descriptor goes through the registry's grammar for it and is kept as a typed value and as its canonical text.

- An invalid or unknown descriptor is dropped with a diagnostic (`gtext_css_atrule_diagnostic()`); the rule stands. Each diagnostic names the sentence of the draft that decides it.
- A rule is ignored as a whole (`result.valid` false, nothing returned) only where the served text says so: `@keyframes` named by a CSS-wide keyword or `none`, `@font-palette-values`
  without `font-family`, `@page` with an invalid selector, a rule in a place that does not allow it (`options.parent`: an `@font-face` in a style rule, a margin rule outside `@page`).
- A rule that is valid and defines nothing says so (`gtext_css_atrule_defines_nothing()`): an `@font-face` without `font-family` or `src` is not considered, a `@counter-style`
  with too few symbols for its system defines nothing.
- `gtext_css_atrule_serialize()` writes the rule as CSSOM's "serialize a CSS rule" does. The text parses back to a rule that serializes to the same bytes.
- `gtext_css_atrule_validity_init()` fills a `GTEXT_CSS_Validity`: a parse given it drops an at-rule that is invalid where it stands, as Syntax says an invalid rule is dropped, and keeps the same tree as a parse without it when every at-rule is valid.

`gtext_css_atrule_ledger_row()` is the table of which kinds parse, validate, serialize, apply and are typed.

## Typed views

Each `_from_atrule()` reads the canonical text of the descriptors and returns an owned struct of plain C types.

- `GTEXT_CSS_Font_Face`: the family (decoded, with a `family_key` folded by Unicode full case folding), `src` entries (`url` or `local`, `format` with a bit, `tech` bits), style with
  the oblique range, closed weight and width ranges, `unicode-range` merged, sorted and non-overlapping (`U+4??` is `U+400-4FF`; `U+0-10FFFF` when absent), feature and variation
  settings, `font-display`, the metric overrides, `size-adjust`, and a `present` bit set that tells `auto` from absent. `gtext_css_font_face_defined()` says whether the rule has both
  `font-family` and `src`.
- `GTEXT_CSS_Feature_Values`: the families and, for each of the seven feature types, name to integers. `GTEXT_CSS_Palette_Values`: the dashed name, families, base palette, and the
  override colors as text in their declared space. `GTEXT_CSS_Color_Profile`: name, source, rendering intent, components.
- `GTEXT_CSS_Counter_Style`: system (with the fixed first value), symbols, additive tuples, negative, prefix, suffix, range, pad, fallback, speak-as, and whether the counts are valid for the system.
- `GTEXT_CSS_Keyframes`: the name, the number of blocks, and the **normalized** frames: offsets strictly ascending, blocks of one offset merged in document order, the later declaration
  of a property winning. `!important` declarations and the `animation-*` properties are not in a keyframe.
- `css_page.h`: page selectors with their specificity `(f, g, h)` and `gtext_css_page_matches()`, a pure function of the page name, first, blank and side.

## The set

`GTEXT_CSS_Atrule_Set` collects the at-rules of a document's trees in order, and finds them.

1. `gtext_css_atrule_set_new()`, then `gtext_css_atrule_set_add_tree()` for each sheet in document order. A conditional group (`@media`, `@supports`, `@when`, `@else`, `@container`) holds when
   the set's `conditions` say so; a group that is false contributes nothing, and declares no layer. Without `conditions`, a group that holds a rule the set collects is
   `GTEXT_CSS_E_UNSUPPORTED`, never a guess. `@layer` statements and blocks order the rules; the walk uses no recursion.
2. `gtext_css_atrule_set_freeze()` assigns the layer ranks, types every rule, resolves the counter style chains and merges the feature values.
3. The finds. Everything they return belongs to the set. The last rule of a name in the highest layer wins: keyframes, counter styles, `@position-try`, palettes (by name and family),
   color profiles; `gtext_css_atrule_set_view_transition()` gives the last `@view-transition`. `gtext_css_atrule_set_font_faces()` gives the defined faces of a family key in cascade
   order. `gtext_css_atrule_set_feature_values()` gives the merge of every `@font-feature-values` rule for a family (a later name replaces an earlier one, per type; names sorted).
   `gtext_css_atrule_set_page_collect()` gives the `@page` rules that match a page in the order of the page cascade: layer rank, then specificity, then place.

A counter style's `extends` chain is followed to a base (a cycle makes each style on it extend `decimal`: `extends_cycle`, `resolved_system` numeric), and its `fallback` chain is followed
with a cycle cut at its first repeat (`fallback_cycle`). Both walks are linear in the number of styles however long the chains.

Names are scoped to the set's `context` (one set for the document and one for each shadow tree). Container names are not.

## Limits

`max_descriptors` and `max_children` bound a rule, `max_list` the lists it unfolds; the set has `max_rules` and `max_rule_depth`. A limit is a status with no partial result.
