# Fuzzing

Six libFuzzer harnesses:

| Target | Harness | Build | Run |
| --- | --- | --- | --- |
| JSON | `fuzz_json.cpp` | `make fuzz-json` | `make fuzz-run-json` |
| YAML | `fuzz_yaml.cpp` | `make fuzz-yaml` | `make fuzz-run-yaml` |
| YAML writers | `fuzz_yaml_writer.cpp` | `make fuzz-yaml-writer` | `make fuzz-run-yaml-writer` |
| CSV  | `fuzz_csv.cpp`  | `make fuzz-csv`  | `make fuzz-run-csv`  |
| TOML | `fuzz_toml.cpp` | `make fuzz-toml` | `make fuzz-run-toml` |
| TOML writer | `fuzz_toml_writer.cpp` | `make fuzz-toml-writer` | `make fuzz-run-toml-writer` |

`make fuzz` builds and runs all six. Each runs for `FUZZ_TIME` seconds
(default 60); override it for a real campaign:

    make fuzz FUZZ_TIME=3600

Requires `clang`. The library sources are recompiled with clang's coverage
instrumentation and linked into the harness rather than linking the ordinary
shared library — libFuzzer steers its mutations by the coverage it observes,
and against an uninstrumented library it sees only the harness and degrades
into random input generation. AddressSanitizer and UndefinedBehaviorSanitizer
are enabled, since a parser reading one byte past a buffer is exactly the bug
being looked for and will not usually crash on its own.

## Fuzzing a writer

Three of these read. `fuzz_yaml_writer.cpp` writes, and it exists because for
a long time nothing did: the YAML harness parses and walks and never calls a
writer, so the most productive tool applied to that module had never been
pointed at half of it.

The property is the one every known writer defect broke:

> If the writer says OK, the bytes it wrote must parse, and must hold the
> same values.

A refusal is always a permitted answer — not every value has a YAML spelling,
and saying so is correct. What is never permitted is claiming success and
producing something this library cannot read back.

Two families of input reach the writers, and they are not the same:

- **documents that came from parsing**, which is what a round trip over
  yaml-test-suite measures; and
- **documents built through the DOM API**, which is where the interesting
  failures were. A corpus of YAML text can only carry values the parser
  accepts, so a DEL never reaches a writer from the first direction and is
  ordinary from the second. The harness therefore treats its bytes as a small
  program for building nodes — scalars, sequences, mappings, and hostile
  anchors and tags — as well as as a document to parse.

A fifth mode runs the streaming parser straight into the streaming writer
with no DOM in between. That is the path a caller of the event API uses, and
it is the only one that carries a `%YAML` or `%TAG` directive, an unresolved
tag spelling, or a comment as far as the writer.

Two of its finds were bugs in the harness rather than in the library, and both
are worth recording, because each marks a place where the property as first
written was stronger than the truth:

- the event API resolves nothing, so `*c%` — an alias to an anchor nobody
  declared — is an ordinary event stream, and the writer is right to hand back
  something equally unresolvable. The property only holds where the input was
  a document.
- a DOM may hold two equal keys; nothing in the API prevents it. Writing that
  document faithfully is the writer doing its job, and refusing to read it
  again is the *reader's* duplicate-key policy doing its own. `{: , : }` was
  the fuzzer's way of asking. The output is read back with
  `GTEXT_YAML_DUPKEY_KEEP_ALL`, because the question is whether this library
  can read what it wrote, not whether a policy likes it.

The rest were real, and every one is in `corpus/yaml-writer/`:

- an anchor named `!`, which the *parser* refused although `ns-anchor-char`
  admits it — nine printable ASCII characters could not begin a name;
- a tag of `!&!`, which the writer spells `!<!&!>` and the parser then read as
  a shorthand naming a handle nobody had declared, because a verbatim tag
  stopped being one as soon as its brackets came off;
- a scalar of bytes that are not UTF-8, which has no spelling at all — every
  escape of 5.7 names a code point, so `\xFF` would read back as U+00FF,
  two bytes and a different value;
- an empty tag string on a node inside a sequence, which is how it reached
  `[""]` — and `[""]` is JSON, so it took the JSON fast path, a second
  implementation of the parser that turned every quoted string back into
  whatever its text resolved to;
- `%` followed by a lone 0xC2, which the parser accepted as a document. The
  `c-printable` gate holds a UTF-8 sequence a feed cut in half rather than
  judging it from its first byte, and at the end of the stream it was still
  holding it — deferring to a scalar validator that a directive line never
  reaches. The writer emitted the directive back and the parser refused what
  it had just accepted;
- a tag of `!!-.#`, which the writer spelled straight out. The
  `tag:yaml.org,2002:` namespace is not the author's to extend, so the
  resolver refuses a tag in it naming no type the spec defines — and the
  writer was producing them. The DOM API validates neither tags nor anchors,
  which is what makes it the right instrument for this;
- a document of one `~`, which the streaming writer wrote as `"~"` — turning
  null into a one-character string. Only a plain scalar is resolved by its
  contents, so quoting is a change of value and not only of style wherever the
  plain text would have resolved to something else. `+1` had the same fault in
  both writers. Two characters missing from a whitelist - and one too many in
  it, since the same list admitted `-` unconditionally: the string `"-"` went
  out plain and came back as a sequence holding one empty node, because
  `ns-plain-first` admits `-` only when something plain-safe follows it;
- a built scalar of `1`, which is the same question from the other end. The
  DOM constructor made a *string* of whatever text it was given and the writer
  wrote that text plain, so the string `"1"` came back as the integer `1` and
  no route through the API could write it otherwise. The constructor takes the
  type from the text now, and `gtext_yaml_node_new_scalar_typed()` is where a
  caller says otherwise - and has to say something true of the text, which it
  once checked only when a tag was there to check it against;
- a tag of `!-`, which the writer spells `!- ` with a space after it — and the
  parser read that as the non-specific tag `!` followed by a block entry. A
  shorthand tag's name is `ns-tag-char+`, and `-` is one. `!-[]` had always
  parsed, because the `[` ends the name before anything can misread it; only
  the space the writer adds exposed it. `!#` followed once it was fixed — `#`
  is `ns-uri-char` too, and starts no comment where no white space precedes
  it.

Three more came out from behind the DOM constructor, one at a time as
each was fixed: a tag did not stop the text from deciding the type, a type
was set without the value that goes with it, and an empty tag string counted
as a tag everywhere except where it was written — which made an entry
holding one vanish from its sequence.

The JSON fast path is the one worth the longest look. It was not a writer bug
at all; the writer was the instrument. Nothing else in the project could have
found it, because `make conformance` asks for `KEEP_ALL` duplicate keys and
that is the one setting which turns the fast path off.

The artifacts are two to six bytes each and say nothing on their own, so the
harness prints what the writer produced and both JSON renderings before it
traps. That is the difference between an artifact worth keeping and one worth
deleting.

The next run went after the parser instead. A flow collection could not be a
block mapping's key on any line but the first (`a: 1` over `{}: 2`), and three
separate places turned out to be measuring the last *scalar* to find out where
that key stood - which says nothing about a key that is a flow collection,
since `{}` has no scalar at all. Two were in the parser and one in the
scanner, and each was only visible once the one in front of it was gone. A
fourth sat behind those: `a:` over `{}: 1` was not refused at all, it was
accepted with `{}` nested inside `a`, because the rule that a node at the
key's own column is the *next key* had never been given to the four places a
completed collection is added to its parent. Giving it to them without asking
which style the collection is costs eight documents of yaml-test-suite, all
zero-indented sequences - `key:` over `- a` is a block sequence standing at
its own key's column and still being the value.

Then a built scalar of `"\n3"`, which `gtext_yaml_node_type()` called the
integer 3 because `strtoll()` skips leading white space. The writer quoted it,
correctly - no plain spelling of that text exists - and the reader read back
the string. A plain scalar has white space at neither end (7.3.3), so text
that carries any can only be quoted, and quoted is string.

The run after that went five deep, one fix uncovering the next, and only the
first was a writer defect:

- the string `---` written plain, which is `c-directives-end` (9.1.2) and is
  kept out of a document's content by `c-forbidden` (9.1.1). The writer
  produced a document marker, called it OK, and the reader agreed with the
  bytes and gave back an empty document. `...` was the same;
- a block mapping's *first* entry could not have an empty key. `a:` over
  `  : 1` was refused, while the same entry later in the same mapping worked:
  a later entry joins a mapping already open, the first has to open one, and
  the branch that opens one ran only where no block mapping was open at all;
- a shorthand tag written with a handle no `%TAG` had declared. The DOM writer
  emits no directives, so a named handle is undeclared there by construction -
  and `!a!3` went out as itself. Refused now, since `!<!a!3>` is a different
  tag and an invented prefix would be worse; the streaming writer tracks the
  handles its own `%TAG` directives declare, and forgets them at each document
  end;
- two uninitialised pointers read off the stack by the DOM writer, because its
  two entry points set every field of their state by hand and a field had just
  been added. Both `memset` first now;
- a property in front of a key not being part of the key. `&a {}` begins at
  the `&`, and a block mapping is indented where its key is, so `&a {}: 1`
  over `b: 2` put the second entry outside the mapping the first had opened.
  In the scanner that blind spot was not new to flow keys at all: `outer:`
  over `  &a x: 1` over `   c` was refused where the same lines without the
  `&a` fold into `1 c`.

Two more after those. A built scalar of `+` and `1` with a break between them
was the integer 1, because `strtoll()` skips leading white space and this code
consumes the sign itself before handing `strtoll` what follows - and that half
was reachable from *text* as well, so `+` over a blank line over `1` parsed as
1 where both references read the string. Not one row of the 10.3.2 table
contains white space anywhere, which is the whole rule and is now written as
one. And the property-in-front-of-a-key defect had a third site: the rule that
a flow collection at the key's own column is the next entry's key measured the
collection rather than the entry, so `a:` over `&k [x]:` put the sequence
where a's value goes.

Three more after those: an empty `!!binary` value refused where base64 of no
bytes is the empty string and the writer writes exactly that; `!!binary` taken
as an assertion nobody checked, so a node built from good base64 answered
false to `gtext_yaml_node_as_binary()` and one built from `(((` was written
after the tag; and a document written with **two root nodes**, because
`*a: x` is an alias named `a:` followed by a second node at document level and
the writer put the second straight after the first, producing `*a:x`.

That last one came through the event-pipe path, which had been trapping **in
silence** - only `must_round_trip()` printed what the writer produced. Two of
this harness's own defects were already listed above; this is the third, and
it is the one that cost the most, because every find there began with reverse
engineering a dozen bytes. It prints now.

Two more again: a directive written straight after a document's content,
where 9.2 wants a `...` first - loud for `%TAG`, and silent for `%YAML` after
a plain scalar, which folded the directive into the scalar and turned `a` into
`a %YAML 1.2`. And a quoted `"<<"` taken for a merge key, where both
references read the two-character string: `{"<<": 1}` was refused and
`{"<<": {a: 1}}` was *merged*, the key vanishing into the mapping around it.
Only a plain scalar is resolved by its contents (10.3.2), for the fourth time
in this file.

Then three more, two of which were regressions of the fixes above. A "-",
"?" or ":" is an indicator only where white space follows it - "- x" is an
entry holding x, "-: 1" a mapping whose key is the plain scalar "-" - and both
the scanner's `line_node_col` and the parser's "does this begin its own line?"
test had been skipping them without asking. And base64, which is exempt from
the writer's quoting whitelist because "/" and "=" are ordinary in it, was
exempt from the plain style's own limits too: a binary scalar written over two
lines came back with its break folded into a space. The bytes were the same,
which is why nothing measuring values noticed; the text was not.

And a kept trailing break doubled by the document separator: a block scalar
ends its own last line, `+` chomping keeps that break as part of the value,
and the break written before the next `---` landed on top of it. Only on `+`,
and only with a second document after it - two conditions at once, neither of
them rare on its own.

Then the same white-space hole through the two characters YAML does not have:
the vertical tab and the form feed are not c-printable, so no parsed scalar
holds one, but the DOM API takes any `char *` and `strtod()` skips them like a
space. `"\v6662."` was the float 6662.

And `!!binary` turned out to be only the first tag the constructor was not
checking: `!!int ""` was built without complaint and written as `!!int ""`,
which this parser refuses. Chasing the six shapes that did that found two
where the *parser* was the one in the wrong - `!!float 12` refused, where
10.3.2's float row makes its fraction optional and both references give 12.0,
and `!!null x` accepted, where the null row is five spellings and nothing
else.

And a nesting limit that reached only half the grammar. `max_depth` was
counted at the `[` and `{` of a flow collection, in the stream layer; block
structure is composed by the DOM parser and never touched it, so `- ` five
thousand times parsed to a DOM five thousand deep with a limit of 256 in
force. The writer is what exposed it - the flow-style writer turns block
nesting into flow nesting, which *is* counted - and that is a trick worth
remembering: running a document through a writer changes its spelling cheaply,
and a limit that only some spellings reach is not a limit.

And then a second limit nobody set: the scanner tracked flow context in a
fixed 32-entry array, and when it ran out the push was *dropped* while the
matching pop still counted down - so past 32 nested flow collections it
believed it was back in block context with the brackets still open. Plain
nesting survives that, because one dropped push and one clamped pop cancel;
the shape the fuzzer built does not, and came back as "Unterminated flow
collection" on a document whose brackets balance. The array grows now, and
`max_depth` is the only limit.

And then the biggest of them. An event's offset indexes the *decoded*
character stream; the parser's positional helpers indexed the raw input the
caller handed in. For UTF-8 with no byte order mark those are the same bytes.
For everything else they are not, and a block mapping with two entries did not
parse at all - `a: 1` over `b: 2` was refused, in UTF-16 and equally in
ordinary UTF-8 behind a mark. One of those helpers scans backwards and had no
bound check, so the mismatch was also a **heap-buffer-overflow**, which is what
ASan caught here. The clamp went in first; the offsets took longer, because
the scanner slides a window over its decode buffer and an absolute offset
cannot index a window. It can be asked to keep the whole thing now, and the
DOM parser asks. Those bytes are the first seed in `corpus/yaml-writer/` worth
tracking, because it no longer traps.

And then a conversion with no answer. UBSan, not ASan: `(int64_t)f` for an
infinity is undefined, and the harness built the node that gets there - a
scalar whose text is `.INF` and whose caller said it was an integer. Looking
for the shape elsewhere found it again in YAML 1.1's sexagesimal integers,
which are accumulated as a double and cast with nothing in between, so
`1:99999999999999999999999999999999` came back as `INT64_MIN`. Neither is
reachable from a corpus of *documents*; the first needs the DOM API, and the
second needs 1.1 mode. `corpus/yaml-writer/int-tag-on-an-infinity.seed` is
the reproducer, kept because it no longer traps.

**An hour found nothing.** `make fuzz-run-yaml-writer FUZZ_TIME=3600`, run
straight after the `.INF` fix: 18,551,639 executions at about 5,150 a second,
no crash, no timeout, no sanitizer report. Every run before it had found
something, each fix exposing the next - twenty-three thousand executions in
twenty minutes for the property-column defect, on a corpus that barely
existed yet; five minutes and 1.9 million for the `.INF` conversion, on one
that did.

**It is worth reading what that does and does not say.** Coverage was still
climbing when the hour ran out - 5,237 edges to 5,276, features 29,121 to
31,422, and 8,615 new corpus units kept. The harness was still reaching code
it had not reached before and simply did not fault there, which is a
different thing from having run out of places to look. What it does say is
that the defects this target finds cheaply are gone, and the next one will
cost more than an hour.

**The corpus has been merged since**, which is the first half of what that
run said to do next. It had grown to 43,318 files and 173MB, and average
throughput had fallen from 7,550 executions a second to 5,150 as libFuzzer
spent its time re-running units that carried nothing new. `-merge=1` cut it
to **5,911 files and 24MB** at *identical* coverage - 5,276 edges and 31,422
features, the same two numbers the hour ended on - and a run over the merged
corpus averages 9,147 a second. That is 1.8 times the work per second for no
loss of reach.

Merge into a destination that already holds the `.seed` files rather than an
empty one. libFuzzer keeps everything already in the destination and adds
only what extends it, so the tracked seeds survive with their names; merging
into an empty directory would keep whichever of them still carried unique
coverage, rename it to a hash, and silently drop the rest. They are tracked
because they no longer trap, which is exactly the property that makes a
coverage-based merge throw them away.

**The second half is done, and it paid immediately.** The harness could only
build three kinds of node, with fixed parse options and two fixed write
options, and what it cannot construct it cannot test - both defects the runs
before it found needed something the corpus alone could not reach, the DOM
API for one and YAML 1.1 mode for the other. It now draws from the input:

- the **parse options**, so 1.1 mode, the three schemas and the key policies
  are reachable at all;
- the **write options**, so indentation, line width, the five scalar styles,
  the three flow styles, canonical form and all five encodings are - which
  puts the writer's UTF-16 and UTF-32 output back through the reader, an axis
  yaml-test-suite does not have;
- the **node kinds** - `!!set`, `!!omap`, `!!pairs`, typed scalars, stored
  scalar styles and comments, none of which three kinds could express;
- **multi-document output**, which had no path here at all.

It found four defects in the first ninety seconds, described on the YAML
format page: a preferred scalar style that changed what a scalar *was*, a
comment written without being checked (a line break in an inline one escaped
into the document as a second mapping entry), a NUL that stopped `strtoll()`
where 10.3.2 does not, and a tag checked against a node's text but not
against its declared type. Two more things it turned up were written down as
open questions rather than defects; both turned out to be defects with
settled answers after all, and one of them - an inline comment inside a flow
collection - had been recorded as reachable only by building a document
through the DOM API, which was wrong. The parser produces the same node from
`[ x, # note`/`  y ]`, so it was a document this library read, wrote, and then
refused to read back.

It has since also found the writer's missing **schema**: `0:0` parsed with
`yaml_1_1` is the integer 0, and it came back a string. Half of that was the
writer never being told which dialect its output was for, which the
`schema` and `yaml_1_1` write options now answer - this harness writes for the
dialect it parsed in, which is the only version of the round trip that means
anything. The other half was the quoting whitelist, which treated `:` as
needing quotes where 7.3.3 does not, and quoting a *non*-string changes what
it is.

A stored artifact that predates all of that turned out to be an omap built
with the same key twice - the DOM appenders enforced neither of `!!omap`'s two
rules, where the resolver enforces both.

The last one still standing was **an anchor written on its own line before a
block collection**, and it is the clearest case yet of why this harness
writes as well as reads. The anchor reached the collection on the way out but
the alias table kept pointing at the scalar it had arrived on, so `*O` inside
the collection named the mapping's first key. Used as a key it made a mapping
with two distinct keys come back *Duplicate mapping key*; used anywhere, it
meant the document said one thing going in and another coming back, which is
the invariant this target exists to check and the only way it could have been
noticed. All thirteen stored artifacts pass now.

Its triage is the lesson worth keeping. Read by eye, the 434 bytes of UTF-16
it produced contained `!!null ""`, which looks like a defect this library has
had before; that guess went into the documentation before it was checked, and
it was wrong. Decoding the output and asking the parser for its *error
message* said `omap keys must be unique`. The harness prints what the writer
wrote because an artifact says nothing on its own - but the parser's own
complaint is shorter and truer than anything inferred from the output.

**And one more, found while the TOML harnesses below were being armed: U+FEFF
in a scalar written as a block scalar.** It is the one character that separates
the plain style's rule from the block styles'. nb-char is "c-printable - b-char
- c-byte-order-mark" (5.4), so a byte order mark has no plain *and* no block
spelling; `scalar_needs_quotes()` had refused it for the plain style since
U+0085 and U+2028 were added beside it, and the block styles never asked,
because they ask `plan_block_scalar()` and that function only looked below
U+0020. A folded scalar carrying one went out as itself and this library's own
parser answered `Byte order mark in scalar content` - five words that were the
whole diagnosis, and truer than anything the 434 bytes of UTF-32 the harness
printed would have suggested. The triage lesson above, applied a second time.

Where it is reachable from was measured rather than assumed, because that is
what decides whether the parser is in the wrong too: **only through
`gtext_yaml_node_set_scalar_style()`**. The only YAML spelling that admits a
U+FEFF is a quoted one - nb-json includes it - so a parsed node remembers a
quoted style, and so does one `gtext_json_to_yaml()` builds; and with the
default flow options every awkward character ends up double-quoted anyway,
which is why `EveryCodePointSurvivesOrIsRefused` had never seen it although it
walks all of Unicode. The test that holds it asks the five stored styles under
block flow, with U+0085, U+2028 and U+2029 beside it as controls: each of those
*does* have a block spelling, so a fix that refused the neighbourhood would
pass only by accident. `corpus/yaml-writer/bom-in-a-block-scalar.seed`.

**And with that one fixed, the next: a mapping entry written with no key.**
Seven minutes of `fuzz_yaml_writer` after the U+FEFF fix, on the
multi-document path in UTF-16, and the parser's complaint is again the whole
statement of it:

    Mapping key missing before ':'

The writer produced a line of `: ":.."` - a value with a colon and nothing
before it - inside a document whose keys are aliases (`*O : *O`, `&O : .*O`).
**Not fixed**: it is a third thread of the same afternoon's work and a
different defect from the byte order mark, and it wants its own look at how a
key that is an alias, or an empty key, reaches the writer. The artifact is
`libs/text/crash-a809a848cc25f19018ab4bad71c91b7a876a9cb9`, and it is a
`crash-` file rather than a `.seed` because a seed is a reproducer that passes.

This path now prints the parser's message before the bytes, which is the
triage lesson above finally applied to it: it had been printing 434 bytes of
UTF-16 and nothing else, so every find here began with decoding a screenful
and guessing. Five words did what the guessing could not, twice in one
afternoon.

Three notes for whoever runs it next. The header is **two bytes** now, not
one - path and dialect in the first, write options in the second - so a
corpus unit written for the old shape means something different. Failure
messages print bytes as escapes, because the moment the encoding became an
axis a UTF-16 document reached the terminal as a screenful of nothing. And
one find in that first run was the harness's own: it probed a scalar's type
with `c_str()` while building the node from the full run of bytes, so a NUL
made the two disagree and the harness manufactured the contradiction it then
reported. Decode the artifact before believing it.

## Fuzzing TOML

`fuzz_toml.cpp` reads and `fuzz_toml_writer.cpp` writes, on the same division
as the YAML pair and for the same reason: a corpus of TOML text carries only
values the parser accepts, and the writer's interesting refusals are all
reachable through the DOM API alone - a comment holding a control character or
a line break, a comment on a value going inside `{ }`, a string that is not
UTF-8, a date-time `chron` will not spell.

Four properties in the reader harness, and each was *seen to fail* by planting
a defect for it. The control - the unmutated tree - is silent through 950,000
executions:

| property | the defect that fires it |
| --- | --- |
| the event walk accepts exactly what the parse accepts, with the same status and position | an empty comment made to stop the walk |
| **1.0.0's documents are a subset of 1.1.0's** | the inline-table trailing-comma rule inverted |
| what the writer writes, the parser reads back with the same values | the backslash left unescaped |
| a trip through JSON settles after one pass | - |

The subset property is the one no corpus row states. It is not a property of
the two specifications - 1.1.0 *tightens* two things 1.0.0's prose allowed -
but it is a property of this module, which takes the strict ABNF reading of
both wherever they disagree; and each of toml-test's two manifests only ever
asks an arm the cases its own list decides. The last row has no entry because
nothing planted so far fires it: it is cheap insurance, and saying so is more
useful than implying it is armed.

The writer harness asserts the round trip under all three table styles, and on
documents that came from *parsing* it also asserts that **the same comments come
back** - the corpus's `comments` mode over inputs nobody chose. Four defects,
all caught, control silent:

| planted defect | what fired |
| --- | --- |
| no control-character check in a comment | the writer wrote what the parser refuses |
| a line break allowed in an inline comment | the writer wrote what the parser refuses |
| a comment written inside `{ }` | the comments changed |
| the writer drops a leading comment | the comments changed |

Comment fidelity is asserted only on the parsed family, and that is a real
limit rather than laziness: a caller *can* set a trailing comment on a
sub-table, which is written after that table's block and read back as the
following header's leading comment - the same comment, in the place it now
occupies.

**A fifteen-minute run found one library defect**, and it is the mirror of a
refusal the writer already made. `GTEXT_TOML_TABLE_STYLE_HEADERS` writes an
array whose elements are all tables as `[[a]]` blocks - each element gets a
header and the array gets nothing - so a comment sitting on the array, which is
where a parse puts the comment from `a = [{...}] # note`, had no line to go on
and was **dropped**. Refused now, with `E_UNREPRESENTABLE`, exactly as a comment
on a value going inside `{ }` is. The corpus could not find it: its comments
mode round-trips the as-read style, where such an array stays inline and its
comment stays on its statement.
`corpus/toml-writer/comment-on-an-array-of-tables.toml.seed`.

**One find in the first ninety seconds was the harness's own**, and it is the
same mistake the YAML harness made twice. Comparing documents by their JSON
*text* made the property stricter than the contract: the writer's documented
rule is that plain keys come before sub-tables, whatever order they were
defined in, because every bare key after a `[header]` belongs to that header's
table. So `[[a.b]]` before `y = 2` must come back in the other order. The
comparison sorts object keys now, which is the comparison the corpus makes.

## The options byte

Each harness consumes the first input byte (two, for CSV and for the TOML
writer) as a selector for
the parse options — for the writer harness, for which of its five paths to
run — and treats the rest as the document. The dialects these
parsers accept are configurable enough that a fixed set of options would
leave most of the state machine unreachable: comments, trailing commas,
single quotes, alias resolution, and CSV's delimiter and quote characters are
all chosen from that byte.

A consequence worth remembering: a corpus file is not a valid document on its
own, it is one byte of options followed by a document. The seeds in
`corpus/*/` are built that way.

## Findings

The first run found four bugs, all fixed:

- **Use-after-free in the JSON parser.** A nested object lives in its
  parent's arena, and `gtext_json_free()` frees the whole arena, so the
  object parser's error path destroyed the parent; the parent's own error
  path then read freed memory. `{"":{` — five bytes — was enough.
- **Infinite loop in the YAML scanner.** A block scalar header running to
  end of input had no exit from the loop skipping the rest of the header
  line. `>[` — two bytes — hung the parser indefinitely.
- **Undefined behavior on empty quoted scalars.** `a: ""` passed a NULL
  pointer to `memcpy`, and asked `malloc(0)` for the buffer, whose NULL
  return would have been reported as an allocation failure.
- **Leaked token buffers.** Several error paths in the stream layer abandoned
  a token that owned a heap buffer.

Then, once token ownership was settled (below) and the fuzzer could reach
deeper, a second infinite loop in the YAML scanner: the block scalar *body*
committed its consumption with `while (cursor < pos2)`, and `scanner_consume()`
cannot advance past the end of the input, so a `pos2` beyond it spun forever.
Same shape as the header loop, one function further down.

## Token ownership

The leaked token buffers were not five independent mistakes. `yaml_internal.h`
documented the scalar payload as "owned by scanner until next token", while
`scanner.c` allocated a fresh buffer per token and `stream.c` freed it — so
every error path that abandoned a token leaked, and the fuzzer found them one
at a time.

The scanner owns the payload now, exactly as the header always said: it is
released when the next token is requested, and on scanner teardown. All
eighteen frees in `stream.c` are gone, consumers borrow rather than own, and
the two that keep the text for longer (the DOM builder and the pull reader)
already copied it.

After the change the YAML fuzzer ran 2.4 million executions clean, with
coverage up from 4,764 to 7,736 — the leak reports had been masking how much
of the parser it could not get to.

## Current state

| Target | Executions | Result |
| --- | ---: | --- |
| JSON | 4.6M | clean |
| YAML | 1.2M | clean |
| YAML writers | see below | still finding things; one open, see below |
| CSV  | 6.5k | clean |
| TOML | 875.7k | clean |
| TOML writer | 1.7M | one defect, fixed; clean after |

The writer harness is new, and its execution count is not yet comparable: it
builds a document and re-parses one on every run, so it is much slower per
execution than a parse-only harness. The four writer defects it was written
for had already been found by hand; it exists so the next four are not, and it
has already earned that — forty-four library defects and three of its own,
listed above. Most of the twenty are in the *reader*, which is not what this harness
was built to test: a writer is an instrument for asking a parser questions a
corpus of inputs cannot phrase, and it turns out to ask a lot of them.

CSV is much slower per execution because the harness reads back every field of
every parsed table; that is deliberate, since indexing is where a row/column
mismatch would show.
