#pragma once
#include "primitive_batch.hpp"
#include "rdp_types.hpp"
#include <bit>
#include <utility>

// Native full-coverage triangles can share a bounding box without touching a
// destination pixel. Test the same integer sample rules as fast_triangle_pixel
// before introducing a framebuffer dependency between adjacent faces.
struct PrimitiveFootprint {
    PrimitiveBounds bounds;
    RDPEdgeCoefficients edge{};
    bool native_triangle = false, first_sample_only = false;

    std::pair<int, int> row(int y) const {
        const auto full = std::pair{bounds.x, bounds.x + bounds.w};
        if (!native_triangle) return full;
        auto at = [](int x, int slope, int steps) {
            return std::bit_cast<std::int32_t>(std::uint32_t(x & ~1) +
                std::uint32_t((slope >> 2) & ~1) * std::uint32_t(steps));
        };
        auto span = [&](int subrow) {
            const int major = at(edge.xh, edge.dxhdy, subrow - (edge.yh & ~3));
            const int minor = subrow < edge.ym
                ? at(edge.xm, edge.dxmdy, subrow - (edge.yh & ~3))
                : at(edge.xl, edge.dxldy, subrow - edge.ym);
            return edge.right_major ? std::pair{major, minor} : std::pair{minor, major};
        };
        auto ceil_pixel = [](std::int64_t x) { return int(-((-x) >> 16)); };
        auto clip = [&](int left, int right) {
            return std::pair{std::max(full.first, left), std::min(full.second, right)};
        };
        const int subrow = y * 4;
        if (first_sample_only) {
            if (subrow < edge.yh || subrow >= edge.yl) return std::pair{0, 0};
            const auto [left, right] = span(subrow);
            return clip(ceil_pixel(left), ceil_pixel(right));
        }
        const int lo = std::max(subrow, int(edge.yh));
        const int hi = std::min(subrow + 3, int(edge.yl) - 1);
        if (lo > hi) return std::pair{0, 0};
        int first = full.second, last = full.first;
        if (subrow + 2 >= edge.yh && subrow + 2 < edge.yl) {
            const auto [left, right] = span(subrow + 2);
            first = ceil_pixel(std::int64_t(left) - 32768);
            last = ceil_pixel(std::int64_t(right) - 32768);
        }
        auto a = span(lo), b = span(hi);
        int left = std::min(a.first, b.first), right = std::max(a.second, b.second);
        std::int64_t width = std::max(std::int64_t(a.second) - a.first, std::int64_t(b.second) - b.first);
        if (edge.ym > lo && edge.ym <= hi) {
            a = span(edge.ym - 1); b = span(edge.ym);
            left = std::min({left, a.first, b.first});
            right = std::max({right, a.second, b.second});
            width = std::max({width, std::int64_t(a.second) - a.first, std::int64_t(b.second) - b.first});
        }
        // GLSL width subtraction can wrap on malformed/offscreen packets.
        // Keep their original conservative dependency rather than infer a gap.
        if (width > INT32_MAX || width < INT32_MIN) return full;
        if (width < 65536 && right > left) {
            first = std::min(first, left >> 16);
            last = std::max(last, ceil_pixel(right));
        }
        return clip(first, last);
    }
    bool intersects(const PrimitiveFootprint &b) const {
        if (!bounds.intersects(b.bounds)) return false;
        if (!native_triangle && !b.native_triangle) return true;
        for (int y = std::max(bounds.y, b.bounds.y), end = std::min(bounds.y + bounds.h, b.bounds.y + b.bounds.h); y < end; ++y) {
            const auto a_row = row(y), b_row = b.row(y);
            if (a_row.first < a_row.second && b_row.first < b_row.second &&
                a_row.first < b_row.second && b_row.first < a_row.second) return true;
        }
        return false;
    }
};
