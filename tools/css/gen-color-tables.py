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
"""Generate src/css/css_color_tables.c: the matrices of the color spaces of CSS Color 4 (tools/css/color_spaces.tsv).

Every matrix is derived with exact rational arithmetic (fractions.Fraction) from the chromaticities of the primaries and the white point, then rounded
once to a double and written as a hexadecimal float, so the table is the same on every host and no digit is transcribed. Bradford adaptation (Color 4,
"Chromatic adaptation") and the Oklab matrices are constants of the draft, written below with their sections.

    tools/css/gen-color-tables.py [--out DIR] [--check] [--controls]

--check regenerates and compares with the committed table, and checks that every space of Color 4 has a row or a handler and a test.
--controls shows each of those fail on a planted mistake (a changed chromaticity, a missing row, a space nobody handles, a space no test names).

The matrices equal the ones the draft prints in its sample code (Color 4, "Sample code for color conversions": `lin_sRGB_to_XYZ`, `lin_P3_to_XYZ`,
`lin_a98rgb_to_XYZ`, `lin_ProPhoto_to_XYZ`, `lin_2020_to_XYZ`, `D65_to_D50`, `D50_to_D65`) to 1e-13 relative, which the first run of this generator measured against
the draft served on 2026-10-30 (the ProPhoto row needs the six-digit chromaticities of the draft's table, not the four-digit ones of the standard).
"""
import os
import sys
from fractions import Fraction as F

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Color 4, "Chromatic adaptation" and the white points of "Predefined color spaces": D50 and D65 as xy chromaticities.
WHITE = {"d50": (F(3457, 10000), F(3585, 10000)), "d65": (F(3127, 10000), F(3290, 10000))}
# Bradford (Color 4, "Chromatic adaptation": the Bradford matrices of the sample code are these, inverted and multiplied with the whites).
BRADFORD = [[F(8951, 10000), F(2664, 10000), F(-1614, 10000)], [F(-7502, 10000), F(17135, 10000), F(367, 10000)], [F(389, 10000), F(-685, 10000), F(10296, 10000)]]
# Oklab, Color 4 "Oklab": XYZ (D65) to LMS, and the LMS' to Lab matrix (Ottosson's, as the draft prints them to 16 digits).
OK_M1 = [["0.8190224379967030", "0.3619062600528904", "-0.1288737815209879"],
         ["0.0329836539323885", "0.9292868615863434", "0.0361446663506424"],
         ["0.0481771893596242", "0.2642395317527308", "0.6335478284694309"]]
OK_M2 = [["0.2104542683093140", "0.7936177747023054", "-0.0040720430116193"],
         ["1.9779985324311684", "-2.4285922420485799", "0.4505937096174110"],
         ["0.0259040424655478", "0.7827717124575296", "-0.8086757549230774"]]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def mat_vec(a, v):
    return [sum(a[i][k] * v[k] for k in range(3)) for i in range(3)]


def mat_inv(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


def xyz_of(xy):
    x, y = xy
    return [x / y, F(1), (1 - x - y) / y]


def rgb_to_xyz(prim, white):
    cols = [xyz_of(p) for p in prim]
    m = [[cols[j][i] for j in range(3)] for i in range(3)]
    s = mat_vec(mat_inv(m), xyz_of(WHITE[white]))
    return [[m[i][j] * s[j] for j in range(3)] for i in range(3)]


def adapt(src, dst):
    ls = mat_vec(BRADFORD, xyz_of(WHITE[src]))
    ld = mat_vec(BRADFORD, xyz_of(WHITE[dst]))
    d = [[F(0)] * 3 for _ in range(3)]
    for i in range(3):
        d[i][i] = ld[i] / ls[i]
    return mat_mul(mat_inv(BRADFORD), mat_mul(d, BRADFORD))


def hexf(fr):
    return float(fr).hex()


def mat_c(name, m):
    rows = ",\n".join("  {%s}" % ", ".join(hexf(v) for v in r) for r in m)
    return "const double %s[3][3] = {\n%s\n};\n" % (name, rows)


def ident(n):
    return n.replace("-", "_")


# Color 4, 10 "Predefined Color Spaces" and 4.4 "<color-space>": each space of the served lists and how this library holds it: a row of the table (the matrix is
# generated), the linear form of a row, or a conversion written in css_color_space.c.
SERVED = {
    "srgb": "row", "srgb-linear": "linear:srgb", "display-p3": "row", "display-p3-linear": "linear:display-p3", "a98-rgb": "row",
    "prophoto-rgb": "row", "rec2020": "row", "xyz": "alias:xyz-d65", "xyz-d50": "builtin", "xyz-d65": "builtin",
    "hsl": "builtin", "hwb": "builtin", "lab": "builtin", "lch": "builtin", "oklab": "builtin", "oklch": "builtin",
}
TEST_FILE = os.path.join(ROOT, "tests", "test-css-color-space.cpp")


def read_spaces(text=None):
    out = []
    if text is None:
        with open(os.path.join(HERE, "color_spaces.tsv"), encoding="utf-8") as h:
            text = h.read()
    if True:
        for n, line in enumerate(text.split("\n"), 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            c = line.split("\t")
            if len(c) != 11:
                raise ValueError("color_spaces.tsv:%d: expected 11 columns" % n)
            out.append(c)
    return out


def coverage(rows, served, test_text):
    """The problems with the table: a space of the served list with neither a row nor a handler, a row for a space the served list lacks, a space no test names."""
    problems = []
    names = {c[0] for c in rows}
    for s, how in served.items():
        if how == "row" and s not in names:
            problems.append("%s: the served list has it as a table row and color_spaces.tsv has none" % s)
        if how == "builtin" and s in names:
            problems.append("%s: listed as a conversion of css_color_space.c and also as a row" % s)
        if how not in ("row", "builtin") and not how.startswith(("linear:", "alias:")):
            problems.append("%s: no handler (%r)" % (s, how))
        if ('"%s"' % s) not in test_text:
            problems.append("%s: tests/test-css-color-space.cpp does not name it" % s)
    for n in names:
        if n not in served:
            problems.append("%s: a row for a space the served list lacks" % n)
    return problems


def generate(rows=None):
    lines = ["""/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* Generated by tools/css/gen-color-tables.py from tools/css/color_spaces.tsv. Do not edit. */

#include "css_color_space.h"
"""]
    for c in (rows if rows is not None else read_spaces()):
        name, kind, white = c[0], c[1], c[2]
        prim = [(F(c[3]), F(c[4])), (F(c[5]), F(c[6])), (F(c[7]), F(c[8]))]
        m = rgb_to_xyz(prim, white)
        lines.append("/* %s: %s */" % (name, c[10]))
        lines.append(mat_c("css_cs_%s_to_xyz" % ident(name), m))
        lines.append(mat_c("css_cs_xyz_to_%s" % ident(name), mat_inv(m)))
    lines.append("/* Bradford adaptation, Color 4 \"Chromatic adaptation\" */")
    lines.append(mat_c("css_cs_d65_to_d50", adapt("d65", "d50")))
    lines.append(mat_c("css_cs_d50_to_d65", adapt("d50", "d65")))
    m1 = [[F(v) for v in r] for r in OK_M1]
    m2 = [[F(v) for v in r] for r in OK_M2]
    lines.append("/* Oklab, Color 4 \"Oklab\" */")
    lines.append(mat_c("css_cs_xyz_to_lms", m1))
    lines.append(mat_c("css_cs_lms_to_xyz", mat_inv(m1)))
    lines.append(mat_c("css_cs_lmsp_to_oklab", m2))
    lines.append(mat_c("css_cs_oklab_to_lmsp", mat_inv(m2)))
    return "\n".join(lines)


def controls():
    """Each mistake the check must see."""
    rows = read_spaces()
    with open(TEST_FILE, encoding="utf-8") as h:
        test_text = h.read()
    good = generate(rows)
    failures = []
    if coverage(rows, SERVED, test_text):
        failures.append("the real table has problems: %s" % coverage(rows, SERVED, test_text))
    changed = [list(r) for r in rows]
    changed[0][3] = "0.641"
    if generate(changed) == good:
        failures.append("a changed chromaticity did not change the table")
    if not coverage([r for r in rows if r[0] != "rec2020"], SERVED, test_text):
        failures.append("a missing row was not seen")
    served2 = dict(SERVED)
    served2["rec2100-pq"] = "unhandled"
    if not coverage(rows, served2, test_text + ' "rec2100-pq" '):
        failures.append("a space with no handler was not seen")
    if not coverage(rows, SERVED, test_text.replace('"oklch"', '"OKLCH"')):
        failures.append("a space no test names was not seen")
    if not coverage(rows + [["bt709"] + rows[0][1:]], SERVED, test_text):
        failures.append("a row for a space the served list lacks was not seen")
    if failures:
        sys.exit("gen-color-tables --controls: " + "; ".join(failures))
    print("gen-color-tables --controls: 6 mistakes seen")


def main():
    args = sys.argv[1:]
    if "--controls" in args:
        controls()
        return
    out = os.path.join(ROOT, "src", "css")
    if "--out" in args:
        out = args[args.index("--out") + 1]
    text = generate()
    path = os.path.join(out, "css_color_tables.c")
    if "--check" in args:
        with open(path, encoding="utf-8") as h:
            if h.read() != text:
                sys.exit("the committed %s is not what the generator writes" % path)
        with open(TEST_FILE, encoding="utf-8") as h:
            problems = coverage(read_spaces(), SERVED, h.read())
        if problems:
            sys.exit("check-css-color: " + "; ".join(problems))
        print("css_color_tables.c is current, %d spaces covered" % len(SERVED))
        return
    os.makedirs(out, exist_ok=True)
    with open(path, "w", encoding="utf-8") as h:
        h.write(text)
    print("gen-color-tables: %d spaces" % len(read_spaces()))


if __name__ == "__main__":
    main()
