#!/usr/bin/env python3
"""The population of the CSS property differential (tools/oracle/css_prop_diff.py): which (property, value) items are asked.

    css_prop_gen.py --seed N --count M [--chunk K/T] [--ours BINARY] [--wpt DIR] [--out FILE]

A population is a pure function of the seed, the count, the chunk, the pin (the registry compiled into the library) and the pinned
web-platform-tests vectors; the same arguments make the same file on every host. One item is a line `property TAB value TAB origin`
(`\\\\`, `\\n` and `\\t` written escaped, as css_prop_ours.c reads them).

What is in it, in this order:

* the **controls** (CONTROLS below): items whose classification is known in advance. A run that classifies one of them any other way
  stops: a differential that cannot tell what it knows is not measuring.
* the **witnesses** of the divergence list (tools/oracle/css_prop_witnesses.txt), each of which is checked against the answers every run.
* for every decidable property, `initial` (the **probe**: every engine that has the property accepts a CSS-wide keyword, so a refusal
  is "the engine does not have the property") and its **witness** (the cheapest sentence of its compiled grammar). That is the
  **core** scope, the default: the part whose divergences the list tools/oracle/css_prop_divergences.txt classifies.
* in the **full** scope, which is what a hunt asks (CSS_PROP_ORACLE_SCOPE=full), also **edges**: the witness and a few sentences that
  hold numbers, each number in turn replaced by the bound of its range, one past it, zero, the extremes, and the number 1 in every unit
  of its type (`css-prop-ours --edges`); **sentences sampled from the grammar** by `css-prop-ours --sample` (the alternatives, repetitions and
  orders chosen at random, numbers drawn from common values and from the boundaries of ranges), the edges at level 2 (every edge,
  and the number 1 in every unit of the type), and every **extracted web-platform-tests vector** that names a property (valid, invalid,
  shorthand and computed-value vectors: the specified value is the item), through the extractor the vector gate uses.
* in the full scope, then, up to COUNT generated items in all, **variants** of those: case variants, white space variants, comments between tokens, and
  one-edit mutations (a character deleted, doubled, swapped, replaced by punctuation; a token deleted, doubled, replaced by one of
  another sentence of the same property; a sign flipped; a digit changed), which are mostly invalid, so that the refusals of the
  engines are measured and not only their acceptances.

The population is not shuffled: chunk K of T takes every T-th generated item starting at K (so that T chunks are the whole and none
shares an item), after the fixed parts, which every chunk has (they are small and are what the controls need).
"""

import argparse
import json
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# (property, value, what the differential must find). `agree-accept`: both engines accept and so does this library; `agree-refuse`:
# neither engine does and neither does this library; the third kind is a disagreement between the engines at the pinned versions, filled
# in by the run that found it (see CONTROL_DISAGREEMENTS in css_prop_diff.py, which holds what each engine said).
CONTROLS = [
    # accepted by all three and written alike
    ("color", "red", "agree-accept"),
    ("margin-top", "10px", "agree-accept"),
    ("display", "block", "agree-accept"),
    ("display", "BLOCK", "agree-accept"),
    ("font-weight", "700", "agree-accept"),
    ("width", "calc(1px + 2%)", "agree-accept"),
    ("color", "rgb(0 0 0 / 0.25)", "agree-accept"),
    ("color", "color(srgb 1 0 0 / 0.5)", "agree-accept"),
    ("line-height", "1.5", "agree-accept"),
    # refused by all three
    ("color", "notacolor", "agree-refuse"),
    ("width", "-1px", "agree-refuse"),
    ("max-width", "-1px", "agree-refuse"),
    ("margin-top", "10px 20px", "agree-refuse"),
    ("opacity", "1px", "agree-refuse"),
    ("z-index", "1.5", "agree-refuse"),
    ("line-height", "-1", "agree-refuse"),
    ("flex-grow", "-1", "agree-refuse"),
]

CAMEL = re.compile(r"([A-Z])")


def dash(name):
    """The property a `style[name]` attribute names (cssFloat, camelCase, webkit prefixes), as css_property_suite.py does."""
    if name == "cssFloat":
        return "float"
    if name.startswith("--") or "-" in name:
        return name if name.startswith("--") else name.lower()
    out = CAMEL.sub(lambda m: "-" + m.group(1).lower(), name)
    if out.startswith(("webkit-", "moz-", "ms-", "o-")):
        out = "-" + out
    return out


def esc(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def unesc(s):
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), s)


# ---- the transforms --------------------------------------------------------------------------------------------------

PUNCT = "()[]{},;/:!+-*#.%<>=&|~^@\"'\\"
TOKEN = re.compile(r"\s+|/\*.*?\*/|[A-Za-z_][\w-]*|[\d.]+(?:[eE][+-]?\d+)?[A-Za-z%]*|\S", re.S)


def tokens(value):
    return TOKEN.findall(value)


def t_case(rng, value, other):
    mode = rng.choice(("upper", "random", "title"))
    if mode == "upper":
        return value.upper()
    if mode == "title":
        return re.sub(r"\b([a-z])", lambda m: m.group(1).upper(), value)
    return "".join(c.upper() if c.isalpha() and rng.random() < 0.5 else c for c in value)


def t_space(rng, value, other):
    out = []
    for tok in tokens(value):
        if tok.isspace():
            out.append(rng.choice((" ", "  ", "\t", "\n", " \n ", "   ")))
        else:
            out.append(tok)
            if tok in ",/" and rng.random() < 0.5:
                out.append(rng.choice((" ", "")))
    s = "".join(out)
    if rng.random() < 0.5:
        s = rng.choice((" ", "\n", "\t ")) + s
    if rng.random() < 0.5:
        s = s + rng.choice((" ", "\n", "  "))
    if rng.random() < 0.3:
        s = s.replace("(", "( ", 1)
    if rng.random() < 0.3 and ")" in s:
        i = s.rindex(")")
        s = s[:i] + " " + s[i:]
    return s


def t_comment(rng, value, other):
    comment = rng.choice(("/**/", "/* c */", "/*x*/", "/* a b */", "/*\n*/"))
    toks = tokens(value)
    ws = [i for i, t in enumerate(toks) if t.isspace()]
    if ws and rng.random() < 0.7:
        i = rng.choice(ws)
        toks[i] = rng.choice((comment, " " + comment, comment + " ", " " + comment + " "))
        return "".join(toks)
    pos = rng.randrange(0, len(value) + 1)
    return value[:pos] + comment + value[pos:]


def t_mutate(rng, value, other):
    """One edit; `other` is another sentence of the same property (a source of tokens that are valid somewhere)."""
    kinds = ["delete-char", "double-char", "swap-chars", "replace-char", "insert-punct", "delete-token", "double-token", "transplant",
             "flip-sign", "change-digit", "strip-unit", "add-unit", "truncate"]
    kind = rng.choice(kinds)
    if not value:
        return rng.choice(("1", "a", "-"))
    pos = rng.randrange(0, len(value))
    toks = [t for t in tokens(value) if not t.isspace()]
    if kind == "delete-char":
        return value[:pos] + value[pos + 1:]
    if kind == "double-char":
        return value[:pos] + value[pos] + value[pos:]
    if kind == "swap-chars" and len(value) > 1:
        pos = min(pos, len(value) - 2)
        return value[:pos] + value[pos + 1] + value[pos] + value[pos + 2:]
    if kind == "replace-char":
        return value[:pos] + rng.choice(PUNCT + "0aZ -") + value[pos + 1:]
    if kind == "insert-punct":
        return value[:pos] + rng.choice(PUNCT) + value[pos:]
    if kind == "delete-token" and toks:
        t = rng.choice(toks)
        return value.replace(t, "", 1)
    if kind == "double-token" and toks:
        t = rng.choice(toks)
        return value.replace(t, t + " " + t, 1)
    if kind == "transplant" and toks:
        donor = [t for t in tokens(other) if not t.isspace()] or ["x"]
        return value.replace(rng.choice(toks), rng.choice(donor), 1)
    if kind == "flip-sign":
        m = re.search(r"[-+]?\d", value)
        if m:
            s = m.start()
            return value[:s] + ("" if value[s] in "-+" else "-") + value[s + (1 if value[s] in "-+" else 0):]
    if kind == "change-digit":
        ds = [i for i, c in enumerate(value) if c.isdigit()]
        if ds:
            i = rng.choice(ds)
            return value[:i] + rng.choice("0123456789") + value[i + 1:]
    if kind == "strip-unit":
        return re.sub(r"(\d)[a-zA-Z%]+", r"\1", value, count=1)
    if kind == "add-unit":
        return re.sub(r"(\d)(?![\d.a-zA-Z%])", lambda m: m.group(1) + rng.choice(("px", "%", "deg", "s", "em", "x", "fr", "foo")), value, count=1)
    if kind == "truncate":
        return value[:pos]
    return value + rng.choice(PUNCT)


TRANSFORMS = [("case", t_case, 1), ("space", t_space, 2), ("comment", t_comment, 2), ("mutate", t_mutate, 5)]


def variant(rng, base, pool):
    """A variant of a base item: one transform, or two."""
    prop, value, origin = base
    other = rng.choice(pool.get(prop, [value]))
    names, fns, weights = zip(*TRANSFORMS)
    n = 2 if rng.random() < 0.25 else 1
    tags = []
    for _ in range(n):
        i = rng.choices(range(len(fns)), weights)[0]
        value = fns[i](rng, value, other)
        tags.append(names[i])
    return prop, value, origin.split("+")[0] + "+" + "+".join(tags)


# ---- the inputs --------------------------------------------------------------------------------------------------------

def wpt_items(wpt_dir):
    """Every value the extracted vectors put into a property, in the order of the files."""
    out = []
    for kind in ("valid", "invalid", "shorthand", "computed"):
        path = os.path.join(wpt_dir, kind + ".json")
        if not os.path.exists(path):
            raise SystemExit("css_prop_gen: %s is not there (run tools/css/extract-wpt-props.py)" % path)
        for v in json.load(open(path, encoding="utf-8")):
            prop = v.get("prop")
            if not prop or v.get("value") is None or v.get("pseudo"):
                continue
            out.append((dash(prop), v["value"], "wpt-" + kind))
            if kind == "valid" and v.get("expected") and v["expected"] != v["value"]:
                out.append((dash(prop), v["expected"], "wpt-expected"))
    return out


def ours_lines(binary, args):
    run = subprocess.run([binary] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_prop_gen: %s %s failed: %s" % (binary, " ".join(args), run.stderr.decode("utf-8", "replace")[:300]))
    for line in run.stdout.decode("utf-8", "surrogateescape").split("\n"):
        if line:
            p, v, tag = line.split("\t")
            yield p, unesc(v), tag


def witness_items(path):
    out = []
    if path and os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) >= 3:
                out.append((f[1], unesc(f[2]), "divergence-witness"))
    return out


def population(seed, count, chunk, ours, wpt_dir, witnesses_path, scope="core", per_property=None):
    """(fixed items, generated items). See the module docstring for what the scopes hold."""
    rng = random.Random(seed)
    full = scope == "full"
    per = (per_property if per_property is not None else max(4, min(40, count // 600 + 4))) if full else 0
    sampled = list(ours_lines(ours, ["--sample", str(seed), str(per)]))
    edges = list(ours_lines(ours, ["--edges", "2"])) if full else []
    probes = []
    run = subprocess.run([ours, "--properties"], stdout=subprocess.PIPE, check=True)
    for line in run.stdout.decode("utf-8").split("\n"):
        f = line.split("\t")
        if len(f) == 5 and f[2] == "DECIDED":
            probes.append((f[0], "initial", "probe"))
    pool = {}
    for p, v, tag in sampled + edges:
        pool.setdefault(p, []).append(v)
    fixed = [(p, v, "control") for p, v, _ in CONTROLS]
    fixed += probes
    fixed += witness_items(witnesses_path)
    fixed += [(p, v, "grammar-" + tag) for p, v, tag in sampled]
    fixed += [(p, v, "grammar-" + tag) for p, v, tag in edges]
    if not full:
        # the core is the controls, the witnesses of the list, one probe and one witness for every property: the part whose divergences
        # the list classifies. Variants, edges, sampled sentences and the vectors of web-platform-tests are what a hunt adds.
        count = 0
    if full:
        fixed += wpt_items(wpt_dir)
    seen = set()
    items = []
    for it in fixed:
        if (it[0], it[1]) not in seen:
            seen.add((it[0], it[1]))
            items.append(it)
    generated = []
    bases = [it for it in items if it[2] in ("grammar-witness", "grammar-edge", "grammar-walk") or it[2].startswith("wpt-")]
    attempts = 0
    while len(generated) < count and bases and attempts < count * 20:
        attempts += 1
        it = variant(rng, bases[rng.randrange(len(bases))], pool)
        if (it[0], it[1]) not in seen and it[1] != "":
            seen.add((it[0], it[1]))
            generated.append(it)
    k, t = chunk
    return items, generated[k::t]


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=3000)
    ap.add_argument("--chunk", default="0/1")
    ap.add_argument("--ours", default=os.path.join(ROOT, "build", "css-prop-ours"))
    ap.add_argument("--wpt", default=os.path.join(ROOT, "build", "css-property-vectors"))
    ap.add_argument("--witnesses", default=os.path.join(HERE, "css_prop_witnesses.txt"))
    ap.add_argument("--scope", default=os.environ.get("CSS_PROP_ORACLE_SCOPE", "core"), choices=("core", "full"))
    ap.add_argument("--out", default="-")
    args = ap.parse_args(argv)
    k, t = (int(x) for x in args.chunk.split("/"))
    if not 0 <= k < t:
        raise SystemExit("css_prop_gen: --chunk K/T wants 0 <= K < T")
    fixed, generated = population(args.seed, args.count, (k, t), args.ours, args.wpt, args.witnesses, args.scope)
    out = sys.stdout if args.out == "-" else open(args.out, "w", encoding="utf-8")
    for p, v, origin in fixed + generated:
        out.write("%s\t%s\t%s\n" % (esc(p), esc(v), origin))
    if out is not sys.stdout:
        out.close()
    sys.stderr.write("css_prop_gen: seed %d, %d fixed items and %d generated (chunk %d of %d)\n" % (args.seed, len(fixed), len(generated), k, t))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
