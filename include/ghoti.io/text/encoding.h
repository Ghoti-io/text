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
 * @file encoding.h
 * @brief Public umbrella header for the Encoding Standard.
 *
 * Include <ghoti.io/text/encoding.h> for the WHATWG Encoding Standard's label table
 * (encoding_core.h: `gtext_encoding_for_label()` and the encodings it returns),
 * its streaming decoders (encoding_decode.h: bytes in an encoding to UTF-8) and its
 * encoders (encoding_encode.h: UTF-8 to the bytes of an encoding). See the manual
 * (documentation/formats/encoding.md).
 */

#ifndef GHOTI_IO_GTEXT_ENCODING_H
#define GHOTI_IO_GTEXT_ENCODING_H

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/encoding/encoding_core.h>
#include <ghoti.io/text/encoding/encoding_decode.h>
#include <ghoti.io/text/encoding/encoding_encode.h>

#endif // GHOTI_IO_GTEXT_ENCODING_H
