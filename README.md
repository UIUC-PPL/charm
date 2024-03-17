# Charm++

[![Build Status](https://travis-ci.org/UIUC-PPL/charm.svg?branch=main)](https://travis-ci.org/UIUC-PPL/charm)
[![Documentation Status](https://readthedocs.org/projects/charm/badge/?version=latest)](https://charm.readthedocs.io/en/latest/?badge=latest)
[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.3370873.svg)](https://doi.org/10.5281/zenodo.3370873)


## Introduction

Charm++ is a message-passing parallel language and runtime system.
It is implemented as a set of libraries for C++, is efficient,
and is portable to a wide variety of parallel machines.
Source code is provided, and non-commercial use is free.


## Getting the Latest Source

You can use anonymous Git access to obtain the latest Charm++ source
code, as follows:

     $ git clone https://github.com/UIUC-PPL/charm


## Build Configuration

### Quick Start:

First-time users are encouraged to run the top-level `build` script and follow its lead:
    
    $ ./build

### Advanced Build Options:
First, you need to decide which version of Charm++ to use. The `build`
script takes several command line options to compile Charm++. The command line syntax is:

     $ ./build <target> <version> [options ...]
                                  [--basedir=dir] [--libdir=dir] [--incdir=dir]
                                  [charmc-options ...]

For detailed help messages, pass `-h` or `--help` to the build script.

### Required:

`<target>` specifies the parts of Charm++ to compile.  The most often used
`<target>` is `charm++`, which will compile the key Charm++ executables and
runtime libraries.  Other common targets are `AMPI` and `LIBS`, which build
Adaptive MPI and Charm++ and all of its libraries, respectively.
`<version>` defines the CPU, OS and communication layer of the machines.  See
  "How to choose a `<version>`" below for details.

### Optional:

`<options>` defines more detailed information of the compilations, including
compilers, features to support, etc.  See "How to choose `<options>`"
below.

* `[--libdir=dir]` specify additional lib paths for building Charm++.
* `[--incdir=dir]` specify additional include paths for building Charm++.
* `[--basedir=dir]` a shortcut to specify additional include and library paths
  for building Charm++, the include path is `dir/include` and library path
  is `dir/lib`.


Running build script, a directory of the name of combination of version and
options like `<version>-<option1>-<option2>-...` will be created and
the build script will compile Charm++ under this directory.

For example, on an ordinary Linux PC:

     $ ./build charm++ netlrts-linux-x86_64

will build Charm++ in the directory `netlrts-linux-x86_64/`. The communication
defaults to UDP packets and the compiler to `gcc`.

For a more complex example, consider a shared-memory build with the
Intel C++ compiler `icc`, where you want the communication to happen over TCP sockets:

     $ ./build charm++ netlrts-linux-x86_64 smp icc tcp

will build Charm++ in the directory `netlrts-linux-x86_64-smp-tcp-icc/`.

You can specify multiple options, however you can use at most one compiler
option. The sequencing of options given to the build script should follow the rules:
1. Compiler option should be at the end
2. Other options are sorted alphabetically

### How to choose a `<version>`:
Here is the table for choosing a correct build version. The default compiler
in Charm++ is `gcc/g++` on Linux and `clang/clang++` on MacOS. However,
one can use `<options>` to specify other compilers. See the detailed explanation
of the `<options>` below.

(Note: this isn't a complete list.  Run `./build` for a complete listing)

| Charm++ Version           | OS      | Communication | Default Compiler                      |
|---------------------------|---------|---------------|---------------------------------------|
| `netlrts-linux-x86_64`    | Linux   | UDP           | GNU compiler                          |
| `netlrts-darwin-x86_64`   | macOS   | UDP           | Clang C++ compiler                    |
| `netlrts-win-x86_64`      | Windows | UDP           | MS Visual C++                         |
| `mpi-linux-x86_64`        | Linux   | MPI           | GNU compiler                          |
| `multicore-linux-x86_64`  | Linux   | Shared memory | GNU compiler                          |
| `multicore-darwin-x86_64` | macOS   | Shared memory | Clang C++ compiler                    |
| `gni-crayxc`              | Linux   | GNI           | CC (whatever PrgEnv module is loaded) |
| `gni-crayxe`              | Linux   | GNI           | CC (whatever PrgEnv module is loaded) |
| `verbs-linux-x86_64`      | Linux   | IB Verbs      | GNU compiler                          |
| `ofi-linux-x86_64`        | Linux   | OFI           | GNU compiler                          |
| `ucx-linux-x86_64`        | Linux   | UCX           | GNU compiler                          |


To choose `<version>`, your choice is determined by two options:

1. The way a parallel program written in Charm++ will communicate:
    - `netlrts-`: Charm++ communicates using the regular TCP/IP stack (UDP packets),
    which works everywhere but is fairly slow.  Use this option for networks of workstations,
    clusters, or single-machine development and testing.
    * `gni-`, `pamilrts-`, `verbs-`, `ofi-`, `ucx-` : Charm++ communicates using direct calls to the machine's
    communication primitives. Use these versions on machines that support them for best performance.
    * `mpi-`: Charm++ communicates using MPI calls. This will work on almost every distributed machine,
    but performance is often worse than using the machine's direct calls referenced above.
    * `multicore-`: Charm++ communicates using shared memory within a single node. A version of
    Charm++ built with this option will not run on more than a single node.


2. Your operating system/architecture:
    * `linux-x86_64`: Linux with AMD64 64-bit x86 instructions
    * `win-x86_64`: MS Windows with MS Visual C++ compiler
    * `darwin-x86_64`: Apple macOS
    * `cray{xe/xc}`: Cray XE/XC Supercomputer
    * `linux-ppc64le`: POWER/PowerPC

Your Charm++ version is made by concatenating the options, e.g.:
* `netlrts-linux-x86_64`: Charm++ for a network of 64-bit Linux workstations compiled using g++.
* `gni-crayxc`: Charm++ for Cray XC systems using the system compiler.


### How to choose `<options>`:

`<version>` above defines the most important OS, CPU, and communication of
your machine.

To use a different compiler or demand additional special feature support, you
need to choose `<options>` from the following list (compilers may also be
suffixed with a version number to use a specific version, e.g. `gcc-7`). Note that
this list is merely a sampling of common options, please see the documentation
for more information:

* `icc` - Intel C/C++ compiler.
* `ifort` - Intel Fortran compiler
* `xlc` - IBM XLC compiler.
* `clang` - Clang compiler.
* `mpicxx` - Use MPI-wrappers for MPI builds.
* `pgcc` - Portland Group's C++ compiler.
* `smp` - Enable direct SMP support.  An `smp` version communicates using
    shared memory within a process but normal message passing across
    processes and nodes. `smp` mode also introduces a dedicated 
    communication thread for every process. Because of locking, `smp` may
    slightly impact non-SMP performance. Try your application to decide if
    enabling `smp` mode improves performance.
* `tcp` - The `netlrts-` version communicates via UDP by default. The `tcp` option
    will use TCP instead. The TCP version of Charm++ is usually slower
    than UDP, but it is more reliable.
* `async` - On PAMI systems, this option enables use of hardware communication
    threads. For applications with significant communication on large
    scale, this option typically improves performance.
* `regularpages` - On Cray systems, Charm++'s default is to use `hugepages`. This
    option disables `hugepages`, and uses regular `malloc` for messages.
* `persistent` - On Cray systems, this option enables use of persistent mode for
    communication.
* `pxshm` - Use POSIX Shared Memory for communication between Charm++ processes
    within a shared-memory host.
* `syncft` - Enable in-memory fault tolerance support in Charm++.
* `tsan` - Compile Charm++ with support for Thread Sanitizer.
* `papi` - Enable PAPI performance counters.
* `ooc` - Build Charm++ with out-of-core execution features.
* `help` - show supported options for a version. For example, for `netlrts-linux-x86_64`, running:
         
         $ ./build charm++ netlrts-linux-x86_64 help
         
     will give:
    
      Supported compilers: clang craycc gcc icc iccstatic msvc pgcc xlc xlc64 icx
      Supported options: common cuda flang gfortran ifort local nolb omp ooc papi perftools persistent pgf90 pxshm smp syncft sysvshm tcp tsan


## Building the Source

If you have downloaded a binary version of Charm++, you can skip
this step -- Charm++ should already be compiled.

Once you have decided on a version, unpack Charm++, `cd` into `charm/`,
and run

     $ ./build <target> <version> <opts>

`<target>` is one of:
        
* `charm++`  The basic Charm++ language
* `AMPI`     An implementation of MPI on top of Charm++
* `LIBS`    Charm++, AMPI, and other libraries built on top of them

`<version>` is described above in the "How to choose a `<version>`" section.

`<opts>` are build-time options (such as the compiler or `smp`), or command
line options passed to the `charmc` compiler script. Common compile time
options such as `-g`, `-O`, `-Ipath`, `-Lpath`, `-llib` are accepted (these
may vary depending on the compiler one has selected).

For example, on a Linux machine, you would run
     
     $ ./build charm++ netlrts-linux-x86_64 -O

This will construct a `netlrts-linux-x86_64` directory, link over all
the Charm++ source code into `netlrts-linux-x86_64/tmp`, build the entire
Charm++ runtime system in `netlrts-linux-x86_64/tmp`, and link example
programs into `netlrts-linux-x86_64/examples`.

Charm++ can be compiled with several optional features enabled or
disabled. These include runtime error checking, tracing, interactive
debugging, deterministic record-replay, and more. They can be
controlled by passing flags of the form `--enable-<featurename>` or
`--disable-<featurename>` to the build command:

     $ ./build charm++ netlrts-linux-x86_64 --disable-tracing

Production optimizations: Pass the configure option `--with-production`
to `./build` to turn on optimizations in Charm++/Converse. This disables
most of the run-time checking performed by Converse and Charm++
runtime. This option should be used only after the program has been
debugged. Also, this option disables Converse/Charm++ tracing
mechanisms such as projections and summary.

Performance analysis: Pass the configuration option `--enable-tracing` to
enable tracing and generation of logs for analysis with Projections. This is
the recommended way to analyze performance of applications.

When Charm++ is built successfully, the directory structure under the
target directory will look like:
```
netlrts-linux-x86_64/
   |
   ---  benchmarks/      # benchmark programs
   |
   ---  bin/             # all executables
   |
   ---  doc/             # documentations
   |
   ---  include/         # header files
   |
   ---  lib/             # static libraries
   |
   ---  lib_so/          # shared libraries
   |
   ---  examples/        # example programs
   |
   ---  tests/           # test programs
   |
   ---  tmp/             # Charm++ build directory
```

## Building a Program

To make a sample program, `cd` into `examples/charm++/NQueen/`.
This program solves the _n_ queens problem-- find how many ways there
are to arrange _n_ queens on an _n_ x _n_ chess board such that none may
attack another.

To build the program, type `make`.  You should get an
executable named `nqueen`.


## Running a Program

Following the previous example, to run the program on two processors, type

     $ ./charmrun ++n 2 ./nqueen 12 6

This should run for a few seconds, and print out:
`There are 14200 Solutions to 12 queens. Time=0.109440 End time=0.112752`

`charmrun` is used to provide a uniform interface to run charm programs.
On some platforms, `charmrun` is just a shell script which calls the
platform-specific start program, such as `mpirun` on MPI versions.

For the `netlrts-` version, `charmrun` is an executable which invokes ssh to start
node programs on remote machines. You should set up a `~/.nodelist` that
enumerates all the machines you want to run jobs on, otherwise it will
create a default `~/.nodelist` for you that contains only `localhost`. Here is a
typical `.nodelist` file:

    group main ++shell /bin/ssh
    host <machinename>

The default remote shell program is ssh, but you can define a different remote
shell to start remote processes using the `++shell` option. You should
also make sure that ssh or your alternative can connect to these machines without
password authentication. Just type following command to verify:
    
    $ ssh <machinename> date

If this gives you current date immediately, your running environment with this
node has been setup correctly.

For development purposes, the `netlrts-` version of `charmrun` comes with an easy-to-use
`++local` option. No remote shell invocation is needed in this case. It starts
node programs right on your local machine. This could be useful if you just
want to run program on only one machine, for example, your laptop. This
can save you all the hassle of setting up ssh daemons.
To use this option, just type:

     $ ./charmrun ++local ./nqueen 12 100 ++n 2

However, for best performance, you should launch one node program per processor.


## Building Dynamic Libraries

In order to compile Charm++ into dynamic libraries, one needs to specify the
`--build-shared` option to the Charm `./build` script. Charm++'s dynamic
libraries are compiled into the `lib_so/` directory. Typically, they are
generated with a `.so` suffix.

One can compile a Charm++ application linking against Charm++ dynamic
libraries by linking with `charmc`'s `-charm-shared` option.
For example:

     $ charmc -o pgm pgm.o -charm-shared

You can then run the program as usual.
Note that linking against Charm++ dynamic libraries produces much smaller
binaries and takes much less linking time.


## Contributing

The recommended way to contribute to Charm++ development is to open a pull request (PR) on GitHub.
To open a pull request, create a fork of the Charm++ repo in your own space
(if you already have a fork, make sure is it up-to-date), and then create a new branch off of the
`main` branch.

GitHub provides a detailed tutorial on creating pull requests 
(https://docs.github.com/en/pull-requests/collaborating-with-pull-requests). 

Each pull request must pass code review and CI tests before it can be merged by someone on
the core development team.
Our wiki contains additional information about pull requests
(https://github.com/UIUC-PPL/charm/wiki/Working-with-Pull-Requests).


## For More Information

The Charm++ documentation is at https://charm.readthedocs.io/

The Charm++ web page, with more information, more programs,
and the latest version of Charm++, is at https://charmplusplus.org

The UIUC Parallel Programming Laboratory web page, with information
on past and present research, is at https://charm.cs.illinois.edu

For questions, comments, suggestions, improvements, or bug reports,
please create an issue or discussion on our GitHub, https://github.com/UIUC-PPL/charm


## Authors

Charm++ was created and is maintained by the Parallel Programming Lab,
in the Computer Science department at the University of Illinois at
Urbana-Champaign.  Our managing professor is Dr. L.V. Kale; students
and staff have included (in rough time order) Wennie Shu, Kevin Nomura, Wayne
Fenton, Balkrishna Ramkumar, Vikram Saletore, Amitabh B. Sinha, Manish
Gupta, Attila Gursoy, Nimish Shah, Sanjeev Krishnan, Jayant DeSouza,
Parthasarathy Ramachandran, Jeff Wright, Michael Lang, Jackie Wang,
Fang Hu, Michael Denardo, Joshua Yelon, Narain Jagathesan, Zehra Sura,
Krishnan Varadarajan, Sameer Paranjpye, Milind Bhandarkar, Robert Brunner,
Terry Wilmarth, Gengbin Zheng, Orion Lawlor, Celso Mendes, Karthik Mahesh,
Neelam Saboo, Greg Koenig, Sameer Kumar, Sayantan Chakravorty, Chao Huang,
Chee Lee, Fillipo Gioachin, Isaac Dooley, Abhinav Bhatele, Aaron Becker,
Ryan Mokos, Ramprasad Venkataraman, Gagan Gupta, Pritish Jetley, Lukasz
Wesolowski, Esteban Meneses, Chao Mei, David Kunzman, Osman Sarood,
Abhishek Gupta, Yanhua Sun, Ehsan Totoni, Akhil Langer, Cyril Bordage,
Harshit Dokania, Prateek Jindal, Jonathan Lifflander, Xiang Ni,
Harshitha Menon, Nikhil Jain, Vipul Harsh, Bilge Acun, Phil Miller,
Seonmyeong Bak, Karthik Senthil, Juan Galvez, Michael Robson, Raghavendra
Kanakagiri, and Venkatasubrahmanian Narayanan. Current developers include
Eric Bohm, Ronak Buch, Eric Mikida, Sam White, Nitin Bhat, Kavitha
Chandrasekar, Jaemin Choi, Matthias Diener, Evan Ramos, Justin Szaday,
Zane Fink, and Pathikrit Ghosh.

Copyright (C) 1989-2023 Regents of the University of Illinois
