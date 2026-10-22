#!/usr/bin/env python3
"""Asking the three CSS references, tinycss2, cssparser and csstree, in their pinned image.

Shared by the differential (css_diff.py). Nothing here judges an answer; it frames requests, runs the
drivers side by side, and refuses to proceed if the image is not the one pinned.

The protocol is containers/css/driver.py's: one request per line, `<mode> <options> <hex of the
input>`, one JSON answer per line, in order. A reference that has no entry point for a mode says so
({"unsupported": true}) and one that raised says so ({"crash": "..."}): both are answers.

Copyright 2026 by Corey Pennycuff
"""

import hashlib
import json
import os
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import oracle_env  # noqa: E402

REFERENCES = ("tinycss2", "cssparser", "csstree")
CONTEXT = os.path.join(HERE, "containers", "css")
# In the order the Dockerfile concatenates them.
DRIVER_FILES = ("driver.py", "driver.mjs", "driver-rs/Cargo.toml", "driver-rs/Cargo.lock", "driver-rs/src/main.rs")


def driver_sha():
    """The SHA-256 of the driver sources as the Dockerfile computes it.

    The image prints this in its version line; a gate that compares it to the files in this tree
    notices an image built from an older copy of any of them.
    """
    digest = hashlib.sha256()
    for name in DRIVER_FILES:
        with open(os.path.join(CONTEXT, name), "rb") as fh:
            digest.update(fh.read())
    return digest.hexdigest()


def check_image():
    """Raise unless the image answering is the pinned one, built from these drivers."""
    line = oracle_env.check_pin("css")
    want = driver_sha()
    if oracle_env.MODE != "host" and want not in line:
        raise oracle_env.OracleUnavailable(
            "the css image's drivers are not the ones in this tree (driver SHA-256 %s):\n  %s\n"
            "Rebuild it with: make oracle-images" % (want, line))
    return line


def request_line(mode, data, proto=None, env=None):
    opts = []
    if proto is not None:
        opts.append("proto=" + proto.encode("ascii", "replace").hex())
    if env is not None:
        opts.append("env=" + env.encode("ascii", "replace").hex())
    return "%s %s %s" % (mode, ",".join(opts) if opts else "-", data.hex() if data else "-")


def _run_batch(cmd, lines):
    """The answers to `lines` from one run of the driver. If the driver dies (a reference that runs out of memory or
    time on one input is a finding, not a failure of the run), the batch is halved until the request that kills it is
    alone, and that request is answered {"crash": "the driver died"}."""
    payload = ("\n".join(lines) + "\n").encode("ascii")
    done = subprocess.run(cmd, input=payload, capture_output=True)
    if done.returncode == 0:
        answers = [l for l in done.stdout.decode("ascii").split("\n") if l]
        if len(answers) == len(lines):
            return [json.loads(l) for l in answers]
    if len(lines) == 1:
        return [{"crash": "the driver died (exit %d)" % done.returncode}]
    mid = len(lines) // 2
    return _run_batch(cmd, lines[:mid]) + _run_batch(cmd, lines[mid:])


def _run_one(which, lines, cmd, out):
    try:
        out[which] = ("ok", _run_batch(cmd, lines))
    except Exception as exc:  # a failure of the harness itself
        out[which] = ("fail", "the %s driver: %s" % (which, exc))


def ask_all(requests, chunk=20000):
    """{reference: [answer]} for each request, the references run concurrently.

    `requests` is {reference: [request line]}: each reference is asked its own list.
    """
    check_image()
    commands = {name: oracle_env.command("css", ["css-driver", name]) for name in requests}
    answers = {name: [] for name in requests}
    longest = max((len(v) for v in requests.values()), default=0)
    for start in range(0, longest, chunk):
        out = {}
        threads = []
        parts = {}
        for name, lines in requests.items():
            part = lines[start:start + chunk]
            parts[name] = part
            if not part:
                out[name] = ("ok", [])
                continue
            threads.append(threading.Thread(target=_run_one, args=(name, part, commands[name], out)))
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        for name in requests:
            kind, value = out[name]
            if kind == "fail":
                raise SystemExit(value)
            if len(value) != len(parts[name]):
                raise SystemExit("%s: sent %d requests, got %d answers" % (name, len(parts[name]), len(value)))
            answers[name].extend(value)
    return answers
