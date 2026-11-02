#!/usr/bin/env python3
"""The steps of the coverage-tests workflow around the model run.

usage: coverage/backlog_job.py rank --site <charm-coverage clone> --k <1-99> --work <dir>
                                    [--exclude-tsv <backlog.tsv> ...]
       coverage/backlog_job.py finish --start <sha> --keys "<key> ..." --work <dir>
                                      --out <dir> [--build <build dir>]
       coverage/backlog_job.py publish --ranked <backlog.ranked.tsv> --changes <dir>
                                       --keys "<key> ..."

All three run in the root of a checkout of this repository. rank and finish run
in the job that runs the model, before and after it; publish runs in a second
job, in a fresh checkout, with nothing the model's job produced except the
files finish wrote to --out, which it checks again before using. The keys and
the start commit travel as step outputs, which the model cannot edit.

rank
    Takes the newest run of the coverage site whose Charm++ commit is an
    ancestor of HEAD (so a run measured on another branch is never used),
    checks that commit out at <work>/measured with contrib/reconverse at the
    run's reconverse commit, and runs backlog.py there, preserving the statuses
    in coverage/backlog.tsv. It picks the first K open rows, leaving out the
    keys that an --exclude-tsv file (the backlog.tsv of an open backlog pull
    request) gives a status other than open while HEAD's file has them open,
    and writes:
        <work>/backlog.ranked.tsv the ranked backlog (coverage/backlog.tsv too)
        <work>/targets.md         the targets with their code, for the model
    and prints, for $GITHUB_OUTPUT: start_sha=<HEAD> and keys=<the keys>.

finish
    Checks that HEAD is still --start (the model may not commit), removes the
    build and the measured checkout, runs make clean in every test directory
    with changes (putting back any new source file a clean target deleted),
    deletes untracked programs, objects and profiles, and writes to --out:
        tests.patch   the changes under tests/ that are text source files
                      (SOURCE below); anything else is left out and listed
        statuses.tsv  key and status for each --keys target whose row in the
                      edited coverage/backlog.tsv has a valid status
        pr-body.md    <work>/pr-body.md, the model's summary, behind a list
                      of everything left out or refused
    and prints changed=true or changed=false (for $GITHUB_OUTPUT).

publish
    Checks that tests.patch touches only text files under tests/, applies it
    to the index and the tree, rewrites coverage/backlog.tsv as the ranked file
    with the statuses from statuses.tsv for --keys only, validates it with
    backlog.py --check, and stages it. Exits 1 if anything is refused, and
    nothing is committed.
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
from backlog import COLUMNS, STATUS, STATUSES, HeaderError, parse_tsv  # noqa: E402

TSV = 'coverage/backlog.tsv'
GENERATED = re.compile(r'\.(o|a|so|dylib|profraw|profdata|decl\.h|def\.h)$|(^|/)charmrun$')
# What a test directory may add to the repository.
SOURCE = re.compile(r'\.(C|cpp|cc|cxx|c|h|hpp|hh|ci|f|F|f90|F90|py|sh|pl|txt|md|in|mk|'
                    r'dat|cfg|json)$|(^|/)(Makefile|makefile|CMakeLists\.txt|\.gitignore)$')
# Git runs without hooks or the file-system monitor, which are files the model
# could have written.
GIT = ['git', '-c', 'core.hooksPath=/dev/null', '-c', 'core.fsmonitor=false']


def die(msg):
    print(f'backlog_job.py: {msg}', file=sys.stderr)
    sys.exit(2)


def git(*args, check=True, strip=True):
    r = subprocess.run([*GIT, *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        die(f'git {" ".join(args)} failed: {r.stderr.strip()}')
    return r.stdout.strip() if strip else r.stdout


def k_arg(s):
    if not re.fullmatch(r'[1-9][0-9]?', s or ''):
        raise argparse.ArgumentTypeError(f"k must be a number from 1 to 99, not '{s}'")
    return int(s)


def keys_arg(s):
    keys = s.split()
    if not keys or not all(re.fullmatch(r'[0-9a-f]{12}', k) for k in keys):
        raise argparse.ArgumentTypeError('keys must be 12-digit hex keys separated by spaces')
    return keys


def read_tsv(path, strict, what):
    """(rows, problems) of a backlog TSV, or ([], [problem]) when the header
    is wrong or the file is missing."""
    try:
        return parse_tsv(path, strict=strict)
    except HeaderError as e:
        return [], [f'{what}: {e}']
    except OSError as e:
        return [], [f'{what}: {e}']


def is_text_source(path):
    if not SOURCE.search(path):
        return False
    try:
        with open(path, 'rb') as fh:
            return b'\0' not in fh.read(1 << 20)
    except OSError:
        return False


# ---------------------------------------------------------------- rank

def rank(a):
    work = os.path.realpath(a.work)
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(a.site, 'runs', 'index.json')) as fh:
        runs = sorted(json.load(fh), key=lambda r: r['date'], reverse=True)
    run = None
    for r in runs:
        ok = subprocess.run([*GIT, 'merge-base', '--is-ancestor', r['charm'], 'HEAD'],
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

    # The statuses of HEAD's file, to tell an open pull request's own changes
    # from the statuses it inherited.
    before = {r['key']: r['status'] for r in read_tsv(TSV, False, TSV)[0]}

    here = os.path.dirname(os.path.abspath(__file__))
    r = subprocess.run([sys.executable, os.path.join(here, 'backlog.py'), '--src', measured,
                        '--out', TSV, lcov])
    if r.returncode != 0:
        die('backlog.py failed')
    shutil.copy(TSV, os.path.join(work, 'backlog.ranked.tsv'))

    exclude = set()
    for f in a.exclude_tsv or []:
        rows, problems = read_tsv(f, False, f)
        for p in problems:
            print(f'backlog_job.py: warning: {p}', file=sys.stderr)
        exclude |= {r['key'] for r in rows
                    if r['status'] != 'open' and before.get(r['key'], 'open') == 'open'}
    rows, _ = parse_tsv(TSV, strict=True)
    pick = [r for r in rows if r['status'] == 'open' and r['key'] not in exclude][:a.k]
    if not pick:
        die('no open rows to work on')

    with open(os.path.join(work, 'targets.md'), 'w') as out:
        out.write(f'Targets: {len(pick)} open rows of coverage/backlog.tsv. Line numbers are '
                  f'those of commit {run["charm"]}, checked out at {measured}; find the same '
                  'code in this checkout by its function and text.\n\n')
        if exclude:
            out.write(f'({len(exclude)} targets of an open backlog pull request were left '
                      'out.)\n\n')
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
    print(f'start_sha={git("rev-parse", "HEAD")}')
    print(f'keys={" ".join(r["key"] for r in pick)}')


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


def make_clean(d, notes):
    """make clean in d, with a time limit, keeping the untracked source files
    the clean target may delete (some glob widely)."""
    kept = {}
    for code, p in changed_paths():
        if code == '??' and os.path.dirname(p) == d and is_text_source(p):
            with open(p, 'rb') as fh:
                kept[p] = fh.read()
    try:
        subprocess.run(['make', '-C', d, 'clean'], capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        notes.append(f'`make clean` in `{d}` did not finish in 120 s and was killed')
    for p, data in kept.items():
        if not os.path.exists(p):
            with open(p, 'wb') as fh:
                fh.write(data)
            notes.append(f'`make clean` in `{d}` deletes `{p}`; put back, but the clean '
                         'target should not match source files')


def finish(a):
    work = os.path.realpath(a.work)
    out = os.path.realpath(a.out)
    os.makedirs(out, exist_ok=True)
    notes = []
    head = git('rev-parse', 'HEAD')
    if head != a.start:
        die(f'HEAD moved from {a.start} to {head}: the model made commits; nothing is opened')

    git('worktree', 'remove', '--force', os.path.join(work, 'measured'), check=False)
    shutil.rmtree(os.path.join(work, 'measured'), ignore_errors=True)
    if a.build:
        shutil.rmtree(a.build, ignore_errors=True)

    # Build outputs in the test directories the model touched.
    dirs = sorted({os.path.dirname(p) for _, p in changed_paths() if p.startswith('tests/')})
    for d in dirs:
        while d and not os.path.exists(os.path.join(d, 'Makefile')):
            d = os.path.dirname(d)
        if d:
            make_clean(d, notes)
    for code, p in changed_paths():
        if code == '??' and (GENERATED.search(p) or is_program(p)):
            os.remove(p)

    # Under tests/: text source files only. Elsewhere: nothing.
    for code, p in changed_paths():
        if p == TSV:
            continue
        if p.startswith('tests/') and 'D' not in code and is_text_source(p):
            continue
        if 'D' in code:
            git('checkout', 'HEAD', '--', p, check=False)
            notes.append(f'put back a deleted file: `{p}`')
        elif code == '??':
            os.remove(p)
            notes.append(f'left out a new file that is not a text source file: `{p}`'
                         if p.startswith('tests/') else
                         f'put back a change outside tests/ and {TSV}: `{p}`')
        else:
            git('checkout', 'HEAD', '--', p, check=False)
            notes.append(f'put back a change to a file that is not a text source file: `{p}`'
                         if p.startswith('tests/') else
                         f'put back a change outside tests/ and {TSV}: `{p}`')

    git('add', '-A', '--', 'tests')
    patch = git('diff', '--cached', '--', 'tests', strip=False)
    tests_changed = bool(patch.strip())
    if tests_changed:
        with open(os.path.join(out, 'tests.patch'), 'w') as fh:
            fh.write(patch)

    # The targets' statuses, from the edited backlog.
    edited = {}
    rows, problems = read_tsv(TSV, True, f'{TSV} as edited')
    for r in rows:
        edited.setdefault(r['key'], r['status'])
    if problems:
        notes.append(f'{TSV} as edited had problems (only the targets\' rows are used): '
                     + '; '.join(problems[:5]))
    accepted = {}
    for k in a.keys:
        s = edited.get(k, 'open')
        if s == 'open':
            notes.append(f'key {k}: no status recorded; left open')
        elif STATUS.match(s):
            accepted[k] = s
        else:
            notes.append(f'key {k}: status "{s}" is not one of {", ".join(STATUSES)}; '
                         'left open')
    with open(os.path.join(out, 'statuses.tsv'), 'w') as fh:
        fh.write(''.join(f'{k}\t{s}\n' for k, s in accepted.items()))

    body = os.path.join(work, 'pr-body.md')
    text = open(body).read() if os.path.exists(body) else '(The model wrote no summary.)\n'
    head_note = ('GitHub starts no CI for a pull request a workflow opens: '
                 '**close and reopen this pull request to run CI.**\n\n')
    if notes:
        head_note += ('**Checks before opening this pull request:**\n'
                      + ''.join(f'- {n}\n' for n in notes) + '\n')
    with open(os.path.join(out, 'pr-body.md'), 'w') as fh:
        fh.write(head_note + text)

    print(f'changed={"true" if tests_changed or accepted else "false"}')
    for n in notes:
        print(f'backlog_job.py: {n}', file=sys.stderr)


# ---------------------------------------------------------------- publish

def publish(a):
    changes = os.path.realpath(a.changes)
    problems = []

    # The statuses first, then the patch: nothing is applied once anything is
    # refused.
    ranked, p1 = read_tsv(a.ranked, True, 'the ranked backlog')
    problems += p1
    statuses = {}
    try:
        with open(os.path.join(changes, 'statuses.tsv')) as fh:
            for ln in fh:
                k, _, s = ln.rstrip('\r\n').partition('\t')
                if k in a.keys and STATUS.match(s) and s != 'open':
                    statuses[k] = s
                elif k:
                    problems.append(f'statuses.tsv: `{k}\t{s}` is not a target with a valid status')
    except OSError as e:
        problems.append(f'statuses.tsv: {e}')

    patch = os.path.join(changes, 'tests.patch')
    if os.path.exists(patch):
        stat = git('apply', '--numstat', '-z', patch, strip=False)
        for e in stat.split('\0'):
            if not e.strip():
                continue
            added, deleted, path = e.split('\t', 2)
            if not path.startswith('tests/') or '/../' in f'/{path}/':
                problems.append(f'the patch touches `{path}`, outside tests/')
            elif added == '-' or deleted == '-':
                problems.append(f'the patch adds a binary file: `{path}`')
            elif not SOURCE.search(path):
                problems.append(f'the patch adds a file that is not a source file: `{path}`')
    if problems:
        for p in problems:
            print(f'backlog_job.py: refused: {p}', file=sys.stderr)
        sys.exit(1)

    if os.path.exists(patch):
        r = subprocess.run([*GIT, 'apply', '--index', '--include=tests/*', patch],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f'backlog_job.py: refused: the patch does not apply: {r.stderr.strip()}',
                  file=sys.stderr)
            sys.exit(1)
    for r in ranked:
        if r['key'] in statuses:
            r['status'] = statuses[r['key']]
    with open(TSV, 'w') as fh:
        fh.write('\t'.join(COLUMNS) + '\n')
        for r in ranked:
            fh.write('\t'.join(r[c] for c in COLUMNS) + '\n')
    here = os.path.dirname(os.path.abspath(__file__))
    r = subprocess.run([sys.executable, os.path.join(here, 'backlog.py'), '--check', TSV],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f'backlog_job.py: refused: {TSV} does not validate: {r.stderr.strip()}',
              file=sys.stderr)
        sys.exit(1)
    git('add', '--', TSV)
    staged = [p for p in git('diff', '--cached', '--name-only').split('\n') if p]
    tests_changed = any(p.startswith('tests/') for p in staged)
    print(f'backlog_job.py: staged {len(staged)} files; statuses set for '
          f'{len(statuses)} of {len(a.keys)} targets', file=sys.stderr)
    # A re-ranked backlog alone is not a pull request.
    print(f'changed={"true" if tests_changed or statuses else "false"}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('rank')
    r.add_argument('--site', required=True)
    r.add_argument('--k', type=k_arg, required=True)
    r.add_argument('--exclude-tsv', nargs='*')
    r.add_argument('--work', required=True)
    f = sub.add_parser('finish')
    f.add_argument('--start', required=True)
    f.add_argument('--keys', type=keys_arg, required=True)
    f.add_argument('--work', required=True)
    f.add_argument('--out', required=True)
    f.add_argument('--build')
    p = sub.add_parser('publish')
    p.add_argument('--ranked', required=True)
    p.add_argument('--changes', required=True)
    p.add_argument('--keys', type=keys_arg, required=True)
    a = ap.parse_args()
    if not os.path.isdir('.git') and not os.path.isfile('.git'):
        die('run this in the root of the repository')
    {'rank': rank, 'finish': finish, 'publish': publish}[a.cmd](a)


if __name__ == '__main__':
    main()
