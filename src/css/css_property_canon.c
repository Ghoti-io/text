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
 * The canonical form of a specified value: the rules that write a value the way the standard's tests and the browsers write it.
 *
 * Specified-value serialization (CSSOM, "serialize a CSS value") writes a value in the shortest form that parses back to the same value
 * ("the shortest form possible"): `display: block flow` is `block`, a position of one keyword has its second component written,
 * `repeat no-repeat` is `repeat-x`, a counter name without a value has the default written, and so on. Each property's specification
 * says which, and the pinned data does not (its `canonicalOrder` is only "per grammar"), so the rules are here, one function for each
 * property or family, and every one has the sentence of the draft or the test that gives it beside it.
 *
 * **The rules read the text the value serializer wrote and write another text** (the typed value is untouched: it is the value as it
 * was matched, and the canonical form is how it is written). Parsing the canonical text and writing it again gives the same text (every
 * rule is idempotent, held by tests/test-css-property-canon.cpp), and the typed value of the canonical text is the same value in the
 * sense of the standard (`block` and `block flow` are one `display`), not always the same tree: gtext_css_property_value_serialize()
 * is the place where the two are told apart, and a property with a rule is the one that tells the registry report not to demand that
 * the tree is the same (tools/css/css_registry_report.c).
 *
 * A rule that does not understand a text leaves it alone: a canonical form is only ever a rewrite of something the rule recognizes.
 * The tokens are split at the top level (outside parentheses, brackets, braces and strings); a list is split at its commas.
 */

#include "css_property_canon.h"
#include "css_internal.h"

#include <ghoti.io/text/css/css_serialize.h>

#include <stdlib.h>
#include <string.h>

/* ---- a growing output buffer ---------------------------------------------------------------------------------------- */

typedef struct {
  const GTEXT_Allocator * a;
  char * d;
  size_t n, cap;
  bool fail;
  bool * sticky; /**< Set as well when this buffer fails: the buffer of the caller whose result it is a part of. */
} cb;

static void cb_put(cb * b, const char * s, size_t n) {
  if (b->fail || !n) {
    return;
  }
  if (b->n + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->n + n + 1) {
      cap *= 2;
    }
    char * g = gtext_allocator_realloc(b->a, b->d, cap);
    if (!g) {
      b->fail = true;
      if (b->sticky) {
        *b->sticky = true;
      }
      return;
    }
    b->d = g;
    b->cap = cap;
  }
  memcpy(b->d + b->n, s, n);
  b->n += n;
  b->d[b->n] = '\0';
}

static void cb_puts(cb * b, const char * s) {
  cb_put(b, s, strlen(s));
}

static void cb_putc(cb * b, char c) {
  cb_put(b, &c, 1);
}

/* ---- spans of the text -------------------------------------------------------------------------------------------------- */

typedef struct {
  const char * p;
  size_t n;
} sp;

typedef struct {
  const GTEXT_Allocator * a;
  sp * v;
  size_t n, cap;
  bool fail;
  bool * sticky;
} spv;

static void spv_push(spv * s, sp x) {
  if (s->fail) {
    return;
  }
  if (s->n == s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 8;
    sp * g = gtext_allocator_realloc(s->a, s->v, cap * sizeof *g);
    if (!g) {
      s->fail = true;
      if (s->sticky) {
        *s->sticky = true;
      }
      return;
    }
    s->v = g;
    s->cap = cap;
  }
  s->v[s->n++] = x;
}

static bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/** Split `t` at the top level on white space (sep ' ') or on commas (sep ','; the white space around a comma goes). */
static void top_split(spv * out, const char * t, size_t n, char sep) {
  int depth = 0;
  char quote = 0;
  size_t start = 0;
  size_t escaped = (size_t)-1; /* the index of the last character an escape took in this piece: `foo\ ` ends in white space that is part of the name */
  bool open = false;
  for (size_t i = 0; i <= n; i++) {
    const char c = i < n ? t[i] : '\0';
    bool cut = false;
    if (i == n) {
      cut = true;
    }
    else if (quote) {
      if (c == '\\' && i + 1 < n) {
        i++;
      }
      else if (c == quote) {
        quote = 0;
      }
    }
    else if (c == '\\' && i + 1 < n && t[i + 1] != '\n' && t[i + 1] != '\r' && t[i + 1] != '\f') {
      /* an escape: the next character is part of the token, whatever it is (a space, a comma, a parenthesis). A hex escape is one to six hex digits
       * and one white space that ends it (`\1 ` is U+0001): that space is part of the escape and is never trimmed or split at */
      i++;
      escaped = i;
      open = true;
      if (is_hex(t[i])) {
        size_t digits = 1;
        while (digits < 6 && i + 1 < n && is_hex(t[i + 1])) {
          i++;
          digits++;
        }
        if (i + 1 < n && is_ws(t[i + 1])) {
          i++;
          escaped = i;
          if (t[i] == '\r' && i + 1 < n && t[i + 1] == '\n') {
            i++;
            escaped = i;
          }
        }
      }
    }
    else if (c == '"' || c == '\'') {
      quote = c;
      open = true;
    }
    else if (c == '(' || c == '[' || c == '{') {
      depth++;
      open = true;
    }
    else if (c == ')' || c == ']' || c == '}') {
      depth = depth > 0 ? depth - 1 : 0;
      open = true;
    }
    else if (depth == 0 && ((sep == ' ' && is_ws(c)) || (sep == ',' && c == ','))) {
      cut = true;
    }
    else {
      open = true;
    }
    if (cut) {
      size_t a = start, b = i;
      while (a < b && is_ws(t[a])) {
        a++;
      }
      while (b > a && is_ws(t[b - 1]) && b - 1 != escaped) {
        b--;
      }
      if (open && b > a) {
        spv_push(out, (sp){t + a, b - a});
      }
      start = i + 1;
      open = false;
      escaped = (size_t)-1;
    }
  }
}

static bool eq(sp a, const char * s) {
  const size_t n = strlen(s);
  return a.n == n && memcmp(a.p, s, n) == 0;
}

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool ieq(sp a, const char * s) {
  const size_t n = strlen(s);
  if (a.n != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (lower(a.p[i]) != s[i]) {
      return false;
    }
  }
  return true;
}

static bool sp_eq(sp a, sp b) {
  return a.n == b.n && memcmp(a.p, b.p, a.n) == 0;
}

static void put_sp(cb * b, sp x) {
  cb_put(b, x.p, x.n);
}

/** `x` joined by single spaces, from `from` to the end. */
static void put_join(cb * b, const sp * x, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (i) {
      cb_putc(b, ' ');
    }
    put_sp(b, x[i]);
  }
}

/* ---- numbers, as decimal text ------------------------------------------------------------------------------------------ */

/** A plain decimal number `[+-]?digits[.digits]` (no exponent), parsed into its parts; false for anything else. */
typedef struct {
  bool neg;
  sp ip, fp;    /**< integer and fraction digits */
} dec;

static bool dec_parse(sp t, dec * d) {
  size_t i = 0;
  d->neg = false;
  if (i < t.n && (t.p[i] == '+' || t.p[i] == '-')) {
    d->neg = t.p[i] == '-';
    i++;
  }
  const size_t ib = i;
  while (i < t.n && t.p[i] >= '0' && t.p[i] <= '9') {
    i++;
  }
  d->ip = (sp){t.p + ib, i - ib};
  d->fp = (sp){t.p + i, 0};
  if (i < t.n && t.p[i] == '.') {
    i++;
    const size_t fb = i;
    while (i < t.n && t.p[i] >= '0' && t.p[i] <= '9') {
      i++;
    }
    d->fp = (sp){t.p + fb, i - fb};
    if (d->fp.n == 0) {
      return false;
    }
  }
  return i == t.n && (d->ip.n > 0 || d->fp.n > 0);
}

static bool dec_zero(const dec * d) {
  for (size_t i = 0; i < d->ip.n; i++) {
    if (d->ip.p[i] != '0') {
      return false;
    }
  }
  for (size_t i = 0; i < d->fp.n; i++) {
    if (d->fp.p[i] != '0') {
      return false;
    }
  }
  return true;
}

/** Whether the number is more than the whole number `limit` (1 or 100 here), compared in decimal. */
static bool dec_exceeds(const dec * d, unsigned limit) {
  char lim[16];
  size_t ln = 0;
  for (unsigned v = limit; v; v /= 10) {
    lim[ln++] = (char)('0' + v % 10);
  }
  for (size_t i = 0; i < ln / 2; i++) {
    const char c = lim[i];
    lim[i] = lim[ln - 1 - i];
    lim[ln - 1 - i] = c;
  }
  size_t lead = 0;
  while (lead < d->ip.n && d->ip.p[lead] == '0') {
    lead++;
  }
  const size_t il = d->ip.n - lead;
  if (il != ln) {
    return il > ln;
  }
  const int c = memcmp(d->ip.p + lead, lim, ln);
  if (c != 0) {
    return c > 0;
  }
  for (size_t i = 0; i < d->fp.n; i++) {
    if (d->fp.p[i] != '0') {
      return true;
    }
  }
  return false;
}

/** A number in the CSSOM spelling: no plus, no trailing zeros, a leading 0 before a point kept (`0.5`), `-0` as `0`. `shift` moves the point left;
 * `max_dec` (when above 0) rounds, half away from zero, to that many decimals, the way the serializer writes a number, so that the text is
 * what the serializer writes back. False, with nothing written, when the digits are more than the fixed buffer holds: the caller then
 * leaves the value as it is rather than lose a sign or a fraction. */
static bool dec_put(cb * b, const dec * d, int shift, int max_dec) {
  char digits[512];
  size_t nd = 0, point;
  if (d->ip.n + d->fp.n + (size_t)shift + 4 > sizeof digits) {
    return false;
  }
  for (int i = 0; i < shift; i++) {
    digits[nd++] = '0';
  }
  /* digits: shift zeros, then the integer digits, then the fraction; the point is after shift + ip digits, moved left by shift */
  memcpy(digits + nd, d->ip.p, d->ip.n);
  nd += d->ip.n;
  point = nd - (size_t)shift;
  memcpy(digits + nd, d->fp.p, d->fp.n);
  nd += d->fp.n;
  if (max_dec > 0 && nd - point > (size_t)max_dec) {
    const bool up = digits[point + (size_t)max_dec] >= '5';
    nd = point + (size_t)max_dec;
    if (up) {
      size_t i = nd;
      while (i > 0 && digits[i - 1] == '9') {
        digits[--i] = '0';
      }
      if (i > 0) {
        digits[i - 1]++;
      }
      else {
        memmove(digits + 1, digits, nd);
        digits[0] = '1';
        nd++;
        point++;
      }
    }
  }
  /* the point sits after `point` digits; strip leading zeros of the integer part and trailing zeros of the fraction */
  size_t lead = 0;
  while (lead + 1 < point && digits[lead] == '0') {
    lead++;
  }
  size_t end = nd;
  while (end > point && digits[end - 1] == '0') {
    end--;
  }
  bool all_zero = true;
  for (size_t i = lead; i < end; i++) {
    if (digits[i] != '0') {
      all_zero = false;
    }
  }
  if (all_zero) {
    cb_putc(b, '0');
    return true;
  }
  if (d->neg) {
    cb_putc(b, '-');
  }
  if (point == lead) {
    cb_putc(b, '0');
  }
  cb_put(b, digits + lead, point - lead);
  if (end > point) {
    cb_putc(b, '.');
    cb_put(b, digits + point, end - point);
  }
  return true;
}

/** The canonical text of a plain number (for comparisons), written into `out` (`cap` bytes); false when `t` is not one or does not fit. The
 * scratch text is allocated with the allocator of `owner`, whose `fail` is set when that allocation fails, so the sweep reaches it. */
static bool num_norm(cb * owner, sp t, char * out, size_t cap) {
  dec d;
  if (!dec_parse(t, &d)) {
    return false;
  }
  cb tmp = {owner->a, NULL, 0, 0, false, &owner->fail};
  const bool wrote = dec_put(&tmp, &d, 0, 6);
  const bool ok = wrote && !tmp.fail && tmp.n < cap;
  if (ok) {
    memcpy(out, tmp.d, tmp.n + 1);
  }
  gtext_allocator_free(tmp.a, tmp.d);
  return ok;
}

static bool num_is(cb * owner, sp t, const char * canonical) {
  char buf[64];
  return num_norm(owner, t, buf, sizeof buf) && strcmp(buf, canonical) == 0;
}

static bool is_zero_dim(sp t);

/** A zero length in a unit that is a fixed multiple of the pixel (`0px`, `0mm`, `0pt`), or a bare zero: the initial length `0px` once it is read. A zero in a relative unit is not (the
 *  pinned browsers write `border-box 0em` as written and `border-box 0mm` as `border-box`). */
static bool is_zero_absolute_length(sp t) {
  if (!is_zero_dim(t)) {
    return false;
  }
  size_t i = 0;
  while (i < t.n && ((t.p[i] >= '0' && t.p[i] <= '9') || t.p[i] == '.' || t.p[i] == '+' || t.p[i] == '-')) {
    i++;
  }
  if (i == t.n) {
    return true;
  }
  const GTEXT_CSS_Unit u = gtext_css_unit_lookup(t.p + i, t.n - i);
  return u != GTEXT_CSS_UNIT_NONE && gtext_css_unit_is_absolute(u);
}

/** A zero length or number: `0`, `0px`, `0.0em` (a percentage is not). */
static bool is_zero_dim(sp t) {
  size_t i = 0;
  while (i < t.n && ((t.p[i] >= '0' && t.p[i] <= '9') || t.p[i] == '.' || t.p[i] == '+' || t.p[i] == '-')) {
    i++;
  }
  dec d;
  if (i == 0 || !dec_parse((sp){t.p, i}, &d) || !dec_zero(&d)) {
    return false;
  }
  if (i == t.n) {
    return true;
  }
  for (size_t k = i; k < t.n; k++) {
    if (!((t.p[k] >= 'a' && t.p[k] <= 'z') || (t.p[k] >= 'A' && t.p[k] <= 'Z'))) {
      return false;
    }
  }
  return true;
}

/** `50%` as `0.5`: the percentage divided by 100 in decimal text. False when `t` is not a plain percentage. */
static bool percent_to_number(cb * b, sp t) {
  if (t.n < 2 || t.p[t.n - 1] != '%') {
    return false;
  }
  dec d;
  if (!dec_parse((sp){t.p, t.n - 1}, &d)) {
    return false;
  }
  return dec_put(b, &d, 2, 6);
}

/* ---- walking the items of a list ----------------------------------------------------------------------------------------- */

/** An item rule: given the tokens of one comma item, write its canonical form and return true, or return false to leave it as it is. */
typedef bool (*item_rule)(cb * out, const sp * t, size_t n);

static bool run_items(cb * out, const char * text, size_t len, item_rule rule, char token_sep) {
  spv items = {out->a, NULL, 0, 0, false, &out->fail};
  top_split(&items, text, len, ',');
  if (items.fail) {
    gtext_allocator_free(out->a, items.v);
    out->fail = true;
    return false;
  }
  for (size_t i = 0; i < items.n; i++) {
    if (i) {
      cb_puts(out, ", ");
    }
    spv toks = {out->a, NULL, 0, 0, false, &out->fail};
    top_split(&toks, items.v[i].p, items.v[i].n, token_sep);
    if (toks.fail) {
      gtext_allocator_free(out->a, toks.v);
      gtext_allocator_free(out->a, items.v);
      out->fail = true;
      return false;
    }
    const size_t mark = out->n;
    if (!(toks.n && rule(out, toks.v, toks.n))) {
      out->n = mark;
      if (out->d) {
        out->d[out->n] = '\0';
      }
      put_sp(out, items.v[i]);
    }
    gtext_allocator_free(out->a, toks.v);
  }
  gtext_allocator_free(out->a, items.v);
  return !out->fail;
}

/* ---- the rules ---------------------------------------------------------------------------------------------------------- */

/* Box Alignment 3: `<baseline-position> = [ first | last ]? baseline`; `first baseline` is `baseline`. */
static bool rule_first_baseline(cb * out, const sp * t, size_t n) {
  if (n == 2 && eq(t[0], "first") && eq(t[1], "baseline")) {
    cb_puts(out, "baseline");
    return true;
  }
  return false;
}

/** The plain percentage `t` (`12.5%`) as a decimal, false for anything else (a `calc()`, a number, a dimension). */
static bool pct_dec(sp t, dec * d) {
  return t.n > 1 && t.p[t.n - 1] == '%' && dec_parse((sp){t.p, t.n - 1}, d);
}

/** The sign of `a - b` for two decimals: -1, 0 or 1, compared digit by digit (no floating point, so the answer is the same on every host). */
static int dec_cmp(const dec * a, const dec * b) {
  const bool az = dec_zero(a), bz = dec_zero(b);
  const bool an = a->neg && !az, bn = b->neg && !bz;
  if (an != bn) {
    return an ? -1 : 1;
  }
  size_t al = 0, bl = 0;
  while (al < a->ip.n && a->ip.p[al] == '0') {
    al++;
  }
  while (bl < b->ip.n && b->ip.p[bl] == '0') {
    bl++;
  }
  int c = 0;
  if (a->ip.n - al != b->ip.n - bl) {
    c = a->ip.n - al > b->ip.n - bl ? 1 : -1;
  }
  else if (a->ip.n - al && (c = memcmp(a->ip.p + al, b->ip.p + bl, a->ip.n - al)) != 0) {
    c = c > 0 ? 1 : -1;
  }
  else {
    const size_t fl = a->fp.n > b->fp.n ? a->fp.n : b->fp.n;
    for (size_t i = 0; i < fl && c == 0; i++) {
      const char ca = i < a->fp.n ? a->fp.p[i] : '0', cb2 = i < b->fp.n ? b->fp.p[i] : '0';
      c = ca == cb2 ? 0 : (ca > cb2 ? 1 : -1);
    }
  }
  return an ? -c : c;
}

/* Easing 2 (served 2026-11-01), 2.1.1: "control points originally specified with two input progress values are turned into two separate control points, and the input
 * progress values are in strictly ascending order" (`linear(0 20%, 0.5 10%, 1)` serializes as `linear(0 20%, 0.5 20%, 1)`, `linear(0, 0.25 25% 75%, 1)` as
 * `linear(0, 0.25 25%, 0.25 75%, 1)`); a control point that "originally lacked an input progress value" is written without one. A function with an argument that is not a
 * number and up to two plain percentages (a `calc()` that stayed) is left as it was written. */
static bool rule_linear_points(cb * out, sp f) {
  spv args = {out->a, NULL, 0, 0, false, &out->fail};
  top_split(&args, f.p + 7, f.n - 8, ',');
  bool done = false;
  dec max_pos = {false, {NULL, 0}, {NULL, 0}};
  sp max_sp = {NULL, 0};
  bool have_max = false;
  cb tmp = {out->a, NULL, 0, 0, false, &out->fail};
  bool ok = !args.fail && args.n > 0;
  cb_puts(&tmp, "linear(");
  bool first = true;
  for (size_t i = 0; ok && i < args.n; i++) {
    spv toks = {out->a, NULL, 0, 0, false, &out->fail};
    top_split(&toks, args.v[i].p, args.v[i].n, ' ');
    sp value = {NULL, 0};
    sp pos[2];
    size_t npos = 0;
    dec d;
    for (size_t k = 0; ok && k < toks.n; k++) {
      if (toks.v[k].p[toks.v[k].n - 1] == '%') {
        if (npos == 2 || !pct_dec(toks.v[k], &d)) {
          ok = false;
        }
        else {
          pos[npos++] = toks.v[k];
        }
      }
      else if (value.p || !dec_parse(toks.v[k], &d)) {
        ok = false;
      }
      else {
        value = toks.v[k];
      }
    }
    ok = ok && !toks.fail && value.p;
    for (size_t k = 0; ok && k < (npos ? npos : 1); k++) {
      sp at = npos ? pos[k] : (sp){NULL, 0};
      if (at.p) {
        dec cur;
        pct_dec(at, &cur);
        if (have_max && dec_cmp(&cur, &max_pos) < 0) {
          at = max_sp;
        }
        else {
          max_pos = cur;
          max_sp = at;
          have_max = true;
        }
      }
      if (!first) {
        cb_puts(&tmp, ", ");
      }
      first = false;
      put_sp(&tmp, value);
      if (at.p) {
        cb_putc(&tmp, ' ');
        put_sp(&tmp, at);
      }
    }
    gtext_allocator_free(out->a, toks.v);
  }
  if (ok && !tmp.fail) {
    cb_putc(&tmp, ')');
    cb_put(out, tmp.d, tmp.n);
    done = true;
  }
  gtext_allocator_free(out->a, tmp.d);
  if (args.fail) {
    out->fail = true;
  }
  gtext_allocator_free(out->a, args.v);
  return done;
}

/* Easing 2: `steps(<n>, jump-end)` and `steps(<n>, end)` are `steps(<n>)`. */
static bool rule_steps(cb * out, const sp * t, size_t n) {
  /* Easing 2 (served 2026-10-31), 2.3.1: "Unlike the other easing function keywords, step-start and step-end do not serialize as themselves. Instead, they serialize as
   * "steps(1, start)" and "steps(1)", respectively." */
  if (n == 1 && eq(t[0], "step-start")) {
    cb_puts(out, "steps(1, start)");
    return true;
  }
  if (n == 1 && eq(t[0], "step-end")) {
    cb_puts(out, "steps(1)");
    return true;
  }
  if (n == 1 && t[0].n > 8 && memcmp(t[0].p, "linear(", 7) == 0 && t[0].p[t[0].n - 1] == ')') {
    return rule_linear_points(out, t[0]);
  }
  if (n != 1 || t[0].n < 8 || memcmp(t[0].p, "steps(", 6) != 0 || t[0].p[t[0].n - 1] != ')') {
    return false;
  }
  spv args = {out->a, NULL, 0, 0, false, &out->fail};
  top_split(&args, t[0].p + 6, t[0].n - 7, ',');
  bool done = false;
  if (!args.fail && args.n == 2 && (eq(args.v[1], "end") || eq(args.v[1], "jump-end"))) {
    cb_puts(out, "steps(");
    put_sp(out, args.v[0]);
    cb_putc(out, ')');
    done = true;
  }
  if (args.fail) {
    out->fail = true;
  }
  gtext_allocator_free(out->a, args.v);
  return done;
}

/* Backgrounds 3 and Masking 1: `<bg-size> = [ <length-percentage> | auto ]{1,2} | cover | contain`; one length is `X auto` (the second
 * value is assumed to be auto) and `auto auto` is `auto`. */
static bool rule_bg_size(cb * out, const sp * t, size_t n) {
  if (n == 1 && !eq(t[0], "auto") && !eq(t[0], "cover") && !eq(t[0], "contain")) {
    put_sp(out, t[0]);
    cb_puts(out, " auto");
    return true;
  }
  if (n == 2 && eq(t[0], "auto") && eq(t[1], "auto")) {
    cb_puts(out, "auto");
    return true;
  }
  return false;
}

/* Two equal keywords are one: `scroll-snap-align: start start`, `position-area: center center`, `border-image-repeat: space space`. */
static bool rule_pair(cb * out, const sp * t, size_t n) {
  if (n == 2 && sp_eq(t[0], t[1])) {
    put_sp(out, t[0]);
    return true;
  }
  return false;
}

/* The radius of one corner: `<length-percentage [0,∞]>{1,2}` is the horizontal and the vertical radius ("border-top-left-radius: <border-radius>" with "<slash-separated-border-radius-syntax>
 * = <length-percentage [0,∞]> [ / <length-percentage [0,∞]> ]?"), so the slash form of one corner is the same two lengths with no slash, and two equal lengths are one:
 * css/css-borders/corner-shape/corner-top-left-valid.html asks `10px / 5px` of corner-top-left to be written `10px 5px` and `10px / 10px` `10px`. */
static bool rule_corner_radius(cb * out, const sp * t, size_t n) {
  if (n == 3 && eq(t[1], "/")) {
    put_sp(out, t[0]);
    if (!sp_eq(t[0], t[2])) {
      cb_putc(out, ' ');
      put_sp(out, t[2]);
    }
    return true;
  }
  return rule_pair(out, t, n);
}

/* Ruby 1 (served 2026-11-07), ruby-overhang: "for compatibility reasons, user agents must support an additional value of none for the 'ruby-overhang' property, as a legacy value alias of
 * spaces": a value alias is the same value, so it is written as the one it is an alias of (css/css-ruby/parsing/ruby-overhang-valid.html: `none` is `spaces`). */
static bool rule_ruby_overhang(cb * out, const sp * t, size_t n) {
  if (n == 1 && ieq(t[0], "none")) {
    cb_put(out, "spaces", 6);
    return true;
  }
  return false;
}

/* Anchor Positioning 1 (served 2026-11-01), position-area: "If only a single keyword is given, it behaves as if the second keyword is span-all if the given keyword is
 * unambigous about its axis ... (For example, top is equivalent to top span-all"; the shortest form leaves the span-all out beside a keyword that names its axis (top, left,
 * x-start, self-y-end, block-end, span-inline-start ...) and keeps it beside one that does not (start, end, center). Two equal keywords are one. */
static bool names_its_axis(sp t) {
  static const char * const pre[] = {"top", "bottom", "left", "right", "x-", "y-", "self-x-", "self-y-", "block-", "inline-", "span-top", "span-bottom", "span-left",
      "span-right", "span-x-", "span-y-", "span-self-x-", "span-self-y-", "span-block-", "span-inline-"};
  for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++) {
    const size_t n = strlen(pre[i]);
    if (t.n >= n && memcmp(t.p, pre[i], n) == 0 && (pre[i][n - 1] == '-' || t.n == n)) {
      return true;
    }
  }
  return false;
}

static bool rule_position_area(cb * out, const sp * t, size_t n) {
  if (n == 2 && eq(t[1], "span-all") && names_its_axis(t[0])) {
    put_sp(out, t[0]);
    return true;
  }
  if (n == 2 && eq(t[0], "span-all") && names_its_axis(t[1])) {
    put_sp(out, t[1]);
    return true;
  }
  return rule_pair(out, t, n);
}

/* Backgrounds 3 `<repeat-style> = repeat-x | repeat-y | [repeat | space | round | no-repeat]{1,2}`: the pair of equal keywords is one,
 * `repeat no-repeat` is `repeat-x` and `no-repeat repeat` is `repeat-y` ("the shorthand `repeat-x` is equivalent to `repeat no-repeat`"). */
static bool rule_repeat_style(cb * out, const sp * t, size_t n) {
  if (n != 2) {
    return false;
  }
  if (sp_eq(t[0], t[1])) {
    put_sp(out, t[0]);
    return true;
  }
  if (eq(t[0], "repeat") && eq(t[1], "no-repeat")) {
    cb_puts(out, "repeat-x");
    return true;
  }
  if (eq(t[0], "no-repeat") && eq(t[1], "repeat")) {
    cb_puts(out, "repeat-y");
    return true;
  }
  return false;
}

/* Values 4, <position>: "If only one value is specified, the second value is assumed to be center": the second component is written. */
static bool is_y_keyword(sp t) {
  return eq(t, "top") || eq(t, "bottom");
}

static bool is_position_keyword(sp t) {
  return eq(t, "left") || eq(t, "right") || eq(t, "top") || eq(t, "bottom") || eq(t, "center");
}

static bool rule_position(cb * out, const sp * t, size_t n) {
  if (n != 1) {
    return false;
  }
  /* a logical keyword (`inline-end`, `y-start`) names an axis the second component depends on: it is left as it is */
  if (!is_position_keyword(t[0]) && ((t[0].p[0] >= 'a' && t[0].p[0] <= 'z') || (t[0].p[0] >= 'A' && t[0].p[0] <= 'Z')) &&
      memchr(t[0].p, '(', t[0].n) == NULL) {
    return false;
  }
  if (is_y_keyword(t[0])) {
    cb_puts(out, "center ");
    put_sp(out, t[0]);
  }
  else if (eq(t[0], "center")) {
    cb_puts(out, "center center");
  }
  else {
    put_sp(out, t[0]);
    cb_puts(out, " center");
  }
  return true;
}

/* Color Adjust 1: `normal | [ light | dark | <custom-ident> ]+ && only?`: `only` is written last. */
static bool rule_color_scheme(cb * out, const sp * t, size_t n) {
  size_t only = n;
  for (size_t i = 0; i < n; i++) {
    if (eq(t[i], "only")) {
      only = i;
    }
  }
  if (only == n || only + 1 == n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (i != only) {
      put_sp(out, t[i]);
      cb_putc(out, ' ');
    }
  }
  cb_puts(out, "only");
  return true;
}

/* A counter list (Lists 3: "If the <integer> is omitted, it defaults to 0 [1 for counter-increment]"): the default is written after a
 * name that has none. A `reversed(name)` takes the default of the property too only when the text gives it a number. */
static bool is_integer_token(sp t) {
  size_t i = 0;
  if (i < t.n && (t.p[i] == '+' || t.p[i] == '-')) {
    i++;
  }
  if (i == t.n) {
    return false;
  }
  for (; i < t.n; i++) {
    if (t.p[i] < '0' || t.p[i] > '9') {
      return false;
    }
  }
  return true;
}

/** The value of a counter: an integer, or a math function (a `reversed()` is a name). */
static bool is_counter_value(sp t) {
  return is_integer_token(t) || (memchr(t.p, '(', t.n) != NULL && !(t.n > 9 && memcmp(t.p, "reversed(", 9) == 0));
}

static bool counter_defaults(cb * out, const sp * t, size_t n, const char * dflt) {
  if (n == 1 && eq(t[0], "none")) {
    return false;
  }
  bool changed = false;
  for (size_t i = 0; i < n; i++) {
    if (i) {
      cb_putc(out, ' ');
    }
    put_sp(out, t[i]);
    const bool is_function = memchr(t[i].p, '(', t[i].n) != NULL;
    if (!is_counter_value(t[i]) && !is_function && (i + 1 == n || !is_counter_value(t[i + 1]))) {
      cb_putc(out, ' ');
      cb_puts(out, dflt);
      changed = true;
    }
  }
  return changed;
}

static bool rule_counter_zero(cb * out, const sp * t, size_t n) {
  return counter_defaults(out, t, n, "0");
}

static bool rule_counter_one(cb * out, const sp * t, size_t n) {
  return counter_defaults(out, t, n, "1");
}

/* Filter Effects 1: "values of amount over 100% are allowed but are clamped" for grayscale(), invert(), opacity(), sepia(); an angle of
 * zero is written with its unit. */
static bool rule_filter(cb * out, const sp * t, size_t n) {
  bool changed = false;
  for (size_t i = 0; i < n; i++) {
    if (i) {
      cb_putc(out, ' ');
    }
    const sp f = t[i];
    const char * clamp[] = {"grayscale(", "invert(", "opacity(", "sepia("};
    bool done = false;
    for (size_t k = 0; k < 4 && !done; k++) {
      const size_t ln = strlen(clamp[k]);
      if (f.n > ln + 1 && memcmp(f.p, clamp[k], ln) == 0 && f.p[f.n - 1] == ')') {
        const sp arg = {f.p + ln, f.n - ln - 1};
        dec d;
        const bool pct = arg.n > 1 && arg.p[arg.n - 1] == '%';
        if (dec_parse(pct ? (sp){arg.p, arg.n - 1} : arg, &d) && !d.neg) {
          if (dec_exceeds(&d, pct ? 100 : 1)) {
            cb_put(out, f.p, ln);
            cb_puts(out, pct ? "100%" : "1");
            cb_putc(out, ')');
            changed = true;
            done = true;
          }
        }
      }
    }
    if (!done && f.n > 12 && memcmp(f.p, "hue-rotate(", 11) == 0 && f.p[f.n - 1] == ')') {
      const sp arg = {f.p + 11, f.n - 12};
      dec d;
      if (dec_parse(arg, &d) && dec_zero(&d)) {
        cb_puts(out, "hue-rotate(0deg)");
        changed = true;
        done = true;
      }
    }
    if (!done) {
      put_sp(out, f);
    }
  }
  return changed;
}

/** An identifier word as an unquoted family name spells it: a letter, `_` or a non-ASCII byte to start (or `-` and one of those), then those, digits
 * and `-`. */
static bool family_word(sp w) {
  size_t i = 0;
  if (i < w.n && w.p[i] == '-') {
    i++;
  }
  if (i >= w.n) {
    return false;
  }
  const unsigned char c0 = (unsigned char)w.p[i];
  if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_' || c0 >= 0x80)) {
    return false;
  }
  for (; i < w.n; i++) {
    const unsigned char c = (unsigned char)w.p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c >= 0x80)) {
      return false;
    }
  }
  return true;
}

/** Whether the bytes are well-formed UTF-8 (no overlong form, no surrogate, nothing above U+10FFFF) and every non-ASCII code point is one the tokenizer
 * reads inside an identifier (U+F104 is not: the tokenizer follows the list of CSS Syntax 3 and not "anything above U+0080"). A string holding other
 * bytes keeps its quotes: as an identifier those would not be read back as the same name. */
static bool utf8_well_formed(const char * s, size_t n) {
  size_t i = 0;
  while (i < n) {
    const unsigned char c = (unsigned char)s[i];
    size_t len;
    uint32_t cp;
    if (c < 0x80) {
      i++;
      continue;
    }
    else if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1F;
    }
    else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      cp = c & 0x0F;
    }
    else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07;
    }
    else {
      return false;
    }
    if (i + len > n) {
      return false;
    }
    for (size_t k = 1; k < len; k++) {
      const unsigned char cc = (unsigned char)s[i + k];
      if ((cc & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF) || !css_is_non_ascii_ident(cp)) {
      return false;
    }
    i += len;
  }
  return true;
}

static bool family_reserved(sp w) {
  static const char * const reserved[] = {"serif", "sans-serif", "cursive", "fantasy", "monospace", "system-ui", "ui-serif", "ui-sans-serif",
      "ui-monospace", "ui-rounded", "emoji", "math", "fangsong", "inherit", "initial", "unset", "revert", "revert-layer", "revert-rule", "default"};
  for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
    if (ieq(w, reserved[i])) {
      return true;
    }
  }
  return false;
}

/* Fonts 4: the generic family names are keywords and are written in lower case; a family name written as a string is written without the quotes
 * when it is a sequence of identifiers (`"New Century Schoolbook"` is `New Century Schoolbook`), except a single word that would then be a keyword
 * (`"serif"`, `"inherit"` stay strings). */
static bool rule_font_family(cb * out, const sp * t, size_t n) {
  static const char * const generic[] = {"serif", "sans-serif", "cursive", "fantasy", "monospace", "system-ui", "ui-serif",
      "ui-sans-serif", "ui-monospace", "ui-rounded", "emoji", "math", "fangsong"};
  if (n != 1) {
    return false;
  }
  if (t[0].n >= 2 && (t[0].p[0] == '"' || t[0].p[0] == '\'') && t[0].p[t[0].n - 1] == t[0].p[0]) {
    const char * s = t[0].p + 1;
    const size_t len = t[0].n - 2;
    if (len == 0 || memchr(s, '\\', len) || memchr(s, '"', len) || memchr(s, '\'', len) || !utf8_well_formed(s, len)) {
      return false;
    }
    size_t words = 0, i = 0;
    while (i < len) {
      size_t j = i;
      while (j < len && s[j] != ' ') {
        j++;
      }
      if (j == i || !family_word((sp){s + i, j - i})) {
        return false;
      }
      words++;
      i = j + 1;
      if (j == len - 1) {
        return false; /* a trailing space */
      }
    }
    if (words == 1 && family_reserved((sp){s, len})) {
      return false;
    }
    cb_put(out, s, len);
    return true;
  }
  for (size_t i = 0; i < sizeof generic / sizeof generic[0]; i++) {
    if (ieq(t[0], generic[i]) && !eq(t[0], generic[i])) {
      cb_puts(out, generic[i]);
      return true;
    }
  }
  return false;
}

/* Transforms 1, transform-origin: "If only one value is specified, the second value is assumed to be center", and the omitted z is 0px; the page
 * (and Firefox) write all three components: `left` is `left center 0px`, `top` is `center top 0px`. A lone `top` or `bottom` is the vertical one. */
static bool rule_transform_origin(cb * out, const sp * t, size_t n) {
  if (n == 1) {
    if (eq(t[0], "top") || eq(t[0], "bottom")) {
      cb_puts(out, "center ");
      put_sp(out, t[0]);
    }
    else {
      put_sp(out, t[0]);
      cb_puts(out, " center");
    }
    cb_puts(out, " 0px");
    return true;
  }
  if (n == 2) {
    put_join(out, t, 2);
    cb_puts(out, " 0px");
    return true;
  }
  return false;
}

/* Fonts 4, font-feature-settings: "<feature-tag-value> = <opentype-tag> [ <integer [0,∞]> | on | off ]?"; a value of 1 (or `on`) is not written
 * and `off` is 0. */
static bool rule_font_feature(cb * out, const sp * t, size_t n) {
  if (n != 2 || t[0].n < 2 || t[0].p[0] != '"') {
    return false;
  }
  if (eq(t[1], "on") || num_is(out, t[1], "1")) {
    put_sp(out, t[0]);
    return true;
  }
  if (eq(t[1], "off")) {
    put_sp(out, t[0]);
    cb_puts(out, " 0");
    return true;
  }
  return false;
}

/* Fonts 4, font-language-override: the trailing spaces of the tag are not part of the value. */
static bool rule_lang_override(cb * out, const sp * t, size_t n) {
  if (n != 1 || t[0].n < 2 || t[0].p[0] != '"' || t[0].p[t[0].n - 1] != '"') {
    return false;
  }
  size_t end = t[0].n - 1;
  while (end > 1 && t[0].p[end - 1] == ' ') {
    end--;
  }
  if (end == t[0].n - 1) {
    return false;
  }
  cb_put(out, t[0].p, end);
  cb_putc(out, '"');
  return true;
}

/* Fonts 5, font-size-adjust: "<font-metric>" is `ex-height` unless given, so it is not written. */
static bool rule_font_size_adjust(cb * out, const sp * t, size_t n) {
  if (n >= 2 && eq(t[0], "ex-height")) {
    put_join(out, t + 1, n - 1);
    return true;
  }
  return false;
}

/** The last `n` bytes of a token (all of it when it is shorter). */
static sp sp_suffix(sp t, size_t n) {
  if (t.n <= n) {
    return t;
  }
  return (sp){t.p + (t.n - n), n};
}

/* Fonts 4 (served 2026-11-01), font-style: the computed value is "the keyword specified, plus angle in degrees if specified", so the text does not make `oblique 0deg` and
 * `normal` one value; it is silent on the specified value, and the pinned browsers write `oblique 0deg` as `normal` both, and disagree on any other unit (Chromium writes
 * `oblique 0rad` and `oblique 0turn` as `normal`, Firefox as written): only the zero in degrees is `normal`, the rest is kept as written. */
static bool rule_font_style(cb * out, const sp * t, size_t n) {
  if (n == 2 && eq(t[0], "oblique") && is_zero_dim(t[1]) && (!(t[1].n && ((t[1].p[t[1].n - 1] | 0x20) >= 'a' && (t[1].p[t[1].n - 1] | 0x20) <= 'z')) || ieq(sp_suffix(t[1], 3), "deg"))) {
    cb_puts(out, "normal");
    return true;
  }
  return false;
}

/* Grid 2: `<'grid-auto-flow'> = [ row | column ] || dense`: `row dense` is `dense` (row is the default direction). */
static bool rule_grid_auto_flow(cb * out, const sp * t, size_t n) {
  if (n == 2 && eq(t[0], "row") && eq(t[1], "dense")) {
    cb_puts(out, "dense");
    return true;
  }
  return false;
}

/* Grid 2, <grid-line>: `span` comes first, in lower case; the integer of `span <integer> <ident>` is 1 by default and is not written. */
static bool rule_grid_line(cb * out, const sp * t, size_t n) {
  size_t span = n;
  for (size_t i = 0; i < n; i++) {
    if (ieq(t[i], "span")) {
      span = i;
    }
  }
  if (span == n) {
    return false;
  }
  size_t one = n;
  if (n == 3) {
    for (size_t i = 0; i < n; i++) {
      if (i != span && eq(t[i], "1")) {
        one = i;
      }
    }
  }
  if (one == n && span == 0 && eq(t[0], "span")) {
    return false;
  }
  cb_puts(out, "span");
  for (size_t i = 0; i < n; i++) {
    if (i != span && i != one) {
      cb_putc(out, ' ');
      put_sp(out, t[i]);
    }
  }
  return true;
}

/* Lists 3: a symbols() with the default type `symbolic` is written without it. */
static bool rule_symbols(cb * out, const sp * t, size_t n) {
  if (n != 1 || t[0].n < 18 || memcmp(t[0].p, "symbols(symbolic ", 17) != 0) {
    return false;
  }
  cb_puts(out, "symbols(");
  cb_put(out, t[0].p + 17, t[0].n - 17);
  return true;
}

/* Scroll Snap 1: `<'scroll-snap-type'> = none | [ x | y | block | inline | both ] [ mandatory | proximity ]?`: `proximity` is the default. */
static bool rule_scroll_snap_type(cb * out, const sp * t, size_t n) {
  if (n == 2 && eq(t[1], "proximity")) {
    put_sp(out, t[0]);
    return true;
  }
  return false;
}

/* Color Adjust, Compositing, Masking, Shapes: an <alpha-value> that was written as a percentage is a number. */
static bool rule_alpha_percent(cb * out, const sp * t, size_t n) {
  return n == 1 && percent_to_number(out, t[0]);
}

/* Transforms 2, translate: "a zero z and then a zero y are not written". */
static bool rule_translate(cb * out, const sp * t, size_t n) {
  size_t keep = n;
  if (keep == 3 && is_zero_dim(t[2])) {
    keep = 2;
  }
  if (keep == 2 && is_zero_dim(t[1])) {
    keep = 1;
  }
  if (keep == n) {
    return false;
  }
  put_join(out, t, keep);
  return true;
}

/* Transforms 2, rotate: `[ none | <angle> | [ x | y | z | <number>{3} ] && <angle> ]`; an axis along x or y is the keyword, an axis along z (or
 * none) is only the angle, and an axis that points the other way is the keyword with the angle negated. */
static bool negatable_angle(sp angle) {
  size_t i = 0;
  while (i < angle.n && ((angle.p[i] >= '0' && angle.p[i] <= '9') || angle.p[i] == '.' || angle.p[i] == '+' || angle.p[i] == '-')) {
    i++;
  }
  dec d;
  if (i == 0 || !dec_parse((sp){angle.p, i}, &d) || i == angle.n) {
    return false;
  }
  for (size_t k = i; k < angle.n; k++) {
    if (!((angle.p[k] >= 'a' && angle.p[k] <= 'z') || (angle.p[k] >= 'A' && angle.p[k] <= 'Z'))) {
      return false;
    }
  }
  return true;
}

/** The angle with its sign turned, when it is a plain number and unit (a calc() or a var() is not negated by writing a sign in front of it); a
 * zero stays `0deg`, never `-0deg`. Nothing is written, and false returned, for any other angle. */
static bool put_negated(cb * out, sp angle) {
  if (!negatable_angle(angle)) {
    return false;
  }
  size_t i = 0;
  while (angle.p[i] == '+' || angle.p[i] == '-' || angle.p[i] == '.' || (angle.p[i] >= '0' && angle.p[i] <= '9')) {
    i++;
  }
  dec d;
  if (!dec_parse((sp){angle.p, i}, &d)) {
    return false;
  }
  if (dec_zero(&d)) {
    cb_putc(out, '0');
  }
  else {
    const size_t skip = (angle.p[0] == '-' || angle.p[0] == '+') ? 1 : 0;
    if (!d.neg) {
      cb_putc(out, '-');
    }
    cb_put(out, angle.p + skip, i - skip);
  }
  cb_put(out, angle.p + i, angle.n - i);
  return true;
}

static int axis_sign(sp t) {
  dec d;
  if (!dec_parse(t, &d)) {
    return 2;
  }
  return dec_zero(&d) ? 0 : (d.neg ? -1 : 1);
}

static bool rule_rotate(cb * out, const sp * t, size_t n) {
  if (n == 2 && eq(t[0], "z")) {
    put_sp(out, t[1]);
    return true;
  }
  if (n == 2 && eq(t[1], "z")) {
    put_sp(out, t[0]);
    return true;
  }
  if (n == 2 && (eq(t[0], "x") || eq(t[0], "y"))) {
    return false;
  }
  if (n == 4) {
    const int sx = axis_sign(t[0]), sy = axis_sign(t[1]), sz = axis_sign(t[2]);
    if (sx == 2 || sy == 2 || sz == 2) {
      return false;
    }
    const int nz = (sx != 0) + (sy != 0) + (sz != 0);
    if (nz != 1) {
      return false;
    }
    const int sign = sx ? sx : (sy ? sy : sz);
    if (sign < 0 && !negatable_angle(t[3])) {
      return false;
    }
    if (sz) {
      if (sign > 0) {
        put_sp(out, t[3]);
      }
      else {
        put_negated(out, t[3]);
      }
      return true;
    }
    cb_puts(out, sx ? "x " : "y ");
    if (sign > 0) {
      put_sp(out, t[3]);
    }
    else {
      put_negated(out, t[3]);
    }
    return true;
  }
  return false;
}

/* Transforms 2, scale: "`scale: <number> | <percentage>` ... The number is the percentage divided by 100"; equal components are one, and a
 * z of 1 is not written. */
static bool rule_scale(cb * out, const sp * t, size_t n) {
  if (n < 1 || n > 3) {
    return false;
  }
  char comp[3][64];
  bool changed = false;
  for (size_t i = 0; i < n; i++) {
    cb tmp = {out->a, NULL, 0, 0, false, &out->fail};
    if (percent_to_number(&tmp, t[i])) {
      changed = true;
    }
    else if (num_norm(out, t[i], comp[i], sizeof comp[i])) {
      cb_puts(&tmp, comp[i]);
      if (tmp.n != t[i].n || memcmp(tmp.d, t[i].p, tmp.n) != 0) {
        changed = true;
      }
    }
    else {
      gtext_allocator_free(out->a, tmp.d);
      return false;
    }
    if (tmp.fail || tmp.n >= sizeof comp[i]) {
      gtext_allocator_free(out->a, tmp.d);
      return false;
    }
    memcpy(comp[i], tmp.d, tmp.n + 1);
    gtext_allocator_free(out->a, tmp.d);
  }
  size_t keep = n;
  if (keep == 3 && strcmp(comp[2], "1") == 0) {
    keep = 2;
    changed = true;
  }
  if (keep == 2 && strcmp(comp[0], comp[1]) == 0) {
    keep = 1;
    changed = true;
  }
  if (!changed) {
    return false;
  }
  for (size_t i = 0; i < keep; i++) {
    if (i) {
      cb_putc(out, ' ');
    }
    cb_puts(out, comp[i]);
  }
  return true;
}

/* Transforms 1 and 2: in a transform function an angle of zero is written with its unit, scale3d() takes numbers (a percentage is divided
 * by 100) and a skew() whose second angle is zero has one argument. */
static bool rule_transform(cb * out, const sp * t, size_t n) {
  bool changed = false;
  for (size_t i = 0; i < n; i++) {
    if (i) {
      cb_putc(out, ' ');
    }
    const sp f = t[i];
    const char * open = memchr(f.p, '(', f.n);
    if (!open || f.p[f.n - 1] != ')') {
      put_sp(out, f);
      continue;
    }
    const sp name = {f.p, (size_t)(open - f.p)};
    const sp inner = {open + 1, f.n - name.n - 2};
    spv args = {out->a, NULL, 0, 0, false, &out->fail};
    top_split(&args, inner.p, inner.n, ',');
    bool handled = false;
    if (!args.fail && args.n) {
      if (ieq(name, "scale3d") || ieq(name, "scale") || ieq(name, "scalex") || ieq(name, "scaley") || ieq(name, "scalez")) {
        cb tmp = {out->a, NULL, 0, 0, false, &out->fail};
        cb_put(&tmp, name.p, name.n);
        cb_putc(&tmp, '(');
        bool any = false;
        for (size_t k = 0; k < args.n; k++) {
          if (k) {
            cb_puts(&tmp, ", ");
          }
          if (percent_to_number(&tmp, args.v[k])) {
            any = true;
          }
          else {
            put_sp(&tmp, args.v[k]);
          }
        }
        cb_putc(&tmp, ')');
        if (any && !tmp.fail) {
          cb_put(out, tmp.d, tmp.n);
          handled = changed = true;
        }
        gtext_allocator_free(out->a, tmp.d);
      }
      else if (ieq(name, "rotate") || ieq(name, "rotatex") || ieq(name, "rotatey") || ieq(name, "rotatez") || ieq(name, "skewx") ||
          ieq(name, "skewy")) {
        if (args.n == 1 && is_zero_dim(args.v[0]) && args.v[0].n >= 1 && (args.v[0].p[args.v[0].n - 1] >= '0' && args.v[0].p[args.v[0].n - 1] <= '9')) {
          put_sp(out, name);
          cb_puts(out, "(0deg)");
          handled = changed = true;
        }
      }
      else if (ieq(name, "skew") && args.n <= 2) {
        const bool a_bare_zero = is_zero_dim(args.v[0]) && args.v[0].p[args.v[0].n - 1] >= '0' && args.v[0].p[args.v[0].n - 1] <= '9';
        const bool second_zero = args.n == 2 && is_zero_dim(args.v[1]);
        if (a_bare_zero || second_zero) {
          put_sp(out, name);
          cb_putc(out, '(');
          if (a_bare_zero) {
            cb_puts(out, "0deg");
          }
          else {
            put_sp(out, args.v[0]);
          }
          if (args.n == 2 && !second_zero) {
            cb_puts(out, ", ");
            put_sp(out, args.v[1]);
          }
          cb_putc(out, ')');
          handled = changed = true;
        }
      }
    }
    if (args.fail) {
      out->fail = true;
    }
    gtext_allocator_free(out->a, args.v);
    if (!handled) {
      put_sp(out, f);
    }
  }
  return changed;
}

/* Display 3, 2.8 "Precomposed Display Values", and the grammar `[ <display-outside> || <display-inside> ] | <display-listitem> | ...`: a
 * display in the full two-keyword form is written as the short keyword it is the equivalent of, and the default `flow` is not written. */
static bool rule_display(cb * out, const sp * t, size_t n) {
  if (n < 2 && !(n == 1 && eq(t[0], "flow"))) {
    return false;
  }
  const char * outside = NULL;
  const char * inside = NULL;
  bool item = false;
  for (size_t i = 0; i < n; i++) {
    if (eq(t[i], "block") || eq(t[i], "inline") || eq(t[i], "run-in")) {
      if (outside) {
        return false;
      }
      outside = eq(t[i], "block") ? "block" : eq(t[i], "inline") ? "inline" : "run-in";
    }
    else if (eq(t[i], "flow") || eq(t[i], "flow-root") || eq(t[i], "table") || eq(t[i], "flex") || eq(t[i], "grid") ||
        eq(t[i], "ruby")) {
      if (inside) {
        return false;
      }
      inside = eq(t[i], "flow") ? "flow" : eq(t[i], "flow-root") ? "flow-root" : eq(t[i], "table") ? "table" : eq(t[i], "flex") ? "flex"
          : eq(t[i], "grid") ? "grid" : "ruby";
    }
    else if (eq(t[i], "list-item")) {
      if (item) {
        return false;
      }
      item = true;
    }
    else {
      return false;
    }
  }
  if (!inside) {
    inside = "flow";
  }
  if (item) {
    if (strcmp(inside, "flow") != 0 && strcmp(inside, "flow-root") != 0) {
      return false;
    }
    bool first = true;
    if (outside && strcmp(outside, "block") != 0) {
      cb_puts(out, outside);
      first = false;
    }
    if (strcmp(inside, "flow-root") == 0) {
      cb_puts(out, first ? "flow-root" : " flow-root");
      first = false;
    }
    cb_puts(out, first ? "list-item" : " list-item");
    return true;
  }
  if (!outside) {
    outside = strcmp(inside, "ruby") == 0 ? "inline" : "block";
  }
  const bool block = strcmp(outside, "block") == 0, inl = strcmp(outside, "inline") == 0;
  if (strcmp(outside, "run-in") == 0) {
    cb_puts(out, "run-in");
    if (strcmp(inside, "flow") != 0) {
      cb_putc(out, ' ');
      cb_puts(out, inside);
    }
    return true;
  }
  if (block && strcmp(inside, "flow") == 0) {
    cb_puts(out, "block");
  }
  else if (inl && strcmp(inside, "flow") == 0) {
    cb_puts(out, "inline");
  }
  else if (block && strcmp(inside, "flow-root") == 0) {
    cb_puts(out, "flow-root");
  }
  else if (inl && strcmp(inside, "flow-root") == 0) {
    cb_puts(out, "inline-block");
  }
  else if (inl && strcmp(inside, "ruby") == 0) {
    cb_puts(out, "ruby");
  }
  else if (block && (strcmp(inside, "table") == 0 || strcmp(inside, "flex") == 0 || strcmp(inside, "grid") == 0)) {
    cb_puts(out, inside);
  }
  else if (inl && (strcmp(inside, "table") == 0 || strcmp(inside, "flex") == 0 || strcmp(inside, "grid") == 0)) {
    cb_puts(out, "inline-");
    cb_puts(out, inside);
  }
  else {
    cb_puts(out, outside);
    cb_putc(out, ' ');
    cb_puts(out, inside);
  }
  return true;
}

/* Inline 3, <text-edge>: "If only one value is specified, both edges are assigned that same keyword if possible": two equal edges are written once
 * (`cap text` is not shortened: the grammar of <text-edge> has no one-value form for `cap`). Both pinned browsers write `text text` as `text`. text-box is a
 * shorthand of text-box-trim and text-box-edge (css_shorthand.c), so the rule belongs to the longhand. */
static bool rule_text_edge(cb * out, const sp * t, size_t n) {
  if (n == 2 && sp_eq(t[0], t[1])) {
    put_join(out, t, 1);
    return true;
  }
  return false;
}

/* Box 4, margin-trim: `none | block | [ block-start || block-end ]` in the pinned data: `block-start block-end` is `block`. The draft Box 4
 * later adds `inline`, `inline-start` and `inline-end`; a value with them does not match the pinned grammar, so there is no rule for them yet. */
static bool rule_margin_trim(cb * out, const sp * t, size_t n) {
  /* a `||` is written in the grammar's order before the rules see it, so `block-end block-start` arrives as `block-start block-end` */
  if (n == 2 && eq(t[0], "block-start") && eq(t[1], "block-end")) {
    cb_puts(out, "block");
    return true;
  }
  return false;
}

/* SVG 2, paint-order: `normal | [ fill || stroke || markers ]`; "If any of the three keywords are omitted, they are painted last, in the order they would be painted
 * with paint-order: normal", so `stroke` is `stroke fill markers`, and the shortest form of a list is the shortest prefix that completes to the same order (the
 * order `fill stroke markers` is `fill`, which is not `normal`: the browsers keep the two apart). The grammar keeps the order the keywords are written in (the order is the value). */
static bool rule_paint_order(cb * out, const sp * t, size_t n) {
  static const char * const standard[3] = {"fill", "stroke", "markers"};
  if (n < 1 || n > 3) {
    return false;
  }
  int given[3];
  bool seen[3] = {false, false, false};
  for (size_t i = 0; i < n; i++) {
    int k = -1;
    for (int j = 0; j < 3; j++) {
      if (eq(t[i], standard[j])) {
        k = j;
      }
    }
    if (k < 0 || seen[k]) {
      return false;
    }
    seen[k] = true;
    given[i] = k;
  }
  int full[3];
  size_t m = 0;
  for (size_t i = 0; i < n; i++) {
    full[m++] = given[i];
  }
  for (int j = 0; j < 3; j++) {
    if (!seen[j]) {
      full[m++] = j;
    }
  }
  for (size_t len = 1; len <= 3; len++) {
    bool used[3] = {false, false, false};
    int completed[3];
    size_t c = 0;
    for (size_t i = 0; i < len; i++) {
      completed[c++] = full[i];
      used[full[i]] = true;
    }
    for (int j = 0; j < 3; j++) {
      if (!used[j]) {
        completed[c++] = j;
      }
    }
    if (completed[0] == full[0] && completed[1] == full[1] && completed[2] == full[2]) {
      for (size_t i = 0; i < len; i++) {
        if (i) {
          cb_putc(out, ' ');
        }
        cb_puts(out, standard[full[i]]);
      }
      return true;
    }
  }
  return false;
}

/* Flexbox 2, flex-wrap: `<'flex-wrap'> = nowrap | [ wrap | wrap-reverse ] || balance`: `wrap` is implied by `balance`. */
static bool rule_flex_wrap(cb * out, const sp * t, size_t n) {
  if (n == 2 && ((eq(t[0], "wrap") && eq(t[1], "balance")) || (eq(t[0], "balance") && eq(t[1], "wrap")))) {
    cb_puts(out, "balance");
    return true;
  }
  return false;
}

/* Images 4, object-fit: `contain scale-down` is `scale-down`. */
static bool rule_object_fit(cb * out, const sp * t, size_t n) {
  if (n == 2 && eq(t[0], "contain") && eq(t[1], "scale-down")) {
    cb_puts(out, "scale-down");
    return true;
  }
  return false;
}


/* Animations 1: a keyframes name that was written as a string is written as the identifier it is (CSSOM "serialize an identifier"),
 * unless the identifier would be a keyword. */
static bool decode_css_string(sp t, cb * out) {
  if (t.n < 2 || (t.p[0] != '"' && t.p[0] != '\'') || t.p[t.n - 1] != t.p[0]) {
    return false;
  }
  for (size_t i = 1; i + 1 < t.n; i++) {
    const char c = t.p[i];
    if (c != '\\') {
      cb_putc(out, c);
      continue;
    }
    i++;
    if (i + 1 >= t.n + 0 && i >= t.n - 1) {
      return false;
    }
    const char e = t.p[i];
    if ((e >= '0' && e <= '9') || (e >= 'a' && e <= 'f') || (e >= 'A' && e <= 'F')) {
      unsigned v = 0;
      size_t k = 0;
      while (k < 6 && i + 1 < t.n && ((t.p[i] >= '0' && t.p[i] <= '9') || (t.p[i] >= 'a' && t.p[i] <= 'f') || (t.p[i] >= 'A' && t.p[i] <= 'F'))) {
        const char h = t.p[i];
        v = v * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
        i++;
        k++;
      }
      if (i + 1 < t.n && is_ws(t.p[i])) {
        i++;
      }
      i--;
      if (v == 0 || v >= 0x80 || (v >= 0xD800 && v <= 0xDFFF)) {
        return false; /* only ASCII escapes are decoded here */
      }
      cb_putc(out, (char)v);
    }
    else {
      cb_putc(out, e);
    }
  }
  return true;
}

static GTEXT_Markup_Status cb_sink_write(void * user, const char * bytes, size_t len) {
  cb_put((cb *)user, bytes, len);
  return ((cb *)user)->fail ? GTEXT_MARKUP_E_WRITE : GTEXT_MARKUP_OK;
}

static bool rule_animation_name(cb * out, const sp * t, size_t n) {
  if (n != 1 || t[0].n < 3) {
    return false;
  }
  cb decoded = {out->a, NULL, 0, 0, false, &out->fail};
  const bool ok = decode_css_string(t[0], &decoded);
  bool done = false;
  if (ok && !decoded.fail && decoded.n > 0) {
    const sp name = {decoded.d, decoded.n};
    static const char * const reserved[] = {"none", "initial", "inherit", "unset", "revert", "revert-layer", "default"};
    bool keyword = false;
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
      keyword = keyword || ieq(name, reserved[i]);
    }
    if (!keyword) {
      const GTEXT_Markup_Sink sink = {cb_sink_write, out};
      done = gtext_css_serialize_identifier(decoded.d, decoded.n, &sink) == GTEXT_CSS_OK;
    }
  }
  gtext_allocator_free(out->a, decoded.d);
  return done;
}

/* Grid 2: "The line names in a track list are optional: an empty `[]` is the same as none", and it is not written. */
static bool rule_grid_template_tracks(cb * out, const char * text, size_t len) {
  if (len >= 7 && memcmp(text, "subgrid", 7) == 0) {
    return false; /* in a subgrid's list an empty name is an entry of the list and stays */
  }
  bool changed = false;
  char quote = 0;
  for (size_t i = 0; i < len; i++) {
    const char c = text[i];
    if (quote) {
      cb_putc(out, c);
      if (c == '\\' && i + 1 < len) {
        cb_putc(out, text[++i]);
      }
      else if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      cb_putc(out, c);
      continue;
    }
    if (c == '[' && i + 1 < len && text[i + 1] == ']') {
      const bool before = out->n == 0 || out->d[out->n - 1] == ' ' || out->d[out->n - 1] == '(';
      const bool after = i + 2 == len || text[i + 2] == ' ' || text[i + 2] == ')' || text[i + 2] == ',';
      if (before && after) {
        changed = true;
        i += 1;
        if (i + 1 < len && text[i + 1] == ' ') {
          i++;
        }
        else if (out->n > 0 && out->d[out->n - 1] == ' ') {
          out->n--;
          out->d[out->n] = '\0';
        }
        continue;
      }
    }
    cb_putc(out, c);
  }
  return changed;
}

/* UI 4, cursor: "<url-set> is a limited version of image-set(), where the <image> sub-production is restricted to <url> only": a string option
 * of the set is written as the url() it stands for. */
static bool rule_cursor(cb * out, const sp * t, size_t n) {
  bool changed = false;
  for (size_t i = 0; i < n; i++) {
    if (i) {
      cb_putc(out, ' ');
    }
    const sp f = t[i];
    if (f.n > 11 && memcmp(f.p, "image-set(", 10) == 0 && f.p[f.n - 1] == ')') {
      spv opts = {out->a, NULL, 0, 0, false, &out->fail};
      top_split(&opts, f.p + 10, f.n - 11, ',');
      cb_puts(out, "image-set(");
      for (size_t k = 0; k < opts.n; k++) {
        if (k) {
          cb_puts(out, ", ");
        }
        /* "Each <string> inside image-set() represents a <url>", and an option with no resolution "behaves as 1x" (Images 4, 2.4; UI 4, cursor): both pinned browsers
           write `image-set("a")` here as `image-set(url("a") 1x)` */
        spv tk = {out->a, NULL, 0, 0, false, &out->fail};
        top_split(&tk, opts.v[k].p, opts.v[k].n, ' ');
        if (tk.n >= 1 && !tk.fail) {
          bool has_resolution = false;
          for (size_t j = 1; j < tk.n; j++) {
            const char c0 = tk.v[j].n ? tk.v[j].p[0] : '\0';
            has_resolution = has_resolution || (c0 >= '0' && c0 <= '9') || c0 == '.' || c0 == '+' || c0 == '-';
          }
          const bool is_string = tk.v[0].n >= 2 && (tk.v[0].p[0] == '"' || tk.v[0].p[0] == '\'');
          if (is_string || !has_resolution) {
            if (is_string) {
              cb_puts(out, "url(");
              put_sp(out, tk.v[0]);
              cb_putc(out, ')');
            }
            else {
              put_sp(out, tk.v[0]);
            }
            if (!has_resolution) {
              cb_puts(out, " 1x");
            }
            for (size_t j = 1; j < tk.n; j++) {
              cb_putc(out, ' ');
              put_sp(out, tk.v[j]);
            }
            gtext_allocator_free(out->a, tk.v);
            changed = true;
            continue;
          }
        }
        gtext_allocator_free(out->a, tk.v);
        put_sp(out, opts.v[k]);
      }
      cb_putc(out, ')');
      if (opts.fail) {
        out->fail = true;
      }
      gtext_allocator_free(out->a, opts.v);
    }
    else {
      put_sp(out, f);
    }
  }
  return changed;
}

/* Shapes 1: a basic shape is written without the parts that are what is meant when they are absent. */
static bool is_zero_radius(sp t) {
  return is_zero_dim(t);
}

/** The path data of a path() normalized (Shapes 1: "the path data is serialized as ... with a space between"): each command letter, `z` as `Z`,
 * and its numbers, one space apart, no trailing zeros. False when the text is not path data this reads. */
static bool normalize_path_data(cb * out, sp str) {
  if (str.n < 2 || str.p[0] != '"' || str.p[str.n - 1] != '"' || memchr(str.p, '\\', str.n)) {
    return false;
  }
  cb tmp = {out->a, NULL, 0, 0, false, &out->fail};
  const char * p = str.p + 1;
  const char * end = str.p + str.n - 1;
  bool first = true;
  char cmd = 0;
  unsigned argi = 0;
  cb_putc(&tmp, '"');
  while (p < end) {
    const char c = *p;
    if (is_ws(c) || c == ',') {
      p++;
      continue;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      if (!strchr("MmLlHhVvCcSsQqTtAaZz", c)) {
        gtext_allocator_free(out->a, tmp.d);
        return false;
      }
      if (!first) {
        cb_putc(&tmp, ' ');
      }
      cb_putc(&tmp, c == 'z' ? 'Z' : c);
      first = false;
      cmd = c;
      argi = 0;
      p++;
      continue;
    }
    if ((cmd == 'a' || cmd == 'A') && (argi % 7 == 3 || argi % 7 == 4)) {
      /* an arc's two flags are one digit each, `0` or `1`, and need no separator after them: `a1 1 0 00.5.5` is the flags 0 and 0, then .5 and .5 */
      if (c != '0' && c != '1') {
        gtext_allocator_free(out->a, tmp.d);
        return false;
      }
      cb_putc(&tmp, ' ');
      cb_putc(&tmp, c);
      argi++;
      p++;
      continue;
    }
    argi++;
    const char * q = p;
    if (*q == '+' || *q == '-') {
      q++;
    }
    const char * digits = q;
    while (q < end && *q >= '0' && *q <= '9') {
      q++;
    }
    if (q < end && *q == '.') {
      q++;
      while (q < end && *q >= '0' && *q <= '9') {
        q++;
      }
    }
    if (q == digits || (q == digits + 1 && *digits == '.')) {
      gtext_allocator_free(out->a, tmp.d);
      return false;
    }
    /* an exponent is part of the number (`1e1` is 10): both pinned browsers write path("M1e1 1e1 L2e1 2e1") as "M 10 10 L 20 20"
       (tools/conformance/css_browser_answers.jsonl, cases path-clip.*). The decimal point is moved in the digits, in text, so no host function is used. */
    char shifted[96];
    dec d;
    if (q < end && (*q == 'e' || *q == 'E')) {
      const char * r = q + 1;
      bool eneg = false;
      if (r < end && (*r == '+' || *r == '-')) {
        eneg = *r == '-';
        r++;
      }
      if (r < end && *r >= '0' && *r <= '9') {
        long ex = 0;
        while (r < end && *r >= '0' && *r <= '9') {
          if (ex < 1000) {
            ex = ex * 10 + (*r - '0');
          }
          r++;
        }
        if (eneg) {
          ex = -ex;
        }
        /* the mantissa's digits without the point, and where the point goes */
        char dg[48];
        size_t nd = 0;
        long ip_len = 0;
        const char * m = digits;
        bool seen_point = false;
        for (; m < q; m++) {
          if (*m == '.') {
            seen_point = true;
          }
          else {
            if (nd >= sizeof dg) {
              gtext_allocator_free(out->a, tmp.d);
              return false;
            }
            dg[nd++] = *m;
            if (!seen_point) {
              ip_len++;
            }
          }
        }
        const long point = ip_len + ex;
        size_t w = 0;
        if (p < digits && *p == '-') {
          shifted[w++] = '-';
        }
        if (point > 40 || point < -40) {
          gtext_allocator_free(out->a, tmp.d);
          return false;
        }
        if (point <= 0) {
          shifted[w++] = '0';
          shifted[w++] = '.';
          for (long z = point; z < 0; z++) {
            shifted[w++] = '0';
          }
          for (size_t z = 0; z < nd; z++) {
            shifted[w++] = dg[z];
          }
        }
        else {
          for (long z = 0; z < point; z++) {
            shifted[w++] = (size_t)z < nd ? dg[z] : '0';
          }
          if ((size_t)point < nd) {
            shifted[w++] = '.';
            for (size_t z = (size_t)point; z < nd; z++) {
              shifted[w++] = dg[z];
            }
          }
        }
        q = r;
        if (!dec_parse((sp){shifted, w}, &d)) {
          gtext_allocator_free(out->a, tmp.d);
          return false;
        }
      }
      else if (!dec_parse((sp){p, (size_t)(q - p)}, &d)) {
        gtext_allocator_free(out->a, tmp.d);
        return false;
      }
    }
    else if (!dec_parse((sp){p, (size_t)(q - p)}, &d)) {
      gtext_allocator_free(out->a, tmp.d);
      return false;
    }
    cb_putc(&tmp, ' ');
    if (!dec_put(&tmp, &d, 0, 0)) {
      gtext_allocator_free(out->a, tmp.d);
      return false;
    }
    p = q;
  }
  cb_putc(&tmp, '"');
  if (tmp.fail) {
    out->fail = true;
    gtext_allocator_free(out->a, tmp.d);
    return false;
  }
  /* a number follows its command letter after a space: `M 20 20`; the first space after a letter is wanted, the rest are numbers' */
  cb_put(out, tmp.d, tmp.n);
  gtext_allocator_free(out->a, tmp.d);
  return true;
}

static bool is_coordinate_arg(sp a) {
  return !(a.n >= 7 && memcmp(a.p, "nonzero", 7) == 0) && !(a.n >= 7 && memcmp(a.p, "evenodd", 7) == 0) && !(a.n >= 5 && memcmp(a.p, "round", 5) == 0);
}

/* Shapes 1 (served 2026-10-31), shape(): the commands' specified value is written as both pinned browsers write it (the served text calls specified value serialization of
 * the shape functions "relatively trivial" and omits defaults only in the computed value): a position that is one keyword is two (`to center` is `to center center`,
 * `to left` is `to left center`, `from center` is `from center center`); "If such component is not provided, the <coordinate-pair> is relative to the segment's start",
 * so `from origin` is not written after the control points of a `to` command and `from start` is not after those of a `by` command (a `to` control point without it is a
 * <position>, which is relative to the reference box); and "rotate <angle>? ... If omitted, this defaults to 0deg", so `rotate 0deg` is not written, and neither is `small`, the size of an arc that is not said to be large. */
static bool shape_one_keyword_position(const sp w, const char ** two) {
  if (eq(w, "center")) { *two = "center center"; return true; }
  if (eq(w, "left")) { *two = "left center"; return true; }
  if (eq(w, "right")) { *two = "right center"; return true; }
  if (eq(w, "top")) { *two = "center top"; return true; }
  if (eq(w, "bottom")) { *two = "center bottom"; return true; }
  return false;
}

static bool shape_stop_word(const sp w) {
  return eq(w, "with") || eq(w, "of") || eq(w, "cw") || eq(w, "ccw") || eq(w, "large") || eq(w, "small") || eq(w, "rotate") || eq(w, "/");
}

/* One argument of shape() (`from ...` or a command), written to `out`; whether anything was changed. */
static bool shape_command(cb * out, sp arg) {
  spv tk = {out->a, NULL, 0, 0, false, &out->fail};
  top_split(&tk, arg.p, arg.n, ' ');
  if (tk.fail || tk.n == 0) {
    gtext_allocator_free(out->a, tk.v);
    out->fail = out->fail || tk.fail;
    return false;
  }
  bool by = false, to = false, changed = false, in_controls = false;
  /* hline and vline take one coordinate, which `left`, `center` and `right` (or `top`, ...) name: it is not a <position> */
  const bool axis_line = eq(tk.v[0], "hline") || eq(tk.v[0], "vline");
  cb o = {out->a, NULL, 0, 0, false, &out->fail};
  for (size_t i = 0; i < tk.n; i++) {
    const sp w = tk.v[i];
    if (eq(w, "by")) {
      by = true;
    }
    else if (eq(w, "to")) {
      to = true;
    }
    if (eq(w, "with")) {
      in_controls = true;
    }
    else if (eq(w, "of") || eq(w, "cw") || eq(w, "ccw") || eq(w, "large") || eq(w, "small") || eq(w, "rotate")) {
      in_controls = false;
    }
    /* `from origin` / `from start` after a control point */
    if (in_controls && eq(w, "from") && i + 1 < tk.n && ((to && eq(tk.v[i + 1], "origin")) || (by && eq(tk.v[i + 1], "start")))) {
      i++;
      changed = true;
      continue;
    }
    /* `rotate 0deg`, and `small`, the default size of an arc ("If omitted, this defaults to small") */
    if (eq(w, "rotate") && i + 1 < tk.n && eq(tk.v[i + 1], "0deg")) {
      i++;
      changed = true;
      continue;
    }
    if (eq(tk.v[0], "arc") && eq(w, "small")) {
      changed = true;
      continue;
    }
    if (o.n) {
      cb_putc(&o, ' ');
    }
    put_sp(&o, w);
    /* a position that is one keyword after `from` (the first argument) or `to` */
    if (!axis_line && (eq(w, "from") || eq(w, "to")) && i + 1 < tk.n && !shape_stop_word(tk.v[i + 1]) && (i + 2 >= tk.n || shape_stop_word(tk.v[i + 2]))) {
      const char * two = NULL;
      if (shape_one_keyword_position(tk.v[i + 1], &two)) {
        cb_putc(&o, ' ');
        cb_puts(&o, two);
        i++;
        changed = true;
      }
    }
  }
  if (changed && !o.fail) {
    cb_put(out, o.d, o.n);
  }
  else {
    changed = false;
  }
  gtext_allocator_free(out->a, o.d);
  gtext_allocator_free(out->a, tk.v);
  return changed;
}

static bool gradient_function(cb * out, sp f);

static bool image_function(cb * out, sp f, unsigned depth);

static bool shape_function(cb * out, sp f) {
  if (image_function(out, f, 0)) {
    return true; /* shape-outside takes an <image> as well */
  }
  const char * open = memchr(f.p, '(', f.n);
  if (!open || f.p[f.n - 1] != ')') {
    return false;
  }
  const sp name = {f.p, (size_t)(open - f.p)};
  const sp inner = {open + 1, f.n - name.n - 2};
  bool done = false;
  if (ieq(name, "circle") || ieq(name, "ellipse")) {
    spv tk = {out->a, NULL, 0, 0, false, &out->fail};
    top_split(&tk, inner.p, inner.n, ' ');
    if (!tk.fail) {
      size_t at = tk.n;
      for (size_t i = 0; i < tk.n; i++) {
        if (eq(tk.v[i], "at")) {
          at = i;
        }
      }
      size_t nrad = at;
      const bool circle = ieq(name, "circle");
      /* closest-side is the default radius: Shapes 1 (served 2026-10-31), ellipse(): the <radial-size> "If omitted it defaults to closest-side", and Images 4 gives
       * `<radial-size> = <radial-extent>{1,2} | <length-percentage [0,∞]>{1,2}`, so for an ellipse one `closest-side` and two are the default as well, and the
       * shortest form leaves them out (both pinned browsers write `ellipse(closest-side closest-side at 10% 20%)` as `ellipse(at 10% 20%)`) */
      if (nrad >= 1 && nrad <= (circle ? 1u : 2u)) {
        bool all_default = true;
        for (size_t i = 0; i < nrad; i++) {
          all_default = all_default && eq(tk.v[i], "closest-side");
        }
        if (all_default) {
          nrad = 0;
        }
      }
      size_t npos = at < tk.n ? tk.n - at - 1 : 0;
      cb pos = {out->a, NULL, 0, 0, false, &out->fail};
      if (npos == 1) {
        rule_position(&pos, tk.v + at + 1, 1);
      }
      const bool changed = nrad != at || (npos == 1 && pos.n > 0);
      if (changed && !pos.fail) {
        put_sp(out, name);
        cb_putc(out, '(');
        put_join(out, tk.v, nrad);
        if (at < tk.n) {
          if (nrad) {
            cb_putc(out, ' ');
          }
          cb_puts(out, "at ");
          if (pos.n) {
            cb_put(out, pos.d, pos.n);
          }
          else {
            put_join(out, tk.v + at + 1, npos);
          }
        }
        cb_putc(out, ')');
        done = true;
      }
      gtext_allocator_free(out->a, pos.d);
    }
    if (tk.fail) {
      out->fail = true;
    }
    gtext_allocator_free(out->a, tk.v);
    return done;
  }
  if (ieq(name, "ray")) {
    /* Motion 1 (served 2026-10-31), changes: "Made <ray-size> optional, defaulting to "closest-side"."; both pinned browsers write `ray(0rad closest-side)` as `ray(0rad)` */
    spv tk = {out->a, NULL, 0, 0, false, &out->fail};
    top_split(&tk, inner.p, inner.n, ' ');
    bool any = false;
    if (!tk.fail) {
      for (size_t i = 0; i < tk.n; i++) {
        any = any || eq(tk.v[i], "closest-side");
      }
      if (any) {
        put_sp(out, name);
        cb_putc(out, '(');
        bool first = true;
        for (size_t i = 0; i < tk.n; i++) {
          if (eq(tk.v[i], "closest-side")) {
            continue;
          }
          if (!first) {
            cb_putc(out, ' ');
          }
          put_sp(out, tk.v[i]);
          first = false;
        }
        cb_putc(out, ')');
      }
    }
    else {
      out->fail = true;
    }
    gtext_allocator_free(out->a, tk.v);
    return any;
  }
  if (ieq(name, "polygon") || ieq(name, "path") || ieq(name, "shape")) {
    spv args = {out->a, NULL, 0, 0, false, &out->fail};
    top_split(&args, inner.p, inner.n, ',');
    if (args.fail) {
      out->fail = true;
      gtext_allocator_free(out->a, args.v);
      return false;
    }
    cb tmp = {out->a, NULL, 0, 0, false, &out->fail};
    bool changed = false;
    if (ieq(name, "polygon") && args.n >= 2 && !is_coordinate_arg(args.v[0])) {
      spv head = {out->a, NULL, 0, 0, false, &out->fail};
      top_split(&head, args.v[0].p, args.v[0].n, ' ');
      cb hc = {out->a, NULL, 0, 0, false, &out->fail};
      for (size_t i = 0; i < head.n; i++) {
        if (eq(head.v[i], "nonzero")) {
          changed = true;
          continue;
        }
        if (eq(head.v[i], "round") && i + 1 < head.n && is_zero_radius(head.v[i + 1])) {
          changed = true;
          i++;
          continue;
        }
        if (hc.n) {
          cb_putc(&hc, ' ');
        }
        put_sp(&hc, head.v[i]);
      }
      if (changed) {
        put_sp(&tmp, name);
        cb_putc(&tmp, '(');
        bool wrote = false;
        if (hc.n) {
          cb_put(&tmp, hc.d, hc.n);
          wrote = true;
        }
        for (size_t i = 1; i < args.n; i++) {
          if (wrote) {
            cb_puts(&tmp, ", ");
          }
          put_sp(&tmp, args.v[i]);
          wrote = true;
        }
        cb_putc(&tmp, ')');
      }
      gtext_allocator_free(out->a, hc.d);
      gtext_allocator_free(out->a, head.v);
    }
    else if (ieq(name, "path") && (args.n == 1 || args.n == 2)) {
      /* path( <'fill-rule'>? , <string> ): `nonzero` is the default fill rule, and the data is written normalized */
      const sp data_arg = args.v[args.n - 1];
      const bool fill_ok = args.n == 1 || eq(args.v[0], "nonzero") || eq(args.v[0], "evenodd");
      cb data = {out->a, NULL, 0, 0, false, &out->fail};
      if (fill_ok && normalize_path_data(&data, data_arg) && !data.fail) {
        const bool nonzero = args.n == 2 && eq(args.v[0], "nonzero");
        if (nonzero || data.n != data_arg.n || memcmp(data.d, data_arg.p, data.n) != 0) {
          put_sp(&tmp, name);
          cb_putc(&tmp, '(');
          if (args.n == 2 && !nonzero) {
            cb_puts(&tmp, "evenodd, ");
          }
          cb_put(&tmp, data.d, data.n);
          cb_putc(&tmp, ')');
          changed = true;
        }
      }
      if (data.fail) {
        out->fail = true;
      }
      gtext_allocator_free(out->a, data.d);
    }
    else if (ieq(name, "shape") && args.n >= 2) {
      cb body = {out->a, NULL, 0, 0, false, &out->fail};
      for (size_t i = 0; i < args.n; i++) {
        sp a = args.v[i];
        if (i == 0 && a.n >= 8 && memcmp(a.p, "nonzero ", 8) == 0) {
          a = (sp){a.p + 8, a.n - 8};  /* `nonzero` is the default fill rule */
          changed = true;
        }
        if (i) {
          cb_puts(&body, ", ");
        }
        if (shape_command(&body, a)) {
          changed = true;
        }
        else {
          put_sp(&body, a);
        }
      }
      if (changed && !body.fail) {
        put_sp(&tmp, name);
        cb_putc(&tmp, '(');
        cb_put(&tmp, body.d, body.n);
        cb_putc(&tmp, ')');
      }
      gtext_allocator_free(out->a, body.d);
    }
    if (changed && !tmp.fail) {
      cb_put(out, tmp.d, tmp.n);
      done = true;
    }
    if (tmp.fail) {
      out->fail = true;
    }
    gtext_allocator_free(out->a, tmp.d);
    gtext_allocator_free(out->a, args.v);
    return done;
  }
  if (ieq(name, "inset") || ieq(name, "xywh") || ieq(name, "rect")) {
    /* a round clause whose radius is zero is not written */
    const char * r = NULL;
    for (size_t i = 0; i + 6 <= inner.n; i++) {
      if (memcmp(inner.p + i, "round ", 6) == 0 && (i == 0 || inner.p[i - 1] == ' ')) {
        r = inner.p + i;
        break;
      }
    }
    if (r) {
      const sp radius = {r + 6, (size_t)(inner.p + inner.n - (r + 6))};
      spv rt = {out->a, NULL, 0, 0, false, &out->fail};
      top_split(&rt, radius.p, radius.n, ' ');
      bool all_zero = !rt.fail && rt.n > 0;
      for (size_t i = 0; i < rt.n; i++) {
        all_zero = all_zero && is_zero_radius(rt.v[i]);
      }
      if (all_zero) {
        size_t cut = (size_t)(r - inner.p);
        while (cut > 0 && inner.p[cut - 1] == ' ') {
          cut--;
        }
        put_sp(out, name);
        cb_putc(out, '(');
        cb_put(out, inner.p, cut);
        cb_putc(out, ')');
        done = true;
      }
      if (rt.fail) {
        out->fail = true;
      }
      gtext_allocator_free(out->a, rt.v);
    }
  }
  return done;
}

static bool rule_shape(cb * out, const sp * t, size_t n) {
  bool changed = false;
  for (size_t i = 0; i < n; i++) {
    if (i) {
      cb_putc(out, ' ');
    }
    if (!shape_function(out, t[i])) {
      put_sp(out, t[i]);
    }
    else {
      changed = true;
    }
  }
  return changed;
}


/* ---- gradients (Images 4, 8) ------------------------------------------------------------------------------------------------------------------------------ */

static bool gradient_quantity(sp s) {
  if (!s.n) {
    return false;
  }
  const char c = s.p[0];
  return (c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' || (s.n > 5 && ieq((sp){s.p, 5}, "calc("));
}

static bool gradient_extent(sp s) {
  return eq(s, "closest-side") || eq(s, "closest-corner") || eq(s, "farthest-side") || eq(s, "farthest-corner");
}

/* the position of `at <position>` as the browsers write it: one value is completed (`70px` is `70px center`), and the center is not written at all */
static void gradient_position(cb * out, const sp * t, size_t n, bool * is_center, bool * ok) {
  *is_center = false;
  *ok = true;
  if (n == 1) {
    cb pos = {out->a, NULL, 0, 0, false, &out->fail};
    if (!rule_position(&pos, t, 1)) {
      *ok = false;
    }
    else {
      *is_center = pos.n == 13 && memcmp(pos.d, "center center", 13) == 0;
      if (!*is_center && t[0].n == 3 && memcmp(t[0].p, "50%", 3) == 0) {
        *is_center = true; /* 50% center is the center */
      }
      cb_put(out, pos.d, pos.n);
    }
    gtext_allocator_free(out->a, pos.d);
  }
  else if (n == 2) {
    const bool a = eq(t[0], "center") || eq(t[0], "50%");
    const bool b = eq(t[1], "center") || eq(t[1], "50%");
    *is_center = a && b;
    put_join(out, t, n);
  }
  else if (n >= 3) {
    put_join(out, t, n);
  }
  else {
    *ok = false;
  }
}

/* An <angle> that is a half turn, written in any unit the browsers fold it from: `180deg`, `200grad`, `0.5turn`. A radian is left alone: the browsers do not agree on it
 * (Chromium keeps `3.14159rad`, Firefox drops it), and the number is not exact in any text. */
static bool gradient_half_turn(sp s) {
  size_t i = 0;
  while (i < s.n && ((s.p[i] >= '0' && s.p[i] <= '9') || s.p[i] == '.' || s.p[i] == '+' || s.p[i] == '-')) {
    i++;
  }
  dec d;
  if (i == 0 || i == s.n || !dec_parse((sp){s.p, i}, &d) || d.neg) {
    return false;
  }
  /* the number compared as decimal text (src/css writes and reads numbers through the math library or as text, never with strtod): leading zeros of the integer part and trailing
   * zeros of the fraction do not count */
  size_t lead = 0;
  while (lead < d.ip.n && d.ip.p[lead] == '0') {
    lead++;
  }
  size_t fn = d.fp.n;
  while (fn > 0 && d.fp.p[fn - 1] == '0') {
    fn--;
  }
  const sp ip = {d.ip.p + lead, d.ip.n - lead};
  const sp fp = {d.fp.p, fn};
  const sp unit = {s.p + i, s.n - i};
  return (ieq(unit, "deg") && eq(ip, "180") && fp.n == 0) || (ieq(unit, "grad") && eq(ip, "200") && fp.n == 0) || (ieq(unit, "turn") && ip.n == 0 && eq(fp, "5"));
}

static bool gradient_angle(sp s) {
  if (!s.n || !((s.p[0] >= '0' && s.p[0] <= '9') || s.p[0] == '.' || s.p[0] == '+' || s.p[0] == '-')) {
    return false;
  }
  size_t i = 0;
  while (i < s.n && ((s.p[i] >= '0' && s.p[i] <= '9') || s.p[i] == '.' || s.p[i] == '+' || s.p[i] == '-' || s.p[i] == 'e' || s.p[i] == 'E')) {
    i++;
  }
  const sp unit = {s.p + i, s.n - i};
  return ieq(unit, "deg") || ieq(unit, "grad") || ieq(unit, "rad") || ieq(unit, "turn");
}

/* The `in <space>` of a gradient without what the grammar defaults: Images 4, 8 (served 2026-11-01), "omitting components when possible without changing the meaning", and both
 * pinned browsers write `in srgb` (the space a gradient is interpolated in when none is given) as nothing and `in oklch shorter hue` as `in oklch`. */
static void gradient_interpolation(cb * parts, const sp * t, size_t n) {
  if (n == 2 && eq(t[0], "in") && eq(t[1], "srgb")) {
    return;
  }
  if (n >= 4 && eq(t[n - 2], "shorter") && eq(t[n - 1], "hue")) {
    n -= 2;
  }
  if (parts->n) {
    cb_putc(parts, ' ');
  }
  put_join(parts, t, n);
}

/* Images 4, 8 (served 2026-11-01): a gradient is serialized "per its individual grammar, ... omitting components when possible without changing the meaning". The configuration
 * of a radial or conic gradient loses `from 0deg`, a center position, a lone `farthest-corner`, the `ellipse` of two sizes (or of none) and the `circle` of one size; a position of
 * one value is completed. Both pinned browsers write `conic-gradient(from 0deg at 50% 50%, red, gold)` as `conic-gradient(red, gold)` and `at 70px` as `at 70px center`. */
static bool gradient_function(cb * out, sp f) {
  const char * open = memchr(f.p, '(', f.n);
  if (!open || f.p[f.n - 1] != ')') {
    return false;
  }
  const sp name = {f.p, (size_t)(open - f.p)};
  const sp inner = {open + 1, f.n - name.n - 2};
  const bool conic = ieq(name, "conic-gradient") || ieq(name, "repeating-conic-gradient");
  const bool radial = ieq(name, "radial-gradient") || ieq(name, "repeating-radial-gradient");
  const bool linear = ieq(name, "linear-gradient") || ieq(name, "repeating-linear-gradient");
  if (!conic && !radial && !linear) {
    return false;
  }
  spv args = {out->a, NULL, 0, 0, false, &out->fail};
  top_split(&args, inner.p, inner.n, ',');
  bool done = false;
  if (!args.fail && args.n >= 2) {
    spv tk = {out->a, NULL, 0, 0, false, &out->fail};
    top_split(&tk, args.v[0].p, args.v[0].n, ' ');
    const bool config = !tk.fail && tk.n > 0 &&
        ((linear && (eq(tk.v[0], "to") || eq(tk.v[0], "in") || gradient_angle(tk.v[0]))) || (!linear && (eq(tk.v[0], "from") || eq(tk.v[0], "at") || eq(tk.v[0], "in"))) ||
         (radial && (eq(tk.v[0], "circle") || eq(tk.v[0], "ellipse") || gradient_extent(tk.v[0]) || gradient_quantity(tk.v[0]))));
    if (config) {
      size_t at = tk.n, in = tk.n;
      for (size_t i = 0; i < tk.n; i++) {
        if (at == tk.n && eq(tk.v[i], "at")) {
          at = i;
        }
        if (in == tk.n && eq(tk.v[i], "in")) {
          in = i;
        }
      }
      const size_t head = at < in ? at : in;
      cb parts = {out->a, NULL, 0, 0, false, &out->fail};
      size_t first = 0;
      if (linear) {
        /* Images 3, 7.1 and 8: a gradient line that is `to bottom` (a half turn in any unit) is the default of the grammar and is not written */
        const bool to_bottom = head == 2 && eq(tk.v[0], "to") && eq(tk.v[1], "bottom");
        if (!to_bottom && !(head == 1 && gradient_half_turn(tk.v[0]))) {
          put_join(&parts, tk.v, head);
        }
      }
      else if (conic) {
        if (head >= 2 && eq(tk.v[0], "from")) {
          if (!eq(tk.v[1], "0deg")) {
            cb_puts(&parts, "from ");
            put_sp(&parts, tk.v[1]);
          }
          first = 2;
        }
        if (first < head) {
          cb_puts(&parts, parts.n ? " " : "");
          put_join(&parts, tk.v + first, head - first);
        }
      }
      else {
        bool shape_circle = false, shape_ellipse = false;
        if (head > 0 && eq(tk.v[0], "circle")) {
          shape_circle = true;
          first = 1;
        }
        else if (head > 0 && eq(tk.v[0], "ellipse")) {
          shape_ellipse = true;
          first = 1;
        }
        const size_t nsize = head - first;
        bool lengths = nsize > 0;
        for (size_t i = first; i < head; i++) {
          lengths = lengths && gradient_quantity(tk.v[i]);
        }
        const bool lone_default = nsize == 1 && eq(tk.v[first], "farthest-corner");
        const bool implied = (shape_circle && lengths && nsize == 1) || (shape_ellipse && (nsize == 0 || (lengths && nsize == 2) || (nsize == 1 && gradient_extent(tk.v[first]))));
        if ((shape_circle || shape_ellipse) && !implied) {
          cb_puts(&parts, shape_circle ? "circle" : "ellipse");
        }
        if (nsize && !lone_default) {
          if (parts.n) {
            cb_putc(&parts, ' ');
          }
          put_join(&parts, tk.v + first, nsize);
        }
      }
      if (!linear && at < tk.n) {
        const size_t pend = in > at ? in : tk.n;
        cb pos = {out->a, NULL, 0, 0, false, &out->fail};
        bool is_center = false, ok = true;
        gradient_position(&pos, tk.v + at + 1, pend - at - 1, &is_center, &ok);
        if (ok && !is_center) {
          if (parts.n) {
            cb_putc(&parts, ' ');
          }
          cb_puts(&parts, "at ");
          cb_put(&parts, pos.d, pos.n);
        }
        else if (!ok) {
          /* a position this rule does not know is written as it is */
          if (parts.n) {
            cb_putc(&parts, ' ');
          }
          put_join(&parts, tk.v + at, pend - at);
        }
        gtext_allocator_free(out->a, pos.d);
      }
      if (in < tk.n) {
        gradient_interpolation(&parts, tk.v + in, tk.n - in);
      }
      /* the text of the first argument, as it was and as it would be written now */
      cb was = {out->a, NULL, 0, 0, false, &out->fail};
      put_join(&was, tk.v, tk.n);
      const bool changed = parts.n != was.n || (parts.n && memcmp(parts.d, was.d, parts.n) != 0);
      if (changed && !parts.fail && !was.fail) {
        put_sp(out, name);
        cb_putc(out, '(');
        bool wrote = false;
        if (parts.n) {
          cb_put(out, parts.d, parts.n);
          wrote = true;
        }
        for (size_t i = 1; i < args.n; i++) {
          if (wrote) {
            cb_puts(out, ", ");
          }
          put_sp(out, args.v[i]);
          wrote = true;
        }
        cb_putc(out, ')');
        done = true;
      }
      gtext_allocator_free(out->a, was.d);
      gtext_allocator_free(out->a, parts.d);
    }
    gtext_allocator_free(out->a, tk.v);
  }
  if (args.fail) {
    out->fail = true;
  }
  gtext_allocator_free(out->a, args.v);
  return done;
}

/* Images 4, 8 and 2.5 (served 2026-11-01): `image-set()` takes an <image> in each option, and Color 5 gives `light-dark()` the images of a property that takes them; the
 * gradient inside one is serialized as it is anywhere else, so the defaults it leaves out are left out there too (both pinned browsers write
 * `image-set(radial-gradient(farthest-corner, red, blue) 1x)` as `image-set(radial-gradient(red, blue) 1x)`). The nesting that is followed is bounded; a deeper one is left as it is. */
#define IMAGE_NEST_MAX 8u

/* Whether a token can be a position of a color stop: a number with a unit or a percentage, or a math function. */
static bool stop_position(sp t) {
  if (!t.n) {
    return false;
  }
  const char c = t.p[0];
  if ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-') {
    return !(t.n >= 2 && c == '-' && t.p[1] == '-');
  }
  return t.p[t.n - 1] == ')' && (ieq((sp){t.p, t.n >= 5 ? 5 : t.n}, "calc(") || (t.n > 4 && ieq((sp){t.p, 4}, "min(")) || (t.n > 4 && ieq((sp){t.p, 4}, "max(")) || (t.n > 6 && ieq((sp){t.p, 6}, "clamp(")));
}

/* Images 4 (served 2026-11-01), 3.5.3: "A color stop with two positions is equivalent to specifying two color stops with the same color", and both pinned browsers write the
 * two stops (`repeating-conic-gradient(color(srgb 1 0 0 / 50%) 2% 3%)` as `... 2%, color(srgb 1 0 0 / 50%) 3%)`). The text of the function with every such stop written as two. */
static bool gradient_stops(cb * out, sp f) {
  const char * open = memchr(f.p, '(', f.n);
  if (!open || f.p[f.n - 1] != ')') {
    return false;
  }
  const sp name = {f.p, (size_t)(open - f.p)};
  if (!(ieq(name, "linear-gradient") || ieq(name, "repeating-linear-gradient") || ieq(name, "radial-gradient") || ieq(name, "repeating-radial-gradient") ||
        ieq(name, "conic-gradient") || ieq(name, "repeating-conic-gradient"))) {
    return false;
  }
  const sp inner = {open + 1, f.n - name.n - 2};
  spv args = {out->a, NULL, 0, 0, false, &out->fail};
  top_split(&args, inner.p, inner.n, ',');
  bool changed = false;
  cb tmp = {out->a, NULL, 0, 0, false, &out->fail};
  cb_put(&tmp, f.p, name.n + 1);
  for (size_t i = 0; !args.fail && i < args.n; i++) {
    if (i) {
      cb_puts(&tmp, ", ");
    }
    spv tk = {out->a, NULL, 0, 0, false, &out->fail};
    top_split(&tk, args.v[i].p, args.v[i].n, ' ');
    static const char * const config[] = {"at", "from", "to", "in", "circle", "ellipse", "closest-side", "farthest-side", "closest-corner", "farthest-corner"};
    bool is_config = false;
    for (size_t k = 0; tk.n && k < sizeof config / sizeof *config; k++) {
      is_config = is_config || ieq(tk.v[0], config[k]);
    }
    if (!tk.fail && tk.n == 3 && !is_config && !stop_position(tk.v[0]) && stop_position(tk.v[1]) && stop_position(tk.v[2])) {
      put_sp(&tmp, tk.v[0]);
      cb_putc(&tmp, ' ');
      put_sp(&tmp, tk.v[1]);
      cb_puts(&tmp, ", ");
      put_sp(&tmp, tk.v[0]);
      cb_putc(&tmp, ' ');
      put_sp(&tmp, tk.v[2]);
      changed = true;
    }
    else {
      put_sp(&tmp, args.v[i]);
    }
    if (tk.fail) {
      args.fail = true;
    }
    gtext_allocator_free(out->a, tk.v);
  }
  cb_putc(&tmp, ')');
  const bool done = changed && !args.fail && !tmp.fail;
  if (done) {
    cb_put(out, tmp.d, tmp.n);
  }
  gtext_allocator_free(out->a, tmp.d);
  gtext_allocator_free(out->a, args.v);
  return done;
}

static bool image_function(cb * out, sp f, unsigned depth) {
  cb split = {out->a, NULL, 0, 0, false, &out->fail};
  if (gradient_stops(&split, f) && !split.fail && split.d) {
    /* the stops of the gradient are written first; the rest of the rewriting reads that text */
    const sp g = {split.d, split.n};
    const bool again = gradient_function(out, g);
    if (!again) {
      cb_put(out, split.d, split.n);
    }
    gtext_allocator_free(out->a, split.d);
    return true;
  }
  gtext_allocator_free(out->a, split.d);
  if (gradient_function(out, f)) {
    return true;
  }
  if (depth >= IMAGE_NEST_MAX) {
    return false;
  }
  const char * open = memchr(f.p, '(', f.n);
  if (!open || f.p[f.n - 1] != ')') {
    return false;
  }
  const sp name = {f.p, (size_t)(open - f.p)};
  const sp inner = {open + 1, f.n - name.n - 2};
  if (!ieq(name, "image-set") && !ieq(name, "-webkit-image-set") && !ieq(name, "light-dark")) {
    return false;
  }
  spv args = {out->a, NULL, 0, 0, false, &out->fail};
  top_split(&args, inner.p, inner.n, ',');
  cb tmp = {out->a, NULL, 0, 0, false, &out->fail};
  bool changed = false;
  if (ieq(name, "-webkit-image-set")) {
    /* Images 4, Appendix A: "-webkit-image-set() ... is turned into image-set() during parsing" */
    cb_puts(&tmp, "image-set(");
    changed = true;
  }
  else {
    cb_put(&tmp, f.p, name.n + 1);
  }
  for (size_t i = 0; !args.fail && i < args.n; i++) {
    if (i) {
      cb_puts(&tmp, ", ");
    }
    spv tk = {out->a, NULL, 0, 0, false, &out->fail};
    top_split(&tk, args.v[i].p, args.v[i].n, ' ');
    /* Images 4, 2.4 (served 2026-11-01): "Each <string> inside image-set() represents a <url>", and an option with no resolution "behaves as 1x"; both pinned browsers
       write `image-set("a" type("image/png"))` as `image-set(url("a") 1x type("image/png"))`. */
    const bool is_set = !ieq(name, "light-dark");
    bool has_resolution = false;
    for (size_t j = 1; is_set && j < tk.n; j++) {
      const char c0 = tk.v[j].n ? tk.v[j].p[0] : '\0';
      has_resolution = has_resolution || (c0 >= '0' && c0 <= '9') || c0 == '.' || c0 == '+' || c0 == '-';
    }
    for (size_t j = 0; !tk.fail && j < tk.n; j++) {
      if (j) {
        cb_putc(&tmp, ' ');
      }
      cb sub = {out->a, NULL, 0, 0, false, &out->fail};
      if (is_set && j == 0 && tk.v[j].n >= 2 && (tk.v[j].p[0] == '"' || tk.v[j].p[0] == '\'')) {
        cb_puts(&tmp, "url(");
        put_sp(&tmp, tk.v[j]);
        cb_putc(&tmp, ')');
        changed = true;
      }
      else if (memchr(tk.v[j].p, '(', tk.v[j].n) && image_function(&sub, tk.v[j], depth + 1)) {
        cb_put(&tmp, sub.d, sub.n);
        changed = true;
      }
      else {
        put_sp(&tmp, tk.v[j]);
      }
      gtext_allocator_free(out->a, sub.d);
      if (is_set && j == 0 && !has_resolution && tk.n >= 1) {
        cb_puts(&tmp, " 1x");
        changed = true;
      }
    }
    if (tk.fail) {
      args.fail = true;
    }
    gtext_allocator_free(out->a, tk.v);
  }
  cb_putc(&tmp, ')');
  const bool done = changed && !args.fail && !tmp.fail;
  if (done) {
    cb_put(out, tmp.d, tmp.n);
  }
  gtext_allocator_free(out->a, tmp.d);
  gtext_allocator_free(out->a, args.v);
  return done;
}

static bool rule_gradient(cb * out, const sp * t, size_t n) {
  if (n != 1) {
    return false;
  }
  return image_function(out, t[0], 0);
}

/* Content 3: `content` takes images where it takes a <content-replacement> or a <content-list> item; the functions among its tokens are rewritten as the images of the other
 * properties are, the rest is left as it is. */
static bool rule_content(cb * out, const sp * t, size_t n) {
  cb tmp = {out->a, NULL, 0, 0, false, &out->fail};
  bool changed = false;
  for (size_t i = 0; i < n; i++) {
    if (i) {
      cb_putc(&tmp, ' ');
    }
    cb sub = {out->a, NULL, 0, 0, false, &out->fail};
    if (memchr(t[i].p, '(', t[i].n) && image_function(&sub, t[i], 0)) {
      cb_put(&tmp, sub.d, sub.n);
      changed = true;
    }
    else {
      put_sp(&tmp, t[i]);
    }
    gtext_allocator_free(out->a, sub.d);
  }
  if (changed && !tmp.fail) {
    cb_put(out, tmp.d, tmp.n);
  }
  const bool done = changed && !tmp.fail;
  gtext_allocator_free(out->a, tmp.d);
  return done;
}

/* Masking 1 (served 2026-10-31): "If no reference box is specified, the border-box will be used as reference box" for a clip-path basic shape, and Motion 1: "If
 * <coord-box> is omitted, it defaults to border-box" for offset-path; the shortest form leaves the default out beside a function (alone it is the value) and writes
 * the shape first (both pinned browsers write `border-box circle(7% at 8% 9%)` as `circle(7% at 8% 9%)`). */
static bool rule_shape_default_box(cb * out, const sp * t, size_t n) {
  if (n == 2 && (eq(t[0], "border-box") || eq(t[1], "border-box"))) {
    const sp fn = eq(t[0], "border-box") ? t[1] : t[0];
    if (memchr(fn.p, '(', fn.n)) {
      if (!shape_function(out, fn)) {
        put_sp(out, fn);
      }
      return true;
    }
  }
  return rule_shape(out, t, n);
}

/* Shapes 1 (served 2026-11-01), shape-outside: "If <shape-box> is not supplied, then the reference box defaults to margin-box"; the shortest form leaves it out beside a
 * function, writes the shape first, and an image alone is the value (both pinned browsers write `circle() margin-box` as `circle()`). */
static bool rule_shape_outside(cb * out, const sp * t, size_t n) {
  if (n == 2 && (eq(t[0], "margin-box") || eq(t[1], "margin-box"))) {
    const sp fn = eq(t[0], "margin-box") ? t[1] : t[0];
    if (memchr(fn.p, '(', fn.n)) {
      if (!shape_function(out, fn)) {
        put_sp(out, fn);
      }
      return true;
    }
  }
  return rule_shape(out, t, n);
}

/* Text 4 (served 2026-11-01), text-justify: "UAs must also support the alternate keyword distribute which must compute to inter-character, thus having the exact same meaning
 * and behavior"; both pinned browsers write it as inter-character already in the specified value. */
static bool rule_text_justify(cb * out, const sp * t, size_t n) {
  bool changed = false;
  for (size_t i = 0; i < n; i++) {
    if (i) {
      cb_putc(out, ' ');
    }
    if (ieq(t[i], "distribute")) {
      cb_puts(out, "inter-character");
      changed = true;
    }
    else {
      put_sp(out, t[i]);
    }
  }
  return changed;
}

/* Scroll-driven Animations 1, <timeline-range-name> <length-percentage>?: the start of a range name is `0%` and its end `100%` by default, and
 * what is the default is not written. */
static bool is_range_name(sp t) {
  return ieq(t, "entry") || ieq(t, "exit") || ieq(t, "cover") || ieq(t, "contain") || ieq(t, "entry-crossing") || ieq(t, "exit-crossing");
}

static bool rule_range_start(cb * out, const sp * t, size_t n) {
  if (n == 2 && is_range_name(t[0]) && eq(t[1], "0%")) {
    put_sp(out, t[0]);
    return true;
  }
  return false;
}

static bool rule_range_end(cb * out, const sp * t, size_t n) {
  if (n == 2 && is_range_name(t[0]) && eq(t[1], "100%")) {
    put_sp(out, t[0]);
    return true;
  }
  return false;
}

/* Overflow 4, `overflow-clip-margin: <visual-box> || <length>`: the box is `padding-box` and the length `0px` by default, and what is the default is not
 * written, except that nothing is `0px`. */
static bool is_visual_box(sp t) {
  return ieq(t, "content-box") || ieq(t, "padding-box") || ieq(t, "border-box");
}

static bool rule_clip_margin(cb * out, const sp * t, size_t n) {
  if (n < 1 || n > 2) {
    return false;
  }
  size_t box = n, len = n;
  for (size_t i = 0; i < n; i++) {
    if (is_visual_box(t[i])) {
      box = i;
    }
    else {
      len = i;
    }
  }
  if ((n == 2 && (box == n || len == n)) || (n == 1 && box == n && len == n)) {
    return false;
  }
  const bool keep_box = box != n && !ieq(t[box], "padding-box");
  const bool keep_len = len != n && !is_zero_absolute_length(t[len]);
  if (!keep_box && !keep_len) {
    cb_puts(out, "0px");
    return true;
  }
  if (keep_box) {
    put_sp(out, t[box]);
  }
  if (keep_len) {
    if (keep_box) {
      cb_putc(out, ' ');
    }
    put_sp(out, t[len]);
  }
  return true;
}

/* Motion Path 1, `offset-rotate: [ auto | reverse ] || <angle>`: an angle of zero adds nothing to a keyword, and the angle follows it. */
static bool rule_offset_rotate(cb * out, const sp * t, size_t n) {
  if (n != 2) {
    return false;
  }
  const size_t kw = (eq(t[0], "auto") || eq(t[0], "reverse")) ? 0 : (eq(t[1], "auto") || eq(t[1], "reverse")) ? 1 : n;
  if (kw == n) {
    return false;
  }
  const size_t ang = 1 - kw;
  if (is_zero_dim(t[ang])) {
    put_sp(out, t[kw]);
    return true;
  }
  if (kw == 0) {
    return false;
  }
  put_sp(out, t[kw]);
  cb_putc(out, ' ');
  put_sp(out, t[ang]);
  return true;
}

/* Borders 4, `border-image-width`, `-outset` and `-slice` (and the mask-border ones) are `<x>{1,4}`: a side that repeats the one it would copy is
 * not written, the way `margin` is. `fill` of a slice stays at the end. */
static bool rule_box_sides(cb * out, const sp * t, size_t n) {
  size_t m = n;
  bool fill = false;
  sp v[4];
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (eq(t[i], "fill")) {
      fill = true;
      m--;
    }
    else if (k < 4) {
      v[k++] = t[i];
    }
  }
  if (k != m || k < 2) {
    return false;
  }
  size_t keep = k;
  if (keep == 4 && sp_eq(v[3], v[1])) {
    keep = 3;
  }
  if (keep == 3 && sp_eq(v[2], v[0])) {
    keep = 2;
  }
  if (keep == 2 && sp_eq(v[1], v[0])) {
    keep = 1;
  }
  if (keep == k && !(fill && t[0].n == 4 && eq(t[0], "fill"))) {
    return false;
  }
  for (size_t i = 0; i < keep; i++) {
    if (i) {
      cb_putc(out, ' ');
    }
    put_sp(out, v[i]);
  }
  if (fill) {
    cb_puts(out, " fill");
  }
  return true;
}

/* Masking 1, Appendix A: "User agents must support separation with commas, but may also support separation without commas"; "Authors should separate offset values with
 * commas." The two pinned browsers write the commas (cases clip.4), and the text gives no serialization of the other form: `rect(1px 2px 3px 4px)` is written with them. */
/* Fonts 4, font-palette: "Percentages are normalized according to CSS Color 5 3.2 Percentage Normalization" for palette-mix(): the percentages that are equal shares are not
 * written, the others are written all (an omitted one as the share it takes), and the default interpolation method (oklab) is left out; the computed value's own writer
 * does this (css_shape_compute.c), so the specified value is the same text. */
static bool rule_palette_mix(cb * out, const char * text, size_t len) {
  if (len < 12 || strncmp(text, "palette-mix(", 12) != 0) {
    return false;
  }
  char * w = NULL;
  size_t wn = 0;
  if (!css_palette_mix_canonical(out->a, text, len, &w, &wn)) {
    out->fail = true;
    return false;
  }
  if (!w) {
    return false;
  }
  cb_put(out, w, wn);
  gtext_allocator_free(out->a, w);
  return true;
}

static bool rule_clip_rect_commas(cb * out, const char * text, size_t len) {
  if (len < 6 || strncmp(text, "rect(", 5) != 0 || text[len - 1] != ')' || memchr(text, ',', len)) {
    return false;
  }
  cb_put(out, text, 5);
  bool gap = false;
  bool first = true;
  size_t depth = 0;
  for (size_t i = 5; i + 1 < len; i++) {
    if (text[i] == '(') {
      depth++;
    }
    else if (text[i] == ')' && depth) {
      depth--;
    }
    if (text[i] == ' ' && depth == 0) {
      gap = true;
      continue;
    }
    if (gap && !first) {
      cb_puts(out, ", ");
    }
    gap = false;
    first = false;
    cb_putc(out, text[i]);
  }
  cb_putc(out, ')');
  return true;
}

/* Grid 2 (served 2026-10-31), 7.3.1, Serialization Of Template Strings: "each null cell token is serialized as a single "." (U+002E FULL STOP)", "consecutive
 * cell tokens are separated by a single space (U+0020 SPACE)" "with all other white space elided." A string is written with the escape `\9 ` for a tab. */
static size_t areas_ws(const char * t, size_t k, size_t end) {
  if (t[k] == ' ' || t[k] == '\t' || t[k] == '\n') {
    return 1;
  }
  if (t[k] == '\\' && k + 2 < end && t[k + 1] == '9' && t[k + 2] == ' ') {
    return 3;
  }
  return 0;
}

static bool rule_grid_areas(cb * out, const char * text, size_t len) {
  size_t i = 0;
  while (i < len) {
    if (text[i] != '"') {
      cb_putc(out, text[i++]);
      continue;
    }
    size_t j = i + 1;
    while (j < len && text[j] != '"') {
      j++;
    }
    if (j >= len) {
      cb_put(out, text + i, len - i);
      return true;
    }
    cb_putc(out, '"');
    bool first = true;
    size_t k = i + 1;
    while (k < j) {
      size_t u;
      while (k < j && (u = areas_ws(text, k, j)) > 0) {
        k += u;
      }
      size_t e = k;
      bool dots = true;
      while (e < j && areas_ws(text, e, j) == 0) {
        dots = dots && text[e] == '.';
        e++;
      }
      if (e > k) {
        if (!first) {
          cb_putc(out, ' ');
        }
        first = false;
        if (dots) {
          cb_putc(out, '.');
        }
        else {
          cb_put(out, text + k, e - k);
        }
      }
      k = e;
    }
    cb_putc(out, '"');
    i = j + 1;
  }
  return true;
}

/* ---- the table ------------------------------------------------------------------------------------------------------------- */

typedef enum { MODE_ITEMS = 1, MODE_WHOLE, MODE_TEXT } canon_mode;

typedef bool (*text_rule)(cb * out, const char * text, size_t len);

typedef struct {
  const char * property;
  item_rule rule;
  canon_mode mode;
  text_rule text;
} canon_row;

#define R(property, rule, mode) {property, rule, mode, NULL}
#define T(property, text) {property, NULL, MODE_TEXT, text}

static const canon_row kRows[] = {
  /* sorted by property */
  R("align-content", rule_first_baseline, MODE_ITEMS),
  R("align-items", rule_first_baseline, MODE_ITEMS),
  R("align-self", rule_first_baseline, MODE_ITEMS),
  R("animation-name", rule_animation_name, MODE_ITEMS),
  R("animation-range-end", rule_range_end, MODE_ITEMS),
  R("animation-range-start", rule_range_start, MODE_ITEMS),
  R("animation-timing-function", rule_steps, MODE_ITEMS),
  R("backdrop-filter", rule_filter, MODE_WHOLE),
  R("background-image", rule_gradient, MODE_ITEMS),
  R("background-position", rule_position, MODE_ITEMS),
  R("background-repeat", rule_repeat_style, MODE_ITEMS),
  R("background-size", rule_bg_size, MODE_ITEMS),
  R("border-bottom-left-radius", rule_corner_radius, MODE_WHOLE),
  R("border-bottom-right-radius", rule_corner_radius, MODE_WHOLE),
  R("border-end-end-radius", rule_corner_radius, MODE_WHOLE),
  R("border-end-start-radius", rule_corner_radius, MODE_WHOLE),
  R("border-image-outset", rule_box_sides, MODE_WHOLE),
  R("border-image-repeat", rule_pair, MODE_WHOLE),
  R("border-image-slice", rule_box_sides, MODE_WHOLE),
  R("border-image-source", rule_gradient, MODE_ITEMS),
  R("border-image-width", rule_box_sides, MODE_WHOLE),
  R("border-shape", rule_shape, MODE_WHOLE),
  R("border-start-end-radius", rule_corner_radius, MODE_WHOLE),
  R("border-start-start-radius", rule_corner_radius, MODE_WHOLE),
  R("border-top-left-radius", rule_corner_radius, MODE_WHOLE),
  R("border-top-right-radius", rule_corner_radius, MODE_WHOLE),
  R("color-interpolation-filters", NULL, 0),
  T("clip", rule_clip_rect_commas),
  T("font-palette", rule_palette_mix),
  R("clip-path", rule_shape_default_box, MODE_WHOLE),
  R("color-scheme", rule_color_scheme, MODE_WHOLE),
  R("content", rule_content, MODE_WHOLE),
  R("counter-increment", rule_counter_one, MODE_WHOLE),
  R("counter-reset", rule_counter_zero, MODE_WHOLE),
  R("counter-set", rule_counter_zero, MODE_WHOLE),
  R("cursor", rule_cursor, MODE_ITEMS),
  R("display", rule_display, MODE_WHOLE),
  R("fill-opacity", rule_alpha_percent, MODE_WHOLE),
  R("filter", rule_filter, MODE_WHOLE),
  R("flex-wrap", rule_flex_wrap, MODE_WHOLE),
  R("flood-opacity", rule_alpha_percent, MODE_WHOLE),
  R("font-family", rule_font_family, MODE_ITEMS),
  R("font-feature-settings", rule_font_feature, MODE_ITEMS),
  R("font-language-override", rule_lang_override, MODE_WHOLE),
  R("font-size-adjust", rule_font_size_adjust, MODE_WHOLE),
  R("font-style", rule_font_style, MODE_WHOLE),
  R("grid-auto-flow", rule_grid_auto_flow, MODE_WHOLE),
  R("grid-column-end", rule_grid_line, MODE_WHOLE),
  R("grid-column-start", rule_grid_line, MODE_WHOLE),
  R("grid-row-end", rule_grid_line, MODE_WHOLE),
  R("grid-row-start", rule_grid_line, MODE_WHOLE),
  T("grid-template-areas", rule_grid_areas),
  T("grid-template-columns", rule_grid_template_tracks),
  T("grid-template-rows", rule_grid_template_tracks),
  R("justify-items", rule_first_baseline, MODE_ITEMS),
  R("justify-self", rule_first_baseline, MODE_ITEMS),
  R("list-style-image", rule_gradient, MODE_ITEMS),
  R("list-style-type", rule_symbols, MODE_WHOLE),
  R("margin-trim", rule_margin_trim, MODE_WHOLE),
  R("mask-image", rule_gradient, MODE_ITEMS),
  R("mask-position", rule_position, MODE_ITEMS),
  R("mask-repeat", rule_repeat_style, MODE_ITEMS),
  R("mask-size", rule_bg_size, MODE_ITEMS),
  R("object-fit", rule_object_fit, MODE_WHOLE),
  R("object-position", rule_position, MODE_ITEMS),
  R("offset-anchor", rule_position, MODE_ITEMS),
  R("offset-path", rule_shape_default_box, MODE_WHOLE),
  R("offset-position", rule_position, MODE_ITEMS),
  R("offset-rotate", rule_offset_rotate, MODE_WHOLE),
  R("opacity", rule_alpha_percent, MODE_WHOLE),
  R("overflow-clip-margin-bottom", rule_clip_margin, MODE_WHOLE),
  R("overflow-clip-margin-left", rule_clip_margin, MODE_WHOLE),
  R("overflow-clip-margin-right", rule_clip_margin, MODE_WHOLE),
  R("overflow-clip-margin-top", rule_clip_margin, MODE_WHOLE),
  R("paint-order", rule_paint_order, MODE_WHOLE),
  R("perspective-origin", rule_position, MODE_ITEMS),
  R("position-area", rule_position_area, MODE_WHOLE),
  R("position-try-fallbacks", rule_position_area, MODE_ITEMS),
  R("rotate", rule_rotate, MODE_WHOLE),
  R("ruby-overhang", rule_ruby_overhang, MODE_WHOLE),
  R("scale", rule_scale, MODE_WHOLE),
  R("scroll-snap-align", rule_pair, MODE_WHOLE),
  R("scroll-snap-type", rule_scroll_snap_type, MODE_WHOLE),
  R("shape-image-threshold", rule_alpha_percent, MODE_WHOLE),
  R("shape-outside", rule_shape_outside, MODE_WHOLE),
  R("stop-opacity", rule_alpha_percent, MODE_WHOLE),
  R("stroke-opacity", rule_alpha_percent, MODE_WHOLE),
  R("text-box-edge", rule_text_edge, MODE_WHOLE),
  R("line-fit-edge", rule_text_edge, MODE_WHOLE),
  R("text-justify", rule_text_justify, MODE_WHOLE),
  R("transform", rule_transform, MODE_WHOLE),
  R("transform-origin", rule_transform_origin, MODE_WHOLE),
  R("transition-timing-function", rule_steps, MODE_ITEMS),
  R("translate", rule_translate, MODE_WHOLE),
};

#undef R
#undef T

#define ROWS (sizeof kRows / sizeof kRows[0])

uint8_t css_canon_rules_of(const char * name, size_t length) {
  for (size_t i = 0; i < ROWS; i++) {
    if (strlen(kRows[i].property) == length && memcmp(kRows[i].property, name, length) == 0) {
      return (uint8_t)(i + 1);
    }
  }
  return 0;
}

static void lower_into(cb * out, const char * text, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = lower(text[i]);
    cb_putc(out, c);
  }
}

GTEXT_CSS_Status css_canon_apply(uint8_t id, const GTEXT_Allocator * a, const char * text, size_t length, char ** out, size_t * out_length) {
  cb b = {a, NULL, 0, 0, false, NULL};
  if (id == 0 || id > ROWS) {
    return GTEXT_CSS_E_INVALID;
  }
  const canon_row * row = &kRows[id - 1];
  if (row->mode == MODE_TEXT) {
    if (!row->text(&b, text, length)) {
      b.n = 0;
      if (b.d) {
        b.d[0] = '\0';
      }
      cb_put(&b, text, length);
    }
  }
  else if (!row->rule) {
    lower_into(&b, text, length);
  }
  else if (row->mode == MODE_ITEMS) {
    if (!run_items(&b, text, length, row->rule, ' ') && b.fail) {
      gtext_allocator_free(a, b.d);
      return GTEXT_CSS_E_OOM;
    }
  }
  else {
    spv toks = {a, NULL, 0, 0, false, NULL};
    top_split(&toks, text, length, ' ');
    if (toks.fail) {
      gtext_allocator_free(a, toks.v);
      return GTEXT_CSS_E_OOM;
    }
    if (!(toks.n && row->rule(&b, toks.v, toks.n))) {
      b.n = 0;
      if (b.d) {
        b.d[0] = '\0';
      }
      cb_put(&b, text, length);
    }
    gtext_allocator_free(a, toks.v);
  }
  if (b.fail) {
    gtext_allocator_free(a, b.d);
    return GTEXT_CSS_E_OOM;
  }
  if (!b.d) {
    b.d = gtext_allocator_malloc(a, 1);
    if (!b.d) {
      return GTEXT_CSS_E_OOM;
    }
    b.d[0] = '\0';
  }
  *out = b.d;
  *out_length = b.n;
  return GTEXT_CSS_OK;
}
