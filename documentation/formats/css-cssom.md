@page format_css_cssom The CSS Object Model

# CSS Object Model

The CSSOM here is the **data model and the algorithms** of the CSS Object Model specification as a native, reference counted, mutable object graph, for a program with no script engine: a browser engine's binding layer, an editor, an inspector. It is not a JavaScript engine, Web IDL, wrapper objects, events or promises; those are a binding's, and the API is shaped for one (object identity is stable, every object has a user-data slot with a finalizer, the specification's exceptions are status codes, and nothing a call returns is a pointer that a later mutation can invalidate).

This page describes what is built: the document and its objects, their lifetime, the style sheet and the rule objects and what a parse makes of a text, the declaration object (`CSSStyleDeclaration`) of a rule, of an element's `style` attribute and free-standing, the IDL attribute names, `CSS.escape()`, and the statuses of the web platform's exceptions. The declaration block underneath is in \ref format_css_computed "css-computed.md"; the immutable sheets and indexes that the style computation reads are in \ref format_css_sheets "css-sheets.md".

Back to \ref "Formats". Include `<ghoti.io/text/css.h>` (`css/css_cssom.h`, `css/css_cssom_core.h`, `css/css_cssom_decl.h`, `css/css_cssom_rule.h`, `css/css_cssom_sheet.h`, `css/css_cssom_util.h`).

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

`gtext_cssom_exception_name(status)` returns the name for those eight and NULL for every other status, which is all a binding needs to throw. `GTEXT_CSS_E_INVALID` is a misuse of the API (a NULL or unusable argument) and is never a content error; `GTEXT_CSS_E_REENTRANT` (a mutation from inside a callback) and `GTEXT_CSS_E_LIMIT_CSSOM` have no exception name. Every mutator has the **strong guarantee**: on any failure, `GTEXT_CSS_E_OOM` included, the object, its versions and the document's counters are as they were. A mutation that leaves the affected declaration byte-identical is `UNCHANGED`: no version, no generation and no notification.

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

A property name that is not a custom property is ASCII lowercased and an alias is its property. `getPropertyValue` of a longhand is its last declaration in its shortest specified form, of a shorthand its longhands written as one value (the empty string when one is missing, when the importance differs or when they are not a value of the shorthand's grammar), of a shorthand all of whose longhands are pending values of one declaration (`margin: var(--x)`) the value as it was written, of a custom property its trimmed token text (the name is case-sensitive). `setProperty` follows the served steps in order: a read-only declaration is `GTEXT_CSS_E_NO_MODIFICATION`; a property that is not supported returns with nothing changed **before the value is looked at** (so `setProperty('bogus', '')` removes nothing and reports `IGNORED_UNKNOWN_PROPERTY`); an empty value is `removeProperty`; a priority that is neither empty nor an ASCII case-insensitive `important` is ignored; an invalid value, and a value that contains `!important`, is ignored. A shorthand expands: a longhand already in the block is replaced **in place**, a new one is appended in the shorthand's order. The `cssText` setter replaces the whole list; a declaration that does not parse is dropped, nested rules are dropped, of two declarations of one property the later replaces the earlier, and an important declaration is not replaced by a later one that is not. A property the registry has but the library cannot judge (`voice-family`), and a shorthand it cannot split into longhands (`font: caption`, a system font), is reported by a `setProperty` as `GTEXT_CSS_E_UNSUPPORTED` naming the property, and **kept as written** by a parse (a `cssText` set, an element's `style` attribute), with a count (`unjudgeable_preserved`): the cascade drops such a declaration for computing, but a declaration block keeps it, so editing another property of an element does not take it out of the `style` attribute. It is an opaque item of the block: in its place in the order, reported by `length` and `item()`, its value read back as it was written, its importance kept, written back verbatim, removed by `removeProperty` like any declaration, and replaced only by a parse of the same property. An opaque shorthand is atomic, since its longhands are not known: a later declaration of one of its longhands that the block holds before it is placed after it (an in-place replacement would be overridden by the shorthand), a declaration of the whole shorthand replaces it, and removing one longhand does not change it.

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

A rule is one object, `GTEXT_CSSOM_Rule`, with a **kind** (the at-rule kinds of the at-rule model, `UNKNOWN` included, and the style rule and the nested-declarations rule: 29 in all), the legacy `type` number CSSOM lists for the rules it lists (0 for every newer one), and the CSSOM interface name it stands for (`CSSLayerBlockRule` and `CSSLayerStatementRule` are one kind, told apart by `has_block`). The kinds table is data (`tools/css/cssom-supplement.tsv`, checked against the ledger of at-rules by `make check-cssom`). A kind no browser exposes (`@when`, `@else`, `@custom-media`, the preserved ones) is a generic rule: in `cssRules`, inactive.

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

**A rule whose syntax is immutable is replaced as a whole.** `gtext_cssom_rule_set_selector_text()` (`selectorText` of a style rule and of `@page`), `_set_key_text()` (`keyText`) and `_set_attribute()` (the writable attributes: a `@counter-style`'s name and descriptors, a `@keyframes` name, the family list of `@font-feature-values`) write the candidate text of the rule's own part, read it again in the place the rule stands in (a nested rule's selector is relative, a prefix must have been declared), and swap the rule's syntax for the result only if it is a rule of the same kind that holds what was set. The rule's identity, its place, the rules inside it and its ids do not change; its source becomes `MODIFIED`; the version of the rule and of its sheet moves once and one `RULE_REPLACED` event is sent. The cost is the size of that one rule, and a call that fails (an allocation included) changes nothing, ids included. A selector that does not parse is **ignored** (CSSOM: "do nothing"; the outcome says `IGNORED_INVALID_SELECTOR`). A key list that does not parse is a `SyntaxError` (`GTEXT_CSS_E_SYNTAX`; CSS Animations 1 says so, the pinned web-platform-tests page and both browsers throw). A value a descriptor refuses is ignored (`IGNORED_INVALID_VALUE`), the empty string removes a descriptor, and a readonly attribute is ignored as WebIDL says (`IGNORED_NOT_ALLOWED_HERE`): `conditionText` is readonly (CSS Conditional 3, and the pinned page asserts assigning to it does nothing). `gtext_cssom_rule_get_attribute()` reads the rest of the attributes CSSOM names by their IDL name (`href`, `layerName`, `supportsText`, `namespaceURI`, `prefix`, `start`, `end`, `containerName`, `containerQuery`, `syntax`, `inherits`, `initialValue`, `navigation`, `types`, `fontFamily`, `basePalette`, `overrideColors`, the descriptors of `@counter-style`, `name`); `*is_null` is true where the IDL says nullable and the attribute is absent.

**`style` of a keyframe, a page or margin rule, `@font-face`, `@position-try`, `@view-transition`, `@counter-style`, `@font-palette-values` and `@color-profile`** is a block that is a view of the rule: the declarations that are properties are in a declaration block (so `margin: auto` is four declarations, written as the shorthand), the descriptors that are not properties (`src`, `size`, `symbols`) follow them. Every edit is made on a copy and kept only if the rule reads again, from the text the copy writes, with what was set: `animation-name` in a keyframe, a `src` that is not a source list, a `!important` descriptor, and a property a page does not take are refused with nothing changed. An `@page` takes the descriptors of the pin and the page properties of Paged Media 3, Appendix A ("CSS 2.1 Properties that apply within the page context": margin, border, padding, background, color, font, text, width, height and the rest of the list), and a custom property; `gtext_cssom_decl_has_attribute()` says which IDL attributes such a block has (a page has `marginTop` and `size` and no `cssFloat`). The rule keeps the order of the text it was last written from; the block lists properties before descriptors.

**Media lists.** `gtext_cssom_sheet_media()` and `gtext_cssom_rule_media()` (an `@import` or an `@media`) return the list, the same object every time, which is a view of its owner and keeps it reachable. `mediaText` is the owner's media query list serialized; setting it **always parses** (a query that is not valid is `not all`, Media Queries 4, 3.2; the empty text is the empty list, which matches everything); `appendMedium()` ignores a text that is not one valid query and does not add a medium whose serialization is already there; `deleteMedium()` of one that is not there is a `NotFoundError`. The media of an `@import` are the queries after its URL, `layer()` and `supports()`.

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

  /* media.mediaText, appendMedium(), deleteMedium(): a text always parses, a medium that is not valid is ignored, a medium that is not there is a NotFoundError */
  GTEXT_CSSOM_Rule * media = gtext_cssom_rule_list_at(rules, 1);
  GTEXT_CSSOM_Media_List * list = NULL;
  gtext_cssom_rule_media(media, &list);
  gtext_cssom_media_set_text(list, "print, (bogus: 1) and", 21, NULL);
  gtext_cssom_media_append(list, "tv", 2, NULL);
  status = gtext_cssom_media_delete(list, "handheld", 8, NULL);
  printf("deleteMedium: %s\n", gtext_cssom_exception_name(status));
  status = gtext_cssom_media_text(list, &css, &length);
  show("mediaText", status, css, document);

  /* a counter style's descriptors are attributes; the empty string removes one */
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

## Utilities

`gtext_cssom_escape()` is `CSS.escape()`: CSSOM's "serialize an identifier" of well-formed UTF-8 (or WTF-8, where a lone surrogate is one code point). The empty text is the empty text, a NUL is U+FFFD, and the result always reads back as the text. A character from U+0080 up is written as it is only where it is a non-ASCII ident code point of CSS Syntax; otherwise it is escaped, which is what the served text says and what the browsers do not do for every such character.

## Determinism

Every operation is a function of the sequence of calls, the registry and the callbacks' answers: no clock, no locale, no hardware, no address-derived value (ids, orders, dumps), no global state and no allocation outside the supplied allocator. `gtext_cssom_decl_dump()` writes a declaration in a canonical, versioned text (`cssom-decl 1`), the same bytes on every host, and `gtext_cssom_decl_hash()` its SHA-256.
