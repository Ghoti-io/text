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
 * Internationalized host names: IDNA2008, RFC 5890 to 5893.
 *
 * The shape of the thing: a host name is labels separated by dots; each label
 * is an A-label (`xn--` followed by Punycode), an ASCII LDH label, or a
 * U-label. An A-label is decoded and then checked exactly as a U-label would
 * be, because that is what it is - RFC 5891 section 4.4 says an A-label is
 * valid only if it round-trips to a valid U-label, so a validator that
 * accepts `xn--` and any trailing base-36 is not checking the thing the
 * keyword names.
 *
 * A U-label is checked against the derived property of RFC 5892 - PVALID,
 * DISALLOWED, or one of the two contextual classes - plus that appendix's
 * contextual rules and RFC 5893's bidi rule. The tables under tables/ are
 * generated from the UCD; everything here is the algorithm that reads them.
 *
 * An internationalized name is mapped before it is validated. UTS #46's
 * mapping step, from tables/uts46_tables.c, folds or removes the characters
 * that table names; the result is put into Normalization Form C; RFC 5892
 * then says whether what remains is valid. A plain ASCII host name is not
 * mapped. A character the mapping table does not list is left alone, which is
 * what an unlisted character means and what keeps a mapping table behind the
 * UCD from inventing a fold the Consortium has not published.
 */

#include <string.h>

#include "idna_internal.h"
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/enums.h>

#include "nfc_internal.h"
#include "tables/tables_internal.h"

#define IDNA_MAX_LABEL_CODEPOINTS 256

/*
 * How many codepoints a name may have once UTS #46 has mapped it.
 *
 * Not an arbitrary ceiling. Punycode never emits fewer characters than it was
 * given - a basic codepoint is copied and a non-basic one costs at least one
 * delta character - so a name of more than 253 codepoints cannot encode to the
 * 253 octets a domain name is allowed. Anything past this bound is already
 * invalid, and stopping here rather than after the encoding keeps the working
 * buffers small enough to live on the stack.
 */
#define IDNA_MAX_MAPPED_CODEPOINTS 256

/* UAX #15 gives four characters as the largest canonical expansion of one. */
#define IDNA_MAX_DECOMPOSED_CODEPOINTS (IDNA_MAX_MAPPED_CODEPOINTS * 4)
#define IDNA_MAX_NAME_OCTETS 253
#define IDNA_MAX_LABEL_OCTETS 63

// ===========================================================================
// Table lookup
// ===========================================================================

/* The ranges are sorted and disjoint, so this is a binary search; `absent` is
 * what a codepoint no range mentions means. */
static uint32_t idna_lookup(const GTEXT_IDNA_Range * ranges, size_t count,
    uint32_t cp, uint32_t absent) {
  size_t lo = 0;
  size_t hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cp < ranges[mid].lo) {
      hi = mid;
    }
    else if (cp > ranges[mid].hi) {
      lo = mid + 1;
    }
    else {
      return ranges[mid].value;
    }
  }
  return absent;
}

/* A codepoint no range mentions is unassigned, and for the only question
 * asked here that is the same answer as disallowed. */
static uint32_t idna_property(uint32_t cp) {
  return idna_lookup(gtext_idna_derived, gtext_idna_derived_count, cp,
      GTEXT_IDNA_DISALLOWED);
}

/* Script, Joining_Type, Bidi_Class and the virama test used to come from four
 * tables generated here - 1,290 of idna_tables.c's 1,948 lines - which were
 * the UCD's own data narrowed to the values RFC 5892's contextual rules and
 * RFC 5893's bidi rule name. They come from ghoti.io-unicode now. Only the
 * IDNA2008 derived property is still generated here, because that one is
 * RFC 5892's own and not a Unicode property.
 *
 * Narrowing those four to the values the rules mention was safe only because
 * every rule below is written as an allow-list with a `default: return 0`:
 * a class the old table called OTHER and a class it had never heard of both
 * fall to the same arm. That is still how they read, so widening them back to
 * the UCD's full sets changes no answer - and it is why this could be a
 * substitution rather than a rewrite.
 *
 * A virama is Canonical_Combining_Class 9, which is what the old table held. */
static int idna_is_virama(uint32_t cp) {
  return guni_combining_class(cp) == 9;
}

// ===========================================================================
// UTF-8
// ===========================================================================

/*
 * Decode one character, or return 0 and leave `pos` alone.
 *
 * Strict: an overlong form, a surrogate and a value past U+10FFFF are all
 * refused rather than repaired, because a host name that needs repairing is
 * not a host name.
 */
static uint32_t idna_utf8_next(const char * s, size_t len, size_t * pos) {
  unsigned char c = (unsigned char)s[*pos];
  size_t extra;
  uint32_t cp;

  if (c < 0x80) {
    *pos += 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0) {
    extra = 1;
    cp = c & 0x1Fu;
  }
  else if ((c & 0xF0) == 0xE0) {
    extra = 2;
    cp = c & 0x0Fu;
  }
  else if ((c & 0xF8) == 0xF0) {
    extra = 3;
    cp = c & 0x07u;
  }
  else {
    return 0;
  }
  if (*pos + extra >= len + 0 && *pos + extra > len - 1) {
    return 0;
  }
  for (size_t i = 1; i <= extra; i++) {
    unsigned char cc = (unsigned char)s[*pos + i];
    if ((cc & 0xC0) != 0x80) {
      return 0;
    }
    cp = (cp << 6) | (cc & 0x3Fu);
  }
  if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800)
      || (extra == 3 && cp < 0x10000)) {
    return 0; // overlong
  }
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return 0;
  }
  *pos += extra + 1;
  return cp;
}

// ===========================================================================
// Punycode, RFC 3492
// ===========================================================================

#define PUNY_BASE 36
#define PUNY_TMIN 1
#define PUNY_TMAX 26
#define PUNY_SKEW 38
#define PUNY_DAMP 700
#define PUNY_INITIAL_BIAS 72
#define PUNY_INITIAL_N 128

static uint32_t puny_adapt(uint32_t delta, uint32_t numpoints, int firsttime) {
  uint32_t k = 0;
  delta = firsttime ? delta / PUNY_DAMP : delta / 2;
  delta += delta / numpoints;
  while (delta > ((PUNY_BASE - PUNY_TMIN) * PUNY_TMAX) / 2) {
    delta /= PUNY_BASE - PUNY_TMIN;
    k += PUNY_BASE;
  }
  return k + (PUNY_BASE - PUNY_TMIN + 1) * delta / (delta + PUNY_SKEW);
}

static int puny_digit(unsigned char c, uint32_t * out) {
  if (c >= 'a' && c <= 'z') {
    *out = c - 'a';
    return 1;
  }
  if (c >= 'A' && c <= 'Z') {
    *out = c - 'A';
    return 1;
  }
  if (c >= '0' && c <= '9') {
    *out = c - '0' + 26;
    return 1;
  }
  return 0;
}

static char puny_char(uint32_t d) {
  return (char)(d < 26 ? 'a' + d : '0' + (d - 26));
}

/* Decode the part after "xn--". Returns 1 on success. */
static int puny_decode(
    const char * in, size_t len, uint32_t * out, size_t * out_len) {
  uint32_t n = PUNY_INITIAL_N;
  uint32_t i = 0;
  uint32_t bias = PUNY_INITIAL_BIAS;
  size_t written = 0;
  size_t start = 0;

  /* Everything before the last delimiter is literal ASCII. */
  for (size_t k = 0; k < len; k++) {
    if (in[k] == '-') {
      start = k + 1;
    }
  }
  for (size_t k = 0; k + 1 <= start && k < (start ? start - 1 : 0); k++) {
    unsigned char c = (unsigned char)in[k];
    if (c >= 0x80) {
      return 0;
    }
    if (written >= IDNA_MAX_LABEL_CODEPOINTS) {
      return 0;
    }
    out[written++] = c;
  }

  size_t at = start;
  while (at < len) {
    uint32_t oldi = i;
    uint32_t w = 1;
    for (uint32_t k = PUNY_BASE;; k += PUNY_BASE) {
      if (at >= len) {
        return 0;
      }
      uint32_t digit;
      if (!puny_digit((unsigned char)in[at++], &digit)) {
        return 0;
      }
      if (digit > (0xFFFFFFFFu - i) / w) {
        return 0; // overflow
      }
      i += digit * w;
      uint32_t t = k <= bias ? PUNY_TMIN
          : (k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias);
      if (digit < t) {
        break;
      }
      if (w > 0xFFFFFFFFu / (PUNY_BASE - t)) {
        return 0;
      }
      w *= PUNY_BASE - t;
    }
    bias = puny_adapt(i - oldi, (uint32_t)written + 1, oldi == 0);
    if (i / (written + 1) > 0xFFFFFFFFu - n) {
      return 0;
    }
    n += i / (uint32_t)(written + 1);
    i %= (uint32_t)(written + 1);
    if (n > 0x10FFFF || (n >= 0xD800 && n <= 0xDFFF)) {
      return 0;
    }
    if (written >= IDNA_MAX_LABEL_CODEPOINTS) {
      return 0;
    }
    memmove(out + i + 1, out + i, (written - i) * sizeof(*out));
    out[i] = n;
    written++;
    i++;
  }
  *out_len = written;
  return 1;
}

/*
 * Encode, so that a decoded A-label can be checked against the label it came
 * from. RFC 5891 section 4.4 requires the round trip: `xn---9uc` decodes to
 * something whose own encoding is different, and a decoder alone would take
 * it. The encoder is also what measures a U-label's A-label form against the
 * 63-octet limit, which is a limit on the encoded form and not on the text.
 */
static int puny_encode(const uint32_t * in, size_t len, char * out,
    size_t capacity, size_t * out_len) {
  uint32_t n = PUNY_INITIAL_N;
  uint32_t delta = 0;
  uint32_t bias = PUNY_INITIAL_BIAS;
  size_t written = 0;
  size_t handled = 0;

  for (size_t k = 0; k < len; k++) {
    if (in[k] < 0x80) {
      if (written >= capacity) {
        return 0;
      }
      out[written++] = (char)in[k];
      handled++;
    }
  }
  size_t basic = handled;
  if (basic > 0) {
    if (written >= capacity) {
      return 0;
    }
    out[written++] = '-';
  }

  while (handled < len) {
    uint32_t m = 0xFFFFFFFFu;
    for (size_t k = 0; k < len; k++) {
      if (in[k] >= n && in[k] < m) {
        m = in[k];
      }
    }
    if (m - n > (0xFFFFFFFFu - delta) / (uint32_t)(handled + 1)) {
      return 0;
    }
    delta += (m - n) * (uint32_t)(handled + 1);
    n = m;
    for (size_t k = 0; k < len; k++) {
      if (in[k] < n) {
        if (++delta == 0) {
          return 0;
        }
        continue;
      }
      if (in[k] != n) {
        continue;
      }
      uint32_t q = delta;
      for (uint32_t k2 = PUNY_BASE;; k2 += PUNY_BASE) {
        uint32_t t = k2 <= bias ? PUNY_TMIN
            : (k2 >= bias + PUNY_TMAX ? PUNY_TMAX : k2 - bias);
        if (q < t) {
          break;
        }
        if (written >= capacity) {
          return 0;
        }
        out[written++] = puny_char(t + (q - t) % (PUNY_BASE - t));
        q = (q - t) / (PUNY_BASE - t);
      }
      if (written >= capacity) {
        return 0;
      }
      out[written++] = puny_char(q);
      bias = puny_adapt(delta, (uint32_t)(handled + 1), handled == basic);
      delta = 0;
      handled++;
    }
    delta++;
    n++;
  }
  *out_len = written;
  return 1;
}

// ===========================================================================
// RFC 5892 appendix A, the contextual rules
// ===========================================================================

static int idna_contextj(const uint32_t * cps, size_t len, size_t at) {
  uint32_t cp = cps[at];
  if (cp == 0x200D) { // ZERO WIDTH JOINER, rule A.2
    return at > 0 && idna_is_virama(cps[at - 1]);
  }
  if (cp != 0x200C) {
    return 0;
  }
  // ZERO WIDTH NON-JOINER, rule A.1: a virama before it, or the joining-type
  // pattern (L|D) T* ZWNJ T* (R|D) around it.
  if (at > 0 && idna_is_virama(cps[at - 1])) {
    return 1;
  }
  size_t i = at;
  while (i > 0 && guni_joining_type(cps[i - 1]) == GUNI_JT_T) {
    i--;
  }
  if (i == 0) {
    return 0;
  }
  uint32_t before = guni_joining_type(cps[i - 1]);
  if (before != GUNI_JT_L && before != GUNI_JT_D) {
    return 0;
  }
  size_t j = at + 1;
  while (j < len && guni_joining_type(cps[j]) == GUNI_JT_T) {
    j++;
  }
  if (j >= len) {
    return 0;
  }
  uint32_t after = guni_joining_type(cps[j]);
  return after == GUNI_JT_R || after == GUNI_JT_D;
}

static int idna_contexto(const uint32_t * cps, size_t len, size_t at) {
  uint32_t cp = cps[at];
  switch (cp) {
  case 0x00B7: // MIDDLE DOT, rule A.3
    return at > 0 && at + 1 < len && cps[at - 1] == 0x006C
        && cps[at + 1] == 0x006C;
  case 0x0375: // GREEK LOWER NUMERAL SIGN, rule A.4
    return at + 1 < len && guni_script(cps[at + 1]) == GUNI_SCRIPT_GREEK;
  case 0x05F3: // HEBREW PUNCTUATION GERESH, rule A.5
  case 0x05F4: // HEBREW PUNCTUATION GERSHAYIM, rule A.6
    return at > 0 && guni_script(cps[at - 1]) == GUNI_SCRIPT_HEBREW;
  case 0x30FB: { // KATAKANA MIDDLE DOT, rule A.7
    for (size_t i = 0; i < len; i++) {
      uint32_t script = guni_script(cps[i]);
      if (script == GUNI_SCRIPT_HIRAGANA
          || script == GUNI_SCRIPT_KATAKANA
          || script == GUNI_SCRIPT_HAN) {
        return 1;
      }
    }
    return 0;
  }
  default:
    break;
  }
  if (cp >= 0x0660 && cp <= 0x0669) { // ARABIC-INDIC DIGITS, rule A.8
    for (size_t i = 0; i < len; i++) {
      if (cps[i] >= 0x06F0 && cps[i] <= 0x06F9) {
        return 0;
      }
    }
    return 1;
  }
  if (cp >= 0x06F0 && cp <= 0x06F9) { // EXTENDED ARABIC-INDIC, rule A.9
    for (size_t i = 0; i < len; i++) {
      if (cps[i] >= 0x0660 && cps[i] <= 0x0669) {
        return 0;
      }
    }
    return 1;
  }
  return 0;
}

// ===========================================================================
// RFC 5893, the bidi rule
// ===========================================================================

static int idna_label_has_rtl(const uint32_t * cps, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint32_t class = guni_bidi_class(cps[i]);
    if (class == GUNI_BIDI_R || class == GUNI_BIDI_AL
        || class == GUNI_BIDI_AN) {
      return 1;
    }
  }
  return 0;
}

/*
 * RFC 5893 section 2, applied to every label once any label in the name has
 * shown the name to be a bidi domain name. The direction of a label is the
 * class of its first character, which is why "0a.א" fails: the name is a bidi
 * domain name because of the second label, and the first label then starts
 * with a digit, which is neither L nor R nor AL.
 */
static int idna_bidi_label_valid(const uint32_t * cps, size_t len) {
  if (len == 0) {
    return 0;
  }
  uint32_t first = guni_bidi_class(cps[0]);
  size_t last = len;
  while (last > 0 && guni_bidi_class(cps[last - 1]) == GUNI_BIDI_NSM) {
    last--; // trailing combining marks do not decide the ending class
  }
  if (last == 0) {
    return 0;
  }
  uint32_t ending = guni_bidi_class(cps[last - 1]);

  if (first == GUNI_BIDI_R || first == GUNI_BIDI_AL) {
    int has_en = 0;
    int has_an = 0;
    for (size_t i = 0; i < len; i++) {
      switch (guni_bidi_class(cps[i])) {
      case GUNI_BIDI_R:
      case GUNI_BIDI_AL:
      case GUNI_BIDI_ES:
      case GUNI_BIDI_CS:
      case GUNI_BIDI_ET:
      case GUNI_BIDI_ON:
      case GUNI_BIDI_BN:
      case GUNI_BIDI_NSM:
        break;
      case GUNI_BIDI_EN:
        has_en = 1;
        break;
      case GUNI_BIDI_AN:
        has_an = 1;
        break;
      default:
        return 0;
      }
    }
    if (has_en && has_an) {
      return 0; // rule 4: not both kinds of digit
    }
    return ending == GUNI_BIDI_R || ending == GUNI_BIDI_AL
        || ending == GUNI_BIDI_EN || ending == GUNI_BIDI_AN;
  }
  if (first == GUNI_BIDI_L) {
    for (size_t i = 0; i < len; i++) {
      switch (guni_bidi_class(cps[i])) {
      case GUNI_BIDI_L:
      case GUNI_BIDI_EN:
      case GUNI_BIDI_ES:
      case GUNI_BIDI_CS:
      case GUNI_BIDI_ET:
      case GUNI_BIDI_ON:
      case GUNI_BIDI_BN:
      case GUNI_BIDI_NSM:
        break;
      default:
        return 0;
      }
    }
    return ending == GUNI_BIDI_L || ending == GUNI_BIDI_EN;
  }
  return 0; // rule 1: a label starts L, R or AL and nothing else
}

// ===========================================================================
// Labels
// ===========================================================================

/* Mn, Mc and Me, which a label may not start with (RFC 5891 section 4.2.3.2).
 * The derived table does not carry General_Category, but every combining mark
 * that is PVALID is one of these, and the rule only bites on a mark that
 * would otherwise be allowed - so the question is asked of the one property
 * that is here: a leading character whose bidi class is NSM, or whose
 * canonical combining class is non-zero. */
static int idna_is_leading_mark(uint32_t cp) {
  return guni_bidi_class(cp) == GUNI_BIDI_NSM || idna_is_virama(cp)
      || (cp >= 0x0900 && cp <= 0x0903) || (cp >= 0x093A && cp <= 0x093C)
      || (cp >= 0x093E && cp <= 0x094F) || (cp >= 0x0951 && cp <= 0x0957);
}

/*
 * A U-label, as codepoints. `bidi_domain` says whether any label in the name
 * carried a right-to-left character, which is a property of the whole name
 * and so cannot be decided here.
 */
static int idna_u_label_valid(
    const uint32_t * cps, size_t len, int bidi_domain) {
  if (len == 0) {
    return 0;
  }
  /* RFC 5891 section 4.2.3.1: no leading or trailing hyphen, and not a
   * hyphen in both the third and fourth positions - that pattern is reserved
   * for the ACE prefix and labels like `aa--bb` are not names. */
  if (cps[0] == '-' || cps[len - 1] == '-') {
    return 0;
  }
  if (len >= 4 && cps[2] == '-' && cps[3] == '-') {
    return 0;
  }
  if (idna_is_leading_mark(cps[0])) {
    return 0;
  }

  for (size_t i = 0; i < len; i++) {
    switch (idna_property(cps[i])) {
    case GTEXT_IDNA_PVALID:
      break;
    case GTEXT_IDNA_CONTEXTJ:
      if (!idna_contextj(cps, len, i)) {
        return 0;
      }
      break;
    case GTEXT_IDNA_CONTEXTO:
      if (!idna_contexto(cps, len, i)) {
        return 0;
      }
      break;
    default:
      return 0;
    }
  }
  if (bidi_domain && !idna_bidi_label_valid(cps, len)) {
    return 0;
  }
  return 1;
}

/* An ASCII LDH label: letters, digits and hyphens, not starting or ending
 * with a hyphen. */
static int idna_ldh_label_valid(const char * s, size_t len) {
  if (len == 0 || len > IDNA_MAX_LABEL_OCTETS) {
    return 0;
  }
  if (s[0] == '-' || s[len - 1] == '-') {
    return 0;
  }
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '-') {
      continue;
    }
    return 0;
  }
  return 1;
}

static int idna_has_ace_prefix(const char * s, size_t len) {
  return len >= 4 && (s[0] == 'x' || s[0] == 'X')
      && (s[1] == 'n' || s[1] == 'N') && s[2] == '-' && s[3] == '-';
}

/*
 * Decode one label to codepoints. An A-label is decoded from Punycode and
 * required to re-encode to itself; anything else is read as UTF-8.
 */
static int idna_label_codepoints(const char * s, size_t len, uint32_t * cps,
    size_t * out_len, int * was_a_label) {
  *was_a_label = 0;
  if (idna_has_ace_prefix(s, len)) {
    char lower[IDNA_MAX_LABEL_OCTETS + 1];
    if (len > IDNA_MAX_LABEL_OCTETS) {
      return 0;
    }
    for (size_t i = 0; i < len; i++) {
      char c = s[i];
      lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    if (!idna_ldh_label_valid(s, len)) {
      return 0;
    }
    if (!puny_decode(lower + 4, len - 4, cps, out_len)) {
      return 0;
    }
    if (*out_len == 0) {
      return 0;
    }
    /* RFC 5891 section 4.4: the decoded label must encode back to exactly
     * what was written, which is what refuses a non-canonical encoding. And
     * a label that decodes to pure ASCII was never an A-label at all. */
    char again[IDNA_MAX_LABEL_OCTETS * 2];
    size_t again_len = 0;
    if (!puny_encode(cps, *out_len, again, sizeof(again), &again_len)) {
      return 0;
    }
    if (again_len != len - 4 || memcmp(again, lower + 4, again_len) != 0) {
      return 0;
    }
    int all_ascii = 1;
    for (size_t i = 0; i < *out_len; i++) {
      if (cps[i] >= 0x80) {
        all_ascii = 0;
        break;
      }
    }
    if (all_ascii) {
      return 0;
    }
    /*
     * UTS #46 section 4.1, criterion 1: the decoded label must already be in
     * NFC. A U-label reaches this function normalised, because the mapping
     * step normalised the whole name before it was split; an A-label does
     * not, because its bytes were Punycode and nothing looked inside them.
     * It is refused rather than normalised - an A-label is a spelling of one
     * exact U-label, and one that decodes to a different string than it
     * claims is not that label.
     */
    uint32_t normalized[IDNA_MAX_DECOMPOSED_CODEPOINTS];
    size_t normalized_len = 0;
    if (!gtext_nfc(cps, *out_len, normalized, sizeof(normalized) / sizeof(normalized[0]),
            &normalized_len)) {
      return 0;
    }
    if (normalized_len != *out_len
        || memcmp(normalized, cps, *out_len * sizeof(uint32_t)) != 0) {
      return 0;
    }
    *was_a_label = 1;
    return 1;
  }

  size_t at = 0;
  size_t written = 0;
  while (at < len) {
    size_t before = at;
    uint32_t cp = idna_utf8_next(s, len, &at);
    if (cp == 0 && at == before) {
      return 0;
    }
    if (written >= IDNA_MAX_LABEL_CODEPOINTS) {
      return 0;
    }
    cps[written++] = cp;
  }
  *out_len = written;
  return 1;
}

// ===========================================================================
// Host names
// ===========================================================================

/*
 * The only label separator, U+002E FULL STOP.
 *
 * UTS #46 section 4.5 treats three others as separators too - `a。b` is two
 * labels - and they used to be listed here because the mapping step was not
 * implemented. They are not listed any more: the mapping step maps all three
 * to FULL STOP before the name is ever split, which is where that rule
 * actually lives. The generator checks that it still does.
 */
static int idna_is_separator(uint32_t cp) {
  return cp == '.';
}

/* Write one codepoint as UTF-8. Returns how many bytes, or 0 if it does not
 * fit or is not encodable. */
static size_t idna_utf8_put(uint32_t cp, char * out, size_t cap) {
  if (cp < 0x80) {
    if (cap < 1) {
      return 0;
    }
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    if (cap < 2) {
      return 0;
    }
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    if (cap < 3 || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return 0;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp <= 0x10FFFF) {
    if (cap < 4) {
      return 0;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
  }
  return 0;
}

/* What UTS #46's mapping step does to `cp`, or 0 if it leaves it alone. */
static const GTEXT_UTS46_Entry * idna_uts46_lookup(uint32_t cp) {
  size_t lo = 0;
  size_t hi = gtext_uts46_map_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cp < gtext_uts46_map[mid].cp) {
      hi = mid;
    }
    else if (cp > gtext_uts46_map[mid].cp) {
      lo = mid + 1;
    }
    else {
      return &gtext_uts46_map[mid];
    }
  }
  return NULL;
}

/*
 * UTS #46 section 4, step 1: map.
 *
 * Nontransitional processing, which is what every current browser does. The
 * difference is the four deviation characters - sharp s, final sigma and the
 * two zero-width joiners - which transitional processing folds away and this
 * leaves alone, so that `faß.example` is not silently the same name as
 * `fass.example`. They need no table entry precisely because nothing happens
 * to them.
 *
 * A character the table does not mention is left alone. That is the table's
 * own rule, and it is why a mapping table older than the UCD does not change
 * a validity answer: a character too new for the mapping table is one the
 * mapping step would not have touched.
 */
static int idna_uts46_map(const char * text, size_t len, uint32_t * out,
    size_t cap, size_t * out_len) {
  size_t at = 0;
  size_t written = 0;
  while (at < len) {
    size_t before = at;
    uint32_t cp = idna_utf8_next(text, len, &at);
    if (cp == 0 && at == before) {
      return 0; // malformed UTF-8
    }
    const GTEXT_UTS46_Entry * entry = idna_uts46_lookup(cp);
    if (!entry) {
      if (written >= cap) {
        return 0;
      }
      out[written++] = cp;
      continue;
    }
    if (entry->status == GTEXT_UTS46_IGNORED) {
      continue;
    }
    if (written + entry->length > cap) {
      return 0;
    }
    for (size_t i = 0; i < entry->length; i++) {
      out[written++] = gtext_uts46_pool[entry->offset + i];
    }
  }
  *out_len = written;
  return 1;
}

static int idna_hostname_valid_mapped(
    const char * text, size_t len, int allow_unicode);

/*
 * UTS #46 steps 1 and 2 - map, then normalise - and then IDNA2008.
 *
 * The two specifications are layered, not merged. UTS #46 says what a name
 * *becomes*; RFC 5892, derived in tools/idna/gen_tables.py from the UCD,
 * still says what is valid. That layering is what JSON Schema section 7.3.4.3
 * asks for, since it defines `idn-hostname` by RFC 5890 and not by UTS #46 -
 * and it is why the two data sets may be different Unicode versions without
 * the answer depending on which.
 *
 * Only for `idn-hostname`. A plain `hostname` is ASCII and mapping it would
 * be answering a question nobody asked: `EXAMPLE.COM` is already a host name,
 * and `ｅxample.com` is not one that a case fold should rescue.
 */
GTEXT_INTERNAL_API int gtext_idna_hostname_valid(
    const char * text, size_t len, int allow_unicode) {
  if (len == 0 || len > 4 * IDNA_MAX_NAME_OCTETS) {
    return 0;
  }
  if (!allow_unicode) {
    return idna_hostname_valid_mapped(text, len, allow_unicode);
  }

  uint32_t mapped[IDNA_MAX_MAPPED_CODEPOINTS];
  size_t mapped_len = 0;
  if (!idna_uts46_map(
          text, len, mapped, IDNA_MAX_MAPPED_CODEPOINTS, &mapped_len)) {
    return 0;
  }
  if (mapped_len == 0) {
    return 0; // every character was ignored, leaving no name at all
  }

  /*
   * Normalised as one string rather than label by label, which is what the
   * specification says and happens to be the same thing: FULL STOP is a
   * starter that composes with nothing, so no composition can cross it.
   */
  uint32_t normalized[IDNA_MAX_DECOMPOSED_CODEPOINTS];
  size_t normalized_len = 0;
  if (!gtext_nfc(mapped, mapped_len, normalized,
          IDNA_MAX_DECOMPOSED_CODEPOINTS, &normalized_len)) {
    return 0;
  }
  if (normalized_len == 0 || normalized_len > IDNA_MAX_MAPPED_CODEPOINTS) {
    return 0;
  }

  char utf8[IDNA_MAX_MAPPED_CODEPOINTS * 4];
  size_t utf8_len = 0;
  for (size_t i = 0; i < normalized_len; i++) {
    size_t wrote =
        idna_utf8_put(normalized[i], utf8 + utf8_len, sizeof(utf8) - utf8_len);
    if (wrote == 0) {
      return 0;
    }
    utf8_len += wrote;
  }
  return idna_hostname_valid_mapped(utf8, utf8_len, allow_unicode);
}

static int idna_hostname_valid_mapped(
    const char * text, size_t len, int allow_unicode) {
  if (len == 0 || len > 4 * IDNA_MAX_NAME_OCTETS) {
    return 0;
  }

  /* Split into labels first, because the bidi rule is a property of the whole
   * name: a label that would pass on its own fails once some other label has
   * made this a bidi domain name. */
  size_t starts[IDNA_MAX_NAME_OCTETS + 1];
  size_t lengths[IDNA_MAX_NAME_OCTETS + 1];
  size_t count = 0;
  size_t at = 0;
  size_t label_start = 0;
  while (at < len) {
    size_t before = at;
    uint32_t cp = idna_utf8_next(text, len, &at);
    if (cp == 0 && at == before) {
      return 0;
    }
    if (!idna_is_separator(cp)) {
      continue;
    }
    if (count >= IDNA_MAX_NAME_OCTETS) {
      return 0;
    }
    starts[count] = label_start;
    lengths[count] = before - label_start;
    count++;
    label_start = at;
  }
  if (count >= IDNA_MAX_NAME_OCTETS) {
    return 0;
  }
  starts[count] = label_start;
  lengths[count] = len - label_start;
  count++;

  /* Every label is decoded once, then checked; the two passes are what lets
   * the bidi question be answered before any label is judged. */
  uint32_t buffer[IDNA_MAX_LABEL_CODEPOINTS];
  size_t total_octets = 0;
  int bidi_domain = 0;

  for (size_t pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < count; i++) {
      const char * label = text + starts[i];
      size_t label_len = lengths[i];
      if (label_len == 0) {
        return 0; // an empty label, which covers a leading or trailing stop
      }
      size_t n = 0;
      int was_a_label = 0;
      if (!idna_label_codepoints(label, label_len, buffer, &n, &was_a_label)) {
        return 0;
      }
      int ascii_only = 1;
      for (size_t k = 0; k < n; k++) {
        if (buffer[k] >= 0x80) {
          ascii_only = 0;
          break;
        }
      }

      if (pass == 0) {
        if (!was_a_label && !ascii_only && !allow_unicode) {
          return 0; // a U-label where only ASCII is allowed
        }
        if (!ascii_only && idna_label_has_rtl(buffer, n)) {
          bidi_domain = 1;
        }
        continue;
      }

      if (was_a_label || !ascii_only) {
        if (!idna_u_label_valid(buffer, n, bidi_domain)) {
          return 0;
        }
        /* The 63-octet limit is on the A-label form, so a U-label is
         * measured by what it would encode to and not by its own length. */
        char encoded[IDNA_MAX_LABEL_OCTETS * 4];
        size_t encoded_len = 0;
        if (!puny_encode(buffer, n, encoded, sizeof(encoded), &encoded_len)) {
          return 0;
        }
        if (encoded_len + 4 > IDNA_MAX_LABEL_OCTETS) {
          return 0;
        }
        total_octets += encoded_len + 4;
      }
      else {
        if (!idna_ldh_label_valid(label, label_len)) {
          return 0;
        }
        if (bidi_domain && !idna_bidi_label_valid(buffer, n)) {
          return 0;
        }
        total_octets += label_len;
      }
      total_octets += 1; // the separator, or the root label's own octet
    }
  }
  return total_octets - 1 <= IDNA_MAX_NAME_OCTETS;
}
