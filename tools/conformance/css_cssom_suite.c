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
 * The op interpreter of the CSSOM gate (tools/css/extract-wpt-cssom.py reads the pages, tools/conformance/css_cssom_suite.py scores them). It runs the programs of
 * declaration.tsv, read from the standard input: a program is a document with the elements its page's markup has, and the ops of the page's top-level statements and tests, run in
 * order on one world as the page's script runs them. No JavaScript engine of any size is in here: an op is one call of the declaration object.
 *
 * Input: `P file quirks tests`, then `O op field...` lines (tab separated; `\\`, `\t`, `\n` escaped). Output: one line `R file index PASS` or `R file index FAIL message` for
 * each test of each program, then `E file message` for a program that could not be run. A failed assertion ends its test, as the exception of an assert in the harness does.
 */

#include <ghoti.io/text/css.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 8
#define UNDEFINED "\x01undefined"

typedef struct {
  char * name;
  char * style;       /* the `style` attribute, or NULL */
  uint64_t version;
} elem;

typedef struct {
  char * name;
  GTEXT_CSSOM_Decl * decl;
  char * error;   /* a style handle that could not be made: every op on it fails the test that runs it with this message */
} handle;

typedef struct {
  char * name;
  GTEXT_CSSOM_Sheet * sheet;   /* what the page's variable named when it was read: retained */
  GTEXT_CSSOM_Rule * rule;     /* retained */
  int is_null;                 /* the variable was null */
  char * error;                /* the read threw: using the variable fails with this */
} binding;

typedef struct {
  GTEXT_CSS_Registry * registry;
  GTEXT_CSSOM_Document * doc;
  binding * bindings;
  size_t nbindings, capbindings;
  GTEXT_CSSOM_Sheet ** sheets;   /* the <style> elements of the page, in document order */
  size_t nsheets, capsheets;
  elem * elems;
  size_t nelems, capelems;
  handle * styles;
  size_t nstyles, capstyles;
  uint64_t tree;
} world;

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

/* ---- the binding over the elements ---------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status get_attribute(void * user, const void * e, const char ** value, size_t * length, bool * present) {
  (void)user;
  const elem * el = e;
  *present = el->style != NULL;
  *value = el->style ? el->style : "";
  *length = el->style ? strlen(el->style) : 0;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status set_attribute(void * user, const void * e, const char * value, size_t length) {
  world * w = user;
  elem * el = (elem *)e;
  free(el->style);
  el->style = dupn(value, length);
  el->version++;
  w->tree++;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status remove_attribute(void * user, const void * e) {
  world * w = user;
  elem * el = (elem *)e;
  free(el->style);
  el->style = NULL;
  el->version++;
  w->tree++;
  return GTEXT_CSS_OK;
}

static uint64_t attribute_version(void * user, const void * e) {
  (void)user;
  return ((const elem *)e)->version;
}

static uint64_t tree_version(void * user) {
  return ((world *)user)->tree;
}

static elem * find_elem(world * w, const char * name) {
  for (size_t i = 0; i < w->nelems; i++) {
    if (strcmp(w->elems[i].name, name) == 0) {
      return &w->elems[i];
    }
  }
  return NULL;
}

static handle * find_style(world * w, const char * name) {
  for (size_t i = 0; i < w->nstyles; i++) {
    if (strcmp(w->styles[i].name, name) == 0) {
      return &w->styles[i];
    }
  }
  return NULL;
}

static GTEXT_CSSOM_Attribute_Binding binding_of(world * w) {
  GTEXT_CSSOM_Attribute_Binding b;
  memset(&b, 0, sizeof b);
  b.size = sizeof b;
  b.user = w;
  b.get_attribute = get_attribute;
  b.set_attribute = set_attribute;
  b.remove_attribute = remove_attribute;
  b.attribute_version = attribute_version;
  return b;
}

static int world_open(world * w, int quirks) {
  memset(w, 0, sizeof *w);
  if (gtext_css_registry_new(NULL, &w->registry) != GTEXT_CSS_OK) {
    return 0;
  }
  GTEXT_CSSOM_Config c = gtext_cssom_config_default();
  c.registry = w->registry;
  c.quirks = quirks != 0;
  c.tree_version = tree_version;
  c.tree_version_user = w;
  w->tree = 1;
  w->capelems = 16;
  w->capstyles = 16;
  w->elems = calloc(w->capelems, sizeof *w->elems);
  w->styles = calloc(w->capstyles, sizeof *w->styles);
  return gtext_cssom_document_new(&c, &w->doc) == GTEXT_CSS_OK && w->elems && w->styles;
}

static void world_close(world * w) {
  for (size_t i = 0; i < w->nbindings; i++) {
    gtext_cssom_sheet_release(w->bindings[i].sheet);
    gtext_cssom_rule_release(w->bindings[i].rule);
    free(w->bindings[i].name);
    free(w->bindings[i].error);
  }
  free(w->bindings);
  for (size_t i = 0; i < w->nstyles; i++) {
    gtext_cssom_decl_release(w->styles[i].decl);
    free(w->styles[i].name);
    free(w->styles[i].error);
  }
  for (size_t i = 0; i < w->nsheets; i++) {
    gtext_cssom_sheet_release(w->sheets[i]);
  }
  free(w->sheets);
  gtext_cssom_document_release(w->doc);
  for (size_t i = 0; i < w->nelems; i++) {
    free(w->elems[i].name);
    free(w->elems[i].style);
  }
  free(w->elems);
  free(w->styles);
  gtext_css_registry_free(w->registry);
}

/* ---- ops ------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  char * f[MAX_FIELDS];
  size_t l[MAX_FIELDS];   /* the length of each field: a field can hold a NUL */
  size_t n;
} op;

static size_t unescape_into(char * s) {
  char * out = s;
  for (const char * p = s; *p; p++) {
    if (*p == '\\' && p[1]) {
      p++;
      *out++ = *p == 't' ? '\t' : *p == 'n' ? '\n' : *p == '0' ? '\0' : *p;
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

static char * take(world * w, char * buf) {
  char * s = dups(buf ? buf : "");
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(w->doc), buf);
  return s;
}

/* ---- sheets and rules: a path is S<n>, then .R<i> (the ith rule of the list of the sheet or rule so far), .P (parentRule), .PS (parentStyleSheet) -------------- */

typedef struct {
  GTEXT_CSSOM_Sheet * sheet;   /* retained, or NULL */
  GTEXT_CSSOM_Rule * rule;     /* retained, or NULL */
  int null_result;             /* the path ended at a parentRule that is null */
} node_ref;

static void ref_release(node_ref * r) {
  gtext_cssom_sheet_release(r->sheet);
  gtext_cssom_rule_release(r->rule);
  r->sheet = NULL;
  r->rule = NULL;
}

/// Follow a path. Returns NULL, or a message (a step of the path is `undefined` in the page's script, which throws when it is read through).
static char * resolve(world * w, const char * path, node_ref * out) {
  char msg[256];
  memset(out, 0, sizeof *out);
  const char * p = path;
  if (*p == '$') {
    const char * end = p;
    while (*end && *end != '.') {
      end++;
    }
    const binding * b = NULL;
    for (size_t i = 0; i < w->nbindings; i++) {
      if (strlen(w->bindings[i].name) == (size_t)(end - p) && memcmp(w->bindings[i].name, p, (size_t)(end - p)) == 0) {
        b = &w->bindings[i];
      }
    }
    if (!b) {
      return dups("a variable the program did not bind");
    }
    if (b->error) {
      return dups(b->error);
    }
    out->sheet = gtext_cssom_sheet_retain(b->sheet);
    out->rule = gtext_cssom_rule_retain(b->rule);
    out->null_result = b->is_null;
    p = end;
    if (out->null_result && *p == '.') {
      ref_release(out);
      return dups("a property of null is read");
    }
  }
  else {
    if (*p != 'S') {
      return dups("a path that does not begin with a sheet");
    }
    const size_t si = (size_t)strtoull(p + 1, (char **)&p, 10);
    if (si >= w->nsheets) {
      snprintf(msg, sizeof msg, "document.styleSheets[%zu] is undefined", si);
      return dups(msg);
    }
    out->sheet = gtext_cssom_sheet_retain(w->sheets[si]);
  }
  while (*p == '.') {
    p++;
    if (*p == 'R') {
      const size_t ri = (size_t)strtoull(p + 1, (char **)&p, 10);
      const GTEXT_CSSOM_Rule_List * list = NULL;
      if (out->rule) {
        list = gtext_cssom_rule_css_rules(out->rule);
      }
      else if (out->sheet && gtext_cssom_sheet_rules(out->sheet, &list) != GTEXT_CSS_OK) {
        ref_release(out);
        return dups("cssRules of a sheet that is not origin-clean throws");
      }
      if (!list) {
        ref_release(out);
        return dups("cssRules is undefined for this rule");
      }
      GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_at(list, ri);
      if (!r) {
        ref_release(out);
        snprintf(msg, sizeof msg, "cssRules[%zu] is undefined", ri);
        return dups(msg);
      }
      gtext_cssom_sheet_release(out->sheet);
      out->sheet = NULL;
      gtext_cssom_rule_release(out->rule);
      out->rule = r;
    }
    else if (*p == 'K' && out->rule) {
      /* findRule(selector): the selector is hex */
      p++;
      char sel[256];
      size_t n = 0;
      while (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]) && n + 1 < sizeof sel) {
        char hex[3] = {p[0], p[1], 0};
        sel[n++] = (char)strtol(hex, NULL, 16);
        p += 2;
      }
      GTEXT_CSSOM_Rule * found = NULL;
      const GTEXT_CSS_Status st = gtext_cssom_keyframes_find_rule(out->rule, sel, n, &found);
      gtext_cssom_rule_release(out->rule);
      out->rule = NULL;
      if (st != GTEXT_CSS_OK) {
        ref_release(out);
        return dups("findRule of a rule that is not @keyframes");
      }
      out->rule = found;
      out->null_result = found == NULL;
    }
    else if (strncmp(p, "PS", 2) == 0 && out->rule) {
      p += 2;
      GTEXT_CSSOM_Sheet * s = gtext_cssom_rule_parent_sheet(out->rule);
      gtext_cssom_rule_release(out->rule);
      out->rule = NULL;
      out->sheet = s;
      out->null_result = s == NULL;
    }
    else if (*p == 'P' && out->rule) {
      p++;
      GTEXT_CSSOM_Rule * r = gtext_cssom_rule_parent_rule(out->rule);
      gtext_cssom_rule_release(out->rule);
      out->rule = r;
      out->null_result = r == NULL;
    }
    else {
      ref_release(out);
      return dups("a step of a path that does not apply");
    }
    if (out->null_result && *p == '.') {
      ref_release(out);
      return dups("a property of null is read");
    }
  }
  return NULL;
}

static char * run_rule_op(world * w, const op * o) {
  const char * name = o->f[0];
  char msg[512];
  if (strcmp(name, "rstyle") == 0 && o->n == 3) {
    if (find_style(w, o->f[1])) {
      return NULL;
    }
    node_ref r;
    char * err = resolve(w, o->f[2], &r);
    if (err) {
      return err;
    }
    if (!r.rule) {
      ref_release(&r);
      return dups("the style of null is read");
    }
    GTEXT_CSSOM_Decl * d = NULL;
    const GTEXT_CSS_Status st = gtext_cssom_rule_style(r.rule, &d);
    ref_release(&r);
    if (w->nstyles == w->capstyles) {
      w->capstyles *= 2;
      w->styles = realloc(w->styles, w->capstyles * sizeof *w->styles);
    }
    w->styles[w->nstyles].name = dups(o->f[1]);
    w->styles[w->nstyles].decl = d;
    w->styles[w->nstyles].error = NULL;
    if (st != GTEXT_CSS_OK) {
      snprintf(msg, sizeof msg, "style of a rule: %s", gtext_css_status_string(st));
      w->styles[w->nstyles].error = dups(msg);
    }
    w->nstyles++;
    return NULL;
  }
  if (strcmp(name, "sheet") == 0 && o->n == 3) {
    GTEXT_CSSOM_Sheet * s = NULL;
    if (gtext_cssom_sheet_new_parsed(w->doc, o->f[2], o->l[2], NULL, &s) != GTEXT_CSS_OK) {
      return dups("a <style> element's text could not be made a sheet");
    }
    if (w->nsheets == w->capsheets) {
      w->capsheets = w->capsheets ? w->capsheets * 2 : 8;
      w->sheets = realloc(w->sheets, w->capsheets * sizeof *w->sheets);
    }
    w->sheets[w->nsheets++] = s;
    return NULL;
  }
  if (strcmp(name, "assert_nsheets") == 0 && o->n == 2) {
    char num[32];
    snprintf(num, sizeof num, "%zu", w->nsheets);
    if (strcmp(num, o->f[1]) != 0) {
      snprintf(msg, sizeof msg, "document.styleSheets.length is %s, expected %s", num, o->f[1]);
      return dups(msg);
    }
    return NULL;
  }
  if ((strcmp(name, "assert_rlen") == 0 || strcmp(name, "assert_rtype") == 0) && o->n == 3) {
    node_ref r;
    char * err = resolve(w, o->f[1], &r);
    if (err) {
      return err;
    }
    char num[32];
    if (strcmp(name, "assert_rlen") == 0) {
      const GTEXT_CSSOM_Rule_List * list = NULL;
      if (r.rule) {
        list = gtext_cssom_rule_css_rules(r.rule);
      }
      else if (r.sheet && gtext_cssom_sheet_rules(r.sheet, &list) != GTEXT_CSS_OK) {
        ref_release(&r);
        return dups("cssRules of a sheet that is not origin-clean throws a SecurityError");
      }
      if (!list) {
        ref_release(&r);
        return dups("cssRules is undefined for this rule: reading its length throws");
      }
      snprintf(num, sizeof num, "%zu", gtext_cssom_rule_list_length(list));
    }
    else {
      if (!r.rule) {
        ref_release(&r);
        return dups("the type of null throws");
      }
      snprintf(num, sizeof num, "%u", gtext_cssom_rule_type(r.rule));
    }
    ref_release(&r);
    if (strcmp(num, o->f[2]) != 0) {
      snprintf(msg, sizeof msg, "%s %s is %s, expected %s", name, o->f[1], num, o->f[2]);
      return dups(msg);
    }
    return NULL;
  }
  if (strcmp(name, "assert_rsame") == 0 && (o->n == 3 || o->n == 2 || o->n == 4)) {
    /* assert_rsame PATH EXPECTED-NULL, or assert_rsame PATH-A PATH-B: identity */
    node_ref a, b;
    memset(&b, 0, sizeof b);
    char * err = resolve(w, o->f[1], &a);
    if (err) {
      return err;
    }
    int same;
    if (o->n == 3 && strcmp(o->f[2], "\x01null") == 0) {
      same = a.sheet == NULL && a.rule == NULL;
    }
    else if (o->n == 3) {
      err = resolve(w, o->f[2], &b);
      if (err) {
        ref_release(&a);
        return err;
      }
      same = a.sheet == b.sheet && a.rule == b.rule && (a.sheet != NULL || a.rule != NULL);
    }
    else {
      ref_release(&a);
      return dups("assert_rsame has two fields");
    }
    int ok = same;
    if (!ok) {
      snprintf(msg, sizeof msg, "%s is %s, expected %s", o->f[1], a.sheet || a.rule ? "an object" : "null", o->f[2]);
    }
    ref_release(&a);
    ref_release(&b);
    return ok ? NULL : dups(msg);
  }
  return dups("not a sheet or rule op");
}


/// The name of the exception a status stands for, "ok" for none, or "status N".
static const char * exception_of(GTEXT_CSS_Status st, char * buf, size_t cap) {
  if (st == GTEXT_CSS_OK) {
    return "ok";
  }
  const char * n = gtext_cssom_exception_name(st);
  if (n) {
    return n;
  }
  snprintf(buf, cap, "status %s", gtext_css_status_string(st));
  return buf;
}

static char * bad_call(const char * call, const char * expected, const char * got) {
  char msg[512];
  if (strcmp(expected, "ok") == 0) {
    snprintf(msg, sizeof msg, "%s threw %s", call, got);
  }
  else if (strcmp(got, "ok") == 0) {
    snprintf(msg, sizeof msg, "%s did not throw; expected %s", call, expected);
  }
  else {
    snprintf(msg, sizeof msg, "%s threw %s; expected %s", call, got, expected);
  }
  return dups(msg);
}

/// insertRule, deleteRule, addRule, appendRule and the keyframes deleteRule, and the assertions and bindings around them.
static char * run_edit_op(world * w, const op * o) {
  const char * name = o->f[0];
  char msg[512], eb[128];
  if (strcmp(name, "bind") == 0 && o->n == 3) {
    node_ref r;
    char * err = resolve(w, o->f[2], &r);
    if (w->nbindings == w->capbindings) {
      w->capbindings = w->capbindings ? w->capbindings * 2 : 16;
      w->bindings = realloc(w->bindings, w->capbindings * sizeof *w->bindings);
    }
    binding * b = &w->bindings[w->nbindings++];
    memset(b, 0, sizeof *b);
    b->name = dups(o->f[1]);
    if (err) {
      b->error = err;
      return NULL;
    }
    b->sheet = r.sheet;
    b->rule = r.rule;
    b->is_null = r.null_result;
    return NULL;
  }
  node_ref r;
  char * err = resolve(w, o->f[1], &r);
  if (err) {
    return err;
  }
  char * result = NULL;
  if (strcmp(name, "rinsert") == 0 && o->n == 6) {
    const int has_index = strcmp(o->f[2], UNDEFINED) != 0;
    const size_t index = has_index ? (size_t)strtoull(o->f[2], NULL, 10) : 0;
    GTEXT_CSS_Status st;
    GTEXT_CSSOM_Rule * made = NULL;
    GTEXT_CSSOM_Outcome oc;
    if (r.rule) {
      st = gtext_cssom_rule_insert_rule(r.rule, o->f[3], o->l[3], index, &made, &oc);
    }
    else if (r.sheet) {
      st = gtext_cssom_sheet_insert_rule(r.sheet, o->f[3], o->l[3], index, &made, &oc);
    }
    else {
      ref_release(&r);
      return dups("insertRule of null");
    }
    gtext_cssom_rule_release(made);
    const char * got = exception_of(st, eb, sizeof eb);
    if (strcmp(got, o->f[4]) != 0) {
      result = bad_call("insertRule", o->f[4], got);
    }
    else if (st == GTEXT_CSS_OK && strcmp(o->f[5], UNDEFINED) != 0) {
      char num[32];
      snprintf(num, sizeof num, "%zu", index);
      if (strcmp(num, o->f[5]) != 0) {
        snprintf(msg, sizeof msg, "insertRule returned %s, expected %s", num, o->f[5]);
        result = dups(msg);
      }
    }
  }
  else if (strcmp(name, "radd") == 0 && o->n == 7 && r.sheet) {
    GTEXT_CSSOM_Outcome oc;
    const int has_index = strcmp(o->f[4], UNDEFINED) != 0;
    const GTEXT_CSS_Status st = gtext_cssom_sheet_add_rule(r.sheet, o->f[2], o->l[2], o->f[3], o->l[3], has_index, has_index ? (size_t)strtoull(o->f[4], NULL, 10) : 0, &oc);
    const char * got = exception_of(st, eb, sizeof eb);
    if (strcmp(got, o->f[5]) != 0) {
      result = bad_call("addRule", o->f[5], got);
    }
  }
  else if (strcmp(name, "rdelete") == 0 && o->n == 4) {
    GTEXT_CSSOM_Outcome oc;
    const size_t index = (size_t)strtoull(o->f[2], NULL, 10);
    GTEXT_CSS_Status st;
    if (r.rule) {
      st = gtext_cssom_rule_delete_rule(r.rule, index, &oc);
    }
    else if (r.sheet) {
      st = gtext_cssom_sheet_delete_rule(r.sheet, index, &oc);
    }
    else {
      ref_release(&r);
      return dups("deleteRule of null");
    }
    const char * got = exception_of(st, eb, sizeof eb);
    if (strcmp(got, o->f[3]) != 0) {
      result = bad_call("deleteRule", o->f[3], got);
    }
  }
  else if (strcmp(name, "rappend") == 0 && o->n == 4 && r.rule) {
    GTEXT_CSSOM_Outcome oc;
    const GTEXT_CSS_Status st = gtext_cssom_keyframes_append_rule(r.rule, o->f[2], o->l[2], NULL, &oc);
    const char * got = exception_of(st, eb, sizeof eb);
    if (strcmp(got, o->f[3]) != 0) {
      result = bad_call("appendRule", o->f[3], got);
    }
  }
  else if (strcmp(name, "rkfdelete") == 0 && o->n == 4 && r.rule) {
    GTEXT_CSSOM_Outcome oc;
    const GTEXT_CSS_Status st = gtext_cssom_keyframes_delete_rule(r.rule, o->f[2], o->l[2], &oc);
    const char * got = exception_of(st, eb, sizeof eb);
    if (strcmp(got, o->f[3]) != 0) {
      result = bad_call("deleteRule", o->f[3], got);
    }
  }
  else if (strcmp(name, "rset") == 0 && o->n == 5 && r.rule) {
    /* rule.attribute = value: selectorText, keyText and the rest by the table of attributes (a readonly one is ignored, as WebIDL says; cssText does nothing) */
    GTEXT_CSSOM_Outcome oc;
    GTEXT_CSS_Status st = GTEXT_CSS_OK;
    if (strcmp(o->f[2], "selectorText") == 0) {
      st = gtext_cssom_rule_set_selector_text(r.rule, o->f[3], o->l[3], &oc);
    }
    else if (strcmp(o->f[2], "keyText") == 0) {
      st = gtext_cssom_rule_set_key_text(r.rule, o->f[3], o->l[3], &oc);
    }
    else if (strcmp(o->f[2], "cssText") != 0) {
      st = gtext_cssom_rule_set_attribute(r.rule, o->f[2], strlen(o->f[2]), o->f[3], o->l[3], &oc);
    }
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      st = GTEXT_CSS_OK;   /* a member the object does not have: script makes a property of the object, and nothing else happens */
    }
    const char * got = exception_of(st, eb, sizeof eb);
    if (strcmp(got, o->f[4]) != 0) {
      result = bad_call(o->f[2], o->f[4], got);
    }
  }
  else if (strcmp(name, "assert_rattr") == 0 && o->n == 4) {
    if (!r.rule) {
      result = dups("an attribute of null is read");
    }
    else {
      char * text = NULL;
      size_t n = 0;
      bool is_null = false;
      GTEXT_CSS_Status st;
      if (strcmp(o->f[2], "selectorText") == 0) {
        st = gtext_cssom_rule_selector_text(r.rule, &text, &n);
      }
      else if (strcmp(o->f[2], "keyText") == 0) {
        st = gtext_cssom_rule_key_text(r.rule, &text, &n);
      }
      else {
        st = gtext_cssom_rule_get_attribute(r.rule, o->f[2], strlen(o->f[2]), &text, &n, &is_null);
      }
      if (st == GTEXT_CSS_E_UNSUPPORTED) {
        if (strcmp(o->f[3], UNDEFINED) != 0) {
          snprintf(msg, sizeof msg, "%s is undefined for this rule, expected \"%.*s\"", o->f[2], (int)(o->l[3] < 200 ? o->l[3] : 200), o->f[3]);
          result = dups(msg);
        }
      }
      else if (st != GTEXT_CSS_OK) {
        snprintf(msg, sizeof msg, "%s: %s", o->f[2], gtext_css_status_string(st));
        result = dups(msg);
      }
      else if (strcmp(o->f[3], "\x01null") == 0 ? !is_null : (is_null || n != o->l[3] || memcmp(text, o->f[3], n) != 0)) {
        snprintf(msg, sizeof msg, "%s is %s\"%.*s\", expected \"%.*s\"", o->f[2], is_null ? "null " : "", (int)(n < 200 ? n : 200), text, (int)(o->l[3] < 200 ? o->l[3] : 200), o->f[3]);
        result = dups(msg);
      }
      gtext_cssom_buffer_free(gtext_cssom_document_allocator(w->doc), text);
    }
  }
  else if ((strcmp(name, "mset") == 0 || strcmp(name, "mappend") == 0 || strcmp(name, "mdelete") == 0) && o->n == 4) {
    GTEXT_CSSOM_Media_List * m = NULL;
    GTEXT_CSS_Status st = r.rule ? gtext_cssom_rule_media(r.rule, &m) : (r.sheet ? gtext_cssom_sheet_media(r.sheet, &m) : GTEXT_CSS_E_INVALID);
    if (st != GTEXT_CSS_OK) {
      result = dups("media of an object that has none is read");
    }
    else {
      GTEXT_CSSOM_Outcome oc;
      st = name[1] == 's' ? gtext_cssom_media_set_text(m, o->f[2], o->l[2], &oc) : name[1] == 'a' ? gtext_cssom_media_append(m, o->f[2], o->l[2], &oc) : gtext_cssom_media_delete(m, o->f[2], o->l[2], &oc);
      const char * got = exception_of(st, eb, sizeof eb);
      if (strcmp(got, o->f[3]) != 0) {
        result = bad_call(name[1] == 's' ? "mediaText" : name[1] == 'a' ? "appendMedium" : "deleteMedium", o->f[3], got);
      }
      gtext_cssom_media_release(m);
    }
  }
  else if ((strcmp(name, "assert_mtext") == 0 && o->n == 3) || (strcmp(name, "assert_mlen") == 0 && o->n == 3) || (strcmp(name, "assert_mitem") == 0 && o->n == 4)) {
    GTEXT_CSSOM_Media_List * m = NULL;
    GTEXT_CSS_Status st = r.rule ? gtext_cssom_rule_media(r.rule, &m) : (r.sheet ? gtext_cssom_sheet_media(r.sheet, &m) : GTEXT_CSS_E_INVALID);
    if (st != GTEXT_CSS_OK) {
      result = dups("media of an object that has none is read");
    }
    else {
      char got[2048];
      const char * expected = o->f[name[8] == 'i' ? 3 : 2];
      got[0] = 0;
      if (strcmp(name, "assert_mtext") == 0) {
        char * t = NULL;
        size_t n = 0;
        if (gtext_cssom_media_text(m, &t, &n) == GTEXT_CSS_OK) {
          snprintf(got, sizeof got, "%.*s", (int)(n < sizeof got - 1 ? n : sizeof got - 1), t);
        }
        gtext_cssom_buffer_free(gtext_cssom_document_allocator(w->doc), t);
      }
      else if (strcmp(name, "assert_mlen") == 0) {
        size_t n = 0;
        gtext_cssom_media_length(m, &n);
        snprintf(got, sizeof got, "%zu", n);
      }
      else {
        char * t = NULL;
        size_t n = 0;
        bool absent = false;
        if (gtext_cssom_media_item(m, (size_t)strtoull(o->f[2], NULL, 10), &t, &n, &absent) == GTEXT_CSS_OK && !absent) {
          snprintf(got, sizeof got, "%.*s", (int)(n < sizeof got - 1 ? n : sizeof got - 1), t);
        }
        else {
          snprintf(got, sizeof got, "%s", "\x01null");
        }
        gtext_cssom_buffer_free(gtext_cssom_document_allocator(w->doc), t);
      }
      if (strcmp(got, expected) != 0) {
        snprintf(msg, sizeof msg, "%s is \"%s\", expected \"%s\"", name + 7, got, expected);
        result = dups(msg);
      }
      gtext_cssom_media_release(m);
    }
  }
  else if (strcmp(name, "assert_rcsstext") == 0 && o->n == 3) {
    if (!r.rule) {
      result = dups("the cssText of null is read");
    }
    else {
      char * text = NULL;
      size_t n = 0;
      const GTEXT_CSS_Status st = gtext_cssom_rule_css_text(r.rule, &text, &n);
      if (st != GTEXT_CSS_OK) {
        snprintf(msg, sizeof msg, "cssText: %s", gtext_css_status_string(st));
        result = dups(msg);
      }
      else if (n != o->l[2] || memcmp(text, o->f[2], n) != 0) {
        snprintf(msg, sizeof msg, "cssText is \"%.*s\", expected \"%.*s\"", (int)(n < 200 ? n : 200), text, (int)(o->l[2] < 200 ? o->l[2] : 200), o->f[2]);
        result = dups(msg);
      }
      gtext_cssom_buffer_free(gtext_cssom_document_allocator(w->doc), text);
    }
  }
  else {
    result = dups("not an editing op");
  }
  ref_release(&r);
  return result;
}

/// Run one op; returns NULL, or a message for a failed assertion (the caller frees it).
static char * run_op(world * w, const op * o, int * failed_status) {
  const char * name = o->f[0];
  char msg[1024];
  *failed_status = 0;
  if (strcmp(name, "sheet") == 0 || strcmp(name, "assert_nsheets") == 0 || strcmp(name, "assert_rlen") == 0 || strcmp(name, "assert_rtype") == 0 ||
      strcmp(name, "assert_rsame") == 0 || strcmp(name, "rstyle") == 0) {
    return run_rule_op(w, o);
  }
  if (strcmp(name, "bind") == 0 || strcmp(name, "rinsert") == 0 || strcmp(name, "radd") == 0 || strcmp(name, "rdelete") == 0 || strcmp(name, "rappend") == 0 ||
      strcmp(name, "rkfdelete") == 0 || strcmp(name, "assert_rcsstext") == 0 || strcmp(name, "rset") == 0 || strcmp(name, "assert_rattr") == 0 || strcmp(name, "mset") == 0 ||
      strcmp(name, "mappend") == 0 || strcmp(name, "mdelete") == 0 || strcmp(name, "assert_mtext") == 0 || strcmp(name, "assert_mlen") == 0 || strcmp(name, "assert_mitem") == 0) {
    return run_edit_op(w, o);
  }
  if (strcmp(name, "style") == 0 && o->n == 3) {
    if (!find_style(w, o->f[1])) {
      elem * e = find_elem(w, o->f[2]);
      if (!e) {
        return dups("style of an element the program did not make");
      }
      GTEXT_CSSOM_Attribute_Binding b = binding_of(w);
      GTEXT_CSSOM_Decl * d = NULL;
      if (gtext_cssom_inline_style(w->doc, e, &b, &d) != GTEXT_CSS_OK) {
        return dups("inline_style failed");
      }
      if (w->nstyles == w->capstyles) {
        w->capstyles *= 2;
        w->styles = realloc(w->styles, w->capstyles * sizeof *w->styles);
      }
      w->styles[w->nstyles].name = dups(o->f[1]);
      w->styles[w->nstyles].decl = d;
      w->styles[w->nstyles].error = NULL;
      w->nstyles++;
    }
    return NULL;
  }
  if (strcmp(name, "elem") == 0 && o->n == 4) {
    if (w->nelems == w->capelems) {
      w->capelems *= 2;
      w->elems = realloc(w->elems, w->capelems * sizeof *w->elems);
    }
    elem * e = &w->elems[w->nelems++];
    e->name = dups(o->f[1]);
    e->style = o->f[3][0] ? dups(o->f[3]) : NULL;
    e->version = 1;
    return NULL;
  }
  if (strcmp(name, "setattr") == 0 && o->n == 3) {
    elem * e = find_elem(w, o->f[1]);
    if (!e) {
      return dups("setAttribute of an element the program did not make");
    }
    set_attribute(w, e, o->f[2], o->l[2]);
    return NULL;
  }
  if (strcmp(name, "rmattr") == 0 && o->n == 2) {
    elem * e = find_elem(w, o->f[1]);
    if (!e) {
      return dups("removeAttribute of an element the program did not make");
    }
    remove_attribute(w, e);
    return NULL;
  }
  if (strcmp(name, "assert_getattr") == 0 && o->n == 3) {
    elem * e = find_elem(w, o->f[1]);
    if (!e) {
      return dups("getAttribute of an element the program did not make");
    }
    const char * got = e->style;
    const int want_null = strcmp(o->f[2], "\x01null") == 0;
    if (want_null ? got != NULL : (got == NULL || strlen(got) != o->l[2] || memcmp(got, o->f[2], o->l[2]) != 0)) {
      snprintf(msg, sizeof msg, "getAttribute('style') is %s%s%s, expected %s", got ? "\"" : "null", got ? got : "", got ? "\"" : "", o->f[2]);
      return dups(msg);
    }
    return NULL;
  }
  if (strcmp(name, "assert_escape") == 0 && o->n == 3) {
    char * out = NULL;
    size_t n = 0;
    GTEXT_CSS_Status st = gtext_cssom_escape_buffer(o->f[1], o->l[1], GTEXT_CSSOM_TEXT_WTF8, NULL, &out, &n);
    if (st != GTEXT_CSS_OK) {
      snprintf(msg, sizeof msg, "CSS.escape failed: %s", gtext_css_status_string(st));
      return dups(msg);
    }
    int ok = n == o->l[2] && memcmp(out, o->f[2], n) == 0;
    if (!ok) {
      snprintf(msg, sizeof msg, "CSS.escape(%s) is \"%s\", expected \"%s\"", o->f[1], out, o->f[2]);
    }
    gtext_cssom_buffer_free(NULL, out);
    return ok ? NULL : dups(msg);
  }
  /* the rest act on a style handle */
  handle * h = o->n >= 2 ? find_style(w, o->f[1]) : NULL;
  if (!h) {
    snprintf(msg, sizeof msg, "the op %s names a style the program did not make", name);
    return dups(msg);
  }
  if (h->error) {
    return dups(h->error);
  }
  GTEXT_CSSOM_Decl * d = h->decl;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (strcmp(name, "set") == 0 && o->n == 5) {
    st = gtext_cssom_decl_set_property(d, o->f[2], o->l[2], o->f[3], o->l[3], o->f[4], o->l[4], NULL);
  }
  else if (strcmp(name, "remove") == 0 && o->n == 3) {
    st = gtext_cssom_decl_remove_property(d, o->f[2], o->l[2], NULL, NULL, NULL);
  }
  else if (strcmp(name, "csstext") == 0 && o->n == 3) {
    st = gtext_cssom_decl_set_css_text(d, o->f[2], o->l[2], NULL);
  }
  else if (strcmp(name, "attr") == 0 && o->n == 4) {
    if (gtext_cssom_decl_has_attribute(d, o->f[2], o->l[2])) {
      st = gtext_cssom_decl_idl_set(d, o->f[2], o->l[2], o->f[3], o->l[3], NULL);
    }
    /* an attribute that is no CSS property's is an ordinary property of the script object: nothing happens to the declaration */
  }
  else if (strncmp(name, "assert_", 7) == 0 || strncmp(name, "anyof_assert_", 13) == 0) {
    const int anyof = strncmp(name, "anyof_", 6) == 0;
    const char * base = anyof ? name + 6 : name;
    const char * expected = o->f[o->n - 1];
    char * got = NULL;
    int is_undefined = 0;
    if (strcmp(base, "assert_value") == 0 && o->n == 4) {
      char * out = NULL;
      st = gtext_cssom_decl_get_property_value(d, o->f[2], o->l[2], &out, NULL);
      got = st == GTEXT_CSS_OK ? take(w, out) : NULL;
    }
    else if (strcmp(base, "assert_priority") == 0 && o->n == 4) {
      const char * p = NULL;
      st = gtext_cssom_decl_get_property_priority(d, o->f[2], o->l[2], &p);
      got = st == GTEXT_CSS_OK ? dups(p) : NULL;
    }
    else if (strcmp(base, "assert_csstext") == 0 && o->n == 3) {
      char * out = NULL;
      st = gtext_cssom_decl_css_text(d, &out, NULL);
      got = st == GTEXT_CSS_OK ? take(w, out) : NULL;
    }
    else if (strcmp(base, "assert_length") == 0 && o->n == 3) {
      size_t n = 0;
      st = gtext_cssom_decl_length(d, &n);
      char num[32];
      snprintf(num, sizeof num, "%zu", n);
      got = dups(num);
    }
    else if (strcmp(base, "assert_item") == 0 && o->n == 4) {
      size_t len = 0;
      st = gtext_cssom_decl_length(d, &len);
      const size_t index = (size_t)strtoull(o->f[2], NULL, 10);
      if (st == GTEXT_CSS_OK && index >= len) {
        is_undefined = 1;   /* `style[i]` past the end is undefined; `item(i)` is the empty string, and the extractor writes assertions on item() with a string */
        got = dups("");
      }
      else if (st == GTEXT_CSS_OK) {
        char * out = NULL;
        size_t n = 0;
        st = gtext_cssom_decl_item(d, index, &out, &n);
        got = st == GTEXT_CSS_OK ? take(w, out) : NULL;
      }
    }
    else if (strcmp(base, "assert_attr") == 0 && o->n == 4) {
      if (!gtext_cssom_decl_has_attribute(d, o->f[2], o->l[2])) {
        is_undefined = 1;   /* an expando: undefined */
        got = dups("");
      }
      else {
        char * out = NULL;
        st = gtext_cssom_decl_idl_get(d, o->f[2], o->l[2], &out, NULL);
        got = st == GTEXT_CSS_OK ? take(w, out) : NULL;
      }
    }
    else if (strcmp(base, "assert_remove") == 0 && o->n == 4) {
      char * out = NULL;
      st = gtext_cssom_decl_remove_property(d, o->f[2], o->l[2], &out, NULL, NULL);
      got = st == GTEXT_CSS_OK ? take(w, out) : NULL;
    }
    else {
      snprintf(msg, sizeof msg, "the op %s has %zu fields", name, o->n);
      return dups(msg);
    }
    if (st != GTEXT_CSS_OK || !got) {
      snprintf(msg, sizeof msg, "%s failed: %s", name, gtext_css_status_string(st));
      free(got);
      return dups(msg);
    }
    int ok = 0;
    if (anyof) {
      const char * p = expected;
      for (;;) {
        const char * end = strchr(p, '\x1f');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == strlen(got) && memcmp(p, got, n) == 0) {
          ok = 1;
          break;
        }
        if (!end) {
          break;
        }
        p = end + 1;
      }
    }
    else if (strcmp(expected, UNDEFINED) == 0) {
      ok = is_undefined;
    }
    else {
      ok = strlen(got) == o->l[o->n - 1] && memcmp(expected, got, o->l[o->n - 1]) == 0 && (!is_undefined || strcmp(base, "assert_item") == 0);
    }
    if (!ok) {
      snprintf(msg, sizeof msg, "%s %s: got \"%s\"%s, expected \"%s\"", name, o->f[2] && o->n > 3 ? o->f[2] : "", got, is_undefined ? " (undefined)" : "", expected);
      free(got);
      return dups(msg);
    }
    free(got);
    return NULL;
  }
  else {
    snprintf(msg, sizeof msg, "the op %s is not one the runner knows (%zu fields)", name, o->n);
    return dups(msg);
  }
  if (st != GTEXT_CSS_OK) {
    snprintf(msg, sizeof msg, "%s: %s", name, gtext_css_status_string(st));
    *failed_status = 1;
    return dups(msg);
  }
  return NULL;
}

/* ---- the programs -------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  char * file;
  int quirks;
  int in_test;
  int in_cleanup;   /* the ops after a `cleanup` op run whether or not the test failed */
  int index;
  int failed;
  char * message;
  world w;
  int open;
} program;

static void end_test(program * p) {
  if (p->in_test) {
    if (p->failed) {
      printf("R\t%s\t%d\tFAIL\t", p->file, p->index);
      for (const char * s = p->message ? p->message : ""; *s; s++) {
        if (*s == '\n') {
          fputs("\\n", stdout);
        }
        else if (*s == '\t') {
          fputs("\\t", stdout);
        }
        else {
          fputc(*s, stdout);
        }
      }
      fputc('\n', stdout);
    }
    else {
      printf("R\t%s\t%d\tPASS\n", p->file, p->index);
    }
  }
  free(p->message);
  p->message = NULL;
  p->in_test = 0;
  p->failed = 0;
}

static void end_program(program * p) {
  end_test(p);
  if (p->open) {
    world_close(&p->w);
    p->open = 0;
  }
  free(p->file);
  p->file = NULL;
}

int main(void) {
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
      p.quirks = atoi(o.f[1]);
      p.open = world_open(&p.w, p.quirks);
      if (!p.open) {
        printf("E\t%s\tcould not open the world\n", p.file);
      }
    }
    else if (line[0] == 'O' && line[1] == '\t' && p.file) {
      op o;
      if (!parse_op(line + 2, &o)) {
        printf("E\t%s\ttoo many fields\n", p.file);
        continue;
      }
      if (strcmp(o.f[0], "test") == 0 && o.n == 2) {
        end_test(&p);
        p.in_test = 1;
        p.in_cleanup = 0;
        p.index = atoi(o.f[1]);
        continue;
      }
      if (strcmp(o.f[0], "cleanup") == 0 && o.n == 1) {
        p.in_cleanup = 1;
        continue;
      }
      if (strcmp(o.f[0], "toplevel") == 0 && o.n == 1) {
        end_test(&p);   /* a statement of the page's own script between two tests: the test before it is over */
        continue;
      }
      if (!p.open || (p.in_test && p.failed && !p.in_cleanup)) {
        continue;   /* the exception of a failed assertion ended the test body */
      }
      int op_failed = 0;
      char * msg = run_op(&p.w, &o, &op_failed);
      if (msg) {
        if (p.in_test) {
          if (!p.failed) {
            p.failed = 1;
            p.message = msg;
          }
          else {
            free(msg);   /* the cleanup of a test that failed: the first failure is the one reported */
          }
        }
        else {
          printf("E\t%s\t%s\n", p.file, msg);   /* a setup statement of the page failed: the program is not what the page ran */
          free(msg);
        }
      }
    }
  }
  end_program(&p);
  free(line);
  return 0;
}
