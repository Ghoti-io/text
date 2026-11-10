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

A declaration's own value is trimmed of white space at both ends when it is consumed (CSS Syntax 3), and **what substitution makes of it is not trimmed again**: `--a: x var(--empty)`
is `x ` and `--b: var(--empty) x` is ` x` (Chromium and Firefox answer so), and the white space after the value of a branch of an `if()`, up to the `;` or the `)` that ends the
branch, is part of it (`if( style( --x : 3 ) : true_value )` is `true_value `, as the one engine that has `if()` answers), where the white space before the value, after the colon, is
not (Values 5: a free-form production "can either (after optional whitespace) start with" a `{`). A branch of white space only has no value.

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
| `if(cond: value; ... else: value)` | `GTEXT_CSS_Environment::condition` for `media()` and `supports()` (a document's own context has it; `gtext_css_conditions_environment_hook()` makes one for a context of the caller's); `style(--x: v)` reads the custom properties, and `style(--x: initial)`, `inherit` and `unset` compare with the value the keyword gives | `GTEXT_CSS_E_UNSUPPORTED` naming `condition` |
| `ident(...)`, `random-item(key, a, b ...)` | nothing; `random_base` or `random_seed` of the environment decides the pick | |
| `random(...)`, `sibling-index()`, `sibling-count()` | `random_base`, and the element interface's `sibling_index` and `sibling_count` | `GTEXT_CSS_E_UNSUPPORTED` naming the callback |
| `cycle(a, b, c)` | the property's value in the parent: the one after the match, wrapping | |

An `attr()` whose type was written and does not read (`type(<url>)`, which Values 5 makes invalid as a syntax, or a token that is no keyword, unit or `type()`) is a failure, and its result with no fallback is the guaranteed-invalid value; an `attr()` with no type at all has the empty string for it, as "FAILURE: If second arg is null, and syntax was omitted, return an empty CSS <string>" says.

`attr()` is **tainted**: what it makes cannot become a URL ("attr() may not be used to make a URL"), so a `url()` or `image()` made from it is invalid at
computed-value time. A function written directly after `...` (`max(var(--list)...)`) is replaced first and the arguments are divided on what it gave.
An argument grammar that is not met is a parse error: `var(--a,!)`, `var(--a,;)` and `var(--a) !important !important` are invalid declarations, and the one
before them in the cascade stands.

## Registered custom properties and `@property`

A **registration** gives a custom property a syntax, whether it inherits, and an initial value. It changes nothing at parse time, and at computed-value time
its value is checked against the syntax and computed by its type: `2em` as a `<length>` is pixels, a list is computed item by item, `<color>` is a color.
A value that is not of the syntax is invalid at computed-value time, and the property takes its **initial value**, which is the property's own, not the
inherited one. The initial value has to be **computationally independent**: `5px`, `1in` and `light-dark(red, blue)` (computed in the environment's color scheme) are, `3em`, `currentcolor` and
anything with `var()` are not. It is optional, for a typed syntax as for `*`: "If initialValue is not present, let parsed initial value be the guaranteed-invalid value", so a
registered `<length>` that nothing sets has no value (and `var(--x, fallback)` takes its fallback), and a value that is not a `<length>` makes it "invalid at computed-value time", which is that same value. A caller registers through the registry builder before it is frozen, as `CSS.registerProperty()` does:

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
`GTEXT_CSS_Compute_Options::property_rules` computes by them. **Every descriptor of the rule is optional** ("While the <custom-property-name> is required, all of
the descriptors are optional. When omitted, it matches the behavior of an unregistered custom property"): `syntax` is `*`, `inherits` is `true`, and the initial value
is the guaranteed-invalid value, for a typed syntax too. A descriptor that is invalid is ignored and the rule stands (a `syntax` that is no syntax string, an `inherits`
that is not `true` or `false`, an `initial-value` that is not of the syntax); an initial value that is of the syntax and needs the element (`3em`, `currentcolor`,
`var()`) makes the registration "register a custom property" refuses, and the rule is **ignored**, as is a rule with a prelude that is not a custom property name. A prelude may
list names (`@property --width, --height { syntax: "<length>"; inherits: false }` registers both with the same descriptors), and a rule may be written as the **statement**
`@property --size <length>: 16px;` (a `<css-type>`, the syntax of one component or `type(<a> | <b>)`, and after a colon the initial value, which may be empty), which is "exactly
equivalent to the normal rule form" with the other descriptors omitted, so the property inherits; both are a CSSPropertyRule and serialize in the block form. The set keeps a
diagnostic with the name, the reason, the rule's place and the sentence of the draft it applies for everything that was ignored; the last valid rule of a name wins, and so does a registration the caller made over a rule. A rule inside
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
      "@property --gap <length>: 4px;\n"
      "@property --broken { syntax: '<length>'; initial-value: 3em }\n";
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Tree * tree = NULL;
  gtext_css_parse_stylesheet(sheet, strlen(sheet), NULL, &tree, NULL);

  GTEXT_CSS_At_Property_Set * rules = NULL;
  gtext_css_at_property_set_new(registry, NULL, &rules);
  gtext_css_at_property_collect(rules, tree, NULL);
  printf("%zu registered\n", gtext_css_at_property_count(rules));
  for (size_t i = 0; i < gtext_css_at_property_diagnostic_total(rules); i++) {
    GTEXT_CSS_At_Property_Diagnostic d;
    gtext_css_at_property_diagnostic(rules, i, &d);
    printf("ignored %.*s: %s\n", (int)d.name.length, d.name.data, gtext_css_at_property_reason_name(d.reason));
  }
  GTEXT_CSS_Custom_Info info;
  const char * names[] = {"--size", "--gap"};
  for (size_t k = 0; k < 2; k++) {
    if (gtext_css_at_property_find(rules, names[k], strlen(names[k]), &info)) {
      printf("%s: syntax %.*s, initial %.*s\n", names[k], (int)info.syntax_length, info.syntax, (int)info.initial_length, info.initial_value);
    }
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
2 registered
ignored --broken: not-computationally-independent
--size: syntax <length>, initial 16px
--gap: syntax <length>, initial 4px
width: 16px
```

## Private custom properties: `@private`

CSS Functions and Mixins 1 gives a style rule temporary variables that no other rule, no descendant's rule and no script can name by accident: an `@private` rule nested in a style rule (or in a
nested group rule of one) holds custom properties and `@property` rules, and the names of its custom properties are the style rule's **private property names**. Within the style rule and
every rule nested in it, a custom property of such a name "is actually a private property, and any reference to a custom property of that name (for example, with a `var()`) is a reference to the
private property". It is a different property from the custom property of the same name, and "Two private properties with the same name are considered the same property if and only if they have
the same nearest style rule ancestor that has their name in its private property names".

```css
section {
  @private { --size: 2px; }
  &, & > *, & > * > * { border-width: var(--size); }   /* the private --size: 2px on every one of them */
}
section > h1 { --size: 4px; }                            /* the custom property --size: another property */
```

How it is built: the draft's own account is "roughly equivalent to" the rule written with a unique name for the property, and that is the whole mechanism. The sheet compiler (`gtext_css_sheet_compile()`
and the CSSOM document's snapshot) gives each property of a style rule that has the name a hidden custom property name (`--`, U+10FFFD, a code point of private use, the sheet's number, the rule's number and the name, so two rules
and two sheets never share one), writes it where the rule and the rules nested in it declare the property, and writes it in the references of their values: the first argument of `var()` and of
`inherit()` (wherever the function is, in a fallback or in the argument of another function), and the name in a `style()` query of an `if()`. Everything after the compiler handles a custom
property: the cascade orders the declarations by the specificity and place of the rule, an unregistered private property inherits as the tokens, `initial`, `inherit` and the rest of the CSS-wide
keywords apply, and a custom function that is called with `var(--size)` as an argument gets the private property's value. The declarations of an `@private` rule are the style rule's, at their
place among its others.

What is read of an `@private` rule: its custom properties; its conditional group rules (`@media`, `@supports`, `@container`, `@when`, `@else`, `@starting-style`), whose custom properties apply where
the condition holds and have their names whether it does or not; and the `@property` rules directly inside it. Anything else (a property that is not a custom property, a style rule, an `@layer`, an
`@scope`, a second `@private`) is invalid and ignored, and the rule stands. An `@private` rule anywhere else (the top of a sheet, a group rule that is not in a style rule, a `@mixin` body) is
invalid and ignored.

**The private properties are not exposed.** `getComputedStyle(element).getPropertyValue("--size")` is the custom property's value, not the private one's, and the enumeration, the length and
`gtext_css_computed_custom_count()`, `_at()` and `_find()` list and find no private property (the names are a run in the order of the names, so the count and the index are those of the others and
cost no more than a map's rank). The rules' own text is unchanged: `cssRules` shows the `@private` rule as a rule that has no interface (a preserved rule), as before.

**Registering a private property.** "A @property rule inside of a @private rule defines a private property name for the parent style rule, just like using the private property normally does. Its
custom property registration only applies to that private property, not any other of the same name", and "global property registrations do not apply to private properties". The compiled sheet
keeps the registrations of the `@property` rules directly inside its `@private` rules, under the hidden names, and the document's set of `@property` registrations reads them
(`gtext_css_at_property_collect_private()` for a caller that compiles sheets itself); the public view of the set (`gtext_css_at_property_count()` and the rest) is the rules at the top of a sheet,
as before. The statement form (`@property --two <color>: blue;`) is read as it is at the top of a sheet. Of two rules for one name in one `@private` rule the later wins.

**Where the pinned text is silent, and what was chosen** (each is the conservative reading; the sentences are the draft's):

- *Custom functions.* "Issue: What about custom functions? If they refer to a custom property not defined in the function, should they see an outer private property? I'm inclined to say yes". The
  issue is open in the pinned text, so a function body's `var(--x)` is the custom property of the element, never a private one; an argument that names the private property at the call
  (`--f(var(--x))`) passes its value, because the call is in the style rule.
- *`@container` style queries and the `@property` rules in conditional group rules.* The names of an `@private` rule in a group rule count ("including those nested in nested group rules") and its
  custom properties apply where the group's condition holds; a `style()` query of a container query (`@container style(--x: 1)`) asks about the custom property, never a private one; and a
  `@property` rule inside a conditional group rule that is inside `@private` (or inside a group rule of the style rule) is not valid there (the places of `@property` are the top of a sheet, a group rule at the top and directly an `@private` rule) and is dropped when the sheet is read, because a registration is not conditional here.
- *`!important` in an `@private` rule.* A resolution of the CSS Working Group (2026-08-04, w3c/csswg-drafts#14212) makes it invalid; the pinned text does not carry it, and the declaration is read as it is anywhere else.
- *`@layer` and `@scope` inside `@private`.* The draft lists "nested group rules"; the layers and scopes of a sheet are placed by the layer walker, which does not enter `@private`, so they are ignored.
- *The rule's own text.* "It is not exposed to the page via any CSSOM APIs" is read of the computed style (the one place a private property has a value). The declarations the author wrote in a style rule,
  those of the rule itself included (`--size: 4px` where `--size` is private), are the rule's text: its `style` and `cssText` keep them as written, so that a rule reads back as it was.
- *Mixin parameters.* "A mixin's parameters define private properties in the mixin body": see the next section. A mixin body's `@private` rule is the mixin's own application's, not the style
  rule's it is substituted into.

## Mixins: `@mixin`, `@apply` and `@contents`

CSS Functions and Mixins 1 defines a mixin as a body of declarations and nested rules that an `@apply` substitutes into a style rule, "in place of the `@apply` rule itself", with arguments for its
parameters and a block of the invoker's own that an `@contents` in the body puts where it is wanted. "`@apply` rules are processed before any styles are applied, as they effectively modify the
stylesheet itself", so the sheet compiler does it (`gtext_css_sheet_compile()` and the CSSOM document's snapshot), and the cascade, the specificity, the layers, the conditions and everything that
follows handle the result as they handle a style rule that had been written out.

```css
@mixin --card(--pad <length>: 8px, --accent: teal) {
  @private { --shadow: 0 1px 2px var(--accent); }     /* a private property of the application */
  padding: var(--pad);
  box-shadow: var(--shadow);
  & > h2 { color: var(--accent); }                    /* a rule nested in the one the mixin is applied in */
  @media (width < 600px) { @contents; }               /* the block the invoker passed, where it is wanted */
}
.tile  { @apply --card(1em) { font-size: smaller; } }
.tile2 { @apply --card; color: red; }                 /* a declaration after the @apply wins over one of the mixin */
```

**What an `@apply` does.** In a style rule (or in a conditional group rule in one, or in a mixin's body) the `@apply` is replaced by the body of the `@mixin` of that name: the body's declarations are
the style rule's own, in the place the `@apply` was among its other declarations (so a declaration of the rule after it wins over the mixin's and one before it loses, and an important
declaration of the body beats a normal one of the rule), with the specificity of the rule; the nested rules and group rules of the body are nested in the style rule, so `&` is its selector; and an
`@apply` in the body is applied in turn. "If no such mixin exists, the `@apply` does nothing", and "if more arguments are passed than the length of the mixin's argument list, the `@apply`
application does nothing". The CSSOM does not change: `cssRules` shows the `@mixin` and the `@apply` as they were written.

**Parameters.** "A mixin's parameters define private properties in the mixin body, set to the value passed for the argument", and "these are accessible using `var()`, and shadow any same-named
custom properties". Each application of a mixin has its own private names (a hidden custom property for each parameter and for each name of the body's `@private` rules, made as `@private` makes them),
and the parameters are declared on the element the style rule selects, at the place of the `@apply`, before the body: a parameter takes the argument written for it (read in the scope of the `@apply`: a
`var()` in an argument names what the style rule's `var()` would), else its default (read in the mixin's scope, so a default may name an earlier parameter: `--right <color>: var(--left)`). A typed parameter
is registered with its type, "an inherit flag of true, and no initial value", so a typed value is computed where it is declared (`--size <length>` with `.2em` on an element of `font-size: 10px` is `2px`
on the element and every descendant), and an untyped one is the tokens, resolved at each element. A parameter that is not passed and has no default has no value (`var()` of it falls back). The draft's
own examples hold: the element's own `--color2` is not the parameter and is not changed, a private property of the body overrides a parameter of the same name, and a following sibling does not see the
argument ("The .foo element will get `color: green` [...] but its following sibling will get `color: red`"). The parameters are private properties, so `getComputedStyle()` shows none of them. A mixin
sees the private properties of the rule it is substituted into, unless it has a parameter or a `@private` property of the name.

**`@contents`.** "If the `@apply` rule invoking the mixin passed a contents block, the `@contents` is replaced with the contents block [...] Otherwise [...] its fallback block", the statement form
(`@contents;`) has none, "passing an empty block will substitute the empty block", and a mixin may use the block in several places, or in none. The block is a declaration list (a rule in it is not read) and
it is read in the scope of the `@apply`: a private property of the style rule is visible to it and a parameter of the mixin is not. Outside a mixin body an `@contents` is invalid and ignored.

**Which mixin.** "If more than one `@mixin` exists for a given name, then the rule in the stronger cascade layer wins, and rules defined later win within the same layer", and "the name of a `@mixin` rule is a
tree-scoped name": an `@apply` finds the mixins of every sheet of its tree scope that takes part in the cascade, before or after its own sheet, in the order of the cascade (the sheets an `@import`
brings in are read first, at the place of the import), and not those of a shadow tree's sheets or of the document's. A sheet that applies a mixin is compiled again when any sheet of the scope
changes (a rule inserted, removed or changed, a sheet disabled), and only such sheets are; the compiler of a sheet given by itself reads the `@mixin` rules of that sheet. A set of mixins can be given to the
compiler instead (`GTEXT_CSS_Sheet_Options::mixins`, a `GTEXT_CSS_Function_Set` made with `GTEXT_CSS_Function_Options::mixins`; `gtext_css_mixin_find()`).

**Limits.** A mixin that applies itself, directly or through others, is a cycle, which is not an error: the `@apply` that would close it does nothing (the draft is silent; `mixin-cycle.tentative.html` is
written with `@result`). Otherwise the expansion is bounded as the rest of the compile is: `GTEXT_CSS_Sheet_Options::max_mixin_depth` (16, at most 256) is the deepest chain of mixins applied inside
one another and `max_mixin_expansions` (65,536) the most applications in a sheet (the applications inside a body count, so a mixin that applies two others, each two more, is stopped by the count); both are
`GTEXT_CSS_E_LIMIT_RULE_NESTING` and `GTEXT_CSS_E_LIMIT_WORK` with the name of the option in the result. What an expansion reads (each item of a body) is charged to `max_compiled_bytes` as the compiled form is, so an
application of a large body many times cannot run for long either, and `max_rule_depth` bounds the stack of rules the expansion lives on.

**Where the pinned text is silent, and what was chosen** ("Evaluating Mixins" is still marked "TODO rewrite to use the new `@private` concepts" and is written with the dropped `@result`; twenty-two of the
twenty-seven pinned web-platform-tests pages are too, and neither Chromium 156 nor Firefox 158 implements `@mixin`; every reading is the conservative one and has one place to change):

- *A mixin in a conditional group rule* (`@media`, `@supports`, `@container`, `@when`) is not read: where a mixin is applied is decided when the sheet is compiled, before a condition is evaluated. Change: `walk_rule()` of `css_custom_function.c`.
- *An `@apply` directly in an `@scope`* (not in a style rule) does nothing: "only valid in the body of a style rule or nested group rule" and an `@scope` is neither.
- *A mixin of an imported sheet* is read as unlayered, whatever `layer()` the `@import` has, and the conditions of the import are not read.
- *A cycle* does nothing (above); the draft names no behavior.
- *The argument or the default of a typed parameter that is not a value of the type* is declared and is invalid at computed-value time (the registration's rule), so `var()` of it falls back; the draft's "the default value must parse successfully according to that parameter type's syntax" is the check of the `@mixin` rule that has one.
- *A parameter with no argument and no default* has no value. The draft says only that "the missing arguments take their default values".
- *Custom functions and private properties.* A custom function called in a mixin's body does not see the application's private properties (the parameters), as for `@private`: "Issue: What about custom functions? [...] I'm inclined to say yes" is an open issue in the pinned text, and the CSS Working Group's discussion of it (w3c/csswg-drafts#14408) closed with no resolution. The decision is kept open; the place is the call in `css_substitute.c`'s function scope.
- *`@container style()` naming a parameter or a private property* is not built: the editor said yes in w3c/csswg-drafts#14410 with no text and no resolution.
- *Diagnostics* of what a body makes the compile read (a selector that is not valid) are the `@apply`'s: the id and the span are those of the rule of the sheet's own tree.
- *An `@property` in a mixin body's `@private`* registers the application's private property, as `@private` does in a style rule.

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

The evaluation of a custom function (`--name()`: the definitions are read, see above); `first-valid()`; the animation and transition of custom properties; the used
value of anything; `revert` resolved after a substitution; the `CSS` object of a browser (the registry builder and the `@property` set are what it
would call). Of `@property`: an `@property` inside the body of an `@function` or a `@mixin` (the draft does not place one there: a function body "accepts conditional group rules ... the result descriptor ... custom
properties"). The CSSOM of a prelude that lists names follows the CSSWG's resolution of 2026-08-06 (w3c/csswg-drafts#14227): "if single @property has multiple names, it will be represented in CSSOM as a single
object with a `names` array and not expand into multiple objects and return empty string for the `name` in that case". So `@property --a, --b { ... }` is one rule (`cssRules.length` counts it once, `deleteRule()`
removes it whole), its `name` is the empty string, `gtext_cssom_property_get_names()` answers `--a` and `--b` (a rule of one name has an array of one), and its text lists every name, so a round trip keeps them. The
draft this library pins still carries an Issue box that says the question "has not been resolved on by the CSSWG", and its IDL has `name` only; the resolution is ahead of the text (the issue is open, "Needs Edits"), and
the interface ledger marks `names` as such.

## `@function` definitions

`css/css_custom_function.h` reads the `@function` rules of stylesheets into the custom functions of CSS Functions and Mixins 1, the way `css/css_at_property.h` reads
`@property`. A call (`--name(args)`) in a value is evaluated when a style is computed, if the set is given to the compute context (see "Calling a function" below); without one, a call is the
guaranteed-invalid value.

```text
GTEXT_CSS_Function_Set * set = NULL;
gtext_css_function_set_new(registry, NULL, &set);          /* options: the allocator, `active` for @media and @supports, limits */
gtext_css_function_collect(set, tree, NULL);               /* the trees must outlive the set (unless it was made with `own_rules`): the body of a rule is read from them */

GTEXT_CSS_Function_Info info;
if (gtext_css_function_find(set, "--double", 8, &info)) {
  /* info.parameter_count, info.returns, info.has_result, info.rule (the body) */
}
```

A rule is invalid, ignored and reported (`gtext_css_function_diagnostic()`), when the function token does not begin with two dashes (`@function --foo ()` is a name
and a block, not a function), a parameter is not `--name [<type>] [: <default>]`, a name is a parameter twice, a type is not one syntax component (or `type(...)`),
or a default is not a value of its type. A declaration of the body that is neither `result` nor a custom property is ignored and reported, and the rule stands.
Of several rules of one name the one in the stronger cascade layer wins, and the later one within a layer; the loser is reported as `superseded`.

### Calling a function

Give the set to the compute context as `GTEXT_CSS_Compute_Options::function_rules` (borrowed; it must outlive the context and not change while the context is used). A
`<dashed-function>` in a property or in a custom property is then replaced, as `var()` is, by the `result` of the function of that name, evaluated as CSS Functions and Mixins 1 says:
the arguments are replaced in the calling context (a `{ ... }` argument is one argument and its braces are not part of it; one that is the guaranteed-invalid value selects the
parameter's default; a value that is not of the parameter's type is not valid and the default is tried), more arguments than parameters is the guaranteed-invalid value, and then the body
is resolved as a second scope that is a child of the calling context's: `var()` finds a local variable, then a parameter, then a custom property at the call site (and, in a function
called from a function, the callers' locals and parameters). `initial` on a local variable is the parameter's value, `inherit` is `inherit()` of the name, and any other CSS-wide keyword is
the guaranteed-invalid value; on `result` they are left alone. A function that is already being evaluated, however far up, is a cycle: every call in it is the guaranteed-invalid value.
A name no rule defines is the guaranteed-invalid value. A conditional group rule in a body (`@media`, `@supports`, `@container`) applies when the environment says so
(`GTEXT_CSS_Environment::condition_status`, else `condition`; `@container` is asked of the element the function is called on, as if its ancestors were the hypothetical element's). A
body is read when it is called, so the declarations of a body are bounded per call (`max_function_declarations`), as is the depth of calls inside calls (`max_call_depth`, a status
and not a stack: a call is a frame of the same explicit stack as `var()`).

A document (`css_cssom_computed.h`) collects the `@function` rules of its sheets that take part, in cascade order, as it does the `@property` rules, and the compute context it makes is
made again only when the functions change.
