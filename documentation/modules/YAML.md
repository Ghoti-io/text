@page yaml_module YAML

# YAML

This document describes the **YAML parsing library in C** implemented in the `text` library in the `ghoti.io` family. The implementation is **cross-platform** and prioritizes **correctness** and **memory safety** over simplicity. It is not dependency-free: the library requires [ghoti.io-cutil](https://github.com/Ghoti-io/cutil) for `GCU_Allocator`, [ghoti.io-chron](https://github.com/Ghoti-io/chron) for `!!timestamp` and [ghoti.io-unicode](https://github.com/Ghoti-io/unicode) for the UCD; the first two appear in *public* headers - `GCHRON_YamlValue` is what a timestamp node holds. See the [Dependencies](README.md#dependencies) section of the README.

---

## 1. Overview

The YAML module provides YAML 1.2.2 processing capabilities with support for streaming parsing, anchors/aliases, comprehensive error handling, and writer/serialization.

### Current status

Alpha. The API may change before 1.0.

**Implemented:** the streaming parser with event callbacks, a pull-model
reader, a DOM parser with accessors, mutation and cloning, and a writer for
both DOM and streaming events. All five scalar styles, block and flow
collections, anchors and aliases with cycle detection, merge keys (`<<`),
multi-document streams, `%YAML` and `%TAG` directives, schema-based implicit
typing (Failsafe, JSON, Core), YAML 1.1 compatibility mode, `!!binary`,
the `!!timestamp`/`!!set`/`!!omap`/`!!pairs` types with validation, custom
application tags, UTF-8/16/32 input with BOM detection, limit enforcement,
and conversion to JSON.

**Measured:** `make conformance` runs the
[YAML test suite](https://github.com/yaml/yaml-test-suite) against this
parser, at the commit pinned in `tools/conformance/YAML_SUITE_COMMIT`. Of the
suite's 406 cases it checks 395, and all 395 pass. The eleven left over carry
no expectation, or one the harness cannot decode. The same harness scores
js-yaml at 82.0% and PyYAML at 77.3% on the value cases. Those two are scored
through the JSON interface, so their denominator is the value cases, not all
395. Shapes the suite never contains are in
`tests/data/yaml/spec-1.2.2.corpus`. See
\ref format_yaml "the YAML format page".

An alias may carry no property of its own. A property on the line above an
alias belongs to the node that line opens. `&a` on its own line over `: 1`
is refused: accepting it would move the anchor onto the value.

There are no benchmarks. The suite runs under valgrind and under ASan/UBSan,
and `tests/fuzz/fuzz_yaml.cpp` parses and walks.

For the specification-level detail - which clauses are implemented, the
deviations, and what evidence backs each claim - see
\ref format_yaml "YAML" under
\ref text_format_references "Format and specification references".

### Core Capabilities

- **Streaming parsing** with event callbacks for memory-efficient processing
- **Full UTF-8 support** with validation and proper encoding handling
- **Anchor/alias resolution** with cycle detection and expansion limits
- **Comprehensive error diagnostics** with position information
- **Configurable limits** for depth, bytes, and alias expansion
- **Chunked input support** for network or file I/O scenarios
- **Memory safety** with zero memory leaks (valgrind-verified)
---

## 2. Parsing Models
### 2.1 Streaming Parsing (Implemented)

Streaming parsing processes YAML incrementally, emitting events as nodes are encountered. This mode is ideal when you need to:

- Process large YAML documents with minimal memory usage
- Handle YAML from network streams or files
- Transform YAML on-the-fly without building a full DOM
- Process multi-document streams

The streaming parser accepts input in chunks and maintains state between calls, making it suitable for network or file I/O scenarios.

**Basic Usage:**

```c
#include <ghoti.io/text/yaml/yaml_stream.h>
#include <ghoti.io/text/yaml/yaml_core.h>

// Define event callback
GTEXT_YAML_Status my_callback(GTEXT_YAML_Stream *s, const void *event, void *user) {
    // Process event
    // event contains scalar values, sequence/mapping starts/ends, etc.
    return GTEXT_YAML_OK;
}

// Create stream parser
GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
GTEXT_YAML_Stream *stream = gtext_yaml_stream_new(&opts, my_callback, user_data);

// Feed input (can be called multiple times with chunks)
const char *yaml = "key: value\nlist: [1, 2, 3]\n";
GTEXT_YAML_Status status = gtext_yaml_stream_feed(stream, yaml, strlen(yaml));
if (status != GTEXT_YAML_OK) {
    // Handle error
}

// Finish parsing (validates completeness)
status = gtext_yaml_stream_finish(stream);

// Cleanup
gtext_yaml_stream_free(stream);
```

The parser correctly handles values (strings, numbers, collections) that span multiple chunks. When a value is incomplete at the end of a chunk, the parser preserves state and waits for more input.

- **No chunk count limit**: Values can span 2, 3, 100, or more chunks
- **Total bytes limit**: Limited by `max_total_bytes` option (default: 64MB)
- **State preservation**: Incomplete values are buffered until completion
- **Examples**:
  - String: a quoted scalar split as `hello` / ` world` across two chunks is
    reassembled into the single value `hello world`
  - Number: `12345` (chunk 1) + `.678` (chunk 2) → correctly parses as `12345.678`
  - Collections spanning chunks work correctly with proper state tracking

**Important:** Always call `gtext_yaml_stream_finish()` after feeding all input chunks. The last value may not be emitted until `finish()` is called, especially if it was incomplete at the end of the final chunk.

### 2.2 DOM Parsing (Implemented)

DOM parsing builds a complete in-memory tree structure of the YAML document. This mode is ideal when you need to:

- Navigate and query the YAML structure
- Modify the YAML structure
- Access values multiple times
- Work with the entire document at once

**DOM Accessors (selected):**
- `gtext_yaml_node_type()`
- `gtext_yaml_node_as_string()`, `gtext_yaml_node_as_bool()`, `gtext_yaml_node_as_int()`, `gtext_yaml_node_as_float()`
- `gtext_yaml_node_is_null()`, `gtext_yaml_node_as_timestamp()`
- `gtext_yaml_node_timestamp_value()` - the `!!timestamp` as the
  `GCHRON_YamlValue` `chron` read it, which is the lossless accessor;
  `gtext_yaml_node_timestamp_is_leap_second()` reports a `:60`
- `gtext_yaml_sequence_length()`, `gtext_yaml_sequence_get()`
- `gtext_yaml_mapping_size()`, `gtext_yaml_mapping_get()`

**Example: Clone a subtree into a new document**

```c
#include <ghoti.io/text/yaml.h>

const char *yaml = "root: {items: [one, two]}\n";
GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), NULL, NULL);
if (!doc) {
  // Handle parse error
}

const GTEXT_YAML_Node *root = gtext_yaml_document_root(doc);
const GTEXT_YAML_Node *items = gtext_yaml_mapping_get(root, "root");

GTEXT_YAML_Document *clone_doc = gtext_yaml_document_new(NULL, NULL);
GTEXT_YAML_Node *clone = gtext_yaml_node_clone(clone_doc, items);
gtext_yaml_document_set_root(clone_doc, clone);

gtext_yaml_free(doc);
gtext_yaml_free(clone_doc);
```

### 2.3 Composed Node Events (Implemented)

`gtext_yaml_document_walk()` and `gtext_yaml_stream_walk()` report a parsed
document as one event per node, in written order, with each node's anchor,
tag and style attached to the node it belongs to. It is the composed view: a
consumer that wants to know what the document *is* wants these, while one
that wants to know what the input *said* wants the streaming parser's
`GTEXT_YAML_Event`, which is shaped like the input and carries indicators,
comments, and properties whose node is not yet known.

Aliases are events of their own and are never followed, so a document with a
cycle in it walks in finite time. The two pieces of provenance the walk needs
are readable from the DOM as well: `gtext_yaml_node_flow_style()` says
whether a collection was written `[like, this]` or as a block, and
`gtext_yaml_document_has_explicit_start()` / `_end()` say whether `---` and
`...` were written or merely implied.

```c
static GTEXT_YAML_Status on_event(const GTEXT_YAML_Node_Event *ev, void *user) {
  (void)user;
  switch (ev->type) {
  case GTEXT_YAML_NODE_EVENT_SCALAR:
    printf("scalar %.*s\n", (int)ev->value_len, ev->value);
    break;
  case GTEXT_YAML_NODE_EVENT_ALIAS:
    printf("alias *%s\n", ev->value);
    break;
  default:
    break;
  }
  return GTEXT_YAML_OK;
}

size_t count = 0;
GTEXT_YAML_Document **docs = gtext_yaml_parse_all(input, len, &count, NULL, NULL);
gtext_yaml_stream_walk(docs, count, on_event, NULL);
```

`tools/conformance/yaml_event_suite.c` is the worked example: it turns these
events into yaml-test-suite's event notation, which is what lets
`make conformance` check the tenth of the corpus that asserts an event stream
rather than a value.

### 2.4 Writing and Formatting (Implemented)

The writer serializes a DOM to a sink and exposes formatting options such as
indentation, scalar styles, flow vs. block collections, and line-width aware
folding.

```c
GTEXT_YAML_Sink sink;
gtext_yaml_sink_buffer(&sink);

GTEXT_YAML_Write_Options opts = gtext_yaml_write_options_default();
opts.pretty = true;
opts.indent_spaces = 4;
opts.scalar_style = GTEXT_YAML_SCALAR_STYLE_FOLDED;
opts.line_width = 12;

gtext_yaml_write_document(doc, &sink, &opts);
printf("%s", gtext_yaml_sink_buffer_data(&sink));
gtext_yaml_sink_buffer_free(&sink);
```

---

## 3. Parse Options

The library provides extensive configuration options for parsing behavior:

### 3.1 Resource Limits

All three size limits use `0` to indicate library defaults:

- **`max_depth`**: Maximum nesting depth — **Default: 256**
- **`max_total_bytes`**: Maximum total input size — **Default: 64MB**
- **`max_alias_expansion`**: Maximum alias expansion count — **Default: 10,000**

`max_depth` bounds the parser, the DOM writer and `gtext_yaml_node_clone()`.
The last two matter because the DOM constructors do not consult it — they have
no parent pointers, so asking a node how deep it sits would cost a walk on
every append — and a document built through the API can therefore nest as far
as memory allows.

Parsing is linear in nesting depth. The resolver, the DOM writer and
`gtext_yaml_node_clone()` keep their stacks on the heap, so depth costs
memory rather than a C stack frame. `SIZE_MAX` removes the limit.
`gtext_yaml_parse_options_effective()` resolves a zero to the default, and
every entry point goes through it.

**A zeroed struct is still not the defaults**, and cannot be: every `bool` in
it zeroes to `false`, which is a setting rather than an absence, so
`validate_utf8`, `resolve_tags`, `allow_aliases` and `allow_merge_keys` all
come out off. Start from `gtext_yaml_parse_options_default()`, or
`gtext_yaml_parse_options_safe()` for untrusted input, and change what you
mean to change. See \ref example_yaml_security for the longer version.

**Example: Setting strict limits for untrusted input**

```c
GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();

// Max 10 MB input
opts.max_total_bytes = 10 * 1024 * 1024;

// Max depth of 50 levels
opts.max_depth = 50;

// Max 100 alias expansions (prevents exponential expansion attacks)
opts.max_alias_expansion = 100;

GTEXT_YAML_Stream *stream = gtext_yaml_stream_new(&opts, callback, user_data);
```

### 3.2 Duplicate Key Handling

- **`GTEXT_YAML_DUPKEY_ERROR`**: Fail parsing when duplicate keys are encountered — **Default** (spec-compliant)
- **`GTEXT_YAML_DUPKEY_FIRST_WINS`**: Use the first occurrence of a duplicate key
- **`GTEXT_YAML_DUPKEY_LAST_WINS`**: Use the last occurrence of a duplicate key
- **`GTEXT_YAML_DUPKEY_KEEP_ALL`**: Keep both pairs. The only mode that keeps
  the document - the two above remove a pair and the default removes the
  parse - so it is the one for a tool that has to see what was written: a
  linter, a formatter, or the event walk that scores this parser against
  yaml-test-suite. `gtext_yaml_mapping_get()` then answers with the first of
  the duplicates, and the JSON fast path is turned off, since a JSON DOM has
  no way to hold two pairs with the same key.

```c
GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
opts.dupkey_mode = GTEXT_YAML_DUPKEY_LAST_WINS;  // Allow duplicate keys, last wins
```

### 3.3 Schema and Tag Resolution

The resolver supports schema-based implicit typing and explicit tag handling.

- **`schema`**: Selects the implicit typing rules.
  - `GTEXT_YAML_SCHEMA_FAILSAFE`: All scalars remain strings.
  - `GTEXT_YAML_SCHEMA_JSON`: JSON-compatible null/bool/int/float/string.
  - `GTEXT_YAML_SCHEMA_CORE`: YAML core schema (includes `.nan`, `.inf`, etc.).
- **`resolve_tags`**: When disabled, preserves explicit tags and keeps scalars as strings.
- **`yaml_1_1`**: When enabled, apply YAML 1.1 implicit typing (yes/no/on/off, 0755 octal, sexagesimal). This is also enabled automatically when a `%YAML 1.1` directive is present.

```c
GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
opts.schema = GTEXT_YAML_SCHEMA_JSON;
opts.resolve_tags = true;
```

### 3.4 Safe Mode

Safe mode configures parsing for untrusted input by disabling potentially
dangerous or surprising features and restricting mapping keys to strings.

Safe mode enforces:
- Aliases disabled
- Merge keys disabled
- Non-standard tags disabled (only standard YAML tags allowed)
- Complex keys disabled (mapping keys must be scalars)
- String-only keys (mapping keys must be strings)

Safe mode helps prevent common YAML attack patterns:
- Exponential alias expansion ("billion laughs") and alias cycles
- Merge key abuse that injects unexpected keys or overrides config
- Non-standard tag constructors that can trigger unsafe behaviors
- Complex key tricks that hide duplicate or conflicting keys

Use the convenience constructor or wrapper:

```c
GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_safe();
GTEXT_YAML_Document *doc = gtext_yaml_parse_safe(yaml, strlen(yaml), &err);
```

For manual control, the relevant toggles are:
- `allow_aliases`
- `allow_merge_keys`
- `allow_complex_keys`
- `require_string_keys`
- `allow_nonstandard_tags` - refuses application-defined tags (`!point`, or a
  global tag under your own prefix). It does not govern the
  `tag:yaml.org,2002:` namespace, where an undefined tag such as `!!bogus` is
  refused whatever the options say.
- `enable_custom_tags`

### 3.5 JSON Fast Path

JSON is a valid subset of YAML. When the input begins with `{` or `[` and
appears JSON-compatible, the parser attempts a JSON fast path for improved
performance. If JSON parsing fails, it falls back to full YAML parsing.

You can force JSON-only parsing with:

```c
GTEXT_YAML_Document *doc = gtext_yaml_parse_json(json, strlen(json), NULL, &err);
```

---

## 4. Error Handling

The YAML parser provides detailed error information when parsing fails.

### 4.1 Error Codes

| Code | Meaning |
|------|---------|
| `GTEXT_YAML_OK` | Success |
| `GTEXT_YAML_E_INVALID` | Generic parse/validation error |
| `GTEXT_YAML_E_OOM` | Out of memory |
| `GTEXT_YAML_E_LIMIT` | A configured limit was exceeded |
| `GTEXT_YAML_E_DEPTH` | Maximum nesting depth exceeded |
| `GTEXT_YAML_E_INCOMPLETE` | More input required to complete parsing |
| `GTEXT_YAML_E_BAD_TOKEN` | Unexpected token encountered |
| `GTEXT_YAML_E_BAD_ESCAPE` | Invalid escape sequence in quoted scalar |
| `GTEXT_YAML_E_DUPKEY` | Duplicate mapping key |
| `GTEXT_YAML_E_WRITE` | Sink/write error (writer only) |
| `GTEXT_YAML_E_STATE` | Operation not valid in current state |

### 4.2 Error Context

When parsing fails, the error includes position information:

```c
GTEXT_YAML_Status status = gtext_yaml_stream_feed(stream, input, len);
if (status != GTEXT_YAML_OK) {
    // Error occurred - get details
    printf("YAML parsing failed with code %d\n", status);
    
    // Future: retrieve detailed error with line/column information
    // GTEXT_YAML_Error *err = gtext_yaml_stream_get_error(stream);
    // printf("Error at line %d, column %d: %s\n", err->line, err->col, err->message);
}
```

### 4.3 Warnings

Non-fatal issues are reported through an optional warning callback. Warnings
do not stop parsing unless `warnings_as_errors` is enabled.

```c
static void on_warning(const GTEXT_YAML_Warning *warning, void *user) {
  (void)user;
  printf("warning: %s\n", warning->message);
}

GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
opts.warning_callback = on_warning;
opts.warning_user_data = NULL;
opts.warnings_as_errors = false;

GTEXT_YAML_Document *doc = gtext_yaml_parse(yaml, strlen(yaml), &opts, NULL);
```

---

### 4.4 Partial Parsing and Recovery

For IDE and validation scenarios, you can parse with recovery and collect
multiple errors while still receiving a best-effort DOM:

```c
GTEXT_YAML_Document *doc = NULL;
GTEXT_YAML_Error *errors = NULL;
size_t error_count = 0;

GTEXT_YAML_Status status = gtext_yaml_parse_partial(
  input,
  len,
  NULL,
  &doc,
  &errors,
  &error_count,
  NULL
);
```

Recovery behavior and limitations:
- Errors are collected in order; the caller owns the array and must free it.
- Recovery resumes at the next top-level node (column 1) or document boundary.
- If recovery produces multiple top-level nodes, the root is a sequence.
- Error markers are emitted as scalar nodes containing an error message.


## 5. Scalar Styles

YAML supports multiple scalar styles, all of which are fully supported:

### 5.1 Plain Scalars

```yaml
key: value
number: 123
boolean: true
```

Plain scalars are unquoted strings that follow YAML's flow syntax rules. They cannot contain certain special characters without quoting.

### 5.2 Single-Quoted Scalars

```yaml
key: 'value with spaces'
path: 'C:\Program Files\'
literal: 'It''s escaped with double quotes'
```

Single-quoted scalars preserve literal characters. Single quotes are escaped by doubling them (`''`).

### 5.3 Double-Quoted Scalars

```yaml
key: "value with\nnewline"
unicode: "Hello \u0041\u0042\u0043"
special: "Tab:\t Quote:\" Backslash:\\"
```

Double-quoted scalars support escape sequences:
- `\\` - Backslash
- `\` followed by a double quote - Quote
- `\n` - Newline
- `\t` - Tab
- `\r` - Carriage return
- `\uXXXX` - Unicode code point (4 hex digits)
- `\UXXXXXXXX` - Unicode code point (8 hex digits)

All of 5.7 is implemented: `\0`, `\a`, `\b`, `\t`, `\n`, `\v`, `\f`, `\r`,
`\e`, a space, `"`, `/`, `\\`, `\N`, `\_`, `\L`, `\P`, `\xNN`, `\uNNNN` and
`\UNNNNNNNN`. All three numeric escapes name a *character* and are encoded as
one - `"\x92"` is U+0092, not a lone byte 0x92 - and each is an error without
its hex digits. Anything else after a backslash is a malformed document
rather than a literal.

### 5.4 Literal Scalars (`|`)

```yaml
script: |
  #!/bin/bash
  echo "Line 1"
  echo "Line 2"
  echo "Line 3"
```

Literal scalars preserve newlines and indentation. Each line break is preserved as-is.

**Line break normalization:** Input line endings are normalized to `\n` during parsing.
Use `GTEXT_YAML_Write_Options.newline` to control output line endings (LF/CRLF/CR).

### 5.5 Folded Scalars (`>`)

```yaml
description: >
  This is a long paragraph
  that will be folded into
  a single line with spaces
  between the words.
  
  Empty lines create paragraph breaks.
```

Folded scalars join lines with spaces, making them ideal for long text passages. Empty lines create paragraph breaks.

---

## 6. Collections

### 6.1 Sequences (Arrays)

**Flow Style:**
```yaml
numbers: [1, 2, 3, 4, 5]
mixed: [string, 123, true, null]
```

**Block Style:**
```yaml
items:
  - first
  - second
  - third
nested:
  - [a, b, c]
  - [d, e, f]
```

### 6.2 Mappings (Objects)

**Flow Style:**
```yaml
config: {host: localhost, port: 8080, debug: true}
```

**Block Style:**
```yaml
server:
  host: localhost
  port: 8080
  ssl:
    enabled: true
    cert: /path/to/cert
```

### 6.3 Nested Collections

```yaml
users:
  - name: Alice
    roles: [admin, user]
    config:
      theme: dark
      notifications: true
  - name: Bob
    roles: [user]
    config:
      theme: light
      notifications: false
```

---

## 7. Anchors and Aliases

YAML supports anchors (`&name`) and aliases (`*name`) for reusing values:

### 7.1 Basic Anchors

```yaml
defaults: &defaults
  timeout: 30
  retries: 3

service1:
  <<: *defaults
  name: api

service2:
  <<: *defaults
  name: worker
  timeout: 60  # Override default
```

### 7.2 Merge Keys (`<<`)

The special key `<<` merges mapping values:

```yaml
base: &base
  x: 1
  y: 2

extended:
  <<: *base
  z: 3
# Results in: {x: 1, y: 2, z: 3}
```

### 7.3 Alias Expansion Limits

To prevent denial-of-service attacks via exponential alias expansion, the parser enforces `max_alias_expansion`:

```yaml
# This could cause exponential expansion:
a: &a [x, x]
b: &b [*a, *a]     # 4 elements
c: &c [*b, *b]     # 8 elements
d: &d [*c, *c]     # 16 elements
e: [*d, *d]        # 32 elements
```

The parser tracks total expansion count and fails with `GTEXT_YAML_E_LIMIT` when exceeded.

### 7.4 Cycles, and the one kind a document can hold

An alias names "the most recent **preceding** node having the same anchor"
(3.2.2.2), so this is not a cycle:

```yaml
a: &a
  b: *b       # *b names an anchor that does not exist yet
b: &b
  a: *a
```

is not a cycle at all - it is a forward reference, and the parser refuses it
with `GTEXT_YAML_E_INVALID` and *Unknown anchor referenced by alias*. PyYAML
and js-yaml refuse it too. Mutual recursion cannot be written in YAML for
that reason: whichever of the two anchors comes second, the alias to it in
the first is a forward reference.

**Self-reference can be, and is accepted.** A node's anchor precedes
everything inside it, so an alias within the anchored collection legitimately
names it:

```yaml
&O
k: v
j: *O         # *O is this mapping
```

`gtext_yaml_alias_target()` returns the enclosing node. PyYAML renders the
same document as `{'k': 'v', 'j': {...}}`. The flow spelling `&O [1, *O]`
names the sequence the same way.

A recursive document is finite to handle, because an alias is a node in its
own right and nothing expands it: the DOM holds the alias and its target,
`max_alias_expansion` counts the aliases a document *writes* rather than the
expansions a reader could take from them, and the writer emits `*O` by name.
What such a document will not do is convert: `gtext_yaml_to_json()` refuses
an alias, because JSON has no way to say one. A caller that walks the DOM
itself is the one that has to expect a cycle, and
`gtext_yaml_node_anchor()` on each collection is what tells it where one can
close.

`max_alias_expansion` is counted by the streaming parser, by the DOM parser
while it resolves aliases, and by `gtext_yaml_to_json()` when it
materializes aliases.

---

## 8. Multi-Document Streams

YAML supports multiple documents in a single stream using `---` (document separator) and `...` (document terminator):

```yaml
---
doc: 1
name: First document
---
doc: 2
name: Second document
...
---
doc: 3
name: Third document
```

**Usage:**

```c
// The streaming parser handles multi-document streams automatically
// Each document's content is delivered via callbacks
// Document boundaries are implicit in the event stream

GTEXT_YAML_Status status = gtext_yaml_stream_feed(stream, multi_doc_yaml, len);
status = gtext_yaml_stream_finish(stream);
```

---

## 9. Encoding Support

The parser accepts UTF-8, UTF-16, and UTF-32 input when a BOM is present.
Input is normalized to UTF-8 internally.

That normalization is what every reported `offset` counts bytes of - an error's,
a warning's, an event's, a node's source location. It is an index into the
buffer you passed in only for UTF-8 with no BOM; a BOM is stripped before
decoding and shifts them by three, and UTF-16 and UTF-32 bear no byte-for-byte
relation to it. `line` and `col` are counted in characters and are right in
every encoding, so use those to locate something in the original.

### 9.1 BOM Detection and Transcoding

Supported BOMs:

- UTF-8: EF BB BF
- UTF-16LE: FF FE
- UTF-16BE: FE FF
- UTF-32LE: FF FE 00 00
- UTF-32BE: 00 00 FE FF

If no BOM is present, input is treated as UTF-8.

### 9.2 UTF-8 Validation

All input is validated as valid UTF-8. Invalid sequences cause `GTEXT_YAML_E_INVALID`:

```c
// Invalid UTF-8 will be rejected
const char *invalid_utf8 = "key: \xFF\xFF invalid";
GTEXT_YAML_Status status = gtext_yaml_stream_feed(stream, invalid_utf8, strlen(invalid_utf8));
// status == GTEXT_YAML_E_INVALID
```

### 9.3 Unicode Escapes

Double-quoted strings support Unicode escapes:

```yaml
unicode: "\u0048\u0065\u006C\u006C\u006F"  # Hello
emoji: "\U0001F600"  # 😀
chinese: "\u4E2D\u6587"  # 中文
```

---

## 10. Limits and Security

### 10.1 Decompression Bomb Protection

The parser implements multiple layers of protection against malicious inputs:

**Max Depth:** Prevents stack overflow via deeply nested structures.
```yaml
# With max_depth=5, this would fail:
- - - - - - - - - - - too deep
```

**Max Total Bytes:** Prevents memory exhaustion from large inputs.
```c
opts.max_total_bytes = 10 * 1024 * 1024;  // 10 MB limit
```

**Max Alias Expansion:** Prevents "YAML bombs" where tiny inputs expand exponentially.
```c
opts.max_alias_expansion = 100;  // Strict limit for untrusted input
```

### 10.2 Example: Strict Limits for Untrusted Input

```c
GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();

// Max 1 MB input
opts.max_total_bytes = 1024 * 1024;

// Max 32 levels deep
opts.max_depth = 32;

// Max 50 alias expansions
opts.max_alias_expansion = 50;

// Use first occurrence for duplicate keys (lenient)
opts.dupkey_mode = GTEXT_YAML_DUPKEY_FIRST_WINS;

GTEXT_YAML_Stream *stream = gtext_yaml_stream_new(&opts, callback, NULL);
```

---

## 11. Common Patterns

### 11.1 Configuration Files

```yaml
# application.yaml
app:
  name: MyApp
  version: 1.0.0
  debug: false

server:
  host: 0.0.0.0
  port: 8080
  workers: 4

database:
  driver: postgresql
  host: db.example.com
  port: 5432
  name: production
  pool:
    min: 5
    max: 20
```

### 11.2 Docker Compose

```yaml
version: '3.8'
services:
  web:
    image: nginx:latest
    ports:
      - "80:80"
    volumes:
      - ./html:/usr/share/nginx/html
  db:
    image: postgres:13
    environment:
      POSTGRES_PASSWORD: secret
```

### 11.3 Kubernetes Manifests

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: nginx-deployment
spec:
  replicas: 3
  selector:
    matchLabels:
      app: nginx
  template:
    metadata:
      labels:
        app: nginx
    spec:
      containers:
      - name: nginx
        image: nginx:1.14.2
        ports:
        - containerPort: 80
```

### 11.4 CI/CD Configuration

```yaml
# .github/workflows/ci.yml
name: CI
on:
  push:
    branches: [main, develop]
  pull_request:
    branches: [main]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Run tests
        run: make test
```

---

## 12. Best Practices

### 12.1 Memory Management

- Always call `gtext_yaml_stream_finish()` after feeding all input
- Always call `gtext_yaml_stream_free()` to release resources
- The parser is designed to be leak-free (verified with valgrind)

### 12.2 Error Handling

- Always check return status codes
- Don't continue processing after errors
- Set appropriate limits for untrusted input

### 12.3 Performance

- Use streaming parsing for large documents
- Feed input in reasonable chunks (4KB-64KB is typical)
- Don't set limits unnecessarily low (causes early failures)

### 12.4 Security

- Always set `max_total_bytes` for untrusted input
- Use strict `max_alias_expansion` (e.g., 100) for untrusted input
- Set reasonable `max_depth` based on expected data structure

---

## 13. Testing

The YAML module is covered by 104 test files holding 768 tests, part of a suite
that runs 2239 tests across 130 binaries with zero failures. The YAML figure is
the one to quote about YAML; the suite total moves whenever any module gains a
test, and this line has carried a stale one before. They cover:

- ✅ All scalar styles (plain, quoted, literal, folded)
- ✅ Escape sequences and Unicode handling
- ✅ Flow and block collections
- ✅ Nested structures up to max depth
- ✅ Anchors, aliases, and merge keys
- ✅ Multi-document streams
- ✅ Error conditions and invalid input
- ✅ Limit enforcement
- ✅ UTF-8 validation
- ✅ Real-world YAML files (Docker, K8s, GitHub Actions, etc.)

All tests pass with zero memory leaks (valgrind-verified).

---

## 13.5 Schema validation

`gtext_yaml_validate()` checks a document against a **JSON Schema**, and
`gtext_yaml_schema_compile()` takes the schema as a YAML document, which is
how they are normally written.

There is no YAML schema language to use instead. YAML 1.2 section 10 defines
schemas and they are about implicit typing - whether `yes` is a boolean - not
about validation; `GTEXT_YAML_Schema` selects one of those and is not a
validator. Kwalify and Rx were proposed as validation languages and neither is
used. What is used is JSON Schema: Kubernetes, OpenAPI, GitHub Actions, Azure
Pipelines, Ansible and the `yaml-language-server` directive all describe YAML
documents with it.

```c
GTEXT_YAML_Error err;
memset(&err, 0, sizeof(err));

GTEXT_JSON_Schema *schema = gtext_yaml_schema_compile(schema_doc, NULL, &err);
switch (gtext_yaml_validate(doc, schema, NULL, &err)) {
  case GTEXT_YAML_OK:
    break;
  case GTEXT_YAML_E_SCHEMA:
    /* err.line and err.col are positions in the YAML. */
    fprintf(stderr, "%s:%d:%d: %s\n", path, err.line, err.col, err.message);
    break;
  default:
    /* The document could not be expressed as JSON, so the schema was never
       applied. A different problem with a different fix. */
    fprintf(stderr, "%s: %s\n", path, err.message);
    break;
}
gtext_json_schema_free(schema);
```

**The position is the point.** Converting with `gtext_yaml_to_json()` and
calling `gtext_json_schema_validate()` gives the same verdict. What it cannot
give is a place in a file the user wrote: a JSON Schema failure names a
pointer into the converted instance, and `/spec/2/env` with no line number
sends them hunting through a document that does not contain those names.
`gtext_yaml_validate()` resolves that pointer back through the YAML.

Three things about the conversion are worth knowing before trusting a pass,
because validation is of the *converted* document:

- **Non-string keys** are refused by default; with
  `coerce_keys_to_strings` the key `1` becomes the name `"1"`, and a schema's
  `properties` has to spell it that way.
- **Aliases** are refused by default; with `allow_resolved_aliases` they are
  expanded, so a schema sees the same subtree twice and `maxItems` counts the
  expansion. A failure inside an expanded copy is reported at the anchor.
- **Tags** become the JSON shapes `gtext_yaml_to_json()` gives them, so a
  schema describes those shapes and not the YAML tags.

One document at a time; walk a multi-document stream and validate each.

`examples/yaml/yaml_validate.c` is a runnable version of the above.

## 14. Future Enhancements

### Known gaps

**What the writers are measured against.** `make conformance-roundtrip` writes
every suite document this parser accepts back out and re-reads it, through the
DOM writer in flow style, the DOM writer in block style, and the streaming
writer. All keep 282 of 282 by value, which is the figure the target
holds them to; block style also keeps every anchor, tag and scalar spelling.
The two flow figures stop at 280 by event for one reason, which is in the
grammar rather than in the writer: `ns-flow-seq-entry` has no empty
alternative, so a flow sequence entry that is the empty node with no
properties has to be written `~`.

**Schema validation used to be here**, as "there is no YAML schema
validator". It is section 13.5 above now. What is still absent, and
deliberately, is a *YAML-native* schema language: a fourth proposal after
Kwalify and Rx would have no users.

**`gtext_yaml_stream_*` does not feed `gtext_yaml_writer_event()`**, however
alike the two look. Both speak `GTEXT_YAML_Event`. The writer takes composed
events. The streaming parser reports `:`, `-` and `,` as indicators and
leaves composing to its consumer. An indicator passed to the writer is
refused. `gtext_yaml_stream_walk()` produces events a writer can take.

yaml-test-suite is a corpus of inputs and tests no writer.
`tests/fuzz/fuzz_yaml_writer.cpp` builds documents through the DOM as well as
from a parse, so a value the parser would have refused still reaches the
writer.

### Building an omap

`!!omap` is an *ordered mapping*, so `gtext_yaml_sequence_append()` and
`gtext_yaml_sequence_insert()` hold one to both halves of that: an entry has
to be a single-pair mapping, and its key must not already be in the omap.
Either violation returns NULL. The parser refuses a document that breaks
either rule, so an omap the appenders accepted but the parser would not read
back could not be written by anything.

`!!pairs` is the sequence-shaped type that *does* take repeated keys, and is
not checked.

### Building scalars

`gtext_yaml_node_new_scalar()` takes the node's type from its text, which is
what a parse of the same characters would report: `"1"` builds an integer,
`"x"` a string. Use `gtext_yaml_node_new_scalar_typed()` to say otherwise -
the *string* `"1"` is a different value from the integer `1`, and YAML spells
the difference with quotes, which the writer then supplies.
`gtext_yaml_node_new_scalar_n()` and `gtext_yaml_node_scalar_length()` are for
values holding a NUL, which `\0` makes a legal thing for a scalar to hold.

The writer has to be told which dialect the output is for, and
`GTEXT_YAML_Write_Options` has `schema` and `yaml_1_1` for it. They default to
the 1.2 core schema, which is what `gtext_yaml_parse_options_default()` reads.
Set them to match the options a document was parsed with, or will be parsed
with, and the writer quotes what that dialect would otherwise resolve - the
string `"yes"` needs quotes for a 1.1 reader and not for a 1.2 one - and
spells a null the way that dialect spells one, which is `null` rather than `~`
under the JSON schema. The failsafe schema resolves nothing, so nothing is
quoted for its sake and no type survives a round trip through it.

Saying otherwise is not the same as saying anything. The type has to be true
of the text, so every type but `GTEXT_YAML_STRING` is checked against it and a
claim the characters cannot carry returns NULL: `"NO"` declared null is
refused, exactly as `!!null NO` is refused on the way in. A string is never
refused, because any text is a string. The rule is that the typed constructor
accepts for a type what this parser accepts behind the tag naming it.

"Text a parse would report" is not the same as "text you could hand the
parser". White space at either end makes it a string, whatever the rest says:
`" 3"` is the *string* `" 3"`, not the integer 3, because a plain scalar's
content has white space at neither end (7.3.3) and only a quoted scalar can
spell it - which is what the writer emits, and what a re-read gives back.

### Two parsers, one contract

`gtext_yaml_parse()` hands input that is also JSON to the JSON parser and
converts the result. That path is on by default. A quoted scalar stays a
string: its contents are resolved only when it was written plain.

`make conformance` asks for `GTEXT_YAML_DUPKEY_KEEP_ALL`, which turns the
fast path off, so the 395 does not include it. `make conformance-fastpath`
reaches five of the suite's 406 documents, because the rest are not JSON.
`tests/yaml/test-yaml-json-fastpath.cpp` is what holds the two together.

Comments and scalar style survive a parse-write cycle when `retain_comments`
was set at parse time and `pretty` is set on the write. The default write
is flow style, which has nowhere for a comment on its own line and no
spelling for a block scalar. `gtext_yaml_node_source_location()`,
`gtext_yaml_to_json()` and `gtext_json_to_yaml()` are implemented.

### Still open

- **Benchmarks.** Parsing speed and memory use are unmeasured. `make
  conformance` scores the test suite; see Compatibility below.

### Compatibility

The parser targets YAML 1.2.2 and answers **all 395 of the**
[YAML test suite](https://github.com/yaml/yaml-test-suite) cases that can be
checked - by value, by event stream, or by refusal - measured by
`make conformance`. That is 395 of the suite's 406; the other eleven carry no
expectation, or one the harness cannot decode, and are neither passed nor
failed. The same harness scores js-yaml at
82.0% and PyYAML at 77.3% on the value cases, so neither reference reaches
100% on the cases it does check.

See \ref format_yaml "the YAML format page" for what that score covers and
what it does not.

---

## 15. API Reference

### Custom Tags

Custom tags are opt-in and configured through parse/write options. Set
`enable_custom_tags=true` and provide a `custom_tags` array with
`custom_tag_count`. The parser calls the `construct` callback for matching
explicit tags. The writer calls the `represent` callback to choose the tag
to emit.

```c
GTEXT_YAML_Custom_Tag handlers[] = {
  {
    .tag = "!example",
    .construct = my_construct,
    .represent = my_represent,
    .user = my_context,
  },
};

GTEXT_YAML_Parse_Options parse_opts = gtext_yaml_parse_options_default();
parse_opts.enable_custom_tags = true;
parse_opts.custom_tags = handlers;
parse_opts.custom_tag_count = 1;

GTEXT_YAML_Write_Options write_opts = gtext_yaml_write_options_default();
write_opts.enable_custom_tags = true;
write_opts.custom_tags = handlers;
write_opts.custom_tag_count = 1;
```

### Core Functions

```c
// Create streaming parser
GTEXT_YAML_Stream* gtext_yaml_stream_new(
    const GTEXT_YAML_Parse_Options *opts,
    GTEXT_YAML_Stream_Callback callback,
    void *user_data
);

// Feed input chunk
GTEXT_YAML_Status gtext_yaml_stream_feed(
    GTEXT_YAML_Stream *stream,
    const char *data,
    size_t len
);

// Finish parsing (validate completeness)
GTEXT_YAML_Status gtext_yaml_stream_finish(GTEXT_YAML_Stream *stream);

// Free resources
void gtext_yaml_stream_free(GTEXT_YAML_Stream *stream);

// Get default options
GTEXT_YAML_Parse_Options gtext_yaml_parse_options_default(void);

// Free error structure
void gtext_yaml_error_free(GTEXT_YAML_Error *err);
```

### Type Definitions

See `<ghoti.io/text/yaml/yaml_core.h>` for complete type definitions and enumerations.

---

## 16. Thread Safety

No YAML object is thread-safe. A document, a parser, a stream, a pull reader
and a writer each belong to one thread at a time; two that were created
separately share nothing and may be used concurrently.

Number conversion does not read or change the process locale. A document
that no thread is modifying may be read from several at once.

The read accessors really are reads. `gtext_yaml_mapping_get()` is a linear
scan of the stored pairs and `gtext_yaml_alias_target()` returns a stored
pointer; aliases and merge keys are resolved while parsing, not cached lazily
on first access. So concurrent readers of a finished document are safe, alias
and merge keys included.

The full rule and the reasoning are in the \ref core_module "Core module page",
section 7.

## 17. License

LGPL-3.0-only. Copyright (C) 2026 Corey Pennycuff. The full text is in
`COPYING.LESSER` at the root of the repository, alongside the `COPYING`
(GPL-3.0) it is written as additional permissions on top of.

Part of the ghoti.io text library.

---

**Last Updated:** September 22, 2026  
**Module Version:** 0.1.0 (Alpha)  
**Test count:** 723 YAML test cases across 103 binaries, of 1,635 across the
suite, all passing.

Both figures count each binary once. `make test` has three group targets
(`Text tests`, `JSON tests`, `CSV tests`) that re-run binaries the per-target
rules have already run, so summing every `[  PASSED  ]` line gives 2,467 -
832 more than there are tests. The rule is to count only what follows a
single-token `### Running <name> ###` header, and to note that 115 such
headers appear while 114 report a total: `testHeaders` is a plain C program
rather than a gtest binary and prints none.

`make test` exits non-zero when any suite fails, which is worth stating
because it has not always been true and because the log cannot be trusted to
say so on its own: a test that *crashes* prints no `[  FAILED  ]` line at
all, so a count of those lines reads a segfaulting suite as green. Measured -
a planted null dereference gives exit 2 and names the suite, with zero
`[  FAILED  ]` lines in the output.
