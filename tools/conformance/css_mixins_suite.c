// SPDX-License-Identifier: LGPL-3.0-only
//
// Copyright (C) 2026 Corey Pennycuff
//
// This file is part of Ghoti.io Text.
//
// Ghoti.io Text is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License version 3 as
// published by the Free Software Foundation.
//
// Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// The runner of tools/conformance/css_mixins_suite.py: a document built as a consumer of the library builds one, and the computed values of two things read from it.
//
// Input, one line a document, fields separated by TAB with `\\`, `\n` and `\t` escaped:
//
//   V TAB <kind> TAB <properties, comma separated> TAB <html>
//
// `custom`: the computed values of `--actual` and `--expected` on the element with id `target`, as `getComputedStyle(target).getPropertyValue(name)` gives them.
// `elements`: the computed values of each property on the element with id `actual` and on the element with id `expected`, written `property: value; ...` for each.
// Output, one line a document: `R TAB <first> TAB <second>`, or `E TAB <what the runner could not do>`.
//
// The document is the Markup tree of the library's HTML parser, the Markup binding (so that its `<style>` elements are the document's sheets and `@function` rules take part), the conditions
// of a 1280 by 800 viewport and a document with container queries answered from the computed styles of the containers. It has no user agent sheet: the pages' own sheets are what is read.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE * g_out;   /* the line being answered: one line a request, an R line or an E line */

typedef struct {
  GTEXT_CSS_Registry * registry;
  GTEXT_Markup_Document * markup;
  GTEXT_CSS_Markup_Adapter * adapter;
  GTEXT_CSS_Selector_Match_Options match_options;
  GTEXT_CSSOM_Document * doc;
  GTEXT_CSS_Conditions * conditions;
  GTEXT_CSSOM_Markup_Binding * binding;
} world;

static char * unescape(const char * s, size_t n, size_t * out_len) {
  char * r = malloc(n + 1);
  size_t k = 0;
  if (!r) {
    abort();
  }
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\' && i + 1 < n) {
      i++;
      r[k++] = s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
    }
    else {
      r[k++] = s[i];
    }
  }
  r[k] = '\0';
  *out_len = k;
  return r;
}

static void escape_out(const char * s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\') {
      fputs("\\\\", g_out);
    }
    else if (s[i] == '\n') {
      fputs("\\n", g_out);
    }
    else if (s[i] == '\t') {
      fputs("\\t", g_out);
    }
    else {
      fputc(s[i], g_out);
    }
  }
}

static GTEXT_CSS_Tristate runner_condition(void * user, GTEXT_CSS_Condition_Kind kind, const GTEXT_CSS_String * text, GTEXT_CSS_Element_Handle element) {
  (void)element;
  const world * w = user;
  return gtext_css_conditions_condition(w->conditions, kind, text);
}

static GTEXT_CSS_Status runner_condition_status(void * user, GTEXT_CSS_Condition_Kind kind, const GTEXT_CSS_String * text, GTEXT_CSS_Element_Handle element, GTEXT_CSS_Tristate * answer,
    const char ** detail) {
  const world * w = user;
  return gtext_css_conditions_condition_element(w->conditions, kind, text, element, NULL, answer, detail);
}

static const GTEXT_CSS_Computed_Style * runner_container_style(void * user, GTEXT_CSS_Element_Handle element) {
  const world * w = user;
  GTEXT_CSS_Container_Interface ci;
  if (!w->doc || gtext_cssom_document_container_interface(w->doc, &ci) != GTEXT_CSS_OK) {
    return NULL;
  }
  return ci.computed_style(ci.user, element);
}

static int world_open(world * w, const char * html, size_t html_len, const char ** why) {
  memset(w, 0, sizeof *w);
  if (gtext_css_registry_new(NULL, &w->registry) != GTEXT_CSS_OK) {
    *why = "making the registry";
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
    *why = "parsing the page";
    return 0;
  }
  gtext_html_result_free(&hr);
  if (gtext_css_markup_adapter_new(w->markup, NULL, &w->adapter) != GTEXT_CSS_OK) {
    *why = "making the adapter";
    return 0;
  }
  w->match_options = gtext_css_markup_match_options(w->adapter);
  GTEXT_CSSOM_Config c = gtext_cssom_config_default();
  c.registry = w->registry;
  c.element_interface = gtext_css_markup_interface(w->adapter);
  c.match_options = &w->match_options;
  c.tree_version = &gtext_cssom_markup_tree_version;
  c.tree_version_user = w->markup;
  GTEXT_CSS_Environment cenv = gtext_css_environment_default();
  cenv.user = w;
  cenv.condition = runner_condition;
  cenv.condition_status = runner_condition_status;
  c.environment = &cenv;
  if (gtext_cssom_document_new(&c, &w->doc) != GTEXT_CSS_OK) {
    *why = "making the document";
    return 0;
  }
  GTEXT_CSS_Container_Interface container;
  if (gtext_cssom_document_container_interface(w->doc, &container) != GTEXT_CSS_OK) {
    *why = "making the container interface";
    return 0;
  }
  container.user = w;
  container.computed_style = runner_container_style;
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.media = &env;
  co.element_interface = c.element_interface;
  co.container = &container;
  if (gtext_css_conditions_new(w->registry, &co, &w->conditions) != GTEXT_CSS_OK || gtext_cssom_document_set_conditions(w->doc, w->conditions, NULL) != GTEXT_CSS_OK) {
    *why = "making the conditions";
    return 0;
  }
  GTEXT_CSSOM_Markup_Options mo = gtext_cssom_markup_options_default();
  if (gtext_cssom_markup_binding_new(w->doc, gtext_cssom_document_scope(w->doc), w->markup, &mo, &w->binding) != GTEXT_CSS_OK || gtext_cssom_markup_sync(w->binding, NULL) != GTEXT_CSS_OK) {
    *why = "binding the sheets";
    return 0;
  }
  return 1;
}

static void world_close(world * w) {
  gtext_cssom_markup_binding_free(w->binding);
  gtext_cssom_document_release(w->doc);
  gtext_css_conditions_free(w->conditions);
  gtext_css_markup_adapter_free(w->adapter);
  gtext_markup_document_free(w->markup);
  gtext_css_registry_free(w->registry);
  memset(w, 0, sizeof *w);
}

static GTEXT_Markup_Node * find_by_id(world * w, const char * id) {
  GTEXT_Markup_Node * root = gtext_markup_document_root(w->markup);
  const size_t len = strlen(id);
  for (GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, 0); n; n = gtext_markup_node_next_in_order(root, n, 0)) {
    GTEXT_Markup_Attribute a;
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT && gtext_markup_element_get_attribute(n, NULL, "id", &a) && a.value_length == len && memcmp(a.value, id, len) == 0) {
      return n;
    }
  }
  return NULL;
}

/// The computed value of a property of an element, written as `getPropertyValue()` does: the text, or "" for none. Returns the status.
static GTEXT_CSS_Status read_value(world * w, GTEXT_Markup_Node * e, const char * name, char ** out, size_t * n) {
  GTEXT_CSSOM_Decl * d = NULL;
  GTEXT_CSS_Status st = gtext_cssom_computed_style(w->doc, gtext_css_markup_handle(e), NULL, 0, &d);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = gtext_cssom_decl_get_property_value(d, name, strlen(name), out, n);
  gtext_cssom_decl_release(d);
  return st;
}

static void answer_property(world * w, GTEXT_Markup_Node * e, const char * name, int * ok, const char ** why) {
  char * v = NULL;
  size_t n = 0;
  const GTEXT_CSS_Status st = read_value(w, e, name, &v, &n);
  if (st != GTEXT_CSS_OK) {
    *ok = 0;
    *why = gtext_css_status_string(st);
    return;
  }
  escape_out(v, n);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(w->doc), v);
}

static void run_one(const char * kind, const char * props, const char * html, size_t html_len, int * ok, const char ** why) {
  world w;
  if (!world_open(&w, html, html_len, why)) {
    *ok = 0;
    world_close(&w);
    return;
  }
  if (strcmp(kind, "custom") == 0) {
    GTEXT_Markup_Node * target = find_by_id(&w, "target");
    if (!target) {
      *ok = 0;
      *why = "no element with id target";
      world_close(&w);
      return;
    }
    answer_property(&w, target, "--actual", ok, why);
    fputc('\t', g_out);
    answer_property(&w, target, "--expected", ok, why);
  }
  else {
    GTEXT_Markup_Node * a = find_by_id(&w, "actual");
    GTEXT_Markup_Node * b = find_by_id(&w, "expected");
    if (!a || !b) {
      *ok = 0;
      *why = "no element with id actual or expected";
      world_close(&w);
      return;
    }
    for (int side = 0; side < 2; side++) {
      char * list = strdup(props);
      for (char * p = strtok(list, ","); p; p = strtok(NULL, ",")) {
        fprintf(g_out, "%s: ", p);
        answer_property(&w, side == 0 ? a : b, p, ok, why);
        fputs("; ", g_out);
      }
      free(list);
      if (side == 0) {
        fputc('\t', g_out);
      }
    }
  }
  world_close(&w);
}

static void run(const char * kind, const char * props, const char * html, size_t html_len) {
  char * buf = NULL;
  size_t len = 0;
  g_out = open_memstream(&buf, &len);
  if (!g_out) {
    abort();
  }
  int ok = 1;
  const char * why = "";
  run_one(kind, props, html, html_len, &ok, &why);
  fclose(g_out);
  g_out = NULL;
  if (ok) {
    fputs("R\t", stdout);
    fwrite(buf, 1, len, stdout);
    fputc('\n', stdout);
  }
  else {
    printf("E\t%s\n", why);
  }
  free(buf);
}

int main(void) {
  char * line = NULL;
  size_t cap = 0;
  ssize_t got;
  while ((got = getline(&line, &cap, stdin)) > 0) {
    if (got > 0 && line[got - 1] == '\n') {
      line[--got] = '\0';
    }
    // V TAB kind TAB props TAB html
    char * f[4];
    size_t nf = 0;
    char * p = line;
    while (nf < 3) {
      char * t = strchr(p, '\t');
      if (!t) {
        break;
      }
      *t = '\0';
      f[nf++] = p;
      p = t + 1;
    }
    if (nf != 3 || strcmp(f[0], "V") != 0) {
      printf("E\tbad request\n");
      fflush(stdout);
      continue;
    }
    f[3] = p;
    size_t kl, pl, hl;
    char * kind = unescape(f[1], strlen(f[1]), &kl);
    char * props = unescape(f[2], strlen(f[2]), &pl);
    char * html = unescape(f[3], strlen(f[3]), &hl);
    run(kind, props, html, hl);
    fflush(stdout);
    free(kind);
    free(props);
    free(html);
  }
  free(line);
  return 0;
}
