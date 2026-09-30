#!/usr/bin/env python3
"""Score the INI reader against `configparser` over this machine's .cfg and .ini files.

Usage: configparser_suite.py <runner> <dir> [<dir> ...]

**This is the only INI corpus gate whose reference is installed**, and it changes what
a corpus can be asked. The Desktop Entry corpus has a validator but no value reader
that agrees with ours about everything; the systemd corpus has neither, because this
machine has no systemd. Here the reference *is* `configparser`, it is in the standard
library of the interpreter running this script, and it can be asked all three
questions about every file:

  verdict    - we accept exactly the files it accepts, and refuse exactly the rest
  values     - every section, key and value agrees, after the indent join
  round trip - writing the document back produces the original bytes

The interpreter version is printed, because it is the reference and a different one is
a different reference. It is deliberately the **host's** rather than a pinned
container's: this gate is about the files on this machine, and
`make check-ini-configparser-oracle` is the pinned comparison over generated
documents.

**A `.cfg` or `.ini` extension does not mean the file is one of these documents**, and
that is the first thing this gate measures rather than assumes. Of the 703 files found
here, 224 are not configparser documents at all - most `lit.cfg` files are Python
scripts, and `/etc/dpkg/dpkg.cfg` is a section-less key-value file. So those 224 are
not noise to be filtered out: they are the only **refusals** a real corpus of this
format offers, and a reader that accepted them would be wrong in the direction no
generated document here tests. They stay in the denominator.

**78 files are excluded from the values score and counted**, for one reason:
`configparser` strips Unicode whitespace and this reader strips ASCII whitespace, so a
value that is a lone no-break space is empty there and one byte long here. The
exclusion predicate is narrow on purpose - it fires only where the two strips actually
reach a different byte - and the gate **asserts that every excluded file really does
differ**. That is the check that keeps an exclusion from widening quietly: a predicate
broader than the defect would start excluding files that agree, and the assertion
would fail.

INI_CONFIGPARSER_DIRS overrides the directories searched.

Copyright 2026 by Corey Pennycuff
"""

import collections
import configparser
import os
import re
import subprocess
import sys

SUFFIXES = (".cfg", ".ini", ".pypirc")

# A section name no document can spell, so `[DEFAULT]` is an ordinary section here.
# The default section's value inheritance is a lookup policy over a parsed tree and
# not a rule of the grammar; this dialect does not implement it, and leaving it on
# would make every section's item list include another section's keys.
NO_DEFAULT_SECTION = "\x00nodefault"

# Every character Python calls whitespace and this reader does not, because none of
# them is a single byte. Listed rather than derived so that the set a run used is in
# the file that used it.
NONASCII_WS = set("\x85\xa0         "
                  "       　")

# What this reader strips: ASCII, and including the four separator controls Python's
# `\s` has and C's `isspace()` does not.
ASCII_WS = " \t\v\f\x1c\x1d\x1e\x1f"


def unicode_strip_bites(text):
    """Whether Python's strip would reach a character this reader's would not.

    Narrow on purpose. A no-break space in the *middle* of a value is data to both,
    so a file containing one is still scorable; the two answers differ only where
    such a character sits at a boundary a strip moves to. Applied to the line, to
    the key and to the value, because all three are stripped.

    The wide predicate - "the file contains any non-ASCII whitespace at all" -
    excluded 115 files where 78 differ, so 37 agreeing files would have left the
    score. An exclusion is allowed to be conservative and is not allowed to be
    unmeasured, which is why the caller asserts that every file this returns True
    for really does disagree.
    """
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        stripped = line.strip(ASCII_WS)
        if stripped and (stripped[0] in NONASCII_WS or stripped[-1] in NONASCII_WS):
            return True
        found = re.search(r"[=:]", stripped)
        if not found:
            continue
        for part in (stripped[:found.start()], stripped[found.end():]):
            part = part.strip(ASCII_WS)
            if part and (part[0] in NONASCII_WS or part[-1] in NONASCII_WS):
                return True
    return False


def hexed(text):
    """Hex, with `.` for the empty string - the spelling the C runner uses.

    A shared spelling and not two, because the first version had the runner print `.`
    and this print nothing, and the comparison then reported 368 differences that were
    entirely its own. A differential's two sides have to be encoded by one rule.
    """
    return text.encode("utf-8").hex() or "."


def corpus(dirs):
    """Every .cfg/.ini file under the given directories, by **real** path.

    De-duplicated the way the systemd gate learned to: `/lib` is a symlink to
    `/usr/lib` on any merged-/usr distribution, and counting paths instead of files
    inflates the denominator in the one direction a corpus count must never be wrong
    in.
    """
    found = {}
    for directory in dirs:
        for root, _, names in os.walk(directory):
            for name in sorted(names):
                if not name.endswith(SUFFIXES):
                    continue
                path = os.path.join(root, name)
                found[os.path.realpath(path)] = path
    return [found[key] for key in sorted(found)]


def reference(path):
    """`(accepted, records)` for one file, as `configparser` reads it.

    Records are hex-encoded to match the runner's output exactly: a value here can
    contain a newline, so a text format cannot carry one unambiguously.
    """
    parser = configparser.ConfigParser(interpolation=None,
        default_section=NO_DEFAULT_SECTION)
    try:
        parser.read(path, encoding="utf-8")
    except Exception as failure:
        return False, type(failure).__name__
    records = []
    for section in parser.sections():
        records.append("G " + hexed(section))
        for key, value in parser.items(section, raw=True):
            records.append("E %s %s" % (hexed(key), hexed(value or "")))
    return True, records


def ours(runner, path):
    """`(accepted, records, rewrite)` for one file, as this module reads it."""
    result = subprocess.run([runner, path], capture_output=True)
    if result.returncode != 0:
        return None, "the runner exited %d" % result.returncode, None
    lines = result.stdout.decode("utf-8", "replace").splitlines()
    if not lines:
        return None, "the runner printed nothing", None
    if lines[0].startswith("cp err"):
        return False, lines[0], None
    records = [line for line in lines[1:] if not line.startswith("W ")]
    rewrite = next((line[2:] for line in lines if line.startswith("W ")), "missing")
    return True, records, rewrite


# The constructs the dialect exists for, counted over the corpus and printed with
# their zeros, because a zero is what says the gate is silent about a rule.
CONSTRUCTS = ("a continuation", "a `:` separator", "a `;` comment", "a `#` comment",
              "CRLF", "a lone CR", "a BOM", "a non-ASCII byte", "an inline `;`",
              "a quote character", "a backslash", "a bare `%`", "an interpolation",
              "a `[DEFAULT]` section", "junk after the `]`")


def census(data):
    """Which of the dialect's constructs this file's bytes contain."""
    found = set()
    lines = data.split(b"\n")
    for line in lines:
        bare = line.strip(b" \t\v\f\x1c\x1d\x1e\x1f\r")
        if bare.startswith(b";"):
            found.add("a `;` comment")
        if bare.startswith(b"#"):
            found.add("a `#` comment")
        if line[:1] in (b" ", b"\t") and bare and not bare.startswith((b"#", b";")):
            found.add("a continuation")
        if bare.startswith(b"[") and b"]" in bare and bare.rsplit(b"]", 1)[1].strip():
            found.add("junk after the `]`")
        if bare == b"[DEFAULT]":
            found.add("a `[DEFAULT]` section")
        head = re.split(rb"[=:]", bare, 1)
        if len(head) == 2 and not bare.startswith((b"#", b";", b"[")):
            if b"=" not in bare or (b":" in bare and bare.index(b":") < bare.index(b"=")):
                found.add("a `:` separator")
            if b";" in head[1]:
                found.add("an inline `;`")
    if b"\r\n" in data:
        found.add("CRLF")
    if re.search(rb"\r(?!\n)", data):
        found.add("a lone CR")
    if data.startswith(b"\xef\xbb\xbf"):
        found.add("a BOM")
    if any(byte > 127 for byte in data):
        found.add("a non-ASCII byte")
    if b'"' in data or b"'" in data:
        found.add("a quote character")
    if b"\\" in data:
        found.add("a backslash")
    if b"%" in data:
        found.add("a bare `%`")
    if b"%(" in data or b"${" in data:
        found.add("an interpolation")
    return found


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: configparser_suite.py <runner> <dir> [<dir>...]\n")
        return 2
    runner, dirs = argv[1], argv[2:]
    files = corpus(dirs)
    print("reference:  CPython %s, configparser from its standard library"
          % sys.version.split()[0])
    print("            interpolation=None, strict=True, allow_no_value=False,")
    print("            inline_comment_prefixes=None, empty_lines_in_values=True,")
    print("            default_section=<unspellable>")
    print("corpus:     %d unique .cfg/.ini files under %s" % (len(files), " ".join(dirs)))
    if not files:
        print("FAIL the corpus is empty; a gate over no files is not a gate")
        return 1

    verdict = [0, 0]
    values = [0, 0]
    rewrite = [0, 0]
    divergence = [0, 0]
    tally = {name: 0 for name in CONSTRUCTS}
    refused_kinds = collections.Counter()
    failures = []
    for path in files:
        try:
            data = open(path, "rb").read()
        except OSError as failure:
            failures.append("%s: cannot read it: %s" % (path, failure))
            continue
        for name in census(data):
            tally[name] += 1
        ref_ok, ref_records = reference(path)
        our_ok, our_records, our_rewrite = ours(runner, path)
        if our_ok is None:
            failures.append("%s: %s" % (path, our_records))
            continue
        verdict[1] += 1
        if our_ok == ref_ok:
            verdict[0] += 1
        else:
            failures.append("%s: reference %s, we %s (%s)"
                % (path, "accepted" if ref_ok else "refused",
                   "accepted" if our_ok else "refused",
                   ref_records if not ref_ok else our_records))
        if not ref_ok:
            refused_kinds[ref_records] += 1
        if not (ref_ok and our_ok):
            continue

        rewrite[1] += 1
        if our_rewrite == "same":
            rewrite[0] += 1
        else:
            failures.append("%s: the rewrite is %s" % (path, our_rewrite))

        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = ""
        if unicode_strip_bites(text):
            # Excluded from `values` and asserted here instead: the departure must
            # still be observable, or the predicate has grown wider than the defect.
            divergence[1] += 1
            if ref_records != our_records:
                divergence[0] += 1
            else:
                failures.append("%s: excluded for Unicode whitespace and yet it "
                    "agrees, so the exclusion is wider than the defect" % path)
            continue
        values[1] += 1
        if ref_records == our_records:
            values[0] += 1
        else:
            first = next((i for i, (a, b) in enumerate(zip(ref_records, our_records))
                          if a != b), min(len(ref_records), len(our_records)))
            failures.append("%s: record %d differs\n      ref %s\n      our %s"
                % (path, first,
                   ref_records[first] if first < len(ref_records) else "<missing>",
                   our_records[first] if first < len(our_records) else "<missing>"))

    for line in failures[:20]:
        print("FAIL " + line)
    print("verdict     %4d of %4d  (accept or refuse, every file)" % tuple(verdict))
    print("values      %4d of %4d  (sections, keys and joined values)" % tuple(values))
    print("round trip  %4d of %4d  (byte for byte)" % tuple(rewrite))
    print("divergence  %4d of %4d  (excluded for Unicode whitespace, and still"
          " differing)" % tuple(divergence))
    print("the reference refuses %d of these files, which is what makes this corpus"
          % sum(refused_kinds.values()))
    print("  able to score a refusal at all:")
    for kind, count in refused_kinds.most_common():
        print("    %-28s %4d" % (kind, count))
    print("what the corpus actually contains, zeros included:")
    for name in CONSTRUCTS:
        print("    %-24s %4d of %d files" % (name, tally[name], len(files)))
    zeros = [name for name in CONSTRUCTS if not tally[name]]
    if zeros:
        print("  ** %s appear in NO file here, so this gate says nothing about them."
              % ", ".join(zeros))
        print("     make check-ini-configparser-oracle is what does.")
    bad = (bool(failures) or verdict[0] != verdict[1] or values[0] != values[1]
           or rewrite[0] != rewrite[1] or divergence[0] != divergence[1])
    print("FAIL" if bad else "PASS: every file agrees with configparser, or is "
          "excluded for a measured reason and counted")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
