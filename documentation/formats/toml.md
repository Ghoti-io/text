@page format_toml TOML

# TOML

This module reads and writes TOML v1.0.0: the whole grammar, all four
date-time types through `ghoti.io-chron`, and every one of the four
redefinition rules as a separate check. It also **reads** the v1.1.0 draft,
behind `GTEXT_TOML_Parse_Options::version`; the writer has no such option and
[The version option](#toml_version) says why that is a finding rather than an
omission. A document can be read two ways - as a tree, or as
[the sequence of statements it was written as](#toml_events), which is what
carries the comments and the positions a tree cannot - and a tree
[converts to and from JSON](#toml_json), where the two data models disagree in
four places and each one is an option rather than a guess. Every claim below was
checked by something that runs, and where the evidence is thin the page says how
thin.
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
  <tt>\\b \\t \\n \\f \\r \\&quot; \\\\ \\uXXXX \\UXXXXXXXX</tt>, and at 1.1.0 also
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
  mistake. With `GTEXT_TOML_Parse_Options::retain_comments` they are kept -
  in the tree where the writer can put them back, and in the event stream
  wherever they were; [Comments](#toml_comments) has the division and the
  count.
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
| <tt>\\e</tt> and <tt>\\xHH</tt> escapes | `scan_escape()`, `src/toml/toml_lexer.c` | `E_BAD_ESCAPE`, with a message naming 1.1.0 | U+001B, and U+00HH |
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
with no writer change at all. By default nothing 1.1.0 adds survives into output
either: the seconds are written, <tt>\\e</tt> and <tt>\\xHH</tt> come back as `\uXXXX` or as the
character, an inline table is one line, and no trailing comma is emitted. A
`version` field on `GTEXT_TOML_Write_Options` would therefore be an axis with no
point of use, which is the same defect as a `table_style` that never reaches its
decision - and that one scored 2250 of 2250 before anyone noticed. The round
trip is asserted instead: `TomlVersion.AOneOneZeroDocumentWritesAsTomlOneZeroZeroAccepts`
reads a document only 1.1.0 accepts, writes it, and reads it back with the
strict arm.

**What the draft's spellings *do* get is a style option**, which is the other
direction and a different question: not "which version is this output valid
under" - all of it is valid under both - but "may the writer use a spelling only
the draft can read". `GTEXT_TOML_Write_Options::spellings` is that, five bits and
zero by default:

| bit | v1.0.0 writes | with the bit |
|---|---|---|
| `_ESCAPE_E` | `\u001B` | `\e` |
| `_ESCAPE_X` | `\u00HH` for an escaped control | `\xHH` |
| `_TIME_NO_SECONDS` | `07:32:00` | `07:32`, where the seconds and the fraction are both zero |
| `_INLINE_NEWLINES` | `{ a = 1, b = 2 }` | one pair to a line, two spaces for each enclosing table |
| `_INLINE_TRAILING_COMMA` | `{ a = 1 }` | `{ a = 1, }` |

Five bits rather than one, because they are five decisions with five
consequences and a single bit standing for a family is a bit somebody later has
to prove reaches all of it. `GTEXT_TOML_SPELL_1_1_0_ALL` is for the caller who
means "the draft's spellings" and accepts that the set widens as this library
learns more of them.

Three things about it are deliberate:

- **Tables only.** A newline and a trailing comma inside `[ ]` have been legal
  since v1.0.0, so an array's layout is not a v1.1.0 spelling and neither bit
  touches one. An empty table stays `{}`: there is no last pair for a comma to
  follow.
- **`\xHH` escapes no more characters than `\uXXXX` did.** `\xf8` would be
  U+00F8 under the draft, and a writer that used it would be escaping a
  character no rule asks to be escaped. So the bit changes the *spelling* of the
  escapes v1.0.0 already required and adds none, and a document of ordinary text
  still reads at 1.0.0 with the bit set.
- **The seconds come off `chron`'s text, not off a second speller.** The draft
  says an omitted `:00` "will be assumed", and this removes exactly the three
  bytes that assumption puts back - the mirror of the reader, which *inserts*
  `:00` before `gchron_parse_toml()` sees the text. The value decides (a zero
  second and a zero fraction, and no fraction in the text - `fraction_digits` 3
  writes `.000` and the seconds cannot come out from in front of it) and the
  text is then checked: where the value says droppable and `:00` is not standing
  where it belongs, the write fails with `E_STATE` rather than mangling a
  timestamp on a guess.

**How it is measured**, because a spelling option is exactly the shape of thing
a corpus comparing values cannot see - which is the finding this module has now
made four times:

- `roundtrip 1.1.0 spellings` writes all 218 valid cases with every bit set and
  reads them back with the 1.1.0 arm, against the same expectations. That is the
  *values* half, and it is what catches a spelling that changes one: the
  seconds-dropping made to fire on any time refuses four cases with `E_STATE` and
  takes the arm to 3875 of 3883.
- `spellings reach` writes each case twice, once with the bits and once without,
  and requires the two answers to be opposites: bytes that differ must be
  refused by the 1.0.0 arm and bytes that do not must be accepted by it. Every
  one of the five spellings is 1.0.0-invalid, so that equivalence holds, and an
  option that reached nothing breaks it on every case carrying the construct.
  **52 of the 218 are written differently**, and that count is the mode's real
  denominator: a zero would mean the corpus holds none of the constructs, so it
  is scored rather than reported. The first draft of this mode printed the
  complaint beside a score of 218 of 218 and met the floor.
- `tests/test-toml-version.cpp` has the exact bytes, bit by bit, the
  interactions (`\e` wins over `\xHH` for the character they share; the two
  inline bits are independent), and the places each bit must *not* reach. Eight
  mutations - each bit made not to reach its point of use, the mask dropped on
  the way into the writer, and the seconds dropped whatever they are - are each
  caught, and the unmutated tree is silent.
- `fuzz_toml_writer` draws the mask from its mode byte's spare bits, so the
  round trip is asserted under arbitrary combinations on documents nobody chose.

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
| `version` | `GTEXT_TOML_VERSION_1_0_0` | Which revision to read; see [The version option](#toml_version) |
| `retain_comments` | `false` | Keep comments: as `GTEXT_TOML_EVT_COMMENT` events, and on the tree where they can be written back |

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
- **Strings are basic strings.** `"..."` with <tt>\\b \\t \\n \\f \\r \\&quot; \\\\</tt> and
  `\uXXXX` for the remaining controls and U+007F, which v1.0.0 §String lists
  among the characters a basic string may not hold unescaped. The form a string
  was written in is not retained - a multi-line string comes back as a basic
  string with `\n` in it - because the DOM does not carry the form and a
  writer that guessed would be inventing one.
- **Keys are bare where v1.0.0 §Keys allows it** (`A-Za-z0-9_-`, non-empty) and
  a quoted basic string otherwise, escaped exactly as a string is. So the empty
  key is `""` and a key containing a NUL is <tt>&quot;a\\u0000b&quot;</tt>.
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
- **Output ends with a newline**, and is empty for an empty root table - unless
  that table carries a comment, in which case the document is comment lines and
  nothing else, which is how it was read. A blank line separates sections, and
  there is none before the first thing written; a leading comment goes after that
  blank line, with its header, and not before it.
- **It refuses rather than writing a document it could not read back.** A
  string or key that is not valid UTF-8 is `GTEXT_TOML_E_BAD_UNICODE`; a
  date-time `chron` will not spell is `GTEXT_TOML_E_DATETIME`. Both are
  reachable only through the constructors - a parsed tree contains neither.
- **Comments are written where they were read**, when the parse kept them:
  above a statement, after it on its line, and at the end of the document.
  `#` followed by the stored text is the line, so a read and a write are an
  identity on a comment line rather than nearly one. A comment it cannot
  place is `GTEXT_TOML_E_UNREPRESENTABLE` rather than dropped; see
  [Comments](#toml_comments).
- **What it never writes.** Multi-line strings, literal strings, non-decimal
  integers, and - with the default options - indentation.
- **The v1.1.0 spellings are available and off.**
  `GTEXT_TOML_Write_Options::spellings` is a mask of
  `GTEXT_TOML_Spelling` bits, each turning on one spelling the draft adds:
  `\e` for U+001B, `\xHH` for an escaped control, `:00` seconds omitted, an
  inline table broken across lines and indented, and a trailing comma after its
  last pair. `GTEXT_TOML_SPELL_1_1_0_ALL` is every one this release knows.
  **Any bit but zero produces a document a v1.0.0 reader refuses**, which today
  is every reader outside this library; see below.

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

@anchor toml_events
## Reading the statements

`gtext_toml_read_events()` walks a document and calls a callback once per
statement, in the order the file wrote them. `gtext_toml_parse()` answers what a
document *means*; this answers what it *says*, which is a different question
whenever the file's arrangement matters - a formatter, a linter, a diff, a tool
that reports a line number.

The two questions differ because a TOML tree is not a picture of its file. A
table may be opened, left, and reopened lower down as a sub-table of itself
(`[a.b]` before `[a]`); dotted keys interleave with headers; a comment between
two keys belongs to neither of them. The stream keeps all of that and the tree
keeps none of it.

| Event | From |
|---|---|
| `GTEXT_TOML_EVT_TABLE` | `[a.b]`, with the whole path |
| `GTEXT_TOML_EVT_ARRAY_TABLE` | `[[a.b]]` |
| `GTEXT_TOML_EVT_KEY` | the key of a pair, at the top level or inside `{ }` |
| `GTEXT_TOML_EVT_VALUE` | a scalar, as a borrowed `GTEXT_TOML_Value *` |
| `GTEXT_TOML_EVT_ARRAY_BEGIN` / `_END` | `[` and `]` |
| `GTEXT_TOML_EVT_INLINE_TABLE_BEGIN` / `_END` | `{` and `}` |
| `GTEXT_TOML_EVT_COMMENT` | a comment, when `retain_comments` asked |

A `KEY` is followed by exactly one value - a `VALUE`, or a balanced pair of
`BEGIN`/`END` events - and the pairs nest. Every event carries `offset`, `line`
and `col`, the same three numbers a `GTEXT_TOML_Error` reports, and each names
where its own token *started*: a multi-line string's value is on the line the
string opened on, not the line it closed on.

**It is the same parser.** The events are emitted by the walk that builds the
tree, a few lines after the check that made each statement legal, so the two
entry points refuse the same documents in the same place - `read_events` builds
the tree too, because TOML's duplicate-key and redefinition rules are answered
against it, and frees it on the way out. A consumer never sees a statement the
document turned out not to be allowed to make: a duplicate key produces no
`KEY` event. It may see a key whose *value* then turns out to be malformed,
because a statement is checked in pieces; the returned status is the verdict,
not the stream.

A callback that returns anything other than `GTEXT_TOML_OK` stops the walk, and
that value is what `gtext_toml_read_events()` returns - so a consumer that has
found what it came for stops the parse instead of reading the rest of the file,
and a perfectly good document can end with a status the *caller* chose.

**There is no pull reader, and that is a decision.** JSON, CSV and YAML each
have one here, with the same four calls in the same order, because each of those
parsers is genuinely incremental. This one is not, for two reasons that belong
to the format rather than to the implementation:

- TOML's refusals are whole-document. A duplicate key, a table defined twice, a
  `[[a]]` against a static `a` - each is decided against everything read so
  far, so a reader that answered before the end would be answering a different
  question from `gtext_toml_parse()`, and "is this a TOML document" would have
  two answers in one library.
- Nothing here is resumable. There are **forty** end-of-buffer tests across the
  lexer and the parser, and each currently means "the document ends here"; in an
  incremental reader each would have to mean "...or more input may follow",
  which is forty places to get right and a second grammar in all but name. The
  figure is one command rather than a claim, so it can be re-derived when either
  file changes:

  ```
  grep -o 'ctx->pos [<>]=* ctx->len' src/toml/toml_{lexer,parser}.c | wc -l
  ```

A `feed`-shaped reader over this parser would therefore accumulate the whole
document and parse it at the end: a streaming interface over something that does
not stream, whose caller would believe memory was bounded when it was not. The
callback gives a caller everything such a reader could, including stopping
early, and promises nothing it does not do.

@anchor toml_comments
## Comments

A comment is not part of TOML's data model, so keeping one is opt-in:
`GTEXT_TOML_Parse_Options::retain_comments`, default `false`. One option with
two points of use, deliberately not two: it decides whether `COMMENT` events are
reported, and the tree's comments are attached from those same events.

**The tree keeps the comments the writer can put back, and no others.**

| Kind | Where it was | Accessor |
|---|---|---|
| leading | the own-line comments immediately above a statement, joined with `\n` | `gtext_toml_value_leading_comment()` |
| inline | the comment after a statement, on its line | `gtext_toml_value_inline_comment()` |
| trailing | the comments after the last statement, on the root | `gtext_toml_value_trailing_comment()` |

They hang from the value a statement defines: the comments around `a = 1` belong
to the node `1`, and the ones around `[a.b]` to the table that header opened.
Consecutive own-line comments are one comment of several lines, which the writer
splits back into a line each; a blank line between two of them does not separate
them, because TOML gives no meaning to one and a writer cannot put it back.

The text is the bytes after the `#`, verbatim and without the line ending. So
`#` followed by the text is the line as written, which is what makes a read and
a write an identity on a comment line; a caller setting one should usually begin
it with a space. The CR of a CRLF belongs to the newline and not to the comment.

**What a tree cannot hold** is a comment inside a value - between two array
elements, or inside `{ }`. There is no statement there to attach it to, and the
writer has no line to put it on, so keeping it would be a promise broken on the
way out.

That second half used to be written as "no line in the document", which said the
format had nowhere for one. It does not: v1.0.0 already allows any number of
newlines and comments inside `[ ]`, this reader accepts them, and
`GTEXT_TOML_SPELL_1_1_0_INLINE_NEWLINES` gives an inline table lines of its own.
What is missing is on this side - **the writer has no multi-line array style**,
so it writes `[1, 2]` and a comment between the two elements has nowhere to go in
*its* output rather than in TOML's. Retaining them would be two pieces of work
and not one: a place on the tree for a comment that belongs to a value rather
than to a statement, and a layout the writer can put it back into. Those are reported by
`gtext_toml_read_events()`, in place, which is the division of labour between
the two readers - and the division is *measured* rather than described: the
corpus mode requires the number of comment lines a tree kept to equal the number
the event stream reported outside any container, for every case. Of the 206
comment lines in toml-test's 208 valid 1.0.0 cases, a tree holds 184 and the 22
others are all inside a value.

**The writer refuses what it cannot spell**, rather than altering it:

| Comment | Status |
|---|---|
| a control character other than tab, or U+007F | `GTEXT_TOML_E_CONTROL` - the rule TOML applies to a comment it reads |
| bytes that are not UTF-8 | `GTEXT_TOML_E_BAD_UNICODE` - as for a string |
| a line break in an *inline* comment | `GTEXT_TOML_E_UNREPRESENTABLE` - there is no second line to put the rest on, and splitting it would give the next statement a comment |
| any comment on a value being written inside `{ }` or `[ ]` | `GTEXT_TOML_E_UNREPRESENTABLE` |
| a *leading or inline* comment on an array that `GTEXT_TOML_TABLE_STYLE_HEADERS` turns into `[[a]]` blocks | `GTEXT_TOML_E_UNREPRESENTABLE` |

The last two rows are the ones a *document* can reach, and they are mirrors of
each other. `GTEXT_TOML_TABLE_STYLE_INLINE` puts every table inside braces, so a
document whose comments were written around headers cannot be written in that
style; `_HEADERS` turns `a = [{...}]` into `[[a]]` blocks, where each element
gets a header and the array gets nothing, so the comment from
`a = [{...}] # note` has no line to go on. Only the leading and inline ones are
refused there: a *trailing* comment means "after everything this node contains",
which is a position whether or not the node has a statement of its own, and it is
the position a table's trailing comment already gets. Nothing is wrong with the document or
with the style in either case; the two cannot be had together and the caller is
told which. A table written inline may still carry its *own* comments, since
those belong to the `a = { ... }` statement - only its children's are
unplaceable.

The second of those was found by `fuzz_toml_writer` rather than by the corpus,
and the reason is worth keeping: the `comments` mode round-trips the **as-read**
style, where an array read as `[ ]` stays inline and its comment stays on its
statement. A corpus mode that asked only the style a document arrived in cannot
see a rule about the styles it did not.

A trailing comment set on anything but the root is written after everything that
node contains, and does **not** survive a round trip: the next header follows it,
so the next parse reads it as that header's leading comment, which is what it now
is. The root is the stable place for one, and the only place a parse puts one.

@anchor toml_json
## JSON, both ways

`gtext_toml_to_json()` and `gtext_json_to_toml()`, beside the YAML module's pair.
The models are nearly the same shape - a table is an object, an array is an
array, a string is a string - and disagree in four places:

| | Has | The other has no |
|---|---|---|
| TOML | the four date-time types | type for them |
| TOML | `inf`, `-inf`, `nan` | spelling for them |
| JSON | `null` | value for it |
| JSON | a number of any size | integer wider than `int64_t` |

Each is an option, and the defaults are not uniform because the losses are not:

| Option | Default | The other arms |
|---|---|---|
| `datetime` | the string `gtext_toml_write()` would have written | refuse |
| `nonfinite` | refuse | the strings `"inf"`, `"-inf"`, `"nan"`; or `null` |
| `null_values` | refuse | leave the member out |

A date-time converts by default because the *value* survives - the string is the
same RFC 3339 text TOML would have written, from the same `chron` call - and only
its type does not. A non-finite float refuses by default because no JSON spelling
of `nan` keeps anything, so a caller has to choose which loss they want rather
than being handed one. And `null` refuses because TOML's way of saying "no value"
is to leave the key out, which `GTEXT_TOML_JSON_NULL_SKIP` does; a `null` **inside
an array** is refused whatever the policy says, since dropping it would shorten
the array and move every later index.

**A number is its lexeme, for its type and for its value.** A lexeme holding
`.`, `e` or `E` is a float and anything else an integer - the same question TOML
asks of the same characters - and the digits are converted with the same
`strtoll` and `gtext_number_strtod()` the TOML lexer uses. Asking the JSON value
what C representations it *carries* is a different question and answers wrongly
twice: a number this library built from a double carries no `int64` at all, and a
range test on the double accepts `-9223372036854775809`, because that literal is
exactly -2^63 once converted. Both of those were found by the corpus mode below,
the first on its first run.

**A round trip is an identity except at two places**, and both belong to JSON:

- a date-time becomes a string and stays one. Re-reading strings that look like
  dates is not on offer - a TOML string that spells a date *is* a string, and a
  conversion that guessed would change what a document means to make its own
  round trip look better;
- a float whose shortest lossless spelling carries no point and no exponent -
  `1.0`, `1e3` - is written `1` and `1000`, because JSON has one number type, and
  comes back a TOML integer. `-0.0` comes back `0`: the value survives and the
  sign of a zero does not.

`gtext_json_to_toml()` requires an object at the top, because v1.0.0 says a TOML
document is a table; an array or a scalar there is
`GTEXT_TOML_E_UNREPRESENTABLE`, which is a different thing from `E_INVALID` and
the distinction is the point of that status code - nothing is wrong with the
JSON. `gtext_toml_to_json()` has no matching rule, because JSON has none: a
single TOML value converts to a single JSON value.

**Depth.** Both conversions walk on an explicit stack, as everything else here
does, so neither can be crashed by nesting. The limit they carry is for the
*other* module: `gtext_json_free()` recurses, and a nested array built through
the JSON DOM API is released at 103,125 levels and dies by 106,250 on an 8 MB
stack (bisected, at that stack size). The default of 256 is the JSON module's own
parse default, so a converted tree is one the JSON parser could have produced
itself; 0 removes the limit and hands the question back to the caller.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|---|---|---|
| Encoding | UTF-8 | anything else, `E_BAD_UNICODE`; a leading BOM, `E_BAD_TOKEN` (a choice, see Deviations) |
| Keys | bare, quoted, literal, dotted, empty, NUL-containing | a multi-line string as a key, `E_BAD_TOKEN` |
| Strings | all four forms, all 1.0.0 escapes, and `\e` / `\xHH` at 1.1.0 | <tt>\\e</tt> and <tt>\\xHH</tt> at 1.0.0, `E_BAD_ESCAPE`; any other escape at either version, `E_BAD_ESCAPE`; surrogate or out-of-range escape, `E_BAD_UNICODE`; control character, `E_CONTROL` |
| Integers | decimal, hex, octal, binary, underscores | outside `int64_t`, `E_RANGE`; leading zero or misplaced underscore, `E_BAD_TOKEN` |
| Floats | fraction, exponent, infinities, NaN | `1.`, `.1`, `1.e2`, `1e`, `E_BAD_TOKEN` |
| Date-times | all four types, `-00:00` as unknown offset, seconds optional at 1.1.0 | omitted seconds at 1.0.0, `E_BAD_TOKEN`; a fraction on the minutes at either version, `E_BAD_TOKEN`; anything `chron` refuses, `E_DATETIME` |
| Arrays | heterogeneous, nested, trailing comma, newlines and comments inside | - |
| Inline tables | pairs, dotted keys, nesting; at 1.1.0 also newlines, comments and a trailing comma at the separators | a newline or a trailing comma at 1.0.0, `E_BAD_TOKEN`; a newline inside a pair at either version, `E_BAD_TOKEN`; two commas, or extending the table afterwards, at either version |
| Tables | headers, arrays of tables, implicit parents | the four redefinition rules above |
| Comments | `#` to end of line; kept on the tree and in the event stream with `retain_comments` | control characters, `E_CONTROL`; on the tree, only the comments a statement can carry - see [Comments](#toml_comments) |
| Reading as events | every statement in the order written, with positions, through `gtext_toml_read_events()` | no incremental `feed`, deliberately - [Reading the statements](#toml_events) says why |
| Versions | 1.0.0 by default, 1.1.0 through `GTEXT_TOML_Parse_Options::version` on the way in and `GTEXT_TOML_Write_Options::spellings` on the way out | an unrecognised version value reads as 1.0.0; an unknown spelling bit is ignored |
| JSON, both ways | every type with a counterpart, in definition order; date-times as strings by default | a non-finite float, `E_UNREPRESENTABLE` by default; a JSON `null`, `E_UNREPRESENTABLE` (or skipped); a `null` in an array, always `E_UNREPRESENTABLE`; an integer outside `int64_t`, `E_RANGE`; a non-object JSON root, `E_UNREPRESENTABLE`; a number with no lexeme, `E_INVALID` |
| Writing | the whole 1.0.0 grammar this module reads, in the spellings under Save - which is also valid 1.1.0, so there is no write-side version option - the five spellings 1.1.0 *adds* behind `GTEXT_TOML_Write_Options::spellings`, off by default, and the comments the tree kept | invalid UTF-8, `E_BAD_UNICODE`; a date-time `chron` refuses, `E_DATETIME`; a non-table root, `E_INVALID`; a comment it cannot place, `E_UNREPRESENTABLE`. No multi-line or literal strings, no non-decimal integers |

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
`make conformance-toml-next`. One corpus, two manifests, **ten scores on the
1.0.0 arm and twelve on the 1.1.0 one**, because it carries expectations rather
than only inputs and so measures both directions. The two extra rows are the
write-side spelling option, and they belong to that arm alone: output carrying a
v1.1.0 spelling is 1.0.0-invalid by construction, so re-reading it with the
strict arm would score the option's purpose as a defect.

```
=== toml-test, TOML 1.0.0 ===          === toml-test, TOML 1.1.0 ===
decode             709 of  709         decode             712 of  712
events             709 of  709         events             712 of  712
comments           144 of  144         comments           154 of  154
via json           208 of  208         via json           218 of  218
roundtrip as-read  709 of  709         roundtrip as-read  712 of  712
roundtrip headers  208 of  208         roundtrip headers  218 of  218
roundtrip inline   208 of  208         roundtrip inline   218 of  218
encode as-read     208 of  208         encode as-read     218 of  218
encode headers     208 of  208         encode headers     218 of  218
crossed             66 of   66         crossed             66 of   66
                                       roundtrip 1.1.0    218 of  218
                                        spellings
                                       spellings reach    219 of  219
total             3377 of 3377         total             3883 of 3883
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
- **The `events` row is a second consumer of the format, not the parser
  agreeing with itself.** The runner rebuilds each document from the event
  stream alone - through the public builder API, with its own find-or-create
  bookkeeping for headers and dotted keys - and that rebuild is scored against
  the suite's own expectation. The rebuild is in the runner and deliberately not
  in the library, where it would be the code the parser already runs and would
  agree by construction. It is also the only thing that exercises the
  constructors over 208 real documents rather than over unit tests. Armed:
  reporting a dotted key as its last part alone loses 56 cases, and dropping the
  array-end event loses 17, while the `decode` row is silent about both.
- **The `comments` row asks two things and is honest about its denominator.**
  Only the cases that have a comment are asked - counting the rest as passes
  would make the score a measurement of how many TOML files have no comments in
  them. Of each it asks that the tree kept exactly the comment lines the event
  stream reported outside any container, and that writing the document and
  reading it again gives the same comments back. It is a round trip, so it is
  blind in the way round trips are: a comment dropped on the way in and one
  dropped on the way out agree with each other. `tests/test-toml-comments.cpp`
  is where the halves are looked at separately.
- **The `via json` row asks the whole corpus and excludes nothing.** Every valid
  case goes out through `gtext_toml_to_json()` and back through
  `gtext_json_to_toml()`, and is compared against the suite's own expectation
  *transformed by the two losses a JSON round trip makes* - a date-time becomes a
  string, and a float JSON writes without a point comes back an integer - with
  the three cases holding a non-finite float expected to be **refused**. The
  alternative, skipping the cases a round trip cannot return, would have left
  exactly the two claims most likely to be wrong unmeasured. It found two defects
  on its first two runs, both about which number is which; they are in the
  section above and pinned in `tests/test-toml-json.cpp`.
- **Only the two `encode` rows contain a second implementation.** Everything
  else, the roundtrip rows included, proves this module's two halves agree with
  each other. In encode mode the TOML this writer produces is read by `tomllib`
  and compared to the suite's own expectation, so the writer is scored against
  something outside this library. `tomllib` reads 1.0.0 only, and those rows
  score 218 of 218 against the 1.1.0 manifest unchanged, which is the evidence
  that 1.1.0 adds spellings and not values. They run with the default write
  options, and they are the reason the spelling option is not a default: the
  same rows with `GTEXT_TOML_SPELL_1_1_0_ALL` would be 218 refusals from a
  reference that reads 1.0.0.
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
  | <tt>\\e</tt> and <tt>\\xHH</tt> accepted at both versions | 1 | - | 3 | caught |
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

- **The comment channels have been seen to fail, and once not to.** Four
  defects planted in the comment code, against the corpus mode and the two new
  test files:

  | planted defect | corpus `comments` | test-toml-comments.cpp | test-toml-events.cpp |
  |---|---|---|---|
  | the comments at the end of a file are not attached to the root | 1 case | 7 tests | - |
  | the writer drops a leading comment on a key-value line | 47 cases | 15 tests | - |
  | a comment inside an array is kept as the next statement's leading comment | 5 cases | 3 tests | - |
  | the CR of a CRLF is kept in the comment text | **-** | **-** | **3 tests** |

  The last row is the same shape as the version option's last two and as phase
  2's `table_style`: a rule about *spelling* that no case in the corpus states.
  No valid case pairs a CRLF line ending with a comment in a way the round trip
  can see, so the corpus scores 100% with the CR kept - the comment text is
  wrong, the document's values are not. That makes three such misses over
  twenty-four planted defects, all three the same shape.

  The first row is worth reading the other way round: the corpus sees that
  defect in exactly one case out of 208, which is a gate arguing for the unit
  file rather than against it.

- **The conversions' gate has been seen to fail, and once not to.** Four defects
  planted in `src/toml/toml_json.c`:

  | planted defect | corpus `via json` | test-toml-json.cpp |
  |---|---|---|
  | an integer converted through a double | 1 case | 3 tests |
  | every JSON number read as a float | 82 cases | 13 tests |
  | the date-time policy ignored, and one always refused | 19 cases | 9 tests |
  | the `null` policy ignored, and one always skipped | **-** | 5 tests |

  The last row is silent for a structural reason worth stating plainly: **no TOML
  document contains a `null`**, so nothing a TOML corpus can hold reaches the
  JSON-to-TOML direction's own hazards - the null policy, the integer wider than
  `int64_t`, the non-object root. The population that would is a *JSON* corpus,
  and this mode does not have one. The first row is the narrow one: only
  `valid/integer/long.toml` and its neighbours spell an integer a double cannot
  hold.

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

**`tests/test-toml-events.cpp`**, 22 tests, for the stream rather than the
grammar: the event sequence of every statement kind, a dotted key arriving as one
key of several parts, a reopened table reported twice, decoded key parts
including one containing a NUL, the positions - with a multi-line string whose
value is reported where it *started*, since the parser's line counter has moved
by the time there is a value to report - the refusals matching a parse's on both
the status and the position, a refused statement producing no event, a callback
stopping the walk at each of the first sixteen events (which is where a
sanitizer sees the half-built tree released on a path nothing else takes), and
the version and depth options reaching the walk.

**`tests/test-toml-comments.cpp`**, 25 tests, for both halves of the round trip
the corpus can only see together: where each kind attaches, the joining of
consecutive lines, the verbatim text, an empty comment being a comment, the
comment inside a value being absent from the tree and present in the stream -
asserted together, in one test - a commented document written back byte for byte,
and each of the four refusals above, none of which any document can reach.

**`tests/test-toml-json.cpp`**, 32 tests, for the conversions: every type with a
counterpart asserted as exact JSON text, the `int64_t` bounds keeping every digit,
all seven arms of the three policies, the four date-time kinds as the strings the
writer would have spelled, both documented round-trip losses asserted by value,
the depth limit in both directions, and that neither conversion borrows from the
tree it was given. Two of its tests are the defects the corpus mode found.

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

**Fuzzing**, `tests/fuzz/fuzz_toml.cpp` and `fuzz_toml_writer.cpp`, which is
what scores the cases nobody chose. The reader harness draws the version, the
comment option and the depth limit from the input and asserts four properties on
every document; the writer harness builds documents through the DOM API as well
as parsing them, because the writer's interesting refusals - a comment holding a
control character, a comment on a value going inside `{ }`, a string that is not
UTF-8 - cannot be reached from any text at all.

Every property was seen to fail, by planting a defect for it, with the
unmutated tree silent in all of them:

| property | the defect that fires it |
|---|---|
| the event walk accepts exactly what the parse accepts, with the same status and position | an empty comment made to stop the walk |
| **1.0.0's documents are a subset of 1.1.0's** | the inline-table trailing-comma rule inverted |
| what the writer writes, the parser reads, with the same values | the backslash left unescaped |
| the same comments come back (on parsed documents, all three styles) | four separate comment defects, above - and this property found a real one: see below |
| a trip through JSON settles after one pass | - |

The subset property is the one nothing else here can state, for the reason the
`crossed` mode exists: each manifest asks an arm only the cases its own list
decides. The last row is cheap insurance that nothing planted so far fires, and
saying so is more useful than implying it is armed.

**The writer harness found a defect in its first long run**, on the property the
corpus states only for the as-read style: under `_HEADERS` a comment on an array
of inline tables was dropped rather than refused. It is in the Comments section
above, with the rule it now follows.

One find in the first ninety seconds was the harness's own, and it is worth the
space: comparing two documents by their JSON *text* is stricter than the
contract, because the writer moves plain keys in front of sub-tables and must -
every bare key after a `[header]` belongs to that header's table. `[[a.b]]`
before `y = 2` comes back the other way round. The comparison sorts object keys
now, which is the comparison the corpus already made.

**The second reader is pinned.** The encode rows read this library's output with
`tomllib`, which is the only place a second implementation appears in this score
- and until now it was reached by `import tomllib` in the scoring script,
whatever `python3` this machine happened to have. `tomllib` is part of CPython
and changes with it, so "the writer's output is readable" meant "readable by this
machine today". It now goes through `tools/oracle/` like every other reference
here, the version is printed above the numbers, and `GHOTI_ORACLE_MODE=host`
still uses this machine's own interpreter and says so in that line. The
reference is asked once for the whole batch rather than per case.

@anchor toml_oracle
## A second reader, over documents nobody chose

`make check-toml-oracle` generates TOML v1.0.0 documents and compares this
library's reading of each against the pinned `tomllib`'s. It is the third thing:
the fuzzers check this library against itself - two of its own readers, and its
writer against its reader - and toml-test is a second implementation over the
cases somebody chose.

`tools/oracle/toml_gen.py` builds a value tree and then spells it, so validity is
a property of the construction rather than something to check afterwards: an
invalid document would make the comparison one of two error messages, and both
implementations refusing is not agreement about anything. The spelling varies
independently of the value, which is where a reader's disagreements live - the
same value as a bare, quoted or dotted key; as a `[header]`, a `[[array of
tables]]` or an inline table; as a basic, literal or multi-line string; with
underscores in an integer, four bases, and any of the five offsets a reference
can hold.

`-00:00` is deliberately not generated. v1.0.0 gives it the meaning "offset
unknown" and Python cannot hold that - `fromisoformat` reads it as UTC and
`isoformat` writes `+00:00` back - so a document using one would produce a
disagreement about the reference's model rather than about either reader. That is
the one place this differential is narrower than the format, and it is stated
rather than discovered.

**Three readings, not two.** The generator says what it meant, this library says
what it read, and the reference says what it read. The generator's claim is not a
third implementation - it is the test author - but it is what catches a generator
that emits something other than what it thinks, and it did: a first version put
an inline table's `key = value` line *after* a `[header]`, where it belongs to
that header's table, so both readers agreed with the text and disagreed with the
generator. Without that third reading it would have printed as a finding in this
library.

Measured: **60,000 documents, no disagreements**, and 3,000 in the gate by
default at about seven seconds. `TOML_ORACLE_COUNT` and `TOML_ORACLE_SEED` set
how many and where to start, and a disagreement prints the seed that reproduces
it on its own.

Seen to fail, three ways, each caught with the document that did it: an
underscore kept in a number rather than stripped, `\t` decoded as a space, and a
fractional second dropped before `chron` sees it. Two no-op mutations as controls
returned clean.

@anchor toml_1_1_oracle
### And a second reader for v1.1.0, as far as one exists

`make check-toml-1-1-oracle` is the same shape against a different reference, and
it is narrower on purpose. Nothing outside this repository reads the v1.1.0
draft. toml++ 3.4.0 is the nearest thing: `TOML_ENABLE_UNRELEASED_FEATURES` turns
on eight items its author cherry-picked from the TOML master branch and the issue
list, and that set **overlaps** v1.1.0's rather than coinciding with it.

So the overlap is written down by name, in
`tools/oracle/toml_1_1_diff.py` - beside the code that generates from it, rather
than in the pin table where it could drift:

| | |
| --- | --- |
| omitted seconds in a time of day (toml#671) | in both, compared here |
| newlines and trailing commas inline (toml#516) | in both, compared here |
| the `\e` escape (toml#790) | in both, compared here |
| the `\xHH` escape (toml#796) | in both, compared here |
| hex floating point (toml#562) | toml++ only, never generated |
| `+` in a bare key (toml#644) | toml++ only, never generated |
| unicode in an unquoted key (toml#687, toml#891) | v1.1.0 only, and unread here |
| a lone CR refused in a multi-line string | v1.1.0 only, no second reader |

The documents are the 1.0.0 generator's with those four spellings applied, so the
value tree is the same and a disagreement is about the relaxation rather than
about the rest of the format - which the `tomllib` differential already covers.

**All five spellings have to turn up in a run**, four items counted as five
because the inline-table relaxation is two independent choices and this library
offers them as two write-side bits for that reason. A run that reached three of
them would print a clean line for the other two, so the count of each is printed
and a missing one fails the gate. That is not a hypothetical: the first version
reached zero `\e` and zero `\xHH` documents - the 1.0.0 generator has no reason
to put U+001B in a string, and the substitution range started at U+0001 so it
never met the `\u0000` the base spelling actually writes - and the gate said so
and failed rather than reporting the three it had.

Measured: **60,000 documents generated, 35,992 of them spelling at least one
relaxation, no disagreements**, in about 45 seconds.

Seen to fail: `\e` decoded as U+001C, and the seconds patched back as `:01`
rather than `:00`, each caught with the document that did it; a no-op mutation as
a control returned clean. The image's driver carries the SHA-256 of the source it
was compiled from and the gate refuses to compare when that differs from the
committed `driver.cpp`, naming `make oracle-images` - checked by editing the file
and watching it refuse.

`gtext_json_to_toml()` has a corpus of its own. `make
conformance-json-to-toml` puts every `y_` and `i_` file of JSONTestSuite's
`test_parsing` - the population `make conformance-json` already clones and pins
- through the conversion, which is where the three places TOML is narrower than
JSON actually live: a `null`, an integer wider than `int64_t`, and a root that
is not a table. The `via json` mode of the TOML corpus cannot reach any of them,
because every document it converts came out of TOML.

The oracle is Python. For each document it walks the values in document order
and names the **first** obstacle the conversion should meet, which is the same
order the conversion walks in - so a document holding a `null` before a
too-large integer has one right answer rather than two, and there is no floor
to set: one disagreement fails the run.

| Mode | Cases | What it asks |
| --- | --- | --- |
| outcome | 105 | the documented refusal, or none |
| round trip | 98 | JSON → TOML → text → TOML → JSON, equal by value |
| null policy | 4 | `NULL_SKIP` drops a member and never an element |
| table root | 105 | v1.0.0: a TOML document is a table |

Each document is wrapped as `{"wrapped": <document>}` for the first three modes,
which is how the ones whose root is not a table reach the rest of the
conversion; `table root` is the unwrapped run, so the refusal is scored rather
than assumed. 25 of the 130 files this library's own JSON parser refuses, so
they are not inputs to a conversion at all - `make conformance-json` is where
that is scored, and the count is printed rather than left out. Two of the 130
carry a duplicate name, so the conversion is asked with the JSON parser's
`LAST_WINS` policy: without it two documents the corpus says must be accepted
would never reach the conversion.

The null-policy mode needs both halves of the policy in its denominator, and
says so: a run that saw only member `null`s or only ones inside an array has
measured one side of a two-sided option and would print 100% for it.

**It found a defect on its first run.** A number's lexeme was copied into a
64-byte buffer and anything longer refused with `E_RANGE`, on the reasoning that
nothing longer fits in an `int64_t` or a double. That is true of a number's
*shortest* spelling and not of a spelling, and JSON has no rule that a number be
written shortly: `y_number_double_close_to_zero.json` is 82 characters and an
ordinary double, and this library's own TOML reader accepts the same literal.
One path in one library disagreed with another about a representable value. The
length is not the question - `strtod` and `strtoll` are, and `ERANGE` from
`strtoll` is what an integer outside `int64_t` looks like.

@anchor toml_not_implemented
## Not implemented

- **An incremental reader.** The event walk exists; a chunk-fed one does not,
  and [Reading the statements](#toml_events) argues that it should not - the two
  reasons are the format's, so this is a closed question rather than an open one
  until one of them changes. Re-checked 2026-09-28: TOML's refusals are still
  whole-document, and the end-of-buffer count the argument rests on is still 40,
  by the command given there rather than by memory.
- **Comments inside a value, on the tree.** They are in the event stream and the
  tree does not keep them. The reason used to be written as "TOML has nowhere to
  put one back", and that was wrong: v1.0.0 already allows newlines and comments
  inside `[ ]` and this reader accepts them, and
  `GTEXT_TOML_SPELL_1_1_0_INLINE_NEWLINES` gives an inline table lines of its
  own. The blocker is on this side and it is two things - a place on the tree for
  a comment attached to a *value* rather than to a statement, and a multi-line
  array style for the writer to put it back into. Neither exists and nothing has
  asked for either; what is recorded here now is the work rather than an excuse.
- **A full 1.1.0 reference.** `make check-toml-1-1-oracle` compares the four
  relaxations toml++ and the draft agree about; the two v1.1.0 items this
  library does not read (unicode in an unquoted key, toml#687 and toml#891) and
  the one it reads that toml++ is not known to (a lone carriage return refused
  inside a multi-line string) have no second reader. Those three are asked of
  toml-test's decision and of nothing else.

---

Back to \ref text_format_references "Formats".
