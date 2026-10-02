#pragma once
// Exchange gateway around the orderbook-engine matching engine
// (github.com/ax0080/orderbook-engine).
//
// Inbound: OUCH 5.0 orders from client sessions, plus "house" orders that stand
// in for the rest of the market. Outbound, for every change to the book:
//   - ITCH 5.0 messages for the public feed (Out::md), one order-by-order view
//     of the book that any L3 feed handler can rebuild exactly;
//   - OUCH 5.0 replies to the owning session (Out::oe).
//
// ITCH shows resting orders only. An incoming order that trades on arrival
// produces Executed messages against the resting orders it hit, and an Add
// Order for whatever is left once it rests. An order that is re-priced (or
// grows) loses priority and is re-published as Delete + Add under a new ITCH
// reference; a size reduction keeps priority and is a Cancel.
//
// The engine reports events through OrderListener callbacks while an operation
// runs; the gateway keeps just enough per-order state to name those orders on
// both feeds.
//
// Out must provide:
//   void md(const std::uint8_t* msg, std::size_t len);               // ITCH message
//   void oe(int session, const std::uint8_t* msg, std::size_t len);  // OUCH message
//   std::uint64_t now_ns();                                          // ns since midnight

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "exchange/matching_engine.h"
#include "mde/itch50_writer.h"
#include "mde/ouch50.h"

namespace mde::sim {

struct GatewayStats {
    std::uint64_t enters = 0, replaces = 0, cancels = 0;
    std::uint64_t accepted = 0, rejected = 0, executions = 0, ignored = 0;
    std::uint64_t itch_messages = 0, ouch_messages = 0;
};

template <class Out>
class Gateway : private exchange::OrderListener {
public:
    static constexpr int kHouse = -1;
    static constexpr std::uint32_t kMaxPrice = 1'999'999'900;   // $199,999.99, the OUCH maximum

    explicit Gateway(Out& out) : engine_(exchange::StpPolicy::None), out_(out) { engine_.set_listener(this); }

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    // Lists an instrument (ITCH Stock Directory) and returns its stock locate.
    std::uint16_t add_symbol(std::string_view name) {
        symbols_.emplace_back(name);
        const auto locate = static_cast<std::uint16_t>(symbols_.size());
        std::uint8_t m[itch::kMaxWrittenLen];
        md(m, itch::put_stock_directory(m, locate, out_.now_ns(), name));
        return locate;
    }
    void system_event(char code) {
        std::uint8_t m[itch::kSystemEventLen];
        md(m, itch::put_system_event(m, out_.now_ns(), code));
    }

    // ------------------------------------------------------------ OUCH input

    void enter(int s, const ouch::EnterOrder& o) {
        ++stats_.enters;
        Session& ss = session(s);
        if (o.user_ref <= ss.last_user_ref) {   // retransmission or out of order: ignored per spec
            ++stats_.ignored;
            return;
        }
        ss.last_user_ref = o.user_ref;
        const std::uint16_t locate = lookup(o.symbol);
        std::uint16_t reason = 0;
        if (!locate) reason = ouch::kRejectInvalidSymbol;
        else if (o.qty == 0 || o.qty >= 1'000'000) reason = ouch::kRejectInvalidQuantity;
        else if (o.price == 0 || o.price > kMaxPrice) reason = ouch::kRejectInvalidPrice;
        else if (o.side != 'B' && o.side != 'S' && o.side != 'T' && o.side != 'E') reason = ouch::kRejectOther;
        if (reason) {
            reject(s, o.user_ref, reason, o.clordid);
            return;
        }
        OrderInfo info{};
        info.locate = locate;
        info.session = s;
        info.user_ref = o.user_ref;
        info.side = o.side == 'B' ? 'B' : 'S';
        info.price4 = static_cast<std::uint32_t>(o.price);
        info.tif = o.tif == ouch::kTifIoc ? ouch::kTifIoc : ouch::kTifDay;
        info.display = o.display;
        info.capacity = o.capacity;
        info.cross = o.cross;
        clordid_ = o.clordid;
        submit(info, o.qty, 100 + static_cast<exchange::TraderId>(s));
    }

    void replace(int s, const ouch::ReplaceOrder& o) {
        ++stats_.replaces;
        Session& ss = session(s);
        const auto it = ss.orders.find(o.orig_user_ref);
        const exchange::Order* order = it == ss.orders.end() ? nullptr : find(it->second);
        if (!order || o.user_ref <= ss.last_user_ref) {   // not live, or reused UserRefNum: silently ignored
            ++stats_.ignored;
            return;
        }
        ss.last_user_ref = o.user_ref;
        const exchange::OrderId id = it->second;
        OrderInfo& info = orders_.at(id);
        if (o.price == 0 || o.price > kMaxPrice || o.qty >= 1'000'000 || o.qty <= order->filled_quantity) {
            reject(s, o.user_ref, o.price == 0 || o.price > kMaxPrice ? ouch::kRejectInvalidPrice
                                                                     : ouch::kRejectInvalidQuantity,
                   o.clordid);
            return;
        }
        const exchange::Price price(static_cast<std::int64_t>(o.price));
        const bool in_place = price == order->price && o.qty <= order->quantity;
        const std::uint32_t before = order->remaining();
        if (!in_place) unpublish(info);
        mod_ = Mod{Mod::Replace, s, o.orig_user_ref, o.user_ref, 0, order->filled_quantity};
        clordid_ = o.clordid;
        info.tif = o.tif == ouch::kTifIoc ? ouch::kTifIoc : ouch::kTifDay;
        engine_.modify_order(id, price, o.qty);
        mod_.kind = Mod::None;
        after_modify(id, in_place, before);
    }

    void cancel(int s, const ouch::CancelOrder& c) {
        ++stats_.cancels;
        Session& ss = session(s);
        const auto it = ss.orders.find(c.user_ref);
        const exchange::Order* order = it == ss.orders.end() ? nullptr : find(it->second);
        if (!order) {   // superfluous cancels are silently ignored
            ++stats_.ignored;
            return;
        }
        const exchange::OrderId id = it->second;
        if (c.qty == 0) {
            cancel_reason_ = ouch::kCancelUser;
            engine_.cancel_order(id);
            cancel_reason_ = ouch::kCancelIoc;
        } else if (c.qty < order->remaining()) {   // reduce: keeps time priority
            const std::uint32_t before = order->remaining();
            mod_ = Mod{Mod::Reduce, s, c.user_ref, c.user_ref, before - c.qty, order->filled_quantity};
            engine_.modify_order(id, order->price, order->filled_quantity + c.qty);
            mod_.kind = Mod::None;
            after_modify(id, true, before);
        } else {
            ++stats_.ignored;
        }
    }

    // ----------------------------------------------------------- house orders

    // Limit order from the simulated rest of the market. Returns the engine id
    // (0 if rejected); the order may trade, rest, or (IOC) be cancelled.
    exchange::OrderId house_limit(std::uint16_t locate, char side, std::uint32_t price4, std::uint32_t qty, bool ioc = false) {
        OrderInfo info{};
        info.locate = locate;
        info.session = kHouse;
        info.side = side;
        info.price4 = price4;
        info.tif = ioc ? ouch::kTifIoc : ouch::kTifDay;
        return submit(info, qty, 1);
    }
    bool house_cancel(exchange::OrderId id) { return orders_.count(id) && engine_.cancel_order(id); }

    // ----------------------------------------------------------------- query

    const exchange::OrderBook* book(std::uint16_t locate) const {
        return locate && locate <= symbols_.size() ? engine_.get_book(symbols_[locate - 1]) : nullptr;
    }
    const std::string& symbol(std::uint16_t locate) const { return symbols_[locate - 1]; }
    std::size_t symbol_count() const { return symbols_.size(); }
    std::size_t live_orders() const { return orders_.size(); }
    const GatewayStats& stats() const { return stats_; }

private:
    struct OrderInfo {
        std::uint64_t itch_ref;    // 0 = not on the public feed (not resting)
        std::uint32_t user_ref;    // OUCH UserRefNum (client orders)
        std::uint32_t price4;
        std::uint16_t locate;
        int session;               // kHouse for house orders
        char side, tif, display, capacity, cross;
    };
    struct Session {
        std::uint32_t last_user_ref = 0;
        std::unordered_map<std::uint32_t, exchange::OrderId> orders;   // live orders by UserRefNum
    };
    // The OUCH request behind an engine modify, for on_modify. The engine
    // delivers callbacks after the whole operation, so on_modify may run after
    // the re-priced order already traded: anything it reports "as of the
    // replace" is captured here beforehand.
    struct Mod {
        enum Kind { None, Replace, Reduce } kind = None;
        int session = 0;
        std::uint32_t orig_user_ref = 0, user_ref = 0, decrement = 0;
        std::uint32_t filled_before = 0;
    };

    exchange::OrderId submit(const OrderInfo& info, std::uint32_t qty, exchange::TraderId trader) {
        pending_ = info;
        pending_qty_ = qty;
        const exchange::OrderId id = engine_.submit_order(
            symbols_[info.locate - 1], info.side == 'B' ? exchange::Side::Buy : exchange::Side::Sell,
            exchange::OrderType::Limit, exchange::Price(info.price4), qty,
            info.tif == ouch::kTifIoc ? exchange::TimeInForce::IOC : exchange::TimeInForce::GTC, trader);
        if (id) publish_if_resting(id);
        return id;
    }

    const exchange::Order* find(exchange::OrderId id) const {
        const auto it = orders_.find(id);
        if (it == orders_.end()) return nullptr;
        const exchange::OrderBook* b = book(it->second.locate);
        return b ? b->find(id) : nullptr;
    }

    // An order that is on the book after an operation, and not yet on the
    // public feed, gets an ITCH Add Order with a fresh reference.
    void publish_if_resting(exchange::OrderId id) {
        const exchange::Order* o = find(id);
        if (!o) return;
        OrderInfo& info = orders_.at(id);
        if (info.itch_ref) return;
        info.itch_ref = ++next_itch_ref_;
        info.price4 = static_cast<std::uint32_t>(o->price.raw);
        std::uint8_t m[itch::kAddOrderLen];
        md(m, itch::put_add_order(m, info.locate, out_.now_ns(), info.itch_ref, info.side, o->remaining(),
                                  symbols_[info.locate - 1], info.price4));
    }

    void unpublish(OrderInfo& info) {
        if (!info.itch_ref) return;
        std::uint8_t m[itch::kDeleteLen];
        md(m, itch::put_delete(m, info.locate, out_.now_ns(), info.itch_ref));
        info.itch_ref = 0;
    }

    void after_modify(exchange::OrderId id, bool in_place, std::uint32_t before) {
        const exchange::Order* o = find(id);
        if (!o) return;
        OrderInfo& info = orders_.at(id);
        if (in_place && info.itch_ref && o->remaining() < before) {
            std::uint8_t m[itch::kCancelLen];
            md(m, itch::put_cancel(m, info.locate, out_.now_ns(), info.itch_ref, before - o->remaining()));
        } else if (!in_place) {
            publish_if_resting(id);
        }
    }

    void reject(int s, std::uint32_t user_ref, std::uint16_t reason, std::string_view clordid) {
        ++stats_.rejected;
        std::uint8_t m[ouch::kRejectedLen];
        oe(s, m, ouch::put(m, ouch::Rejected{out_.now_ns(), user_ref, reason, clordid}));
    }

    void forget(exchange::OrderId id) {
        const auto it = orders_.find(id);
        if (it == orders_.end()) return;
        if (it->second.session != kHouse) session(it->second.session).orders.erase(it->second.user_ref);
        orders_.erase(it);
    }

    Session& session(int s) {
        if (static_cast<std::size_t>(s) >= sessions_.size()) sessions_.resize(static_cast<std::size_t>(s) + 1);
        return sessions_[static_cast<std::size_t>(s)];
    }

    std::uint16_t lookup(std::string_view name) const {
        for (std::size_t i = 0; i < symbols_.size(); ++i)
            if (symbols_[i] == name) return static_cast<std::uint16_t>(i + 1);
        return 0;
    }

    void md(const std::uint8_t* m, std::size_t len) {
        ++stats_.itch_messages;
        out_.md(m, len);
    }
    void oe(int s, const std::uint8_t* m, std::size_t len) {
        ++stats_.ouch_messages;
        out_.oe(s, m, len);
    }

    // ------------------------------------------------ engine callbacks

    void on_accept(exchange::OrderId id) override {
        orders_[id] = pending_;
        ++stats_.accepted;
        if (pending_.session == kHouse) return;
        session(pending_.session).orders[pending_.user_ref] = id;
        // The engine accepts before matching: echo the order as entered.
        const ouch::Accepted a{out_.now_ns(), pending_.user_ref, pending_.side, pending_qty_,
                               symbols_[pending_.locate - 1], pending_.price4, pending_.tif, pending_.display, id,
                               pending_.capacity, 'N', pending_.cross, ouch::kStateLive, clordid_};
        std::uint8_t m[ouch::kAcceptedLen];
        oe(pending_.session, m, ouch::put(m, a));
    }

    void on_reject(exchange::OrderId, const char* why) override {
        if (pending_.session == kHouse) return;
        const std::string_view r(why);
        reject(pending_.session, pending_.user_ref,
               r.find("quantity") != r.npos ? ouch::kRejectInvalidQuantity
               : r.find("price") != r.npos  ? ouch::kRejectInvalidPrice
                                            : ouch::kRejectOther,
               clordid_);
    }

    void on_trade(const exchange::Trade&) override { ++match_; }

    void on_fill(exchange::OrderId id, exchange::Price p, exchange::Quantity q, exchange::Quantity remaining) override {
        const auto it = orders_.find(id);
        if (it == orders_.end()) return;
        const OrderInfo& info = it->second;
        ++stats_.executions;
        if (info.itch_ref) {
            std::uint8_t m[itch::kExecutedLen];
            md(m, itch::put_executed(m, info.locate, out_.now_ns(), info.itch_ref, q, match_));
        }
        if (info.session != kHouse) {
            std::uint8_t m[ouch::kExecutedLen];
            oe(info.session, m,
               ouch::put(m, ouch::Executed{out_.now_ns(), info.user_ref, q, static_cast<std::uint64_t>(p.raw),
                                           info.itch_ref ? ouch::kLiquidityAdded : ouch::kLiquidityRemoved, match_}));
        }
        if (remaining == 0) forget(id);
    }

    void on_cancel(exchange::OrderId id, exchange::Quantity remaining) override {
        const auto it = orders_.find(id);
        if (it == orders_.end()) return;
        OrderInfo& info = it->second;
        unpublish(info);
        if (info.session != kHouse && remaining > 0) {
            std::uint8_t m[ouch::kCanceledLen];
            oe(info.session, m, ouch::put(m, ouch::Canceled{out_.now_ns(), info.user_ref, remaining, cancel_reason_}));
        }
        forget(id);
    }

    void on_modify(exchange::OrderId id, exchange::Price p, exchange::Quantity qty) override {
        if (mod_.kind == Mod::None) return;
        OrderInfo& info = orders_.at(id);
        if (mod_.kind == Mod::Reduce) {
            std::uint8_t m[ouch::kCanceledLen];
            oe(mod_.session, m, ouch::put(m, ouch::Canceled{out_.now_ns(), info.user_ref, mod_.decrement, ouch::kCancelUser}));
            return;
        }
        Session& ss = session(mod_.session);
        ss.orders.erase(mod_.orig_user_ref);
        ss.orders[mod_.user_ref] = id;
        info.user_ref = mod_.user_ref;
        info.price4 = static_cast<std::uint32_t>(p.raw);
        ouch::Replaced r{out_.now_ns(), mod_.orig_user_ref, mod_.user_ref, info.side, qty - mod_.filled_before,
                         symbols_[info.locate - 1], static_cast<std::uint64_t>(p.raw), info.tif, info.display, id,
                         info.capacity, 'N', info.cross, ouch::kStateLive, clordid_};
        std::uint8_t m[ouch::kReplacedLen];
        oe(mod_.session, m, ouch::put(m, r));
    }

    void on_modify_reject(exchange::OrderId, const char*) override {
        if (mod_.kind == Mod::Replace) reject(mod_.session, mod_.user_ref, ouch::kRejectOther, clordid_);
    }

    exchange::MatchingEngine engine_;
    Out& out_;
    std::vector<std::string> symbols_;
    std::unordered_map<exchange::OrderId, OrderInfo> orders_;
    std::vector<Session> sessions_;
    OrderInfo pending_{};             // the order being submitted, for on_accept / on_reject
    std::uint32_t pending_qty_ = 0;
    std::string_view clordid_;        // ClOrdID of the OUCH request being handled
    Mod mod_;
    char cancel_reason_ = ouch::kCancelIoc;   // reason for engine-initiated cancels (IOC remainder)
    std::uint64_t next_itch_ref_ = 0;
    std::uint64_t match_ = 0;
    GatewayStats stats_;
};

}  // namespace mde::sim
