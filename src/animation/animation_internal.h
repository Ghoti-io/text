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
 * What the Animation state machine shares with the effect (effect.c). Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_ANIMATION_ANIMATION_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_ANIMATION_ANIMATION_INTERNAL_H

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/animation/animation.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Record which keyframe effect object the animation holds (NULL for none). The link the other way is the effect's.
void anim_effect_put(GTEXT_Anim_Animation * animation, GTEXT_Anim_Effect * effect);

#ifdef __cplusplus
}
#endif

#endif
