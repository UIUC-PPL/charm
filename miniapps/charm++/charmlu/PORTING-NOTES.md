# charmlu: what stops it from running (November 2026)

`make test` skips charmlu for now. Two problems stop it; both are open
questions about Charm++, not only porting work.

## 1. `[local]` SDAG entry methods that contain `when`

`lu.ci` declares `pivotBatch`, `factorAboveDiagBlock` and
`factorBelowDiagBlock` as `[local]` SDAG entry methods with `when` clauses. A
`[local]` call runs the method as a direct function call. For an SDAG method
with a `when`, charmxi generates a stub that aborts with "Direct SDAG call is
not allowed". That stub was added in 2018 by commit 7c127d8c5 (charmxi #738,
"prevent direct calls to sdag entry functions"). The manual's reason: such a
method is asynchronous, so it must be called through a proxy rather than look
like a function call that returns at once. charmlu predates that change.
Dropping `[local]` makes the solve hang.

To do:
- Track how `[local]` calls of SDAG methods with `when` behaved before
  7c127d8c5, and why charmlu relied on them. Is there a pattern here, a
  synchronous start of an SDAG method on a local object, that is worth
  supporting?
- Then either allow `[local]` on such methods in charmxi, or change charmlu,
  and find out why it hangs without `[local]`.

## 2. Arrays created from a PE other than 0

charmlu's `LUSolver` creates chare arrays, and seed balancing can place it on a
PE other than 0. A synchronous `CProxy_X::ckNew` of an array on a PE other
than 0 fails on both lines (probe: one array created on PE 0 and one from a
chare on PE 1, 2 PEs):

- recharm: aborts at `CkAssert(CkMyPe() == 0)` in
  `ck::objid::nextCollectionNumber` (`src/ck-core/cklocation.C`). Collection
  numbers come from a per-process counter that only PE 0 may use. The assert
  fires even in a `--with-production` build.
- classic (8.x, netlrts): the group id of an array created on PE 1 is
  negative, and object-id creation aborts with "ObjID ran out of collection
  bits".

Creating an array from any PE used to work in classic Charm++. To do:
- Find when it stopped working in classic: bisect with a probe like the one
  above. The negative-gid check in the ObjID code is a likely place.
- Unless that change had a good reason for dropping it, make it work again on
  recharm: for example, have `CkCreateArray` on another PE get its collection
  number from PE 0, or reserve per-PE ranges.
- Meanwhile `ckNew` with a callback (asynchronous creation, which runs on
  PE 0) works from any PE.

## Changes charmlu needs once it runs

- Create `LUSolver` on PE 0, unless problem 2 is fixed.
- Turn the residual warning into `CkAbort`, so `make test` fails on a wrong
  answer.
