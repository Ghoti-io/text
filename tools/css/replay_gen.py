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
    if kind == 'o':
        return 'R.o[%s]' % jstr(v)
    raise ValueError('operand %r' % f)


def E(name):
    return 'R.e[%s]' % jstr(name)


def S(name):
    return 'R.s[%s]' % jstr(name)


def Rg(name):
    return 'R.r[%s]' % jstr(name)


def O(name):
    return 'R.o[%s]' % jstr(name)


def idx_arg(f):
    """An index argument of an op (`n:3`, or `u:` for none) as the argument of the call."""
    return '' if f.startswith('u:') else f[2:]


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
        if how == 'list':
            return '%s = R.l[%s][%s] || null;' % (E(r), jstr(a[2]), a[3])
        if how == 'qsa':
            return '%s = %s.querySelectorAll(%s)[%s] || null;' % (E(r), root, jstr(a[3]), a[4] if len(a) > 4 else '0')
        if how == 'nav':
            return '%s = %s.%s;' % (E(r), E(a[2]), a[3])
        if how == 'byidin':
            return '%s = %s.getElementById(%s);' % (E(r), E(a[2]), jstr(a[3]))
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
    if n == 'create':
        if a[2]:
            return '%s = document.createElementNS(%s, %s);' % (E(a[0]), jstr(a[2]), jstr(a[1]))
        return '%s = document.createElement(%s);' % (E(a[0]), jstr(a[1]))
    if n == 'ctext':
        return '%s = document.createTextNode(%s);' % (E(a[0]), jstr(a[1]))
    if n == 'ccomment':
        return '%s = document.createComment(%s);' % (E(a[0]), jstr(a[1]))
    if n == 'cfrag':
        return '%s = document.createDocumentFragment();' % E(a[0])
    if n == 'clone':
        return '%s = %s.cloneNode(%s);' % (E(a[0]), E(a[1]), a[2])
    if n in ('append', 'prepend', 'replacechildren', 'before', 'after', 'replacewith'):
        method = {'replacechildren': 'replaceChildren', 'replacewith': 'replaceWith'}.get(n, n)
        return '%s.%s(%s);' % (E(a[0]), method, ', '.join(lit(x) for x in a[1:]))
    if n == 'appendchild':
        return '%s.appendChild(%s);' % (E(a[0]), E(a[1]))
    if n == 'insertbefore':
        return '%s.insertBefore(%s, %s);' % (E(a[0]), E(a[1]), lit(a[2]))
    if n == 'removechild':
        return '%s.removeChild(%s);' % (E(a[0]), E(a[1]))
    if n == 'replacechild':
        return '%s.replaceChild(%s, %s);' % (E(a[0]), E(a[1]), E(a[2]))
    if n == 'remove':
        return '%s.remove();' % E(a[0])
    if n == 'tcget':
        return '%s = %s.textContent;' % (Rg(a[0]), E(a[1]))
    if n == 'tcset':
        return '%s.textContent = %s;' % (E(a[0]), jstr(a[1]))
    if n == 'dset':
        return '%s.data = %s;' % (E(a[0]), jstr(a[1]))
    if n == 'ihget':
        return '%s = %s.innerHTML;' % (Rg(a[0]), E(a[1]))
    if n == 'ohget':
        return '%s = %s.outerHTML;' % (Rg(a[0]), E(a[1]))
    if n == 'ihset':
        return '%s.innerHTML = %s;' % (E(a[0]), jstr(a[1]))
    if n == 'ihsetu':
        return '%s.setHTMLUnsafe(%s);' % (E(a[0]), jstr(a[1]))
    if n == 'attach':
        return '%s = %s.attachShadow({mode: %s});' % (E(a[0]), E(a[1]), jstr(a[2]))
    if n == 'rnsheets':
        return '%s = %s.styleSheets.length;' % (Rg(a[0]), E(a[1]))
    if n == 'rsheetat':
        return '%s = %s.styleSheets[%s];' % (O(a[0]), E(a[1]), a[2])
    if n == 'radoptset':
        return '%s.adoptedStyleSheets = [%s];' % (E(a[0]), ', '.join(O(x) for x in a[1:]))
    if n == 'radoptpush':
        return '%s.adoptedStyleSheets.push(%s);' % (E(a[0]), O(a[1]))
    if n == 'radoptn':
        return '%s = %s.adoptedStyleSheets.length;' % (Rg(a[0]), E(a[1]))
    if n == 'radoptat':
        return '%s = %s.adoptedStyleSheets[%s];' % (O(a[0]), E(a[1]), a[2])
    if n == 'nread':
        if a[2] == 'hasChildNodes':
            return '%s = %s.hasChildNodes();' % (Rg(a[0]), E(a[1]))
        return '%s = %s.%s;' % (Rg(a[0]), E(a[1]), a[2])
    if n == 'contains':
        return '%s = %s.contains(%s);' % (Rg(a[0]), E(a[1]), lit(a[2]))
    if n == 'matches':
        return '%s = %s.matches(%s);' % (Rg(a[0]), E(a[1]), jstr(a[2]))
    if n == 'closest':
        return '%s = %s.closest(%s);' % (E(a[0]), E(a[1]), jstr(a[2]))
    if n == 'child':
        return '%s = %s.%s[%s] || null;' % (E(a[0]), E(a[1]), 'children' if a[2] == 'elements' else 'childNodes', a[3])
    if n == 'nchildren':
        return '%s = %s.%s.length;' % (Rg(a[0]), E(a[1]), 'children' if a[2] == 'elements' else 'childNodes')
    if n == 'qsa':
        return 'R.l[%s] = %s.querySelectorAll(%s);' % (jstr(a[0]), 'document' if a[1] == 'doc' else E(a[1]), jstr(a[2]))
    if n == 'llen':
        return '%s = R.l[%s].length;' % (Rg(a[0]), jstr(a[1]))
    if n == 'qlen':
        return '%s = %s.querySelectorAll(%s).length;' % (Rg(a[0]), 'document' if a[1] == 'doc' else E(a[1]), jstr(a[2]))
    if n == 'iftrue':
        return 'if (%s) {' % lit(a[0])
    if n == 'iffalse':
        return 'if (!%s) {' % lit(a[0])
    if n == 'else':
        return '} else {'
    if n == 'endif':
        return '}'
    if n == 'sheetof':
        return '%s = %s.sheet;' % (O(a[0]), E(a[1]))
    if n == 'nsheets':
        return '%s = document.styleSheets.length;' % Rg(a[0])
    if n == 'sheetat':
        return '%s = document.styleSheets%s;' % (O(a[0]), '[%s]' % a[1] if a[2] == 'undefined' else '.item(%s)' % a[1])
    if n == 'newsheet':
        fields = []
        if a[1].startswith('s:'):
            fields.append('media: %s' % jstr(a[1][2:]))
        if a[2] == 'b:true':
            fields.append('disabled: true')
        return '%s = new CSSStyleSheet(%s);' % (O(a[0]), '{%s}' % ', '.join(fields) if fields else '')
    if n == 'replacesync':
        return '%s.replaceSync(%s);' % (O(a[0]), jstr(a[1]))
    if n == 'rlen':
        return '%s = %s.cssRules.length;' % (Rg(a[0]), O(a[1]))
    if n == 'ruleat':
        return '%s = %s.cssRules%s;' % (O(a[0]), O(a[1]), '[%s]' % a[2] if a[3] == 'undefined' else '.item(%s)' % a[2])
    if n == 'klen':
        return '%s = %s.length;' % (Rg(a[0]), O(a[1]))
    if n == 'kat':
        return '%s = %s[%s];' % (O(a[0]), O(a[1]), a[2])
    if n == 'rparent':
        return '%s = %s.parentRule;' % (O(a[0]), O(a[1]))
    if n == 'rsheet':
        return '%s = %s.parentStyleSheet;' % (O(a[0]), O(a[1]))
    if n == 'rstyle':
        return '%s = %s.style;' % (S(a[0]), O(a[1]))
    if n == 'omedia':
        return '%s = %s.media;' % (O(a[0]), O(a[1]))
    if n == 'kfind':
        return '%s = %s.findRule(%s);' % (O(a[0]), O(a[1]), jstr(a[2]))
    if n == 'ownernode':
        return '%s = %s.ownerNode;' % (E(a[0]), O(a[1]))
    if n == 'ownerrule':
        return '%s = %s.ownerRule;' % (O(a[0]), O(a[1]))
    if n == 'rattr':
        return '%s = %s[%s];' % (Rg(a[0]), O(a[1]), jstr(a[2]))
    if n == 'rtype':
        return '%s = %s.type;' % (Rg(a[0]), O(a[1]))
    if n == 'shattr':
        return '%s = %s[%s];' % (Rg(a[0]), O(a[1]), jstr(a[2]))
    if n == 'insertrule':
        return '%s = %s.insertRule(%s);' % (Rg(a[0]), O(a[1]), ', '.join([jstr(a[2])] + ([idx_arg(a[3])] if idx_arg(a[3]) else [])))
    if n in ('deleterule', 'removerule'):
        arg = jstr(a[1][2:]) if a[1].startswith('s:') else a[1][2:]
        return '%s.%s(%s);' % (O(a[0]), 'deleteRule' if n == 'deleterule' else 'removeRule', arg)
    if n == 'addrule':
        return '%s = %s.addRule(%s);' % (Rg(a[0]), O(a[1]), ', '.join([jstr(a[2]), jstr(a[3])] + ([idx_arg(a[4])] if idx_arg(a[4]) else [])))
    if n == 'appendrule':
        return '%s.appendRule(%s);' % (O(a[0]), jstr(a[1]))
    if n == 'rset':
        return '%s[%s] = %s;' % (O(a[0]), jstr(a[1]), jstr(a[2]))
    if n == 'sdisabled':
        return '%s.disabled = %s;' % (O(a[0]), a[1])
    if n == 'mtext':
        return '%s = %s.mediaText;' % (Rg(a[0]), O(a[1]))
    if n == 'mlen':
        return '%s = %s.length;' % (Rg(a[0]), O(a[1]))
    if n == 'mitem':
        return '%s = %s%s;' % (Rg(a[0]), O(a[1]), '[%s]' % a[2] if a[3] == 'undefined' else '.item(%s)' % a[2])
    if n == 'mset':
        return '%s.mediaText = %s;' % (O(a[0]), jstr(a[1]))
    if n in ('mappend', 'mdelete'):
        return '%s.%s(%s);' % (O(a[0]), 'appendMedium' if n == 'mappend' else 'deleteMedium', jstr(a[1]))
    if n == 'rinst':
        return '%s = (%s instanceof %s);' % (Rg(a[0]), O(a[1]), a[2])
    if n == 'adoptset':
        return 'document.adoptedStyleSheets = [%s];' % ', '.join(O(x) for x in a)
    if n == 'adoptpush':
        return 'document.adoptedStyleSheets.push(%s);' % O(a[0])
    if n == 'adoptn':
        return '%s = document.adoptedStyleSheets.length;' % Rg(a[0])
    if n == 'adoptat':
        return '%s = document.adoptedStyleSheets[%s];' % (O(a[0]), a[1])
    if n == 'mql':
        return '%s = window.matchMedia(%s);' % (O(a[0]), jstr(a[1]))
    if n == 'mqread':
        return '%s = %s.%s;' % (Rg(a[0]), O(a[1]), a[2])
    if n == 'mqcss':
        # resources/matchmedia-utils.js query_is_css_parseable(), as the helper writes it (the runner does it on a constructed sheet)
        return ('%s = (function (query) { const style = document.createElement("style"); style.type = "text/css"; document.head.appendChild(style); const sheet = style.sheet; '
                'try { sheet.insertRule("@media " + query + "{}", 0); return sheet.cssRules.length == 1 && sheet.cssRules[0].media.mediaText != "not all"; } '
                'finally { while (sheet.cssRules.length) sheet.deleteRule(0); style.remove(); } })(%s);') % (Rg(a[0]), jstr(a[1]))
    if n == 'try':
        return 'try {'
    if n == 'endtry':
        return '} catch (e) {}'
    if n == 'throws':
        return 'assert_throws_%s(%s, function () {' % (a[0], jstr(a[1]) if a[0] == 'dom' else a[1])
    if n == 'endthrows':
        return '});'
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
    lines = ['var R = window.__R || (window.__R = {e: Object.create(null), s: Object.create(null), r: Object.create(null), l: Object.create(null), o: Object.create(null)});']
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
            if all(op[0] == 'doc' for op in body):
                continue   # a modeled helper script (src): it runs as it is, and defines functions that no op calls
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
