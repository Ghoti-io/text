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
 * The computed value of a basic shape (`clip-path`, `shape-outside`, `offset-path`, `border-shape`).
 *
 * CSS Shapes 1 (Editor's Draft of 8 October 2026, served 2026-10-30), 3.2: "The values in a <basic-shape> function are computed as specified, with these
 * exceptions:" and "All <basic-shape-rect> functions compute to the equivalent inset() function."; 3.3: "Omitting optional components means that default
 * values do not show up in the serialization." and "<position> serialization rules mean that keywords compute to percentages, and serialize in
 * horizontal-vertical order." Given rect(t r b l) "the equivalent function is inset(t calc(100% - r) calc(100% - b) l)" and given xywh(x y w h) it is
 * "inset(y calc(100% - x - w) calc(100% - y - h) x)"; rect()'s "auto value makes the edge of the box coincide with the corresponding edge of the reference box".
 *
 * Where the text is silent the pinned pages decide (and both browsers, recorded at the end of the slice): a ray's angle is in degrees, a ray's
 * default size is not written, shape()'s commands drop their defaults, and a reference box that is the
 * default of its property is not written.
 *
 * **The way it is done.** The value is first computed by its types (lengths absolute, calculations simplified), then written, rewritten as text by the
 * rules above, read back as the property is, and computed by its types again: the second pass is what turns `calc(100% - (1%))` into `99%`. The rewrite
 * reads and writes text and nothing else; it has no recursion (a function nests only through `calc()`, which it never opens), it is linear in the text,
 * and every allocation is the context's.
 */

#include <stdint.h>
#include <string.h>

#include "css_compute_internal.h"
#include "css_internal.h"

#include <ghoti.io/text/css/css_serialize.h>

/* ---- a growing text ------------------------------------------------------------------------------------------ */

typedef struct {
  const GTEXT_Allocator * a;
  char * d;
  size_t n;
  size_t cap;
  bool fail;
} sb;

static void sb_put(sb * b, const char * p, size_t n) {
  if (b->fail || n == 0) {
    return;
  }
  if (b->n + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->n + n + 1) {
      if (cap > SIZE_MAX / 4) {
        b->fail = true;
        return;
      }
      cap *= 2;
    }
    char * grown = gtext_allocator_realloc((GTEXT_Allocator *)b->a, b->d, cap);
    if (!grown) {
      b->fail = true;
      return;
    }
    b->d = grown;
    b->cap = cap;
  }
  memcpy(b->d + b->n, p, n);
  b->n += n;
  b->d[b->n] = '\0';
}

static void sb_puts(sb * b, const char * s) {
  sb_put(b, s, strlen(s));
}

static void sb_putc(sb * b, char c) {
  sb_put(b, &c, 1);
}

/* ---- spans and the top-level split ------------------------------------------------------------------------------ */

typedef struct {
  const char * p;
  size_t n;
} span;

typedef struct {
  const GTEXT_Allocator * a;
  span * v;
  size_t n;
  size_t cap;
  bool fail;
} spv;

static void spv_push(spv * s, span x) {
  if (s->fail) {
    return;
  }
  if (s->n == s->cap) {
    const size_t cap = s->cap ? s->cap * 2 : 8;
    span * grown = gtext_allocator_realloc((GTEXT_Allocator *)s->a, s->v, cap * sizeof *grown);
    if (!grown) {
      s->fail = true;
      return;
    }
    s->v = grown;
    s->cap = cap;
  }
  s->v[s->n++] = x;
}

static bool is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/// Split at the top level (outside parentheses, brackets and strings) on white space (sep ' ': a run of it) or on a comma (sep ','; the white space around it goes).
static void split_top(spv * out, span in, char sep) {
  size_t depth = 0;
  size_t start = 0;
  char quote = 0;
  for (size_t i = 0; i <= in.n; i++) {
    const char c = i < in.n ? in.p[i] : 0;
    if (i < in.n && quote) {
      if (c == '\\' && i + 1 < in.n) {
        i++;
      }
      else if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (i < in.n && (c == '"' || c == '\'')) {
      quote = c;
      continue;
    }
    if (i < in.n && (c == '(' || c == '[' || c == '{')) {
      depth++;
      continue;
    }
    if (i < in.n && (c == ')' || c == ']' || c == '}')) {
      if (depth) {
        depth--;
      }
      continue;
    }
    const bool at_sep = i == in.n || (depth == 0 && (sep == ' ' ? is_ws(c) : c == sep));
    if (!at_sep) {
      continue;
    }
    size_t a = start;
    size_t b = i;
    while (a < b && is_ws(in.p[a])) {
      a++;
    }
    while (b > a && is_ws(in.p[b - 1])) {
      b--;
    }
    if (b > a || sep == ',') {
      spv_push(out, (span){in.p + a, b - a});
    }
    start = i + 1;
  }
}

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool eq(span s, const char * w) {
  const size_t n = strlen(w);
  if (s.n != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (lower(s.p[i]) != w[i]) {
      return false;
    }
  }
  return true;
}

static bool starts_fn(span s, const char * name) {
  const size_t n = strlen(name);
  if (s.n < n + 2 || s.p[n] != '(' || s.p[s.n - 1] != ')') {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (lower(s.p[i]) != name[i]) {
      return false;
    }
  }
  return true;
}

static span fn_inner(span f, size_t name_len) {
  return (span){f.p + name_len + 1, f.n - name_len - 2};
}

/// Whether a token is an identifier (a keyword) and not a number, a dimension, a function, a string.
static bool is_ident(span s) {
  if (!s.n) {
    return false;
  }
  const char c = lower(s.p[0]);
  return (c >= 'a' && c <= 'z') || c == '-' ? !(s.n > 1 && c == '-' && ((s.p[1] >= '0' && s.p[1] <= '9') || s.p[1] == '.')) && !memchr(s.p, '(', s.n) : false;
}

static void put_span(sb * b, span s) {
  sb_put(b, s.p, s.n);
}

/// `calc(100% - (a) - (b))` for lengths-percentages or calculations; a literal zero length is not a term (the pages write `100%` for `rect(0 0 0 0)`'s far edges).
static void put_from_end2(sb * b, span x, span y) {
  const bool zx = eq(x, "0px") || x.n == 0;
  const bool zy = eq(y, "0px") || y.n == 0;
  if (zx && zy) {
    sb_puts(b, "100%");
    return;
  }
  sb_puts(b, "calc(100%");
  if (!zx) {
    sb_puts(b, " - (");
    put_span(b, x);
    sb_putc(b, ')');
  }
  if (!zy) {
    sb_puts(b, " - (");
    put_span(b, y);
    sb_putc(b, ')');
  }
  sb_putc(b, ')');
}

static void put_from_end(sb * b, span x) {
  put_from_end2(b, x, (span){NULL, 0});
}

/* ---- positions ------------------------------------------------------------------------------------------------- */

typedef enum { AX_NONE, AX_H, AX_V, AX_C } axis_kind;

static axis_kind axis_of(span s) {
  if (eq(s, "left") || eq(s, "right")) {
    return AX_H;
  }
  if (eq(s, "top") || eq(s, "bottom")) {
    return AX_V;
  }
  if (eq(s, "center")) {
    return AX_C;
  }
  return AX_NONE;
}

static const char * kw_percent(span s) {
  if (eq(s, "left") || eq(s, "top")) {
    return "0%";
  }
  if (eq(s, "center")) {
    return "50%";
  }
  return "100%";
}

/// A `<position>` of one to four terms as its horizontal and vertical offsets from the top left (Shapes 1, 3.3: "keywords compute to percentages, and
/// serialize in horizontal-vertical order"); a far-edge offset is `calc(100% - (offset))`. False when the terms are not a position.
static bool position_pair(sb * h, sb * v, const span * t, size_t n) {
  if (n == 0 || n > 4) {
    return false;
  }
  if (n <= 2) {
    span a = t[0];
    span b = n == 2 ? t[1] : (span){"center", 6};
    axis_kind ka = axis_of(a);
    axis_kind kb = axis_of(b);
    if (n == 1) {
      /* one term: a vertical keyword is the vertical offset, anything else the horizontal one */
      if (ka == AX_V) {
        sb_puts(h, "50%");
        sb_puts(v, kw_percent(a));
        return true;
      }
      if (ka == AX_NONE && is_ident(a)) {
        return false;
      }
      if (ka == AX_NONE) {
        put_span(h, a);
      }
      else {
        sb_puts(h, kw_percent(a));
      }
      sb_puts(v, "50%");
      return true;
    }
    if ((is_ident(a) && ka == AX_NONE) || (is_ident(b) && kb == AX_NONE)) {
      return false;
    }
    if (ka == AX_V || kb == AX_H) {
      span x = a;
      a = b;
      b = x;
      axis_kind y = ka;
      ka = kb;
      kb = y;
    }
    if (ka == AX_V || kb == AX_H) {
      return false;
    }
    if (ka == AX_NONE) {
      put_span(h, a);
    }
    else {
      sb_puts(h, kw_percent(a));
    }
    if (kb == AX_NONE) {
      put_span(v, b);
    }
    else {
      sb_puts(v, kw_percent(b));
    }
    return true;
  }
  /* three or four terms: edge keywords with optional offsets */
  span hk = {NULL, 0}, vk = {NULL, 0};
  span ho = {NULL, 0}, vo = {NULL, 0};
  bool center_seen = false;
  for (size_t i = 0; i < n; i++) {
    const axis_kind k = axis_of(t[i]);
    if (k == AX_NONE) {
      return false;
    }
    span off = {NULL, 0};
    if (k != AX_C && i + 1 < n && axis_of(t[i + 1]) == AX_NONE) {
      if (is_ident(t[i + 1])) {
        return false;
      }
      off = t[i + 1];
      i++;
    }
    if (k == AX_H) {
      if (hk.p) {
        return false;
      }
      hk = t[i - (off.p ? 1 : 0)];
      ho = off;
    }
    else if (k == AX_V) {
      if (vk.p) {
        return false;
      }
      vk = t[i - (off.p ? 1 : 0)];
      vo = off;
    }
    else {
      if (center_seen) {
        return false;
      }
      center_seen = true;
    }
  }
  if (center_seen && hk.p && vk.p) {
    return false;
  }
  if (!hk.p) {
    sb_puts(h, "50%");
  }
  else if (!ho.p) {
    sb_puts(h, kw_percent(hk));
  }
  else if (eq(hk, "left")) {
    put_span(h, ho);
  }
  else {
    put_from_end(h, ho);
  }
  if (!vk.p) {
    sb_puts(v, "50%");
  }
  else if (!vo.p) {
    sb_puts(v, kw_percent(vk));
  }
  else if (eq(vk, "top")) {
    put_span(v, vo);
  }
  else {
    put_from_end(v, vo);
  }
  return true;
}

/// Write the position `t[0..n)` as `h v`; the terms as they were when they are not a position.
static void put_position(sb * out, const span * t, size_t n) {
  sb h = {out->a, NULL, 0, 0, false};
  sb v = {out->a, NULL, 0, 0, false};
  if (position_pair(&h, &v, t, n) && !h.fail && !v.fail) {
    sb_put(out, h.d, h.n);
    sb_putc(out, ' ');
    sb_put(out, v.d, v.n);
  }
  else {
    for (size_t i = 0; i < n; i++) {
      if (i) {
        sb_putc(out, ' ');
      }
      put_span(out, t[i]);
    }
  }
  out->fail = out->fail || h.fail || v.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, h.d);
  gtext_allocator_free((GTEXT_Allocator *)out->a, v.d);
}

/* ---- the functions ---------------------------------------------------------------------------------------------- */

static void put_tokens(sb * out, const span * t, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (i) {
      sb_putc(out, ' ');
    }
    put_span(out, t[i]);
  }
}

/// `round` and what follows it, the radii of rect(), xywh() and inset(): the index of `round` among the terms, or n.
static size_t round_at(const span * t, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (eq(t[i], "round")) {
      return i;
    }
  }
  return n;
}

static void put_round(sb * out, const span * t, size_t n, size_t r) {
  if (r < n) {
    sb_puts(out, " round ");
    put_tokens(out, t + r + 1, n - r - 1);
  }
}

static bool fn_rect(sb * out, span inner, bool xywh) {
  spv tk = {out->a, NULL, 0, 0, false};
  split_top(&tk, inner, ' ');
  const size_t r = tk.fail ? 0 : round_at(tk.v, tk.n);
  bool done = false;
  if (!tk.fail && r == 4) {
    sb_puts(out, "inset(");
    if (xywh) {
      /* inset(y calc(100% - x - w) calc(100% - y - h) x) */
      const span x = tk.v[0], y = tk.v[1], w = tk.v[2], h = tk.v[3];
      put_span(out, y);
      sb_putc(out, ' ');
      put_from_end2(out, x, w);
      sb_putc(out, ' ');
      put_from_end2(out, y, h);
      sb_putc(out, ' ');
      put_span(out, x);
    }
    else {
      /* inset(t calc(100% - r) calc(100% - b) l); an auto edge is the edge of the reference box */
      for (unsigned k = 0; k < 4; k++) {
        if (k) {
          sb_putc(out, ' ');
        }
        if (eq(tk.v[k], "auto")) {
          sb_puts(out, "0%");
        }
        else if (k == 1 || k == 2) {
          put_from_end(out, tk.v[k]);
        }
        else {
          put_span(out, tk.v[k]);
        }
      }
    }
    put_round(out, tk.v, tk.n, r);
    sb_putc(out, ')');
    done = true;
  }
  out->fail = out->fail || tk.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, tk.v);
  return done;
}

static bool is_extent(span s) {
  return eq(s, "closest-side") || eq(s, "farthest-side") || eq(s, "closest-corner") || eq(s, "farthest-corner");
}

static bool fn_radial(sb * out, span inner, const char * name) {
  spv tk = {out->a, NULL, 0, 0, false};
  split_top(&tk, inner, ' ');
  bool done = false;
  if (!tk.fail) {
    size_t at = tk.n;
    for (size_t i = 0; i < tk.n; i++) {
      if (eq(tk.v[i], "at")) {
        at = i;
        break;
      }
    }
    bool all_default = at > 0;
    for (size_t i = 0; i < at; i++) {
      all_default = all_default && eq(tk.v[i], "closest-side");
    }
    sb_puts(out, name);
    sb_putc(out, '(');
    size_t nrad = all_default ? 0 : at;
    put_tokens(out, tk.v, nrad);
    if (at < tk.n) {
      if (nrad) {
        sb_putc(out, ' ');
      }
      sb_puts(out, "at ");
      put_position(out, tk.v + at + 1, tk.n - at - 1);
    }
    sb_putc(out, ')');
    done = true;
  }
  (void)is_extent;
  out->fail = out->fail || tk.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, tk.v);
  return done;
}

/// ray( <angle> && <ray-size>? && contain? && [ at <position> ]? ): in that order, the default size and no default else written, the position in percentages.
static bool fn_ray(sb * out, span inner) {
  spv tk = {out->a, NULL, 0, 0, false};
  split_top(&tk, inner, ' ');
  bool done = false;
  if (!tk.fail) {
    span angle = {NULL, 0}, size = {NULL, 0};
    bool contain = false;
    size_t at = tk.n;
    for (size_t i = 0; i < tk.n; i++) {
      if (eq(tk.v[i], "at")) {
        at = i;
        break;
      }
      if (eq(tk.v[i], "contain")) {
        contain = true;
      }
      else if (eq(tk.v[i], "sides") || is_extent(tk.v[i])) {
        size = tk.v[i];
      }
      else {
        angle = tk.v[i];
      }
    }
    if (angle.p) {
      sb_puts(out, "ray(");
      put_span(out, angle);
      if (size.p && !eq(size, "closest-side")) {
        sb_putc(out, ' ');
        put_span(out, size);
      }
      if (contain) {
        sb_puts(out, " contain");
      }
      if (at < tk.n) {
        sb_puts(out, " at ");
        put_position(out, tk.v + at + 1, tk.n - at - 1);
      }
      sb_putc(out, ')');
      done = true;
    }
  }
  out->fail = out->fail || tk.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, tk.v);
  return done;
}

/// The terms of `t[from..to)` that make a `to`, `by` or `with` operand: up to the next word that starts a part of the command.
static size_t operand_end(const span * t, size_t from, size_t to) {
  size_t i = from;
  while (i < to) {
    if (eq(t[i], "to") || eq(t[i], "by") || eq(t[i], "with") || eq(t[i], "of") || eq(t[i], "rotate") || eq(t[i], "cw") || eq(t[i], "ccw") ||
        eq(t[i], "large") || eq(t[i], "small") || eq(t[i], "from") || eq(t[i], "/")) {
      break;
    }
    i++;
  }
  return i;
}

/// One command of shape(): `to` positions are percentages, `hline to`/`vline to` keywords too, an arc's defaults are not written.
static void shape_command(sb * out, span cmd) {
  spv t = {out->a, NULL, 0, 0, false};
  split_top(&t, cmd, ' ');
  if (t.fail || t.n == 0) {
    out->fail = out->fail || t.fail;
    gtext_allocator_free((GTEXT_Allocator *)out->a, t.v);
    put_span(out, cmd);
    return;
  }
  const bool arc = eq(t.v[0], "arc");
  for (size_t i = 0; i < t.n;) {
    const span w = t.v[i];
    if (i) {
      /* decided below, per term */
    }
    if (arc && (eq(w, "small") || eq(w, "ccw"))) {
      i++;
      continue;
    }
    if (arc && eq(w, "rotate") && i + 1 < t.n && eq(t.v[i + 1], "0deg")) {
      i += 2;
      continue;
    }
    if (i) {
      sb_putc(out, ' ');
    }
    put_span(out, w);
    i++;
    if ((eq(w, "to") || eq(w, "with") || eq(w, "/")) && i < t.n) {
      const size_t e = operand_end(t.v, i, t.n);
      const bool line_axis = eq(t.v[0], "hline") || eq(t.v[0], "vline");
      if (line_axis) {
        /* hline to left|center|right and vline to top|center|bottom: a percentage */
        if (e == i + 1 && axis_of(t.v[i]) != AX_NONE) {
          sb_putc(out, ' ');
          sb_puts(out, kw_percent(t.v[i]));
        }
        else {
          sb_putc(out, ' ');
          put_tokens(out, t.v + i, e - i);
        }
      }
      else if (e > i && e - i <= 4 && (axis_of(t.v[i]) != AX_NONE || (e - i >= 2 && axis_of(t.v[e - 1]) != AX_NONE))) {
        sb_putc(out, ' ');
        put_position(out, t.v + i, e - i);
      }
      else {
        sb_putc(out, ' ');
        put_tokens(out, t.v + i, e - i);
      }
      i = e;
    }
  }
  out->fail = out->fail || t.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, t.v);
}

static bool fn_shape(sb * out, span inner) {
  spv args = {out->a, NULL, 0, 0, false};
  split_top(&args, inner, ',');
  bool done = false;
  if (!args.fail && args.n >= 2) {
    spv head = {out->a, NULL, 0, 0, false};
    split_top(&head, args.v[0], ' ');
    size_t from = head.n;
    for (size_t i = 0; i < head.n; i++) {
      if (eq(head.v[i], "from")) {
        from = i;
        break;
      }
    }
    if (!head.fail && from < head.n) {
      sb_puts(out, "shape(");
      bool wrote = false;
      for (size_t i = 0; i < from; i++) {
        if (eq(head.v[i], "nonzero")) {
          continue;
        }
        if (wrote) {
          sb_putc(out, ' ');
        }
        put_span(out, head.v[i]);
        wrote = true;
      }
      if (wrote) {
        sb_putc(out, ' ');
      }
      sb_puts(out, "from ");
      put_position(out, head.v + from + 1, head.n - from - 1);
      for (size_t i = 1; i < args.n; i++) {
        sb_puts(out, ", ");
        shape_command(out, args.v[i]);
      }
      sb_putc(out, ')');
      done = true;
    }
    out->fail = out->fail || head.fail;
    gtext_allocator_free((GTEXT_Allocator *)out->a, head.v);
  }
  out->fail = out->fail || args.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, args.v);
  return done;
}


/* ---- gradients (Images 4) ---------------------------------------------------------------------------------------- */

/// Whether a token is a length, a percentage, an angle or a calculation (not a color): what can be a color stop's position or a hint.
static bool is_quantity(span s) {
  if (!s.n) {
    return false;
  }
  const char c = s.p[0];
  return (c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' || starts_fn(s, "calc") || starts_fn(s, "min") || starts_fn(s, "max") || starts_fn(s, "clamp");
}

/// Whether the first argument of a gradient is its configuration (shape, size, position, direction, interpolation) and not a color stop.
static bool is_gradient_config(span arg) {
  size_t i = 0;
  while (i < arg.n && !is_ws(arg.p[i])) {
    i++;
  }
  const span first = {arg.p, i};
  if (is_quantity(first)) {
    return true;
  }
  return eq(first, "circle") || eq(first, "ellipse") || eq(first, "at") || eq(first, "from") || eq(first, "in") || eq(first, "to") || is_extent(first);
}

/// A color stop, or two when it has two positions (Images 4: `<color> <length-percentage>{2}` is two stops).
static void put_stop(sb * out, span arg) {
  spv t = {out->a, NULL, 0, 0, false};
  split_top(&t, arg, ' ');
  if (!t.fail && t.n == 3 && is_quantity(t.v[1]) && is_quantity(t.v[2]) && !is_quantity(t.v[0])) {
    put_span(out, t.v[0]);
    sb_putc(out, ' ');
    put_span(out, t.v[1]);
    sb_puts(out, ", ");
    put_span(out, t.v[0]);
    sb_putc(out, ' ');
    put_span(out, t.v[2]);
  }
  else {
    put_span(out, arg);
  }
  out->fail = out->fail || t.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, t.v);
}

/// The configuration of a radial or conic gradient: a shape or size that is implied by the other is not written, nor a center position, nor `from 0deg`.
static void gradient_config(sb * out, span cfg, bool conic) {
  spv t = {out->a, NULL, 0, 0, false};
  split_top(&t, cfg, ' ');
  size_t at = t.n, in = t.n;
  for (size_t i = 0; i < t.n; i++) {
    if (at == t.n && eq(t.v[i], "at")) {
      at = i;
    }
    if (in == t.n && eq(t.v[i], "in")) {
      in = i;
    }
  }
  const size_t head_end = at < in ? at : in;
  sb parts = {out->a, NULL, 0, 0, false};
  if (conic) {
    /* `from <angle>` */
    if (head_end >= 2 && eq(t.v[0], "from") && !eq(t.v[1], "0deg")) {
      sb_puts(&parts, "from ");
      put_span(&parts, t.v[1]);
    }
  }
  else {
    span shape = {NULL, 0};
    size_t first_size = 0;
    if (head_end > 0 && (eq(t.v[0], "circle") || eq(t.v[0], "ellipse"))) {
      shape = t.v[0];
      first_size = 1;
    }
    const size_t nsize = head_end - first_size;
    bool lengths = nsize > 0;
    for (size_t i = first_size; i < head_end; i++) {
      lengths = lengths && is_quantity(t.v[i]);
    }
    const bool default_size = nsize == 1 && eq(t.v[first_size], "farthest-corner");
    /* a circle of one length and an ellipse of two are what the lengths say */
    const bool shape_implied = lengths && ((eq(shape, "circle") && nsize == 1) || (eq(shape, "ellipse") && nsize == 2));
    if (shape.p && !shape_implied) {
      put_span(&parts, shape);
    }
    if (nsize && !default_size) {
      if (parts.n) {
        sb_putc(&parts, ' ');
      }
      put_tokens(&parts, t.v + first_size, nsize);
    }
  }
  if (at < t.n) {
    const size_t pend = in > at ? in : t.n;
    sb pos = {out->a, NULL, 0, 0, false};
    put_position(&pos, t.v + at + 1, pend - at - 1);
    if (!(pos.n == 7 && memcmp(pos.d, "50% 50%", 7) == 0)) {
      if (parts.n) {
        sb_putc(&parts, ' ');
      }
      sb_puts(&parts, "at ");
      sb_put(&parts, pos.d, pos.n);
    }
    parts.fail = parts.fail || pos.fail;
    gtext_allocator_free((GTEXT_Allocator *)out->a, pos.d);
  }
  if (in < t.n) {
    if (parts.n) {
      sb_putc(&parts, ' ');
    }
    put_tokens(&parts, t.v + in, t.n - in);
  }
  sb_put(out, parts.d, parts.n);
  out->fail = out->fail || t.fail || parts.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, parts.d);
  gtext_allocator_free((GTEXT_Allocator *)out->a, t.v);
}

static bool fn_gradient(sb * out, const char * name, span inner, int kind) {
  spv args = {out->a, NULL, 0, 0, false};
  split_top(&args, inner, ',');
  bool done = false;
  if (!args.fail && args.n >= 2) {
    sb body = {out->a, NULL, 0, 0, false};
    size_t first_stop = 0;
    if (is_gradient_config(args.v[0])) {
      first_stop = 1;
      if (kind == 0) {
        put_span(&body, args.v[0]);
      }
      else {
        gradient_config(&body, args.v[0], kind == 2);
      }
    }
    sb_puts(out, name);
    sb_putc(out, '(');
    bool wrote = body.n > 0;
    sb_put(out, body.d, body.n);
    for (size_t i = first_stop; i < args.n; i++) {
      if (wrote) {
        sb_puts(out, ", ");
      }
      put_stop(out, args.v[i]);
      wrote = true;
    }
    sb_putc(out, ')');
    out->fail = out->fail || body.fail;
    gtext_allocator_free((GTEXT_Allocator *)out->a, body.d);
    done = true;
  }
  out->fail = out->fail || args.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, args.v);
  return done;
}

/// One shape function rewritten; false (nothing written) when it is not one of the shape functions or does not read.
static bool shape_function(sb * out, span f) {
  static const struct {
    const char * name;
    int kind;
  } F[] = {{"rect", 1}, {"xywh", 2}, {"circle", 3}, {"ellipse", 4}, {"ray", 5}, {"shape", 7}, {"linear-gradient", 10}, {"repeating-linear-gradient", 10},
      {"radial-gradient", 11}, {"repeating-radial-gradient", 11}, {"conic-gradient", 12}, {"repeating-conic-gradient", 12}};
  for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
    if (!starts_fn(f, F[i].name)) {
      continue;
    }
    const span inner = fn_inner(f, strlen(F[i].name));
    switch (F[i].kind) {
    case 1: return fn_rect(out, inner, false);
    case 2: return fn_rect(out, inner, true);
    case 3: return fn_radial(out, inner, "circle");
    case 4: return fn_radial(out, inner, "ellipse");
    case 5: return fn_ray(out, inner);
    case 10: return fn_gradient(out, F[i].name, inner, 0);
    case 11: return fn_gradient(out, F[i].name, inner, 1);
    case 12: return fn_gradient(out, F[i].name, inner, 2);
    default: return fn_shape(out, inner);
    }
  }
  return false;
}

static bool is_shape_fn(span t) {
  static const char * const N[] = {"inset", "rect", "xywh", "circle", "ellipse", "polygon", "path", "shape", "ray"};
  for (size_t i = 0; i < sizeof N / sizeof N[0]; i++) {
    if (starts_fn(t, N[i])) {
      return true;
    }
  }
  return false;
}

/* ---- the property's value ---------------------------------------------------------------------------------------- */

typedef enum { SHAPE_PROP_OTHER, SHAPE_PROP_OUTSIDE, SHAPE_PROP_BORDER_SHAPE } shape_prop;

static bool box_is(span s, const char * w) {
  return eq(s, w);
}

/// The text of one comma-free part of a shape property's value rewritten: every shape function by the rules above, and a reference box that is the property's default not written
/// when a shape is with it. `*changed` says whether the text differs.
static bool rewrite_item(sb * out, span text, shape_prop prop, bool * changed) {
  spv tk = {out->a, NULL, 0, 0, false};
  split_top(&tk, text, ' ');
  if (tk.fail) {
    out->fail = true;
    gtext_allocator_free((GTEXT_Allocator *)out->a, tk.v);
    return false;
  }
  size_t shapes = 0;
  for (size_t i = 0; i < tk.n; i++) {
    shapes += is_shape_fn(tk.v[i]) ? 1 : 0;
  }
  bool first = true;
  size_t seen = 0;
  for (size_t i = 0; i < tk.n; i++) {
    sb piece = {out->a, NULL, 0, 0, false};
    bool wrote_piece = false;
    if (is_shape_fn(tk.v[i])) {
      seen++;
    }
    if (memchr(tk.v[i].p, '(', tk.v[i].n)) {
      if (shape_function(&piece, tk.v[i]) && !piece.fail) {
        wrote_piece = true;
      }
    }
    else if (shapes > 0 && is_ident(tk.v[i])) {
      /* a reference box next to a shape */
      bool drop = false;
      if (prop == SHAPE_PROP_OUTSIDE) {
        drop = box_is(tk.v[i], "margin-box");
      }
      else if (prop == SHAPE_PROP_BORDER_SHAPE) {
        if (shapes == 2) {
          drop = seen == 1 ? box_is(tk.v[i], "border-box") : box_is(tk.v[i], "padding-box");
        }
      }
      else {
        drop = box_is(tk.v[i], "border-box");
      }
      if (drop) {
        *changed = true;
        out->fail = out->fail || piece.fail;
        gtext_allocator_free((GTEXT_Allocator *)out->a, piece.d);
        continue;
      }
    }
    if (!first) {
      sb_putc(out, ' ');
    }
    first = false;
    if (wrote_piece) {
      if (piece.n != tk.v[i].n || memcmp(piece.d, tk.v[i].p, piece.n) != 0) {
        *changed = true;
      }
      sb_put(out, piece.d, piece.n);
    }
    else {
      put_span(out, tk.v[i]);
    }
    out->fail = out->fail || piece.fail;
    gtext_allocator_free((GTEXT_Allocator *)out->a, piece.d);
  }
  gtext_allocator_free((GTEXT_Allocator *)out->a, tk.v);
  return !out->fail;
}


/// The whole value: each of its comma-separated parts (image lists, `content`) rewritten.
static bool rewrite_value(sb * out, span text, shape_prop prop, bool * changed) {
  spv parts = {out->a, NULL, 0, 0, false};
  split_top(&parts, text, ',');
  if (parts.fail) {
    out->fail = true;
    gtext_allocator_free((GTEXT_Allocator *)out->a, parts.v);
    return false;
  }
  for (size_t i = 0; i < parts.n; i++) {
    if (i) {
      sb_puts(out, ", ");
    }
    rewrite_item(out, parts.v[i], prop, changed);
  }
  gtext_allocator_free((GTEXT_Allocator *)out->a, parts.v);
  return !out->fail;
}

/// inset() with equal offsets written as the shortest form of the 1 to 4 (Shapes 1, 3.3: default components "do not show up in the serialization"; CSSOM: the
/// shortest form that reads back as the same value).
static void collapse_inset(sb * out, span text, bool * changed) {
  spv tk = {out->a, NULL, 0, 0, false};
  split_top(&tk, text, ' ');
  for (size_t i = 0; i < tk.n && !tk.fail; i++) {
    if (i) {
      sb_putc(out, ' ');
    }
    if (!starts_fn(tk.v[i], "inset")) {
      put_span(out, tk.v[i]);
      continue;
    }
    spv a = {out->a, NULL, 0, 0, false};
    split_top(&a, fn_inner(tk.v[i], 5), ' ');
    const size_t r = a.fail ? 0 : round_at(a.v, a.n);
    if (a.fail || r != 4) {
      put_span(out, tk.v[i]);
      out->fail = out->fail || a.fail;
      gtext_allocator_free((GTEXT_Allocator *)out->a, a.v);
      continue;
    }
    #define SAME(x, y) (a.v[x].n == a.v[y].n && memcmp(a.v[x].p, a.v[y].p, a.v[x].n) == 0)
    size_t keep = 4;
    if (SAME(1, 3)) {
      keep = 3;
      if (SAME(0, 2)) {
        keep = 2;
        if (SAME(0, 1)) {
          keep = 1;
        }
      }
    }
    #undef SAME
    if (keep != 4) {
      *changed = true;
    }
    sb_puts(out, "inset(");
    put_tokens(out, a.v, keep);
    put_round(out, a.v, a.n, r);
    sb_putc(out, ')');
    gtext_allocator_free((GTEXT_Allocator *)out->a, a.v);
  }
  out->fail = out->fail || tk.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, tk.v);
}

/* ---- the algorithm ---------------------------------------------------------------------------------------------- */

/// Read `text` as a value of the property and compute it by its types. `*out` is NULL when the text is not a value of the property.
static GTEXT_CSS_Status reread(css_state * st, GTEXT_CSS_Property_Id id, const char * text, size_t length, bool compute, GTEXT_CSS_Value_Set ** out) {
  *out = NULL;
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = &st->cc->alloc;
  GTEXT_CSS_Property_Value pv;
  memset(&pv, 0, sizeof pv);
  const GTEXT_CSS_Status ps = gtext_css_parse_property_text(st->cc->registry, st->cc->meta[id - 1].name, st->cc->meta[id - 1].name_length, text, length, &po, &pv);
  if (ps != GTEXT_CSS_OK) {
    gtext_css_property_value_free(&pv);
    return ps == GTEXT_CSS_E_OOM ? css_st_fail(st, ps, NULL, NULL) : GTEXT_CSS_OK;
  }
  if (pv.outcome != GTEXT_CSS_PROP_RESULT_VALID || !gtext_css_value_set_root(pv.set)) {
    gtext_css_property_value_free(&pv);
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Status s = GTEXT_CSS_OK;
  if (compute) {
    s = css_cv_by_type(st, pv.set, id, out);
  }
  else {
    *out = gtext_css_value_set_retain(pv.set);
  }
  gtext_css_property_value_free(&pv);
  return s;
}

GTEXT_CSS_Status css_algo_shape(css_state * st, GTEXT_CSS_Property_Id id, const css_pmeta * m, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  GTEXT_CSS_Value_Set * bt = NULL;
  GTEXT_CSS_Status s = css_cv_by_type(st, src, id, &bt);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(bt);
  if (!root || root->kind == GTEXT_CSS_VAL_KEYWORD) {
    *out = bt;
    return GTEXT_CSS_OK;
  }
  shape_prop prop = SHAPE_PROP_OTHER;
  if (m->name_length == 13 && memcmp(m->name, "shape-outside", 13) == 0) {
    prop = SHAPE_PROP_OUTSIDE;
  }
  else if (m->name_length == 12 && memcmp(m->name, "border-shape", 12) == 0) {
    prop = SHAPE_PROP_BORDER_SHAPE;
  }
  GTEXT_Allocator alloc = st->cc->alloc;
  GTEXT_CSS_Serialize_Options so;
  memset(&so, 0, sizeof so);
  so.allocator = &alloc;
  char * text = NULL;
  size_t text_len = 0;
  s = gtext_css_serialize_value_buffer(root, &so, &text, &text_len);
  if (s != GTEXT_CSS_OK) {
    gtext_css_value_set_free(bt);
    return css_st_fail(st, s, NULL, NULL);
  }
  /* pass one: the functions and the boxes */
  sb one = {&alloc, NULL, 0, 0, false};
  bool changed = false;
  rewrite_value(&one, (span){text, text_len}, prop, &changed);
  gtext_css_serialize_buffer_free(&so, text);
  if (one.fail) {
    gtext_allocator_free(&alloc, one.d);
    gtext_css_value_set_free(bt);
    return css_st_fail(st, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  if (!changed) {
    gtext_allocator_free(&alloc, one.d);
    *out = bt;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value_Set * p1 = NULL;
  s = reread(st, id, one.d, one.n, true, &p1);
  gtext_allocator_free(&alloc, one.d);
  if (s != GTEXT_CSS_OK || !p1) {
    gtext_css_value_set_free(bt);
    return s;
  }
  /* pass two: equal offsets of an inset() */
  char * t2 = NULL;
  size_t t2n = 0;
  s = gtext_css_serialize_value_buffer(gtext_css_value_set_root(p1), &so, &t2, &t2n);
  if (s != GTEXT_CSS_OK) {
    gtext_css_value_set_free(p1);
    gtext_css_value_set_free(bt);
    return css_st_fail(st, s, NULL, NULL);
  }
  sb two = {&alloc, NULL, 0, 0, false};
  bool changed2 = false;
  collapse_inset(&two, (span){t2, t2n}, &changed2);
  gtext_css_serialize_buffer_free(&so, t2);
  if (two.fail) {
    gtext_allocator_free(&alloc, two.d);
    gtext_css_value_set_free(p1);
    gtext_css_value_set_free(bt);
    return css_st_fail(st, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  GTEXT_CSS_Value_Set * p2 = NULL;
  if (changed2) {
    s = reread(st, id, two.d, two.n, false, &p2);
  }
  gtext_allocator_free(&alloc, two.d);
  if (s != GTEXT_CSS_OK) {
    gtext_css_value_set_free(p1);
    gtext_css_value_set_free(bt);
    return s;
  }
  gtext_css_value_set_free(bt);
  if (p2) {
    gtext_css_value_set_free(p1);
    *out = p2;
  }
  else {
    *out = p1;
  }
  return GTEXT_CSS_OK;
}
