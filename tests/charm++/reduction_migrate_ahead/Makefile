-include ../../common.mk
-include ../../../include/conv-mach-opt.mak
CHARMC=../../../bin/charmc $(OPTS)

all: reduction_migrate_ahead

reduction_migrate_ahead.decl.h reduction_migrate_ahead.def.h: reduction_migrate_ahead.ci
	$(CHARMC) reduction_migrate_ahead.ci

reduction_migrate_ahead: reduction_migrate_ahead.C reduction_migrate_ahead.decl.h reduction_migrate_ahead.def.h
	$(CHARMC) -language charm++ reduction_migrate_ahead.C -o reduction_migrate_ahead

# Elements migrate to the next PE while reductions are in flight; odd PE
# counts give the uneven layouts the defect needs (#4145).
test: reduction_migrate_ahead
	$(call run, ./reduction_migrate_ahead +p3)
	$(call run, ./reduction_migrate_ahead +p4)
	$(call run, ./reduction_migrate_ahead +p2 50 8)

smptest: reduction_migrate_ahead
	$(call run, ./reduction_migrate_ahead +p3 ++ppn 3)

clean:
	rm -f *.decl.h *.def.h *.o reduction_migrate_ahead charmrun
