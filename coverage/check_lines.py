#!/usr/bin/env python3
"""Checks how much of given line ranges of the runtime one test directory reaches.

usage: coverage/check_lines.py <build-dir> <test-dir> <file>:<first>-<last> [...]
                               [--procs "1 2"] [--timeout 300] [--keep <dir>]

<build-dir> is an instrumented build (./build ... clang --coverage, see
coverage/README.md). The test directory is cleaned (make clean), built against
it and its `test` target run once per process count in --procs, with
LLVM_PROFILE_FILE pointing at a fresh directory outside the tree. The profiles
are merged and measured with llvm-cov over the programs the build produced plus
libreconverse, as report.sh measures them.

For each <file>:<first>-<last> (repository-relative, line numbers of this
checkout) it prints every instrumented line in the range with its hit count,
marking error-path lines (backlog.error_path_lines: calls that abort, assert or
print, and lines of only braces, else, break or a bare return), the branch
outcomes in the range that never ran, and a verdict:

    tested       every instrumented line that is not an error path was reached
    partial      some were
    not reached  none were

Exits 0 if every range is tested, 1 if some range is partial or not reached,
and 2 if something could not be built, run or measured, including a range with
no instrumented lines in the measured programs.

LLVM_PROFDATA and LLVM_COV override the tools and may hold a command with
arguments (e.g. "xcrun llvm-cov"), as in report.sh. On macOS the tools are
found with xcrun; elsewhere on PATH, then in /usr/lib/llvm-<N>/bin, newest N
first.
"""

import argparse
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from backlog import error_path_lines  # noqa: E402
from combine import Names, load  # noqa: E402

SRC = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def die(msg):
    print(f'check_lines.py: {msg}', file=sys.stderr)
    sys.exit(2)


def tool(name, env):
    if os.environ.get(env):
        return shlex.split(os.environ[env])
    if sys.platform == 'darwin' and shutil.which('xcrun'):
        r = subprocess.run(['xcrun', '--find', name], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return [r.stdout.strip()]
    if shutil.which(name):
        return [shutil.which(name)]

    def version(p):
        m = re.search(r'/llvm-(\d+)/', p)
        return int(m.group(1)) if m else -1
    cands = sorted(glob.glob(f'/usr/lib/llvm-*/bin/{name}'), key=version, reverse=True)
    if cands:
        return [cands[0]]
    die(f'{name} not found (set {env})')


def parse_range(s):
    m = re.match(r'^(.+):(\d+)-(\d+)$', s)
    if not m:
        die(f"'{s}' is not <file>:<first>-<last>")
    a, b = int(m.group(2)), int(m.group(3))
    if a > b:
        die(f"'{s}': first line after last")
    return m.group(1), a, b


def run(cmd, log, env=None, timeout=None):
    with open(log, 'a') as fh:
        fh.write(f'$ {" ".join(cmd)}\n')
        fh.flush()
        try:
            return subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, env=env,
                                  timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            fh.write(f'(killed after {timeout} s)\n')
            return 'timeout'
        except OSError as e:
            fh.write(f'({e})\n')
            return 'cannot run'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('build')
    ap.add_argument('testdir')
    ap.add_argument('ranges', nargs='+')
    ap.add_argument('--procs', default='1 2')
    ap.add_argument('--timeout', type=int, default=300, help='seconds per test run')
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

    work = os.path.realpath(a.keep or tempfile.mkdtemp(prefix='check_lines.'))
    os.makedirs(work, exist_ok=True)
    log = os.path.join(work, 'log.txt')
    prof = os.path.join(work, 'profraw')
    shutil.rmtree(prof, ignore_errors=True)
    os.makedirs(prof)

    # Clean first, so every program is rebuilt and therefore measured below.
    run(['make', '-C', testdir, 'clean', f'CHARMC={charmc}'], log)
    marker = time.time()
    time.sleep(1)  # programs built from here on are newer than the marker
    if run(['make', '-C', testdir, f'CHARMC={charmc}'], log) != 0:
        die(f'building {testdir} failed; see {log}')

    for p in a.procs.split():
        for f in glob.glob(os.path.expanduser('~/.tmp/lct_pmi_file-*')):
            shutil.rmtree(f, ignore_errors=True)
        env = dict(os.environ, LLVM_PROFILE_FILE=os.path.join(prof, '%p-%m.profraw'),
                   TESTRUN_PROCS=p)
        rc = run(['make', '-C', testdir, 'test'], log, env=env, timeout=a.timeout)
        print(f'check_lines.py: test with {p} process(es): '
              f'{"passed" if rc == 0 else f"FAILED ({rc})"}', file=sys.stderr)
        if rc != 0:
            die(f'the test failed with {p} process(es); see {log}')

    raws = glob.glob(os.path.join(prof, '*.profraw'))
    if not raws:
        die(f'the test wrote no profiles (did it run a program?); see {log}')
    merged = os.path.join(work, 'merged.profdata')
    if run([*profdata, 'merge', '-sparse', '-o', merged, *raws], log) != 0:
        die(f'llvm-profdata merge failed; see {log}')

    progs = []
    for root, _, files in os.walk(testdir):
        for f in files:
            p = os.path.join(root, f)
            if os.access(p, os.X_OK) and os.path.getmtime(p) > marker:
                with open(p, 'rb') as fh:
                    if fh.read(4) in (b'\x7fELF', b'\xcf\xfa\xed\xfe'):  # ELF, Mach-O 64
                        progs.append(p)
    progs = [p for p in progs if os.path.basename(p) != 'charmrun']
    if not progs:
        die(f'make built no programs in {testdir}; see {log}')
    for lib in ('libreconverse.so', 'libreconverse.dylib'):
        p = os.path.join(build, 'lib', lib)
        if os.path.exists(p):
            progs.append(p)

    lcov = os.path.join(work, 'run.lcov')
    cmd = [*llvmcov, 'export', '-format=lcov', f'-instr-profile={merged}', progs[0]]
    for p in progs[1:]:
        cmd += ['-object', p]
    with open(lcov, 'w') as out, open(log, 'a') as err:
        if subprocess.run(cmd, stdout=out, stderr=err).returncode != 0:
            die(f'llvm-cov export failed; see {log}')

    files = load(lcov, Names(SRC))
    worst = 0
    for name, first, last in ranges:
        try:
            with open(os.path.join(SRC, name), errors='replace') as fh:
                errors = error_path_lines(fh.read())
        except OSError:
            die(f'{name}: not found in {SRC}')
        cov = files.get(name)
        lines = sorted(l for l in (cov.da if cov else {}) if first <= l <= last)
        if not lines:
            print(f'{name}:{first}-{last}: no instrumented lines in the measured programs '
                  '(wrong file or lines, or no measured program contains this code)')
            worst = 2
            continue
        want = [l for l in lines if l not in errors]
        hit = [l for l in want if cov.da[l] > 0]
        for l in lines:
            note = '' if l in want else '\t(error path)'
            print(f'  {name}:{l}\t{cov.da[l]}{note}')
        # A count of None is lcov's '-': the branch's block never ran.
        missed = sorted((l, b, br) for (l, b, br), n in cov.br.items()
                        if first <= l <= last and not n)
        nbr = sum(1 for (l, _, _) in cov.br if first <= l <= last)
        if nbr:
            print(f'  branch outcomes in the range: {nbr - len(missed)} of {nbr} taken'
                  + ('; never taken: ' + ', '.join(f'line {l} (block {b}, branch {br})'
                                                   for l, b, br in missed) if missed else ''))
        if want and len(hit) == len(want):
            verdict = 'tested'
        elif hit:
            verdict, worst = 'partial', max(worst, 1)
        elif want:
            verdict, worst = 'not reached', max(worst, 1)
        else:
            verdict = 'tested'  # only error-path lines: nothing else to reach
        print(f'{name}:{first}-{last}: {verdict}: {len(hit)} of {len(want)} lines reached '
              f'({len(lines) - len(want)} error-path lines not counted)')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(worst)


if __name__ == '__main__':
    main()
