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
"""Write the seed corpus of tests/fuzz/fuzz_css_values.cpp: tests/fuzz/corpus/css-values/.

    tools/css/make-fuzz-values-seeds.py

A seed is three option bytes (see the harness) and CSS text. The seeds are a list of math functions, colors and the
functions of the function families (images, gradients, transforms, easing, shapes, substitution), each alone, each with
a tiny math depth, a tiny node bound and a tiny color nesting, and each with an allocation to fail.
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "tests", "fuzz", "corpus", "css-values")

TEXTS = """
calc(1px + 2px)
calc(1px + 2em)
calc(10px * 3 - 5px / 2)
calc(100% - 10px)
calc(1 + 2 * 3)
calc((1px + 2px) * (3 - 1))
calc(infinity * 1px)
calc(NaN * 1px)
calc(-infinity * 1px)
calc(0px * infinity)
calc(1px / 0)
calc(pi * 2)
calc(e)
calc(1deg + 0.5turn)
calc(1s + 100ms)
calc(1x + 96dppx)
min(1px, 2px, 3em)
max(10%, 5px)
clamp(1px, 50%, 10px)
clamp(none, 5px, 10px)
clamp(1px, none, none)
round(nearest, 7px, 2px)
round(up, 7px, 2px)
round(to-zero, -7px, 2px)
round(5.5, 2)
mod(-7px, 3px)
rem(-7px, 3px)
mod(1px, infinity * 1px)
sin(30deg)
cos(1rad)
tan(45deg)
asin(0.5)
acos(2)
atan(1)
atan2(1px, 1px)
pow(2, 3)
sqrt(16)
hypot(3px, 4px)
log(100, 10)
log(-1)
exp(1)
abs(-5px)
sign(-5px)
calc(sin(30deg) * 1px)
calc(pow(2, 10) * 1px)
calc(round(down, 5.5px, 1px) + mod(7px, 3px))
calc(min(1px, 2px) + max(3px, 4px))
calc(calc(calc(calc(1px))))
calc(((((((1)))))) * 1px)
calc(1px +2px)
calc(1px + )
calc()
calc(1px 2px)
calc(1px * 1px)
calc(10 / 3px)
calc(var(--a) + 1px)
calc(1px + attr(x px))
calc(sibling-index() * 1px)
calc(random(1, 2) * 1px)
random(--x, 1px, 5px, 1px)
progress(5px from 0px to 10px)
progress(50% from 0% to 100%)
calc(progress(5 from 0 to 10 no-clamp) * 1px)
red
RED
transparent
currentcolor
canvastext
buttonface
#f00
#F00A
#ff0000
#ff000080
#ggg
#12
rgb(255 0 0)
rgb(255, 0, 0)
rgba(255, 0, 0, 0.5)
rgb(255 0 0 / 50%)
rgb(none 0 0)
rgb(100% 0% 0%)
rgb(calc(255 * 0.5) 0 0)
rgb(300 -5 0)
hsl(120 100% 50%)
hsl(120deg, 100%, 50%)
hsl(none 100% 50%)
hsl(calc(60 * 2) 50% 50% / calc(0.5))
hwb(120 20% 30%)
hwb(0 100% 100%)
lab(50% 40 59)
lab(none none none)
lch(50% 40 59deg)
lch(50% 40 -360)
oklab(0.5 0.1 -0.1)
oklch(0.5 0.1 120)
oklch(70% 0.2 3.5turn / 0.5)
color(srgb 1 0 0)
color(display-p3 0.5 0.5 0.5 / 0.5)
color(xyz-d65 0.2 0.3 0.4)
color(--custom 1 2 3)
color(srgb none 0 0)
color(rec2020 1 1 1)
color-mix(in srgb, red, blue)
color-mix(in srgb, red 30%, blue)
color-mix(in oklch longer hue, red 10%, blue 20%)
color-mix(in lab, color-mix(in lch, red, blue), green)
color-mix(in srgb, red 0%, blue 0%)
color-mix(in srgb, red 150%, blue)
light-dark(red, blue)
light-dark(light-dark(red, blue), green)
contrast-color(white)
contrast-color(color-mix(in srgb, red, blue))
rgb(from red r g b)
rgb(from red calc(r + 10) g b / alpha)
hsl(from rgb(1 2 3) h s l)
oklch(from var(--c) l c h)
lab(from color-mix(in srgb, red, blue) calc(l * 0.5) a b)
color(from red srgb r g b)
color(from red --custom c1 c2 c3)
device-cmyk(0 0 0 1)
linear-gradient(red, blue)
linear-gradient(to right, red 0%, blue 100%)
linear-gradient(45deg, red, 20%, blue)
linear-gradient(in oklch longer hue, red, blue)
repeating-linear-gradient(red 0 10px, blue 10px 20px)
radial-gradient(circle at center, red, blue)
radial-gradient(closest-side ellipse at 10% 20%, red, blue)
radial-gradient(10px 20px at 30px 40px, red 0 50%, blue)
conic-gradient(from 90deg at 50% 50%, red, blue, red)
conic-gradient(red 0deg 90deg, blue 90deg 360deg)
linear-gradient(red calc(1px + 2px), calc(50% - 5px), blue)
linear-gradient(red, red, red, red, red, red, red, red, red, red, red, red)
image-set("a.png" 1x, "b.png" 2x)
image-set(url(a.png) 1x type("image/png"), url(b.png) 2dppx)
-webkit-image-set(url(a.png) 1x)
image(red)
image("a.png", red)
cross-fade(url(a.png), url(b.png))
cross-fade(50% url(a.png), 25% url(b.png))
element(#id)
paint(foo, 1, 2)
url(a.png)
src("a.png")
src(var(--x))
translate(10px)
translate(10px, 20%)
translate3d(1px, 2px, 3px)
rotate(45deg)
rotate3d(1, 1, 0, 1turn)
scale(1.5)
scale(1, 2)
scale3d(1, 2, 3)
skew(10deg, 20deg)
skewX(10deg)
matrix(1, 0, 0, 1, 0, 0)
matrix3d(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)
perspective(100px)
perspective(none)
translate(calc(1px + 2%)) rotate(calc(1turn / 4)) scale(calc(1 + 1))
translateX(1px) translateY(2px) translateZ(3px)
none
ease
ease-in-out
linear
step-start
step-end
cubic-bezier(0.1, 0.7, 1, 0.1)
cubic-bezier(2, 0, 3, 1)
cubic-bezier(0, 0, 0, 0)
steps(4, jump-start)
steps(4)
steps(1, jump-none)
steps(0)
linear(0, 0.25 25% 75%, 1)
linear(0, 1 50%, 0 25%, 1)
linear(0 10% 0%, 1)
linear(1, 0, 1, 0, 1, 0)
inset(10px 20px round 5px)
inset(10px round 5px / 1px 2px)
circle(50% at 10px 20px)
circle(closest-side at center)
ellipse(10px 20px at 30% 40%)
polygon(0 0, 100% 0, 50% 100%)
polygon(evenodd, 0 0, 1px 1px, 2px 0)
path("M 0 0 L 10 10")
path(evenodd, "M 0 0 Z")
rect(10px auto 20% 5px round 3px)
xywh(0 0 100px 50px round 5px)
shape(from 0 0, line to 10px 10px, close)
shape(evenodd from 0 0, hline by 10px, vline to 5px, curve to 1px 1px with 0 0 / 2px 2px, arc to 5px 5px of 3px)
var(--a)
var(--a, 1px)
var(--a, var(--b, calc(1px + 2px)))
env(safe-area-inset-top, 0px)
attr(data-x px, 5px)
if(media(width > 1px): red; else: blue)
inherit
unset
1px
50%
2
0
1e400
1e-400
-0
-0px
+5px
.5em
5e2px
1px 2px
1px, 2px
a b c
(1px)
[red]
{x}
url(
""".strip().split("\n")


def seed(name, o0, o1, o2, text):
    data = bytes([o0, o1, o2]) + text.encode("utf-8")
    with open(os.path.join(OUT, name + ".seed"), "wb") as f:
        f.write(data)


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".seed") and not f.startswith("reproducer-"):
            os.remove(os.path.join(OUT, f))
    n = 0
    for i, t in enumerate(TEXTS):
        seed("plain-%03d" % i, 0, 1, 0, t)
        seed("unitless-%03d" % i, 1, 1, 0, t)
        n += 2
        for o0, tag in ((2, "depth"), (4, "nodes"), (8, "nesting"), (14, "alltiny")):
            seed("%s-%03d" % (tag, i), o0, 1 + (i % 5), 0, t)
            n += 1
        seed("oom-%03d" % i, 0, 1, 1 + (i % 40), t)
        seed("oom2-%03d" % i, 0, 1, 1 + ((i * 7) % 90), t)
        n += 2
    # nesting that is deep, to reach the limits through the tiny-limit options and the defaults
    deep = ["calc(" * 40 + "1px" + ")" * 40, "min(" * 300 + "1px" + ")" * 300,
            "color-mix(in srgb, " * 40 + "red" + ", blue)" * 40,
            "light-dark(" * 40 + "red" + ", blue)" * 40,
            "rgb(from " * 40 + "red" + " r g b)" * 40,
            "calc(" + "1px + " * 200 + "1px)", "linear-gradient(" + "red, " * 200 + "blue)",
            "linear(" + "0, " * 100 + "1)", "polygon(" + "1px 1px, " * 100 + "2px 2px)"]
    for i, t in enumerate(deep):
        for o0, tag in ((0, "deep"), (14, "deeptiny")):
            seed("%s-%03d" % (tag, i), o0, 3, 0, t)
            n += 1
    own = {
        "own-math-no-fault": (0, 1, 255, "calc(1px + 2px) min(1px, 2em) clamp(1px, 50%, 10px) calc(10px * 3 - 5px / 2)"),
        "own-color-no-fault": (0, 1, 255, "rgb(255 0 0 / 0.25) color-mix(in srgb, red, blue) hsl(120 100% 50%) oklch(0.5 0.1 120)"),
        "own-alpha": (0, 1, 0, "rgb(255 0 0 / 0.25)"),
        "own-linear-unsorted": (0, 1, 0, "linear(0, 1 50%, 0 25%, 1)"),
        "own-nodes-exact": (0, 1, 0, "calc((1px + 2px) * (3 - 1))"),
    }
    for name, (o0, o1, o2, t) in own.items():
        seed(name, o0, o1, o2, t)
        n += 1
    print("wrote %d seeds to %s" % (n, OUT))


if __name__ == "__main__":
    main()
