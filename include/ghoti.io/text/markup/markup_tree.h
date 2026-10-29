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
 * @file markup_tree.h
 * @brief Building, reading and changing a markup document.
 *
 * ## Ownership
 *
 * A document owns every node made in it, **including a node that is not in the
 * tree**. gtext_markup_create_element() hands back a node with no parent; it is
 * the document's until it is inserted, and if the caller never inserts it,
 * gtext_markup_document_free() still releases it. The point is the error path:
 * a function that creates three nodes and fails on the fourth leaks nothing.
 * gtext_markup_node_free() releases a detached node early, and only a detached
 * one.
 *
 * A node never belongs to two documents. Inserting a node from another
 * document fails with GTEXT_MARKUP_E_WRONG_DOCUMENT; gtext_markup_clone() makes
 * a copy in the target document, which is the way across.
 *
 * ## Names
 *
 * A name is a namespace URI, a prefix and a local name. All three are
 * NUL-terminated UTF-8; the namespace and the prefix may be NULL, the local
 * name may not be empty. **An empty namespace or prefix is the same as NULL**:
 * XML's `xmlns=""` undeclares a namespace and the DOM reads that as null, and a
 * tree that kept both would hold two "no namespace"s that compare unequal. **The tree does not check that a name is a valid XML
 * name**, because the HTML parser builds elements called `a$b` and `x:y` that
 * XML would refuse; gtext_markup_is_xml_name() and its siblings in
 * markup_escape.h are what an XML writer asks. Two names are the same name if
 * their namespace and local name are equal; the prefix is spelling.
 *
 * Every string returned from this API is valid until the node it came from is
 * freed or the field is changed, whichever comes first.
 *
 * ## Complexity
 *
 * Insertion of a node with no children is O(depth of the parent), and O(size
 * of the subtree) otherwise when max_depth is set (see
 * GTEXT_Markup_Options::max_depth). Removal is O(1). Every walk this library
 * makes, freeing and cloning included, is iterative, so no tree is too deep to
 * release.
 *
 * ## Threads
 *
 * A document and everything in it is used from one thread at a time. Reading
 * does not change anything and may be shared if nobody writes.
 */

#ifndef GHOTI_IO_GTEXT_MARKUP_MARKUP_TREE_H
#define GHOTI_IO_GTEXT_MARKUP_MARKUP_TREE_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A markup document: the owner of its nodes, and the holder of what is
 *   true of the document as a whole.
 */
typedef struct GTEXT_Markup_Document GTEXT_Markup_Document;

/**
 * @brief A node of a document. Opaque.
 *
 * Document, fragment, doctype, element, text, CDATA section, comment and
 * processing instruction are all this type; gtext_markup_node_kind() says which.
 * An attribute is not a node.
 */
typedef struct GTEXT_Markup_Node GTEXT_Markup_Node;

/** The namespace of XML itself, bound to the prefix `xml`. */
#define GTEXT_MARKUP_NS_XML "http://www.w3.org/XML/1998/namespace"
/** The namespace of namespace declarations: `xmlns` and `xmlns:*` attributes. */
#define GTEXT_MARKUP_NS_XMLNS "http://www.w3.org/2000/xmlns/"
/** The HTML and XHTML namespace. */
#define GTEXT_MARKUP_NS_HTML "http://www.w3.org/1999/xhtml"
/** The SVG namespace. */
#define GTEXT_MARKUP_NS_SVG "http://www.w3.org/2000/svg"
/** The MathML namespace. */
#define GTEXT_MARKUP_NS_MATHML "http://www.w3.org/1998/Math/MathML"
/** The XLink namespace, which SVG's `href` has used. */
#define GTEXT_MARKUP_NS_XLINK "http://www.w3.org/1999/xlink"

typedef enum {
  GTEXT_MARKUP_ATTRIBUTE_EXPLICIT = 0,
  GTEXT_MARKUP_ATTRIBUTE_DEFAULTED = 1
} GTEXT_Markup_Attribute_Origin;

/**
 * @struct GTEXT_Markup_Attribute
 * @brief A view of one attribute.
 *
 * Valid until the owning element's attributes are next changed or the element
 * is freed. `value` is always NUL-terminated, in addition to `value_length`
 * saying how long it is, so a value without an embedded NUL is a C string.
 * `origin` reports whether a DTD supplied the attribute or it appeared in the
 * start tag. Programmatically created attributes are explicit by default.
 */
typedef struct {
  const char * ns;     ///< Namespace URI, or NULL for none.
  const char * prefix; ///< Prefix as written, or NULL.
  const char * local;  ///< Local name. Never NULL, never empty.
  const char * value;  ///< The value.
  size_t value_length; ///< Bytes of `value`.
  GTEXT_Markup_Attribute_Origin origin; ///< Explicit or supplied by the DTD.
} GTEXT_Markup_Attribute;

/* ---- Documents ---------------------------------------------------------- */

/**
 * @brief Create an empty document.
 *
 * The document starts with its root node (kind DOCUMENT) and nothing in it.
 *
 * @param opts Options, or NULL for gtext_markup_options_default().
 * @param out Receives the document. Written only on success.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_INVALID if `out` is NULL, or
 *   GTEXT_MARKUP_E_OOM.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_document_new(
    const GTEXT_Markup_Options * opts, GTEXT_Markup_Document ** out);

/**
 * @brief Release a document and every node made in it.
 *
 * Detached nodes included. Safe for a document of any depth. NULL is ignored.
 *
 * @param doc The document.
 */
GTEXT_API void gtext_markup_document_free(GTEXT_Markup_Document * doc);

/**
 * @brief The root node of the document, which has kind DOCUMENT.
 *
 * @param doc The document.
 * @return The root, or NULL if `doc` is NULL.
 */
GTEXT_API GTEXT_Markup_Node * gtext_markup_document_root(
    const GTEXT_Markup_Document * doc);

/**
 * @brief The document's root element, or NULL if it has none.
 *
 * @param doc The document.
 * @return The one element child of the root.
 */
GTEXT_API GTEXT_Markup_Node * gtext_markup_document_element(
    const GTEXT_Markup_Document * doc);

/**
 * @brief The document's doctype, or NULL if it has none.
 *
 * @param doc The document.
 * @return The one doctype child of the root.
 */
GTEXT_API GTEXT_Markup_Node * gtext_markup_document_doctype(
    const GTEXT_Markup_Document * doc);

/**
 * @brief Look up a document-owned XML source descriptor by location source ID.
 *
 * The returned `name` is borrowed from `doc` and remains valid until the
 * document is freed. The output is written only on success. IDs from a
 * different document have no meaning unless that document's descriptors were
 * imported by cloning its nodes.
 */
GTEXT_API bool gtext_markup_document_source(
    const GTEXT_Markup_Document * doc, uint32_t source_id,
    GTEXT_Markup_Source * out);

/**
 * @brief How many nodes the document holds, detached ones and template
 *   contents included.
 *
 * The figure max_nodes is checked against.
 *
 * @param doc The document.
 * @return The count, including the root.
 */
GTEXT_API size_t gtext_markup_document_node_count(
    const GTEXT_Markup_Document * doc);

/** Whether retained DTD knowledge is incomplete or stale after an edit. */
GTEXT_API bool gtext_markup_document_dtd_knowledge_incomplete(
    const GTEXT_Markup_Document * doc);

typedef enum {
  GTEXT_MARKUP_DTD_RESOURCE_EXTERNAL_SUBSET = 0,
  GTEXT_MARKUP_DTD_RESOURCE_PARAMETER_ENTITY,
  GTEXT_MARKUP_DTD_RESOURCE_GENERAL_ENTITY
} GTEXT_Markup_DTD_Resource_Kind;

typedef struct {
  GTEXT_Markup_DTD_Resource_Kind kind;
  const char * name;      ///< Entity name, or NULL for the external subset.
  const char * system_id; ///< Identifier requested from the resolver.
  GTEXT_Markup_Location location; ///< Request location in the document/source.
} GTEXT_Markup_DTD_Unread_Resource;

/** Count external resources declined or unavailable during DTD processing. */
GTEXT_API size_t gtext_markup_document_dtd_unread_resource_count(
    const GTEXT_Markup_Document * doc);
/** Read one unavailable resource record; strings are borrowed from the doc. */
GTEXT_API bool gtext_markup_document_dtd_unread_resource(
    const GTEXT_Markup_Document * doc, size_t index,
    GTEXT_Markup_DTD_Unread_Resource * out);

/**
 * @brief Whether the document was made by the XML parser (an XHTML document read as XML is one: its names are case-sensitive, which a query must know).
 *
 * False for an HTML parse and for a document built by hand.
 */
GTEXT_API bool gtext_markup_document_parsed_as_xml(const GTEXT_Markup_Document * doc);

/** @brief The document mode; NO_QUIRKS unless something set it. */
GTEXT_API GTEXT_Markup_Mode gtext_markup_document_mode(
    const GTEXT_Markup_Document * doc);

/** @brief Set the document mode. Ignored if `doc` is NULL. */
GTEXT_API void gtext_markup_document_set_mode(
    GTEXT_Markup_Document * doc, GTEXT_Markup_Mode mode);

/**
 * @brief Record the document's encoding and how it was decided.
 *
 * @param doc The document.
 * @param label The encoding's name as written, e.g. "utf-8", or NULL to clear
 *   it. Copied.
 * @param source How the label was reached.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_INVALID, GTEXT_MARKUP_E_ENCODING if
 *   the label is not UTF-8, or GTEXT_MARKUP_E_OOM.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_document_set_encoding(
    GTEXT_Markup_Document * doc, const char * label,
    GTEXT_Markup_Encoding_Source source);

/**
 * @brief The recorded encoding label, or NULL if none was recorded.
 *
 * @param doc The document.
 * @param source Receives how it was decided. May be NULL.
 * @return The label, valid until it is next set or the document is freed.
 */
GTEXT_API const char * gtext_markup_document_encoding(
    const GTEXT_Markup_Document * doc, GTEXT_Markup_Encoding_Source * source);

/**
 * @brief Record the XML declaration's `version` and `standalone`.
 *
 * @param doc The document.
 * @param version The version string, e.g. "1.0", or NULL for no declaration.
 *   Copied.
 * @param standalone The pseudo-attribute.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_INVALID, GTEXT_MARKUP_E_ENCODING or
 *   GTEXT_MARKUP_E_OOM.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_document_set_xml_declaration(
    GTEXT_Markup_Document * doc, const char * version,
    GTEXT_Markup_Standalone standalone);

/**
 * @brief The recorded XML version, or NULL if there is no declaration.
 *
 * @param doc The document.
 * @param standalone Receives the pseudo-attribute. May be NULL.
 * @return The version string.
 */
GTEXT_API const char * gtext_markup_document_xml_version(
    const GTEXT_Markup_Document * doc, GTEXT_Markup_Standalone * standalone);

/* ---- Making nodes ------------------------------------------------------- */

/**
 * @brief Create an element. It is detached.
 *
 * @param doc The document that will own it.
 * @param ns Namespace URI, or NULL for none.
 * @param prefix Prefix, or NULL.
 * @param local Local name. Not NULL, not empty.
 * @param out Receives the element. Written only on success.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_INVALID, GTEXT_MARKUP_E_NAME,
 *   GTEXT_MARKUP_E_ENCODING, GTEXT_MARKUP_E_LIMIT or GTEXT_MARKUP_E_OOM.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_create_element(
    GTEXT_Markup_Document * doc, const char * ns, const char * prefix,
    const char * local, GTEXT_Markup_Node ** out);

/**
 * @brief Create a text node. It is detached.
 *
 * @param doc The document that will own it.
 * @param data The characters, UTF-8; need not be terminated. A NUL is allowed.
 * @param length Bytes of `data`.
 * @param out Receives the node. Written only on success.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_INVALID, GTEXT_MARKUP_E_ENCODING,
 *   GTEXT_MARKUP_E_LIMIT or GTEXT_MARKUP_E_OOM.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_create_text(
    GTEXT_Markup_Document * doc, const char * data, size_t length,
    GTEXT_Markup_Node ** out);

/** @brief Create a CDATA section. As gtext_markup_create_text(). */
GTEXT_API GTEXT_Markup_Status gtext_markup_create_cdata(
    GTEXT_Markup_Document * doc, const char * data, size_t length,
    GTEXT_Markup_Node ** out);

/** @brief Create a comment. As gtext_markup_create_text(). */
GTEXT_API GTEXT_Markup_Status gtext_markup_create_comment(
    GTEXT_Markup_Document * doc, const char * data, size_t length,
    GTEXT_Markup_Node ** out);

/**
 * @brief Create a processing instruction. It is detached.
 *
 * @param doc The document that will own it.
 * @param target The target name. Not NULL, not empty.
 * @param data The data, UTF-8; need not be terminated.
 * @param length Bytes of `data`.
 * @param out Receives the node. Written only on success.
 * @return As gtext_markup_create_element().
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_create_processing_instruction(
    GTEXT_Markup_Document * doc, const char * target, const char * data,
    size_t length, GTEXT_Markup_Node ** out);

/** Create an unresolved entity-reference placeholder, used by XML parsers
 * when the caller explicitly elects to skip an unavailable external entity. */
GTEXT_API GTEXT_Markup_Status gtext_markup_create_entity_reference(
    GTEXT_Markup_Document * doc, const char * name, size_t length,
    GTEXT_Markup_Node ** out);
/** The unresolved entity's lexical name, or NULL for another node kind. */
GTEXT_API const char * gtext_markup_entity_reference_name(
    const GTEXT_Markup_Node * node);

/**
 * @brief Create a doctype. It is detached.
 *
 * The public and system identifiers are NULL when absent, and an empty string
 * is not the same thing: HTML tells `<!DOCTYPE html>` from
 * `<!DOCTYPE html PUBLIC "">`.
 *
 * @param doc The document that will own it.
 * @param name The name, which HTML allows to be empty. Not NULL.
 * @param public_id The public identifier, or NULL.
 * @param system_id The system identifier, or NULL.
 * @param out Receives the node. Written only on success.
 * @return As gtext_markup_create_element(), except that an empty name is
 *   accepted.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_create_doctype(
    GTEXT_Markup_Document * doc, const char * name, const char * public_id,
    const char * system_id, GTEXT_Markup_Node ** out);

/**
 * @brief Create an empty document fragment. It is detached.
 *
 * @param doc The document that will own it.
 * @param out Receives the fragment. Written only on success.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_INVALID, GTEXT_MARKUP_E_LIMIT or
 *   GTEXT_MARKUP_E_OOM.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_create_fragment(
    GTEXT_Markup_Document * doc, GTEXT_Markup_Node ** out);

/* ---- Reading a node ----------------------------------------------------- */

/** @brief The kind of a node; GTEXT_MARKUP_KIND_NONE for NULL. */
GTEXT_API GTEXT_Markup_Kind gtext_markup_node_kind(const GTEXT_Markup_Node * node);

/** @brief The document that owns a node, or NULL for NULL. */
GTEXT_API GTEXT_Markup_Document * gtext_markup_node_document(
    const GTEXT_Markup_Node * node);

/**
 * @brief The parent of a node, or NULL for a detached node, a document, and a
 *   template's content.
 */
GTEXT_API GTEXT_Markup_Node * gtext_markup_node_parent(
    const GTEXT_Markup_Node * node);

/** @brief The first child, or NULL. */
GTEXT_API GTEXT_Markup_Node * gtext_markup_node_first_child(
    const GTEXT_Markup_Node * node);

/** @brief The last child, or NULL. */
GTEXT_API GTEXT_Markup_Node * gtext_markup_node_last_child(
    const GTEXT_Markup_Node * node);

/** @brief The sibling before this node, or NULL. */
GTEXT_API GTEXT_Markup_Node * gtext_markup_node_previous_sibling(
    const GTEXT_Markup_Node * node);

/** @brief The sibling after this node, or NULL. */
GTEXT_API GTEXT_Markup_Node * gtext_markup_node_next_sibling(
    const GTEXT_Markup_Node * node);

/**
 * @brief How many children a node has. O(children).
 *
 * @param node The node.
 * @return The count; 0 for NULL and for a kind that has none.
 */
GTEXT_API size_t gtext_markup_node_child_count(const GTEXT_Markup_Node * node);

/**
 * @brief The next node in document order, without recursion.
 *
 * With `skip_children` false this is the next node of a pre-order walk of the
 * subtree rooted at `root`: the first child if there is one, else the next
 * sibling, else the next sibling of the nearest ancestor that has one. With
 * `skip_children` true the node's own children are passed over, which is what
 * a walk that has decided not to look inside wants.
 *
 * Template contents are not visited: they are not children.
 *
 * @param root Where the walk is confined to. The walk ends, returning NULL,
 *   rather than leave this subtree.
 * @param node The current node, `root` itself to start.
 * @param skip_children Do not descend into `node`.
 * @return The next node, or NULL at the end.
 */
GTEXT_API GTEXT_Markup_Node * gtext_markup_node_next_in_order(
    const GTEXT_Markup_Node * root, const GTEXT_Markup_Node * node,
    bool skip_children);

/**
 * @brief The characters of a text node, CDATA section or comment, or the data
 *   of a processing instruction.
 *
 * Always NUL-terminated in addition to `*length`.
 *
 * @param node The node.
 * @param length Receives the byte count. May be NULL.
 * @return The data, or NULL for any other kind of node.
 */
GTEXT_API const char * gtext_markup_node_data(
    const GTEXT_Markup_Node * node, size_t * length);

/**
 * @brief Replace the data of a text node, CDATA section, comment or processing
 *   instruction.
 *
 * @param node The node.
 * @param data The new data; need not be terminated.
 * @param length Bytes of `data`.
 * @return GTEXT_MARKUP_OK; GTEXT_MARKUP_E_INVALID for another kind of node;
 *   GTEXT_MARKUP_E_ENCODING; GTEXT_MARKUP_E_OOM, in which case the old data is
 *   still there.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_node_set_data(
    GTEXT_Markup_Node * node, const char * data, size_t length);

/**
 * @brief The concatenated text of a node and everything inside it.
 *
 * Text nodes and CDATA sections only; comments and processing instructions
 * contribute nothing. This is what `textContent` is.
 *
 * @param node The node.
 * @param allocator The allocator for the result, or NULL for the document's.
 * @param out Receives a NUL-terminated string to release with
 *   gtext_markup_string_free(). Written only on success.
 * @param length Receives its length. May be NULL.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_INVALID or GTEXT_MARKUP_E_OOM.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_node_text_content(
    const GTEXT_Markup_Node * node, const GTEXT_Allocator * allocator,
    char ** out, size_t * length);

/**
 * @brief Release a string this API returned.
 *
 * @param allocator The allocator it was made with, or NULL for the default.
 * @param string The string. NULL is ignored.
 */
GTEXT_API void gtext_markup_string_free(
    const GTEXT_Allocator * allocator, char * string);

/** @brief An element's namespace URI, or NULL. */
GTEXT_API const char * gtext_markup_element_namespace(
    const GTEXT_Markup_Node * element);

/** @brief An element's prefix, or NULL. */
GTEXT_API const char * gtext_markup_element_prefix(
    const GTEXT_Markup_Node * element);

/** @brief An element's local name; NULL if the node is not an element. */
GTEXT_API const char * gtext_markup_element_local_name(
    const GTEXT_Markup_Node * element);

/**
 * @brief Is this element called this?
 *
 * Namespace and local name, which is what makes two names the same; the
 * prefix is ignored.
 *
 * @param element The node.
 * @param ns A namespace URI, or NULL for none.
 * @param local A local name.
 * @return true if `element` is an element with exactly that name.
 */
GTEXT_API bool gtext_markup_element_is(const GTEXT_Markup_Node * element,
    const char * ns, const char * local);

/** @brief A processing instruction's target; NULL for any other kind. */
GTEXT_API const char * gtext_markup_pi_target(const GTEXT_Markup_Node * node);

/** @brief A doctype's name; NULL for any other kind. */
GTEXT_API const char * gtext_markup_doctype_name(const GTEXT_Markup_Node * node);

/** @brief A doctype's public identifier; NULL if absent or not a doctype. */
GTEXT_API const char * gtext_markup_doctype_public_id(
    const GTEXT_Markup_Node * node);

/** @brief A doctype's system identifier; NULL if absent or not a doctype. */
GTEXT_API const char * gtext_markup_doctype_system_id(
    const GTEXT_Markup_Node * node);

/** The retained source kind of one internal-subset item. */
typedef enum {
  GTEXT_MARKUP_DTD_ELEMENT,
  GTEXT_MARKUP_DTD_ATTLIST,
  GTEXT_MARKUP_DTD_ENTITY,
  GTEXT_MARKUP_DTD_NOTATION,
  GTEXT_MARKUP_DTD_COMMENT,
  GTEXT_MARKUP_DTD_PROCESSING_INSTRUCTION,
  GTEXT_MARKUP_DTD_PARAMETER_REFERENCE
} GTEXT_Markup_DTD_Syntax_Kind;

/**
 * One retained DTD construct. Raw internal and external syntax entries have a
 * NULL `parameter_entity`. Generated entries identify the PE that supplied
 * the declaration text.
 */
typedef struct {
  GTEXT_Markup_DTD_Syntax_Kind kind;
  const char * source; ///< Decoded UTF-8 construct; valid while the doctype lives.
  size_t source_length;
  GTEXT_Markup_Location location;
  /** NULL for raw syntax; the PE name that generated this declaration otherwise. */
  const char * parameter_entity;
} GTEXT_Markup_DTD_Syntax;

/** Number of retained internal-subset constructs (zero for non-doctypes). */
GTEXT_API size_t gtext_markup_doctype_syntax_count(
    const GTEXT_Markup_Node * node);

/** Read a retained construct in declaration order. False for an invalid index;
 * `out` is unchanged when false is returned. */
GTEXT_API bool gtext_markup_doctype_syntax(
    const GTEXT_Markup_Node * node, size_t index,
    GTEXT_Markup_DTD_Syntax * out);

/** Finite work ceiling for checked internal DTD edits. Zero selects 1 MiB. */
typedef struct {
  size_t max_work;
} GTEXT_Markup_DTD_Edit_Options;

/** Return the finite default DTD edit limits. */
GTEXT_API GTEXT_Markup_DTD_Edit_Options
gtext_markup_dtd_edit_options_default(void);

/**
 * Insert or replace one checked internal-subset construct. `source` is the
 * complete XML spelling (for example `<!ELEMENT item EMPTY>`). The kind
 * selects the grammar production, and the shared XML DTD parser verifies both
 * that construct and the resulting complete internal subset before it is
 * published. External syntax remains read-only. Every failure leaves the
 * doctype unchanged.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_doctype_insert_syntax(
    GTEXT_Markup_Node * doctype, size_t index,
    GTEXT_Markup_DTD_Syntax_Kind kind, const char * source, size_t length,
    const GTEXT_Markup_DTD_Edit_Options * options);
GTEXT_API GTEXT_Markup_Status gtext_markup_doctype_replace_syntax(
    GTEXT_Markup_Node * doctype, size_t index,
    GTEXT_Markup_DTD_Syntax_Kind kind, const char * source, size_t length,
    const GTEXT_Markup_DTD_Edit_Options * options);
/** Move an item to `to_index` in the final list. */
GTEXT_API GTEXT_Markup_Status gtext_markup_doctype_move_syntax(
    GTEXT_Markup_Node * doctype, size_t from_index, size_t to_index,
    const GTEXT_Markup_DTD_Edit_Options * options);
/** Remove one internal-subset item. */
GTEXT_API GTEXT_Markup_Status gtext_markup_doctype_remove_syntax(
    GTEXT_Markup_Node * doctype, size_t index,
    const GTEXT_Markup_DTD_Edit_Options * options);

/**
 * Number of constructs in the caller-resolved external subset retained as a
 * read-only snapshot when a DOM tree is built. Event-only parsing has no
 * doctype node and therefore retains no snapshot. The snapshot has independent ownership from the
 * editable internal subset. It contains declarations, comments and processing
 * instructions in source order, including constructs in INCLUDE sections.
 * The text declaration and conditional-section delimiters are parser controls
 * and are not snapshot
 * entries; ignored-section contents are not retained. `source` contains the
 * complete construct after source decoding, with its original line-end code
 * points retained; it may differ from the resolver bytes when transcoding was
 * needed. Each construct's location identifies its start in the physical external source: source_id is
 * that source, offset is its decoded UTF-8 byte coordinate (a UTF-8 BOM counts;
 * a BOM consumed while transcoding does not), and line/column are relative to
 * that source.
 */
GTEXT_API size_t gtext_markup_doctype_external_syntax_count(
    const GTEXT_Markup_Node * node);

/**
 * Read one retained external-subset construct in declaration order. The
 * returned source is borrowed from the doctype and remains valid while it
 * lives. Its location identifies the construct start in the physical external
 * source, including that source's ID, decoded offset, line and column.
 * False for an invalid index or NULL output; `out` is unchanged then.
 */
GTEXT_API bool gtext_markup_doctype_external_syntax(
    const GTEXT_Markup_Node * node, size_t index, GTEXT_Markup_DTD_Syntax * out);

/**
 * @brief Get the number of constructs parsed from parameter-entity text.
 * @param node A doctype node; may be NULL or another node kind.
 * @return The generated construct count, or zero for other nodes.
 */
GTEXT_API size_t gtext_markup_doctype_generated_syntax_count(
    const GTEXT_Markup_Node * node);
/**
 * @brief Read a construct parsed from parameter-entity replacement text.
 * @param node A doctype node.
 * @param index Zero-based generated construct index.
 * @param out Receives the construct kind, source, location, and PE name.
 * @return True when `index` is in range; false otherwise. `out` is unchanged
 *   on failure.
 *
 * The returned `source` and `parameter_entity` strings are borrowed from the
 * doctype and remain valid while it lives. `parameter_entity` is the lexical
 * name of the PE frame that supplied the construct text. The returned kind is
 * one of GTEXT_MARKUP_DTD_ELEMENT, GTEXT_MARKUP_DTD_ATTLIST,
 * GTEXT_MARKUP_DTD_ENTITY, GTEXT_MARKUP_DTD_NOTATION,
 * GTEXT_MARKUP_DTD_COMMENT, GTEXT_MARKUP_DTD_PROCESSING_INSTRUCTION, or
 * GTEXT_MARKUP_DTD_PARAMETER_REFERENCE. Thus entries can be declarations,
 * comments, processing instructions, or retained parameter-entity reference
 * constructs.
 */
GTEXT_API bool gtext_markup_doctype_generated_syntax(
    const GTEXT_Markup_Node * node, size_t index, GTEXT_Markup_DTD_Syntax * out);

/**
 * @brief Where in the source a node came from.
 *
 * @param node The node.
 * @param out Receives the location. May be NULL to only ask.
 * @return true if the node has one; false for a node built by hand.
 */
GTEXT_API bool gtext_markup_node_location(
    const GTEXT_Markup_Node * node, GTEXT_Markup_Location * out);

/**
 * @brief Number of physical-source runs retained for a text node.
 *
 * XML parsing with DTD processing can produce one text node from adjacent
 * physical sources; each run is represented by a span. Other node kinds and
 * nodes without recorded provenance report zero.
 */
GTEXT_API size_t gtext_markup_text_span_count(const GTEXT_Markup_Node * node);
/**
 * @brief Read one text provenance span.
 *
 * The span is copied to `out`; it remains meaningful while the node's owning
 * document is alive. Returns false for a non-text node, a NULL output, or an
 * out-of-range index.
 */
GTEXT_API bool gtext_markup_text_span_at(const GTEXT_Markup_Node * node,
    size_t index, GTEXT_Markup_Text_Span * out);

/**
 * @brief Record where a node came from.
 *
 * Parsers call this; so may anything that builds a tree from a source.
 *
 * @param node The node.
 * @param location The location, or NULL to clear it.
 */
GTEXT_API void gtext_markup_node_set_location(
    GTEXT_Markup_Node * node, const GTEXT_Markup_Location * location);

/* ---- Attributes --------------------------------------------------------- */

/**
 * @brief How many attributes an element has.
 *
 * @param element The element.
 * @return The count; 0 for NULL and for a node that is not an element.
 */
GTEXT_API size_t gtext_markup_element_attribute_count(
    const GTEXT_Markup_Node * element);

/**
 * @brief Read the attribute at an index.
 *
 * Attributes keep the order they were added in; replacing one's value does not
 * move it.
 *
 * @param element The element.
 * @param index From 0 to the count minus 1.
 * @param out Receives the view. Written only on success.
 * @return GTEXT_MARKUP_OK or GTEXT_MARKUP_E_INVALID.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_element_attribute_at(
    const GTEXT_Markup_Node * element, size_t index,
    GTEXT_Markup_Attribute * out);

/**
 * @brief Find an attribute by name.
 *
 * @param element The element.
 * @param ns The attribute's namespace URI, or NULL for none.
 * @param local The local name.
 * @param out Receives the view. May be NULL to only ask.
 * @return true if there is such an attribute.
 */
GTEXT_API bool gtext_markup_element_get_attribute(
    const GTEXT_Markup_Node * element, const char * ns, const char * local,
    GTEXT_Markup_Attribute * out);

/**
 * @brief Add an attribute, or replace the value of the one with this name.
 *
 * Names are compared by namespace and local name. Replacing keeps the
 * attribute's place in the order and takes the new prefix.
 *
 * @param element The element.
 * @param ns The namespace URI, or NULL for none.
 * @param prefix The prefix, or NULL.
 * @param local The local name. Not NULL, not empty.
 * @param value The value; need not be terminated.
 * @param value_length Bytes of `value`.
 * @return GTEXT_MARKUP_OK; GTEXT_MARKUP_E_INVALID; GTEXT_MARKUP_E_NAME;
 *   GTEXT_MARKUP_E_ENCODING; GTEXT_MARKUP_E_LIMIT if the element is full;
 *   GTEXT_MARKUP_E_OOM. On failure the element is unchanged.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_element_set_attribute(
    GTEXT_Markup_Node * element, const char * ns, const char * prefix,
    const char * local, const char * value, size_t value_length);

/**
 * @brief Remove an attribute.
 *
 * @param element The element.
 * @param ns The namespace URI, or NULL for none.
 * @param local The local name.
 * @return true if there was one to remove.
 */
GTEXT_API bool gtext_markup_element_remove_attribute(
    GTEXT_Markup_Node * element, const char * ns, const char * local);

/**
 * @brief Record where an attribute came from.
 *
 * @param element The element.
 * @param index The attribute's index.
 * @param location The location, or NULL to clear it.
 * @return GTEXT_MARKUP_OK or GTEXT_MARKUP_E_INVALID.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_element_set_attribute_location(
    GTEXT_Markup_Node * element, size_t index,
    const GTEXT_Markup_Location * location);

/** Set whether a parsed attribute was explicit or supplied by a DTD. */
GTEXT_API GTEXT_Markup_Status gtext_markup_element_set_attribute_origin(
    GTEXT_Markup_Node * element, size_t index,
    GTEXT_Markup_Attribute_Origin origin);

/**
 * @brief Where an attribute came from.
 *
 * @param element The element.
 * @param index The attribute's index.
 * @param out Receives the location. May be NULL to only ask.
 * @return true if it has one.
 */
GTEXT_API bool gtext_markup_element_attribute_location(
    const GTEXT_Markup_Node * element, size_t index,
    GTEXT_Markup_Location * out);

/* ---- Template contents -------------------------------------------------- */

/**
 * @brief A template element's content, or NULL if it has none.
 *
 * HTML's `<template>` keeps what is inside it in a fragment that is not its
 * child, so that a walk of the page does not see it. The fragment belongs to
 * the element and is released with it. It has no parent, and cannot be
 * inserted or freed on its own.
 *
 * @param element The element.
 * @return The fragment, or NULL.
 */
GTEXT_API GTEXT_Markup_Node * gtext_markup_element_template_content(
    const GTEXT_Markup_Node * element);

/**
 * @brief Give an element a content fragment if it lacks one.
 *
 * @param element The element.
 * @param out Receives the fragment, new or existing. May be NULL.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_INVALID, GTEXT_MARKUP_E_LIMIT or
 *   GTEXT_MARKUP_E_OOM.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_element_ensure_template_content(
    GTEXT_Markup_Node * element, GTEXT_Markup_Node ** out);

/* ---- Changing the tree -------------------------------------------------- */

/**
 * @brief Insert a node before another, or at the end.
 *
 * Inserting a node that is already in the tree moves it. Inserting a fragment
 * moves its children, in order, and leaves the fragment empty. The rules are
 * the DOM's: a document may hold one element and one doctype, the doctype
 * before the element, and no text; elements and fragments hold elements, text,
 * CDATA sections, comments and processing instructions; nothing else holds
 * anything; and a node cannot be put inside itself.
 *
 * All of it is checked before anything moves, so a failed insertion leaves the
 * tree as it was. When more than one thing is wrong the status is the first of
 * these that applies, in this order: a NULL argument (GTEXT_MARKUP_E_INVALID); a
 * node from another document (GTEXT_MARKUP_E_WRONG_DOCUMENT); a `child` that is
 * a document or a template's content (GTEXT_MARKUP_E_INVALID); a `parent` that
 * cannot hold children (GTEXT_MARKUP_E_HIERARCHY); a `reference` that is not a
 * child of `parent` (GTEXT_MARKUP_E_INVALID); a cycle, or a kind that may not go
 * there (GTEXT_MARKUP_E_HIERARCHY); the document's own rules about one element
 * and one doctype (GTEXT_MARKUP_E_HIERARCHY); and last the depth limit
 * (GTEXT_MARKUP_E_LIMIT). The order matters to a caller that acts differently
 * on a refused request than on a request that was never valid.
 *
 * Inserting a node before itself leaves it where it is, when it is a child of
 * `parent`; when it is not, the reference is invalid like any other.
 *
 * @param parent The new parent.
 * @param child The node to insert.
 * @param reference A child of `parent` to insert before, or NULL for the end.
 * @return GTEXT_MARKUP_OK; GTEXT_MARKUP_E_INVALID for a NULL, a `reference`
 *   that is not a child of `parent`, or a `child` that is a document;
 *   GTEXT_MARKUP_E_WRONG_DOCUMENT; GTEXT_MARKUP_E_HIERARCHY;
 *   GTEXT_MARKUP_E_LIMIT.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_insert_before(
    GTEXT_Markup_Node * parent, GTEXT_Markup_Node * child,
    GTEXT_Markup_Node * reference);

/**
 * @brief Insert a node as the last child.
 *
 * As gtext_markup_insert_before() with a NULL reference.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_append_child(
    GTEXT_Markup_Node * parent, GTEXT_Markup_Node * child);

/**
 * @brief Take a node out of the tree.
 *
 * The node and everything under it stay valid and stay the document's, as a
 * detached subtree. Removing a node that is already detached does nothing.
 *
 * @param node The node. Not a document, and not a template's content.
 * @return GTEXT_MARKUP_OK, or GTEXT_MARKUP_E_INVALID.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_remove(GTEXT_Markup_Node * node);

/**
 * @brief Put one node where another is, and detach the other.
 *
 * Equivalent to insert-then-remove, but checked as one step: the rules are
 * applied to the tree as it would be after the replacement, so replacing a
 * document's only element with another element succeeds.
 *
 * @param old_child A child of some parent.
 * @param new_child The node to put there; a fragment's children are spliced in.
 * @return As gtext_markup_insert_before().
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_replace(
    GTEXT_Markup_Node * old_child, GTEXT_Markup_Node * new_child);

/**
 * @brief Release a detached node and everything under it.
 *
 * @param node A node with no parent.
 * @return GTEXT_MARKUP_OK; GTEXT_MARKUP_E_STATE if the node is still in the
 *   tree, or is a document or a template's content; GTEXT_MARKUP_E_INVALID for
 *   NULL.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_node_free(GTEXT_Markup_Node * node);

/**
 * @brief Copy a node, into any document.
 *
 * The copy is detached, in `doc`. This is how a node crosses documents:
 * clone it into the target and free the original. Namespaces and prefixes are
 * copied as written; nothing is added or repaired. A document cannot be cloned
 * (use a fragment's children), and a template's content is cloned with it.
 * Doctypes copy retained declarations, the external snapshot, unresolved
 * resource records and source provenance. Their destination document inherits
 * the doctype's incomplete-knowledge state. Entity-reference nodes preserve
 * their unresolved spelling and provenance.
 *
 * The copy is not checked against `doc`'s max_depth: it is a detached tree, and
 * the limit is applied, subtree height included, when it is inserted. The node
 * and attribute limits are applied as it is built.
 *
 * @param doc The document the copy will belong to.
 * @param node The node to copy.
 * @param deep Copy the children too.
 * @param out Receives the copy. Written only on success, and on failure
 *   nothing is left behind in `doc`.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_INVALID, GTEXT_MARKUP_E_LIMIT or
 *   GTEXT_MARKUP_E_OOM.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_clone(
    GTEXT_Markup_Document * doc, const GTEXT_Markup_Node * node, bool deep,
    GTEXT_Markup_Node ** out);

/**
 * @brief Are two trees the same?
 *
 * Structural: kinds, names (namespace and local, and the prefix too), data,
 * attributes as a set - order is not significant, as in the DOM - children in
 * order, template contents, and retained DTD syntax and external snapshot
 * content. DTD source locations, identities and ownership are ignored, as are
 * other source locations and which document a node lives in.
 *
 * @param a A node.
 * @param b A node.
 * @return true if they are equal. Two NULLs are equal.
 */
GTEXT_API bool gtext_markup_equal(
    const GTEXT_Markup_Node * a, const GTEXT_Markup_Node * b);

/**
 * @brief Write a readable description of a subtree, for debugging and tests.
 *
 * One node per line, indented two spaces a level; an element's attributes on
 * lines of their own under it; text, comments and values quoted with
 * non-printing bytes escaped, so that the output is unambiguous and one tree
 * has exactly one dump. Names are written `{namespace}prefix:local`.
 *
 * @param node The root of what to describe.
 * @param file Where to write.
 */
GTEXT_API void gtext_markup_dump(const GTEXT_Markup_Node * node, FILE * file);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_MARKUP_MARKUP_TREE_H
