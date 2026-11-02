# Charm++ internals

How the runtime works inside, one chapter per subsystem, written as a
description of the code as it stands. This is the place to start when you
join the group and will work on the runtime rather than on an application.

What belongs here and what does not:

- Each chapter describes the current design: the data structures, the
  protocols between PEs and processes, the invariants, and where in
  `src/` each piece lives. A reader should be able to open the named files
  and recognize what the chapter describes.
- Alternatives considered, measurements, and the history of decisions do
  not belong here. They live in the design documents under `doc/` (for
  example `doc/objid64-design.md` and its companions), which each chapter
  links to. When a design document and a chapter disagree, the chapter is
  wrong or stale: fix the chapter.
- User-facing behavior (what an application programmer calls, what the
  options mean) belongs in the manuals under `doc/charm++/` and
  `doc/converse/`, not here.

Chapters are markdown files in this directory, rendered by GitHub. They are
not yet in the Sphinx tree that builds charm.readthedocs.io; adding them
there needs the `myst-parser` extension in `doc/conf.py` and a toctree
entry, and is planned once there is more than one chapter.

## Chapters

| chapter | status |
|---|---|
| [Object ids and element location](object-ids.md) | written 2026-10-17 (64-bit id redesign, charm #3994) |
| [Routing a send to an array element](array-send-order.md) | written 2026-10-24 (send before the array manager exists, charm #4088) |
| [Array broadcasts and migrating elements](array-broadcast-migration.md) | written 2026-10-25 (broadcast epochs, stored broadcasts, when `stableLocations` applies; AMPI collective hang, charm #4072) |
| Message path: envelope, scheduler, entry method dispatch | not yet written |
| Chare arrays: creation, insertion, migration, sections | not yet written |
| Groups and nodegroups | not yet written |
| Load balancing: the LB framework and TreeLB | not yet written |
| [Replication as a load-balancing decision](lb-multiplicity.md) | written 2026-10-31 (the replica declaration, clone/retire decision kinds, their delivery and memory accounting, ShedLB's group pass) |
| Checkpoint, restart, shrink/expand | not yet written |
| The reconverse layer: what Charm++ asks of the machine layer | not yet written |

To add a chapter, copy the shape of an existing one: a one-paragraph
summary, a diagram where the structure has one, the data structures, the
protocols, the limits and their diagnostics, and a closing "where in the
source" list. Keep it to the current state, and link the design document
that holds the reasons.
