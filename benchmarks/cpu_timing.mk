CC ?= cc
ROOT := $(dir $(lastword $(MAKEFILE_LIST)))..
CPU := $(ROOT)/src/device/r4300
SOURCES := $(ROOT)/benchmarks/cpu_timing.c $(addprefix $(CPU)/,cp0.c cp1.c pure_interp.c cached_interp.c r4300_core.c tlb.c idec.c interrupt.c)

cpu_timing: cpu_fpu_native.o $(SOURCES) $(ROOT)/benchmarks/cpu_behavior.h $(ROOT)/benchmarks/cpu_fpu_cop0.h $(wildcard $(CPU)/*.h)
	$(CC) -O3 -std=gnu99 -ffast-math -ffunction-sections -fdata-sections $(CFLAGS) -I$(ROOT)/src -I$(ROOT)/src/api -I$(ROOT)/custom -I$(ROOT)/deps/libretro-common/include $(SOURCES) cpu_fpu_native.o -Wl,--gc-sections -lm -o $@

.PHONY: check
check: cpu_timing cpu_fpu_diff
	./cpu_timing
	./cpu_fpu_diff

cpu_fpu_native.o: $(CPU)/fpu_native.c $(CPU)/fpu_native.h $(CPU)/fpu_neon.h $(CPU)/cp1.h
	$(CC) -O3 -std=gnu99 $(CFLAGS) -fno-fast-math -fno-math-errno -frounding-math -ffp-contract=off -I$(ROOT)/custom -c $< -o $@

cpu_fpu_portable.o: $(CPU)/fpu_native.c $(CPU)/fpu_native.h $(CPU)/fpu_neon.h $(CPU)/cp1.h
	$(CC) -O3 -std=gnu99 $(CFLAGS) -DM64P_FPU_PORTABLE -Dfpu_native_eval=fpu_native_eval_portable -fno-fast-math -fno-math-errno -frounding-math -ffp-contract=off -I$(ROOT)/custom -c $< -o $@

cpu_fpu_diff: $(ROOT)/benchmarks/fpu_native_diff.c cpu_fpu_native.o cpu_fpu_portable.o
	$(CC) -O3 -std=gnu99 -ffast-math $(CFLAGS) -I$(ROOT)/src $^ -lm -o $@
