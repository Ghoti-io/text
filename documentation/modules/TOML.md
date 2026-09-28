@page toml_module TOML

# TOML

The TOML module reads a [TOML v1.0.0](https://toml.io/en/v1.0.0) document into
a tree and gives you accessors for it. It is a reader: there is no writer yet.

Like the rest of `text` it depends on
[ghoti.io-cutil](https://github.com/Ghoti-io/cutil) and
[ghoti.io-chron](https://github.com/Ghoti-io/chron) - and here chron is not
incidental. TOML's four date-time types *are* chron values, so
`<ghoti.io/text/toml/toml_dom.h>` includes `<ghoti.io/chron/chron.h>` and a
date-time you read out of a document can be converted, compared or written
without a second header.

---

@note This page documents the **API**. For the specification-level view -
which clauses are implemented, the four redefinition rules, every rejection
with its status code, the corpus score and what nothing checks yet - see
\ref format_toml "TOML" under
\ref text_format_references "Format and specification references".

## 1. Overview

```c
#include <ghoti.io/text/toml.h>

GTEXT_TOML_Error err;
memset(&err, 0, sizeof(err));
GTEXT_TOML_Value * root = gtext_toml_parse(bytes, len, NULL, &err);
if (!root) {
  fprintf(stderr, "line %d column %d: %s\n", err.line, err.col, err.message);
  gtext_toml_error_free(&err);
  return 1;
}
...
gtext_toml_free(root);
```

Two calls and one release. The whole tree belongs to the root: no node is
freed on its own, and no pointer into the tree outlives `gtext_toml_free()`.
Nothing points into the input buffer either, so `bytes` may be released as soon
as the parse returns - TOML strings need unescaping and its keys need joining,
so there is no in-situ mode to offer.

## 2. Entry points

| Function | What it does |
|---|---|
| `gtext_toml_parse()` | Parse a buffer. Returns the root table, or NULL. |
| `gtext_toml_parse_file()` | Read and parse a file, incrementally, so a pipe or `/dev/stdin` works. |
| `gtext_toml_free()` | Release a document. |
| `gtext_toml_error_free()` | Release the snippet inside an error. Safe on a zeroed struct, and twice. |
| `gtext_toml_parse_options_default()` | The defaults, by value. |

A successful parse always returns a `GTEXT_TOML_TABLE`, because a TOML document
*is* a table. An empty input gives an empty table rather than an error.

## 3. Options

```c
GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
opts.max_depth = 32;
```

| Field | Default | Meaning |
|---|---|---|
| `allocator` | `NULL` | Every allocation of the parse goes through it; `NULL` means `gtext_allocator_default()` |
| `max_depth` | 256 | Nesting of arrays and inline tables; 0 for no limit |
| `max_total_bytes` | 0 | Bytes of input; 0 for no limit. `gtext_toml_parse_file()` applies it *while* reading, so an over-large file is refused rather than held |

Start from `gtext_toml_parse_options_default()` rather than from a zeroed
struct: zeroed asks for no depth limit, which is a decision rather than a
default.

`max_depth` at 0 is genuinely safe here and not merely allowed. The value
parser builds on an explicit stack and the teardown walks a worklist, so
neither the parse nor the free grows the C stack with the document's nesting.

## 4. Reading the tree

`gtext_toml_value_type()` returns one of seven:

| Type | Accessor |
|---|---|
| `GTEXT_TOML_TABLE` | `gtext_toml_table_size()`, `_table_key_at()`, `_table_value_at()`, `_table_get()` |
| `GTEXT_TOML_ARRAY` | `gtext_toml_array_size()`, `_array_get()` |
| `GTEXT_TOML_STRING` | `gtext_toml_value_string()` |
| `GTEXT_TOML_INTEGER` | `gtext_toml_value_integer()` → `int64_t` |
| `GTEXT_TOML_FLOAT` | `gtext_toml_value_float()` → `double` |
| `GTEXT_TOML_BOOLEAN` | `gtext_toml_value_boolean()` |
| `GTEXT_TOML_DATETIME` | `gtext_toml_value_datetime()` → `GCHRON_TomlValue` |

Every accessor returns NULL or `false` when the value is of another type, so a
caller may test the type or test the accessor, whichever reads better.

**Seven types where the specification lists ten.** The four date-time types
share `GTEXT_TOML_DATETIME` and are told apart by `GCHRON_TomlValue::kind`:

```c
GCHRON_TomlValue dt;
if (gtext_toml_value_datetime(value, &dt)) {
  switch (dt.kind) {
    case GCHRON_TOML_OFFSET_DATE_TIME: ...  /* 1979-05-27T07:32:00Z */
    case GCHRON_TOML_LOCAL_DATE_TIME:  ...  /* 1979-05-27T07:32:00 */
    case GCHRON_TOML_LOCAL_DATE:       ...  /* 1979-05-27 */
    case GCHRON_TOML_LOCAL_TIME:       ...  /* 07:32:00 */
  }
}
```

Four enumerators here would be a second spelling of `GCHRON_TomlKind`, and two
spellings of one axis have to be kept in step by whoever edits either.

### Keys

`gtext_toml_table_key_at()` returns keys **in the order the document defined
them**, not sorted and not in whatever order a hash would give. That is what a
writer needs to reproduce an arrangement a person chose, and there is no other
order a reader could rely on.

A key is the *decoded* string, so `a.b`, `a."b"` and `a.'b'` all name one
entry, and `'a.b'` is a single key containing a dot. A key may also contain a
NUL, because U+0000 has a valid escape and may be written in a quoted key -
which is why every key function takes and returns a length, and why
`gtext_toml_table_get()` takes `key_len` rather than reading to a terminator.
The returned bytes are NUL-terminated as a convenience, but the length is what
is authoritative.

## 5. Errors

```c
typedef struct {
  GTEXT_TOML_Status code;
  const char * message;        /* static; never freed */
  size_t offset;               /* byte offset into the input */
  int line;                    /* 1-based */
  int col;                     /* 1-based, in characters */
  char * context_snippet;      /* the offending line; freed by _error_free() */
  size_t context_snippet_len;
  size_t caret_offset;
} GTEXT_TOML_Error;
```

`offset` is always an index into the buffer you passed: TOML is UTF-8 by
definition, so there is no decoding step that could move it - unlike the YAML
module, where a byte order mark and the UTF-16 encodings make offsets and
buffer positions two different things.

`col` counts **characters**, so a column number points at the right character
in a line containing multi-byte text.

`GTEXT_TOML_Status` names the kind of refusal rather than folding everything
into one code: `E_DUPKEY` is a mistake in the document's data, `E_REDEFINE` in
its structure, `E_CONTROL` and `E_BAD_UNICODE` in its encoding, `E_RANGE` in a
number, `E_DATETIME` in a date, `E_BAD_ESCAPE` in a string, `E_BAD_TOKEN` in
its syntax. \ref format_toml "The format page" says which malformation
produces which.

`context_snippet` comes from the default allocator even when the parse used
yours, and `gtext_toml_error_free()` releases it through the same one - an
error struct outlives the parse, and a parse can fail before it has read its
options at all.

## 6. What is not here yet

- A **writer**.
- **TOML 1.1.0**, which will be an option rather than a relaxation of the
  default: 1.1.0 also refuses two things 1.0.0 accepts.
- A **pull reader / event API**, as JSON, CSV and YAML have.
- **Comments in the tree**, which the YAML DOM does carry.
- `toml_to_json` and `json_to_toml`.

\ref format_toml "The format page" has the measured state of each.
