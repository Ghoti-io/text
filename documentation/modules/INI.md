@page ini_module INI

# INI

The INI module reads a
[Desktop Entry](https://specifications.freedesktop.org/desktop-entry-spec/latest/)
file into a tree, gives you accessors for it, lets you build one from nothing,
and writes one back out **byte for byte**.

It is the one module here whose format is chosen by the caller rather than named
by the module. There is no INI specification, so `GTEXT_INI_Dialect` names which
INI, and the default is Desktop Entry 1.5 - the only INI dialect with a
normative document and two independent reference implementations.

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

Change individual fields if you need something between the two. ::GTEXT_INI_Dialect
is a plain struct and every field is an axis on which real specified dialects
disagree; \ref format_ini "the format page" has the table.

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

No systemd, git-config or EditorConfig dialect; no preamble support yet; no
streaming reader; and no Win32 dialect, which is absent by decision rather than
by omission. \ref format_ini "The format page" says why for each.
