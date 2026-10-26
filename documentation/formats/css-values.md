@page format_css_values CSS values and the Value Definition Syntax

# CSS values and the Value Definition Syntax

`gtext_css_compile()` reads a grammar **as a CSS specification writes it**
(`[ <length-percentage [0,∞]> | auto ]{1,4}`) and `gtext_css_match()` decides
whether component values match it and what they mean, as one immutable **typed
value** that every later layer shares: keywords as interned atoms, numbers with
units, lists with their separators, functions, and the grammar slot that matched
each of them. Values are written back as CSSOM writes a specified value, and the
text reads back as the same value. This is the engine; the property registry and property-level parsing are built on it
(\ref format_css_registry "css-registry.md"), and computed values, shorthands and logical properties on top of that
(\ref format_css_computed "css-computed.md"). It sits on the syntax layer of \ref
format_css "css.md". Back to \ref text_format_references "Formats".

Include `<ghoti.io/text/css.h>`.

## Quick start

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Compile a property's grammar, match a declaration's value against it, and read the typed result. */
static void grammar_quick_start(void) {
  const char * grammar = "[ <length> | <percentage> | auto ]{1,4}";
  GTEXT_CSS_Compiled * compiled = NULL;
  GTEXT_CSS_Grammar_Result built;
  if (gtext_css_compile(grammar, strlen(grammar), NULL, NULL, NULL, &compiled, &built) != GTEXT_CSS_OK) {
    printf("not compiled: %s %s\n", built.message, built.name);
    return;
  }
  const char * css = "10PX auto 5%";
  GTEXT_CSS_Tree * tree = NULL;
  gtext_css_parse_component_values(css, strlen(css), NULL, &tree, NULL);

  bool matched = false;
  GTEXT_CSS_Value_Set * set = NULL;
  gtext_css_match(compiled, tree->values, tree->value_count, NULL, &matched, &set, NULL);
  const GTEXT_CSS_Value * list = gtext_css_value_set_root(set);
  printf("matched: %s, a %s of %zu\n", matched ? "yes" : "no", gtext_css_val_kind_name(list->kind), list->child_count);
  for (size_t i = 0; i < list->child_count; i++) {
    const GTEXT_CSS_Value * v = &list->children[i];
    printf("  %s", gtext_css_val_kind_name(v->kind));
    if (v->kind == GTEXT_CSS_VAL_DIMENSION) {
      printf(" %g %s", v->number, gtext_css_unit_name(v->unit));
    }
    printf("\n");
  }
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_value_buffer(list, NULL, &text, &length);
  printf("written: %s\n", text);
  gtext_css_serialize_buffer_free(NULL, text);

  gtext_css_value_set_free(set);
  gtext_css_tree_free(tree);
  gtext_css_compiled_free(compiled);
}
```

prints

```
matched: yes, a list of 3
  dimension 10 px
  keyword
  percentage
written: 10px auto 5%
```

The grammar is compiled once (it is immutable and may be shared by threads); the
input is the component values a parse made (a declaration's value, an
attribute); the result is a **value set**, which owns every value in it and is
freed in one call. It does not refer to the tree or the grammar it came from, so
it may outlive both.

## Two steps, and who supplies the types

`gtext_css_definition_parse()` reads the grammar text into a definition;
`gtext_css_definition_link()` resolves the types it names, **native types
first and then the caller's resolver**, copies what it needs and returns the
compiled grammar. `gtext_css_compile()` does both. The composite types of the
specifications (`<position>`, `<length-percentage>`, `<color>`, the grammar of a
property for `<'width'>`, a function type such as `<rgb()>`) are grammars of
their own, and are supplied by the resolver, not by code.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The types a grammar names come from the caller: the resolver hands out their definitions. */
typedef struct {
  GTEXT_CSS_Definition * length_percentage;
  GTEXT_CSS_Definition * x;
} grammar_types_known;

static const GTEXT_CSS_Definition * grammar_types_resolve(void * user, GTEXT_CSS_Ref_Kind kind, const char * name,
    size_t length) {
  grammar_types_known * known = (grammar_types_known *)user;
  (void)kind;
  if (length == 17 && memcmp(name, "length-percentage", 17) == 0) {
    return known->length_percentage;
  }
  if (length == 1 && name[0] == 'x') {
    return known->x;
  }
  return NULL;
}

static void grammar_types(void) {
  grammar_types_known known;
  const char * lp = "<length> | <percentage>";
  const char * x = "<x> | a"; /* a type that can match itself without consuming anything */
  gtext_css_definition_parse(lp, strlen(lp), NULL, &known.length_percentage, NULL);
  gtext_css_definition_parse(x, strlen(x), NULL, &known.x, NULL);

  const char * grammars[] = {"<length-percentage [0,\xe2\x88\x9e]>#", "<nosuch> | auto", "<x>"};
  const char * css = "1px, 50%, -1px";
  for (size_t i = 0; i < 3; i++) {
    GTEXT_CSS_Compiled * compiled = NULL;
    GTEXT_CSS_Grammar_Result result;
    const GTEXT_CSS_Status status = gtext_css_compile(grammars[i], strlen(grammars[i]), grammar_types_resolve, &known,
        NULL, &compiled, &result);
    if (status != GTEXT_CSS_OK) {
      printf("%s: %s\n", result.name, result.message);
      continue;
    }
    GTEXT_CSS_Tree * tree = NULL;
    gtext_css_parse_component_values(css, strlen(css), NULL, &tree, NULL);
    bool matched = true;
    gtext_css_match(compiled, tree->values, tree->value_count, NULL, &matched, NULL, NULL);
    printf("\"%s\" against %s: %s\n", css, grammars[i], matched ? "matches" : "does not match");
    gtext_css_tree_free(tree);
    gtext_css_compiled_free(compiled);
  }
  gtext_css_definition_free(known.length_percentage);
  gtext_css_definition_free(known.x);
}
```

prints

```
"1px, 50%, -1px" against <length-percentage [0,∞]>#: does not match
<nosuch>: a type or property the definition names is not known
<x>: a definition recurses through itself without consuming input
```

Linking refuses what cannot work: a reference nothing resolves
(`GTEXT_CSS_E_REFERENCE`, naming it), and **left recursion**, a type that can
match itself without consuming a component value, whether directly, through
another type, or behind a prefix that can be empty (`GTEXT_CSS_E_LEFT_RECURSION`,
naming a reference on the cycle). Recursion that consumes first (`<x> = a <x>?`,
or through a function's arguments) is fine.

## The notation

| Notation | Meaning |
|---|---|
| `a b` | all, in order |
| `a && b` | all, in any order (at most 64 members) |
| `a \|\| b` | one or more, in any order (at most 64 members) |
| `a \| b` | one of them; tried in written order |
| `[ ... ]` | a group |
| `?` `*` `+` | zero or one, zero or more, one or more |
| `#` | one or more, separated by commas |
| `{A}` `{A,B}` `{A,}` `#{A,B}` | counts (at most 65,535) |
| `[ ... ]!` | the group must produce at least one value |
| `<number [0,∞]>` `<length [0px,1in]>` | a range, inclusive; bounds `∞`, `-∞` (also with U+2212) or a number with a unit that is the type's own (`<number [0px,10px]>` is an error) |
| `<foo>` `<'width'>` `<foo()>` | a type, a property's grammar, a function type |
| `name( ... )` `{ ... }` `( ... )` | function and block notation |
| `,` `/` `:` `;` `=` `'c'` | a literal token (`'c'` is one well-formed code point; the draft has no escape for a quote or a backslash) |
| `'[' ... ']'` | a square-bracket block of the input (`<line-names> = '[' <custom-ident>* ']'`); the unquoted brackets group the grammar |

Precedence is the draft's: juxtaposition binds tighter than `&&`, then `||`,
then `|`. White space and comments between components are ignored when matching.
Keywords match ASCII case-insensitively.

A multiplier on a multiplier is two nodes, as written, and the typed value has the shape the
grammar spells (`[ x* ]+` is a list of lists); the search still does it in linear work.

The native types are `<integer> <number> <percentage> <dimension> <length>
<angle> <time> <frequency> <resolution> <flex> <ratio> <zero> <string> <url>
<ident> <custom-ident> <dashed-ident> <any-value> <declaration-value> <urange>
<an+b> <declaration-list> <rule-list>` and every `<*-token>`.

## The typed value

A value is immutable and owned by a value set. A match produces at most one value
for a node of the grammar:

- a terminal is one value (keyword, custom-ident, dashed-ident, string, url, number,
  integer, percentage, dimension, ratio, zero, unicode-range, a preserved token,
  or an opaque run of tokens for `<any-value>`);
- a juxtaposition is a **list** (space separator) of what its present items
  produced; an `a / b` or `a , b` whose segments cannot be empty is a list with the
  slash or comma separator, and a separator that cannot be attached to a segment
  stays a token, so that the text always reads back;
- `||` and `&&` are lists of the present members **in grammar order**, whatever
  order the input had (`a || b || c` on `c a` is `a c`);
- a multiplier with a largest count above one is a list (comma separated for `#`);
- a function or block is a value with the values of its arguments as children.

Every value records the grammar slot that matched it (`gtext_css_compiled_slot_text()`
returns the grammar text of the slot), the component value and span it was read from,
and an interpolation class (number, length, percentage, angle, time, color
(reserved), list, discrete), so that animation can be added without changing the
representation.

### Decisions the standard leaves open

- **Among parses**: alternatives in written order; the earlier items of a
  juxtaposition take as much as they can; the first shortest path through a
  repetition. The same input always gives the same value.
- **`0`** matches `<length>` as `0px` (Values 4, 6.2) and not `<angle>`, `<time>` or
  `<percentage>`. A bare non-zero number matches `<length>` only under the
  `unitless_length` option (HTML's quirk); which properties enable it is the
  property layer's decision.
- **A literal out of its range does not match.** A math function (`calc()`,
  `min()`, ..., `sign()`) matches **by name** every type the draft lets it stand for
  (`<length> <frequency> <angle> <time> <flex> <resolution> <percentage> <number> <integer>`,
  and a `<ratio>`'s numbers; not `<dimension>` or `<zero>`) and is checked and
  clamped by the math layer; the value is a function with the flag
  `GTEXT_CSS_VFLAG_MATH` and the slot's range. A relative length (`em`, `vw`)
  has no value yet, so it is in a range when some value of its sign is.
- **An arbitrary substitution function** anywhere in the input (`var()`, `env()`, `attr()`,
  `if()`, `inherit()`, `ident()`, `random-item()`, the ones the served Values 5, Variables 1
  and Environment drafts call so; a function with a dashed name is not among them) makes the
  whole value **pending substitution**, valid at parse time as CSS Variables requires, and
  `gtext_css_has_arbitrary_substitution()` reports it.
- **A range on a composite type** restricts the numeric terminals in its definition that
  its bounds fit and leaves the rest alone; the draft says nothing about it. Two ranges that
  leave nothing are refused when linked.
- **`<any-value>` and `<declaration-value>`** do not contain a bad string, a bad url or an
  unmatched closing token anywhere, inside functions and blocks too (Variables 1, 2.1); only
  the semicolon and `!` restriction is for the top level.
- **A `<ratio>`** is `<number [0,∞]> [ / <number [0,∞]> ]?` and "is always serialized with
  both components"; a component may be a math function.
- **`<custom-ident>`** is any identifier except the CSS-wide keywords and `default`;
  **`<urange>`** is a unicode-range token, which only a `unicode-range`
  declaration's value has; **`<whitespace-token>`** consumes nothing and succeeds
  where white space precedes the next component value; **`<an+b>`** is read by the
  syntax layer's An+B and its value is the canonical text.
- A serialized **unit** is lower case except `Q`, `Hz` and `kHz`; the draft says only
  that unit identifiers are ASCII case-insensitive, so the spelling is this
  library's decision, written in `css_units.c`.

## Units

Every unit of Values 4 and 5 is known by name in any case, grouped by type. An
absolute unit converts to the canonical unit by the draft's fixed ratios in IEEE
double (`value * numerator / denominator`, the same on every host); a relative unit
is never resolved here.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Units: looked up by name in any case, converted when their ratio is fixed, left alone when it is not. */
static void values_units(void) {
  const char * names[] = {"IN", "mm", "Q", "turn", "Khz", "em", "vmax", "parsec"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    const GTEXT_CSS_Unit unit = gtext_css_unit_lookup(names[i], strlen(names[i]));
    if (unit == GTEXT_CSS_UNIT_NONE) {
      printf("%s: not a unit\n", names[i]);
      continue;
    }
    double canonical = 0;
    if (gtext_css_unit_to_canonical(unit, 1.0, &canonical)) {
      printf("1%s is %g%s\n", gtext_css_unit_name(unit), canonical, gtext_css_unit_name(gtext_css_unit_canonical(unit)));
    }
    else {
      printf("1%s is relative: the environment decides\n", gtext_css_unit_name(unit));
    }
  }
}
```

prints

```
1in is 96px
1mm is 3.77953px
1Q is 0.944882px
1turn is 360deg
1kHz is 1000Hz
1em is relative: the environment decides
1vmax is relative: the environment decides
parsec: not a unit
```

## Writing a value

`gtext_css_serialize_value()` writes a specified value as CSSOM does: a number by
the shortest digits that round to it, laid out in base ten with at most six
decimals; identifiers and strings through the serializers of the syntax layer;
`url("...")`; `", "` and `" / "` between list items. **Writing, matching the text
again and writing again gives the same text.**

## Limits

A match is bounded in work (`max_match_work`) and in memory (`max_memo_entries`,
which counts each remembered answer, each interval of one, and each pending
frame, so the depth of the evaluation is bounded too). **A bound that is reached is
`GTEXT_CSS_E_LIMIT_WORK`, never "no match"**: the answer is not known. A grammar is
bounded in text (`max_definition_bytes`, which also bounds the texts of all the
definitions a grammar is linked from), nesting (`max_definition_depth`) and nodes
(`max_definition_nodes`, which also bounds the linked whole); a limit above what a
32-bit offset or index can hold (2^30 bytes, 2^28 nodes, 2^20 depth, 2^30 memo
entries) is that maximum; an `&&` or `||` group has at most 64 members
(`GTEXT_CSS_E_LIMIT_GROUP`); an atom table has its own limits
(`gtext_css_atom_table_limits()`, `GTEXT_CSS_E_LIMIT_ATOMS`); the text of one opaque
value is bounded by `max_text_bytes` (`GTEXT_CSS_E_LIMIT_OUTPUT`). Each is its own status, tested at the bound and one past.
Nothing in reading, linking, matching or writing recurses.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A match is bounded in work and memory, and a bound that is reached is a status, never "no match". */
static void values_limits(void) {
  GTEXT_CSS_Compiled * compiled = NULL;
  const char * grammar = "[ [ a? b? ]* ]+";
  gtext_css_compile(grammar, strlen(grammar), NULL, NULL, NULL, &compiled, NULL);
  char css[4000];
  size_t n = 0;
  for (int i = 0; i < 1000; i++) {
    n += (size_t)snprintf(css + n, sizeof css - n, "a ");
  }
  GTEXT_CSS_Tree * tree = NULL;
  gtext_css_parse_component_values(css, n, NULL, &tree, NULL);

  GTEXT_CSS_Match_Options options = gtext_css_match_options_default();
  options.max_match_work = 100;
  bool matched = false;
  GTEXT_CSS_Match_Result result;
  GTEXT_CSS_Status status = gtext_css_match(compiled, tree->values, tree->value_count, &options, &matched, NULL, &result);
  printf("with 100 units of work: %s\n", gtext_css_status_string(status));

  options.max_match_work = 0; /* the default */
  status = gtext_css_match(compiled, tree->values, tree->value_count, &options, &matched, NULL, &result);
  printf("by default: %s, matched %s, %zu positions\n", gtext_css_status_string(status), matched ? "yes" : "no", result.positions);
  printf("work for 1000 tokens: %zu units\n", result.work);
  gtext_css_tree_free(tree);
  gtext_css_compiled_free(compiled);
}
```

prints

```
with 100 units of work: match work or memo entries beyond max_match_work or max_memo_entries
by default: success, matched yes, 1001 positions
work for 1000 tokens: 46036 units
```

## Math functions

`calc()`, `min()`, `max()`, `clamp()`, `round()`, `mod()`, `rem()`, the trigonometric,
exponential and sign functions, and the constants `e`, `pi`, `infinity`, `-infinity`
and `NaN` are read by `gtext_css_math_parse()` into an immutable tree typed by the
draft's type system: integer exponents over length, angle, time, frequency,
resolution and flex, plus a percent and its hint. A node of an impossible type
(`1px * 1px` as a length, `10 / 3px`, `min(1px, 1deg)`) makes the whole function
invalid, not an error: the status is OK and `*valid` is false, so a property can try
its next alternative. The tree is simplified as it is built, as Values 4, 10.10
says: numeric leaves of one unit combine, absolute units become the canonical unit,
the terms of a sum are sorted (a number, a percentage, then dimensions by unit name),
and a function whose arguments are all known is replaced by its result. An infinite
or NaN result is kept, as `calc(infinity * 1px)`, and clamped only by
`gtext_css_math_clamp()`, which a property uses at computed time with its own range.
`mod()` and `rem()` are exact (the remainder of `rem()` is never rounded, and `mod()`
is below the modulus in magnitude even when adding it rounds to it), and `progress()` is
`progress(no-clamp? <calc-sum>, <calc-sum>, <calc-sum>)` as Values 5 writes it. The
tree-counting functions `sibling-index()` and `sibling-count()` take no arguments and stay
unresolved. Every
transcendental function goes through the `math` library, never libm
(`make check-css-libm` fails if a source includes `<math.h>`).

A calculation, read and written:

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Math functions: read as a typed tree, simplified as the draft says, and written as a specified value is. */
static void values_math(void) {
  const char * inputs[] = {"calc(1px + 2px)", "calc(20px + 2em)", "calc(10% - 5px)", "min(20px, 10%, 3em)",
      "calc(2 * 3deg)", "calc(1px * 2px)", "calc(infinity * 1px)", "mod(-7px, 3px)", "round(up, 7px, 2px)"};
  for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
    GTEXT_CSS_Tree * tree = NULL;
    if (gtext_css_parse_component_values(inputs[i], strlen(inputs[i]), NULL, &tree, NULL) != GTEXT_CSS_OK) {
      continue;
    }
    GTEXT_CSS_Math * math = NULL;
    bool valid = false;
    if (gtext_css_math_parse(&tree->values[0], NULL, NULL, &valid, &math, NULL) == GTEXT_CSS_OK && valid) {
      char * text = NULL;
      size_t length = 0;
      if (gtext_css_math_serialize_buffer(math, NULL, &text, &length) == GTEXT_CSS_OK) {
        printf("%s -> %s\n", inputs[i], text);
        gtext_css_serialize_buffer_free(NULL, text);
      }
    }
    else {
      printf("%s -> not a valid calculation\n", inputs[i]);
    }
    gtext_css_math_free(math);
    gtext_css_tree_free(tree);
  }
}
```

prints

```
calc(1px + 2px) -> calc(3px)
calc(20px + 2em) -> calc(2em + 20px)
calc(10% - 5px) -> calc(10% - 5px)
min(20px, 10%, 3em) -> min(20px, 10%, 3em)
calc(2 * 3deg) -> calc(6deg)
calc(1px * 2px) -> not a valid calculation
calc(infinity * 1px) -> calc(infinity * 1px)
mod(-7px, 3px) -> calc(2px)
round(up, 7px, 2px) -> calc(8px)
```

`gtext_css_math_numeric()` says whether the whole calculation came to one number and
which, and `gtext_css_math_resolve()` resolves a calculation against an environment
(`GTEXT_CSS_Math_Env`: the size of a font unit, a percentage basis). Nesting is a
limit (`max_depth`, default 256: `GTEXT_CSS_E_LIMIT_DEPTH`, with the place in the
result), as is the number of nodes (`max_nodes`, default 65,536:
`GTEXT_CSS_E_LIMIT_MATH_NODES`); nothing recurses, so a deep input costs time and
memory, not stack.

In a grammar, `<length>`, `<length-percentage>`, `<angle>`, `<time>`, `<frequency>`,
`<resolution>`, `<flex>`, `<number>`, `<integer>`, `<percentage>` and the ranges
attached to them accept a math function by its category, and the value holds the tree
(`GTEXT_CSS_VFLAG_MATH`); a percentage written inside it sets
`GTEXT_CSS_VFLAG_MATH_PERCENT`, so a slot that does not take a percentage refuses it.

## Colors

`gtext_css_color_parse()` reads one component value as a `<color>`: the named colors
and the system colors, `transparent`, `currentcolor`, the hexadecimal forms, `rgb()`,
`rgba()`, `hsl()`, `hsla()`, `hwb()`, `lab()`, `lch()`, `oklab()`, `oklch()`,
`color()` with the predefined spaces and a custom one, `device-cmyk()`, `color-mix()`,
`light-dark()` and `contrast-color()`, and the relative color syntax
(`rgb(from red r g b / 0.5)`). The channels are kept as written (a number, a
percentage, an angle, `none`, a symbol of a relative color, or a math function), and
the serialization is the **declared value** of Color 4 and 5: the legacy `rgb()` and
`rgba()` form for an sRGB color without `none` (an 8-bit alpha as Color 4, 16.1.1 writes it: the
integer percentage whose 2.55 multiple rounds to it, divided by 100, otherwise the alpha over 0.255 rounded and divided by 1000), `color(srgb ...)` when a channel is `none`,
the reference ranges and clamps of each function, hue normalization, the percentages
of `color-mix()` made to add up, and a relative color as it was written. Nothing is
converted to another space or resolved against a context here: `gtext_css_color_srgb()`
answers only for the colors that are one sRGB value.

The declared value of some colors:

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Colors: the declared value, as CSS Color 4 and 5 serialize it. */
static void values_color(void) {
  const char * inputs[] = {"RED", "#ff000080", "rgb(255 0 0 / 50%)", "hsl(120deg 100% 50%)", "rgb(none 0 0)",
      "oklch(0.5 0.1 120deg)", "color-mix(in srgb, red 30%, blue)", "rgb(from red r g b / 0.5)", "rgb(1 2)"};
  for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
    GTEXT_CSS_Tree * tree = NULL;
    if (gtext_css_parse_component_values(inputs[i], strlen(inputs[i]), NULL, &tree, NULL) != GTEXT_CSS_OK) {
      continue;
    }
    GTEXT_CSS_Color * color = NULL;
    bool valid = false;
    if (gtext_css_color_parse(&tree->values[0], NULL, &valid, &color, NULL) == GTEXT_CSS_OK && valid) {
      char * text = NULL;
      size_t length = 0;
      if (gtext_css_color_serialize_buffer(color, NULL, &text, &length) == GTEXT_CSS_OK) {
        printf("%s -> %s\n", inputs[i], text);
        gtext_css_serialize_buffer_free(NULL, text);
      }
    }
    else {
      printf("%s -> not a color\n", inputs[i]);
    }
    gtext_css_color_free(color);
    gtext_css_tree_free(tree);
  }
}
```

prints

```
RED -> red
#ff000080 -> rgba(255, 0, 0, 0.5)
rgb(255 0 0 / 50%) -> rgba(255, 0, 0, 0.5)
hsl(120deg 100% 50%) -> rgb(0, 255, 0)
rgb(none 0 0) -> color(srgb none 0 0)
oklch(0.5 0.1 120deg) -> oklch(0.5 0.1 120)
color-mix(in srgb, red 30%, blue) -> color-mix(in srgb, red 30%, blue 70%)
rgb(from red r g b / 0.5) -> rgb(from red r g b / 0.5)
rgb(1 2) -> not a color
```

Colors nest in colors (`color-mix()`, `light-dark()`, the origin of a relative
color): `max_nesting`, default 32 and at most 4096, is a limit
(`GTEXT_CSS_E_LIMIT_COLOR_NESTING`), and the math functions inside a channel have the
limits of the previous section.

## Function types

`gtext_css_function_types_new()` makes the definitions of `<image>`, `<gradient>`,
`<transform-list>` and its functions, `<easing-function>`, `<basic-shape>`, `<color>`
and the substitution functions (`var()`, `env()`, `attr()`, `if()`, `random()`,
`sibling-index()` ...), registered into the grammar engine through
`gtext_css_function_types_resolve()` as the resolver passed to `gtext_css_compile()`.
A caller may supply a fallback resolver for its own types. The matched value is a
tree of typed values; typed views read it: `gtext_css_easing_from_value()` (with the
draft's fixups: `linear()` inputs are made not to decrease, `steps()` needs a count of
at least 1, or 2 with `jump-none`, and an invalid one is `*valid` false),
`gtext_css_gradient_from_value()`, `gtext_css_image_kind()`,
`gtext_css_image_set_options()`, `gtext_css_transform_function_from_value()` and
`gtext_css_shape_from_value()`. A `url()` is kept as written and not resolved; a value
with `var()`, `env()` or `attr()` in it matches and stays pending.

**One rule for every view.** A view returns `GTEXT_CSS_E_INVALID` only for a call with a
NULL argument and `GTEXT_CSS_E_OOM` when memory runs out. A value that is not what the
view reads (another function, a keyword, a `steps(0)`, a gradient with a stop that has
neither color nor position) is `GTEXT_CSS_OK` with `*valid` false, and the result
(`*out`, the option array and its count, the struct) is NULL, 0 or zeroed whenever the
view did not make one, so a caller may test either. Every view takes the allocator of
its result; a failure leaves nothing allocated. A transform argument that is a length is
in the unit it was written in, or in the unit of the one value a calculation came to
(`translateX(calc(1em + 2em))` is 3 `em`); one that needs an environment is
`GTEXT_CSS_TARG_MATH` with its `source`. The shape view gives the kind, the fill rule,
`round` and the string of `path()`; the lengths and positions are in the matched value
itself, typed by the grammar. A gradient borrows the matched value (`g->value`), which
must outlive it.

`gtext_css_function_types_resolve()` makes each definition the first time it is asked
for. If that fails for lack of memory the resolver returns NULL, `gtext_css_function_types_status()`
says `GTEXT_CSS_E_OOM`, and nothing is remembered as bad: asking again tries again.
`gtext_css_function_types_prepare()` makes them all, after which the object does not
change and any number of links may use it at once.

`gtext_css_function_family()` is a closed list: the functions this layer defines types for,
plus the math and color functions. The functions of other modules (the filter functions,
`anchor()`, `ray()`, `minmax()`, the prefixed gradients) are NONE.

An easing function, matched and read:

```c
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_function.h>
#include <stdio.h>
#include <string.h>

/* The function types: a grammar names them (<image>, <easing-function> ...), and a view reads the matched value. */
static void values_functions(void) {
  GTEXT_CSS_Function_Types * types = NULL;
  if (gtext_css_function_types_new(NULL, &types) != GTEXT_CSS_OK) {
    return;
  }
  GTEXT_CSS_Compiled * easing = NULL;
  if (gtext_css_compile("<easing-function>", 17, gtext_css_function_types_resolve, types, NULL, &easing, NULL) == GTEXT_CSS_OK) {
    const char * inputs[] = {"ease-in-out", "steps(4, jump-start)", "linear(0, 1 50%, 0 25%, 1)", "cubic-bezier(0.1, 0.7, 1, 0.1)", "steps(0)"};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
      GTEXT_CSS_Tree * tree = NULL;
      if (gtext_css_parse_component_values(inputs[i], strlen(inputs[i]), NULL, &tree, NULL) != GTEXT_CSS_OK) {
        continue;
      }
      bool matched = false;
      GTEXT_CSS_Value_Set * set = NULL;
      if (gtext_css_match(easing, tree->values, tree->value_count, NULL, &matched, &set, NULL) == GTEXT_CSS_OK && matched) {
        GTEXT_CSS_Easing * e = NULL;
        bool valid = false;
        if (gtext_css_easing_from_value(gtext_css_value_set_root(set), NULL, &valid, &e) == GTEXT_CSS_OK && valid) {
          char * text = NULL;
          size_t length = 0;
          if (gtext_css_easing_serialize_buffer(e, NULL, &text, &length) == GTEXT_CSS_OK) {
            printf("%s -> %s\n", inputs[i], text);
            gtext_css_serialize_buffer_free(NULL, text);
          }
        }
        else {
          printf("%s -> matches, but is not a valid easing function\n", inputs[i]);
        }
        gtext_css_easing_free(e);
      }
      else {
        printf("%s -> does not match\n", inputs[i]);
      }
      gtext_css_value_set_free(set);
      gtext_css_tree_free(tree);
    }
  }
  gtext_css_compiled_free(easing);
  gtext_css_function_types_free(types);
}
```

prints

```
ease-in-out -> ease-in-out
steps(4, jump-start) -> steps(4, jump-start)
linear(0, 1 50%, 0 25%, 1) -> linear(0, 1 50%, 0 50%, 1)
cubic-bezier(0.1, 0.7, 1, 0.1) -> cubic-bezier(0.1, 0.7, 1, 0.1)
steps(0) -> matches, but is not a valid easing function
```

The gradient has at most `max_stops` stops and hints (default 65,536:
`GTEXT_CSS_E_LIMIT_STOPS`); the functions packed into the value of one match are
bounded by `max_function_nodes` (default 1,048,576, 0 selects it; one more is
`GTEXT_CSS_E_LIMIT_FUNCTION_NODES`, and the match then has no value).

## Cost

Matching is a search with memoization on the grammar node, the position and, for
`&&` and `||`, the members used, and every step, every position of a set it looks at
and every step of reading the typed value off it is a unit of `max_match_work`, so
the bound bounds the CPU.

- **Linear** in the size of a math function or a color, in the quadratic-nesting suite
  at 4.00 for 4 times the input: a sum or a product of many terms, parentheses and
  `min()` nested to the depth, a sum nested to the left, `color-mix()`, a relative
  color and `light-dark()` nested to the limit, and a gradient, a transform list,
  `linear()`, `polygon()` and an image list of many items.
- **Linear** in the number of component values: a repetition of a repetition
  (`[ [ a? b? ]* ]+`), several repetitions in a row (`a* a* a* b`), an alternation
  whose alternatives overlap (`[ a | a a ]+ b`), a comma list, recursion through
  functions, one `<any-value>`, and the link of a long chain of types that can match
  nothing. The quadratic-nesting suite holds these to 4.00 for 4 times the input.
- **Quadratic**, finite and cut by the bound: an item applied one position at a time to
  the long run of positions an enclosing repetition reaches (`[ a+ b? ]+`), an
  `<any-value>` followed by something it can swallow, repeated (`[ x <any-value> y ]#`),
  and two wide runs joined by `&&` or `||` (`<any-value> && <any-value>`).
- **Memory**: the sets of positions and the typed value's arrays are held in addition to
  the memo entries the bound counts. Measured on inputs of 4000 tokens built to make
  them large, the most was about 410 bytes for each memo entry and each component value
  of the input, and 28 KB with nothing in it.

## Using it as the syntax layer's hook

`gtext_css_matcher_grammar()` fills the `GTEXT_CSS_Grammar` hook of
`gtext_css_parse_grammar()` and the list forms; the matcher keeps the value set of
each group that matched (`gtext_css_matcher_value_set()`), and the status of a
failed match (a limit, memory), since the hook can answer only yes or no.

## Normative references

- **CSS Values and Units Module Level 4, Editor's Draft** (served 2026-10-22,
  dated 24 August 2026): the Value Definition Syntax, the units and their ratios,
  `<custom-ident>`, `<dashed-ident>`, `<ratio>`, ranges; Level 5 for the arbitrary
  substitution functions and the container units.
- **CSS Variables Level 1** for substitution, **CSSOM** for serialization.
- Oracles give behavior only; no other implementation's code was read.

## Tested scope

The engine's own tests (notation, types, ranges, resolution, left recursion,
limits at their bound and one past, the hook), the typed value's, the units' and
the serializer's tests with the fixed point, the allocation-failure sweeps, the
grammar-corpus gate (`tools/css/grammar-corpus.txt`, a seed list in the shape of
the specifications' grammars, with a census of every construct and
`make check-css-grammar-plants`, which plants a defect for each construct and shows
the gate fail), the fuzzer `fuzz_css_grammar` with its seeds and six planted
defects, and the quadratic-suite shapes. The value layer adds the math tests (the
type algebra, simplification, serialization, exact `mod()` and `rem()`, the numeric
functions against a family of published values compared in ulps, `tests/data/css-math-values.txt`),
the color tests, the function-type tests, the WPT value gate
(`make conformance-css-values`: the WPT css-values and css-color serialization
vectors, scored, with the failures that remain listed with the draft sentence that
explains each in `tools/conformance/CSS_VALUES_EXCEPTIONS`, and controls that plant
a defect and must fail), the fuzzer `fuzz_css_values` with about 1,700 seeds and five
planted defects (`make check-css-fuzz-plants`), and the no-libm gate. The pinned property data and the
per-property WPT vectors are the registry's.

## Not implemented

Per-property vectors (the registry, \ref format_css_registry "css-registry.md", has the properties); the computed
value of a property and the resolution of its relative units are \ref format_css_computed "css-computed.md"'s, and so is the resolution of `url()` against a base; the
conversion of any color to another space
(color conversion, gamut mapping, interpolation and the computed value of
`color-mix()`, `light-dark()` and a relative color); the computation of
`contrast-color()`; the resolution of `url()`; `sibling-index()`, `random()` and the
like are parsed and kept, never evaluated here.
