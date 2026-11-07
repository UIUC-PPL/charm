# Charm++ mini-applications

Mini-applications that show how Charm++ programs are structured at a larger
scale than the programs in `examples/`, and that serve as benchmarks and as
stress tests of the runtime. They are described at
<https://charmplusplus.org/miniApps/>.

| directory | what it is |
|---|---|
| `charm++/amr` | mesh restructuring for adaptive mesh refinement |
| `charm++/barnes` | Barnes-Hut N-body (needs the `structures` library from ChaNGa) |
| `charm++/charmlu` | dense LU solver, from the HPC Challenge (needs BLAS and LAPACK) |
| `charm++/fft-trans` | 1D FFT with three all-to-all transposes (needs FFTW3) |
| `charm++/hpccg` | HPCCG conjugate gradient, from the Mantevo suite |
| `charm++/hpccstream` | HPC Challenge STREAM (EP Stream) |
| `charm++/kripke` | LLNL's Kripke, a proxy for Sn particle transport |
| `charm++/leanmd` | molecular dynamics with the Lennard-Jones potential |
| `charm++/randomAccess` | HPC Challenge RandomAccess |
| `charm++/triangularsolver` | solver for sparse triangular systems |

Each was imported with its history from the repository of the same name under
<https://github.com/UIUC-PPL> in November 2026. The imported commits keep their
original paths, so `git log -- miniapps/charm++/<name>` shows only the import
merge; `git log <merge>^2` shows the history before it. `examples/charm++/leanmd` is an older, simpler version of LeanMD.

## Licenses

Everything here is under the Charm++ license (`LICENSE` at the top of the
repository), except:

- `charm++/hpccg`: the files carrying the Sandia Corporation copyright are
  under the GNU Lesser General Public License, version 2.1 or later
  (`charm++/hpccg/COPYING.LESSER`).
- `charm++/kripke`: a Charm++ port of LLNL's Kripke, under the BSD 3-Clause
  License (`charm++/kripke/LICENSE`).
- `charm++/hpccstream/MPIStream`: John D. McCalpin's STREAM, under the license
  in the header of `stream.c`, which includes conditions on publishing
  results.

The external libraries that charmlu (BLAS, LAPACK), fft-trans (FFTW3) and
barnes (ChaNGa's `structures`) need are not part of this repository; build
against the system's copies.
