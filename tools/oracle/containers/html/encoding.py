#!/usr/bin/env python3
"""The Python half of the Encoding references (tools/oracle/encoding_ref.py): the `codecs` of CPython 3.13.

Python's codecs are not the Encoding Standard: they are a second reading where they agree with it, and a list of differences where they do
not (cp1252 leaves five bytes undefined, ISO-8859-x has no unmapped bytes where the standard has them, mac_cyrillic and mac_roman
differ in a few places, there is no x-user-defined and no replacement, and the label table is Python's own). tools/oracle/encoding_diff.py
says which of those it reads and which it only counts.

Read from the repository by the gate, not copied into the image. Every string is hex, "-" for none. Requests (stdin) and answers (stdout):

  PD <codec> <errors> <bytes>     bytes.decode(codec, errors) with errors strict | replace     -> OUT <utf8> | ERR <exception name>
  PE <codec> <errors> <utf8>      str.encode(codec, errors) with strict | xmlcharrefreplace    -> OUT <bytes> | ERR <exception name>
  PING                                                                                         -> PONG

and, as a command line, `encoding.py enum <codec> <n> <errors> <start> <count>`, the records of tools/oracle/encoding_ours.c, and
`encoding.py enumx <codec> <errors> <spec>`, the records of the product of byte sets of the spec (see encoding_ours.c), and
`encoding.py encenum <codec> <errors> <first> <last>`, the records of encoding each code point by itself (255 for a failure).

Copyright 2026 by Corey Pennycuff
"""

import sys


def unhex(s):
    return b"" if s == "-" else bytes.fromhex(s)


def hexs(b):
    return "-" if not b else b.hex()


def answer(line):
    t = line.split()
    if not t:
        return None
    try:
        if t[0] == "PING":
            return "PONG"
        if t[0] == "PD":
            try:
                return "OUT " + hexs(unhex(t[3]).decode(t[1], t[2]).encode("utf-8", "surrogatepass"))
            except Exception as e:  # noqa: BLE001 - the exception is the answer
                return "ERR " + type(e).__name__
        if t[0] == "PE":
            try:
                return "OUT " + hexs(unhex(t[3]).decode("utf-8").encode(t[1], t[2]))
            except Exception as e:  # noqa: BLE001
                return "ERR " + type(e).__name__
        return "ERR E_REQUEST"
    except Exception as e:  # noqa: BLE001
        return "ERR " + type(e).__name__


def enum(codec, n, errors, start, count):
    out = sys.stdout.buffer
    buf = bytearray()
    for v in range(start, start + count):
        data = bytes((v >> (8 * (n - 1 - k))) & 0xFF for k in range(n))
        try:
            r = data.decode(codec, errors).encode("utf-8", "surrogatepass")
            buf.append(len(r))
            buf += r
        except Exception:  # noqa: BLE001
            buf.append(255)
        if len(buf) > (1 << 20):
            out.write(buf)
            buf.clear()
    out.write(buf)
    out.flush()


def parse_spec(text):
    sets = []
    for item_list in text.split(":"):
        bytes_ = set()
        for item in item_list.split(","):
            if "-" in item:
                lo, hi = (int(x, 16) for x in item.split("-"))
            else:
                lo = hi = int(item, 16)
            bytes_.update(range(lo, hi + 1))
        sets.append(sorted(bytes_))
    return sets


def enumx(codec, errors, spec):
    import itertools
    out = sys.stdout.buffer
    buf = bytearray()
    for combo in itertools.product(*parse_spec(spec)):
        data = bytes(combo)
        try:
            r = data.decode(codec, errors).encode("utf-8", "surrogatepass")
            buf.append(len(r))
            buf += r
        except Exception:  # noqa: BLE001
            buf.append(255)
        if len(buf) > (1 << 20):
            out.write(buf)
            buf.clear()
    out.write(buf)
    out.flush()


def encenum(codec, errors, first, last):
    """For each code point: the length (255 for a failure) and the bytes of its encoding, and one more byte after them: 1 if the codec decodes those
    bytes back to the code point, 0 if it decodes them to something else, 2 if it cannot."""
    out = sys.stdout.buffer
    buf = bytearray()
    for cp in range(first, last + 1):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        try:
            r = chr(cp).encode(codec, errors)
            buf.append(len(r))
            buf += r
            try:
                buf.append(1 if r.decode(codec) == chr(cp) else 0)
            except Exception:  # noqa: BLE001
                buf.append(2)
        except Exception:  # noqa: BLE001
            buf.append(255)
        if len(buf) > (1 << 20):
            out.write(buf)
            buf.clear()
    out.write(buf)
    out.flush()


def main():
    if len(sys.argv) == 6 and sys.argv[1] == "encenum":
        encenum(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]))
        return
    if len(sys.argv) == 5 and sys.argv[1] == "enumx":
        enumx(sys.argv[2], sys.argv[3], sys.argv[4])
        return
    if len(sys.argv) == 7 and sys.argv[1] == "enum":
        enum(sys.argv[2], int(sys.argv[3]), sys.argv[4], int(sys.argv[5]), int(sys.argv[6]))
        return
    for line in sys.stdin:
        a = answer(line)
        if a is not None:
            sys.stdout.write(a + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
