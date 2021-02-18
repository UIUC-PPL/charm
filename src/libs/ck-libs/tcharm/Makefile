include ../common.mk

HEADERS=$(CDIR)/include/tcharm_impl.h $(CDIR)/include/tcharm.h \
        $(CDIR)/include/tcharmc.h $(CDIR)/include/tcharmf.h \
        $(CDIR)/include/tcharm.decl.h
OBJS=tcharm.o
COMPAT=compat_uns.o compat_us.o compat_funs.o compat_fus.o compat_regmm.o \
       compat_ns.o compat_es.o
CHARMXI=$(CDIR)/tmp/charmxi

LIB=libmoduletcharm
DEST=$(LIBDIR)/$(LIB).a
COMPATLIB=$(LIBDIR)/libtcharm-compat.a

OBJS2=tcharmmain.o
LIB2=libmoduletcharmmain
DEST2=$(LIBDIR)/$(LIB2).a

OBJECTS = $(OBJS) $(OBJS2) $(COMPAT)
CIFILES = tcharm.decl.h  tcharmmain.decl.h

.SUFFIXES:
.SUFFIXES: .c .o

all: $(DEST) $(COMPATLIB) $(DEST2)

$(DEST): $(OBJS) $(HEADERS)
	$(CHARMC) $(OBJS) -o $@
	cp $(LIB).dep $(LIBDIR)/$(LIB).dep

$(DEST2): $(OBJS2) $(HEADERS)
	$(CHARMC) $(OBJS2) -o $@
	cp $(LIB2).dep $(LIBDIR)/$(LIB2).dep

$(COMPATLIB): $(COMPAT) 
	$(CHARMC) $(COMPAT) -o $@

tcharm.decl.h: tcharm.ci $(CDIR)/bin/charmxi
	$(CHARMC) -c $<

tcharm.def.h: tcharm.decl.h

tcharmmain.decl.h: tcharmmain.ci $(CDIR)/bin/charmxi
	$(CHARMC) -c $<

tcharmmain.def.h: tcharmmain.decl.h

tcharm.o: tcharm.C tcharm_impl.h tcharmc.h tcharm.decl.h tcharm.h tcharm.def.h
	$(CHARMC) -o $@ -c $<

tcharmmain.o: tcharmmain.C tcharm_impl.h tcharmc.h tcharm.decl.h tcharm.h tcharmmain.decl.h tcharmmain.def.h
	$(CHARMC) -o $@ -c $<

compat_uns.o: compat_uns.c tcharmc.h
	$(CHARMC) -o $@ -c $<

compat_us.o: compat_us.c tcharmc.h
	$(CHARMC) -o $@ -c $<

compat_funs.o: compat_funs.c tcharmc.h
	$(CHARMC) -o $@ -c $<

compat_fus.o: compat_fus.c tcharmc.h
	$(CHARMC) -o $@ -c $<

compat_regmm.o: compat_regmm.c
	$(CHARMC) -o $@ -c $<

compat_ns.o: compat_ns.c
	$(CHARMC) -o $@ -c $<

compat_es.o: compat_es.c
	$(CHARMC) -o $@ -c $<

headers: $(HEADERS)

clean: 
	-rm -fr *.o *~ *.decl.h *.def.h gmon.out headers conv-host charmrun

realclean: clean
	rm -f $(HEADERS) $(DEST) $(DEST2) $(COMPATLIB)

