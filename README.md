# Ghoti.io Text

Parsers and writers for structured text, in C. Each format below has a
document you can walk, a streaming parser, and a writer. They share one
contract for errors, limits and allocation.

## Formats

This is what the library implements.

- **JSON.** RFC 8259 / ECMA-404, plus JSON Pointer (RFC 6901), JSONPath (RFC 9535), JSON Patch (RFC 6902), JSON Merge Patch (RFC 7386), and JSON Schema (2020-12, 2019-09, draft-07, draft-06 and draft-04, each with its own keyword set and every required assertion of the suite answered). JSONC and JSON5 are each a set of options.
- **CSV.** RFC 4180, and other dialects through `GTEXT_CSV_Dialect`.
- **YAML.** YAML 1.2.2, including its Core, JSON and Failsafe schemas, plus a YAML 1.1 resolution mode and the 1.1 types `!!timestamp`, `!!set`, `!!omap`, `!!pairs` and the `<<` merge key.
- **TOML.** TOML v1.0.0, read and write, and the v1.1.0 draft read behind an option: the whole grammar, all four date-time types through `ghoti.io-chron`, comments kept on request, a statement-by-statement event walk, conversions to and from the JSON tree, and 3,377 of 3,377 against toml-test's 1.0.0 manifest with 3,883 of 3,883 against its 1.1.0 one - ten scores from the one corpus and twelve on the newer arm, including each document rebuilt from the event stream alone, every comment carried through a write and a second read, every case sent out to JSON and back, and a crossed mode that runs each version arm over the cases the other manifest drops and requires the wrong answer. The writer has no version *option*, which is a measurement rather than an omission: 1.1.0 adds spellings, not values, so the encode rows score 218 of 218 against its manifest unchanged. The spellings it adds are a style option instead - `\e`, `\xHH`, omitted `:00` seconds, a multi-line inline table and its trailing comma - five bits, off by default because each one produces a document a 1.0.0 reader refuses, and measured over the corpus two ways: the values survive a write and a second read, and the bytes change exactly where the strict arm then refuses them (52 of the 218 valid cases).
- **INI.** No such specification exists, so the dialect is named rather than guessed: the freedesktop.org **Desktop Entry Specification 1.5 (2020-04-27)**, read and written, with a generic dialect defined as that grammar plus seven named changes - six relaxations and one CRLF normalisation, separated because only the six carry the property that every document the strict dialect accepts reads identically under both. Sections 3, 4 and 5 in full - the `#`-only comment rule, the `A-Za-z0-9-` key charset, `\\s \\n \\t \\r \\\\` escapes, `;`-separated lists with the specification's awkward trailing-terminator rule, and the `key[lang_COUNTRY@MODIFIER]` fallback chain. Values are decoded at the accessor and not during the parse, which is where both reference implementations put it and what lets an unmodified document write back **byte for byte**, comments and unknown keys included, as section 3 requires. 202 of 202 real `.desktop` files on this machine parse, rewrite byte-identically, and read identically under both dialects. A third dialect is **`git-config(1)`'s**, which is not a relaxation of Desktop Entry in either direction - it accepts a preamble, comments anywhere on a line, backslash continuation, quoted runs that toggle, subsections in both spellings, valueless keys and repeated keys, and it *refuses* a key not beginning with a letter and a group name outside `A-Za-z0-9-.`. Compared against git itself over 20,000 generated documents: its accept/reject, every canonical name and value, every single-value lookup and every byte-identical rewrite agree, across 93 constructs. Generated rather than collected because the 25 git config files on this machine carry one of those constructs between them. A fourth dialect is **EditorConfig 0.17.2**, the only one of the four with a *normative conformance suite* - its specification requires a conforming core to pass `editorconfig-core-test` - and the only place this library scores higher than the reference implementations: **34 of 34** of that suite's grammar assertions, where `editorconfig-core-c` and `editorconfig-core-py` each score 33. Both fail the same one, and for one shared reason: both descend from Python's `ConfigParser` and truncate a value at a whitespace-preceded `#`, which the specification says is part of the value. So the two cores are used as differential oracles rather than as authorities, over 20,000 generated documents, with a third score that asserts each of their twenty measured departures from the specification is *still* there - a core fixed upstream fails loudly instead of quietly inflating the agreement.

A fifth dialect is **`systemd.syntax(7)`**, the widest of them: a backslash continuation that joins with a *space*, skips an intervening comment block and can appear in a section name or a key - so a name is stored as the document's bytes with the joined form beside it - plus the full C escape set including four variable-length numeric forms, a lone CR as a line terminator, and quoting that is explicitly *not* part of the grammar. Every rule was measured against systemd 257 in a pinned container, because this machine has no systemd at all while carrying 165 unit files: all 165 parse and write back byte for byte, and a generated population of 5,000 documents agrees with systemd on whether a line has a grammar fault and on how every value splits into words. That reference took four probe rounds to make usable - `systemd-analyze verify` exits **0** on a syntax error, so its diagnostics rather than its status are the oracle, and the only channel that reports a parsed value rewrites a carriage return and truncates a long message.

The sixth is **Python's `configparser`**, and it is the only one with **no specification at all** - the Python documentation describes what the module does rather than defining a format, and says so - so every rule of it is a measurement and none is a citation. Two of them needed the module's own source rather than a probe: the continuation is an *indented following line*, joined with a newline, compared strictly against the indent of the line the entry began on, and a key's separator is therefore sought on that line alone while every continuation line is appended without being read. Four axes of the dialect struct grew for it, each one a `bool` could not hold: two separator characters (`=` or `:`, whichever comes first), a third whitespace set (Python's `\s` includes the four ASCII separator controls and C's `isspace()` does not), a third answer for what follows a header's `]` (discard it, where Desktop Entry refuses and git reads an entry), and folded keys with unfolded section names. It is scored twice: **703 of 703** of this machine's `.cfg` and `.ini` files get the same verdict as `configparser` itself - and 224 of those it *refuses*, most `lit.cfg` files being Python scripts, which is the only place a real corpus of this format offers a refusal to score - and 20,000 generated documents agree with a pinned interpreter on every section, key and joined value. Interpolation is off by default and available on request, and the measurement is why it is that way round: over the 479 real documents here the *default* `BasicInterpolation` refuses a value in **301** of them and changes a value in **none**, so interpolating by default would read 63% of this machine's `.ini` files worse and none better. That argues for "not by default" and says nothing about offering it, which the docs claimed anyway for a while; `gtext_ini_value_interpolate()` is the narrower claim implemented, as an accessor rather than a dialect field. The gate now runs the reference three times - pinned, `BasicInterpolation`, `ExtendedInterpolation` - both to score that pass against what it reimplements and to **fail** if the pinned configuration stops keeping a behaviour out, which is the hole a printed pin left: an exclusion names a document and keeps the knowledge, while a pin removes a behaviour from the comparison.

The seventh and last is the **Win32 profile API**, and it is the one dialect here whose reference is not the thing it stands for: wine is a reimplementation of the Windows API, and no run on this machine establishes what a Microsoft `kernel32` does. It was recorded as "never ship" for a reason that was true and answered a different question - `GetPrivateProfileString` consults the registry's `IniFileMapping` for the section before reading the file, so the **API's** answer is not a function of the file's bytes, which disqualifies only the claim to reproduce that API. Reading the `.ini` files that exist on Windows is a different thing, those files mostly belong to applications that parse them themselves, and it was reachable all along. Thirty rules, every one measured, because the API's documentation states two. **The reference disagrees with itself about the most consequential rule in the format**: `GetPrivateProfileSection` discards a line whose first non-blank byte is `;` and `GetPrivateProfileString` retrieves it, so `;disabled=1` is a comment to one entry point and a live setting to the other - and `#` is a comment to neither. The dialect follows the enumeration API, because a reader handed back a setting its author commented out cannot be recovered from downstream, and the differential asserts the disagreement with the other half is still observable rather than quietly picking a side. Five struct fields grew for it, each a rule no `bool` already held: an empty key that a lookup can retrieve, a section name trimmed *inside* the brackets so `[ b ]` and `[b]` are one section, an unclosed `[a` that is not a malformed header but an ordinary valueless entry, one matching pair of surrounding quotes coming off a value as a *wrapper* where git's is a toggle, and a lookup that does **not** search a second `[a]` where every other dialect merges. It is the only one of the seven that **refuses nothing** - the key charset is open, the empty key and empty section name are both spellable, a line with no separator is a valueless entry - which the fuzzer asserts on every input it reads as bytes and the differential scores
over 83 axes. That qualification is the encoding check and not a rule of the grammar -
the fuzzer's assertion was unqualified for one round and failed on the first input
beginning `FF FE`, which is how it found out. Scored twice: 701 real `.ini` and `.cfg` files on this machine agree with the pinned reference over **118,790** value comparisons and **214,774** lookups, and that corpus is real bytes of the right shape from the wrong provenance, since this host has exactly two `.ini` files a Windows application wrote - which the gate prints where the number is read - and a second gate answers that as far
as this host can, by asking `WritePrivateProfileString` to **author** 22 files, whose
provenance is right by construction. The lookup score is there because the other scores walk the tree and never call `gtext_ini_document_get()`; adding it found three defects in one function, the worst of which made a section name with a dot in it unfindable by the spelling the file itself uses.

A Windows `.ini` that is **UTF-16** is read on request rather than by default, and the
default is the interesting half. Before `GTEXT_INI_E_ENCODING` existed such a file did
not fail to parse - it parsed, into one group with an empty name whose keys were the
file's bytes with NULs between them, and under the Win32 dialect that was unconditional
because the dialect refuses nothing. Set `decode_utf16` and the file is decoded to UTF-8
and read; leave it and a UTF-16 or UTF-32 byte-order mark is a refusal that names the
encoding. There is no content sniffing, and that is measured rather than chosen: given a
UTF-16 document with no mark the reference reads **nothing** from it, so a reader that
guessed would be reading a document the reference does not read.
\ref text_format_references "Formats" has the rest.

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
| `make conformance-json-schema-all` | The schema engine against every draft it reads |
| `make conformance-json-to-toml` | `gtext_json_to_toml()` over JSONTestSuite's documents |
| `make conformance-csv` | CSV against csv-spectrum |
| `make conformance-jsonpath` | JSONPath against its compliance suite |
| `make conformance-toml` | TOML against toml-test's 1.0.0 manifest |
| `make conformance-toml-next` | TOML against its 1.1.0 manifest |
| `make check-toml-oracle` | The TOML reader against a pinned `tomllib`, over generated documents |
| `make check-toml-1-1-oracle` | The v1.1.0 reader against toml++'s unreleased set |
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
still deliver every member of a repeated name.

Every **JSON** entry point now takes a caller's allocator: parsing, the
streaming parser and its pull reader, the writer, JSON Pointer, JSON Patch,
gtext_json_clone() and JSON Schema. Patch and clone inherit the allocator of the
tree they are given, because what they produce lives in that tree; Schema names
its own in GTEXT_JSON_Schema_Options, because a compiled schema outlives the
document it was built from.

The **YAML and CSV writers** were the last two, and are done:
GTEXT_YAML_Write_Options and GTEXT_CSV_Write_Options each carry an allocator now,
as JSON's, TOML's and INI's already did. A buffer sink is deliberately exempt - it
is created before any allocator is named - but a writer's own scratch memory is
not, and that is what was converted: the DOM writer's frame stack, the
anchors-written set, the event writer's handle, stack and %TAG handles, the CSV
escape buffer and the CSV writer's handle. \ref format_allocator_todo
"allocator-todo.md" has the per-file counts and the six controls.

Every GTEXT_*_Options structure in the library now either carries an allocator or
documents why it does not. The two that do not are GTEXT_YAML_To_JSON_Options and
GTEXT_TOML_To_JSON_Options: each routes its own frame stack through an allocator
but builds its output with the `gtext_json_new_*` constructors, which take none,
so an option there would read as covering a tree it could not reach.

Two allocations stay on the C library by design: `GTEXT_JSON_Error::context_snippet`
and its CSV equivalent, because gtext_json_error_free() is handed only the error
and cannot learn which allocator made the snippet, and the `gtext_json_new_*` DOM
builders, which take no options and so have no caller allocator to inherit.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
