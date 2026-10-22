#include "rdp_parser.hpp"
#include "rdp_tmem.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

namespace {
std::uint64_t load_be64(const std::uint8_t *p) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}

std::int32_t sign_extend(std::uint32_t value, unsigned bits) {
    const std::uint32_t m = 1u << (bits - 1);
    const std::uint32_t mask = (bits == 32) ? 0xffffffffu : ((1u << bits) - 1u);
    value &= mask;
    return static_cast<std::int32_t>((value ^ m) - m);
}

RDPColor color32(std::uint64_t w) {
    return {std::uint8_t(w >> 24), std::uint8_t(w >> 16), std::uint8_t(w >> 8), std::uint8_t(w)};
}

RDPEdgeCoefficients decode_edges(std::span<const std::uint64_t> w) {
    RDPEdgeCoefficients e{};
    e.right_major = ((w[0] >> 55) & 1) != 0;
    e.level = std::uint8_t((w[0] >> 51) & 7);
    e.tile = std::uint8_t((w[0] >> 48) & 7);
    e.yl = sign_extend(std::uint32_t((w[0] >> 32) & 0x3fff), 14);
    e.ym = sign_extend(std::uint32_t((w[0] >> 16) & 0x3fff), 14);
    e.yh = sign_extend(std::uint32_t(w[0] & 0x3fff), 14);
    e.xl = sign_extend(std::uint32_t(w[1] >> 32), 28);
    e.dxldy = sign_extend(std::uint32_t(w[1]), 30);
    e.xh = sign_extend(std::uint32_t(w[2] >> 32), 28);
    e.dxhdy = sign_extend(std::uint32_t(w[2]), 30);
    e.xm = sign_extend(std::uint32_t(w[3] >> 32), 28);
    e.dxmdy = sign_extend(std::uint32_t(w[3]), 30);
    return e;
}

// Each 64-bit word packs the four 16-bit component halves in RGBA order.
// Value/DX integer halves precede their fractional halves, followed by DE/DY.
RDPAttribute4 decode_attribute(std::span<const std::uint64_t> w) {
    if (w.size() < 8)
        throw std::runtime_error("truncated RDP coefficient block");
    RDPAttribute4 a{};
    auto coeff = [&](unsigned component, unsigned integer_word, unsigned fraction_word) {
        const unsigned shift = 48u - component * 16u;
        const auto hi = std::uint32_t((w[integer_word] >> shift) & 0xffff);
        const auto lo = std::uint32_t((w[fraction_word] >> shift) & 0xffff);
        return std::int32_t((hi << 16) | lo);
    };
    for (unsigned c = 0; c < 4; ++c) {
        a.value[c] = coeff(c, 0, 2);
        a.dx[c] = coeff(c, 1, 3);
        a.de[c] = coeff(c, 4, 6);
        a.dy[c] = coeff(c, 5, 7);
    }
    return a;
}

RDPTriangle decode_triangle(RDPOpcode op, std::span<const std::uint64_t> w) {
    RDPTriangle t{};
    t.opcode = op;
    t.edge = decode_edges(w.first(4));
    t.shade_enabled = rdp_triangle_has_shade(op);
    t.texture_enabled = rdp_triangle_has_texture(op);
    t.z_enabled = rdp_triangle_has_z(op);
    std::size_t off = 4;
    if (t.shade_enabled) {
        t.shade = decode_attribute(w.subspan(off, 8));
        off += 8;
    }
    if (t.texture_enabled) {
        t.texture = decode_attribute(w.subspan(off, 8));
        off += 8;
    }
    if (t.z_enabled) {
        t.depth.z = std::int32_t(std::uint32_t(w[off] >> 32));
        t.depth.dzdx = std::int32_t(std::uint32_t(w[off]));
        t.depth.dzde = std::int32_t(std::uint32_t(w[off + 1] >> 32));
        t.depth.dzdy = std::int32_t(std::uint32_t(w[off + 1]));
    }
    return t;
}

RDPImage decode_image(std::uint64_t w) {
    RDPImage i{};
    i.format = RDPImageFormat((w >> 53) & 7);
    i.size = RDPImageSize((w >> 51) & 3);
    i.width = std::uint16_t(((w >> 32) & 0x3ff) + 1);
    i.address = std::uint32_t(w & 0x03ffffff);
    return i;
}

void decode_tile(RDPState &s, std::uint64_t w) {
    auto &t = s.tiles[(w >> 24) & 7];
    t.format = RDPImageFormat((w >> 53) & 7);
    t.size = RDPImageSize((w >> 51) & 3);
    t.line = std::uint16_t((w >> 41) & 0x1ff);
    t.tmem = std::uint16_t((w >> 32) & 0x1ff);
    t.palette = std::uint8_t((w >> 20) & 0xf);
    t.clamp_t = ((w >> 19) & 1) != 0;
    t.mirror_t = ((w >> 18) & 1) != 0;
    t.mask_t = std::uint8_t((w >> 14) & 0xf);
    t.shift_t = std::uint8_t((w >> 10) & 0xf);
    t.clamp_s = ((w >> 9) & 1) != 0;
    t.mirror_s = ((w >> 8) & 1) != 0;
    t.mask_s = std::uint8_t((w >> 4) & 0xf);
    t.shift_s = std::uint8_t(w & 0xf);
}

void decode_bounds(RDPState &s, std::uint64_t w) {
    auto &t = s.tiles[(w >> 24) & 7];
    t.sl = std::uint16_t((w >> 44) & 0xfff);
    t.tl = std::uint16_t((w >> 32) & 0xfff);
    t.sh = std::uint16_t((w >> 12) & 0xfff);
    t.th = std::uint16_t(w & 0xfff);
}
} // namespace

RDPCommandParser::RDPCommandParser(std::span<const std::uint8_t> rdram) : rdram_(rdram) {
}
void RDPCommandParser::set_rdram(std::span<const std::uint8_t> r) {
    rdram_ = r;
}
void RDPCommandParser::reset() {
    state_ = {};
    pending_.clear();
}
void RDPCommandParser::feed(std::span<const std::uint8_t> b) {
    pending_.insert(pending_.end(), b.begin(), b.end());
    parse_pending();
}
void RDPCommandParser::execute(std::span<const std::uint8_t> b) {
    feed(b);
    if (!pending_.empty())
        throw std::runtime_error("incomplete RDP command stream");
}

void RDPCommandParser::discard_pending() {
    pending_.clear();
}

void RDPCommandParser::parse_pending() {
    std::size_t used = 0;
    while (pending_.size() - used >= 8) {
        const auto first = load_be64(pending_.data() + used);
        const auto op = RDPOpcode((first >> 56) & 0x3f);
        const auto words = rdp_command_words(op);
        if (pending_.size() - used < words * 8)
            break;
        std::array<std::uint64_t, 22> w{};
        for (std::size_t i = 0; i < words; ++i)
            w[i] = load_be64(pending_.data() + used + i * 8);
        dispatch(op, std::span<const std::uint64_t>(w.data(), words));
        used += words * 8;
    }
    if (used)
        pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(used));
}

void RDPCommandParser::dispatch(RDPOpcode op, std::span<const std::uint64_t> w) {
    const auto x = w[0];
    if (rdp_is_triangle(op)) {
        if (callbacks_.draw_triangle)
            callbacks_.draw_triangle(decode_triangle(op, w), state_);
        return;
    }
    switch (op) {
    case RDPOpcode::NoOp:
        break;
    case RDPOpcode::TextureRectangle:
    case RDPOpcode::TextureRectangleFlip:
        if (callbacks_.draw_texture_rectangle) {
            RDPTextureRectangle r{};
            r.flipped = op == RDPOpcode::TextureRectangleFlip;
            // RDP encodes the lower-right corner in the high word.
            r.xl = std::uint16_t((x >> 44) & 0xfff);
            r.yl = std::uint16_t((x >> 32) & 0xfff);
            r.tile = std::uint8_t((x >> 24) & 7);
            r.xh = std::uint16_t((x >> 12) & 0xfff);
            r.yh = std::uint16_t(x & 0xfff);
            r.s = std::int16_t(w[1] >> 48);
            r.t = std::int16_t(w[1] >> 32);
            r.dsdx = std::int16_t(w[1] >> 16);
            r.dtdy = std::int16_t(w[1]);
            callbacks_.draw_texture_rectangle(r, state_);
        }
        break;
    case RDPOpcode::SyncLoad:
        if (callbacks_.sync_load)
            callbacks_.sync_load();
        break;
    case RDPOpcode::SyncPipe:
        if (callbacks_.sync_pipe)
            callbacks_.sync_pipe();
        break;
    case RDPOpcode::SyncTile:
        if (callbacks_.sync_tile)
            callbacks_.sync_tile();
        break;
    case RDPOpcode::SetKeyGB:
        state_.key.width_g = std::uint16_t((x >> 44) & 0xfff);
        state_.key.width_b = std::uint16_t((x >> 32) & 0xfff);
        state_.key.center_g = std::uint8_t(x >> 24);
        state_.key.scale_g = std::uint8_t(x >> 16);
        state_.key.center_b = std::uint8_t(x >> 8);
        state_.key.scale_b = std::uint8_t(x);
        break;
    case RDPOpcode::SetKeyR:
        state_.key.width_r = std::uint16_t((x >> 16) & 0xfff);
        state_.key.center_r = std::uint8_t(x >> 8);
        state_.key.scale_r = std::uint8_t(x);
        break;
    case RDPOpcode::SetConvert:
        for (unsigned i = 0; i < 6; ++i) {
            unsigned sh = 45 - i * 9;
            state_.convert.k[i] = std::int16_t(sign_extend(std::uint32_t((x >> sh) & 0x1ff), 9));
        }
        break;
    case RDPOpcode::SetScissor:
        state_.scissor_xh = std::uint16_t((x >> 44) & 0xfff);
        state_.scissor_yh = std::uint16_t((x >> 32) & 0xfff);
        state_.scissor_xl = std::uint16_t((x >> 12) & 0xfff);
        state_.scissor_yl = std::uint16_t(x & 0xfff);
        break;
    case RDPOpcode::SetPrimDepth:
        state_.primitive_z = std::uint16_t(x >> 16);
        state_.primitive_dz = std::uint16_t(x);
        break;
    case RDPOpcode::SetOtherModes:
        state_.other_modes = x;
        break;
    case RDPOpcode::LoadTLUT:
        if (callbacks_.load_texture && callbacks_.load_texture(state_, op, x)) {
            decode_bounds(state_, x);
            break;
        }
        if (callbacks_.before_texture_load)
            callbacks_.before_texture_load(state_, op, x);
        decode_bounds(state_, x);
        rdp_load_tlut(state_, x, rdram_);
        break;
    case RDPOpcode::SetTileSize:
        decode_bounds(state_, x);
        break;
    case RDPOpcode::LoadBlock:
        if (callbacks_.load_texture && callbacks_.load_texture(state_, op, x)) {
            decode_bounds(state_, x);
            break;
        }
        if (callbacks_.before_texture_load)
            callbacks_.before_texture_load(state_, op, x);
        decode_bounds(state_, x);
        rdp_load_block(state_, x, rdram_);
        break;
    case RDPOpcode::LoadTile:
        if (callbacks_.load_texture && callbacks_.load_texture(state_, op, x)) {
            decode_bounds(state_, x);
            break;
        }
        if (callbacks_.before_texture_load)
            callbacks_.before_texture_load(state_, op, x);
        decode_bounds(state_, x);
        rdp_load_tile(state_, x, rdram_);
        break;
    case RDPOpcode::SetTile:
        decode_tile(state_, x);
        break;
    case RDPOpcode::FillRectangle:
        if (callbacks_.draw_fill_rectangle) {
            RDPFillRectangle r{};
            r.xl = std::uint16_t((x >> 44) & 0xfff);
            r.yl = std::uint16_t((x >> 32) & 0xfff);
            r.xh = std::uint16_t((x >> 12) & 0xfff);
            r.yh = std::uint16_t(x & 0xfff);
            callbacks_.draw_fill_rectangle(r, state_);
        }
        break;
    case RDPOpcode::SetFillColor:
        state_.fill_color = std::uint32_t(x);
        break;
    case RDPOpcode::SetFogColor:
        state_.fog = color32(x);
        break;
    case RDPOpcode::SetBlendColor:
        state_.blend = color32(x);
        break;
    case RDPOpcode::SetPrimColor:
        state_.primitive_min_lod = std::uint8_t((x >> 40) & 0x1f);
        state_.primitive_lod_fraction = std::uint8_t((x >> 32) & 0xff);
        state_.primitive = color32(x);
        break;
    case RDPOpcode::SetEnvColor:
        state_.environment = color32(x);
        break;
    case RDPOpcode::SetCombine:
        state_.combine = x;
        break;
    case RDPOpcode::SetTextureImage:
        state_.texture_image = decode_image(x);
        break;
    case RDPOpcode::SetZImage: {
        const auto address = std::uint32_t(x & 0x03ffffff);
        if (address != state_.z_image_address && state_.z_image_address &&
            callbacks_.before_z_image_change)
            callbacks_.before_z_image_change(state_);
        state_.z_image_address = address;
        break;
    }
    case RDPOpcode::SetColorImage:
        state_.color_image = decode_image(x);
        break;
    case RDPOpcode::SyncFull:
        if (callbacks_.sync_full)
            callbacks_.sync_full();
        break;
    default:
        break;
    }
}
