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
 * The HTML tree builder's insertion modes: WHATWG HTML, "Tree construction",
 * 13.2.6.4 "The rules for parsing tokens in HTML content", as served at
 * html.spec.whatwg.org/multipage/parsing.html on 2026-10-11 (Living Standard,
 * last updated 9 October 2026).
 *
 * Implemented here: initial (13.2.6.4.1), before html (.2), before head (.3), in
 * head (.4), in head noscript (.5), after head (.6), in body (.7), text (.8),
 * after body (.17), in frameset (.18), after frameset (.19), after after body
 * (.20), after after frameset (.21), and since stage 3d in table (.9), in table
 * text (.10), in caption (.11), in column group (.12), in table body (.13), in
 * row (.14), in cell (.15) and in template (.16); "reset the insertion mode
 * appropriately" (13.2.4.1); the quirks-mode determination of the initial mode;
 * the generic raw text and RCDATA element parsing algorithms (13.2.6.2). Foster
 * parenting is not a mode: "in table" switches the flag on around the "in body"
 * rules, and the appropriate place for inserting a node (html_tree.c) reads it.
 *
 * **There is no "in select" mode in the standard as served on that date**: the
 * select element is parsed by ordinary "in body" rules (a `select` start tag,
 * `option`, `optgroup`, `hr`, `input` and the `select` end tag), and `select` is
 * one of the boundaries of "has an element in scope". They are implemented here
 * and are not a stub.
 *
 * Foreign content (3e) is in html_tree_foreign.c: the tree construction
 * dispatcher below picks its rules for a token before the current insertion mode
 * is asked, and `math` and `svg` start tags of "in body" call it. The fragment
 * parsing algorithm (13.4) is html_tree_fragment.c; the modes here read its
 * context element where the standard says "fragment case" (reset the insertion
 * mode, the select context, `</html>` after the body, `</frameset>`).
 *
 * Every function that handles a token returns R_DONE (consumed), R_REPROCESS
 * (the mode changed; handle the same token again in the new mode) or R_FAIL
 * (tb->status says why).
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "html_tree_private.h"

#define HT_ROOT(tb) gtext_markup_document_root((tb)->doc)

typedef int (*mode_fn)(GTEXT_HTML_Tree *, const Tk *);

static int h_in_body(GTEXT_HTML_Tree * tb, const Tk * tk);
static int h_in_head(GTEXT_HTML_Tree * tb, const Tk * tk);

static bool is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static size_t ws_prefix(const char * p, size_t n) {
  size_t i = 0;
  while (i < n && is_ws(p[i])) {
    i++;
  }
  return i;
}

static bool is_name(const GTEXT_HTML_Token * t, const char * s) {
  const size_t n = strlen(s);
  return t->name_length == n && memcmp(t->name, s, n) == 0;
}

/* ---- quirks mode -------------------------------------------------------- */

/* The public identifiers that put the document in quirks mode when the DOCTYPE's
   starts with them (ASCII case-insensitively), from the "initial" insertion mode. */
static const char * const k_quirks_prefixes[] = {
    "+//Silmaril//dtd html Pro v0r11 19970101//",
    "-//AS//DTD HTML 3.0 asWedit + extensions//",
    "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//",
    "-//IETF//DTD HTML 2.0 Level 1//",
    "-//IETF//DTD HTML 2.0 Level 2//",
    "-//IETF//DTD HTML 2.0 Strict Level 1//",
    "-//IETF//DTD HTML 2.0 Strict Level 2//",
    "-//IETF//DTD HTML 2.0 Strict//",
    "-//IETF//DTD HTML 2.0//",
    "-//IETF//DTD HTML 2.1E//",
    "-//IETF//DTD HTML 3.0//",
    "-//IETF//DTD HTML 3.2 Final//",
    "-//IETF//DTD HTML 3.2//",
    "-//IETF//DTD HTML 3//",
    "-//IETF//DTD HTML Level 0//",
    "-//IETF//DTD HTML Level 1//",
    "-//IETF//DTD HTML Level 2//",
    "-//IETF//DTD HTML Level 3//",
    "-//IETF//DTD HTML Strict Level 0//",
    "-//IETF//DTD HTML Strict Level 1//",
    "-//IETF//DTD HTML Strict Level 2//",
    "-//IETF//DTD HTML Strict Level 3//",
    "-//IETF//DTD HTML Strict//",
    "-//IETF//DTD HTML//",
    "-//Metrius//DTD Metrius Presentational//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 2.0 Tables//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 3.0 Tables//",
    "-//Netscape Comm. Corp.//DTD HTML//",
    "-//Netscape Comm. Corp.//DTD Strict HTML//",
    "-//O'Reilly and Associates//DTD HTML 2.0//",
    "-//O'Reilly and Associates//DTD HTML Extended 1.0//",
    "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//",
    "-//SQ//DTD HTML 2.0 HoTMetaL + extensions//",
    "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//",
    "-//SoftQuad//DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0//",
    "-//Spyglass//DTD HTML 2.0 Extended//",
    "-//Sun Microsystems Corp.//DTD HotJava HTML//",
    "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//",
    "-//W3C//DTD HTML 3 1995-03-24//",
    "-//W3C//DTD HTML 3.2 Draft//",
    "-//W3C//DTD HTML 3.2 Final//",
    "-//W3C//DTD HTML 3.2//",
    "-//W3C//DTD HTML 3.2S Draft//",
    "-//W3C//DTD HTML 4.0 Frameset//",
    "-//W3C//DTD HTML 4.0 Transitional//",
    "-//W3C//DTD HTML Experimental 19960712//",
    "-//W3C//DTD HTML Experimental 970421//",
    "-//W3C//DTD W3 HTML//",
    "-//W3O//DTD W3 HTML 3.0//",
    "-//WebTechs//DTD Mozilla HTML 2.0//",
    "-//WebTechs//DTD Mozilla HTML//",
};

/* ASCII case folding, never the locale's: "I" folds to "i" in every locale. */
static char ascii_lower(char c) {
  return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static bool ci_prefix_eq(const char * a, const char * b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) {
      return false;
    }
  }
  return true;
}

static bool ci_equal(const char * a, size_t an, const char * b) {
  return an == strlen(b) && ci_prefix_eq(a, b, an);
}

bool ht_ci_equal(const char * a, size_t an, const char * b) {
  return ci_equal(a, an, b);
}

static bool ci_starts(const char * s, size_t n, const char * prefix) {
  const size_t pn = strlen(prefix);
  return n >= pn && ci_prefix_eq(s, prefix, pn);
}

int ht_tree_quirks_mode(bool has_name, const char * name, size_t name_len,
    bool has_pub, const char * pub, size_t pub_len, bool has_sys,
    const char * sys, size_t sys_len, bool force_quirks) {
  if (force_quirks || !has_name || name_len != 4 || memcmp(name, "html", 4) != 0) {
    return GTEXT_MARKUP_MODE_QUIRKS;
  }
  if (has_pub) {
    if (ci_equal(pub, pub_len, "-//W3O//DTD W3 HTML Strict 3.0//EN//")
        || ci_equal(pub, pub_len, "-/W3C/DTD HTML 4.0 Transitional/EN")
        || ci_equal(pub, pub_len, "HTML")) {
      return GTEXT_MARKUP_MODE_QUIRKS;
    }
  }
  if (has_sys
      && ci_equal(sys, sys_len,
          "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd")) {
    return GTEXT_MARKUP_MODE_QUIRKS;
  }
  if (has_pub) {
    for (size_t i = 0; i < sizeof(k_quirks_prefixes) / sizeof(k_quirks_prefixes[0]); i++) {
      if (ci_starts(pub, pub_len, k_quirks_prefixes[i])) {
        return GTEXT_MARKUP_MODE_QUIRKS;
      }
    }
    const bool sys_empty = !has_sys || sys_len == 0;
    if (sys_empty
        && (ci_starts(pub, pub_len, "-//W3C//DTD HTML 4.01 Frameset//")
            || ci_starts(pub, pub_len, "-//W3C//DTD HTML 4.01 Transitional//"))) {
      return GTEXT_MARKUP_MODE_QUIRKS;
    }
    if (ci_starts(pub, pub_len, "-//W3C//DTD XHTML 1.0 Frameset//")
        || ci_starts(pub, pub_len, "-//W3C//DTD XHTML 1.0 Transitional//")) {
      return GTEXT_MARKUP_MODE_LIMITED_QUIRKS;
    }
    if (!sys_empty
        && (ci_starts(pub, pub_len, "-//W3C//DTD HTML 4.01 Frameset//")
            || ci_starts(pub, pub_len, "-//W3C//DTD HTML 4.01 Transitional//"))) {
      return GTEXT_MARKUP_MODE_LIMITED_QUIRKS;
    }
  }
  return GTEXT_MARKUP_MODE_NO_QUIRKS;
}

int gtext_html_tree_test_quirks_mode(const char * name, const char * public_id,
    const char * system_id, bool force_quirks) {
  return ht_tree_quirks_mode(name != NULL, name, name ? strlen(name) : 0,
      public_id != NULL, public_id, public_id ? strlen(public_id) : 0,
      system_id != NULL, system_id, system_id ? strlen(system_id) : 0, force_quirks);
}

/* ---- helpers ------------------------------------------------------------ */

static void set_quirks(GTEXT_HTML_Tree * tb) {
  /* "The parser cannot change the mode flag" and "iframe srcdoc document" are for a document
     that something else has already put in a mode; this parser makes its own. */
  gtext_markup_document_set_mode(tb->doc, GTEXT_MARKUP_MODE_QUIRKS);
}

static bool attribute_is(const Tk * tk, const char * name, const char * value) {
  const size_t nl = strlen(name);
  for (size_t i = 0; i < tk->nattrs; i++) {
    if (tk->attrs[i].name_length == nl && memcmp(tk->attrs[i].name, name, nl) == 0) {
      return ci_equal(tk->attrs[i].value, tk->attrs[i].value_length, value);
    }
  }
  return false;
}

static bool has_attribute(const Tk * tk, const char * name) {
  const size_t nl = strlen(name);
  for (size_t i = 0; i < tk->nattrs; i++) {
    if (tk->attrs[i].name_length == nl && memcmp(tk->attrs[i].name, name, nl) == 0) {
      return true;
    }
  }
  return false;
}

/* The generic raw text / RCDATA element parsing algorithms. */
static int generic_text(GTEXT_HTML_Tree * tb, const Tk * tk, GTEXT_HTML_TokenizerState state) {
  HT_CK(ht_insert_element(tb, tk, NULL));
  gtext_html_tokenizer_set_state(tb->tok, state);
  tb->orig_mode = tb->mode;
  tb->mode = M_TEXT;
  return R_DONE;
}

static int unexpected(GTEXT_HTML_Tree * tb, GTEXT_HTML_TreeErrorCode code) {
  ht_tree_error(tb, code);
  return tb->status == GTEXT_HTML_TREE_OK ? R_DONE : R_FAIL;
}

#define IGNORE_START(tb) unexpected((tb), GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG)
#define IGNORE_END(tb) unexpected((tb), GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG)

static int end_template(GTEXT_HTML_Tree * tb);
static int h_in_template(GTEXT_HTML_Tree * tb, const Tk * tk);

static bool is_current(const GTEXT_HTML_Tree * tb, uint32_t id) {
  return tb->n > 0 && HT_STACK_TOP(tb).id == id;
}

/* Is there a template element on the stack of open elements? */
static bool template_on_stack(const GTEXT_HTML_Tree * tb) {
  return tb->names[T_TEMPLATE].topidx != NONE_IDX;
}

/* "An HTML parser is parsing template contents if there is a template element on the stack of open
   elements, or the parser's fragment context element is a template element." */
/* Is the form element pointer set? (To an element made by this parser, or to the fragment context's form.) */
static bool form_pointer_set(const GTEXT_HTML_Tree * tb) {
  return tb->form_el != NULL || tb->form_ctx;
}

static bool parsing_template_contents(const GTEXT_HTML_Tree * tb) {
  return template_on_stack(tb) || tb->ctx_template;
}

/* Is there an element open that "has not been closed" as the end of a body means it:
   anything but dd dt li optgroup option p rb rp rt rtc tbody td tfoot th thead tr
   body html. */
static bool has_unclosed_element(const GTEXT_HTML_Tree * tb) {
  for (size_t i = 0; i < tb->n; i++) {
    switch (tb->stack[i].id) {
      case T_DD:
      case T_DT:
      case T_LI:
      case T_OPTGROUP:
      case T_OPTION:
      case T_P:
      case T_RB:
      case T_RP:
      case T_RT:
      case T_RTC:
      case T_TBODY:
      case T_TD:
      case T_TFOOT:
      case T_TH:
      case T_THEAD:
      case T_TR:
      case T_BODY:
      case T_HTML:
        break;
      default:
        return true;
    }
  }
  return false;
}

/* ---- "initial" ---------------------------------------------------------- */

static int h_initial(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment_into(tb, HT_ROOT(tb), tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi_into(tb, HT_ROOT(tb), tk->tok));
      return R_DONE;
    case TK_DOCTYPE: {
      const GTEXT_HTML_Token * t = tk->tok;
      const bool is_html = t->has_name && is_name(t, "html");
      const bool sys_ok = !t->has_system_id
          || (t->system_id_length == 19 && memcmp(t->system_id, "about:legacy-compat", 19) == 0);
      if (!is_html || t->has_public_id || !sys_ok) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_NON_CONFORMING_DOCTYPE);
      }
      HT_CK(ht_insert_doctype(tb, t));
      ht_set_mode_from_doctype(tb, t);
      tb->mode = M_BEFORE_HTML;
      return R_DONE;
    }
    default:
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_MISSING_DOCTYPE);
      set_quirks(tb);
      tb->mode = M_BEFORE_HTML;
      return R_REPROCESS;
  }
}

/* ---- "before html" ------------------------------------------------------ */

static int h_before_html(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_DOCTYPE:
      return unexpected(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
    case TK_COMMENT:
      HT_CK(ht_insert_comment_into(tb, HT_ROOT(tb), tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi_into(tb, HT_ROOT(tb), tk->tok));
      return R_DONE;
    case TK_START:
      if (tk->id == T_HTML) {
        HT_CK(ht_insert_element_into(tb, HT_ROOT(tb), T_HTML, tk->attrs, tk->nattrs, NULL));
        tb->mode = M_BEFORE_HEAD;
        return R_DONE;
      }
      break;
    case TK_END:
      if (tk->id != T_HEAD && tk->id != T_BODY && tk->id != T_HTML && tk->id != T_BR) {
        return IGNORE_END(tb);
      }
      break;
    default:
      break;
  }
  HT_CK(ht_insert_element_into(tb, HT_ROOT(tb), T_HTML, NULL, 0, NULL));
  tb->mode = M_BEFORE_HEAD;
  return R_REPROCESS;
}

/* ---- "before head" ------------------------------------------------------ */

static int h_before_head(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment(tb, tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi(tb, tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      return unexpected(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
    case TK_START:
      if (tk->id == T_HTML) {
        return h_in_body(tb, tk);
      }
      if (tk->id == T_HEAD) {
        HT_CK(ht_insert_element(tb, tk, &tb->head_el));
        tb->mode = M_IN_HEAD;
        return R_DONE;
      }
      break;
    case TK_END:
      if (tk->id != T_HEAD && tk->id != T_BODY && tk->id != T_HTML && tk->id != T_BR) {
        return IGNORE_END(tb);
      }
      break;
    default:
      break;
  }
  HT_CK(ht_insert_element_bare(tb, T_HEAD, &tb->head_el));
  tb->mode = M_IN_HEAD;
  return R_REPROCESS;
}

/* ---- "in head" ---------------------------------------------------------- */

/* The end tag "template" of "in head" (and so of every mode that defers to it). */
static int end_template(GTEXT_HTML_Tree * tb) {
  if (!template_on_stack(tb)) {
    return IGNORE_END(tb);
  }
  ht_generate_all_implied_end_tags_thoroughly(tb);
  if (!is_current(tb, T_TEMPLATE)) {
    ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
  }
  ht_stack_pop_until(tb, T_TEMPLATE);
  ht_fmt_clear_to_marker(tb);
  ht_tmpl_pop(tb);
  tb->mode = ht_tree_reset_mode(tb);
  return tb->status == GTEXT_HTML_TREE_OK ? R_DONE : R_FAIL;
}

static int h_in_head(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment(tb, tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi(tb, tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      return unexpected(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
    case TK_START:
      switch (tk->id) {
        case T_HTML:
          return h_in_body(tb, tk);
        case T_BASE:
        case T_BASEFONT:
        case T_BGSOUND:
        case T_LINK:
          HT_CK(ht_insert_element(tb, tk, NULL));
          HT_CK(ht_pop_current_acknowledge(tb, tk));
          return R_DONE;
        case T_META:
          HT_CK(ht_insert_element(tb, tk, NULL));
          HT_CK(ht_pop_current_acknowledge(tb, tk));
          HT_CK(ht_meta_encoding(tb, tk->attrs, tk->nattrs));
          return R_DONE;
        case T_TITLE:
          return generic_text(tb, tk, GTEXT_HTML_STATE_RCDATA);
        case T_NOSCRIPT:
          if (!tb->opt.scripting) {
            HT_CK(ht_insert_element(tb, tk, NULL));
            tb->mode = M_IN_HEAD_NOSCRIPT;
            return R_DONE;
          }
          return generic_text(tb, tk, GTEXT_HTML_STATE_RAWTEXT);
        case T_NOFRAMES:
        case T_STYLE:
          return generic_text(tb, tk, GTEXT_HTML_STATE_RAWTEXT);
        case T_SCRIPT:
          return generic_text(tb, tk, GTEXT_HTML_STATE_SCRIPT_DATA);
        case T_TEMPLATE:
          /* The scripting-facing steps of the standard's template start tag (declarative shadow
             roots, content patching) need a live document to act on; without one they end in
             "insert an HTML element for the token", which is what is done. */
          HT_CK(ht_fmt_push_marker(tb));
          tb->frameset_ok = false;
          HT_CK(ht_tmpl_push(tb, M_IN_TEMPLATE));
          HT_CK(ht_insert_element(tb, tk, NULL));
          tb->mode = M_IN_TEMPLATE;
          return R_DONE;
        case T_HEAD:
          return IGNORE_START(tb);
        default:
          break;
      }
      break;
    case TK_END:
      if (tk->id == T_HEAD) {
        ht_stack_pop(tb);
        tb->mode = M_AFTER_HEAD;
        return R_DONE;
      }
      if (tk->id == T_TEMPLATE) {
        return end_template(tb);
      }
      if (tk->id != T_BODY && tk->id != T_HTML && tk->id != T_BR) {
        return IGNORE_END(tb);
      }
      break;
    default:
      break;
  }
  ht_stack_pop(tb);
  tb->mode = M_AFTER_HEAD;
  return R_REPROCESS;
}

/* ---- "in head noscript" ------------------------------------------------- */

static int h_in_head_noscript(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_DOCTYPE:
      return unexpected(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
    case TK_COMMENT:
    case TK_PI:
      return h_in_head(tb, tk);
    case TK_START:
      switch (tk->id) {
        case T_HTML:
          return h_in_body(tb, tk);
        case T_BASEFONT:
        case T_BGSOUND:
        case T_LINK:
        case T_META:
        case T_NOFRAMES:
        case T_STYLE:
          return h_in_head(tb, tk);
        case T_HEAD:
        case T_NOSCRIPT:
          return IGNORE_START(tb);
        default:
          break;
      }
      break;
    case TK_END:
      if (tk->id == T_NOSCRIPT) {
        ht_stack_pop(tb);
        tb->mode = M_IN_HEAD;
        return R_DONE;
      }
      if (tk->id != T_BR) {
        return IGNORE_END(tb);
      }
      break;
    default:
      break;
  }
  ht_tree_error(tb, tk->type == TK_EOF ? GTEXT_HTML_TREE_ERR_UNEXPECTED_EOF
                                    : tk->type == TK_START
          ? GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG
          : GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG);
  ht_stack_pop(tb);
  tb->mode = M_IN_HEAD;
  return R_REPROCESS;
}

/* ---- "after head" ------------------------------------------------------- */

static int h_after_head(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment(tb, tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi(tb, tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      return unexpected(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
    case TK_START:
      switch (tk->id) {
        case T_HTML:
          return h_in_body(tb, tk);
        case T_BODY:
          HT_CK(ht_insert_element(tb, tk, NULL));
          tb->frameset_ok = false;
          tb->mode = M_IN_BODY;
          return R_DONE;
        case T_FRAMESET:
          HT_CK(ht_insert_element(tb, tk, NULL));
          tb->mode = M_IN_FRAMESET;
          return R_DONE;
        case T_TEMPLATE:
        case T_BASE:
        case T_BASEFONT:
        case T_BGSOUND:
        case T_LINK:
        case T_META:
        case T_NOFRAMES:
        case T_SCRIPT:
        case T_STYLE:
        case T_TITLE: {
          ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG);
          HT_CK(tb->status == GTEXT_HTML_TREE_OK);
          HT_CK(ht_stack_push(tb, tb->head_el, T_HEAD, NULL));
          const size_t at = tb->n - 1;
          const int r = h_in_head(tb, tk);
          if (r == R_FAIL) {
            return r;
          }
          ht_stack_remove_at(tb, at);
          return r;
        }
        case T_HEAD:
          return IGNORE_START(tb);
        default:
          break;
      }
      break;
    case TK_END:
      if (tk->id == T_TEMPLATE) {
        return h_in_head(tb, tk);
      }
      if (tk->id != T_BODY && tk->id != T_HTML && tk->id != T_BR) {
        return IGNORE_END(tb);
      }
      break;
    default:
      break;
  }
  HT_CK(ht_insert_element_bare(tb, T_BODY, NULL));
  tb->frameset_ok = true;
  tb->mode = M_IN_BODY;
  return R_REPROCESS;
}

/* ---- "text" ------------------------------------------------------------- */

static int h_text(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_EOF:
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_EOF);
      ht_stack_pop(tb);
      tb->mode = tb->orig_mode;
      return R_REPROCESS;
    case TK_END:
      ht_stack_pop(tb);
      tb->mode = tb->orig_mode;
      return R_DONE;
    default:
      return R_DONE;
  }
}

/* ---- "in body" ---------------------------------------------------------- */

static int body_start_li(GTEXT_HTML_Tree * tb, const Tk * tk) {
  tb->frameset_ok = false;
  const uint32_t j = tb->names[T_LI].topidx;
  if (j != NONE_IDX && j >= HT_STACK_TOP(tb).fl_li) {
    ht_generate_implied_end_tags(tb, T_LI);
    if (!is_current(tb, T_LI)) {
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
    }
    ht_stack_pop_until(tb, T_LI);
  }
  HT_CK(ht_close_p_if_in_button_scope(tb));
  HT_CK(ht_insert_element(tb, tk, NULL));
  return R_DONE;
}

static int body_start_dd_dt(GTEXT_HTML_Tree * tb, const Tk * tk) {
  tb->frameset_ok = false;
  const uint32_t jd = tb->names[T_DD].topidx;
  const uint32_t jt = tb->names[T_DT].topidx;
  const uint32_t floor = HT_STACK_TOP(tb).fl_li;
  uint32_t found = NONE_IDX;
  uint32_t which = 0;
  if (jd != NONE_IDX && jd >= floor) {
    found = jd;
    which = T_DD;
  }
  if (jt != NONE_IDX && jt >= floor && (found == NONE_IDX || jt > found)) {
    found = jt;
    which = T_DT;
  }
  if (found != NONE_IDX) {
    ht_generate_implied_end_tags(tb, which);
    if (!is_current(tb, which)) {
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
    }
    ht_stack_pop_until(tb, which);
  }
  HT_CK(ht_close_p_if_in_button_scope(tb));
  HT_CK(ht_insert_element(tb, tk, NULL));
  return R_DONE;
}

static int body_start_a(GTEXT_HTML_Tree * tb, const Tk * tk) {
  fmt_entry * old = tb->names[T_A].fmt_last;
  if (old && ht_fmt_after_marker(tb, old)) {
    ht_tree_error(tb, GTEXT_HTML_TREE_ERR_MISNESTED_FORMATTING);
    const uint64_t serial = old->serial;
    HT_CK(ht_adoption_agency(tb, T_A));
    if (old->kind == FE_ELEMENT && old->serial == serial) {
      if (old->stack_idx != SIZE_MAX) {
        ht_stack_remove_at(tb, old->stack_idx);
      }
      ht_fmt_remove(tb, old);
    }
  }
  HT_CK(ht_fmt_reconstruct(tb));
  HT_CK(ht_insert_element(tb, tk, NULL));
  HT_CK(ht_fmt_push_element(tb, tk));
  return R_DONE;
}

static int body_start_formatting(GTEXT_HTML_Tree * tb, const Tk * tk) {
  HT_CK(ht_fmt_reconstruct(tb));
  HT_CK(ht_insert_element(tb, tk, NULL));
  HT_CK(ht_fmt_push_element(tb, tk));
  return R_DONE;
}

static int body_start_nobr(GTEXT_HTML_Tree * tb, const Tk * tk) {
  HT_CK(ht_fmt_reconstruct(tb));
  if (ht_stack_in_scope(tb, T_NOBR, SCOPE_GEN)) {
    ht_tree_error(tb, GTEXT_HTML_TREE_ERR_MISNESTED_FORMATTING);
    HT_CK(ht_adoption_agency(tb, T_NOBR));
    HT_CK(ht_fmt_reconstruct(tb));
  }
  HT_CK(ht_insert_element(tb, tk, NULL));
  HT_CK(ht_fmt_push_element(tb, tk));
  return R_DONE;
}

/* area, br, embed, img, keygen, wbr */
static int body_start_void(GTEXT_HTML_Tree * tb, const Tk * tk) {
  HT_CK(ht_fmt_reconstruct(tb));
  HT_CK(ht_insert_element(tb, tk, NULL));
  HT_CK(ht_pop_current_acknowledge(tb, tk));
  tb->frameset_ok = false;
  return R_DONE;
}

static int body_start_option(GTEXT_HTML_Tree * tb, const Tk * tk) {
  if (ht_stack_in_scope(tb, T_SELECT, SCOPE_GEN)) {
    ht_generate_implied_end_tags(tb, T_OPTGROUP);
    if (ht_stack_in_scope(tb, T_OPTION, SCOPE_GEN)) {
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_ELEMENT_IN_CONTEXT);
    }
  }
  else if (is_current(tb, T_OPTION)) {
    ht_stack_pop(tb);
  }
  HT_CK(ht_fmt_reconstruct(tb));
  HT_CK(ht_insert_element(tb, tk, NULL));
  return R_DONE;
}

static int body_start_optgroup(GTEXT_HTML_Tree * tb, const Tk * tk) {
  if (ht_stack_in_scope(tb, T_SELECT, SCOPE_GEN)) {
    ht_generate_implied_end_tags(tb, NONE_IDX);
    if (ht_stack_in_scope(tb, T_OPTION, SCOPE_GEN)
        || ht_stack_in_scope(tb, T_OPTGROUP, SCOPE_GEN)) {
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_ELEMENT_IN_CONTEXT);
    }
  }
  else if (is_current(tb, T_OPTION)) {
    ht_stack_pop(tb);
  }
  HT_CK(ht_fmt_reconstruct(tb));
  HT_CK(ht_insert_element(tb, tk, NULL));
  return R_DONE;
}

static int body_start(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->id) {
    case T_HTML:
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG);
      if (template_on_stack(tb)) {
        return R_DONE;
      }
      HT_CK(ht_merge_attributes(tb, tb->stack[0].node, tk->attrs, tk->nattrs));
      return R_DONE;
    case T_BASE:
    case T_BASEFONT:
    case T_BGSOUND:
    case T_LINK:
    case T_META:
    case T_NOFRAMES:
    case T_SCRIPT:
    case T_STYLE:
    case T_TITLE:
      return h_in_head(tb, tk);
    case T_TEMPLATE:
      return h_in_head(tb, tk);
    case T_BODY:
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG);
      if (tb->n == 1 || tb->stack[1].id != T_BODY || template_on_stack(tb)) {
        return R_DONE;
      }
      tb->frameset_ok = false;
      HT_CK(ht_merge_attributes(tb, tb->stack[1].node, tk->attrs, tk->nattrs));
      return R_DONE;
    case T_FRAMESET:
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG);
      if (tb->n == 1 || tb->stack[1].id != T_BODY || !tb->frameset_ok) {
        return R_DONE;
      }
      markup_detach_unchecked(tb->stack[1].node);
      while (tb->n > 1) {
        ht_stack_pop(tb);
      }
      HT_CK(ht_insert_element(tb, tk, NULL));
      tb->mode = M_IN_FRAMESET;
      return R_DONE;
    case T_ADDRESS:
    case T_ARTICLE:
    case T_ASIDE:
    case T_BLOCKQUOTE:
    case T_CENTER:
    case T_DETAILS:
    case T_DIALOG:
    case T_DIR:
    case T_DIV:
    case T_DL:
    case T_FIELDSET:
    case T_FIGCAPTION:
    case T_FIGURE:
    case T_FOOTER:
    case T_HEADER:
    case T_HGROUP:
    case T_MAIN:
    case T_MENU:
    case T_NAV:
    case T_OL:
    case T_P:
    case T_SEARCH:
    case T_SECTION:
    case T_SUMMARY:
    case T_UL:
      HT_CK(ht_close_p_if_in_button_scope(tb));
      HT_CK(ht_insert_element(tb, tk, NULL));
      return R_DONE;
    case T_H1:
    case T_H2:
    case T_H3:
    case T_H4:
    case T_H5:
    case T_H6:
      HT_CK(ht_close_p_if_in_button_scope(tb));
      if (ht_is_heading_id(HT_STACK_TOP(tb).id)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_ELEMENT_IN_CONTEXT);
        ht_stack_pop(tb);
      }
      HT_CK(ht_insert_element(tb, tk, NULL));
      return R_DONE;
    case T_PRE:
    case T_LISTING:
      HT_CK(ht_close_p_if_in_button_scope(tb));
      HT_CK(ht_insert_element(tb, tk, NULL));
      tb->skip_lf = true;
      tb->frameset_ok = false;
      return R_DONE;
    case T_FORM: {
      /* "If the form element pointer is not null, and the parser is not parsing template contents, then this
         is a parse error; ignore the token. Otherwise: ... Insert an HTML element for the token, and, if the
         parser is not parsing template contents, set the form element pointer to point to the element
         created." (13.2.6.4.7; "parsing template contents" includes a template fragment context.) */
      const bool in_template = parsing_template_contents(tb);
      if (form_pointer_set(tb) && !in_template) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_NESTED_FORM);
        return R_DONE;
      }
      HT_CK(ht_close_p_if_in_button_scope(tb));
      HT_CK(ht_insert_element(tb, tk, in_template ? NULL : &tb->form_el));
      return R_DONE;
    }
    case T_LI:
      return body_start_li(tb, tk);
    case T_DD:
    case T_DT:
      return body_start_dd_dt(tb, tk);
    case T_PLAINTEXT:
      HT_CK(ht_close_p_if_in_button_scope(tb));
      HT_CK(ht_insert_element(tb, tk, NULL));
      gtext_html_tokenizer_set_state(tb->tok, GTEXT_HTML_STATE_PLAINTEXT);
      return R_DONE;
    case T_BUTTON:
      if (ht_stack_in_scope(tb, T_BUTTON, SCOPE_GEN)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_ELEMENT_IN_CONTEXT);
        ht_generate_implied_end_tags(tb, NONE_IDX);
        ht_stack_pop_until(tb, T_BUTTON);
      }
      HT_CK(ht_fmt_reconstruct(tb));
      HT_CK(ht_insert_element(tb, tk, NULL));
      tb->frameset_ok = false;
      return R_DONE;
    case T_A:
      return body_start_a(tb, tk);
    case T_B:
    case T_BIG:
    case T_CODE:
    case T_EM:
    case T_FONT:
    case T_I:
    case T_S:
    case T_SMALL:
    case T_STRIKE:
    case T_STRONG:
    case T_TT:
    case T_U:
      return body_start_formatting(tb, tk);
    case T_NOBR:
      return body_start_nobr(tb, tk);
    case T_APPLET:
    case T_MARQUEE:
    case T_OBJECT:
      HT_CK(ht_fmt_reconstruct(tb));
      HT_CK(ht_insert_element(tb, tk, NULL));
      HT_CK(ht_fmt_push_marker(tb));
      tb->frameset_ok = false;
      return R_DONE;
    case T_TABLE:
      if (gtext_markup_document_mode(tb->doc) != GTEXT_MARKUP_MODE_QUIRKS) {
        HT_CK(ht_close_p_if_in_button_scope(tb));
      }
      HT_CK(ht_insert_element(tb, tk, NULL));
      tb->frameset_ok = false;
      tb->mode = M_IN_TABLE;
      return R_DONE;
    case T_AREA:
    case T_BR:
    case T_EMBED:
    case T_IMG:
    case T_KEYGEN:
    case T_WBR:
      return body_start_void(tb, tk);
    case T_INPUT:
      if (tb->ctx_select) {
        /* The fragment context element is a select element (fragment case). */
        return IGNORE_START(tb);
      }
      if (ht_stack_in_scope(tb, T_SELECT, SCOPE_GEN)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_ELEMENT_IN_CONTEXT);
        ht_stack_pop_until(tb, T_SELECT);
      }
      HT_CK(ht_fmt_reconstruct(tb));
      HT_CK(ht_insert_element(tb, tk, NULL));
      HT_CK(ht_pop_current_acknowledge(tb, tk));
      if (!has_attribute(tk, "type") || !attribute_is(tk, "type", "hidden")) {
        tb->frameset_ok = false;
      }
      return R_DONE;
    case T_PARAM:
    case T_SOURCE:
    case T_TRACK:
      HT_CK(ht_insert_element(tb, tk, NULL));
      HT_CK(ht_pop_current_acknowledge(tb, tk));
      return R_DONE;
    case T_HR:
      HT_CK(ht_close_p_if_in_button_scope(tb));
      if (ht_stack_in_scope(tb, T_SELECT, SCOPE_GEN)) {
        ht_generate_implied_end_tags(tb, NONE_IDX);
        if (ht_stack_in_scope(tb, T_OPTION, SCOPE_GEN)
            || ht_stack_in_scope(tb, T_OPTGROUP, SCOPE_GEN)) {
          ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_ELEMENT_IN_CONTEXT);
        }
      }
      HT_CK(ht_insert_element(tb, tk, NULL));
      HT_CK(ht_pop_current_acknowledge(tb, tk));
      tb->frameset_ok = false;
      return R_DONE;
    case T_IMAGE: {
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_DEPRECATED_TAG);
      Tk img = *tk;
      img.id = T_IMG;
      return body_start(tb, &img);
    }
    case T_TEXTAREA:
      HT_CK(ht_insert_element(tb, tk, NULL));
      tb->skip_lf = true;
      gtext_html_tokenizer_set_state(tb->tok, GTEXT_HTML_STATE_RCDATA);
      tb->orig_mode = tb->mode;
      tb->frameset_ok = false;
      tb->mode = M_TEXT;
      return R_DONE;
    case T_XMP:
      HT_CK(ht_close_p_if_in_button_scope(tb));
      HT_CK(ht_fmt_reconstruct(tb));
      tb->frameset_ok = false;
      return generic_text(tb, tk, GTEXT_HTML_STATE_RAWTEXT);
    case T_IFRAME:
      tb->frameset_ok = false;
      return generic_text(tb, tk, GTEXT_HTML_STATE_RAWTEXT);
    case T_NOEMBED:
      return generic_text(tb, tk, GTEXT_HTML_STATE_RAWTEXT);
    case T_NOSCRIPT:
      if (tb->opt.scripting) {
        return generic_text(tb, tk, GTEXT_HTML_STATE_RAWTEXT);
      }
      HT_CK(ht_fmt_reconstruct(tb));
      HT_CK(ht_insert_element(tb, tk, NULL));
      return R_DONE;
    case T_SELECT:
      if (tb->ctx_select) {
        /* The fragment context element is a select element (fragment case): "Parse error. Ignore
           the token." */
        return IGNORE_START(tb);
      }
      if (ht_stack_in_scope(tb, T_SELECT, SCOPE_GEN)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_ELEMENT_IN_CONTEXT);
        ht_stack_pop_until(tb, T_SELECT);
        return R_DONE;
      }
      HT_CK(ht_fmt_reconstruct(tb));
      HT_CK(ht_insert_element(tb, tk, NULL));
      tb->frameset_ok = false;
      return R_DONE;
    case T_OPTION:
      return body_start_option(tb, tk);
    case T_OPTGROUP:
      return body_start_optgroup(tb, tk);
    case T_RB:
    case T_RTC:
      if (ht_stack_in_scope(tb, T_RUBY, SCOPE_GEN)) {
        ht_generate_implied_end_tags(tb, NONE_IDX);
        if (!is_current(tb, T_RUBY)) {
          ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_ELEMENT_IN_CONTEXT);
        }
      }
      HT_CK(ht_insert_element(tb, tk, NULL));
      return R_DONE;
    case T_RP:
    case T_RT:
      if (ht_stack_in_scope(tb, T_RUBY, SCOPE_GEN)) {
        ht_generate_implied_end_tags(tb, T_RTC);
        if (!is_current(tb, T_RTC) && !is_current(tb, T_RUBY)) {
          ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_ELEMENT_IN_CONTEXT);
        }
      }
      HT_CK(ht_insert_element(tb, tk, NULL));
      return R_DONE;
    case T_MATH:
      return ht_body_start_foreign(tb, tk, NS_MATH);
    case T_SVG:
      return ht_body_start_foreign(tb, tk, NS_SVG);
    case T_CAPTION:
    case T_COL:
    case T_COLGROUP:
    case T_FRAME:
    case T_HEAD:
    case T_TBODY:
    case T_TD:
    case T_TFOOT:
    case T_TH:
    case T_THEAD:
    case T_TR:
      return IGNORE_START(tb);
    default:
      HT_CK(ht_fmt_reconstruct(tb));
      HT_CK(ht_insert_element(tb, tk, NULL));
      return R_DONE;
  }
}

/* The index of `node` in the stack, scanning down from the top, or SIZE_MAX. */
static size_t stack_find_node(GTEXT_HTML_Tree * tb, const HtNode * node) {
  for (size_t i = tb->n; i > 0; i--) {
    tb->work++;
    if (tb->stack[i - 1].node == node) {
      return i - 1;
    }
  }
  return SIZE_MAX;
}

static int body_end(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->id) {
    case T_TEMPLATE:
      return h_in_head(tb, tk);
    case T_BODY:
    case T_HTML:
      if (!ht_stack_in_scope(tb, T_BODY, SCOPE_GEN)) {
        return IGNORE_END(tb);
      }
      if (has_unclosed_element(tb)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
      }
      tb->mode = M_AFTER_BODY;
      return tk->id == T_HTML ? R_REPROCESS : R_DONE;
    case T_ADDRESS:
    case T_ARTICLE:
    case T_ASIDE:
    case T_BLOCKQUOTE:
    case T_BUTTON:
    case T_CENTER:
    case T_DETAILS:
    case T_DIALOG:
    case T_DIR:
    case T_DIV:
    case T_DL:
    case T_FIELDSET:
    case T_FIGCAPTION:
    case T_FIGURE:
    case T_FOOTER:
    case T_HEADER:
    case T_HGROUP:
    case T_LISTING:
    case T_MAIN:
    case T_MENU:
    case T_NAV:
    case T_OL:
    case T_PRE:
    case T_SEARCH:
    case T_SECTION:
    case T_SELECT:
    case T_SUMMARY:
    case T_UL:
      if (!ht_stack_in_scope(tb, tk->id, SCOPE_GEN)) {
        return IGNORE_END(tb);
      }
      ht_generate_implied_end_tags(tb, NONE_IDX);
      if (!is_current(tb, tk->id)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
      }
      ht_stack_pop_until(tb, tk->id);
      return R_DONE;
    case T_FORM: {
      if (parsing_template_contents(tb)) {
        if (!ht_stack_in_scope(tb, T_FORM, SCOPE_GEN)) {
          return IGNORE_END(tb);
        }
        ht_generate_implied_end_tags(tb, NONE_IDX);
        if (!is_current(tb, T_FORM)) {
          ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
        }
        ht_stack_pop_until(tb, T_FORM);
        return R_DONE;
      }
      /* "If the parser is not parsing template contents: 1. Let node be the element that the form element
         pointer is set to, or null if it is not set to an element. 2. Set the form element pointer to null.
         3. If node is null or if the stack of open elements does not have node in scope, ... ignore the
         token." A context's form is such an element, and is never on the stack. */
      HtNode * node = tb->form_el;
      tb->form_el = NULL;
      tb->form_ctx = false;
      size_t at = SIZE_MAX;
      if (node) {
        at = stack_find_node(tb, node);
      }
      if (!node || at == SIZE_MAX || at < HT_STACK_TOP(tb).fl_gen) {
        return IGNORE_END(tb);
      }
      ht_generate_implied_end_tags(tb, NONE_IDX);
      if (HT_STACK_TOP(tb).node != node) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
      }
      /* The implied end tags can have popped nothing that is the form itself. */
      at = stack_find_node(tb, node);
      ht_stack_remove_at(tb, at);
      return R_DONE;
    }
    case T_P:
      if (!ht_stack_in_scope(tb, T_P, SCOPE_BUTTON)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG);
        HT_CK(ht_insert_element_bare(tb, T_P, NULL));
      }
      HT_CK(ht_close_p_element(tb));
      return R_DONE;
    case T_LI:
      if (!ht_stack_in_scope(tb, T_LI, SCOPE_LIST)) {
        return IGNORE_END(tb);
      }
      ht_generate_implied_end_tags(tb, T_LI);
      if (!is_current(tb, T_LI)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
      }
      ht_stack_pop_until(tb, T_LI);
      return R_DONE;
    case T_DD:
    case T_DT:
      if (!ht_stack_in_scope(tb, tk->id, SCOPE_GEN)) {
        return IGNORE_END(tb);
      }
      ht_generate_implied_end_tags(tb, tk->id);
      if (!is_current(tb, tk->id)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
      }
      ht_stack_pop_until(tb, tk->id);
      return R_DONE;
    case T_H1:
    case T_H2:
    case T_H3:
    case T_H4:
    case T_H5:
    case T_H6:
      if (!ht_stack_has_heading_in_scope(tb)) {
        return IGNORE_END(tb);
      }
      ht_generate_implied_end_tags(tb, NONE_IDX);
      if (!is_current(tb, tk->id)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
      }
      ht_stack_pop_until_heading(tb);
      return R_DONE;
    case T_A:
    case T_B:
    case T_BIG:
    case T_CODE:
    case T_EM:
    case T_FONT:
    case T_I:
    case T_NOBR:
    case T_S:
    case T_SMALL:
    case T_STRIKE:
    case T_STRONG:
    case T_TT:
    case T_U:
      HT_CK(ht_adoption_agency(tb, tk->id));
      return R_DONE;
    case T_APPLET:
    case T_MARQUEE:
    case T_OBJECT:
      if (!ht_stack_in_scope(tb, tk->id, SCOPE_GEN)) {
        return IGNORE_END(tb);
      }
      ht_generate_implied_end_tags(tb, NONE_IDX);
      if (!is_current(tb, tk->id)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
      }
      ht_stack_pop_until(tb, tk->id);
      ht_fmt_clear_to_marker(tb);
      return R_DONE;
    case T_BR: {
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_DEPRECATED_TAG);
      Tk br = *tk;
      br.type = TK_START;
      br.attrs = NULL;
      br.nattrs = 0;
      br.self_closing = false;
      return body_start(tb, &br);
    }
    default:
      HT_CK(ht_any_other_end_tag(tb, tk->id));
      return R_DONE;
  }
}

static int h_in_body(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment(tb, tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi(tb, tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      return unexpected(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
    case TK_START:
      return body_start(tb, tk);
    case TK_END:
      return body_end(tb, tk);
    case TK_EOF:
      if (tb->ntmpl > 0) {
        return h_in_template(tb, tk);
      }
      if (has_unclosed_element(tb)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_EOF);
      }
      tb->stopped = true;
      return R_DONE;
    default:
      return R_DONE;
  }
}

/* Characters in "in body": NUL is ignored, the rest is reconstructed into and inserted. */
static bool body_chars(GTEXT_HTML_Tree * tb, const char * p, size_t n) {
  while (n > 0) {
    const char * nul = memchr(p, '\0', n);
    const size_t len = nul ? (size_t)(nul - p) : n;
    if (len > 0) {
      if (!ht_fmt_reconstruct(tb) || !ht_insert_text(tb, p, len)) {
        return false;
      }
      if (ws_prefix(p, len) != len) {
        tb->frameset_ok = false;
      }
    }
    if (!nul) {
      break;
    }
    ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_NULL_CHARACTER);
    p += len + 1;
    n -= len + 1;
  }
  return tb->status == GTEXT_HTML_TREE_OK;
}

/* ---- the "after" modes and the framesets -------------------------------- */

static int h_after_body(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment_into(tb, tb->stack[0].node, tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi_into(tb, tb->stack[0].node, tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      return unexpected(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
    case TK_START:
      if (tk->id == T_HTML) {
        return h_in_body(tb, tk);
      }
      break;
    case TK_END:
      if (tk->id == T_HTML) {
        if (tb->frag) {
          /* The fragment context element is non-null: a parse error, ignore the token (fragment case). */
          return IGNORE_END(tb);
        }
        tb->mode = M_AFTER_AFTER_BODY;
        return R_DONE;
      }
      break;
    case TK_EOF:
      tb->stopped = true;
      return R_DONE;
    default:
      break;
  }
  ht_tree_error(tb, tk->type == TK_START ? GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG
                                      : GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG);
  tb->mode = M_IN_BODY;
  return R_REPROCESS;
}

static int h_in_frameset(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment(tb, tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi(tb, tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      return unexpected(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
    case TK_START:
      switch (tk->id) {
        case T_HTML:
          return h_in_body(tb, tk);
        case T_FRAMESET:
          HT_CK(ht_insert_element(tb, tk, NULL));
          return R_DONE;
        case T_FRAME:
          HT_CK(ht_insert_element(tb, tk, NULL));
          HT_CK(ht_pop_current_acknowledge(tb, tk));
          return R_DONE;
        case T_NOFRAMES:
          return h_in_head(tb, tk);
        default:
          return IGNORE_START(tb);
      }
    case TK_END:
      if (tk->id == T_FRAMESET) {
        if (tb->n == 1) {
          return IGNORE_END(tb);
        }
        ht_stack_pop(tb);
        /* "If the parser's fragment context element is null and the current node is no longer a frameset
           element, then switch the insertion mode to "after frameset"." */
        if (!tb->frag && !is_current(tb, T_FRAMESET)) {
          tb->mode = M_AFTER_FRAMESET;
        }
        return R_DONE;
      }
      return IGNORE_END(tb);
    case TK_EOF:
      if (tb->n != 1) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_EOF);
      }
      tb->stopped = true;
      return R_DONE;
    default:
      return R_DONE;
  }
}

static int h_after_frameset(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment(tb, tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi(tb, tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      return unexpected(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
    case TK_START:
      if (tk->id == T_HTML) {
        return h_in_body(tb, tk);
      }
      if (tk->id == T_NOFRAMES) {
        return h_in_head(tb, tk);
      }
      return IGNORE_START(tb);
    case TK_END:
      if (tk->id == T_HTML) {
        tb->mode = M_AFTER_AFTER_FRAMESET;
        return R_DONE;
      }
      return IGNORE_END(tb);
    case TK_EOF:
      tb->stopped = true;
      return R_DONE;
    default:
      return R_DONE;
  }
}

static int h_after_after_body(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment_into(tb, HT_ROOT(tb), tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi_into(tb, HT_ROOT(tb), tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      return h_in_body(tb, tk);
    case TK_START:
      if (tk->id == T_HTML) {
        return h_in_body(tb, tk);
      }
      break;
    case TK_EOF:
      tb->stopped = true;
      return R_DONE;
    default:
      break;
  }
  ht_tree_error(tb, tk->type == TK_START ? GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG
                                      : GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG);
  tb->mode = M_IN_BODY;
  return R_REPROCESS;
}

static int h_after_after_frameset(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment_into(tb, HT_ROOT(tb), tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi_into(tb, HT_ROOT(tb), tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      return h_in_body(tb, tk);
    case TK_START:
      if (tk->id == T_HTML) {
        return h_in_body(tb, tk);
      }
      if (tk->id == T_NOFRAMES) {
        return h_in_head(tb, tk);
      }
      return IGNORE_START(tb);
    case TK_EOF:
      tb->stopped = true;
      return R_DONE;
    default:
      return IGNORE_END(tb);
  }
}

/* ---- the table modes ---------------------------------------------------- */

/* Is the current node a table, tbody, template, tfoot, thead or tr? */
static bool current_is_table_part(const GTEXT_HTML_Tree * tb) {
  switch (HT_STACK_TOP(tb).id) {
    case T_TABLE:
    case T_TBODY:
    case T_TEMPLATE:
    case T_TFOOT:
    case T_THEAD:
    case T_TR:
      return true;
    default:
      return false;
  }
}

/* "Clear the stack back to a table context / a table body context / a table row context": pop
   until the current node is one of the named kinds, a template or the html element. Every pop
   undoes a push, so the cost is amortised over the tokens that opened the elements. */
enum { CTX_TABLE, CTX_BODY, CTX_ROW };

static void clear_to_context(GTEXT_HTML_Tree * tb, int ctx) {
  while (tb->n > 1) {
    const uint32_t id = HT_STACK_TOP(tb).id;
    if (id == T_TEMPLATE || id == T_HTML) {
      break;
    }
    if (ctx == CTX_TABLE && id == T_TABLE) {
      break;
    }
    if (ctx == CTX_BODY && (id == T_TBODY || id == T_TFOOT || id == T_THEAD)) {
      break;
    }
    if (ctx == CTX_ROW && id == T_TR) {
      break;
    }
    ht_stack_pop(tb);
  }
}

static bool in_table_scope(const GTEXT_HTML_Tree * tb, uint32_t id) {
  return ht_stack_in_scope(tb, id, SCOPE_TABLE);
}

/* Errors for characters that are about to be handled as "anything else" (foster parented, or ignored in a
   column group): one for each character, at the place of that character. `c` describes where the bytes of
   `base` came from and is moved forward; `p` is in `base`. A UTF-8 continuation byte is not a character, and
   a NUL is not counted here: "in body" reports it (once). */
static void char_errors(GTEXT_HTML_Tree * tb, SegCursor * c, const char * base, const char * p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const unsigned char b = (unsigned char)p[i];
    if ((b & 0xC0) != 0x80 && b != 0) {
      ht_seg_cursor_advance(c, (size_t)(p - base) + i);
      ht_tree_error_at(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_CHARACTER, c->line, c->column, c->offset);
    }
  }
}

/* The "anything else" entry of "in table" for characters: a parse error each, then "in body" with
   foster parenting enabled. */
static bool foster_chars(GTEXT_HTML_Tree * tb, SegCursor * c, const char * base, const char * p, size_t n) {
  char_errors(tb, c, base, p, n);
  tb->foster = true;
  const bool ok = body_chars(tb, p, n);
  tb->foster = false;
  return ok;
}

static bool pend_push(GTEXT_HTML_Tree * tb, size_t index, size_t line, size_t column, size_t offset) {
  if (tb->pend_nsegs >= GTEXT_HTML_MAX_TEXT_SEGMENTS) {
    return true; /* bounded: the rest are counted on from the last, as the tokenizer's are */
  }
  if (tb->pend_nsegs == tb->pend_segs_cap) {
    const size_t cap = tb->pend_segs_cap ? tb->pend_segs_cap * 2 : 8;
    GTEXT_HTML_TextSegment * grown =
        gtext_allocator_realloc(&tb->alloc, tb->pend_segs, cap * sizeof(*grown));
    if (!grown) {
      return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
    }
    tb->pend_segs = grown;
    tb->pend_segs_cap = cap;
  }
  GTEXT_HTML_TextSegment * seg = &tb->pend_segs[tb->pend_nsegs++];
  seg->index = index;
  seg->line = line;
  seg->column = column;
  seg->offset = offset;
  return true;
}

/* Remember where the chunk [a, a + len) of the token's data came from, for the errors of the characters
   that wait: the chunk's start, and each segment of the token that starts inside it. */
static bool pend_note(GTEXT_HTML_Tree * tb, size_t a, size_t len) {
  SegCursor * c = &tb->tcur;
  ht_seg_cursor_advance(c, a);
  if (!pend_push(tb, tb->pend_len, c->line, c->column, c->offset)) {
    return false;
  }
  for (size_t k = c->next; k < c->nsegs && c->segs[k].index < a + len; k++) {
    if (!pend_push(tb, tb->pend_len + (c->segs[k].index - a), c->segs[k].line, c->segs[k].column,
            c->segs[k].offset)) {
      return false;
    }
  }
  return true;
}

/* "In table text": the characters of a token. NUL is a parse error and dropped. Nothing that is
   not white space can be inserted in a table, so once a character that is not white space is
   seen, what is pending and what follows is foster parented; until then the white space waits.
   The pending characters are never more than the token's run, which the tokenizer's text limit
   bounds; the check below states it. */
static bool table_text_chars(GTEXT_HTML_Tree * tb, const char * p, size_t n) {
  const char * base = tb->cur->data;
  while (n > 0) {
    const char * nul = memchr(p, '\0', n);
    const size_t len = nul ? (size_t)(nul - p) : n;
    if (len > 0) {
      if (!tb->pend_foster && ws_prefix(p, len) != len) {
        if (tb->pend_len > 0) {
          SegCursor pc;
          ht_seg_cursor_init(&pc, tb->pend, tb->pend_segs, tb->pend_nsegs, tb->cur->line,
              tb->cur->column, tb->cur->offset, tb->pend_dl, tb->pend_len);
          if (!foster_chars(tb, &pc, tb->pend, tb->pend, tb->pend_len)) {
            return false;
          }
        }
        tb->pend_len = 0;
        tb->pend_nsegs = 0;
        tb->pend_foster = true;
      }
      if (tb->pend_foster) {
        if (!foster_chars(tb, &tb->tcur, base, p, len)) {
          return false;
        }
      }
      else {
        const size_t limit = tb->opt.max_text_bytes ? tb->opt.max_text_bytes
                                                    : GTEXT_HTML_TOKENIZER_DEFAULT_MAX_TEXT;
        if (len > limit || tb->pend_len > limit - len) {
          return ht_tree_fail(tb, GTEXT_HTML_TREE_E_LIMIT_TEXT);
        }
        if (tb->pend_len + len > tb->pend_cap) {
          size_t cap = tb->pend_cap ? tb->pend_cap : 256;
          while (cap < tb->pend_len + len) {
            cap = cap > SIZE_MAX / 2 ? tb->pend_len + len : cap * 2;
          }
          char * grown = gtext_allocator_realloc(&tb->alloc, tb->pend, cap);
          if (!grown) {
            return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
          }
          tb->pend = grown;
          if (tb->cur->deltas) { /* a source map: the deltas of the data go with the data */
            uint8_t * grown_dl = gtext_allocator_realloc(&tb->alloc, tb->pend_dl, cap);
            if (!grown_dl) {
              return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
            }
            tb->pend_dl = grown_dl;
          }
          tb->pend_cap = cap;
        }
        if (!pend_note(tb, (size_t)(p - base), len)) {
          return false;
        }
        memcpy(tb->pend + tb->pend_len, p, len);
        if (tb->cur->deltas) {
          memcpy(tb->pend_dl + tb->pend_len, tb->cur->deltas + (size_t)(p - base), len);
        }
        tb->pend_len += len;
      }
    }
    if (!nul) {
      break;
    }
    ht_seg_cursor_advance(&tb->tcur, (size_t)(nul - base));
    ht_tree_error_at(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_NULL_CHARACTER, tb->tcur.line,
        tb->tcur.column, tb->tcur.offset);
    p += len + 1;
    n -= len + 1;
  }
  return tb->status == GTEXT_HTML_TREE_OK;
}

static int h_in_table(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment(tb, tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi(tb, tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      return unexpected(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
    case TK_EOF:
      return h_in_body(tb, tk);
    case TK_START:
      switch (tk->id) {
        case T_CAPTION:
          clear_to_context(tb, CTX_TABLE);
          HT_CK(ht_fmt_push_marker(tb));
          HT_CK(ht_insert_element(tb, tk, NULL));
          tb->mode = M_IN_CAPTION;
          return R_DONE;
        case T_COLGROUP:
          clear_to_context(tb, CTX_TABLE);
          HT_CK(ht_insert_element(tb, tk, NULL));
          tb->mode = M_IN_COLUMN_GROUP;
          return R_DONE;
        case T_COL:
          clear_to_context(tb, CTX_TABLE);
          HT_CK(ht_insert_element_bare(tb, T_COLGROUP, NULL));
          tb->mode = M_IN_COLUMN_GROUP;
          return R_REPROCESS;
        case T_TBODY:
        case T_TFOOT:
        case T_THEAD:
          clear_to_context(tb, CTX_TABLE);
          HT_CK(ht_insert_element(tb, tk, NULL));
          tb->mode = M_IN_TABLE_BODY;
          return R_DONE;
        case T_TD:
        case T_TH:
        case T_TR:
          clear_to_context(tb, CTX_TABLE);
          HT_CK(ht_insert_element_bare(tb, T_TBODY, NULL));
          tb->mode = M_IN_TABLE_BODY;
          return R_REPROCESS;
        case T_TABLE:
          ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG);
          if (!in_table_scope(tb, T_TABLE)) {
            return R_DONE;
          }
          ht_stack_pop_until(tb, T_TABLE);
          tb->mode = ht_tree_reset_mode(tb);
          return R_REPROCESS;
        case T_STYLE:
        case T_SCRIPT:
        case T_TEMPLATE:
          return h_in_head(tb, tk);
        case T_INPUT:
          if (!has_attribute(tk, "type") || !attribute_is(tk, "type", "hidden")) {
            break;
          }
          ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG);
          HT_CK(ht_insert_element(tb, tk, NULL));
          HT_CK(ht_pop_current_acknowledge(tb, tk));
          return R_DONE;
        case T_FORM: {
          ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG);
          const bool in_template = parsing_template_contents(tb);
          /* "In table", form: "If the form element pointer is not null, and the parser is not parsing template
             contents, then ignore the token. Otherwise: Insert an HTML element for the token, and, if the parser
             is not parsing template contents, set the form element pointer ...; pop that form element". */
          if (form_pointer_set(tb) && !in_template) {
            return R_DONE;
          }
          HT_CK(ht_insert_element(tb, tk, in_template ? NULL : &tb->form_el));
          ht_stack_pop(tb);
          return R_DONE;
        }
        default:
          break;
      }
      break;
    case TK_END:
      switch (tk->id) {
        case T_TABLE:
          if (!in_table_scope(tb, T_TABLE)) {
            return IGNORE_END(tb);
          }
          if (!is_current(tb, T_TABLE)) {
            /* The chapter names no error here, but the vectors list one (html5lib's
               end-tag-too-early-named): elements the table closes by popping them. */
            ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
          }
          ht_stack_pop_until(tb, T_TABLE);
          tb->mode = ht_tree_reset_mode(tb);
          return R_DONE;
        case T_BODY:
        case T_CAPTION:
        case T_COL:
        case T_COLGROUP:
        case T_HTML:
        case T_TBODY:
        case T_TD:
        case T_TFOOT:
        case T_TH:
        case T_THEAD:
        case T_TR:
          return IGNORE_END(tb);
        case T_TEMPLATE:
          return h_in_head(tb, tk);
        default:
          break;
      }
      break;
    default:
      break;
  }
  /* Anything else: a parse error, and "in body" with foster parenting enabled. */
  ht_tree_error(tb, tk->type == TK_START ? GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG
                                         : GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG);
  tb->foster = true;
  const int r = h_in_body(tb, tk);
  tb->foster = false;
  return r;
}

/* Done with the pending characters (the token that ended them is not a character): white space
   that waited goes into the table, the rest was inserted as it came. */
static int h_in_table_text(GTEXT_HTML_Tree * tb, const Tk * tk) {
  (void)tk;
  if (!tb->pend_foster && tb->pend_len > 0) {
    HT_CK(ht_insert_text(tb, tb->pend, tb->pend_len));
  }
  tb->pend_len = 0;
  tb->pend_nsegs = 0;
  tb->pend_foster = false;
  tb->mode = tb->orig_mode;
  return R_REPROCESS;
}

/* Close a caption (the end tag "caption", or a token that implies it). */
static int close_caption(GTEXT_HTML_Tree * tb) {
  ht_generate_implied_end_tags(tb, NONE_IDX);
  if (!is_current(tb, T_CAPTION)) {
    ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
  }
  ht_stack_pop_until(tb, T_CAPTION);
  ht_fmt_clear_to_marker(tb);
  tb->mode = M_IN_TABLE;
  return tb->status == GTEXT_HTML_TREE_OK ? R_DONE : R_FAIL;
}

static int h_in_caption(GTEXT_HTML_Tree * tb, const Tk * tk) {
  if (tk->type == TK_END) {
    switch (tk->id) {
      case T_CAPTION:
        if (!in_table_scope(tb, T_CAPTION)) {
          return IGNORE_END(tb);
        }
        return close_caption(tb);
      case T_TABLE:
        if (!in_table_scope(tb, T_CAPTION)) {
          return IGNORE_END(tb);
        }
        close_caption(tb);
        return tb->status == GTEXT_HTML_TREE_OK ? R_REPROCESS : R_FAIL;
      case T_BODY:
      case T_COL:
      case T_COLGROUP:
      case T_HTML:
      case T_TBODY:
      case T_TD:
      case T_TFOOT:
      case T_TH:
      case T_THEAD:
      case T_TR:
        return IGNORE_END(tb);
      default:
        break;
    }
  }
  else if (tk->type == TK_START) {
    switch (tk->id) {
      case T_CAPTION:
      case T_COL:
      case T_COLGROUP:
      case T_TBODY:
      case T_TD:
      case T_TFOOT:
      case T_TH:
      case T_THEAD:
      case T_TR:
        if (!in_table_scope(tb, T_CAPTION)) {
          return IGNORE_START(tb);
        }
        close_caption(tb);
        return tb->status == GTEXT_HTML_TREE_OK ? R_REPROCESS : R_FAIL;
      default:
        break;
    }
  }
  return h_in_body(tb, tk);
}

static int h_in_column_group(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment(tb, tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi(tb, tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      return unexpected(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
    case TK_EOF:
      return h_in_body(tb, tk);
    case TK_START:
      if (tk->id == T_HTML) {
        return h_in_body(tb, tk);
      }
      if (tk->id == T_COL) {
        HT_CK(ht_insert_element(tb, tk, NULL));
        HT_CK(ht_pop_current_acknowledge(tb, tk));
        return R_DONE;
      }
      if (tk->id == T_TEMPLATE) {
        return h_in_head(tb, tk);
      }
      break;
    case TK_END:
      if (tk->id == T_COLGROUP) {
        if (!is_current(tb, T_COLGROUP)) {
          return IGNORE_END(tb);
        }
        ht_stack_pop(tb);
        tb->mode = M_IN_TABLE;
        return R_DONE;
      }
      if (tk->id == T_COL) {
        return IGNORE_END(tb);
      }
      if (tk->id == T_TEMPLATE) {
        return h_in_head(tb, tk);
      }
      break;
    default:
      break;
  }
  if (!is_current(tb, T_COLGROUP)) {
    return unexpected(tb, tk->type == TK_START ? GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG
                                               : GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG);
  }
  ht_stack_pop(tb);
  tb->mode = M_IN_TABLE;
  return R_REPROCESS;
}

static int h_in_table_body(GTEXT_HTML_Tree * tb, const Tk * tk) {
  if (tk->type == TK_START) {
    switch (tk->id) {
      case T_TR:
        clear_to_context(tb, CTX_BODY);
        HT_CK(ht_insert_element(tb, tk, NULL));
        tb->mode = M_IN_ROW;
        return R_DONE;
      case T_TH:
      case T_TD:
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG);
        clear_to_context(tb, CTX_BODY);
        HT_CK(ht_insert_element_bare(tb, T_TR, NULL));
        tb->mode = M_IN_ROW;
        return R_REPROCESS;
      case T_CAPTION:
      case T_COL:
      case T_COLGROUP:
      case T_TBODY:
      case T_TFOOT:
      case T_THEAD:
        goto leave_body;
      default:
        break;
    }
  }
  else if (tk->type == TK_END) {
    switch (tk->id) {
      case T_TBODY:
      case T_TFOOT:
      case T_THEAD:
        if (!in_table_scope(tb, tk->id)) {
          return IGNORE_END(tb);
        }
        clear_to_context(tb, CTX_BODY);
        ht_stack_pop(tb);
        tb->mode = M_IN_TABLE;
        return R_DONE;
      case T_TABLE:
        goto leave_body;
      case T_BODY:
      case T_CAPTION:
      case T_COL:
      case T_COLGROUP:
      case T_HTML:
      case T_TD:
      case T_TH:
      case T_TR:
        return IGNORE_END(tb);
      default:
        break;
    }
  }
  return h_in_table(tb, tk);
leave_body:
  if (!in_table_scope(tb, T_TBODY) && !in_table_scope(tb, T_THEAD)
      && !in_table_scope(tb, T_TFOOT)) {
    return unexpected(tb, tk->type == TK_START ? GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG
                                               : GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG);
  }
  clear_to_context(tb, CTX_BODY);
  ht_stack_pop(tb);
  tb->mode = M_IN_TABLE;
  return R_REPROCESS;
}

static int h_in_row(GTEXT_HTML_Tree * tb, const Tk * tk) {
  if (tk->type == TK_START) {
    switch (tk->id) {
      case T_TH:
      case T_TD:
        clear_to_context(tb, CTX_ROW);
        HT_CK(ht_insert_element(tb, tk, NULL));
        tb->mode = M_IN_CELL;
        HT_CK(ht_fmt_push_marker(tb));
        return R_DONE;
      case T_CAPTION:
      case T_COL:
      case T_COLGROUP:
      case T_TBODY:
      case T_TFOOT:
      case T_THEAD:
      case T_TR:
        goto leave_row;
      default:
        break;
    }
  }
  else if (tk->type == TK_END) {
    switch (tk->id) {
      case T_TR:
        if (!in_table_scope(tb, T_TR)) {
          return IGNORE_END(tb);
        }
        clear_to_context(tb, CTX_ROW);
        ht_stack_pop(tb);
        tb->mode = M_IN_TABLE_BODY;
        return R_DONE;
      case T_TABLE:
        goto leave_row;
      case T_TBODY:
      case T_TFOOT:
      case T_THEAD:
        if (!in_table_scope(tb, tk->id) || !in_table_scope(tb, T_TR)) {
          return IGNORE_END(tb);
        }
        goto leave_row;
      case T_BODY:
      case T_CAPTION:
      case T_COL:
      case T_COLGROUP:
      case T_HTML:
      case T_TD:
      case T_TH:
        return IGNORE_END(tb);
      default:
        break;
    }
  }
  return h_in_table(tb, tk);
leave_row:
  if (!in_table_scope(tb, T_TR)) {
    return unexpected(tb, tk->type == TK_START ? GTEXT_HTML_TREE_ERR_UNEXPECTED_START_TAG
                                               : GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG);
  }
  clear_to_context(tb, CTX_ROW);
  ht_stack_pop(tb);
  tb->mode = M_IN_TABLE_BODY;
  return R_REPROCESS;
}

/* "Close the cell". */
static int close_cell(GTEXT_HTML_Tree * tb) {
  ht_generate_implied_end_tags(tb, NONE_IDX);
  if (!is_current(tb, T_TD) && !is_current(tb, T_TH)) {
    ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
  }
  while (tb->n > 0) {
    const uint32_t popped = HT_STACK_TOP(tb).id;
    ht_stack_pop(tb);
    if (popped == T_TD || popped == T_TH) {
      break;
    }
  }
  ht_fmt_clear_to_marker(tb);
  tb->mode = M_IN_ROW;
  return tb->status == GTEXT_HTML_TREE_OK ? R_DONE : R_FAIL;
}

static int h_in_cell(GTEXT_HTML_Tree * tb, const Tk * tk) {
  if (tk->type == TK_END) {
    switch (tk->id) {
      case T_TD:
      case T_TH:
        if (!in_table_scope(tb, tk->id)) {
          return IGNORE_END(tb);
        }
        ht_generate_implied_end_tags(tb, NONE_IDX);
        if (!is_current(tb, tk->id)) {
          ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
        }
        ht_stack_pop_until(tb, tk->id);
        ht_fmt_clear_to_marker(tb);
        tb->mode = M_IN_ROW;
        return R_DONE;
      case T_BODY:
      case T_CAPTION:
      case T_COL:
      case T_COLGROUP:
      case T_HTML:
        return IGNORE_END(tb);
      case T_TABLE:
      case T_TBODY:
      case T_TFOOT:
      case T_THEAD:
      case T_TR:
        if (!in_table_scope(tb, tk->id)) {
          return IGNORE_END(tb);
        }
        close_cell(tb);
        return tb->status == GTEXT_HTML_TREE_OK ? R_REPROCESS : R_FAIL;
      default:
        break;
    }
  }
  else if (tk->type == TK_START) {
    switch (tk->id) {
      case T_CAPTION:
      case T_COL:
      case T_COLGROUP:
      case T_TBODY:
      case T_TD:
      case T_TFOOT:
      case T_TH:
      case T_THEAD:
      case T_TR:
        /* The standard asserts a td or th in table scope; the stack cannot be otherwise here,
           and a stack that were would be ignored, not closed. */
        if (!in_table_scope(tb, T_TD) && !in_table_scope(tb, T_TH)) {
          return IGNORE_START(tb);
        }
        close_cell(tb);
        return tb->status == GTEXT_HTML_TREE_OK ? R_REPROCESS : R_FAIL;
      default:
        break;
    }
  }
  return h_in_body(tb, tk);
}

/* ---- "in template" ------------------------------------------------------ */

/* Replace the current template insertion mode and switch to it. */
static int template_to(GTEXT_HTML_Tree * tb, InsertionMode m) {
  ht_tmpl_pop(tb);
  HT_CK(ht_tmpl_push(tb, m));
  tb->mode = m;
  return R_REPROCESS;
}

static int h_in_template(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
    case TK_PI:
    case TK_DOCTYPE:
      return h_in_body(tb, tk);
    case TK_START:
      switch (tk->id) {
        case T_BASE:
        case T_BASEFONT:
        case T_BGSOUND:
        case T_LINK:
        case T_META:
        case T_NOFRAMES:
        case T_SCRIPT:
        case T_STYLE:
        case T_TEMPLATE:
        case T_TITLE:
          return h_in_head(tb, tk);
        case T_CAPTION:
        case T_COLGROUP:
        case T_TBODY:
        case T_TFOOT:
        case T_THEAD:
          return template_to(tb, M_IN_TABLE);
        case T_COL:
          return template_to(tb, M_IN_COLUMN_GROUP);
        case T_TR:
          return template_to(tb, M_IN_TABLE_BODY);
        case T_TD:
        case T_TH:
          return template_to(tb, M_IN_ROW);
        default:
          return template_to(tb, M_IN_BODY);
      }
    case TK_END:
      if (tk->id == T_TEMPLATE) {
        return h_in_head(tb, tk);
      }
      return IGNORE_END(tb);
    case TK_EOF:
      if (!template_on_stack(tb)) {
        /* No template element is open: stop. (A template insertion mode left over, which the
           algorithms cannot leave, is dropped rather than trusted.) */
        tb->ntmpl = 0;
        tb->stopped = true;
        return R_DONE;
      }
      /* "Pop until a template has been popped, clear the formatting list to the marker, pop the template
         insertion mode, reset the insertion mode, reprocess the token", once for each template that is
         open. Every mode but "in template" hands the end of the file to "in body", which hands it back
         here while a template insertion mode is left, so the reprocessing between two templates only
         ever comes back to this rule: it is done as a loop, and the chain does not grow with the nesting. */
      while (template_on_stack(tb)) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_EOF);
        ht_stack_pop_until(tb, T_TEMPLATE);
        ht_fmt_clear_to_marker(tb);
        ht_tmpl_pop(tb);
        tb->mode = ht_tree_reset_mode(tb);
      }
      return R_REPROCESS;
    default:
      return R_DONE;
  }
}

static const mode_fn k_modes[M__COUNT] = {
    [M_INITIAL] = h_initial,
    [M_BEFORE_HTML] = h_before_html,
    [M_BEFORE_HEAD] = h_before_head,
    [M_IN_HEAD] = h_in_head,
    [M_IN_HEAD_NOSCRIPT] = h_in_head_noscript,
    [M_AFTER_HEAD] = h_after_head,
    [M_IN_BODY] = h_in_body,
    [M_TEXT] = h_text,
    [M_IN_TABLE] = h_in_table,
    [M_IN_TABLE_TEXT] = h_in_table_text,
    [M_IN_CAPTION] = h_in_caption,
    [M_IN_COLUMN_GROUP] = h_in_column_group,
    [M_IN_TABLE_BODY] = h_in_table_body,
    [M_IN_ROW] = h_in_row,
    [M_IN_CELL] = h_in_cell,
    [M_IN_TEMPLATE] = h_in_template,
    [M_AFTER_BODY] = h_after_body,
    [M_IN_FRAMESET] = h_in_frameset,
    [M_AFTER_FRAMESET] = h_after_frameset,
    [M_AFTER_AFTER_BODY] = h_after_after_body,
    [M_AFTER_AFTER_FRAMESET] = h_after_after_frameset,
};

static const char * const k_mode_names[M__COUNT] = {
    [M_INITIAL] = "initial",
    [M_BEFORE_HTML] = "before html",
    [M_BEFORE_HEAD] = "before head",
    [M_IN_HEAD] = "in head",
    [M_IN_HEAD_NOSCRIPT] = "in head noscript",
    [M_AFTER_HEAD] = "after head",
    [M_IN_BODY] = "in body",
    [M_TEXT] = "text",
    [M_IN_TABLE] = "in table",
    [M_IN_TABLE_TEXT] = "in table text",
    [M_IN_CAPTION] = "in caption",
    [M_IN_COLUMN_GROUP] = "in column group",
    [M_IN_TABLE_BODY] = "in table body",
    [M_IN_ROW] = "in row",
    [M_IN_CELL] = "in cell",
    [M_IN_TEMPLATE] = "in template",
    [M_AFTER_BODY] = "after body",
    [M_IN_FRAMESET] = "in frameset",
    [M_AFTER_FRAMESET] = "after frameset",
    [M_AFTER_AFTER_BODY] = "after after body",
    [M_AFTER_AFTER_FRAMESET] = "after after frameset",
};

int ht_html_mode_token(GTEXT_HTML_Tree * tb, const Tk * tk) {
  return k_modes[tb->mode](tb, tk);
}

const char * ht_tree_mode_name(InsertionMode m) {
  return (unsigned)m < M__COUNT ? k_mode_names[m] : "";
}

/* ---- reset the insertion mode appropriately ----------------------------- */

InsertionMode ht_tree_reset_mode(const GTEXT_HTML_Tree * tb) {
  size_t i = tb->n;
  bool last = false;
  while (i > 0) {
    uint32_t id = tb->stack[i - 1].id;
    if (i == 1) {
      last = true;
      /* "If the parser's fragment context element is non-null, then set node to that element (fragment
         case)." The context is described by its name slot. */
      if (tb->frag) {
        id = tb->ctx_id;
      }
    }
    switch (id) {
      case T_TD:
      case T_TH:
        if (!last) {
          return M_IN_CELL;
        }
        break;
      case T_TR:
        return M_IN_ROW;
      case T_TBODY:
      case T_THEAD:
      case T_TFOOT:
        return M_IN_TABLE_BODY;
      case T_CAPTION:
        return M_IN_CAPTION;
      case T_COLGROUP:
        return M_IN_COLUMN_GROUP;
      case T_TABLE:
        return M_IN_TABLE;
      case T_TEMPLATE:
        /* The current template insertion mode. */
        return tb->ntmpl > 0 ? (InsertionMode)tb->tmpl[tb->ntmpl - 1] : M_IN_TEMPLATE;
      case T_HEAD:
        if (!last) {
          return M_IN_HEAD;
        }
        break;
      case T_BODY:
        return M_IN_BODY;
      case T_FRAMESET:
        return M_IN_FRAMESET;
      case T_HTML:
        return tb->head_el == NULL ? M_BEFORE_HEAD : M_AFTER_HEAD;
      default:
        break;
    }
    if (last) {
      return M_IN_BODY;
    }
    i--;
  }
  return M_IN_BODY;
}

const char * gtext_html_tree_test_reset_mode(
    const char * const * names, size_t count, bool head_set) {
  GTEXT_HTML_Tree * tb = gtext_html_tree_new(NULL);
  if (!tb) {
    return NULL;
  }
  HtNode * parent = gtext_markup_document_root(tb->doc);
  bool ok = true;
  for (size_t i = 0; i < count && ok; i++) {
    uint32_t id = 0;
    HtNode * el = NULL;
    /* "template/in table" is a template whose template insertion mode is "in table" (a plain
       "template" is "in template"): the stack of those modes is built beside the stack of elements. */
    const char * slash = strchr(names[i], '/');
    const size_t nl = slash ? (size_t)(slash - names[i]) : strlen(names[i]);
    ok = ht_names_lookup(tb, names[i], nl, true, &id)
        && ht_insert_element_into(tb, parent, id, NULL, 0, &el);
    if (ok && id == T_TEMPLATE) {
      InsertionMode m = M_IN_TEMPLATE;
      bool known = slash == NULL;
      for (unsigned k = 0; slash && k < M__COUNT; k++) {
        if (strcmp(ht_tree_mode_name((InsertionMode)k), slash + 1) == 0) {
          m = (InsertionMode)k;
          known = true;
        }
      }
      /* A suffix that names no insertion mode is a mistake in the test, not "in template". */
      ok = known && ht_tmpl_push(tb, m);
    }
    parent = el;
  }
  const char * result = NULL;
  if (ok) {
    if (head_set) {
      tb->head_el = tb->n > 0 ? tb->stack[tb->n - 1].node : NULL;
    }
    result = ht_tree_mode_name(ht_tree_reset_mode(tb));
  }
  gtext_html_tree_free(tb);
  return result;
}

/* ---- the dispatcher ----------------------------------------------------- */

/* Characters, a run at a time, in whatever mode the tree is in. A mode that takes only
   leading white space consumes that and hands the rest to the next mode (or drops it). */
static bool handle_chars(GTEXT_HTML_Tree * tb, const char * p, size_t n) {
  /* Every pass either consumes characters or changes the mode toward one that does: the longest run of
     passes that consume nothing is a chain of modes ("initial" to "in body" is six), bounded as a token's
     reprocessing is. */
  size_t stalled = 0;
  size_t left = n;
  while (n > 0) {
    if (tb->status != GTEXT_HTML_TREE_OK) {
      return false;
    }
    stalled = n == left ? stalled + 1 : 0;
    left = n;
    if (stalled > tb->reprocess_limit) {
      return ht_tree_fail(tb, GTEXT_HTML_TREE_E_REPROCESS);
    }
    if (ht_use_foreign_rules(tb, NULL, true)) {
      return ht_foreign_chars(tb, p, n);
    }
    size_t k;
    switch (tb->mode) {
      case M_INITIAL:
        k = ws_prefix(p, n);
        p += k;
        n -= k;
        if (n == 0) {
          return true;
        }
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_MISSING_DOCTYPE);
        set_quirks(tb);
        tb->mode = M_BEFORE_HTML;
        break;
      case M_BEFORE_HTML:
        k = ws_prefix(p, n);
        p += k;
        n -= k;
        if (n == 0) {
          return true;
        }
        if (!ht_insert_element_into(tb, HT_ROOT(tb), T_HTML, NULL, 0, NULL)) {
          return false;
        }
        tb->mode = M_BEFORE_HEAD;
        break;
      case M_BEFORE_HEAD:
        k = ws_prefix(p, n);
        p += k;
        n -= k;
        if (n == 0) {
          return true;
        }
        if (!ht_insert_element_bare(tb, T_HEAD, &tb->head_el)) {
          return false;
        }
        tb->mode = M_IN_HEAD;
        break;
      case M_IN_HEAD:
      case M_IN_HEAD_NOSCRIPT:
      case M_AFTER_HEAD:
        k = ws_prefix(p, n);
        if (k > 0 && !ht_insert_text(tb, p, k)) {
          return false;
        }
        p += k;
        n -= k;
        if (n == 0) {
          return true;
        }
        if (tb->mode == M_IN_HEAD) {
          ht_stack_pop(tb);
          tb->mode = M_AFTER_HEAD;
        }
        else if (tb->mode == M_IN_HEAD_NOSCRIPT) {
          ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_CHARACTER);
          ht_stack_pop(tb);
          tb->mode = M_IN_HEAD;
        }
        else {
          if (!ht_insert_element_bare(tb, T_BODY, NULL)) {
            return false;
          }
          tb->frameset_ok = true;
          tb->mode = M_IN_BODY;
        }
        break;
      case M_IN_BODY:
        return body_chars(tb, p, n);
      case M_TEXT:
        return ht_insert_text(tb, p, n);
      case M_AFTER_BODY:
      case M_AFTER_AFTER_BODY:
        k = ws_prefix(p, n);
        if (k > 0 && !body_chars(tb, p, k)) {
          return false;
        }
        p += k;
        n -= k;
        if (n == 0) {
          return true;
        }
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_CHARACTER);
        tb->mode = M_IN_BODY;
        break;
      case M_IN_FRAMESET:
      case M_AFTER_FRAMESET:
      case M_AFTER_AFTER_FRAMESET:
        /* White space is kept (inserted, or in the last mode handled as "in body" does);
           every other character is a parse error and is dropped. */
        while (n > 0) {
          k = ws_prefix(p, n);
          if (k > 0) {
            if (tb->mode == M_AFTER_AFTER_FRAMESET ? !body_chars(tb, p, k)
                                                   : !ht_insert_text(tb, p, k)) {
              return false;
            }
            p += k;
            n -= k;
          }
          size_t bad = 0;
          while (bad < n && !is_ws(p[bad])) {
            /* One error per character: a UTF-8 continuation byte is not one. */
            if (((unsigned char)p[bad] & 0xC0) != 0x80) {
              ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_CHARACTER);
            }
            bad++;
          }
          p += bad;
          n -= bad;
        }
        return tb->status == GTEXT_HTML_TREE_OK;
      case M_IN_TABLE:
      case M_IN_TABLE_BODY:
      case M_IN_ROW:
        if (current_is_table_part(tb)) {
          /* "In table text" collects the characters; a token that is not one ends it. */
          tb->pend_len = 0;
          tb->pend_nsegs = 0;
          tb->pend_foster = false;
          tb->orig_mode = tb->mode;
          tb->mode = M_IN_TABLE_TEXT;
          break;
        }
        return foster_chars(tb, &tb->tcur, tb->cur->data, p, n);
      case M_IN_TABLE_TEXT:
        return table_text_chars(tb, p, n);
      case M_IN_CAPTION:
      case M_IN_CELL:
      case M_IN_TEMPLATE:
        return body_chars(tb, p, n);
      case M_IN_COLUMN_GROUP:
        /* White space is inserted. Anything else leaves the column group when it is the current
           node ("anything else": pop it, switch to "in table", reprocess) and is a parse error
           that is ignored when it is not (a template, say): each such character is dropped, and
           the white space among them still goes in. */
        while (n > 0) {
          k = ws_prefix(p, n);
          if (k > 0 && !ht_insert_text(tb, p, k)) {
            return false;
          }
          p += k;
          n -= k;
          if (n == 0) {
            return true;
          }
          if (is_current(tb, T_COLGROUP)) {
            ht_stack_pop(tb);
            tb->mode = M_IN_TABLE;
            break;
          }
          size_t bad = 0;
          while (bad < n && !is_ws(p[bad])) {
            bad++;
          }
          char_errors(tb, &tb->tcur, tb->cur->data, p, bad);
          p += bad;
          n -= bad;
        }
        if (n == 0) {
          return tb->status == GTEXT_HTML_TREE_OK;
        }
        break;
      default:
        return ht_tree_fail(tb, GTEXT_HTML_TREE_E_INVALID);
    }
  }
  return tb->status == GTEXT_HTML_TREE_OK;
}

bool ht_tree_handle_token(GTEXT_HTML_Tree * tb, const GTEXT_HTML_Token * t) {
  tb->cur = t;
  tb->work++;
  const bool skip_lf = tb->skip_lf;
  tb->skip_lf = false;
  tb->ack_self_closing = false;
  Tk tk;
  memset(&tk, 0, sizeof(tk));
  tk.tok = t;
  switch (t->type) {
    case GTEXT_HTML_TOKEN_CHARACTER: {
      const char * p = t->data;
      size_t n = t->data_length;
      if (skip_lf && n > 0 && p[0] == '\n') {
        p++;
        n--;
      }
      ht_seg_cursor_init(&tb->tcur, t->data, t->segments, t->segment_count, t->line, t->column, t->offset,
          t->deltas, t->data_length);
      return handle_chars(tb, p, n);
    }
    case GTEXT_HTML_TOKEN_DOCTYPE:
      tk.type = TK_DOCTYPE;
      break;
    case GTEXT_HTML_TOKEN_COMMENT:
      tk.type = TK_COMMENT;
      break;
    case GTEXT_HTML_TOKEN_PROCESSING_INSTRUCTION:
      tk.type = TK_PI;
      break;
    case GTEXT_HTML_TOKEN_EOF:
      tk.type = TK_EOF;
      break;
    case GTEXT_HTML_TOKEN_START_TAG:
      tk.type = TK_START;
      tk.attrs = t->attributes;
      tk.nattrs = t->attribute_count;
      tk.self_closing = t->self_closing;
      if (!ht_names_lookup(tb, t->name, t->name_length, true, &tk.id)) {
        return false;
      }
      break;
    case GTEXT_HTML_TOKEN_END_TAG:
      tk.type = TK_END;
      if (!ht_names_lookup(tb, t->name, t->name_length, false, &tk.id)) {
        return false;
      }
      break;
    default:
      return ht_tree_fail(tb, GTEXT_HTML_TREE_E_INVALID);
  }
  for (size_t again = 0; again <= tb->reprocess_limit; again++) {
    /* The tree construction dispatcher: the rules of foreign content, or the current insertion mode's. */
    const int r = ht_use_foreign_rules(tb, &tk, false) ? ht_foreign_token(tb, &tk)
                                                       : k_modes[tb->mode](tb, &tk);
    if (r == R_FAIL || tb->status != GTEXT_HTML_TREE_OK) {
      return false;
    }
    if (r == R_DONE) {
      if (tk.type == TK_START && tk.self_closing && !tb->ack_self_closing) {
        ht_tree_error_raw(tb,
            (int)GTEXT_HTML_ERR_NON_VOID_HTML_ELEMENT_START_TAG_WITH_TRAILING_SOLIDUS);
      }
      return tb->status == GTEXT_HTML_TREE_OK;
    }
  }
  return ht_tree_fail(tb, GTEXT_HTML_TREE_E_REPROCESS);
}
