@page format_json JSON

# JSON

The JSON parser implements RFC 8259 strictly by default, and relaxes toward
the JSONC dialect only when explicitly asked. Everything claimed below was
checked against the built library rather than read off the source, and the
cases that were checked are listed under
[Tested scope](#json-tested-scope). Back to the
\ref text_format_references "format index".

## Normative references

- **Specification:** [RFC 8259](https://www.rfc-editor.org/rfc/rfc8259),
  *The JavaScript Object Notation (JSON) Data Interchange Format*, December
  2017. Identical in grammar to
  [ECMA-404, 2nd edition](https://ecma-international.org/publications-and-standards/standards/ecma-404/).
- **JSON Pointer:** [RFC 6901](https://www.rfc-editor.org/rfc/rfc6901), April
  2013.
- **JSON Patch:** [RFC 6902](https://www.rfc-editor.org/rfc/rfc6902), April
  2013.
- **JSON Merge Patch:** [RFC 7386](https://www.rfc-editor.org/rfc/rfc7386),
  October 2014.
- **JSON Schema:** still **names no draft**, but now covers most of
  draft-07 and 2020-12 including `$ref`, and refuses any schema it cannot
  fully enforce - see [Deviations](#json-deviations).
- **JSON5:** [json5.org](https://json5.org/), version 1.0.0, whose numeric
  and string grammars are ECMAScript's. Its extensions are opt-in and named
  individually below, one option per difference rather than one dialect flag.
- **JSONC:** no specification exists. The extensions are opt-in and named
  individually below; where the dialect is ambiguous this page states what the
  parser does, and that decision is the specification as far as this library
  is concerned.

All parser allocations are owned by the returned `GTEXT_JSON_Value` and
released by `gtext_json_free()`. Error context snippets are separately owned
and released by `gtext_json_error_free()`.

## Parts implemented

**Values (RFC 8259 §3-§6).** All six: `null`, `true`/`false`, number, string,
array, object. A JSON text may be any value, not only an array or object -
RFC 8259 §2 widened this from the older RFC 4627, and the parser follows the
newer rule, so `42` and `"hi"` are complete documents.

**Numbers (§6).** The grammar is enforced exactly: a leading `-` but never a
leading `+`, no leading zeros, at least one digit either side of a `.`, and a
complete exponent. Beyond the grammar, the parser keeps more than a `double`:

- `preserve_number_lexeme` (default on) retains the original text, which is
  what makes exact round-tripping possible.
- `parse_int64` and `parse_uint64` (default on) detect exact integer
  representations.
- `parse_double` (default on) derives a `double` when the value is
  representable.

A number too large or too precise for any of those is still kept exactly, as
its lexeme, and read back with `gtext_json_get_number_lexeme()`. That is the
string-backed arbitrary-precision decimal, and it is on by default; there is
no separate option for it.

An integer too large for `int64_t` or `uint64_t` is **not** an error. It
parses, the lexeme is preserved, and the integer accessors simply do not
apply to it. This is a deliberate choice and is listed under
[Deviations](#json-deviations) because several popular parsers reject or
silently round instead.

**Strings (§7).** All escapes of the specification, `\u` included. Surrogate
pairs must be well formed: a high surrogate must be followed by a low
surrogate. Lone or reversed surrogates are rejected with
`GTEXT_JSON_E_BAD_UNICODE`. Unescaped control characters below U+0020 are
rejected unless `allow_unescaped_controls` is set.

**Encoding (§8.1).** Input must be UTF-8 and is validated by default
(`validate_utf8`), through the lexer. Validation is by value and not only by
shape: an overlong encoding, a surrogate half, and anything past U+10FFFF are
all refused, as RFC 3629 §3 requires. They were not, until JSONTestSuite was
scored for the first time - `C0 AF`, an overlong `/` and the classic way past
a filter that matches on the character rather than the bytes, parsed as a
string. A leading UTF-8 BOM is accepted and skipped by default
(`allow_leading_bom`); §8.1 forbids emitting one, and the writer never does.

**Duplicate names (§4).** The specification says names *should* be unique but
does not require it, so every parser has to choose. This one rejects by
default - `GTEXT_JSON_DUPKEY_ERROR` - and offers `FIRST_WINS`, `LAST_WINS`
and `COLLECT`, the last gathering duplicates into an array.

**JSONC extensions**, each off by default: `allow_comments` (`//` and
`/* */`), `allow_trailing_commas`, `allow_single_quotes`,
`allow_nonfinite_numbers` (`NaN`, `Infinity`, `-Infinity`) and
`allow_unescaped_controls`. None of these is RFC 8259; enabling any of them
means the input is no longer JSON.

**JSON5 numeric literals**, each off by default and each a separate question:

- `allow_hex_numbers` - `0x1F`, `0XdeadBEEF`, `-0x10`. A hex literal has no
  fraction and no exponent, because `e` is one of its digits: `0x1e2` is 482,
  not 100. The value goes into `int64`/`uint64` and the `double` is derived
  from it; the preserved lexeme keeps the spelling that was written.
- `allow_leading_plus` - `+1`, `+1.5e2`. The integer accessors see through the
  sign, so `+42` still has an `int64`.
- `allow_bare_decimal_point` - `.5` and `5.`, one option because they are one
  question: whether the point may sit at an edge. `.` alone is still not a
  number, and neither is `.e1`.

JSON5 puts the sign in front of the whole value, so `+Infinity` and `+NaN` are
valid JSON5 and need `allow_leading_plus` *and* `allow_nonfinite_numbers`.
`-NaN` needs only the latter, for the same reason `-Infinity` always has.
NaN's sign is not observable, so both signed spellings give NaN.

**JSON5 string escapes**, also off by default and also two separate questions:

- `allow_ecma_escapes` - `\xHH`, `\v`, `\0`, and ECMAScript's rule that any
  other character after a backslash is that character (`\a` is `a`, `\'` is an
  apostrophe). `\xHH` names a *codepoint*, so `\xe9` decodes to the two UTF-8
  bytes of U+00E9 rather than to the byte 0xE9, which would not be UTF-8 at
  all. `\0` is U+0000 and is an error where a digit follows it; `\1` through
  `\9` are always errors, because those were octal escapes in a language that
  no longer has them.
- `allow_line_continuations` - a backslash before a line terminator contributes
  nothing, which is how JSON5 writes a string over several lines. All five
  terminator sequences: LF, CR, CRLF, U+2028 and U+2029. CRLF counts as one, so
  the LF is not left behind to be read as an unescaped control character.

A backslash before a line terminator is a continuation and not an identity
escape, so with `allow_ecma_escapes` alone it is an error rather than quietly
meaning a newline. A *raw* newline inside a string is still a control character
under both options; the continuation is the backslash's doing.

**JSON5 unquoted object names.** `allow_unquoted_keys`, off by default, makes
`{a: 1}` legal. The name is an ECMAScript `IdentifierName`, which is wider than
`[A-Za-z_]`: any character with the Unicode property ID_Start may begin one, any
with ID_Continue may continue it, `$` and `_` may do either, and `\uXXXX`
escapes are allowed - `{\u0061: 1}` names `a`, and a surrogate pair reaches an
astral character as it does in ECMAScript 5.1. `\u{1F600}`, the ECMAScript 2015
spelling, is not accepted, because the JSON5 specification is written against
5.1. The properties come from the same generated table as the whitespace, so a
character that is merely non-ASCII is not a name: U+1F600 has neither property
and is refused, and U+0301 has ID_Continue only, so it may continue a name but
not start one.

`IdentifierName` includes the reserved words, so `{true: 1}` is an object whose
name is `true`, and so are `{null: 1}`, `{NaN: 1}` and `{Infinity: 1}` - the
last two without `allow_nonfinite_numbers`, which is about values. Those words
are still keywords wherever a value is expected, and `-Infinity` is not a name
at all, because a name cannot begin with a sign.

The name is decoded before it is compared, so `{a:1,"a":2}` and
`{a:1,\u0061:2}` are duplicate names, and with `normalize_unicode` an unquoted
name is normalized exactly as a quoted one is.

**JSON5 whitespace.** `allow_ecma_whitespace`, off by default, widens the space
*between* tokens from JSON's four characters - tab, LF, CR, space - to
ECMAScript's set: vertical tab, form feed, U+FEFF, every character in
General_Category Zs (U+00A0 and U+3000 among them), and the line terminators
U+2028 and U+2029. Zs is asked of `ghoti.io-unicode`, because that category has
moved before: U+180E was Zs until Unicode 6.3 reclassified it as Cf, so the
answer depends on which UCD version answers, and the suite keeps one. A character that only
looks space-like is not whitespace - U+200B ZERO WIDTH SPACE is Cf and stays a
syntax error.

**JSONPath (RFC 9535).**
`gtext_json_path_compile()` and `gtext_json_path_select()`, with
`gtext_json_path_query()` for a one-shot, and `_select_paths()` / `_query_paths()`
where the *normalized path* of each result is wanted as well as the node. The root identifier, child and
descendant segments, and the name, wildcard, index and slice selectors,
including several selectors in one bracket. A result is a node list in the order
the specification gives, and it may hold the same node twice - `$[0,0]` selects
the first element twice, and §2.3.1.2 says so.

The **filter selector** is implemented: `$[?@.price < 10]`, with `&&`, `||`,
`!`, parentheses, the six comparison operators, and the functions `length()`,
`count()`, `value()`, `match()` and `search()`. A filter may hold another
filter, and a comparison may name the document root - `$.a[?@.b == $.x]` - as
well as the current node.

`match()` asks whether the whole string matches an I-Regexp (RFC 9485).
`search()` asks whether any substring does. Both are compiled by
ghoti.io-regex. A pattern that is not an I-Regexp makes the function false,
which is what RFC 9535 says, and the query itself stays well-formed. A match
the engine stops because a limit was reached is `GTEXT_JSON_E_LIMIT`: that is
not a false result. `GTEXT_JSON_E_PATH_UNSUPPORTED` remains the status for a
construct this build cannot evaluate, because dropping it and running the rest
of the filter would select every element of the array.

An **ill-typed** query is invalid rather than false, as §2.4.2 says: `length()`
takes a value so its argument cannot be a multi-node query, `count()` and
`value()` take a node list so their arguments cannot be literals, only a
singular query may be compared, and a value is not a test expression. Each of
those is `GTEXT_JSON_E_PATH`.

The examples in RFC 9535 §1.5 and the slice examples in §2.3.4 are in the suite,
in `tests/test-json-path.cpp`, written from the RFC rather than from this
implementation. `make conformance-jsonpath` scores it against the
[JSONPath Compliance Test Suite](https://github.com/jsonpath-standard/jsonpath-compliance-test-suite):
**704 of the 706 cases**. The other two, "explicit caret" and "explicit
dollar", treat `^` and `$` as anchors. RFC 9485 makes both ordinary
characters, and that is what is implemented. They are reported as a
disagreement with the suite, not as passes and not as failures. A case this
build refused as unsupported would be reported beside that number as well.

That score covers **both** halves of what the suite asserts: the node list -
which nodes a query selects and in what order - and the *normalized path* of each
result (§2.7), which `gtext_json_path_query_paths()` produces.
`$['store']['book'][0]['author']` is the only spelling §2.7 blesses: brackets
throughout, single-quoted names, no negative indices. Comparing only the values
would pass a query that selected the right nodes by the wrong route, and a
planted off-by-one in the index builder takes the score from 650 to 364.

That runner has found three defects so far, each of which the hand-written tests
agreed with: `$ ` is not a well-formed query, because `segments = *(S segment)`
puts the blank space *before* a segment; blank space *is* allowed before each
segment of a query inside a filter, so `length(@ .a .b)` is one query with two
segments; and `<=` is defined as "less than or equal" rather than as an ordering
of its own, which is why `null <= null` is true even though null is unordered.

**Pointer, Patch and Merge Patch.** RFC 6901 evaluation including the `~0`
and `~1` escapes; the six RFC 6902 operations `add`, `remove`, `replace`,
`move`, `copy` and `test`, applied atomically so that a failing operation
leaves the document unchanged; and RFC 7386 recursive merge.

The worked examples in RFC 6901 section 5, RFC 6902 Appendix A and the RFC
7386 Appendix A test table are all in the suite, in
`tests/test-rfc-conformance.cpp`. Writing the three appendix
tables down found three divergences that the existing tests, all written
against the implementation, agreed with: `"/"` resolved to the root rather
than to the member named `""`, `move` applied its `add` before its `remove`
so that moving within one array used unshifted indices, and a merge patch
adding a new object member stored its `null` members instead of dropping
them.

**Schema.** 2020-12, 2019-09, draft-07, draft-06 and draft-04, each with its own
keyword set. `$schema` selects the dialect. A keyword this library cannot
enforce fails compilation and is named. `minLength` and `maxLength` count
characters, not bytes - see [Deviations](#json-deviations). Schemas compile
once and validate many instances. What is still refused is under
[Gaps](#json-not-implemented).

## Limits

Applied unless overridden; `0` in the option means "use the default".

| Option | Default |
|---|---|
| `max_depth` | 256 |
| `max_string_bytes` | 16 MiB |
| `max_container_elems` | 1 Mi elements |
| `max_total_bytes` | 64 MiB |

**`max_depth` is a stack budget, not only a resource limit.**
`gtext_json_parse()` is recursive descent, so a document's nesting depth is the
parser's stack depth - measured at about **448 bytes per level**, calibrated by
bisecting the depth at which the parse dies at four stack sizes (445, 447, 448
and 447 bytes per level at 1, 2, 4 and 8 MiB). The default 256 needs some 115 KB
and is safe anywhere; the ceiling is about **2,300 levels on a 1 MiB thread
stack** and **18,000 on Linux's 8 MiB main stack**, and past it the parse does
not return an error - the process dies. So raising this limit for untrusted
input is choosing a number rather than removing one.

`gtext_json_stream_feed()` has no such bound: the streaming parser keeps its
nesting stack on the heap, and a 50,000-level document goes through it with
`max_depth` raised. For deep or untrusted input it is the parser to use. This is
the one place the two parsers accept different documents, and the difference is
in the safe direction.

## Save

The writer emits RFC 8259 by default and nothing else: compact, no trailing
newline, no BOM, keys in insertion order, and the original number lexeme
reproduced byte for byte when it was preserved. That last point is the reason
`canonical_numbers` is off by default - normalizing a lexeme is lossy, and
the safe default for a writer is to hand back what it was given.

Pretty-printing, indent width, newline string, spacing around `:` and `,`,
and inline thresholds for short arrays and objects are all configurable.
For a stable byte-for-byte output across runs, `sort_object_keys` orders
names, `escape_unicode` forces `\uXXXX` for non-ASCII, and `canonical_numbers`
normalizes numeric lexemes. String escapes are always normalized: the DOM
stores decoded strings, so the writer re-escapes canonically and the input's
original escape spellings are not retained.

`escape_unicode` and `escape_all_non_ascii` escape **codepoints**, so `é`
becomes `\u00E9` and an astral character becomes a surrogate pair -
`\uD83D\uDE00`. They used to escape each *byte* of the UTF-8 sequence, which
turned `é` into `\u00C3\u00A9`: valid JSON holding the two characters `Ã©`.
The two options are synonyms; nothing observable distinguishes them.

`space_after_comma` is a compact-mode option. In pretty mode the comma is
followed by an indent beginning with a newline, so the space would be trailing
white space at the end of every line, and it is not written.

**A string that is not valid UTF-8 is refused** with
`GTEXT_JSON_E_BAD_UNICODE`, by both writers, rather than written through. A
parse validates UTF-8, so such a string can only reach a writer through the
`gtext_json_new_*` builders or `gtext_json_writer_string()`; written out it
would be bytes this library's own parser rejects. The refusal can leave a
partial document in the sink, as a sink failure can - these writers stream, and
only `gtext_csv_write_table()` in this library promises otherwise.

`allow_nonfinite_numbers` must be set for the writer to emit `NaN` or
`Infinity`, and doing so produces output that is not JSON. Without it a
non-finite value is an error rather than a silent `null`, which is what
several other libraries substitute.

### Two writers, and what only one of them can do

`gtext_json_write_value()` is handed a whole document and
`gtext_json_writer_*()` is handed one call at a time, and that difference is
not only a matter of convenience: **four of the options cannot be honoured
incrementally**, because each needs a container complete in hand before its
first byte can go out.

| Option | Why a streaming writer cannot honour it |
|---|---|
| `sort_object_keys` | the order of the names is not known until the last one has arrived, and the first was already written |
| `inline_array_threshold` | whether an array is short enough to inline is a fact about its finished length |
| `inline_object_threshold` | likewise for an object's member count |
| `canonical_numbers` | it chooses reformatting over a lexeme a parse preserved, and nothing here was parsed: `gtext_json_writer_number_lexeme()` is the caller handing over bytes and `gtext_json_writer_number_double()` formats, so the choice the option makes is the choice of which function to call |

`gtext_json_writer_new()` accepts all four and ignores them. It does not fail,
because an options structure is shared between the two writers and a caller
who sets `sort_object_keys` once for a program that uses both should not have
the incremental one refuse to start. The header says which, field by field,
and `JsonWriterAgreement.TheFourOptionsTheIncrementalWriterCannotHonour`
asserts both halves: that the incremental writer ignores them, and that the value writer
honours them - so the list cannot grow quietly.

Everything else is the same writer. Both paths produce the same bytes for the
same document under the same options, which is now asserted rather than
assumed; see **Tested scope** below.

## One input, several JSON texts

A JSON text is one value. RFC 8259 says so, and by default a token after it is
`GTEXT_JSON_E_TRAILING_GARBAGE` - which is right for a document and wrong for a
log, an export or a network stream, where the bytes are a *sequence* of values.
That format has no single specification, and three readings of it are in use
that disagree about inputs which occur. So
`GTEXT_JSON_Parse_Options::records` is an enumeration rather than a flag:

| Mode | Between two records | Accepts |
|---|---|---|
| `GTEXT_JSON_RECORDS_OFF` | nothing may follow the first | one JSON text; the default, and what every release before this did |
| `GTEXT_JSON_RECORDS_WHITESPACE` | any JSON white space, including none | `{"a":1}{"b":2}`, `1 2`, and a pretty-printed value per record |
| `GTEXT_JSON_RECORDS_LINE` | at least one LF, and no line end *inside* a record | NDJSON / JSON Lines |
| `GTEXT_JSON_RECORDS_SEQ` | an RS (0x1E) before each record, the first included | RFC 7464 `application/json-seq` |

Nothing about a record's own grammar changes in any mode. A record is a JSON
text held to exactly the same rules, including every limit and the
duplicate-name policy.

**`LINE` is the only mode that can say "one value per line",** and that is why
it is worth distinguishing from `WHITESPACE` rather than being a stricter
version of it. Under `WHITESPACE`, a file whose records a buggy writer ran
together with no separator is accepted and a reader expecting one record per
line silently sees fewer records than there are lines. `LINE` refuses that, and
refuses a value printed across lines, because in a format where the line is the
record such a value is read as several broken records by every other tool.

**Two modes refuse some parse options**, at the first call with
`GTEXT_JSON_E_INVALID`, rather than making a promise with a hole in it.

`LINE` refuses three. `allow_unescaped_controls` and `allow_line_continuations`
each let a line end reach the inside of a *string*, where the rule cannot see
it; `allow_comments` admits `//`, which is *terminated* by a line end, so a
comment and a record cannot share a line.

`SEQ` refuses `allow_comments`. An RS is the byte that makes RFC 7464's framing
unambiguous precisely because it cannot occur inside a JSON text - but it can
occur inside a comment, and a comment is an extension to the text rather than
to the framing. With both on, a `//` comment running to the end of the input
swallows every RS after it, so the streaming parser reads the rest as one
comment where a reader slicing on RS reads several records. RFC 7464's grammar
has no comments in it.

`WHITESPACE` accepts comments between records and makes no claim that any of
these would break, so it is where a caller who wants both is routed.

`SEQ` **skips framing that introduces nothing**: `RS RS` is an RS with no
record after it, and RFC 7464 has a reader discard a truncated element rather
than fail on it. A trailing RS at the end of the input is the same case.

Three entry points read the format:

- **`gtext_json_stream_feed()`** emits `GTEXT_JSON_EVT_RECORD_END` once per
  record. That event exists because the others cannot answer the question: `1 2`
  is two records and emits two `EVT_NUMBER` events, which is also what the
  single value `[1,2]` emits between its array markers.
- **the pull reader**, which wraps the streaming parser and so delivers the
  same events.
- **`gtext_json_parse_multiple()` in a loop**, which returns one value and says
  where the next begins, so a sequence is read one record at a time without
  ever holding more than one. It honours `records`, so the framing is checked
  rather than merely tolerated, and trailing framing that introduces nothing -
  a final newline, a json-seq RS that RFC 7464 says to discard - is counted as
  consumed, so the loop is a plain `while (off < len)`.

`gtext_json_parse()` is **not** one of them. It returns one value and has
nowhere to put a second, so it refuses trailing content whatever `records`
says, and a test asserts that rather than leaving it to this paragraph.

On the writing side `GTEXT_JSON_Write_Options::records` is the same
enumeration, so a program that reads records and writes them back names the
format once. `gtext_json_write_value()` frames each call, which needs no state;
the incremental writer defers each record's terminator to the next record or to
`gtext_json_writer_finish()`. The two produce the same bytes, which is
asserted. A records mode subsumes `trailing_newline` rather than adding to it -
honouring both would end the output in a blank line that a reader of this
format reads as one more separator - and `LINE` with `pretty` is
`GTEXT_JSON_E_INVALID`, because a writer asked for both is asked for a file
this library's own `LINE` reader could not read back.

**With `records` off, a second top-level value is now refused** with
`GTEXT_JSON_E_STATE`. That is a fix rather than a restriction: the incremental
writer used to accept one and write `{"a":1}{"b":2}`, which is not JSON and
which this parser refuses, while every call and `finish()` returned
`GTEXT_JSON_OK`.

`examples/json/json_ndjson.c` reads the same bytes in all four modes and prints
what each one says.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|---|---|---|
| Top-level value | any of the six | empty input, `GTEXT_JSON_E_BAD_TOKEN` |
| Numbers | full §6 grammar | `.5`, `+1` `E_BAD_TOKEN`; `0123` `E_BAD_NUMBER`; `5.`, `1e` `E_INCOMPLETE` |
| Non-finite | opt-in | default `GTEXT_JSON_E_NONFINITE` |
| Integers beyond 64 bits | parsed, lexeme kept | no integer accessor applies |
| Strings | all §7 escapes | lone/reversed surrogate `E_BAD_UNICODE`; raw control `E_BAD_TOKEN` |
| Encoding | UTF-8, validated | invalid sequence `E_BAD_UNICODE` |
| Leading BOM | accepted, skipped | never written |
| Duplicate names | four policies | default `GTEXT_JSON_E_DUPKEY` |
| Trailing content | single value; a consumed-length parse; or a sequence, with `records` | `GTEXT_JSON_E_TRAILING_GARBAGE` by default |
| Comments, trailing commas, single quotes | opt-in | default `E_BAD_TOKEN` / `E_TRAILING_GARBAGE` |
| Depth | 256 default | `GTEXT_JSON_E_DEPTH` |
| Size limits | four, configurable | `GTEXT_JSON_E_LIMIT` |

@anchor json-deviations
## Where this parser differs from other parsers

| Case | This parser | Elsewhere |
|---|---|---|
| Duplicate names | error by default | most parsers take the last silently |
| Integer beyond `int64`/`uint64` | accepted, exact lexeme kept | often rounded to `double`, or rejected |
| Non-finite on write | error unless opted in | frequently written as `null` |
| Number round-trip | original lexeme reproduced | usually reformatted from `double` |
| `5.` and `1e` | `E_INCOMPLETE` rather than `E_BAD_NUMBER` | a single malformed-number error |

The last row is a wart rather than a design decision: a truncated number and a
grammatically invalid one are both simply invalid at top level, and reporting
one as "incomplete" invites a caller to wait for more input that will not
help. It is documented here because the status code is part of the API and
changing it would break callers. (With `allow_bare_decimal_point`, `5.` is a
number and the row does not apply to it; `1e` is still incomplete.)

A leading byte-order mark is skipped only where the input begins. U+FEFF
inside a string is an ordinary character. White space after a top-level value
may arrive in a later feed; other content there is
`GTEXT_JSON_E_TRAILING_GARBAGE`. A stream of only white space, or only a
comment, is refused at `finish()`, as `gtext_json_parse()` refuses it.

**The streaming parser enforces ERROR and FIRST_WINS.** `dupkeys` defaults to
`GTEXT_JSON_DUPKEY_ERROR`. A repeated name - quoted, unquoted, or written once
each way - is `GTEXT_JSON_E_DUPKEY` from both parsers. FIRST_WINS parses the
later member and does not deliver it: the callback never sees that key. The
later value is still parsed, so a broken one is an error.

LAST_WINS and COLLECT still deliver every member. A value the callback has
already been handed cannot be replaced or wrapped afterwards, and buffering
every object until its `}` would stop the stream being a stream. Both of those
modes accept a repeated name, and so does the DOM parser, so the two still
agree about whether the document is JSON. What they do not agree about is
which value a caller who builds an object from the events ends up with.

@anchor json-tested-scope
## Tested scope

**Fixtures** live in `tests/data/json/`, hand-written and grouped by what
they exercise: `rfc8259/` for the base grammar, `numbers/` for the integer
boundaries and precision, `unicode/` for surrogate handling, `invalid/` for
each rejection, `jsonc/` for the extensions, and `valid/` for structural
cases including large containers.

**Direct behavioral check.** Every row of the compliance checklist above was
produced by parsing the literal input with
`gtext_json_parse_options_default()` and recording the returned status, not
by reading the parser. Where this page names a status code, that code was
observed.

**Fuzzing.** `tests/fuzz/fuzz_json.cpp` under libFuzzer with ASan and UBSan,
seeded from `tests/fuzz/corpus/json/`. The harness spends its first input
byte selecting parse options, so the extension paths are reachable rather
than dead. It has found real bugs - a use-after-free in the object parser's
error path among them; `tests/fuzz/README.md` records what and how.

`tests/fuzz/fuzz_json_writer.cpp` reaches what that harness cannot. A corpus
of JSON text drives a parse, and the incremental API is a sequence of calls
that no document produces - so its input is a **program** rather than a
document: each byte selects a writer call, and the writer's own structural
rules are what keep the program legal. The same bytes build a shadow document
through the DOM API, which is written with `gtext_json_write_value()` and
compared as canonical text, so the differential is free.

**Records.** `tests/test-json-records.cpp` is 21 tests over
`GTEXT_JSON_Parse_Options::records`, and what shapes them is that a mode which
merely accepts more is easy to assert and easy to get wrong in the direction
that matters. So every mode is checked in both directions - the inputs it must
accept *and* the inputs it must refuse that a weaker mode accepts - and three
pairs of modes are compared over the same input, because a test that only feeds
NDJSON to NDJSON mode cannot tell `LINE` from `WHITESPACE`. Every case also runs
one byte at a time, since a separator is the one thing in this feature a feed
boundary can split, and the streaming parser's two older defects were both
answers that depended on where the caller's chunks fell. Ten planted mutations
were each caught by the test written for it.

`tests/fuzz/fuzz_json_records.cpp` states the same property over inputs nobody
chose: the streaming parser and a loop over `gtext_json_parse_multiple()` must
agree about whether an input is a legal sequence and about how many records it
holds, at every chunk size, and whatever the input turned out to hold must
survive being written back and read again. It found five defects from an empty
corpus, four of them older than the option it was written for - a partial
keyword as the last record emitting no boundary event, leading blank lines
counted as part of the first record, an exponent sign absorbed after the
exponent's digits on resumption, and `gtext_json_parse_multiple()` reporting
unlexable trailing bytes as consumed, which silently dropped them.

**The two writers are compared directly.**
`tests/test-writer-agreement.cpp` writes the same values both ways under nine
options, over every object size from zero to four members by nine value kinds,
and reparses each result with its member count checked. It exists because the
incremental writer emitted `{"a":1,"b":,2}` for every object of two or more
members - invalid JSON, from a sequence of calls that each returned
`GTEXT_JSON_OK`, `gtext_json_writer_finish()` included. A single member was
right by accident. The one test over that API had asserted that three
substrings appeared in the output, and all three do appear in that malformed
line, which is why a substring assertion is not a test of a writer.

**Reach of the oracles.** `make conformance-json` clones
[JSONTestSuite](https://github.com/nst/JSONTestSuite) and scores this parser
against its `test_parsing` cases, which exist precisely to catch the
disagreements a hand-written fixture set will not think of. Of the 283 that
are decidable - 95 a parser must accept, 188 it must refuse - **281 pass,
99.3%**, and the two that do not are the duplicate-name policy rather than the
grammar: RFC 8259 says names SHOULD be unique and leaves the behaviour
unspecified when they are not, and this parser refuses them by default. With
`dupkeys = GTEXT_JSON_DUPKEY_LAST_WINS` the score is **283 of 283**.

Not one of the 188 must-refuse cases is accepted, which is the direction that
matters for a parser reading input it did not write.

The remaining 35 cases are marked `i_`, meaning the suite leaves the answer to
the implementation - very deep nesting, lone surrogates, huge exponents. This
parser accepts 14 of them. They are reported rather than scored.

NULL options are the defaults, including a number's lexeme and its integer
and floating accessors. `JsonStreamDom.TheTwoParsersAgreeOnWhatJsonIs`
compares the DOM parser and the streaming parser on the same inputs, the
streaming parser both in one feed and a byte at a time.
`make conformance-json` scores the DOM parser.

**The schema engine has an oracle of its own.** `make conformance-json-schema` clones
[JSON-Schema-Test-Suite](https://github.com/json-schema-org/JSON-Schema-Test-Suite)
at the commit in `tools/conformance/JSON_SCHEMA_COMMIT` and runs the
`draft2020-12` directory:

| Set | Files | Assertions | Answered correctly | Schemas refused |
|---|---|---|---|---|
| `required` | 46 | 1,301 | **1,301 (100.0%)** | 0 |
| `optional` | 13 | 162 | **162 (100.0%)** | 0 |
| `optional/format`, asserting | 21 | 866 | **866 (100.0%)** | 0 |

The "schemas refused" column is the one to read first, and is why the
percentage is worth anything: this engine refuses a schema it cannot fully
enforce, so a keyword it had not implemented would show up there as a case
never run rather than as a wrong answer. Zero refused and zero wrong is the
only combination that means what the percentage appears to mean.

The denominator was checked against the corpus rather than taken from the
runner - the 46 files hold 1,301 assertions between them, which is the number
answered - because a harness that silently skips a file reads exactly like one
that passes it. `pattern`, `patternProperties` and `format` need a
regular-expression provider, and the run supplies `ghoti.io-regex`; without
one those keywords are refused rather than ignored, and the suite is scored
with them present because that is the configuration in which the engine is
complete.

@anchor json-not-implemented
## Gaps

The dialect is 2020-12 by default. 2019-09, draft-07, draft-06 and draft-04
are each read with their own keyword set, scoped to the resource that declares
`$schema` - including draft-04's three rules of its own: `id` for the
identifier, boolean `exclusiveMinimum`/`exclusiveMaximum` over
`minimum`/`maximum`, and `integer` as a constraint on how the number is
written. draft-03 and earlier are refused, and so is the unversioned
`http://json-schema.org/schema#`, which names no draft. Every published
meta-schema of every dialect it reads is embedded: 2020-12's nine, 2019-09's
seven, and one each for draft-07, draft-06 and draft-04.

A schema this library cannot fully enforce is refused at compile time.

- `pattern` and `patternProperties` need a regular-expression provider in
  `GTEXT_JSON_Schema_Options`: three function pointers and a context pointer.
  With a provider they are compiled at schema-compile time and enforced at
  validation time. The dialect the provider must implement is ECMA-262 with
  the `u` flag, the match is a search rather than an anchored match, and both
  strings are UTF-8 with lengths given. A search that could not finish
  becomes `GTEXT_JSON_E_LIMIT`. Recording that as "no match" would turn a
  budget into a wrong validation result.
- `regex` as a `format` value needs that same provider. It is the only name
  in the format vocabulary this library declines; every other one is checked.
- `$recursiveRef` is accepted only with the value `"#"`. 2019-09 defines
  exactly one. `$recursiveRef` and `$recursiveAnchor` are that draft's
  spelling of `$dynamicRef` and `$dynamicAnchor`.

`$id` establishes a base and an embedded resource, `$anchor` names a location
in one, and `$ref` resolves `#`, `#/...`, `#name`, a relative URI and an
absolute one. A reference that leaves the document goes through
`GTEXT_JSON_Schema_Options::resolver`, and is refused at compile time when
there is none.

`normalize_unicode` runs in the lexer, where a JSON string becomes bytes, so
object names are normalized as well as values. `{"\u00e9":1,"e\u0301":2}` is
one name written twice, and with the option on the default duplicate policy
refuses it. The normalizer is `src/idna/nfc_utf8.c`, checked by
`make check-nfc-oracle` against Python's `unicodedata`. It requires
`validate_utf8`, which is on by default, and it disables `in_situ_mode` for
strings: canonical ordering can keep the byte length and change the bytes, so
a length test alone would return the un-normalized input. Numbers are still
referenced in place.

The streaming parser's LAST_WINS and COLLECT modes still deliver
every member of a repeated name; see above.

Every entry point here takes a caller's allocator - parsing, the writer, the
streaming parser and its pull reader, JSON Pointer, JSON Patch and JSON Schema.
`GTEXT_JSON_Error::context_snippet` does not, because gtext_json_error_free() is
handed only the error, and neither do the `gtext_json_new_*` DOM builders, which
take no options. The \ref format_allocator_todo "allocator page" tracks what is
left across the library.

Some refusals carry line 0 and column 0. The message names the fault.

---

Back to \ref text_format_references "Format and specification references".
