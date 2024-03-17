Example Applications
====================

| This section contains a list of applications that have been written or
  adapted to work with AMPI. Most applications are available on git:
| ``git clone ssh://charm.cs.illinois.edu:9418/benchmarks/ampi-benchmarks``.

Most benchmarks can be compiled with the provided top-level Makefile:

.. code-block:: bash

       $ git clone ssh://charm.cs.illinois.edu:9418/benchmarks/ampi-benchmarks
       $ cd ampi-benchmarks
       $ make -f Makefile.ampi

Mantevo project v3.0
--------------------

Set of mini-apps from the Mantevo project. Download at
https://mantevo.org/download/.

MiniFE
~~~~~~

-  Mantevo mini-app for unstructured implicit Finite Element
   computations.

-  No changes necessary to source to run on AMPI. Modify file
   ``makefile.ampi`` and change variable ``AMPIDIR`` to point to your
   Charm++ directory, execute ``make -f makefile.ampi`` to build the
   program.

-  Refer to the ``README`` file on how to run the program. For example:
   ``./charmrun ++n 4 ./miniFE.x nx=30 ny=30 nz=30 +vp32``

MiniMD v2.0
~~~~~~~~~~~

-  Mantevo mini-app for particle interaction in a Lennard-Jones system,
   as in the LAMMPS MD code.

-  No changes necessary to source code. Modify file ``Makefile.ampi``
   and change variable ``AMPIDIR`` to point to your Charm++ directory,
   execute ``make ampi`` to build the program.

-  Refer to the ``README`` file on how to run the program. For example:
   ``./charmrun ++n 4 ./miniMD_ampi +vp32``

CoMD v1.1
~~~~~~~~~

-  Mantevo mini-app for molecular dynamics codes:
   https://github.com/exmatex/CoMD

-  To AMPI-ize it, we had to remove calls to not thread-safe
   ``getopt()``. Support for dynamic load balancing has been added in
   the main loop and the command line options. It will run on all
   platforms.

-  Just update the Makefile to point to AMPI compilers and run with the
   provided run scripts.

MiniXYCE v1.0
~~~~~~~~~~~~~

-  Mantevo mini-app for discrete analog circuit simulation, version 1.0,
   with serial, MPI, OpenMP, and MPI+OpenMP versions.

-  No changes besides Makefile necessary to run with virtualization. To
   build, do ``cp common/generate_info_header miniXyce_ref/.``, modify
   the CC path in ``miniXyce_ref/`` and run ``make``. Run scripts are in
   ``test/``.

-  Example run command:
   ``./charmrun ++n 3 ./miniXyce.x +vp3 -circuit ../tests/cir1.net -t_start 1e-6 -pf params.txt``

HPCCG v1.0
~~~~~~~~~~

-  Mantevo mini-app for sparse iterative solves using the Conjugate
   Gradient method for a problem similar to that of MiniFE.

-  No changes necessary except to set compilers in ``Makefile`` to the
   AMPI compilers.

-  Run with a command such as:
   ``./charmrun ++n 2 ./test_HPCCG 20 30 10 +vp16``

MiniAMR v1.0
~~~~~~~~~~~~

-  miniAMR applies a stencil calculation on a unit cube computational
   domain, which is refined over time.

-  No changes if using swapglobals. Explicitly extern global variables
   if using TLS.

Not yet AMPI-zed (reason)
~~~~~~~~~~~~~~~~~~~~~~~~~

MiniAero v1.0 (build issues), MiniGhost v1.0.1 (globals), MiniSMAC2D
v2.0 (globals), TeaLeaf v1.0 (globals), CloverLeaf v1.1 (globals),
CloverLeaf3D v1.0 (globals).

LLNL ASC Proxy Apps
-------------------

LULESH v2.0
~~~~~~~~~~~

-  LLNL Unstructured Lagrangian-Eulerian Shock Hydrodynamics proxy app:
   https://codesign.llnl.gov/lulesh.php

-  Charm++, MPI, MPI+OpenMP, Liszt, Loci, Chapel versions all exist for
   comparison.

-  Manually privatized version of LULESH 2.0, plus a version with PUP
   routines in subdirectory ``pup_lulesh202/``.

AMG 2013
~~~~~~~~

-  LLNL ASC proxy app: Algebraic Multi-Grid solver for linear systems
   arising from unstructured meshes:
   https://codesign.llnl.gov/amg2013.php

-  AMG is based on HYPRE, both from LLNL. The only change necessary to
   get AMG running on AMPI with virtualization is to remove calls to
   HYPRE’s timing interface, which is not thread-safe.

-  To build, point the CC variable in Makefile.include to your AMPI CC
   wrapper script and ``make``. Executable is ``test/amg2013``.

Lassen v1.0
~~~~~~~~~~~

-  LLNL ASC mini-app for wave-tracking applications with dynamic load
   imbalance. Reference versions are serial, MPI, Charm++, and
   MPI/Charm++ interop: https://codesign.llnl.gov/lassen.php

-  No changes necessary to enable AMPI virtualization. Requires some
   C++11 support. Set ``AMPIDIR`` in Makefile and ``make``. Run with:
   ``./charmrun ++n 4 ./lassen_mpi +vp8 default 2 2 2 50 50 50``

Kripke v1.1
~~~~~~~~~~~

-  LLNL ASC proxy app for ARDRA, a full Sn deterministic particle
   transport application: https://codesign.llnl.gov/kripke.php

-  Charm++, MPI, MPI+OpenMP, MPI+RAJA, MPI+CUDA, MPI+OCCA versions exist
   for comparison.

-  Kripke requires no changes between MPI and AMPI since it has no
   global/static variables. It uses cmake so edit the cmake toolchain
   files in ``cmake/toolchain/`` to point to the AMPI compilers, and
   build in a build directory:

   .. code-block:: bash

      $ mkdir build; cd build;
      $ cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/Toolchain/linux-gcc-ampi.cmake
      -DENABLE_OPENMP=OFF
      $ make

   Run with:

   .. code-block:: bash

      $ ./charmrun ++n 8 ./src/tools/kripke +vp8 --zones 64,64,64 --procs 2,2,2 --nest ZDG

MCB v1.0.3 (2013)
~~~~~~~~~~~~~~~~~

-  LLNL ASC proxy app for Monte Carlo particle transport codes:
   https://codesign.llnl.gov/mcb.php

-  MPI+OpenMP reference version.

-  Run with:

   .. code-block:: bash

      $ OMP_NUM_THREADS=1 ./charmrun ++n 4 ./../src/MCBenchmark.exe --weakScaling
       --distributedSource --nCores=1 --numParticles=20000 --multiSigma --nThreadCore=1 +vp16

.. _not-yet-ampi-zed-reason-1:

Not yet AMPI-zed (reason)
~~~~~~~~~~~~~~~~~~~~~~~~~

: UMT 2013 (global variables).

Other Applications
------------------

MILC 7.0
~~~~~~~~

-  MILC is a code to study quantum chromodynamics (QCD) physics.
   http://www.nersc.gov/users/computational-systems/cori/nersc-8-procurement/trinity-nersc-8-rfp/nersc-8-trinity-benchmarks/milc/

-  Moved ``MPI_Init_thread`` call to ``main()``, added ``__thread`` to
   all global/static variable declarations. Runs on AMPI with
   virtualization when using -tlsglobals.

-  Build: edit ``ks_imp_ds/Makefile`` to use AMPI compiler wrappers, run
   ``make su3_rmd`` in ``ks_imp_ds/``

-  Run with: ``./su3_rmd +vp8 ../benchmark_n8/single_node/n8_single.in``

SNAP v1.01 (C version)
~~~~~~~~~~~~~~~~~~~~~~

-  LANL proxy app for PARTISN, an Sn deterministic particle transport
   application: https://github.com/losalamos/SNAP

-  SNAP is an update to Sweep3D. It simulates the same thing as Kripke,
   but with a different decomposition and slight algorithmic
   differences. It uses a 1- or 2-dimensional decomposition and the KBA
   algorithm to perform parallel sweeps over the 3-dimensional problem
   space. It contains all of the memory, computation, and network
   performance characteristics of a real particle transport code.

-  Original SNAP code is Fortran90-MPI-OpenMP, but this is a
   C-MPI-OpenMP version of it provided along with the original version.
   The Fortran90 version will require global variable privatization,
   while the C version works out of the box on all platforms.

-  Edit the Makefile for AMPI compiler paths and run with:
   ``./charmrun ++n 4 ./snap +vp4 --fi center_src/fin01 --fo center_src/fout01``

Sweep3D
~~~~~~~

-  Sweep3D is a *particle transport* program that analyzes the flux of
   particles along a space. It solves a three-dimensional particle
   transport problem.

-  This mini-app has been deprecated, and replaced at LANL by SNAP
   (above).

-  Build/Run Instructions:

   -  Modify the ``makefile`` and change variable CHARMC to point to
      your Charm++ compiler command, execute ``make mpi`` to build the
      program.

   -  Modify file ``input`` to set the different parameters. Refer to
      file ``README`` on how to change those parameters. Run with:
      ``./charmrun ./sweep3d.mpi ++n 8 +vp16``

PENNANT v0.8
~~~~~~~~~~~~

-  Unstructured mesh Rad-Hydro mini-app for a full application at LANL
   called FLAG. https://github.com/losalamos/PENNANT

-  Written in C++, only global/static variables that need to be
   privatized are mype and numpe. Done manually.

-  Legion, Regent, MPI, MPI+OpenMP, MPI+CUDA versions of PENNANT exist
   for comparison.

-  For PENNANT-v0.8, point CC in Makefile to AMPICC and just ’make’. Run
   with the provided input files, such as:
   ``./charmrun ++n 2 ./build/pennant +vp8 test/noh/noh.pnt``

Benchmarks
----------

Jacobi-2D (Fortran)
~~~~~~~~~~~~~~~~~~~

-  Jacobi-2D with 1D decomposition. Problem size and number of
   iterations are defined in the source code. Manually privatized.

Jacobi-3D (C)
~~~~~~~~~~~~~

-  Jacobi-3D with 3D decomposition. Manually privatized. Includes
   multiple versions: Isomalloc, PUP, FT, LB, Isend/Irecv, Iput/Iget.

NAS Parallel Benchmarks (NPB 3.3)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

-  A collection of kernels used in different scientific applications.
   They are mainly implementations of various linear algebra methods.
   http://www.nas.nasa.gov/Resources/Software/npb.html

-  Build/Run Instructions:

   -  Modify file ``config/make.def`` to make variable ``CHAMRDIR``
      point to the right Charm++ directory.

   -  Use ``make <benchmark> NPROCS=<P> CLASS=<C>`` to build a
      particular benchmark. The values for ``<benchmark>`` are (bt, cg,
      dt, ep, ft, is, lu, mg, sp), ``<P>`` is the number of ranks and
      ``<C>`` is the class or the problem size (to be chosen from
      A,B,C,D or E). Some benchmarks may have restrictions on values of
      ``<P>`` and ``<C>``. For instance, to make CG benchmark with 256
      ranks and class C, we will use the following command:
      ``make cg NPROCS=256``

   -  The resulting executable file will be generated in the respective
      directory for the benchmark. In the previous example, a file
      *cg.256.C* will appear in the *CG* and ``bin/`` directories. To
      run the particular benchmark, you must follow the standard
      procedure of running AMPI programs:
      ``./charmrun ./cg.C.256 ++n 64 +vp256 ++nodelist nodelist``

NAS PB Multi-Zone Version (NPB-MZ 3.3)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

-  A multi-zone version of BT, SP and LU NPB benchmarks. The multi-zone
   intentionally divides the space unevenly among ranks and causes load
   imbalance. The original goal of multi-zone versions was to offer an
   test case for hybrid MPI+OpenMP programming, where the load imbalance
   can be dealt with by increasing the number of threads in those ranks
   with more computation.
   http://www.nas.nasa.gov/Resources/Software/npb.html

-  The BT-MZ program shows the heaviest load imbalance.

-  Build/Run Instructions:

   -  Modify file ``config/make.def`` to make variable ``CHAMRDIR``
      point to the right Charm++ build.

   -  Use the format ``make <benchmark> NPROCS=<P> CLASS=<C>`` to build
      a particular benchmark. The values for ``<benchmark>`` are (bt-mz,
      lu-mz, sp-mz), ``<P>`` is the number of ranks and ``<C>`` is the
      class or the problem size (to be chosen from A,B,C,D or E). Some
      benchmarks may have restrictions on values of ``<P>`` and ``<C>``.
      For instance, to make the BT-MZ benchmark with 256 ranks and class
      C, you can use the following command:
      ``make bt-mz NPROCS=256 CLASS=C``

   -  The resulting executable file will be generated in the *bin/*
      directory. In the previous example, a file *bt-mz.256.C* will be
      created in the ``bin`` directory. To run the particular benchmark,
      you must follow the standard procedure of running AMPI programs:
      ``./charmrun ./bt-mz.C.256 ++n 64 +vp256 ++nodelist nodelist``

HPCG v3.0
~~~~~~~~~

-  High Performance Conjugate Gradient benchmark, version 3.0. Companion
   metric to Linpack, with many vendor-optimized implementations
   available: http://hpcg-benchmark.org/

-  No AMPI-ization needed. To build, modify ``setup/Make.AMPI`` for
   compiler paths, do
   ``mkdir build && cd build && configure ../setup/Make.AMPI && make``.
   To run, do ``./charmrun ++n 16 ./bin/xhpcg +vp64``

Intel Parallel Research Kernels (PRK) v2.16
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

-  A variety of kernels (Branch, DGEMM, Nstream, Random, Reduce, Sparse,
   Stencil, Synch_global, Synch_p2p, and Transpose) implemented for a
   variety of runtimes (SERIAL, OpenMP, MPI-1, MPI-RMA, MPI-SHM,
   MPI+OpenMP, SHMEM, FG_MPI, UPC, Grappa, Charm++, and AMPI).
   https://github.com/ParRes/Kernels

-  For AMPI tests, set ``CHARMTOP`` and run: ``make allampi``. There are
   run scripts included.

OSU Microbenchmarks
~~~~~~~~~~~~~~~~~~~

MPI collectives performance testing suite.
https://charm.cs.illinois.edu/gerrit/#/admin/projects/benchmarks/osu-collectives-benchmarking

-  Build with: ``./configure CC=~/charm/bin/ampicc && make``

Third Party Open Source Libraries
---------------------------------

HYPRE-2.11.1
~~~~~~~~~~~~

-  High Performance Preconditioners and solvers library from LLNL.
   https://computation.llnl.gov/project/linear_solvers/software.php

-  Hypre-2.11.1 builds on top of AMPI using the configure command:

   .. code-block:: bash

      $ ./configure --with-MPI \
            CC=~/charm/bin/ampicc \
            CXX=~/charm/bin/ampicxx \
            F77=~/charm/bin/ampif77 \
            --with-MPI-include=~/charm/include \
            --with-MPI-lib-dirs=~/charm/lib \
            --with-MPI-libs=mpi --without-timing --without-print-errors
      $ make -j8

-  All HYPRE tests and examples pass tests with virtualization,
   migration, etc. except for those that use Hypre’s timing interface,
   which uses a global variable internally. So just remove those calls
   and do not define ``HYPRE_TIMING`` when compiling a code that uses
   Hypre. In the examples directory, you’ll have to set the compilers to
   your AMPI compilers explicitly too. In the test directory, you’ll
   have to edit the Makefile to 1) Remove ``-DHYPRE_TIMING`` from both
   ``CDEFS`` and ``CXXDEFS``, 2) Remove both ``${MPILIBS}`` and
   ``${MPIFLAGS}`` from ``MPILIBFLAGS``, and 3) Remove ``${LIBS}`` from
   ``LIBFLAGS``. Then run ``make``.

-  To run the ``new_ij`` test, run:
   ``./charmrun ++n 64 ./new_ij -n 128 128 128 -P 4 4 4 -intertype 6 -tol 1e-8 -CF 0 -solver 61 -agg_nl 1 27pt -Pmx 6 -ns 4 -mu 1 -hmis -rlx 13 +vp64``

MFEM-3.2
~~~~~~~~

-  MFEM is a scalable library for Finite Element Methods developed at
   LLNL. http://mfem.org/

-  MFEM-3.2 builds on top of AMPI (and METIS-4.0.3 and HYPRE-2.11.1).
   Download MFEM,
   `HYPRE <https://computation.llnl.gov/project/linear_solvers/software.php>`__,
   and `METIS <http://glaros.dtc.umn.edu/gkhome/fsroot/sw/metis/OLD>`__.
   Untar all 3 in the same top-level directory.

-  Build HYPRE-2.11.1 as described above.

-  Build METIS-4.0.3 by doing ``cd metis-4.0.3/ && make``

-  Build MFEM-3.2 serial first by doing ``make serial``

-  Build MFEM-3.2 parallel by doing:

   -  First, comment out ``#define HYPRE_TIMING`` in
      ``mfem/linalg/hypre.hpp``. Also, you must add a
      ``#define hypre_clearTiming()`` at the top of
      ``linalg/hypre.cpp``, because Hypre-2.11.1 has a bug where it
      doesn’t provide a definition of this function if you don’t define
      ``HYPRE_TIMING``.

   -  ``make parallel MFEM_USE_MPI=YES MPICXX=~/charm/bin/ampicxx HYPRE_DIR=~/hypre-2.11.1/src/hypre METIS_DIR=~/metis-4.0.3``

-  To run an example, do
   ``./charmrun ++n 4 ./ex15p -m ../data/amr-quad.mesh +vp16``. You may
   want to add the runtime options ``-no-vis`` and ``-no-visit`` to
   speed things up.

-  All example programs and miniapps pass with virtualization, and
   migration if added.

XBraid-1.1
~~~~~~~~~~

-  XBraid is a scalable library for parallel time integration using
   MultiGrid, developed at LLNL.
   https://computation.llnl.gov/project/parallel-time-integration/software.php

-  XBraid-1.1 builds on top of AMPI (and its examples/drivers build on
   top of MFEM-3.2, HYPRE-2.11.1, and METIS-4.0.3 or METIS-5.1.0).

-  To build XBraid, modify the variables CC, MPICC, and MPICXX in
   makefile.inc to point to your AMPI compilers, then do ``make``.

-  To build XBraid’s examples/ and drivers/ modify the paths to MFEM and
   HYPRE in their Makefiles and ``make``.

-  To run an example, do
   ``./charmrun ++n 2 ./ex-02 -pgrid 1 1 8 -ml 15 -nt 128 -nx 33 33 -mi 100 +vp8 ++local``.

-  To run a driver, do
   ``./charmrun ++n 4 ./drive-03 -pgrid 2 2 2 2 -nl 32 32 32 -nt 16 -ml 15 +vp16 ++local``

Other AMPI codes
----------------

-  FLASH

-  BRAMS (Weather prediction model)

-  CGPOP

-  Fractography3D (Crack Propagation)

-  JetAlloc

-  PlasComCM (XPACC)

-  PlasCom2 (XPACC)

-  Harm3D
