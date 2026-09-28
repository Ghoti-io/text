"""The second TOML reader, asked through its pin rather than through PATH.

`tomllib` is the only second implementation anywhere in this library's TOML
measurements: the toml-test corpus's encode rows read the writer's output with
it, and `tools/oracle/toml_diff.py` compares the reader against it over generated
documents. Both used to reach it differently, and the first of them reached it by
`import tomllib` in the scoring script - whatever `python3` this machine happened
to have, of a version nothing recorded.

That is the shape this repository spent a day removing from its Unicode gates. A
reference nobody chose is not a weaker measurement, it is a measurement of an
unknown: `tomllib` is part of CPython and changes with it, so "the writer's output
is readable" meant "readable by this machine today".

So there is one way to ask, it goes through `tools/oracle/oracle_env.py`, and the
version it answers with is printed above the numbers. `GHOTI_ORACLE_MODE=host`
still uses this machine's own interpreter and says so - that hatch exists for a
machine with no container engine, which is exactly the machine whose `python3` is
not the pinned one.

The protocol is a batch. A container invocation per case would be hundreds of
them for the corpus and tens of thousands for the differential; framing is by
length rather than by line because a TOML document contains newlines and may
contain anything else.

Copyright 2026 by Corey Pennycuff
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ORACLE = os.path.join(os.path.dirname(HERE), "oracle")
sys.path.insert(0, ORACLE)

import oracle_env  # noqa: E402

NAME = "tomllib"
OracleUnavailable = oracle_env.OracleUnavailable


def provenance():
    """One line naming the reference that will answer. Raises if it cannot."""
    return oracle_env.provenance([NAME])


def as_native(payload):
    """The reference's JSON in the shape toml_native's comparisons expect.

    A date-time has no JSON spelling, so `toml_ask.py` writes it as
    `["date-time", "<canonical>"]`; here it becomes the tuple `actual_native()`
    produces for one, so that one comparison serves both sides.
    """
    if isinstance(payload, list):
        if (len(payload) == 2 and payload[0] == "date-time"
                and isinstance(payload[1], str)):
            return (payload[0], payload[1])
        return [as_native(item) for item in payload]
    if isinstance(payload, dict):
        return dict((key, as_native(item)) for key, item in payload.items())
    return payload


def read_all(documents):
    """Each document as ('ok', native) or ('err', why), in order.

    One invocation for the whole batch. Raises OracleUnavailable if the reference
    cannot be reached, if it exits non-zero, or if it answers a different number
    of documents than it was asked - that last one is not paranoia, it is the
    difference between a comparison and a comparison against the wrong document,
    which prints as a disagreement.
    """
    argv = oracle_env.command(
        NAME, ["python3", os.path.join(ORACLE, "toml_ask.py")])
    request = []
    for text in documents:
        body = text.encode("utf-8") if isinstance(text, str) else text
        request.append(b"%d\n" % len(body))
        request.append(body)
    finished = subprocess.run(argv, input=b"".join(request),
                              capture_output=True)
    if finished.returncode != 0:
        raise OracleUnavailable(
            "the reference exited %d\n%s"
            % (finished.returncode,
               oracle_env.reference_stderr(
                   finished.stderr.decode("utf-8", "replace"))))
    lines = finished.stdout.decode("utf-8", "replace").split("\n")
    if not lines or not lines[0].startswith("version "):
        raise OracleUnavailable(
            "the reference did not say what it is: %r" % lines[:1])
    answers = [line for line in lines[1:] if line]
    if len(answers) != len(documents):
        raise OracleUnavailable(
            "asked %d documents and got %d answers"
            % (len(documents), len(answers)))
    out = []
    for answer in answers:
        if answer.startswith("ok "):
            out.append(("ok", as_native(json.loads(answer[3:]))))
        else:
            out.append(("err", answer[4:]))
    return out
