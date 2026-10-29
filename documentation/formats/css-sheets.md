@page format_css_sheets Style sheets

# Style sheets

A parsed style sheet becomes an immutable object that is compiled once and shared, an index merges the sheets of a document, and for each element the index adds to the
cascade the declarations of the rules that match it. This page is the sheet, the index and what they decide where the specifications leave a question to the library;
the selectors are \ref format_css_selectors "css-selectors.md" and the cascade \ref format_css_cascade "css-cascade.md".
Back to \ref text_format_references "Formats".

Include `<ghoti.io/text/css.h>` (`css/css_sheet.h`).

## Quick start

```c
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <ghoti.io/text/markup.h>
#include <ghoti.io/text/markup/markup_query.h>
#include <stdio.h>
#include <string.h>

/* A style sheet is compiled once and shared; an index merges the sheets of a document; for each element the index adds to the cascade the declarations of the rules that match
   it, and the cascade says which declaration wins each property. Here the element is a node of a Markup document. */
static void sheet_quick_start(void) {
  static const char html[] = "<!DOCTYPE html><div class=card><p id=a class=lead>one</p><p id=b>two</p></div>";
  static const char css[] =
      "p { color: red; margin-top: 1px }\n"
      ".card > .lead { color: green }\n"
      "@layer base { p { margin-top: 5px; margin-left: 5px } }\n"
      "#b { margin-top: 9px !important }\n";

  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);

  GTEXT_Markup_Document * document = NULL;
  GTEXT_HTML_Result html_result;
  memset(&html_result, 0, sizeof html_result);
  gtext_html_parse(html, strlen(html), NULL, &document, &html_result);
  gtext_html_result_free(&html_result);

  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result parsed;
  memset(&parsed, 0, sizeof parsed);
  gtext_css_parse_stylesheet(css, strlen(css), NULL, &tree, &parsed);
  GTEXT_CSS_Sheet * sheet = NULL;
  gtext_css_sheet_compile(tree, registry, NULL, &sheet, NULL);
  gtext_css_tree_free(tree);
  gtext_css_result_free(&parsed);

  GTEXT_CSS_Markup_Adapter * adapter = NULL;
  gtext_css_markup_adapter_new(document, NULL, &adapter);

  GTEXT_CSS_Rule_Index_Options index_options = gtext_css_markup_rule_index_options(adapter);
  GTEXT_CSS_Rule_Index * index = NULL;
  gtext_css_rule_index_new(registry, &index_options, &index);
  GTEXT_CSS_Sheet_Registration registration;
  memset(&registration, 0, sizeof registration);
  registration.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  gtext_css_rule_index_add_sheet(index, sheet, &registration, NULL);
  gtext_css_rule_index_freeze(index, NULL);

  GTEXT_CSS_Selector_Match_Options options = gtext_css_markup_match_options(adapter);
  GTEXT_CSS_Selector_Match_Context * match = NULL;
  gtext_css_selector_match_context_new(gtext_css_markup_interface(adapter), &options, &match);

  GTEXT_Markup_Node ** paragraphs = NULL;
  size_t count = 0;
  gtext_markup_query_selector_all(gtext_markup_document_root(document), "p", 1, NULL, &paragraphs, &count, NULL);
  for (size_t e = 0; e < count; e++) {
    GTEXT_CSS_Candidates * set = NULL;
    gtext_css_candidates_new(registry, NULL, NULL, &set);
    gtext_css_rules_collect(index, match, gtext_css_markup_handle(paragraphs[e]), NULL, set, NULL, NULL);
    GTEXT_CSS_Cascaded * cascaded = NULL;
    gtext_css_cascade(set, NULL, &cascaded, NULL);
    GTEXT_Markup_Attribute id;
    gtext_markup_element_get_attribute(paragraphs[e], NULL, "id", &id);
    for (size_t i = 0; i < gtext_css_cascaded_count(cascaded); i++) {
      GTEXT_CSS_Cascade_Entry winner;
      gtext_css_cascaded_at(cascaded, i, &winner, NULL);
      GTEXT_CSS_Property_Info info;
      gtext_css_registry_property_info(registry, winner.property, &info);
      char * value = NULL;
      size_t length = 0;
      gtext_css_serialize_value_buffer(gtext_css_value_set_root(winner.set), NULL, &value, &length);
      printf("%.*s %s: %s\n", (int)id.value_length, id.value, info.name, value);
      gtext_css_serialize_buffer_free(NULL, value);
    }
    gtext_css_cascaded_free(cascaded);
    gtext_css_candidates_free(set);
  }
  gtext_markup_query_free(NULL, paragraphs);
  gtext_css_selector_match_context_free(match);
  gtext_css_markup_adapter_free(adapter);
  gtext_css_rule_index_free(index);
  gtext_css_sheet_free(sheet);
  gtext_markup_document_free(document);
  gtext_css_registry_free(registry);
}
```

prints

```
a color: green
a margin-left: 5px
a margin-top: 1px
b color: red
b margin-left: 5px
b margin-top: 9px
```

`a` is `.card > .lead` (more specific than `p`, so green), and its `margin-top` is the unlayered `p` rule: an unlayered declaration beats one in a layer, whatever the
order. `b` has the `!important` of `#b`. `margin-left` is set only inside `@layer base` and still applies, because nothing outranks it.

## The sheet

`gtext_css_sheet_compile()` turns a tree of a style sheet (`gtext_css_parse_stylesheet()`) into a `GTEXT_CSS_Sheet`: **immutable, reference counted with atomic counts
(`gtext_css_sheet_retain()`, `gtext_css_sheet_free()`), and shareable between threads and between indexes**. Nothing refers to the tree afterwards, so the tree may be freed.
The compiled form is a record for each complex selector of each style rule, with its specificity, its place among the rules, the layer and the `@scope` it is in, and the
rule's declarations, which are parsed **once** through the registry and shared by every element they apply to (the candidate set retains the sheet's values and does not copy
them).

| Source | What the sheet does |
|---|---|
| a style rule | one record for each complex selector of its list, and the typed declarations; an invalid selector drops the rule with what is nested in it, a diagnostic says so |
| a declaration | an unknown property, an invalid value or a value the library cannot judge (`GTEXT_CSS_E_UNSUPPORTED`, never guessed) is dropped, with a diagnostic |
| nesting | resolved without textual expansion: `&` stands for the parent's list and the matcher evaluates it as `:is()` of it |
| a nested declarations rule | the declarations after a nested rule have a place of their own, so one after a nested rule of equal specificity beats it |
| `@namespace` | fills the prefix map of the rules after it; a prefix it does not declare makes a selector invalid |
| `@layer` | statements, blocks, nested and anonymous layers, kept in the sheet's own layer tree (a sheet does not depend on a document's registry) |
| `@import` | read where the standard allows one; the sheet asks the caller for the imported sheet through `GTEXT_CSS_Import_Fn` and **never fetches** |
| `@media`, `@supports`, `@container`, `@starting-style`, `@when`, `@else` | **condition nodes** the sheet keeps as data (below); `@document` is a node that never applies |
| `@scope` | a scope the rules inside it are in (below) |

**Nesting.** `.a, #b { & .c {} .d {} > .e {} }` has three records for the nested rules, each a selector of its own list: a compound with no `&` is `& ` and the selector, a
leading combinator relates to `&`, and `&` has the specificity of the most specific selector of the parent's list ((1,1,0) here). A list nested forty levels deep, five selectors
each, is five records and compiles in time linear in the text, not 5^40. Style rules may be nested `max_rule_depth` deep (default 64); one more is
`GTEXT_CSS_E_LIMIT_RULE_NESTING`.

**Conditions are data.** Compiling a sheet evaluates nothing. Every conditional group rule becomes a **condition node** of the sheet (the parsed condition, immutable, with a link to
the node it is inside), and every record, every layer occurrence and every import carries the node it is under (`GTEXT_CSS_Sheet_Record::condition`, `gtext_css_sheet_condition()`).
So one compiled sheet serves any number of environments and documents: a resize is a new `GTEXT_CSS_Conditions` (\ref format_css_conditions "css-conditions.md") and a new index, and
the sheet is untouched. A condition that cannot be read is a node that never applies, with a diagnostic (an `@media` list replaces an invalid query by `not all`, as Media Queries 4
says). `@else` joins the rule before it, if that is an `@when` or `@else` with nothing but white space and comments between: it applies only if no earlier rule of its chain does
(Conditional 5, 4), and an `@else` outside a chain is invalid. A conditional rule that holds only data (`@media print { @font-face {} }`) is no node. Nodes are limited by
`max_conditions` (default 1,048,576, `GTEXT_CSS_E_LIMIT_CONDITIONS`).

**Imports.** `GTEXT_CSS_Sheet_Options::import` is asked for **every** import. It gets the rule's `url`, `layer`, `scope()`, `supports()` and media list, as text and **parsed**
(`media_list`, `supports_condition`: evaluate them with the conditions object to decide whether to fetch at all), and returns an already compiled sheet, **one owner given to the
importer** (return `gtext_css_sheet_retain()` of a cached one, which is how a sheet is kept for a later resize), or NULL for a fetch that failed or was refused, which contributes
nothing and is a diagnostic. The grammar is `[ layer | layer(name) ] || [ scope | scope(...) ] || supports(...)` in any order, then the media list. `scope(<selector>)`,
`scope((<start>) to (<end>))` and `scope()` (the implicit root) wrap the imported sheet's rules in an implicit `@scope`; a selector that names `&` or `:scope` keeps its meaning, and the
others have the implied `:where(:scope)` in front of them. An `@import` after a style rule is ignored. A sheet that imports deeper than `max_import_depth` (default 16) is `GTEXT_CSS_E_LIMIT_RULE_NESTING`.

`gtext_css_sheet_record()` and `gtext_css_sheet_record_declarations()` read what was compiled, `gtext_css_sheet_diagnostic()` what was dropped and why (with a reference to the
sentence of the draft that decides it), and `gtext_css_sheet_dump()` writes a canonical, versioned text of the sheet that is the same on every platform.

## The index

`gtext_css_rule_index_new()`, then `gtext_css_rule_index_add_sheet()` for each sheet in the order they appear in the document, then `gtext_css_rule_index_freeze()`. The index
**flattens** the sheets: an imported sheet goes in before the sheet that imports it, at the place of the import, and every sheet and rule has one order of appearance; the layers of
each sheet are merged into the layer registry of the sheet's context, in that order, so the first-appearance order of the cascade layers holds across sheets. A frozen index is
read only and may be used by any number of threads, each with a match context of its own. The records are in buckets by the id, a class or the local name of the subject
(the rest in one list), so the work to style an element is the records of the keys it has and not the number of rules (`make test` asserts the same count of rules considered
for 100 rules as for 8,000).

**Conditions.** `GTEXT_CSS_Rule_Index_Options::conditions` is the evaluator the index is built for, and is **required once a sheet holds a condition node** (`GTEXT_CSS_E_UNSUPPORTED`
naming `conditions` when it is not: never a guess). Each node is evaluated **once** as the sheet is added (a node is met after the node it is inside, so n nested rules cost O(n)); the
records and imports whose chain is false are not in the index, and `gtext_css_rule_index_record_count()` counts the others. **A layer is declared at an occurrence whose chain holds**:
`@media (width > 9999px) { @layer x {} } @layer y; @media screen { @layer x {} }` declares y, then x, and a layer under a false `@media` or a false `@import ... supports()` is not
declared at all (Cascade 5, 6.4.3); under an `@container` it is, because the condition can differ for each element. A chain with a container node keeps its record, flagged element
dependent, and `gtext_css_rules_collect()` asks the container interface about it for each element (\ref format_css_conditions "css-conditions.md"). Two conditions objects over the same
sheets give two indexes whose records are the sheets' own where the conditions agree.

`gtext_css_rules_collect(index, match, element, pseudo, set, options, result)` adds to a candidate set one entry for each declaration of each rule that matches the element (or the
pseudo-element `pseudo` asks for: a list of one complex selector made of pseudo-elements, as `gtext_css_selector_match()` takes). The entry has every key the cascade defines:
`origin`, `important` (the declaration's), `layer` (in the registry of the entry's context, which the call gives the set), `context`, `scoped` and `scope_distance`, `specificity` (the
highest of the complex selectors of the rule that match), and `sheet` and `rule` (the order of appearance). A set takes one call; make a new set for each element.
`options` (NULL for the defaults) says whether the records under `@starting-style` take part (`starting_style`: compute the style once without and once with, as Transitions 2 defines the starting style) and gives the call context of the container queries. `GTEXT_CSS_Style_Options::rules` and `::match` do the whole chain in `gtext_css_style_compute()` (\ref format_css_style "css-style.md"), and `::pseudo` gives the style of a
pseudo-element's entries.

## What the specifications leave open, and what the library decides

**The context number (Cascade 5, 6.1).** An entry's `context` is the number of the **tree scope of the sheet that owns the rule**, whatever element matched. A sheet is
registered with its scope handle and its context number, assigned in shadow-including tree order: the document is 0 with no handle; a shadow root has a handle (the
interface's `tree_scope`), a number above the number of the scope that holds its host (`parent_scope`), and `shadow` set. So a `:host` rule and a `::slotted()` rule of a shadow tree's
sheet carry that tree's number, and lose, for normal declarations, to the outer tree's rules on the same host, and win for `!important`; a `host::part(a)` rule of an outer sheet carries
the outer number. The index checks the registrations: two scopes with one number, or a shadow tree numbered below its host's scope, is `GTEXT_CSS_E_INVALID` naming `context order`.
The layer registry is per context, so two trees order identically named layers each in their own order. A rule of the document never reaches into a shadow tree (a fuzz property), and
`::slotted()` follows the chain of slots that are themselves assigned: every shadow tree on the chain contributes its rules.

**Scope proximity (Cascade 6, 3.1).** A rule in `@scope (<start>)? to (<end>)?` applies to an element E when some scoping root R, an element matching `start`, is an inclusive
ancestor of E, no element from E up to and including R matches `end` (with `:scope` R; so a limit that names the root, `to (:scope)` or `to (&)`, leaves nothing in scope, as in the browsers), and the selector, with the implied `& ` in front of it unless it contains `&` or `:scope`, matches with
`:scope` R. The entry is `scoped`, and `scope_distance` is the least number of generational hops from R to E over the roots that qualify. `start` is read as written (a selector list; a `:scope` or `&` in it is the enclosing context's), and `end` is read as the
rules inside are, relative to R. With no prelude the root is the registration's `implicit_root`. An `@scope` inside another (or inside a style rule) reads its `start` as a nested selector, with the implied `&`: its
roots are inside a root of the enclosing scope, which is `:scope` and `&` there (the browsers agree: an `@scope (.a)` inside an `@scope (.a)` styles nothing for a single `.a`). Nesting
of scopes is limited to 8. The declarations that are directly in an `@scope` apply to the root
with no specificity. The search for the roots is a walk up the ancestors, bounded by `max_work` of the match context; whether an ancestor is a root of a scope for the element being collected is
kept (for that element), so that scopes nested to the limit of 8 cost a polynomial of the depth of the tree and not its power. A pseudo-element in the start or the limit makes the prelude invalid, and
the rule is dropped. `gtext_css_rules_collect()` refuses (`GTEXT_CSS_E_INVALID`) a match context whose `html_document` or `quirks` is not the index's: the buckets are keyed by the index's folding, and a
context that folds otherwise would find no rule and say nothing.

**The conditional `@import` (Cascade 5, 6.4.3).** The filter is called for an `@import` that has `supports()` or a media list **before** its layer is declared, so a layer
introduced by an import that does not apply is not declared.

## Limits, diagnostics, threads

`max_compiled_bytes` (default 256 MiB) bounds the compiled sheet (`GTEXT_CSS_E_LIMIT_SHEET_SIZE`), the index `max_records` (default 16,777,216), and the match context's limits
(\ref format_css_selectors "css-selectors.md") bound a collection: `GTEXT_CSS_E_LIMIT_MATCH_WORK` and `_MEMO` are never a wrong answer. A callback the sheets need and the element
interface lacks is `GTEXT_CSS_E_UNSUPPORTED` and the result names it (`child_content`, `tree_scope`, `id`, ...), for the callbacks the index reads to find the records of an element
as well as for the selectors. A compile fails with `GTEXT_CSS_E_OOM` at any allocation, with nothing leaked (`tests/test-css-sheet-oom.cpp` fails every allocation in turn).
A sheet and a frozen index are shared between threads; a match context, a candidate set and an adapter are not.

## Tested scope

`tests/test-css-sheet.cpp` holds each rule above as a test named for it; `test-css-sheet-cost.cpp` the counter bounds, `test-css-sheet-oom.cpp` an allocation failed at every place,
`test-css-sheet-threads.cpp` eight threads on one sheet and one index; `test-css-sheet-reference.cpp` the index against a second route to the answer (a nested rule is the flat selector
with `:is(parent)` for `&`; an `@scope` rule is the roots, their descendants and the limits worked out one element at a time by the selector engine; the spellings of the shadow selectors
that must collect the same elements); `tests/fuzz/fuzz_css_sheet.cpp` compares the index with the brute force (every rule's selector list matched against every element, for the element and for each
pseudo-element asked) and checks that the memo and the ancestor filter change nothing; `make check-css-selector-plants` shows each fail on a planted defect.

`make check-css-sheet-oracle` is the differential against the browsers: 200 generated documents (17,191 questions: the computed `color`, `background-color`, `margin-top`,
`margin-left` and `padding-left` of every element, a shadow tree's included, and what `querySelectorAll()` selects) with style sheets of nested rules, layers, `@scope` (nested, with limits, with
`:scope` inside the arguments of pseudo-classes), conditional groups, `@namespace` with an inline `<svg>`, `dir` attributes, quirks-mode documents, and shadow trees (inside shadow trees, with slots
assigned to slots), asked of this library, of Chromium 156 and of Firefox 158. Nothing diverges that is this library's. What remains is Chromium throwing on a quoted `:lang("*-CH")`, which Selectors 4
allows and Firefox accepts, and Firefox not implementing `:host-context()`, which Chromium does (listed in `tools/oracle/css_sheet_divergences.txt` with the sentence). A hunt of 1,500 documents of
another seed (129,709 questions of the browsers, 165,887 with the Servo `selectors` crate, which the quirks-mode documents are told of, through `tools/oracle/containers/css/selectors-driver`) has
nothing that is this library's either; hunts of the earlier population found the defects this page's decisions record (the host is featureless and the top of its shadow tree, a `&` that stands
for a pseudo-element represents nothing, the roots of a nested `@scope` are inside the outer root, a slot assigned to a slot is replaced by what is assigned to it).
The gate shows itself failing: the list tampered with six ways and the planted defects of the library (`tools/oracle/CSS_SHEET_ORACLE_PLANTS`) are each caught.
