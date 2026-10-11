# parallax

![parallax](https://user-images.githubusercontent.com/56025978/170892431-f2e70f44-1a40-4dc6-975d-0a89d00a8589.png)

**Use other libretro cores and N64 emulators for any use for gaming.** 

**Please**

## What?

Personal N64 emulator code playset for anything with:

* **x86 CPUs w/SSE4.2 level hardware**.
* **Vulkan 1.1 + OGL 4.6**.
* **Win7 and above/modern Linux**
* **Modern GCC/G++ with C++17 and up support**

## How?

Making a garbage fire by hacking on parallel-n64 to see what the heck happens, if any.

This is in no way intended to be a "daily driver" or even maintained as such. This is worked on **entirely** when I feel like to mess around with N64 things.



## Headless interpreter validation

Reproducible pure/cached interpreter correctness and performance tooling is bundled under `benchmarks/headless/`. Benchmark-only core builds can force either interpreter and expose a read-only timing snapshot without changing normal release builds. See `benchmarks/headless/README.md` for build and run commands.
