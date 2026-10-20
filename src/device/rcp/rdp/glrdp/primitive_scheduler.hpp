#pragma once
#include "primitive_footprint.hpp"
#include <vector>

// Preserve the order of every pair sharing a destination pixel. Independent
// primitives may fill earlier batches instead of flushing at the first overlap.
class PrimitiveScheduler {
  public:
    static constexpr unsigned max_queued = 256;
    std::vector<std::vector<unsigned>> groups;
    void clear() { groups.clear(); write_only_groups_.clear(); }
    void append(const std::vector<PrimitiveFootprint> &previous,
                const PrimitiveFootprint &incoming, unsigned capacity) {
        if (incoming.independent_write && !groups.empty() &&
            write_only_groups_.back() && groups.back().size() < capacity) {
            groups.back().push_back(unsigned(previous.size()));
            return;
        }
        unsigned first = 0;
        for (unsigned i = unsigned(groups.size()); i > 0; --i) {
            bool dependency = false, ordered_writes = incoming.independent_write;
            for (unsigned index : groups[i - 1]) {
                if (!previous[index].intersects(incoming)) continue;
                dependency = true;
                ordered_writes &= previous[index].independent_write;
                if (!ordered_writes) break;
            }
            if (dependency) {
                // Instances commit framebuffer writes in primitive order.
                // Writes with no color/Z reads can share this draw, but must
                // still follow overlapping writes in every earlier group.
                first = ordered_writes ? i - 1 : i;
                break;
            }
        }
        while (first < groups.size() && groups[first].size() == capacity) ++first;
        if (first == groups.size()) {
            groups.emplace_back();
            write_only_groups_.push_back(incoming.independent_write);
        } else {
            write_only_groups_[first] = write_only_groups_[first] && incoming.independent_write;
        }
        groups[first].push_back(unsigned(previous.size()));
    }
  private:
    std::vector<bool> write_only_groups_;
};
