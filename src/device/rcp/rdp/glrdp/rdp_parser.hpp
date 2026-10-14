#pragma once
#include "rdp_types.hpp"
#include "rdp_tmem.hpp"
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

struct RDPCallbacks {
    std::function<void(const RDPTriangle &, const RDPState &)> draw_triangle;
    std::function<void(const RDPTextureRectangle &, const RDPState &)> draw_texture_rectangle;
    std::function<void(const RDPFillRectangle &, const RDPState &)> draw_fill_rectangle;
    std::function<void()> sync_load;
    std::function<void()> sync_pipe;
    std::function<void()> sync_tile;
    std::function<void()> sync_full;
    std::function<void(const RDPState &)> before_texture_load;
    std::function<void(const RDPState &)> before_z_image_change;
};

class RDPCommandParser {
  public:
    explicit RDPCommandParser(std::span<const std::uint8_t> rdram = {});
    void set_rdram(std::span<const std::uint8_t>);
    void set_memory(const N64Memory *memory) {
        rdram_ = RDPMemoryView(memory);
    }
    void reset();
    void feed(std::span<const std::uint8_t>);
    void execute(std::span<const std::uint8_t>);
    void discard_pending();
    RDPState &state() {
        return state_;
    }
    const RDPState &state() const {
        return state_;
    }
    RDPCallbacks &callbacks() {
        return callbacks_;
    }

  private:
    void parse_pending();
    void dispatch(RDPOpcode, std::span<const std::uint64_t>);
    RDPState state_{};
    RDPCallbacks callbacks_{};
    RDPMemoryView rdram_{};
    std::vector<std::uint8_t> pending_;
};
