#!/usr/bin/env python3
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
"""The evaluator's own tests: tools/css/eval-corpus/cases.txt run through tools/css/cssom_eval.py, and the evaluator seen to fail.

    check-cssom-eval.py              run the corpus
    check-cssom-eval.py --controls   run the corpus on copies of the evaluator with a defect planted in each (every one has to fail a case)
    check-cssom-eval.py --node       also run the pages of the corpus in node, where their assertions are checked by a real JavaScript engine

The corpus is not made of pages of the pin: each case is a page written for the evaluator, with a hand-checked outcome (and, where the outcome is an op, `has <fields of the op joined by |>`), so that an evaluator that unrolls a loop one iteration short,
adds a number to a string wrongly or reads a closure's variable from the wrong scope fails here, and not by passing a page of the suite it has misread.
"""
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, 'eval-corpus', 'cases.txt')

# name, file, text (exactly once), replacement: the planted defects of the evaluator
PLANTS = [
    ('loop-unrolled-one-short', 'cssom_eval.py',
     "            if test is not None and not truthy(self.eval(test, loop)):\n                break\n            # a fresh binding",
     "            if test is not None and not truthy(self.eval(test, loop)):\n                break\n            if test is not None and test[0] == 'bin' and test[1] == '<' and truthy(self.eval(('bin', '>=', test[2], ('bin', '-', test[3], ('num', 1))), loop)):\n                break\n            # a fresh binding"),
    ('assert-not-equals-taken-as-equals', 'cssom_eval.py',
     "if strict_eq(a[0], a[1]) or (isinstance(a[0], float) and isinstance(a[1], float) and a[0] != a[0] and a[1] != a[1]):",
     "if not strict_eq(a[0], a[1]) or (isinstance(a[0], float) and isinstance(a[1], float) and a[0] != a[0] and a[1] != a[1]):"),
    ('second-assertion-taken-as-the-first', 'cssom_eval.py',
     "        if not same_value(a[0], a[1]):\n            raise AssertFail('assert_equals:",
     "        self.cur.nasserts = getattr(self.cur, 'nasserts', 0) + 1\n        if not same_value(a[0], a[1]) and self.cur.nasserts == 1:\n            raise AssertFail('assert_equals:"),
    ('function-inlined-with-the-callers-scope', 'cssom_eval.py',
     [("        scope = Scope(fn.scope, True)\n        if not node[3]:", "        scope = Scope(getattr(self, 'dyn', None) or fn.scope, True)\n        if not node[3]:"),
      ("            args = self.eval_args(e[2], scope)\n            if isinstance(fn, BoundMethod):", "            args = self.eval_args(e[2], scope)\n            self.dyn = scope\n            if isinstance(fn, BoundMethod):"),
      ("        args = self.eval_args(e[2], scope)\n        return self.call(fn, UNDEF, args)", "        args = self.eval_args(e[2], scope)\n        self.dyn = scope\n        return self.call(fn, UNDEF, args)")]),
    ('negative-zero-printed-as-minus-zero', 'cssom_eval.py',
     "    if x == 0:\n        return '0'\n    if x == int(x) and abs(x) < 9007199254740992:",
     "    if x == 0:\n        return '-0' if math.copysign(1, x) < 0 else '0'\n    if x == int(x) and abs(x) < 9007199254740992:"),
    ('closure-shares-the-loop-variable', 'cssom_eval.py',
     "            it = Scope(loop)\n            for n in names:",
     "            it = loop\n            for n in names:"),
    ('plus-of-a-number-and-a-string', 'cssom_eval.py',
     "            if isinstance(a, str) or isinstance(b, str):\n                r = to_str(a) + to_str(b)",
     "            if isinstance(a, str) and isinstance(b, str):\n                r = to_str(a) + to_str(b)"),
    ('unicode-brace-escape-truncated', 'extract-wpt-cssom.py',
     "            out.append(chr(int(body[i + 2:j], 16)))",
     "            out.append(chr(int(body[i + 2:j][:4], 16)))"),
    ('unmodeled-test-does-not-poison', 'cssom_eval.py',
     "            if func_may_change_world(args[0].node, self.funcs):",
     "            if False:"),
    ('step-limit-off', 'cssom_eval.py', "STEP_LIMIT = 400000", "STEP_LIMIT = 10 ** 15"),
    ('default-name-counter-not-advanced', 'cssom_eval.py',
     "            self.default_names += 1\n",
     "            self.default_names += 0\n"),
    ('shared-state-test-scored', 'cssom_eval.py',
     "                rec.reason = 'shared-state'\n        except Unmodeled as u:",
     "                rec.reason = None\n        except Unmodeled as u:"),
    ('prefix-offset-in-characters', 'cssom_eval.py',
     "it.enter_script(len(text[:end].encode('utf-8', 'surrogatepass')), [i for",
     "it.enter_script(end, [i for"),
    ('prefix-offset-one-short', 'cssom_eval.py',
     "            self.ops.append(('doc', str(end)))",
     "            self.ops.append(('doc', str(end - 1)))"),
    ('mutation-does-not-freeze-the-document', 'cssom_eval.py',
     "        if fields[0] in MUTATING_OPS:\n            self.mutated = True",
     "        if False:\n            self.mutated = True"),
    ('null-written-as-the-word', 'cssom_eval.py',
     "    return '' if v is None else to_str(v)",
     "    return to_str(v)"),
    ('style-assignment-not-forwarded', 'cssom_eval.py',
     "    if key == 'style':\n        # [PutForwards=cssText]",
     "    if key == 'style-':\n        # [PutForwards=cssText]"),
    ('element-identity-by-lookup', 'cssom_eval.py',
     "(isinstance(a, (HostElem, HostObj)) or isinstance(b, (HostElem, HostObj))) and not isinstance(a, Rt) and not isinstance(b, Rt) and a is not b:\n            # whether",
     "(isinstance(a, (HostElem, HostObj)) or isinstance(b, (HostElem, HostObj))) and not isinstance(a, Rt) and not isinstance(b, Rt) and False:\n            # whether"),
    ('supports-drops-the-value', 'cssom_eval.py',
     "            args = [to_str(x) for x in a[:2]]",
     "            args = [to_str(x) for x in a[:1]]"),
    ('minus-zero-operand-lost', 'cssom_eval.py',
     "        return 'n:' + ('-0' if v == 0 and math.copysign(1.0, v) < 0 else js_num_str(v))",
     "        return 'n:' + js_num_str(v)"),
    ('insert-before-reference-and-child-swapped', 'cssom_eval.py',
     "        i.emit('insertbefore', el.name, child.name, ref)",
     "        i.emit('insertbefore', el.name, ref, child.name)"),
    ('replace-child-arguments-swapped', 'cssom_eval.py',
     "                fields = [need_node(x, op).name for x in a[:nargs]]",
     "                fields = [need_node(x, op).name for x in reversed(a[:nargs])]"),
    ('append-converts-a-number-as-python-does', 'cssom_eval.py',
     "        return 's:' + to_str(v)\n    raise Unmodeled('host-node-argument')",
     "        return 's:' + str(v)\n    raise Unmodeled('host-node-argument')"),
    ('a-node-that-may-not-exist-taken-as-there', 'cssom_eval.py',
     "    if isinstance(v, (HostElem, HostObj)) and v.nullable:\n        raise Unmodeled('host-node-may-be-null')\n",
     ""),
    ('guard-loses-its-else', 'cssom_eval.py',
     "            self.emit('else')\n",
     "            pass\n"),
    ('guard-lets-a-page-variable-be-assigned', 'cssom_eval.py',
     "                if any(n not in declared for n in pattern_names(t)):\n                    return False",
     "                pass"),
    ('node-list-is-not-a-snapshot', 'cssom_eval.py',
     "    n = interp.new_node('l')\n    interp.emit('qsa', n, root, a[0])\n    return HostList(n, None, 'static')",
     "    return HostList(root, a[0])"),
    ('rule-list-item-read-as-an-index', 'cssom_eval.py',
     "                return at(i, int(a[0]), 'null')\n            return BoundMethod(item)\n        return at(interp, int(key), 'undefined')\n    raise Unmodeled('dom-cssRules-' + key)",
     "                return at(i, int(a[0]), 'undefined')\n            return BoundMethod(item)\n        return at(interp, int(key), 'undefined')\n    raise Unmodeled('dom-cssRules-' + key)"),
    ('insert-rule-index-dropped', 'cssom_eval.py',
     "text_arg(a, 0, 'insertRule'), index_field(a[1] if len(a) > 1 else UNDEF, 'insertRule'))",
     "text_arg(a, 0, 'insertRule'), index_field(UNDEF, 'insertRule'))"),
    ('assert-throws-body-run-outside-the-block', 'cssom_eval.py',
     "        self.emit('throws', kind, name)\n        self.call(fn, UNDEF, [])\n        self.emit('endthrows')",
     "        self.call(fn, UNDEF, [])\n        self.emit('throws', kind, name)\n        self.emit('endthrows')"),
    ('constructed-sheet-options-dropped', 'cssom_eval.py',
     "            self.emit('newsheet', n, media, disabled)",
     "            self.emit('newsheet', n, 'u:', 'b:false')"),
    ('adopted-sheets-assignment-ignored', 'cssom_eval.py',
     "        interp.emit('adoptset', *[x.name for x in value])\n        return",
     "        return"),
    ('instanceof-reads-the-wrong-class', 'cssom_eval.py',
     "                self.emit('rinst', r, a.name, b.name)",
     "                self.emit('rinst', r, a.name, 'CSSRule')"),
    ('sheet-that-may-not-exist-taken-as-there', 'cssom_eval.py',
     "    if isinstance(v, (HostElem, HostObj)) and v.nullable:\n        raise Unmodeled('host-node-may-be-null')",
     "    if isinstance(v, HostElem) and v.nullable:\n        raise Unmodeled('host-node-may-be-null')"),
    ('try-around-an-op-dropped', 'cssom_eval.py',
     "                    self.ops.insert(n0, ('try',))\n",
     "                    pass\n"),
    ('expando-of-a-rule-forwarded-as-an-attribute', 'cssom_eval.py',
     "        if key not in RULE_WRITABLE:",
     "        if False:"),
    ('optional-chain-over-a-node-that-may-not-exist-taken-as-there', 'cssom_eval.py',
     "        if e[3] and isinstance(obj, (HostElem, HostObj)) and obj.nullable:\n            raise Unmodeled('host-optional-chain')   # whether the chain stops is a fact of the document\n",
     ""),
    ('catch-with-statements-dropped', 'cssom_eval.py',
     "if handler is None or handler != ('block', []) or fin is not None or not self.guard_safe(blk, declared):",
     "if handler is None or fin is not None or not self.guard_safe(blk, declared):"),
    ('text-content-null-written-as-the-word', 'cssom_eval.py',
     "        interp.emit('tcset' if key == 'textContent' else 'ihset', el.name, null_empty(value))",
     "        interp.emit('tcset' if key == 'textContent' else 'ihset', el.name, to_str(value))"),
]


def load_cases():
    cases, cur, in_page = [], None, False
    for line in open(CASES, encoding='utf-8'):
        if line.startswith('#') and cur is None:
            continue
        if line.startswith('=== '):
            cur = {'name': line[4:].strip(), 'tests': None, 'outcome': {}, 'names': {}, 'has': [], 'unread': False, 'page': []}
            cases.append(cur)
            in_page = False
        elif cur is not None and not in_page:
            line = line.rstrip('\n')
            if line == '---':
                in_page = True
            elif line.startswith('tests '):
                cur['tests'] = int(line.split()[1])
            elif line.startswith('outcome '):
                _, k, what = line.split(None, 2)
                cur['outcome'][int(k)] = what.strip()
            elif line.startswith('has '):
                cur['has'].append(line[4:])
            elif line.startswith('seq '):
                cur.setdefault('seq', []).append(line[4:].split(';'))
            elif line.startswith('name '):
                _, k, what = line.split(None, 2)
                cur['names'][int(k)] = what
            elif line == 'unread':
                cur['unread'] = True
        elif cur is not None:
            cur['page'].append(line)
    for c in cases:
        c['page'] = ''.join(c['page'])
    return cases


def load_evaluator(directory):
    sys.path.insert(0, directory)
    for name in ('cssom_js', 'cssom_eval', 'extract_wpt_cssom_tokens'):
        sys.modules.pop(name, None)
    spec = importlib.util.spec_from_file_location('cssom_eval', os.path.join(directory, 'cssom_eval.py'))
    mod = importlib.util.module_from_spec(spec)
    sys.modules['cssom_eval'] = mod
    spec.loader.exec_module(mod)
    sys.path.pop(0)
    return mod


def run_cases(E, cases):
    problems = []
    for c in cases:
        res = E.evaluate_page('corpus/%s.html' % c['name'], c['page'])
        if c['unread']:
            if res is not None:
                problems.append('%s: the page is read and should be left unread' % c['name'])
            continue
        if res is None:
            problems.append('%s: the page is left unread' % c['name'])
            continue
        if len(res.tests) != c['tests']:
            problems.append('%s: %d tests, expected %d' % (c['name'], len(res.tests), c['tests']))
            continue
        for k, t in enumerate(res.tests):
            want = c['outcome'].get(k, 'ok')
            ops = []
            if t.reason is None:
                grab = False
                for op in res.ops:
                    if op[0] == 'test':
                        grab = int(op[1]) == k
                    elif grab:
                        ops.append(op)
            have = 'unmodeled:' + t.reason if t.reason is not None else ('fail' if any(o[0] == 'fail' for o in ops) else 'ok')
            if have != want:
                problems.append('%s: test %d is %s, expected %s' % (c['name'], k, have, want))
        lines = ['|'.join(op) for op in res.ops]
        for needle in c['has']:
            if not any((l == needle[:-1]) if needle.endswith('$') else l.startswith(needle) for l in lines):
                problems.append('%s: the program has no op that starts with %r' % (c['name'], needle))
        for chain in c.get('seq', []):
            at = 0
            for need in chain:
                while at < len(lines) and not lines[at].startswith(need):
                    at += 1
                if at == len(lines):
                    problems.append('%s: the program has no op that starts with %r after the ones before it in %r' % (c['name'], need, chain))
                    break
                at += 1
        for k, name in c['names'].items():
            if k < len(res.tests) and res.tests[k].name != name:
                problems.append('%s: test %d is named %r, expected %r' % (c['name'], k, res.tests[k].name, name))
    return problems


def node_check(cases):
    """The pages whose tests are all meant to hold (and the ones meant to fail) run under node, with a minimal testharness: a real engine agrees with the hand-checked outcomes."""
    node = shutil.which('node')
    if not node:
        print('check-cssom-eval: node is not available; the cases were checked by hand only')
        return []
    shim = r"""
const results = []; let cur = null; let nameCounter = 0;
function fail(m) { const e = new Error(m); e.assertion = true; throw e; }
function same(a, b) { return Object.is(a, b); }
global.assert_equals = (a, b) => { if (!same(a, b)) fail('assert_equals ' + a + ' ' + b); };
global.assert_not_equals = (a, b) => { if (a === b) fail('assert_not_equals'); };
global.assert_true = (a) => { if (a !== true) fail('assert_true'); };
global.assert_false = (a) => { if (a !== false) fail('assert_false'); };
global.assert_in_array = (a, arr) => { if (!arr.includes(a)) fail('assert_in_array'); };
global.assert_array_equals = (a, b) => { if (a.length !== b.length || !a.every((x, i) => same(x, b[i]))) fail('assert_array_equals'); };
global.assert_approx_equals = (a, b, e) => { if (Math.abs(a - b) > e) fail('assert_approx_equals'); };
global.assert_greater_than = (a, b) => { if (!(a > b)) fail('gt'); };
global.assert_less_than = (a, b) => { if (!(a < b)) fail('lt'); };
global.test = (fn, name) => { const t = { add_cleanup(f) { this.c.push(f); }, c: [] }; let ok = true; try { fn.call(t, t); } catch (e) { if (!e.assertion) throw e; ok = false; } for (const f of t.c.reverse()) { try { f(); } catch (e) {} } results.push(ok); };
global.document = { body: { setAttribute() {}, removeAttribute() {} }, getElementById() { return {}; } };
global.getComputedStyle = () => ({});
"""
    problems = []
    for c in cases:
        uses = re.search(r'getComputedStyle|CSS\.|CSS[A-Z]\w*|document\.|assert_throws|class A|undefinedName|while \(true\) \{ \}', c['page'])
        if c['unread'] or uses and 'cleanup' not in c['name']:
            continue
        scripts = re.findall(r'<script>(.*?)</script>', c['page'], re.S)
        with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False) as f:
            f.write(shim + '\n' + '\n'.join(scripts) + '\nconsole.log(JSON.stringify(results));\n')
            path = f.name
        p = subprocess.run([node, path], capture_output=True, text=True, timeout=60)
        os.unlink(path)
        if p.returncode != 0:
            problems.append('%s: node failed: %s' % (c['name'], p.stderr.strip().splitlines()[-1] if p.stderr.strip() else p.returncode))
            continue
        import json
        got = json.loads(p.stdout.strip().splitlines()[-1])
        if len(got) != c['tests']:
            problems.append('%s: node ran %d tests, expected %d' % (c['name'], len(got), c['tests']))
            continue
        for k, ok in enumerate(got):
            want = c['outcome'].get(k, 'ok')
            if (want == 'ok') != ok:
                problems.append('%s: node says test %d %s, the case says %s' % (c['name'], k, 'passes' if ok else 'fails', want))
    return problems


def main():
    cases = load_cases()
    if '--controls' in sys.argv:
        failed = 0
        with tempfile.TemporaryDirectory() as scratch:
            for plant in PLANTS:
                name, fname, old = plant[0], plant[1], plant[2]
                new = plant[3] if len(plant) > 3 else None
                d = os.path.join(scratch, name)
                os.makedirs(d)
                for f in ('cssom_eval.py', 'cssom_js.py', 'extract-wpt-cssom.py'):
                    shutil.copy(os.path.join(HERE, f), os.path.join(d, f))
                path = os.path.join(d, fname)
                text = open(path, encoding='utf-8').read()
                pairs = old if isinstance(old, list) else [(old, new)]
                bad = [o for o, _ in pairs if text.count(o) != 1]
                if bad:
                    print('control %s: the text to plant is found %d times in %s, not once' % (name, text.count(bad[0]), fname))
                    failed += 1
                    continue
                for o, n in pairs:
                    text = text.replace(o, n)
                open(path, 'w', encoding='utf-8').write(text)
                runner = ("import sys, importlib.util; sys.argv=['x']; spec=importlib.util.spec_from_file_location('c', %r); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); "
                          "E=m.load_evaluator(%r); p=m.run_cases(E, m.load_cases()); print('\\n'.join(p)); sys.exit(1 if p else 0)" % (os.path.join(HERE, 'check-cssom-eval.py'), d))
                try:
                    r = subprocess.run([sys.executable, '-c', runner], capture_output=True, text=True, timeout=60)
                    caught = r.returncode != 0
                    why = (r.stdout.strip().splitlines() or [''])[0]
                except subprocess.TimeoutExpired:
                    caught, why = True, 'the evaluator did not stop'
                if not caught:
                    print('control %s: the corpus does not fail on the planted defect' % name)
                    failed += 1
                else:
                    print('control %s: caught (%s)' % (name, why[:90]))
        if failed:
            return 1
        print('eval corpus: %d defects planted, all caught' % len(PLANTS))
        return 0
    E = load_evaluator(HERE)
    problems = run_cases(E, cases)
    if '--node' in sys.argv:
        problems += node_check(cases)
    if problems:
        print('\n'.join(problems))
        return 1
    print('eval corpus: %d cases, as expected' % len(cases))
    return 0


if __name__ == '__main__':
    sys.exit(main())
