@page format_toml TOML

# TOML

This module reads and writes TOML v1.0.0: the whole grammar, all four
date-time types through `ghoti.io-chron`, and every one of the four
redefinition rules as a separate check. Every claim below was checked by
something that runs, and where the evidence is thin the page says how thin.
Back to \ref text_format_references "Formats".

## Normative references

- **Specification:** [TOML v1.0.0](https://toml.io/en/v1.0.0), 2021-01-12.
- **Date-times:** delegated to `ghoti.io-chron`'s `gchron_parse_toml()`, which
  implements the four productions of §Offset Date-Time, §Local Date-Time,
  §Local Date and §Local Time. This module finds the extent of a date-time
  token and nothing else - not the grammar, not the leap-year rule, not the
  offset range.
- **TOML v1.1.0 is not implemented.** It is still a draft, and it is not 1.0.0
  plus permissions: it *refuses* two things 1.0.0 accepts. Section
  [Not implemented](#toml_not_implemented) lists the eleven cases and the
  measurement behind that number.

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
  `\b \t \n \f \r \" \\ \uXXXX \UXXXXXXXX`; an escape must name a Unicode
  scalar value, so a surrogate or a value above U+10FFFF is
  `GTEXT_TOML_E_BAD_UNICODE` rather than a bad escape - the escape is
  well-formed and names something that is not a character. Multi-line forms
  trim a newline immediately after the opening delimiter, and multi-line basic
  applies the line-ending backslash including the whitespace between the
  backslash and the newline. Up to two quotes adjacent to a closing delimiter
  are content (`"""x"""""` is `x""`); a sixth leaves a quote in the stream,
  where the statement parser refuses it, which is what the reference does.
- **Newlines.** LF and CRLF. A lone CR is refused everywhere -
  `GTEXT_TOML_E_CONTROL` - including inside a multi-line string, where 1.0.0
  leaves it to the control-character rule and 1.1.0 names it. Inside a string's
  value CRLF becomes LF, so one document read on two platforms gives one value.
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
  `d = 1979-05-27 # comment` is a local date and a comment.
- **Tables.** `[header]`, `[[array of tables]]`, inline `{ }`, and the tables a
  dotted key creates. Keys come back in definition order.

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
  key is `""` and a key containing a NUL is `"a\u0000b"`.
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
| Strings | all four forms, all 1.0.0 escapes | `\e`, `\xHH`, `E_BAD_ESCAPE`; surrogate or out-of-range escape, `E_BAD_UNICODE`; control character, `E_CONTROL` |
| Integers | decimal, hex, octal, binary, underscores | outside `int64_t`, `E_RANGE`; leading zero or misplaced underscore, `E_BAD_TOKEN` |
| Floats | fraction, exponent, infinities, NaN | `1.`, `.1`, `1.e2`, `1e`, `E_BAD_TOKEN` |
| Date-times | all four types, `-00:00` as unknown offset | anything `chron` refuses, `E_DATETIME` |
| Arrays | heterogeneous, nested, trailing comma, newlines and comments inside | - |
| Inline tables | pairs, dotted keys, nesting | a newline or a trailing comma inside, `E_BAD_TOKEN` (1.0.0) |
| Tables | headers, arrays of tables, implicit parents | the four redefinition rules above |
| Comments | `#` to end of line | control characters, `E_CONTROL` |
| Writing | the whole 1.0.0 grammar this module reads, in the spellings under Save | invalid UTF-8, `E_BAD_UNICODE`; a date-time `chron` refuses, `E_DATETIME`; a non-table root, `E_INVALID`. No comments, no multi-line or literal strings, no non-decimal integers |

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
`tools/conformance/TOML_SUITE_COMMIT`. `make conformance-toml`. One corpus,
six scores, because it carries expectations rather than only inputs and so
measures both directions:

```
decode                709 of  709   the reader, against the suite expectations
roundtrip as-read     709 of  709   parse, write, parse again
roundtrip headers     208 of  208   the same, every table forced to a [header]
roundtrip inline      208 of  208   the same, every table forced inline
encode as-read        208 of  208   the writer, with tomllib reading what it wrote
encode headers        208 of  208   the same, every table forced to a [header]
total                2250 of 2250   (100.0%)
```

Several things about that score, because a clean one is where this page is
least useful if it stops:

- **How much of the corpus was asked, beside how much passed.** All 709 cases
  the suite's own `tests/files-toml-1.0.0` manifest lists, with nothing
  skipped. The manifest rather than a directory walk: the tree also holds the
  1.1.0 cases, and scoring a 1.0.0 parser against those reads as a parser
  defect.
- **501 of the 709 are invalid**, so the majority of this corpus walks the
  refusal paths rather than the happy one. That is unusual and it is the most
  valuable thing about it.
- **Only the two `encode` rows contain a second implementation.** Everything
  else, the roundtrip rows included, proves this module's two halves agree
  with each other. In encode mode the TOML this writer produces is read by
  `tomllib` and compared to the suite's own expectation, so the writer is
  scored against something outside this library.
- **Exit status 3 is separated from exit 1.** The runner exits 1 for a document
  this library refuses, which is what 501 cases want; 3 means the runner could
  not write, or could not read back, its own output, and no case of any kind
  may produce it. Without that separation a writer that emitted garbage would
  have scored as a correct refusal on all 501 invalid cases.
- **The reader gate has been seen to fail.** Five defects were planted one at a
  time and each was caught: accepting a lone CR as a newline (1 case),
  dropping the leading-zero check (9), letting a header redefine a
  header-defined table (12), skipping the up-front UTF-8 validation (2), and
  accepting a duplicate key (13).
- **The writer gate has been seen to fail, and once not to.** Seven writer
  defects were planted. Six moved the score: emitting sub-tables in place
  rather than holding them to a second pass (952 cases), omitting the `.0` that
  keeps a float a float (30), dropping the sign of a negative zero (5),
  allowing a dot in a bare key (35), not escaping the backslash (60), and
  writing an empty array of tables as zero headers (2 - and only in the two
  rows that force headers, which is what those rows are for).

  **The seventh moved nothing.** A `table_style` that never reached its
  decision scored 2250 of 2250, because forcing headers where inline was asked
  for produces a document with the same *value*, and a comparison by value -
  which is the only comparison tagged JSON supports - cannot see the
  difference. That is a structural blindness of this corpus, not a gap in how
  it is driven, and it is why the style option is pinned as **text** in
  `tests/test-toml-writer.cpp`. Under the same mutation that file fails.

**Sanitizers.** All 2250 runs under ASan + UBSan + LSan with
`-fno-sanitize-recover`: 0 sanitizer reports, and no exit status outside
`{0, 1}`. The verdict comes from each case's stderr rather than its exit
status, for the reason above - on an invalid case an abort and a refusal are
the same status. Armed twice, and the two probes did not overlap:

- A planted leak of the writer's frame stack was reported on every valid case.
- A planted one-byte overread while copying an ASCII character produced **no
  sanitizer report at all** - the byte it read was inside the same allocation,
  since the buffer is NUL-terminated with slack - and was caught only by the
  exit-3 channel, on 303 runs, because the character it appended made the
  writer's own output unparseable. Two instruments, and each saw something the
  other did not.

**`tests/test-toml.cpp`**, 23 tests, for what the corpus cannot see: accessors
against the wrong type and against NULL, definition order, key identity and
NUL-containing keys, the eight redefinition rows above, the `int64_t` bounds
from both sides and one past each, locale independence, error line and column,
`max_depth` and `max_total_bytes`, a 100,000-deep document parsed and freed,
file parsing, and each 1.1.0 addition refused with the specific code that says
which kind of refusal it is - so when the option lands, these tests change
rather than continuing to pass for the wrong reason.

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
corpus's own manifest is the only reference there is.

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
the corpus stays at 2250 of 2250, because it runs in the C locale.

**No writer reference.** `tomllib` reads TOML and does not write it, and
`tomli_w` is not in the standard library, so nothing outside this repository
was used to check *which* of two valid spellings the writer chooses - only that
what it wrote reads back as the right value. The Save section above is
therefore the specification for the shapes, and `tests/test-toml-writer.cpp` is
what holds it.

**What nothing checks yet.** There is no fuzz harness for this module, in
either direction, and no differential that puts generated documents to both
this parser and `tomllib`. The corpus scores the cases someone chose; neither
of those exists yet, so nothing scores the cases nobody chose.

@anchor toml_not_implemented
## Not implemented

- **TOML 1.1.0, behind an option.** `make conformance-toml-next` scores this
  module against the 1.1.0 manifest today and prints the work as a list:
  **2252 of 2296**, with the same eleven valid cases refused in each of the
  four rows that read TOML - `\e` and `\xHH` escapes, a newline or a trailing
  comma inside an inline table, and a time without seconds. All 494 invalid
  cases are still refused, including `invalid/control/multi-cr` and
  `rawmulti-cr`, which 1.1.0 newly forbids and this parser already does. That
  asymmetry is the reason the option cannot be written as "relax some checks".

  **It is a reader-only job**, which the same run says: both `encode` rows
  score **218 of 218** on the 1.1.0 manifest. 1.1.0 adds spellings, not values,
  so a writer that emits 1.0.0 spellings of 1.1.0 documents is already correct
  for both - and the two things 1.1.0 newly forbids are bare carriage returns,
  which this writer escapes and so never emits.
- **A fuzz harness**, for the reader and for the writer. `tests/fuzz` has them
  for the other formats; TOML has neither.
- **A pull reader / event API**, as JSON, CSV and YAML have. TOML's semantics
  are whole-document - a header can reopen a table defined earlier, and dotted
  keys interleave - so an event stream is not simply a linearisation of the
  file and needs its own design decision first.
- **Comments in the DOM.** The YAML DOM carries leading and inline comments;
  this one does not yet.
- **`toml_to_json` / `json_to_toml`**, beside the existing `yaml_to_json`.

---

Back to \ref text_format_references "Formats".
