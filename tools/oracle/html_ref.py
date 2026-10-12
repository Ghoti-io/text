#!/usr/bin/env python3
"""Asking the two HTML references, html5lib and parse5, in their pinned image.

Shared by html_vectors.py (does each reference reproduce the standard's own
tree-construction vectors?) and html_diff.py (does the XHTML write mode survive
them?). Nothing here judges an answer; it frames requests, runs the two drivers
side by side, and refuses to proceed if the image is not the one pinned.

The protocol is containers/html/driver.py's: one JSON request per line, one JSON
answer per line, in order. A reference that cannot answer a request (html5lib is
given a fragment context in a foreign namespace, which its API cannot express)
says so in an `error` field and that is an answer, not a missing one.

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

REFERENCES = ("html5lib", "parse5")
DRIVER_FILES = ("driver.py", "driver.mjs")


def driver_sha():
    """The SHA-256 of both driver files as the Dockerfile computes it.

    The image prints this in its version line; a gate that compares it to the
    files in this tree notices an image built from an older copy of either.
    """
    digest = hashlib.sha256()
    for name in DRIVER_FILES:
        with open(os.path.join(HERE, "containers", "html", name), "rb") as fh:
            digest.update(fh.read())
    return digest.hexdigest()


def check_image():
    """Raise unless the image answering is the pinned one, built from these drivers."""
    line = oracle_env.check_pin("html")
    want = driver_sha()
    if oracle_env.MODE != "host" and want not in line:
        raise oracle_env.OracleUnavailable(
            "the html image's drivers are not the ones in this tree (driver SHA-256 %s):\n  %s\n"
            "Rebuild it with: docker build -t <the tag in IMAGES> tools/oracle/containers/html" % (want, line))
    return line


def request(html, scripting=False, context=None):
    return {"html": html, "scripting": scripting, "context": context}


def _run_one(which, payload, cmd, out):
    done = subprocess.run(cmd, input=payload, capture_output=True)
    if done.returncode != 0:
        out[which] = ("fail", "the %s driver exited %d:\n%s" % (
            which, done.returncode,
            oracle_env.reference_stderr(done.stderr.decode("utf-8", "replace"))[-2000:]))
        return
    out[which] = ("ok", done.stdout.decode("ascii").split("\n"))


def ask_all(requests, chunk=20000):
    """{reference: [answer]} for each request, both references run concurrently."""
    check_image()
    # Resolved here, on one thread: oracle_env fills its tables lazily and is not safe to
    # enter from two threads at once.
    commands = {name: oracle_env.command("html", ["html-driver", name]) for name in REFERENCES}
    answers = {name: [] for name in REFERENCES}
    for start in range(0, len(requests), chunk):
        part = requests[start:start + chunk]
        payload = ("\n".join(json.dumps(r, ensure_ascii=True) for r in part) + "\n").encode("ascii")
        out = {}
        threads = [threading.Thread(target=_run_one, args=(name, payload, commands[name], out)) for name in REFERENCES]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        for name in REFERENCES:
            kind, value = out[name]
            if kind == "fail":
                raise SystemExit(value)
            lines = [l for l in value if l]
            if len(lines) != len(part):
                raise SystemExit("%s: sent %d requests, got %d answers" % (name, len(part), len(lines)))
            answers[name].extend(json.loads(l) for l in lines)
    return answers


def _label_key(line):
    # The dump's own order: UTF-16 code units of the label (the part before `=`), which for the labels here
    # is code point order.
    return [ord(c) for c in line.split("=", 1)[0]]


def normalize_dump(dump):
    """A reference's dump in the web-platform-tests format.

    The drivers write an `xmlns` attribute in the XMLNS namespace as `xmlns="..."`, the label of an attribute in
    no namespace; the WPT serializer (resources/test.js) writes any attribute with a namespace as its prefix
    word and local name, so `xmlns xmlns="..."`. The attributes of an SVG or MathML element that are named xmlns
    are always the namespaced one (the parser makes them so), so the rewrite is made under those elements only,
    and the attributes of such an element are sorted again, since the new label sorts elsewhere (`xmlns xlink`
    before `xmlns xmlns`).
    """
    if not isinstance(dump, str) or "xmlns=" not in dump:
        return dump
    lines = dump.split("\n")
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        out.append(line)
        i += 1
        if not line.startswith("| "):
            continue
        body = line[2:].lstrip(" ")
        indent = len(line) - 2 - len(body)
        if not (body.startswith("<") and not body.startswith("<!") and not body.startswith("<?") and body.endswith(">")):
            continue
        if not (body[1:].startswith("svg ") or body[1:].startswith("math ")):
            continue
        # The attribute lines of this element: the lines one level in that are not elements, text or comments.
        run = []
        while i < len(lines) and lines[i].startswith("| " + " " * (indent + 2)):
            inner = lines[i][2 + indent + 2:]
            if inner.startswith("<") or inner.startswith('"') or inner == "content" or inner.startswith(" "):
                break
            if inner.startswith("xmlns="):
                inner = "xmlns " + inner
            run.append("| " + " " * (indent + 2) + inner)
            i += 1
        run.sort(key=lambda l: _label_key(l[2 + indent + 2:]))
        out.extend(run)
    return "\n".join(out)
