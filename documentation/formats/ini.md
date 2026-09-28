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
3. Entries may appear before the first group header (still
   ::GTEXT_INI_E_NO_GROUP today - see *Not implemented*).
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

**5.3M executions clean** since those fixes, in two runs. One earlier 600-second
run ended at 3.26M with an ASan "nested bug" abort that produced no error type,
no stack and no artifact; it has not recurred and its cause is unknown. It is
recorded in `tests/fuzz/README.md` rather than explained away, and the harness now
names its own failing property so a recurrence identifies itself.

## Not implemented

- **The other dialects.** `systemd.syntax(7)`, `git-config(1)` and EditorConfig
  each have a specification, a reference implementation and a corpus, and
  `notes/text/INI-DIALECTS.md` has the axis table and the plan. What each needs
  that this one did not: systemd's continuation rule, where a trailing backslash
  becomes a space and an intervening comment block is skipped; git's
  multi-valued keys and its two escape layers; EditorConfig's preamble and its
  34-assertion conformance suite.
- **A preamble.** GTEXT_INI_Dialect::allow_preamble exists and entries before
  the first group are still ::GTEXT_INI_E_NO_GROUP, because a preamble entry
  needs somewhere to live and inventing an unnamed group would put a group in
  the tree the document does not have. EditorConfig is the dialect that needs
  it, and it should arrive with the shape that dialect wants rather than with a
  guess.
- **Win32 `.ini`.** Deliberately absent, and not because its reference is out of
  reach - a `GetPrivateProfileStringA` probe under wine runs in this workspace
  today. `GetPrivateProfileString` is documented as consulting the registry's
  `IniFileMapping` for the section and reading the file only *"if there is no
  subkey or entry for the section name"*, so its answer is not a function of the
  file's bytes and there is nothing for a text library to conform to. Two
  machines with the same file can correctly return different values.
- **Interpolation.** Absent, and measured rather than assumed: across every
  INI-shaped file on this machine, **zero** use `%(name)s` or `${section:key}`,
  and three contain a bare `%` that `configparser`'s default interpolation
  *refuses*. systemd's `%` specifiers need the unit name and the host, which a
  text parser does not have.
- **A streaming or incremental reader.** The document is read whole. An INI file
  is flat and line-oriented, so a pull reader would be a small piece of work; no
  caller has needed one.
- **A caller allocator for the writer.** The reader and the accessors take one;
  the buffer sink deliberately does not, for the reason gtext_ini_sink_buffer()
  gives. See \ref format_allocator_todo "Caller allocators".
