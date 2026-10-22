#!/usr/bin/env python3
"""CSS Syntax against tinycss2, cssparser and csstree.

    css_diff.py <ours-runner> [--count N] [--seed S] [--explore] [--no-corpus]
                              [--plant KIND] [--chunk N]

The claim under test: the tokens, rules and declarations this library reads from a stylesheet are
the ones independent implementations read, and where they differ the difference is one of a short
list of causes, each decided by a sentence of the served CSS Syntax draft (css_divergences.txt).

Every item of the population (css_gen.py) is read by this library and by the references that have an
entry point for the question, in these modes:

  tokens        a list of component values (tinycss2, cssparser, csstree);
  stylesheet    a stylesheet's rules, a block as component values (tinycss2, cssparser);
  declarations  a list of declarations (tinycss2, cssparser);
  blocks        a block's contents, nesting included (tinycss2, cssparser);
  toplevel      the top-level rules and at-rules with their extents in the input and the declarations
                of each rule's block (csstree: it predates CSS Nesting and reads a nested rule as raw
                text, so only the top level is a question it can answer);
  bytes         a stylesheet from bytes, with its encoding (tinycss2 only: the others do not decode).

Both sides are put in the representation of css-parsing-tests (css_syntax_suite.c writes ours; the
drivers in containers/css write theirs) and then normalized for what a reference cannot say (see
NORMALIZE): a number in cssparser is an f32 with no spelling, csstree has offsets in UTF-16 code
units, the error markers of the 2021 model. Nothing else is normalized.

**What fails the run.**

  * A divergence whose class is not in css_divergences.txt, or a listed class that no divergence of
    the default population falls in (a stale entry), or one whose count has changed. The list is the
    record of every way these implementations differ from this one, with the sentence of the draft
    that decides each. The default population is fixed (--count 20000 --seed 1), so its counts are
    a pin; a hunt (any other count or seed) is held only to "every divergence is of a listed class".
  * A divergence of class `unexplained`, which is a divergence nobody has read yet: it is printed
    with its input and is fixed or classified, never listed as it stands.
  * A reference that could not answer, or a drifted population: too few items compared in a mode.

  * With a defect planted (`--plant`, or `--baseline RUNNER` when the runner is this library with a defect in a copy of
    its source), a divergence the unplanted library does not have, whatever class would explain it: an experiment
    that takes the bytes the defect is about away (a class that drops non-ASCII text drops a byte order mark with it)
    explains nothing that matters.

`--plant KIND` corrupts what is compared on purpose and `run-css-oracle.sh --controls` requires the
run to fail for each; a differential that cannot be seen to fail reports the same clean line as one
that passes.

Copyright 2026 by Corey Pennycuff
"""

import collections
import copy
import json
import math
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import css_gen  # noqa: E402
import css_ref  # noqa: E402
import oracle_env  # noqa: E402

LIST = os.path.join(HERE, "css_divergences.txt")
DEFAULT_COUNT, DEFAULT_SEED = 20000, 1

OURS_ENTRY = {"tokens": "component_values", "stylesheet": "stylesheet", "declarations": "declaration_list",
              "blocks": "block_contents", "toplevel": "toplevel", "bytes": "stylesheet_bytes"}
ASKS = {"tokens": ("tinycss2", "cssparser", "csstree"), "stylesheet": ("tinycss2", "cssparser"),
        "declarations": ("tinycss2", "cssparser"), "blocks": ("tinycss2", "cssparser"), "toplevel": ("csstree",),
        "bytes": ("tinycss2",)}
TEXT_MODES = ("tokens", "stylesheet", "declarations", "blocks", "toplevel")


def run_ours(runner, lines):
    """[(result json or None, end status)] for each request line, from the conformance runner."""
    done = subprocess.run([runner], input=("\n".join(lines) + "\n").encode("ascii"), capture_output=True)
    if done.returncode != 0:
        raise SystemExit("the runner exited %d:\n%s" % (done.returncode, done.stderr.decode("utf-8", "replace")[-2000:]))
    out = []
    cur = {}
    for raw in done.stdout.decode("utf-8", "surrogateescape").split("\n"):
        if not raw:
            continue
        key, _, rest = raw.partition(" ")
        if key == "END":
            out.append((json.loads(cur["RESULT"]) if "RESULT" in cur else None, rest, cur.get("ENCODING"),
                        {"roundtrip": cur.get("ROUNDTRIP"), "stream": cur.get("STREAM")}))
            cur = {}
        else:
            cur[key] = rest
    if len(out) != len(lines):
        raise SystemExit("sent %d requests to the runner, got %d answers" % (len(lines), len(out)))
    return out


def build_requests(items):
    """{mode: [(item index, request line)]} for the references; ours uses the same items."""
    by_mode = {m: [] for m in OURS_ENTRY}
    for idx, it in enumerate(items):
        if "bytes" in it["tags"]:
            by_mode["bytes"].append((idx, it))
        else:
            for m in TEXT_MODES:
                by_mode[m].append((idx, it))
    return by_mode


def ours_line(mode, it):
    opts = []
    if it["proto"] is not None:
        opts.append("proto=" + it["proto"].encode("ascii", "replace").hex())
    if it["env"] is not None:
        opts.append("env=" + it["env"].encode("ascii", "replace").hex())
    return "%s %s %s" % (OURS_ENTRY[mode], ",".join(opts) if opts else "-", it["data"].hex() if it["data"] else "-")


def collect(items, runner, chunk):
    """results[mode][ref][item index] = answer; ours under 'ours'."""
    by_mode = build_requests(items)
    results = {m: {} for m in by_mode}
    for mode, pairs in by_mode.items():
        if not pairs:
            continue
        ours = []
        for start in range(0, len(pairs), chunk):
            ours.extend(run_ours(runner, [ours_line(mode, it) for _, it in pairs[start:start + chunk]]))
        results[mode]["ours"] = {idx: ours[k] for k, (idx, _) in enumerate(pairs)}
    # The references, each its own list of requests, concurrently.
    requests = {ref: [] for ref in css_ref.REFERENCES}
    owners = {ref: [] for ref in css_ref.REFERENCES}
    for mode, pairs in by_mode.items():
        for ref in ASKS[mode]:
            for idx, it in pairs:
                requests[ref].append(css_ref.request_line(mode, it["data"], it["proto"], it["env"]))
                owners[ref].append((mode, idx))
    answers = css_ref.ask_all(requests, chunk=max(1000, chunk))
    for ref in css_ref.REFERENCES:
        for (mode, idx), ans in zip(owners[ref], answers[ref]):
            results[mode].setdefault(ref, {})[idx] = ans
    return results


# ---- normalization -----------------------------------------------------------------------------

def f32(x):
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def same_number(a, b):
    if isinstance(a, float) and isinstance(b, float) and math.isnan(a) and math.isnan(b):
        return True
    return a == b


def walk(v, fn):
    """Apply `fn` to every list of component values (a list that is a value, at any depth), post-order."""
    if isinstance(v, list):
        return fn([walk(x, fn) for x in v])
    return v


def drop_invalid(rules):
    """The 2021 model's marker for a construct it dropped; the draft returns nothing, and ours writes nothing."""
    if isinstance(rules, list):
        return [r for r in rules if r != ["error", "invalid"]]
    return rules


def to_cssparser(v, ours):
    """A value in the terms cssparser can say: no number spelling, numbers as f32, a percentage as a fraction
    (ours is divided by 100, cssparser's already is), no error markers for what the end of the input closed."""
    if isinstance(v, list):
        if v and v[0] in ("number", "percentage", "dimension") and len(v) >= 4 and isinstance(v[1], str):
            value = v[2] / 100 if (ours and v[0] == "percentage") else v[2]
            return [v[0], "", f32(value)] + [x for x in v[3:]]
        if v and v[0] == "error" and len(v) == 2 and v[1] in ("eof-in-string", "eof-in-url", "eof-in-comment", "eof-in-escape"):
            return None
        out = [to_cssparser(x, ours) for x in v]
        return [x for x in out if x is not None]
    return v


def normalize(ref, mode, value):
    if mode == "toplevel":
        return value
    if mode in ("stylesheet", "declarations", "blocks"):
        value = drop_invalid(value) if isinstance(value, list) else value
    if mode == "bytes" and isinstance(value, list) and len(value) == 2:
        value = [drop_invalid(value[0]), value[1]]
    if ref in ("cssparser", "ours-for-cssparser") and isinstance(value, list):
        value = to_cssparser(value, ref == "ours-for-cssparser")
    return value


def equal(a, b):
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(equal(x, y) for x, y in zip(a, b))
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool) and not isinstance(b, bool):
        return same_number(float(a), float(b))
    return a == b


def utf16_offsets(data):
    """Map a byte offset of well-formed UTF-8 `data` to the JavaScript string's UTF-16 offset; None if it is ill formed."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return None
    out = {}
    byte = 0
    unit = 0
    for ch in text:
        out[byte] = unit
        byte += len(ch.encode("utf-8"))
        unit += 2 if ord(ch) > 0xFFFF else 1
    out[byte] = unit
    return out


def ours_toplevel(value, data):
    """Our top-level extents in UTF-16 units (csstree's), or None for ill-formed input."""
    m = utf16_offsets(data)
    if m is None:
        return None
    out = []
    for item in value:
        if item[0] == "rule":
            out.append(["rule", m.get(item[1], -1), m.get(item[2], -1), item[3]])
        else:
            out.append(["at", item[1], m.get(item[2], -1), m.get(item[3], -1), item[4]])
    return out


def experiment(runner, mode, ref, rows, chunk):
    """Whether this library and `ref` agree about each of `rows` ((data, proto, env)) in `mode`: a list of bool."""
    items = [{"data": d, "proto": p, "env": e, "tags": ["bytes"] if mode == "bytes" else ["x"]} for d, p, e in rows]
    ours = []
    for start in range(0, len(items), chunk):
        ours.extend(run_ours(runner, [ours_line(mode, it) for it in items[start:start + chunk]]))
    reqs = {ref: [css_ref.request_line(mode, it["data"], it["proto"], it["env"]) for it in items]}
    theirs = css_ref.ask_all(reqs, chunk=max(1000, chunk))[ref]
    if PLANT:
        ours = [corrupt(PLANT, mode, o) for o in ours]
    return [agree(mode, ref, o, t, it["data"]) for o, t, it in zip(ours, theirs, items)]


def answers_of(mode, ref, o, t, data):
    """(ours, theirs) normalized, or None when either side gave no comparable answer."""
    if isinstance(t, dict) or o[1] != "DONE":
        return None
    mine = o[0]
    if mode == "bytes" and mine is not None and o[2]:
        mine = [mine, o[2].split(" ")[0]]
    if mode == "toplevel":
        mine = ours_toplevel(mine, data)
        if mine is None:
            return None
    a, b = normalize("ours-for-cssparser" if ref == "cssparser" else "ours", mode, mine), normalize(ref, mode, t)
    if mode == "toplevel" and isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        # csstree gives an at-rule's name as written, escapes and all; ours is the decoded name. A name with an
        # escape in it is not compared.
        for x, y in zip(a, b):
            if x[0] == "at" and y[0] == "at" and "\\" in y[1]:
                x[1] = y[1] = "\\"
            if x[0] == "rule" and y[0] == "rule" and len(x[3]) == len(y[3]):
                for dx, dy in zip(x[3], y[3]):
                    if "\\" in dy[0]:
                        dx[0] = dy[0] = "\\"
    return a, b


def agree(mode, ref, o, t, data):
    ab = answers_of(mode, ref, o, t, data)
    return ab is not None and equal(*ab)


def classify_core(runner, divergences, items, chunk):
    """{(mode, ref, classes): [(item, ours, theirs)]}: each divergence by an irreducible set of known classes whose
    neutralization makes this library and the reference agree about the neutralized input; () is `unexplained`.

    First every class that applies is neutralized at once: if the two still disagree, nothing known explains it. Then
    each class in turn is dropped from the set when the two still agree without it, so what is left is a set no member
    of which can be left out (the divergence is the sum of those causes, and is reported under each of them).
    """
    import css_classes
    cur = {}
    for k, (mode, ref, idx, a, b) in enumerate(divergences):
        it = items[idx]
        cands = []
        for name, c in sorted(css_classes.CLASSES.items()):
            if ref not in c["who"].split():
                continue
            if c["when"] is not None and not c["when"](mode, ref, a, b):
                continue
            cands.append(name)
        cur[k] = cands

    def neutralized(k, names):
        it = items[divergences[k][2]]
        data = it["data"]
        for name in names:
            data = css_classes.CLASSES[name]["fn"](data)
        return (data, it["proto"], it["env"])

    def run(ks, sets):
        out = {}
        by_pair = collections.defaultdict(list)
        for k in ks:
            by_pair[(divergences[k][0], divergences[k][1])].append(k)
        for (mode, ref), group in by_pair.items():
            rows = [neutralized(k, sets[k]) for k in group]
            for k, ok in zip(group, experiment(runner, mode, ref, rows, chunk)):
                out[k] = ok
        return out

    explained = {}
    full = run([k for k in cur if cur[k]], cur)
    for k, ok in full.items():
        if ok:
            explained[k] = list(cur[k])
    for name in sorted(css_classes.CLASSES):
        ks = [k for k in explained if name in explained[k] and len(explained[k]) > 1]
        sets = {k: [n for n in explained[k] if n != name] for k in ks}
        if not ks:
            continue
        for k, ok in run(ks, sets).items():
            if ok:
                explained[k] = sets[k]
    # Neutralizers can get in each other's way (one rewrites what another looks for), so a divergence that the
    # whole set does not explain is tried against every smaller set, smallest first.
    import itertools
    for size in (1, 2, 3):
        todo = [(k, combo) for k in cur if k not in explained for combo in itertools.combinations(cur[k], size)]
        if not todo:
            continue
        sets = {i: combo for i, (k, combo) in enumerate(todo)}
        owner = {i: k for i, (k, combo) in enumerate(todo)}
        by_pair = collections.defaultdict(list)
        for i, k in owner.items():
            by_pair[(divergences[k][0], divergences[k][1])].append(i)
        for (mode, ref), group in by_pair.items():
            rows = [neutralized(owner[i], sets[i]) for i in group]
            for i, ok in zip(group, experiment(runner, mode, ref, rows, chunk)):
                if ok and owner[i] not in explained:
                    explained[owner[i]] = list(sets[i])
    grouped = collections.defaultdict(list)
    for k, (mode, ref, idx, a, b) in enumerate(divergences):
        grouped[(mode, ref, tuple(explained.get(k, ())))].append((items[idx], a, b))
    return grouped


def diverge_rows(runner, mode, ref, rows, chunk):
    """[(ours normalized, theirs normalized) or None] for each of rows ((data, proto, env)): where they differ."""
    items = [{"data": d, "proto": p, "env": e, "tags": ["bytes"] if mode == "bytes" else ["x"]} for d, p, e in rows]
    ours = []
    for start in range(0, len(items), chunk):
        ours.extend(run_ours(runner, [ours_line(mode, it) for it in items[start:start + chunk]]))
    if PLANT:
        ours = [corrupt(PLANT, mode, o) for o in ours]
    theirs = css_ref.ask_all({ref: [css_ref.request_line(mode, it["data"], it["proto"], it["env"]) for it in items]}, chunk=max(1000, chunk))[ref]
    out = []
    for o, t, it in zip(ours, theirs, items):
        ab = answers_of(mode, ref, o, t, it["data"])
        out.append(ab if ab is not None and not equal(*ab) else None)
    return out


def minimize(runner, mode, ref, data, proto, env, chunk):
    """The smallest input (by deleting characters, ddmin) that this library and the reference still disagree about."""
    cur = data.decode("utf-8", "surrogateescape")
    size = max(1, len(cur) // 2)
    while True:
        progress = True
        while progress:
            progress = False
            cands = []
            step = max(1, min(size, len(cur)))
            for k in range(0, len(cur), step):
                c = cur[:k] + cur[k + step:]
                if c != cur:
                    cands.append(c)
            if not cands:
                break
            rows = [(c.encode("utf-8", "surrogateescape"), proto, env) for c in cands]
            for c, res in zip(cands, diverge_rows(runner, mode, ref, rows, chunk)):
                if res is not None:
                    cur = c
                    progress = True
                    break
        if size == 1:
            break
        size //= 2
    return cur.encode("utf-8", "surrogateescape")


def classify(runner, divergences, items, chunk):
    """classify_core, and for what it cannot explain: the divergence is cut down to the smallest input that still
    diverges (a handful of characters, where the causes are not tangled) and that is classified instead."""
    grouped = classify_core(runner, divergences, items, chunk)
    unexplained = grouped.pop((None,), None)
    residue = []
    for key in [k for k in grouped if k[2] == ()]:
        residue.extend((key[0], key[1], row) for row in grouped.pop(key))
    if not residue:
        return grouped
    extra_items = []
    extra = []
    still = []
    for mode, ref, row in residue[40:]:  # (a bound on the work: forty are cut down, the rest stay unexplained, which fails the run all the same)
        still.append((mode, ref, row))
    for mode, ref, (it, a, b) in residue[:40]:
        small = minimize(runner, mode, ref, it["data"], it["proto"], it["env"], chunk)
        new = dict(it)
        new["data"] = small
        new["tags"] = it["tags"] + ["minimized"]
        res = diverge_rows(runner, mode, ref, [(small, it["proto"], it["env"])], chunk)[0]
        if res is None:  # (cannot happen: the minimizer only accepts inputs that diverge)
            extra.append((mode, ref, len(extra_items), a, b))
        else:
            extra.append((mode, ref, len(extra_items), res[0], res[1]))
        extra_items.append(new)
    again = classify_core(runner, extra, extra_items, chunk)
    for key, rows in again.items():
        grouped.setdefault(key, []).extend(rows)
    for mode, ref, row in still:
        grouped.setdefault((mode, ref, ()), []).append(row)
    return grouped


def read_list():
    """{(mode, ref, class): (count, witness, reason)} from css_divergences.txt."""
    out = {}
    if not os.path.exists(LIST):
        return out
    with open(LIST, encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 6:
                raise SystemExit("%s:%d: not mode, reference, class, count, witness, reason" % (LIST, n))
            mode, ref, name, count, witness, reason = parts
            out[(mode, ref, name)] = (int(count), witness, reason)
    return out


PLANTS = {
    "ours-drops-a-token": "our answer to a list of component values loses its last token",
    "ours-number": "a number in our answer is off by one",
    "ours-rule-extent": "a top-level rule in our answer ends a byte late",
    "ours-encoding": "our answer to a stylesheet from bytes names another encoding",
    "ours-forgets-important": "a declaration in our answer is not important when it was",
    "stale-entry": "a class of the list that no divergence falls in is added to it",
    "change-count": "a class of the list has another count",
    "no-list": "the list is read as empty",
}


PLANT = None  # the kind of corruption in force (--plant), applied to everything this library says, experiments included


def corrupt(kind, mode, o):
    """What this library said (o), corrupted as `kind` says; unchanged when the kind has nothing to do with it."""
    value = o[0]
    if kind == "ours-drops-a-token" and mode == "tokens" and isinstance(value, list) and value:
        return (value[:-1],) + tuple(o[1:])
    if kind == "ours-number" and mode == "tokens" and isinstance(value, list):
        def bump(v):
            if isinstance(v, list):
                if v and v[0] in ("number", "percentage", "dimension") and len(v) >= 4 and isinstance(v[2], (int, float)):
                    return v[:2] + [v[2] + 1] + v[3:]
                return [bump(x) for x in v]
            return v
        return (bump(value),) + tuple(o[1:])
    if kind == "ours-rule-extent" and mode == "toplevel" and isinstance(value, list) and value:
        # (every rule and at-rule extent, not one: a class that takes the input's first rule away must not take the corruption with it)
        return ([["rule", x[1], x[2] + 1, x[3]] if x[0] == "rule" else ["at", x[1], x[2], x[3] + 1, x[4]] for x in value],) + tuple(o[1:])
    if kind == "ours-encoding" and mode == "bytes" and o[2]:
        return (value, o[1], "utf-8 default" if not o[2].startswith("utf-8") else "windows-1251 default", o[3])
    if kind == "ours-forgets-important" and mode in ("declarations", "blocks") and isinstance(value, list):
        def unimportant(v):
            if isinstance(v, list):
                if len(v) == 4 and v[0] == "declaration" and v[3] is True:
                    return v[:3] + [False]
                return [unimportant(x) for x in v]
            return v
        return (unimportant(value),) + tuple(o[1:])
    return o


def plant_ours(kind, results):
    """Corrupt what this library said, on purpose (run-css-oracle.sh --controls requires the run to fail)."""
    global PLANT
    PLANT = kind
    for mode, by_ref in results.items():
        ours = by_ref.get("ours")
        if ours:
            for idx, o in list(ours.items()):
                ours[idx] = corrupt(kind, mode, o)


def find_divergences(results, items, summary):
    """[(mode, reference, item index, ours, theirs)] where this library and a reference disagree; summary counts what was asked."""
    divergences = []
    for mode in OURS_ENTRY:
        ours = results[mode].get("ours")
        if not ours:
            continue
        for ref in ASKS[mode]:
            theirs = results[mode][ref]
            for idx, o in ours.items():
                t = theirs[idx]
                summary[(mode, ref, "compared")] += 1
                if isinstance(t, dict):
                    summary[(mode, ref, "refused:" + ("unsupported" if t.get("unsupported") else "crash"))] += 1
                    continue
                if o[1] != "DONE":
                    summary[(mode, ref, "ours-failed")] += 1
                    continue
                if ref == ASKS[mode][0]:  # (once per item, not once per reference)
                    if o[3].get("roundtrip") not in (None, "ok"):
                        summary[(mode, "ours", "refused:round-trip")] += 1
                    if o[3].get("stream") not in (None, "ok"):
                        summary[(mode, "ours", "refused:stream")] += 1
                ab = answers_of(mode, ref, o, t, items[idx]["data"])
                if ab is None:
                    summary[(mode, ref, "skipped:ill-formed")] += 1
                    continue
                if not equal(*ab):
                    summary[(mode, ref, "differ")] += 1
                    divergences.append((mode, ref, idx, ab[0], ab[1]))
    return divergences


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    runner = args.pop(0)
    count, seed, explore, corpus, chunk, propose, plant = DEFAULT_COUNT, DEFAULT_SEED, False, True, 5000, False, None
    baseline = None  # the runner of the unplanted library, when `runner` is one with a defect planted in it
    while args:
        a = args.pop(0)
        if a == "--count":
            count = int(args.pop(0))
        elif a == "--seed":
            seed = int(args.pop(0))
        elif a == "--explore":
            explore = True
        elif a == "--propose":
            propose = True
        elif a == "--plant":
            plant = args.pop(0)
            if plant not in PLANTS:
                raise SystemExit("unknown plant %r (one of %s)" % (plant, ", ".join(PLANTS)))
        elif a == "--baseline":
            baseline = args.pop(0)
        elif a == "--no-corpus":
            corpus = False
        elif a == "--chunk":
            chunk = int(args.pop(0))
        else:
            raise SystemExit("unknown argument " + a)
    items = css_gen.population(count, seed, corpus=corpus)
    print(css_ref.check_image())
    results = collect(items, runner, chunk)
    base_keys = None  # (mode, reference, item index) of the divergences the unplanted library has; None: nothing is planted
    if plant and plant in ("ours-drops-a-token", "ours-number", "ours-rule-extent", "ours-encoding", "ours-forgets-important"):
        base_results = copy.deepcopy(results)
        base_keys = {(m, r, i) for m, r, i, _a, _b in find_divergences(base_results, items, collections.Counter())}
    elif baseline:
        base_keys = {(m, r, i) for m, r, i, _a, _b in find_divergences(collect(items, baseline, chunk), items, collections.Counter())}
    if plant:
        plant_ours(plant, results)
    summary = collections.Counter()
    divergences = find_divergences(results, items, summary)
    fresh = []
    if base_keys is not None:
        # A divergence the unplanted library does not have is the defect's, whatever class would explain it: an experiment
        # may take the very bytes the defect is about away (a class that drops the non-ASCII text drops a byte order mark),
        # and then "explains" a defect that nothing known explains.
        fresh = [d for d in divergences if (d[0], d[1], d[2]) not in base_keys]
        divergences = [d for d in divergences if (d[0], d[1], d[2]) in base_keys]
    for key in sorted(summary):
        print("%-14s %-9s %-24s %d" % (key + (summary[key],)))
    grouped = classify(runner, divergences, items, chunk)
    per_class = collections.defaultdict(list)
    for (mode, ref, classes), rows in grouped.items():
        for name in classes or ("unexplained",):
            per_class[(mode, ref, name)].extend(rows)
    import css_classes
    failures = []
    for (mode, ref), n in sorted(collections.Counter((d[0], d[1]) for d in fresh).items()):
        failures.append("%d divergences of %s/%s that no known class explains (the unplanted library does not have them)" % (n, mode, ref))
        for _m, _r, idx, a, b in sorted((d for d in fresh if d[:2] == (mode, ref)), key=lambda d: len(items[d[2]]["data"]))[:3]:
            print("      input %r %s %s" % (items[idx]["data"][:140], items[idx]["proto"], items[idx]["env"]))
            print("        ours   %s" % json.dumps(a)[:240])
            print("        theirs %s" % json.dumps(b)[:240])
    for (mode, ref, name), rows in sorted(per_class.items()):
        shortest = min(rows, key=lambda r: len(r[0]["data"]))
        print("%-12s %-9s %-44s %6d  %r" % (mode, ref, name, len(rows), shortest[0]["data"][:50]))
        if name == "unexplained":
            failures.append("%d divergences of %s/%s that no known class explains" % (len(rows), mode, ref))
            for it, a, b in sorted(rows, key=lambda r: len(r[0]["data"]))[:8]:
                print("      input %r %s %s" % (it["data"][:140], it["proto"], it["env"]))
                print("        ours   %s" % json.dumps(a)[:240])
                print("        theirs %s" % json.dumps(b)[:240])
        elif explore:
            print("      reason: " + css_classes.CLASSES[name]["reason"])
    refused = [k for k in summary if k[2].startswith("refused") or k[2] == "ours-failed"]
    for k in refused:
        failures.append("%s/%s: %d x %s" % (k[0], k[1], summary[k], k[2]))
    default = (count, seed) == (DEFAULT_COUNT, DEFAULT_SEED) and corpus
    if plant == "no-list":
        listed = {}
    else:
        listed = read_list()
    if plant == "stale-entry":
        listed[("tokens", "tinycss2", "css21-match-tokens-stale")] = (1, "b'x'", "planted")
    found = {k: len(rows) for k, rows in per_class.items() if k[2] != "unexplained"}
    if propose:
        print("# proposed lines of css_divergences.txt")
        for (mode, ref, name), rows in sorted(per_class.items()):
            if name == "unexplained":
                continue
            shortest = min(rows, key=lambda r: len(r[0]["data"]))
            print("%s\t%s\t%s\t%d\t%r\t%s" % (mode, ref, name, len(rows), shortest[0]["data"][:80], css_classes.CLASSES[name]["reason"]))
    else:
        for key in sorted(found):
            if key not in listed:
                failures.append("%s/%s: class %s is not in css_divergences.txt (%d divergences)" % (key + (found[key],)))
        if default:
            for key, (n, witness, reason) in sorted(listed.items()):
                if key not in found and n == 0:
                    continue  # (found by a hunt, not in the default population: held to zero here)
                if key not in found:
                    failures.append("%s/%s: class %s is listed and no divergence falls in it (a stale entry)" % key)
                elif plant == "change-count" and key == sorted(listed)[0] or found[key] != n:
                    failures.append("%s/%s: class %s has %d divergences, the list says %d" % (key + (found[key], n)))
                elif key[2] not in css_classes.CLASSES or reason != css_classes.CLASSES[key[2]]["reason"]:
                    failures.append("%s/%s: class %s: the list's reason is not the class's" % key)
        floors = {"tokens": 3000, "stylesheet": 3000, "declarations": 3000, "blocks": 3000, "toplevel": 2000, "bytes": 200}
        if default:
            for mode, floor in floors.items():
                for ref in ASKS[mode]:
                    if summary[(mode, ref, "compared")] < floor:
                        failures.append("%s/%s: only %d items compared, below the floor of %d" % (mode, ref, summary[(mode, ref, "compared")], floor))
    if failures:
        sys.stderr.write("\033[0;31m### the CSS differential failed ###\033[0m\n")
        for f in failures:
            sys.stderr.write("  " + f + "\n")
        sys.exit(1)
    print("css differential: %d items, every divergence is of a listed class%s" % (len(items), "" if default else " (a hunt: counts not held)"))


if __name__ == "__main__":
    main()
