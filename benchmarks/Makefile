CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -I../deps/libretro-common/include
ifneq ($(OS),Windows_NT)
LDLIBS += -ldl
else
LDLIBS += -lopengl32 -lgdi32 -luser32
endif

all: benchmark

input_script_checks: input_script_checks.c input_script.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ input_script_checks.c

gameplay_benchmark: benchmark.c gl_context.h input_script.h ../deps/libretro-common/include/libretro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ benchmark.c $(LDLIBS)

# Separate executable so existing recorded replays keep their original harness.
gameplay_benchmark_interval: benchmark.c gl_context.h input_script.h ../deps/libretro-common/include/libretro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ benchmark.c $(LDLIBS)

# Opt-in wall-clock measurements; keep archived correctness harnesses immutable.
gameplay_benchmark_frametime: benchmark.c gl_context.h input_script.h ../deps/libretro-common/include/libretro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ benchmark.c $(LDLIBS)

benchmark: benchmark.c gl_context.h input_script.h ../deps/libretro-common/include/libretro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ benchmark.c $(LDLIBS)

clean:
	rm -f benchmark benchmark.exe gameplay_benchmark gameplay_benchmark.exe gameplay_benchmark_interval gameplay_benchmark_interval.exe input_script_checks input_script_checks.exe

.PHONY: all clean
