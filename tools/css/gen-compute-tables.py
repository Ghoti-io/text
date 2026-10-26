#!/usr/bin/env python3
"""Generate the computed-value tables of the CSS layer, and hold every property of the pin to its classification.

The pin (w3c/webref, through gen-css-tables.py) gives each property a ``computedValue`` sentence. This script reads

  tools/css/computed_phrases.tsv   the closed set of sentences the machine classifies (identity, by-type, shorthand, or "rule")
  tools/css/computed_rules.tsv     the properties with an algorithm of their own, those the pin decides wrongly or not at all, the ones that are not implemented
  tools/css/compute_order.tsv      the properties that are computed first, in order, with what each reads
  tools/css/svg_presentation.txt   the SVG 2 presentation attributes, which (with every inherited property) must be computable
  tools/css/shorthand_rules.tsv    how each shorthand is split

and writes ``src/css/tables/css_tables_compute.c``: for every property of the registry (in the sorted order of the pin, the order of its ids) the
class of its computed value, its algorithm, its shorthand kind, whether it is required and whether its algorithm is partial, and the position in
the order of computation.

It fails (exit 1, every problem listed) when

  * a property of the pin is classified by no phrase and no row, or by a "rule" phrase and no row;
  * a phrase or a row or a shorthand rule names nothing in the pin (stale), or a row is there twice;
  * a row's class or algorithm is not an enumerator of css_computed.h, a sentence is not the pin's (or does not say ``draft:URL:`` and quote one),
    a unimplemented reason is not a sentence;
  * a required property (inherited, an SVG 2 presentation attribute, or a longhand of a shorthand in that list) is listed unimplemented;
  * the order lists a property twice, one that does not exist, an alias or shorthand, or a property that reads one that is not above it;
  * (``--check``) the committed table is not what this writes;
  * (``--depth LEDGER``) the ``computes`` column of the depth ledger is not what the classes say.

``--controls`` shows each of those fail: it makes the mistake in a copy of the inputs and requires the report.

Usage:
    gen-compute-tables.py [--out FILE] [--check] [--controls] [--depth LEDGER [--write-depth]]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import copy
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import webref_model as wm  # noqa: E402

_spec = importlib.util.spec_from_file_location("gen_css_tables", os.path.join(HERE, "gen-css-tables.py"))
gen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen)

HEADERS = os.path.join(ROOT, "include", "ghoti.io", "text", "css")
OUT_DEFAULT = os.path.join(ROOT, "src", "css", "tables", "css_tables_compute.c")


def die(msg):
    sys.stderr.write("gen-compute-tables: %s\n" % msg)
    raise SystemExit(1)


def header_enum(header, name, prefix):
    text = open(os.path.join(HEADERS, header), encoding="utf-8").read()
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)   # a comment may hold a brace (`{1,4}`)
    m = re.search(r"typedef enum \{([^}]*)\} %s;" % name, text, re.S)
    if not m:
        die("%s has no enum %s" % (header, name))
    body = m.group(1)
    names = [x.strip().split("=")[0].strip() for x in body.split(",") if x.strip()]
    out = {}
    for i, n in enumerate(names):
        if not n.startswith(prefix):
            die("enumerator %s of %s lacks the prefix %s" % (n, name, prefix))
        out[n[len(prefix):]] = i
    return out


def read_tsv(path, columns, what):
    rows = []
    with open(path, encoding="utf-8") as handle:
        for n, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            f = line.split("\t")
            if len(f) != columns:
                die("%s:%d: %d columns, %s" % (path, n, len(f), what))
            rows.append((n, f))
    return rows


class Inputs:
    """Everything the classification reads, in a form the controls can edit."""

    def __init__(self, props, names):
        self.props = props
        self.names = names
        self.phrases = {}      # phrase -> class
        self.rules = []        # (line, [property, class, algorithm, fidelity, initial, sentence])
        self.order = []        # (line, [property, depends, why])
        self.svg = []
        self.shorthand = []    # (line, [name, kind, note])
        self.enums = {}

    @staticmethod
    def load(props, names, root=HERE):
        i = Inputs(props, names)
        for n, f in read_tsv(os.path.join(root, "computed_phrases.tsv"), 2, "class TAB phrase"):
            if f[1] in i.phrases:
                die("computed_phrases.tsv:%d: the phrase is there twice" % n)
            i.phrases[f[1]] = f[0]
        i.rules = read_tsv(os.path.join(root, "computed_rules.tsv"), 6,
                           "property TAB class TAB algorithm TAB fidelity TAB initial TAB sentence")
        i.order = read_tsv(os.path.join(root, "compute_order.tsv"), 3, "property TAB reads TAB why")
        i.svg = [l.strip() for l in open(os.path.join(root, "svg_presentation.txt"), encoding="utf-8") if l.strip() and not l.startswith("#")]
        i.shorthand = read_tsv(os.path.join(root, "shorthand_rules.tsv"), 3, "shorthand TAB kind TAB note")
        i.enums = {
            "class": header_enum("css_computed.h", "GTEXT_CSS_Compute_Class", "GTEXT_CSS_COMPUTE_"),
            "algo": header_enum("css_computed.h", "GTEXT_CSS_Compute_Algorithm", "GTEXT_CSS_ALGO_"),
            "sh": header_enum("css_shorthand.h", "GTEXT_CSS_Shorthand_Kind", "GTEXT_CSS_SHORTHAND_"),
        }
        return i


CLASS_OF_ROW = {"algorithm": "ALGORITHM", "identity": "IDENTITY", "by-type": "BY_TYPE", "custom": "CUSTOM", "unimplemented": "UNIMPLEMENTED"}
CLASS_OF_PHRASE = {"identity": "IDENTITY", "by-type": "BY_TYPE", "shorthand": "SHORTHAND"}


def classify(inp):
    """Returns (rows, problems): rows[name] = dict(klass, algo, sh, flags, order, initial)."""
    props, names = inp.props, inp.names
    problems = []
    E = inp.enums
    rules = {}
    for n, f in inp.rules:
        if f[0] in rules:
            problems.append("computed_rules.tsv:%d: %s is there twice" % (n, f[0]))
        rules[f[0]] = (n, f)
    used_phrases = set()
    used_rules = set()
    rows = {}
    for name in names:
        p = props[name]
        row = {"klass": None, "algo": "NONE", "sh": "UNIMPLEMENTED", "partial": False, "required": False, "initial": "", "order": 0}
        phrase = p.get("computed_value")
        if p["alias_of"]:
            row["klass"] = "ALIAS"
            if name in rules:
                problems.append("computed_rules.tsv:%d: %s is an alias; it is computed as %s" % (rules[name][0], name, p["alias_of"]))
        elif p["longhands"] or name == "all":
            row["klass"] = "SHORTHAND"
        elif name in rules:
            n, f = rules[name]
            used_rules.add(name)
            cls = CLASS_OF_ROW.get(f[1])
            if not cls:
                problems.append("computed_rules.tsv:%d: %s: the class %r is not algorithm, identity, by-type, custom or unimplemented" % (n, name, f[1]))
                cls = "UNIMPLEMENTED"
            row["klass"] = cls
            if cls == "ALGORITHM" or cls == "CUSTOM":
                if f[2] not in E["algo"] or f[2] == "NONE":
                    problems.append("computed_rules.tsv:%d: %s: %r is not an enumerator GTEXT_CSS_ALGO_*" % (n, name, f[2]))
                else:
                    row["algo"] = f[2]
            elif f[2] != "-":
                problems.append("computed_rules.tsv:%d: %s: a row of class %s has no algorithm" % (n, name, f[1]))
            if f[3].startswith("partial: ") and len(f[3]) > 28:
                row["partial"] = True
            elif f[3] != "exact" and cls != "UNIMPLEMENTED":
                problems.append("computed_rules.tsv:%d: %s: the fidelity is 'exact' or 'partial: ' and what is not done" % (n, name))
            if cls == "UNIMPLEMENTED":
                if len(f[5]) < 40 or len(f[5].split()) < 6:
                    problems.append("computed_rules.tsv:%d: %s: unimplemented without a reason (a sentence)" % (n, name))
            else:
                sentence = f[5]
                if sentence.startswith("draft:"):
                    m = re.match(r"draft:(https?://\S+): (.{20,})$", sentence)
                    if not m or len(m.group(2).split()) < 4:
                        problems.append("computed_rules.tsv:%d: %s: a 'draft:URL: sentence' needs a sentence of four words and twenty characters" % (n, name))
                elif phrase is None:
                    problems.append("computed_rules.tsv:%d: %s: the pin has no sentence for it; quote the draft with 'draft:URL: sentence'" % (n, name))
                elif sentence != phrase:
                    problems.append("computed_rules.tsv:%d: %s: the sentence is not the pin's (%r)" % (n, name, phrase))
            row["initial"] = "" if f[4] == "-" else f[4]
        elif phrase is not None and phrase in inp.phrases:
            used_phrases.add(phrase)
            cls = inp.phrases[phrase]
            if cls == "rule":
                problems.append("%s: its computed-value sentence %r says it has an algorithm, and computed_rules.tsv has no row for it" % (name, phrase))
                row["klass"] = "UNIMPLEMENTED"
            elif cls in CLASS_OF_PHRASE:
                row["klass"] = CLASS_OF_PHRASE[cls]
            else:
                problems.append("computed_phrases.tsv: the class %r of %r is not identity, by-type, shorthand or rule" % (cls, phrase))
                row["klass"] = "UNIMPLEMENTED"
        else:
            problems.append("%s: not classified (its computed-value sentence is %r and computed_rules.tsv has no row)" % (name, phrase))
            row["klass"] = "UNIMPLEMENTED"
        rows[name] = row
    for ph, cls in inp.phrases.items():
        if ph not in used_phrases and not any(props[n].get("computed_value") == ph for n in names):
            problems.append("computed_phrases.tsv: the phrase %r is the sentence of no property of the pin (stale)" % ph)
    for name, (n, f) in rules.items():
        if name not in props:
            problems.append("computed_rules.tsv:%d: %s is not a property of the pin (stale)" % (n, name))
        elif name not in used_rules and not props[name]["alias_of"] and not (props[name]["longhands"] or name == "all"):
            problems.append("computed_rules.tsv:%d: %s is not used (stale)" % (n, name))
        elif name not in used_rules:
            problems.append("computed_rules.tsv:%d: %s is a shorthand or an alias; its row would never be read (stale)" % (n, name))

    # shorthands
    shorts = {}
    for n, f in inp.shorthand:
        if f[0] in shorts:
            problems.append("shorthand_rules.tsv:%d: %s is there twice" % (n, f[0]))
        shorts[f[0]] = (n, f)
        if f[0] not in props:
            problems.append("shorthand_rules.tsv:%d: %s is not a property of the pin (stale)" % (n, f[0]))
        elif props[f[0]]["alias_of"] or not (props[f[0]]["longhands"] or f[0] == "all"):
            problems.append("shorthand_rules.tsv:%d: %s is not a shorthand of the pin (an alias follows its target)" % (n, f[0]))
        elif f[1] not in E["sh"]:
            problems.append("shorthand_rules.tsv:%d: %s: %r is not an enumerator GTEXT_CSS_SHORTHAND_*" % (n, f[0], f[1]))
        elif f[1] == "HOOK":
            problems.append("shorthand_rules.tsv:%d: %s: HOOK is a caller's shorthand, not a rule" % (n, f[0]))
        else:
            rows[f[0]]["sh"] = f[1]
        if f[1] == "UNIMPLEMENTED" and len(f[2]) < 30:
            problems.append("shorthand_rules.tsv:%d: %s is unimplemented and says nothing why" % (n, f[0]))
    for name in names:
        p = props[name]
        if (p["longhands"] or name == "all") and not p["alias_of"] and name not in shorts:
            problems.append("%s: a shorthand of the pin with no row in shorthand_rules.tsv" % name)

    # required: inherited, SVG 2 presentation attributes (alias -> target, shorthand -> its longhands)
    req = set()

    def need(n, trail=()):
        if n not in props or n in req or n in trail:
            return
        req.add(n)
        if props[n]["alias_of"]:
            need(props[n]["alias_of"], trail + (n,))
        for t in props[n]["longhands"] + props[n].get("resets", []):
            need(t, trail + (n,))
    for name in names:
        p = props[name]
        if p["inherited_final"] == 2 and not p["alias_of"] and not p["longhands"]:  # GTEXT_CSS_INHERIT_YES
            need(name)
    for name in inp.svg:
        if name not in props:
            problems.append("svg_presentation.txt: %s is not a property of the pin" % name)
        else:
            need(name)
    for name in req:
        rows[name]["required"] = True
        if rows[name]["klass"] == "UNIMPLEMENTED":
            problems.append("%s: required (inherited, or an SVG 2 presentation attribute, or a longhand of one) and listed unimplemented" % name)

    # the order
    seen = {}
    for n, f in inp.order:
        name = f[0]
        if name in seen:
            problems.append("compute_order.tsv:%d: %s is there twice" % (n, name))
            continue
        pos = len(seen) + 1
        seen[name] = pos
        if name not in props:
            problems.append("compute_order.tsv:%d: %s is not a property of the pin" % (n, name))
            continue
        if props[name]["alias_of"] or props[name]["longhands"]:
            problems.append("compute_order.tsv:%d: %s is an alias or a shorthand (name the property)" % (n, name))
            continue
        if rows[name]["klass"] in ("UNIMPLEMENTED",):
            problems.append("compute_order.tsv:%d: %s is not implemented" % (n, name))
        rows[name]["order"] = pos
        for dep in ([] if f[1] == "-" else f[1].split(",")):
            if dep not in props:
                problems.append("compute_order.tsv:%d: %s reads %s, which is not a property of the pin" % (n, name, dep))
            elif dep not in seen or seen[dep] >= pos:
                problems.append("compute_order.tsv:%d: %s reads %s, which is not above it (a cycle, or a property computed after what reads it)" % (n, name, dep))
    if len(seen) >= 65535:
        problems.append("compute_order.tsv: more than 65534 properties")
    return rows, problems


def fnv1a(names):
    h = 0xcbf29ce484222325
    for n in names:
        for b in n.encode("utf-8") + b"\0":
            h ^= b
            h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def emit(inp, rows, path):
    E = inp.enums
    pool = gen.Pool()
    pool.data += b"\0"
    lines = []
    for name in inp.names:
        r = rows[name]
        init = pool.add(r["initial"])
        flags = (1 if r["required"] else 0) | (2 if r["partial"] else 0)
        lines.append("  {%d, %d, %d, %d, %d, %d, %d}, /* %s */" % (
            E["class"][r["klass"]], E["algo"][r["algo"]], E["sh"][r["sh"]], flags, r["order"], init[1], init[0], name))
    order = [n for _, (n, _, _) in sorted(((rows[n]["order"], (n, 0, 0)) for n in inp.names if rows[n]["order"]))]
    with open(os.path.join(HERE, "WEBREF_COMMIT")) as h:
        commit = h.read().strip()
    out = [gen.LICENSE, """
/**
 * @file
 *
 * GENERATED by tools/css/gen-compute-tables.py from the classification of the computed-value sentences of w3c/webref (branch curated, commit %s):
 * tools/css/computed_phrases.tsv, computed_rules.tsv, compute_order.tsv, svg_presentation.txt and shorthand_rules.tsv, and the enumerators of
 * css_computed.h and css_shorthand.h. Do not edit.
 *
 * Regenerate with:  tools/css/gen-compute-tables.py
 * Verify with:      make check-css-compute
 */

#include "../css_compute_tables.h"

/* the names of the registry's generated properties, in order, hashed: css_compute_tables.h compares it with the registry's */
const uint64_t gtext_css_tbl_compute_names_hash = 0x%016xULL;

const size_t gtext_css_tbl_compute_count = %d;

""" % (commit, fnv1a(inp.names), len(inp.names))]
    out.append("/* one row for each property of the pin in the order of its ids: class, algorithm, shorthand kind, flags, position in the order of computation (0: none),\n"
               "   and the initial value override (length, offset into the pool) */\n")
    out.append("const css_tbl_compute gtext_css_tbl_compute[%d] = {\n%s\n};\n\n" % (len(inp.names), "\n".join(lines)))
    out.append(gen.char_array("gtext_css_tbl_compute_pool", pool.data, "initial value overrides"))
    out.append("\nconst size_t gtext_css_tbl_compute_pool_size = %d;\n" % len(pool.data))
    out.append("\n/* the properties computed first, by property index + 1, in order */\nconst size_t gtext_css_tbl_compute_order_count = %d;\n" % len(order))
    idx = {n: i + 1 for i, n in enumerate(inp.names)}
    out.append("const uint32_t gtext_css_tbl_compute_order[%d] = {%s};\n" % (max(len(order), 1), ", ".join(str(idx[n]) for n in order) or "0"))
    text = "".join(out)
    return text


def build_inputs():
    model = wm.load()
    built = gen.build(model)
    return Inputs.load(built["props"], built["names"])


def depth_check(inp, rows, ledger, write):
    problems = []
    lines = open(ledger, encoding="utf-8").read().split("\n")
    changed = 0
    out = []
    for line in lines:
        if line and not line.startswith("#"):
            c = line.split("\t")
            if c[0] == "property" and c[1] in rows:
                want = "0" if rows[c[1]]["klass"] == "UNIMPLEMENTED" else "1"
                if c[4] != want:
                    if c[4] == "1" and not write:
                        problems.append("REGRESSION: property %s: computes was 1 and its class is unimplemented now" % c[1])
                    elif not write:
                        problems.append("OUT OF DATE: property %s: computes is %s in the ledger and the classification says %s (run with --write-depth)" % (c[1], c[4], want))
                    c[4] = want
                    changed += 1
                    line = "\t".join(c)
            elif c[0] == "property" and c[1] not in rows:
                problems.append("STALE: property %s is in the ledger and not in the pin" % c[1])
        out.append(line)
    seen = {l.split("\t")[1] for l in lines if l.startswith("property\t")}
    for n in inp.names:
        if n not in seen:
            problems.append("MISSING: property %s has no row in the ledger" % n)
    if write:
        with open(ledger, "w", encoding="utf-8", newline="\n") as h:
            h.write("\n".join(out))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=OUT_DEFAULT)
    ap.add_argument("--check", action="store_true", help="fail unless the committed table is what this writes")
    ap.add_argument("--controls", action="store_true", help="show each check fail")
    ap.add_argument("--depth", default=None)
    ap.add_argument("--write-depth", action="store_true")
    args = ap.parse_args()
    inp = build_inputs()
    rows, problems = classify(inp)
    if problems:
        sys.stderr.write("gen-compute-tables: %d problems\n" % len(problems))
        for p in problems[:80]:
            sys.stderr.write("  " + p + "\n")
        return 1
    text = emit(inp, rows, args.out)
    status = 0
    if args.check:
        have = open(args.out, encoding="utf-8").read() if os.path.exists(args.out) else None
        if have != text:
            sys.stderr.write("gen-compute-tables: %s is not what tools/css/gen-compute-tables.py writes (a hand edit, or the inputs changed: run the generator)\n" % args.out)
            status = 1
    else:
        with open(args.out, "w", encoding="utf-8", newline="\n") as h:
            h.write(text)
    if args.depth:
        probs = depth_check(inp, rows, args.depth, args.write_depth)
        for p in probs[:40]:
            sys.stderr.write("  " + p + "\n")
        if probs:
            status = 1
    by_class = {}
    for r in rows.values():
        by_class[r["klass"]] = by_class.get(r["klass"], 0) + 1
    sys.stdout.write("gen-compute-tables: %d properties: %s; %d required; %d partial; %d in the order table\n" % (
        len(inp.names), ", ".join("%s %d" % (k.lower(), v) for k, v in sorted(by_class.items())),
        sum(1 for r in rows.values() if r["required"]), sum(1 for r in rows.values() if r["partial"]), sum(1 for r in rows.values() if r["order"])))
    if args.controls:
        status |= controls(inp)
    return status


def controls(inp):
    """Make each mistake in a copy of the inputs and require the report."""
    bad = 0

    def expect(what, edit, want):
        nonlocal bad
        c = copy.deepcopy(inp)
        edit(c)
        _, probs = classify(c)
        if not any(want in p for p in probs):
            sys.stderr.write("control %s: not caught (wanted %r; got %r)\n" % (what, want, probs[:3]))
            bad = 1
        else:
            print("control %s: caught" % what)

    def first_rule(c, cls):
        for i, (n, f) in enumerate(c.rules):
            if f[1] == cls:
                return i

    def drop_rule(c):
        for i, (n, f) in enumerate(c.rules):
            if f[1] == "algorithm" and c.phrases.get(c.props[f[0]].get("computed_value")) == "rule":
                del c.rules[i]
                return
    expect("a property with an algorithm and no row", drop_rule, "has an algorithm")

    def drop_phrase(c):
        for ph, cls in list(c.phrases.items()):
            if cls == "identity":
                del c.phrases[ph]
                return
    expect("a sentence that is not classified", drop_phrase, "not classified")

    def stale_phrase(c):
        c.phrases["a sentence of no property at all"] = "identity"
    expect("a stale phrase", stale_phrase, "stale")

    def stale_rule(c):
        c.rules.append((9999, ["no-such-property", "by-type", "-", "exact", "-", "x"]))
    expect("a stale row", stale_rule, "stale")

    def required_unimplemented(c):
        c.rules.append((9998, ["font-kerning", "unimplemented", "-", "-", "-", "the computed value of this is not decided here yet at all (a test)"]))
        c.rules = [r for r in c.rules if not (r[1][0] == "font-kerning" and r[1][1] != "unimplemented")]
    expect("a required property listed unimplemented", required_unimplemented, "required")

    def bad_algo(c):
        i = first_rule(c, "algorithm")
        c.rules[i][1][2] = "NO_SUCH_ALGORITHM"
    expect("an algorithm that is not an enumerator", bad_algo, "not an enumerator")

    def wrong_sentence(c):
        i = first_rule(c, "algorithm")
        c.rules[i][1][5] = "a sentence the pin does not have in it"
    expect("a sentence that is not the pin's", wrong_sentence, "not the pin's")

    def order_dup(c):
        c.order.append((9997, [c.order[0][1][0], "-", "again"]))
    expect("a property twice in the order", order_dup, "twice")

    def order_cycle(c):
        c.order[0] = (c.order[0][0], [c.order[0][1][0], c.order[1][1][0], "reads what is below it"])
    expect("a property reading one below it", order_cycle, "not above it")

    def short_missing(c):
        c.shorthand.pop(0)
    expect("a shorthand with no rule", short_missing, "no row in shorthand_rules.tsv")

    def short_stale(c):
        c.shorthand.append((9996, ["no-such-shorthand", "BOX", ""]))
    expect("a shorthand rule that names nothing", short_stale, "stale")

    def short_bad_kind(c):
        c.shorthand[0] = (c.shorthand[0][0], [c.shorthand[0][1][0], "TRIANGLE", ""])
    expect("a shorthand kind that is not an enumerator", short_bad_kind, "not an enumerator")

    # the committed table: one byte of it edited must not equal what the generator writes
    rows, _ = classify(inp)
    text = emit(inp, rows, "")
    committed = open(OUT_DEFAULT, encoding="utf-8").read() if os.path.exists(OUT_DEFAULT) else ""
    if text != committed:
        sys.stderr.write("control: the committed table differs from the generator's output; cannot show a hand edit is caught\n")
        bad = 1
    else:
        edited = committed.replace("{", "{ ", 1)
        if edited == text:
            sys.stderr.write("control: the edit of the table did not change it\n")
            bad = 1
        else:
            print("control a hand-edited table: caught")
    return bad


if __name__ == "__main__":
    sys.exit(main())
