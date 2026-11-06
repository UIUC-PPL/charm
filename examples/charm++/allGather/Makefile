-include ../../common.mk
CHARMC = ../../../bin/charmc $(OPTS)
TYPE ?= RING

all: build

build:
	$(CHARMC) ./user.ci
	$(CHARMC) -o allGather -D$(TYPE) ./user.C -lmoduleallGather

test:
	@$(CHARMC) ./user.ci
	@echo "\033[1;34mRunning allGather with RING algorithm\033[0m"
	@$(CHARMC) -o allGather -DRING ./user.C -lmoduleallGather
	$(call run, +p4 ./allGather 4 29 7 )
	@echo "\033[1;34mRunning allGather with HYPERCUBE algorithm\033[0m"
	@$(CHARMC) -o allGather -DHYPERCUBE ./user.C -lmoduleallGather
	$(call run, +p4 ./allGather 4 29 7 )
	@echo "\033[1;34mRunning allGather with HYPERCUBE algorithm on 5 PEs (not a power of two)\033[0m"
	$(call run, +p5 ./allGather 5 29 7 )
	@echo "\033[1;34mRunning allGather with FLOODING algorithm\033[0m"
	@$(CHARMC) -o allGather -DFLOODING ./user.C -lmoduleallGather
	$(call run, +p4 ./allGather 4 29 7 )
	@echo "\033[1;32mAll Tests completed!\033[0m"

testp:
	@$(CHARMC) ./user.ci
	@echo "\033[1;34mRunning allGather with RING algorithm\033[0m"
	@$(CHARMC) -o allGather -DRING ./user.C -lmoduleallGather
	$(call run, +p$(P) ./allGather $(P) 29 7 )
	@echo "\033[1;34mRunning allGather with HYPERCUBE algorithm\033[0m"
	@$(CHARMC) -o allGather -DHYPERCUBE ./user.C -lmoduleallGather
	$(call run, +p$(P) ./allGather $(P) 29 7 )
	@echo "\033[1;34mRunning allGather with FLOODING algorithm\033[0m"
	@$(CHARMC) -o allGather -DFLOODING ./user.C -lmoduleallGather
	$(call run, +p$(P) ./allGather $(P) 29 7 )
	@echo "\033[1;32mAll Tests completed!\033[0m"

clean:
	rm -f *.decl.h *.def.h *.o charmrun allGather

.phony : all build test clean
