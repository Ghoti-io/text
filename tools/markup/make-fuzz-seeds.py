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
"""Write the seed corpus for fuzz_markup_tree.

    tools/markup/make-fuzz-seeds.py

The harness (tests/fuzz/fuzz_markup_tree.cpp) reads no document: its input is two option bytes and then a
program for building and rearranging a tree, one opcode byte and its operand bytes at a time, where an
operand is an index into the list of every node the document owns (the root's subtree in document order,
then each detached subtree, newest first). The seeds are programs, written here with a small model of that
list so that an operand names the node the shape means. The model is the tree's rules as far as building
goes (create, append, insert before, remove, move, clone); it is not the library, and when it drifts from
it (a limit refused an append the model made) the program is still a program, which is all the harness asks.

The seeds are tracked (tests/fuzz/corpus/.gitignore keeps `*.seed` and nothing else) and they are
*derived*: this script writes them again, the same bytes each time (a fixed seed per file, from its name).
Run it after a change to the lists below and commit the result. Files named `reproducer-*.seed` are not
touched: they are the recorded finds, replayed with the rest by `make test`
(tests/test-markup-tree-fuzz-seeds.cpp) and by every fuzz run.

What is written:

  build-*      a tree of one shape - a chain, a fan, a balanced tree, a ladder of text between elements,
               every kind of node - built to depth and width, under each set of limits that matters to it;
  edit-*       a built tree, then each kind of edit on it: insert before, remove, move, replace, free,
               clone (deep and shallow), set and remove an attribute, set data, a template's content;
  refuse-*     the programs whose point is a refusal: a text node given a child, a node put inside its own
               descendant, the root removed, an attribute past the cap, a document past its node limit;
  alloc-*      a built tree with an allocation failure armed before each kind of operation, for each of the
               first dozen allocations;
  random-*     a few dozen programs of random operations, for the search to start from.

Copyright 2026 by Corey Pennycuff
"""

import os
import random
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "markup-tree")
MAX_SEED = 1024
EXPECTED = (250, 600)  # the size of the corpus; a selection that drifts out of it is a mistake

# The harness's tables (tests/fuzz/fuzz_markup_tree.cpp). o1 % 6 picks the depth limit, (o1 // 6) % 8 the node cap, o2 % 6 the
# attribute cap.
DEPTHS = (0, 2, 3, 5, 8, 256)
NODE_CAPS = (0, 8, 16, 40, 0, 0, 200, 0)
ATTR_CAPS = (0, 1, 2, 4, 1024, 0)
NO_LIMITS = (5, 0)  # depth 256, no node cap, unlimited attributes (o1, o2)


def options(depth=None, nodes=None, attrs=None):
    """(o1, o2) for the named limits, given as values of the tables above (None: the table's no-limit entry)."""
    d = {None: 5}.get(depth, DEPTHS.index(depth) if depth in DEPTHS else 5)
    n = {None: 0}.get(nodes, NODE_CAPS.index(nodes) if nodes in NODE_CAPS else 0)
    a = {None: 5}.get(attrs, ATTR_CAPS.index(attrs) if attrs in ATTR_CAPS else 5)
    return (d + 6 * n, a)


# Opcodes
CREATE_ELEMENT, CREATE_TEXT, CREATE_CHARDATA, CREATE_PI_DOCTYPE, CREATE_FRAGMENT, APPEND, INSERT_BEFORE, REMOVE, REPLACE, FREE, \
    CLONE, SET_ATTRIBUTE, REMOVE_ATTRIBUTE, SET_DATA, ENSURE_TEMPLATE, ARM = range(16)

# kNames: a b c template x svg.
A, B, C, TEMPLATE, X, SVG = range(6)


class Node:
    def __init__(self, kind):
        self.kind = kind  # "element", "text", "chardata", "fragment", "root"
        self.parent = None
        self.children = []
        self.template = False

    def can_parent(self):
        return self.kind in ("element", "fragment")

    def ancestors_or_self(self):
        n = self
        while n is not None:
            yield n
            n = n.parent


class Program:
    """A program and the model of the list of nodes its operands index."""

    def __init__(self, o1, o2):
        self.out = bytearray([o1, o2])
        self.root = Node("root")
        self.detached = []  # newest first
        self.all_ids = {}

    # ---- the list of nodes ----
    def live(self):
        out = []

        def walk(n):
            out.append(n)
            for c in n.children:
                walk(c)

        walk(self.root)
        for top in self.detached:
            walk(top)
        return out

    def index(self, node):
        for i, n in enumerate(self.live()):
            if n is node:
                return i
        raise KeyError("a node the model does not hold")

    def byte(self, b):
        self.out.append(b & 0xFF)

    def operand(self, node):
        i = self.index(node)
        if i > 255:
            raise ValueError("more nodes than an operand reaches")
        self.byte(i)

    # ---- creating (a new node is detached, newest first) ----
    def _new(self, kind):
        n = Node(kind)
        self.detached.insert(0, n)
        return n

    def element(self, name=A, ns=0, prefix=0):
        self.byte(CREATE_ELEMENT)
        self.byte(ns + 4 * prefix)
        self.byte(name)
        n = self._new("element")
        n.template = name == TEMPLATE and ns in (0, 3)
        return n

    def text(self, which=1):
        self.byte(CREATE_TEXT)
        self.byte(which)
        return self._new("text")

    def chardata(self, comment=True, which=1):
        self.byte(CREATE_CHARDATA)
        self.byte((which * 2) | (0 if comment else 1))
        return self._new("chardata")

    def processing_instruction(self, which=1):
        self.byte(CREATE_PI_DOCTYPE)
        self.byte(which * 2 + 1)
        return self._new("chardata")

    def doctype(self, flags=0):
        self.byte(CREATE_PI_DOCTYPE)
        self.byte(flags & 0xFE)
        return self._new("doctype")

    def fragment(self):
        self.byte(CREATE_FRAGMENT)
        return self._new("fragment")

    # ---- placing ----
    def _detach(self, node):
        if node.parent is not None:
            node.parent.children.remove(node)
            node.parent = None
        elif node in self.detached:
            self.detached.remove(node)

    def append(self, parent, child):
        self.byte(APPEND)
        self.operand(parent)
        self.operand(child)
        self._detach(child)
        child.parent = parent
        parent.children.append(child)

    def insert_before(self, parent, child, ref):
        self.byte(INSERT_BEFORE)
        self.operand(parent)
        self.operand(child)
        if ref is None:
            self.byte(0)
        else:
            self.byte(1)
            self.operand(ref)
        self._detach(child)
        child.parent = parent
        parent.children.insert(parent.children.index(ref) if ref is not None else len(parent.children), child)

    def remove(self, node):
        self.byte(REMOVE)
        self.operand(node)
        self._detach(node)
        self.detached.insert(0, node)

    def replace(self, old, new):
        self.byte(REPLACE)
        self.operand(old)
        self.operand(new)
        parent = old.parent
        at = parent.children.index(old)
        self._detach(new)
        parent.children[at] = new
        new.parent = parent
        old.parent = None
        self.detached.insert(0, old)

    def free(self, node):
        self.byte(FREE)
        self.operand(node)
        self._detach(node)

    def clone(self, node, deep):
        self.byte(CLONE)
        self.operand(node)
        self.byte(1 if deep else 0)

        def copy(n, keep_children):
            c = Node(n.kind)
            c.template = n.template
            if keep_children:
                for k in n.children:
                    kc = copy(k, True)
                    kc.parent = c
                    c.children.append(kc)
            return c

        c = copy(node, deep)
        self.detached.insert(0, c)
        return c

    def set_attribute(self, element, ns=0, prefix=0, name=A, value=1):
        self.byte(SET_ATTRIBUTE)
        self.operand(element)
        self.byte(ns + 4 * prefix + 12 * name)
        self.byte(value)

    def remove_attribute(self, element, ns=0, name=A):
        self.byte(REMOVE_ATTRIBUTE)
        self.operand(element)
        self.byte(ns + 4 * name)

    def set_data(self, node, which=2):
        self.byte(SET_DATA)
        self.operand(node)
        self.byte(which)

    def ensure_template(self, element):
        self.byte(ENSURE_TEMPLATE)
        self.operand(element)

    def arm(self, n):
        """Fail the n-th allocation (1 to 12) of the next operation."""
        self.byte(ARM)
        self.byte(n - 1)

    def raw(self, *bytes_):
        for b in bytes_:
            self.byte(b)

    def result(self):
        return bytes(self.out)


# ---- shapes -----------------------------------------------------------------

def chain(p, n, name_of=lambda i: [A, B, C][i % 3]):
    top = p.element(name_of(0))
    prev = top
    for i in range(1, n):
        e = p.element(name_of(i))
        p.append(prev, e)
        prev = e
    return top, prev


def fan(p, n, kinds="et"):
    top = p.element(A)
    for i in range(n):
        k = kinds[i % len(kinds)]
        if k == "e":
            c = p.element([B, C, X][i % 3])
        elif k == "t":
            c = p.text(i % 6)
        elif k == "c":
            c = p.chardata(True, i % 6)
        elif k == "d":
            c = p.chardata(False, i % 6)
        else:
            c = p.processing_instruction(i % 6)
        p.append(top, c)
    return top


def balanced(p, depth, width):
    def build(level):
        e = p.element([A, B, C][level % 3])
        if level + 1 < depth:
            for _ in range(width):
                p.append(e, build(level + 1))
        else:
            p.append(e, p.text(level % 6))
        return e

    return build(0)


def ladder(p, n):
    top = p.element(A)
    prev = top
    for i in range(n):
        p.append(prev, p.text(i % 6))
        e = p.element(B)
        p.append(prev, e)
        prev = e
    return top


def every_kind(p):
    top = p.element(A, ns=1, prefix=1)
    for c in (p.element(B, ns=2, prefix=2), p.text(3), p.chardata(True, 4), p.chardata(False, 5), p.processing_instruction(2),
              p.element(SVG, ns=3), p.element(TEMPLATE, ns=3)):
        p.append(top, c)
    return top


LIMIT_SETS = (
    ("free", options()),
    ("depth3", options(depth=3)),
    ("depth5", options(depth=5)),
    ("nodes8", options(nodes=8)),
    ("nodes16", options(nodes=16)),
    ("nodes40", options(nodes=40)),
    ("attrs1", options(attrs=1)),
    ("depth2-nodes16-attrs2", options(depth=2, nodes=16, attrs=2)),
)


def seeds():
    out = {}

    def add(name, program):
        data = program.result()
        if len(data) > MAX_SEED:
            raise SystemExit("%s is %d bytes, over the %d a seed may be" % (name, len(data), MAX_SEED))
        out[name] = data

    def rng(name):
        return random.Random(zlib.crc32(name.encode()))

    # ---- build-* ----
    shapes = []
    for n in (1, 2, 3, 5, 8, 12, 20):
        shapes.append(("chain-%d" % n, lambda p, n=n: chain(p, n)))
    for n in (1, 3, 7, 16, 30):
        shapes.append(("fan-%d" % n, lambda p, n=n: fan(p, n)))
    shapes.append(("fan-mixed", lambda p: fan(p, 15, "etcdi")))
    shapes.append(("balanced-3x2", lambda p: balanced(p, 3, 2)))
    shapes.append(("balanced-4x2", lambda p: balanced(p, 4, 2)))
    shapes.append(("balanced-3x4", lambda p: balanced(p, 3, 4)))
    shapes.append(("ladder-6", lambda p: ladder(p, 6)))
    shapes.append(("every-kind", every_kind))
    for shape, build in shapes:
        for limits, (o1, o2) in LIMIT_SETS:
            p = Program(o1, o2)
            build(p)
            add("build-%s-%s" % (shape, limits), p)

    # ---- edit-* ----
    def edited(name, edit, build=lambda p: fan(p, 5, "et"), limits=("free", options())):
        p = Program(*limits[1])
        top = build(p)
        edit(p, top)
        add("edit-%s" % name, p)

    edited("insert-before-first", lambda p, top: p.insert_before(top, p.element(C), top.children[0]))
    edited("insert-before-null", lambda p, top: p.insert_before(top, p.element(C), None))
    edited("insert-before-last", lambda p, top: p.insert_before(top, p.text(2), top.children[-1]))
    edited("insert-fragment", lambda p, top: _insert_fragment(p, top))
    edited("remove-first", lambda p, top: p.remove(top.children[0]))
    edited("remove-last", lambda p, top: p.remove(top.children[-1]))
    edited("remove-subtree", lambda p, top: p.remove(top), build=lambda p: balanced(p, 3, 2))
    edited("remove-then-reattach", lambda p, top: (p.remove(top.children[1]), p.append(top, p.detached[0])))
    edited("move-within", lambda p, top: p.append(top, top.children[0]))
    edited("move-between", lambda p, top: _move_between(p))
    edited("replace-child", lambda p, top: p.replace(top.children[1], p.element(X)))
    edited("replace-with-attached", lambda p, top: p.replace(top.children[0], top.children[-1]))
    edited("replace-subtree", lambda p, top: p.replace(top.children[0], balanced(p, 2, 2)))
    edited("free-leaf", lambda p, top: p.free(top.children[0]))
    edited("free-subtree", lambda p, top: p.free(top), build=lambda p: balanced(p, 3, 3))
    edited("free-detached", lambda p, top: p.free(p.element(B)))
    edited("remove-then-free", lambda p, top: (p.remove(top.children[0]), p.free(p.detached[0])))
    edited("remove-subtree-then-free", lambda p, top: (p.remove(top.children[0]), p.free(p.detached[0])), build=lambda p: balanced(p, 3, 2))
    edited("clone-shallow", lambda p, top: p.clone(top, False))
    edited("clone-deep", lambda p, top: p.clone(top, True))
    edited("clone-deep-attach", lambda p, top: _clone_attach(p, top), build=lambda p: balanced(p, 3, 2))
    edited("clone-text", lambda p, top: p.clone(top.children[1], True))
    edited("attribute-set", lambda p, top: p.set_attribute(top, 0, 0, A, 1))
    edited("attribute-set-namespaced", lambda p, top: p.set_attribute(top, 1, 1, B, 2))
    edited("attribute-replace", lambda p, top: (p.set_attribute(top, 0, 0, A, 1), p.set_attribute(top, 0, 0, A, 3)))
    edited("attribute-many", lambda p, top: _attributes(p, top, 6), limits=("attrs2", options(attrs=2)))
    edited("attribute-many-free", lambda p, top: _attributes(p, top, 8))
    edited("attribute-remove", lambda p, top: (p.set_attribute(top, 0, 0, A, 1), p.remove_attribute(top, 0, A)))
    edited("attribute-remove-absent", lambda p, top: p.remove_attribute(top, 2, C))
    edited("data-text", lambda p, top: p.set_data(top.children[1], 3))
    edited("data-comment", lambda p, top: p.set_data(top.children[0], 5), build=lambda p: fan(p, 4, "ct"))
    edited("data-element", lambda p, top: p.set_data(top, 1))
    edited("template-ensure", lambda p, top: p.ensure_template(top), build=lambda p: p.element(TEMPLATE))
    edited("template-fill", lambda p, top: _template_fill(p, top), build=lambda p: p.element(TEMPLATE))
    edited("template-ensure-twice", lambda p, top: (p.ensure_template(top), p.ensure_template(top)), build=lambda p: p.element(TEMPLATE, ns=3))
    edited("template-not-a-template", lambda p, top: p.ensure_template(top))

    # ---- refuse-* ----
    def refusing(name, body, limits=("free", options())):
        p = Program(*limits[1])
        body(p)
        add("refuse-%s" % name, p)

    refusing("text-as-parent", lambda p: p.append(p.text(1), p.element(A)))
    refusing("comment-as-parent", lambda p: p.append(p.chardata(True), p.text(2)))
    refusing("into-own-descendant", lambda p: _cycle(p))
    refusing("into-self", lambda p: _self(p))
    refusing("root-removed", lambda p: p.raw(REMOVE, 0))
    refusing("root-freed", lambda p: p.raw(FREE, 0))
    refusing("root-as-child", lambda p: _root_as_child(p))
    refusing("second-document-element", lambda p: _second_element(p))
    refusing("doctype-in-element", lambda p: p.append(p.element(A), p.doctype(6)))
    refusing("insert-before-a-stranger", lambda p: _stranger(p))
    refusing("replace-root", lambda p: p.raw(REPLACE, 0, 1))
    refusing("attribute-on-text", lambda p: p.set_attribute(p.text(1)))
    refusing("fragment-in-fragment", lambda p: p.append(p.fragment(), p.fragment()))
    refusing("nodes-past-the-cap", lambda p: [p.element(A) for _ in range(12)], limits=("nodes8", options(nodes=8)))
    refusing("depth-past-the-cap", lambda p: chain(p, 8), limits=("depth3", options(depth=3)))
    refusing("depth-by-a-move", lambda p: _depth_by_move(p), limits=("depth3", options(depth=3)))
    refusing("depth-by-a-replace", lambda p: _depth_by_replace(p), limits=("depth2", options(depth=2)))
    refusing("attributes-past-the-cap", lambda p: _attributes(p, p.element(A), 5), limits=("attrs1", options(attrs=1)))
    refusing("clone-past-the-node-cap", lambda p: _clone_past_cap(p), limits=("nodes16", options(nodes=16)))
    refusing("clone-past-the-depth-cap", lambda p: p.clone(chain(p, 4)[0], True), limits=("depth3", options(depth=3)))
    refusing("template-in-attached", lambda p: _template_attached(p))

    # ---- alloc-* ----
    ops = (
        ("element", lambda p: p.element(A)),
        ("text", lambda p: p.text(2)),
        ("comment", lambda p: p.chardata(True, 3)),
        ("fragment", lambda p: p.fragment()),
    )
    for n in range(1, 13):
        for name, make in ops:
            p = Program(*NO_LIMITS)
            fan(p, 3, "et")
            p.arm(n)
            make(p)
            add("alloc-%s-%d" % (name, n), p)
        p = Program(*NO_LIMITS)
        top = fan(p, 3, "et")
        p.arm(n)
        p.clone(top, True)
        add("alloc-clone-deep-%d" % n, p)
        p = Program(*NO_LIMITS)
        top = fan(p, 3, "et")
        p.arm(n)
        p.set_attribute(top, 1, 1, B, 2)
        add("alloc-attribute-%d" % n, p)
        # (a clone copies the attributes after it has made the element: the failures of that step are the ones of the later allocations)
        p = Program(*NO_LIMITS)
        top = p.element(A, ns=1, prefix=1)
        p.set_attribute(top, 1, 1, B, 2)
        p.set_attribute(top, 2, 2, C, 3)
        p.arm(n)
        p.clone(top, False)
        add("alloc-clone-attributes-%d" % n, p)
        p = Program(*NO_LIMITS)
        top = p.element(A)
        p.append(top, p.element(B))
        p.set_attribute(top, 0, 0, A, 2)
        p.arm(n)
        p.clone(top, True)
        add("alloc-clone-attributes-deep-%d" % n, p)
        p = Program(*NO_LIMITS)
        top = fan(p, 3, "et")
        p.arm(n)
        p.set_data(top.children[1], 3)
        add("alloc-data-%d" % n, p)
        p = Program(*NO_LIMITS)
        top = p.element(TEMPLATE)
        p.arm(n)
        p.ensure_template(top)
        add("alloc-template-%d" % n, p)
        p = Program(*NO_LIMITS)
        top = fan(p, 3, "et")
        p.arm(n)
        p.insert_before(top, p.element(C), top.children[0])
        add("alloc-insert-%d" % n, p)

    # ---- random-* ----
    for k in range(60):
        name = "random-%02d" % k
        r = rng(name)
        o1, o2 = r.choice(LIMIT_SETS)[1] if k % 3 == 0 else NO_LIMITS
        p = Program(o1, o2)
        body = bytearray()
        for _ in range(r.randrange(10, 140)):
            body.append(r.choice((0, 0, 1, 1, 2, 3, 4, 5, 5, 5, 6, 7, 8, 9, 10, 11, 11, 12, 13, 14, 15)))
            for _ in range(r.randrange(0, 4)):
                body.append(r.randrange(256))
        p.out += body
        add(name, p)
    return out


# ---- the scripted edits that are not one line ------------------------------------------------------------

def _insert_fragment(p, top):
    f = p.fragment()
    p.append(f, p.element(B))
    p.append(f, p.text(2))
    p.insert_before(top, f, top.children[0])


def _move_between(p):
    a = balanced(p, 2, 2)
    b = p.element(C)
    p.append(b, p.text(3))
    p.append(b, a.children[0])


def _clone_attach(p, top):
    c = p.clone(top, True)
    p.append(c.children[0], p.clone(c, False))


def _attributes(p, element, n):
    for i in range(n):
        p.set_attribute(element, i % 4, (i // 4) % 3, i % 6, i % 6)


def _template_fill(p, template):
    """A template's content is a fragment that follows the template in the list; the model does not hold it, so its operand is the index."""
    p.ensure_template(template)
    filler = p.element(A)  # newest detached: index 1
    p.raw(APPEND, p.index(template) + 1, p.index(filler))


def _cycle(p):
    top, bottom = chain(p, 3)
    p.append(bottom, top)


def _self(p):
    e = p.element(A)
    p.append(e, e)


def _root_as_child(p):
    e = p.element(A)
    p.raw(APPEND, p.index(e), 0)


def _second_element(p):
    a, b = p.element(A), p.element(B)
    p.raw(APPEND, 0, p.index(a))
    p.raw(APPEND, 0, p.index(b))


def _stranger(p):
    a, b, c = p.element(A), p.element(B), p.element(C)
    p.byte(INSERT_BEFORE)
    p.operand(a)
    p.operand(b)
    p.byte(1)
    p.operand(c)


def _depth_by_move(p):
    top, bottom = chain(p, 3)
    other, _ = chain(p, 2)
    p.append(bottom, other)


def _depth_by_replace(p):
    top = p.element(A)
    child = p.element(B)
    p.append(top, child)
    deep, _ = chain(p, 3)
    p.replace(child, deep)


def _clone_past_cap(p):
    top = fan(p, 7, "et")
    p.clone(top, True)


def _template_attached(p):
    t = p.element(TEMPLATE, ns=3)
    outer = p.element(A)
    p.append(outer, t)
    p.ensure_template(t)


def main():
    produced = seeds()
    if not EXPECTED[0] <= len(produced) <= EXPECTED[1]:
        raise SystemExit("%d seeds, expected %d to %d: a selection that drifts out of the range is a mistake" % ((len(produced),) + EXPECTED))
    os.makedirs(CORPUS, exist_ok=True)
    for f in os.listdir(CORPUS):
        if f.endswith(".seed") and not f.startswith("reproducer-"):
            os.remove(os.path.join(CORPUS, f))
    for name, data in sorted(produced.items()):
        with open(os.path.join(CORPUS, name + ".seed"), "wb") as fh:
            fh.write(data)
    print("%d seeds written to %s" % (len(produced), os.path.relpath(CORPUS, ROOT)))


if __name__ == "__main__":
    main()
