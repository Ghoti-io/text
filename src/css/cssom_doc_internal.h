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
 * What the document sources share (css_cssom_document.h): the tree scope and its two lists, the media query list, and the calls between the files. Private.
 *
 * **Who holds what.** The core holds each registered scope with an internal count; a scope holds its parent scope, each of its owner sheets and each entry of its adopted list with an
 * internal count (a sheet listed twice in an adopted list is held twice). A sheet in a `styleSheets` list points back at its scope weakly (`css_sheet::scope`), so a sheet can be refused a
 * second list and found for its removal. A media query list is held by its callers only; the core links the live ones in creation order, weakly, and a list unlinks itself when it dies.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSSOM_DOC_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSSOM_DOC_INTERNAL_H

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_cssom.h>
#include <ghoti.io/text/macros.h>

#include "cssom_internal.h"
#include "cssom_rule_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GTEXT_CSSOM_Scope css_scope;
typedef struct GTEXT_CSSOM_Media_Query_List css_mql;

struct GTEXT_CSSOM_Scope {
  css_obj obj;
  css_core * home;                    ///< The core of the document the scope is registered with, or NULL (removed, or the document was released).
  uint32_t context;
  const void * handle;
  bool shadow;
  bool is_document;
  css_scope * parent;                 ///< The scope that holds the host (an internal count held), or NULL.
  const void * implicit_root;
  css_sheet ** sheets;                ///< `styleSheets`.
  size_t nsheets, capsheets;
  css_sheet ** adopted;               ///< `adoptedStyleSheets`.
  size_t nadopted, capadopted;
};

css_scope * css_scope_from_obj(css_obj * o);

/// The document's own scope, made with the core.
GTEXT_CSS_Status css_scope_create_document(css_core * core, css_scope ** out);

/// The start of the teardown at the release of the document handle: every registered scope lets go of its home and the core lets go of them.
void css_scope_teardown_all(css_core * core);

/// The scope the sheet is listed in as an owner sheet (weak), or NULL.
css_scope * css_sheet_scope(const css_sheet * sheet);

/// The greatest change stamp among the sheets a scope lists (owner sheets and adopted sheets that take part in its document) with the sheets they import; `*sheets` counts those looked at.
GTEXT_CSS_Status css_scope_stamp(const css_scope * scope, uint64_t * stamp, uint64_t * sheets);

/// Whether the sheet takes part in the document the scope is registered with: it was made by that document.
bool css_scope_owns(const css_scope * scope, const css_sheet * sheet);

/// The registered scopes ordered by context number (ties by creation); a new array the caller frees, or NULL on memory.
css_scope ** css_scopes_by_context(css_core * core, size_t * count);

/* ---- the index (cssom_index.c) ----------------------------------------------------------------------------------------------------------- */

/// Let go of the index and the sheets registered for the user-agent and user origins.
void css_index_teardown(css_core * core);

/// Read the `@property` rules of a live sheet into `*set` (made on the first rule that can hold one; NULL stays NULL for a sheet that has none), in the document's cascade order
/// when called for each sheet in turn (cssom_snapshot.c). The sheets it imports are not read.
GTEXT_CSS_Status css_sheet_collect_properties(css_core * core, css_sheet * sheet, GTEXT_CSS_At_Property_Set ** set);
/// The same for the `@function` rules (css_custom_function.h): the set owns copies of the rules it defines.
GTEXT_CSS_Status css_sheet_collect_functions(css_core * core, css_sheet * sheet, GTEXT_CSS_Function_Set ** set);
/// The same for the `@keyframes` rules (and the group and layer rules that hold them): added to `*set`, made first when it is NULL (cssom_index.c freezes it).
GTEXT_CSS_Status css_sheet_collect_keyframes(css_core * core, css_sheet * sheet, GTEXT_CSS_Atrule_Set ** set);

/// The `@mixin` rules `@apply` may name (css_sheet.h, GTEXT_CSS_Sheet_Options::mixins), read from the live sheets of one tree scope (cssom_snapshot.c): a set of rules borrowed from the live
/// sheets and from the trees of the rules that were read again, which live as long as the scan does.
typedef struct css_mixin_scan css_mixin_scan;

/// A scan that holds nothing yet.
GTEXT_CSS_Status css_mixin_scan_new(css_core * core, css_mixin_scan ** out);

/// Read the `@mixin` rules of a sheet into the scan, after those it has: first those of the sheets it imports (an import is at the top of a sheet), then its own. The layer of an
/// imported sheet's rules is the one they are in there, whatever `layer()` the import has, and the conditions of an import are not read.
GTEXT_CSS_Status css_mixin_scan_sheet(css_mixin_scan * scan, css_sheet * sheet);

/// The set the scan made (frozen; owned by the scan).
const GTEXT_CSS_Function_Set * css_mixin_scan_set(const css_mixin_scan * scan);

void css_mixin_scan_free(css_mixin_scan * scan);

/// The mixins visible to the sheet `target` (cssom_index.c): the `@mixin` rules of the sheets of the tree scope it is in, in the order of the cascade ("The name of a @mixin rule is a tree-scoped
/// name"), or its own and its imports' when it takes part in no document.
GTEXT_CSS_Status css_index_mixin_scan(css_core * core, css_sheet * target, css_mixin_scan ** out);

/// What a snapshot that applies mixins depends on besides its own sheet: the stamp, the structure, the registrations and the conditions the index is built for.
void css_index_dependency(css_core * core, uint64_t out[4]);

/* ---- imports (cssom_snapshot.c, cssom_sheet.c, cssom_insert.c) -------------------------------------------------------------------------- */

/// Ask the document's import callback for the sheet of the `@import` rule `rule`, which is for `sheet`: `*made` is the new sheet (one reference, the callback's) or NULL, with `*pending` true when
/// the callback said so. `*asked` is false when there is no callback; `*nesting_refused` is true when the callbacks were nested too deep to ask. A refusal of a sheet is the caller's to judge.
/// The mutation guard is held for the call.
GTEXT_CSS_Status css_import_ask(css_sheet * sheet, css_rule * rule, bool * asked, css_sheet ** made, bool * pending, bool * nesting_refused);

/// Ask for every `@import` of a sheet that was just made from text and attach what comes (a sheet refused, or a nesting refused, is a diagnostic of the sheet).
GTEXT_CSS_Status css_sheet_ask_imports(css_sheet * sheet);

/// Whether `candidate` can become the sheet an import of `host` holds (`host` may be NULL for a rule that is not in a sheet yet): GTEXT_CSS_OK, else why not.
GTEXT_CSS_Status css_import_check(const css_sheet * host, const css_sheet * candidate);

/// Room for `more` diagnostics of the sheet; false on memory. After it css_sheet_diag_push() cannot fail.
bool css_sheet_diag_reserve(css_sheet * sheet, size_t more);
void css_sheet_diag_push(css_sheet * sheet, GTEXT_CSSOM_Parse_Diag_Kind kind, GTEXT_CSS_Span span, const char * reference);

/// Attach the sheet to the rule without events or checks (the caller checked with css_import_check() and nothing below can fail).
void css_import_attach(css_rule * rule, css_sheet * sheet);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSSOM_DOC_INTERNAL_H
