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
"""The population of the condition differential (tools/oracle/css_cond_diff.py): media query lists for `matchMedia()`, supports conditions for `CSS.supports(text)`, and
(property, value) pairs for `CSS.supports(property, value)`, asked in a handful of viewport environments (css_cond_env.tsv).

    Gen(seed, index).document() -> {"env": name, "w": ..., "h": ..., "dpr": ..., "q": [["M", text] | ["S", text] | ["P", property, value] | ["R", text], ...]}

The generator is a grammar with its mistakes in it on purpose: units of every family, values of the wrong kind, prefixes on discrete features, ranges with their comparisons
in both directions, mixed `and`/`or`/`not`, reserved words as media types, unknown features, general-enclosed junk, and the declarations of every kind a supports condition
can hold. A population is a function of the seed and the document's index only.
"""

import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))

_r = random.Random()
rng = _r.random
R = _r.choice

ranges = {
 'width':['px','em','vw','rem','cm','in','pt','calc(100px + 10em)','0','1280px','1279.5px','1280.0001px'],
 'height':['px','em','vh','rem','800px'],
 'device-width':['px','em'], 'device-height':['px'],
 'aspect-ratio':None, 'device-aspect-ratio':None,
 'resolution':['dpi','dppx','x','dpcm','infinite','1dppx','96dpi','2x','192dpi'],
 'color':['int'],'color-index':['int'],'monochrome':['int'],
 'horizontal-viewport-segments':['int'],'vertical-viewport-segments':['int'],
 '-webkit-device-pixel-ratio':['num'],
}
disc = {
 'orientation':'portrait landscape','pointer':'none coarse fine','hover':'none hover','any-pointer':'none coarse fine','any-hover':'none hover',
 'prefers-color-scheme':'light dark','prefers-reduced-motion':'no-preference reduce','prefers-contrast':'no-preference less more custom',
 'color-gamut':'srgb p3 rec2020','dynamic-range':'standard high','video-dynamic-range':'standard high','video-color-gamut':'srgb p3 rec2020',
 'scan':'interlace progressive','update':'none slow fast','overflow-block':'none scroll paged','overflow-inline':'none scroll',
 'display-mode':'fullscreen standalone minimal-ui browser picture-in-picture','forced-colors':'none active','inverted-colors':'none inverted',
 'scripting':'none initial-only enabled','prefers-reduced-transparency':'no-preference reduce','prefers-reduced-data':'no-preference reduce',
 'grid':'0 1','-webkit-transform-3d':'0 1','nav-controls':'none back','prefers-reduced-motion ':'', 'shape':'rect round','resizable':'true false','ua-color-scheme':'light dark',
 'display-state':'normal minimized maximized fullscreen','environment-blending':'opaque additive subtractive'}
def num():
    return R(['0','1','2','-1','0.5','1.5','3','8','24','100','1280','800','256','1e2','+5','1e0','.5','5.',' 7 ','1e3','0.0','-0','10000000000000'])
def length(unit=None):
    u = unit or R(['px','px','px','em','rem','vw','vh','cm','mm','in','pt','pc','q','Q','ex','ch','vmin','vmax','dvw','svh','lvw','cqw','lh','rlh','cap','ic','', 'PX','Px'])
    if u.startswith('calc'): return u
    if rng()<.08: return R(['calc(100px + 2em)','calc(50% + 10px)','calc(1px * 3)','min(100px, 2000px)','max(1px,2px)','clamp(1px, 50vw, 2000px)','calc(100vw - 20px)','calc(1 / 0 * 1px)','calc(infinity * 1px)','calc(NaN * 1px)','calc(1px/2)','calc(100% + 1px)', 'calc(1 * 1280px)','calc(2px + 3)','abs(-100px)','sign(-5px)*1px','round(100.5px, 1px)','mod(1281px, 1000px)','rem(1500px,1000px)','hypot(3px,4px)','sin(30deg)*1px','pow(2,3)*1px'])
    if u in ('em','rem','vw','vh','px','cm','mm','in','pt','pc','q','Q','ex','ch','vmin','vmax','dvw','svh','lvw','cqw','lh','rlh','cap','ic','PX','Px'):
        return num()+u
    return num()
def ratio():
    return R(['16/9','1/1','1 / 1','16 /9','4/3','0/1','1/0','0/0','1','1.5','16/9.5','2/1','1280/800','8/5','0.5','calc(16/9)','calc(1/2)','1e0/2','-1/1','3 / 2','1.6','5/0.0001', '1000000/1','16/ 9', '16 / 0', '0 / 5','0/0.5','infinity','calc(infinity)/1','1/calc(2)','calc(2)/calc(3)'])
def res():
    return R(['1dppx','96dpi','2dppx','1x','0','0dppx','1.5x','192dpi','37.795275591dpcm','38dpcm','2x','infinite','Infinite','0.5x','-1x','1e0x','100dpi','72dpi'])
def val(f):
    k = ranges[f]
    if f in ('aspect-ratio','device-aspect-ratio'): return ratio()
    if f=='resolution': return res()
    if k==['int']: return R(['0','1','8','2','256','-1','1.0','8.5','+1','1e1','3','24','10000000000000000000'])
    if k==['num']: return R(['1','0','2','1.5','0.5','-1','1.00','96','1e0','.5'])
    return length()
def feat(f=None):
    r = rng()
    if r<.55:
        f = f or R(list(ranges))
        cmp = R(['<','<=','>','>=','=',':'])
        r2 = rng()
        if r2<.55:
            p = R(['','min-','max-','min-','max-']) if f in ranges else ''
            return '(%s%s: %s)' % (p, f, val(f))
        if r2<.7:
            return '(%s %s %s)' % (f, R(['<','<=','>','>=','=']), val(f))
        if r2<.82:
            return '(%s %s %s)' % (val(f), R(['<','<=','>','>=','=']), f)
        if r2<.95:
            a = R(['<','<=']); b = R([a,a,a,R(['<','<=']),'>','>=','='])
            if rng()<.5:
                return '(%s %s %s %s %s)' % (val(f), a, f, b, val(f))
            a2 = R(['>','>=']); b2 = R([a2,a2,R(['>','>=']),'<'])
            return '(%s %s %s %s %s)' % (val(f), a2, f, b2, val(f))
        return '(%s)' % f
    if r<.8:
        f = R(list(disc))
        f=f.strip()
        vals = disc[f+' '] if f+' ' in disc else disc.get(f,'').split()
        if isinstance(vals,str): vals = vals.split()
        v = R(vals + ['bogus','', '1','0','none']) if vals else R(['0','1','bogus'])
        p = R(['','','','min-','max-']) 
        form = rng()
        if form<.7: return '(%s: %s)' % (f, v) if v else '(%s)' % f
        if form<.8: return '(%s)' % f
        if form<.9: return '(%s%s: %s)' % (p,f,v)
        return '(%s = %s)' % (f, v)
    return R(['(foo)','(foo: bar)','(min-foo: 1)','(width: )','(: 1px)','(color: foo)','(--custom)','(--custom: 1)','()','( )','(1 < 2)','(1px < 2px)','(width < 1px < 2px)','(width: 1px 2px)','(width = = 1px)','(1px < width > 2px)','(1px = width = 2px)','(1px <= width < 2px)','(width: 1px !important)','(width: 1px;)','(width:1px,)','(-webkit-min-device-pixel-ratio: 1)','(-webkit-max-device-pixel-ratio: 2)','(-moz-foo)','(-webkit-foo: 1)','(color) and (foo)','[width]','{width}','(width > 1px','(width > 1px)) ','url(x)','("str")','(width: "a")','(min-resolution: 1)','(resolution: 1)','(resolution > 0)','(aspect-ratio: 0)','(aspect-ratio)','(min-aspect-ratio: 1/1)','(width: 0)','(width: -0)','(width: 0.0)','(width: 1e3px)','(width: 1E3PX)','(color: 08)','(width: calc(1px + 1))','(prefers-color-scheme)','(prefers-color-scheme: )','(1 = color)','(8 = color)','(8 <= color)','(color >= 8)','(color > 7.5)','(color: 8.0)','(color: 1e1)', 'selector(a)', 'style(--a:1)','(width: var(--x))','(width: env(x))','(width:attr(x))','(width: 1px/2)','(width: 100px) /**/','/**/ (width: 100px)', '(width: 1280px)', '(height: 800px)', '(width/*c*/:1280px)'])
def cond(d=0):
    r = rng()
    if d>3 or r<.45: return feat()
    if r<.6: return 'not ' + cond_paren(d+1)
    if r<.8:
        n = R([2,2,3]); op = R(['and','and','or'])
        return (' %s ' % op).join(cond_paren(d+1) for _ in range(n))
    return cond_paren(d+1)
def cond_paren(d):
    c = cond(d) 
    if c.startswith('(') and not c.startswith('(not') and rng()<.85 and c.count('(')==1 : return c
    return '(' + c + ')'
def mq():
    r = rng()
    ty = R(['all','screen','print','speech','tty','tv','projection','handheld','braille','embossed','aural','Screen','PRINT','foo','not','only','or','and','layer','none','default','auto','inherit','initial','unset','revert'])
    if r<.30: return cond()
    if r<.55:
        pre = R(['','','only ','not ','ONLY ','Not '])
        s = pre+ty
        if rng()<.7: s += ' and ' + cond_paren(1)
        if rng()<.15: s += ' and ' + cond_paren(1)
        return s
    if r<.6: return ty
    if r<.7: return 'not '+ty+' and '+R(['(color)','(width)','(foo)','(width > 0)'])
    if r<.75: return R(['','  ',',','a,','a, b',',a',' , ','screen,','screen,print','print, (color)','(color), (foo)','not (color), print','screen and (color), not screen'])
    if r<.8: return '%s and %s or %s' % (cond_paren(2), cond_paren(2), cond_paren(2))
    if r<.85: return 'screen and (color) or (hover)'
    if r<.9: return R(['screen and not (color)','screen or (color)','not screen and (color)','only (color)','not (color) and (width)','not (color) or (width)','(color) and not (width)','not not (color)','not (not (color))','not ((color))','((color))','(((color)))','( (color) and (width) )','screen and ((color) or (width))','screen and (not (color))','screen and not (color) and (width)','only screen','only screen and','screen and','and (color)','screen (color)','screen, ','not','only','not all','not  all','NOT ALL','all and (color)','all and not (color)','not all and (color)','(color) and','(color) or','and','or','(color) (width)','(color)and(width)','(color)and (width)','(color) and(width)','screen and(color)','screen and (color)and (width)','@media (color)','(color) /* c */ and (width)','(color)\tand\n(width)','\f(color)','(color)\\0 ','sc\\72een','\\73creen','not\\ all'])
    return cond()
def supports_text():
    props = ['display','color','width','foo','--x','--','-webkit-appearance','appearance','background','grid-template-columns','backdrop-filter','align-content','gap','transition','font-variant','text-wrap','anchor-name','container-type','Display','DISPLAY','zoom','-moz-appearance','scroll-timeline','view-transition-name','field-sizing','interpolate-size','margin-trim','color-scheme','offset-path','-webkit-box-shadow','line-clamp','text-box-trim','font-synthesis-position','overflow','overflow-anchor']
    vals = ['grid','red','1px','auto','inherit','initial','unset','revert','revert-layer','var(--x)','var()','calc(1px + 2px)','1','10px 20px','foo','!important','red !important','red!important','red ! important','"x"','','  ','blue;','red}','{','url(x)','env(x)','attr(x)','calc(1px + )','none','block','flex','subgrid','masonry','inline-grid','repeat(2, 1fr)','1fr 1fr','light dark','rgb(1 2 3)','color-mix(in srgb, red, blue)','light-dark(red, blue)','oklch(0.5 0.1 20)','if(media(width>1px): 1px; else: 2px)','first-valid(foo, 1px)','env(safe-area-inset-top)','10px !important','anchor(top)','1px/2px','sibling-index()','random(1, 2)','1e3px']
    def decl():
        p = R(props); v = R(vals)
        return '(%s: %s)' % (p,v) if rng()<.9 else '(%s:%s)' % (p,v)
    def leaf():
        r = rng()
        if r<.6: return decl()
        if r<.72: return R(['selector(a)','selector(a > b)','selector(:is(a,b))','selector(:foo)','selector(::before)','selector(::foo)','selector(a:hover)','selector(:has(a))','selector(a b)','selector()','selector(&)','selector(a, b)','selector(:is())','selector(:is(:foo))','selector(:not(:foo))','selector(:where(a,))','selector(:nth-child(2n+1 of a))','selector(:host)','selector(:host(a))','selector(::part(x))','selector(::slotted(a))','selector(:focus-visible)','selector(:modal)','selector(:popover-open)','selector(:open)','selector(:user-valid)','selector(::marker)','selector(::highlight(x))','selector(::view-transition)','selector(::scroll-button(*))','selector(:state(x))','selector(:-webkit-any(a))','selector(:-moz-any(a))','selector(::-webkit-scrollbar)','selector(a >> b)','selector(a||b)','selector(*|a)','selector(:dir(ltr))','selector(:lang(en))','selector(:lang("*-CH"))','selector([a=b i])','selector([a=b s])','selector(.a#b)','selector(a+b)','selector(a~b)','selector(> a)','selector(+ a)','selector(a >)','selector(:is(a b))','selector(:is(> a))','selector(:has(> a))','selector(:has(a b))','selector(:nth-child(odd))','selector(:nth-last-of-type(2))','selector(:scope)','selector(:root)','selector(:target-current)','selector(::details-content)','selector(::file-selector-button)','selector(::checkmark)','selector(::picker(select))','selector(:-webkit-autofill)','selector(:defined)','selector(:fullscreen)','selector(:picture-in-picture)','selector(:muted)','selector(:playing)','selector(:seeking)','selector(:paused)'])
        if r<.8: return R(['font-tech(variations)','font-tech(color-COLRv1)','font-tech(color-colrv0)','font-tech(bogus)','font-tech()','font-tech(features-opentype)','font-tech(features-aat)','font-tech(features-graphite)','font-tech(palettes)','font-tech(incremental)','font-tech(incremental-patch)','font-tech(incremental-range)','font-tech(color-svg)','font-tech(color-sbix)','font-tech(color-cbdt)','font-tech(variations, palettes)','font-tech("variations")','font-format(woff2)','font-format(woff)','font-format(opentype)','font-format(truetype)','font-format(collection)','font-format(embedded-opentype)','font-format(svg)','font-format(bogus)','font-format("woff2")','font-format()','font-format(woff2 woff)','font-format(woff, woff2)','font-format(WOFF2)','font-format(truetype-variations)','font-format(opentype-variations)','font-format(woff-variations)','font-format(woff2-variations)','font-format(woff2,)'])
        if r<.9: return R(['at-rule(@media)','at-rule(@foo)','at-rule(@font-face)','at-rule(@container)','at-rule(@layer)','at-rule(@property)','at-rule(@scope)','at-rule(@starting-style)','at-rule(@counter-style)','at-rule(@keyframes)','at-rule(@page)','at-rule(@import)','at-rule(@charset)','at-rule(@namespace)','at-rule(@supports)','at-rule(@custom-media)','at-rule(@position-try)','at-rule(@view-transition)','at-rule(@font-feature-values)','at-rule(@font-palette-values)','at-rule(@color-profile)','at-rule(@when)','at-rule(@else)','at-rule(@function)','at-rule(@mixin)','at-rule(@apply)','at-rule(media)','at-rule()','at-rule(@MEDIA)','at-rule(@-webkit-keyframes)','at-rule(@viewport)','at-rule(@document)','at-rule(@nest)','at-rule(@container style(--a:1))','at-rule(@media (color))','at-rule(@font-face foo)', 'at-rule(@page :first)','at-rule(@top-left)','at-rule(@scroll-timeline)','at-rule(@try)'])
        return R(['(foo)','(foo bar)','foo','(  )','()','[a]','{a}','url(x)','"s"','(a: b) c','foo()','env(x)','named-feature(x)','env(safe-area-inset-top)','env(foo)','(not (display:grid))','(display:grid) and','and (display:grid)','( (display:grid) )','not','or','and','(display: grid) !important','selector','font-tech','(selector(a))','(font-tech(variations))','((display:grid))','(((display:grid)))','( display : grid )','(display:grid)(display:grid)','(display:grid)/**/','/**/(display:grid)','display:grid','(display:grid;)','(display:grid;color:red)','(display:grid,)','(color:red) and (display:grid) or (width:1px)'])
    def cond(d=0):
        r = rng()
        if d>3 or r<.45: return leaf()
        if r<.62: return 'not ' + par(d+1)
        if r<.85:
            op = R(['and','or']); n=R([2,3])
            return (' %s ' % op).join(par(d+1) for _ in range(n))
        return par(d+1)
    def par(d):
        c = cond(d)
        if c.startswith('(') and c.count('(')==1: return c
        if c[0:4] in ('sele','font','at-r'): return c
        return '('+c+')'
    return cond()


def environments():
    out = []
    for line in open(os.path.join(HERE, "css_cond_env.tsv"), encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        name, w, h, dpr = line.rstrip("\n").split("\t")
        out.append((name, int(w), int(h), float(dpr)))
    return out


BARE_DECLARATIONS = ["display:grid", "display: grid", "display:grid !important", "(display:grid) ", "color:", "color:red;", "--x:", "--x: ", "--x:{}", "--x:[",
                     "width:1px;height:2px", "display:foo", "display"]


class Gen:
    """One document of the population."""

    PER_DOCUMENT = 40

    def __init__(self, seed, index):
        self.seed, self.index = seed, index

    def document(self):
        _r.seed((self.seed << 32) ^ (self.index * 2654435761) ^ 0x5bd1e995)
        envs = environments()
        name, w, h, dpr = envs[self.index % len(envs)]
        q = []
        for _ in range(self.PER_DOCUMENT):
            r = rng()
            if r < 0.55:
                q.append(["M", mq()])
            elif r < 0.9:
                q.append(["S", supports_text()])
            else:
                q.append(["P", R(["display", "color", "--x", "width", "foo", "--", "-webkit-appearance", "gap"]),
                          R(["grid", "red", "", "1px", "var(--x)", "foo", "!important", " red ", "red !important", "inherit", "calc(1px + 2px)", "initial", "revert", "1", "a;b", "}", "url(a b)", "var()"])])
        if self.index % 5 == 0:
            q.append(["S", R(BARE_DECLARATIONS)])
        return {"env": name, "w": w, "h": h, "dpr": dpr, "q": q}
