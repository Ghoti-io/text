@page format_css_cssom The CSS Object Model

# CSS Object Model

The CSSOM here is the **data model and the algorithms** of the CSS Object Model specification as a native, reference counted, mutable object graph, for a program with no script engine: a browser engine's binding layer, an editor, an inspector. It is not a JavaScript engine, Web IDL, wrapper objects, events or promises; those are a binding's, and the API is shaped for one (object identity is stable, every object has a user-data slot with a finalizer, the specification's exceptions are status codes, and nothing a call returns is a pointer that a later mutation can invalidate).

This page describes what is built: the document and its objects, their lifetime, the style sheet and the rule objects and what a parse makes of a text, the declaration object (`CSSStyleDeclaration`) of a rule, of an element's `style` attribute and free-standing, the IDL attribute names, `getComputedStyle()` and the resolved values, `CSS.escape()`, and the statuses of the web platform's exceptions. The declaration block underneath is in \ref format_css_computed "css-computed.md"; the immutable sheets and indexes that the style computation reads are in \ref format_css_sheets "css-sheets.md".

Back to \ref "Formats". Include `<ghoti.io/text/css.h>` (`css/css_cssom.h`, `css/css_cssom_computed.h`, `css/css_cssom_core.h`, `css/css_cssom_decl.h`, `css/css_cssom_rule.h`, `css/css_cssom_sheet.h`, `css/css_cssom_util.h`).

## The document and its objects

A `GTEXT_CSSOM_Document` is the unit of ownership. It holds the registry (not owned: it outlives the document), the limits, the counters (the **generation**, which every mutation of any object the document owns that changed something increments, and the ids) and the caller's tree version. Objects are created by the document, reference counted with a plain, non-atomic count (`retain` and `release`), and valid for as long as anyone holds them, **including after the document handle has been released**: the document's core (allocator, registry pointer, limits and counters) is alive until the last of them is freed.

The live objects are **single-writer and not thread-safe**; the contract is the DOM's. All calls on the objects of one document are serialized by the caller, no object has a lock, and two documents on one registry are independent. What crosses threads is immutable: a typed value (`gtext_cssom_decl_value_set()` returns one retained with an atomic count), a snapshot, an index.

```text
GTEXT_CSSOM_Config config = gtext_cssom_config_default();
config.registry = registry;
GTEXT_CSSOM_Document * document = NULL;
gtext_cssom_document_new(&config, &document);
```

The limits are fields of the config; a field left at 0 selects its default, and 0 never means unlimited. Each limit is its own status and an exact answer: the call fails whole and changes nothing (`GTEXT_CSS_E_LIMIT_CSSOM` for live objects, live bytes, element-owned declarations and the spent generation counter; `GTEXT_CSS_E_LIMIT_DECLARATIONS`, `_LIMIT_INPUT`, `_LIMIT_OUTPUT` and `_LIMIT_WORK` for a declaration block, its text and one call's work). The counters `first_generation` and `first_id` start the generation and the ids; a test sets the first near 2^64 to reach the spent counter.

### Lifetime and keep-alive

Every object has an **internal** count (owning edges) and an **external** count (the callers' `retain` and `release`). The first external reference of a child adds one external hold on its parent, and so up the chain to the sheet, released when the child's count returns to zero: a held rule keeps its sheet alive. The parent pointer is weak, so there is no strong cycle, and a detached subtree keeps no hold on its former parent. An object is freed when both counts are zero. Freeing never recurses on the C stack: a chain of a million objects is freed by one release.

`gtext_cssom_object_set_user(object, pointer, finalizer)` stores the binding's wrapper. Replacing the pair runs the old finalizer first, clearing it runs it, and the finalizer also runs once when the object is freed, called with the pointer and the object's kind. **A finalizer must not call any `gtext_cssom_*` function on the dying object**, nor on the parents of a rule that dies with them (a graph that dies together is freed parent first). It may release and `set_user` other objects and free its own memory, and a mutation attempted from it, or from a notification callback, is refused with `GTEXT_CSS_E_REENTRANT`. `retain`, `release`, `set_user` and every finalizer run on the document's writer thread; a binding whose collector finalizes elsewhere queues the release to the writer.

## Mutations, outcomes and the exceptions

A mutator returns `GTEXT_CSS_OK` or a failure's status and fills a `GTEXT_CSSOM_Outcome`. Where the specification **ignores** an operation (a value that does not parse, a property that is not supported, a priority that is not `important`) the call returns `GTEXT_CSS_OK` and the outcome says which ignore it was, so that an editor can show it and a binding can discard it. Where the specification **throws**, the status is the exception's:

| Status | DOMException |
|---|---|
| `GTEXT_CSS_E_SYNTAX` | `SyntaxError` |
| `GTEXT_CSS_E_HIERARCHY` | `HierarchyRequestError` |
| `GTEXT_CSS_E_INDEX_SIZE` | `IndexSizeError` |
| `GTEXT_CSS_E_INVALID_STATE` | `InvalidStateError` |
| `GTEXT_CSS_E_NOT_ALLOWED` | `NotAllowedError` |
| `GTEXT_CSS_E_NO_MODIFICATION` | `NoModificationAllowedError` |
| `GTEXT_CSS_E_NOT_FOUND` | `NotFoundError` |
| `GTEXT_CSS_E_SECURITY` | `SecurityError` |
| `GTEXT_CSS_E_INVALID_ACCESS` | `InvalidAccessError` |

`gtext_cssom_exception_name(status)` returns the name for those nine and NULL for every other status, which is all a binding needs to throw. `GTEXT_CSS_E_INVALID` is a misuse of the API (a NULL or unusable argument) and is never a content error; `GTEXT_CSS_E_REENTRANT` (a mutation from inside a callback) and `GTEXT_CSS_E_LIMIT_CSSOM` have no exception name. Every mutator has the **strong guarantee**: on any failure, `GTEXT_CSS_E_OOM` included, the object, its versions and the document's counters are as they were. A mutation that leaves the affected declaration byte-identical is `UNCHANGED`: no version, no generation and no notification.

## Declarations

`GTEXT_CSSOM_Decl` is the object of every owner of declarations: free-standing (`gtext_cssom_decl_new()`, for editors and tests), an element's `style` attribute, a rule's, or a computed style's (read-only). It is built on the declaration block of \ref format_css_computed "css-computed.md", which gains `gtext_css_block_set()`, `_remove()` and `_find()` over an open-addressing index (amortized O(1); a block's `gtext_css_block_probes()` counts the entries looked at), and every operation is CSSOM's algorithm with its steps in the order the specification gives them.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static const char * outcome_name(GTEXT_CSSOM_Outcome_Kind kind) {
  switch (kind) {
  case GTEXT_CSSOM_CHANGED: return "changed";
  case GTEXT_CSSOM_UNCHANGED: return "unchanged";
  case GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY: return "ignored: unknown property";
  case GTEXT_CSSOM_IGNORED_INVALID_VALUE: return "ignored: invalid value";
  case GTEXT_CSSOM_IGNORED_BAD_PRIORITY: return "ignored: bad priority";
  default: return "ignored";
  }
}

/* A declaration block is an object: setProperty reports what became of each call, and the block serializes as CSSOM does. */
static void cssom_declaration(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);
  GTEXT_CSSOM_Decl * style = NULL;
  gtext_cssom_decl_new(document, &style);

  const char * calls[][3] = {{"margin-top", "5px", ""}, {"margin", "1px 2px", ""}, {"margin", "1px 2px", ""}, {"color", "nonsense", ""},
      {"color", "red", "IMPORTANT"}, {"width", "1px", "later"}, {"bogus", "1", ""}};
  for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
    GTEXT_CSSOM_Outcome outcome;
    gtext_cssom_decl_set_property(style, calls[i][0], strlen(calls[i][0]), calls[i][1], strlen(calls[i][1]), calls[i][2], strlen(calls[i][2]), &outcome);
    printf("%s: %s -> %s\n", calls[i][0], calls[i][1], outcome_name(outcome.kind));
  }
  char * text = NULL;
  size_t length = 0;
  gtext_cssom_decl_css_text(style, &text, &length);
  printf("cssText: %s\n", text);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), text);

  gtext_cssom_decl_remove_property(style, "margin", 6, &text, &length, NULL);
  printf("removed margin: %s\n", text);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), text);
  size_t count = 0;
  gtext_cssom_decl_length(style, &count);
  printf("length: %zu\n", count);

  gtext_cssom_decl_release(style);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
```

prints

```
margin-top: 5px -> changed
margin: 1px 2px -> changed
margin: 1px 2px -> unchanged
color: nonsense -> ignored: invalid value
color: red -> changed
width: 1px -> ignored: bad priority
bogus: 1 -> ignored: unknown property
cssText: margin: 1px 2px; color: red !important;
removed margin: 1px 2px
length: 1
```

A property name that is not a custom property is ASCII lowercased and an alias is its property. `getPropertyValue` of a longhand is its last declaration in its shortest specified form, of a shorthand its longhands written as one value (the empty string when one is missing, when the importance differs or when they are not a value of the shorthand's grammar), of a shorthand all of whose longhands are pending values of one declaration (`margin: var(--x)`) the value as it was written, of a custom property its trimmed token text (the name is case-sensitive). `setProperty` follows the served steps in order: a read-only declaration is `GTEXT_CSS_E_NO_MODIFICATION`; a property that is not supported returns with nothing changed **before the value is looked at** (so `setProperty('bogus', '')` removes nothing and reports `IGNORED_UNKNOWN_PROPERTY`); an empty value is `removeProperty`; a priority that is neither empty nor an ASCII case-insensitive `important` is ignored; an invalid value, and a value that contains `!important`, is ignored. A shorthand expands: a longhand already in the block is replaced **in place**, a new one is appended in the shorthand's order. The `cssText` setter replaces the whole list; a declaration that does not parse is dropped, nested rules are dropped, of two declarations of one property the later replaces the earlier, and an important declaration is not replaced by a later one that is not. A property the registry has but the library cannot judge (`voice-family`), and a shorthand it cannot split into longhands (`font: caption`, a system font), is reported by a `setProperty` as `GTEXT_CSS_E_UNSUPPORTED` naming the property, and **kept as written** by a parse (a `cssText` set, an element's `style` attribute), with a count (`unjudgeable_preserved`): the cascade drops such a declaration for computing, but a declaration block keeps it, so editing another property of an element does not take it out of the `style` attribute. It is an opaque item of the block: in its place in the order, reported by `length` and `item()`, its value read back as it was written, its importance kept, written back verbatim, removed by `removeProperty` like any declaration, and replaced only by a parse of the same property. An opaque shorthand is atomic, since its longhands are not known: a later declaration of one of its longhands that the block holds before it is placed after it (an in-place replacement would be overridden by the shorthand), a declaration of the whole shorthand replaces it, and removing one longhand does not change it. **`all` is written as its longhands**, every property that is neither a shorthand, an alias, a custom property, `direction` nor `unicode-bidi` (CSS Cascade 5, "all"; about six hundred of them, and every property the caller registered, which the registry adds to them when it is frozen; a registry of more than about 3,400 registered properties holds more than the library reads for one shorthand, and a declaration of `all` is then refused with `GTEXT_CSS_E_LIMIT_DECLARATIONS` instead of leaving some out): `setProperty('all', 'revert')` and `all: revert` in a `cssText` set every one, in place of the declarations that were there; a property declared after it overrides it; `removeProperty('all')` removes every one of them and returns the keyword; `getPropertyValue('all')` is the keyword when every one of them is still the keyword of one `all` declaration and the empty string otherwise; `cssText` writes `all: revert` when that holds and the longhands one by one when it does not (a shorthand is also not written when a property of its logical property group with another mapping logic stands between its longhands in the block: CSSOM, "serialize into a shorthand form", so `margin-top`, `margin-inline-start`, `margin-bottom`, ... stay longhands, where Chromium writes the shorthand anyway; and `font` takes a `font-stretch` that is a calculation of a percentage a keyword names), as both browsers do (in the order and with the properties of the registry, which are not the browsers'). The change event of a mutation lists the longhands that changed, and `all_changed` is set when the list holds every longhand of `all`.

**A parsed list and `setProperty` are two algorithms.** `setProperty()` (and the other editors) replace a declaration **in its place**, as CSSOM's "set a CSS declaration" says. A list of declarations that is parsed (a `cssText` assignment, a `style` attribute, a rule's block) is read declaration by declaration, and **the later declaration of a property removes the earlier one and is appended**: `color:red;margin:1px;color:blue` is `margin: 1px; color: blue;`, and a shorthand that follows a longhand it sets goes after everything before it. CSSOM leaves this open; Chromium 156 and Firefox 158 give the same answer (the recorded cases are in `tools/oracle/css_cssom_gen.py`), so it is theirs. An important declaration is not replaced by one that is not, and keeps its place (Firefox 158's; Chromium moves it, which the differential lists as `browsers-differ`). The block of a rule that holds descriptors as well as properties (`@page { size: a4; margin: 1px }`) is read the same way for its properties, and a `cssText` assignment to it keeps the order of the text it was given when descriptors are in it (a limit: the rule keeps the text as given).

`gtext_cssom_decl_value_set()` returns the typed value of a declaration retained: it survives any later mutation and is valid on any thread. It is the typed access this layer gives in place of Typed OM.

### The IDL attribute names

`style.backgroundColor`, `style['background-color']`, `cssFloat`, the `webkit`-prefixed forms and the alias of every legacy property are a generated table (`tools/css/cssom_idl_names.tsv`, the "CSS property to IDL attribute" algorithm run over the registry's properties and checked against the names the pinned data lists). `gtext_cssom_idl_property()` maps an attribute name to its property; `gtext_cssom_decl_idl_get()` and `_idl_set()` are `getPropertyValue` and `setProperty(..., "")` through it. An attribute that is not in the table is not a CSS property, and a binding treats it as an ordinary property of the script object.

## An element's `style` attribute

`gtext_cssom_inline_style(document, element, binding, &decl)` returns the declaration of an element: the same object while anyone holds it. The attribute is its only storage, reached through the caller's `GTEXT_CSSOM_Attribute_Binding` (a get and a set function, and optionally the attribute's version). A mutation **writes the attribute back** with the block's serialization (an empty block sets the empty string; the attribute is never removed), and a read is **validated by content**: the declaration remembers the exact string it last parsed or wrote and each call that reads or edits it validates once, by one integer when the binding has an `attribute_version`, by nothing when the caller's tree version (`tree_version` in the config, or `gtext_cssom_document_tree_changed()`) has not moved, else by comparing the attribute and parsing it again if it differs. A write updates the remembered string, so a declaration never parses its own write. The costs are counters (`gtext_cssom_document_counters()`: `inline_reads`, `inline_compares`, `inline_reparses`, `inline_writes`), so a loop of `item(i)` over n declarations compares at most once.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The caller's element is anything; its style attribute is reached through two functions. */
typedef struct {
  char style[128];
} element;

static GTEXT_CSS_Status get_attribute(void * user, const void * e, const char ** value, size_t * length, bool * present) {
  const element * el = (const element *)e;
  (void)user;
  *value = el->style;
  *length = strlen(el->style);
  *present = el->style[0] != '\0';
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status set_attribute(void * user, const void * e, const char * value, size_t length) {
  element * el = (element *)e;
  (void)user;
  memcpy(el->style, value, length);
  el->style[length] = '\0';
  return GTEXT_CSS_OK;
}

/* An element's style object has no copy of the attribute that can disagree with it: a write goes to the attribute, and a change made behind its back is read on the next call. */
static void cssom_inline_style(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);

  element div;
  strcpy(div.style, "color: red; margin: 1px");
  GTEXT_CSSOM_Attribute_Binding binding;
  memset(&binding, 0, sizeof binding);
  binding.size = sizeof binding;
  binding.get_attribute = get_attribute;
  binding.set_attribute = set_attribute;
  GTEXT_CSSOM_Decl * style = NULL;
  gtext_cssom_inline_style(document, &div, &binding, &style);

  GTEXT_CSSOM_Outcome outcome;
  gtext_cssom_decl_set_property(style, "color", 5, "blue", 4, "", 0, &outcome);
  printf("attribute: %s (written: %s)\n", div.style, outcome.attribute_written ? "yes" : "no");
  gtext_cssom_decl_set_property(style, "color", 5, "blue", 4, "", 0, &outcome);
  printf("again: written: %s\n", outcome.attribute_written ? "yes" : "no");

  strcpy(div.style, "width: 5px");   /* setAttribute('style', ...) through the caller's own API */
  char * value = NULL;
  size_t length = 0;
  gtext_cssom_decl_get_property_value(style, "width", 5, &value, &length);
  printf("width: %s\n", value);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), value);
  gtext_cssom_decl_get_property_value(style, "color", 5, &value, &length);
  printf("color: \"%s\"\n", value);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), value);

  gtext_cssom_decl_release(style);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
```

prints

```
attribute: color: blue; margin: 1px; (written: yes)
again: written: no
width: 5px
color: ""
```

The outcome's `attribute_written` is a separate fact from whether anything changed: as served, the `cssText` setter always updates the attribute, `setProperty` only if something was updated, and `removeProperty` only if something was removed. `gtext_cssom_document_element_released()` is the caller's notice that a handle is about to be freed or reused: the declaration, if still held, keeps what it held and never calls the binding again, and a new handle at the same address gets a declaration of its own. Quirks mode (`quirks` in the config) lets an element-owned declaration read a unitless number as a length where the properties allow it, and nothing else.

## Style sheets and rules

A `GTEXT_CSSOM_Sheet` (`CSSStyleSheet`) is a live, reference counted object: the list of its rules (`cssRules`), the attributes CSSOM gives a sheet (`type`, `href`, `title`, `disabled`, `ownerNode`, a base URL that the library only hands back and never parses), the origin-clean and alternate flags, and a version that moves with every change to the sheet or to anything in it. It is a different thing from the immutable `GTEXT_CSS_Sheet` of \ref format_css_sheets "css-sheets.md": the live sheet is for editing and for binding, the compiled one is what the cascade reads. `gtext_cssom_sheet_new_parsed()` is CSS Syntax's "parse a stylesheet" and CSSOM's "create a CSS style sheet": every rule that is valid where it is becomes a rule object, built top-down without recursion.

A rule is one object, `GTEXT_CSSOM_Rule`, with a **kind** (the at-rule kinds of the at-rule model, `UNKNOWN` included, and the style rule and the nested-declarations rule: 32 in all), the legacy `type` number CSSOM lists for the rules it lists (0 for every newer one), and the CSSOM interface name it stands for (`CSSLayerBlockRule` and `CSSLayerStatementRule` are one kind, told apart by `has_block`; so are the two interfaces of `@apply` and the two of `@contents`). The kinds table is data (`tools/css/cssom-supplement.tsv`, checked against the ledger of at-rules by `make check-cssom`). A kind no browser exposes (`@when`, `@else`, `@custom-media`, the preserved ones) is a generic rule: in `cssRules`, inactive.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Parse a sheet, walk its rules, edit the declarations of one of them, and give back the memory of a sheet most of whose rules are gone. */
static void cssom_sheet(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);

  const char * text = "a { color: red }\n@media print { b { width: 1px } }\nbogus >{ }\n";
  GTEXT_CSSOM_Sheet * sheet = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &sheet);

  const GTEXT_CSSOM_Rule_List * rules = NULL;
  gtext_cssom_sheet_rules(sheet, &rules);
  printf("rules: %zu, dropped: %zu\n", gtext_cssom_rule_list_length(rules), gtext_cssom_sheet_diagnostic_total(sheet));
  for (GTEXT_CSSOM_Rule * rule = gtext_cssom_rule_list_first(rules); rule;) {
    GTEXT_CSSOM_Source_Info where;
    gtext_cssom_rule_source(rule, &where);
    printf("%s (type %u) at line %zu\n", gtext_cssom_rule_interface_name(rule), gtext_cssom_rule_type(rule), where.span.line);
    GTEXT_CSSOM_Rule * next = gtext_cssom_rule_list_next(rules, rule);
    gtext_cssom_rule_release(rule);
    rule = next;
  }

  /* cssRules[1].cssRules[0].style.setProperty('width', '2px'): the rule inside the @media rule, and the sheet, are changed */
  GTEXT_CSSOM_Rule * media = gtext_cssom_rule_list_at(rules, 1);
  GTEXT_CSSOM_Rule * inner = gtext_cssom_rule_list_at(gtext_cssom_rule_css_rules(media), 0);
  GTEXT_CSSOM_Decl * style = NULL;
  gtext_cssom_rule_style(inner, &style);
  GTEXT_CSSOM_Outcome outcome;
  gtext_cssom_decl_set_property(style, "width", 5, "2px", 3, "", 0, &outcome);
  char * css = NULL;
  size_t length = 0;
  gtext_cssom_decl_css_text(style, &css, &length);
  GTEXT_CSSOM_Sheet * owner = gtext_cssom_rule_parent_sheet(inner);
  printf("style: %s (sheet version %llu, owner is the sheet: %s)\n", css, (unsigned long long)gtext_cssom_sheet_version(sheet), owner == sheet ? "yes" : "no");
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), css);
  gtext_cssom_sheet_release(owner);

  /* a held rule keeps its sheet: let go of the sheet and the document handle, and read through the rule */
  gtext_cssom_decl_release(style);
  gtext_cssom_rule_release(media);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_document_release(document);
  GTEXT_CSSOM_Sheet * still = gtext_cssom_rule_parent_sheet(inner);
  const GTEXT_CSSOM_Rule_List * again = NULL;
  gtext_cssom_sheet_rules(still, &again);
  printf("after the release: %zu rules\n", gtext_cssom_rule_list_length(again));
  gtext_cssom_sheet_release(still);
  gtext_cssom_rule_release(inner);
  gtext_css_registry_free(registry);
}
```

The program above prints

```
rules: 2, dropped: 1
CSSStyleRule (type 1) at line 1
CSSMediaRule (type 4) at line 2
style: width: 2px; (sheet version 1, owner is the sheet: yes)
after the release: 2 rules
```

**What is not a rule.** `@charset` (the decoder has read the bytes by it), an at-rule the library does not know, a rule that is invalid where it is, and everything inside it: an `@import` or `@namespace` after the rules they must precede (an invalid rule is no rule, so it does not end the place of the imports, and neither does a `@layer` statement), a style rule whose selector list is not valid (for a nested rule, as a relative selector list; a namespace prefix is valid only if an `@namespace` before it declared it), a keyframe whose selector is not `from`, `to` or a percentage, and an at-rule whose own grammar refuses its prelude, its block or its place. Each is a diagnostic with a kind, a span and the sentence of the standard that decides it (`gtext_cssom_sheet_diagnostic()`), and the parse still succeeds. The standard's "invalid rule error" is applied as it says, so the declarations on both sides of an invalid nested rule are one run, not two.

**Nesting.** A style rule's `style` holds the declarations before its first nested rule; the declarations after a nested rule are a nested-declarations rule in its place. A grouping rule inside a style rule holds nested-declarations rules for its declarations; the same grouping rule at the top level holds none (declarations are not valid there). `@keyframes` holds keyframe rules, and `@page` its margin rules.

**Identity and lifetime.** The same rule is returned every time it is asked for (`cssRules[0] === cssRules[0]`), for as long as it lives. Every call that returns an object returns it **retained**, so nothing a caller holds can be invalidated by a later mutation; a list (`CSSRuleList`) is a view of its owner and allocates nothing. A rule's pointer to its parent is weak and `parentStyleSheet` is derived by following the parents, so removing a rule is one pointer and every rule under it reads no sheet at once; a removed rule is still a valid object. A held rule keeps its parents, and so its sheet, alive. The list of rules is an order-statistics treap whose nodes are the rules and whose shape depends only on the sequence of calls: `length` is O(1), `item(i)`, `index_of` and a removal are O(log n), a walk is O(1) a step, and nothing recurses.

**The declarations of a rule** (`gtext_cssom_rule_style()`) are the declaration object of the previous section, built the first time they are asked for: a parse that holds a million style rules builds none. Mutating them is a mutation of the rule and of its sheet (the rule's version, the sheet's version, the document's generation, one notification naming the rule and the sheet); if the rule has been removed from its sheet it changes the rule and nothing else, and no notification is sent. Only a style rule and a nested-declarations rule answer `style` so far: the other kinds that have a declaration block hold typed descriptors, and asking one for a `CSSStyleDeclaration` is `GTEXT_CSS_E_UNSUPPORTED`, not a guess.

**Positions.** A parsed rule has the span of its text (byte offsets, and the line and the column of the first byte, the column in UTF-16 code units) and the id of the source buffer it was read from, and says whether the position can be trusted (`PARSED`, `INSERTED`, `MODIFIED`). `gtext_cssom_rule_source_text()` and `gtext_cssom_sheet_source_text()` return a copy of the bytes.

**One syntax tree per parse, and compaction.** Each parse is one reference counted syntax tree (`gtext_css_tree_retain()`; a tree is immutable, so sharing it is safe) and one copy of its text, held by every rule of that parse. The tree lives while any rule of it is attached or held, so deleting most of a very large sheet frees nothing until the last rule of that parse goes; the bytes of the trees count against the document's live-bytes limit (`tree_bytes` of the counters). `gtext_cssom_sheet_compact()` copies what remains into one new tree and releases the old ones, in time proportional to what remains; no id, identity or version changes, and the spans of the compacted rules become `MODIFIED`.

**Limits.** `max_rules` (rules in a sheet, at every depth, counting every rule the parse makes), `max_rule_nesting` (default 64, at most 4,096), `max_input_bytes`, `max_work`, `max_live_objects` and `max_live_bytes` are fields of the config; each is its own status and an exact answer: the parse fails whole and leaves nothing. A nest a hundred thousand deep is a status (the parser's depth limit) and not a crash, and a sheet of a million rules is held and freed without a call that recurses. `gtext_cssom_sheet_rules()` of a sheet whose origin-clean flag is unset is `GTEXT_CSS_E_SECURITY`. `gtext_cssom_sheet_dump()` and `gtext_cssom_rule_dump()` write a sheet or a rule with everything under it in a canonical, versioned text (`cssom-sheet 1`, `cssom-rule 1`) that reads and builds nothing, with a SHA-256 of each.

## Inserting and deleting rules, and writing them

`gtext_cssom_sheet_insert_rule()` is `CSSStyleSheet.insertRule()`, `gtext_cssom_sheet_delete_rule()` is `deleteRule()`, and `gtext_cssom_rule_insert_rule()` and `_delete_rule()` are the same two for the rules inside a style rule, a grouping rule (`@media`, `@supports`, `@container`, `@starting-style`, `@scope`, a `@layer` with a block) and `@page`. The legacy `addRule()` and `removeRule()` are `gtext_cssom_sheet_add_rule()` and `_remove_rule()`; `rules` is `gtext_cssom_sheet_rules()`. A rule inserted is a rule object at once, with everything inside it, and `rule_out` returns it retained; a rule deleted is detached, with everything inside it (`parentStyleSheet` and `parentRule` read null at once, the object stays valid and readable, its id is unchanged), and a change to a detached rule changes the rule and nothing else.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

struct Out {
  char data[512];
  size_t used;
};

static GTEXT_Markup_Status collect(void * user, const char * bytes, size_t n) {
  struct Out * o = (struct Out *)user;
  memcpy(o->data + o->used, bytes, n);
  o->used += n;
  return GTEXT_MARKUP_OK;
}

/* An editor's round trip: parse a sheet, insert and delete rules, read the exceptions, write the sheet back. */
static void cssom_insert(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);

  const char * text = "@namespace s url(urn:s);\na { color: red }\n";
  GTEXT_CSSOM_Sheet * sheet = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &sheet);

  /* sheet.insertRule("@media print { s|b { width: 1px } }", 2): the rule, and the rule inside it, are objects at once */
  const char * media = "@media print { s|b { width: 1px } }";
  GTEXT_CSSOM_Rule * made = NULL;
  GTEXT_CSS_Status status = gtext_cssom_sheet_insert_rule(sheet, media, strlen(media), 2, &made, NULL);
  printf("insert media: %s\n", gtext_css_status_string(status));

  /* the exceptions have names a binding throws: an @import after a style rule, a prefix nobody declared, text that is not a rule, an index past the end */
  const char * bad[] = {"@import url(a.css);", "t|c { }", "a { } b { }", "d { }"};
  const size_t where[] = {2, 2, 0, 9};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    status = gtext_cssom_sheet_insert_rule(sheet, bad[i], strlen(bad[i]), where[i], NULL, NULL);
    printf("%s -> %s\n", bad[i], gtext_cssom_exception_name(status));
  }

  /* into a style rule: a block of declarations becomes a nested-declarations rule, a relative selector gets its & */
  GTEXT_CSSOM_Rule * a = NULL;
  const GTEXT_CSSOM_Rule_List * rules = NULL;
  gtext_cssom_sheet_rules(sheet, &rules);
  a = gtext_cssom_rule_list_at(rules, 1);
  gtext_cssom_rule_insert_rule(a, "> c { }", 7, 0, NULL, NULL);
  gtext_cssom_rule_insert_rule(a, "width: 2px", 10, 1, NULL, NULL);

  /* deleteRule detaches the rule: it stays readable, and no sheet holds it */
  GTEXT_CSSOM_Outcome outcome;
  gtext_cssom_sheet_delete_rule(sheet, 2, &outcome);
  char * css = NULL;
  size_t length = 0;
  gtext_cssom_rule_css_text(made, &css, &length);
  GTEXT_CSSOM_Sheet * parent = gtext_cssom_rule_parent_sheet(made);
  printf("deleted: %s (parentStyleSheet is %s)\n", css, parent ? "set" : "null");
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), css);

  /* the whole sheet, as text */
  struct Out out;
  memset(&out, 0, sizeof out);
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = collect;
  gtext_cssom_sheet_serialize(sheet, &sink);
  printf("%.*s", (int)out.used, out.data);

  gtext_cssom_rule_release(a);
  gtext_cssom_rule_release(made);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
```

The program above prints

```
insert media: success
@import url(a.css); -> HierarchyRequestError
t|c { } -> SyntaxError
a { } b { } -> SyntaxError
d { } -> IndexSizeError
deleted: @media print {
  s|b { width: 1px; }
} (parentStyleSheet is null)
@namespace s url("urn:s");
a {
  color: red;
  & > c { }
  width: 2px;
}
```

**Every failure is a status, and an exact one.** The eight DOMException names of the web platform are eight statuses (`GTEXT_CSS_E_SYNTAX`, `_HIERARCHY`, `_INDEX_SIZE`, `_INVALID_STATE`, `_NOT_ALLOWED`, `_NO_MODIFICATION`, `_NOT_FOUND`, `_SECURITY`) and `gtext_cssom_exception_name()` gives the name a binding throws; `GTEXT_CSS_E_INVALID` is a misuse of the API and never a content error. A call that fails changes nothing: the sheet, its versions, its ids, the document's counters and its observers' view are as they were, an allocation failure and every limit included.

**The order of checks is part of the contract**, and the first error wins. `CSSStyleSheet.insertRule()`: a call from inside an observer is `GTEXT_CSS_E_REENTRANT`; then `SecurityError` (the sheet is not origin-clean), `NotAllowedError` (the disallow-modification flag), `SyntaxError` for a text that is not exactly one valid rule (this is "parse a rule", and it comes **before** the index: a bad index with unparseable text is a SyntaxError, which is what the draft says, though both Chromium 156 and Firefox 158 look at the index first), `SyntaxError` for an `@import` into a constructed sheet, `IndexSizeError`, `HierarchyRequestError` for a rule that CSS does not allow at that place, and `InvalidStateError` for an `@namespace` into a list that holds anything but `@import` and `@namespace` rules. `deleteRule()`: `SecurityError`, `NotAllowedError`, `IndexSizeError`, `InvalidStateError` (an `@namespace` rule, when the list holds anything else). A rule's `insertRule()` looks at the index **first**, before the text; then the text is parsed as one rule; if it is not one and the rule is inside a style rule (the **nested flag**: the style rule itself, or a grouping rule inside one, at any depth), the text is read as a list of declarations and a non-empty result becomes a nested-declarations rule (`color:red` into a style rule), an empty one being a SyntaxError; then `HierarchyRequestError`. Outside a style rule a bare declaration block is a SyntaxError (the pinned web-platform-tests page and both browsers; the prose of the draft says only "with the nested flag set"). An at-rule that is valid at the top of a sheet and not where it is inserted (`@import` or `@namespace` in a group, `@font-face` in a style rule) is a HierarchyRequestError, as both browsers answer; an at-rule that is valid nowhere (`@top-left` in a group, an unknown one, `@charset`) is a SyntaxError.

**The order of a sheet's top** is CSS Cascade 5 and CSS Namespaces 3: an `@import` precedes every other rule except `@layer` statements; an `@namespace` follows the imports and precedes every other rule; a rule other than those precedes neither. The checks are counts kept in the rule list (the number of rules of each class in every subtree), so an insertion and a deletion are O(log n) however many imports a sheet has. An inserted `@namespace` rule declares its prefix for the selectors inserted after it, and a deleted one stops declaring it; a style rule keeps the namespaces it was made in, so `x|a` is written `x|a` after the rule has left its sheet.

**Relative selectors** (`> a`, `&`, an implied `&`) are valid only inside a style rule, or directly in an `@scope`; a selector with a namespace prefix is valid only if the sheet declared it. A nested selector is written with its `&` (`& > c`).

**Keyframes.** `gtext_cssom_keyframes_append_rule()` appends a keyframe, whatever keys the rule has; text that is not one valid keyframe is **ignored** (the call succeeds, the outcome says `IGNORED_INVALID_SELECTOR`). `_find_rule()` and `_delete_rule()` match the **last** keyframe whose selector list is the one given (white space does not matter, `from` and `to` are `0%` and `100%`, the number and the order of the values must match); no match is nothing.

**`cssText`** (`gtext_cssom_rule_css_text()`, or `gtext_cssom_rule_serialize()` to a sink) is CSSOM's "serialize a CSS rule", one function per kind over the serializers of the layers below. A style rule is its selector list, ` {`, its declarations, and the rules inside it, each after a newline and two spaces, then a newline and `}`; with nothing inside it is `a { }`, and with declarations only `a { color: red; }`. A grouping rule is `@media print {`, the rules inside, a newline and `}`. A rule inside a rule is **not** indented again, which is what the draft says and both browsers write. `@import` and `@namespace` write their URL as `url("...")`. The other rules are the typed model's serialization (\ref format_css_atrules "css-atrules.md"). Writing is iterative: a nest of any depth, or a rule with a hundred thousand rules inside it, is written with no recursion in the C stack. A rule with no rules inside it keeps its text for as long as its version stays (an unchanged leaf is O(1) to reach and O(length) to copy); a rule with rules inside it keeps nothing, and neither does the serialization of a sheet or through a sink. The kept texts count in `live_bytes`, are bounded by `max_cached_text_bytes` (default 64 MiB; past it the entries added first are dropped, and a text too long to keep is not an error), and are dropped when the rule's version moves.

`gtext_cssom_sheet_serialize()` writes the `cssText` of every rule, each followed by a line feed. Parsing the text gives a sheet that writes the same bytes, for every sheet the parse can make except where the syntax layer owns the answer (a rule closed by the end of the input, a custom property value holding `{` or `(`).

**Where this library and the browsers differ**, and which of them it follows. The sheet's text follows the served draft: the parse before the index (above); a media query list is written in its canonical form (the pinned `serialize-media-rule.html` says so) where a `@supports` condition keeps the author's spacing, as both browsers do; `@keyframes` is written one rule to a line, indented two spaces, where Chromium writes `@keyframes k { ` and Firefox does not indent. A style rule directly in an `@scope` that begins with a combinator is written as the browsers write it (`> a`); the prelude of an `@scope` inside a style rule is written without the `&` it is read with. `insertRule` into a detached rule works (Firefox 158 throws). An `@page` takes margin rules with `insertRule`; a style rule into it is a HierarchyRequestError (Chromium answers SyntaxError, Firefox accepts it).

## The attributes of a rule, media lists and constructed sheets

**A rule whose syntax is immutable is replaced as a whole.** `gtext_cssom_rule_set_selector_text()` (`selectorText` of a style rule and of `@page`), `_set_key_text()` (`keyText`) and `_set_attribute()` (the writable attributes: a `@counter-style`'s name and descriptors, a `@keyframes` name, the family list of `@font-feature-values`) write the candidate text of the rule's own part, read it again in the place the rule stands in (a nested rule's selector is relative, a prefix must have been declared), and swap the rule's syntax for the result only if it is a rule of the same kind that holds what was set. The rule's identity, its place, the rules inside it and its ids do not change; its source becomes `MODIFIED`; the version of the rule and of its sheet moves once and one `RULE_REPLACED` event is sent. The cost is the size of that one rule, and a call that fails (an allocation included) changes nothing, ids included. A selector that does not parse is **ignored** (CSSOM: "do nothing"; the outcome says `IGNORED_INVALID_SELECTOR`). A key list that does not parse is a `SyntaxError` (`GTEXT_CSS_E_SYNTAX`; CSS Animations 1 says so, the pinned web-platform-tests page and both browsers throw). A value a descriptor refuses is ignored (`IGNORED_INVALID_VALUE`), the empty string is not a value of a descriptor and does nothing, and a readonly attribute is ignored as WebIDL says (`IGNORED_NOT_ALLOWED_HERE`): `conditionText` is readonly (CSS Conditional 3, and the pinned page asserts assigning to it does nothing). `gtext_cssom_rule_get_attribute()` reads the rest of the attributes CSSOM names by their IDL name (`href`, `layerName`, `supportsText`, `namespaceURI`, `prefix`, `start`, `end`, `containerName`, `containerQuery`, `syntax`, `inherits`, `initialValue`, `navigation`, `types`, `fontFamily`, `basePalette`, `overrideColors`, the descriptors of `@counter-style`, `name`); `*is_null` is true where the IDL says nullable and the attribute is absent. The three descriptors of `@color-profile` (`src`, `renderingIntent`, `components`) are readonly attributes like the rest.

**`CSSFontFeatureValuesMap`.** The seven value maps of an `@font-feature-values` rule (`annotation`, `ornaments`, `stylistic`, `swash`, `characterVariant`, `styleset`, `historicalForms`) are views of the rule, not objects: `gtext_cssom_feature_map_size()` / `_at()` / `_get()` read the entries of one map from the rule's typed model every time (so two retrievals are the same data and a map of another rule is untouched), and `_set()` / `_delete()` / `_clear()` rewrite the rule as a whole with the machinery above: the rule keeps its identity, its version and its sheet's move once, one `RULE_REPLACED` is sent, a write that says what the map says is `UNCHANGED`. The blocks of one type are written back as one block holding the last-defined value of each name (CSS Fonts 4, 13.2), a name that is set again keeps its place, and the feature blocks survive a `fontFamily` assignment. `set()` of a sequence longer than the block takes (one value, two for `character-variant`, any number for `styleset`) or of an empty one is `GTEXT_CSS_E_INVALID_ACCESS` ("throws an InvalidAccessError exception"); a name an identifier cannot carry (the empty name) is ignored (`IGNORED_INVALID_VALUE`).

**The rules of CSS Functions and Mixins 1: `@mixin`, `@apply` and `@contents`.** `@mixin --name(<parameters>) { <body> }` is a `CSSMixinRule`, a grouping rule whose body is read as a style rule's is ("The body of a mixin is parsed as a nested declarations rule; it accepts properties, conditional group rules, and nested style rules, just like an ordinary style rule would"): the declarations of it are `CSSNestedDeclarations` rules in its `cssRules`, a selector in it is a relative one, and an `@layer` anywhere in it, at any depth, is invalid and dropped ("@layer is not valid within a mixin body"). A `@mixin` is valid where a `@keyframes` is (the top of a sheet and a group rule, never in a style rule or another mixin), its name is a function token or an identifier that begins with two dashes (the parentheses are optional), a parameter is a `--name`, an optional type and an optional default that must be a value of the type, and no name is twice. `@apply --name(<arguments>);` and `@apply --name(<arguments>) { <contents> }` are `CSSApplyStatementRule` and `CSSApplyBlockRule`, valid only in the body of a style rule, of a mixin or of a group rule inside one; `@contents;` and `@contents { <fallback> }` are `CSSContentsStatementRule` and `CSSContentsBlockRule`, valid only inside a mixin body. The block forms are grouping rules, the statement forms have no `cssRules`; `insertRule()` reads an `@apply`, an `@contents` and (in a mixin) an `@layer` in the place it is inserted at, so `insertRule('@apply --m;')` at the top of a sheet is a `SyntaxError`. `cssText` is the draft's own serialization, in which the rules inside a mixin, an `@apply` block or an `@contents` block are joined by a single space and not set on lines of their own (`@mixin --m(--a <length>: 1px) { color: red; & b { color: blue; } }`; the draft writes no space before the brace of a mixin, the other three algorithms have one, and it is written). `gtext_cssom_rule_get_attribute()` has `name` of a mixin and of an `@apply` (serialized as an identifier) and `contents` of a mixin (`true` when a `@contents` is anywhere inside it); `gtext_cssom_mixin_get_parameters()` and `gtext_cssom_apply_get_arguments()` are `getParameters()` and `getArguments()`. **Nothing applies a mixin yet**: the rules are recognized and inactive, an `@apply` changes no style, and the body of a mixin is never cascaded. **There is no `@result` rule**: the grammar, the IDL and the CSSOM section of the pinned draft have none, its section on defining mixins and its examples write the body as a style rule's, and the one section that still writes `@result` begins "Issue: TODO rewrite to use the new @private concepts", so a `@result` in a mixin body is an unknown at-rule and is dropped; most of the pinned web-platform-tests pages of the mixins are written with it (an earlier draft), and neither Chromium 156 nor Firefox 158 implements any of this, so there is no browser to ask.

**`@private`** (CSS Functions and Mixins 1, "Private Custom Properties") is valid as a child of a style rule or of a conditional group rule of one and has no interface in the pinned IDL, so it is a rule of a style rule's `cssRules` that has no interface of its own (`gtext_cssom_rule_interface_name()` is `CSSRule`, `cssRules` is not a member). What is inside it is what makes it a rule, so its text keeps the contents as the syntax serializes them (`@private { --x:1; @property --a <length>: 2px; @media print {--y:3;} }`), a rule of a sheet that has been changed is read again from its text, and what is inside is not a rule of the sheet. `@property` is valid directly inside it and nowhere else in a style rule. Its custom properties are **private properties** of the style rule: see the manual of the custom properties for what they are in the cascade, and for what the computed style does not show of them.

**`style` of a keyframe, a page or margin rule, `@font-face`, `@position-try`, `@view-transition`, `@counter-style`, `@font-palette-values` and `@color-profile`** is a block that is a view of the rule: the declarations that are properties are in a declaration block (so `margin: auto` is four declarations, written as the shorthand), the descriptors that are not properties (`src`, `size`, `symbols`) follow them. Every edit is made on a copy and kept only if the rule reads again, from the text the copy writes, with what was set: `animation-name` in a keyframe, a `src` that is not a source list, a `!important` descriptor, and a property a page does not take are refused with nothing changed. An `@page` takes the descriptors of the pin and the page properties of Paged Media 3, Appendix A ("CSS 2.1 Properties that apply within the page context": margin, border, padding, background, color, font, text, width, height and the rest of the list), and a custom property; `gtext_cssom_decl_has_attribute()` says which IDL attributes such a block has (a page has `marginTop` and `size` and no `cssFloat`). The rule keeps the order of the text it was last written from; the block lists properties before descriptors.

**Media lists.** `gtext_cssom_sheet_media()` and `gtext_cssom_rule_media()` (an `@import` or an `@media`) return the list, the same object every time, which is a view of its owner and keeps it reachable. `mediaText` is the owner's media query list serialized; setting it **always parses** (a query that is not valid is `not all`, Media Queries 4, 3.2; the empty text is the empty list, which matches everything); `appendMedium()` and `deleteMedium()` parse their argument as CSSOM 4.1 does ("return null if more than one media query is returned"; the empty text returns none): a text that is not exactly one query does nothing, a query that is not valid is not null but `not all` ("a media query that ends up being 'ignored' will turn into 'not all'") and is appended, or deleted, as such; `appendMedium()` does not add a medium whose serialization is already there, and `deleteMedium()` of one that is not there is a `NotFoundError`. The media of an `@import` are the queries after its URL, `layer()` and `supports()`. **The text is written as it was read**, as Chromium and Firefox write it: a `<general-enclosed>` keeps its white space, comments and escapes, and one that the end of the input closed stays unclosed (`screen and (` reads back as `screen and (`). The rule stores the closed text, `screen and ()`, because an unclosed parenthesis in a prelude would take the `{` after it, and keeps the text as read beside it; the text as read ends with the next change of the list or of the rule. `appendMedium()` and `deleteMedium()` compare a medium with the items as they were read, so `screen and (` and `screen and ()` are two media; `matchMedia(text).media` is the text as read. A rule read from a style sheet's text (a parse, `insertRule()`, `replaceSync()`) keeps the text of its `<general-enclosed>` as it was written in the same way, in `@media`, `@import` and `@container` (`conditionText` of `@media screen and ( a )` is `screen and ( a )`, of `@container  foo ( a:  b )` is `foo ( a:  b )`): the at-rule model keeps the queries that differ (`gtext_css_atrule_prelude_read()`) when it is given the text the rule's spans index (`GTEXT_CSS_Atrule_Options::source`), and the rule keeps them beside its text from the moment it is made, so a compaction of the sheet does not lose them. A feature the library knows is written from its parts (`(min-width:  1px)` is `(min-width: 1px)`), and an `@supports` condition is not this.

**Constructed sheets.** `gtext_cssom_sheet_new_constructed()` is `new CSSStyleSheet()`: empty, `constructed`, no owner node. `gtext_cssom_sheet_replace_sync()` is `replaceSync()`: the text is read as a style sheet, its `@import` rules are dropped (a diagnostic each), and the rest become the sheet's rules while **every old rule is detached, the nested ones too** (`parentStyleSheet` reads null at once). It is `NotAllowedError` on a sheet that is not constructed. `replace()` is its two synchronous steps, `_replace_begin()` (sets disallow-modification: `insertRule` and `deleteRule` are `NotAllowedError` until it ends) and `_replace_complete()`; the promise is the caller's. A replace reads the text into a sheet of its own first, so a failure changes nothing.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static void show(const char * label, GTEXT_CSS_Status status, char * text, GTEXT_CSSOM_Document * document) {
  printf("%s: %s\n", label, status == GTEXT_CSS_OK ? text : gtext_css_status_string(status));
  if (status == GTEXT_CSS_OK) {
    gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), text);
  }
}

/* An editor's panel: change a selector, a counter style and a media list, read an @page block, and replace a constructed sheet. */
static void cssom_setters(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);

  const char * text = "a, b { color: red } @media screen { c {} } @counter-style n { system: cyclic; symbols: a b } @page { size: a3; margin: 1em 2em }";
  GTEXT_CSSOM_Sheet * sheet = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &sheet);
  const GTEXT_CSSOM_Rule_List * rules = NULL;
  gtext_cssom_sheet_rules(sheet, &rules);

  /* style.selectorText = "d > e": the rule is replaced as a whole, and keeps its identity; text that is not a selector is ignored */
  GTEXT_CSSOM_Rule * a = gtext_cssom_rule_list_at(rules, 0);
  GTEXT_CSSOM_Outcome outcome;
  GTEXT_CSS_Status status;
  gtext_cssom_rule_set_selector_text(a, "d > e", 5, &outcome);
  printf("selectorText: changed %d\n", outcome.kind == GTEXT_CSSOM_CHANGED);
  gtext_cssom_rule_set_selector_text(a, "d >", 3, &outcome);
  printf("selectorText: ignored %d\n", outcome.kind == GTEXT_CSSOM_IGNORED_INVALID_SELECTOR);
  char * css = NULL;
  size_t length = 0;
  status = gtext_cssom_rule_css_text(a, &css, &length);
  show("cssText", status, css, document);

  /* media.mediaText, appendMedium(), deleteMedium(): a text always parses, a medium that is not valid is `not all`, a text that is not one query does nothing, a medium that is not there is a NotFoundError */
  GTEXT_CSSOM_Rule * media = gtext_cssom_rule_list_at(rules, 1);
  GTEXT_CSSOM_Media_List * list = NULL;
  gtext_cssom_rule_media(media, &list);
  gtext_cssom_media_set_text(list, "print, (bogus: 1) and", 21, NULL);
  gtext_cssom_media_append(list, "tv", 2, NULL);
  status = gtext_cssom_media_delete(list, "handheld", 8, NULL);
  printf("deleteMedium: %s\n", gtext_cssom_exception_name(status));
  status = gtext_cssom_media_text(list, &css, &length);
  show("mediaText", status, css, document);

  /* a counter style's descriptors are attributes; a value that is not valid for one (the empty string too) does nothing */
  GTEXT_CSSOM_Rule * counter = gtext_cssom_rule_list_at(rules, 2);
  gtext_cssom_rule_set_attribute(counter, "symbols", 7, "x y z", 5, NULL);
  gtext_cssom_rule_set_attribute(counter, "system", 6, "bogus (", 7, &outcome);
  printf("system: ignored %d\n", outcome.kind == GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  status = gtext_cssom_rule_css_text(counter, &css, &length);
  show("counter style", status, css, document);

  /* the block of an @page rule: margins are properties, size is a descriptor; what a page does not take is refused */
  GTEXT_CSSOM_Rule * page = gtext_cssom_rule_list_at(rules, 3);
  GTEXT_CSSOM_Decl * block = NULL;
  gtext_cssom_rule_style(page, &block);
  size_t count = 0;
  gtext_cssom_decl_length(block, &count);
  gtext_cssom_decl_set_property(block, "transform", 9, "none", 4, "", 0, &outcome);
  printf("page block: %zu declarations, transform ignored %d\n", count, outcome.kind == GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  gtext_cssom_decl_set_property(block, "size", 4, "landscape", 9, "", 0, NULL);
  status = gtext_cssom_rule_css_text(page, &css, &length);
  show("page", status, css, document);

  /* new CSSStyleSheet() and replaceSync(): the @import is dropped, and every old rule is detached, the nested ones too */
  GTEXT_CSSOM_Sheet * constructed = NULL;
  gtext_cssom_sheet_new_constructed(document, NULL, &constructed);
  gtext_cssom_sheet_replace_sync(constructed, "x { y { } }", 11, NULL);
  const GTEXT_CSSOM_Rule_List * crules = NULL;
  gtext_cssom_sheet_rules(constructed, &crules);
  GTEXT_CSSOM_Rule * old = gtext_cssom_rule_list_at(crules, 0);
  gtext_cssom_sheet_replace_sync(constructed, "@import 'a.css'; z { }", 22, NULL);
  GTEXT_CSSOM_Sheet * parent = gtext_cssom_rule_parent_sheet(old);
  printf("old rule: parentStyleSheet is %s; sheet rules: %zu\n", parent ? "set" : "null", gtext_cssom_rule_list_length(crules));
  status = gtext_cssom_sheet_replace_sync(sheet, "a {}", 4, NULL);
  printf("replaceSync on a parsed sheet: %s\n", gtext_cssom_exception_name(status));

  gtext_cssom_rule_release(old);
  gtext_cssom_sheet_release(constructed);
  gtext_cssom_decl_release(block);
  gtext_cssom_rule_release(page);
  gtext_cssom_rule_release(counter);
  gtext_cssom_media_release(list);
  gtext_cssom_rule_release(media);
  gtext_cssom_rule_release(a);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
```

prints

```
selectorText: changed 1
selectorText: ignored 1
cssText: d > e { color: red; }
deleteMedium: NotFoundError
mediaText: print, not all, tv
system: ignored 1
counter style: @counter-style n { system: cyclic; symbols: x y z; }
page block: 5 declarations, transform ignored 1
page: @page { margin: 1em 2em; size: landscape; }
old rule: parentStyleSheet is null; sheet rules: 1
replaceSync on a parsed sheet: NotAllowedError
```

## Snapshots and imports

The cascade reads a `GTEXT_CSS_Sheet` (\ref format_css_sheets "css-sheets.md"), which is immutable, atomically counted and shareable between threads; a live sheet is for editing. `gtext_cssom_sheet_snapshot()` is the bridge: it returns a reference to the compiled form of the sheet's **current** state, and **the same object for as long as nothing in the sheet, or in the sheets it imports, changes**, so any number of readers cost one compile. A snapshot, once handed out, is never altered by a later mutation (a test holds one across 10^5 of them) and is valid on any thread for as long as its holder keeps it, the sheet and its document included. Asking changes nothing: no version, no generation, no event. A sheet that is not origin-clean or is disabled is snapshotted all the same (the first guards the script's access to the rules, the second is the document's decision about whether the sheet takes part).

**What is compiled.** The compile of \ref format_css_sheets "css-sheets.md" over an array of sources (`gtext_css_sheet_compile_rules()`: each top-level rule with the syntax tree it was read from, because a live sheet's rules come from different parses). A rule that has not been changed since it was read, **and whose own syntax is what the text the object model writes for it reads back as**, is compiled from that syntax, so its spans stay (a record says which source they index, `source_id`, which is the rule's `gtext_cssom_rule_source()`). A rule that was changed, or has a changed rule inside it, or whose declarations the object model writes differently from the text (`margin: 1px; margin-top: 2px` is one `margin` in a declaration block; a property written twice is written once) is compiled from the parse of its own `cssText`, whose bytes are not kept (`source_id` 0). Which of the two a rule is, is decided once and remembered. So **the dump of a snapshot is the dump of the compile of the sheet's serialization, after every step of every sequence of mutations** (`snapshot(live) == compile(serialize(live))`), and the cascade sees exactly the state the object model shows. The counters `snapshot_compiles`, `snapshot_hits`, `snapshot_rules_reused`, `snapshot_rules_reparsed` and `snapshot_rules_checked` say what a call did.

**What it costs: the known quadratic shape.** A compile is O(the sheet): every top-level rule is walked, and the ones changed since the last are parsed from their text again. So *n* mutations and then one ask is one compile (O(n + S) for S rules), and *n* times "one mutation, one ask" is *n* compiles of S rules each, *n* x S, with only the changed rule parsed each time. The library does not hide it (`tests/test-cssom-snapshot-cost.cpp` asserts both figures exactly); an incremental update of the compiled sheet, which is not built, would remove it. A change in one sheet compiles that sheet only; a rule that was not changed is not read again when another is.

**Imports are single-owner.** `gtext_cssom_import_rule_complete(rule, sheet, outcome)` gives an `@import` rule the sheet the caller loaded (the library never fetches), or takes it away with NULL; `gtext_cssom_import_rule_sheet()` and `gtext_cssom_sheet_owner_rule()` read the link both ways. The rule holds the sheet with an owning edge, so there is no strong cycle and no sheet has two owners: a sheet that an import already holds, that has an owner node, or that is constructed is refused (`GTEXT_CSS_E_INVALID_STATE`), a sheet that is, or reaches through its imports, the sheet the rule is in is refused as a cycle (`GTEXT_CSS_E_HIERARCHY`), and a caller that wants the same text in two places makes two sheets. The snapshot of the importer holds the **snapshot of the imported sheet**, one level down (the same object the imported sheet's own snapshot is while it is unchanged), through the compile's import callback; an import with no sheet contributes nothing and is a diagnostic. A held imported sheet keeps the import rule that owns it, and so the sheet that holds the rule. Imports nest to the sheet option `max_import_depth` (default 16, at most 64): one more is `GTEXT_CSS_E_LIMIT_RULE_NESTING`.

**The composite version.** Every change of a sheet or of anything in it gives the sheet a new *stamp* from a counter of the document that only rises, and `gtext_cssom_sheet_composite_version()` is the greatest stamp among the sheet and the sheets it imports at every level. It therefore changes exactly when something in them does (a change in an imported sheet is the importer's; replacing the imported sheet by one that was last changed long ago is a change of the importer, because completing the import is), and a snapshot is kept with the stamp it was made at. The value is otherwise opaque.

An allocation failure in a snapshot is `GTEXT_CSS_E_OOM` with the sheet, its cache and the document as they were; a call that finds its cache allocates nothing, and importing a sheet allocates nothing (`tests/test-cssom-snapshot-oom.cpp` fails every allocation in turn).

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The cascade reads a snapshot: the compiled form of a live sheet as it is now, the same object until something in the sheet changes. */
static void cssom_snapshot(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);

  const char * text = "@import url(base.css); a { color: red } @media print { b { width: 1px } }";
  GTEXT_CSSOM_Sheet * sheet = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &sheet);
  GTEXT_CSS_Sheet * first = NULL;
  gtext_cssom_sheet_snapshot(sheet, &first);
  printf("records: %zu, imports: %zu\n", gtext_css_sheet_record_count(first), gtext_css_sheet_import_count(first));

  /* the caller fetched base.css: the import rule holds the sheet it loaded, and the importer changed */
  const char * base = "c { margin: 0 }";
  GTEXT_CSSOM_Sheet * child = NULL;
  gtext_cssom_sheet_new_parsed(document, base, strlen(base), NULL, &child);
  const GTEXT_CSSOM_Rule_List * rules = NULL;
  gtext_cssom_sheet_rules(sheet, &rules);
  GTEXT_CSSOM_Rule * import = gtext_cssom_rule_list_at(rules, 0);
  gtext_cssom_import_rule_complete(import, child, NULL);

  GTEXT_CSS_Sheet * second = NULL;
  GTEXT_CSS_Sheet * again = NULL;
  gtext_cssom_sheet_snapshot(sheet, &second);
  gtext_cssom_sheet_snapshot(sheet, &again);
  printf("after the import: records %zu, imports %zu, imported records %zu\n", gtext_css_sheet_record_count(second), gtext_css_sheet_import_count(second),
      gtext_css_sheet_record_count(gtext_css_sheet_import(second, 0)));
  printf("asked again: %s object\n", again == second ? "the same" : "another");

  /* a change inside the imported sheet is a change of the sheet that imports it; a snapshot handed out is never altered */
  gtext_cssom_sheet_insert_rule(child, "d { top: 0 }", 12, 0, NULL, NULL);
  GTEXT_CSS_Sheet * third = NULL;
  gtext_cssom_sheet_snapshot(sheet, &third);
  printf("after a change in the import: %s object, imported records %zu; the second still has %zu\n", third == second ? "the same" : "a new",
      gtext_css_sheet_record_count(gtext_css_sheet_import(third, 0)), gtext_css_sheet_record_count(gtext_css_sheet_import(second, 0)));

  gtext_css_sheet_free(first);
  gtext_css_sheet_free(second);
  gtext_css_sheet_free(again);
  gtext_css_sheet_free(third);
  gtext_cssom_rule_release(import);
  gtext_cssom_sheet_release(child);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
```

prints

```
records: 2, imports: 0
after the import: records 2, imports 1, imported records 1
asked again: the same object
after a change in the import: a new object, imported records 2; the second still has 1
```

## The document, tree scopes and the index

A document is more than the owner of objects: it is what the cascade is computed for. `gtext_cssom_document_index()` returns its **frozen `GTEXT_CSS_Rule_Index`**, the one the engine's style computation reads.

**The first thing to know: the index is borrowed.** `gtext_cssom_document_index()` keeps the owner and returns a pointer that is valid until the next ask that has to build a new index, or until the document is released. A caller that holds it across an ask retains it (`gtext_css_rule_index_retain()`), and a thread that computes styles while the writer goes on takes its own owner with `gtext_cssom_document_index_retained()`; the count is atomic and the index, with the snapshots it holds, goes with the last owner. Everything else in this section is single-writer, like the rest of the object model.

**What is in it.** The user-agent and user sheets the caller registered (`gtext_cssom_document_register_sheet()`: compiled sheets, as given, first and in order of registration), then the author sheets: for each **tree scope** in order of its context number, the owner sheets of its `styleSheets` list in list order, then its **adopted** sheets. A sheet takes part as its snapshot when it is not `disabled`, its `media` is true for the document's conditions, and the document made it (an adopted sheet is a constructed sheet of the scope's document, checked when it is adopted, `GTEXT_CSS_E_NOT_ALLOWED` otherwise). A sheet listed more than once in an adopted list takes part once, **at its last position**: both Chromium 156 and Firefox 158 answer so, and the pinned `CSSStyleSheet-constructable-duplicate.html` asserts it. A sheet with a pending import takes part with the rules it has; the pending import contributes nothing until it is completed.

**Tree scopes.** The document makes scope 0 (`gtext_cssom_document_scope()`); `gtext_cssom_scope_new()` registers a shadow tree's with its handle, its parent scope, its **context number** (assigned in shadow-including tree order; 0 takes the next number in the order the scopes are made, which is right when a host's scope is made before its shadow trees) and, for adopted sheets, the shadow host as the implicit root of an `@scope` with no prelude. The library does not see the caller's tree, so `styleSheets` is what the caller puts there (`gtext_cssom_scope_insert_sheet()` at an index, in tree order of the owner nodes); a sheet taken out (`gtext_cssom_scope_remove_sheet()`, or its scope removed) **stays a valid, mutable object** with no owner node, leaves the cascade, and `insertRule` on it works and affects nothing. The registrations are checked by the index exactly as the rule index checks them: a shadow tree whose context is not greater than its host scope's is `GTEXT_CSS_E_INVALID` from the ask. `gtext_cssom_scope_reparent()` moves a scope to another document (a shadow root adopted by a template) and **keeps both its lists**: the new document's index takes the sheets that document made, the others stay listed, take part nowhere and take part again if the scope returns.

**It is cached, and it is rebuilt once.** The index is keyed on the greatest change stamp among every sheet the scopes list and the sheets they import (the composite versions of the previous section), the document's structure version (a scope, a list or an adopted list changed), its registration version and its conditions epoch. The same pointer is returned while the key is unchanged; any change builds one new index, lazily, at the next ask, **however many changes came before it**: *n* mutations and one ask are one build and one compile of each edited sheet, O(n + S). *n* times "one mutation, one ask" is *n* builds of the whole document, n x S records, and compiles only the sheet that was edited. That is this layer's known quadratic cost, stated here and asserted by `tests/test-cssom-document-cost.cpp` (`index_builds`, `snapshot_compiles`, `index_records_rebuilt`); an incremental update of the index, which is not built, would remove it, and the events already carry the delta it needs. A hit looks at the listed sheets (`index_key_sheets`) and builds nothing.

**The environment.** `gtext_cssom_document_set_conditions()` retains a new `GTEXT_CSS_Conditions` object (a resize, a scheme switch) and increases the generation. The next index is built for it, and **its snapshots are the same objects**: a compiled sheet keeps its conditions as data, so an environment change recompiles nothing. Then Media Queries 4's "evaluate media queries and report changes" runs: every `MediaQueryList` that has a listener is evaluated once (lists with the same text share the evaluation), a list whose answer changed calls its listeners in the order they were added, the lists in the order they were made, after the change is complete. A listener may read and may not mutate (`GTEXT_CSS_E_REENTRANT`); one added during the report is not called for it, one removed is not called; the first status a listener returns is `observer_status` of the outcome. The evaluation is done before anything changes, so a failure of it (a unit the environment cannot answer, memory) leaves the document as it was. `gtext_cssom_match_media()` is `matchMedia()` (an invalid query is `not all`, the empty text matches), `matches` is cached per conditions object, and `gtext_cssom_supports()` and `gtext_cssom_supports_text()` are `CSS.supports()` over the document's conditions. The conditions are the config's `conditions` (retained), or the registry's defaults. `gtext_cssom_document_conditions()` returns them **borrowed**, and the pointer is valid only until `set_conditions` replaces them or the document is released (the document lets go of the object it replaces); a caller that keeps it past such a call, as a binding does in a wrapper, takes `gtext_cssom_document_conditions_retained()`, a reference of its own that it gives back with `gtext_css_conditions_free()`. A listener, or an observer of the change, may release the document handle: no listener is called after it, as no observer is.

**Imports and the callback.** With `GTEXT_CSSOM_Config::import` the document asks for the sheet of each `@import` of a sheet made from text, and of an `@import` inserted by `insertRule` (asked last, when nothing else can fail, with the rule not yet in the sheet). The callback is given the rule's `href`, `media`, `supports()` and `layer` as CSSOM states them and the sheet's base URL (never parsed or joined), and answers with a new sheet (the rule takes it; the callback's reference is released), or `GTEXT_CSSOM_IMPORT_PENDING` (nothing contributes until `gtext_cssom_import_rule_complete()` gives the sheet, or none; `gtext_cssom_sheet_pending_imports()` is what a `replace()` promise waits on) or `GTEXT_CSSOM_IMPORT_FAILED`. Imports stay single-owner: a sheet that has an owner, or that would be its own import, is **released and recorded as a diagnostic** of the sheet (`GTEXT_CSSOM_PARSE_IMPORT_REFUSED`) and the import contributes nothing. While the callback runs a mutation is `GTEXT_CSS_E_REENTRANT` but the callback may make sheets, and a sheet it makes is asked for its own imports; the nesting is bounded at 64. A status other than `GTEXT_CSS_OK` from the callback fails the call that asked: a sheet being made is dropped, and an `insertRule` changes nothing (ids that the callback's own sheets took stay taken). A constructed sheet drops its `@import` rules and asks nobody.

An allocation failure in any of these calls is `GTEXT_CSS_E_OOM` with the document, its lists, versions, ids and events as they were (`tests/test-cssom-document-oom.cpp` fails every allocation in turn). The documented concurrency model, one writer thread and any number of readers on retained indexes, is run by `tests/test-cssom-threads.cpp` (ten thousand mutations, eight readers, in the ThreadSanitizer arm). Several documents on one registry are independent single-writer graphs (`tests/test-cssom-multidoc.cpp`).

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static GTEXT_CSS_Status on_change(void * user, GTEXT_CSSOM_Media_Query_List * list, bool matches) {
  (void)user;
  (void)list;
  printf("change: matches %s\n", matches ? "yes" : "no");
  return GTEXT_CSS_OK;
}

/* The document as the cascade sees it: a tree scope with an owner sheet and an adopted sheet, the frozen index built from them, and the environment changing. */
static void cssom_document(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Media_Environment narrow_env = gtext_css_media_environment_default(NULL);
  narrow_env.viewport.width = 800;
  GTEXT_CSS_Conditions_Options options = gtext_css_conditions_options_default();
  options.media = &narrow_env;
  GTEXT_CSS_Conditions * narrow = NULL;
  gtext_css_conditions_new(registry, &options, &narrow);

  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  config.conditions = narrow;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(document);

  /* document.styleSheets: the owner sheet is listed where its owner node is in the caller's tree */
  const char * text = "a { color: red } @media (min-width: 1000px) { b { width: 1px } }";
  GTEXT_CSSOM_Sheet * owner = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &owner);
  gtext_cssom_scope_insert_sheet(scope, owner, 0, NULL);
  const GTEXT_CSS_Rule_Index * first = NULL;
  const GTEXT_CSS_Rule_Index * again = NULL;
  gtext_cssom_document_index(document, &first);
  gtext_cssom_document_index(document, &again);
  printf("index: %zu records, asked again: %s object\n", gtext_css_rule_index_record_count(first), again == first ? "the same" : "another");

  /* document.adoptedStyleSheets: a constructed sheet of this document, ordered after the owner sheets */
  GTEXT_CSSOM_Sheet * adopted = NULL;
  gtext_cssom_sheet_new_constructed(document, NULL, &adopted);
  gtext_cssom_sheet_replace_sync(adopted, "c { margin: 0 }", 15, NULL);
  gtext_cssom_scope_adopted_insert(scope, adopted, 0, NULL);
  GTEXT_CSS_Rule_Index * held = NULL;
  gtext_cssom_document_index_retained(document, &held);
  printf("after adopting: %zu records in %zu sheets (the old index is not the new one: %s)\n", gtext_css_rule_index_record_count(held), gtext_css_rule_index_sheet_count(held),
      held != first ? "yes" : "no");

  /* matchMedia, and the environment changing: the listener runs after the change, and the @media rule joins the index; no sheet is compiled again */
  GTEXT_CSSOM_Media_Query_List * wide_query = NULL;
  gtext_cssom_match_media(document, "(min-width: 1000px)", 19, &wide_query);
  gtext_cssom_media_query_list_add_listener(wide_query, on_change, NULL);
  GTEXT_CSS_Sheet * before = NULL;
  gtext_cssom_sheet_snapshot(owner, &before);
  GTEXT_CSS_Media_Environment wide_env = gtext_css_media_environment_default(NULL);
  wide_env.viewport.width = 1200;
  options.media = &wide_env;
  GTEXT_CSS_Conditions * wide = NULL;
  gtext_css_conditions_new(registry, &options, &wide);
  gtext_cssom_document_set_conditions(document, wide, NULL);
  const GTEXT_CSS_Rule_Index * after_index = NULL;
  gtext_cssom_document_index(document, &after_index);
  GTEXT_CSS_Sheet * after = NULL;
  gtext_cssom_sheet_snapshot(owner, &after);
  printf("after the resize: %zu records, the sheet is %s object\n", gtext_css_rule_index_record_count(after_index), after == before ? "the same" : "another");

  gtext_css_sheet_free(before);
  gtext_css_sheet_free(after);
  gtext_css_rule_index_free(held);
  gtext_cssom_media_query_list_release(wide_query);
  gtext_cssom_sheet_release(adopted);
  gtext_cssom_sheet_release(owner);
  gtext_cssom_document_release(document);
  gtext_css_conditions_free(wide);
  gtext_css_conditions_free(narrow);
  gtext_css_registry_free(registry);
}
```

prints

```
index: 1 records, asked again: the same object
after adopting: 2 records in 2 sheets (the old index is not the new one: yes)
change: matches yes
after the resize: 3 records, the sheet is the same object
```

## Observers, events and batches

`gtext_cssom_document_observe(document, fn, user, &handle)` registers an observer; `gtext_cssom_document_unobserve()` removes it by the handle (a number the document never reuses). At most `max_observers` (default 1,024) may be registered; one more is `GTEXT_CSS_E_LIMIT_CSSOM`.

**One event for each mutation that changed something**, delivered after the mutation is complete and consistent and before the call returns: the state an observer reads is the state after the mutation, and `event->generation` is the generation the mutation made. A mutation that changes nothing (a byte-identical `setProperty`, a value that is ignored, a rule that no sheet holds) sends nothing. The kinds are `SHEET_ADDED`, `SHEET_REMOVED`, `SHEET_DISABLED_CHANGED`, `SHEET_MEDIA_CHANGED`, `RULE_INSERTED`, `RULE_REMOVED`, `RULE_REPLACED` (a selector, a condition or a descriptor text changed, or a whole sheet was replaced), `DECLARATIONS_CHANGED`, `INLINE_STYLE_CHANGED`, `ADOPTED_CHANGED`, `IMPORT_COMPLETED` and `CONDITIONS_CHANGED`.

**What an event holds.** Ids and pointers (`sheet`, `rule`, `declaration`, `scope`, the `element` of an inline style) that are **borrowed**: the document holds them for the delivery, even if a later event of the same batch removed them for everybody else, so that a binding can queue a record for an object's wrapper; take your own reference to keep one. The `path` of the rule, the indexes from the sheet's `cssRules` down to it, **as of that event** (a removed rule: where it was), and `detached_since`, whether the rule is out of its sheet by the time of delivery. In a batch the graph may have moved on: ids and pointers are the reliable pair.

**The affected set** is data computed from what the mutation already touched, never a walk of the sheet. A style rule's event carries its selector list (`selectors`, and `selectors_before` for a replace) with the dependency summary of each (`GTEXT_CSS_DEP_*`, the state bits, the attribute names); `@media`, `@import` and `@supports` carry the condition summary of the media layer, and `@container` carries it with `environment` 0 and `container` holding the kinds of question its conditions ask of their query container (`GTEXT_CSS_CONTAINER_ASKS_SIZE`, `_STYLE`, `_SCROLL_STATE`, `_ANCHORED`, `_UNKNOWN`; a name alone asks none), because a container's size is not a fact about the environment; a declaration event carries the changed properties, ascending and without repeats, a shorthand contributing its longhands, the names of the changed custom properties, and whether `all` is among them. What the library does not list it says as *anything*: a rule that holds rules, a whole sheet, a finished import and a change of the sheet list have `selector_dependencies.kinds` equal to `GTEXT_CSSOM_DEP_ALL`. `summary_complete` is false when the summary could not be made (an allocation failed after the mutation was complete, which cannot be undone): treat everything as affected. The mutation itself never fails for it.

**Delivery** is synchronous and in registration order. An observer may read any object and may not mutate: every mutator, `begin_batch` and `end_batch` return `GTEXT_CSS_E_REENTRANT` from inside one. It may register and remove observers; one registered during a delivery is not called for the events already made, one removed is not called again. A status other than `GTEXT_CSS_OK` is `outcome->observer_status` (the first one), the mutation stays, and the other observers are called.

**Batches** are an editor's transaction: `gtext_cssom_document_begin_batch()` defers delivery to the end of the outermost `end_batch()`, in the order the mutations were made, while versions and the generation still move with each mutation. The place of an event in the queue is taken before the mutation changes anything, so a batch that is full (`max_queued_events`) or cannot grow refuses the mutation, with nothing changed. The queue is memory like the objects are: its array and the copies in its events (the path, the property ids, the selector lists) count against `max_live_bytes`, and a batch that has filled that budget refuses the next mutation in the same way (the event of the mutation that fills it is still kept; the check is before the next one). A batch with no observer queues nothing, and the queue's memory goes back when the outermost batch ends. The handle going away with a batch open drops what was queued.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static const char * kind_name(GTEXT_CSSOM_Event_Kind kind) {
  switch (kind) {
  case GTEXT_CSSOM_EVENT_RULE_INSERTED: return "rule inserted";
  case GTEXT_CSSOM_EVENT_RULE_REMOVED: return "rule removed";
  case GTEXT_CSSOM_EVENT_RULE_REPLACED: return "rule replaced";
  case GTEXT_CSSOM_EVENT_DECLARATIONS_CHANGED: return "declarations changed";
  default: return "other";
  }
}

/* What an invalidation set needs, read from the event: where the rule is, its selectors before and after, and which properties changed. */
static GTEXT_CSS_Status observe(void * user, const GTEXT_CSSOM_Event * event) {
  const GTEXT_Allocator * allocator = (const GTEXT_Allocator *)user;
  printf("%s", kind_name(event->kind));
  if (event->has_path) {
    printf(" at");
    for (size_t i = 0; i < event->path_length; i++) {
      printf(" %zu", event->path[i]);
    }
  }
  char * text = NULL;
  size_t length = 0;
  if (event->selectors_before && gtext_css_selector_serialize_buffer(event->selectors_before, allocator, &text, &length) == GTEXT_CSS_OK) {
    printf(", was %s", text);
    gtext_css_selector_buffer_free(allocator, text);
  }
  if (event->selectors && gtext_css_selector_serialize_buffer(event->selectors, allocator, &text, &length) == GTEXT_CSS_OK) {
    printf(", selectors %s (class %s, attribute %s)", text, (event->selector_dependencies.kinds & GTEXT_CSS_DEP_CLASS) ? "yes" : "no",
        (event->selector_dependencies.kinds & GTEXT_CSS_DEP_ATTR) ? "yes" : "no");
    gtext_css_selector_buffer_free(allocator, text);
  }
  if (event->property_count) {
    printf(", %zu properties", event->property_count);
  }
  printf(", detached %s\n", event->detached_since ? "yes" : "no");
  return GTEXT_CSS_OK;
}

/* Observers: one event for each mutation that changed something, after the state is consistent; a batch defers them to its end. */
static void cssom_events(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);
  const GTEXT_Allocator * allocator = gtext_cssom_document_allocator(document);

  const char * text = "a { color: red } @media screen { b { width: 1px } }";
  GTEXT_CSSOM_Sheet * sheet = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &sheet);
  GTEXT_CSSOM_Observer_Handle handle = 0;
  gtext_cssom_document_observe(document, observe, (void *)allocator, &handle);

  gtext_cssom_sheet_insert_rule(sheet, "ul > li.on[title] { margin: 0 }", 31, 0, NULL, NULL);
  const GTEXT_CSSOM_Rule_List * rules = NULL;
  gtext_cssom_sheet_rules(sheet, &rules);
  GTEXT_CSSOM_Rule * first = gtext_cssom_rule_list_at(rules, 0);
  gtext_cssom_rule_set_selector_text(first, "p, q", 4, NULL);

  /* a batch: nothing is delivered until the outermost one ends, in the order the mutations were made */
  gtext_cssom_document_begin_batch(document);
  gtext_cssom_sheet_delete_rule(sheet, 0, NULL);
  GTEXT_CSSOM_Decl * style = NULL;
  GTEXT_CSSOM_Rule * second = gtext_cssom_rule_list_at(rules, 0);
  gtext_cssom_rule_style(second, &style);
  gtext_cssom_decl_set_property(style, "color", 5, "blue", 4, "", 0, NULL);
  printf("in the batch: nothing yet\n");
  gtext_cssom_document_end_batch(document, NULL);

  gtext_cssom_decl_release(style);
  gtext_cssom_rule_release(second);
  gtext_cssom_rule_release(first);
  gtext_cssom_document_unobserve(document, handle);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
```

prints

```
rule inserted at 0, selectors ul > li.on[title] (class yes, attribute yes), detached no
rule replaced at 0, was ul > li.on[title], selectors p, q (class no, attribute no), detached no
in the batch: nothing yet
rule removed at 0, selectors p, q (class no, attribute no), detached yes
declarations changed at 0, selectors a (class no, attribute no), 1 properties, detached no
```

## The Markup binding

`css_cssom_markup.h` connects a Markup document to a tree scope. `gtext_cssom_markup_sync()` makes the scope's `styleSheets` say what the document's elements say, with the HTML Standard's rules: an HTML or SVG `style` element that is connected, whose `type` is absent, empty or, whole, an ASCII case-insensitive `text/css` (`text/css; charset=utf-8` and ` text/css` are not: the browsers do not trim or parse it), has a sheet made from its child text content, with its `media`, its `title` and the element as `owner_node`; an HTML `link` with `rel` `stylesheet` and an `href` is asked of the **link callback**, which gives the text (the library never fetches), says the link is pending (`gtext_cssom_markup_link_complete()` gives it later) or says it failed.

**What a change does.** A change of a `<style>` element's text removes the sheet and makes a new one, with a new `sheet_id`: the rules the object model had added are gone and the element's text is not changed by them (`insertRule` on a `<style>` sheet changes only the sheet, and the next sync keeps it, because the text did not change). An element that moved is a new sheet and the others are kept, snapshots and all. An element that left the tree takes its sheet out of `styleSheets` and the index (`SHEET_REMOVED`), and **the sheet stays a valid, mutable object** with `owner_node` NULL: `insertRule` on it works and affects nothing. `media` and `title` are changed in place. Both browsers agree on every one of these but the last two and on text set to the value it already had; where they differ the library is on the side of Chromium (the same sheet), and Firefox makes a new one.

**`sync` is cheap when nothing changed.** The Markup document's generation counts every change of the tree. A sync that finds it unchanged since the last one returns with no walk; otherwise it walks the tree once, compares each style element's text with the text its sheet was made from (length, then bytes) and keeps what is unchanged. A caller that is told which element changed calls `gtext_cssom_markup_style_element_changed()` and pays for that element. A sync is not atomic as a whole, but every step is: one that fails stops with the document consistent and the next call goes on. The events of one sync are one batch.

```c
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* A Markup document's <style> elements are the owner sheets of the scope: one sync makes the list say what the elements say, and an unchanged tree costs no walk. */
static void cssom_markup(void) {
  const char * html = "<!DOCTYPE html><html><head><style>p { color: red }</style><style type=\"text/plain\">q { }</style></head><body><style>div { width: 1px }</style></body></html>";
  GTEXT_Markup_Document * markup = NULL;
  GTEXT_HTML_Result result;
  memset(&result, 0, sizeof result);
  gtext_html_parse(html, strlen(html), NULL, &markup, &result);
  gtext_html_result_free(&result);

  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  config.tree_version = gtext_cssom_markup_tree_version;   /* the Markup document's generation: an edit of an element's style attribute is seen */
  config.tree_version_user = markup;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(document);

  GTEXT_CSSOM_Markup_Binding * binding = NULL;
  gtext_cssom_markup_binding_new(document, scope, markup, NULL, &binding);
  gtext_cssom_markup_sync(binding, NULL);
  printf("sheets: %zu (the style with type text/plain has none)\n", gtext_cssom_scope_sheet_count(scope));

  GTEXT_CSSOM_Markup_Counters before, after;
  gtext_cssom_markup_binding_counters(binding, &before);
  gtext_cssom_markup_sync(binding, NULL);
  gtext_cssom_markup_binding_counters(binding, &after);
  printf("sync of an unchanged tree: walks %llu\n", (unsigned long long)(after.walks - before.walks));

  /* a change of an element's text is a new sheet; the other is kept */
  GTEXT_Markup_Node * root = gtext_markup_document_root(markup);
  GTEXT_Markup_Node * node = gtext_markup_node_next_in_order(root, root, false);
  while (node && !gtext_markup_element_is(node, GTEXT_MARKUP_NS_HTML, "style")) {
    node = gtext_markup_node_next_in_order(root, node, false);
  }
  GTEXT_CSSOM_Sheet * old_sheet = gtext_cssom_markup_sheet_of(binding, node);
  GTEXT_Markup_Node * old_text = gtext_markup_node_first_child(node);
  gtext_markup_node_set_data(old_text, "p { color: blue }", 17);
  GTEXT_CSSOM_Outcome outcome;
  gtext_cssom_markup_sync(binding, &outcome);
  GTEXT_CSSOM_Sheet * new_sheet = gtext_cssom_markup_sheet_of(binding, node);
  printf("after the text change: %s sheet, the old sheet has %s owner, outcome %s\n", new_sheet != old_sheet ? "a new" : "the same", gtext_cssom_sheet_owner_node(old_sheet) ? "an" : "no",
      outcome.kind == GTEXT_CSSOM_CHANGED ? "changed" : "unchanged");

  gtext_cssom_sheet_release(new_sheet);
  gtext_cssom_sheet_release(old_sheet);
  gtext_cssom_markup_binding_free(binding);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
  gtext_markup_document_free(markup);
}
```

prints

```
sheets: 2 (the style with type text/plain has none)
sync of an unchanged tree: walks 0
after the text change: a new sheet, the old sheet has no owner, outcome changed
```

`gtext_cssom_markup_inline_binding()` is the attribute binding of the previous section for a Markup element, and `gtext_cssom_markup_tree_version()` the config's `tree_version` hook for a Markup document, so that a declaration notices a `setAttribute('style', ...)` made through the Markup API on its next call.

## `getComputedStyle()` and resolved values

`css_cssom_computed.h`: `gtext_cssom_computed_style(document, element, pseudo, pseudo_length, &decl)` is `getComputedStyle(element, pseudo)`. It returns a new `CSSStyleDeclaration` each call (owner `COMPUTED`) that is **read-only** (every mutator is `NoModificationAllowedError`, the `cssText` getter is the empty string, `getPropertyPriority` is always empty) and **live**: each call checks the document's generation and the caller's tree version against those it last read for, and reads again when either moved, so a read after `insertRule`, `setProperty` or a change of the tree (a class, a `style` attribute, a move) sees the new cascade. It is a pure function of the document and the tree: two reads at one state are byte-identical.

**When it is empty.** As the served text says (section 7.2, step 5), the list is empty unless the element is connected, part of the flat tree and rendered. The three facts are the bits of `element_state` in the config (absent: all true, and for a Markup document a loose element, whose top is not the document element, is not connected) and the layout interface's `is_rendered`. A `display: none` element is connected and in the tree, so it has its values. A pseudo-element argument is a string that starts with a colon and parses as one `<pseudo-element-selector>`; any other string is read as no pseudo-element (`'before'` is the element, as Firefox and the served text have it, and Chromium has it as `::before`), and `::slotted()`, `::part()`, an unknown pseudo-element, `:hover` and a string with white space at the end make the declaration empty, as both browsers answer.

**Which properties a pseudo-element has.** A declaration of a property that does not apply to `::first-line`, `::first-letter` or `::marker` takes no part in that pseudo-element's cascade (the rule index leaves it out when it is asked for the pseudo-element), so its computed value is the inherited or the initial one, as if nothing had been declared. CSS Pseudo-Elements 4 lists what applies to `::first-line` (the font, color, opacity, background, typesetting, text decoration, `ruby-position` and inline layout properties; not `writing-mode`, `direction` or `text-orientation`) and to `::first-letter` (the same, with margin, padding, border and `box-shadow`); CSS Lists 3 lists what applies to a marker box (`text-combine-upright`, `unicode-bidi`, `direction`, `content`, the animation and transition properties, and the inheritable properties that apply to text). The text lets a user agent apply others; the library denies a property it does not list where Chromium and Firefox both leave it unapplied (display, the box and positioning properties, transforms, the flex, grid, multi-column, table and list properties and so on), and keeps applying a listed one that both leave out (the typesetting properties such as `word-break`, `white-space` and `hyphens`, and `text-emphasis`). The table is `src/css/css_pseudo_apply.c`, held to the recorded answers of the two browsers by `tools/css/check-pseudo-apply.py`. `text-shadow` has the four lengths of CSS Text Decoration 4's computed value (the spread is `0px`), where both browsers write three. Every other pseudo-element has every property.

**What it lists.** Every supported longhand in lexicographic order with the vendor-prefixed ones (names that begin with a hyphen) after the others, as both browsers list them, then the element's custom properties that are not the guaranteed-invalid value, in the byte order of their names (the served text says the order is undefined; Chromium and Firefox differ). A shorthand is written from the resolved longhands, so its value is the empty string when a longhand it needs is not supported.

**Resolved values.** CSSOM's section 9: the computed value, except where the property's category says otherwise. `tools/css/resolved_value.tsv` is the data (each row quotes the sentence of the served text that says it), `gen-cssom-tables.py` checks it against the registry and writes it. Every `<color>` in any value is its used value ("The resolved value of a `<color>` is its used value", Color 4, 15): a keyword is its sRGB color (`rgb(255, 0, 0)`, `transparent` is `rgba(0, 0, 0, 0)`), `currentcolor` is the element's color (and a `color-mix()` or relative color that held it is computed with it), and the used value of `auto` in `caret-color` and `outline-color` is the current color, which both browsers answer. A system color stays as it is unless the environment of the compute context has a `resolve_color` callback. A used value that is a fact about a laid out box (`width`, `height`, the margins, the paddings, `transform-origin`, `perspective-origin`, `grid-template-columns` and `-rows`, `line-height` unless it is `normal`, the insets of a positioned box, `transform`) is the **layout interface's**: its `used_value` gives a typed value or says "does not apply", and then, as when the box is `display: none` or `contents` or the inset is not positioned, the computed value stands. With no `used_value` callback the computed value stands in and `gtext_cssom_computed_origin()` says `ABSENT_LAYOUT` for it, `USED` for what the layout or a rule of the library made, `COMPUTED` otherwise; a layout that fails fails the read, naming itself in `gtext_cssom_computed_failure()`. Two rules need no layout: an `auto` `animation-duration` is `0s` when `animation-timeline` is `auto`, and an `auto` minimum size is zero except for the items of a flex or grid container (and for a box whose `aspect-ratio` is not `auto`).

**Conditions and containers in a document's styles.** A compute context the document makes for itself answers `if(media())` and `if(supports())` from the conditions object the document evaluates with, as it is at the time of the question (an environment change that replaces the object is followed); `media()` takes a feature without the parentheses a media query writes around one (`media(height <= 600px)`). A query container is read from the styles the document has already computed: `gtext_cssom_document_container_interface()` fills the part of a `GTEXT_CSS_Container_Interface` that a document can answer (`computed_style` and the compute context), the caller adds the box, scroll state and anchored fallback of its layout, builds the conditions object with it and gives that object to the document with `gtext_cssom_document_set_conditions()`. Without it a `@container` style query is `GTEXT_CSS_E_UNSUPPORTED` naming `container.computed_style`. The computed `background-position` of `getComputedStyle()` is the pair of offsets from the top left that Backgrounds 4, 2.7 describes (`5px 9px`), while `background-position-x` and `-y` keep their origin keyword.

**`@property` rules in a document's styles.** The `@property` rules of the sheets that take part in the document (not disabled, media true) register custom properties in the compute context the document makes for itself, so `getComputedStyle()` reads a registered property as its computed value (`calc(1px + 2px)` as `3px`, `1em` at `font-size: 10px` as `10px`, an unset non-inherited property as its initial value) and `if(style(--x: 3px))` compares computed values. The rules are read in the order the cascade puts them, as Cascade 5 orders at-rules that cascade by name: "the last one in stylesheet order wins" among rules in one layer, a later layer over an earlier one (`@layer a, b;` orders layers across sheets), an unlayered rule over any layered one, and a layer's own rules over those of its sub-layers; the blocks of a true `@media`, `@supports` and `@container`, of `@scope` and `@starting-style`, and of `@layer` are entered (Chromium and Firefox register the rules of each). A rule that is not valid by Properties and Values API 1 (a prelude that is not one, or an initial value that is of the syntax and needs the element: every descriptor is optional, a prelude may list names, and `@property --x <length>: 1px;` is a rule too, written in the block form) is not in the sheet at all (the parse drops it, as the browsers do), so the diagnostics of `gtext_cssom_document_property_rules()` report only valid rules that another replaced or that a registration of the caller's registry overrode ("A custom property registration from CSS.registerProperty() further wins over any @property rules"). The set is rebuilt with the rule index, so only a change of the sheets, of what a conditional rule holds, or of the document's structure reads the rules again; the compute context is made again only when the registrations or diagnostics differ (`property_context_rebuilds`), a document whose sheets hold no `@property`, `@layer` or group rule reads nothing, and the styles cached and the computed declarations read before the change read again. A compute context the caller supplies keeps its own registrations: `gtext_cssom_document_property_rules()` reports the document's rules with `applied` false and `property_unapplied` counts them. Not read: the rules of a sheet an `@import` loaded, and of the sheets registered for the user-agent and user origins (compiled sheets, which keep no `@property` rule). A compute context handed to a conditions object (`gtext_cssom_document_container_interface()`) stays valid, with the registrations it was made with, until the document goes; ask for the interface again after the rules change to see the new ones.

**Animations in a document's styles.** The `@keyframes` rules of the document's sheets (the sheets that take part, in the order the cascade puts them, the last in the highest layer winning by name; the rules of a shadow tree are not read) are what `getComputedStyle()` samples an element's `animation-*` from. The library reads no clock: the time of an animation is the layout's fact, like a laid out box, and it is given through `GTEXT_CSSOM_Layout_Interface::animation_time` (the local time of the animation's effect, in milliseconds, for one effect of one element or pseudo-element; `*known` false when the caller has none) and keyed through `::timeline_version` (a number the caller advances whenever a time it gives could have changed, as `tree_version` is advanced for the tree; a style is kept for one version only, and without a version a document with `@keyframes` rules keeps no style across reads). An animation given a time has its keyframes' properties sampled (css_animation.h), in the animation origin of the cascade, so the value that `getComputedStyle()` reads is the value of the animation at that time, and what depends on it follows. An animation that is given none is not sampled: the computed value is the underlying one, and `gtext_cssom_computed_origin()` says `GTEXT_CSSOM_ORIGIN_ABSENT_TIMELINE` for each property, custom properties included (a shorthand is as strong as its longhands, `USED` before `ABSENT_LAYOUT` or `ABSENT_TIMELINE`). A failure of `animation_time` fails the read and `gtext_cssom_computed_failure()` names it. Nothing runs: no animation starts and no event is queued.

**Transitions in a document's styles.** A transition needs the style before the change, so the library does not start one on a read. The layout interface's `transitions` (the running transitions of an element or pseudo-element, a `GTEXT_CSS_Transition_Set`, and the time to take their current values at) and `transition_version` (the key of the cached styles, as `timeline_version` is) give `getComputedStyle()` the sets; each running transition has its current value declared at the transition origin, above every other, and what inherits it follows (\ref format_css_transitions "css-transitions.md"). The sets are the caller's, kept in a `GTEXT_CSSOM_Transition_Tracker` or in a structure of its own: `gtext_cssom_transition_tracker_flush()` is a style change event for an element and its ancestors, which computes their *after-change styles* (`gtext_cssom_document_after_change_style()`: the style without transitions) and updates their sets against the style of the previous event. A layout that gives `transitions` and no `transition_version` keeps no style across reads.

**Cost.** The style of an element is computed from its inheritance parent's, and the document keeps the styles it computed in a cache for the state it computed them in (bounded by `max_cached_styles`, the entry cached first going first, so that the answer never depends on it), so n reads over a chain of depth d cost d + n computes, not d x n (`computed_computes` of the counters). A document with neither a `tree_version` hook nor a notification (`gtext_cssom_document_tree_changed()` with `computed_trust_tree_changed`) cannot know the tree is unchanged and computes each read again. `gtext_cssom_document_element_released()` drops that element's cache entry and freezes its declarations (a reused address never answers for the old element; called from a callback of a read it takes effect when the outermost read ends, and if that needs memory it could not get, every computed declaration of the document is frozen then); `gtext_cssom_document_set_compute_context()` replaces the context the styles are made with. `:visited` never matches in this library (the matcher answers as the privacy-preserving reading), so the privacy rule of `getComputedStyle()` has nothing to hide. A callback of the caller's tree or layout cannot mutate the document: the attempt is `GTEXT_CSS_E_REENTRANT`.

```c
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* The layout of this page is a table of one entry: the used width of the element is 49.5px. A real consumer answers for the box it laid out. */
static GTEXT_CSS_Status used_width(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, GTEXT_CSS_Property_Id property, bool * applies,
    GTEXT_CSS_Value_Set ** out) {
  (void)element;
  (void)pseudo;
  GTEXT_CSS_Property_Value value;
  memset(&value, 0, sizeof value);
  *applies = false;
  GTEXT_CSS_Property_Info info;
  GTEXT_CSS_Registry * registry = (GTEXT_CSS_Registry *)user;
  if (!gtext_css_registry_property_info(registry, property, &info) || strcmp(info.name, "width") != 0) {
    return GTEXT_CSS_OK;   /* does not apply: the resolved value is the computed value */
  }
  if (gtext_css_parse_property_text(registry, "width", 5, "49.5px", 6, NULL, &value) != GTEXT_CSS_OK || !value.set) {
    return GTEXT_CSS_E_INTERNAL;
  }
  *out = value.set;   /* the document takes the set */
  *applies = true;
  return GTEXT_CSS_OK;
}

static void show(GTEXT_CSSOM_Decl * style, const char * property) {
  char * value = NULL;
  size_t length = 0;
  gtext_cssom_decl_get_property_value(style, property, strlen(property), &value, &length);
  GTEXT_CSSOM_Origin origin = GTEXT_CSSOM_ORIGIN_COMPUTED;
  bool found = false;
  gtext_cssom_computed_origin(style, property, strlen(property), &origin, &found);
  printf("%s: %s (%s)\n", property, value, origin == GTEXT_CSSOM_ORIGIN_USED ? "used" : origin == GTEXT_CSSOM_ORIGIN_ABSENT_LAYOUT ? "no layout" : "computed");
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(gtext_cssom_decl_document(style)), value);
}

/* getComputedStyle(): a live, read-only declaration of resolved values. A color is its used value (a keyword is its sRGB color, `currentcolor` the element's color), a size is
   the used value when the layout gives one and the computed value when it does not, and the declaration reads again when the document or the tree moved. */
static void cssom_computed(void) {
  const char * html = "<!DOCTYPE html><html><head><style>p { color: red; width: 50px; border-top-style: solid } .b { color: blue }</style></head><body><p id=\"a\">x</p></body></html>";
  GTEXT_Markup_Document * markup = NULL;
  GTEXT_HTML_Result result;
  memset(&result, 0, sizeof result);
  gtext_html_parse(html, strlen(html), NULL, &markup, &result);
  gtext_html_result_free(&result);
  GTEXT_CSS_Markup_Adapter * adapter = NULL;
  gtext_css_markup_adapter_new(markup, NULL, &adapter);
  GTEXT_CSS_Selector_Match_Options match = gtext_css_markup_match_options(adapter);

  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Layout_Interface layout;
  memset(&layout, 0, sizeof layout);
  layout.size = sizeof layout;
  layout.user = registry;
  layout.used_value = used_width;
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  config.element_interface = gtext_css_markup_interface(adapter);
  config.match_options = &match;
  config.tree_version = gtext_cssom_markup_tree_version;
  config.tree_version_user = markup;
  config.layout = &layout;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);
  GTEXT_CSSOM_Markup_Binding * binding = NULL;
  gtext_cssom_markup_binding_new(document, gtext_cssom_document_scope(document), markup, NULL, &binding);
  gtext_cssom_markup_sync(binding, NULL);

  GTEXT_Markup_Node * root = gtext_markup_document_root(markup);
  GTEXT_Markup_Node * p = gtext_markup_node_next_in_order(root, root, false);
  while (p && !gtext_markup_element_is(p, GTEXT_MARKUP_NS_HTML, "p")) {
    p = gtext_markup_node_next_in_order(root, p, false);
  }
  GTEXT_CSSOM_Decl * style = NULL;
  gtext_cssom_computed_style(document, gtext_css_markup_handle(p), NULL, 0, &style);
  show(style, "color");
  show(style, "border-top-color");
  show(style, "width");
  show(style, "margin-top");

  /* the same object, after the tree changed */
  gtext_markup_element_set_attribute(p, NULL, NULL, "class", "b", 1);
  show(style, "color");

  char * text = NULL;
  size_t length = 0;
  gtext_cssom_decl_css_text(style, &text, &length);
  printf("cssText: \"%s\"\n", text);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), text);
  GTEXT_CSSOM_Outcome outcome;
  printf("setProperty: %s\n", gtext_cssom_exception_name(gtext_cssom_decl_set_property(style, "color", 5, "green", 5, "", 0, &outcome)));

  gtext_cssom_decl_release(style);
  gtext_cssom_markup_binding_free(binding);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
  gtext_css_markup_adapter_free(adapter);
  gtext_markup_document_free(markup);
}
```

prints

```
color: rgb(255, 0, 0) (used)
border-top-color: rgb(255, 0, 0) (used)
width: 49.5px (used)
margin-top: 0px (computed)
color: rgb(0, 0, 255) (used)
cssText: ""
setProperty: NoModificationAllowedError
```

## Utilities

`gtext_cssom_escape()` is `CSS.escape()`: CSSOM's "serialize an identifier" of well-formed UTF-8 (or WTF-8, where a lone surrogate is one code point). The empty text is the empty text, a NUL is U+FFFD, and the result always reads back as the text. A character from U+0080 up is written as it is only where it is a non-ASCII ident code point of CSS Syntax; otherwise it is escaped, which is what the served text says and what the browsers do not do for every such character.

## Determinism

Every operation is a function of the sequence of calls, the registry and the callbacks' answers: no clock, no locale, no hardware, no address-derived value (ids, orders, dumps), no global state and no allocation outside the supplied allocator. `gtext_cssom_decl_dump()` writes a declaration in a canonical, versioned text (`cssom-decl 1`), the same bytes on every host, and `gtext_cssom_decl_hash()` its SHA-256.
