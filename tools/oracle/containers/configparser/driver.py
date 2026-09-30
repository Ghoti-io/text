#!/usr/bin/env python3
"""The `configparser` reference, framed for the batch protocol.

    driver.py --version
    driver.py [--interpolation=none|basic|extended] < <stream of documents>

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

                                 **`--interpolation` is the one pin the gate does not
                                 only print**, and the reason is that a pin is wider
                                 than an exclusion: it removes a behaviour from the
                                 comparison entirely, so nothing would fail if this
                                 module's handling of `%` changed or if the
                                 reference's did. So `tools/oracle/ini_cp_diff.py`
                                 runs this driver three times - `none` for the five
                                 scores, and `basic` and `extended` to compare
                                 `gtext_ini_value_interpolate()` against the thing it
                                 is a reimplementation of, and to assert that the
                                 default configuration still answers differently from
                                 the pinned one on the axes that say so.
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


INTERPOLATIONS = {
    "none": None,
    "basic": configparser.BasicInterpolation,
    "extended": configparser.ExtendedInterpolation,
}


def answer(data, out, which):
    """One document's record, for the configuration @p which names.

    **Every line is built before any is written**, which matters only once
    interpolation is on and then matters a great deal: `read()` stores values raw
    and `get()` is what interpolates, so `InterpolationSyntaxError` is raised
    while the items are being *enumerated* rather than while the file is being
    read. Writing as it went emitted a `G` line and some `E` lines and then an
    error, giving a record that is neither an acceptance nor a refusal. Measured:
    `[s]\na = 100%\n` reads without complaint under `BasicInterpolation`.

    A per-value failure is reported as a refusal of the whole document, because
    that is what a caller of `parser.items(section)` sees - the exception comes out
    of the iteration and there is no partial result to have.
    """
    lines = []
    handle, path = tempfile.mkstemp()
    try:
        os.write(handle, data)
        os.close(handle)
        style = INTERPOLATIONS[which]
        parser = configparser.ConfigParser(
            interpolation=style() if style else None,
            default_section=NO_DEFAULT_SECTION)
        try:
            parser.read(path, encoding="utf-8")
            for section in parser.sections():
                lines.append("G %s" % hexed(section))
                # raw=True under `none` keeps that run byte-identical to what it
                # was before this parameter existed; the two are the same call
                # there, because `interpolation=None` installs a pass-through.
                for key, value in parser.items(section, raw=style is None):
                    lines.append("E %s %s" % (hexed(key), hexed(value or "")))
        except Exception as failure:
            lines = ["cp err %s" % type(failure).__name__]
        else:
            lines.insert(0, "cp ok")
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    out.write("BEGIN\n")
    for line in lines:
        out.write(line + "\n")
    out.write("END\n")


def main(argv):
    if len(argv) > 1 and argv[1] == "--version":
        sys.stdout.write("python %s, configparser in the standard library\n"
                         % sys.version.split()[0])
        return 0
    which = "none"
    for arg in argv[1:]:
        if arg.startswith("--interpolation="):
            which = arg.split("=", 1)[1]
        else:
            sys.exit("unknown argument %r" % arg)
    if which not in INTERPOLATIONS:
        sys.exit("--interpolation must be one of %s"
                 % ", ".join(sorted(INTERPOLATIONS)))
    stream = sys.stdin.buffer
    while True:
        data = read_block(stream)
        if data is None:
            break
        answer(data, sys.stdout, which)
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
