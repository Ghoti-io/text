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
 * What the container files share (css_container.c, css_container_style.c, css_rule_index.c). Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_CONTAINER_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_CONTAINER_INTERNAL_H

#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_container.h>
#include <ghoti.io/text/css/css_logical.h>
#include <ghoti.io/text/macros.h>

#include "css_condition_internal.h"
#include "css_media_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// A query container being queried: its style, its kinds, and the things asked of the caller once.
struct css_cq_ctx {
  const GTEXT_CSS_Conditions * c;
  GTEXT_CSS_Condition_Call * call;
  const GTEXT_CSS_Element_Interface * iface;
  GTEXT_CSS_Element_Handle container;
  const GTEXT_CSS_Computed_Style * style;
  uint32_t types;                      ///< GTEXT_CSS_CONTAINER_TYPE_*
  GTEXT_CSS_Flow flow;
  bool inline_horizontal;
  bool size_loaded, size_ok;
  GTEXT_CSS_Container_Metrics size;
  bool scroll_loaded, scroll_ok;
  GTEXT_CSS_Container_Scroll_State scroll;
};
typedef struct css_cq_ctx css_cq_ctx;

/// The nearest inclusive ancestor of `start` (its parent's, with `from_element` false) that is a query container for the request; NULL if none.
GTEXT_CSS_Status css_cq_resolve(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call, const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle start,
    bool from_element, const char * name, size_t name_len, uint32_t mask, GTEXT_CSS_Element_Handle * out);

/// One condition (root) of a container list for an element, memoized by (container, root).
GTEXT_CSS_Status css_cq_eval_root(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call, const GTEXT_CSS_Element_Interface * iface,
    const struct GTEXT_CSS_Media_List_Internal * list, uint32_t root, GTEXT_CSS_Element_Handle element, bool from_element, GTEXT_CSS_Tristate * out);

/// A style() feature against the container (css_container_style.c).
GTEXT_CSS_Tristate css_cq_eval_style(css_cq_ctx * cq, const struct GTEXT_CSS_Media_List_Internal * in, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail);

/// What the style queries keep in a call (a compute context), and its release and reset.
void css_cq_style_free(struct GTEXT_CSS_Condition_Call_Internal * ci);
void css_cq_style_reset(struct GTEXT_CSS_Condition_Call_Internal * ci);

#ifdef __cplusplus
}
#endif

#endif
