#pragma once
#include "rdp_types.hpp"
#include "n64_memory.hpp"
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <span>
#include <vector>
class RDPMemoryView {
  public:
    RDPMemoryView(std::span<const std::uint8_t> bytes = {}) : bytes_(bytes) {
    }
    RDPMemoryView(const N64Memory *memory) : memory_(memory) {
    }
    std::size_t size() const {
        return memory_ ? memory_->size() : bytes_.size();
    }
    std::uint8_t operator[](std::size_t address) const {
        // Match the RDP's 24-bit bus and Angrylion's absent-RDRAM behavior.
        // An unpopulated address returns zero; it does not abort later commands.
        address &= 0x00ffffffu;
        if (address >= size())
            return 0;
        return memory_ ? memory_->read8(std::uint32_t(address)) : bytes_[address];
    }

    void read_word(std::uint64_t address, std::uint8_t (&word)[8]) const {
        address &= 0x00ffffffu;
        if (size() >= 8 && address <= size() - 8 && address <= 0x00fffff8u) {
            if (memory_)
                memory_->copy_be(std::uint32_t(address), word, 8);
            else
                std::memcpy(word, bytes_.data() + address, 8);
        } else {
            for (unsigned i = 0; i < 8; ++i)
                word[i] = (*this)[address + i];
        }
    }

  private:
    std::span<const std::uint8_t> bytes_;
    const N64Memory *memory_ = nullptr;
};
struct DecodedTexture {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
void rdp_load_tile(RDPState &, std::uint64_t, RDPMemoryView);
void rdp_load_block(RDPState &, std::uint64_t, RDPMemoryView);
void rdp_load_tlut(RDPState &, std::uint64_t, RDPMemoryView);
// UINT32_MAX leaves a TMEM byte unchanged; other entries are 24-bit source
// addresses. Uses the same bank packing as the CPU load pipe.
bool rdp_texture_load_map(const RDPState &, RDPOpcode, std::uint64_t,
                          std::array<std::uint32_t, 4096> &);
bool rdp_tile_uses_gpu(const RDPState &, std::uint8_t);
DecodedTexture rdp_decode_tile(const RDPState &, std::uint8_t, bool compact = false);
inline bool rdp_linear_copy_tile(const RDPState &s, unsigned tile) {
    const auto &t = s.tiles[tile & 7];
    return ((s.other_modes >> 52) & 3) == 2 && !((s.other_modes >> 47) & 1) &&
           t.size == RDPImageSize::Bits16 &&
           (t.format == RDPImageFormat::RGBA || t.format == RDPImageFormat::IA);
}

struct RDPTileStorage {
    unsigned width, height, texel_shift = 0;
};
inline RDPTileStorage rdp_tile_storage(const RDPState &s, unsigned tile, bool compact) {
    const auto &t = s.tiles[tile & 7];
    if (rdp_linear_copy_tile(s, tile)) return {512, 4, 0};
    // The RDP subtracts integer endpoints in a wrapping 10-bit datapath.
    RDPTileStorage result{unsigned((((t.sh >> 2) - (t.sl >> 2)) & 0x3ff) + 1),
                          unsigned((((t.th >> 2) - (t.tl >> 2)) & 0x3ff) + 1), 0};
    if (t.sh < t.sl && t.mask_s && !t.clamp_s)
        result.width = 1u << std::min(unsigned(t.mask_s), 10u);
    if (t.th < t.tl && t.mask_t && !t.clamp_t)
        result.height = 1u << std::min(unsigned(t.mask_t), 10u);
    if (!compact) return result;
    if (t.mask_s) result.width = std::min(result.width, 1u << std::min(unsigned(t.mask_s), 10u));
    if (t.mask_t) result.height = std::min(result.height, 1u << std::min(unsigned(t.mask_t), 10u));
    const bool palette = ((s.other_modes >> 47) & 1) != 0;
    const bool half_tmem = palette || t.size == RDPImageSize::Bits32 || t.format == RDPImageFormat::YUV;
    const unsigned rows = half_tmem ? 256u : 512u;
    result.height = std::min(result.height, std::max(2u, rows / std::gcd(unsigned(t.line), rows)));
    // A flat snapshot also removes overlap between adjacent decoded rows.
    // Texel stepping follows the actual bank layout, including split YUV/RGBA
    // and the YUV palette byte path, rather than just the descriptor's size.
    const bool yuv_bytes = t.format == RDPImageFormat::YUV &&
                           (palette || t.size == RDPImageSize::Bits16);
    const unsigned shift = yuv_bytes ? 3u : 4u - std::min(unsigned(t.size), 2u);
    const unsigned texels = (half_tmem ? 256u : 512u) << shift;
    if (result.width * result.height > texels)
        result = {512, texels / 512, shift};
    return result;
}
