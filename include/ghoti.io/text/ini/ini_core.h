/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file ini_core.h
 * @brief Status codes, error payload, dialect and parse options for INI.
 *
 * **There is no INI specification, and this module never claims to implement
 * one.** What it implements is a *named dialect*, chosen by the caller, and
 * every dialect here is either a published specification or a documented
 * derivation from one. The default is
 * ::GTEXT_INI_DIALECT_DESKTOP_ENTRY - the freedesktop.org Desktop Entry
 * Specification, version 1.5, dated 2020-04-27 - because it is the only INI
 * dialect with both a normative document and two independent reference
 * implementations to be measured against.
 *
 * ::GTEXT_INI_DIALECT_GENERIC is that grammar plus seven named changes and
 * nothing else - **six relaxations and one normalisation**. It is not "be
 * permissive": a written list is what makes it checkable, and @ref format_ini
 * names each one with the document that exercises it.
 *
 * The distinction between the six and the one matters and was learned the hard
 * way. A relaxation only widens what is accepted, so every document the strict
 * dialect accepts is accepted by the generic one *with the same values* - which
 * is a free conformance property over a corpus that already exists.
 * ::GTEXT_INI_Dialect::accept_crlf is not like that: it removes a CR from a line
 * the strict dialect already accepted, so the two disagree about the value.
 * The inherited property therefore holds over documents containing no CR, and
 * the gates say so rather than scoring the difference as a failure.
 *
 * **The reader returns raw value bytes.** Unescaping, list splitting, typed
 * conversion and locale selection are in ini_value.h, because that is where
 * both reference implementations put them - GLib parses `a\qb` and fails only
 * when asked for it as a string, and `systemd.syntax(7)` scopes quoting to
 * "settings where quoting is allowed", which a grammar cannot know. Doing it
 * during the parse would also destroy the original spelling, and Desktop Entry
 * §3 *requires* that a rewrite preserve what it did not understand.
 */

#ifndef GHOTI_IO_GTEXT_INI_INI_CORE_H
#define GHOTI_IO_GTEXT_INI_INI_CORE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_INI_Status
 * @brief Status codes returned by INI APIs.
 *
 * The refusals are split finely because an INI dialect's rules are separable
 * in a way a single "invalid" hides: a duplicate key is a rule about the
 * document's data, a key-name character is a rule about its spelling, and an
 * entry before the first group is a rule about its structure. A caller
 * switching dialects needs to know which rule it tripped over, because each
 * one is relaxed by a different dialect.
 */
typedef enum {
  GTEXT_INI_OK = 0,     ///< Success.
  GTEXT_INI_E_INVALID,  ///< A parameter was not usable (NULL, absurd length).
  GTEXT_INI_E_OOM,      ///< Out of memory.
  GTEXT_INI_E_LIMIT,    ///< A configured limit was exceeded.
  /**
   * A line is none of blank, comment, group header or entry.
   *
   * This is the code both references return for the same input: GLib says
   * "which is not a key-value pair, group, or comment" and
   * `desktop-file-validate` says "which is not a comment, a group or an
   * entry". It is also what a `;` comment produces under
   * ::GTEXT_INI_DIALECT_DESKTOP_ENTRY, where `;` is not a comment character.
   */
  GTEXT_INI_E_BAD_LINE,
  /**
   * A group name contains `[`, `]` or a control character (Desktop Entry
   * §3.2), or the header is unterminated.
   */
  GTEXT_INI_E_BAD_GROUP,
  /**
   * A key name uses a character the dialect's key charset does not allow.
   * Desktop Entry §3.3 permits only `A-Za-z0-9-`, plus a `[LOCALE]` postfix.
   */
  GTEXT_INI_E_BAD_KEY,
  /** Two groups have the same name, which Desktop Entry §3.2 forbids. */
  GTEXT_INI_E_DUPGROUP,
  /**
   * Two keys in one group have the same name, which Desktop Entry §3.3
   * forbids. Under ::GTEXT_INI_DUPKEY_FIRST_WINS, ::GTEXT_INI_DUPKEY_LAST_WINS
   * or ::GTEXT_INI_DUPKEY_COLLECT this is not an error.
   */
  GTEXT_INI_E_DUPKEY,
  /**
   * An entry appeared before the first group header.
   *
   * Desktop Entry §3.2 allows nothing but comments before the first group, and
   * GLib reports it separately from a malformed line, so this module does too.
   * ::GTEXT_INI_Dialect::allow_preamble turns it off.
   */
  GTEXT_INI_E_NO_GROUP,
  /**
   * An escape sequence the dialect does not define. Desktop Entry §4 defines
   * exactly `\s`, `\n`, `\t`, `\r` and `\\` for the string types, plus `\;`
   * inside a list.
   *
   * Raised by ini_value.h, never by the parser: an unknown escape is a
   * property of a *value*, and a document containing one still parses. That
   * is not this module's invention - it is what GLib does, and the reason
   * `desktop-file-validate` accepts a document `g_key_file_get_string()`
   * refuses.
   */
  GTEXT_INI_E_BAD_ESCAPE,
  /** Invalid UTF-8. Desktop Entry §3 requires the file to be UTF-8. */
  GTEXT_INI_E_BAD_UNICODE,
  /** A control character where the dialect forbids one. */
  GTEXT_INI_E_CONTROL,
  /** A number outside what its C type can hold. */
  GTEXT_INI_E_RANGE,
  /**
   * A value is not of the type asked for: `gtext_ini_value_bool()` on
   * anything but `true` or `false` (Desktop Entry §4 admits no others), or
   * `gtext_ini_value_double()` on something `%f` does not accept.
   */
  GTEXT_INI_E_TYPE,
  /** A sink refused output during serialization. */
  GTEXT_INI_E_WRITE,
  /**
   * The value is fine and the dialect has no spelling for it.
   *
   * Not a malformed document, which is what every code above reports. A
   * dialect with no quoting cannot represent a value whose first character is
   * a space *except* through an escape, so the writer reports this when asked
   * to emit one under a dialect whose escape set is empty - and a caller can
   * act on it by choosing a dialect that has one.
   */
  GTEXT_INI_E_UNREPRESENTABLE
} GTEXT_INI_Status;

/**
 * @struct GTEXT_INI_Error
 * @brief Where a parse failed and what it was looking at.
 *
 * `offset` is a byte index into the input the caller passed. INI dialects are
 * UTF-8 (Desktop Entry §3, EditorConfig, systemd) and this module refuses
 * other encodings rather than transcoding, so there is no decoding step that
 * could move it.
 *
 * `line` is 1-based. `col` counts characters rather than bytes, so it points
 * at the right character in a line containing multi-byte text.
 *
 * `context_snippet` is heap-allocated when present and released by
 * gtext_ini_error_free().
 */
typedef struct {
  GTEXT_INI_Status code;      ///< What went wrong.
  const char * message;       ///< Static string; never freed.
  size_t offset;              ///< Byte offset into the input, 0-based.
  int line;                   ///< Line, 1-based.
  int col;                    ///< Column in characters, 1-based.
  char * context_snippet;     ///< Nearby input, or NULL. Freed by
                              ///< gtext_ini_error_free().
  size_t context_snippet_len; ///< Bytes of context_snippet.
  size_t caret_offset;        ///< Byte offset within context_snippet of the
                              ///< position `offset` names.
} GTEXT_INI_Error;

/**
 * @brief Release anything heap-allocated inside an error.
 *
 * Safe on a zeroed struct, on one a failed parse filled in, and twice.
 *
 * **The snippet comes from the default allocator even when the parse used a
 * caller's**, and this releases it through the default one too, for the reason
 * gtext_toml_error_free() gives: an error struct outlives the parse, and a
 * parse can fail before it has read its options at all.
 *
 * @param err The error to release. NULL is ignored.
 */
GTEXT_API void gtext_ini_error_free(GTEXT_INI_Error * err);

/**
 * @enum GTEXT_INI_Dupkey_Mode
 * @brief What a second entry with the same key in one group means.
 *
 * **The tree stores every entry either way**, in document order, and this
 * decides only what gtext_ini_group_get() answers and whether the parse
 * refuses the document. That is deliberate: git config and systemd both need
 * every occurrence, and a tree that kept one value per key could not grow the
 * others later without changing the type a caller walks.
 * gtext_ini_group_count_key() and gtext_ini_group_get_nth() reach them under
 * every mode, and are the spelling of `git config --get-all`.
 */
typedef enum {
  /** Refuse the document. Desktop Entry §3.3. The default. */
  GTEXT_INI_DUPKEY_ERROR = 0,
  /** Lookup answers the first occurrence. Win32's profile API does this. */
  GTEXT_INI_DUPKEY_FIRST_WINS,
  /** Lookup answers the last occurrence. GLib's `GKeyFile` does this. */
  GTEXT_INI_DUPKEY_LAST_WINS,
  /**
   * Every occurrence is a value of its own, and a caller is expected to read
   * them with gtext_ini_group_count_key() and gtext_ini_group_get_nth().
   *
   * **Lookup answers the last**, which is git's: measured, `git config --get
   * a.k` on a file with `k = 1` then `k = 2` prints `2`, while `--get-all`
   * prints both in order. An earlier draft of this comment said "the first",
   * written before any dialect used the mode and contradicted by the only
   * reference that has it.
   *
   * So this and ::GTEXT_INI_DUPKEY_LAST_WINS select the same value, and the
   * library cannot tell them apart. The difference is a promise to the caller
   * rather than a behaviour: under LAST_WINS the earlier occurrences have been
   * overridden and reading them is reading dead data, and under COLLECT they
   * are values the document meant. Nothing here enforces that, which is why it
   * is written down.
   */
  GTEXT_INI_DUPKEY_COLLECT
} GTEXT_INI_Dupkey_Mode;

/**
 * @enum GTEXT_INI_Name_Style
 * @brief The grammar a dialect's group and key names follow.
 *
 * A single "strict or not" flag was here first and could not hold three
 * grammars, because the two specified dialects disagree about group names and
 * key names *independently*:
 *
 * | | group name | key name |
 * |---|---|---|
 * | Desktop Entry §3.2, §3.3 | any ASCII but `[`, `]`, control | `A-Za-z0-9-`, optional `[LOCALE]` |
 * | git config | `A-Za-z0-9-.`, plus a quoted subsection | `A-Za-z0-9-`, **first character alphabetic** |
 * | EditorConfig | **any byte, `]` included** | anything but `=` |
 *
 * So git is *stricter* than Desktop Entry about group names and stricter again
 * about the first character of a key, while being the only one of the two with
 * a subsection; EditorConfig is looser than either about both. Those are not
 * points on one axis, and one bit for a family the references split several ways
 * is a mistake this repository has made before.
 *
 * The style also decides **where a group name ends**, which is not a charset
 * question and cannot be asked one byte at a time: a dialect whose names may
 * hold a `]` has to close the header at the *last* one on the line rather than
 * the first. gtext_ini_group_close_is_last() is that question, asked by name so
 * that the parser never tests an id.
 *
 * Every entry here was measured against the reference rather than read off the
 * manual page - `git-config(1)` says a variable "must belong to some section",
 * and git accepts one that does not.
 */
typedef enum {
  /**
   * Desktop Entry §3.2 and §3.3. A group name is any ASCII except `[`, `]` and
   * a control character; a key is `A-Za-z0-9-` with an optional `[LOCALE]`
   * postfix that ::GTEXT_INI_Dialect::locale_postfix governs.
   */
  GTEXT_INI_NAMES_DESKTOP_ENTRY = 0,
  /**
   * git config. A section name is `A-Za-z0-9-.` with no case significance; a
   * key is `A-Za-z0-9-` and **must begin with a letter**, so `1k` and `-k` are
   * both refused and `k-1` is not. Measured: `git config -f` exits 128 on
   * `1k = v`, on `-k = v`, on `k_1 = v` and on `k.1 = v`, and exits 0 on
   * `[12] k = v` - a *section* name may start with a digit even though a key
   * may not.
   */
  GTEXT_INI_NAMES_GIT,
  /**
   * Anything that reads back as itself: a key may not contain `=` or a line
   * terminator and a group name may not contain `[`, `]` or a control
   * character. What ::GTEXT_INI_DIALECT_GENERIC uses.
   */
  GTEXT_INI_NAMES_ANY,
  /**
   * EditorConfig 0.17.2. A key is "the part before the first `=` on the line",
   * so it may hold anything but `=` - a space included, and `ke y = value` is a
   * key called `ke y`, which the conformance suite asserts. A section name "may
   * contain any characters between the square brackets", so the header closes at
   * the **last** `]` on the line and `[a]b]` is one section named `a]b`.
   *
   * Measured both ways against both cores: core-c finds the last `]` and core-py
   * matches greedily to the same place, and neither refuses a `[` or a control
   * character inside a name. **The empty name `[]` is accepted**, which core-c
   * does and core-py refuses; see gtext_ini_group_is_preamble() for the one
   * consequence in the tree.
   *
   * Unlike ::GTEXT_INI_NAMES_ANY, a `#` or `;` inside a name is not special and
   * a `]` does not end it. core-py refuses `[a#b]` outright - a bug against its
   * own specification, which says any characters.
   */
  GTEXT_INI_NAMES_EDITORCONFIG,
  /**
   * systemd unit and configuration files. A key is everything before the first
   * `=` on the logical line, trimmed - measured, `Environ ment=v` is a key called
   * `Environ ment` and `Environment-x=v` one called `Environment-x`, both reported
   * as *unknown settings* rather than as syntax errors. A section name is
   * everything between `[` and the first `]`, and may hold a space: `[Serv ice]`
   * is a section named `Serv ice`.
   *
   * The same shape as ::GTEXT_INI_NAMES_EDITORCONFIG for keys and as
   * ::GTEXT_INI_NAMES_ANY for group names, and it is its own arm anyway, because
   * what makes systemd's names different is not their charset: **a name may
   * contain a line continuation.** `[Serv\` then `ice]` is the section
   * `Serv ice` - measured - so the joined form is the canonical form and the
   * document's own bytes are the name. See
   * ::GTEXT_INI_CONTINUATION_JOIN_SPACE.
   */
  GTEXT_INI_NAMES_SYSTEMD,
  /**
   * Python `configparser`, as CPython 3.13.5 reads it. Both halves come from one
   * regular expression apiece, and reading them is what settles two rules no
   * prose states.
   *
   * A key is `(?P<option>.*?)\s*(?P<vi>=|:)`: everything before the **first** `=`
   * or `:` on the line, trimmed - so `ke y = v` is a key called `ke y`, `k:b=c`
   * has the value `b=c` and `k=b:c` the value `b:c`, whichever delimiter comes
   * first. An empty key is refused: `= v` is a `ParsingError`.
   *
   * A section header is `\[(?P<header>.+)\]` matched with `re.match`, and three
   * consequences fall out of that spelling, all measured:
   *
   *   - `.+` is greedy, so the header closes at the **last** `]` on the line and
   *     `[a]b]` is one section named `a]b`, exactly as EditorConfig does.
   *   - `re.match` is not `fullmatch`, so **anything after that `]` is silently
   *     discarded**: `[a]junk` is the section `a`, and so is `[a] k = v`. That is
   *     ::GTEXT_INI_HEADER_REMAINDER_IGNORE, and it is the reason the axis is an
   *     enum rather than a flag.
   *   - `.+` needs one character, so **`[]` is refused** while
   *     ::GTEXT_INI_NAMES_EDITORCONFIG accepts it -
   *     ::GTEXT_INI_Dialect::allow_empty_group_name is that difference.
   *
   * The name is not trimmed inside the brackets: `[ b ]` is a section literally
   * named `" b "`, and `[ ]` one named `" "`. Any byte but a line terminator may
   * appear in it, `[`, `#` and a control character included.
   */
  GTEXT_INI_NAMES_CONFIGPARSER,
  /**
   * The Win32 profile API's names, measured under wine because there is no
   * document that states them.
   *
   * Keys are ::GTEXT_INI_NAMES_EDITORCONFIG's shape - everything before the
   * first `=` on the line, trimmed - and group names are any byte but a line
   * terminator, closed at the **last** `]`, and *trimmed*. Three properties are
   * this style's alone and each is measured:
   *
   * - **The name is trimmed inside the brackets**, so `[ b ]` and `[b]` are one
   *   section. Every other dialect here either forbids the space or keeps it;
   *   ::GTEXT_INI_Dialect::trim_group_name carries it.
   * - **The empty name and the empty key are both spellable.** `[]` parses, and
   *   `= v` is an entry whose key is the empty string, which
   *   `GetPrivateProfileStringA(sec, "", ...)` retrieves.
   * - **A `[` line with no `]` is not a header at all** - it is an ordinary
   *   line, and since a Win32 line needs no `=`, it is a valueless entry named
   *   `[a`. ::GTEXT_INI_Dialect::unclosed_header_is_line carries that.
   *
   * `;` and `#` are ordinary bytes in both names: measured, `k;c=v` is the key
   * `k;c` and `[a;b]` is the section `a;b`.
   */
  GTEXT_INI_NAMES_WIN32
} GTEXT_INI_Name_Style;

/**
 * @enum GTEXT_INI_Continuation_Mode
 * @brief Whether a logical line may span several physical lines, and how.
 *
 * The three dialects that have a continuation rule disagree about what the
 * join *inserts*, which is why this is an enum and not a flag: git joins with
 * nothing, systemd replaces the backslash with a space, and configparser joins
 * an indented line with a newline. A parser that got this wrong would produce a
 * value differing from the reference by exactly one character, which is the
 * kind of difference a corpus of real files never notices.
 *
 * Only the modes this module implements are listed. The other two are named in
 * @ref format_ini under what is not implemented, because a constant nothing
 * reads is worse than an absent one.
 */
typedef enum {
  /** A line is a line. Desktop Entry, EditorConfig. */
  GTEXT_INI_CONTINUATION_NONE = 0,
  /**
   * A backslash immediately before the line terminator joins this line to the
   * next **with nothing between them**, and both the backslash and the
   * terminator are discarded.
   *
   * git's, and measured rather than read: `k = one\` then `two` gives
   * `onetwo`, and `k = one \` then `two` gives `one two` - the space that
   * survives is the one that was already in the value, not one the join added.
   *
   * Two further rules that come with it, both measured, and both of which a
   * plausible implementation gets wrong:
   *
   *   - A continuation inside a quoted run is still a continuation:
   *     `k = "one\` / `two"` gives `onetwo`.
   *   - **An inline comment swallows it.** `k = v # c\` followed by `d = w`
   *     gives `a.k = v` and `a.d = w`, not a joined line: once a comment has
   *     started, the backslash is comment text, so the line ends at its
   *     terminator like any other.
   */
  GTEXT_INI_CONTINUATION_JOIN_EMPTY,
  /**
   * systemd: a trailing backslash is **replaced by a space**, and a comment block
   * between the two halves is skipped.
   *
   * `systemd.syntax(7)` says exactly this - "lines ending in a backslash are
   * concatenated with the following line while reading and the backslash is
   * replaced by a space character" - and it needed a **minimal pair** to confirm,
   * because every example in the manual indents the continued line. With the
   * second line indented, joining with a space and joining with nothing both give
   * two words and look identical. With it flush left they differ, and inside a
   * quoted run the answer is unambiguous: `Environment="W1\` then `W2"` is the
   * single word `W1 W2`, hex `5731205732`. Measured against systemd 257.
   *
   * Two rules travel with it, both measured and neither obvious:
   *
   *   - **A comment block between the halves is skipped**, so a continuation
   *     jumps over a `#` or `;` line and joins with whatever follows it. The
   *     manual says so and it is true.
   *   - **A blank line ends the continuation** instead of being skipped. The
   *     trailing backslash then simply disappears, exactly as one at end of input
   *     does. The manual does not say this.
   *
   * Unlike git's, **this continuation is a property of the line rather than of the
   * value**: it works on a group header and on a key as well, because systemd
   * assembles the logical line before classifying it. That is why
   * gtext_ini_continuation_at() is shared by the parser and the value scanner
   * rather than living in the scanner alone.
   */
  GTEXT_INI_CONTINUATION_JOIN_SPACE,
  /**
   * configparser: a following line **indented more deeply than the line the entry
   * began on** is part of that entry's value, and the lines are joined with a
   * **newline**.
   *
   * This is the one continuation here that is not announced by a marker in the
   * value. The other two are a backslash the value's own bytes carry, so a
   * scanner walking the value finds them; this one is a property of the *next*
   * line, and it cannot be recognised without knowing how far the entry's own
   * line was indented. gtext_ini_dialect_scans_values() is false for it, and the
   * parser decides the extent.
   *
   * Every rule below was measured against CPython 3.13.5, and the ones that are
   * not in the documentation are the ones that decide whether a value is right:
   *
   *   - The comparison is **strictly greater, against the entry's own line**, not
   *     "is indented at all": `  k=1` followed by `  2` is a *syntax error*
   *     (`  2` is read as an entry and has no delimiter), and `    k=1` followed
   *     by `  2` is the same error. Indentation is counted in characters, and a
   *     tab counts as one.
   *   - A **group header ends the continuation** whatever its indentation:
   *     configparser clears the current key when it reads one, with the comment
   *     "so sections can't start with a continuation line". But an *indented*
   *     header while a value is open is not a header at all - it is a
   *     continuation line whose text happens to be `[b]`.
   *   - A **comment line contributes nothing and does not end it**, so a value
   *     jumps over a `#` or `;` line exactly as systemd's does. The comment test
   *     runs first and it runs on the *stripped* line, so an indented `#` is a
   *     comment and never a continuation.
   *   - A **blank line contributes an empty line to the value** and does not end
   *     it: `k=1`, blank, `  2` is `1\n\n2`. But trailing blank lines are
   *     dropped, because the reference joins the pieces and then strips the
   *     result - so the value ends at the last line that contributed text, and
   *     the blank and comment lines after it belong to the document.
   *   - Each contributing line is **stripped on both sides** before joining, the
   *     first one included.
   *
   * The stored raw value therefore spans several lines and holds every byte
   * between them, comment lines included; gtext_ini_unescape() performs the join.
   */
  GTEXT_INI_CONTINUATION_INDENT
} GTEXT_INI_Continuation_Mode;

/**
 * @enum GTEXT_INI_Bool_Style
 * @brief Which spellings gtext_ini_value_bool() accepts.
 *
 * Desktop Entry §4 admits two words and no others - "values of type boolean must
 * either be the string `true` or `false`" - and systemd admits eight. That is not
 * a relaxation a caller can express by setting a flag, because the two sets are
 * *lists*, so it is an enum with a member per dialect family rather than a
 * character set.
 *
 * The typed accessors for numbers need no such thing: systemd's time spans and
 * sizes are per-setting rules rather than grammar, and @ref format_ini's "Not
 * implemented" says where they belong instead.
 */
typedef enum {
  /** `true` and `false`, exactly, as Desktop Entry §4 requires. */
  GTEXT_INI_BOOLS_TRUE_FALSE = 0,
  /**
   * systemd's set: `1`, `yes`, `true`, `on` and `0`, `no`, `false`, `off`.
   * Case-sensitive, like every other systemd comparison.
   */
  GTEXT_INI_BOOLS_SYSTEMD,
  /**
   * configparser's `BOOLEAN_STATES`: the same eight words systemd admits, and
   * **case-insensitively**, because `getboolean()` lower-cases the value before
   * looking it up. Measured: `YES`, `Yes` and `TRUE` are all accepted and `n`,
   * `t` and `2` are not.
   *
   * So this and ::GTEXT_INI_BOOLS_SYSTEMD differ in exactly one respect and it
   * is not the word list. A single flag for "systemd's set" would have made
   * `TRUE` a type error under a dialect that accepts it.
   */
  GTEXT_INI_BOOLS_CONFIGPARSER
} GTEXT_INI_Bool_Style;

/**
 * @enum GTEXT_INI_Space_Set
 * @brief Which bytes a dialect treats as whitespace.
 *
 * Three answers, measured against three references, and the differences are two
 * bytes here and four there - which is exactly why this is a table rather than an
 * assumption. No corpus of real files on this machine contains any of the six.
 *
 * The line terminator is never in any of these sets: a line ends at its
 * terminator before any trimming happens, and which bytes terminate a line is
 * ::GTEXT_INI_Dialect::accept_crlf and ::GTEXT_INI_Dialect::lone_cr_terminates.
 */
typedef enum {
  /**
   * Space and tab, and nothing else. Desktop Entry and git config.
   *
   * git carries its own ctype table in which `\v` and `\f` are control
   * characters, and it is not an accident of the implementation: measured,
   * `k = a\v` keeps the vertical tab as the value's last byte and `\vk = v` is a
   * syntax error rather than skipped indentation.
   */
  GTEXT_INI_SPACE_BLANK = 0,
  /**
   * `<ctype.h>`'s `isspace()` in the C locale: space, tab, `\v`, `\f` (and the
   * terminators, which never reach here). EditorConfig, whose two cores both ask
   * the platform - core-c calls `isspace()` and core-py uses Python's `\s`.
   */
  GTEXT_INI_SPACE_CTYPE,
  /**
   * Python's `\s` restricted to ASCII: `isspace()`'s set **plus `\x1c` through
   * `\x1f`**, the four ASCII separator controls. configparser's, and measured
   * rather than assumed - all four are stripped as indentation, accepted between
   * a key and its delimiter, trimmed off the end of a value, and allowed to
   * precede a comment introducer.
   *
   * It is ASCII-only on purpose, and the omission is a **stated divergence rather
   * than an oversight**: Python's `\s` and `str.strip()` are Unicode-aware, so a
   * no-break space or a U+0085 also indents a line there. This reader is
   * byte-oriented and cannot ask that question of one byte, so those documents
   * are excluded from the differential and counted, and @ref format_ini names the
   * rule they would need.
   */
  GTEXT_INI_SPACE_PYTHON
} GTEXT_INI_Space_Set;

/**
 * @enum GTEXT_INI_Header_Remainder
 * @brief What anything but whitespace after a group header's `]` means.
 *
 * Three references, three answers, on one of the least interesting-looking lines
 * a document can contain. This was a `bool` until configparser arrived and needed
 * the third value; a flag would have had to call `[a]junk` either an error or an
 * entry, and it is neither.
 */
typedef enum {
  /**
   * Refuse the document. Desktop Entry, EditorConfig and systemd: `GKeyFile`
   * rejects `[G] junk`, and anything claiming to read the same documents must.
   */
  GTEXT_INI_HEADER_REMAINDER_ERROR = 0,
  /**
   * The remainder is an entry on the same line. git config, which says so -
   * "the remainder of the line after the section header" is a setting - and means
   * it: `[a] k = v` sets `a.k` and `[a] junk` sets a valueless `junk`. A comment
   * introducer there is still a comment.
   */
  GTEXT_INI_HEADER_REMAINDER_ENTRY,
  /**
   * The remainder is **discarded**, and the document is accepted. configparser,
   * and not by decision: its section pattern is applied with `re.match`, which
   * does not have to reach the end of the line, so whatever follows the last `]`
   * is simply never looked at. `[a]junk`, `[a] k = v` and `[a]=v` are all the
   * section `a` and nothing else.
   *
   * This module keeps those bytes so that a rewrite reproduces them; what it does
   * not do is give them a meaning.
   */
  GTEXT_INI_HEADER_REMAINDER_IGNORE
} GTEXT_INI_Header_Remainder;

/**
 * @enum GTEXT_INI_Dialect_Id
 * @brief Which dialect a ::GTEXT_INI_Dialect was built from.
 *
 * Carried so that a dialect can say where it came from - an error message, and
 * the conformance runner's report, both name it. A caller who changes a field
 * of a constructed dialect leaves this alone on purpose: it records the *base*,
 * which is the thing a deviation is measured against.
 */
typedef enum {
  /**
   * freedesktop.org Desktop Entry Specification, version 1.5, 2020-04-27.
   */
  GTEXT_INI_DIALECT_DESKTOP_ENTRY = 0,
  /**
   * Desktop Entry with six relaxations and one normalisation, listed on
   * @ref format_ini. Every document the Desktop Entry dialect accepts **and
   * that contains no CR** parses identically here - a tested property rather
   * than a claim, because a relaxation may only add accepted documents. The CR
   * proviso is ::GTEXT_INI_Dialect::accept_crlf, which is the one of the seven
   * changes that alters a value rather than widening acceptance.
   */
  GTEXT_INI_DIALECT_GENERIC,
  /**
   * git config, as `git-config(1)` "Syntax" defines it and as git itself reads
   * it. The version measured against is named in
   * `tools/oracle/containers/IMAGES`.
   *
   * This one is **not** a relaxation of Desktop Entry in either direction, and
   * that is the point of it being its own dialect rather than a set of flags on
   * another: it accepts documents Desktop Entry refuses (a preamble, an inline
   * comment, a continuation, a quoted value, a valueless key, a repeated key,
   * a subsection) *and* refuses documents Desktop Entry accepts (a key that
   * does not begin with a letter, a group name containing anything but
   * `A-Za-z0-9-.`).
   */
  GTEXT_INI_DIALECT_GIT_CONFIG,
  /**
   * EditorConfig, as specification 0.17.2 defines it. The versions of the two
   * cores it was differed against are named in
   * `tools/oracle/containers/IMAGES`, and the normative conformance suite is
   * pinned in `tools/conformance/EDITORCONFIG_SUITE_COMMIT`.
   *
   * The simplest grammar of the four and **the only one with a normative
   * conformance suite**, which is why it is the only dialect here whose
   * correctness claim is a pass count rather than a differential: `make
   * conformance-ini-editorconfig` scores the 34 `parser` assertions of
   * `editorconfig-core-test`. Both reference cores score 33 of those 34, so
   * agreeing with either of them everywhere would be a failure.
   */
  GTEXT_INI_DIALECT_EDITORCONFIG,
  /**
   * systemd unit and configuration file syntax, as `systemd.syntax(7)` defines it
   * and as systemd itself reads it. The version measured against is named in
   * `tools/oracle/containers/IMAGES`.
   *
   * The widest grammar of the five and the only one that **does not refuse a
   * malformed line**: systemd warns and skips it. This module refuses the
   * document instead, and @ref format_ini says why and what the differential does
   * about it.
   */
  GTEXT_INI_DIALECT_SYSTEMD,
  /**
   * Python `configparser`, as CPython reads it. The version measured against is
   * named in `tools/oracle/containers/IMAGES`.
   *
   * **The one dialect here whose specification is an implementation.** The Python
   * documentation describes `configparser`'s behaviour rather than defining a
   * format, and says so; there is no document to be conformant to and no
   * conformance suite to score, so this dialect's correctness claim is a
   * differential and nothing else. Every field of it was measured.
   *
   * Two consequences of that are worth reading before trusting a number:
   *
   *   - **The reference has no single answer.** Five configurations of
   *     `ConfigParser` gave five different readings of one document, so the
   *     differential pins its configuration and prints it:
   *     `interpolation=None`, `strict=True`, `allow_no_value=False`,
   *     `inline_comment_prefixes=None`, `empty_lines_in_values=True`, and
   *     `default_section` set to a name no document can spell. This dialect is
   *     the *default* configuration except for interpolation and the default
   *     section, both of which are layers above the grammar - @ref format_ini
   *     says what each choice leaves out.
   *   - **The input channel changes the grammar.** `read_string()` and
   *     `read(path)` disagree about exactly one thing: a lone CR. Python's
   *     universal-newline translation makes it a line terminator when a file is
   *     read and leaves it as data when a string is, and nothing in
   *     `configparser` itself is involved either way. This dialect follows the
   *     file, because a file is what an INI document is;
   *     ::GTEXT_INI_Dialect::lone_cr_terminates records it.
   */
  GTEXT_INI_DIALECT_CONFIGPARSER,
  /**
   * The Win32 profile API, as `GetPrivateProfileString` and
   * `GetPrivateProfileSection` read a file.
   *
   * **The one dialect here whose reference is two entry points of one
   * implementation that disagree with each other**, and the disagreement is
   * about the most consequential rule in the format: a `;` line.
   * `GetPrivateProfileSection` discards it; `GetPrivateProfileString` does not,
   * so `;disabled=1` is a live setting to one API and a comment to the other.
   * This dialect follows the enumeration API and treats `;` as a comment,
   * because a reader that returns a commented-out setting is worse than one
   * that agrees with only half of its reference. `#` is a comment to neither
   * and is an ordinary byte here.
   *
   * **It cannot reproduce what an application sees**, and that is a property of
   * the API rather than of this reader: `GetPrivateProfileString` consults the
   * registry's `IniFileMapping` for the section and reads the file only "if
   * there is no subkey or entry for the section name". Where a mapping exists,
   * no reader of the file can agree with it. @ref format_ini has the
   * quotation. What this dialect is for is the far larger population: the
   * `.ini` files on Windows that applications parse themselves.
   */
  GTEXT_INI_DIALECT_WIN32
} GTEXT_INI_Dialect_Id;

/**
 * @struct GTEXT_INI_Dialect
 * @brief The exact rules a parse follows.
 *
 * Build one with gtext_ini_dialect_desktop_entry() or
 * gtext_ini_dialect_generic() and change what you mean to change. A zeroed
 * struct is *not* a dialect: it names no comment character, so every comment
 * line in the document would be a syntax error.
 *
 * Every field here is an axis on which real, specified dialects disagree, and
 * @ref format_ini has the table. A field is not added for an axis with no
 * dialect on the other side of it.
 */
typedef struct {
  /** Which dialect this was built from. See ::GTEXT_INI_Dialect_Id. */
  GTEXT_INI_Dialect_Id id;

  /**
   * Whether a line beginning with `#` is a comment. True in every dialect.
   */
  bool comment_hash;

  /**
   * Whether a line beginning with `;` is a comment.
   *
   * **False for Desktop Entry**, which is the single most surprising rule in
   * the dialect and is confirmed by both references: GLib returns an error for
   * `; comment`, and `desktop-file-validate` exits 1 saying it is "not a
   * comment, a group or an entry".
   */
  bool comment_semicolon;

  /**
   * Whether a comment or group or entry may begin after leading whitespace.
   *
   * False for Desktop Entry, whose §3 grammar puts `#` and `[` at the start of
   * the line. The two references disagree here - GLib accepts an indented line
   * and the validator reports it - so this module follows the specification
   * and the validator, and @ref format_ini records GLib's laxity as a
   * difference rather than adopting it.
   */
  bool allow_leading_whitespace;

  /**
   * Whether entries may appear before the first group header.
   *
   * False for Desktop Entry §3.2. EditorConfig is the one specified dialect
   * that allows it, which is why the axis exists.
   */
  bool allow_preamble;

  /** Whether a second group with an existing name is accepted. */
  bool allow_duplicate_groups;

  /**
   * Whether `[]` is a group whose name is the empty string.
   *
   * **True for EditorConfig alone**, and it is a disagreement between that
   * dialect's own two references: core-c accepts `[]` and core-py refuses it,
   * and the specification says a name "may contain any characters", so the
   * permissive reading wins. Everything else here refuses it - `GKeyFile` fails
   * with "Invalid group name: ", and configparser's section pattern needs one
   * character, so `[]` is not a header there and falls through to be read as an
   * ordinary line, which has no delimiter and is a `ParsingError`.
   *
   * The one consequence in the tree is that gtext_ini_group_find("") can find a
   * `[]` group or the preamble group, whichever comes first;
   * gtext_ini_group_is_preamble() tells them apart.
   */
  bool allow_empty_group_name;

  /**
   * Whether an entry's key may be the empty string.
   *
   * ::GTEXT_INI_DIALECT_WIN32 alone, and it is not a relaxation for its own
   * sake: measured, `= v` is an entry that
   * `GetPrivateProfileStringA(sec, "", ...)` returns `v` for, so the empty key
   * is *addressable* rather than merely tolerated. Every other dialect here
   * refuses it - core-c reads `=v` as a property named by the empty string and
   * core-py refuses it, and the EditorConfig arm follows core-py.
   *
   * It is the key-side twin of ::GTEXT_INI_Dialect::allow_empty_group_name and
   * is a separate field because the two references split differently: Win32
   * allows both, EditorConfig the group name only.
   */
  bool allow_empty_key;

  /**
   * Whether the whitespace inside a group header's brackets is part of the name.
   *
   * True for ::GTEXT_INI_DIALECT_WIN32, where `[ b ]`, `[\tb\t]` and `[b]` are
   * one section - measured, with the whitespace set being
   * ::GTEXT_INI_Dialect::space_set's rather than a blank-only run.
   *
   * False for every other dialect, and the three of them that permit a space in
   * a name *keep* it: configparser's `[ b ]` is a section literally named
   * `" b "`, and asking for `[b]` there is a `KeyError`. So this is not a
   * convenience that could be turned on everywhere; it changes which document a
   * lookup answers.
   */
  bool trim_group_name;

  /**
   * Whether a line whose first non-blank byte is `[` but which holds no `]` is
   * an ordinary line rather than a malformed header.
   *
   * True for ::GTEXT_INI_DIALECT_WIN32 only. Measured: `[a` followed by `k=v`
   * leaves `k` in whatever section was current - the preamble, if no header has
   * been seen - and `GetPrivateProfileSectionA` reports `[a` itself as one of
   * that section's entries. Nothing is refused and nothing is a new section.
   *
   * For every other dialect an unclosed header is ::GTEXT_INI_E_BAD_GROUP,
   * which is what the three specifications that discuss it say. Reaching the
   * entry path instead is only coherent for a dialect that also accepts a line
   * with no separator, so this travels with
   * ::GTEXT_INI_Dialect::valueless_keys.
   */
  bool unclosed_header_is_line;

  /**
   * Whether one matching pair of surrounding `"` or `'` is removed from a value.
   *
   * True for ::GTEXT_INI_DIALECT_WIN32, whose documentation says so - "if the
   * string associated with lpKeyName is enclosed in single or double quotation
   * marks, the marks are discarded" - and whose behaviour under wine agrees.
   *
   * **This is a wrapper, and ::GTEXT_INI_Dialect::quoted_values is a toggle.**
   * They are not two settings of one idea and the measurements are what separate
   * them: git's `k = x" mid "y` is `x mid y`, while Win32's is `x" mid "y`
   * unchanged, because neither end is a quote. Exactly one pair comes off
   * (`""x""` is `"x"`), both ends must be present (`"x` and `x"` are
   * themselves), and the two ends must be the same character (`"x'` is
   * unchanged). The strip happens after the value is trimmed, so `"  x  "`
   * keeps its spaces - which is the only way to spell a value with a leading or
   * trailing blank in this dialect.
   */
  bool strip_wrapping_quotes;

  /**
   * Whether a lookup searches every group of a name or only the first.
   *
   * True for every dialect but ::GTEXT_INI_DIALECT_WIN32, and that is `GKeyFile`'s
   * behaviour: it merges repeated group headers, so a key under the second `[G]`
   * is found under `G`. The tree keeps the two groups apart either way, so this
   * changes what a lookup answers and never what the document says.
   *
   * **Win32 does not merge**, and it is measured twice over: a key in a second
   * `[a]` is not retrievable at all, while `GetPrivateProfileSectionNames` still
   * lists that section - so the duplicate is visible and its contents are not.
   * The same rule is what makes a `[]` section unreachable, because the empty name
   * finds the preamble and stops there.
   *
   * It is a field rather than a consequence of
   * ::GTEXT_INI_Dialect::allow_duplicate_groups because three dialects allow
   * duplicates and disagree about this: generic and git merge, Win32 does not.
   */
  bool merge_duplicate_groups;

  /** What a second entry with an existing key in one group means. */
  GTEXT_INI_Dupkey_Mode dupkey;

  /** Which grammar group and key names follow. See ::GTEXT_INI_Name_Style. */
  GTEXT_INI_Name_Style name_style;

  /**
   * Whether `key[LOCALE]` is recognized as a localized spelling of `key`.
   *
   * When true the postfix is kept in the stored key - the tree holds what the
   * document said - and gtext_ini_group_get_locale() does the matching.
   * ::GTEXT_INI_Dialect::require_unlocalized_key is the rule that a postfixed
   * key may not appear without its bare form.
   */
  bool locale_postfix;

  /**
   * Whether a `key[LOCALE]` entry requires a plain `key` in the same group.
   *
   * Desktop Entry §5 requires it, and only `desktop-file-validate` enforces
   * it; GLib accepts an orphan. True for Desktop Entry.
   */
  bool require_unlocalized_key;

  /** Whether CRLF is accepted as a line terminator as well as LF. */
  bool accept_crlf;

  /**
   * Whether a **lone CR** is a line terminator in its own right.
   *
   * True only for systemd, and measured rather than read - `systemd.syntax(7)`
   * does not mention it. `Environment=W1\rEnvironment=W2` yields two settings, and
   * `Environment=W1\rW2` reports the second half's fault on **line 7**, so the
   * line counter advanced. Every other dialect here treats a lone CR as data: to
   * Desktop Entry it is part of the value, to git and EditorConfig it is
   * whitespace.
   *
   * Independent of ::accept_crlf, which only says whether a CR *before an LF* is
   * part of that terminator.
   */
  bool lone_cr_terminates;

  /**
   * Which bytes count as whitespace. See ::GTEXT_INI_Space_Set, which has the
   * measurement behind each of the three sets.
   *
   * This was a `bool` meaning "`isspace()` rather than space and tab" until
   * configparser needed a third set, and the bool's own documentation had already
   * said what was wrong with it: "two bytes is the whole difference". It is four
   * more bytes for the third set, and a flag cannot hold three values.
   */
  GTEXT_INI_Space_Set space_set;

  /**
   * Whether trailing whitespace is stripped from a value.
   *
   * **False for Desktop Entry**, and this is the one place the specification
   * is silent and a reference had to settle it. §3.3 says only that "space
   * before and after the equals sign should be ignored", so the leading run is
   * gone either way; GLib *keeps* the trailing run, and this module keeps it
   * too, so that a value and its round trip agree with the reference.
   * @ref format_ini has the reproduction.
   */
  bool trim_trailing_space;

  /**
   * Whether a UTF-8 byte-order mark at the start of input is skipped.
   *
   * False for Desktop Entry: both references refuse a document that begins
   * with one, so accepting it would be this module reading a document no
   * reference reads. It is one of the generic dialect's relaxations.
   */
  bool skip_bom;

  /**
   * The escape sequences a value may contain, as the set Desktop Entry §4
   * defines: `\s`, `\n`, `\t`, `\r`, `\\`. NULL means the dialect defines no
   * escapes, and gtext_ini_value_string() is then a copy.
   *
   * A static string; not owned. Each character in it is the letter *after* the
   * backslash. Only single-letter sequences: the numeric forms are
   * ::numeric_escapes, because a letter set cannot express a variable-length one.
   */
  const char * escapes;

  /**
   * Whether the **parse** applies the escape set, refusing a sequence that is not
   * in it, or whether a backslash is just a byte until a caller decodes one.
   *
   * **True for git config alone**, and measured: `git config -f` exits 128 on a
   * file containing `k = a\qb`, because git unescapes while it reads. Everything
   * else here leaves it to gtext_ini_unescape() - Desktop Entry because
   * `g_key_file_load_from_data()` accepts such a file and only
   * `g_key_file_get_string()` refuses it, and systemd because `config_parse()`
   * hands the raw value to the setting's own parser and never looks at a
   * backslash. So a systemd unit carrying `ExecStart=/bin/foo \q` parses, and the
   * complaint arrives where a caller asked a question that depends on it.
   *
   * It does not change where a *continuation* is recognised: that is the line's
   * structure rather than a value's content, and
   * ::GTEXT_INI_Dialect::continuation governs it for every dialect.
   */
  bool escapes_in_grammar;

  /**
   * Whether `\xHH`, `\nnn` (octal), `\uNNNN` and `\UNNNNNNNN` are escapes too.
   *
   * systemd's set, and the first variable-length escapes in this module: two hex
   * digits, **exactly three** octal digits, four and eight hex digits, with the
   * last two producing multi-byte UTF-8. Measured as bytes against systemd 257 -
   * `\x41` and `\101` are both `A`, `\u00e9` is two bytes and `\U0001F600` is
   * four.
   *
   * **A malformed one is an error**, exactly as an unknown escape *letter* is:
   * `\10`, `\400`, `\u41`, `\U00110000`, `\xZZ` and all three spellings of NUL
   * make systemd discard the whole setting.
   *
   * That cost a wrong answer to find, and the wrong answer is worth recording. The
   * first probe extracted the reported value by splitting each diagnostic on
   * `ignoring: ` - a substring that appears in **both** "Invalid environment
   * assignment, ignoring: " (a successfully parsed word) and "Invalid syntax,
   * ignoring: " (the setting refused outright). So a refusal echoing the raw value
   * read exactly like a word that had kept its backslash, and the rule came out
   * backwards. Keeping the message *type* is what separated them.
   */
  bool numeric_escapes;

  /**
   * The characters that may separate a key from its value, as a NUL-terminated
   * string in no particular order. **Never NULL and never empty**, and the first
   * character is the one the writer emits for a synthesized entry.
   *
   * `"="` for five of the six dialects and `"=:"` for configparser, whose option
   * pattern is `(?P<vi>=|:)`. Which one appears in a given line does not matter:
   * the key ends at whichever comes **first**, so `k=b:c` has the value `b:c` and
   * `k:b=c` has the value `b=c`. Measured both ways.
   *
   * It is a set rather than a single character because it has to be asked of one
   * byte at a time in two places - finding where a key ends, and deciding whether
   * a byte may appear *in* a key - and those two must not be able to disagree.
   */
  const char * separators;

  /**
   * The list separator for gtext_ini_value_list(), or 0 if the dialect has no
   * list spelling. `';'` for Desktop Entry §4.
   */
  char list_separator;

  /**
   * Whether a value splits into **words** on unquoted whitespace, with `"` and
   * `'` quoting and escapes decoded inside both.
   *
   * systemd's spelling of a list, and the reason it is a separate axis from
   * ::list_separator: there is no delimiter character to name. `systemd.syntax(7)`
   * is explicit that this applies only "for settings where quoting is allowed",
   * which the grammar cannot know because it depends on the setting - so the
   * parser never does it and gtext_ini_value_words() is where a caller asks.
   *
   * Measured against systemd 257, and three of the rules are not in the manual:
   * quoting is a **toggle, not a wrapper** (`x"y z"` is the one word `xy z`,
   * `a"b c"` is `ab c`), **escapes are processed inside single quotes too**
   * (unlike a shell), and an empty quoted run produces an **empty word**. An
   * unclosed quote is an error.
   */
  bool word_split;

  /** Which spellings gtext_ini_value_bool() accepts. See ::GTEXT_INI_Bool_Style. */
  GTEXT_INI_Bool_Style bool_style;

  /**
   * Whether a logical line may span several physical lines, and how.
   *
   * A dialect with a continuation rule changes what "the raw value" is: the
   * stored bytes then contain the backslashes and the line terminators they
   * span, because those bytes are in the document and @ref format_ini's
   * byte-identical rewrite requirement does not exempt them. The join happens
   * in ini_value.h with every other decoding step.
   */
  GTEXT_INI_Continuation_Mode continuation;

  /**
   * Whether `#` and `;` begin a comment **anywhere on a line**, not only at
   * its start.
   *
   * git config is the one specified dialect here that has them, and they are
   * what makes its value scan structural rather than a byte range: `k = v # c`
   * has the value `v`, and `k = "v # c"` has the value `v # c`, so the parser
   * cannot find where a value ends without tracking the quoting. Measured: no
   * space is needed either side, `k = v#c` is also `v`.
   *
   * Which introducers count is still ::comment_hash and ::comment_semicolon, so
   * a dialect with inline comments and only one introducer is expressible.
   */
  bool inline_comments;

  /**
   * Whether `"` toggles a quoted run inside a value.
   *
   * Quoting in git config is a **toggle, not a wrapper**, which is the rule
   * most easily got wrong: measured, `k = x" mid "y` is `x mid y` and
   * `k = "a"b` is `ab`, so a reader that required the value to begin and end
   * with a quote would refuse two documents git accepts. Inside a run,
   * whitespace and the comment introducers are ordinary bytes; a run left open
   * at the end of a logical line is an error.
   */
  bool quoted_values;

  /**
   * Whether **key** names are matched without regard to case.
   *
   * git config folds both to lower case, and **does not fold a quoted
   * subsection name** - so `[a "SubB"]` and `[a "subb"]` are different groups
   * while `[Core]` and `[core]` are one. The tree keeps every name as the
   * document spelled it and carries the folded form beside it for lookup, so a
   * rewrite is still byte-identical.
   *
   * Group names are ::fold_group_case, because two dialects fold one and not the
   * other.
   *
   * The fold is ASCII, and for configparser that is a **stated divergence**:
   * `optionxform` is Python's `str.lower()`, which is Unicode-aware, so `KE` with
   * an acute accent folds there and `I` with a dot above folds to two characters.
   * @ref format_ini names it, and the differential excludes such documents and
   * counts them.
   */
  bool fold_case;

  /**
   * Whether **group** names are matched without regard to case as well.
   *
   * True for git config alone, and the split is not a nicety: EditorConfig and
   * configparser both fold keys and both leave section names exactly as written.
   * EditorConfig's section name is a filepath glob, and whether two spellings of
   * a path are one file is the filesystem's question rather than the format's;
   * configparser simply uses the name as a dictionary key, and `[A]` and `[a]`
   * are two sections - measured.
   *
   * This was derived from ::fold_case by excluding a name style, which worked
   * while EditorConfig was the only exception and became a list of exceptions in
   * shared code the moment configparser arrived. gtext_ini_group_names_fold()
   * reads this field now and tests no id at all.
   */
  bool fold_group_case;

  /**
   * Whether a group header may carry a subsection: `[section "sub"]`, and the
   * deprecated `[section.sub]`.
   *
   * Three rules travel with it, each measured against git:
   *
   *   - The quoted name is **case-sensitive** and may hold any byte but a line
   *     terminator - `]`, `[`, `#` and `;` included.
   *   - Inside it a backslash **drops**: `\t` is the letter `t` and `\"` is a
   *     quote. This is the same file's *second* escape layer, and the layer is
   *     chosen by position rather than by any marker - `\t` in a value is a
   *     tab on the very next line.
   *   - The dotted form lower-cases the subsection, so `[a.SubB]` and
   *     `[a "subb"]` name the same group and `[a "SubB"]` does not.
   *
   * The whole header must be `[name]`, `[name "sub"]` or `[name.sub]` and
   * nothing else: `[a"b"]` with no space is refused, and so is `[a "b" ]`.
   */
  bool subsection_syntax;

  /**
   * Whether a key with no `=` at all is a legal entry with **no value**.
   *
   * git's shorthand for boolean true, and the reason the tree has to tell an
   * absent value from an empty one: `k` and `k =` are both legal and are not
   * the same entry. gtext_ini_group_value_present_at() is that distinction,
   * because a NULL from gtext_ini_group_get() would otherwise mean two things.
   *
   * Measured: a valueless key may **not** carry a trailing comment - `k ; c`
   * is refused, though `[a] ; c` on a header line is fine. Nothing in the
   * manual page says so.
   *
   * **It used to be meaningful only with a closed key charset**, and that is no
   * longer true - the note is kept because the reasoning was sound and the
   * conclusion was about the code rather than about the format. A dialect whose
   * keys may hold any byte has to find the `=` before it knows where the key
   * ended, so the flag was dead in that branch: setting it true on the
   * EditorConfig dialect changed nothing at all, which a mutation found by
   * flipping it and watching both gates stay green.
   *
   * ::GTEXT_INI_DIALECT_WIN32 has an open key charset **and** valueless entries -
   * measured, `novalue` alone is an entry that `GetPrivateProfileSectionA`
   * reports and `GetPrivateProfileStringA` cannot see - so the open-charset
   * branch now honours the flag by taking the whole trimmed line as the key.
   * For ::GTEXT_INI_NAMES_EDITORCONFIG and ::GTEXT_INI_NAMES_DESKTOP_ENTRY
   * nothing still reads it, and the mutation that proved that is still in the
   * table.
   */
  bool valueless_keys;

  /**
   * Whether a value must be well-formed UTF-8 for gtext_ini_unescape() to hand
   * it over.
   *
   * True for Desktop Entry, and measured rather than assumed on both sides:
   * `g_key_file_get_string()` refuses a value containing a bare `0xFF` that
   * `g_key_file_load_from_data()` accepted, while `git config --get` hands the
   * same bytes back unchanged. So this is not a question about the format's
   * character set - both dialects *parse* such a document - but about what the
   * reference's string accessor promises, and the two references disagree.
   *
   * The parser never consults it. Validating during a parse is what neither
   * reference does, and Desktop Entry §3.1 permits any byte but LF in a comment.
   */
  bool utf8_values;

  /**
   * What anything but whitespace after a group header's `]` means. See
   * ::GTEXT_INI_Header_Remainder.
   *
   * A `bool` until configparser, which neither refuses such a line nor reads an
   * entry from it: it discards the remainder and keeps the section.
   */
  GTEXT_INI_Header_Remainder header_remainder;
} GTEXT_INI_Dialect;

/**
 * @brief The Desktop Entry dialect: specification 1.5, 2020-04-27.
 *
 * `#` comments only, no leading whitespace, no preamble, no duplicate group,
 * no duplicate key, `A-Za-z0-9-` keys with a `[LOCALE]` postfix that requires
 * its bare form, LF only, trailing whitespace kept, no BOM, escapes `snrt\`
 * and `;` as the list separator.
 *
 * @return The dialect, by value.
 */
GTEXT_API GTEXT_INI_Dialect gtext_ini_dialect_desktop_entry(void);

/**
 * @brief Desktop Entry with six relaxations and one normalisation.
 *
 * The six relaxations: `;` comments, leading whitespace, a preamble, duplicate
 * groups, duplicate keys (::GTEXT_INI_DUPKEY_LAST_WINS), and any key charset.
 * The normalisation: CRLF is a line terminator, and a leading BOM is skipped.
 * Nothing else differs - the escape set and the list separator are Desktop
 * Entry's.
 *
 * Its correctness claim is inherited rather than asserted: **every document the
 * Desktop Entry dialect accepts, and that contains no CR, parses identically
 * through this one**, which is gated over the same corpus. The CR proviso is the
 * normalisation: a document with a CRLF on an entry line reads one byte shorter
 * here, on purpose.
 *
 * @return The dialect, by value.
 */
GTEXT_API GTEXT_INI_Dialect gtext_ini_dialect_generic(void);

/**
 * @brief The git config dialect, as `git-config(1)` "Syntax" defines it.
 *
 * `#` and `;` comments **anywhere on a line**, a preamble, subsections in both
 * spellings, case-folded section and key names with case-sensitive quoted
 * subsections, valueless keys, repeated keys as a list, an entry after the
 * header on the same line, backslash continuation joining with nothing, quoted
 * runs that toggle, and the escape set `\"`, `\\`, `\n`, `\t`, `\b` and
 * nothing else.
 *
 * It is not built by relaxing Desktop Entry, and could not be: it refuses
 * documents Desktop Entry accepts as well as accepting documents Desktop Entry
 * refuses. @ref format_ini has the table both ways round, and the differential
 * against git itself is `make check-ini-git-oracle`.
 *
 * **What it deliberately does not do** is interpret a value's type. git's
 * `--type=bool`, `--type=int` with its `k`/`m`/`g` suffixes and `--type=path`
 * are its porcelain's rules, not its file grammar's, and a valueless key being
 * boolean true is the one of those that reaches the grammar - which is why it
 * is spelled as an absent value here and left for the caller to read as true.
 *
 * @return The dialect, by value.
 */
GTEXT_API GTEXT_INI_Dialect gtext_ini_dialect_git_config(void);

/**
 * @brief The EditorConfig dialect, as specification 0.17.2 defines it.
 *
 * `#` and `;` comments at the start of a line only, leading and trailing
 * whitespace removed from the line **before** it is classified, a preamble,
 * duplicate groups and duplicate keys with the last winning, case-insensitive
 * keys with case-sensitive section names, section names holding any byte and
 * closing at the last `]`, keys holding anything but `=`, no escapes, no
 * quoting, no continuation, no list, CRLF accepted and a BOM skipped.
 *
 * Like git config it is **not** built by relaxing Desktop Entry: it accepts a
 * preamble, a duplicate group, a duplicate key and a section name Desktop Entry
 * refuses, and refuses the `\n` escape and the `;`-separated list Desktop Entry
 * defines. Building it as a relaxation would also inherit the next field added
 * to Desktop Entry's constructor, whose default here would be a guess.
 *
 * **Its correctness claim is the only one in this module that is a pass count
 * rather than an agreement.** EditorConfig has a normative conformance suite -
 * "a conforming core or plugin must pass the tests in the core-tests
 * repository" - and `make conformance-ini-editorconfig` runs the 34 of its 202
 * assertions that test the grammar. The other 168 test a filepath glob matcher,
 * file discovery and a command line, none of which is a text library's job.
 *
 * The two reference cores are used as differential oracles instead, and
 * **both of them score 33 of those 34**: each strips a whitespace-preceded `#`
 * from a value, where the specification says a `#` "anywhere other than at the
 * beginning of a line does *not* start a comment". They fail it for one shared
 * reason - both descend from Python's `ConfigParser` - so this is not two
 * independent readings agreeing, and @ref format_ini says so where the number
 * is quoted.
 *
 * **What it deliberately does not do** is interpret a value. `indent_size`,
 * `tab_width`, the `unset` value and the lower-casing that cores apply to six
 * known property names are EditorConfig's *properties*, not its file grammar,
 * and they belong to whatever reads the document.
 *
 * @return The dialect, by value.
 */
GTEXT_API GTEXT_INI_Dialect gtext_ini_dialect_editorconfig(void);

/**
 * @brief The systemd dialect, as `systemd.syntax(7)` defines it.
 *
 * `#` and `;` comments at the start of a line, no preamble, duplicate sections
 * merged, repeated keys collected as a list, case-**sensitive** keys, section and
 * key names that may hold a space, a backslash continuation that **joins with a
 * space** and skips an intervening comment block, the full C escape set including
 * the four numeric forms, and a lone CR as a line terminator.
 *
 * Not built by relaxing anything, like the two dialects before it. Three things
 * are unique to it in this module:
 *
 *   - **The continuation is a property of the line, not of the value.** It works
 *     on a group header and on a key, because systemd assembles the logical line
 *     before classifying it. So a group's or an entry's stored name is the
 *     document's own bytes and the **joined form is the canonical form** - the
 *     same two-form storage case folding already needed.
 *   - **Variable-length escapes**, and a malformed one is data rather than an
 *     error. See ::GTEXT_INI_Dialect::numeric_escapes.
 *   - **Quoting is not in the grammar.** The specification says so itself, which
 *     is the layering claim @ref format_ini makes stated by somebody else, so
 *     ::GTEXT_INI_Dialect::quoted_values is false and gtext_ini_value_words() is
 *     where a caller splits one.
 *
 * **What it deliberately does not do** is interpret a value's type beyond
 * booleans. systemd's time spans belong to `ghoti.io-chron`, whose duration type
 * already covers them; its sizes and its `%`-specifiers - which need the unit name
 * and the host - are not a text parser's to know. @ref format_ini says so for each.
 *
 * **Where it departs from systemd on purpose:** systemd warns about a malformed
 * line and *skips* it, keeping the rest of the file; this refuses the document.
 * A reader that silently dropped a setting would be the worse failure for a
 * library whose caller cannot see the warning, and @ref format_ini records what
 * the differential does about the difference.
 *
 * @return The dialect, by value.
 */
GTEXT_API GTEXT_INI_Dialect gtext_ini_dialect_systemd(void);

/**
 * @brief The Python `configparser` dialect, as CPython reads a file.
 *
 * `#` and `;` comments, **`=` or `:`** as the separator, no preamble, no
 * duplicate section and no duplicate key, keys lower-cased and sections not,
 * quotes and backslashes literal, inline comments off, a valueless key refused,
 * and a continuation **by indentation that joins with a newline**.
 *
 * **Its specification is an implementation**, which makes it the one dialect here
 * whose every rule had to be measured and none of which can be cited. Four of
 * those measurements are the reason the dialect struct grew:
 *
 *   - **Two separator characters**, whichever comes first
 *     (::GTEXT_INI_Dialect::separators).
 *   - **A third whitespace set**: Python's `\s` includes `\x1c` through `\x1f`
 *     and `isspace()` does not (::GTEXT_INI_SPACE_PYTHON).
 *   - **A third answer for what follows a header's `]`**: discard it
 *     (::GTEXT_INI_HEADER_REMAINDER_IGNORE).
 *   - **Folded keys with unfolded sections**, which EditorConfig also needs and
 *     which used to be spelled by excluding a name style
 *     (::GTEXT_INI_Dialect::fold_group_case).
 *
 * **Three things it does not implement, each for a stated reason**, and
 * @ref format_ini has the counts behind them:
 *
 *   - **Interpolation.** `%(name)s` and `${section:key}` appear in **no file** on
 *     this machine, while a bare `%` appears in three - and a bare `%` is exactly
 *     what configparser's *default* interpolation refuses. Shipping the default
 *     form would only break files that are otherwise fine, so values come back
 *     raw and the differential pins `interpolation=None`.
 *   - **The `[DEFAULT]` section's inheritance.** It is a lookup policy over a
 *     parsed tree rather than a rule of the grammar: a caller wanting it asks the
 *     section and then asks `DEFAULT`. `[DEFAULT]` is an ordinary group here.
 *   - **Unicode-aware whitespace and case folding**, which are properties of
 *     Python's `str` and not of the format. Both are named divergences with a
 *     test each rather than gaps.
 *
 * **A lone CR terminates a line here**, and that is a choice between two
 * behaviours of the reference rather than a reading of it: `read(path)` gets
 * Python's universal-newline translation and `read_string()` does not, so they
 * disagree about that one byte and about nothing else. A file is what an INI
 * document is, so this follows the file.
 *
 * @return The dialect, by value.
 */
GTEXT_API GTEXT_INI_Dialect gtext_ini_dialect_configparser(void);

/**
 * The Win32 profile API's dialect, measured rather than read.
 *
 * `GetPrivateProfileString` has documentation and no grammar: two sentences
 * about quoting and case, and nothing about lines, headers, comments or
 * duplicates. So every rule here came from a probe under wine, and the probe is
 * `tools/oracle/containers/win32/`.
 *
 * **What it is for.** Not to reproduce the API's answer - it cannot, and neither
 * can any reader of the file, because `IniFileMapping` may redirect a section
 * to the registry. It is for the `.ini` files that exist on Windows, which
 * overwhelmingly belong to applications that parse the file themselves. Against
 * those, this is the best-documented reading of the format available.
 *
 * **The rules, each measured:**
 *
 * - Lines end at LF, CRLF **or a lone CR**, and blank lines are ignored.
 * - The whitespace set is `isspace()`'s, not blank-only: VT and FF are trimmed,
 *   and `\x1c`-`\x1f` are not.
 * - A header is a line whose first non-blank byte is `[` and which holds a `]`.
 *   The name runs to the **last** `]`, is then **trimmed**, and anything after
 *   that `]` is discarded - `[a]junk` is `a`, `[a]]junk` is `a]`.
 * - `[]` is a legal header whose name is empty. A lookup for the empty name
 *   finds the preamble instead, so a `[]` section's entries are unreachable by
 *   name; gtext_ini_group_is_preamble() is how a caller tells the two apart.
 * - A `[` line with no `]` is not a header. It is a valueless entry.
 * - `=` is the only separator and the **first** one on the line splits it:
 *   `k=a=b` is `a=b`.
 * - Key and value are both trimmed; one matching pair of surrounding quotes
 *   then comes off the value.
 * - A line with no `=` is a valueless entry, which `GetPrivateProfileSection`
 *   reports and `GetPrivateProfileString` cannot retrieve.
 * - Keys and group names both fold case, **ASCII only**: measured, `[\xe9]` and
 *   `[\xc9]` are two sections. This is the one dialect in this module whose case
 *   rule a byte-oriented reader can implement exactly.
 * - The first of two duplicate keys wins, and the first of two duplicate
 *   sections does; a later duplicate section is unreachable by lookup and is
 *   still listed by `GetPrivateProfileSectionNames`.
 * - There are no escapes and no continuations. A trailing backslash is data.
 *
 * **The one stated divergence, and it is measured rather than chosen:** `;`
 * introduces a comment here. `GetPrivateProfileSection` agrees and
 * `GetPrivateProfileString` does not, so the two halves of the reference
 * disagree and this follows the half that does not hand back a commented-out
 * setting. `#` is a comment to neither API and is an ordinary byte here, which
 * is the other half of the same measurement and is the row most readers of this
 * format get wrong in the opposite direction.
 *
 * @return The dialect by value.
 */
GTEXT_API GTEXT_INI_Dialect gtext_ini_dialect_win32(void);

/**
 * @struct GTEXT_INI_Parse_Options
 * @brief What a parse is allowed to do.
 *
 * Zeroed is not the default: use gtext_ini_parse_options_default() and change
 * what you mean to change. A zeroed struct carries a zeroed dialect, which
 * names no comment character at all.
 */
typedef struct {
  /**
   * The allocator every allocation of the parse goes through, or NULL for
   * gtext_allocator_default(). `make check-allocators` keeps this true.
   */
  const GTEXT_Allocator * allocator;

  /** The rules to read by. Default gtext_ini_dialect_desktop_entry(). */
  GTEXT_INI_Dialect dialect;

  /**
   * Maximum bytes of input, or 0 for no limit. Applied by the file reader
   * while reading, so an over-large file is refused without being held.
   */
  size_t max_total_bytes;

  /**
   * Maximum groups, or 0 for no limit. Default 0: an INI document is flat, so
   * there is no recursion to bound and a count limit would only be a policy.
   */
  size_t max_groups;

  /**
   * Maximum entries per group, or 0 for no limit. Default 0, for the same
   * reason as ::max_groups.
   */
  size_t max_entries_per_group;

  /**
   * Whether comments and blank lines are kept on the tree. **Default true**,
   * which is the opposite of the TOML module's choice, and the reason is the
   * specification rather than taste: Desktop Entry §3 requires that a
   * compliant implementation not drop what it does not understand and that
   * comments "should be preserved across reads and writes". A default that
   * discarded them would make the default configuration unable to rewrite a
   * file correctly.
   *
   * Setting it false costs an allocation per comment and makes
   * gtext_ini_write() emit a document with no comments, which for this dialect
   * is a deliberate act rather than an oversight.
   */
  bool retain_comments;
} GTEXT_INI_Parse_Options;

/**
 * @brief The defaults: allocator NULL, Desktop Entry, no limits, comments kept.
 *
 * @return The defaults, by value.
 */
GTEXT_API GTEXT_INI_Parse_Options gtext_ini_parse_options_default(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_INI_INI_CORE_H
