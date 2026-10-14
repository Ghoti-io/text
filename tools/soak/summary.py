#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Summarise a soak job's results directory: executions, coverage growth, artifacts, exit codes."""
import os, re, sys, glob

def main(r):
    names = sorted({os.path.basename(p).rsplit('-s', 1)[0] for p in glob.glob(os.path.join(r, 'logs', '*-s[12].log'))})
    print('%-16s %-4s %12s %9s %7s %7s %7s %6s %s' % ('harness', 'seed', 'execs', 'secs', 'cov0', 'cov', 'ft', 'corp', 'exit'))
    for n in names:
        for s in (1, 2):
            log = os.path.join(r, 'logs', '%s-s%d.log' % (n, s))
            if not os.path.exists(log):
                continue
            txt = open(log, errors='replace').read()
            inited = re.search(r'INITED cov: (\d+) ft: (\d+) corp: (\d+)', txt)
            last = None
            for m in re.finditer(r'#(\d+)\s+(?:NEW|REDUCE|pulse|DONE|INITED)\s+cov: (\d+) ft: (\d+) corp: (\d+)/', txt):
                last = m
            execs = re.search(r'stat::number_of_executed_units: (\d+)', txt)
            secs = re.search(r'stat::average_exec_per_sec: (\d+)', txt)
            ex = open(os.path.join(r, 'logs', '%s-s%d.exit' % (n, s))).read().strip() if os.path.exists(log[:-4] + '.exit') else '?'
            e = int(execs.group(1)) if execs else 0
            sec = e // int(secs.group(1)) if secs and int(secs.group(1)) else 0
            print('%-16s %-4d %12d %9d %7s %7s %7s %6s %s' % (n, s, e, sec,
                  inited.group(1) if inited else '?', last.group(2) if last else '?',
                  last.group(3) if last else '?', last.group(4) if last else '?', ex))
    arts = sorted(glob.glob(os.path.join(r, 'artifacts', '*')))
    print('artifacts: %d' % len(arts))
    for a in arts:
        print('  ' + os.path.basename(a))
    for n in names:
        mlog = os.path.join(r, 'logs', n + '-merge.log')
        if os.path.exists(mlog):
            t = open(mlog, errors='replace').read()
            m = re.findall(r'MERGE-OUTER: (\d+) new files with (\d+) new features added; (\d+) new coverage edges', t)
            print('merge %-16s %s' % (n, m[-1] if m else t.strip().splitlines()[-1:]))
            print('   exit', open(os.path.join(r, 'logs', n + '-merge.exit')).read().strip(),
                  'files', open(os.path.join(r, 'logs', n + '-nfiles.txt')).read().strip())

main(sys.argv[1])
