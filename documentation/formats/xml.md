@page format_xml XML

# XML

`gtext_xml_parse()` reads a whole **XML 1.0 (Fifth Edition)** document, in UTF-8,
UTF-16, UTF-32, ISO-8859-1 or US-ASCII, with **Namespaces in XML 1.0** and builds
a \ref format_markup "Markup tree" from it. `GTEXT_XML_Stream` (push) and
`GTEXT_XML_Reader` (pull) read the same documents as a stream of events, in
pieces, without building a tree (**Event parsers** below). `gtext_xml_write()` and
its buffer and file forms write a tree back as XML, and `gtext_xml_parse_file()`
reads a file (**Writer** and **Files** below). The fuzzers and the
reference-parser differential are the next slice and are not here; the **Gaps**
section lists them.
Back to \ref text_format_references "Formats".

## Normative references

- [Extensible Markup Language (XML) 1.0 (Fifth Edition)](https://www.w3.org/TR/xml/):
  well-formedness, the character and name productions, line-end normalization
  (2.11), attribute-value normalization (3.3.3), the document type declaration
  (2.8, only as far as finding its end), and the byte order and encoding rules
  of Appendix F, and 4.3.3 for the declaration checked against the bytes.
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
counts in the offset and not in the columns. For input that is converted (UTF-16,
UTF-32, ISO-8859-1, US-ASCII) a line and column still count characters of the
original, but the **offset counts bytes of the UTF-8 form** the parser read, and
a byte order mark is not counted at all.

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

The parser reads UTF-8. Anything else it supports is converted to UTF-8 first,
in one pass and through the document's allocator, so the parse itself is the same
for every encoding. The document's encoding name and its source (`BOM`,
`DECLARATION`, `DEFAULT`) describe the **original**: a UTF-16 document with a mark
says `UTF-16` from `BOM`, and one declared `ISO-8859-1` says `ISO-8859-1` from
`DECLARATION`. The input limit applies to the original bytes.

| Input | Result |
|---|---|
| UTF-8, with or without a byte order mark | parsed; a mark is skipped and the document's encoding source is `BOM` |
| UTF-16 or UTF-32, little or big endian, by byte order mark or by the first bytes of `<?` (Appendix F; a mark-less UTF-16 document that starts with `<` and anything else is read as UTF-8 and refused) | converted and parsed. A document with no mark and no declaration is read as the family its first bytes show, named `UTF-16` or `UTF-32` from `DEFAULT` |
| a declaration `encoding="ISO-8859-1"` or `"US-ASCII"` (case-insensitive) in a document with no mark | converted (every byte of Latin-1 is the code point of that value) and parsed. The label is read from the declaration in the raw bytes before converting |
| a UTF-16 or UTF-32 document whose label names another family (`UTF-8`, `ISO-8859-1`, `US-ASCII`, or the other width) | `GTEXT_XML_E_ENCODING_MISMATCH`, at the label |
| a UTF-8 document (with or without a mark) labeled `UTF-16` or `UTF-32`, or one with a UTF-8 mark labeled `ISO-8859-1` or `US-ASCII` | the same |
| a bare label `UTF-16` or `UTF-32` | matches either byte order |
| a lone or reversed surrogate, a UTF-32 unit past U+10FFFF or in the surrogate range, a document that ends inside a code unit, a byte of 0x80 or above under `US-ASCII` | `GTEXT_XML_E_INVALID_SEQUENCE`, at the unit |
| the labels `UTF-16LE`, `UTF-16BE`, `UTF-32LE`, `UTF-32BE` and aliases such as `UCS-2` or `latin1` (declined on purpose) | `GTEXT_XML_E_UNSUPPORTED_ENCODING` at the label |
| EBCDIC, by its first bytes; any other label (`windows-1252`, `Shift_JIS`, ...) | `GTEXT_XML_E_UNSUPPORTED_ENCODING` (offset 0 for EBCDIC, the label otherwise) |
| an XML declaration whose `encoding` is not an `EncName` | `GTEXT_XML_E_SYNTAX` |
| a version other than 1.0 | `GTEXT_XML_E_UNSUPPORTED_VERSION` for `1.<digits>`, `GTEXT_XML_E_SYNTAX` for anything the production does not allow |
| bytes that are not UTF-8 | `GTEXT_XML_E_UTF8` |
| UTF-8 that is not an XML character | `GTEXT_XML_E_CHARACTER` |

An unsupported answer is a statement about the **parser** and not about the
document: the document may be well-formed. The conformance score keeps those
cases apart for that reason. The mismatch and invalid-sequence answers are
statements about the **document** and score as well-formedness refusals. The
other labels, a streaming decoder, and the four unusual byte orders of
Appendix F are not supported.

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
| `_INVALID_SEQUENCE` | bytes that are not valid in the document's encoding, positioned at the unit |
| `_ENCODING_MISMATCH` | the declaration names a different family from the one the document is in, positioned at the label |
| `_LIMIT_*` | a limit above, each its own status |
| `_INVALID`, `_OOM` | an unusable argument; an allocation failed |
| `_INCOMPLETE`, `_STATE` | event parsers only: the pull reader needs more input; a call that does not fit the state (input after the end) |

## Event parsers

```c
#include <ghoti.io/text/xml.h>

static GTEXT_XML_Status on_event(const GTEXT_XML_Event * e, void * user) {
  if (e->type == GTEXT_XML_EVENT_ELEMENT_START) {
    printf("<%s> with %zu attributes\n", e->local_name, e->attribute_count);
  }
  return GTEXT_XML_OK;            /* anything else stops the parse */
}

GTEXT_XML_Stream * s = gtext_xml_stream_new(NULL, on_event, NULL);
GTEXT_XML_Error err;
while ((n = read(fd, buf, sizeof buf)) > 0) {
  if (gtext_xml_stream_feed(s, buf, (size_t)n, &err) != GTEXT_XML_OK) { /* refused */ }
}
gtext_xml_stream_finish(s, &err);   /* the end of the input: DOCUMENT_END, or why not */
gtext_xml_stream_free(s);
```

`GTEXT_XML_Stream` has the shape of the CSV stream: `_new` with a callback,
`_feed`, `_finish`, `_free`. `GTEXT_XML_Reader` is the pull form: `_new`, `_feed`
(`NULL, 0` ends the input, and doing it twice is accepted), `_next`, `_free`. It
wraps the push parser and copies each event, strings and attributes included,
into a queue, so an event stays valid until the next `_next()`; the push parser's
pointers are valid only during the callback. `_next()` returns
`GTEXT_XML_E_INCOMPLETE` when it needs more input, `GTEXT_XML_E_STATE` after the
last event of a finished document, and the parse error, again and again, once one
has happened (events queued before it are delivered first). Input after the end is
`E_STATE`. The two statuses are appended to the enumeration.

A pull loop, reading a file in pieces:

```c
GTEXT_XML_Reader * r = gtext_xml_reader_new(NULL);
GTEXT_XML_Error err;
size_t n;
int more = 1;
while (more) {
  n = fread(buf, 1, sizeof buf, f);
  /* NULL, 0 ends the input */
  if (gtext_xml_reader_feed(r, n ? buf : NULL, n, &err) != GTEXT_XML_OK) {
    /* err says why and where; the events before it can still be read */
    break;
  }
  more = n != 0;
  GTEXT_XML_Event e;
  GTEXT_XML_Status st;
  while ((st = gtext_xml_reader_next(r, &e)) == GTEXT_XML_OK) {
    /* use e; its pointers are valid until the next call */
  }
  if (st == GTEXT_XML_E_STATE) {
    break;                    /* the last event of a finished document */
  }
  /* GTEXT_XML_E_INCOMPLETE: feed more. Anything else is the parse error. */
}
gtext_xml_reader_free(r);
```

A document that is cut short is refused by the feed that ends the input, so its
status is checked there, not only at the end of the loop.

**The events.** `DOCUMENT_START` (version, encoding and why, standalone),
`DOCTYPE`, `ELEMENT_START` (namespace, prefix, local name, and the attributes, each
resolved and located; `xmlns` declarations are attributes, as in the tree),
`ELEMENT_END` (the same names; an empty-element tag is a start and an end),
`TEXT`, `CDATA`, `COMMENT`, `PROCESSING_INSTRUCTION`, `DOCUMENT_END`. Every event
has the location of its construct, the same one the tree's node would have, and
replaying the events into a tree gives a tree equal to `gtext_xml_parse()`'s,
with the location of every node and attribute (the tests and the conformance
gate do exactly that). The locations of `ELEMENT_END` and of the document's start
and end are not in a tree; the event-log tests check them by hand.

**Chunk boundaries do not change the events.** Character data is *coalesced*: it
is delivered as one `TEXT` event when the next markup begins, whatever it was
cut across and however many references and line ends it contains (a CR LF cut
between its bytes is one line feed). A construct is never cut: a start tag, a
comment, a CDATA section and the rest are delivered whole, when their last byte
has arrived. What can still change is held back - a multi-byte character, a
carriage return, a `]` or two before a possible `>` - and nothing else.

**Memory and time.** Memory is bounded by the largest single construct, not by
the document: bytes that have been read are dropped. (A run of character data is
one construct, so it is held until it ends; a start tag with a megabyte of
attribute values is held until its `>`.) **A construct that never ends is bounded
only by `max_input_bytes`**: an unterminated start tag, comment or processing
instruction is held, with no event and no error, until it ends, the input ends,
or the total fed passes that limit. The name, attribute and namespace limits
apply when the construct is read, not while it is waited for. A caller that
streams should set `max_input_bytes` to what it can hold; the 64 MiB default also
caps the size of the whole document. The exception is the XML declaration, which
is waited for up to 64 KiB and then refused with `GTEXT_XML_E_SYNTAX`. Work is **linear in the
input however it is cut**. The parser does not try a construct again when a byte
arrives: a small second state machine per kind of construct (comment, processing
instruction, CDATA section, start tag, end tag, reference, document type, XML
declaration) keeps its place between feeds and only looks for where the construct
*ends*, so each byte is looked at a constant number of times by it and once by
the real lexer, which runs only on a construct that is complete. The bound is
tested, not asserted: a megabyte comment, character data, CDATA section,
processing instruction and attribute value, and quarter-megabyte adversarial
shapes (a comment of `>`, of `->`, a start tag of white space, a document type of
a thousand declarations with `>` in their literals, a character reference of
digits), each fed one byte at a time, finish in a fraction of a second where a
quadratic parser would need hours. If that scan and the lexer ever disagree
about a malformed construct, the attempt is undone and not repeated until the
input ends, so the cost stays linear and the verdict is still the lexer's. The
lexer, given a buffer that ends in the middle of a token (a multi-byte character,
a name, `?>`, `SYSTEM`), says that the input ended and never invents a syntax
error; a test runs it after every byte of many damaged documents, bypassing
the scan, to show that the verdict is the DOM parse's wherever the buffer ends.

**The same verdicts.** The lexer, namespace scope, document type scanner and
limits are the DOM parser's own code - the tree is the only thing the event
parser leaves out - so a document is refused with the same status, message,
offset, line and column, however it is cut, and `make conformance-xml` holds
that to the whole suite (below). Three things differ from a whole-input parse
and are by construction:

- *When.* A mistake early in a construct is reported when the construct is
  complete (or at `_finish`), not at the byte itself, and the events before the
  construct have been delivered. A document that ends too soon is refused by
  `_finish` (`GTEXT_XML_E_UNEXPECTED_EOF`).
- *Bytes invalid in the encoding.* `gtext_xml_parse()` converts a UTF-16, UTF-32,
  ISO-8859-1 or US-ASCII document whole before reading it, so it reports a bad
  unit (`_INVALID_SEQUENCE`) even if the document is not well-formed earlier. The
  event parser converts as bytes arrive and reports whichever comes first.
- *`max_input_bytes`.* It counts everything fed; when the total passes it the
  call fails with `_LIMIT_INPUT` at offset 0, line 1, column 1, as the DOM parse
  does, even if a syntax error would have been found in what came before.
  `max_nodes` counts what the tree would hold (the document node included), so
  both refuse the same documents at the same place.

**Encodings.** The same five. The first four bytes decide UTF-16, UTF-32 and a
UTF-8 mark, and for a document with none that begins `<?xml` the parser waits for
the declaration to end, because it may say `ISO-8859-1` or `US-ASCII`; until then
nothing is delivered. After that the input is converted piece by piece, carrying
a cut code unit or a surrogate pair into the next piece.

**The callback.** It may return any status other than `GTEXT_XML_OK` to stop. That
status is returned from the `_feed` or `_finish` that was running, and the stream
is finished: every later call returns it again. Events before the stop have been
delivered, none after it. A stream is also finished by a refusal, a limit and
out-of-memory (`GTEXT_XML_E_OOM`, with nothing leaked); `_free` is always safe.

## Writer

```c
#include <ghoti.io/text/xml.h>

GTEXT_XML_Write_Options o = gtext_xml_write_options_default();
o.indent = "  ";                          /* NULL (the default) is compact */
o.encoding = GTEXT_XML_OUTPUT_UTF16LE;    /* UTF-8 by default */

GTEXT_XML_Write_Result r;
char * bytes = NULL;
size_t n = 0;
if (gtext_xml_write_buffer(doc, &o, &bytes, &n, &r) != GTEXT_XML_OK) {
  fprintf(stderr, "%s at %s\n", r.message, r.path);    /* e.g. "/a/b[2]/comment()" */
} else {
  /* r.elements_indented, r.elements_skipped_mixed, ... say what it did */
  gtext_xml_write_buffer_free(doc, &o, bytes);
}
```

`gtext_xml_write()` takes a `GTEXT_Markup_Sink` (the one `gtext_markup_escape()`
uses: a `write(user, bytes, len)` returning a `GTEXT_Markup_Status`);
`gtext_xml_write_buffer()` and `gtext_xml_write_file()` are the same writer into
a buffer and into a file. A buffer is released with `gtext_xml_write_buffer_free()`
given the same options (it belongs to `opts->allocator`, else the document's; a
NULL document frees nothing unless the options carry the allocator). On failure
`out` and `out_len` are untouched. The document must not be modified while a
write is running. The file form checks the tree before it creates the temporary
file, so a refused tree creates nothing. The options, the result and the statuses are in
`xml_writer.h`.

**The contract.** `parse(write(tree))` is a tree equal to `tree`
(`gtext_markup_equal()`; locations are not compared), and `write(parse(write(tree)))`
is the same bytes. That holds for every tree the writer accepts, with two
exceptions XML cannot tell apart and the writer counts instead of hiding
(`text_nodes_coalesced`): an empty text node, and a text node that follows another
text node. It holds for the whole of the W3C suite (below). Pretty printing adds
white space and so changes the tree by design.

**What is refused, and how.** A tree XML has no spelling for is refused with a
status, the path of the node (`r.path`: `/a/b[2]/@x`, an index only where there
are same-named siblings; a long path keeps its end behind `...`) and, where a
name is at fault, the name (`r.name`). Nothing is repaired:

| The tree holds | Status |
|---|---|
| a comment with `--` or ending in `-`; processing instruction data with `?>` or starting with white space; a CDATA section with `]]>`; a carriage return in a comment, a processing instruction or a CDATA section (XML reads it back as a line feed, and there is no reference to hide it behind) | `_UNREPRESENTABLE` |
| a character XML forbids (a C0 control other than tab, line feed and carriage return, U+FFFE, U+FFFF), in text, an attribute, a comment, anything | `_CHARACTER` |
| a name that is not an NCName (an element, attribute or prefix), a document type name that is not a Name, a processing instruction target that is not an NCName or is `xml` in any case | `_NAME` |
| an element or attribute whose prefix is not declared in scope, or is bound to another namespace; an `xmlns` attribute that contradicts the namespace of the element that carries it (or of an attribute using that prefix); a prefix with no namespace, or an attribute in a namespace with no prefix; `xmlns`, `xml` or their namespaces used for anything else; a declaration that undeclares a prefix | `_NAMESPACE` |
| a character the output encoding has no byte for, in a place with no reference (a name, comment, processing instruction, CDATA section, identifier in the document type) | `_UNREPRESENTABLE` |
| a public identifier with no system identifier, with a character it cannot hold or with white space the parser would collapse; a system identifier with both kinds of quote or a carriage return | `_DOCTYPE` |
| a template element's content (it is not a child) or a document with no root element | `_UNREPRESENTABLE` |
| a document version other than `1.0` | `_UNSUPPORTED_VERSION` |
| nesting past `max_depth` (256), output past `max_output_bytes` (256 MiB) | `_LIMIT_DEPTH`, `_LIMIT_OUTPUT` |

**A refusal writes nothing to the sink.** The writer walks the tree twice with
the same code: first checking everything and counting the bytes, without output,
then for real. That is how the output limit is a refusal and not a truncation, and
why a 100,000-deep tree asked to be pretty printed (which would write depth
squared bytes of indent) is refused at once. A sink that fails during the second
walk (`_WRITE`, or `_OOM` when it returned `GTEXT_MARKUP_E_OOM`) has received a
prefix; the file form writes to a temporary file and so leaves the old file alone.

**Escaping.** Text: `&` `<` `>` and a carriage return as `&#13;`. Attribute values,
in double quotes: those and `"`, and tab, line feed and carriage return as `&#9;`
`&#10;` `&#13;`, because a raw one would be read back as a space. `>` is always
written `&gt;`, so `]]>` cannot occur in text. Under ISO-8859-1 and US-ASCII a
character the encoding lacks is written `&#x20AC;` in text and attribute values.

**Options.**

| Option | Default | |
|---|---|---|
| `xml_declaration` | `AUTO` | `AUTO` writes `<?xml ...?>` when the document has a version or standalone flag, or the output is ISO-8859-1; `ALWAYS`; `NEVER`. ISO-8859-1 is the one that forces it because its output can hold bytes that are not UTF-8, which only the declaration tells a reader about, so a document that had none comes back with `version="1.0"`; US-ASCII output is UTF-8 and UTF-16 has its byte order mark, so neither needs one. With `NEVER`, ISO-8859-1 output of a character from U+0080 to U+00FF is refused (`_UNREPRESENTABLE`). Version and standalone are the document's, and a version other than `1.0` is refused in every mode; the encoding is the output's (`UTF-8`, `UTF-16`, `ISO-8859-1`, `US-ASCII`), not the label the document was parsed with |
| `encoding` | UTF-8 | UTF-16 little or big endian (with a byte order mark), ISO-8859-1, US-ASCII |
| `expand_empty_elements` | off | `<a></a>` instead of `<a/>` |
| `declare_namespaces` | off | add the `xmlns` declarations a name needs, on the element that needs them after its other attributes (`namespace_declarations_added`), instead of refusing. It rebinds a prefix an enclosing element bound to another namespace, never one the element's own name or another attribute already relies on, never contradicts the element's own `xmlns` attribute, and never invents a prefix for an attribute |
| `indent`, `newline` | NULL, `"\n"` | `indent` NULL is compact; otherwise spaces and tabs. `newline` is a string of CR and LF |
| `max_depth`, `max_output_bytes` | 256, 256 MiB | |
| `allocator` | the document's | used for the writer's working state and for the buffer of `_write_buffer` |

**Pretty printing never changes what is significant, and says what it did.** An
element's children are put on lines of their own only when all of them are
elements, comments and processing instructions, and never in or below an element
with `xml:space="preserve"` (an `xml:space="default"` below it does not undo that:
the safe reading). An element with a text or CDATA child, and everything under
it, is left exactly as it was, as `xmllint --format` does. The result counts
`elements_indented`, `elements_skipped_mixed` and `elements_skipped_preserve`
(elements that have children and were not laid out, by the reason); an element
with no children is none of them. The newline option is also what separates the
top-level items (declaration, document type, comments, root element), whether or
not indentation is on. A pretty-printed document read back has the white space as
text nodes, so its elements are mixed and writing it again gives the same bytes
when the newline is a bare line feed (with `"\r\n"` the carriage return is read
back as a line feed).

**The document type** is written from its name and public and system identifiers.
The internal subset is not stored by the parser and cannot be written. The tree
does not record whether the source had one, so `doctype_without_subset` is set
for every document type written, subset or not; it says what is true of the
output (no subset in it), not that one was lost. DTD output is a later
phase.

**Order and depth.** Attributes are written in tree order, which is source order
for a parsed tree; the walk uses the parent and sibling links and a heap array of
frames, never the C stack, and namespace scope is a hash table from prefix to its
innermost binding, so a lookup costs the same at any depth. A tree 100,000 levels
deep with a new prefix at every level is written in well under a second.

## Files

`gtext_xml_parse_file()` reads through the library's file layer as the CSV and
JSON file functions do: a pipe works, the size limit (`max_input_bytes`) is applied
while reading, and the statuses are the same ones: `GTEXT_XML_E_INVALID` with
"No such file or directory", "Permission denied opening file" or "Failed to
open/read file" at line 1, column 1; `_LIMIT_INPUT`; `_OOM`.
`gtext_xml_write_file()` writes to a temporary file in the destination's directory,
commits it and renames it over the destination, so a failed write, a refused tree
and the output limit all leave the previous file as it was and no temporary behind.

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

**The event parsers are held to the same suite.** The runner is run three ways on
every case that is in scope: `gtext_xml_parse()`, the event parser fed the whole
document in one call, and the event parser fed one byte at a time. The second and
third must give **exactly the DOM parse's line**: the same status, offset, line,
column and message for a refusal, and for an acceptance a tree - built from the
replayed events - equal to the DOM parse's in every name, value and node and
attribute location. A case where they differ fails the gate. The one allowed
difference is the order of an encoding error, above; it is pinned to one case by
id in the script, `hst-lhs-009` (a UTF-16 mark followed by single-byte text), and
any other case that would need it fails.

**The writer is held to the same suite.** For every in-scope case the parser
accepts the runner writes the tree in UTF-8, UTF-16 LE and BE,
ISO-8859-1 and US-ASCII, parses each output and requires a tree equal to the
original (and the same declaration), then writes that tree again and requires the
same bytes. ISO-8859-1 and US-ASCII may refuse a document with a character they
lack in a name, comment, processing instruction or CDATA section
(`_UNREPRESENTABLE`, counted, with `XMLCONF_MAX_ROUNDTRIP_REFUSED` a ceiling on
how many); any other refusal or difference fails the gate, and
`XMLCONF_MIN_ROUNDTRIP` is a floor on how many were run. A document with no
declaration comes back from ISO-8859-1 with `version="1.0"` (the runner asserts
that) and from every other output as it was.

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
- **Encodings** (`tests/test-xml-encodings.cpp`): each row of the encoding matrix,
  the mismatches in both directions, malformed units, lines, columns and offsets
  for converted input, the input limit on original bytes, 300 random documents
  encoded in every supported encoding (with and without a mark) and compared
  with the tree of the same document in UTF-8, and the allocator failing at every
  point of converting four documents.
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

- **The event parsers** (`tests/test-xml-stream.cpp`; replay and
  comparison in `tests/xml_replay.h`): generated documents (namespaces,
  every reference and line-end spelling, CDATA, comments, processing
  instructions, a document type with a hostile subset) cut whole, by bytes, at
  random sizes and **at every single offset** and at every pair of offsets of a
  hard one, each giving one event sequence (compared as text, locations
  included) and a replayed tree equal to the DOM parse's; the same documents
  damaged by one or two bytes, hand-written refusals, and every
  proper prefix, each refused with the DOM parse's status and position however
  cut; every encoding, with the unit cut at every byte; the callback stopping
  at every event; every limit against the DOM parse's; the push parser and the
  reader with the allocator failing at every point (UTF-8, UTF-16, UTF-32,
  Latin-1; whole and by bytes); linear work, with time bounds for many
  shapes and a ratio, and a count of bytes examined for the declaration wait;
  memory, with a long run of small elements held to a peak of a few
  kilobytes; and **timeliness**, that every construct but the
  document's end is delivered by `_feed` alone. The tests were checked by
  mutation: removing the hold-back of a `]`, of a CR or of a partial character,
  of the byte kept behind the cursor, or making the scan for the end of a
  construct restart or get a quote or a dash count wrong each fails one.

- **The writer** (`tests/test-xml-writer.cpp`): every row of the
  specification's matrix; each refusal with its status and path, and that the sink
  was not called; every way a namespace can be wrong and the option that adds the
  declarations; shapes the tree itself does not allow, pinned so a change to the
  tree would show; every output encoding byte for byte, including supplementary
  characters in UTF-16 and references in the one-byte encodings; the declaration
  modes; pretty printing, its counts, `xml:space`, the newline and indent; the
  document type; empty and adjacent text nodes; the depth and output limits at,
  one under and one over their values; a tree 100,000 levels deep written and read
  back; a 100,000-deep scope with a new prefix per level, with a time bound; the
  sink failing at every call and the allocator failing at every allocation
  (compact, pretty, UTF-16, ASCII, namespace-adding, and the document's
  allocator), each time with the right status and nothing leaked; files (round
  trip in two encodings, missing, unreadable, over the limit, refused trees
  leaving the old file and creating none, the allocator failing at every point of
  a file write). **A model that shares no code**: random trees generated as a
  plain C++ structure, serialized by a second implementation of the rules
  (compact and pretty), built into the library's tree and written - the two byte
  strings must be equal - then parsed back equal and written again identically,
  in UTF-16 too; more in the one-byte encodings; and more with every declaration
  stripped and `declare_namespaces` on, whose names must come back in their
  namespaces. `XML_WRITER_SEEDS=<n>` runs those sweeps with that many seeds. The
  tests were checked by mutation: deliberate defects in the writer (a refusal
  check removed, the dry walk removed, a limit off by one, `xml:space` not
  inherited, mixed content indented, a contradiction ignored, swapped surrogates,
  a leaked array, ...) are each caught by the test meant to catch them.

## Gaps

- The writer has no streaming (event) form, no canonicalization (C14N), no HTML
  serialization and no DTD output; the internal subset of a document type is not
  written because it is not stored.
- Encodings other than UTF-8, UTF-16, UTF-32, ISO-8859-1 and US-ASCII are declined (see Encodings).
- No fuzzers (the parser's, the event parser's and the writer's are slice 1e) and
  no reference-parser differential (libxml2, expat): the scores above are against
  the specification's own suite only. Where those two disagree
  with each other about names, the Markup tree page has the measurement.
- No DTD processing: declarations are skipped, entities other than the
  predefined five are refused, and 340 not-well-formed documents whose fault is
  inside a DTD are accepted (listed, with their reasons).
- No XML 1.1, no canonicalization, no XPath. None is planned in this phase.

For `gtext_xml_parse()`, conversion happens before parsing, so an invalid sequence
anywhere in a non-UTF-8 document is reported in preference to an earlier syntax
error, and the converted copy is held in memory beside the original (up to twice the size for
ISO-8859-1, 1.5 times for UTF-16). `max_input_bytes` limits the original only.
For converted input, `offset` in an error counts bytes of the UTF-8 form, not of
the caller's buffer; `line` and `column` count characters.
