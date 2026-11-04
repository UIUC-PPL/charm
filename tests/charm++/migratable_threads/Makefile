-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)

OBJS = migthread.o

all: migthread

# -memory isomalloc: the point of the test is that allocations made inside a
# thread land in that thread's own migratable heap, which needs the intercepting
# memory module.
migthread: $(OBJS)
	$(CHARMC) -language charm++ -memory isomalloc -o migthread $(OBJS)

migthread.decl.h: migthread.ci
	$(CHARMC) migthread.ci

migthread.o: migthread.C migthread.decl.h
	$(CHARMC) -memory isomalloc -c migthread.C

clean:
	rm -f *.decl.h *.def.h conv-host *.o migthread charmrun *.log *.sum *.sts

test: all
	$(call run, ./migthread +p1 )

# The round trip does not depend on the PE count; ++ppn (several PEs in one
# process) adds nothing to it beyond running on an SMP process.
smptest: all
	$(call run, ./migthread +p2 ++ppn 2 )
