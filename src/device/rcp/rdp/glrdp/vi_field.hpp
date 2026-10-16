#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <algorithm>

// Include the final vertical resampling tap and fractional Y offset. This is
// an address span for image selection, not a framebuffer allocation size.
inline uint32_t vi_source_byte_count(uint32_t status, uint32_t width, uint32_t vstart,
                                     uint32_t yscale) {
    const int lines = std::max(1, (int(vstart & 1023) - int((vstart >> 16) & 1023)) / 2);
    const uint64_t last_y = uint64_t(lines - 1) * (yscale & 4095) + ((yscale >> 16) & 4095);
    const uint64_t rows = (last_y + 1023) / 1024 + 1;
    return uint32_t(std::min<uint64_t>(UINT32_MAX, uint64_t(std::max(width, 1u)) * rows *
                                                       ((status & 3) == 3 ? 4 : 2)));
}

// The VI supplies one field per interrupt, not a new progressive frame. Keep
// history across origin/field-offset changes, but never across display modes.
struct VIFieldHistory {
    bool valid = false;
    unsigned lower = 0;
    int previous_start = 0;
    std::array<uint32_t, 8> mode{};

    void reset() {
        valid = false;
    }
    bool update(uint32_t status, uint32_t width, uint32_t hstart, uint32_t vstart, uint32_t xscale,
                uint32_t yscale, uint32_t vsync, uint32_t current, bool bob) {
        if (!(status & 64) || !(status & 2)) {
            reset();
            return false;
        }
        const int start = int((vstart >> 16) & 1023);
        const std::array<uint32_t, 8> next = {status & 3,    width,
                                              hstart,        xscale & 4095,
                                              yscale & 4095, uint32_t((vsync & 1023) > 550),
                                              uint32_t(bob), uint32_t(int(vstart & 1023) - start)};
        bool preserve = valid && mode == next && std::abs(start - previous_start) <= 2;
        if (current != UINT32_MAX)
            lower = (current & 1) ^ 1;
        else if (preserve)
            lower = start == previous_start ? lower ^ 1 : unsigned(start < previous_start);
        else
            lower = 0;
        mode = next;
        previous_start = start;
        valid = true;
        return preserve;
    }
};
