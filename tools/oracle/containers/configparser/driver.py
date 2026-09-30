#!/usr/bin/env python3
"""The `configparser` reference, framed for the batch protocol.

    driver.py --version
    driver.py < <stream of documents>

Per document: `<len>\n` then that many bytes. `-1\n` ends the stream. Output:

    BEGIN
    cp ok | cp err <ExceptionName>
    G <hex section name>
    E <hex key> <hex value>
    END

**Reads a file, not a string, and that is the measurement rather than a detail.**
Python's universal-newline translation applies to `read(path)` and not to
`read_string()`, so the two disagree about exactly one thing: a lone CR is a line
terminator in a file and data in a string. Of 38 documents probed both ways it is the
only difference between the channels. A file is what an INI document is, so this reads
one - and `tools/oracle/ini_cp_gen.py` generates lone-CR documents precisely because
the choice is visible.

**The configuration is pinned and printed by the gate**, because five configurations of
`ConfigParser` give five readings of one document:

  interpolation=None           - interpolation is a pass over an assembled value and
                                 not part of the grammar. It is also the trap in this
                                 dialect: over the 479 real documents on this machine
                                 the *default* `BasicInterpolation` refuses a value in
                                 **301** of them and changes a value in **none**.
  strict=True                  - the default. A duplicate section or key is an error.
  allow_no_value=False         - the default. A line with no delimiter is an error.
  inline_comment_prefixes=None - the default. `k = a ; c` keeps the whole value.
  empty_lines_in_values=True   - the default. A blank line inside a value is a line.
  default_section=<unspellable> - **not** the default, and the one other deliberate
                                 departure: `[DEFAULT]`'s value inheritance is a lookup
                                 policy over a parsed tree rather than a rule of the
                                 grammar, and leaving it on would make every section's
                                 item list include another section's keys. With the
                                 name set to something no document can spell,
                                 `[DEFAULT]` is an ordinary section on both sides.

Copyright 2026 by Corey Pennycuff
"""

import configparser
import os
import sys
import tempfile

NO_DEFAULT_SECTION = "\x00nodefault"


def hexed(text):
    """Hex, with `.` for the empty string.

    One spelling shared with tools/oracle/ini_cp_ours.c. The first version of this
    comparison had the two sides encode an empty value differently and reported 368
    differences that were entirely its own.
    """
    return text.encode("utf-8").hex() or "."


def read_block(stream):
    header = stream.readline()
    if not header:
        return None
    try:
        length = int(header.decode("ascii", "replace").strip())
    except ValueError:
        return None
    if length < 0:
        return None
    data = stream.read(length)
    if len(data) != length:
        sys.exit(3)
    return data


def answer(data, out):
    out.write("BEGIN\n")
    handle, path = tempfile.mkstemp()
    try:
        os.write(handle, data)
        os.close(handle)
        parser = configparser.ConfigParser(interpolation=None,
            default_section=NO_DEFAULT_SECTION)
        try:
            parser.read(path, encoding="utf-8")
        except Exception as failure:
            out.write("cp err %s\n" % type(failure).__name__)
            out.write("END\n")
            return
        out.write("cp ok\n")
        for section in parser.sections():
            out.write("G %s\n" % hexed(section))
            for key, value in parser.items(section, raw=True):
                out.write("E %s %s\n" % (hexed(key), hexed(value or "")))
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    out.write("END\n")


def main(argv):
    if len(argv) > 1 and argv[1] == "--version":
        sys.stdout.write("python %s, configparser in the standard library\n"
                         % sys.version.split()[0])
        return 0
    stream = sys.stdin.buffer
    while True:
        data = read_block(stream)
        if data is None:
            break
        answer(data, sys.stdout)
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
