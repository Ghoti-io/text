@page format_yaml YAML

# YAML

YAML is by far the largest of the three specifications this library
implements. This page says what is implemented, which limits are still
load-bearing, and how far the claim to 1.2 conformance has been measured.
`make conformance` scores 395 of the 395 yaml-test-suite cases it can check.
That is a statement about those documents; [Where the suite ends](#yaml-where-the-suite-ends)
is the rest. Back to the \ref text_format_references "format index".

Read \ref yaml_module "the YAML module page" for the API. This page is about
the format.

## Normative references

- **Specification:** [YAML 1.2.2](https://yaml.org/spec/1.2.2/), October
  2021. This is the current revision; it is editorial relative to 1.2.1 and
  clause numbers below refer to it.
- **YAML 1.1:** [yaml.org/spec/1.1](https://yaml.org/spec/1.1/), for the
  resolution rules the compatibility mode restores.
- **Core schema:** 1.2.2 §10.3. **JSON schema:** §10.2. **Failsafe schema:**
  §10.1.
- **Tags:** the `tag:yaml.org,2002:` namespace, §10.
- `!!timestamp`, `!!set`, `!!omap` and `!!pairs` are **not** part of the YAML
  1.2 core schema. They come from the
  [YAML 1.1 type repository](https://yaml.org/type/), which 1.2 does not
  carry forward. Their treatment here is described below and is this
  library's decision, not a specified behavior.

Parser allocations come from an arena owned by the `GTEXT_YAML_Document` and
released by `gtext_yaml_free()`. Error context snippets are separately owned
and released by `gtext_yaml_error_free()`.

## Parts implemented

**Structure.** Block and flow collections (§8, §7.4), nested to the depth
limit. Multi-document streams with `---` and `...` markers (§9). Explicit
keys (`?`), and complex keys behind `allow_complex_keys`.

**Scalar styles (§7, §8.1).** All five: plain, single-quoted, double-quoted,
literal (`|`) and folded (`>`), with the clip, strip (`-`) and keep (`+`)
chomping indicators. Folding and chomping were checked directly: `key: >`
over two indented lines yields `a b`, and `key: |` yields `a\nb`.

**Escapes (§5.7).** The full set, including `\xNN`, `\uXXXX` and
`\UXXXXXXXX`.

**Encoding (§5.2).** UTF-8, UTF-16 and UTF-32 with BOM detection and
transcoding to UTF-8. UTF-8 validation is on by default and is genuinely
wired, unlike CSV's. On the way out a byte order mark is a property of the
*stream*, not of a document: a multi-document write emits one, at the front,
however many documents it writes, and so does one streaming writer across
however many pass through it. The grammar would admit one per document -
`l-document-prefix` takes a mark at the head of each - which is why nothing
that compares values could have measured this;
`YamlEncoding.OneByteOrderMarkPerStreamNotPerDocument` counts the bytes.

**Directives (§6.8).** `%YAML` and `%TAG`, with tag handle resolution.
A `%YAML 1.1` directive switches resolution, as does the `yaml_1_1` parse
option.

**Schemas (§10).** `GTEXT_YAML_SCHEMA_CORE` by default, with `JSON` and
`FAILSAFE` available. Core resolution was checked directly: `true` resolves
to a bool node, `123` to int, `3.14` to float, and both `null` and `~` to
null.

These are *resolution* schemas - which plain scalar is a boolean - and have
nothing to do with validation, which is a confusion the shared word invites.
Validating a document's shape is `gtext_yaml_validate()`, against a JSON
Schema; see \ref yaml_schema_module "the YAML schema page".

Under 1.2 defaults, `yes` resolves to the **string** `"yes"` and `0755` to
the **string** `"0755"` - correct for 1.2, which removed 1.1's `y|yes|on`
booleans and leading-zero octals. So do `1_000`, `0b101` and `0O14`: digit
separators, binary literals and the upper-case base prefixes are all 1.1's,
and 10.3.2 admits only `[-+]? [0-9]+`, `0o [0-7]+` and `0x [0-9a-fA-F]+`.
Enabling 1.1 compatibility restores all of them, along with sexagesimals
(`190:20:30`), and the older ones emit a warning
(`GTEXT_YAML_WARNING_YAML11_BOOL`, `_OCTAL`, `_SEXAGESIMAL`) so that a
document relying on the old rules is visible rather than silent.

Each row of the table is a list of spellings rather than a word matched
without regard to case, so `tRue`, `nULL` and `.Nan` are strings. `%YAML 1.7`
is parsed as 1.2 with a `GTEXT_YAML_WARNING_YAML_VERSION`; `%YAML 2.0` is
refused, since 6.8.1 has a processor decline a major version it does not
implement.

**Anchors and aliases (§6.9, §7.1).** `&anchor` and `*alias`, with cycle
detection and a total-expansion limit that bounds the billion-laughs attack.
An alias may stand where a block mapping's key does - `ns-flow-yaml-node`
begins with `c-ns-alias-node` (§8.2.2) - with or without a value: `*b : *a`
and `*b :` are both entries, the second one keyed by whatever `&b` named and
valued by the empty node.

**Merge keys.** The `<<` key from the 1.1 type repository, on by default via
`allow_merge_keys`. Verified: `a: &A {x: 1}` merged into a mapping that also
sets `y` yields both keys. `gtext_yaml_document_has_merge_keys()` reports
whether a document used them.

**Tags.** `!!str`, `!!int`, `!!float`, `!!bool`, `!!null`, `!!seq`, `!!map`
from the core schema. From the 1.1 type repository, with the limits noted:

- `!!binary` is base64-decoded. The node remains a string node - its string
  accessor returns the base64 source text - and `gtext_yaml_node_as_binary()`
  returns the decoded bytes. Writing re-emits the base64 form.
- `!!timestamp` is read by [`chron`](https://github.com/Ghoti-io/chron),
  which implements the YAML 1.1 type repository's own expression. The node
  stays a string node - its string accessor returns the **normalized**
  spelling, `YYYY-MM-DDTHH:MM:SS` with the shortest fraction that loses
  nothing - and the value is reached through
  `gtext_yaml_node_timestamp_value()`, which hands back a `GCHRON_YamlValue`.
  `gtext_yaml_node_as_timestamp()` remains as a flattened view of the same
  thing.

  Three things are worth knowing. **A timestamp with no zone is not UTC**: the
  document did not say which zone it meant, and `GCHRON_YAML_DATE_TIME` keeps
  the civil reading rather than deciding for it - `chron`'s `zoned.h` is where
  a caller who knows the zone resolves it, and where the conversion can report
  that the reading names no instant, or two. No timezone conversion happens
  here. **A `:60` second is kept exactly as written**, because the value holds
  it as `:59` of the same minute and re-emitting that would move the reading a
  second earlier; `gtext_yaml_node_timestamp_is_leap_second()` says so.
  And this library does **not** resolve timestamps implicitly - an untagged
  `2001-12-14` is a string - so `!!timestamp` is the only way in.
- `!!set` validates that mapping values are null. `!!omap` and `!!pairs`
  validate that entries are single-pair mappings, and `!!omap` enforces
  unique keys. Distinct `GTEXT_YAML_Node_Type` values exist for all three.
  `gtext_yaml_to_json()` renders them as the structures they already are - a
  set as an object with null values, an omap or pairs as an array of
  single-pair objects, which keeps an omap's order and a pairs' duplicate
  keys. Only the tag is lost, as it is for every tagged node.

Custom application tags are supported behind `enable_custom_tags`, with
constructor, representer and JSON-converter callbacks.

**Parsing models.** A one-shot DOM parser, an event-driven streaming parser,
and a pull-model reader. The DOM supports accessors, mutation, sequence
insert/append/remove, and deep cloning.

**YAML to JSON.** `gtext_yaml_to_json()` converts a document, with options
governing how the YAML-only constructs that JSON cannot express are handled.

## Parse options and limits

| Option | Default | Safe mode |
|---|---|---|
| `schema` | `CORE` | `CORE` |
| `dupkeys` | `ERROR` | `ERROR` |
| `max_depth` | 256 | 64 |
| `max_total_bytes` | 64 MiB | 16 MiB |
| `max_alias_expansion` | 10,000 | 1,000 |
| `validate_utf8` | `true` | `true` |
| `resolve_tags` | `true` | `true` |
| `retain_comments` | `false` | `false` |
| `yaml_1_1` | `false` | `false` |
| `allow_aliases` | `true` | **`false`** |
| `allow_merge_keys` | `true` | **`false`** |
| `allow_complex_keys` | `true` | **`false`** |
| `allow_nonstandard_tags` | `true` | **`false`** |
| `require_string_keys` | `false` | **`true`** |
| `enable_custom_tags` | `false` | `false` |
| `enable_json_fast_path` | `true` | `true` |

`gtext_yaml_parse_options_safe()` returns the right-hand column. It is the
correct starting point for untrusted input: it removes aliases entirely,
which is the only complete defense against expansion attacks, and it refuses
non-string keys, which is what most consumers assume anyway.

### Tags

Two rules govern tags, and only one of them is an option.

A tag in the `tag:yaml.org,2002:` namespace has to name a type the spec
defines. `!!bogus` is a malformed document and is refused whatever the
options say, because that namespace is not the author's to extend. The
types this library resolves there are `str`, `bool`, `int`, `float`,
`null`, `seq`, `map`, `set`, `omap`, `pairs`, `binary`, `timestamp` and
`merge`; `!!value` and `!!yaml` are named by the 1.1 type repository but
are not among them, and are refused for the same reason.

Everything else - a local tag like `!point`, or a global one under your own
prefix - is what tags exist for, and is accepted by default. The spec's own
examples use them freely. `allow_nonstandard_tags = false` refuses them, and
that is the setting's whole job: it is a lockdown for input you do not
trust, not a correctness rule, and turning it on for ordinary documents will
refuse valid YAML.

A `%TAG` directive is expanded before either rule is applied, so a handle
redirected away from the YAML namespace escapes the first rule and a handle
pointed into it does not.

The event API (`gtext_yaml_stream_*`) reports tags as written and resolves
nothing, so neither rule applies there; a consumer of events decides what a
tag means for itself.

## Save

The default write options are UTF-8 with no BOM, two-space indent, plain
scalars, `line_width` 0 (no folding), `pretty` off, and `flow_style`
`GTEXT_YAML_FLOW_STYLE_AUTO`. With `pretty` off, `AUTO` writes the document
in flow style. A flow collection has nowhere to put a comment on a line of
its own, and no spelling for a block scalar, so a rewrite that does not ask
for `pretty` comes back as `{key: value, ...}` and loses the leading
comments.

Comments and scalar style survive when `retain_comments` was set at parse
time and `pretty` is set on the write. `pretty` with `AUTO` writes block
style, except for a collection that is empty, anchored, tagged, or already
inside a flow collection. `tests/yaml/test-yaml-comment-roundtrip.cpp` pins
both halves.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|---|---|---|
| Block collections | yes | |
| Flow collections | yes | |
| All five scalar styles | yes | |
| Chomping indicators | clip, strip, keep | |
| Multi-document streams | yes | |
| `%YAML` / `%TAG` | yes | |
| Core / JSON / Failsafe schema | yes | |
| YAML 1.1 resolution | opt-in, warns | |
| Anchors and aliases | yes, cycle-detected | `GTEXT_YAML_E_LIMIT` past `max_alias_expansion` |
| Merge keys | yes, default on | |
| `!!binary` | decoded | via a separate accessor |
| `!!timestamp` | parsed, by `chron` | `GCHRON_YamlValue`; normalized on output; explicit tag only |
| `!!set` / `!!omap` / `!!pairs` | validated, own node types | convert to JSON structurally |
| UTF-16 / UTF-32 input | yes, transcoded | |
| Duplicate keys | `ERROR` default | `GTEXT_YAML_E_DUPKEY` |
| Depth | 256 default | `GTEXT_YAML_E_DEPTH` |
| Comment round-trip | with `retain_comments` and `pretty` | dropped under the default flow style |
| Scalar style round-trip | with `pretty`, including folded and literal | flow style has no block scalar |
| Plain scalars containing `-`, `,`, `?`, `#` | yes | `: ` and ` #` end the scalar |
| Multi-line plain scalars | yes, in block context | folded to spaces; blank lines give breaks |
| Multi-line plain scalars in flow | yes | `[a` over an indented `b]` is one scalar |
| Flow plain scalars with spaces | yes | `[a - b, c]` is two entries |
| Block scalar chomping and folding | yes | clip, strip and keep; indentation indicator honoured |
| Single-pair mappings in flow (`[a: 1]`) | yes | equivalent to `[{a: 1}]` |
| Tabs inside a plain scalar | yes | 1.2 allows them; PyYAML, a 1.1 parser, does not |

## Limits

**A null cannot survive a round trip through the failsafe schema.** That
schema resolves nothing, so `null` is written and the string `"null"` comes
back. There is no spelling the failsafe schema reads as a null.

**Every `offset` this module reports**, on an error, a warning, an event, or
a node's source location, counts bytes of the decoded character stream, not
of the buffer passed in. They are the same number for UTF-8 with no
byte-order mark, and for nothing else: a mark moves them by three, and
UTF-16 and UTF-32 have no byte-for-byte relation to it. `line` and `col` are
counted in characters and are right in every encoding.

@anchor yaml-where-the-suite-ends
## Where the suite ends

`make conformance` scores yaml-test-suite at 395 of the 395 cases it can
check, out of 406 shipped. The other eleven carry no expectation, or one the
harness cannot decode. Passing that corpus is a statement about those
documents.

Shapes the suite never contains are in `tests/data/yaml/spec-1.2.2.corpus`,
and `make test` scores that file. It covers case-sensitive core-schema
spellings (`tRue`, `nULL`, `.Nan` are strings), YAML 1.1 integer forms under
1.2 (`1_000`, `0b101`, `0O14` are strings), a tab separating a directive
from its argument, one `%TAG` per handle, `%YAML 2.0` refused, a directive
prologue that must be followed by `---`, an alias that names only a
preceding anchor, `c-printable`, and where a byte-order mark may stand.

`make conformance` asks for `GTEXT_YAML_DUPKEY_KEEP_ALL`, which turns off
the JSON fast path inside `gtext_yaml_parse()`. The 395 does not include
that path. `make conformance-fastpath` compares the two paths over the suite
and reaches five documents, because the rest are not JSON.
`tests/yaml/test-yaml-json-fastpath.cpp` is the check that the two agree,
and that a quoted scalar stays a string.

The suite is a corpus of inputs. It does not test the writer.
`make conformance-roundtrip` runs it backwards.

@anchor yaml-tested-scope
## Tested scope

**Tests.** 104 test files under `tests/yaml/`. They cover the scalar styles,
collections, anchors and aliases including the cycle and
exponential-expansion cases, merge keys, the tag types, directives,
multi-document streams, UTF-8 and the other encodings, the DOM accessors and
mutation, cloning, the writers, the pull reader, chunked scanning, partial
input, the limits, safe mode, 1.1 mode, config mode, and YAML-to-JSON
conversion. `tests/yaml/test-yaml-real-world.cpp` parses Docker Compose,
Kubernetes and GitHub Actions shapes.

**Fixtures.** `tests/data/yaml/` holds the formatting files, one binary
regression case, and `spec-1.2.2.corpus`. Most tests carry their YAML inline
as string literals.

**Fuzzing.** Two harnesses under libFuzzer with ASan and UBSan.
`tests/fuzz/fuzz_yaml.cpp` parses and walks. `tests/fuzz/fuzz_yaml_writer.cpp`
holds the writers to this: if the writer says OK, the bytes it wrote must
parse, and must hold the same values. It reaches documents that came from
parsing, documents built through the DOM API, and the streaming parser
feeding the streaming writer with no DOM in between.

**Memory.** The suite runs clean under valgrind and under ASan/UBSan, with
`-fno-sanitize-recover=undefined` so a finding fails the run it is found in.

**What a score does not say.** The same harness scores js-yaml and PyYAML
alongside this parser. Nothing fails a build on a disagreement with either.
Fuzzing finds crashes and a writer whose output this parser refuses. It
does not find a parser that returns the wrong string. Parsing speed and
memory use are unmeasured.

## JSON and YAML

`gtext_json_to_yaml()` is the reverse of `gtext_yaml_to_json()`. YAML 1.2
section 10.2 makes JSON a subset of YAML, so the conversion cannot fail on
the grammar; it can fail on `max_depth`.

Types are preserved rather than re-resolved. YAML resolves a plain scalar by
its contents, so a JSON string reading `true`, `null`, `42`, `1.5`, `~` or
`yes` would change type if it were written plain. Every string becomes a
node explicitly typed `GTEXT_YAML_STRING`, object names included, and the
writer quotes what needs quoting. Numbers keep the lexeme the JSON parser
preserved, so `1.0`, `1e3` and an integer of thirty digits survive as
written.

One case is lossy: a JSON integer too large for an `int64` has no YAML
integer node here, and becomes a string. That is what this library's own
YAML parser does with the same digits.

## Not implemented

- **Benchmarks.** Parsing speed and memory use are unmeasured. Treat the parser as suitable for configuration-sized documents.
- **Native Windows (MSVC)** is untested; MSYS2/MinGW is exercised.

@anchor yaml-status
## Status

Alpha. The API may change before 1.0. The parser is appropriate for configuration files from sources you control, and for prototyping. For untrusted input use `gtext_yaml_parse_options_safe()`, which bounds resource consumption.

`make conformance` runs
[yaml-test-suite](https://github.com/yaml/yaml-test-suite) against this
parser. Of the 395 cases it can check, those carrying a `json` field are
checked by value, those carrying a `tree` by event stream, and those marked
`fail` by refusal. All 395 pass. The eleven left over carry no expectation,
or one the harness cannot decode. The figure to quote is 395 of the 395
checkable, out of 406 shipped. The same harness scores js-yaml at 82.0% and
PyYAML at 77.3% on the value cases.

---

Back to \ref text_format_references "Format and specification references".
