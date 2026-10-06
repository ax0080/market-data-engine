// Exchange gateway against the real matching engine: random OUCH and house
// order flow, checking after every operation that
//   - the L3 book rebuilt from the published ITCH feed equals the engine's book
//     (every level: price, quantity, order count), and
//   - each client's view built only from OUCH replies (open quantity per order)
//     equals what the engine holds.

#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <vector>

#include "check.h"
#include "gateway.h"
#include "mde/itch50.h"
#include "mde/l3_book.h"
#include "mde/ouch50.h"

using namespace mde;

namespace {

struct ClientView {   // what a client knows from OUCH alone
    std::map<std::uint32_t, std::int64_t> open;           // UserRefNum -> open shares
    std::map<std::uint32_t, std::uint64_t> order_ref;     // UserRefNum -> exchange order id
    std::uint64_t rejects = 0, executions = 0;
    void on_system_event(std::uint64_t, char) {}
    void on_accepted(const ouch::Accepted& a) {
        open[a.user_ref] = a.qty;
        order_ref[a.user_ref] = a.order_ref;
    }
    void on_replaced(const ouch::Replaced& r) {
        open.erase(r.orig_user_ref);
        order_ref.erase(r.orig_user_ref);
        open[r.user_ref] = r.qty;
        order_ref[r.user_ref] = r.order_ref;
    }
    void on_executed(const ouch::Executed& e) {
        ++executions;
        if ((open[e.user_ref] -= e.qty) == 0) open.erase(e.user_ref);
    }
    void on_canceled(const ouch::Canceled& c) {
        if ((open[c.user_ref] -= c.qty) == 0) open.erase(c.user_ref);
    }
    void on_rejected(const ouch::Rejected&) { ++rejects; }
};

struct TestOut {
    L3Book book{1 << 16};
    itch::Decoder<L3Book> dec{book};
    std::vector<ClientView> clients = std::vector<ClientView>(3);
    std::uint64_t t = 0;
    void md(const std::uint8_t* m, std::size_t len) { dec.decode(m, len); }
    void oe(int s, const std::uint8_t* m, std::size_t len) { CHECK(ouch::decode_outbound(m, len, clients[s])); }
    std::uint64_t now_ns() { return ++t; }
};

using Gw = sim::Gateway<TestOut>;

bool same_side(const exchange::OrderBook& eb, exchange::Side side, const PriceLevels& lv) {
    const auto depth = eb.depth(side, 1 << 20);
    if (depth.size() != lv.depth()) return false;
    for (std::size_t k = 0; k < depth.size(); ++k) {
        const Level& l = lv.at(k);
        if (l.price != depth[k].price.raw * itch::kPriceMul || l.qty != static_cast<Qty>(depth[k].quantity) * kScale ||
            l.orders != depth[k].order_count)
            return false;
    }
    return true;
}

bool books_match(const Gw& gw, const TestOut& out) {
    for (std::uint16_t loc = 1; loc <= gw.symbol_count(); ++loc) {
        const exchange::OrderBook* eb = gw.book(loc);
        const L3Book::Instrument* ib = out.book.instrument(loc);
        const bool engine_empty = !eb || (eb->bid_level_count() == 0 && eb->ask_level_count() == 0);
        if (!ib) {
            if (!engine_empty) return false;
            continue;
        }
        if (!eb) return ib->bids.empty() && ib->asks.empty();
        if (!same_side(*eb, exchange::Side::Buy, ib->bids) || !same_side(*eb, exchange::Side::Sell, ib->asks)) return false;
    }
    return true;
}

bool clients_match(const Gw& gw, const TestOut& out) {
    for (const ClientView& c : out.clients) {
        for (const auto& [ref, open] : c.open) {
            const std::uint64_t id = c.order_ref.at(ref);
            const exchange::Order* o = nullptr;
            for (std::uint16_t loc = 1; loc <= gw.symbol_count() && !o; ++loc)
                if (const auto* b = gw.book(loc)) o = b->find(id);
            if (!o || static_cast<std::int64_t>(o->remaining()) != open) return false;
        }
    }
    return true;
}

}  // namespace

TEST(gateway_itch_and_ouch_stay_consistent_with_engine) {
    TestOut out;
    Gw gw(out);
    const char* names[] = {"AAPL", "MSFT"};
    for (const char* n : names) gw.add_symbol(n);
    gw.system_event(itch::kStartOfMessages);

    std::mt19937 rng(2024);
    std::vector<exchange::OrderId> house;
    std::vector<std::uint32_t> next_ref(3, 1);
    auto price = [&] { return static_cast<std::uint32_t>(1'000'000 + 100 * (rng() % 21)); };   // 100.00 .. 100.20
    bool books_ok = true, clients_ok = true;
    int ops = 0;

    for (; ops < 20000; ++ops) {
        const unsigned r = rng() % 100;
        const auto loc = static_cast<std::uint16_t>(1 + rng() % 2);
        const int s = static_cast<int>(rng() % 3);
        ClientView& cv = out.clients[s];
        if (r < 35) {
            const char side = rng() % 2 ? 'B' : 'S';
            if (const auto id = gw.house_limit(loc, side, price(), 100 * (1 + rng() % 10), rng() % 10 == 0)) house.push_back(id);
        } else if (r < 50 && !house.empty()) {
            const std::size_t k = rng() % house.size();
            gw.house_cancel(house[k]);
            house[k] = house.back();
            house.pop_back();
        } else if (r < 75) {
            ouch::EnterOrder o{};
            o.user_ref = next_ref[s]++;
            o.side = "BST"[rng() % 3];
            o.qty = 100 * (1 + rng() % 8);
            o.symbol = names[loc - 1];
            o.price = price();
            o.tif = rng() % 4 == 0 ? ouch::kTifIoc : ouch::kTifDay;
            o.clordid = "C";
            gw.enter(s, o);
        } else if (r < 88 && !cv.open.empty()) {
            auto it = cv.open.begin();
            std::advance(it, static_cast<long>(rng() % cv.open.size()));
            ouch::ReplaceOrder o{};
            o.orig_user_ref = it->first;
            o.user_ref = next_ref[s]++;
            o.qty = 100 * (1 + rng() % 10);   // total liable: may be at or below what already executed -> reject
            o.price = rng() % 3 ? price() : 0;   // some invalid prices
            gw.replace(s, o);
        } else if (!cv.open.empty()) {
            auto it = cv.open.begin();
            std::advance(it, static_cast<long>(rng() % cv.open.size()));
            const auto q = static_cast<std::uint32_t>(rng() % 2 ? 0 : rng() % static_cast<std::uint32_t>(it->second + 1));
            gw.cancel(s, ouch::CancelOrder{it->first, q});
        }
        // invalid requests: unknown symbol, retransmitted (old) UserRefNum
        if (ops % 997 == 0) {
            ouch::EnterOrder bad{};
            bad.user_ref = next_ref[s]++;
            bad.side = 'B';
            bad.qty = 100;
            bad.symbol = "NOPE";
            bad.price = price();
            gw.enter(s, bad);
            bad.symbol = "AAPL";
            bad.user_ref = 1;
            gw.enter(s, bad);
        }
        books_ok = books_match(gw, out);
        clients_ok = clients_match(gw, out);
        if (!books_ok || !clients_ok) break;
    }
    CHECK_EQ(ops, 20000);
    CHECK(books_ok);
    CHECK(clients_ok);
    CHECK_EQ(out.book.stats().unknown_order, 0);
    CHECK_EQ(out.book.stats().duplicate_add, 0);
    std::uint64_t execs = 0, rejects = 0;
    for (const ClientView& c : out.clients) {
        execs += c.executions;
        rejects += c.rejects;
    }
    CHECK(execs > 1000);   // the flow really trades
    CHECK(rejects > 0);    // and the reject paths ran
    CHECK(gw.stats().ignored > 0);
}

TEST(gateway_rejects_orders_after_close) {
    TestOut out;
    Gw gw(out);
    const auto loc = gw.add_symbol("AAPL");
    gw.house_limit(loc, 'S', 1'000'000, 500);   // resting ask
    ouch::EnterOrder o{};
    o.user_ref = 1;
    o.side = 'B';
    o.qty = 100;
    o.symbol = "AAPL";
    o.price = 1'000'000;
    o.clordid = "C";
    gw.enter(0, o);   // trades before the close
    CHECK_EQ(out.clients[0].executions, 1);

    gw.close();
    const auto itch_before = out.dec.stats().messages;
    o.user_ref = 2;
    gw.enter(0, o);                                      // would trade: rejected instead
    gw.cancel(0, ouch::CancelOrder{1, 0});               // ignored
    CHECK_EQ(out.clients[0].rejects, 1);
    CHECK_EQ(out.clients[0].executions, 1);
    CHECK_EQ(out.dec.stats().messages, itch_before);     // nothing more on the public feed
    const exchange::Order* ask = gw.book(loc)->find(1);   // the house ask: 400 left, untouched by the close
    CHECK(ask != nullptr);
    if (ask) CHECK_EQ(ask->remaining(), 400);
}
