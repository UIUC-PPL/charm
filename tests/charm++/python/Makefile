-include ../../common.mk
CHARMDIR = ../../..
-include $(CHARMDIR)/include/conv-mach-opt.mak
CHARMC = $(CHARMDIR)/bin/charmc $(OPTS)
INCL = -I/usr/include/python/
LIBS = -lpthread -lutil -ldl -lm -lpython$(CMK_BUILD_PYTHON)
INCLC = -I$(CHARMDIR)/include/
MODULES = -module PythonCCS -module charmdebug_python
PYMODULE = $(CHARMDIR)/lib/libmodulePythonCCS.a

OBJS = server.o


all: server client

server: $(OBJS)
	$(CHARMC) $(INCL) -language charm++ -o server $(OBJS) $(LIBS) $(MODULES)

server.decl.h: server.ci $(PYMODULE) $(CHARMDIR)/bin/charmxi
	$(CHARMC) $<

client: client.C
	$(CHARMC) -seq $(INCLC) $(OPTS) -o $@ $^ $(PYMODULE) -lccs-client

clean:
	rm -f *.decl.h *.def.h *.o server client charmrun *~


server.o: server.C server.decl.h server.h
	$(CHARMC) -c $<

test: all
	$(call run, ./server ++remote-shell ssh ++server ++server-port 1234 +p2 $(OPTS) &)
	./client localhost 1234 4 1 0 <python.code
	sleep 2
	$(call run, ./server ++remote-shell ssh ++server ++server-port 1234 +p2 $(OPTS) &)
	./client localhost 1234 4 0 0 <python.code
