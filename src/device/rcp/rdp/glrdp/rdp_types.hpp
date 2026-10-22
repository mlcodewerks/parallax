#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include "gpu_tmem_state.hpp"

enum class RDPOpcode : std::uint8_t {
    NoOp = 0x00,
    TriNoShade = 0x08,
    TriNoShadeZ = 0x09,
    TriTexture = 0x0a,
    TriTextureZ = 0x0b,
    TriShade = 0x0c,
    TriShadeZ = 0x0d,
    TriShadeTexture = 0x0e,
    TriShadeTextureZ = 0x0f,
    TextureRectangle = 0x24,
    TextureRectangleFlip = 0x25,
    SyncLoad = 0x26,
    SyncPipe = 0x27,
    SyncTile = 0x28,
    SyncFull = 0x29,
    SetKeyGB = 0x2a,
    SetKeyR = 0x2b,
    SetConvert = 0x2c,
    SetScissor = 0x2d,
    SetPrimDepth = 0x2e,
    SetOtherModes = 0x2f,
    LoadTLUT = 0x30,
    Reserved31 = 0x31,
    SetTileSize = 0x32,
    LoadBlock = 0x33,
    LoadTile = 0x34,
    SetTile = 0x35,
    FillRectangle = 0x36,
    SetFillColor = 0x37,
    SetFogColor = 0x38,
    SetBlendColor = 0x39,
    SetPrimColor = 0x3a,
    SetEnvColor = 0x3b,
    SetCombine = 0x3c,
    SetTextureImage = 0x3d,
    SetZImage = 0x3e,
    SetColorImage = 0x3f
};
constexpr bool rdp_is_triangle(RDPOpcode o) {
    auto v = std::uint8_t(o);
    return v >= 8 && v <= 15;
}
constexpr bool rdp_triangle_has_z(RDPOpcode o) {
    return (std::uint8_t(o) & 1) != 0;
}
constexpr bool rdp_triangle_has_texture(RDPOpcode o) {
    auto v = std::uint8_t(o);
    return v == 10 || v == 11 || v == 14 || v == 15;
}
constexpr bool rdp_triangle_has_shade(RDPOpcode o) {
    return std::uint8_t(o) >= 12;
}
constexpr std::size_t rdp_command_words(RDPOpcode o) {
    switch (o) {
    case RDPOpcode::TriNoShade:
        return 4;
    case RDPOpcode::TriNoShadeZ:
        return 6;
    case RDPOpcode::TriTexture:
        return 12;
    case RDPOpcode::TriTextureZ:
        return 14;
    case RDPOpcode::TriShade:
        return 12;
    case RDPOpcode::TriShadeZ:
        return 14;
    case RDPOpcode::TriShadeTexture:
        return 20;
    case RDPOpcode::TriShadeTextureZ:
        return 22;
    case RDPOpcode::TextureRectangle:
    case RDPOpcode::TextureRectangleFlip:
        return 2;
    default:
        return 1;
    }
}

enum class RDPImageFormat : std::uint8_t { RGBA = 0, YUV = 1, CI = 2, IA = 3, I = 4 };
enum class RDPImageSize : std::uint8_t { Bits4 = 0, Bits8 = 1, Bits16 = 2, Bits32 = 3 };
struct RDPImage {
    RDPImageFormat format = RDPImageFormat::RGBA;
    RDPImageSize size = RDPImageSize::Bits16;
    std::uint16_t width = 1;
    std::uint32_t address = 0;
};
struct RDPTile {
    RDPImageFormat format = RDPImageFormat::RGBA;
    RDPImageSize size = RDPImageSize::Bits16;
    std::uint16_t line = 0, tmem = 0;
    std::uint8_t palette = 0;
    bool clamp_t = false, mirror_t = false;
    std::uint8_t mask_t = 0, shift_t = 0;
    bool clamp_s = false, mirror_s = false;
    std::uint8_t mask_s = 0, shift_s = 0;
    std::uint16_t sl = 0, tl = 0, sh = 0, th = 0;
};
struct RDPColor {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
};
struct RDPEdgeCoefficients {
    bool right_major = false;
    std::uint8_t level = 0, tile = 0;
    std::int32_t yl = 0, ym = 0, yh = 0, xl = 0, dxldy = 0, xh = 0, dxhdy = 0, xm = 0, dxmdy = 0;
};
struct RDPAttribute4 {
    std::array<std::int32_t, 4> value{}, dx{}, de{}, dy{};
};
struct RDPZCoefficients {
    std::int32_t z = 0, dzdx = 0, dzde = 0, dzdy = 0;
};
struct RDPTriangle {
    RDPOpcode opcode = RDPOpcode::TriNoShade;
    RDPEdgeCoefficients edge{};
    bool shade_enabled = false, texture_enabled = false, z_enabled = false;
    RDPAttribute4 shade{}, texture{};
    RDPZCoefficients depth{};
};
struct RDPTextureRectangle {
    bool flipped = false;
    std::uint8_t tile = 0;
    std::uint16_t xh = 0, yh = 0, xl = 0, yl = 0;
    std::int16_t s = 0, t = 0, dsdx = 0, dtdy = 0;
};
struct RDPFillRectangle {
    std::uint16_t xl = 0, yl = 0, xh = 0, yh = 0;
};
struct RDPConvert {
    std::array<std::int16_t, 6> k{};
};
struct RDPKey {
    std::uint16_t width_r = 0, width_g = 0, width_b = 0;
    std::uint8_t center_r = 0, center_g = 0, center_b = 0, scale_r = 0, scale_g = 0, scale_b = 0;
};
struct RDPState {
    std::array<std::uint8_t, 4096> tmem{};
    std::shared_ptr<RDPTMEMBacking> gpu_tmem;
    std::array<RDPTile, 8> tiles{};
    RDPImage texture_image{}, color_image{};
    std::uint32_t z_image_address = 0;
    std::uint64_t other_modes = 0, combine = 0;
    std::uint16_t scissor_xh = 0, scissor_yh = 0, scissor_xl = 0, scissor_yl = 0;
    std::uint32_t fill_color = 0;
    RDPColor fog{}, blend{}, primitive{}, environment{};
    std::uint8_t primitive_min_lod = 0, primitive_lod_fraction = 0;
    std::uint16_t primitive_z = 0, primitive_dz = 0;
    RDPConvert convert{};
    RDPKey key{};
    std::uint64_t tmem_generation = 0;
};
