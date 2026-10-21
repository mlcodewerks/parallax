CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -I../deps/libretro-common/include
ifneq ($(OS),Windows_NT)
LDLIBS += -ldl
else
LDLIBS += -lopengl32 -lgdi32 -luser32
endif

all: benchmark

# Windows-only process sampler; not part of a core or ordinary harness build.
win_cpu_sample: win_cpu_sample.cpp
	$(CXX) -O2 -std=c++17 -o $@ $<

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

gameplay_benchmark_calls: benchmark.c gl_context.h gl_call_counts.h input_script.h ../deps/libretro-common/include/libretro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ benchmark.c $(LDLIBS)

gameplay_benchmark_vi: benchmark.c gl_context.h input_script.h ../deps/libretro-common/include/libretro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ benchmark.c $(LDLIBS)

gameplay_benchmark_audio: benchmark.c gl_context.h input_script.h ../deps/libretro-common/include/libretro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ benchmark.c $(LDLIBS)

gameplay_benchmark_timing: benchmark.c gl_context.h input_script.h ../deps/libretro-common/include/libretro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DM64P_BENCH_OPTIONAL_DEBUG -o $@ benchmark.c $(LDLIBS)

benchmark: benchmark.c gl_context.h input_script.h ../deps/libretro-common/include/libretro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ benchmark.c $(LDLIBS)

clean:
	rm -f benchmark benchmark.exe gameplay_benchmark gameplay_benchmark.exe gameplay_benchmark_interval gameplay_benchmark_interval.exe input_script_checks input_script_checks.exe

.PHONY: all clean

# VFS-aware frontend; archived benchmark executables remain unchanged.
gameplay_benchmark_native: benchmark.c frontend_vfs.h gl_context.h input_script.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPARALLAX_TEST_VFS -DM64P_BENCH_OPTIONAL_DEBUG -o $@ benchmark.c $(LDLIBS)

native_vfs_checks: native_vfs_checks.c ../src/core/file.c ../src/core/file.h ../src/main/util.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -D__LIBRETRO__ -I../src -I../custom -I../deps -ffunction-sections -fdata-sections native_vfs_checks.c ../src/core/file.c ../src/main/util.c -Wl,--gc-sections -lm -o $@

native_lifecycle_checks: native_lifecycle_checks.c frontend_vfs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ native_lifecycle_checks.c $(LDLIBS)
