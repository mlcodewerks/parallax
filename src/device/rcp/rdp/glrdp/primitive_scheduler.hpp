#pragma once
#include "primitive_footprint.hpp"
#include <vector>

// Preserve the order of every pair sharing a destination pixel. Independent
// primitives may fill earlier batches instead of flushing at the first overlap.
class PrimitiveScheduler {
  public:
    static constexpr unsigned max_queued = 256;
    std::vector<std::vector<unsigned>> groups;
    void clear() { groups.clear(); }
    void append(const std::vector<PrimitiveFootprint> &previous,
                const PrimitiveFootprint &incoming, unsigned capacity) {
        unsigned first = 0;
        for (unsigned i = unsigned(groups.size()); i > 0; --i) {
            bool dependency = false;
            for (unsigned index : groups[i - 1])
                if (previous[index].intersects(incoming)) { dependency = true; break; }
            if (dependency) { first = i; break; }
        }
        while (first < groups.size() && groups[first].size() == capacity) ++first;
        if (first == groups.size()) groups.emplace_back();
        groups[first].push_back(unsigned(previous.size()));
    }
};
