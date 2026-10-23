@page format_css_values CSS values and the Value Definition Syntax

# CSS values and the Value Definition Syntax

`gtext_css_compile()` reads a grammar **as a CSS specification writes it**
(`[ <length-percentage [0,∞]> | auto ]{1,4}`) and `gtext_css_match()` decides
whether component values match it and what they mean, as one immutable **typed
value** that every later layer shares: keywords as interned atoms, numbers with
units, lists with their separators, functions, and the grammar slot that matched
each of them. Values are written back as CSSOM writes a specified value, and the
text reads back as the same value. This is the engine; the registry of
properties, math functions, colors and computed values are built on it and are
not here yet (see "Not implemented"). It sits on the syntax layer of \ref
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
work for 1000 tokens: 44035 units
```

## Cost

Matching is a search with memoization on the grammar node, the position and, for
`&&` and `||`, the members used, and every step, every position of a set it looks at
and every step of reading the typed value off it is a unit of `max_match_work`, so
the bound bounds the CPU.

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
defects, and the quadratic-suite shapes. The pinned property data and the
per-property WPT vectors are the registry's.

## Not implemented

Math functions as expressions, colors, images, transforms, easing functions and
shapes (the next phase types them; until then they are grammars the caller
supplies); the property registry and property-level parsing; per-property
vectors; computed values and the resolution of any relative unit.
