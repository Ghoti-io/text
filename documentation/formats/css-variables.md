@page format_css_variables Custom properties, substitution and @property

# Custom properties, substitution and @property

A declaration can hold a function whose value is not known until an element is: `var()`, `env()`, `attr()`, `if()`, `inherit()`, `ident()`,
`random-item()`, `cycle()`. CSS Values 5 calls them **arbitrary substitution functions**. A declaration that holds one is valid from the moment it is parsed,
and **pending** until a style is computed; this layer is what computes it. It replaces the functions in the custom properties of the element
(`--*`), then in every other property that holds one, and hands the result to the computed-value layer as the text it would have been had it been written
that way. It also registers custom properties (`@property`, `CSS.registerProperty()`) and computes the registered ones by the rules of their syntax. Back to
\ref text_format_references "Formats"; the layers it sits on are \ref format_css_computed "css-computed.md" (the style that is computed), \ref format_css_cascade
"css-cascade.md" (which declaration wins) and \ref format_css_registry "css-registry.md" (what a property's value is).

Include `<ghoti.io/text/css.h>` (`css/css_custom.h`, `css/css_substitute.h`, `css/css_at_property.h`, `css/css_registry.h`, `css/css_computed.h`).

## Quick start

Nothing has to be called to substitute: a declaration block with `var()` in it is computed as any other is. The custom properties of the style are read with
`gtext_css_computed_custom_find()`; a name the element has no value for, or whose value is the guaranteed-invalid value, is not found.

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

static void show_custom(const GTEXT_CSS_Computed_Style * style, const char * name) {
  const GTEXT_CSS_Custom_Value * value = NULL;
  if (!gtext_css_computed_custom_find(style, name, strlen(name), &value)) {
    printf("%s: (the guaranteed-invalid value)\n", name);
    return;
  }
  char * text = NULL;
  size_t length = 0;
  gtext_css_custom_value_text(value, NULL, &text, &length);
  printf("%s: %s\n", name, text);
  gtext_css_serialize_buffer_free(NULL, text);
}

/* `var()` and the other substitution functions are replaced when a style is computed: the custom properties first, then every property that holds one.
   Nothing else has to be called; a declaration block with `var()` in it is computed as any other is. */
static void variables_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  const declaration css[] = {
      {"--gap", "4px 8px"},
      {"--pad", "var(--gap) 2px"},
      {"--cycle-a", "var(--cycle-b)"},
      {"--cycle-b", "var(--cycle-a)"},
      {"margin", "var(--gap)"},
      {"width", "var(--missing, 10px)"},
      {"height", "var(--cycle-a, 3px)"},
  };
  GTEXT_CSS_Declaration_Block * block = block_of(registry, css, 7);
  GTEXT_CSS_Computed_Style * style = NULL;
  gtext_css_compute_style(context, block, NULL, NULL, &style, NULL);

  show_custom(style, "--pad");
  show_custom(style, "--cycle-a");
  show(registry, style, "margin-top");
  show(registry, style, "margin-right");
  show(registry, style, "width");
  show(registry, style, "height");

  gtext_css_computed_free(style);
  gtext_css_block_free(block);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
```

prints

```
--pad: 4px 8px 2px
--cycle-a: (the guaranteed-invalid value)
margin-top: 4px
margin-right: 8px
width: 10px
height: 3px
```

`--cycle-a` and `--cycle-b` refer to each other, so both are the guaranteed-invalid value and `var(--cycle-a, 3px)` takes its fallback. `margin: var(--gap)`
was replaced as a whole and then expanded: `4px 8px` is a margin of two values.

## What happens, in what order

1. The custom properties the element declares are resolved first, since any other property may refer to them. The value of each has its functions replaced
   (a cycle makes every property in it the guaranteed-invalid value), and a registered one is checked against its syntax and computed by the rules of its type.
2. A pending ordinary property has its functions replaced. If that left the guaranteed-invalid value, or text that is not a value of the property, it is
   **invalid at computed-value time**, which is `unset`: the inherited value for a property that inherits, the initial value for one that does not (not the
   value of an earlier declaration: that one lost in the cascade already). Otherwise the text is parsed as the property's value, and computed as any value is.
3. A shorthand that held a function is replaced and parsed as a whole, and then expanded to its longhands: `margin: var(--m)` with `--m: 1px 2px` is four
   longhands. If it is invalid, every longhand is.
4. If a value, after replacement, is only a CSS-wide keyword (`var(--x, initial)`, or a custom property that is `var(--x, unset)`), it is that keyword.
   `revert` and `revert-layer` cannot be resolved here, the cascade being over: they are invalid at computed-value time.

The replacement follows the appendix of CSS Values 5 step by step. The arguments of a function are divided before anything in them is replaced; a
replacement that leaves the guaranteed-invalid value in its place makes what holds it guaranteed-invalid, and the fallback of a `var()` is used only
when the property it names is. Nothing recurses: the machine has an explicit stack, so a chain of a hundred thousand custom properties is a hundred thousand
frames of memory and not of the program's stack.

## Custom property values

A custom property's value is its **tokens, exactly as specified**: `--c: /* a */ 1.0   2.50 /* b */ x` is that text, comments and spacing included, and
names are case-sensitive (`--foo` and `--FOO` are two properties). A value that comes from a replacement is the pieces joined, with `/**/` between two
tokens that would otherwise merge into one (`var(--gap)px` with `--gap: 20` is `20/**/px`, not the token `20px`).

The **guaranteed-invalid value** is not the empty value. `--x:;` is a valid, empty value (`margin: var(--x) 1px` is `margin: 1px`); a property that was
never declared, or came to an invalid substitution, or is `initial`, has the guaranteed-invalid value, which is "not found" to
`gtext_css_computed_custom_find()` and makes any `var()` of it take its fallback. `inherit` and `unset` take the parent's value, as custom properties inherit.

A value is a `GTEXT_CSS_Custom_Value` (`css/css_custom.h`): immutable, reference counted, and a **token rope**. Building the value of `var(--a) var(--a)` joins two
references to one rope, so memory is what was written and not what is named; the number of tokens each would have is a counter that saturates, which is what
the budget below is held to. `gtext_css_custom_value_text()` writes it, `_length()` and `_tokens()` count it, `_tainted()` says whether `attr()` made any
of it. A value may be shared by any number of threads.

## Cycles

"If there is a cycle in the dependency graph, all the custom properties in the cycle are invalid at computed-value time." Every property in a cycle is the
guaranteed-invalid value, and a property that refers to a cycle only reads the invalid value as it would read any: with its fallback if it has one. A
cycle through the font-relative units of a registered property (`--len: 2em` for the `font-size` that reads it) is a cycle too.

## The other functions

| Function | What it needs | Without it |
|---|---|---|
| `var(--name, fallback)`, `inherit(--name, fallback)` | nothing; `inherit()` reads the parent's computed value | |
| `env(name index..., fallback)` | `GTEXT_CSS_Environment::env_variable` | the name is unknown: the fallback is used |
| `attr(name type, fallback)` | the element interface's `attribute` callback (css_element.h) | `GTEXT_CSS_E_UNSUPPORTED` naming `attribute` |
| `if(cond: value; ... else: value)` | `GTEXT_CSS_Environment::condition` for `media()` and `supports()`; `style(--x: v)` reads the custom properties | `GTEXT_CSS_E_UNSUPPORTED` naming `condition` |
| `ident(...)`, `random-item(key, a, b ...)` | nothing; `random_base` or `random_seed` of the environment decides the pick | |
| `random(...)`, `sibling-index()`, `sibling-count()` | `random_base`, and the element interface's `sibling_index` and `sibling_count` | `GTEXT_CSS_E_UNSUPPORTED` naming the callback |
| `cycle(a, b, c)` | the property's value in the parent: the one after the match, wrapping | |

`attr()` is **tainted**: what it makes cannot become a URL ("attr() may not be used to make a URL"), so a `url()` or `image()` made from it is invalid at
computed-value time. A function written directly after `...` (`max(var(--list)...)`) is replaced first and the arguments are divided on what it gave.
An argument grammar that is not met is a parse error: `var(--a,!)`, `var(--a,;)` and `var(--a) !important !important` are invalid declarations, and the one
before them in the cascade stands.

## Registered custom properties and `@property`

A **registration** gives a custom property a syntax, whether it inherits, and an initial value. It changes nothing at parse time, and at computed-value time
its value is checked against the syntax and computed by its type: `2em` as a `<length>` is pixels, a list is computed item by item, `<color>` is a color.
A value that is not of the syntax is invalid at computed-value time, and the property takes its **initial value**, which is the property's own, not the
inherited one. The initial value has to be **computationally independent**: `5px`, `1in` and `light-dark(red, blue)` (computed in the environment's color scheme) are, `3em`, `currentcolor` and
anything with `var()` are not. A caller registers through the registry builder before it is frozen, as `CSS.registerProperty()` does:

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

static void show_custom(const GTEXT_CSS_Computed_Style * style, const char * name) {
  const GTEXT_CSS_Custom_Value * value = NULL;
  if (!gtext_css_computed_custom_find(style, name, strlen(name), &value)) {
    printf("%s: (the guaranteed-invalid value)\n", name);
    return;
  }
  char * text = NULL;
  size_t length = 0;
  gtext_css_custom_value_text(value, NULL, &text, &length);
  printf("%s: %s\n", name, text);
  gtext_css_serialize_buffer_free(NULL, text);
}

/* A registered custom property has a syntax, whether it inherits and an initial value. Its value is checked against the syntax and computed by the rules of
   its type: `2em` is a length, so it becomes pixels against the element's font size, and `red` is not one, so the property is invalid at computed-value time
   and takes its initial value. A registration is made as `CSS.registerProperty()` makes one: through the builder, before the registry is frozen. */
static void variables_registered(void) {
  GTEXT_CSS_Registry_Builder * builder = NULL;
  gtext_css_registry_builder_new(NULL, &builder);
  GTEXT_CSS_Custom_Def def;
  memset(&def, 0, sizeof def);
  def.name = "--len";
  def.syntax = "<length>";
  def.inherits = false;
  def.initial_value = "0px";
  gtext_css_registry_register_custom(builder, &def, NULL);
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_freeze(builder, &registry, NULL);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  const declaration good[] = {{"font-size", "20px"}, {"--len", "2em"}};
  const declaration bad[] = {{"--len", "red"}};
  GTEXT_CSS_Declaration_Block * good_block = block_of(registry, good, 2);
  GTEXT_CSS_Declaration_Block * bad_block = block_of(registry, bad, 1);
  GTEXT_CSS_Computed_Style * good_style = NULL;
  GTEXT_CSS_Computed_Style * bad_style = NULL;
  gtext_css_compute_style(context, good_block, NULL, NULL, &good_style, NULL);
  gtext_css_compute_style(context, bad_block, NULL, NULL, &bad_style, NULL);

  show_custom(good_style, "--len");
  show_custom(bad_style, "--len");

  gtext_css_computed_free(bad_style);
  gtext_css_computed_free(good_style);
  gtext_css_block_free(bad_block);
  gtext_css_block_free(good_block);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
```

prints

```
--len: 40px
--len: 0px
```

An `@property` rule is a registration too (css_at_property.h). The rules of the sheets are collected into a set, and a compute context made with
`GTEXT_CSS_Compute_Options::property_rules` computes by them. A rule without `syntax` or `inherits`, with an initial value that is not of the syntax or is not
computationally independent, or with a name that is not a custom property's, is **ignored**, and the set keeps a diagnostic with the name, the reason, the
rule's place and the sentence of the draft it applies; the last valid rule of a name wins, and so does a registration the caller made over a rule. A rule inside
`@layer` is read; one inside `@media` or `@supports` is read when the `active` callback of the options says the condition holds, and `GTEXT_CSS_E_UNSUPPORTED`
(naming `active_rule`) when there is none: a condition is never guessed.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* An `@property` rule of a style sheet is a registration too. The rules of the sheets are collected into a set, and a compute context made with the set
   computes the registered properties by them. A rule that is not valid is ignored, and the set says which and where; a registration the caller made
   with gtext_css_registry_register_custom() wins over a rule of the same name. */
static void variables_property_rule(void) {
  const char sheet[] =
      "@property --size { syntax: '<length>'; inherits: true; initial-value: 16px }\n"
      "@property --broken { inherits: true }\n";
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Tree * tree = NULL;
  gtext_css_parse_stylesheet(sheet, strlen(sheet), NULL, &tree, NULL);

  GTEXT_CSS_At_Property_Set * rules = NULL;
  gtext_css_at_property_set_new(registry, NULL, &rules);
  gtext_css_at_property_collect(rules, tree, NULL);
  printf("%zu rule registered\n", gtext_css_at_property_count(rules));
  for (size_t i = 0; i < gtext_css_at_property_diagnostic_total(rules); i++) {
    GTEXT_CSS_At_Property_Diagnostic d;
    gtext_css_at_property_diagnostic(rules, i, &d);
    printf("ignored %.*s: %s\n", (int)d.name.length, d.name.data, gtext_css_at_property_reason_name(d.reason));
  }
  GTEXT_CSS_Custom_Info info;
  if (gtext_css_at_property_find(rules, "--size", 6, &info)) {
    printf("--size: syntax %.*s, initial %.*s\n", (int)info.syntax_length, info.syntax, (int)info.initial_length, info.initial_value);
  }

  GTEXT_CSS_Compute_Options options = gtext_css_compute_options_default();
  options.property_rules = rules;
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, &options, &context, NULL);
  GTEXT_CSS_Declaration_Block * block = NULL;
  gtext_css_block_new(registry, NULL, &block);
  gtext_css_block_add_text(block, "width", 5, "var(--size)", 11, false, NULL, NULL, NULL);
  GTEXT_CSS_Computed_Style * style = NULL;
  gtext_css_compute_style(context, block, NULL, NULL, &style, NULL);
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_value_buffer(gtext_css_computed_root(style, gtext_css_registry_find_property(registry, "width", 5)), NULL, &text, &length);
  printf("width: %s\n", text);
  gtext_css_serialize_buffer_free(NULL, text);

  gtext_css_computed_free(style);
  gtext_css_block_free(block);
  gtext_css_compute_context_free(context);
  gtext_css_at_property_set_free(rules);
  gtext_css_tree_free(tree);
  gtext_css_registry_free(registry);
}
```

prints

```
1 rule registered
ignored --broken: no-syntax
--size: syntax <length>, initial 16px
width: 16px
```

## The budget

"UAs must impose a UA-defined limit on the allowed length of the token stream that an arbitrary substitution function expands into." Without one, thirty
declarations of a few bytes each name a value of a billion tokens. `GTEXT_CSS_Compute_Options::substitute` (css_substitute.h) holds the limits, each with
a default and none of them zero-means-unlimited:

| Field | Default | A value past it |
|---|---|---|
| `max_substituted_tokens` | 1,048,576 | is the guaranteed-invalid value, counted in `substitution_limited` |
| `max_element_substituted_tokens` | 16,777,216 | is the guaranteed-invalid value, once the element's values together have used it |
| `max_value_bytes` | 1 MiB | a substituted value: the guaranteed-invalid value; a declared one: `GTEXT_CSS_E_LIMIT_CUSTOM_VALUE` |
| `max_element_substituted_bytes` | 8 MiB | is the guaranteed-invalid value, once the element's values together have that many bytes (reading a value costs its bytes, a sixteenth of a step each) |
| `max_custom_properties` | 65,536 | `GTEXT_CSS_E_LIMIT_CUSTOM_PROPERTIES` |
| `max_substitution_depth` | 100,000 frames | a custom property past it (a link of a chain is three frames, so about 33,000 links) is the guaranteed-invalid value, as are those made of it; functions nested past it, `GTEXT_CSS_E_LIMIT_DEPTH` |

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A few bytes of CSS can name a value of any size: each of these thirty custom properties is two copies of the one before. The values are token ropes,
   so building them costs what was written and not what they name, and a value that would have more tokens than the budget allows is the guaranteed-invalid
   value, as are all that are made of it (the bytes of a value, 1 MiB, are past the budget before its tokens are). The result of the computation counts what happened. */
static void variables_budget(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);
  GTEXT_CSS_Declaration_Block * block = NULL;
  gtext_css_block_new(registry, NULL, &block);
  gtext_css_block_add_text(block, "--v0", 4, "lol", 3, false, NULL, NULL, NULL);
  for (int i = 1; i <= 30; i++) {
    char name[16], value[48];
    int n = snprintf(name, sizeof name, "--v%d", i);
    int m = snprintf(value, sizeof value, "var(--v%d) var(--v%d)", i - 1, i - 1);
    gtext_css_block_add_text(block, name, (size_t)n, value, (size_t)m, false, NULL, NULL, NULL);
  }
  GTEXT_CSS_Computed_Style * style = NULL;
  GTEXT_CSS_Compute_Result result;
  gtext_css_compute_style(context, block, NULL, NULL, &style, &result);

  const GTEXT_CSS_Custom_Value * value = NULL;
  printf("--v18 %s\n", gtext_css_computed_custom_find(style, "--v18", 5, &value) ? "has a value" : "is guaranteed-invalid");
  printf("--v19 %s\n", gtext_css_computed_custom_find(style, "--v19", 5, &value) ? "has a value" : "is guaranteed-invalid");
  printf("--v30 %s\n", gtext_css_computed_custom_find(style, "--v30", 5, &value) ? "has a value" : "is guaranteed-invalid");
  printf("values stopped by the budget: %llu\n", (unsigned long long)result.substitution_limited);
  printf("work under 200000: %s\n", result.work < 200000 ? "yes" : "no");

  gtext_css_computed_free(style);
  gtext_css_block_free(block);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
```

prints

```
--v18 has a value
--v19 is guaranteed-invalid
--v30 is guaranteed-invalid
values stopped by the budget: 1
work under 200000: yes
```

`GTEXT_CSS_Compute_Result` reports `substituted_tokens` (what the values would have been, summed), `substitution_peak` (the longest one built) and
`substitution_limited`, and the quadratic suite asserts on them and on `work`, not on a clock: chains, fan-out, deep fallbacks, cycles and the doubling shape are
linear in the text written.

## The block API and the cascade

A custom property whose value holds a function is carried as written: `gtext_css_block_add_custom()` and `gtext_css_candidates_add_custom()` both take it, and a
block made by hand and one made from the cascade compute alike. A declaration that is not valid at parse time (an argument grammar not met, a bad string) is not
added, and the declaration before it stands.

## Threads and errors

A compute context is immutable once made; its registrations and the syntaxes it has compiled are shared, and a style is immutable and may be read by any
number of threads. Every allocation goes through the allocator the context was made with: a failure is `GTEXT_CSS_E_OOM` with no style returned and nothing
left behind. Statuses are the computed-value layer's, with the two limits above (`GTEXT_CSS_E_LIMIT_CUSTOM_PROPERTIES`, `GTEXT_CSS_E_LIMIT_CUSTOM_VALUE`); none is `GTEXT_CSS_E_INTERNAL`.

## How it is checked

Every rule is a named test that quotes the draft (`tests/test-css-custom.cpp`, `tests/test-css-property-rule.cpp`); the allocation sweep of `tests/test-css-custom-oom.cpp`
fails the n-th allocation of every kind of computation; a libFuzzer harness (`tests/fuzz/fuzz_css_custom.cpp`, with kept seeds replayed by `make test`) checks that a style
computed twice is the same, that a computed value is a fixed point, that the counters keep their bounds, that the block API and the cascade agree and that a failed
allocation is `OOM`; and `make conformance-css-variables` scores the pages of web-platform-tests' `css-variables` and `css-properties-values-api` that are data:
the registrations (`CSS.registerProperty()` accepting and refusing syntax strings and initial values), the reftests of `css-variables` (the page and its
reference must compute the same style, run through the cascade of this library), the cycle tests and the two pages that must not crash. The pages it does not read
are listed with the reason in `tools/css/UNEXTRACTED_VARIABLES`; the cases it fails are listed, with the sentence of the served draft, in
`tools/conformance/CSS_VARIABLES_EXCEPTIONS`.

## Not here

`@function` and custom functions; `first-valid()`; the animation and transition of custom properties; the used
value of anything; `revert` resolved after a substitution; the `CSS` object of a browser (the registry builder and the `@property` set are what it
would call).
