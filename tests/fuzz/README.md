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
| INI  | `fuzz_ini.cpp`  | `make fuzz-ini`  | `make fuzz-run-ini`  |

`make fuzz` builds and runs all seven. Each runs for `FUZZ_TIME` seconds
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

**And with that one fixed, the next: an alias key with no value.** Seven
minutes of `fuzz_yaml_writer` after the U+FEFF fix, on the multi-document
path in UTF-16, and the parser's complaint is again the whole statement of
it:

    Mapping key missing before ':'

Except that it was not. The message named the wrong entry, and the wrong
side of the library: what the artifact's document held was

    *O :
    *P : y

and the entry at fault is the *first* of those, whose value was never
written. A block mapping's children are collected as one alternating key,
value, key, value list, so a key whose value is absent has to be handed the
null the empty node stands for or the list is left one short. Three kinds of
node arrive where that is decided - scalars, aliases, and finished flow
collections - and `mapping_supply_null_value()` was reached from two of them.
An ALIAS event never asked.

So the list went into the next entry one short, that entry's `:` found an
even length in front of it, and the parser reported a key missing from a line
that had one. Two aliases in a row were needed to see it at all: with an
ordinary scalar in the following entry, the SCALAR event's own copy of the
rule supplied the value the alias had not, and the document parsed.

One column is the whole measurement. An ALIAS event's column is the first
character of the anchor name and the `*` that introduces it stands one to its
left, which is exactly the difference between *at* the key's column, so the
next key, and *past* it, so the previous key's value. The fix is a four-line
`alias_node_col()`; the three cases in the test that hold that line are
`b: *O`, `b:` over `  *O`, and `b:` over `- *O`, each of which a rule applied
one column too far left turns into a second entry.

Worth saying that an alias belongs where the writer put it:
`ns-s-block-map-implicit-key` is `ns-s-implicit-yaml-key`, which is
`ns-flow-yaml-node`, whose first alternative is `c-ns-alias-node` (8.2.2,
7.1). The writer was writing conforming YAML that this library then refused
to read. `corpus/yaml-writer/alias-key-with-no-value.seed`, and
`YamlBlockMapKeys.AnAliasKeyWithNoValueStillHasOne` is the gate - three
mutations, each caught by the case meant for it: the rule removed, its column
one place right so it never fires, and its column at the line's start so it
fires for a value.

`make conformance` scores exactly what it scored before, both ways, because
yaml-test-suite has no input of this shape. An alias key *with* a value is
there (E76Z, `&a a: &b b` over `*b : *a`), which is why the accepting half of
the rule was never missing. An alias key without one appears twice and neither
is an input:

    grep -nE '^[[:space:]]*\*[^[:space:]]+[[:space:]]*:[[:space:]]*$' \
        build/yaml-test-suite/src/*.yaml

reports `X38W`, inside that case's `dump:` block rather than its `yaml:` one
and with a sequence for its value on the lines below, and `2SXE`, where `*a:`
is an alias to the anchor `&a:` standing as a value and there is no separator
colon on the line at all. The corpus is 351 cases and holds neither.

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

The writer harness asserts the round trip under all three table styles and under
any combination of the v1.1.0 write spellings, and on documents that came from
*parsing* it also asserts that **the same comments come back** - the corpus's
`comments` mode over inputs nobody chose. Four defects, all caught, control
silent:

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

**The v1.1.0 write spellings are an axis here too**, drawn from the mode byte's
spare bits rather than from a third header byte: `sel` has five bits left once
the table style and the parse version have taken theirs, and the spelling mask
needs five of its own. So the header stays two bytes and a corpus unit keeps its
length and its body offset; what changes is that its mode byte now also picks a
spelling, which the fuzzer re-explores on its own.

The read-back version follows the mask, and it has to: every spelling the option
adds is 1.0.0-invalid by construction, so re-reading spelt bytes with the strict
arm would report the option's whole purpose as a writer defect. A zero mask
reads back at 1.0.0, which is the stricter of the two and the right default for
the property.

## Fuzzing INI

`fuzz_ini.cpp` asserts five properties beyond "it did not crash", and the first
one is the whole reason the harness is worth having:

- **The strict dialect is a subset of the generic one.** The generic dialect is
  Desktop Entry plus six relaxations and one normalisation; a relaxation may
  only *add* accepted documents, so anything the strict dialect accepts the
  generic one must accept, and for an input **containing no CR** must give the
  same groups, keys and raw values. The corpus can only state this over documents
  that are already valid, which is every file on the disk.
- **An unmodified document writes back byte for byte** (Desktop Entry §3).
- **A normalizing write's output parses and holds the same values**, which the
  byte-identical property does not imply, because normalizing takes a different
  branch.
- **Every raw value goes through every accessor** - unescape, escape, list,
  bool, int, double, interpolation under all three styles, and the locale chain. Most
  fail, and a failure is a fine answer; reading a byte that is not there is not.
- **`GTEXT_INI_INTERPOLATION_NONE` returns the input**, always. It is the default, so a
  regression there changes what every caller who passed no options gets.
- **`gtext_ini_value_needs_interpolation()` and the pass agree about the trigger byte.**
  Two readings of one predicate written in two places, which is the shape that drifts: if
  the detector says a value does not need the pass, the pass must succeed and return it
  unchanged. The converse is deliberately *not* asserted - a value holding a `%` can
  still come back unchanged, since `%(k)s` resolving to `%(k)s` is a legal document, and
  asserting the biconditional would fail on a correct library.
- **A second parse of the same bytes is the same document.**

**All of those but the first run under seven dialects** - Desktop Entry, generic,
git config, EditorConfig, systemd, configparser and Win32 - and that is a correction
rather than a refinement. The
byte-identical rewrite was asserted under the *strict* dialect alone for a while,
and the strict dialect has `skip_bom` false and refuses a document beginning with
a byte-order mark. So when the generic dialect turned out to be **discarding** a
BOM rather than skipping it - writing a document back three bytes shorter than it
came in - no input this harness could generate reached the path, and it had to be
found by a differential against git instead. A property asserted under one
dialect says nothing about another that relaxes the rule the property depends on.

**Win32 carries a property none of the other six can**, and it is about refusal rather
than about agreement: it must parse **every input it reads as bytes**. Its key charset is
open, the empty key and the empty section name are both spellable, a line with no
separator is a valueless entry and an unclosed header is an ordinary line, so no byte
sequence is left for that reader to reject. A claim of that shape is exactly what a
fuzzer is for.

**The qualification arrived by the property failing, which is the right way round.** It
was unconditional for one round, and then an encoding check went in above the grammar:
a document opening `FF FE`, `FE FF` or `FF FE 00 00` is now ::GTEXT_INI_E_ENCODING to
every dialect including this one. At four bytes over a two-byte alphabet the fuzzer
reaches that prefix almost at once, so the assertion failed on its next run rather than
outliving the thing it described. Nothing about the dialect changed - every rule below
the encoding still refuses nothing - and the harness says so by naming the excluded set
with gtext_ini_detect_encoding() rather than with a byte test of its own, so a mark this
harness does not know about cannot silently widen the exemption. **The converse went in
beside it**, which the unconditional form could not state: a marked document must be
refused by *every* dialect, so a sniffer that fell through would fail here instead of
reading as the property still holding.

The first property has no analogue for git config, EditorConfig, systemd,
configparser or Win32, and the absence is a finding: none of the five is a relaxation
of Desktop Entry in either direction. Win32's reason is its own and is the only one
that runs in both directions at once within a single rule: `;` is a comment to it and
`#` is not, which is the exact opposite of the generic dialect on both counts - so one
line is a comment to one and an entry to the other, and the next line the other way
round. configparser sits outside every relation for a reason none of
the others has - a `:` ends a key for it and is an ordinary key byte to the other five,
so the same line is a *different entry* rather than a legal-or-not question - and it is
the only dialect that reaches the indent scan, the second join, and a writer branch that
**inserts** bytes rather than emitting the value it was given. systemd sits outside every subset relation here in both directions at once -
it refuses a preamble that the other two accept, ends a line on a lone CR that all four
others treat as data, and accepts a continuation inside a name that none of them has. git
accepts a preamble, a continuation and a valueless key that Desktop Entry refuses,
and refuses a key not starting with a letter and a group name outside
`A-Za-z0-9-.` that Desktop Entry accepts; EditorConfig accepts a section name
holding any byte, an empty section name and a duplicate key, and refuses the `\n`
escape and the `;`-separated list Desktop Entry *defines* - so the same bytes can
be a legal document to both dialects and mean different things. Neither direction
of the subset relation holds and there is nothing to assert; asserting either would
fail on the first input that exercised the difference.

EditorConfig earns its place here for a second reason: it is the **widest** of the
four, so it is the dialect that actually reaches the writer and the value layer on
arbitrary input where the other three refuse early. It is also the only one with
no escape set at all, and that turned out to matter - gtext_ini_unescape() refused
every backslash for an escape-free dialect, which is the documented behaviour's
exact opposite, and no dialect existed to show it until this one.

### The parity property found a defect on its first run

At 237,647 executions, 90 seconds, on an empty corpus. The input reduced to:

```
[G]
k=v<CR>          <- a trailing CR, and no linefeed after it
```

The CRLF test was `bytes[content_end - 1] == '\r'` with no clause asking whether
an LF was actually there, so at end of input the generic dialect **stripped a CR
that was not a terminator** and produced a value one byte shorter than the strict
dialect's. Neither a corpus nor a round trip could have seen it: the byte still
came back in the line's trailing run, so the document rewrote byte for byte and
scored clean. Only comparing the two dialects' *values* on the same bytes
separates them, which is the property this harness exists to assert.
`corpus/ini/trailing-cr-with-no-linefeed.ini.seed`.

### And then it found that the property itself was wrong

Rebuilt with the parser fixed, the same property trapped again at **30,209
executions** on a document whose *header* ends with LF and whose *entry* ends
with CRLF. This time the library was right and the property was wrong.

`accept_crlf` is not a relaxation. The other six generic-dialect changes only
widen what is accepted, so they carry the inherited property; this one removes a
CR from the content of a line the strict dialect **already accepted**, so
`Exec=/bin/true` + CRLF is `/bin/true\r` to the strict dialect and `/bin/true` to
the generic one, and both are correct. The claim "a relaxation may only add
accepted documents" had been written into the header, the constructor, two
documentation pages, the README and this property, and the corpus could not
contradict any of them because **this machine has zero CRLF `.desktop` files**.

So the property now asserts acceptance unconditionally and values only for an
input containing no CR, and `conformance-ini-desktop-entry` excludes a CR-bearing
file from its parity score and counts it. The exclusion path is not reachable
from the corpus, so it was exercised on purpose: a directory with one CRLF file
and one plain one reports `parity 1 of 1, 1 excluded`.

### One abort with no reproducer

A 600-second run ended at **3,263,258 executions** with
`AddressSanitizer:DEADLYSIGNAL` twice and "nested bug in the same thread,
aborting" - no error type, no stack, and **no artifact**, because libFuzzer never
got to write one. It has not recurred: 1,979,816 executions in fork mode and
3,339,326 in a later run, both clean, and the 4,073-file corpus replays clean
deterministically. The cause is unknown and is recorded here rather than
explained away.

What it did produce is a better instrument. `__builtin_trap()` raises SIGILL,
which ASan reports as DEADLYSIGNAL, so a failing property and a genuine memory
fault look identical - and if ASan's own report faults, neither is identified.
`fuzz_ini.cpp` now names the property and dumps the input itself, escaped and
flushed, before trapping. If it returns, the log will say which of the five it
was. An instrument that only works when the crash handler works is not an
instrument.

The fix drew out a second, subtler one in the writer. Refusing the value
outright - which is what "a CR would end the line" suggests - made a document
this module *reads* unwritable, and the round-trip test caught that immediately.
A trailing CR is unsafe only when an LF is written **directly** after it: at end
of file, or before a `\r\n` terminator that supplies its own CR, it round-trips
fine. So `ini_value_writable()` takes the byte that will follow the value, and
refuses exactly the values that would read back differently rather than every
value that looks dangerous. A writer that is too strict is a defect too.

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
| YAML writers | 5.9M | 25 defects, all fixed; clean after the last |
| CSV  | 6.5k | clean |
| TOML | 875.7k | clean |
| TOML writer | 2.8M | one defect, fixed; clean after, with the spellings axis |
| INI  | 5.3M | two defects and one false claim, all fixed; clean after, with one unexplained abort noted below |
| INI, three dialects | 4.4M + 1.9M | clean; the git config dialect added, and every property but the subset relation now asserted under all three. Two runs because the first binary predated two edits that changed no behaviour - an uncalled function removed and a comment - and a figure carried across a rebuild is not a figure |
| INI, four dialects | 2.5M + 578k | clean, no artifacts; the EditorConfig dialect added as a fourth arm. Two runs again and for the same reason: the second binary is the one this commit ships. It is the widest of the four, so it is the arm that actually reaches the writer and the value layer on arbitrary input - and the only one with no escape set, which is what exposed gtext_ini_unescape() refusing every backslash for an escape-free dialect |
| INI, five dialects | 800k + 334k | the systemd dialect added, and coverage rose from 1,228 to 1,418 edges on the same corpus - the largest jump any of the four additions produced, because it is the only dialect whose continuation reaches the **line assembler** and therefore the only one under which a group name and a key are not spans of the document. Two runs for the usual reason: the second binary is the one this commit ships. The execution rate is a third of the four-dialect figure and that is expected - five parses and five write-and-reparse cycles per input, against four |
| INI, seven dialects | 664k then 1.24M | **one defect, and it was the generic dialect's** - a key whose own first bytes are a UTF-8 BOM. `skip_bom` strips a BOM only at offset 0, so ` <BOM>j=v` is an entry whose key really is `<BOM>j`; a **normalized** write then drops the leading blank, the key lands at offset 0, and a reader strips it - so the document came back with a different key, or with no entries at all. Found at 663,648 executions on a four-byte input, ` <BOM>`, which is only that short because the seventh dialect needs no separator to make a line an entry. The generic dialect has had this for as long as it has had `skip_bom`, and 570k executions across five and six dialects never reached it: what changed is not the property but the *cost of the path to it*. Fixed by refusing a normalized write of such a name, which leaves the verbatim write - the default - working. The clean run after is 1,242,145 executions, and coverage rose from 1,580 to **1,673** edges. The Win32 arm also carries the harness's only unconditional per-dialect property: it must never refuse an input |

Both TOML harnesses were re-run after `gtext_json_to_toml()` stopped refusing a
long number lexeme (2026-09-28): 65.6k and 510.2k executions, no crash and no
artifact written. That is a confirmation run rather than a campaign, and it is
here because the change moved a copy from a stack buffer to the allocator on a
path `fuzz_toml`'s round trip reaches.

The writer harness's execution count is not comparable with the readers': it
builds a document and re-parses one on every run, so it is much slower per
execution than a parse-only harness.  The 5.9M above is one 901-second run on
the merged corpus after the last fix, at about 6,600 executions a second. The four writer defects it was written
for had already been found by hand; it exists so the next four are not, and it
has already earned that — **twenty-five** defects in the library and three of
its own. The figure is a count of the finds named in "Fuzzing a writer" above,
one per bullet or paragraph, which is the only instrument there is for it: a
few of those bullets name two faults of one shape found together, and none of
them is a count of commits. It replaces two numbers that had drifted apart in
this paragraph and disagreed with each other.

Most of them are in the *reader*, which is not what this harness was built to
test: a writer is an instrument for asking a parser questions a corpus of
inputs cannot phrase, and it turns out to ask a lot of them.

CSV is much slower per execution because the harness reads back every field of
every parsed table; that is deliberate, since indexing is where a row/column
mismatch would show.
