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
# The tree builder (stages 3c and 3d)

`src/html/html_tree.c`, `html_tree_modes.c`, `html_tree_internal.h` (the contract) and
`html_tree_private.h`. It drives the tokenizer above and builds a \ref format_markup "Markup"
document: elements in the HTML namespace with attributes in no namespace, a doctype node,
comments, processing instructions, text with adjacent runs merged, the document mode. Internal
only, as the tokenizer is.

## Specification

WHATWG HTML, "Tree construction" (13.2.4 "Parse state", 13.2.6 "Tree construction"), **as served
at html.spec.whatwg.org/multipage/parsing.html on 2026-10-11** (Living Standard, last updated 9
October 2026, the revision the tokenizer was written from; fetched again for 3c, and again **2026-10-12 04:25 UTC for
3d**: the same revision, Last-Modified 9 October 2026 08:28 GMT), read mode by mode. Implemented:

| Section | What |
| --- | --- |
| 13.2.4.1 to 13.2.4.5 | the insertion mode, **reset the insertion mode appropriately** (complete: the table, caption, column group and template branches, the template's branch reading the current template insertion mode; tested over 33 stacks, templates with their modes included), the stack of open elements and its scopes (general, list item, button, table), the list of active formatting elements with **the Noah's Ark clause** (three), reconstruct the active formatting elements, clear up to the last marker, the head, form and frameset-ok state |
| 13.2.6.1, 13.2.6.2, 13.2.6.3 | inserting nodes, the generic raw text and RCDATA parsing algorithms, generate implied end tags |
| 13.2.6.4.1 to 13.2.6.4.8 | the modes initial, before html, before head, in head, in head noscript (the scripting flag is an option, off by default), after head, **in body (every start and end tag rule)**, text |
| 13.2.6.4.9 to 13.2.6.4.16 | **(3d)** in table, in table text, in caption, in column group, in table body, in row, in cell, in template |
| 13.2.6.1, 13.2.4 | **(3d)** the appropriate place for inserting a node with foster parenting, the stack of template insertion modes, the template contents, "generate all implied end tags thoroughly" |
| 13.2.6.4.17 to 13.2.6.4.21 | after body, in frameset, after frameset, after after body, after after frameset |
| 13.2.6.4.7 | **the adoption agency algorithm**, outer-loop limit 8 and inner-loop limit 3, and "any other end tag" |
| 13.2.6.4.1 | the quirks-mode determination from the doctype (public and system identifier tables) |

Five things in the standard as served that the task statements did not expect, and what was done (the quirks rule
for the 4.01 Frameset and Transitional identifiers says "the system identifier is *missing or the empty string*" in
the copy served 2026-10-11, so an empty one counts as absent for it, as the code has it; absent and empty stay two
states from the tokenizer's flag to the tree's NULL and ""):

- **There is no "in select" insertion mode.** The `select` element is parsed by ordinary "in body" rules
  (`select`, `option`, `optgroup`, `hr`, `input` and the `select` end tag), and `select` is one of the
  boundaries of "has an element in scope". They are implemented, not stubbed, and `<select>` parses.
  (html5lib 1.1 and parse5 8.0.1 parse it by the older rules. Until 3d their documents with a `select` were
  left out of the differential; now they are asked, and a divergence in one is explained by removing the select
  elements, a method with a blind spot stated under "Measured".)
- **Processing instructions are nodes.** The standard's tokenizer has processing instruction states and the
  tree builder inserts them as nodes (`<?php echo?>` is a processing instruction node, `<?a$>` a comment,
  per the tokenizer); the pinned vectors expect nodes (`processing-instructions.dat`). The task statement said
  "no PIs (bogus comments instead)"; the standard and the vectors win. Neither reference has them.

- **The template start tag has two branches this parser cannot take**: declarative shadow roots (it needs the
  "allow declarative shadow roots" option, false by default, and a host element) and content patching by a `for`
  attribute (it needs a live document to patch). Without those both end in "insert an HTML element for the token",
  which is what is done; the "insertion target" and "insertion end marker" the appropriate-place algorithm reads are never
  set, so a template's place is its contents.
- **A form start tag in a table inside a template inserts the form** (and sets no pointer), and after head's anything-else
  sets frameset-ok to "ok" again when it makes the body: html5lib 1.1 and parse5 8.0.1 read both by the older rules.
  Both are the served text, re-read 2026-10-12 (the page unchanged: Last-Modified 9 October 2026 08:28 GMT). "In table",
  start tag "form": "Parse error. If the form element pointer is not null, and the parser is not parsing template
  contents, then ignore the token. Otherwise: Insert an HTML element for the token, and, if the parser is not parsing
  template contents, set the form element pointer to point to the element created. Pop that form element off the stack of
  open elements." (The older text, which both references follow, ignored the token when there was a template on the
  stack or the pointer was set.) "After head", anything else: "Insert an HTML element for a "body" start tag token with no
  attributes. Set the frameset-ok flag to "ok". Switch the insertion mode to "in body". Reprocess the current token.", and
  the template start tag in "in head" says "Set the frameset-ok flag to "not ok"." The witnesses
  `<form><template><table><form>` and `<head><template></template></head><p><frameset>` pin both.
- **The chapter names no error for an end tag `table` that pops open elements**, but the vectors list one, so one is raised
  (`open-elements-at-end-tag`). The `#errors` count is not gating beyond the floors.

## Not here yet: the stage gate

`svg` and `math` start tags (foreign content) are a construct of the next stage. A document that reaches one fails with
`GTEXT_HTML_TREE_E_NOT_YET`, sticky, no tree, and `gtext_html_tree_not_yet()` says which (foreign content, 3e); the 3d
values for table and template are gone. The fragment parsing algorithm is 3e as well. Not done at all in this stage: a public
API, the `<meta>` encoding steps (3f), serialization, scripts.

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
A tree-stage error is positioned at the first character of the token that caused it, except the errors raised for each character of a run (text foster parented out of a table, characters ignored in a column group, a NUL in the white space that waits in a table), which are at their own character: the tokenizer gives a character token the places its pieces came from (`segments`: a character reference, a carriage return and line feed, a replaced byte each start a piece), the first 1,024 of a run exact and the rest counted on from the last, so the memory is bounded however many references a run holds. Tokenizer errors are at the character. Ten of the tree stage's kinds have tests with exact name, line, column and byte offset (`ParseErrorsAreNamedAndPositioned`).

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

Numbers are from the gates' output, one run of each after the last change to the code (2026-10-12); the commands are
named so they can be run again.

- `make conformance-html-tree`: the web-platform-tests tree-construction vectors at the commit pinned in
  `tools/html/WPT_COMMIT`: 1,962 vectors in 58 files, and 72 hand-chosen ones
  (`tools/conformance/html_tree_extra.dat`, expected trees that html5lib and parse5 both give, rechecked by
  `tools/oracle/html_tree_extra_check.py`; 13 of them are the table modes'). The four
  `scripted_*.dat` files (6 vectors, trees that scripts make) are not run. **What the denominators are**:
  1,962 + 72 = 2,034 vectors are run; 206 of them are fragment vectors and 218 stop at foreign content (3e), which
  leaves **1,610 in scope** (initial documents the builder parses to a tree); **1,606 pass; 4 are listed because no
  tree builder following the chapter can pass them** (`webkit02.dat` 45 to 48 expect a `selectedcontent` element to
  be filled with the selected option's content, a DOM behaviour of the form controls). The 428 **tagged exceptions**
  (`tools/conformance/HTML_TREE_EXCEPTIONS`) are those 206 + 218 + the 4: stage 3d **0** (it was 318: 219 table, 99
  template), stage 3e 424 (it was 384: 40 vectors that were 3d also hold svg or math, and say "was 3d"), none 4. In the
  order the plan asks for, reproduced by undoing the groups in scratch copies, on the same 2,034 vectors: the table,
  section, row, cell, caption and column group modes alone pass 1,402 of the 1,487 in scope (a template stops a
  document, so the denominator is smaller; 85 need foster parenting); with foster parenting 1,483 of 1,487; with the
  template modes 1,606 of 1,610. (3c's own run, before the 13 new extras: 1,315 of 1,319.)
  `make conformance-html-tree-asan` runs the same gate with the library and the runner built with ASan and UBSan (the
  runner frees each chunk at once). A vector written for a scripting setting runs with it, the others with the flag off
  and on. Every run is also cut every way (every two-way split, a byte at a time, pieces of 2 to 9, random pieces) and
  must give the same answer, errors and their places included, and the tree passes the invariant checker (template
  contents included). `#errors` is compared by count and reported, not gating: 1,396 of the 1,534 vectors that list
  errors have exactly the count (every difference is ours being higher, 124 of them in the processing instruction vectors,
  whose expected counts predate the states). The gate fails on an unlisted failure, a stale exception, and below 1,590
  passing. **It is made to fail 87 ways** (`run-html-tree.sh --controls`, 76 lines of output because the twelve bad requests
  print as one): 21 that are not planted defects (the vectors lie, the list lies five ways, three floors, twelve bad
  requests), and **66 planted defects**, one rule undone each, 26 written as sed patterns (3c's; 27 before, one of them split into two literal plants) and 40 as literal texts (38
  of 3d's, and those two). The 3d ones: the stack not cleared to a table context, the
  implied colgroup, tbody and tr, a table start tag closing a `p` in quirks mode or never, white space or other text of a
  table always or never foster parented, the caption, the cell and the close-the-cell algorithm not clearing the
  formatting list, the column group mode never entered, a `col` left open, the table body and row contexts not cleared, a
  cell start tag not closing the cell, the form in a table left open or ignored for an open form (twice, one for each of
  the two modes that have the rule), a hidden input foster parented, foster parenting never enabled / after the table /
  preferring the table to the template, the adoption agency and reconstruction ignoring foster parenting, text merging
  with the wrong neighbour, the template contents unused, the template mode stack not replaced / not popped, the
  template's end of the file ignored, the `html` start tag merging attributes in a template, the body start tag
  ignoring the template, the body form start tag setting a pointer in a template, the reset ignoring the template mode.
  **Two of the plants are error-count-only defects**: no tree differs (the end tag `template` popping the cell, row and
  section without an error whether or not its implied end tags are thorough; the error for the elements a table end tag
  pops), so only the `#errors` floors see them; their exact error lists are pinned by unit tests instead. **Rules no
  vector reaches, so no plant here**: the template start tag's marker, the end tag `form` in a template, and the
  pointer a form in a table in a template does not set (unit tests with exact trees; the first two also by the
  differential's plants).
- `tests/test-html-tree.cpp` (85 tests; 54 before 3d) and `tests/test-html-tokenizer.cpp` (84; one new, for the places
  of a character token's pieces): the matrix rows by name (implied sections, foster parenting, nested tables, cell close,
  form in a table, template and its modes, EOF, quirks, adoption with foster parenting, depth), the insertion mode after
  prefixes of tags, error names and places in tables (each character of a foster parented run at its own line, column
  and byte, across references, CR LF, astral characters, NULs, pieces of the input and the pending white space),
  "reset the insertion mode" over 33 stacks (a mode name it does not know fails the call), quirks over 43 doctypes, the
  text of the served standard for the form and the frameset-ok rules, the markup text node's buffer size after each way
  its data is made, replaced or copied (under ASan), the reprocess limit as its own sticky status, nested templates closed by
  the end of the file as a loop (50,000 of them with a raised limit), every split offset of every vector (two settings of the
  scripting flag; over 100,000 cut runs), three-way cuts of ten awkward inputs, an allocation failure at every point (five
  documents, two cuts, and the white space that waits), the limits (600 templates or tables deep, each its own status and no
  tree), eight threads, and **the work counted at two sizes** (n and 4n) on deep `div`, `p` after 3,000 `span`s, `li`,
  `dd`/`dt`, stray end tags, `b`/`i` mis-nesting, runs of `a`, `i`, `nobr`, attributes on many elements, and, new, nested tables,
  thousands of rows and cells, cells left open, row closes, captions, column groups, long foster-parented text runs, text
  alternating between the text before a table and the text in it, foster-parented elements and blocks, the adoption agency
  moving blocks before a table, nested templates and templates in cells: the ratio stays under 6, and the work is under
  `GTEXT_HTML_TREE_WORK_BOUND(n)` (64 n + 4096). Text and white space are also measured in bytes asked of realloc, which the
  work counter cannot see (a text node keeps the size of its buffer, so a document that alternates between nodes does not
  copy a long one on each append; the pending white space of a table doubles).
- `make check-html-oracle` (the tree part, `run-html-oracle.sh --tree`): 10,476 documents (1,404 vectors in scope without a
  processing instruction, the 72 extras, 6,000 generated, 3,000 mutated; the generated ones hold tables with every
  combination of caption, colgroup, col, section, row and cell and tags missing, nested tables, strays that foster parent,
  misnested forms and hidden inputs, `p` and formatting elements around and inside tables, templates alone, around table
  parts, nested, in tables and with tables in them, and `select`, `option` and `optgroup`) put to html5lib 1.1 and parse5
  8.0.1 and this builder; none stopped at a later stage. This builder equals parse5 on 9,497 and html5lib on 7,237. **Every
  divergence (3,242 documents) is one of 29 kinds** in `tools/oracle/html_tree_divergences.txt`, each **verified** by asking
  again with the elements the reference reads by older rules removed, one at a time and then all of them (`search`, `dialog`,
  `main`, `hgroup`, `figcaption`, `summary`, `menu`, `isindex`, `command`, the ruby parts, `frameset`, and for 3d `select`,
  `template`, the end tags `tbody`, `thead` and `tfoot` (parse5 ends a row at one with no such section open), and `form`
  (parse5 asks whether any form is in scope at `</form>`)); a divergence in a document with none of them, or one that
  survives their removal, is unexplained and fails; so does a stale entry or a changed count. **The blind spot**: `template`,
  `select` and `form` explain *any* divergence in a document that holds the element, so a defect of this library there could
  hide. It could not be narrowed to signatures (parse5's table scope reaches past a template into rows and tables outside it).
  Instead: each deviation of a reference is a **witness**, eight small documents whose trees were worked out from the chapter
  by hand (parse5's table scope does not stop at a template; its row accepts an end tag `thead` with no thead; it ignores a
  form in a template; it asks about any form at `</form>`; both ignore a form in a table in a template when a form is open
  outside it, and keep frameset-ok cleared after a template; both read `select` by the old rules; html5lib 1.1 puts a template
  in the head into the body), which this builder gives and the reference does not, checked on every run with the scripting flag
  off and on and only if the reference answered (a crash or an error is not a deviation); and the documents in those kinds
  where **neither reference agrees** with this builder (651 in the default population) are counted, and the count is gated
  like a kind's. **Made to fail 44 ways** (`run-html-oracle.sh --control NAME`, one at a time: the answer corrupted two
  ways, the list three, the witnesses once, the uncorroborated count once, the extra cases once, 14 planted defects of 3c and
  22 of 3d, written as literal texts: one or more per mode, foster parenting four ways, the template and form rules nine). Three
  hunts beyond the default (`HTML_TREE_ORACLE_COUNT=30000`, seeds 21, 22 and 23: 46,476 + 46,476 + 46,475 = 139,427
  documents) found nothing unexplained; eight kinds appear only in hunts (`hunt` in the list).
- `make fuzz-html-tree`: `fuzz_html_tree` (whole, a byte at a time, pieces, random pieces and one cut: the tree, the errors with
  their places and the status identical; valid tree, and since 3d the shapes the table and template modes make (a tr in a table
  section, a td in a tr, a col empty and in a colgroup, a template with a content and no children, text directly in a table part
  only white space, the template contents walked too); sticky failures; tiny limits; an allocation failing at each allocation
  up to the last of the parse; linear work), 1,839 seeds: the vectors and the extras and 27 deep, wide, table and template
  ones (a long run of white space in a table, a run with references, CR LF, NULs and astral characters among them), replayed by
  `make test`; **90,653 runs in 942 seconds** (three runs of 270, 270 and 402 seconds) after the last change found nothing.
  `make check-html-fuzz-plants` plants fifteen defects in the tree builder: ten are searched for, five replayed from a seed,
  and so is the unedited control, which stays silent. Nine are 3d's (a shape, a leak, a heap overflow: text left in a table,
  a template with children, the pending text and the template mode stack not freed, a text buffer size recorded wrongly,
  no implied tr or tbody, a col not closed, the template that foster parenting finds read from the wrong place).
