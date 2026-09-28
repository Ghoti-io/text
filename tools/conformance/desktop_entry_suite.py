#!/usr/bin/env python3
"""Score the INI reader over a corpus of real Desktop Entry files.

Usage: desktop_entry_suite.py <runner> <dir> [<dir> ...]

Three scores, each with its denominator derived from the corpus at run time and
printed, because **this corpus is machine-specific**: it is whatever `.desktop`
files this machine has installed, so a count taken here is not a count anywhere
else and a hardcoded floor would be a claim about somebody else's disk.

  parses     - the reader accepts the file under the Desktop Entry dialect
  round trip - writing it back produces the same bytes (specification §3)
  parity     - the generic dialect produces the same tree, over the files that
               contain no CR: `accept_crlf` is the one generic-dialect change
               that alters a value rather than widening acceptance, so those
               files are excluded and counted instead of scored

A zero denominator **fails**. A corpus gate whose denominator is "however many
files I found" prints a clean sweep on an empty directory, and that is the
failure this repository keeps meeting: a sweep that cannot see returns clean.

What this gate cannot do is test the rules the corpus does not exercise. Every
file here is already valid, so none of them carries a duplicate key, a `;`
comment, an indented line, a BOM, CRLF or a non-ASCII name - the refusals are
tested in tests/test-ini.cpp and, when it exists, by the generated differential
against GKeyFile. This scores acceptance and preservation, and says so.

Copyright 2026 by Corey Pennycuff
"""

import os
import subprocess
import sys


def corpus(dirs):
    """Every .desktop file under the given directories, sorted."""
    found = []
    for d in dirs:
        for root, _, names in os.walk(d):
            found += [os.path.join(root, n) for n in sorted(names)
                      if n.endswith('.desktop')]
    return sorted(found)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    runner, dirs = sys.argv[1], sys.argv[2:]
    files = corpus(dirs)
    print('corpus: %d .desktop files under %s' % (len(files), ', '.join(dirs)))
    if not files:
        print('FAIL: the corpus is empty, so every score below would be a '
              'vacuous 0 of 0')
        return 1

    proc = subprocess.run([runner], input='\n'.join(files) + '\n',
                          capture_output=True, text=True)
    if proc.returncode != 0:
        print('FAIL: the runner exited %d' % proc.returncode)
        print(proc.stderr[:4000])
        return 1
    records = [r.split('\t') for r in proc.stdout.split('\n') if r]
    if len(records) != len(files):
        # A count mismatch means the runner lost or invented a record, and every
        # ratio below would be computed over the wrong denominator.
        print('FAIL: %d records for %d files' % (len(records), len(files)))
        return 1

    parsed = [r for r in records if r[1] == 'ok']
    same = [r for r in parsed if r[2] == 'same']
    generic_ok = [r for r in parsed if r[3] == 'ok']
    # The generic dialect's CRLF handling is the one change that is not a
    # relaxation, so a file containing a CR is excluded from parity and counted
    # rather than scored either way.
    cr = [r for r in parsed if r[4] == 'skip:cr']
    comparable = [r for r in parsed if r[4] != 'skip:cr']
    parity = [r for r in comparable if r[4] == 'same']

    print('parses        %4d of %4d' % (len(parsed), len(records)))
    print('round trip    %4d of %4d  (byte-identical, specification 3)'
          % (len(same), len(parsed)))
    print('generic reads %4d of %4d' % (len(generic_ok), len(parsed)))
    print('generic parity%4d of %4d  (same tree as the strict dialect; %d '
          'excluded for containing a CR, which the generic dialect normalises)'
          % (len(parity), len(comparable), len(cr)))
    if not comparable:
        print('FAIL: every file contains a CR, so the parity score is vacuous')
        return 1

    failures = [r for r in records
                if r[1] != 'ok' or r[2] != 'same' or r[3] != 'ok'
                or r[4] not in ('same', 'skip:cr')]
    for r in failures[:20]:
        print('  %s: strict=%s roundtrip=%s generic=%s parity=%s %s'
              % (r[0], r[1], r[2], r[3], r[4], r[5] if len(r) > 5 else ''))
    if len(failures) > 20:
        print('  ... and %d more' % (len(failures) - 20))

    if failures:
        print('FAIL: %d of %d files did not meet all four' % (len(failures),
              len(records)))
        return 1
    print('PASS: %d files parse, rewrite byte for byte, and read identically '
          'under both dialects' % len(records))
    return 0


if __name__ == '__main__':
    sys.exit(main())
