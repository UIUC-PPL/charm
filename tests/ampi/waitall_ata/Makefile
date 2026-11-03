-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: waitall_ata

waitall_ata: test.o
	$(CHARMC) -o waitall_ata test.o

test.o: test.C
	$(CHARMC) -c test.C

# Stage "migrate" keeps an MPI_Ialltoall outstanding across AMPI_Migrate,
# which AMPI's default migration mode allows (its blocks travel as messages to
# the rank); with +balancer RandCentLB the ranks move while it is outstanding.
test: waitall_ata
	$(call run, ./waitall_ata +p1 +vp4 )
	$(call run, ./waitall_ata +p1 +vp7 )
	$(call run, ./waitall_ata +p2 +vp4 )
	$(call run, ./waitall_ata +p2 +vp7 )
	$(call run, ./waitall_ata +p2 +vp4 +balancer RandCentLB )

testp: waitall_ata
	$(call run, ./waitall_ata +p$(P) +vp$(P) )
	$(call run, ./waitall_ata +p$(P) +vp$$(( $(P) * 2 )) )

clean:
	rm -f *.o *.mod waitall_ata *~ conv-host charmrun ampirun waitall_ata.exe waitall_ata.pdb waitall_ata.ilk
