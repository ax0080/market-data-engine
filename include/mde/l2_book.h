#pragma once
// Price-level (L2) books for one instrument, driven by SetLevel / ClearBook events.
//
//   L2Book      sorted-vector sides: best for shallow books (few levels near the top).
//   L2TickBook  tick-grid sides (TickLevels): O(1) updates anywhere in a deep book,
//               which is what crypto venues publish.

#include "mde/price_levels.h"
#include "mde/tick_levels.h"
#include "mde/types.h"

namespace mde {

class L2Book {
public:
    void on_event(const BookEvent& e) {
        if (e.type == EventType::SetLevel) {
            (e.side == Side::Buy ? bids : asks).set(e.price, e.qty);
        } else if (e.type == EventType::ClearBook) {
            bids.clear();
            asks.clear();
        }
    }

    bool crossed() const { return !bids.empty() && !asks.empty() && bids.best().price >= asks.best().price; }

    PriceLevels bids{Side::Buy};
    PriceLevels asks{Side::Sell};
};

class L2TickBook {
public:
    explicit L2TickBook(Price tick) : bids(Side::Buy, tick), asks(Side::Sell, tick) {}

    void on_event(const BookEvent& e) {
        if (e.type == EventType::SetLevel) {
            (e.side == Side::Buy ? bids : asks).set(e.price, e.qty);
        } else if (e.type == EventType::ClearBook) {
            bids.clear();
            asks.clear();
        }
    }

    bool crossed() const { return !bids.empty() && !asks.empty() && bids.best().price >= asks.best().price; }

    TickLevels bids;
    TickLevels asks;
};

}  // namespace mde
