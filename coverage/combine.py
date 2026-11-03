#!/usr/bin/env python3
"""Combines lcov exports from several builds or machines into one report.

usage: coverage/combine.py [--lcov <file>] [--src <repo>] label=path.lcov label=path.lcov ...

Each input is the <out>/report/<tier>.lcov that coverage/report.sh wrote for
one build (another architecture, build option or machine). Paths are
normalized to repository-relative names, so the same source file from two
checkouts in different places is one file: a path is cut before its first
component named src, contrib, tests, examples or benchmarks. A header that
reached the profile through the build's include/ directory is attributed to
the file of the same name under src/ or contrib/reconverse/ when that name is
unique, and a generated .decl.h/.def.h to the directory of the .ci file that
declares its module (marked "[gen]"); --src (default: this script's
repository) is used to look those up. Anything else is counted under
"other". Tests, examples and benchmarks are dropped, as report.sh drops them.

The inputs are combined as `lcov -a` combines them: per file, line hit counts
are summed, functions are summed by name and branches by (line, block,
branch). A line instrumented in one input and not in another counts as
instrumented in the combination. The line numbers must refer to the same
sources, so the inputs should come from the same commit.

Writes a Markdown report to stdout; --lcov also writes the combined lcov
(source paths repository-relative).
"""

import argparse
import collections
import glob
import os
import re
import sys

TOPS = ('src', 'contrib', 'tests', 'examples', 'benchmarks')
EXCLUDED = re.compile(r'^(tests|examples|benchmarks)/|^contrib/reconverse/(tests|examples|benchmarks)/')
AREAS = ('src/ck-core', 'src/ck-ldb', 'src/libs', 'src/arch', 'src/conv-core', 'src/util',
         'contrib/reconverse/src', 'contrib/reconverse/include')


def die(msg):
    print(f'combine.py: {msg}', file=sys.stderr)
    sys.exit(2)


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--lcov', help='write the combined lcov here')
    ap.add_argument('--src', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'),
                    help='repository root, for attributing build include/ copies (default: this repository)')
    ap.add_argument('inputs', nargs='+', metavar='label=path.lcov')
    a = ap.parse_args()
    if len(a.inputs) < 2:
        ap.error('give at least two label=path.lcov inputs')
    pairs = []
    for s in a.inputs:
        label, sep, path = s.partition('=')
        if not sep or not label or not path:
            ap.error(f"'{s}' is not label=path.lcov")
        if label in (p[0] for p in pairs):
            ap.error(f"label '{label}' given twice")
        pairs.append((label, path))
    return a, pairs


class Names:
    """Maps an SF path to a repository-relative name."""

    def __init__(self, src):
        self.src = os.path.realpath(src)
        self.byname = collections.defaultdict(list)
        for top in ('src', 'contrib/reconverse/src', 'contrib/reconverse/include'):
            for root, _, files in os.walk(os.path.join(self.src, top)):
                for f in files:
                    self.byname[f].append(os.path.relpath(os.path.join(root, f), self.src))
        self.modules = {}
        for ci in glob.glob(os.path.join(self.src, 'src', '**', '*.ci'), recursive=True):
            with open(ci, errors='ignore') as fh:
                for m in re.findall(r'^\s*(?:main)?module\s+(\w+)', fh.read(), re.M):
                    self.modules.setdefault(m, os.path.relpath(os.path.dirname(ci), self.src))
        self.cache = {}

    def name(self, path):
        if path in self.cache:
            return self.cache[path]
        p = os.path.normpath(path).split('/')
        n = None
        for i, c in enumerate(p):
            if c in TOPS:
                n = '/'.join(p[i:])
                break
            if c == 'include' and i + 1 < len(p):
                n = self._include('/'.join(p[i + 1:]))
                break
        self.cache[path] = n or 'other:' + os.path.normpath(path)
        return self.cache[path]

    def _include(self, rel):
        f = os.path.basename(rel)
        g = re.match(r'(\w+)\.(decl|def)\.h$', f)
        if g and g.group(1) in self.modules:
            return f'{self.modules[g.group(1)]}/{f} [gen]'
        same = self.byname.get(f, [])
        if len(same) == 1:
            return same[0]
        return 'other:include/' + rel


def fname(name):
    """Drops the '<translation unit>:' prefix llvm-cov gives a function with
    internal linkage, as covreport.py does."""
    return re.sub(r'^[^:]*\.(C|c|cpp|cc|cxx|h|hpp):', '', name)


class Cov:
    def __init__(self):
        self.da = collections.defaultdict(int)
        self.fn = {}                              # name -> first line
        self.fnda = collections.defaultdict(int)
        self.br = {}                              # (line, block, branch) -> count or None


def load(path, names):
    try:
        fh = open(path, errors='replace')
    except OSError as e:
        die(f'cannot read {path}: {e.strerror}')
    files = collections.defaultdict(Cov)
    cur = None
    records = 0
    with fh:
        for no, ln in enumerate(fh, 1):
            ln = ln.strip()
            if not ln:
                continue
            tag, _, rest = ln.partition(':')
            try:
                if tag == 'SF':
                    n = names.name(rest)
                    cur = None if EXCLUDED.match(n) else files[n]
                    records += 1
                    continue
                if tag == 'end_of_record':
                    cur = None
                    continue
                if no == 1 and tag != 'TN':
                    raise ValueError
                if cur is None:
                    if tag in ('DA', 'FN', 'FNDA', 'BRDA') and records == 0:
                        raise ValueError
                    continue
                if tag == 'DA':
                    line, cnt = rest.split(',')[:2]
                    cur.da[int(line)] += int(cnt)
                elif tag == 'FN':
                    f = rest.split(',')
                    cur.fn.setdefault(fname(f[-1]), int(f[0]))
                elif tag == 'FNDA':
                    cnt, f = rest.split(',', 1)
                    cur.fnda[fname(f)] += int(cnt)
                elif tag == 'BRDA':
                    line, block, branch, taken = rest.split(',')
                    k = (int(line), block, branch)
                    t = None if taken == '-' else int(taken)
                    old = cur.br.get(k)
                    cur.br[k] = t if old is None else old + (t or 0)
            except ValueError:
                die(f'{path}:{no}: not an lcov file (line "{ln[:60]}")')
    if records == 0:
        die(f'{path}: no SF: records; not an lcov file')
    for c in files.values():
        for f in c.fnda:
            c.fn.setdefault(f, 0)
    return files


def combine(inputs):
    out = collections.defaultdict(Cov)
    for files in inputs:
        for n, c in files.items():
            o = out[n]
            for k, v in c.da.items():
                o.da[k] += v
            for k, v in c.fn.items():
                o.fn.setdefault(k, v)
            for k, v in c.fnda.items():
                o.fnda[k] += v
            for k, v in c.br.items():
                old = o.br.get(k)
                o.br[k] = v if old is None else old + (v or 0)
    return out


def area(n):
    if n.startswith('other:'):
        return 'other'
    if n.endswith(' [gen]'):
        return 'generated .decl.h/.def.h'
    for a in AREAS:
        if n.startswith(a + '/'):
            return a
    if n.startswith('src/'):
        return 'other src/'
    return 'other'


AREA_ORDER = AREAS[:6] + ('other src/', 'generated .decl.h/.def.h') + AREAS[6:] + ('other',)


def part(n):
    a = area(n)
    if a.startswith('contrib/'):
        return 'reconverse'
    if a == 'other':
        return 'other'
    return 'charm'


def summary(files, pred=lambda n: True):
    """lines covered, lines, functions covered, functions."""
    s = [0, 0, 0, 0]
    for n, c in files.items():
        if pred(n):
            s[0] += sum(1 for v in c.da.values() if v > 0)
            s[1] += len(c.da)
            s[2] += sum(1 for v in c.fnda.values() if v > 0)
            s[3] += len(c.fn)
    return s


def pct(a, b):
    return f'{100.0 * a / b:.1f}%' if b else '-'


def covered(files):
    return {(n, k) for n, c in files.items() for k, v in c.da.items() if v > 0}


def write_lcov(path, files):
    with open(path, 'w') as fh:
        for n in sorted(files):
            c = files[n]
            fh.write('SF:' + n.replace(' [gen]', '').replace('other:', '', 1) + '\n')
            for f, line in sorted(c.fn.items(), key=lambda x: (x[1], x[0])):
                fh.write(f'FN:{line},{f}\n')
            for f in sorted(c.fn, key=lambda x: (c.fn[x], x)):
                fh.write(f'FNDA:{c.fnda.get(f, 0)},{f}\n')
            fh.write(f'FNF:{len(c.fn)}\nFNH:{sum(1 for v in c.fnda.values() if v > 0)}\n')
            for (line, block, branch), t in sorted(c.br.items(), key=lambda x: (x[0][0], x[0][1], x[0][2])):
                fh.write(f'BRDA:{line},{block},{branch},{"-" if t is None else t}\n')
            fh.write(f'BRF:{len(c.br)}\nBRH:{sum(1 for t in c.br.values() if t)}\n')
            for line in sorted(c.da):
                fh.write(f'DA:{line},{c.da[line]}\n')
            fh.write(f'LF:{len(c.da)}\nLH:{sum(1 for v in c.da.values() if v > 0)}\n')
            fh.write('end_of_record\n')


def main():
    a, pairs = parse_args()
    names = Names(a.src)
    labels = [lb for lb, _ in pairs]
    T = {lb: load(p, names) for lb, p in pairs}
    U = combine(T[lb] for lb in labels)
    rows = labels + ['combined']
    T['combined'] = U
    o = []
    w = o.append

    w('# Combined coverage\n')
    w('Inputs: ' + '; '.join(f'{lb} = `{p}`' for lb, p in pairs) + '.')
    w('Lines and functions are counted over Charm++ (src/, with generated .decl.h/.def.h) and '
      'reconverse (contrib/reconverse); tests, examples and benchmarks are left out. '
      'A line instrumented in any input is instrumented in the combination.\n')

    w('## Totals\n')
    for title, pred in (('Charm++ and reconverse', lambda n: part(n) != 'other'),
                        ('Charm++ (src/ and generated headers)', lambda n: part(n) == 'charm'),
                        ('Reconverse (contrib/reconverse)', lambda n: part(n) == 'reconverse')):
        w(f'**{title}**\n')
        w('| input | files | lines covered / instrumented | lines % | functions covered / total | functions % |')
        w('|---|---|---|---|---|---|')
        for lb in rows:
            s = summary(T[lb], pred)
            nf = sum(1 for n in T[lb] if pred(n))
            w(f'| {lb} | {nf} | {s[0]} / {s[1]} | {pct(s[0], s[1])} | {s[2]} / {s[3]} | {pct(s[2], s[3])} |')
        w('')

    w('## Per area (lines covered / instrumented)\n')
    w('| area | ' + ' | '.join(rows) + ' |')
    w('|---|' + '---|' * len(rows))
    for ar in AREA_ORDER:
        cells = []
        for lb in rows:
            s = summary(T[lb], lambda n: area(n) == ar)
            cells.append(f'{s[0]} / {s[1]} ({pct(s[0], s[1])})')
        if any(summary(T[lb], lambda n: area(n) == ar)[1] for lb in rows):
            w(f'| {ar} | ' + ' | '.join(cells) + ' |')
    w('')
    other = sorted(n[6:] for n in U if area(n) == 'other')
    if other:
        w(f'"other" holds {len(other)} files that are under neither src/ nor contrib/reconverse/ '
          '(or a build include/ copy whose name is not unique in the tree): '
          + ', '.join(f'`{n}`' for n in other[:10]) + (' ...' if len(other) > 10 else '') + '\n')

    w('## Lines covered only by one input\n')
    w('Lines that the input covers and no other input covers: what that build or machine adds.\n')
    cov = {lb: covered(T[lb]) for lb in labels}
    only = {}
    for lb in labels:
        rest = set().union(*(cov[x] for x in labels if x != lb))
        only[lb] = cov[lb] - rest
    w('| area | ' + ' | '.join(f'only {lb}' for lb in labels) + ' |')
    w('|---|' + '---|' * len(labels))
    for ar in AREA_ORDER:
        cells = [sum(1 for n, _ in only[lb] if area(n) == ar) for lb in labels]
        if any(cells):
            w(f'| {ar} | ' + ' | '.join(str(c) for c in cells) + ' |')
    w('| **all** | ' + ' | '.join(str(sum(1 for n, _ in only[lb] if area(n) != 'other'))
                                 for lb in labels) + ' |')
    w('')
    for lb in labels:
        per = collections.Counter(n for n, _ in only[lb] if area(n) != 'other')
        if per:
            w(f'Files with the most lines only {lb} covers: '
              + ', '.join(f'`{n}` {k}' for n, k in per.most_common(10)) + '.\n')

    w('## Files instrumented in one input and absent from another\n')
    w('A file absent from an input was not compiled in that configuration (platform or '
      'ifdef\'d code) or no program of that build linked it.\n')
    measured = {lb: {n for n in T[lb] if area(n) != 'other'} for lb in labels}
    common = set.intersection(*measured.values())
    w(f'{len(common)} files are in every input; {len(set.union(*measured.values()))} in the combination.\n')
    w('| in | not in | files | lines instrumented | first ten |')
    w('|---|---|---|---|---|')
    for x in labels:
        for y in labels:
            if x == y:
                continue
            d = sorted(measured[x] - measured[y])
            nl = sum(len(T[x][n].da) for n in d)
            w(f'| {x} | {y} | {len(d)} | {nl} | ' + ', '.join(f'`{n}`' for n in d[:10]) + ' |')
    w('')

    if a.lcov:
        write_lcov(a.lcov, U)
        w(f'Combined lcov: `{a.lcov}`.')
    print('\n'.join(o))


if __name__ == '__main__':
    main()
