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
 * The same text rewrite also writes the mixes and the easing functions the way their drafts serialize them: `dynamic-range-limit-mix()` (Color HDR 1, 3.3: the
 * percentages normalized to sum to 100%, in the order standard, constrained, no-limit, a limit at 0% left out), `palette-mix()` (Values 5, 6.1: omitted percentages share
 * what the written ones leave; the default method and equal shares are not written; percentages that are all written stay as written), `steps()`/`step-start`/`step-end`
 * (Easing 2, 2.3.1) and `linear()` (2.1.1: two positions are two points, positions ascending, a missing position not filled in).
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

static void put_number(sb * b, double v) {
  char buf[400];
  const size_t n = css_cssom_number(v, false, buf);
  sb_put(b, buf, n);
}


/* ---- filter functions (Filter Effects 1) --------------------------------------------------------------------------- */

/// A plain number or percentage token as a value (a percentage is a hundredth of its number); false for anything else. Read here, in IEEE arithmetic and no host function.
static bool amount_value(span s, double * v) {
  const char * p = s.p;
  const char * end = s.p + s.n;
  bool pct = false;
  if (end > p && end[-1] == '%') {
    pct = true;
    end--;
  }
  bool neg = false;
  if (p < end && (*p == '+' || *p == '-')) {
    neg = *p == '-';
    p++;
  }
  double m = 0.0;
  int scale = 0;
  size_t digits = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    if (digits < 17) {
      m = m * 10.0 + (double)(*p - '0');
    }
    else {
      scale++;
    }
    digits++;
    p++;
  }
  if (p < end && *p == '.') {
    p++;
    while (p < end && *p >= '0' && *p <= '9') {
      if (digits < 17) {
        m = m * 10.0 + (double)(*p - '0');
        scale--;
      }
      digits++;
      p++;
    }
  }
  if (digits == 0 || p != end || scale > 300 || scale < -300) {
    return false;
  }
  while (scale > 0) {
    m *= 10.0;
    scale--;
  }
  while (scale < 0) {
    m /= 10.0;
    scale++;
  }
  if (pct) {
    m /= 100.0;
  }
  *v = neg ? -m : m;
  return true;
}

/// Filter Effects 1, 6.2: "Omitted values are included and compute to their defaults." (6.1: the default of grayscale(), sepia() and invert() is 1, of blur()
/// 0px; "Values of amount over 100% are allowed but UAs must clamp the values to 1" for grayscale(), sepia(), invert() and opacity(); "Negative values are not
/// allowed"); a percentage is the number it is a hundredth of (6.4), which is how the pinned pages write a computed amount.
static bool fn_filter(sb * out, const char * name, span inner, int kind) {
  spv t = {out->a, NULL, 0, 0, false};
  split_top(&t, inner, ' ');
  bool done = false;
  if (!t.fail && t.n <= 1) {
    if (kind == 0) { /* blur */
      if (t.n == 0) {
        sb_puts(out, "blur(0px)");
        done = true;
      }
    }
    else if (kind == 1) { /* hue-rotate */
      if (t.n == 0) {
        sb_puts(out, "hue-rotate(0deg)");
        done = true;
      }
    }
    else {
      double v = 1.0;
      if (t.n == 0 || amount_value(t.v[0], &v)) {
        if (v < 0.0) {
          v = 0.0;
        }
        if (kind == 3 && v > 1.0) { /* grayscale, invert, opacity, sepia */
          v = 1.0;
        }
        sb_puts(out, name);
        sb_putc(out, '(');
        put_number(out, v);
        sb_putc(out, ')');
        done = true;
      }
    }
  }
  out->fail = out->fail || t.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, t.v);
  return done;
}


/// drop-shadow( <color>? && <length>{2,3} ): "Omitted values are included and compute to their defaults" - the color is currentcolor (which a computed value keeps, Color 4
/// 15.5, as the keyword), the blur radius 0px; the color is written first.
static bool fn_drop_shadow(sb * out, span inner) {
  spv t = {out->a, NULL, 0, 0, false};
  split_top(&t, inner, ' ');
  bool done = false;
  if (!t.fail && t.n >= 2 && t.n <= 4) {
    size_t lengths = 0;
    size_t color = t.n;
    for (size_t i = 0; i < t.n; i++) {
      if (is_quantity(t.v[i])) {
        lengths++;
      }
      else {
        color = color == t.n ? i : t.n + 1;
      }
    }
    if (color <= t.n && lengths >= 2 && lengths <= 3 && lengths + (color < t.n ? 1 : 0) == t.n) {
      sb_puts(out, "drop-shadow(");
      if (color < t.n) {
        put_span(out, t.v[color]);
      }
      else {
        sb_puts(out, "currentcolor");
      }
      for (size_t i = 0; i < t.n; i++) {
        if (i != color) {
          sb_putc(out, ' ');
          put_span(out, t.v[i]);
        }
      }
      if (lengths == 2) {
        sb_puts(out, " 0px");
      }
      sb_putc(out, ')');
      done = true;
    }
  }
  out->fail = out->fail || t.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, t.v);
  return done;
}

/// One shape function rewritten; false (nothing written) when it is not one of the shape functions or does not read.
static bool shape_function(sb * out, span f) {
  static const struct {
    const char * name;
    int kind;
  } F[] = {{"rect", 1}, {"xywh", 2}, {"circle", 3}, {"ellipse", 4}, {"ray", 5}, {"shape", 7}, {"linear-gradient", 10}, {"repeating-linear-gradient", 10},
      {"radial-gradient", 11}, {"repeating-radial-gradient", 11}, {"conic-gradient", 12}, {"repeating-conic-gradient", 12},
      {"blur", 20}, {"hue-rotate", 21}, {"brightness", 22}, {"contrast", 22}, {"saturate", 22}, {"grayscale", 23}, {"invert", 23}, {"opacity", 23}, {"sepia", 23}, {"drop-shadow", 24}};
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
    case 20: return fn_filter(out, F[i].name, inner, 0);
    case 21: return fn_filter(out, F[i].name, inner, 1);
    case 22: return fn_filter(out, F[i].name, inner, 2);
    case 23: return fn_filter(out, F[i].name, inner, 3);
    case 24: return fn_drop_shadow(out, inner);
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

typedef enum { SHAPE_PROP_OTHER, SHAPE_PROP_OUTSIDE, SHAPE_PROP_BORDER_SHAPE, SHAPE_PROP_PALETTE, SHAPE_PROP_RANGE_LIMIT, SHAPE_PROP_EASING, SHAPE_PROP_SETTINGS, SHAPE_PROP_AREAS, SHAPE_PROP_ROTATE } shape_prop;

/* ---- mixes: dynamic-range-limit-mix() and palette-mix() ------------------------------------------------------------------ */

/// One mix function of the text. The nodes are in the order of the text, so a function comes before the functions inside it.
typedef struct {
  bool palette;        ///< palette-mix() (else dynamic-range-limit-mix()).
  span method;         ///< palette-mix(): the interpolation method as written (`in srgb`), or empty.
  size_t first, last;  ///< Its arguments, a list through `next`; MIX_NONE when there are none.
  size_t count;
  double w[3];         ///< dynamic-range-limit-mix(): the weights of standard, constrained and no-limit (percent), once computed.
  bool omit_pct;       ///< palette-mix(): the percentages are equal shares of the whole and are not written.
} mix_node;

/// One argument: a value (a word, or a mix function) and its percentage.
typedef struct {
  span word;           ///< The value, when `child` is MIX_NONE.
  size_t child;        ///< The mix function that is the value, or MIX_NONE.
  double pct;
  bool has_pct;
  bool has_value;
  size_t next;
} mix_item;

#define MIX_NONE ((size_t)-1)

typedef struct {
  const GTEXT_Allocator * a;
  mix_node * nodes;
  size_t nn, capn;
  mix_item * items;
  size_t ni, capi;
  bool fail;           ///< Out of memory.
} mix_state;

static void mix_free(mix_state * m) {
  gtext_allocator_free((GTEXT_Allocator *)m->a, m->nodes);
  gtext_allocator_free((GTEXT_Allocator *)m->a, m->items);
}

static size_t mix_new_node(mix_state * m, bool palette) {
  if (m->nn == m->capn) {
    const size_t cap = m->capn ? m->capn * 2 : 8;
    mix_node * g = gtext_allocator_realloc((GTEXT_Allocator *)m->a, m->nodes, cap * sizeof *g);
    if (!g) {
      m->fail = true;
      return MIX_NONE;
    }
    m->nodes = g;
    m->capn = cap;
  }
  mix_node * n = &m->nodes[m->nn];
  memset(n, 0, sizeof *n);
  n->palette = palette;
  n->first = n->last = MIX_NONE;
  return m->nn++;
}

static void mix_add_item(mix_state * m, size_t node, const mix_item * it) {
  if (m->ni == m->capi) {
    const size_t cap = m->capi ? m->capi * 2 : 8;
    mix_item * g = gtext_allocator_realloc((GTEXT_Allocator *)m->a, m->items, cap * sizeof *g);
    if (!g) {
      m->fail = true;
      return;
    }
    m->items = g;
    m->capi = cap;
  }
  m->items[m->ni] = *it;
  m->items[m->ni].next = MIX_NONE;
  mix_node * n = &m->nodes[node];
  if (n->last == MIX_NONE) {
    n->first = m->ni;
  }
  else {
    m->items[n->last].next = m->ni;
  }
  n->last = m->ni;
  n->count++;
  m->ni++;
}

static bool is_word_end(char c) {
  return is_ws(c) || c == ',' || c == '(' || c == ')';
}

/// The argument being read of each open function: a stack that grows with the nesting, in the one array with the nodes' stack.
typedef struct {
  size_t node;
  mix_item cur;
} mix_frame;

/// Read the mix function in `text` into nodes and items. False when the text is not exactly one such function (it is then left as it is). No recursion: the open
/// functions are a stack, and the work is linear in the text.
static bool mix_read(mix_state * m, span text) {
  static const struct {
    const char * name;
    bool palette;
  } fns[] = {{"palette-mix", true}, {"dynamic-range-limit-mix", false}};
  mix_frame * fr = NULL;
  size_t nf = 0, capf = 0;
  bool root_seen = false, ok = true;
  size_t pos = 0;
  const char * t = text.p;
  const size_t n = text.n;
  while (ok) {
    while (pos < n && is_ws(t[pos])) {
      pos++;
    }
    if (pos >= n) {
      ok = root_seen && nf == 0;
      break;
    }
    if (nf == 0 && root_seen) {
      ok = false;
      break;
    }
    const char c = t[pos];
    if (c == ',' || c == ')') {
      if (nf == 0) {
        ok = false;
        break;
      }
      mix_frame * f = &fr[nf - 1];
      if (f->cur.has_value || f->cur.has_pct) {
        mix_add_item(m, f->node, &f->cur);
      }
      memset(&f->cur, 0, sizeof f->cur);
      f->cur.child = MIX_NONE;
      pos++;
      if (c == ')') {
        nf--;
      }
      ok = !m->fail;
      continue;
    }
    const size_t start = pos;
    while (pos < n && !is_word_end(t[pos])) {
      pos++;
    }
    span word = {t + start, pos - start};
    if (word.n == 0) {
      ok = false;
      break;
    }
    if (pos < n && t[pos] == '(') {
      int which = -1;
      for (size_t i = 0; i < 2; i++) {
        if (eq(word, fns[i].name)) {
          which = (int)i;
        }
      }
      if (which >= 0 && (nf || !root_seen)) {
        const size_t node = mix_new_node(m, fns[which].palette);
        if (node == MIX_NONE) {
          ok = false;
          break;
        }
        if (nf == capf) {
          const size_t cap = capf ? capf * 2 : 8;
          mix_frame * g = gtext_allocator_realloc((GTEXT_Allocator *)m->a, fr, cap * sizeof *g);
          if (!g) {
            m->fail = true;
            ok = false;
            break;
          }
          fr = g;
          capf = cap;
        }
        if (nf) {
          if (fr[nf - 1].cur.has_value) {
            ok = false;
            break;
          }
          fr[nf - 1].cur.child = node;
          fr[nf - 1].cur.has_value = true;
        }
        else {
          root_seen = true;
        }
        fr[nf].node = node;
        memset(&fr[nf].cur, 0, sizeof fr[nf].cur);
        fr[nf].cur.child = MIX_NONE;
        nf++;
        pos++;
        continue;
      }
      /* another function (a calculation that stayed): its text, balanced, is the word */
      size_t depth = 0;
      while (pos < n) {
        if (t[pos] == '(') {
          depth++;
        }
        else if (t[pos] == ')' && --depth == 0) {
          pos++;
          break;
        }
        pos++;
      }
      if (depth != 0) {
        ok = false;
        break;
      }
      word.n = pos - start;
    }
    if (nf == 0) {
      ok = false;
      break;
    }
    mix_frame * f = &fr[nf - 1];
    mix_node * node = &m->nodes[f->node];
    if (word.p[word.n - 1] == '%') {
      double v;
      /* amount_value reads a number (here without its percent sign) and nothing else */
      if (!amount_value((span){word.p, word.n - 1}, &v) || f->cur.has_pct) {
        ok = false;
        break;
      }
      f->cur.pct = v;
      f->cur.has_pct = true;
    }
    else if (node->palette && node->count == 0 && !f->cur.has_value && eq(word, "in")) {
      while (pos < n && t[pos] != ',') {
        pos++;
      }
      size_t e = pos;
      while (e > start && is_ws(t[e - 1])) {
        e--;
      }
      node->method = (span){t + start, e - start};
    }
    else if (f->cur.has_value) {
      ok = false;
      break;
    }
    else {
      f->cur.word = word;
      f->cur.has_value = true;
    }
  }
  gtext_allocator_free((GTEXT_Allocator *)m->a, fr);
  return ok && !m->fail && m->nn > 0;
}

/// Normalizing Mix Percentages (Values 5, 6.1): "For each omitted percentage in items, set it to (100% - specified sum) / (number of omitted percentages)", the specified
/// sum clamped to 100%. The scaling of a total over 100% is not applied to the written value: Values 5, 6.2 makes the computed value "the mix notation itself, with its
/// arguments computed individually" where it cannot be calculated, and a palette is not calculated here. A mix whose percentages are all written is left as written.
static void mix_normalize(mix_state * m, mix_node * n) {
  double specified = 0.0;
  size_t omitted = 0;
  for (size_t i = n->first; i != MIX_NONE; i = m->items[i].next) {
    if (m->items[i].has_pct) {
      specified += m->items[i].pct;
    }
    else {
      omitted++;
    }
  }
  if (specified > 100.0) {
    specified = 100.0;
  }
  double total = 0.0;
  for (size_t i = n->first; i != MIX_NONE; i = m->items[i].next) {
    if (!m->items[i].has_pct) {
      m->items[i].pct = (100.0 - specified) / (double)omitted;
      m->items[i].has_pct = true;
    }
    total += m->items[i].pct;
  }
  /* equal shares of the whole: the percentages are not written */
  bool equal = total > 99.999999 && total < 100.000001 && n->count > 0;
  const double share = n->count ? 100.0 / (double)n->count : 0.0;
  for (size_t i = n->first; equal && i != MIX_NONE; i = m->items[i].next) {
    const double d = m->items[i].pct - share;
    equal = d > -0.000001 && d < 0.000001;
  }
  n->omit_pct = equal;
}

static bool drl_weights(span word, double w[3]) {
  w[0] = w[1] = w[2] = 0.0;
  if (eq(word, "standard")) {
    w[0] = 100.0;
  }
  else if (eq(word, "constrained")) {
    w[1] = 100.0;
  }
  else if (eq(word, "no-limit")) {
    w[2] = 100.0;
  }
  else {
    return false;
  }
  return true;
}

/// Color HDR 1, 3.3: "Let p1, ..., pN be the mixing percentages, normalized to sum to 100%", and the percentage of each limit is the sum of the arguments' own,
/// weighted by those. A function comes before the ones inside it in `nodes`, so the last node is done first.
static bool drl_compute(mix_state * m) {
  for (size_t k = m->nn; k-- > 0;) {
    mix_node * n = &m->nodes[k];
    double sum = 0.0;
    for (size_t i = n->first; i != MIX_NONE; i = m->items[i].next) {
      if (!m->items[i].has_pct) {
        return false;
      }
      sum += m->items[i].pct;
    }
    n->w[0] = n->w[1] = n->w[2] = 0.0;
    if (sum <= 0.0) {
      continue;
    }
    for (size_t i = n->first; i != MIX_NONE; i = m->items[i].next) {
      double cw[3];
      if (m->items[i].child != MIX_NONE) {
        memcpy(cw, m->nodes[m->items[i].child].w, sizeof cw);
      }
      else if (!m->items[i].has_value || !drl_weights(m->items[i].word, cw)) {
        return false;
      }
      for (int j = 0; j < 3; j++) {
        n->w[j] += cw[j] * (m->items[i].pct / sum);
      }
    }
  }
  return true;
}

static void put_percent(sb * b, double v) {
  put_number(b, v);
  sb_putc(b, '%');
}

/// "...the computed value is dynamic-range-limit-mix(), with parameters standard, constrained, and no-limit, in that order, and percentages p_standard,
/// p_constrained_high, and p_no_limit, omitting parameters with a percentage equal to 0%. If all parameters are thereby omitted, the computed value is the initial value."
static void drl_write(const mix_node * n, sb * out) {
  static const char * const names[3] = {"standard", "constrained", "no-limit"};
  for (int j = 0; j < 3; j++) {
    if (n->w[j] > 99.9999995) {
      sb_puts(out, names[j]);
      return;
    }
  }
  bool any = false;
  for (int j = 0; j < 3; j++) {
    any = any || n->w[j] >= 0.0000005;
  }
  if (!any) {
    sb_puts(out, "no-limit");
    return;
  }
  size_t written = 0;
  sb_puts(out, "dynamic-range-limit-mix(");
  for (int j = 0; j < 3; j++) {
    if (n->w[j] >= 0.0000005) {
      if (written++) {
        sb_puts(out, ", ");
      }
      sb_puts(out, names[j]);
      sb_putc(out, ' ');
      put_percent(out, n->w[j]);
    }
  }
  sb_putc(out, ')');
}

static bool method_is_default(span method) {
  /* Color 5: oklab is the default interpolation space, and is not written */
  return method.n == 0 || eq(method, "in oklab");
}

/// The palette-mix() as the text writes it: the interpolation method unless it is the default, then each argument with its normalized percentage (none when the
/// shares are equal). Arguments that are mixes are written in place, with an explicit stack.
static void palette_write(mix_state * m, sb * out) {
  typedef struct {
    size_t node;
    size_t next;     ///< The next argument to write.
    size_t parent;   ///< The argument of the enclosing function that this function is the value of, or MIX_NONE.
  } pframe;
  pframe * st = NULL;
  size_t sp = 0, cap = 0;
  for (size_t k = 0; k < m->nn; k++) {
    mix_normalize(m, &m->nodes[k]);
  }
  size_t node = 0;
  size_t parent = MIX_NONE;
  for (;;) {
    /* open `node` */
    if (sp == cap) {
      cap = cap ? cap * 2 : 8;
      pframe * g = gtext_allocator_realloc((GTEXT_Allocator *)m->a, st, cap * sizeof *g);
      if (!g) {
        out->fail = true;
        break;
      }
      st = g;
    }
    st[sp].node = node;
    st[sp].next = m->nodes[node].first;
    st[sp].parent = parent;
    sp++;
    sb_puts(out, "palette-mix(");
    if (!method_is_default(m->nodes[node].method)) {
      put_span(out, m->nodes[node].method);
      sb_puts(out, ", ");
    }
    /* write arguments until one opens a function or the function ends */
    bool opened = false;
    while (sp && !opened) {
      pframe * f = &st[sp - 1];
      if (f->next == MIX_NONE) {
        sb_putc(out, ')');
        const size_t done_parent = f->parent;
        sp--;
        if (done_parent != MIX_NONE && sp && !m->nodes[st[sp - 1].node].omit_pct) {
          sb_putc(out, ' ');
          put_percent(out, m->items[done_parent].pct);
        }
        continue;
      }
      const size_t i = f->next;
      const mix_item * it = &m->items[i];
      f->next = it->next;
      if (i != m->nodes[f->node].first) {
        sb_puts(out, ", ");
      }
      if (it->child != MIX_NONE) {
        node = it->child;
        parent = i;
        opened = true;
        break;
      }
      put_span(out, it->word);
      if (!m->nodes[f->node].omit_pct) {
        sb_putc(out, ' ');
        put_percent(out, it->pct);
      }
    }
    if (!opened) {
      break;
    }
  }
  gtext_allocator_free((GTEXT_Allocator *)m->a, st);
}

/// A dynamic-range-limit or font-palette whose value is a mix, rewritten; `*changed` says whether the text differs. Anything else is left as it is.
static bool rewrite_mix(sb * out, span text, bool palette, bool * changed) {
  mix_state m;
  memset(&m, 0, sizeof m);
  m.a = out->a;
  const bool read = mix_read(&m, text);
  if (m.fail) {
    out->fail = true;
  }
  if (!read || (m.nodes[0].palette != palette)) {
    mix_free(&m);
    put_span(out, text);
    return !out->fail;
  }
  sb w = {out->a, NULL, 0, 0, false};
  if (palette) {
    palette_write(&m, &w);
  }
  else if (drl_compute(&m)) {
    drl_write(&m.nodes[0], &w);
  }
  mix_free(&m);
  if (w.fail) {
    out->fail = true;
  }
  if (w.n == 0 || w.fail) {
    put_span(out, text);
  }
  else {
    if (w.n != text.n || memcmp(w.d, text.p, w.n) != 0) {
      *changed = true;
    }
    sb_put(out, w.d, w.n);
  }
  gtext_allocator_free((GTEXT_Allocator *)out->a, w.d);
  return !out->fail;
}

/* ---- easing functions (Easing 2) ---------------------------------------------------------------------------------------- */

/// A control point's input progress as a plain percentage (`50%`); false for anything else (a calculation that stayed, a number).
static bool plain_percent(span s, double * v) {
  return s.n > 1 && s.p[s.n - 1] == '%' && amount_value((span){s.p, s.n - 1}, v);
}

/// One control point of a linear(): its output progress and its input progress, when it has a plain percentage.
typedef struct {
  span out;
  span pos;
  bool has;   ///< an input progress was written
  bool plain; ///< and it is a plain percentage
  double v;
} lin_point;

/// Easing 2, 2.1.1: "control points originally specified with two input progress values are turned into two separate control points, and the input progress values
/// are in strictly ascending order" (`linear(0 20%, 0.5 10%, 1)` serializes as `linear(0 20%, 0.5 20%, 1)`). The text then gives the missing input progress values
/// ("If the first control point lacks an input progress value, set it to 0", the last to the greater of 100% and the largest so far, and a run in between evenly
/// between the neighbors that have one) as the steps that evaluate the function; it does not say whether a computed value shows them. Both pinned browsers write
/// them (`linear(0, 1)` is `linear(0 0%, 1 100%)`: tools/conformance/css_browser_answers.jsonl, cases linear.*), so this does too (the standing rule: where the text
/// is silent, the recorded answers of both browsers decide). A function with one control point, or with a position that is not a plain percentage (a calculation
/// that stayed), keeps the positions as written.
static void rewrite_linear(sb * out, span inner) {
  spv args = {out->a, NULL, 0, 0, false};
  split_top(&args, inner, ',');
  lin_point * pts = NULL;
  size_t n = 0;
  bool plain_all = !args.fail;
  if (!args.fail) {
    pts = (lin_point *)gtext_allocator_realloc((GTEXT_Allocator *)out->a, NULL, (args.n * 2 + 1) * sizeof *pts);
    if (!pts) {
      out->fail = true;
      plain_all = false;
    }
  }
  for (size_t i = 0; plain_all && i < args.n; i++) {
    spv t = {out->a, NULL, 0, 0, false};
    split_top(&t, args.v[i], ' ');
    if (t.fail) {
      out->fail = true;
      plain_all = false;
    }
    else if (t.n == 0 || t.n > 3) {
      plain_all = false;
    }
    else {
      const size_t points = t.n >= 3 ? 2u : 1u;
      for (size_t k = 0; k < points; k++) {
        lin_point * q = &pts[n++];
        q->out = t.v[0];
        q->has = t.n >= 2;
        q->pos = q->has ? t.v[1 + k] : (span){NULL, 0};
        q->plain = q->has && plain_percent(q->pos, &q->v);
        if (q->has && !q->plain) {
          plain_all = false;
        }
      }
    }
    gtext_allocator_free((GTEXT_Allocator *)out->a, t.v);
  }
  if (plain_all && n >= 2) {
    double run = 0.0;
    bool have_run = false;
    for (size_t i = 0; i < n; i++) {
      if (pts[i].has) {
        if (have_run && pts[i].v < run) {
          pts[i].v = run;
        }
        run = pts[i].v;
        have_run = true;
      }
    }
    if (!pts[0].has) {
      pts[0].has = true;
      pts[0].v = 0.0;
    }
    if (!pts[n - 1].has) {
      pts[n - 1].has = true;
      pts[n - 1].v = run > 100.0 ? run : 100.0;
    }
    /* the first and the last may have been raised above a neighbor that was written lower: ascending again */
    double top = pts[0].v;
    for (size_t i = 1; i < n; i++) {
      if (pts[i].has && pts[i].v < top) {
        pts[i].v = top;
      }
      if (pts[i].has) {
        top = pts[i].v;
      }
    }
    for (size_t i = 1; i < n; i++) {
      if (pts[i].has) {
        continue;
      }
      size_t j = i;
      while (!pts[j].has) {
        j++;
      }
      const double lo = pts[i - 1].v;
      const double hi = pts[j].v;
      for (size_t k = i; k < j; k++) {
        pts[k].has = true;
        pts[k].v = lo + (hi - lo) * (double)(k - (i - 1)) / (double)(j - (i - 1));
      }
      i = j;
    }
    for (size_t i = 0; i < n; i++) {
      if (i) {
        sb_puts(out, ", ");
      }
      put_span(out, pts[i].out);
      sb_putc(out, ' ');
      put_percent(out, pts[i].v);
    }
  }
  else if (!args.fail && !out->fail) {
    /* positions as written, but ascending where they are plain percentages */
    double run = 0.0;
    bool have_run = false;
    bool first = true;
    for (size_t i = 0; i < args.n; i++) {
      spv t = {out->a, NULL, 0, 0, false};
      split_top(&t, args.v[i], ' ');
      if (t.fail) {
        out->fail = true;
        gtext_allocator_free((GTEXT_Allocator *)out->a, t.v);
        break;
      }
      const size_t points = t.n >= 3 ? 2u : 1u;
      for (size_t k = 0; k < points && t.n > 0; k++) {
        if (!first) {
          sb_puts(out, ", ");
        }
        first = false;
        put_span(out, t.v[0]);
        if (t.n >= 2) {
          const span pos = t.v[1 + k];
          double v;
          sb_putc(out, ' ');
          if (plain_percent(pos, &v)) {
            if (have_run && v < run) {
              v = run;
            }
            run = v;
            have_run = true;
            put_percent(out, v);
          }
          else {
            put_span(out, pos);
          }
        }
      }
      if (t.n == 0) {
        if (!first) {
          sb_puts(out, ", ");
        }
        first = false;
      }
      gtext_allocator_free((GTEXT_Allocator *)out->a, t.v);
    }
  }
  if (pts) {
    gtext_allocator_free((GTEXT_Allocator *)out->a, pts);
  }
  out->fail = out->fail || args.fail;
  gtext_allocator_free((GTEXT_Allocator *)out->a, args.v);
}

/// Easing 2, 2.3.1: "step-start and step-end do not serialize as themselves. Instead, they serialize as steps(1, start) and steps(1)", and a steps() with the step position
/// "end or jump-end" is written without it. 2.3: "If the <step-position> is jump-none, the <integer> must be at least 2 ... Otherwise, the <integer> must be at least 1",
/// so a calculation that computes to less is the least the function is valid with (Values 4, 10.7: a calculation is clamped to the range of its slot).
static void rewrite_easing_item(sb * out, span item) {
  if (eq(item, "step-start")) {
    sb_puts(out, "steps(1, start)");
    return;
  }
  if (eq(item, "step-end")) {
    sb_puts(out, "steps(1)");
    return;
  }
  if (starts_fn(item, "steps")) {
    spv a = {out->a, NULL, 0, 0, false};
    split_top(&a, fn_inner(item, 5), ',');
    double n;
    if (!a.fail && a.n >= 1 && a.n <= 2 && amount_value(a.v[0], &n) && a.v[0].p[a.v[0].n - 1] != '%' && n == (double)(long)n) {
      const double least = a.n == 2 && eq(a.v[1], "jump-none") ? 2.0 : 1.0;
      const bool end = a.n == 2 && (eq(a.v[1], "end") || eq(a.v[1], "jump-end"));
      sb_puts(out, "steps(");
      put_number(out, n < least ? least : n);
      if (a.n == 2 && !end) {
        sb_puts(out, ", ");
        put_span(out, a.v[1]);
      }
      sb_putc(out, ')');
      gtext_allocator_free((GTEXT_Allocator *)out->a, a.v);
      return;
    }
    out->fail = out->fail || a.fail;
    gtext_allocator_free((GTEXT_Allocator *)out->a, a.v);
  }
  else if (starts_fn(item, "linear")) {
    sb_puts(out, "linear(");
    rewrite_linear(out, fn_inner(item, 6));
    sb_putc(out, ')');
    return;
  }
  put_span(out, item);
}

static void rewrite_easing(sb * out, span text) {
  spv parts = {out->a, NULL, 0, 0, false};
  split_top(&parts, text, ',');
  if (parts.fail) {
    out->fail = true;
    gtext_allocator_free((GTEXT_Allocator *)out->a, parts.v);
    return;
  }
  for (size_t i = 0; i < parts.n; i++) {
    if (i) {
      sb_puts(out, ", ");
    }
    rewrite_easing_item(out, parts.v[i]);
  }
  gtext_allocator_free((GTEXT_Allocator *)out->a, parts.v);
}


/* ---- the property's value ---------------------------------------------------------------------------------------- */


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


/* ---- lists and keywords the standard leaves to the browsers' common answer ------------------------------------------------ */

static int span_cmp(span x, span y) {
  const size_t m = x.n < y.n ? x.n : y.n;
  const int c = memcmp(x.p, y.p, m);
  if (c) {
    return c;
  }
  return x.n < y.n ? -1 : (x.n > y.n ? 1 : 0);
}

/// font-variation-settings: Fonts 4 gives the computed value as "the keyword normal or a list, each item a string paired with a number" and is silent on the order of the
/// items and on a tag written twice. Both pinned browsers write the items in ascending order of their tags and keep the last of a repeated tag
/// (tools/conformance/css_browser_answers.jsonl, cases fontvar.*), so this does too. `*changed` is set when the text differs.
static void rewrite_variation_settings(sb * out, span text) {
  spv parts = {out->a, NULL, 0, 0, false};
  split_top(&parts, text, ',');
  if (parts.fail || parts.n < 2) {
    out->fail = out->fail || parts.fail;
    put_span(out, text);
    gtext_allocator_free((GTEXT_Allocator *)out->a, parts.v);
    return;
  }
  /* each item is a quoted tag and a number: the tag is everything up to the closing quote */
  span * tag = (span *)gtext_allocator_realloc((GTEXT_Allocator *)out->a, NULL, parts.n * sizeof *tag);
  size_t * order = (size_t *)gtext_allocator_realloc((GTEXT_Allocator *)out->a, NULL, parts.n * sizeof *order);
  if (!tag || !order) {
    out->fail = true;
    gtext_allocator_free((GTEXT_Allocator *)out->a, tag);
    gtext_allocator_free((GTEXT_Allocator *)out->a, order);
    gtext_allocator_free((GTEXT_Allocator *)out->a, parts.v);
    return;
  }
  bool ok = true;
  for (size_t i = 0; i < parts.n && ok; i++) {
    const span it = parts.v[i];
    size_t close = 0;
    if (it.n >= 2 && it.p[0] == '"') {
      for (close = 1; close < it.n && it.p[close] != '"'; close++) {
        if (it.p[close] == '\\') {
          ok = false;
        }
      }
    }
    if (close == 0 || close >= it.n) {
      ok = false;
    }
    else {
      tag[i] = (span){it.p, close + 1};
    }
    order[i] = i;
  }
  if (!ok) {
    put_span(out, text);
  }
  else {
    /* a stable insertion sort by tag; of equal tags the later item is the one kept */
    for (size_t i = 1; i < parts.n; i++) {
      const size_t k = order[i];
      size_t j = i;
      while (j > 0 && span_cmp(tag[order[j - 1]], tag[k]) > 0) {
        order[j] = order[j - 1];
        j--;
      }
      order[j] = k;
    }
    bool first = true;
    for (size_t i = 0; i < parts.n; i++) {
      if (i + 1 < parts.n && span_cmp(tag[order[i]], tag[order[i + 1]]) == 0) {
        continue;
      }
      if (!first) {
        sb_puts(out, ", ");
      }
      first = false;
      put_span(out, parts.v[order[i]]);
    }
  }
  gtext_allocator_free((GTEXT_Allocator *)out->a, tag);
  gtext_allocator_free((GTEXT_Allocator *)out->a, order);
  gtext_allocator_free((GTEXT_Allocator *)out->a, parts.v);
}

/// The length of the white space at `k` in a serialized string (a space, a tab or a line feed, or the escape `\9 ` the serializer writes for a tab), or 0.
static size_t ws_unit(span t, size_t k, size_t end) {
  if (t.p[k] == ' ' || t.p[k] == '\t' || t.p[k] == '\n') {
    return 1;
  }
  if (t.p[k] == '\\' && k + 2 < end && t.p[k + 1] == '9' && t.p[k + 2] == ' ') {
    return 3;
  }
  return 0;
}

/// grid-template-areas: Grid 1 gives the computed value as "the keyword none or a list of string values" and is silent on the white space inside a row. Both pinned
/// browsers write the cell names of a row separated by one space and a run of full stops as one (cases gta.*), so this does too.
static void rewrite_areas(sb * out, span text) {
  size_t i = 0;
  while (i < text.n) {
    if (text.p[i] != '"') {
      sb_putc(out, text.p[i++]);
      continue;
    }
    size_t j = i + 1;
    while (j < text.n && text.p[j] != '"') {
      j++;
    }
    if (j >= text.n) {
      put_span(out, (span){text.p + i, text.n - i});
      return;
    }
    sb_putc(out, '"');
    bool first = true;
    size_t k = i + 1;
    while (k < j) {
      size_t u;
      while (k < j && (u = ws_unit(text, k, j)) > 0) {
        k += u;
      }
      size_t e = k;
      bool dots = true;
      while (e < j && ws_unit(text, e, j) == 0) {
        dots = dots && text.p[e] == '.';
        e++;
      }
      if (e > k) {
        if (!first) {
          sb_putc(out, ' ');
        }
        first = false;
        if (dots) {
          sb_putc(out, '.');
        }
        else {
          put_span(out, (span){text.p + k, e - k});
        }
      }
      k = e;
    }
    sb_putc(out, '"');
    i = j + 1;
  }
}

/// offset-rotate: Motion 1 gives the computed value as "computed <angle> value, optionally preceded by auto" and defines `reverse` as `auto 180deg` ("equivalent to
/// auto 180deg"); both pinned browsers write `auto 180deg` and `auto 190deg` for `reverse 10deg` (cases angle-other.*).
static void rewrite_rotate(sb * out, span text) {
  if (eq(text, "reverse")) {
    sb_puts(out, "auto 180deg");
    return;
  }
  if (text.n > 8 && memcmp(text.p, "reverse ", 8) == 0 && text.n > 11 && memcmp(text.p + text.n - 3, "deg", 3) == 0) {
    double a;
    if (amount_value((span){text.p + 8, text.n - 8 - 3}, &a)) {
      sb_puts(out, "auto ");
      put_number(out, a + 180.0);
      sb_puts(out, "deg");
      return;
    }
  }
  put_span(out, text);
}

/// The whole value: each of its comma-separated parts (image lists, `content`) rewritten.
static bool rewrite_value(sb * out, span text, shape_prop prop, bool * changed) {
  if (prop == SHAPE_PROP_SETTINGS || prop == SHAPE_PROP_AREAS || prop == SHAPE_PROP_ROTATE) {
    sb w = {out->a, NULL, 0, 0, false};
    if (prop == SHAPE_PROP_SETTINGS) {
      rewrite_variation_settings(&w, text);
    }
    else if (prop == SHAPE_PROP_AREAS) {
      rewrite_areas(&w, text);
    }
    else {
      rewrite_rotate(&w, text);
    }
    if (w.fail) {
      out->fail = true;
    }
    else if (w.n != text.n || memcmp(w.d, text.p, w.n) != 0) {
      *changed = true;
    }
    if (w.fail || w.n == 0) {
      put_span(out, text);
    }
    else {
      sb_put(out, w.d, w.n);
    }
    gtext_allocator_free((GTEXT_Allocator *)out->a, w.d);
    return !out->fail;
  }
  if (prop == SHAPE_PROP_PALETTE || prop == SHAPE_PROP_RANGE_LIMIT) {
    return rewrite_mix(out, text, prop == SHAPE_PROP_PALETTE, changed);
  }
  if (prop == SHAPE_PROP_EASING) {
    sb w = {out->a, NULL, 0, 0, false};
    rewrite_easing(&w, text);
    if (w.fail) {
      out->fail = true;
    }
    else if (w.n != text.n || memcmp(w.d, text.p, w.n) != 0) {
      *changed = true;
    }
    if (w.fail || w.n == 0) {
      put_span(out, text);
    }
    else {
      sb_put(out, w.d, w.n);
    }
    gtext_allocator_free((GTEXT_Allocator *)out->a, w.d);
    return !out->fail;
  }
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

static bool span_same(span x, span y) {
  return x.n == y.n && memcmp(x.p, y.p, x.n) == 0;
}

/// The shortest form of four values given in top, right, bottom, left order (or the corners of a radius list): 4 values to 3 when the second and fourth are the same, to 2
/// when the first and third are too, to 1 when all are. Returns the count.
static size_t shortest4(const span * v) {
  size_t keep = 4;
  if (span_same(v[1], v[3])) {
    keep = 3;
    if (span_same(v[0], v[2])) {
      keep = 2;
      if (span_same(v[0], v[1])) {
        keep = 1;
      }
    }
  }
  return keep;
}

/// The values of a border-radius-like list (1 to 4 of them) expanded to four: "a" is "a a a a", "a b" is "a b a b", "a b c" is "a b c b".
static bool expand4(const span * in, size_t n, span * out4) {
  if (n < 1 || n > 4) {
    return false;
  }
  static const size_t src[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
  for (size_t i = 0; i < 4; i++) {
    out4[i] = in[src[n - 1][i]];
  }
  return true;
}

/// The radii after `round`, written as the shortest form: both lists of a `/` pair expanded to the four corners, each shortened, and the second list left out when it
/// is the first. Both pinned browsers write `inset(0px round 10px)` for `inset(0 round 10px 10px 10px 10px)` and `round 3px` for `round 3px 3px / 3px 3px`
/// (tools/conformance/css_browser_answers.jsonl, cases radii.*). Returns false (and writes nothing) when the list is not made of 1 to 4 terms on each side.
static bool put_round_short(sb * out, const span * t, size_t n, size_t r, bool * changed) {
  if (r >= n) {
    return true;
  }
  size_t slash = n;
  for (size_t i = r + 1; i < n; i++) {
    if (t[i].n == 1 && t[i].p[0] == '/') {
      slash = i;
      break;
    }
  }
  span h[4], v[4];
  const size_t hn = slash - r - 1;
  if (!expand4(t + r + 1, hn, h)) {
    return false;
  }
  size_t vn = 0;
  bool same_lists = true;
  if (slash < n) {
    vn = n - slash - 1;
    if (!expand4(t + slash + 1, vn, v)) {
      return false;
    }
    for (size_t i = 0; i < 4; i++) {
      same_lists = same_lists && span_same(h[i], v[i]);
    }
  }
  const size_t hk = shortest4(h);
  sb_puts(out, " round ");
  put_tokens(out, h, hk);
  size_t vk = 0;
  if (slash < n && !same_lists) {
    vk = shortest4(v);
    sb_puts(out, " / ");
    put_tokens(out, v, vk);
  }
  const size_t written = hk + (vk ? vk + 1 : 0);
  if (written != n - r - 1) {
    *changed = true;
  }
  return true;
}

/// inset() with equal offsets and equal radii written as the shortest form (Shapes 1, 3.3: default components "do not show up in the serialization"; CSSOM: the
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
    if (a.fail || r < 1 || r > 4) {
      put_span(out, tk.v[i]);
      out->fail = out->fail || a.fail;
      gtext_allocator_free((GTEXT_Allocator *)out->a, a.v);
      continue;
    }
    span off[4];
    expand4(a.v, r, off);
    const size_t keep = shortest4(off);
    if (keep != r) {
      *changed = true;
    }
    sb tail = {out->a, NULL, 0, 0, false};
    bool tail_changed = false;
    const bool ok = put_round_short(&tail, a.v, a.n, r, &tail_changed);
    if (tail.fail) {
      out->fail = true;
    }
    sb_puts(out, "inset(");
    put_tokens(out, off, keep);
    if (ok) {
      sb_put(out, tail.d, tail.n);
      *changed = *changed || tail_changed;
    }
    else {
      put_round(out, a.v, a.n, r);
    }
    sb_putc(out, ')');
    gtext_allocator_free((GTEXT_Allocator *)out->a, tail.d);
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
  shape_prop prop = SHAPE_PROP_OTHER;
  if (m->name_length == 13 && memcmp(m->name, "shape-outside", 13) == 0) {
    prop = SHAPE_PROP_OUTSIDE;
  }
  else if (m->name_length == 12 && memcmp(m->name, "border-shape", 12) == 0) {
    prop = SHAPE_PROP_BORDER_SHAPE;
  }
  else if (m->name_length == 12 && memcmp(m->name, "font-palette", 12) == 0) {
    prop = SHAPE_PROP_PALETTE;
  }
  else if (m->name_length == 19 && memcmp(m->name, "dynamic-range-limit", 19) == 0) {
    prop = SHAPE_PROP_RANGE_LIMIT;
  }
  else if ((m->name_length == 25 && memcmp(m->name, "animation-timing-function", 25) == 0) ||
           (m->name_length == 26 && memcmp(m->name, "transition-timing-function", 26) == 0)) {
    prop = SHAPE_PROP_EASING;
  }
  else if (m->name_length == 23 && memcmp(m->name, "font-variation-settings", 23) == 0) {
    prop = SHAPE_PROP_SETTINGS;
  }
  else if (m->name_length == 19 && memcmp(m->name, "grid-template-areas", 19) == 0) {
    prop = SHAPE_PROP_AREAS;
  }
  else if (m->name_length == 13 && memcmp(m->name, "offset-rotate", 13) == 0) {
    prop = SHAPE_PROP_ROTATE;
  }
  /* a keyword is its own computed value, except step-start and step-end, which are written as steps() */
  if (!root || (root->kind == GTEXT_CSS_VAL_KEYWORD && prop != SHAPE_PROP_EASING && prop != SHAPE_PROP_ROTATE)) {
    *out = bt;
    return GTEXT_CSS_OK;
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
  GTEXT_CSS_Value_Set * p1 = NULL;
  if (!changed) {
    /* nothing to rewrite, but an inset() as written may still have equal offsets (pass two) */
    gtext_allocator_free(&alloc, one.d);
    p1 = gtext_css_value_set_retain(bt);
  }
  else {
    s = reread(st, id, one.d, one.n, true, &p1);
    gtext_allocator_free(&alloc, one.d);
    if (s != GTEXT_CSS_OK || !p1) {
      gtext_css_value_set_free(bt);
      return s;
    }
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
