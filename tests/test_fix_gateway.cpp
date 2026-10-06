// FIX order entry against the real matching engine: random NewOrderSingle,
// OrderCancelRequest and OrderCancelReplaceRequest from three FIX clients, plus
// house flow. After every operation:
//   - every request has been answered (ExecutionReport or OrderCancelReject),
//   - each client's open orders, rebuilt only from ExecutionReports
//     (ClOrdID -> LeavesQty), match the engine,
//   - the L3 book rebuilt from the published ITCH matches the engine.

#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "fix_order_entry.h"
#include "gateway.h"
#include "mde/fix44.h"
#include "mde/itch50.h"
#include "mde/l3_book.h"

using namespace mde;

namespace {

struct FixClient {
    std::map<std::string, std::int64_t> open;           // ClOrdID -> LeavesQty
    std::map<std::string, std::uint64_t> order_id;      // ClOrdID -> exchange OrderID
    std::set<std::string> awaiting;                     // requests not yet answered
    std::uint64_t fills = 0, rejects = 0, cancel_rejects = 0, replaced = 0;

    void on(const fix::Message& m) {
        const std::string id(m.get(fix::tag::ClOrdID));
        awaiting.erase(id);
        if (m.type() == '9') {
            ++cancel_rejects;
            return;
        }
        const char et = m.get(fix::tag::ExecType)[0];
        const auto leaves = static_cast<std::int64_t>(m.get_uint(fix::tag::LeavesQty));
        std::string key = id;
        if (et == '5') {   // replaced: the order continues under the new ClOrdID
            ++replaced;
            open.erase(std::string(m.get(fix::tag::OrigClOrdID)));
        } else if (et == '4' && m.has(fix::tag::OrigClOrdID)) {   // cancel answered under the request's ClOrdID
            key = std::string(m.get(fix::tag::OrigClOrdID));
        }
        if (et == '8') ++rejects;
        if (et == 'F') ++fills;
        if (et == '8' || leaves == 0) {
            open.erase(key);
            order_id.erase(key);
        } else {
            open[key] = leaves;
            order_id[key] = m.get_uint(fix::tag::OrderID);
        }
    }
};

struct Out;
using Gw = sim::Gateway<Out>;

struct Out {
    L3Book book{1 << 16};
    itch::Decoder<L3Book> dec{book};
    std::vector<sim::FixOrderEntry> fix;
    std::vector<FixClient> clients;
    std::uint64_t t = 0;
    fix::Encoder reply;

    // Delivers adapter replies straight to client s, as its FIX parser would see them.
    auto sender(int s) {
        return [this, s](char type, auto&& fill) {
            reply.begin(type, "EXCH", "C" + std::to_string(s), 1, t);
            fill(reply);
            const auto m = reply.finish();
            fix::Message p;
            CHECK(p.parse(m.first, m.second));
            clients[static_cast<std::size_t>(s)].on(p);
        };
    }

    void md(const std::uint8_t* m, std::size_t len) { dec.decode(m, len); }
    void oe(int s, const std::uint8_t* m, std::size_t len) {
        fix[static_cast<std::size_t>(s)].on_ouch(m, len, t, sender(s));
    }
    std::uint64_t now_ns() { return ++t; }
};

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

bool books_match(const Gw& gw, const Out& out) {
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

bool clients_match(const Gw& gw, const Out& out) {
    for (const FixClient& c : out.clients) {
        if (!c.awaiting.empty()) return false;
        for (const auto& [id, leaves] : c.open) {
            const std::uint64_t oid = c.order_id.at(id);
            const exchange::Order* o = nullptr;
            for (std::uint16_t loc = 1; loc <= gw.symbol_count() && !o; ++loc)
                if (const auto* b = gw.book(loc)) o = b->find(oid);
            if (!o || static_cast<std::int64_t>(o->remaining()) != leaves) return false;
        }
    }
    return true;
}

}  // namespace

TEST(fix_order_entry_consistent_with_engine) {
    Out out;
    for (int s = 0; s < 3; ++s) {
        out.fix.emplace_back(s);
        out.clients.emplace_back();
    }
    Gw gw(out);
    const char* names[] = {"AAPL", "MSFT"};
    for (const char* n : names) gw.add_symbol(n);

    std::mt19937 rng(77);
    std::vector<exchange::OrderId> house;
    int next_id = 0;
    auto price = [&] { return static_cast<std::uint64_t>(1'000'000 + 100 * (rng() % 21)); };
    fix::Encoder req;
    auto send_request = [&](int s, char type, auto&& fill) {
        req.begin(type, "C" + std::to_string(s), "EXCH", 1, out.t);
        fill(req);
        const auto m = req.finish();
        fix::Message p;
        p.parse(m.first, m.second);
        out.clients[static_cast<std::size_t>(s)].awaiting.insert(std::string(p.get(fix::tag::ClOrdID)));
        out.fix[static_cast<std::size_t>(s)].on_request(p, gw, out.t, out.sender(s));
    };

    bool ok = true;
    int ops = 0;
    for (; ops < 20000 && ok; ++ops) {
        const unsigned r = rng() % 100;
        const auto loc = static_cast<std::uint16_t>(1 + rng() % 2);
        const int s = static_cast<int>(rng() % 3);
        FixClient& c = out.clients[static_cast<std::size_t>(s)];
        auto pick_open = [&]() -> std::string {
            if (c.open.empty()) return "nope";
            auto it = c.open.begin();
            std::advance(it, static_cast<long>(rng() % c.open.size()));
            return it->first;
        };
        if (r < 30) {
            if (const auto id = gw.house_limit(loc, rng() % 2 ? 'B' : 'S', static_cast<std::uint32_t>(price()),
                                               100 * (1 + rng() % 10), rng() % 10 == 0))
                house.push_back(id);
        } else if (r < 42 && !house.empty()) {
            const std::size_t k = rng() % house.size();
            gw.house_cancel(house[k]);
            house[k] = house.back();
            house.pop_back();
        } else if (r < 72) {
            const std::string id = "N" + std::to_string(++next_id);
            const char side = "125"[rng() % 3];
            const bool bad_type = rng() % 50 == 0;
            send_request(s, 'D', [&](fix::Encoder& e) {
                e.field(fix::tag::ClOrdID, id).field(fix::tag::Symbol, names[loc - 1]).field_char(fix::tag::Side, side);
                e.field_uint(fix::tag::OrderQty, 100 * (1 + rng() % 8));
                e.field_char(fix::tag::OrdType, bad_type ? '1' : '2').field_price(fix::tag::Price, price());
                e.field_char(fix::tag::TimeInForce, rng() % 4 == 0 ? '3' : '0');
            });
        } else if (r < 86) {
            const std::string orig = pick_open();
            const std::string id = "R" + std::to_string(++next_id);
            send_request(s, 'G', [&](fix::Encoder& e) {
                e.field(fix::tag::ClOrdID, id).field(fix::tag::OrigClOrdID, orig).field(fix::tag::Symbol, names[loc - 1]);
                e.field_char(fix::tag::Side, '1').field_uint(fix::tag::OrderQty, 100 * (1 + rng() % 10));
                e.field_char(fix::tag::OrdType, '2').field_price(fix::tag::Price, rng() % 5 ? price() : 0);
            });
        } else {
            const std::string orig = rng() % 10 ? pick_open() : "unknown";
            const std::string id = "X" + std::to_string(++next_id);
            send_request(s, 'F', [&](fix::Encoder& e) {
                e.field(fix::tag::ClOrdID, id).field(fix::tag::OrigClOrdID, orig).field(fix::tag::Symbol, names[loc - 1]);
                e.field_char(fix::tag::Side, '1');
            });
        }
        ok = books_match(gw, out) && clients_match(gw, out);
    }
    CHECK_EQ(ops, 20000);
    CHECK(ok);
    CHECK_EQ(out.book.stats().unknown_order, 0);
    std::uint64_t fills = 0, rejects = 0, cxl_rejects = 0, replaced = 0;
    for (const FixClient& c : out.clients) {
        fills += c.fills;
        rejects += c.rejects;
        cxl_rejects += c.cancel_rejects;
        replaced += c.replaced;
    }
    CHECK(fills > 1000);       // the flow trades
    CHECK(rejects > 0);        // NewOrderSingle reject path ran
    CHECK(cxl_rejects > 0);    // OrderCancelReject path ran
    CHECK(replaced > 100);     // replaces went through
}
