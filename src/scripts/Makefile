#############################################################################"
#
# This one Makefile serves all of converse and charm.  It includes
# rules for building:
#
#     * The CONVERSE libraries
#     * Charm++ runtime
#     * ITC++ Translator
#     * Languages
#
# This Makefile is common to all machines.  This works because the charmc
# is smart enough to "conceal" the differences between machines.  charmc
# gets its smarts by reading the conv-mach.sh configuration file.
#
# If you wish to compile with debugging, optimization, or some other charm
# option, you can do so by setting OPTS.  For example, if you need a lot
# of debugging information, you could type:
#
#     make charm++ OPTS="-g -save -verbose"
#
###############################################################################

SHELL=/bin/sh
L=../lib
INC=../include

include charmpath.mk
include conv-mach-opt.mak

CC=$(CHARMC)
CXX=$(CHARMC)
CFLAGS=-I.
CXXFLAGS=-I.

all: charm++ bigsim LIBS AMPI f90charm TSM SM PVM everylb

all-test: all-test-tests all-test-examples all-test-benchmarks

all-test-tests: bigsim LIBS
	$(MAKE) -C ../tests all

all-test-examples: bigsim LIBS
	$(MAKE) -C ../examples all

all-test-benchmarks: bigsim LIBS
	$(MAKE) -C ../benchmarks all

all-zc: charm++
	$(MAKE) -C ../tests/charm++/zerocopy all
	$(MAKE) -C ../examples/charm++/zerocopy all
	$(MAKE) -C ../benchmarks/charm++/zerocopy all

charm++: charm-target
	@echo "$(OPTS)" > OPTS
	touch charm++

bgampi: bigsim AMPI

TSM: converse $(L)/libtsm.a

SM: converse $(L)/libsm.a

PVM: converse $(L)/libpvmc.a

AMR: charm++ LIBS
	$(MAKE) -C libs/ck-libs/amr all

FEM: ParFUM

ParFUM: charm++ AMPI
	$(MAKE) -C libs/ck-libs/ ParFUM

MBLOCK: AMPI MBLOCKC MBLOCKF

MBLOCKC: charm++ LIBS

MBLOCKF: MBLOCKC

AMPI:  charm++ $(BIGSIM)
	$(MAKE) -C libs/ck-libs/ ampi

AMPIC AMPIF: AMPI

ARMCI: charm++ LIBS


f90charm: charm++ $(L)/libf90charm.a

libcharm: charm++
	cd $(L) ; $(CHARMC) -standalone -whole-archive -c++stl -shared -o libcharm.so $(LIBCHARM_LIBS)

charmpy: libcharm

charm4py: libcharm

LIBCHARM_LIBS= libck.a libconv-core.a libconv-util.a libmemory-default.a libconv-machine.a \
               libthreads-default.a libconv-partition.a libtmgr.a libhwloc_embedded.a \
               libldb-rand.a libconv-ldb.a libmoduleGreedyRefineLB.a libmoduleRandCentLB.a

DEFAULT_LIBS= completion NDMeshStreamer dummy pythonCCS io ckloop

default_libs: $(DEFAULT_LIBS)

$(DEFAULT_LIBS) cache pose fftlib liveViz metis:
	$(MAKE) -C libs/ck-libs/$@

openmp_llvm:
	$(MAKE) -C libs/conv-libs/$@

ChaNGa: charm++ cache liveViz everylb ckloop

cache: charm-core
liveViz: charm-core
io: charm-core
pose: charm-core
fftlib: charm-core
dummy: charmxi
pythonCCS: charmxi
completion: charmxi
NDMeshStreamer: charm-core completion

$(L)/libmpi-mainmodule.a: mpi-mainmodule.o
	$(CHARMC) -o $@ $<

tmgr: $(L)/libtmgr.a
$(L)/libtmgr.a:
	$(MAKE) -C topomanager -f Makefile_charm

$(L)/libckmain.a: main.o
	$(CHARMC) -o $@ $^

$(L)/libckmainf.a: mainf.o
	-$(CHARMC) -o $@ $^ || touch $@

translators: charmxi conv-cpm

msa: charm++
	$(MAKE) -C libs/ck-libs/multiphaseSharedArrays

jade: charm++ msa
#	make the lib, include
	$(MAKE) -C langs/jade


# This should be done offline, since the user does not need to compile
# parser files, etc.  All they need is the libjade + include files,
# antlr.jar, jade.jar, and a few scripts which the jade target takes
# care of.
#jadeall: jade
#	make parser.g -> .java files
#	$(MAKE) -C langs/jade p
#	make the jade.jar file
#	$(MAKE) -C ../java/charm/jade

#include TAU defs
TAU_MAKEFILE=Make.tau
include	$(TAU_MAKEFILE) #stub-makefile

#builds TAU's trace library
Tau: 
	echo "buliding charm trace library with support for TAU."
	$(MAKE) $(L)/libtrace-Tau.a $(L)/libtrace-all.a TAU_TRACE_OBJ=libtrace-Tau.o

LIBS: CONVLIBS CHARMLIBS everylb
	echo "Built LIBS Successfully"

bgtest: bigsim LIBS
	$(MAKE) -C ../tests bgtest
	$(MAKE) -C ../examples bgtest
	$(MAKE) -C ../benchmarks bgtest

syncfttest mpisyncfttest : charm++ LIBS
	$(MAKE) -C ../tests $@

test: bigsim LIBS
	$(MAKE) -C ../tests test
	$(MAKE) -C ../examples test
	$(MAKE) -C ../benchmarks test

test-tests: bigsim LIBS
	$(MAKE) -C ../tests test

test-examples: bigsim LIBS
	$(MAKE) -C ../examples test

test-benchmarks: bigsim LIBS
	$(MAKE) -C ../benchmarks test

test-zc: charm++
	$(MAKE) -C ../tests/charm++/zerocopy test
	$(MAKE) -C ../examples/charm++/zerocopy test
	$(MAKE) -C ../benchmarks/charm++/zerocopy test

test-converse: converse
	$(MAKE) -C ../tests test-converse
	$(MAKE) -C ../benchmarks test-converse

test-charm: charm++
	$(MAKE) -C ../tests test-charm
	$(MAKE) -C ../benchmarks test-charm

test-AMPI: AMPI
	$(MAKE) -C ../tests test-AMPI
	$(MAKE) -C ../benchmarks test-AMPI

test-tests-converse: converse
	$(MAKE) -C ../tests test-converse

test-tests-charm: charm++
	$(MAKE) -C ../tests test-charm

test-tests-AMPI: AMPI
	$(MAKE) -C ../tests test-AMPI

test-benchmarks-converse: converse
	$(MAKE) -C ../benchmarks test-converse

test-benchmarks-charm: charm++
	$(MAKE) -C ../benchmarks test-charm

test-benchmarks-AMPI: AMPI
	$(MAKE) -C ../benchmarks test-AMPI


CHARMC=../bin/charmc $(CHARMOPTS) $(OPTS)

SEQCHARMC=../bin/charmc -seq $(CHARMOPTS)

NATIVECHARMC=../bin/charmc -host $(CHARMOPTS)

CHARMXI=../bin/charmc -intrinsic $(OPTS)

CIDEPENDFILE = Make.cidepends
DEPENDFILE = Make.depends

include $(CIDEPENDFILE)
include $(DEPENDFILE)

CIFILES = $(wildcard *.ci)

%.ci.stamp: %.ci ../bin/charmxi
	$(CHARMXI) $< && touch $@

# Automatically generated by script Makefile_lb.sh
include Make.lb

# Build changes for external libraries
include Make.extlib

# GPU manager support
include Make.gpu

###############################################################################
#
# The basics (bin, lib, include, headers, charmc, conv-mach.sh)
#
###############################################################################

CVHEADERS=cpthreads.h converse.h conv-trace.h conv-random.h conv-qd.h \
      msgq.h queueing.h conv-taskQ.h taskqueue.h conv-cpath.h conv-cpm.h persistent.h\
      trace.h trace-common.h trace-bluegene.h trace-projections.h  \
      trace-simple.h trace-controlPoints.h charm-api.h \
      conv-ccs.h ccs-client.C ccs-client.h \
      ccs-server.h ccs-auth.C ccs-auth.h \
      memory-isomalloc.h debug-conv.h debug-conv++.h conv-autoconfig.h \
      conv-common.h conv-config.sh conv-config.h conv-mach.h conv-mach.sh conv-mach-common.h \
      blue.h blue-conv.h bgconverse.h cmipool.h mempool.h cmiqueue.h \
      cmitls.h lrtslock.h conv-rdma.h lrts-common.h conv-header.h

# The .c files are there to be #included by clients whole
# This is a bit unusual, but makes client linking simpler.
UTILHEADERS=pup.h pupf.h pup_c.h pup_stl.h pup_mpi.h pup_toNetwork.h pup_toNetwork4.h pup_paged.h pup_cmialloc.h\
	pup_c_functions.h \
	ckimage.h ckdll.h ckhashtable.h ckbitvector.h cklists.h ckliststring.h \
	cksequence.h ckstatistics.h ckvector3d.h conv-lists.h ckcomplex.h \
	sockRoutines.h sockRoutines.C cmimemcpy.h simd.h SSE-Double.h SSE-Float.h \
	crc32.h ckBIconfig.h rand48_replacement.h ckregex.h spanningTree.h cmirdmautils.h

CKHEADERS=ck.h ckstream.h objid.h envelope.h init.h qd.h charm.h charm++.h \
	  ckfutures.h ckIgetControl.h cktiming.h debug-charm.h\
	  ckcallback.h CkCallback.decl.h ckcallback-ccs.h 	\
	  cksection.h ckmessage.h cklocrec.h ckmigratable.h \
	  ckarrayindex.h ckarrayoptions.h ckarray.h cklocation.h ckmulticast.h ckreduction.h \
	  ckcheckpoint.h ckmemcheckpoint.h ckevacuation.h ckrdma.h\
          ckobjid.h\
	  ckobjQ.h readonly.h charm++_type_traits.h \
          $(UTILHEADERS) \
	  waitqd.h LBDatabase.h MetaBalancer.h RandomForestModel.h lbdb.h $(LBHEADERS) \
          LBComm.h LBOM.h LBObj.h LBMachineUtil.h \
	  RefinerTemp.h Refiner.h ckgraph.h ckheap.h \
          elements.h topology.h manager.h \
	  BaseLB.h CentralLB.h CentralLBMsg.h \
	  DistBaseLB.h HybridBaseLB.h HybridLBMsg.h \
	  BlueGene.h middle.h middle-conv.h middle-blue.h \
          CkMarshall.decl.h CkArray.decl.h CkLocation.decl.h CkMulticast.decl.h	\
          CkReduction.decl.h \
          waitqd.decl.h \
          CkFutures.decl.h CkCheckpoint.decl.h CkCheckpointStatus.decl.h CkMemCheckpoint.decl.h \
          LBDatabase.decl.h MetaBalancer.decl.h CentralLB.decl.h NullLB.decl.h \
          BaseLB.decl.h \
	  DistBaseLB.decl.h \
          HybridBaseLB.decl.h EveryLB.decl.h CommonLBs.decl.h \
          TraceSummary.decl.h TraceAutoPerf.decl.h TraceProjections.decl.h \
          TraceSimple.decl.h TraceControlPoints.decl.h TraceTau.decl.h \
	  TraceUtilization.decl.h BlueGene.decl.h \
	  ControlPoints.decl.h PathHistory.decl.h \
	  pathHistory.h envelope-path.h \
	  XArraySectionReducer.h \
          controlPoints.h controlPointsf.h arrayRedistributor.h cp_effects.h register.h stats.h   \
	  cksequence_internal.h cksequence_factory.h random_sequence.h strided_sequence.h \
	  mpi-interoperate.h mpi_main.decl.h  \
			trace-perf.h picsdefs.h  picsautoperf.h picstreenode.h \
			picsdecisiontree.h \
	    picsautoperfAPI.h picsautoperfAPIC.h \
	  $(CVHEADERS)

BLUE_HEADERS = blue_types.h bigsim_timing.h blue_defs.h bigsim_logs.h blue.h blue_impl.h bigsim_network.h bigsim_record.h bigsim_ooc.h bigsim_debug.h shared-alloc.h

ALLHEADERS=$(CKHEADERS) \
    simplemsg.h sm.h pvmc.h pvm3.h sdag.h vector2d.h \
	ckset.h LBSimulation.h  \
	$(BLUE_HEADERS)

basics: headers
	touch basics

# Headers is used only for the dependencies
headers: dirs+sources
	$(MAKE) -C topomanager -f Makefile_charm headers
	$(MAKE) charmxi
	$(MAKE) headerlinks

configure: Makefile.am configure.ac
	autoreconf

conv-autoconfig.h.in: configure.ac
	autoheader

conv-autoconfig.h: configure conv-autoconfig.h.in config_opts.sh
	@. ./config_opts.sh; echo ./configure $(QUIET) $$CONFIG_OPTS;  \
	./configure $(QUIET) $$CONFIG_OPTS && touch conv-autoconfig.h

.PHONY: clean veryclean VERSION.new config

VERSION: VERSION.new
	SRCBASE=$(SRCBASE) ./commitid.sh

commitid.C: VERSION
	echo "extern const char * const CmiCommitID;" > $@
	echo "const char * const CmiCommitID = \"`cat VERSION`\";" >> $@
commitid.o: CC=$(CHARMC)

headerlinks: dirs+sources Makefile $(ALLHEADERS)
	@[ "x$QUIET" = "x--quiet" ] || echo "Soft-linking headers..."
	-for hdr in $(filter-out dirs+sources Makefile,$(sort $^)) ; do test ! -f "../include/`basename $$hdr`" && ./system_ln "../tmp/$$hdr" ../include ; done
	touch headerlinks

dirs+sources:
	pwd
	find . -type l -exec rm {} \;
	rm -rf QuickThreads
	rm -rf hwloc
	rm -rf libs
	rm -rf ../bin ; mkdir ../bin
	if case $(CMK_VDIR) in *win64*) true;; *-win-*) true;; *) false;; esac; then \
		cp -f $(SRCBASE)/arch/win/unix2nt* ../bin; \
		cp -f $(SRCBASE)/arch/win/createlink.exe ../bin; \
	fi
	rm -rf ../lib ; mkdir ../lib
ifeq (false,$(CMK_NO_BUILD_SHARED))
	rm -rf ../lib_so ; mkdir ../lib_so; touch ../lib_so/.charmso
endif
	rm -rf ../benchmarks
	rm -rf ../examples
	rm -rf ../tests
	$(SRCBASE)/scripts/gatherflat $(SRCBASE)/scripts          .
	./gatherflat $(SRCBASE)/conv-core                        .
	./gatherflat $(SRCBASE)/conv-ldb                         .
	./gatherflat $(SRCBASE)/conv-ccs                         .
	./gatherflat $(SRCBASE)/conv-perf                        .
	./gatherflat $(SRCBASE)/ck-core                          .
	./gatherflat $(SRCBASE)/ck-perf                          .
	./gatherflat $(SRCBASE)/ck-pics                          .
	./gatherflat $(SRCBASE)/ck-ldb                           .
	./gatherflat $(SRCBASE)/ck-cp                            .
	./gatherflat $(SRCBASE)/langs/simplemsg                  .
	./gatherflat $(SRCBASE)/langs/pvmc                       .
	./gatherflat $(SRCBASE)/langs/bluegene                   .
	./gatherflat $(SRCBASE)/langs/f90charm                   .
	./gatherflat $(SRCBASE)/xlatcpm                          .
	./gathertree $(SRCBASE)/QuickThreads                     QuickThreads
	./gathertree $(SRCBASE)/../contrib/hwloc                 hwloc
	./gathertree $(SRCBASE)/libs                             libs
	./gathertree $(SRCBASE)/arch/util                        .
	./gathertree $(SRCBASE)/arch/util/proc_management        proc_management
	./gathertree $(SRCBASE)/util                             .
	./gathertree $(SRCBASE)/langs                            langs
	./gathertree $(SRCBASE)/langs/jade                       langs/jade
	./gathertree $(SRCBASE)/arch/common                      .
	./gathertree $(SRCBASE)/arch/$(CMK_GDIR)                 .
	test -f $(SRCBASE)/arch/$(CMK_GDIR)/gdir_link && cat $(SRCBASE)/arch/$(CMK_GDIR)/gdir_link > .gdir.new && ./gathertree $(SRCBASE)/arch/`cat .gdir.new`      . || true
	./gatherflat $(SRCBASE)/arch/$(CMK_VDIR)                 .
	./gathertree $(SRCBASE)/../benchmarks                    ../benchmarks
	./gathertree $(SRCBASE)/../examples                      ../examples
	./gathertree $(SRCBASE)/../tests                         ../tests
	rm -f ../bin/dep.pl ; cp dep.pl ../bin/
	chmod +x charmc
	-./system_ln  ../tmp/charmc ../bin/
	-./system_ln  $(SRCBASE)/scripts/testrun ../bin/
	-./system_ln  ../tmp/tree.txt ../bin/
	-./system_ln  ../tmp/fuzzytree.txt ../bin/
#	-ln -s ../../java/bin charmjavabin
#	-ln -s ../../java/bin ../bin/charmjavabin
#	-ln -s charmjavabin/* ../bin/
#	-ln -s ../java ..
	rm -rf ../include ; mkdir ../include
	-./system_ln  ../tmp/conv-*.*h ../include
	-./system_ln  ../tmp/cc-*.*h ../include
	-./system_ln  ../tmp/conv-mach-opt.sh ../include
	-./system_ln  ../tmp/conv-mach-opt.mak ../include
	if [ -x ./special.sh ] ; then SRCBASE=$(SRCBASE) ./special.sh ; fi
	if [ ! -f conv-common.h ] ; then ( touch conv-common.h ) ; fi
	touch dirs+sources

###############################################################################
#
# Converse Libraries
# 
###############################################################################
CLBLIBS=$(L)/libldb-rand.a $(L)/libldb-spray.a   $(L)/libldb-workstealing.a  $(L)/libldb-neighbor.a $(L)/libldb-none.a $(L)/libldb-test.a $(L)/libldb-bluegene.a

TRACELIBS = $(L)/libtrace-converse.a $(L)/libtracef_f.a 

BUILD_TRACING := $(CMK_TRACE_ENABLED)

ifneq ($(BUILD_TRACING),0)

TRACELIBS += \
  $(L)/libtrace-projections.a \
  $(L)/libtrace-controlPoints.a \
  $(L)/libtrace-summary.a \
  $(L)/libtrace-utilization.a \
  $(L)/libtrace-simple.a \
  $(L)/libtrace-counter.a \
  $(L)/libtrace-bluegene.a \
  $(L)/libtrace-projector.a \
  $(L)/libtrace-all.a \
  $(L)/libtrace-memory.a \
  $(L)/libtrace-perfReport.a \

endif

MEMLIBS = \
  $(L)/libmemory-default.a \
  $(L)/libmemory-os.a \
  $(L)/libmemory-os-verbose.a \
  $(L)/libmemory-os-isomalloc.a \
  $(L)/libmemory-os-leak.a \
  $(L)/libmemory-os-paranoid.a \
  $(L)/libmemory-os-lock.a \
  $(L)/libmemory-os-wrapper.a \

ifneq ($(CMK_HAS_MMAP),0)

MEMLIBS += \
  $(L)/libmemory-gnu.a \
  $(L)/libmemory-gnu-verbose.a \
  $(L)/libmemory-gnu-paranoid.a \
  $(L)/libmemory-gnu-leak.a \
  $(L)/libmemory-gnu-isomalloc.a \

endif

ifneq ($(CMK_CHARMDEBUG),0)
ifneq ($(CMK_HAS_MMAP),0)

MEMLIBS += \
  $(L)/libmemory-gnu-charmdebug.a \
  $(L)/libmemory-gnu-charmdebug-mmap.a \
  $(L)/libmemory-gnu-charmdebug-slot.a \
  $(L)/libmemory-gnu-charmdebug-mmap-slot.a \
  $(L)/libmemory-os-charmdebug.a \

ifneq ($(CMK_HAS_MALLOC_HOOK),0)

MEMLIBS += \
  $(L)/libmemory-hooks-charmdebug.a \

endif

endif
endif

ifneq ($(findstring -win64,$(CMK_VDIR))$(findstring -win-,$(CMK_VDIR)),)
  IS_WINDOWS := 1
else
  IS_WINDOWS := 0
endif

THREADLIBS = \
  $(L)/libthreads-default.a \
  $(L)/libthreads-default-tls.a \
  $(L)/libthreads-qt.a \
  $(L)/libthreads-qt-tls.a \
  $(L)/libthreads-uFcontext.a \
  $(L)/libthreads-uFcontext-tls.a \
  $(L)/libthreads-stackcopy.a \

ifneq ($(IS_WINDOWS),0)

THREADLIBS += \
  $(L)/libthreads-fibers.a \

else

THREADLIBS += \
  $(L)/libthreads-context.a \
  $(L)/libthreads-context-tls.a \
  $(L)/libthreads-uJcontext.a \
  $(L)/libthreads-uJcontext-tls.a \
  $(L)/libthreads-pthreads.a \
  $(L)/libthreads-memoryalias.a \
  $(L)/libthreads-qt-memoryalias.a \
  $(L)/libthreads-context-memoryalias.a \
  $(L)/libthreads-uJcontext-memoryalias.a \

endif

CVLIBS=$(L)/libconv-core.a \
	$(L)/conv-static.o \
	$(L)/libconv-machine.a \
	$(L)/libconv-util.a $(L)/libconv-utilf.a \
	$(CLBLIBS) $(TRACELIBS) $(MEMLIBS) $(THREADLIBS) \
	$(L)/libccs-client.a $(L)/libconv-partition.a $(L)/libhwloc_embedded.a \
	$(L)/libconv-ldb.a

LIBCONV_CORE=convcore.o hrctimer.o conv-conds.o conv-taskQ.o queueing.o msgmgr.o \
	cpm.o cpthreads.o futures.o cldb.o random.o \
	debug-conv.o debug-conv++.o conv-ccs.o ccs-builtins.o middle-ccs.o \
	traceCore.o traceCoreCommon.o \
	converseProjections.o machineProjections.o \
	quiescence.o isomalloc.o mem-arena.o memory-darwin-clang.o \
	global-nop.o cmipool.o cpuaffinity.o cputopology.o  \
	cmitls.o memoryaffinity.o commitid.o conv-interoperate.o conv-rdma.o \

LIBCONV_LDB = topology.o generate.o edgelist.o

LIBCONV_UTIL=pup_util.o pup_toNetwork.o pup_toNetwork4.o		\
	pup_xlater.o pup_c.o pup_paged.o pup_cmialloc.o			\
	ckimage.o ckdll.o ckhashtable.o sockRoutines.o			\
	conv-lists.o persist-comm.o mempool.o \
	crc32.o  lz4.o partitioning_strategies.o hilbert.o \
	spanningTree.o cmirdmautils.o

LIBCONV_UTILF=pup_f.o

include Makefile.machine

converse: charmrun-target swapglobal-target conv-cpm tmgr hwloc-target

cpuaffinity.o $(L)/libhwloc_embedded.a $(INC)/hwloc.h $(INC)/hwloc/autogen/config.h $(INC)/hwloc/rename.h $(INC)/hwloc/bitmap.h $(INC)/hwloc/helper.h $(INC)/hwloc/inlines.h $(INC)/hwloc/diff.h $(INC)/hwloc/deprecated.h: hwloc-target

hwloc-target:  conv-autoconfig.h
	( $(MAKE) -C hwloc )
	( test -e hwloc/hwloc/.libs/libhwloc_embedded.lib && cp -f hwloc/hwloc/.libs/libhwloc_embedded.lib hwloc/hwloc/.libs/libhwloc_embedded.a ) || true
	( cp -f hwloc/hwloc/.libs/libhwloc_embedded.a $(L) )
	( cp -f hwloc/include/hwloc.h $(INC) )
	( cp -LRf hwloc/include/hwloc $(INC) )
ifeq (false,$(CMK_NO_BUILD_SHARED))
	( test -d ../lib_so && cp -f hwloc/hwloc/.libs/libhwloc_embedded.*$(CMK_SHARED_SUF)* ../lib_so/ ) || true
endif
	touch $@

charmrun-target: hwloc-target charmrun  sockRoutines.C sockRoutines.h ccs-server.C ccs-server.h sockRoutines-seq.o $(L)/libck.a $(L)/libhwloc_embedded.a $(CVLIBS) conv-static.o
	if [ -f charmrun ] ; then ( cp -f charmrun ../bin ) ; fi

QUICK_THREADS: $(CVHEADERS)
	-( $(MAKE) -C QuickThreads clean )
	( cd QuickThreads;./configure )
	( $(MAKE) -C QuickThreads qt  OPTS='$(OPTS) -Wno-error' ) || ( echo "Did you try to build netlrts-linux on a 64-bit system? Try netlrts-linux-x86_64" && false )
	cp -f QuickThreads/libckqt.a ../lib/
ifeq (false,$(CMK_NO_BUILD_SHARED))
	-( test -d ../lib_so && cp -f QuickThreads/libckqt.$(CMK_SHARED_SUF) ../lib_so/ ) || true
endif
	touch QUICK_THREADS

CONVLIBS: converse
	$(MAKE) -C libs convlibs

$(L)/libconv-core.a: $(LIBCONV_CORE)
	$(CHARMC) $(TAU_LIBS) -o $@ $(LIBCONV_CORE) 

$(L)/libconv-partition.a: custom_partitioner.o set_partition_params.o
	$(CHARMC) -o $@ $^

$(L)/conv-static.o: conv-static.o
	cp -f $< $@

$(L)/libconv-machine.a: machine.o
	$(CHARMC) -o $@ $<

machine.o: machine.C $(CVHEADERS)
	$(CHARMC) -I. -o $@ $<

$(L)/libconv-util.a: $(LIBCONV_UTIL)
	$(CHARMC) -o $@ $(LIBCONV_UTIL)

$(L)/libconv-utilf.a: $(LIBCONV_UTILF)
	-$(CHARMC) -o $@ $(LIBCONV_UTILF) || touch $@

$(L)/libconv-ldb.a: $(LIBCONV_LDB)
	$(CHARMC) -o $@ $^

$(L)/libccs-client.a: ccs-client.o
	$(CHARMC) -o $@ ccs-client.o

ccs-client.o: ccs-client.C $(CVHEADERS)
	$(CHARMC) -DCMK_NOT_USE_CONVERSE=1 ccs-client.C

sockRoutines-seq.o: sockRoutines.C $(CVHEADERS) $(UTILHEADERS)
	$(SEQCHARMC) -DCMK_NOT_USE_CONVERSE=1 -o sockRoutines-seq.o sockRoutines.C

pup_f.f90: pup_f.f90.sh
	rm -f pup_f.f90 && ./pup_f.f90.sh

pup_f.o: pup_f.f90 $(CVHEADERS)
	-$(CHARMC) -c pup_f.f90 && $(CHARMC) -cpmod ../include pupmod.M || touch pup_f.o

## Converse load balancers (seed balancers, -balance)
$(L)/libldb-none.a: cldb.none.C $(CVHEADERS)
	$(CHARMC) -o $@ cldb.none.C

$(L)/libldb-rand.a: cldb.rand.C cldb.h $(CVHEADERS)
	$(CHARMC) -o $@ cldb.rand.C

$(L)/libldb-neighbor.a: cldb.neighbor.C cldb.neighbor.h $(CVHEADERS)
	$(CHARMC) -o $@ cldb.neighbor.C

$(L)/libldb-workstealing.a: cldb.workstealing.C cldb.workstealing.h $(CVHEADERS)
	$(CHARMC) -o $@ cldb.workstealing.C

$(L)/libldb-spray.a: cldb.spray.C $(CVHEADERS)
	$(CHARMC) -o $@ cldb.spray.C


#$(L)/libldb-prioritycentralized.a: cldb.prioritycentralized.C cldb.prioritycentralized.h  $(CVHEADERS)
#	$(CHARMC) -o $@ cldb.prioritycentralized.C

#//$(L)/libldb-prioritycentralizedopt.a: cldb.prioritycentralizedopt.c cldb.prioritycentralizedopt.h  $(CVHEADERS)
#//	$(CHARMC) -o $@ cldb.prioritycentralizedopt.c 

$(L)/libldb-test.a: cldb.test.C $(CVHEADERS)
	$(CHARMC) -o $@ cldb.test.C

$(L)/libldb-bluegene.a: cldb.bluegene.C $(CVHEADERS)
	$(CHARMC) -o $@ cldb.bluegene.C

## Memory allocation libraries (-memory)
MEM_DEPS=memory-gnu.C memory-gnu-internal.C converse.h conv-mach.h $(CVHEADERS)

# $1 is the name of the module, $2 is any additional dependency beyond memory.C and $(MEM_DEPS), $3 is extra arguments to charmc
define libmem
$(L)/libmemory-$1.a: memory.C $$(MEM_DEPS) $2
	$$(CHARMC) -I. -o libmemory-$1.o $3 $$<
	$$(CHARMC) -I. -o $$@ $3 libmemory-$1.o
endef

$(eval $(call libmem,default,                           ,-DCMK_MEMORY_BUILD_DEFAULT))
$(eval $(call libmem,os,                                ,-DCMK_MEMORY_BUILD_OS))
$(eval $(call libmem,os-verbose,    memory-verbose.C,    -DCMK_MEMORY_BUILD_OS_WRAPPED -DCMK_MEMORY_BUILD_VERBOSE    -touch-on-failure))
$(eval $(call libmem,os-paranoid,   memory-paranoid.C,   -DCMK_MEMORY_BUILD_OS_WRAPPED -DCMK_MEMORY_BUILD_PARANOID   -touch-on-failure))
$(eval $(call libmem,os-leak,       memory-leak.C,       -DCMK_MEMORY_BUILD_OS_WRAPPED -DCMK_MEMORY_BUILD_LEAK       -touch-on-failure))
$(eval $(call libmem,os-isomalloc,  memory-isomalloc.C,  -DCMK_MEMORY_BUILD_OS_WRAPPED -DCMK_MEMORY_BUILD_ISOMALLOC  -touch-on-failure))
$(eval $(call libmem,os-lock,       memory-lock.C,       -DCMK_MEMORY_BUILD_OS_WRAPPED -DCMK_MEMORY_BUILD_LOCK       -touch-on-failure))
$(eval $(call libmem,os-charmdebug, memory-charmdebug.C, -DCMK_MEMORY_BUILD_OS_WRAPPED -DCMK_MEMORY_BUILD_CHARMDEBUG -touch-on-failure))
$(L)/libmemory-os-wrapper.a: memory-os-wrapper.C $(MEM_DEPS)
	-$(CHARMC) -o $@ memory-os-wrapper.C || touch $@

$(eval $(call libmem,hooks-charmdebug,,-DCMK_MEMORY_BUILD_GNU_HOOKS -DCMK_MEMORY_BUILD_CHARMDEBUG -touch-on-failure))

# If the system doesn't have sbrk, these compilations may fail.
#  This is OK, but then we can't use "-memory gnu" or friends.
$(eval $(call libmem,gnu,                                        ,-DCMK_MEMORY_BUILD_GNU -touch-on-failure))
$(eval $(call libmem,gnu-verbose,             memory-verbose.C   ,-DCMK_MEMORY_BUILD_VERBOSE -touch-on-failure))
$(eval $(call libmem,gnu-paranoid,            memory-paranoid.C  ,-DCMK_MEMORY_BUILD_PARANOID -touch-on-failure))
$(eval $(call libmem,gnu-leak,                memory-leak.C      ,-DCMK_MEMORY_BUILD_LEAK -touch-on-failure))
$(eval $(call libmem,gnu-isomalloc,           memory-isomalloc.C ,-DCMK_MEMORY_BUILD_ISOMALLOC -touch-on-failure))
$(eval $(call libmem,gnu-charmdebug,          memory-charmdebug.C,-DCMK_MEMORY_BUILD_CHARMDEBUG -optimize -touch-on-failure))
$(eval $(call libmem,gnu-charmdebug-slot,     memory-charmdebug.C,-DCMK_MEMORY_BUILD_CHARMDEBUG -DCMK_SEPARATE_SLOT -optimize -touch-on-failure))
$(eval $(call libmem,gnu-charmdebug-mmap,     memory-charmdebug.C,-DCMK_MEMORY_BUILD_CHARMDEBUG -DCPD_USE_MMAP -optimize -touch-on-failure))
$(eval $(call libmem,gnu-charmdebug-mmap-slot,memory-charmdebug.C,-DCMK_MEMORY_BUILD_CHARMDEBUG -DCPD_USE_MMAP -DCMK_SEPARATE_SLOT -optimize -touch-on-failure))

## Thread libraries (-thread)

LIBTHREADSDEPS = $(CVHEADERS) QUICK_THREADS

# $1 is the name of the module, $2 is any additional dependency beyond threads.C and $(LIBTHREADSDEPS), $3 is extra arguments to charmc, $4 is an option to include 'boost-context'
define libthreads
$(L)/libthreads-$1.a: threads.C $$(LIBTHREADSDEPS) $2
	$$(CHARMC) -IQuickThreads -I. -o libthreads-$1.o $3 $$<
ifeq (1,$$(strip $4))
	$$(CHARMC) -IQuickThreads -I. -o $$@ $3 $$(wildcard boost-context/*.o) libthreads-$1.o
else
	$$(CHARMC) -IQuickThreads -I. -o $$@ $3 libthreads-$1.o
endif
endef

uFcontext_asm:
	$(MAKE) -C boost-context
	touch uFcontext_asm

$(eval $(call libthreads,default,            uFcontext_asm,-DCMK_THREADS_BUILD_DEFAULT=1, 1 ))
$(eval $(call libthreads,default-tls,        uFcontext_asm,-DCMK_THREADS_BUILD_DEFAULT -DCMK_THREADS_BUILD_TLS=1 -touch-on-failure, 1 ))
$(eval $(call libthreads,qt,                              ,-DCMK_THREADS_BUILD_QT=1, 0))
$(eval $(call libthreads,qt-tls,                          ,-DCMK_THREADS_BUILD_QT=1 -DCMK_THREADS_BUILD_TLS=1 -touch-on-failure, 0))
$(eval $(call libthreads,context,                         ,-DCMK_THREADS_BUILD_CONTEXT=1 -touch-on-failure, 0))
$(eval $(call libthreads,context-tls,                     ,-DCMK_THREADS_BUILD_CONTEXT=1 -DCMK_THREADS_BUILD_TLS=1 -touch-on-failure, 0))
$(eval $(call libthreads,uJcontext,            uJcontext.C,-DCMK_THREADS_BUILD_JCONTEXT=1 -U_FORTIFY_SOURCE -touch-on-failure,0))
$(eval $(call libthreads,uJcontext-tls,        uJcontext.C,-DCMK_THREADS_BUILD_JCONTEXT=1 -DCMK_THREADS_BUILD_TLS=1 -U_FORTIFY_SOURCE -touch-on-failure,0))
$(eval $(call libthreads,uFcontext,          uFcontext_asm,-DCMK_THREADS_BUILD_FCONTEXT=1 -U_FORTIFY_SOURCE -touch-on-failure, 1))
$(eval $(call libthreads,uFcontext-tls,      uFcontext_asm,-DCMK_THREADS_BUILD_FCONTEXT=1 -DCMK_THREADS_BUILD_TLS=1 -U_FORTIFY_SOURCE -touch-on-failure, 1))
$(eval $(call libthreads,pthreads,                        ,-DCMK_THREADS_BUILD_PTHREADS=1 -touch-on-failure,0))
$(eval $(call libthreads,fibers,                          ,-DCMK_THREADS_BUILD_FIBERS=1 -touch-on-failure,0))
$(eval $(call libthreads,stackcopy,                       ,-DCMK_THREADS_BUILD_STACKCOPY=1 -touch-on-failure,0))
$(eval $(call libthreads,memoryalias,                     ,-DCMK_THREADS_BUILD_DEFAULT=1 -DCMK_THREADS_ALIAS_STACK=1 -touch-on-failure,0))
$(eval $(call libthreads,qt-memoryalias,                  ,-DCMK_THREADS_BUILD_QT=1 -DCMK_THREADS_ALIAS_STACK=1 -touch-on-failure,0))
$(eval $(call libthreads,context-memoryalias,             ,-DCMK_THREADS_BUILD_CONTEXT=1 -DCMK_THREADS_ALIAS_STACK=1 -touch-on-failure,0))
$(eval $(call libthreads,uJcontext-memoryalias,uJcontext.C,-DCMK_THREADS_BUILD_JCONTEXT=1 -DCMK_THREADS_ALIAS_STACK=1 -U_FORTIFY_SOURCE -touch-on-failure,0))

## Global swapping (-swapglobal)
swapglobal-target: $(L)/libglobal-swap.a

$(L)/libglobal-swap.a: global-elfgot.C $(CVHEADERS)
	-$(CHARMC) -c global-elfgot.C -o $@ || touch $@

###############################################################################
#
# Charm Libraries
#
###############################################################################

CK_LIBS_CORE=$(L)/libck.a

LIBCK_CORE=trace-common.o tracec.o tracef.o init.o register.o qd.o ck.o \
	   msgalloc.o ckfutures.o ckIgetControl.o debug-message.o debug-charm.o ckcallback.o \
	   cklocation.o ckmulticast.o ckarrayoptions.o ckarray.o ckreduction.o ckrdma.o\
           waitqd.o LBDatabase.o MetaBalancer.o weakTest.o treeTest.o forestTest.o readmodel.o lbdbf.o ckobjQ.o  \
	   ckcheckpoint.o ckmemcheckpoint.o ckevacuation.o ckobjid.o\
           LBComm.o LBObj.o LBMachineUtil.o CentralPredictor.o \
	   BaseLB.o CentralLB.o HybridBaseLB.o DistBaseLB.o \
           ckgraph.o LButil.o RefinerTemp.o Refiner.o \
           manager.o ckset.o ckheap.o \
	   NullLB.o LBSimulation.o modifyScheduler.o \
	   charmProjections.o cktiming.o ckbitvector.o \
           pathHistory.o controlPoints.o arrayRedistributor.o cp_effects.o \
           trace-controlPoints.o mpi-interoperate.o ckregex.o sdag.o \

charm-core: converse $(L)/libck.a $(L)/libckf.a $(L)/libckmain.a $(L)/libckmainf.a
charm-target: loadbalancers default_libs $(L)/libmpi-mainmodule.a tmgr

CHARMLIBS: charm++ CONVLIBS $(BIGSIM)
	$(MAKE) -C libs charmlibs

$(L)/libck.a: $(LIBCK_CORE) 
	$(CHARMC) -o $@ $(LIBCK_CORE)

## Charm fortran AMPI

CK_FOR_LIBS_CORE=$(L)/libckf.a

LIBCKF_CORE=charmf.o charmmod.o

$(L)/libckf.a: $(LIBCKF_CORE)
	-$(CHARMC) -o $@ $(LIBCKF_CORE) || touch $@

charmf.o:  charmf.C $(CKHEADERS) charmf.h
	$(CHARMC) -c -I. charmf.C

charmmod.o: charmmod.f90
	-$(CHARMC) -c charmmod.f90 || touch $@
	-$(CHARMC) -cpmod ../include charm.M

mainf.o: mainf.f90
	-$(CHARMC) -o $@ -c $< || touch $@

## Tracing libraries (profiling, -tracemode)
LIBTRACE_PROJ=trace-projections.o
$(L)/libtrace-projections.a: $(LIBTRACE_PROJ)
	$(CHARMC) -o $@ $(LIBTRACE_PROJ)

LIBTRACE_CP=trace-controlPoints.o
$(L)/libtrace-controlPoints.a: $(LIBTRACE_CP)
	$(CHARMC) -o $@ $(LIBTRACE_CP)

LIBTRACE_AP=trace-perf.o picsautoperf.o picsautoperfAPI.o picstreenode.o picsdecisiontree.o
$(L)/libtrace-perfReport.a: $(LIBTRACE_AP)
	$(CHARMC) -o $@ $(LIBTRACE_AP)

LIBTRACE_SUMM=trace-summary.o
$(L)/libtrace-summary.a: $(LIBTRACE_SUMM)
	$(CHARMC) -o $@ $(LIBTRACE_SUMM)

LIBTRACE_UTIL=trace-utilization.o
$(L)/libtrace-utilization.a: $(LIBTRACE_UTIL)
	$(CHARMC) -o $@ $(LIBTRACE_UTIL)

LIBTRACE_SIMPLE=trace-simple.o
$(L)/libtrace-simple.a: $(LIBTRACE_SIMPLE)
	$(CHARMC) -o $@ $(LIBTRACE_SIMPLE)

libtrace-Tau.o: trace-Tau.C charm++.h charm.h converse.h conv-config.h \
  conv-autoconfig.h conv-common.h conv-mach.h conv-mach-opt.h \
  conv-mach-ifort.h pup_c.h conv-cpm.h conv-cpath.h conv-qd.h \
  pup_c_functions.h \
  conv-random.h conv-lists.h conv-trace.h persistent.h debug-conv.h pup.h \
  middle.h middle-conv.h cklists.h ckbitvector.h ckstream.h init.h \
  ckhashtable.h debug-charm.h CkMarshall.decl.h cksection.h ckcallback.h \
  conv-ccs.h sockRoutines.h ccs-server.h ckobjQ.h ckreduction.h \
  CkReduction.decl.h \
  ckmemcheckpoint.h CkMemCheckpoint.decl.h readonly.h ckarray.h cklocrec.h ckmigratable.h \
  cklocation.h ckmulticast.h LBDatabase.h MetaBalancer.h RandomForestModel.h lbdb.h LBObj.h LBOM.h LBComm.h \
  LBMachineUtil.h LBDatabase.decl.h MetaBalancer.decl.h NullLB.decl.h BaseLB.decl.h \
  CkLocation.decl.h CkMulticast.decl.h CkArray.decl.h objid.h envelope.h envelope-path.h XArraySectionReducer.h CkFutures.decl.h \
  waitqd.h waitqd.decl.h \
  sdag.h ckcheckpoint.h CkCheckpoint.decl.h ckevacuation.h \
  trace.h trace-bluegene.h trace-simple.h \
  register.h trace-common.h trace-simpleBOC.h TraceSimple.decl.h \
  TraceSimple.def.h ckregex.h
	$(CHARMC) -c -I. $(TAU_INCLUDE) $(TAU_DEFS) trace-Tau.C -o libtrace-Tau.o

tau_selective.o: $(TAUROOT)/utils/tau_selective.cpp 
	$(CHARMC) -I$(TAUROOT)/utils -DTAU_DYNINST $(TAUROOT)/utils/tau_selective.cpp

$(L)/libtrace-Tau.a: $(TAU_TRACE_OBJ) tau_selective.o
	$(CHARMC) $(TAU_LIBS) -o $@ $(TAU_TRACE_OBJ) tau_selective.o $(TAU_TRACE_OBJ)

LIBTRACE_COUNT=trace-counter.o
$(L)/libtrace-counter.a: $(LIBTRACE_COUNT)
	$(CHARMC) -o $@ $(LIBTRACE_COUNT)

LIBTRACE_BG=trace-bluegene.o
$(L)/libtrace-bluegene.a:$(LIBTRACE_BG)
	$(CHARMC) -o $@ $(LIBTRACE_BG)

LIBTRACE_MEMORY=trace-memory.o
$(L)/libtrace-memory.a: $(LIBTRACE_MEMORY)
	$(CHARMC) -o $@ $(LIBTRACE_MEMORY)

LIBTRACE_ALL=trace-all.o trace-projections.o trace-controlPoints.o picstreenode.o picsdecisiontree.o picsautoperfAPI.o picsautoperf.o trace-perf.o trace-summary.o trace-simple.o  \
$(TAU_TRACE_OBJ) trace-projector.o traceCore.o traceCoreCommon.o charmProjections.o converseProjections.o machineProjections.o trace-memory.o trace-utilization.o

$(L)/libtrace-all.a: $(LIBTRACE_ALL)
	$(CHARMC) -o $@ $(LIBTRACE_ALL)

LIBTRACE_PROJECTOR=trace-projector.o traceCore.o traceCoreCommon.o charmProjections.o converseProjections.o machineProjections.o
$(L)/libtrace-projector.a: $(LIBTRACE_PROJECTOR)
	$(CHARMC) -o  $@ $(LIBTRACE_PROJECTOR)

LIBTRACE_CONVERSE=trace-converse.o
$(L)/libtrace-converse.a: $(LIBTRACE_CONVERSE)
	$(CHARMC) -o  $@ $(LIBTRACE_CONVERSE)

LIBTRACE_FOR=tracef_f.o
$(L)/libtracef_f.a: $(LIBTRACE_FOR)
	-$(CHARMC) -o $@ $(LIBTRACE_FOR) || touch $@

tracef_f.o: tracef_f.f90
	-$(CHARMC) -o $@ -c tracef_f.f90 && $(CHARMC) -cpmod ../include tracemod.M  || touch $@

# used for make depends
TRACE_OBJS =  trace-projections.o trace-controlPoints.o picstreenode.o picsdecisiontree.o trace-perf.o picsautoperfAPI.o picsautoperf.o trace-summary.o  trace-simple.o \
	      trace-counter.o trace-utilization.o	\
	      trace-bluegene.o trace-projector.o trace-converse.o trace-all.o \
          trace-memory.o 

###############################################################################
#
# Dynamic Load Balancers
#
###############################################################################

loadbalancers: $(COMMON_LDBS) $(L)/libmoduleCommonLBs.a

everylb: $(ALL_LDBS) $(L)/libmoduleEveryLB.a
MetisLB.o GridMetisLB.o: metis

%LB: $(L)/libmodule%LB.a
	@true
$(L)/libmodule%LB.a: %LB.o
	$(CHARMC) -o $@ $^
	( [ -r libmodule$*LB.dep ] && cp -f libmodule$*LB.dep $(L)/ ) || true
$(L)/libmoduleCommonLBs.a:
	$(CHARMC) -o $@ $^
	( [ -r libmoduleCommonLBs.dep ] && cp -f libmoduleCommonLBs.dep $(L)/ ) || true

###############################################################################
#
# BlueGene Emulator
#
###############################################################################

bigsim:  bigsim-target

bigemulator: bigsim

bigsim-target: charm++ $(L)/libconv-bigsim.a $(L)/libconv-bigsim-logs.a $(L)/libconv-bluegene-stub.a $(L)/libblue-standalone.a

$(L)/libbluegene.a: BlueGene.o
	$(CHARMC) -o $@ $^

# (Converse) Blue Gene emulator
BLUE_OBJS = blue.o bigsim_node.o bigsim_proc.o bigsim_init.o bigsim_timing.o bigsim_record.o bigsim_ooc.o shared_alloc.o
BIGSIM_IO_OBJS = bigsim_read.o bigsim_logs.o bigsim_api.o

LIBBLUE_OBJS = $(BLUE_OBJS) $(BIGSIM_IO_OBJS) blue_stub.o blue_standalone.o

$(L)/libconv-bigsim.a: $(BLUE_OBJS)
	$(CHARMC) -o $@ $^

$(L)/libconv-bigsim-logs.a: $(BIGSIM_IO_OBJS)
	$(CHARMC) -o $@ $^

$(L)/libconv-bluegene-stub.a:  blue_stub.o
	$(CHARMC) -o $@ $^

$(L)/libblue-standalone.a: blue_standalone.o
	$(CHARMC) -o $@ $^

###############################################################################
#
# The CPM scanner
#
###############################################################################

conv-cpm: conv-cpm.o
	$(NATIVECHARMC) -language c++ -o conv-cpm -cp ../bin/ conv-cpm.o

conv-cpm.o: conv-cpm.C $(CVHEADERS)
	$(NATIVECHARMC) conv-cpm.C

###############################################################################
#
# The interface translator
#
###############################################################################

XIHEADERS=conv-autoconfig.h xi-AstNode.h xi-Construct.h xi-grammar.tab.h xi-Message.h xi-Parameter.h xi-SdagConstruct.h xi-BlockConstruct.h xi-Template.h xi-util.h xi-Chare.h xi-Entry.h xi-Member.h xi-Module.h xi-SdagCollection.h xi-symbol.h xi-Type.h xi-Value.h xi-main.h
CHARMXIOBJ=xi-main.o xi-grammar.tab.o xi-scan.o xi-Template.o xi-AstNode.o xi-Entry.o xi-Member.o xi-Parameter.o xi-SdagConstruct.o xi-BlockConstruct.o xi-Type.o xi-Chare.o xi-Message.o xi-Construct.o xi-Module.o xi-SdagCollection.o xi-Value.o xi-util.o
SDAGOBJ=sdag-globals.o CSdagConstruct.o CEntry.o CParsedFile.o CStateVar.o Serial.o Case.o CaseList.o Else.o Forall.o For.o If.o IntExpr.o OList.o Overlap.o SdagEntry.o SList.o Template.o When.o While.o
SDAGHEADERS= CToken.h EToken.h CParsedFile.h sdag-globals.h CEntry.h Serial.h Case.h CaseList.h Else.h Forall.h For.h If.h IntExpr.h OList.h Overlap.h SdagEntry.h SList.h Template.h When.h While.h Constructs.h

$(CHARMXIOBJ) $(SDAGOBJ): $(XIHEADERS) $(SDAGHEADERS)
$(CHARMXIOBJ) $(SDAGOBJ): CXX=$(NATIVECHARMC)
$(CHARMXIOBJ) $(SDAGOBJ): CXXFLAGS=-I$(SRCBASE)/xlat-i/ -I$(SRCBASE)/xlat-i/sdag/ -I.
xi-scan.o: CXXFLAGS+=-DYY_NEVER_INTERACTIVE
VPATH=$(SRCBASE)/xlat-i:$(SRCBASE)/xlat-i/sdag:$(SRCBASE)/xlat-i/sdag/constructs

charmxi: ../bin/charmxi

../bin/charmxi: $(CHARMXIOBJ) $(SDAGOBJ)
	$(NATIVECHARMC) -language c++ -cp ../bin/ -o charmxi $^

xi-clean:
	rm $(CHARMXIOBJ) $(SDAGOBJ)

###############################################################################
#
# Simple Messaging
#
###############################################################################

$(L)/libtsm.a: converse simplemsg.C simplemsg.h
	$(CHARMC) -o $@ simplemsg.C

$(L)/libsm.a: converse sm.C sm.h
	$(CHARMC) -o $@ sm.C

###############################################################################
#
# PVM
#
###############################################################################

PVMC=pvmc_buf.o pvmc_comm.o pvmc_conv.o pvmc_main.o pvmc_pack.o pvmc_groups.o

$(L)/libpvmc.a: $(PVMC)
	$(CHARMC) -o $@ $(PVMC)

pvmc_buf.o: pvmc_buf.C pvmc.h pvm3.h converse.h
	$(CHARMC) -o $@ pvmc_buf.C

pvmc_conv.o: pvmc_conv.C pvmc.h pvm3.h converse.h
	$(CHARMC) -o $@ pvmc_conv.C

pvmc_pack.o: pvmc_pack.C pvmc.h pvm3.h converse.h
	$(CHARMC) -o $@ pvmc_pack.C

pvmc_comm.o: pvmc_comm.C pvmc.h pvm3.h converse.h
	$(CHARMC) -o $@ pvmc_comm.C

pvmc_groups.o: pvmc_groups.C pvmc.h pvm3.h converse.h
	$(CHARMC) -o $@ pvmc_groups.C

pvmc_main.o: pvmc_main.C pvmc.h pvm3.h converse.h
	$(CHARMC) -o $@ pvmc_main.C

$(L)/libf90charm.a: charmxi f90main.ci f90main.h f90main.C
	$(CHARMC) f90main.ci
	$(CHARMC) -c -DAMPI_FORTRAN=1 f90main.C
	-$(CHARMC) -o $@ f90main.o

##############################################################################
# adding this entry for files which do not belong to any of other locations

OTHER_OBJS= mpi-mainmodule.o main.o custom_partitioner.o BlueGene.o set_partition_params.o

##############################################################################

###############################################################################
#
# Make clean
#
###############################################################################

install:
	@test ! -d $(DESTDIR)/include && mkdir -p $(DESTDIR)/include || true
	@test ! -d $(DESTDIR)/lib && mkdir -p $(DESTDIR)/lib || true
ifeq (false,$(CMK_NO_BUILD_SHARED))
	@test ! -d $(DESTDIR)/lib_so && mkdir -p $(DESTDIR)/lib_so || true
endif
	@test ! -d $(DESTDIR)/bin && mkdir -p $(DESTDIR)/bin || true
	# cp -f ../include/*   $(DESTDIR)/include
	for f in ../include/*; do if test -f $$f; then cp -f $$f $(DESTDIR)/include; fi; done
	cp -f ../lib/*       $(DESTDIR)/lib
ifeq (false,$(CMK_NO_BUILD_SHARED))
	cp -f ../lib_so/*    $(DESTDIR)/lib_so || true
endif
#	cp -f ../bin/*       $(DESTDIR)/bin
	for f in ../bin/*; do if test -f $$f; then cp -f $$f $(DESTDIR)/bin; fi; done


clean:
	rm -f conv-autoconfig.h config.cache
	rm -f QuickThreads/libckqt.a
	rm -f charmxi conv-cpm
	rm -f TAGS basics cmk_extras core
	rm -f core *.a
	rm -f core *.o
	rm -f core #*#
	rm -f core *~
	rm -f *.stamp
	if [ -d charmrun-src ] ; then ( $(MAKE) -C charmrun-src clean ) ; fi
	( $(MAKE) -C libs clean )
	( $(MAKE) -C ../benchmarks clean )
	( $(MAKE) -C ../examples clean )
	( $(MAKE) -C ../tests clean )

veryclean:
	rm -rf ../bin ../lib ../src ../benchmarks ../examples ../tests ../include
	mv Makefile .Makefile
	mv Make.depends .Make.depends
	rm -rf [a-z]* [A-Z]*
	mv .Make.depends Make.depends
	rm -rf .[a-z]* .[A-Z]*


config: configure conv-autoconfig.h.in

cidepends: charmxi
	echo "Creating " $(CIDEPENDFILE) " ...";
	if [ -f $(CIDEPENDFILE) ]; then \
           /bin/cp -f $(CIDEPENDFILE) $(CIDEPENDFILE).old; \
        fi; \
	echo '#generated by make cidepends' > $(CIDEPENDFILE); \
	for cifile in $(sort $(wildcard *.ci)); do \
              echo "checking generated modules for $$cifile" ; \
              $(CHARMXI) -M  $$cifile | sed '/\.stamp:/a \\t $$(CHARMC) -intrinsic $$< && touch $$@' >> $(CIDEPENDFILE) ; \
        done

depends: cidepends commitid.C
	$(MAKE) basics
	echo "Creating " $(DEPENDFILE) " ...";
	if [ -f $(DEPENDFILE) ]; then \
           /bin/cp -f $(DEPENDFILE) $(DEPENDFILE).old; \
        fi; \
	echo '#generated by make depends' > $(DEPENDFILE); \
	processFiles=''; 	\
        for i in $(sort $(LIBCONV_CORE) $(LIBCONV_LDB) $(LIBCONV_UTIL) $(TRACE_OBJS) $(LIBCK_CORE) $(ALL_LB_OBJS) $(LIBBLUE_OBJS) $(OTHER_OBJS)) ; do \
	      processed='false';	\
	      for f in $$processFiles; do 	\
		if test $$i = $$f ; then	\
		  processed=true;  break;	\
		fi;	\
	      done;	\
	      test $$processed = 'true'  && continue;	\
	      processFiles="$$processFiles $$i";	\
	      SRCFILE=`basename $$i .o`.C ; \
	      [ ! -f $$SRCFILE ] && SRCFILE=`basename $$i .o`.c ;	\
              echo "checking dependencies for $$SRCFILE" ; \
              if g++ -std=c++0x -MM -Wno-deprecated -I. -I$(INC) $$SRCFILE  >> $(DEPENDFILE); then true ; else echo '' ; echo "Compilation of '$$SRCFILE' failed, please fix it first!!!!" ; exit; fi;  \
	      echo '' >> $(DEPENDFILE) ; \
        done;  \

dependsCaseSensitive: cidepends
	$(MAKE) basics
	@echo "Creating " $(DEPENDFILE) " ...";
	if [ -f $(DEPENDFILE) ]; then \
           /bin/cp -f $(DEPENDFILE) $(DEPENDFILE).old; \
        fi; \
	echo '#generated by make depends' > $(DEPENDFILE); \
	processFiles=''; 	\
        for i in $(sort $(LIBCONV_CORE) $(LIBCONV_UTIL) $(TRACE_OBJS) $(LIBCK_CORE) $(ALL_LB_OBJS) $(LIBBLUE_OBJS) $(OTHER_OBJS)); do \
	      processed='false';	\
	      for f in $$processFiles; do 	\
		if test $$i = $$f ; then	\
		  processed=true;  break;	\
		fi;	\
	      done;	\
	      test $$processed = 'true'  && continue;	\
	      processFiles="$$processFiles $$i";	\
	      SRCFILE=`basename $$i .o`.C ; \
	      SRCDIR=`dirname $$i` ; \
	      found=`/usr/bin/find $$SRCDIR -depth 1 -name $$SRCFILE`; \
              [ ! $$found ] && SRCFILE=`basename $$i .o`.c ; \
              echo "checking dependencies for $$SRCFILE" ; \
              if g++ -std=c++0x -MM -Wno-deprecated -I. -I$(INC) $$SRCFILE  >> $(DEPENDFILE); then true ; else echo '' ; echo "Compilation of '$$SRCFILE' failed, please fix it first!!!!" ; exit; fi;  \
	      echo '' >> $(DEPENDFILE) ; \
        done;  \

# If OPTS was not defined, use the build time options
export OPTS ?= $(OPTSATBUILDTIME)
ifeq ($(strip $(OPTS)),)
$(warning Variable OPTS is defined to an empty string. Are you sure this is what you want?)
endif


-include machine.d
machine.o: machine.d
machine.d: machine.C conv-autoconfig.h $(CKHEADERS)
	@set -e; \
	rm -f $@; \
	$(CHARMC) $(OPTS) -M -I. -I$(INC) $< >$@.$$$$; \
	sed -e 's,\($*\)\.o[ :]*,\1.o $@ : ,g' -e "s,[Cc]:/,`command -v cygpath >/dev/null 2>&1 && cygpath -u 'C:/' || echo '/c'`/," < $@.$$$$ > $@; \
	rm -f $@.$$$$

# Dummy target to allow recursive 'make am--refresh' from hwloc's Makefile
am--refresh:
