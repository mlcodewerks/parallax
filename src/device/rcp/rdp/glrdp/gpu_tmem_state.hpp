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
    // Repeating the same load from an unchanged source preserves this snapshot
    // and its decoded tiles. Retained bytes already belong to this snapshot.
    std::array<std::uint32_t, 4096> last_load_map{};
    std::uint64_t last_load_serial = 0;
    std::uint64_t last_load_version = 0;
    std::uint32_t last_load_source = 0;
    bool last_load_valid = false;
    struct Tile {
        std::array<std::uint64_t, 19> parameters;
        std::array<int, 4> region;
    };
    std::vector<Tile> tiles;
};
