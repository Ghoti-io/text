#!/usr/bin/env python3
"""The second EditorConfig reference: editorconfig-core-py, over the same batch
protocol as driver.c so that one run of the gate can ask both.

    editorconfig-py-driver --version
    editorconfig-py-driver < batch

Protocol, identical to containers/editorconfig/driver.c: per document, `<len>\\n`
then that many bytes of the document, then `<len>\\n` then that many bytes of the
query filename. `-1\\n` ends the stream. Output per document:

    BEGIN
    ec ok | ec err <line> | ec fail <code>
    V <hex name> <hex value>
    END

`ec err 0` is used where core-py reports a parsing error without a usable line
number, because its exception carries a list of bad lines rather than the first
one - the gate only asks whether the document was refused, and by whom.

**The conf file is not called `.editorconfig`.** core-py walks upward from the
queried path merging every conf file it finds, so a stray `.editorconfig` in /tmp
or / would silently join every document in the run.

Copyright 2026 by Corey Pennycuff
"""

import os
import sys
import tempfile

from editorconfig import VERSION
from editorconfig.exceptions import ParsingError
from editorconfig.handler import EditorConfigHandler

CONF_NAME = ".gtext-ec"
DRIVER_SHA = os.environ.get("GTEXT_PY_DRIVER_SHA", "unknown")


def read_block(stream):
    """Read `<len>\\n<len bytes>`. Returns None at end of stream."""
    header = stream.readline()
    if not header:
        return None
    length = int(header.strip() or -1)
    if length < 0:
        return None
    data = b""
    while len(data) < length:
        chunk = stream.read(length - len(data))
        if not chunk:
            raise SystemExit(3)
        data += chunk
    return data


def put_hex(out, data):
    if data is None:
        out.write("-")
    elif not data:
        out.write(".")
    else:
        out.write(data.hex())


def main(argv):
    if len(argv) > 1 and argv[1] == "--version":
        major, minor, patch, _ = VERSION
        print(f"editorconfig-core-py {major}.{minor}.{patch}, "
              f"driver {DRIVER_SHA}")
        return 0

    stream = sys.stdin.buffer
    out = sys.stdout
    with tempfile.TemporaryDirectory(prefix="gtext-ecpy-") as directory:
        conf = os.path.join(directory, CONF_NAME)
        while True:
            doc = read_block(stream)
            if doc is None:
                break
            name = read_block(stream)
            if name is None:
                break
            with open(conf, "wb") as f:
                f.write(doc)
            # The queried path need not exist: core-py matches the name.
            query = os.path.join(directory,
                                 name.decode("utf-8", "surrogateescape"))
            out.write("BEGIN\n")
            try:
                handler = EditorConfigHandler(query, CONF_NAME)
                options = handler.get_configurations()
                out.write("ec ok\n")
                for key, value in options.items():
                    out.write("V ")
                    put_hex(out, key.encode("utf-8", "surrogateescape"))
                    out.write(" ")
                    put_hex(out, value.encode("utf-8", "surrogateescape"))
                    out.write("\n")
            except ParsingError:
                # Refused. core-py raises at end of file and discards the
                # options it had collected, so there is nothing to report.
                out.write("ec err 0\n")
            except UnicodeDecodeError:
                # Not a verdict about the grammar: core-py opens the file as
                # UTF-8 and a document with an invalid byte never reaches its
                # parser at all. The gate excludes these and counts them.
                out.write("ec fail -2\n")
            out.write("END\n")
    out.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
