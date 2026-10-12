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
 * The tree builder's private structures, shared by html_tree.c (the state: the
 * stack of open elements, the list of active formatting elements, insertion,
 * the adoption agency algorithm) and html_tree_modes.c (the insertion modes).
 * Nothing outside src/html includes this, except the unit test that checks the
 * tag table against the vocabulary.
 */

#ifndef GHOTI_IO_GTEXT_SRC_HTML_HTML_TREE_PRIVATE_H
#define GHOTI_IO_GTEXT_SRC_HTML_HTML_TREE_PRIVATE_H

#include <ghoti.io/text/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../markup/markup_internal.h"
#include "html_tree_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GTEXT_Markup_Node HtNode;
typedef GTEXT_Markup_Document HtDoc;

/// "No index" for the 32-bit stack indexes.
#define NONE_IDX UINT32_MAX

/* ---- the tag table ------------------------------------------------------ */

/* Flags of a tag name, for the stack entry's scope and category bookkeeping. */
enum {
  F_SPECIAL = 1,    /* the standard's "special" category */
  F_FMT = 2,        /* the "formatting" category */
  F_SC_GEN = 4,     /* a boundary of "has an element in scope" */
  F_SC_LIST = 8,    /* also a boundary of list item scope (ol, ul) */
  F_SC_BUTTON = 16, /* also a boundary of button scope (button) */
  F_SC_TABLE = 32,  /* a boundary of table scope (html, table, template) */
  F_IMPLIED = 64    /* ended by "generate implied end tags" */
};

/* Every name the algorithms mention, in byte order of the names (the table is
   searched with a binary search, and a table out of order would miss names without a
   word: tests/test-html-tree.cpp, TheTagTableIsInOrderAndAgreesWithTheVocabulary, checks
   the order, and the flags against the HTML vocabulary). */
#define HTML_TAGS(X) \
  X(T_A, "a", F_FMT) \
  X(T_ADDRESS, "address", F_SPECIAL) \
  X(T_APPLET, "applet", F_SPECIAL | F_SC_GEN) \
  X(T_AREA, "area", F_SPECIAL) \
  X(T_ARTICLE, "article", F_SPECIAL) \
  X(T_ASIDE, "aside", F_SPECIAL) \
  X(T_B, "b", F_FMT) \
  X(T_BASE, "base", F_SPECIAL) \
  X(T_BASEFONT, "basefont", F_SPECIAL) \
  X(T_BGSOUND, "bgsound", F_SPECIAL) \
  X(T_BIG, "big", F_FMT) \
  X(T_BLOCKQUOTE, "blockquote", F_SPECIAL) \
  X(T_BODY, "body", F_SPECIAL) \
  X(T_BR, "br", F_SPECIAL) \
  X(T_BUTTON, "button", F_SPECIAL | F_SC_BUTTON) \
  X(T_CAPTION, "caption", F_SPECIAL | F_SC_GEN) \
  X(T_CENTER, "center", F_SPECIAL) \
  X(T_CODE, "code", F_FMT) \
  X(T_COL, "col", F_SPECIAL) \
  X(T_COLGROUP, "colgroup", F_SPECIAL) \
  X(T_DD, "dd", F_SPECIAL | F_IMPLIED) \
  X(T_DETAILS, "details", F_SPECIAL) \
  X(T_DIALOG, "dialog", 0) \
  X(T_DIR, "dir", F_SPECIAL) \
  X(T_DIV, "div", F_SPECIAL) \
  X(T_DL, "dl", F_SPECIAL) \
  X(T_DT, "dt", F_SPECIAL | F_IMPLIED) \
  X(T_EM, "em", F_FMT) \
  X(T_EMBED, "embed", F_SPECIAL) \
  X(T_FIELDSET, "fieldset", F_SPECIAL) \
  X(T_FIGCAPTION, "figcaption", F_SPECIAL) \
  X(T_FIGURE, "figure", F_SPECIAL) \
  X(T_FONT, "font", F_FMT) \
  X(T_FOOTER, "footer", F_SPECIAL) \
  X(T_FORM, "form", F_SPECIAL) \
  X(T_FRAME, "frame", F_SPECIAL) \
  X(T_FRAMESET, "frameset", F_SPECIAL) \
  X(T_H1, "h1", F_SPECIAL) \
  X(T_H2, "h2", F_SPECIAL) \
  X(T_H3, "h3", F_SPECIAL) \
  X(T_H4, "h4", F_SPECIAL) \
  X(T_H5, "h5", F_SPECIAL) \
  X(T_H6, "h6", F_SPECIAL) \
  X(T_HEAD, "head", F_SPECIAL) \
  X(T_HEADER, "header", F_SPECIAL) \
  X(T_HGROUP, "hgroup", F_SPECIAL) \
  X(T_HR, "hr", F_SPECIAL) \
  X(T_HTML, "html", F_SPECIAL | F_SC_GEN | F_SC_TABLE) \
  X(T_I, "i", F_FMT) \
  X(T_IFRAME, "iframe", F_SPECIAL) \
  X(T_IMAGE, "image", 0) \
  X(T_IMG, "img", F_SPECIAL) \
  X(T_INPUT, "input", F_SPECIAL) \
  X(T_KEYGEN, "keygen", F_SPECIAL) \
  X(T_LI, "li", F_SPECIAL | F_IMPLIED) \
  X(T_LINK, "link", F_SPECIAL) \
  X(T_LISTING, "listing", F_SPECIAL) \
  X(T_MAIN, "main", F_SPECIAL) \
  X(T_MARQUEE, "marquee", F_SPECIAL | F_SC_GEN) \
  X(T_MATH, "math", 0) \
  X(T_MENU, "menu", F_SPECIAL) \
  X(T_META, "meta", F_SPECIAL) \
  X(T_NAV, "nav", F_SPECIAL) \
  X(T_NOBR, "nobr", F_FMT) \
  X(T_NOEMBED, "noembed", F_SPECIAL) \
  X(T_NOFRAMES, "noframes", F_SPECIAL) \
  X(T_NOSCRIPT, "noscript", F_SPECIAL) \
  X(T_OBJECT, "object", F_SPECIAL | F_SC_GEN) \
  X(T_OL, "ol", F_SPECIAL | F_SC_LIST) \
  X(T_OPTGROUP, "optgroup", F_IMPLIED) \
  X(T_OPTION, "option", F_IMPLIED) \
  X(T_P, "p", F_SPECIAL | F_IMPLIED) \
  X(T_PARAM, "param", F_SPECIAL) \
  X(T_PLAINTEXT, "plaintext", F_SPECIAL) \
  X(T_PRE, "pre", F_SPECIAL) \
  X(T_RB, "rb", F_IMPLIED) \
  X(T_RP, "rp", F_IMPLIED) \
  X(T_RT, "rt", F_IMPLIED) \
  X(T_RTC, "rtc", F_IMPLIED) \
  X(T_RUBY, "ruby", 0) \
  X(T_S, "s", F_FMT) \
  X(T_SCRIPT, "script", F_SPECIAL) \
  X(T_SEARCH, "search", F_SPECIAL) \
  X(T_SECTION, "section", F_SPECIAL) \
  X(T_SELECT, "select", F_SPECIAL | F_SC_GEN) \
  X(T_SMALL, "small", F_FMT) \
  X(T_SOURCE, "source", F_SPECIAL) \
  X(T_STRIKE, "strike", F_FMT) \
  X(T_STRONG, "strong", F_FMT) \
  X(T_STYLE, "style", F_SPECIAL) \
  X(T_SUMMARY, "summary", F_SPECIAL) \
  X(T_SVG, "svg", 0) \
  X(T_TABLE, "table", F_SPECIAL | F_SC_GEN | F_SC_TABLE) \
  X(T_TBODY, "tbody", F_SPECIAL) \
  X(T_TD, "td", F_SPECIAL | F_SC_GEN) \
  X(T_TEMPLATE, "template", F_SPECIAL | F_SC_GEN | F_SC_TABLE) \
  X(T_TEXTAREA, "textarea", F_SPECIAL) \
  X(T_TFOOT, "tfoot", F_SPECIAL) \
  X(T_TH, "th", F_SPECIAL | F_SC_GEN) \
  X(T_THEAD, "thead", F_SPECIAL) \
  X(T_TITLE, "title", F_SPECIAL) \
  X(T_TR, "tr", F_SPECIAL) \
  X(T_TRACK, "track", F_SPECIAL) \
  X(T_TT, "tt", F_FMT) \
  X(T_U, "u", F_FMT) \
  X(T_UL, "ul", F_SPECIAL | F_SC_LIST) \
  X(T_WBR, "wbr", F_SPECIAL) \
  X(T_XMP, "xmp", F_SPECIAL)

enum {
#define X(id, name, flags) id,
  HTML_TAGS(X)
#undef X
  T__COUNT
};

/// The table, for the unit test that checks it against the vocabulary.
typedef struct {
  const char * name;
  uint32_t flags;
} HtmlTagInfo;
extern const HtmlTagInfo gtext_html_tag_table[];

/* ---- insertion modes ---------------------------------------------------- */

typedef enum {
  M_INITIAL = 0,
  M_BEFORE_HTML,
  M_BEFORE_HEAD,
  M_IN_HEAD,
  M_IN_HEAD_NOSCRIPT,
  M_AFTER_HEAD,
  M_IN_BODY,
  M_TEXT,
  M_IN_TABLE,        /* 3d: not parsed yet */
  M_IN_TABLE_TEXT,   /* 3d */
  M_IN_CAPTION,      /* 3d */
  M_IN_COLUMN_GROUP, /* 3d */
  M_IN_TABLE_BODY,   /* 3d */
  M_IN_ROW,          /* 3d */
  M_IN_CELL,         /* 3d */
  M_IN_TEMPLATE,     /* 3d */
  M_AFTER_BODY,
  M_IN_FRAMESET,
  M_AFTER_FRAMESET,
  M_AFTER_AFTER_BODY,
  M_AFTER_AFTER_FRAMESET,
  M__COUNT
} InsertionMode;

/* ---- the list of active formatting elements ----------------------------- */

enum { FE_FREE = 0, FE_ELEMENT, FE_MARKER, FE_BOOKMARK };

typedef struct fmt_entry {
  struct fmt_entry * prev; /* list order, oldest first */
  struct fmt_entry * next;
  struct fmt_entry * same_prev; /* the previous element entry with the same name */
  struct fmt_entry * same_next;
  struct fmt_entry * prev_marker; /* a marker: the marker before it */
  HtNode * el;               /* NULL for a marker or the bookmark */
  uint64_t attr_hash;      /* order-independent hash of the attributes as created */
  uint64_t seq;            /* increasing; an entry is after the last marker iff seq > marker's */
  uint64_t serial;         /* distinguishes a live entry from a recycled one */
  size_t stack_idx;        /* index in the stack, or SIZE_MAX */
  uint32_t id;
  uint32_t nattrs;
  uint8_t kind;
} fmt_entry;

/* ---- the stack of open elements ----------------------------------------- */

typedef struct {
  HtNode * node;
  fmt_entry * fmt;       /* its entry in the list of active formatting elements, or NULL */
  uint32_t id;
  uint32_t prev_same;    /* index of the next entry down with the same name, or NONE_IDX */
  /* The index of the nearest boundary at or below this entry, for each kind of
     scope: "has X in scope" is "X's topmost index >= the top entry's floor". */
  uint32_t fl_gen;
  uint32_t fl_list;
  uint32_t fl_button;
  uint32_t fl_table;
  uint32_t fl_special;
  uint32_t fl_li;        /* topmost special element that is not address, div or p */
  uint16_t flags;        /* F_* of the name */
  uint8_t removed;       /* adoption agency scratch */
} stack_entry;

/* ---- names -------------------------------------------------------------- */

typedef struct {
  markup_atom * atom;   /* interned lazily; the builder holds a reference */
  fmt_entry * fmt_last; /* the last formatting-list entry with this name */
  const char * text;
  uint32_t topidx;      /* topmost stack index with this name, or NONE_IDX */
  uint32_t len;
  uint32_t flags;
  uint8_t dynamic;      /* text is owned */
} name_slot;

/* ---- a token as the modes see it ---------------------------------------- */

typedef enum {
  TK_DOCTYPE = 1,
  TK_START,
  TK_END,
  TK_COMMENT,
  TK_PI,
  TK_EOF
} TkType;

typedef struct {
  TkType type;
  const GTEXT_HTML_Token * tok;
  uint32_t id;                         /* start and end tags */
  const GTEXT_HTML_Attribute * attrs;  /* start tags */
  size_t nattrs;
  bool self_closing;
} Tk;

/* A hash index of the attribute names of an element that start tags keep adding to
   (the html element and the body element), so that merging is O(1) per attribute. */
typedef struct {
  HtNode * el;        /* whose attributes these are; rebuilt if it is another element */
  uint32_t * slots; /* attribute index + 1, 0 empty */
  size_t cap;       /* a power of two, or 0 */
  size_t count;
} attr_index;

/* ---- the builder -------------------------------------------------------- */

struct GTEXT_HTML_Tree {
  GTEXT_Allocator alloc;
  GTEXT_HTML_TreeOptions opt;
  size_t max_open;
  size_t max_errors;
  GTEXT_HTML_Tokenizer * tok;
  HtDoc * doc;
  bool doc_taken;
  bool finished;
  bool stopped;
  attr_index merge_index[2];
  bool tok_done; /* the tokenizer delivered its end-of-file token */
  GTEXT_HTML_TreeStatus status;
  GTEXT_HTML_NotYet not_yet;

  InsertionMode mode;
  InsertionMode orig_mode;
  bool frameset_ok;
  bool skip_lf;
  bool ack_self_closing;
  HtNode * head_el;
  HtNode * form_el;

  stack_entry * stack;
  size_t n;      /* entries */
  size_t cap;

  name_slot * names;
  size_t nnames;
  size_t cap_names;
  uint32_t * dyn_tab; /* open addressing: id + 1, 0 empty */
  size_t dyn_cap;
  size_t dyn_count;
  uint64_t seed;
  markup_atom * ns_html;

  fmt_entry * fmt_head;
  fmt_entry * fmt_tail;
  fmt_entry * last_marker;
  fmt_entry * fmt_free;
  fmt_entry bookmark;
  uint64_t fmt_seq;
  uint64_t fmt_serial;
  void ** fmt_blocks; /* blocks of entries, to free */
  size_t fmt_nblocks;
  size_t fmt_block_cap;

  /* the text node being extended and the capacity of its buffer */
  HtNode * text_node;
  size_t text_cap;

  /* the token being handled, for error positions and locations */
  const GTEXT_HTML_Token * cur;

  GTEXT_HTML_TreeError * errors;
  size_t nerrors;
  size_t cap_errors;
  size_t dropped;
  size_t tok_errors_seen; /* tokenizer errors already copied */

  size_t work;
};

/* ---- shared functions (html_tree.c) ------------------------------------- */

typedef enum { R_DONE = 0, R_REPROCESS = 1, R_FAIL = -1 } HandlerResult;
#define HT_CK(x) \
  do { \
    if (!(x)) { \
      return R_FAIL; \
    } \
  } while (0)

bool ht_tree_fail(GTEXT_HTML_Tree * tb, GTEXT_HTML_TreeStatus status);
bool ht_tree_not_yet(GTEXT_HTML_Tree * tb, GTEXT_HTML_NotYet what);
bool ht_tree_markup_status(GTEXT_HTML_Tree * tb, GTEXT_Markup_Status status);
void ht_tree_error(GTEXT_HTML_Tree * tb, GTEXT_HTML_TreeErrorCode code);
void ht_tree_error_raw(GTEXT_HTML_Tree * tb, int code);

/* names */
bool ht_names_lookup(GTEXT_HTML_Tree * tb, const char * bytes, size_t len,
    bool insert, uint32_t * id);

/* the stack */
#define HT_STACK_TOP(tb) ((tb)->stack[(tb)->n - 1])
bool ht_stack_push(GTEXT_HTML_Tree * tb, HtNode * node, uint32_t id, fmt_entry * fmt);
void ht_stack_pop(GTEXT_HTML_Tree * tb);
void ht_stack_remove_at(GTEXT_HTML_Tree * tb, size_t idx);
bool ht_stack_in_scope(const GTEXT_HTML_Tree * tb, uint32_t id, int kind);
void ht_stack_pop_until(GTEXT_HTML_Tree * tb, uint32_t id);
enum { SCOPE_GEN, SCOPE_LIST, SCOPE_BUTTON, SCOPE_TABLE };
bool ht_stack_has_heading_in_scope(const GTEXT_HTML_Tree * tb);
void ht_stack_pop_until_heading(GTEXT_HTML_Tree * tb);
void ht_generate_implied_end_tags(GTEXT_HTML_Tree * tb, uint32_t except);
bool ht_close_p_element(GTEXT_HTML_Tree * tb);
bool ht_close_p_if_in_button_scope(GTEXT_HTML_Tree * tb);
bool ht_any_other_end_tag(GTEXT_HTML_Tree * tb, uint32_t id);
bool ht_is_heading_id(uint32_t id);

/* insertion */
bool ht_insert_element(GTEXT_HTML_Tree * tb, const Tk * tk, HtNode ** out);
bool ht_insert_element_bare(GTEXT_HTML_Tree * tb, uint32_t id, HtNode ** out);
bool ht_insert_element_into(GTEXT_HTML_Tree * tb, HtNode * parent, uint32_t id,
    const GTEXT_HTML_Attribute * attrs, size_t nattrs, HtNode ** out);
bool ht_insert_text(GTEXT_HTML_Tree * tb, const char * data, size_t length);
bool ht_insert_comment_into(GTEXT_HTML_Tree * tb, HtNode * parent, const GTEXT_HTML_Token * t);
bool ht_insert_pi_into(GTEXT_HTML_Tree * tb, HtNode * parent, const GTEXT_HTML_Token * t);
bool ht_merge_attributes(GTEXT_HTML_Tree * tb, HtNode * element, const GTEXT_HTML_Attribute * attrs, size_t n);
bool ht_insert_doctype(GTEXT_HTML_Tree * tb, const GTEXT_HTML_Token * t);
void ht_set_mode_from_doctype(GTEXT_HTML_Tree * tb, const GTEXT_HTML_Token * t);
bool ht_pop_current_acknowledge(GTEXT_HTML_Tree * tb, const Tk * tk);

/* the list of active formatting elements */
bool ht_fmt_push_marker(GTEXT_HTML_Tree * tb);
void ht_fmt_clear_to_marker(GTEXT_HTML_Tree * tb);
bool ht_fmt_push_element(GTEXT_HTML_Tree * tb, const Tk * tk);
bool ht_fmt_reconstruct(GTEXT_HTML_Tree * tb);
bool ht_fmt_after_marker(const GTEXT_HTML_Tree * tb, const fmt_entry * e);
void ht_fmt_remove(GTEXT_HTML_Tree * tb, fmt_entry * e);
bool ht_adoption_agency(GTEXT_HTML_Tree * tb, uint32_t subject);

/* modes (html_tree_modes.c) */
bool ht_tree_handle_token(GTEXT_HTML_Tree * tb, const GTEXT_HTML_Token * t);
InsertionMode ht_tree_reset_mode(const GTEXT_HTML_Tree * tb, bool * unsupported);
const char * ht_tree_mode_name(InsertionMode m);
int ht_tree_quirks_mode(bool has_name, const char * name, size_t name_len,
    bool has_pub, const char * pub, size_t pub_len, bool has_sys,
    const char * sys, size_t sys_len, bool force_quirks);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_HTML_HTML_TREE_PRIVATE_H
