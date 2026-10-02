@page format_ini INI (Desktop Entry)

# INI (Desktop Entry)

There is no INI specification, so this module implements a **named dialect**
chosen by the caller rather than a format called "INI". The default is the
freedesktop.org **Desktop Entry Specification, version 1.5, dated 2020-04-27**,
which is the only INI dialect with both a normative document and two independent
reference implementations to measure against - and they disagree with each other
and with the specification often enough that the disagreements are the most
useful part of this page. Every claim here was checked against GLib's `GKeyFile`
2.84.4 and `desktop-file-validate` 0.28-1 before it was written; the
reproductions are in the workspace at `notes/text/INI-DIALECTS.md` §A.2, §A.3
and §A.9. The format index is \ref text_format_references "Formats".

## Normative references

- **Specification:** [Desktop Entry Specification, version 1.5, 2020-04-27](https://specifications.freedesktop.org/desktop-entry-spec/latest/)
  - [§3 Basic format of the file](https://specifications.freedesktop.org/desktop-entry-spec/latest/basic-format.html) - lines, comments, group headers, entries
  - [§4 Possible value types](https://specifications.freedesktop.org/desktop-entry-spec/latest/value-types.html) - the five types, the escape set, lists
  - [§5 Localized values for keys](https://specifications.freedesktop.org/desktop-entry-spec/latest/localized-keys.html) - the `[LOCALE]` postfix and the match order

The document tree comes from the allocator in
GTEXT_INI_Parse_Options::allocator, or the default, and owns every byte in it -
group names, keys, raw values, and the comment runs between them. Release it
with gtext_ini_free() and nothing else. A decoded string from
gtext_ini_unescape() or gtext_ini_escape() is released with
gtext_ini_string_free() **through the same allocator it was given**; a list with
gtext_ini_list_free(). An error's `context_snippet` comes from the default
allocator even when the parse used a caller's, for the reason
gtext_ini_error_free() documents.

## Parts implemented

- **§3 lines.** UTF-8 in, LF-separated, case significant everywhere. A final
  line needs no terminator. A line that is empty or holds nothing but spaces and
  tabs is a blank line, and §3.1 makes a blank line a comment.
- **§3.1 comments.** Lines *beginning with* `#`. There is no inline comment
  form, so a `#` or `;` inside a value is data. A comment line may hold any byte
  but LF, including bytes that are not UTF-8.
- **§3.2 group headers.** `[name]`, where the name is non-empty ASCII without
  `[`, `]` or a control character. The header may be followed by whitespace and
  by nothing else. Two groups may not share a name.
- **§3.3 entries.** `Key=Value`. The whitespace around `=` is ignored; the key
  charset is `A-Za-z0-9-` plus an optional `[LOCALE]` postfix; two keys in one
  group may not share a name. An entry before the first group header is refused.
- **§4 value types.** gtext_ini_value_bool() accepts exactly `true` and `false`;
  gtext_ini_value_int() reads a decimal integer into `int64_t`;
  gtext_ini_value_double() reads `%f` **in the C locale** whatever `LC_NUMERIC`
  says. gtext_ini_unescape() applies exactly `\s`, `\n`, `\t`, `\r` and `\\`.
- **§4 lists.** gtext_ini_value_list() splits on `;`, treats one trailing
  separator as the terminator, and reads `\;` as a literal semicolon - so `a;b`
  and `a;b;` are both two items and `a;b;;` is three.
- **§5 localized keys.** The postfix is kept in the stored key;
  gtext_ini_group_get_locale() does the match, in §5's order:
  `lang_COUNTRY@MODIFIER`, `lang_COUNTRY`, `lang@MODIFIER`, `lang`, then the
  unpostfixed key, with the encoding ignored. A postfixed key with no
  unpostfixed form is refused.
- **§3 preservation.** "Compliant implementations MUST not remove any fields
  from the file, even if they don't support them", and comments "should be
  preserved across reads and writes". An unmodified document therefore writes
  back **byte for byte**, unknown keys and all.
- **A second dialect.** gtext_ini_dialect_generic() is the same grammar with
  seven named changes - six relaxations and one normalisation - listed under
  *Dialects* below.

## Limits

| Option | Default | Effect |
|---|---|---|
| GTEXT_INI_Parse_Options::dialect | gtext_ini_dialect_desktop_entry() | The rules to read by |
| GTEXT_INI_Parse_Options::allocator | `NULL` | gtext_allocator_default() |
| GTEXT_INI_Parse_Options::max_total_bytes | 0 | No limit. Applied by the file reader while reading |
| GTEXT_INI_Parse_Options::max_groups | 0 | No limit |
| GTEXT_INI_Parse_Options::max_entries_per_group | 0 | No limit |
| GTEXT_INI_Parse_Options::retain_comments | **`true`** | Comments and blank lines are kept |

The two count limits default to no limit, and that is a measurement rather than
an oversight: an INI document is flat, so there is no recursion to bound and
nothing a count limit would make *safe* - it would only be a policy. The depth
limit every other parser here carries has no analogue.

`retain_comments` defaults to `true`, the opposite of
GTEXT_TOML_Parse_Options::retain_comments, because §3 requires a rewrite to
preserve comments: a default that dropped them could not rewrite a file
correctly, so the cheap default would be the non-conformant one.

## Save

The writer reproduces each line from the pieces the tree kept - the leading
whitespace, the key, the run from the key to the value, the value, and the run
from the value to the end of the line. So an unmodified document writes back
byte for byte, including `Name  =  spaced out   ` and a header with trailing
whitespace. An entry a caller created or whose value was changed is written
`key=value` with the dialect's terminator.

- GTEXT_INI_Write_Options::normalize writes `key=value` for everything and drops
  the original spacing. The round trip is then deliberately not byte-identical.
- GTEXT_INI_Write_Options::emit_comments, default `true`. False is a departure
  from §3's preservation requirement, available because a caller may be
  generating a file rather than rewriting one.
- GTEXT_INI_Write_Options::crlf, default false. §3 specifies linefeed.

**What it never writes is a document that would read back differently.** A value
containing a line terminator, or whose first byte is a space, is reported as
::GTEXT_INI_E_UNREPRESENTABLE rather than emitted - the first would end the line
early and the second would come back shorter, because §3.3 discards the run
after `=`. The caller's fix is gtext_ini_escape(), which spells both, and which
itself reports ::GTEXT_INI_E_UNREPRESENTABLE under a dialect whose escape set
cannot.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|---|---|---|
| Encoding | UTF-8; a NUL inside a value | Validated at the accessor, not the parse: gtext_ini_unescape() returns ::GTEXT_INI_E_BAD_UNICODE. Neither reference validates it earlier |
| Line terminator | LF | CRLF is refused at the first group header (::GTEXT_INI_E_BAD_LINE); a lone CR is never a terminator |
| Byte-order mark | - | ::GTEXT_INI_E_BAD_LINE. Both references refuse a document beginning with one |
| Comments | `#` at the start of a line; blank and whitespace-only lines | `;` is ::GTEXT_INI_E_BAD_LINE. No inline form - a `#` inside a value is data |
| Leading whitespace | - | A comment, header or entry indented by a space or tab is refused |
| Group header | `[name]` plus trailing whitespace | Missing `]` is ::GTEXT_INI_E_BAD_GROUP; anything else after `]` is ::GTEXT_INI_E_BAD_LINE; an empty, non-ASCII, bracketed or control-bearing name is ::GTEXT_INI_E_BAD_GROUP |
| Duplicate group | - | ::GTEXT_INI_E_DUPGROUP (§3.2) |
| Entry | `Key=Value`, whitespace around `=` ignored | A line with no `=` is ::GTEXT_INI_E_BAD_LINE; one before the first header is ::GTEXT_INI_E_NO_GROUP |
| Key name | `A-Za-z0-9-` plus `[LOCALE]` | Anything else is ::GTEXT_INI_E_BAD_KEY, including an empty key |
| Duplicate key | - | ::GTEXT_INI_E_DUPKEY (§3.3) |
| Localized key | `key[lang_COUNTRY.ENC@MOD]` | An orphan postfixed key is ::GTEXT_INI_E_BAD_KEY (§5) |
| Trailing whitespace in a value | Kept, as part of the value | The specification is silent; `GKeyFile` keeps it and so does this |
| Escapes | `\s \n \t \r \\` | Anything else, and a trailing lone backslash, is ::GTEXT_INI_E_BAD_ESCAPE. `\;` only inside a list |
| Booleans | `true`, `false` | Everything else is ::GTEXT_INI_E_TYPE - not `1`, `yes`, `on` or `True` |
| Numbers | Decimal `int64_t`; `%f` in the C locale | Leading or trailing whitespace, `inf` and `nan` are ::GTEXT_INI_E_TYPE; out of range is ::GTEXT_INI_E_RANGE |

## Dialects

gtext_ini_dialect_generic() is Desktop Entry with **seven named changes and
nothing else**, in the shape `gtext_csv_dialect_permissive()` already takes for
CSV: a derivation from a specification rather than an invention, so that its
correctness is inherited rather than asserted. **Six of the seven are
relaxations and the seventh is not**, which is a distinction worth keeping
because only the six carry the inherited property.

The six relaxations - each only *widens* what is accepted:

1. `;` begins a comment as well as `#`.
2. A comment, header or entry may be indented.
3. Entries may appear before the first group header. They live in a group with an
   empty name for which gtext_ini_group_is_preamble() is true and **no header is
   written**, so a rewrite does not invent a `[]` line. This was refused outright
   until the git config dialect needed it, because git accepts one and
   `git-config(1)` says it cannot.
4. A duplicate group is accepted, and a lookup merges the two.
5. A duplicate key is accepted, last one winning a lookup.
6. A key may be any bytes but `=` and a line terminator; a group name any bytes
   but `[`, `]` and a control character.

The one normalisation:

7. CRLF is a line terminator, and a leading byte-order mark is skipped.

**Number 7 changes a value rather than widening acceptance**, and that is the
whole reason to separate it. A document with `Exec=/bin/true` followed by CRLF
is accepted by *both* dialects - the strict one reads the value as
`/bin/true\r`, because §3 makes the CR part of the line, and the generic one
reads `/bin/true`. So the inherited property is: **every document the strict
dialect accepts, and that contains no CR, parses identically under both**. The
fuzz harness asserted it without that proviso and trapped at 30,209 executions;
`make conformance-ini-desktop-entry` now excludes a file containing a CR from
the parity score and counts it.

The escape set and the list separator are deliberately untouched: a change to
how a *value is decoded* would not be a relaxation either, and unlike CRLF there
is no reason to want one.

### git config

gtext_ini_dialect_git_config() is `git-config(1)`'s "Syntax", and it is **not**
built by relaxing Desktop Entry in either direction - which is why it is written
out field by field rather than derived. It accepts documents Desktop Entry
refuses *and* refuses documents Desktop Entry accepts:

| | git config accepts | Desktop Entry |
|---|---|---|
| `;` comment | yes | ::GTEXT_INI_E_BAD_LINE |
| Comment **anywhere on a line** | yes, `k = v # c` is `v` | no such rule |
| Entry before any header | yes, named `k` with no prefix | ::GTEXT_INI_E_NO_GROUP |
| Key with no `=` | yes, value **absent** | ::GTEXT_INI_E_BAD_LINE |
| Backslash continuation | yes, joins with nothing | no such rule |
| Quoted run in a value | yes, a **toggle** | `"` is an ordinary byte |
| Subsection `[a "b"]` | yes, case-sensitive | `a "b"` is just a group name |
| Repeated key | every occurrence is a value | ::GTEXT_INI_E_DUPKEY |
| Entry after `]` on one line | yes | ::GTEXT_INI_E_BAD_LINE |
| | **git config refuses** | **Desktop Entry accepts** |
| Key not starting with a letter | ::GTEXT_INI_E_BAD_KEY | `1k` and `-k` are fine |
| Key with `_` or `.` | ::GTEXT_INI_E_BAD_LINE | `_` no, `.` no - but for a different reason |
| Group name outside `A-Za-z0-9-.` | ::GTEXT_INI_E_BAD_GROUP | any ASCII but `[`, `]`, control |
| Non-ASCII group name | ::GTEXT_INI_E_BAD_GROUP | also refused (§3.2), but `GKeyFile` accepts |

Four rules of it are worth stating on their own, because each is a place a
plausible implementation goes wrong and none of the four is in the manual page:

1. **Quoting is a toggle, not a wrapper.** `k = x" mid "y` is the single value
   `x mid y`, and `k = "a"b` is `ab`. A reader that required the value to begin
   and end with a quote would refuse two documents git accepts.
2. **Trailing whitespace is dropped by tracking the last content byte, not by
   trimming from the right.** An escape counts as content, so `k = a\t` keeps the
   tab while `k = a ` does not keep the space. A right-to-left trim gets the first
   of those wrong. **A continuation also fixes that boundary**, which is the rule
   with the least support in the manual page and the one this module got wrong
   first: `k = false   \` keeps its three trailing spaces, and the same value
   without the backslash does not. It took a document carrying a trailing-space
   value *and* a continuation for the differential to show it, which is why it
   appeared at 20,000 generated documents and not at 500.
3. **git's whitespace is not C's.** git carries its own ctype table in which
   `\v` and `\f` are control characters rather than space, so a trailing `\v`
   stays in the value and a leading one is a syntax error rather than skipped
   indentation. A parser built on `isspace()` differs from git on exactly those
   two bytes - a difference no corpus of real files would ever show.
4. **One file, two escape layers, chosen by position.** Inside a subsection name
   a backslash *drops*: `[a "x\ty"]` is the subsection `xty`. The same two bytes
   in a value one line later are a tab.

And two consequences reach the API rather than the grammar:

- **A valueless key is not an empty value.** `k` and `k =` are both legal and are
  different entries, so gtext_ini_group_get() returning NULL would mean two
  things - absent, or present-with-no-value, which git's porcelain reads as
  boolean true. gtext_ini_group_value_present_at() is the distinction, and
  gtext_ini_group_find() is the index primitive it pairs with.
- **A name has a spelling and a canonical form.** The tree keeps
  `[Remote "orig in"]` as the document wrote it, so a rewrite is byte-identical,
  and carries `remote.orig in` beside it for lookup -
  gtext_ini_group_canonical_name() and gtext_ini_group_canonical_key_at(). The
  section part folds and a **quoted** subsection does not, so `[a "SubB"]` is
  reachable only as `a.SubB` and `[a.SubB]` only as `a.subb`. Measured: git
  answers both of those and refuses the two crossed spellings.

#### What the git differential measured

`make check-ini-git-oracle` generates the population rather than collecting one,
and the reason is measurable. Over the **25** git config files on this machine -
every `.git/config` under `$HOME` plus `/etc/gitconfig` - **24** carry a quoted
subsection and **zero** carry any of the other ten constructs above: no dotted
subsection, no continuation, no quoted value, no valueless key, no inline comment,
no repeated key, no `;` comment line, no CRLF, no preamble entry, no non-ASCII
byte. A conformance run over that corpus would score one construct out of eleven
and print clean.

At 20,000 generated documents, seed 20260929, against git 1:2.47.3-0+deb13u1:

| Score | | Excluded |
|---|---:|---:|
| `intent` - our verdict is what the generator meant | **20,000 / 20,000** | - |
| `legality` - our accept/reject matches git | **20,000 / 20,000** | - |
| `values` - canonical names and values match `--list` | **10,924 / 10,924** | 1,108 |
| `lookup` - a single-value read answers the last, as `--get` does | **10,924 / 10,924** | 1,108 |
| `rewrite` - an accepted document writes back byte for byte | **12,032 / 12,032** | - |

93 of 93 axes exercised. The 1,108 exclusions are one cause under two names -
`nul-in-value` 678 and `nul-in-subsection` 452 - because git reads a config file
with C string functions and truncates at a NUL where this reader keeps the bytes.
They are counted and named rather than dropped, and they stay in `legality` and
`intent`, which both implementations do answer.

**One reference, not two**, and that is this gate's weakness rather than its
design: the same program decides whether a git config file is legal and what its
values are, so "we agree" cannot be told from "we are both wrong the same way".
What stands in for a second opinion is the `intent` score - every rule written
down from `git-config(1)` and from measurement before either program is asked.

Nine mutations were applied to see the gate fail, and **seven of the nine moved a
score**: admitting `_` to the key charset (intent and legality to 492/500),
treating `\v` and `\f` as whitespace (490/500), dropping the continuation rule
(453/500), discarding the BOM on write (rewrite 297/318), folding a quoted
subsection (values 272/285), removing one exclusion-table entry (values 285/305 -
the excluded count drops, which is the control on the exclusion set itself), and
stopping one axis in the generator (the axis check). Two did not, and both are
recorded rather than explained away:

- **Making `DUPKEY_COLLECT` answer the first occurrence changed nothing**, because
  every score walked the tree and none called the single-value lookup. The
  `lookup` score was added for it, and now moves to 198/285.
- **Removing the writer's representability check changed nothing**, and cannot:
  every value a parse stored is writable by construction, so that path is
  reachable only from a caller setting one. It is covered by
  `IniGit.TheWriterRefusesAValueThatWouldNotReadBackAsItself` instead, which is
  the honest place for it.

What is deliberately left out is git's *types*. `--type=bool`, `--type=int` with
its `k`/`m`/`g` suffixes and `--type=path` are its porcelain's rules rather than
its file grammar's, and the one of those that reaches the grammar - a valueless
key meaning true - is spelled here as an absent value for the caller to read.

### EditorConfig

gtext_ini_dialect_editorconfig() is specification **0.17.2**, and it is the only
dialect here whose correctness claim is a **pass count against a normative
suite** rather than an agreement with a reference. The specification says a
conforming core "must pass the tests in the core-tests repository", and
`make conformance-ini-editorconfig` scores the 34 of that suite's 202 assertions
that test the grammar. The other 168 test a filepath glob matcher (130), file
discovery and precedence (24), value semantics (10) and a command line (3); none
is a text library's job, and quoting 202 while scoring a sixth of it would be the
wrong number.

Like git config it is not a relaxation of Desktop Entry in either direction, so it
too is written out field by field:

| | EditorConfig accepts | Desktop Entry |
|---|---|---|
| `; comment` | a comment | ::GTEXT_INI_E_BAD_LINE |
| `  [a]`, `  k=v`, `[a]   ` | the line is trimmed **before** it is classified | leading whitespace refused |
| `k=v` before any header | a preamble; `root` is defined to live there | ::GTEXT_INI_E_NO_GROUP |
| `[a]` twice, `k=1` twice | merged, last assignment wins | ::GTEXT_INI_E_DUPGROUP, ::GTEXT_INI_E_DUPKEY |
| `[a]b]`, `[a#b]`, `[]`, `[ a b ]` | any byte between the brackets, closing at the **last** `]` | `[`, `]` and control characters refused; `[]` refused |
| `ke y=v`, `k:e=v` | the key is everything before the **first** `=` | ::GTEXT_INI_E_BAD_KEY |
| CRLF, a leading BOM | accepted | LF only, BOM refused |
| `Indent` and `indent` | one key: keys are case-insensitive | two keys |

and it refuses what Desktop Entry accepts:

| | EditorConfig | Desktop Entry |
|---|---|---|
| `k=a\nb` | `\n` is two bytes; the dialect defines **no escapes** | a newline |
| `k=a;b;` | one value; there is no list spelling | a two-item list |
| `bare` | ::GTEXT_INI_E_BAD_LINE - the pair rule needs the `=` | ::GTEXT_INI_E_BAD_LINE, same |
| `k:v` | ::GTEXT_INI_E_BAD_LINE - `:` is not a separator | ::GTEXT_INI_E_BAD_LINE, same |
| `[a] junk` | ::GTEXT_INI_E_BAD_LINE - after the trim a header must end with `]` | ::GTEXT_INI_E_BAD_LINE, same |
| `[a]\rk=v` | ::GTEXT_INI_E_BAD_LINE - a lone CR is not a line separator | one line, the CR is data |
| `k=v ` | the trailing run is trimmed | the trailing run is **kept**, because GLib keeps it |
| `k=\va\v` | `\v` and `\f` are whitespace here | not whitespace in any other dialect |

That last row is the one field no other dialect sets. Both EditorConfig cores ask
the platform - core-c calls `isspace()` and core-py matches Python's `\s` - while
git carries its own ctype table in which `\v` and `\f` are control characters, so
the two dialects hold opposite rules about exactly two bytes.
::GTEXT_INI_Dialect::ctype_whitespace is that choice. No corpus of real files
contains either byte in either position.

#### Both references fail the suite, and that is the finding

**editorconfig-core-c 0.12.11 and editorconfig-core-py 0.17.1 each score 33 of
34.** Both fail the same assertion, `semicolon_or_hash_in_property`: the
specification says a `#` or `;` "anywhere other than at the beginning of a line
does *not* start a comment, but is part of the text of that line", and both cores
truncate a value at a whitespace-preceded one. This module scores 34 of 34.

Their agreement there is **not** corroboration, and the difference from the
Desktop Entry pin matters. `GKeyFile` and `desktop-file-validate` share a
community and not an implementation, so when they agree that is two readings; the
two EditorConfig cores both descend from Python's `ConfigParser` - core-py says so
in its own docstring and core-c is `inih`, which cites it as well - and the
inline-comment truncation is precisely the inherited behaviour. So the
specification gets the vote, and `make check-ini-editorconfig-oracle` is built
around saying so.

**Twenty constructs in all** separate at least one core from the specification, the
inline-comment truncation above included - eleven of them core-c's and fourteen
core-py's, overlapping in five. All were measured;
`notes/text/INI-DIALECTS.md` §A.16 has the transcript, and the gate's
`divergence-c` and `divergence-py` scores hold each one as a standing assertion.
The others worth naming here, because each is a rule a reader might otherwise copy
from a reference:

- **core-py refuses an indented comment.** It tests the line's first byte before
  stripping, so `  # c` is a parse error - against its own specification's first
  line rule, and the suite does not cover it.
- **core-py splits lines on a vertical tab, a form feed and a lone CR**, because
  Python's `splitlines()` does. `k=a\vb` is two lines to it.
- **core-py refuses `[a#b]`, `[a;b]` and `[]`**, all of which the specification
  allows.
- **core-py ends a key at a `:`**, so `k:e=v` is `k` = `e=v`, and maps the exact
  value `""` to the empty string. Neither is in the specification.
- **core-c silently drops** a key over 1024 bytes, a value over 4096, and splits a
  physical line at 5000 - the specification's lengths are a **floor** ("cores must
  accept ... up to and including"), not a cap, which is why this dialect needed no
  length field at all.
- **core-c reads `=v` as a property whose name is the empty string.**
- **Both** accept `:` as a separator and both silently ignore whatever follows a
  header's `]`.

#### What the EditorConfig differential measured

`make check-ini-editorconfig-oracle` asks both cores about generated documents.
Every section name it emits is a literal filename with no glob metacharacter, and
the query is one of those names, so the differential needs **no glob matcher** -
globbing is the conformance gate's business, not this one's.

At 20,000 generated documents, seed 20260929:

| Score | | Excluded |
|---|---:|---:|
| `intent` - our verdict is what specification 0.17.2 says | **20,000 / 20,000** | - |
| `rewrite` - an accepted document writes back byte for byte | **12,699 / 12,699** | - |
| `values-c` - resolved properties match core-c | **5,469 / 5,469** | 843 |
| `values-py` - resolved properties match core-py | **6,045 / 6,045** | 814 |
| `divergence-c` - each known departure of core-c is still there | **11 / 11 axes** | - |
| `divergence-py` - each known departure of core-py is still there | **14 / 14 axes** | - |

61 of 61 axes exercised. The `divergence` scores are the unusual ones and they are
the point: a document carrying a construct a core is known to get wrong is taken
*out* of that core's `values` denominator and put into a per-axis check that the
departure is still observable. A core fixed upstream fails `divergence` loudly
rather than quietly inflating `values`, and so does a generator that stops
emitting the discriminating document. The exclusions are the cases with no oracle
at all: a NUL in a value, which core-c truncates at, and an invalid UTF-8 byte,
which core-py's codec refuses before its parser ever sees it.

Eleven mutations were applied to see the two gates fail, and **nine moved a
score**: closing the header at the first `]` (intent 1,134/1,200 and both `values`),
dropping `\v`/`\f` from the whitespace set (values-c 278/342), turning inline
comments on (conformance **25 of 34**, values-c 261/342), folding group names as
`fold_case` alone would (conformance **5 of 34**), keeping trailing whitespace
(conformance 33 of 34), not skipping a BOM (conformance 33 of 34), treating a
header remainder as an entry (intent 1,162/1,200), narrowing the separator run's
whitespace to git's (values-c), and refusing an empty section name (intent
1,159/1,200 *and* `divergence-py` losing `section-empty` - the conformance gate saw
nothing, because the suite has no `[]` case). Two did not:

- **Setting `valueless_keys` true changed nothing**, and that is a fact about the
  flag rather than about the gates: a dialect whose keys may hold any byte has to
  find the `=` before it knows where the key ended, so a line with no `=` is a bad
  line before there is a key to call valueless. The flag is only meaningful with a
  closed key charset, and ::GTEXT_INI_Dialect::valueless_keys now says so.
- **Removing the writer's representability check changed nothing**, and cannot,
  for the same reason it cannot in the git differential: every value a parse stored
  is writable by construction. It is covered by
  `IniEditorConfig.TheWriterRefusesAValueThatWouldNotReadBackAsItself`.

What is deliberately left out is EditorConfig's *properties*. `indent_size`,
`tab_width`, the `unset` value and the lower-casing that both cores apply to six
known property names are semantics on top of the grammar, and they belong to
whatever reads the document. So does the filepath glob.

### systemd

gtext_ini_dialect_systemd() is `systemd.syntax(7)`, and the fifth and last of the
named dialects. Every rule was measured against **systemd 257** in a pinned
container, because **this machine has no systemd at all** - no `systemd-analyze`, no
`systemctl`, PID 1 is `init` - while carrying 165 unit files shipped by other
packages. So the corpus is local and the reference cannot be, which is the reverse of
every other dialect here and the reason this one came last.

Like the two before it, not a relaxation of anything:

| | systemd | elsewhere |
|---|---|---|
| `A=W1\` + `W2` | one value, `W1 W2` - the backslash **becomes a space** | no continuation, or git's joins with nothing |
| `A=W1\` + `# c` + `W2` | the comment block is **skipped** and the halves join | - |
| `A=W1\` + blank + `B=2` | the blank line **ends** the continuation; the backslash disappears | - |
| `[Serv\` + `ice]` | one section named `Serv ice` - **a name may be continued** | no other dialect has this |
| `A=1<CR>B=2` | two settings: **a lone CR ends a line** | data to Desktop Entry, whitespace to git and EditorConfig |
| `A=1` before any header | refused - "Assignment outside of section" | EditorConfig's specification names a preamble; git accepts one |
| `\a \b \f \v \n \r \t \s \\ \" \'` | all of them | Desktop Entry has five, git has five, EditorConfig none |
| `\x41 \101 \u00e9 \U0001F600` | four **variable-length** numeric forms | no other dialect here |
| `ExecStart=/bin/foo \q` | **parses**; the complaint arrives at the accessor | git refuses the document |
| `A="x" 'y' z` | three words, quoting removed - but only if a caller asks | git's quoting is in the grammar |
| `A=yes` | boolean true | Desktop Entry admits `true` and `false` and nothing else |
| `\v` | **not** whitespace | not whitespace to git either; whitespace to EditorConfig |

Three of those needed something the other four dialects did not.

**The continuation is a property of the line, not of the value.** git's exists only
inside a value, so gtext_ini_scan_value() can own it; systemd assembles the logical
line *before* classifying it, so the same rule has to serve a group header and a key
as well. gtext_ini_continuation_at() is that one implementation, asked by the parser,
the value scanner and the canonical-name step - and the consequence for the tree is
that a name is no longer a span of the document. It keeps the bytes as written, so the
rewrite is byte-identical, and the **joined form is the canonical form**, which is the
same two-form storage case folding already needed.

**Variable-length escapes**, with multi-byte output. ::GTEXT_INI_Dialect::escapes is a
set of letters and cannot hold `\xHH` or `\U0001F600`, so
::GTEXT_INI_Dialect::numeric_escapes is a second field. A lone surrogate is *encoded*
rather than refused - `\ud800` is `ED A0 80`, which is what systemd produces.

**Quoting and escaping are not in the grammar**, and the specification says so
itself - quoting applies only "for settings where quoting is allowed", which the
grammar cannot know because it depends on the setting. That is this page's layering
claim stated by somebody else, so ::GTEXT_INI_Dialect::quoted_values is false,
::GTEXT_INI_Dialect::escapes_in_grammar is false, and gtext_ini_value_words() is where
a caller splits a value. git is the only dialect here that refuses a bad escape while
*reading*, and that too was measured rather than assumed.

#### Where this module departs from systemd on purpose

**systemd warns about a malformed line and skips it; this refuses the document.** A
unit file with `not a pair` on line 6 loses line 6 and keeps the rest, and
`systemd-analyze verify` exits **0**. This module returns ::GTEXT_INI_E_BAD_LINE.

The reason is the caller. A library's user cannot see a warning, so a reader that
skipped the line would surface the failure as behaviour - a setting silently absent -
rather than as an error. systemd can do it because a warning reaches its journal and a
human; a parser handing back a tree cannot. So the differential compares the
**presence** of a grammar fault, which is the part both agree on, and not the recovery.

#### What the systemd differential measured

The oracle question was open from the first day of this work, and
`notes/text/INI-DIALECTS.md` §A.13 listed it as the one reference whose feasibility had
never been tested. The answer is yes, with two caveats that took four probe rounds to
find:

- **`systemd-analyze verify` exits 0 on a syntax error.** Its exit status reports
  *semantic* failure - a service with no `ExecStart=` - and says nothing about the
  grammar. So the oracle is the **diagnostics**, classified; a gate built on the status
  would have scored every broken document as legal and printed clean.
- **Values come back through `Environment=` and nowhere else.** No verb prints a parsed
  setting, but that one validates each word as `NAME=VALUE` and reports a failure
  verbatim - after unquoting, unescaping and word splitting. It is systemd's whole
  value grammar echoed back, one word per line.

  And that channel **rewrites a CR to an LF** and **truncates at 2,097 bytes**. Both are
  measured, both live in the differential's exclusions, and the first was found by a
  minimal pair: all four spellings of a carriage return arrive as `0x0a` while an
  `\x0e` escape arrives as `0x0e`, so the rewriting is the logger's and not the
  decoder's. Read in a terminal, that difference says systemd decodes `\r` to a
  newline, which would be a systemd bug rather than a reporting artefact.

At 5,000 generated documents, seed 20260929, against systemd 257.13-1~deb13u1:

| Score | | Excluded |
|---|---:|---:|
| `intent` - our verdict is what the rule says | **5,000 / 5,000** | - |
| `grammar` - a line-grammar fault is reported by both or neither | **4,745 / 4,745** | - |
| `words` - `Environment=` splits the same way | **3,509 / 3,509** | 79 |
| `rewrite` - an accepted document writes back byte for byte | **3,588 / 3,588** | - |
| `divergence` - systemd is still wrong where it is known to be | **1 / 1 axis** | - |

83 of 83 axes exercised. `make conformance-ini-systemd` reads every unit file on the
machine: **165 of 165 parse and write back byte for byte**.

**That corpus gate prints a set of zeros, and they are the point.** Of the 165 files, a
`;` comment, CRLF, a byte-order mark, a non-ASCII byte and **every escape sequence**
appear in **none**, and a line continuation in 2. So a clean run over every systemd
unit file on a working Linux system says almost nothing about the rules the dialect is
hardest to get right, which is why the gate prints the census rather than only the
score.

#### A systemd defect the gate asserts is still there

**systemd skips a byte-order mark too late.** The skip happens after the comment test
and after the leading-whitespace skip, so a first line of `<BOM># c` is not recognised
as a comment; it falls to the assignment branch, which reports "Assignment outside of
section." before it has even looked for an `=`. Pinned down by six probes: `<BOM>[Unit]`
is fine and `<BOM>` alone is fine, while `<BOM># c`, `<BOM>xyz`, `<BOM>k=v` and
`<BOM>   # c` all draw the spurious complaint.

This module skips the mark properly. Rather than excluding those documents, the gate
asserts per axis that the departure is **still observable** - the same instrument the
EditorConfig differential uses - so a systemd fixed upstream fails loudly instead of
quietly inflating `grammar`.

Twelve mutations were applied, and **eleven moved a score**: joining with nothing
instead of a space (intent 754/800), not skipping a comment block (754/800), not
ending at a blank line (rewrite 538/562), a lone CR not terminating (intent 777/800),
allowing a preamble (739/800), a header remainder as an entry (783/800), validating
escapes during the parse (732/800), dropping the numeric escapes (words 489/544),
quoting as a wrapper rather than a toggle (words 517/544), keeping a trailing lone
backslash in a word (words 542/544), and assembling only the physical line (intent
739/800).

The header-remainder mutation moved nothing at first, and the fix was the generator's
rather than the gate's: `[Install] junk` is refused whatever the flag says, because a
remainder with no `=` is a bad line either way, so the axis had to emit
`[Install] WantedBy=...` before the flag could matter. The one mutation still not
caught is the boolean set, and it cannot be: the reference's only value channel is
`Environment=`, which reports words, so nothing in the differential ever asks
gtext_ini_value_bool() anything. `IniSystemd.BooleansTakeTheWiderSet` covers it.

What is deliberately left out is systemd's **types**. Its time spans belong to
`ghoti.io-chron`, whose duration type already covers `s min h d w ms us` and the
summing rule; its sizes and its `%`-specifiers - which need the unit name and the
host - are not a text parser's to know.

### Python configparser

gtext_ini_dialect_configparser() is Python's `configparser`, and it is **the one
dialect here with no specification at all**. The Python documentation describes what
the module does rather than defining a format, and says so; there is no normative
document, no conformance suite, and nothing to cite. So every field of the dialect is
a measurement against CPython, and the module's own source settled two rules no
probe of a single document could have.

The reference version is pinned in `tools/oracle/containers/IMAGES`. The pin is
CPython 3.14 and this machine's interpreter is 3.13.5, so before anything was built on
the host probes they were **re-run inside the pin**: all 102 probe documents and the
38-document channel comparison are byte-identical between the two. 3.13 rewrote
`_read_inner()` and added `MultilineContinuationError`; 3.14 changed nothing this
format can see.

Not a relaxation of anything:

| | configparser | elsewhere |
|---|---|---|
| `k: v` | `=` **or** `:`, whichever comes first - so `k:b=c` is `b=c` and `k=b:c` is `b:c` | `=` only, everywhere else |
| `k = 1` + `  2` | one value, `1\n2` - **an indented line continues it, joined with a newline** | git and systemd use a trailing backslash; the other two have no continuation |
| `  k = 1` + `  2` | **refused.** The comparison is against the entry's own line, strictly | - |
| `k = 1` + `#c` + `  2` | the comment contributes nothing and does not end it: `1\n2` | systemd skips a comment block too; nothing else has one to skip |
| `k = 1` + blank + `  2` | the blank line **is** a line: `1\n\n2` | a blank line *ends* a systemd continuation |
| `k = 1` + `  2` + blank + `[b]` | the value ends at `2`; the blanks belong to the document | - |
| `j = ;black` | the value is `;black` - a **value** may begin with a comment introducer | the same everywhere; what is new is that an indented `;white` under it is a comment |
| `[a]junk` | the section `a`, and `junk` is **discarded** | Desktop Entry refuses it; git reads an entry from it |
| `[]` | refused - the header pattern needs one character | EditorConfig accepts it, and is the only dialect that does |
| `[a]b]` | one section named `a]b` | EditorConfig agrees; the other three close at the first `]` |
| `[ b ]` | a section literally named `" b "` | the name is never trimmed anywhere here |
| `[A]` and `[a]` | **two** sections, while `k` and `K` are one option | git folds both; EditorConfig folds the key only, as here |
| `k1 = 1` + `K1 = 2` | refused - the duplicate check is on the **folded** name | Desktop Entry refuses a duplicate and does not fold |
| `[a]` twice | refused. `strict=True` is the default | git and systemd merge; EditorConfig lets the later win |
| `k = a ; c` | the value is `a ; c` - inline comments are **off by default** | git truncates at a `#` or `;`; both EditorConfig cores do too, against their own specification |
| `k = "q v"` | the quotes are part of the value | git's quoting is in the grammar |
| `k = a\nb` | four characters; **no escapes exist** | Desktop Entry has five, git five, systemd eleven plus four numeric forms |
| `k` alone | refused. `allow_no_value=False` is the default | git reads it as boolean true |
| `k = v<CR>j = w` | two entries - **a lone CR ends a line** | systemd agrees; the others treat it as data or whitespace |
| `\x1c` to `\x1f` | **whitespace** | `isspace()` does not say so, so no other dialect here agrees |
| `k = TRUE` | boolean true - the eight words, **folded** | systemd admits the same eight and folds none of them |
| `<BOM>[a]` | refused. `read()` decodes as UTF-8, not `utf-8-sig` | the generic dialect skips a mark; Desktop Entry refuses one |

Four of those forced a change to the dialect struct, and each is an axis a `bool`
could not hold:

- **Two separator characters** (::GTEXT_INI_Dialect::separators). It is a set rather
  than a character because two places have to agree about it - where a key ends, and
  whether a byte may appear inside one - and a charset written against only the first
  would read `k:v` as a key called `k:v` in one and as `k` = `v` in the other.
- **A third whitespace set** (::GTEXT_INI_SPACE_PYTHON). Python's `\s` includes the
  four ASCII separator controls and C's `isspace()` does not. Measured in all four
  positions the predicate is asked about: they strip as indentation, sit between a key
  and its delimiter, trim off a value's end, and may precede a comment introducer.
- **A third answer for what follows a header's `]`**
  (::GTEXT_INI_HEADER_REMAINDER_IGNORE). Three references, three answers. This one
  keeps the bytes so a rewrite reproduces them and gives them no meaning.
- **Folded keys with unfolded sections** (::GTEXT_INI_Dialect::fold_group_case), which
  EditorConfig needs too. It used to be spelled as `fold_case` minus a name style,
  which was right while one dialect wanted it and became a list of exceptions in
  shared code the moment a second did.

**The continuation is the one that changed the parser**, and the way it differs from
the other two is the point. A backslash continuation is announced by the value's own
bytes, so a scanner walking the value finds it wherever it is - which is why systemd's
reaches a group header and a key. An indent continuation is a property of the **next**
line and cannot be recognised without knowing how far the entry's own line was
indented, so gtext_ini_dialect_scans_values() is false for it and the parser decides
the extent. Two consequences followed:

- A key's separator is sought on its **own physical line only**. The reference matches
  its option pattern against that line and appends every continuation line without
  looking at it, so a `=` on a continuation line is data. Bounding the search by the
  logical line instead read `k` followed by an indented `j=2` as one entry named
  `k\n  j`, where the reference reports the bare `k` as an error.
- The value's extent ends at the **last line that contributed text**, not at the first
  line that is not a continuation. The reference joins the pieces and then strips the
  result, so trailing blank and comment lines are not in the value - and a rewrite is
  byte-identical either way, which is why this needed a *value* comparison to find.

**Interpolation is off by default and available on request**, and the measurement is
why it is that way round rather than either extreme:

- Over the 479 real `configparser` documents on this machine the *default*
  `BasicInterpolation` refuses a value in **301 of them** and changes a value in
  **none**. Five files contain `%(` or `${` and not one is a reference it could
  resolve: the `${prefix}` in numpy's `npymath.ini` is pkg-config's own variable
  syntax, which `ExtendedInterpolation` would try to read as `section:key` and fail. So
  a reader that interpolated by default would read 63% of this machine's `.ini` and
  `.cfg` files worse and none of them better.
- **The population that could have overturned that was measured separately and did
  not.** The corpus gate scans `/etc /usr/share /usr/lib` filtered to `.ini` and
  `.cfg`, and the files that use interpolation are Python *project* configuration -
  `tox.ini`, `setup.cfg`, `alembic.ini`, `pytest.ini` - which live in project trees
  and home directories. There are 103 of those on this machine and **only 5 are within
  the corpus gate's reach**. Nine contain `%(name)s`, and running `configparser` over
  each shows every one of the nine is a `logging` format string
  (`log_cli_format = %(asctime)s %(levelname)s %(message)s`), which the default
  `BasicInterpolation` refuses with `InterpolationMissingOptionError` - `pytest` reads
  that file with `iniconfig`, not with `configparser`. Same finding as numpy's
  `${prefix}`, on the population chosen to break it.
- **What that measures is "not by default", and it was read for a while as "not at
  all".** Nothing above bears on whether a caller who wants Python's reading should be
  able to ask for it, and the docs claimed the wider thing under a heading full of
  numbers, which made an inference look measured. gtext_ini_value_interpolate() is the
  narrower claim implemented: ::GTEXT_INI_Interpolation selects `BasicInterpolation`
  or `ExtendedInterpolation`, gtext_ini_value_needs_interpolation() answers whether a
  value can be used raw under a style, and the default of every spelling is
  ::GTEXT_INI_INTERPOLATION_NONE. It is an accessor beside gtext_ini_unescape() and
  **not** a ::GTEXT_INI_Dialect field, because no reference in a value changes how a
  document is tokenized.
- **The pin is now asserted rather than printed**, which is the other half of what was
  wrong. `interpolation=None` is one of six configurations the differential pins, and a
  pin is *wider than an exclusion*: an exclusion names a document and keeps the
  knowledge, while a pin removes a behaviour from the comparison, so nothing would have
  failed if this module's handling of `%` changed or if the reference's had. The gate
  now runs the reference three times - `none` to score, `basic` and `extended` to score
  our own pass against the thing it reimplements and to fail if the default stops
  answering differently. See `pin` and `interp` under Tested scope.
- **One deliberate departure, which is a bound the reference does not have.**
  `max_depth` caps how deep a chain of references goes and not how large it gets:
  `a = ${b}${b}` doubles per hop, so ten hops is a thousandfold.
  GTEXT_INI_Interpolate_Options::max_output bounds the result - by default
  proportionally, at sixteen times the input or 64 KiB, whichever is larger - and
  `SIZE_MAX` restores the reference's own behaviour. The differential sets `SIZE_MAX`
  so that a policy cannot read as a disagreement, and
  `IniInterpolation.TheOutputIsBounded` exhibits the amplification before bounding it.

(An earlier measurement of the first point, over eleven files, reported three - correct
over that population and far too weak to decide anything.)

**Two things are deliberately not implemented**, each with the measurement behind it:

- **`[DEFAULT]`'s value inheritance.** A lookup policy over a parsed tree rather than a
  rule of the grammar: a caller who wants it asks the section and then asks `DEFAULT`.
  `[DEFAULT]` is an ordinary group here, and the differential pins `default_section` to
  a name no document can spell so that both sides agree about what it is. The one place
  it is a parameter rather than absent is
  GTEXT_INI_Interpolate_Options::defaults, because a reference *inside* a value has to
  resolve against some map and the reference's is that chain.
- **Unicode-aware whitespace and case folding.** Properties of Python's `str`, not of
  the format. `configparser` strips a no-break space from a value's edge and
  lower-cases `É` in a key; a byte-oriented reader can do neither, because neither is a
  single byte. Both are asserted as divergences rather than left as gaps - the
  differential scores that each departure is **still observable**, so a change to the
  fold fails loudly instead of quietly inflating the values score.

**A lone CR terminates a line here, and that is a choice between two behaviours of the
reference rather than a reading of it.** `read_string()` wraps its text in a
`StringIO` whose newline is `'\n'`, so no translation happens and `k=v<CR>j=w` is one
value; `read(path)` opens the file with `newline=None` and Python's universal-newline
translation turns a lone CR into an LF before `configparser` sees it. Of 38 documents
probed both ways that is the **only** difference between the two channels. A file is
what an INI document is, so this dialect follows the file, and the differential's
driver reads one for the same reason.

#### Tested scope

**`make conformance-ini-configparser`** is the only INI corpus gate whose reference is
**installed**, and that changes what a corpus can be asked. The Desktop Entry corpus
has a validator but no value reader that agrees with this one about everything; the
systemd corpus has neither, because this machine has no systemd. Here `configparser` is
in the standard library of the interpreter that scores the gate, so every file gets all
three questions. Measured over `/etc`, `/usr/share` and `/usr/lib`, **703 unique files**:

| Score | |
|---|---:|
| `verdict` - we accept exactly the files the reference accepts | **703 / 703** |
| `values` - sections, keys and joined values agree | **401 / 401** |
| `round trip` - byte for byte | **479 / 479** |
| `divergence` - excluded for Unicode whitespace, and still differing | **78 / 78** |

**A `.cfg` or `.ini` extension does not mean the file is one of these documents**, and
the gate measures that rather than filtering it away. The reference refuses **224 of
the 703**: 221 with `MissingSectionHeaderError`, because most `lit.cfg` files are
Python scripts and `/etc/dpkg/dpkg.cfg` is a section-less key-value file, plus one
duplicate option, one parse error and one file that is not UTF-8. Those 224 stay in the
denominator, and they are the only **refusals** a real corpus of this format offers - a
reader that accepted them would be wrong in the one direction no generated document
tests.

The 78 exclusions are one thing: `configparser` strips Unicode whitespace and this
reader strips ASCII whitespace, so a value that is a lone no-break space is empty there
and one byte long here. TeXLive's babel locale files have one. The exclusion predicate
is **narrow on purpose** - it fires only where the two strips reach a different byte -
and the gate asserts that every excluded file really does differ. The wide version of
the predicate ("the file contains any non-ASCII whitespace") excluded 115 files of
which 78 differ, so 37 agreeing files would have silently left the score; the assertion
is what keeps an exclusion from widening quietly.

The census it prints matters for the same reason the systemd one does: of 703 files a
**lone CR, a byte-order mark and a `[DEFAULT]` section appear in none**, and CRLF in
one. So the corpus is unusually good here and still silent about three rules.

**`make check-ini-configparser-oracle`** is what covers them: generated documents
against the pinned interpreter. At **20,000 documents**, seed 20260929, against
CPython 3.14.7:

| Score | | Excluded |
|---|---:|---:|
| `intent` - our verdict is what the generator's own reading says | **20,000 / 20,000** | - |
| `verdict` - our verdict matches the reference's | **16,030 / 16,030** | - |
| `values` - sections, keys and joined values agree | **10,145 / 10,145** | - |
| `rewrite` - an accepted document writes back byte for byte | **10,145 / 10,145** | - |
| `divergence` - each known departure is still observable | **2,965 / 2,965** | - |
| `interp` - our interpolation matches Basic's and Extended's | **32,060 / 32,060** | - |
| `pin` - `interpolation=None` still keeps a behaviour out | **2,622 / 2,622** | - |
| the reference's channel cannot decode the document | - | 1,005 |

88 of 88 axes exercised. The 1,005 exclusions are documents holding a byte that is not
UTF-8: `read()` raises `UnicodeDecodeError` before `configparser` sees a line, which is
a limit of the **channel** and not a disagreement about the grammar - the encoding is
`read()`'s parameter rather than the format's rule.

**The last two scores exist because a pin is wider than an exclusion**, and for one
round of this gate nothing could fail on the one pin that mattered. `interpolation=None`
was printed above the numbers, honestly, and the four `%`/`$` axes were generated and
scored all along - under a configuration in which both sides answer `100%` with `100%`,
which is agreement about nothing. An exclusion names a document and keeps the knowledge;
a pin removes a behaviour from the comparison, so no change to this module's handling of
`%`, and no change to the reference's, would have been visible here.

So the gate runs both sides three times: `none` for the five scores above, and `basic`
and `extended` for `interp`, which compares gtext_ini_value_interpolate() against the
implementations it reimplements, and for `pin`, which asserts per axis that the
reference's own *default* still answers a document differently from the pinned
configuration. `value-extended-interpolation` needs the third run and not the default,
measured: `BasicInterpolation` leaves `${sect:alpha}` alone, because `$` is not its
trigger byte, and an assertion that looked for a divergence under `basic` for every axis
in the table would have failed on that one and been right to. Every score was put in a
position to fail before being believed - a bare `%` treated as a literal, the
reference's `basic` run silently made the pinned one, and a construct stopped being
generated - and each broke the run loudly.

Two instrument bugs came out of that, neither in the library. The first: `interp`
originally scored every non-excluded document, on the reasoning that a document carrying
a Unicode divergence still has an interpolation answer. It does, and the answer is
contaminated - a no-break space at a value's edge is stripped by the reference under
*every* configuration - so the score read 625 of 756 while the pass was correct on all
of them. The second: the generator's `cont-empty-first-line` branch calls `value()` and
then **discards the line it built**, so a document recorded as carrying
`value-interpolation` went out with no `%(alpha)s` in it, and `pin` found the reference's
`basic` configuration answering it exactly as the pinned one did - true of those bytes
and silent about the rule. That is the same trap `needs_plain_entry()` already existed
for, met from the other side: those axes are chosen inside `value()` by the roll rather
than by a mode, so the guard has to read what `value()` marked.

**`intent` carries more weight here than in any other INI gate**, and that is a
consequence of the dialect having no specification. For git, systemd and EditorConfig a
document can be checked against prose; here the generator's own table *is* the prose,
and without it a rule both the reference and this library got wrong would pass every
other score untouched.

**The differential found two defects, and each needed a construct the corpus does not
contain.** Both are one shape: a CR is whitespace to any dialect that accepts CRLF
while an LF is whitespace to none, because a line ends at an LF before anything trims.
So a value whose first line is empty begins at its own terminator, and under CRLF that
first byte is a CR - which the parser skipped as leading whitespace, walking into the
continuation and losing the value's empty first line, and which the writer then called
leading whitespace too and declared unrepresentable, so a document just parsed could
not be written back. The corpus has one CRLF file and no lone CR at all.

**And the generator's own structure was the third finding.** Its first version offered
every axis independently, and three collisions followed, each of which read as a
library defect until it was traced: a refusing axis beside a divergence axis made the
divergence unobservable, because both sides refused the document and the departure
could not be seen; two refusing axes in one document made the `intent` expectation a
guess about which fault a parser reaches first; and a key drawn twice from a
five-element list is a duplicate key, which this dialect refuses. So a mode is chosen
first and each document carries at most one special construct. The symptom that named
the first of those was the gate's own "this divergence was never observed" assertion -
which is what that assertion is for.

#### Twenty-seven mutations, all twenty-seven caught

Each rule of the dialect and each new branch was flipped, and the gate that noticed
recorded. Every one moved a score, which is the first time in this module that has been
true of a dialect on the first pass - and the reason is that this is the fourth dialect
built with the same three instruments, so the gaps the earlier ones found were already
closed before this one started.

**Five are caught by the unit tests alone, and each for a reason that is a property of
the instruments rather than a hole in them:**

| Mutation | Why no score can reach it |
|---|---|
| `valueless_keys` turned on | unreachable for any dialect whose keys are not a closed character set: the key's extent is defined by the separator, so a line without one is a bad line before there is a key to call valueless. The same finding as EditorConfig's, and the field's own documentation says so |
| `bool_style` set to systemd's | the reference has **no channel that returns a boolean**. `getboolean()` is a second call on an already-parsed value, and the differential compares raw values |
| the writer's verbatim/synthesized split collapsed | reachable only through `gtext_ini_group_set()`. Both gates score documents this module *parsed*, and those take the verbatim path |
| a synthesized value's CR break allowed | the same reason, and this one was a **live defect** until it was found by re-reading the writer: the writer emitted `"a<CR>b"` and produced a document that would not re-parse |
| the join's final strip removed | a parse cannot produce a value whose last contributing line is blank, because the span it stores ends at the last line that contributed text. A caller handing `gtext_ini_unescape()` its own raw value can |

The last two are the honest reading of that table: **two of the five were unmeasured
until this pass**, not merely unreachable. The CR rule was a defect and is now a test;
the join's strip was dead to every gate and is now a test. A mutation that moves nothing
is a question - is this branch unreachable, or is it untested? - and answering it is the
point of applying the mutation at all.

#### Twenty-one more for interpolation, and the one that found a defect

gtext_ini_value_interpolate() got its own pass, because a feature with 16 unit tests and
a 32,060-comparison differential is still only as good as what would fail if it broke.
Twenty-one mutations, every one applied and caught - four by the `interp` score, one by
`values`, the rest by the unit tests alone, which is the expected split: a mutation that
changes a *value* is what a differential is for and a mutation that changes a *status* on
a document the reference also refuses is not.

**One of them was not a mutation.** `section-key-sees-the-defaults` added
GTEXT_INI_Interpolate_Options::defaults to the lookup behind a `${section:key}` path,
and no score moved - because the code was wrong and the mutation was the fix. The
comment above it read:

> A `section:key` reference does not see the defaults, because the reference reaches it
> through `parser.get(sect, opt, raw=True)` while a bare `${key}` reads the current
> section's map - and only that map is the chain that includes `[DEFAULT]`. **Measured
> rather than inferred from symmetry.**

It was inferred, and the word "measured" was doing the work that a measurement should
have. `get()` resolves through `_unify_values()`, which chains the section's own vars
with the defaults, so both spellings see them:

```
[DEFAULT]        ${shared}    -> from-default
shared = ...     ${o:shared}  -> from-default    <- we answered E_INTERPOLATION_MISSING
[o] own = ...    ${o:own}     -> from-o
```

The nested case agrees, because the reference recurses with
`dict(parser.items(sect, raw=True))` and `items()` merges the defaults too.

**Why no instrument could have caught it, which is the part worth keeping.** No unit
test covered the rule in either direction *and the differential cannot reach it at all*:
the driver pins `default_section` to a name no document can spell, precisely so that
`[DEFAULT]` is an ordinary section on both sides, so there is no defaults chain in that
gate to compare and `ini_cp_ours.c` passes NULL. A rule a gate excludes by construction
has to be carried by the unit tests, and the exclusion is exactly what makes it easy to
forget that. The mutation is now inverted - what must be caught is taking the defaults
*away* - and `IniInterpolation.BothExtendedSpellingsSeeTheDefaults` carries the rule.

**One mutation reported `NOT-APPLIED` on the second pass**, because the fix had rewritten
the call it anchored on from three lines to two. That is the check earning its place: the
harness asserts its anchor matches exactly once, and without that a mutation that lands
nowhere is indistinguishable from one nothing catches. Re-anchored and caught.

**And the harness made the same class of mistake as the code.** Its restore step took
each file back with `git show HEAD:`, which is right only while the working tree *is*
HEAD - and the second run was started with the defaults fix uncommitted. The first
restore reverted it silently; every later mutation would have been scored against code
that no longer contained the fix, and the fix would have been gone with nothing saying
so. It now snapshots the working tree's own bytes at startup and compares back after
each iteration, so a dirty tree - the normal case immediately after a mutation finds
something - is legitimate. Restoring to HEAD is not the same thing as restoring.

Nine mutations are caught by all three gates, and the two most interesting are about the
*extent* of a value rather than about a flag: ending the logical line at the first line
that is not a continuation (rather than at the last line that contributed text) and
letting the indent comparison be `<` instead of `<=`. Both change values while leaving
every rewrite byte-identical, which is why a round-trip score cannot see them and a value
comparison must exist.

### Win32 profile API

gtext_ini_dialect_win32() is the Win32 profile API - `GetPrivateProfileString`,
`GetPrivateProfileSection` and `GetPrivateProfileSectionNames` - and it is **the one
dialect here whose reference is not the thing it stands for**. There is no
specification; the API's documentation states two rules, quote stripping and case
insensitivity, and this dialect has thirty. Every other rule was measured under
wine, which is a reimplementation of the Windows API and not Windows.

**Why it exists at all, after being recorded as "never ship".** The disqualifying
fact was true and answered a different question. `GetPrivateProfileString` consults
the registry's `IniFileMapping` for the section and reads the file only *"if there is
no subkey or entry for the section name"*, so the **API's** answer is not a function
of the file's bytes: two machines with the same file can correctly return different
values. That rules out claiming to reproduce the API. It says nothing about reading
the `.ini` files that exist on Windows, and those overwhelmingly belong to
applications that parse the file themselves - the profile functions date to Windows
3.x and Microsoft's own documentation says they exist for 16-bit compatibility. The
large population is the one this dialect is for.

**The reference disagrees with itself, and the disagreement is the format's most
consequential rule.** `GetPrivateProfileSection` discards a line whose first
non-blank byte is `;`. `GetPrivateProfileString` retrieves it. So `;disabled=1` is a
comment to one entry point and a live setting to the other:

```
[a]
;disabled=1
k=v
```

`GetPrivateProfileSectionA("a", …)` reports `k=v` and nothing else;
`GetPrivateProfileStringA("a", ";disabled", …)` returns `1`. **This dialect follows
the enumeration API**, because a reader whose caller asks for `disabled` and is told
nothing is correctly served, while one handed a setting its author commented out
cannot be recovered from downstream. `#` is a comment to **neither** API - measured,
`#hash=2` is both listed and retrievable - and is an ordinary byte here. That is the
half most readers of this format get wrong in the other direction.

The `;` convention appears to work for prose comments under both APIs for a reason
that is not a comment rule at all: `; some prose` has no `=`, and a line with no
separator is invisible to `GetPrivateProfileString` anyway. It is the
*commented-out setting* - by far the commonest kind in a real `.ini` - where the two
APIs part.

Not a relaxation of anything:

| | Win32 | elsewhere |
|---|---|---|
| `;k=v` | a comment - following `GetPrivateProfileSection`, and **not** `GetPrivateProfileString` | a comment for generic, EditorConfig, systemd and configparser; a key for Desktop Entry |
| `#k=v` | an **entry**, key `#k` | a comment everywhere else |
| `[ b ]` | the section `b` - the name is **trimmed inside the brackets** | `" b "` for configparser and EditorConfig; refused by Desktop Entry and git |
| `[a]b]` | the section `a]b` - closes at the **last** `]` | the same for EditorConfig and configparser; the first `]` elsewhere |
| `[a]junk` | the section `a`, remainder discarded | three dialects, three answers - see ::GTEXT_INI_Header_Remainder |
| `[a` | **not a header.** A valueless entry named `[a`, and the section in force does not change | `GTEXT_INI_E_BAD_GROUP` everywhere else |
| `[]` | a legal header whose entries are reachable by **no name**: the empty name finds the preamble | accepted by EditorConfig, refused by the rest |
| `= v` | an entry whose key is the empty string, which a lookup retrieves | refused everywhere else |
| `novalue` | an entry with no value: listed by the enumeration API, invisible to the lookup | git alone, and only with a closed key charset |
| `k=a=b` | `a=b` - the **first** separator splits the line | the same everywhere; configparser's may be a `:` |
| `k="v"` | `v` - one matching pair of surrounding quotes comes off | git has a quote *toggle*; nobody else has either |
| `k=x" mid "y` | unchanged - neither end is a quote | git gives `x mid y` |
| `k=v` then `K=2` | the **first** wins | last for generic, an error for Desktop Entry and configparser, collected for git |
| `[a]` … `[A]` | one section for lookup, two in the tree, and a lookup does **not** search the second | every other dialect merges duplicate sections, which is GKeyFile's behaviour |
| a lone CR | ends a line | data everywhere but systemd and configparser |
| `[\xe9]` vs `[\xc9]` | two sections - folding is **ASCII only** | configparser folds through Python `str`, which a byte reader cannot follow |
| `k=one\` | the value `one\` - no continuation, no escapes, a backslash is data | git and systemd continue; configparser indents |

**This dialect refuses nothing**, and that is a property rather than an observation.
The key charset is open, the empty key and the empty section name are both
spellable, a line with no separator is a valueless entry, and an unclosed header is
an ordinary line - so no byte sequence is left to reject. The reference cannot report
an error either: the profile API has no way to say a file is malformed. It is the
only one of the seven that can make this claim, `tests/fuzz/fuzz_ini.cpp` asserts it
on every input, and the differential's `intent` score is the same assertion over 83
documents. That score caught two defects: `gtext_ini_canon_group()` was written for
git and refused every Win32 name outside `A-Za-z0-9-.`, and `gtext_ini_canon_key()`
refused the empty key - and a canonicalization failure is reported as
::GTEXT_INI_E_OOM two frames up, so twelve documents were refused with an allocation
failure.

Five fields exist because of this dialect and each is a rule no bool already held:
::GTEXT_INI_Dialect::allow_empty_key, ::GTEXT_INI_Dialect::trim_group_name,
::GTEXT_INI_Dialect::unclosed_header_is_line,
::GTEXT_INI_Dialect::strip_wrapping_quotes and
::GTEXT_INI_Dialect::merge_duplicate_groups. A sixth change is not a new field:
::GTEXT_INI_Dialect::valueless_keys is now live with an **open** key charset, which
its own documentation said was impossible. That reasoning was sound and was about the
code rather than about the format - `[a` has to become *something*, so the
open-charset branch takes the whole trimmed line as the key.

**What the writer had to learn.** The profile API writes CRLF, `key=value` with no
spaces, and trims a value it was given - measured, writing `"  pad  "` and reading it
back gives `pad`, so a round trip through the API is lossy in a way this module's is
not. On this side the consequence is narrower and sharper: all three line terminators
end a line here, so a synthesized value containing a lone CR cannot be written. It
was, until a unit test asked: the writer declared `a\rb` representable, emitted it
verbatim, and the document read back as two entries. Reachable only through
gtext_ini_group_set(), which neither gate exercises because both start from bytes -
the same blind spot that hid configparser's CR defect.

**One thing stays out**, and it is the registry redirection, which no reader of a
file can follow.

**UTF-16 was the second thing and is not any more.** The entry above said "a caller
transcodes, or asks for a decision", which was a decision deferred rather than made,
and what settled it was measuring what happened without one. A UTF-16LE `.ini` handed
to gtext_ini_parse() did not fail - it **succeeded**, producing one group whose name
was empty and five entries whose keys were the file's bytes with NULs between them.
Under this dialect that outcome was unconditional, because the dialect refuses
nothing, so no caller had any way to tell the nonsense from a reading. A gap you can
describe is a gap; a gap that returns a document is a defect.

So:

  - ::GTEXT_INI_E_ENCODING is what a document with a UTF-16 or UTF-32 byte-order mark
    gets by default, under **every** dialect. gtext_ini_detect_encoding() is public,
    because a caller holding one needs to know which it has.
  - GTEXT_INI_Parse_Options::decode_utf16 reads it instead: UTF-16LE and UTF-16BE are
    decoded to UTF-8 and parsed, and gtext_ini_document_source_encoding() reports what
    the file was. UTF-32 is refused whatever that option says - no decoder ships,
    because a Windows `.ini` is UTF-16 when it is not bytes and a UTF-32 one has never
    been observed.
  - **A decoded document does not rewrite byte-identically**, and cannot: the tree
    holds UTF-8 that never appeared on disk, so gtext_ini_write() emits UTF-8 with no
    mark. That is the arithmetic of the request rather than a defect - the caller asked
    for the text - and the accessor exists so a caller meaning to write UTF-16 back can
    re-encode.
  - Error offsets under a decode are offsets into a buffer the caller never sees, which
    is the real cost and the reason the option is opt-in rather than automatic.

**There is no content sniffing, and that is measured rather than chosen.** A BOM-less
UTF-16 document still reads as nonsense. The shape that would identify one - a NUL at
every odd offset - is real and is a guess, and the reference does not guess either:
given a UTF-16LE document with no mark, `GetPrivateProfileSectionNames` reports nothing
and every `GetPrivateProfileString` is absent, because the first line reads as a
section name beginning with a NUL and the API hands back C strings. A reader that
sniffed would be reading a document the reference does not read. With a mark the
reference reads the file and answers exactly as it does for the UTF-8 form, which is
what `make check-ini-win32-encoding-oracle` scores.

The one thing under this heading that is still only a decision: a UTF-16 document is
decoded and not re-encoded on the way out, so this module cannot *write* a UTF-16
`.ini`. Nothing has asked it to, and `WritePrivateProfileStringW` does not create one
either - measured: asked to author a file through the `W` entry points, the reference
wrote ANSI and transcoded the non-ASCII away.

## Deviations

| Case | This parser | Elsewhere |
|---|---|---|
| `; comment` | ::GTEXT_INI_E_BAD_LINE | Same in both references. Most other INI readers treat `;` as a comment |
| Duplicate group | ::GTEXT_INI_E_DUPGROUP | `GKeyFile` **merges them silently**, against §3.2. `desktop-file-validate` refuses |
| Duplicate key | ::GTEXT_INI_E_DUPKEY | `GKeyFile` takes the last **and lists the key twice** in `g_key_file_get_keys()`. The validator refuses |
| `k_und=v`, `k.dot=v`, `k space=v`, a non-ASCII key | ::GTEXT_INI_E_BAD_KEY | `GKeyFile` accepts all four. §3.3 and the validator refuse them |
| A non-ASCII group name | ::GTEXT_INI_E_BAD_GROUP | `GKeyFile` accepts it. §3.2 says "all ASCII characters" |
| An orphan `key[de]` | ::GTEXT_INI_E_BAD_KEY | `GKeyFile` accepts it; only the validator refuses |
| An indented line | Refused | `GKeyFile` accepts it; the validator reports it and continues |
| CRLF | Refused at the first header | `GKeyFile` accepts CRLF everywhere and strips the CR |
| `[]` | ::GTEXT_INI_E_BAD_GROUP | `GKeyFile` refuses it too ("Invalid group name: "). §3.2's rule would admit it; the reference settles what the specification leaves open |
| A NUL inside a value | **Kept.** The value is `a\0b`, three bytes | `GKeyFile` **truncates at the NUL**, because its strings are NUL-terminated. This module is length-based throughout |
| Invalid UTF-8 | Parses; gtext_ini_unescape() refuses | `GKeyFile` the same. `desktop-file-validate` exits **0** with a warning that it may not work correctly |
| `\q` in a value | Parses; gtext_ini_unescape() refuses | `GKeyFile` the same. The validator accepts it. **Neither reference answers a document with an unknown escape**, so the conformance gate excludes and counts those rather than scoring them |

The pattern worth naming: **`GKeyFile` is the reference for what a value *is*,
and `desktop-file-validate` is the reference for whether a document is *legal*.**
Neither alone is a second reader, and a differential built on `GKeyFile` would
agree with it about duplicate groups and score clean while violating §3.2.

## Tested scope

**`make conformance-ini-desktop-entry`** - every `.desktop` file on this
machine, four scores: it parses, it writes back byte-identically, the generic
dialect reads it, and the generic dialect produces the same tree. Measured
2026-09-28: **202 of 202 on all four**. The corpus is neither fetched nor
pinned, because no Desktop Entry test suite exists to fetch; the denominator is
derived at run time and printed, and an **empty corpus fails** rather than
scoring a vacuous 0 of 0.

**`tests/test-ini.cpp`** - 48 tests. Every refusal in the checklist above, the
escape set in both directions with a round trip over eight subjects, the list
terminator rule, the locale fallback chain, the duplicate-key storage shape, the
byte-identical rewrite of a document with deliberately awkward spacing, and the
generic dialect's parity over eleven shapes the corpus does not contain.

**Where each reach ends, measured rather than assumed.** Four mutations were
planted and the gate that caught each one recorded:

| Mutation | Corpus gate | Unit tests |
|---|---|---|
| Comments are not accumulated | **round trip 70 of 202** | caught |
| The generic dialect trims trailing whitespace | **parity 184 of 202** | caught |
| The strict dialect trims trailing whitespace | *clean* | **caught** |
| A header with trailing whitespace is refused | *clean* | **caught** |

The last two are the honest limit of the corpus, and the reason is countable:
**no file in the corpus has anything after its `]`**, and although 25 of them
have a value with trailing whitespace, trimming it in *both* dialects leaves the
round trip byte-exact because the trimmed run is still written back. So the
corpus scores acceptance and preservation and cannot score those two rules; the
unit tests can, and do. A corpus grown from real files flatters a parser exactly
where the real files are uniform.

**`make check-ini-oracle`** is the gate the corpus cannot be: generated
documents, against **both references in one pinned image, over the same bytes in
one pass**. It exists because every real file on this machine is already valid,
so the corpus reaches no refusal at all - no duplicate key, no `;` comment, no
BOM, no CRLF, no non-ASCII name.

Four scores, each with its own denominator, plus the generator's own intent as a
third reading that separates "the subject is wrong" from "the generator emitted
something other than what it thinks":

| Score | What it compares |
|---|---|
| `intent` | our verdict is the one the generator meant to produce |
| `legality` | our accept/reject matches `desktop-file-validate`'s syntax verdict |
| `values` | for documents both accept, the groups, keys and raw values match `GKeyFile` |
| `strings` | gtext_ini_unescape() agrees with `g_key_file_get_string()` |

Measured, 20,000 documents at seed 20260928:

| Score | | Excluded |
|---|---:|---:|
| `intent` | **20,000 of 20,000** | - |
| `legality` | **8,365 of 8,365** | 11,635 |
| `values` | **9,776 of 9,776** | 1,301 |
| `strings` | **7,172 of 7,172** | 3,905 |

The `legality` exclusions are the four stricter-validator axes, by axis:
whitespace-only line 3,019, a space after `]` 2,427, invalid UTF-8 2,355, a CR
inside a value 2,319. More is excluded there than scored, which is why the
control on the exclusion table matters and is in the mutation table below.

**47 axes, every one required to appear in the run.** An axis the generator stops
emitting fails the gate rather than quietly shrinking the population - which is
exactly how the corpus came to be blind to four of the rules above.

**The exclusions are counted and named, never silent.** Two axes have no oracle at
all: a document with an unknown escape, or with a trailing lone backslash, is
parsed by `GKeyFile` and refused only at `get_string()` while the validator
accepts it outright, so nothing outside this repository decides it. A NUL in a
value is excluded from `values` because `GKeyFile` truncates there by
construction. And a document carrying one of four axes where **the validator is
stricter than both `GKeyFile` and this module** - a whitespace-only line, a space
after `]`, a CR anywhere, or non-UTF-8 - is excluded from `legality`, because
scoring it either way would be wrong: as a failure it would assert a rule this
module has decided not to implement, and as a pass it would hide a real
difference. In all four the validator's own message ends "The validation will
continue", so it is a complaint about a document it can still read. That table
lives in `tools/oracle/ini_diff.py`, by name, with the reason for each.

**Every score was seen to fail**, and each by the comparison meant for it:

| Mutation | intent | legality | values | strings | axes |
|---|---|---|---|---|---|
| `;` accepted as a comment | **982 / 1000** | **407 / 417** | - | - | - |
| duplicate keys allowed | **981 / 1000** | **409 / 417** | **511 / 529** | - | - |
| trailing whitespace trimmed | - | - | **445 / 511** | **323 / 372** | - |
| an undefined escape accepted | - | - | - | **315 / 372** | - |
| the generator drops one axis | - | - | - | - | **FAIL** |
| one exclusion removed | - | **441 / 481** | - | - | - |

The third row is the point of the whole gate: trimming trailing whitespace in the
strict dialect is the mutation the **corpus structurally cannot see**, and here it
moves two scores. The last row is the control on the exclusions themselves -
removing one makes the excluded count drop and the comparison fail, so the
exclusions are a knowing-difference set rather than a place failures go to hide.

**`make fuzz-ini`** asserts five properties beyond "it did not crash", and the
first of them - that the strict dialect is a subset of the generic one, since a
relaxation may only add accepted documents - **found a defect at 237,647
executions on its first run**. A trailing CR with no linefeed after it was
stripped as though it were a CRLF terminator, so the generic dialect produced a
value one byte shorter than the strict dialect's. Neither the corpus nor the
round trip could see it: the byte still came back in the line's trailing run, so
the document rewrote byte for byte and scored clean. Fixing it drew out a second
defect in the opposite direction - the writer refused *every* value containing a
CR, which made a document this module reads unwritable - so
`ini_value_writable()` now takes the byte that will follow the value and refuses
only the values that would actually read back differently. Reproducer:
`tests/fuzz/corpus/ini/trailing-cr-with-no-linefeed.ini.seed`.

It then found something larger: **`GTEXT_INI_Dialect::accept_crlf` is not a
relaxation**, so the claim that every document the strict dialect accepts reads
identically under the generic one was false, and had been written into six
places. Six of the generic dialect's seven changes only widen acceptance; that
one removes a CR from the content of a line the strict dialect already accepted.
The corpus could not contradict it because this machine has **zero** CRLF
`.desktop` files. The claim now carries its proviso and the gate excludes and
counts a CR-bearing file.

**`make fuzz-ini-writer`** asks the other half of the question. `fuzz_ini.cpp`
reaches `gtext_ini_write()` only with documents a parse produced, and a parse
has already refused everything the writer would have to refuse - so the group
names and keys reachable only through `gtext_ini_new()`,
`gtext_ini_document_add_group()` and `gtext_ini_group_set()` were reached by
nothing. A name holding `]`, a newline, a NUL or bytes that are not UTF-8, and
a value holding a line break, are what that harness builds, across all seven
dialects, and it asserts the round trip **only for what the DOM accepted**.
That is deliberate: the DOM API's own refusals are the other half of what is
under test, since a name the writer could not round-trip should be refused on
the way in rather than written and lost, and the harness fails either way
round.

**5.3M executions clean** since those fixes, in two runs. One earlier 600-second
run ended at 3.26M with an ASan "nested bug" abort that produced no error type,
no stack and no artifact; it has not recurred and its cause is unknown. It is
recorded in `tests/fuzz/README.md` rather than explained away, and the harness now
names its own failing property so a recurrence identifies itself.

**`make conformance-ini-win32`** scores this machine's `.ini` and `.cfg` files against
wine's profile API. Measured 2026-09-29: **701 files**, and every score clean -
`intent` 701/701 (this dialect refuses nothing), `names` 701/701, `sections`
5,371/5,371, `values` **118,790/118,790**, `lookup` **214,774/214,774**, `rewrite`
701/701.

**The corpus is real bytes of the right shape from the wrong provenance**, and the gate
prints that with the number. This machine has exactly **two** `.ini` files a Windows
application wrote, both inside a wine prefix; the 701 belong to freedesktop, Python and
other tools. The profile API reads any of them, so they are a valid population for "do
we agree with the reference about real bytes" and no population at all for "is this
format used this way". **It found nothing**, on its first run and every run since - which
is worth saying plainly, because for configparser the corpus found the cheapest of three
defects and here it found none. That is the honest measure of what a weaker population
buys.

**`make check-ini-win32-authored-oracle` is what can be done about that provenance**,
and it is the only population here whose provenance is right by construction. Files a
Windows application wrote cannot be conjured on this host. Files *the profile API*
wrote can: `WritePrivateProfileString` is the other half of the same reference, so the
gate asks the container to author 22 files and scores our reader against the reference's
reading of them. Measured 2026-09-30: `intent` 22/22, `names` 22/22, `sections` 25/25,
`values` **38/38**, `lookup` **35/35**, `rewrite` 22/22.

It is also the only gate that exercises the reference as a **writer**; every other ask
in this module is about what a lookup returns. What its first run confirmed was already
known - the whitespace trim recorded two paragraphs above, measured earlier by a one-off
probe - so it discovered nothing, and the gain is that the rule now sits under a gate
instead of in a sentence nothing would contradict. A wine that stopped trimming would
fail here.

A control was applied before the score was believed, because a gate green on its first
run has not been seen to fail: disabling the quote strip took `values` to 36/38 and
`lookup` to 33/35, naming `quoted` both times.

**`make check-ini-win32-encoding-oracle`** scores the UTF-16 decode, over each generated
document re-encoded as UTF-16LE and UTF-16BE with a mark. Measured 2026-09-30: 162
documents, `intent` 162/162, `names` 162/162, `sections` 168/168, `values` 182/182,
`lookup` **190/190**, `parity` 162/162; 4 generated documents excluded for holding a
non-ASCII byte, and 6 asks excluded for a NUL.

The `parity` score exists because of this gate's own control, and is the more useful
half of it. Breaking the byte order took `names` to 90/162, which is the gate working -
and `values` read 171/171 and `lookup` 173/173 at the same moment, both green, because a
broken decode produced fewer groups and therefore fewer asks. **Every denominator here
is derived from our own tree, so a defect that makes the tree smaller shrinks the
population that would have caught it.** `parity` compares the decoded tree against the
same document read the only other way it can be read, one row per document, with a
denominator that cannot move: it went to 82/162 under the same control.

The ASCII-only population is stated with the number rather than left implicit: the A
entry points transcode to the host code page, so a document holding U+00A5 comes back as
`A5` from the UTF-16 file and `C2 A5` from the UTF-8 one. That is the channel narrowing
rather than a disagreement about the file, so those documents are counted out of the
score instead of quietly dropped.

**`make check-ini-win32-oracle`** is the differential, and it is shaped by the reference
having **three entry points of which two disagree**. 85 axes, one construct per
document (and the corpus gate above runs the same comparison over real files, with
`--corpus`, rather than a second copy of it):

| Score | | Excluded |
|---|---:|---:|
| `intent` - every document parses, because this dialect refuses nothing | **85 / 85** | - |
| `names` - `GetPrivateProfileSectionNames` against our section list | **85 / 85** | - |
| `sections` - `GetPrivateProfileSection` against our entries, raw values | **88 / 88** | - |
| `values` - `GetPrivateProfileString` against our decoded values | **91 / 91** | - |
| `lookup` - the same, asked through gtext_ini_document_get() | **99 / 99** | - |
| `rewrite` - an accepted document writes back byte for byte | **85 / 85** | - |
| `divergence` - each stated departure is still observable | **4 / 4** | 1 not observable |
| the reference returns C strings and cannot express a NUL | - | 3 |

**`intent` means something different here than in any other INI gate.** Elsewhere it is
our verdict against the generator's reading of a specification. Here the claim is that
*there is no document to refuse*, and it is checked as such - a refusal is a defect even
though no reference can report one. It earned its place immediately, catching two
defects at once: `gtext_ini_canon_group()` had been written for git and refused every
Win32 name outside `A-Za-z0-9-.`, and `gtext_ini_canon_key()` refused the empty key -
and because `ini_add_group()` reports a canonicalization failure as
::GTEXT_INI_E_OOM, twelve documents were refused with an out-of-memory error.

**`lookup` exists because the other scores cannot see a lookup.** `names`, `sections`
and `values` all walk the tree by index and resolve first-wins in the harness, so none
of them reaches gtext_ini_document_get() - and a differential that compares a tree
cannot find a defect in an accessor. Adding it found **three**, all of them in
`ini_group_matches()` and all invisible to every other score:

- git's `section.subsection` rule - fold up to the first `.`, compare the rest byte for
  byte - was applied to **every** folding dialect, because git was the only one when it
  was written. A Win32 lookup for `[Foo.Bar]` spelled as the file spells it found
  nothing, while `foo.bar` worked. A dotted section name is ordinary in a real `.ini`.
- The query was not trimmed where the stored name had been, so `[ b ]` was findable as
  `b` and not as `" b "` - again, the spelling a caller who copied the name out of the
  file would have.
- The empty name found the first group carrying it, where the reference resolves it to
  the **preamble** specifically: `[]` with no preamble answers nothing.

The first of those three had also escaped the harness for a reason worth naming: both
gates asked the reference using the **canonical** name, so no query they generated ever
carried an upper-case letter after a dot. A harness that normalizes its own input cannot
test a normalization.

**The divergence score asserts a disagreement rather than an agreement**, which the
reference makes necessary: four axes where `GetPrivateProfileString` retrieves something
this reader does not, three of them `;` lines and one a value truncated at a NUL. A wine
that began treating `;` as a comment would fail here instead of quietly making `values`
look better. A fifth axis is in the table for its reasoning and predicts no observable
difference - a `;` line with no `=` is dropped by both APIs and by this reader, for two
unrelated reasons - and is counted separately rather than forgiven.

**And the fuzzer found a seventh defect, which belongs to the generic dialect.** A key
whose own first bytes are a UTF-8 BOM: `skip_bom` strips a BOM only at offset 0, so
` <BOM>j=v` - a blank, then the BOM - is an entry whose key really *is* `<BOM>j`, and
that reading is correct. A **normalized** write then drops the leading blank, the key
lands at offset 0, and a reader strips it there, so the document comes back with a
different key or with none at all. It is refused now, in normalize mode only; the
verbatim write keeps the blank and round-trips, which is why the default path is
untouched.

The generic dialect has had that defect for as long as it has had `skip_bom`, and
570,000 fuzz executions across five and six dialects never reached it. What changed is
not the property but the **cost of the path**: under a dialect where a line needs no
separator to be an entry, the whole reproducer is four bytes. A seventh arm made an old
defect in the second arm cheap to find.

**Where each reach ends, measured.** 28 mutations, every one caught, control clean:

| Mutation | unit | oracle | corpus |
|---|:-:|:-:|:-:|
| `;` is not a comment | ✓ | ✓ | ✓ |
| `#` **is** a comment | ✓ | ✓ | ✓ |
| the group name is not trimmed | ✓ | ✓ | |
| an unclosed header is an error | ✓ | ✓ | |
| quotes are not stripped | ✓ | ✓ | ✓ |
| the empty key is refused | ✓ | ✓ | |
| a lookup merges duplicate groups | ✓ | | |
| a lone CR is data | ✓ | ✓ | |
| the whitespace set is blank-only | ✓ | ✓ | |
| trailing space is kept | ✓ | ✓ | ✓ |
| the last duplicate key wins | ✓ | | |
| keys do not fold | ✓ | | |
| group names do not fold | ✓ | ✓ | |
| the BOM is not skipped | ✓ | ✓ | |
| the empty group name is refused | ✓ | ✓ | |
| valueless keys are refused | ✓ | ✓ | ✓ |
| UTF-8 validation is on | ✓ | ✓ | |
| a header remainder is an error | ✓ | ✓ | |
| Desktop Entry's escape set is inherited | ✓ | ✓ | ✓ |
| the locale postfix is on | ✓ | | |
| the header closes at the **first** `]` | ✓ | ✓ | |
| the quote strip accepts a single byte | ✓ | ✓ | ✓ |
| the quote strip ignores which quote | ✓ | ✓ | ✓ |
| the group name trims leading only | ✓ | ✓ | |
| the writer allows a synthesized CR | ✓ | | |
| a valueless key is not trimmed | ✓ | ✓ | ✓ |
| `canon_key` refuses the empty key | ✓ | ✓ | |
| `canon_group` uses git's charset | ✓ | ✓ | ✓ |

**Six of the 28 are caught by the unit tests alone**, and they are the same shape in
every case: a rule about a *lookup* or about a *synthesized* value. Both gates start
from bytes and ask what they mean, so neither can reach gtext_ini_group_set() or - before
the `lookup` score existed - gtext_ini_document_get(). That is not a gap to close by
adding documents; it is what a unit test is for.

**The harness failed its own control twice**, and both failures were the restore step
rather than a mutation. `shutil.copy2` preserves the mtime, so a restored source was
older than the object built from the mutated one and `make` rebuilt nothing - every gate
scored the previous mutation's binary. Fixing that, the control failed again, because
the control runs *before* the first restore and inherited stale artifacts from the
aborted run. Both sweeps are void; the third is the one above. A control that cannot
fail proves nothing, and this one failed twice and was right both times.

## Not implemented

**Every named dialect is now implemented.** Desktop Entry, generic, git config,
EditorConfig, systemd, configparser and Win32: seven, of which one has a normative
conformance suite, three have a normative document, one is a written derivation of
another, one has no specification at all, and one has no specification *and* no
reference that is the thing it stands for. What is left out below is a *feature* of
one of them.

**Win32 `.ini` was on this list and is not any more**, and the correction is worth
keeping because the reasoning was sound and answered a question nobody asked.
`GetPrivateProfileString` consults the registry before the file, so the API's answer
is not a function of the file's bytes - true, and it disqualifies only the claim to
reproduce that API. Reading the `.ini` files that exist on Windows is a different
thing, it is what the format is for, and it was reachable the whole time. See the
dialect's own section above.

What the systemd work needed, recorded because the estimate was wrong twice:

  - **A continuation that joins with a space** and skips an intervening comment block.
    Predicted, and correct. ::GTEXT_INI_Continuation_Mode had two members rather than
    four because a constant nothing reads is worse than an absent one, and
    `GTEXT_INI_CONTINUATION_JOIN_SPACE` arrived with the code that implements it.
  - **Variable-length escapes** with multi-byte output. Predicted, and correct - though
    it turned out to need a *field* as well as a mechanism, because
    ::GTEXT_INI_Dialect::escapes is a set of letters and cannot hold `\xHH`.
  - **A words accessor**, gtext_ini_value_words(), and a dialect parameter on
    gtext_ini_value_bool() - which was the only accessor in the value layer without
    one. Predicted, and correct.

  **First wrong prediction:** the comment-block skip was called "the single nastiest
  rule in any of these dialects" before any of it was built. It is not. The skipped
  lines sit inside the value's contiguous extent and are discarded while decoding,
  exactly as git's backslash-plus-newline bytes already are, so the storage that
  shipped for git holds it unchanged.

  **Second wrong prediction, and the one that mattered:** the continuation was taken to
  be a property of the *value*, as git's is. It is a property of the **line** - it works
  in a group header and in a key - so it reached the tree's storage, where a name became
  the document's bytes with the joined form beside it as the canonical one. No reading of
  `systemd.syntax(7)` suggests that; it took a probe.

What the configparser work needed, recorded the same way, because the predictions were
wrong in the opposite direction from systemd's - the obstacle named in advance turned
out not to be one, and the thing that reached the parser was not on the list:

  - **Interpolation was named as the reason this dialect had not been done**, and it is
    not an obstacle at all: it is a pass over an assembled value, it ships in no form,
    and the measurement that settles it is that the *default* interpolation refuses a
    value in 301 of the 479 real documents on this machine and changes one in none. The
    earlier reading of the same question, over eleven files, said "three" - correct over
    that population and far too weak to decide anything.
  - **The continuation reached the parser rather than the value layer**, which is the
    mirror image of systemd's surprise. There the continuation turned out to be a
    property of the line when it had been taken for a property of the value; here it is
    a property of the *following* line, which is neither - so the value scanner cannot
    find it and a key's separator has to be sought on its own physical line only.
  - **Three of the four struct changes were axes no prediction named**: two separator
    characters, a third whitespace set, and a third answer for what follows a `]`. Each
    was found by probing rather than by reading, and none of them is about the
    continuation everybody had noticed.
  - **The cheapest finding came from the corpus, not the differential.** A value
    beginning with `;` is not a comment - the reference tests the whole line - and 331
    of the 479 real documents have one, while none of the 102 probe documents and none
    of the generator's axes did. It made those values come back empty.
- **Win32 `.ini`, again.** Deliberately absent, and not because its reference is out
  of reach - a `GetPrivateProfileStringA` probe under wine runs in this workspace
  today. `GetPrivateProfileString` is documented as consulting the registry's
  `IniFileMapping` for the section and reading the file only *"if there is no
  subkey or entry for the section name"*, so its answer is not a function of the
  file's bytes and there is nothing for a text library to conform to. Two
  machines with the same file can correctly return different values.
- **Interpolation.** Absent, and measured - twice, because the first measurement was
  taken over the wrong population. Over the **479 real `configparser` documents** on
  this machine the default `BasicInterpolation` refuses a value in **301** of them and
  changes a value in **none**; five contain `%(` or `${` and not one is an
  interpolation, the `${prefix}` in numpy's `npymath.ini` being pkg-config's own
  variable syntax. So the only measurable effect of shipping the default form is to
  break 63% of the files here. The earlier figure - "zero use it, three contain a bare
  `%`" - was taken over eleven files and is correct over those eleven; it was far too
  small a population to decide the question, and it happened to point the same way.
  systemd's `%` specifiers are a separate thing and need the unit name and the host,
  which a text parser does not have.
- **A streaming or incremental reader.** The document is read whole. An INI file
  is flat and line-oriented, so a pull reader would be a small piece of work; no
  caller has needed one.
- **A caller allocator for the writer.** The reader and the accessors take one;
  the buffer sink deliberately does not, for the reason gtext_ini_sink_buffer()
  gives. See \ref format_allocator_todo "Caller allocators".
