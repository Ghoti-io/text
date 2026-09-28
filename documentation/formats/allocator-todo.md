@page format_allocator_todo Allocators

# Allocators

`GTEXT_JSON_Parse_Options::allocator` routes a whole JSON parse through a
caller-supplied `GTEXT_Allocator`, which is cutil's `GCU_Allocator` under a
local name. **CSV, YAML and INI now do the same**, through
`GTEXT_CSV_Parse_Options::allocator`, `GTEXT_YAML_Parse_Options::allocator` and
`GTEXT_INI_Parse_Options::allocator`. INI is the one that covers its *accessors*
as well - `gtext_ini_unescape()`, `gtext_ini_escape()` and
`gtext_ini_value_list()` each take one and each has a matching free function -
because the INI reader hands back raw bytes and the decoding is a separate call,
so an allocator that stopped at the parse would cover less of that module than
of the others. What is left is the JSON entry points other than parsing, listed
at the end.

Two INI allocations are deliberately *not* the caller's, and both are the same
exception the other modules make: an error's `context_snippet`, because an error
outlives the parse and a parse can fail before it has read its options, and a
buffer sink's buffer, because a sink is created before any options exist.

## Why it had to be all or nothing

A partial allocator is worse than none. If `GTEXT_CSV_Parse_Options` gained an
`allocator` field that the arena honored but the table structure did not, then
`gtext_csv_free_table()` would release the arena through the caller's
allocator and the table structure through `free()`. That is not an incomplete
feature; it is heap corruption in a caller who supplies an arena, and the
caller has no way to see it coming.

So the rule is the one `make check-allocators` enforces: a file is added to
`ALLOCATOR_CLEAN_SOURCES` when every allocation in it goes through the
allocator, and the public option is only documented as covering what that list
covers.

The list is the gate's whole field of view, so a file that is clean and *not*
on it is the bad case: correct today, with nothing holding it there. Three were
found in exactly that state - `src/allocator.c`, `src/idna/nfc_utf8.c` and
`src/yaml/json_to_yaml.c`, all three already allocator-clean and none of them
watched. Adding a converted file to the list belongs in the commit that
converts it.

## CSV: done

`GTEXT_CSV_Parse_Options::allocator` covers the parse and everything the table
owns afterwards. `src/csv/csv_table.c`, `src/csv/csv_stream.c` and
`src/csv/csv_stream_buffer.c` are in `ALLOCATOR_CLEAN_SOURCES`, so
`make check-allocators` fails on a raw allocation in any of them. 174 sites
were converted.

The arena and the context each carry the allocator; the arena carries its own
copy because `csv_arena_free()` releases the structure the allocator was read
from, so it has to be captured before the loop rather than read inside it. The
three temporary-array holders - `csv_column_op_temp_arrays`,
`csv_compact_structures` and `csv_clone_structures` - carry it too, set before
their first allocation so that an unwind triggered by that first failure still
has a valid allocator to free through. Clones and compactions take the
allocator of the table they came from, so compacting never moves a caller's
data onto the C heap.

Two public entry points were added, because `gtext_csv_new_table()` and
`gtext_csv_new_table_with_headers()` take no options and so have nowhere to
name an allocator: `gtext_csv_new_table_with_allocator()` and
`gtext_csv_new_table_with_headers_and_allocator()`. The old names delegate to
them with `NULL`.

**Two things are deliberately still on the C library, and neither can mix with
the other side.** `GTEXT_CSV_Error` and its context snippet are released by
`gtext_csv_error_free()`, which is handed an error and no allocator - the same
is true of `GTEXT_JSON_Error`. And the writer - sinks, the writer structure,
and the transient escape buffer - is a separate entry point taking write
options, exactly as the JSON writer is. Neither is ever freed through a
caller's allocator or vice versa. The one exempt site inside a clean file
carries an `allocator-exempt` marker, which is what the gate reads.

### What the conversion actually caught

Worth recording, because both were found by a check rather than by reading.

`csv_field_buffer_init()` ends with `memset(fb, 0, sizeof(*fb))`. The
allocator was being assigned to the field buffer *above* that call, so the
memset wiped it: the buffer then grew through the C library and
`gtext_csv_stream_free()` released it through the caller's allocator. The
tracking allocator's guard word caught it as a block it had never made. The
fix was to make the allocator a parameter of the initializer rather than
something a caller assigns afterwards, so the ordering cannot be got wrong
again.

The other was a gap in the tests, not the code. Planting the exact defect this
page describes - the table structure on the C library while its arena comes
from the caller's allocator - left the whole suite green, because
`csv_create_empty_table()` is reached only by parsing zero bytes and nothing
had ever done that. `Allocator.CsvEmptyInputBalancesThroughTheAllocator`
exists for that path, and fails on the planted defect with
"freed a block this allocator never made".

## YAML: done

`GTEXT_YAML_Parse_Options::allocator` covers every parse entry point -
`gtext_yaml_parse()`, `_parse_all()`, `_parse_json()`, `_parse_partial()`,
`_parse_safe()`, `gtext_yaml_document_new()`, the streaming parser and the pull
reader - along with the scanner behind them, the arena, the alias table, the
DOM manipulation functions, and `gtext_yaml_to_json()`. Twelve files are in
`ALLOCATOR_CLEAN_SOURCES`. Around 280 sites.

The same shape as CSV: the arena carries its own copy of the allocator because
`yaml_arena_free()` releases the structure it was read from; `GTEXT_YAML_DynBuf`
takes its allocator as a parameter of `gtext_yaml_dynbuf_init()` rather than
having one assigned afterwards, which is the CSV lesson applied before it could
bite again; and the clone map, clone stack and resolver stack each carry one,
set at their declaration so no push can precede it.

Two things are deliberately exempt and cannot mix with the rest: the error
structures, released by `gtext_yaml_error_free()` which is handed no allocator,
and the writer. Both match JSON and CSV.

**One exemption is specific to YAML and worth knowing about.**
`gtext_yaml_parse_all()` returns an array of document pointers, and its
published contract - the example in `yaml_dom.h` - has the caller release that
array with plain `free()`. Routing it through a caller's allocator would turn
that documented call into a free through the wrong one, which is heap corruption
in exactly the code that was written against the documentation. So the array
stays on the C library; the documents it points at, which are all the memory of
any size, do not. It is never freed through a caller's allocator anywhere, so
the two still do not mix.

### What the conversion caught

**`strdup()` was invisible to the gate.** `make check-allocators` matched
`malloc`, `calloc`, `realloc` and `free` - so fifteen `strdup()` calls in the
parser and the stream went on allocating from the C library while the frees
beside them were converted. That is a free through the wrong allocator, and it
was caught by the tracking allocator's guard word in four of the new tests
rather than by reading the diff. The gate matches `strdup` and `strndup` now,
and `gtext_yaml_strdup()` exists so the call sites have somewhere to go.

**Two headers declared the same function.** `src/yaml/yaml_internal.h` is the
real one, 631 lines, included by fourteen files; `include/ghoti.io/text/yaml/
yaml_internal.h` is a 47-line stub included by `reader.c` alone, declaring the
character reader. Changing a signature in one left the other stale, and the
compiler reported it as conflicting types rather than as the duplication it is.
Both are updated.

**A regex rewrite put an argument outside the call it belonged to**, turning
`f(a, b)` into `f(a), b` - a comma expression that compiles, discards the call's
result, and tests the wrong thing. `-Werror` caught it as a wrong argument count
in that instance, and the whole conversion was then swept for the shape. The
same pass also rewrote the word `free()` inside a comment. A mechanical change
of this size needs the compiler read carefully rather than trusted to be silent.

## The other JSON entry points

`GTEXT_JSON_Parse_Options::allocator` covers parsing. The JSON writer, the
streaming parser, JSON Pointer, JSON Patch and JSON Schema each have their own
entry points and take no allocator. Each needs an options structure of its own
or an added parameter; none of them shares the parse options.

## The error-snippet exception

`GTEXT_JSON_Error::context_snippet` and its CSV equivalent stay C-library
memory in every plan above. `gtext_json_error_free()` and
`gtext_csv_error_free()` receive only the error, so they cannot learn which
allocator produced the snippet, and freeing it through the wrong one is worse
than the single diagnostic allocation it would save. Changing that means
putting an allocator in the public error structure, which is a decision about
the API rather than an implementation detail. The sites are marked
`allocator-exempt` in the source so the check passes them deliberately rather
than by omission.

---

Back to \ref format_comparison "Comparison with other libraries".
