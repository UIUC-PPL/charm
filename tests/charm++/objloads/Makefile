-include ../../common.mk
CHARMDIR = ../../..
CHARMC = $(CHARMDIR)/bin/charmc $(OPTS)

# Needs a build with tracing (--enable-tracing): the server links
# -tracemode utilization. Without it the test is skipped.
TRACELIB = $(wildcard $(CHARMDIR)/lib/libtrace-utilization.a)

ifeq ($(TRACELIB),)
all:
	@echo "objloads: skipped, this build has no tracing (libtrace-utilization.a)"
test: all
else
all: objloads client

objloads: objloads.o
	$(CHARMC) -language charm++ -tracemode utilization -o objloads objloads.o

objloads.decl.h: objloads.ci
	$(CHARMC) objloads.ci

objloads.o: objloads.C objloads.decl.h
	$(CHARMC) -c objloads.C

client: client.C
	$(CHARMC) -seq -o client client.C -lccs-client

# 4 arrays x 8 elements, running 12 s; the client connects after 3 s.
test: all
	$(call run, ./objloads ++server ++server-port 12345 +p2 8 12) & pid=$$!; \
	sleep 3; ./client localhost 12345 32; status=$$?; wait $$pid; exit $$status
endif

clean:
	rm -f *.decl.h *.def.h *.o objloads client charmrun *.log *.sts *.util.sts
