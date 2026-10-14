#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Text.  See COPYING.LESSER.
"""The one measured set of numbers for the XML, HTML, Encoding Standard and markup work (Phase 3k, part B).

    tools/phase3-counts.py [--tests]

Everything printed is counted from the file or the listing that defines it, at the moment this runs: the vectors of the pinned
suites, the exception and divergence lists, the plants, the fuzz seeds, the quadratic shapes (tools/html/counts.py asks the suite
itself) and the Encoding module's own counts (tools/encoding/counts.py). `--tests` also asks each built test program for its list
of tests (`--gtest_list_tests`; build them first) and prints the tests per family. The plan record
(notes/text/XML-HTML-PLAN.md, "Phase 3k as built") quotes this output with the date and the commit; no other document types a number
that this prints. Re-run it when a list changes and compare: `tools/phase3-counts.py > /tmp/c; diff` against the plan's block.
"""

import collections
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TP = os.path.join(ROOT, "third_party")


def lines_of(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return [l.rstrip("\n") for l in fh if l.strip() and not l.startswith("#")]


def tracked(pattern):
    """The seed files of a corpus directory: the *.seed files on disk, which are the ones git tracks (the grown corpora beside them are ignored)."""
    return sorted(glob.glob(os.path.join(ROOT, pattern.replace("/*", "/*.seed"))))


def count_dat(path):
    n = 0
    with open(path, encoding="utf-8", errors="replace") as fh:
        for l in fh:
            if l.startswith("#data"):
                n += 1
    return n


def out(label, value):
    print("%-58s %s" % (label + ":", value))


def xml():
    print("== XML")
    conf = os.path.join(TP, "xml", "xmlconf")
    kinds = collections.Counter()
    total = 0
    for f in glob.glob(os.path.join(conf, "**", "*.xml"), recursive=True):
        try:
            txt = open(f, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for m in re.finditer(r"<TEST\b[^>]*\bTYPE=\"(\w[\w-]*)\"", txt):
            kinds[m.group(1)] += 1
            total += 1
    # the catalogue files each contain TEST elements; the top-level xmlconf.xml only includes them by entity
    out("W3C XML Conformance Suite test cases (all catalogues)", "%d (%s)" % (total, ", ".join("%s %d" % kv for kv in sorted(kinds.items()))))
    out("xmlconf exceptions (XMLCONF_EXCEPTIONS)", len(lines_of(os.path.join(ROOT, "tools", "conformance", "XMLCONF_EXCEPTIONS"))))
    out("reference disagreements, libxml2 and expat (xml_disagreements.txt)", len(lines_of(os.path.join(ROOT, "tools", "oracle", "xml_disagreements.txt"))))
    src = open(os.path.join(ROOT, "tools", "xml", "plant-fuzz.py")).read()
    block = src[src.index("PLANTS = {"):]
    out("fuzz plants (tools/xml/plant-fuzz.py)", len(re.findall(r'^    "[a-z0-9-]+":', block.split("\n}\n")[0], re.M)))
    for d in ("xml", "xml-writer"):
        seeds = tracked("tests/fuzz/corpus/%s/*" % d)
        out("fuzz seeds, tests/fuzz/corpus/%s" % d, "%d (%d reproducers)" % (len(seeds), sum(1 for s in seeds if "/reproducer-" in s)))


def html():
    print("== HTML")
    tok = 0
    for f in glob.glob(os.path.join(TP, "html", "html5lib-tests", "tokenizer", "*.test")):
        try:
            data = json.load(open(f, encoding="utf-8"))
        except Exception:
            continue
        tok += len(data.get("tests", data.get("xmlViolationTests", [])))
    out("tokenizer vectors (html5lib-tests/tokenizer, files)", "%d tests in %d files" % (tok, len(glob.glob(os.path.join(TP, "html", "html5lib-tests", "tokenizer", "*.test")))))
    res = os.path.join(TP, "html", "wpt", "html", "syntax", "parsing", "resources")
    dats = glob.glob(os.path.join(res, "*.dat"))
    out("tree-construction vectors (wpt .../parsing/resources/*.dat)", "%d in %d files" % (sum(count_dat(d) for d in dats), len(dats)))
    ser = 0
    for f in glob.glob(os.path.join(TP, "html", "html5lib-tests", "serializer", "*.test")):
        try:
            ser += len(json.load(open(f, encoding="utf-8")).get("tests", []))
        except Exception:
            pass
    out("serializer vectors (html5lib-tests/serializer)", ser)
    enc = glob.glob(os.path.join(TP, "html", "html5lib-tests", "encoding", "*.dat"))
    out("encoding vectors (html5lib-tests/encoding/*.dat)", sum(count_dat(d) for d in enc))
    for name, f in (("tokenizer exceptions", "HTML_TOKENIZER_EXCEPTIONS"), ("tree exceptions", "HTML_TREE_EXCEPTIONS"),
                    ("serializer exceptions", "HTML_SERIALIZER_EXCEPTIONS"), ("encoding exceptions", "HTML_ENCODING_EXCEPTIONS"),
                    ("serializer plants", "HTML_SERIALIZER_PLANTS")):
        out("%s (%s)" % (name, f), len(lines_of(os.path.join(ROOT, "tools", "conformance", f))))
    for name, f in (("tree-vector deviations of the references", "html_deviations.txt"), ("tree differential divergences", "html_tree_divergences.txt"),
                    ("tokenizer differential divergences", "html_tokenizer_divergences.txt"), ("XHTML/HTML differential divergences", "html_divergences.txt"),
                    ("serializer differential divergences", "html_serialize_divergences.txt"), ("sniffing differential divergences", "html_sniff_divergences.txt")):
        out("%s (%s)" % (name, f), len(lines_of(os.path.join(ROOT, "tools", "oracle", f))))
    for name, f in (("serializer oracle plants", "HTML_SERIALIZE_ORACLE_PLANTS"), ("sniffing oracle plants", "HTML_SNIFF_ORACLE_PLANTS")):
        out("%s (%s)" % (name, f), len(lines_of(os.path.join(ROOT, "tools", "oracle", f))))
    src = open(os.path.join(ROOT, "tools", "html", "plant-fuzz.py")).read()
    block = src[src.index("PLANTS = {"):].split("\n}\n")[0]
    per = collections.Counter(h for _, h in re.findall(r'^    "([a-z0-9-]+)":\s*\(\s*"([a-z-]+)"', block, re.M))
    out("fuzz plants (tools/html/plant-fuzz.py)", "%d (%s)" % (sum(per.values()), ", ".join("%s %d" % kv for kv in sorted(per.items()))))
    for d in ("html-tokenizer", "html-tree", "html", "html-serialize"):
        seeds = tracked("tests/fuzz/corpus/%s/*" % d)
        out("fuzz seeds, tests/fuzz/corpus/%s" % d, "%d (%d reproducers)" % (len(seeds), sum(1 for s in seeds if "/reproducer-" in s)))
    p = subprocess.run([sys.executable, os.path.join(HERE, "html", "counts.py")], capture_output=True, text=True)
    print(p.stdout.rstrip())
    if p.returncode != 0:
        print("(tools/html/counts.py failed: %s)" % p.stderr.strip()[-200:])


def encoding():
    print("== Encoding Standard")
    p = subprocess.run([sys.executable, os.path.join(HERE, "encoding", "counts.py")], capture_output=True, text=True)
    print(p.stdout.rstrip())
    seeds = tracked("tests/fuzz/corpus/encoding/*")
    out("fuzz seeds tracked, tests/fuzz/corpus/encoding", "%d (%d reproducers)" % (len(seeds), sum(1 for s in seeds if "/reproducer-" in s)))


def markup():
    print("== Markup tree")
    seeds = tracked("tests/fuzz/corpus/markup-tree/*")
    out("fuzz seeds tracked, tests/fuzz/corpus/markup-tree", len(seeds))


def tests():
    print("== Test programs (gtest listings)")
    apps = os.path.join(ROOT, "build", "linux", "release", "apps")
    fam = collections.OrderedDict((k, [0, 0]) for k in ("Xml", "Xhtml", "Html", "Encoding", "Markup", "Quadratic", "Coverage"))
    for exe in sorted(glob.glob(os.path.join(apps, "test*"))):
        name = os.path.basename(exe)
        if name.endswith(".d") or not os.access(exe, os.X_OK):
            continue
        key = next((k for k in fam if name.startswith("test" + k)), None)
        if key is None:
            continue
        env = dict(os.environ, LD_LIBRARY_PATH=apps + ":" + os.environ.get("LD_LIBRARY_PATH", ""))
        r = subprocess.run([exe, "--gtest_list_tests"], capture_output=True, text=True, env=env)
        n = sum(1 for l in r.stdout.splitlines() if l.startswith("  ") and not l.startswith("   "))
        # a parameterised test is listed once per parameter; "  name/3" style lines are counted as listed
        fam[key][0] += 1
        fam[key][1] += n
    for k, (progs, n) in fam.items():
        out("test programs and tests, tests/test-%s*.cpp" % k.lower(), "%d programs, %d tests" % (progs, n))
    out("all of the above", "%d programs, %d tests" % (sum(v[0] for v in fam.values()), sum(v[1] for v in fam.values())))


def main(argv):
    xml()
    html()
    encoding()
    markup()
    if "--tests" in argv:
        tests()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
