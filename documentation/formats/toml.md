@page format_toml TOML

# TOML

The parser reads TOML v1.0.0: the whole grammar, all four date-time types
through `ghoti.io-chron`, and every one of the four redefinition rules as a
separate check. There is no writer yet. Every claim below was checked by
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

A parsed document belongs to the root `GTEXT_TOML_Value *` and is released by
one `gtext_toml_free()` of it. Every node carries the allocator it was made
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
on an explicit stack and the teardown walks a worklist, so neither the parse
nor the free uses the C stack for nesting. A 100,000-deep document is parsed,
walked and freed in the test suite.

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
| Writing | - | **there is no writer**; see Not implemented |

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
`tools/conformance/TOML_SUITE_COMMIT`. `make conformance-toml`.

```
valid    208 of 208
invalid  501 of 501 refused
total    709 of 709  (100.0%)
```

Three things about that score, because a clean one is where this page is least
useful if it stops:

- **How much of the corpus was asked, beside how much passed.** All 709 cases
  the suite's own `tests/files-toml-1.0.0` manifest lists, with nothing
  skipped. The manifest rather than a directory walk: the tree also holds the
  1.1.0 cases, and scoring a 1.0.0 parser against those reads as a parser
  defect.
- **501 of the 709 are invalid**, so the majority of this corpus walks the
  refusal paths rather than the happy one. That is unusual and it is the most
  valuable thing about it.
- **The gate has been seen to fail.** Five defects were planted one at a time
  and each was caught: accepting a lone CR as a newline (1 case), dropping the
  leading-zero check (9), letting a header redefine a header-defined table
  (12), skipping the up-front UTF-8 validation (2), and accepting a duplicate
  key (13). The two thin ones are why `tests/test-toml.cpp` covers the CR rule
  and the non-string UTF-8 rule directly: a check one corpus case away from
  being untested will be deleted one day by someone who cannot see what it is
  for.

**`tests/test-toml.cpp`**, 24 tests, for what the corpus cannot see: accessors
against the wrong type and against NULL, definition order, key identity and
NUL-containing keys, the eight redefinition rows above, the `int64_t` bounds
from both sides and one past each, locale independence, error line and column,
`max_depth` and `max_total_bytes`, a 100,000-deep document parsed and freed,
file parsing, and each 1.1.0 addition refused with the specific code that says
which kind of refusal it is - so when the option lands, these tests change
rather than continuing to pass for the wrong reason.

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

**What nothing checks yet.** There is no fuzz harness for this module, and no
differential that puts generated documents to both this parser and `tomllib`.
The corpus scores the cases someone chose; neither of those exists yet, so
nothing scores the cases nobody chose.

@anchor toml_not_implemented
## Not implemented

- **The writer.** Phase 2. toml-test has an encoder mode - tagged JSON in,
  TOML out, handed to a separate decoder - so the corpus already here measures
  it, and there is no round-trip-only excuse available.
- **TOML 1.1.0, behind an option.** Phase 3. `make conformance-toml-next`
  scores this parser against the 1.1.0 manifest today and prints the work as a
  list: **701 of 712**, with eleven valid cases refused - `\e` and `\xHH`
  escapes, a newline or a trailing comma inside an inline table, and a time
  without seconds. All 494 invalid cases are still refused, including
  `invalid/control/multi-cr` and `rawmulti-cr`, which 1.1.0 newly forbids and
  this parser already does. That asymmetry is the reason the option cannot be
  written as "relax some checks".
- **A pull reader / event API**, as JSON, CSV and YAML have. TOML's semantics
  are whole-document - a header can reopen a table defined earlier, and dotted
  keys interleave - so an event stream is not simply a linearisation of the
  file and needs its own design decision first.
- **Comments in the DOM.** The YAML DOM carries leading and inline comments;
  this one does not yet.
- **`toml_to_json` / `json_to_toml`**, beside the existing `yaml_to_json`.

---

Back to \ref text_format_references "Formats".
