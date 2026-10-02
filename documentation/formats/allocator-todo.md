@page format_allocator_todo Allocators

# Allocators

`GTEXT_JSON_Parse_Options::allocator` routes a whole JSON parse through a
caller-supplied `GTEXT_Allocator`, which is cutil's `GCU_Allocator` under a
local name, and **`GTEXT_JSON_Write_Options::allocator` now covers the writer's
own working memory**. **CSV, YAML and INI also do the same for parsing**, through
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

## A correction: only the *sink* is exempt, not the writer

The CSV and YAML sections above say the writer is deliberately exempt, and that
was wider than the argument supporting it. The argument is about the **sink**:
`gtext_json_sink_buffer()` takes no options, so a sink is created before any
allocator is named and outlives the write, and routing its buffer through a
caller allocator would mean freeing through whichever options happened to be
passed last. That is sound, and it is about the sink alone.

A writer's *working memory* is a different thing. It is allocated and released
inside one call, from options the caller supplied, and nothing about it predates
anything. **INI already drew that line correctly** -
`GTEXT_INI_Write_Options::allocator` is "the allocator the writer's own working
memory comes from... Not the sink's: a buffer sink owns its buffer" - so the
exemption had already been superseded by the newest module while this page still
recorded it as a decision in force. That is the shape worth noticing: a
documented decision that a later module quietly improved on reads exactly like a
decision still standing.

## JSON writer: done

`GTEXT_JSON_Write_Options::allocator` covers the scratch a write takes and gives
back - the frame stack the value walk carries, and the sorted index array a
`sort_object_keys` write builds per object - plus, for the incremental API, the
`GTEXT_JSON_Writer` handle and its stack, which `gtext_json_writer_new()`
allocates and `gtext_json_writer_free()` releases through the same one.
`src/json/json_writer.c` is in `ALLOCATOR_CLEAN_SOURCES`.

The handle reads its allocator from the **options argument**, not from the copy
it stores, because the handle is allocated before that copy exists - the
`csv_field_buffer_init()` lesson in a new place, where an allocator assigned
after the allocation it serves is the bug. `gtext_json_writer_free()` then reads
the stored copy, which is sound because it is set once and never cleared.

The sink allocations carry `allocator-exempt` markers, so the gate passes them
deliberately rather than by omission.

### What the controls showed

Three defects planted, three caught, and two of them taught something:

- indices from the C library and freed through the allocator - caught by the
  tracking allocator's guard word, "freed a block this allocator never made".
- the frame growth bypassing the allocator *consistently*, so nothing is
  mismatched and the counters simply stay at zero. Caught by asserting the
  allocator was **used** (`total_allocations > 0`) rather than only balanced. A
  bypass is invisible to a balance check, because bypassing is balanced.
- the writer's stack freed by the C library - which does not fail a test, it
  **aborts the process** with `munmap_chunk(): invalid pointer`. The first
  scorer counted `[  FAILED  ]` lines and reported this control as not caught,
  because a crash prints no summary. Same family as "the exit status is the
  verdict".

## JSON Pointer: done

The walk allocates one transient buffer per token - the decoded form, since `~0`
and `~1` mean a token is not always a span of the input - and releases it before
the next token is read. Nothing it allocates outlives the call, so no free
function needs the allocator handed back.

`gtext_json_pointer_get()` and `gtext_json_pointer_get_mut()` take **no options**,
so there was nowhere to put a field.
`gtext_json_pointer_get_with_allocator()` and
`gtext_json_pointer_get_mut_with_allocator()` are the entry points that name one,
the shape `gtext_csv_new_table_with_allocator()` already set for exactly this
situation; the original two delegate with NULL and are unchanged.
`src/json/json_pointer.c` is in `ALLOCATOR_CLEAN_SOURCES`.

Threading it through `json_pointer_evaluate()` is also what `json_patch.c` will
need: every pointer walk a patch performs happens inside a call the caller gave
an allocator to, so the internal signature had to grow whether or not the public
one did.

### What the controls showed

Three planted, three caught, and the second is why the failure-path test exists:

- the buffer from the C library and freed through the allocator - the guard word,
  sixteen times over eight tests.
- **one error path leaking it.** Six of the walk's seven returns happen with the
  buffer live, and a pointer that *resolves* takes none of them - so the success
  test passes while a leak sits in the miss case. Only
  `JsonPointerBalancesWhenTheWalkFails`, which walks four different ways to miss,
  fails on this.
- the entry points dropping the allocator on the way in, which balances
  perfectly and is caught by asserting the allocator was used.

One of my own: the first version of the NULL-fallback test built a tracking
allocator, passed NULL, and asserted it served nothing - true however the code
behaves, since an allocator that is not passed cannot be reached. `-Werror`
reported it as an unused variable, which is the compiler noticing a vacuous test
before I did. It compares the three spellings' answers now.

## JSON streaming parser: done

The field was already there and already reachable. `gtext_json_stream_new()`
takes `GTEXT_JSON_Parse_Options` - the same structure the parser takes, with the
same `allocator` member - and read nothing from it. So this was never "an entry
point that takes no allocator": it was one that accepted an allocator and
dropped it, which is strictly worse, because a caller who sets the field has
nothing to notice.

(The paragraph this section replaced said the streaming parser "does not share
the parse options". It does share them. That sentence is why this looked like
the same size of job as Patch and Schema, which genuinely have nowhere to put
the field.)

Converted, in dependency order rather than file order, because the stream's
memory is spread across three files:

- `json_utils.c` - `json_buffer_grow_unified()`, the one shared growth helper,
  gained a trailing allocator parameter. Both its callers are on the stream's
  path, so nothing else had to change.
- `json_stream_buffer.c` - `json_token_buffer` records its allocator, set by
  `json_token_buffer_init()` **as a parameter**. The same shape as
  `csv_field_buffer::alloc` and for the same reason: the site that grows the
  buffer and the site that releases it must be unable to name different
  allocators.
- `json_stream.c` - the stream structure, its 4096-byte input buffer, the state
  stack, each object's key-name array and each key copy. The handle reads
  `opt->allocator` into a local before the `calloc` that creates the structure
  that will hold the copy, the way `gtext_json_writer_new()` and
  `gtext_csv_stream_new()` already do.

`src/json/json_stream.c`, `src/json/json_stream_buffer.c` and
`src/json/json_utils.c` are now on `ALLOCATOR_CLEAN_SOURCES`. One exemption:
the `free()` of `GTEXT_JSON_Error::context_snippet`, per the section below.

### What the gate could not see

`src/json/json_pull_reader.c` was **already** on `ALLOCATOR_CLEAN_SOURCES` and
had been passing. It routes its own structure, its event queue and its key
copies through `opts->allocator`, and then calls
`gtext_json_stream_new(opts, ...)`. Everything under that call came from the C
library.

`make check-allocators` greps a listed file for direct `malloc`, `calloc`,
`realloc` and `free`. That is a sound predicate. The sentence people read off a
passing run - *this component's memory comes from the caller's allocator* - is a
different one, and the two come apart the moment a listed file allocates by
calling something that is not listed. A per-file grep cannot see a call.

No test covered it either, and the shape of that hole is worth keeping:
`tests/test-allocator.cpp` had `CsvStream` + `CsvPullReader` and `YamlStream` +
`YamlPullReader`, and neither of the JSON pair. Three formats with the same
two-layer shape, two covered on both layers, one on neither - readable from the
test names without knowing anything about the defect.

### What the controls showed

A balance assertion alone would not have caught this, which is why these tests
assert a floor on bytes outstanding while the stream is alive rather than only
`live_blocks == 0` at the end. `live_blocks == 0` holds when the allocator
serves nothing, and `total_allocations > 0` holds from the wrapper's own
structure while everything beneath it bypasses.

| control | result |
| --- | --- |
| A. stream structure and input buffer back on the C library (the historical defect) | rc=1, all four tests fail. `JsonPullReaderBalances` reports `live_bytes` **80 vs 4096**: the reader's own structure was served and the stream's buffer was not - the exact gap `total_allocations > 0` would have passed |
| B. token buffer freed through the C library | rc=134, `munmap_chunk(): invalid pointer`. Caught by glibc *before* gtest adjudicates, so it prints no summary at all and a scorer counting `[  FAILED  ]` lines reads it as zero failures |
| C. `gtext_json_stream_free()` stops releasing the key-name arrays | rc=1, caught **only** by `JsonStreamBalancesWhenTheInputIsRefused`. A well-formed document pops its own stack, so the success-path test never reaches that arm |
| D. `json_buffer_grow_unified()` ignores the allocator it is given | rc=1, all four fail |

Two further mutations never got as far as running: a constant loop bound tripped
`-Werror=type-limits` and an unused allocator parameter tripped
`-Werror=unused-parameter`. The compiler refuses those two spellings of the
defect outright, which is worth knowing but is not a statement about the tests -
both controls had to be rewritten into forms that compile before they measured
anything.

## JSON Patch: done

No API change, as predicted: `gtext_json_patch_apply()` and
`gtext_json_merge_patch()` are handed a DOM, every `GTEXT_JSON_Value` carries
`ctx`, and `json_context::alloc` is never NULL. All forty raw calls sat in three
functions - `json_patch_find_parent_and_token()`, `json_patch_add()` and
`json_patch_remove()` - each of which already takes `root`, so the conversion
was `root->ctx->alloc` at three function tops and ten distinct call spellings
rewritten.

### The larger half was not the token buffers

Writing a test for the conversion found a defect the conversion would not have
touched. Both entry points clone the whole document for atomicity, and built the
clone's context with

```c
json_context * clone_ctx = json_context_new(NULL);
```

so **a deep copy of the entire document came from the C library on every patch
call**, discarding an allocator that was sitting in `root->ctx`. That is far
more memory than every token buffer in the file put together, and for an arena
caller it is the allocation they would most want inside it. It also decided
what the operations allocated through, because they read the allocator from the
tree they are given and that tree is the clone - so converting the token buffers
without this would have left them correctly reading the *wrong* allocator.

`gtext_json_clone()` had it too, through `json_value_clone_into_new_context()`
in `json_dom.c`: a public entry point that takes no options, whose only possible
allocator is the source's, passing NULL. Self-consistent - the clone records the
context that made it, so `gtext_json_free()` releases through the same one - and
a complete bypass. All three now inherit.

**`json_dom.c` was already on `ALLOCATOR_CLEAN_SOURCES` and passed throughout.**
`json_context_new(NULL)` is not a `malloc` call, so the grep cannot see it. This
is the same blindness as the pull reader, and worth stating as its own shape:
the check sees raw allocation *calls*, so an allocator discarded at an internal
API boundary is invisible to it. Audit for that by grepping the constructor
rather than the allocator, and ask of each site whether an allocator was
available to pass.

`json_context_new(` has **14 call sites**, counted rather than estimated:

| where | count | passes |
| --- | --- | --- |
| `json_dom.c`, the `gtext_json_new_*` builders | 9 | `NULL`, correctly - no caller allocator exists to inherit |
| `json_dom.c`, `gtext_json_clone()` | 1 | the source's, as of this commit |
| `json_patch.c` | 2 | the tree's, as of this commit |
| `json_parser.c` | 1 | `opt->allocator` - the one that was always right |
| `json_schema.c:4739` | 1 | `NULL`, and outstanding - the schema's own context |

One incorrect `NULL` is left, and it belongs with the Schema work below.

**As of the Schema conversion, none is.** The count is still 14, recounted with
comment lines excluded - prose quoting `json_context_new(NULL)` matches a naive
grep, and did: `json_dom.c` 8 `NULL` in the public builders, plus
`json_value_clone_new_context()` and `json_value_new_string_on()` taking one;
`json_parser.c` 1; `json_patch.c` 2; `json_schema.c` 1.

### What the controls showed

| control | result |
| --- | --- |
| A. `patch_apply`'s clone context back to `json_context_new(NULL)` | rc=1, fails `JsonPatchBalancesThroughTheAllocator` and nothing else |
| B. `gtext_json_clone()`'s context back to NULL | rc=1, fails `JsonCloneInheritsTheSourcesAllocator` and nothing else |
| C. the fourteen `token_buf` frees back to the C library | rc=134, `munmap_chunk(): invalid pointer` - caught by glibc before gtest reports, so it prints no summary |

A and B failing exactly one test each is the useful part: the clone fix and the
token-buffer conversion are separately covered, so neither can regress behind
the other.

The first version of `JsonPatchBalancesThroughTheAllocator` is also why the
patch document matters. Its paths were plain object keys, the token buffers are
only allocated for a token that must be decoded or parsed as an index, and the
test reported `88 vs 88` - the patch had allocated nothing at all through the
tree's allocator. The paths now use `~0`, `~1` and numeric indices deliberately.

## JSON Schema: done

One field on the structure that already existed, `GTEXT_JSON_Schema_Options`,
which `gtext_json_schema_compile_with_options()` already took - as
`GTEXT_JSON_Write_Options` did, and not a new structure. 110 raw calls converted,
97 in `json_schema.c` and 13 in `json_uri.c`; four left deliberately.

`gtext_json_schema_validate()` and `gtext_json_schema_free()` take no options and
need none. The allocator is recorded once, on `schema->ctx`, by the
`json_context_new(alloc)` that was `json_context_new(NULL)` - the last incorrect
`NULL` of the fourteen counted in the Patch section - so every function
downstream reads it from `cc->ctx->alloc` or `schema->ctx->alloc` and nothing
defaults it a second time.

### Why Schema names its allocator and Patch inherits one

Patch and `gtext_json_clone()` take the allocator of the tree they are given,
and that is right there: what they produce lives inside that tree and dies with
it. A compiled schema does not. The header promises the schema document may be
freed after compiling, which is the whole reason the schema clones it - so a
schema built from the document's allocator would outlive the lifetime the caller
chose that allocator for. The caller has to say, and the option is where.

That distinction has a test of its own, because one allocator cannot make the
assertion: `JsonSchemaClonesTheDocumentOnItsOwnAllocator` parses the document
through allocator A, compiles through allocator B, and asserts A sees *nothing*
of the compile. With a single allocator a clone taken from either satisfies every
count, so the test would have passed whichever one it came from - which is the
question being asked.

### What the gate could not see, three times

`make check-allocators` greps each listed file for a direct `malloc`. Three
bypasses here were **calls**, so no amount of grepping `json_schema.c` could
find them, and all three were green before they were fixed:

| where | what went to the C library |
| --- | --- |
| `gtext_json_parse(text, len, &popts, ...)` for an embedded meta-schema, with `popts` straight from `gtext_json_parse_options_default()` | all nine documents of the 2020-12 dialect, every time a schema validated another schema |
| `gtext_json_new_string(kname, klen)` in `propertyNames` | one throwaway string value per key of every object the keyword checks |
| `gtext_json_pointer_get(root, ...)` while resolving a `$ref` fragment | JSON Pointer's token buffer, once per path segment - and `gtext_json_pointer_get_with_allocator()` had existed since the Pointer conversion |

The first is measured: compiling `{"$ref": ".../2020-12/schema"}` puts **762,350
bytes in 1,457 allocations** through the caller's allocator now, against
**237,294 in 731** with that one line removed. The difference is the parsed
documents; the compiled nodes and resource tables were always on the allocator,
which is why a balance check saw nothing wrong.

The second needed a new internal entry point rather than a fix in place:
`json_value_new_string_on()` is `gtext_json_new_string()` with the allocator
spelled out, and the public builder now calls it with `NULL`. The public
builders keep taking no allocator - they have no options and so no caller
allocator to inherit - but inside the library, where there is one, they are no
longer used.

### What the controls showed

Five mutations, one per mechanism, each reverting one decision:

| control | reverted | what happened |
| --- | --- | --- |
| schema-context | `json_context_new(alloc)` → `NULL` | **aborted**: `munmap_chunk(): invalid pointer`. Memory from the C library freed through the counting allocator; glibc caught it before the test's own guard assertion could. Recorded as caught-by-glibc, because a crash prints no gtest summary and a scorer counting `[ FAILED ]` lines reads it as zero failures |
| embedded-parse | drop `popts.allocator = alloc` | the meta-schema floor fails |
| propertynames-temp | `json_value_new_string_on` → `gtext_json_new_string` | validation-allocation count fails |
| eval-marks | the `eval_init` call site passes `NULL` | validation-allocation count fails |
| doc-clone | `json_value_clone_new_context(doc, alloc)` → `gtext_json_clone(doc)` | the two-allocator test fails: *compiling took memory from the document's allocator* |

Two of those controls failed to measure anything on their first attempt, and
both failures were in the instrument rather than in the code:

- **The embedded-meta-schema floor was vacuous.** It was first written as
  `>= 100000`, which sits *below both* 762,350 and 237,294 - so the test passed
  with the bypass in place. The control is the only reason that was found; the
  test had been green either way. The floor is now 400 KB, and the comment
  beside it carries both measurements so the next reader can see what it
  separates.
- **The eval-marks mutation did not compile.** Setting `eval->alloc = NULL`
  left the function's `alloc` parameter unused, and `-Werror=unused-parameter`
  refused it. A mutation that does not build measures nothing, so it was moved
  to the call site, where passing `NULL` compiles and is the same revert.

### A check that would have caught the pull reader

Still not built, and still recorded here rather than mistaken for coverage: for
every file on `ALLOCATOR_CLEAN_SOURCES`, the files *it calls into* within the
same component should be on the list too. `json_schema.c` and `json_uri.c` are
both listed now, so the JSON component satisfies it again.

It would not have caught any of the three bypasses above. Each is a call into a
file that was *already listed* - `json_parser.c`, `json_dom.c`,
`json_pointer.c` - through an entry point that takes no allocator. That is a
third shape, after the per-file grep and the callee-not-listed rule: **an
allocator-aware component calling an allocator-blind entry point of an
allocator-aware file.** The audit for it is to grep the *entry points* that take
no allocator and ask of each caller whether one was in hand - which is how all
three were found, and what the byte floors in `tests/test-allocator.cpp` check
now that they are fixed.

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
