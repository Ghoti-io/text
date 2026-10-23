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
 * @file css_value.h
 * @brief CSS typed values: the value model, the units of Values 4 and 5, the
 *   keyword atoms, and specified-value serialization (CSSOM).
 *
 * A component value (css_core.h) is what was written. A @ref GTEXT_CSS_Value
 * is what a grammar (css_grammar.h) decided it means: a keyword, a number with
 * a unit, a list, a function. Every layer above syntax (math, colors, the
 * property registry, computed values, the cascade) works on this one model.
 *
 * ## Ownership
 *
 * A value is immutable and is owned by a @ref GTEXT_CSS_Value_Set, which is
 * freed in one call. Every pointer in a value (its text, its children) points
 * into memory the set owns, and nothing in a value points at the component
 * values it was read from, so a set may outlive the tree that was matched. A
 * set is read-only after the match and may be read from any number of threads.
 *
 * ## Shape
 *
 * A match produces at most one value for a node of the grammar:
 *
 * - a terminal (a keyword, `<length>`, `<string>` ...) is one value;
 * - a juxtaposition (`a b`) is a list with the space separator of the values
 *   its present items produced, in grammar order; an `a / b` or `a , b` whose
 *   segments cannot be empty is a list with the slash or the comma separator;
 * - `||` and `&&` are lists of the members that are present, **in grammar
 *   order** whatever order the input had, the absent ones omitted;
 * - a multiplier whose largest count is more than one (`*`, `+`, `{1,4}`, `#`)
 *   is a list of what each repetition produced (comma separated for `#`), and
 *   `?` is the value itself or nothing; a multiplier on a multiplier is both, as written
 *   (`[ a* ]+` is a list of lists, `[ a? ]*` a list of the `a`s);
 * - `name( ... )` is a function whose children are what the arguments
 *   produced; `{ ... }` and `( ... )` blocks are block values likewise;
 * - `|` and `[ ]` add no node of their own.
 *
 * Every value records the grammar slot that matched it (an index that
 * gtext_css_compiled_slot_text() turns back into the grammar's own text), the
 * component value and span it was read from, and an interpolation class, so
 * that animation can be added without changing the representation.
 *
 * ## Numbers and units
 *
 * `number` is the double the token held (correctly rounded by the math
 * library). An absolute unit converts to its canonical unit by the fixed ratios
 * of Values 4 section 6 in IEEE double, by gtext_css_unit_to_canonical(); a
 * relative unit (font-relative, viewport, container) is never resolved here:
 * what it is relative to is the environment, which belongs to the computed-value
 * layer.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_VALUE_H
#define GHOTI_IO_GTEXT_CSS_CSS_VALUE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_serialize.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- atoms ------------------------------------------------------------ */

/** @brief An interned keyword: 0 is none, and two keywords that differ only in
 *  ASCII case are the same atom. */
typedef uint32_t GTEXT_CSS_Atom;

/** @brief The atom of nothing. */
#define GTEXT_CSS_ATOM_NONE ((GTEXT_CSS_Atom)0)

/** @brief A table of keyword atoms (the struct is private). */
typedef struct GTEXT_CSS_Atom_Table GTEXT_CSS_Atom_Table;

/**
 * @brief Make an empty atom table.
 *
 * @param allocator NULL for the default.
 * @param out Receives it; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_atom_table_new(const GTEXT_Allocator * allocator,
    GTEXT_CSS_Atom_Table ** out);

/** @brief Free a table and every name in it; NULL is ignored. */
GTEXT_API void gtext_css_atom_table_free(GTEXT_CSS_Atom_Table * table);

/**
 * @brief Bound a table: the most atoms and the most bytes of spelling (each NUL counted).
 *
 * The defaults are 16,777,216 atoms and 1 GiB, which are also the most a limit can be
 * (an atom is a 32-bit number and so is an offset into the spellings); a larger value is
 * that maximum and 0 selects the default. A table that holds more than a new limit
 * keeps what it has and refuses more.
 *
 * @param table The table. Not NULL.
 * @param max_atoms The most atoms, or 0.
 * @param max_bytes The most bytes of spelling, or 0.
 * @return GTEXT_CSS_OK or GTEXT_CSS_E_INVALID.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_atom_table_limits(GTEXT_CSS_Atom_Table * table, size_t max_atoms, size_t max_bytes);

/**
 * @brief The atom of a keyword, made if it is new.
 *
 * Keywords are compared by ASCII case folding, and the spelling kept is that of
 * the first one interned (the grammar's spelling, because a compiled grammar
 * interns its keywords before anything is matched).
 *
 * @param table The table. Not NULL.
 * @param text The keyword; need not be terminated.
 * @param length Its length; 0 is refused.
 * @param out Receives the atom; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID, GTEXT_CSS_E_OOM, or GTEXT_CSS_E_LIMIT_ATOMS when
 *   the table's atom or byte limit is reached (nothing is added).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_atom_intern(GTEXT_CSS_Atom_Table * table,
    const char * text, size_t length, GTEXT_CSS_Atom * out);

/** @brief The atom of a keyword, or GTEXT_CSS_ATOM_NONE if it was never
 *  interned. Never makes one. */
GTEXT_API GTEXT_CSS_Atom gtext_css_atom_lookup(const GTEXT_CSS_Atom_Table * table,
    const char * text, size_t length);

/**
 * @brief The spelling of an atom.
 *
 * @param table The table.
 * @param atom An atom of it.
 * @param length Receives the length (NULL if not wanted).
 * @return A NUL-terminated string owned by the table, or NULL for an atom it
 *   does not hold (GTEXT_CSS_ATOM_NONE included).
 */
GTEXT_API const char * gtext_css_atom_name(const GTEXT_CSS_Atom_Table * table,
    GTEXT_CSS_Atom atom, size_t * length);

/** @brief How many atoms the table holds. NULL is 0. */
GTEXT_API size_t gtext_css_atom_count(const GTEXT_CSS_Atom_Table * table);

/* ---- units ------------------------------------------------------------ */

/** @brief What a unit measures. */
typedef enum {
  GTEXT_CSS_UNIT_TYPE_NONE = 0,   ///< Not a unit.
  GTEXT_CSS_UNIT_TYPE_LENGTH,     ///< `<length>`
  GTEXT_CSS_UNIT_TYPE_ANGLE,      ///< `<angle>`
  GTEXT_CSS_UNIT_TYPE_TIME,       ///< `<time>`
  GTEXT_CSS_UNIT_TYPE_FREQUENCY,  ///< `<frequency>`
  GTEXT_CSS_UNIT_TYPE_RESOLUTION, ///< `<resolution>`
  GTEXT_CSS_UNIT_TYPE_FLEX        ///< `<flex>`
} GTEXT_CSS_Unit_Type;

/**
 * @brief Every unit of CSS Values 4 and 5, by a number that does not change
 *   between releases.
 */
typedef enum {
  GTEXT_CSS_UNIT_NONE = 0, ///< No unit (a plain number); also what an unknown unit has.

  /* absolute lengths (Values 4, 6.2) */
  GTEXT_CSS_UNIT_CM = 1, GTEXT_CSS_UNIT_MM, GTEXT_CSS_UNIT_Q, GTEXT_CSS_UNIT_IN,
  GTEXT_CSS_UNIT_PC, GTEXT_CSS_UNIT_PT, GTEXT_CSS_UNIT_PX,

  /* font-relative lengths (6.1) */
  GTEXT_CSS_UNIT_EM = 8, GTEXT_CSS_UNIT_REM, GTEXT_CSS_UNIT_EX, GTEXT_CSS_UNIT_REX,
  GTEXT_CSS_UNIT_CAP, GTEXT_CSS_UNIT_RCAP, GTEXT_CSS_UNIT_CH, GTEXT_CSS_UNIT_RCH,
  GTEXT_CSS_UNIT_IC, GTEXT_CSS_UNIT_RIC, GTEXT_CSS_UNIT_LH, GTEXT_CSS_UNIT_RLH,

  /* viewport-percentage lengths (6.1.2): the plain, small, large and dynamic forms */
  GTEXT_CSS_UNIT_VW = 20, GTEXT_CSS_UNIT_SVW, GTEXT_CSS_UNIT_LVW, GTEXT_CSS_UNIT_DVW,
  GTEXT_CSS_UNIT_VH, GTEXT_CSS_UNIT_SVH, GTEXT_CSS_UNIT_LVH, GTEXT_CSS_UNIT_DVH,
  GTEXT_CSS_UNIT_VI, GTEXT_CSS_UNIT_SVI, GTEXT_CSS_UNIT_LVI, GTEXT_CSS_UNIT_DVI,
  GTEXT_CSS_UNIT_VB, GTEXT_CSS_UNIT_SVB, GTEXT_CSS_UNIT_LVB, GTEXT_CSS_UNIT_DVB,
  GTEXT_CSS_UNIT_VMIN, GTEXT_CSS_UNIT_SVMIN, GTEXT_CSS_UNIT_LVMIN, GTEXT_CSS_UNIT_DVMIN,
  GTEXT_CSS_UNIT_VMAX, GTEXT_CSS_UNIT_SVMAX, GTEXT_CSS_UNIT_LVMAX, GTEXT_CSS_UNIT_DVMAX,

  /* container query lengths (Containment 3) */
  GTEXT_CSS_UNIT_CQW = 44, GTEXT_CSS_UNIT_CQH, GTEXT_CSS_UNIT_CQI, GTEXT_CSS_UNIT_CQB,
  GTEXT_CSS_UNIT_CQMIN, GTEXT_CSS_UNIT_CQMAX,

  /* angles (6.3) */
  GTEXT_CSS_UNIT_DEG = 50, GTEXT_CSS_UNIT_GRAD, GTEXT_CSS_UNIT_RAD, GTEXT_CSS_UNIT_TURN,
  /* times (6.4) */
  GTEXT_CSS_UNIT_S = 54, GTEXT_CSS_UNIT_MS,
  /* frequencies (6.5) */
  GTEXT_CSS_UNIT_HZ = 56, GTEXT_CSS_UNIT_KHZ,
  /* resolutions (6.6) */
  GTEXT_CSS_UNIT_DPI = 58, GTEXT_CSS_UNIT_DPCM, GTEXT_CSS_UNIT_DPPX, GTEXT_CSS_UNIT_X,
  /* flexible lengths (CSS Grid) */
  GTEXT_CSS_UNIT_FR = 62,

  GTEXT_CSS_UNIT_COUNT ///< The number of units; not a unit.
} GTEXT_CSS_Unit;

/**
 * @brief The unit with this name.
 *
 * @param name The unit as written, ASCII case-insensitive (`PX`, `Q`, `q`,
 *   `kHz`); need not be terminated.
 * @param length Its length.
 * @return The unit, or GTEXT_CSS_UNIT_NONE when the name is not one of the
 *   units of Values 4 and 5.
 */
GTEXT_API GTEXT_CSS_Unit gtext_css_unit_lookup(const char * name, size_t length);

/**
 * @brief The spelling a unit is serialized in: lower case, except `Q`, `Hz`
 *   and `kHz`.
 *
 * @param unit Any value.
 * @return A static string, or NULL for NONE and for anything that is not a unit.
 */
GTEXT_API const char * gtext_css_unit_name(GTEXT_CSS_Unit unit);

/** @brief What a unit measures (GTEXT_CSS_UNIT_TYPE_NONE for NONE or a non-unit). */
GTEXT_API GTEXT_CSS_Unit_Type gtext_css_unit_type(GTEXT_CSS_Unit unit);

/** @brief Whether a unit has a fixed ratio to its canonical unit: every angle,
 *  time, frequency, resolution and flex unit, and the absolute lengths. The
 *  font-relative, viewport and container lengths are not. */
GTEXT_API bool gtext_css_unit_is_absolute(GTEXT_CSS_Unit unit);

/** @brief The canonical unit of a unit's type (px, deg, s, Hz, dppx, fr), or
 *  NONE for NONE or a non-unit. A relative length's is px too (the unit that
 *  lengths are serialized in once computed): it names the type's unit, and
 *  gtext_css_unit_to_canonical() does not convert a relative unit to it. */
GTEXT_API GTEXT_CSS_Unit gtext_css_unit_canonical(GTEXT_CSS_Unit unit);

/**
 * @brief Convert a quantity of an absolute unit to its canonical unit.
 *
 * The ratios are the draft's: 1in = 2.54cm = 25.4mm = 101.6Q = 72pt = 6pc =
 * 96px; 1turn = 360deg = 400grad = 2 pi rad; 1s = 1000ms; 1kHz = 1000Hz; 1dppx
 * = 1x = 96dpi = 96/2.54 dpcm. Each is applied as `value * numerator /
 * denominator` in IEEE double, so the result is the same on every host.
 *
 * @param unit The unit.
 * @param value The quantity.
 * @param out Receives the quantity in the canonical unit; written only on
 *   success. Not NULL.
 * @return true, or false for a relative unit, a non-unit or a NULL `out`.
 */
GTEXT_API bool gtext_css_unit_to_canonical(GTEXT_CSS_Unit unit, double value, double * out);

/* ---- values ----------------------------------------------------------- */

/** @brief What a typed value is. */
typedef enum {
  GTEXT_CSS_VAL_KEYWORD = 0, ///< A keyword (`text` is the grammar's spelling for a
                             ///< keyword the grammar names, the input's for `<ident>`; `atom` is set when the grammar names it).
  GTEXT_CSS_VAL_CUSTOM_IDENT, ///< `<custom-ident>`: `text`, case preserved.
  GTEXT_CSS_VAL_DASHED_IDENT, ///< `<dashed-ident>`: `text`, case preserved, begins `--`.
  GTEXT_CSS_VAL_STRING,       ///< `<string>`: `text` is the string's value.
  GTEXT_CSS_VAL_URL,          ///< `<url>`: `text` is the URL; `children` any url modifiers.
  GTEXT_CSS_VAL_NUMBER,       ///< `<number>`: `number`.
  GTEXT_CSS_VAL_INTEGER,      ///< `<integer>`: `number`, an integer.
  GTEXT_CSS_VAL_PERCENTAGE,   ///< `<percentage>`: `number` (50 for `50%`).
  GTEXT_CSS_VAL_DIMENSION,    ///< A number and a unit: `number`, `unit`; an unknown unit
                              ///< (`<dimension>`) has unit NONE and its spelling in `text`.
  GTEXT_CSS_VAL_RATIO,        ///< `<ratio>`: `number` over `number2`; written with both ("<ratio> is always serialized with
                              ///< both components"). A component that is a math function makes `children` its two
                              ///< components (a number or the function) and sets GTEXT_CSS_VFLAG_MATH.
  GTEXT_CSS_VAL_ZERO,         ///< `<zero>`: the number 0.
  GTEXT_CSS_VAL_LIST,         ///< A list: `children`, joined by `separator`.
  GTEXT_CSS_VAL_FUNCTION,     ///< A function: `text` is its name, `children` its arguments' values.
  GTEXT_CSS_VAL_UNICODE_RANGE, ///< `<urange>`: `range_start` to `range_end`.
  GTEXT_CSS_VAL_PENDING,      ///< A value with `var()`, `env()` or `attr()` in it: valid
                              ///< now, and meaningful only after substitution. `text` is its
                              ///< serialization.
  GTEXT_CSS_VAL_UNPARSED,     ///< `<any-value>`, `<declaration-value>` and the like: `text`
                              ///< is the serialization of the component values.
  GTEXT_CSS_VAL_TOKEN,        ///< A preserved token (`<delim-token>`, a literal `/`, ...): `text` is its serialization.
  GTEXT_CSS_VAL_BLOCK,        ///< A `{}`, `[]` or `()` block of a grammar: `children`; `text` is the
                              ///< opening character.
  GTEXT_CSS_VAL_KIND_COUNT    ///< The number of kinds; not a kind.
} GTEXT_CSS_Val_Kind;

/** @brief How a value interpolates (animation is added in a later layer; the
 *  class is kept so that it can be without changing the representation). */
typedef enum {
  GTEXT_CSS_INTERP_DISCRETE = 0, ///< Flips at 50%: keywords, identifiers, strings, urls, and the frequency, resolution and flex dimensions.
  GTEXT_CSS_INTERP_NUMBER,       ///< `<number>`, `<integer>`, `<ratio>`.
  GTEXT_CSS_INTERP_LENGTH,       ///< `<length>`.
  GTEXT_CSS_INTERP_PERCENTAGE,   ///< `<percentage>`.
  GTEXT_CSS_INTERP_ANGLE,        ///< `<angle>`.
  GTEXT_CSS_INTERP_TIME,         ///< `<time>`.
  GTEXT_CSS_INTERP_COLOR,        ///< Reserved for colors (a later layer).
  GTEXT_CSS_INTERP_LIST          ///< A list, interpolated item by item.
} GTEXT_CSS_Interp;

/** @brief How a list's items are joined when it is written. */
typedef enum {
  GTEXT_CSS_SEP_SPACE = 0, ///< `" "`
  GTEXT_CSS_SEP_COMMA,     ///< `", "`
  GTEXT_CSS_SEP_SLASH      ///< `" / "`
} GTEXT_CSS_List_Separator;

/** @name Value flags
 * Bits of GTEXT_CSS_Value::flags.
 * @{ */
/** A math function (`calc()`, `min()`, ...) that matched the type by name only:
 *  its argument types are checked, and its value clamped to the slot's range at
 *  computed-value time, by the math layer. `range_lo`/`range_hi` are the range. */
#define GTEXT_CSS_VFLAG_MATH 0x01u
/** The unitless `0` that matched `<length>`: 0px. */
#define GTEXT_CSS_VFLAG_ZERO_LENGTH 0x02u
/** A bare number accepted as a length because the parser option for HTML quirks
 *  was set: `unit` is px. */
#define GTEXT_CSS_VFLAG_UNITLESS_LENGTH 0x04u
/** A ratio that was written with a denominator (`16 / 9`). */
#define GTEXT_CSS_VFLAG_RATIO_DENOMINATOR 0x08u
/** The component value was an integer token (a `<number>` that happened to be written as one). */
#define GTEXT_CSS_VFLAG_WRITTEN_INTEGER 0x10u
/** The number's source spelling was inexact (GTEXT_CSS_FLAG_NUMBER_INEXACT). */
#define GTEXT_CSS_VFLAG_INEXACT 0x20u
/** @} */

/** @brief A typed value (the struct below). */
typedef struct GTEXT_CSS_Value GTEXT_CSS_Value;

/**
 * @struct GTEXT_CSS_Value
 * @brief A typed value: immutable, owned by a @ref GTEXT_CSS_Value_Set.
 *
 * Which fields mean something depends on `kind` (the table in the kind's
 * comment); the others are zero.
 */
struct GTEXT_CSS_Value {
  GTEXT_CSS_Val_Kind kind;          ///< What it is.
  GTEXT_CSS_Atom atom;              ///< A keyword the grammar names: its atom.
  double number;                    ///< The number (a ratio's numerator).
  double number2;                   ///< A ratio's denominator.
  GTEXT_CSS_Unit unit;              ///< A dimension's unit.
  GTEXT_CSS_Interp interp;          ///< How it interpolates.
  GTEXT_CSS_List_Separator separator; ///< A list's separator.
  uint32_t flags;                   ///< GTEXT_CSS_VFLAG_* bits.
  uint32_t slot;                    ///< The grammar node that matched it (gtext_css_compiled_slot_text()).
  GTEXT_CSS_Id component;           ///< The id of the component value it was read from, or 0 for a list, a function
                                    ///< or a block made of several (and for a component value with no id).
  GTEXT_CSS_Span span;              ///< The input it covers: the component value's span, or from the first
                                    ///< child's start to the last child's end.
  GTEXT_CSS_String text;            ///< See the kinds; NUL-terminated, owned by the set.
  const GTEXT_CSS_Value * children; ///< A list's items, a function's or a block's contents, a url's modifiers.
  size_t child_count;               ///< Their number.
  uint32_t range_start;             ///< A unicode range's first code point.
  uint32_t range_end;               ///< Its last.
  double range_lo;                  ///< For a math function: the slot's range, lower bound (canonical unit).
  double range_hi;                  ///< Its upper bound.
};

/** @brief A set of typed values and the memory they live in (the struct is private). */
typedef struct GTEXT_CSS_Value_Set GTEXT_CSS_Value_Set;

/**
 * @brief The value a match produced.
 *
 * @param set A set a match returned.
 * @return The root value, or NULL when the grammar matched and produced nothing
 *   (an empty input against `a?`) or the set is NULL.
 */
GTEXT_API const GTEXT_CSS_Value * gtext_css_value_set_root(const GTEXT_CSS_Value_Set * set);

/** @brief How many values the set holds, the root and every descendant. NULL is 0. */
GTEXT_API size_t gtext_css_value_set_count(const GTEXT_CSS_Value_Set * set);

/**
 * @brief Free a set and every value in it.
 *
 * The pointer is dead after the call, as is every pointer into the set. NULL
 * is ignored.
 */
GTEXT_API void gtext_css_value_set_free(GTEXT_CSS_Value_Set * set);

/** @brief The name of a value kind ("keyword", "dimension", "list", ...). A
 *  value that is not a kind returns "unknown". Static. */
GTEXT_API const char * gtext_css_val_kind_name(GTEXT_CSS_Val_Kind kind);

/* ---- substitution ----------------------------------------------------- */

/**
 * @brief Whether component values contain an arbitrary substitution function.
 *
 * A value that holds `var()`, `env()` or `attr()` anywhere, however deep in
 * functions and blocks (the name matched ASCII case-insensitively), cannot be
 * checked against a grammar until it is substituted; a match reports it as a
 * GTEXT_CSS_VAL_PENDING value, valid at parse time as CSS Variables requires.
 *
 * @param values The values; NULL only with a count of 0.
 * @param count Their number.
 * @param allocator NULL for the default; used for the walk's stack (nothing
 *   recurses).
 * @param found Receives the answer; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_has_arbitrary_substitution(
    const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_Allocator * allocator, bool * found);

/* ---- serialization ---------------------------------------------------- */

/**
 * @brief Write a value as CSSOM writes a specified value.
 *
 * - numbers by the shortest digits that round to the value, laid out as CSSOM's
 *   `<number>` is (base ten, no exponent, six decimals at most), through the math
 *   library; a percentage adds `%`, a dimension the unit in its canonical
 *   spelling (gtext_css_unit_name()) and an unknown unit as it was written;
 * - identifiers by CSSOM's "serialize an identifier" and strings by "serialize a
 *   string", through the serializers of css_serialize.h; a keyword in the
 *   grammar's spelling, a custom identifier with its case;
 * - `url("...")`;
 * - list separators `" "`, `", "` and `" / "`, and a list nested in a list
 *   written as it is, with no parentheses;
 * - a function as `name(` its children `)`.
 *
 * Writing a value, parsing the text and matching it again, and writing that,
 * gives the same text (the fixed point) for every value a grammar produced.
 *
 * @param value The value. Not NULL.
 * @param options NULL for the defaults (`numbers` is ignored: numbers are always CSSOM's).
 * @param sink Where the text goes. Not NULL, with a `write`.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM (the walk's stack);
 *   GTEXT_CSS_E_WRITE; GTEXT_CSS_E_LIMIT_OUTPUT.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_serialize_value(const GTEXT_CSS_Value * value,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink);

/**
 * @brief Write a value to a new buffer.
 *
 * @param value The value. Not NULL.
 * @param options NULL for the defaults; its allocator makes the buffer.
 * @param out Receives the text, NUL-terminated (not counted in `out_length`);
 *   release it with gtext_css_serialize_buffer_free(). Written only on success.
 * @param out_length Receives its length. Not NULL.
 * @return As gtext_css_serialize_value(); a failure leaves no buffer.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_serialize_value_buffer(const GTEXT_CSS_Value * value,
    const GTEXT_CSS_Serialize_Options * options, char ** out, size_t * out_length);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_VALUE_H
