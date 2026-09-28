# Ghoti.io Text

Parsers and writers for structured text, in C. Each format below has a
document you can walk, a streaming parser, and a writer. They share one
contract for errors, limits and allocation.

## Formats

This is what the library implements.

- **JSON.** RFC 8259 / ECMA-404, plus JSON Pointer (RFC 6901), JSONPath (RFC 9535), JSON Patch (RFC 6902), JSON Merge Patch (RFC 7386), and JSON Schema (2020-12, 2019-09, draft-07 and draft-06). JSONC and JSON5 are each a set of options.
- **CSV.** RFC 4180, and other dialects through `GTEXT_CSV_Dialect`.
- **YAML.** YAML 1.2.2, including its Core, JSON and Failsafe schemas, plus a YAML 1.1 resolution mode and the 1.1 types `!!timestamp`, `!!set`, `!!omap`, `!!pairs` and the `<<` merge key.
- **TOML.** TOML v1.0.0, read only: the whole grammar, all four date-time types through `ghoti.io-chron`, and 709 of 709 of toml-test's 1.0.0 cases. There is no writer yet, and TOML 1.1.0 is not implemented - it is still a draft, and it refuses two things 1.0.0 accepts, so it will be an option rather than a relaxation.

INI is planned and has no parser yet.

## Before you call it

For every format:

- Options come from `*_options_default()`. `NULL` is that struct. There is no global mode.
- An error is a status code plus a byte offset, line and column. Release a snippet with `*_error_free()`. The message is a static string.
- Every parser caps nesting depth, total input size and its own counts, each with a default.
- Pass a `GTEXT_Allocator`, or `NULL` for the default.
- `gtext_*_parse_file()` reads incrementally, so a pipe works, and the size limit applies before the whole file is in memory.
- `gtext_*_write_file()` writes a temporary beside the destination and renames it into place.

### JSON

| Standard | What it means here |
| --- | --- |
| RFC 8259 / ECMA-404 | The default grammar. A document may be any value, so `42` is complete. Numbers keep the text they were written with; integer and `double` accessors apply when the value fits. |
| Duplicate names (RFC 8259 §4) | An error, unless the caller chooses `FIRST_WINS`, `LAST_WINS` or `COLLECT`. |
| JSONC and JSON5 | Each extension is its own option, off by default: comments, trailing commas, single quotes, `NaN`, hex numbers, and the rest. Turning one on leaves RFC 8259. |
| RFC 6901 JSON Pointer | Names one place in a document. |
| RFC 9535 JSONPath | Selects a set of nodes. `match()` and `search()` compile an I-Regexp with ghoti.io-regex. |
| RFC 6902 JSON Patch, RFC 7386 Merge Patch | Applied to a document already parsed. |
| JSON Schema 2020-12, 2019-09, draft-07, draft-06 | A keyword this library cannot enforce fails compilation and names the keyword. `pattern` and `patternProperties` run only when the caller supplies a regular-expression engine. `format` is an annotation unless the caller or the schema asks for it to be checked. `$schema` selects the dialect; a document with none is read as 2020-12, or as `default_dialect` when the caller set one. |
| IDNA2008 and UTS #46 | What `hostname` and `idn-hostname` check. |
| chron's grammars | What `date`, `date-time`, `time` and `duration` check. |

### CSV

| Standard | What it means here |
| --- | --- |
| RFC 4180 | The default dialect: comma, CRLF, doubled quotes. |
| Line endings | Bare LF is accepted. A bare CR is refused. |
| Header row | The first row is data until `treat_first_row_as_header` is set. |
| Other dialects | Delimiter, quote, escape, trimming, comments and a repeated header name have no specification of their own. `GTEXT_CSV_Dialect` is the one this library implements. Rows do not have to be rectangular. The default writer quotes a field that needs it (`GTEXT_CSV_QUOTE_MINIMAL`). `GTEXT_CSV_QUOTE_NONE` refuses that field instead of writing it bare. |

### YAML

| Standard | What it means here |
| --- | --- |
| YAML 1.2.2 | Block and flow collections, all five scalar styles, anchors and aliases, tags, and multi-document streams. Input may be UTF-8, UTF-16 or UTF-32. A document can be turned into JSON and back. |
| Core schema (1.2.2 §10.3) | The default. `yes` and `0755` are strings. JSON schema and Failsafe are the other two choices. |
| YAML 1.1 resolution | A `%YAML 1.1` directive, or the 1.1 parse option, restores the older spellings and warns when one of them matched. |
| YAML 1.1 type repository | `!!timestamp` comes back as a chron value. The `<<` merge key is on by default. Neither is part of the 1.2 core schema. |
| Untrusted input | `gtext_yaml_parse_safe()` is the hardened option set. Anchors detect cycles, and expansion is capped. |

## Examples

### JSON, read and write

```c
#include <ghoti.io/text/json.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char * src = "{\"name\":\"ghoti\",\"version\":[0,0,0]}";
  GTEXT_JSON_Error err = {0};
  GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

  GTEXT_JSON_Value * doc = gtext_json_parse(src, strlen(src), &opts, &err);
  if (!doc) {
    fprintf(stderr, "%s at line %d, column %d\n",
        err.message, err.line, err.col);
    gtext_json_error_free(&err);
    return 1;
  }

  const GTEXT_JSON_Value * field =
      gtext_json_object_get(doc, "name", strlen("name"));
  const char * name = NULL;
  size_t name_len = 0;
  if (field && gtext_json_get_string(field, &name, &name_len) == GTEXT_JSON_OK) {
    printf("%.*s\n", (int)name_len, name);
  }

  GTEXT_JSON_Sink sink;
  if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) {
    gtext_json_free(doc);
    return 1;
  }
  GTEXT_JSON_Write_Options write = gtext_json_write_options_default();
  write.pretty = true;
  if (gtext_json_write_value(&sink, &write, doc, &err) != GTEXT_JSON_OK) {
    fprintf(stderr, "%s\n", err.message);
    gtext_json_error_free(&err);
    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(doc);
    return 1;
  }
  printf("%s\n", gtext_json_sink_buffer_data(&sink));

  gtext_json_sink_buffer_free(&sink);
  gtext_json_free(doc);
  return 0;
}
```

```
ghoti
{
  "name": "ghoti",
  "version": [
    0,
    0,
    0
  ]
}
```

`gtext_json_parse_file()` and `gtext_json_write_file()` are the same calls
on a path. The write replaces the file by renaming a temporary beside it, so
an interrupted write leaves the old file in place.

### CSV, with a header

```c
#include <ghoti.io/text/csv.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char * src = "name,version\nghoti,0\n";
  GTEXT_CSV_Error err = {0};
  GTEXT_CSV_Parse_Options opts = gtext_csv_parse_options_default();
  opts.dialect.treat_first_row_as_header = true;

  GTEXT_CSV_Table * table =
      gtext_csv_parse_table(src, strlen(src), &opts, &err);
  if (!table) {
    fprintf(stderr, "%s\n", err.message);
    gtext_csv_error_free(&err);
    return 1;
  }

  size_t col = 0;
  size_t len = 0;
  if (gtext_csv_header_index(table, "name", &col) == GTEXT_CSV_OK) {
    const char * name = gtext_csv_field(table, 0, col, &len);
    printf("%.*s\n", (int)len, name);
  }

  gtext_csv_free_table(table);
  return 0;
}
```

```
ghoti
```

Row 0 is the first data row. Without `treat_first_row_as_header`, that same
row would have been the words `name` and `version`.

### YAML

```c
#include <ghoti.io/text/yaml.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char * src = "name: ghoti\nversion: 0\n";
  GTEXT_YAML_Error err = {0};

  GTEXT_YAML_Document * doc = gtext_yaml_parse(src, strlen(src), NULL, &err);
  if (!doc) {
    fprintf(stderr, "%s at line %d, column %d\n",
        err.message, err.line, err.col);
    gtext_yaml_error_free(&err);
    return 1;
  }

  const GTEXT_YAML_Node * name =
      gtext_yaml_mapping_get(gtext_yaml_document_root(doc), "name");
  const char * text = name ? gtext_yaml_node_as_string(name) : NULL;
  if (text) {
    printf("%s\n", text);
  }

  gtext_yaml_free(doc);
  return 0;
}
```

```
ghoti
```

`gtext_yaml_parse()` reads the first document. A stream of several is
`gtext_yaml_parse_all()`.

More programs live in `examples/`: streaming, building a document by hand,
JSON Pointer, JSON Patch, and JSON Schema.

## Compile and link

Once the library is installed, pkg-config carries the include path, the
library, and its dependencies:

```bash
cc -o show show.c $(pkg-config --cflags --libs ghoti.io-text-0)
```

The module name ends in the major version, `-0` for this release, so two
majors can be installed side by side. A build made with `make BRANCH=-dev`
installs `ghoti.io-text-dev` instead.

## Building the library

[cutil](https://github.com/Ghoti-io/cutil),
[chron](https://github.com/Ghoti-io/chron) and
[unicode](https://github.com/Ghoti-io/unicode) must already be installed
where pkg-config can see them. A dependency it cannot find is a hard error
naming the fix.

```bash
make
make test
sudo make install
```

From the parent of a suite checkout, which installs cutil, chron, and unicode first:

```bash
./suite/install.sh
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/text test PREFIX="$PWD/.local"
```

`make test` is the suite. `make help` lists the rest. The ones that reach
outside this repository:

| Target | What it does |
| --- | --- |
| `make conformance` | YAML against yaml-test-suite |
| `make conformance-json` | JSON against JSONTestSuite |
| `make conformance-csv` | CSV against csv-spectrum |
| `make conformance-jsonpath` | JSONPath against its compliance suite |
| `make fuzz` | Build and run the parsers' fuzzers |
| `make docs` | The Doxygen manual, into `./docs` |

The conformance targets clone their corpora on first use. Google Test is
required only to build the tests, and clang only to build the fuzzers.

## The API

Each format has one header: `<ghoti.io/text/json.h>`,
`<ghoti.io/text/csv.h>`, `<ghoti.io/text/yaml.h>`. Everything is prefixed
`gtext_` / `GTEXT_`.

**Two ways to read.** The DOM functions take a whole buffer and return an
owned tree: `gtext_json_parse()`, `gtext_csv_parse_table()`,
`gtext_yaml_parse()`. The streaming parsers take input in chunks of any size.
Each format also has a pull reader, where the caller asks for the next
event.

**Writing** mirrors reading: serialise a document, or drive a streaming
writer with events.

**Allocation.** Pass a `GTEXT_Allocator` (cutil's `GCU_Allocator` under this
library's name) or `NULL` for the default. The document owns what the parse
allocated; the format's free function releases it. `NULL` is safe to free.

[Formats](#formats) is what is implemented.
[Before you call it](#before-you-call-it) is what that changes about a call.

## Dependencies

All four are found through pkg-config, and the installed `.pc` file names
them, so a program that links `ghoti.io-text-0` links these too.

- [ghoti.io-cutil](https://github.com/Ghoti-io/cutil) — the allocator every
  parse and every owned document goes through.
- [ghoti.io-chron](https://github.com/Ghoti-io/chron) — YAML's `!!timestamp`,
  and JSON Schema's `date`, `date-time`, `time` and `duration` formats.
  `chron.h` is included from the YAML DOM header, so a program that reads a
  timestamp gets the type.
- [ghoti.io-unicode](https://github.com/Ghoti-io/unicode) — normalisation and
  the character properties JSON5 names, JSON5 whitespace and IDNA need. It
  is a link dependency: it does not appear in a public header.
- [ghoti.io-regex](https://github.com/Ghoti-io/regex) — I-Regexp for JSONPath
  `match()` and `search()`. It is a link dependency: it does not appear in a
  public header. JSON Schema's `pattern` keyword is a different dialect and
  still takes the caller-supplied provider.

## Documentation

The manual is `make docs`. The pages worth reading as files:

| Page | What it settles |
| --- | --- |
| \ref text_format_references "formats.md" | Which specification each parser implements |
| \ref format_json "json.md" | RFC 8259, the extensions, Pointer, Patch, Schema |
| \ref format_csv "csv.md" | RFC 4180 and what each dialect option does |
| \ref format_yaml "yaml.md" | YAML 1.2.2, and where this parser departs from it |
| \ref text_modules "modules/" | The API: types, functions, options |

The format pages are the authority for what a given byte sequence does. The
module pages are how to call it.

## Status

JSON, CSV and YAML parse and write. YAML's specification is much larger
than the other two, and passing its corpus is a statement about those
documents;
\ref format_yaml "yaml.md" says where
that stops.

What is still open is on the JSON side. Streaming LAST_WINS and COLLECT
still deliver every member of a repeated name. The writer, JSON Pointer,
JSON Patch and JSON Schema do not take a caller allocator.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
