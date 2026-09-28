#!/usr/bin/env python3
"""Read a batch of TOML documents with `tomllib`, as the reference sees them.

Runs *inside* the pinned image. `tomllib` has no version of its own - it is part
of CPython - so what the pin names is the interpreter, and that is what the
first line of output says.

The protocol is a batch, for the reason `nfc_ask.py` gives: a container
invocation per document would be thousands of them, and a protocol that dropped
or reordered one would compare against the wrong document, which is worse than
no comparison because it prints as a disagreement.

Framing is by length rather than by line, because a TOML document contains
newlines and may contain anything else. Each request is a decimal byte count on
its own line followed by exactly that many bytes; the batch ends at end of
input. One response line per request, in order:

  ok <json>     the document's values, JSON-encoded
  err <text>    tomllib refused it, or the bytes are not UTF-8

A date-time has no JSON spelling, so it comes back as `["date-time", "<text>"]`
in the canonical form tools/conformance/toml_native.py defines - imported from
the mounted tree rather than restated here, so that the two sides of the
comparison cannot disagree about whether `Z` and `+00:00` are one offset.

Copyright 2026 by Corey Pennycuff
"""

import datetime
import json
import os
import sys
import tomllib

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "conformance"))
from toml_native import normalise_datetime  # noqa: E402


def jsonable(value):
    """The reading, in a shape json.dumps can write and the host can compare."""
    if isinstance(value, dict):
        return dict((key, jsonable(item)) for key, item in value.items())
    if isinstance(value, list):
        return [jsonable(item) for item in value]
    if isinstance(value, (datetime.datetime, datetime.date, datetime.time)):
        return ["date-time", normalise_datetime(value.isoformat())]
    return value


def requests(stream):
    """Each framed document, as bytes."""
    while True:
        header = stream.readline()
        if not header:
            return
        header = header.strip()
        if not header:
            continue
        count = int(header)
        body = stream.read(count)
        if len(body) != count:
            raise SystemExit("toml_ask: short read: wanted %d, got %d"
                             % (count, len(body)))
        yield body


def main():
    out = sys.stdout
    out.write("version python %s, tomllib in the standard library\n"
              % sys.version.split()[0])
    for body in requests(sys.stdin.buffer):
        try:
            text = body.decode("utf-8")
        except UnicodeDecodeError as why:
            out.write("err not UTF-8: %s\n" % why)
            continue
        try:
            value = tomllib.loads(text)
        except tomllib.TOMLDecodeError as why:
            out.write("err %s\n" % str(why).replace("\n", " "))
            continue
        except RecursionError:
            out.write("err tomllib recursed past its limit\n")
            continue
        out.write("ok %s\n" % json.dumps(jsonable(value), allow_nan=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
