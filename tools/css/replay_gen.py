# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Text.
#
# Ghoti.io Text is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
#
# Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
# for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""The replay of a flat program on the real DOM of a browser: the independent reading of the evaluator.

    replay_gen.py PROGRAMS_TSV PAGE_ROOT OUT_ROOT [--pages FILE] [--plant PAGE=NAME ...]

The flat program of a page (``script.tsv``, tools/css/cssom_eval.py) is what the library's runner executes. The page itself is what a browser executes. This file is the third thing: it
reads the *program* (not the evaluator, not the page's script) and writes, for each page, the page's own markup with the text of every inline script replaced by the JavaScript that
performs the program's ops on the browser's DOM in the same place, under the page's own testharness.js. The replay is run in Chromium and Firefox (tools/oracle/css_page_record.py
--replay) and what each test of it reports has to be what the page reports for the same test: when it is not, the evaluator turned the page into a program that is not the page
(a loop unrolled one short, an assertion taken as another, a function inlined with the wrong scope) and no number the program yields can be trusted. Nothing here uses the library.

The tests of the replay are named ``<index>|<name>`` (the index of the test in the page, so the replay's tests are matched to the page's whatever their names); a test the evaluator did
not turn into ops has no replay. ``doc N`` ops (the runner parses the markup up to N) are not translated: the browser has the document the page's script saw, because the script is where it
was. ``--plant PAGE=NAME`` mistranslates one page on purpose (``drop-assert``, ``flip-expect``, ``wrong-id``, ``negate-cmp``) for the controls of the oracle.
"""
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

UNESC = {'t': '\t', 'n': '\n', 'r': '\r', '0': '\0', '\\': '\\'}


def unesc(s):
    return re.sub(r'\\(.)', lambda m: UNESC.get(m.group(1), m.group(1)), s, flags=re.S)


def split_programs(data):
    """{page: (sha-256 of the program's bytes, html, [op fields])} for the text of script.tsv."""
    out = {}
    cur = None
    for raw in data.split(b'\n'):
        if not raw:
            continue
        line = raw.decode('utf-8', 'surrogatepass')
        f = line.split('\t')
        if f[0] == 'P':
            cur = [unesc(f[1]), hashlib.sha256(), None, []]
            out[cur[0]] = cur
        if cur is None:
            continue
        cur[1].update(raw + b'\n')
        if f[0] == 'H':
            cur[2] = unesc(f[1])
        elif f[0] == 'O':
            cur[3].append([unesc(x) for x in f[1:]])
    return {k: (v[1].hexdigest(), v[2], v[3]) for k, v in out.items()}


def jstr(s):
    """A JavaScript string literal safe inside an HTML or XML script element."""
    return json.dumps(s, ensure_ascii=True).replace('<', '\\u003c').replace('>', '\\u003e').replace('&', '\\u0026')


def lit(f):
    kind, _, v = f.partition(':')
    if kind == 's':
        return jstr(v)
    if kind == 'n':
        return v
    if kind == 'b':
        return v
    if kind == 'N':
        return 'null'
    if kind == 'u':
        return 'undefined'
    if kind == 'r':
        return 'R.r[%s]' % jstr(v)
    if kind == 'e':
        return 'R.e[%s]' % jstr(v)
    raise ValueError('operand %r' % f)


def E(name):
    return 'R.e[%s]' % jstr(name)


def S(name):
    return 'R.s[%s]' % jstr(name)


def Rg(name):
    return 'R.r[%s]' % jstr(name)


def translate(op):
    """The JavaScript statement of one op (a list of fields, the op name first)."""
    n, a = op[0], op[1:]
    if n == 'elem':
        r, how = a[0], a[1]
        if how == 'byid':
            return '%s = document.getElementById(%s);' % (E(r), jstr(a[2]))
        if how in ('body', 'head'):
            return '%s = document.%s;' % (E(r), how)
        if how == 'html':
            return '%s = document.documentElement;' % E(r)
        root = 'document' if a[2] == 'doc' else E(a[2])
        if how == 'sel':
            return '%s = %s.querySelector(%s);' % (E(r), root, jstr(a[3]))
        if how == 'qsa':
            return '%s = %s.querySelectorAll(%s)[%s] || null;' % (E(r), root, jstr(a[3]), a[4] if len(a) > 4 else '0')
        if how == 'nav':
            return '%s = %s.%s;' % (E(r), E(a[2]), a[3])
    if n == 'gcs':
        # with no pseudo-element the call has one argument: both browsers answer a root's rlh differently when an explicit undefined is passed
        return '%s = getComputedStyle(%s%s);' % (S(a[0]), E(a[1]), '' if a[2] == '' else ', ' + jstr(a[2]))
    if n == 'style':
        return '%s = %s.style;' % (S(a[0]), E(a[1]))
    if n == 'sget':
        return '%s = %s.getPropertyValue(%s);' % (Rg(a[0]), S(a[1]), jstr(a[2]))
    if n == 'sattr':
        return '%s = %s[%s];' % (Rg(a[0]), S(a[1]), jstr(a[2]))
    if n == 'shas':
        return '%s = (%s in %s);' % (Rg(a[0]), jstr(a[2]), S(a[1]))
    if n == 'sprio':
        return '%s = %s.getPropertyPriority(%s);' % (Rg(a[0]), S(a[1]), jstr(a[2]))
    if n == 'sitem':
        return '%s = %s.item(%s);' % (Rg(a[0]), S(a[1]), a[2])
    if n == 'slen':
        return '%s = %s.length;' % (Rg(a[0]), S(a[1]))
    if n == 'scss':
        return '%s = %s.cssText;' % (Rg(a[0]), S(a[1]))
    if n == 'sset':
        return '%s.setProperty(%s, %s, %s);' % (S(a[0]), jstr(a[1]), jstr(a[2]), jstr(a[3]))
    if n == 'sattrset':
        return '%s[%s] = %s;' % (S(a[0]), jstr(a[1]), jstr(a[2]))
    if n == 'scsstext':
        return '%s.cssText = %s;' % (S(a[0]), jstr(a[1]))
    if n == 'sremove':
        return '%s = %s.removeProperty(%s);' % (Rg(a[0]), S(a[1]), jstr(a[2]))
    if n == 'setattr':
        return '%s.setAttribute(%s, %s);' % (E(a[0]), jstr(a[1]), jstr(a[2]))
    if n == 'getattr':
        return '%s = %s.getAttribute(%s);' % (Rg(a[0]), E(a[1]), jstr(a[2]))
    if n == 'hasattr':
        return '%s = %s.hasAttribute(%s);' % (Rg(a[0]), E(a[1]), jstr(a[2]))
    if n == 'rmattr':
        return '%s.removeAttribute(%s);' % (E(a[0]), jstr(a[1]))
    if n in ('classadd', 'classremove'):
        return '%s.classList.%s(%s);' % (E(a[0]), n[5:], ', '.join(jstr(x) for x in a[1:]))
    if n == 'classtoggle':
        return '%s = %s.classList.toggle(%s);' % (Rg(a[0]), E(a[1]), jstr(a[2]))
    if n == 'classhas':
        return '%s = %s.classList.contains(%s);' % (Rg(a[0]), E(a[1]), jstr(a[2]))
    if n == 'csssupports':
        return '%s = CSS.supports(%s);' % (Rg(a[0]), ', '.join(jstr(x) for x in a[1:]))
    if n == 'cssescape':
        return '%s = CSS.escape(%s);' % (Rg(a[0]), jstr(a[1]))
    if n == 'cmp':
        sym = {'eq': '===', 'ne': '!==', 'leq': '==', 'lne': '!='}[a[1]]
        return '%s = (%s %s %s);' % (Rg(a[0]), lit(a[2]), sym, lit(a[3]))
    if n == 'not':
        return '%s = !%s;' % (Rg(a[0]), lit(a[1]))
    if n == 'fail':
        return 'assert_unreached(%s);' % jstr(a[0] if a else 'failed')
    if n == 'assert_in_array':
        return 'assert_in_array(%s, [%s], %s);' % (lit(a[0]), ', '.join(lit(x) for x in a[1:-1]), jstr(a[-1]))
    if n.startswith('assert_'):
        return '%s(%s, %s);' % (n, ', '.join(lit(x) for x in a[:-1]), jstr(a[-1]))
    raise ValueError('the op %s has no replay' % n)


PLANTS = ('drop-assert', 'flip-expect', 'wrong-id', 'negate-cmp')


def plant_index(ops, plant):
    """The index in `ops` of the first op the plant concerns, or None."""
    for i, op in enumerate(ops):
        if plant == 'drop-assert' and op[0].startswith('assert_'):
            return i
        if plant == 'flip-expect' and op[0] == 'assert_equals' and op[2].startswith('s:'):
            return i
        if plant == 'wrong-id' and op[0] == 'elem' and op[2] == 'byid':
            return i
        if plant == 'negate-cmp' and op[0] == 'cmp':
            return i
    return None


def apply_plant(ops, plant):
    """The ops of a page with one mistranslation (for the controls): the first op the plant concerns."""
    at = plant_index(ops, plant)
    if at is None:
        return ops
    op = list(ops[at])
    if plant == 'drop-assert':
        return ops[:at] + ops[at + 1:]
    if plant == 'flip-expect':
        op[2] += 'X'
    elif plant == 'wrong-id':
        op[3] += 'X'
    elif plant == 'negate-cmp':
        op[2] = {'eq': 'ne', 'ne': 'eq', 'leq': 'lne', 'lne': 'leq'}[op[2]]
    return ops[:at] + [op] + ops[at + 1:]


def script_bodies(ops):
    """The ops of each script, in order: [(end offset in bytes of the script, [ops])]."""
    scripts = []
    for op in ops:
        if op[0] == 'script':
            scripts.append((int(op[1]), []))
        elif scripts:
            scripts[-1][1].append(op)
    return scripts


def translate_script(ops):
    """The JavaScript of one script's ops."""
    lines = ['var R = window.__R || (window.__R = {e: Object.create(null), s: Object.create(null), r: Object.create(null)});']
    cur = None
    for op in ops:
        if op[0] == 'doc':
            continue
        if op[0] == 'test':
            cur = {'idx': op[1], 'name': op[2] if len(op) > 2 else '', 'body': [], 'cleanup': [], 'in_cleanup': False}
            continue
        if op[0] == 'cleanup':
            cur['in_cleanup'] = True
            continue
        if op[0] == 'end':
            body = ['  ' + x for x in cur['body']]
            if cur['cleanup']:
                body.insert(0, '  t.add_cleanup(function () {\n' + '\n'.join('    ' + x for x in cur['cleanup']) + '\n  });')
            lines.append('test(function (t) {\n' + '\n'.join(body) + '\n}, %s);' % jstr('%s|%s' % (cur['idx'], cur['name'])))
            cur = None
            continue
        stmt = translate(op)
        if cur is None:
            lines.append(stmt)
        else:
            cur['cleanup' if cur['in_cleanup'] else 'body'].append(stmt)
    return '\n'.join(lines) + '\n'


def replay_page(html, ends_to_spans, ops, plant=None):
    """The page's markup with each inline script's text replaced by the replay of the ops of that script."""
    if plant:
        ops = apply_plant(ops, plant)
    pieces, last = [], 0
    for end, body in script_bodies(ops):
        if end not in ends_to_spans:
            raise ValueError('no inline script ends at byte %d' % end)
        a, b = ends_to_spans[end]
        pieces.append(html[last:a])
        pieces.append('\n' + translate_script(body))
        last = b
    pieces.append(html[last:])
    return ''.join(pieces)


def spans_by_end(html):
    """{end offset in bytes of a script's end tag: (start, end) in characters of its text} for the inline scripts of the page, by the same scan the evaluator uses."""
    import cssom_eval as CE
    scan = CE.PageScan()
    scan.text = html
    for m in re.finditer('\n', html):
        scan.line_starts.append(m.end())
    scan.feed(html)
    scan.close()
    out = {}
    for (attrs, _text, _mark), end, span in zip(scan.scripts, scan.script_ends, scan.script_spans):
        if not attrs.get('src'):
            out[len(html[:end].encode('utf-8', 'surrogatepass'))] = span
    return out


def mirror(page_root, out_root, rel):
    """Make the directories of `rel` real under out_root, each entry of them a link to the page root's but the next step of the path, and return the path the page is written at."""
    parts = rel.split('/')
    for d in range(len(parts)):
        src_dir = os.path.join(page_root, *parts[:d])
        dst_dir = os.path.join(out_root, *parts[:d])
        if os.path.islink(dst_dir):
            os.unlink(dst_dir)
        fresh = not os.path.isdir(dst_dir)
        os.makedirs(dst_dir, exist_ok=True)
        if fresh:
            for name in os.listdir(src_dir):
                if name != parts[d]:
                    os.symlink(os.path.join(src_dir, name), os.path.join(dst_dir, name))
    dst = os.path.join(out_root, *parts)
    if os.path.islink(dst):
        os.unlink(dst)
    return dst


def generate(programs_path, page_root, out_root, pages=None, plants=None):
    """Write the replay pages; returns {page: program sha}."""
    plants = plants or {}
    progs = split_programs(open(programs_path, 'rb').read())
    done = {}
    for page in sorted(pages if pages is not None else progs):
        sha, html, ops = progs[page]
        dst = mirror(page_root, out_root, page)
        text = replay_page(html, spans_by_end(html), ops, plants.get(page))
        with open(dst, 'w', encoding='utf-8', errors='surrogatepass', newline='') as f:
            f.write(text)
        done[page] = sha
    return done


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    plants = {}
    pages = None
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == '--plant':
            k, _, v = argv[i + 1].partition('=')
            plants[k] = v
        if a == '--pages':
            pages = [l.rstrip('\n') for l in open(argv[i + 1], encoding='utf-8') if l.strip()]
    plant_values = set(argv[i + 1] for i, a in enumerate(argv) if a in ('--plant', '--pages'))
    args = [a for a in args if a not in plant_values]
    done = generate(args[0], os.path.abspath(args[1]), os.path.abspath(args[2]), pages, plants)
    print('replay_gen: %d pages' % len(done))


if __name__ == '__main__':
    main()
