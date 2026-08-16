#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob::fuzz {

// A set of OrderIds supporting O(1) add/remove and a deterministic random
// pick by index. Deliberately NOT std::unordered_set: picking a "random
// element" from one by counting iterator advances depends on the
// standard library's internal bucket/iteration order, which is not
// specified and differs between libc++ (this machine) and libstdc++ (CI's
// ubuntu-latest) even given byte-identical insert/remove history and RNG
// draws. "A seed alone must reproduce a run exactly" doesn't hold if the
// pick itself is platform-dependent. This keeps ids in a plain vector
// (fully deterministic order: insertion order, mutated only by swap-
// remove) and uses the unordered_map purely for O(1) existence/index
// lookup during Remove -- that map is never iterated, so its internal
// ordering can't leak into any decision.
class IdPool {
  public:
    void Add(OrderId id) {
        index_of_[id.value] = ids_.size();
        ids_.push_back(id);
    }

    void Remove(OrderId id) {
        auto it = index_of_.find(id.value);
        if (it == index_of_.end()) {
            return;
        }
        const std::size_t index = it->second;
        const std::size_t last = ids_.size() - 1;
        if (index != last) {
            ids_[index] = ids_[last];
            index_of_[ids_[index].value] = index;
        }
        ids_.pop_back();
        index_of_.erase(it);
    }

    [[nodiscard]] bool Contains(OrderId id) const { return index_of_.contains(id.value); }
    [[nodiscard]] bool Empty() const { return ids_.empty(); }
    [[nodiscard]] std::size_t Size() const { return ids_.size(); }

    // `index` must be in [0, Size()) -- callers pick it via their own RNG
    // (e.g. rng() % pool.Size()) so the pool itself never touches
    // randomness, keeping it a pure, independently-testable data
    // structure.
    [[nodiscard]] OrderId At(std::size_t index) const { return ids_[index]; }

  private:
    std::vector<OrderId> ids_;
    std::unordered_map<std::uint64_t, std::size_t> index_of_;
};

}  // namespace lob::fuzz
