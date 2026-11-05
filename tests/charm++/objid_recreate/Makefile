-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)

all: recreate

recreate: recreate.decl.h recreate.C
	$(CHARMC) recreate.C -o recreate

recreate.decl.h: recreate.ci
	$(CHARMC) recreate.ci

# The home PE's handling of an element re-created under the same id, with the
# home-side handlers driven directly in the orders the network can produce.
test: recreate
	$(call run, ./recreate +p1)
	$(call run, ./recreate +p4)

smptest: recreate
	$(call run, ./recreate +p4)

clean:
	rm -rf *.decl.h *.def.h recreate charmrun
