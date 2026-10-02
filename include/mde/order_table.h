#pragma once
// Open-addressing hash table from order id to resting order.
//
// Linear probing keeps a lookup inside one or two cache lines. Deletion uses
// backward-shift (no tombstones), so a full trading day of adds and deletes never
// degrades probe lengths. Capacity is a power of two and doubles at 50% load.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mde/types.h"

namespace mde {

struct RestingOrder {
    std::uint64_t id;    // 0 = empty slot (ITCH order references start at 1)
    Price price;
    Qty qty;
    std::uint16_t symbol;
    Side side;
};

class OrderTable {
public:
    explicit OrderTable(std::size_t initial_capacity = std::size_t{1} << 22) {
        std::size_t cap = 16;
        while (cap < initial_capacity) cap <<= 1;
        slots_.assign(cap, RestingOrder{});
        mask_ = cap - 1;
    }

    // Returns nullptr if the id is not present.
    RestingOrder* find(std::uint64_t id) {
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            RestingOrder& s = slots_[i];
            if (s.id == id) return &s;
            if (s.id == 0) return nullptr;
        }
    }

    // Inserts a new order; returns false if the id already exists.
    bool insert(const RestingOrder& o) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        for (std::size_t i = home(o.id);; i = (i + 1) & mask_) {
            RestingOrder& s = slots_[i];
            if (s.id == 0) {
                s = o;
                ++size_;
                return true;
            }
            if (s.id == o.id) return false;
        }
    }

    // Removes the slot that `slot` points to (must come from find()).
    void erase(RestingOrder* slot) {
        std::size_t i = static_cast<std::size_t>(slot - slots_.data());
        // Backward-shift: pull later members of the probe run into the hole.
        for (std::size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
            const RestingOrder& s = slots_[j];
            if (s.id == 0) break;
            const std::size_t h = home(s.id);
            // s may move to i only if its home is not in the cyclic range (i, j]
            const bool in_range = (i <= j) ? (h > i && h <= j) : (h > i || h <= j);
            if (!in_range) {
                slots_[i] = s;
                i = j;
            }
        }
        slots_[i] = RestingOrder{};
        --size_;
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return slots_.size(); }

private:
    std::size_t home(std::uint64_t id) const {
        // NASDAQ assigns order references sequentially and most orders are short-lived,
        // so the identity hash keeps recently added (= most often touched) orders in
        // neighbouring slots and in cache. On the full 30 Jan 2019 day this measured
        // 65 ns/message against 81 ns with a multiplicative (scattering) hash. Feeds
        // with random ids should define MDE_SCATTER_HASH to avoid clustering.
#if defined(MDE_SCATTER_HASH)
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> 20) & mask_;
#else
        return static_cast<std::size_t>(id) & mask_;
#endif
    }

    void grow() {
        std::vector<RestingOrder> old;
        old.swap(slots_);
        slots_.assign(old.size() * 2, RestingOrder{});
        mask_ = slots_.size() - 1;
        size_ = 0;
        for (const RestingOrder& o : old)
            if (o.id != 0) insert(o);
    }

    std::vector<RestingOrder> slots_;
    std::size_t mask_{};
    std::size_t size_{};
};

}  // namespace mde
