#!/usr/bin/env python3
#
# Fail if a file on ALLOCATOR_CLEAN_SOURCES reaches memory the caller's
# allocator cannot see, by calling into a file that is not on the list and does
# allocate.
#
# `make check-allocators` reads one file at a time, so it cannot see a call.
# A listed file calling an unlisted one is how fifteen strdup()s and a whole
# streaming parser stayed invisible to it: every line in the listed file was
# clean, and the memory came from the next file along.
#
# The obvious form of this check - "every callee of a listed file must itself be
# listed" - answers a nearby question and is useless. Run that way the library
# reports sixty violations, and almost all of them are calls to things like
# gtext_json_parse_options_default() or toml_fail(), which allocate nothing at
# all and have no business on a list about allocation. A check nobody can act on
# is a check that gets turned off.
#
# So the predicate here is narrower and is the one that matters: a callee is
# reported only if the file defining it **contains a raw allocation that is not
# marked exempt**. That is the signature of memory the caller cannot reach.
# Everything else is a call between files, which is not a finding.
#
# What this still cannot see, and no textual check can: a listed file calling an
# allocator-aware entry point and passing the *wrong* allocator. The call is
# well-formed, the file is listed, and only the choice of allocator is wrong.
# That shape needs a behavioural test with two allocators - see
# Allocator.YamlWriteAnchorSetComesFromTheWriteNotTheDocument.
#
# Exit status is 0 when the only findings are in ALLOWED below, 1 otherwise.
# Pass --report to list every finding, allowed ones included, and always exit 0.
#
# Copyright 2026 by Corey Pennycuff

import os
import re
import sys

# Findings that are known, argued, and not fixed here. Each entry is
# (defining file, reason). A file in this list is NOT a pass - it is a recorded
# gap, and the reason has to say what would close it.
# Empty, and the entry that used to be here is worth keeping a record of,
# because its reason was wrong in a way this script caused. It read:
#
#   src/text_number.c: gtext_number_strtod()'s respelling buffer [...] Closing
#   it means an allocator parameter on gtext_number_strtod() AND
#   gtext_number_format_*(), which is a signature change at seven call sites
#   across five files.
#
# gtext_number_format_i64(), _u64() and _format_double() allocate nothing at
# all - they are snprintf with a bounds check. They appeared in the reason
# because **this script reports a file, not a function**: the predicate is
# "the file defining the callee contains a raw allocation", so every entry
# point in a file with one allocation anywhere is named as a caller. Written
# into the allowance as the cost of the fix, that made the job look about
# three times larger than it was, and pointed it at four functions that could
# not have been the problem. Only gtext_number_strtod() needed the parameter:
# seven call sites, in seven files, not five.
#
# A file-level finding read as a function-level one is the shape to watch for
# in anything this script prints.
ALLOWED = {}


def listed_sources(makefile='Makefile'):
    """The ALLOCATOR_CLEAN_SOURCES list, read from the makefile itself.

    Read rather than passed in, so the check cannot drift from the list it is
    about.
    """
    text = open(makefile, encoding='utf-8', errors='replace').read()
    m = re.search(r'^ALLOCATOR_CLEAN_SOURCES :=(.*?)\n\n', text, re.S | re.M)
    if not m:
        sys.stderr.write('ALLOCATOR_CLEAN_SOURCES not found in %s\n' % makefile)
        sys.exit(2)
    return set(re.findall(r'(src/\S+\.c)', m.group(1)))


RAW = re.compile(
    r'(^|[^_A-Za-z0-9])(malloc|calloc|realloc|free|strdup|strndup)\s*\(')
COMMENT = re.compile(r'\s*(\*|//|/\*)')


def raw_allocations(path):
    """Lines in @p path that allocate outside GTEXT_Allocator.

    The same three exclusions `make check-allocators` makes, so that the two
    agree about what counts: a gtext_allocator_* call, an `allocator-exempt`
    marker on the line, and a comment line.
    """
    found = []
    try:
        lines = open(path, encoding='utf-8', errors='replace').readlines()
    except OSError:
        return found
    for number, line in enumerate(lines, 1):
        if 'gtext_allocator_' in line or 'allocator-exempt' in line:
            continue
        if COMMENT.match(line):
            continue
        if RAW.search(line):
            found.append((number, line.strip()))
    return found


# A definition, as opposed to a call or a declaration: starts at column 0 and
# the line does not end the statement. Imperfect by construction - this is not a
# C parser - but it errs towards missing a definition rather than inventing one,
# and a missed definition can only cost a finding, never create one.
DEFINITION = re.compile(r'^(?:[A-Za-z_][\w \t\*]*?)\b([a-z_][A-Za-z0-9_]*)\s*\([^;]*$')
CALL = re.compile(r'\b([a-z_][A-Za-z0-9_]*)\s*\(')
NOT_CALLS = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'defined',
             'do', 'else', 'case'}


def c_sources(root='src'):
    out = []
    for directory, _, files in os.walk(root):
        for name in files:
            if name.endswith('.c'):
                out.append(os.path.join(directory, name))
    return sorted(out)


def main():
    report_only = '--report' in sys.argv[1:]
    listed = listed_sources()
    sources = c_sources()

    definitions = {}
    for path in sources:
        for line in open(path, encoding='utf-8', errors='replace'):
            if line.startswith((' ', '\t', '#', '/', '*', '}')):
                continue
            m = DEFINITION.match(line.rstrip())
            if m:
                definitions.setdefault(m.group(1), path)

    # Which unlisted files allocate, computed once.
    allocating = {p: raw_allocations(p) for p in sources if p not in listed}
    allocating = {p: v for p, v in allocating.items() if v}

    findings = {}
    for path in sorted(listed):
        if not os.path.exists(path):
            sys.stderr.write('listed but missing: %s\n' % path)
            return 2
        for line in open(path, encoding='utf-8', errors='replace'):
            for name in CALL.findall(line):
                if name in NOT_CALLS:
                    continue
                definer = definitions.get(name)
                if definer and definer != path and definer in allocating:
                    findings.setdefault(definer, set()).add((name, path))

    unexpected = {k: v for k, v in findings.items() if k not in ALLOWED}

    for definer in sorted(findings):
        allowed = definer in ALLOWED
        if allowed and not report_only:
            continue
        tag = 'allowed' if allowed else 'NOT ALLOWED'
        print('%s  (%d raw allocation(s))  [%s]'
              % (definer, len(allocating[definer]), tag))
        for number, text in allocating[definer]:
            print('    %s:%d: %s' % (definer, number, text))
        for name, caller in sorted(findings[definer]):
            print('    called as %s() from %s' % (name, caller))
        if allowed:
            print('    reason: %s' % ALLOWED[definer])
        print()

    for path in sorted(ALLOWED):
        if path not in findings:
            print('STALE ALLOWANCE: %s is no longer reached from a listed '
                  'file, so its entry in ALLOWED should go.' % path)

    if report_only:
        return 0

    if unexpected:
        sys.stderr.write(
            '\n### A file that must use GTEXT_Allocator calls into one that '
            'does not ###\n\n'
            'Every allocation above happens while serving a caller who supplied '
            'an allocator,\nand comes from the C library instead. '
            '`make check-allocators` cannot see it: it\nreads one file at a '
            'time, and each line of the listed file is clean.\n\n'
            'Either convert the callee and add it to ALLOCATOR_CLEAN_SOURCES, '
            'or - if the\nmemory genuinely cannot take a caller allocator - '
            'mark the lines\n`allocator-exempt` and record the reason in '
            "this script's ALLOWED table.\n")
        return 1

    n = len(ALLOWED)
    if n == 0:
        print('\033[0;32mNo listed file reaches unlisted memory, and there '
              'are no recorded gaps.\033[0m')
    else:
        print('\033[0;32mNo listed file reaches unlisted memory, except the '
              '%d recorded gap%s.\033[0m' % (n, '' if n == 1 else 's'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
