#pragma once
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Pixel-exact entries are immutable until the renderer flushes and resets the
// atlas. Shader-side tile addressing precedes the atlas offset, so filtering,
// wrapping and mirroring cannot bleed into neighbouring allocations.
class TextureAtlas {
  public:
    using Region = std::array<int, 4>;
    struct Entry { Region region; std::vector<std::uint8_t> pixels; };
    int size = 2048;
    void clear() { entries_.clear(); shelves_.clear(); used_height_ = 0; }
    static std::uint64_t key(unsigned w, unsigned h, const std::vector<std::uint8_t> &pixels) {
        std::uint64_t hash = (std::uint64_t(w) << 32) | h;
        for (auto byte : pixels) hash = (hash ^ byte) * UINT64_C(1099511628211);
        return hash;
    }
    const Entry *find(std::uint64_t key, unsigned w, unsigned h,
                      const std::vector<std::uint8_t> &pixels) const {
        const auto range = entries_.equal_range(key);
        for (auto i = range.first; i != range.second; ++i)
            if (i->second.region[2] == int(w) && i->second.region[3] == int(h) &&
                i->second.pixels == pixels) return &i->second;
        return nullptr;
    }
    Region allocate(int w, int h) {
        if (w <= 0 || h <= 0 || w > size || h > size) return {};
        Shelf *best = nullptr;
        for (auto &s : shelves_)
            if (s.height >= h && s.x + w <= size && (!best || s.height < best->height)) best = &s;
        if (!best) {
            if (used_height_ + h > size) return {};
            shelves_.push_back({0, used_height_, h}); best = &shelves_.back();
            used_height_ += h;
        }
        const Region region = {best->x, best->y, w, h}; best->x += w;
        return region;
    }
    void insert(std::uint64_t key, Region region, std::vector<std::uint8_t> pixels) {
        entries_.emplace(key, Entry{region, std::move(pixels)});
    }
  private:
    struct Shelf { int x, y, height; };
    std::vector<Shelf> shelves_;
    int used_height_ = 0;
    std::unordered_multimap<std::uint64_t, Entry> entries_;
};
