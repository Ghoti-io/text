#!/usr/bin/env python3
"""html5lib's encoding sniffer, for tools/oracle/html_sniff_diff.py.

    /opt/html-oracle/venv/bin/python3 sniff.py          one JSON request per line on stdin, one JSON answer per line on stdout

A request is

    {"hex": "<the document's bytes>", "transport": <label or null>, "likely": <label or null>,
     "default": <label or null>, "ignore_meta": <bool>, "mode": "sniff" | "parse"}

`sniff` is html5lib's `HTMLBinaryInputStream.determineEncoding` with `useChardet` off: the byte order mark, the transport label
(certain), the `<meta>` prescan of the first 1024 bytes, the "likely" encoding (tentative: the nearest thing html5lib has to a
tentative label from the caller) and the default. `parse` is `HTMLParser.parse` on the bytes (a restart on a late `<meta>`
included) and the encoding it ends with, `documentEncoding`. `ignore_meta` stubs the prescan and the parser's `<meta>` rule
out, which html5lib has no option for.

`{"labels": true}` is answered with the list of every label webencodings knows. An answer is {"enc": <webencodings name or null>, "conf": <"certain" | "tentative" | null>} or {"error": "<what>"}. The file is read
from this tree on the bind mount that makes the tree visible in the container; the image is not rebuilt for it. Nothing here
judges an answer.

Copyright 2026 by Corey Pennycuff
"""

import json
import sys

import html5lib
from html5lib import _inputstream, html5parser


def main():
    # `ignore_meta`: html5lib has no such option; both places it reads a declaration are replaced for the request.
    prescan = _inputstream.HTMLBinaryInputStream.detectEncodingMeta
    change = _inputstream.HTMLBinaryInputStream.changeEncoding
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        req = json.loads(line)
        if req.get("labels"):  # every label webencodings knows, for the label-table comparison
            import webencodings
            sys.stdout.write(json.dumps(sorted(webencodings.LABELS)) + "\n")
            sys.stdout.flush()
            continue
        data = bytes.fromhex(req["hex"])
        if req.get("ignore_meta"):
            _inputstream.HTMLBinaryInputStream.detectEncodingMeta = lambda self: None
            _inputstream.HTMLBinaryInputStream.changeEncoding = lambda self, new: None
        else:
            _inputstream.HTMLBinaryInputStream.detectEncodingMeta = prescan
            _inputstream.HTMLBinaryInputStream.changeEncoding = change
        kwargs = {"transport_encoding": req.get("transport"), "likely_encoding": req.get("likely"),
                  "default_encoding": req.get("default") or "windows-1252", "useChardet": False}
        try:
            if req.get("mode") == "parse":
                p = html5parser.HTMLParser()
                p.parse(data, **kwargs)
                out = {"enc": p.documentEncoding, "conf": None}
            else:
                s = _inputstream.HTMLBinaryInputStream(data, **kwargs)
                enc, conf = s.charEncoding
                out = {"enc": enc.name, "conf": conf}
        except Exception as e:  # an answer too: a reference that cannot answer says so
            out = {"error": "%s: %s" % (type(e).__name__, e)}
        sys.stdout.write(json.dumps(out) + "\n")
        sys.stdout.flush()  # (a question asked while the program runs is answered at once)


if __name__ == "__main__":
    main()
