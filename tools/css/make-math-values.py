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
"""Make tests/data/css-math-values.txt: the published value family of the elementary functions of the math functions.

    tools/css/make-math-values.py > tests/data/css-math-values.txt

Each line is `<function> <argument hex floats...> <expected hex float>`, the expected value being the function's value at
those exact arguments, **correctly rounded**: computed in 90 decimal digits (the series of the sine, the cosine and the arctangent,
and the decimal module's exp, ln and square root, nothing of any C library or of the library under test) and rounded to the nearest double.
tests/test-css-math.cpp measures the library against them in units in the last place, with a bound for each function that it
holds the library to, and shows that a result one unit too far trips the same comparison.
"""
from decimal import Decimal, getcontext
import math
import random

getcontext().prec = 90
D = Decimal


def pi():
    # Machin: pi = 16 atan(1/5) - 4 atan(1/239)
    def atan_inv(n):
        x = D(1) / n
        s = x
        t = x
        k = 1
        while abs(t) > D(10) ** -95:
            t = -t * x * x
            k += 2
            s += t / k
        return s
    return 16 * atan_inv(5) - 4 * atan_inv(239)


PI = pi()
TWO_PI = 2 * PI


def sin_cos(x):
    x = D(x) % TWO_PI
    if x > PI:
        x -= TWO_PI
    s, c = D(0), D(0)
    term = x
    k = 1
    s = term
    while abs(term) > D(10) ** -95:
        term = -term * x * x / ((k + 1) * (k + 2))
        k += 2
        s += term
    term = D(1)
    c = term
    k = 0
    while abs(term) > D(10) ** -95:
        term = -term * x * x / ((k + 1) * (k + 2))
        k += 2
        c += term
    return s, c


def atan(x):
    x = D(x)
    if x < 0:
        return -atan(-x)
    if x > 1:
        return PI / 2 - atan(1 / x)
    for _ in range(8):
        x = x / (1 + (1 + x * x).sqrt())
    s = x
    term = x
    k = 1
    while abs(term) > D(10) ** -95:
        term = -term * x * x
        k += 2
        s += term / k
    return s * 256


def atan2(y, x):
    y, x = D(y), D(x)
    if x > 0:
        return atan(y / x)
    if x < 0:
        return atan(y / x) + (PI if y >= 0 else -PI)
    return PI / 2 if y > 0 else -PI / 2


def asin(x):
    x = D(x)
    return atan2(x, ((1 - x) * (1 + x)).sqrt())


def acos(x):
    x = D(x)
    return atan2(((1 - x) * (1 + x)).sqrt(), x)


def rnd(d):
    return float(d)


def hexf(v):
    return float(v).hex()


random.seed(20261022)
lines = []


def emit(name, args, value):
    lines.append(' '.join([name] + [hexf(a) for a in args] + [hexf(rnd(value))]))


def pts(lo, hi, n):
    return [lo + (hi - lo) * i / n for i in range(n + 1)]


trig_inputs = [0.0, 1e-300, 1e-10, 0.1, 0.5, 0.5235987755982988, 0.7853981633974483, 1.0, 1.5707963267948966, 2.0, 3.0, 3.141592653589793,
               4.0, 6.283185307179586, 10.0, 100.0, 1000.0, 12345.678, 1e6, 1e10, 1e15] + [random.uniform(-20, 20) for _ in range(40)]
for x in trig_inputs:
    for v in (x, -x):
        s, c = sin_cos(D(v))
        emit('sin', [v], s)
        emit('cos', [v], c)
        if abs(c) > D(10) ** -30:
            emit('tan', [v], s / c)
arc_inputs = [0.0, 1e-10, 0.1, 0.25, 0.5, 0.7071067811865476, 0.9, 0.99, 0.999999, 1.0] + [random.uniform(0, 1) for _ in range(30)]
for x in arc_inputs:
    for v in (x, -x):
        emit('asin', [v], asin(v))
        emit('acos', [v], acos(v))
for x in [0.0, 1e-10, 0.1, 0.5, 1.0, 2.0, 10.0, 1e3, 1e8, 1e15] + [random.uniform(0, 50) for _ in range(30)]:
    for v in (x, -x):
        emit('atan', [v], atan(v))
for _ in range(80):
    y, x = random.uniform(-10, 10), random.uniform(-10, 10)
    emit('atan2', [y, x], atan2(y, x))
for y, x in [(1.0, 1.0), (1.0, -1.0), (-1.0, -1.0), (-1.0, 1.0), (0.0, 1.0), (1.0, 0.0), (1e-10, 1.0), (1.0, 1e-10)]:
    emit('atan2', [y, x], atan2(y, x))
for x in [0.0, 1e-10, 0.5, 1.0, 2.0, 10.0, 100.0, 700.0, -1.0, -10.0, -700.0] + [random.uniform(-30, 30) for _ in range(40)]:
    emit('exp', [x], D(x).exp())
for x in [1e-300, 1e-10, 0.1, 0.5, 1.0, 1.5, 2.0, 2.718281828459045, 10.0, 100.0, 1e10, 1e300] + [random.uniform(0.001, 1000) for _ in range(40)]:
    emit('log', [x], D(x).ln())
for x in [0.5, 2.0, 3.0, 10.0, 100.0]:
    for b in [2.0, 10.0, 0.5, 3.0]:
        if x != 1.0:
            emit('log2', [x, b], D(x).ln() / D(b).ln())
for x in [0.0, 1e-300, 1e-10, 0.25, 0.5, 1.0, 2.0, 3.0, 10.0, 1e10, 1e300] + [random.uniform(0, 1000) for _ in range(40)]:
    emit('sqrt', [x], D(x).sqrt())
for _ in range(60):
    a, b = random.uniform(-100, 100), random.uniform(-100, 100)
    emit('hypot', [a, b], (D(a) * D(a) + D(b) * D(b)).sqrt())
for _ in range(10):
    a, b, c = random.uniform(-100, 100), random.uniform(-100, 100), random.uniform(-100, 100)
    emit('hypot3', [a, b, c], (D(a) ** 2 + D(b) ** 2 + D(c) ** 2).sqrt())
for x in [0.5, 1.0, 1.5, 2.0, 3.0, 10.0, 100.0] + [random.uniform(0.01, 50) for _ in range(30)]:
    for y in [-3.0, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0, 3.0, 10.0, 0.1]:
        emit('pow', [x, y], (D(y) * D(x).ln()).exp())
for a, b in [(5.0, 3.0), (18.0, 5.0), (0.3, 0.1), (1e10, 7.0), (123.456, 0.01), (2.0 ** 60, 3.0), (5.5, 2.25)]:
    d = D(a) % D(b)
    emit('fmod', [a, b], d)
print('# The published value family of the elementary functions: <function> <arguments> <correctly rounded value>, hex floats.')
print('# Made by tools/css/make-math-values.py (90 digit decimal arithmetic); do not edit.')
for l in lines:
    print(l)
