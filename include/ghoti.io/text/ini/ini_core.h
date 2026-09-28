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
 * gtext_ini_group_get_all_at() reaches them under every mode.
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
   * them with gtext_ini_group_get_all_at(). Lookup answers the first, so that
   * a caller who forgets still gets document order rather than a surprise.
   */
  GTEXT_INI_DUPKEY_COLLECT
} GTEXT_INI_Dupkey_Mode;

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
  GTEXT_INI_DIALECT_GENERIC
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

  /**
   * Whether names follow Desktop Entry's spelling rules.
   *
   * When true, a key name is restricted to `A-Za-z0-9-` plus an optional
   * `[LOCALE]` postfix (§3.3) and a group name to ASCII (§3.2). When false a
   * key may be any bytes but `=` and a line terminator, and a group name any
   * bytes but `[`, `]` and a control character.
   *
   * One field rather than two because no dialect here relaxes one and not the
   * other: both restrictions are §3's, and both are what the generic dialect
   * drops. A second field nothing sets differently would be an axis with no
   * point of use.
   */
  bool strict_key_charset;

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
