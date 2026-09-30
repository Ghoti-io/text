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
 * @file ini_dom.h
 * @brief The INI document tree: groups, entries and the comments between them.
 *
 * An INI document is flat - a sequence of groups, each a sequence of entries -
 * so there is no depth to bound and no recursion to unwind. What the tree adds
 * over a hash map is **document order and the original spelling**, both of
 * which Desktop Entry §3 requires an implementation to preserve across a
 * rewrite.
 *
 * Every value this header returns is the **raw bytes between the delimiter and
 * the end of the line**, with the leading run of spaces after `=` removed and
 * nothing else done to it. Escapes are still escapes. To get a usable string,
 * a list, a boolean or a number, use ini_value.h.
 *
 * Duplicate groups and duplicate keys are *stored* under every dialect, in
 * document order, even where the dialect refuses the document - a refusal
 * happens before the tree is returned, so a caller never sees a tree that
 * violates its own dialect. Where duplicates are allowed,
 * gtext_ini_document_get() and gtext_ini_group_get() resolve them per
 * ::GTEXT_INI_Dupkey_Mode and gtext_ini_group_get_nth() reaches the rest.
 */

#ifndef GHOTI_IO_GTEXT_INI_INI_DOM_H
#define GHOTI_IO_GTEXT_INI_INI_DOM_H

#include <ghoti.io/text/ini/ini_core.h>
#include <ghoti.io/text/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief An INI document. Opaque; owns every group, key, value and comment in
 *   it.
 */
typedef struct GTEXT_INI_Document GTEXT_INI_Document;

/**
 * @brief One group of an INI document. Opaque; owned by its document, and
 *   invalidated when the document is freed or when a group is added to it.
 */
typedef struct GTEXT_INI_Group GTEXT_INI_Group;

/**
 * @brief Parse a document from memory.
 *
 * @param bytes The document. May contain NUL bytes: this module is
 *   length-based throughout, which is a deliberate deviation from `GKeyFile`,
 *   whose NUL-terminated strings truncate a value at the first NUL. @ref
 *   format_ini records it as a deviation with its reproduction.
 * @param len Length of @p bytes in bytes.
 * @param opts Options, or NULL for gtext_ini_parse_options_default().
 * @param err Filled in on failure; release with gtext_ini_error_free(). May be
 *   NULL.
 * @return The document, or NULL on failure. Release with gtext_ini_free().
 */
GTEXT_API GTEXT_INI_Document * gtext_ini_parse(const char * bytes, size_t len,
    const GTEXT_INI_Parse_Options * opts, GTEXT_INI_Error * err);

/**
 * @brief Parse a document from a file, reading incrementally.
 *
 * A pipe works, and ::GTEXT_INI_Parse_Options::max_total_bytes is applied
 * while reading rather than after, so an over-large file is refused without
 * being held in memory.
 *
 * @param path The file to read.
 * @param opts Options, or NULL for gtext_ini_parse_options_default().
 * @param err Filled in on failure. May be NULL.
 * @return The document, or NULL on failure.
 */
GTEXT_API GTEXT_INI_Document * gtext_ini_parse_file(const char * path,
    const GTEXT_INI_Parse_Options * opts, GTEXT_INI_Error * err);

/**
 * @brief An empty document, for building one up.
 *
 * @param opts Options, or NULL for the defaults. Only the allocator and the
 *   dialect are read; the dialect is kept, so that gtext_ini_group_set()
 *   enforces the same key rules a parse would have.
 * @return The document, or NULL on allocation failure.
 */
GTEXT_API GTEXT_INI_Document * gtext_ini_new(
    const GTEXT_INI_Parse_Options * opts);

/**
 * @brief Release a document and everything in it.
 *
 * @param doc The document. NULL is ignored.
 */
GTEXT_API void gtext_ini_free(GTEXT_INI_Document * doc);

/**
 * @brief What encoding the parsed bytes carried a mark for.
 *
 * ::GTEXT_INI_SOURCE_BYTES for a document with no mark, for one built by
 * gtext_ini_new(), and for NULL. ::GTEXT_INI_SOURCE_UTF8 when the bytes carried a
 * UTF-8 mark - whether or not ::GTEXT_INI_Dialect::skip_bom skipped it, because
 * this reports what the file was and that field decides what the parser did with
 * it. ::GTEXT_INI_SOURCE_UTF16LE or ::GTEXT_INI_SOURCE_UTF16BE when
 * GTEXT_INI_Parse_Options::decode_utf16 decoded one - the only case in which the
 * tree's bytes are not a subsequence of the caller's.
 *
 * **The reason to ask is gtext_ini_write().** It emits what the tree holds, which
 * for a decoded document is UTF-8 with no mark, so a caller that means to write
 * the file back as it found it needs to know that and re-encode. Every other
 * value means a rewrite is in the document's own bytes.
 *
 * @param doc The document. NULL yields ::GTEXT_INI_SOURCE_BYTES.
 * @return The encoding its mark named.
 */
GTEXT_API GTEXT_INI_Source_Encoding gtext_ini_document_source_encoding(
    const GTEXT_INI_Document * doc);

/**
 * @brief How many groups the document has, counting duplicates separately.
 *
 * @param doc The document.
 * @return The count, or 0 if @p doc is NULL.
 */
GTEXT_API size_t gtext_ini_document_group_count(
    const GTEXT_INI_Document * doc);

/**
 * @brief The group at an index, in document order.
 *
 * @param doc The document.
 * @param index 0-based.
 * @return The group, or NULL if out of range.
 */
GTEXT_API const GTEXT_INI_Group * gtext_ini_document_group_at(
    const GTEXT_INI_Document * doc, size_t index);

/**
 * @brief The first group with a given name.
 *
 * @param doc The document.
 * Matched by the dialect's rules, not byte for byte. Desktop Entry §3 says
 * "case is significant everywhere in the file" and this is a byte comparison
 * there - but ::GTEXT_INI_DIALECT_GIT, ::GTEXT_INI_DIALECT_EDITORCONFIG and
 * ::GTEXT_INI_DIALECT_WIN32 each fold a group name, and Win32 trims one, so
 * `[ Boot ]` is found by `boot`.
 *
 * **This paragraph said "no dialect here folds a group name" for three dialects
 * after that stopped being true**, which is worth recording next to the
 * correction: the sentence was accurate when written, cited a specification to
 * say so, and nothing about adding a folding dialect brings you back to the
 * accessor whose contract it changed. The same claim was wrong in
 * gtext_ini_document_group()'s neighbour below for the same reason.
 *
 * @param doc The document.
 * @param name The group name, NUL-terminated.
 * @return The group, or NULL if there is none.
 */
GTEXT_API const GTEXT_INI_Group * gtext_ini_document_group(
    const GTEXT_INI_Document * doc, const char * name);

/**
 * @brief Look a key up across every group of a given name.
 *
 * On a document with a repeated group header the dialect decides whether the
 * repeats are one section: ::GTEXT_INI_Dialect::merge_duplicate_groups. GLib
 * merges them, so a key in the second `[G]` is found under `G`; the Win32 profile
 * API does not, and a key in a second `[a]` is retrievable by no name at all
 * while `GetPrivateProfileSectionNames` still lists that section. Either way the
 * tree keeps the groups apart and this searches as many as the dialect says, so
 * the merge is a property of the lookup rather than a loss of what the document
 * said.
 *
 * The name is matched the way gtext_ini_document_group() matches it, folding and
 * trimming per dialect.
 *
 * @param doc The document.
 * @param group The group name.
 * @param key The key, including any `[LOCALE]` postfix.
 * @param len Receives the value's length in bytes. May be NULL.
 * @return The raw value, or NULL if absent. Not NUL-terminated in general -
 *   read @p len. Owned by the document.
 */
GTEXT_API const char * gtext_ini_document_get(const GTEXT_INI_Document * doc,
    const char * group, const char * key, size_t * len);

/**
 * @brief Append a group.
 *
 * Refuses a duplicate name when the dialect does, and a name containing `[`,
 * `]` or a control character always (Desktop Entry §3.2).
 *
 * **Invalidates every ::GTEXT_INI_Group pointer previously returned for this
 * document**, because the group array may move. Look groups up again after
 * adding one.
 *
 * @param doc The document.
 * @param name The group name, NUL-terminated.
 * @param out Receives the new group. May be NULL.
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_DUPGROUP, ::GTEXT_INI_E_BAD_GROUP,
 *   ::GTEXT_INI_E_INVALID or ::GTEXT_INI_E_OOM.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_document_add_group(
    GTEXT_INI_Document * doc, const char * name, GTEXT_INI_Group ** out);

/**
 * @brief The comment and blank lines before the first group header, verbatim.
 *
 * Includes each line's terminator, so that writing it back reproduces the
 * input's bytes. NULL when there were none or when
 * ::GTEXT_INI_Parse_Options::retain_comments was false.
 *
 * @param doc The document.
 * @param len Receives the length in bytes. May be NULL.
 * @return The text, or NULL.
 */
GTEXT_API const char * gtext_ini_document_leading_comment(
    const GTEXT_INI_Document * doc, size_t * len);

/**
 * @brief The comment and blank lines after the last entry, verbatim.
 *
 * @param doc The document.
 * @param len Receives the length in bytes. May be NULL.
 * @return The text, or NULL.
 */
GTEXT_API const char * gtext_ini_document_trailing_comment(
    const GTEXT_INI_Document * doc, size_t * len);

/**
 * @brief A group's name.
 *
 * @param group The group.
 * @param len Receives the length in bytes. May be NULL.
 * @return The name, or NULL if @p group is NULL.
 */
GTEXT_API const char * gtext_ini_group_name(const GTEXT_INI_Group * group,
    size_t * len);

/**
 * @brief How many entries the group has, counting duplicates separately.
 *
 * @param group The group.
 * @return The count, or 0 if @p group is NULL.
 */
GTEXT_API size_t gtext_ini_group_entry_count(const GTEXT_INI_Group * group);

/**
 * @brief The key at an index, in document order.
 *
 * The key is exactly what the document said, **including any `[LOCALE]`
 * postfix**: the tree holds the spelling and gtext_ini_group_get_locale() does
 * the matching, which is how `GKeyFile` behaves - `g_key_file_get_keys()` on a
 * file with `Name[de]` reports a key literally named `Name[de]`.
 *
 * @param group The group.
 * @param index 0-based.
 * @param len Receives the length in bytes. May be NULL.
 * @return The key, or NULL if out of range.
 */
GTEXT_API const char * gtext_ini_group_key_at(const GTEXT_INI_Group * group,
    size_t index, size_t * len);

/**
 * @brief The raw value at an index, in document order.
 *
 * @param group The group.
 * @param index 0-based.
 * @param len Receives the length in bytes. May be NULL.
 * @return The raw value, or NULL if out of range. An entry written `k=` has a
 *   zero-length value, which is not the same as absent.
 */
GTEXT_API const char * gtext_ini_group_value_at(const GTEXT_INI_Group * group,
    size_t index, size_t * len);

/**
 * @brief Look a key up in one group, resolving duplicates per the dialect.
 *
 * @param group The group.
 * @param key The key, NUL-terminated, including any `[LOCALE]` postfix.
 * @param len Receives the length in bytes. May be NULL.
 * @return The raw value, or NULL if absent.
 */
/**
 * @brief The index of the @p n-th entry carrying @p key, or `SIZE_MAX`.
 *
 * The primitive gtext_ini_group_get() is built on, exposed because a NULL from
 * that function means two different things and this one separates them: an
 * absent key returns `SIZE_MAX`, and a key present with no value returns its
 * index, which gtext_ini_group_value_present_at() then answers `false` for.
 *
 * Folding is the dialect's. Under ::GTEXT_INI_Dialect::fold_case the key is
 * matched without regard to case.
 *
 * @param group The group. NULL yields `SIZE_MAX`.
 * @param key The key, NUL-terminated.
 * @param n Which occurrence, counting from zero in document order.
 * @return The entry index, or `SIZE_MAX` when there is no such occurrence.
 */
GTEXT_API size_t gtext_ini_group_find(const GTEXT_INI_Group * group,
    const char * key, size_t n);

/**
 * @brief Whether the entry at @p index has a value at all.
 *
 * False for a *valueless key* - git's `k` with no `=`, which its porcelain reads
 * as boolean true. Such an entry is present, is written back as `k` with no `=`,
 * and gtext_ini_group_value_at() returns NULL for it.
 *
 * This exists because NULL cannot carry the distinction: gtext_ini_group_get()
 * returns NULL for a key that is not there *and* for one that is there with no
 * value, and a caller reading git config has to tell those apart - the first
 * means "unset", the second means "true".
 *
 * @param group The group. NULL yields false.
 * @param index The entry index.
 * @return True when the entry has a value, false when it has none or the index
 *   is out of range.
 */
GTEXT_API bool gtext_ini_group_value_present_at(const GTEXT_INI_Group * group,
    size_t index);

/**
 * @brief The group's canonical name - what a lookup matches against.
 *
 * The same bytes as gtext_ini_group_name() for a dialect that neither folds case
 * nor has subsections. For git it is the folded `section` or
 * `section.subsection`, which is the name `git config --list` prints: a header
 * spelled `[Remote "orig in"]` has the name `Remote "orig in"` and the canonical
 * name `remote.orig in`.
 *
 * @param group The group. NULL yields NULL.
 * @param len Receives the length. May be NULL.
 * @return The canonical name, not NUL-terminated-dependent but always
 *   NUL-terminated in practice.
 */
GTEXT_API const char * gtext_ini_group_canonical_name(
    const GTEXT_INI_Group * group, size_t * len);

/**
 * @brief The entry's canonical key - what a lookup matches against.
 *
 * The same bytes as gtext_ini_group_key_at() for a dialect that does not fold
 * case; the lower-cased spelling for one that does. `[a] Bare = v` under git
 * keeps the key `Bare` on the tree, so a rewrite is byte-identical, and answers
 * `bare` here, which is the name `git config --list` prints.
 *
 * @param group The group. NULL yields NULL.
 * @param index The entry index.
 * @param len Receives the length. May be NULL.
 * @return The canonical key, or NULL when the index is out of range.
 */
GTEXT_API const char * gtext_ini_group_canonical_key_at(
    const GTEXT_INI_Group * group, size_t index, size_t * len);

/**
 * @brief Whether this group holds entries that appeared before any header.
 *
 * True only under ::GTEXT_INI_Dialect::allow_preamble, for the one synthetic
 * group such entries live in. Its name is empty and **no header is written for
 * it**, so a rewrite does not invent a `[]` line that the document never had.
 *
 * @param group The group. NULL yields false.
 * @return True for the preamble group.
 */
GTEXT_API bool gtext_ini_group_is_preamble(const GTEXT_INI_Group * group);

GTEXT_API const char * gtext_ini_group_get(const GTEXT_INI_Group * group,
    const char * key, size_t * len);

/**
 * @brief How many entries in the group carry a given key.
 *
 * Greater than one only where the dialect allows a duplicate. This is the
 * denominator a caller iterating with gtext_ini_group_get_nth() needs.
 *
 * @param group The group.
 * @param key The key, NUL-terminated.
 * @return The count.
 */
GTEXT_API size_t gtext_ini_group_count_key(const GTEXT_INI_Group * group,
    const char * key);

/**
 * @brief The n-th value for a key, in document order.
 *
 * Reaches every occurrence under every ::GTEXT_INI_Dupkey_Mode, including
 * ::GTEXT_INI_DUPKEY_LAST_WINS, where gtext_ini_group_get() answers only one
 * of them. The mode decides what a *lookup* means, never what the tree holds.
 *
 * @param group The group.
 * @param key The key, NUL-terminated.
 * @param n 0-based, in document order.
 * @param len Receives the length in bytes. May be NULL.
 * @return The raw value, or NULL if @p n is past the count.
 */
GTEXT_API const char * gtext_ini_group_get_nth(const GTEXT_INI_Group * group,
    const char * key, size_t n, size_t * len);

/**
 * @brief Set a key's raw value, replacing an existing entry or appending one.
 *
 * The value is stored verbatim; nothing is escaped. A caller writing a value
 * that contains a newline, or whose first character is a space, must escape it
 * first with gtext_ini_escape() - or the writer will report
 * ::GTEXT_INI_E_UNREPRESENTABLE rather than emit a document that reads back
 * differently.
 *
 * Under ::GTEXT_INI_DUPKEY_COLLECT this replaces the first occurrence rather
 * than appending a second, because a setter that grew the list every time it
 * was called would make an idempotent update impossible. Use
 * gtext_ini_group_add() to append.
 *
 * @param group The group.
 * @param key The key, NUL-terminated.
 * @param value The raw value.
 * @param value_len Length of @p value.
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_BAD_KEY, ::GTEXT_INI_E_INVALID or
 *   ::GTEXT_INI_E_OOM.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_group_set(GTEXT_INI_Group * group,
    const char * key, const char * value, size_t value_len);

/**
 * @brief Append an entry, whether or not the key is already present.
 *
 * Refuses a duplicate under ::GTEXT_INI_DUPKEY_ERROR, which is what makes this
 * safe to call in a loop under the Desktop Entry dialect: the second call
 * fails rather than producing a document the dialect would not have parsed.
 *
 * @param group The group.
 * @param key The key, NUL-terminated.
 * @param value The raw value.
 * @param value_len Length of @p value.
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_DUPKEY, ::GTEXT_INI_E_BAD_KEY,
 *   ::GTEXT_INI_E_INVALID or ::GTEXT_INI_E_OOM.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_group_add(GTEXT_INI_Group * group,
    const char * key, const char * value, size_t value_len);

/**
 * @brief The comment and blank lines immediately before the group's header.
 *
 * @param group The group.
 * @param len Receives the length in bytes. May be NULL.
 * @return The text with its line terminators, or NULL.
 */
GTEXT_API const char * gtext_ini_group_leading_comment(
    const GTEXT_INI_Group * group, size_t * len);

/**
 * @brief The comment and blank lines immediately before an entry.
 *
 * Desktop Entry has no inline comment form, so every comment in the document
 * belongs before some line, and these three accessors plus the document's two
 * account for all of them. That is what makes a byte-identical rewrite
 * possible rather than merely likely.
 *
 * @param group The group.
 * @param index The entry's index, 0-based.
 * @param len Receives the length in bytes. May be NULL.
 * @return The text with its line terminators, or NULL.
 */
GTEXT_API const char * gtext_ini_group_entry_comment_at(
    const GTEXT_INI_Group * group, size_t index, size_t * len);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_INI_INI_DOM_H
