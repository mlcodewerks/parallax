#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

class N64Memory {
  public:
    void bind(std::uint8_t *rdram, std::size_t size, bool byteswapped) {
        data_ = rdram;
        size_ = size;
        byteswapped_ = byteswapped;
        // RDRAM has two RDP-only bits per halfword. CPU/DMA stores leave
        // them intact; color coverage and Z's DZ exponent share these bits.
        hidden_.assign((size + 1) / 2, 3);
    }

    std::uint8_t read8(std::uint32_t address) const {
        check(address, 1);
        return data_[byteswapped_ ? (address ^ 3u) : address];
    }

    std::uint16_t read16(std::uint32_t address) const {
        return std::uint16_t((std::uint16_t(read8(address)) << 8) | read8(address + 1));
    }

    std::uint32_t read32(std::uint32_t address) const {
        return (std::uint32_t(read8(address)) << 24) | (std::uint32_t(read8(address + 1)) << 16) |
               (std::uint32_t(read8(address + 2)) << 8) | read8(address + 3);
    }

    void write8(std::uint32_t address, std::uint8_t value) {
        check(address, 1);
        data_[byteswapped_ ? (address ^ 3u) : address] = value;
    }

    void write16(std::uint32_t a, std::uint16_t v) {
        write8(a, std::uint8_t(v >> 8));
        write8(a + 1, std::uint8_t(v));
    }

    void write32(std::uint32_t a, std::uint32_t v) {
        write8(a, std::uint8_t(v >> 24));
        write8(a + 1, std::uint8_t(v >> 16));
        write8(a + 2, std::uint8_t(v >> 8));
        write8(a + 3, std::uint8_t(v));
    }

    std::uint8_t read_hidden16(std::uint32_t address) const {
        check(address, 2);
        return hidden_[address >> 1];
    }

    void write_hidden16(std::uint32_t address, std::uint8_t value) {
        check(address, 2);
        hidden_[address >> 1] = value & 3u;
    }

    void copy_be(std::uint32_t address, std::uint8_t *dst, std::size_t count) const {
        check(address, count);
        if (!count) return;
        if (!byteswapped_) {
            std::memcpy(dst, data_ + address, count);
            return;
        }
        std::size_t i = 0;
        for (; i < count && ((address + i) & 3u); ++i)
            dst[i] = data_[(address + i) ^ 3u];
        for (; i + 4 <= count; i += 4) {
            std::uint32_t word;
            std::memcpy(&word, data_ + address + i, sizeof(word));
            word = (word << 24) | ((word & 0xff00u) << 8) |
                   ((word >> 8) & 0xff00u) | (word >> 24);
            std::memcpy(dst + i, &word, sizeof(word));
        }
        for (; i < count; ++i)
            dst[i] = data_[(address + i) ^ 3u];
    }

    // A complete unchanged row can be committed without per-pixel bounds
    // checks or temporary packed-color storage. Keep coverage quantization
    // identical to the scalar framebuffer path, including its two hidden bits.
    void write_rgba16(std::uint32_t address, const std::uint8_t *rgba, std::size_t pixels) {
        if ((address & 1u) || pixels > size_ / 2)
            throw std::runtime_error("Invalid RGBA16 row");
        check(address, pixels * 2);
        if (byteswapped_)
            write_rgba16_row<true>(address, rgba, pixels);
        else
            write_rgba16_row<false>(address, rgba, pixels);
    }

    bool matches_be(std::uint32_t address, const std::uint8_t *snapshot, std::size_t count) const {
        check(address, count);
        if (!count)
            return true;
        if (!byteswapped_)
            return std::memcmp(data_ + address, snapshot, count) == 0;
        std::size_t i = 0;
        for (; i < count && ((address + i) & 3u); ++i)
            if (data_[(address + i) ^ 3u] != snapshot[i])
                return false;
        // Compare complete host words with one bounds check for the range.
        // Reversing the snapshot word works on either host byte order.
        for (; i + 4 <= count; i += 4) {
            std::uint32_t actual, expected;
            std::memcpy(&actual, data_ + address + i, sizeof(actual));
            std::memcpy(&expected, snapshot + i, sizeof(expected));
            expected = (expected << 24) | ((expected & 0xff00u) << 8) |
                       ((expected >> 8) & 0xff00u) | (expected >> 24);
            if (actual != expected)
                return false;
        }
        for (; i < count; ++i)
            if (data_[(address + i) ^ 3u] != snapshot[i])
                return false;
        return true;
    }

    std::size_t size() const {
        return size_;
    }

  private:
    template<bool Swapped>
    void write_rgba16_row(std::uint32_t address, const std::uint8_t *rgba, std::size_t pixels) {
        for (std::size_t i = 0; i < pixels; ++i) {
            const auto *p = rgba + i * 4;
            const std::uint16_t value = std::uint16_t((p[0] >> 3) << 11) |
                std::uint16_t((p[1] >> 3) << 6) | std::uint16_t((p[2] >> 3) << 1) | (p[3] > 128);
            const std::size_t a = address + i * 2;
            data_[Swapped ? (a ^ 3u) : a] = std::uint8_t(value >> 8);
            data_[Swapped ? ((a + 1) ^ 3u) : a + 1] = std::uint8_t(value);
            const unsigned samples = (unsigned(p[3]) * 8 + 127) / 255;
            hidden_[(address >> 1) + i] = std::uint8_t(samples ? samples - 1 : 0) & 3u;
        }
    }

    void check(std::uint32_t address, std::size_t count) const {
        if (!data_ || address > size_ || count > size_ - address)
            throw std::runtime_error("RDRAM access out of range");
    }
    std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
    bool byteswapped_ = true;
    std::vector<std::uint8_t> hidden_;
};
