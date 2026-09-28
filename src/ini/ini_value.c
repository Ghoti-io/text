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
 * @file ini_value.c
 * @brief Decoding a raw INI value, and Desktop Entry's locale fallback.
 *
 * This is where everything the parser deliberately did not do happens:
 * escapes, list splitting, typed conversion, UTF-8 validation and locale
 * selection. The split is `GKeyFile`'s, measured rather than guessed - a
 * document with `k=a\qb` or with a stray 0xFF in a value parses there and fails
 * only at `g_key_file_get_string()`.
 */

#include "../text_number_internal.h"
#include "ini_internal.h"
#include <errno.h>
#include <stdlib.h>

/** Whether [bytes, bytes+len) is well-formed UTF-8, rejecting surrogates and
 *  over-long forms. */
static bool ini_utf8_ok(const char * bytes, size_t len) {
  size_t i = 0;
  while (i < len) {
    unsigned char c = (unsigned char) bytes[i];
    size_t need;
    unsigned long cp;
    if (c < 0x80) {
      i++;
      continue;
    }
    else if ((c & 0xE0) == 0xC0) {
      need = 1;
      cp = c & 0x1Fu;
    }
    else if ((c & 0xF0) == 0xE0) {
      need = 2;
      cp = c & 0x0Fu;
    }
    else if ((c & 0xF8) == 0xF0) {
      need = 3;
      cp = c & 0x07u;
    }
    else {
      return false; /* A continuation byte or 0xF8..0xFF as a lead. */
    }
    if (i + need >= len + 0 && i + need > len - 1) return false;
    for (size_t k = 1; k <= need; k++) {
      unsigned char cc = (unsigned char) bytes[i + k];
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    /* Over-long, surrogate, or beyond U+10FFFF - each of which has a shorter
     * or no legal spelling, so accepting it would accept two spellings of one
     * string. */
    if (need == 1 && cp < 0x80) return false;
    if (need == 2 && cp < 0x800) return false;
    if (need == 3 && cp < 0x10000) return false;
    if (cp > 0x10FFFF) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    i += need + 1;
  }
  return true;
}

/** A space or a tab, the only whitespace an INI line may carry inside it. */
static bool ini_is_space_byte(char c) { return c == ' ' || c == '\t'; }

/** The byte an escape letter stands for, or 0 if the letter names none. */
static char ini_escape_byte(char letter) {
  switch (letter) {
    case 's': return ' ';
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '\\': return '\\';
    default: return 0;
  }
}

/** Whether @p letter is in the dialect's escape set. */
static bool ini_escape_allowed(const GTEXT_INI_Dialect * dialect, char letter) {
  if (!dialect->escapes) return false;
  for (const char * e = dialect->escapes; *e; e++) {
    if (*e == letter) return true;
  }
  return false;
}

/**
 * Decode escapes from @p raw into a fresh buffer.
 *
 * @param extra An additional letter to accept, or 0. gtext_ini_value_list()
 *   passes `;`, because Desktop Entry §4 defines `\;` only inside a list and a
 *   bare `a\;b` is refused by g_key_file_get_string().
 */
static GTEXT_INI_Status ini_decode(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t raw_len, const GTEXT_Allocator * alloc,
    char extra, char ** out, size_t * out_len) {
  if (!dialect || !out) return GTEXT_INI_E_INVALID;
  if (!raw && raw_len) return GTEXT_INI_E_INVALID;
  *out = NULL;
  if (out_len) *out_len = 0;
  if (!ini_utf8_ok(raw ? raw : "", raw_len)) return GTEXT_INI_E_BAD_UNICODE;
  /* The decoded form is never longer than the raw form: every escape is two
   * bytes in and one out. So one allocation, sized once, and no growth. */
  char * buf = gtext_allocator_malloc(alloc, raw_len + 1);
  if (!buf) return GTEXT_INI_E_OOM;
  size_t w = 0;
  for (size_t i = 0; i < raw_len; i++) {
    if (raw[i] != '\\') {
      buf[w++] = raw[i];
      continue;
    }
    if (i + 1 >= raw_len) {
      /* A trailing lone backslash names no sequence. */
      gtext_allocator_free(alloc, buf);
      return GTEXT_INI_E_BAD_ESCAPE;
    }
    char letter = raw[i + 1];
    if (extra && letter == extra) {
      buf[w++] = extra;
      i++;
      continue;
    }
    if (!ini_escape_allowed(dialect, letter)) {
      gtext_allocator_free(alloc, buf);
      return GTEXT_INI_E_BAD_ESCAPE;
    }
    buf[w++] = ini_escape_byte(letter);
    i++;
  }
  buf[w] = '\0';
  *out = buf;
  if (out_len) *out_len = w;
  return GTEXT_INI_OK;
}

GTEXT_INI_Status gtext_ini_unescape(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t raw_len, const GTEXT_Allocator * alloc,
    char ** out, size_t * out_len) {
  if (!alloc) alloc = gtext_allocator_default();
  return ini_decode(dialect, raw, raw_len, alloc, 0, out, out_len);
}

GTEXT_INI_Status gtext_ini_escape(const GTEXT_INI_Dialect * dialect,
    const char * text, size_t text_len, const GTEXT_Allocator * alloc,
    char ** out, size_t * out_len) {
  if (!dialect || !out) return GTEXT_INI_E_INVALID;
  if (!text && text_len) return GTEXT_INI_E_INVALID;
  if (!alloc) alloc = gtext_allocator_default();
  *out = NULL;
  if (out_len) *out_len = 0;
  char * buf = gtext_allocator_malloc(alloc, text_len * 2 + 1);
  if (!buf) return GTEXT_INI_E_OOM;
  size_t w = 0;
  for (size_t i = 0; i < text_len; i++) {
    char c = text[i];
    /*
     * A leading space has to be escaped and an interior one does not: §3.3
     * discards the run after `=`, so a value that begins with a space would
     * read back shorter. Everything else here has no other spelling at all.
     */
    bool must = (c == '\n' || c == '\r' || c == '\t' || c == '\\') ||
                (c == ' ' && i == 0);
    if (!must) {
      buf[w++] = c;
      continue;
    }
    char letter = c == '\n'   ? 'n'
                  : c == '\r' ? 'r'
                  : c == '\t' ? 't'
                  : c == '\\' ? '\\'
                              : 's';
    if (!ini_escape_allowed(dialect, letter)) {
      /* The dialect cannot spell this byte, and emitting it raw would produce a
       * document that reads back as something else. */
      gtext_allocator_free(alloc, buf);
      return GTEXT_INI_E_UNREPRESENTABLE;
    }
    buf[w++] = '\\';
    buf[w++] = letter;
  }
  buf[w] = '\0';
  *out = buf;
  if (out_len) *out_len = w;
  return GTEXT_INI_OK;
}

void gtext_ini_string_free(const GTEXT_Allocator * alloc, char * text) {
  if (!text) return;
  gtext_allocator_free(alloc ? alloc : gtext_allocator_default(), text);
}

struct GTEXT_INI_List {
  const GTEXT_Allocator * alloc;
  ini_str * items;
  size_t count;
};

GTEXT_INI_Status gtext_ini_value_list(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t raw_len, const GTEXT_Allocator * alloc,
    GTEXT_INI_List ** out) {
  if (!dialect || !out) return GTEXT_INI_E_INVALID;
  if (!raw && raw_len) return GTEXT_INI_E_INVALID;
  *out = NULL;
  char sep = dialect->list_separator;
  if (!sep) return GTEXT_INI_E_INVALID;
  if (!alloc) alloc = gtext_allocator_default();

  /*
   * §4: items are separated by the separator, the value "may be optionally
   * terminated by" one, and "trailing empty strings must always be terminated
   * with a semicolon". So `a;b` and `a;b;` are both two items, and `a;b;;` is
   * three with the last empty: one trailing separator is the terminator and a
   * second one introduces an empty item. Counting the separators and then
   * dropping *one* trailing empty item is what produces that, and it is the
   * asymmetry that makes a naive split wrong.
   */
  size_t count = 1;
  for (size_t i = 0; i < raw_len; i++) {
    if (raw[i] == '\\') {
      i++;
      continue;
    }
    if (raw[i] == sep) count++;
  }
  if (raw_len && raw[raw_len - 1] == sep &&
      (raw_len < 2 || raw[raw_len - 2] != '\\')) {
    count--; /* The terminator, not an empty item. */
  }
  if (!raw_len) count = 0;

  GTEXT_INI_List * list = gtext_allocator_calloc(alloc, 1, sizeof(*list));
  if (!list) return GTEXT_INI_E_OOM;
  list->alloc = alloc;
  list->count = count;
  if (count) {
    list->items = gtext_allocator_calloc(alloc, count, sizeof(*list->items));
    if (!list->items) {
      gtext_allocator_free(alloc, list);
      return GTEXT_INI_E_OOM;
    }
  }

  size_t index = 0;
  size_t start = 0;
  GTEXT_INI_Status status = GTEXT_INI_OK;
  for (size_t i = 0; i <= raw_len && index < count; i++) {
    if (i < raw_len && raw[i] == '\\') {
      i++;
      continue;
    }
    if (i != raw_len && raw[i] != sep) continue;
    char * decoded = NULL;
    size_t decoded_len = 0;
    status = ini_decode(dialect, raw + start, i - start, alloc, sep, &decoded,
        &decoded_len);
    if (status != GTEXT_INI_OK) break;
    list->items[index].data = decoded;
    list->items[index].len = decoded_len;
    index++;
    start = i + 1;
  }
  if (status != GTEXT_INI_OK) {
    gtext_ini_list_free(list);
    return status;
  }
  list->count = index;
  *out = list;
  return GTEXT_INI_OK;
}

size_t gtext_ini_list_count(const GTEXT_INI_List * list) {
  return list ? list->count : 0;
}

const char * gtext_ini_list_at(const GTEXT_INI_List * list, size_t index,
    size_t * len) {
  if (len) *len = 0;
  if (!list || index >= list->count) return NULL;
  if (len) *len = list->items[index].len;
  return list->items[index].data;
}

void gtext_ini_list_free(GTEXT_INI_List * list) {
  if (!list) return;
  const GTEXT_Allocator * alloc = list->alloc;
  for (size_t i = 0; i < list->count; i++) {
    if (list->items[i].data) gtext_allocator_free(alloc, list->items[i].data);
  }
  if (list->items) gtext_allocator_free(alloc, list->items);
  gtext_allocator_free(alloc, list);
}

GTEXT_INI_Status gtext_ini_value_bool(const char * raw, size_t raw_len,
    bool * out) {
  if (!out || (!raw && raw_len)) return GTEXT_INI_E_INVALID;
  /* §4: "must either be the string true or false". Nothing else, and no case
   * folding - §3 says case is significant everywhere in the file. */
  if (raw_len == 4 && memcmp(raw, "true", 4) == 0) {
    *out = true;
    return GTEXT_INI_OK;
  }
  if (raw_len == 5 && memcmp(raw, "false", 5) == 0) {
    *out = false;
    return GTEXT_INI_OK;
  }
  return GTEXT_INI_E_TYPE;
}

/** Copy into a small stack buffer, or the heap when the value is long. */
static char * ini_dup_for_strtod(const char * raw, size_t raw_len,
    char * stack, size_t stack_size, char ** heap) {
  *heap = NULL;
  char * text = stack;
  if (raw_len + 1 > stack_size) {
    /* A number has no bound on how it may be *written* - `0.0000...1` is a
     * valid spelling of a small double - so the buffer cannot be sized for a
     * shortest form. This is the defect the TOML reader had. */
    *heap = gtext_allocator_malloc(gtext_allocator_default(), raw_len + 1);
    if (!*heap) return NULL;
    text = *heap;
  }
  if (raw_len) memcpy(text, raw, raw_len);
  text[raw_len] = '\0';
  return text;
}

GTEXT_INI_Status gtext_ini_value_int(const char * raw, size_t raw_len,
    int64_t * out) {
  if (!out || (!raw && raw_len)) return GTEXT_INI_E_INVALID;
  if (!raw_len) return GTEXT_INI_E_TYPE;
  size_t i = 0;
  bool negative = false;
  if (raw[0] == '-' || raw[0] == '+') {
    negative = raw[0] == '-';
    i = 1;
  }
  if (i >= raw_len) return GTEXT_INI_E_TYPE;
  uint64_t magnitude = 0;
  for (; i < raw_len; i++) {
    if (raw[i] < '0' || raw[i] > '9') return GTEXT_INI_E_TYPE;
    unsigned digit = (unsigned) (raw[i] - '0');
    /* Checked before the multiply, so the overflow never happens rather than
     * being detected after the fact - signed overflow is undefined and an
     * unsigned wrap would make a large value look small. */
    if (magnitude > (UINT64_C(0xFFFFFFFFFFFFFFFF) - digit) / 10) {
      return GTEXT_INI_E_RANGE;
    }
    magnitude = magnitude * 10 + digit;
  }
  if (negative) {
    if (magnitude > (uint64_t) INT64_MAX + 1) return GTEXT_INI_E_RANGE;
    if (magnitude == (uint64_t) INT64_MAX + 1) {
      *out = INT64_MIN;
      return GTEXT_INI_OK;
    }
    *out = -(int64_t) magnitude;
    return GTEXT_INI_OK;
  }
  if (magnitude > (uint64_t) INT64_MAX) return GTEXT_INI_E_RANGE;
  *out = (int64_t) magnitude;
  return GTEXT_INI_OK;
}

GTEXT_INI_Status gtext_ini_value_double(const char * raw, size_t raw_len,
    double * out) {
  if (!out || (!raw && raw_len)) return GTEXT_INI_E_INVALID;
  if (!raw_len) return GTEXT_INI_E_TYPE;
  /*
   * `%f` skips leading whitespace and this does not, deliberately. The parser
   * has already removed the run after `=`, so a stored value never begins with
   * one; accepting it here would accept a spelling no document can produce, and
   * would disagree with gtext_ini_value_int() about the same bytes.
   */
  if (ini_is_space_byte(raw[0])) return GTEXT_INI_E_TYPE;
  /* §4 defines numeric by `%f` in the **C locale**. strtod reads LC_NUMERIC, so
   * a process in a comma-decimal locale would otherwise read `1.5` as 1 and
   * accept `1,5` as one and a half. gtext_text_strtod_c() is the library's
   * locale-independent conversion; see LOCALE-INDEPENDENT-NUMBERS. */
  char stack[64];
  char * heap = NULL;
  char * text = ini_dup_for_strtod(raw, raw_len, stack, sizeof(stack), &heap);
  if (!text) return GTEXT_INI_E_OOM;
  char * end = NULL;
  double value = gtext_number_strtod(text, &end);
  GTEXT_INI_Status status = GTEXT_INI_OK;
  if (end == text || (size_t) (end - text) != raw_len) {
    status = GTEXT_INI_E_TYPE;
  }
  else if (value != value || value > 1.7976931348623157e308 ||
           value < -1.7976931348623157e308) {
    /* `inf` and `nan` are things `%f` reads and a desktop entry has no use for;
     * reporting them as a type error keeps a value the writer cannot spell back
     * out of the tree. An overflowing spelling is a range error. */
    status = errno == ERANGE ? GTEXT_INI_E_RANGE : GTEXT_INI_E_TYPE;
  }
  if (heap) gtext_allocator_free(gtext_allocator_default(), heap);
  if (status == GTEXT_INI_OK) *out = value;
  return status;
}

/** One candidate spelling of a localized key, built into @p buf. */
static const char * ini_locale_try(const GTEXT_INI_Group * group,
    const char * key, size_t key_len, const char * lang, size_t lang_len,
    const char * country, size_t country_len, const char * modifier,
    size_t modifier_len, size_t * len) {
  char buf[256];
  size_t need = key_len + 1 + lang_len + (country_len ? 1 + country_len : 0) +
                (modifier_len ? 1 + modifier_len : 0) + 1 + 1;
  if (need > sizeof(buf)) return NULL; /* No real locale is this long. */
  size_t w = 0;
  memcpy(buf + w, key, key_len);
  w += key_len;
  buf[w++] = '[';
  memcpy(buf + w, lang, lang_len);
  w += lang_len;
  if (country_len) {
    buf[w++] = '_';
    memcpy(buf + w, country, country_len);
    w += country_len;
  }
  if (modifier_len) {
    buf[w++] = '@';
    memcpy(buf + w, modifier, modifier_len);
    w += modifier_len;
  }
  buf[w++] = ']';
  buf[w] = '\0';
  return gtext_ini_group_get(group, buf, len);
}

const char * gtext_ini_group_get_locale(const GTEXT_INI_Group * group,
    const char * key, const char * locale, size_t * len) {
  if (len) *len = 0;
  if (!group || !key) return NULL;
  size_t key_len = strlen(key);
  if (!locale) {
    /* POSIX precedence. A caller wanting none of it passes "". */
    locale = getenv("LC_ALL");
    if (!locale || !*locale) locale = getenv("LC_MESSAGES");
    if (!locale || !*locale) locale = getenv("LANG");
    if (!locale) locale = "";
  }

  /* lang_COUNTRY.ENCODING@MODIFIER, every part after lang optional. The
   * encoding is parsed only so that it can be ignored, which §5 requires. */
  size_t locale_len = strlen(locale);
  size_t lang_len = 0;
  while (lang_len < locale_len && locale[lang_len] != '_' &&
         locale[lang_len] != '.' && locale[lang_len] != '@') {
    lang_len++;
  }
  const char * country = NULL;
  size_t country_len = 0;
  const char * modifier = NULL;
  size_t modifier_len = 0;
  size_t i = lang_len;
  if (i < locale_len && locale[i] == '_') {
    country = locale + i + 1;
    i++;
    while (i < locale_len && locale[i] != '.' && locale[i] != '@') i++;
    country_len = (size_t) (locale + i - country);
  }
  if (i < locale_len && locale[i] == '.') {
    while (i < locale_len && locale[i] != '@') i++;
  }
  if (i < locale_len && locale[i] == '@') {
    modifier = locale + i + 1;
    modifier_len = locale_len - i - 1;
  }

  /*
   * §5's order exactly: lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER,
   * lang, then the unpostfixed key. A candidate whose parts the locale does not
   * have is skipped rather than tried with an empty part - "if LC_MESSAGES does
   * not have a MODIFIER field, then no key with a modifier will be matched".
   */
  const char * found = NULL;
  if (lang_len) {
    if (country_len && modifier_len) {
      found = ini_locale_try(group, key, key_len, locale, lang_len, country,
          country_len, modifier, modifier_len, len);
    }
    if (!found && country_len) {
      found = ini_locale_try(group, key, key_len, locale, lang_len, country,
          country_len, NULL, 0, len);
    }
    if (!found && modifier_len) {
      found = ini_locale_try(group, key, key_len, locale, lang_len, NULL, 0,
          modifier, modifier_len, len);
    }
    if (!found) {
      found = ini_locale_try(group, key, key_len, locale, lang_len, NULL, 0,
          NULL, 0, len);
    }
  }
  if (!found) found = gtext_ini_group_get(group, key, len);
  return found;
}
