-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)

all: groupdep

groupdep: groupdep.decl.h groupdep.C
	$(CHARMC) groupdep.C -o groupdep

groupdep.decl.h: groupdep.ci
	$(CHARMC) groupdep.ci

# The request to create B can reach PE 0 before or after A's creation broadcast;
# the randomized queue produces both orders over the seeds.
test: groupdep
	$(call run, ./groupdep +p4)
	$(call run, ./groupdep +p4 +randomized_msgq +randomized_seed 1)
	$(call run, ./groupdep +p4 +randomized_msgq +randomized_seed 2)
	$(call run, ./groupdep +p4 +randomized_msgq +randomized_seed 3)
	$(call run, ./groupdep +p4 +randomized_msgq +randomized_seed 4)

smptest: groupdep
	$(call run, ./groupdep +p4 +randomized_msgq +randomized_seed 1)

clean:
	rm -f *.decl.h *.def.h *.o groupdep charmrun
