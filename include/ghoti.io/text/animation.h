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
 * @file animation.h
 * @brief Public umbrella header for the Web Animations model.
 *
 * Include <ghoti.io/text/animation.h> for the model of Web Animations 1 as a value and a state, with no clock, no promise and no event loop of its own: the timing of an effect
 * (timing.h), the events a frame owes (events.h), the CSS Animations of an element (css_animations.h) and the animations of a document (world.h).
 */

#ifndef GHOTI_IO_GTEXT_ANIMATION_H
#define GHOTI_IO_GTEXT_ANIMATION_H

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/animation/css_animations.h>
#include <ghoti.io/text/animation/events.h>
#include <ghoti.io/text/animation/timing.h>
#include <ghoti.io/text/animation/world.h>

#endif // GHOTI_IO_GTEXT_ANIMATION_H
