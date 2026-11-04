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
"""The evaluator of the scoring interpreter: a page's script, run offline over a total subset of JavaScript, becomes a flat program.

    evaluate_page(rel, html) -> PageResult | None

The evaluator never touches the library and has no host object of its own. It runs the page's inline scripts in document order over the grammar of tools/css/cssom_js.py, with
the harness (`test`, `setup`, `add_cleanup`, the `assert_*` family) modeled, and writes down what the page does to the *document*: ops (tools/conformance/css_cssom_script_suite.c
says what each does) in the order the page runs them, with every loop unrolled, every function inlined, every template literal, string method and array literal folded to a
constant. A value that only the library can give (`getComputedStyle(x).color`) is a **register** (``Rt``): it can be asserted on, compared with ``===``, and nothing else; any
other use of it (arithmetic, a branch, a string method) raises ``Unmodeled('computed-value-expression')``, because folding it would be a guess.

**Total.** Anything outside the subset raises ``Unmodeled(construct)``; a test whose body raises it is listed with that construct as its reason and contributes no ops, and the
tests after it are listed too when it could have changed the document (``unmodeled:shared-state``); a statement of the *top level* that raises it makes the page unread
(``evaluate_page`` returns None and the page keeps the reasons of tools/css/extract-wpt-cssom.py), because then the set of tests is not known. A step limit makes a runaway
loop a construct too (``step-limit``). Nothing is guessed and nothing is dropped.

Test counts: a page's tests are the `test()` calls its script makes, counted as it runs (so a loop is unrolled and a function called twice is two tests). ``PageResult.static_calls``
is the count of `test(` call sites in the source, the second method of the census; the two differ only for a page that makes tests in a loop or a function, and the page record of the
browsers (tools/oracle/css_page_record.py) is the third: the gate requires all of the names the evaluator gives to be the names the harness reported.
"""
import html.parser
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cssom_js as J   # noqa: E402

STEP_LIMIT = 400000
MAX_OPS = 200000
MAX_STRING = 1 << 20


class Unmodeled(Exception):
    def __init__(self, construct):
        Exception.__init__(self, construct)
        self.construct = construct


class JSThrow(Exception):
    def __init__(self, value, kind='Error'):
        Exception.__init__(self, str(value))
        self.value = value
        self.kind = kind


class AssertFail(JSThrow):
    pass


class BreakSignal(Exception):
    pass


class ContinueSignal(Exception):
    pass


class ReturnSignal(Exception):
    def __init__(self, value):
        Exception.__init__(self)
        self.value = value


class Undef:
    def __repr__(self):
        return 'undefined'

    def __bool__(self):
        return False


UNDEF = Undef()


class JArr(list):
    pass


class JObj(dict):
    pass


class Func:
    def __init__(self, node, scope, name=None):
        self.node = node
        self.scope = scope
        self.name = name or node[5]
        self.props = JObj()


class Native:
    def __init__(self, fn, name=''):
        self.fn = fn
        self.name = name


class Rt:
    """A value only the library can give: a register of the runner. ``kind``: s (string), n (number), b (boolean), sn (string or null), any."""
    __slots__ = ('reg', 'kind')

    def __init__(self, reg, kind):
        self.reg = reg
        self.kind = kind


class Regex:
    def __init__(self, text):
        m = re.match(r'/(.*)/([a-z]*)$', text, re.S)
        self.source, self.flags = m.group(1), m.group(2)
        flags = 0
        if 'i' in self.flags:
            flags |= re.I
        if 's' in self.flags:
            flags |= re.S
        if 'm' in self.flags:
            flags |= re.M
        if re.search(r'\(\?<[=!]|\\p\{|\\u\{|\(\?<\w+>|\\k<', self.source) or 'u' in self.flags or 'y' in self.flags or 'd' in self.flags:
            raise Unmodeled('js-regexp')
        try:
            self.re = re.compile(self.source, flags)
        except re.error:
            raise Unmodeled('js-regexp')
        self.glob = 'g' in self.flags


# ---- conversions -------------------------------------------------------------------------------------------------------------------------

def is_num(v):
    return isinstance(v, float)


def js_num_str(x):
    if x != x:
        return 'NaN'
    if x in (math.inf, -math.inf):
        return 'Infinity' if x > 0 else '-Infinity'
    if x == 0:
        return '0'
    if x == int(x) and abs(x) < 9007199254740992:
        return str(int(x))
    r = repr(abs(x))
    sign = '-' if x < 0 else ''
    # Python's repr is the shortest round-trip digits; rewrite as ECMAScript's Number::toString
    if 'e' in r:
        mant, exp = r.split('e')
        e10 = int(exp)
    else:
        mant, e10 = r, 0
    if '.' in mant:
        ip, fp = mant.split('.')
    else:
        ip, fp = mant, ''
    digits = (ip + fp).lstrip('0')
    lead = len(ip + fp) - len((ip + fp).lstrip('0'))
    point = len(ip) + e10 - lead   # decimal point position relative to the start of `digits`
    digits = digits.rstrip('0') or '0'
    k = len(digits)
    n = point
    if k <= n <= 21:
        return sign + digits + '0' * (n - k)
    if 0 < n <= 21:
        return sign + digits[:n] + '.' + digits[n:]
    if -6 < n <= 0:
        return sign + '0.' + '0' * (-n) + digits
    e = n - 1
    es = ('+' if e >= 0 else '-') + str(abs(e))
    if k == 1:
        return sign + digits + 'e' + es
    return sign + digits[0] + '.' + digits[1:] + 'e' + es


def to_str(v):
    if isinstance(v, str):
        return v
    if v is UNDEF:
        return 'undefined'
    if v is None:
        return 'null'
    if v is True:
        return 'true'
    if v is False:
        return 'false'
    if isinstance(v, float):
        return js_num_str(v)
    if isinstance(v, JArr):
        return ','.join('' if x is UNDEF or x is None else to_str(x) for x in v)
    if isinstance(v, JObj):
        return '[object Object]'
    if isinstance(v, (Func, Native)):
        raise Unmodeled('js-function-to-string')
    if isinstance(v, Rt):
        raise Unmodeled('computed-value-expression')
    raise Unmodeled('host-to-string')


def to_num(v):
    if isinstance(v, float):
        return v
    if v is True:
        return 1.0
    if v is False or v is None:
        return 0.0
    if v is UNDEF:
        return math.nan
    if isinstance(v, str):
        s = v.strip()
        if s == '':
            return 0.0
        if re.fullmatch(r'[+-]?(\d+\.?\d*([eE][+-]?\d+)?|\.\d+([eE][+-]?\d+)?)', s):
            return float(s)
        if re.fullmatch(r'0[xX][0-9a-fA-F]+', s):
            return float(int(s, 16))
        if s in ('Infinity', '+Infinity'):
            return math.inf
        if s == '-Infinity':
            return -math.inf
        return math.nan
    if isinstance(v, JArr):
        return to_num(to_str(v))
    if isinstance(v, JObj):
        return math.nan
    if isinstance(v, Rt):
        raise Unmodeled('computed-value-expression')
    raise Unmodeled('host-to-number')


def truthy(v):
    if isinstance(v, Rt):
        raise Unmodeled('computed-value-expression')
    if v is UNDEF or v is None or v is False:
        return False
    if isinstance(v, float):
        return not (v == 0 or v != v)
    if isinstance(v, str):
        return v != ''
    return True


def typeof(v):
    if v is UNDEF:
        return 'undefined'
    if v is None:
        return 'object'
    if isinstance(v, bool):
        return 'boolean'
    if isinstance(v, float):
        return 'number'
    if isinstance(v, str):
        return 'string'
    if isinstance(v, (Func, Native)):
        return 'function'
    if isinstance(v, Rt):
        raise Unmodeled('computed-value-expression')
    return 'object'


def strict_eq(a, b):
    if isinstance(a, Rt) or isinstance(b, Rt):
        raise Unmodeled('computed-value-expression')
    if isinstance(a, bool) or isinstance(b, bool):
        return isinstance(a, bool) and isinstance(b, bool) and a == b
    if isinstance(a, float) and isinstance(b, float):
        return a == b
    if isinstance(a, str) and isinstance(b, str):
        return a == b
    if a is UNDEF or b is UNDEF:
        return a is b
    if a is None or b is None:
        return a is b
    return a is b


def loose_eq(a, b):
    if isinstance(a, Rt) or isinstance(b, Rt):
        raise Unmodeled('computed-value-expression')
    if (a is None or a is UNDEF) and (b is None or b is UNDEF):
        return True
    if a is None or a is UNDEF or b is None or b is UNDEF:
        return False
    if type(a) is type(b) or (isinstance(a, (JArr, JObj)) and isinstance(b, (JArr, JObj))):
        return strict_eq(a, b)
    if isinstance(a, (JArr, JObj, Func, Native)) or isinstance(b, (JArr, JObj, Func, Native)):
        if isinstance(a, (JArr,)) and not isinstance(b, (JArr, JObj)):
            return loose_eq(to_str(a), b)
        if isinstance(b, (JArr,)) and not isinstance(a, (JArr, JObj)):
            return loose_eq(a, to_str(b))
        return False
    return to_num(a) == to_num(b)


def same_value(a, b):
    """testharness' assert_equals: `===`, except that NaN equals NaN and 0 differs from -0."""
    if isinstance(a, float) and isinstance(b, float):
        if a != a and b != b:
            return True
        if a == 0 and b == 0:
            return math.copysign(1, a) == math.copysign(1, b)
        return a == b
    return strict_eq(a, b)


# ---- the document, as the page's script sees it -----------------------------------------------------------------------------------------

def camel_to_css(name):
    if name == 'cssFloat':
        return 'float'
    if name.startswith('--') or '-' in name:
        return name
    out = re.sub(r'([A-Z])', lambda m: '-' + m.group(1).lower(), name)
    if re.match(r'(webkit|moz|ms|o)-', out):
        out = '-' + out
    return out


def fmt_value(v):
    """testharness' format_value, for the messages the evaluator itself writes (constants)."""
    if isinstance(v, str):
        return '"' + v.replace('\\', '\\\\').replace('"', '\\"') + '"'
    if v is UNDEF:
        return 'undefined'
    if v is None:
        return 'null'
    if isinstance(v, (bool, float)):
        return to_str(v)
    if isinstance(v, JArr):
        return '[' + ', '.join(fmt_value(x) for x in v) + ']'
    return 'object "%s"' % to_str(v) if isinstance(v, JObj) else 'object'


def operand(v):
    """The operand field of an op for a value: a constant or a register."""
    if isinstance(v, Rt):
        return 'r:' + v.reg
    if isinstance(v, HostElem):
        return 'e:' + v.name
    if isinstance(v, str):
        return 's:' + v
    if isinstance(v, bool):
        return 'b:' + ('true' if v else 'false')
    if isinstance(v, float):
        return 'n:' + ('-0' if v == 0 and math.copysign(1.0, v) < 0 else js_num_str(v))
    if v is None:
        return 'N:'
    if v is UNDEF:
        return 'u:'
    raise Unmodeled('host-operand')


class Host:
    """An object of the document: the evaluator knows its name in the program, never its state."""
    kind = 'host'

    def __init__(self, name):
        self.name = name


class HostElem(Host):
    pass


class HostStyle(Host):
    """A CSSStyleDeclaration: ``computed`` is getComputedStyle()'s, else the element's style attribute."""

    def __init__(self, name, computed):
        Host.__init__(self, name)
        self.computed = computed


class HostClassList(Host):
    pass


class HostList(Host):
    """A NodeList: the elements a selector matches, indexed at run time (its length is not known offline)."""

    def __init__(self, root, selector):
        Host.__init__(self, 'list')
        self.root = root
        self.selector = selector


class HostDoc(Host):
    pass


class HostWindow(Host):
    pass


class HostTest(Host):
    pass


class HostMath(Host):
    pass


class HostCSS(Host):
    """The CSS namespace object: `supports` and `escape` are calls of the library (registers), the rest is not modeled."""


# the ops that change the tree, an attribute, a style attribute or a rule: after one the runner's document is no longer a parse of a prefix of the markup
MUTATING_OPS = {'setattr', 'rmattr', 'classadd', 'classremove', 'classtoggle', 'sset', 'sattrset', 'scsstext', 'sremove'}

STYLE_METHODS = {'getPropertyValue', 'getPropertyPriority', 'setProperty', 'removeProperty', 'item'}
STYLE_NON_PROPS = {'length', 'cssText', 'parentRule', 'cssFloat'}
MUTATORS = re.compile(r'^(setAttribute|removeAttribute|setProperty|removeProperty|append|appendChild|prepend|remove|removeChild|insertBefore|replaceWith|replaceChildren|replaceChild|'
                      r'insertAdjacent\w*|add|toggle|replace|replaceSync|insertRule|deleteRule|attachShadow|adoptedStyleSheets|cloneNode|setAttributeNS|removeAttributeNS|'
                      r'toggleAttribute|setHTMLUnsafe|importNode|adoptNode|createElement\w*|write|open|close|click|focus|blur|showPopover|hidePopover|showModal|requestFullscreen)$')


def may_mutate(node, funcs, seen, depth=0):
    """Whether the AST can change the world: an assignment to a member, a call of a mutating method, or anything the evaluator does not read. Conservative."""
    if depth > 6 or not isinstance(node, (tuple, list)):
        return False
    if isinstance(node, list):
        return any(may_mutate(x, funcs, seen, depth) for x in node)
    k = node[0] if node else None
    if k == 'unsup':
        return node[1] != 'js-class'   # a class declaration changes nothing until it is used
    if k == 'new':
        callee = node[1]
        return not (isinstance(callee, tuple) and callee[0] == 'id' and callee[1] in ('Array', 'Object', 'Error', 'TypeError', 'RangeError'))
    if k in ('asg', 'upd'):
        tgt = node[2] if k == 'asg' else node[3]
        if isinstance(tgt, tuple) and tgt[0] in ('mem', 'idx'):
            return True
    if k == 'call':
        callee = node[1]
        if isinstance(callee, tuple) and callee[0] == 'mem' and MUTATORS.match(callee[2]):
            return True
        if isinstance(callee, tuple) and callee[0] == 'id' and callee[1] in funcs and callee[1] not in seen:
            seen.add(callee[1])
            if may_mutate(funcs[callee[1]], funcs, seen, depth + 1):
                return True
    if k == 'un' and node[1] == 'delete':
        return True
    return any(may_mutate(x, funcs, seen, depth) for x in node[1:] if isinstance(x, (tuple, list)))


def declared_names(node, out):
    """Every name a function body declares anywhere inside it (parameters, var/let/const, function declarations, loop and catch bindings)."""
    if not isinstance(node, (tuple, list)):
        return
    if isinstance(node, list):
        for x in node:
            declared_names(x, out)
        return
    k = node[0] if node else None
    if k == 'var':
        for target, init in node[2]:
            out.update(pattern_names(target))
    elif k in ('forof', 'forin'):
        if node[1] is not None:
            out.update(pattern_names(node[2]))
    elif k == 'fdecl':
        out.add(node[1])
    elif k == 'try' and node[2] is not None:
        out.update(pattern_names(node[2]))
    elif k == 'func':
        for p in node[1]:
            out.update(pattern_names(p))
    for x in node[1:]:
        if isinstance(x, (tuple, list)):
            declared_names(x, out)


def outer_writes(node, declared, out):
    """The names the AST assigns to without declaring them: variables of the page, whose value the evaluator loses with a test it cannot run."""
    if not isinstance(node, (tuple, list)):
        return
    if isinstance(node, list):
        for x in node:
            outer_writes(x, declared, out)
        return
    k = node[0] if node else None
    if k in ('asg', 'upd'):
        t = node[2] if k == 'asg' else node[3]
        if isinstance(t, tuple):
            names = pattern_names(t) if t[0] in ('pid', 'parr', 'pobj') else []
            out.update(n for n in names if n not in declared)
    for x in node[1:]:
        if isinstance(x, (tuple, list)):
            outer_writes(x, declared, out)


def writes_outer(node, declared):
    out = set()
    outer_writes(node, declared, out)
    return bool(out)


def func_may_change_world(fn_node, funcs):
    """Whether a test function can change the document or a variable of the page."""
    declared = set()
    declared_names(fn_node, declared)
    return may_mutate(fn_node[2], funcs, set()) or writes_outer(fn_node[2], declared)


class Scope:
    __slots__ = ('vars', 'parent', 'fn')

    def __init__(self, parent=None, fn=False):
        self.vars = {}
        self.parent = parent
        self.fn = fn

    def lookup(self, name):
        s = self
        while s is not None:
            if name in s.vars:
                return s
            s = s.parent
        return None


class TestState:
    def __init__(self, index, name):
        self.index = index
        self.name = name
        self.cleanups = []
        self.failed = False


class TestRecord:
    def __init__(self, index, name):
        self.index = index
        self.name = name
        self.reason = None   # None: modeled; else the construct (without the code prefix)


class PageResult:
    def __init__(self):
        self.ops = []
        self.tests = []          # TestRecord, in the order the page made them
        self.static_calls = 0
        self.quirks = False
        self.html = ''
        self.ids = []


class Interp:
    def __init__(self, ids, dom_ready_flags=None):
        self.ops = []
        self.steps = 0
        self.regs = 0
        self.nodes = 0
        self.tests = []
        self.cur = None            # TestState
        self.poisoned = False
        self.try_depth = 0
        self.ids = set(ids)
        self.glob = Scope(None, True)
        self.funcs = {}            # name -> function body AST (for may_mutate)
        self.stale = False         # the document of this script is longer than the one the runner holds and the program has changed it: it cannot be parsed again
        self.extent = None         # the length of the markup the runner's document was parsed from (a prefix of the page), None before the first `doc`
        self.mutated = False
        self.title = None
        self.default_names = 0
        self.tainted = set()       # page variables a test the evaluator could not run assigns: their value is not known
        self.install_globals()

    # ---- ops ----
    def emit(self, *fields):
        if len(self.ops) > MAX_OPS:
            raise Unmodeled('op-limit')
        if fields[0] in MUTATING_OPS:
            self.mutated = True
        self.ops.append(tuple(str(f) for f in fields))

    def enter_script(self, end, visible_ids):
        """A script is about to run. The document it sees is the markup up to and including its own end tag (the parser has inserted the script element and no more): the runner
        parses that prefix (``doc``), which is possible while the program has not changed the tree, since a parse of a longer prefix would lose the change."""
        self.ids = set(visible_ids)
        self.ops.append(('script', str(end)))
        if self.extent == end:
            self.stale = False
        elif self.mutated and self.extent is not None:
            self.stale = True
        else:
            self.stale = False
            self.extent = end
            self.ops.append(('doc', str(end)))

    def new_reg(self):
        self.regs += 1
        return 'r%d' % self.regs

    def new_node(self, prefix):
        self.nodes += 1
        return '%s%d' % (prefix, self.nodes)

    def need_dom(self):
        if self.stale:
            raise Unmodeled('script-before-markup-end')

    # ---- scope and globals ----
    def install_globals(self):
        g = self.glob.vars
        doc = HostDoc('document')
        win = HostWindow('window')
        g['document'] = doc
        g['window'] = win
        g['self'] = win
        g['globalThis'] = win
        g['undefined'] = UNDEF
        g['NaN'] = math.nan
        g['Infinity'] = math.inf
        g['Math'] = HostMath('Math')
        g['CSS'] = HostCSS('CSS')
        g['console'] = JObj(log=Native(lambda i, t, a: UNDEF, 'log'), warn=Native(lambda i, t, a: UNDEF), error=Native(lambda i, t, a: UNDEF), info=Native(lambda i, t, a: UNDEF))
        for name in ('test', 'setup', 'assert_equals', 'assert_not_equals', 'assert_true', 'assert_false', 'assert_array_equals', 'assert_approx_equals', 'assert_greater_than',
                     'assert_less_than', 'assert_greater_than_equal', 'assert_less_than_equal', 'assert_in_array', 'assert_unreached', 'assert_regexp_match', 'assert_class_string',
                     'add_completion_callback', 'done', 'getComputedStyle', 'parseInt', 'parseFloat', 'isNaN', 'String', 'Number', 'Boolean', 'Array', 'Object', 'JSON', 'isFinite',
                     'assert_throws_dom', 'assert_throws_js', 'assert_throws_exactly', 'assert_implements', 'assert_implements_optional'):
            g[name] = Native(getattr(self, 'n_' + name, None) or self.n_unsupported(name), name)

    def n_unsupported(self, name):
        def f(interp, this, args):
            raise Unmodeled('harness-' + name)
        return f

    # ---- the harness ----
    def n_setup(self, interp, this, args):
        for a in args:
            if isinstance(a, (Func,)):
                self.call(a, UNDEF, [])
        return UNDEF

    def n_add_completion_callback(self, interp, this, args):
        return UNDEF

    def n_done(self, interp, this, args):
        return UNDEF

    def n_test(self, interp, this, args):
        if not args or not isinstance(args[0], Func):
            raise Unmodeled('harness-test-argument')
        if self.cur is not None:
            raise Unmodeled('harness-nested-test')
        idx = len(self.tests)
        name = ''
        if len(args) > 1 and args[1] is not UNDEF and not (isinstance(args[1], str) and args[1] == ''):
            try:
                name = to_str(args[1]) if not isinstance(args[1], Rt) else ''
            except Unmodeled:
                name = ''
        else:
            # testharness.js: the title of the page, then the title and a counter
            name = (self.title or '') + (' %d' % self.default_names if self.default_names > 0 else '')
            self.default_names += 1
        rec = TestRecord(idx, name)
        self.tests.append(rec)
        was_poisoned = self.poisoned
        checkpoint = len(self.ops)
        reg_checkpoint = self.regs
        self.cur = TestState(idx, name)
        self.emit('test', idx, name)
        try:
            try:
                self.call(args[0], HostTest('t'), [HostTest('t')])
            except AssertFail as a:
                self.emit('fail', a.value)
            except JSThrow as t:
                raise Unmodeled('js-exception')
            # the cleanups run after the body, in reverse order, whether it passed or not
            self.emit('cleanup')
            for fn in reversed(self.cur.cleanups):
                try:
                    self.call(fn, HostTest('t'), [])
                except AssertFail:
                    pass
            self.emit('end')
            if was_poisoned:
                # an earlier test the evaluator could not read may have changed the document: this one is read to name its own reason if it has one, and is not scored
                del self.ops[checkpoint:]
                self.regs = reg_checkpoint
                rec.reason = 'shared-state'
        except Unmodeled as u:
            del self.ops[checkpoint:]
            self.regs = reg_checkpoint
            rec.reason = u.construct
            if func_may_change_world(args[0].node, self.funcs):
                self.poisoned = True
            declared = set()
            declared_names(args[0].node, declared)
            outer_writes(args[0].node[2], declared, self.tainted)
        finally:
            self.cur = None
        return UNDEF

    def need_test(self):
        if self.cur is None:
            raise Unmodeled('harness-assert-outside-test')

    def desc(self, a, k):
        if len(a) > k:
            try:
                return to_str(a[k]) if not isinstance(a[k], Rt) else ''
            except Unmodeled:
                return ''
        return ''

    def runtime_assert(self, op, vals, msg):
        if self.try_depth:
            raise Unmodeled('harness-assert-in-try')
        self.emit(op, *([operand(v) for v in vals] + [msg]))

    def fail(self, text, msg):
        raise AssertFail(('assert_%s: %s%s' % (text, (msg + ' ') if msg else '', '')).strip())

    def n_assert_equals(self, interp, this, a):
        self.need_test()
        msg = self.desc(a, 2)
        if len(a) < 2:
            raise Unmodeled('harness-assert-arity')
        if isinstance(a[0], Rt) or isinstance(a[1], Rt) or (isinstance(a[0], HostElem) and a[0] is not a[1]) or (isinstance(a[1], HostElem) and a[0] is not a[1]):
            if any(not isinstance(x, (Rt, str, float, bool, HostElem)) and x is not None and x is not UNDEF for x in a[:2]):
                raise Unmodeled('computed-value-compared-with-object')
            self.runtime_assert('assert_equals', a[:2], msg)
            return UNDEF
        if not same_value(a[0], a[1]):
            raise AssertFail('assert_equals: %sexpected %s but got %s' % (msg + ' ' if msg else '', fmt_value(a[1]), fmt_value(a[0])))
        return UNDEF

    def n_assert_not_equals(self, interp, this, a):
        self.need_test()
        msg = self.desc(a, 2)
        if len(a) < 2:
            raise Unmodeled('harness-assert-arity')
        if isinstance(a[0], Rt) or isinstance(a[1], Rt) or (isinstance(a[0], HostElem) and a[0] is not a[1]) or (isinstance(a[1], HostElem) and a[0] is not a[1]):
            if any(not isinstance(x, (Rt, str, float, bool, HostElem)) and x is not None and x is not UNDEF for x in a[:2]):
                raise Unmodeled('computed-value-compared-with-object')
            self.runtime_assert('assert_not_equals', a[:2], msg)
            return UNDEF
        if strict_eq(a[0], a[1]) or (isinstance(a[0], float) and isinstance(a[1], float) and a[0] != a[0] and a[1] != a[1]):
            raise AssertFail('assert_not_equals: %sgot disallowed value %s' % (msg + ' ' if msg else '', fmt_value(a[0])))
        return UNDEF

    def n_assert_true(self, interp, this, a):
        self.need_test()
        msg = self.desc(a, 1)
        if not a:
            raise Unmodeled('harness-assert-arity')
        if isinstance(a[0], Rt):
            self.runtime_assert('assert_true', a[:1], msg)
            return UNDEF
        if a[0] is not True:
            raise AssertFail('assert_true: %sexpected true got %s' % (msg + ' ' if msg else '', fmt_value(a[0])))
        return UNDEF

    def n_assert_false(self, interp, this, a):
        self.need_test()
        msg = self.desc(a, 1)
        if not a:
            raise Unmodeled('harness-assert-arity')
        if isinstance(a[0], Rt):
            self.runtime_assert('assert_false', a[:1], msg)
            return UNDEF
        if a[0] is not False:
            raise AssertFail('assert_false: %sexpected false got %s' % (msg + ' ' if msg else '', fmt_value(a[0])))
        return UNDEF

    def n_assert_unreached(self, interp, this, a):
        self.need_test()
        raise AssertFail('assert_unreached: %sReached unreachable code' % (self.desc(a, 0) + ' ' if self.desc(a, 0) else ''))

    def n_assert_in_array(self, interp, this, a):
        self.need_test()
        msg = self.desc(a, 2)
        if len(a) < 2 or not isinstance(a[1], JArr):
            raise Unmodeled('harness-assert-arity')
        if isinstance(a[0], Rt):
            if any(isinstance(x, Rt) or not isinstance(x, (str, float, bool)) and x is not None and x is not UNDEF for x in a[1]):
                raise Unmodeled('computed-value-compared-with-object')
            if self.try_depth:
                raise Unmodeled('harness-assert-in-try')
            self.emit('assert_in_array', operand(a[0]), *([operand(x) for x in a[1]] + [msg]))
            return UNDEF
        if not any(strict_eq(a[0], x) for x in a[1]):
            raise AssertFail('assert_in_array: %svalue %s not in array %s' % (msg + ' ' if msg else '', fmt_value(a[0]), fmt_value(a[1])))
        return UNDEF

    def n_assert_array_equals(self, interp, this, a):
        self.need_test()
        msg = self.desc(a, 2)
        if len(a) < 2:
            raise Unmodeled('harness-assert-arity')
        for x in a[:2]:
            if not isinstance(x, JArr):
                raise Unmodeled('harness-assert-array-argument')
        if any(isinstance(y, Rt) for x in a[:2] for y in x):
            if len(a[0]) != len(a[1]):
                raise AssertFail('assert_array_equals: %slengths differ, expected array %s length %d, got %s length %d' % (msg + ' ' if msg else '', fmt_value(a[1]), len(a[1]), fmt_value(a[0]), len(a[0])))
            for x, y in zip(a[0], a[1]):
                self.n_assert_equals(interp, this, [x, y, msg])
            return UNDEF
        if len(a[0]) != len(a[1]) or not all(same_value(x, y) for x, y in zip(a[0], a[1])):
            raise AssertFail('assert_array_equals: %sexpected array %s got %s' % (msg + ' ' if msg else '', fmt_value(a[1]), fmt_value(a[0])))
        return UNDEF

    def numeric_assert(self, name, a, test, nargs):
        self.need_test()
        msg = self.desc(a, nargs)
        if len(a) < nargs:
            raise Unmodeled('harness-assert-arity')
        if any(isinstance(x, Rt) for x in a[:nargs]):
            self.runtime_assert(name, a[:nargs], msg)
            return UNDEF
        if not all(isinstance(x, float) for x in a[:nargs]):
            raise AssertFail('%s: %swrong type' % (name, msg + ' ' if msg else ''))
        if not test(*a[:nargs]):
            raise AssertFail('%s: %sfailed for %s' % (name, msg + ' ' if msg else '', ', '.join(fmt_value(x) for x in a[:nargs])))
        return UNDEF

    def n_assert_approx_equals(self, interp, this, a):
        return self.numeric_assert('assert_approx_equals', a, lambda x, y, e: abs(x - y) <= e, 3)

    def n_assert_greater_than(self, interp, this, a):
        return self.numeric_assert('assert_greater_than', a, lambda x, y: x > y, 2)

    def n_assert_less_than(self, interp, this, a):
        return self.numeric_assert('assert_less_than', a, lambda x, y: x < y, 2)

    def n_assert_greater_than_equal(self, interp, this, a):
        return self.numeric_assert('assert_greater_than_equal', a, lambda x, y: x >= y, 2)

    def n_assert_less_than_equal(self, interp, this, a):
        return self.numeric_assert('assert_less_than_equal', a, lambda x, y: x <= y, 2)

    def n_getComputedStyle(self, interp, this, a):
        self.need_dom()
        if not a or not isinstance(a[0], HostElem):
            raise Unmodeled('host-getComputedStyle-argument')
        pseudo = ''
        if len(a) > 1 and a[1] is not UNDEF:
            if isinstance(a[1], str):
                pseudo = a[1]
            elif a[1] is None:
                pseudo = ''
            else:
                raise Unmodeled('host-getComputedStyle-argument')
        s = self.new_node('s')
        self.emit('gcs', s, a[0].name, pseudo)
        return HostStyle(s, True)

    def n_parseInt(self, interp, this, a):
        s = to_str(a[0]).strip() if a else 'undefined'
        radix = int(to_num(a[1])) if len(a) > 1 and a[1] is not UNDEF else 10
        m = re.match(r'[+-]?\d+', s) if radix == 10 else None
        return float(int(m.group(0))) if m else math.nan

    def n_parseFloat(self, interp, this, a):
        s = to_str(a[0]).strip() if a else 'undefined'
        m = re.match(r'[+-]?(\d+\.?\d*([eE][+-]?\d+)?|\.\d+([eE][+-]?\d+)?)', s)
        return float(m.group(0)) if m else math.nan

    def n_isNaN(self, interp, this, a):
        x = to_num(a[0]) if a else math.nan
        return x != x

    def n_isFinite(self, interp, this, a):
        x = to_num(a[0]) if a else math.nan
        return x == x and x not in (math.inf, -math.inf)

    def n_String(self, interp, this, a):
        return to_str(a[0]) if a else ''

    def n_Number(self, interp, this, a):
        return to_num(a[0]) if a else 0.0

    def n_Boolean(self, interp, this, a):
        return truthy(a[0]) if a else False

    # ---- steps ----
    def tick(self):
        self.steps += 1
        if self.steps > STEP_LIMIT:
            raise Unmodeled('step-limit')

    # ---- calling ----
    def call(self, fn, this, args):
        if isinstance(fn, Native):
            return fn.fn(self, this, args)
        if not isinstance(fn, Func):
            if isinstance(fn, Rt):
                raise Unmodeled('computed-value-expression')
            raise Unmodeled('js-call-non-function')
        node = fn.node
        if node[4]:
            raise Unmodeled('js-async-function')
        scope = Scope(fn.scope, True)
        if not node[3]:
            scope.vars['this'] = this
            scope.vars['arguments'] = JArr(args)
        for i, p in enumerate(node[1]):
            if p[0] == 'prest':
                self.bind(p[1], JArr(args[i:]), scope, 'let')
            else:
                self.bind(p, args[i] if i < len(args) else UNDEF, scope, 'let')
        if self.depth > 80:
            raise Unmodeled('js-recursion')
        self.depth += 1
        try:
            self.hoist(node[2], scope)
            for st in node[2]:
                self.exec(st, scope)
        except ReturnSignal as r:
            return r.value
        finally:
            self.depth -= 1
        return UNDEF

    depth = 0

    def hoist(self, body, scope):
        for st in body:
            if st[0] == 'fdecl':
                f = Func(st[2], scope, st[1])
                scope.vars[st[1]] = f
                if st[1]:
                    self.funcs[st[1]] = st[2][2]

    # ---- binding ----
    def bind(self, pat, value, scope, kind):
        k = pat[0]
        if k == 'pid':
            if kind == 'var':
                s = scope
                while not s.fn:
                    s = s.parent
                s.vars[pat[1]] = value
            elif kind == 'assign':
                s = scope.lookup(pat[1]) or self.glob
                s.vars[pat[1]] = value
            else:
                scope.vars[pat[1]] = value
        elif k == 'pdef':
            self.bind(pat[1], self.eval(pat[2], scope) if value is UNDEF else value, scope, kind)
        elif k == 'parr':
            if not isinstance(value, (JArr, str)):
                raise Unmodeled('js-destructure')
            items = list(value)
            for i, p in enumerate(pat[1]):
                if p is None:
                    continue
                if p[0] == 'prest':
                    self.bind(p[1], JArr(items[i:]), scope, kind)
                else:
                    self.bind(p, items[i] if i < len(items) else UNDEF, scope, kind)
        elif k == 'pobj':
            if not isinstance(value, JObj):
                raise Unmodeled('js-destructure')
            used = set()
            for pr in pat[1]:
                if pr[0] == 'rest':
                    self.bind(pr[2], JObj({a: b for a, b in value.items() if a not in used}), scope, kind)
                    continue
                key = to_str(self.eval(pr[1], scope)) if pr[3] else pr[1]
                used.add(key)
                self.bind(pr[2], value.get(key, UNDEF), scope, kind)
        elif k in ('mem', 'idx'):
            self.assign_member(pat, value, scope)
        else:
            raise Unmodeled('js-pattern')

    # ---- statements ----
    def exec(self, st, scope):
        self.tick()
        k = st[0]
        if k == 'expr':
            self.eval(st[1], scope)
        elif k == 'var':
            for target, init in st[2]:
                if init is None:
                    if st[1] == 'var' and target[0] == 'pid' and scope.lookup(target[1]) is not None:
                        continue
                    self.bind(target, UNDEF, scope, st[1])
                else:
                    v = self.eval(init, scope)
                    if isinstance(v, Func) and v.name is None and target[0] == 'pid':
                        v.name = target[1]
                        self.funcs.setdefault(target[1], v.node[2])
                    self.bind(target, v, scope, st[1])
        elif k == 'fdecl':
            pass
        elif k == 'ret':
            raise ReturnSignal(UNDEF if st[1] is None else self.eval(st[1], scope))
        elif k == 'if':
            if truthy(self.eval(st[1], scope)):
                self.exec(st[2], scope)
            elif st[3] is not None:
                self.exec(st[3], scope)
        elif k == 'block':
            inner = Scope(scope)
            self.hoist(st[1], inner)
            for s in st[1]:
                self.exec(s, inner)
        elif k == 'for':
            self.exec_for(st, scope)
        elif k == 'forof':
            it = self.eval(st[3], scope)
            if isinstance(it, str):
                if any(ord(c) > 0xFFFF for c in it):
                    raise Unmodeled('utf16-string-index')
                seq = list(it)
            elif isinstance(it, JArr):
                seq = list(it)
            elif isinstance(it, JObj) and it.get('__entries__') is not None:
                seq = list(it['__entries__'])
            else:
                raise Unmodeled('js-iterate-nonarray' if not isinstance(it, HostList) else 'dom-iterate-nodelist')
            for v in seq:
                self.tick()
                inner = Scope(scope)
                self.bind(st[2], v, inner, st[1] or 'assign')
                try:
                    self.exec(st[4], inner)
                except ContinueSignal:
                    continue
                except BreakSignal:
                    break
        elif k == 'forin':
            o = self.eval(st[3], scope)
            if isinstance(o, JObj):
                keys = list(o.keys())
            elif isinstance(o, JArr):
                keys = [str(i) for i in range(len(o))]
            else:
                raise Unmodeled('js-iterate-nonarray')
            for key in keys:
                self.tick()
                inner = Scope(scope)
                self.bind(st[2], key, inner, st[1] or 'assign')
                try:
                    self.exec(st[4], inner)
                except ContinueSignal:
                    continue
                except BreakSignal:
                    break
        elif k == 'while':
            while truthy(self.eval(st[1], scope)):
                self.tick()
                try:
                    self.exec(st[2], scope)
                except ContinueSignal:
                    continue
                except BreakSignal:
                    break
        elif k == 'dowhile':
            while True:
                self.tick()
                try:
                    self.exec(st[1], scope)
                except ContinueSignal:
                    pass
                except BreakSignal:
                    break
                if not truthy(self.eval(st[2], scope)):
                    break
        elif k == 'break':
            raise BreakSignal()
        elif k == 'continue':
            raise ContinueSignal()
        elif k == 'throw':
            v = self.eval(st[1], scope)
            raise JSThrow(v)
        elif k == 'try':
            self.exec_try(st, scope)
        elif k == 'empty':
            pass
        elif k == 'unsup':
            raise Unmodeled(st[1])
        else:
            raise Unmodeled('js-statement-' + k)

    def exec_try(self, st, scope):
        _, blk, param, handler, fin = st
        try:
            try:
                self.try_depth += 1
                try:
                    self.exec(blk, scope)
                finally:
                    self.try_depth -= 1
            except (JSThrow,) as t:
                if handler is None:
                    raise
                inner = Scope(scope)
                if param is not None:
                    self.bind(param, t.value if not isinstance(t, AssertFail) else JObj(message=t.value, name='AssertionError'), inner, 'let')
                self.exec(handler, inner)
        finally:
            if fin is not None:
                self.exec(fin, scope)

    def exec_for(self, st, scope):
        _, init, test, update, body = st
        loop = Scope(scope)
        names = []
        if init is not None:
            self.exec(init, loop)
            if init[0] == 'var' and init[1] in ('let', 'const'):
                names = [n for t, _ in init[2] for n in pattern_names(t)]
        while True:
            self.tick()
            if test is not None and not truthy(self.eval(test, loop)):
                break
            # a fresh binding of the loop's variables for each iteration, so that a closure sees its own
            it = Scope(loop)
            for n in names:
                it.vars[n] = loop.vars[n]
            try:
                self.exec(body, it)
            except ContinueSignal:
                pass
            except BreakSignal:
                break
            for n in names:
                loop.vars[n] = it.vars[n]
            if update is not None:
                self.eval(update, loop)

    # ---- expressions ----
    def eval(self, e, scope):
        self.tick()
        k = e[0]
        m = getattr(self, 'e_' + k, None)
        if m is None:
            raise Unmodeled('js-expression-' + k)
        return m(e, scope)

    def e_num(self, e, scope):
        return float(e[1])

    def e_str(self, e, scope):
        return e[1]

    def e_lit(self, e, scope):
        return e[1]

    def e_paren(self, e, scope):
        return self.eval(e[1], scope)

    def e_this(self, e, scope):
        s = scope.lookup('this')
        return s.vars['this'] if s is not None else UNDEF

    def e_unsup(self, e, scope):
        raise Unmodeled(e[1])

    def e_regex(self, e, scope):
        return Regex(e[1])

    def e_tmpl(self, e, scope):
        out = [e[1][0]]
        for s, x in zip(e[1][1:], e[2]):
            v = self.eval(x, scope)
            out.append(to_str(v))
            out.append(s)
        r = ''.join(out)
        if len(r) > MAX_STRING:
            raise Unmodeled('string-limit')
        return r

    def e_id(self, e, scope):
        name = e[1]
        s = scope.lookup(name)
        if s is not None:
            if name in self.tainted and s is self.glob:
                raise Unmodeled('state-of-an-unmodeled-test')
            return s.vars[name]
        if name in self.ids:
            self.need_dom()
            n = self.new_node('e')
            self.emit('elem', n, 'byid', name)
            return HostElem(n)
        raise Unmodeled(('host-global-' if name[:1].isupper() or name in ('CSS', 'document', 'navigator', 'location') else 'js-reference-error-') + name)

    def e_arr(self, e, scope):
        out = JArr()
        for x in e[1]:
            if x is None:
                out.append(UNDEF)
            elif x[0] == 'spread':
                v = self.eval(x[1], scope)
                if isinstance(v, (JArr, str)):
                    out.extend(list(v))
                else:
                    raise Unmodeled('js-spread')
            else:
                out.append(self.eval(x, scope))
        return out

    def e_obj(self, e, scope):
        out = JObj()
        for kind, key, val, computed in e[1]:
            if kind == 'spread':
                v = self.eval(val, scope)
                if isinstance(v, JObj):
                    out.update(v)
                elif v is None or v is UNDEF:
                    pass
                else:
                    raise Unmodeled('js-spread')
                continue
            if kind in ('get', 'set'):
                raise Unmodeled('js-accessor')
            kk = to_str(self.eval(key, scope)) if computed else key
            v = self.eval(val, scope)
            if isinstance(v, Func) and v.name is None:
                v.name = kk
            out[kk] = v
        return out

    def e_func(self, e, scope):
        f = Func(e, scope)
        return f

    def e_seq(self, e, scope):
        v = UNDEF
        for x in e[1]:
            v = self.eval(x, scope)
        return v

    def e_cond(self, e, scope):
        return self.eval(e[2], scope) if truthy(self.eval(e[1], scope)) else self.eval(e[3], scope)

    def e_log(self, e, scope):
        op = e[1]
        a = self.eval(e[2], scope)
        if op == '&&':
            return self.eval(e[3], scope) if truthy(a) else a
        if op == '||':
            return a if truthy(a) else self.eval(e[3], scope)
        if isinstance(a, Rt):
            raise Unmodeled('computed-value-expression')
        return self.eval(e[3], scope) if (a is None or a is UNDEF) else a

    def e_un(self, e, scope):
        op = e[1]
        if op == 'typeof' and e[2][0] == 'id' and e[2][1] not in self.names_in(scope, e[2][1]):
            return 'undefined'
        v = self.eval(e[2], scope)
        if op == '!':
            if isinstance(v, Rt):
                if v.kind not in ('b', 'any'):
                    raise Unmodeled('computed-value-expression')
                r = self.new_reg()
                self.emit('not', r, 'r:' + v.reg)
                return Rt(r, 'b')
            return not truthy(v)
        if op == '-':
            return -to_num(v)
        if op == '+':
            return to_num(v)
        if op == '~':
            return float(~int(to_num(v)) & 0xFFFFFFFF if False else ~to_int32(to_num(v)))
        if op == 'typeof':
            return typeof(v)
        if op == 'void':
            return UNDEF
        raise Unmodeled('js-unary-' + op)

    def names_in(self, scope, name):
        s = scope.lookup(name)
        if s is not None:
            return [name]
        if name in self.ids:
            return [name]
        return []

    def e_bin(self, e, scope):
        op = e[1]
        a = self.eval(e[2], scope)
        b = self.eval(e[3], scope)
        if op in ('===', '!==', '==', '!=') and (isinstance(a, HostElem) or isinstance(b, HostElem)) and not isinstance(a, Rt) and not isinstance(b, Rt) and a is not b:
            # whether two lookups found the same element is a fact of the document, not of the page: the runner compares the nodes
            if not all(isinstance(x, HostElem) or x is None or x is UNDEF for x in (a, b)):
                raise Unmodeled('host-element-compared-with-value')
            r = self.new_reg()
            self.emit('cmp', r, {'===': 'eq', '!==': 'ne', '==': 'leq', '!=': 'lne'}[op], operand(a), operand(b))
            return Rt(r, 'b')
        if isinstance(a, Rt) or isinstance(b, Rt):
            if op in ('===', '!==', '==', '!='):
                r = self.new_reg()
                if op in ('==', '!=') and not (isinstance(a, (str, float, bool)) and isinstance(b, (str, float, bool))) and False:
                    raise Unmodeled('computed-value-expression')
                if self.try_depth:
                    pass
                self.emit('cmp', r, {'===': 'eq', '!==': 'ne', '==': 'leq', '!=': 'lne'}[op], operand(a), operand(b))
                return Rt(r, 'b')
            raise Unmodeled('computed-value-expression')
        if op == '===':
            return strict_eq(a, b)
        if op == '!==':
            return not strict_eq(a, b)
        if op == '==':
            return loose_eq(a, b)
        if op == '!=':
            return not loose_eq(a, b)
        if op == '+':
            if isinstance(a, JArr) or isinstance(b, JArr) or isinstance(a, JObj) or isinstance(b, JObj):
                return to_str(a) + to_str(b)
            if isinstance(a, str) or isinstance(b, str):
                r = to_str(a) + to_str(b)
                if len(r) > MAX_STRING:
                    raise Unmodeled('string-limit')
                return r
            return to_num(a) + to_num(b)
        if op in ('<', '>', '<=', '>='):
            if isinstance(a, str) and isinstance(b, str):
                x, y = a, b
            else:
                x, y = to_num(a), to_num(b)
                if x != x or y != y:
                    return False
            return {'<': x < y, '>': x > y, '<=': x <= y, '>=': x >= y}[op]
        if op in ('-', '*', '/', '%', '**'):
            x, y = to_num(a), to_num(b)
            if op == '-':
                return x - y
            if op == '*':
                return x * y
            if op == '/':
                if y == 0:
                    if x == 0 or x != x:
                        return math.nan
                    return math.copysign(math.inf, x) * (math.copysign(1, y))
                return x / y
            if op == '%':
                if y == 0 or x != x or y != y or x in (math.inf, -math.inf):
                    return math.nan
                if y in (math.inf, -math.inf):
                    return x
                r = math.fmod(x, y)
                return r
            try:
                return float(x ** y)
            except (OverflowError, ZeroDivisionError):
                return math.inf
        if op in ('&', '|', '^', '<<', '>>', '>>>'):
            x, y = to_int32(to_num(a)), to_int32(to_num(b))
            if op == '&':
                return float(x & y)
            if op == '|':
                return float(x | y)
            if op == '^':
                return float(x ^ y)
            if op == '<<':
                return float(to_int32(float(x << (y & 31))))
            if op == '>>':
                return float(x >> (y & 31))
            return float((x & 0xFFFFFFFF) >> (y & 31))
        if op == 'in':
            if isinstance(b, JObj):
                return to_str(a) in b
            if isinstance(b, JArr):
                return to_str(a) in [str(i) for i in range(len(b))] or to_str(a) == 'length'
            if isinstance(b, HostStyle) and not isinstance(a, Rt):
                # whether the declaration object has the attribute (a property's camel-cased or dashed name): a fact of the library's property table
                r = self.new_reg()
                self.emit('shas', r, b.name, to_str(a))
                return Rt(r, 'b')
            raise Unmodeled('js-in-operator')
        raise Unmodeled('js-operator-' + op)

    def e_asg(self, e, scope):
        op, target, rhs = e[1], e[2], e[3]
        if op == '=':
            v = self.eval(rhs, scope)
            if isinstance(v, Func) and v.name is None and target[0] == 'pid':
                v.name = target[1]
            self.bind(target, v, scope, 'assign')
            return v
        base = {'+=': '+', '-=': '-', '*=': '*', '/=': '/', '%=': '%', '**=': '**', '<<=': '<<', '>>=': '>>', '>>>=': '>>>', '&=': '&', '|=': '|', '^=': '^'}.get(op)
        cur = self.read_target(target, scope)
        if base is None:
            lop = op[:-1]
            if lop == '&&' and not truthy(cur):
                return cur
            if lop == '||' and truthy(cur):
                return cur
            if lop == '??' and not (cur is None or cur is UNDEF):
                return cur
            v = self.eval(rhs, scope)
        else:
            v = self.e_bin(('bin', base, ('val', cur), ('val', self.eval(rhs, scope))), scope)
        self.bind(target, v, scope, 'assign')
        return v

    def e_val(self, e, scope):
        return e[1]

    def read_target(self, t, scope):
        if t[0] == 'pid':
            return self.e_id(('id', t[1]), scope)
        return self.eval(t, scope)

    def e_upd(self, e, scope):
        cur = to_num(self.read_target(e[3], scope))
        new = cur + (1.0 if e[1] == '++' else -1.0)
        self.bind(e[3], new, scope, 'assign')
        return new if e[2] else cur

    def assign_member(self, t, value, scope):
        obj = self.eval(t[1], scope)
        key = t[2] if t[0] == 'mem' else self.eval(t[2], scope)
        if isinstance(obj, JArr):
            if key == 'length':
                n = int(to_num(value))
                del obj[n:]
                obj.extend([UNDEF] * (n - len(obj)))
                return
            i = to_num(key) if not isinstance(key, str) else to_num(key)
            if i != i or i != int(i) or i < 0:
                raise Unmodeled('js-array-property')
            i = int(i)
            while len(obj) <= i:
                obj.append(UNDEF)
            obj[i] = value
        elif isinstance(obj, JObj):
            obj[to_str(key)] = value
        elif isinstance(obj, Host):
            self.host_set(obj, to_str(key) if not isinstance(key, str) else key, value)
        elif isinstance(obj, Func):
            obj.props[to_str(key)] = value
        else:
            raise Unmodeled('js-assign-member')

    def e_mem(self, e, scope):
        obj = self.eval(e[1], scope)
        if e[3] and (obj is None or obj is UNDEF):
            return UNDEF
        return self.get(obj, e[2])

    def e_idx(self, e, scope):
        obj = self.eval(e[1], scope)
        if e[3] and (obj is None or obj is UNDEF):
            return UNDEF
        key = self.eval(e[2], scope)
        if isinstance(key, Rt):
            raise Unmodeled('computed-value-expression')
        if isinstance(key, float):
            if isinstance(obj, (JArr, str)):
                i = key
                if i != int(i) or i < 0 or i >= len(obj):
                    return UNDEF
                if isinstance(obj, str) and any(ord(c) > 0xFFFF for c in obj):
                    raise Unmodeled('utf16-string-index')
                return obj[int(i)]
            key = js_num_str(key)
        return self.get(obj, to_str(key), numeric=isinstance(e[2], tuple) and False)

    def e_new(self, e, scope):
        name = e[1][1] if e[1][0] == 'id' else None
        args = self.eval_args(e[2], scope)
        if name == 'Array':
            n = int(to_num(args[0])) if len(args) == 1 and isinstance(args[0], float) else None
            return JArr([UNDEF] * n) if n is not None else JArr(args)
        if name == 'Object':
            return JObj()
        if name in ('Error', 'TypeError', 'RangeError'):
            return JObj(message=to_str(args[0]) if args else '', name=name)
        if name == 'Set' and (not args or isinstance(args[0], JArr)):
            raise Unmodeled('js-set')
        if name == 'Map':
            raise Unmodeled('js-map')
        raise Unmodeled('js-new-' + (name or 'expression'))

    def eval_args(self, args, scope):
        out = []
        for a in args:
            if a[0] == 'spread':
                v = self.eval(a[1], scope)
                if isinstance(v, (JArr, str)):
                    out.extend(list(v))
                else:
                    raise Unmodeled('js-spread')
            else:
                out.append(self.eval(a, scope))
        return out

    def e_call(self, e, scope):
        callee_node = e[1]
        if callee_node[0] in ('mem', 'idx'):
            obj = self.eval(callee_node[1], scope)
            if callee_node[3] and (obj is None or obj is UNDEF):
                return UNDEF
            key = callee_node[2] if callee_node[0] == 'mem' else to_str(self.eval(callee_node[2], scope))
            fn = self.get(obj, key, for_call=True)
            if e[3] and (fn is None or fn is UNDEF):
                return UNDEF
            args = self.eval_args(e[2], scope)
            if isinstance(fn, BoundMethod):
                return fn.call(self, args)
            return self.call(fn, obj, args)
        fn = self.eval(callee_node, scope)
        if e[3] and (fn is None or fn is UNDEF):
            return UNDEF
        args = self.eval_args(e[2], scope)
        return self.call(fn, UNDEF, args)

    # ---- property access ----
    def get(self, obj, key, for_call=False, numeric=False):
        if isinstance(obj, Rt):
            raise Unmodeled('computed-value-expression')
        if obj is None or obj is UNDEF:
            raise JSThrow('Cannot read properties of %s (reading %s)' % (to_str(obj), key), 'TypeError')
        if isinstance(obj, Host):
            return self.host_get(obj, key, for_call)
        if isinstance(obj, str):
            return get_string_member(self, obj, key, for_call)
        if isinstance(obj, JArr):
            return get_array_member(self, obj, key, for_call)
        if isinstance(obj, JObj):
            if key in obj:
                return obj[key]
            if for_call:
                return get_object_member(self, obj, key)
            return UNDEF
        if isinstance(obj, float):
            return get_number_member(self, obj, key, for_call)
        if isinstance(obj, bool):
            raise Unmodeled('js-boolean-member')
        if isinstance(obj, Func):
            if key in obj.props:
                return obj.props[key]
            if key == 'name':
                return obj.name or ''
            if key == 'call':
                return BoundMethod(lambda i, args, f=obj: i.call(f, args[0] if args else UNDEF, args[1:]))
            if key == 'bind':
                raise Unmodeled('js-bind')
            return UNDEF
        if isinstance(obj, Regex):
            if key == 'test':
                return BoundMethod(lambda i, args, r=obj: bool(r.re.search(to_str(args[0]))))
            raise Unmodeled('js-regexp')
        if isinstance(obj, Native):
            if obj.name == 'Object':
                return object_static(self, key)
            if obj.name == 'Array':
                return array_static(self, key)
            if obj.name == 'JSON':
                raise Unmodeled('js-json')
            if obj.name == 'Number':
                return number_static(self, key)
            if obj.name == 'String':
                return string_static(self, key)
            return UNDEF
        raise Unmodeled('js-member')

    def host_get(self, h, key, for_call):
        from_host = HOST_GET.get(type(h))
        if from_host is None:
            raise Unmodeled('host-member')
        return from_host(self, h, key, for_call)

    def host_set(self, h, key, value):
        setter = HOST_SET.get(type(h))
        if setter is None:
            raise Unmodeled('host-assign')
        setter(self, h, key, value)


class BoundMethod:
    def __init__(self, fn):
        self.fn = fn

    def call(self, interp, args):
        return self.fn(interp, args)


def to_int32(x):
    if x != x or x in (math.inf, -math.inf):
        return 0
    n = int(x) & 0xFFFFFFFF
    return n - (1 << 32) if n >= (1 << 31) else n


def pattern_names(p):
    if p is None:
        return []
    k = p[0]
    if k == 'pid':
        return [p[1]]
    if k == 'pdef':
        return pattern_names(p[1])
    if k == 'prest':
        return pattern_names(p[1])
    if k == 'parr':
        return [n for x in p[1] for n in pattern_names(x)]
    if k == 'pobj':
        return [n for pr in p[1] for n in pattern_names(pr[2])]
    return []


# ---- built-in members of the JavaScript values --------------------------------------------------------------------------------------

def need_str(v):
    return to_str(v)


def get_string_member(interp, s, key, for_call):
    if key == 'length':
        return float(len(s) + sum(1 for c in s if ord(c) > 0xFFFF))
    if not for_call:
        raise Unmodeled('js-string-member')
    if any(ord(c) > 0xFFFF for c in s) and key in ('slice', 'substring', 'substr', 'charAt', 'charCodeAt', 'indexOf', 'padStart', 'padEnd', 'split'):
        raise Unmodeled('utf16-string-index')

    def clamp(v, n, default):
        if v is UNDEF:
            return default
        x = to_num(v)
        if x != x:
            return 0
        x = int(x) if x not in (math.inf, -math.inf) else (n if x > 0 else -n)
        if x < 0:
            x = max(0, n + x)
        return min(x, n)
    table = {
        'toUpperCase': lambda a: s.upper(),
        'toLowerCase': lambda a: s.lower(),
        'trim': lambda a: s.strip(' \t\n\r\f\v ﻿'),
        'trimStart': lambda a: s.lstrip(' \t\n\r\f\v ﻿'),
        'trimEnd': lambda a: s.rstrip(' \t\n\r\f\v ﻿'),
        'slice': lambda a: s[clamp(a[0] if a else UNDEF, len(s), 0):clamp(a[1] if len(a) > 1 else UNDEF, len(s), len(s))],
        'substring': lambda a: substring(s, a),
        'substr': lambda a: substr(s, a),
        'charAt': lambda a: s[int(to_num(a[0]))] if a and 0 <= to_num(a[0]) < len(s) else '',
        'charCodeAt': lambda a: float(ord(s[int(to_num(a[0]))])) if a and 0 <= to_num(a[0]) < len(s) else math.nan,
        'indexOf': lambda a: float(s.find(to_str(a[0]), int(to_num(a[1])) if len(a) > 1 else 0)),
        'lastIndexOf': lambda a: float(s.rfind(to_str(a[0]))),
        'includes': lambda a: to_str(a[0]) in s,
        'startsWith': lambda a: s.startswith(to_str(a[0]), int(to_num(a[1])) if len(a) > 1 else 0),
        'endsWith': lambda a: s.endswith(to_str(a[0])),
        'repeat': lambda a: s * int(to_num(a[0])) if 0 <= to_num(a[0]) < 1e5 and len(s) * int(to_num(a[0])) < MAX_STRING else raise_(Unmodeled('string-limit')),
        'concat': lambda a: s + ''.join(to_str(x) for x in a),
        'toString': lambda a: s,
        'valueOf': lambda a: s,
        'padStart': lambda a: pad(s, a, True),
        'padEnd': lambda a: pad(s, a, False),
        'split': lambda a: split_string(s, a),
        'replace': lambda a: replace_string(interp, s, a, False),
        'replaceAll': lambda a: replace_string(interp, s, a, True),
        'localeCompare': lambda a: float((s > to_str(a[0])) - (s < to_str(a[0]))),
        'match': lambda a: raise_(Unmodeled('js-regexp')),
        'normalize': lambda a: raise_(Unmodeled('js-normalize')),
        'at': lambda a: s[int(to_num(a[0]))] if a and -len(s) <= to_num(a[0]) < len(s) else UNDEF,
    }
    f = table.get(key)
    if f is None:
        raise Unmodeled('js-string-method')
    return BoundMethod(lambda i, args, f=f: f(args))


def raise_(e):
    raise e


def substring(s, a):
    n = len(s)

    def cl(v, d):
        if v is UNDEF:
            return d
        x = to_num(v)
        if x != x:
            return 0
        return int(min(max(x, 0), n)) if x not in (math.inf, -math.inf) else (n if x > 0 else 0)
    x, y = cl(a[0] if a else UNDEF, 0), cl(a[1] if len(a) > 1 else UNDEF, n)
    if x > y:
        x, y = y, x
    return s[x:y]


def substr(s, a):
    n = len(s)
    start = int(to_num(a[0])) if a else 0
    if start < 0:
        start = max(0, n + start)
    ln = int(to_num(a[1])) if len(a) > 1 and a[1] is not UNDEF else n - start
    return s[start:start + max(0, ln)]


def pad(s, a, start):
    n = int(to_num(a[0])) if a else 0
    fill = to_str(a[1]) if len(a) > 1 and a[1] is not UNDEF else ' '
    if n <= len(s) or fill == '':
        return s
    if n > MAX_STRING:
        raise Unmodeled('string-limit')
    need = n - len(s)
    p = (fill * (need // len(fill) + 1))[:need]
    return p + s if start else s + p


def split_string(s, a):
    if not a or a[0] is UNDEF:
        return JArr([s])
    if isinstance(a[0], Regex):
        raise Unmodeled('js-regexp')
    sep = to_str(a[0])
    parts = list(s) if sep == '' else s.split(sep)
    if len(a) > 1 and a[1] is not UNDEF:
        parts = parts[:int(to_num(a[1]))]
    return JArr(parts)


def replace_string(interp, s, a, everything):
    if len(a) < 2:
        raise Unmodeled('js-string-method')
    pat, rep = a[0], a[1]
    if isinstance(rep, (Func, Native)):
        raise Unmodeled('js-replace-function')
    rep = to_str(rep)
    if re.search(r'\$[&1-9$`\']', rep):
        raise Unmodeled('js-replace-pattern')
    if isinstance(pat, Regex):
        count = 0 if pat.glob else 1
        return pat.re.sub(lambda m: rep, s, count=count)
    pat = to_str(pat)
    return s.replace(pat, rep) if everything else s.replace(pat, rep, 1)


def get_number_member(interp, x, key, for_call):
    if not for_call:
        raise Unmodeled('js-number-member')
    if key == 'toString':
        return BoundMethod(lambda i, a: js_num_str(x) if not a or a[0] is UNDEF or to_num(a[0]) == 10 else raise_(Unmodeled('js-number-radix')))
    if key == 'toFixed':
        def tf(i, a):
            d = int(to_num(a[0])) if a else 0
            if x != x or abs(x) >= 1e21:
                return js_num_str(x)
            from decimal import Decimal, ROUND_HALF_UP
            q = Decimal(x).quantize(Decimal(1).scaleb(-d), rounding=ROUND_HALF_UP)
            s = format(q, 'f')
            return '-' + s[1:] if s.startswith('-') and float(q) == 0 and x >= 0 else s
        return BoundMethod(tf)
    raise Unmodeled('js-number-method')


def get_object_member(interp, o, key):
    if key == 'hasOwnProperty':
        return BoundMethod(lambda i, a: to_str(a[0]) in o)
    if key == 'toString':
        return BoundMethod(lambda i, a: '[object Object]')
    return UNDEF


def get_array_member(interp, arr, key, for_call):
    if key == 'length':
        return float(len(arr))
    if re.fullmatch(r'\d+', key):
        i = int(key)
        return arr[i] if i < len(arr) else UNDEF
    if not for_call:
        raise Unmodeled('js-array-member')

    def callback(a, n):
        if not a or not isinstance(a[0], (Func, Native)):
            raise Unmodeled('js-array-callback')
        return a[0]

    def each(a, interp_, fn_use):
        fn = callback(a, 0)
        out = []
        for i, v in enumerate(list(arr)):
            interp_.tick()
            out.append(fn_use(interp_.call(fn, UNDEF, [v, float(i), arr]), v, i))
        return out
    table = {
        'push': lambda i, a: (arr.extend(a), float(len(arr)))[1],
        'pop': lambda i, a: arr.pop() if arr else UNDEF,
        'shift': lambda i, a: arr.pop(0) if arr else UNDEF,
        'unshift': lambda i, a: (arr.__setitem__(slice(0, 0), a), float(len(arr)))[1],
        'join': lambda i, a: (to_str(a[0]) if a and a[0] is not UNDEF else ',').join('' if x is UNDEF or x is None else to_str(x) for x in arr),
        'toString': lambda i, a: to_str(arr),
        'slice': lambda i, a: JArr(arr[arr_idx(a, 0, len(arr), 0):arr_idx(a, 1, len(arr), len(arr))]),
        'concat': lambda i, a: JArr(list(arr) + [y for x in a for y in (x if isinstance(x, JArr) else [x])]),
        'includes': lambda i, a: any(same_value_zero(x, a[0]) for x in arr) if a else False,
        'indexOf': lambda i, a: next((float(j) for j, x in enumerate(arr) if a and strict_eq(x, a[0])), -1.0),
        'reverse': lambda i, a: (arr.reverse(), arr)[1],
        'forEach': lambda i, a: (each(a, i, lambda r, v, k: None), UNDEF)[1],
        'map': lambda i, a: JArr(each(a, i, lambda r, v, k: r)),
        'filter': lambda i, a: JArr([v for r, v in zip(each(a, i, lambda r, v, k: truthy(r)), list(arr)) if r]),
        'some': lambda i, a: any(each(a, i, lambda r, v, k: truthy(r))),
        'every': lambda i, a: all(each(a, i, lambda r, v, k: truthy(r))),
        'find': lambda i, a: next((v for r, v in zip(each(a, i, lambda r, v, k: truthy(r)), list(arr)) if r), UNDEF),
        'findIndex': lambda i, a: next((float(k) for k, r in enumerate(each(a, i, lambda r, v, k: truthy(r))) if r), -1.0),
        'flat': lambda i, a: JArr([y for x in arr for y in (x if isinstance(x, JArr) else [x])]),
        'flatMap': lambda i, a: JArr([y for r in each(a, i, lambda r, v, k: r) for y in (r if isinstance(r, JArr) else [r])]),
        'entries': lambda i, a: JObj(__entries__=[JArr([float(k), v]) for k, v in enumerate(arr)]),
        'reduce': lambda i, a: reduce_array(i, arr, a),
        'sort': lambda i, a: sort_array(i, arr, a),
        'at': lambda i, a: (arr[int(to_num(a[0]))] if -len(arr) <= to_num(a[0]) < len(arr) else UNDEF) if a else UNDEF,
        'fill': lambda i, a: (arr.__setitem__(slice(0, len(arr)), [a[0]] * len(arr)), arr)[1],
        'splice': lambda i, a: splice_array(arr, a),
    }
    f = table.get(key)
    if f is None:
        raise Unmodeled('js-array-method')
    return BoundMethod(f)


def same_value_zero(a, b):
    if isinstance(a, float) and isinstance(b, float) and a != a and b != b:
        return True
    return strict_eq(a, b)


def arr_idx(a, k, n, default):
    if len(a) <= k or a[k] is UNDEF:
        return default
    x = to_num(a[k])
    if x != x:
        return 0
    x = int(x) if x not in (math.inf, -math.inf) else (n if x > 0 else -n)
    if x < 0:
        x = max(0, n + x)
    return min(x, n)


def reduce_array(interp, arr, a):
    if not a or not isinstance(a[0], (Func, Native)):
        raise Unmodeled('js-array-callback')
    items = list(arr)
    if len(a) > 1:
        acc = a[1]
    elif items:
        acc = items.pop(0)
    else:
        raise JSThrow('Reduce of empty array with no initial value', 'TypeError')
    for i, v in enumerate(items):
        interp.tick()
        acc = interp.call(a[0], UNDEF, [acc, v, float(i), arr])
    return acc


def sort_array(interp, arr, a):
    import functools
    if a and isinstance(a[0], (Func, Native)):
        fn = a[0]

        def cmp(x, y):
            r = to_num(interp.call(fn, UNDEF, [x, y]))
            return -1 if r < 0 else (1 if r > 0 else 0)
        arr.sort(key=functools.cmp_to_key(cmp))
    else:
        arr.sort(key=lambda x: to_str(x))
    return arr


def splice_array(arr, a):
    start = arr_idx(a, 0, len(arr), 0)
    cnt = len(arr) - start if len(a) < 2 else max(0, int(to_num(a[1])))
    removed = arr[start:start + cnt]
    arr[start:start + cnt] = a[2:]
    return JArr(removed)


def object_static(interp, key):
    def keys(i, a):
        o = a[0]
        if isinstance(o, JObj):
            return JArr(list(o.keys()))
        if isinstance(o, JArr):
            return JArr([str(k) for k in range(len(o))])
        raise Unmodeled('js-object-keys')
    table = {
        'keys': keys,
        'values': lambda i, a: JArr(list(a[0].values())) if isinstance(a[0], JObj) else raise_(Unmodeled('js-object-values')),
        'entries': lambda i, a: JArr([JArr([k, v]) for k, v in a[0].items()]) if isinstance(a[0], JObj) else raise_(Unmodeled('js-object-entries')),
        'assign': lambda i, a: (a[0].update(*[x for x in a[1:] if isinstance(x, JObj)]), a[0])[1] if isinstance(a[0], JObj) else raise_(Unmodeled('js-object-assign')),
        'freeze': lambda i, a: a[0],
        'fromEntries': lambda i, a: JObj({to_str(p[0]): p[1] for p in a[0]}) if isinstance(a[0], JArr) else raise_(Unmodeled('js-object-fromentries')),
    }
    if key not in table:
        raise Unmodeled('js-object-static')
    return BoundMethod(table[key])


def array_static(interp, key):
    if key == 'isArray':
        return BoundMethod(lambda i, a: isinstance(a[0], JArr) if a else False)
    if key == 'of':
        return BoundMethod(lambda i, a: JArr(a))
    if key == 'from':
        def frm(i, a):
            src = a[0]
            if isinstance(src, (JArr, str)):
                items = list(src)
            elif isinstance(src, JObj) and isinstance(src.get('length'), float):
                items = [src.get(str(k), UNDEF) for k in range(int(src['length']))]
            else:
                raise Unmodeled('js-array-from')
            if len(a) > 1:
                items = [i.call(a[1], UNDEF, [v, float(k)]) for k, v in enumerate(items)]
            return JArr(items)
        return BoundMethod(frm)
    raise Unmodeled('js-array-static')


def number_static(interp, key):
    if key == 'isInteger':
        return BoundMethod(lambda i, a: isinstance(a[0], float) and a[0] == a[0] and a[0] not in (math.inf, -math.inf) and a[0] == int(a[0]))
    if key == 'isNaN':
        return BoundMethod(lambda i, a: isinstance(a[0], float) and a[0] != a[0])
    if key == 'parseFloat':
        return BoundMethod(lambda i, a: interp.n_parseFloat(i, None, a))
    if key == 'parseInt':
        return BoundMethod(lambda i, a: interp.n_parseInt(i, None, a))
    if key == 'EPSILON':
        return 2.220446049250313e-16
    raise Unmodeled('js-number-static')


def string_static(interp, key):
    if key == 'fromCharCode':
        return BoundMethod(lambda i, a: ''.join(chr(int(to_num(x))) for x in a))
    raise Unmodeled('js-string-static')


MATH = {
    'floor': lambda x: math.floor(x) if x == x and x not in (math.inf, -math.inf) else x,
    'ceil': lambda x: math.ceil(x) if x == x and x not in (math.inf, -math.inf) else x,
    'round': lambda x: math.floor(x + 0.5) if x == x and x not in (math.inf, -math.inf) else x,
    'abs': abs,
    'trunc': lambda x: math.trunc(x) if x == x and x not in (math.inf, -math.inf) else x,
    'sqrt': lambda x: math.sqrt(x) if x >= 0 else math.nan,
    'sign': lambda x: (x > 0) - (x < 0) if x == x else math.nan,
}


# ---- the host objects ----------------------------------------------------------------------------------------------------------------

SPECIAL_ELEM_PROPS = {
    'body': 'body', 'documentElement': 'html', 'head': 'head',
}
ELEM_NAV = {'parentElement': 'parentElement', 'parentNode': 'parentElement', 'firstElementChild': 'firstElementChild', 'lastElementChild': 'lastElementChild',
            'nextElementSibling': 'nextElementSibling', 'previousElementSibling': 'previousElementSibling'}


def doc_get(interp, d, key, for_call):
    if key in SPECIAL_ELEM_PROPS:
        interp.need_dom()
        n = interp.new_node('e')
        interp.emit('elem', n, SPECIAL_ELEM_PROPS[key], '')
        return HostElem(n)
    if key == 'getElementById':
        def f(i, a):
            i.need_dom()
            if not a or not isinstance(a[0], str):
                raise Unmodeled('host-getElementById-argument')
            n = i.new_node('e')
            i.emit('elem', n, 'byid', a[0])
            return HostElem(n)
        return BoundMethod(f)
    if key == 'querySelector':
        return BoundMethod(lambda i, a: query(i, 'doc', a))
    if key == 'querySelectorAll':
        return BoundMethod(lambda i, a: query_all(i, 'doc', a))
    if key == 'defaultView':
        return interp.glob.vars['window']
    raise Unmodeled('dom-document-' + key)


def query(interp, root, a):
    interp.need_dom()
    if not a or not isinstance(a[0], str):
        raise Unmodeled('host-querySelector-argument')
    n = interp.new_node('e')
    interp.emit('elem', n, 'sel', root, a[0])
    return HostElem(n)


def query_all(interp, root, a):
    interp.need_dom()
    if not a or not isinstance(a[0], str):
        raise Unmodeled('host-querySelectorAll-argument')
    return HostList(root, a[0])


def list_get(interp, lst, key, for_call):
    if re.fullmatch(r'\d+', key):
        n = interp.new_node('e')
        interp.emit('elem', n, 'qsa', lst.root, lst.selector, key)
        return HostElem(n)
    if key == 'item':
        def f(i, a):
            if not a or not isinstance(a[0], float) or a[0] != int(a[0]) or a[0] < 0:
                raise Unmodeled('host-nodelist-item')
            return list_get(i, lst, str(int(a[0])), False)
        return BoundMethod(f)
    raise Unmodeled('dom-nodelist-' + key)


def window_get(interp, w, key, for_call):
    g = interp.glob.vars
    if key in ('document', 'getComputedStyle', 'window', 'self', 'parent', 'top'):
        if key in ('parent', 'top'):
            return w
        return g[key]
    if key in g:
        return g[key]
    raise Unmodeled('host-window-' + key)


def window_set(interp, w, key, value):
    raise Unmodeled('host-window-assign')


def test_get(interp, t, key, for_call):
    if key == 'add_cleanup':
        def f(i, a):
            if not a or not isinstance(a[0], Func):
                raise Unmodeled('harness-add-cleanup')
            i.cur.cleanups.append(a[0])
            return UNDEF
        return BoundMethod(f)
    if key == 'step':
        def st(i, a):
            if not a or not isinstance(a[0], Func):
                raise Unmodeled('harness-step')
            return i.call(a[0], t, a[1:])
        return BoundMethod(st)
    if key == 'name':
        return interp.cur.name
    raise Unmodeled('harness-test-' + key)


def math_get(interp, m, key, for_call):
    if key in MATH:
        def f(i, a):
            return float(MATH[key](to_num(a[0]) if a else math.nan))
        return BoundMethod(f)
    if key in ('max', 'min'):
        def mm(i, a):
            xs = [to_num(x) for x in a]
            if not xs:
                return -math.inf if key == 'max' else math.inf
            if any(x != x for x in xs):
                return math.nan
            return max(xs) if key == 'max' else min(xs)
        return BoundMethod(mm)
    if key == 'PI':
        return math.pi
    if key == 'pow':
        return BoundMethod(lambda i, a: i.e_bin(('bin', '**', ('val', a[0]), ('val', a[1])), None))
    raise Unmodeled('js-math-' + key)


def elem_get(interp, el, key, for_call):
    if key in ELEM_NAV:
        n = interp.new_node('e')
        interp.emit('elem', n, 'nav', el.name, ELEM_NAV[key])
        return HostElem(n)
    if key == 'style':
        s = interp.new_node('s')
        interp.emit('style', s, el.name)
        return HostStyle(s, False)
    if key == 'classList':
        return HostClassList(el.name)
    if key == 'getAttribute':
        def f(i, a):
            r = i.new_reg()
            if not a or not isinstance(a[0], str):
                raise Unmodeled('host-getAttribute-argument')
            i.emit('getattr', r, el.name, a[0])
            return Rt(r, 'sn')
        return BoundMethod(f)
    if key == 'hasAttribute':
        def f(i, a):
            r = i.new_reg()
            if not a or not isinstance(a[0], str):
                raise Unmodeled('host-hasAttribute-argument')
            i.emit('hasattr', r, el.name, a[0])
            return Rt(r, 'b')
        return BoundMethod(f)
    if key == 'setAttribute':
        def f(i, a):
            if len(a) < 2 or not isinstance(a[0], str):
                raise Unmodeled('host-setAttribute-argument')
            i.emit('setattr', el.name, a[0], to_str(a[1]))
            return UNDEF
        return BoundMethod(f)
    if key == 'removeAttribute':
        def f(i, a):
            if not a or not isinstance(a[0], str):
                raise Unmodeled('host-removeAttribute-argument')
            i.emit('rmattr', el.name, a[0])
            return UNDEF
        return BoundMethod(f)
    if key == 'querySelector':
        return BoundMethod(lambda i, a: query(i, el.name, a))
    if key == 'querySelectorAll':
        return BoundMethod(lambda i, a: query_all(i, el.name, a))
    if key in ('id', 'className'):
        r = interp.new_reg()
        interp.emit('getattr', r, el.name, 'id' if key == 'id' else 'class')
        return Rt(r, 'sn')
    raise Unmodeled('dom-element-' + key)


def css_get(interp, c, key, for_call):
    if key == 'supports':
        def f(i, a):
            if not a:
                raise Unmodeled('host-CSS-supports-argument')   # a TypeError of the page
            r = i.new_reg()
            args = [to_str(x) for x in a[:2]]
            i.emit('csssupports', r, *args)
            return Rt(r, 'b')
        return BoundMethod(f)
    if key == 'escape':
        def f(i, a):
            if not a:
                raise Unmodeled('host-CSS-escape-argument')
            r = i.new_reg()
            i.emit('cssescape', r, to_str(a[0]))
            return Rt(r, 's')
        return BoundMethod(f)
    raise Unmodeled('host-CSS-' + key)


def elem_set(interp, el, key, value):
    if key == 'style':
        # [PutForwards=cssText]: the assignment is el.style.cssText = value
        s = interp.new_node('s')
        interp.emit('style', s, el.name)
        interp.emit('scsstext', s, null_empty(value))
        return
    if key in ('id', 'className'):
        interp.emit('setattr', el.name, 'id' if key == 'id' else 'class', to_str(value))
        return
    raise Unmodeled('dom-element-assign-' + key)


def classlist_get(interp, cl, key, for_call):
    if key in ('add', 'remove'):
        def f(i, a):
            if not a or any(not isinstance(x, str) for x in a):
                raise Unmodeled('host-classList-argument')
            i.emit('class' + key, cl.name, *a)
            return UNDEF
        return BoundMethod(f)
    if key == 'toggle':
        def t(i, a):
            if len(a) != 1 or not isinstance(a[0], str):
                raise Unmodeled('host-classList-argument')
            r = i.new_reg()
            i.emit('classtoggle', r, cl.name, a[0])
            return Rt(r, 'b')
        return BoundMethod(t)
    if key == 'contains':
        def c(i, a):
            r = i.new_reg()
            i.emit('classhas', r, cl.name, a[0] if a and isinstance(a[0], str) else raise_(Unmodeled('host-classList-argument')))
            return Rt(r, 'b')
        return BoundMethod(c)
    raise Unmodeled('dom-classList-' + key)


def style_get(interp, st, key, for_call):
    prefix = 'c' if st.computed else ''
    if key == 'getPropertyValue':
        def f(i, a):
            if not a or not isinstance(a[0], str):
                raise Unmodeled('host-getPropertyValue-argument')
            r = i.new_reg()
            i.emit('sget', r, st.name, a[0])
            return Rt(r, 's')
        return BoundMethod(f)
    if key == 'getPropertyPriority':
        def f(i, a):
            if not a or not isinstance(a[0], str):
                raise Unmodeled('host-getPropertyPriority-argument')
            r = i.new_reg()
            i.emit('sprio', r, st.name, a[0])
            return Rt(r, 's')
        return BoundMethod(f)
    if key == 'setProperty':
        def f(i, a):
            if len(a) < 2 or not isinstance(a[0], str):
                raise Unmodeled('host-setProperty-argument')
            i.emit('sset', st.name, a[0], null_empty(a[1]), null_empty(a[2]) if len(a) > 2 and a[2] is not UNDEF else '')
            return UNDEF
        return BoundMethod(f)
    if key == 'removeProperty':
        def f(i, a):
            if not a or not isinstance(a[0], str):
                raise Unmodeled('host-removeProperty-argument')
            r = i.new_reg()
            i.emit('sremove', r, st.name, a[0])
            return Rt(r, 's')
        return BoundMethod(f)
    if key == 'item':
        def f(i, a):
            if not a or not isinstance(a[0], float):
                raise Unmodeled('host-item-argument')
            r = i.new_reg()
            i.emit('sitem', r, st.name, js_num_str(a[0]))
            return Rt(r, 's')
        return BoundMethod(f)
    if key == 'length':
        r = interp.new_reg()
        interp.emit('slen', r, st.name)
        return Rt(r, 'n')
    if key == 'cssText':
        r = interp.new_reg()
        interp.emit('scss', r, st.name)
        return Rt(r, 's')
    if key in ('parentRule',):
        raise Unmodeled('cssom-parentRule')
    if re.fullmatch(r'\d+', key):
        raise Unmodeled('host-style-index')
    if for_call and key not in ('getPropertyValue',):
        raise Unmodeled('host-style-method-' + key)
    r = interp.new_reg()
    interp.emit('sattr', r, st.name, key)
    return Rt(r, 'any')


def null_empty(v):
    """[LegacyNullToEmptyString] CSSOMString: null is the empty string, everything else is converted as any string."""
    return '' if v is None else to_str(v)


def style_set(interp, st, key, value):
    if st.computed:
        # the computed declaration is read-only: the assignment is an error in strict mode, and a no-op otherwise; the runner decides by the library
        pass
    if key == 'cssText':
        interp.emit('scsstext', st.name, null_empty(value))
        return
    interp.emit('sattrset', st.name, key, null_empty(value))


HOST_GET = {HostDoc: doc_get, HostWindow: window_get, HostElem: elem_get, HostStyle: style_get, HostClassList: classlist_get, HostList: list_get, HostTest: test_get, HostMath: math_get, HostCSS: css_get}
HOST_SET = {HostWindow: window_set, HostElem: elem_set, HostStyle: style_set}


# ---- the page ------------------------------------------------------------------------------------------------------------------------

class PageScan(html.parser.HTMLParser):
    """The scripts of a page in document order, and what the markup says that the evaluator needs: its element ids, the stylesheets it links, whether it is in quirks mode."""

    def __init__(self):
        html.parser.HTMLParser.__init__(self, convert_charrefs=False)
        self.scripts = []        # (attrs dict, text, ids_after)
        self.ids = []            # all ids, in order
        self.id_positions = []   # (id, element ordinal)
        self.elements = 0
        self.script_marks = []   # element ordinal when each script starts
        self.cur = None
        self.helpers = []
        self.links = []
        self.doctype = False
        self.in_script = False
        self.templates = 0
        self.odd = []
        self.script_ends = []    # the offset just after each script's end tag (inline or not), in document order
        self.script_spans = []   # (start, end) of each script's text, in characters
        self.text_start = 0
        self.text = ''
        self.line_starts = [0]
        self.title = None        # the data of the first text node of the first <title>, as testharness.js reads it
        self.in_title = False
        self.title_buf = []

    def handle_decl(self, decl):
        if decl.lower().startswith('doctype'):
            self.doctype = True

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == 'script':
            self.script_marks.append(self.elements)
            self.cur = (a, [])
            self.in_script = True
            line, col = self.getpos()
            gt = self.text.find('>', self.line_starts[line - 1] + col)
            self.text_start = gt + 1 if gt >= 0 else len(self.text)
            return
        if tag == 'title' and self.title is None:
            self.in_title = True
            self.title_buf = []
        self.elements += 1
        if 'id' in a and a['id'] is not None:
            self.ids.append(a['id'])
            self.id_positions.append((a['id'], self.elements))
        if tag == 'link' and (a.get('rel') or '').lower() == 'stylesheet':
            self.links.append(a.get('href'))
        if tag == 'template':
            self.templates += 1
        if tag in ('iframe', 'object', 'embed', 'svg', 'math'):
            self.odd.append(tag)

    def handle_entityref(self, name):
        if self.in_title:
            self.title_buf.append('&%s;' % name)

    def handle_charref(self, name):
        if self.in_title:
            self.title_buf.append('&#%s;' % name)

    def handle_endtag(self, tag):
        if tag == 'title' and self.in_title:
            self.in_title = False
            self.title = html.unescape(''.join(self.title_buf))
        if tag == 'script' and self.in_script:
            self.scripts.append((self.cur[0], ''.join(self.cur[1]), self.script_marks[-1]))
            self.in_script = False
            line, col = self.getpos()
            at = self.line_starts[line - 1] + col
            gt = self.text.find('>', at)
            self.script_ends.append(gt + 1 if gt >= 0 else len(self.text))
            self.script_spans.append((self.text_start, at))   # the characters of the script's text, between its start tag and its end tag

    def handle_data(self, data):
        if self.in_script:
            self.cur[1].append(data)
        if self.in_title:
            self.title_buf.append(data)

    def unknown_decl(self, data):
        pass


CALL_SITE = re.compile(r'(?<![\w.$])(?:test|promise_test|async_test)\s*\(')
HARNESS_SRC = ('testharness.js', 'testharnessreport.js')


def evaluate_page(rel, text):
    """The PageResult for an HTML page, or None when the page cannot be read as a whole (see the module comment)."""
    scan = PageScan()
    try:
        scan.text = text
        for m in re.finditer('\n', text):
            scan.line_starts.append(m.end())
        scan.feed(text)
        scan.close()
    except Exception:
        return None
    srcs = [a.get('src') for a, _, _ in scan.scripts if a.get('src')]
    # `scripts` holds the inline ones in `scan.scripts`; src scripts show as empty text with a src attribute
    inline = [(a, t, mark) for a, t, mark in scan.scripts if not a.get('src')]
    for a, t, mark in scan.scripts:
        if a.get('src') and os.path.basename(a['src'].split('?')[0]) not in ('testharness.js', 'testharnessreport.js'):
            return None
        if a.get('type') and a['type'].lower() not in ('text/javascript', 'application/javascript', 'text/ecmascript', ''):
            return None
    if scan.links or scan.templates or scan.odd:
        return None
    res = PageResult()
    res.html = text
    res.ids = scan.ids
    res.quirks = not scan.doctype
    res.static_calls = sum(len(CALL_SITE.findall(t)) for _, t, _ in inline)
    it = Interp([])
    it.title = scan.title if scan.title else os.path.basename(rel).split('.')[0]   # testharness.js get_title(): the title's first text node, else the file's name up to its first dot
    for (attrs, source, mark), end in [(sc, e) for sc, e in zip(scan.scripts, scan.script_ends) if not sc[0].get('src')]:
        it.enter_script(len(text[:end].encode('utf-8', 'surrogatepass')), [i for i, pos in scan.id_positions if pos <= mark])
        try:
            prog = J.parse_program(source)
        except Exception:
            return None
        scope = it.glob
        try:
            it.hoist(prog, scope)
            for st in prog:
                it.exec(st, scope)
        except (Unmodeled, JSThrow, BreakSignal, ContinueSignal, ReturnSignal, RecursionError):
            return None   # a top-level statement the evaluator cannot run: the set of tests is not known
    res.ops = it.ops
    res.tests = it.tests
    return res


# ---- the flat program ----------------------------------------------------------------------------------------------------------------------

def esc_field(s):
    return s.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n').replace('\r', '\\r').replace('\0', '\\0')


def render_program(rel, res):
    """The text of a PageResult for the runner: ``P file quirks tests``, ``H html``, then the ops. The tests the evaluator could not model have no ops (their numbers are in the page's
    list); the runner reports a result for each test that has ops."""
    modeled = sum(1 for t in res.tests if t.reason is None)
    lines = ['P\t%s\t%d\t%d' % (esc_field(rel), 1 if res.quirks else 0, modeled), 'H\t' + esc_field(res.html)]
    for op in res.ops:
        lines.append('O\t' + '\t'.join(esc_field(f) for f in op))
    return ('\n'.join(lines) + '\n').encode('utf-8', 'surrogatepass')


# ---- the reasons ---------------------------------------------------------------------------------------------------------------------------

GEOMETRY = re.compile(r'offset(?:Width|Height|Left|Top|Parent)|client(?:Width|Height|Left|Top)|scroll(?:Width|Height|Top|Left)|getBoundingClientRect|getClientRects|focus|hover', re.I)


def reason_code(construct):
    """The code of REASON_CODES for a test the evaluator could not turn into ops: a residue code where the construct is one of those (geometry), else ``unmodeled:<construct>``."""
    if construct == 'shared-state':
        return 'unmodeled:shared-state'
    if construct == 'script-before-markup-end':
        return 'unmodeled:incremental-document'
    if GEOMETRY.search(construct):
        return 'layout-geometry'
    return 'unmodeled:' + construct
