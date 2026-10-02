@page format_comparison Comparison

# Comparison

The goal this page measures against is a specific one: that a project could
choose `ghoti.io/text` *instead of* libyaml, RapidJSON, libcsv or PyYAML, and
not give anything up that it depended on. That is a higher bar than "parses
the format correctly", which the \ref text_format_references "format pages" already
cover. This page is about what a migrating caller would find missing.

Everything below was checked by compiling and running against the library, not
by reading headers. Where a number appears, the program that produced it is
named. Claims about *other* libraries are from their published feature sets
and are marked as such; only this library and the Python standard library were
measured here, on one machine, in one sitting.

---

## What would block an adoption today

| # | Finding | Scope |
|---|---|---|
| 1 | JSON parses at roughly a third of Python's stdlib speed | JSON |
| 2 | The YAML and CSV writers' working memory takes no caller allocator | YAML, CSV |
| 3 | Streaming JSON LAST_WINS and COLLECT still deliver every repeated name | JSON |
| 4 | There is no YAML schema validator | YAML |

The library is LGPL-3.0-only. libyaml, RapidJSON and PyYAML are MIT. LGPL is
the license a commercial license can sit beside; a permissive license leaves
nothing to sell. An adopter who needs MIT terms is a customer for that
license.

## Throughput

Measured with a 3.7 MB JSON document of 20,000 records and a 12.4 MB CSV of
200,000 rows, best of three runs each, library built at `-O2`, compared
against the Python standard library on the same machine and the same files.

| Parser | This library | Python stdlib | Ratio |
|---|---|---|---|
| JSON to DOM | 57.5 MB/s | 151.5 MB/s (`json.loads`) | 0.38x |
| CSV to DOM | 101.8 MB/s | 110.7 MB/s (`csv.reader`) | 0.92x |

CSV is at parity with the Python C implementation. JSON is 2.6 times slower
than Python's standard library. RapidJSON and yyjson are positioned an order
of magnitude above this, and simdjson one beyond that. A caller choosing a
JSON parser on speed will not choose this one.

A `gprof` profile of both parsers shows no single dominant hotspot. The cost
is spread across per-byte state-machine work. Two observations, neither
confirmed by changing the code and re-measuring:

- The CSV table parser runs on top of the streaming parser, paying an event
  callback per field and a position update per byte, at 17.4 million calls to
  `csv_stream_advance_position()` for the 12.4 MB input.
- `json_get_limit()` is called 1.8 million times parsing 3.7 MB, a limits
  lookup on the hot path.

## Allocators

`GTEXT_Allocator` is cutil's `GCU_Allocator`. `gtext_allocator_default()`
returns `gcu_allocator_default()`.

A caller allocator covers every parse entry point in JSON, CSV and YAML: the
arena, the DOM or table, and, for CSV, every later operation on that table.
YAML's coverage includes the scanner, the alias table, DOM manipulation and
`gtext_yaml_to_json()`. `make check-allocators` fails the build if a
converted file calls `malloc`, `calloc`, `realloc`, `free`, `strdup` or
`strndup` directly.

Still open: the JSON writer, streaming parser, Pointer, Patch and Schema.
In all three formats, error structures and writers stay on the C library, and
there is no path on which the two allocators mix. `gtext_yaml_parse_all()`'s
array of document pointers is released with `free()`, which is its published
contract.

RapidJSON makes the allocator a template parameter, yyjson takes an
allocator struct, jansson has `json_set_alloc_funcs()`, and libyaml lets the
caller control the emitter buffer.

## Parsing models

| Format | DOM | Push (feed) | Pull (next) |
|---|---|---|---|
| JSON | yes | `gtext_json_stream_feed()` | `gtext_json_reader_next()` |
| CSV | yes | `gtext_csv_stream_feed()` | `gtext_csv_reader_next()` |
| YAML | yes | `gtext_yaml_stream_feed()` | `gtext_yaml_reader_next()` |

Each pull reader copies an event's bytes into a queue. The push callback's
pointer does not outlive the callback.

## Thread safety

No object is thread-safe. Distinct objects share nothing and may be used
concurrently. The rule, including version accessors and number formatting, is
in section 7 of the \ref core_module "Core module page", with a per-module
statement on the JSON, CSV and YAML pages.

## JSON

Compared against nlohmann/json, RapidJSON, jansson and cJSON.

**Present.** DOM with typed accessors; push streaming and a pull reader; a
writer with buffer and fixed-buffer sinks; file read and write; JSON Pointer
(RFC 6901); JSON Patch (RFC 6902); JSON Merge Patch (RFC 7386); JSONPath
(RFC 9535) with the filter selector, normalized paths, and `match()` and
`search()`; duplicate-key policy with four modes;
number handling that keeps the original lexeme; in-situ parsing; depth,
string, element and total-size limits; JSONC and JSON5 as opt-in options;
canonical output with sorted keys; errors with offset, line, column and a
snippet. JSON Schema 2020-12, 2019-09, draft-07, draft-06 and draft-04, refusing a
schema it cannot enforce. `normalize_unicode`. `gtext_json_to_yaml()`.

The patch and pointer set, the duplicate-key modes and the preserved lexeme
are uncommon among C JSON libraries.

**Missing.**

- A caller allocator on the writer, the streaming parser, Pointer, Patch and
  Schema. Parsing has one.
- LAST_WINS and COLLECT in the streaming parser. ERROR refuses a repeated
  name and FIRST_WINS does not deliver the later member. The other two still
  emit every member, because a value already handed to the callback cannot be
  replaced.
- SIMD scanning, which is what the throughput gap is about.

## CSV

Compared against libcsv, Python's `csv` module and rapidcsv.

**Present.** A table DOM with row and column operations; irregular rows;
configurable dialect; dialect presets; `gtext_csv_sniff()`; push streaming
and a pull reader; a writer with Python's four quoting policies; file read
and write; in-situ mode; a caller allocator on the parse and the table;
errors with byte offset, line, column, row and column indices, and a
snippet.

**Missing, on purpose.**

- Type inference. Fields are bytes.
- Conversion to JSON.
- RFC 7111 fragment identifiers.

The writer and `GTEXT_CSV_Error` stay on the C library.

## YAML

Compared against libyaml, libfyaml, PyYAML and yaml-cpp.

**Present.** DOM, including `omap`, `pairs` and `set`; anchors and aliases
with an expansion limit; merge keys; complex keys; custom tags; comment
retention and scalar style on a parse-write cycle when `retain_comments` and
`pretty` are set; source location per node; binary and timestamp accessors;
multi-document parse and emit; a JSON fast path; a safe-mode option set;
push streaming and a pull reader; a YAML 1.1 resolution mode; conversion to
and from JSON; a caller allocator on the parse. This is the most complete of
the three formats.

libyaml discards comments, and yaml-cpp's support is partial. A tool that
rewrites a configuration file and must keep its comments is a case where
this library is the better choice.

**Missing.**

- Schema validation. `GTEXT_YAML_Schema` selects implicit typing (failsafe,
  JSON or core). It is not a validator, and there is no Kwalify or Rx.
- In-situ parsing. Over the suite's parsing documents, 70% of scalars are
  contiguous in the input and 30% of scalar bytes are. Folding, escapes and
  block indentation mean the text in the document is not the text of the
  value. The scanner also owns a buffer it mutates, and UTF-16 and UTF-32
  input is decoded into that buffer, so there is no caller buffer to point
  at. The DOM parser retains the decoded buffer for source locations.

## What this adds up to

On correctness and on breadth, the library is in good shape. YAML comment
and style preservation, JSON Patch and Pointer together with the
duplicate-key modes, and JSONPath with normalized paths are ahead of the
common alternatives.

What is left:

- **JSON throughput.** SIMD scanning is the item. The measurement to take
  first is where the time goes.
- **LAST_WINS and COLLECT on the streaming JSON parser.** ERROR and
  FIRST_WINS are enforced. The other two still deliver every member, because
  holding the object until it closes is what would make a stream able to
  replace a value it has already handed over.
- **The YAML and CSV writers' working memory** still comes from the C library.
  Their write options carry no allocator, where JSON's, TOML's and INI's do; a
  buffer sink is exempt by design, a writer's scratch memory is not. Every JSON
  entry point takes one now, the writer, streaming parser, Pointer, Patch and
  Schema included. The \ref format_allocator_todo "allocator page" tracks what
  is left.

---

Back to \ref text_format_references "Format and specification references".
