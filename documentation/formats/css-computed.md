@page format_css_computed Computed values, shorthands and logical properties

# Computed values, shorthands and logical properties

The registry (\ref format_css_registry "css-registry.md") says what a property's value is; this layer says what it *comes to* on an
element. It **computes** the value of every property of one element from the declarations that won the cascade, the style of the element it
inherits from and the environment; it **maps** logical properties to the physical ones they share a value with; and it **expands** a shorthand
into its longhands and writes a block's longhands back as the shortest shorthands, as CSSOM does. It sits on the property layer; the
cascade, selector matching and `var()` substitution are not here, they hand this layer their results. Back to \ref text_format_references
"Formats".

Include `<ghoti.io/text/css.h>` (`css/css_computed.h`, `css/css_element.h`, `css/css_logical.h`, `css/css_shorthand.h`).

## Quick start

A style is computed from a **declaration block** (the winners of the cascade, in ascending priority) and the style of the parent. Lengths become
pixels against the font size, a property nobody declares is inherited or initial, and a logical property is the physical one the element's
writing mode says.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const char * name;
  const char * value;
} declaration;

static GTEXT_CSS_Declaration_Block * block_of(const GTEXT_CSS_Registry * registry, const declaration * list, size_t count) {
  GTEXT_CSS_Declaration_Block * block = NULL;
  gtext_css_block_new(registry, NULL, &block);
  for (size_t i = 0; i < count; i++) {
    gtext_css_block_add_text(block, list[i].name, strlen(list[i].name), list[i].value, strlen(list[i].value), false, NULL, NULL, NULL);
  }
  return block;
}

static void show(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Computed_Style * style, const char * name) {
  const GTEXT_CSS_Value * value = gtext_css_computed_root(style, gtext_css_registry_find_property(registry, name, strlen(name)));
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_value_buffer(value, NULL, &text, &length);
  printf("%s: %s\n", name, text);
  gtext_css_serialize_buffer_free(NULL, text);
}

/* A style is computed from the declarations that won the cascade and the style of the parent: lengths become pixels against the font size,
   a property nobody declares is inherited or initial, a logical property is the physical one the writing mode says. */
static void computed_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  const declaration parent_css[] = {{"font-size", "20px"}, {"color", "red"}};
  const declaration child_css[] = {{"width", "2em"}, {"margin-inline-start", "5px"}, {"border-style", "none"}, {"border-width", "3px"}};
  GTEXT_CSS_Declaration_Block * parent_block = block_of(registry, parent_css, 2);
  GTEXT_CSS_Declaration_Block * child_block = block_of(registry, child_css, 4);

  GTEXT_CSS_Computed_Style * parent = NULL;
  GTEXT_CSS_Computed_Style * child = NULL;
  gtext_css_compute_style(context, parent_block, NULL, NULL, &parent, NULL);
  gtext_css_compute_style(context, child_block, parent, NULL, &child, NULL);

  show(registry, child, "width");
  show(registry, child, "color");
  show(registry, child, "margin-left");
  show(registry, child, "border-top-width");

  gtext_css_computed_free(child);
  gtext_css_computed_free(parent);
  gtext_css_block_free(child_block);
  gtext_css_block_free(parent_block);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
```

prints

```
width: 40px
color: red
margin-left: 5px
border-top-width: 0px
```

`width: 2em` is 40px because the parent's `font-size` is 20px; `color` was not declared and is inherited; `margin-inline-start` of a horizontal
left-to-right element is `margin-left`; a border width under `border-style: none` computes to 0px.

## The tree and the environment

The library never owns or walks a tree except through a table of callbacks (`GTEXT_CSS_Element_Interface`, css_element.h): `parent`, a separate
`inheritance_parent`, names, classes, attributes, siblings and document order. A callback the table lacks and a computation needs is
`GTEXT_CSS_E_UNSUPPORTED` naming it (`gtext_css_element_callback_name()`); it is never worked around. Inheritance is its own callback because
it does not always follow the tree: an SVG `<use>` clone inherits from the `<use>`, and the elements of a shadow tree through the flat tree.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

typedef struct node {
  const char * name;
  const struct node * parent;
  const struct node * inherits_from;
  const char * font_size;
  GTEXT_CSS_Declaration_Block * block;
} node;

static GTEXT_CSS_Element_Handle tree_parent(void * user, GTEXT_CSS_Element_Handle element) {
  (void)user;
  return ((const node *)element)->parent;
}

static GTEXT_CSS_Element_Handle inheritance_parent(void * user, GTEXT_CSS_Element_Handle element) {
  (void)user;
  return ((const node *)element)->inherits_from;
}

static GTEXT_CSS_Status declarations_of(void * user, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_Declaration_Block ** block) {
  (void)user;
  *block = ((const node *)element)->block;
  return GTEXT_CSS_OK;
}

/* The tree and the inheritance are two things: a clone made for an SVG <use> sits in the shadow tree and inherits from the <use>, not from the
   element it was cloned from. The caller says both. */
static void computed_element(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  node root = {"svg", NULL, NULL, "10px", NULL};
  node source = {"symbol", &root, &root, "40px", NULL};
  node use = {"use", &root, &root, "20px", NULL};
  node clone = {"symbol clone", &source, &use, NULL, NULL};
  node * all[] = {&root, &source, &use, &clone};
  for (size_t i = 0; i < 4; i++) {
    gtext_css_block_new(registry, NULL, &all[i]->block);
    if (all[i]->font_size) {
      gtext_css_block_add_text(all[i]->block, "font-size", 9, all[i]->font_size, strlen(all[i]->font_size), false, NULL, NULL, NULL);
    }
    gtext_css_block_add_text(all[i]->block, "width", 5, "2em", 3, false, NULL, NULL, NULL);
  }

  GTEXT_CSS_Element_Interface iface;
  memset(&iface, 0, sizeof iface);
  iface.size = sizeof iface;
  iface.parent = tree_parent;
  iface.inheritance_parent = inheritance_parent;
  GTEXT_CSS_Declaration_Source source_of = {NULL, declarations_of};

  for (size_t i = 0; i < 4; i++) {
    GTEXT_CSS_Computed_Style * style = NULL;
    gtext_css_compute_element(context, &iface, &source_of, all[i], &style, NULL);
    char * text = NULL;
    size_t length = 0;
    gtext_css_serialize_value_buffer(gtext_css_computed_root(style, gtext_css_registry_find_property(registry, "width", 5)), NULL, &text, &length);
    printf("%s: width %s\n", all[i]->name, text);
    gtext_css_serialize_buffer_free(NULL, text);
    gtext_css_computed_free(style);
  }
  for (size_t i = 0; i < 4; i++) {
    gtext_css_block_free(all[i]->block);
  }
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
```

prints

```
svg: width 20px
symbol: width 80px
use: width 40px
symbol clone: width 40px
```

`gtext_css_compute_element()` follows the chain of inheritance parents from the root down, iteratively and in work linear in the depth, and
returns the style of the element asked for.

What a style depends on that no document says is the `GTEXT_CSS_Environment`: the viewport sizes (small, large and dynamic), what `medium` is, the
device pixel ratio, the color scheme, the language, the base URL, and optional callbacks for what only a font engine, a layout engine or a color
engine knows (`ex`, `ch`, `cap`, `ic` and their root forms, the `normal` line height, container query lengths, colors that need conversion).
Nothing reads the process, the clock or the network. A computation that needs a callback the environment lacks fails with
`GTEXT_CSS_E_UNSUPPORTED` naming the unit and returns **no** style, never part of one. The one exception is color: `text` does not convert colors, so
a `color-mix()` or a relative color whose parts are computed stays as written, flagged `GTEXT_CSS_VFLAG_COLOR_UNRESOLVED`, unless the environment
has a color callback to resolve it.

## How a value is computed

Every property of the pin has a **class** (`gtext_css_compute_class()`), taken from the sentence the pin gives for its computed value and held by a
gate (`make check-css-compute`): every property is classified, has an algorithm of its own, or is listed as unimplemented with a reason. The
properties that are inherited, and those of SVG 2's presentation attributes, may not be unimplemented.

| Class | What it does |
|---|---|
| identity | the computed value is the specified value |
| by type | each value type computes as Values 4 says: absolute lengths to `px`, font-relative lengths against the element's computed `font-size` (`rem` against the root's), viewport lengths against the environment, angles to `deg`, times to `s`, frequencies to `Hz`, resolutions to `dppx`; percentages kept; math functions simplified and clamped to their slot; `light-dark()` resolved from the color scheme; `currentcolor` kept; relative URLs resolved against the base URL; lists item by item |
| algorithm | the property's own: `font-size` keywords, `larger` and `smaller`, `bolder` and `lighter`, `line-height`, border widths under `none` and `hidden`, blockification of `display`, `content`, `text-decoration` and the others the specification writes as prose |
| unimplemented | `GTEXT_CSS_E_UNSUPPORTED` naming the property when a declaration needs it computed |

`initial`, `inherit` and `unset` are applied here, and `all` is a declaration at its place in the block (a later declaration of a property beats
it, an earlier one does not; it spares `direction`, `unicode-bidi` and the custom properties). `revert` and `revert-layer` are results of the
cascade and are refused as inputs, and so is a value still holding an unsubstituted `var()`: `GTEXT_CSS_E_INVALID` with a `detail` that says which.

Properties are computed in a fixed order (`writing-mode`, `direction`, `text-orientation`, the font properties, `font-size`, `line-height`, the border
styles, `float`, `position`, `display`, then the rest), and a property reads only what is above it or the parent's style. A computed value is an
immutable, reference-counted `GTEXT_CSS_Value_Set`: a style holds one pointer for every property of the registry, what it inherits is the very set
of the parent, so a long chain of inheritance shares what it inherits and a value outlives the style that made it while anyone retains it.

## Logical properties

The pin puts 128 properties in 20 logical groups (`margin`, `border-width`, `inset`, `size`, `overflow`, `border-radius` ...). Members of a group
share a computed value: "determined by cascading the declarations of both properties together as one" (Logical Properties 1), so of the
declarations that map to one physical slot the last in the block wins. The map is a function of the element's computed `writing-mode`,
`direction` and `text-orientation` (css_logical.h: `gtext_css_logical_map()`, `gtext_css_logical_slot()`, `gtext_css_logical_sharers()`), and the cascade
layer calls it to decide the winners before computing.

## Shorthands

A shorthand added to a declaration block is **expanded** into its longhands, and the properties it resets (`font` sets `font-kerning`). The value
is split into each longhand's own text, which that longhand parses with its own grammar, so a longhand gets exactly the typed value that writing
it would have given. How a shorthand is split is its **kind** (`gtext_css_shorthand_kind()`): `{1,4}` boxes, pairs, components in any order,
comma separated layers, one value copied to every longhand, and the shorthands with algorithms of their own: `border-radius`, `font`, `flex`,
the grid lines, `grid-template` and `grid`, `offset`, `text-align`, `font-synthesis`, and keyword shorthands such as `white-space: pre`. A
shorthand that holds `var()` sets every longhand to a pending value; a CSS-wide keyword sets every longhand to it. A caller may register a
shorthand with a **hook** that does the splitting. A shorthand of the pin that has no rule is `GTEXT_CSS_E_UNSUPPORTED` naming it.

A block is written back as **CSSOM** says: when all of a shorthand's longhands are present with equal importance and none is pending, the shorthand
is written in its shortest form, leaving out the components that are their initial value, and the longhands are not written again.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A shorthand is declared once and stored as its longhands; the block writes it back as the shortest shorthand its longhands make. */
static void shorthand_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Declaration_Block * block = NULL;
  gtext_css_block_new(registry, NULL, &block);

  const char * declarations[][2] = {{"margin", "1px 2px"}, {"border", "red solid 2px"}, {"margin-top", "5px"}, {"flex", "2"}};
  for (size_t i = 0; i < sizeof declarations / sizeof declarations[0]; i++) {
    gtext_css_block_add_text(block, declarations[i][0], strlen(declarations[i][0]), declarations[i][1], strlen(declarations[i][1]), false, NULL, NULL, NULL);
  }
  printf("%zu longhands\n", gtext_css_block_count(block));

  char * text = NULL;
  size_t length = 0;
  gtext_css_block_serialize_buffer(block, &text, &length);
  printf("%s\n", text);
  gtext_css_serialize_buffer_free(NULL, text);

  const char * asked[] = {"margin", "margin-right", "border-top-style", "grid"};
  for (size_t i = 0; i < sizeof asked / sizeof asked[0]; i++) {
    gtext_css_block_get_property_value(block, gtext_css_registry_find_property(registry, asked[i], strlen(asked[i])), &text, &length, NULL);
    printf("%s -> \"%s\"\n", asked[i], text);
    gtext_css_serialize_buffer_free(NULL, text);
  }
  gtext_css_block_free(block);
  gtext_css_registry_free(registry);
}
```

prints

```
25 longhands
margin: 5px 2px 1px; border: 2px solid red; flex: 2 1 0%;
margin -> "5px 2px 1px"
margin-right -> "2px"
border-top-style -> "solid"
grid -> ""
```

`margin-top: 5px` came after `margin: 1px 2px`, and the four sides still make one `margin`. `border-top-style` alone has a value; `grid` has none in
this block, so its value is the empty string, as CSSOM answers.

## Limits

The depth of an element chain, the length of a list and the number of nodes of one computed value are limits (`GTEXT_CSS_Compute_Options`), each with
its own status; so are a block's declarations and the bytes its text may take (`GTEXT_CSS_Declaration_Options`). Nothing recurses in the library's
code, every allocation goes through the allocator the caller gave (a failure is `GTEXT_CSS_E_OOM` with nothing returned and nothing left behind), a
context is immutable and may be shared by threads that each compute their own styles, and a style may be read by any number of threads.

## What this layer does not do

It does not match selectors, run the cascade, substitute `var()` or register `@property` (the cascade layer does, and hands this one the winning
declarations); it does not convert colors, lay anything out or compute used values.
