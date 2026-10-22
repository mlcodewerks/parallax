#pragma once
#include <array>
#include <cstdint>
#include <vector>

// A load-pipe snapshot owns its GPU bytes independently of the framebuffer it
// came from. Copies of RDPState keep the snapshot alive until their tiles have
// been decoded. GL resources are released by GPUTMEM while its context exists.
struct RDPTMEMBacking {
    std::uint32_t texture = 0, fbo = 0;
    std::uint64_t serial = 0, atlas_epoch = 0;
    std::array<std::uint8_t, 4096> gpu_bytes{};
    struct Tile {
        std::array<std::uint64_t, 19> parameters;
        std::array<int, 4> region;
    };
    std::vector<Tile> tiles;
};
