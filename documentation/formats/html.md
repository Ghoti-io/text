@page format_html HTML tokenizer (internal)

# HTML tokenizer

**Status: internal.** There is no public HTML API yet. `src/html/html_tokenizer_internal.h`
is the interface the tree builder (Phase 3c) will drive; nothing here is installed or
exported. Back to \ref text_format_references "Formats".

## Specification

WHATWG HTML, "Tokenization" (13.2.5, all 84 states) and "Preprocessing the input
stream" (13.2.3.5), **as served at html.spec.whatwg.org/multipage/parsing.html on
2026-10-11** (Living Standard, last updated 9 October 2026), read state by state. That
text has processing instruction states (13.2.5.72 to 13.2.5.76) that html5lib 1.1 and
parse5 8.0.1 predate. The vectors are html5lib-tests at the commit pinned in
`tools/html/HTML5LIB_TESTS_COMMIT`. The C1 table of the numeric reference end state is
in `html_tokenizer.c`, checked against the Encoding standard's windows-1252 index by a
test and by `numericEntities.test`.

## Interface

Pull-driven: `feed()` a chunk (borrowed until `next()` says `NEED_INPUT`), `next()`
for tokens, `finish()` at the end. A token is delivered the moment it is complete and
nothing past it has been read, so `set_state()` between tokens applies to the very next
character. `set_last_start_tag()`, `allow_cdata` (the option's callback, overridden by
`set_allow_cdata(DEFAULT | ALLOW | REFUSE)`, which stands until changed), and a work counter
for tests.

**Character runs are held.** A run of characters is coalesced, so it is delivered only when
something that is not a character ends it, or the input ends; it is held in memory. A run
longer than `max_text_bytes` is therefore a hard failure (`E_LIMIT_TEXT`), not a split in two
tokens. The default for it is the default for the input (64 MiB), so by default the input limit
is reached first. Text that merely looks like an end tag (`</styl` and ten thousand letters in
RCDATA) is text and has no limit but that one.

**Limits.** Six are failures, each its own sticky status: input bytes fed, name, value,
comment, text run, attributes on a tag. `max_errors` is not one: it counts and drops
(`count_errors_only` keeps none). 0 selects a default and never means unlimited (`SIZE_MAX`
is as near as the type allows). The input limit is the one exception to "the answer is the
same however the input is cut": it is on what has been fed, so a whole input over it fails
before any token and a cut one delivers the tokens before the byte that crosses it.

**Memory.** When a few bytes are waiting for the rest of a character or a look-ahead and a
large chunk arrives, the tail and at most 256 bytes of the chunk are joined and the rest is
read in place; the copy is bounded by the tail and that window, not by the chunk.

**Attribute names** are found by a seeded hash table (a seed per tokenizer, `hash_seed` to
fix it), so a tag of a thousand attributes costs a thousand probes, and names built to collide
for one tokenizer do not for another.

Input is UTF-8; an invalid sequence is one U+FFFD per maximal subpart (the Encoding
standard's decoder) and the non-standard error `invalid-utf-8-in-input-stream`. A byte
order mark is not stripped and other encodings are not read: that is Phase 3f. The
`wtf8` option reads the three-byte forms of lone surrogates, which the html5lib vectors
contain.

## Errors

53 codes: 51 the tokenizer raises, `non-void-html-element-start-tag-with-trailing-solidus`
(the standard's, declared for the tree builder, which owns it), and the non-standard
`invalid-utf-8-in-input-stream`. Each has line, column and byte offset; kept up to
`max_errors`, the rest counted. Positions follow the vectors: the position of the
current input character; the numeric reference end state and a missing semicolon after a
named reference at the character after; `incorrectly-opened-comment` at the character
after the `!`; **columns count UTF-16 code units** (an astral character is 2); lines count
normalized newlines.

## Measured

Numbers are from the gates' output; the commands are named so they can be run again.

- `make conformance-html-tokenizer`: html5lib-tests tokenizer vectors, 7,053 runs (6,827
  cases in their initial states); 7,030 pass and 23 are exceptions with reasons
  (`tools/conformance/HTML_TOKENIZER_EXCEPTIONS`: vectors that predate the processing
  instruction states, put an end-of-file error at the end of a target, order two errors
  differently, or expect the XML infoset coercion). Each run is also cut a byte at a time and
  in pieces of 1 to 9 bytes, read as strict UTF-8 where the input has no lone surrogate, and run
  under seeded tiny limits (what it delivered must be the beginning of the roomy run's answer);
  the answers must be identical, with no exceptions list. The gate is made to fail 31 ways
  (`run-html-tokenizer.sh --controls`).
- `make check-html-oracle` (the tokenizer part, `run-html-oracle.sh --tokens`): differential
  against html5lib 1.1 and parse5 8.0.1, 13,999 inputs in the default population (the vectors'
  inputs, the tree-construction documents, 6,000 generated and mutated). parse5's tokens agree on
  all of them, its errors on all but 30, html5lib's tokens on all but 130; every divergence is
  one of the classified causes in `tools/oracle/html_tokenizer_divergences.txt` (parse5
  miscounts a line feed after `&`, omits unknown-named-reference in attributes, orders a stream
  error after reference errors, throws on two lone low surrogates; html5lib names a missing
  DOCTYPE name `""`, replaces NUL in CDATA, and keeps the comment-start state on NUL). The
  list describes the default population only; a hunt is `html_tokenizer_diff.py --count N
  --seed S` (three of 100,000 on seeds 21 to 23 were run for this page: nothing unclassified).
  27 controls: the answer corrupted five ways, the list three, 19 planted tokenizer defects,
  and the two that probe the attribute-value scanner.
- **Known gap: processing instructions have no differential coverage.** Neither reference has
  the states, so inputs containing `<?` are left out (except PLAINTEXT) and the vectors are the
  only external check. A hand-written test (`ProcessingInstructionsEndWhereTheBogusComment...`)
  checks where they end.
- `make fuzz-html-tokenizer`: `fuzz_html_tokenizer` (whole, byte-at-a-time, pieces; token
  properties; limits; allocation failure at every point; linear work), 2,980 seeds from the
  vectors plus 7 recorded reproducers and a 900-attribute seed, replayed by `make test`.
  `make check-html-fuzz-plants` plants 8 defects: 6 are *replayed* from a recorded input (that
  shows the harness can see the defect), 2 (`whitespace-flag`, `upper-case-tag`) are *searched
  for* from the seeds within the time budget, and so is the unedited control, which must stay
  silent.
- `tests/test-html-tokenizer.cpp` (83): every row of the specification's matrix, every two-way
  cut of a corpus in each state, an out-of-memory failure at every allocation, work counts with
  the limits raised.
- Work: fed a byte at a time the tokenizer does at most `GTEXT_HTML_TOKENIZER_WORK_BOUND(n)`
  (48 n + 4096) units; the unit tests, the fuzzer and this page use that one bound. No
  throughput figure is claimed.

## Not done

Tree construction, a public API, encoding determination (3f), scripting beyond the states a
tree builder sets.
