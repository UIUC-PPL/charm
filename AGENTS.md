# Working on Charm++ with an AI coding agent

This file is read by agent tools that look for `AGENTS.md`; `CLAUDE.md`
next to it imports it for Claude Code. It says where the knowledge is, not
how to code.

## Read these first

- **The manual**, <https://charm.readthedocs.io> (source under `doc/`):
  what an application calls, what an option means, how to build and run.
- **`doc/internals/`**: how the runtime works inside, one chapter per
  subsystem, with the files in `src/` each chapter describes. Start at
  `doc/internals/README.md`. A change to a subsystem that has a chapter
  updates the chapter in the same pull request.
- **The practitioner notes**, <https://github.com/charmplusplus/charm-notes>:
  dated lessons from real runs, a syntax quick reference, machine
  profiles, and an LCI reference. The repository is a skill: clone it and
  link it as `~/.claude/skills/charm-notes` (Claude Code) or upload the
  package it builds (claude.ai). Prefer a dated note over general memory
  of Charm++ when they disagree, and say which note you relied on.

## Testing

- `tests/`, `examples/` and `benchmarks/` each have a `make test`; the
  manual's "Testing and coverage" section explains the targets.
- Every pull request runs the directories listed in `TEST_DIRS` of
  `.github/workflows/reconverse-ci.yaml`. A change to the runtime is
  expected to come with a test that reaches it.
- How much of `src/` and reconverse the tests reach is measured weekly at
  <https://charmplusplus.org/charm-coverage/>; `coverage/README.md` has
  the recipe for measuring locally and the traps in reading the numbers.

## Conventions

- Comments describe the code as it is; the reasons and the history go in
  the pull request and, for a subsystem with a chapter, in `doc/internals/`.
- The default branch is `recharm` (Charm++ on reconverse). `classic` is
  the frozen classic-Converse line and takes fixes only.
- Reconverse is the `contrib/reconverse` submodule; its own repository is
  <https://github.com/charmplusplus/reconverse>.
