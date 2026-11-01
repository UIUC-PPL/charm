#!/usr/bin/env python3
"""Ranks the uncovered code of the runtime core into a backlog of test targets.

usage: coverage/backlog.py [--src <repo>] [--out coverage/backlog.tsv] [--top N] <tier>.lcov[.gz]

Reads an lcov export from coverage/report.sh (or the .lcov.gz published with
each run at https://charmplusplus.org/charm-coverage/) and writes one row per
block of uncovered code in src/ck-core and contrib/reconverse/src. A block is
a maximal run of instrumented lines with a hit count of 0 inside one function.

Each block gets a tier from its file (TIERS below) and a priority:

    tier weight * min(uncovered lines, CAP) * (1 + log2(1 + commits touching the file
                                                        in the last 90 days))

Commits are counted with git in --src; a shallow clone counts none and a
warning says so.

Blocks of at most SMALL lines whose source only aborts, asserts or prints are
left out and counted in the summary: they are error paths, not test targets.

Rows are keyed by file, function and a hash of the block's source text, so a
row keeps its key when lines above it move. When --out already exists, the
status of every row whose key is still present is kept; the status is one of
open, tested, unreachable, hpc-only or skip:<reason>, and new rows are open.
A row whose block is no longer uncovered disappears.

Writes the TSV sorted by priority, highest first, and a summary to stderr;
--top N also prints the first N open rows.
"""

import argparse
import collections
import gzip
import hashlib
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from combine import Names, load  # noqa: E402

CORE = ('src/ck-core/', 'contrib/reconverse/src/')

# Tier A: the protocols (location, delivery, reductions, callbacks, quiescence,
# zero copy, checkpoint) and the scheduler and threads under them. Tier B: code
# that is probably dead on this line or not reachable on a CI runner (in-memory
# checkpoint, CCS, persistent communication, shared-memory layers, old
# schedulers). Tier C: the rest of the two directories.
TIERS = [
    ('A', re.compile(r'^src/ck-core/(cklocation|cklocrec|ckarray\w*|ck|ckreduction|ckmulticast|'
                     r'ckcallback\w*|ckrdma|ckfutures|qd|waitqd|init|ckcheckpoint|objid|'
                     r'ckmigratable|cksyncbarrier|XArraySectionReducer)\.(C|h)$|'
                     r'^contrib/reconverse/src/(convcore|threads|collectives|conv-rdma|'
                     r'scheduler|queueing|conv-conds|cldb\w*)\.(cpp|C|h)$')),
    ('B', re.compile(r'ckmemcheckpoint|/ccs/|persist-comm|cmishm|scheduler_old|'
                     r'scheduler_randomized|msgmgr|mempool')),
    ('C', re.compile(r'')),
]
WEIGHT = {'A': 10, 'B': 1, 'C': 3}

# A tier-A block in one of these functions is tier C: placement maps other than
# the default ones, debugging, statistics and printing helpers, and command-line
# parsing, which share files with the protocols but are not part of them.
DEMOTE = re.compile(r'(Hilbert|ReadFile|Simple1DFile|ConfigurableRR|Block|Cld|Prop)\w*Map|'
                    r'Loader|arrInfo|[Dd]ebug|[Ss]tats|[Pp]rint|[Dd]ump|[Tt]race|'
                    r'CmiGetArg|CmiArg|CmiReadSize|CmiScanf|CmiAbort')

# A block's size counts up to CAP lines, so one large unused feature does not
# outrank every protocol path.
CAP = 30

SMALL = 6
TRIVIAL = re.compile(r'Abort|CkError|CmiError|Assert|fprintf\s*\(\s*stderr|'
                     r'\bDEB\w*\s*\(|CkPrintf|CmiPrintf|^\s*[{}]?\s*$|^\s*(break|return)\b')

STATUS = re.compile(r'^(open|tested|unreachable|hpc-only|skip:.+)$')
COLUMNS = ['priority', 'tier', 'status', 'file', 'lines', 'uncovered', 'churn90',
           'function', 'key']


def die(msg):
    print(f'backlog.py: {msg}', file=sys.stderr)
    sys.exit(2)


def tier_of(name):
    for t, rx in TIERS:
        if rx.search(name):
            return t
    return 'C'


def read_lcov(path, names):
    if path.endswith('.gz'):
        with tempfile.NamedTemporaryFile('wb', suffix='.lcov', delete=False) as tmp:
            with gzip.open(path, 'rb') as src:
                shutil.copyfileobj(src, tmp)
        try:
            return load(tmp.name, names)
        finally:
            os.unlink(tmp.name)
    return load(path, names)


def churn(src, files):
    """Commits in the last 90 days touching each file: charm's own files from git log
    in src, reconverse's from git log in its submodule checkout."""
    out = churn_in(src, '', files, ['src/ck-core/'])
    out.update(churn_in(os.path.join(src, 'contrib', 'reconverse'), 'contrib/reconverse/',
                        files, ['src/']))
    return out


def churn_in(repo, prefix, files, paths):
    out = collections.Counter()
    try:
        shallow = subprocess.run(['git', '-C', repo, 'rev-parse', '--is-shallow-repository'],
                                 capture_output=True, text=True, check=True).stdout.strip()
        if shallow == 'true':
            # Enough history is enough: the oldest commit must predate the window.
            oldest = subprocess.run(['git', '-C', repo, 'log', '--reverse', '--format=%ct'],
                                    capture_output=True, text=True, check=True).stdout.split()
            if not oldest or time.time() - int(oldest[0]) < 90 * 86400:
                print(f'backlog.py: warning: {repo} is a shallow clone with less than 90 days '
                      'of history, so its files get no commit counts; fetch more history to '
                      'include them', file=sys.stderr)
                return out
        log = subprocess.run(['git', '-C', repo, 'log', '--since=90.days', '--format=',
                              '--name-only', '--', *paths],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        print(f'backlog.py: warning: git log failed in {repo}, so its files get no commit '
              'counts', file=sys.stderr)
        return out
    for ln in log.splitlines():
        if prefix + ln in files:
            out[prefix + ln] += 1
    return out


def demangle(names):
    names = sorted(set(names))
    exe = shutil.which('c++filt') or shutil.which('llvm-cxxfilt')
    if not exe or not names:
        return {n: n for n in names}
    r = subprocess.run([exe], input='\n'.join(names), capture_output=True, text=True)
    out = r.stdout.splitlines()
    if r.returncode != 0 or len(out) != len(names):
        return {n: n for n in names}
    return dict(zip(names, out))


def blocks(cov):
    """Yields each block as the list of its uncovered line numbers."""
    starts = sorted((line, f) for f, line in cov.fn.items() if line > 0)

    def func_at(line):
        best = ''
        for s, f in starts:
            if s > line:
                break
            best = f
        return best

    run = []
    for line in sorted(cov.da):
        if cov.da[line] == 0 and (not run or func_at(line) == func_at(run[0])):
            run.append(line)
            continue
        if run:
            yield run
        run = [line] if cov.da[line] == 0 else []
    if run:
        yield run


def trivial(run, text):
    if len(run) > SMALL:
        return False
    return all(TRIVIAL.search(text[l - 1]) for l in run if 0 < l <= len(text))


def key_of(name, func, run, text):
    body = '\n'.join(text[l - 1].strip() for l in run if 0 < l <= len(text))
    h = hashlib.sha1(f'{name}\0{func}\0{body}'.encode()).hexdigest()[:12]
    return h


def read_status(path):
    status = {}
    if not os.path.exists(path):
        return status
    with open(path) as fh:
        header = fh.readline().rstrip('\n').split('\t')
        if header != COLUMNS:
            die(f'{path}: unexpected header; expected {" ".join(COLUMNS)}')
        for no, ln in enumerate(fh, 2):
            row = dict(zip(COLUMNS, ln.rstrip('\n').split('\t')))
            if not STATUS.match(row.get('status', '')):
                die(f'{path}:{no}: status "{row.get("status")}" is not open, tested, '
                    'unreachable, hpc-only or skip:<reason>')
            status[row['key']] = row['status']
    return status


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('lcov')
    ap.add_argument('--src', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    ap.add_argument('--out', default=None, help='default: <src>/coverage/backlog.tsv')
    ap.add_argument('--top', type=int, default=0)
    a = ap.parse_args()
    src = os.path.realpath(a.src)
    out = a.out or os.path.join(src, 'coverage', 'backlog.tsv')

    files = read_lcov(a.lcov, Names(src))
    core = {n: c for n, c in files.items()
            if n.startswith(CORE) and not n.endswith(' [gen]')}
    if not core:
        die(f'{a.lcov}: no files under {" or ".join(CORE)}')
    commits = churn(src, set(core))
    old = read_status(out)
    pretty = demangle(f for c in core.values() for f in c.fn)

    rows, skipped, missing = [], collections.Counter(), []
    for name in sorted(core):
        path = os.path.join(src, name)
        try:
            with open(path, errors='replace') as fh:
                text = fh.read().split('\n')
        except OSError:
            missing.append(name)
            continue
        t = tier_of(name)
        cov = core[name]
        for run in blocks(cov):
            func = ''
            for f, line in sorted(cov.fn.items(), key=lambda x: x[1]):
                if 0 < line <= run[0]:
                    func = f
            if trivial(run, text):
                skipped['C' if t == 'A' and DEMOTE.search(pretty.get(func, func)) else t] += 1
                continue
            k = key_of(name, func, run, text)
            bt = t
            if t == 'A' and DEMOTE.search(pretty.get(func, func)):
                bt = 'C'
            rows.append({
                'priority': round(WEIGHT[bt] * min(len(run), CAP)
                                  * (1 + math.log2(1 + commits[name]))),
                'tier': bt,
                'status': old.get(k, 'open'),
                'file': name,
                'lines': f'{run[0]}-{run[-1]}',
                'uncovered': len(run),
                'churn90': commits[name],
                'function': func,
                'key': k,
            })

    for r in rows:
        r['function'] = re.sub(r'\s+', ' ', pretty.get(r['function'], r['function']))[:120]
    rows.sort(key=lambda r: (-r['priority'], r['file'], int(r['lines'].split('-')[0])))

    with open(out, 'w') as fh:
        fh.write('\t'.join(COLUMNS) + '\n')
        for r in rows:
            fh.write('\t'.join(str(r[c]) for c in COLUMNS) + '\n')

    by = collections.defaultdict(collections.Counter)
    for r in rows:
        by[r['tier']][r['status'].split(':')[0]] += 1
        by[r['tier']]['lines'] += r['uncovered']
    print(f'backlog.py: wrote {len(rows)} rows to {out}', file=sys.stderr)
    for t in 'ABC':
        c = by[t]
        print(f'  tier {t}: {sum(v for k, v in c.items() if k != "lines")} blocks, '
              f'{c["lines"]} uncovered lines; open {c["open"]}, tested {c["tested"]}, '
              f'unreachable {c["unreachable"]}, hpc-only {c["hpc-only"]}, skip {c["skip"]}; '
              f'{skipped[t]} error-path blocks left out', file=sys.stderr)
    if missing:
        print(f'backlog.py: warning: {len(missing)} files in the lcov are not in --src '
              f'(different commit?): {", ".join(missing[:5])}', file=sys.stderr)
    if a.top:
        print('\t'.join(COLUMNS[:-1]))
        for r in [r for r in rows if r['status'] == 'open'][:a.top]:
            print('\t'.join(str(r[c]) for c in COLUMNS[:-1]))


if __name__ == '__main__':
    main()
