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
  GTEXT_INI_NAMES_EDITORCONFIG
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
  GTEXT_INI_CONTINUATION_JOIN_EMPTY
} GTEXT_INI_Continuation_Mode;

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
  GTEXT_INI_DIALECT_EDITORCONFIG
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
   * Whether the dialect's whitespace is `<ctype.h>`'s `isspace()` set rather
   * than just space and tab - so a vertical tab and a form feed are whitespace
   * too.
   *
   * **True only for EditorConfig, and false for git config on purpose.** git
   * carries its own ctype table in which `\v` and `\f` are control characters,
   * measured: `k = a\v` keeps the vertical tab as the value's last byte and
   * `\vk = v` is a syntax error rather than skipped indentation. Both
   * EditorConfig cores reach for the platform's answer instead - core-c calls
   * `isspace()` and core-py uses Python's `\s` - so under that dialect the same
   * two bytes are trimmed away.
   *
   * Two bytes is the whole difference, and no corpus of real files contains
   * either, which is exactly why it is a field rather than an assumption.
   *
   * The line terminator is never in this set: an LF ends the line before any
   * trimming happens, and a CR is ::accept_crlf's.
   */
  bool ctype_whitespace;

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
   * backslash.
   */
  const char * escapes;

  /**
   * The list separator for gtext_ini_value_list(), or 0 if the dialect has no
   * list spelling. `';'` for Desktop Entry §4.
   */
  char list_separator;

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
   * Whether group and key names are matched without regard to case.
   *
   * git config folds both to lower case, and **does not fold a quoted
   * subsection name** - so `[a "SubB"]` and `[a "subb"]` are different groups
   * while `[Core]` and `[core]` are one. The tree keeps every name as the
   * document spelled it and carries the folded form beside it for lookup, so a
   * rewrite is still byte-identical.
   */
  bool fold_case;

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
   * **It is only meaningful with a closed key charset**, and that coupling is
   * worth stating because the flag otherwise looks independent. A dialect whose
   * keys may hold any byte has to find the `=` before it knows where the key
   * ended, so a line with no `=` is a bad line before there is a key to call
   * valueless - which is why setting this true on the EditorConfig dialect
   * changes nothing at all. Found by mutation: the flag flipped and both gates
   * stayed green, because for ::GTEXT_INI_NAMES_EDITORCONFIG and
   * ::GTEXT_INI_NAMES_DESKTOP_ENTRY nothing reads it.
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
   * Whether what follows a group header's `]` on the same line is an entry.
   *
   * `git-config(1)` says "all the other lines (and the remainder of the line
   * after the section header) are recognized as setting variables", and it
   * means it: `[a] k = v` sets `a.k`, and `[a] junk` sets a *valueless* `junk`.
   * A dialect without this refuses anything but whitespace after the `]`.
   */
  bool header_remainder_is_entry;
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
