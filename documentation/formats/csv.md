@page format_csv CSV

# CSV

CSV has a specification that almost nobody writes and almost nobody reads.
RFC 4180 describes one dialect; real files are a family of dialects that
disagree about quoting, line endings, whitespace and whether rows must be
rectangular. This parser implements RFC 4180 as its default and exposes the
disagreements as explicit options rather than guessing. Back to
\ref text_format_references "Format and specification references".

## Normative references

- **Specification:** [RFC 4180](https://www.rfc-editor.org/rfc/rfc4180),
  *Common Format and MIME Type for Comma-Separated Values (CSV) Files*,
  October 2005. It is Informational, not a Standards Track document, and it
  says so: it documents the format "as it is used" rather than defining it.
- **Erratum:** RFC 4180 was updated by
  [RFC 7111](https://www.rfc-editor.org/rfc/rfc7111) only to add fragment
  identifiers for selecting rows and columns from a CSV resource. That is a
  URI concern, not a grammar one, and is not implemented here.
- Everything outside RFC 4180's grammar - the semicolon dialect, tab
  separation, backslash escaping, comment lines, ragged rows - has **no
  specification at all**. Where this page describes those, the description is
  the specification as far as this library is concerned.

Parser allocations are owned by the returned `GTEXT_CSV_Table` and released
by `gtext_csv_free_table()`. Error context snippets are separately owned and
released by `gtext_csv_error_free()`.

## Parts implemented

**The RFC 4180 grammar (§2).** Records separated by CRLF; fields separated by
commas; fields optionally enclosed in double quotes; a quoted field may
contain commas, CRLF and doubled quotes. All of it, and it is the default
dialect.

RFC 4180 §2 rules 1-7 are enforced with two deliberate relaxations, both on
by default because refusing them would reject most CSV in existence:

- **Bare LF is accepted as a record separator.** §2 specifies CRLF. `accept_lf`
  defaults to `true`, `accept_crlf` to `true`. Turning `accept_lf` off gives
  the letter of the RFC.
- **The last record need not end in a line break**, which §2 already permits,
  quoted or not.

**Bare CR is *not* accepted by default.** `accept_cr` defaults to `false`, so
a classic Mac-style file fails with `GTEXT_CSV_E_INVALID` and the message
"Newline in unquoted field" rather than being silently read as one enormous
row. This is the one place the parser is stricter than most, and it is
deliberate: a lone CR is far more often a corrupted CRLF than an intended
separator.

**Dialect options**, all on `GTEXT_CSV_Dialect`:

| Option | Default | Effect |
|---|---|---|
| `delimiter` | `,` | any single byte; `;` and tab are the common alternatives |
| `quote` | double quote | any single byte |
| `escape` | `DOUBLED_QUOTE` | or `BACKSLASH`, or `NONE` |
| `newline_in_quotes` | `true` | when false, a newline inside a quoted field is `GTEXT_CSV_E_INVALID` |
| `accept_lf` | `true` | relaxation, see above |
| `accept_crlf` | `true` | RFC 4180 §2 rule 1 |
| `accept_cr` | `false` | see above |
| `trim_unquoted_fields` | `false` | when true, strips leading and trailing spaces and tabs from unquoted fields; quoted fields are untouched |
| `allow_space_after_delimiter` | `false` | when off, the space is kept as field data |
| `allow_unquoted_quotes` | `false` | a quote inside a bare field |
| `allow_unquoted_newlines` | `false` | a CR or LF that is not a terminator; see [Deviations](#csv-deviations) |
| `allow_comments` | `false` | with `comment_prefix`, default `"#"` |
| `treat_first_row_as_header` | `false` | RFC 4180 §2 rule 3, and the `Content-Type` `header` parameter |
| `header_dup_mode` | `FIRST_WINS` | or `ERROR`, `LAST_WINS`, `COLLECT` |

Note that `allow_space_after_delimiter` off does **not** make ` b` an error in
`a, b`; it makes the space part of the field, which is what RFC 4180 §2 rule 4
requires ("Spaces are considered part of a field and should not be ignored").
The option controls whether the space may be *discarded*, not whether it may
appear.

**Irregular rows.** RFC 4180 §2 rule 4 requires every record to contain the
same number of fields. Real exports routinely violate it, so the table model
accepts ragged rows, reports them through `gtext_csv_has_irregular_rows()`,
and offers `gtext_csv_normalize_rows()` and `gtext_csv_normalize_to_max()` to
square them up. Nothing is padded or truncated behind the caller's back.

**BOM.** A leading UTF-8 BOM is stripped by default; `keep_bom` retains it as
field data.

**Streaming.** An event-driven parser covering the same grammar, for inputs
that should not be materialized as a table. The result does not depend on how
the caller divides the input into chunks.

**Files.** `gtext_csv_parse_file()` and `gtext_csv_write_file()`. Reading is
incremental, so a pipe or `/dev/stdin` works and `max_total_bytes` is enforced
before the whole file is in memory. Writing is atomic - a temporary file
beside the destination, renamed over it once complete - so an interrupted
write leaves the previous file intact.

## Limits

| Option | Default |
|---|---|
| `max_rows` | 10,000,000 |
| `max_cols` | 100,000 |
| `max_field_bytes` | 16 MiB |
| `max_record_bytes` | 64 MiB |
| `max_total_bytes` | 1 GiB |

Error context snippets are generated by default, with
`context_radius_bytes` of 40 either side. Clear `enable_context_snippet` to
suppress them and the allocation they need; `0` for the radius means the
default, as it does for the limits above. Every limit in the table is checked
against a document that exceeds it, at the boundary.

## Save

The writer emits RFC 4180 by default: comma delimiter, double-quote quoting, doubled
quotes for escaping, and quoting applied only where the field requires it -
`quote_if_needed`. A field is quoted when it contains the delimiter, a quote,
or a line break. `quote_all_fields` and `quote_empty_fields` force the issue
where a downstream consumer is fussier than the format.

`quoting` names the policy as a whole, with the four values Python's `csv`
module uses: `GTEXT_CSV_QUOTE_MINIMAL` (the default, and RFC 4180's own rule),
`_ALL`, `_NONNUMERIC` and `_NONE`. `MINIMAL` is zero, so a zero-initialized
options structure means what it always meant, and `quote_all_fields` still wins
when set, so code written before the enum existed behaves as it did.

`NONNUMERIC` asks a question about the bytes rather than about a type. Nothing
in this module infers types - fields are bytes, deliberately - so where Python
asks whether the value is an `int` or a `float`, this asks whether the text
would be read as a number:

    [+-]? ( digits ( '.' digits? )? | '.' digits ) ( [eE] [+-]? digits )?

matched against the whole field. Narrower than C's `strtod`: no hex floats, no
`inf`, no `nan`, no surrounding space, and an empty field is not a number.
`+1`, `007`, `1.` and `.5` are, because spreadsheets write them. The policy can
only ever add quotes, never remove them, which matters when the dialect's
delimiter is one of the characters a number may contain - a `.` delimiter makes
`1.5` both numeric and unwritable bare, so it is quoted.

The newline string is configurable. `trailing_newline` decides whether the
last record ends in one, and `trim_trailing_empty_fields` drops the empty
fields at the end of a record, for consumers that treat a trailing delimiter
as an error.

Both of those are **row- and table-level questions handed to a writer that may
only see one field at a time**, and the streaming writer honoured neither
until it was made to. Which record is the last is not known at
`gtext_csv_writer_record_end()` - only at `gtext_csv_writer_finish()` - so the
record separator is now held back and emitted either when the next record
begins or, if the option asks, as the terminator at `finish()`. Not after a
failure: a record whose field was refused was never written, and emitting the
terminator would leave a caller who ignores the status with a file holding one
stray newline and no data. Whether an empty field is a *trailing* one is not
known until the next field arrives, so an empty field is counted rather than
written, a following field flushes the run through the ordinary field path -
`quote_empty_fields` and the quoting policy still reach them - and
`record_end()` discards what is left. A record of nothing but empty fields
therefore writes no field at all, which is what the table writer does for the
same row.

**The writer refuses a field it cannot represent.** Quoting is not a
presentation choice in CSV; it is the only thing that carries the delimiter, a
CR or an LF through a field. Neither escape mode is an alternative - both
`GTEXT_CSV_ESCAPE_DOUBLED_QUOTE` and `GTEXT_CSV_ESCAPE_BACKSLASH` concern the
quote character alone, so this parser reads `a\,b` as the two fields `a\` and
`b` under either. So a caller who clears `quote_if_needed`, `quote_all_fields`
and `quote_empty_fields` and then writes such a field has asked for a document
that cannot exist, and gets `GTEXT_CSV_E_UNQUOTABLE_FIELD`.

`gtext_csv_write_table()` checks the whole table before the first byte reaches
the sink, so a refusal leaves the sink untouched rather than half a file. The
streaming writer is handed one field at a time and has nothing to look ahead
at, so there the refusal is per-field and whatever was already written stays
written.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|---|---|---|
| Comma-delimited records | yes, default | |
| CRLF separator | yes, default | |
| Bare LF | accepted by default | `accept_lf = false` for strict §2 |
| Bare CR | **off by default** | `GTEXT_CSV_E_INVALID`, "Newline in unquoted field" |
| Quoted fields | yes | |
| Delimiter inside quotes | yes | |
| Line break inside quotes | yes, `newline_in_quotes` | |
| Doubled quote escape | yes, default | |
| Backslash escape | opt-in dialect | not RFC 4180 |
| Unterminated quote | | `GTEXT_CSV_E_UNTERMINATED_QUOTE` |
| Quote in an unquoted field | opt-in | `GTEXT_CSV_E_UNEXPECTED_QUOTE` |
| Stray quote inside a quoted field | | `GTEXT_CSV_E_INVALID`, "must be followed by quote, delimiter, or newline" |
| Ragged rows | accepted and reported | never silently padded |
| Empty input | 0 rows, not an error | |
| Input of a single newline | 0 rows, not an error | |
| Trailing delimiter | yields a final empty field | |
| Leading BOM | stripped by default | |
| Invalid UTF-8 | rejected by default | `GTEXT_CSV_E_INVALID_UTF8`, including the streaming parser and a sequence split across chunks |
| Size limits | five, configurable | `GTEXT_CSV_E_LIMIT`, `E_TOO_MANY_ROWS`, `E_TOO_MANY_COLS` |

@anchor csv-deviations
## Deviations

**`allow_unquoted_newlines` applies to a CR or LF that is not a record terminator.** `accept_lf`, `accept_crlf` and `accept_cr` decide which bytes end a record. The option says what to do with a CR or LF that is left over. With `accept_cr` false, which is the default:

| Input | Option off | Option on |
|---|---|---|
| `a,b\rc` | rejected, bare CR | one record, second field `b\rc` |
| `aB\r\n` | one record, field `aB` | unchanged; CRLF is a terminator |
| `a\r\nB\r\n` | two records | unchanged |
| `a\r\r\nb` | rejected | `a\r` then `b`: the bare CR is content, the CRLF ends the record |

Setting `accept_cr` makes the first row two records. The dialect decides what a terminator is, and the option decides what happens to everything else.

**Inside a quoted field, `""` is one quote**, and the field continues until a quote that is not doubled. `field1,"text"",field2` is an unterminated quoted field. Python's `csv` returns `field1` and `text",field2`.

**Bare CR is rejected by default**, which is stricter than Python's `csv` module.

## Tested scope

**Fixtures** live in `tests/data/csv/`, hand-written and grouped by intent:
`strict/` for the RFC 4180 grammar, `dialects/` for semicolon, TSV and
backslash-escape variants, `edge-cases/` for BOM, the three line endings,
empty tables, empty and consecutive-empty fields and ragged rows, and
`invalid/` for each rejection.

**Direct behavioral check.** The compliance checklist was produced by parsing
each literal input with `gtext_csv_parse_options_default()` and recording the
status and the resulting field bytes.

**Fuzzing.** `tests/fuzz/fuzz_csv.cpp` under libFuzzer with ASan and UBSan,
seeded from `tests/fuzz/corpus/csv/`. CSV's harness consumes the first *two*
input bytes as an options selector, since the delimiter and quote characters
have to come from somewhere for the dialect paths to be reachable.

It parses each document **both** ways and compares, so a wrong answer fails as
loudly as a crash. Chunk sizes come from the document's own bytes, letting the
fuzzer steer a boundary onto whichever byte breaks the parser, and each chunk
is copied through a scratch buffer that is overwritten immediately afterwards,
so a retained pointer is caught rather than tolerated. It drives both
`gtext_csv_parse_table()` and the streaming parser.

**Reach of the oracles, and where it ends.** `make conformance-csv` scores
this parser against [csv-spectrum](https://github.com/maxogden/csv-spectrum),
cloned on first use and pinned to the commit named in
`tools/conformance/CSV_SUITE_COMMIT`. Of the twelve cases it ships, **eleven
are checkable and all eleven pass** under
`gtext_csv_parse_options_default()`, so the figure to quote is "11 of the 11
checkable, out of 12 shipped" and never a bare 100%. The twelfth,
`location_coordinates`, is excluded because the suite's own fixture is a bare
object where every other one is an array of rows, and the phone number in it
is not the number in its own csv - a defect in the suite rather than an
answer about this parser, so the scorer reports it as unusable instead of
counting it against the parser. The scorer reports separately what
`dialect.allow_unquoted_quotes` would change, so a disagreement that is a
dialect rather than the grammar reads as one.

**The pass rate is scored, not enforced.** `CSS_MIN` and `CSS_MIN_CORPUS`
make the scorer exit non-zero below a given pass rate or corpus fraction, and
the `conformance-csv` target sets neither, so the gate prints its number and
then succeeds whatever the number is. `conformance-roundtrip` and
`conformance-fastpath` pass floors of 100; `conformance`, `conformance-json`
and `conformance-csv` pass none. A score nobody is held to is a report rather
than a gate.

There is still **no differential test against another parser**. That matters
less for CSV than it would for JSON, because there is no authority to be
differentially correct against - but Python's `csv` module encodes a
widely-held reading of the ambiguous cases, and the comparison against it
recorded above was made by hand, once, rather than by anything that runs
again.

**The two writers are compared directly.**
`CsvWriterAgreement.BothWritersAgreeOverTheOptionAndShapeMatrix` in
`tests/test-writer-agreement.cpp` writes the same rows through
`gtext_csv_write_table()` and through `gtext_csv_writer_*()` over 352
combinations of options and shapes, comparing the bytes and the verdict, and
`tests/fuzz/fuzz_csv_writer.cpp` states the same property over fields nobody
chose - a field holding the delimiter, the quote character, a bare CR, a NUL
or bytes that are not UTF-8, and an empty field in every position, which is
where `fuzz_csv.cpp` cannot reach because it only writes what it parsed.
Rows are given a uniform field count deliberately: `gtext_csv_row_append()`
refuses a ragged row, so a ragged shape would hand the two writers different
content and read as a disagreement in the library.

Round-tripping is asserted only where the policy promises one.
`GTEXT_CSV_QUOTE_NONE` may refuse a field outright with
`GTEXT_CSV_E_UNQUOTABLE_FIELD`, as **Save** above describes, and with
`always_escape_quotes` cleared a quote inside an unquoted field is emitted
verbatim - readable by a parser with `allow_unquoted_quotes` on, and the
deliberate interoperability choice that option's documentation leaves to the
caller. Both are choices rather than defects, so neither is held to the round
trip.

**Round-trip is now pinned.** `CsvRoundTrip.WriteThenReparsePreservesFields`
in `tests/test-csv.cpp` parses, writes and reparses twelve documents chosen
for difficulty - embedded delimiters, doubled quotes, embedded newlines,
empty and consecutive-empty fields, a trailing delimiter, preserved
surrounding spaces, CRLF, ragged rows, multi-byte UTF-8, a field whose only
content is a quote character, and a record of nothing but delimiters - and
requires the fields to come back identical. The property held before the test
was written, so this pins existing behavior rather than recording a fix.

## Dialect sniffing

`gtext_csv_sniff()` guesses by parsing with each candidate rather than by
counting characters. A frequency count reads the `;` in
`name,"Smith; John",42` as structure, and cannot see that `a,b;c / d,e,f;g`
is regular under `;` and ragged under `,` even though `,` is twice as common.
Parsing also means the sniffer cannot disagree with the parser that reads the
document next, because it is the same one.

It tries `,` `;` tab `|` `:` and the two quote characters, reads the newline
style directly rather than scoring it, and drops a truncated final record so
that sniffing the first few kilobytes of a large file works. It refuses
rather than guessing when no candidate splits the sample or when two explain
it equally well.

`treat_first_row_as_header` is not guessed. Whether the first row names the
columns is a question about meaning, and no arrangement of bytes settles it.

Named dialects are exported too: `gtext_csv_dialect_tsv()`, `_semicolon()`,
`_backslash_escape()`, `_excel()` and `_permissive()`, each differing from
`gtext_csv_dialect_default()` only in the field it names.

## Pull reader

`gtext_csv_reader_new()`, `gtext_csv_reader_feed()`, `gtext_csv_reader_next()`
and `gtext_csv_reader_free()` wrap the streaming parser. The reader copies
each event's bytes into its queue. The push callback's `data` points into the
caller's chunk or into a field buffer about to be reused, so a reader that
kept the pointer would hand back overwritten memory.

## Not implemented

- **RFC 7111 fragment identifiers** (`#row=`, `#col=`, `#cell=`). A URI
  feature, not a parse.
- **Type inference.** Fields are bytes. Nothing converts them to numbers or
  dates. `GTEXT_CSV_QUOTE_NONNUMERIC` asks whether a field's text spells a
  number when deciding whether to quote it, which is a question about bytes.

---

Back to \ref text_format_references "Format and specification references".
