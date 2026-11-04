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
"""The parser of the scoring interpreter's JavaScript subset (tools/css/cssom_eval.py evaluates what it parses).

    parse_program(source) -> [statement]

The grammar is the part of ECMAScript that the script pages of web-platform-tests' css/ use for their tests: declarations, functions and arrows, the statements `if`, `for`,
`for...of`, `for...in`, `while`, `do...while`, `try`, `throw`, `return`, `break` and `continue`, template literals, array and object literals with spread, shorthand, computed
keys and destructuring patterns, optional chaining, and the operators. A construct outside it is not a parse error: it is a node ``('unsup', construct)`` that raises
``Unmodeled(construct)`` when it is *run*, so that a page is read as far as it goes and the first test that needs the construct is the one that is listed. The tokens are those of
tools/css/extract-wpt-cssom.py (one tokenizer in the tree).

Expression nodes: num, str, tmpl, id, arr, obj, func, call, new, mem, idx, un, bin, log, cond, asg, upd, seq, spread, regex, this, unsup. Statement nodes: var, fdecl, ret, if, for,
forof, forin, while, dowhile, block, expr, break, continue, throw, try, empty, unsup. Patterns: pid, parr, pobj, pdef, prest (and mem or idx as an assignment target).
"""
import importlib.util
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))


def _load_tokenizer():
    spec = importlib.util.spec_from_file_location('extract_wpt_cssom_tokens', os.path.join(HERE, 'extract-wpt-cssom.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_X = _load_tokenizer()
Tok = _X.Tok
unescape_js = _X.unescape_js
tokenize_js = _X.tokenize


class JSParseError(Exception):
    """The source is not in the grammar at a place the parser cannot recover from."""

    def __init__(self, construct):
        Exception.__init__(self, construct)
        self.construct = construct


EOF = Tok('eof', '', None, True)

BINARY = [
    ['??'], ['||'], ['&&'], ['|'], ['^'], ['&'], ['==', '!=', '===', '!=='], ['<', '>', '<=', '>=', 'instanceof', 'in'], ['<<', '>>', '>>>'], ['+', '-'], ['*', '/', '%'], ['**'],
]
PREC = {}
for _i, _ops in enumerate(BINARY):
    for _op in _ops:
        PREC[_op] = _i + 1
ASSIGN = {'=', '+=', '-=', '*=', '/=', '%=', '**=', '<<=', '>>=', '>>>=', '&=', '|=', '^=', '&&=', '||=', '??='}
RESERVED_STMT = {'class', 'switch', 'with', 'import', 'export', 'debugger'}


def scan_template(raw):
    """(strings, expression sources) of a template literal's text (between the backticks)."""
    strings, exprs = [], []
    cur = []
    i = 0
    n = len(raw)
    while i < n:
        c = raw[i]
        if c == '\\':
            cur.append(raw[i:i + 2])
            i += 2
            continue
        if raw.startswith('${', i):
            depth = 1
            j = i + 2
            while j < n and depth:
                d = raw[j]
                if d in '"\'':
                    k = j + 1
                    while k < n and raw[k] != d:
                        k += 2 if raw[k] == '\\' else 1
                    j = k + 1
                    continue
                if d == '`':
                    k = j + 1
                    while k < n and raw[k] != '`':
                        k += 2 if raw[k] == '\\' else 1
                    j = k + 1
                    continue
                if d == '{':
                    depth += 1
                elif d == '}':
                    depth -= 1
                j += 1
            strings.append(unescape_js(''.join(cur)))
            cur = []
            exprs.append(raw[i + 2:j - 1])
            i = j
            continue
        cur.append(c)
        i += 1
    strings.append(unescape_js(''.join(cur)))
    return strings, exprs


class Parser:
    def __init__(self, toks):
        self.toks = toks
        self.i = 0

    # ---- token helpers ----
    def peek(self, k=0):
        j = self.i + k
        return self.toks[j] if j < len(self.toks) else EOF

    def next(self):
        t = self.peek()
        self.i += 1
        return t

    def at(self, text, k=0):
        t = self.peek(k)
        return t.text == text and t.kind in ('punct', 'ident')

    def eat(self, text):
        if self.at(text):
            self.i += 1
            return True
        return False

    def expect(self, text):
        if not self.eat(text):
            raise JSParseError('syntax:expected-' + text)

    def done(self):
        return self.i >= len(self.toks)

    # ---- statements ----
    def program(self):
        out = []
        while not self.done():
            out.append(self.statement())
        return out

    def skip_balanced(self):
        """Skip a balanced group starting at the current open token."""
        depth = 0
        while not self.done():
            t = self.next()
            if t.kind == 'punct' and t.text in '([{':
                depth += 1
            elif t.kind == 'punct' and t.text in ')]}':
                depth -= 1
                if depth == 0:
                    return

    def skip_statement(self):
        """Skip to the end of a statement we do not parse: through balanced groups to a `;` at depth 0, or after a block that ends the statement."""
        depth = 0
        while not self.done():
            t = self.peek()
            if depth == 0 and t.kind == 'punct' and t.text == ';':
                self.i += 1
                return
            if depth == 0 and t.kind == 'punct' and t.text == '}':
                return
            self.i += 1
            if t.kind == 'punct' and t.text in '([{':
                depth += 1
            elif t.kind == 'punct' and t.text in ')]}':
                depth -= 1
                if depth == 0 and t.text == '}' and not (self.at('else') or self.at('catch') or self.at('finally') or self.at(')') or self.at('.') or self.at(',')):
                    nxt = self.peek()
                    if nxt.nl or nxt.kind == 'eof' or nxt.text == ';':
                        if nxt.text == ';':
                            self.i += 1
                        return

    def end_statement(self):
        if self.eat(';'):
            return
        t = self.peek()
        if self.done() or t.nl or self.at('}'):
            return
        raise JSParseError('syntax:statement-end')

    def block(self):
        self.expect('{')
        body = []
        while not self.at('}'):
            if self.done():
                raise JSParseError('syntax:unclosed-block')
            body.append(self.statement())
        self.expect('}')
        return ('block', body)

    def statement(self):
        t = self.peek()
        if t.kind == 'punct':
            if t.text == '{':
                return self.block()
            if t.text == ';':
                self.i += 1
                return ('empty',)
        if t.kind == 'ident':
            w = t.text
            if w in RESERVED_STMT:
                self.skip_statement()
                return ('unsup', 'js-' + w)
            if w in ('var', 'let', 'const') and (self.peek(1).kind == 'ident' or self.peek(1).text in ('[', '{')):
                node = self.var_decl()
                self.end_statement()
                return node
            if w == 'function' or (w == 'async' and self.peek(1).text == 'function'):
                f = self.function_expr()
                return ('fdecl', f[5], f)
            if w == 'return':
                self.i += 1
                if self.at(';') or self.at('}') or self.peek().nl or self.done():
                    self.eat(';')
                    return ('ret', None)
                e = self.expression()
                self.end_statement()
                return ('ret', e)
            if w == 'if':
                self.i += 1
                self.expect('(')
                c = self.expression()
                self.expect(')')
                a = self.statement()
                b = None
                if self.eat('else'):
                    b = self.statement()
                return ('if', c, a, b)
            if w == 'for':
                return self.for_statement()
            if w == 'while':
                self.i += 1
                self.expect('(')
                c = self.expression()
                self.expect(')')
                return ('while', c, self.statement())
            if w == 'do':
                self.i += 1
                body = self.statement()
                self.expect('while')
                self.expect('(')
                c = self.expression()
                self.expect(')')
                self.eat(';')
                return ('dowhile', body, c)
            if w in ('break', 'continue'):
                self.i += 1
                if self.peek().kind == 'ident' and not self.peek().nl:
                    self.skip_statement()
                    return ('unsup', 'js-label')
                self.end_statement()
                return (w,)
            if w == 'throw':
                self.i += 1
                e = self.expression()
                self.end_statement()
                return ('throw', e)
            if w == 'try':
                self.i += 1
                blk = self.block()
                param, handler, fin = None, None, None
                if self.eat('catch'):
                    if self.eat('('):
                        param = self.binding_target()
                        self.expect(')')
                    handler = self.block()
                if self.eat('finally'):
                    fin = self.block()
                return ('try', blk, param, handler, fin)
            if self.peek(1).text == ':' and self.peek(1).kind == 'punct' and w not in ('default', 'case'):
                self.skip_statement()
                return ('unsup', 'js-label')
        e = self.expression()
        self.end_statement()
        return ('expr', e)

    def var_decl(self):
        kind = self.next().text
        decls = []
        while True:
            target = self.binding_target()
            init = None
            if self.eat('='):
                init = self.assignment()
            decls.append((target, init))
            if not self.eat(','):
                break
        return ('var', kind, decls)

    def for_statement(self):
        self.i += 1
        self.expect('(')
        init = None
        if self.at(';'):
            pass
        elif self.peek().text in ('var', 'let', 'const') and self.peek().kind == 'ident':
            kind = self.next().text
            target = self.binding_target()
            if self.at('of') or self.at('in'):
                which = self.next().text
                e = self.assignment() if which == 'of' else self.expression()
                self.expect(')')
                return ('forof' if which == 'of' else 'forin', kind, target, e, self.statement())
            decls = []
            ini = None
            if self.eat('='):
                ini = self.assignment_noin()
            decls.append((target, ini))
            while self.eat(','):
                t2 = self.binding_target()
                i2 = self.assignment_noin() if self.eat('=') else None
                decls.append((t2, i2))
            init = ('var', kind, decls)
        else:
            e = self.expression_noin()
            if self.at('of') or self.at('in'):
                which = self.next().text
                e2 = self.assignment() if which == 'of' else self.expression()
                self.expect(')')
                return ('forof' if which == 'of' else 'forin', None, self.to_pattern(e), e2, self.statement())
            init = ('expr', e)
        self.expect(';')
        test = None if self.at(';') else self.expression()
        self.expect(';')
        update = None if self.at(')') else self.expression()
        self.expect(')')
        return ('for', init, test, update, self.statement())

    # `in` inside a for-init is the loop's keyword: parse with it disabled
    no_in = False

    def assignment_noin(self):
        old, self.no_in = self.no_in, True
        try:
            return self.assignment()
        finally:
            self.no_in = old

    def expression_noin(self):
        old, self.no_in = self.no_in, True
        try:
            return self.expression()
        finally:
            self.no_in = old

    # ---- patterns ----
    def binding_target(self):
        t = self.peek()
        if t.kind == 'ident':
            self.i += 1
            return ('pid', t.text)
        if t.text == '[':
            self.i += 1
            elems = []
            while not self.at(']'):
                if self.at(','):
                    self.i += 1
                    elems.append(None)
                    continue
                if self.eat('...'):
                    elems.append(('prest', self.binding_target()))
                else:
                    p = self.binding_target()
                    if self.eat('='):
                        p = ('pdef', p, self.assignment())
                    elems.append(p)
                if not self.at(']'):
                    self.expect(',')
            self.expect(']')
            return ('parr', elems)
        if t.text == '{':
            self.i += 1
            props = []
            while not self.at('}'):
                if self.eat('...'):
                    props.append(('rest', None, self.binding_target()))
                else:
                    key_tok = self.next()
                    computed = None
                    if key_tok.text == '[':
                        computed = self.assignment()
                        self.expect(']')
                        key = None
                    elif key_tok.kind == 'str':
                        key = key_tok.value
                    elif key_tok.kind == 'num':
                        key = num_key(key_tok.value)
                    else:
                        key = key_tok.text
                    if self.eat(':'):
                        p = self.binding_target()
                    else:
                        p = ('pid', key)
                    if self.eat('='):
                        p = ('pdef', p, self.assignment())
                    props.append(('prop', key if computed is None else computed, p, computed is not None))
                if not self.at('}'):
                    self.expect(',')
            self.expect('}')
            return ('pobj', props)
        raise JSParseError('syntax:binding')

    def to_pattern(self, e):
        """An expression read as an assignment target or a pattern."""
        k = e[0]
        if k == 'id':
            return ('pid', e[1])
        if k in ('mem', 'idx'):
            return e
        if k == 'arr':
            out = []
            for x in e[1]:
                if x is None:
                    out.append(None)
                elif x[0] == 'spread':
                    out.append(('prest', self.to_pattern(x[1])))
                else:
                    out.append(self.to_pattern(x))
            return ('parr', out)
        if k == 'obj':
            props = []
            for kind, key, val, computed in e[1]:
                if kind == 'spread':
                    props.append(('rest', None, self.to_pattern(val)))
                elif kind == 'init':
                    props.append(('prop', key, self.to_pattern(val), computed))
                else:
                    raise JSParseError('syntax:pattern')
            return ('pobj', props)
        if k == 'asg' and e[1] == '=':
            return ('pdef', self.to_pattern(e[2]), e[3])
        raise JSParseError('syntax:pattern')

    # ---- expressions ----
    def expression(self):
        e = self.assignment()
        if self.at(','):
            items = [e]
            while self.eat(','):
                items.append(self.assignment())
            return ('seq', items)
        return e

    def is_arrow_ahead(self):
        t = self.peek()
        if t.kind == 'ident' and self.at('=>', 1) and t.text not in ('async',):
            return True
        if t.kind == 'ident' and t.text == 'async':
            if self.peek(1).kind == 'ident' and self.at('=>', 2):
                return True
            if self.at('(', 1):
                return self.paren_then_arrow(1)
            return False
        if self.at('('):
            return self.paren_then_arrow(0)
        return False

    def paren_then_arrow(self, k):
        depth = 0
        j = self.i + k
        while j < len(self.toks):
            t = self.toks[j]
            if t.kind == 'punct' and t.text in '([{':
                depth += 1
            elif t.kind == 'punct' and t.text in ')]}':
                depth -= 1
                if depth == 0:
                    nxt = self.toks[j + 1] if j + 1 < len(self.toks) else EOF
                    return nxt.kind == 'punct' and nxt.text == '=>'
            j += 1
        return False

    def arrow(self):
        is_async = False
        if self.at('async'):
            self.i += 1
            is_async = True
        if self.peek().kind == 'ident':
            params = [('pid', self.next().text)]
        else:
            params = self.params()
        self.expect('=>')
        if self.at('{'):
            body = self.block()[1]
            expr = False
        else:
            body = [('ret', self.assignment())]
            expr = True
        return ('func', params, body, True, is_async, None)

    def params(self):
        self.expect('(')
        out = []
        while not self.at(')'):
            if self.eat('...'):
                out.append(('prest', self.binding_target()))
            else:
                p = self.binding_target()
                if self.eat('='):
                    p = ('pdef', p, self.assignment())
                out.append(p)
            if not self.at(')'):
                self.expect(',')
        self.expect(')')
        return out

    def function_expr(self):
        is_async = False
        if self.at('async'):
            self.i += 1
            is_async = True
        self.expect('function')
        gen = self.eat('*')
        name = None
        if self.peek().kind == 'ident' and not self.at('('):
            name = self.next().text
        params = self.params()
        body = self.block()[1]
        if gen:
            return ('func', params, [('unsup', 'js-generator')], False, is_async, name)
        return ('func', params, body, False, is_async, name)

    def assignment(self):
        if self.is_arrow_ahead():
            return self.arrow()
        if self.at('yield') :
            raise JSParseError('syntax:yield')
        left = self.conditional()
        t = self.peek()
        if t.kind == 'punct' and t.text in ASSIGN:
            self.i += 1
            right = self.assignment()
            target = self.to_pattern(left) if t.text == '=' and left[0] in ('arr', 'obj') else left
            if target[0] == 'id':
                target = ('pid', target[1])
            return ('asg', t.text, target, right)
        return left

    def conditional(self):
        c = self.binary(1)
        if self.at('?') and not self.at('?.'):
            self.i += 1
            old, self.no_in = self.no_in, False
            a = self.assignment()
            self.no_in = old
            self.expect(':')
            b = self.assignment()
            return ('cond', c, a, b)
        return c

    def binary(self, level):
        if level > len(BINARY):
            return self.unary()
        left = self.binary(level + 1)
        while True:
            t = self.peek()
            op = t.text
            if not (t.kind in ('punct', 'ident') and PREC.get(op) == level):
                return left
            if op == 'in' and self.no_in:
                return left
            if t.kind == 'ident' and op not in ('instanceof', 'in'):
                return left
            self.i += 1
            if op == '**':
                right = self.binary(level)
            else:
                right = self.binary(level + 1)
            left = ('log' if op in ('&&', '||', '??') else 'bin', op, left, right)

    def unary(self):
        t = self.peek()
        if t.kind == 'punct' and t.text in ('!', '-', '+', '~'):
            self.i += 1
            return ('un', t.text, self.unary())
        if t.kind == 'ident' and t.text in ('typeof', 'void', 'delete'):
            self.i += 1
            return ('un', t.text, self.unary())
        if t.kind == 'ident' and t.text == 'await':
            self.i += 1
            self.unary()
            return ('unsup', 'js-await')
        if t.kind == 'punct' and t.text in ('++', '--'):
            self.i += 1
            return ('upd', t.text, True, self.to_pattern(self.unary()))
        e = self.postfix()
        return e

    def postfix(self):
        e = self.call_member()
        t = self.peek()
        if t.kind == 'punct' and t.text in ('++', '--') and not t.nl:
            self.i += 1
            return ('upd', t.text, False, self.to_pattern(e))
        return e

    def arguments(self):
        self.expect('(')
        args = []
        old, self.no_in = self.no_in, False
        while not self.at(')'):
            if self.eat('...'):
                args.append(('spread', self.assignment()))
            else:
                args.append(self.assignment())
            if not self.at(')'):
                self.expect(',')
        self.no_in = old
        self.expect(')')
        return args

    def call_member(self):
        if self.at('new'):
            self.i += 1
            if self.at('.'):
                self.skip_statement()
                return ('unsup', 'js-new-target')
            callee = self.call_member_no_call()
            args = self.arguments() if self.at('(') else []
            e = ('new', callee, args)
        else:
            e = self.primary()
        return self.suffixes(e, True)

    def call_member_no_call(self):
        e = self.primary() if not self.at('new') else self.call_member()
        return self.suffixes(e, False)

    def suffixes(self, e, calls):
        while True:
            t = self.peek()
            if t.kind != 'punct' and not (t.kind == 'tmpl'):
                return e
            if t.kind == 'tmpl':
                # a tagged template
                self.i += 1
                return ('unsup', 'js-tagged-template')
            if t.text == '.':
                self.i += 1
                n = self.next()
                if n.kind != 'ident':
                    raise JSParseError('syntax:member')
                e = ('mem', e, n.text, False)
            elif t.text == '?.':
                self.i += 1
                if self.at('('):
                    e = ('call', e, self.arguments(), True)
                elif self.at('['):
                    self.i += 1
                    k = self.expression()
                    self.expect(']')
                    e = ('idx', e, k, True)
                else:
                    n = self.next()
                    e = ('mem', e, n.text, True)
            elif t.text == '[':
                self.i += 1
                old, self.no_in = self.no_in, False
                k = self.expression()
                self.no_in = old
                self.expect(']')
                e = ('idx', e, k, False)
            elif t.text == '(' and calls:
                e = ('call', e, self.arguments(), False)
            else:
                return e

    def primary(self):
        t = self.next()
        if t.kind == 'num':
            return ('num', t.value)
        if t.kind == 'str':
            return ('str', t.value)
        if t.kind == 'tmpl':
            strings, srcs = scan_template(t.text[1:-1])
            exprs = []
            for s in srcs:
                sub = Parser(tokenize_js(s))
                exprs.append(sub.expression())
            return ('tmpl', strings, exprs)
        if t.kind == 'regex':
            return ('regex', t.text)
        if t.kind == 'ident':
            w = t.text
            if w in ('function',) or (w == 'async' and self.at('function')):
                self.i -= 1
                return self.function_expr()
            if w == 'true':
                return ('lit', True)
            if w == 'false':
                return ('lit', False)
            if w == 'null':
                return ('lit', None)
            if w == 'this':
                return ('this',)
            if w == 'class':
                self.i -= 1
                self.i += 1
                self.skip_class_expr()
                return ('unsup', 'js-class')
            return ('id', w)
        if t.kind == 'punct':
            if t.text == '(':
                old, self.no_in = self.no_in, False
                e = self.expression()
                self.no_in = old
                self.expect(')')
                return ('paren', e)
            if t.text == '[':
                elems = []
                old, self.no_in = self.no_in, False
                while not self.at(']'):
                    if self.at(','):
                        self.i += 1
                        elems.append(None)
                        continue
                    if self.eat('...'):
                        elems.append(('spread', self.assignment()))
                    else:
                        elems.append(self.assignment())
                    if not self.at(']'):
                        self.expect(',')
                self.no_in = old
                self.expect(']')
                return ('arr', elems)
            if t.text == '{':
                return self.object_literal()
        raise JSParseError('syntax:primary:' + t.text)

    def skip_class_expr(self):
        while not self.done() and not self.at('{'):
            self.i += 1
        if not self.done():
            self.skip_balanced()

    def object_literal(self):
        props = []
        old, self.no_in = self.no_in, False
        while not self.at('}'):
            if self.eat('...'):
                props.append(('spread', None, self.assignment(), False))
            else:
                is_async = False
                kind = 'init'
                if self.peek().kind == 'ident' and self.peek().text in ('get', 'set', 'async') and self.peek(1).text not in (',', ':', '(', '}'):
                    mod = self.next().text
                    if mod == 'async':
                        is_async = True
                    else:
                        kind = mod
                gen = self.eat('*')
                kt = self.next()
                computed = False
                if kt.text == '[' and kt.kind == 'punct':
                    key = self.assignment()
                    self.expect(']')
                    computed = True
                elif kt.kind == 'str':
                    key = kt.value
                elif kt.kind == 'num':
                    key = num_key(kt.value)
                elif kt.kind == 'ident':
                    key = kt.text
                else:
                    raise JSParseError('syntax:object-key')
                if kind in ('get', 'set'):
                    params = self.params()
                    body = self.block()[1]
                    props.append((kind, key, ('func', params, body, False, False, None), computed))
                elif self.at('('):
                    params = self.params()
                    body = self.block()[1]
                    props.append(('init', key, ('func', params, body, False, is_async, None), computed))
                elif self.eat(':'):
                    props.append(('init', key, self.assignment(), computed))
                else:
                    if kt.kind != 'ident':
                        raise JSParseError('syntax:object-shorthand')
                    val = ('id', key)
                    if self.at('='):   # a shorthand with a default, valid only in a pattern
                        self.i += 1
                        val = ('asg', '=', ('pid', key), self.assignment())
                    props.append(('init', key, val, False))
            if not self.at('}'):
                self.expect(',')
        self.no_in = old
        self.expect('}')
        return ('obj', props)


def num_key(v):
    return str(int(v)) if v == int(v) else repr(v)


def parse_program(source):
    toks = tokenize_js(source)
    p = Parser(toks)
    return p.program()


def parse_expression(source):
    return Parser(tokenize_js(source)).expression()
