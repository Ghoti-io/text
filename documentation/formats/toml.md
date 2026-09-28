@page format_toml TOML

# TOML

This module reads and writes TOML v1.0.0: the whole grammar, all four
date-time types through `ghoti.io-chron`, and every one of the four
redefinition rules as a separate check. It also **reads** the v1.1.0 draft,
behind `GTEXT_TOML_Parse_Options::version`; the writer has no such option and
[The version option](#toml_version) says why that is a finding rather than an
omission. Every claim below was checked by something that runs, and where the
evidence is thin the page says how thin.
Back to \ref text_format_references "Formats".

## Normative references

- **Specification:** [TOML v1.0.0](https://toml.io/en/v1.0.0), 2021-01-12.
- **Date-times:** delegated to `ghoti.io-chron`'s `gchron_parse_toml()`, which
  implements the four productions of §Offset Date-Time, §Local Date-Time,
  §Local Date and §Local Time. This module finds the extent of a date-time
  token and nothing else - not the grammar, not the leap-year rule, not the
  offset range.
- **TOML v1.1.0** (draft, no release date), read when
  `GTEXT_TOML_Parse_Options::version` is `GTEXT_TOML_VERSION_1_1_0`. Not the
  default, because it is a draft and because the reference below cannot read it.
  It is not 1.0.0 plus permissions - it also settles two questions 1.0.0 left
  contradictory - so [The version option](#toml_version) gives the change list
  clause by clause rather than calling it a relaxation. Both specification texts
  ship in the pinned corpus checkout, under `specs/`.

A parsed or built document belongs to the root `GTEXT_TOML_Value *` and is
released by one `gtext_toml_free()` of it. Every node carries the allocator it was made
with, so a document built through a caller's allocator is freed through the
same one; an error's `context_snippet` is the exception and comes from the
default allocator always, because an error struct outlives the parse and a
parse can fail before it has read its options.

## Parts implemented

- **Encoding.** UTF-8 only, validated for the whole document up front (§"TOML
  documents must be valid UTF-8 encoded Unicode documents"), and again
  per-character inside strings. The two are complementary rather than
  redundant: breaking the up-front check leaves exactly two corpus cases
  failing - `invalid/encoding/bad-codepoint` and `bad-utf8-in-comment` - and
  those are the bytes no scanner looks at.
- **Keys.** Bare (`A-Za-z0-9_-`), basic-quoted, literal-quoted and dotted, with
  whitespace permitted around the dots. **Key identity is the decoded string**,
  so `a.b`, `a."b"` and `a.'b'` are one key and `'a.b'` is one key containing a
  dot. A key may contain a NUL, since U+0000 has an escape and may be written
  in a quoted key, so keys are compared by bytes and a length rather than with
  `strcmp`.
- **Strings.** All four forms. Basic and multi-line basic take the escapes
  <tt>\\b \\t \\n \\f \\r \\" \\\\ \\uXXXX \\UXXXXXXXX</tt>, and at 1.1.0 also
  <tt>\\e</tt> and <tt>\\xHH</tt>; an escape must name a Unicode
  scalar value, so a surrogate or a value above U+10FFFF is
  `GTEXT_TOML_E_BAD_UNICODE` rather than a bad escape - the escape is
  well-formed and names something that is not a character. Multi-line forms
  trim a newline immediately after the opening delimiter, and multi-line basic
  applies the line-ending backslash including the whitespace between the
  backslash and the newline. Up to two quotes adjacent to a closing delimiter
  are content (`"""x"""""` is `x""`); a sixth leaves a quote in the stream,
  where the statement parser refuses it, which is what the reference does.
- **Newlines.** LF and CRLF. A lone CR is refused everywhere -
  `GTEXT_TOML_E_CONTROL` - including inside a multi-line string, and **at both
  versions**: 1.0.0's prose permits one there ("the control characters other
  than tab, line feed, and carriage return") while its own ABNF does not, since
  `mlb-unescaped` omits `%x0D` and `newline` is LF or CRLF. 1.1.0 settles that
  in the ABNF's favour, and this module takes the ABNF's reading under both, so
  no document is refused at 1.1.0 and accepted at 1.0.0. toml-test leaves
  `invalid/control/multi-cr` and `rawmulti-cr` out of the 1.0.0 manifest for
  exactly that reason, so neither manifest scores this and
  `tests/test-toml-version.cpp` holds it. Inside a string's value CRLF becomes
  LF, so one document read on two platforms gives one value.
- **Comments.** `#` to the end of the line, with the same control-character
  rule as strings: tab is allowed, every other C0 control and U+007F are not.
  Checking strings and forgetting comments is the common shape of this
  mistake.
- **Integers.** `int64_t` exactly, in decimal with an optional sign and in
  `0x`, `0o` and `0b` without one. Underscores must have a digit on each side.
  Leading zeroes are refused in the integer part and permitted in a
  fraction and an exponent, which is what the `zero-prefixable-int` production
  says.
- **Floats.** IEEE 754 double, with `inf`, `-inf`, `+inf`, `nan`, `-nan` and
  `+nan`. The conversion goes through `gtext_number_strtod()`, not `strtod()`,
  so it reads a decimal point whatever `LC_NUMERIC` says.
- **Booleans.** `true` and `false`, and a value must end at a terminator, so
  `truex` is not `true` followed by `x`.
- **Date-times.** All four, told apart by `GCHRON_TomlValue::kind`. A space
  between a date and a time is a separator only when a time really follows, so
  `d = 1979-05-27 # comment` is a local date and a comment. At 1.1.0 the seconds
  may be omitted; see the version section for how, which matters because `chron`
  is still the only date-time grammar here.
- **Tables.** `[header]`, `[[array of tables]]`, inline `{ }`, and the tables a
  dotted key creates. Keys come back in definition order. At 1.1.0 an inline
  table may carry newlines, comments and a trailing comma - but only at its
  separators, not inside a pair.

@anchor toml_version
## The version option

`GTEXT_TOML_Parse_Options::version` selects `GTEXT_TOML_VERSION_1_0_0` (the
default, and the value a zeroed struct and a `NULL` options pointer both give)
or `GTEXT_TOML_VERSION_1_1_0`. An unrecognised value reads as the strict arm:
the switch is written as "is this 1.1.0" rather than "is this 1.0.0", so a field
filled in from a configuration file cannot fall into a draft grammar.

**Three constructs move, and the option reaches exactly three places.** Each is
a point of use rather than a flag somebody might later read:

| What moves | Where the option is read | 1.0.0 | 1.1.0 |
|---|---|---|---|
| `\e` and `\xHH` escapes | `scan_escape()`, `src/toml/toml_lexer.c` | `E_BAD_ESCAPE`, with a message naming 1.1.0 | U+001B, and U+00HH |
| newline, comment or trailing comma inside `{ }` | `inline_space()` and the `WANT_KEY` state, `src/toml/toml_parser.c` | `E_BAD_TOKEN` | accepted at the separators |
| a time with the seconds omitted | `scan_datetime()`, `src/toml/toml_lexer.c` | `E_BAD_TOKEN`, pointing at where the seconds are missing | read as `:00` |

Three details are worth spelling out, because each is a place a plausible
implementation goes wrong:

- **`\xHH` names a scalar value, not a byte.** 1.1.0: "all TOML strings are
  sequences of Unicode characters, _not_ byte sequences". So `\xf8` is U+00F8
  and two bytes of UTF-8, and it goes through the same scanner as `\uXXXX` with
  a digit count of two. A reader that appended one `0xF8` byte would be accepted
  by every corpus mode that only asks whether a document was refused, and would
  then hold a string that is not UTF-8.
- **The omitted seconds are a rewrite of the text, not a second grammar.**
  1.1.0 says `:00` "will be assumed", so the scanner inserts `:00` after the
  minutes and hands the result to `gchron_parse_toml()` as before. `chron` stays
  the only date-time grammar in this library, which is the whole point of the
  dependency; a `chron` option for optional seconds would have been the other
  way to do it, and `chron` has none. The text without seconds is bounded by the
  grammar - no seconds means no fraction, so the longest is
  `YYYY-MM-DDTHH:MM+HH:MM` - and the buffer is checked against that bound
  anyway, because a bound argued from a grammar and a bound the code enforces
  are two different things.
- **1.1.0 relaxes the inline table's *separators* and not its pairs.** A line
  break may sit next to the opening brace, next to the closing brace, and on
  either side of a comma; it may not sit between a key and its `=` or between
  `=` and its value, because `keyval-sep` is still plain `ws`. **Neither
  manifest has a case either way**, which was measured: both halves of that
  mistake were planted and left the 1.0.0 manifest, the 1.1.0 manifest and the
  crossed mode all at 100%, with only `tests/test-toml-version.cpp` failing.

**Four things 1.1.0 changes need no option**, and saying so is the point - a
version switch with more arms than the specification has changes is a switch
somebody will later have to disprove:

| 1.1.0 change | Why nothing is gated |
|---|---|
| A lone CR inside a multi-line string becomes explicitly invalid | Refused at both versions; 1.0.0's ABNF already said so and only its prose disagreed. See Newlines above. |
| A multi-line string as a quoted key becomes explicitly invalid | Refused at both; 1.0.0's `quoted-key` production names the single-line forms only, and this module checked it in the key scanner from the start. |
| Multi-line *literal* strings "must normalize newlines in the same manner as multi-line basic strings" | Both forms already normalise CRLF to LF and refuse a lone CR, at both versions. |
| A note pointing at the ABNF for "the exact ranges of allowed code points" in a bare key | The prose still says `A-Za-z0-9_-`; the note reads like a relaxation and is not one. A non-ASCII bare key is refused at both versions. |

**The writer has no version option, and that is a measurement.** Every spelling
1.0.0 defines is still a 1.1.0 spelling, so a writer emitting 1.0.0 is already
correct for both - the encode rows score 218 of 218 against the 1.1.0 manifest
with no writer change at all. Nothing 1.1.0 adds survives into output either:
the seconds are always written, `\e` and `\xHH` come back as `\uXXXX` or as the
character, an inline table is one line, and no trailing comma is emitted. A
`version` field on `GTEXT_TOML_Write_Options` would therefore be an axis with no
point of use, which is the same defect as a `table_style` that never reaches its
decision - and that one scored 2250 of 2250 before anyone noticed. The round
trip is asserted instead: `TomlVersion.AOneOneZeroDocumentWritesAsTomlOneZeroZeroAccepts`
reads a document only 1.1.0 accepts, writes it, and reads it back with the
strict arm.

## The redefinition rules are four rules

This is the hard part of TOML, and each rule was pinned against the pinned
reference rather than read off the prose. A single "already defined" test gets
at least one of them wrong.

| Case | Verdict | Status code |
|---|---|---|
| `[a]` twice | refused | `GTEXT_TOML_E_REDEFINE` |
| `[a]` after `[a.b]` - a super-table afterwards | **allowed** | - |
| `[x]` / `a.b = 1` / `[x.a]` - a header over a dotted table | refused | `GTEXT_TOML_E_REDEFINE` |
| `[x]` / `a.b = 1` / `[x.a.c]` - a header *below* a dotted table | **allowed** | - |
| `[x.a.b]` / `[x]` / `a.c = 1` - a dotted key over an implicit table | **allowed** | - |
| `a = {b = 1}` / `a.c = 2` - adding to an inline table | refused | `GTEXT_TOML_E_REDEFINE` |
| `a = []` / `[[a]]` - appending to a static array | refused | `GTEXT_TOML_E_REDEFINE` |
| `a.b = 1` / `a.b = 2` | refused | `GTEXT_TOML_E_DUPKEY` |

The mechanism is a four-state origin on each table - implicit, header, dotted,
inline - plus a two-state one on each array. A boolean would collapse the three
allowed rows into the refused ones.

## Limits

| Option | Default | Meaning |
|---|---|---|
| `allocator` | `NULL` | Every allocation of the parse; `NULL` is `gtext_allocator_default()` |
| `max_depth` | 256 | Nesting of arrays and inline tables; 0 for no limit |
| `max_total_bytes` | 0 | Bytes of input, 0 for no limit; applied *while* reading a file rather than after |

`max_depth` at 0 is safe rather than merely permitted: the value parser builds
on an explicit stack, the teardown walks a worklist, and the writer walks two
explicit stacks of its own, so nothing about nesting touches the C stack. A
100,000-deep document is parsed, walked, written, read again and freed in the
test suite.

The writer has no depth limit and needs none. A limit on a parse refuses absurd
input before allocating for it; the tree a write is given is already in memory,
so there is nothing left to refuse cheaply.

| Write option | Default | Meaning |
|---|---|---|
| `allocator` | `NULL` | The write's own scratch - the two frame stacks and the header-path buffer. The document is not copied |
| `table_style` | `GTEXT_TOML_TABLE_STYLE_AS_READ` | How to spell a table; see Save |
| `datetime` | `NULL` | A `GCHRON_WriteOptions *`, or `chron`'s defaults |

## Save

`gtext_toml_write()` takes a document and a sink; `gtext_toml_write_file()`
writes one atomically, so a failure part way through leaves the previous file
whole rather than truncated. A growable-buffer sink and a caller's fixed-buffer
sink are provided, and a fixed buffer that runs out **fails** rather than
handing back a shorter document - the byte count alone cannot tell a full
buffer from an exactly-fitting one, which is what
`gtext_toml_sink_fixed_buffer_truncated()` is for.

**What it chooses, and why.**

- **A table is spelled the way it was read.** `[header]` for a table a header
  defined, `{ }` for one written inline; `[[header]]` for an array grown by
  `[[header]]` and `[ ]` for one written `[ ]`, even when every element of it
  is a table. TOML calls neither spelling canonical, so reproducing what
  arrived is the only choice that does not silently rewrite a file a person
  arranged. `table_style` overrides it in either direction:
  `GTEXT_TOML_TABLE_STYLE_HEADERS` and `_INLINE`.
- **Plain keys come before sub-tables**, whatever order they were defined in.
  This is not a choice: every bare key after a `[header]` belongs to the table
  that header opened, so a sub-table written before a plain key would move the
  key into it. Order *among* the plain keys, and among the sub-tables, is the
  order the document gave.
- **An empty array is always `key = []`**, even under `_HEADERS`. Zero
  `[[key]]` headers would not say that `key` exists at all.
- **Strings are basic strings.** `"..."` with `\b \t \n \f \r \" \\` and
  `\uXXXX` for the remaining controls and U+007F, which v1.0.0 §String lists
  among the characters a basic string may not hold unescaped. The form a string
  was written in is not retained - a multi-line string comes back as a basic
  string with `\n` in it - because the DOM does not carry the form and a
  writer that guessed would be inventing one.
- **Keys are bare where v1.0.0 §Keys allows it** (`A-Za-z0-9_-`, non-empty) and
  a quoted basic string otherwise, escaped exactly as a string is. So the empty
  key is `""` and a key containing a NUL is <tt>"a\\u0000b"</tt>.
- **A float keeps its type.** The shortest `%g` precision that reads back as
  the same double is used - found by converting and reading back, not by
  assuming 17, so 0.1 is `0.1` and not `0.10000000000000001` - and `.0` is
  appended when the result carries neither a point nor an exponent, because
  v1.0.0 §Float requires one and `1` is a valid TOML *integer*. The sign of a
  zero is kept; `-0.0` is not `0.0`, and the corpus distinguishes them.
  `inf`, `-inf`, `nan` and `-nan` are written as such.
- **An integer is decimal.** The base a number was written in is not retained,
  so `0xff` comes back as `255`.
- **A date-time is `chron`'s** `gchron_write_toml()`, with the caller's
  `GCHRON_WriteOptions` if any. An unknown offset is `-00:00`, which is the
  only spelling that carries the meaning.
- **Output ends with a newline**, and is empty for an empty root table. A blank
  line separates sections, and there is none before the first thing written.
- **It refuses rather than writing a document it could not read back.** A
  string or key that is not valid UTF-8 is `GTEXT_TOML_E_BAD_UNICODE`; a
  date-time `chron` will not spell is `GTEXT_TOML_E_DATETIME`. Both are
  reachable only through the constructors - a parsed tree contains neither.
- **What it never writes.** Comments (the DOM does not carry them), multi-line
  strings, literal strings, non-decimal integers, and indentation.

**Building a document** is `gtext_toml_new_table()`, `_new_array()`,
`_new_string()`, `_new_integer()`, `_new_float()`, `_new_boolean()`,
`_new_datetime()`, `gtext_toml_table_set()`, `gtext_toml_array_append()` and
`gtext_toml_value_set_inline()`. `table_set` refuses a duplicate key with
`E_DUPKEY` rather than replacing: TOML says defining a key twice is invalid, and
a builder that replaced would let a caller write a document differing from the
one they described with nothing saying so. Both setters refuse with `E_STATE` a
value that is already in a tree, that is the container itself, or that is an
ancestor of it - a double free and two shapes of cycle, none of which any test
could detect afterwards - and with `E_INVALID` a value from a different
allocator, since the whole tree is freed through the root's.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|---|---|---|
| Encoding | UTF-8 | anything else, `E_BAD_UNICODE`; a leading BOM, `E_BAD_TOKEN` (a choice, see Deviations) |
| Keys | bare, quoted, literal, dotted, empty, NUL-containing | a multi-line string as a key, `E_BAD_TOKEN` |
| Strings | all four forms, all 1.0.0 escapes, and `\e` / `\xHH` at 1.1.0 | `\e` and `\xHH` at 1.0.0, `E_BAD_ESCAPE`; any other escape at either version, `E_BAD_ESCAPE`; surrogate or out-of-range escape, `E_BAD_UNICODE`; control character, `E_CONTROL` |
| Integers | decimal, hex, octal, binary, underscores | outside `int64_t`, `E_RANGE`; leading zero or misplaced underscore, `E_BAD_TOKEN` |
| Floats | fraction, exponent, infinities, NaN | `1.`, `.1`, `1.e2`, `1e`, `E_BAD_TOKEN` |
| Date-times | all four types, `-00:00` as unknown offset, seconds optional at 1.1.0 | omitted seconds at 1.0.0, `E_BAD_TOKEN`; a fraction on the minutes at either version, `E_BAD_TOKEN`; anything `chron` refuses, `E_DATETIME` |
| Arrays | heterogeneous, nested, trailing comma, newlines and comments inside | - |
| Inline tables | pairs, dotted keys, nesting; at 1.1.0 also newlines, comments and a trailing comma at the separators | a newline or a trailing comma at 1.0.0, `E_BAD_TOKEN`; a newline inside a pair at either version, `E_BAD_TOKEN`; two commas, or extending the table afterwards, at either version |
| Tables | headers, arrays of tables, implicit parents | the four redefinition rules above |
| Comments | `#` to end of line | control characters, `E_CONTROL` |
| Versions | 1.0.0 by default, 1.1.0 through `GTEXT_TOML_Parse_Options::version` | an unrecognised version value reads as 1.0.0 |
| Writing | the whole 1.0.0 grammar this module reads, in the spellings under Save - which is also valid 1.1.0, so there is no write-side version option | invalid UTF-8, `E_BAD_UNICODE`; a date-time `chron` refuses, `E_DATETIME`; a non-table root, `E_INVALID`. No comments, no multi-line or literal strings, no non-decimal integers |

## Deviations

| Case | This parser | Elsewhere |
|---|---|---|
| A leading byte order mark, `\xEF\xBB\xBFa = 1` | refused, `E_BAD_TOKEN` | Neither 1.0.0 nor the 1.1.0 draft mentions a BOM and toml-test has no case for a leading one, so this is a choice, not a rule. It is refused because the pinned reference refuses it: CPython 3.13.5's `tomllib` gives `Invalid statement (at line 1, column 1)`. Measured, not assumed. |
| NaN's sign | carried, not interpreted | TOML has `-nan` and `+nan` and says nothing about which NaN either is. `gtext_toml_value_float()` hands back a NaN whose sign bit follows the text. |
| Newlines inside a multi-line string | CRLF normalised to LF | TOML permits a parser to normalise newlines to whatever suits its platform. LF is chosen so that one document gives one value on two platforms, which is what a differential needs. `tomllib` does the same. |

Nothing here is a known disagreement with a reference on a document either of
them accepts. If one is found it goes in this table with a reproduction.

## Tested scope

**toml-test, pinned at `ff49d109861c1ad25af53f687f2aef19ab650600`** (2026-09-15),
`tools/conformance/TOML_SUITE_COMMIT`. Scored by `make conformance-toml` and by
`make conformance-toml-next`. One corpus, two manifests, seven scores each,
because it carries expectations rather than only inputs and so measures both
directions:

```
=== toml-test, TOML 1.0.0 ===          === toml-test, TOML 1.1.0 ===
decode             709 of  709         decode             712 of  712
roundtrip as-read  709 of  709         roundtrip as-read  712 of  712
roundtrip headers  208 of  208         roundtrip headers  218 of  218
roundtrip inline   208 of  208         roundtrip inline   218 of  218
encode as-read     208 of  208         encode as-read     218 of  218
encode headers     208 of  208         encode headers     218 of  218
crossed             66 of   66         crossed             66 of   66
total             2316 of 2316         total             2362 of 2362
```

Several things about those scores, because a clean one is where this page is
least useful if it stops:

- **How much of the corpus was asked, beside how much passed.** Every case the
  suite's own `tests/files-toml-1.0.0` or `files-toml-1.1.0` manifest lists,
  with nothing skipped, and the version arm always taken from the same place as
  the expectations - the runner is given `--version=` by the same script that
  read the manifest. The manifest rather than a directory walk: the tree holds
  both versions' cases, and asking one arm the other's would read as a parser
  defect.
- **501 of 709 cases are invalid at 1.0.0 and 494 of 712 at 1.1.0**, so the
  majority of this corpus walks the refusal paths rather than the happy one.
  That is unusual and it is the most valuable thing about it.
- **Only the two `encode` rows contain a second implementation.** Everything
  else, the roundtrip rows included, proves this module's two halves agree with
  each other. In encode mode the TOML this writer produces is read by `tomllib`
  and compared to the suite's own expectation, so the writer is scored against
  something outside this library. `tomllib` reads 1.0.0 only, and those rows
  score 218 of 218 against the 1.1.0 manifest unchanged, which is the evidence
  that 1.1.0 adds spellings and not values.
- **The `crossed` row is the only one that can tell the two arms apart.** Each
  ordinary row asks an arm the cases its own manifest decides, so it is silent
  about everything that manifest drops. `crossed` computes four populations from
  the two manifests - nine cases 1.1.0 relaxed, six it added, two it tightened,
  and sixteen that differ only because the spec extracts were renumbered - and
  runs each under *both* arms with the expectation each carries. 33 of its 66
  runs are cases no manifest asks anybody. The populations come from the
  manifests rather than from a list in the script, because a hand-copied copy of
  the subject's own list is two spellings of one set and the copy is the one
  that goes stale.
- **Exit status 3 is separated from exit 1.** The runner exits 1 for a document
  this library refuses, which is what most cases want; 3 means the runner could
  not write, or could not read back, its own output, and no case of any kind may
  produce it. Without that separation a writer that emitted garbage would have
  scored as a correct refusal on every invalid case.
- **The reader gate has been seen to fail.** Five defects were planted one at a
  time and each was caught: accepting a lone CR as a newline (1 case), dropping
  the leading-zero check (9), letting a header redefine a header-defined table
  (12), skipping the up-front UTF-8 validation (2), and accepting a duplicate
  key (13).
- **The writer gate has been seen to fail, and once not to.** Seven writer
  defects were planted. Six moved the score: emitting sub-tables in place rather
  than holding them to a second pass (952 cases), omitting the `.0` that keeps a
  float a float (30), dropping the sign of a negative zero (5), allowing a dot
  in a bare key (35), not escaping the backslash (60), and writing an empty
  array of tables as zero headers (2 - and only in the two rows that force
  headers, which is what those rows are for).

  **The seventh moved nothing.** A `table_style` that never reached its decision
  scored 2250 of 2250, because forcing headers where inline was asked for
  produces a document with the same *value*, and a comparison by value - which
  is the only comparison tagged JSON supports - cannot see the difference. That
  is a structural blindness of this corpus, not a gap in how it is driven, and it
  is why the style option is pinned as **text** in `tests/test-toml-writer.cpp`.
  Under the same mutation that file fails.
- **The version option's gates have been seen to fail, and twice not to.** Eight
  defects were planted in the option, each run against all four channels. The
  unmutated tree is silent in all four.

  | planted defect | 1.0.0 | 1.1.0 | crossed | test-toml-version.cpp |
  |---|---|---|---|---|
  | the inline-table skipper relaxed at both versions | 4 | - | 4 | caught |
  | `\e` and `\xHH` accepted at both versions | 1 | - | 3 | caught |
  | an omitted second assumed at both versions | 3 | - | 4 | caught |
  | the option stored and never put on the parse context | - | 11 | 15 | caught |
  | `\xHH` appended as one byte rather than as U+00HH | - | 2 | - | caught |
  | a lone CR admitted at 1.0.0 (its prose over its ABNF) | - | - | **2** | caught |
  | a newline between an inline key and its `=` at 1.1.0 | - | - | - | **only here** |
  | a newline between an inline `=` and its value at 1.1.0 | - | - | - | **only here** |

  Two readings of that table. The `crossed` row earns its place: it caught six
  of the eight where the ordinary rows caught five, and the lone-CR defect was
  seen by **no other corpus channel**, because both cases for it sit in the
  1.1.0 invalid list and in neither 1.0.0 list. And the last two rows are the
  phase-2 finding in a new place: 1.1.0's relaxation stops at the inline table's
  separators, no case in either manifest says so, and a parser that relaxed the
  whole inline table scores 100% everywhere.

  One trap found while planting those: the sloppy version of the last mutation
  *is* caught. Relaxing the `=`-to-value gap without restricting it to an inline
  table also relaxes a top-level `key =` line, which `invalid/key/newline-06`
  refuses. A mutation that leaks outside the code under test measures the leak,
  and would have left this page claiming a gate that does not exist.

**Sanitizers.** All 5,684 reading runs - every case of both manifests, in the
decode mode and all three roundtrip modes - under ASan + UBSan + LSan with
`-fno-sanitize-recover`: 0 sanitizer reports, and no exit status outside
`{0, 1}`. The verdict comes from each case's stderr rather than its exit status,
for the reason above: on an invalid case an abort and a refusal are the same
status. Armed twice over the code phase 3 added:

- A planted leak on the omitted-seconds path was reported by LSan on every case
  that takes it.
- Shrinking the seconds-insertion buffer below the bound the grammar gives was
  reported as a stack-buffer-overflow - but **only on the longer of two cases**.
  `lt3 = 07:32` still fits an eight-byte buffer once `:00` is in, and
  `1979-05-27 07:32-07:00` does not. One example is not a population, even for a
  probe.

  In phase 2 the second probe was the reverse - a one-byte overread that drew no
  sanitizer report at all and was caught only by the exit-3 channel. That channel
  is a writer channel, and phase 3 changed no writer code, so it is not armed by
  this phase and the note is here rather than a claim that it was.

**`tests/test-toml.cpp`**, 23 tests, for what the corpus cannot see: accessors
against the wrong type and against NULL, definition order, key identity and
NUL-containing keys, the eight redefinition rows above, the `int64_t` bounds
from both sides and one past each, error line and column, `max_depth` and
`max_total_bytes`, a 100,000-deep document parsed and freed, file parsing, and
the 1.1.0 additions refused when no version is named at all.

**`tests/test-toml-version.cpp`**, 21 tests, for the option: the three routes to
the default and the unrecognised value, each of the three points of use in both
arms with the *value* asserted and not only acceptance, where the relaxation
stops, the four 1.1.0 changes that need no option, and the round trip that
stands in for a write-side version field. Its header carries the table above.

**`tests/test-toml-writer.cpp`**, 30 tests, for the writer's *text*, which is
the half no comparison by value can reach: the three table styles each asserted
as exact output, the ordering rule, the header paths, key and string spellings
including the empty key and a key containing a NUL, the float spellings, the
four date-time kinds and the unknown offset, `GCHRON_WriteOptions` reaching
`chron`, both sinks including the difference between a full fixed buffer and an
exactly-fitting one, a sink's own status coming back unchanged, the three shapes
the constructors refuse, and a 100,000-deep document written and read again.

Two defects in this module were found by that file rather than by the corpus,
and both are the kind a corpus structurally cannot find: a duplicate key was
reported four columns to the right of the key it complained about, under a
comment claiming otherwise, and `gtext_toml_parse(NULL, 10, ...)` segfaulted
while building the snippet for the refusal, because the error path walked a
buffer that was the thing being refused.

**The reference.** CPython's `tomllib` (standard library from 3.11, TOML
1.0.0), used to pin the redefinition rules and the multi-line-string edges
before they were written, and available in the `python` image already pinned in
`tools/oracle/containers/IMAGES` - no new image and no new wheel.
**Its reach ends at 1.0.0**: it implements nothing of 1.1.0, so on that arm the
corpus's own manifest is the only reference there is, and the fifteen cases the
two manifests disagree over are checked against the suite's decision and against
nothing else. A reference behind the pin is the harmless direction and this is
where it is written down.

**Locale independence** is asserted in `tests/test-locale-numbers.cpp` and not
beside the other TOML tests, because that file generates a comma-decimal locale
with `localedef` and *fails* when it cannot, rather than hoping one is
installed. The test that was in `tests/test-toml.cpp` called
`setlocale(LC_NUMERIC, "de_DE.UTF-8")` and used whatever came back - which is
NULL on the machine this was developed on, so it ran in the C locale every time
and measured nothing. Both halves need saying: `strtod` in a comma locale stops
at the point and reads `3.5` as `3`, and `snprintf` writes `3,5`, which is not
one TOML value but a key, a comma and another key. A mutation putting the
writer's doubles through `snprintf` is caught by that file and by nothing else -
re-measured after phase 3, both arms of the corpus stay at 2316 of 2316 and 2362
of 2362, because they run in the C locale.

**No writer reference.** `tomllib` reads TOML and does not write it, and
`tomli_w` is not in the standard library, so nothing outside this repository
was used to check *which* of two valid spellings the writer chooses - only that
what it wrote reads back as the right value. The same is true one step further
for 1.1.0: nothing outside reads the draft at all. The Save section above is
therefore the specification for the shapes, and `tests/test-toml-writer.cpp` is
what holds it.

**What nothing checks yet.** There is no fuzz harness for this module, in
either direction, and no differential that puts generated documents to both
this parser and `tomllib`. The corpus scores the cases someone chose; neither
of those exists yet, so nothing scores the cases nobody chose.

@anchor toml_not_implemented
## Not implemented

- **A fuzz harness**, for the reader and for the writer, and with the version
  option as an axis. `tests/fuzz` has them for the other formats; TOML has
  neither.
- **A pull reader / event API**, as JSON, CSV and YAML have. TOML's semantics
  are whole-document - a header can reopen a table defined earlier, and dotted
  keys interleave - so an event stream is not simply a linearisation of the
  file and needs its own design decision first.
- **Comments in the DOM.** The YAML DOM carries leading and inline comments;
  this one does not yet.
- **`toml_to_json` / `json_to_toml`**, beside the existing `yaml_to_json`.
- **A 1.1.0 reference.** Nothing outside this repository reads the draft, so
  the fifteen cases the two manifests disagree over are checked against
  toml-test's decision and against nothing else. When a second 1.1.0
  implementation exists it belongs in the encode rows the way `tomllib` is in
  the 1.0.0 ones.

---

Back to \ref text_format_references "Formats".
