#pragma once
#include "rdp_tmem.hpp"
#include "texture_atlas.hpp"
#include <cstring>
#include <list>
#include <iterator>
#include <unordered_map>

// Atlas regions are immutable until the atlas is cleared. Remember the exact
// TMEM input to avoid decoding and hashing the same expanded pixels again.
class TextureDecodeCache {
  public:
    struct Key {
        std::array<std::uint64_t, 19> parameters;
        std::uint64_t hash;
    };
    explicit TextureDecodeCache(std::size_t capacity = 256) : capacity_(capacity) {}
    TextureDecodeCache(const TextureDecodeCache &) = delete;
    TextureDecodeCache &operator=(const TextureDecodeCache &) = delete;

    static Key key(const RDPState &s, unsigned tile) {
        const auto &t = s.tiles[tile & 7];
        Key result{{std::uint64_t(t.format), std::uint64_t(t.size), t.line, t.tmem,
                    t.palette, t.sl, t.tl, t.sh, t.th, t.mask_s, t.shift_s,
                    t.clamp_s, t.mirror_s, t.mask_t, t.shift_t, t.clamp_t,
                    t.mirror_t, (s.other_modes >> 46) & 3, rdp_linear_copy_tile(s, tile)},
                   UINT64_C(14695981039346656037)};
        auto mix = [&](std::uint64_t word) {
            result.hash = (result.hash ^ word) * UINT64_C(1099511628211);
        };
        for (auto parameter : result.parameters) mix(parameter);
        for (std::size_t at = 0; at < s.tmem.size(); at += sizeof(std::uint64_t)) {
            std::uint64_t word;
            std::memcpy(&word, s.tmem.data() + at, sizeof(word));
            mix(word);
        }
        return result;
    }

    const TextureAtlas::Region *find(const Key &key,
                                    const std::array<std::uint8_t, 4096> &tmem) {
        const auto range = index_.equal_range(key.hash);
        for (auto i = range.first; i != range.second; ++i) {
            auto entry = i->second;
            if (entry->key.parameters == key.parameters && entry->tmem == tmem) {
                entries_.splice(entries_.begin(), entries_, entry);
                return &entry->region;
            }
        }
        return nullptr;
    }

    void insert(const Key &key, const std::array<std::uint8_t, 4096> &tmem,
                TextureAtlas::Region region) {
        if (!capacity_ || !region[2]) return;
        if (entries_.size() == capacity_) {
            auto oldest = std::prev(entries_.end());
            const auto range = index_.equal_range(oldest->key.hash);
            for (auto i = range.first; i != range.second; ++i)
                if (i->second == oldest) { index_.erase(i); break; }
            entries_.erase(oldest);
        }
        entries_.push_front({key, tmem, region});
        index_.emplace(key.hash, entries_.begin());
    }

    void clear() { index_.clear(); entries_.clear(); }

  private:
    struct Entry {
        Key key;
        std::array<std::uint8_t, 4096> tmem;
        TextureAtlas::Region region;
    };
    std::size_t capacity_;
    std::list<Entry> entries_;
    std::unordered_multimap<std::uint64_t, std::list<Entry>::iterator> index_;
};
