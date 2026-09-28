@page toml_module TOML

# TOML

The TOML module reads a [TOML v1.0.0](https://toml.io/en/v1.0.0) document into
a tree, gives you accessors for it, lets you build one from nothing, and writes
one back out.

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
| `gtext_toml_write()` | Write a document to a sink. |
| `gtext_toml_write_file()` | Write a document to a file, atomically. |
| `gtext_toml_write_options_default()` | The write defaults, by value. |
| `gtext_toml_read_events()` | Walk a document statement by statement, calling a callback. See section 5. |

A successful parse always returns a `GTEXT_TOML_TABLE`, because a TOML document
*is* a table. An empty input gives an empty table rather than an error, and
writing it gives an empty document.

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
| `version` | `GTEXT_TOML_VERSION_1_0_0` | Which revision to read. `GTEXT_TOML_VERSION_1_1_0` reads the draft |
| `retain_comments` | `false` | Keep comments: as events from `gtext_toml_read_events()`, and on the tree where the writer can put them back |

Start from `gtext_toml_parse_options_default()` rather than from a zeroed
struct: zeroed asks for no depth limit, which is a decision rather than a
default. `version` is the one field whose zero *is* the right answer for a
caller who did not think about it - it is the released specification - and it is
asserted from all three routes (the constructor, a zeroed struct, a `NULL`
options pointer) in `tests/test-toml-version.cpp`.

`max_depth` at 0 is genuinely safe here and not merely allowed. The value
parser builds on an explicit stack and the teardown walks a worklist, so
neither the parse nor the free grows the C stack with the document's nesting.

### 3b. The 1.1.0 option

```c
GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
opts.version = GTEXT_TOML_VERSION_1_1_0;
```

Three constructs change, and nothing else:

```toml
# All three are E_BAD_* at 1.0.0 and read at 1.1.0.
esc  = "\e and \x41"          # U+001B and "A"
tbl  = {                      # newlines, comments and a trailing comma,
  a = 1,                      # at the separators only
}
time = 07:32                  # the seconds are read as :00
```

Two things to know before reaching for it:

- **It is a draft.** No released reference implements it - CPython's `tomllib`
  is 1.0.0 - so on that arm the only reference is toml-test's own 1.1.0
  manifest. That is why it is not the default.
- **The relaxation stops at the inline table's separators.** `{a\n= 1}` and
  `{a =\n1}` are invalid at 1.1.0 too, because `keyval-sep` is still plain
  whitespace. Neither manifest has a case either way; `\ref format_toml` says
  how that was measured.

**There is no write-side version option**, and that is a finding rather than an
omission: every spelling 1.0.0 defines is still a 1.1.0 spelling, so the writer
is already correct for both. Measured - the corpus's encode rows score 218 of
218 against the 1.1.0 manifest with no writer change at all.

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

## 4b. Writing, and building a document

```c
GTEXT_TOML_Sink sink;
gtext_toml_sink_buffer(&sink);
if (gtext_toml_write(root, &sink, NULL) == GTEXT_TOML_OK) {
  fwrite(gtext_toml_sink_buffer_data(&sink), 1,
      gtext_toml_sink_buffer_size(&sink), stdout);
}
gtext_toml_sink_buffer_free(&sink);
```

Two sinks are provided - `gtext_toml_sink_buffer()`, which grows, and
`gtext_toml_sink_fixed_buffer()`, which uses a buffer of yours - and a sink is
a function pointer and a `void *`, so a third is four lines. A sink's callback
returns a `GTEXT_TOML_Status` rather than an `int`, and the writer hands it back
to you unchanged: `E_OOM` from inside a sink stays distinguishable from
`E_WRITE`, which is the distinction a caller retrying on a full destination
needs.

A fixed buffer that runs out **fails**, and the write returns `E_WRITE`, rather
than handing back a shorter document. Ask
`gtext_toml_sink_fixed_buffer_truncated()` rather than comparing the byte count:
a full buffer and an exactly-fitting document have the same count.

### What the output looks like

| `table_style` | A table | An array whose elements are all tables |
|---|---|---|
| `_AS_READ` (default) | as it was read: `[header]` or `{ }` | as it was read: `[[header]]` or `[ ]` |
| `_HEADERS` | `[header]` | `[[header]]`, unless empty |
| `_INLINE` | `{ }` | `[{ }, { }]` |

Two things are not options. Plain keys are written before sub-tables, because
every bare key after a `[header]` belongs to the table that header opened; and
an empty array is always `key = []`, because zero `[[key]]` headers would not
say that the key exists. \ref format_toml "The format page" has the rest -
which spelling every scalar gets, and why.

### Building one

```c
GTEXT_TOML_Value * root = gtext_toml_new_table(NULL);
GTEXT_TOML_Value * port = gtext_toml_new_integer(NULL, 8080);
if (gtext_toml_table_set(root, "port", 4, port) != GTEXT_TOML_OK) {
  gtext_toml_free(port);     /* on failure it is still yours */
}
gtext_toml_free(root);
```

`gtext_toml_new_table()`, `_new_array()`, `_new_string()`, `_new_integer()`,
`_new_float()`, `_new_boolean()` and `_new_datetime()` make a node;
`gtext_toml_table_set()` and `gtext_toml_array_append()` take ownership of it
**on success only**; `gtext_toml_value_set_inline()` says which spelling a
container should get under `_AS_READ`.

Every constructor takes the allocator, because a node carries the one it was
made with and `gtext_toml_free()` releases the whole tree through the root's.
Mixing two in one tree is refused with `E_INVALID` rather than corrupting the
heap later - though `NULL` and `gtext_allocator_default()` count as the same
allocator, since they are.

Three shapes are refused with `E_STATE`, all at the one call that could create
them: storing a value that is already in a tree (a double free at teardown),
storing a container inside itself, and storing it inside one of its own
descendants (two shapes of cycle, which would make the writer loop and the free
walk repeat). None of the three is something a test could detect afterwards.
A duplicate key is `E_DUPKEY` and not a replacement: TOML says defining a key
twice is invalid.

## 5. The statements, in the order they were written

```c
static GTEXT_TOML_Status on_event(void * user, const GTEXT_TOML_Event * e,
    GTEXT_TOML_Error * err) {
  (void) user;
  (void) err;
  if (e->type == GTEXT_TOML_EVT_KEY) {
    printf("line %d: %.*s\n", e->line,
        (int) e->key.parts[e->key.count - 1].len,
        e->key.parts[e->key.count - 1].data);
  }
  return GTEXT_TOML_OK; /* anything else stops the walk */
}

GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
opts.retain_comments = true;  /* for GTEXT_TOML_EVT_COMMENT */
gtext_toml_read_events(bytes, len, &opts, on_event, NULL, &err);
```

The tree says what a document *means*; this says what it *says*. They differ
whenever the arrangement matters: a table can be reopened lower down as a
sub-table of itself, dotted keys interleave with headers, and a comment between
two keys belongs to neither. Nine event types, `TABLE` and `ARRAY_TABLE` and
`KEY` carrying a decoded key path, `VALUE` carrying a borrowed node you read with
the ordinary accessors, the four brackets, and `COMMENT`.

It is the same parser with a callback, so it refuses exactly what
`gtext_toml_parse()` refuses, in the same place - and it builds the tree anyway,
because TOML's redefinition rules are answered against it, then frees it. What
you save is the tree's *lifetime*, not its cost: nothing comes back, so there is
nothing to free.

**There is no `feed`-shaped reader here** as JSON, CSV and YAML have, because
this parser is not incremental and a `feed` over it would buffer the whole
document while implying otherwise. \ref format_toml "The format page" gives the
two reasons, both of them the format's.

## 5b. Comments

```c
GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
opts.retain_comments = true;
GTEXT_TOML_Value * root = gtext_toml_parse(bytes, len, &opts, &err);
const char * why = gtext_toml_value_leading_comment(
    gtext_toml_table_get(root, "port", 4));
```

`gtext_toml_value_leading_comment()`, `_inline_comment()` and
`_trailing_comment()` read them; the matching `_set_*` calls write them. They
hang from the value a statement defined - the comments around `a = 1` belong to
the node `1` - and the text is the bytes after the `#`, verbatim, so `#` plus the
text is the line. Write a leading space yourself if you want one.

The tree keeps the comments the writer can put back. One inside `{ }` or between
two array elements has no statement to attach to and no line in the output to go
on, so it is reported by `gtext_toml_read_events()` and not kept here; the writer
answers `GTEXT_TOML_E_UNREPRESENTABLE` if it is handed one it cannot place.
\ref format_toml "The format page" has the division, the count over the corpus,
and the four refusals.

## 6. Errors

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

## 7. What is not here yet

- **A 1.1.0 reference.** The option reads the draft; nothing outside this
  repository does, so the fifteen cases where the two manifests disagree are
  checked against toml-test's decision and against nothing else.
- An **incremental reader**. `gtext_toml_read_events()` walks a whole document;
  a chunk-fed one is argued against on the format page rather than pending.
- **Comments inside a value, on the tree.** They reach you through the event
  walk; the tree keeps the ones the writer can put back.
- `toml_to_json` and `json_to_toml`.

\ref format_toml "The format page" has the measured state of each.
