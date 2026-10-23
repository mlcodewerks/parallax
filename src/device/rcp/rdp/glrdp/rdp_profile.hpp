#pragma once

// Inclusive CPU timings for diagnostic cores only. Ordinary cores compile
// every scope away and expose no profiling entry point.
#define GLRDP_PROFILE_FIELDS(X) \
    X(commands) X(present) X(draw) X(flush) X(submit) \
    X(texture_load) X(load_map) X(tile) X(cpu_decode) \
    X(gpu_load) X(gpu_decode) X(color_bind) X(color_upload) X(color_read) \
    X(depth_ensure) X(depth_read) X(cpu_write) X(state_save) X(state_restore) \
    X(color_resize) X(color_merge) X(color_alias) X(color_retire) X(depth_alias) \
    X(color_alloc) X(depth_alloc) X(depth_import) X(gpu_alias)

#ifdef PARALLAX_GLRDP_PROFILE
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
namespace glrdp_profile {
enum Field {
#define GLRDP_PROFILE_ENUM(name) name,
    GLRDP_PROFILE_FIELDS(GLRDP_PROFILE_ENUM)
#undef GLRDP_PROFILE_ENUM
    count
};
struct Value { double milliseconds = 0; std::uint64_t calls = 0; };
inline std::array<Value, count> values{};
inline unsigned step = 0;
inline void event(const char *name, unsigned address, int width, int height) {
    static FILE *file = [] {
        const auto *path = std::getenv("M64P_BENCH_RENDERER_EVENTS");
        auto *stream = path ? std::fopen(path, "w") : nullptr;
        if (stream) std::fputs("step,event,address,width,height\n", stream);
        return stream;
    }();
    if (file) std::fprintf(file, "%u,%s,%x,%d,%d\n", step, name, address, width, height);
}
class Scope {
    Field field_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  public:
    explicit Scope(Field field) : field_(field) {}
    ~Scope() {
        auto &value = values[field_];
        value.milliseconds += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start_).count();
        ++value.calls;
    }
};
inline void take(double *out) {
    for (unsigned i = 0; i < count; ++i) {
        out[i * 2] = values[i].milliseconds;
        out[i * 2 + 1] = double(values[i].calls);
    }
    values = {};
    ++step;
}
}
#define GLRDP_PROFILE_SCOPE(name) glrdp_profile::Scope glrdp_profile_scope(glrdp_profile::name)
#define GLRDP_PROFILE_EVENT(name, address, width, height) glrdp_profile::event(name, address, width, height)
#else
#define GLRDP_PROFILE_SCOPE(name) do {} while (false)
#define GLRDP_PROFILE_EVENT(name, address, width, height) do {} while (false)
#endif
