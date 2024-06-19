Extensions
==========

The following are AMPI extensions to the MPI standard, which will be
explained in detail in this manual. All AMPI extensions to the MPI
standard are prefixed with ``AMPI_`` rather than ``MPI_``. All
extensions are available in C, C++, and Fortran, with the exception of
``AMPI_Command_argument_count`` and ``AMPI_Get_command_argument`` which
are only available in Fortran.

.. code-block:: none

   AMPI_Migrate          AMPI_Register_pup            AMPI_Get_pup_data
   AMPI_Migrate_to_pe    AMPI_Set_migratable
   AMPI_Load_set_value   AMPI_Load_start_measure      AMPI_Load_stop_measure
   AMPI_Iget             AMPI_Iget_wait               AMPI_Iget_data
   AMPI_Iget_free        AMPI_Type_is_contiguous
   AMPI_Yield            AMPI_Suspend                 AMPI_Resume
   AMPI_Alltoall_medium  AMPI_Alltoall_long
   AMPI_Register_just_migrated         AMPI_Register_about_to_migrate
   AMPI_Command_argument_count         AMPI_Get_command_argument

Serialization
-------------

Some of AMPI's primary benefits are made possible by the ability to pack
and unpack the entire state of a program and transmit it over the network
or write a snapshot of it to the filesystem.

In the vast majority of cases, this serialization is fully automated
using a custom memory allocator, Isomalloc, which returns virtual memory
addresses that are globally unique across an entire job. This
means that every worker thread in the system reserves slices of virtual
memory for all user-level threads, allowing transparent migration of
stacks and pointers into memory. (Isomalloc requires 64-bit virtual
memory addresses and support from the operating system for mapping
memory to arbitrary virtual addresses.) Applications built with AMPI's
toolchain wrappers are automatically linked with Isomalloc as the active
``malloc`` implementation if the target platform supports the feature.

For systems that do not support Isomalloc and for users that wish to
have more fine-grain control over which application data structures will
be copied at migration time, we have added a few calls to AMPI. These
include the ability to register thread-specific data with the run-time
system, and the means to pack and unpack all of the thread’s data. This
mode of operation requires passing ``-memory default`` at link time to
disable Isomalloc's heap interception.

.. warning::

   Most users may skip this section unless you have specific needs.

AMPI packs up any data internal to the runtime in use by the rank,
including the thread’s stack. This means that the local variables
declared in subroutines in a rank, which are created on the stack, are
automatically packed by the runtime system. However, without Isomalloc,
the runtime has no way of knowing what other data are in use by the
rank. Thus upon starting execution, a rank needs to notify the system
about the data that it is going to use (apart from local variables).
Even with the data registration, AMPI cannot determine what size the
data is, or whether the registered data contains pointers to other
places in memory. For this purpose, a packing subroutine also needs to
be provided to the AMPI runtime system along with registered data.
The call provided by AMPI
for doing this is ``AMPI_Register_pup``. This function takes three
arguments: a data item to be transported along with the rank, the pack
subroutine, and a pointer to an integer which denotes the registration
identifier. In C/C++ programs, it may be necessary to use this integer
value after migration completes and control returns to the rank with the
function ``AMPI_Get_pup_data``.

Once the AMPI runtime system decides which ranks to send to which
processors, it calls the specified pack subroutine for that rank, with
the rank-specific data that was registered with the system using
``AMPI_Register_pup``. If an AMPI application uses Isomalloc, then the
system will define the Pack/Unpack routines for the user. This section
explains how a subroutine should be written for performing explicit
pack/unpack.

There are three steps for transporting the rank’s data to another
processor. First, the system calls a subroutine to get the size of the
buffer required to pack the rank’s data. This is called the "sizing"
step. In the next step, which is called immediately afterward on the
source processor, the system allocates the required buffer and calls the
subroutine to pack the rank’s data into that buffer. This is called the
"packing" step. This packed data is then sent as a message to the
destination processor, where first a rank is created (along with the
thread) and a subroutine is called to unpack the rank’s data from the
buffer. This is called the "unpacking" step.

Though the above description mentions three subroutines called by the
AMPI runtime system, it is possible to actually write a single
subroutine that will perform all the three tasks. This is achieved using
something we call a "pupper". A pupper is an external subroutine that is
passed to the rank’s pack-unpack-sizing subroutine, and this subroutine,
when called in different phases performs different tasks. An example
will make this clear:

Suppose the user data, chunk, is defined as a derived type in Fortran90:

.. code-block:: fortran

   !FORTRAN EXAMPLE
   MODULE chunkmod
     INTEGER, parameter :: nx=4, ny=4, tchunks=16
     TYPE, PUBLIC :: chunk
         REAL(KIND=8) t(22,22)
         INTEGER xidx, yidx
         REAL(KIND=8), dimension(400):: bxm, bxp, bym, byp
     END TYPE chunk
   END MODULE

.. code-block:: c++

   //C Example
   struct chunk{
     double t;
     int xidx, yidx;
     double bxm,bxp,bym,byp;
   };

Then the pack-unpack subroutine ``chunkpup`` for this chunk module is
written as:

.. code-block:: fortran

   !FORTRAN EXAMPLE
   SUBROUTINE chunkpup(p, c)
     USE pupmod
     USE chunkmod
     IMPLICIT NONE
     INTEGER :: p
     TYPE(chunk) :: c

     call pup(p, c%t)
     call pup(p, c%xidx)
     call pup(p, c%yidx)
     call pup(p, c%bxm)
     call pup(p, c%bxp)
     call pup(p, c%bym)
     call pup(p, c%byp)
   end subroutine

.. code-block:: c++

   //C Example
   void chunkpup(pup_er p, struct chunk c){
     pup_double(p,c.t);
     pup_int(p,c.xidx);
     pup_int(p,c.yidx);
     pup_double(p,c.bxm);
     pup_double(p,c.bxp);
     pup_double(p,c.bym);
     pup_double(p,c.byp);
   }

There are several things to note in this example. First, the same
subroutine ``pup`` (declared in module ``pupmod``) is called to
size/pack/unpack any type of data. This is possible because of procedure
overloading possible in Fortran90. Second is the integer argument ``p``.
It is this argument that specifies whether this invocation of subroutine
``chunkpup`` is sizing, packing or unpacking. Third, the integer
parameters declared in the type ``chunk`` need not be packed or unpacked
since they are guaranteed to be constants and thus available on any
processor.

A few other functions are provided in module ``pupmod``. These functions
provide more control over the packing/unpacking process. Suppose one
modifies the ``chunk`` type to include allocatable data or pointers that
are allocated dynamically at runtime. In this case, when chunk is
packed, these allocated data structures should be deallocated after
copying them to buffers, and when chunk is unpacked, these data
structures should be allocated before copying them from the buffers. For
this purpose, one needs to know whether the invocation of ``chunkpup``
is a packing one or unpacking one. For this purpose, the ``pupmod``
module provides functions ``fpup_isdeleting``\ (``fpup_isunpacking``).
These functions return logical value ``.TRUE.`` if the invocation is for
packing (unpacking), and ``.FALSE.`` otherwise. The following example
demonstrates this:

Suppose the type ``dchunk`` is declared as:

.. code-block:: fortran

   !FORTRAN EXAMPLE
   MODULE dchunkmod
     TYPE, PUBLIC :: dchunk
         INTEGER :: asize
         REAL(KIND=8), pointer :: xarr(:), yarr(:)
     END TYPE dchunk
   END MODULE

.. code-block:: c++

   //C Example
   struct dchunk{
     int asize;
     double* xarr, *yarr;
   };

Then the pack-unpack subroutine is written as:

.. code-block:: fortran

   !FORTRAN EXAMPLE
   SUBROUTINE dchunkpup(p, c)
     USE pupmod
     USE dchunkmod
     IMPLICIT NONE
     INTEGER :: p
     TYPE(dchunk) :: c

     pup(p, c%asize)

     IF (fpup_isunpacking(p)) THEN       !! if invocation is for unpacking
       allocate(c%xarr(c%asize))
       ALLOCATE(c%yarr(c%asize))
     ENDIF

     pup(p, c%xarr)
     pup(p, c%yarr)

     IF (fpup_isdeleting(p)) THEN        !! if invocation is for packing
       DEALLOCATE(c%xarr)
       DEALLOCATE(c%yarr)
     ENDIF


   END SUBROUTINE

.. code-block:: c++

   //C Example
   void dchunkpup(pup_er p, struct dchunk c){
     pup_int(p,c.asize);
     if(pup_isUnpacking(p)){
       c.xarr = (double *)malloc(sizeof(double)*c.asize);
       c.yarr = (double *)malloc(sizeof(double)*c.asize);
     }
     pup_doubles(p,c.xarr,c.asize);
     pup_doubles(p,c.yarr,c.asize);
     if(pup_isPacking(p)){
       free(c.xarr);
       free(c.yarr);
     }
   }

One more function ``fpup_issizing`` is also available in module
``pupmod`` that returns ``.TRUE.`` when the invocation is a sizing one.
In practice one almost never needs to use it.

Charm++ also provides higher-level PUP routines for C++ STL data
structures and Fortran90 data types. The STL PUP routines will deduce
the size of the structure automatically, so that the size of the data
does not have to be passed in to the PUP routine. This facilitates
writing PUP routines for large pre-existing codebases. To use it, simply
include pup_stl.h in the user code. For modern Fortran with pointers and
allocatable data types, AMPI provides a similarly automated PUP
interface called apup. User code can include pupmod and then call apup()
on any array (pointer or allocatable, multi-dimensional) of built-in
types (character, short, int, long, real, double, complex, double
complex, logical) and the runtime will deduce the size and shape of the
array, including unassociated and NULL pointers. Here is the dchunk
example from earlier, written to use the apup interface:

.. code-block:: fortran

   !FORTRAN EXAMPLE
   SUBROUTINE dchunkpup(p, c)
     USE pupmod
     USE dchunkmod
     IMPLICIT NONE
     INTEGER :: p
     TYPE(dchunk) :: c

     !! no need for asize
     !! no isunpacking allocation necessary

     apup(p, c%xarr)
     apup(p, c%yarr)

     !! no isdeleting deallocation necessary

   END SUBROUTINE

Calling ``MPI_`` routines or accessing global variables that have been
privatized by use of tlsglobals or swapglobals from inside a user PUP
routine is currently not allowed in AMPI. Users can store MPI-related
information like communicator rank and size in data structures to be be
packed and unpacked before they are needed inside a PUP routine.

Load Balancing and Migration
----------------------------

AMPI provides support for migrating MPI ranks between nodes of a system.
If the AMPI runtime system is prompted to examine the distribution of
work throughout the job and decides that load imbalance exists within
the application, it will invoke one of its internal load balancing
strategies, which determines the new mapping of AMPI ranks so as to
balance the load. Then the AMPI runtime serializes the rank’s state as
described above and moves it to its new home processor.

AMPI provides a subroutine ``AMPI_Migrate(MPI_Info hints);`` for this
purpose. Each rank periodically calls ``AMPI_Migrate``. Typical CSE
applications are iterative and perform multiple time-steps. One should
call ``AMPI_Migrate`` in each rank at the end of some fixed number of
timesteps. The frequency of ``AMPI_Migrate`` should be determined by a
tradeoff between conflicting factors such as the load balancing
overhead, and performance degradation caused by load imbalance. In some
other applications, where application suspects that load imbalance may
have occurred, as in the case of adaptive mesh refinement; it would be
more effective if it performs a couple of timesteps before telling the
system to re-map ranks. This will give the AMPI runtime system some time
to collect the new load and communication statistics upon which it bases
its migration decisions. Note that ``AMPI_Migrate`` does NOT tell the
system to migrate the rank, but merely tells the system to check the
load balance after all the ranks call ``AMPI_Migrate``. To migrate the
rank or not is decided only by the system’s load balancing strategy.

The AMPI runtime system could detect load imbalance by itself and invoke
the load balancing strategy. However, if the application code is
going to pack/unpack the rank’s data, writing the pack subroutine will
be complicated if migrations occur at a stage unknown to the
application. For example, if the system decides to migrate a rank while
it is in initialization stage (say, reading input files), application
code will have to keep track of how much data it has read, what files
are open etc. Typically, since initialization occurs only once in the
beginning, load imbalance at that stage would not matter much.
Therefore, we want the demand to perform a load balance check to be
initiated by the application.

Essentially, a call to ``AMPI_Migrate`` signifies to the runtime system
that the application has reached a point at which it is safe to
serialize the local state. Knowing this, the runtime system can act in
several ways.

The MPI_Info object taken as a parameter by ``AMPI_Migrate`` gives users
a way to influence the runtime system’s decision-making and behavior.
AMPI provides two built-in MPI_Info objects for this, called
``AMPI_INFO_LB_SYNC`` and ``AMPI_INFO_LB_ASYNC``. Synchronous load
balancing assumes that the application is already at a synchronization
point. Asynchronous load balancing does not assume this.

Calling ``AMPI_Migrate`` on a rank with pending send requests (i.e. from
MPI_Isend) is currently not supported, therefore users should always
wait on any outstanding send requests before calling ``AMPI_Migrate``.

.. code-block:: c++

   // Main time-stepping loop
   for (int iter=0; iter < max_iters; iter++) {

     // Time step work ...

     if (iter % lb_freq == 0)
       AMPI_Migrate(AMPI_INFO_LB_SYNC);
   }

``AMPI_Migrate_to_pe`` migrates the calling rank to the specified PE. 
``AMPI_Migrate`` is preferred to users calling ``AMPI_Migrate_to_pe`` directly,
because ``AMPI_Migrate_to_pe`` requires Charm++ support for anytime migration.
Anytime migration requires the runtime system to buffer Charm++ broadcasts,
which has a memory overhead. Consequently, users are required to run with
``+ampiEnableMigrateToPe`` in order to call this extension routine.

Note that migrating ranks around the cores and nodes of a system can
change which ranks share physical resources, such as memory. A
consequence of this is that communicators created via
``MPI_Comm_split_type`` are invalidated by calls to ``AMPI_Migrate``
that result in migration which breaks the semantics of that communicator
type. The only valid routine to call on such communicators is
``MPI_Comm_free``.

We also provide callbacks that user code can register with the runtime
system to be invoked just before and right after migration:
``AMPI_Register_about_to_migrate`` and ``AMPI_Register_just_migrated``
respectively. Note that the callbacks are only invoked on those ranks
that are about to actually migrate or have just actually migrated.

AMPI provide routines for starting and stopping load measurements, and
for users to explicitly set the load value of a rank using the
following: ``AMPI_Load_start_measure``, ``AMPI_Load_stop_measure``,
``AMPI_Load_reset_measure``, and ``AMPI_Load_set_value``. And since AMPI
builds on top of Charm++, users can experiment with the suite of load
balancing strategies included with Charm++, as well as write their own
strategies based on user-level information and heuristics.

Checkpointing and Fault Tolerance
---------------------------------

Using the same serialization functionality as AMPI's migration support,
it is also possible to save the state of the program to disk, so that if
the program were to crash abruptly, or if the allocated time for the
program expires before completing execution, the program can be
restarted from the previously checkpointed state.

To perform a checkpoint in an AMPI program, all you have to do is make a
call to ``int AMPI_Migrate(MPI_Info hints)`` with an ``MPI_Info`` object
that specifies how you would like to checkpoint. Checkpointing can be
thought of as migrating AMPI ranks to storage. Users set the
checkpointing policy on an ``MPI_Info`` object’s ``"ampi_checkpoint"``
key to one of the following values: ``"to_file=directory_name"`` or
``"false"``. To perform checkpointing in memory a built-in MPI_Info
object called ``AMPI_INFO_CHKPT_IN_MEMORY`` is provided.

Checkpointing to file tells the runtime system to save checkpoints in a
given directory. (Typically, in an iterative program, the iteration
number, converted to a character string, can serve as a checkpoint
directory name.) This directory is created, and the entire state of the
program is checkpointed to this directory. One can restart the program
from the checkpointed state (using the same, more, or fewer physical
processors than were checkpointed with) by specifying
``"+restart directory_name"`` on the command-line.

Checkpointing in memory allows applications to transparently tolerate
failures online. The checkpointing scheme used here is a double
in-memory checkpoint, in which virtual processors exchange checkpoints
pairwise across nodes in each other’s memory such that if one node
fails, that failed node’s AMPI ranks can be restarted by its buddy once
the failure is detected by the runtime system. As long as no two buddy
nodes fail in the same checkpointing interval, the system can restart
online without intervention from the user (provided the job scheduler
does not revoke its allocation). Any load imbalance resulting from the
restart can then be managed by the runtime system. Use of this scheme is
illustrated in the code snippet below.

.. code-block:: c++

   // Main time-stepping loop
   for (int iter=0; iter < max_iters; iter++) {

     // Time step work ...

     if (iter % chkpt_freq == 0)
       AMPI_Migrate(AMPI_INFO_CHKPT_IN_MEMORY);
   }

A value of ``"false"`` results in no checkpoint being done that step.
Note that ``AMPI_Migrate`` is a collective function, meaning every
virtual processor in the program needs to call this subroutine with the
same MPI_Info object. The checkpointing capabilities of AMPI are powered
by the Charm++ runtime system. For more information about
checkpoint/restart mechanisms please refer to the Charm++
manual: :numref:`sec:checkpoint`.

Memory Efficiency
-----------------

MPI functions usually require the user to preallocate the data buffers
needed before the functions being called. For unblocking communication
primitives, sometimes the user would like to do lazy memory allocation
until the data actually arrives, which gives the opportunities to write
more memory efficient programs. We provide a set of AMPI functions as an
extension to the standard MPI-2 one-sided calls, where we provide a
split phase ``MPI_Get`` called ``AMPI_Iget``. ``AMPI_Iget`` preserves
the similar semantics as ``MPI_Get`` except that no user buffer is
provided to hold incoming data. ``AMPI_Iget_wait`` will block until the
requested data arrives and runtime system takes care to allocate space,
do appropriate unpacking based on data type, and return.
``AMPI_Iget_free`` lets the runtime system free the resources being used
for this get request including the data buffer. Finally,
``AMPI_Iget_data`` is the routine used to access the data.

.. code-block:: c++


   int AMPI_Iget(MPI_Aint orgdisp, int orgcnt, MPI_Datatype orgtype, int rank,
                 MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Win win,
                 MPI_Request *request);

   int AMPI_Iget_wait(MPI_Request *request, MPI_Status *status, MPI_Win win);

   int AMPI_Iget_free(MPI_Request *request, MPI_Status *status, MPI_Win win);

   int AMPI_Iget_data(void *data, MPI_Status status);

Compute Resource Awareness
--------------------------

AMPI provides a set of built-in attributes on all communicators to find
the number of the worker thread or process that a rank is currently
running on, its home worker thread, as well as the total number of
worker threads and processes in the job. We define a worker thread to
be a thread on which one of more AMPI ranks are scheduled. We define a
process here as an operating system process, which may contain one or
more worker threads. The built-in attributes are listed in the following table:

+------------------------+-------------------------------------------------+
| Attribute              | Defintion                                       |
+========================+=================================================+
| ``AMPI_MY_WTH``        | Worker thread the rank is currently running on. |
+------------------------+-------------------------------------------------+
| ``AMPI_MY_PROCESS``    | OS process the rank is currently running on.    |
+------------------------+-------------------------------------------------+
| ``AMPI_NUM_WTHS``      | Number of worker threads in the application.    |
+------------------------+-------------------------------------------------+
| ``AMPI_NUM_PROCESSES`` | Number of OS processes in the application.      |
+------------------------+-------------------------------------------------+
| ``AMPI_MY_HOME_WTH``   | Home worker thread of the rank.                 |
+------------------------+-------------------------------------------------+

These attributes are accessible from any rank by calling
``MPI_Comm_get_attr``, such as:

.. code-block:: fortran

   ! Fortran:
   integer (kind=MPI_ADDRESS_KIND) :: my_wth_ptr
   integer :: my_wth, flag, ierr
   call MPI_Comm_get_attr(MPI_COMM_WORLD, AMPI_MY_WTH, my_wth_ptr, flag, ierr)
   my_wth = my_wth_ptr

.. code-block:: c++

   // C/C++:
   int * my_wth_ptr;
   int my_wth, flag;
   MPI_Comm_get_attr(MPI_COMM_WORLD, AMPI_MY_WTH, &my_wth_ptr, &flag);
   my_wth = *my_wth_ptr;

.. warning::

   The pointers retrieved for these attributes will become invalid after
   migration. Always copy their values into local variables if you need
   to access the old values after a migration.

AMPI also provides extra communicator types that users can pass to
``MPI_Comm_split_type``: ``AMPI_COMM_TYPE_HOST`` for splitting a
communicator into disjoint sets of ranks that share the same physical
host, ``AMPI_COMM_TYPE_PROCESS`` for splitting a communicator into
disjoint sets of ranks that share the same operating system process, and
``AMPI_COMM_TYPE_WTH``, for splitting a communicator into disjoint sets
of ranks that share the same worker thread.

Charm++ Interoperation
----------------------

There is preliminary support for interoperating AMPI programs with Charm++
programs. This allows users to launch an AMPI program with an arbitrary number
of virtual processes in the same executable as a Charm++ program that contains
arbitrary collections of chares, with both AMPI ranks and chares being co-scheduled
by the runtime system. We also provide an entry method ``void injectMsg(int n, char buf[n])``
for chares to communicate with AMPI ranks. An example program can be found in
``examples/charm++/AMPI-interop``.

Sequential Re-run of a Parallel Node
------------------------------------

In some scenarios, a sequential re-run of a parallel node is desired.
One example is instruction-level accurate architecture simulations, in
which case the user may wish to repeat the execution of a node in a
parallel run in the sequential simulator. AMPI provides support for such
needs by logging the change in the MPI environment on a certain
processors. To activate the feature, build AMPI module with variable
"AMPIMSGLOG" defined, like the following command in charm directory.
(Linking with zlib "-lz" might be required with this, for generating
compressed log file.)

.. code-block:: bash

   $ ./build AMPI netlrts-linux-x86_64 -DAMPIMSGLOG

The feature is used in two phases: writing (logging) the environment and
repeating the run. The first logging phase is invoked by a parallel run
of the AMPI program with some additional command line options.

.. code-block:: bash

   $ ./charmrun ./pgm ++n 4 +vp4 +msgLogWrite +msgLogRank 2 +msgLogFilename "msg2.log"

In the above example, a parallel run with 4 worker threads and 4 AMPI
ranks will be executed, and the changes in the MPI environment of worker
thread 2 (also rank 2, starting from 0) will get logged into diskfile
"msg2.log".

Unlike the first run, the re-run is a sequential program, so it is not
invoked by charmrun (and omitting charmrun options like ++n 4 and +vp4),
and additional command line options are required as well.

.. code-block:: bash

   $ ./pgm +msgLogRead +msgLogRank 2 +msgLogFilename "msg2.log"

User Defined Initial Mapping
----------------------------

By default AMPI maps virtual processes to processing elements in a
blocked fashion. This maximizes communication locality in the common
case, but may not be ideal for all applications. With AMPI, users can
define the initial mapping of virtual processors to physical processors
at runtime, either choosing from the predefined initial mappings below
or defining their own mapping in a file.

Round Robin
   This mapping scheme maps virtual processor to physical processor in
   round-robin fashion, i.e. if there are 8 virtual processors and 2
   physical processors then virtual processors indexed 0,2,4,6 will be
   mapped to physical processor 0 and virtual processors indexed 1,3,5,7
   will be mapped to physical processor 1.

   .. code-block:: bash

      $ ./charmrun ./hello +p2 +vp8 +mapping RR_MAP

Block Mapping
   This mapping scheme maps virtual processors to physical processor in
   ranks, i.e. if there are 8 virtual processors and 2 physical
   processors then virtual processors indexed 0,1,2,3 will be mapped to
   physical processor 0 and virtual processors indexed 4,5,6,7 will be
   mapped to physical processor 1.

   .. code-block:: bash

      $ ./charmrun ./hello +p2 +vp8 +mapping BLOCK_MAP

Proportional Mapping
   This scheme takes the processing capability of physical processors
   into account for mapping virtual processors to physical processors,
   i.e. if there are 2 processors running at different frequencies, then
   the number of virtual processors mapped to processors will be in
   proportion to their processing power. To make the load balancing
   framework aware of the heterogeneity of the system, the flag
   *+LBTestPESpeed* should also be used.

   .. code-block:: bash

      $ ./charmrun ./hello +p2 +vp8 +mapping PROP_MAP
      $ ./charmrun ./hello +p2 +vp8 +mapping PROP_MAP +balancer GreedyLB +LBTestPESpeed

Custom Mapping
   To define your own mapping scheme, create a file named "mapfile"
   which contains on each line the PE number you'd like that virtual
   process to start on. This file is read when specifying the ``+mapping
   MAPFILE`` option. The following mapfile will result in VPs 0, 2, 4,
   and 6 being created on PE 0 and VPs 1, 3, 5, and 7 being created on
   PE 1:

   .. code-block:: none

      0
      1
      0
      1
      0
      1
      0
      1

   .. code-block:: bash

      $ ./charmrun ./hello +p2 +vp8 +mapping MAPFILE

   Note that users can find the current mapping of ranks to PEs (after
   dynamic load balancing) by calling ``AMPI_Comm_get_attr`` on
   ``MPI_COMM_WORLD`` with the predefined ``AMPI_MY_WTH`` attribute.
   This information can be gathered and dumped to a file for use in
   future runs as the mapfile.

Performance Visualization
-------------------------

AMPI users can take advantage of Charm++’s tracing framework and
associated performance visualization tool, Projections. Projections
provides a number of different views of performance data that help users
diagnose performance issues. Along with the traditional Timeline view,
Projections also offers visualizations of load imbalance and
communication-related data.

In order to generate tracing logs from an application to view in
Projections, link with ``ampicc -tracemode projections``.

AMPI defines the following extensions for tracing support:

.. code-block:: none

   AMPI_Trace_begin                      AMPI_Trace_end

When using the *Timeline* view in Projections, AMPI users can visualize
what each VP on each processor is doing (what MPI method it is running
or blocked in) by clicking the *View* tab and then selecting *Show
Nested Bracketed User Events* from the drop down menu. See the
Projections manual for information on performance analysis and
visualization.

AMPI users can also use any tracing libraries or tools that rely on
MPI’s PMPI profiling interface, though such tools may not be aware of
AMPI process virtualization.

.. _adaptive-mpi-ampi-codes:
