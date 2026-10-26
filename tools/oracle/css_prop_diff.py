#!/usr/bin/env python3
"""The CSS property differential: this library against Chromium (Blink) and Firefox (Gecko, style engine Stylo).

    css_prop_diff.py --work DIR [--seed N] [--count M] [--stage populate|ours|browsers|classify|all]
                     [--ours BINARY] [--replay DIR] [--plant NAME] [--write-list FILE]

Every item of a population (css_prop_gen.py: grammar witnesses and sampled sentences, boundary numbers and units, case, white space and
comment variants, one-edit mutations, every extracted web-platform-tests vector) is asked of three readers: this library
(css-prop-ours, `gtext_css_parse_property_text` and the serializer), and the two browsers through one WebDriver BiDi driver in a pinned
image (containers/cssprop): `style.setProperty` then `getPropertyValue`, and `CSS.supports`. The three answers of an item are an
acceptance (the property was set, or not) and, for an acceptance, a serialization.

**What is a divergence.** An item is a divergence when the three do not agree:

  acc:ABC     the acceptances, ours (A), Chromium (B) and Firefox (C), as 0 and 1, when they are not all alike:
              acc:100 (ours accepts, both browsers refuse), acc:011 (ours refuses, both accept), acc:010 and acc:001 (the browsers
              disagree and ours sides with one of them), acc:110 and acc:101 (ours and one browser accept), acc:000/acc:111 do not occur;
  ser:...     the acceptances are such that two or more readers accept and their serializations are not all the same string; the pattern
              names which readers wrote alike (ser:A|B|C all differ, ser:A=B|C, ser:A=C|B, ser:A|B=C);
  sup:E       an engine's `CSS.supports` and its `setProperty` disagree about the item (sup:chromium, sup:firefox).

**What explains it.** The key of a divergence is `<pattern>:<property>:<feature>`. The feature is the smallest item of the same pattern and
property that has the divergence, written as tokens with numbers, dimensions and strings abstracted (`N`, `D`, `S`): every larger item
of that pattern and property whose tokens include the feature's is explained by it and counts for it. It is the argument of the HTML
differentials' feature sets, made with the tokens of the value instead of the nodes of a tree: a divergence is not only counted but
named, and a defect hiding in an item that has another divergence in it is limited to the properties and patterns that are listed.
tools/oracle/css_prop_divergences.txt classifies each key (class, reason), and the gate is this program: it fails (exit 3) on a key that
is not listed, a listed key that does not occur, a key whose count is not the listed one (the count is the population's signature, which
is why the seed and the count are part of the list's description), a class `ours` (never allowed in a finished list: when the two
engines agree and this library differs, it is this library's until the served draft says otherwise), a witness that does not hold, and a
control that is classified any other way.

**Witnesses and controls.** tools/oracle/css_prop_witnesses.txt gives every listed key a small item with the answer worked out from the served
draft for this library and what the engines are expected to say; the run asks it and fails if the answers are different. The controls
(css_prop_gen.CONTROLS) are items that all three readers accept and items that all three refuse, which must be classified as agreed, and
the disagreements between the engines recorded in css_prop_controls.txt, which must be classified as the engines then said.

--replay DIR reads the browsers' answers from an earlier run's directory instead of asking the browsers: this library's side is run again,
so a library edited in a copy (the plants of CSS_PROPERTY_ORACLE_PLANTS) is measured against browsers that did not change. --plant NAME
corrupts one thing in the data on the way (an engine's answer, the list, a witness) so that the gate can be shown to fail.
"""

import argparse
import collections
import hashlib
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import css_prop_gen  # noqa: E402
import oracle_env  # noqa: E402

CLASSES = ("standard-newer", "reference-old", "reference-wrong", "legacy-compat", "ours")
ENGINES = ("chromium", "firefox")
LIST = os.path.join(HERE, "css_prop_divergences.txt")
WITNESSES = os.path.join(HERE, "css_prop_witnesses.txt")
CONTROLS = os.path.join(HERE, "css_prop_controls.txt")
DRIVER_FILES = ("driver.mjs", "package.json", "package-lock.json")
GATE_EXIT = 3


# ---- the readers ---------------------------------------------------------------------------------------------------------

def read_population(path):
    items = []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line:
            continue
        p, v, origin = line.split("\t")
        items.append((css_prop_gen.unesc(p), css_prop_gen.unesc(v), origin))
    return items


def write_population(path, items):
    with open(path, "w", encoding="utf-8") as f:
        for p, v, origin in items:
            f.write("%s\t%s\t%s\n" % (css_prop_gen.esc(p), css_prop_gen.esc(v), origin))


def run_ours(binary, items):
    """Ours: ('OK', serialization) | ('NO',) | ('UNDECIDED',) | ('UNKNOWN',) | ('ERR', text) for each item."""
    data = "".join("%s\t%s\n" % (css_prop_gen.esc(p), css_prop_gen.esc(v)) for p, v, _ in items).encode("utf-8", "surrogateescape")
    run = subprocess.run([binary], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_prop_diff: %s exited %d: %s" % (binary, run.returncode, run.stderr.decode("utf-8", "replace")[:300]))
    lines = run.stdout.decode("utf-8", "surrogateescape").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if len(lines) != len(items):
        raise SystemExit("css_prop_diff: %s answered %d lines to %d items" % (binary, len(lines), len(items)))
    out = []
    for line in lines:
        f = line.split("\t", 1)
        if f[0] == "OK":
            out.append(("OK", css_prop_gen.unesc(f[1])))
        else:
            out.append((f[0],) + tuple(f[1:]))
    return out


def driver_sha():
    h = hashlib.sha256()
    base = os.path.join(HERE, "containers", "cssprop")
    for name in DRIVER_FILES:
        h.update(open(os.path.join(base, name), "rb").read())
    return h.hexdigest()


def check_driver(version_line):
    """The image holds the driver of this repository: its printed hash is the hash of the files here."""
    m = re.search(r"driver ([0-9a-f]{64})", version_line)
    if not m:
        raise oracle_env.OracleUnavailable("cssprop: the version line names no driver hash: %r" % version_line)
    if m.group(1) != driver_sha():
        raise oracle_env.OracleUnavailable(
            "cssprop: the image was built from another copy of its driver (image %s, repository %s). Rebuild it: make oracle-images"
            % (m.group(1), driver_sha()))


ENGINE_PART = {"chromium": "chrome", "firefox": "firefox"}


def header_problems(headers):
    """The answers say what produced them (the driver writes it into the header of every run, from the binaries it ran), and it has to be the pin
    of containers/IMAGES: the versions and the four archive hashes, the driver of this repository, and the browser each session reported. A
    replay is held to it as a live run is, so answers from another browser build or another image are not classified as if they were these."""
    problems = []
    table = oracle_env.pins()
    if "cssprop" not in table:
        return ["HEADER: containers/IMAGES has no cssprop pin to hold the answers to"]
    want = table["cssprop"][1]
    named = dict(w.split(" ", 1) for w in (p.strip() for p in want.split(",")) if " " in w)
    for e in ENGINES:
        h = headers.get(e) or {}
        image = h.get("image", "")
        if not image:
            problems.append("HEADER: the %s answers carry no description of the image that made them (an older driver); ask the browsers again" % e)
            continue
        if want not in image:
            problems.append("HEADER: the %s answers were made by %r, and containers/IMAGES pins %r" % (e, image, want))
        m = re.search(r"driver ([0-9a-f]{64})", image)
        if not m or m.group(1) != driver_sha():
            problems.append("HEADER: the %s answers were made by another copy of the driver (%s, the repository has %s)"
                            % (e, m.group(1)[:12] if m else "none", driver_sha()[:12]))
        wanted = named.get(ENGINE_PART[e])
        if wanted and h.get("browser") != wanted:
            problems.append("HEADER: the %s session reported the browser %r and the pin is %s %s" % (e, h.get("browser"), ENGINE_PART[e], wanted))
    return problems


def run_engine(engine, items, batch_timeout=None):
    """The answers of one browser: a list of [accepted, value, cssText, supports, error], and its header."""
    data = "".join(json.dumps([p, v], ensure_ascii=False) + "\n" for p, v, _ in items).encode("utf-8", "surrogatepass")
    argv = oracle_env.command("cssprop", ["cssprop-driver", engine])
    run = subprocess.run(argv, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_prop_diff: the %s driver exited %d:\n%s" % (engine, run.returncode, oracle_env.reference_stderr(run.stderr.decode("utf-8", "replace"))[-1500:]))
    lines = run.stdout.decode("utf-8", "replace").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    header = json.loads(lines[0][1:])
    rows = [json.loads(x) for x in lines[1:]]
    if len(rows) != len(items):
        raise SystemExit("css_prop_diff: the %s driver answered %d rows to %d items" % (engine, len(rows), len(items)))
    return header, rows


# ---- tokens ----------------------------------------------------------------------------------------------------------------

_TOKEN = re.compile(r"""\s+|/\*.*?\*/|"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?[A-Za-z%]*|[A-Za-z_\-][\w\-]*\(?|#[\w-]+|\S""", re.S)


def abstract_tokens(value):
    """The tokens of a value with comments and white space gone and numbers, dimensions, strings and hashes abstracted."""
    out = []
    for t in _TOKEN.findall(value):
        if t.isspace() or t.startswith("/*"):
            continue
        if t[0] in "\"'":
            out.append("S")
        elif t[0] == "#":
            out.append("#H")
        elif re.match(r"[+-]?(\d|\.\d)", t):
            m = re.match(r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?(.*)$", t)
            unit = m.group(1).lower() if m else ""
            out.append("N" if not unit else ("P" if unit == "%" else "D"))
        else:
            out.append(t.lower())
    return out


# ---- the divergences -------------------------------------------------------------------------------------------------------

def engine_view(row):
    """(accepted, serialization) of a browser's row."""
    accepted = bool(row[0])
    ser = row[1] if row[1] else row[2]
    return accepted, ser


def support_of(items, answers):
    """{property: (chromium has it, firefox has it)} from the probes: every engine that has a property accepts `initial` for it."""
    out = {}
    for i, (prop, _value, origin) in enumerate(items):
        if origin == "probe":
            out[prop] = (bool(answers["chromium"][i][0]), bool(answers["firefox"][i][0]))
    return out


def patterns_of(ours, chromium, firefox, support):
    """The divergence patterns of one item (a list; usually zero or one).

    An engine that does not have the property takes no part: there is nothing to compare with, and that fact is the `support:` divergence
    of the property. Ours takes part when it can judge the property.
    """
    if ours[0] not in ("OK", "NO"):
        return ["ours-undecided"] if ours[0] == "UNDECIDED" else ["ours-" + ours[0].lower()]
    a = ours[0] == "OK"
    present = [(n, row) for n, row, has in (("B", chromium, support[0]), ("C", firefox, support[1])) if has]
    out = []
    if not present:
        return out
    views = {n: engine_view(row) for n, row in present}
    bits = {"A": a}
    bits.update({n: v[0] for n, v in views.items()})
    if len(set(bits.values())) > 1:
        out.append("acc:" + "".join(("1" if bits[n] else "0") if n in bits else "-" for n in "ABC"))
    sers = [(n, s) for n, ok, s in [("A", a, ours[1] if a else None)] + [(n, v[0], v[1]) for n, v in views.items()] if ok]
    if len(sers) >= 2 and len({s for _, s in sers}) > 1:
        groups = collections.OrderedDict()
        for n, s in sers:
            groups.setdefault(s, []).append(n)
        out.append("ser:" + "|".join("=".join(g) for g in groups.values()))
    for n, row in present:
        if bool(row[0]) != bool(row[3]):
            out.append("sup:" + ("chromium" if n == "B" else "firefox"))
    return out


def digest_of(items, ours, answers, members):
    """The signature of every item of a key, not only how many there are: the property, the value and the three answers of each, hashed. A key whose
    items are the same in number but not in what the library (or an engine) says of them changes it, and the list has to be looked at again."""
    h = hashlib.sha256()
    for line in sorted("%s\t%s\t%r\t%r\t%r" % (items[i][0], items[i][1], tuple(ours[i]), engine_view(answers["chromium"][i]), engine_view(answers["firefox"][i]))
                       for i in members):
        h.update(line.encode("utf-8", "surrogatepass") + b"\n")
    return h.hexdigest()[:16]


def classify(items, ours, answers):
    """key -> {count, rep, pattern, prop}. Smallest-first, as the module docstring says."""
    by_group = collections.defaultdict(list)
    undecided = collections.Counter()
    unanswered = []
    support = support_of(items, answers)
    features = {}
    for prop, (b, c) in sorted(support.items()):
        lacking = [n for n, has in (("chromium", b), ("firefox", c)) if not has]
        probe_index = next(i for i, it in enumerate(items) if it[0] == prop and it[2] == "probe")
        if lacking and ours[probe_index][0] == "OK":
            features["support:%s:%s" % ("+".join(lacking), prop)] = {"count": 1, "rep": probe_index, "pattern": "support:" + "+".join(lacking), "prop": prop,
                                                                       "digest": digest_of(items, ours, answers, [probe_index])}
    for i, (item, o) in enumerate(zip(items, ours)):
        if item[2] == "probe":
            continue
        rows = [answers[e][i] for e in ENGINES]
        if any(r[4].startswith("hang") for r in rows):
            unanswered.append(i)
            continue
        for pattern in patterns_of(o, rows[0], rows[1], support.get(item[0], (True, True))):
            if pattern.startswith("ours-"):
                undecided[(pattern, item[0])] += 1
                continue
            toks = abstract_tokens(item[1])
            by_group[(pattern, item[0])].append((len(toks), item[1], i, collections.Counter(toks)))
    for (pattern, prop), members in sorted(by_group.items()):
        members.sort(key=lambda m: (m[0], m[1], m[2]))
        feats = []
        for n, value, i, bag in members:
            for f in feats:
                if all(bag[t] >= c for t, c in f["bag"].items()):
                    f["count"] += 1
                    f["members"].append(i)
                    break
            else:
                feats.append({"rep": i, "bag": bag, "count": 1, "toks": abstract_tokens(value), "members": [i]})
        for f in feats:
            key = "%s:%s:%s" % (pattern, prop, " ".join(f["toks"]) if f["toks"] else "(empty)")
            features[key] = {"count": f["count"], "rep": f["rep"], "pattern": pattern, "prop": prop,
                             "digest": digest_of(items, ours, answers, f["members"])}
    return features, undecided, unanswered


def read_list(path):
    rows, problems = {}, []
    if not os.path.exists(path):
        return rows, ["%s is not there" % path]
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) != 5:
            problems.append("%s:%d: not key, count, class, digest, reason" % (path, n))
            continue
        key, count, cls, digest, reason = f
        if key in rows:
            problems.append("%s:%d: %s twice" % (path, n, key))
        if cls not in CLASSES:
            problems.append("%s:%d: class %r is not one of %s" % (path, n, cls, ", ".join(CLASSES)))
        if len(reason) < 30 or "browser behavior" in reason.lower() and len(reason) < 60:
            problems.append("%s:%d: the reason is missing, or is the blanket \"browser behavior\"" % (path, n))
        if "*" in key:
            problems.append("%s:%d: a key with a wildcard is a blanket" % (path, n))
        rows[key] = (count, cls, reason, digest)
    return rows, problems


def read_witnesses(path):
    """key -> [(property, value, ours expectation, reference expectation)]; an expectation is `OK=<text>` or `NO`."""
    out = collections.defaultdict(list)
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) == 5:
                out[f[0]].append((f[1], css_prop_gen.unesc(f[2]), f[3], f[4]))
    return out


def read_controls(path):
    """(property, value, chromium accepts, firefox accepts): the disagreements between the engines at the pinned versions."""
    out = []
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) == 4:
                out.append((f[0], css_prop_gen.unesc(f[1]), f[2] == "1", f[3] == "1"))
    return out


# ---- the gate ---------------------------------------------------------------------------------------------------------------

def gate(items, ours, answers, headers, list_path, args):
    witnesses_path, controls_path = args.witnesses, args.controls
    bad = header_problems(headers)
    index = {(p, v): i for i, (p, v, _) in enumerate(items)}

    # the controls: known in advance. All three accept and write the value alike, or all three refuse it.
    for p, v, kind in css_prop_gen.CONTROLS:
        i = index[(p, v)]
        want = kind == "agree-accept"
        views = [(ours[i][0] == "OK", ours[i][1] if ours[i][0] == "OK" else None)] + [engine_view(answers[e][i]) for e in ENGINES]
        accepted = tuple(x[0] for x in views)
        if accepted != (want, want, want) or (want and len({x[1] for x in views}) != 1):
            bad.append("CONTROL: %s: %s is not %s by all three, written alike (ours %r, chromium %r, firefox %r)" % (
                p, json.dumps(v), "accepted" if want else "refused", views[0], views[1], views[2]))
    for p, v, cb, fb in read_controls(controls_path):
        i = index.get((p, v))
        if i is None:
            bad.append("CONTROL: the engine-disagreement control %s: %s is not in the population" % (p, json.dumps(v)))
            continue
        got = (engine_view(answers["chromium"][i])[0], engine_view(answers["firefox"][i])[0])
        if got != (cb, fb):
            bad.append("CONTROL: %s: %s: the engines were recorded as chromium %s, firefox %s and now say %s, %s" % (
                p, json.dumps(v), cb, fb, *got))

    features, undecided, unanswered = classify(items, ours, answers)
    listed, problems = read_list(list_path)
    bad += ["LIST: " + p for p in problems]
    for key in sorted(set(features) - set(listed)):
        f = features[key]
        p, v, _ = items[f["rep"]]
        bad.append("UNLISTED: %s  count %d  e.g. %s: %s  ours %s | chromium %s | firefox %s" % (
            key, f["count"], p, json.dumps(v), ours[f["rep"]], engine_view(answers["chromium"][f["rep"]]), engine_view(answers["firefox"][f["rep"]])))
    for key in sorted(set(listed) - set(features)):
        bad.append("GONE: %s is listed and does not occur" % key)
    for key in sorted(set(listed) & set(features)):
        if int(listed[key][0]) != features[key]["count"]:
            bad.append("COUNT: %s occurs %d times and the list says %s" % (key, features[key]["count"], listed[key][0]))
    for key in sorted(set(listed) & set(features)):
        if listed[key][3] != features[key]["digest"]:
            bad.append("DIGEST: %s: the %d items of the key are not the ones the list pinned (it says %s, they hash to %s): an answer of this library or "
                       "of an engine changed for one of them; look at each and write the list again" % (key, features[key]["count"], listed[key][3], features[key]["digest"]))
    ours_keys = [k for k in listed if listed[k][1] == "ours" and k in features]
    if len(ours_keys) > args.max_ours:
        bad.append("OURS: %d keys are class ours (defects of this library) and the ceiling is %d; class ours is never allowed in a finished list, "
                   "and the ceiling only goes down (first: %s)" % (len(ours_keys), args.max_ours, ours_keys[0]))

    # the witnesses
    wit = read_witnesses(witnesses_path)
    for key in sorted(wit):
        if key not in listed:
            bad.append("WITNESS: %s is a witness of a key that is not listed" % key)
    for key in sorted(set(listed) - set(wit)):
        bad.append("WITNESS: %s has no witness (css_prop_witnesses.txt)" % key)
    for key, ws in sorted(wit.items()):
        for p, v, ours_want, ref_want in ws:
            i = index.get((p, v))
            if i is None:
                bad.append("WITNESS: %s: %s: %s is not in the population" % (key, p, json.dumps(v)))
                continue
            got_o = ("OK=" + ours[i][1]) if ours[i][0] == "OK" else ours[i][0]
            if got_o != ours_want:
                bad.append("WITNESS: %s: ours says %r for %s: %s, the witness says %r" % (key, got_o, p, json.dumps(v), ours_want))
            refs = []
            for e in ENGINES:
                ok, s = engine_view(answers[e][i])
                refs.append(("OK=" + s) if ok else "NO")
            if ref_want not in refs and ref_want != "-":
                bad.append("WITNESS: %s: the engines say %r for %s: %s, the witness says one of them says %r" % (key, refs, p, json.dumps(v), ref_want))
    return bad, features, listed, undecided, unanswered


def summarize(items, ours, answers, headers, features, listed, undecided, unanswered):
    n = len(items)
    acc = collections.Counter()
    for i in range(n):
        acc[(ours[i][0] == "OK", engine_view(answers["chromium"][i])[0], engine_view(answers["firefox"][i])[0])] += 1
    print("css property differential: %d items; %s" % (n, "; ".join("%s %s" % (e, headers[e].get("browser", "?")) for e in ENGINES)))
    print("  acceptance (ours, chromium, firefox): " + ", ".join("%s%s%s %d" % (("1" if a else "0"), ("1" if b else "0"), ("1" if c else "0"), cnt) for (a, b, c), cnt in sorted(acc.items())))
    by = collections.Counter(k.split(":", 2)[0] + ":" + k.split(":", 2)[1] if k.startswith(("acc:", "ser:", "sup:")) else k for k in features)
    print("  divergence keys: %d, items explained: %d (%d listed)" % (len(features), sum(f["count"] for f in features.values()), len(set(features) & set(listed))))
    print("  items ours cannot judge (undecided property): %d; items an engine did not answer: %d" % (sum(undecided.values()), len(unanswered)))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--seed", type=int, default=int(os.environ.get("CSS_PROP_ORACLE_SEED", "1")))
    ap.add_argument("--count", type=int, default=int(os.environ.get("CSS_PROP_ORACLE_COUNT", "3000")))
    ap.add_argument("--chunk", default=os.environ.get("CSS_PROP_ORACLE_CHUNK", "0/1"))
    ap.add_argument("--scope", default=os.environ.get("CSS_PROP_ORACLE_SCOPE", "core"), choices=("core", "full"))
    ap.add_argument("--stage", default="all")
    ap.add_argument("--ours", default=os.path.join(ROOT, "build", "css-prop-ours"))
    ap.add_argument("--wpt", default=os.path.join(ROOT, "build", "css-property-vectors"))
    ap.add_argument("--replay", default=None)
    ap.add_argument("--plant", default=None)
    ap.add_argument("--list", default=LIST)
    ap.add_argument("--witnesses", default=WITNESSES)
    ap.add_argument("--controls", default=CONTROLS)
    ap.add_argument("--write-list", default=None)
    ap.add_argument("--write-witnesses", default=None)
    ap.add_argument("--max-ours", type=int, default=int(os.environ.get("CSS_PROP_ORACLE_MAX_OURS", "0")))
    args = ap.parse_args(argv)
    work = args.work
    os.makedirs(work, exist_ok=True)
    stages = ("populate", "ours", "browsers", "classify") if args.stage == "all" else (args.stage,)
    pop_path = os.path.join(work, "population.tsv")
    ours_path = os.path.join(work, "ours.json")

    if "populate" in stages:
        k, t = (int(x) for x in args.chunk.split("/"))
        fixed, generated = css_prop_gen.population(args.seed, args.count, (k, t), args.ours, args.wpt, args.witnesses, args.scope)
        items = fixed + generated
        write_population(pop_path, items)
        print("css property differential: seed %d, %d fixed and %d generated items" % (args.seed, len(fixed), len(generated)), flush=True)
    items = read_population(pop_path)

    ours = None
    if "ours" in stages:
        ours = run_ours(args.ours, items)
        json.dump(ours, open(ours_path, "w"), ensure_ascii=False)
    if ours is None and ("classify" in stages):
        ours = [tuple(x) for x in json.load(open(ours_path))]

    answers = {}
    headers = {}
    if "browsers" in stages:
        try:
            line = oracle_env.provenance(["cssprop"])
            check_driver(oracle_env.version("cssprop"))
        except oracle_env.OracleUnavailable as why:
            return oracle_env.decline("css property differential", why)
        print(line, flush=True)
        for e in ENGINES:
            t0 = time.time()
            header, rows = run_engine(e, items)
            headers[e] = header
            answers[e] = rows
            json.dump({"header": header, "rows": rows}, open(os.path.join(work, e + ".json"), "w"), ensure_ascii=False)
            print("  %s %s: %d items in %.0f s" % (e, header.get("browser"), len(items), time.time() - t0), flush=True)
    if "classify" in stages:
        src = args.replay or work
        for e in ENGINES:
            if e not in answers:
                data = json.load(open(os.path.join(src, e + ".json")))
                headers[e], answers[e] = data["header"], data["rows"]
            if len(answers[e]) != len(items):
                raise SystemExit("css_prop_diff: %s has %d rows for %d items (a replay needs the same population)" % (e, len(answers[e]), len(items)))
        if args.plant:
            apply_plant(args.plant, items, ours, answers, headers)
        list_path = args.list
        bad, features, listed, undecided, unanswered = gate(items, ours, answers, headers, list_path, args)
        summarize(items, ours, answers, headers, features, listed, undecided, unanswered)
        if args.write_list:
            write_list(args.write_list, items, ours, answers, features, listed, headers, args.ours, args.write_witnesses)
        if unanswered:
            bad.append("UNANSWERED: %d items hung an engine (first: %s)" % (len(unanswered), json.dumps(items[unanswered[0]])))
        if bad:
            print("\nCSS PROPERTY DIFFERENTIAL FAILED (%d problems)" % len(bad))
            for b in bad[:int(os.environ.get("CSS_PROP_ORACLE_SHOW", "60"))]:
                print("  " + b)
            return GATE_EXIT
        print("css property differential: every divergence is classified, every witness holds, every control is as it was")
    return 0


def properties_info(binary):
    """{name: pin file the definition came from} from css-prop-ours --properties."""
    run = subprocess.run([binary, "--properties"], stdout=subprocess.PIPE, check=True)
    out = {}
    for line in run.stdout.decode("utf-8").split("\n"):
        f = line.split("\t")
        if len(f) == 5:
            out[f[0]] = f[4]
    return out


def support_class(key, headers, source):
    """The class and reason of a `support:` key, which the probes decide: the engine lacks the property."""
    which, prop = key.split(":")[1:3]
    lacking = which.split("+")
    names = " and ".join(lacking)
    versions = ", ".join("%s %s" % (e, headers[e].get("browser", "?")) for e in lacking)
    cls = "standard-newer" if len(lacking) == 2 else "reference-old"
    verb = "refuse" if len(lacking) == 2 else "refuses"
    who = "neither engine has it" if len(lacking) == 2 else "that engine does not have it"
    why = ("the pin defines %s (%s, tools/css/WEBREF_COMMIT) and %s %s `initial` for it, which every engine that has a property accepts "
           "(a CSS-wide keyword is valid for every property): %s at %s"
           % (prop, source if source and source != "-" else "the pinned data", names, verb, who, versions))
    return cls, why


def write_list(path, items, ours, answers, features, listed, headers, binary, witnesses_path):
    """The list as it would have to be to pass: the classes and reasons already given are kept, the `support:` keys (which the probes
    decide) are classified, the rest are `unclassified`; and a witness for every key is written to `witnesses_path`."""
    info = properties_info(binary)
    wit = []
    with open(path, "w", encoding="utf-8") as f:
        for key in sorted(features):
            if key in listed:
                cls, reason = listed[key][1:3]
            elif key.startswith("support:"):
                cls, reason = support_class(key, headers, info.get(features[key]["prop"]))
            else:
                cls, reason = "unclassified", "TODO"
            f.write("%s\t%d\t%s\t%s\t%s\n" % (key, features[key]["count"], cls, features[key]["digest"], reason))
            rep = features[key]["rep"]
            p, v, _ = items[rep]
            o = ("OK=" + ours[rep][1]) if ours[rep][0] == "OK" else ours[rep][0]
            refs = []
            for e in ENGINES:
                ok, s = engine_view(answers[e][rep])
                refs.append(("OK=" + s) if ok else "NO")
            ref = next((r for r in refs if r != o), refs[0])
            wit.append("%s\t%s\t%s\t%s\t%s" % (key, p, css_prop_gen.esc(v), o, ref))
    if witnesses_path:
        with open(witnesses_path, "w", encoding="utf-8") as f:
            f.write("# tools/oracle/css_prop_diff.py: one witness for every key of css_prop_divergences.txt: the smallest item of the key, what this library\n"
                    "# says of it (OK=<serialization>, NO) and what an engine says that differs from that. Columns: key, property, value, ours, reference.\n"
                    "# The run asks every witness again and fails when either answer is not the one written here.\n")
            f.write("\n".join(wit) + "\n")


def apply_plant(name, items, ours, answers, headers):
    """Corrupt the data so that a gate that works fails."""
    if name == "ours-accepts-a-refused-item":
        for i, o in enumerate(ours):
            if o[0] == "NO" and not answers["chromium"][i][0] and not answers["firefox"][i][0]:
                ours[i] = ("OK", items[i][1])
                return
    elif name == "ours-refuses-an-accepted-item":
        for i, o in enumerate(ours):
            if o[0] == "OK" and answers["chromium"][i][0] and answers["firefox"][i][0]:
                ours[i] = ("NO",)
                return
    elif name == "ours-writes-differently":
        for i, o in enumerate(ours):
            if o[0] == "OK" and answers["chromium"][i][0] and answers["firefox"][i][0] and o[1] == engine_view(answers["chromium"][i])[1]:
                ours[i] = ("OK", o[1] + " ")
                return
    elif name == "engine-flips-an-acceptance":
        for i, o in enumerate(ours):
            if o[0] == "OK" and answers["chromium"][i][0] and answers["firefox"][i][0]:
                answers["chromium"][i] = [0, "", "", 0, ""]
                return
    elif name == "header-names-another-browser":
        headers["firefox"]["browser"] = "157.0"
        return
    elif name == "header-names-another-hash":
        headers["chromium"]["image"] = headers["chromium"]["image"].replace("geckodriver=", "geckodriver=0", 1)
        return
    elif name == "header-is-from-another-driver":
        headers["firefox"]["image"] = re.sub(r"driver [0-9a-f]{64}", "driver " + "0" * 64, headers["firefox"]["image"])
        return
    elif name == "header-is-missing":
        del headers["chromium"]["image"]
        return
    elif name == "control-is-misclassified":
        i = next(i for i, it in enumerate(items) if it[2] == "control")
        answers["firefox"][i] = [0, "", "", 0, ""]
        return
    raise SystemExit("css_prop_diff: plant %r changed nothing (the population has no item for it)" % name)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
