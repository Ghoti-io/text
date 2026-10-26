@page format_css_registry The CSS property registry and property-level parsing

# The CSS property registry and property-level parsing

No property is unknown to the library. The **registry** holds every property, descriptor,
at-rule, value type, function and selector of the CSS specifications, generated from the
W3C's machine-readable extracts of them and pinned by commit and by the SHA-256 of each
file, with each property's grammar, initial value, inheritance, animation type,
percentage basis, computed value, logical group and shorthand, longhand and legacy-alias
relations; a caller adds its own properties over it and may redefine one of the pin's if
it says so. **Property-level parsing** turns a declaration's name and value into a typed
value (see \ref format_css_values "css-values.md"), or into the reason it is not one.
It sits on the value layer and the syntax layer (\ref format_css "css.md"). Back to \ref
text_format_references "Formats".

Include `<ghoti.io/text/css.h>` (`css/css_registry.h`, `css/css_property.h`).

## Quick start

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Every property of the specifications is in the registry; a declaration's value becomes a typed value, or a reason it is not one. */
static void registry_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);

  const char * declarations[][2] = {
      {"margin-top", "10px"}, {"margin", "1px 2px"}, {"WIDTH", "revert-layer"}, {"margin", "var(--m)"},
      {"width", "-1px"}, {"-moz-nosuch", "1"}, {"-webkit-transform", "rotate(3deg)"}, {"clip", "auto"},
  };
  for (size_t i = 0; i < sizeof declarations / sizeof declarations[0]; i++) {
    const char * name = declarations[i][0];
    const char * value = declarations[i][1];
    GTEXT_CSS_Property_Value result;
    const GTEXT_CSS_Status status =
        gtext_css_parse_property_text(registry, name, strlen(name), value, strlen(value), NULL, &result);
    printf("%s: %s -> ", name, value);
    if (status != GTEXT_CSS_OK) {
      printf("%s\n", gtext_css_status_string(status));
    }
    else if (result.outcome == GTEXT_CSS_PROP_RESULT_INVALID) {
      printf("invalid\n");
    }
    else if (result.outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN) {
      printf("unknown property\n");
    }
    else {
      char * text = NULL;
      size_t length = 0;
      gtext_css_property_value_serialize_buffer(&result, NULL, &text, &length);
      printf("%s %s\n", result.outcome == GTEXT_CSS_PROP_RESULT_WIDE ? "wide keyword" :
                        result.outcome == GTEXT_CSS_PROP_RESULT_PENDING ? "pending" : "valid", text);
      gtext_css_serialize_buffer_free(NULL, text);
    }
    gtext_css_property_value_free(&result);
  }
  gtext_css_registry_free(registry);
}
```

prints

```
margin-top: 10px -> valid 10px
margin: 1px 2px -> valid 1px 2px
WIDTH: revert-layer -> wide keyword revert-layer
margin: var(--m) -> pending var(--m)
width: -1px -> invalid
-moz-nosuch: 1 -> unknown property
-webkit-transform: rotate(3deg) -> valid rotate(3deg)
clip: auto -> not supported
```

`clip` is the last line because the pin gives its grammar in terms of a type the pin does
not define (`<top>`); the library says so and never guesses (see "What the pin does not
say"). A registry is made once and shared; it is immutable and any number of threads may
read it.

## The generated layer

`tools/css/gen-css-tables.py` reads the `curated` branch of `w3c/webref` (commit
`d20af004eff967a9b8e562ea3423c29402a5ce61`, 122 per-series files in `ed/css`, and the
three css-values definition files of `ed/dfns`), after `tools/css/fetch.sh` has checked
the checkout against the commit and `tools/css/WEBREF_SHA256` has checked every file, and
writes `src/css/tables/`: const data with no pointer in it, so the shared library carries
no relocation for it. The tables are committed; `make check-css-tables` regenerates them,
runs the generator twice to show it is deterministic, and diffs, so a hand edit, a
changed pin and a stale table each fail it (`make check-css-registry-controls` shows
each failing). Raising the pin is a deliberate commit that reviews the generated diff.

**Consolidation.** webref has one file for each series of a specification
(`css-fonts-5.json` is level 5 of the series `css-fonts`). A name is resolved once for
each scope (a property by its name, a descriptor by its at-rule and name, a type or
function by its name and, where the pin nests it in a property, by that property). The
base definition is the entry with a grammar in the highest level of its series; the
`newValues` of every entry are appended as alternatives (`auto | <box-size> |
<anchor-size()>`); every other field is the base entry's own and a field it lacks is
absent, not taken from another level. Two series that both give a grammar for one name
are an error. A legacy alias is a name for its target and keeps no data of its own.

**Prose is mapped, not guessed.** The pin's `inherited` and `animationType` are free
text (38 different animation strings, `yes?`, `see individual properties`). Each distinct
string is mapped to a closed enum by `tools/css/normalize.tsv`; an unmapped string fails
generation, so does a line no property uses, and the raw string is kept beside the enum
(`GTEXT_CSS_Property_Info::inherited_raw`). A shorthand whose inheritance the pin leaves
to its longhands has it derived from them (all alike, or `GTEXT_CSS_INHERIT_MIXED`).

**Departures from the pin** are all in `tools/css/consolidate.tsv` (a nested function
whose value is a prototype and not a grammar is dropped; two types the pin defines only
through their keyword values are given those keywords as the grammar) and
`tools/css/supplement.tsv` (the custom property pattern `--*`, which CSS Variables
defines and the curated data does not list). Nothing else of the pin is edited.

What is generated: 823 properties (64 of them legacy aliases), 48 at-rules with 109
descriptors (declarations, media and container features, nested at-rules), 670 value
types and functions, 159 selectors (name and syntax text, for the selector layer), and an
atom table of the 1,100 keywords of every grammar (`gtext_css_registry_keyword_lookup()`).
The units and functions the css-values definitions list are checked against the library
by a test: every unit has its place, and the functions it does not yet evaluate are
listed in the test.

## Looking things up

`gtext_css_registry_find_property()` is ASCII case-insensitive and finds the caller's
registration of a name before the pin's. A name that begins `--` is a custom property:
it is not folded, and if the registry has no property by that name it is the pattern
`--*`. An unlisted vendor prefix is simply not found, and is an *unknown property*. An
alias is found as itself; `gtext_css_registry_resolve_alias()` follows it.
`gtext_css_registry_property_info()` returns a `GTEXT_CSS_Property_Info`: the grammar and
initial value as text, the inheritance and animation enums with the pin's strings, the
applies-to, percentages and computed-value prose, the logical group (and
`gtext_css_registry_logical_group_member()` lists a group), the longhands, the properties
a shorthand resets without listing them (`font` resets `font-kerning`), the names of the
CSSStyleDeclaration attributes, and flags (shorthand, alias, no grammar, quirks, custom,
override, generated). The at-rules, descriptors, types, selectors and keywords have the
same kind of accessors; a type the pin nests in a property is found for that property
(`gtext_css_registry_find_type()`).

## Registering your own

A caller builds a registry in a `GTEXT_CSS_Registry_Builder`, registers properties, aliases and
shorthands, and freezes it. A property has a name, an inheritance, an initial value, a
grammar in the Value Definition Syntax, a computed-value class and an animation type; a shorthand a
list of longhands and a hook that expands it (the cascade layer calls it; the registry stores
and returns it).

**Redefining a property that exists, the pin's or an earlier registration, is refused with
`GTEXT_CSS_E_EXISTS` unless the definition says `override`, and then
`gtext_css_registry_override_at()` reports it.** The grammar of an override may name its own
property (`<'width'> | foo`); that is the definition it replaces, so that an override can extend
one. Registration is validated: the name, the grammar (parsed and linked against everything the
registry knows, native types and the pin's and the library's function types), and the initial
value (it must match the grammar, and be a concrete value: not a CSS-wide keyword, not a
`var()`). A failure leaves the builder as it was. Freezing compiles every registered grammar
again, so that an override made after a grammar that names it cannot leave that grammar
unable to link unnoticed, and a cycle through an override is refused there and named.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A caller registers a property the specifications lack, and redefines one they have, which is refused unless it says so. */
static void registry_caller(void) {
  GTEXT_CSS_Registry_Builder * builder = NULL;
  gtext_css_registry_builder_new(NULL, &builder);

  GTEXT_CSS_Property_Def weight;
  memset(&weight, 0, sizeof weight);
  weight.name = "vector-weight";
  weight.grammar = "<number [0,\xe2\x88\x9e]> | auto";
  weight.initial = "1";
  weight.inherited = true;
  GTEXT_CSS_Registry_Result result;
  printf("vector-weight: %s\n", gtext_css_status_string(gtext_css_registry_register_property(builder, &weight, &result)));

  GTEXT_CSS_Property_Def rule;
  memset(&rule, 0, sizeof rule);
  rule.name = "fill-rule";
  rule.grammar = "nonzero | evenodd | auto";
  rule.initial = "nonzero";
  printf("fill-rule: %s\n", gtext_css_status_string(gtext_css_registry_register_property(builder, &rule, &result)));
  rule.override = true;
  printf("fill-rule, overriding: %s\n", gtext_css_status_string(gtext_css_registry_register_property(builder, &rule, &result)));

  weight.name = "vector-bad";
  weight.initial = "heavy";
  printf("a bad initial value: %s\n", gtext_css_status_string(gtext_css_registry_register_property(builder, &weight, &result)));

  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_freeze(builder, &registry, NULL);
  const char * tests[][2] = {{"vector-weight", "2.5"}, {"vector-weight", "-1"}, {"fill-rule", "auto"}};
  for (size_t i = 0; i < 3; i++) {
    GTEXT_CSS_Property_Value value;
    gtext_css_parse_property_text(registry, tests[i][0], strlen(tests[i][0]), tests[i][1], strlen(tests[i][1]), NULL, &value);
    printf("%s: %s -> %s\n", tests[i][0], tests[i][1], value.outcome == GTEXT_CSS_PROP_RESULT_VALID ? "valid" : "invalid");
    gtext_css_property_value_free(&value);
  }
  GTEXT_CSS_Property_Id by = 0, was = 0;
  for (size_t i = 0; gtext_css_registry_override_at(registry, i, &by, &was); i++) {
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(registry, by, &info);
    printf("override of %s\n", info.name);
  }
  gtext_css_registry_free(registry);
}
```

prints

```
vector-weight: success
fill-rule: a property of that name is already registered and the registration does not override it
fill-rule, overriding: success
a bad initial value: invalid argument
vector-weight: 2.5 -> valid
vector-weight: -1 -> invalid
fill-rule: auto -> valid
override of fill-rule
```

Limits (`GTEXT_CSS_Registry_Options`): registrations (65,536), the length of a name (256), of an
initial value (65,536) and the longhands of a shorthand (1,024), each its own status
(`GTEXT_CSS_E_LIMIT_REGISTRY`), tested at the bound and one past; a grammar is held to the
grammar engine's limits. A name is looked up in a hash table, so many registrations cost no more
each than few (the quadratic-nesting suite holds the counter of the registry to a ratio).

## Parsing a declaration

`gtext_css_parse_property()` (component values), `gtext_css_parse_property_text()` (text) and
`gtext_css_parse_property_declaration()` (a declaration of a tree) apply the whole-property
rules, in this order:

1. The name is looked up (case-insensitive, `--*` exact); an alias is parsed as its target. No
   such property: outcome `GTEXT_CSS_PROP_RESULT_UNKNOWN`.
2. **CSS-wide keywords** `initial inherit unset revert revert-layer revert-rule` (the six the
   pin defines) are valid *alone* for every property, shorthands and custom properties
   included: outcome `WIDE`, with the keyword.
3. `all` accepts nothing else (decided: not even a substitution).
4. A value with `var()`, `env()`, `attr()` or another arbitrary substitution function anywhere in
   it is **pending**: valid now, whatever the property's grammar, meaningful after substitution.
5. Otherwise the value must match the property's grammar; the result is the typed value in the
   grammar's slots (`VALID`) or `INVALID`. A shorthand's typed value follows the shorthand's
   grammar; expansion into longhands is the cascade's.

The status is separate from the outcome: an invalid value is `GTEXT_CSS_OK` with the outcome
`INVALID`. A property with **no usable grammar** (the pin gives none, names a type nothing
defines, or writes something that is not a Value Definition Syntax definition) cannot be
decided either way and returns `GTEXT_CSS_E_UNSUPPORTED` with the property named in the result;
`gtext_css_registry_property_state()` says why. A limit of the match (`GTEXT_CSS_E_LIMIT_WORK`) is
its own status: the answer is not known.

**Quirks.** With the option, a unitless number is a `px` length in the 33 properties of CSS
Values 4, Appendix C (`GTEXT_CSS_PROP_QUIRK_UNITLESS`, listed with the sentence in
`tools/css/QUIRKS_UNITLESS.txt`), and not in the properties that include or reference them (the
`background` shorthand) nor inside `calc()`.

**Descriptors.** With `GTEXT_CSS_Property_Options::at_rule` (`@font-face`) the name is a
descriptor of that at-rule. A descriptor takes no CSS-wide keyword and no substitution (it is not
a property of an element); a media or container feature is a descriptor of `@media` or
`@container`. A nested at-rule (`@swash`) is syntax, not a declaration.

## Validity callbacks

`gtext_css_property_validity()` fills the `GTEXT_CSS_Validity` of the syntax layer so that a parse
drops the declarations CSS says it must, and reads them again as a rule where CSS Nesting says to.
Inside an at-rule that has declaration descriptors (`@font-face`) the names are its descriptors.
A declaration of a property the library cannot decide is kept and counted
(`undecided`), a limit that kept a declaration from being judged is in `status`.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The validity callbacks make a parse drop the declarations that CSS says it must, and the descriptors are those of the at-rule. */
static void registry_validity(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Property_Validity validity;
  gtext_css_property_validity_init(&validity, registry, NULL);

  GTEXT_CSS_Parse_Options options = gtext_css_parse_options_default();
  options.validity = gtext_css_property_validity(&validity);
  const char * css = "@font-face { font-display: swap; width: 1px } a { color: red; font-display: swap; width: -1px; nosuch: 1 }";
  GTEXT_CSS_Tree * tree = NULL;
  gtext_css_parse_stylesheet(css, strlen(css), &options, &tree, NULL);
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_tree_buffer(tree, NULL, &text, &length);
  printf("%s\n", text);
  printf("judged %zu, dropped %zu\n", validity.checked, validity.rejected);
  gtext_css_serialize_buffer_free(NULL, text);
  gtext_css_tree_free(tree);
  gtext_css_registry_free(registry);
}
```

prints

```
@font-face {font-display:swap;}a {color:red;}
judged 6, dropped 4
```

## Compiling is lazy, and thread-safe

A grammar is compiled the first time a property is asked for (a registry asked only for `margin` does
not compile `grid`), once, by whichever thread asks first, under a lock, and published to the
others as an immutable object. `gtext_css_registry_prepare()` compiles all of them (about ten
milliseconds and five megabytes, measured), after which no path locks. A compile that runs out of memory is
not remembered as a bad grammar: asking again with memory succeeds, tested by an allocation-failure
sweep.

A grammar names types in this order: the engine's native types; the type the pin nests in the
property being compiled; the pin's global type; the library's own function types
(`css_function.h`, for `circle()`, the transform functions and the like, which the pin does not
define); and last a type the pin nests in some other property, when they all say the same thing
(`fill-opacity` names `<opacity-value>`, which the pin defines inside `opacity`). A definition of
the pin that could not be made (memory, or its text) is never answered by a later source.

## The depth ledger and the gates

`tools/css/depth.tsv` has a row for every property and descriptor: **parses** (a value derived from the
compiled grammar, and the initial value, is accepted), **serializes** (it writes, and the text parses and
writes the same again), **computes** (filled by the computed-value layer) and **wpt** (filled by the value
vector scoring); `make check-css-depth` refuses a column that went from 1 to 0 against the measurement,
against the committed ledger, and a property missing from the ledger. At this commit 888 of the 932 rows
parse and serialize; the rest are the gaps below.

`make check-css-grammars` compiles every grammar of the pin and fails on one that neither compiles
nor is listed, with its reason, in `tools/css/UNRESOLVED_REFERENCES` (stale entries fail too, and a
floor holds). `make check-css-registry-controls` shows all three gates fail, and `make
check-css-grammar-plants` plants a defect in the library for each requirement.

## What the pin does not say

The pin's own gaps are listed in `tools/css/UNRESOLVED_REFERENCES`, one line each with the grammar that
names it and a reason: 46 types the pin names and does not define (prose only, or no entry:
`<symbolic-counter-style>` of `content`, `<url-set>` of `cursor`, `<top>` of `clip`, `<font-src-list>` of
`@font-face src`, `<alpha-value>`), the grammars whose text is not a Value Definition Syntax definition, the
initial values that are prose. 29 properties cannot be decided because of them. Supplying a missing
definition is a line in `tools/css/supplement.tsv` with the specification sentence it quotes; none is
supplied but the custom property pattern.

Two things the pin gets different from the specifications in use are worth knowing. Fill and Stroke 3's
`<paint>` has no `<color>` (`fill: red` is invalid by the pin; its own initial value `black` does not parse,
which the list records), so `vector` registers SVG 2's `fill` and `stroke` with `override`. And a
prose rule in a grammar (Grid's `<grid-line>` excludes `span` from its `<custom-ident>`) is not in the
grammar; the scored vectors of the next phase find these.

## Normative references

- The data: `w3c/webref`, `curated`, `ed/css` and `ed/dfns/css-values-{3,4,5}.json`.
- CSS Values and Units 4 (the Value Definition Syntax, including Appendix C, "Quirky Lengths", and the
  omissible commas of section 2), CSS Cascade (the CSS-wide keywords), CSS Variables 1 (the custom property
  pattern and substitution), CSS Syntax 3 (declarations and validity).
- Oracles give behavior only; no other implementation's code was read.

## Not implemented

Computed values, shorthand expansion, inheritance and the cascade; `@property` and custom-property
registration; per-property WPT scoring and the differential against a browser; registering a type or a
selector. Each is a later layer; the registry's shapes are made for them.
