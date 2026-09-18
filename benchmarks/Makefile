CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -Iinclude
LDLIBS += -ldl

all: benchmark

benchmark: benchmark.c include/libretro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ benchmark.c $(LDLIBS)

clean:
	rm -f benchmark

.PHONY: all clean
