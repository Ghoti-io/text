#!/usr/bin/env python3
"""Asking the Encoding references: Node's `TextDecoder` and `TextEncoder`, CPython's codecs, and this library.

Shared by encoding_diff.py (the differentials) and encoding_wpt.py (the web-platform-tests runner). Nothing here judges an answer; it
frames requests, runs the drivers side by side, and refuses to proceed if the image is not the one pinned.

**Two references in the html image** (the one containers/IMAGES pins as `encoding`, which is the `html` image under another question):

- Node, with full ICU. `TextDecoder` and `TextEncoder` are the Encoding Standard's API, so the *label table*, *streaming*, *fatal* and
  *ignoreBOM* are the standard's own and are scored; the decoders under them are ICU's converters (Node 20 with ICU 76.1), which agree
  with the standard for UTF-8 and UTF-16 and differ from its indexes in places. Where they differ the standard decides and the list
  of encoding_divergences.txt says which place.
- CPython 3.13's codecs: a second reading where they agree with the standard, listed where they do not.

The drivers (containers/html/encoding.mjs and encoding.py) are read from this tree on the bind mount that makes the tree visible in the
container at its own path; the image is not rebuilt for them. What the pin checks is Node's version, ICU's and Python's.

Copyright 2026 by Corey Pennycuff
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import oracle_env  # noqa: E402

CONTAINER = os.path.join(HERE, "containers", "html")
NODE_DRIVER = os.path.join(CONTAINER, "encoding.mjs")
PY_DRIVER = os.path.join(CONTAINER, "encoding.py")


def check_image():
    """Raise unless the image answering is the pinned one."""
    return oracle_env.check_pin("encoding")


def hexs(b):
    return "-" if not b else b.hex()


def unhex(s):
    return b"" if s == "-" else bytes.fromhex(s)


def node_command(*args):
    return oracle_env.command("encoding", ["node", NODE_DRIVER] + [str(a) for a in args])


def python_command(*args):
    return oracle_env.command("encoding", ["python3", PY_DRIVER] + [str(a) for a in args])


def ask(command, lines):
    """The answers (one line each) of a line-protocol driver to `lines`, all at once."""
    payload = ("\n".join(lines) + "\n").encode("ascii")
    done = subprocess.run(command, input=payload, capture_output=True)
    if done.returncode != 0:
        raise SystemExit("a reference exited %d:\n%s" % (
            done.returncode, oracle_env.reference_stderr(done.stderr.decode("utf-8", "replace"))[-2000:]))
    answers = done.stdout.decode("ascii").split("\n")
    if answers and answers[-1] == "":
        answers.pop()
    if len(answers) != len(lines):
        raise SystemExit("sent %d requests, got %d answers" % (len(lines), len(answers)))
    return answers


def ask_node(lines, chunk=20000):
    check_image()
    cmd = node_command()
    out = []
    for i in range(0, len(lines), chunk):
        out.extend(ask(cmd, lines[i:i + chunk]))
    return out


def ask_python(lines, chunk=50000):
    check_image()
    cmd = python_command()
    out = []
    for i in range(0, len(lines), chunk):
        out.extend(ask(cmd, lines[i:i + chunk]))
    return out


def enum_stream(command):
    """The raw bytes a driver's `enum` command writes."""
    done = subprocess.run(command, capture_output=True)
    if done.returncode != 0:
        raise SystemExit("an enum command exited %d:\n%s" % (
            done.returncode, oracle_env.reference_stderr(done.stderr.decode("utf-8", "replace"))[-2000:]))
    return done.stdout


def records(stream):
    """The records of an enum stream: bytes for an output, None for a failure."""
    out = []
    i = 0
    n = len(stream)
    while i < n:
        length = stream[i]
        i += 1
        if length == 255:
            out.append(None)
        else:
            out.append(stream[i:i + length])
            i += length
    return out
