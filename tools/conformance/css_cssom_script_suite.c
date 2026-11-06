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
 * message`, or `R file index LAYOUT property` (the test read a property whose used value needs a layout the library was not given: it is neither passed nor failed), or
 * `R file index TIMELINE property` (the test read a property that an animation targets, and the animation was given no time: the same), for each test,
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
 * create E TAG NAMESPACE | ctext E DATA | ccomment E DATA | cfrag E | clone E SRC true|false (nodes are `eN` too: an element, a text node, a fragment); append|prepend|replacechildren|before|
 * after|replacewith NODE ARG... (an ARG is `e:name` or `s:text`); appendchild PARENT CHILD; insertbefore PARENT CHILD REF (`N:` for none); removechild PARENT CHILD; replacechild PARENT
 * NEW OLD; remove NODE; tcget R NODE; tcset NODE VALUE; dset NODE VALUE; ihget|ohget R NODE; ihset NODE VALUE; nread R NODE nodeType|nodeName|tagName|localName|namespaceURI|nodeValue|
 * data|isConnected|hasChildNodes|childElementCount; contains R A B; matches R E SEL; closest E2 E SEL; child E PARENT elements|nodes INDEX; nchildren R PARENT elements|nodes; qlen R ROOT SEL;
 * the nav of elem also takes firstChild|lastChild|nextSibling|previousSibling|parentNode. After a change of the tree the Markup binding is synced, so a style element made by the script has
 * its sheet.
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
  char * name;
  int kind;    /* a sheet, a rule or a media list */
  int state;   /* the object, null, or undefined */
  void * ptr;  /* retained */
} object;

typedef struct {
  char * name;
  GTEXT_Markup_Node ** nodes;   /* a static NodeList: the elements a querySelectorAll() found when it was called */
  size_t n;
} nodelist;

typedef struct {
  GTEXT_CSS_Registry * registry;
  GTEXT_CSS_Conditions * conditions;
  GTEXT_CSSOM_Document * doc;
  GTEXT_Markup_Document * markup;
  GTEXT_CSS_Markup_Adapter * adapter;
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
  nodelist * lists;
  size_t nlists, caplists;
  object * objs;
  size_t nobjs, capobjs;
} world;

/* The user agent's style sheet is the part of the HTML rendering section that the display and the body's margin need; the library has no user agent sheet of its own. */
/* `[inert] { interactivity: inert }` is the rule CSS UI 4 ("Preventing All Interaction: the interactivity property") says to insert into the HTML style sheet. */
static const char UA_SHEET[] =
    "html, address, blockquote, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing, main, p, plaintext, pre, search, xmp, article, aside, h1, h2, "
    "h3, h4, h5, h6, hgroup, nav, section, dir, dd, dl, dt, menu, ol, ul, details, summary, fieldset, optgroup, option { display: block }\n"
    "body { display: block; margin: 8px }\n"
    "li { display: list-item }\n"
    "table { display: table } caption { display: table-caption } colgroup { display: table-column-group } col { display: table-column } thead { display: table-header-group }\n"
    "tbody { display: table-row-group } tfoot { display: table-footer-group } tr { display: table-row } td, th { display: table-cell }\n"
    "[hidden], area, base, basefont, datalist, head, link, meta, noembed, noframes, param, rp, script, style, template, title { display: none }\n"
    "input, button, select, textarea { display: inline-block }\n"
    "[inert] { interactivity: inert }\n";

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
    "[inert] { interactivity: inert }\n"
    "p, dl, multicol { margin-block: 1em } blockquote, figure { margin-block: 1em; margin-inline: 40px } center { text-align: center } hr { margin-block: 0.5em; margin-inline: auto; border-style: inset; border-width: 1px; color: gray }\n"
    "h1 { font-size: 2em; margin-block: 0.67em } h2 { font-size: 1.5em; margin-block: 0.83em } h3 { font-size: 1.17em; margin-block: 1em } h4 { margin-block: 1.33em } h5 { font-size: 0.83em; margin-block: 1.67em } h6 { font-size: 0.67em; margin-block: 2.33em }\n"
    "h1, h2, h3, h4, h5, h6, b, strong, th { font-weight: bold } i, cite, em, var, address, dfn { font-style: italic } dt { } dd { margin-inline-start: 40px }\n"
    "u, ins { text-decoration: underline } s, strike, del { text-decoration: line-through } a:any-link { text-decoration: underline } abbr[title], acronym[title] { text-decoration: underline dotted }\n"
    "pre, listing, xmp, plaintext { white-space: pre } textarea { white-space: pre-wrap } code, kbd, samp, tt, pre, listing, xmp, plaintext { font-family: monospace }\n"
    "small, sub, sup { font-size: smaller } big { font-size: larger } sub { vertical-align: sub } sup { vertical-align: super } mark { background-color: yellow; color: black }\n"
    "ul, ol, dir, menu { margin-block: 1em; padding-inline-start: 40px } ul, dir, menu { list-style-type: disc } ol { list-style-type: decimal }\n"
    "table { box-sizing: border-box; border-spacing: 2px; border-collapse: separate } td, th { padding: 1px } th { text-align: center } caption { text-align: center }\n"
    "fieldset { margin-inline: 2px; padding-block: 0.35em 0.625em; padding-inline: 0.75em; border: 2px groove } img { } svg:not(:root) { overflow: hidden }\n";

/* The system colors are the user's: a computed value keeps the keyword until the consumer's environment says what it is (css_element.h resolve_color). The runner is the consumer here and
 * answers every system color of CSS Color 4 and the deprecated ones' counterparts with the sRGB value Chromium gives it in each scheme (a palette is the consumer's to choose, and the two
 * browsers choose differently for several: the computed-value gate supplies the same values for the names it asks about). Any other name is not answered. */
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
  /* the sRGB value of each system color in the light and the dark scheme: Chromium's (the browsers differ in several, and the consumer's palette is the consumer's) */
  static const struct {
    const char * name;
    double light[4], dark[4];
  } palette[] = {
      {"canvas", {255, 255, 255, 1}, {18, 18, 18, 1}},
      {"canvastext", {0, 0, 0, 1}, {255, 255, 255, 1}},
      {"linktext", {0, 0, 238, 1}, {158, 158, 255, 1}},
      {"visitedtext", {85, 26, 139, 1}, {208, 173, 240, 1}},
      {"activetext", {255, 0, 0, 1}, {255, 0, 0, 1}},
      {"buttonface", {239, 239, 239, 1}, {107, 107, 107, 1}},
      {"buttontext", {0, 0, 0, 1}, {255, 255, 255, 1}},
      {"buttonborder", {0, 0, 0, 1}, {255, 255, 255, 1}},
      {"field", {255, 255, 255, 1}, {59, 59, 59, 1}},
      {"fieldtext", {0, 0, 0, 1}, {255, 255, 255, 1}},
      {"highlight", {0, 65, 198, 0.8}, {0, 65, 198, 0.8}},
      {"highlighttext", {255, 255, 255, 1}, {255, 255, 255, 1}},
      {"selecteditem", {25, 103, 210, 1}, {153, 200, 255, 1}},
      {"selecteditemtext", {255, 255, 255, 1}, {59, 59, 59, 1}},
      {"mark", {255, 255, 0, 1}, {255, 255, 0, 1}},
      {"marktext", {0, 0, 0, 1}, {0, 0, 0, 1}},
      {"graytext", {128, 128, 128, 1}, {128, 128, 128, 1}},
      {"accentcolor", {0, 117, 255, 1}, {0, 117, 255, 1}},
      {"accentcolortext", {255, 255, 255, 1}, {255, 255, 255, 1}},
      {"appworkspace", {255, 255, 255, 1}, {18, 18, 18, 1}},
      {"scrollbar", {255, 255, 255, 1}, {18, 18, 18, 1}},
  };
  for (size_t i = 0; i < sizeof palette / sizeof *palette; i++) {
    if (strcmp(w, palette[i].name) == 0) {
      for (int k = 0; k < 4; k++) {
        rgba[k] = dark ? palette[i].dark[k] : palette[i].light[k];
      }
      return true;
    }
  }
  return false;
}

static int g_palette = 1;
static const char * g_ua_sheet = UA_SHEET;
static size_t g_ua_len = sizeof UA_SHEET - 1;

/* The conditions the environment of the compute context answers `if(media())` with, and the styles a query container is read from, are the world's: the context, the conditions object and
   the document are made in that order and each needs the next, so the callbacks reach them through the world. */
/* The runner is the caller of the library, and its document timeline stands at 0: every animation began there, so the time of each is 0 (the pages that need more are timing pages, which the
   scoring interpreter does not read). With CSSOM_RUNNER_TIMELINE=none it gives no time at all, which is the case the controls show: the property an animation targets then says
   ABSENT_TIMELINE and the test is not credited. The version of the timeline never moves. */
static int g_timeline = 1;

static GTEXT_CSS_Status runner_animation_time(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, const GTEXT_CSS_Anim_Effect * effect, double * time_ms, bool * known) {
  (void)user;
  (void)element;
  (void)pseudo;
  (void)effect;
  *time_ms = 0.0;
  *known = g_timeline != 0;
  return GTEXT_CSS_OK;
}

static uint64_t runner_timeline_version(void * user) {
  (void)user;
  return 0;
}

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
  GTEXT_CSSOM_Layout_Interface layout;
  memset(&layout, 0, sizeof layout);
  layout.size = sizeof layout;
  layout.animation_time = runner_animation_time;
  layout.timeline_version = runner_timeline_version;
  c.layout = &layout;
  /* the runner is a consumer with an environment of its own (the palette, and conditions that are the world's); the document makes the compute context with it and with the registrations of the
     `@property` rules of its sheets, and the conditions object that reads a query container's style is given to the document once it exists */
  GTEXT_CSS_Environment cenv = gtext_css_environment_default();
  cenv.user = w;
  cenv.condition = runner_condition;
  if (g_palette) {
    cenv.resolve_color = runner_resolve_color;
  }
  c.environment = &cenv;
  if (gtext_cssom_document_new(&c, &w->doc) != GTEXT_CSS_OK) {
    return 0;
  }
  {
    GTEXT_CSS_Container_Interface container;
    if (gtext_cssom_document_container_interface(w->doc, &container) != GTEXT_CSS_OK) {
      return 0;
    }
    container.user = w;
    container.computed_style = runner_container_style;
    GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
    co.media = &env;
    co.element_interface = c.element_interface;
    co.container = &container;
    if (gtext_css_conditions_new(w->registry, &co, &w->conditions) != GTEXT_CSS_OK || gtext_cssom_document_set_conditions(w->doc, w->conditions, NULL) != GTEXT_CSS_OK) {
      return 0;
    }
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

static void obj_release(object * o);

static void world_close(world * w) {
  for (size_t i = 0; i < w->nobjs; i++) {
    obj_release(&w->objs[i]);
    free(w->objs[i].name);
  }
  free(w->objs);
  for (size_t i = 0; i < w->nstyles; i++) {
    gtext_cssom_decl_release(w->styles[i].decl);
    free(w->styles[i].name);
    free(w->styles[i].elem_name);
    free(w->styles[i].pseudo);
  }
  for (size_t i = 0; i < w->nelems; i++) {
    free(w->elems[i].name);
  }
  for (size_t i = 0; i < w->nlists; i++) {
    free(w->lists[i].name);
    free(w->lists[i].nodes);
  }
  free(w->lists);
  for (size_t i = 0; i < w->nregs; i++) {
    free(w->regs[i].name);
    value_free(&w->regs[i].v);
  }
  free(w->elems);
  free(w->styles);
  free(w->regs);
  gtext_cssom_markup_binding_free(w->binding);
  gtext_cssom_document_release(w->doc);
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

static nodelist * find_list(world * w, const char * name) {
  for (size_t i = w->nlists; i-- > 0;) {
    if (strcmp(w->lists[i].name, name) == 0) {
      return &w->lists[i];
    }
  }
  return NULL;
}

/* takes ownership of `nodes` */
static void put_list(world * w, const char * name, GTEXT_Markup_Node ** nodes, size_t n) {
  nodelist * l = find_list(w, name);
  if (l) {
    free(l->nodes);
  }
  else {
    if (w->nlists == w->caplists) {
      w->caplists = w->caplists ? w->caplists * 2 : 8;
      w->lists = realloc(w->lists, w->caplists * sizeof *w->lists);
    }
    l = &w->lists[w->nlists++];
    l->name = dups(name);
  }
  l->nodes = nodes;
  l->n = n;
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
static object * find_obj(world * w, const char * name);

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
    case 'o': {
      /* a sheet, a rule or a media list: two lookups that found the same object are the same object */
      object * ob = find_obj(w, f + 2);
      if (ob) {
        if (ob->state != 0 || !ob->ptr) {
          return val(ob->state == 1 ? 'N' : 'u', "", 0);
        }
        char id[32];
        snprintf(id, sizeof id, "%p", ob->ptr);
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
  int timeline;
  char * timeline_property;
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
  if (!s->computed || (t->layout && t->timeline)) {
    return;
  }
  GTEXT_CSSOM_Origin origin = GTEXT_CSSOM_ORIGIN_COMPUTED;
  bool found = false;
  if (gtext_cssom_computed_origin(s->decl, css_name, strlen(css_name), &origin, &found) == GTEXT_CSS_OK && found && origin == GTEXT_CSSOM_ORIGIN_ABSENT_LAYOUT && !t->layout) {
    t->layout = 1;
    t->layout_property = dups(css_name);
  }
  if (found && origin == GTEXT_CSSOM_ORIGIN_ABSENT_TIMELINE && !t->timeline) {
    t->timeline = 1;
    t->timeline_property = dups(css_name);
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
  {
    /* an exception the page's script would catch is written "Name: what", which is what assert_throws_dom() is given */
    const char * exception = gtext_cssom_exception_name(st);
    if (exception) {
      return fmt("%s: %s", exception, what);
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

/* ---- the document ops: creating, inserting, removing and reading nodes ------------------------------------------------------------------------ */

static char * lower_ascii(const char * s, size_t n) {
  char * d = dupn(s, n);
  for (char * p = d; *p; p++) {
    if (*p >= 'A' && *p <= 'Z') {
      *p = (char)(*p + 32);
    }
  }
  return d;
}

/* the exception a Markup status stands for, as the page's script would see it */
static char * markup_exception(const char * what, GTEXT_Markup_Status ms) {
  switch (ms) {
  case GTEXT_MARKUP_E_HIERARCHY:
    return fmt("HierarchyRequestError: %s: the new child cannot be inserted there", what);
  case GTEXT_MARKUP_E_NAME:
    return fmt("InvalidCharacterError: %s: the name is not valid", what);
  case GTEXT_MARKUP_E_WRONG_DOCUMENT:
    return fmt("WrongDocumentError: %s", what);
  default:
    return fmt("%s failed: %d", what, (int)ms);
  }
}

/* after a change of the tree the binding re-reads the style elements (it returns at once when nothing changed) */
static void sync_world(world * w) {
  if (w->binding) {
    gtext_cssom_markup_sync(w->binding, NULL);
  }
}

static int node_is_inclusive_ancestor(const GTEXT_Markup_Node * a, const GTEXT_Markup_Node * n) {
  for (; n; n = gtext_markup_node_parent(n)) {
    if (n == a) {
      return 1;
    }
  }
  return 0;
}

static int node_connected(world * w, const GTEXT_Markup_Node * n) {
  return node_is_inclusive_ancestor(gtext_markup_document_root(w->markup), n);
}

/* An argument that is a node or a string (the argument list of append(), before(), replaceChildren()...): a string is made a text node, null is the text "null". Returns the node. */
static GTEXT_Markup_Node * argument_node(world * w, const char * f, size_t l, char ** failure) {
  *failure = NULL;
  GTEXT_Markup_Node * n = NULL;
  if (l >= 2 && f[0] == 'e' && f[1] == ':') {
    elem * e = find_elem(w, f + 2);
    if (!e) {
      *failure = dups("the op names an element the program did not make");
      return NULL;
    }
    if (e->node) {
      return e->node;
    }
    GTEXT_Markup_Status ms = gtext_markup_create_text(w->markup, "null", 4, &n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("createTextNode", ms);
    }
    return n;
  }
  if (l >= 2 && f[0] == 's' && f[1] == ':') {
    GTEXT_Markup_Status ms = gtext_markup_create_text(w->markup, f + 2, l - 2, &n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("createTextNode", ms);
    }
    return n;
  }
  *failure = dups("the argument of the op is neither a node nor a string");
  return NULL;
}

/* "convert nodes into a node": one node as it is, several in a fragment */
static GTEXT_Markup_Node * argument_nodes(world * w, const op * o, size_t first, char ** failure) {
  *failure = NULL;
  if (o->n == first + 1) {
    return argument_node(w, o->f[first], o->l[first], failure);
  }
  GTEXT_Markup_Node * frag = NULL;
  GTEXT_Markup_Status ms = gtext_markup_create_fragment(w->markup, &frag);
  if (ms != GTEXT_MARKUP_OK) {
    *failure = markup_exception("createDocumentFragment", ms);
    return NULL;
  }
  for (size_t i = first; i < o->n; i++) {
    GTEXT_Markup_Node * n = argument_node(w, o->f[i], o->l[i], failure);
    if (!n) {
      return NULL;
    }
    ms = gtext_markup_append_child(frag, n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("append", ms);
      return NULL;
    }
  }
  return frag;
}

static const char * node_name_of(const GTEXT_Markup_Node * n, char ** owned) {
  *owned = NULL;
  switch (gtext_markup_node_kind(n)) {
  case GTEXT_MARKUP_KIND_DOCUMENT:
    return "#document";
  case GTEXT_MARKUP_KIND_FRAGMENT:
    return "#document-fragment";
  case GTEXT_MARKUP_KIND_TEXT:
    return "#text";
  case GTEXT_MARKUP_KIND_CDATA:
    return "#cdata-section";
  case GTEXT_MARKUP_KIND_COMMENT:
    return "#comment";
  case GTEXT_MARKUP_KIND_DOCTYPE: {
    const char * d = gtext_markup_doctype_name(n);
    return d ? d : "";
  }
  case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
    const char * d = gtext_markup_pi_target(n);
    return d ? d : "";
  }
  case GTEXT_MARKUP_KIND_ELEMENT: {
    /* the qualified name; an element of the HTML namespace in an HTML document is upper case */
    const char * local = gtext_markup_element_local_name(n);
    const char * prefix = gtext_markup_element_prefix(n);
    const char * ns = gtext_markup_element_namespace(n);
    char * q = prefix && *prefix ? fmt("%s:%s", prefix, local) : dups(local);
    if (ns && strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0) {
      for (char * p = q; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
          *p = (char)(*p - 32);
        }
      }
    }
    *owned = q;
    return q;
  }
  default:
    return "";
  }
}

static int node_type_of(const GTEXT_Markup_Node * n) {
  switch (gtext_markup_node_kind(n)) {
  case GTEXT_MARKUP_KIND_ELEMENT:
    return 1;
  case GTEXT_MARKUP_KIND_TEXT:
    return 3;
  case GTEXT_MARKUP_KIND_CDATA:
    return 4;
  case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
    return 7;
  case GTEXT_MARKUP_KIND_COMMENT:
    return 8;
  case GTEXT_MARKUP_KIND_DOCUMENT:
    return 9;
  case GTEXT_MARKUP_KIND_DOCTYPE:
    return 10;
  case GTEXT_MARKUP_KIND_FRAGMENT:
    return 11;
  default:
    return 0;
  }
}

static int is_character_data(const GTEXT_Markup_Node * n) {
  const GTEXT_Markup_Kind k = gtext_markup_node_kind(n);
  return k == GTEXT_MARKUP_KIND_TEXT || k == GTEXT_MARKUP_KIND_CDATA || k == GTEXT_MARKUP_KIND_COMMENT || k == GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION;
}

static void put_bool(world * w, const char * reg, int b) {
  put_reg(w, reg, val('b', b ? "true" : "false", b ? 4 : 5));
}

static void put_num(world * w, const char * reg, size_t n) {
  char buf[32];
  snprintf(buf, sizeof buf, "%zu", n);
  put_reg(w, reg, val('n', buf, strlen(buf)));
}

static GTEXT_Markup_Node * first_child_of(GTEXT_Markup_Node * n) {
  return gtext_markup_node_first_child(n);
}

/* the children of an element, in order, as a list to hold while they are moved */
static GTEXT_Markup_Node ** child_list(GTEXT_Markup_Node * parent, size_t * count) {
  size_t n = 0;
  for (GTEXT_Markup_Node * c = first_child_of(parent); c; c = gtext_markup_node_next_sibling(c)) {
    n++;
  }
  GTEXT_Markup_Node ** list = malloc((n ? n : 1) * sizeof *list);
  size_t i = 0;
  for (GTEXT_Markup_Node * c = first_child_of(parent); c; c = gtext_markup_node_next_sibling(c)) {
    list[i++] = c;
  }
  *count = n;
  return list;
}

/* Replace all the children of `parent` with `node` (NULL: with nothing), as the DOM's "replace all": on a refusal the tree is as it was. */
static char * replace_all(GTEXT_Markup_Node * parent, GTEXT_Markup_Node * node, const char * what) {
  size_t count;
  GTEXT_Markup_Node ** old = child_list(parent, &count);
  for (size_t i = 0; i < count; i++) {
    gtext_markup_remove(old[i]);
  }
  GTEXT_Markup_Status ms = node ? gtext_markup_append_child(parent, node) : GTEXT_MARKUP_OK;
  if (ms != GTEXT_MARKUP_OK) {
    for (size_t i = 0; i < count; i++) {
      gtext_markup_append_child(parent, old[i]);
    }
    free(old);
    return markup_exception(what, ms);
  }
  free(old);
  return NULL;
}

static GTEXT_HTML_Namespace html_namespace_of(const GTEXT_Markup_Node * el) {
  const char * ns = gtext_markup_element_namespace(el);
  if (ns && strcmp(ns, "http://www.w3.org/2000/svg") == 0) {
    return GTEXT_HTML_NS_SVG;
  }
  if (ns && strcmp(ns, "http://www.w3.org/1998/Math/MathML") == 0) {
    return GTEXT_HTML_NS_MATHML;
  }
  return GTEXT_HTML_NS_HTML;
}

/* innerHTML = text: the fragment parsing algorithm in the context of the element, the result cloned into the document and put in place of the children */
static char * set_inner_html(world * w, GTEXT_Markup_Node * el, const char * text, size_t len) {
  GTEXT_HTML_Fragment_Context ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.ns = html_namespace_of(el);
  ctx.local_name = gtext_markup_element_local_name(el);
  ctx.quirks = gtext_markup_document_mode(w->markup);
  for (const GTEXT_Markup_Node * p = el; p; p = gtext_markup_node_parent(p)) {
    if (gtext_markup_node_kind(p) == GTEXT_MARKUP_KIND_ELEMENT && strcmp(gtext_markup_element_local_name(p), "form") == 0) {
      ctx.form_ancestor = true;
    }
  }
  GTEXT_Markup_Document * scratch = NULL;
  GTEXT_Markup_Node * frag = NULL;
  GTEXT_HTML_Result hr;
  memset(&hr, 0, sizeof hr);
  GTEXT_HTML_Status hs = gtext_html_parse_fragment(&ctx, text, len, NULL, &scratch, &frag, &hr);
  gtext_html_result_free(&hr);
  if (hs != GTEXT_HTML_OK) {
    return fmt("innerHTML: the fragment parse failed: %d", (int)hs);
  }
  GTEXT_Markup_Node * copy = NULL;
  GTEXT_Markup_Status ms = gtext_markup_clone(w->markup, frag, true, &copy);
  gtext_markup_document_free(scratch);
  if (ms != GTEXT_MARKUP_OK) {
    return markup_exception("innerHTML", ms);
  }
  GTEXT_Markup_Node * target = el;
  if (strcmp(gtext_markup_element_local_name(el), "template") == 0 && gtext_markup_element_template_content(el)) {
    target = gtext_markup_element_template_content(el);
  }
  return replace_all(target, copy, "innerHTML");
}

static char * serialize_to_reg(world * w, const char * reg, GTEXT_Markup_Node * n, GTEXT_HTML_Serialize_Scope scope) {
  char * out = NULL;
  size_t len = 0;
  GTEXT_HTML_Serialize_Result sr;
  memset(&sr, 0, sizeof sr);
  GTEXT_HTML_Status hs = gtext_html_serialize_buffer(n, scope, NULL, &out, &len, &sr);
  if (hs != GTEXT_HTML_OK) {
    return fmt("serialization failed: %d", (int)hs);
  }
  put_reg(w, reg, val('s', out, len));
  gtext_html_serialize_buffer_free(w->markup, NULL, out);
  return NULL;
}

/* A node's own place among the lists a script reads: the child at INDEX of the elements (`children`) or of all the nodes (`childNodes`), and how many. */
static GTEXT_Markup_Node * child_at(GTEXT_Markup_Node * parent, int elements_only, size_t index, size_t * total) {
  size_t i = 0;
  GTEXT_Markup_Node * found = NULL;
  for (GTEXT_Markup_Node * c = first_child_of(parent); c; c = gtext_markup_node_next_sibling(c)) {
    if (elements_only && !is_element(c)) {
      continue;
    }
    if (i == index && !found) {
      found = c;
    }
    i++;
  }
  if (total) {
    *total = i;
  }
  return found;
}

/* The document ops. Returns 1 when the op was one of these (its failure, if any, in *failure). */
static int dom_op(world * w, const op * o, int * program_error, char ** failure) {
  const char * name = o->f[0];
  *failure = NULL;
#define NEEDN(n_) do { if (o->n != (n_)) { *program_error = 1; *failure = fmt("the op %s has %zu fields, not %d", name, o->n, (n_)); return 1; } } while (0)
#define NODE(var, fld) elem * var = find_elem(w, o->f[fld]); if (!var) { *program_error = 1; *failure = fmt("the op %s names the node %s, which the program did not make", name, o->f[fld]); return 1; } \
  if (!var->node) { *failure = dups("TypeError: the node is null"); return 1; }
  GTEXT_Markup_Status ms;
  if (strcmp(name, "create") == 0) {
    NEEDN(4);   /* create E TAG NAMESPACE (the namespace is "" for createElement in an HTML document) */
    char * tag = o->f[3][0] ? dupn(o->f[2], o->l[2]) : lower_ascii(o->f[2], o->l[2]);
    GTEXT_Markup_Node * n = NULL;
    if (!gtext_markup_is_xml_name(tag, strlen(tag))) {
      *failure = fmt("InvalidCharacterError: createElement: the name \"%s\" is not valid", tag);
    }
    else {
      ms = gtext_markup_create_element(w->markup, o->f[3][0] ? o->f[3] : GTEXT_MARKUP_NS_HTML, NULL, tag, &n);
      if (ms != GTEXT_MARKUP_OK) {
        *failure = markup_exception("createElement", ms);
      }
      else {
        put_elem(w, o->f[1], n);
      }
    }
    free(tag);
    return 1;
  }
  if (strcmp(name, "ctext") == 0 || strcmp(name, "ccomment") == 0) {
    NEEDN(3);
    GTEXT_Markup_Node * n = NULL;
    ms = name[1] == 't' ? gtext_markup_create_text(w->markup, o->f[2], o->l[2], &n) : gtext_markup_create_comment(w->markup, o->f[2], o->l[2], &n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception(name[1] == 't' ? "createTextNode" : "createComment", ms);
    }
    else {
      put_elem(w, o->f[1], n);
    }
    return 1;
  }
  if (strcmp(name, "cfrag") == 0) {
    NEEDN(2);
    GTEXT_Markup_Node * n = NULL;
    ms = gtext_markup_create_fragment(w->markup, &n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("createDocumentFragment", ms);
    }
    else {
      put_elem(w, o->f[1], n);
    }
    return 1;
  }
  if (strcmp(name, "clone") == 0) {
    NEEDN(4);   /* clone E SRC DEEP */
    NODE(src, 2);
    GTEXT_Markup_Node * n = NULL;
    ms = gtext_markup_clone(w->markup, src->node, strcmp(o->f[3], "true") == 0, &n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("cloneNode", ms);
    }
    else {
      put_elem(w, o->f[1], n);
    }
    return 1;
  }
  if (strcmp(name, "append") == 0 || strcmp(name, "prepend") == 0 || strcmp(name, "replacechildren") == 0) {
    if (o->n < 2) {
      *program_error = 1;
      *failure = dups("the op has no parent");
      return 1;
    }
    NODE(parent, 1);
    GTEXT_Markup_Node * node = NULL;
    if (o->n > 2) {
      node = argument_nodes(w, o, 2, failure);
      if (!node) {
        return 1;
      }
    }
    if (name[0] == 'r') {
      *failure = replace_all(parent->node, node, "replaceChildren");
    }
    else if (node) {
      ms = gtext_markup_insert_before(parent->node, node, name[0] == 'p' ? first_child_of(parent->node) : NULL);
      if (ms != GTEXT_MARKUP_OK) {
        *failure = markup_exception(name, ms);
      }
    }
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "appendchild") == 0 || strcmp(name, "insertbefore") == 0) {
    const int ib = name[0] == 'i';
    NEEDN(ib ? 4 : 3);
    NODE(parent, 1);
    elem * child = find_elem(w, o->f[2]);
    if (!child) {
      *program_error = 1;
      *failure = fmt("the op %s names the node %s, which the program did not make", name, o->f[2]);
      return 1;
    }
    if (!child->node) {
      *failure = dups("TypeError: Failed to execute on 'Node': parameter 1 is not of type 'Node'.");
      return 1;
    }
    GTEXT_Markup_Node * ref = NULL;
    if (ib && strcmp(o->f[3], "N:") != 0) {
      elem * r = find_elem(w, o->f[3] + 2);
      if (!r) {
        *program_error = 1;
        *failure = dups("insertbefore names a reference the program did not make");
        return 1;
      }
      ref = r->node;
    }
    if (ref && gtext_markup_node_parent(ref) != parent->node) {
      *failure = dups("NotFoundError: insertBefore: the reference node is not a child of this node");
      return 1;
    }
    ms = gtext_markup_insert_before(parent->node, child->node, ref);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception(ib ? "insertBefore" : "appendChild", ms);
    }
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "removechild") == 0) {
    NEEDN(3);
    NODE(parent, 1);
    NODE(child, 2);
    if (gtext_markup_node_parent(child->node) != parent->node) {
      *failure = dups("NotFoundError: removeChild: the node is not a child of this node");
      return 1;
    }
    gtext_markup_remove(child->node);
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "replacechild") == 0) {
    NEEDN(4);   /* replacechild PARENT NEW OLD */
    NODE(parent, 1);
    NODE(nw, 2);
    NODE(old, 3);
    if (gtext_markup_node_parent(old->node) != parent->node) {
      *failure = dups("NotFoundError: replaceChild: the node is not a child of this node");
      return 1;
    }
    ms = gtext_markup_replace(old->node, nw->node);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("replaceChild", ms);
    }
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "remove") == 0) {
    NEEDN(2);
    NODE(n, 1);
    gtext_markup_remove(n->node);
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "before") == 0 || strcmp(name, "after") == 0 || strcmp(name, "replacewith") == 0) {
    if (o->n < 2) {
      *program_error = 1;
      *failure = dups("the op has no node");
      return 1;
    }
    NODE(n, 1);
    GTEXT_Markup_Node * parent = gtext_markup_node_parent(n->node);
    if (!parent) {
      return 1;
    }
    /* the nodes of the arguments, to tell a viable sibling from one of them */
    GTEXT_Markup_Node * ref = NULL;
    const int is_arg = 0;
    (void)is_arg;
    if (name[0] == 'b') {
      ref = n->node;
      for (;;) {
        GTEXT_Markup_Node * prev = gtext_markup_node_previous_sibling(ref);
        int arg = 0;
        for (size_t i = 2; prev && i < o->n; i++) {
          if (o->l[i] >= 2 && o->f[i][0] == 'e') {
            elem * e = find_elem(w, o->f[i] + 2);
            arg |= e && e->node == prev;
          }
        }
        if (!arg) {
          break;
        }
        ref = prev;
      }
      ref = gtext_markup_node_previous_sibling(ref);
      ref = ref ? gtext_markup_node_next_sibling(ref) : first_child_of(parent);
    }
    else if (name[0] == 'a') {
      ref = gtext_markup_node_next_sibling(n->node);
      for (;;) {
        int arg = 0;
        for (size_t i = 2; ref && i < o->n; i++) {
          if (o->l[i] >= 2 && o->f[i][0] == 'e') {
            elem * e = find_elem(w, o->f[i] + 2);
            arg |= e && e->node == ref;
          }
        }
        if (!arg) {
          break;
        }
        ref = gtext_markup_node_next_sibling(ref);
      }
    }
    GTEXT_Markup_Node * node = NULL;
    if (o->n > 2) {
      node = argument_nodes(w, o, 2, failure);
      if (!node) {
        return 1;
      }
    }
    if (name[0] == 'r') {
      if (node) {
        ms = gtext_markup_replace(n->node, node);
        if (ms != GTEXT_MARKUP_OK) {
          *failure = markup_exception("replaceWith", ms);
        }
      }
      else {
        gtext_markup_remove(n->node);
      }
    }
    else if (node) {
      ms = gtext_markup_insert_before(parent, node, ref);
      if (ms != GTEXT_MARKUP_OK) {
        *failure = markup_exception(name, ms);
      }
    }
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "tcget") == 0) {
    NEEDN(3);
    NODE(n, 2);
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(n->node);
    if (k == GTEXT_MARKUP_KIND_DOCUMENT || k == GTEXT_MARKUP_KIND_DOCTYPE) {
      put_reg(w, o->f[1], val('N', "", 0));
    }
    else if (is_character_data(n->node)) {
      size_t len = 0;
      const char * d = gtext_markup_node_data(n->node, &len);
      put_reg(w, o->f[1], val('s', d ? d : "", d ? len : 0));
    }
    else {
      char * text = NULL;
      size_t len = 0;
      ms = gtext_markup_node_text_content(n->node, NULL, &text, &len);
      if (ms != GTEXT_MARKUP_OK) {
        *failure = markup_exception("textContent", ms);
        return 1;
      }
      put_reg(w, o->f[1], val('s', text, len));
      gtext_markup_string_free(NULL, text);
    }
    return 1;
  }
  if (strcmp(name, "tcset") == 0 || strcmp(name, "dset") == 0) {
    NEEDN(3);
    NODE(n, 1);
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(n->node);
    if (name[0] == 'd' && !is_character_data(n->node)) {
      return 1;   /* `data` is an ordinary property of any other node: an expando of the script object */
    }
    if (is_character_data(n->node)) {
      ms = gtext_markup_node_set_data(n->node, o->f[2], o->l[2]);
      if (ms != GTEXT_MARKUP_OK) {
        *failure = markup_exception("textContent", ms);
      }
    }
    else if (k == GTEXT_MARKUP_KIND_ELEMENT || k == GTEXT_MARKUP_KIND_FRAGMENT) {
      GTEXT_Markup_Node * text = NULL;
      if (o->l[2]) {
        ms = gtext_markup_create_text(w->markup, o->f[2], o->l[2], &text);
        if (ms != GTEXT_MARKUP_OK) {
          *failure = markup_exception("textContent", ms);
          return 1;
        }
      }
      *failure = replace_all(n->node, text, "textContent");
    }
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "ihget") == 0 || strcmp(name, "ohget") == 0) {
    NEEDN(3);
    NODE(n, 2);
    *failure = serialize_to_reg(w, o->f[1], n->node, name[0] == 'i' ? GTEXT_HTML_SERIALIZE_CHILDREN : GTEXT_HTML_SERIALIZE_OUTER);
    return 1;
  }
  if (strcmp(name, "ihset") == 0) {
    NEEDN(3);
    NODE(n, 1);
    *failure = set_inner_html(w, n->node, o->f[2], o->l[2]);
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "nread") == 0) {
    NEEDN(4);   /* nread R NODE WHAT */
    NODE(n, 2);
    const char * what = o->f[3];
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(n->node);
    if (strcmp(what, "nodeType") == 0) {
      put_num(w, o->f[1], (size_t)node_type_of(n->node));
    }
    else if (strcmp(what, "nodeName") == 0) {
      char * owned;
      const char * s = node_name_of(n->node, &owned);
      put_reg(w, o->f[1], val('s', s, strlen(s)));
      free(owned);
    }
    else if (strcmp(what, "tagName") == 0 || strcmp(what, "localName") == 0 || strcmp(what, "namespaceURI") == 0) {
      /* undefined on a node that is not an element (localName and namespaceURI are null on other nodes, tagName is not there) */
      if (k != GTEXT_MARKUP_KIND_ELEMENT) {
        put_reg(w, o->f[1], what[0] == 't' ? val('u', "", 0) : val('N', "", 0));
      }
      else if (what[0] == 't') {
        char * owned;
        const char * s = node_name_of(n->node, &owned);
        put_reg(w, o->f[1], val('s', s, strlen(s)));
        free(owned);
      }
      else if (what[0] == 'l') {
        const char * s = gtext_markup_element_local_name(n->node);
        put_reg(w, o->f[1], val('s', s, strlen(s)));
      }
      else {
        const char * s = gtext_markup_element_namespace(n->node);
        put_reg(w, o->f[1], s ? val('s', s, strlen(s)) : val('N', "", 0));
      }
    }
    else if (strcmp(what, "nodeValue") == 0 || strcmp(what, "data") == 0) {
      if (is_character_data(n->node)) {
        size_t len = 0;
        const char * d = gtext_markup_node_data(n->node, &len);
        put_reg(w, o->f[1], val('s', d ? d : "", d ? len : 0));
      }
      else {
        put_reg(w, o->f[1], what[0] == 'n' ? val('N', "", 0) : val('u', "", 0));
      }
    }
    else if (strcmp(what, "isConnected") == 0) {
      put_bool(w, o->f[1], node_connected(w, n->node));
    }
    else if (strcmp(what, "hasChildNodes") == 0) {
      put_bool(w, o->f[1], first_child_of(n->node) != NULL);
    }
    else if (strcmp(what, "childElementCount") == 0) {
      size_t total = 0;
      child_at(n->node, 1, (size_t)-1, &total);
      put_num(w, o->f[1], total);
    }
    else {
      *program_error = 1;
      *failure = fmt("nread %s is not known", what);
    }
    return 1;
  }
  if (strcmp(name, "contains") == 0) {
    NEEDN(4);   /* contains R A B (B may be null: false) */
    NODE(a, 2);
    elem * b = find_elem(w, o->f[3]);
    if (!b) {
      *program_error = 1;
      *failure = dups("contains names a node the program did not make");
      return 1;
    }
    put_bool(w, o->f[1], b->node && node_is_inclusive_ancestor(a->node, b->node));
    return 1;
  }
  if (strcmp(name, "matches") == 0 || strcmp(name, "closest") == 0) {
    NEEDN(4);   /* matches R E SEL; closest E2 E SEL */
    NODE(e, 2);
    if (!is_element(e->node)) {
      *program_error = 1;
      *failure = dups("matches on a node that is not an element");
      return 1;
    }
    bool matched = false;
    GTEXT_Markup_Node * found = NULL;
    if (name[0] == 'm') {
      ms = gtext_markup_matches(e->node, o->f[3], o->l[3], NULL, &matched, NULL);
    }
    else {
      ms = gtext_markup_closest(e->node, o->f[3], o->l[3], NULL, &found, NULL);
    }
    if (ms == GTEXT_MARKUP_E_SELECTOR) {
      *failure = dups("SyntaxError: the selector is not valid");
      return 1;
    }
    if (ms != GTEXT_MARKUP_OK) {
      *failure = fmt("%s failed: %d", name, (int)ms);
      return 1;
    }
    if (name[0] == 'm') {
      put_bool(w, o->f[1], matched);
    }
    else {
      put_elem(w, o->f[1], found);
    }
    return 1;
  }
  if (strcmp(name, "qsa") == 0) {
    NEEDN(4);   /* qsa L ROOT SELECTOR: a static NodeList */
    const GTEXT_Markup_Node * scope = gtext_markup_document_root(w->markup);
    if (strcmp(o->f[2], "doc") != 0) {
      NODE(r, 2);
      scope = r->node;
    }
    GTEXT_Markup_Node ** nodes = NULL;
    size_t count = 0;
    ms = gtext_markup_query_selector_all(scope, o->f[3], o->l[3], NULL, &nodes, &count, NULL);
    if (ms == GTEXT_MARKUP_E_SELECTOR) {
      *failure = dups("SyntaxError: the selector is not valid");
      return 1;
    }
    if (ms != GTEXT_MARKUP_OK) {
      *failure = fmt("querySelectorAll failed: %d", (int)ms);
      return 1;
    }
    GTEXT_Markup_Node ** own = malloc((count ? count : 1) * sizeof *own);
    memcpy(own, nodes, count * sizeof *own);
    gtext_markup_query_free(NULL, nodes);
    put_list(w, o->f[1], own, count);
    return 1;
  }
  if (strcmp(name, "llen") == 0 || strcmp(name, "lget") == 0) {
    NEEDN(name[1] == 'l' ? 3 : 4);   /* llen R L; lget E L INDEX */
    nodelist * l = find_list(w, o->f[2]);
    if (!l) {
      *program_error = 1;
      *failure = fmt("the op %s names the list %s, which the program did not make", name, o->f[2]);
      return 1;
    }
    if (name[1] == 'l') {
      put_num(w, o->f[1], l->n);
    }
    else {
      const size_t idx = (size_t)strtoull(o->f[3], NULL, 10);
      put_elem(w, o->f[1], idx < l->n ? l->nodes[idx] : NULL);
    }
    return 1;
  }
  if (strcmp(name, "qlen") == 0) {
    NEEDN(4);   /* qlen R ROOT SELECTOR */
    const GTEXT_Markup_Node * scope = gtext_markup_document_root(w->markup);
    if (strcmp(o->f[2], "doc") != 0) {
      NODE(r, 2);
      scope = r->node;
    }
    GTEXT_Markup_Node ** nodes = NULL;
    size_t count = 0;
    ms = gtext_markup_query_selector_all(scope, o->f[3], o->l[3], NULL, &nodes, &count, NULL);
    if (ms == GTEXT_MARKUP_E_SELECTOR) {
      *failure = dups("SyntaxError: the selector is not valid");
      return 1;
    }
    if (ms != GTEXT_MARKUP_OK) {
      *failure = fmt("querySelectorAll failed: %d", (int)ms);
      return 1;
    }
    gtext_markup_query_free(NULL, nodes);
    put_num(w, o->f[1], count);
    return 1;
  }
  if (strcmp(name, "child") == 0 || strcmp(name, "nchildren") == 0) {
    /* child E PARENT elements|nodes INDEX; nchildren R PARENT elements|nodes */
    const int len_op = name[0] == 'n';
    NEEDN(len_op ? 4 : 5);
    NODE(p, 2);
    const int only_elements = strcmp(o->f[3], "elements") == 0;
    size_t total = 0;
    if (len_op) {
      child_at(p->node, only_elements, (size_t)-1, &total);
      put_num(w, o->f[1], total);
    }
    else {
      put_elem(w, o->f[1], child_at(p->node, only_elements, (size_t)strtoull(o->f[4], NULL, 10), &total));
    }
    return 1;
  }
  return 0;
#undef NEEDN
#undef NODE
}

/* ---- the style sheet ops: sheets, rules, rule lists and media lists as objects of the page ------------------------------------------------------- */

enum { OBJ_SHEET = 1, OBJ_RULE, OBJ_MEDIA };
enum { ST_OBJECT = 0, ST_NULL, ST_UNDEFINED };

static void obj_release(object * o) {
  if (o->state == ST_OBJECT && o->ptr) {
    if (o->kind == OBJ_SHEET) {
      gtext_cssom_sheet_release(o->ptr);
    }
    else if (o->kind == OBJ_RULE) {
      gtext_cssom_rule_release(o->ptr);
    }
    else if (o->kind == OBJ_MEDIA) {
      gtext_cssom_media_release(o->ptr);
    }
  }
  o->ptr = NULL;
}

static object * find_obj(world * w, const char * name) {
  for (size_t i = w->nobjs; i-- > 0;) {
    if (strcmp(w->objs[i].name, name) == 0) {
      return &w->objs[i];
    }
  }
  return NULL;
}

/* takes the reference the caller holds (ptr may be NULL for null and undefined) */
static void put_obj(world * w, const char * name, int kind, int state, void * ptr) {
  object * o = find_obj(w, name);
  if (o) {
    obj_release(o);
  }
  else {
    if (w->nobjs == w->capobjs) {
      w->capobjs = w->capobjs ? w->capobjs * 2 : 16;
      w->objs = realloc(w->objs, w->capobjs * sizeof *w->objs);
    }
    o = &w->objs[w->nobjs++];
    o->name = dups(name);
  }
  o->kind = kind;
  o->state = ptr ? ST_OBJECT : state;
  o->ptr = ptr;
}

static void put_found(world * w, const char * name, int kind, void * ptr, int absent) {
  put_obj(w, name, kind, ptr ? ST_OBJECT : absent, ptr);
}

/* the exception a status stands for, as the page's script would see it: "Name: what" */
static char * css_exception(const char * what, GTEXT_CSS_Status st) {
  const char * n = gtext_cssom_exception_name(st);
  if (n) {
    return fmt("%s: %s", n, what);
  }
  return fmt("%s: %s", what, gtext_css_status_string(st));
}

static GTEXT_CSSOM_Scope * world_scope(world * w) {
  return gtext_cssom_document_scope(w->doc);
}

/* a sheet's or rule's list of rules; NULL when the object has none (a rule that is not a grouping rule) */
static GTEXT_CSS_Status rule_list_of(object * o, const GTEXT_CSSOM_Rule_List ** list) {
  *list = NULL;
  if (o->kind == OBJ_SHEET) {
    return gtext_cssom_sheet_rules(o->ptr, list);
  }
  *list = gtext_cssom_rule_css_rules(o->ptr);
  return GTEXT_CSS_OK;
}

static int is_style_or_link(const GTEXT_Markup_Node * n) {
  const char * l = gtext_markup_element_local_name(n);
  const char * ns = gtext_markup_element_namespace(n);
  return l && ns && strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0 && (strcmp(l, "style") == 0 || strcmp(l, "link") == 0);
}

/* The sheet ops. Returns 1 when the op was one of these (its failure, if any, in *failure). */
static int sheet_op(world * w, const op * o, int * program_error, char ** failure) {
  const char * name = o->f[0];
  *failure = NULL;
#define NEEDS(n_) do { if (o->n != (n_)) { *program_error = 1; *failure = fmt("the op %s has %zu fields, not %d", name, o->n, (n_)); return 1; } } while (0)
#define OBJ(var, fld, k_) object * var = find_obj(w, o->f[fld]); \
  if (!var) { *program_error = 1; *failure = fmt("the op %s names the object %s, which the program did not make", name, o->f[fld]); return 1; } \
  if (var->state != ST_OBJECT) { *failure = fmt("TypeError: %s of %s", name, var->state == ST_NULL ? "null" : "undefined"); return 1; } \
  if (var->kind != (k_) && (k_) != 0) { *failure = fmt("TypeError: %s of an object of another kind", name); return 1; }
  GTEXT_CSS_Status st;
  GTEXT_CSSOM_Outcome oc;
  const int absent_default = ST_UNDEFINED;
  (void)absent_default;
  if (strcmp(name, "sheetof") == 0) {
    NEEDS(3);   /* sheetof O E: element.sheet: null for a style or link element without one, undefined on any other element */
    elem * e = find_elem(w, o->f[2]);
    if (!e) {
      *program_error = 1;
      *failure = dups("sheetof names an element the program did not make");
      return 1;
    }
    if (!e->node) {
      *failure = dups("TypeError: the element is null");
      return 1;
    }
    sync_world(w);
    if (!is_element(e->node) || !is_style_or_link(e->node)) {
      put_obj(w, o->f[1], OBJ_SHEET, ST_UNDEFINED, NULL);
    }
    else {
      put_found(w, o->f[1], OBJ_SHEET, gtext_cssom_markup_sheet_of(w->binding, e->node), ST_NULL);
    }
    return 1;
  }
  if (strcmp(name, "nsheets") == 0) {
    NEEDS(2);
    sync_world(w);
    put_num(w, o->f[1], gtext_cssom_scope_sheet_count(world_scope(w)));
    return 1;
  }
  if (strcmp(name, "sheetat") == 0) {
    NEEDS(4);   /* sheetat O INDEX null|undefined */
    sync_world(w);
    put_found(w, o->f[1], OBJ_SHEET, gtext_cssom_scope_sheet_at(world_scope(w), (size_t)strtoull(o->f[2], NULL, 10)), o->f[3][0] == 'n' ? ST_NULL : ST_UNDEFINED);
    return 1;
  }
  if (strcmp(name, "newsheet") == 0) {
    NEEDS(4);   /* newsheet O MEDIA DISABLED: new CSSStyleSheet({media, disabled}) */
    GTEXT_CSSOM_Sheet_Options so = gtext_cssom_sheet_options_default();
    if (o->f[2][0] == 's') {
      so.media = o->f[2] + 2;
    }
    so.disabled = strcmp(o->f[3], "b:true") == 0;
    GTEXT_CSSOM_Sheet * sh = NULL;
    st = gtext_cssom_sheet_new_constructed(w->doc, &so, &sh);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("new CSSStyleSheet", st);
      return 1;
    }
    put_obj(w, o->f[1], OBJ_SHEET, ST_OBJECT, sh);
    return 1;
  }
  if (strcmp(name, "replacesync") == 0) {
    NEEDS(3);
    OBJ(sh, 1, OBJ_SHEET);
    st = gtext_cssom_sheet_replace_sync(sh->ptr, o->f[2], o->l[2], &oc);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("replaceSync", st);
    }
    return 1;
  }
  if (strcmp(name, "rlen") == 0) {
    NEEDS(3);   /* rlen R OBJ: cssRules.length */
    OBJ(ob, 2, 0);
    if (ob->kind == OBJ_MEDIA) {
      *failure = dups("TypeError: cssRules of a media list");
      return 1;
    }
    const GTEXT_CSSOM_Rule_List * list;
    st = rule_list_of(ob, &list);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("cssRules", st);
    }
    else if (!list) {
      *failure = dups("TypeError: cssRules is undefined for this rule");
    }
    else {
      put_num(w, o->f[1], gtext_cssom_rule_list_length(list));
    }
    return 1;
  }
  if (strcmp(name, "ruleat") == 0) {
    NEEDS(5);   /* ruleat O OBJ INDEX null|undefined */
    OBJ(ob, 2, 0);
    const GTEXT_CSSOM_Rule_List * list;
    if (ob->kind == OBJ_MEDIA) {
      *failure = dups("TypeError: cssRules of a media list");
      return 1;
    }
    st = rule_list_of(ob, &list);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("cssRules", st);
    }
    else if (!list) {
      *failure = dups("TypeError: cssRules is undefined for this rule");
    }
    else {
      put_found(w, o->f[1], OBJ_RULE, gtext_cssom_rule_list_at(list, (size_t)strtoull(o->f[3], NULL, 10)), o->f[4][0] == 'n' ? ST_NULL : ST_UNDEFINED);
    }
    return 1;
  }
  if (strcmp(name, "klen") == 0 || strcmp(name, "kat") == 0) {
    /* rule.length and rule[i]: the indexed getter of CSSKeyframesRule; undefined on any other rule. klen R RULE; kat O RULE INDEX */
    NEEDS(name[1] == 'l' ? 3 : 4);
    OBJ(ru, 2, OBJ_RULE);
    if (strcmp(gtext_cssom_rule_interface_name(ru->ptr), "CSSKeyframesRule") != 0) {
      if (name[1] == 'l') {
        put_reg(w, o->f[1], val('u', "", 0));
      }
      else {
        put_obj(w, o->f[1], OBJ_RULE, ST_UNDEFINED, NULL);
      }
    }
    else if (name[1] == 'l') {
      put_num(w, o->f[1], gtext_cssom_rule_list_length(gtext_cssom_rule_css_rules(ru->ptr)));
    }
    else {
      put_found(w, o->f[1], OBJ_RULE, gtext_cssom_rule_list_at(gtext_cssom_rule_css_rules(ru->ptr), (size_t)strtoull(o->f[3], NULL, 10)), ST_UNDEFINED);
    }
    return 1;
  }
  if (strcmp(name, "rparent") == 0 || strcmp(name, "rsheet") == 0) {
    NEEDS(3);   /* rparent O RULE: parentRule; rsheet O RULE: parentStyleSheet */
    OBJ(ru, 2, OBJ_RULE);
    if (name[1] == 'p') {
      put_found(w, o->f[1], OBJ_RULE, gtext_cssom_rule_parent_rule(ru->ptr), ST_NULL);
    }
    else {
      put_found(w, o->f[1], OBJ_SHEET, gtext_cssom_rule_parent_sheet(ru->ptr), ST_NULL);
    }
    return 1;
  }
  if (strcmp(name, "rstyle") == 0) {
    NEEDS(3);   /* rstyle S RULE: rule.style */
    OBJ(ru, 2, OBJ_RULE);
    GTEXT_CSSOM_Decl * d = NULL;
    st = gtext_cssom_rule_style(ru->ptr, &d);
    if (st != GTEXT_CSS_OK) {
      *failure = fmt("TypeError: this rule has no style (%s)", gtext_css_status_string(st));
      return 1;
    }
    put_style(w, o->f[1], d, 0, NULL, "", "");
    return 1;
  }
  if (strcmp(name, "omedia") == 0) {
    NEEDS(3);   /* omedia M OBJ: media */
    OBJ(ob, 2, 0);
    GTEXT_CSSOM_Media_List * m = NULL;
    st = ob->kind == OBJ_RULE ? gtext_cssom_rule_media(ob->ptr, &m) : ob->kind == OBJ_SHEET ? gtext_cssom_sheet_media(ob->ptr, &m) : GTEXT_CSS_E_INVALID;
    if (st != GTEXT_CSS_OK) {
      put_obj(w, o->f[1], OBJ_MEDIA, ST_UNDEFINED, NULL);
    }
    else {
      put_obj(w, o->f[1], OBJ_MEDIA, ST_OBJECT, m);
    }
    return 1;
  }
  if (strcmp(name, "kfind") == 0) {
    NEEDS(4);
    OBJ(ru, 2, OBJ_RULE);
    GTEXT_CSSOM_Rule * found = NULL;
    st = gtext_cssom_keyframes_find_rule(ru->ptr, o->f[3], o->l[3], &found);
    if (st != GTEXT_CSS_OK) {
      *failure = dups("TypeError: findRule of a rule that is not @keyframes");
      return 1;
    }
    put_found(w, o->f[1], OBJ_RULE, found, ST_NULL);
    return 1;
  }
  if (strcmp(name, "ownernode") == 0) {
    NEEDS(3);   /* ownernode E SHEET: a node, or null */
    OBJ(sh, 2, OBJ_SHEET);
    put_elem(w, o->f[1], (GTEXT_Markup_Node *)gtext_cssom_sheet_owner_node(sh->ptr));
    return 1;
  }
  if (strcmp(name, "ownerrule") == 0) {
    NEEDS(3);
    OBJ(sh, 2, OBJ_SHEET);
    GTEXT_CSSOM_Rule * r = NULL;
    gtext_cssom_sheet_owner_rule(sh->ptr, &r);
    put_found(w, o->f[1], OBJ_RULE, r, ST_NULL);
    return 1;
  }
  if (strcmp(name, "rattr") == 0) {
    NEEDS(4);   /* rattr R RULE NAME */
    OBJ(ru, 2, OBJ_RULE);
    char * text = NULL;
    size_t n = 0;
    bool is_null = false;
    if (strcmp(o->f[3], "selectorText") == 0) {
      st = gtext_cssom_rule_selector_text(ru->ptr, &text, &n);
    }
    else if (strcmp(o->f[3], "keyText") == 0) {
      st = gtext_cssom_rule_key_text(ru->ptr, &text, &n);
    }
    else if (strcmp(o->f[3], "cssText") == 0) {
      st = gtext_cssom_rule_css_text(ru->ptr, &text, &n);
    }
    else {
      st = gtext_cssom_rule_get_attribute(ru->ptr, o->f[3], strlen(o->f[3]), &text, &n, &is_null);
    }
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      put_reg(w, o->f[1], val('u', "", 0));   /* a member the interface does not have */
    }
    else if (st != GTEXT_CSS_OK) {
      *failure = css_exception(o->f[3], st);
    }
    else if (is_null) {
      put_reg(w, o->f[1], val('N', "", 0));
      gtext_cssom_buffer_free(gtext_cssom_document_allocator(w->doc), text);
    }
    else {
      put_reg(w, o->f[1], take_string(w, text, n));
    }
    return 1;
  }
  if (strcmp(name, "rtype") == 0) {
    NEEDS(3);
    OBJ(ru, 2, OBJ_RULE);
    put_num(w, o->f[1], gtext_cssom_rule_type(ru->ptr));
    return 1;
  }
  if (strcmp(name, "shattr") == 0) {
    NEEDS(4);   /* shattr R SHEET NAME */
    OBJ(sh, 2, OBJ_SHEET);
    char * text = NULL;
    size_t n = 0;
    const char * a = o->f[3];
    if (strcmp(a, "href") == 0) {
      st = gtext_cssom_sheet_href(sh->ptr, &text, &n);
    }
    else if (strcmp(a, "title") == 0) {
      st = gtext_cssom_sheet_title(sh->ptr, &text, &n);
    }
    else if (strcmp(a, "type") == 0) {
      const char * t = gtext_cssom_sheet_type(sh->ptr);
      put_reg(w, o->f[1], val('s', t, strlen(t)));
      return 1;
    }
    else if (strcmp(a, "disabled") == 0) {
      put_bool(w, o->f[1], gtext_cssom_sheet_disabled(sh->ptr));
      return 1;
    }
    else {
      *program_error = 1;
      *failure = fmt("shattr %s is not known", a);
      return 1;
    }
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception(a, st);
    }
    else if (!text) {
      put_reg(w, o->f[1], val('N', "", 0));   /* href and title are null for a sheet that has none */
    }
    else {
      put_reg(w, o->f[1], take_string(w, text, n));
    }
    return 1;
  }
  if (strcmp(name, "insertrule") == 0) {
    NEEDS(5);   /* insertrule R OBJ TEXT INDEX (u: for no index argument) */
    OBJ(ob, 2, 0);
    const size_t index = o->f[4][0] == 'u' ? 0 : (size_t)strtoull(o->f[4] + 2, NULL, 10);
    GTEXT_CSSOM_Rule * made = NULL;
    if (ob->kind == OBJ_SHEET) {
      st = gtext_cssom_sheet_insert_rule(ob->ptr, o->f[3], o->l[3], index, &made, &oc);
    }
    else if (ob->kind == OBJ_RULE) {
      st = gtext_cssom_rule_insert_rule(ob->ptr, o->f[3], o->l[3], index, &made, &oc);
    }
    else {
      st = GTEXT_CSS_E_INVALID;
    }
    gtext_cssom_rule_release(made);
    sync_world(w);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      *failure = dups("TypeError: insertRule is not a function");
    }
    else if (st != GTEXT_CSS_OK) {
      *failure = css_exception("insertRule", st);
    }
    else {
      put_num(w, o->f[1], index);
    }
    return 1;
  }
  if (strcmp(name, "deleterule") == 0) {
    NEEDS(3);   /* deleterule OBJ ARG: an index, or for @keyframes the text of a key */
    OBJ(ob, 1, 0);
    if (ob->kind == OBJ_RULE && strcmp(gtext_cssom_rule_interface_name(ob->ptr), "CSSKeyframesRule") == 0) {
      st = gtext_cssom_keyframes_delete_rule(ob->ptr, o->f[2] + 2, o->l[2] - 2, &oc);
    }
    else {
      const size_t index = (size_t)strtoull(o->f[2] + 2, NULL, 10);
      st = ob->kind == OBJ_SHEET ? gtext_cssom_sheet_delete_rule(ob->ptr, index, &oc) : ob->kind == OBJ_RULE ? gtext_cssom_rule_delete_rule(ob->ptr, index, &oc) : GTEXT_CSS_E_INVALID;
    }
    sync_world(w);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      *failure = dups("TypeError: deleteRule is not a function");
    }
    else if (st != GTEXT_CSS_OK) {
      *failure = css_exception("deleteRule", st);
    }
    return 1;
  }
  if (strcmp(name, "addrule") == 0 || strcmp(name, "removerule") == 0) {
    if (name[0] == 'a') {
      NEEDS(6);   /* addrule R SHEET SELECTOR STYLE INDEX */
    }
    else {
      NEEDS(3);   /* removerule SHEET INDEX */
    }
    OBJ(sh, name[0] == 'a' ? 2 : 1, OBJ_SHEET);
    if (name[0] == 'a') {
      const bool has_index = o->f[5][0] != 'u';
      const size_t index = has_index ? (size_t)strtoull(o->f[5] + 2, NULL, 10) : 0;
      st = gtext_cssom_sheet_add_rule(sh->ptr, o->f[3], o->l[3], o->f[4], o->l[4], has_index, index, &oc);
      if (st == GTEXT_CSS_OK) {
        put_reg(w, o->f[1], val('n', "-1", 2));   /* the binding returns -1 */
      }
    }
    else {
      st = gtext_cssom_sheet_remove_rule(sh->ptr, (size_t)strtoull(o->f[2] + 2, NULL, 10), &oc);
    }
    sync_world(w);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception(name[0] == 'a' ? "addRule" : "removeRule", st);
    }
    return 1;
  }
  if (strcmp(name, "appendrule") == 0) {
    NEEDS(3);
    OBJ(ru, 1, OBJ_RULE);
    st = gtext_cssom_keyframes_append_rule(ru->ptr, o->f[2], o->l[2], NULL, &oc);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      *failure = dups("TypeError: appendRule is not a function");
    }
    else if (st != GTEXT_CSS_OK) {
      *failure = css_exception("appendRule", st);
    }
    return 1;
  }
  if (strcmp(name, "rset") == 0) {
    NEEDS(4);   /* rset RULE NAME VALUE */
    OBJ(ru, 1, OBJ_RULE);
    st = GTEXT_CSS_OK;
    if (strcmp(o->f[2], "selectorText") == 0) {
      st = gtext_cssom_rule_set_selector_text(ru->ptr, o->f[3], o->l[3], &oc);
    }
    else if (strcmp(o->f[2], "keyText") == 0) {
      st = gtext_cssom_rule_set_key_text(ru->ptr, o->f[3], o->l[3], &oc);
    }
    else if (strcmp(o->f[2], "cssText") != 0) {
      st = gtext_cssom_rule_set_attribute(ru->ptr, o->f[2], strlen(o->f[2]), o->f[3], o->l[3], &oc);
    }
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      st = GTEXT_CSS_OK;   /* a member the object does not have: script makes a property of the object, and nothing else happens */
    }
    sync_world(w);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception(o->f[2], st);
    }
    return 1;
  }
  if (strcmp(name, "sdisabled") == 0) {
    NEEDS(3);
    OBJ(sh, 1, OBJ_SHEET);
    st = gtext_cssom_sheet_set_disabled(sh->ptr, strcmp(o->f[2], "true") == 0, &oc);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("disabled", st);
    }
    return 1;
  }
  if (strcmp(name, "mtext") == 0 || strcmp(name, "mlen") == 0 || strcmp(name, "mitem") == 0) {
    NEEDS(name[1] == 'i' ? 5 : 3);   /* mtext R M; mlen R M; mitem R M INDEX null|undefined */
    OBJ(m, 2, OBJ_MEDIA);
    if (name[1] == 't') {
      char * text = NULL;
      size_t n = 0;
      st = gtext_cssom_media_text(m->ptr, &text, &n);
      if (st != GTEXT_CSS_OK) {
        *failure = css_exception("mediaText", st);
      }
      else {
        put_reg(w, o->f[1], take_string(w, text, n));
      }
    }
    else if (name[1] == 'l') {
      size_t n = 0;
      gtext_cssom_media_length(m->ptr, &n);
      put_num(w, o->f[1], n);
    }
    else {
      char * text = NULL;
      size_t n = 0;
      bool is_null = false;
      size_t len = 0;
      gtext_cssom_media_length(m->ptr, &len);
      const size_t index = (size_t)strtoull(o->f[3], NULL, 10);
      if (index >= len) {
        put_reg(w, o->f[1], o->f[4][0] == 'n' ? val('N', "", 0) : val('u', "", 0));
        return 1;
      }
      st = gtext_cssom_media_item(m->ptr, index, &text, &n, &is_null);
      if (st != GTEXT_CSS_OK || is_null) {
        put_reg(w, o->f[1], val('N', "", 0));
      }
      else {
        put_reg(w, o->f[1], take_string(w, text, n));
      }
    }
    return 1;
  }
  if (strcmp(name, "mset") == 0 || strcmp(name, "mappend") == 0 || strcmp(name, "mdelete") == 0) {
    NEEDS(3);
    OBJ(m, 1, OBJ_MEDIA);
    st = name[1] == 's' ? gtext_cssom_media_set_text(m->ptr, o->f[2], o->l[2], &oc) : name[1] == 'a' ? gtext_cssom_media_append(m->ptr, o->f[2], o->l[2], &oc) :
                                                                                                          gtext_cssom_media_delete(m->ptr, o->f[2], o->l[2], &oc);
    sync_world(w);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception(name[1] == 's' ? "mediaText" : name[1] == 'a' ? "appendMedium" : "deleteMedium", st);
    }
    return 1;
  }
  if (strcmp(name, "rinst") == 0) {
    NEEDS(4);   /* rinst R OBJ CLASS: x instanceof CLASS, for the names of the interfaces of rules and the sheet */
    object * ob = find_obj(w, o->f[2]);
    if (!ob) {
      *program_error = 1;
      *failure = dups("rinst names an object the program did not make");
      return 1;
    }
    int yes = 0;
    if (ob->state == ST_OBJECT) {
      if (ob->kind == OBJ_SHEET) {
        yes = strcmp(o->f[3], "CSSStyleSheet") == 0 || strcmp(o->f[3], "StyleSheet") == 0;
      }
      else if (ob->kind == OBJ_RULE) {
        yes = strcmp(o->f[3], "CSSRule") == 0 || strcmp(o->f[3], gtext_cssom_rule_interface_name(ob->ptr)) == 0;
      }
      else {
        yes = strcmp(o->f[3], "MediaList") == 0;
      }
    }
    put_bool(w, o->f[1], yes);
    return 1;
  }
  if (strcmp(name, "adoptset") == 0) {
    /* document.adoptedStyleSheets = [sheets]: the checks of the setter are the library's */
    GTEXT_CSSOM_Sheet * list[64];
    if (o->n > 65) {
      *program_error = 1;
      *failure = dups("adoptset has too many sheets");
      return 1;
    }
    for (size_t i = 1; i < o->n; i++) {
      object * ob = find_obj(w, o->f[i]);
      if (!ob) {
        *program_error = 1;
        *failure = dups("adoptset names an object the program did not make");
        return 1;
      }
      if (ob->state != ST_OBJECT || ob->kind != OBJ_SHEET) {
        *failure = dups("TypeError: adoptedStyleSheets takes sheets");
        return 1;
      }
      list[i - 1] = ob->ptr;
    }
    st = gtext_cssom_scope_adopted_set(world_scope(w), list, o->n - 1, &oc);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("adoptedStyleSheets", st);
    }
    return 1;
  }
  if (strcmp(name, "adoptpush") == 0) {
    NEEDS(2);
    OBJ(sh, 1, OBJ_SHEET);
    st = gtext_cssom_scope_adopted_insert(world_scope(w), sh->ptr, gtext_cssom_scope_adopted_count(world_scope(w)), &oc);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("adoptedStyleSheets", st);
    }
    return 1;
  }
  if (strcmp(name, "adoptn") == 0) {
    NEEDS(2);
    put_num(w, o->f[1], gtext_cssom_scope_adopted_count(world_scope(w)));
    return 1;
  }
  if (strcmp(name, "adoptat") == 0) {
    NEEDS(3);
    put_found(w, o->f[1], OBJ_SHEET, gtext_cssom_scope_adopted_at(world_scope(w), (size_t)strtoull(o->f[2], NULL, 10)), ST_UNDEFINED);
    return 1;
  }
  return 0;
#undef NEEDS
#undef OBJ
}

/* ---- running one op --------------------------------------------------------------------------------------------------------------------- */

/// Returns NULL, or the message of a failure of the test (an assertion, or the exception the page's script would have thrown).
static char * run_op(world * w, const op * o, testrun * t, int * program_error) {
  const char * name = o->f[0];
  *program_error = 0;
  int ok;
#define NEED(n_) do { if (o->n != (n_)) { *program_error = 1; return fmt("the op %s has %zu fields, not %d", name, o->n, (n_)); } } while (0)
#define ELEM(var, fld) elem * var = find_elem(w, o->f[fld]); if (!var) { *program_error = 1; return fmt("the op %s names the element %s, which the program did not make", name, o->f[fld]); } \
  if (!var->node) { return dups("TypeError: the element is null"); } \
  if (!is_element(var->node)) { return dups("TypeError: the node is not an element"); }
#define STYLE(var, fld) style * var = find_style(w, o->f[fld]); if (!var) { *program_error = 1; return fmt("the op %s names the style %s, which the program did not make", name, o->f[fld]); }

  {
    char * failure = NULL;
    if (dom_op(w, o, program_error, &failure) || sheet_op(w, o, program_error, &failure)) {
      return failure;
    }
  }
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
    else if (strcmp(how, "list") == 0) {
      nodelist * l = o->n > 4 ? find_list(w, o->f[3]) : NULL;
      if (!l) {
        *program_error = 1;
        return fmt("elem list names the list %s, which the program did not make", o->n > 3 ? o->f[3] : "");
      }
      const size_t idx = (size_t)strtoull(o->f[4], NULL, 10);
      found = idx < l->n ? l->nodes[idx] : NULL;
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
      else if (strcmp(what, "firstChild") == 0) {
        found = gtext_markup_node_first_child(n);
      }
      else if (strcmp(what, "lastChild") == 0) {
        found = gtext_markup_node_last_child(n);
      }
      else if (strcmp(what, "nextSibling") == 0) {
        found = gtext_markup_node_next_sibling(n);
      }
      else if (strcmp(what, "previousSibling") == 0) {
        found = gtext_markup_node_previous_sibling(n);
      }
      else if (strcmp(what, "parentNode") == 0) {
        found = gtext_markup_node_parent(n);
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
    sync_world(w);
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
    sync_world(w);
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
    sync_world(w);
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
      sync_world(w);
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
  /* `iftrue X` / `iffalse X` ... `else` ... `endif` (X an operand): the ops between are run when the value is true in JavaScript's sense (for a node, when there is one), and skipped otherwise; the blocks nest */
  int gdepth;
  char gparent[32], gcond[32], gact[32];
  /* `throws KIND NAME` ... `endthrows`: the ops between are the function of assert_throws_dom() or assert_throws_js(); the first failure among them is the exception, and the rest is skipped */
  int exp_active, exp_ignore;
  char * exp_kind, * exp_name, * exp_msg;
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
  /* The world is opened where it stays: the callbacks of its compute context and of its conditions hold its address (`cenv.user`, `container.user`) and so do its match options, so a
     world opened in a local and copied into the program would leave them pointing at a frame that has returned. The old world is moved aside first; its data is read, never its
     callbacks. */
  world old = p->w;
  const int had = p->open;
  world * nw = &p->w;
  if (!world_open(nw, p->html, extent)) {
    world_close(nw);
    p->w = old;
    return 0;
  }
  if (had) {
    world * ow = &old;
    for (size_t i = 0; i < ow->nelems; i++) {
      GTEXT_Markup_Node * n = NULL;
      if (ow->elems[i].node) {
        size_t depth;
        size_t * path = node_path(ow->elems[i].node, &depth);
        n = node_at(nw->markup, path, depth);
        free(path);
      }
      put_elem(nw, ow->elems[i].name, n);
    }
    for (size_t i = 0; i < ow->nlists; i++) {
      GTEXT_Markup_Node ** nodes = malloc((ow->lists[i].n ? ow->lists[i].n : 1) * sizeof *nodes);
      for (size_t k = 0; k < ow->lists[i].n; k++) {
        size_t depth;
        size_t * path = node_path(ow->lists[i].nodes[k], &depth);
        nodes[k] = node_at(nw->markup, path, depth);
        free(path);
      }
      put_list(nw, ow->lists[i].name, nodes, ow->lists[i].n);
    }
    for (size_t i = 0; i < ow->nregs; i++) {
      put_reg(nw, ow->regs[i].name, val(ow->regs[i].v.kind, ow->regs[i].v.s, ow->regs[i].v.n));
    }
    g_doc = nw->doc;
    for (size_t i = 0; i < ow->nstyles; i++) {
      style * s = &ow->styles[i];
      elem * e = find_elem(nw, s->elem_name);
      GTEXT_CSSOM_Decl * d = NULL;
      if (e && e->node) {
        if (s->computed) {
          gtext_cssom_computed_style(nw->doc, gtext_css_markup_handle(e->node), s->pseudo, strlen(s->pseudo), &d);
        }
        else {
          gtext_cssom_inline_style(nw->doc, e->node, &nw->attr, &d);
        }
      }
      put_style(nw, s->name, d, s->computed, e ? e->node : NULL, s->elem_name, s->pseudo);
    }
    world_close(ow);
  }
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
    else if (p->t.timeline) {
      printf("R\t%s\t%d\tTIMELINE\t", p->file, p->index);
      print_escaped(p->t.timeline_property ? p->t.timeline_property : "");
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
  free(p->t.timeline_property);
  memset(&p->t, 0, sizeof p->t);
  p->in_test = 0;
  p->in_cleanup = 0;
}

static void end_program(program * p) {
  end_test(p);
  p->gdepth = 0;
  p->exp_active = 0;
  free(p->exp_kind);
  free(p->exp_name);
  free(p->exp_msg);
  p->exp_kind = p->exp_name = p->exp_msg = NULL;
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
  const char * tl = getenv("CSSOM_RUNNER_TIMELINE");
  if (tl && strcmp(tl, "none") == 0) {
    g_timeline = 0;
  }
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
      if (strcmp(o.f[0], "iftrue") == 0 || strcmp(o.f[0], "iffalse") == 0 || strcmp(o.f[0], "else") == 0 || strcmp(o.f[0], "endif") == 0) {
        if (o.f[0][0] == 'e' && o.f[0][1] == 'n') {
          if (p.gdepth > 0) {
            p.gdepth--;
          }
        }
        else if (o.f[0][0] == 'e') {
          if (p.gdepth > 0) {
            p.gact[p.gdepth - 1] = p.gparent[p.gdepth - 1] && !p.gcond[p.gdepth - 1];
          }
        }
        else if (p.gdepth < 32 && o.n == 2) {
          const int parent = p.gdepth == 0 ? 1 : p.gact[p.gdepth - 1];
          int cond = 0;
          if (parent) {
            int okay = 0;
            value v = operand(&p.w, o.f[1], o.l[1], &okay);
            /* JavaScript's truthiness of the value: a node is true when there is one */
            const int truth = !okay ? 0 : v.kind == 'b' ? strcmp(v.s, "true") == 0 : v.kind == 's' ? v.n > 0 : v.kind == 'n' ? (strtod(v.s, NULL) != 0 && strtod(v.s, NULL) == strtod(v.s, NULL)) : v.kind == 'e';
            value_free(&v);
            cond = o.f[0][2] == 't' ? truth : !truth;
          }
          p.gparent[p.gdepth] = (char)parent;
          p.gcond[p.gdepth] = (char)cond;
          p.gact[p.gdepth] = (char)(parent && cond);
          p.gdepth++;
        }
        continue;
      }
      if (p.gdepth > 0 && !p.gact[p.gdepth - 1]) {
        continue;
      }
      if ((strcmp(o.f[0], "throws") == 0 && o.n == 3) || (strcmp(o.f[0], "try") == 0 && o.n == 1)) {
        p.exp_active = 1;
        p.exp_ignore = !p.open || (p.in_test && p.t.failed && !p.in_cleanup);
        free(p.exp_kind);
        free(p.exp_name);
        free(p.exp_msg);
        p.exp_kind = dups(o.n == 3 ? o.f[1] : "any");
        p.exp_name = dups(o.n == 3 ? o.f[2] : "");
        p.exp_msg = NULL;
        continue;
      }
      if (strcmp(o.f[0], "endthrows") == 0 || strcmp(o.f[0], "endtry") == 0) {
        if (p.exp_active && !p.exp_ignore && strcmp(p.exp_kind, "any") != 0) {
          char * result = NULL;
          const size_t nl = strlen(p.exp_name);
          if (!p.exp_msg) {
            result = fmt("assert_throws_%s: function did not throw", p.exp_kind);
          }
          else if (strncmp(p.exp_msg, p.exp_name, nl) != 0 || p.exp_msg[nl] != ':') {
            result = fmt("assert_throws_%s: function threw \"%.80s\", expected %s", p.exp_kind, p.exp_msg, p.exp_name);
          }
          if (result && p.in_test && !p.t.failed) {
            p.t.failed = 1;
            p.t.message = result;
          }
          else {
            free(result);
          }
        }
        p.exp_active = 0;
        free(p.exp_msg);
        p.exp_msg = NULL;
        continue;
      }
      if (p.exp_active && (p.exp_ignore || p.exp_msg)) {
        continue;
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
        else if (p.exp_active) {
          p.exp_msg = msg;
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
