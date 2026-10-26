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
      {"width", "-1px"}, {"-moz-nosuch", "1"}, {"-webkit-transform", "rotate(3deg)"}, {"nav-up", "auto"},
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
nav-up: auto -> not supported
```

`nav-up` is the last line because the pin gives its grammar in terms of a type the pin does
not define (`<id>`); the library says so and never guesses (see "What the pin does not
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

**Types nested in something that is not a property** are read as global definitions. The pin
nests `inset()` and `shape()` inside `<basic-shape>`, `<alpha-value>` inside `<color>`, `<page-size>`
inside `size` of `@page`, `type()` inside `@function` and `attr()`, and the command types of `shape()`
one level further. Each is the pin's definition of that name wherever a grammar names it. A definition
the pin repeats (`repeat()` is nested in nine entries) is one entry when the copies agree, and
`tools/css/consolidate.tsv` drops the copy that does not (`image-set()`'s `type( <string> )`, a different
function from `type( <syntax> )`). The library's own function types still answer first for the names
they define in code (the basic shapes, the transforms), so adding these definitions did not change what
any of those parses to; what changed is that `<alpha-value>`, `<page-size>`, `<type()>` and
`<keypress()>` now resolve, and `link-parameters` and `@page size` can be judged.

What is generated: 823 properties (64 of them legacy aliases), 48 at-rules with 110
descriptors (declarations, media and container features, nested at-rules), 709 value
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
one, and a second override of the same name extends the first. (The own name is replaced by the
definition it stands for when the override is registered; a generated property that names the
name, as `margin` names `margin-top` and `flex-basis` names `width`, gets the override.) A
registered shorthand may have shorthands among its longhands, and expands to what they expand to.
Registration is validated: the name (not a CSS-wide keyword, not a `--` name; the result's
`detail` says which), the grammar (parsed and linked against everything the registry knows,
native types and the pin's and the library's function types), and the initial value (it must
match the grammar, and be a concrete value: not a CSS-wide keyword, not a `var()`; `detail` says
which). An alias may not name a custom property, and a shorthand may not name a custom property,
the property it replaces, or one longhand twice. A failure leaves the builder as it was.

Freezing compiles every registered grammar, and, once anything has been overridden, every
property of the pin, so that an override cannot leave a grammar that names it unable to link
unnoticed. A cycle through an override, or any property that no longer links, is refused there
and named: the result names the registered property or the override to blame and `detail` says
which kind of failure it was. **A failed freeze leaves a builder** that keeps every registration;
what the attempt compiled is forgotten (a later registration is taken into account by the next
freeze), and a freeze that failed for memory succeeds when asked again.

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

The cost that is not linear: a registered property whose grammar names the property registered
before it, in a chain, compiles each link with the whole chain below it, because a compiled grammar
carries what it names. That is quadratic in the depth of the chain, in time and in memory, by
construction, and `max_grammar_nodes` (default 2,097,152 nodes in all, about 80 bytes each) bounds it
with its own refusal (`GTEXT_CSS_E_LIMIT_REGISTRY`, `detail` naming the option). Measured on a
32-thread x86-64 build host: a chain of 400 costs 40 ms and 26 MB, 1,000 costs 0.3 s and 132 MB, and
the budget stops a chain at about 1,150 links at under 200 MB. (Without a budget the chain went on to
about 3,900 links, 10 s and 1.9 GB.) The link work is charged to the registry's work counter, so the
quadratic-nesting suite sees it grow with the square of the chain and not faster. A chain of
overrides of one name, which is what a caller who adjusts a property repeatedly writes, is linear
(1,600 overrides: about 40 ms).

## Parsing a declaration

`gtext_css_parse_property()` (component values), `gtext_css_parse_property_text()` (text) and
`gtext_css_parse_property_declaration()` (a declaration of a tree) apply the whole-property
rules, in this order:

1. The name is looked up (case-insensitive, `--*` exact); an alias is parsed as its target. No
   such property: outcome `GTEXT_CSS_PROP_RESULT_UNKNOWN`.
2. **CSS-wide keywords** `initial inherit unset revert revert-layer revert-rule` (the six the
   pin defines) are valid *alone* for every property, shorthands and custom properties
   included: outcome `WIDE`, with the keyword.
3. `all` accepts nothing else (CSS Cascade 4: "It only accepts the CSS-wide keywords", so not even
   a substitution).
4. A value with `var()`, `env()`, `attr()` or another arbitrary substitution function anywhere in
   it is **pending**: valid now, whatever the property's grammar, meaningful after substitution,
   provided each of those functions is itself valid: CSS Values 5 says a value is assumed valid
   at parse time when "all of those functions are themselves syntactically valid according to
   their argument grammars", and `var()`, `env()` and `attr()` take a first argument, so
   `width: var()` and `--x: var()` are `INVALID`.
5. Otherwise the value must match the property's grammar; the result is the typed value in the
   grammar's slots (`VALID`) or `INVALID`. A shorthand's typed value follows the shorthand's
   grammar; expansion into longhands is the cascade's.

The status is separate from the outcome: an invalid value is `GTEXT_CSS_OK` with the outcome
`INVALID`. A property with **no usable grammar** (the pin gives none, names a type nothing
defines, or writes something that is not a Value Definition Syntax definition) cannot be
decided either way and returns `GTEXT_CSS_E_UNSUPPORTED` with the property named in the result;
`gtext_css_registry_property_state()` says why. A limit of the match (`GTEXT_CSS_E_LIMIT_WORK`) is
its own status: the answer is not known.

How long a hostile value takes before that limit is measured, not guessed. On a 32-thread x86-64 build
host a comma list of a million `1px,` items is read in 0.45 s, and a `font-family` of 100,000 names is
matched in 1.6 s (6.3 million units of work); a `font-family` of 300,000 or a million names runs into the
default `max_match_work` and returns `GTEXT_CSS_E_LIMIT_WORK` after about 11.5 s. The work is charged for
every item the match visits, so a caller that takes untrusted CSS and wants an answer sooner sets
`GTEXT_CSS_Property_Options::match` with a lower `max_match_work` (the time to the limit is proportional to it); a
limit reached is never an answer.

**Quirks.** With the option, a unitless number is a `px` length in the 33 properties of CSS
Values 4, Appendix C (`GTEXT_CSS_PROP_QUIRK_UNITLESS`, listed with the sentence in
`tools/css/QUIRKS_UNITLESS.txt`), and not in the properties that include or reference them (the
`background` shorthand) nor inside `calc()` or any other function (`fit-content(10)` is not a length;
`clip: rect(...)` is the one function that Appendix C names).

**Descriptors.** With `GTEXT_CSS_Property_Options::at_rule` (`@font-face`) the name is a
descriptor of that at-rule. A descriptor takes no CSS-wide keyword and no substitution (it is not
a property of an element); a media or container feature is a descriptor of `@media` or
`@container`. A nested at-rule (`@swash`) is syntax, not a declaration. An at-rule that takes
properties as well (`@page`, `@function`: `GTEXT_CSS_At_Rule_Info::properties`) tries the
descriptors first and then the properties, so `@page { size: a4; margin: 1in }` has both. The
legacy name of a descriptor is the descriptor (`font-stretch` of `@font-face` is `font-width`,
Fonts 4: "a font-stretch descriptor exists which is a legacy name alias and functions in the
identical way to the font-width descriptor"). `parse_property_text` reads the `unicode-range`
descriptor through the declaration route, because only there does the tokenizer make its tokens;
a range whose start is after its end, or past U+10FFFF, is invalid.

## Validity callbacks

`gtext_css_property_validity()` fills the `GTEXT_CSS_Validity` of the syntax layer so that a parse
drops the declarations CSS says it must, and reads them again as a rule where CSS Nesting says to.
Inside an at-rule that has declaration descriptors (`@font-face`) the names are its descriptors; in
one that also takes properties (`@page`, `@function`) a name that is no descriptor is judged as a
property. An at-rule nested in a style rule (`a { @media screen { ... } }`) holds properties.
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
against the commit before it (or `CSS_DEPTH_BASE`, a git ref) and a tracked floor (`tools/css/DEPTH_FLOOR`,
which only goes up), and a property missing from the ledger. At this commit 906 of the 933 rows parse
and serialize; the rest are the gaps below. `parses` means some witness matched; the gate also reports how
many did and checks that each reads back as the same typed values, not only the same text: 3,745 of 3,756
witnesses parse and all 3,745 do. The 11 that do not (in `background`, `font-palette`, `initial-letter-align`,
`list-style-type` and `offset`) are witnesses the generator wrote with an empty required part, which are
not valid values.

`make check-css-grammars` compiles every grammar of the pin and fails on one that neither compiles
nor is listed, with its reason, in `tools/css/UNRESOLVED_REFERENCES` (stale entries fail too, and a
floor holds). `make check-css-package` compares the names and syntax of the tables with a second reading
of the same data, the `@webref/css` package at a pinned version and integrity (`tools/css/WEBREF_PACKAGE`,
fetched by `tools/css/fetch.sh`), and fails on a difference that is not classified in
`tools/css/WEBREF_PACKAGE_DIVERGENCES`; it fails, and does not skip, when the package is not there. The 6
classified differences are five texts that differ and one type the pin nests with no grammar that the
package does not list (`<output-value>`); the 37 types nested inside another type's or a descriptor's entry
that the generator did not read are read now, and the package and the tables agree on all of them. `make check-css-registry-controls` shows the gates fail, and `make
check-css-grammar-plants` plants a defect in the library for each requirement.

## What the pin does not say

The pin's own gaps are listed in `tools/css/UNRESOLVED_REFERENCES`, one line each with the grammar that
names it and a reason: types the pin names and does not define (prose only, or no entry: `<decibel>` and
`<semitones>` of Speech, `<id>` of `nav-up`, `<click()>` of `event-trigger`, `<font-src-list>` of
`@font-face src`), the grammars whose text is not a Value Definition Syntax definition, the initial values that
are prose. Nineteen properties cannot be decided because of them (the five legacy `-webkit-box-*` ones, which have
no grammar; `glyph-orientation-vertical`, whose text is not a definition; and thirteen of Speech, Spatial
Navigation and Animation Triggers).

**What the pin lacks and the served drafts give is supplied, one line and one sentence at a time.**
`tools/css/supplement-types.tsv` gives a type the pin names without a grammar the definition the draft writes
(`<timeline-range-name>` of Scroll-driven Animations, `<url-set>` of `cursor`, the `<top>`, `<right>`, `<bottom>` and
`<left>` of `clip`'s `rect()`), each with the sentence it rests on, and fails generation when the pin has a grammar
for the name by then. `tools/css/consolidate.tsv` appends `<light-dark-image>` to `<image>` (Color 5 gives the
function an image form that the pin's `<image>` does not list) and replaces the `<symbolic-counter-style>` of
`<content-list>` by the `<symbolic-glyph>` Content 3 writes; both fail when the pin's text is not the one the
line expects. `tools/css/supplement.tsv` is still only the custom property pattern. These were found by scoring the
property tests of web-platform-tests (`make conformance-css-property`): the properties they made decidable
(`animation-range`, `cursor`, `clip`, `content` ...) were the largest block of its failures.

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
