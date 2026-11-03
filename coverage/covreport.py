#!/usr/bin/env python3
"""Writes <out>/REPORT.md from the exports coverage/report.sh produced.

Paths are reported relative to the repository root. The build copies many
headers into <build>/include/ (and the repository's include/ symlink points
there), so one header can reach the profile under two or three paths; each
copy is attributed back to the byte-identical file under src/ or
contrib/reconverse/, and each generated .decl.h/.def.h to the directory of
the .ci file that declares its module (marked "[gen]"). Lines and functions
are unions over those paths (from the lcov export); regions are the maximum
over them (from the summary export), an approximation for the few files
reached through more than one path.
"""

import argparse
import collections
import filecmp
import glob
import json
import os
import re

SRC_EXTS = ('.C', '.c', '.cpp', '.cc', '.cxx')


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--src', required=True, help='repository root')
    ap.add_argument('--build', required=True, help='build directory')
    ap.add_argument('--out', required=True, help='coverage/run.sh output directory')
    ap.add_argument('--labels', required=True, help='cumulative tier labels, in order')
    return ap.parse_args()


class Paths:
    """Maps a path from the profile to a repository-relative name."""

    def __init__(self, src, build):
        self.src = os.path.realpath(src)
        self.build = os.path.realpath(build)
        self.buildrel = os.path.relpath(self.build, self.src)
        self.incmap = self._map_includes()
        self.cache = {}

    def _map_includes(self):
        byname = collections.defaultdict(list)
        for top in ('src', 'contrib/reconverse/src', 'contrib/reconverse/include'):
            for root, _, files in os.walk(os.path.join(self.src, top)):
                for f in files:
                    byname[f].append(os.path.join(root, f))
        modules = {}
        for ci in glob.glob(os.path.join(self.src, 'src', '**', '*.ci'), recursive=True):
            with open(ci, errors='ignore') as fh:
                for m in re.findall(r'^\s*(?:main)?module\s+(\w+)', fh.read(), re.M):
                    modules.setdefault(m, ci)
        inc = os.path.join(self.build, 'include')
        mapping = {}
        for root, _, files in os.walk(inc):
            for f in files:
                p = os.path.join(root, f)
                if os.path.islink(p):
                    continue
                key = os.path.relpath(p, self.build)
                same = [s for s in byname.get(f, []) if filecmp.cmp(s, p, shallow=False)]
                if same:
                    mapping[key] = os.path.relpath(same[0], self.src)
                    continue
                g = re.match(r'(\w+)\.(decl|def)\.h$', f)
                if g and g.group(1) in modules:
                    d = os.path.relpath(os.path.dirname(modules[g.group(1)]), self.src)
                    mapping[key] = f'{d}/{f} [gen]'
        return mapping

    def name(self, path):
        if path in self.cache:
            return self.cache[path]
        p = os.path.realpath(path) if os.path.exists(path) else os.path.normpath(path)
        if p.startswith(self.build + '/'):
            rel = os.path.relpath(p, self.build)
            n = self.incmap.get(rel, 'BUILD/' + rel)
        elif p.startswith(self.src + '/'):
            n = os.path.relpath(p, self.src)
        else:
            n = p
        self.cache[path] = n
        return n


def is_generated(n):
    return n.endswith(' [gen]') or n.endswith('.decl.h') or n.endswith('.def.h')


def part(n):
    if n.startswith('contrib/reconverse/'):
        return 'reconverse'
    if n.startswith('src/') or n.startswith('BUILD/'):
        return 'charm'
    return 'other'


def fname(name):
    """One name per source function: a function with internal linkage (a
    static function in a header) is named '<translation unit>:<name>', once
    per translation unit that includes it."""
    return re.sub(r'^[^:]*\.(C|c|cpp|cc|cxx|h|hpp):', '', name)


def load(rep, label, paths):
    """Per file: (lines hit, lines, functions hit, functions, regions hit, regions)."""
    regions = {}
    with open(os.path.join(rep, f'{label}-summary.json')) as fh:
        data = json.load(fh)['data'][0]
    for f in data['files']:
        n = paths.name(f['filename'])
        r = f['summary']['regions']
        old = regions.get(n, (0, 0))
        regions[n] = (max(old[0], r['covered']), max(old[1], r['count']))
    lines = collections.defaultdict(dict)
    fns = collections.defaultdict(dict)
    cur = None
    with open(os.path.join(rep, f'{label}.lcov')) as fh:
        for ln in fh:
            if ln.startswith('SF:'):
                cur = paths.name(ln[3:].strip())
                lines[cur]
            elif ln.startswith('DA:'):
                no, cnt = ln[3:].split(',')[:2]
                no = int(no)
                lines[cur][no] = max(lines[cur].get(no, 0), int(cnt))
            elif ln.startswith('FNDA:'):
                cnt, fn = ln[5:].strip().split(',', 1)
                fn = fname(fn)
                fns[cur][fn] = max(fns[cur].get(fn, 0), int(cnt))
            elif ln.startswith('FN:'):
                fns[cur].setdefault(fname(ln[3:].strip().split(',', 1)[1]), 0)
    files = {}
    for n, ls in lines.items():
        fs = fns.get(n, {})
        r = regions.get(n, (0, 0))
        files[n] = (sum(1 for v in ls.values() if v > 0), len(ls),
                    sum(1 for v in fs.values() if v > 0), len(fs), r[0], r[1])
    return files


def total(files, pred=lambda n: True):
    s = [0] * 6
    for n, v in files.items():
        if pred(n):
            for i in range(6):
                s[i] += v[i]
    return s


def pct(a, b):
    return f'{100.0 * a / b:.1f}%' if b else '-'


def group(n):
    n = n.replace(' [gen]', '')
    p = n.split('/')
    if p[0] == 'BUILD':
        return 'BUILD (generated in the build directory)'
    if n.startswith('contrib/reconverse/'):
        return '/'.join(p[:3])
    if p[0] != 'src':
        return p[0]
    if len(p) > 3 and p[1] == 'libs' and p[2] == 'ck-libs':
        return '/'.join(p[:4])
    if p[1] == 'arch' and len(p) > 2:
        return '/'.join(p[:3])
    return '/'.join(p[:2])


def read_tsv(path):
    if not os.path.exists(path):
        return []
    with open(path) as fh:
        return [ln.rstrip('\n').split('\t') for ln in fh if ln.strip()]


def compiled_sources(paths):
    """Sources the CMake build compiled into Charm++ and reconverse (not LCI)."""
    found = set()
    pat = re.compile(r'"([^"]+)"')
    for dep in glob.glob(os.path.join(paths.build, '**', 'DependInfo.cmake'), recursive=True):
        with open(dep, errors='ignore') as fh:
            for p in pat.findall(fh.read()):
                if not p.endswith(SRC_EXTS) or not os.path.isabs(p) or not os.path.exists(p):
                    continue
                n = paths.name(p)
                if part(n) == 'other' or n.startswith(('src/xlat-i/', 'BUILD/_deps/')):
                    continue
                found.add(n)
    return sorted(found)


def main():
    a = parse_args()
    out = os.path.realpath(a.out)
    rep = os.path.join(out, 'report')
    labels = a.labels.split()
    paths = Paths(a.src, a.build)
    T = {lb: load(rep, lb, paths) for lb in labels}
    first, last = labels[0], labels[-1]
    o = []
    w = o.append

    w('# Charm++ and reconverse coverage\n')
    meta = os.path.join(out, 'meta.txt')
    if os.path.exists(meta):
        w('```')
        w(open(meta).read().rstrip())
        w('```\n')
    tiers = read_tsv(os.path.join(out, 'tiers.tsv'))
    if tiers:
        w('Tiers run: ' + '; '.join(f'{t[0]} ({t[1]} directories, {t[2]} s)' for t in tiers) + '.')
    with open(os.path.join(rep, 'objects.txt')) as fh:
        nobj = sum(1 for _ in fh)
    w(f'Measured over {nobj} binaries (every test and example program built, plus '
      'libreconverse). Charm++ libraries are static: a source no program links does '
      'not count at all (see "Compiled sources absent from every binary").\n')

    scopes = [
        ('Charm++, all files (src/ and generated .decl.h/.def.h)', lambda n: part(n) == 'charm'),
        ('Charm++, hand-written files only (no .decl.h/.def.h)',
         lambda n: part(n) == 'charm' and not is_generated(n)),
        ('Reconverse (contrib/reconverse)', lambda n: part(n) == 'reconverse'),
        ('Charm++ and reconverse together, hand-written only',
         lambda n: part(n) != 'other' and not is_generated(n)),
    ]
    w('## Headline\n')
    for title, pred in scopes:
        w(f'**{title}**\n')
        w('| tier | lines covered / total | lines % | functions covered / total | functions % | regions covered / total | regions % |')
        w('|---|---|---|---|---|---|---|')
        for lb in labels:
            s = total(T[lb], pred)
            w(f'| {lb} | {s[0]} / {s[1]} | {pct(s[0], s[1])} | {s[2]} / {s[3]} | {pct(s[2], s[3])} | {s[4]} / {s[5]} | {pct(s[4], s[5])} |')
        w('')
    other = sorted(n for n in T[last] if part(n) == 'other')
    if other:
        w(f'{len(other)} measured files are outside src/ and contrib/reconverse/ and are counted in no table: '
          + ', '.join(other[:20]) + (' ...' if len(other) > 20 else '') + '\n')

    ck = os.path.join(rep, 'ck-convcore.txt')
    if os.path.exists(ck):
        w(f'`ck-convcore-summary` over the {last} lcov export (src/ck-core and contrib/reconverse/src, '
          'source paths only):\n')
        w('```')
        w(open(ck).read().rstrip())
        w('```\n')

    w(f'## Per directory ({last}; {first} for reference)\n')
    w(f'| directory | files | {last} lines covered / total | {last} % | {first} lines covered / total | {first} % |')
    w('|---|---|---|---|---|---|')
    groups = collections.defaultdict(list)
    for n in T[last]:
        if part(n) != 'other':
            groups[group(n)].append(n)
    for g in sorted(groups, key=lambda g: (g.startswith('contrib'), g)):
        u = total(T[last], lambda n: group(n) == g)
        c = total(T[first], lambda n: group(n) == g)
        w(f'| {g} | {len(groups[g])} | {u[0]} / {u[1]} | {pct(u[0], u[1])} | {c[0]} / {c[1]} | {pct(c[0], c[1])} |')
    w('')

    for d in ('src/ck-core', 'contrib/reconverse/src'):
        w(f'### Per file: {d}\n')
        w(f'| file | {first} covered / total | {first} % | {last} covered / total | {last} % |')
        w('|---|---|---|---|---|')
        for n in sorted(x for x in T[last] if group(x) == d):
            u = T[last][n]
            c = T[first].get(n, (0, u[1]))
            w(f'| {n[len(d) + 1:]} | {c[0]} / {c[1]} | {pct(c[0], c[1])} | {u[0]} / {u[1]} | {pct(u[0], u[1])} |')
        w('')

    w(f'### Lowest 30 files with more than 100 lines ({last})\n')
    w('| file | lines covered / total | % |')
    w('|---|---|---|')
    big = [(v[0] / v[1], n, v) for n, v in T[last].items() if v[1] > 100 and part(n) != 'other']
    for _, n, v in sorted(big)[:30]:
        w(f'| {n} | {v[0]} / {v[1]} | {pct(v[0], v[1])} |')
    w('')

    comp = compiled_sources(paths)
    seen = {n.replace(' [gen]', '') for n in T[last]}
    absent = [c for c in comp if c not in seen]
    w('## Compiled sources absent from every binary\n')
    w(f'{len(absent)} of {len(comp)} source files the build compiles (from the CMake '
      'DependInfo.cmake lists; LCI and src/xlat-i, the host tool charmxi, left out) appear in no '
      'measured binary, either because no program links them or because they hold no '
      'instrumentable code in this configuration. They are in no denominator above.\n')
    w('```')
    w('\n'.join(absent) if absent else '(none)')
    w('```\n')

    tier_names = [t for t in ('ci', 'all', 'ex') if os.path.exists(os.path.join(out, f'results-{t}.tsv'))]
    w('## Tests that ran nothing\n')
    w('A passing `make test` run that wrote no profile: the target started no program, or '
      'testrun skipped every run for this process count. These pass in CI without testing anything.\n')
    vac = [(t, r) for t in tier_names for r in read_tsv(os.path.join(out, f'vacuous-{t}.tsv'))]
    if vac:
        w('| tier | directory | run | reason |')
        w('|---|---|---|---|')
        for t, r in vac:
            w(f'| {t} | {r[0]} | {r[1]} | {r[2] if len(r) > 2 else ""} |')
    else:
        w('(none)')
    w('')

    w('## Test outcomes\n')
    w('Outcome (wall seconds). A run that fails or times out writes no profile, so its '
      'partial execution is not counted.\n')
    for t in tier_names:
        rows = read_tsv(os.path.join(out, f'results-{t}.tsv'))
        bydir = collections.OrderedDict()
        steps = []
        for r in rows:
            r += [''] * (5 - len(r))
            bydir.setdefault(r[0], {})[r[1]] = r
            if r[1] not in steps:
                steps.append(r[1])
        counts = collections.Counter(f'{r[1]} {re.sub(r"[(].*", "", r[2])}' for r in rows)
        w(f'### Tier {t} ({len(bydir)} directories)\n')
        w('| directory | ' + ' | '.join(steps) + ' | note |')
        w('|---|' + '---|' * len(steps) + '---|')
        for d, st in bydir.items():
            cells = [f'{st[s][2]} ({st[s][3]})' if s in st else '-' for s in steps]
            notes = '; '.join(f'{s}: {st[s][4]}' for s in steps if s in st and st[s][4])
            w(f'| {d} | ' + ' | '.join(cells) + f' | {notes} |')
        w('\nCounts: ' + ', '.join(f'{k}: {v}' for k, v in sorted(counts.items())) + '\n')
        unreached = os.path.join(out, f'unreached-{t}.txt')
        if os.path.exists(unreached) and os.path.getsize(unreached):
            w(f'Directories with a test target that no aggregator Makefile reaches (not run in tier {t}): '
              + ', '.join(open(unreached).read().split()) + '\n')

    excl = os.path.join(paths.src, 'coverage', 'exclude.txt')
    if os.path.exists(excl):
        rows = [ln.strip() for ln in open(excl) if ln.strip() and not ln.startswith('#')]
        w('## Excluded directories (coverage/exclude.txt)\n')
        w('```')
        w('\n'.join(rows) if rows else '(none)')
        w('```\n')

    w('## Notes\n')
    for lb in labels:
        err = os.path.join(rep, f'{lb}-report.err')
        if os.path.exists(err):
            m = re.search(r'(\d+) functions have mismatched data', open(err).read())
            if m:
                w(f'- {lb}: llvm-cov warned that {m.group(1)} functions have mismatched data. '
                  'Expected: every test defines its own `main` and handlers under common names, '
                  'and header inlines exist in several binaries. To see which: '
                  '`llvm-profdata show --all-functions <profdata> | grep \':$\' | sort | uniq -d`.')
    w('- Denominators move between configurations (header instantiations, build options, '
      'which binaries were built), so compare percentages loosely and covered-line counts of '
      'one file where it matters.')
    w('- Files: report/<label>.profdata, report/<label>-report.txt (llvm-cov report, unmapped '
      'paths), report/<label>-summary.json, report/<label>.lcov, report/objects.txt, '
      'results-<tier>.tsv, vacuous-<tier>.tsv, logs/<tier>/.')

    with open(os.path.join(out, 'REPORT.md'), 'w') as fh:
        fh.write('\n'.join(o) + '\n')


if __name__ == '__main__':
    main()
