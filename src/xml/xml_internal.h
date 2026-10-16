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
 * The XML parser's internal state.
 *
 * ## Shape
 *
 * One pass over the whole input, no recursion: the open elements are a heap
 * array (`frames`) bounded by `max_depth`, so a document nested a million deep
 * is refused at the limit and never reaches the C stack. Every function that
 * can fail returns a GTEXT_XML_Status and has already recorded where and why in
 * `ctx->err`; the first failure wins and nothing overwrites it.
 *
 * The cursor (`pos`, `line`, `column`) moves only through xml_consume(), the
 * one place that knows what a line end is, so a position can never disagree
 * with the bytes consumed.
 *
 * ## Seams for the DTD phase
 *
 * Phase 4 attaches entity resolution, attribute types and default attributes
 * here. The three function pointers in xml_seams are NULL for every public
 * parse, and each is consulted at exactly one place, so Phase 4 changes no
 * Phase 1 behaviour by being added. They are reachable from tests through
 * xml_parse_with_seams() so that "NULL changes nothing, non-NULL is honoured"
 * is checked rather than assumed.
 */

#ifndef GHOTI_IO_GTEXT_SRC_XML_XML_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_XML_XML_INTERNAL_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/xml.h>
#include <ghoti.io/text/xml/xml_stream.h>
#include <stddef.h>
#include <stdint.h>

#include "../markup/markup_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Defaults ----------------------------------------------------------- */

#define XML_DEFAULT_MAX_INPUT ((size_t)64 * 1024 * 1024)
#define XML_DEFAULT_MAX_DEPTH ((size_t)256)
#define XML_DEFAULT_MAX_ATTRIBUTES ((size_t)1024)
#define XML_DEFAULT_MAX_ATTRIBUTE_BYTES ((size_t)16 * 1024 * 1024)
#define XML_DEFAULT_MAX_NAME_BYTES ((size_t)8192)
#define XML_DEFAULT_MAX_NODES ((size_t)10000000)
#define XML_DEFAULT_MAX_NAMESPACES ((size_t)256)
#define XML_DEFAULT_MAX_SOURCE_DEPTH ((size_t)64)
#define XML_DEFAULT_MAX_EXTERNAL_SOURCE_BYTES ((size_t)16 * 1024 * 1024)
#define XML_DEFAULT_MAX_EXTERNAL_BYTES ((size_t)64 * 1024 * 1024)
#define XML_DEFAULT_MAX_EXPANDED_BYTES ((size_t)64 * 1024 * 1024)
#define XML_DEFAULT_MAX_ENTITY_DEPTH ((size_t)32)
#define XML_DEFAULT_MAX_REFERENCES ((size_t)1000000)
#define XML_DEFAULT_MAX_DECLARATIONS ((size_t)100000)
#define XML_DEFAULT_MAX_TOTAL_WORK ((size_t)256 * 1024 * 1024)
#define XML_DEFAULT_MAX_VALIDATION_WORK ((size_t)64 * 1024 * 1024)
#define XML_DEFAULT_MAX_VALIDATION_MEMORY ((size_t)16 * 1024 * 1024)
#define XML_DEFAULT_MAX_VALIDATION_DIAGNOSTICS ((size_t)256)
#define XML_DEFAULT_MAX_PULL_QUEUE_BYTES ((size_t)1024 * 1024)
#define XML_DEFAULT_MAX_PULL_QUEUE_EVENTS ((size_t)256)
#define XML_WRITE_DEFAULT_MAX_DEPTH ((size_t)256)
#define XML_WRITE_DEFAULT_MAX_OUTPUT ((size_t)256 * 1024 * 1024)

/* ---- Writer (xml_writer.c) ---------------------------------------------- */

/** The writer's checking walk alone: everything gtext_xml_write() would refuse
 *  is refused, and nothing is written. For the file form, which must not create
 *  a file for a tree it is going to refuse. */
GTEXT_INTERNAL_API GTEXT_XML_Status xml_write_check(
    const GTEXT_Markup_Document * doc, const GTEXT_XML_Write_Options * opts,
    GTEXT_XML_Write_Result * result);

/* ---- Encodings (xml_encoding.c) ----------------------------------------- */

/** What the document is in, as far as the first bytes and the label say. */
typedef enum {
  XML_FAMILY_UTF8 = 0, ///< UTF-8, or nothing says otherwise.
  XML_FAMILY_UTF16LE,
  XML_FAMILY_UTF16BE,
  XML_FAMILY_UTF32LE,
  XML_FAMILY_UTF32BE,
  XML_FAMILY_LATIN1, ///< Declared ISO-8859-1.
  XML_FAMILY_ASCII   ///< Declared US-ASCII.
} xml_family;

/** An encoding label from the declaration, by family. */
typedef enum {
  XML_LABEL_OTHER = 0,
  XML_LABEL_UTF8,
  XML_LABEL_UTF16,
  XML_LABEL_UTF32,
  XML_LABEL_LATIN1,
  XML_LABEL_ASCII
} xml_label;

/** The effective XML character profile. Zero initialization means XML 1.0. */
typedef enum {
  XML_VERSION_10 = 0,
  XML_VERSION_11
} xml_version;

/* ---- Seams -------------------------------------------------------------- */

/** An attribute a declaration supplies when the start tag omits it. */
typedef struct {
  const char * name;  ///< The name as written, prefix included. NUL-terminated.
  const char * value; ///< The default value. NUL-terminated.
} xml_default_attribute;

/**
 * Where the DTD phase attaches. All NULL means "no DTD processing", which is
 * what Phase 1 is.
 */
typedef struct {
  /** Replacement text for a general entity the five predefined ones do not
   *  cover. Return non-zero and set *text / *length to use it as character
   *  data; zero for "not declared", which is an error. */
  int (*resolve_entity)(void * user, const char * name, size_t name_length,
      const char ** text, size_t * length);
  /**
   * Test/internal seam for a parsed replacement source. A successful callback
   * returns borrowed UTF-8 replacement bytes and a non-NULL stable identity.
   * Identity values must uniquely identify sources among active expansions so
   * recursive inclusion can be detected. The bytes and identity storage must
   * remain valid until the corresponding source frame is popped.
   */
  int (*open_entity_source)(void * user, const char * name,
      size_t name_length, const void ** identity, const void ** bytes,
      size_t * length);
  /** Non-zero if the attribute is declared with a tokenized type, so that its
   *  value is normalized further (runs of spaces collapse, the ends trim). */
  int (*attribute_type)(void * user, const char * element, size_t element_length,
      const char * attribute, size_t attribute_length);
  /** The defaults for an element; returns how many and sets *list. */
  size_t (*default_attributes)(void * user, const char * element,
      size_t element_length, const xml_default_attribute ** list);
  void * user;
} xml_seams;

/* ---- State -------------------------------------------------------------- */

/** One namespace declaration in scope. One allocation: prefix, NUL, uri, NUL. */
typedef struct {
  char * data;
  size_t prefix_length; ///< 0 is the default namespace.
  size_t uri_length;    ///< 0 undeclares (only for the default namespace).
} xml_binding;

/** An open element. */
typedef struct {
  GTEXT_Markup_Node * node;
  const unsigned char * name; ///< As written, in the input.
  size_t name_length;
  size_t binding_mark; ///< bindings.count when the element opened.
  size_t name_offset;  ///< Event parsing: where the name is kept, in `names`,
                       ///< because the input it was read from is not kept.
} xml_frame;

/** Physical input kinds understood by the shared parser. */
typedef enum {
  XML_SOURCE_DOCUMENT = 0,
  XML_SOURCE_INTERNAL_ENTITY,
  XML_SOURCE_EXTERNAL_ENTITY,
  XML_SOURCE_PARAMETER_ENTITY,
  XML_SOURCE_INTERNAL_PARAMETER_ENTITY,
  XML_SOURCE_EXTERNAL_SUBSET
} xml_source_kind;

enum { XML_SOURCE_GRAMMAR_CONTENT = 1 };
#define XML_SOURCE_IS_PARAMETER_ENTITY(kind) \
  ((kind) == XML_SOURCE_PARAMETER_ENTITY \
      || (kind) == XML_SOURCE_INTERNAL_PARAMETER_ENTITY)

/**
 * Heap frame for a physical XML source. The active parser cursor remains in
 * xml_ctx for compatibility with the lexer; a suspended parent frame records
 * its cursor and grammar boundary so push/pop never recurse in C.
 * `bytes` are borrowed until pop (resolver ownership is added with the resolver
 * API); identity is a stable opaque value supplied by the caller/test seam.
 */
typedef struct {
  const unsigned char * bytes;
  size_t length;
  size_t pos;
  size_t base;
  size_t line;
  size_t column;
  uint32_t cursor_previous;
  size_t entry_element_depth;
  size_t entry_binding_count;
  size_t bytes_charged;
  unsigned int grammar_context;
  xml_source_kind kind;
  const void * identity;
  const char * base_uri; ///< Physical base for relative external identifiers.
  char * owned_base_uri; ///< Resolver URI retained until this source is popped.
  char * owned_name;
  char * owned_bytes;
  char * owned_decoded;
  void (*external_close)(void *);
  void * external_handle;
  uint32_t source_id;
  uint32_t reference_source_id;
  xml_version version;
  unsigned char suspended;
  unsigned char external_source;
} xml_source_frame;

/** A parameter entity declaration encountered earlier in the DTD. */
typedef struct {
  char * name;
  size_t name_length;
  unsigned char * replacement;
  size_t replacement_length;
  char * public_id;
  char * system_id;
  unsigned char external;
  const void * identity;
} xml_parameter_entity;

/** One effective general entity declaration used while reading the document. */
typedef struct {
  char * name;
  size_t name_length;
  unsigned char * replacement;
  size_t replacement_length;
  char * public_id;
  char * system_id;
  char * base_uri;
  char * notation_name;
  size_t notation_name_length;
  GTEXT_Markup_Location declaration_location;
  const void * identity;
  unsigned char external;
  unsigned char unparsed;
  unsigned char external_context;
} xml_general_entity;

/** One effective DTD binding. The declaration text remains owned by the
 * doctype syntax model; these records are temporary lookup keys for this
 * parse, so they never become writable source syntax. */
typedef struct {
  int kind; /* GTEXT_MARKUP_DTD_* */
  char * element_name; /* ATTLIST element name, otherwise NULL */
  size_t element_name_length;
  char * name; /* declaration name, or ATTLIST attribute name */
  size_t name_length;
  size_t syntax_index;
  unsigned char external;
  unsigned char attribute_tokenized;
  unsigned char attribute_type; /* CDATA..NOTATION, private XML_ATYPE_* */
  unsigned char attribute_required;
  unsigned char attribute_default_kind; /* 0 none, 1 default, 2 fixed */
  char * attribute_default_value;
  size_t attribute_default_length;
  char * attribute_enumeration;
  size_t attribute_enumeration_length;
  GTEXT_Markup_Location declaration_location;
} xml_dtd_binding;

enum { XML_ATYPE_CDATA, XML_ATYPE_ID, XML_ATYPE_IDREF, XML_ATYPE_IDREFS,
  XML_ATYPE_ENTITY, XML_ATYPE_ENTITIES, XML_ATYPE_NMTOKEN, XML_ATYPE_NMTOKENS,
  XML_ATYPE_ENUMERATION, XML_ATYPE_NOTATION };

typedef struct {
  size_t target;
  size_t name_offset; /* SIZE_MAX denotes epsilon. */
  size_t name_length;
} xml_nfa_edge;

typedef struct {
  xml_nfa_edge edges[2];
  unsigned char edge_count;
} xml_nfa_state;

typedef struct {
  size_t name_offset;
  size_t name_length;
} xml_content_name;

typedef enum {
  XML_CONTENT_EMPTY,
  XML_CONTENT_ANY,
  XML_CONTENT_MIXED,
  XML_CONTENT_CHILDREN
} xml_content_kind;

typedef struct {
  char * element_name;
  size_t element_name_length;
  char * declaration;
  size_t declaration_length;
  GTEXT_Markup_Location location;
  xml_content_kind kind;
  xml_nfa_state * states;
  size_t state_count;
  size_t state_capacity;
  size_t start_state;
  size_t accept_state;
  xml_content_name * mixed_names;
  size_t mixed_name_count;
  unsigned char external;
  unsigned char ambiguity_reported;
  unsigned char duplicate_reported;
  size_t validation_budget_bytes;
} xml_content_decl;

typedef struct {
  const xml_content_decl * model;
  unsigned char * active;
  size_t active_bytes;
  GTEXT_Markup_Location location;
  GTEXT_Markup_Location child_location;
} xml_validation_frame;

typedef struct {
  GTEXT_XML_External_Source source;
  markup_buf bytes;
  xml_source_kind source_kind;
  unsigned char opened;
  unsigned char frame_pushed;
  unsigned char declined;
} xml_resolved_resource;

/** Persistent DOCTYPE header and node while a pull reader yields inside its
 * internal subset. The parser owns this until the declaration completes. */
typedef struct {
  markup_buf name;
  markup_buf public_id;
  markup_buf system_id;
  GTEXT_Markup_Location start;
  GTEXT_Markup_Node * node;
  unsigned char active;
  unsigned char header_done;
  unsigned char has_subset;
  unsigned char internal_subset_done;
  unsigned char doctype_closed;
  unsigned char external_subset_pending;
  unsigned char external_subset_done;
  unsigned char has_public;
  unsigned char has_system;
} xml_doctype_continuation;

/** An attribute of the start tag being read. */
typedef struct {
  size_t name_length;
  size_t colon;       ///< Offset of the first ':' within the name, or SIZE_MAX.
  size_t value_offset; ///< Into `attr_pool`.
  size_t value_length;
  uint32_t raw_hash;
  GTEXT_Markup_Location where;
  const unsigned char * name_ptr; ///< Where the name is: the input, or the
                                  ///< seam's string for a defaulted attribute.
  int kind;           ///< XML_ATTR_*.
  const char * ns;    ///< Resolved namespace URI, or NULL.
  unsigned char defaulted;
  unsigned char normalization_changed;
  unsigned char fixed_value_matches;
} xml_attr;

/** What an attribute of a start tag is. */
enum {
  XML_ATTR_PLAIN = 0,       ///< No prefix, not a declaration.
  XML_ATTR_PREFIXED,        ///< `p:name`, p resolved.
  XML_ATTR_DEFAULT_NS_DECL, ///< `xmlns`.
  XML_ATTR_PREFIX_DECL      ///< `xmlns:p`.
};

typedef struct {
  uint32_t stamp; ///< Slot is live only when equal to the set's stamp.
  uint32_t index;
  uint32_t hash;
} xml_slot;

/** A set of attribute names that is empty again in O(1): see xml_hset_begin(). */
typedef struct {
  xml_slot * slots;
  size_t capacity; ///< A power of two, or 0.
  size_t count;
  uint32_t stamp;
} xml_hset;

struct GTEXT_XML_Stream;

typedef struct {
  const unsigned char * in;
  size_t length;
  size_t pos;
  size_t base; ///< Event parsing: the offset in the document of in[0], because
               ///< bytes already read are dropped. Always 0 for a DOM parse.
  size_t line;
  size_t column;
  uint32_t cursor_previous; ///< Previous decoded source character for CR pairs.

  size_t max_depth;
  size_t max_attributes;
  size_t max_attribute_bytes;
  size_t max_name_bytes;
  size_t max_namespaces;
  size_t max_source_depth;
  size_t max_external_source_bytes;
  size_t max_external_bytes;
  size_t max_expanded_bytes;
  size_t max_entity_depth;
  size_t max_references;
  size_t max_declarations;
  size_t max_total_work;
  size_t max_validation_work;
  size_t max_validation_memory;
  size_t max_validation_diagnostics;
  size_t external_bytes;
  size_t expanded_bytes;
  size_t references;
  size_t declarations;
  size_t total_work;
  size_t validation_work;
  size_t validation_memory;
  size_t validation_content_decls_bytes;
  size_t validation_frames_bytes;
  size_t validation_diagnostic_count;
  unsigned char validation_diagnostics_truncated;
  xml_content_decl * content_decls;
  size_t content_decl_count;
  size_t content_decl_capacity;
  xml_validation_frame * validation_frames;
  size_t validation_frame_count;
  size_t validation_frame_capacity;
  unsigned char * validation_scratch;
  size_t validation_scratch_bytes;
  size_t * validation_worklist;
  size_t validation_worklist_capacity;
  size_t validation_worklist_bytes;
  char * validation_doctype_name;
  size_t validation_doctype_name_length;
  char ** validation_ids;
  size_t * validation_id_lengths;
  size_t validation_id_count;
  size_t validation_id_capacity;
  char ** validation_idrefs;
  size_t * validation_idref_lengths;
  GTEXT_Markup_Location * validation_idref_locations;
  size_t validation_idref_count;
  size_t validation_idref_capacity;
  size_t entity_depth;

  GTEXT_Markup_Document * doc;       ///< NULL when parsing events.
  const GTEXT_Allocator * allocator; ///< The document's copy.
  xml_seams seams;
  GTEXT_XML_DTD_Mode dtd_mode;
  GTEXT_XML_DTD_External_Policy dtd_external_policy;
  GTEXT_Markup_Standalone standalone;
  unsigned char dtd_unread_parameter_entity;
  unsigned char dtd_in_markup_declaration;
  unsigned char dtd_incomplete_declaration;
  unsigned char dtd_suppress_declaration_effects;
  unsigned char dtd_knowledge_incomplete;
  GTEXT_XML_External_Access external_access;
  GTEXT_XML_Resolver_cb resolver;
  void * resolver_user_data;
  const char * base_uri;
  unsigned int resolver_callback_active;
  unsigned int resolver_reentered;

  /** Non-NULL when the parse delivers events (xml_stream.c) instead of building
   *  a tree: every place the tree parser would make a node calls an xml_emit_*
   *  function instead. */
  struct GTEXT_XML_Stream * stream;
  GTEXT_Markup_Location tag_end_at; ///< Where `/>` of an empty element was.
  markup_buf names;                 ///< Event parsing: the names of the open
                                    ///< elements, one after another.

  GTEXT_XML_Error err;

  /** Event parsing, before the end of the input: the buffer may end inside a
   *  token, so the lexer reports "the input ended" (UNEXPECTED_EOF), and not a
   *  syntax error, where more input could still make it well-formed. Always 0
   *  for a DOM parse, where the end of the buffer is the end of the document. */
  int partial;

  xml_family enc_family; ///< What the document was found to be in.
  xml_version version;   ///< Effective profile; defaults to XML 1.0.
  int enc_bom;           ///< It began with a byte order mark.
  markup_buf decoded;    ///< The UTF-8 form of converted input; `in` points here.

  markup_buf text;        ///< Character data being gathered.
  GTEXT_Markup_Location text_where;
  markup_buf attr_pool;   ///< Attribute values of the current start tag.
  markup_buf scratch;     ///< NUL-terminated copies of names for the tree.

  xml_attr * attrs;
  size_t attr_count;
  size_t attr_capacity;
  xml_hset raw_names;
  xml_hset expanded_names;

  xml_frame * frames;
  size_t frame_count;
  size_t frame_capacity;

  xml_binding * bindings;
  size_t binding_count;
  size_t binding_capacity;

  xml_source_frame * sources;
  size_t source_count;
  size_t source_capacity;
  xml_parameter_entity * parameter_entities;
  size_t parameter_entity_count;
  size_t parameter_entity_capacity;
  xml_general_entity * general_entities;
  size_t general_entity_count;
  size_t general_entity_capacity;
  xml_general_entity * attribute_entity_stack[64];
  size_t attribute_entity_depth;
  xml_dtd_binding * dtd_bindings;
  size_t dtd_binding_count;
  size_t dtd_binding_capacity;
  const char * dtd_generated_parameter_entity;
  unsigned char * dtd_external_modes;
  size_t dtd_external_depth;
  size_t dtd_external_capacity;
  size_t dtd_external_entry_source_count;
  unsigned char dtd_external_active;
  xml_doctype_continuation doctype_continuation;
  unsigned char dtd_yielded;
  unsigned char * dtd_name_token;
  size_t dtd_name_token_length;
  size_t dtd_name_token_capacity;
  uint32_t next_source_id;
  char ** source_names; ///< Owned labels retained through parser/reader lifetime.
  size_t source_name_count;
  size_t source_name_capacity;
} xml_ctx;

/**
 * Decide the encoding from the first bytes (and, for a document with no mark
 * that declares ISO-8859-1 or US-ASCII, the declaration), and convert the input
 * to UTF-8 if it is not already: `in`/`length` then name the converted copy.
 * A UTF-8 mark is skipped by moving `pos`.
 */
GTEXT_XML_Status xml_detect_encoding(xml_ctx * p);
/**
 * What the first `n` bytes say, as xml_detect_encoding() decides it: the family,
 * how many leading bytes are a byte order mark, and whether there was one.
 * Returns non-zero for EBCDIC, which is refused. Pure: it looks at the bytes and
 * nothing else, so a document that arrives in pieces is decided by the same code.
 */
int xml_sniff(const unsigned char * s, size_t n, xml_family * family,
    size_t * skip, int * bom);
/**
 * Are `n` bytes too few to decide? Fewer than four, or a declaration that may
 * still say ISO-8859-1 and has not ended (`*has_close` tells whether a '>'
 * has been seen).
 */
int xml_sniff_needs_more(const unsigned char * s, size_t n, int * has_close);

/** Conversion to UTF-8, in pieces: see xml_encoding.c. */
typedef struct {
  xml_ctx * p;
  markup_buf * out;
  xml_family family;
  size_t total;        ///< Bytes of UTF-8 produced so far.
  size_t raw_total;    ///< Original encoded bytes accepted so far.
  size_t raw_unit_offset; ///< Start of the current encoded unit.
  size_t high_raw_offset; ///< Start of a pending UTF-16 high surrogate.
  size_t line;
  size_t column;
  uint32_t previous;
  xml_version version; ///< Declared profile, when the XML declaration is present.
  unsigned char left[4]; ///< A partial code unit carried from the last piece.
  size_t left_n;
  uint32_t high;       ///< A high surrogate waiting for its low one.
} xml_converter;
void xml_converter_init(
    xml_converter * c, xml_ctx * p, markup_buf * out, xml_family family);
/** Convert a piece, appending to `out`. `final` says no more will come, so a
 *  partial unit is an error. Errors are recorded in `p->err`. */
GTEXT_XML_Status xml_converter_feed(xml_converter * c,
    const unsigned char * data, size_t n, int final);

/** Classify an encoding label, case-insensitively. */
xml_label xml_classify_label(const unsigned char * label, size_t length);
/**
 * Check the declaration's label against the family found. Mismatches the
 * specification makes fatal are GTEXT_XML_E_ENCODING_MISMATCH, any other
 * unsupported label is GTEXT_XML_E_UNSUPPORTED_ENCODING; both at `where`.
 */
GTEXT_XML_Status xml_check_label(xml_ctx * p, const unsigned char * label,
    size_t length, const GTEXT_Markup_Location * where);
/** The name a document gets when it declares none. */
const char * xml_family_name(xml_family family);

/* ---- Errors ------------------------------------------------------------- */

/** Record a failure at `where` (the first one wins) and return `code`. */
GTEXT_XML_Status xml_fail_at(xml_ctx * p, GTEXT_XML_Status code,
    const char * message, const GTEXT_Markup_Location * where);
/** As xml_fail_at() at the cursor. */
GTEXT_XML_Status xml_fail(
    xml_ctx * p, GTEXT_XML_Status code, const char * message);
/**
 * "This is not what the grammar allows here." Chooses the more specific code
 * when the character at the cursor is the real trouble: end of input, a byte
 * sequence that is not UTF-8, a character XML forbids. Otherwise E_SYNTAX with
 * `message`.
 */
GTEXT_XML_Status xml_unexpected(xml_ctx * p, const char * message);
/** The location of the cursor. */
GTEXT_Markup_Location xml_here(const xml_ctx * p);
/** Map a markup status from the tree to the parser's. */
GTEXT_XML_Status xml_from_markup(xml_ctx * p, GTEXT_Markup_Status status);
/** Charge a checked counter before work or allocation; zero selects defaults upstream. */
GTEXT_XML_Status xml_charge(xml_ctx * p, size_t * counter, size_t limit,
    size_t amount, GTEXT_XML_Status code, const char * message);
GTEXT_XML_Status xml_charge_work(xml_ctx * p, size_t amount);

/* ---- Cursor and characters (xml_lexer.c) -------------------------------- */

/** Move the cursor over `n` bytes that have already been validated. */
GTEXT_XML_Status xml_consume(xml_ctx * p, size_t n);
#define XML_CONSUME_OR_RETURN(p, n) do { \
  GTEXT_XML_Status xml_consume_status__ = xml_consume((p), (n)); \
  if (xml_consume_status__ != GTEXT_XML_OK) return xml_consume_status__; \
} while (0)

/** Initialize, push, or pop one physical source on the bounded heap stack. */
GTEXT_XML_Status xml_source_init(xml_ctx * p, const void * identity);
GTEXT_XML_Status xml_source_push(xml_ctx * p, xml_source_kind kind,
    const void * identity, const char * name, size_t name_length,
    const void * bytes, size_t length);
/** Charge another chunk read from the active external source. */
GTEXT_XML_Status xml_source_charge_external(xml_ctx * p, size_t amount);
GTEXT_XML_Status xml_source_pop(xml_ctx * p);
void xml_source_free(xml_ctx * p);
/** Acquire and account for one explicitly authorized external subset. The
 *  current story establishes source ownership; DTD grammar is handled by the
 *  external-subset parser story. */
GTEXT_XML_Status xml_resolve_external_subset(xml_ctx * p,
    GTEXT_Markup_Node * doctype, const char * public_id, const char * system_id);
GTEXT_XML_Status xml_resolve_external_resource(xml_ctx * p,
    GTEXT_XML_Resource_Kind resource_kind, xml_source_kind source_kind,
    const char * source_name, const char * public_id, const char * system_id,
    const char * declaration_base_uri, xml_resolved_resource * out);
GTEXT_XML_Status xml_finish_external_resource(xml_ctx * p,
    xml_resolved_resource * resource, GTEXT_XML_Status status);
/** Parse a decoded external subset, whose source frame is currently active. */
GTEXT_XML_Status xml_parse_external_subset(xml_ctx * p,
    GTEXT_Markup_Node * doctype, const unsigned char * bytes, size_t length,
    const char * base_uri);
GTEXT_XML_Status xml_resume_external_subset(
    xml_ctx * p, GTEXT_Markup_Node * doctype);
GTEXT_XML_Status xml_activate_external_parameter_entity(xml_ctx * p,
    xml_resolved_resource * resource);
int xml_stream_resolver_callback_active(const struct GTEXT_XML_Stream * stream);
void xml_stream_note_resolver_reentry(struct GTEXT_XML_Stream * stream);
void xml_stream_request_pause(struct GTEXT_XML_Stream * stream);
GTEXT_XML_Status xml_stream_resume(struct GTEXT_XML_Stream * stream,
    GTEXT_XML_Error * err);
GTEXT_INTERNAL_API size_t xml_reader_queue_count(
    const struct GTEXT_XML_Reader * reader);
GTEXT_INTERNAL_API size_t xml_reader_queue_bytes(
    const struct GTEXT_XML_Reader * reader);

/** Result of decoding one character. */
typedef enum {
  XML_CHAR_OK = 0,
  XML_CHAR_EOF,
  XML_CHAR_BAD_UTF8,
  XML_CHAR_FORBIDDEN, ///< UTF-8 is fine; XML's Char production excludes it.
  XML_CHAR_RESTRICTED ///< XML 1.1 Char, but forbidden as a literal.
} xml_char_status;

/** Decode and classify the character at `at`. */
xml_char_status xml_decode(
    const xml_ctx * p, size_t at, uint32_t * code_point, size_t * bytes);

/** XML whitespace: tab, line feed, carriage return, space. */
static inline int xml_is_space(unsigned char c) {
  return c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D;
}

/**
 * Is the input at the cursor a proper prefix of `literal` that the end of the
 * buffer cut short? Only when `partial`: then the answer to "is it `literal`?"
 * is "not yet known". Callers that would otherwise report a mismatch report the
 * end of the input instead.
 */
int xml_cut_literal(const xml_ctx * p, const char * literal, size_t length);
/**
 * Is `at` the end of what has arrived: past the last byte, or (when `partial`)
 * at the start of a character the end cut short?
 */
int xml_at_end(const xml_ctx * p, size_t at);

/** Skip whitespace; the number of bytes skipped. */
size_t xml_skip_space(xml_ctx * p);
/** Does the input at the cursor start with this? */
int xml_looking_at(const xml_ctx * p, const char * literal, size_t length);
/** Consume `literal` or fail with `message` (via xml_unexpected). */
GTEXT_XML_Status xml_expect(
    xml_ctx * p, const char * literal, size_t length, const char * message);

/**
 * Measure the Name at the cursor without consuming it. Fails if there is none
 * or it is longer than the limit.
 */
GTEXT_XML_Status xml_scan_name(xml_ctx * p, size_t * length);

/** Append `[from, to)` of the input to `buf`, line ends normalized to LF. */
GTEXT_XML_Status xml_append_normalized(
    xml_ctx * p, markup_buf * buf, size_t from, size_t to);
/** Byte length of one XML source line-end sequence at `at`, or zero. */
size_t xml_line_end_bytes(const xml_ctx * p, size_t at);
/** XML whitespace at `at`, including line ends normalized for the profile. */
size_t xml_space_bytes(const xml_ctx * p, size_t at);
/** Probe the first declaration pseudo-attribute to select XML 1.1 early. */
void xml_probe_xml11_declaration(xml_ctx * p);
/** XML 1.1 line-end sequence length, independent of the current profile. */
size_t xml_xml11_line_end_bytes(const xml_ctx * p, size_t at);
/** Append a code point as UTF-8. */
GTEXT_XML_Status xml_append_code_point(
    xml_ctx * p, markup_buf * buf, uint32_t code_point);
/** Append bytes, mapping the markup status to the parser's. */
GTEXT_XML_Status xml_append(
    xml_ctx * p, markup_buf * buf, const char * bytes, size_t length);

/**
 * Read a reference at the cursor (which must be on `&`) and append what it
 * stands for to `buf`. Errors are positioned at the `&`.
 */
GTEXT_XML_Status xml_parse_reference(xml_ctx * p, markup_buf * buf);

/**
 * Check a run of character data starting at the cursor and find where it ends:
 * at '<', at '&', or at the end of the input. Every character is validated and
 * "]]>" is refused. Leaves the cursor at the start of the run.
 */
GTEXT_XML_Status xml_scan_text_run(xml_ctx * p, size_t * end);

/** An allocation through the document's allocator; NULL on failure. */
void * xml_alloc(xml_ctx * p, size_t size);
void xml_release(xml_ctx * p, void * ptr);
/** Grow an array to hold at least `needed` elements. */
GTEXT_XML_Status xml_grow(xml_ctx * p, void ** array, size_t * capacity,
    size_t element_size, size_t needed);

/* ---- Names and sets ----------------------------------------------------- */

uint32_t xml_hash(const void * bytes, size_t length, uint32_t seed);
/** Empty the set, with room for `expected` members. */
GTEXT_XML_Status xml_hset_begin(xml_ctx * p, xml_hset * set, size_t expected);
/** Does member `existing` equal `key`? Called only when the hashes agree. */
typedef int (*xml_hset_same)(
    const xml_ctx * p, uint32_t existing, const void * key);
/**
 * Add `index` under `hash`. Returns 0 if added, 1 if an equal member (by
 * `same`) is already there, -1 on allocation failure.
 */
int xml_hset_insert(xml_ctx * p, xml_hset * set, uint32_t hash, uint32_t index,
    xml_hset_same same, const void * key);
void xml_hset_free(xml_ctx * p, xml_hset * set);

/* ---- Namespaces (xml_namespace.c) --------------------------------------- */

/** Open a scope: remember how many bindings are in effect. */
size_t xml_scope_mark(const xml_ctx * p);
/** Close it: drop every binding made since. */
void xml_scope_release(xml_ctx * p, size_t mark);
/**
 * Declare `prefix` (empty for the default namespace) as `uri`, enforcing the
 * constraints on the `xml` and `xmlns` prefixes and the limit. Errors are
 * positioned at `where`.
 */
GTEXT_XML_Status xml_scope_declare(xml_ctx * p, const char * prefix,
    size_t prefix_length, const char * uri, size_t uri_length,
    const GTEXT_Markup_Location * where);
/**
 * Resolve a prefix (length 0 for the default namespace) to a NUL-terminated
 * URI. Returns 1 and sets *uri to it, or to NULL for "no namespace"; returns 0
 * if the prefix is unbound. `xml` is always bound.
 */
int xml_scope_lookup(const xml_ctx * p, const char * prefix,
    size_t prefix_length, const char ** uri);
void xml_scope_free(xml_ctx * p);

/** Split a name at its colon and check both halves are NCNames. */
GTEXT_XML_Status xml_check_qname(xml_ctx * p, const unsigned char * name,
    size_t name_length, size_t * colon, const GTEXT_Markup_Location * where);

/* ---- Pieces the document type shares with the parser -------------------- */

/**
 * Parse the comment at the cursor (on `<!--`). With `keep`, it is delivered -
 * added to the tree as a child of the open element, or as an event; without,
 * it is only checked and moved over (the internal subset of a document type).
 */
GTEXT_XML_Status xml_parse_comment(xml_ctx * p, int keep);
/** As xml_parse_comment() for the processing instruction at `<?`. */
GTEXT_XML_Status xml_parse_pi(xml_ctx * p, int keep);
/** Set a node's location and link it as `parent`'s last child. */
void xml_attach(GTEXT_Markup_Node * parent, GTEXT_Markup_Node * node,
    const GTEXT_Markup_Location * where);

/* ---- Document type (xml_doctype.c) -------------------------------------- */

/** Parse `<!DOCTYPE ...>` at the cursor and add the doctype node. */
GTEXT_XML_Status xml_parse_doctype(xml_ctx * p);
void xml_doctype_continuation_reset(xml_ctx * p);
/** Rebuild the derived declaration indexes from retained doctype syntax. */
GTEXT_INTERNAL_API GTEXT_XML_Status xml_doctype_rebuild_indexes(
    xml_ctx * p, GTEXT_Markup_Node * doctype);

/* ---- Structural DTD validation (xml_validation.c) ----------------------- */
GTEXT_XML_Status xml_validation_add_element_decl(xml_ctx *p,
    const unsigned char *source, size_t length,
    const GTEXT_Markup_Location *location, int external);
GTEXT_XML_Status xml_validation_start_element(xml_ctx *p,
    const unsigned char *name, size_t name_length,
    const GTEXT_Markup_Location *location, int is_empty,
    const xml_attr *attrs, size_t attr_count, const markup_buf *attr_pool);
GTEXT_XML_Status xml_validation_end_element(xml_ctx *p,
    const unsigned char *name, size_t name_length,
    const GTEXT_Markup_Location *location);
GTEXT_XML_Status xml_validation_finish(xml_ctx *p);
GTEXT_XML_Status xml_validation_external_entity_reference(xml_ctx *p,
    const xml_general_entity *entity, const GTEXT_Markup_Location *where);
GTEXT_XML_Status xml_validation_text(xml_ctx *p, const char *text,
    size_t length, const GTEXT_Markup_Location *location,
    int cdata);
void xml_validation_cleanup(xml_ctx *p);

/* ---- Pieces the event parser drives (xml_parser.c) ---------------------- */

/** The XML declaration at the cursor (on `<?xml`). */
GTEXT_XML_Status xml_parse_xml_declaration(xml_ctx * p);
/** The start tag at the cursor (on `<`). *is_empty is set for `<a/>`. */
GTEXT_XML_Status xml_parse_start_tag(xml_ctx * p, int * is_empty);
/** The end tag at the cursor (on `</`). */
GTEXT_XML_Status xml_parse_end_tag(xml_ctx * p);
/** The CDATA section at the cursor (on `<![CDATA[`). */
GTEXT_XML_Status xml_parse_cdata(xml_ctx * p);
/** Hand over the character data gathered so far, if there is any. */
GTEXT_XML_Status xml_flush_text(xml_ctx * p);

/* ---- Events (xml_stream.c) ----------------------------------------------- */

/*
 * What the parser calls where the DOM parser makes a node. Each returns OK, or
 * the status that stops the parse: the callback's own, or a limit. None of them
 * keeps a pointer it is given.
 */
GTEXT_XML_Status xml_emit_document_start(xml_ctx * p, const char * version,
    const char * encoding, size_t encoding_length,
    GTEXT_Markup_Encoding_Source source, GTEXT_Markup_Standalone standalone);
GTEXT_XML_Status xml_emit_doctype(xml_ctx * p, const GTEXT_Markup_Location * at,
    const char * name, const char * public_id, const char * system_id);
/** The start tag just read: p->attrs and p->attr_pool hold its attributes,
 *  already resolved. */
GTEXT_XML_Status xml_emit_element_start(xml_ctx * p,
    const GTEXT_Markup_Location * at, const unsigned char * name,
    size_t name_length, size_t colon, const char * element_ns);
/** The end of the element whose name is given, at `at`, in scope. */
GTEXT_XML_Status xml_emit_element_end(xml_ctx * p,
    const GTEXT_Markup_Location * at, const unsigned char * name,
    size_t name_length);
GTEXT_XML_Status xml_emit_text(xml_ctx * p, const GTEXT_Markup_Location * at,
    const char * data, size_t length);
GTEXT_XML_Status xml_emit_cdata(xml_ctx * p, const GTEXT_Markup_Location * at,
    const char * data, size_t length);
GTEXT_XML_Status xml_emit_comment(xml_ctx * p, const GTEXT_Markup_Location * at,
    const char * data, size_t length);
GTEXT_XML_Status xml_emit_pi(xml_ctx * p, const GTEXT_Markup_Location * at,
    const char * target, size_t target_length, const char * data, size_t length);
GTEXT_XML_Status xml_emit_skipped_entity(xml_ctx * p,
    const GTEXT_Markup_Location * at, const char * name, size_t length);
GTEXT_XML_Status xml_emit_unread_resource(xml_ctx * p,
    const GTEXT_Markup_Location * at, GTEXT_XML_Resource_Kind kind,
    const char * name, const char * system_id);
GTEXT_XML_Status xml_emit_validation_diagnostic(xml_ctx *p,
    GTEXT_XML_Validation_Code code, const GTEXT_Markup_Location *where,
    const char *message);
GTEXT_XML_Status xml_emit_validation_diagnostics_truncated(xml_ctx *p,
    const GTEXT_Markup_Location *where);

/**
 * Make the event parser take an attempt at every construct whenever any input
 * arrives, instead of only when the scan for its end says it is complete. For
 * tests: it is quadratic, and it exists so that the lexer is shown to give the
 * DOM parse's verdict for a buffer that ends anywhere.
 */
GTEXT_INTERNAL_API size_t xml_stream_work(const struct GTEXT_XML_Stream * stream);
GTEXT_INTERNAL_API void xml_stream_set_eager(struct GTEXT_XML_Stream * stream, int eager);
GTEXT_INTERNAL_API int xml_stream_pause_pending(
    const struct GTEXT_XML_Stream * stream);

/* ---- Entry (xml_parser.c) ----------------------------------------------- */

/**
 * gtext_xml_parse() with the DTD seams filled in. For tests; the public entry
 * point passes none.
 */
GTEXT_INTERNAL_API GTEXT_XML_Status xml_parse_with_seams(const void * data,
    size_t len, const GTEXT_XML_Parse_Options * opts, const xml_seams * seams,
    GTEXT_Markup_Document ** out, GTEXT_XML_Error * err);

GTEXT_INTERNAL_API struct GTEXT_XML_Stream * xml_stream_new_with_seams(
    const GTEXT_XML_Parse_Options * options, GTEXT_XML_Event_cb callback,
    void * user_data, const xml_seams * seams);

/** Validate public mode options before an input source is consumed. */
GTEXT_INTERNAL_API GTEXT_XML_Status xml_validate_dtd_options(
    const GTEXT_XML_Parse_Options * options, GTEXT_XML_Error * err);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_XML_XML_INTERNAL_H
