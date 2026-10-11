@page format_markup Markup tree

# Markup tree

The document model that XML, XHTML and HTML share: a tree of documents,
fragments, doctypes, elements, text, CDATA sections, comments and processing
instructions, with namespace-aware names and attributes, built and changed
through an API that refuses what would break it and leaves the tree as it was
when it does. **This is the tree and the lexical rules for writing it.** The
parsers that fill it and the writers that read it are separate pages (see
`notes/text/XML-HTML-PLAN.md`); nothing on this page reads or writes a markup
language - \ref format_xml "XML" is the first parser that fills the tree and the
first writer that reads it - and the sections below say what has and has not been
checked.
Back to \ref text_format_references "Formats".

## Normative references

- **The tree's rules:** the [DOM Standard](https://dom.spec.whatwg.org/),
  section 4.2.4 "Mutation algorithms" - *ensure pre-insert validity* - for what
  may be a child of what. Not the whole DOM: there are no live collections, no
  events, no shadow trees, and attributes are not nodes.
- **Names and characters:** [XML 1.0 (Fifth Edition)](https://www.w3.org/TR/xml/),
  productions [2] Char, [4] and [4a] name characters, [15] Comment, [16] PI,
  [18] CDSect, and sections 2.11 and 3.3.3 on line-end and attribute-value
  normalization (which is why a carriage return, and a tab or line feed in an
  attribute value, are written as references).
- **Prefixes and local names:** [Namespaces in XML 1.0 (Third Edition)](https://www.w3.org/TR/xml-names/),
  production [4] NCName.
- **Document mode:** the [HTML Standard](https://html.spec.whatwg.org/),
  "Document mode", carried on the document and not decided here.

## Ownership

A document owns every node made in it, **including a node that is not in the
tree**. A created node is detached and belongs to the document until it is
inserted, so a caller that creates three nodes and fails on the fourth leaks
nothing, and `gtext_markup_document_free()` releases whatever is left. A node
never belongs to two documents: inserting one from another document is
`GTEXT_MARKUP_E_WRONG_DOCUMENT`, and `gtext_markup_clone()` into the target is
the way across.

A template element's content is a fragment owned by the element, not one of
its children, so a walk of a page does not see inside a `<template>`. Every
walk this library makes of its own (freeing, cloning, comparing, dumping)
visits the content before the children, iteratively.

## Names

| What | Rule |
|---|---|
| Parts of a name | a namespace URI, a prefix and a local name, all NUL-terminated UTF-8 |
| Empty namespace or prefix | the same as NULL, because `xmlns=""` undeclares a namespace and the DOM reads that as null |
| Local name | not empty (`GTEXT_MARKUP_E_NAME`) |
| Bytes that are not UTF-8 | `GTEXT_MARKUP_E_ENCODING`, in a name and in data alike |
| Two names are the same | when namespace and local name are equal; the prefix is spelling |
| Whether a name is an XML name | **not checked by the tree.** The HTML parser builds elements called `a$b` and `x:y`. `gtext_markup_is_xml_name()` and `_ncname()` are what an XML writer asks |

## What may hold what

| Parent | May hold |
|---|---|
| document | one element and one doctype (the doctype first), any number of comments and processing instructions; **no text** |
| fragment, element | elements, text, CDATA sections, comments, processing instructions |
| anything else | nothing |

A doctype goes only in a document. A fragment is never a child: inserting one
inserts its children, in order, and leaves it empty. A document is never a
child. All of it is checked before anything moves.

When more than one thing is wrong, the status is the first of these that
applies: a NULL argument; a node from another document; a `child` that is a
document or a template's content; a parent that cannot hold children; a
reference that is not a child of the parent; a cycle or a kind that may not go
there; the document's one-element rule; and last the depth limit. The order is
documented on `gtext_markup_insert_before()` because a caller can act on it.

## Limits

| Option | Default | Zero means | Stops |
|---|---|---|---|
| `max_depth` | 256 | no limit | an insertion that would put any node deeper, **counting the descendants of a subtree being moved** |
| `max_nodes` | 0 | no limit | creating or cloning a node when the document already holds that many, detached nodes and template contents included |
| `max_attributes` | 1024 | no limit | adding an attribute to an element that has that many; replacing one adds none |

All three fail with `GTEXT_MARKUP_E_LIMIT`, and a failed operation changes
nothing. A cloned tree is not checked against the target's `max_depth` as it is
built (it is detached); the limit applies when it is inserted.

## Cost

Insertion of a childless node is O(depth of the parent) when `max_depth` is
set and O(1) when it is not. Inserting a node that has children costs a walk of
its subtree when `max_depth` is set, because that is how the descendants are
counted; moving large subtrees repeatedly is what a parser's reparenting does,
so the parsers will use an unchecked internal path and enforce their own
depth. Removal is O(1). Freeing, cloning, comparing and dumping are O(size)
and never recurse, so a tree of any depth can be released. (A dump indents by
depth, so its *output* is quadratic in depth by nature.)

## Escaping and the lexical checks

| Mode | Escapes |
|---|---|
| `GTEXT_MARKUP_ESCAPE_XML_TEXT` | `&` `<` `>`, and a carriage return as `&#13;` |
| `GTEXT_MARKUP_ESCAPE_XML_ATTRIBUTE` | the above, plus `"`, and tab, line feed and carriage return as `&#9;` `&#10;` `&#13;` |

Escaping is for text the language can spell. **Checking** is for text it
cannot, and it refuses rather than repairs: a character XML 1.0 forbids even as
a reference (the C0 controls except tab, line feed and carriage return, and
U+FFFE and U+FFFF), `--` or a trailing `-` in a comment, `?>` in a processing
instruction, `]]>` in a CDATA section. The whole input is checked before the
first byte is written, so a refusal has written nothing. Bytes that are not
UTF-8 are `GTEXT_MARKUP_E_ENCODING`; a valid character the language cannot
spell is `GTEXT_MARKUP_E_UNREPRESENTABLE`.

## The HTML vocabulary

`<ghoti.io/text/markup/markup_html.h>` answers the questions the XHTML profile
and, later, the HTML parser and sanitizer ask about names, from tables taken from
the HTML Standard (WHATWG) on 2026-10-11 (the source and date are in the table's
header comment in `src/markup/markup_html.c`):

| Question | Names |
|---|---|
| void (`gtext_markup_html_is_void()`) | `area` `base` `br` `col` `embed` `hr` `img` `input` `link` `meta` `source` `track` `wbr`, and the legacy `basefont` `bgsound` `frame` `keygen` `param` that HTML parsing and serialization still treat so |
| raw text (`..._is_raw_text()`) | `script` `style` |
| escapable raw text (`..._is_escapable_raw_text()`) | `textarea` `title` |
| first line feed dropped (`..._drops_first_newline()`) | `pre` `textarea` `listing` |
| boolean attribute (`..._is_boolean_attribute()`) | the attribute index's rows of type "Boolean attribute": per element (`disabled` on `button`, not on `p`) plus the global `autofocus` `inert` `itemscope` `headingreset`; the obsolete presentational ones are not in it |

Each has a form taking a namespace and a local name, and a `_node_` form taking an
element. **They answer only for the XHTML namespace**: `<br>` in the SVG
namespace, or in none, is not void. Lookups are a binary search over static
tables - O(log n), no allocation, thread-safe - and need no parse step: a tree
read by the XML parser from XHTML already has the namespace. Nothing here sniffs
or parses; see \ref format_xml "XML" for why the parser must always be chosen by
the caller.

One more question is asked of SVG and MathML names and has no `_node_` form:
`gtext_markup_html_foreign_name_survives()` says whether an HTML parser reads an
element or attribute name back as written. It lower-cases every name and restores
the case of 36 SVG element names (`foreignObject`), 58 SVG attribute names
(`viewBox`) and MathML's `definitionURL`; any other name with an upper case letter
(`contentScriptType`, which the standard dropped, is one) comes back as a different
name. Its table is transcribed from parse5 8.0.1's copy of the standard's (the source is
in `src/markup/markup_html.c`), and the XHTML writer uses it.

## Statuses

| Status | When |
|---|---|
| `GTEXT_MARKUP_E_INVALID` | NULL where a pointer is required; an index out of range; the wrong kind of node for the operation; a reference that is not a child of the parent |
| `GTEXT_MARKUP_E_OOM` | an allocation failed |
| `GTEXT_MARKUP_E_LIMIT` | a limit above |
| `GTEXT_MARKUP_E_HIERARCHY` | a cycle, a kind in a place it may not go, a second root element, a doctype after the element |
| `GTEXT_MARKUP_E_WRONG_DOCUMENT` | a node from another document |
| `GTEXT_MARKUP_E_STATE` | freeing a node that is still in the tree, or the root, or a template's content |
| `GTEXT_MARKUP_E_NAME` | an empty local name or target |
| `GTEXT_MARKUP_E_ENCODING` | bytes that are not UTF-8 |
| `GTEXT_MARKUP_E_UNREPRESENTABLE` | text the language has no spelling for |
| `GTEXT_MARKUP_E_WRITE` | a sink refused output |

## Tested scope

- **Contract tests** (`tests/test-markup-tree.cpp`, 95 cases): every creation
  function, the whole parent-by-child kind table written out from the DOM, the
  document's own rules, fragments, cycles, moving and replacing, ownership,
  attributes, template contents, cloning, equality, the dump's exact text,
  every limit at and one past its value, and a tree a hundred thousand deep.
- **A reference model** (`tests/test-markup-model.cpp`): a second tree in plain
  C++, sharing no code with the library, with every operation applied to both
  over random sequences - three sweeps of sixty seeds and 250 operations each,
  with no limits, with tight depth, and with scarce nodes and attributes -
  comparing the status
  returned (including which rule wins when two apply), the library's whole
  state, and the library's own invariant checker after every step. It found a
  defect the contract tests had not: `insert_before(parent, x, x)` for an `x`
  that was not a child of `parent` was treated as an append.
- **Failure injection**: every operation that allocates is run again with its
  first allocation failing, then its second, and so on until it gets through. A
  failed operation must leave the world exactly as it was; the name table's
  growth is best-effort and is allowed to be absorbed.
- **A fuzzer** (`tests/fuzz/fuzz_markup_tree.cpp`): the input is a program for
  building and rearranging a tree, with allocation failures part of the
  program. Checked after every operation: the invariants hold, and anything
  other than OK changed nothing.
- **Mutation controls**: 27 deliberate defects in the tree and the escaping,
  each caught by the tests meant to catch it. Two of them were not at first -
  one was an equivalent mutant, and the other exposed a rule written in two
  places, which is now written in one.
- **Names against two references.** Over every code point, the name predicate
  agrees with the specification's own productions (parsed from the
  specification's text, in the test) and with **libxml2's parser** (2.9.14,
  fifth-edition rules): no disagreement on a name start character, and none on a
  name character apart from tab, line feed, carriage return and space, where
  the probe document `<a\t/>` is well formed because whitespace ends a name,
  which says nothing about whether it can be part of one. **expat** (2.8.3) disagrees on 917,504 supplementary-plane code points
  and about 19,000 in the BMP: its name tables are those of an earlier edition,
  and the fifth edition's broader ranges are what the predicate implements.
  This is a one-off run recorded here; the pinned differential arrives with the
  XML parser.

## Gaps

- The XML parser fills the tree and the XML writer reads it back
  (\ref format_xml "XML"); the HTML parser and serializer are later phases.
- HTML's escaping modes (`&nbsp;`, attribute quoting) belong to the HTML
  serializer and are not here.
- The vocabulary is the standard's lists as of its date; it does not follow the
  living standard on its own, and the legacy presentational boolean attributes
  (`compact`, `nowrap`, ...) are left out.
- Nothing in the tree validates namespace constraints (a prefix bound to a
  namespace, `xmlns` declarations in scope). The XML parser enforces them as it
  reads; a tree built by hand is not held to them.
- No `normalize()` (merging adjacent text nodes), no ID lookup, no
  `getElementsBy*`. XPath and selectors are later.
- Attribute order is preserved and not significant to equality, as in the DOM.
