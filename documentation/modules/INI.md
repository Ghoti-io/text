@page ini_module INI

# INI

The INI module reads a
[Desktop Entry](https://specifications.freedesktop.org/desktop-entry-spec/latest/)
file into a tree, gives you accessors for it, lets you build one from nothing,
and writes one back out **byte for byte**.

It is the one module here whose format is chosen by the caller rather than named
by the module. There is no INI specification, so `GTEXT_INI_Dialect` names which
INI - six of them: Desktop Entry, a generic derivation of it, git config,
EditorConfig, systemd and Python's configparser - and the default is Desktop Entry 1.5.
EditorConfig is the only one with a **normative conformance suite**, and the only one
where this module scores higher than either reference implementation does; configparser
is the only one with **no specification at all**, so every rule of it is a measurement
rather than a citation.

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

**Interpolation does not ship, and the number is the reason**: over the 479 real
`configparser` documents on this machine the *default* `BasicInterpolation` refuses a
value in **301** of them and changes a value in **none**. `[DEFAULT]`'s value
inheritance does not ship either - it is a lookup over a parsed tree, and a caller who
wants it asks the section and then asks `DEFAULT`.

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

## 6. What is not here

No `configparser` dialect; no streaming reader; and no Win32 dialect, which is absent
by decision rather than by omission - its answers are not a function of the file's
bytes. \ref format_ini "The format page" says why for each, and names the one
continuation mode still deliberately absent from the enum because nothing implements
it.
