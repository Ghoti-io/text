#!/usr/bin/env python3
"""Score the INI reader over this machine's systemd unit files.

Usage: systemd_unit_suite.py <runner> <dir> [<dir> ...]

Two scores, each with its denominator derived from the corpus at run time and
printed, because **this corpus is machine-specific**: it is whatever unit files this
machine happens to carry, so a count taken here is not a count anywhere else and a
hardcoded floor would be a claim about somebody else's disk. A zero denominator
**fails** - a corpus gate whose denominator is "however many files I found" prints a
clean sweep on an empty directory.

  parses     - the reader accepts the file under the systemd dialect
  round trip - writing it back produces the same bytes

**The interesting number this gate prints is a set of zeros**, and they are the
reason the differential exists. It counts how many files use each of the constructs
the dialect turns on, and on this machine a `;` comment, CRLF, a byte-order mark and
**every escape sequence** score zero, while a line continuation scores 2 of 165. So a
clean run over this corpus says almost nothing about the rules the dialect is hardest
to get right, and printing the zeros is what keeps that from being mistaken for
coverage.

There is a second reason the corpus cannot be the gate here: **this machine has no
systemd**. The files are shipped by other packages; there is no `systemd-analyze` to
say whether they are valid, and PID 1 is `init`. So unlike the Desktop Entry corpus,
which comes with a validator, this one can only be asked whether we accept and
preserve it - never whether we are right to.

Copyright 2026 by Corey Pennycuff
"""

import os
import subprocess
import sys

SUFFIXES = (".service", ".socket", ".timer", ".target", ".mount", ".automount",
            ".path", ".slice", ".scope", ".swap", ".device", ".conf", ".network",
            ".netdev", ".link", ".nspawn", ".dnssd", ".preset")

# The constructs the dialect exists for. Counted over the corpus and printed, zeros
# included, because a zero is the finding.
def census(data):
    """Which of the dialect's constructs this file's bytes contain."""
    found = set()
    lines = data.split(b"\n")
    for line in lines:
        stripped = line.strip(b" \t")
        if stripped.endswith(b"\\"):
            found.add("continuation")
        if stripped.startswith(b";"):
            found.add("semicolon comment")
        if stripped.startswith(b"#"):
            found.add("hash comment")
    if b"\r\n" in data:
        found.add("CRLF")
    if data.startswith(b"\xef\xbb\xbf"):
        found.add("BOM")
    if any(b > 127 for b in data):
        found.add("non-ASCII byte")
    if b'"' in data or b"'" in data:
        found.add("a quote character")
    if b"\\x" in data or b"\\u" in data or b"\\U" in data:
        found.add("a numeric escape")
    if b"\\t" in data or b"\\n" in data or b"\\s" in data:
        found.add("a letter escape")
    # A repeated key in one section, which is the dialect's list spelling.
    section = None
    seen = {}
    for line in lines:
        stripped = line.strip(b" \t")
        if stripped.startswith(b"[") and stripped.endswith(b"]"):
            section = stripped
            continue
        if b"=" in stripped and not stripped.startswith((b"#", b";")):
            key = (section, stripped.split(b"=", 1)[0].strip(b" \t"))
            seen[key] = seen.get(key, 0) + 1
    if any(n > 1 for n in seen.values()):
        found.add("a repeated key")
    return found


CONSTRUCTS = ("continuation", "semicolon comment", "hash comment", "CRLF", "BOM",
              "non-ASCII byte", "a quote character", "a numeric escape",
              "a letter escape", "a repeated key")


def corpus(dirs):
    """Every unit-shaped file under the given directories, sorted and de-duplicated.

    By **real** path, and it matters twice over. `/lib` is a symlink to `/usr/lib` on
    this machine and on every merged-/usr distribution, and a `.wants` directory is
    full of symlinks to unit files one level up. Counting paths instead of files gave
    470 where there are 165 - a denominator inflated by 185%, every duplicate a file
    already counted. An inflated denominator makes a score look more thorough than it
    is, which is the one direction a corpus count must never be wrong in.
    """
    found = {}
    for d in dirs:
        for root, _, names in os.walk(d):
            for name in sorted(names):
                if not name.endswith(SUFFIXES):
                    continue
                path = os.path.join(root, name)
                found[os.path.realpath(path)] = path
    return [found[key] for key in sorted(found)]


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: systemd_unit_suite.py <runner> <dir> [<dir>...]\n")
        return 2
    runner, dirs = argv[1], argv[2:]
    files = corpus(dirs)
    print("corpus: %d unit files under %s" % (len(files), " ".join(dirs)))
    if not files:
        print("FAIL the corpus is empty; a gate over no files is not a gate")
        return 1

    parses = [0, 0]
    roundtrip = [0, 0]
    tally = {name: 0 for name in CONSTRUCTS}
    failures = []
    for path in files:
        data = open(path, "rb").read()
        for name in census(data):
            if name in tally:
                tally[name] += 1
        result = subprocess.run([runner, path], capture_output=True)
        if result.returncode != 0:
            failures.append("%s: the runner exited %d" % (path, result.returncode))
            continue
        line = result.stdout.decode("utf-8", "replace").strip()
        parses[1] += 1
        if line.startswith("ok"):
            parses[0] += 1
            roundtrip[1] += 1
            if "same" in line:
                roundtrip[0] += 1
            else:
                failures.append("%s: parsed and did not write back" % path)
        else:
            failures.append("%s: %s" % (path, line))

    for line in failures[:20]:
        print("FAIL " + line)
    print("parses      %4d of %4d" % (parses[0], parses[1]))
    print("round trip  %4d of %4d" % (roundtrip[0], roundtrip[1]))
    print("what the corpus actually contains, zeros included:")
    for name in CONSTRUCTS:
        print("    %-20s %4d of %d files" % (name, tally[name], len(files)))
    zeros = [n for n in CONSTRUCTS if not tally[n]]
    if zeros:
        print("  ** %s appear in NO file here, so this gate says nothing about them."
              % ", ".join(zeros))
        print("     That is what make check-ini-systemd-oracle is for.")
    bad = bool(failures) or parses[0] != parses[1] or roundtrip[0] != roundtrip[1]
    print("FAIL" if bad else "PASS: every unit file parses and writes back")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
