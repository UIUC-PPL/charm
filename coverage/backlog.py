#!/usr/bin/env python3
"""Ranks the uncovered code of the runtime core into a backlog of test targets.

usage: coverage/backlog.py [--src <repo>] [--out coverage/backlog.tsv] [--top N] <tier>.lcov[.gz]
       coverage/backlog.py --check coverage/backlog.tsv

Reads an lcov export from coverage/report.sh (or the .lcov.gz published with
each run at https://charmplusplus.org/charm-coverage/) and writes one row per
block of uncovered code in src/ck-core and contrib/reconverse/src. A block is
a maximal run of instrumented lines with a hit count of 0 inside one function.
A line's function is the innermost function whose extent (from its first line
in the lcov to its matching closing brace in the source) contains the line.

Each block gets a tier from its file (TIERS below) and a priority:

    tier weight * min(uncovered lines, CAP) * (1 + log2(1 + commits touching the file
                                                        in the last 90 days))

Commits are counted with git in --src and in its contrib/reconverse checkout;
a shallow clone with less than 90 days of history counts none and a warning
says so.

A block that is only an error path is left out and counted in the summary: a
block in which some line is a call that aborts, asserts or prints (possibly
over several lines) and every other line is only braces, else, break or a bare
return (error_path_lines below). A block of only a bare return or break is a
normal early exit and stays in. Lines outside every function (macro bodies,
which llvm-cov attributes to the #define) are left out too.

Rows are keyed by file, the text of the function's signature (which names
the function without depending on a demangler or on which template
instantiations exist), the block's text, and the block's position among the
blocks with that signature and text, so a row keeps its key when lines above
it move. When --out already exists, the status of every row whose key is
still present is kept; the status is one of STATUSES, and new rows are open. A
row whose block is no longer uncovered disappears; the code a partial test left
uncovered appears as a new row. Reading the old file skips blank or short rows
and treats an unknown status as open, with a warning, but stops if the header
is not the expected one (nothing may silently reset every status); --check
validates a file strictly (exit 1 on any problem).

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

sys.dont_write_bytecode = True
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

# Calls that make a statement an error path. DEB* and DEBUG* are the debug
# print macros of ck-core.
ERROR_CALL = re.compile(r'\b(CkAbort|CmiAbort|CkError|CmiError|CkPrintf|CmiPrintf|fprintf|printf|'
                        r'CkAssert|CkAssertMsg|CmiAssert|CmiAssertMsg|CkEnforce|CmiEnforce|'
                        r'DEB\w*|DEBUG\w*)\s*\(')
ERROR_TOKENS = {'{', '}', ';', 'else', 'break', 'return'}

STATUSES = ('open', 'tested', 'partial', 'unreachable', 'hpc-only', 'skip:<reason>')
STATUS = re.compile(r'^(open|tested|partial|unreachable|hpc-only|skip:\S.*)$')
COLUMNS = ['priority', 'tier', 'status', 'file', 'lines', 'uncovered', 'churn90',
           'function', 'key']


def die(msg):
    print(f'backlog.py: {msg}', file=sys.stderr)
    sys.exit(2)


def warn(msg):
    print(f'backlog.py: warning: {msg}', file=sys.stderr)


def tier_of(name):
    for t, rx in TIERS:
        if rx.search(name):
            return t
    return 'C'


# ---------------------------------------------------------------- source text

def blank_comments_and_strings(src):
    """The source with comments and the contents of string and character
    literals replaced by spaces; newlines and every offset are kept."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                out[i] = ' '
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if src[k] != '\n':
                    out[k] = ' '
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and src[j] != c and src[j] != '\n':
                if src[j] == '\\':
                    out[j] = ' '
                    j += 1
                if j < n and src[j] != '\n':
                    out[j] = ' '
                j += 1
            i = j + 1
        else:
            i += 1
    return ''.join(out)


def line_starts(src):
    starts = [0]
    for m in re.finditer('\n', src):
        starts.append(m.end())
    return starts


def line_of(starts, off):
    lo, hi = 0, len(starts) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if starts[mid] <= off:
            lo = mid
        else:
            hi = mid - 1
    return lo + 1


def match_close(code, i, open_='(', close=')'):
    """Offset just past the bracket that closes the one at i, or -1."""
    depth = 0
    for j in range(i, len(code)):
        if code[j] == open_:
            depth += 1
        elif code[j] == close:
            depth -= 1
            if depth == 0:
                return j + 1
    return -1


def error_path_lines(src):
    """(calls, filler), two sets of 1-based line numbers. calls: lines that are
    an aborting, asserting or printing call statement, possibly with braces,
    else, break or a bare return around it on the same line, or lines entirely
    inside such a call spanning several lines. filler: lines of only braces,
    semicolons, else, break or a bare return."""
    code = blank_comments_and_strings(src)
    starts = line_starts(src)
    inside = bytearray(len(code))
    for m in ERROR_CALL.finditer(code):
        end = match_close(code, m.end() - 1)
        if end < 0:
            continue
        semi = re.match(r'\s*;', code[end:])
        if semi:
            end += semi.end()
        for k in range(m.start(), end):
            inside[k] = 1
    calls, filler = set(), set()
    for no, s in enumerate(starts, 1):
        e = starts[no] - 1 if no < len(starts) else len(code)
        rest = ''.join(' ' if inside[k] else code[k] for k in range(s, e))
        toks = re.findall(r'\w+|[^\s\w]', rest)
        if not all(t in ERROR_TOKENS for t in toks):
            continue
        if any(inside[s:e]):
            calls.add(no)
        elif toks:
            filler.add(no)
    return calls, filler


def error_path_block(lines, calls, filler):
    """Whether a block (its line numbers) is only an error path: some line is
    an error call and every line is an error call or filler."""
    return any(l in calls for l in lines) and all(l in calls or l in filler for l in lines)


def not_counted(lines, calls, filler):
    """The lines of a range that a test need not reach: the error calls, and
    the filler around them when the range has any."""
    if any(l in calls for l in lines):
        return {l for l in lines if l in calls or l in filler}
    return set()


def function_extents(src, fn):
    """{name: (first, last, signature)} for each function in fn ({name: first
    line}): last is the line of the brace matching the first '{' at or after
    the function's first line (the first line alone if none is found within
    40 lines), and signature the text of the function's signature (see
    signature_text), or '' without a brace."""
    code = blank_comments_and_strings(src)
    starts = line_starts(src)
    out = {}
    for name, first in fn.items():
        if first <= 0 or first > len(starts):
            continue
        b = code.find('{', starts[first - 1])
        if b < 0 or line_of(starts, b) > first + 40:
            out[name] = (first, first, '')
            continue
        e = match_close(code, b, '{', '}')
        out[name] = (first, line_of(starts, e - 1) if e > 0 else first,
                     signature_text(code, starts, b))
    return out


def signature_text(code, starts, brace):
    """The source text before a function's opening brace back to the end of
    the previous declaration or statement (a line with ; { or }, or a
    preprocessor line), whitespace collapsed; code has comments and strings
    blanked. llvm-cov puts a function's first line at its signature or at its
    brace, and this text is the same either way."""
    no = line_of(starts, brace)
    parts = [code[starts[no - 1]:brace]]
    for n in range(no - 1, max(0, no - 30), -1):
        ln = code[starts[n - 1]:starts[n] - 1]
        if re.search(r'[;{}]', ln) or ln.lstrip().startswith('#'):
            break
        parts.append(ln)
    return re.sub(r'\s+', ' ', ' '.join(reversed(parts))).strip()


def innermost(extents, pretty):
    """A function line -> (first line, name) of the innermost enclosing function;
    ties between functions with the same extent go to the smallest name."""
    spans = sorted(((b - a, a, pretty.get(n, n), n) for n, (a, b, _) in extents.items()))
    cache = {}

    def at(line):
        if line not in cache:
            cache[line] = None
            for _, a, _, n in spans:
                if a <= line <= extents[n][1]:
                    cache[line] = (a, n)
                    break
        return cache[line]
    return at


# ---------------------------------------------------------------- coverage

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
                warn(f'{repo} is a shallow clone with less than 90 days of history, so its '
                     'files get no commit counts; fetch more history to include them')
                return out
        log = subprocess.run(['git', '-C', repo, 'log', '--since=90.days', '--format=',
                              '--name-only', '--', *paths],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        warn(f'git log failed in {repo}, so its files get no commit counts')
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


def blocks(cov, func_at, orphans):
    """Yields each block as the list of its uncovered line numbers. Uncovered
    lines in no function are counted in orphans and left out."""
    run = []
    for line in sorted(cov.da):
        if cov.da[line] == 0 and func_at(line) is None:
            orphans[0] += 1
            continue
        if cov.da[line] == 0 and (not run or func_at(line) == func_at(run[0])):
            run.append(line)
            continue
        if run:
            yield run
        run = [line] if cov.da[line] == 0 else []
    if run:
        yield run


# ---------------------------------------------------------------- the TSV

class HeaderError(Exception):
    """The file is not a backlog TSV (wrong header)."""


def parse_tsv(path, strict):
    """(rows as dicts, problems). strict: every problem is reported; otherwise
    blank and short rows are skipped and unknown statuses read as open. Raises
    HeaderError when the header is not COLUMNS, whatever strict is."""
    rows, problems = [], []
    with open(path) as fh:
        header = fh.readline().rstrip('\r\n').split('\t')
        if header != COLUMNS:
            raise HeaderError(f'{path}:1: header is not {" ".join(COLUMNS)}')
        seen = {}
        for no, ln in enumerate(fh, 2):
            ln = ln.rstrip('\r\n')
            if not ln.strip():
                if strict:
                    problems.append(f'{path}:{no}: blank line')
                continue
            f = ln.split('\t')
            if len(f) != len(COLUMNS):
                problems.append(f'{path}:{no}: {len(f)} columns, not {len(COLUMNS)}')
                continue
            row = dict(zip(COLUMNS, f))
            if not STATUS.match(row['status']):
                problems.append(f'{path}:{no}: status "{row["status"]}" is not one of '
                                f'{", ".join(STATUSES)}')
                if not strict:
                    row['status'] = 'open'
            if row['key'] in seen:
                problems.append(f'{path}:{no}: key {row["key"]} repeats line {seen[row["key"]]}')
            seen[row['key']] = no
            rows.append(row)
    return rows, problems


def read_status(path):
    if not os.path.exists(path):
        return {}
    try:
        rows, problems = parse_tsv(path, strict=False)
    except HeaderError as e:
        die(f'{e}; fix the file rather than lose every status')
    for p in problems:
        warn(p)
    return {r['key']: r['status'] for r in rows}


def check(path):
    try:
        rows, problems = parse_tsv(path, strict=True)
    except HeaderError as e:
        rows, problems = [], [str(e)]
    for p in problems:
        print(f'backlog.py: {p}', file=sys.stderr)
    if problems:
        sys.exit(1)
    print(f'backlog.py: {path}: {len(rows)} rows, all valid', file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('lcov', nargs='?')
    ap.add_argument('--src', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    ap.add_argument('--out', default=None, help='default: <src>/coverage/backlog.tsv')
    ap.add_argument('--top', type=int, default=0)
    ap.add_argument('--check', metavar='TSV', help='validate a backlog file and exit')
    a = ap.parse_args()
    if a.check:
        check(a.check)
        return
    if not a.lcov:
        ap.error('give an lcov file, or --check <tsv>')
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

    rows, skipped, missing, orphans = [], collections.Counter(), [], [0]
    for name in sorted(core):
        path = os.path.join(src, name)
        try:
            with open(path, errors='replace') as fh:
                source = fh.read()
        except OSError:
            missing.append(name)
            continue
        text = source.split('\n')
        cov = core[name]
        extents = function_extents(source, cov.fn)
        func_at = innermost(extents, pretty)
        calls, filler = error_path_lines(source)
        t = tier_of(name)
        # Blocks with the same text in a function of the same signature are
        # numbered in order.
        occurrences = collections.Counter()
        for run in blocks(cov, func_at, orphans):
            _, func = func_at(run[0])
            label = re.sub(r'\s+', ' ', pretty.get(func, func))
            bt = 'C' if t == 'A' and DEMOTE.search(label) else t
            if error_path_block(run, calls, filler):
                skipped[bt] += 1
                continue
            body = '\n'.join(text[l - 1].strip() for l in run if 0 < l <= len(text))
            sig = extents[func][2]
            occurrences[(sig, body)] += 1
            k = hashlib.sha1(f'{name}\0{sig}\0{body}\0{occurrences[(sig, body)]}'
                             .encode()).hexdigest()[:12]
            rows.append({
                'priority': round(WEIGHT[bt] * min(len(run), CAP)
                                  * (1 + math.log2(1 + commits[name]))),
                'tier': bt,
                'status': old.get(k, 'open'),
                'file': name,
                'lines': f'{run[0]}-{run[-1]}',
                'uncovered': len(run),
                'churn90': commits[name],
                'function': label[:120],
                'key': k,
            })

    dup = [k for k, n in collections.Counter(r['key'] for r in rows).items() if n > 1]
    if dup:
        die(f'{len(dup)} keys repeat (first: {dup[0]}); this is a bug in backlog.py')
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
              f'partial {c["partial"]}, unreachable {c["unreachable"]}, '
              f'hpc-only {c["hpc-only"]}, skip {c["skip"]}; '
              f'{skipped[t]} error-path blocks left out', file=sys.stderr)
    if orphans[0]:
        print(f'  {orphans[0]} uncovered lines outside every function (macro bodies) left out',
              file=sys.stderr)
    if missing:
        warn(f'{len(missing)} files in the lcov are not in --src (different commit?): '
             f'{", ".join(missing[:5])}')
    if a.top:
        print('\t'.join(COLUMNS[:-1]))
        for r in [r for r in rows if r['status'] == 'open'][:a.top]:
            print('\t'.join(str(r[c]) for c in COLUMNS[:-1]))


if __name__ == '__main__':
    main()
