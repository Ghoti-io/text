@page format_css_cascade The cascade and cascade layers

# The cascade and cascade layers

The computed-value layer (\ref format_css_computed "css-computed.md") takes, for one element, the declarations that won; this layer decides **which declaration
wins**. A caller hands it the candidate declarations of an element with the keys that sort them, and it returns, for each property, the winning declaration
with where it came from, the `revert` family of keywords resolved, and on request the declarations that lost and why. It does not match selectors or
evaluate conditions: the selector matcher, the attribute readers and the at-rule layer produce the candidates and their keys, and a test builds them directly.
Back to \ref text_format_references "Formats".

Include `<ghoti.io/text/css.h>` (`css/css_cascade.h`, `css/css_layer.h`).

## Quick start

Candidates go into a set with their origin, importance, layer, specificity and order of appearance; the cascade answers one winner for each property. A
later layer beats an earlier one, declarations that are in no layer beat every layer, `!important` turns the layers round, and `revert-layer` takes the
layer of the declaration out of the cascade.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static void cq_add(GTEXT_CSS_Candidates * set, const char * name, const char * value, GTEXT_CSS_Layer_Id layer, bool important, unsigned order) {
  GTEXT_CSS_Cascade_Entry keys;
  memset(&keys, 0, sizeof keys);
  keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  keys.layer = layer;
  keys.important = important;
  keys.sheet = order;
  gtext_css_candidates_add_text(set, &keys, name, strlen(name), value, strlen(value), NULL, NULL, NULL);
}

/* The candidate declarations of one element go in with the keys that sort them; the cascade says which declaration wins each property. A later layer
   beats an earlier one and unlayered declarations beat every layer, `!important` turns the layers round, and `revert-layer` takes a declaration's layer
   out of the cascade. */
static void cascade_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Layer_Registry * layers = NULL;
  gtext_css_layer_registry_new(NULL, &layers);
  GTEXT_CSS_Layer_Id base = 0, theme = 0;
  gtext_css_layer_declare(layers, GTEXT_CSS_ORIGIN_AUTHOR, 0, "base", 4, &base, NULL);
  gtext_css_layer_declare(layers, GTEXT_CSS_ORIGIN_AUTHOR, 0, "theme", 5, &theme, NULL);
  gtext_css_layer_freeze(layers);

  GTEXT_CSS_Candidates * set = NULL;
  gtext_css_candidates_new(registry, layers, NULL, &set);
  cq_add(set, "color", "red", base, false, 0);
  cq_add(set, "color", "blue", theme, false, 1);
  cq_add(set, "color", "green", GTEXT_CSS_LAYER_NONE, false, 2);
  cq_add(set, "width", "10px", base, false, 3);
  cq_add(set, "width", "20px", theme, false, 4);
  cq_add(set, "margin-left", "1px", base, true, 5);
  cq_add(set, "margin-left", "2px", theme, true, 6);
  cq_add(set, "height", "5px", base, false, 7);
  cq_add(set, "height", "revert-layer", theme, false, 8);

  GTEXT_CSS_Cascaded * cascaded = NULL;
  gtext_css_cascade(set, NULL, &cascaded, NULL);
  for (size_t i = 0; i < gtext_css_cascaded_count(cascaded); i++) {
    GTEXT_CSS_Cascade_Entry winner;
    gtext_css_cascaded_at(cascaded, i, &winner, NULL);
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(registry, winner.property, &info);
    char * value = NULL;
    size_t length = 0;
    gtext_css_serialize_value_buffer(gtext_css_value_set_root(winner.set), NULL, &value, &length);
    char layer[32];
    gtext_css_layer_path(layers, winner.layer, layer, sizeof layer);
    printf("%s: %s (%s)\n", info.name, value, winner.layer ? layer : "unlayered");
    gtext_css_serialize_buffer_free(NULL, value);
  }
  gtext_css_cascaded_free(cascaded);
  gtext_css_candidates_free(set);
  gtext_css_layer_registry_free(layers);
  gtext_css_registry_free(registry);
}
```

prints

```
color: green (unlayered)
height: 5px (base)
margin-left: 1px (base)
width: 20px (theme)
```

`color` has three candidates and the unlayered one wins; `width` is `theme`'s, the later layer; `margin-left` is `!important` in both layers, and for important
declarations the **earlier** layer wins; `height` in `theme` is `revert-layer`, so it is as if `theme` had not set it.

## The sort order

The criteria, in descending order of precedence, are Cascade 5, section 6.1 (with Cascade 6, section 3.1, for scope), each a field of
`GTEXT_CSS_Cascade_Entry`. Where a criterion splits on importance the second column is the case of an `!important` declaration.

| # | Criterion | Normal declarations | Important declarations |
|---|---|---|---|
| 1 | origin and importance | transition, important user agent, important user, important author, animation, normal author, presentational hint, normal user, normal user agent | (the same list) |
| 2 | context (`context`: 0 is the outermost shadow tree) | the outer context wins | the inner context wins |
| 3 | element-attached (`element_attached`, a `style` attribute) | attached over style rules | attached over style rules |
| 4 | layer (`layer`) | the later layer; unlayered declarations are last | the earlier layer |
| 5 | specificity (`specificity`, A then B then C) | higher | higher |
| 6 | scope proximity (`scoped`, `scope_distance`) | fewer hops; no scope is infinite | fewer hops |
| 7 | order of appearance (`sheet`, `rule`, `declaration`) | later | later |

Two entries that are equal in every key (which no caller has a reason to make) are ordered by the order they were added. A **presentational hint**
(`presentational_hint`: an HTML presentational attribute, an SVG presentation attribute) is an **origin of its own** between the normal user and the normal
author declarations, with specificity zero: below every normal author declaration of any context, layered or not, above the user and user-agent origins. It is
author-level for `revert` (which rolls it back with the author declarations) and `revert-layer` rolls back to it.

Specificity components are 32 bits and **saturate**: `gtext_css_specificity_make()` and `gtext_css_specificity_add()` stop at `UINT32_MAX`, they do not wrap,
so a selector with 2^32 ids is not weaker than one with none. Cascade 6 states proximity in prose as applying "regardless of specificity or order of
appearance" and lists it after specificity in the sorting order; this follows the list.

## Layers

`GTEXT_CSS_Layer_Registry` (css_layer.h) holds the layers of the origins that have them (user-agent, user and author) in the order of their **first
declaration**, with nested layers grouped inside their parent and the parent's own declarations after them: the example of Cascade 5, 6.4.3 is the order
`reset.type, reset, framework.theme, framework`, then the unlayered declarations. A layer's place is a number assigned once, at `gtext_css_layer_freeze()`,
so comparing two layers is comparing two integers. Anonymous layers (`@layer { }`, `@import ... layer`) are a new layer each time. Layers are scoped to their
origin **and context**: make a registry for each shadow context, and give it to the candidate set with `gtext_css_candidates_use_layers()` (the registry given to
`gtext_css_candidates_new()` is the default for the contexts that have none); that call is refused after a candidate of the context has been added or the set
sealed. A layer's path writes a `.` or a `\` in a layer's name with a backslash before it, so that the layer `a.b` and the layer `b` inside `a` have different
paths.

`gtext_css_cascade_collect_layers()` declares the layers of a parsed sheet: `@layer` statements and blocks, nested in each other and in style rules, `@scope`
and `@starting-style`, and `@import` with `layer` or `layer(name)`. An invalid rule (a CSS-wide keyword as a name, a name with white space in it, a number)
declares nothing. `@import` is read only where it is valid: at the top level, before any style rule or at-rule other than `@charset`, `@layer` statements
and other imports, with a `<url>` or a `<string>` first (its conditions go through the filter like those of `@media`); a statement `@layer` inside a style rule
declares nothing. A layer inside `@media`, `@supports`, `@container` or `@document` counts only when the **filter** the caller gives
(`GTEXT_CSS_Layer_Collect_Options::active`) says the condition applies; with no filter such a rule that holds a layer is `GTEXT_CSS_E_UNSUPPORTED` naming
`active_rule`, and one that holds none is skipped. The filter is the at-rule layer's (C4); the structure is final.

## Rolling back

`revert`, `revert-layer` and `revert-rule` never reach the computed-value layer; the cascade resolves them.

- `revert` rolls back to the cascaded value of the earlier **origin**: from an author declaration (the animation and transition origins included) as if no
  author declaration were specified, from a user declaration as if no author or user declaration were, and from a user-agent declaration it is `unset`.
- `revert-layer` rolls back "as if no rules were specified in the current cascade layer, or between its normal and important levels in the cascade". For
  important element-attached styles it only reverts the element-attached styles and the intervening animation origin; the animation origin is its own layer.
- `revert-rule` rolls back as if the style rule the declaration is in were not there: every declaration of that rule, normal and important alike.
- Importance does not change what a keyword rolls back to: a normal and an `!important` `revert-layer` both roll back to the declarations below the
  layer's normal level, so the important declarations of later layers, and the normal ones of later layers, other contexts and the unlayered styles, that lie
  between its two levels are out too (Chromium and Firefox agree; tools/oracle/cascade_probe_answers.txt in the repository).

When nothing is left to roll back to the property has **no declared value**: it is not in the result, and takes the inherited or initial value. A rolled-back
declaration is counted in the winner's `rollbacks`. A property whose best candidate rolls back is the only one that sorts its candidates, and a chain of
rollbacks is linear in the candidates: the walk only moves forward.

## `all`, shorthands, logical groups

A shorthand is expanded (css_shorthand.h) and each longhand takes the declaration's keys; `margin: revert` rolls back each of its longhands. A shorthand
holding `var()` stays pending longhands, carried and not read here (the computed-value layer substitutes them, \ref format_css_variables "css-variables.md"). `all` is one candidate that applies to every property but
`direction`, `unicode-bidi` and the custom properties; it is merged with each property's own candidates and not copied into them.

The members of a logical group that share a physical slot in the element's flow are cascaded as one: `margin-left` and `margin-inline-start` are one set of
candidates when the element's `writing-mode` and `direction` make `margin-inline-start` the left, and the winner is whichever sorts first, under whichever
name it was declared. The flow is cascaded first, from the cascade of `writing-mode`, `direction` and `text-orientation`, with `inherit` and `unset` taken
from the inheritance parent's flow (`GTEXT_CSS_Cascade_Options::parent_flow`) or the flow is given outright (`flow`). A flow property that is still pending
is taken as `unset` by the cascade (the computed style substitutes it, and maps the logical properties by what it comes to): a caller whose flow comes from a
`var()` gives `flow` outright.

## The result

`gtext_css_cascaded_at()` gives the winners in a canonical order that does not depend on the order the candidates were added in: the registered properties
by id, then the custom properties by name. Each winner is a `GTEXT_CSS_Cascade_Entry` with its provenance: the property, the custom property's name, the value,
the shorthand it came through, whether `all` declared it, the origin, importance, layer, context, specificity, scope, order, and the C0 ids and span of the
rule and declaration the caller put in the keys. With `keep_losers` the losers of each property are kept in descending order of precedence, each marked
`GTEXT_CSS_LOSS_OUTRANKED` or `GTEXT_CSS_LOSS_ROLLED_BACK` (it sorts above the winner and a rollback took it out). `gtext_css_cascaded_dump()` and
`gtext_css_cascaded_hash()` are a canonical text and a hash of the whole result.

`gtext_css_candidates_add_declaration()` takes a declaration of a C0 tree: an invalid value never enters the cascade (so a lower-priority valid declaration
wins), and an unknown property is dropped; both are recorded as **diagnostics** that name the property (`gtext_css_candidates_diagnostic()`).

## Computing the winners

`gtext_css_cascaded_to_block()` makes the declaration block that `gtext_css_compute_style()` takes: each winner once, `all` expanded to the properties it
won, nothing rolling.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static void cc_add(GTEXT_CSS_Candidates * set, GTEXT_CSS_Origin origin, const char * name, const char * value, unsigned order) {
  GTEXT_CSS_Cascade_Entry keys;
  memset(&keys, 0, sizeof keys);
  keys.origin = origin;
  keys.sheet = order;
  gtext_css_candidates_add_text(set, &keys, name, strlen(name), value, strlen(value), NULL, NULL, NULL);
}

static void cc_show(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Computed_Style * style, const char * name) {
  const GTEXT_CSS_Value * value = gtext_css_computed_root(style, gtext_css_registry_find_property(registry, name, strlen(name)));
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_value_buffer(value, NULL, &text, &length);
  printf("%s: %s\n", name, text);
  gtext_css_serialize_buffer_free(NULL, text);
}

/* The cascade's winners are a declaration block, which the computed-value layer takes as it is. The logical and physical margins of the element share a
   slot, so the later declaration wins whichever name it is written with, and the author's `revert` gives the user's color back. */
static void cascade_computed(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  GTEXT_CSS_Candidates * set = NULL;
  gtext_css_candidates_new(registry, NULL, NULL, &set);
  cc_add(set, GTEXT_CSS_ORIGIN_AUTHOR, "direction", "rtl", 0);
  cc_add(set, GTEXT_CSS_ORIGIN_AUTHOR, "margin-right", "20px", 1);
  cc_add(set, GTEXT_CSS_ORIGIN_AUTHOR, "margin-inline-start", "10px", 2);
  cc_add(set, GTEXT_CSS_ORIGIN_AUTHOR, "margin-left", "5px", 3);
  cc_add(set, GTEXT_CSS_ORIGIN_USER, "color", "green", 4);
  cc_add(set, GTEXT_CSS_ORIGIN_AUTHOR, "color", "revert", 5);

  GTEXT_CSS_Cascaded * cascaded = NULL;
  gtext_css_cascade(set, NULL, &cascaded, NULL);
  GTEXT_CSS_Declaration_Block * block = NULL;
  gtext_css_cascaded_to_block(cascaded, NULL, &block);
  GTEXT_CSS_Computed_Style * style = NULL;
  gtext_css_compute_style(context, block, NULL, NULL, &style, NULL);
  cc_show(registry, style, "color");
  cc_show(registry, style, "margin-left");
  cc_show(registry, style, "margin-right");

  gtext_css_computed_free(style);
  gtext_css_block_free(block);
  gtext_css_cascaded_free(cascaded);
  gtext_css_candidates_free(set);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
```

prints

```
color: green
margin-left: 5px
margin-right: 10px
```

`direction: rtl` makes `margin-inline-start` the right margin, and it is declared after `margin-right: 20px`, so it wins the slot; the author's `revert` of
`color` gives the user's `green` back.

## Limits, cost and reentrancy

The number of candidates (`GTEXT_CSS_E_LIMIT_CANDIDATES`, naming the declaration that crossed it), the number of layers (`GTEXT_CSS_E_LIMIT_LAYERS`), the
depth of layer nesting (`GTEXT_CSS_E_LIMIT_LAYER_DEPTH`) and the depth of rules the sheet reader follows (`GTEXT_CSS_E_LIMIT_DEPTH`) are each their own status,
tested at the bound and one past. Nothing recurses. A cascade is linear in the candidates plus a sort of the candidates of each property whose best candidate
rolls back, of the names of the custom properties that have a winner, and of the losers when they are kept; the layers of a registry are compared in constant
time. `all` costs a pass over the properties it applies to. A candidate set is mutable until the first cascade seals it (adding to a sealed set is
`GTEXT_CSS_E_INVALID`); then it and every result are immutable, a result holds the set it was made from, and any number of threads may read them, each
cascading with its own state. Adding candidates and registering layer registries is for one thread, and the first cascade of a set seals it: an `add()`
concurrent with that first `cascade()` is not supported. An allocation that fails is `GTEXT_CSS_E_OOM` with nothing returned and nothing leaked.

## Verification

Each criterion of the sort order, each rolling keyword, the flow, `all` and the logical groups have a test that quotes the draft sentence it rests on
(tests/test-css-cascade.cpp), and a permutation test shuffles the candidates of random elements. A libFuzzer harness (tests/fuzz/fuzz_css_cascade.cpp) checks
every cascade against a **reference model** written criterion by criterion with rollbacks resolved by removing and starting again; its seeds are replayed
by `make test`. Planted defects (every pair of neighbouring criteria swapped, each rolling keyword undone, memory let go of) are seen caught by the unit
tests and the seeds in `make test` and by the fuzzer in `make check-css-fuzz-plants`; the quadratic suite has shapes for the sort, the chains, the layers
and the custom property names, and plants that make each quadratic. The browser differential of cascaded results is the element layer's (C2d).
