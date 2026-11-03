@page text_format_adding Adding a format

# Adding a format

The documentation half of adding a parser. The code half - the headers, the
option struct, the streaming and DOM entry points, the namespace block and
the limits contract - is in \ref core_module "the Core module page"; this page is about what the format's page must
answer before the parser is considered finished.

One format, one page, under `documentation/formats/`. A format does not share
a page with another one: the two grow at different rates, their checklists
answer different questions, and a reader looking for one of them should not
have to scroll past the other.

The format pages are not the module pages. \ref text_modules "Modules" documents
the API - how to call it, what the options are, what the examples do. The
format pages document the *format* - which specification, which clauses,
which deviations, and what evidence exists for each claim. A sentence that
would still be true if the library were rewritten in another language belongs
on a format page.

## Checklist

- [ ] `documentation/formats/<format>.md` exists, opens with
      `@page format_<format> <Title>`, and is linked from the table in
      \ref text_format_references "Format and specification references".
- [ ] The specification is **named with its version and date** and linked.
      A format with no formal standard says so and links what documentation
      there is; where that documentation is silent, the page states what the
      parser does and why, because that decision is now the specification as
      far as this library is concerned.
- [ ] Every claim of support says which clause or production it refers to.
      "Supports Unicode" is not a claim anyone can check; "surrogate pairs
      per RFC 8259 §7, lone surrogates rejected with
      `GTEXT_JSON_E_BAD_UNICODE`" is.
- [ ] Every rejection names the status code it returns, and why that
      malformation is refused rather than recovered from.
- [ ] Every default is stated as a value, in a table. An option documented
      without its default is an option the reader has to go and read the
      source for.
- [ ] Deviations from reference parsers are **listed as cases**, each with a
      reproduction. A deviation nobody has written down is a defect waiting
      to be reported as one by somebody else.
- [ ] Tested scope says where the fixtures come from, how they were
      generated, which external oracle checked which claim, and **where each
      oracle's reach ends**. A round trip through this library alone proves
      the parser and writer consistent with each other, not correct; say so
      where that is all there is.
- [ ] The gaps are listed. Absences are cheap to write down while they are
      fresh and expensive to rediscover.
- [ ] The module page and any option tables are updated for whatever the
      format adds.
- [ ] The parent `README.md` lists the format among the ones with a parser -
      and does not list it before there is one.

## Page template

```markdown
@page format_<format> <Title>

# <Title>

One paragraph: what the parser implements, that the claims below were
checked, and a link back to the format index.

## Normative references

- **Specification:** <name, version, date, link>
- <the individually linked parts, if the document is split>

Which allocator owns what, and which free function releases it.

## Parts implemented

Bullets, one per structural area: grammar productions, encodings, extensions,
metadata. Each cites its clause.

## Limits

A table of option and default value.

## Save

What the writer chooses and why; which options change it; what it never
writes.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|------|-----------|-----------------------|

## Deviations

| Case | This parser | Elsewhere |
|---|---|---|

## Tested scope

Fixtures and their generator; the oracles and the reach of each; the fuzz
harnesses; the properties asserted without an oracle.

## Not implemented

What is absent, and what closing each gap would take.
```

Open and close the page with a link back to the index, written as
`format_references` in a Doxygen reference command - the first in the opening
paragraph, the last on its own line after a horizontal rule.

## Why the tested-scope section is not optional

The value of these pages is that a claim on them can be traced to something
that runs. The JSON page can name the status code returned for each malformed
input because each one was parsed and the result recorded. The CSV page could
say that `validate_utf8` did not validate UTF-8 - a defect found by checking
the claim rather than reading the code path, and fixed as a result. The YAML
page could give a table of plain scalars that a reference parser and this one
disagreed about, which is how a silent data-loss bug was found and fixed; and
it gives the score against a corpus nobody here chose, which is the only
figure that says how long that table really is. That last part took a while
to arrive, and when it did it cut the apparent conformance roughly in half:
a hand-built corpus agreed at 152 of 153 documents, and yaml-test-suite at
191 of 368. Fixing what it found has since taken that to 395 of 395, which
is the other half of the point: a score nobody here chose is also a list of
what to do next.

Name the limit of the corpus as well. 395 of 395 is a measurement over 406
documents; it is not a statement about the grammar those documents sample.
Reading the spec afterwards turned up fourteen divergences, and a scan showed
that not one of them appears in any of the 406 - which is precisely why the
score did not move. Three of the fourteen were found only while checking the
others, and two items on the first list turned out not to be divergences at
all: the characters in question render as nothing on a terminal, and the
output had been read off a screen instead of out of the bytes. Print the
bytes. A corpus can only ever report on the questions it asks,
so when the score stops moving, go and read the specification.

Name the limit of the oracle too, not only the oracle. That harness checked
366 cases for a long time and skipped 38 of them, and the skipped ones were
not a random sample: the suite gives a case an event stream instead of a JSON
value exactly when the value cannot be written as JSON, which is the same
ground a parser is most likely to get wrong. Sixteen of those 38 turned out
to be valid documents the parser refused, all the while the score read "all
366 checked cases pass". Print how much of the corpus was asked, beside how
much of it passed.

And when the corpus is a corpus of *inputs*, ask what it says about the other
direction. yaml-test-suite tests no writer at all, so the whole write side of
that module went unmeasured behind a parser score of 395 of 395. Running the
suite backwards - parse, write, parse, compare - costs nothing, needs no new
corpus, and scored 90.4% the first time it was run. It found a resolved tag
written as bare text, block scalars with no chomping indicator, folding that
turned line breaks into spaces, and three parser defects besides, one of them
a four-hundred-line loop that existed in two drifted copies. If a module both
reads and writes, the corpus you already have measures twice.

That last sentence is the most useful one on any of the three pages. A format
page that only lists features is a marketing document. Name the oracle, name
its limit, and name what nothing checks.

## Where a limitations document goes

It goes in the format page, under **Deviations** and **Not implemented**.

Not in a separate file. `documentation/YAML-LIMITATIONS.md` was a 504-line
standalone status document with no `@page` directive, which meant Doxygen
never rendered it and nothing linked to it. Because nothing linked to it,
nothing kept it honest: by the time it was folded into
\ref format_yaml "the YAML page" it described merge keys, DOM mutation, node
cloning, YAML-to-JSON conversion and the fuzzing infrastructure as
unimplemented, all five having shipped, and led with a critical bug whose
description no longer matched the defect that was actually still there.

A limitations list that lives next to the feature list gets corrected when
the feature list does.
