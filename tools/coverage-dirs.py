#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Line and branch coverage per source directory, and the uncovered lines, from a --coverage build.

    tools/coverage-dirs.py OBJDIR [--dirs src/xml src/markup ...] [--list]

Run from the project root after the instrumented tests ran (the .gcda files are under OBJDIR).  Runs gcov
with branch counts (gcov -b -c -p), merges the instantiations of a line that more than one object reaches
(a line counts as executed if any reached it; a branch as taken if any took it), and prints per directory:
lines (executable, executed, percent), branches (total, taken at least once, percent).  --list prints, for
each file, the executable lines never executed and the lines with a branch never taken (with the line text),
which is what the coverage review of Phase 3k classifies into covered, unreachable-with-a-reason, or
defensive.  tools/coverage.sh is the line-only report `make coverage` prints.
"""
import collections, glob, os, re, subprocess, sys

def collect(obj, dirs):
    """Returns (lines, branches): file -> {line: [hit, text]}, file -> {(line, idx): taken}."""
    root = os.getcwd()
    gcdas = [os.path.abspath(p) for p in glob.glob(os.path.join(obj, '**', '*.gcda'), recursive=True)]
    if not gcdas:
        sys.exit('no .gcda files under ' + obj)
    before = set(glob.glob(os.path.join(root, '*.gcov')))
    for g in gcdas:
        subprocess.run(['gcov', '-b', '-c', '-p', '-r', '-o', os.path.dirname(g), g], cwd=root,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    lines = collections.defaultdict(dict)
    branches = collections.defaultdict(dict)
    made = [p for p in glob.glob(os.path.join(root, '*.gcov')) if p not in before]
    for gc in made:
        src = None
        cur = None
        for raw in open(gc, errors='replace'):
            raw = raw.rstrip('\n')
            m = re.match(r'\s*-:\s*0:Source:(.*)', raw)
            if m:
                src = m.group(1)
                if not any(src.startswith(d + '/') for d in dirs):
                    src = None
                    break
                continue
            if src is None:
                continue
            m = re.match(r'\s*([^:]+):\s*(\d+):(.*)', raw)
            if m:
                cnt, ln, text = m.group(1).strip(), int(m.group(2)), m.group(3)
                if ln == 0 or cnt == '-':
                    cur = None
                    continue
                cur = ln
                hit = cnt not in ('#####', '=====')
                ent = lines[src].setdefault(ln, [False, text])
                ent[0] = ent[0] or hit
                continue
            m = re.match(r'branch\s+(\d+)\s+(taken (\d+)|never executed)', raw)
            if m and cur is not None:
                taken = m.group(3) is not None and int(m.group(3)) > 0
                key = (cur, int(m.group(1)))
                branches[src][key] = branches[src].get(key, False) or taken
    for p in made:
        os.unlink(p)
    return lines, branches


CLASSES = [
    ('size-overflow', 'A guard against a size or capacity that would overflow size_t. Reaching it takes an allocation request of 2^63 bytes or more '
                      '(or a count whose product with the element size overflows), which no input can produce under the library\'s limits and no test can '
                      'ask an allocator for; the arithmetic is the proof, and a 32-bit run reaches the narrower ones.'),
    ('oom', 'The failure arm of an allocation, or of a call that fails only by allocation failure, at a site the n-th-allocation sweeps (testHtmlApiOom, '
            'testEncodingOom, the fuzzers\' failing allocator) do not reach because the site\'s allocation is covered by an earlier, larger one or '
            'the sweep ends before it.'),
    ('propagate', 'An error status returned unchanged to the caller after a callee failed (`if (call != OK) return status;`), where the callee can '
                  'fail only by allocation failure or a refusing sink, the two things the sweeps inject; the arm is one line and the callee\'s own failure '
                  'is tested where the callee is.'),
    ('internal', 'The report of a state the code proves impossible (a defect of this library, E_INTERNAL or an invalid builder argument that every '
                 'public entry point has already checked). Defensive and kept so that a defect is a status and not undefined behavior.'),
    ('enum-default', 'The `default:` arm of a switch over an enum, or the return after an exhaustive switch: it can be reached only by a C caller '
                     'that passes a value outside the enum, which the public entry points reject before they get here.'),
    ('dead-token', 'The arm for a token the producer never makes (the HTML tokenizer has no processing-instruction token: `<?` is a bogus comment). '
                   'Kept so that the consumer is total over its input type.'),
    ('compound', 'A branch of a compound condition: one operand of `&&`/`||`/`?:` (or a `switch` with a case) that never decided the result '
                 'in any test, while the line itself ran. These are the sub-conditions of a decision made on a short-circuit; the other '
                 'operand\'s outcome is covered, and the decision they feed is covered on both sides by the line above or below.'),
    ('loop', 'The exit (or the zero-iteration entry) of a `for`/`while` loop that the tests never took the other way: a loop whose count is '
             'bounded by an input the tests always make non-empty (or never make empty).'),
    ('narrowed', 'Code the call sites make unreachable: a condition every caller has already ruled out (the ASCII byte handled by the decoder loop before '
                 'the codec step, a tie in a sort of names that are unique in a tree, a comparison whose second operand is a lower-case literal, the '
                 'first code point of a pair that no index makes U+FEFF, an encoding of the enum that has no codec, when all of them have). Kept so the '
                 'function is total and correct on its own.'),
    ('xarch-32bit', 'Reachable only with a 32-bit size_t (the offset map\'s 27 bits). Run by suite/tools/xarch/text.sh i686 (HtmlSourceMapLimit), not by a 64-bit make test.'),
    ('fault-injection', 'The status of a file operation that fails in the middle (a write error, an unclassified error): needs a file system that fails, which the '
                        'tests of the file layer inject in cutil, where the layer lives; the other statuses of the same switch are tested here.'),
    ('reachable-untested', 'Reachable, and not reached by the tests that make test runs. Not claimed unreachable: reached, or not, by the differentials, the fuzzers '
                           'and the conformance gates (their runs add to the coverage of the first three: see the plan), listed so that a change to the code '
                           'beside it adds the test.'),
    ('other', 'Not yet classified: the review reads these one by one.'),
]


OVERRIDES = [
    (r'encoding_cjk\.c', r'return -1;', 'internal'),
    (r'encoding_decode\.c', r'out->cp\[0\] = \(uint32_t\)byte;|out->n = 1;|return GTEXT_DEC_EMIT;|strip_pending = false;|cp0 == 0xFEFF|return gtext_out_put_cp\(&dec->out, cp1\);|^break;$|E_UNSUPPORTED', 'narrowed'),
    (r'encoding_encode\.c', r'E_UNSUPPORTED', 'narrowed'),
    (r'html_api\.c', r'GTEXT_HTML_TREE_OK|return GTEXT_HTML_OK;|return tree_verdict', 'narrowed'),
    (r'html_api\.c', r'LIMIT_INPUT', 'xarch-32bit'),
    (r'(html|xml)_file_io\.c', r'FILE_E_WRITE|Failed to write|File operation failed|E_WRITE;|E_INVALID;', 'fault-injection'),
    (r'html_serialize\.c', r'y = \(char\)\(y \+ 32\)|a->index < b->index|return GTEXT_HTML_E_WRITE;', 'narrowed'),
    (r'xml_writer\.c', r'case GTEXT_XML_OUTPUT_UTF8|cp < 0x|b\[[0-3]\] =|n = [1-4];|return out_raw\(w, b, n\)|0xC0|0xE0|0xF0', 'narrowed'),
    (r'xml_encoding\.c', r'XML_FAMILY_LATIN1|XML_FAMILY_ASCII|return "(ISO-8859-1|US-ASCII)"', 'narrowed'),
    (r'html_tokenizer\.c', r'want \+= 0x20', 'narrowed'),
    (r'markup_tree\.c', r'prefix->refs\+\+|d->prefix = s->prefix|ns->refs\+\+', 'narrowed'),
]


FALLBACK = [
    (r'xml_stream\.c|xml_parser\.c|xml_lexer\.c|xml_doctype\.c|xml_writer\.c|markup_walk\.c|markup_support\.c|markup_tree\.c|html_tokenizer\.c|html_tree|html_sniff\.c|html_serialize\.c|html_api\.c', r'.*', 'reachable-untested'),
]


def override(path, text, table=None):
    t = text.strip()
    for fre, tre, cls in (table or OVERRIDES):
        if re.search(fre, path) and re.search(tre, t):
            return cls
    return None


def classify(kind, text, above, path=''):
    o = override(path, text)
    if o and kind == 'L':
        return o
    t = text.strip()
    ctx = ' '.join(above)
    if re.search(r'SIZE_MAX|UINT32_MAX|overflow', t + ' ' + ctx) and not re.search(r'status|message', t) or 'SIZE_MAX' in t:
        return 'size-overflow'
    if re.search(r'TK_PI|insert_pi|Q_PI', t) or (re.search(r'case TK_PI', ctx) and kind == 'L'):
        return 'dead-token'
    if re.search(r'E_INTERNAL|INTERNAL|not reachable|not reached|defect|impossible|never', t + ' ' + ctx, re.I):
        return 'internal'
    if t.startswith('default:') or re.match(r'case GTEXT_\w*(NONE|COUNT)\b', t) or re.search(r'^\s*default:', ctx.split('||')[-1] if ctx else ''):
        return 'enum-default'
    if re.search(r'OOM|oom\(|out of memory|->oom|\balloc', t) or re.search(r'OOM|\balloc|oom\(', ctx):
        return 'oom'
    if re.match(r'(return\s+(\w+(->|\.))*(status|st|false|NULL|true)\s*;|return\s+\w+_(status|fail)\(.*\);|return\s+fail\(|return\s+xml_fail\()', t) \
            or (kind == 'B' and re.search(r'!=\s*GTEXT_\w*OK|==\s*GTEXT_\w*OK|\bHT_CK\(|if \(!\w+\)|if \((\w+(->|\.))*\w*err\w*\)|if \((out|result|out_len)\)', t)):
        return 'propagate'
    if kind == 'B':
        if re.search(r'&&|\|\||\?', t) or re.match(r'(&&|\|\|)', t) or t.endswith(('&&', '||')):
            return 'compound'
        if t.startswith('switch (') or t.startswith('case ') or re.match(r'\}?\s*while \(', t) or t.startswith(('for (', 'while (')) or t.startswith('} while'):
            return 'compound' if t.startswith(('switch', 'case')) else 'loop'
        return 'reachable-untested'
    return override(path, text, FALLBACK) or 'other'


def main():
    args = sys.argv[1:]
    obj = args[0]
    dirs = ['src/xml', 'src/markup', 'src/html', 'src/encoding']
    listing = '--list' in args
    classify_mode = '--classify' in args
    if '--dirs' in args:
        i = args.index('--dirs')
        dirs = [a for a in args[i + 1:] if not a.startswith('--')]
    root = os.getcwd()
    lines, branches = collect(obj, dirs)
    totals = collections.OrderedDict()
    for d in dirs:
        L = E = B = T = 0
        for f in lines:
            if f.startswith(d + '/'):
                L += len(lines[f]); E += sum(1 for v in lines[f].values() if v[0])
                B += len(branches[f]); T += sum(1 for v in branches[f].values() if v)
        totals[d] = (L, E, B, T)
    print('%-14s %8s %8s %7s   %8s %8s %7s' % ('directory', 'lines', 'hit', '%', 'branches', 'taken', '%'))
    gl = ge = gb = gt = 0
    for d, (L, E, B, T) in totals.items():
        print('%-14s %8d %8d %6.1f%%   %8d %8d %6.1f%%' % (d, L, E, 100.0 * E / max(L, 1), B, T, 100.0 * T / max(B, 1)))
        gl += L; ge += E; gb += B; gt += T
    print('%-14s %8d %8d %6.1f%%   %8d %8d %6.1f%%' % ('all', gl, ge, 100.0 * ge / max(gl, 1), gb, gt, 100.0 * gt / max(gb, 1)))
    print()
    print('%-44s %6s %6s %6s %6s' % ('file', 'lines', 'miss', 'brs', 'miss'))
    for f in sorted(lines):
        L = len(lines[f]); M = sum(1 for v in lines[f].values() if not v[0])
        B = len(branches[f]); X = sum(1 for v in branches[f].values() if not v)
        print('%-44s %6d %6d %6d %6d' % (f, L, M, B, X))

    def source(f):
        try:
            return open(os.path.join(root, f), errors='replace').read().split('\n')
        except OSError:
            return []

    gaps = []   # (file, kind, line, text, class)
    for f in sorted(lines):
        src = source(f)
        miss = sorted(l for l, v in lines[f].items() if not v[0])
        bmiss = sorted({l for (l, i), t in branches[f].items() if not t} - set(miss))
        for kind, group in (('L', miss), ('B', bmiss)):
            for l in group:
                above = [x.strip() for x in src[max(0, l - 4):l - 1]]
                text = src[l - 1] if l - 1 < len(src) else lines[f].get(l, [False, ''])[1]
                gaps.append((f, kind, l, text, classify(kind, text, above, f)))
    if listing:
        print()
        cur = None
        for f, kind, l, text, cls in gaps:
            if f != cur:
                print('== %s' % f)
                cur = f
            print('  %s%-6d [%s] %s' % (kind, l, cls, text.strip()[:140]))
    if classify_mode:
        print()
        print('%-14s %-14s %7s %7s' % ('directory', 'class', 'lines', 'branch-only'))
        for d in dirs:
            for name, _ in CLASSES:
                nl = sum(1 for g in gaps if g[0].startswith(d + '/') and g[4] == name and g[1] == 'L')
                nb = sum(1 for g in gaps if g[0].startswith(d + '/') and g[4] == name and g[1] == 'B')
                if nl or nb:
                    print('%-14s %-14s %7d %7d' % (d, name, nl, nb))
        print()
        for name, why in CLASSES:
            print('%-14s %s' % (name, why))
        others = [g for g in gaps if g[4] == 'other']
        if others:
            print()
            print('unclassified (%d):' % len(others))
            for f, kind, l, text, cls in others:
                print('  %s:%d %s %s' % (f, l, kind, text.strip()[:130]))


main()
