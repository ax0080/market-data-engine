#pragma once
// Venue-neutral market-data events. Every decoder turns its wire format into
// BookEvent values and hands them to a sink chosen at compile time, so the hot
// path has no virtual calls.

#include <concepts>
#include <cstdint>

namespace mde {

// Fixed-point price and quantity, 8 decimal places for every venue. Exact for
// crypto (8 dp) and NASDAQ (prices are 4 dp, shares are integers), so book
// arithmetic never touches floating point.
using Price = std::int64_t;
using Qty = std::int64_t;
inline constexpr std::int64_t kScale = 100'000'000;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

enum class EventType : std::uint8_t {
    AddOrder,       // L3: new resting order (order_id, side, price, qty)
    ExecuteOrder,   // L3: qty filled against a resting order
    CancelOrder,    // L3: qty removed from a resting order (partial cancel)
    DeleteOrder,    // L3: order removed entirely
    ReplaceOrder,   // L3: order_id -> new_order_id with new price and qty
    SetLevel,       // L2: aggregate qty at price is now qty (0 removes the level)
    ClearBook,      // L2: drop all levels (snapshot follows)
};

struct BookEvent {
    EventType type{};
    Side side{};
    std::uint16_t symbol{};      // venue-local instrument id (ITCH stock locate, or caller-assigned)
    std::uint64_t order_id{};
    std::uint64_t new_order_id{};
    Price price{};
    Qty qty{};
    std::uint64_t seq{};         // venue sequence number (0 if the format has none)
    std::uint64_t ts_ns{};       // venue timestamp in ns (since midnight for ITCH, epoch for crypto)
};

template <class S>
concept EventSink = requires(S& s, const BookEvent& e) {
    { s.on_event(e) };
};

}  // namespace mde
