"""Score the declaration object against the CSSOM pages of the pinned web-platform-tests.

    css_cssom_suite.py <vectors dir> <runner> <exceptions> <unscored> [--regenerate]

The vectors are made by tools/css/extract-wpt-cssom.py: ``declaration.tsv``, a program for each page whose script has tests the reader recognizes (the op interpreter of
tools/conformance/css_cssom_suite.c runs it on a document, with the elements the page's markup has), and ``UNEXTRACTED_CSSOM``, every page or test the reader does not read, with
its number of tests and the reason.

Every failure has to be listed, one line a test, in CSS_CSSOM_EXCEPTIONS: the test (``file#n``), the first assertion that failed, a class and the reason. The classes are
``served-text`` and ``wpt-wrong`` (the reason quotes a sentence of the served draft that decides, and says what the page expects instead), ``browsers-differ`` (the two
recorded browser answers differ and the library keeps the served text), ``wpt-tentative`` (the page is tentative and no browser record exists) and ``defect`` (a library
defect, with its owner: ``owner: <where it is fixed>``; the number of them is a ceiling, CSS_CSSOM_MAX_DEFECTS, that only falls). The gate fails on a failure that is not
listed ("UNEXPLAINED FAILURES"), on a listed test that passes, does not exist or fails another way ("STALE EXCEPTIONS"), on a reason without a quote where one is needed or
a class outside the five ("SUITE PROBLEMS"), when fewer tests were scored than the floor (CSS_CSSOM_MIN_SCORED) or fewer pages than CSS_CSSOM_MIN_PAGES ("below the floor"), or a smaller fraction of the tests of the touched pages than CSS_CSSOM_MIN_FRACTION (parts in ten thousand; it only rises),
on more defects than the ceiling ("defects above the ceiling"), and on a runner that does not answer.

The pages not read are in UNSCORED_CSSOM: the file, the number of its tests and the reason. A line that differs from what the reader finds fails ("UNSCORED VECTORS"), so a
page cannot leave the scored set unlisted and a listed page cannot stay when the reader reads it.
"""
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(errors='backslashreplace')

CLASSES = ('served-text', 'wpt-wrong', 'browsers-differ', 'wpt-tentative', 'defect')


def quotes_a_sentence(reason):
    return any(len(q) >= 20 and len(q.split()) >= 4 for q in re.findall(r'"([^"]*)"', reason))


def load_tsv(path):
    rows = []
    if os.path.exists(path):
        for n, line in enumerate(open(path, encoding='utf-8'), 1):
            line = line.rstrip('\n')
            if line and not line.startswith('#'):
                rows.append((n, line.split('\t')))
    return rows


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    regenerate = '--regenerate' in sys.argv
    vdir, runner, exc_path, unscored_path = args[:4]
    with open(os.path.join(vdir, 'declaration.tsv'), 'rb') as f:
        data = f.read()
    proc = subprocess.run([runner], input=data, stdout=subprocess.PIPE, check=False)
    if proc.returncode != 0:
        print('SUITE PROBLEMS: the runner exited %d' % proc.returncode)
        return 1
    results = {}
    errors = []
    for line in proc.stdout.decode('utf-8', 'surrogateescape').split('\n')[:-1]:
        f = line.split('\t')
        if f[0] == 'R':
            results['%s#%s' % (f[1], f[2])] = (f[3] + ('\t' + f[4] if len(f) > 4 else '')).encode('ascii', 'backslashreplace').decode('ascii')
        elif f[0] == 'E':
            errors.append(line)
    expected = 0
    for line in data.split(b'\n'):
        if line.startswith(b'P\t'):
            expected += int(line.split(b'\t')[3])
    out = []
    if errors:
        out.append('SUITE PROBLEMS: a program could not be run as the page ran it (%d):' % len(errors))
        out.extend('  ' + e for e in errors[:20])
    if len(results) != expected:
        out.append('SUITE PROBLEMS: %d results for %d tests' % (len(results), expected))
    failures = {k: v.split('\t', 1)[1] if '\t' in v else '' for k, v in results.items() if not v.startswith('PASS')}
    scored = len(results)
    pages = len({k.rsplit('#', 1)[0] for k in results})
    if regenerate:
        for k, v in sorted(failures.items()):
            print('%s\t%s\tTODO\tTODO' % (k, v))
        return 0
    exceptions = {}
    defects = 0
    for n, f in load_tsv(exc_path):
        if len(f) != 4:
            out.append('SUITE PROBLEMS: %s:%d has %d fields, not four' % (os.path.basename(exc_path), n, len(f)))
            continue
        vec, got, cls, reason = f
        if cls not in CLASSES:
            out.append('SUITE PROBLEMS: %s:%d: class %r is not one of %s' % (os.path.basename(exc_path), n, cls, ', '.join(CLASSES)))
        elif cls in ('served-text', 'wpt-wrong') and not quotes_a_sentence(reason):
            out.append('SUITE PROBLEMS: %s:%d: a %s reason quotes a sentence of the served draft' % (os.path.basename(exc_path), n, cls))
        elif cls == 'defect':
            defects += 1
            if not re.search(r'owner: \S', reason):
                out.append('SUITE PROBLEMS: %s:%d: a defect names its owner ("owner: ...")' % (os.path.basename(exc_path), n))
        exceptions[vec] = (got, n)
    unexplained = [k for k in failures if k not in exceptions]
    stale = []
    for k, (got, n) in exceptions.items():
        if k not in results:
            stale.append('%s:%d: %s does not exist' % (os.path.basename(exc_path), n, k))
        elif k not in failures:
            stale.append('%s:%d: %s passes' % (os.path.basename(exc_path), n, k))
        elif failures[k] != got:
            stale.append('%s:%d: %s fails with "%s", not "%s"' % (os.path.basename(exc_path), n, k, failures[k], got))
    if unexplained:
        out.append('UNEXPLAINED FAILURES (%d):' % len(unexplained))
        for k in sorted(unexplained)[:80]:
            out.append('  %s  %s' % (k, failures[k]))
    if stale:
        out.append('STALE EXCEPTIONS:')
        out.extend('  ' + s for s in stale)
    ceiling = os.environ.get('CSS_CSSOM_MAX_DEFECTS')
    if ceiling is not None and defects > int(ceiling):
        out.append('defects above the ceiling: %d listed, the ceiling is %s' % (defects, ceiling))
    # The pages not read.
    have = {}
    for line in open(os.path.join(vdir, 'UNEXTRACTED_CSSOM'), encoding='utf-8', errors='surrogateescape'):
        f = line.rstrip('\n').split('\t')
        have[(f[0], f[2])] = f[1]
    listed = {}
    for n, f in load_tsv(unscored_path):
        if len(f) != 3:
            out.append('SUITE PROBLEMS: %s:%d has %d fields, not three' % (os.path.basename(unscored_path), n, len(f)))
            continue
        listed[(f[0], f[2])] = f[1]
    if have != listed:
        out.append('UNSCORED VECTORS: the pages not read differ from %s' % os.path.basename(unscored_path))
        shown = 0
        for k in sorted(set(have) | set(listed)):
            if have.get(k) != listed.get(k) and shown < 40:
                out.append('  %s (%s): read %s, listed %s' % (k[0], k[1][:40], have.get(k), listed.get(k)))
                shown += 1
    floor = int(os.environ.get('CSS_CSSOM_MIN_SCORED', '0'))
    if scored < floor:
        out.append('below the floor: %d tests scored, the floor is %d' % (scored, floor))
    floor_pages = int(os.environ.get('CSS_CSSOM_MIN_PAGES', '0'))
    if pages < floor_pages:
        out.append('below the floor: %d pages scored, the floor is %d' % (pages, floor_pages))
    unread = sum(int(v) for v in have.values())
    # the tests the scoring interpreter scored (tools/conformance/css_cssom_script_suite.py wrote them) are scored tests of the touched pages too; the ones it read and that need a
    # layout are accounted for and not scored
    interp, layout = 0, 0
    note = os.path.join(vdir, 'SCRIPT_SCORED')
    if os.path.exists(note):
        interp, layout = (int(x) for x in open(note).read().split())
    scored_all = scored + interp
    fraction = 10000 * scored_all // (scored_all + layout + unread) if scored_all + layout + unread else 0
    floor_fraction = int(os.environ.get('CSS_CSSOM_MIN_FRACTION', '0'))
    if fraction < floor_fraction:
        out.append('below the floor: %d of %d tests of the touched pages are scored (%d in ten thousand), the floor is %d' % (scored_all, scored_all + layout + unread, fraction, floor_fraction))
    if out:
        print('\n'.join(out))
        return 1
    touched = len({k[0] for k in have}) + pages
    print('css-cssom: %d tests of %d pages scored here, %d pass, %d are exceptions (%d defects); %d more scored by the interpreter; %d (page, reason) pairs not read; scored fraction %d in ten thousand of %d tests' % (
        scored, pages, scored - len(failures), len(failures), defects, interp, len(have), fraction, scored_all + layout + unread))
    return 0


sys.exit(main())
