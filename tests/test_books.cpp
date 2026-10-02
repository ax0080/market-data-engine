#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "check.h"
#include "mde/l2_book.h"
#include "mde/l3_book.h"
#include "mde/order_table.h"
#include "mde/price_levels.h"
#include "mde/tick_levels.h"

using namespace mde;

TEST(price_levels_keep_best_at_top) {
    PriceLevels bids(Side::Buy), asks(Side::Sell);
    for (Price p : {100, 103, 101, 102}) {
        bids.add(p, 10, 1);
        asks.add(p + 10, 10, 1);
    }
    CHECK_EQ(bids.best().price, 103);
    CHECK_EQ(bids.at(1).price, 102);
    CHECK_EQ(bids.at(3).price, 100);
    CHECK_EQ(asks.best().price, 110);
    CHECK_EQ(asks.at(3).price, 113);
    bids.add(103, -10, -1);   // remove best
    CHECK_EQ(bids.best().price, 102);
    CHECK_EQ(bids.depth(), 3u);
}

TEST(price_levels_set_and_remove) {
    PriceLevels asks(Side::Sell);
    asks.set(105, 7);
    asks.set(104, 3);
    asks.set(105, 9);   // overwrite
    CHECK_EQ(asks.depth(), 2u);
    CHECK_EQ(asks.at(1).qty, 9);
    asks.set(104, 0);   // remove
    asks.set(200, 0);   // removing a missing level is a no-op
    CHECK_EQ(asks.depth(), 1u);
    CHECK_EQ(asks.best().price, 105);
}

TEST(order_table_matches_unordered_map_under_random_ops) {
    OrderTable t(16);   // tiny start: exercises growth and wrap-around
    std::unordered_map<std::uint64_t, Qty> ref;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 200'000; ++i) {
        const std::uint64_t id = 1 + rng() % 5'000;
        const int op = static_cast<int>(rng() % 3);
        if (op == 0) {
            const bool inserted = t.insert(RestingOrder{id, 100, static_cast<Qty>(id), 1, Side::Buy});
            CHECK(inserted == (ref.count(id) == 0));
            ref.emplace(id, static_cast<Qty>(id));
        } else if (op == 1) {
            RestingOrder* o = t.find(id);
            CHECK((o != nullptr) == (ref.count(id) == 1));
            if (o) {
                t.erase(o);
                ref.erase(id);
            }
        } else {
            RestingOrder* o = t.find(id);
            CHECK((o != nullptr) == (ref.count(id) == 1));
            if (o) CHECK_EQ(o->qty, ref[id]);
        }
    }
    CHECK_EQ(t.size(), ref.size());
    for (const auto& [id, q] : ref) {
        const RestingOrder* o = t.find(id);
        CHECK(o != nullptr);
        if (o) CHECK_EQ(o->qty, q);
    }
}

namespace {
BookEvent ev(EventType type, std::uint64_t id, Side side, Price px, Qty qty, std::uint64_t new_id = 0) {
    BookEvent e;
    e.type = type;
    e.order_id = id;
    e.new_order_id = new_id;
    e.side = side;
    e.price = px;
    e.qty = qty;
    e.symbol = 3;
    return e;
}
}  // namespace

TEST(l3_book_lifecycle) {
    L3Book book(64);
    book.on_event(ev(EventType::AddOrder, 1, Side::Buy, 100, 50));
    book.on_event(ev(EventType::AddOrder, 2, Side::Buy, 100, 30));
    book.on_event(ev(EventType::AddOrder, 3, Side::Buy, 99, 20));
    book.on_event(ev(EventType::AddOrder, 4, Side::Sell, 101, 40));
    const L3Book::Instrument* in = book.instrument(3);
    CHECK(in != nullptr);
    CHECK_EQ(in->bids.best().price, 100);
    CHECK_EQ(in->bids.best().qty, 80);
    CHECK_EQ(in->bids.best().orders, 2u);

    book.on_event(ev(EventType::ExecuteOrder, 1, Side::Buy, 0, 20));   // partial fill
    CHECK_EQ(in->bids.best().qty, 60);
    book.on_event(ev(EventType::CancelOrder, 1, Side::Buy, 0, 30));    // cancels the rest -> order gone
    CHECK_EQ(in->bids.best().qty, 30);
    CHECK_EQ(in->bids.best().orders, 1u);
    book.on_event(ev(EventType::DeleteOrder, 2, Side::Buy, 0, 0));     // level 100 empties
    CHECK_EQ(in->bids.best().price, 99);

    book.on_event(ev(EventType::ReplaceOrder, 4, Side::Sell, 102, 15, 5));   // 4 -> 5 @102 x15
    CHECK_EQ(in->asks.best().price, 102);
    CHECK_EQ(in->asks.best().qty, 15);
    CHECK_EQ(book.live_orders(), 2u);
    CHECK_EQ(book.stats().unknown_order, 0u);

    book.on_event(ev(EventType::DeleteOrder, 999, Side::Buy, 0, 0));
    CHECK_EQ(book.stats().unknown_order, 1u);
}

TEST(tick_levels_match_std_map_with_drift_and_overflow) {
    // Random walk of the mid price over many window widths forces re-centring;
    // occasional far-away and off-grid prices exercise the overflow map.
    const Price tick = 100;
    for (Side side : {Side::Buy, Side::Sell}) {
        TickLevels t(side, tick);
        std::map<Price, Qty> ref;
        std::mt19937_64 rng(side == Side::Buy ? 11 : 12);
        Price mid = 10'000'000;
        for (int i = 0; i < 300'000; ++i) {
            mid += (static_cast<Price>(rng() % 201) - 100) * tick * 20;
            Price p = mid + (static_cast<Price>(rng() % 2001) - 1000) * tick;
            if (rng() % 50 == 0) p += (rng() % 2 ? 1 : -1) * TickLevels::kWindow * tick * 3;   // far away
            if (rng() % 100 == 0) p += 37;                                                        // off grid
            const Qty q = rng() % 3 == 0 ? 0 : static_cast<Qty>(1 + rng() % 1000);
            t.set(p, q);
            if (q > 0) {
                ref[p] = q;
            } else {
                ref.erase(p);
            }
            if (i % 997 == 0 || i == 299'999) {
                CHECK_EQ(t.depth(), ref.size());
                if (!ref.empty()) {
                    const auto best = side == Side::Buy ? *ref.rbegin() : *ref.begin();
                    CHECK_EQ(t.best().price, best.first);
                    CHECK_EQ(t.best().qty, best.second);
                }
            }
        }
        // Full ordered walk must equal the reference, best first.
        std::vector<std::pair<Price, Qty>> walk;
        t.for_each([&](Price p, Qty q) {
            walk.emplace_back(p, q);
            return true;
        });
        std::vector<std::pair<Price, Qty>> expect(ref.begin(), ref.end());
        if (side == Side::Buy) std::reverse(expect.begin(), expect.end());
        CHECK(walk == expect);
    }
}

TEST(l2_book_clear_and_cross_check) {
    L2Book b;
    BookEvent e;
    e.type = EventType::SetLevel;
    e.side = Side::Buy;
    e.price = 100;
    e.qty = 5;
    b.on_event(e);
    e.side = Side::Sell;
    e.price = 101;
    b.on_event(e);
    CHECK(!b.crossed());
    e.price = 99;   // ask below bid
    b.on_event(e);
    CHECK(b.crossed());
    e.type = EventType::ClearBook;
    b.on_event(e);
    CHECK(b.bids.empty() && b.asks.empty());
}
