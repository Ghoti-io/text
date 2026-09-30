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
 * @file ini_internal.h
 * @brief Shared internals of the INI module.
 *
 * The ownership rule is uniform and deliberately boring: **the document owns
 * every byte in it, and every stored string is its own allocation from the
 * document's allocator.** Nothing points into the caller's input buffer, so a
 * document outlives the bytes it was parsed from, and there is no per-field
 * "is this owned" flag to get wrong.
 *
 * An entry keeps the original line in five pieces rather than as a copy, so
 * that a byte-identical rewrite costs no duplicated bytes:
 *
 *     line = pre + key + sep + value + eol
 *
 * `pre` is the leading whitespace, `sep` is everything between the key and the
 * value (the whitespace, the `=`, and the whitespace after it), and `eol` is
 * everything after the value including the line terminator. Concatenating the
 * five reproduces the input exactly; an entry a caller creates gets
 * `sep = "="` and `eol = "\n"`. That is why Desktop Entry §3's preservation
 * requirement is met by construction rather than by care.
 *
 * **A continuation dialect does not break that.** Under
 * ::GTEXT_INI_CONTINUATION_JOIN_EMPTY one logical line spans several physical
 * ones, and the five pieces still tile it exactly: `value` then contains the
 * backslashes and the line terminators it spans, and `eol` is measured from the
 * end of the value to the end of the *logical* line. So `value` can contain an
 * LF, which is why the writer's representability check consults the dialect
 * rather than refusing one outright.
 */

#ifndef GHOTI_IO_GTEXT_INI_INI_INTERNAL_H
#define GHOTI_IO_GTEXT_INI_INI_INTERNAL_H

#include <ghoti.io/text/ini.h>
#include <ghoti.io/text/macros.h>
#include <string.h>

/** A counted, owned string. A NULL `data` means absent, not empty. */
typedef struct {
  char * data;
  size_t len;
} ini_str;

/** One `key = value` line. */
typedef struct {
  ini_str key;     ///< As the document spelled it, postfix included.
  /**
   * The folded form used for lookup, or absent when the dialect does not fold.
   *
   * Present rather than folding at lookup time because a lookup must not
   * allocate, and absent rather than always-a-copy because a dialect with no
   * folding would then pay for a second copy of every key. `canon.data` being
   * NULL means "compare ::key directly", which is the only reading.
   */
  ini_str canon;
  ini_str value;   ///< Raw: leading run after `=` removed, nothing decoded.
  ini_str pre;     ///< Leading whitespace of the line.
  ini_str sep;     ///< From the end of the key to the start of the value.
  ini_str eol;     ///< From the end of the value to the end of the line.
  ini_str comment; ///< Comment and blank lines immediately above, verbatim.
  /**
   * Whether ::value is a value at all.
   *
   * False for a valueless key - git's `k` with no `=`, which is its shorthand
   * for boolean true. `value` is then absent and ::sep is too. This is a field
   * rather than `value.data == NULL` because a caller asking
   * gtext_ini_group_get() for a missing key also gets NULL, and one sentinel
   * meaning both would make the two indistinguishable.
   */
  bool has_value;
  /**
   * Whether ::pre, ::sep and ::eol hold the document's own bytes.
   *
   * True for a parsed entry and false for one a caller created, which is what
   * decides whether a non-normalizing write reproduces the line or synthesizes
   * one. Tested rather than inferred from `sep.data`, which is absent for a
   * valueless key that *is* verbatim.
   */
  bool verbatim;
} ini_entry;

/** One `[group]` and the entries under it. */
struct GTEXT_INI_Group {
  ini_str name;
  ini_str canon;    ///< Folded/canonical name, or absent. See ini_entry::canon.
  ini_str comment;  ///< Comment and blank lines immediately above the header.
  ini_str hdr_pre;  ///< Leading whitespace before `[`.
  ini_str hdr_post; ///< From `]` to the end of the line, terminator included.
  ini_entry * entries;
  size_t count;
  size_t capacity;
  /**
   * Whether this group stands for entries that came before any header.
   *
   * git accepts them - measured, `k = v` in a file with no section at all is
   * `k`, and `git-config(1)`'s claim that a variable "must belong to some
   * section" is wrong about its own implementation. Such a group has an empty
   * name, and the writer emits no header for it, so a rewrite does not invent
   * one. The alternative - refusing the document - would mean this module reads
   * fewer git config files than git does.
   */
  bool preamble;
  bool verbatim;    ///< Whether ::hdr_pre and ::hdr_post are the document's.
  struct GTEXT_INI_Document * doc; ///< For the dialect. The document does not
                                  ///< move, so this stays valid when the
                                  ///< group array is reallocated.
};

struct GTEXT_INI_Document {
  const GTEXT_Allocator * alloc;
  GTEXT_INI_Dialect dialect;
  GTEXT_INI_Group * groups;
  size_t count;
  size_t capacity;
  /**
   * The byte-order mark the parse skipped, or absent.
   *
   * Kept because ::GTEXT_INI_Dialect::skip_bom means "not part of the first
   * line", not "throw it away": a document that opens with one and is written
   * back without it is three bytes shorter than it was. Separate from ::leading
   * rather than prepended to it because ::leading is comment text and is gated
   * on GTEXT_INI_Write_Options::emit_comments, and a BOM is not a comment.
   *
   * Found by the git differential, not by the fuzzer, and the reason is worth
   * keeping: fuzz_ini.cpp asserts the byte-identical rewrite under the *strict*
   * dialect, which has skip_bom false and refuses a document beginning with one,
   * so no input it could generate would reach this path. A property asserted
   * under one dialect says nothing about another that relaxes the rule the
   * property depends on.
   */
  ini_str bom;
  ini_str leading;  ///< Before the first group header.
  ini_str trailing; ///< After the last line that belongs to an entry.
  bool synthesized; ///< True when built by gtext_ini_new() rather than parsed.
};

/** Duplicate @p len bytes, NUL-terminating one past the end. */
GTEXT_INTERNAL_API bool gtext_ini_str_set(const GTEXT_Allocator * alloc,
    ini_str * out, const char * bytes, size_t len);

/** Release a string and mark it absent. */
GTEXT_INTERNAL_API void gtext_ini_str_clear(const GTEXT_Allocator * alloc,
    ini_str * s);

/** Fill in @p err, copying no snippet when @p err is NULL. */
GTEXT_INTERNAL_API void gtext_ini_set_error(GTEXT_INI_Error * err,
    GTEXT_INI_Status code, const char * message, const char * input,
    size_t input_len, size_t offset);

/** Whether @p name is a legal group name for @p dialect. */
GTEXT_INTERNAL_API bool gtext_ini_group_name_ok(const GTEXT_INI_Dialect * dialect,
    const char * name, size_t len);

/**
 * Whether @p c may appear anywhere in a key name for @p dialect.
 *
 * Separate from gtext_ini_key_ok() because a *scan* needs to know where a key
 * ends and cannot ask about the whole name: git's rule that the first byte must
 * be a letter would then stop the scan at the `1` of `ab12`, cutting the key
 * short and reading the rest as a syntax error. gtext_ini_key_ok() is this
 * predicate over every byte plus whatever positional rules the dialect adds, so
 * the charset itself is written once.
 */
GTEXT_INTERNAL_API bool gtext_ini_key_char_ok(const GTEXT_INI_Dialect * dialect,
    char c);

/** Whether @p key is a legal key for @p dialect, postfix included. */
GTEXT_INTERNAL_API bool gtext_ini_key_ok(const GTEXT_INI_Dialect * dialect,
    const char * key, size_t len);

/** The offset of the `[` that starts a key's locale postfix, or len. */
GTEXT_INTERNAL_API size_t gtext_ini_key_base_len(const char * key, size_t len);

/** Append an entry to a group without any duplicate or charset check. */
GTEXT_INTERNAL_API ini_entry * gtext_ini_group_push(GTEXT_INI_Group * group);

/**
 * Whether @p c is whitespace to @p dialect.
 *
 * **git's answer is not C's.** git carries its own ctype table in which `\v`
 * and `\f` are control characters and not space, so `k = a\v` keeps the
 * vertical tab as the value's last byte instead of trimming it, and a `\v`
 * where a key should start is a syntax error rather than skipped indentation.
 * Both measured against git 2.47.3. A parser built on `isspace()` differs from
 * git on exactly those two bytes, which no corpus of real files would ever
 * show, so the predicate is a function of the dialect and not of <ctype.h>.
 *
 * **EditorConfig is the dialect that does want C's answer**, because both of its
 * cores ask the platform: core-c calls `isspace()` and core-py matches `\s`.
 * ::GTEXT_INI_Dialect::space_set is that choice, so the dialects with a rule about
 * `\v` can hold opposite ones - and configparser, whose whitespace is Python's
 * `\s` rather than C's, needs a third answer again: four more bytes, `\x1c`
 * through `\x1f`.
 *
 * CR is space to git and not to Desktop Entry, which is what lets a CRLF file
 * read correctly under a dialect that has no CRLF rule of its own.
 */
GTEXT_INTERNAL_API bool gtext_ini_is_space(const GTEXT_INI_Dialect * dialect,
    char c);

/**
 * Whether @p c separates a key from its value under @p dialect.
 *
 * `=` for five dialects and `=` or `:` for configparser. Asked of one byte,
 * because the two places that need it - where a key ends, and whether a byte may
 * appear inside one - would otherwise be free to disagree about `k:v`.
 */
GTEXT_INTERNAL_API bool gtext_ini_is_separator(const GTEXT_INI_Dialect * dialect,
    char c);

/**
 * The canonical, foldable form of a group name, or false when there is none.
 *
 * Writes into @p out, which must have room for @p len bytes. For git this
 * lower-cases the section part and, for the deprecated dotted spelling, the
 * subsection too - but leaves a quoted subsection alone, because git compares
 * that case-sensitively. Used on both the storing and the querying side so that
 * the two cannot drift apart.
 *
 * @param raw The bytes between `[` and `]`, as the document spelled them.
 * @param out Receives the canonical form.
 * @param out_len Receives its length, which is at most @p len.
 * @return False when the name is not a legal group name for the dialect.
 *
 * There is deliberately **no matching function for the query side**. A query is
 * folded in place by ini_dom.c's comparison, byte against byte, because a lookup
 * must not allocate and the rule is not the same one: a header's dotted
 * subsection folds and a query's does not, since a query carries no quotes and
 * cannot say which spelling the caller meant. A `_query` variant of this function
 * was written and then removed when nothing called it - the comparison had done
 * the work inline all along, and a second implementation of a folding rule is
 * exactly what drifts.
 */
GTEXT_INTERNAL_API bool gtext_ini_canon_group(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t len, char * out, size_t * out_len);

/** The canonical form of a key: lower-cased when the dialect folds. */
GTEXT_INTERNAL_API bool gtext_ini_canon_key(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t len, char * out, size_t * out_len);

/** What scanning a structured value found. See gtext_ini_scan_value(). */
typedef struct {
  /**
   * Bytes from the start of the value up to and including its last content
   * byte - what gets stored as the raw value.
   *
   * Trailing whitespace and an inline comment are outside it, and an escape is
   * *inside* it even when it spells a space: measured, `k = a\t` has the value
   * `a\t` and the tab is content, while `k = a\t ` trims only the literal
   * space. So this cannot be computed by trimming the span from the right.
   */
  size_t content_len;
  /** Bytes the logical line occupies, its final terminator excluded. */
  size_t logical_len;
  /** Length of the terminator that ended it: 0 at end of input, 1 or 2. */
  size_t term_len;
  /** A quoted run was still open when the logical line ended. */
  bool open_quote;
  /** A backslash named no escape sequence the dialect has. */
  bool bad_escape;
  /** The offset within the value at which the fault was found. */
  size_t fault_offset;
  /**
   * Whether the scan ran out of input **immediately after a backslash**, so the
   * value's last byte is one that has not yet consumed its line terminator.
   *
   * Deliberately narrower than "ended with a continuation", and the narrowing is
   * the point. A value holding `a\` *and* the newline it joined over is
   * self-contained: re-reading it performs the same join and stops at the next
   * terminator, whatever that is. A value ending in a bare backslash is not - it
   * will pair with whichever terminator is written after it and swallow the line
   * beyond. Only the second is unsafe, and only when something does follow.
   *
   * Setting it for every continuation refused `k = false   \` followed by a
   * comment line - a document the parse itself produces - which is how the
   * distinction got found.
   */
  bool trailing_backslash;
} ini_value_scan;

/**
 * Scan - and optionally decode - a value whose extent the syntax decides.
 *
 * This is the single implementation of git's value grammar, and it is one
 * function because the parser and the value layer must agree about it exactly:
 * the parser calls it to find where the logical line ends, and
 * gtext_ini_unescape() calls it again over the stored bytes to produce the
 * decoded string. Two copies of a rule this fiddly would drift, and the drift
 * would be invisible - the document would parse and give a wrong value.
 *
 * Re-scanning just the stored span is sound because that span ends at a content
 * byte, so nothing that was dropped can affect what is kept.
 *
 * @param dialect The rules. ::GTEXT_INI_Dialect::quoted_values,
 *   ::GTEXT_INI_Dialect::inline_comments and
 *   ::GTEXT_INI_Dialect::continuation all steer it.
 * @param raw The first byte of the value.
 * @param len Bytes available, to the end of the input.
 * @param out Receives the decoded bytes, or NULL to measure only. Needs room
 *   for @p len bytes: decoding only ever shrinks.
 * @param out_len Receives the decoded length. May be NULL.
 * @param scan Receives the measurements. May not be NULL.
 * @return GTEXT_INI_OK, or the refusal @p scan describes.
 */
GTEXT_INTERNAL_API GTEXT_INI_Status gtext_ini_scan_value(
    const GTEXT_INI_Dialect * dialect, const char * raw, size_t len,
    char * out, size_t * out_len, ini_value_scan * scan);

/** Whether @p dialect decides a value's extent by scanning its syntax. */
GTEXT_INTERNAL_API bool gtext_ini_dialect_scans_values(
    const GTEXT_INI_Dialect * dialect);

/**
 * What a line continuation at @p at consumes, if there is one there.
 *
 * **One implementation, three callers**, and that is the point: the parser needs it
 * to find where a logical line ends, the value scanner needs it to join a value,
 * and the canonical-name step needs it to join a name. systemd's continuation works
 * on a group header and on a key as well as in a value, because systemd assembles
 * the logical line before classifying it - so a second copy of "what is a
 * continuation" would be a second place for the comment-block rule to drift.
 */
typedef struct {
  /**
   * Bytes consumed from @p at: the backslash, its terminator, and any comment
   * lines skipped over. Zero when there is no continuation here.
   */
  size_t span;
  /**
   * Whether the logical line **ends** after those bytes rather than continuing.
   *
   * True when the next physical line is blank or the input has run out. Measured:
   * a blank line ends a systemd continuation instead of being skipped, and the
   * trailing backslash then simply disappears - exactly as one at end of input
   * does. The blank line itself is left unconsumed, so the parser handles it where
   * it handles every other blank line.
   */
  bool ends_line;
} ini_continuation;

GTEXT_INTERNAL_API ini_continuation gtext_ini_continuation_at(
    const GTEXT_INI_Dialect * dialect, const char * bytes, size_t len,
    size_t at);

/**
 * How many bytes of line terminator sit at @p at, or 0 if none does.
 *
 * A lone CR is a terminator only for systemd - ::GTEXT_INI_Dialect::lone_cr_terminates,
 * measured rather than read, since `systemd.syntax(7)` does not mention it. For
 * every other dialect a CR counts only as the first byte of a CRLF.
 */
GTEXT_INTERNAL_API size_t gtext_ini_terminator_len(
    const GTEXT_INI_Dialect * dialect, const char * bytes, size_t len,
    size_t at);

/**
 * Whether a group header closes at the **last** `]` on the line, not the first.
 *
 * True for EditorConfig, whose section names "may contain any characters between
 * the square brackets" - so `[a]b]` is one section named `a]b`, and a scan for
 * the first `]` would cut it in the wrong place. Both cores do this: core-c has
 * `find_last_char_or_comment` and core-py's greedy `[^\#;]+` backtracks to the
 * same place. True for configparser too, which arrives at it from the other
 * direction: its header pattern is a greedy `.+` between the brackets, so the
 * match lands on the last `]` whether anyone intended that or not.
 *
 * Asked as a capability rather than as `id == GTEXT_INI_DIALECT_EDITORCONFIG`,
 * for the reason gtext_ini_dialect_scans_values() gives: a `== SOME_DIALECT` test
 * in shared code is a bug waiting for the second dialect that needs the same
 * behaviour.
 */
GTEXT_INTERNAL_API bool gtext_ini_group_close_is_last(
    const GTEXT_INI_Dialect * dialect);

/**
 * Whether ::GTEXT_INI_Dialect::fold_case reaches **group** names as well as keys.
 *
 * It does not always, and the three dialects that fold disagree about which half:
 * git folds a section name and not a quoted subsection, EditorConfig and
 * configparser fold a key and not a section name at all. EditorConfig's
 * specification is explicit - "pair keys are case-insensitive; all keys are
 * lowercased after parsing" says nothing about a section, and a section is a
 * filepath glob whose case significance is the filesystem's question rather than
 * the format's; configparser's section name is a dictionary key `optionxform`
 * never sees, so `[A]` and `[a]` are two sections.
 *
 * So `fold_case` alone cannot answer "does this group name have a canonical
 * form", and every caller that wants to know asks this instead. It reads
 * ::GTEXT_INI_Dialect::fold_group_case and tests no name style: it used to be
 * `fold_case` minus EditorConfig, which became a list of exceptions the moment a
 * second dialect wanted the same thing.
 */
GTEXT_INTERNAL_API bool gtext_ini_group_names_fold(
    const GTEXT_INI_Dialect * dialect);

/**
 * Whether a **name** may contain a continuation, so that its canonical form is
 * the joined one.
 *
 * True for systemd alone, and it is not the same question as "does the dialect
 * have a continuation". systemd assembles the logical line before classifying it,
 * so `[Serv\` + `ice]` is a section and `Environ\` + `ment=v` a key;
 * configparser's continuation is the *next* line's indentation, which can only
 * ever extend a value, because an entry has to exist before a line can be more
 * indented than the one that started it.
 *
 * Asked so that ini_set_canon() does not build a joined form for a name that
 * cannot have one.
 */
GTEXT_INTERNAL_API bool gtext_ini_names_may_continue(
    const GTEXT_INI_Dialect * dialect);

/** What a physical line is, before anything looks at its contents. */
typedef enum {
  INI_LINE_CONTENT,
  INI_LINE_COMMENT,
  INI_LINE_BLANK
} ini_line_kind;

/**
 * Classify the physical line at @p at: content, comment, or blank.
 *
 * Exported because two callers need exactly the reader's own answer:
 * gtext_ini_continuation_at() decides whether a systemd continuation skips a line,
 * and the parser's indent-continuation scan decides whether a line contributes to
 * a configparser value. A second copy of "is this a comment" is a second place for
 * the leading-whitespace rule to drift - and the rule matters here: the test runs
 * on the *stripped* line, so an indented `#` is a comment and never a continuation.
 *
 * @param line_end Receives the offset just past the line's terminator.
 */
GTEXT_INTERNAL_API ini_line_kind gtext_ini_classify_line(
    const GTEXT_INI_Dialect * dialect, const char * bytes, size_t len, size_t at,
    size_t * line_end);

/**
 * The offset of the first non-whitespace byte of the line at @p at, relative to
 * @p at - configparser's indentation level.
 *
 * Counted in **bytes**, which is Python's count in characters for every document
 * the differential compares: its `\S` search is over a `str`, so a multi-byte
 * indent character would count once there and several times here. The only such
 * characters are the Unicode whitespace outside ASCII, which is already the
 * divergence ::GTEXT_INI_SPACE_PYTHON names.
 *
 * Returns the line's whole content length for a blank line, which no caller uses:
 * a blank line is classified before its indent is asked for.
 */
GTEXT_INTERNAL_API size_t gtext_ini_indent_width(
    const GTEXT_INI_Dialect * dialect, const char * bytes, size_t len,
    size_t at);

/**
 * Join an indent-continued raw value into its logical form.
 *
 * configparser's algorithm exactly: split on line terminators, drop the comment
 * lines, strip each remaining line on both sides, join what is left with a single
 * LF, then strip the result's trailing whitespace. A blank line contributes an
 * empty line, which is why the join is not simply "remove the terminators".
 *
 * @param out Receives the joined bytes, or NULL to measure only. Never longer
 *   than @p len.
 * @param out_len Receives the joined length. May not be NULL.
 */
GTEXT_INTERNAL_API void gtext_ini_join_indent(
    const GTEXT_INI_Dialect * dialect, const char * raw, size_t len, char * out,
    size_t * out_len);

/**
 * Whether an indent-continued value would read back as itself.
 *
 * The writer's question, and it cannot be a list of forbidden bytes: an LF is
 * fine here - it is how a continuation is spelled - unless the line after it
 * would be read as something other than a continuation of this value. So the
 * predicate walks the value with the reader's own line rules and asks, of each
 * line after the first, whether re-reading would take it back.
 *
 * @param verbatim Whether the writer will emit these bytes unchanged. It decides
 *   the one rule the two emissions disagree about: a verbatim value carries its
 *   own indentation and a synthesized one is given it, so each requires exactly
 *   what the other forbids.
 */
GTEXT_INTERNAL_API bool gtext_ini_indent_value_ok(
    const GTEXT_INI_Dialect * dialect, const char * value, size_t len,
    bool verbatim);

#endif // GHOTI_IO_GTEXT_INI_INI_INTERNAL_H
