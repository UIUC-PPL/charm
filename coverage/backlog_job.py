#!/usr/bin/env python3
"""The steps of the coverage-tests workflow around the model run.

usage: coverage/backlog_job.py rank --site <charm-coverage clone> --k <1-99>
                                    [--exclude-tsv <backlog.tsv> ...] [--work ..]
       coverage/backlog_job.py finish --build <build dir> [--work ..]

Both run in the root of a full clone (fetch-depth 0) of this repository and
keep their files in --work (default: the parent directory).

rank
    Takes the newest run of the coverage site whose Charm++ commit is an
    ancestor of HEAD (so a run measured on another branch is never used),
    checks that commit out at <work>/measured with contrib/reconverse at the
    run's reconverse commit, and runs backlog.py there, preserving the statuses
    in coverage/backlog.tsv. It picks the first K open rows, leaving out the
    keys that any --exclude-tsv file (the backlog.tsv of an open backlog pull
    request) already gives a status other than open, and writes:
        coverage/backlog.tsv      the ranked backlog
        <work>/backlog.ranked.tsv a copy, which finish starts from
        <work>/targets.md         the targets with their code, for the model
        <work>/targets.keys       their keys
        <work>/start-sha          HEAD, which finish checks

finish
    Checks that HEAD has not moved (the model may not commit), removes the
    build and the measured checkout, runs make clean in every test directory
    with changes, deletes untracked programs, objects and profiles, and puts
    back every change outside tests/ and coverage/backlog.tsv. Rewrites
    coverage/backlog.tsv as the ranked file with only the targets' statuses
    taken from the edited one, and only valid ones. Everything it put back or
    refused is listed at the top of <work>/pr-body.md. Stages the result and
    prints changed=true or changed=false (for $GITHUB_OUTPUT).
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from backlog import COLUMNS, STATUS, STATUSES, parse_tsv  # noqa: E402

TSV = 'coverage/backlog.tsv'
GENERATED = re.compile(r'\.(o|a|so|dylib|profraw|profdata|decl\.h|def\.h)$|(^|/)charmrun$')


def die(msg):
    print(f'backlog_job.py: {msg}', file=sys.stderr)
    sys.exit(2)


def git(*args, check=True, strip=True):
    r = subprocess.run(['git', *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        die(f'git {" ".join(args)} failed: {r.stderr.strip()}')
    return r.stdout.strip() if strip else r.stdout


def k_arg(s):
    if not re.fullmatch(r'[1-9][0-9]?', s or ''):
        raise argparse.ArgumentTypeError(f"k must be a number from 1 to 99, not '{s}'")
    return int(s)


# ---------------------------------------------------------------- rank

def rank(a):
    work = os.path.realpath(a.work)
    with open(os.path.join(a.site, 'runs', 'index.json')) as fh:
        runs = sorted(json.load(fh), key=lambda r: r['date'], reverse=True)
    run = None
    for r in runs:
        ok = subprocess.run(['git', 'merge-base', '--is-ancestor', r['charm'], 'HEAD'],
                            capture_output=True).returncode == 0
        if ok:
            run = r
            break
        print(f'backlog_job.py: skipping coverage run {r["id"]}: charm {r["charm"]} is not '
              'on this branch', file=sys.stderr)
    if not run:
        die('no published coverage run measured a commit of this branch')
    lcov = os.path.join(a.site, 'runs', run['id'], 'ci+all+ex.lcov.gz')
    if not os.path.exists(lcov):
        die(f'{lcov} not found')
    print(f'backlog_job.py: coverage run {run["id"]}: charm {run["charm"]}, '
          f'reconverse {run["reconverse"]}', file=sys.stderr)

    measured = os.path.join(work, 'measured')
    if os.path.exists(measured):
        git('worktree', 'remove', '--force', measured, check=False)
        shutil.rmtree(measured, ignore_errors=True)
    git('worktree', 'add', '--quiet', '--detach', measured, run['charm'])
    git('-C', measured, 'submodule', 'update', '--quiet', '--init', 'contrib/reconverse')
    rec = os.path.join(measured, 'contrib', 'reconverse')
    if not git('-C', rec, 'rev-parse', 'HEAD').startswith(run['reconverse']):
        git('-C', rec, 'fetch', '--quiet', 'origin', check=False)
        git('-C', rec, 'checkout', '--quiet', '--detach', run['reconverse'])

    here = os.path.dirname(os.path.abspath(__file__))
    r = subprocess.run([sys.executable, os.path.join(here, 'backlog.py'), '--src', measured,
                        '--out', TSV, lcov])
    if r.returncode != 0:
        die('backlog.py failed')
    shutil.copy(TSV, os.path.join(work, 'backlog.ranked.tsv'))

    exclude = set()
    for f in a.exclude_tsv or []:
        rows, _ = parse_tsv(f, strict=False)
        exclude |= {r['key'] for r in rows if r['status'] != 'open'}
    rows, _ = parse_tsv(TSV, strict=True)
    pick = [r for r in rows if r['status'] == 'open' and r['key'] not in exclude][:a.k]
    if not pick:
        die('no open rows to work on')

    with open(os.path.join(work, 'targets.keys'), 'w') as fh:
        fh.write(''.join(r['key'] + '\n' for r in pick))
    with open(os.path.join(work, 'start-sha'), 'w') as fh:
        fh.write(git('rev-parse', 'HEAD') + '\n')
    with open(os.path.join(work, 'targets.md'), 'w') as out:
        out.write(f'Targets: {len(pick)} open rows of coverage/backlog.tsv. Line numbers are '
                  f'those of commit {run["charm"]}, checked out at {measured}; find the same '
                  'code in this checkout by its function and text.\n\n')
        if exclude:
            out.write(f'({len(exclude)} keys of an open backlog pull request were left out.)\n\n')
        for i, r in enumerate(pick, 1):
            first, last = map(int, r['lines'].split('-'))
            with open(os.path.join(measured, r['file']), errors='replace') as fh:
                src = fh.read().split('\n')
            body = '\n'.join(src[max(0, first - 4):last + 2])
            out.write(f"## {i}. key {r['key']} (tier {r['tier']}, priority {r['priority']})\n\n"
                      f"{r['file']} lines {r['lines']} ({r['uncovered']} uncovered) in "
                      f"{r['function']}\n\n```cpp\n{body}\n```\n\n")
    print(f'backlog_job.py: picked {len(pick)}: {" ".join(r["key"] for r in pick)}',
          file=sys.stderr)


# ---------------------------------------------------------------- finish

def changed_paths():
    out = git('status', '--porcelain', '--untracked-files=all', '-z', strip=False)
    paths = []
    entries = out.split('\0')
    i = 0
    while i < len(entries):
        e = entries[i]
        i += 1
        if not e:
            continue
        code, path = e[:2], e[3:]
        if code[0] in 'RC':  # a rename's source follows
            i += 1
        paths.append((code, path))
    return paths


def is_program(path):
    try:
        with open(path, 'rb') as fh:
            return fh.read(4) in (b'\x7fELF', b'\xcf\xfa\xed\xfe')
    except OSError:
        return False


def finish(a):
    work = os.path.realpath(a.work)
    notes = []
    start = open(os.path.join(work, 'start-sha')).read().strip()
    head = git('rev-parse', 'HEAD')
    if head != start:
        die(f'HEAD moved from {start} to {head}: the model made commits; nothing is opened')

    measured = os.path.join(work, 'measured')
    git('worktree', 'remove', '--force', measured, check=False)
    shutil.rmtree(measured, ignore_errors=True)
    if a.build:
        shutil.rmtree(a.build, ignore_errors=True)

    # Build outputs in the test directories the model touched.
    dirs = sorted({os.path.dirname(p) for _, p in changed_paths() if p.startswith('tests/')})
    for d in dirs:
        while d and not os.path.exists(os.path.join(d, 'Makefile')):
            d = os.path.dirname(d)
        if d:
            subprocess.run(['make', '-C', d, 'clean'], capture_output=True)
    for code, p in changed_paths():
        if code == '??' and (GENERATED.search(p) or is_program(p)):
            os.remove(p)

    # Nothing outside tests/ and the backlog.
    for code, p in changed_paths():
        if p.startswith('tests/') or p == TSV:
            continue
        if code == '??':
            os.remove(p)
        else:
            git('checkout', 'HEAD', '--', p, check=False)
        notes.append(f'put back a change outside tests/ and {TSV}: `{p}`')
    for code, p in changed_paths():
        if 'D' in code:
            git('checkout', 'HEAD', '--', p, check=False)
            notes.append(f'put back a deleted file: `{p}`')

    # The backlog: the ranked file, with the targets' statuses from the edited one.
    keys = open(os.path.join(work, 'targets.keys')).read().split()
    ranked, _ = parse_tsv(os.path.join(work, 'backlog.ranked.tsv'), strict=True)
    edited = {}
    if os.path.exists(TSV):
        rows, problems = parse_tsv(TSV, strict=True)
        for r in rows:
            edited.setdefault(r['key'], r['status'])
        if problems:
            notes.append(f'{TSV} as edited had problems (only the targets\' rows are used): '
                         + '; '.join(problems[:5]))
    for r in ranked:
        if r['key'] in keys:
            s = edited.get(r['key'], 'open')
            if STATUS.match(s):
                r['status'] = s
            else:
                notes.append(f'key {r["key"]}: status "{s}" is not one of '
                             f'{", ".join(STATUSES)}; left open')
    for k in keys:
        if edited.get(k, 'open') == 'open':
            notes.append(f'key {k}: no status recorded; left open')
    with open(TSV, 'w') as fh:
        fh.write('\t'.join(COLUMNS) + '\n')
        for r in ranked:
            fh.write('\t'.join(r[c] for c in COLUMNS) + '\n')

    body = os.path.join(work, 'pr-body.md')
    text = open(body).read() if os.path.exists(body) else '(The model wrote no summary.)\n'
    head_note = ('GitHub starts no CI for a pull request a workflow opens: '
                 '**close and reopen this pull request to run CI.**\n\n')
    if notes:
        head_note += ('**Checks before opening this pull request:**\n'
                      + ''.join(f'- {n}\n' for n in notes) + '\n')
    with open(body, 'w') as fh:
        fh.write(head_note + text)

    git('add', '-A', '--', 'tests', TSV)
    staged = git('diff', '--cached', '--name-only')
    tests_changed = any(p.startswith('tests/') for p in staged.split('\n') if p)
    statuses = any(edited.get(k, 'open') != 'open' for k in keys)
    print(f'changed={"true" if tests_changed or statuses else "false"}')
    for n in notes:
        print(f'backlog_job.py: {n}', file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('rank')
    r.add_argument('--site', required=True)
    r.add_argument('--k', type=k_arg, required=True)
    r.add_argument('--exclude-tsv', nargs='*')
    r.add_argument('--work', default='..')
    f = sub.add_parser('finish')
    f.add_argument('--build')
    f.add_argument('--work', default='..')
    a = ap.parse_args()
    if not os.path.isdir('.git') and not os.path.isfile('.git'):
        die('run this in the root of the repository')
    (rank if a.cmd == 'rank' else finish)(a)


if __name__ == '__main__':
    main()
