#pragma once
// One side of a deep L2 book on a fixed tick grid.
//
// Crypto venues publish thousands of levels and update them across the whole
// depth, so a sorted vector pays a memmove of thousands of entries per update.
// Here a price maps directly to an array slot: idx = (price - anchor) / tick.
//   * set() is O(1): one array write, no search, no shifting.
//   * The array covers a window of kWindow ticks centred on the market. Levels
//     outside it (orders parked far away) go to a small ordered overflow map.
//   * The best price is tracked incrementally; when the best level is removed
//     the next one is found by scanning toward the far side, which is short in
//     practice because the top of the book is dense.
//   * If the best price drifts within kMargin ticks of either edge the window is
//     re-centred (O(kWindow), rare).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "mde/types.h"

namespace mde {

class TickLevels {
public:
    static constexpr std::int64_t kWindow = std::int64_t{1} << 17;   // 131,072 ticks
    static constexpr std::int64_t kMargin = kWindow / 8;

    TickLevels(Side side, Price tick) : side_(side), tick_(tick), qty_(kWindow, 0) {}

    void set(Price price, Qty qty) {
        if (!anchored_) anchor_at(price);
        const std::int64_t d = price - anchor_;
        const std::int64_t i = d / tick_;
        // Off-grid prices (not a multiple of the tick) never alias an array slot.
        if (i < 0 || i >= kWindow || d != i * tick_) {
            set_overflow(price, qty);
            return;
        }
        Qty& slot = qty_[static_cast<std::size_t>(i)];
        if (slot == 0 && qty > 0) ++count_;
        if (slot > 0 && qty <= 0) --count_;
        slot = qty > 0 ? qty : 0;
        if (qty > 0) {
            if (best_ < 0 || better_idx(i, best_)) best_ = i;
            if (best_ < kMargin || best_ >= kWindow - kMargin) recentre();
        } else if (i == best_) {
            find_next_best();
        }
    }

    void clear() {
        std::fill(qty_.begin(), qty_.end(), 0);
        overflow_.clear();
        count_ = 0;
        best_ = -1;
        anchored_ = false;
    }

    bool empty() const { return depth() == 0; }
    std::size_t depth() const { return count_ + overflow_.size(); }

    // Best level; valid only if !empty(). Overflow can hold off-grid prices inside
    // the window's range, so it is always compared against the window's best.
    Level best() const {
        if (overflow_.empty()) return {price_of(best_), qty_[static_cast<std::size_t>(best_)], 0};
        const auto& [op, oq] = best_overflow();
        if (best_ < 0 || better_price(op, price_of(best_))) return {op, oq, 0};
        return {price_of(best_), qty_[static_cast<std::size_t>(best_)], 0};
    }

    // Visits levels from the best outward until f returns false (window and
    // overflow merged in price order).
    void for_each(const std::function<bool(Price, Qty)>& f) const {
        std::vector<std::pair<Price, Qty>> ov(overflow_.begin(), overflow_.end());
        sort_best_first(ov);
        std::size_t k = 0;
        const std::int64_t step = side_ == Side::Buy ? -1 : 1;
        for (std::int64_t i = side_ == Side::Buy ? kWindow - 1 : 0; i >= 0 && i < kWindow; i += step) {
            const Qty q = qty_[static_cast<std::size_t>(i)];
            if (q <= 0) continue;
            const Price p = price_of(i);
            for (; k < ov.size() && better_price(ov[k].first, p); ++k)
                if (!f(ov[k].first, ov[k].second)) return;
            if (!f(p, q)) return;
        }
        for (; k < ov.size(); ++k)
            if (!f(ov[k].first, ov[k].second)) return;
    }

private:
    std::int64_t index(Price p) const { return (p - anchor_) / tick_; }
    Price price_of(std::int64_t i) const { return anchor_ + i * tick_; }
    bool better_idx(std::int64_t a, std::int64_t b) const { return side_ == Side::Buy ? a > b : a < b; }
    bool better_price(Price a, Price b) const { return side_ == Side::Buy ? a > b : a < b; }
    Price window_best_edge() const { return side_ == Side::Buy ? price_of(kWindow - 1) : price_of(0); }

    // The grid is always aligned to multiples of the tick, so an off-grid first
    // price cannot shift the grid and push every regular price into overflow.
    void anchor_at(Price p) {
        Price base = (p / tick_) * tick_;
        if (base > p) base -= tick_;   // floor for negative prices
        anchor_ = base - (kWindow / 2) * tick_;
        anchored_ = true;
    }

    void set_overflow(Price p, Qty q) {
        if (q > 0) {
            overflow_[p] = q;
        } else {
            overflow_.erase(p);
        }
        // A level beyond the window's better edge is the new best: move the window to it.
        // So is the first level of an otherwise empty window.
        if (q > 0 && (better_price(p, window_best_edge()) || count_ == 0)) recentre_on(p);
    }

    std::pair<Price, Qty> best_overflow() const {
        return side_ == Side::Buy ? *overflow_.rbegin() : *overflow_.begin();
    }

    void find_next_best() {
        const std::int64_t step = side_ == Side::Buy ? -1 : 1;
        for (std::int64_t i = best_ + step; i >= 0 && i < kWindow; i += step)
            if (qty_[static_cast<std::size_t>(i)] > 0) {
                best_ = i;
                return;
            }
        best_ = -1;   // window empty on the far side; best is in overflow (if any)
    }

    void recentre() { recentre_on(price_of(best_)); }

    // Re-anchor the window around price p, moving levels between array and overflow.
    void recentre_on(Price p) {
        std::vector<std::pair<Price, Qty>> all;
        all.reserve(count_ + overflow_.size());
        for (std::int64_t i = 0; i < kWindow; ++i)
            if (qty_[static_cast<std::size_t>(i)] > 0) all.emplace_back(price_of(i), qty_[static_cast<std::size_t>(i)]);
        for (const auto& kv : overflow_) all.push_back(kv);
        std::fill(qty_.begin(), qty_.end(), 0);
        overflow_.clear();
        count_ = 0;
        best_ = -1;
        anchor_at(p);
        for (const auto& [pp, q] : all) {
            const std::int64_t i = index(pp);
            if (i < 0 || i >= kWindow || pp - anchor_ != i * tick_) {
                overflow_[pp] = q;
            } else {
                qty_[static_cast<std::size_t>(i)] = q;
                ++count_;
                if (best_ < 0 || better_idx(i, best_)) best_ = i;
            }
        }
    }

    void sort_best_first(std::vector<std::pair<Price, Qty>>& v) const {
        std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) { return better_price(a.first, b.first); });
    }

    Side side_;
    Price tick_;
    Price anchor_ = 0;
    bool anchored_ = false;
    std::vector<Qty> qty_;
    std::size_t count_ = 0;
    std::int64_t best_ = -1;          // window index of the best level, -1 if none in the window
    std::map<Price, Qty> overflow_;   // levels outside the window
};

}  // namespace mde
