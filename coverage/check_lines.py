#!/usr/bin/env python3
"""Checks whether one test directory's run reaches given lines of the runtime.

usage: coverage/check_lines.py <build-dir> <test-dir> <file>:<first>-<last> [...]
                               [--procs "1 2"] [--keep <dir>]

<build-dir> is an instrumented build (./build ... clang --coverage, see
coverage/README.md). The test directory is built against it and its `test`
target run once per process count in --procs, with LLVM_PROFILE_FILE pointing
at a fresh directory. The profiles are merged and measured with llvm-cov over
the programs the run built plus libreconverse, as report.sh measures them.

For each <file>:<first>-<last> (a repository-relative path), prints every
instrumented line in the range with its hit count, then a summary line:

    <file>:<first>-<last>: <hit> of <instrumented> instrumented lines reached

Exits 0 if every range has at least one reached line, 1 if some range has
none, 2 on a build, run or tool failure. The test's own exit status is
reported; a failing test is a failure (2).

LLVM_PROFDATA and LLVM_COV override the tools, as in report.sh.
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from combine import Names, load  # noqa: E402

SRC = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def die(msg, code=2):
    print(f'check_lines.py: {msg}', file=sys.stderr)
    sys.exit(code)


def tool(name, env):
    t = os.environ.get(env) or shutil.which(name)
    if not t:
        for cand in sorted(glob.glob(f'/usr/lib/llvm-*/bin/{name}'), reverse=True):
            return cand
        die(f'{name} not found (set {env})')
    return t


def parse_range(s):
    m = re.match(r'^(.+):(\d+)-(\d+)$', s)
    if not m:
        die(f"'{s}' is not <file>:<first>-<last>")
    a, b = int(m.group(2)), int(m.group(3))
    if a > b:
        die(f"'{s}': first line after last")
    return m.group(1), a, b


def run(cmd, log, env=None, cwd=None):
    with open(log, 'a') as fh:
        fh.write(f'$ {" ".join(cmd)}\n')
        fh.flush()
        return subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, env=env, cwd=cwd).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('build')
    ap.add_argument('testdir')
    ap.add_argument('ranges', nargs='+')
    ap.add_argument('--procs', default='1 2')
    ap.add_argument('--keep', help='keep profiles, lcov and logs in this directory')
    a = ap.parse_args()

    build = os.path.realpath(a.build)
    testdir = os.path.realpath(a.testdir)
    charmc = os.path.join(build, 'bin', 'charmc')
    if not os.path.exists(charmc):
        die(f'{charmc} not found')
    opt = os.path.join(build, 'include', 'conv-mach-opt.sh')
    if not os.path.exists(opt) or 'CMK_COVERAGE=1' not in open(opt).read():
        die(f'{build} is not an instrumented build (no CMK_COVERAGE=1 in {opt})')
    ranges = [parse_range(r) for r in a.ranges]
    profdata, llvmcov = tool('llvm-profdata', 'LLVM_PROFDATA'), tool('llvm-cov', 'LLVM_COV')

    work = a.keep or tempfile.mkdtemp(prefix='check_lines.')
    os.makedirs(work, exist_ok=True)
    log = os.path.join(work, 'log.txt')
    prof = os.path.join(work, 'profraw')
    shutil.rmtree(prof, ignore_errors=True)
    os.makedirs(prof)

    marker = time.time()
    time.sleep(1)  # programs built from here on are newer than the marker
    if run(['make', '-C', testdir, f'CHARMC={charmc}'], log) != 0:
        die(f'building {testdir} failed; see {log}')

    for p in a.procs.split():
        for f in glob.glob(os.path.expanduser('~/.tmp/lct_pmi_file-*')):
            shutil.rmtree(f, ignore_errors=True)
        env = dict(os.environ, LLVM_PROFILE_FILE=os.path.join(prof, '%p-%m.profraw'),
                   TESTRUN_PROCS=p)
        rc = run(['timeout', '300', 'make', '-C', testdir, 'test'], log, env=env)
        print(f'check_lines.py: test with {p} process(es): '
              f'{"passed" if rc == 0 else f"FAILED (exit {rc})"}', file=sys.stderr)
        if rc != 0:
            die(f'the test failed with {p} process(es); see {log}')

    raws = glob.glob(os.path.join(prof, '*.profraw'))
    if not raws:
        die(f'the test wrote no profiles (did it run a program?); see {log}')
    merged = os.path.join(work, 'merged.profdata')
    if run([profdata, 'merge', '-sparse', '-o', merged, *raws], log) != 0:
        die(f'llvm-profdata merge failed; see {log}')

    progs = []
    for root, _, files in os.walk(testdir):
        for f in files:
            p = os.path.join(root, f)
            if os.access(p, os.X_OK) and os.path.getmtime(p) > marker:
                with open(p, 'rb') as fh:
                    if fh.read(4) in (b'\x7fELF', b'\xcf\xfa\xed\xfe'):  # ELF, Mach-O 64
                        progs.append(p)
    for lib in ('libreconverse.so', 'libreconverse.dylib'):
        p = os.path.join(build, 'lib', lib)
        if os.path.exists(p):
            progs.append(p)
    progs = [p for p in progs if os.path.basename(p) != 'charmrun']
    if not progs:
        die('no programs were built by the test directory')

    lcov = os.path.join(work, 'run.lcov')
    cmd = [llvmcov, 'export', '-format=lcov', f'-instr-profile={merged}', progs[0]]
    for p in progs[1:]:
        cmd += ['-object', p]
    with open(lcov, 'w') as out, open(log, 'a') as err:
        if subprocess.run(cmd, stdout=out, stderr=err).returncode != 0:
            die(f'llvm-cov export failed; see {log}')

    files = load(lcov, Names(SRC))
    bad = 0
    for name, a_, b_ in ranges:
        cov = files.get(name)
        lines = sorted(l for l in (cov.da if cov else {}) if a_ <= l <= b_)
        hit = [l for l in lines if cov.da[l] > 0]
        for l in lines:
            print(f'  {name}:{l}\t{cov.da[l]}')
        print(f'{name}:{a_}-{b_}: {len(hit)} of {len(lines)} instrumented lines reached')
        if not hit:
            bad += 1
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
