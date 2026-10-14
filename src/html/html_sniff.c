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
 * @file
 *
 * The pieces of the HTML standard's encoding sniffing algorithm: BOM sniffing, the `<meta>` prescan with its attribute reader, the extraction
 * of a charset from a `content` attribute, and the XML declaration fallback. Written from the standard as served at
 * https://html.spec.whatwg.org/multipage/parsing.html (13.2.3.2) and urls-and-fetching.html (2.5.3) on 2026-10-13, step by step in its order; the
 * names in the comments are its. No content statistics are looked at: an encoding is only ever one the document or its caller says.
 *
 * The prescan reads at most GTEXT_HTML_PRESCAN_BYTES. Anywhere its position goes past the last byte it has - inside the attribute reader as
 * well as in the loop - it stops, which the standard says is when "runs out of bytes"; the position is only ever advanced by adv().
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "html_sniff_internal.h"

/// Whether `s[0..n)` is the lower-case ASCII string `lower`, ignoring ASCII case.
bool html_ascii_equals_nocase(const char * s, size_t n, const char * lower) {
  const size_t m = strlen(lower);
  if (n != m) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c + 0x20);
    }
    if (c != lower[i]) {
      return false;
    }
  }
  return true;
}

/// The encoding a label names (NULL names none), by the Encoding standard's table.
GTEXT_Encoding html_label_encoding(const char * label) {
  return label ? gtext_encoding_for_label(label, strlen(label)) : GTEXT_ENCODING_NONE;
}

/* ---- BOM sniff ---------------------------------------------------------- */

/// BOM sniffing: the encoding of a leading byte order mark and its length, or NONE.
GTEXT_Encoding html_bom_sniff(const unsigned char * d, size_t n, size_t * length) {
  if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) {
    *length = 3;
    return GTEXT_ENCODING_UTF_8;
  }
  if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) {
    *length = 2;
    return GTEXT_ENCODING_UTF_16BE;
  }
  if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
    *length = 2;
    return GTEXT_ENCODING_UTF_16LE;
  }
  *length = 0;
  return GTEXT_ENCODING_NONE;
}

/* ---- extracting a character encoding from a meta element (2.5.3) ----------- */

static bool is_ascii_ws(unsigned char c) {
  return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

/// The fetch standard's "extract a character encoding from a meta element" over `s[0..n)`, or NONE.
GTEXT_Encoding html_meta_extract_encoding(const char * s, size_t n) {
  /* "Let position be a pointer into s, initially pointing at the start of the string." */
  size_t position = 0;
  for (;;) {
    /* "Loop: Find the first seven characters in s after position that are an ASCII case-insensitive match for the word "charset". If no
       such match is found, return nothing." */
    size_t at = position;
    for (;; at++) {
      if (n < 7 || at > n - 7) {
        return GTEXT_ENCODING_NONE;
      }
      if (html_ascii_equals_nocase(s + at, 7, "charset")) {
        break;
      }
    }
    position = at + 7;
    /* "Skip any ASCII whitespace that immediately follow the word "charset"." */
    while (position < n && is_ascii_ws((unsigned char)s[position])) {
      position++;
    }
    /* "If the next character is not a U+003D EQUALS SIGN (=), then move position to point just before that next character, and jump back
       to the step labeled loop." (At the end of the string there is no next character: the loop finds no match.) */
    if (position >= n || s[position] != '=') {
      continue;
    }
    position++;
    /* "Skip any ASCII whitespace that immediately follow the equals sign." */
    while (position < n && is_ascii_ws((unsigned char)s[position])) {
      position++;
    }
    /* "Process the next character as follows:" */
    if (position >= n) {
      return GTEXT_ENCODING_NONE; /* "If there is no next character: return nothing." */
    }
    const char c = s[position];
    if (c == '"' || c == '\'') {
      /* "If it is a quote and there is a later quote of the same kind in s: return the result of getting an encoding from the substring that is
         between this character and the next earliest occurrence of this character." */
      const char * close = memchr(s + position + 1, c, n - position - 1);
      if (close) {
        return gtext_encoding_for_label(s + position + 1, (size_t)(close - (s + position + 1)));
      }
      return GTEXT_ENCODING_NONE; /* "If it is an unmatched quote: return nothing." */
    }
    /* "Otherwise: return the result of getting an encoding from the substring that consists of this character up to but not including the
       first ASCII whitespace or U+003B SEMICOLON character (;), or the end of s, whichever comes first." */
    size_t end = position;
    while (end < n && !is_ascii_ws((unsigned char)s[end]) && s[end] != ';') {
      end++;
    }
    return gtext_encoding_for_label(s + position, end - position);
  }
}

/* ---- get an XML encoding ------------------------------------------------- */

/// The standard's "get an XML encoding" over the start of the stream, or NONE (failure).
GTEXT_Encoding html_get_xml_encoding(const unsigned char * d, size_t n) {
  /* "Let encodingPosition be a pointer to the start of the stream. If encodingPosition does not point to the start of a byte sequence
     0x3C, 0x3F, 0x78, 0x6D, 0x6C (`<?xml`), then return failure." */
  if (n < 5 || memcmp(d, "<?xml", 5) != 0) {
    return GTEXT_ENCODING_NONE;
  }
  /* "Let xmlDeclarationEnd be a pointer to the next byte in the input byte stream which is 0x3E (>). If there is no such byte, then return
     failure." */
  const unsigned char * gt = memchr(d, '>', n);
  if (!gt) {
    return GTEXT_ENCODING_NONE;
  }
  const size_t decl_end = (size_t)(gt - d);
  /* "Set encodingPosition to the position of the first occurrence of the subsequence of bytes (`encoding`) at or after the current
     encodingPosition and before xmlDeclarationEnd. If there is no such sequence, then return failure." */
  size_t pos = 0;
  for (;; pos++) {
    if (decl_end < 8 || pos > decl_end - 8) {
      return GTEXT_ENCODING_NONE;
    }
    if (memcmp(d + pos, "encoding", 8) == 0) {
      break;
    }
  }
  /* "Advance encodingPosition past the 0x67 (g) byte." */
  pos += 8;
  /* "While the byte at encodingPosition is less than or equal to 0x20, advance encodingPosition to the next byte." */
  while (pos < n && d[pos] <= 0x20) {
    pos++;
  }
  /* "If the byte at encodingPosition is not 0x3D (=), then return failure." (Past the end of the bytes there is no byte.) */
  if (pos >= n || d[pos] != '=') {
    return GTEXT_ENCODING_NONE;
  }
  pos++;
  while (pos < n && d[pos] <= 0x20) {
    pos++;
  }
  /* "Let quoteMark be the byte at encodingPosition. If quoteMark is not either 0x22 (") or 0x27 ('), then return failure." */
  if (pos >= n || (d[pos] != '"' && d[pos] != '\'')) {
    return GTEXT_ENCODING_NONE;
  }
  const unsigned char quote = d[pos];
  pos++;
  /* "Let encodingEndPosition be the position of the next occurrence of quoteMark at or after encodingPosition. If quoteMark does not occur
     again, then return failure." */
  const unsigned char * end = pos < n ? memchr(d + pos, quote, n - pos) : NULL;
  if (!end) {
    return GTEXT_ENCODING_NONE;
  }
  const size_t len = (size_t)(end - (d + pos));
  /* "If potentialEncoding contains one or more bytes whose byte value is 0x20 or below, then return failure." */
  for (size_t i = 0; i < len; i++) {
    if (d[pos + i] <= 0x20) {
      return GTEXT_ENCODING_NONE;
    }
  }
  /* "Let encoding be the result of getting an encoding given potentialEncoding isomorphic decoded. If the encoding is UTF-16BE/LE, then
     change it to UTF-8." */
  GTEXT_Encoding e = gtext_encoding_for_label((const char *)d + pos, len);
  if (e == GTEXT_ENCODING_UTF_16BE || e == GTEXT_ENCODING_UTF_16LE) {
    e = GTEXT_ENCODING_UTF_8;
  }
  return e;
}

/* ---- the prescan --------------------------------------------------------- */

/// The prescan's reading position over `d[0..n)`. `out` is set when the position goes past the last byte.
typedef struct {
  const unsigned char * d; ///< The bytes.
  size_t n;                ///< Their number (at most 1024).
  size_t pos;              ///< The position pointer.
  bool out;                ///< The position went past the last byte.
  size_t work;             ///< Tests: positions advanced and bytes looked at by the scans that do not go through adv().
} Scan;

/// Advance the position to the next byte; "runs out of bytes" if there is none.
static bool adv(Scan * s) {
  s->pos++;
  s->work++;
  if (s->pos >= s->n) {
    s->out = true;
    return false;
  }
  return true;
}

/// The attribute reader's result.
typedef struct {
  bool found;                      ///< An attribute was read (false: "there isn't one").
  unsigned char name[GTEXT_HTML_PRESCAN_BYTES + 1]; ///< The name, in lower case.
  size_t name_len;                 ///< Its length.
  unsigned char value[GTEXT_HTML_PRESCAN_BYTES + 1]; ///< The value, in lower case.
  size_t value_len;                ///< Its length.
} Attribute;

static unsigned char lower(unsigned char b) {
  return (b >= 'A' && b <= 'Z') ? (unsigned char)(b + 0x20) : b;
}

/// "When the prescan a byte stream to determine its encoding algorithm says to get an attribute, it means doing this". False when the
/// prescan ran out of bytes (the caller aborts).
static bool get_attribute(Scan * s, Attribute * a) {
  a->found = false;
  a->name_len = 0;
  a->value_len = 0;
  /* "If the byte at position is one of 0x09, 0x0A, 0x0C, 0x0D, 0x20, or 0x2F, then advance position to the next byte and redo this step." */
  while (s->d[s->pos] == 0x09 || s->d[s->pos] == 0x0A || s->d[s->pos] == 0x0C || s->d[s->pos] == 0x0D || s->d[s->pos] == 0x20
      || s->d[s->pos] == 0x2F) {
    if (!adv(s)) {
      return false;
    }
  }
  /* "If the byte at position is 0x3E (>), then abort the get an attribute algorithm. There isn't one." */
  if (s->d[s->pos] == 0x3E) {
    return true;
  }
  /* "Otherwise, the byte at position is the start of the attribute name." */
  a->found = true;
  for (;;) {
    const unsigned char b = s->d[s->pos];
    if (b == 0x3D && a->name_len > 0) {
      /* "Advance position to the next byte and jump to the step below labeled value." */
      if (!adv(s)) {
        return false;
      }
      goto value;
    }
    if (b == 0x09 || b == 0x0A || b == 0x0C || b == 0x0D || b == 0x20) {
      goto spaces;
    }
    if (b == 0x2F || b == 0x3E) {
      return true; /* the name, and the empty string for a value */
    }
    a->name[a->name_len++] = lower(b); /* (lower case for A-Z; other bytes as they are) */
    if (!adv(s)) {
      return false;
    }
  }
spaces:
  /* "Spaces: If the byte at position is one of 0x09, 0x0A, 0x0C, 0x0D, or 0x20, then advance position to the next byte, then, repeat this step." */
  while (s->d[s->pos] == 0x09 || s->d[s->pos] == 0x0A || s->d[s->pos] == 0x0C || s->d[s->pos] == 0x0D || s->d[s->pos] == 0x20) {
    if (!adv(s)) {
      return false;
    }
  }
  /* "If the byte at position is not 0x3D (=), abort the get an attribute algorithm. The attribute's name is the value of attribute name, its
     value is the empty string." */
  if (s->d[s->pos] != 0x3D) {
    return true;
  }
  /* "Advance position past the 0x3D (=) byte." */
  if (!adv(s)) {
    return false;
  }
value:
  /* "Value: If the byte at position is one of 0x09, 0x0A, 0x0C, 0x0D, or 0x20, then advance position to the next byte, then, repeat this step." */
  while (s->d[s->pos] == 0x09 || s->d[s->pos] == 0x0A || s->d[s->pos] == 0x0C || s->d[s->pos] == 0x0D || s->d[s->pos] == 0x20) {
    if (!adv(s)) {
      return false;
    }
  }
  {
    const unsigned char b = s->d[s->pos];
    if (b == 0x22 || b == 0x27) {
      /* "Quote loop: Advance position to the next byte. If the byte at position is the quote, then advance position to the next byte and abort
         the "get an attribute" algorithm." */
      for (;;) {
        if (!adv(s)) {
          return false;
        }
        if (s->d[s->pos] == b) {
          if (!adv(s)) {
            return false;
          }
          return true;
        }
        a->value[a->value_len++] = lower(s->d[s->pos]);
      }
    }
    if (b == 0x3E) {
      return true; /* the name, and the empty string */
    }
    a->value[a->value_len++] = lower(b);
    if (!adv(s)) {
      return false;
    }
  }
  /* "Process the byte at position as follows:" (an unquoted value) */
  for (;;) {
    const unsigned char b = s->d[s->pos];
    if (b == 0x09 || b == 0x0A || b == 0x0C || b == 0x0D || b == 0x20 || b == 0x3E) {
      return true;
    }
    a->value[a->value_len++] = lower(b);
    if (!adv(s)) {
      return false;
    }
  }
}

/// The prescan's `charset` variable: the null value, failure, or an encoding.
typedef enum { CS_NULL, CS_FAILURE, CS_ENCODING } CharsetState;

/// The prescan of the first 1024 bytes of `d[0..n)`: the encoding it finds, or NONE.
GTEXT_Encoding html_prescan(const unsigned char * d, size_t n) {
  return html_prescan_counted(d, n, NULL);
}

GTEXT_Encoding html_prescan_counted(const unsigned char * d, size_t n, size_t * work) {
  if (work) {
    *work = 0;
  }
  if (n > GTEXT_HTML_PRESCAN_BYTES) {
    n = GTEXT_HTML_PRESCAN_BYTES; /* "the end condition" */
  }
  /* "Prescan for UTF-16 XML declarations: if position points to a sequence of bytes starting with 0x3C, 0x0, 0x3F, 0x0, 0x78, 0x0 (UTF-16
     little-endian '<?x'): return UTF-16LE; with 0x0, 0x3C, 0x0, 0x3F, 0x0, 0x78: return UTF-16BE." */
  if (n >= 6 && d[0] == 0x3C && d[1] == 0 && d[2] == 0x3F && d[3] == 0 && d[4] == 0x78 && d[5] == 0) {
    return GTEXT_ENCODING_UTF_16LE;
  }
  if (n >= 6 && d[0] == 0 && d[1] == 0x3C && d[2] == 0 && d[3] == 0x3F && d[4] == 0 && d[5] == 0x78) {
    return GTEXT_ENCODING_UTF_16BE;
  }
  Scan s = {d, n, 0, n == 0, 0};
  GTEXT_Encoding result = GTEXT_ENCODING_NONE;
  while (!s.out) {
    /* "Loop: If position points to:" */
    const size_t left = s.n - s.pos;
    const unsigned char * p = d + s.pos;
    if (left >= 4 && memcmp(p, "<!--", 4) == 0) {
      /* "Advance the position pointer so that it points at the first 0x3E byte which is preceded by two 0x2D bytes (i.e. at the end of an ASCII
         '-->' sequence) and comes after the 0x3C byte that was found. (The two 0x2D bytes can be the same as those in the '<!--' sequence.)" */
      size_t j = s.pos + 4;
      for (; j < s.n; j++) {
        if (d[j] == 0x3E && d[j - 1] == 0x2D && d[j - 2] == 0x2D) {
          break;
        }
      }
      s.work += j - s.pos;
      if (j >= s.n) {
        break; /* ran out of bytes */
      }
      s.pos = j;
    }
    else if (left >= 6 && p[0] == '<' && (p[1] | 0x20) == 'm' && (p[2] | 0x20) == 'e' && (p[3] | 0x20) == 't' && (p[4] | 0x20) == 'a'
        && (p[5] == 0x09 || p[5] == 0x0A || p[5] == 0x0C || p[5] == 0x0D || p[5] == 0x20 || p[5] == 0x2F)) {
      /* "Advance the position pointer so that it points at the next 0x09, 0x0A, 0x0C, 0x0D, 0x20, or 0x2F byte (the one in sequence of
         characters matched above)." */
      s.pos += 5;
      /* "Let attribute list be an empty list of strings. Let got pragma be false. Let need pragma be null. Let charset be the null value." */
      unsigned char names[GTEXT_HTML_PRESCAN_BYTES + 8];
      size_t names_len = 0;
      bool got_pragma = false;
      int need_pragma = -1; /* null */
      CharsetState charset_state = CS_NULL;
      GTEXT_Encoding charset = GTEXT_ENCODING_NONE;
      Attribute a;
      for (;;) {
        /* "Attributes: Get an attribute and its value. If no attribute was sniffed, then jump to the processing step below." */
        if (!get_attribute(&s, &a)) {
          break;
        }
        if (!a.found) {
          break;
        }
        /* "If the attribute's name is already in attribute list, then return to the step labeled attributes." */
        bool seen = false;
        s.work += names_len;
        for (size_t i = 0; i < names_len && !seen;) {
          const size_t len = names[i];
          if (len == a.name_len && memcmp(names + i + 1, a.name, len) == 0) {
            seen = true;
          }
          i += len + 1;
        }
        if (seen) {
          continue;
        }
        /* "Add the attribute's name to attribute list." (Names are at most 255 bytes here; a longer one cannot match a name that is looked for.) */
        if (a.name_len < 256 && names_len + a.name_len + 1 <= sizeof(names)) {
          names[names_len++] = (unsigned char)a.name_len;
          memcpy(names + names_len, a.name, a.name_len);
          names_len += a.name_len;
        }
        /* "Run the appropriate step from the following list, if one applies:" */
        if (a.name_len == 10 && memcmp(a.name, "http-equiv", 10) == 0) {
          if (a.value_len == 12 && memcmp(a.value, "content-type", 12) == 0) {
            got_pragma = true;
          }
        }
        else if (a.name_len == 7 && memcmp(a.name, "content", 7) == 0) {
          const GTEXT_Encoding e = html_meta_extract_encoding((const char *)a.value, a.value_len);
          if (e != GTEXT_ENCODING_NONE && charset_state == CS_NULL) {
            charset_state = CS_ENCODING;
            charset = e;
            need_pragma = 1;
          }
        }
        else if (a.name_len == 7 && memcmp(a.name, "charset", 7) == 0) {
          const GTEXT_Encoding e = gtext_encoding_for_label((const char *)a.value, a.value_len);
          charset_state = e == GTEXT_ENCODING_NONE ? CS_FAILURE : CS_ENCODING;
          charset = e;
          need_pragma = 0;
        }
      }
      if (s.out) {
        break;
      }
      /* "Processing: If need pragma is null, then jump to the step below labeled next byte. If need pragma is true but got pragma is false,
         then jump to the step below labeled next byte. If charset is failure, then jump to the step below labeled next byte." */
      if (need_pragma >= 0 && !(need_pragma == 1 && !got_pragma) && charset_state == CS_ENCODING) {
        /* "If charset is UTF-16BE/LE, then set charset to UTF-8. If charset is x-user-defined, then set charset to windows-1252. Return charset." */
        if (charset == GTEXT_ENCODING_UTF_16BE || charset == GTEXT_ENCODING_UTF_16LE) {
          charset = GTEXT_ENCODING_UTF_8;
        }
        else if (charset == GTEXT_ENCODING_X_USER_DEFINED) {
          charset = GTEXT_ENCODING_WINDOWS_1252;
        }
        result = charset;
        goto done;
      }
    }
    else if (left >= 2 && p[0] == '<' && ((left >= 3 && p[1] == '/' && ((p[2] | 0x20) >= 'a' && (p[2] | 0x20) <= 'z'))
                                              || ((p[1] | 0x20) >= 'a' && (p[1] | 0x20) <= 'z'))) {
      /* "Advance the position pointer so that it points at the next 0x09, 0x0A, 0x0C, 0x0D, 0x20, or 0x3E byte." (From the byte after the `<`
         and the optional `/`, which is a letter: the name starts there.) */
      s.pos += (p[1] == '/') ? 2 : 1;
      while (s.d[s.pos] != 0x09 && s.d[s.pos] != 0x0A && s.d[s.pos] != 0x0C && s.d[s.pos] != 0x0D && s.d[s.pos] != 0x20
          && s.d[s.pos] != 0x3E) {
        if (!adv(&s)) {
          break;
        }
      }
      if (s.out) {
        break;
      }
      /* "Repeatedly get an attribute until no further attributes can be found, then jump to the step below labeled next byte." */
      Attribute a;
      for (;;) {
        if (!get_attribute(&s, &a)) {
          break;
        }
        if (!a.found) {
          break;
        }
      }
      if (s.out) {
        break;
      }
    }
    else if (left >= 2 && p[0] == '<' && (p[1] == '!' || p[1] == '/' || p[1] == '?')) {
      /* "Advance the position pointer so that it points at the first 0x3E byte (>) that comes after the 0x3C byte that was found." */
      const unsigned char * gt = memchr(p + 1, '>', left - 1);
      s.work += gt ? (size_t)(gt - p) : left;
      if (!gt) {
        break;
      }
      s.pos = (size_t)(gt - d);
    }
    /* "Any other byte: Do nothing with that byte." */
    /* "Next byte: Move position so it points at the next byte in the input byte stream, and return to the step above labeled loop." */
    if (!adv(&s)) {
      break;
    }
  }
  /* "If at any point ... the user agent either runs out of bytes ... or reaches its end condition, then abort the prescan a byte stream to
     determine its encoding algorithm and return the result get an XML encoding applied to the same bytes." */
  result = html_get_xml_encoding(d, n);
done:
  if (work) {
    *work = s.work + n; /* (the fallback reads the same window once more) */
  }
  return result;
}
