-include ../../common.mk
-include ../../../include/conv-mach-opt.mak
CHARMBASE=../../../
CHARMC=../../../bin/ampicxx $(OPTS)
TOKENS=6

-include $(CHARMBASE)/include/conv-mach-opt.mak

all: ampi

AMPI_TARGETS := \
  jacobi \
  jacobi.pup \
  jacobi-get \

ifeq (1,$(CMK_SUPPORTS_TLSGLOBALS))
  AMPI_TARGETS += jacobi.tls
endif

ifeq (1,$(CMK_SUPPORTS_MEMORY_ISOMALLOC))
  AMPI_TARGETS += jacobi.iso
endif

ampi: $(AMPI_TARGETS)

mpi: jacobi.C
	mpiCC -o jacobi jacobi.C  $(OPTS)

jacobi-cpp: jacobi-cpp.C
	$(CHARMC) -c jacobi-cpp.C 
	$(CHARMC) -o $@ jacobi-cpp.o -lm

jacobi.o: jacobi.C
	$(CHARMC) -c jacobi.C 

jacobi: jacobi.o
	$(CHARMC) -o $@ $^ -lm

jacobi.pup: jacobi.o
	$(CHARMC) -o $@ $^ -memory default -lm

jacobi.iso: jacobi.C
	$(CHARMC) -c -DNO_PUP jacobi.C -o jacobi.iso.o
	$(CHARMC) -o $@ jacobi.iso.o -memory isomalloc

jacobi.tls: jacobi.C
	$(CHARMC) -c -tlsglobals jacobi.C -o jacobi.tls.o
	$(CHARMC) -o jacobi.tls jacobi.tls.o -tlsglobals

jacobi.rose: jacobi.C
	$(CHARMC) -roseomptlsglobals -o jacobi.rose.o -c $<
	$(CHARMC) -roseomptlsglobals -o $@ jacobi.rose.o

jacobi-get: jacobi-get.C
	$(CHARMC) -c jacobi-get.C
	$(CHARMC) -o $@ jacobi-get.o -lm

jacobi-get-tokens:
	cd $(CHARMBASE)/tmp; touch machine.c; touch ckfutures.C; make AMPI OPTS="-DIGET_FLOWCONTROL=1 -DIGET_TOKENNUM=$(TOKENS)"; cd -; 
	make jacobi-get;

test: ampi
	$(call run, +p3 ./jacobi.pup 2 2 2 40 +vp8 +balancer RotateLB +LBDebug 1 )
	$(call run, +p3 ./jacobi 2 2 2 40 +vp8 +balancer RotateLB +LBDebug 1 )
	$(call run, +p3 ./jacobi-get 2 2 2 40 +vp8 +balancer RotateLB +LBDebug 1 )
ifeq (1,$(CMK_SUPPORTS_TLSGLOBALS))
	$(call run, +p3 ./jacobi.tls 2 2 2 40 +vp8 +balancer RotateLB +LBDebug 1)
endif
ifeq (1,$(CMK_SUPPORTS_MEMORY_ISOMALLOC))
	$(call run, +p3 ./jacobi.iso 2 2 2 40 +vp8 +balancer RotateLB +LBDebug 1)
endif

testp: ampi
	$(call run, +p$(P) ./jacobi.pup $(P) 1 1 40 +vp$(P) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi.pup $(P) 2 1 40 +vp$$(( $(P) * 2 )) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi.pup $(P) 2 2 40 +vp$$(( $(P) * 4 )) +balancer RotateLB +LBDebug 1 )

	$(call run, +p$(P) ./jacobi $(P) 1 1 40 +vp$(P) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi $(P) 2 1 40 +vp$$(( $(P) * 2 )) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi $(P) 2 2 40 +vp$$(( $(P) * 4 )) +balancer RotateLB +LBDebug 1 )

ifeq (1,$(CMK_SUPPORTS_TLSGLOBALS))
	$(call run, +p$(P) ./jacobi.tls $(P) 1 1 40 +vp$(P) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi.tls $(P) 2 1 40 +vp$$(( $(P) * 2 )) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi.tls $(P) 2 2 40 +vp$$(( $(P) * 4 )) +balancer RotateLB +LBDebug 1 )
endif

ifeq (1,$(CMK_SUPPORTS_MEMORY_ISOMALLOC))
	$(call run, +p$(P) ./jacobi.iso $(P) 1 1 40 +vp$(P) +balancer RotateLB +LBDebug 1)
	$(call run, +p$(P) ./jacobi.iso $(P) 2 1 40 +vp$$(( $(P) * 2 )) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi.iso $(P) 2 2 40 +vp$$(( $(P) * 4 )) +balancer RotateLB +LBDebug 1 )
endif



clean:
	rm -f *.o jacobi *~ moduleinit.C charmrun conv-host jacobi-cpp jacobi.iso jacobi-get jacobi.tls ampirun
	rm -rf 40 80 120
