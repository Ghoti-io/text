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
 * @file ini_core.c
 * @brief Dialect constructors, option defaults, error release, small helpers.
 */

#include "ini_internal.h"
#include <stdlib.h>

/*
 * Desktop Entry §4's escape set, as the letters that may follow a backslash.
 * `;` is absent on purpose: §4 defines it only inside a list, and
 * g_key_file_get_string() refuses a bare `a\;b` that
 * g_key_file_get_string_list() accepts. gtext_ini_value_list() adds it.
 */
static const char ini_desktop_escapes[] = "snrt\\";

/*
 * git's escape set, from `git-config(1)`: "\n", "\t" and "\b" for the control
 * characters, and "\\" and "\"" for themselves. "Other char escape sequences
 * (including octal escape sequences) are invalid" - measured, `k = a\qb` and
 * `k = a\101b` are both refused, and so is `k = a\ b`.
 *
 * Note what is *absent*: `\r`, which Desktop Entry has, and `\s`. git has no
 * spelling for a carriage return in a value at all.
 */
static const char ini_git_escapes[] = "ntb\\\"";

/**
 * systemd's single-letter escapes: the full C set.
 *
 * `a b f n r t v` and `\\ " '`, plus Desktop Entry's `\s` for a space, which
 * systemd also has. The four numeric forms are not spellable here - a letter set
 * cannot hold a variable-length sequence - and are
 * ::GTEXT_INI_Dialect::numeric_escapes instead.
 *
 * Every one measured as bytes against systemd 257, because the diagnostic channel
 * renders a control character as nothing and four of these are control characters:
 * `\a` is 07, `\b` 08, `\f` 0c, `\v` 0b.
 */
static const char ini_systemd_escapes[] = "abfnrtvs\\\"'";

GTEXT_INI_Dialect gtext_ini_dialect_desktop_entry(void) {
  GTEXT_INI_Dialect d;
  memset(&d, 0, sizeof(d));
  d.id = GTEXT_INI_DIALECT_DESKTOP_ENTRY;
  d.comment_hash = true;
  d.comment_semicolon = false;
  d.allow_leading_whitespace = false;
  d.allow_preamble = false;
  d.allow_duplicate_groups = false;
  d.dupkey = GTEXT_INI_DUPKEY_ERROR;
  d.name_style = GTEXT_INI_NAMES_DESKTOP_ENTRY;
  d.locale_postfix = true;
  d.require_unlocalized_key = true;
  d.accept_crlf = false;
  d.trim_trailing_space = false;
  d.skip_bom = false;
  d.escapes = ini_desktop_escapes;
  d.list_separator = ';';
  d.utf8_values = true;
  d.continuation = GTEXT_INI_CONTINUATION_NONE;
  d.inline_comments = false;
  d.quoted_values = false;
  d.fold_case = false;
  d.subsection_syntax = false;
  d.valueless_keys = false;
  d.header_remainder_is_entry = false;
  return d;
}

GTEXT_INI_Dialect gtext_ini_dialect_generic(void) {
  /*
   * Built by relaxing the base rather than by listing fields, so that a field
   * added to the Desktop Entry dialect is inherited here instead of silently
   * defaulting to zero.
   *
   * **Six of the seven assignments are relaxations and one is not.** The six
   * only widen what is accepted, so every document the strict dialect accepts
   * is accepted here with the same values. `accept_crlf` is different: it
   * removes a CR from the *content* of a line the strict dialect already
   * accepted, so a document with a CRLF on an entry line reads with one byte
   * less here. That is a behaviour change dressed as a relaxation, and calling
   * it one cost a defect - fuzz_ini.cpp's parity property found it at 30,209
   * executions, which is exactly what it is for. `skip_bom` travels with it
   * because a BOM is the same kind of thing at the start of the document.
   *
   * `escapes` and `list_separator` are deliberately untouched: a change to how
   * a value is *decoded* would not be a relaxation either, and unlike CRLF
   * there is no reason to want one.
   */
  GTEXT_INI_Dialect d = gtext_ini_dialect_desktop_entry();
  d.id = GTEXT_INI_DIALECT_GENERIC;
  /* The six relaxations. */
  d.comment_semicolon = true;
  d.allow_leading_whitespace = true;
  d.allow_preamble = true;
  d.allow_duplicate_groups = true;
  d.dupkey = GTEXT_INI_DUPKEY_LAST_WINS;
  d.name_style = GTEXT_INI_NAMES_ANY;
  /* The one normalisation. See above: this changes values, not just which
   * documents are accepted. */
  d.accept_crlf = true;
  d.skip_bom = true;
  return d;
}

GTEXT_INI_Dialect gtext_ini_dialect_git_config(void) {
  /*
   * Written out field by field rather than derived from another dialect, which
   * is the opposite of gtext_ini_dialect_generic() and is deliberate.
   *
   * The generic dialect is Desktop Entry plus a list of changes, so deriving it
   * means a field added later is inherited and the list stays short. git config
   * is not a relaxation of Desktop Entry in *either* direction - it accepts a
   * preamble, an inline comment, a continuation, a quoted value, a valueless
   * key, a repeated key and a subsection, and it refuses a key that does not
   * begin with a letter and a group name holding anything but `A-Za-z0-9-.`.
   * Deriving it would hide that by making the differences look like a short
   * list of tweaks, and would silently inherit any future Desktop Entry field
   * whose default happens to be wrong here. A dialect that is its own grammar
   * is spelled as its own grammar.
   */
  GTEXT_INI_Dialect d;
  memset(&d, 0, sizeof(d));
  d.id = GTEXT_INI_DIALECT_GIT_CONFIG;
  d.comment_hash = true;
  d.comment_semicolon = true;
  /* Leading whitespace before a key or a header is skipped; measured, both
   * `  [a]` and `\t k = v` are accepted. */
  d.allow_leading_whitespace = true;
  d.allow_preamble = true;
  /* `[core]` twice is one section to git: `--list` prints both entries and
   * merges them under one name. */
  d.allow_duplicate_groups = true;
  /* Every occurrence is a value. `--get` answers the last and `--get-all` all
   * of them, in order. */
  d.dupkey = GTEXT_INI_DUPKEY_COLLECT;
  d.name_style = GTEXT_INI_NAMES_GIT;
  /* No locale postfix: `k[de]` is not a git key at all, because `[` is not in
   * its key charset. Leaving locale_postfix false is what makes `k[de] = v` a
   * GTEXT_INI_E_BAD_KEY here rather than a localized spelling of `k`. */
  d.locale_postfix = false;
  d.require_unlocalized_key = false;
  /*
   * git converts CRLF to LF as it reads - its get_next_char() peeks after a CR
   * and keeps the CR only when no LF follows. So a lone CR is data, and one
   * before an LF is part of the terminator.
   */
  d.accept_crlf = true;
  /*
   * Trailing whitespace is dropped from a value, but not by trimming the span:
   * git tracks the offset of the last content byte, and an escape counts as
   * content. `k = a\t` therefore keeps the tab. The scanner is what implements
   * that; this flag records the intent for a reader of the dialect.
   */
  d.trim_trailing_space = true;
  /*
   * Measured: a file beginning with a UTF-8 BOM parses, and the first section
   * is found. git does not reject it and does not fold it into the name.
   */
  d.skip_bom = true;
  d.escapes = ini_git_escapes;
  /* True here and nowhere else: git unescapes while it reads, so a bad escape is
   * a syntax error rather than a question for the accessor. Measured - `git config
   * -f` exits 128 on `k = a\\qb`. */
  d.escapes_in_grammar = true;
  /*
   * **No list separator.** git has multi-valued keys, and they are spelled as
   * repeated lines rather than as one delimited value - which is what
   * GTEXT_INI_DUPKEY_COLLECT is for. Setting a separator here would invent a
   * syntax git does not have, and gtext_ini_value_list() correctly refuses a
   * dialect that has none.
   */
  d.list_separator = 0;
  /* False: `git config --get` returns a value holding a bare 0xFF unchanged,
   * where g_key_file_get_string() refuses one. */
  d.utf8_values = false;
  d.continuation = GTEXT_INI_CONTINUATION_JOIN_EMPTY;
  d.inline_comments = true;
  d.quoted_values = true;
  d.fold_case = true;
  d.subsection_syntax = true;
  d.valueless_keys = true;
  d.header_remainder_is_entry = true;
  return d;
}

GTEXT_INI_Dialect gtext_ini_dialect_editorconfig(void) {
  /*
   * Field by field, for the reason gtext_ini_dialect_git_config() gives: this is
   * not a relaxation of Desktop Entry in either direction. It accepts a preamble,
   * a duplicate group, a duplicate key, a leading BOM, CRLF, a `;` comment and a
   * section name holding any byte; it refuses the `\n` escape and the
   * `;`-separated list Desktop Entry defines, so its values mean something
   * different for the same bytes.
   */
  GTEXT_INI_Dialect d;
  memset(&d, 0, sizeof(d));
  d.id = GTEXT_INI_DIALECT_EDITORCONFIG;
  d.comment_hash = true;
  d.comment_semicolon = true;
  /*
   * The specification's line rule is "remove all leading and trailing whitespace,
   * then process the remaining text as specified for its type" - the trim happens
   * *before* the classification, so an indented `[a]` is a header and an indented
   * `# c` is a comment. That is two flags here rather than one: this one for the
   * leading run, ::trim_trailing_space for the trailing one, and the fact that
   * blankness is tested over the whole line for a whitespace-only line.
   *
   * core-py gets this wrong and the conformance suite does not catch it: it tests
   * `line[0] in '#;'` before stripping, so `  # c` is a parse error there.
   * Measured.
   */
  d.allow_leading_whitespace = true;
  /* "Root: must be specified in the preamble" - the format has a preamble by
   * name, and is the only one of these four dialects whose specification does. */
  d.allow_preamble = true;
  /* Two sections with the same glob merge, and the later assignment wins;
   * asserted by the suite's `repeat_sections_ML` and `basic_cascade_ML`. */
  d.allow_duplicate_groups = true;
  d.dupkey = GTEXT_INI_DUPKEY_LAST_WINS;
  d.name_style = GTEXT_INI_NAMES_EDITORCONFIG;
  /* No locale postfix: `[` and `]` are ordinary bytes in an EditorConfig key, so
   * `k[de]` is a key called `k[de]`. */
  d.locale_postfix = false;
  d.require_unlocalized_key = false;
  /* "LF or CRLF line separators." The suite asserts the CRLF case. */
  d.accept_crlf = true;
  /* "Keys and values are trimmed of leading and trailing whitespace, but include
   * any whitespace that is between non-whitespace characters." So
   * `key= value with whitespace inside  ` is `value with whitespace inside`,
   * which the suite asserts. */
  d.trim_trailing_space = true;
  /*
   * Not in the specification, which says only that the file is UTF-8; both cores
   * skip one, and the suite's `bom_at_head` asserts that they must. Skipped and
   * kept, not discarded - the writer puts it back.
   */
  d.skip_bom = true;
  /*
   * **No escapes at all**, and this is the rule most easily assumed away: the
   * specification defines no escaping mechanism, so a backslash is a byte.
   * Asserted by the suite - `key1=value \; not comment` has the value
   * `value \; not comment`, backslash included. A `\;` inside a *section name*
   * does mean a literal `;` to a core, but that is the glob matcher's escape and
   * not the parser's, so it is not here either.
   */
  d.escapes = NULL;
  /* "Any line that is not one of the above is invalid" and there is no list
   * spelling; a `;` in a value is part of the value. */
  d.list_separator = 0;
  /*
   * False, matching both cores, which hand back a value holding a bare 0xFF
   * unchanged. The specification's "must be UTF-8 encoded" is a requirement on
   * whoever writes the file; enforcing it in a reader would refuse a document no
   * reference refuses. Same reasoning as git config's.
   */
  d.utf8_values = false;
  /*
   * The whitespace set is C's, unlike every other dialect here. Both cores ask
   * the platform - core-c calls `isspace()` and core-py matches `\s` - so a
   * vertical tab and a form feed are trimmed. Measured against core-c, which
   * reads `k=\va\v` as the value `a`.
   */
  d.ctype_whitespace = true;
  /* No continuation. A line that begins with whitespace is a line, not the
   * previous one continued: core-c has the `INI_ALLOW_MULTILINE` machinery
   * inherited from ConfigParser and compiles it *out*, because the suite's
   * `spaces_before_middle_property_ML` asserts three separate keys. */
  d.continuation = GTEXT_INI_CONTINUATION_NONE;
  /*
   * **False, against both references.** "A `;` or `#` anywhere other than at the
   * beginning of a line does *not* start a comment, but is part of the text of
   * that line", and the suite asserts it: `key2=value # not comment` has the
   * value `value # not comment`. Both cores truncate there and so score 33 of 34
   * - for one shared reason, that both descend from `ConfigParser`, so their
   * agreement is heritage rather than corroboration.
   */
  d.inline_comments = false;
  /* No quoting: `k="a b"` is the four-character value with its quotes.
   * core-py maps the exact value `""` to the empty string, which is neither in
   * the specification nor in core-c. */
  d.quoted_values = false;
  /* "Pair keys are case-insensitive. All keys are lowercased after parsing."
   * Section names are *not* folded - see gtext_ini_group_names_fold(). */
  d.fold_case = true;
  d.subsection_syntax = false;
  /* A key with no `=` is not a valueless entry, it is an invalid line: the
   * specification's pair rule needs the `=`, and both cores report an error.
   * `k=` is an empty value, which is a different thing and is legal. */
  d.valueless_keys = false;
  /*
   * False, and this is the one place the specification is stricter than both
   * cores. After the trim a header line must *end* with `]`, so `[a] junk` is
   * not a section header, is not a comment and has no `=` - it is an invalid
   * line. Both cores silently ignore the remainder. With the last-`]` rule,
   * `[a] junk]` is instead one section named `a] junk`, which all three agree on.
   */
  d.header_remainder_is_entry = false;
  return d;
}

GTEXT_INI_Dialect gtext_ini_dialect_systemd(void) {
  /*
   * Field by field, for the reason the two dialects above give. Every value here
   * was measured against systemd 257 rather than read off `systemd.syntax(7)`;
   * notes/text/INI-DIALECTS.md §A.17 has the transcript, and five of these
   * contradict the manual page or fill a silence in it.
   */
  GTEXT_INI_Dialect d;
  memset(&d, 0, sizeof(d));
  d.id = GTEXT_INI_DIALECT_SYSTEMD;
  d.comment_hash = true;
  d.comment_semicolon = true;
  /* Measured: an indented comment, header and entry are all accepted. */
  d.allow_leading_whitespace = true;
  /*
   * **False.** "Assignment outside of section. Ignoring." - systemd refuses an
   * entry before the first header, which makes it the only dialect here besides
   * Desktop Entry to do so, and the opposite of EditorConfig, whose specification
   * names the preamble.
   */
  d.allow_preamble = false;
  /* `[Service]` twice is one section. */
  d.allow_duplicate_groups = true;
  /*
   * Every occurrence is a value, as with git. The *reset* rule - an empty
   * assignment clears the list - is a per-setting semantic rather than a grammar
   * rule, so the tree keeps the empty entry and a caller applies the reset.
   */
  d.dupkey = GTEXT_INI_DUPKEY_COLLECT;
  d.name_style = GTEXT_INI_NAMES_SYSTEMD;
  d.locale_postfix = false;
  d.require_unlocalized_key = false;
  d.accept_crlf = true;
  /*
   * Measured, and not in the manual page. A lone CR ends a line: two settings
   * separated by one are two settings, and the line counter advances across it.
   */
  d.lone_cr_terminates = true;
  /*
   * Measured, and the discriminating case is subtle: `Environment=A\s` and
   * `Environment=A\s   ` give the same two bytes, so the trailing run is stripped
   * from the raw line **before** unescaping and an escaped space at the end
   * survives. Same rule as git's, reached from the other direction.
   */
  d.trim_trailing_space = true;
  d.skip_bom = true;
  d.escapes = ini_systemd_escapes;
  d.numeric_escapes = true;
  /*
   * **False**, and this is the layering claim §5 makes, stated by systemd's own
   * code: `config_parse()` hands the raw value to the setting's parser and never
   * looks at a backslash, so a unit carrying `ExecStart=/bin/foo \\q` parses and
   * only a caller who asks for the decoded form is refused. git is the opposite
   * and is the only dialect here that is.
   */
  d.escapes_in_grammar = false;
  /*
   * **No list separator, and word splitting instead.** systemd spells a list two
   * ways - a repeated key, which is ::dupkey, and whitespace-separated words,
   * which is ::word_split. Neither is a delimiter character, so naming one here
   * would invent a syntax systemd does not have.
   */
  d.list_separator = 0;
  d.word_split = true;
  d.bool_style = GTEXT_INI_BOOLS_SYSTEMD;
  /* False: systemd hands back a value holding a bare 0xFF unchanged, as git does,
   * so validation belongs to whoever asked for a string. */
  d.utf8_values = false;
  /*
   * **False, and this was a live guess worth checking.** systemd is a C program, so
   * `isspace()` was the plausible answer; it is wrong three ways.
   * `Environment\v=W1` and `\vEnvironment=W1` both leave the vertical tab in the
   * key, and a literal `\v` inside a value stays in the word rather than splitting
   * it. So systemd is git's answer, not EditorConfig's.
   */
  d.ctype_whitespace = false;
  d.continuation = GTEXT_INI_CONTINUATION_JOIN_SPACE;
  /* Measured: `Environment=W1 # x` is five words, so a `#` after the start of a
   * line is data. */
  d.inline_comments = false;
  /*
   * **False, and the specification is why** - quoting applies only "for settings
   * where quoting is allowed", which the grammar cannot know. So the parser stores
   * the quotes and gtext_ini_value_words() removes them, which is the layering
   * claim @ref format_ini makes, stated by somebody else's specification.
   */
  d.quoted_values = false;
  /* Measured: `environment=W1` is reported as an *unknown key*, so case matters. */
  d.fold_case = false;
  d.subsection_syntax = false;
  /* "Missing '=', ignoring line." - a bare key is not an entry. */
  d.valueless_keys = false;
  /* "Invalid section header '[Service] junk'" - the remainder is refused, not read
   * as an entry. */
  d.header_remainder_is_entry = false;
  return d;
}

GTEXT_INI_Parse_Options gtext_ini_parse_options_default(void) {
  GTEXT_INI_Parse_Options o;
  memset(&o, 0, sizeof(o));
  o.allocator = NULL;
  o.dialect = gtext_ini_dialect_desktop_entry();
  o.max_total_bytes = 0;
  o.max_groups = 0;
  o.max_entries_per_group = 0;
  /* True, unlike the TOML module: Desktop Entry §3 requires that a rewrite
   * preserve comments and unknown fields, so a default that dropped them could
   * not rewrite a file correctly. */
  o.retain_comments = true;
  return o;
}

void gtext_ini_error_free(GTEXT_INI_Error * err) {
  if (!err) return;
  /* The default allocator, not the parse's: an error outlives the parse, and a
   * parse can fail before it has read its options. See
   * gtext_toml_error_free(). */
  if (err->context_snippet) {
    gtext_allocator_free(gtext_allocator_default(), err->context_snippet);
    err->context_snippet = NULL;
  }
  err->context_snippet_len = 0;
  err->caret_offset = 0;
}

bool gtext_ini_str_set(const GTEXT_Allocator * alloc, ini_str * out,
    const char * bytes, size_t len) {
  char * copy = gtext_allocator_malloc(alloc, len + 1);
  if (!copy) return false;
  if (len) memcpy(copy, bytes, len);
  copy[len] = '\0';
  if (out->data) gtext_allocator_free(alloc, out->data);
  out->data = copy;
  out->len = len;
  return true;
}

void gtext_ini_str_clear(const GTEXT_Allocator * alloc, ini_str * s) {
  if (s->data) gtext_allocator_free(alloc, s->data);
  s->data = NULL;
  s->len = 0;
}

/** How many bytes of context an error carries either side of the caret. */
#define INI_SNIPPET_RADIUS 40

void gtext_ini_set_error(GTEXT_INI_Error * err, GTEXT_INI_Status code,
    const char * message, const char * input, size_t input_len,
    size_t offset) {
  if (!err) return;
  memset(err, 0, sizeof(*err));
  err->code = code;
  err->message = message;
  err->offset = offset;

  /* Line and column, counted from the start. Columns are characters, so a
   * continuation byte does not advance one. */
  int line = 1;
  int col = 1;
  for (size_t i = 0; i < offset && i < input_len; i++) {
    if (input[i] == '\n') {
      line++;
      col = 1;
    }
    else if ((input[i] & 0xC0) != 0x80) {
      col++;
    }
  }
  err->line = line;
  err->col = col;

  if (!input || !input_len) return;
  size_t start = offset > INI_SNIPPET_RADIUS ? offset - INI_SNIPPET_RADIUS : 0;
  size_t end = offset + INI_SNIPPET_RADIUS;
  if (end > input_len) end = input_len;
  size_t span = end - start;
  char * snippet = gtext_allocator_malloc(gtext_allocator_default(), span + 1);
  if (!snippet) return; /* An error with no snippet is still the right error. */
  memcpy(snippet, input + start, span);
  snippet[span] = '\0';
  err->context_snippet = snippet;
  err->context_snippet_len = span;
  err->caret_offset = offset - start;
}

size_t gtext_ini_key_base_len(const char * key, size_t len) {
  /* A postfix is a trailing `[...]`, so scan from the end: a key may contain a
   * `[` that is not the postfix's when the charset is not strict. */
  if (len < 3 || key[len - 1] != ']') return len;
  for (size_t i = len - 2; i > 0; i--) {
    if (key[i] == '[') return i;
    if (key[i] == ']') return len; /* `a]b]` is not a postfix. */
  }
  return len;
}

bool gtext_ini_is_space(const GTEXT_INI_Dialect * dialect, char c) {
  if (c == ' ' || c == '\t') return true;
  /*
   * CR is whitespace to git and data to Desktop Entry. Tying it to accept_crlf
   * rather than to the dialect id keeps it a property a caller can set.
   */
  if (c == '\r' && dialect->accept_crlf) return true;
  /*
   * `\v` and `\f` are whitespace to EditorConfig and **not** to git, which is
   * why this is a field. git carries its own ctype table classing them as
   * control characters - measured, a trailing `\v` stays in git's value and a
   * leading one is a syntax error - while both EditorConfig cores ask the
   * platform and trim them. Desktop Entry's grammar has neither, so its answer
   * is the same either way.
   *
   * LF is never here: the line ends at it before any trimming happens.
   */
  if (dialect->ctype_whitespace && (c == '\v' || c == '\f')) return true;
  return false;
}

/** Whether @p c may appear in a git section or key name: `A-Za-z0-9-`. */
static bool ini_git_keychar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-';
}

static char ini_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

bool gtext_ini_canon_group(const GTEXT_INI_Dialect * dialect, const char * raw,
    size_t len, char * out, size_t * out_len) {
  *out_len = 0;
  if (!gtext_ini_group_names_fold(dialect) && !dialect->subsection_syntax) {
    return false;
  }
  size_t w = 0;
  size_t i = 0;
  /* The section part: `A-Za-z0-9-` plus `.`, folded. A `.` here is the
   * deprecated subsection spelling, whose subsection folds too, so the whole
   * run can be folded in one pass. */
  while (i < len && (ini_git_keychar(raw[i]) || raw[i] == '.')) {
    out[w++] = dialect->fold_case ? ini_lower(raw[i]) : raw[i];
    i++;
  }
  if (i == len) {
    /* No quoted subsection. An empty name is refused - measured, `[]` is a
     * syntax error to git while `[a ""]` is not. */
    if (!w) return false;
    *out_len = w;
    return true;
  }
  if (!dialect->subsection_syntax) return false;
  /* `[section "sub"]`: whitespace, then a quoted run, then nothing. */
  if (!gtext_ini_is_space(dialect, raw[i])) return false;
  while (i < len && gtext_ini_is_space(dialect, raw[i])) i++;
  if (i >= len || raw[i] != '"') return false;
  i++;
  if (!w) return false;
  out[w++] = '.';
  bool closed = false;
  while (i < len) {
    char c = raw[i];
    if (c == '"') {
      closed = true;
      i++;
      break;
    }
    if (c == '\\') {
      /*
       * The second escape layer, and the one that surprises: a backslash inside
       * a subsection name is simply dropped. Measured - `[a "x\ty"]` is the
       * subsection `xty`, while `\t` in a value one line later is a tab.
       */
      i++;
      if (i >= len) return false;
      c = raw[i];
    }
    /* Case is preserved here, and that is the whole difference between the two
     * spellings: `[a "SubB"]` is not `[a "subb"]`, but `[a.SubB]` is. */
    out[w++] = c;
    i++;
  }
  if (!closed) return false;
  if (i != len) return false; /* `[a "b" ]` and `[a "b"x]` are refused. */
  *out_len = w;
  return true;
}

bool gtext_ini_canon_key(const GTEXT_INI_Dialect * dialect, const char * raw,
    size_t len, char * out, size_t * out_len) {
  *out_len = 0;
  if (!dialect->fold_case) return false;
  if (!len) return false;
  for (size_t i = 0; i < len; i++) out[i] = ini_lower(raw[i]);
  *out_len = len;
  return true;
}

bool gtext_ini_group_name_ok(const GTEXT_INI_Dialect * dialect,
    const char * name, size_t len) {
  /*
   * Desktop Entry §3.2: any ASCII except `[`, `]` and control characters.
   *
   * The empty name is refused, which the specification does not say and
   * `GKeyFile` does: `g_key_file_load_from_data()` on `[]` fails with "Invalid
   * group name: ". The specification being silent, the reference decides, and
   * @ref format_ini records that this is why.
   */
  if (dialect->name_style == GTEXT_INI_NAMES_SYSTEMD) {
    /*
     * Everything between `[` and the first `]`, and a space is fine: measured,
     * `[Serv ice]` is a section named `Serv ice` and systemd reports it as an
     * *unknown section* rather than as a syntax error. A terminator is allowed for
     * the same reason it is in a key - the name may be continued, and the parser is
     * the gate.
     */
    if (!len) return false;
    for (size_t i = 0; i < len; i++) {
      unsigned char c = (unsigned char) name[i];
      if (c == '[' || c == ']') return false;
      if ((c == '\n' || c == '\r') &&
          dialect->continuation == GTEXT_INI_CONTINUATION_NONE) {
        return false;
      }
    }
    return true;
  }
  if (dialect->name_style == GTEXT_INI_NAMES_EDITORCONFIG) {
    /*
     * "May contain any characters between the square brackets" - so `[`, `]`,
     * `#`, `;` and a control character are all in the name, and there is nothing
     * left to check but the line terminator, which cannot be here because the
     * header was found on one line.
     *
     * **The empty name is accepted**, which is the opposite of every other
     * dialect here and is deliberate. core-c accepts `[]` and core-py refuses it;
     * the specification says any characters, and refusing would throw away the
     * rest of a document over a section that simply matches no file. The cost is
     * that gtext_ini_group_find("") can find either a `[]` group or the preamble
     * group, whichever comes first, and gtext_ini_group_is_preamble() is how a
     * caller tells them apart.
     */
    for (size_t i = 0; i < len; i++) {
      if (name[i] == '\n') return false;
    }
    return true;
  }
  if (!len) return false;
  if (dialect->name_style == GTEXT_INI_NAMES_GIT) {
    /*
     * git's section grammar, which is stricter than Desktop Entry's: only
     * `A-Za-z0-9-.`, and then optionally whitespace and a quoted subsection.
     * Measured - `[a_b]` and `[caf\xc3\xa9]` are both refused, `[a-b]`, `[a.b]`,
     * `[1a]` and `[12]` are accepted. A section name, unlike a key, need not
     * begin with a letter.
     *
     * The subsection's own rules are gtext_ini_canon_group()'s, and asking it is
     * how this stays a single implementation: a name is legal exactly when it
     * canonicalizes. A second copy of the quoting rules here is what would
     * drift.
     */
    char stack[512];
    char * buf = stack;
    char * heap = NULL;
    if (len > sizeof(stack)) {
      heap = gtext_allocator_malloc(gtext_allocator_default(), len);
      if (!heap) return false;
      buf = heap;
    }
    size_t canon_len = 0;
    bool ok = gtext_ini_canon_group(dialect, name, len, buf, &canon_len);
    if (heap) gtext_allocator_free(gtext_allocator_default(), heap);
    return ok;
  }
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char) name[i];
    if (c == '[' || c == ']') return false;
    if (c < 0x20 || c == 0x7F) return false;
    /* §3.2 says "all ASCII characters", so a byte above 0x7F is outside the
     * name grammar. `GKeyFile` accepts one; @ref format_ini records that as a
     * deviation, with the reproduction, rather than following it. */
    if (dialect->name_style == GTEXT_INI_NAMES_DESKTOP_ENTRY && c > 0x7F) {
      return false;
    }
  }
  return true;
}

bool gtext_ini_group_close_is_last(const GTEXT_INI_Dialect * dialect) {
  /* A capability, not an id test - see ini_internal.h. The question is a property
   * of the name grammar: a dialect whose names may hold a `]` cannot stop at the
   * first one. */
  return dialect->name_style == GTEXT_INI_NAMES_EDITORCONFIG;
}

bool gtext_ini_group_names_fold(const GTEXT_INI_Dialect * dialect) {
  /*
   * EditorConfig folds keys and not sections, so `fold_case` on its own answers
   * the wrong question for a group. A section name is a filepath glob, and
   * whether two spellings of a path are the same file is the filesystem's
   * question rather than the format's - which is why the specification folds keys
   * explicitly and says nothing about sections.
   */
  return dialect->fold_case &&
         dialect->name_style != GTEXT_INI_NAMES_EDITORCONFIG;
}

bool gtext_ini_key_char_ok(const GTEXT_INI_Dialect * dialect, char c) {
  switch (dialect->name_style) {
    case GTEXT_INI_NAMES_GIT:
      return ini_git_keychar(c);
    case GTEXT_INI_NAMES_ANY:
    case GTEXT_INI_NAMES_EDITORCONFIG:
    case GTEXT_INI_NAMES_SYSTEMD:
      /*
       * Anything that reads back as itself: not the delimiter, not a terminator.
       * EditorConfig shares this arm rather than having one of its own, because
       * "the part before the first `=` on the line" is the same rule - the suite
       * asserts `ke y=value`, a key with a space in it. What differs between the
       * two styles is the *group* name, not the key.
       *
       * systemd shares it too, and then **allows a terminator as well**, because a
       * key may be continued: `Environ\` + `ment=v` is the key `Environ ment` and
       * the stored span holds the backslash and the newline. The parser is the gate
       * for that - a terminator reaches a key only by way of a continuation it
       * already followed - so the predicate is permissive here on purpose rather
       * than re-deriving the line structure.
       */
      if (dialect->name_style == GTEXT_INI_NAMES_SYSTEMD &&
          (c == '\n' || c == '\r')) {
        return dialect->continuation != GTEXT_INI_CONTINUATION_NONE;
      }
      return c != '=' && c != '\n' && c != '\r';
    case GTEXT_INI_NAMES_DESKTOP_ENTRY:
    default:
      /* §3.3's `A-Za-z0-9-`. The `[LOCALE]` postfix has its own charset and is
       * checked by gtext_ini_key_ok() rather than here, because `[` and `]` are
       * legal in a key only in that one position. */
      return ini_git_keychar(c);
  }
}

bool gtext_ini_key_ok(const GTEXT_INI_Dialect * dialect, const char * key,
    size_t len) {
  if (!len) return false;
  if (dialect->name_style == GTEXT_INI_NAMES_GIT) {
    /*
     * `A-Za-z0-9-`, and the first byte must be a letter. The first-byte rule is
     * git's and is not Desktop Entry's: measured, `1k = v` and `-k = v` are both
     * refused where `k-1 = v` is not. It is also the one place git is stricter
     * about keys than about sections, where `[12]` is fine.
     */
    char first = key[0];
    bool alpha = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!alpha) return false;
    for (size_t i = 0; i < len; i++) {
      if (!gtext_ini_key_char_ok(dialect, key[i])) return false;
    }
    return true;
  }
  if (dialect->name_style == GTEXT_INI_NAMES_ANY ||
      dialect->name_style == GTEXT_INI_NAMES_EDITORCONFIG ||
      dialect->name_style == GTEXT_INI_NAMES_SYSTEMD) {
    /* Still not anything at all: a key may not contain the delimiter or a line
     * terminator, or the document would not read back as itself. An empty key is
     * refused by the `!len` test above, which is where this dialect parts company
     * with core-c: core-c reads `=v` as a property whose name is the empty
     * string, and core-py refuses it as this does. */
    for (size_t i = 0; i < len; i++) {
      if (!gtext_ini_key_char_ok(dialect, key[i])) return false;
    }
    return true;
  }
  /* Desktop Entry §3.3 from here. */
  size_t base = dialect->locale_postfix ? gtext_ini_key_base_len(key, len)
                                        : len;
  if (!base) return false;
  for (size_t i = 0; i < base; i++) {
    if (!gtext_ini_key_char_ok(dialect, key[i])) return false;
  }
  if (base == len) return true;
  /* The postfix: `[` at base, `]` at the end, and a non-empty locale between
   * it of the shape lang_COUNTRY.ENCODING@MODIFIER (§5). */
  if (key[len - 1] != ']') return false;
  if (len - base < 3) return false;
  for (size_t i = base + 1; i + 1 < len; i++) {
    char c = key[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
              c == '@';
    if (!ok) return false;
  }
  return true;
}

ini_entry * gtext_ini_group_push(GTEXT_INI_Group * group) {
  const GTEXT_Allocator * alloc = group->doc->alloc;
  if (group->count == group->capacity) {
    size_t want = group->capacity ? group->capacity * 2 : 8;
    ini_entry * grown = gtext_allocator_realloc(
        alloc, group->entries, want * sizeof(*grown));
    if (!grown) return NULL;
    group->entries = grown;
    group->capacity = want;
  }
  ini_entry * e = &group->entries[group->count];
  memset(e, 0, sizeof(*e));
  group->count++;
  return e;
}
