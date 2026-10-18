#include "rdp_tmem.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace {
std::uint32_t bpp(RDPImageSize s) {
    return 4u << std::uint32_t(s);
}
std::size_t swizzle(std::size_t a, std::uint32_t row, RDPImageSize size) {
    a &= 4095;
    if (row & 1)
        a ^= 4;
    return a & 4095;
}
void wb(RDPState &s, std::size_t a, std::uint32_t row, RDPImageSize z, std::uint8_t v) {
    s.tmem[swizzle(a, row, z)] = v;
}
std::uint8_t rb(const RDPState &s, std::size_t a, std::uint32_t row, RDPImageSize z) {
    return s.tmem[swizzle(a, row, z)];
}
std::uint8_t e3(unsigned x) {
    return std::uint8_t((x * 255 + 3) / 7);
}
std::uint8_t e4(unsigned x) {
    return std::uint8_t(x * 17);
}
std::uint8_t e5(unsigned x) {
    return std::uint8_t((x << 3) | (x >> 2));
}
struct C {
    std::uint8_t r, g, b, a;
};
C rgba16(std::uint16_t v) {
    return {e5((v >> 11) & 31), e5((v >> 6) & 31), e5((v >> 1) & 31),
            std::uint8_t((v & 1) ? 255 : 0)};
}
C ia16(std::uint16_t v) {
    auto i = std::uint8_t(v >> 8);
    return {i, i, i, std::uint8_t(v)};
}
C yuv(std::uint8_t y, std::uint8_t u, std::uint8_t v) {
    double Y = y, U = int(u) - 128, V = int(v) - 128;
    auto q = [](double x) { return std::uint8_t(std::clamp<int>(int(std::lround(x)), 0, 255)); };
    return {q(Y + 1.402 * V), q(Y - .344136 * U - .714136 * V), q(Y + 1.772 * U), 255};
}
C tlut(const RDPState &s, std::uint8_t i) {
    std::size_t a = (0x800 + std::size_t(i) * 8) & 0xfff;
    auto v = std::uint16_t((s.tmem[a] << 8) | s.tmem[(a + 1) & 0xfff]);
    return ((s.other_modes >> 46) & 1) ? ia16(v) : rgba16(v);
}
int modp(int v, int m) {
    int r = v % m;
    return r < 0 ? r + m : r;
}
int shiftc(int c, std::uint8_t sh) {
    // RDP S/T shifts are signed arithmetic shifts for 0..10 and left shifts
    // for 11..15. Preserve sign and constrain the result to the coordinate
    // datapath instead of relying on implementation-specific overflow.
    if (sh == 0)
        return c;
    if (sh <= 10)
        return c >> sh;
    const unsigned amount = 16u - sh;
    const std::int64_t widened = std::int64_t(c) * (std::int64_t(1) << amount);
    return int(std::clamp<std::int64_t>(widened, -0x40000000ll, 0x3fffffffll));
}
int coord(int c, std::uint16_t lo, std::uint16_t hi, std::uint8_t sh, std::uint8_t mask,
          bool mirror, bool clamp) {
    c = shiftc(c, sh) - int(lo);
    int t = c >> 2;
    // Mask/mirror belongs to the wrapping stage; clamp is the final tile
    // boundary operation. The old order allowed masking to undo a clamp.
    if (mask) {
        int p = 1 << mask;
        if (mirror) {
            int q = modp(t, p * 2);
            t = q >= p ? p * 2 - 1 - q : q;
        } else
            t = modp(t, p);
    }
    if (clamp)
        t = std::clamp(t, 0, std::max(0, (int(hi) - int(lo)) >> 2));
    return t;
}
C sample(const RDPState &s, const RDPTile &t, unsigned x, unsigned y) {
    std::size_t base = std::size_t(t.tmem) * 8 + std::size_t(y) * std::size_t(t.line) * 8;
    // TLUT index fetches always use the low TMEM half. YUV descriptors use
    // the byte index path independently of their declared texel size.
    const bool palette_enabled = ((s.other_modes >> 47) & 1) != 0;
    if (palette_enabled && t.format == RDPImageFormat::YUV) {
        const auto index = rb(s, (base + x) & 0x7ff, y, RDPImageSize::Bits8);
        return tlut(s, t.size == RDPImageSize::Bits4 ? std::uint8_t((t.palette << 4) | (index >> 4))
                                                     : index);
    }
    switch (t.size) {
    case RDPImageSize::Bits4: {
        auto p = rb(s, palette_enabled ? ((base + x / 2) & 0x7ff) : (base + x / 2), y, t.size);
        auto v = std::uint8_t((x & 1) ? p & 15 : p >> 4);
        if ((s.other_modes >> 47) & 1)
            return tlut(s, std::uint8_t((t.palette << 4) | v));
        if (t.format == RDPImageFormat::I || t.format == RDPImageFormat::RGBA ||
            t.format == RDPImageFormat::CI) {
            auto i = e4(v);
            return {i, i, i, i};
        }
        if (t.format == RDPImageFormat::IA) {
            auto i = e3((v >> 1) & 7);
            return {i, i, i, std::uint8_t((v & 1) ? 255 : 0)};
        }
        if (t.format == RDPImageFormat::CI)
            return tlut(s, std::uint8_t((t.palette << 4) | v));
        break;
    }
    case RDPImageSize::Bits8: {
        auto v = rb(s, palette_enabled ? ((base + x) & 0x7ff) : (base + x), y, t.size);
        if ((s.other_modes >> 47) & 1)
            return tlut(s, v);
        if (t.format == RDPImageFormat::I || t.format == RDPImageFormat::RGBA ||
            t.format == RDPImageFormat::CI)
            return {v, v, v, v};
        if (t.format == RDPImageFormat::IA) {
            auto i = e4(v >> 4);
            return {i, i, i, e4(v & 15)};
        }
        if (t.format == RDPImageFormat::CI)
            return tlut(s, v);
        break;
    }
    case RDPImageSize::Bits16: {
        if ((s.other_modes >> 47) & 1) {
            const std::size_t address = (base + x * 2) & 0x7ff;
            return tlut(s, rb(s, address, y, t.size));
        }
        if (t.format == RDPImageFormat::YUV) {
            const auto pair = (base + (x & ~1u)) & 0x7ff;
            const auto u = rb(s, pair, y, RDPImageSize::Bits8);
            const auto v = rb(s, pair + 1, y, RDPImageSize::Bits8);
            const auto luma = rb(s, ((base + x) & 0x7ff) | 0x800, y, RDPImageSize::Bits8);
            // Keep the biased chroma until the GPU's texture pipeline applies
            // SetConvert. Converting here loses signed/overflow values and
            // applies conversion before the RDP's interpolation stage.
            return {u, v, luma, luma};
        }
        auto v = std::uint16_t((rb(s, base + x * 2, y, t.size) << 8) |
                               rb(s, base + x * 2 + 1, y, t.size));
        if (t.format == RDPImageFormat::RGBA)
            return rgba16(v);
        if (t.format == RDPImageFormat::IA)
            return ia16(v);
        break;
    }
    case RDPImageSize::Bits32: {
        std::size_t lo = (base + x * 2) & 0x7ff, hi = lo | 0x800;
        if ((s.other_modes >> 47) & 1)
            return tlut(s, rb(s, lo, y, t.size));
        return {rb(s, lo, y, t.size), rb(s, lo + 1, y, t.size), rb(s, hi, y, t.size),
                rb(s, hi + 1, y, t.size)};
    }
    }
    return {255, 0, 255, 255};
}
// LoadTile and LoadBlock share the 64-bit load pipe. Source size controls
// burst stepping; the load tile controls bank addressing and split packing.
void load_word(RDPState &s, const RDPTile &t, RDPMemoryView r, std::uint64_t source, unsigned texel,
               unsigned row) {
    const unsigned shorts = (t.size == RDPImageSize::Bits8 || t.format == RDPImageFormat::YUV)
                                ? texel >> 1
                                : (t.size == RDPImageSize::Bits4 ? texel >> 2 : texel);
    const unsigned start = (((t.tmem + ((t.line * row) & 511)) * 4) + shorts) & 0x7fd;
    unsigned bank[4];
    for (unsigned lane = 0; lane < 4; ++lane) {
        const unsigned index = ((start + lane) & 0x7ff) ^ ((row & 1) ? 2 : 0);
        bank[index & 3] = index & 0x3ff;
    }
    auto store = [&](unsigned index, unsigned a, unsigned b) {
        s.tmem[index * 2] = r[source + a];
        s.tmem[index * 2 + 1] = r[source + b];
    };
    const bool split = t.format == RDPImageFormat::RGBA && t.size == RDPImageSize::Bits32;
    if (t.format == RDPImageFormat::YUV) {
        const unsigned lane = (((shorts & 2) != 0) ^ ((row & 1) != 0)) ? 2 : 0;
        store(bank[lane], 0, 2);
        store(bank[lane + 1], 4, 6);
        store(bank[lane] | 0x400, 1, 3);
        store(bank[lane + 1] | 0x400, 5, 7);
    } else if (split) {
        const unsigned lane = (((shorts & 2) != 0) ^ ((row & 1) != 0)) ? 2 : 0;
        store(bank[lane], 0, 1);
        store(bank[lane + 1], 4, 5);
        store(bank[lane] | 0x400, 2, 3);
        store(bank[lane + 1] | 0x400, 6, 7);
    } else {
        for (unsigned lane = 0; lane < 4; ++lane) {
            const unsigned part = lane ^ ((row & 1) ? 2 : 0);
            store(bank[lane] | (start & 0x400), part * 2, part * 2 + 1);
        }
    }
}
} // namespace
void rdp_load_tile(RDPState &s, std::uint64_t cmd, RDPMemoryView r) {
    auto ti = std::uint8_t((cmd >> 24) & 7);
    auto &t = s.tiles[ti];
    unsigned sl = ((cmd >> 44) & 0xfff) >> 2, tl = ((cmd >> 32) & 0xfff) >> 2,
             sh = ((cmd >> 12) & 0xfff) >> 2, th = (cmd & 0xfff) >> 2;
    if (sh < sl || th < tl)
        return;
    const unsigned bits = bpp(s.texture_image.size);
    // Four-bit texture images cannot enter the RDP load pipe. Games load
    // packed four-bit textures through an eight/sixteen-bit image descriptor.
    if (bits < 8)
        return;
    const unsigned words = ((sh - sl + 1) * bits + 63) / 64;
    for (unsigned y = 0; y <= th - tl; ++y) {
        const auto source = std::uint64_t(s.texture_image.address) +
                            (std::uint64_t(tl + y) * s.texture_image.width + sl) * (bits / 8);
        for (unsigned word = 0; word < words; ++word)
            load_word(s, t, r, source + word * 8, word * (64 / bits), y);
    }
    ++s.tmem_generation;
}
void rdp_load_block(RDPState &s, std::uint64_t cmd, RDPMemoryView r) {
    auto ti = std::uint8_t((cmd >> 24) & 7);
    auto &t = s.tiles[ti];
    unsigned sl = ((cmd >> 44) & 0xfff), tl = ((cmd >> 32) & 0xfff), sh = ((cmd >> 12) & 0xfff),
             dxt = unsigned(cmd & 0xfff);
    if (sh < sl)
        return;
    unsigned bits = bpp(s.texture_image.size), count = sh - sl + 1;
    std::uint64_t source_texel = std::uint64_t(tl) * s.texture_image.width + sl;
    std::size_t base = std::size_t(t.tmem) * 8;
    if (bits >= 8) {
        // The image size controls the source stream, while the load tile
        // controls TMEM addressing and bank packing. Games can reinterpret
        // the stream with a different tile size before drawing it.
        const unsigned words = (count * bits + 63) / 64;
        const auto source = std::uint64_t(s.texture_image.address) + source_texel * bits / 8;
        for (unsigned word = 0; word < words; ++word) {
            const unsigned row = (word * dxt) >> 11;
            const unsigned texel = word * (64 / bits);
            load_word(s, t, r, source + word * 8, texel, row);
        }
        ++s.tmem_generation;
        return;
    }
    unsigned word = 0, row = 0, acc = 0, last_qword = 0;
    for (unsigned x = 0; x < count; ++x) {
        unsigned qword = (x * bits) >> 6;
        if (qword != last_qword) {
            word += qword - last_qword;
            last_qword = qword;
            acc += dxt;
            if (acc >= 0x800) {
                row += acc >> 11;
                acc &= 0x7ff;
            }
        }
        std::size_t dst = base + std::size_t(word) * 8 + ((x * bits) & 63) / 8;
        std::uint64_t tex = source_texel + x;
        if (bits == 4) {
            auto sa = s.texture_image.address + tex / 2;
            auto n = std::uint8_t((tex & 1) ? r[sa] & 15 : r[sa] >> 4);
            auto a = swizzle(dst, row, t.size);
            auto &d = s.tmem[a];
            d = std::uint8_t(((x * bits) & 4) ? ((d & 0xf0) | n) : ((d & 0x0f) | (n << 4)));
        } else if (bits == 32) {
            auto sa = s.texture_image.address + tex * 4;
            // Each 64-bit source word contributes four bytes to each TMEM bank.
            std::size_t lo = (base + std::size_t(x) * 2) & 0x7ff;
            wb(s, lo, row, t.size, r[sa]);
            wb(s, lo + 1, row, t.size, r[sa + 1]);
            wb(s, lo | 0x800, row, t.size, r[sa + 2]);
            wb(s, (lo + 1) | 0x800, row, t.size, r[sa + 3]);
        } else {
            unsigned bytes = bits / 8;
            auto sa = s.texture_image.address + tex * bytes;
            for (unsigned b = 0; b < bytes; ++b)
                wb(s, dst + b, row, t.size, r[sa + b]);
        }
    }
    ++s.tmem_generation;
}
void rdp_load_tlut(RDPState &s, std::uint64_t cmd, RDPMemoryView r) {
    auto ti = std::uint8_t((cmd >> 24) & 7);
    auto &t = s.tiles[ti];
    unsigned sl = ((cmd >> 44) & 0xfff) >> 2, tl = ((cmd >> 32) & 0xfff) >> 2,
             sh = ((cmd >> 12) & 0xfff) >> 2;
    if (sh < sl)
        return;
    unsigned count = sh - sl + 1;
    auto base = std::uint64_t(s.texture_image.address) +
                (std::uint64_t(tl) * s.texture_image.width + sl) * 2;
    for (unsigned i = 0; i < count; ++i) {
        // A TLUT transfer replicates each source halfword over the four
        // banks. Its load coordinates advance by sixteen; the load tile's
        // size controls their conversion to halfword addresses, just as
        // for a block load. The usual 4-bit load tile advances eight bytes.
        const unsigned shorts = i * (t.format == RDPImageFormat::YUV ? 8u
                                     : t.size == RDPImageSize::Bits4 ? 4u
                                     : t.size == RDPImageSize::Bits8 ? 8u
                                                                     : 16u);
        const unsigned start = ((unsigned(t.tmem) * 4u) + shorts) & 0x7fdu;
        if (t.format == RDPImageFormat::YUV) {
            const auto high = r[base + i * 2], low = r[base + i * 2 + 1];
            const unsigned first = (shorts & 2u) ? 2u : 0u;
            for (unsigned lane = first; lane < first + 2; ++lane) {
                const unsigned index = (start + lane) & 0x3ffu;
                s.tmem[index * 2] = s.tmem[index * 2 + 1] = high;
                s.tmem[(index | 0x400u) * 2] = s.tmem[(index | 0x400u) * 2 + 1] = low;
            }
            continue;
        }
        const bool split = t.format == RDPImageFormat::RGBA && t.size == RDPImageSize::Bits32;
        const unsigned first = split ? ((shorts & 2u) ? 2u : 0u) : 0u;
        const unsigned lanes = split ? 2u : 4u;
        const auto high = r[base + i * 2], low = r[base + i * 2 + 1];
        for (unsigned lane = first; lane < first + lanes; ++lane) {
            const unsigned index = ((start + lane) & 0x3ffu) | (split ? 0u : (start & 0x400u));
            s.tmem[index * 2] = high;
            s.tmem[index * 2 + 1] = low;
            if (split) {
                s.tmem[(index | 0x400u) * 2] = high;
                s.tmem[(index | 0x400u) * 2 + 1] = low;
            }
        }
    }
    ++s.tmem_generation;
}
DecodedTexture rdp_decode_tile(const RDPState &s, std::uint8_t ti) {
    const auto &t = s.tiles[ti & 7];
    if (rdp_linear_copy_tile(s, ti)) {
        // Copy fetches wrap in TMEM and ignore tile clamp bounds. Store all
        // halfwords in a compact texture; the GPU applies row/bank addressing.
        DecodedTexture d{512, 4, {}};
        d.rgba.resize(2048 * 4);
        for (unsigned i = 0; i < 2048; ++i) {
            const auto address = std::size_t(t.tmem) * 8 + i * 2;
            const auto c = rgba16(
                std::uint16_t((rb(s, address, 0, t.size) << 8) | rb(s, address + 1, 0, t.size)));
            d.rgba[i * 4] = c.r;
            d.rgba[i * 4 + 1] = c.g;
            d.rgba[i * 4 + 2] = c.b;
            d.rgba[i * 4 + 3] = c.a;
        }
        return d;
    }
    DecodedTexture d{};
    // SetTileSize is not an ordered host rectangle. The RDP subtracts
    // integer tile endpoints in a 10-bit datapath, including wrapped bounds.
    // Rejecting these tiles left draws sampling a previous GL binding.
    d.width = (((t.sh >> 2) - (t.sl >> 2)) & 0x3ff) + 1;
    d.height = (((t.th >> 2) - (t.tl >> 2)) & 0x3ff) + 1;
    // Unclamped wrapped tiles only address their mask period. Sin and
    // Punishment uses this for scrolling 128x64 tiles with inverted bounds.
    if (t.sh < t.sl && t.mask_s && !t.clamp_s)
        d.width = 1u << std::min(unsigned(t.mask_s), 10u);
    if (t.th < t.tl && t.mask_t && !t.clamp_t)
        d.height = 1u << std::min(unsigned(t.mask_t), 10u);
    if (!d.width || !d.height || d.width > 2048 || d.height > 2048)
        throw std::runtime_error("invalid TMEM tile dimensions");
    d.rgba.resize(std::size_t(d.width) * d.height * 4);
    for (unsigned y = 0; y < d.height; ++y)
        for (unsigned x = 0; x < d.width; ++x) {
            auto c = sample(s, t, x, y);
            auto o = (std::size_t(y) * d.width + x) * 4;
            d.rgba[o] = c.r;
            d.rgba[o + 1] = c.g;
            d.rgba[o + 2] = c.b;
            d.rgba[o + 3] = c.a;
        }
    return d;
}
