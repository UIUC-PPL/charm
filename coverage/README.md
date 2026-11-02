# Coverage of Charm++ and reconverse

Line, function and region coverage from clang source-based coverage
(`-fprofile-instr-generate -fcoverage-mapping`), measured over Charm++'s own
tests and examples on the reconverse runtime. It covers both Charm++
(`src/`) and the runtime under it (`contrib/reconverse`).

## Recipe

```bash
# 1. An instrumented build (needs clang; Apple clang on macOS).
./build charm++ reconverse-darwin-arm8 -j6 -g --coverage
#    (Linux: ./build charm++ reconverse-linux-x86_64 clang -j4 -g --coverage;
#     the 'clang' option is what makes charmc link with clang as well)

# 2. Build and run the tests, collecting profiles (about 20-25 min on a laptop).
coverage/run.sh reconverse-darwin-arm8 --tier all-tiers --shapes "1 2"

# 3. Merge, measure and write coverage/out/REPORT.md.
coverage/report.sh reconverse-darwin-arm8 coverage/out
```

`--coverage` adds the flags to `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS` and the
linker flags before reconverse is configured, so libreconverse is
instrumented too, and records `CMK_COVERAGE=1` in `conv-mach-opt.sh`. charmc
reads that and instruments every program it compiles and links, so a plain
`make -C tests/charm++/<dir>` against the build is instrumented with no
`OPTS`, including `make CHARMC=<build>/bin/charmc` as CI runs it.

`run.sh` options: `--tier ci|all|ex|all-tiers`, `--shapes "1 2"` (process
counts; 2 runs through lcrun with `TESTRUN_PROCS=2`), `--out <dir>` (default
`coverage/out`), `--list` (print the tiers without running anything).
The total PE count of each run is the `+p` in the test's Makefile; `testrun`
divides it over the processes of the shape, uses fewer processes when there
are more processes than PEs, and skips a run whose PE count the process count
does not divide. The ci tier's Makefiles use `+p1` to `+p6` (most runs
`+p4`), so shape 2 runs 2 processes of 2 or 3 PEs and shape 4 runs 4
processes of 1 PE (and `+p3` as 3 processes; `+p6` is skipped). Shapes 1
and 2 have been run over all tiers, shape 4 over the ci tier (see "Last
verification run").
Timeouts: `COV_BUILD_TIMEOUT` (900 s) and `COV_RUN_TIMEOUT` (300 s) per
directory; `timeout` is used where it exists, `coverage/tmo.pl` otherwise
(macOS), and either kills the whole process group; a step killed for
running past its limit is recorded as `TIMEOUT` on both. Before a
multi-process run `run.sh` waits for any other lcrun job on the machine to
finish (two lcrun jobs at once stall each other on a laptop) and clears
`~/.tmp/lct_pmi_file-*`; if one wait runs out (`COV_LCRUN_WAIT`, 1800 s), it
stops waiting for the rest of the run.

`report.sh` writes `REPORT.md`, plus per cumulative tier the merged profile,
the `llvm-cov report` text, `export -summary-only` JSON and an lcov export
under `<out>/report/`; `COV_HTML=1` adds `llvm-cov show -format=html` of the
last tier. It picks `llvm-profdata`/`llvm-cov` from the compiler's
directory, `xcrun` or `PATH`; set `LLVM_PROFDATA`/`LLVM_COV` to override.
It measures the programs `run.sh` built (`programs-<tier>.txt`) plus
libreconverse, and merges a tier only if it was run against the build being
measured: `run.sh` records the build's libraries in `stamp-<tier>.txt`
(`coverage/buildstamp.sh`), and `report.sh` stops if a tier's stamp no
longer matches, so rerun that tier after a rebuild or remove its
`profraw/<tier>`.
`.github/workflows/coverage.yaml` runs all of this weekly on ubuntu-latest
and on demand, and uploads the output.

## Published reports

The weekly and manual workflow runs publish to
<https://charmplusplus.org/charm-coverage/> (repository
`charmplusplus/charm-coverage`, served by GitHub Pages from its `main`
branch): a front page with the headline numbers of every run, and under
`runs/<date>-<charm sha>/` the report (Markdown and HTML), the result
tables, the summary JSON and gzipped lcov exports of each cumulative tier,
and the llvm-cov per-file HTML view. `coverage/publish.py` writes a run
into a checkout of that repository and regenerates the front page; the
workflow then commits and pushes with the deploy key stored in the
`COVERAGE_DEPLOY_KEY` secret of charmplusplus/charm. Pull-request runs
upload the artifact only.

The per-file HTML view (about 50 MB, 5 MB in git) is kept for the newest
eight runs; the rest of a run (about 3 MB) is kept for good. The
repository therefore grows by roughly 8 MB a week, and its history can be
squashed to one commit when it gets large, since every published file is in
the working tree. To publish a run made elsewhere (another machine, a
combined report), run `publish.py --site <clone> --out <dir>` by hand and
push.

## Combining builds and machines

No one build exercises all the code: platform files, `#ifdef` branches and
build options differ between architectures and configurations. To measure
their union:

1. On each machine and for each build, run steps 1-3 of the recipe with its
   own `--out`, e.g. `--out coverage/out/linux-x86_64` (the same `<out>` goes
   to `report.sh`).
2. Keep `<out>/report/ci+all+ex.lcov` (or the lcov of the tier you ran) and
   `<out>/REPORT.md` from each, and copy them to one place.
3. Combine them, giving each a label:

```bash
python3 coverage/combine.py --lcov combined.lcov \
  linux=linux/ci+all+ex.lcov mac=mac/ci+all+ex.lcov > COMBINED.md
```

`combine.py` takes paths relative to the repository, cut before the first
`src`, `contrib`, `tests`, `examples` or `benchmarks` component, so checkouts
in different places match. Headers seen through a build's `include/` are
attributed to their `src/` or `contrib/reconverse/` file of the same name,
and generated `.decl.h`/`.def.h` to the directory of their `.ci` file, as
`REPORT.md` does. It sums hit counts as `lcov -a` does; a line instrumented in
any input is instrumented in the union.

`COMBINED.md` shows, per input and for the union, lines and functions covered
per area; "lines covered only by <label>", the lines that one input covers and
no other does, which is what each build or machine adds; and the files that
one input instruments and another does not. `--lcov` writes the union as lcov,
for `genhtml` or another lcov tool.

Limits:

- The inputs must come from the same commit; line numbers are compared as
  they are.
- Functions are matched by mangled name, and the mangled name of a function
  that takes a standard-library type or a 64-bit integer differs between
  platforms (libstdc++ against libc++; `uint64_t` is `unsigned long` on Linux
  and `unsigned long long` on macOS). About 1900 functions per side differ
  this way between the Linux and macOS exports, so the function union of two
  platforms overstates the total. Compare lines.
- `llvm-cov` HTML and `REPORT.md` are per build: llvm-cov needs the binaries
  and profiles of one build. The combined view is `COMBINED.md` and the lcov.

## Tiers

| tier | what | how it is found |
|---|---|---|
| `ci` | what CI runs | `TEST_DIRS` in `.github/workflows/reconverse-ci.yaml`, read at run time |
| `all` | every other test | directories under `tests/` that `make test` reaches through the aggregator Makefiles (`TESTDIRS`, else `DIRS`) and that have a `test` target |
| `ex` | examples | the same under `examples/` |

Tiers are reported cumulatively: `ci`, `ci+all`, `ci+all+ex`. Aggregators
are not run themselves; their leaves are. `smptest` targets are not run:
reconverse's `testrun` drops `++ppn`, so they differ from the `test` runs
only in PE counts, and every reconverse process already runs several PEs. An aggregator that also runs a
program of its own (`examples/charm++/PUP`) is run with `DIRS` and
`TESTDIRS` emptied. Each tier's directories that have a `test` target but
that no aggregator reaches are listed in `unreached-<tier>.txt` and in the
report: they are not measured.

To add a test to the measured set, give its directory a `test` target and
list it in its parent's `DIRS`; it is picked up on the next run. To add it to
the CI tier, add it to `TEST_DIRS` in `reconverse-ci.yaml`.

`coverage/exclude.txt` lists directories that are skipped (one per line,
reason after `#`; a directory excludes everything below it). Every entry is
a directory that fails to build or to run on this line today; delete the
line when it is fixed.

## Reading the numbers: traps

- **Static libraries hide unlinked code.** Charm++'s libraries are static,
  and llvm-cov sees only what some program links. A source no test links
  (most trace modules, CkLoop, several load balancers) adds nothing to any
  denominator, so the percentage looks better than the tree is. The report
  lists these files under "Compiled sources absent from every binary".
  Reconverse is measured through `libreconverse`, a shared library, so all of
  it is counted.
- **Tests that run nothing.** A `test` target can pass without starting a
  program: a Makefile that does not include `tests/common.mk` expands
  `$(call run, ...)` to nothing, and `testrun` skips a run whose PE count
  does not divide by the process count. `run.sh` checks that each passing
  run wrote a profile and lists those that did not under "Tests that ran
  nothing".
- **"N functions have mismatched data"** from llvm-cov is expected: every
  test defines its own `main` and handlers under the same names, and header
  inlines exist in several binaries. List the names with
  `llvm-profdata show --all-functions <profdata> | grep ':$' | sort | uniq -d`.
  On 2026-10-25 the only runtime functions among them were
  `CmiMemoryInit` and `CmiMemoryIsomallocDisablePop`, which
  `src/conv-core/memory.C` defines once per `-memory` variant; the rest
  were test and example functions.
- **Denominators move.** Which binaries were built, header instantiations
  and build options all change the totals. Compare percentages loosely, and
  covered-line counts of one file where it matters.
- A test that aborts, segfaults or times out writes no profile; its partial
  run is not counted.
- `bin/charmxi` is built uninstrumented, so `src/xlat-i` is not measured.
- Headers reach the profile under several paths (`src/`, the build's
  `include/` copies, the `include/` symlink). The report attributes each copy
  to its `src/` or `contrib/reconverse/` original and each generated
  `.decl.h`/`.def.h` to the directory of its `.ci` file (marked `[gen]`).

## The backlog and the weekly tests

`coverage/backlog.py` turns a published run's lcov into `coverage/backlog.tsv`:
one row per block of uncovered code in `src/ck-core` and
`contrib/reconverse/src`, with a tier (A: the protocols and the scheduler
under them; B: probably dead or not reachable on a CI runner; C: the rest), a
priority, and a status (`open`, `tested`, `partial`, `unreachable`,
`hpc-only`, `skip:<reason>`). Blocks that only abort, assert or print are left
out, and so are macro bodies (lines outside every function). Rows are keyed
by the function and the text of the code, so statuses survive line moves; the
code a `partial` test left uncovered comes back as a new, smaller row.
`backlog.py --check <tsv>` validates the file.

`coverage/check_lines.py <build> <test-dir> <file>:<first>-<last>` cleans,
builds and runs one test directory against an instrumented build and says
whether the range is tested (every line that is not an error path reached),
partial or not reached, with the branch outcomes in the range that never ran.

`.github/workflows/coverage-tests.yaml` runs on Mondays and by hand, never on
pull requests, and does nothing while a pull request labeled
`coverage-backlog` is open. Its `tests` job ranks the backlog from the newest
run measured on `recharm` (`coverage/backlog_job.py rank`), builds an
instrumented Charm++, and has Claude write tests for the first K open rows
(default 3) or give them a status; `backlog_job.py finish` then reduces the
model's changes to a patch of text source files under `tests/` and the
targets' statuses. That job runs with a read-only token and the
`ANTHROPIC_API_KEY` secret of the `claude-ci` environment, which only
`recharm` may use. A second job, `publish`, starts from a fresh checkout,
checks the patch and the statuses again (`backlog_job.py publish`) and opens
one draft pull request; it is the only job with write access and runs no
model. GitHub starts no CI for a pull request a workflow opens: close and
reopen it to run CI.

## Last verification run

2026-10-25, macOS (Apple Silicon, Apple clang 16), charm `8d7638f51`,
reconverse `a8efcfc23`, `./build charm++ reconverse-darwin-arm8 -j6 -g
--coverage` (105 s), shapes 1 and 2. Tiers: ci 21 directories (247 s), all
36 (185 s), ex 81 (378 s); `report.sh` 68 s; 150 binaries measured.

Line coverage (functions and regions are in REPORT.md):

| tier | Charm++, all files | Charm++, hand-written | reconverse |
|---|---|---|---|
| ci | 21.8% (17893 / 82065) | 27.1% (12696 / 46906) | 45.5% (3911 / 8595) |
| ci+all | 30.0% (24600 / 82071) | 36.0% (16896 / 46912) | 47.2% (4054 / 8595) |
| ci+all+ex | 36.1% (29628 / 82067) | 45.4% (21300 / 46908) | 47.3% (4067 / 8595) |

The 2026-10-04 measurement with ad hoc scripts (charm `6eeeae95b`) had
36.3% and 46.9% for the last tier. The hand-written denominator grew by
about 5000 lines since then, mostly `src/conv-core` (memory.C, isomalloc.C,
memory-gnu-internal.C: 4129 lines now, 1 then), which the measured programs
now link. 89 of 219 compiled sources are linked by no program.
Runs that passed without running anything: `dynamic_insertion_deletion`
(ci tier) and `delegation/multicast` with 2 processes (every PE count is
odd), `tests/charm++/collide` (no Collide library in a charm++ build) and
`examples/charm++/allGather` (its Makefile does not include `common.mk`).
Nine `unreg` zerocopy examples fail with 2 processes only ("Network
completion failed for peer rank 0", plain build too).

2026-10-27, the same machine, charm `9eefa72ff`: the ci tier with shape 4
(4 processes) after shapes 1 and 2. All 21 directories built and passed
(250 s); nine runs with `+p6` were skipped. Combined with `combine.py`,
shape 4 covers 20 lines that shapes 1 and 2 do not (`ckreduction.C` 7,
`cksyncbarrier.C` 7, `TreeLevel.h` 4, `ckarray.C` 1, `isomalloc.C` 1), and
shapes 1 and 2 cover 497 lines that shape 4 does not (`TreeLevel.h` 54,
`ckrdma.C` 52, `cklocation.C` 47, reconverse `collectives.cpp` 41: shape 4
runs one PE per process and skips the `+p6` runs). The ci tier on Linux CI and
on macOS cover 212 and 105 lines that the other does not, out of about
21900 each.
