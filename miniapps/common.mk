ifneq ($(wildcard ../../../bin/.),)
	run = ../../../bin/testrun $(PRETESTOPTS) $(1) $(TESTOPTS)
else
	ifneq ($(wildcard ../../../../bin/.),)
		run = ../../../../bin/testrun $(PRETESTOPTS) $(1) $(TESTOPTS)
	else
		run = ../../../../../bin/testrun $(PRETESTOPTS) $(1) $(TESTOPTS)
	endif
endif
