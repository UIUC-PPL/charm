-include ../../common.mk
CHARMC = ../../../bin/charmc $(OPTS)

all: memckpt_rollback

memckpt_rollback: memckpt_rollback.o
	$(CHARMC) -language charm++ -o $@ memckpt_rollback.o -module CommonLBs

memckpt_rollback.decl.h: memckpt_rollback.ci
	$(CHARMC) memckpt_rollback.ci

memckpt_rollback.o: memckpt_rollback.C memckpt_rollback.decl.h
	$(CHARMC) -c memckpt_rollback.C

test: all testrestriction
	$(call run, ./memckpt_rollback +p1 )
	$(call run, ./memckpt_rollback +p2 )
	$(call run, ./memckpt_rollback +p4 )
	$(call run, ./memckpt_rollback +p4 -rollbacks 2 )

# A migration between the checkpoint and the rollback must abort the rollback.
testrestriction: all
	@if $(call run, ./memckpt_rollback +p4 -lb +balancer RotateLB ) > restriction.out 2>&1; then \
	  cat restriction.out; echo "testrestriction: expected a nonzero exit"; exit 1; fi
	@grep -q "rollback refused: .* array elements migrated" restriction.out || \
	  { cat restriction.out; echo "testrestriction: restriction message missing"; exit 1; }
	@echo "memckpt_rollback testrestriction passed"

testp: all
	$(call run, ./memckpt_rollback +p$(P) )

smptest: all
	$(call run, ./memckpt_rollback +p4 ++ppn 2 )

clean:
	rm -f *.decl.h *.def.h *.o memckpt_rollback charmrun restriction.out
