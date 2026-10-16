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
 * The markup tree's internal structures.
 *
 * ## Why a node is shaped the way it is
 *
 * Every node carries `parent`, `first_child`, `last_child`, `prev` and `next`,
 * and the last two do double duty. A node in the tree uses them as sibling
 * links. A node out of the tree has no siblings, so it uses them as the links
 * of the document's list of detached nodes - which is what lets a document
 * free a node nobody ever inserted, without costing every node two more
 * pointers. The public accessors say NULL for a sibling when there is no
 * parent, so the sharing is invisible from outside; everything in src/markup
 * that reads `prev` or `next` must know which meaning it has, and the rule is
 * `parent != NULL`.
 *
 * A template's content fragment is the one other exception to "no parent means
 * detached": its raw `parent` is the element that owns it, so that a walk can
 * climb back out, and the flag MARKUP_F_CONTENT says it is not that element's
 * child. The walk the library uses for freeing, cloning and comparing treats an
 * element's content as a child before its real children (the "extended"
 * order); the public walk does not.
 *
 * Names are interned per document and reference counted, so a namespace URI
 * that appears on ten thousand elements is stored once and two names are equal
 * if their atoms are the same pointer.
 */

#ifndef GHOTI_IO_GTEXT_SRC_MARKUP_MARKUP_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_MARKUP_MARKUP_INTERNAL_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One interned string: a namespace URI, a prefix or a name. */
typedef struct markup_atom {
  struct markup_atom * next; ///< Next in the bucket's chain.
  size_t hash;               ///< The full hash, kept to make resizing cheap.
  size_t refs;               ///< References from nodes and attributes.
  size_t scratch;            ///< Used only by markup_check_invariants().
  size_t length;             ///< Bytes of `text`, not counting the NUL.
#ifdef __cplusplus
  char text[1];              ///< The bytes, NUL-terminated. (A flexible array
                             ///< member in C; C++ has none, and a test that
                             ///< includes this header never allocates one.)
#else
  char text[];               ///< The bytes, NUL-terminated.
#endif
} markup_atom;

/** Node flags. */
enum {
  MARKUP_F_ROOT = 1,    ///< The document's own root node.
  MARKUP_F_CONTENT = 2, ///< A template element's content fragment.
  MARKUP_F_LOCATION = 4 ///< `offset`, `line` and `column` are meaningful.
};

/** One attribute of an element. */
typedef struct {
  markup_atom * ns;     ///< NULL for none.
  markup_atom * prefix; ///< NULL for none.
  markup_atom * local;  ///< Never NULL.
  char * value;         ///< NUL-terminated copy.
  size_t value_length;
  size_t offset;        ///< Location, if `has_location`.
  uint32_t line;
  uint32_t column;
  uint32_t source_id;
  uint32_t reference_source_id;
  size_t raw_offset;
  uint8_t has_raw_offset;
  uint8_t has_location;
  uint8_t origin;
} markup_attr;

/** The part of every node. */
struct GTEXT_Markup_Node {
  uint8_t kind;  ///< A GTEXT_Markup_Kind.
  uint8_t flags; ///< MARKUP_F_*.
  /// Text, CDATA and comment nodes only: log2 of the size of the data buffer once markup_text_append() has
  /// grown it (0: the buffer is exactly the data and its NUL). It lives in the padding after `flags`.
  uint8_t cap_log2;
  uint32_t line;
  uint32_t column;
  size_t offset;
  uint32_t source_id;
  uint32_t reference_source_id;
  size_t raw_offset;
  uint8_t has_raw_offset;
  struct GTEXT_Markup_Document * doc;
  struct GTEXT_Markup_Node * parent;
  struct GTEXT_Markup_Node * first_child;
  struct GTEXT_Markup_Node * last_child;
  struct GTEXT_Markup_Node * prev; ///< Sibling, or detached-list neighbour.
  struct GTEXT_Markup_Node * next;
};

typedef struct {
  struct GTEXT_Markup_Node base;
  markup_atom * ns;
  markup_atom * prefix;
  markup_atom * local;
  markup_attr * attrs;
  size_t attr_count;
  size_t attr_capacity;
  struct GTEXT_Markup_Node * content; ///< Template content, or NULL.
} markup_element;

/** Text, CDATA and comment nodes. */
typedef struct {
  struct GTEXT_Markup_Node base;
  char * data;
  size_t length;
  GTEXT_Markup_Text_Span * spans;
  size_t span_count;
  size_t span_capacity;
} markup_text;

typedef struct {
  struct GTEXT_Markup_Node base;
  markup_atom * target;
  char * data;
  size_t length;
} markup_pi;

typedef struct {
  int kind;
  char * source;
  size_t length;
  GTEXT_Markup_Location location;
  char * parameter_entity;
  uint64_t sequence;
  uint8_t external_context;
} markup_dtd_syntax;

typedef struct {
  int kind;
  char * element_name;
  size_t element_name_length;
  char * name;
  size_t name_length;
  const char * source; ///< Borrowed from the retained syntax record.
  size_t source_length;
  GTEXT_Markup_Location location;
  uint8_t external_context;
} markup_dtd_binding;

typedef struct {
  GTEXT_Markup_Source view;
  char * owned_name;
  const struct GTEXT_Markup_Document * origin_doc;
  uint32_t origin_id;
} markup_source;

typedef struct {
  struct GTEXT_Markup_Node base;
  char * name;
  char * public_id; ///< NULL when absent.
  char * system_id; ///< NULL when absent.
  markup_dtd_syntax * syntax;
  size_t syntax_count;
  size_t syntax_capacity;
  markup_dtd_syntax * external_syntax;
  size_t external_syntax_count;
  size_t external_syntax_capacity;
  markup_dtd_syntax * generated_syntax;
  size_t generated_syntax_count;
  size_t generated_syntax_capacity;
  markup_dtd_binding * bindings;
  size_t binding_count;
  size_t binding_capacity;
  uint64_t next_syntax_sequence;
} markup_doctype;

struct GTEXT_Markup_Document {
  GTEXT_Allocator allocator;
  GTEXT_Markup_Options options;
  markup_atom ** buckets;
  size_t bucket_count;
  size_t atom_count;
  size_t seed;
  size_t node_count;
  struct GTEXT_Markup_Node * root;
  struct GTEXT_Markup_Node * detached; ///< Head of the detached-node list.
  GTEXT_Markup_Mode mode;
  char * encoding;
  GTEXT_Markup_Encoding_Source encoding_source;
  char * xml_version;
  GTEXT_Markup_Standalone standalone;
  markup_source * sources;
  size_t source_count;
  size_t source_capacity;
};

/* ---- memory ------------------------------------------------------------- */

void * markup_alloc(struct GTEXT_Markup_Document * doc, size_t size);
void * markup_realloc(
    struct GTEXT_Markup_Document * doc, void * ptr, size_t size);
void markup_free(struct GTEXT_Markup_Document * doc, void * ptr);
/** Copy `len` bytes and a NUL. NULL on allocation failure. */
char * markup_copy(
    struct GTEXT_Markup_Document * doc, const char * bytes, size_t len);
GTEXT_Markup_Status markup_doctype_append_syntax(
    struct GTEXT_Markup_Node * node, int kind, const char * source,
    size_t length, const GTEXT_Markup_Location * location);
GTEXT_Markup_Status markup_doctype_append_external_syntax(
    struct GTEXT_Markup_Node * node, int kind, const char * source,
    size_t length, const GTEXT_Markup_Location * location);
GTEXT_Markup_Status markup_doctype_append_generated_syntax(
    struct GTEXT_Markup_Node * node, int kind, const char * source,
    size_t length, const GTEXT_Markup_Location * location,
    const char * parameter_entity, int external_context);
GTEXT_Markup_Status markup_doctype_set_binding(
    struct GTEXT_Markup_Node * node, int kind, const char * element_name,
    size_t element_name_length, const char * name, size_t name_length,
    const char * source, size_t source_length,
    const GTEXT_Markup_Location * location, int external_context);
void markup_doctype_clear_bindings(struct GTEXT_Markup_Node * node);
GTEXT_Markup_Status markup_source_add(GTEXT_Markup_Document * doc,
    uint32_t id, uint32_t parent_id, GTEXT_Markup_Source_Kind kind,
    const char * name);
GTEXT_Markup_Status markup_text_add_span(GTEXT_Markup_Document * doc,
    struct GTEXT_Markup_Node * node, size_t start, size_t length,
    const GTEXT_Markup_Location * location);

/* ---- atoms -------------------------------------------------------------- */

/** Intern `len` bytes, taking a reference. NULL on allocation failure. */
markup_atom * markup_atom_intern(
    struct GTEXT_Markup_Document * doc, const char * bytes, size_t len);
/** The atom for these bytes if there is one; takes no reference. */
markup_atom * markup_atom_find(
    const struct GTEXT_Markup_Document * doc, const char * bytes, size_t len);
void markup_atom_release(
    struct GTEXT_Markup_Document * doc, markup_atom * atom);

/* ---- strings ------------------------------------------------------------ */

/**
 * Check a name or data string: GTEXT_MARKUP_OK, or E_ENCODING for bytes that
 * are not UTF-8. NULL with length 0 is fine.
 */
GTEXT_Markup_Status markup_check_utf8(const char * bytes, size_t len);

/* ---- XML name productions ----------------------------------------------- */

/** XML 1.0 (Fifth Edition) production [4], NameStartChar. */
bool markup_xml_name_start_char(uint32_t c);
/** Production [4a], NameChar. */
bool markup_xml_name_char(uint32_t c);

/* ---- walking ------------------------------------------------------------ */

/** The first of a node's extended children: its template content, else its
 *  first child. */
struct GTEXT_Markup_Node * markup_ext_first(const struct GTEXT_Markup_Node * n);
/** The extended sibling after `n`: after a template's content, the element's
 *  first child; otherwise `next`. */
struct GTEXT_Markup_Node * markup_ext_next(const struct GTEXT_Markup_Node * n);

/* ---- operations the other files in this directory share ----------------- */

/** Free a subtree of any depth without recursion. Its top must be on no list. */
void markup_free_subtree(
    struct GTEXT_Markup_Document * doc, struct GTEXT_Markup_Node * top);
/** Link a node that is on the detached list as `parent`'s last child, with no
 *  rule checked. For code that is building a copy of a tree that was valid. */
void markup_append_unchecked(
    struct GTEXT_Markup_Node * parent, struct GTEXT_Markup_Node * child);
/** Make a detached fragment `element`'s template content, with no rule checked. */
void markup_set_content_unchecked(
    struct GTEXT_Markup_Node * element, struct GTEXT_Markup_Node * fragment);
/** Take a node off the detached list and free it and everything under it. */
void markup_discard_detached(
    struct GTEXT_Markup_Document * doc, struct GTEXT_Markup_Node * node);

/* ---- for the HTML tree builder ------------------------------------------ */

/** A detached element named by atoms already interned in `doc`; takes a
 *  reference on each (`ns` may be NULL). No name or UTF-8 check. */
GTEXT_Markup_Status markup_element_create_atoms(struct GTEXT_Markup_Document * doc,
    markup_atom * ns, markup_atom * local, struct GTEXT_Markup_Node ** out);
/** Add an attribute with no check for a duplicate and no limit (the tokenizer
 *  has removed duplicates and counted). `ns` and `prefix` may be NULL. */
GTEXT_Markup_Status markup_element_append_attribute(
    struct GTEXT_Markup_Document * doc, struct GTEXT_Markup_Node * element,
    markup_atom * ns, markup_atom * prefix, const char * local,
    size_t local_length, const char * value, size_t value_length);
/** A detached copy of an element: name, prefix, attributes and location, no
 *  children. */
GTEXT_Markup_Status markup_element_clone_shallow(
    struct GTEXT_Markup_Document * doc, const struct GTEXT_Markup_Node * src,
    struct GTEXT_Markup_Node ** out);
/** Put `child` in `parent` before `reference` (NULL: last), from wherever it is,
 *  with no rule checked and no limit counted. */
void markup_insert_unchecked(struct GTEXT_Markup_Node * parent,
    struct GTEXT_Markup_Node * child, struct GTEXT_Markup_Node * reference);
/** Take a node out of its parent, onto the detached list. A no-op for a node
 *  that has no parent. */
void markup_detach_unchecked(struct GTEXT_Markup_Node * node);
/** Move every child of `from` to the end of `to`'s children. O(children). */
void markup_move_children_unchecked(
    struct GTEXT_Markup_Node * from, struct GTEXT_Markup_Node * to);
/** Append bytes to a text node's data. The buffer grows by doubling to a power of two, and the node
 *  remembers its size (`cap_log2`), so a caller that appends to a node now and then, among others, pays
 *  for a copy only when it grows. `*capacity` receives the size of the buffer afterwards; it is not read. */
GTEXT_Markup_Status markup_text_append(struct GTEXT_Markup_Document * doc,
    struct GTEXT_Markup_Node * node, size_t * capacity, const char * bytes,
    size_t length);

/* ---- a growable buffer -------------------------------------------------- */

typedef struct {
  const GTEXT_Allocator * allocator;
  char * data;
  size_t length;
  size_t capacity;
} markup_buf;

void markup_buf_init(markup_buf * buf, const GTEXT_Allocator * allocator);
/** Append bytes. Keeps `data` NUL-terminated. E_OOM on failure, buf intact. */
GTEXT_Markup_Status markup_buf_append(
    markup_buf * buf, const char * bytes, size_t len);
/** The sink that appends to a markup_buf. */
GTEXT_Markup_Status markup_buf_sink_write(
    void * user, const char * bytes, size_t len);
void markup_buf_free(markup_buf * buf);

/* ---- checks for tests --------------------------------------------------- */

/**
 * Verify every invariant the tree promises.
 *
 * Parent, child and sibling links agree; the detached list is well formed; the
 * node count is the number of nodes there are; every interned string's
 * reference count is the number of references there are; no cycle; the
 * document's rules hold at every container; attribute names are unique.
 *
 * @return NULL if all hold, otherwise a static description of the first that
 *   does not.
 */
GTEXT_INTERNAL_API const char * markup_check_invariants(
    const struct GTEXT_Markup_Document * doc);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_MARKUP_MARKUP_INTERNAL_H
