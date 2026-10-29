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
 * @file css_selector.h
 * @brief Selectors Level 4: the parser, the compiled form, the serializer and the specificity.
 *
 * Normative text: Selectors Level 4 and 5, CSS Nesting 1, CSS Scoping 1 and CSS Shadow Parts 1 (shadow selectors), CSS Pseudo-Elements 4, CSS
 * Custom State, CSSOM ("serialize a selector") and the DOM Standard, as the served Editor's Drafts say (read 2026-10-28). The sentences that
 * the tests quote are in tests/test-css-selector.cpp. Another implementation's code is not read.
 *
 * ## The compiled form
 *
 * A parse returns a @ref GTEXT_CSS_Selector_List, which owns every node in one block of memory (like a GTEXT_CSS_Tree), is **immutable** and
 * **reference counted** (gtext_css_selector_list_retain(), gtext_css_selector_list_free(); the counts are atomic, so a compiled selector may be
 * shared by any number of threads). A list is complex selectors; a complex selector is compounds with the combinators between them; a compound
 * is simple selectors. Every node has a stable `id` (from 1, in the order of the parse) and a span in the bytes it was read from, and the
 * nodes are plain structs read in place.
 *
 * ## The modes
 *
 * The grammar of Selectors 4 has five lists, and the entry points take the mode of the one wanted:
 *
 * | Mode | Where | An invalid selector |
 * |---|---|---|
 * | GTEXT_CSS_SELECTOR_COMPLEX_LIST | the prelude of a style rule, `querySelector()` | **unforgiving**: the whole list is invalid |
 * | GTEXT_CSS_SELECTOR_FORGIVING_LIST | the argument of `:is()` and `:where()` | kept as written and never matched (GTEXT_CSS_Complex::raw); an empty argument is an empty list that matches nothing |
 * | GTEXT_CSS_SELECTOR_RELATIVE_LIST | the argument of `:has()`, a nested style rule | unforgiving; each selector may begin with a combinator |
 * | GTEXT_CSS_SELECTOR_COMPOUND | `:host()`, `:host-context()`, `::slotted()` | one compound selector |
 * | GTEXT_CSS_SELECTOR_REAL_LIST | `:not()`, the `of S` of `:nth-child()` | unforgiving; no pseudo-element |
 *
 * **A selector that is not valid is an answer, not a failure** (the rule of C0): the call returns GTEXT_CSS_OK, the list is NULL, and the result
 * holds a diagnostic with a span and a reference to the sentence of the draft that makes it invalid. A failure is an argument, memory or a
 * limit, each limit its own status.
 *
 * ## The selectors are data
 *
 * Which pseudo-classes and pseudo-elements exist, what each takes as an argument, what may follow a pseudo-element, which state bits and
 * callbacks a match asks the caller for, the specificity rule and the status in this library are rows of tools/css/selectors.tsv, generated
 * from the pinned webref data and the supplement; the parser reads them from the generated src/css/css_selector_tables.h. A selector that is
 * not a row (an unknown or vendor-prefixed name) is invalid, never guessed.
 *
 * ## Specificity
 *
 * Exactly the draft's, with the arithmetic of gtext_css_specificity_add() (components saturate at `UINT32_MAX`). A type selector or a
 * pseudo-element counts in C, a class, an attribute selector or a pseudo-class in B, an id in A; `:is()`, `:not()`, `:has()` and the `of S` of
 * `:nth-child()` take the most specific complex selector of their argument, `:nth-*` itself adding (0,1,0); `:where()` is zero, as is the empty
 * forgiving list; `:host` is (0,1,0); `:host(S)` and `:host-context(S)` add S; `::slotted(S)` is a pseudo-element plus S; a tree-abiding chain
 * of pseudo-elements counts each. **`&`** is the most specific complex selector of the list it stands for (the parent rule's, or `:where(:scope)`
 * in an `@scope` and `:scope` at the top level, which is what a parse alone assumes: (0,1,0)); a style sheet recomputes it with the parent.
 *
 * ## Serialization
 *
 * gtext_css_selector_serialize() is CSSOM's "serialize a group of selectors": canonical, and a parse of its output equals the parse of the
 * input in structure and specificity (the fuzz harness checks the fixed point).
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_SELECTOR_H
#define GHOTI_IO_GTEXT_CSS_CSS_SELECTOR_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_cascade.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_parse.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @name Dependency kinds
 *  What a change of the tree can affect for a selector (the invalidation summary of gtext_css_selector_dependencies(); the consumer is C6).
 *  @{ */
#define GTEXT_CSS_DEP_NAME 0x1u        ///< The local name or namespace of an element.
#define GTEXT_CSS_DEP_ID 0x2u          ///< An id.
#define GTEXT_CSS_DEP_CLASS 0x4u       ///< A class.
#define GTEXT_CSS_DEP_ATTR 0x8u        ///< An attribute (the names are listed).
#define GTEXT_CSS_DEP_STATE 0x10u      ///< A state bit (the bits are listed).
#define GTEXT_CSS_DEP_TREE 0x20u       ///< The ancestors (a descendant or child combinator, or `:root`).
#define GTEXT_CSS_DEP_SIBLING 0x40u    ///< The preceding siblings (`+` or `~`).
#define GTEXT_CSS_DEP_FIRST_LAST 0x80u ///< The first or last position among siblings (`:first-child`, `:last-of-type`, ...).
#define GTEXT_CSS_DEP_NTH 0x100u       ///< The index among siblings (`:nth-*`).
#define GTEXT_CSS_DEP_EMPTY 0x200u     ///< Whether the element has children (`:empty`).
#define GTEXT_CSS_DEP_HAS 0x400u       ///< The descendants or later siblings (`:has()`).
#define GTEXT_CSS_DEP_LANG 0x800u      ///< The language (`:lang()`).
#define GTEXT_CSS_DEP_DIR 0x1000u      ///< The direction (`:dir()`).
#define GTEXT_CSS_DEP_LOGICAL 0x2000u  ///< A logical combination (`:is()`, `:where()`, `:not()`): the arguments' kinds are included.
#define GTEXT_CSS_DEP_COLUMN 0x4000u   ///< A column (`||`, `:nth-col()`): never matches here.
#define GTEXT_CSS_DEP_SCOPE 0x8000u    ///< `:scope`.
#define GTEXT_CSS_DEP_HOST 0x10000u    ///< A shadow host or the shadow tree (`:host`, `::slotted()`, `::part()`).
#define GTEXT_CSS_DEP_NESTING 0x20000u ///< The nesting selector.
#define GTEXT_CSS_DEP_PSEUDO 0x40000u  ///< A pseudo-element.
/** @} */

/** @brief Which list of the grammar a parse reads (see the table of the file). */
typedef enum {
  GTEXT_CSS_SELECTOR_COMPLEX_LIST = 0, ///< `<complex-selector-list>`, unforgiving.
  GTEXT_CSS_SELECTOR_FORGIVING_LIST,   ///< `<forgiving-selector-list>`: an invalid entry is dropped.
  GTEXT_CSS_SELECTOR_RELATIVE_LIST,    ///< `<relative-selector-list>`, unforgiving; a selector may begin with a combinator.
  GTEXT_CSS_SELECTOR_COMPOUND,         ///< One `<compound-selector>`.
  GTEXT_CSS_SELECTOR_REAL_LIST         ///< `<complex-real-selector-list>`, unforgiving, no pseudo-element.
} GTEXT_CSS_Selector_Mode;

/** @brief The combinator before a compound (the first compound of a complex selector has NONE, a relative selector's has the leading combinator). */
typedef enum {
  GTEXT_CSS_COMBINATOR_NONE = 0,  ///< None: the first compound.
  GTEXT_CSS_COMBINATOR_DESCENDANT, ///< White space. As the leading combinator of a relative selector, the implied one.
  GTEXT_CSS_COMBINATOR_CHILD,     ///< `>`
  GTEXT_CSS_COMBINATOR_NEXT_SIBLING, ///< `+`
  GTEXT_CSS_COMBINATOR_SUBSEQUENT_SIBLING, ///< `~`
  GTEXT_CSS_COMBINATOR_COLUMN     ///< `||`: parses, serializes and has a specificity, and never matches (no tree interface has columns).
} GTEXT_CSS_Combinator;

/** @brief What a simple selector is. */
typedef enum {
  GTEXT_CSS_SIMPLE_TYPE = 0,    ///< A type selector (`div`, `svg|rect`).
  GTEXT_CSS_SIMPLE_UNIVERSAL,   ///< `*`, `*|*`, `ns|*`.
  GTEXT_CSS_SIMPLE_ID,          ///< `#id`.
  GTEXT_CSS_SIMPLE_CLASS,       ///< `.class`.
  GTEXT_CSS_SIMPLE_ATTRIBUTE,   ///< `[attr]`, `[attr=value i]`.
  GTEXT_CSS_SIMPLE_PSEUDO_CLASS, ///< `:hover`, `:nth-child(2n+1 of .a)`.
  GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT, ///< `::before`, `::part(a b)`, and the legacy `:before`.
  GTEXT_CSS_SIMPLE_NESTING      ///< `&`.
} GTEXT_CSS_Simple_Kind;

/** @brief The namespace of a type selector or an attribute selector as written. */
typedef enum {
  GTEXT_CSS_NS_UNSPECIFIED = 0, ///< No prefix: a type selector uses the default namespace (if the map has one), an attribute has none.
  GTEXT_CSS_NS_ANY,             ///< `*|`
  GTEXT_CSS_NS_NONE,            ///< `|`: no namespace.
  GTEXT_CSS_NS_NAMED            ///< `prefix|`
} GTEXT_CSS_Ns_Kind;

/** @brief The operator of an attribute selector. */
typedef enum {
  GTEXT_CSS_ATTR_EXISTS = 0, ///< `[a]`
  GTEXT_CSS_ATTR_EQUALS,     ///< `=`
  GTEXT_CSS_ATTR_INCLUDES,   ///< `~=`: a whitespace separated word.
  GTEXT_CSS_ATTR_DASH,       ///< `|=`: equal, or equal followed by `-`.
  GTEXT_CSS_ATTR_PREFIX,     ///< `^=`
  GTEXT_CSS_ATTR_SUFFIX,     ///< `$=`
  GTEXT_CSS_ATTR_SUBSTRING   ///< `*=`
} GTEXT_CSS_Attr_Op;

/** @brief The case flag of an attribute selector. */
typedef enum {
  GTEXT_CSS_ATTR_CASE_DEFAULT = 0, ///< None written: the document language decides (HTML has a list of attributes whose values are compared ignoring case).
  GTEXT_CSS_ATTR_CASE_INSENSITIVE, ///< `i`
  GTEXT_CSS_ATTR_CASE_SENSITIVE    ///< `s`
} GTEXT_CSS_Attr_Case;

typedef struct GTEXT_CSS_Selector_List GTEXT_CSS_Selector_List;

/**
 * @struct GTEXT_CSS_Simple
 * @brief A simple selector.
 *
 * Which fields mean something depends on `kind`: a type, universal, id, class or attribute selector has `name` (a universal selector's is empty) and
 * the namespace fields (type, universal and attribute); an attribute selector has `op`, `value`, `attr_case`; a pseudo-class or pseudo-element has
 * `pseudo` (a row of the tables: gtext_css_selector_row()) and what its argument made (`anb_a` is 1 for the `*` of `::highlight(*)`, `::scroll-button(*)` and a view transition name): `anb_a`, `anb_b` and `list` (`:nth-child()` and `of S`), `list`
 * (`:is() :where() :not() :has() :host() :host-context() ::slotted() :current()`), `strings` (`:lang()` ranges, `::part()` names, `:state()`, `::highlight()`,
 * a view transition name, `:dir()`'s keyword), `levels` (`:heading()`).
 */
typedef struct {
  GTEXT_CSS_Id id;                  ///< Unique in the list, from 1, in the order of the parse.
  GTEXT_CSS_Span span;              ///< Where it was read.
  GTEXT_CSS_Simple_Kind kind;       ///< Which.
  GTEXT_CSS_String name;            ///< The local name, the id, the class, the attribute name or the pseudo's canonical name ("nth-child", lower case).
  GTEXT_CSS_Ns_Kind ns_kind;        ///< As written (type, universal, attribute).
  GTEXT_CSS_String ns_prefix;       ///< NS_NAMED: the prefix as written.
  GTEXT_CSS_String ns_uri;          ///< The namespace URI the prefix (or the default namespace) stood for when the selector was parsed; `data` NULL for none.
  bool ns_from_default;             ///< A type selector with no prefix that took the map's default namespace.
  GTEXT_CSS_Attr_Op op;             ///< Attribute: the operator.
  GTEXT_CSS_Attr_Case attr_case;    ///< Attribute: the flag.
  GTEXT_CSS_String value;           ///< Attribute: the value.
  uint16_t pseudo;                  ///< Pseudo-class and pseudo-element: the row of the tables (gtext_css_selector_row()).
  bool legacy;                      ///< A pseudo-element written with one colon (`:before`).
  bool functional;                  ///< A pseudo-class or pseudo-element written with parentheses.
  int64_t anb_a;                    ///< An+B: A.
  int64_t anb_b;                    ///< An+B: B.
  const GTEXT_CSS_Selector_List * list; ///< The argument selectors (see above), or NULL.
  const GTEXT_CSS_String * strings; ///< A list of names or ranges, or NULL.
  const bool * string_quoted;       ///< For each of `strings`: it was written as a string (`:lang("de")`) and not as an identifier.
  size_t string_count;              ///< Their number.
  const int64_t * levels;           ///< `:heading()`: the levels.
  size_t level_count;               ///< Their number.
  bool has_of;                      ///< `:nth-child(An+B of S)`: `list` is S.
  bool never;                       ///< Can never match: a selector a forgiving list accepted after a pseudo-element that does not allow it (`::part(a):is(:first-child)`).
} GTEXT_CSS_Simple;

/**
 * @struct GTEXT_CSS_Compound
 * @brief A compound selector and the combinator before it.
 *
 * The simple selectors are in the order written, a type or universal selector first. The pseudo-element chain of a complex selector (`::part(a)::before`)
 * is in its **last compound**, from `first_pseudo_element` on; before it is the originating element's compound.
 */
typedef struct {
  GTEXT_CSS_Id id;                  ///< Unique in the list.
  GTEXT_CSS_Span span;              ///< Where it was read.
  GTEXT_CSS_Combinator combinator;  ///< The combinator to the compound before it (see GTEXT_CSS_Combinator).
  const GTEXT_CSS_Simple * simples; ///< The simple selectors.
  size_t simple_count;              ///< Their number.
  size_t first_pseudo_element;      ///< The index of the first pseudo-element, or `simple_count` for none.
} GTEXT_CSS_Compound;

/** @struct GTEXT_CSS_Complex @brief A complex selector: compounds joined by combinators. */
typedef struct {
  GTEXT_CSS_Id id;                  ///< Unique in the list.
  GTEXT_CSS_Span span;              ///< Where it was read.
  const GTEXT_CSS_Compound * compounds; ///< Left to right; matching goes right to left.
  size_t compound_count;            ///< Their number (at least 1).
  bool relative;                    ///< A relative selector: the first compound's `combinator` is the leading combinator (DESCENDANT when none was written).
  bool has_nesting;                 ///< Contains the nesting selector (in a compound or in the arguments of a functional pseudo-class).
  bool has_pseudo_element;          ///< Ends in a pseudo-element.
  bool never;                       ///< Can never match (see GTEXT_CSS_Simple::never): a combinator after a pseudo-element's restriction, or a raw entry.
  GTEXT_CSS_String raw;             ///< An entry of a forgiving list that is not a valid selector, kept as written (`data` non-NULL; no compounds, `never` set). The Working Group
                                    ///< resolved to "preserve invalid selectors inside forgiving selector lists" (csswg-drafts 8356): it is serialized, never matched, and has no specificity.
  bool anchor_dependent;            ///< Mentions `:scope` or `&`, which `:has()` ties to its anchor: not memoized across anchors.
  GTEXT_CSS_Specificity specificity; ///< The specificity, with `&` as `:scope` (see the file's description).
  uint32_t dependencies;            ///< GTEXT_CSS_DEP_* of the whole selector.
} GTEXT_CSS_Complex;

/** @struct GTEXT_CSS_Selector_List @brief A list of complex selectors: the compiled form. Immutable and reference counted. */
struct GTEXT_CSS_Selector_List {
  GTEXT_CSS_Id id;                  ///< Unique in the parse.
  GTEXT_CSS_Span span;              ///< Where it was read.
  GTEXT_CSS_Selector_Mode mode;     ///< Which list it is.
  const GTEXT_CSS_Complex * complex; ///< The selectors (none for an empty forgiving list).
  size_t count;                     ///< Their number.
  GTEXT_CSS_Specificity max_specificity; ///< The highest of the selectors' (zero for none).
  bool has_nesting;                 ///< Some selector contains the nesting selector.
  size_t node_count;                ///< Nodes in the list and all lists inside it (the size the limit counts).
  size_t uid;                       ///< Distinguishes this parse from every other in the process (the matcher's caches are keyed by it with the node ids).
  uint32_t dependencies;            ///< GTEXT_CSS_DEP_* of every selector in the list and the lists inside it.
  uint64_t state;                   ///< The GTEXT_CSS_STATE_* bits it tests.
  uint64_t callbacks;               ///< A bit for each GTEXT_CSS_Element_Callback it calls.
  bool unmatchable;                 ///< Some selector in it is parse-only (see GTEXT_CSS_Selector_Requires).
  struct GTEXT_CSS_Selector_List_Internal * internal; ///< Private.
};

/** @brief Another owner of the list (it is freed when the last lets go). Returns `list`; NULL is ignored. */
GTEXT_API GTEXT_CSS_Selector_List * gtext_css_selector_list_retain(const GTEXT_CSS_Selector_List * list);

/** @brief One owner fewer; the last frees the list and every node. NULL is ignored. */
GTEXT_API void gtext_css_selector_list_free(const GTEXT_CSS_Selector_List * list);

/** @brief One namespace prefix of a map. */
typedef struct {
  const char * prefix; ///< The prefix, NUL-terminated (non-empty).
  const char * uri;    ///< The namespace URI, NUL-terminated.
} GTEXT_CSS_Namespace;

/**
 * @struct GTEXT_CSS_Namespace_Map
 * @brief The prefixes a selector may use (`@namespace`): a type selector with a prefix the map lacks is invalid.
 *
 * The strings are copied into the list; the map need not outlive the call.
 */
typedef struct {
  const GTEXT_CSS_Namespace * entries; ///< The prefixes; NULL with a count of 0.
  size_t count;                        ///< Their number.
  const char * default_uri;            ///< The default namespace (`@namespace url(...)`), or NULL for none: a type selector with no prefix then matches any namespace.
} GTEXT_CSS_Namespace_Map;

/** @brief Called for each diagnostic the parse keeps; and the kinds. */
typedef enum {
  GTEXT_CSS_SELDIAG_SYNTAX = 0,   ///< A token that cannot be where it is (the grammar of the list).
  GTEXT_CSS_SELDIAG_EMPTY,        ///< No selector where one is required.
  GTEXT_CSS_SELDIAG_UNKNOWN_SELECTOR, ///< A pseudo-class or pseudo-element that no row of the tables names (a vendor prefix included), or one the library treats as invalid.
  GTEXT_CSS_SELDIAG_ARGUMENT,     ///< The argument of a pseudo-class or pseudo-element is not its grammar.
  GTEXT_CSS_SELDIAG_NOT_ALLOWED,  ///< A selector that is valid but not here (a pseudo-element in `:not()`, `:has()` in `:has()`, a structural pseudo-class after `::part()`).
  GTEXT_CSS_SELDIAG_PREFIX,       ///< A namespace prefix that the map does not declare.
  GTEXT_CSS_SELDIAG_DROPPED,      ///< A forgiving list dropped an entry (the span is the entry's).
  GTEXT_CSS_SELDIAG_TRAILING      ///< Input after the selector.
} GTEXT_CSS_Selector_Diag_Kind;

/** @brief One diagnostic. */
typedef struct {
  GTEXT_CSS_Selector_Diag_Kind kind; ///< What.
  GTEXT_CSS_Span span;               ///< Where, in the bytes of the input.
  const char * reference;            ///< A static reference to the sentence of the draft that decides it ("Selectors 4, 3.6.1"), never NULL.
} GTEXT_CSS_Selector_Diagnostic;

/** @brief What a parse of selectors may do. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;  ///< NULL for the default; it makes the list and the result.
  /** The list wanted (GTEXT_CSS_SELECTOR_COMPLEX_LIST, 0, is the default). */
  GTEXT_CSS_Selector_Mode mode;
  /** The prefixes and the default namespace; NULL: a type or attribute selector with a prefix other than `*` and the empty one is invalid. */
  const GTEXT_CSS_Namespace_Map * namespaces;
  /** Most nodes (simple selectors, compounds, complex selectors and lists, counting the arguments). Default 1,048,576; GTEXT_CSS_E_LIMIT_SELECTOR_COMPONENTS. */
  size_t max_components;
  /** Most complex selectors in one list. Default 65,536; GTEXT_CSS_E_LIMIT_SELECTOR_LIST. */
  size_t max_list;
  /** Most compounds in one complex selector (`a b c ...`). Default 1,024; GTEXT_CSS_E_LIMIT_SELECTOR_COMPONENTS. The matcher answers a complex selector by recursing once per compound, so this is what keeps its use of the C stack bounded. */
  size_t max_compounds;
  /** Deepest nesting of functional pseudo-classes (`:is(:is(...))`). Default 64; GTEXT_CSS_E_LIMIT_SELECTOR_DEPTH. The parser does not recurse in the C stack beyond it. */
  size_t max_depth;
  /** Most diagnostics kept (the rest are counted). Default 1,024. */
  size_t max_diagnostics;
  /** For the text entry point: the limits of tokenizing and parsing the text (css_core.h). */
  GTEXT_CSS_Parse_Options parse;
} GTEXT_CSS_Selector_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Selector_Options gtext_css_selector_options_default(void);

/** @brief What a parse reports. Zero it before the first use; free it with gtext_css_selector_result_free(). */
typedef struct {
  GTEXT_CSS_Status status;        ///< What the call returned.
  const char * message;           ///< A static description; never NULL after a call.
  GTEXT_CSS_Span failure;         ///< For a limit: where it was reached.
  bool valid;                     ///< The selector is valid (`list` was made). false is an answer, with a diagnostic.
  size_t diagnostic_total;        ///< Every diagnostic raised (more than the kept ones when there were many).
  struct GTEXT_CSS_Selector_Result_Internal * internal; ///< Private: the kept diagnostics.
} GTEXT_CSS_Selector_Result;

/** @brief Release what a result holds and reset it. Safe on NULL and on a zeroed result. */
GTEXT_API void gtext_css_selector_result_free(GTEXT_CSS_Selector_Result * result);

/** @brief How many diagnostics the result keeps. NULL is 0. */
GTEXT_API size_t gtext_css_selector_result_diagnostic_count(const GTEXT_CSS_Selector_Result * result);

/** @brief The `index`th kept diagnostic, in the order raised; false past the end. */
GTEXT_API bool gtext_css_selector_result_diagnostic(const GTEXT_CSS_Selector_Result * result, size_t index, GTEXT_CSS_Selector_Diagnostic * out);

/**
 * @brief Parse selectors from text.
 *
 * The text is tokenized as a list of component values and read by gtext_css_selector_parse_values().
 *
 * @param text UTF-8; need not be terminated. NULL only with a `length` of 0.
 * @param length Its length in bytes.
 * @param options NULL for the defaults.
 * @param out Receives the list (one owner); NULL when the selector is not valid; written only on success. Not NULL.
 * @param result NULL, or filled whatever happens.
 * @return GTEXT_CSS_OK (also for an invalid selector: the list is NULL and the result has the diagnostic); GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM;
 *   the limits of the tokenizer and GTEXT_CSS_E_LIMIT_SELECTOR_COMPONENTS, _LIST and _DEPTH.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_selector_parse(const char * text, size_t length, const GTEXT_CSS_Selector_Options * options,
    GTEXT_CSS_Selector_List ** out, GTEXT_CSS_Selector_Result * result);

/**
 * @brief Parse selectors from component values: the prelude of a style rule, the arguments of a functional pseudo-class.
 *
 * The values must outlive the call only; everything the list keeps is copied. The spans of the list are the values' own.
 *
 * @return As gtext_css_selector_parse().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_selector_parse_values(const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Selector_Options * options, GTEXT_CSS_Selector_List ** out, GTEXT_CSS_Selector_Result * result);

/**
 * @brief CSSOM's "serialize a group of selectors" (the selectors joined by `, `).
 *
 * @param list The list. Not NULL.
 * @param sink Where the text goes. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_WRITE.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_selector_serialize(const GTEXT_CSS_Selector_List * list, const GTEXT_Markup_Sink * sink);

/** @brief CSSOM's "serialize a selector" for one complex selector of a list. */
GTEXT_API GTEXT_CSS_Status gtext_css_selector_serialize_complex(const GTEXT_CSS_Complex * complex, const GTEXT_Markup_Sink * sink);

/** @brief The text of a list in a new buffer (release it with gtext_css_selector_buffer_free()); NUL-terminated, the length not counting the NUL. */
GTEXT_API GTEXT_CSS_Status gtext_css_selector_serialize_buffer(const GTEXT_CSS_Selector_List * list, const GTEXT_Allocator * allocator, char ** out,
    size_t * length);

/** @brief Release a buffer from gtext_css_selector_serialize_buffer(). */
GTEXT_API void gtext_css_selector_buffer_free(const GTEXT_Allocator * allocator, char * buffer);

/**
 * @brief The specificity of a complex selector when `&` stands for a selector of the given specificity (the list of the parent rule, or zero in `@scope`).
 *
 * A parse assumes (0,1,0) for `&`, which is what it is at the top level; a style sheet recomputes the selectors that contain one.
 */
GTEXT_API GTEXT_CSS_Specificity gtext_css_complex_specificity(const GTEXT_CSS_Complex * complex, GTEXT_CSS_Specificity nesting);

/** @brief gtext_css_complex_specificity() for the most specific complex selector of the list. */
GTEXT_API GTEXT_CSS_Specificity gtext_css_selector_list_specificity(const GTEXT_CSS_Selector_List * list, GTEXT_CSS_Specificity nesting);

/** @brief What a compiled selector asks of the caller. */
typedef struct {
  uint64_t callbacks;      ///< A bit for each GTEXT_CSS_Element_Callback (`1 << number`) the selector calls.
  uint64_t state;          ///< The GTEXT_CSS_STATE_* bits it tests.
  bool unmatchable;        ///< Some selector in it is parse-only: a match is GTEXT_CSS_E_UNSUPPORTED naming `state` or never (`||`).
} GTEXT_CSS_Selector_Requires;

/**
 * @brief The callbacks and state bits that matching a list calls, so that a consumer checks its element interface once, at compile time.
 *
 * @param list The list. Not NULL.
 * @param out Receives it. Not NULL.
 */
GTEXT_API void gtext_css_selector_requires(const GTEXT_CSS_Selector_List * list, GTEXT_CSS_Selector_Requires * out);

/**
 * @brief Check an element interface against what a list requires.
 *
 * @param iface The table.
 * @param list The list.
 * @param result NULL, or filled; `callback` names the first callback that is absent.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_UNSUPPORTED.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_selector_check_interface(const GTEXT_CSS_Element_Interface * iface, const GTEXT_CSS_Selector_List * list,
    GTEXT_CSS_Element_Result * result);

/** @brief What a compiled selector depends on (for invalidation; nothing here consumes it). */
typedef struct {
  uint32_t kinds;                  ///< GTEXT_CSS_DEP_* of the whole list.
  uint64_t state;                  ///< The state bits.
  const GTEXT_CSS_String * attributes; ///< The attribute names it tests (as written, once each, in order of first use; an HTML document compares them ignoring case).
  size_t attribute_count;          ///< Their number.
} GTEXT_CSS_Selector_Dependencies;

/**
 * @brief The dependency summary of a list; the `attributes` are owned by the list.
 *
 * @param list The list. Not NULL.
 * @param out Receives it. Not NULL.
 */
GTEXT_API void gtext_css_selector_dependencies(const GTEXT_CSS_Selector_List * list, GTEXT_CSS_Selector_Dependencies * out);

/** @brief What the tables say about a row. */
typedef struct {
  const char * name;        ///< As webref spells it (":nth-child()", "::part()", "&").
  const char * base;        ///< Without colons or parentheses.
  const char * note;        ///< The supplement's note.
  uint64_t state;           ///< The state bits a match asks for.
  uint32_t callbacks;       ///< The callbacks a match asks for.
  uint32_t dependencies;    ///< GTEXT_CSS_DEP_* bits.
  bool pseudo_element;      ///< A pseudo-element.
  bool implemented;         ///< Parsed, serialized and matched.
  bool parse_only;          ///< Parsed and serialized; a match is GTEXT_CSS_E_UNSUPPORTED, or never.
} GTEXT_CSS_Selector_Row;

/** @brief The number of rows of the tables. */
GTEXT_API size_t gtext_css_selector_row_count(void);

/** @brief One row; false for a number past the end. */
GTEXT_API bool gtext_css_selector_row(size_t index, GTEXT_CSS_Selector_Row * out);

/** @brief The row of a name as webref spells it (":hover", "::part()", ">"), or -1. */
GTEXT_API int gtext_css_selector_find_row(const char * name);

/** @brief The sentinel a `ns` argument of the `attribute` callback points at for `*|`: any namespace, none included. Compare the pointer. */
GTEXT_API const GTEXT_CSS_String * gtext_css_namespace_any(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_SELECTOR_H
