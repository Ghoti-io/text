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
 * The runner of the scoring interpreter (tools/css/cssom_eval.py makes the programs, tools/conformance/css_cssom_script_suite.py scores them). A program is a page: the page's HTML,
 * parsed by the library's HTML parser into a Markup tree, and the ops that the page's script performed on it, in the order it performed them, every loop unrolled and every function
 * inlined. No JavaScript is in here: an op is one call of the library, on the document a consumer would build (the Markup binding, the style attribute, the rule index, the computed
 * style), so the gate scores the integration and not a test double.
 *
 * Input, tab separated with `\\`, `\t`, `\n`, `\r` and `\0` escaped: `P file quirks tests`, `H html`, then `O op field...` lines. Output: `R file index PASS`, `R file index FAIL
 * message`, or `R file index LAYOUT property` (the test read a property whose used value needs a layout the library was not given: it is neither passed nor failed), for each test,
 * and `E file message` for a program that could not be run as the page ran it.
 *
 * Values. A value is a type and a text: `s` string, `n` number, `b` boolean, `N` null, `u` undefined. An operand is `s:text`, `n:3`, `b:true`, `N:`, `u:` or `r:<register>`. An
 * assertion compares as `testharness.js` does (strict equality, the same type and the same value).
 *
 * Ops (elements are `eN`, style declarations `sN`, registers `rN`): elem E byid ID | sel ROOT SELECTOR | qsa ROOT SELECTOR INDEX | body | html | head | nav FROM
 * parentElement|firstElementChild|lastElementChild|nextElementSibling|previousElementSibling; gcs S E PSEUDO; style S E; sget R S PROP; sattr R S NAME; sprio R S PROP; sitem R S INDEX;
 * slen R S; scss R S; sset S PROP VALUE PRIORITY; sattrset S NAME VALUE; scsstext S VALUE; sremove R S PROP; setattr E NAME VALUE; getattr R E NAME; hasattr R E NAME; rmattr E NAME;
 * classadd|classremove E TOKEN...; classtoggle R E TOKEN; classhas R E TOKEN; cmp R eq|ne|leq|lne A B; not R A; assert_equals A B MSG; assert_not_equals A B MSG; assert_true A MSG;
 * assert_false A MSG; assert_in_array A B... MSG; assert_approx_equals A B EPS MSG; assert_greater_than|assert_less_than|assert_greater_than_equal|assert_less_than_equal A B MSG;
 * fail MSG; test INDEX NAME; cleanup (the ops after it run whether or not the test failed); end. An op on an element that is null (a lookup that found nothing) fails the test, as
 * the TypeError of the page does.
 */

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 40

/* ---- strings -------------------------------------------------------------------------------------------------------------------------------- */

static char * dupn(const char * s, size_t n) {
  char * d = malloc(n + 1);
  if (!d) {
    abort();
  }
  memcpy(d, s, n);
  d[n] = '\0';
  return d;
}

static char * dups(const char * s) {
  return dupn(s, strlen(s));
}

static char * fmt(const char * f, ...) __attribute__((format(printf, 1, 2)));
static char * fmt(const char * f, ...) {
  char buf[2048];
  __builtin_va_list ap;
  __builtin_va_start(ap, f);
  vsnprintf(buf, sizeof buf, f, ap);
  __builtin_va_end(ap);
  return dups(buf);
}

typedef struct {
  char * f[MAX_FIELDS];
  size_t l[MAX_FIELDS];
  size_t n;
} op;

static size_t unescape_into(char * s) {
  char * out = s;
  for (const char * p = s; *p; p++) {
    if (*p == '\\' && p[1]) {
      p++;
      *out++ = *p == 't' ? '\t' : *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == '0' ? '\0' : *p;
    }
    else {
      *out++ = *p;
    }
  }
  *out = '\0';
  return (size_t)(out - s);
}

static int parse_op(char * line, op * o) {
  o->n = 0;
  char * p = line;
  for (;;) {
    char * tab = strchr(p, '\t');
    if (o->n == MAX_FIELDS) {
      return 0;
    }
    o->f[o->n++] = p;
    if (!tab) {
      break;
    }
    *tab = '\0';
    p = tab + 1;
  }
  for (size_t i = 0; i < o->n; i++) {
    o->l[i] = unescape_into(o->f[i]);
  }
  return 1;
}

/* ---- values and registers -------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  char kind;   /* s n b N u */
  char * s;
  size_t n;
} value;

static value val(char kind, const char * s, size_t n) {
  value v;
  v.kind = kind;
  v.s = dupn(s ? s : "", n);
  v.n = n;
  return v;
}

static void value_free(value * v) {
  free(v->s);
  v->s = NULL;
}


typedef struct {
  char * name;
  value v;
} reg;

/* ---- the world ---------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  char * name;
  GTEXT_Markup_Node * node;   /* NULL: a lookup that found nothing */
} elem;

typedef struct {
  char * name;
  GTEXT_CSSOM_Decl * decl;
  int computed;
  GTEXT_Markup_Node * node;
  char * elem_name;   /* the element the declaration was made for, and the pseudo-element argument of getComputedStyle: to make it again when the document is parsed again */
  char * pseudo;
} style;

typedef struct {
  GTEXT_CSS_Registry * registry;
  GTEXT_CSS_Conditions * conditions;
  GTEXT_CSSOM_Document * doc;
  GTEXT_Markup_Document * markup;
  GTEXT_CSS_Markup_Adapter * adapter;
  GTEXT_CSS_Compute_Context * compute;   /* the environment of the runner: the system colors (NULL with CSSOM_RUNNER_UA=none, which also leaves out the user agent sheet) */
  GTEXT_CSS_Selector_Match_Options match_options;
  GTEXT_CSSOM_Markup_Binding * binding;
  GTEXT_CSS_Tree * ua_tree;
  GTEXT_CSS_Sheet * ua_sheet;
  GTEXT_CSSOM_Attribute_Binding attr;
  elem * elems;
  size_t nelems, capelems;
  style * styles;
  size_t nstyles, capstyles;
  reg * regs;
  size_t nregs, capregs;
} world;

/* The user agent's style sheet is the part of the HTML rendering section that the display and the body's margin need; the library has no user agent sheet of its own. */
static const char UA_SHEET[] =
    "html, address, blockquote, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing, main, p, plaintext, pre, search, xmp, article, aside, h1, h2, "
    "h3, h4, h5, h6, hgroup, nav, section, dir, dd, dl, dt, menu, ol, ul, details, summary, fieldset, optgroup, option { display: block }\n"
    "body { display: block; margin: 8px }\n"
    "li { display: list-item }\n"
    "table { display: table } caption { display: table-caption } colgroup { display: table-column-group } col { display: table-column } thead { display: table-header-group }\n"
    "tbody { display: table-row-group } tfoot { display: table-footer-group } tr { display: table-row } td, th { display: table-cell }\n"
    "[hidden], area, base, basefont, datalist, head, link, meta, noembed, noframes, param, rp, script, style, template, title { display: none }\n"
    "input, button, select, textarea { display: inline-block }\n";

/* The same question asked of a fuller sheet, and of none: a failure whose answer moves with the user agent sheet of this runner is a failure that may be the runner's (the gate lists it
 * apart from the library's defects). The fuller sheet is typed from the rendering section of the HTML Standard as far as the properties a computed read can see; it is an instrument for
 * that comparison, not a claim of conformance. Selected by the environment variable CSSOM_RUNNER_UA: `none` (no sheet and no system color palette), `full`; otherwise the sheet above. */
static const char UA_SHEET_FULL[] =
    "html, address, blockquote, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing, main, p, plaintext, pre, search, xmp, article, aside, h1, h2, "
    "h3, h4, h5, h6, hgroup, nav, section, dir, dd, dl, dt, menu, ol, ul, details, summary, fieldset, optgroup, option { display: block }\n"
    "body { display: block; margin: 8px }\n"
    "li { display: list-item }\n"
    "table { display: table } caption { display: table-caption } colgroup { display: table-column-group } col { display: table-column } thead { display: table-header-group }\n"
    "tbody { display: table-row-group } tfoot { display: table-footer-group } tr { display: table-row } td, th { display: table-cell }\n"
    "[hidden], area, base, basefont, datalist, head, link, meta, noembed, noframes, param, rp, script, style, template, title { display: none }\n"
    "input, button, select, textarea { display: inline-block }\n"
    "p, dl, multicol { margin-block: 1em } blockquote, figure { margin-block: 1em; margin-inline: 40px } center { text-align: center } hr { margin-block: 0.5em; margin-inline: auto; border-style: inset; border-width: 1px; color: gray }\n"
    "h1 { font-size: 2em; margin-block: 0.67em } h2 { font-size: 1.5em; margin-block: 0.83em } h3 { font-size: 1.17em; margin-block: 1em } h4 { margin-block: 1.33em } h5 { font-size: 0.83em; margin-block: 1.67em } h6 { font-size: 0.67em; margin-block: 2.33em }\n"
    "h1, h2, h3, h4, h5, h6, b, strong, th { font-weight: bold } i, cite, em, var, address, dfn { font-style: italic } dt { } dd { margin-inline-start: 40px }\n"
    "u, ins { text-decoration: underline } s, strike, del { text-decoration: line-through } a:any-link { text-decoration: underline } abbr[title], acronym[title] { text-decoration: underline dotted }\n"
    "pre, listing, xmp, plaintext { white-space: pre } textarea { white-space: pre-wrap } code, kbd, samp, tt, pre, listing, xmp, plaintext { font-family: monospace }\n"
    "small, sub, sup { font-size: smaller } big { font-size: larger } sub { vertical-align: sub } sup { vertical-align: super } mark { background-color: yellow; color: black }\n"
    "ul, ol, dir, menu { margin-block: 1em; padding-inline-start: 40px } ul, dir, menu { list-style-type: disc } ol { list-style-type: decimal }\n"
    "table { box-sizing: border-box; border-spacing: 2px; border-collapse: separate } td, th { padding: 1px } th { text-align: center } caption { text-align: center }\n"
    "fieldset { margin-inline: 2px; padding-block: 0.35em 0.625em; padding-inline: 0.75em; border: 2px groove } img { } svg:not(:root) { overflow: hidden }\n";

/* The system colors are the user's: a computed value keeps the keyword until the consumer's environment says what it is (css_element.h resolve_color). The runner is the consumer here, and
 * gives the sRGB value both browsers give each (the palette tools/oracle/css_resolved.h has, which the computed-value gate also supplies): CanvasText, FieldText and ButtonText are black
 * and Canvas, Field and ButtonFace white in the light scheme, and the reverse (white, and 18 for the grounds) in the dark one. Any other name is not answered. */
static bool runner_resolve_color(void * user, const GTEXT_CSS_Color * color, GTEXT_CSS_Color_Scheme scheme, double rgba[4]) {
  (void)user;
  if (!color || color->kind != GTEXT_CSS_COLOR_KEYWORD || (color->keyword != GTEXT_CSS_COLOR_KW_SYSTEM && color->keyword != GTEXT_CSS_COLOR_KW_DEPRECATED)) {
    return false;
  }
  char w[32];
  if (color->name.length >= sizeof w) {
    return false;
  }
  for (size_t i = 0; i < color->name.length; i++) {
    w[i] = (char)tolower((unsigned char)color->name.data[i]);
  }
  w[color->name.length] = '\0';
  const bool dark = scheme == GTEXT_CSS_SCHEME_DARK;
  if (strcmp(w, "canvastext") == 0 || strcmp(w, "fieldtext") == 0 || strcmp(w, "buttontext") == 0) {
    rgba[0] = rgba[1] = rgba[2] = dark ? 255 : 0;
    rgba[3] = 1;
    return true;
  }
  if (strcmp(w, "canvas") == 0 || strcmp(w, "field") == 0 || strcmp(w, "buttonface") == 0) {
    rgba[0] = rgba[1] = rgba[2] = dark ? 18 : 255;
    rgba[3] = 1;
    return true;
  }
  return false;
}

static int g_palette = 1;
static const char * g_ua_sheet = UA_SHEET;
static size_t g_ua_len = sizeof UA_SHEET - 1;

/* The conditions the environment of the compute context answers `if(media())` with, and the styles a query container is read from, are the world's: the context, the conditions object and
   the document are made in that order and each needs the next, so the callbacks reach them through the world. */
static GTEXT_CSS_Tristate runner_condition(void * user, GTEXT_CSS_Condition_Kind kind, const GTEXT_CSS_String * text, GTEXT_CSS_Element_Handle element) {
  (void)element;
  const world * w = user;
  return gtext_css_conditions_condition(w->conditions, kind, text);
}

static const GTEXT_CSS_Computed_Style * runner_container_style(void * user, GTEXT_CSS_Element_Handle element) {
  const world * w = user;
  GTEXT_CSS_Container_Interface ci;
  if (!w->doc || gtext_cssom_document_container_interface(w->doc, &ci) != GTEXT_CSS_OK) {
    return NULL;
  }
  return ci.computed_style(ci.user, element);
}

static int world_open(world * w, const char * html, size_t html_len) {
  memset(w, 0, sizeof *w);
  if (gtext_css_registry_new(NULL, &w->registry) != GTEXT_CSS_OK) {
    return 0;
  }
  GTEXT_CSS_Media_Environment env = gtext_css_media_environment_default(NULL);
  env.viewport.width = 1280;
  env.viewport.height = 800;
  env.device.width = 1280;
  env.device.height = 800;
  GTEXT_HTML_Result hr;
  memset(&hr, 0, sizeof hr);
  if (gtext_html_parse(html, html_len, NULL, &w->markup, &hr) != GTEXT_HTML_OK) {
    return 0;
  }
  gtext_html_result_free(&hr);
  if (gtext_css_markup_adapter_new(w->markup, NULL, &w->adapter) != GTEXT_CSS_OK) {
    return 0;
  }
  w->match_options = gtext_css_markup_match_options(w->adapter);
  GTEXT_CSSOM_Config c = gtext_cssom_config_default();
  c.registry = w->registry;
  c.element_interface = gtext_css_markup_interface(w->adapter);
  c.match_options = &w->match_options;
  c.tree_version = &gtext_cssom_markup_tree_version;
  c.tree_version_user = w->markup;
  {
    GTEXT_CSS_Environment cenv = gtext_css_environment_default();
    cenv.user = w;
    cenv.condition = runner_condition;
    if (g_palette) {
      cenv.resolve_color = runner_resolve_color;
    }
    GTEXT_CSS_Compute_Options cco = gtext_css_compute_options_default();
    cco.elements = c.element_interface;
    GTEXT_CSS_Compute_Result cres;
    memset(&cres, 0, sizeof cres);
    if (gtext_css_compute_context_new(w->registry, &cenv, &cco, &w->compute, &cres) != GTEXT_CSS_OK) {
      return 0;
    }
    c.compute_context = w->compute;
  }
  GTEXT_CSS_Container_Interface container;
  memset(&container, 0, sizeof container);
  container.size = sizeof container;
  container.user = w;
  container.computed_style = runner_container_style;
  container.compute_context = w->compute;
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.media = &env;
  co.element_interface = c.element_interface;
  co.container = &container;
  if (gtext_css_conditions_new(w->registry, &co, &w->conditions) != GTEXT_CSS_OK) {
    return 0;
  }
  c.conditions = w->conditions;
  if (gtext_cssom_document_new(&c, &w->doc) != GTEXT_CSS_OK) {
    return 0;
  }
  GTEXT_CSS_Result pr;
  memset(&pr, 0, sizeof pr);
  if (g_ua_len) {
    if (gtext_css_parse_stylesheet(g_ua_sheet, g_ua_len, NULL, &w->ua_tree, &pr) != GTEXT_CSS_OK ||
        gtext_css_sheet_compile(w->ua_tree, w->registry, NULL, &w->ua_sheet, NULL) != GTEXT_CSS_OK ||
        gtext_cssom_document_register_sheet(w->doc, GTEXT_CSS_ORIGIN_USER_AGENT, w->ua_sheet, NULL) != GTEXT_CSS_OK) {
      return 0;
    }
  }
  GTEXT_CSSOM_Markup_Options mo = gtext_cssom_markup_options_default();
  if (gtext_cssom_markup_binding_new(w->doc, gtext_cssom_document_scope(w->doc), w->markup, &mo, &w->binding) != GTEXT_CSS_OK ||
      gtext_cssom_markup_sync(w->binding, NULL) != GTEXT_CSS_OK) {
    return 0;
  }
  w->attr = gtext_cssom_markup_inline_binding();
  w->capelems = w->capstyles = w->capregs = 32;
  w->elems = calloc(w->capelems, sizeof *w->elems);
  w->styles = calloc(w->capstyles, sizeof *w->styles);
  w->regs = calloc(w->capregs, sizeof *w->regs);
  return 1;
}

static void world_close(world * w) {
  for (size_t i = 0; i < w->nstyles; i++) {
    gtext_cssom_decl_release(w->styles[i].decl);
    free(w->styles[i].name);
    free(w->styles[i].elem_name);
    free(w->styles[i].pseudo);
  }
  for (size_t i = 0; i < w->nelems; i++) {
    free(w->elems[i].name);
  }
  for (size_t i = 0; i < w->nregs; i++) {
    free(w->regs[i].name);
    value_free(&w->regs[i].v);
  }
  free(w->elems);
  free(w->styles);
  free(w->regs);
  gtext_cssom_markup_binding_free(w->binding);
  gtext_cssom_document_release(w->doc);
  gtext_css_compute_context_free(w->compute);
  gtext_css_sheet_free(w->ua_sheet);
  gtext_css_tree_free(w->ua_tree);
  gtext_css_conditions_free(w->conditions);
  gtext_css_markup_adapter_free(w->adapter);
  gtext_markup_document_free(w->markup);
  gtext_css_registry_free(w->registry);
  memset(w, 0, sizeof *w);
}

static elem * find_elem(world * w, const char * name) {
  for (size_t i = w->nelems; i-- > 0;) {
    if (strcmp(w->elems[i].name, name) == 0) {
      return &w->elems[i];
    }
  }
  return NULL;
}

static void put_elem(world * w, const char * name, GTEXT_Markup_Node * node) {
  elem * e = find_elem(w, name);
  if (!e) {
    if (w->nelems == w->capelems) {
      w->capelems *= 2;
      w->elems = realloc(w->elems, w->capelems * sizeof *w->elems);
    }
    e = &w->elems[w->nelems++];
    e->name = dups(name);
  }
  e->node = node;
}

static style * find_style(world * w, const char * name) {
  for (size_t i = w->nstyles; i-- > 0;) {
    if (strcmp(w->styles[i].name, name) == 0) {
      return &w->styles[i];
    }
  }
  return NULL;
}

static void put_style(world * w, const char * name, GTEXT_CSSOM_Decl * d, int computed, GTEXT_Markup_Node * node, const char * elem_name, const char * pseudo) {
  if (w->nstyles == w->capstyles) {
    w->capstyles *= 2;
    w->styles = realloc(w->styles, w->capstyles * sizeof *w->styles);
  }
  style * s = &w->styles[w->nstyles++];
  s->name = dups(name);
  s->decl = d;
  s->computed = computed;
  s->node = node;
  s->elem_name = dups(elem_name);
  s->pseudo = dups(pseudo);
}

static reg * find_reg(world * w, const char * name) {
  for (size_t i = w->nregs; i-- > 0;) {
    if (strcmp(w->regs[i].name, name) == 0) {
      return &w->regs[i];
    }
  }
  return NULL;
}

static void put_reg(world * w, const char * name, value v) {
  reg * r = find_reg(w, name);
  if (r) {
    value_free(&r->v);
    r->v = v;
    return;
  }
  if (w->nregs == w->capregs) {
    w->capregs *= 2;
    w->regs = realloc(w->regs, w->capregs * sizeof *w->regs);
  }
  w->regs[w->nregs].name = dups(name);
  w->regs[w->nregs].v = v;
  w->nregs++;
}

/* The operand of an op. The value is a copy the caller frees. */
static value operand(world * w, const char * f, size_t l, int * ok) {
  *ok = 1;
  if (l >= 2 && f[1] == ':') {
    switch (f[0]) {
    case 's':
      return val('s', f + 2, l - 2);
    case 'n':
      return val('n', f + 2, l - 2);
    case 'b':
      return val('b', f + 2, l - 2);
    case 'N':
      return val('N', "", 0);
    case 'u':
      return val('u', "", 0);
    case 'e': {
      /* an element: two lookups that found the same node are the same object; a lookup that found nothing is null */
      elem * e = find_elem(w, f + 2);
      if (e) {
        if (!e->node) {
          return val('N', "", 0);
        }
        char id[32];
        snprintf(id, sizeof id, "%p", (void *)e->node);
        return val('e', id, strlen(id));
      }
      break;
    }
    case 'r': {
      reg * r = find_reg(w, f + 2);
      if (r) {
        return val(r->v.kind, r->v.s, r->v.n);
      }
      break;
    }
    default:
      break;
    }
  }
  *ok = 0;
  return val('u', "", 0);
}

/* ---- the Markup tree -------------------------------------------------------------------------------------------------------------------- */

static int is_element(const GTEXT_Markup_Node * n) {
  return n && gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT;
}

static int local_is(const GTEXT_Markup_Node * n, const char * name) {
  const char * l = gtext_markup_element_local_name(n);
  return l && strcmp(l, name) == 0;
}

static int attr_get(const GTEXT_Markup_Node * n, const char * name, const char ** value, size_t * len) {
  GTEXT_Markup_Attribute a;
  if (!gtext_markup_element_get_attribute(n, NULL, name, &a)) {
    return 0;
  }
  *value = a.value;
  *len = a.value_length;
  return 1;
}

static GTEXT_Markup_Node * root_element(world * w) {
  GTEXT_Markup_Node * root = gtext_markup_document_root(w->markup);
  for (GTEXT_Markup_Node * n = gtext_markup_node_first_child(root); n; n = gtext_markup_node_next_sibling(n)) {
    if (is_element(n)) {
      return n;
    }
  }
  return NULL;
}

static GTEXT_Markup_Node * find_by_local(world * w, const char * name) {
  GTEXT_Markup_Node * root = gtext_markup_document_root(w->markup);
  for (GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, 0); n; n = gtext_markup_node_next_in_order(root, n, 0)) {
    if (is_element(n) && local_is(n, name)) {
      return n;
    }
  }
  return NULL;
}

static GTEXT_Markup_Node * find_by_id(world * w, const char * id, size_t len) {
  GTEXT_Markup_Node * root = gtext_markup_document_root(w->markup);
  for (GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, 0); n; n = gtext_markup_node_next_in_order(root, n, 0)) {
    const char * v;
    size_t vl;
    if (is_element(n) && attr_get(n, "id", &v, &vl) && vl == len && memcmp(v, id, len) == 0) {
      return n;
    }
  }
  return NULL;
}

static GTEXT_Markup_Node * element_sibling(GTEXT_Markup_Node * n, int forward) {
  for (GTEXT_Markup_Node * s = forward ? gtext_markup_node_next_sibling(n) : gtext_markup_node_previous_sibling(n); s;
       s = forward ? gtext_markup_node_next_sibling(s) : gtext_markup_node_previous_sibling(s)) {
    if (is_element(s)) {
      return s;
    }
  }
  return NULL;
}

/* ---- the declaration ops -------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  int failed;
  char * message;
  int layout;
  char * layout_property;
} testrun;

/* `backgroundColor` -> `background-color`; `cssFloat` -> `float`; `webkitX` -> `-webkit-x`. */
static char * idl_to_css(const char * s, size_t n) {
  char * out = malloc(n * 2 + 3);
  size_t o = 0;
  if (n == 8 && memcmp(s, "cssFloat", 8) == 0) {
    return dups("float");
  }
  if (memchr(s, '-', n)) {
    return dupn(s, n);
  }
  if ((n > 6 && memcmp(s, "webkit", 6) == 0 && isupper((unsigned char)s[6])) || (n > 3 && memcmp(s, "moz", 3) == 0 && isupper((unsigned char)s[3])) ||
      (n > 2 && memcmp(s, "ms", 2) == 0 && isupper((unsigned char)s[2]))) {
    out[o++] = '-';
  }
  for (size_t i = 0; i < n; i++) {
    if (isupper((unsigned char)s[i])) {
      out[o++] = '-';
      out[o++] = (char)tolower((unsigned char)s[i]);
    }
    else {
      out[o++] = s[i];
    }
  }
  out[o] = '\0';
  return out;
}

static void note_layout(testrun * t, style * s, const char * css_name) {
  if (!s->computed || t->layout) {
    return;
  }
  GTEXT_CSSOM_Origin origin = GTEXT_CSSOM_ORIGIN_COMPUTED;
  bool found = false;
  if (gtext_cssom_computed_origin(s->decl, css_name, strlen(css_name), &origin, &found) == GTEXT_CSS_OK && found && origin == GTEXT_CSSOM_ORIGIN_ABSENT_LAYOUT) {
    t->layout = 1;
    t->layout_property = dups(css_name);
  }
}

static value take_string(world * w, char * buf, size_t n) {
  value v = val('s', buf ? buf : "", buf ? n : 0);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(w->doc), buf);
  return v;
}

static GTEXT_CSSOM_Document * g_doc;   /* the document of the program being run, for the name of what a failed read was missing */

static char * status_failure(const char * what, GTEXT_CSS_Status st) {
  if (st == GTEXT_CSS_E_UNSUPPORTED && g_doc) {
    GTEXT_CSSOM_Computed_Failure f;
    memset(&f, 0, sizeof f);
    if (gtext_cssom_computed_failure(g_doc, &f) == GTEXT_CSS_OK && f.callback[0]) {
      return fmt("%s: unsupported, needs %s", what, f.callback);
    }
  }
  return fmt("%s: %s", what, gtext_css_status_string(st));
}

/* ---- the harness assertions --------------------------------------------------------------------------------------------------------------------- */

static void fmt_value(const value * v, char * out, size_t cap) {
  switch (v->kind) {
  case 's': {
    size_t o = 0;
    if (cap < 8) {
      return;
    }
    out[o++] = '"';
    for (size_t i = 0; i < v->n && o + 4 < cap; i++) {
      if (v->s[i] == '"' || v->s[i] == '\\') {
        out[o++] = '\\';
      }
      out[o++] = v->s[i];
    }
    out[o++] = '"';
    out[o] = '\0';
    break;
  }
  case 'N':
    snprintf(out, cap, "null");
    break;
  case 'u':
    snprintf(out, cap, "undefined");
    break;
  case 'e':
    snprintf(out, cap, "object \"[object Element]\"");
    break;
  default:
    snprintf(out, cap, "%s", v->s);
    break;
  }
}

/* signed: testharness.js's same_value() (assert_equals, assert_not_equals) tells +0 from -0; `===` does not */
static int same_as(const value * a, const value * b, int signed_zero) {
  if (a->kind != b->kind) {
    return 0;
  }
  if (a->kind == 'n') {
    const double x = strtod(a->s, NULL), y = strtod(b->s, NULL);
    if (signed_zero && x == 0 && y == 0) {
      return signbit(x) == signbit(y);
    }
    return x == y || (x != x && y != y);
  }
  if (a->kind == 'N' || a->kind == 'u') {
    return 1;
  }
  return a->n == b->n && memcmp(a->s, b->s, a->n) == 0;
}

static int same(const value * a, const value * b) {
  return same_as(a, b, 0);
}

static char * mismatch(const char * name, const char * msg, const value * expected, const value * got) {
  char e[400], g[400];
  fmt_value(expected, e, sizeof e);
  fmt_value(got, g, sizeof g);
  return fmt("%s: %s%sexpected %s but got %s", name, msg, *msg ? " " : "", e, g);
}

/* ---- running one op --------------------------------------------------------------------------------------------------------------------- */

/// Returns NULL, or the message of a failure of the test (an assertion, or the exception the page's script would have thrown).
static char * run_op(world * w, const op * o, testrun * t, int * program_error) {
  const char * name = o->f[0];
  *program_error = 0;
  int ok;
#define NEED(n_) do { if (o->n != (n_)) { *program_error = 1; return fmt("the op %s has %zu fields, not %d", name, o->n, (n_)); } } while (0)
#define ELEM(var, fld) elem * var = find_elem(w, o->f[fld]); if (!var) { *program_error = 1; return fmt("the op %s names the element %s, which the program did not make", name, o->f[fld]); } \
  if (!var->node) { return dups("TypeError: the element is null"); }
#define STYLE(var, fld) style * var = find_style(w, o->f[fld]); if (!var) { *program_error = 1; return fmt("the op %s names the style %s, which the program did not make", name, o->f[fld]); }

  if (strcmp(name, "elem") == 0) {
    if (o->n < 4) {
      *program_error = 1;
      return dups("elem has too few fields");
    }
    const char * how = o->f[2];
    GTEXT_Markup_Node * found = NULL;
    if (strcmp(how, "byid") == 0) {
      found = find_by_id(w, o->f[3], o->l[3]);
    }
    else if (strcmp(how, "body") == 0) {
      found = find_by_local(w, "body");
    }
    else if (strcmp(how, "head") == 0) {
      found = find_by_local(w, "head");
    }
    else if (strcmp(how, "html") == 0) {
      found = root_element(w);
    }
    else if (strcmp(how, "sel") == 0 || strcmp(how, "qsa") == 0) {
      const GTEXT_Markup_Node * scope = gtext_markup_document_root(w->markup);
      if (strcmp(o->f[3], "doc") != 0) {
        elem * r = find_elem(w, o->f[3]);
        if (!r) {
          *program_error = 1;
          return fmt("the op elem names the element %s, which the program did not make", o->f[3]);
        }
        if (!r->node) {
          return dups("TypeError: the element is null");
        }
        scope = r->node;
      }
      if (o->n < 5) {
        *program_error = 1;
        return dups("elem sel has too few fields");
      }
      GTEXT_Markup_Node ** nodes = NULL;
      size_t count = 0;
      GTEXT_Markup_Status ms;
      if (strcmp(how, "sel") == 0) {
        GTEXT_Markup_Node * one = NULL;
        ms = gtext_markup_query_selector(scope, o->f[4], o->l[4], NULL, &one, NULL);
        if (ms != GTEXT_MARKUP_OK && ms != GTEXT_MARKUP_E_SELECTOR) {
          return fmt("querySelector failed: %d", (int)ms);
        }
        if (ms == GTEXT_MARKUP_E_SELECTOR) {
          return dups("SyntaxError: the selector is not valid");
        }
        found = one;
      }
      else {
        ms = gtext_markup_query_selector_all(scope, o->f[4], o->l[4], NULL, &nodes, &count, NULL);
        if (ms == GTEXT_MARKUP_E_SELECTOR) {
          return dups("SyntaxError: the selector is not valid");
        }
        if (ms != GTEXT_MARKUP_OK) {
          return fmt("querySelectorAll failed: %d", (int)ms);
        }
        const size_t idx = o->n > 5 ? (size_t)strtoull(o->f[5], NULL, 10) : 0;
        found = idx < count ? nodes[idx] : NULL;
        gtext_markup_query_free(NULL, nodes);
      }
    }
    else if (strcmp(how, "nav") == 0) {
      elem * from = find_elem(w, o->f[3]);
      if (!from) {
        *program_error = 1;
        return fmt("the op elem names the element %s, which the program did not make", o->f[3]);
      }
      if (!from->node) {
        return dups("TypeError: the element is null");
      }
      const char * what = o->n > 4 ? o->f[4] : "";
      GTEXT_Markup_Node * n = from->node;
      if (strcmp(what, "parentElement") == 0) {
        GTEXT_Markup_Node * p = gtext_markup_node_parent(n);
        found = is_element(p) ? p : NULL;
      }
      else if (strcmp(what, "firstElementChild") == 0) {
        for (GTEXT_Markup_Node * c = gtext_markup_node_first_child(n); c && !found; c = gtext_markup_node_next_sibling(c)) {
          found = is_element(c) ? c : NULL;
        }
      }
      else if (strcmp(what, "lastElementChild") == 0) {
        for (GTEXT_Markup_Node * c = gtext_markup_node_last_child(n); c && !found; c = gtext_markup_node_previous_sibling(c)) {
          found = is_element(c) ? c : NULL;
        }
      }
      else if (strcmp(what, "nextElementSibling") == 0) {
        found = element_sibling(n, 1);
      }
      else if (strcmp(what, "previousElementSibling") == 0) {
        found = element_sibling(n, 0);
      }
      else {
        *program_error = 1;
        return fmt("elem nav %s is not known", what);
      }
    }
    else {
      *program_error = 1;
      return fmt("elem %s is not known", how);
    }
    put_elem(w, o->f[1], found);
    return NULL;
  }
  if (strcmp(name, "gcs") == 0) {
    NEED(4);
    ELEM(e, 2);
    GTEXT_CSSOM_Decl * d = NULL;
    GTEXT_CSS_Status st = gtext_cssom_computed_style(w->doc, gtext_css_markup_handle(e->node), o->f[3], o->l[3], &d);
    if (st != GTEXT_CSS_OK) {
      return status_failure("getComputedStyle", st);
    }
    put_style(w, o->f[1], d, 1, e->node, o->f[2], o->f[3]);
    return NULL;
  }
  if (strcmp(name, "style") == 0) {
    NEED(3);
    ELEM(e, 2);
    GTEXT_CSSOM_Decl * d = NULL;
    GTEXT_CSS_Status st = gtext_cssom_inline_style(w->doc, e->node, &w->attr, &d);
    if (st != GTEXT_CSS_OK) {
      return status_failure("element.style", st);
    }
    put_style(w, o->f[1], d, 0, e->node, o->f[2], "");
    return NULL;
  }
  if (strcmp(name, "sget") == 0 || strcmp(name, "sprio") == 0 || strcmp(name, "sattr") == 0 || strcmp(name, "sremove") == 0) {
    NEED(4);
    STYLE(s, 2);
    GTEXT_CSS_Status st;
    if (strcmp(name, "sget") == 0) {
      char * out = NULL;
      size_t n = 0;
      st = gtext_cssom_decl_get_property_value(s->decl, o->f[3], o->l[3], &out, &n);
      if (st != GTEXT_CSS_OK) {
        return status_failure("getPropertyValue", st);
      }
      put_reg(w, o->f[1], take_string(w, out, n));
      note_layout(t, s, o->f[3]);
    }
    else if (strcmp(name, "sprio") == 0) {
      const char * p = NULL;
      st = gtext_cssom_decl_get_property_priority(s->decl, o->f[3], o->l[3], &p);
      if (st != GTEXT_CSS_OK) {
        return status_failure("getPropertyPriority", st);
      }
      put_reg(w, o->f[1], val('s', p ? p : "", p ? strlen(p) : 0));
    }
    else if (strcmp(name, "sremove") == 0) {
      char * out = NULL;
      size_t n = 0;
      st = gtext_cssom_decl_remove_property(s->decl, o->f[3], o->l[3], &out, &n, NULL);
      if (st != GTEXT_CSS_OK) {
        return status_failure("removeProperty", st);
      }
      put_reg(w, o->f[1], take_string(w, out, n));
    }
    else {
      if (!gtext_cssom_decl_has_attribute(s->decl, o->f[3], o->l[3])) {
        put_reg(w, o->f[1], val('u', "", 0));
        return NULL;
      }
      char * out = NULL;
      size_t n = 0;
      st = gtext_cssom_decl_idl_get(s->decl, o->f[3], o->l[3], &out, &n);
      if (st != GTEXT_CSS_OK) {
        return status_failure("style attribute", st);
      }
      put_reg(w, o->f[1], take_string(w, out, n));
      char * css = idl_to_css(o->f[3], o->l[3]);
      note_layout(t, s, css);
      free(css);
    }
    return NULL;
  }
  if (strcmp(name, "slen") == 0 || strcmp(name, "scss") == 0 || strcmp(name, "sitem") == 0) {
    STYLE(s, 2);
    GTEXT_CSS_Status st;
    if (strcmp(name, "slen") == 0) {
      size_t n = 0;
      st = gtext_cssom_decl_length(s->decl, &n);
      if (st != GTEXT_CSS_OK) {
        return status_failure("length", st);
      }
      char num[32];
      snprintf(num, sizeof num, "%zu", n);
      put_reg(w, o->f[1], val('n', num, strlen(num)));
    }
    else if (strcmp(name, "scss") == 0) {
      char * out = NULL;
      size_t n = 0;
      st = gtext_cssom_decl_css_text(s->decl, &out, &n);
      if (st != GTEXT_CSS_OK) {
        return status_failure("cssText", st);
      }
      put_reg(w, o->f[1], take_string(w, out, n));
    }
    else {
      size_t len = 0;
      st = gtext_cssom_decl_length(s->decl, &len);
      const size_t index = (size_t)strtoull(o->f[3], NULL, 10);
      if (st != GTEXT_CSS_OK) {
        return status_failure("length", st);
      }
      if (index >= len) {
        put_reg(w, o->f[1], val('s', "", 0));
        return NULL;
      }
      char * out = NULL;
      size_t n = 0;
      st = gtext_cssom_decl_item(s->decl, index, &out, &n);
      if (st != GTEXT_CSS_OK) {
        return status_failure("item", st);
      }
      put_reg(w, o->f[1], take_string(w, out, n));
    }
    return NULL;
  }
  if (strcmp(name, "sset") == 0) {
    NEED(5);
    STYLE(s, 1);
    GTEXT_CSS_Status st = gtext_cssom_decl_set_property(s->decl, o->f[2], o->l[2], o->f[3], o->l[3], o->f[4], o->l[4], NULL);
    return st == GTEXT_CSS_OK ? NULL : status_failure("setProperty", st);
  }
  if (strcmp(name, "sattrset") == 0) {
    NEED(4);
    STYLE(s, 1);
    if (!gtext_cssom_decl_has_attribute(s->decl, o->f[2], o->l[2])) {
      return NULL;   /* an expando of the script object: nothing happens to the declaration */
    }
    GTEXT_CSS_Status st = gtext_cssom_decl_idl_set(s->decl, o->f[2], o->l[2], o->f[3], o->l[3], NULL);
    return st == GTEXT_CSS_OK ? NULL : status_failure("style attribute", st);
  }
  if (strcmp(name, "scsstext") == 0) {
    NEED(3);
    STYLE(s, 1);
    GTEXT_CSS_Status st = gtext_cssom_decl_set_css_text(s->decl, o->f[2], o->l[2], NULL);
    return st == GTEXT_CSS_OK ? NULL : status_failure("cssText", st);
  }
  if (strcmp(name, "setattr") == 0) {
    NEED(4);
    ELEM(e, 1);
    char * lower = dupn(o->f[2], o->l[2]);
    for (char * p = lower; *p; p++) {
      *p = (char)tolower((unsigned char)*p);
    }
    GTEXT_Markup_Status ms = gtext_markup_element_set_attribute(e->node, NULL, NULL, lower, o->f[3], o->l[3]);
    free(lower);
    return ms == GTEXT_MARKUP_OK ? NULL : fmt("setAttribute failed: %d", (int)ms);
  }
  if (strcmp(name, "rmattr") == 0) {
    NEED(3);
    ELEM(e, 1);
    char * lower = dupn(o->f[2], o->l[2]);
    for (char * p = lower; *p; p++) {
      *p = (char)tolower((unsigned char)*p);
    }
    gtext_markup_element_remove_attribute(e->node, NULL, lower);
    free(lower);
    return NULL;
  }
  if (strcmp(name, "getattr") == 0 || strcmp(name, "hasattr") == 0) {
    NEED(4);
    ELEM(e, 2);
    char * lower = dupn(o->f[3], o->l[3]);
    for (char * p = lower; *p; p++) {
      *p = (char)tolower((unsigned char)*p);
    }
    const char * v;
    size_t vl;
    const int has = attr_get(e->node, lower, &v, &vl);
    free(lower);
    if (strcmp(name, "hasattr") == 0) {
      put_reg(w, o->f[1], val('b', has ? "true" : "false", has ? 4 : 5));
    }
    else {
      put_reg(w, o->f[1], has ? val('s', v, vl) : val('N', "", 0));
    }
    return NULL;
  }
  if (strncmp(name, "class", 5) == 0) {
    const int add = strcmp(name, "classadd") == 0, rem = strcmp(name, "classremove") == 0, tog = strcmp(name, "classtoggle") == 0, has = strcmp(name, "classhas") == 0;
    if (!(add || rem || tog || has)) {
      *program_error = 1;
      return fmt("the op %s is not known", name);
    }
    const size_t first = (tog || has) ? 3 : 2;
    ELEM(e, (tog || has) ? 2 : 1);
    const char * cur = "";
    size_t cl = 0;
    attr_get(e->node, "class", &cur, &cl);
    char * list = dupn(cur, cl);
    /* the tokens of the class attribute */
    char * tokens[256];
    size_t nt = 0;
    for (char * p = strtok(list, " \t\n\f\r"); p && nt < 256; p = strtok(NULL, " \t\n\f\r")) {
      tokens[nt++] = p;
    }
    int result = 0, changed = 0;
    for (size_t k = first; k < o->n; k++) {
      size_t at = nt;
      for (size_t i = 0; i < nt; i++) {
        if (strcmp(tokens[i], o->f[k]) == 0) {
          at = i;
          break;
        }
      }
      if (has) {
        result = at < nt;
      }
      else if (add || (tog && at == nt)) {
        if (at == nt && nt < 256) {
          tokens[nt++] = o->f[k];
          changed = 1;
        }
        result = 1;
      }
      else if (rem || (tog && at < nt)) {
        if (at < nt) {
          for (size_t i = at; i + 1 < nt; i++) {
            tokens[i] = tokens[i + 1];
          }
          nt--;
          changed = 1;
        }
        result = 0;
      }
    }
    if (changed) {
      size_t total = 1;
      for (size_t i = 0; i < nt; i++) {
        total += strlen(tokens[i]) + 1;
      }
      char * joined = malloc(total);
      size_t jo = 0;
      for (size_t i = 0; i < nt; i++) {
        if (i) {
          joined[jo++] = ' ';
        }
        jo += (size_t)sprintf(joined + jo, "%s", tokens[i]);
      }
      joined[jo] = '\0';
      gtext_markup_element_set_attribute(e->node, NULL, NULL, "class", joined, jo);
      free(joined);
    }
    free(list);
    if (tog || has) {
      put_reg(w, o->f[1], val('b', result ? "true" : "false", result ? 4 : 5));
    }
    return NULL;
  }
  if (strcmp(name, "shas") == 0) {
    NEED(4);
    STYLE(s, 2);
    const bool has = gtext_cssom_decl_has_attribute(s->decl, o->f[3], o->l[3]);
    put_reg(w, o->f[1], val('b', has ? "true" : "false", has ? 4 : 5));
    return NULL;
  }
  if (strcmp(name, "script") == 0) {
    return NULL;   /* where a script ends in the page: the replay of the oracle puts the ops of a script back where the script was */
  }
  if (strcmp(name, "csssupports") == 0) {
    /* CSS.supports(conditionText) with one argument, CSS.supports(property, value) with two (the extra arguments of a call are ignored by the binding) */
    if (o->n != 3 && o->n != 4) {
      *program_error = 1;
      return dups("csssupports has the wrong number of fields");
    }
    bool result = false;
    GTEXT_CSS_Status st;
    if (o->n == 3) {
      st = gtext_css_conditions_supports_text(w->conditions, o->f[2], o->l[2], &result);
    }
    else {
      st = gtext_css_conditions_supports(w->conditions, o->f[2], o->f[3], &result);
    }
    if (st != GTEXT_CSS_OK) {
      return status_failure("CSS.supports", st);
    }
    put_reg(w, o->f[1], val('b', result ? "true" : "false", result ? 4 : 5));
    return NULL;
  }
  if (strcmp(name, "cssescape") == 0) {
    NEED(3);
    char * out = NULL;
    size_t n = 0;
    GTEXT_CSS_Status st = gtext_cssom_escape_buffer(o->f[2], o->l[2], GTEXT_CSSOM_TEXT_WTF8, NULL, &out, &n);
    if (st != GTEXT_CSS_OK) {
      return status_failure("CSS.escape", st);
    }
    put_reg(w, o->f[1], take_string(w, out, n));
    return NULL;
  }
  if (strcmp(name, "cmp") == 0) {
    NEED(5);
    value a = operand(w, o->f[3], o->l[3], &ok), b;
    if (!ok) {
      *program_error = 1;
      return dups("cmp: an operand the program did not make");
    }
    b = operand(w, o->f[4], o->l[4], &ok);
    if (!ok) {
      value_free(&a);
      *program_error = 1;
      return dups("cmp: an operand the program did not make");
    }
    int eq = same(&a, &b);
    if ((o->f[2][0] == 'l') && !eq) {
      /* loose equality: a number and a string compare as numbers; null and undefined are equal */
      if ((a.kind == 'N' || a.kind == 'u') && (b.kind == 'N' || b.kind == 'u')) {
        eq = 1;
      }
      else if ((a.kind == 'n' && b.kind == 's') || (a.kind == 's' && b.kind == 'n')) {
        eq = strtod(a.s, NULL) == strtod(b.s, NULL);
      }
    }
    const int negate = strcmp(o->f[2], "ne") == 0 || strcmp(o->f[2], "lne") == 0;
    const int r = negate ? !eq : eq;
    put_reg(w, o->f[1], val('b', r ? "true" : "false", r ? 4 : 5));
    value_free(&a);
    value_free(&b);
    return NULL;
  }
  if (strcmp(name, "not") == 0) {
    NEED(3);
    value a = operand(w, o->f[2], o->l[2], &ok);
    if (!ok) {
      *program_error = 1;
      return dups("not: an operand the program did not make");
    }
    const int truthy = a.kind == 'b' ? strcmp(a.s, "true") == 0 : a.kind == 's' ? a.n > 0 : a.kind == 'n' ? strtod(a.s, NULL) != 0 : 0;
    put_reg(w, o->f[1], val('b', truthy ? "false" : "true", truthy ? 5 : 4));
    value_free(&a);
    return NULL;
  }
  if (strcmp(name, "fail") == 0) {
    return dups(o->n > 1 ? o->f[1] : "failed");
  }
  if (strncmp(name, "assert_", 7) == 0) {
    value vals[MAX_FIELDS];
    size_t nv = 0;
    const int has_msg = 1;
    for (size_t i = 1; i + (has_msg ? 1 : 0) < o->n; i++) {
      vals[nv] = operand(w, o->f[i], o->l[i], &ok);
      if (!ok) {
        for (size_t k = 0; k < nv; k++) {
          value_free(&vals[k]);
        }
        *program_error = 1;
        return fmt("%s: an operand the program did not make", name);
      }
      nv++;
    }
    const char * msg = o->n > 1 ? o->f[o->n - 1] : "";
    char * result = NULL;
    if (strcmp(name, "assert_equals") == 0 && nv == 2) {
      if (!same_as(&vals[0], &vals[1], 1)) {
        result = mismatch(name, msg, &vals[1], &vals[0]);
      }
    }
    else if (strcmp(name, "assert_not_equals") == 0 && nv == 2) {
      if (same_as(&vals[0], &vals[1], 1)) {
        char g[400];
        fmt_value(&vals[0], g, sizeof g);
        result = fmt("assert_not_equals: %sgot disallowed value %s", msg, g);
      }
    }
    else if ((strcmp(name, "assert_true") == 0 || strcmp(name, "assert_false") == 0) && nv == 1) {
      const int want = strcmp(name, "assert_true") == 0;
      if (!(vals[0].kind == 'b' && (strcmp(vals[0].s, "true") == 0) == want)) {
        char g[400];
        fmt_value(&vals[0], g, sizeof g);
        result = fmt("%s: %sexpected %s got %s", name, msg, want ? "true" : "false", g);
      }
    }
    else if (strcmp(name, "assert_in_array") == 0 && nv >= 2) {
      int found = 0;
      for (size_t k = 1; k < nv; k++) {
        found |= same(&vals[0], &vals[k]);
      }
      if (!found) {
        char g[400];
        fmt_value(&vals[0], g, sizeof g);
        result = fmt("assert_in_array: %svalue %s not in array", msg, g);
      }
    }
    else if (strcmp(name, "assert_approx_equals") == 0 && nv == 3) {
      if (vals[0].kind != 'n' || vals[1].kind != 'n' || vals[2].kind != 'n' || fabs(strtod(vals[0].s, NULL) - strtod(vals[1].s, NULL)) > strtod(vals[2].s, NULL)) {
        result = fmt("assert_approx_equals: %s%s differs from %s by more than %s", msg, vals[0].s, vals[1].s, vals[2].s);
      }
    }
    else if (nv == 2 && (strcmp(name, "assert_greater_than") == 0 || strcmp(name, "assert_less_than") == 0 || strcmp(name, "assert_greater_than_equal") == 0 ||
                         strcmp(name, "assert_less_than_equal") == 0)) {
      int good = 0;
      if (vals[0].kind == 'n' && vals[1].kind == 'n') {
        const double x = strtod(vals[0].s, NULL), y = strtod(vals[1].s, NULL);
        good = strcmp(name, "assert_greater_than") == 0 ? x > y : strcmp(name, "assert_less_than") == 0 ? x < y : strcmp(name, "assert_greater_than_equal") == 0 ? x >= y : x <= y;
      }
      if (!good) {
        result = fmt("%s: %sfailed for %s, %s", name, msg, vals[0].s, vals[1].s);
      }
    }
    else {
      *program_error = 1;
      result = fmt("the op %s has %zu operands", name, nv);
    }
    for (size_t k = 0; k < nv; k++) {
      value_free(&vals[k]);
    }
    return result;
  }
  *program_error = 1;
  return fmt("the op %s is not one the runner knows", name);
#undef NEED
#undef ELEM
#undef STYLE
}

/* ---- the programs -------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  char * file;
  char * html;       /* the page's markup, all of it */
  size_t html_len;
  world w;
  int open;
  int in_test;
  int in_cleanup;
  int index;
  testrun t;
  int errors;
} program;

/* ---- the document as a script sees it -------------------------------------------------------------------------------------------------- */

/* A script runs when the parser reaches its end tag, so it sees the markup up to there (`doc N` gives N, in bytes). The document is parsed again from that prefix; the elements, the
 * declarations and the registers the program holds are carried over, an element by its place in the tree (the index of each node among its siblings from the root), a declaration by
 * the element and the pseudo-element it was made for. The evaluator emits `doc` only while the program has changed nothing, so the new tree is the old one grown. */
static size_t * node_path(const GTEXT_Markup_Node * n, size_t * depth) {
  size_t cap = 16, d = 0;
  size_t * path = malloc(cap * sizeof *path);
  for (const GTEXT_Markup_Node * c = n; c && gtext_markup_node_parent(c); c = gtext_markup_node_parent(c)) {
    size_t index = 0;
    for (GTEXT_Markup_Node * sib = gtext_markup_node_previous_sibling(c); sib; sib = gtext_markup_node_previous_sibling(sib)) {
      index++;
    }
    if (d == cap) {
      cap *= 2;
      path = realloc(path, cap * sizeof *path);
    }
    path[d++] = index;
  }
  *depth = d;
  return path;
}

static GTEXT_Markup_Node * node_at(GTEXT_Markup_Document * m, const size_t * path, size_t depth) {
  GTEXT_Markup_Node * n = gtext_markup_document_root(m);
  while (depth-- > 0 && n) {
    n = gtext_markup_node_first_child(n);
    for (size_t i = path[depth]; n && i > 0; i--) {
      n = gtext_markup_node_next_sibling(n);
    }
  }
  return n;
}

static int program_open(program * p, size_t extent) {
  if (extent > p->html_len) {
    extent = p->html_len;
  }
  world nw;
  if (!world_open(&nw, p->html, extent)) {
    world_close(&nw);
    return 0;
  }
  if (p->open) {
    world * ow = &p->w;
    for (size_t i = 0; i < ow->nelems; i++) {
      GTEXT_Markup_Node * n = NULL;
      if (ow->elems[i].node) {
        size_t depth;
        size_t * path = node_path(ow->elems[i].node, &depth);
        n = node_at(nw.markup, path, depth);
        free(path);
      }
      put_elem(&nw, ow->elems[i].name, n);
    }
    for (size_t i = 0; i < ow->nregs; i++) {
      put_reg(&nw, ow->regs[i].name, val(ow->regs[i].v.kind, ow->regs[i].v.s, ow->regs[i].v.n));
    }
    g_doc = nw.doc;
    for (size_t i = 0; i < ow->nstyles; i++) {
      style * s = &ow->styles[i];
      elem * e = find_elem(&nw, s->elem_name);
      GTEXT_CSSOM_Decl * d = NULL;
      if (e && e->node) {
        if (s->computed) {
          gtext_cssom_computed_style(nw.doc, gtext_css_markup_handle(e->node), s->pseudo, strlen(s->pseudo), &d);
        }
        else {
          gtext_cssom_inline_style(nw.doc, e->node, &nw.attr, &d);
        }
      }
      put_style(&nw, s->name, d, s->computed, e ? e->node : NULL, s->elem_name, s->pseudo);
    }
    world_close(ow);
  }
  p->w = nw;
  p->open = 1;
  g_doc = p->w.doc;
  return 1;
}

static void print_escaped(const char * s) {
  for (; *s; s++) {
    if (*s == '\n') {
      fputs("\\n", stdout);
    }
    else if (*s == '\t') {
      fputs("\\t", stdout);
    }
    else if (*s == '\r') {
      fputs("\\r", stdout);
    }
    else {
      fputc(*s, stdout);
    }
  }
}

static void end_test(program * p) {
  if (p->in_test) {
    if (p->t.layout) {
      printf("R\t%s\t%d\tLAYOUT\t", p->file, p->index);
      print_escaped(p->t.layout_property ? p->t.layout_property : "");
      fputc('\n', stdout);
    }
    else if (p->t.failed) {
      printf("R\t%s\t%d\tFAIL\t", p->file, p->index);
      print_escaped(p->t.message ? p->t.message : "");
      fputc('\n', stdout);
    }
    else {
      printf("R\t%s\t%d\tPASS\n", p->file, p->index);
    }
  }
  free(p->t.message);
  free(p->t.layout_property);
  memset(&p->t, 0, sizeof p->t);
  p->in_test = 0;
  p->in_cleanup = 0;
}

static void end_program(program * p) {
  end_test(p);
  if (p->open) {
    world_close(&p->w);
    p->open = 0;
  }
  free(p->file);
  free(p->html);
  p->file = NULL;
  p->html = NULL;
}

int main(void) {
  const char * ua = getenv("CSSOM_RUNNER_UA");
  if (ua && strcmp(ua, "none") == 0) {
    g_ua_len = 0;
    g_palette = 0;
  }
  else if (ua && strcmp(ua, "full") == 0) {
    g_ua_sheet = UA_SHEET_FULL;
    g_ua_len = sizeof UA_SHEET_FULL - 1;
  }
  program p;
  memset(&p, 0, sizeof p);
  char * line = NULL;
  size_t cap = 0;
  ssize_t len;
  while ((len = getline(&line, &cap, stdin)) >= 0) {
    if (len && line[len - 1] == '\n') {
      line[--len] = '\0';
    }
    if (line[0] == 'P' && line[1] == '\t') {
      end_program(&p);
      op o;
      if (!parse_op(line + 2, &o) || o.n != 3) {
        printf("E\t?\tbad program line\n");
        continue;
      }
      p.file = dups(o.f[0]);
    }
    else if (line[0] == 'H' && line[1] == '\t' && p.file) {
      op o;
      if (!parse_op(line + 2, &o) || o.n != 1) {
        printf("E\t%s\tbad document line\n", p.file);
        continue;
      }
      /* the document is parsed when the first op needs it: a program that says `doc N` first never builds the whole page */
      p.html = dupn(o.f[0], o.l[0]);
      p.html_len = o.l[0];
    }
    else if (line[0] == 'O' && line[1] == '\t' && p.file) {
      op o;
      if (!parse_op(line + 2, &o)) {
        printf("E\t%s\ttoo many fields\n", p.file);
        continue;
      }
      if (strcmp(o.f[0], "test") == 0 && o.n >= 2) {
        end_test(&p);
        p.in_test = 1;
        p.index = atoi(o.f[1]);
        continue;
      }
      if (strcmp(o.f[0], "cleanup") == 0 && o.n == 1) {
        p.in_cleanup = 1;
        continue;
      }
      if (strcmp(o.f[0], "end") == 0 && o.n == 1) {
        end_test(&p);
        continue;
      }
      if (strcmp(o.f[0], "doc") == 0 && o.n == 2 && p.html) {
        if (!program_open(&p, (size_t)strtoull(o.f[1], NULL, 10))) {
          printf("E\t%s\tcould not open the document\n", p.file);
        }
        continue;
      }
      if (!p.open && p.html && !program_open(&p, p.html_len)) {
        printf("E\t%s\tcould not open the document\n", p.file);
      }
      if (!p.open || (p.in_test && p.t.failed && !p.in_cleanup)) {
        if (p.open && (strcmp(o.f[0], "elem") == 0 || strcmp(o.f[0], "gcs") == 0 || strcmp(o.f[0], "sget") == 0 || strcmp(o.f[0], "sattr") == 0)) {
          /* the exception of a failed assertion ended the test body; the reads after it are still looked at, for the one thing a read tells whatever the assertion did: that it needs a layout */
          int ignored = 0;
          free(run_op(&p.w, &o, &p.t, &ignored));
        }
        continue;
      }
      int program_error = 0;
      char * msg = run_op(&p.w, &o, &p.t, &program_error);
      if (msg && !p.t.layout) {
        /* a read that failed for want of a font metric, a container's size, scroll state or anchored fallback is a read that needs a layout, whatever the page asserts */
        const char * at = strstr(msg, "unsupported, needs ");
        if (at) {
          static const char * const layout_callbacks[] = {"cq", "ch", "ex", "ic", "cap", "lh", "rlh", "rch", "rex", "ric", "rcap", "container.size", "container.scroll_state", "container.anchored_fallback"};
          at += strlen("unsupported, needs ");
          for (size_t k = 0; k < sizeof layout_callbacks / sizeof *layout_callbacks; k++) {
            const size_t n = strlen(layout_callbacks[k]);
            if (strncmp(at, layout_callbacks[k], n) == 0 && (at[n] == '\0' || k == 0)) {
              p.t.layout = 1;
              p.t.layout_property = fmt("needs the %s of a layout", at);
            }
          }
        }
      }
      if (msg) {
        if (program_error) {
          printf("E\t%s\t", p.file);
          print_escaped(msg);
          fputc('\n', stdout);
          free(msg);
        }
        else if (p.in_test) {
          if (!p.t.failed) {
            p.t.failed = 1;
            p.t.message = msg;
          }
          else {
            free(msg);
          }
        }
        else {
          printf("E\t%s\t", p.file);   /* a statement of the page's own script outside any test threw */
          print_escaped(msg);
          fputc('\n', stdout);
          free(msg);
        }
      }
    }
  }
  end_program(&p);
  free(line);
  return 0;
}
