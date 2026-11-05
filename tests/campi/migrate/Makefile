-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: migrate

migrate: migrate.o
	$(CHARMC) -o migrate migrate.o

migrate.o: migrate.C
	$(CHARMC) -std=c++20 -c migrate.C

clean:
	rm -f *.o migrate charmrun conv-host ampirun
	rm -f plain.log fake.log lb.log chkpt.log restart.log
	rm -rf log

# The restarted run must end with the summary line of the run that wrote
# the checkpoint.
define check_restart
	grep "^migrate: .* checksum sum" $(1) && test "$$(grep '^migrate: .* checksum sum' $(1))" = "$$(grep '^migrate: .* checksum sum' $(2))"
endef

test: migrate
	$(call run, +p2 ./migrate +vp4 ) 2>&1 | tee plain.log
	grep "^All tests passed" plain.log
	$(call run, +p2 ./migrate +vp4 +campi_fakemigrate ) 2>&1 | tee fake.log
	grep "^All tests passed" fake.log
	$(call run, +p4 ./migrate +vp4 +balancer RotateLB ) 2>&1 | tee lb.log
	grep "^All tests passed" lb.log
	rm -rf log
	$(call run, +p4 ./migrate -chkpt log +vp4 +balancer RotateLB ) 2>&1 | tee chkpt.log
	grep "^All tests passed" chkpt.log
	-sync
	$(call run, +p2 ./migrate -chkpt log +vp4 +balancer RotateLB +restart log ) 2>&1 | tee restart.log
	grep "^All tests passed" restart.log
	$(call check_restart,restart.log,chkpt.log)
