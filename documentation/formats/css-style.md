@page format_css_style Computed style of an element

# Computed style of an element

One call turns an element of the caller's tree into its computed style: the element's **presentation attributes** (SVG 2's `stroke-width="2"`), its `style`
attribute, the declarations of the rules that matched it, and the computed style of the element it inherits from, cascaded and computed. The style it returns
is immutable and reference counted, readable from any number of threads, and has a canonical text and a SHA-256, so that a document dump can embed it. Back to
\ref text_format_references "Formats"; the layers it is made of are \ref format_css_computed "css-computed.md" (the style), \ref format_css_cascade
"css-cascade.md" (which declaration wins), \ref format_css_variables "css-variables.md" (`var()` and the other functions) and \ref format_css_values
"css-values.md" (the value types and their units).

Include `<ghoti.io/text/css.h>` (`css/css_style.h`, `css/css_presentation.h`, `css/css_computed.h`, `css/css_element.h`).

## Quick start

The library never owns or walks a tree except through the **element interface** (`css_element.h`): a table of callbacks over the caller's own nodes. For
presentation attributes it needs three of them, `local_name`, `namespace_uri` and `attribute`. The context holds the table, the registry, the environment and the
limits, and any number of threads may compute with one. `gtext_css_style_compute()` takes the context, the element, the style of the **inheritance parent** and
the options (NULL: read both kinds of attribute, with every attribute-only spelling).

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The caller's tree: an element is whatever the caller says it is. Here, a name and its attributes. */
typedef struct {
  const char * name;
  const char * attributes[8][2];
} node;

static bool node_local_name(void * user, GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out) {
  (void)user;
  out->data = ((const node *)element)->name;
  out->length = strlen(out->data);
  return true;
}

static bool node_namespace(void * user, GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out) {
  (void)user;
  (void)element;
  out->data = GTEXT_CSS_SVG_NAMESPACE;
  out->length = strlen(out->data);
  return true;
}

static bool node_attribute(void * user, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_String * ns, const GTEXT_CSS_String * name, GTEXT_CSS_String * out) {
  (void)user;
  (void)ns;
  const node * n = (const node *)element;
  for (size_t i = 0; i < 8 && n->attributes[i][0]; i++) {
    if (strlen(n->attributes[i][0]) == name->length && memcmp(n->attributes[i][0], name->data, name->length) == 0) {
      out->data = n->attributes[i][1];
      out->length = strlen(out->data);
      return true;
    }
  }
  return false;
}

static void show(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Computed_Style * style, const char * name) {
  const GTEXT_CSS_Value * value = gtext_css_computed_root(style, gtext_css_registry_find_property(registry, name, strlen(name)));
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_value_buffer(value, NULL, &text, &length);
  printf("%s: %s\n", name, text);
  gtext_css_serialize_buffer_free(NULL, text);
}

/* One call turns an element into a computed style: its presentation attributes (SVG's `stroke-width="2"`, a number being user units), its `style`
   attribute, and the style of the element it inherits from. A declaration that is not valid is dropped and counted, as CSS says. */
static void style_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);

  node group = {"g", {{"font-size", "20"}, {"stroke-width", "1"}}};
  node rect = {"rect", {{"stroke-width", "2"}, {"opacity", "0.5"}, {"stroke-linecap", "wavy"}, {"style", "letter-spacing: 0.5em; bogus: 1; stroke-width: 5"}}};

  GTEXT_CSS_Element_Interface table;
  memset(&table, 0, sizeof table);
  table.size = sizeof table;
  table.local_name = node_local_name;
  table.namespace_uri = node_namespace;
  table.attribute = node_attribute;

  GTEXT_CSS_Compute_Options options = gtext_css_compute_options_default();
  options.elements = &table;
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, &options, &context, NULL);

  GTEXT_CSS_Computed_Style *parent = NULL, *style = NULL;
  gtext_css_style_compute(context, &group, NULL, NULL, 0, NULL, &parent, NULL);
  gtext_css_style_compute(context, &rect, parent, NULL, 0, NULL, &style, NULL);

  show(registry, style, "font-size");
  show(registry, style, "stroke-width");
  show(registry, style, "letter-spacing");
  show(registry, style, "opacity");

  GTEXT_CSS_Cascade_Entry where;
  gtext_css_computed_specified(style, gtext_css_registry_find_property(registry, "stroke-width", 12), &where);
  printf("stroke-width came from the %s\n", where.element_attached ? "style attribute" : where.presentational_hint ? "presentation attribute" : "a rule");
  gtext_css_computed_specified(style, gtext_css_registry_find_property(registry, "opacity", 7), &where);
  printf("opacity came from the %s\n", where.element_attached ? "style attribute" : where.presentational_hint ? "presentation attribute" : "a rule");

  for (size_t i = 0; i < gtext_css_computed_diagnostic_total(style); i++) {
    GTEXT_CSS_Style_Diagnostic d;
    gtext_css_computed_diagnostic(style, i, &d);
    printf("dropped %.*s at byte %zu\n", (int)d.name.length, d.name.data, d.offset);
  }

  gtext_css_computed_free(style);
  gtext_css_computed_free(parent);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
```

prints

```
font-size: 20px
stroke-width: 5px
letter-spacing: 10px
opacity: 0.5
stroke-width came from the style attribute
opacity came from the presentation attribute
dropped stroke-linecap at byte 0
dropped bogus at byte 23
```

## What the call does

1. It reads the element's **presentation attributes** through the table of `css_presentation.h` (SVG 2's, unless the options name another) and its `style`
   attribute through the element interface, and adds the declarations of the entries the caller gives.
2. When `GTEXT_CSS_Style_Options::rules` and `::match` are set (`::starting_style` lets the rules under `@starting-style` take part, `::container_call` is the cache of the container queries), it first adds the declarations of the style rules that match the element
   (\ref format_css_sheets "css-sheets.md"), with the keys the cascade defines; `::pseudo` asks for a pseudo-element's rules instead.
3. It **cascades** them (\ref format_css_cascade "css-cascade.md"). A presentation attribute is an author-level declaration of specificity zero that comes
   before every style sheet of the author: any rule beats it, whatever its specificity, and the `style` attribute beats it too. The `style` attribute is over
   every rule that is not `!important`.
4. It **computes** the style from the winners, the parent's style and the environment (\ref format_css_computed "css-computed.md"), substituting `var()` and
   the other functions (\ref format_css_variables "css-variables.md").

The **parent** is the style of the element that this one *inherits from*, which is the tree parent for most elements and the `<use>` element for the clone that
a `<use>` instantiates (SVG 2): the caller passes the style it computed for that element, and the call asks the tree nothing about it. A style shares what it
inherits with its parent's, so a chain of n elements costs O(n).

An attribute or a declaration that is not valid is **dropped**, as CSS says, and is a diagnostic (`gtext_css_computed_diagnostic()`): its kind, the property or
attribute, and where it was, as an offset into the attribute's value plus the base offset the options give (so a caller maps it to its document). A lower-priority
declaration, the inherited value or the initial value stands. A property the engine cannot compute is `GTEXT_CSS_E_UNSUPPORTED`, and the result names it.

## Presentation attributes

The table of which attribute sets which property, on which elements, is data: `gtext_css_presentation_table_svg2()` is SVG 2's (6.6, generated from
`tools/css/svg_presentation.tsv`), and a caller who needs another list (HTML's presentational hints, a profile of SVG) gives a `GTEXT_CSS_Presentation_Table`
of its own in `GTEXT_CSS_Style_Options::presentation`. An attribute in the SVG namespace only counts on the elements the table names: `cx` on a `circle`, not on
a `rect`; `transform` on every element but `pattern` and the gradients, which have `patternTransform` and `gradientTransform`.

An attribute's value is mostly a CSS value, and where SVG gives it a spelling the property does not have, the spelling is an option (each on in
`gtext_css_presentation_options_default()`, each off in a zeroed struct):

| Option | The attribute |
|--------|---------------|
| `unitless_lengths` | a number is a length in user units, in every property that takes a `<length>`: `stroke-width="2"` is `2px` |
| `svg_transform` | `transform`, `gradientTransform` and `patternTransform` are SVG's list (`rotate(45 10 10)`, `translate(10,20)`, numbers without units); a value that is not one is read as CSS's |
| `path_data` | `d` is path data, and the property `d` holds the string (the pin's grammar of `d` is `none | <string>`; engines that take `path("...")` need a caller that writes it) |
| `trim_whitespace` | the white space at both ends of a value is not part of it |

The HTML Standard's presentational hints are the same mechanism with a table of the caller's; the library ships SVG's only.

## Reading a style

A style is read by property (`gtext_css_computed_get()`, `gtext_css_computed_get_by_name()`), in the registry's order (`gtext_css_computed_next()`), by its
custom properties (`gtext_css_computed_custom_find()`, `_at()`), and for the **specified value** (`gtext_css_computed_specified()`): the winning declaration
with its provenance (origin, importance, whether it came from the `style` attribute, a presentation attribute or a rule, and the span in its source).

The declarations of rules are entries (`GTEXT_CSS_Style_Entry`): a list of declarations with the keys the cascade sorts by. Once selectors exist a matcher
supplies them; today a caller who has its own rule matching does.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const char * name;
  const char * attributes[4][2];
} node;

static bool node_local_name(void * user, GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out) {
  (void)user;
  out->data = ((const node *)element)->name;
  out->length = strlen(out->data);
  return true;
}

static bool node_namespace(void * user, GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out) {
  (void)user;
  (void)element;
  out->data = GTEXT_CSS_SVG_NAMESPACE;
  out->length = strlen(out->data);
  return true;
}

static bool node_attribute(void * user, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_String * ns, const GTEXT_CSS_String * name, GTEXT_CSS_String * out) {
  (void)user;
  (void)ns;
  const node * n = (const node *)element;
  for (size_t i = 0; i < 4 && n->attributes[i][0]; i++) {
    if (strlen(n->attributes[i][0]) == name->length && memcmp(n->attributes[i][0], name->data, name->length) == 0) {
      out->data = n->attributes[i][1];
      out->length = strlen(out->data);
      return true;
    }
  }
  return false;
}

static void show(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Computed_Style * style, const char * name) {
  const GTEXT_CSS_Value * value = gtext_css_computed_root(style, gtext_css_registry_find_property(registry, name, strlen(name)));
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_value_buffer(value, NULL, &text, &length);
  printf("%s: %s\n", name, text);
  gtext_css_serialize_buffer_free(NULL, text);
}

static GTEXT_CSS_Style_Entry rule(const char * declarations, uint32_t order, unsigned specificity) {
  GTEXT_CSS_Style_Entry entry;
  memset(&entry, 0, sizeof entry);
  entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  entry.keys.rule = order;
  entry.keys.specificity.b = specificity;
  entry.text.data = declarations;
  entry.text.length = strlen(declarations);
  return entry;
}

/* The declarations of the style rules that matched an element (once selectors exist, a matcher supplies them) are entries with the keys the cascade sorts by.
   A presentation attribute is under every rule, whatever its specificity; the `style` attribute is over every rule that is not `!important`. */
static void style_entries(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Element_Interface table;
  memset(&table, 0, sizeof table);
  table.size = sizeof table;
  table.local_name = node_local_name;
  table.namespace_uri = node_namespace;
  table.attribute = node_attribute;
  GTEXT_CSS_Compute_Options options = gtext_css_compute_options_default();
  options.elements = &table;
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, &options, &context, NULL);

  node rect = {"rect", {{"stroke-width", "2"}, {"opacity", "0.25"}, {"font-size", "10"}, {"style", "font-size: 30px"}}};
  const GTEXT_CSS_Style_Entry rules[] = {
      rule("stroke-width: 4px; opacity: 0.5", 0, 0),
      rule("opacity: 0.75", 1, 10),            /* more specific: wins over the first */
      rule("font-size: 99px", 2, 0),           /* loses to the style attribute */
  };
  GTEXT_CSS_Computed_Style * style = NULL;
  gtext_css_style_compute(context, &rect, NULL, rules, 3, NULL, &style, NULL);
  show(registry, style, "stroke-width");
  show(registry, style, "opacity");
  show(registry, style, "font-size");

  gtext_css_computed_free(style);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
```

prints

```
stroke-width: 4px
opacity: 0.75
font-size: 30px
```

## The dump and the hash

`gtext_css_computed_dump()` writes the style as canonical text: a first line `css-computed 1` (`GTEXT_CSS_COMPUTED_DUMP_VERSION`, which changes with any
change of the format), a second naming the commit of the pin the registry's properties come from, then one line `name: value` for every property that has a
value, **in the byte order of the names** (the custom properties among them, `--` sorting before the letters), each value as a computed value is serialized.
Which declaration a value came from is not in it. `gtext_css_computed_hash()` is the SHA-256 of that text (`GTEXT_CSS_COMPUTED_HASH_LENGTH` bytes), computed
with the security library as the text is written, so a large style costs no buffer.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  char text[256];
  size_t length, lines;
} collected;

static GTEXT_Markup_Status collect(void * user, const char * data, size_t length) {
  collected * c = (collected *)user;
  for (size_t i = 0; i < length; i++) {
    if (data[i] == '\n') {
      c->lines++;
    }
    if (c->length < sizeof c->text - 1 && c->lines < 1) {
      c->text[c->length++] = data[i];
    }
  }
  c->text[c->length] = '\0';
  return GTEXT_MARKUP_OK;
}

static void compute(const GTEXT_CSS_Compute_Context * context, const char * declaration, GTEXT_CSS_Computed_Style ** out) {
  GTEXT_CSS_Style_Entry entry;
  memset(&entry, 0, sizeof entry);
  entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  entry.text.data = declaration;
  entry.text.length = strlen(declaration);
  GTEXT_CSS_Style_Options options;
  memset(&options, 0, sizeof options); /* no attributes are read: the entry is all there is */
  gtext_css_style_compute(context, NULL, NULL, &entry, 1, &options, out, NULL);
}

/* A computed style has a canonical text, one line to a property with the properties in the byte order of their names, and the SHA-256 of that text. Two
   cascades that come to the same values give the same text and the same hash, whatever declarations they came from; a document dump can embed the hash. */
static void style_dump(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  GTEXT_CSS_Computed_Style *a = NULL, *b = NULL, *c = NULL;
  compute(context, "margin-left: 1px; color: red", &a);
  compute(context, "color: red; margin-left: 1px; margin-left: 1px", &b);
  compute(context, "color: blue", &c);

  collected text;
  memset(&text, 0, sizeof text);
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &text;
  sink.write = collect;
  gtext_css_computed_dump(a, &sink);
  printf("version %d, begins:\n%s\n", GTEXT_CSS_COMPUTED_DUMP_VERSION, text.text);
  size_t properties = 0, cursor = 0;
  while (gtext_css_computed_next(a, &cursor, NULL, NULL)) {
    properties++;
  }
  printf("a line for each property, after the two of the header: %s\n", text.lines == properties + 2 ? "yes" : "no");

  unsigned char ha[GTEXT_CSS_COMPUTED_HASH_LENGTH], hb[GTEXT_CSS_COMPUTED_HASH_LENGTH], hc[GTEXT_CSS_COMPUTED_HASH_LENGTH];
  gtext_css_computed_hash(a, ha);
  gtext_css_computed_hash(b, hb);
  gtext_css_computed_hash(c, hc);
  printf("hash of %d bytes\n", (int)GTEXT_CSS_COMPUTED_HASH_LENGTH);
  printf("same values, same hash: %s\n", memcmp(ha, hb, sizeof ha) == 0 ? "yes" : "no");
  printf("other values, same hash: %s\n", memcmp(ha, hc, sizeof ha) == 0 ? "yes" : "no");

  gtext_css_computed_free(a);
  gtext_css_computed_free(b);
  gtext_css_computed_free(c);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
```

prints

```
version 1, begins:
css-computed 1
a line for each property, after the two of the header: yes
hash of 32 bytes
same values, same hash: yes
other values, same hash: no
```

## Against Chromium and Firefox

`make check-css-style-oracle` asks this library, Chromium 156 and Firefox 158 (Gecko, whose style engine is Stylo) for the computed style of the same elements: a
seeded population of SVG fixtures, presentation attributes on SVG elements, generated values of every property, generated trees of HTML and SVG elements with
`style` and presentation attributes, and twelve small documents with a style sheet, in both color schemes. The browsers answer `getComputedStyle()` through one
WebDriver BiDi driver in a pinned image, with the documents inside an element with `display: none` so that the resolved value of a property is its computed value.
A divergence is keyed by its pattern, its property and the smallest declaration that has it, and `tools/oracle/css_style_divergences.txt` gives each key a class and
the sentence of the served draft that decides it (`reference-wrong`, `reference-old`, `legacy-compat`, `unspecified`, ...); the gate fails on a key that is not listed,
a listed key that no longer occurs, a count or digest that changed, a witness that does not hold, a control that is not as it was, and on any key of class `ours`. Its
controls make the gate fail on purpose, among them eight of the thirteen defects planted in this library's own sources (the others are below the population and are
seen by the unit tests).

**The cascade.** The twelve style-sheet documents (`cascade-*` in `tools/oracle/css_style_gen.py`) put a presentation attribute, a rule of a `<style>` element
(selectors `#id`, `*` and an element's name; selector matching is C3) and the `style` attribute on one element, with `!important`, with equal and unequal specificity and
with a shorthand, and the three agree on all of them. What the population still does not reach is the rest of the cascade: layers, `@scope`, shadow contexts and
selectors other than those three, which the unit tests and the cascade's own fuzzer cover and a browser cannot be asked about until selectors exist here.

What the oracle found in the first runs and is fixed: the used color scheme of `color-scheme: normal` is the user agent default, not the preference;
`display: contents` is `none` on an SVG element that is no container of rendered content (and on a `switch`, as both browsers compute it); `stroke-dasharray` is one
comma separated list and its dashes cannot be negative; `word-spacing: normal` is `0px`, a zero `letter-spacing` is `normal`, a bare `font-style: oblique` stays
`oblique`; the order of `paint-order` is its value and its shortest form is written; and `d` as an attribute is the `<string>` of the served draft.

## Limits and threads

The candidates of one element (`GTEXT_CSS_Style_Options::candidates`), the declarations of a list (`parse`), the depth of a chain of elements, the length of a
list and the budget of substitution are limits with a status of their own, and a limit is asserted by its bound. A context is immutable after it is made and
may be shared by any number of threads, each computing its own styles; a style is immutable and reference counted with atomic counts
(`gtext_css_computed_retain()`, `gtext_css_computed_free()`), so any thread may hold and release it. The call allocates through the context's allocator and
leaves nothing behind when an allocation fails, including the buffers a presentation attribute is converted in, which are the candidate set's allocator's like everything else.
`tests/test-css-style-threads.cpp` computes, retains and frees from eight threads on one shared context and parent and compares every dump and hash with a single thread's
(it runs in the ThreadSanitizer build with the other tests).

## What is not here

Conditional rules (the filter that says which `@media`, `@supports` and `@container` apply is the environment's, and not here), used values and layout, animations and
transitions. A caller that is an SVG consumer registers the grammars of `fill` and `stroke`, which the registry's pin leaves unresolved, as `vector` does
(\ref format_css_registry "css-registry.md").
