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
 * @file css_registry.h
 * @brief The registry of CSS properties, descriptors, at-rules, value types, functions and selectors: the
 *   generated set from the W3C's curated specification data, and the caller's own registrations over it.
 *
 * ## Two layers
 *
 * The **generated layer** is const data in the library (`src/css/tables/`), written by `tools/css/gen-css-tables.py`
 * from the `curated` branch of `w3c/webref`, pinned by commit and by the SHA-256 of every file: every property
 * with its grammar (the Value Definition Syntax), initial value, inheritance, animation type, applies-to,
 * percentage basis, computed value, canonical order, logical group and shorthand, longhand and legacy-alias
 * relations; every descriptor of every at-rule; every value type and function, with the ones the pin nests inside
 * a property scoped to it; every selector; and an atom table of every keyword of every grammar. No property is
 * unknown to the library; what is phased is the depth of its semantics, which the generated ledger
 * (`tools/css/depth.tsv`) tracks. The generated layer needs no allocation and no relocation to be there, and is
 * read-only.
 *
 * The **caller layer** is built in a @ref GTEXT_CSS_Registry_Builder: properties (name, inheritance, initial value,
 * grammar, computed-value type, animation type), aliases and shorthands (a list of longhands and a hook that expands
 * the shorthand, which the cascade calls). A caller may register a property the pin lacks (`vector`'s own), or
 * redefine a generated one. **Redefining a generated property, or one already registered, is refused with
 * GTEXT_CSS_E_EXISTS unless the definition says `override`, and then the override is reported** by
 * gtext_css_registry_override_at(). Registration is validated when it is made: the grammar is parsed and linked
 * against everything the registry knows, and the initial value must parse against it (and be a concrete value: not
 * a CSS-wide keyword, not a `var()`). Vendor prefixes the pin does not list (`-moz-nosuch`) are unknown properties,
 * which the cascade drops as the standard says; the pin's legacy aliases (`-webkit-transform`) are in the registry
 * as aliases of their targets.
 *
 * ## Frozen
 *
 * gtext_css_registry_freeze() turns the builder into a @ref GTEXT_CSS_Registry, which is **immutable and may be
 * shared by any number of threads**. Compiling a grammar is lazy (a registry that is asked for only `margin` does
 * not compile `grid`); that cache is the one thing that changes after the freeze, and it is guarded: a grammar is
 * compiled once, by whichever thread asks first, under a lock, and published to the others as an immutable object.
 * gtext_css_registry_prepare() compiles every grammar now, after which nothing changes and nothing is locked on the
 * way to a match.
 *
 * ## Identity
 *
 * A property is a @ref GTEXT_CSS_Property_Id: 0 is none, 1 up to gtext_css_registry_generated_property_count() are the
 * pin's, in the sorted order of their names, and the caller's follow in the order they were registered. An id
 * never changes for a registry. An override has an id of its own; a lookup by name finds the override.
 *
 * ## Resolution of a type
 *
 * A grammar reference `<name>` is resolved by, in order: the native types of the engine (css_grammar.h); the type
 * the pin nests inside the property being compiled; the pin's global type; the hand-written function types of
 * css_function.h (for the ones the pin has no grammar for: `circle()`, the transform functions ...). `<'name'>` is
 * the property of that name, an override first.
 *
 * ## Limits
 *
 * The number of caller registrations, the length of a name, of a grammar and of an initial value are bounded
 * (GTEXT_CSS_Registry_Options), each its own status, tested at the bound and one past.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_REGISTRY_H
#define GHOTI_IO_GTEXT_CSS_CSS_REGISTRY_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_grammar.h>
#include <ghoti.io/text/css/css_value.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A registry before it is frozen (the struct is private). Not safe to share. */
typedef struct GTEXT_CSS_Registry_Builder GTEXT_CSS_Registry_Builder;

/** @brief A frozen registry (the struct is private). Immutable; any number of threads may read it. */
typedef struct GTEXT_CSS_Registry GTEXT_CSS_Registry;

/** @brief A property of a registry: 0 is none. */
typedef uint32_t GTEXT_CSS_Property_Id;

/** @brief The id of no property. */
#define GTEXT_CSS_PROPERTY_NONE ((GTEXT_CSS_Property_Id)0)

/** @brief An at-rule of the generated layer: 0 is none. */
typedef uint32_t GTEXT_CSS_At_Rule_Id;

/** @brief A descriptor of the generated layer: 0 is none. */
typedef uint32_t GTEXT_CSS_Descriptor_Id;

/* ---- the closed enums that normalize the pin's prose --------------------- */

/**
 * @brief Whether a property is inherited.
 *
 * The pin's `inherited` field is free text (`yes`, `no`, `see individual properties`, `yes?` ...); the mapping of every
 * string it holds is `tools/css/normalize.tsv`, and the raw string is kept (GTEXT_CSS_Property_Info::inherited_raw).
 * The enumerators are in this order and the generator reads them from this header.
 */
typedef enum {
  GTEXT_CSS_INHERIT_UNKNOWN = 0, ///< The pin does not say (no field, `n/a`, `?`).
  GTEXT_CSS_INHERIT_NO,          ///< Not inherited.
  GTEXT_CSS_INHERIT_YES,         ///< Inherited.
  GTEXT_CSS_INHERIT_MIXED        ///< A shorthand whose longhands are not all alike (and `all`).
} GTEXT_CSS_Inherit;

/**
 * @brief How a property interpolates, from the pin's `animationType` text (tools/css/normalize.tsv maps each distinct
 *   string). The enumerators are in this order and the generator reads them from this header.
 */
typedef enum {
  GTEXT_CSS_ANIM_UNSPECIFIED = 0, ///< The pin gives none.
  GTEXT_CSS_ANIM_NOT_ANIMATABLE,  ///< `not animatable`.
  GTEXT_CSS_ANIM_DISCRETE,        ///< `discrete`.
  GTEXT_CSS_ANIM_BY_COMPUTED_VALUE, ///< `by computed value (type)`: each value type's own interpolation.
  GTEXT_CSS_ANIM_NUMBER,          ///< `number`.
  GTEXT_CSS_ANIM_INTEGER,         ///< `as integer`.
  GTEXT_CSS_ANIM_LENGTH,          ///< `as length`.
  GTEXT_CSS_ANIM_REPEATABLE_LIST, ///< `repeatable list`.
  GTEXT_CSS_ANIM_LIST_PER_ITEM,   ///< Item by item if the list lengths match, discrete otherwise.
  GTEXT_CSS_ANIM_SHAPE,           ///< As `<basic-shape>` where possible, discrete otherwise.
  GTEXT_CSS_ANIM_SHADOW_LIST,     ///< `as shadow list`.
  GTEXT_CSS_ANIM_TRANSFORM,       ///< A transform list, or the rules of `translate`, `rotate`, `scale`.
  GTEXT_CSS_ANIM_FIT_CONTENT,     ///< By computed value, recursing into `fit-content()`.
  GTEXT_CSS_ANIM_FILTER,          ///< The prose of Filter Effects.
  GTEXT_CSS_ANIM_SEE_LONGHANDS,   ///< A shorthand: each longhand's.
  GTEXT_CSS_ANIM_PROSE            ///< A rule written as prose in the property's own specification.
} GTEXT_CSS_Animation;

/**
 * @brief The class of a property's computed value, for the properties a caller registers. The pin gives prose only
 *   (GTEXT_CSS_Property_Info::computed_value), so a generated property reports GTEXT_CSS_COMPUTED_UNSPECIFIED; the
 *   computed-value layer holds the per-property exception table.
 */
typedef enum {
  GTEXT_CSS_COMPUTED_UNSPECIFIED = 0, ///< Prose only.
  GTEXT_CSS_COMPUTED_AS_SPECIFIED,    ///< The specified value.
  GTEXT_CSS_COMPUTED_ABSOLUTE_LENGTHS, ///< The specified value with every length made absolute.
  GTEXT_CSS_COMPUTED_COLOR            ///< A resolved color.
} GTEXT_CSS_Computed;

/** @name Property flags
 *  Bits of GTEXT_CSS_Property_Info::flags.
 *  @{ */
#define GTEXT_CSS_PROP_SHORTHAND 0x01u      ///< Has longhands (or is `all`).
#define GTEXT_CSS_PROP_ALIAS 0x02u          ///< A legacy name of another property (`alias_of`).
#define GTEXT_CSS_PROP_NO_GRAMMAR 0x04u     ///< The pin lists the name and gives no grammar (the compatibility `-webkit-box-*`).
#define GTEXT_CSS_PROP_QUIRK_UNITLESS 0x08u ///< Accepts a unitless length in quirks mode (Values 4, Appendix C).
#define GTEXT_CSS_PROP_CUSTOM 0x10u         ///< The custom property pattern `--*`.
#define GTEXT_CSS_PROP_OVERRIDE 0x20u       ///< The caller's redefinition of another property.
#define GTEXT_CSS_PROP_GENERATED 0x40u      ///< From the pin (a caller's property lacks this bit).
/** @} */

/**
 * @struct GTEXT_CSS_Property_Info
 * @brief Everything the registry knows of one property. Every pointer is owned by the registry and valid until it is
 *   freed; a text the registry does not have is NULL.
 */
typedef struct {
  GTEXT_CSS_Property_Id id;        ///< The property.
  const char * name;               ///< Its name, lower case (`--*` for the custom property pattern), NUL-terminated.
  size_t name_length;              ///< Its length.
  const char * grammar;            ///< The grammar of a value, in the Value Definition Syntax (with the pin's `newValues` appended); NULL for an alias (use the target's) or GTEXT_CSS_PROP_NO_GRAMMAR.
  const char * initial;            ///< The initial value, as text; NULL when the pin gives none.
  GTEXT_CSS_Inherit inherited;     ///< Whether it is inherited (derived for a shorthand the pin leaves to its longhands).
  const char * inherited_raw;      ///< The pin's own string; NULL when it has none.
  GTEXT_CSS_Animation animation;   ///< How it interpolates.
  const char * animation_raw;      ///< The pin's own string; NULL when it has none.
  GTEXT_CSS_Computed computed;     ///< The class of its computed value (GTEXT_CSS_COMPUTED_UNSPECIFIED for the pin's).
  const char * computed_value;     ///< The pin's prose for the computed value.
  const char * applies_to;         ///< The pin's `appliesTo` prose.
  const char * percentages;        ///< The pin's `percentages` prose (the percentage basis).
  const char * canonical_order;    ///< The pin's `canonicalOrder`.
  const char * logical_group;      ///< The name of its logical property group (`margin`, `size` ...), or NULL.
  const char * source;             ///< The pin's file the base definition came from (`css-box`), or NULL for a caller's.
  GTEXT_CSS_Property_Id alias_of;  ///< For an alias, its target; else 0.
  size_t longhand_count;           ///< A shorthand's longhands.
  size_t reset_longhand_count;     ///< Properties it resets and that are not among its longhands (`all`-style resets, `font` resets `font-kerning`).
  size_t style_name_count;         ///< The names of its CSSStyleDeclaration attributes (`margin-top`, `marginTop`).
  uint32_t flags;                  ///< GTEXT_CSS_PROP_* bits.
} GTEXT_CSS_Property_Info;

/** @brief What a descriptor is. */
typedef enum {
  GTEXT_CSS_DESC_DECLARATION = 0, ///< An ordinary descriptor: `name: value` in the at-rule's block (`src` of `@font-face`).
  GTEXT_CSS_DESC_FEATURE_RANGE,   ///< A media or container feature that takes a range (`width`).
  GTEXT_CSS_DESC_FEATURE_DISCRETE, ///< A media or container feature that takes a keyword (`orientation`).
  GTEXT_CSS_DESC_AT_RULE          ///< A nested at-rule (`@swash` of `@font-feature-values`).
} GTEXT_CSS_Descriptor_Kind;

/** @brief What the pin says of an at-rule. */
typedef struct {
  GTEXT_CSS_At_Rule_Id id;     ///< The at-rule.
  const char * name;           ///< `@font-face`; lower case.
  size_t name_length;          ///< Its length.
  const char * grammar;        ///< The at-rule's own syntax (its prelude or block), or NULL.
  size_t descriptor_count;     ///< Its descriptors, features and nested at-rules.
  bool declarations;           ///< Its block is declarations of descriptors (a `@font-face`; not a `@media`, whose descriptors are features).
  bool properties;             ///< Its block also takes declarations of properties (`@page`: its margin and page properties; `@function`: custom properties).
} GTEXT_CSS_At_Rule_Info;

/** @brief What the pin says of a descriptor. */
typedef struct {
  GTEXT_CSS_Descriptor_Id id;  ///< The descriptor.
  GTEXT_CSS_At_Rule_Id at_rule; ///< Its at-rule.
  const char * name;           ///< Lower case.
  size_t name_length;          ///< Its length.
  const char * grammar;        ///< Its grammar.
  const char * initial;        ///< Its initial value as text, or NULL; `N/A` and prose are kept as the pin has them.
  GTEXT_CSS_Descriptor_Kind kind; ///< What it is.
} GTEXT_CSS_Descriptor_Info;

/** @name Type flags
 *  Bits of GTEXT_CSS_Type_Info::flags.
 *  @{ */
#define GTEXT_CSS_TYPE_FUNCTION 0x01u   ///< A function (`rgb()`), not a `<type>`.
#define GTEXT_CSS_TYPE_NATIVE 0x02u     ///< The grammar engine implements it in code; the pin's grammar, if any, is not used.
#define GTEXT_CSS_TYPE_NO_GRAMMAR 0x04u ///< The pin names it and gives no grammar, and the engine does not implement it.
#define GTEXT_CSS_TYPE_SCOPED 0x08u     ///< The pin defines it inside one property (`scope`).
/** @} */

/** @brief What the pin says of a value type or function. */
typedef struct {
  const char * name;              ///< `length-percentage`, `rgb()`: without the angle brackets.
  size_t name_length;             ///< Its length.
  const char * grammar;           ///< Its grammar; NULL when it has none.
  GTEXT_CSS_Property_Id scope;    ///< The property that defines it, or 0 for a global type (the pin's properties only).
  const char * source;            ///< The pin's file.
  uint32_t flags;                 ///< GTEXT_CSS_TYPE_* bits.
} GTEXT_CSS_Type_Info;

/** @brief What the pin says of a selector. */
typedef struct {
  const char * name;              ///< `:hover`, `::before`, `:is()`.
  size_t name_length;             ///< Its length.
  const char * syntax;            ///< The pin's syntax text (not a Value Definition Syntax grammar), or NULL.
  const char * source;            ///< The pin's file.
} GTEXT_CSS_Selector_Info;

/* ---- options and the builder --------------------------------------------- */

/**
 * @struct GTEXT_CSS_Registry_Options
 * @brief The allocator and limits of a registry. A zeroed struct is the default; 0 never means unlimited.
 */
typedef struct {
  /** The allocator of everything the registry makes, or NULL for the default. The struct is copied; its `ctx` must outlive the registry. */
  const GTEXT_Allocator * allocator;
  /** The options a grammar is parsed and linked under (NULL for the defaults). Copied. Its allocator is replaced by the registry's. */
  const GTEXT_CSS_Grammar_Options * grammar;
  /** The options registration validates the initial value under (NULL for the defaults). Copied. Its allocator is replaced by the registry's. */
  const GTEXT_CSS_Match_Options * match;
  /** Most properties, aliases and shorthands the caller registers. Default 65,536; at most 2^24. One more is GTEXT_CSS_E_LIMIT_REGISTRY. */
  size_t max_registrations;
  /** Longest property name, in bytes. Default 256; at most 4096. One more is GTEXT_CSS_E_LIMIT_REGISTRY. */
  size_t max_name_bytes;
  /** Longest initial value text, in bytes. Default 65,536; at most 2^24. One more is GTEXT_CSS_E_LIMIT_REGISTRY.
   *  (A grammar's length is `grammar->max_definition_bytes`: GTEXT_CSS_E_LIMIT_DEFINITION.) */
  size_t max_initial_bytes;
  /** Most longhands of one shorthand. Default 1024; at most 65,536. One more is GTEXT_CSS_E_LIMIT_REGISTRY. */
  size_t max_longhands;
  /** Most nodes the compiled grammars of all the registrations may have together. A compiled grammar carries the grammars it names, so
   *  properties that each name the one before hold the square of the chain's length; this bounds the memory of that (about 80 bytes a
   *  node). Default 2,097,152; at most 2^28. A registration past it is GTEXT_CSS_E_LIMIT_REGISTRY, with a `detail` that says so. */
  size_t max_grammar_nodes;
} GTEXT_CSS_Registry_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Registry_Options gtext_css_registry_options_default(void);

/**
 * @struct GTEXT_CSS_Registry_Result
 * @brief What a registration, a freeze or a preparation reports besides its status. Plain data; filled whatever happens.
 */
typedef struct {
  GTEXT_CSS_Status status;   ///< What the call returned.
  const char * message;      ///< A static description; never NULL after a call.
  char name[96];             ///< The name concerned (a property, a longhand, a reference), cut to fit; empty if none.
  GTEXT_CSS_Property_Id id;  ///< A successful registration: the property made. A GTEXT_CSS_E_EXISTS: the property that is already there.
  const char * detail;       ///< A static sentence saying which rule a GTEXT_CSS_E_INVALID broke (the name, the initial value ...), or NULL.
  GTEXT_CSS_Grammar_Result grammar; ///< A grammar that failed to parse or link: where and what (name of the missing type, ...).
} GTEXT_CSS_Registry_Result;

/**
 * @brief Make an empty builder over the generated layer.
 *
 * @param options NULL for the defaults.
 * @param out Receives the builder; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID (a NULL `out`, or an allocator that cannot allocate) or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_registry_builder_new(const GTEXT_CSS_Registry_Options * options,
    GTEXT_CSS_Registry_Builder ** out);

/** @brief Free a builder and everything registered in it; NULL is ignored. (gtext_css_registry_freeze() consumes the builder.) */
GTEXT_API void gtext_css_registry_builder_free(GTEXT_CSS_Registry_Builder * builder);

/**
 * @struct GTEXT_CSS_Property_Def
 * @brief A property a caller registers. A zeroed struct plus `name`, `initial` and `grammar` is a valid definition.
 */
typedef struct {
  const char * name;               ///< ASCII letters, digits, `-` and `_`, not beginning `--` (custom properties are `@property`'s); case-insensitive, stored lower case. Required.
  bool inherited;                  ///< Whether it is inherited.
  const char * initial;            ///< The initial value; required. It must parse against `grammar`, and be a concrete value.
  const char * grammar;            ///< The grammar of a value, in the Value Definition Syntax. Required.
  GTEXT_CSS_Computed computed;     ///< The class of its computed value.
  GTEXT_CSS_Animation animation;   ///< How it interpolates.
  bool quirk_unitless_length;      ///< Whether a unitless length is accepted in quirks mode.
  bool override;                   ///< Say that this redefines a property that exists (generated or registered). Without it, that is GTEXT_CSS_E_EXISTS.
} GTEXT_CSS_Property_Def;

/**
 * @brief Register a property.
 *
 * Validated now: the name, the grammar (parsed and linked against the registry as it stands), and the initial value
 * (matched against the grammar, with no `var()` in it and not a CSS-wide keyword). Nothing is registered on a failure.
 *
 * @param builder The builder. Not NULL.
 * @param def The definition; strings are copied. Not NULL.
 * @param result NULL, or filled (the id made, or what went wrong).
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, a bad name, an initial value that does not match or is not
 *   concrete); GTEXT_CSS_E_EXISTS (the name is taken and `override` is not set); GTEXT_CSS_E_GRAMMAR,
 *   GTEXT_CSS_E_REFERENCE, GTEXT_CSS_E_LEFT_RECURSION or a grammar limit (the result's `grammar` says where);
 *   GTEXT_CSS_E_LIMIT_WORK (the initial value could not be checked in the match limits);
 *   GTEXT_CSS_E_LIMIT_REGISTRY; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_registry_register_property(GTEXT_CSS_Registry_Builder * builder,
    const GTEXT_CSS_Property_Def * def, GTEXT_CSS_Registry_Result * result);

/**
 * @brief Register a name for a property that exists (the generated layer's aliases are of this kind).
 *
 * @param builder The builder. Not NULL.
 * @param alias The new name (as for GTEXT_CSS_Property_Def::name).
 * @param target The property it names; it may itself be an alias, which is followed.
 * @param override As for GTEXT_CSS_Property_Def.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_EXISTS; GTEXT_CSS_E_REFERENCE (no such target; the result names it);
 *   GTEXT_CSS_E_LIMIT_REGISTRY; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_registry_register_alias(GTEXT_CSS_Registry_Builder * builder,
    const char * alias, const char * target, bool override, GTEXT_CSS_Registry_Result * result);

/**
 * @brief One longhand a shorthand's expansion produced (for the hook below).
 */
typedef struct {
  void * user; ///< Passed back unchanged.
  /** Receives the value of the longhand at `index` of the shorthand's list. Return anything but GTEXT_CSS_OK to stop the expansion with that status. */
  GTEXT_CSS_Status (*emit)(void * user, size_t index, const GTEXT_CSS_Value * value);
} GTEXT_CSS_Longhand_Sink;

/**
 * @brief The hook that expands a shorthand's typed value into its longhands' values.
 *
 * The registry stores it and hands it back (gtext_css_registry_shorthand_hook()); the cascade layer calls it. It is
 * given the value the shorthand's grammar produced and a sink for the longhands, in the order of the list.
 */
typedef GTEXT_CSS_Status (*GTEXT_CSS_Shorthand_Hook)(void * user, const GTEXT_CSS_Value * value,
    const GTEXT_CSS_Longhand_Sink * sink);

/**
 * @struct GTEXT_CSS_Shorthand_Def
 * @brief A shorthand a caller registers.
 */
typedef struct {
  const char * name;                    ///< As for GTEXT_CSS_Property_Def::name. Required.
  const char * grammar;                 ///< The grammar of the shorthand's value. Required.
  const char * const * longhands;       ///< The names of its longhands, which must exist. Required; at least one.
  size_t longhand_count;                ///< Their number (at most `max_longhands`).
  GTEXT_CSS_Shorthand_Hook hook;        ///< The expansion; may be NULL where the grammar is all a consumer wants.
  void * hook_user;                     ///< Passed to the hook.
  bool override;                        ///< As for GTEXT_CSS_Property_Def.
} GTEXT_CSS_Shorthand_Def;

/**
 * @brief Register a shorthand.
 *
 * The shorthand has no initial value and no inheritance of its own: its `inherited` is derived from its longhands, and its
 * value is parsed against `grammar`, as the generated shorthands' are. Validated as gtext_css_registry_register_property() is,
 * and every longhand must exist (GTEXT_CSS_E_REFERENCE, naming it).
 *
 * @return As gtext_css_registry_register_property().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_registry_register_shorthand(GTEXT_CSS_Registry_Builder * builder,
    const GTEXT_CSS_Shorthand_Def * def, GTEXT_CSS_Registry_Result * result);

/**
 * @struct GTEXT_CSS_Custom_Def
 * @brief A custom property a caller registers, as `CSS.registerProperty()` is given it (CSS Properties and Values API 1, 4.1).
 *
 * A custom property (`--name`) is not a property of the registry: the registry has the one pattern `--*`. A registration gives a name the syntax its
 * value is checked against when the style is computed, whether it inherits, and an initial value. It changes nothing at parse time ("Registering a
 * custom property must not affect the cascade in any way"); css_computed.h computes with it.
 */
typedef struct {
  const char * name;         ///< `--name`, case kept (custom property names are case-sensitive); at least one character after the dashes. Required.
  const char * syntax;       ///< A syntax string (`<length> | auto`, `<color>#`, `*`); NULL is `*`.
  bool inherits;             ///< Whether the property inherits. (`registerProperty()` requires it to be said; there is no default here.)
  const char * initial_value; ///< The initial value as CSS text; NULL for none (then the syntax has to be `*`, and the initial value is the guaranteed-invalid
                             ///< value). For a syntax other than `*` it has to be a value of the syntax that can be computed without an element (`5px`, `1in`,
                             ///< `10vw`; not `3em`, `currentcolor` or `var(--x)`).
} GTEXT_CSS_Custom_Def;

/** @brief What the registry holds of a registered custom property. The strings are owned by the registry. */
typedef struct {
  const char * name;         ///< `--name`.
  size_t name_length;
  const char * syntax;       ///< The syntax string as it was given (`*` when none was).
  size_t syntax_length;
  bool universal;            ///< The syntax is `*`.
  bool inherits;
  bool has_initial;          ///< An initial value was given.
  const char * initial_value; ///< Its text with the white space at its ends trimmed; NULL when there is none.
  size_t initial_length;
  size_t component_count;    ///< The components of the syntax (0 for `*`).
} GTEXT_CSS_Custom_Info;

/**
 * @brief Register a custom property.
 *
 * Validated as `CSS.registerProperty()` validates (Properties and Values API 1, 4.1, "register a custom property"): the name must be a custom property
 * name string, the syntax a syntax string, a name that is registered already is an error (`InvalidModificationError` there, GTEXT_CSS_E_EXISTS here), and the
 * initial value, if given, must be a value of the syntax that is computationally independent; a syntax other than `*` with no initial value is an error. Each
 * component of the syntax is compiled here. Nothing is registered on a failure.
 *
 * @param builder The builder. Not NULL.
 * @param def The definition; strings are copied. Not NULL.
 * @param result NULL, or filled (`detail` says which rule a GTEXT_CSS_E_INVALID broke).
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_EXISTS; GTEXT_CSS_E_LIMIT_REGISTRY; GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_WORK (the initial value could not
 *   be checked in the match limits).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_registry_register_custom(GTEXT_CSS_Registry_Builder * builder, const GTEXT_CSS_Custom_Def * def,
    GTEXT_CSS_Registry_Result * result);

/**
 * @brief Freeze a builder into a registry.
 *
 * Every registered grammar is compiled now, and so is every property of the pin when anything has been overridden (so that a
 * redefinition made after a grammar that names it, or an override of a property that the pin's own grammars name, cannot leave a
 * grammar unable to link unnoticed). On success the builder is consumed and must not be used or freed. On failure the builder
 * is still a builder: it keeps every registration, nothing the attempt compiled is remembered (a later registration is taken into
 * account by the next freeze), and the result names the property to blame, with `detail` saying which kind of failure it was.
 *
 * @param builder The builder. Not NULL.
 * @param out Receives the registry; written only on success. Not NULL.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; the failure of a registered grammar that no longer links.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_registry_freeze(GTEXT_CSS_Registry_Builder * builder,
    GTEXT_CSS_Registry ** out, GTEXT_CSS_Registry_Result * result);

/**
 * @brief A frozen registry of the generated layer alone (no registrations): a builder made, frozen and consumed in one call.
 *
 * @param options NULL for the defaults.
 * @param out Receives the registry; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_registry_new(const GTEXT_CSS_Registry_Options * options, GTEXT_CSS_Registry ** out);

/** @brief Free a registry, every grammar it compiled and every registration; NULL is ignored. */
GTEXT_API void gtext_css_registry_free(GTEXT_CSS_Registry * registry);

/**
 * @brief Compile every grammar now.
 *
 * Afterward the registry does no locking on any path. A grammar the pin leaves unresolved, or that cannot be linked, is not
 * an error here: it stays unable to match (gtext_css_registry_property_state()).
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM (the call may be repeated).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_registry_prepare(GTEXT_CSS_Registry * registry);

/* ---- properties ----------------------------------------------------------- */

/** @brief How many properties the pin has (aliases and name-only entries included). */
GTEXT_API size_t gtext_css_registry_generated_property_count(void);

/** @brief How many properties the registry has: the pin's and the caller's. NULL is 0. */
GTEXT_API size_t gtext_css_registry_property_count(const GTEXT_CSS_Registry * registry);

/**
 * @brief The property with this name.
 *
 * Names are ASCII case-insensitive, except that a name beginning `--` is a custom property: it is looked up
 * case-sensitively and, if the registry does not have it by that name, is the pattern `--*` of the pin (css-variables).
 * The caller's registration of a name is found before the pin's. An alias is found as itself (see `alias_of`).
 *
 * @param registry The registry.
 * @param name The name; need not be terminated.
 * @param length Its length.
 * @return The id, or GTEXT_CSS_PROPERTY_NONE for an unknown name (a vendor prefix the pin does not list among them).
 */
GTEXT_API GTEXT_CSS_Property_Id gtext_css_registry_find_property(const GTEXT_CSS_Registry * registry,
    const char * name, size_t length);

/** @brief The property an alias names, following a chain; the property itself otherwise; 0 for an id that is not one. */
GTEXT_API GTEXT_CSS_Property_Id gtext_css_registry_resolve_alias(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id);

/**
 * @brief What the registry knows of a property.
 *
 * @param registry The registry.
 * @param id A property of it.
 * @param info Receives it; written only on success. Not NULL.
 * @return true, or false for an id it does not have.
 */
GTEXT_API bool gtext_css_registry_property_info(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id,
    GTEXT_CSS_Property_Info * info);

/** @brief The `index`th longhand of a shorthand; 0 for an index it does not have. */
GTEXT_API GTEXT_CSS_Property_Id gtext_css_registry_longhand(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id, size_t index);

/** @brief The `index`th property a shorthand resets without it being a longhand; 0 for an index it does not have. */
GTEXT_API GTEXT_CSS_Property_Id gtext_css_registry_reset_longhand(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id, size_t index);

/** @brief The `index`th name of the property's CSSStyleDeclaration attributes (`marginTop`); NULL for an index it does not have. */
GTEXT_API const char * gtext_css_registry_style_name(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id, size_t index);

/** @brief How many members the logical property group of this name has; 0 for a name that is not one. */
GTEXT_API size_t gtext_css_registry_logical_group_size(const char * group, size_t length);

/** @brief The `index`th member (the pin's, in the sorted order of their names, or the caller's override of one) of a logical group; 0 for an index it does not have. */
GTEXT_API GTEXT_CSS_Property_Id gtext_css_registry_logical_group_member(const GTEXT_CSS_Registry * registry, const char * group, size_t length, size_t index);

/** @brief The shorthand hook and user pointer of a shorthand registered with one; false for any other property. */
GTEXT_API bool gtext_css_registry_shorthand_hook(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id,
    GTEXT_CSS_Shorthand_Hook * hook, void ** user);

/** @brief What became of a property's grammar. */
typedef enum {
  GTEXT_CSS_GRAMMAR_NOT_COMPILED = 0, ///< Not asked for yet (the registry is lazy).
  GTEXT_CSS_GRAMMAR_OK,               ///< Compiled and linked.
  GTEXT_CSS_GRAMMAR_NONE,             ///< The property has no grammar (GTEXT_CSS_PROP_NO_GRAMMAR).
  GTEXT_CSS_GRAMMAR_UNRESOLVED,       ///< The grammar names a type nothing resolves (the pin's gap; GTEXT_CSS_E_REFERENCE).
  GTEXT_CSS_GRAMMAR_FAILED            ///< Any other failure to compile it.
} GTEXT_CSS_Grammar_State;

/**
 * @brief The state of a property's grammar, compiling it first if it has not been (an alias reports its target's).
 *
 * @param registry The registry. Not NULL.
 * @param id The property.
 * @param state Receives the state. Not NULL.
 * @param grammar_result NULL, or receives the compile's report (the missing type is in its `name`).
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (an id the registry does not have); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_registry_property_state(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id,
    GTEXT_CSS_Grammar_State * state, GTEXT_CSS_Grammar_Result * grammar_result);

/** @brief How many overrides the builder's registrations made. NULL is 0. */
GTEXT_API size_t gtext_css_registry_override_count(const GTEXT_CSS_Registry * registry);

/**
 * @brief The `index`th override, in the order they were registered.
 *
 * @param registry The registry.
 * @param index Its place.
 * @param id Receives the property that overrides (NULL if not wanted).
 * @param overridden Receives the property it replaced (NULL if not wanted): a generated one, or an earlier registration.
 * @return true, or false for an index it does not have.
 */
GTEXT_API bool gtext_css_registry_override_at(const GTEXT_CSS_Registry * registry, size_t index,
    GTEXT_CSS_Property_Id * id, GTEXT_CSS_Property_Id * overridden);

/** @brief How many custom properties were registered (gtext_css_registry_register_custom()). NULL is 0. */
GTEXT_API size_t gtext_css_registry_custom_count(const GTEXT_CSS_Registry * registry);

/** @brief The `index`th registered custom property, in the order they were registered; false for an index it does not have. */
GTEXT_API bool gtext_css_registry_custom_at(const GTEXT_CSS_Registry * registry, size_t index, GTEXT_CSS_Custom_Info * info);

/** @brief The registered custom property with this name (case-sensitive); false if there is none. */
GTEXT_API bool gtext_css_registry_find_custom(const GTEXT_CSS_Registry * registry, const char * name, size_t length, GTEXT_CSS_Custom_Info * info);

/* ---- at-rules and descriptors (the generated layer) ----------------------- */

/** @brief How many at-rules the pin has. */
GTEXT_API size_t gtext_css_registry_at_rule_count(void);

/** @brief The at-rule with this name (`@font-face`, ASCII case-insensitive); 0 if the pin has none. */
GTEXT_API GTEXT_CSS_At_Rule_Id gtext_css_registry_find_at_rule(const char * name, size_t length);

/** @brief What the pin says of an at-rule; false for an id it does not have. */
GTEXT_API bool gtext_css_registry_at_rule_info(GTEXT_CSS_At_Rule_Id id, GTEXT_CSS_At_Rule_Info * info);

/** @brief The `index`th descriptor of an at-rule (sorted by name); 0 for an index it does not have. */
GTEXT_API GTEXT_CSS_Descriptor_Id gtext_css_registry_descriptor_at(GTEXT_CSS_At_Rule_Id at_rule, size_t index);

/** @brief The descriptor of an at-rule with this name (ASCII case-insensitive); 0 if there is none. */
GTEXT_API GTEXT_CSS_Descriptor_Id gtext_css_registry_find_descriptor(GTEXT_CSS_At_Rule_Id at_rule, const char * name, size_t length);

/** @brief What the pin says of a descriptor; false for an id it does not have. */
GTEXT_API bool gtext_css_registry_descriptor_info(GTEXT_CSS_Descriptor_Id id, GTEXT_CSS_Descriptor_Info * info);

/** @brief How many descriptors the pin has in all. */
GTEXT_API size_t gtext_css_registry_descriptor_count(void);

/* ---- types, functions, selectors, keywords (the generated layer) ---------- */

/** @brief How many value types and functions the pin has (the global and the property-scoped). */
GTEXT_API size_t gtext_css_registry_type_count(void);

/** @brief The `index`th type or function, in the sorted order of their names (scoped ones after the global one of the same name); false past the end. */
GTEXT_API bool gtext_css_registry_type_at(size_t index, GTEXT_CSS_Type_Info * info);

/**
 * @brief The type or function with this name.
 *
 * @param name `length-percentage`, `rgb()`; without angle brackets; case-sensitive (`CDO-token`).
 * @param length Its length.
 * @param scope The property whose scope is searched first (0 for none): a type the pin nests in a property is found for that property
 *   only, and the global type otherwise.
 * @param info Receives it (NULL if not wanted).
 * @return true, or false if there is none.
 */
GTEXT_API bool gtext_css_registry_find_type(const char * name, size_t length, GTEXT_CSS_Property_Id scope,
    GTEXT_CSS_Type_Info * info);

/**
 * @brief Compile the grammar of the `index`th type or function of the pin (by itself, in the scope the pin gives it), and report the state.
 *
 * The check that every grammar of the pin compiles and links against the generated resolver is this call over every index
 * (and gtext_css_registry_property_state() over every property, gtext_css_registry_descriptor_state() over every descriptor).
 * A type the engine implements in code, or the pin gives no grammar, is GTEXT_CSS_GRAMMAR_NONE.
 *
 * @param registry The registry. Not NULL.
 * @param index As for gtext_css_registry_type_at().
 * @param state Receives the state. Not NULL.
 * @param grammar_result NULL, or receives the compile's report (a missing type is in its `name`).
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (an index past the end); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_registry_type_state(const GTEXT_CSS_Registry * registry, size_t index,
    GTEXT_CSS_Grammar_State * state, GTEXT_CSS_Grammar_Result * grammar_result);

/** @brief As gtext_css_registry_type_state(), for a descriptor of the generated layer (GTEXT_CSS_E_INVALID for an id it does not have). */
GTEXT_API GTEXT_CSS_Status gtext_css_registry_descriptor_state(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Descriptor_Id id,
    GTEXT_CSS_Grammar_State * state, GTEXT_CSS_Grammar_Result * grammar_result);

/** @brief How many selectors the pin has. */
GTEXT_API size_t gtext_css_registry_selector_count(void);

/** @brief The `index`th selector (sorted by name); false past the end. */
GTEXT_API bool gtext_css_registry_selector_at(size_t index, GTEXT_CSS_Selector_Info * info);

/** @brief The selector with this name (ASCII case-insensitive); false if there is none. */
GTEXT_API bool gtext_css_registry_find_selector(const char * name, size_t length, GTEXT_CSS_Selector_Info * info);

/** @brief How many keywords the atom table of the pin has (every keyword of every grammar, lower case, once). */
GTEXT_API size_t gtext_css_registry_keyword_count(void);

/** @brief The `index`th keyword in sorted order; NULL past the end. The id of a keyword is its index plus one. */
GTEXT_API const char * gtext_css_registry_keyword_at(size_t index, size_t * length);

/** @brief The id (index plus one) of a keyword, ASCII case-insensitive; 0 if no grammar of the pin has it. */
GTEXT_API uint32_t gtext_css_registry_keyword_lookup(const char * name, size_t length);

/* ---- the units of the pin -------------------------------------------------- */

/** @brief How many units the pin's Values definitions list (gtext_css_unit_lookup() must know each of them). */
GTEXT_API size_t gtext_css_registry_pin_unit_count(void);

/** @brief The `index`th unit of the pin: its spelling, and what it measures (`length`, `angle`, `time`, `frequency`, `resolution`). */
GTEXT_API bool gtext_css_registry_pin_unit_at(size_t index, const char ** name, const char ** measures);

/** @brief How many functions the pin's Values definitions list. */
GTEXT_API size_t gtext_css_registry_pin_function_count(void);

/** @brief The `index`th function of those definitions (`calc()`); NULL past the end. */
GTEXT_API const char * gtext_css_registry_pin_function_at(size_t index);

/* ---- the pin ---------------------------------------------------------------- */

/** @brief The commit of w3c/webref (branch `curated`) the generated layer was written from. Static. */
GTEXT_API const char * gtext_css_registry_pin_commit(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_REGISTRY_H
