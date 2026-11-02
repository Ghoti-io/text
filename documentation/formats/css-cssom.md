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

`gtext_cssom_object_set_user(object, pointer, finalizer)` stores the binding's wrapper. Replacing the pair runs the old finalizer first, clearing it runs it, and the finalizer also runs once when the object is freed, called with the pointer and the object's kind. **A finalizer must not call any `gtext_cssom_*` function on the dying object.** It may release and `set_user` other objects and free its own memory, and a mutation attempted from it, or from a notification callback, is refused with `GTEXT_CSS_E_REENTRANT`. `retain`, `release`, `set_user` and every finalizer run on the document's writer thread; a binding whose collector finalizes elsewhere queues the release to the writer.

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

## Utilities

`gtext_cssom_escape()` is `CSS.escape()`: CSSOM's "serialize an identifier" of well-formed UTF-8 (or WTF-8, where a lone surrogate is one code point). The empty text is the empty text, a NUL is U+FFFD, and the result always reads back as the text. A character from U+0080 up is written as it is only where it is a non-ASCII ident code point of CSS Syntax; otherwise it is escaped, which is what the served text says and what the browsers do not do for every such character.

## Determinism

Every operation is a function of the sequence of calls, the registry and the callbacks' answers: no clock, no locale, no hardware, no address-derived value (ids, orders, dumps), no global state and no allocation outside the supplied allocator. `gtext_cssom_decl_dump()` writes a declaration in a canonical, versioned text (`cssom-decl 1`), the same bytes on every host, and `gtext_cssom_decl_hash()` its SHA-256.
