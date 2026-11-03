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
 * @brief `MediaList`: the media query list of a style sheet, an `@import` rule and an `@media` rule (CSS Object Model, Media Queries 4).
 *
 * A media list is a **view of its owner**: it holds no list of its own, so it cannot disagree with the sheet or the rule. `mediaText` is the owner's media query list serialized
 * ("serialize a media query list"); setting it replaces the list, and **a text always parses**: a query that is not valid becomes `not all` (Media Queries 4, 3.2), the empty text is
 * the empty list, which matches everything. `appendMedium` and `deleteMedium` compare by serialization. The same list object is returned every time its owner is asked
 * (`sheet.media === sheet.media`); it is reference counted like every object and keeps its owner reachable while it is held.
 *
 * **The text is the text as it was read.** A `<general-enclosed>` that Media Queries 4 keeps unknown (`(a b)`, `foo(x)`) is written as it was written, white space, comments and
 * escapes included, and one that the end of the input closed stays unclosed: `screen and (` reads back as `screen and (`, which is what Chromium and Firefox write. The rule stores the
 * closed text (`screen and ()`), because an unclosed parenthesis in a prelude would take the `{` after it, and keeps the text as read beside it: `mediaText`, `item()`, a rule's
 * `conditionText` and `cssText`, an `@import` rule's `cssText` and `matchMedia().media` write the text as read; `length` counts the queries of the closed text; `appendMedium` and
 * `deleteMedium` compare a medium with the items as they were read (`screen and (` and `screen and ()` are two media); a script's own `matchMedia`-free readers (the style engine,
 * the import request, the event summary) see the closed text, which means the same. Setting the list again, or any other change of the rule's own part, ends the text as read.
 *
 * Setting the text of a sheet's list moves the sheet's version and sends `SHEET_MEDIA_CHANGED`; of a rule's, replaces the rule as a whole (css_cssom_rule.h) and sends `RULE_REPLACED`.
 * A change that leaves the serialization as it was is `UNCHANGED`: no version, no event.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CSSOM_MEDIA_H
#define GHOTI_IO_GTEXT_CSS_CSS_CSSOM_MEDIA_H

#include <ghoti.io/text/css/css_cssom_core.h>
#include <ghoti.io/text/css/css_cssom_rule.h>
#include <ghoti.io/text/css/css_cssom_sheet.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A media list (the struct is private). Reference counted with a plain count: the document's writer thread only. */
typedef struct GTEXT_CSSOM_Media_List GTEXT_CSSOM_Media_List;

/** @brief Take another external reference. @return The list. */
GTEXT_API GTEXT_CSSOM_Media_List * gtext_cssom_media_retain(GTEXT_CSSOM_Media_List * list);

/** @brief Give one back. NULL is ignored. */
GTEXT_API void gtext_cssom_media_release(GTEXT_CSSOM_Media_List * list);

/** @brief `media` of a sheet: its list, returned retained, the same object every time. @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_LIMIT_CSSOM; GTEXT_CSS_E_OOM. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_media(GTEXT_CSSOM_Sheet * sheet, GTEXT_CSSOM_Media_List ** out);

/** @brief `media` of an `@import` or `@media` rule. @return As gtext_cssom_sheet_media(); GTEXT_CSS_E_UNSUPPORTED for another kind. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_rule_media(GTEXT_CSSOM_Rule * rule, GTEXT_CSSOM_Media_List ** out);

/** @brief `mediaText`, in a new buffer (gtext_cssom_buffer_free()). */
GTEXT_API GTEXT_CSS_Status gtext_cssom_media_text(GTEXT_CSSOM_Media_List * list, char ** out, size_t * length);

/**
 * @brief The `mediaText` setter: the list becomes the parse of the text. Atomic. A rule that holds the list is replaced as a whole.
 * @return GTEXT_CSS_OK (`*outcome`: CHANGED or UNCHANGED); GTEXT_CSS_E_REENTRANT; GTEXT_CSS_E_SECURITY and GTEXT_CSS_E_NOT_ALLOWED for a sheet that may not be modified; the limits;
 *   GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_media_set_text(GTEXT_CSSOM_Media_List * list, const char * text, size_t length, GTEXT_CSSOM_Outcome * outcome);

/** @brief `length`: how many queries. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_media_length(GTEXT_CSSOM_Media_List * list, size_t * out);

/** @brief `item(index)`: the query as serialized, in a new buffer; `*is_null` true (and an empty string) past the end. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_media_item(GTEXT_CSSOM_Media_List * list, size_t index, char ** out, size_t * length, bool * is_null);

/**
 * @brief `appendMedium(medium)`: parse `medium` as one query; text that is not one valid query (several, or an error) is **ignored** (`IGNORED_INVALID_VALUE`, as the plan has it: the
 *   draft would append `not all`), and a medium whose serialization is already in the list is not added (`UNCHANGED`). The empty string is ignored.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_media_append(GTEXT_CSSOM_Media_List * list, const char * medium, size_t length, GTEXT_CSSOM_Outcome * outcome);

/** @brief `deleteMedium(medium)`: remove the query whose serialization equals the medium's; GTEXT_CSS_E_NOT_FOUND (NotFoundError) if there is none. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_media_delete(GTEXT_CSSOM_Media_List * list, const char * medium, size_t length, GTEXT_CSSOM_Outcome * outcome);

/* ---- constructed style sheets ---------------------------------------------------------------------------------------------------------------- */

/**
 * @brief `new CSSStyleSheet(options)`: an empty constructed sheet (`constructed` set, no owner node, no parent). `options->media`, `disabled` and `base_url` are the constructor's
 *   `media`, `disabled` and `baseURL`. The result has one external reference of the caller's.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_new_constructed(GTEXT_CSSOM_Document * document, const GTEXT_CSSOM_Sheet_Options * options, GTEXT_CSSOM_Sheet ** out);

/**
 * @brief `replaceSync(text)`: parse the text as a style sheet, **drop its `@import` rules** (a diagnostic each: CSSOM warns, it does not throw), make the rest the sheet's rules and
 *   **detach every old rule, the nested ones too** (`parentStyleSheet` and `parentRule` read NULL at once). Atomic.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_NOT_ALLOWED (the sheet is not constructed, or is being replaced: `disallow modification`); GTEXT_CSS_E_REENTRANT; the limits of a parse; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_replace_sync(GTEXT_CSSOM_Sheet * sheet, const char * text, size_t length, GTEXT_CSSOM_Outcome * outcome);

/**
 * @brief The synchronous steps of `replace(text)`: this begins one (the sheet is not constructed, or one is pending, is GTEXT_CSS_E_NOT_ALLOWED; else `disallow modification` is set and the
 *   pending count rises), and gtext_cssom_sheet_replace_complete() ends it. The promise, and what runs between the two, are the caller's.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_replace_begin(GTEXT_CSSOM_Sheet * sheet);

/** @brief End a replace begun: the rules are set as gtext_cssom_sheet_replace_sync() sets them, and the flag is cleared whether or not it succeeds (a failed parse leaves the rules as they were). */
GTEXT_API GTEXT_CSS_Status gtext_cssom_sheet_replace_complete(GTEXT_CSSOM_Sheet * sheet, const char * text, size_t length, GTEXT_CSSOM_Outcome * outcome);

/** @brief Whether `replace()` is pending (`disallow modification`): `insertRule` and `deleteRule` are then GTEXT_CSS_E_NOT_ALLOWED. */
GTEXT_API bool gtext_cssom_sheet_replace_pending(const GTEXT_CSSOM_Sheet * sheet);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CSSOM_MEDIA_H
