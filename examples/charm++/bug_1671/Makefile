-include ../../common.mk
CHARMC= ../../../bin/charmc $(OPTS)

all: ng_test

OBJS = ng_test.o

ng_test: $(OBJS)
	$(CHARMC) -language charm++ -o ng_test $(OBJS)

cifiles: ng_test.ci
	$(CHARMC)  ng_test.ci
	touch cifiles

ng_test.o: ng_test.C cifiles
	$(CHARMC) -c ng_test.C

test: all
	$(call run, +p8 ./ng_test 20 200000 10000)

clean:
	rm -f *.decl.h *.def.h conv-host *.o ng_test charmrun cifiles
