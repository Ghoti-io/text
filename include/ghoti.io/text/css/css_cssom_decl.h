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
 * @brief `CSSStyleDeclaration`: the declaration object of every owner (CSS Object Model, "The CSSStyleDeclaration Interface").
 *
 * A @ref GTEXT_CSSOM_Decl is built on the declaration block of css_shorthand.h and has one of four owners:
 *
 * - **free-standing** (gtext_cssom_decl_new()): for editors and tests; what `new CSSStyleDeclaration` would be and the IDL forbids;
 * - **element-owned** (gtext_cssom_inline_style()): an element's `style` attribute. Its text is the declaration's only storage: a mutation writes the attribute
 *   back through the caller's binding ("update style attribute for") and a read is **validated by content**, so that a change made behind the object's back
 *   (`setAttribute('style', ...)` through any API) is seen on the next call without an observer: the declaration remembers the exact attribute string it last
 *   parsed or wrote, and each call that reads or edits it validates it once, by one integer when the binding has an `attribute_version`, by nothing when the
 *   caller's tree version has not moved since, else by comparing the attribute (length, then bytes) and parsing it again if it differs. The write-back updates
 *   the remembered string, so a declaration never re-parses its own write;
 * - **rule-owned** (a style rule, a keyframe, a page or margin rule, a font-face, a position-try, a nested-declarations rule): the declarations are the rule's
 *   own, and a mutation is a mutation of the rule and of its sheet (the owner hook of css_cssom_rule.h);
 * - **computed** (css_cssom_computed.h): read-only; every mutator is GTEXT_CSS_E_NO_MODIFICATION.
 *
 * ## The operations
 *
 * Each is CSSOM's algorithm, and a test names each step it follows. `length` and `item(i)`, `getPropertyValue(name)` (a longhand's last declaration in its
 * shortest specified form; a shorthand from its longhands, the empty string when any is missing, when the importance differs or when they are not a value of its
 * grammar; a shorthand all of whose longhands are pending values of one declaration (`margin: var(--x)`) as the original value; a custom property by its stored
 * token text; the empty string for none), `getPropertyPriority(name)`, `setProperty(name, value, priority)`, `removeProperty(name)`, and the `cssText` getter and
 * setter. A property name that is not a custom property is ASCII lowercased; an alias is its property.
 *
 * `setProperty` follows the served steps in order: a read-only declaration is GTEXT_CSS_E_NO_MODIFICATION; a property that is not a custom property and is not
 * supported returns with nothing changed **before the value is looked at** (`setProperty('bogus', '')` removes nothing and reports IGNORED_UNKNOWN_PROPERTY); an empty
 * value is `removeProperty`; a priority that is neither empty nor an ASCII case-insensitive `important` is ignored; an invalid value, and a value that contains
 * `!important`, is ignored with nothing changed. The value is parsed by gtext_css_parse_property_text(), the function the cascade uses; a registered custom
 * property's value is not validated at set time, as the specification says. A property the registry has but the library cannot judge (the properties of
 * GTEXT_CSS_E_UNSUPPORTED), and a shorthand it cannot split, is **kept as written** on a read (an opaque item of the block: in its place, with its importance, written back verbatim) and **reported** on a set, never guessed: the call returns GTEXT_CSS_E_UNSUPPORTED and the outcome names it.
 *
 * A shorthand expands through the registry's rules: a longhand already in the block is replaced **in place**, a new one is appended in the shorthand's order. A
 * mutation that leaves the affected declaration byte-identical (the same longhand value text and importance) is UNCHANGED: no version, no generation, no
 * notification. Any other bumps the declaration's version once and the document's generation once.
 *
 * ## The IDL attribute names
 *
 * `style.backgroundColor`, `style['background-color']`, `cssFloat`, the `webkit`-prefixed forms and the alias of every legacy property are the generated table
 * tools/css/cssom_idl_names.tsv ("CSS property to IDL attribute"): gtext_cssom_idl_property() maps an attribute name to the property it stands for, and the
 * `_idl_get` and `_idl_set` operations are `getPropertyValue` and `setProperty(..., "")` on it. An attribute that is not in the table is not a CSS property: the
 * calls return false or GTEXT_CSS_E_INVALID, and a binding treats it as an ordinary JavaScript property.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CSSOM_DECL_H
#define GHOTI_IO_GTEXT_CSS_CSS_CSSOM_DECL_H

#include <ghoti.io/text/css/css_cssom_core.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/css/css_value.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A declaration block object (the struct is private). */
typedef struct GTEXT_CSSOM_Decl GTEXT_CSSOM_Decl;

/** @brief Whose declarations they are. */
typedef enum {
  GTEXT_CSSOM_DECL_FREE = 0,   ///< Free-standing.
  GTEXT_CSSOM_DECL_RULE,       ///< A rule's.
  GTEXT_CSSOM_DECL_ELEMENT,    ///< An element's `style` attribute.
  GTEXT_CSSOM_DECL_COMPUTED    ///< A computed style's (read-only).
} GTEXT_CSSOM_Decl_Owner;

/**
 * @struct GTEXT_CSSOM_Attribute_Binding
 * @brief The caller's `style` attribute, for an element-owned declaration. Size-prefixed; copied. Every callback takes the `user` pointer and returns a status; a
 *   failing callback fails the read or the write naming nothing else, and the declaration is as it was.
 */
typedef struct {
  size_t size;      ///< `sizeof` this struct as the caller's header has it. Required.
  void * user;      ///< Passed to every callback.
  /** Read the element's `style` attribute: `*present` says whether there is one, `*value` and `*length` its bytes. The bytes are borrowed, valid until the next call
   *  of any callback or until the attribute changes. Required. */
  GTEXT_CSS_Status (*get_attribute)(void * user, const void * element, const char ** value, size_t * length, bool * present);
  /** Set the attribute to this text (the declaration block's serialization; the empty string for an empty block: the attribute is not removed). Required. */
  GTEXT_CSS_Status (*set_attribute)(void * user, const void * element, const char * value, size_t length);
  /** Remove the attribute. Optional: no operation of a declaration calls it (CSSOM never removes the attribute), it is the binding's complete interface for the callers
   *  that edit rules and elements through one adapter. */
  GTEXT_CSS_Status (*remove_attribute)(void * user, const void * element);
  /** A counter the caller advances whenever that element's `style` attribute changes. Optional: with it a validation is one integer comparison. */
  uint64_t (*attribute_version)(void * user, const void * element);
} GTEXT_CSSOM_Attribute_Binding;

/**
 * @brief A free-standing declaration, empty.
 * @param document The document. Not NULL.
 * @param out Receives the declaration with one external reference (the caller's); written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, or a document that was released); GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_CSSOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_new(GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Decl ** out);

/** @brief Take another external reference. Writer thread only. @return The declaration. */
GTEXT_API GTEXT_CSSOM_Decl * gtext_cssom_decl_retain(GTEXT_CSSOM_Decl * decl);

/** @brief Give one back. The object is freed when its last reference, internal or external, goes. NULL is ignored. */
GTEXT_API void gtext_cssom_decl_release(GTEXT_CSSOM_Decl * decl);

/** @brief Whose declarations these are. */
GTEXT_API GTEXT_CSSOM_Decl_Owner gtext_cssom_decl_owner(const GTEXT_CSSOM_Decl * decl);

/** @brief Whether every mutator is GTEXT_CSS_E_NO_MODIFICATION. */
GTEXT_API bool gtext_cssom_decl_readonly(const GTEXT_CSSOM_Decl * decl);

/** @brief The declaration's version: it increases by one with each mutation that changed it. 0 for NULL. */
GTEXT_API uint64_t gtext_cssom_decl_version(const GTEXT_CSSOM_Decl * decl);

/** @brief The document the declaration belongs to, or NULL when its handle was released. */
GTEXT_API GTEXT_CSSOM_Document * gtext_cssom_decl_document(const GTEXT_CSSOM_Decl * decl);

/** @brief `length`. For an element-owned declaration this validates the attribute and can fail with what the binding failed with. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_length(GTEXT_CSSOM_Decl * decl, size_t * out);

/**
 * @brief `item(index)`: the name of the `index`th declaration, as a longhand is named (a shorthand's longhands are listed, not the shorthand), a custom property as written.
 * @param out Receives the name in a new NUL-terminated buffer (release with gtext_cssom_buffer_free()); an empty string past the end, as `item()` answers.
 * @param out_length Receives its length; NULL if not wanted.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_item(GTEXT_CSSOM_Decl * decl, size_t index, char ** out, size_t * out_length);

/** @brief `getPropertyValue(name)`. @param out Receives the value in a new buffer (release with gtext_cssom_buffer_free()); the empty string for none. @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_OUTPUT. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_get_property_value(GTEXT_CSSOM_Decl * decl, const char * name, size_t name_length, char ** out, size_t * out_length);

/**
 * @brief `getPropertyPriority(name)`: `important` if the declaration is important; for a shorthand if **all** its longhands are present and important; else the empty string.
 * @param out Receives a static string, `"important"` or `""`. Not NULL.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_get_property_priority(GTEXT_CSSOM_Decl * decl, const char * name, size_t name_length, const char ** out);

/**
 * @brief `setProperty(name, value, priority)`.
 *
 * @param decl The declaration. Not NULL.
 * @param name The property name; a name beginning `--` is a custom property and is case-sensitive.
 * @param value The value, without `!important`; the empty string removes the property.
 * @param priority The priority: the empty string or `important`, ASCII case-insensitive.
 * @param outcome NULL, or filled (CHANGED, UNCHANGED, or the ignore; `attribute_written`; `property`).
 * @return GTEXT_CSS_OK (also for every ignore: see the outcome); GTEXT_CSS_E_NO_MODIFICATION; GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_UNSUPPORTED (a property the registry
 *   has and the library cannot judge: the outcome's `property`); GTEXT_CSS_E_LIMIT_DECLARATIONS; GTEXT_CSS_E_LIMIT_CSSOM; GTEXT_CSS_E_LIMIT_INPUT; GTEXT_CSS_E_OOM; and the
 *   failure of the caller's binding. A failed call changes nothing.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_set_property(GTEXT_CSSOM_Decl * decl, const char * name, size_t name_length, const char * value, size_t value_length,
    const char * priority, size_t priority_length, GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief `removeProperty(name)`: removes the declaration (a shorthand's longhands) and returns the value it had.
 * @param old_value Receives the old value in a new buffer (release with gtext_cssom_buffer_free()); the empty string when nothing was removed. NULL if not wanted.
 * @param old_length Receives its length; NULL if not wanted.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_remove_property(GTEXT_CSSOM_Decl * decl, const char * name, size_t name_length, char ** old_value, size_t * old_length,
    GTEXT_CSSOM_Outcome * outcome);

/** @brief The `cssText` getter: "serialize a CSS declaration block". @param out Receives a new buffer (release with gtext_cssom_buffer_free()). */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_css_text(GTEXT_CSSOM_Decl * decl, char ** out, size_t * out_length);

/** @brief The `cssText` getter to a sink. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_serialize(GTEXT_CSSOM_Decl * decl, const GTEXT_Markup_Sink * sink);

/**
 * @brief The `cssText` setter: parse the text as a list of declarations and replace the whole list; a declaration that does not parse is dropped, one the library cannot
 *   judge is kept as written, nested rules in the text are dropped, and the later of two declarations of a property replaces the earlier and takes its place.
 *
 * The write-back of an element-owned declaration is unconditional (CSSOM's step 4): `attribute_written` is true even when the text was byte-identical, in which case
 * the outcome is UNCHANGED, with no version and no notification.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_set_css_text(GTEXT_CSSOM_Decl * decl, const char * text, size_t length, GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief The typed value of a declaration (the C1 value of css_value.h): the typed access this layer gives in place of Typed OM.
 * @param name A longhand or a custom property.
 * @param out Receives the value set **retained** (release with gtext_css_value_set_free()): atomic, valid across any later mutation and on any thread. NULL for a CSS-wide
 *   keyword; written only when the declaration exists.
 * @param wide Receives the CSS-wide keyword if the declaration is one, else GTEXT_CSS_WIDE_NONE; NULL if not wanted.
 * @param important Receives the importance; NULL if not wanted.
 * @param found Receives whether the declaration exists. Not NULL.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_value_set(GTEXT_CSSOM_Decl * decl, const char * name, size_t name_length, GTEXT_CSS_Value_Set ** out,
    GTEXT_CSS_Wide_Keyword * wide, bool * important, bool * found);

/* ---- the IDL attribute names -------------------------------------------------------------------------------------------------------------- */

/**
 * @brief The property an IDL attribute of `CSSStyleDeclaration` stands for: `backgroundColor` and `background-color` are `background-color`, `cssFloat` is `float`,
 *   `webkitTransform` and `WebkitTransform` are `-webkit-transform`.
 * @param attribute The attribute name (case-sensitive).
 * @param length Its length.
 * @param property Receives the property name (static, NUL-terminated); NULL if not wanted.
 * @param property_length Receives its length; NULL if not wanted.
 * @return Whether the attribute is a CSS property's.
 */
GTEXT_API bool gtext_cssom_idl_property(const char * attribute, size_t length, const char ** property, size_t * property_length);

/** @brief `style[attribute]` for a property attribute: gtext_cssom_decl_get_property_value() of the property. @return GTEXT_CSS_E_INVALID if the attribute is not a CSS property's. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_idl_get(GTEXT_CSSOM_Decl * decl, const char * attribute, size_t length, char ** out, size_t * out_length);

/** @brief `style[attribute] = value`: gtext_cssom_decl_set_property() with the empty priority (the empty string removes the property). @return GTEXT_CSS_E_INVALID if the attribute is not a CSS property's. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_idl_set(GTEXT_CSSOM_Decl * decl, const char * attribute, size_t length, const char * value, size_t value_length,
    GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief The camel-cased IDL attribute of a property ("CSS property to IDL attribute"): `font-size` is `fontSize`, `-webkit-transform` is `WebkitTransform`.
 * @param property The property name. @param length Its length.
 * @param out Receives a new NUL-terminated buffer, from the allocator given (NULL: the default); release with gtext_cssom_buffer_free() given the same allocator.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_idl_attribute(const char * property, size_t length, const GTEXT_Allocator * allocator, char ** out, size_t * out_length);

/* ---- element-owned declarations ---------------------------------------------------------------------------------------------------------- */

/**
 * @brief The declaration of an element's `style` attribute (`ElementCSSInlineStyle.style`).
 *
 * The same pointer is returned while anyone holds it; the document keeps a weak map from element to declaration, an entry removed when the last reference goes,
 * bounded by `max_inline_styles`. The `binding` is read when the declaration is made. A call for an element the document has already given a declaration returns
 * that declaration (retained).
 *
 * @param document The document; GTEXT_CSS_E_INVALID if its handle was released.
 * @param element The caller's element handle. Not NULL.
 * @param binding The attribute binding. Not NULL.
 * @param out Receives the declaration, retained; written only on success.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_CSSOM; the failure of the binding's `get_attribute`; GTEXT_CSS_E_LIMIT_INPUT.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_inline_style(GTEXT_CSSOM_Document * document, const void * element, const GTEXT_CSSOM_Attribute_Binding * binding,
    GTEXT_CSSOM_Decl ** out);

/** @brief The caller's notice, with the option `inline_trust_notifications`, that the element's `style` attribute changed: the declaration reads it again on its next call. */
GTEXT_API void gtext_cssom_inline_style_attribute_changed(GTEXT_CSSOM_Document * document, const void * element);

/* ---- dumps ---------------------------------------------------------------------------------------------------------------------------------- */

/** @brief The version of the text gtext_cssom_decl_dump() writes; it changes with any change of the format. */
#define GTEXT_CSSOM_DECL_DUMP_VERSION 1

/**
 * @brief Write the declaration in a canonical, versioned text: `cssom-decl 1`, the owner kind, the read-only flag, the version, then one line for each declaration
 *   in order (name, importance, outcome, the value as it is serialized), then the `cssText`. The same bytes on every host. Does not validate an element-owned
 *   declaration against its attribute: it dumps what the object holds.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_dump(const GTEXT_CSSOM_Decl * decl, const GTEXT_Markup_Sink * sink);

/** @brief The length of a hash: SHA-256. */
#define GTEXT_CSSOM_HASH_LENGTH 32u

/** @brief The SHA-256 of the text gtext_cssom_decl_dump() writes. @param hash Receives GTEXT_CSSOM_HASH_LENGTH bytes. Not NULL. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_decl_hash(const GTEXT_CSSOM_Decl * decl, unsigned char hash[GTEXT_CSSOM_HASH_LENGTH]);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CSSOM_DECL_H
