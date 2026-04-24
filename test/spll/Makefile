# This work is part of the White Rabbit project
#
# Copyright (C) 2026 CERN (www.cern.ch)
#
# Released according to the GNU GPL, version 2 or any later version.

INCFLAGS=-I../../include -I../../pp_printf -I../.. -include ../../include/generated/autoconf.h
CFLAGS=$(INCFLAGS) -g -MMD -Wall -Werror

all: test_softpll test_spll_main

spll_main.o: ../../softpll/spll_main.c
	$(CC) -c $(CFLAGS) -o $@ $<

spll_common.o: ../../softpll/spll_common.c
	$(CC) -c $(CFLAGS) -o $@ $<

spll_helper.o: ../../softpll/spll_helper.c
	$(CC) -c $(CFLAGS) -o $@ $<

spll_ptracker.o: ../../softpll/spll_ptracker.c
	$(CC) -c $(CFLAGS) -o $@ $<

softpll_ng.o: ../../softpll/softpll_ng.c
	$(CC) -c $(CFLAGS) -o $@ $<

div64.o: ../../pp_printf/div64.c
	$(CC) -c $(CFLAGS) -o $@ $<

OBJS=spll_common.o spll_main.o spll_helper.o spll_ptracker.o softpll_ng.o div64.o hw.o

test_softpll: test_softpll.o $(OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(OBJS)

test_spll_main: test_spll_main.o $(OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(OBJS)

check: test_spll_main
	./test_spll_main

clean:
	$(RM) *.o *.d test_spll_main test_softpll

.PHONY: check clean

-include $(OBJS:.o=.d)
