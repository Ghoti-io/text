@page format_xml XML

# XML

`gtext_xml_parse()` reads a whole **UTF-8 XML 1.0 (Fifth Edition)** document
with **Namespaces in XML 1.0** and builds a
\ref format_markup "Markup tree" from it. This is the first slice of the XML
phase: the DOM parse. The event parsers (push and pull), the writer, the file
entry points, UTF-16 and UTF-32, and the reference-parser differential are the
next slices and are not here; the **Gaps** section lists them.
Back to \ref text_format_references "Formats".

## Normative references

- [Extensible Markup Language (XML) 1.0 (Fifth Edition)](https://www.w3.org/TR/xml/):
  well-formedness, the character and name productions, line-end normalization
  (2.11), attribute-value normalization (3.3.3), the document type declaration
  (2.8, only as far as finding its end), and the byte order and encoding rules
  of Appendix F (only as far as recognizing what is not UTF-8).
- [Namespaces in XML 1.0 (Third Edition)](https://www.w3.org/TR/xml-names/):
  prefix declaration and scope, the reserved `xml` and `xmlns`, qualified names.
- The [W3C XML Conformance Test Suite](https://www.w3.org/XML/Test/) (release
  `xmlts20130923`), pinned by hash, as the score below.

## What the parse produces

| Input | Tree |
|---|---|
| element | an element with namespace URI, prefix and local name; the default namespace applies to it and **not** to its attributes |
| `xmlns="u"`, `xmlns:p="u"` | kept as attributes in the namespace `http://www.w3.org/2000/xmlns/` (prefix NULL and local `xmlns`; prefix `xmlns` and local `p`), in source order with the others |
| character data | one text node up to the next markup, with the five predefined entities and numeric references already expanded and CR LF and lone CR turned into LF |
| CDATA section | a CDATA node of its own; adjacent ones are not merged |
| comment, processing instruction | their nodes |
| white space outside the root element | not kept |
| attribute value | normalized as CDATA: each literal tab, line feed and carriage return becomes one space; a *reference* to one stays itself |
| `<?xml ... ?>` | the document's version (`"1.0"`), standalone flag and encoding label; with no declaration the version is NULL and the encoding is `UTF-8` from the default |
| `<!DOCTYPE ...>` | a doctype node: name, public identifier (white space collapsed, as the specification asks), system identifier |

Every element, text, CDATA, comment, processing instruction and doctype node
carries its source location, and so does every attribute. A location is the
byte offset, the line and the column of the construct's first character; a line
ends at LF, CR or CR LF, a column counts characters, and a UTF-8 byte order mark
counts in the offset and not in the columns.

## The document type declaration is recognized, not processed

The internal subset is **skipped by a bounded scanner**. It knows where each
kind of token ends - a comment at `-->`, a processing instruction at `?>`, a
declaration at the first `>` outside a quoted literal - so a `]` inside a
literal, a comment or a processing instruction does not end the subset (the
classic miss), and an unterminated subset is an error. It moves forward on every
step and is linear in the input.

It checks characters, that each declaration is delimited, that nothing which
cannot occur in a declaration (a `<`, a `]`, a parameter entity reference)
appears outside a literal, and that white space follows the declaration keyword.
It does **not** check a declaration's grammar. No declaration takes effect: not
an entity, not a default attribute, not an attribute type. Consequently:

- **Any entity reference other than the five predefined ones and numeric
  references is an error** (`GTEXT_XML_E_ENTITY`), declared or not, and the
  position is the `&`.
- External identifiers (`SYSTEM`, `PUBLIC`) are recorded in the doctype node and
  **never opened**. The parser reads no file and makes no network request, and
  there is no option that turns that on; this is the XXE decision, made once.
- Attribute values are normalized as CDATA, because no attribute is typed.

The parser keeps three internal function pointers where the DTD phase attaches
(entity replacement text, attribute types, default attributes). They are NULL
for every public parse, each is consulted at exactly one place, and
`tests/test-xml-limits.cpp` checks both that NULL changes nothing and that a
non-NULL one is honoured.

## Encodings

UTF-8 only, and the decision is explicit:

| Input | Result |
|---|---|
| UTF-8, with or without a byte order mark | parsed; a mark is skipped and the document's encoding source is `BOM` |
| UTF-16 or UTF-32, by byte order mark or by the first bytes (Appendix F) | `GTEXT_XML_E_UNSUPPORTED_ENCODING`, at offset 0 |
| EBCDIC, by its first bytes | the same |
| an XML declaration whose `encoding` is anything but `UTF-8` (case-insensitive), including `US-ASCII` and `ISO-8859-1` | the same, positioned at the label |
| an XML declaration whose `encoding` is not an `EncName` | `GTEXT_XML_E_SYNTAX` |
| a version other than 1.0 | `GTEXT_XML_E_UNSUPPORTED_VERSION` for `1.<digits>`, `GTEXT_XML_E_SYNTAX` for anything the production does not allow |
| bytes that are not UTF-8 | `GTEXT_XML_E_UTF8` |
| UTF-8 that is not an XML character | `GTEXT_XML_E_CHARACTER` |

An unsupported answer is a statement about the **parser** and not about the
document: the document may be well-formed. The conformance score keeps those
cases apart for that reason. UTF-16 and UTF-32 by byte order mark, the
declaration checked against what was detected, and a decision about
`ISO-8859-1` and `US-ASCII` are the next slice.

## Limits

Each has a default from the first release; `0` in the options selects it, and
`0` never means "unlimited" (`SIZE_MAX` is as close as the type allows).

| Limit | Default | Status when exceeded |
|---|---|---|
| input length | 64 MiB | `GTEXT_XML_E_LIMIT_INPUT`, before anything is read |
| element depth (the root is 1) | 256 | `_LIMIT_DEPTH` |
| attributes on one element, declarations included | 1024 | `_LIMIT_ATTRIBUTES` |
| an attribute value, after expansion | 16 MiB | `_LIMIT_VALUE` |
| a name, in bytes | 8192 | `_LIMIT_NAME` |
| nodes in the document, the document node included | 10,000,000 | `_LIMIT_NODES` |
| namespace declarations in scope at once | 256 | `_LIMIT_NAMESPACES` |

The parser holds its open elements in a heap array, so a document nested a
million deep is refused at the limit and never reaches the C stack; with the
limit lifted, 300,000 levels parse, free and walk. The name limit is 8192
because the conformance suite has a processing-instruction target of 4,248
bytes. Duplicate attribute detection is a hash set that empties in constant time
per element, so an element with a thousand attributes is not a quadratic cost.
The document keeps the depth, node and attribute limits for later edits (its
depth limit is one more than the elements', because the character data inside
the deepest element is one level further down).

## Errors

On any failure there is **no tree** and nothing is leaked, including when the
allocator fails at any point; `*out` is written only on success. The error
carries a static message and the position of the construct at fault: the `&` of
a bad reference, the `</` of a mismatched end tag, the second of two equal
attributes, the token that went over a limit, otherwise the first character the
parser could not accept.

| Status | Meaning |
|---|---|
| `GTEXT_XML_E_SYNTAX` | not well-formed, no more specific status |
| `_UNEXPECTED_EOF` | the input ended inside a construct or before the root element |
| `_UTF8`, `_CHARACTER` | the bytes, or the character, as above; also a numeric reference to a character XML forbids |
| `_MISMATCHED_TAG` | an end tag that is not its start tag's |
| `_DUPLICATE_ATTRIBUTE` | the same name twice, or two names with the same namespace and local name |
| `_ENTITY` | a reference other than the predefined five and numeric ones |
| `_NAMESPACE` | an unbound prefix, a name with two colons, `xmlns` or `xml` misused, a prefix undeclared, a colon in a processing-instruction target |
| `_DOCTYPE` | a malformed document type declaration or internal subset |
| `_UNSUPPORTED_ENCODING`, `_UNSUPPORTED_VERSION` | as above |
| `_LIMIT_*` | a limit above, each its own status |
| `_INVALID`, `_OOM` | an unusable argument; an allocation failed |

## Conformance

`make conformance-xml` scores the parser against the W3C XML Conformance Test
Suite, pinned by SHA-256 (`tools/xml/XMLCONF_SHA256`, fetched by
`tools/xml/fetch.sh` into `third_party/xml/`, which is not tracked). The gate
fails, and says how to fetch, when the data is absent; it never skips.

Scored **by outcome**:

- every **not-well-formed** case must be refused;
- every **valid** and **invalid** case is a well-formed document and must be
  accepted - except one the parser refuses for naming an entity, which needs the
  DTD that declares it processed and is counted apart. The two classes are
  reported separately, and whether a document is *valid* is not asked until the
  DTD phase.

Counted and not scored, each with its reason: XML 1.1 cases, cases that apply to
processors that are not namespace aware, cases for editions before the fifth,
cases the parser answers with an unsupported encoding or version, and the
suite's `error` class (left to the processor by the specification).

A not-well-formed case scores only if refused for a well-formedness reason (a
limit or out-of-memory refusal does not count). A scored case that fails and is not in `tools/conformance/XMLCONF_EXCEPTIONS`
fails the gate, **and so does an exception that no longer fails**, so the list
cannot outlive its reasons. Every exception is a not-well-formed document that is
not-well-formed only through something inside a DTD - a declaration's grammar,
a reference that is wrong against a declaration, or an external subset - which
this parser skips and a later DTD-processing step will parse.

Two things the suite itself got wrong, found while scoring:

- the 2013 catalogue gives the `eduni/misc` cases the base `eduni/namespaces/misc/`,
  where there are no files; they are in `eduni/misc/`, and the runner looks there
  second and counts them;
- the catalogue's `ENTITIES="none"` means no *external* entity, and many such
  documents use an internal one, so it cannot be used to decide which well-formed
  documents are readable without a DTD.

## Tested scope

- **Matrix and grammar tests** (`tests/test-xml-parse.cpp`, 32 cases): each row
  of the specification's matrix, every production the parser reads, the exact
  status and position of several hundred refusals (written from the
  specifications, in lines, columns and bytes counted by hand, including all
  three line-end forms and multi-byte characters), the document type scanner
  over its hostile cases, namespace scope and its errors, numeric references
  past 32 and 64 bits, and a document 300,000 levels deep.
- **Limits and failure injection** (`tests/test-xml-limits.cpp`): each limit at
  its value, one past it and far past it, as its own status; the allocator
  failing at every one of the rich document's allocations in turn (more than 150
  of them), and of four refused documents, each time with no tree, no leak and no
  bad free; the three DTD seams. The tree's name table grows on a best-effort
  basis, so one failed allocation there is absorbed by design; the test counts
  those so that a second kind would be noticed.
- **A model that shares no code** (`tests/test-xml-model.cpp`): 4,000 random
  documents generated as text and as a tree at once, in every spelling XML
  allows, parsed and compared node by node; then every proper prefix of 300
  documents that ends before the root does, each of which must be refused.
- **Every tree is held to the invariant checker** that the Markup tree is.

## Gaps

- No event parsers (push `GTEXT_XML_Stream`, pull `GTEXT_XML_Reader`), so no
  check that chunk boundaries do not matter.
- No writer, no `gtext_xml_parse_file` / `_write_file`, no round trip.
- No UTF-16 or UTF-32, and no decision yet about `ISO-8859-1` and `US-ASCII`.
- No fuzzers and no reference-parser differential (libxml2, expat): the scores
  above are against the specification's own suite only. Where those two disagree
  with each other about names, the Markup tree page has the measurement.
- No DTD processing: declarations are skipped, entities other than the
  predefined five are refused, and 340 not-well-formed documents whose fault is
  inside a DTD are accepted (listed, with their reasons).
- No XML 1.1, no canonicalization, no XPath. None is planned in this phase.
