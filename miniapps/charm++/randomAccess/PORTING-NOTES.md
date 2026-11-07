# randomAccess: what stops it from building (November 2026)

`make test` skips randomAccess for now. The code uses the original TRAM
interface (`GroupMeshStreamer::ckNew(bufferSize, dims, ...)`, `init`,
`insertData`, `done`), which Charm++ removed in 2020 (#2775).

Plan: port it to the new TRAM library, htram (<https://github.com/UIUC-PPL/htram>),
not to the in-tree TRAM that replaced the original interface. The in-tree
example `examples/charm++/TRAM/aggregateRandomAccessGroup` shows the update
pattern. `examples/charm++/TRAM/randomAccessGroup` and
`randomAccessArray` contain the same broken code; they are not in that
directory's test list.
