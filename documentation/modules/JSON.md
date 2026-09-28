@page json_module JSON

# JSON

This document describes the **full‑featured JSON parsing/writing library in C** implemented in the `text` library in the `ghoti.io` family. The implementation is **cross‑platform** and prioritizes **correctness** and **spec compliance** over simplicity. It is not dependency‑free: the library requires [ghoti.io-cutil](https://github.com/Ghoti-io/cutil), [ghoti.io-chron](https://github.com/Ghoti-io/chron) and [ghoti.io-unicode](https://github.com/Ghoti-io/unicode); the first two appear in *public* headers and unicode is a link dependency only, so a consumer needs their headers to compile against this one. See the [Dependencies](README.md#dependencies) section of the README.

---

@note This page documents the **API**. For the specification-level view -
which RFC clauses are implemented, what each malformed input returns, the
deviations from other parsers, and what evidence backs each claim - see
\ref format_json "JSON" under
\ref text_format_references "Format and specification references".

## 1. Overview

The JSON module provides comprehensive JSON processing capabilities with support for strict RFC 8259 / ECMA‑404 compliance, extended JSON modes, multiple parsing and writing models, and advanced features like JSON Pointer, Patch, and Schema validation.

### Core Capabilities

- **Strict JSON parsing** per RFC 8259 / ECMA‑404 with full grammar correctness
- **Extended JSON modes** (JSONC, trailing commas, non-finite numbers, relaxed
  strings, and the JSON5 dialect in full)
- **Three parsing models**: DOM, push streaming, and a pull reader
- **Two writing models**: DOM serialization and streaming writer
- **High-quality error diagnostics** with position information and context snippets
- **Round-trip correctness** including exact number preservation
- **JSON Pointer (RFC 6901)**, **JSONPath (RFC 9535)**, **JSON Patch (RFC 6902)**, and **JSON Merge Patch (RFC 7386)**
- **JSON Schema** for 2020-12, 2019-09, draft-07 and draft-06. A keyword this library cannot enforce fails compilation
- A pull reader, beside the DOM parser and the streaming parser

---

## 2. Parsing Modes

### 2.1 DOM Parsing

DOM parsing builds a complete in-memory tree structure of the JSON document. This mode is ideal when you need to:

- Navigate and query the JSON structure
- Modify the JSON structure
- Access values multiple times
- Work with the entire document at once

The DOM is allocated from an arena, making cleanup simple with a single `gtext_json_free()` call.

### 2.2 Streaming Parsing

Streaming parsing processes JSON incrementally, emitting events as values are encountered. This mode is ideal when you need to:

- Process large JSON documents with minimal memory usage
- Handle JSON from network streams or files
- Transform JSON on-the-fly without building a full DOM
- Process NDJSON (newline-delimited JSON) streams

The streaming parser accepts input in chunks and maintains state between calls, making it suitable for network or file I/O scenarios.

**Multi-Chunk Value Handling:**
The parser correctly handles values (strings, numbers) that span multiple chunks. When a value is incomplete at the end of a chunk, the parser preserves state and waits for more input. This ensures correct parsing regardless of how the input is split across chunks.

- **No chunk count limit**: Values can span 2, 3, 100, or more chunks
- **Total bytes limit**: Limited by `max_total_bytes` option (default: 64MB, configurable)
- **State preservation**: Incomplete values are buffered until completion
- **Examples**:
  - String: a quoted scalar split as `hello` / `world` across two chunks is
    reassembled into the single value `helloworld`
  - Number: `12345` (chunk 1) + `.678` (chunk 2) → correctly parses as `12345.678`
  - Escape sequences and Unicode escapes also work correctly across chunks

**Important:** Always call `gtext_json_stream_finish()` after feeding all input chunks. The last value may not be emitted until `finish()` is called, especially if it was incomplete at the end of the final chunk. This ensures all values are processed and the JSON structure is validated as complete.

---

## 3. Writing Modes

### 3.1 DOM Serialization

DOM serialization writes a complete JSON value tree to output. This is the simplest mode for converting a DOM back to JSON text.

### 3.2 Streaming Writer

The streaming writer allows you to construct JSON incrementally with structural enforcement. The writer maintains an internal stack to ensure valid JSON output (e.g., preventing values without keys inside objects).

---

## 4. Parse Options

The library provides extensive configuration options for parsing behavior:

### 4.1 Strictness / Extensions

- **`allow_comments`**: Enable JSONC mode (single-line `//` and multi-line `/* */` comments) — **Default: `false`**
- **`allow_trailing_commas`**: Allow trailing commas in arrays and objects — **Default: `false`**
- **`allow_nonfinite_numbers`**: Allow `NaN`, `Infinity`, and `-Infinity` as number values — **Default: `false`**
- **`allow_single_quotes`**: Allow single-quoted strings (relaxed mode) — **Default: `false`**
- **`allow_unescaped_controls`**: Allow unescaped control characters in strings (relaxed mode) — **Default: `false`**
- **`allow_hex_numbers`**: Allow a hexadecimal integer, `0x1F` (JSON5). `e` is
  one of its digits, so `0x1e2` is 482 — **Default: `false`**
- **`allow_leading_plus`**: Allow a leading `+` on a number (JSON5). With
  `allow_nonfinite_numbers`, also `+Infinity` and `+NaN` — **Default: `false`**
- **`allow_bare_decimal_point`**: Allow `.5` and `5.` (JSON5) — **Default:
  `false`**
- **`allow_ecma_escapes`**: Allow ECMAScript's string escapes (JSON5): `\xHH`,
  `\v`, `\0`, and any other character after a backslash meaning itself —
  **Default: `false`**
- **`allow_line_continuations`**: Allow a backslash before a line terminator
  inside a string to continue it (JSON5) — **Default: `false`**
- **`allow_ecma_whitespace`**: Allow ECMAScript's whitespace between tokens
  (JSON5): vertical tab, form feed, U+FEFF, General_Category Zs, U+2028 and
  U+2029 — **Default: `false`**
- **`allow_unquoted_keys`**: Allow an unquoted object name, `{a: 1}` (JSON5).
  The name is an ECMAScript IdentifierName, so ID_Start and ID_Continue decide
  it, `\uXXXX` escapes are allowed, and a reserved word is a name —
  **Default: `false`**

`gtext_json_parse_options_json5()` returns options with all eleven of the JSON5
options above set, and everything else as the default has it. See the
\ref format_json "JSON page" for what each one accepts and refuses.

### 4.2 Unicode / Input Handling

- **`allow_leading_bom`**: Allow UTF-8 BOM at the start of input — **Default: `true`**
- **`validate_utf8`**: Validate UTF-8 sequences in input — **Default: `true`**
- **`normalize_unicode`**: Apply NFC normalization to strings, object names
  included, so duplicate-name detection compares normalized names. Requires
  `validate_utf8`; turns off `in_situ_mode` for strings, because normalizing
  has to copy. — **Default: `false`**
- **`in_situ_mode`**: Zero-copy mode that references input buffer directly — **Default: `false`**

### 4.3 Duplicate Key Handling

- **`GTEXT_JSON_DUPKEY_ERROR`**: Fail parsing when duplicate keys are encountered — **Default**
- **`GTEXT_JSON_DUPKEY_FIRST_WINS`**: Use the first occurrence of a duplicate key
- **`GTEXT_JSON_DUPKEY_LAST_WINS`**: Use the last occurrence of a duplicate key
- **`GTEXT_JSON_DUPKEY_COLLECT`**: Store all values for duplicate keys in an array

### 4.4 Resource Limits

All limits use `0` to indicate library defaults:

- **`max_depth`**: Maximum nesting depth (default: 256)
- **`max_string_bytes`**: Maximum string size in bytes (default: 16MB)
- **`max_container_elems`**: Maximum array/object elements (default: 1M)
- **`max_total_bytes`**: Maximum total input size (default: 64MB)

### 4.5 Number Fidelity

- **`preserve_number_lexeme`**: Preserve original number text for exact round-trip — **Default: `true`**
- **`parse_int64`**: Detect and parse exact int64 representation — **Default: `true`**
- **`parse_uint64`**: Detect and parse exact uint64 representation — **Default: `true`**
- **`parse_double`**: Derive double representation when representable — **Default: `true`**

Numbers can be accessed in multiple representations simultaneously, allowing you to choose the most appropriate form for your use case.

---

## 5. Write Options

The library provides extensive configuration options for output formatting:

### 5.1 Formatting

- **`pretty`**: Pretty-print output with indentation — **Default: `false`**
- **`indent_spaces`**: Number of spaces per indent level — **Default: `2`**
- **`newline`**: Newline string — **Default: `"\n"`** (can use `"\r\n"`)
- **`trailing_newline`**: Add trailing newline at end of output — **Default: `false`**
- **`space_after_colon`**: Add space after `:` in objects — **Default: `false`**
- **`space_after_comma`**: Add space after `,` in arrays/objects — **Default: `false`**
- **`inline_array_threshold`**: Maximum elements for inline array formatting — **Default: `-1`** (always inline when not pretty)
- **`inline_object_threshold`**: Maximum pairs for inline object formatting — **Default: `-1`** (always inline when not pretty)

### 5.2 Escaping

- **`escape_solidus`**: Escape forward slash `/` — **Default: `false`**
- **`escape_unicode`**: Output `\uXXXX` for non-ASCII characters (canonical mode) — **Default: `false`**
- **`escape_all_non_ascii`**: Escape all non-ASCII characters (stricter) — **Default: `false`**

### 5.3 Canonical / Deterministic Output

- **`sort_object_keys`**: Sort object keys for stable, deterministic output — **Default: `false`**
- **`canonical_numbers`**: Normalize numeric lexemes (use with care) — **Default: `false`**

### 5.4 Floating-Point Formatting

- **`float_format`**: Formatting strategy:
  - `GTEXT_JSON_FLOAT_SHORTEST`: Shortest representation (default)
  - `GTEXT_JSON_FLOAT_FIXED`: Fixed-point notation
  - `GTEXT_JSON_FLOAT_SCIENTIFIC`: Scientific notation
- **`float_precision`**: Precision for fixed/scientific format (default: 6)

### 5.5 Extensions

- **`allow_nonfinite_numbers`**: Emit `NaN`/`Infinity` if node contains non-finite values — **Default: `false`**

---

## 6. Output Sinks

The writer supports multiple output destinations through a sink abstraction:

- **Growable buffer**: Dynamically-growing buffer for complete output
- **Fixed buffer**: Fixed-size buffer with truncation detection
- **Callback sink**: Custom write function for any destination (files, network, etc.)

---

## 7. DOM Operations

### 7.1 Value Access

The DOM provides type-safe accessors for all JSON value types:

- **Scalars**: `gtext_json_get_bool()`, `gtext_json_get_string()`
- **Numbers**: Multiple representations available (`get_i64()`, `get_u64()`, `get_double()`, `get_number_lexeme()`)
- **Arrays**: `gtext_json_array_size()`, `gtext_json_array_get()`
- **Objects**: `gtext_json_object_size()`, `gtext_json_object_get()`, `gtext_json_object_key()`, `gtext_json_object_value()`

### 7.2 Value Creation and Mutation

The DOM supports programmatic creation and modification:

- **Value constructors**: Create null, bool, number, string, array, and object values
- **Array operations**: `push()`, `set()`, `insert()`, `remove()`
- **Object operations**: `put()`, `remove()`

### 7.3 Utility Operations

- **Deep equality**: Compare two JSON values with configurable semantics (lexeme-based or numeric equivalence)
- **Deep clone**: Clone a value tree into a new arena
- **Object merge**: Merge two objects with configurable conflict policy (first-wins, last-wins, or error)

---

## 8. Streaming Parser Events

The streaming parser emits events for:

- **Value events**: `NULL`, `BOOL`, `NUMBER`, `STRING`
- **Structure events**: `ARRAY_BEGIN`, `ARRAY_END`, `OBJECT_BEGIN`, `OBJECT_END`
- **Key events**: `KEY` (for object keys)

Each event includes the relevant data (boolean value, string/number text, etc.) and maintains position information for error reporting.

---

## 9. JSON Pointer (RFC 6901)

JSON Pointer provides path-based access to nested JSON values using a string syntax like `/a/0/b`:

- Evaluate pointers against DOM trees
- Support for escape sequences (`~0` for `~`, `~1` for `/`)
- Mutable access for patch operations

---

## 10. JSON Patch (RFC 6902)

JSON Patch allows modifying JSON documents using a sequence of operations:

- **add**: Add a value at a path
- **remove**: Remove a value at a path
- **replace**: Replace a value at a path
- **move**: Move a value from one path to another
- **copy**: Copy a value from one path to another
- **test**: Test that a value at a path equals an expected value

Patches are represented as JSON arrays of operation objects and are applied atomically (all operations succeed or the patch fails).

---

## 11. JSON Merge Patch (RFC 7386)

JSON Merge Patch provides a simpler merge operation that recursively merges a patch document into a target document. This is distinct from JSON Patch and is useful for configuration updates.

---

## 12. JSON Schema Validation

The engine reads JSON Schema 2020-12, 2019-09, draft-07 and draft-06, each
with its own keyword set. `$schema` selects the dialect. A schema this
library cannot fully enforce is refused at compile time and names the
keyword. `pattern` and `patternProperties` run only when the caller supplies
a regular-expression engine. What is still open is in section 17.1. The format page is the
authority for which keyword does what.

---

## 13. Error Reporting

The library provides comprehensive error information:

- **Error codes**: Stable error codes for programmatic handling
- **Human-readable messages**: Descriptive error messages
- **Position information**: Byte offset, line number, and column number
- **Enhanced diagnostics** (optional):
  - Context snippet showing the error location
  - Caret positioning within the snippet
  - Expected vs actual token descriptions

Error context snippets are dynamically allocated and must be freed via `gtext_json_error_free()`.

---

## 14. Additional Features

### 14.1 In-Situ / Zero-Copy Parsing

An optional in-situ parsing mode allows the DOM to reference slices of the input buffer directly, avoiding copies for strings and number lexemes. This mode requires the input buffer to remain valid for the lifetime of the DOM.

### 14.2 Multiple Top-Level Value Parsing

The parser can parse a single JSON value and return the number of bytes consumed, allowing you to parse multiple values from the same buffer sequentially. This is useful for embedding JSON in larger syntaxes or custom protocols.

### 14.3 Round-Trip Correctness

When number lexeme preservation is enabled, the library guarantees exact round-trip: parse → write → parse results in identical number representations. This is critical for applications that need to preserve exact numeric values.

---

## 15. Design Philosophy

The library prioritizes **correctness over simplicity**:

1. **Strict by default**: Strict JSON compliance is the default; extensions are explicit opt-ins
2. **Exact number preservation**: Number lexemes are preserved by default for round-trip correctness
3. **Unicode correctness**: Proper UTF-8 handling and surrogate pair validation
4. **Configurable duplicate keys**: Duplicate key handling is configurable (default: error)
5. **Arena allocation**: DOM uses arena allocation for predictable ownership and fast cleanup

---

## 16. Getting Started

Include the umbrella header:

```c
#include <ghoti.io/text/json.h>
```

For fine-grained control, include specific headers:

```c
#include <ghoti.io/text/json/json_core.h>  // Core types and options
#include <ghoti.io/text/json/json_dom.h>   // DOM parsing and manipulation
#include <ghoti.io/text/json/json_stream.h> // Streaming parser
#include <ghoti.io/text/json/json_writer.h> // Writer
#include <ghoti.io/text/json/json_pointer.h> // JSON Pointer
#include <ghoti.io/text/json/json_path.h>    // JSONPath
#include <ghoti.io/text/json/json_patch.h>   // JSON Patch
#include <ghoti.io/text/json/json_schema.h>  // Schema validation
```

Comprehensive usage examples are provided in the `examples/` directory.

---

## 17. What remains

### 17.1 What the schema engine does not cover

Every standard keyword in every draft this engine reads is either enforced
or ignored for a reason the specification gives. What remains is one entry:

- **draft-04 and earlier** are refused by decision, not by omission - see below

Three things were on this list until the identifier pre-pass was given the
dialect, and they are worth recording because all three were invisible to the
2020-12 and 2019-09 suites and visible only to the older two:

- **`$ref` beside a sibling `$id`** leaves the base URI alone before 2019-09,
  because the `$ref` is the whole schema there and every keyword beside it is
  ignored. The compiler honoured that; the pre-pass did not, because it ran
  before any dialect was read, and it registered the `$id` anyway. Two
  assertions in each of draft-07 and draft-06 were answered *wrongly*
- **`$anchor` is 2019-09's keyword**, and registering one in a draft-07
  document answers a `$ref` the draft says finds nothing. The older spelling is
  an `$id` holding only a fragment
- **a JSON pointer that crosses into an embedded resource** was keeping the
  referring resource's base URI. `#/definitions/baz/definitions/bar`, where
  `baz` carries an `$id`, finds the right subschema, and a relative `$ref`
  inside it belongs to `baz`'s resource. Two assertions refused in each of the
  two older drafts, and the newer suites do not ask: their version of that case
  uses a pointer that stays inside one resource

A fourth was listed and was never true. draft-07's location-independent
identifier - an `$id` holding only a fragment - was described here as a name
this engine would not find, and the suite's four groups that use one had always
passed. Resolving `#name` as a base change produces `base#name`, which is the
same URI the anchor spelling registers, and a relative reference discards its
base's fragment (RFC 3986 section 5.2.2), so everything nested below resolved
against the same base either way. The rule is now written out as the draft
states it, which changes no answer and stops the agreement between two
spellings from resting on another function's fragment handling.

### `$schema` selects a draft

A `$ref` can leave the document and land in one written years earlier, and
that document is read as what it says it is. The dialect is scoped exactly as
the base URI is: it applies to the resource that declares it and everything
inside, and the resource outside is unaffected, so the same keyword can mean
different things either side of a boundary within one compile.

2020-12, 2019-09, draft-07 and draft-06 are read. What differs between them
and is honoured here:

- a keyword the draft did not have yet is an unknown member and is ignored -
  `prefixItems`, `$dynamicRef` and `$dynamicAnchor` before 2020-12;
  `unevaluatedItems`, `unevaluatedProperties`, `dependentSchemas`,
  `dependentRequired`, `minContains` and `maxContains` before 2019-09;
  `if`, `then` and `else` before draft-07
- before 2019-09, a schema object containing `$ref` **is** that reference:
  every other keyword beside it is ignored. 2019-09 made `$ref` an applicator
  like any other, so its siblings apply
- an array-valued `items` with `additionalItems`, which is how the older
  drafts spell what 2020-12 calls `prefixItems` and `items`, compiles to the
  same slots in every draft

draft-04 and earlier are **refused**, and the error names the draft. draft-04
spells `exclusiveMinimum` as a boolean that modifies `minimum`, and `$id` as
`id`; reading one of those as if it were draft-06 does not produce a wrong
keyword, it produces a wrong answer about the instance, which is exactly what
this engine refuses rather than guesses at.

A document that carries no `$schema` at all is read as
`GTEXT_JSON_Schema_Options::default_dialect`, or as 2020-12 when the caller
has not set one. `$schema` is optional and its absence does not make a
document dialect-free - it was written against something - so guessing the
current draft is right but is still a guess, and this is how a caller who
knows better says so. A `$schema` inside the document always wins, per
resource. A `default_dialect` naming a draft this library cannot read is
refused rather than replaced by the default.

Identifiers are resolved the same way in every draft: `$id`, `$anchor` and
`$defs` establish resources and names whatever the dialect says. draft-07 has
no `$anchor` and spells a location-independent identifier as an `$id` holding
only a fragment, and that spelling is not implemented - a draft-07 document
that uses one has a name this engine will not find.

### Measured, per draft

`make conformance-json-schema` scores against JSON-Schema-Test-Suite at the
commit pinned in `tools/conformance/JSON_SCHEMA_COMMIT`. `JSS_DRAFT` picks the
directory, and the runner tells the engine which dialect that directory is
written in - almost no schema in the suite carries a `$schema`, so an
implementation that is not told reads every file as its own default and is
scored on rules the draft predates.

`make conformance-json-schema-all` runs all four, each against the same floor,
and is what `conformance-all` calls. Until it existed the three older drafts
were measured by hand, so nothing failed when one of them was short - and two of
them were short by eight assertions each for as long as that was true. A score
nothing gates is a score nobody re-reads.

| Draft | required | optional | optional/format |
| --- | --- | --- | --- |
| 2020-12 | 1301 / 1301 | 162 / 162 | 866 / 866 |
| 2019-09 | 1261 / 1261 | 158 / 158 | 866 / 866 |
| draft-07 | 929 / 929 | 114 / 118 | 785 / 785 |
| draft-06 | 841 / 841 | 106 / 106 | 407 / 407 |

Every required assertion in every draft this engine reads is answered, and
answered correctly. draft-07 was 921 of 929 and draft-06 833 of 841: four
refused in each for a meta-schema that was not embedded, two answered wrongly
for `$ref` beside a sibling `$id`, and two refused for a pointer crossing into
an embedded resource.

The one thing left outside the required suites is draft-07's optional
`content.json`, four assertions that want `contentMediaType` and
`contentEncoding` enforced. They are annotations in 2020-12 and this engine
ignores them in every draft, which is the same answer in both directions rather
than a gap that moves with the dialect.
`pattern` and `patternProperties` are measured through `ghoti.io-regex`, which
the runner links when it is installed and says so when it does not.

`$vocabulary` is implemented. A metaschema named by `$schema` and reachable
through the resolver says which vocabularies a schema written against it
uses, and a keyword from a vocabulary not in use is not a keyword - it is an
unknown member, and unknown members are ignored. A vocabulary declared
required that this engine does not have is refused, which is the
specification's rule and the honest one: the metaschema has said the schema
cannot be understood without it. Declaring the format-assertion vocabulary
turns `format` into an assertion, which is the mechanism the specification
provides for that.

When `$schema` names a metaschema neither the resolver nor the embedded set
can supply, the standard dialect is assumed rather than the schema refused.

### The published metaschemas are embedded

The 2020-12 dialect describes itself, so "is this a valid schema?" is a
question written in JSON Schema: `{"$ref":
"https://json-schema.org/draft/2020-12/schema"}` applied to the schema being
asked about. All nine published documents - the root and the eight under
`.../2020-12/meta/` - ship inside this library, so that reference resolves
with no resolver configured. So do 2019-09's seven, and the single document
each that draft-07 and draft-06 publish: eighteen in all, which is every
dialect this engine reads. `$vocabulary` arrived in 2019-09, so the two older
drafts have nothing to split a root document into - the keyword set is the
draft.

Neither older document uses `pattern` in a schema position, so unlike the
newer two they resolve with no regular-expression provider either. `pattern`
appears in them as a keyword being *described*, and what makes
`patternProperties`' keys regular expressions is spelled `"format": "regex"`,
which is an annotation.

The bytes are held one source line at a time and joined when a document is
used, rather than as one string literal each. C99 guarantees only 4,095
characters in a string literal after concatenation and draft-07's meta-schema
is 4,979 bytes; every compiler in use here accepts more, and the alternative
was a pedantic diagnostic switched off in the one file where somebody else's
document decides whether the limit is met.

This is the opposite of what the IDNA tables do with the Unicode Character
Database, and the difference is the point. The UCD versions: 17.0.0
supersedes 16.0.0, and a committed copy would be a stale copy of somebody
else's data, so only the *derivation* is committed and the data is fetched.
These URIs do not version. The whole reference model of 2020-12 rests on each
of them naming one fixed document forever, so there is nothing to fall behind
- and the alternative to embedding them is a validator that opens a
connection in the middle of a compile, to a URI it read out of the document
it was handed.

A resolver, when there is one, is asked first and wins. A caller serving one
of those URIs themselves - a mirror, a stricter variant - is not overruled by
a copy they never asked for.

The meta-schemas constrain `$id` and `$anchor` with `pattern`, so a `$ref`
that reaches them needs a regular-expression provider too. Without one the
compile is refused, for the same reason `pattern` is refused anywhere else.

The bytes are the published ones, verbatim. `tools/metaschema/fetch.sh`
retrieves them into the ignored `third_party/`, `tools/metaschema/gen_metaschema.py`
turns them into the committed `src/json/metaschema/metaschema_docs.c`, and
`make check-metaschema` regenerates and diffs. That gate is also the content
check: because nothing is reformatted, any difference at all is a difference
from what json-schema.org publishes.

`format` and the `content*` family are no longer on that list. All four are
annotations in 2020-12, so a validator that ignores them is conformant and
one that refuses them - as this did - is not. The three content keywords are
ignored. `format` is ignored by default and enforced when
`GTEXT_JSON_Schema_Options::format` is set to `GTEXT_JSON_FORMAT_ASSERT`,
which checks `date-time`, `date`, `time` and `duration` through
ghoti.io-chron, `regex` through the caller's regular-expression provider, and
the address, name, mailbox, URI and pointer formats here. Under that policy a
name in the vocabulary it cannot check is refused rather than ignored, which
is now only `regex` with no provider.

`hostname` and `idn-hostname` are IDNA2008 - RFC 5890 to 5893 - and an
`xn--` label is decoded and checked as the U-label it encodes, including the
round trip RFC 5891 section 4.4 requires. 2020-12 section 7.3.3 defines
`hostname` to include Punycode-produced names, so the LDH rule alone is not
the keyword.

`idn-hostname` additionally runs UTS #46's mapping and normalisation step
before any of that, in its nontransitional form:

- fullwidth and other compatibility forms are folded to their ASCII
  counterparts, so `１２３` is the name `123` and `ｘｎ--nxasmq6b` is an
  A-label that then has to decode;
- ignorable characters - a zero-width space, a soft hyphen, a variation
  selector - are removed, before the length is measured rather than after;
- the result is put into Normalization Form C, so a name spelled with a
  combining acute is the same name as one spelled with the precomposed
  character. An `xn--` label is *refused* if it decodes to something that is
  not already NFC, rather than normalised, because an A-label is the spelling
  of one exact U-label;
- the four deviation characters - sharp s, final sigma and the two zero-width
  joiners - are left alone, which is what every current browser does and what
  keeps `faß.example` from silently being `fass.example`.

The three other full stops UTS #46 treats as label separators are separators
because the mapping turns all of them into FULL STOP, not because they are
listed anywhere in this library.

The two specifications stay layered rather than merged: UTS #46 says what a
name *becomes*, and RFC 5892 still says what is valid, because 2020-12
section 7.3.4.3 defines the format by RFC 5890. That layering is also why the
two data sets may be different Unicode versions without the answer depending
on which - only the characters the mapping table *changes* are taken from it.

### The tables, and what checks them

Two generated files under `src/idna/tables/`, both committed so that a build
needs neither the network nor Python:

- `idna_tables.c`, the RFC 5892 derived property, from
  `tools/idna/gen_tables.py`;
- `uts46_tables.c`, the characters UTS #46's mapping step changes, from
  `tools/idna/gen_uts46.py`.

There were four. `nfc_tables.c` - the combining classes, canonical
decompositions and composition pairs - and the narrow Script, Joining_Type,
Bidi_Class and virama tables inside `idna_tables.c` were about 2,600 lines of
UCD data that [ghoti.io-unicode](https://github.com/Ghoti-io/unicode) now holds
for the whole suite. What is still generated here is what Unicode does not
define: RFC 5892's derived property, and UTS #46's mapping table, which is not
part of the UCD and versions on its own schedule.

The UCD version is pinned in `tools/idna/UCD_VERSION` and the mapping table's
in `tools/idna/IDNA_MAPPING_VERSION`. The mapping table is not part of the UCD,
so the two stay separate pins even when they name the same release. Both are
17.0.0.

Four gates:

- `make check-idna-tables` regenerates and diffs, so a committed table cannot
  drift from the generator that is supposed to produce it;
- `make check-idna-oracle` compares the derived property against python-idna's
  - a different author's reading of the same RFC - and this library's reading
  of the mapping table against python-idna's, *on the version python-idna was
  built from*, so that a genuine change between table versions is not reported
  as a finding;
- `make check-nfc-oracle` compares the normalisation this library reaches
  through `ghoti.io-unicode` against CPython's, over three and a half million
  sequences: every codepoint alone, every starter-and-mark pair,
  starter-and-two-marks across the combining classes, and Hangul in every
  combination. A wrong normaliser is right about almost every string, which is
  exactly why it needs an oracle rather than a test suite. `unicode` runs its
  own, larger oracle; this one asks the question through the call path `text`
  actually uses, which is the part a migration can break;
- `make check-ucd-pin` fails if the `unicode` this build linked was generated
  from a different UCD version than `tools/idna/UCD_VERSION` names. Two
  versions in one library would mean JSON5 names and IDNA validity disagreeing
  about which characters exist.

`pattern` and `patternProperties` are implemented, but only against a
regular-expression engine the caller supplies through
`GTEXT_JSON_Schema_Options::regex`; this library has none of its own. Without
a provider they are refused like the keywords above. See
\ref format_json "the JSON format page" for the contract a provider has to
meet.

### 17.2 JSONPath

JSONPath (RFC 9535) is implemented, including the filter selector and
`match()` and `search()`. Those two compile an I-Regexp with ghoti.io-regex.
`make conformance-jsonpath` scores the compliance suite. The format page is
the authority for the rest.

---

## 18. Security Considerations

The JSON library implements comprehensive defensive programming practices to ensure memory safety, prevent undefined behavior, and handle malicious or malformed input gracefully.

### 18.1 Integer Overflow Protection

All arithmetic operations throughout the library are protected against integer overflow and underflow:

- **Addition overflow checks**: All additions use overflow-safe checks (e.g., `if (a > SIZE_MAX - b)`) before performing operations
- **Multiplication overflow checks**: Multiplications are validated to prevent overflow (e.g., `if (a > SIZE_MAX / b)`)
- **Subtraction underflow checks**: Subtractions are validated to prevent underflow
- **Position tracking**: Line and column numbers use overflow-safe increment operations with INT_MAX limits

**Shared Utilities:**
The library uses shared utility functions from `json_utils.c` for consistent overflow protection:
- `json_check_add_overflow()` - checks if addition would overflow
- `json_check_mul_overflow()` - checks if multiplication would overflow
- `json_check_sub_underflow()` - checks if subtraction would underflow
- `json_check_int_overflow()` - checks integer overflow for position tracking

**Examples:**
- Buffer size calculations are validated before allocation
- String length calculations are checked for overflow
- Container element counts are validated against limits
- Total bytes consumed are tracked with overflow protection

### 18.2 Bounds Checking

All array and buffer accesses are protected with defensive bounds checking:

- **Array access**: All array element accesses validate indices against array size before access
- **Buffer access**: All buffer operations validate offsets against buffer size
- **Pointer arithmetic**: All pointer arithmetic is validated to ensure pointers remain within valid ranges
- **String operations**: String operations validate lengths and offsets before access

**Examples:**
- `gtext_json_array_get()` validates index against array size before access
- `gtext_json_object_key()` validates index against object size before access
- Lexer and parser validate buffer offsets before reading
- Stream operations validate stack indices before access

### 18.3 NULL Pointer Handling

All functions implement comprehensive NULL pointer checks:

- **Public API functions**: All public API functions validate required parameters for NULL
- **Internal functions**: Internal functions validate pointers before dereferencing
- **Error handling**: NULL pointer errors are handled gracefully with appropriate error codes
- **Resource cleanup**: Cleanup functions handle NULL gracefully (no-op for NULL pointers)

**Examples:**
- `gtext_json_parse()` returns NULL and sets error if input buffer is NULL
- `gtext_json_stream_new()` returns NULL if callback is NULL
- `gtext_json_stream_free()` safely handles NULL (no-op)
- All accessor functions validate value pointers before access

### 18.4 Input Validation

Comprehensive input validation is performed at all API boundaries:

- **Input size validation**: Input sizes are validated to prevent obvious overflow (SIZE_MAX/2 limit)
- **Resource limits**: All resource limits (string length, container size, total bytes) are enforced
- **State validation**: Stream state is validated before operations
- **Option validation**: Parse options are validated (limits checked via `json_get_limit()`)

**Examples:**
- `gtext_json_parse()` validates input size before parsing
- `gtext_json_stream_feed()` validates input size and stream state
- String length limits are enforced during parsing
- Container element limits are enforced during parsing

### 18.5 Error Handling

The library provides comprehensive error handling with detailed diagnostics:

- **Error codes**: Stable error codes for programmatic error handling
- **Error context**: Error structures include position information (offset, line, column)
- **Context snippets**: Error context snippets show the error location in input
- **Resource cleanup**: All error paths properly clean up resources (no memory leaks)
- **Error state**: Streams enter error state on failure (subsequent operations return error)

**Error Reporting:**
- Error structures include detailed position information
- Context snippets are generated for better diagnostics
- Error messages are descriptive and actionable
- Error cleanup is automatic (no resource leaks on error)

### 18.6 Resource Management

The library implements safe resource management patterns:

- **Arena allocation**: DOM values are allocated from an arena (single free operation)
- **Automatic cleanup**: Error paths automatically clean up allocated resources
- **Buffer management**: Buffers are properly managed with overflow-safe growth
- **Context snippet cleanup**: Error context snippets are properly freed

**Examples:**
- DOM values are freed via single `gtext_json_free()` call
- Stream resources are freed via `gtext_json_stream_free()`
- Error context snippets are freed via `gtext_json_error_free()`
- All cleanup functions handle NULL gracefully

### 18.7 Testing and Validation

The library includes comprehensive test coverage for security-critical scenarios:

- **Overflow/underflow tests**: 6 test cases covering overflow scenarios
- **NULL pointer tests**: 8 test cases covering NULL pointer handling
- **Bounds violation tests**: 4 test cases covering bounds checking
- **Invalid state tests**: 5 test cases covering state machine validation
- **Memory safety**: All tests pass Valgrind validation (no memory leaks)

**Test Coverage:**
- All overflow protection code paths are tested
- All NULL pointer checks are tested
- All bounds checking code paths are tested
- All error handling paths are tested
- Memory safety is validated with Valgrind

### 18.8 Thread Safety

No JSON object is thread-safe. A `GTEXT_JSON_Value` tree, a parser, a stream
and a writer each belong to one thread at a time; two that were created
separately share nothing and may be used concurrently. A DOM that no thread is
modifying may be read from several at once.

If you pass an allocator through `GTEXT_JSON_Parse_Options::allocator`, that
allocator must itself be thread-safe when the values built from it are used
from more than one thread - the library adds no locking.

The full rule, the reasoning, and the one platform caveat about locale and
number formatting are in the \ref core_module "Core module page", section 7.

### 18.9 Best Practices for Users

When using the JSON library, follow these security best practices:

1. **Validate input sizes**: Check input sizes before parsing (library enforces SIZE_MAX/2 limit)
2. **Handle errors**: Always check return values and error structures
3. **Free resources**: Always free allocated values and error structures
4. **Set appropriate limits**: Configure resource limits based on your use case
5. **Validate UTF-8**: Enable UTF-8 validation for untrusted input
6. **Use streaming parser**: For large inputs, use streaming parser to limit memory usage

**Example:**
```c
// Validate input size before parsing
if (input_len > SIZE_MAX / 2) {
    // Handle error - input too large
    return;
}

// Parse with error handling
gtext_json_error err = {0};
gtext_json_value* value = gtext_json_parse(input, input_len, NULL, &err);
if (value == NULL) {
    // Handle error - check err.code and err.message
    gtext_json_error_free(&err);
    return;
}

// Use value...

// Free resources
gtext_json_free(value);
```

---
