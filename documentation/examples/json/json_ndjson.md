@page example_json_ndjson JSON: NDJSON / JSON Lines

# JSON: one input, several JSON texts

A JSON text is one value. RFC 8259 says so, and this parser refuses anything
after it - which is right for a document and wrong for a log, an export or a
network stream, where the bytes are a *sequence* of values. This example sets
`GTEXT_JSON_Parse_Options::records` and reads the same bytes four ways.

## What This Example Demonstrates

- **The four modes, and why it is an enumeration** - `OFF`, `WHITESPACE`,
  `LINE` (NDJSON / JSON Lines) and `SEQ` (RFC 7464). They disagree about inputs
  that occur, which the run prints: records run together with no separator, and
  a value printed across lines, are accepted by one mode and refused by another.
- **`GTEXT_JSON_EVT_RECORD_END`** - the event that says where a record ended.
  The others cannot: `1 2` is two records and emits two number events, which is
  also what the single value `[1,2]` emits between its array markers.
- **The DOM half** - `gtext_json_parse_multiple()` in a loop reads a sequence
  one record at a time, never holding more than one.
- **Writing records back** - `GTEXT_JSON_Write_Options::records` is the same
  enumeration, so a program that reads this format and writes it names the
  format once.
- **That the answer does not depend on the chunking** - the same sequence read
  one byte at a time gives the same records.

## Use Case

Use this example when you need to:
- Read or write NDJSON / JSON Lines
- Read RFC 7464 `application/json-seq`
- Process a stream whose records arrive one at a time
- Choose between the three readings for data of known provenance

## Source Code

@include examples/json/json_ndjson.c

## Key API Functions Used

- `GTEXT_JSON_Parse_Options::records` - which reading of a sequence is meant
- `gtext_json_stream_feed()` - emits `GTEXT_JSON_EVT_RECORD_END` per record
- `gtext_json_parse_multiple()` - one value, and where the next begins
- `GTEXT_JSON_Write_Options::records` - the framing to write

## Important Notes

**Pick the mode from the producer, not for convenience.** Under
`GTEXT_JSON_RECORDS_WHITESPACE` a file whose records a buggy writer ran
together with no separator at all is accepted, and a reader expecting one
record per line then sees fewer records than there are lines.
`GTEXT_JSON_RECORDS_LINE` is the only mode that can say "one value per line".

**Some modes refuse some parse options**, with `GTEXT_JSON_E_INVALID`, rather
than making a promise with a hole in it. `LINE` refuses
`allow_unescaped_controls`, `allow_line_continuations` and `allow_comments`:
each puts a line end somewhere the rule cannot see it or cannot allow it. `SEQ`
refuses `allow_comments`, because an RS inside a comment is framing to a reader
that slices on it and content to one that does not.
`GTEXT_JSON_RECORDS_WHITESPACE` accepts comments between records, and is where
a caller who wants both is routed.

**`gtext_json_parse()` is not part of this.** It returns one value and has
nowhere to put a second, so it refuses trailing content whatever `records`
says.

## Related Examples

- [json_stream.c](@ref example_json_stream) - the streaming parser itself
- [json_basic.c](@ref example_json_basic) - Basic DOM parsing
