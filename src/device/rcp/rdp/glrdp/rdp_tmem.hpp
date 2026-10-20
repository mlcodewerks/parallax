#pragma once
#include "rdp_types.hpp"
#include "n64_memory.hpp"
#include <cstdint>
#include <cstring>
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
DecodedTexture rdp_decode_tile(const RDPState &, std::uint8_t);
inline bool rdp_linear_copy_tile(const RDPState &s, unsigned tile) {
    const auto &t = s.tiles[tile & 7];
    return ((s.other_modes >> 52) & 3) == 2 && !((s.other_modes >> 47) & 1) &&
           t.size == RDPImageSize::Bits16 &&
           (t.format == RDPImageFormat::RGBA || t.format == RDPImageFormat::IA);
}
