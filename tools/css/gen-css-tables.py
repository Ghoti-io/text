#!/usr/bin/env python3
"""Generate the CSS registry tables from the pinned w3c/webref data.

Reads ``third_party/css/webref`` (fetched and verified against ``tools/css/WEBREF_COMMIT`` and ``tools/css/WEBREF_SHA256``
by ``tools/css/fetch.sh``; this script verifies the hashes again) and writes ``src/css/tables/*.c``. The output is
committed, because a build needs neither the network nor Python; a person runs this script when the pin changes, and
``make check-css-tables`` runs it into a scratch directory and diffs, to prove that what is committed is what this
produces and that no generated file was edited by hand.

What is written (the C side is src/css/tables/css_tables.h):

* properties: grammar, initial value, inheritance and animation type (the pin's free text mapped to closed enums by
  ``tools/css/normalize.tsv``, the raw string kept), applies-to, percentages, computed value, canonical order, logical
  group, longhands, reset-only longhands, legacy aliases, the names of the CSSStyleDeclaration attributes;
* descriptors per at-rule (features and nested at-rules included), at-rules;
* value types and functions, the property-scoped ones scoped;
* selectors (name and syntax text, for the selector layer);
* the atom table: every keyword of every grammar;
* the units and the functions the css-values definitions list (checked against the library by a test).

The consolidation rule is in ``tools/css/webref_model.py``. Determinism is a requirement: everything is emitted from sorted
input in a fixed format with no timestamp.

Usage:
    tools/css/gen-css-tables.py [--out DIR] [--check-only]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import os
import re
import sys

import webref_model as wm

HERE = wm.HERE
ROOT = wm.ROOT

LICENSE = """\
/*
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
"""

# The flag bits shared with css_registry.h (checked there by _Static_assert in the generated code).
PROP_SHORTHAND, PROP_ALIAS, PROP_NO_GRAMMAR, PROP_QUIRK_UNITLESS, PROP_CUSTOM = 0x01, 0x02, 0x04, 0x08, 0x10
TYPE_FUNCTION, TYPE_NATIVE, TYPE_NO_GRAMMAR, TYPE_SCOPED = 0x01, 0x02, 0x04, 0x08

DESC_KINDS = {None: 0, "range": 1, "discrete": 2, "at-rule": 3}


def die(msg):
    sys.stderr.write("gen-css-tables: %s\n" % msg)
    raise SystemExit(1)


# ---- the closed enums, read from the public header -------------------------

def header_enum(name_suffix):
    """The enumerators of ``typedef enum { ... } GTEXT_CSS_<name_suffix>;`` in css_registry.h, in order, as a name -> value map."""
    path = os.path.join(ROOT, "include", "ghoti.io", "text", "css", "css_registry.h")
    text = open(path, encoding="utf-8").read()
    m = re.search(r"typedef enum \{([^}]*)\} GTEXT_CSS_%s;" % name_suffix, text, re.S)
    if not m:
        die("css_registry.h has no enum GTEXT_CSS_%s" % name_suffix)
    body = re.sub(r"//[^\n]*|/\*.*?\*/|///<[^\n]*", "", m.group(1), flags=re.S)
    names = [x.strip().split("=")[0].strip() for x in body.split(",") if x.strip()]
    prefix = {"Inherit": "GTEXT_CSS_INHERIT_", "Animation": "GTEXT_CSS_ANIM_"}[name_suffix]
    out = {}
    for i, n in enumerate(names):
        if not n.startswith(prefix):
            die("enumerator %s of GTEXT_CSS_%s lacks the prefix %s" % (n, name_suffix, prefix))
        out[n[len(prefix):]] = i
    return out


def read_normalize():
    path = os.path.join(HERE, "normalize.tsv")
    table = {"inherited": {}, "animation": {}}
    with open(path, encoding="utf-8") as handle:
        for n, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) < 3 or cols[0] not in table:
                die("normalize.tsv:%d: expected field, string, enumerator" % n)
            if cols[1] in table[cols[0]]:
                die("normalize.tsv:%d: %r mapped twice" % (n, cols[1]))
            table[cols[0]][cols[1]] = cols[2]
    return table


def read_consolidate():
    path = os.path.join(HERE, "consolidate.tsv")
    rules = {"ignore-grammar": {}, "enum-from-values": {}, "descriptor-alias": {}, "nested-ignore": {}}
    if not os.path.exists(path):
        return rules
    with open(path, encoding="utf-8") as handle:
        for n, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if cols[0] not in rules or len(cols) < 4:
                die("consolidate.tsv:%d: expected directive, scope, name, reason" % n)
            rules[cols[0]][(cols[1], cols[2])] = cols[3]
    return rules


def read_supplement():
    rows = []
    path = os.path.join(HERE, "supplement.tsv")
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8") as handle:
        for n, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) != 10:
                die("supplement.tsv:%d: expected 10 columns" % n)
            rows.append(cols)
    return rows


def read_quirks():
    names = []
    with open(os.path.join(HERE, "QUIRKS_UNITLESS.txt"), encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line and not line.startswith("#"):
                names.append(line)
    return names


def read_natives():
    """The names the grammar engine implements in code (the table kNatives of css_grammar_parse.c)."""
    text = open(os.path.join(ROOT, "src", "css", "css_grammar_parse.c"), encoding="utf-8").read()
    m = re.search(r"kNatives\[\] = \{(.*?)\n\};", text, re.S)
    if not m:
        die("css_grammar_parse.c: kNatives not found")
    names = re.findall(r'\{"([^"]+)",\s*NT_', m.group(1))
    if len(names) < 40:
        die("css_grammar_parse.c: only %d native types found" % len(names))
    return set(names)


# ---- keywords ----------------------------------------------------------------

IDENT = re.compile(r"[A-Za-z_-][A-Za-z0-9_-]*")


def keywords_of(grammar):
    """Every keyword a Value Definition Syntax text spells: identifiers outside ``<...>``, quotes and function names."""
    out = set()
    i, n = 0, len(grammar)
    while i < n:
        c = grammar[i]
        if c == "<":
            j = grammar.find(">", i)
            i = n if j < 0 else j + 1
        elif c == "'":
            j = grammar.find("'", i + 1)
            i = n if j < 0 else j + 1
        elif c == "{":
            j = grammar.find("}", i)
            i = n if j < 0 else j + 1
        elif c.isalpha() or c in "_-":
            m = IDENT.match(grammar, i)
            word = m.group(0)
            i = m.end()
            if i < n and grammar[i] == "(":
                continue
            if word in ("-", "--"):
                continue
            out.add(word.lower())
        else:
            i += 1
    return out


# ---- the pool and the writers ------------------------------------------------

class Pool:
    def __init__(self):
        self.data = bytearray()
        self.index = {}

    def add(self, text):
        """The (offset, length) of a string, once. None and the empty string are (0, 0): the pool starts with a NUL."""
        if not text:
            return (0, 0)
        raw = text.encode("utf-8")
        if raw in self.index:
            return self.index[raw]
        off = len(self.data)
        self.data += raw + b"\0"
        self.index[raw] = (off, len(raw))
        return self.index[raw]


def cstr(pair):
    return "{%d, %d}" % pair


def char_array(name, data, comment):
    out = ["/* %s */\n" % comment, "const char %s[%d] = {\n" % (name, len(data))]
    line = "  "
    for b in data:
        item = "'\\0'," if b == 0 else ("'\\\\'," if b == 0x5C else ("'\\''," if b == 0x27 else (("'%s'," % chr(b)) if 0x20 <= b < 0x7F else "'\\%03o'," % b)))
        if len(line) + len(item) + 1 > 118:
            out.append(line.rstrip() + "\n")
            line = "  "
        line += item + " "
    out.append(line.rstrip() + "\n};\n")
    return "".join(out)


def header(title, source_hash, cmd):
    return LICENSE + """
/**
 * @file
 *
 * GENERATED by tools/css/gen-css-tables.py from w3c/webref (branch curated, commit %s;
 * manifest sha256 %s).
 * Do not edit.
 *
 * %s
 * Regenerate with:  tools/css/gen-css-tables.py
 * Verify with:      make check-css-tables
 */

#include "../css_tables.h"

""" % (open(os.path.join(HERE, "WEBREF_COMMIT")).read().strip(), source_hash, title)


# ---- the model ---------------------------------------------------------------

def build(model):
    norm = read_normalize()
    consolidate = read_consolidate()
    quirks = read_quirks()
    natives = read_natives()
    inh_enum = header_enum("Inherit")
    anim_enum = header_enum("Animation")

    used_norm = {"inherited": set(), "animation": set()}

    # --- properties ---
    props = {}
    for name, entries in model["props"].items():
        alias = None
        for e in entries:
            if "legacyAliasOf" in e.raw:
                if alias not in (None, e.raw["legacyAliasOf"]):
                    die("%s: two alias targets" % name)
                alias = e.raw["legacyAliasOf"]
        base = wm.base_of(entries, "property " + name)
        any_entry = base or sorted(entries, key=wm.Entry.key)[-1]
        row = {"name": name, "alias_of": alias, "source": any_entry.stem}
        r = any_entry.raw
        row["style"] = list(r.get("styleDeclaration", []))
        if alias:
            row["grammar"] = None
        elif base is None:
            row["grammar"] = None
            row["nogram"] = True
        else:
            row["grammar"] = wm.merged_grammar(base, entries)
        if not alias and base is not None:
            for k, f in (("initial", "initial"), ("appliesTo", "applies_to"), ("percentages", "percentages"),
                         ("computedValue", "computed_value"), ("canonicalOrder", "canonical_order"),
                         ("logicalPropertyGroup", "group")):
                row[f] = r.get(k)
            row["inherited_raw"] = r.get("inherited")
            row["animation_raw"] = r.get("animationType")
            row["longhands"] = list(r.get("longhands", []))
            row["resets"] = list(r.get("resetLonghands", []))
        else:
            row.setdefault("longhands", [])
            row.setdefault("resets", [])
        props[name] = row

    for name, grammar, initial, inherited, animation, computed, applies, percentages, canonical, source in read_supplement():
        if name in props:
            die("supplement.tsv: %s is in the pin; the supplement is stale" % name)
        props[name] = {"name": name, "alias_of": None, "source": source, "style": [], "grammar": grammar, "initial": initial,
                       "applies_to": applies, "percentages": percentages, "computed_value": computed, "canonical_order": canonical,
                       "group": None, "inherited_raw": inherited, "animation_raw": animation, "longhands": [], "resets": []}

    names = sorted(props)
    index = {n: i + 1 for i, n in enumerate(names)}

    # integrity of the relations
    for n, row in props.items():
        if row["alias_of"] and row["alias_of"] not in props:
            die("%s is an alias of %s, which the pin does not define" % (n, row["alias_of"]))
        for k in ("longhands", "resets"):
            for t in row[k]:
                if t not in props:
                    die("%s lists %s %s, which the pin does not define" % (n, k[:-1], t))
    # an alias of an alias is followed at run time; a cycle is not allowed
    for n in props:
        seen, cur = set(), n
        while props[cur]["alias_of"]:
            if cur in seen:
                die("alias cycle at %s" % cur)
            seen.add(cur)
            cur = props[cur]["alias_of"]

    # --- inheritance and animation: the mapped enums ---
    def map_field(field, raw, enum, prop):
        if raw is None:
            return enum["UNKNOWN" if field == "inherited" else "UNSPECIFIED"], None
        if raw not in norm[field]:
            die("%s: the %s string %r is not in tools/css/normalize.tsv" % (prop, field, raw))
        used_norm[field].add(raw)
        e = norm[field][raw]
        if e == "DERIVE":
            return None, raw
        if e not in enum:
            die("normalize.tsv maps %r to %s, which is not an enumerator" % (raw, e))
        return enum[e], raw

    for n, row in props.items():
        if row["alias_of"]:
            row["inherited"], row["animation"] = None, None
            continue
        row["inherited"], _ = map_field("inherited", row.get("inherited_raw"), inh_enum, n)
        row["animation"], _ = map_field("animation", row.get("animation_raw"), anim_enum, n)

    # shorthand inheritance: derived from the longhands where the pin leaves it to them
    derived = {}

    def derive(n, trail=()):
        row = props[n]
        if n in derived:
            return derived[n]
        if n in trail:
            die("longhand cycle through %s" % n)
        if row["alias_of"]:
            target = row["alias_of"]
            derived[n] = derive(target, trail + (n,))
            return derived[n]
        if row["inherited"] is not None:
            derived[n] = row["inherited"]
            return derived[n]
        lh = row["longhands"]
        if n == "all" or not lh:
            derived[n] = inh_enum["MIXED"] if n == "all" else inh_enum["UNKNOWN"]
            return derived[n]
        vals = {derive(t, trail + (n,)) for t in lh}
        if vals == {inh_enum["YES"]}:
            derived[n] = inh_enum["YES"]
        elif vals == {inh_enum["NO"]}:
            derived[n] = inh_enum["NO"]
        elif inh_enum["UNKNOWN"] in vals and len(vals) == 1:
            derived[n] = inh_enum["UNKNOWN"]
        else:
            derived[n] = inh_enum["MIXED"]
        return derived[n]

    for n in names:
        props[n]["inherited_final"] = derive(n)
    for n in names:
        props[n]["flags"] = ((PROP_SHORTHAND if (props[n]["longhands"] or n == "all") else 0)
                             | (PROP_ALIAS if props[n]["alias_of"] else 0)
                             | (PROP_NO_GRAMMAR if props[n].get("nogram") else 0)
                             | (PROP_CUSTOM if n == "--*" else 0))
    for q in quirks:
        if q not in props:
            die("QUIRKS_UNITLESS.txt names %s, which the pin does not define" % q)
        props[q]["flags"] |= PROP_QUIRK_UNITLESS

    for field, seen in used_norm.items():
        for raw in norm[field]:
            if raw not in seen:
                die("normalize.tsv: the %s string %r is used by no property (stale)" % (field, raw))

    # --- types and functions ---
    types = []
    # the types and functions the pin nests in something that is not a property are global definitions (webref_model.py); the
    # directive nested-ignore drops one container's copy of a name whose copies say different things (`type()`)
    merged_types = wm.merged_types(model, consolidate["nested-ignore"])
    for k, v in consolidate["nested-ignore"].items():
        if not (isinstance(v, tuple) and v[0] == "used"):
            die("consolidate.tsv: nested-ignore %s %s names no nested type (stale)" % k)
    for (scope, key), entries in merged_types.items():
        base = wm.base_of(entries, "type %s" % key)
        grammar = wm.merged_grammar(base, entries) if base else None
        # a "grammar" that is the name again is prose, not a definition (`fit-content()` of column-width)
        if grammar is not None and grammar.strip() == key:
            grammar = None
        if (scope, key) in consolidate["ignore-grammar"]:
            if grammar is None:
                die("consolidate.tsv: %s %s has no grammar to ignore" % (scope, key))
            grammar = None
            consolidate["ignore-grammar"][(scope, key)] = ("used", consolidate["ignore-grammar"][(scope, key)])
        if scope and scope not in props:
            die("type %s is nested in %s, which the pin does not define" % (key, scope))
        if (scope, key) in consolidate["enum-from-values"]:
            # the pin lists the alternatives of the type only as the keyword values nested in its entry
            if grammar is not None:
                die("consolidate.tsv: %s %s has a grammar of its own" % (scope, key))
            words = []
            for e in entries:
                for v in e.raw.get("values", []):
                    if v["type"] == "value" and re.fullmatch(r"[A-Za-z][A-Za-z0-9-]*", v.get("value", "")) and v["value"] not in words:
                        words.append(v["value"])
            if not words:
                die("consolidate.tsv: %s %s lists no keyword values" % (scope, key))
            grammar = " | ".join(words)
            consolidate["enum-from-values"][(scope, key)] = ("used", "")
        flags = TYPE_FUNCTION if key.endswith(")") else 0
        if scope:
            flags |= TYPE_SCOPED
        if key in natives and not scope:
            flags |= TYPE_NATIVE
        elif grammar is None:
            flags |= TYPE_NO_GRAMMAR
        src = (base or sorted(entries, key=wm.Entry.key)[-1]).stem
        types.append({"name": key, "scope": index[scope] if scope else 0, "grammar": grammar, "source": src, "flags": flags})
    for rule in ("ignore-grammar", "enum-from-values"):
        for k, v in consolidate[rule].items():
            if not (isinstance(v, tuple) and v[0] == "used"):
                die("consolidate.tsv: %s %s %s names no type (stale)" % ((rule,) + k))
    types.sort(key=lambda t: (t["name"].encode(), t["scope"]))
    for a, b in zip(types, types[1:]):
        if (a["name"], a["scope"]) == (b["name"], b["scope"]):
            die("duplicate type %s" % a["name"])

    # --- at-rules and descriptors ---
    atnames = sorted(model["atrules"])
    atindex = {n: i + 1 for i, n in enumerate(atnames)}
    atrows = []
    descrows = []
    for n in atnames:
        entries = model["atrules"][n]
        base = wm.base_of(entries, "at-rule " + n)
        grammar = wm.merged_grammar(base, entries) if base else None
        mine = sorted((k for k in model["descs"] if k[0] == n), key=lambda k: k[1])
        first = len(descrows)
        decl = False
        for (a, dn) in mine:
            des = model["descs"][(a, dn)]
            db = wm.base_of(des, "descriptor %s %s" % (a, dn))
            if db is None:
                die("descriptor %s %s has no grammar" % (a, dn))
            kind = DESC_KINDS[db.raw.get("type")]
            if kind == 0:
                decl = True
            descrows.append({"at": atindex[a], "name": dn, "grammar": wm.merged_grammar(db, des),
                             "initial": db.raw.get("initial"), "kind": kind})
        for (scope, alias), reason in sorted(consolidate["descriptor-alias"].items()):
            if scope != n:
                continue
            target = reason.split(":", 1)[0].strip()
            row = next((d for d in descrows[first:] if d["name"] == target), None)
            if row is None or any(d["name"] == alias for d in descrows[first:]):
                die("consolidate.tsv: descriptor-alias %s %s -> %s: the target is missing or the alias exists" % (scope, alias, target))
            descrows.append(dict(row, name=alias))
            mine = mine + [(n, alias)]
            consolidate["descriptor-alias"][(scope, alias)] = "used"
            decl = True
        if True:
            tail = sorted(descrows[first:], key=lambda d: d["name"].encode())
            descrows[first:] = tail
        atrows.append({"name": n, "grammar": grammar, "first": first, "count": len(mine), "decl": decl,
                       "source": (base or sorted(entries, key=wm.Entry.key)[-1]).stem})
    for k, v in consolidate["descriptor-alias"].items():
        if v != "used":
            die("consolidate.tsv: descriptor-alias %s %s names no at-rule (stale)" % k)
    for k in model["descs"]:
        if k[0] not in atindex:
            die("descriptor %s of an at-rule the pin does not define" % (k,))

    # --- selectors ---
    sels = []
    for n in sorted(model["selectors"]):
        entries = model["selectors"][n]
        base = wm.base_of(entries, "selector " + n)
        any_entry = base or sorted(entries, key=wm.Entry.key)[-1]
        sels.append({"name": n, "syntax": base.raw["value"] if base else None, "source": any_entry.stem})

    # --- keywords ---
    kw = set()
    for row in props.values():
        if row["grammar"]:
            kw |= keywords_of(row["grammar"])
    for t in types:
        if t["grammar"]:
            kw |= keywords_of(t["grammar"])
    for d in descrows:
        kw |= keywords_of(d["grammar"])
    for a in atrows:
        if a["grammar"]:
            kw |= keywords_of(a["grammar"])
    keywords = sorted(kw)

    # --- the units and the functions of the css-values definitions ---
    units, funcs = {}, set()
    for lvl in (3, 4, 5):
        for d in model["dfns"][lvl]:
            if d["type"] == "value" and d["for"] and d["for"][0] in ("<length>", "<angle>", "<time>", "<frequency>", "<resolution>", "<flex>"):
                for text in d["linkingText"]:
                    if re.fullmatch(r"[A-Za-z]+", text) and not text.endswith(" unit"):
                        measures = d["for"][0][1:-1]
                        if units.get(text, measures) != measures:
                            die("unit %s is for two types" % text)
                        units[text] = measures
            elif d["type"] == "function":
                for text in d["linkingText"]:
                    if re.fullmatch(r"[a-z-]+\(\)", text):
                        funcs.add(text)

    return {"props": props, "names": names, "index": index, "types": types, "atrows": atrows, "descrows": descrows,
            "sels": sels, "keywords": keywords, "units": sorted(units.items()), "funcs": sorted(funcs),
            "manifest_hash": model["manifest_hash"], "stems": model["stems"]}


# ---- emission ----------------------------------------------------------------

def emit(b, outdir):
    pool = Pool()
    pool.data += b"\0"  # offset 0 is the empty string
    mh = b["manifest_hash"]
    files = {}

    # properties
    names, props, index = b["names"], b["props"], b["index"]
    refs = []
    styles = []
    groups = sorted({p["group"] for p in props.values() if p.get("group")})
    gidx = {g: i + 1 for i, g in enumerate(groups)}
    rows = []
    for n in names:
        p = props[n]
        lh_first = len(refs)
        refs += [index[t] for t in p["longhands"]]
        rs_first = len(refs)
        refs += [index[t] for t in p["resets"]]
        st_first = len(styles)
        styles += [pool.add(s) for s in p["style"]]
        rows.append("  {.name = %s, .grammar = %s, .initial = %s, .inherited_raw = %s, .animation_raw = %s, .computed_value = %s,"
                    " .applies_to = %s, .percentages = %s, .canonical_order = %s, .source = %s, .group = %d,"
                    " .alias_of = %d, .longhand_first = %d, .longhand_count = %d, .reset_first = %d, .reset_count = %d,"
                    " .style_first = %d, .style_count = %d, .inherited = %d, .animation = %d, .flags = %d}," % (
                        cstr(pool.add(n)), cstr(pool.add(p["grammar"])), cstr(pool.add(p.get("initial"))),
                        cstr(pool.add(p.get("inherited_raw"))), cstr(pool.add(p.get("animation_raw"))),
                        cstr(pool.add(p.get("computed_value"))), cstr(pool.add(p.get("applies_to"))),
                        cstr(pool.add(p.get("percentages"))), cstr(pool.add(p.get("canonical_order"))),
                        cstr(pool.add(p["source"])), gidx.get(p.get("group"), 0),
                        index[p["alias_of"]] if p["alias_of"] else 0, lh_first, len(p["longhands"]),
                        rs_first, len(p["resets"]), st_first, len(p["style"]),
                        p["inherited_final"], p["animation"] if p["animation"] is not None else 0, p["flags"]))
    gmembers = []
    grows = []
    for g in groups:
        first = len(gmembers)
        mem = [index[n] for n in names if props[n].get("group") == g]
        gmembers += mem
        grows.append("  {%s, %d, %d}," % (cstr(pool.add(g)), first, len(mem)))

    pc = header("The properties.", mh, "") + """\
_Static_assert(GTEXT_CSS_PROP_SHORTHAND == %d && GTEXT_CSS_PROP_ALIAS == %d && GTEXT_CSS_PROP_NO_GRAMMAR == %d &&
                   GTEXT_CSS_PROP_QUIRK_UNITLESS == %d && GTEXT_CSS_PROP_CUSTOM == %d,
               "the generator and css_registry.h disagree on the property flags");

const size_t gtext_css_tbl_property_count = %d;
const css_tbl_property gtext_css_tbl_properties[%d] = {
%s
};

const uint32_t gtext_css_tbl_property_refs[%d] = {
%s
};
const size_t gtext_css_tbl_property_ref_count = %d;

const css_tstr gtext_css_tbl_style_names[%d] = {
%s
};

const size_t gtext_css_tbl_group_count = %d;
const css_tbl_group gtext_css_tbl_groups[%d] = {
%s
};
const uint32_t gtext_css_tbl_group_members[%d] = {
%s
};
""" % (PROP_SHORTHAND, PROP_ALIAS, PROP_NO_GRAMMAR, PROP_QUIRK_UNITLESS, PROP_CUSTOM, len(names), len(names), "\n".join(rows),
       max(1, len(refs)), wrap(refs), len(refs), max(1, len(styles)), "\n".join("  %s," % cstr(s) for s in styles) or "  {0, 0},",
       len(groups), len(groups), "\n".join(grows), max(1, len(gmembers)), wrap(gmembers))
    files["css_tables_props.c"] = pc

    # types
    trows = []
    for t in b["types"]:
        trows.append("  {.name = %s, .grammar = %s, .source = %s, .scope = %d, .flags = %d}," % (
            cstr(pool.add(t["name"])), cstr(pool.add(t["grammar"])), cstr(pool.add(t["source"])), t["scope"], t["flags"]))
    tc = header("The value types and functions.", mh, "") + """\
_Static_assert(GTEXT_CSS_TYPE_FUNCTION == %d && GTEXT_CSS_TYPE_NATIVE == %d && GTEXT_CSS_TYPE_NO_GRAMMAR == %d &&
                   GTEXT_CSS_TYPE_SCOPED == %d,
               "the generator and css_registry.h disagree on the type flags");

const size_t gtext_css_tbl_type_count = %d;
const css_tbl_type gtext_css_tbl_types[%d] = {
%s
};
""" % (TYPE_FUNCTION, TYPE_NATIVE, TYPE_NO_GRAMMAR, TYPE_SCOPED, len(b["types"]), len(b["types"]), "\n".join(trows))
    files["css_tables_types.c"] = tc

    # at-rules and descriptors
    arows = []
    for a in b["atrows"]:
        arows.append("  {.name = %s, .grammar = %s, .source = %s, .first = %d, .count = %d, .declarations = %d}," % (
            cstr(pool.add(a["name"])), cstr(pool.add(a["grammar"])), cstr(pool.add(a["source"])), a["first"], a["count"], 1 if a["decl"] else 0))
    drows = []
    for d in b["descrows"]:
        drows.append("  {.name = %s, .grammar = %s, .initial = %s, .at_rule = %d, .kind = %d}," % (
            cstr(pool.add(d["name"])), cstr(pool.add(d["grammar"])), cstr(pool.add(d["initial"])), d["at"], d["kind"]))
    ac = header("The at-rules and their descriptors.", mh, "") + """\
const size_t gtext_css_tbl_at_rule_count = %d;
const css_tbl_at_rule gtext_css_tbl_at_rules[%d] = {
%s
};

const size_t gtext_css_tbl_descriptor_count = %d;
const css_tbl_descriptor gtext_css_tbl_descriptors[%d] = {
%s
};
""" % (len(b["atrows"]), len(b["atrows"]), "\n".join(arows), len(b["descrows"]), len(b["descrows"]), "\n".join(drows))
    files["css_tables_atrules.c"] = ac

    # selectors, keywords, units, functions
    srows = ["  {.name = %s, .syntax = %s, .source = %s}," % (cstr(pool.add(s["name"])), cstr(pool.add(s["syntax"])), cstr(pool.add(s["source"]))) for s in b["sels"]]
    krows = ["  %s," % cstr(pool.add(k)) for k in b["keywords"]]
    urows = ["  {%s, %s}," % (cstr(pool.add(n)), cstr(pool.add(m))) for n, m in b["units"]]
    frows = ["  %s," % cstr(pool.add(f)) for f in b["funcs"]]
    commit = open(os.path.join(HERE, "WEBREF_COMMIT")).read().strip()
    mc = header("The selectors, the keywords, the units and functions of the Values definitions.", mh, "") + """\
const size_t gtext_css_tbl_selector_count = %d;
const css_tbl_selector gtext_css_tbl_selectors[%d] = {
%s
};

const size_t gtext_css_tbl_keyword_count = %d;
const css_tstr gtext_css_tbl_keywords[%d] = {
%s
};

const size_t gtext_css_tbl_unit_count = %d;
const css_tbl_unit gtext_css_tbl_units[%d] = {
%s
};

const size_t gtext_css_tbl_function_count = %d;
const css_tstr gtext_css_tbl_functions[%d] = {
%s
};

const char gtext_css_tbl_pin_commit[] = "%s";
""" % (len(b["sels"]), len(b["sels"]), "\n".join(srows), len(b["keywords"]), len(b["keywords"]), "\n".join(krows),
       len(b["units"]), len(b["units"]), "\n".join(urows), len(b["funcs"]), len(b["funcs"]), "\n".join(frows), commit)
    files["css_tables_misc.c"] = mc

    # the pool last: every file above has added its strings
    if len(pool.data) >= 1 << 31:
        die("the string pool is %d bytes; the offsets are 32 bits" % len(pool.data))
    files["css_tables_pool.c"] = header("The string pool: every string of the tables, NUL-terminated.", mh, "") + (
        "const size_t gtext_css_tbl_pool_size = %d;\n" % len(pool.data)) + char_array("gtext_css_tbl_pool", pool.data, "offset 0 is the empty string")

    os.makedirs(outdir, exist_ok=True)
    for name, text in sorted(files.items()):
        with open(os.path.join(outdir, name), "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    return files


def wrap(nums):
    if not nums:
        return "  0"
    lines, cur = [], "  "
    for x in nums:
        item = "%d, " % x
        if len(cur) + len(item) > 118:
            lines.append(cur.rstrip())
            cur = "  "
        cur += item
    lines.append(cur.rstrip())
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(ROOT, "src", "css", "tables"))
    ap.add_argument("--data", default=wm.DATA)
    ap.add_argument("--manifest", default=None, help="the manifest the data is checked against (default tools/css/WEBREF_SHA256)")
    args = ap.parse_args()
    model = wm.load(args.data, args.manifest)
    built = build(model)
    emit(built, args.out)
    sys.stdout.write("gen-css-tables: %d properties (%d aliases), %d types and functions, %d at-rules, %d descriptors, %d selectors, %d keywords\n" % (
        len(built["names"]), sum(1 for p in built["props"].values() if p["alias_of"]), len(built["types"]), len(built["atrows"]),
        len(built["descrows"]), len(built["sels"]), len(built["keywords"])))


if __name__ == "__main__":
    main()
