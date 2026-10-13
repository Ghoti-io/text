#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Text.
#
# Ghoti.io Text is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
#
# Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
# for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""The Encoding Standard's multi-byte decoders and encoders, written a second time, in Python, straight from the standard's text.

This is the arbiter of tools/oracle/encoding_diff.py for the multi-byte encodings: the library (C, tables generated from
indexes.json by tools/encoding/gen_tables.py) and this module (Python, reading indexes.json directly with the standard's
own words for the index operations) are two programs that share nothing but the standard's data and its text, so where they agree
the answer is not one program's reading, and where Node's ICU converters or Python's codecs differ from both, the difference is the
reference's. Written as the standard writes them: a handler per encoding, given one byte (or None for end-of-queue) and an I/O queue it can
prepend ("restore") to, returning "finished", "continue", an error, or one or more code points.

Not fast (about 10 microseconds a byte); the differential runs it over the sequences where a reference differs, not over everything.

Copyright 2026 by Corey Pennycuff
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.path.dirname(os.path.dirname(HERE)), "third_party", "encoding")

FINISHED = "finished"
CONTINUE = "continue"
ERROR = "error"

_INDEXES = None


def indexes():
    global _INDEXES
    if _INDEXES is None:
        with open(os.path.join(DATA, "indexes.json"), encoding="utf-8") as fh:
            _INDEXES = json.load(fh)
    return _INDEXES


def index_code_point(name, pointer):
    """'The index code point for pointer in index': the code point, or None."""
    index = indexes()[name]
    if pointer is None or pointer < 0 or pointer >= len(index):
        return None
    return index[pointer]


def index_pointer(name, code_point, exclude=lambda p: False, last=()):
    """'The index pointer for codePoint in index': the first pointer (or the last, for the code points in `last`), skipping excluded entries."""
    index = indexes()[name]
    found = None
    for pointer, cp in enumerate(index):
        if cp == code_point and not exclude(pointer):
            if code_point not in last:
                return pointer
            found = pointer
    return found


_RANGES = None


def ranges():
    global _RANGES
    if _RANGES is None:
        _RANGES = indexes()["gb18030-ranges"]
    return _RANGES


def gb18030_ranges_code_point(pointer):
    if (39419 < pointer < 189000) or pointer > 1237575:
        return None
    if pointer == 7457:
        return 0xE7C7
    offset, cp_offset = max(r for r in ranges() if r[0] <= pointer)
    return cp_offset + pointer - offset


def gb18030_ranges_pointer(cp):
    if cp == 0xE7C7:
        return 7457
    offset, pointer_offset = max((r[1], r[0]) for r in ranges() if r[1] <= cp)
    return pointer_offset + cp - offset


# ---- decoders ------------------------------------------------------------------------------------------------------------------

class Decoder:
    """Base: `handle(byte)` returns FINISHED, CONTINUE, ERROR or a list of code points, and may set self.restore to a list of bytes."""

    def __init__(self):
        self.restore = []


class Gb18030(Decoder):
    def __init__(self):
        super().__init__()
        self.first = self.second = self.third = 0

    def handle(self, byte):
        if byte is None and self.first == 0 and self.second == 0 and self.third == 0:
            return FINISHED
        if byte is None:
            self.first = self.second = self.third = 0
            return ERROR
        if self.third != 0:
            if not 0x30 <= byte <= 0x39:
                self.restore = [self.second, self.third, byte]
                self.first = self.second = self.third = 0
                return ERROR
            cp = gb18030_ranges_code_point((self.first - 0x81) * (10 * 126 * 10) + (self.second - 0x30) * (10 * 126)
                                           + (self.third - 0x81) * 10 + byte - 0x30)
            self.first = self.second = self.third = 0
            if cp is None:
                return ERROR
            return [cp]
        if self.second != 0:
            if 0x81 <= byte <= 0xFE:
                self.third = byte
                return CONTINUE
            self.restore = [self.second, byte]
            self.first = self.second = 0
            return ERROR
        if self.first != 0:
            if 0x30 <= byte <= 0x39:
                self.second = byte
                return CONTINUE
            leading = self.first
            self.first = 0
            pointer = None
            offset = 0x40 if byte < 0x7F else 0x41
            if 0x40 <= byte <= 0x7E or 0x80 <= byte <= 0xFE:
                pointer = (leading - 0x81) * 190 + (byte - offset)
            cp = None if pointer is None else index_code_point("gb18030", pointer)
            if cp is not None:
                return [cp]
            if byte <= 0x7F:
                self.restore = [byte]
            return ERROR
        if byte <= 0x7F:
            return [byte]
        if byte == 0x80:
            return [0x20AC]
        if 0x81 <= byte <= 0xFE:
            self.first = byte
            return CONTINUE
        return ERROR


class Big5(Decoder):
    TABLE = {1133: [0x00CA, 0x0304], 1135: [0x00CA, 0x030C], 1164: [0x00EA, 0x0304], 1166: [0x00EA, 0x030C]}

    def __init__(self):
        super().__init__()
        self.leading = 0

    def handle(self, byte):
        if byte is None and self.leading != 0:
            self.leading = 0
            return ERROR
        if byte is None:
            return FINISHED
        if self.leading != 0:
            leading = self.leading
            self.leading = 0
            pointer = None
            offset = 0x40 if byte < 0x7F else 0x62
            if 0x40 <= byte <= 0x7E or 0xA1 <= byte <= 0xFE:
                pointer = (leading - 0x81) * 157 + (byte - offset)
            if pointer in self.TABLE:
                return list(self.TABLE[pointer])
            cp = None if pointer is None else index_code_point("big5", pointer)
            if cp is not None:
                return [cp]
            if byte <= 0x7F:
                self.restore = [byte]
            return ERROR
        if byte <= 0x7F:
            return [byte]
        if 0x81 <= byte <= 0xFE:
            self.leading = byte
            return CONTINUE
        return ERROR


class EucJp(Decoder):
    def __init__(self):
        super().__init__()
        self.jis0212 = False
        self.leading = 0

    def handle(self, byte):
        if byte is None and self.leading != 0:
            self.leading = 0
            return ERROR
        if byte is None:
            return FINISHED
        if self.leading == 0x8E and 0xA1 <= byte <= 0xDF:
            self.leading = 0
            return [0xFF61 - 0xA1 + byte]
        if self.leading == 0x8F and 0xA1 <= byte <= 0xFE:
            self.jis0212 = True
            self.leading = byte
            return CONTINUE
        if self.leading != 0:
            leading = self.leading
            self.leading = 0
            cp = None
            if 0xA1 <= leading <= 0xFE and 0xA1 <= byte <= 0xFE:
                cp = index_code_point("jis0212" if self.jis0212 else "jis0208", (leading - 0xA1) * 94 + byte - 0xA1)
            self.jis0212 = False
            if cp is not None:
                return [cp]
            if byte <= 0x7F:
                self.restore = [byte]
            return ERROR
        if byte <= 0x7F:
            return [byte]
        if byte in (0x8E, 0x8F) or 0xA1 <= byte <= 0xFE:
            self.leading = byte
            return CONTINUE
        return ERROR


class Iso2022Jp(Decoder):
    ASCII, ROMAN, KATAKANA, LEAD, TRAIL, ESCAPE_START, ESCAPE = range(7)

    def __init__(self):
        super().__init__()
        self.state = self.ASCII
        self.output_state = self.ASCII
        self.leading = 0
        self.output = False

    def handle(self, byte):
        s = self.state
        if s == self.ASCII:
            if byte == 0x1B:
                self.state = self.ESCAPE_START
                return CONTINUE
            if byte is not None and 0 <= byte <= 0x7F and byte not in (0x0E, 0x0F):
                self.output = False
                return [byte]
            if byte is None:
                return FINISHED
            self.output = False
            return ERROR
        if s == self.ROMAN:
            if byte == 0x1B:
                self.state = self.ESCAPE_START
                return CONTINUE
            if byte == 0x5C:
                self.output = False
                return [0x00A5]
            if byte == 0x7E:
                self.output = False
                return [0x203E]
            if byte is not None and 0 <= byte <= 0x7F and byte not in (0x0E, 0x0F, 0x5C, 0x7E):
                self.output = False
                return [byte]
            if byte is None:
                return FINISHED
            self.output = False
            return ERROR
        if s == self.KATAKANA:
            if byte == 0x1B:
                self.state = self.ESCAPE_START
                return CONTINUE
            if byte is not None and 0x21 <= byte <= 0x5F:
                self.output = False
                return [0xFF61 - 0x21 + byte]
            if byte is None:
                return FINISHED
            self.output = False
            return ERROR
        if s == self.LEAD:
            if byte == 0x1B:
                self.state = self.ESCAPE_START
                return CONTINUE
            if byte is not None and 0x21 <= byte <= 0x7E:
                self.output = False
                self.leading = byte
                self.state = self.TRAIL
                return CONTINUE
            if byte is None:
                return FINISHED
            self.output = False
            return ERROR
        if s == self.TRAIL:
            if byte == 0x1B:
                self.state = self.ESCAPE_START
                return ERROR
            if byte is not None and 0x21 <= byte <= 0x7E:
                self.state = self.LEAD
                cp = index_code_point("jis0208", (self.leading - 0x21) * 94 + byte - 0x21)
                if cp is None:
                    return ERROR
                return [cp]
            self.state = self.LEAD
            return ERROR
        if s == self.ESCAPE_START:
            if byte in (0x24, 0x28):
                self.leading = byte
                self.state = self.ESCAPE
                return CONTINUE
            if byte is not None:
                self.restore = [byte]
            self.output = False
            self.state = self.output_state
            return ERROR
        # escape
        leading = self.leading
        self.leading = 0
        state = None
        if leading == 0x28 and byte == 0x42:
            state = self.ASCII
        if leading == 0x28 and byte == 0x4A:
            state = self.ROMAN
        if leading == 0x28 and byte == 0x49:
            state = self.KATAKANA
        if leading == 0x24 and byte in (0x40, 0x42):
            state = self.LEAD
        if state is not None:
            self.state = self.output_state = state
            output = self.output
            self.output = True
            return CONTINUE if not output else ERROR
        self.restore = [leading] if byte is None else [leading, byte]
        self.output = False
        self.state = self.output_state
        return ERROR


class ShiftJis(Decoder):
    def __init__(self):
        super().__init__()
        self.leading = 0

    def handle(self, byte):
        if byte is None and self.leading != 0:
            self.leading = 0
            return ERROR
        if byte is None:
            return FINISHED
        if self.leading != 0:
            leading = self.leading
            self.leading = 0
            pointer = None
            offset = 0x40 if byte < 0x7F else 0x41
            leading_offset = 0x81 if leading < 0xA0 else 0xC1
            if 0x40 <= byte <= 0x7E or 0x80 <= byte <= 0xFC:
                pointer = (leading - leading_offset) * 188 + byte - offset
            if pointer is not None and 8836 <= pointer <= 10715:
                return [0xE000 - 8836 + pointer]
            cp = None if pointer is None else index_code_point("jis0208", pointer)
            if cp is not None:
                return [cp]
            if byte <= 0x7F:
                self.restore = [byte]
            return ERROR
        if byte <= 0x7F or byte == 0x80:
            return [byte]
        if 0xA1 <= byte <= 0xDF:
            return [0xFF61 - 0xA1 + byte]
        if 0x81 <= byte <= 0x9F or 0xE0 <= byte <= 0xFC:
            self.leading = byte
            return CONTINUE
        return ERROR


class EucKr(Decoder):
    def __init__(self):
        super().__init__()
        self.leading = 0

    def handle(self, byte):
        if byte is None and self.leading != 0:
            self.leading = 0
            return ERROR
        if byte is None:
            return FINISHED
        if self.leading != 0:
            leading = self.leading
            self.leading = 0
            pointer = None
            if 0x41 <= byte <= 0xFE:
                pointer = (leading - 0x81) * 190 + (byte - 0x41)
            cp = None if pointer is None else index_code_point("euc-kr", pointer)
            if cp is not None:
                return [cp]
            if byte <= 0x7F:
                self.restore = [byte]
            return ERROR
        if byte <= 0x7F:
            return [byte]
        if 0x81 <= byte <= 0xFE:
            self.leading = byte
            return CONTINUE
        return ERROR


DECODERS = {
    "gbk": Gb18030, "gb18030": Gb18030, "big5": Big5, "euc-jp": EucJp, "iso-2022-jp": Iso2022Jp,
    "shift_jis": ShiftJis, "euc-kr": EucKr,
}


def decode(name, data, fatal=False, make=None):
    """'Process a queue' for a decoder, on the whole input and the end-of-queue. Returns (code points, None) or (code points so far, offset of the fatal error).

    The offset is the position, in `data`, of the item at which the handler returned its error (a restored byte counts as the position it came from)."""
    dec = (make or DECODERS[name])()
    queue = list(data)
    out = []
    position = 0  # index in data of the next byte to be read from the input proper
    pending = []  # restored bytes, to be read first (the front of the queue), as (byte, original position)
    while True:
        if pending:
            byte, where = pending.pop(0)
        elif queue:
            byte = queue.pop(0)
            where = position
            position += 1
        else:
            byte, where = None, position
        dec.restore = []
        result = dec.handle(byte)
        if result == FINISHED:
            return out, None
        if result == CONTINUE:
            continue
        if result == ERROR:
            if fatal:
                return out, where
            out.append(0xFFFD)
            # "restore" puts bytes in front of the queue; their positions are the ones they were read from (the last restored are the latest read)
            restored = list(dec.restore)
            base = where - len(restored) + 1 if byte is not None else where - len(restored)
            pending = [(b, base + k) for k, b in enumerate(restored)] + pending
            continue
        out.extend(result)


def decode_units(name, data):
    """The decode of `data` cut into the units the standard's handler works in: [(start, end, kind, code points)].

    A unit is the bytes the handler read from the first one it was given after the previous unit's result to the one at which it returned a result:
    `kind` is "ok" (code points), "err" (U+FFFD) or "err-restore" (U+FFFD, and the last byte or bytes of the unit were restored: the next unit starts at them,
    so units overlap by what was restored). Escape sequences of ISO-2022-JP, which produce nothing, are part of the unit of the next result. A fatal-mode
    decode stops at the first "err" unit; the replacement-mode output is the concatenation of the units' code points."""
    dec = DECODERS[name]()
    queue = list(data)
    position = 0
    pending = []
    units = []
    start = 0           # the first byte of the unit being read
    cursor = 0          # the position in `data` of the next byte to read (a restored byte moves it back)
    while True:
        if pending:
            byte = pending.pop(0)
            cursor += 1
        elif queue:
            byte = queue.pop(0)
            position += 1
            cursor = position
        else:
            byte = None
        dec.restore = []
        result = dec.handle(byte)
        if result == FINISHED:
            return units
        if result == CONTINUE:
            continue
        if result == ERROR:
            restored = list(dec.restore)
            end = cursor
            units.append((start, end, "err-restore" if restored else "err", [0xFFFD]))
            cursor -= len(restored) if byte is not None else 0
            pending = restored + pending
            if byte is None:
                # the restored bytes of an end-of-queue error are the ones already read: they come back
                cursor = end - len(restored)
            start = cursor
            continue
        units.append((start, cursor, "ok", list(result)))
        start = cursor


# ---- encoders ------------------------------------------------------------------------------------------------------------------

GB18030_SIDE = {0xE78D: (0xA6, 0xD9), 0xE78E: (0xA6, 0xDA), 0xE78F: (0xA6, 0xDB), 0xE790: (0xA6, 0xDC), 0xE791: (0xA6, 0xDD),
                0xE792: (0xA6, 0xDE), 0xE793: (0xA6, 0xDF), 0xE794: (0xA6, 0xEC), 0xE795: (0xA6, 0xED), 0xE796: (0xA6, 0xF3),
                0xE81E: (0xFE, 0x59), 0xE826: (0xFE, 0x61), 0xE82B: (0xFE, 0x66), 0xE82C: (0xFE, 0x67), 0xE832: (0xFE, 0x6D),
                0xE843: (0xFE, 0x7E), 0xE854: (0xFE, 0x90), 0xE864: (0xFE, 0xA0)}

_REV_CACHE = {}


def _reverse(key, name, exclude=lambda p: False, last=()):
    """code point -> pointer for 'the index pointer for a code point' (computed once from the index in the standard's words)."""
    if key not in _REV_CACHE:
        table = {}
        for pointer, cp in enumerate(indexes()[name]):
            if cp is None or exclude(pointer):
                continue
            if cp in last or cp not in table:
                table[cp] = pointer
        _REV_CACHE[key] = table
    return _REV_CACHE[key]


def rev_gb18030():
    return _reverse("gb18030", "gb18030")


def rev_big5():
    # "index Big5 excluding all entries whose pointer is less than (0xA1 - 0x81) x 157", the last pointer for six code points.
    return _reverse("big5", "big5", exclude=lambda p: p < (0xA1 - 0x81) * 157, last=(0x2550, 0x255E, 0x2561, 0x256A, 0x5341, 0x5345))


def rev_jis0208():
    return _reverse("jis0208", "jis0208")


def rev_shift_jis():
    # "index jis0208 excluding all entries whose pointer is in the range 8272 to 8835, inclusive"
    return _reverse("shift_jis", "jis0208", exclude=lambda p: 8272 <= p <= 8835)


def rev_euc_kr():
    return _reverse("euc-kr", "euc-kr")


class EncError(Exception):
    def __init__(self, cp, ref=None):
        """`cp`: the code point handled (what a fatal status names); `ref`: the code point "error with codePoint" names, which the html mode writes as a reference."""
        super().__init__(cp)
        self.cp = cp
        self.ref = cp if ref is None else ref


def encode_gb18030(cp, is_gbk):
    if cp <= 0x7F:
        return bytes([cp])
    if cp == 0xE5E5:
        raise EncError(cp)
    if is_gbk and cp == 0x20AC:
        return b"\x80"
    if cp in GB18030_SIDE:
        return bytes(GB18030_SIDE[cp])
    pointer = rev_gb18030().get(cp)
    if pointer is not None:
        leading = pointer // 190 + 0x81
        trailing = pointer % 190
        offset = 0x40 if trailing < 0x3F else 0x41
        return bytes([leading, trailing + offset])
    if is_gbk:
        raise EncError(cp)
    pointer = gb18030_ranges_pointer(cp)
    byte1, pointer = divmod(pointer, 10 * 126 * 10)
    byte2, pointer = divmod(pointer, 10 * 126)
    byte3, byte4 = divmod(pointer, 10)
    return bytes([byte1 + 0x81, byte2 + 0x30, byte3 + 0x81, byte4 + 0x30])


def encode_big5(cp):
    if cp <= 0x7F:
        return bytes([cp])
    pointer = rev_big5().get(cp)
    if pointer is None:
        raise EncError(cp)
    leading = pointer // 157 + 0x81
    trailing = pointer % 157
    offset = 0x40 if trailing < 0x3F else 0x62
    return bytes([leading, trailing + offset])


def encode_euc_jp(cp):
    if cp <= 0x7F:
        return bytes([cp])
    if cp == 0xA5:
        return b"\x5c"
    if cp == 0x203E:
        return b"\x7e"
    if 0xFF61 <= cp <= 0xFF9F:
        return bytes([0x8E, cp - 0xFF61 + 0xA1])
    if cp == 0x2212:
        cp = 0xFF0D
    pointer = rev_jis0208().get(cp)
    if pointer is None:
        raise EncError(cp)
    return bytes([pointer // 94 + 0xA1, pointer % 94 + 0xA1])


def encode_shift_jis(cp):
    if cp <= 0x7F or cp == 0x80:
        return bytes([cp])
    if cp == 0xA5:
        return b"\x5c"
    if cp == 0x203E:
        return b"\x7e"
    if 0xFF61 <= cp <= 0xFF9F:
        return bytes([cp - 0xFF61 + 0xA1])
    if cp == 0x2212:
        cp = 0xFF0D
    pointer = rev_shift_jis().get(cp)
    if pointer is None:
        raise EncError(cp)
    leading = pointer // 188
    leading_offset = 0x81 if leading < 0x1F else 0xC1
    trailing = pointer % 188
    offset = 0x40 if trailing < 0x3F else 0x41
    return bytes([leading + leading_offset, trailing + offset])


def encode_euc_kr(cp):
    if cp <= 0x7F:
        return bytes([cp])
    pointer = rev_euc_kr().get(cp)
    if pointer is None:
        raise EncError(cp)
    return bytes([pointer // 190 + 0x81, pointer % 190 + 0x41])


class Iso2022JpEncoder:
    def __init__(self):
        self.state = "ascii"

    def handle(self, cp):
        """Returns (bytes, restore_cp or None); raises EncError. cp None is end-of-queue; returns (None, None) for finished."""
        if cp is None and self.state != "ascii":
            self.state = "ascii"
            return b"\x1b\x28\x42", None
        if cp is None:
            return None, None
        if self.state in ("ascii", "roman") and cp in (0x0E, 0x0F, 0x1B):
            raise EncError(cp, 0xFFFD)  # the fatal status names the control; the html mode's reference is U+FFFD's ("rather than codePoint to prevent attacks")
        if self.state == "ascii" and cp <= 0x7F:
            return bytes([cp]), None
        if self.state == "roman" and ((cp <= 0x7F and cp not in (0x5C, 0x7E)) or cp in (0xA5, 0x203E)):
            if cp <= 0x7F:
                return bytes([cp]), None
            return (b"\x5c" if cp == 0xA5 else b"\x7e"), None
        if cp <= 0x7F and self.state != "ascii":
            self.state = "ascii"
            return b"\x1b\x28\x42", cp
        if cp in (0xA5, 0x203E) and self.state != "roman":
            self.state = "roman"
            return b"\x1b\x28\x4a", cp
        if cp == 0x2212:
            cp = 0xFF0D
        if 0xFF61 <= cp <= 0xFF9F:
            cp = index_code_point("iso-2022-jp-katakana", cp - 0xFF61)
        pointer = rev_jis0208().get(cp)
        if pointer is None:
            if self.state == "jis0208":
                self.state = "ascii"
                return b"\x1b\x28\x42", cp
            raise EncError(cp)
        if self.state != "jis0208":
            self.state = "jis0208"
            return b"\x1b\x24\x42", cp
        return bytes([pointer // 94 + 0x21, pointer % 94 + 0x21]), None


ENCODERS = {
    "gbk": lambda cp: encode_gb18030(cp, True), "gb18030": lambda cp: encode_gb18030(cp, False), "big5": encode_big5,
    "euc-jp": encode_euc_jp, "shift_jis": encode_shift_jis, "euc-kr": encode_euc_kr,
}


def encode(name, cps, mode="html"):
    """'Process a queue' for an encoder, then end-of-queue. Returns (bytes, None) or (bytes so far, (index in cps, code point of the error)) in fatal mode.

    mode: "html" (a numeric reference for each unencodable code point), "fatal"."""
    out = bytearray()
    if name == "iso-2022-jp":
        enc = Iso2022JpEncoder()
        queue = list(enumerate(cps))
        position = 0
        while queue:
            position, cp = queue.pop(0)
            try:
                data, restore = enc.handle(cp)
            except EncError as e:
                if mode == "fatal":
                    return bytes(out), (position, e.cp)
                out += ("&#%d;" % e.ref).encode("ascii")
                continue
            out += data
            if restore is not None:
                queue.insert(0, (position, restore))
        data, _ = enc.handle(None)
        if data:
            out += data
        return bytes(out), None
    fn = ENCODERS[name]
    for position, cp in enumerate(cps):
        try:
            out += fn(cp)
        except EncError as e:
            if mode == "fatal":
                return bytes(out), (position, e.cp)
            out += ("&#%d;" % e.ref).encode("ascii")
    return bytes(out), None


def encode_path(name, cp):
    """Which branch of the standard's encoder a code point takes, as a short phrase: what the encoder's algorithm says about it."""
    ix = indexes()
    if name == "iso-2022-jp":
        if cp in (0x0E, 0x0F, 0x1B):
            return "ISO-2022-JP: U+000E, U+000F and U+001B are an error with U+FFFD"
        if cp <= 0x7F:
            return "ASCII"
        if cp in (0xA5, 0x203E):
            return "Roman state (U+00A5, U+203E)"
        if cp == 0x2212:
            return "U+2212 is set to U+FF0D"
        if 0xFF61 <= cp <= 0xFF9F:
            return "half-width katakana through index ISO-2022-JP katakana"
        return "index jis0208 pointer" if rev_jis0208().get(cp) is not None else "not in index jis0208"
    if cp <= 0x7F:
        return "ASCII"
    if name in ("gbk", "gb18030"):
        if cp == 0xE5E5:
            return "U+E5E5 is an error"
        if name == "gbk" and cp == 0x20AC:
            return "GBK: U+20AC is byte 0x80"
        if cp in GB18030_SIDE:
            return "gb18030 encoder's side table"
        if rev_gb18030().get(cp) is not None:
            return "index gb18030 pointer"
        return "GBK: no four-byte form" if name == "gbk" else "index gb18030 ranges pointer (four bytes)"
    if name == "big5":
        pointer = rev_big5().get(cp)
        if pointer is not None:
            return "index Big5 pointer (the last of the pointers, for six code points)" if cp in (0x2550, 0x255E, 0x2561, 0x256A, 0x5341, 0x5345) else "index Big5 pointer"
        if any(c == cp for c in ix["big5"][:(0xA1 - 0x81) * 157]):
            return "only in index Big5 below pointer 5024 (the Hong Kong extensions, excluded)"
        return "not in index Big5"
    if name in ("euc-jp", "shift_jis"):
        if cp == 0xA5 or cp == 0x203E:
            return "U+00A5 and U+203E are the ASCII bytes 0x5C and 0x7E"
        if 0xFF61 <= cp <= 0xFF9F:
            return "half-width katakana"
        if name == "shift_jis" and cp == 0x80:
            return "Shift_JIS: U+0080 is byte 0x80"
        if cp == 0x2212:
            return "U+2212 is set to U+FF0D"
        if name == "shift_jis":
            first = rev_jis0208().get(cp)
            pointer = rev_shift_jis().get(cp)
            if pointer is None:
                return "not in index Shift_JIS"
            return "index Shift_JIS pointer (a later one: the first is in 8272..8835)" if first != pointer else "index Shift_JIS pointer"
        return "index jis0208 pointer" if rev_jis0208().get(cp) is not None else "not in index jis0208"
    return "index EUC-KR pointer" if rev_euc_kr().get(cp) is not None else "not in index EUC-KR"
