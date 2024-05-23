Using Existing MPI Codes with AMPI
==================================

Due to the nature of AMPI's virtualized ranks, some changes to existing
MPI codes may be necessary for them to function correctly with AMPI.

Entry Point
-----------

To convert an existing program to use AMPI, the main function or program
may need to be renamed. The changes should be made as follows:

Fortran
~~~~~~~

You must declare the main program as a subroutine called "MPI_MAIN". Do
not declare the main subroutine as a *program* because it will never be
called by the AMPI runtime.

.. code-block:: fortran

   program pgm -> subroutine MPI_Main
       ...                       ...
   end program -> end subroutine

C or C++
~~~~~~~~

The main function can be left as is, if ``mpi.h`` is included before the
main function. This header file has a preprocessor macro that renames
main, and the renamed version is called by the AMPI runtime for each
rank.

Command Line Argument Parsing
-----------------------------

Fortran
~~~~~~~

For parsing Fortran command line arguments, AMPI Fortran programs should
use our extension APIs, which are similar to Fortran 2003’s standard
APIs. For example:

.. code-block:: fortran

   integer :: i, argc, ierr
   integer, parameter :: arg_len = 128
   character(len=arg_len), dimension(:), allocatable :: raw_arguments

   call AMPI_Command_argument_count(argc)
   allocate(raw_arguments(argc))
   do i = 1, size(raw_arguments)
       call AMPI_Get_command_argument(i, raw_arguments(i), arg_len, ierr)
   end do

C or C++
~~~~~~~~

Existing code for parsing ``argc`` and ``argv`` should be sufficient,
provided that it takes place *after* ``MPI_Init``.

Global Variable Privatization
-----------------------------

In AMPI, ranks are implemented as user-level threads that coexist
within OS processes or OS threads, depending on how the Charm++
runtime was built. Traditional MPI
programs assume that each rank has an entire OS process to itself,
and that only one thread of control exists within its address space.
This allows them to safely use global and static variables in their
code. However, global and static variables are problematic for
multi-threaded environments such as AMPI or OpenMP. This is because
there is a single instance of those variables, so they will be shared
among different ranks in the single address space, and this could lead
to the program producing an incorrect result or crashing.

The following code is an example of this problem. Each rank queries its
numeric ID, stores it in a global variable, waits on a global barrier,
and then prints the value that was stored. If this code is run with
multiple ranks virtualized inside one OS process, each rank will store
its ID in the same single location in memory. The result is that all
ranks will print the ID of whichever one was the last to successfully
update that location. For this code to be semantically valid with AMPI,
each rank needs its own separate instance of the variable. This is
where the need arises for some special handling of these unsafe
variables in existing MPI applications, which we call *privatization*.

.. code-block:: c++

  int rank_global;

  void print_ranks(void)
  {
    MPI_Comm_rank(MPI_COMM_WORLD, &rank_global);

    MPI_Barrier(MPI_COMM_WORLD);

    printf("rank: %d\n", rank_global);
  }

The basic transformation needed to port MPI programs to AMPI is
privatization of global and static variables. Module variables, "saved"
subroutine local variables, and common blocks in Fortran90 also belong to
this category. Certain API calls use global variables internally, such as
``strtok`` in the C standard library, and as a result they are also
unsafe. If such a program is executed without privatization on AMPI, all
the AMPI ranks that reside in the same process will access the same
copy of such variables, which is clearly not the desired semantics. Note
that global variables that are constant or are only written to once
during initialization with the same value across all ranks are already
thread-safe.

To ensure AMPI programs execute correctly, it is necessary to make such
variables "private" to individual ranks. We provide several options to
achieve this with varying degrees of portability and required developer
effort.

.. warning::

   If you are writing a new MPI application from scratch and would like
   to support AMPI as a first-class target, it is highly recommended to
   follow certain guidelines for writing your code to avoid the global
   variable problem entirely, eliminating the need for time-consuming
   refactoring or platform-specific privatization methods later on. See
   the Manual Code Editing section below for an example of how to
   structure your code in order to accomplish this.

Manual Code Editing
~~~~~~~~~~~~~~~~~~~

With regard to performance and portability, the ideal approach to resolve
the global variable problem is to refactor your code to avoid use of
globals entirely. However, this comes with the obvious caveat that it
requires developer time to implement and can involve invasive changes
across the entire codebase, similar to converting a shared library to be
reentrant in order to allow multiple instantiations from the same OS
process. If these costs are a significant barrier to entry, it can be
helpful to instead explore one of the simpler transformations or fully
automated methods described below.

We have employed a strategy of argument passing to do this privatization
transformation. That is, the global variables are bunched together in a
single user-defined type, which is allocated by each thread dynamically
or on the stack. Then a pointer to this type is passed from subroutine
to subroutine as an argument. Since the subroutine arguments are passed
on the stack, which is not shared across all threads, each subroutine
when executing within a thread operates on a private copy of the global
variables.

This scheme is demonstrated in the following examples. The original
Fortran90 code contains a module ``shareddata``. This module is used in
the ``MPI_MAIN`` subroutine and a subroutine ``subA``. Note that
``PROGRAM PGM`` was renamed to ``SUBROUTINE MPI_MAIN`` and ``END PROGRAM``
was renamed to ``END SUBROUTINE``.

.. code-block:: fortran

   !FORTRAN EXAMPLE
   MODULE shareddata
     INTEGER :: myrank
     DOUBLE PRECISION :: xyz(100)
   END MODULE

   SUBROUTINE MPI_MAIN                               ! Previously PROGRAM PGM
     USE shareddata
     include 'mpif.h'
     INTEGER :: i, ierr
     CALL MPI_Init(ierr)
     CALL MPI_Comm_rank(MPI_COMM_WORLD, myrank, ierr)
     DO i = 1, 100
       xyz(i) =  i + myrank
     END DO
     CALL subA
     CALL MPI_Finalize(ierr)
   END SUBROUTINE                                    ! Previously END PROGRAM

   SUBROUTINE subA
     USE shareddata
     INTEGER :: i
     DO i = 1, 100
       xyz(i) = xyz(i) + 1.0
     END DO
   END SUBROUTINE

.. code-block:: c++

   //C Example
   #include <mpi.h>

   int myrank;
   double xyz[100];

   void subA();
   int main(int argc, char** argv){
     int i;
     MPI_Init(&argc, &argv);
     MPI_Comm_rank(MPI_COMM_WORLD, &myrank);
     for(i=0;i<100;i++)
       xyz[i] = i + myrank;
     subA();
     MPI_Finalize();
   }

   void subA(){
     int i;
     for(i=0;i<100;i++)
       xyz[i] = xyz[i] + 1.0;
   }

AMPI executes the main subroutine inside a user-level thread as a
subroutine.

Now we transform this program using the argument passing strategy. We
first group the shared data into a user-defined type.

.. code-block:: fortran

   !FORTRAN EXAMPLE
   MODULE shareddata
     TYPE chunk ! modified
       INTEGER :: myrank
       DOUBLE PRECISION :: xyz(100)
     END TYPE ! modified
   END MODULE

.. code-block:: c++

   //C Example
   struct shareddata{
     int myrank;
     double xyz[100];
   };

Now we modify the main subroutine to dynamically allocate this data and
change the references to them. Subroutine ``subA`` is then modified to
take this data as argument.

.. code-block:: fortran

   !FORTRAN EXAMPLE
   SUBROUTINE MPI_Main
     USE shareddata
     USE AMPI
     INTEGER :: i, ierr
     TYPE(chunk), pointer :: c ! modified
     CALL MPI_Init(ierr)
     ALLOCATE(c) ! modified
     CALL MPI_Comm_rank(MPI_COMM_WORLD, c%myrank, ierr)
     DO i = 1, 100
       c%xyz(i) =  i + c%myrank ! modified
     END DO
     CALL subA(c)
     CALL MPI_Finalize(ierr)
   END SUBROUTINE

   SUBROUTINE subA(c)
     USE shareddata
     TYPE(chunk) :: c ! modified
     INTEGER :: i
     DO i = 1, 100
       c%xyz(i) = c%xyz(i) + 1.0 ! modified
     END DO
   END SUBROUTINE

.. code-block:: c++

   //C Example
   void MPI_Main{
     int i,ierr;
     struct shareddata *c;
     ierr = MPI_Init();
     c = (struct shareddata*)malloc(sizeof(struct shareddata));
     ierr = MPI_Comm_rank(MPI_COMM_WORLD, c.myrank);
     for(i=0;i<100;i++)
       c.xyz[i] = i + c.myrank;
     subA(c);
     ierr = MPI_Finalize();
   }

   void subA(struct shareddata *c){
     int i;
     for(i=0;i<100;i++)
       c.xyz[i] = c.xyz[i] + 1.0;
   }

With these changes, the above program can be made thread-safe. Note that
it is not really necessary to dynamically allocate ``chunk``. One could
have declared it as a local variable in subroutine ``MPI_Main``. (Or for
a small example such as this, one could have just removed the
``shareddata`` module, and instead declared both variables ``xyz`` and
``myrank`` as local variables). This is indeed a good idea if shared
data are small in size. For large shared data, it would be better to do
heap allocation because in AMPI, the stack sizes are fixed at the
beginning (and can be specified from the command line) and stacks do not
grow dynamically.

PIEglobals: Automatic Position-Independent Executable Runtime Relocation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Position-Independent Executable (PIE) Globals allows fully automatic
privatization of global variables on GNU/Linux systems without
modification of user code. All languages (C, C++, Fortran, etc.) are
supported. Runtime migration, load balancing, checkpointing, and SMP
mode are all fully supported.

This method works by combining a specific method of building binaries
with GNU extensions to the dynamic linker. First, AMPI's toolchain
wrapper compiles your user program as a Position-Independent Executable
(PIE) and links it against a special shim of function pointers instead
of the normal AMPI runtime. It then builds a small loader utility that
links directly against AMPI. This loader dynamically opens the PIE
binary after the AMPI runtime is fully initialized. The glibc
extension ``dl_iterate_phdr`` is called before and after the ``dlopen``
call in order to determine the location of the PIE binary's code and
data segments in memory. This is useful because PIE binaries locate the
data segment containing global variables immediately after the code
segment so that they are accessed relative to the instruction pointer.
The PIEglobals loader makes a copy of the code and data segments for
each AMPI rank in the job via the Isomalloc allocator, thereby
privatizing their global state. It then constructs a synthetic function
pointer for each rank at its new locations and calls it.

To use PIEglobals in your AMPI program, compile and link with the
``-pieglobals`` parameter:

.. code-block:: bash

   $ ampicxx -o example.o -c example.cpp -pieglobals
   $ ampicxx -o example example.o -pieglobals

No further effort is needed. Global variables in ``example.cpp`` will be
automatically privatized when the program is run. Any libraries and
shared objects compiled as PIE will also be privatized. However, if
these objects call MPI functions, it will be necessary to build them
with the AMPI toolchain wrappers, ``-pieglobals``, and potentially also
the ``-standalone`` parameter in the case of shared objects. It is
recommended to do this in any case so that AMPI can ensure everything is
built as PIE.

One important caveat is that the relocated code segments are opaque to
runtime debuggers such as GDB and LLDB because debug symbols are not
translated to their new location in memory. For this reason it is
recommended to perform as much development and debugging as possible in
non-virtualized mode so the program can be debugged normally. One
faculty provided to assist in debugging with virtualization is the
``pieglobalsfind`` function. This can be called at runtime to translate
a privatized address back to its original location as allocated by the
system's runtime linker, thereby associating it with any debug symbols
included in the binary. In GDB, the command takes the form
``call pieglobalsfind((void *)0x...)``. It can be useful to directly
pass in the instruction pointer as an argument, such as
``call pieglobalsfind($rip)`` on x86_64.

TLSglobals: Automatic Thread-Local Storage Swapping
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Thread Local Store (TLS) was originally employed in kernel threads to
localize variables to threads and provide thread safety. It can be used
by annotating global/static variable declarations in C with
*thread_local*, in C with *__thread* or C11 with *thread_local* or
*_Thread_local*, and in Fortran with OpenMP’s *threadprivate*
attribute. OpenMP is required for using tlsglobals in Fortran code since
Fortran has no other method of using TLS. The *__thread* keyword is not
an official extension of the C language, though compiler writers are
encouraged to implement this feature.

It handles both global and static variables and has no context-switching
overhead. AMPI provides runtime support for privatizing thread-local
variables to user-level threads by changing the TLS segment register
when context switching between user-level threads. The runtime overhead
is that of changing a single pointer per user-level thread context
switch. Currently, Charm++ supports it for x86, x86_64, AArch64, and
POWER platforms when
using GNU or LLVM compilers, as well as macOS on all supported
architectures.

.. code-block:: c++

   // C/C++ example:
   int myrank;
   double xyz[100];

.. code-block:: fortran

   ! Fortran example:
   integer :: myrank
   real*8, dimension(100) :: xyz

For the example above, the following changes to the code handle the
global variables:

.. code-block:: c++

   // C++ example:
   thread_local int myrank;
   thread_local double xyz[100];

   // C example:
   __thread int myrank;
   __thread double xyz[100];

.. code-block:: fortran

   ! Fortran example:
   integer :: myrank
   real*8, dimension(100) :: xyz
   !$omp threadprivate(myrank)
   !$omp threadprivate(xyz)

The runtime system also should know that TLSglobals is used at both
compile and link time:

.. code-block:: bash

   $ ampicxx -o example example.C -tlsglobals

PiPglobals: Automatic Process-in-Process Runtime Linking Privatization
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Process-in-Process (PiP) [PiP2018]_ Globals allows fully automatic
privatization of global variables on GNU/Linux systems without
modification of user code. All languages (C, C++, Fortran, etc.) are
supported. This method currently lacks support for checkpointing and
migration, which are necessary for load balancing and fault tolerance.
Additionally, overdecomposition is limited to approximately 12 virtual
ranks per logical node, though this can be resolved by building a
patched version of glibc.

As with PIEglobals, this method compiles your user program as a
Position-Independent Executable (PIE) and links it against a special
shim of function pointers. A small loader utility calls the
glibc-specific function ``dlmopen`` on the PIE binary with a unique
namespace index. The loader uses ``dlsym`` to populate the PIE binary's
function pointers and then it calls the entry point. This ``dlmopen``
and ``dlsym`` process repeats for each rank. As soon as execution jumps
into the PIE binary, any global variables referenced within will appear
privatized. This is because PIE binaries locate the global data segment
immediately after the code segment so that PIE global variables are
accessed relative to the instruction pointer, and because ``dlmopen``
creates a separate copy of these segments in memory for each unique
namespace index.

Optionally, the first step in using PiPglobals is to build PiP-glibc to
overcome the limitation on rank count per process. Use the instructions
at https://github.com/RIKEN-SysSoft/PiP/blob/pip-1/INSTALL.md to download
an installable PiP package or build PiP-glibc from source by following
the ``Patched GLIBC`` section. AMPI may be able to automatically detect
PiP's location if installed as a package, but otherwise set and export
the environment variable ``PIP_GLIBC_INSTALL_DIR`` to the value of
``<GLIBC_INSTALL_DIR>`` as used in the above instructions. For example:

.. code-block:: bash

   $ export PIP_GLIBC_INSTALL_DIR=~/pip

To use PiPglobals in your AMPI program (with or without PiP-glibc),
compile and link with the ``-pipglobals`` parameter:

.. code-block:: bash

   $ ampicxx -o example.o -c example.cpp -pipglobals
   $ ampicxx -o example example.o -pipglobals

No further effort is needed. Global variables in ``example.cpp`` will be
automatically privatized when the program is run. Any libraries and
shared objects compiled as PIE will also be privatized. However, if
these objects call MPI functions, it will be necessary to build them
with the AMPI toolchain wrappers, ``-pipglobals``, and potentially also
the ``-standalone`` parameter in the case of shared objects. It is
recommended to do this in any case so that AMPI can ensure everything is
built as PIE.

Potential future support for checkpointing and migration will require
modification of the ``ld-linux.so`` runtime loader to intercept mmap
allocations of the previously mentioned segments and redirect them
through Isomalloc. The present lack of support for these features mean
PiPglobals is best suited for testing AMPI during exploratory phases
of development, and for production jobs not requiring load balancing or
fault tolerance.

FSglobals: Automatic Filesystem-Based Runtime Linking Privatization
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Filesystem Globals (FSglobals) was discovered during the development of
PiPglobals and the two are highly similar. Like PiPglobals, it
requires no modification of user code and works with any language.
It also currently lacks support for checkpointing and migration,
preventing use of load balancing and fault tolerance. Unlike PiPglobals,
it is portable beyond GNU/Linux and has no limits to overdecomposition
beyond available disk space.

FSglobals works in the same way as PiPglobals except that instead of
specifying namespaces using ``dlmopen``, which is a GNU/Linux-specific
feature, this method creates copies of the user's PIE binary on the
filesystem for each rank and calls the POSIX-standard ``dlopen``.

To use FSglobals, compile and link with the ``-fsglobals`` parameter:

.. code-block:: bash

   $ ampicxx -o example.o -c example.cpp -fsglobals
   $ ampicxx -o example example.o -fsglobals

No additional steps are required. Global variables in ``example.cpp``
will be automatically privatized when the program is run. Variables in
statically linked libraries will also be privatized if compiled as PIE.
It is recommended to achieve this by building with the AMPI toolchain
wrappers and ``-fsglobals``, and this is necessary if the libraries call
MPI functions. Shared objects are currently not supported by FSglobals
due to the extra overhead of iterating through all dependencies and
copying each one per rank while avoiding system components, plus the
complexity of ensuring each rank's program binary sees the proper set of
objects.

This method's use of the filesystem is a drawback in that it is slow
during startup and can be considered wasteful. Additionally, support for
load balancing and fault tolerance would require further development in
the future, using the same infrastructure as what PiPglobals would
require. For these reasons FSglobals is best suited for the R&D phase
of AMPI program development and for small jobs, and it may be less
suitable for large production environments.

Swapglobals: Automatic Global Offset Table Swapping
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Thanks to the ELF Object Format, we have successfully automated the
procedure of switching the set of user global variables when switching
thread contexts. Executable and Linkable Format (ELF) is a common
standard file format for Object Files in Unix-like operating systems.
ELF maintains a Global Offset Table (GOT) for globals so it is possible
to switch GOT contents at thread context-switch by the runtime system.

The only thing that the user needs to do is pass the flag
``-swapglobals`` at both compile and link time (e.g. "ampicc -o prog
prog.c -swapglobals"). This method does not require any changes to the
source code and works with any language (C, C++, Fortran, etc). However,
it does not handle static variables, has a context switching overhead
that grows with the number of global variables, and is incompatible with
SMP builds of AMPI, where multiple virtual ranks can execute
simultaneously on different scheduler threads within an OS process.

Currently, this feature only works on x86 and x86_64 platforms that
fully support ELF, and it requires ld version 2.23 or older, or else a
patched version of ld 2.24+ that we provide here:
https://charm.cs.illinois.edu/gerrit/gitweb?p=libbfd-patches.git;a=tree;f=swapglobals

For these reasons, and because more robust privatization methods are
available, swapglobals is considered deprecated.

Source-to-Source Transformation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One final approach is to use a tool to transform your program's source
code, implementing the changes described in one of the sections above in
an automated fashion.

We have multiple tools for automating these transformations for different
languages.
Currently, there is a tool called *Photran*
(http://www.eclipse.org/photran) for refactoring Fortran codes
that can do this transformation. It is Eclipse-based and works by
constructing Abstract Syntax Trees (ASTs) of the program.
We also have a tool built with *LLVM/LibTooling* that applies the
TLSglobals transformation to C/C++ codes, available upon request.

Summary
~~~~~~~

Table :numref:`tab:portability` shows portability of
different schemes.

.. _tab:portability:
.. table:: Portability of current implementations of three privatization schemes. "Yes" means we have implemented this technique. "Maybe" indicates there are no theoretical problems, but no implementation exists. "No" indicates the technique is impossible on this platform.

   ==================== ===== ====== ======= === ====== ===== ===== =======
   Privatization Scheme Linux Mac OS Windows x86 x86_64 POWER ARMv7 AArch64
   ==================== ===== ====== ======= === ====== ===== ===== =======
   Manual Code Editing  Yes   Yes    Yes     Yes Yes    Yes   Yes   Yes
   PIEglobals           Yes   No     No      Yes Yes    Yes   Yes   Yes
   TLSglobals           Yes   Yes    Maybe   Yes Yes    Yes   No    Yes
   PiPglobals           Yes   No     No      Yes Yes    Yes   Yes   Yes
   FSglobals            Yes   Yes    Yes     Yes Yes    Yes   Yes   Yes
   Swapglobals          Yes   No     No      Yes Yes    Yes   Yes   Yes
   ==================== ===== ====== ======= === ====== ===== ===== =======
