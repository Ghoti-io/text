#!/usr/bin/env python3
"""Write the seed corpora for fuzz_xml and fuzz_xml_writer.

    tools/xml/make-fuzz-seeds.py

The seeds are tracked (tests/fuzz/corpus/.gitignore keeps `*.seed` and nothing else),
and they are *derived*: this script reads the W3C XML Conformance Test Suite under
third_party/xml/xmlconf (fetch it with tools/xml/fetch.sh) and writes them again, the
same bytes each time. Run it after a change to the selection and commit the result.
Files named `reproducer-*.seed` are not touched: they are the recorded finds, each one
the input that made a fuzzer fail before the fix, replayed by `make test`
(tests/test-xml-fuzz-seeds.cpp) and by every fuzz run.

fuzz_xml: four option bytes and then a document (tests/fuzz/fuzz_xml.cpp). A quarter
of the suite's documents of 300 bytes or fewer - which is nearly all of them - and
every other document up to 4,096 bytes, libFuzzer's default input size: the ones that
are not plain UTF-8 or are longer, because those are the encodings and the long
constructs a mutator would have to find by itself. A document over 4,096 bytes is not
a seed (libFuzzer would cut it to that). The option bytes are one of a few sets chosen
by a hash of the document, so adding or removing a document changes no other file. The
not-well-formed half of the suite is in, and so is the rest: a corpus of accepted
documents would teach the search nothing about refusals.

Also written: one tiny document for each way of starting a document in an encoding
(each byte order mark, a UTF-16 and a UTF-32 `<?`, an ISO-8859-1 and a US-ASCII
declaration, a declaration that lies), since coverage of the encoding detector is
reached by the first four bytes and by nothing else.

fuzz_xml_writer: a few dozen programs, which are bytes and nothing else. The
interesting ones are found by the search in seconds; these only start it somewhere
other than zero.

Copyright 2026 by Corey Pennycuff
"""

import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "oracle"))

import xml_gen  # noqa: E402

CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus")
MAX_SEED = 4096  # libFuzzer's default -max_len

# Option bytes (o1, o2, o3, o4) for fuzz_xml (the layout is in the harness), chosen by a hash
# of the document.
OPTIONS = (
    (0x00, 0x00, 0x00, 0x00),  # default limits, none of the extra modes
    (0xC0, 0x80, 0x00, 0x00),  # both streams eager, the allocator-failure runs
    (0x37, 0x2D, 0x35, 0x00),  # tiny limits: depth 5, 3 attributes, a value of 4 bytes, a name
                               # of 3, 6 nodes, 3 namespaces in scope
    (0x00, 0x80, 0x00, 0x03),  # the allocator-failure runs, pieces of 5 bytes
    (0x00, 0x00, 0x40, 0x11),  # an input limit 14 bytes under the document's length, pieces of 3
    (0x20, 0x06, 0x00, 0x00),  # one attribute at most, an attribute value of 16 bytes
)

# What the script must produce, so that a change to the selection that quietly drops most of
# the seeds fails here and not in a fuzz run that has nothing to start from.
EXPECTED_SUITE_SEEDS = (700, 1000)
EXPECTED_WRITER_PROGRAMS = 48

ENCODING_STARTS = {
    "utf8-bom": b"\xef\xbb\xbf<a/>",
    "utf16le-bom": b"\xff\xfe<\x00a\x00/\x00>\x00",
    "utf16be-bom": b"\xfe\xff\x00<\x00a\x00/\x00>",
    "utf32le-bom": b"\xff\xfe\x00\x00<\x00\x00\x00a\x00\x00\x00/\x00\x00\x00>\x00\x00\x00",
    "utf32be-bom": b"\x00\x00\xfe\xff\x00\x00\x00<\x00\x00\x00a\x00\x00\x00/\x00\x00\x00>",
    "utf16le-decl": b"<\x00?\x00x\x00m\x00l\x00 \x00v\x00e\x00r\x00s\x00i\x00o\x00n\x00=\x00\"\x001\x00.\x000\x00\"\x00?\x00>\x00<\x00a\x00/\x00>\x00",
    "utf16be-decl": b"\x00<\x00?\x00x\x00m\x00l\x00 \x00v\x00e\x00r\x00s\x00i\x00o\x00n\x00=\x00\"\x001\x00.\x000\x00\"\x00?\x00>\x00<\x00a\x00/\x00>",
    "utf32le-decl": b"<\x00\x00\x00?\x00\x00\x00x\x00\x00\x00m\x00\x00\x00l\x00\x00\x00 \x00\x00\x00v\x00\x00\x00e\x00\x00\x00r\x00\x00\x00s\x00\x00\x00i\x00\x00\x00o\x00\x00\x00n\x00\x00\x00=\x00\x00\x00\"\x00\x00\x001\x00\x00\x00.\x00\x00\x000\x00\x00\x00\"\x00\x00\x00?\x00\x00\x00>\x00\x00\x00<\x00\x00\x00a\x00\x00\x00/\x00\x00\x00>\x00\x00\x00",
    "latin1-declared": b"<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a b=\"\xe9\"/>",
    "ascii-declared": b"<?xml version=\"1.0\" encoding=\"US-ASCII\"?><a/>",
    "label-lies-utf16": b"\xff\xfe<\x00?\x00x\x00m\x00l\x00 \x00v\x00e\x00r\x00s\x00i\x00o\x00n\x00=\x00\"\x001\x00.\x000\x00\"\x00 \x00e\x00n\x00c\x00o\x00d\x00i\x00n\x00g\x00=\x00\"\x00U\x00T\x00F\x00-\x008\x00\"\x00?\x00>\x00<\x00a\x00/\x00>\x00",
    "label-lies-utf8": b"<?xml version=\"1.0\" encoding=\"UTF-16\"?><a/>",
    "lone-surrogate": b"\xff\xfe<\x00a\x00>\x00\x00\xd8<\x00/\x00a\x00>\x00",
    "ebcdic": b"\x4c\x6f\xa7\x94\x93",
    "unsupported-label": b"<?xml version=\"1.0\" encoding=\"windows-1252\"?><a/>",
    "version-11": b"<?xml version=\"1.1\"?><a/>",
}


def write(directory, name, data):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, name + ".seed"), "wb") as fh:
        fh.write(data)


def safe(ident):
    return re.sub(r"[^A-Za-z0-9._-]", "_", ident)


def main():
    xml_dir = os.path.join(CORPUS, "xml")
    # Only what this script writes: a reproducer-*.seed is a find, recorded by hand, and
    # is not regenerated from anything.
    for old in (os.listdir(xml_dir) if os.path.isdir(xml_dir) else []):
        if old.endswith(".seed") and old.startswith(("xmlconf-", "encoding-")):
            os.unlink(os.path.join(xml_dir, old))
    cases = sorted(xml_gen.xmlconf_cases(), key=lambda c: c[0])
    kept = 0
    for index, (ident, data, _tags) in enumerate(cases):
        plain_utf8 = len(data) <= 300 and not data.startswith((b"\xef\xbb\xbf", b"\xff\xfe", b"\xfe\xff", b"\x00", b"<\x00"))
        try:
            data.decode("utf-8")
        except UnicodeDecodeError:
            plain_utf8 = False
        if plain_utf8 and index % 4 != 0:
            continue
        if len(data) > MAX_SEED:
            continue
        pick = int.from_bytes(hashlib.sha256(data).digest()[:4], "big") % len(OPTIONS)
        write(xml_dir, "xmlconf-" + safe(ident), bytes(OPTIONS[pick]) + data)
        kept += 1
    for name, data in sorted(ENCODING_STARTS.items()):
        write(xml_dir, "encoding-" + name, bytes(OPTIONS[0]) + data)
    # Programs for the writer: bytes from a fixed generator.
    rng = xml_gen.Rng(20261011)
    writer_dir = os.path.join(CORPUS, "xml-writer")
    for old in (os.listdir(writer_dir) if os.path.isdir(writer_dir) else []):
        if old.endswith(".seed") and old.startswith("program-"):
            os.unlink(os.path.join(writer_dir, old))
    for n in range(EXPECTED_WRITER_PROGRAMS):
        length = 24 + rng.below(160)
        prog = bytes(rng.below(256) for _ in range(length))
        # The first bytes choose strict or wild mode: alternate them so both start populated.
        prog = bytes((prog[0], prog[1], (prog[2] & 0xfe) | (n & 1))) + prog[3:]
        write(writer_dir, "program-%02d" % n, prog)
    if not EXPECTED_SUITE_SEEDS[0] <= kept <= EXPECTED_SUITE_SEEDS[1]:
        raise SystemExit("%d suite seeds written; the documentation says %d to %d" % ((kept,) + EXPECTED_SUITE_SEEDS))
    print("fuzz_xml: %d seeds from the suite and %d encoding starts; fuzz_xml_writer: %d programs"
          % (kept, len(ENCODING_STARTS), EXPECTED_WRITER_PROGRAMS))


if __name__ == "__main__":
    main()
