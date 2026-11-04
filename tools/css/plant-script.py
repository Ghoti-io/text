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
"""Plant a defect in the cascade, computed-value, inheritance or selector code and show that a test the scoring interpreter scores sees it.

    PREFIX=... DEP_PCS=... ARCHIVE=... plant-script.py <vectors dir> <runner object source> [name ...]

Each line of tools/conformance/CSS_CSSOM_SCRIPT_PLANTS is one edit to one file of src/css/ (or, when the file is written `runner:`, to the runner itself), made in a copy in a scratch
directory (the working tree is never touched). The edited file is compiled alone and linked ahead of the library's archive into the runner of the scoring interpreter (tools/conformance/css_cssom_script_suite.c), which runs the programs of
`script.tsv`. The **control** is the same with no edit. A plant has to change the results of the control: `fail` means a test that passes in the control fails; `layout` means a test that
the control leaves as needing a layout is scored (the library would claim a used value it does not have).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, errors='replace', **kw)


def load_plants():
    out = []
    for line in open(os.path.join(ROOT, 'tools', 'conformance', 'CSS_CSSOM_SCRIPT_PLANTS'), encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        name, rel, old, new, must = line.rstrip('\n').split('\t')
        out.append((name, rel, old.replace('\\n', '\n'), new.replace('\\n', '\n'), must))
    return out


def results(exe, program):
    p = subprocess.run([exe], input=program, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=900)
    out = {}
    for line in p.stdout.decode('utf-8', 'replace').split('\n'):
        f = line.split('\t')
        if f[0] == 'R':
            out[(f[1], f[2])] = f[3]
    return p.returncode, out


def main(argv):
    prefix, dep_pcs, archive = (os.environ.get(k) for k in ('PREFIX', 'DEP_PCS', 'ARCHIVE'))
    if not (prefix and dep_pcs and archive) or len(argv) < 3:
        sys.stderr.write('PREFIX, DEP_PCS and ARCHIVE must be set, and the vectors dir and the runner source given\n')
        return 2
    vectors, source = argv[1], argv[2]
    archive = os.path.abspath(archive)
    generated = os.path.join(os.path.dirname(os.path.dirname(archive)), 'generated')
    env = dict(os.environ, PKG_CONFIG_PATH=os.path.join(prefix, 'share', 'pkgconfig'))
    cflags = sh(['pkg-config', '--cflags'] + dep_pcs.split(), env=env).stdout.split()
    libs = sh(['pkg-config', '--libs'] + dep_pcs.split(), env=env).stdout.split()
    plants = load_plants()
    names = argv[3:] or ['control'] + [p[0] for p in plants]
    program = open(os.path.join(vectors, 'script.tsv'), 'rb').read()
    # the hand-written programs of the runner ride along: they are the ones that see a defect of the runner's own (the document a script sees, what a new parse carries over)
    program += b'\n' + open(os.path.join(ROOT, 'tools', 'conformance', 'CSS_CSSOM_SCRIPT_FIXTURES'), 'rb').read()
    scratch = tempfile.mkdtemp(prefix='css-script-plants-')
    failures = 0
    try:
        obj = os.path.join(scratch, 'runner.o')
        done = sh(['cc', '-std=gnu17', '-O1', '-g', '-D_GNU_SOURCE', '-c', '-I', os.path.join(ROOT, 'include'), '-I', generated] + cflags + [source, '-o', obj])
        if done.returncode != 0:
            sys.stderr.write(done.stderr[-3000:])
            return 2
        control = None
        for name in ['control'] + [n for n in names if n != 'control']:
            plant = next((p for p in plants if p[0] == name), None)
            if name != 'control' and plant is None:
                sys.stderr.write('no such plant: %s\n' % name)
                return 2
            work = os.path.join(scratch, name)
            os.makedirs(work)
            objs = []
            run_obj = obj
            if plant and plant[1] == 'runner:':
                # a defect in the runner: the edit is made in a copy of its source and the edited runner is linked in place of the unedited one
                _, _, old, new, must = plant
                text = open(source, encoding='utf-8').read()
                if text.count(old) != 1:
                    sys.stderr.write('plant %s: the text is in the runner %d times, not once\n' % (name, text.count(old)))
                    return 2
                edited = os.path.join(work, 'runner_plant.c')
                open(edited, 'w', encoding='utf-8').write(text.replace(old, new))
                run_obj = os.path.join(work, 'runner_plant.o')
                done = sh(['cc', '-std=gnu17', '-O1', '-g', '-D_GNU_SOURCE', '-c', '-I', os.path.join(ROOT, 'include'), '-I', generated] + cflags + [edited, '-o', run_obj])
                if done.returncode != 0:
                    sys.stderr.write('plant %s did not compile:\n%s' % (name, done.stderr[-3000:]))
                    return 2
            elif plant:
                _, rel, old, new, must = plant
                shutil.copytree(os.path.join(ROOT, 'src'), os.path.join(work, 'src'))
                path = os.path.join(work, 'src', 'css', rel)
                text = open(path, encoding='utf-8').read()
                if text.count(old) != 1:
                    sys.stderr.write('plant %s: the text is in %s %d times, not once\n' % (name, rel, text.count(old)))
                    return 2
                open(path, 'w', encoding='utf-8').write(text.replace(old, new))
                po = os.path.join(work, 'plant.o')
                done = sh(['cc', '-std=c17', '-O2', '-fPIC', '-fvisibility=hidden', '-DGTEXT_BUILD', '-w', '-I', os.path.join(ROOT, 'include'), '-I', generated,
                           '-I', os.path.join(work, 'src', 'css'), '-I', os.path.join(work, 'src', 'markup')] + cflags + ['-c', path, '-o', po])
                if done.returncode != 0:
                    sys.stderr.write('plant %s did not compile:\n%s' % (name, done.stderr[-3000:]))
                    return 2
                objs = [po]
            exe = os.path.join(work, 'runner')
            done = sh(['cc', '-o', exe, run_obj] + objs + [archive] + libs + ['-lm', '-Wl,-rpath,' + os.path.join(prefix, 'lib', 'ghoti.io'), '-Wl,--allow-multiple-definition'])
            if done.returncode != 0:
                sys.stderr.write('plant %s did not link:\n%s' % (name, done.stderr[-3000:]))
                return 2
            code, res = results(exe, program)
            if name == 'control':
                control = res
                print('control: %d results' % len(res))
                continue
            if plant[4] == 'fail':
                changed = [k for k, v in res.items() if v == 'FAIL' and control.get(k) == 'PASS']
            else:
                changed = [k for k, v in res.items() if v != 'LAYOUT' and control.get(k) == 'LAYOUT']
            if not changed:
                print('plant %s: NOT CAUGHT (no test %s)' % (name, 'passes in the control and fails' if plant[4] == 'fail' else 'moves out of the layout list'))
                failures += 1
            else:
                print('plant %s: caught by %d tests (%s#%s ...)' % (name, len(changed), changed[0][0], changed[0][1]))
        print('script plants: %s' % ('all caught' if failures == 0 else '%d not caught' % failures))
        return 0 if failures == 0 else 1
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
