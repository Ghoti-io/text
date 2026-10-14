#!/usr/bin/env python3
"""The instructions behind the quadratic suite's counters, recorded (not gated).

    quadratic-instructions.py [--binary PATH] [--jobs N] [--group G] [--shape NAME] [--out FILE] [--report FILE]

tests/test-quadratic.cpp counts the library's own work units (tokens handled, stack entries walked, bytes
examined, handler steps) and holds their growth to a bound. A counter sees only what its author put it
beside. This runs each shape of the same suite, at n, 4n and 16n, under `valgrind --tool=callgrind` and reads
the instructions executed inside the three regions (`quad_region_1`, `_4`, `_16`, the only code callgrind
collects: the generation of the input is inside them, and is linear), so the growth of the machine's work
can be put beside the growth of the counter's. (`perf stat` would do the same where the kernel allows it;
perf_event_paranoid is 3 here.)

Per shape it prints the counter's ratios (4n/n and 16n/4n) and the instructions' ratios, and **flags** a shape
where the instructions grow faster than FLAG (default 5.5) when the counter does not: the counter then
misses work, and the shape is a candidate for a defect (a copy, a rescan, a lookup the counter does not
see). The flags are what to read; the numbers are a record, not a gate, because instruction counts depend on
the compiler and the C library and the counters do not. Exit status is 0 whatever is flagged; 2 if the
tool could not run.

Copyright 2026 by Corey Pennycuff
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_BINARY = os.path.join(ROOT, "build", "linux", "release", "apps", "testQuadratic")
FLAG = 5.5


def list_shapes(binary):
    env = dict(os.environ, GTEXT_QUAD_LIST="1")
    done = subprocess.run([binary, "--gtest_filter=Quadratic.ListShapes"], capture_output=True, text=True, env=env)
    shapes = []
    for line in done.stdout.splitlines():
        if line.startswith("SHAPE\t"):
            _, group, name, units = line.split("\t")
            shapes.append((group, name, int(units)))
    if not shapes:
        raise SystemExit("the suite listed no shapes (%s exited %d):\n%s" % (binary, done.returncode, done.stderr[-500:]))
    return shapes


REGION = re.compile(r"^\s*([\d,]+)\s.*test-quadratic\.cpp:quad_region_(1|4|16) \[")


def instructions(binary, name, scratch):
    out = os.path.join(scratch, "cg.%d.out" % abs(hash(name)))
    env = dict(os.environ, GTEXT_QUAD_ONE=name)
    cmd = ["valgrind", "--tool=callgrind", "--callgrind-out-file=" + out, "--collect-atstart=no",
           "--toggle-collect=quad_region_1", "--toggle-collect=quad_region_4", "--toggle-collect=quad_region_16",
           "--quiet", binary, "--gtest_filter=Quadratic.RunOne"]
    done = subprocess.run(cmd, capture_output=True, text=True, env=env)
    counters = None
    for line in done.stdout.splitlines():
        if line.startswith("RUNONE\t"):
            parts = line.split("\t")
            counters = tuple(int(x) for x in parts[2:5])
    if done.returncode != 0 or counters is None:
        return None, "valgrind or the shape failed (%d): %s" % (done.returncode, (done.stderr or done.stdout)[-300:])
    ann = subprocess.run(["callgrind_annotate", "--inclusive=yes", "--threshold=100", out], capture_output=True, text=True)
    found = {}
    for line in ann.stdout.splitlines():
        m = REGION.match(line)
        if m:
            found[m.group(2)] = int(m.group(1).replace(",", ""))
    os.unlink(out)
    if set(found) != {"1", "4", "16"}:
        return None, "callgrind_annotate showed the regions %s, not all three" % sorted(found)
    return (counters, (found["1"], found["4"], found["16"])), None


def ratio(a, b):
    return b / a if a else float("inf")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--binary", default=DEFAULT_BINARY)
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--group")
    ap.add_argument("--shape")
    ap.add_argument("--flag", type=float, default=FLAG)
    ap.add_argument("--out", help="write the table here as well (tab separated)")
    args = ap.parse_args(argv)
    if not os.path.exists(args.binary):
        sys.stderr.write("%s is not built: make build/linux/release/apps/testQuadratic PREFIX=...\n" % args.binary)
        return 2
    shapes = [s for s in list_shapes(args.binary) if (not args.group or s[0] == args.group) and (not args.shape or s[1] == args.shape)]
    rows, flagged, failed = [], [], []
    with tempfile.TemporaryDirectory() as scratch:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = {pool.submit(instructions, args.binary, s[1], scratch): s for s in shapes}
            for fut in concurrent.futures.as_completed(futures):
                group, name, units = futures[fut]
                got, why = fut.result()
                if got is None:
                    failed.append((name, why))
                    continue
                (c1, c4, c16), (i1, i4, i16) = got
                rows.append((group, name, units, c1, c4, c16, i1, i4, i16))
    rows.sort()
    lines = []
    header = "group\tshape\tunits\tcounter(n)\tcounter(4n)\tcounter(16n)\tIr(n)\tIr(4n)\tIr(16n)\tcounter 4n/n\tcounter 16n/4n\tIr 4n/n\tIr 16n/4n\tflag"
    lines.append(header)
    for group, name, units, c1, c4, c16, i1, i4, i16 in rows:
        cr1, cr2 = ratio(c1, c4), ratio(c4, c16)
        ir1, ir2 = ratio(i1, i4), ratio(i4, i16)
        flag = ""
        # flat shapes (counter ratio ~1) are flagged when the instructions grow like input, which they may: the input is built inside the region
        if (ir1 > args.flag and cr1 <= args.flag) or (ir2 > args.flag and cr2 <= args.flag):
            flag = "INSTRUCTIONS GROW FASTER THAN THE COUNTER"
            flagged.append((group, name, cr1, cr2, ir1, ir2))
        lines.append("%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.2f\t%.2f\t%.2f\t%.2f\t%s" % (
            group, name, units, c1, c4, c16, i1, i4, i16, cr1, cr2, ir1, ir2, flag))
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if args.out:
        with open(args.out, "w") as fh:
            fh.write(text)
    sys.stdout.write("\n%d shapes measured, %d flagged, %d failed to run\n" % (len(rows), len(flagged), len(failed)))
    for g, n, cr1, cr2, ir1, ir2 in flagged:
        sys.stdout.write("FLAG [%s] %s: counter x%.2f x%.2f, instructions x%.2f x%.2f\n" % (g, n, cr1, cr2, ir1, ir2))
    for n, why in failed:
        sys.stdout.write("FAILED %s: %s\n" % (n, why))
    return 2 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
