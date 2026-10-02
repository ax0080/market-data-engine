#pragma once
// One side of a price-level book as a sorted std::vector with the best price at
// the back. Almost every update touches a price near the top of the book, so the
// search starts from the back and an insert or erase moves only the few levels
// behind it. This is far more cache-friendly than a node-based std::map.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mde/types.h"

// GCC 15 reports -Wstringop-overread on vector::insert/erase here after heavy
// inlining in some callers: it cannot prove the vector is non-empty on a path that
// never executes. The accesses are bounds-checked by construction (and the tests
// run clean under AddressSanitizer), so the warning is silenced for this header only.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif

namespace mde {

struct Level {
    Price price;
    Qty qty;
    std::uint32_t orders;   // live order count (L3 books); unused for L2
};

class PriceLevels {
public:
    // No up-front reserve: an L3 book creates two of these for every instrument
    // id, and most of the 65,536 ids are never used.
    explicit PriceLevels(Side side) : side_(side) {}

    // Add delta (may be negative) to the level at price; removes it at zero.
    void add(Price price, Qty delta, int order_delta) {
        const std::size_t i = find_or_insert(price);
        Level& l = levels_[i];
        l.qty += delta;
        l.orders += static_cast<std::uint32_t>(order_delta);
        if (l.qty <= 0) levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // Set the aggregate qty at price (L2 feeds); qty 0 removes the level.
    void set(Price price, Qty qty) {
        if (qty <= 0) {
            const std::size_t i = find(price);
            if (i != npos) levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
        levels_[find_or_insert(price)].qty = qty;
    }

    void clear() { levels_.clear(); }
    bool empty() const { return levels_.empty(); }
    std::size_t depth() const { return levels_.size(); }
    const Level& best() const { return levels_.back(); }
    // k-th level from the top (0 = best)
    const Level& at(std::size_t k) const { return levels_[levels_.size() - 1 - k]; }

    // Visits levels from the best outward until f returns false.
    template <class F>
    void for_each(F&& f) const {
        for (std::size_t i = levels_.size(); i-- > 0;)
            if (!f(levels_[i].price, levels_[i].qty)) return;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    // "a is a better price than b" for this side
    bool better(Price a, Price b) const { return side_ == Side::Buy ? a > b : a < b; }

    std::size_t find(Price price) const {
        for (std::size_t i = levels_.size(); i-- > 0;) {
            if (levels_[i].price == price) return i;
            if (better(price, levels_[i].price)) return npos;   // passed where it would be
        }
        return npos;
    }

    std::size_t find_or_insert(Price price) {
        std::size_t i = levels_.size();
        while (i > 0 && better(levels_[i - 1].price, price)) --i;   // skip better levels from the top
        if (i > 0 && levels_[i - 1].price == price) return i - 1;
        levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), Level{price, 0, 0});
        return i;
    }

    Side side_;
    std::vector<Level> levels_;   // worst ... best
};

}  // namespace mde

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
