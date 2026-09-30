@page ini_module INI

# INI

The INI module reads a
[Desktop Entry](https://specifications.freedesktop.org/desktop-entry-spec/latest/)
file into a tree, gives you accessors for it, lets you build one from nothing,
and writes one back out **byte for byte**.

It is the one module here whose format is chosen by the caller rather than named
by the module. There is no INI specification, so `GTEXT_INI_Dialect` names which
INI - seven of them: Desktop Entry, a generic derivation of it, git config,
EditorConfig, systemd, Python's configparser and the Win32 profile API - and the
default is Desktop Entry 1.5. EditorConfig is the only one with a **normative
conformance suite**, and the only one where this module scores higher than either
reference implementation does; configparser is the only one with **no specification at
all**, so every rule of it is a measurement rather than a citation; Win32 has no
specification *and* no reference that is the thing it stands for - wine is a
reimplementation of the Windows API - so it is the weakest-grounded of the seven and
its gate prints that with every run.

---

@note This page documents the **API**. For the specification-level view - which
clauses are implemented, every rejection with its status code, the twelve places
this parser deviates from `GKeyFile` or from `desktop-file-validate`, the corpus
score and what nothing checks yet - see \ref format_ini "INI (Desktop Entry)"
under \ref text_format_references "Format and specification references".

## 1. Overview

```c
#include <ghoti.io/text/ini.h>

GTEXT_INI_Error err;
GTEXT_INI_Document * doc = gtext_ini_parse_file("app.desktop", NULL, &err);
if (!doc) {
  fprintf(stderr, "%s at line %d\n", err.message, err.line);
  gtext_ini_error_free(&err);
  return 1;
}

size_t len = 0;
const char * raw = gtext_ini_document_get(doc, "Desktop Entry", "Exec", &len);

gtext_ini_free(doc);
```

`NULL` options are gtext_ini_parse_options_default(): the Desktop Entry dialect,
the default allocator, no limits, and **comments kept** - the one default that
differs from the TOML module's, because §3 of the specification requires a
rewrite to preserve them.

## 2. The value you get back is raw

This is the thing to understand before anything else. gtext_ini_document_get()
and gtext_ini_group_value_at() return **the bytes between the delimiter and the
end of the line**, with the leading run of spaces after `=` removed and nothing
else done to them. `Name=a\nb` gives you the five characters `a\nb` - a
backslash and an `n`, not a newline.

That is not a shortcut. It is where both reference implementations put the line:
`g_key_file_load_from_data()` accepts a document containing `k=a\qb`,
`g_key_file_get_value()` hands back `a\qb`, and only `g_key_file_get_string()`
fails. Decoding during the parse would also destroy the original spelling, and
§3 requires that a rewrite preserve what it did not understand.

So to get a usable value, ask for one:

```c
GTEXT_INI_Dialect dialect = gtext_ini_dialect_desktop_entry();
char * text = NULL;
size_t text_len = 0;
if (gtext_ini_unescape(&dialect, raw, len, NULL, &text, &text_len)
    == GTEXT_INI_OK) {
  puts(text);
  gtext_ini_string_free(NULL, text);
}
```

| Want | Call | Refuses with |
|---|---|---|
| a string | gtext_ini_unescape() | ::GTEXT_INI_E_BAD_ESCAPE, ::GTEXT_INI_E_BAD_UNICODE |
| a `;`-separated list | gtext_ini_value_list() | ::GTEXT_INI_E_BAD_ESCAPE |
| a boolean | gtext_ini_value_bool() | ::GTEXT_INI_E_TYPE |
| an integer | gtext_ini_value_int() | ::GTEXT_INI_E_TYPE, ::GTEXT_INI_E_RANGE |
| a double | gtext_ini_value_double() | ::GTEXT_INI_E_TYPE, ::GTEXT_INI_E_RANGE |
| the localized string | gtext_ini_group_get_locale() | returns `NULL` if even the bare key is absent |

gtext_ini_value_double() reads `%f` **in the C locale** whatever `LC_NUMERIC`
says, because §4 defines the type that way: `1.5` is one and a half in a
comma-decimal locale, and `1,5` is not a number in any.

## 3. Walking a document

```c
for (size_t g = 0; g < gtext_ini_document_group_count(doc); g++) {
  const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
  size_t name_len = 0;
  const char * name = gtext_ini_group_name(group, &name_len);
  printf("[%.*s]\n", (int) name_len, name);
  for (size_t e = 0; e < gtext_ini_group_entry_count(group); e++) {
    size_t klen = 0;
    size_t vlen = 0;
    const char * key = gtext_ini_group_key_at(group, e, &klen);
    const char * value = gtext_ini_group_value_at(group, e, &vlen);
    printf("  %.*s = %.*s\n", (int) klen, key, (int) vlen, value);
  }
}
```

Two things the tree does that a hash map would not:

- **Keys keep their `[LOCALE]` postfix.** A file with `Name[de]=x` has a key
  literally named `Name[de]`, which is what `g_key_file_get_keys()` reports too.
  gtext_ini_group_get_locale() does the §5 match; the tree holds the spelling.
- **Duplicates are stored, not resolved.** Every occurrence is in the tree in
  document order under every ::GTEXT_INI_Dupkey_Mode. The mode decides what a
  *lookup* answers, and gtext_ini_group_count_key() with
  gtext_ini_group_get_nth() reaches the rest. The Desktop Entry dialect refuses
  a duplicate outright, so this matters only under a dialect that allows one -
  and it is in the type now because git config and systemd will both need it.

gtext_ini_document_get() searches **every** group of the given name, which is
how `GKeyFile` behaves on a file with a repeated header. The tree keeps the two
groups apart so a rewrite reproduces the document; the merge lives in the
lookup.

## 4. Dialects

```c
GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
opts.dialect = gtext_ini_dialect_generic();
```

gtext_ini_dialect_generic() is Desktop Entry with **six relaxations and one
normalisation**. The six relaxations - `;` comments, indented lines, a preamble,
duplicate groups, duplicate keys, any key charset - only widen what is accepted,
so they carry a free property: every document the strict dialect accepts parses
identically under both, gated over the whole corpus.

The normalisation is CRLF (and a skipped leading BOM), and it is called out
separately because it **changes a value rather than widening acceptance**: the
strict dialect reads `Exec=/bin/true` followed by CRLF as `/bin/true\r`, since §3
makes the CR part of the line, and the generic dialect reads `/bin/true`. So the
inherited property holds over documents containing no CR, and the gates say so.

The escape set and the list separator are untouched: a change to how a value is
*decoded* would not be a relaxation either.

### git config

```c
opts.dialect = gtext_ini_dialect_git_config();
```

`git-config(1)`'s "Syntax", and **not** a relaxation of Desktop Entry in either
direction - which is why it is its own constructor rather than a derivation. It
accepts a preamble, comments anywhere on a line, a backslash continuation, quoted
runs inside a value, subsections in both spellings, valueless keys, repeated keys
and an entry after the `]`; and it *refuses* a key that does not begin with a
letter and a group name outside `A-Za-z0-9-.`, both of which Desktop Entry allows.

Three things a caller meets that the other two dialects do not:

```c
/* A valueless key is not an empty value. `k` and `k =` are both legal, and
 * gtext_ini_group_get() returns NULL for both a missing key and a valueless one -
 * so the predicate is what tells them apart. */
size_t index = gtext_ini_group_find(group, "bare", 0);
if (index != SIZE_MAX && !gtext_ini_group_value_present_at(group, index)) {
  /* git's shorthand for boolean true. */
}

/* A name has a spelling and a canonical form. The tree keeps what the document
 * wrote, so a rewrite is byte-identical; lookups use the canonical form. */
size_t len = 0;
const char * canonical = gtext_ini_group_canonical_name(group, &len);
/* `[Remote "orig in"]` -> name `Remote "orig in"`, canonical `remote.orig in` */

/* Repeated keys are a list. gtext_ini_group_get() answers the last, as
 * `git config --get` does; count_key and get_nth are `--get-all`. */
size_t n = gtext_ini_group_count_key(group, "url");
```

The **section** part of a name folds and a **quoted** subsection does not, so
`[a "SubB"]` is reachable only as `a.SubB` and `[a.SubB]` only as `a.subb` - git's
own rule, and measured both ways.

`make check-ini-git-oracle` compares the reader against git itself over 20,000
generated documents; \ref format_ini "the format page" has the figures and the
mutations that were seen to move them.

### EditorConfig

```c
opts.dialect = gtext_ini_dialect_editorconfig();
```

Specification 0.17.2, and the simplest grammar of the four: no escapes, no
quoting, no continuation, no list, no inline comments. Two rules a caller is most
likely to be surprised by, both of which its conformance suite asserts:

```c
/* The line is trimmed **before** it is classified, so an indented header is a
 * header and an indented comment is a comment. core-py gets the second wrong. */

/* A section name may hold any byte, so the header closes at the LAST `]`. */
GTEXT_INI_Document * doc = gtext_ini_parse("[a]b]\nk=v\n", &opts, NULL);
/* one group, named `a]b` - the same bytes are two things under git */

/* Keys are case-insensitive and section names are not: the canonical key is
 * lower-cased and the group name is compared byte for byte. */
size_t len = 0;
const char * key = gtext_ini_group_canonical_key_at(group, 0, &len);
```

What this dialect does **not** do is resolve properties for a filepath. The
section name is a glob, and matching one is a separate job from reading the
document - 130 of the suite's 202 assertions are about that matcher, and they are
out of scope. gtext_ini_document_group_at() in order plus your own matcher is the
whole of it; `tools/conformance/editorconfig_suite.py` is a worked example.

`make conformance-ini-editorconfig` scores the suite's 34 grammar assertions -
**34 of 34, where both reference cores score 33** - and
`make check-ini-editorconfig-oracle` differs the reader against both of them over
20,000 generated documents. \ref format_ini "The format page" has the figures, the
twenty places a core departs from its own specification, and the mutations that
were seen to move a score.

### systemd

```c
opts.dialect = gtext_ini_dialect_systemd();
```

`systemd.syntax(7)`, and the widest grammar of the five. Three things a caller meets
that no other dialect here has:

```c
/* A continuation joins with a **space**, and it can appear in a name as well as in a
 * value - so a name is the document's bytes and the joined form is canonical. */
GTEXT_INI_Document * doc = gtext_ini_parse("[Serv\\\nice]\nA=1\n", &opts, NULL);
const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
size_t len = 0;
gtext_ini_group_name(group, &len);            /* `Serv\<LF>ice` - what was written */
gtext_ini_group_canonical_name(group, &len);  /* `Serv ice`      - what it means */

/* Quoting and escaping are **per setting**, not part of the grammar - the
 * specification says so - so the parser stores them and a caller asks. */
GTEXT_INI_List * words = NULL;
gtext_ini_value_words(&dialect, raw, raw_len, NULL, &words);
/* `"x" 'y' z` is three words; escapes are decoded inside single quotes too. */

/* And the booleans are the wider set. This is the accessor that gained a dialect
 * parameter when this dialect arrived, being the only one in the value layer without
 * one. */
bool flag = false;
gtext_ini_value_bool(&dialect, "yes", 3, &flag);
```

**A malformed line is refused here and skipped by systemd**, which is a deliberate
departure: systemd warns and keeps the rest of the file, and a library whose caller
cannot see a warning must not silently drop a setting. \ref format_ini
"The format page" says so where the number is quoted.

`make conformance-ini-systemd` reads every unit file on this machine - **165 of 165**
parse and write back byte for byte - and `make check-ini-systemd-oracle` differs
against systemd itself over 5,000 generated documents.

### Python configparser

```c
GTEXT_INI_Dialect dialect = gtext_ini_dialect_configparser();
GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
opts.dialect = dialect;
GTEXT_INI_Document * doc = gtext_ini_parse(text, len, &opts, &err);

/* `=` or `:`, whichever comes first, so `opt : b=c` is the key `opt` and the value
 * `b=c`. */

/* A value can span lines, and the **joined** form is what a caller wants: the raw
 * value holds the terminators and the indentation, and gtext_ini_unescape() performs
 * the join - the same two-layer shape systemd's escapes have. `k = 1` followed by an
 * indented `2` joins to "1\n2". */
char * joined = NULL;
size_t joined_len = 0;
gtext_ini_unescape(&dialect, raw, raw_len, NULL, &joined, &joined_len);
gtext_ini_string_free(NULL, joined);

/* Booleans are systemd's eight words, and unlike systemd's they fold. */
bool flag = false;
gtext_ini_value_bool(&dialect, "TRUE", 4, &flag);
```

Three things a caller meets that no other dialect here has:

- **Two separator characters.** The key ends at the first `=` or `:` on the line.
- **A continuation with no marker.** An indented line continues the previous entry's
  value, joined with a newline - so the value's extent depends on the *next* line, and
  a synthesized multi-line value is written back with the indentation supplied by the
  writer. A value that could not read back as itself - one whose second line would be
  read as a comment, or whose last line is blank - is ::GTEXT_INI_E_UNREPRESENTABLE
  rather than mangled.
- **No escapes at all.** A backslash is data, and so are quotes.

**Interpolation is off by default and available on request**, and the number is why it
is that way round: over the 479 real `configparser` documents on this machine the
*default* `BasicInterpolation` refuses a value in **301** of them and changes a value in
**none**, so a reader that interpolated by default would read 63% of them worse and none
of them better. That measures "not by default"; it says nothing about whether a caller
who wants Python's reading can ask for it, and for a while the docs claimed the wider
thing. gtext_ini_value_interpolate() is the narrower claim implemented -
::GTEXT_INI_INTERPOLATION_BASIC and ::GTEXT_INI_INTERPOLATION_EXTENDED, with
gtext_ini_value_needs_interpolation() to ask whether a value can be used raw - and it is
an accessor beside gtext_ini_unescape() rather than a ::GTEXT_INI_Dialect field, because
no reference in a value changes how a document is tokenized. The default of every
spelling, including a zero-initialized options struct, is
::GTEXT_INI_INTERPOLATION_NONE.

`[DEFAULT]`'s value inheritance does not ship - it is a lookup over a parsed tree, and a
caller who wants it asks the section and then asks `DEFAULT`. The one place it appears is
GTEXT_INI_Interpolate_Options::defaults, because a reference *inside* a value has to
resolve against some map and the reference's is that chain.

`make conformance-ini-configparser` scores this machine's 703 `.cfg` and `.ini` files
against `configparser` itself - the only INI corpus gate here whose reference is
installed - and `make check-ini-configparser-oracle` differs against a pinned
interpreter over 20,000 generated documents.

### Something in between

Change individual fields if you need something between any two of the six.
::GTEXT_INI_Dialect is a plain struct and every field is an axis on which real
specified dialects disagree; \ref format_ini "the format page" has the table.

## 5. Building and writing

```c
GTEXT_INI_Document * doc = gtext_ini_new(NULL);
GTEXT_INI_Group * group = NULL;
gtext_ini_document_add_group(doc, "Desktop Entry", &group);
gtext_ini_group_set(group, "Type", "Application", 11);

GTEXT_INI_Sink sink;
gtext_ini_sink_buffer(&sink);
gtext_ini_write(doc, &sink, NULL);
fputs(gtext_ini_sink_buffer_data(&sink), stdout);
gtext_ini_sink_buffer_free(&sink);
gtext_ini_free(doc);
```

@warning gtext_ini_document_add_group() **invalidates every
::GTEXT_INI_Group pointer** previously returned for that document, because the
group array may move. Look a group up again after adding one.

gtext_ini_group_set() replaces an existing entry or appends one;
gtext_ini_group_add() always appends, and refuses a duplicate under the Desktop
Entry dialect - so a loop cannot build a document the dialect would not have
parsed.

**The writer refuses a value it cannot spell** rather than emitting one that
reads back differently. A value containing a newline, or beginning with a space,
is ::GTEXT_INI_E_UNREPRESENTABLE; gtext_ini_escape() is the fix:

```c
char * escaped = NULL;
size_t escaped_len = 0;
gtext_ini_escape(&dialect, "line one\nline two", 17, NULL, &escaped,
    &escaped_len);
gtext_ini_group_set(group, "Comment", escaped, escaped_len);
gtext_ini_string_free(NULL, escaped);
```

A parsed document that nothing has modified writes back **byte for byte** -
spacing, comments, blank lines, unknown keys and all. Set
GTEXT_INI_Write_Options::normalize to get `key=value` throughout instead, and
GTEXT_INI_Write_Options::emit_comments to `false` to drop the comments
deliberately.

gtext_ini_write_file() writes a temporary beside the destination and renames it
into place, so an interrupted write leaves the previous file intact.

### Win32 profile API

```c
GTEXT_INI_Dialect dialect = gtext_ini_dialect_win32();
```

For the `.ini` files that exist on Windows. **Not** for reproducing what
`GetPrivateProfileString` returns on a given machine, which nothing that reads only
the file can do: the API consults the registry's `IniFileMapping` for the section
first. What it is for is the far larger population of files whose readers are the
applications themselves.

Thirty rules, all measured under wine, because the API documents two. The ones most
likely to surprise:

- **`;` is a comment and `#` is not.** The reference disagrees with itself here -
  `GetPrivateProfileSection` drops a `;` line and `GetPrivateProfileString` retrieves
  it - so `;disabled=1` is a comment to one entry point and a live setting to the
  other. This follows the enumeration API. `#` is a comment to neither.
- **A lookup is case-insensitive over ASCII only**, for keys and section names both,
  and the first of two duplicates wins. A lookup does **not** search a second
  `[a]`, unlike every other dialect here.
- **`[ b ]` and `[b]` are one section**; `[a]b]` is the section `a]b`; `[a]junk` is
  `a`; `[a` with no `]` is not a header at all but a valueless entry.
- **One matching pair of surrounding quotes comes off a value**, which is how a value
  with a leading or trailing blank is spelled. This is a wrapper and not git's
  toggle: `x" mid "y` is unchanged here.
- **No rule of the grammar ever refuses anything.** Every byte sequence is a legal
  document. The one refusal is above the grammar and applies to every dialect: a
  UTF-16 or UTF-32 byte-order mark is ::GTEXT_INI_E_ENCODING unless
  GTEXT_INI_Parse_Options::decode_utf16 is set, in which case UTF-16 is decoded to
  UTF-8 and read. Before that check existed a UTF-16LE `.ini` *parsed*, into one
  group with an empty name whose keys were the file's bytes with NULs between them -
  and because this dialect refuses nothing, that was unconditional.

`make conformance-ini-win32` scores this machine's 701 `.ini` and `.cfg` files against
wine's profile API - 118,790 value comparisons and 214,774 lookups - and
`make check-ini-win32-oracle` differs against the pinned image over 85 generated axes,
scoring the section list, the section enumeration, the per-key value and the lookup
through gtext_ini_document_get() separately. The corpus is real bytes of the
right shape from the **wrong provenance**: this machine has two `.ini` files a Windows
application wrote, and the gate says so where the number is printed.

Two further gates were added on 2026-09-30, each answering something those two cannot.
`make check-ini-win32-authored-oracle` is the one population whose provenance is right
by construction: `WritePrivateProfileString` is the other half of the same reference, so
it is asked to **author** 22 files and our reader is scored against the reference's
reading of them. `make check-ini-win32-encoding-oracle` scores the UTF-16 decode over
each generated document re-encoded in both byte orders - 162 documents, 190 lookups -
which is possible because the reference reads a marked UTF-16 file and answers exactly
as it does for the UTF-8 form. It reads **nothing** from an unmarked one, which is why
there is no content sniffing here.

## 6. What is not here

No streaming reader. Interpolation used to be listed here, and the correction is worth
keeping because the measurement never supported the entry: over the 479 real
`configparser` documents on this machine the default `BasicInterpolation` refuses a value
in 301 of them and changes one in none, which argues for not doing it **by default** and
is silent about offering it at all. It now ships as gtext_ini_value_interpolate(), off
unless asked for. What is genuinely absent is any interpolation for the other six
dialects, because none of their references has the concept.

**Every named dialect is implemented**, Win32 included. That entry used to read "no
Win32 dialect, which is absent by decision - its answers are not a function of the
file's bytes", and the correction is worth keeping: that sentence is true of the
**API's** answers, because `GetPrivateProfileString` consults the registry before the
file. It is not true of the file, and reading the `.ini` files that exist on Windows -
which mostly belong to applications with their own parsers - was the actual goal and
was reachable all along. **One** thing genuinely stays out: the registry redirection,
which no reader of a file can follow.

UTF-16 input was the second and is not any more, and it is worth keeping for the same
reason the Win32 entry is. It was recorded as a scope boundary - "a caller transcodes,
or asks for a decision" - which reads like a decision and is a decision deferred. What
settled it was measuring what happened without one: such a file did not fail, it
succeeded, into nonsense, unconditionally under the dialect that refuses nothing. **A
gap you can describe is a gap; a gap that returns a document is a defect**, and nothing
had checked which this was. UTF-16 is now decoded on request and refused by default, and
what stays out of *that* is writing UTF-16 back - which the reference does not do either:
asked to author a file through the `W` entry points it wrote ANSI and transcoded the
non-ASCII away.

\ref format_ini "The format page" says why for each, and names the one continuation
mode still deliberately absent from the enum because nothing implements it.
