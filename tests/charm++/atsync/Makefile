-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS) $(MOPTS)

OBJS = atsync.o

all: atsync

atsync: $(OBJS)
	$(CHARMC) -language charm++ -module CommonLBs -o atsync $(OBJS)

atsync.decl.h: atsync.ci
	$(CHARMC)  atsync.ci

clean:
	rm -f *.decl.h *.def.h conv-host *.o atsync charmrun *.log *.sum *.sts *~

atsync.o: atsync.C atsync.decl.h
	$(CHARMC) -c atsync.C

test: all
	$(call run, ./atsync 8 +p2 +setcpuaffinity +balancer RotateLB )
	$(call run, ./atsync 8 +p4 +setcpuaffinity +balancer RotateLB )
	$(call run, ./atsync 8 +p6 +setcpuaffinity +balancer RotateLB )

testp: all
	$(call run, ./atsync $(P) +p$(P) +setcpuaffinity +balancer RotateLB )

smptest: all
	$(call run, ./atsync 8 +p2 ++ppn 2 +setcpuaffinity +balancer RotateLB )
	$(call run, ./atsync 8 +p4 ++ppn 2 +setcpuaffinity +balancer RotateLB )
	$(call run, ./atsync 8 +p6 ++ppn 2 +setcpuaffinity +balancer RotateLB )
