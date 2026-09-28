@page text_format_references Formats


One page per format. Each says which external specification the parser
implements, which parts of it, where the implementation is deliberately
stricter or accidentally different from the reference parsers, and how every
claim on the page was checked. They are the authoritative reference for
grammar, option semantics and rejection behavior - ahead of the module pages,
which describe the API, and ahead of the source comments, which point back
here.

The distinction matters when reading: \ref json_module "JSON", \ref csv_module
"CSV", \ref yaml_module "YAML" and \ref toml_module "TOML" under
\ref text_modules "Modules" tell you how to call the library. The pages here tell you what it will do with a given byte
sequence, and how confident anyone should be about that.

## Formats

| Format | Page | Specification | Read | Write |
|---|---|---|---|---|
| JSON | @subpage format_json "JSON" | RFC 8259 / ECMA-404, plus RFC 6901, 6902, 7386, 9535, and JSON Schema 2020-12, 2019-09, draft-07, draft-06 | strict RFC 8259. JSONC and JSON5 are separate options, off by default | compact, pretty, and a canonical mode |
| CSV | @subpage format_csv "CSV" | RFC 4180, plus configurable dialects | RFC 4180 and looser dialects; irregular rows | RFC 4180 with configurable quoting |
| YAML | @subpage format_yaml "YAML" | YAML 1.2.2, with a 1.1 resolution mode | block and flow, anchors, tags, multi-document | DOM and streaming event serialization |
| TOML | @subpage format_toml "TOML" | TOML v1.0.0 (2021-01-12) and the v1.1.0 draft behind an option; date-times delegated to ghoti.io-chron | the whole 1.0.0 grammar read and written, 1.1.0 read, a statement-by-statement event walk, comments through a write, JSON both ways; 3,377 of 3,377 and 3,446 of 3,446 toml-test measurements | no 1.1.0 reference outside the corpus; no incremental reader (argued, not pending); no corpus behind the JSON-to-TOML direction |

A cross-format audit against the libraries these are meant to replace is in
\ref format_comparison "Comparison with other libraries".

INI is planned and has no parser, so it has no page here. A page is written
with the parser, not after it. `GTEXT_YAML_MODE_CONFIG` is a YAML parse preset,
not a parser for either format.

## What each page contains

Every page answers the same questions in the same order, so that two formats
can be compared by reading the same heading twice. A page omits a section it
has nothing to put under.

- **Normative references** - the specification, its version, and the parts of
  it linked individually where the document is split up.
- **Parts implemented** - the grammar productions, option semantics and
  extensions, each citing the clause it comes from.
- **Save behavior** - what the writer chooses, and why that choice rather
  than another.
- **Compliance checklist** - a table of area, what is supported, and what is
  rejected with which status code.
- **Deviations** - where the parser is stricter than a reference parser, or
  where it differs because it is wrong. Named cases, each pinned by a
  reproduction.
- **Tested scope** - the fixtures, how they were generated, the external
  oracles and the reach of each, the fuzz harnesses, and the gaps.
- **Not implemented** - what is absent, listed so it is visible rather than
  discovered. The JSON page calls this section Gaps.

Format-independent concerns - the result codes, the allocation and ownership
contract, the limits - are described in the
\ref core_module "Core module page"; the pages here cover only what a given
format does with them.

## A note on confidence

JSON has a small, closed grammar. Its page is checked case by case, including
JSONTestSuite and the JSON Schema test suite. CSV has no single grammar, so
its page spends its length on what the dialect options mean, and csv-spectrum
is scored. YAML's specification is the largest of the three. `make
conformance` scores yaml-test-suite at 395 of the 395 cases it can check.
That figure is a statement about those documents; the YAML page says where
the suite ends. The deviations that page records are closed.

## Comparison with other libraries

@subpage format_comparison "Comparison with other libraries" asks a different
question from the pages above. They ask whether a parser implements its
specification; that page asks what a caller migrating from libyaml, RapidJSON,
libcsv or PyYAML would find missing, and ranks the answers by how many
adoptions each one blocks. Its findings are measured rather than surveyed, and
two of them - the absent license, since resolved by moving the whole suite to
LGPL-3.0-only, and the unoptimized default build - are suite-wide rather than
particular to this library.

## Work in progress

@subpage format_allocator_todo "Caller allocators" records what remains. CSV and
YAML parses take a caller allocator. The JSON writer, the streaming parser,
JSON Pointer, JSON Patch and JSON Schema do not yet. A parse option that
covers an arena but not the structure around it is heap corruption for
anyone who uses it, which is why the work stopped short of a half-finished
option.

## Adding a format

@subpage text_format_adding "Adding a format" is the checklist and the page template:
what a new parser's documentation has to answer before the parser is
considered done, and the skeleton to copy.
