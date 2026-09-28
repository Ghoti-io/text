#!/usr/bin/env python3
"""Turn the published meta-schemas into a C file the library embeds.

Every dialect this library reads: 2020-12's nine documents, 2019-09's seven,
and one each for draft-07, draft-06 and draft-04.

The documents are fetched by tools/metaschema/fetch.sh into
third_party/json-schema/<date>/ and are not committed; the file this writes
is, so that a build needs neither the network nor Python. `make
check-metaschema` regenerates into a temporary directory and diffs, which is
what stops the committed copy from drifting away from what is published.

The bytes are embedded verbatim - indentation, key order and all. Nothing here
reformats or minifies them. A normalised copy would be a different document
that happens to mean the same thing, and then the diff gate would be checking
this script's idea of the meta-schema rather than the published one.

Usage:  tools/metaschema/gen_metaschema.py [--ucd DIR] [--out DIR]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import sys

PREFIX = "https://json-schema.org/draft/"
OLD_PREFIX = "http://json-schema.org/"

# The order the file lists them in: newest dialect first, and within each the
# root, then the vocabulary meta-schemas in the order the root's `allOf`
# references them.
#
# 2020-12 ends with format-assertion, which the root does not reference
# because the vocabulary it describes is the optional one. 2019-09 has neither
# that nor `unevaluated`: it has a single `format` vocabulary, and it keeps
# `unevaluatedItems` and `unevaluatedProperties` in `applicator`.
DIALECTS = [
    ("2020-12", ["core", "applicator", "unevaluated", "validation",
                 "meta-data", "format-annotation", "format-assertion",
                 "content"]),
    ("2019-09", ["core", "applicator", "validation", "meta-data", "format",
                 "content"]),
]

# draft-07, draft-06 and draft-04, which have no vocabulary split to list:
# `$vocabulary` arrived in 2019-09, so before it the keyword set is the draft
# itself and each of these publishes one document.
#
# The URI is `http` and has no `/draft/` segment, and it is recorded here
# *without* the empty fragment the document's own `$id` carries. That is not a
# tidying: a reference is resolved to an absolute URI and its fragment read
# afterwards, so `http://json-schema.org/draft-07/schema#` and
# `...schema` both arrive at this table as the same key. `load()` accepts either
# spelling of `$id` for exactly that reason.
OLD_DIALECTS = ["draft-07", "draft-06", "draft-04"]

DOCUMENTS = []
for _draft, _vocabs in DIALECTS:
    _base = PREFIX + _draft
    DOCUMENTS.append((_draft, "schema.json", _base + "/schema"))
    for _vocab in _vocabs:
        DOCUMENTS.append(
            (_draft, "meta/%s.json" % _vocab, _base + "/meta/" + _vocab))
for _draft in OLD_DIALECTS:
    DOCUMENTS.append((_draft, "schema.json", OLD_PREFIX + _draft + "/schema"))


def c_identifier(uri):
    """`.../2019-09/meta/meta-data` -> `2019_09_meta_meta_data`."""
    for prefix in (PREFIX, OLD_PREFIX):
        if uri.startswith(prefix):
            tail = uri[len(prefix):]
            break
    else:
        raise ValueError("no prefix for %r" % uri)
    return tail.replace("/", "_").replace("-", "_")


def c_string(chunk):
    """One C string literal for one line of the source document."""
    out = []
    for byte in chunk:
        ch = chr(byte)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif 0x20 <= byte < 0x7F:
            out.append(ch)
        else:
            # Should not arise - every published document is printable ASCII -
            # but an octal escape is correct rather than clever if one day one
            # is not, and `\xNN` would swallow a following hex digit.
            out.append("\\%03o" % byte)
    return '"' + "".join(out) + '"'


def load(directory, relative, uri):
    path = os.path.join(directory, relative)
    try:
        with open(path, "rb") as handle:
            raw = handle.read()
    except OSError as exc:
        sys.exit("%s: %s\nrun tools/metaschema/fetch.sh first" % (path, exc))

    if not raw:
        sys.exit("%s is empty" % path)
    if any(byte >= 0x80 for byte in raw):
        sys.exit("%s is not ASCII; the embedding assumes it is" % path)
    if b"\r" in raw:
        sys.exit("%s has CRLF line endings; refetch it" % path)
    # No stored length travels with the document: its size is the sum of its
    # lines' lengths, measured where they are joined. A NUL would make that sum
    # wrong, so it is refused here rather than silently truncating a document.
    if b"\0" in raw:
        sys.exit("%s contains a NUL byte; the embedding assumes it does not"
                 % path)

    # The fetch could have been answered with the wrong document, or with a
    # page about the document. Both parse as something; only the right one
    # calls itself by the URI it was asked for.
    try:
        parsed = json.loads(raw.decode("ascii"))
    except ValueError as exc:
        sys.exit("%s is not JSON: %s" % (path, exc))
    if not isinstance(parsed, dict):
        sys.exit("%s is not a JSON object" % path)
    # Either spelling of the identifier, and either spelling of the URI.
    # draft-04 calls the keyword `id`, which is what draft-06 renamed to `$id`.
    # The older drafts' own value carries an empty fragment and the newer ones'
    # does not; a fragment names a location *within* a document and the empty
    # one names the whole of it, so the two are the same identity and this table
    # is keyed by the fragmentless form.
    declared = parsed.get("$id", parsed.get("id"))
    if declared not in (uri, uri + "#"):
        sys.exit("%s declares %r, expected %r" % (path, declared, uri))

    return raw


# Every generated source carries the same licence notice as a hand-written
# one. It is emitted here rather than added afterwards, so that regenerating
# does not quietly drop it.
LICENSE_NOTICE = """\
/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
"""


def emit(handle, documents):
    handle.write(
        LICENSE_NOTICE + "\n"
        "/**\n"
        " * @file\n"
        " *\n"
        " * The published JSON Schema meta-schemas, verbatim: 2020-12's nine\n"
        " * documents, 2019-09's seven, and one each for draft-07, draft-06\n"
        " * and draft-04.\n"
        " *\n"
        " * GENERATED by tools/metaschema/gen_metaschema.py from the documents\n"
        " * tools/metaschema/fetch.sh retrieves. Do not edit: `make\n"
        " * check-metaschema` regenerates this file and fails on a difference.\n"
        " *\n"
        " * These are somebody else's documents and are reproduced unchanged,\n"
        " * under the terms they are published with. They are embedded rather\n"
        " * than fetched because the alternative is a validator that opens a\n"
        " * connection during a compile, to a URI taken from the document it was\n"
        " * asked to compile.\n"
        " *\n"
        " * One string literal per source line, joined when a document is used.\n"
        " * See metaschema_internal.h for why a document is not one literal.\n"
        " */\n"
        "\n"
        "#include \"metaschema_internal.h\"\n")

    for uri, raw in documents:
        name = c_identifier(uri)
        handle.write("\n/* %s */\n" % uri)
        handle.write(
            "static const char * const metaschema_%s[] = {\n" % name)
        for line in raw.splitlines(True):
            handle.write("    %s,\n" % c_string(line))
        handle.write("};\n")

    handle.write("\nconst json_metaschema_doc json_metaschema_docs[] = {\n")
    for uri, _raw in documents:
        name = c_identifier(uri)
        handle.write("    {\"%s\",\n" % uri)
        handle.write("        metaschema_%s,\n" % name)
        handle.write("        sizeof(metaschema_%s)\n" % name)
        handle.write("            / sizeof(metaschema_%s[0])},\n" % name)
    handle.write("};\n\n")
    handle.write("const size_t json_metaschema_doc_count =\n"
                 "    sizeof(json_metaschema_docs)"
                 " / sizeof(json_metaschema_docs[0]);\n")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--documents",
        default=os.path.join(root, "third_party", "json-schema"),
        help="where fetch.sh put the published documents")
    parser.add_argument(
        "--out",
        default=os.path.join(root, "src", "json", "metaschema"),
        help="directory to write metaschema_docs.c into")
    args = parser.parse_args()

    documents = [
        (uri, load(os.path.join(args.documents, draft), relative, uri))
        for draft, relative, uri in DOCUMENTS
    ]

    os.makedirs(args.out, exist_ok=True)
    target = os.path.join(args.out, "metaschema_docs.c")
    with open(target, "w", encoding="ascii", newline="\n") as handle:
        emit(handle, documents)

    total = sum(len(raw) for _, raw in documents)
    print("wrote %s: %d documents, %d bytes of JSON"
          % (target, len(documents), total))


if __name__ == "__main__":
    main()
