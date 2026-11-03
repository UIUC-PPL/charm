# Charm++ on Windows (`reconverse-win-x86_64`)

`reconverse-win-x86_64` builds Charm++ on Reconverse for 64-bit Windows,
using Microsoft Visual C++ (MSVC). Programs built with it are native Windows
executables.

A program runs as **one process**: its PEs are threads in that process, so
`+pe N` gives you N PEs on one machine. A Windows build has no communication
backend, so it cannot run across processes or machines. The target is meant
for desktop use, NAMD in particular.

The build is driven from **Cygwin**, as Charm++'s Windows builds always have
been. Cygwin supplies bash, make, CMake and a C preprocessor. All compiling
and linking is done by MSVC, through the wrapper script
`src/arch/win/unix2nt_cc`.

## Requirements

- **Visual Studio 2022**, or the Build Tools for Visual Studio 2022, with the
  **Desktop development with C++** workload. It provides the tools the build
  calls:
  - `cl`, the compiler
  - `link`, the linker
  - `ml64`, the assembler (Reconverse's context-switch code is MASM)
  - `lib`, the library archiver
  - the Windows SDK
- **Cygwin (64-bit)**, from <https://cygwin.com/install.html>, with these
  packages in addition to the default set:
  - `cmake`
  - `make`
  - `gcc-core`, which provides the `cpp` that preprocesses `.ci` files (gcc
    itself is not used)
  - `git`, unless you clone with Git for Windows

  CI installs exactly `cmake make gcc-core`.

MSYS2, MinGW and Git Bash are not supported: `./build` stops with an error
under them. You do not need Python, MPI, libfabric or hwloc, because a
Windows build does not fetch or build LCI.

## Get the source

The build scripts run under bash, so they must be checked out with Unix (LF)
line endings. Git for Windows converts them to CRLF by default. Turn that off
**before** cloning. This changes the setting for all your repositories; it is
the reliable way, because the submodule is cloned with your global settings.
(Cygwin's own `git` does not convert line endings.)

```bash
git config --global core.autocrlf false
git clone --recurse-submodules https://github.com/charmplusplus/charm
```

Until charmplusplus/charm#4087 is merged, use its branch:

```bash
cd charm
git checkout windows-reconverse
git submodule update --init -- contrib/reconverse
```

## Open a build shell

The build needs two environments at once: MSVC's, so that `cl`, `link` and
`ml64` are on `PATH` and `VCINSTALLDIR` is set, and Cygwin's. To get both:

1. From the Start menu, open **x64 Native Tools Command Prompt for VS 2022**.
   Don't use the plain "Developer Command Prompt": by default it targets
   32-bit x86.
2. In that prompt, start a Cygwin login shell that stays in the current
   directory:

   ```bat
   cd C:\path\to\charm
   set CHERE_INVOKING=1
   C:\cygwin64\bin\bash.exe --login
   ```

   Cygwin's login shell puts its own directories in front of the inherited
   `PATH`, so the MSVC tools remain available.
3. Check that the shell finds the tools:

   ```bash
   command -v cl ml64 cmake cpp
   ```

   - `cl` and `ml64` should be under `.../MSVC/<version>/bin/Hostx64/x64/`.
   - `cmake` should be `/usr/bin/cmake`, Cygwin's. Visual Studio's CMake is
     also on `PATH`, but it does not understand Cygwin paths.
   - `command -v link` prints `/usr/bin/link`. This is Cygwin's `link`
     command, not MSVC's linker; `unix2nt_cc` works around it, so ignore it.

## Build Charm++

```bash
./build charm++ reconverse-win-x86_64 --with-production -j8
```

- For a debug build, use `-g` instead of `--with-production`.
- The configure step takes a few minutes on a desktop. The full build takes
  longer.
- The build goes into `reconverse-win-x86_64/`. The top-level `bin/`,
  `include/` and `lib/` links point at it.

For this target, the build makes the following choices itself. Don't
override them:

- `./build` selects `unix2nt_cc` as the C and C++ compiler and disables
  Fortran.
- `./build` turns off Reconverse's LCI and LCW backends and does not fetch LCI.
- Reconverse is built as a static library, archived with MSVC's `lib`. An
  MSVC DLL cannot export the runtime's thread-local variables.
- `charmc` compiles C++ as C++17 (`/std:c++17`, set in
  `src/arch/common/cc-msvc.sh`). MSVC defaults to C++14, and Reconverse needs
  C++17.

## Build and run a program

```bash
cd examples/charm++/NQueen
make
./nqueen.exe +pe 4 12 6
```

`charmc` adds `.exe` to executable names. Every link also writes a `.pdb`
(debug symbols) next to the `.exe`.

Run the executable directly: there is no `charmrun`, `lcrun` or `mpiexec` on
Windows. `+pe` is the number of PEs (threads). The program is linked against
the static MSVC runtime (`/MT`) and Windows system libraries only. It also
runs from `cmd.exe` or PowerShell, without Cygwin.

### Compiler flags and mixing in other libraries

- **`charmc -g`** adds debug information (`/Z7`) and keeps the release C
  runtime. So you can link a `-g` application against a `--with-production`
  Charm++, and a `-g` Charm++ links against Reconverse, which CMake builds
  without `-g`.
- **Other libraries.** Static libraries and object files you link into a
  Charm++ program must also be built with the static release runtime
  (`/MT`). One built with `/MD` or `/MTd` fails at link time with `LNK2038`
  (`RuntimeLibrary` or `_ITERATOR_DEBUG_LEVEL` mismatch). DLLs, linked
  through their import libraries, can use any runtime: NAMD links Tcl and
  FFTW this way.
- **Seeing the real commands.** `charmc -verbose` prints the `unix2nt_cc`
  command lines. To see the `cl`, `link` and `ml64` commands that
  `unix2nt_cc` runs, pass it `-verbose`: `charmc -c++-option -verbose` when
  compiling C++ (`-cc-option` for C), and `charmc -ld++-option -verbose` when
  linking.

## Run the tests

This runs the same tier CI runs. `TEST_DIRS` is defined in
`.github/workflows/reconverse-ci.yaml`:

```bash
TEST_DIRS=$(sed -n '/^  TEST_DIRS: >-/,/^$/p' .github/workflows/reconverse-ci.yaml | sed 1d)
B=$PWD/reconverse-win-x86_64
for d in $TEST_DIRS; do make -C $d CHARMC=$B/bin/charmc; done
for d in $TEST_DIRS; do make -C $d test RUN_PROCS=; done
```

`RUN_PROCS=` turns the two-process runs that some test Makefiles request
(`user-driven-interop`) into single-process ones. The tier passes with both a
`-g` and a `--with-production` build.

## Crashes and debugging

- **Stack traces.** `CkAbort`/`CmiAbort`, and fatal faults such as access
  violations, print a stack trace. With the `.pdb` present, frames show
  function names, for example `Main::Main+0x51`; code compiled with `-g`
  also shows file:line. Without the `.pdb`, the program's own frames show
  only module+offset. A fault is reported with the POSIX signal it
  corresponds to (for example `SIGSEGV`).
- **No crash dialogs.** The Windows Error Reporting and `abort()` dialogs are
  suppressed, so a crashed run exits instead of waiting for someone to close
  a window.
- **`+truecrash`** leaves Windows' default crash handling in place. Use it
  when you want the Visual Studio just-in-time debugger to catch the fault.
- **Debugger.** To debug from the start, open the `.exe` in Visual Studio
  (File > Open > Project/Solution, then pick the `.exe`). Set the command
  arguments, for example `+pe 2 12 6`, and run. Build with `charmc -g` for
  full local-variable information.
- **Stack sizes.** The main thread, on which PE 0 runs, and every other PE
  thread each reserve an 8 MB stack. The Windows default is 1 MB.

## Limitations

- **One process only.** There is no multi-process or multi-machine execution
  on Windows. For that, build `reconverse-linux-x86_64` on Linux or WSL.
- **No `--enable-shmem`.** Reconverse has no shared-memory IPC pool on
  Windows. A program built with it aborts at startup with "the shared-memory
  IPC pool is not available on Windows".
- **No CPU affinity.**
  - hwloc is not built or used.
  - `+setcpuaffinity`, `+pemap` and `+showcpuaffinity` are accepted but do
    nothing.
- **AMPI and Fortran.** Fortran is disabled, so Fortran AMPI is unavailable.
  C/C++ AMPI is not covered by the Windows CI tier.
- **CCS** (`++server`, used by liveViz, PythonCCS and CharmDebug) is
  untested on Windows. Its server opens a TCP listening port,
  and Windows Defender Firewall then asks for administrator approval.
- **Untested options.** `--build-shared` and compilers other than MSVC (for
  example clang-cl) have not been tested with this target.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `$VCINSTALLDIR is not set properly` | The Cygwin shell was not started from the x64 Native Tools prompt. Start it as in [Open a build shell](#open-a-build-shell). |
| `$'\r': command not found`, or `bad interpreter` from `./build` | The checkout has CRLF line endings. Set `core.autocrlf false` and clone again. |
| `building Charm++ under MSYS or MinGW is not supported` | You are in Git Bash or MSYS2. Use Cygwin's bash. |
| CMake errors about paths such as `/cygdrive/...` or `C:/...` | Visual Studio's CMake is being used instead of Cygwin's. `command -v cmake` must print `/usr/bin/cmake`. |
| `cpp: command not found` while building `.ci` files | Install Cygwin's `gcc-core` package. |
| `LNK2038` mismatch for `RuntimeLibrary` or `_ITERATOR_DEBUG_LEVEL` | An object or library was built with a different C runtime. Rebuild it with `/MT`, as described under [Compiler flags and mixing in other libraries](#compiler-flags-and-mixing-in-other-libraries). |
| Stack trace shows only `module+offset` | The `.pdb` is missing. Keep it next to the `.exe`. |
