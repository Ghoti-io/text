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
# The tree builder (stages 3c, 3d and 3e)

`src/html/html_tree.c`, `html_tree_modes.c`, `html_tree_foreign.c`, `html_tree_fragment.c`, `html_tree_internal.h` (the contract) and
`html_tree_private.h`. It drives the tokenizer above and builds a \ref format_markup "Markup"
document: elements in the HTML namespace with attributes in no namespace (and, in foreign content, elements in the SVG and MathML
namespaces with the names and attributes the standard's tables give them), a doctype node,
comments, processing instructions, text with adjacent runs merged, the document mode. Internal
only, as the tokenizer is.

## Specification

WHATWG HTML, "Tree construction" (13.2.4 "Parse state", 13.2.6 "Tree construction"), **as served
at html.spec.whatwg.org/multipage/parsing.html on 2026-10-11** (Living Standard, last updated 9
October 2026, the revision the tokenizer was written from; fetched again for 3c, and again **2026-10-12 04:25 UTC for
3d**: the same revision, Last-Modified 9 October 2026 08:28 GMT; and a third time **2026-10-12 09:42 UTC for 3e**: Last-Modified
Fri, 09 Oct 2026 08:28:19 GMT, the same revision, so there is no difference from the 3d text to note), read mode by mode. Implemented:

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
| 13.2.6, 13.2.4.2 | **(3e)** the tree construction dispatcher (which rules a token is handled by), the adjusted current node, the MathML text integration points and the HTML integration points (SVG `foreignObject`, `desc`, `title`; MathML `annotation-xml` whose start tag had `encoding` text/html or application/xhtml+xml, ASCII case-insensitively) |
| 13.2.6.1 | **(3e)** "adjust MathML attributes" (`definitionURL`), "adjust SVG attributes" (58 names), "adjust foreign attributes" (the eleven `xlink:*`, `xml:*`, `xmlns` names), "insert a foreign element", and the parse error for an `xmlns` that is not the element's namespace |
| 13.2.6.4.7 | **(3e)** the `math` and `svg` start tags of "in body" |
| 13.2.6.5 | **(3e)** the rules for parsing tokens in foreign content: characters (NUL, white space, other), comments, processing instructions, DOCTYPE, the 44 start tags (and `font` with `color`, `face` or `size`) and the end tags `br` and `p` that break out, "any other start tag" (the adjusted current node's namespace, the SVG element names of the 37-name table), the SVG `script` end tag and a self-closing `script`, and "any other end tag" (the walk down the stack) |
| 13.4 | **(3e)** the HTML fragment parsing algorithm, step by step (`html_tree_fragment.c`); the "fragment case" branches of the modes |

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

## Foreign content and fragments (stage 3e)

**Foreign content.** An element is the pair (namespace, name): a name slot of the builder is keyed by both, so `<svg><td>` is not an HTML
`td` to the stack's per-name indexes (the topmost open element of a name, the scope floors, "any other end tag"), and the nine
elements the standard makes special and scope boundaries (MathML `mi mo mn ms mtext annotation-xml`, SVG `foreignObject desc title`)
carry those flags while the HTML ones of the same names are never confused with them. "Is this the HTML namespace" is a comparison,
and the foreign end tag rule's walk down the stack is two index lookups and a comparison with the nearest HTML element below the
current node, kept per stack entry, so it costs O(1) (the pops of a breakout are the pushes, paid once). The tables (37 SVG element
names, 58 SVG attribute names, `definitionURL`, the eleven namespaced attributes) are data in `html_tree_foreign.c`, extracted from
the served text by script and checked by a test against the vocabulary the writers use (`gtext_markup_html_foreign_name_survives()`).
**That check found a gap**: the vocabulary had been copied from parse5 8.0.1, which lacks `feDropShadow` (`fedropshadow` is in the
standard's table); it is added, and the XHTML writer's differential lists the references' gap as a class of its own
(`ref:both:feDropShadow`, `tools/oracle/html_divergences.txt`). The tokenizer's CDATA decision is the adjusted current node's
namespace (`ht_allow_cdata`), evaluated when `<![CDATA[` is read; every token before it has been handled by then, and characters never
change the stack, so the answer is the same however the input is cut.

**Fragments.** `gtext_html_tree_new_fragment(options, &context, &status)` runs the algorithm's steps 1 to 21 (the document's mode from
the context's document, the tokenizer's state from the context element (RCDATA for `title` and `textarea`, RAWTEXT for `style xmp
iframe noembed noframes`, script data for `script`, RAWTEXT for `noscript` when scripting is on, PLAINTEXT; HTML elements only: an SVG
`title` is not RCDATA, which the vectors say), a root `html` as the stack's only node, "in template" pushed for a template context, the
fake start tag's attributes for an `annotation-xml` context, **reset the insertion mode appropriately** reading the context element,
the form element pointer when the context or an ancestor is a form); feed and `finish` as for a document; `gtext_html_tree_take_fragment()`
moves the root's children into a document fragment and hands over the document that owns it. The context is described **by value**
(`GTEXT_HTML_FragmentContext`: namespace, local name, attributes, form-ancestor flag, quirks mode, scripting flag); no tree node is made
for it and nothing of the caller's is read after the call (a test frees the name and attributes at once; under a sanitizer a kept
pointer fails). The standard as served makes the fragment the "root insertion target" (nodes whose target parent is the root go to
the fragment); here they are inserted into the root and moved at the end, which is the same tree.

Branches of the standard that are marked "fragment case" and were stubs until now: `</html>` after the body (ignored), `</frameset>` (no
switch to "after frameset"), the `select` and `input` start tags when the context is a `select`, "parsing template contents" for a
template context (the `form` rules), the end of the file in "in template" with no template on the stack (stop), foster parenting that
returns the root, table-context clearing and the table and caption end tags that find nothing in scope.

There is no public HTML API, no `<meta>` encoding step (3f), no serialization, no scripts. The "not yet" status of the earlier stages
(`GTEXT_HTML_TREE_E_NOT_YET` and its accessors) is removed: nothing raised it once foreign content was built. The scorer's exceptions
format keeps its stage column, for a future construct that is not.

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
solidus that nothing acknowledged, an ignored one included); for the rest it says "parse error", and the 16
codes here are named for what happened (`missing-doctype`, `unexpected-end-tag`, `misnested-formatting`...; stage 3e added `html-in-foreign-content`, for a breakout, and `xmlns-attribute-mismatch`, which the standard describes at the end of "create an element for a token").
A tree-stage error is positioned at the first character of the token that caused it, except the errors raised for each character of a run (text foster parented out of a table, characters ignored in a column group, a NUL in the white space that waits in a table), which are at their own character: the tokenizer gives a character token the places its pieces came from (`segments`: a character reference, a carriage return and line feed, a replaced byte each start a piece), the first 1,024 of a run exact and the rest counted on from the last, so the memory is bounded however many references a run holds. Tokenizer errors are at the character. Ten of the tree stage's kinds have tests with exact name, line, column and byte offset (`ParseErrorsAreNamedAndPositioned`), and two more, `html-in-foreign-content` and `xmlns-attribute-mismatch`, have one in the foreign content tests (`TheForeignErrorsAreNamedAndPositioned`); the other kinds are named, and the vectors count them.

## Design

Tag names are looked up once per tag in a table of the 119 names the algorithms mention (sorted, binary
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

- `make test` runs the vector gates too (`check-conformance-vectors`): the XML suite, the HTML tokenizer's vectors with their controls and the
  tree builder's vectors, foreign content and fragments included. **The decision**: measured on the development machine they take 24, 28 and 20
  seconds, under two minutes together, so they are in `ALL_TEST_GATES` and not a subset; the data being absent is a failure that names
  `tools/html/fetch.sh` or `tools/xml/fetch.sh`, as for every gate that reads `third_party/`. What stays outside `make test` is the tree gate's
  controls (`make conformance-html-tree`, about five minutes: it builds and runs 128 planted defects), the oracle and the fuzz plants.
- `make check-foreign-tables` (also in `make test`): the SVG and MathML name tables are kept by hand in `html_tree_foreign.c`, `markup_html.c`,
  `test-markup-html.cpp`, the oracle's generator and the fuzz seed generator. `tools/html/foreign_tables.txt` is the one transcription of the
  chapter's tables (37 SVG tag names, 58 SVG attributes, `definitionURL`, 11 foreign attributes) and `tools/html/check-foreign-tables.py` reads each
  copy from its declaration and fails on any difference in either direction, or on a declaration it cannot find; dropping `feDropShadow` from
  one copy was seen to fail it.
- `make conformance-html-tree`: the web-platform-tests tree-construction vectors at the commit pinned in
  `tools/html/WPT_COMMIT`: 1,962 vectors in 58 files, and 133 hand-chosen ones
  (`tools/conformance/html_tree_extra.dat`: expected trees that html5lib and parse5 both give, rechecked by
  `tools/oracle/html_tree_extra_check.py`; 13 are the table modes', 61 are stage 3e's: the breakout list in six groups, `font`,
  the formatting elements reopened before an `svg`, the foreign end tag's walk, the special foreign elements, CDATA, the
  attribute adjustments and thirty-two fragments; the ones in an SVG or MathML context are asked of parse5 alone, and the
  checker verifies that html5lib still cannot take one). The four `scripted_*.dat` files (6 vectors, trees that scripts make)
  are not run. **What the denominators are**: 1,962 + 133 = 2,095 vectors are run, **all in scope**: 1,857 initial documents and
  238 fragments (206 vectors and 32 extras); **2,091 pass; 4 are listed because no tree builder following the chapter can pass
  them** (`webkit02.dat` 45 to 48 expect a `selectedcontent` element to be filled with the selected option's content, a DOM
  behaviour of the form controls). The **tagged exceptions** (`tools/conformance/HTML_TREE_EXCEPTIONS`) are those 4: stage 3e
  **0** (it was 424: 218 foreign, 206 fragment), 3d 0, none 4. **Where the vectors came from**: the pinned html5lib-tests holds no
  tree-construction vectors (only tokenizer, serializer and encoding data), so the web-platform-tests copy is the whole of
  them. The order the plan asks for, reproduced by undoing the groups in scratch copies on the same 2,095 vectors (the
  integration points, the breakout and the end tag rules together, then the fragment algorithm): with foreign elements and their
  adjustments alone, **133 of the 1,857 documents fail** (1,724 pass; each of the three groups alone: integration points 31
  documents and 31 fragments, breakout 40 and 16, the end tag walk 6 and 0); with the integration points, the breakout and the
  end tags, **all 1,857 pass** (no fragment can, there is no algorithm yet); with the fragment algorithm, **2,091 of 2,095 pass,
  the four being the `selectedcontent` ones**. `make conformance-html-tree-asan` runs the same gate with the library and the runner
  built with ASan and UBSan (the runner frees each chunk at once). A vector written for a scripting setting runs with it, the
  others with the flag off and on, a fragment vector in the context its `#document-fragment` line names (the runner takes
  `ctx:x<hex>`, `form` and a quirks word). Every run is also cut every way (every two-way split, a byte at a time, pieces of 2 to
  9 bytes, random pieces) and must give the same answer, errors and their places included, and the tree passes the invariant
  checker. `#errors` is compared by count and reported, not gating: 1,792 of the 1,958 passing vectors that list errors have
  exactly the count (every difference is ours being higher, 124 of them in the processing instruction vectors, whose expected counts
  predate the states, 11 in `plain-text-unsafe.dat`, 6 in `html5test-com.dat`). The gate fails on an unlisted failure, a stale
  exception, and below 2,085 passing, 2,050 loaded and 1,792 with the exact count. **It is made to fail 159 ways**
  (`run-html-tree.sh --controls`, 138 lines of output because the twenty-two bad requests print as one; `--control NAME` runs one):
  31 that are not planted defects (the vectors lie, the list lies five ways, three floors, twenty-two bad requests), and **128
  planted defects**, one rule undone each: 66 of 3c and 3d, and **62 of 3e**: the SVG element, SVG attribute, MathML attribute
  and namespaced attribute adjustments (and one entry missing from each table), the annotation-xml encodings, the breakout (never;
  three names missing from the list; `font` with `face`; `font` without attributes; the end tags `p` and `br`; popping past an
  integration point; leaving foreign elements open), the foreign end tag (popping one element, walking past HTML elements,
  looking only in SVG, never reaching the HTML rules, the SVG `script` end tag, self-closing elements), the integration points
  (MathML text, SVG, as scope boundaries, as special elements, `mglyph`, `malignmark`, `svg` in annotation-xml, characters and start tags
  in them), NUL and frameset-ok in foreign text, CDATA never or always allowed, the adjusted current node ignoring the fragment
  context, and the fragment algorithm (the tokenizer state, `title`, `script`, `plaintext`, `noscript`, the reset from the context, the
  result not moved, the select context, the template context, `</html>` and `</frameset>`, the integration point contexts, the
  context's namespace). **Rules no vector reaches, so no plant here**: the form element pointer of a context with a form
  ancestor, the quirks mode of the context's document and the encoding attribute of an annotation-xml context (none can be
  written in the vectors' format; `tests/test-html-tree-foreign.cpp` holds them, and a plant of each was made by hand and fails
  it), the seeding of the stack of template insertion modes by a template context (no effect anyone can see: the reset falls back to
  "in template", and the first start tag replaces the mode), and the parse error for an `xmlns` that is not the element's namespace
  (an error-count-only rule; its test names the error and its place).
- `tests/test-html-tree.cpp` (85 tests; two rewritten for 3e: the not-yet status and the foreign content in a cell) and
  `tests/test-html-tree-foreign.cpp` (44; 3e): the matrix rows by name (inline SVG, MathML and annotation-xml, breakout, foreignObject,
  foreign attributes, CDATA, NUL, the fragment contexts, the form pointer, the depth limit, out of memory), **the adjustment tables as
  data** (sizes 37, 58 and 11; every row applied in SVG and not in MathML or HTML; each target survives the vocabulary
  `gtext_markup_html_foreign_name_survives()` and each folded form does not; the nine special foreign elements against
  `gtext_markup_html_is_special()`), every one of the 44 breakout names and 64 names that are not on the list, `font`'s three
  attributes, each integration point (five MathML text, annotation-xml with ten encodings, SVG `foreignObject`, `desc`, `title`) with HTML
  and foreign content inside, the boundary of scope (`</p>` in a `desc`, an `li` in a `foreignObject`), the end tag walk (case,
  nearest, across namespaces, across integration points, not past an HTML element), the SVG `script`, "reset the insertion mode" over
  29 contexts, the tokenizer's state for each context (HTML elements only), the table, template, select, frameset, html, body
  and integration point contexts, argument checks of the fragment API, the context described by value (freed at once), every
  split offset of every one of the 238 fragment vectors in both scripting settings (over 7,700 cut runs) and three-way cuts of ten
  foreign-heavy inputs in documents and in seven contexts, an allocation failure at every point (seven documents and fragments, two
  cuts; and in `new_fragment`), the limits (600 nested `svg`, `math`, `g`, `foreignObject` each its own status and no tree; the
  boundary 510 and 511; fragments; nodes), **and the work counted at two sizes** (n and 4n): deep SVG and MathML, strays and
  self-closing elements at depth 3,000, runs of breakouts popping foreign elements, many integration point switches (`foreignObject`,
  `desc`, `title`, `mi`, `annotation-xml`), large fragments in `td`, `tbody`, `template`, `svg`, `foreignObject`, `mi` and `select`
  contexts, and 16,000 distinct foreign element names: the ratio stays under 6 and the work under
  `GTEXT_HTML_TREE_WORK_BOUND(n)` (64 n + 4096). The older tests' figures (3c and 3d) stand.
- `make check-html-oracle` (the tree part, `run-html-oracle.sh --tree`): **18,461 documents** (1,622 initial-document vectors without a
  processing instruction and 206 fragment vectors, the 133 extras, 6,000 generated and 3,000 mutated as before, and since 3e 3,000
  with `svg` and `math` embedded in HTML (every SVG name whose case is restored, every attribute that needs adjusting, the integration points
  with HTML and foreign content inside, annotation-xml with each encoding, the breakout tags, `font`, `</p>` and `</br>`,
  CDATA, scripts, end tags that match, differ in case and match nothing; in formatting elements, paragraphs, tables and cells, templates, lists,
  selects and forms), 1,500 of those mutated, and 3,000 fragments in random contexts (55 % HTML elements of every mode and tokenizer state,
  25 % SVG, 20 % MathML)) put to html5lib 1.1 and parse5 8.0.1 and this builder; none stopped. **This builder equals html5lib on 11,730 and
  parse5 on 16,604** (both on 11,703). **1,460 fragments are in an SVG or MathML context, which html5lib's API cannot take** (it must say so,
  and the check that it still does is in every run); parse5 alone judges them. **Every divergence (5,411 documents) is one of 58 kinds** in
  `tools/oracle/html_tree_divergences.txt`, each verified like the 3d ones, but, since 3d's lesson, **an explanation of foreign content is not
  the deletion of an element where it can be a transformation that keeps the rest of the document in the question**: `@endtags`
  (delete the end tags that name an element of foreign content in this builder's tree: both references find the element an end tag
  closes by its name alone in the HTML rules, so `</ms>` closes a MathML `ms` the standard leaves open and an `</td>` finds an SVG
  `td`), `@cdata` (a CDATA section becomes its text: parse5 makes a comment in an integration point), `@context` (a foreign context named
  like an HTML element is renamed: parse5 reads the name without the namespace), `@noscript` (the scripting flag on for the noscript
  context: both references ignore the flag), `@selectctx` (a select context becomes a div: both read the fragment by the replaced "in
  select" mode), `@rootend` and `@resetnames` (found by the hunts: parse5's foreign end tag walk stops before the root element of a
  fragment, so an unmatched end tag is never handed to the HTML rules; both references' reset of the insertion mode reads names without
  namespaces, so a MathML `tr` is "in row"); and by removal only where the reference is older than the standard and the other reference
  corroborates (`html5lib:alone:` kinds: html5lib 1.1 has `foreignObject` as the only special foreign element, so `mi`, `mo`, `mn`, `ms`, `mtext`,
  `annotation-xml`, `desc` and `title` do not stop its "any other end tag"; and it keeps `</p>` and `</br>` inside foreign content), plus
  `fedropshadow`, which **neither reference has in its table of SVG names** (the standard has it; a finding of this stage, see above). Every
  deviation of a reference has a **witness**, a document whose tree was worked out from the chapter by hand: 19, eleven of them new
  (checked on every run, with the scripting flag both ways unless the entry fixes it, and a context for a fragment). The count of
  **documents where nothing but the witnesses and the vectors stands behind this builder** (both references differ and the cause is one
  that could hide a defect; or only parse5 could be asked and it differs) is **1,519** and is gated like a kind's. **Made to fail 87
  ways** (`run-html-oracle.sh --control NAME`): the answer corrupted two ways, the list three, the witnesses once, the uncorroborated
  count once, the extra cases once, 14 planted defects of 3c, 22 of 3d, and **43 of 3e** (one per rule group and for the fragment
  algorithm; run in parallel scratch copies, each caught). **Hunts**: `HTML_TREE_ORACLE_COUNT=15000`, seeds 21 to 24: 4 x 43,211 =
  **172,844 documents**, run in bounded chunks, found six causes the first time and none unclassified after (each is a `hunt` entry of
  the list; html5lib 1.1 stops on an assertion in its template handling for two documents, which parse5 then judges alone).
- `make fuzz-html-tree`: `fuzz_html_tree` now parses fragments too (bit 1 of the first option byte; its high six bits choose among 64 contexts: HTML
  elements of every mode, SVG and MathML elements, each integration point, an annotation-xml with and without the encoding, a form, a quirks
  mode), holds foreign trees to the shapes a parser can make (names the standard's table gives, attributes only in the XLink, XML and XMLNS
  namespaces and only on foreign elements, no NUL in text, a fragment's root empty) and sweeps allocation failure through `new_fragment`
  as well; 2,136 seeds (1,839 + 297: the fragment vectors in their contexts and 40 foreign and fragment inputs at the sizes that make a naive
  implementation quadratic), replayed by `make test`; **52,810 runs in 853 seconds** (three runs of 281, 286 and 286 s) after the last change found nothing.
  `make check-html-fuzz-plants` plants twenty-one defects in the tree builder: thirteen are searched for, eight replayed from a seed, and so is
  the unedited control, which stays silent. Six are 3e's: the foreign namespaces' atoms not released (the invariant checker's reference counts), the SVG
  element and attribute names left lower case (the shape property), a NUL kept in foreign text, an allocation failure absorbed while a foreign
  attribute is added, and the fragment's result not moved out of the root.

## Not done

A public HTML API, encoding determination and the `<meta>` steps (3f), serialization, sanitization, scripts (the `scripted_*.dat` vectors stay
out of scope: `scripted_adoption01.dat`, `scripted_ark.dat`, `scripted_foster01.dat`, `scripted_webkit01.dat`, 6 vectors).
