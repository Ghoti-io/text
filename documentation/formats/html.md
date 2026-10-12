@page format_html HTML tokenizer and tree builder (internal)

# HTML tokenizer and tree builder

**Status: internal.** There is no public HTML API yet. `src/html/html_tokenizer_internal.h`
and `src/html/html_tree_internal.h` are the interfaces of the two stages (the tree builder
drives the tokenizer); nothing here is installed or exported. The tokenizer comes first on this
page, the tree builder after it (\ref format_html_tree "below"). Back to \ref text_format_references "Formats".

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

## Not done (tokenizer)

A public API, encoding determination (3f), scripting beyond the states a tree builder sets.


\anchor format_html_tree
# The tree builder (stage 3c)

`src/html/html_tree.c`, `html_tree_modes.c`, `html_tree_internal.h` (the contract) and
`html_tree_private.h`. It drives the tokenizer above and builds a \ref format_markup "Markup"
document: elements in the HTML namespace with attributes in no namespace, a doctype node,
comments, processing instructions, text with adjacent runs merged, the document mode. Internal
only, as the tokenizer is.

## Specification

WHATWG HTML, "Tree construction" (13.2.4 "Parse state", 13.2.6 "Tree construction"), **as served
at html.spec.whatwg.org/multipage/parsing.html on 2026-10-11** (Living Standard, last updated 9
October 2026, the revision the tokenizer was written from; fetched again for this stage), read mode by mode. Implemented:

| Section | What |
| --- | --- |
| 13.2.4.1 to 13.2.4.5 | the insertion mode, **reset the insertion mode appropriately** (complete: the table, caption, column group and template branches are there, tested over stacks the stage cannot reach), the stack of open elements and its scopes (general, list item, button, table), the list of active formatting elements with **the Noah's Ark clause** (three), reconstruct the active formatting elements, clear up to the last marker, the head, form and frameset-ok state |
| 13.2.6.1, 13.2.6.2, 13.2.6.3 | inserting nodes, the generic raw text and RCDATA parsing algorithms, generate implied end tags |
| 13.2.6.4.1 to 13.2.6.4.8 | the modes initial, before html, before head, in head, in head noscript (the scripting flag is an option, off by default), after head, **in body (every start and end tag rule)**, text |
| 13.2.6.4.17 to 13.2.6.4.21 | after body, in frameset, after frameset, after after body, after after frameset |
| 13.2.6.4.7 | **the adoption agency algorithm**, outer-loop limit 8 and inner-loop limit 3, and "any other end tag" |
| 13.2.6.4.1 | the quirks-mode determination from the doctype (public and system identifier tables) |

Two things in the standard as served that the task statement did not expect, and what was done (the quirks rule
for the 4.01 Frameset and Transitional identifiers says "the system identifier is *missing or the empty string*" in
the copy served 2026-10-11, so an empty one counts as absent for it, as the code has it; absent and empty stay two
states from the tokenizer's flag to the tree's NULL and ""):

- **There is no "in select" insertion mode.** The `select` element is parsed by ordinary "in body" rules
  (`select`, `option`, `optgroup`, `hr`, `input` and the `select` end tag), and `select` is one of the
  boundaries of "has an element in scope". They are implemented, not stubbed, and `<select>` parses.
  (html5lib 1.1 and parse5 8.0.1 parse it by the older rules; their documents with a `select` are left out
  of the differential, and the vectors that hold one are the conformance gate's.)
- **Processing instructions are nodes.** The standard's tokenizer has processing instruction states and the
  tree builder inserts them as nodes (`<?php echo?>` is a processing instruction node, `<?a$>` a comment,
  per the tokenizer); the pinned vectors expect nodes (`processing-instructions.dat`). The task statement said
  "no PIs (bogus comments instead)"; the standard and the vectors win. Neither reference has them.

## Not here yet: the stage gates

`table` (in body), `template` (anywhere) and `svg` and `math` start tags are constructs of the next stages.
A document that reaches one fails with `GTEXT_HTML_TREE_E_NOT_YET`, sticky, no tree, and
`gtext_html_tree_not_yet()` says which (`table` and `template` are stage 3d, foreign content 3e). End tags
for them are ordinary ignored end tags (no template is ever open). The fragment parsing algorithm is 3e.
Not done at all in this stage: a public API, the `<meta>` encoding steps (3f), serialization, scripts.

## Interface

`gtext_html_tree_new(&options)`, `feed()` (any chunking; the chunk is not kept), `finish()`,
`take_document()`, `free()`, or `gtext_html_parse()` for a whole buffer. The tree and the errors are
identical however the input is cut (tested at every split offset, a byte at a time and in random pieces,
on every vector, and in the fuzzer). Options: the allocator (used for the document too), `max_open_elements`
(default 512 and it counts `html` and `body`, as the browsers' limit does: 510 `div`s fit, the 511th is
`GTEXT_HTML_TREE_E_LIMIT_DEPTH`), `max_nodes` (10,000,000, `E_LIMIT_NODES`), the tokenizer's six limits
(each its own status, passed through), `max_errors`, `scripting`, `hash_seed`. A failure is sticky and
returns no document and no partial tree; an allocation failure at any point is `E_OOM` and leaks nothing
(swept at every allocation, on three documents and two cuts, in the tests and in the fuzzer). The document is
made with no depth limit of its own: the parser's limit is the stack's.

**Errors.** Both stages' parse errors, in the order raised, with code, line, column (UTF-16 units, as the
tokenizer's) and byte offset, capped by `max_errors`, the rest counted. The standard names one tree-stage
error (`non-void-html-element-start-tag-with-trailing-solidus`, raised for a start tag with a trailing
solidus that nothing acknowledged, an ignored one included); for the rest it says "parse error", and the 14
codes here are named for what happened (`missing-doctype`, `unexpected-end-tag`, `misnested-formatting`...).
A tree-stage error is positioned at the first character of the token that caused it (for a NUL in a run of text, the start of the run; tokenizer errors are at the character). Ten of the tree stage's kinds have tests with exact name, line, column and byte offset (`ParseErrorsAreNamedAndPositioned`).

## Design

Tag names are looked up once per tag in a table of the 115 names the algorithms mention (sorted, binary
search; its flags are checked against the HTML vocabulary's `gtext_markup_html_is_special()` and
`_is_formatting()` by a test, so the category is written once) and in a seeded hash table for the rest. The
stack of open elements is an array. For each name it keeps the index of the topmost open element of that
name, and each entry holds, for each kind of scope, the index of the nearest boundary at or below it (its
"floor"). So **"has a `p` in button scope" is one comparison**, and so are "any other end tag", the `li`, `dd`
and `dt` start tag loops, and "the last `a` after the last marker" (each formatting-list entry is chained to
the previous one of its name). The few operations that change the middle of the stack (the adoption agency
algorithm, `</form>`, the head element after the head) unwind the entries above, edit the array and derive
them again: O(entries above), the cost of the shift itself. No code recurses. Elements are created from
names interned once (`markup_element_create_atoms`), attributes are appended without the duplicate check the
tokenizer has already done, and a run of text appended to a text node grows its buffer geometrically.

Where the cost is what the standard forces: reconstructing the formatting elements costs the number reopened;
the adoption agency algorithm costs the distance it walks and the entries above the formatting element that it
shifts; a repeated `<html>` or `<body>` start tag costs the product of its attribute count and the element's
(O(1) each: a hash index of the names of the html and body elements, so any number of start tags with new names is linear, and the elements keep every distinct name, as the standard has no limit); the Noah's Ark clause compares a new formatting element with the entries of its name after the last marker, and every formatting start tag reconstructs first, so all of them are open: at most the depth of the stack, 512 by default, per token (a test with distinct attributes, nested, misnested and reconstructed, shows a unit of fixed depth is linear in its repetitions); `</body>`,
`</html>` and the end of the file scan the stack once.

## Measured

Numbers are from the gates' output; the commands are named so they can be run again.

- `make conformance-html-tree`: the web-platform-tests tree-construction vectors at the commit pinned in
  `tools/html/WPT_COMMIT`: 1,962 vectors in 58 files, and 59 hand-chosen ones
  (`tools/conformance/html_tree_extra.dat`, expected trees that html5lib and parse5 both give, rechecked by
  `tools/oracle/html_tree_extra_check.py`; they exist because the gate's own plants showed rules the vectors
  never exercise: the adoption agency's bookmark, `ol` and `ul` as list item scope boundaries). The four
  `scripted_*.dat` files (6 vectors, trees that scripts make) are not run. **1,319 vectors are in scope
  (initial documents the builder parses); 1,315 pass; 4 are listed because no tree builder following the
  chapter can pass them** (`webkit02.dat` 45 to 48 expect a `selectedcontent` element to be filled with the
  selected option's content, a DOM behaviour of the form controls), and 706 are the **tagged exceptions**
  (`tools/conformance/HTML_TREE_EXCEPTIONS`): stage 3d 318 (219 table, 99 template), stage 3e 384 (178
  foreign content, 206 fragment vectors), none 4. `make conformance-html-tree-asan` runs the same gate with the library and the runner built with ASan and UBSan (the runner frees each chunk at once). A vector written for a scripting setting runs with it, the
  others with the flag off and on. Every run is also cut every way (every two-way split, a byte at a time,
  pieces of 2 to 9, random pieces) and must give the same answer, and the tree passes the invariant checker.
  `#errors` is compared by count and reported, not gating: 1,129 of the 1,256 passing vectors have exactly the
  count (every difference is ours being higher, 120 of them in the processing instruction vectors, whose
  expected counts predate the states). The gate fails on an unlisted failure, a stale exception (one that
  now passes, or stops elsewhere), and below 1,300 passing; **it is made to fail 48 ways**
  (`run-html-tree.sh --controls`: the vectors lie, the list lies five ways, three floors (passing, loaded, exact `#errors`), twelve bad requests,
  and 27 planted defects in the builder, one rule undone each: the Noah's Ark count, both adoption agency
  limits and its bookmark, the scope tables, the `li`/`dd`/`dt` loops, the form pointer, text merging, `pre`'s
  line feed, the scripting flag...). Plants of the quirks tables cannot be seen by vectors (the document mode
  is not in the dump, and the vectors that show it use tables): the tables are tested directly instead
  (`TheDoctypeDecidesTheDocumentMode`, 43 doctypes).
- `tests/test-html-tree.cpp` (54 tests): the matrix rows by name, hand-worked adoption agency and Noah's Ark
  cases, the tag table against the vocabulary, "reset the insertion mode" over 23 stacks, quirks over 43
  doctypes, every split offset of every vector (two settings of the scripting flag; over 100,000 cut runs)
  and every three-way cut of six awkward inputs, an allocation failure at every point, the limits, eight
  threads, and **the work counted at two sizes** (n and 4n) on deep `div`, `p` after 3,000 `span`s, `li`, `dd`/
  `dt`, stray end tags, `b`/`i` mis-nesting, runs of `a`, `i`, `nobr`, and attributes on many elements: the
  ratio stays under 6, and the work is under `GTEXT_HTML_TREE_WORK_BOUND(n)` (64 n + 4096). A plant that
  makes the scope check walk the stack fails them, as it must.
- `make check-html-oracle` (the tree part, `run-html-oracle.sh --tree`): 10,143 documents (the 1,084 in-scope
  vectors that have no `select` or processing instruction, the 59 extras, 6,000 generated, 3,000 mutated)
  put to html5lib 1.1 and parse5 8.0.1 and this builder; none stopped at a later stage. This builder
  equals parse5 on 9,869 and html5lib on 8,871. **Every divergence is one of 18 kinds** in
  `tools/oracle/html_tree_divergences.txt`: both references do not know `search` (a 2023 addition: special, and
  it closes a `p`; `both:` where they agree with each other, `three-way:` where they differ from each other too,
  judged against parse5), and html5lib 1.1 (2020) alone differs where parse5 agrees with this builder. Every
  explanation is **verified** by asking again with the elements the reference reads by older rules removed
  (`search`, `dialog`, `main`, `hgroup`, `figcaption`, `summary`, `menu`, `isindex`, `command`, the ruby parts,
  `frameset`): the divergence is explained only if the two then agree; 230 of html5lib's do not (`alone:unverified`:
  its older adoption agency rules, which nest the copies of the formatting elements the other way round). A divergence in a document with none of them, or one
  that survives their removal, is unexplained and fails; so does a stale entry or a changed count. **Made to
  fail 20 ways** (the answer corrupted two ways, the list three, the extra cases once, and 14 planted
  defects). Two hunts beyond the default (`HTML_TREE_ORACLE_COUNT=30000`, seeds 21 and 22: 46,138 documents each, 90,000 of them generated or mutated) found nothing unexplained, in the same 18 kinds.
- `make fuzz-html-tree`: `fuzz_html_tree` (whole, a byte at a time, pieces, random pieces and one cut: the
  tree, the errors and the status identical; valid tree; sticky failures; tiny limits; an allocation failing at
  each allocation up to the last of the parse, which the harness checks it reached; linear work), 1,807 seeds from the vectors and the extras plus 13 deep and wide ones,
  replayed by `make test`; 31,410 runs of 150 seconds found nothing (before the sweep was made to run to the last allocation of each parse). `make check-html-fuzz-plants` plants six
  defects: two are *replayed* from a seed, four are *searched for* from the seeds within the time budget, and so
  is the unedited control, which stays silent.
