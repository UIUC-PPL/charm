==============================
Converse and Charm++ Libraries
==============================

.. contents::
   :depth: 3

Introduction
============

This manual describes Charm++ and Converse libraries. This is a work in
progress towards a standard library for parallel programming on top of
the Converse and Charm++ system. All of these libraries are included in
the source and binary distributions of Charm++/Converse.

liveViz Library
===============

.. _introduction-1:

Introduction
------------

If array elements compute a small piece of a large 2D image, then these
image chunks can be combined across processors to form one large image
using the liveViz library. In other words, liveViz provides a way to
reduce 2D-image data, which combines small chunks of images deposited by
chares into one large image.

This visualization library follows the client server model. The server,
a parallel Charm++ program, does all image assembly, and opens a network
(CCS) socket which clients use to request and download images. The
client is a small Java program. A typical use of this is:

.. code-block:: bash

   	cd charm/examples/charm++/wave2d
   	make
   	./charmrun ./wave2d +p2 ++server ++server-port 1234
   	~/ccs_tools/bin/liveViz localhost 1234

Use git to obtain a copy of ccs_tools (prior to using liveViz) and build
it by:

.. code-block:: bash

         cd ccs_tools;
         ant;

How to use liveViz with Charm++ program
---------------------------------------

The liveViz routines are in the Charm++ header “liveViz.h”.

A typical program provides a chare array with one entry method with the
following prototype:

.. code-block:: c++

     entry void functionName(liveVizRequestMsg *m);

This entry method is supposed to deposit its (array element’s) chunk of
the image. This entry method has following structure:

.. code-block:: c++

     void myArray::functionName (liveVizRequestMsg *m)
     {
       // prepare image chunk
          ...

       liveVizDeposit (m, startX, startY, width, height, imageBuff, this);

       // delete image buffer if it was dynamically allocated
     }

Here, “width” and “height” are the size, in pixels, of this array
element’s portion of the image, contributed in “imageBuff” (described
below). This will show up on the client’s assembled image at 0-based
pixel (startX,startY). The client’s display width and height are stored
in m->req.wid and m->req.ht.

By default, liveViz combines image chunks by doing a saturating sum of
overlapping pixel values. If you want liveViz to combine image chunks by
using max (i.e. for overlapping pixels in deposited image chunks, final
image will have the pixel with highest intensity or in other words
largest value), you need to pass one more parameter (liveVizCombine_t)
to the “liveVizDeposit” function:

.. code-block:: c++

    liveVizDeposit (m, startX, startY, width, height, imageBuff, this,
                    max_image_data);

You can also reduce floating-point image data using sum_float_image_data
or max_float_image_data.

Format of deposit image
-----------------------

“imageBuff” is run of bytes representing a rectangular portion of the
image. This buffer represents image using a row-major format, so 0-based
pixel (x,y) (x increasing to the right, y increasing downward in typical
graphics fashion) is stored at array offset “x+y*width”.

If the image is gray-scale (as determined by liveVizConfig, below), each
pixel is represented by one byte. If the image is color, each pixel is
represented by 3 consecutive bytes representing red, green, and blue
intensity.

If the image is floating-point, each pixel is represented by a single
‘float’, and after assembly colorized by calling the user-provided
routine below. This routine converts fully assembled ‘float’ pixels to
RGB 3-byte pixels, and is called only on processor 0 after each client
request.

.. code-block:: c++

  extern "C"
  void liveVizFloatToRGB(liveVizRequest &req,
      const float *floatSrc, unsigned char *destRgb,
      int nPixels);

liveViz Initialization
----------------------

liveViz library needs to be initialized before it can be used for
visualization. For initialization follow the following steps from your
main chare:

#. Create your chare array (array proxy object ’a’) with the entry
   method ’functionName’ (described above). You must create the chare
   array using a CkArrayOptions ’opts’ parameter. For instance,

   .. code-block:: c++

      	CkArrayOptions opts(rows, cols);
      	array = CProxy_Type::ckNew(opts);

#. Create a CkCallback object (’c’), specifying ’functionName’ as the
   callback function. This callback will be invoked whenever the client
   requests a new image.

#. Create a liveVizConfig object (’cfg’). LiveVizConfig takes a number
   of parameters, as described below.

#. Call liveVizInit (cfg, a, c, opts).

The liveVizConfig parameters are:

-  The first parameter is the pixel type to be reduced:

   -  “false” or liveVizConfig::pix_greyscale means a greyscale image (1
      byte per pixel).

   -  “true” or liveVizConfig::pix_color means a color image (3 RGB
      bytes per pixel).

   -  liveVizConfig::pix_float means a floating-point color image (1
      float per pixel, can only be used with sum_float_image_data or
      max_float_image_data).

-  The second parameter is the flag “serverPush”, which is passed to the
   client application. If set to true, the client will repeatedly
   request for images. When set to false the client will only request
   for images when its window is resized and needs to be updated.

-  The third parameter is an optional 3D bounding box (type CkBbox3d).
   If present, this puts the client into a 3D visualization mode.

A typical 2D, RGB, non-push call to liveVizConfig looks like this:

.. code-block:: c++

      liveVizConfig cfg(true,false);

Compilation
-----------

A Charm++ program that uses liveViz must be linked with ’-module
liveViz’.

Before compiling a liveViz program, the liveViz library may need to be
compiled. To compile the liveViz library:

-  go to .../charm/tmp/libs/ck-libs/liveViz

-  make

Poll Mode
---------

In some cases you may want a server to deposit images only when it is
ready to do so. For this case the server will not register a callback
function that triggers image generation, but rather the server will
deposit an image at its convenience. For example a server may want to
create a movie or series of images corresponding to some timesteps in a
simulation. The server will have a timestep loop in which an array
computes some data for a timestep. At the end of each iteration the
server will deposit the image. The use of LiveViz’s Poll Mode supports
this type of server generation of images.

Poll Mode contains a few significant differences to the standard mode.
First we describe the use of Poll Mode, and then we will describe the
differences. liveVizPoll must get control during the creation of your
array, so you call liveVizPollInit with no parameters.

.. code-block:: c++

   	liveVizPollInit();
   	CkArrayOptions opts(nChares);
   	arr = CProxy_lvServer::ckNew(opts);

To deposit an image, the server just calls liveVizPollDeposit. The
server must take care not to generate too many images, before a client
requests them. Each server generated image is buffered until the client
can get the image. The buffered images will be stored in memory on
processor 0.

.. code-block:: c++

     liveVizPollDeposit(this,
                        startX,startY,            // Location of local piece
                        localSizeX,localSizeY,    // Dimensions of the piece I'm depositing
                        globalSizeX,globalSizeY,  // Dimensions of the entire image
                        img,                      // Image byte array
                        sum_image_data,           // Desired image combiner
                        3                         // Bytes/pixel
                       );

The last two parameters are optional. By default they are set to
sum_image_data and 3 bytes per pixel.

A sample liveVizPoll server and client are available at:

.. code-block:: none

              .../charm/examples/charm++/lvServer
              .../ccs_tools/bin/lvClient

This example server uses a PythonCCS command to cause an image to be
generated by the server. The client also then gets the image.

LiveViz provides multiple image combiner types. Any supported type can
be used as a parameter to liveVizPollDeposit. Valid combiners include:
sum_float_image_data, max_float_image_data, sum_image_data, and
max_image_data.

The differences in Poll Mode may be apparent. There is no callback
function which causes the server to generate and deposit an image.
Furthermore, a server may generate an image before or after a client has
sent a request. The deposit function, therefore is more complicated, as
the server will specify information about the image that it is
generating. The client will no longer specify the desired size or other
configuration options, since the server may generate the image before
the client request is available to the server. The liveVizPollInit call
takes no parameters.

The server should call Deposit with the same global size and combiner
type on all of the array elements which correspond to the “this”
parameter.

The latest version of liveVizPoll is not backwards compatable with older
versions. The old version had some fundamental problems which would
occur if a server generated an image before a client requested it. Thus
the new version buffers server generated images until requested by a
client. Furthermore the client requests are also buffered if they arrive
before the server generates the images. Problems could also occur during
migration with the old version.

Caveats
-------

If you use the old version of “liveVizInit" method that only receives 3
parameters, you will find a known bug caused by how “liveVizDeposit”
internally uses a reduction to build the image.

Using that version of the “liveVizInit" method, its contribute call is
handled as if it were the chare calling “liveVizDeposit” that actually
contributed to the liveViz reduction. If there is any other reduction
going on elsewhere in this chare, some liveViz contribute calls might be
issued before the corresponding non-liveViz contribute is reached. This
would imply that image data would be treated as if were part of the
non-liveViz reduction, leading to unexpected behavior potentially
anywhere in the non-liveViz code.

Multi-phase Shared Arrays Library
=================================

The Multiphase Shared Arrays (MSA) library provides a specialized shared
memory abstraction in Charm++ that provides automatic memory management.
Explicitly shared memory provides the convenience of shared memory
programming while exposing the performance issues to programmers and the
“intelligent” ARTS.

Each MSA is accessed in one specific mode during each phase of
execution: ``read-only`` mode, in which any thread can read any element
of the array; ``write-once`` mode, in which each element of the array is
written to (possibly multiple times) by at most one worker thread, and
no reads are allowed and ``accumulate`` mode, in which any threads can
add values to any array element, and no reads or writes are permitted. A
``sync`` call is used to denote the end of a phase.

We permit multiple copies of a page of data on different processors and
provide automatic fetching and caching of remote data. For example,
initially an array might be put in ``write-once`` mode while it is
populated with data from a file. This determines the cache behavior and
the permitted operations on the array during this phase. ``write-once``
means every thread can write to a different element of the array. The
user is responsible for ensuring that two threads do not write to the
same element; the system helps by detecting violations. From the cache
maintenance viewpoint, each page of the data can be over-written on it’s
owning processor without worrying about transferring ownership or
maintaining coherence. At the ``sync``, the data is simply merged.
Subsequently, the array may be ``read-only`` for a while, thereafter
data might be ``accumulate``\ ’d into it, followed by it returning to
``read-only`` mode. In the ``accumulate`` phase, each local copy of the
page on each processor could have its accumulations tracked
independently without maintaining page coherence, and the results
combined at the end of the phase. The ``accumulate`` operations also
include set-theoretic union operations, i.e. appending items to a set of
objects would also be a valid ``accumulate`` operation. User-level or
compiler-inserted explicit ``prefetch`` calls can be used to improve
performance.

A software engineering benefit that accrues from the explicitly shared
memory programming paradigm is the (relative) ease and simplicity of
programming. No complex, buggy data-distribution and messaging
calculations are required to access data.

To use MSA in a Charm++ program:

-  build Charm++ for your architecture, e.g. ``netlrts-linux-x86_64``.

-  ``cd charm/netlrts-linux-x86_64/tmp/libs/ck-libs/multiphaseSharedArrays/; make``

-  ``#include “msa/msa.h”`` in your header file.

-  Compile using ``charmc`` with the option ``-module msa``

The API is as follows: See the example programs in
``charm/pgms/charm++/multiphaseSharedArrays``.

3D FFT Library
==============

The previous 3D FFT library has been deprecated and replaced with this
new 3D FFT library. The new 3D FFT library source can be downloaded with
following command: *git clone
https://charm.cs.illinois.edu/gerrit/libs/fft*

Introduction and Motivation
---------------------------

The 3D Charm-FFT library provides an interface to do parallel 3D FFT
computation in a scalable fashion.

The parallelization is achieved by splitting the 3D transform into three
phases, using 2D decomposition. First, 1D FFTs are computed over the
pencils; then a ’transform’ is performed and 1D FFTs are done over
second dimension; again a ’transform’ is performed and FFTs are computed
over the last dimension. So this approach takes three computation phases
and two ’transform’ phases.

This library allows users to create multiple instances of the library
and perform concurrent FFTs using them. Each of the FFT instances run in
background as other parts of user code execute, and a callback is
invoked when FFT is complete.

Features
--------

Charm-FFT library provides the following features:

-  *2D-decomposition*: Users can define fine-grained 2D-decomposition
   that increases the amount of available parallelism and improves
   network utilization.

-  *Cutoff-based smaller grid*: The data grid may have a cut off.
   Charm-FFT improves performance by avoiding communication and
   computation of the data beyond the cutoff.

-  *User-defined mapping of library objects*: The placement of objects
   that constitute the library instance can be defined by the user based
   on the application’s other concurrent communication and placement of
   other objects.

-  *Overlap with other computational work*: Given the callback-based
   interface and Charm++’s asynchrony, the FFTs are performed in the
   background while other application work can be done in parallel.

Compilation and Execution
-------------------------

To install the FFT library, you will need to have charm++ installed in
you system. You can follow the Charm++ manual to do that. Then, ensure
that FFTW3 is installed. FFTW3 can be downloaded from
*http://www.fftw.org*.  The Charm-FFT library source can be downloaded
with following command: *git clone
https://charm.cs.illinois.edu/gerrit/libs/fft*

Inside of Charm-FFT directory, you will find *Makefile.default*. Copy
this file to *Makefile.common*, change the copy’s variable *FFT3_HOME*
to point your FFTW3 installation and *CHARM_DIR* to point your Charm++
installation then run *make*.  To use Charm-FFT library in an
application, add the line *extern module fft_Charm;* to it charm
interface (.ci) file and include *fft_charm.h* and *fftw3.h* in relevant
C files. Finally to compile the program, pass *-lfft_charm* and -lfftw3
as arguments to *charmc*.

Library Interface
-----------------

To use Charm-FFT interface, the user must start by calling
*Charm_createFFT* with following parameters.

.. code-block:: none

       Charm_createFFT(N_x, N_y, N_z, z_x, z_y, y_x, y_z, x_yz, cutoff, hmati, fft_type, CkCallback);

       Where:
       int N_x : X dimension of FFT calculation
       int N_y : Y dimension of FFT calculation
       int N_z : Z dimension of FFT calculation
       int z_x : X dimension of Z pencil chare array
       int z_y : Y dimension of Z pencil chare array
       int y_x : X dimension of Y pencil chare array
       int y_z : Z dimension of Y pencil chare array
       int x_yz: A dimension of X pencil chare array
       double cutoff: Cutoff of FFT grid
       double *hmati: Hamiltonian matrix representing cutoff
       FFT_TYPE: Type of FFT to perform. Either CC for complex-to-complex or RC for real-complex
       CkCallback: A Charm++ entry method for callback upon the completion of library initialization

This creates necessary proxies (Z,Y,X etc) for performing FFT of size
:math:`N_x \times N_y * N_z` using 2D chare arrays (pencils) of size
:math:`n_y \times n_x` (ZPencils), :math:`n_z \times n_x` (YPencils),
and :math:`n_x \times n_y` (XPencils). When done, calls
:math:`myCallback` which should receive :math:`CProxy\_fft2d\ id` as a
unique identifier for the newly created set of proxies.

An example of Charm-FFT initialization using Charm_createFFT:

.. code-block:: charmci

  // .ci
  extern module fft_charm;

  mainchare Main {
      entry Main(CkArgMsg *m);
  }

  group Driver {
      entry Driver(FFT_Type fft_type);
      entry void proxyCreated(idMsg *msg);
      entry void fftDone();
  }

  // .C
  Main::Main(CkArgMsg *m) {
      ...
      /* Assume FFT of size N_x, N_y, N_z */
      FFT_Type fft_type = CC

      Charm_createFFT(N_x, N_y, N_z, z_x, z_y, y_x, y_z, x_yz, cutoff, hmati,
                      fft_type, CkCallback(CkIndex_Driver::proxyCreated(NULL), driverProxy));
  }

  Driver::proxyCreated(idMsg *msg) {
      CProxy_fft2d fftProxy = msg->id;
      delete msg;
  }

In this example, an entry method *Driver::proxyCreated* will be called
when an FFT instance has been created.

Using the newly received proxy, the user can identify whether a local PE
has XPencils and/or ZPencils.

.. code-block:: c++

       void Driver::proxyCreated(idMsg *msg) {
         CProxy_fft2d fftProxy = msg->id;

         delete msg;

         bool hasX = Charm_isOutputPE(fftProxy),
              hasZ = Charm_isInputPE(fftProxy);

         ...
       }

Then, the grid’s dimensions on a PE can be acquired by using
*Charm_getOutputExtents* and *Charm_getInputExtents*.

.. code-block:: c++

       if (hasX) {
         Charm_getOutputExtents(gridStart[MY_X], gridEnd[MY_X],
                               gridStart[MY_Y], gridEnd[MY_Y],
                               gridStart[MY_Z], gridEnd[MY_Z],
                               fftProxy);
       }

       if (hasZ) {
         Charm_getInputExtents(gridStart[MY_X], gridEnd[MY_X],
                               gridStart[MY_Y], gridEnd[MY_Y],
                               gridStart[MY_Z], gridEnd[MY_Z],
                               fftProxy);
       }

       for(int i = 0; i < 3; i++) {
         gridLength[i] = gridEnd[i] - gridStart[i];
       }

With the grid’s dimension, the user must allocate and set the input and
output buffers. In most cases, this is simply the product of the three
dimensions, but for real-to-complex FFT calcaultion, FFTW-style storage
for the input buffers is used (as shown below).

.. code-block:: c++

       dataSize = gridLength[MY_X] * gridLength[MY_Y] * gridLength[MY_Z];

       if (hasX) {
         dataOut = (complex*) fftw_malloc(dataSize * sizeof(complex));

         Charm_setOutputMemory((void*) dataOut, fftProxy);
       }

       if (hasZ) {
         if (fftType == RC) {
           // FFTW style storage
           dataSize = gridLength[MY_X] * gridLength[MY_Y] * (gridLength[MY_Z]/2 + 1);
         }

         dataIn = (complex*) fftw_malloc(dataSize * sizeof(complex));

         Charm_setInputMemory((void*) dataIn, fftProxy);
       }

Then, from *PE0*, start the forward or backward FFT, setting the entry
method *fftDone* as the callback function that will be called when the
FFT operation is complete.

For forward FFT

.. code-block:: c++

       if (CkMyPe() == 0) {
           Charm_doForwardFFT(CkCallback(CkIndex_Driver::fftDone(), thisProxy), fftProxy);
       }

For backward FFT

.. code-block:: c++

       if (CkMyPe() == 0) {
           Charm_doBackwardFFT(CkCallback(CkIndex_Driver::fftDone(), thisProxy), fftProxy);
       }

The sample program to run a backward FFT can be found in
*Your_Charm_FFT_Path/tests/simple_tests*


TRAM
====

Overview
--------

Topological Routing and Aggregation Module is a library for optimization
of many-to-many and all-to-all collective communication patterns in
Charm++ applications. The library performs topological routing and
aggregation of network communication in the context of a virtual grid
topology comprising the Charm++ Processing Elements (PEs) in the
parallel run. The number of dimensions and their sizes within this
topology are specified by the user when initializing an instance of the
library.

TRAM is implemented as a Charm++ group, so an *instance* of TRAM has one
object on every PE used in the run. We use the term *local instance* to
denote a member of the TRAM group on a particular PE.

Most collective communication patterns involve sending linear arrays of
a single data type. In order to more efficiently aggregate and process
data, TRAM restricts the data sent using the library to a single data
type specified by the user through a template parameter when
initializing an instance of the library. We use the term *data item* to
denote a single object of this datatype submitted to the library for
sending. While the library is active (i.e. after initialization and
before termination), an arbitrary number of data items can be submitted
to the library at each PE.

On systems with an underlying grid or torus network topology, it can be
beneficial to configure the virtual topology for TRAM to match the
physical topology of the network. This can easily be accomplished using
the Charm++ Topology Manager.

The next two sections explain the routing and aggregation techniques
used in the library.

Routing
~~~~~~~

Let the variables :math:`j` and :math:`k` denote PEs within an
N-dimensional virtual topology of PEs and :math:`x` denote a dimension
of the grid. We represent the coordinates of :math:`j` and :math:`k`
within the grid as :math:`\left
(j_0, j_1, \ldots, j_{N-1} \right)` and :math:`\left (k_0, k_1, \ldots,
k_{N-1} \right)`. Also, let

.. math::

   f(x, j, k) =
   \begin{cases}
   0, & \text{if } j_x = k_x \\
   1, & \text{if } j_x \ne k_x
   \end{cases}

:math:`j` and :math:`k` are *peers* if

.. math:: \sum_{d=0}^{N-1} f(d, j, k) = 1 .

When using TRAM, PEs communicate directly only with their peers. Sending
to a PE which is not a peer is handled inside the library by routing the
data through one or more *intermediate destinations* along the route to
the *final destination*.

Suppose a data item destined for PE :math:`k` is submitted to the
library at PE :math:`j`. If :math:`k` is a peer of :math:`j`, the data
item will be sent directly to :math:`k`, possibly along with other data
items for which :math:`k` is the final or intermediate destination. If
:math:`k` is not a peer of :math:`j`, the data item will be sent to an
intermediate destination :math:`m` along the route to :math:`k` whose
index is :math:`\left (j_0, j_1, \ldots, j_{i-1}, k_i,
j_{i+1}, \ldots, j_{N-1} \right)`, where :math:`i` is the greatest value
of :math:`x` for which :math:`f(x, j, k) = 1`.

Note that in obtaining the coordinates of :math:`m` from :math:`j`,
exactly one of the coordinates of :math:`j` which differs from the
coordinates of :math:`k` is made to agree with :math:`k`. It follows
that m is a peer of :math:`j`, and that using this routing process at
:math:`m` and every subsequent intermediate destination along the route
eventually leads to the data item being received at :math:`k`.
Consequently, the number of messages :math:`F(j, k)` that will carry the
data item to the destination is

.. math:: F(j,k) = \sum_{d=0}^{N-1}f(d, j, k) .

Aggregation
~~~~~~~~~~~

Communicating over the network of a parallel machine involves per
message bandwidth and processing overhead. TRAM amortizes this overhead
by aggregating data items at the source and every intermediate
destination along the route to the final destination.

Every local instance of the TRAM group buffers the data items that have
been submitted locally or received from another PE for forwarding.
Because only peers communicate directly in the virtual grid, it suffices
to have a single buffer per PE for every peer. Given a dimension d
within the virtual topology, let :math:`s_d` denote its *size*, or the
number of distinct values a coordinate for dimension d can take.
Consequently, each local instance allocates up to :math:`s_d - 1`
buffers per dimension, for a total of :math:`\sum_{d=0}^{N-1} (s_d - 1)`
buffers. Note that this is normally significantly less than the total
number of PEs specified by the virtual topology, which is equal to
:math:`\prod_{d=0}^{N-1}
{s_d}`.

Sending with TRAM is done by submitting a data item and a destination
identifier, either PE or array index, using a function call to the local
instance. If the index belongs to a peer, the library places the data
item in the buffer for the peer’s PE. Otherwise, the library calculates
the index of the intermediate destination using the previously described
algorithm, and places the data item in the buffer for the resulting PE,
which by design is always a peer of the local PE. Buffers are sent out
immediately when they become full. When a message is received at an
intermediate destination, the data items comprising it are distributed
into the appropriate buffers for subsequent sending. In the process, if
a data item is determined to have reached its final destination, it is
immediately delivered.

The total buffering capacity specified by the user may be reached even
when no single buffer is completely filled up. In that case the buffer
with the greatest number of buffered data items is sent.

Sending to a Chare Array
^^^^^^^^^^^^^^^^^^^^^^^^

For sending to a chare array, the entry method should be marked [aggregate],
which can take attribute parameters:

.. code-block:: c++

   array [1D] test {
     entry [aggregate(numDimensions: 2, bufferSize: 2048, thresholdFractionNumer : 1,
     thresholdFractionDenom : 2, cutoffFractionNumer : 1,
     cutoffFractionDenom : 2)] void ping(vector<int> data);
   };

Description of parameters:

-  maxNumDataItemsBuffered: maximum number of items that the library is
   allowed to buffer per PE

-  numDimensions: number of dimensions in grid of PEs

-  bufferSize: size of the buffer for each peer, in terms of number of
   data items

-  thresholdFractionNumer: numerator of the fraction of the buffer that
   data items

-  thresholdFractionDenom: size of the buffer for each peer, in terms of number of
   data items

-  cutoffFractionNumer: size of the buffer for each peer, in terms of number of
   data items

-  cutoffFractionDenom: size of the buffer for each peer, in terms of number of
   data items

Sending
~~~~~~~

Sending with TRAM is done through calls to the entry method marked as [aggregate].

Termination
~~~~~~~~~~~

Flushing and termination mechanisms are used in TRAM to prevent deadlock
due to indefinite buffering of items. Flushing works by sending out all
buffers in a local instance if no items have been submitted or received
since the last progress check. Meanwhile, termination detection support is
necessary for certain applications.

Currently, the only termination detection method supported is quiescence
detection.

When using quiescence detection, no end callback is used, and no done
calls are required. Instead, termination of a communication step is
achieved using the quiescence detection framework in Charm++, which
supports passing a callback as parameter. TRAM is set up such that
quiescence will not be detected until all items sent in the current
communication step have been delivered to their final destinations.

Periodic flushing is an auxiliary mechanism which checks at a regular
interval whether any sends have taken place since the last time the
check was performed. If not, the mechanism sends out all the data items
buffered per local instance of the library.  A typical use case for periodic
flushing is when the submission of a data item B to TRAM happens as a
result of the delivery of another data item A sent using the same TRAM
instance. If A is buffered inside the library and insufficient data
items are submitted to cause the buffer holding A to be sent out, a
deadlock could arise. With the periodic flushing mechanism, the buffer
holding A is guaranteed to be sent out eventually, and deadlock is
prevented.

Opting into Fixed-Size Message Handling
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Variable-sized message handling in TRAM includes storing
and sending additional data that is irrelevant in
the case of fixed-size messages. To opt into the faster
fixed-size codepath, the is_PUPbytes type trait should be
explicitly defined for the message type:

.. code-block:: charmci

   array [1D] test {
     entry [aggregate(numDimensions: 2, bufferSize: 2048, thresholdFractionNumer : 1,
     thresholdFractionDenom : 2, cutoffFractionNumer : 1,
     cutoffFractionDenom : 2)] void ping(int data);
   };

.. code-block:: c++

   template <>
   struct is_PUPbytes<int> {
     static const bool value = true;
   };

Example
-------

For example code showing how to use TRAM, see ``examples/charm++/TRAM`` and
``benchmarks/charm++/streamingAllToAll`` in the Charm++ repository.


CkIO
====

Overview
--------

CkIO is a library for parallel I/O in Charm++. It supports both reading and writing via two independent library components which both involve aggregation. The CkIO abstraction helps get the best performance out of the parallel file system and avoid contention on I/O nodes, while supporting any user-level chare decomposition. 

CkIO Output
-----------

The CkIO output library improves the performance of write
operations by aggregating data at intermediate nodes and batching writes to
align with the stripe size of the underlying parallel file system (such as
Lustre). This helps avoid contention on the I/O nodes by using fewer messages to
communicate with them and preventing small or non-contiguous disk operations.

Under the hood, when a write is issued, the associated data is sent to the PE(s)
corresponding to the stripe of the file the write is destined for. The data is
then kept on that PE until enough contiguous data is collected, after which the
entire stripe is actually written to the filesystem all in one fell swoop. The
size and layout of stripes and the number and organization of aggregating PEs
are available as options for the user to customize.

CkIO Input
----------

The CkIO input library similarly aggregates read requests for a single file via an intermediate layer of chares, called "Buffer Chares." The number of Buffer Chares should be chosen to read from the file system with optimal granularity. Currently, the choice of the number of Buffer Chares must be made by the user (via the Options parameter, discussed below), considering factors such as file size, number of PEs, and number of nodes.

Using CkIO
----------

CkIO is designed as a session-oriented, callback-centric library. The steps to
using the library are different for input and output, but follow the same basic structure:

#. Open a file via ``Ck::IO::open``. Note that at the lowest level CkIO uses POSIX seek, read, and write (or the Microsoft equivalent for Windows) and therefore must only be used on seek-able file types. 
#. Create a session for writing to the file via ``Ck::IO::startSession`` or create a session for reading from a file via ``Ck::IO::startReadSession``.
#. Write or read via ``Ck::IO::write`` or ``Ck::IO::read``. Note that these function take a
   session token that is passed into the callback, which should refer to the current session.
#. In the case of a read session, the session must be closed manually when the read is complete via ``Ck::IO::closeReadSession``
#. When the specified amount of data for the session has been written or a read session has been closed, a
   completion callback is invoked, from which one may start another session or
   close the file via ``Ck::IO::close`` (same call for writing or reading).

Parallel Output API
~~~~~~~~~~~~~~~~~~~

The following functions comprise the interface to the library for parallel file output:


- Opening a file:

  .. code-block:: c++

     void Ck::IO::open(std::string path, CkCallback opened, Ck::IO::Options opts)

  Open the given file with the options specified in ``opts``, and send a
  ``FileReadyMsg`` (wraps a ``Ck::IO::File file``) to the ``opened`` callback
  when the system is ready to accept session requests on that file. If the
  specified file does not exist, it will be created. Should only be called from
  a single PE, once per file.

  ``Ck::IO::Options`` is a struct with the following output-relevant fields:

  - ``writeStripe`` - Amount of contiguous data (in bytes) to gather before
    writing to the file (default: file system stripe size if using Lustre and
    API provides it, otherwise 4 MB)
  - ``peStripe`` - Amount of contiguous data to assign to each active PE
    (default: ``4 * writeStripe``)
  - ``activePEs`` - Number of PEs to use for I/O (default: min(32, number of
    PEs))
  - ``basePE`` - Index of first participating PE (default: 0)
  - ``skipPEs`` - Gap between participating PEs (default : ``CkMyNodeSize()``)


- Starting a write session:

  Note there are two variants of the ``startSession`` function, a regular one
  and one that writes a user specified chunk of data to the file at the end of a
  session.

  .. code-block:: c++

    void Ck::IO::startSession(Ck::IO::File file, size_t size, size_t offset, CkCallback ready,
                   CkCallback complete)

  Prepare to write data into ``file``, in the window defined by ``size`` and
  ``offset`` (both specified in bytes). When the session is set up, a
  ``SessionReadyMsg`` (wraps a ``Ck::IO::Session session``) will be sent to the
  ``ready`` callback. When all of the data has been written and synced, an empty
  ``CkReductionMsg`` will be sent to the ``complete`` callback. Should only be
  called from a single PE, once per session.

  .. code-block:: c++

     void Ck::IO::startSession(Ck::IO::File file, size_t size, size_t offset, CkCallback ready,
                    const char *commitData, size_t commitSize, size_t commitOffset,
                    CkCallback complete)

  Prepare to write data into ``file``, in the window defined by ``size`` and
  ``offset`` (both specified in bytes). When the session is set up, a
  ``SessionReadyMsg`` (wraps a ``Ck::IO::Session session``) will be sent to the
  ``ready`` callback. When all of the data has been written and synced, an
  additional write of ``commitData`` (of size ``commitSize``) will be made to
  the file at the specified offset (``commitOffset``) to "commit" the session's
  work. When that write has completed, an empty ``CkReductionMsg`` will be sent
  to the ``complete`` callback. Should only be called from a single PE, once per
  session.

- Writing data:

  .. code-block:: c++

    void Ck::IO::write(Ck::IO::Session session, const char *data, size_t bytes, size_t offset)

  Write the given data into the file to which ``session`` is associated. The
  offset is relative to the file as a whole, not to the session's offset. Note
  that ``session`` is provided as a member of the ``SessionReadyMsg`` sent to
  the ``ready`` callback after a session has started. Can be called multiple
  times from multiple PEs.

- Closing a file:

  .. code-block:: c++

    void Ck::IO::close(Ck::IO::File file, CkCallback closed)

  Close a previously opened file. All sessions on that file must have already
  signaled that they are complete. Note that ``file`` is provided as a member of
  the ``FileReadyMsg`` sent to the ``opened`` callback after a file has been
  opened. Should only be called from a single PE, once per file.

Parallel Input API
~~~~~~~~~~~~~~~~~~

The following functions comprise the interface to the library for parallel file input:


- Opening a file:

  .. code-block:: c++

     void Ck::IO::open(std::string path, CkCallback opened, Ck::IO::Options opts)

  Open the given file with the options specified in ``opts``, and send a
  ``FileReadyMsg`` (wraps a ``Ck::IO::File file``) to the ``opened`` callback
  when the system is ready to accept session requests on that file. If the
  specified file does not exist, it will be created. Should only be called from
  a single PE, once per file.

  ``Ck::IO::Options`` is a struct with the following input-relevant fields:

  - ``numReaders`` - number of Buffer Chares, or aggregators. The user should chose this number to optimally decompose the read. Typically, chosing the number of Buffer Chares to be the number of PEs performs well.


- Starting a read session:

  Note there are two variants of the ``startReadSession`` function, a regular one and a variant which takes an additional argument allowing the user to map Buffer Chares to specified PEs in a round-robin fashion.

  .. code-block:: c++

    void startReadSession(File file, size_t bytes, size_t offset, CkCallback ready)

  Prepare to read data from ``file``, in the window defined by ``size`` and
  ``offset`` (both specified in bytes). On starting the session, the buffer 
  chares begin eagerly reading all requested data into memory. The ready callback 
  is invoked once these reads have been initiated (but they are not guaranteed to be complete at this point).

  .. code-block:: c++

    void startReadSession(File file, size_t bytes, size_t offset, CkCallback ready, std::vector<int> pes_to_map)

  This function is similar to the previous one, but the extra argument pes_to_map allows the user to specify a list of PEs to map the Buffer Chares to. 
  This argument should contain a sequence of numbers representing pes. The Buffer Chares will be mapped to the PEs in a round-robin fashion. 
  This can be useful when the user has a specific decomposition in mind for the read.

- Reading data:

  .. code-block:: c++

    void read(Session session, size_t bytes, size_t offset, char* data, CkCallback after_read);

  This method is invoked to read data asynchronously from the read session. This method returns immediately to the caller, but the 
  read is only guaranteed complete once the callback ``after_read`` is called. Internally, the read request is buffered
  until the Buffer Chares can respond with the requested data. After the read finishes, the 
  after_read callback is invoked taking a ReadCompleteMsg* which points to a char* buffer, the offset,
  and the number of bytes of the read.


- Closing a file:

  .. code-block:: c++

    void Ck::IO::close(Ck::IO::File file, CkCallback closed)

  Close a previously-opened file. All read sessions on that file must have already
  been closed. Note that ``file`` is provided as a member of
  the ``FileReadyMsg`` sent to the ``opened`` callback after a file has been
  opened. This method should only be called from a single PE, once per file.

FileReader API
--------------

The FileReader API is an additional abstraction layer built on top of Ck::IO to support 
streaming reads from a file and implement the callback internally. This API is designed to 
match that of the c++ std::ifstream. Under the hood, when an application reads a small
number of bytes via a FileReader object,
the Buffer Chare will send a large chunk of data to the FileReader which can be buffered there
until the application requests more.

- Creating a FileReader object:

  .. code-block:: c++

    FileReader::FileReader(Ck::IO::Session session)

  Before creating a FileReader, the Ck::IO::Session must be created (see above). This session is 
  passed in to the FileReader constructor.

- Reading data:

  .. code-block:: c++

    FileReader& FileReader::read(char* buffer, size_t num_bytes_to_read)

  Read the specified number of bytes from the file opened in the session. The data is read into the buffer.
  This method is blocking and returns a pointer to the FileReader object.

- Seeking:

  There are two functions for seeking in the file, one for seeking from the current position, and one for seeking from a set position (like the end, or the beginning).
  
  .. code-block:: c++
  
    FileReader& FileReader::seekg(size_t pos)

  .. code-block:: c++
  
    FileReader& FileReader::seekg(size_t pos, std::ios_base::seekdir dir)

  The options for std::ios_base::seekdir are std::ios_base::beg, std::ios_base::cur, and std::ios_base::end.

- Tell functionality:

  .. code-block:: c++

    size_t FileReader::tellg()

  This function returns the current position in the file.

- End of file and gcount:

  .. code-block:: c++

    bool FileReader::eof()

  This function returns true if the end of the file has been reached and false otherwise.

  .. code-block:: c++

    size_t FileReader::gcount()

  This function returns the number of bytes read by the last read operation.
 

Examples
--------

For example code showing how to use CkIO for output, see ``tests/charm++/io/``.

For example code showing how to use CkIO for input (including FileReader), see ``tests/charm++/io_read/``.
