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
| TOML | @subpage format_toml "TOML" | TOML v1.0.0 (2021-01-12) and the v1.1.0 draft behind an option; date-times delegated to ghoti.io-chron | the whole 1.0.0 grammar read and written, 1.1.0 read, a statement-by-statement event walk, comments through a write, JSON both ways; 3,377 of 3,377 and 3,883 of 3,883 toml-test measurements, a pinned `tomllib` over 60,000 generated documents, and toml++ over the four v1.1.0 relaxations it shares | no comment inside a value on the tree; no incremental reader (argued, not pending); the two v1.1.0 unquoted-key relaxations |
| INI | @subpage format_ini "INI (Desktop Entry)" | freedesktop.org Desktop Entry Specification 1.5 (2020-04-27); a derived generic dialect for the long tail; `git-config(1)`; EditorConfig 0.17.2; `systemd.syntax(7)`; Python `configparser`, which has none; the Win32 profile API, which has none and no reference that is Windows | the whole of sections 3, 4 and 5, comments and unknown keys kept on the tree, values decoded at the accessor as both references do; 202 of 202 real `.desktop` files on this machine parse, rewrite byte for byte, and read identically under both dialects; **34 of 34** of `editorconfig-core-test`'s grammar assertions, where both reference cores score 33; 165 of 165 systemd unit files parse and rewrite byte for byte; 703 of 703 `.cfg` and `.ini` files get the same verdict as `configparser` itself, which refuses 224 of them; 701 real `.ini` and `.cfg` files agree with wine's Win32 profile API over 118,790 value comparisons and 214,774 lookups, plus 22 files the profile API itself authored and 162 documents re-encoded as UTF-16 in both byte orders | byte-identical by default, or normalized; refuses a value the dialect cannot spell rather than mangling it |

A cross-format audit against the libraries these are meant to replace is in
\ref format_comparison "Comparison with other libraries".

INI is the one format here whose *name* does not identify a grammar. There is no
INI specification, so the page above does not describe "INI": it describes the
freedesktop.org **Desktop Entry Specification 1.5 (2020-04-27)**, which is the
dialect the parser reads by default, plus a generic dialect defined as that
grammar with seven named changes - six relaxations and one CRLF
normalisation - plus **`git-config(1)`** and **EditorConfig 0.17.2**, each a
grammar of its own rather than a derivation of either: both accept documents
Desktop Entry refuses and refuse documents Desktop Entry accepts. A parser calling
itself simply "INI" would have nothing to be scored against but its author's
choices; a parser reading a named dialect has a specification, a reference
implementation and a corpus, and the page records where they disagree.

**Every named dialect is now implemented**, and the seven differ in how much a clean
score is worth. The page says so rather than presenting one number - **six distinct
situations**, which is the most useful thing the INI work has to say about oracles:

- **Desktop Entry has two reference implementations that disagree with each
  other**, so agreeing with both is a real constraint, and the sharing between them
  is incidental: one community, two codebases.
- **git config has one.** The same program decides legality and values, so
  agreement there cannot be told from being wrong the same way, and what stands in
  for a second opinion is a written statement of each rule that the gate scores
  separately.
- **EditorConfig has two that disagree *and* a normative conformance suite that
  both of them fail** - 33 of 34 each, for one shared reason, since both descend
  from Python's `ConfigParser`. So the specification gets the vote, agreeing with
  the references everywhere would be a failure, and the gate's third score asserts
  that each known departure is *still there* rather than ignoring it.
- **systemd has one reference whose instrument answers a different question than it
  appears to.** `systemd-analyze verify` **exits 0 on a syntax error** - it warns, skips
  the line and keeps the file - so a gate built on its exit status would score every
  broken document as legal and print clean. The diagnostics are the oracle, and values
  come back only through `Environment=`, whose reporting channel rewrites a carriage
  return and truncates a long message. Both limits live in the differential's
  exclusions. Whether that reference was usable at all was an open question for the
  whole of the INI work, and answering it was most of the effort for this dialect.

- **configparser has one reference and it is also the specification** - weaker than
  git's lone reference, because there `git-config(1)` at least exists to disagree with
  git. So two other things carry the weight: the generator's own reading of the
  dialect, scored as a separate `intent` number, and a local corpus of 703 real files
  of which the reference **refuses 224** - most `lit.cfg` files are Python scripts -
  which is the only place this format's refusals can be scored against something real.

- **Win32 has one reference that is not the thing it stands for, and it disagrees with
  itself.** wine is a reimplementation of the Windows API; no run here establishes what
  a Microsoft kernel32 does, and the gate prints that with every number. Two of the
  thirty rules have a second source in Microsoft's own documentation and twenty-eight
  do not, which is the weakest grounding of the seven. What partly offsets it is that
  the reference has three entry points and two of them disagree about the format's most
  consequential rule - `GetPrivateProfileSection` drops a `;` line and
  `GetPrivateProfileString` retrieves it - so it is a two-reference differential built
  from one implementation, scored both ways, with the disagreement **asserted** rather
  than resolved by quietly picking a side.

**Win32 `.ini` used to be listed here as deliberately absent**, on the grounds that
`GetPrivateProfileString` consults the registry before the file so its answer is not a
function of the file's bytes. That is true of the **API** and it disqualifies only the
claim to reproduce it. It is not true of the file, and reading the `.ini` files that
exist on Windows - which mostly belong to applications with their own parsers - is a
different question that was reachable the whole time. The registry redirection is now a
documented limit of the dialect rather than a reason not to have one.

**UTF-16 input was listed beside it and is not any more**, and the correction has a
different shape worth keeping. It was not a reason not to do something - it was a scope
boundary written as prose ("a caller transcodes, or asks for a decision"), which reads
like a decision and is a decision deferred. Nothing had checked what happened to such a
file, and what happened was that it parsed: one group with an empty name, keys made of
the file's own bytes with NULs between them, unconditionally, because the Win32 dialect
refuses nothing. A gap you can describe is a gap; a gap that returns a document is a
defect. UTF-16 is now refused by default and decoded on request, and the reference reads
a marked UTF-16 file the same way it reads the UTF-8 form - so the decode is measured
rather than asserted.

`GTEXT_YAML_MODE_CONFIG` is a YAML parse preset, not a parser for either format.

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

@subpage format_allocator_todo "Caller allocators" records how that went. Every
parse takes one, so does every JSON entry point - the writer, the streaming
parser and its pull reader, JSON Pointer, JSON Patch and JSON Schema - and so do
the YAML and CSV writers and, last of all, the bytes of the file itself for every
`*_parse_file()` entry point. Every `GTEXT_*_Options` structure in the library now
either carries an allocator or documents why it does not; the two that do not are
the YAML and TOML *to-JSON* converters, whose output is built by
`gtext_json_new_*` and so comes from the JSON module's own arenas. A buffer sink
stays exempt on purpose, because it is created before any allocator is named, and
`make check-allocator-callees` stops a converted file reaching memory through an
unconverted one.

A parse option that covers an arena but not the structure around it is heap
corruption for anyone who uses it, which is why each conversion was all or
nothing rather than a half-finished option.

## Adding a format

@subpage text_format_adding "Adding a format" is the checklist and the page template:
what a new parser's documentation has to answer before the parser is
considered done, and the skeleton to copy.
