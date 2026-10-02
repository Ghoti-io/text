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
 * @brief Internal header for YAML implementation.
 */

#ifndef GHOTI_IO_GTEXT_SRC_YAML_YAML_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_YAML_YAML_INTERNAL_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>

#include <stdlib.h>
#include <stdint.h>

#include <ghoti.io/text/yaml/yaml_core.h>

#include <ghoti.io/chron/chron.h>

/* Forward declarations */
typedef struct GTEXT_YAML_Stream GTEXT_YAML_Stream;

/* True when @p suffix names one of the types the "tag:yaml.org,2002:"
   namespace defines.  That namespace is not the author's to extend, so a tag
   in it naming anything else is a malformed document - which the resolver
   refuses on the way in and the writer refuses on the way out. */
GTEXT_INTERNAL_API bool gtext_yaml_tag_is_defined_standard(const char *suffix);

/* Whether @p entry may be added to the !!omap @p omap.
   
   An omap is an ordered mapping: 10.x's !!omap takes a sequence of
   single-pair mappings whose keys are unique, and the resolver refuses a
   parsed one that breaks either rule.  The DOM appenders ask this so that
   they refuse the same thing, using the same comparison - a node the
   constructors accept and the parser will not read back is a node nothing
   can write.  Pass NULL for @p omap to ask only about the entry's shape. */
GTEXT_INTERNAL_API bool gtext_yaml_omap_can_take(
	const GTEXT_YAML_Node *omap,
	const GTEXT_YAML_Node *entry
);

/* A set of DOM nodes keyed on the pointer, for a walk that has to know where
   it has already been.  "Seen before" is the only question asked of it and
   there is no deletion, so an open-addressed table of pointers is the whole
   implementation; see yaml_node_set.c.

   Zero-initialised is a valid empty set, and so is the result of
   gtext_yaml_node_set_init(); either way it allocates on first insert. */
typedef struct {
	const GTEXT_YAML_Node **slots;
	size_t capacity;   /* a power of two, or 0 while nothing is allocated */
	size_t count;
	const GTEXT_Allocator *alloc;
} GTEXT_YAML_Node_Set;

GTEXT_INTERNAL_API void gtext_yaml_node_set_init(
	GTEXT_YAML_Node_Set *set,
	const GTEXT_Allocator *alloc
);

/* True when @p n was added, false when it was already there.  @p oom - which
   may be NULL - is set when the table could not grow, which is the one case a
   caller must not read as "already seen". */
GTEXT_INTERNAL_API bool gtext_yaml_node_set_add(
	GTEXT_YAML_Node_Set *set,
	const GTEXT_YAML_Node *n,
	bool *oom
);

GTEXT_INTERNAL_API bool gtext_yaml_node_set_has(
	const GTEXT_YAML_Node_Set *set,
	const GTEXT_YAML_Node *n
);

GTEXT_INTERNAL_API void gtext_yaml_node_set_free(GTEXT_YAML_Node_Set *set);

/* True when @p value, written as a plain scalar, would resolve to something
   other than a string under the 1.2 core schema.  The writer asks so that a
   string node whose text spells a number or a null goes out in quotes rather
   than coming back as the number. */
GTEXT_INTERNAL_API bool gtext_yaml_plain_text_resolves_to_non_string(
	const char *value,
	size_t len
);

/* The same question asked of a named dialect rather than of the 1.2 core
   schema.  Which texts resolve is what a schema and a version *are*, so the
   writer has to ask about the one its output is meant to be read back in:
   "yes" is a bool in 1.1 and a string in 1.2, so the string "yes" needs
   quotes for one reader and not for the other. */
GTEXT_INTERNAL_API bool gtext_yaml_plain_text_resolves_to_non_string_as(
	const char *value,
	size_t len,
	GTEXT_YAML_Schema schema,
	bool yaml_1_1
);

/* A plain spelling @p schema resolves to null.  "~" is one for the core
   schema and for 1.1; the JSON schema knows only the word, and the failsafe
   schema knows neither - a null cannot survive that one at all. */
GTEXT_INTERNAL_API const char *gtext_yaml_null_spelling_for(
	GTEXT_YAML_Schema schema
);

/* The type @p value would resolve to if it were written as a plain scalar
   under the 1.2 core schema.  The DOM constructors use it so that a node
   built from text reports the same type a parsed one would. */
GTEXT_INTERNAL_API GTEXT_YAML_Node_Type gtext_yaml_plain_text_type(
	const char *value,
	size_t len
);

/* The same, and the value that goes with the type: a node that says it is an
   integer has to hold one, or gtext_yaml_node_as_int() and every conversion
   built on it answer with whatever the union was left at.  Out parameters may
   be NULL. */
GTEXT_INTERNAL_API GTEXT_YAML_Node_Type gtext_yaml_plain_text_classify(
	const char *value,
	size_t len,
	bool *bool_out,
	int64_t *int_out,
	double *float_out,
	const GTEXT_Allocator *alloc
);

/* The same, under a named schema and version.  The one above is this with the
   1.2 core schema, rather than a second copy of the rows. */
GTEXT_INTERNAL_API GTEXT_YAML_Node_Type gtext_yaml_plain_text_classify_as(
	const char *value,
	size_t len,
	GTEXT_YAML_Schema schema,
	bool yaml_1_1,
	bool *bool_out,
	int64_t *int_out,
	double *float_out,
	/* The scratch buffer the numeric probes use. The writer passes NULL - it
	   is a separate entry point with no allocator, as in JSON and CSV. */
	const GTEXT_Allocator *alloc
);

/* Whether converting @p f to int64_t has a defined answer.
 *
 * It has none for a NaN, for either infinity, or for anything that truncates
 * past the type's range: 6.3.1.4 leaves all of those undefined, and on
 * x86-64 what the hardware does is hand back INT64_MIN.  Every place that
 * turns a double into an integer asks this first.
 *
 * Shared rather than written twice, because the two callers had already
 * drifted: one was a UBSan report and the other was quietly returning
 * INT64_MIN. */
GTEXT_INTERNAL_API bool gtext_yaml_double_fits_int64(double f);

GTEXT_INTERNAL_API GTEXT_YAML_Parse_Options gtext_yaml_parse_options_effective(
	const GTEXT_YAML_Parse_Options *opts
);

/* Internal structures for the scanner, reader, and parser. */

typedef struct GTEXT_YAML_CharReader GTEXT_YAML_CharReader;

GTEXT_INTERNAL_API GTEXT_YAML_CharReader *gtext_yaml_char_reader_new(
	const char *data,
	size_t len,
	const GTEXT_Allocator *alloc
);
GTEXT_INTERNAL_API void gtext_yaml_char_reader_free(GTEXT_YAML_CharReader *r);
GTEXT_INTERNAL_API int gtext_yaml_char_reader_peek(GTEXT_YAML_CharReader *r);
GTEXT_INTERNAL_API int gtext_yaml_char_reader_consume(GTEXT_YAML_CharReader *r);
GTEXT_INTERNAL_API size_t gtext_yaml_char_reader_offset(
	const GTEXT_YAML_CharReader *r
);
GTEXT_INTERNAL_API void gtext_yaml_char_reader_position(
	const GTEXT_YAML_CharReader *r,
	int *line,
	int *col
);

/* UTF-8 validation and scalar buffer utilities (Task 2.2) */

/* Return 1 if the buffer of length `len` is valid UTF-8, 0 otherwise. */
GTEXT_INTERNAL_API int gtext_utf8_validate(const char *buf, size_t len);

/* Simple dynamic buffer used to assemble scalars that may cross input chunks. */
typedef struct {
	char *data;
	size_t len;
	size_t cap;
	/* The allocator `data` came from. A parameter of the initialiser rather
	   than something a caller assigns afterwards: dynbuf_grow() reaches it
	   from here without a signature change, and there is no window in which
	   the buffer exists without knowing where its memory comes from. */
	const GTEXT_Allocator *alloc;
} GTEXT_YAML_DynBuf;

/**
 * @brief strdup through an allocator.
 *
 * Exists because strdup() is invisible to the eye and was invisible to
 * make check-allocators as well: the gate matched malloc, calloc, realloc and
 * free, so fifteen strdup() calls in the parser and the stream kept allocating
 * from the C library while the frees beside them were converted. That is a free
 * through the wrong allocator, which is heap corruption - caught by the
 * tracking allocator's guard word rather than by review. The gate matches
 * strdup and strndup now too.
 *
 * NULL in gives NULL out, which is what every call site here wants.
 */
GTEXT_INTERNAL_API char *gtext_yaml_strdup(const char *s, const GTEXT_Allocator *alloc);

GTEXT_INTERNAL_API int gtext_yaml_dynbuf_init(GTEXT_YAML_DynBuf *b, const GTEXT_Allocator *alloc);
GTEXT_INTERNAL_API void gtext_yaml_dynbuf_free(GTEXT_YAML_DynBuf *b);
GTEXT_INTERNAL_API int gtext_yaml_dynbuf_append(GTEXT_YAML_DynBuf *b, const char *data, size_t len);

/* Minimal scanner/tokenizer (Task 2.3) */
typedef enum {
	GTEXT_YAML_TOKEN_INDICATOR,
	GTEXT_YAML_TOKEN_SCALAR,
	GTEXT_YAML_TOKEN_COMMENT,
	GTEXT_YAML_TOKEN_DIRECTIVE,
	GTEXT_YAML_TOKEN_DOCUMENT_START,  /* "---" */
	GTEXT_YAML_TOKEN_DOCUMENT_END,    /* "..." */
	GTEXT_YAML_TOKEN_EOF,
	GTEXT_YAML_TOKEN_ERROR
} GTEXT_YAML_Token_Type;

typedef struct {
	GTEXT_YAML_Token_Type type;
	/* For INDICATOR: 1-byte char in `c`.
		 For SCALAR and COMMENT: pointer/length into a buffer owned by the
		 scanner. It stays valid until the next gtext_yaml_scanner_next() call
		 on the same scanner, and is released then; a consumer that needs the
		 text for longer must copy it. Consumers must not free it.
	*/
	union {
		char c;
		struct { const char *ptr; size_t len; } scalar;
		struct { const char *ptr; size_t len; bool inline_comment; } comment;
	} u;
	GTEXT_YAML_Scalar_Style scalar_style; /* For scalar tokens */
	size_t offset; /* byte offset where token begins */
	int line, col;  /* position */
} GTEXT_YAML_Token;

typedef struct GTEXT_YAML_Scanner GTEXT_YAML_Scanner;

/* Streaming scanner API: create empty scanner, feed chunks, mark finish. */
GTEXT_INTERNAL_API GTEXT_YAML_Scanner *gtext_yaml_scanner_new(const GTEXT_Allocator *alloc);
GTEXT_INTERNAL_API void gtext_yaml_scanner_free(GTEXT_YAML_Scanner *s);
GTEXT_INTERNAL_API int gtext_yaml_scanner_feed(GTEXT_YAML_Scanner *s, const char *data, size_t len);
GTEXT_INTERNAL_API void gtext_yaml_scanner_finish(GTEXT_YAML_Scanner *s);
GTEXT_INTERNAL_API GTEXT_YAML_Status gtext_yaml_scanner_next(GTEXT_YAML_Scanner *s, GTEXT_YAML_Token *tok, GTEXT_YAML_Error *err);

/**
 * @brief Keep the decoded input rather than dropping it as it is consumed.
 *
 * Call before the first feed. Every offset the scanner reports counts bytes
 * of the *decoded* stream, so that stream is the only text those offsets can
 * be used to index - and by default the scanner drops its consumed prefix,
 * which leaves nothing at the front to index into. Retaining costs the
 * document's length in memory, which is why it is asked for rather than
 * assumed.
 */
GTEXT_INTERNAL_API void gtext_yaml_scanner_retain_decoded(GTEXT_YAML_Scanner *s);

/**
 * @brief The decoded stream, when it is being retained.
 *
 * False - and nothing written - when it is not. The pointer is the scanner's
 * own buffer and moves when it grows, so it is fetched where it is used
 * rather than kept.
 */
GTEXT_INTERNAL_API bool gtext_yaml_scanner_decoded(const GTEXT_YAML_Scanner *s, const char **data, size_t *len);

/* ====================================================================
 * Arena Allocator and Context (Phase 4)
 * ==================================================================== */

/**
 * @brief Arena block structure
 *
 * Each block contains a chunk of memory that can be allocated from.
 * Blocks are linked together to form the arena.
 *
 * WARNING: This struct uses a flexible array member pattern and must
 * always be used as a pointer. Never copy or pass by value.
 */
typedef struct yaml_arena_block {
	struct yaml_arena_block *next;  /* Next block in chain */
	size_t used;                    /* Bytes used in this block */
	size_t size;                    /* Total size of this block */
	char data[1];                   /* Flexible array member (C99 FAM) */
} yaml_arena_block;

/**
 * @brief Arena allocator structure
 *
 * Manages a collection of blocks for efficient bulk allocation.
 * All memory is freed when the arena is destroyed.
 */
typedef struct yaml_arena {
	yaml_arena_block *first;    /* First block in chain */
	yaml_arena_block *current;  /* Current allocation block */
	/* The allocator every block came from, and the one they go back to.
	   Held here rather than only on the context because yaml_arena_free()
	   releases the structure it was read from, so it has to be captured
	   before the loop - and because freeing a block through the wrong
	   allocator corrupts the heap rather than leaking. */
	const GTEXT_Allocator *alloc;
	size_t block_size;          /* Size of new blocks (grows exponentially) */
} yaml_arena;

/**
 * @brief YAML context structure
 *
 * Owns the arena allocator and tracks document-level information.
 * All nodes in a document share the same context.
 */
typedef struct yaml_context {
	yaml_arena *arena;              /* Arena allocator */
	/* The decoded character stream, which is what every offset the scanner
	   and the stream report counts bytes of - not the bytes the caller
	   handed in. Those are the same thing only for UTF-8 with no byte order
	   mark, and this field used to hold the caller's buffer, so every
	   positional question the parser asks was answered from the wrong text
	   whenever they differed: a mark, or any of the UTF-16 and UTF-32
	   encodings, and a block mapping with two entries did not parse at all.

	   Borrowed from the scanner, which owns it and moves it as it grows, so
	   it is refreshed as each event arrives rather than set once. NULL until
	   the first one, and the helpers that read it all answer for themselves
	   when it is. */
	const char *decoded_input;
	size_t decoded_input_len;

	/* Where the line holding the last offset anyone asked about begins.
	 *
	 * Five helpers in the parser need "which line is this offset on", and all
	 * five found out by walking backwards to the nearest break. That is
	 * O(line length) a call, and a deeply nested flow document is *one* line:
	 * parsing 20000 nested sequences walked 199,990,000 bytes backwards, in
	 * 19999 calls that each re-covered ground the last one had.
	 *
	 * The parser asks about offsets that mostly move forwards, so the answer
	 * is kept and extended instead. line_cache_upto is how far it has been
	 * verified: no break lies in [line_cache_start, line_cache_upto), so any
	 * offset in that span is on the line starting at line_cache_start. A
	 * question past it scans forward from there rather than back from the
	 * offset, which makes the whole parse O(n) instead of O(n^2); a question
	 * before it falls back to the walk.
	 *
	 * line_cache_buffer is which buffer the other two describe. The scanner
	 * owns the input and moves it as it grows, so a moved buffer drops the
	 * cache rather than being trusted. */
	const char *line_cache_buffer;
	size_t line_cache_start;
	size_t line_cache_upto;

	size_t node_count;              /* Total nodes allocated (statistics) */

	/* The caller's allocator, or NULL for gtext_allocator_default(). Every
	   allocation a document owns is reachable from here. */
	const GTEXT_Allocator *alloc;
} yaml_context;

/* Arena API */
GTEXT_INTERNAL_API yaml_arena *yaml_arena_new(const GTEXT_Allocator *alloc);
GTEXT_INTERNAL_API void yaml_arena_free(yaml_arena *arena);
GTEXT_INTERNAL_API void *yaml_arena_alloc(yaml_arena *arena, size_t size, size_t align);

/* Context API */
GTEXT_INTERNAL_API yaml_context *yaml_context_new(const GTEXT_Allocator *alloc);
GTEXT_INTERNAL_API void yaml_context_free(yaml_context *ctx);
GTEXT_INTERNAL_API void yaml_context_set_decoded_input(yaml_context *ctx, const char *buf, size_t len);

/**
 * @brief Where the line containing @p offset begins.
 *
 * Amortised O(1) over offsets that move forwards, which is how the parser
 * asks. Returns 0 when there is no input to answer about.
 *
 * @param ctx Context holding the decoded input (may be NULL)
 * @param offset Byte offset into that input
 * @return Offset of the first byte of that offset's line
 */
GTEXT_INTERNAL_API size_t yaml_context_line_start(yaml_context *ctx, size_t offset);
GTEXT_INTERNAL_API void *yaml_context_alloc(yaml_context *ctx, size_t size, size_t align);

/**
 * @brief Report a non-fatal issue, honoring the mask and warnings_as_errors.
 *
 * Shared because the parser has one of its own to report - a "%YAML" naming
 * a minor version it does not implement - and the resolver's copy already
 * had the mask, the callback and the promote-to-error rule right.
 *
 * Returns GTEXT_YAML_E_INVALID when opts->warnings_as_errors is set, having
 * filled in @p error; GTEXT_YAML_OK otherwise, masked or not.
 */
GTEXT_INTERNAL_API GTEXT_YAML_Status gtext_yaml_emit_warning(
	const GTEXT_YAML_Parse_Options *opts,
	GTEXT_YAML_Warning_Code code,
	const char *message,
	GTEXT_YAML_Error *error
);

/* ============================================================================
 * Phase 4: DOM Node Structures
 * ============================================================================ */

/**
 * @brief Internal DOM node representation.
 *
 * All DOM nodes are allocated from the context arena. Scalars store their
 * content as strings (type resolution is deferred to Phase 5). Collections
 * use flexible array members for children/pairs.
 *
 * Design decisions:
 * - All scalars are strings initially (no int/float/bool until Phase 5)
 * - Sequences store child pointers in flexible array
 * - Mappings store key-value pairs in flexible array
 * - Anchors stored as strings (resolver tracks identity)
 * - Tags stored as strings (resolver handles semantics)
 */

/* Scalar node (all scalars are strings until Phase 5 type resolution) */
typedef struct {
	GTEXT_YAML_Node_Type type;  /* GTEXT_YAML_STRING initially */
	const char *value;          /* Null-terminated string (arena-allocated) */
	size_t length;              /* Length excluding null terminator */
	const char *leading_comment; /* Optional leading comment */
	const char *inline_comment;  /* Optional inline comment */
	size_t source_offset;       /* Byte offset for node start */
	int source_line;            /* 1-based line number */
	int source_col;             /* 1-based column number */
	GTEXT_YAML_Scalar_Style scalar_style; /* Preferred scalar style */
	bool bool_value;            /* Parsed boolean value */
	int64_t int_value;          /* Parsed integer value */
	double float_value;         /* Parsed floating-point value */
	bool has_timestamp;         /* True if timestamp was parsed */
	/* The timestamp itself, as chron read it.  This was twelve loose ints
	   and four flags, parsed by a hundred lines in yaml_resolve.c that had
	   never been held against another implementation and were off-spec in
	   five ways.  Time is not this library's business. */
	GCHRON_YamlValue timestamp;
	/* The text said `:60`.  The value above says `:59`, where the kernel puts
	   the repeated second; this is the evidence that it did not have to, and
	   the reason the scalar is left as it was written. */
	bool timestamp_leap_second;
	bool has_binary;            /* True if binary data was parsed */
	const unsigned char *binary_data; /* Binary payload (arena-allocated) */
	size_t binary_len;          /* Length of binary payload */
	const char *tag;            /* Optional tag (e.g., "!!str"), NULL if none */
	const char *anchor;         /* Optional anchor name, NULL if none */
} yaml_node_scalar;

/* Sequence node (array of child nodes) */
typedef struct {
	GTEXT_YAML_Node_Type type;  /* GTEXT_YAML_SEQUENCE */
	/* How the collection was written: FLOW for "[a, b]", BLOCK for "- a",
	   AUTO for a node that was built rather than parsed.  The two spellings
	   mean the same thing to a reader of the value and are not the same
	   document, so a writer that has to reproduce the input - or a caller
	   asking what it was given - needs the distinction kept. */
	GTEXT_YAML_Flow_Style flow_style;
	const char *tag;            /* Optional tag, NULL if none */
	const char *anchor;         /* Optional anchor name, NULL if none */
	const char *leading_comment; /* Optional leading comment */
	const char *inline_comment;  /* Optional inline comment */
	size_t source_offset;       /* Byte offset for node start */
	int source_line;            /* 1-based line number */
	int source_col;             /* 1-based column number */
	size_t count;               /* Number of children */
	GTEXT_YAML_Node *children[1]; /* Flexible array member */
} yaml_node_sequence;

/* Mapping node (array of key-value pairs) */
typedef struct {
	const char *key_tag;        /* Optional tag for key */
	const char *value_tag;      /* Optional tag for value */
	GTEXT_YAML_Node *key;       /* Key node (can be any type per YAML spec) */
	GTEXT_YAML_Node *value;     /* Value node */
} yaml_mapping_pair;

typedef struct {
	const char *handle;
	const char *prefix;
} yaml_tag_handle;

typedef struct {
	GTEXT_YAML_Node_Type type;  /* GTEXT_YAML_MAPPING */
	GTEXT_YAML_Flow_Style flow_style; /* As for a sequence, above. */
	const char *tag;            /* Optional tag, NULL if none */
	const char *anchor;         /* Optional anchor name, NULL if none */
	const char *leading_comment; /* Optional leading comment */
	const char *inline_comment;  /* Optional inline comment */
	size_t source_offset;       /* Byte offset for node start */
	int source_line;            /* 1-based line number */
	int source_col;             /* 1-based column number */
	size_t count;               /* Number of key-value pairs */
	yaml_mapping_pair pairs[1]; /* Flexible array member */
} yaml_node_mapping;

/* Alias node (references another node by anchor name) */
typedef struct {
	GTEXT_YAML_Node_Type type;  /* GTEXT_YAML_ALIAS */
	const char *anchor_name;    /* Name of the referenced anchor (arena-allocated) */
	GTEXT_YAML_Node *target;    /* Resolved target node (NULL until resolved) */
	const char *leading_comment; /* Optional leading comment */
	const char *inline_comment;  /* Optional inline comment */
	size_t source_offset;       /* Byte offset for node start */
	int source_line;            /* 1-based line number */
	int source_col;             /* 1-based column number */
} yaml_node_alias;

/* Union node type (public type is opaque pointer to this) */
struct GTEXT_YAML_Node {
	GTEXT_YAML_Node_Type type;
	union {
		yaml_node_scalar scalar;
		yaml_node_sequence sequence;
		yaml_node_mapping mapping;
		yaml_node_alias alias;
		/* Note: We use the full structs in the union to avoid pointer chasing,
		 * but this means the union size is determined by the largest member.
		 * For now, we accept this tradeoff for simpler allocation. */
	} as;
};

/* Document structure (root + metadata) */
struct GTEXT_YAML_Document {
	yaml_context *ctx;          /* Owns the arena */
	GTEXT_YAML_Node *root;      /* Root node of the document */
	GTEXT_YAML_Parse_Options options; /* Parse options used */
	size_t node_count;          /* Total nodes allocated (statistics) */
	size_t document_index;      /* Index in multi-document stream (0-based) */
	bool has_directives;        /* True if %YAML or %TAG directives present */
	/* Whether "---" opened this document and "..." closed it.  Every document
	   has a start and an end; only some of them were written down, and the
	   difference is not recoverable from the tree. */
	bool explicit_start;
	bool explicit_end;
	bool has_merge_keys;        /* True if merge keys (<<) were used */
	int yaml_version_major;     /* YAML version from %YAML directive (0 if none) */
	int yaml_version_minor;
	const char *input_newline;  /* Detected line ending for input (if any) */
	yaml_tag_handle *tag_handles; /* Array of %TAG handle mappings */
	size_t tag_handle_count;
};

/* ============================================================================
 * Phase 4: DOM Parser API (Internal)
 * ============================================================================ */

/**
 * @brief Parse YAML string into DOM document.
 *
 * This is the internal implementation of gtext_yaml_parse(). It uses the
 * streaming parser internally and builds a DOM tree in the context arena.
 *
 * @param input Input string (must remain valid for document lifetime)
 * @param length Length of input string
 * @param options Parse options (NULL for defaults)
 * @param error Error output (may be NULL)
 * @return Document on success, NULL on error
 */
GTEXT_INTERNAL_API GTEXT_YAML_Document *yaml_parse_document(
	const char *input,
	size_t length,
	const GTEXT_YAML_Parse_Options *options,
	GTEXT_YAML_Error *error
);

GTEXT_INTERNAL_API GTEXT_YAML_Document *yaml_parse_json_document(
	const char *input,
	size_t length,
	const GTEXT_YAML_Parse_Options *options,
	GTEXT_YAML_Error *error
);

/**
 * @brief Resolve tags and implicit scalar types for a document.
 */
GTEXT_INTERNAL_API GTEXT_YAML_Status yaml_resolve_document(
	GTEXT_YAML_Document *doc,
	GTEXT_YAML_Error *error
);

/**
 * @brief Node factory functions (allocate from context arena).
 */
/**
 * @brief Decode a "!!binary" scalar's base64 text into the document's arena.
 *
 * Shared so the DOM constructor answers the same way the resolver does: a
 * node tagged !!binary holds the decoded bytes, and text that is not base64
 * is not a binary value at all.
 */
GTEXT_INTERNAL_API bool gtext_yaml_base64_decode(
	GTEXT_YAML_Document *doc,
	const char *value,
	size_t len,
	const unsigned char **out_data,
	size_t *out_len
);

GTEXT_INTERNAL_API GTEXT_YAML_Node *yaml_node_new_scalar(
	yaml_context *ctx,
	const char *value,
	size_t length,
	const char *tag,
	const char *anchor
);

GTEXT_INTERNAL_API GTEXT_YAML_Node *yaml_node_new_sequence(
	yaml_context *ctx,
	size_t capacity,
	const char *tag,
	const char *anchor
);

GTEXT_INTERNAL_API GTEXT_YAML_Node *yaml_node_new_mapping(
	yaml_context *ctx,
	size_t capacity,
	const char *tag,
	const char *anchor
);

GTEXT_INTERNAL_API GTEXT_YAML_Node *yaml_node_new_alias(
	yaml_context *ctx,
	const char *anchor_name
);

/* Stream internal API */
GTEXT_INTERNAL_API void gtext_yaml_stream_set_sync_mode(GTEXT_YAML_Stream *s, bool sync);

/**
 * @brief The scanner's retention switch and its buffer, reached through the
 *        stream, which is all the DOM parser is given.
 *
 * See gtext_yaml_scanner_retain_decoded(). Retention is asked for once, after
 * the stream is made and before it is fed; the buffer is asked for on every
 * event, because it moves.
 */
GTEXT_INTERNAL_API void gtext_yaml_stream_retain_decoded_input(GTEXT_YAML_Stream *s);
GTEXT_INTERNAL_API bool gtext_yaml_stream_decoded_input(const GTEXT_YAML_Stream *s, const char **data, size_t *len);

/**
 * @brief Report what the scanner said when it last refused a token.
 *
 * gtext_yaml_stream_feed() and gtext_yaml_stream_finish() hand back a status
 * and nothing else, so a caller that wants to tell the user which fault it hit
 * has to come back and ask. Returns false, leaving @p out alone, when the
 * stream has not seen a scan fail.
 *
 * The message points at a string literal in the scanner and stays valid after
 * the stream is freed; the struct carries nothing that needs releasing.
 */
GTEXT_INTERNAL_API bool gtext_yaml_stream_last_error(
	const GTEXT_YAML_Stream *s,
	GTEXT_YAML_Error *out
);

/**
 * @brief Spell a non-string mapping key the way gtext_yaml_to_json() does.
 *
 * The name a YAML key gets in the converted JSON, for the keys JSON has no
 * spelling for: a null, a bool, an int, a float. Written into @p buf, which
 * must be at least 40 bytes; @p out is set to point into it, or to a literal
 * for the cases that need no formatting.
 *
 * Shared with the validator rather than copied into it. A JSON Pointer from a
 * schema failure indexes the *converted* document, and resolving it against
 * the YAML document means asking which YAML key produced that name - which is
 * this question, backwards. Two implementations of it would drift, and the
 * drift would present as a validation error reported at the wrong line.
 *
 * @return GTEXT_YAML_OK, or GTEXT_YAML_E_INVALID for a key with no JSON name
 *         (a collection, or an infinity or NaN).
 */
GTEXT_INTERNAL_API GTEXT_YAML_Status yaml_coerce_key_name(
	const GTEXT_YAML_Node *key,
	char *buf,
	size_t buf_size,
	const char **out,
	const GTEXT_Allocator *alloc,
	GTEXT_YAML_Error *out_err
);

#endif /* GHOTI_IO_GTEXT_SRC_YAML_YAML_INTERNAL_H */
