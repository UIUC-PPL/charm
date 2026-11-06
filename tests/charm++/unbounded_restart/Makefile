-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)
# lcrun of a reconverse build (testprocs is reconverse-only).
LCRUN ?= ../../../bin/lcrun

all: restart
	rm -rf ckptlog/

restart: restart.decl.h restart.C
	$(CHARMC) restart.C -o restart

restart.decl.h: restart.ci
	$(CHARMC) restart.ci

# Restart with the same and with a different number of PEs.
test: all
	-rm -rf ckptlog
	$(call run, ./restart +p4 )
	-sync
	$(call run, ./restart +p4 +restart ckptlog )
	$(call run, ./restart +p2 +restart ckptlog )
	$(call run, ./restart +p3 +restart ckptlog )
	-rm -rf ckptlog

smptest: test

# A chain of nine restarts, each from the checkpoint the previous restarted run
# wrote (-c). Before 2026-10-10 every restart spent 1/8 of the allocator's pool and
# the ninth aborted ("has used all 2^U unique element ids").
testchain: all
	-rm -rf ckptlog
	$(call run, ./restart +p4 )
	$(call run, ./restart +p4 +restart ckptlog -c )
	$(call run, ./restart +p4 +restart ckptlog -c )
	$(call run, ./restart +p4 +restart ckptlog -c )
	$(call run, ./restart +p4 +restart ckptlog -c )
	$(call run, ./restart +p4 +restart ckptlog -c )
	$(call run, ./restart +p4 +restart ckptlog -c )
	$(call run, ./restart +p4 +restart ckptlog -c )
	$(call run, ./restart +p4 +restart ckptlog -c )
	$(call run, ./restart +p4 +restart ckptlog -c )
	-rm -rf ckptlog

# A chain under a narrow pool: +objid_expand 2^24 leaves U = 25 unique bits, so the
# allocator's cap on 1 process is 2^21 and the first grant after a restart is the
# cap / 64 = 2^15 rule, not the flat 2^16 (the "Generation N: the previous restart
# consumed" lines show it). Before 2026-10-12 the flat 2^16 floor met the cap from
# 8192 processes up and every restart there cost 1/8 of the pool.
testnarrow: all
	-rm -rf ckptlog
	$(call run, ./restart +p4 +objid_expand 16777216 )
	$(call run, ./restart +p4 +restart ckptlog -c )
	$(call run, ./restart +p4 +restart ckptlog -c )
	$(call run, ./restart +p4 +restart ckptlog -c )
	$(call run, ./restart +p2 +restart ckptlog -c )
	-rm -rf ckptlog

# Reconverse only: checkpoint from 2 processes x 1 PE, restart on 1 process x 2 PEs,
# on 2 processes x 1 PE, and on 2 processes x 2 PEs (the shape of charm #4018).
testprocs: all
	-rm -rf ckptlog
	$(LCRUN) -n 2 ./restart +pe 2
	./restart +pe 2 +restart ckptlog
	$(LCRUN) -n 2 ./restart +pe 2 +restart ckptlog
	$(LCRUN) -n 2 ./restart +pe 4 +restart ckptlog
	-rm -rf ckptlog
	$(LCRUN) -n 2 ./restart +pe 2
	./restart +pe 2 +restart ckptlog -c
	$(LCRUN) -n 2 ./restart +pe 4 +restart ckptlog -c
	$(LCRUN) -n 2 ./restart +pe 2 +restart ckptlog -c
	-rm -rf ckptlog

clean:
	rm -f *.decl.h *.def.h *.o restart charmrun
	rm -rf ckptlog/
