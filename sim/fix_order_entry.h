#pragma once
// FIX 4.4 order entry in front of the exchange gateway: the way venues often
// run a FIX gateway beside their native binary protocol.
//
//   NewOrderSingle (D)          -> OUCH Enter Order
//   OrderCancelRequest (F)      -> OUCH Cancel Order
//   OrderCancelReplaceRequest (G) -> OUCH Replace Order
//   OUCH Accepted / Executed / Canceled / Replaced / Rejected
//                               -> ExecutionReport (8) or OrderCancelReject (9)
//
// The gateway names orders by a numeric UserRefNum and reports quantities per
// event; FIX names them by the client's ClOrdID and every ExecutionReport
// carries the running CumQty, LeavesQty and AvgPx. This class keeps that
// per-order state. It holds no transport: replies go to send(type, fill),
// where fill(fix::Encoder&) writes the body fields.
//
// The gateway answers synchronously, from inside the call that submitted the
// request, so a request that got no reply at all (the order was no longer
// live) is detected right after the call and answered with a reject.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "mde/fix44.h"
#include "mde/ouch50.h"

namespace mde::sim {

class FixOrderEntry {
public:
    explicit FixOrderEntry(int account) : account_(account) {}

    // An application message from the client, in sequence.
    template <class Gw, class Send>
    void on_request(const fix::Message& m, Gw& gw, std::uint64_t now, Send&& send) {
        now_ = now;
        switch (m.type()) {
            case 'D': new_order(m, gw, send); break;
            case 'F': cancel(m, gw, send); break;
            case 'G': replace(m, gw, send); break;
            default: break;
        }
    }

    // An OUCH reply from the gateway for this account.
    template <class Send>
    void on_ouch(const std::uint8_t* p, std::size_t n, std::uint64_t now, Send&& send) {
        now_ = now;
        Reply<Send> r{*this, send};
        ouch::decode_outbound(p, n, r);
    }

    std::size_t live_orders() const { return orders_.size(); }

private:
    struct Order {
        std::string clordid, symbol;
        std::string pending_cancel;   // ClOrdID of an OrderCancelRequest awaiting its answer
        char side = '1';              // FIX side: 1 buy, 2 sell, 5 sell short
        std::uint64_t order_id = 0, price = 0;
        std::uint32_t cum = 0, leaves = 0;
        std::uint64_t notional = 0;   // sum of fill price * qty, for AvgPx
    };
    struct PendingReplace {
        std::uint32_t orig = 0;
        std::string clordid;
    };

    // ------------------------------------------------------------ requests

    template <class Gw, class Send>
    void new_order(const fix::Message& m, Gw& gw, Send& send) {
        const std::string clordid(m.get(fix::tag::ClOrdID));
        const std::string_view side = m.get(fix::tag::Side);
        const char ouch_side = side == "1" ? 'B' : side == "2" ? 'S' : side == "5" ? 'T' : '\0';
        const std::uint64_t qty = m.get_uint(fix::tag::OrderQty);
        const char* reason = nullptr;
        if (clordid.empty() || !m.has(fix::tag::Symbol) || !m.has(fix::tag::Price)) reason = "missing required field";
        else if (by_clordid_.count(clordid)) reason = "duplicate ClOrdID";
        else if (!ouch_side) reason = "unsupported Side";
        else if (m.get(fix::tag::OrdType) != "2") reason = "only limit orders (OrdType=2) are supported";
        else if (qty == 0 || qty >= 1'000'000) reason = "invalid OrderQty";
        if (reason) {
            Order o;
            o.clordid = clordid;
            o.symbol = std::string(m.get(fix::tag::Symbol));
            o.side = side.empty() ? '1' : side[0];
            exec_report(send, o, '8', '8', reason);
            return;
        }
        const std::uint32_t ref = ++next_ref_;
        Order& o = orders_[ref];
        o.clordid = clordid;
        o.symbol = std::string(m.get(fix::tag::Symbol));
        o.side = side[0];
        o.price = m.get_price(fix::tag::Price);
        by_clordid_[clordid] = ref;

        ouch::EnterOrder e{};
        e.user_ref = ref;
        e.side = ouch_side;
        e.qty = static_cast<std::uint32_t>(qty);
        e.symbol = o.symbol;
        e.price = o.price;
        e.tif = m.get(fix::tag::TimeInForce) == "3" ? ouch::kTifIoc : ouch::kTifDay;
        e.clordid = clordid;
        gw.enter(account_, e);   // replies arrive through on_ouch before this returns
    }

    template <class Gw, class Send>
    void cancel(const fix::Message& m, Gw& gw, Send& send) {
        const std::string clordid(m.get(fix::tag::ClOrdID));
        const std::string orig(m.get(fix::tag::OrigClOrdID));
        const auto it = by_clordid_.find(orig);
        if (it == by_clordid_.end()) {
            cancel_reject(send, clordid, orig, 0, '1', "unknown order");
            return;
        }
        const std::uint32_t ref = it->second;
        orders_[ref].pending_cancel = clordid;
        gw.cancel(account_, ouch::CancelOrder{ref, 0});
        const auto o = orders_.find(ref);
        if (o != orders_.end() && o->second.pending_cancel == clordid) {   // no answer: not live any more
            o->second.pending_cancel.clear();
            cancel_reject(send, clordid, orig, o->second.order_id, '1', "too late to cancel");
        }
    }

    template <class Gw, class Send>
    void replace(const fix::Message& m, Gw& gw, Send& send) {
        const std::string clordid(m.get(fix::tag::ClOrdID));
        const std::string orig(m.get(fix::tag::OrigClOrdID));
        const auto it = by_clordid_.find(orig);
        if (it == by_clordid_.end() || clordid.empty() || by_clordid_.count(clordid)) {
            cancel_reject(send, clordid, orig, 0, '2', it == by_clordid_.end() ? "unknown order" : "invalid ClOrdID");
            return;
        }
        const std::uint32_t orig_ref = it->second;
        const std::uint32_t ref = ++next_ref_;
        replaces_[ref] = PendingReplace{orig_ref, clordid};
        ouch::ReplaceOrder r{};
        r.orig_user_ref = orig_ref;
        r.user_ref = ref;
        r.qty = static_cast<std::uint32_t>(m.get_uint(fix::tag::OrderQty));   // FIX and OUCH: total incl. executed
        r.price = m.get_price(fix::tag::Price);
        r.tif = m.get(fix::tag::TimeInForce) == "3" ? ouch::kTifIoc : ouch::kTifDay;
        r.clordid = clordid;
        gw.replace(account_, r);
        if (replaces_.erase(ref)) {   // no answer: the order was not live
            const auto o = orders_.find(orig_ref);
            cancel_reject(send, clordid, orig, o == orders_.end() ? 0 : o->second.order_id, '2', "order not live");
        }
    }

    // ------------------------------------------------------------- replies

    template <class Send>
    struct Reply {
        FixOrderEntry& self;
        Send& send;
        void on_system_event(std::uint64_t, char) {}
        void on_accepted(const ouch::Accepted& a) {
            const auto it = self.orders_.find(a.user_ref);
            if (it == self.orders_.end()) return;
            Order& o = it->second;
            o.order_id = a.order_ref;
            o.leaves = a.qty;
            o.price = a.price;
            self.exec_report(send, o, '0', '0');
        }
        void on_executed(const ouch::Executed& e) {
            const auto it = self.orders_.find(e.user_ref);
            if (it == self.orders_.end()) return;
            Order& o = it->second;
            o.cum += e.qty;
            o.leaves -= e.qty < o.leaves ? e.qty : o.leaves;
            o.notional += e.price * e.qty;
            self.exec_report(send, o, 'F', o.leaves == 0 ? '2' : '1', nullptr, e.qty, e.price);
            if (o.leaves == 0) self.forget(e.user_ref);
        }
        void on_canceled(const ouch::Canceled& c) {
            const auto it = self.orders_.find(c.user_ref);
            if (it == self.orders_.end()) return;
            Order& o = it->second;
            o.leaves -= c.qty < o.leaves ? c.qty : o.leaves;
            const char status = o.leaves == 0 ? '4' : (o.cum > 0 ? '1' : '0');
            if (!o.pending_cancel.empty()) {   // answer to an OrderCancelRequest: reported under its ClOrdID
                Order reported = o;
                reported.clordid = o.pending_cancel;
                o.pending_cancel.clear();
                self.exec_report(send, reported, '4', status, nullptr, 0, 0, o.clordid);
            } else {   // IOC remainder or exchange-initiated
                self.exec_report(send, o, '4', status);
            }
            if (o.leaves == 0) self.forget(c.user_ref);
        }
        void on_replaced(const ouch::Replaced& r) {
            const auto pr = self.replaces_.find(r.user_ref);
            const auto it = self.orders_.find(r.orig_user_ref);
            if (pr == self.replaces_.end() || it == self.orders_.end()) return;
            Order o = std::move(it->second);
            const std::string orig = o.clordid;
            self.orders_.erase(it);
            self.by_clordid_.erase(orig);
            o.clordid = pr->second.clordid;
            o.leaves = r.qty;
            o.price = r.price;
            o.order_id = r.order_ref;
            self.replaces_.erase(pr);
            self.by_clordid_[o.clordid] = r.user_ref;
            Order& placed = self.orders_[r.user_ref] = std::move(o);
            self.exec_report(send, placed, '5', placed.cum > 0 ? '1' : '0', nullptr, 0, 0, orig);
        }
        void on_rejected(const ouch::Rejected& j) {
            const auto pr = self.replaces_.find(j.user_ref);
            if (pr != self.replaces_.end()) {   // a rejected replace leaves the original intact
                const auto o = self.orders_.find(pr->second.orig);
                const std::string orig = o == self.orders_.end() ? std::string() : o->second.clordid;
                self.cancel_reject(send, pr->second.clordid, orig, o == self.orders_.end() ? 0 : o->second.order_id,
                                   '2', "replace rejected");
                self.replaces_.erase(pr);
                return;
            }
            const auto it = self.orders_.find(j.user_ref);
            if (it == self.orders_.end()) return;
            self.exec_report(send, it->second, '8', '8', "rejected by exchange");
            self.forget(j.user_ref);
        }
    };

    template <class Send>
    void exec_report(Send& send, const Order& o, char exec_type, char status, const char* text = nullptr,
                     std::uint32_t last_qty = 0, std::uint64_t last_px = 0, std::string_view orig = {}) {
        const std::uint64_t exec_id = ++exec_id_;
        send('8', [&](fix::Encoder& e) {
            e.field_uint(fix::tag::OrderID, o.order_id);
            e.field(fix::tag::ClOrdID, o.clordid);
            if (!orig.empty()) e.field(fix::tag::OrigClOrdID, orig);
            e.field_uint(fix::tag::ExecID, exec_id);
            e.field_char(fix::tag::ExecType, exec_type);
            e.field_char(fix::tag::OrdStatus, status);
            e.field(fix::tag::Symbol, o.symbol);
            e.field_char(fix::tag::Side, o.side);
            e.field_uint(fix::tag::OrderQty, std::uint64_t{o.cum} + o.leaves);
            e.field_price(fix::tag::Price, o.price);
            if (last_qty) {
                e.field_uint(fix::tag::LastQty, last_qty);
                e.field_price(fix::tag::LastPx, last_px);
            }
            e.field_uint(fix::tag::LeavesQty, o.leaves);
            e.field_uint(fix::tag::CumQty, o.cum);
            e.field_price(fix::tag::AvgPx, o.cum ? o.notional / o.cum : 0);
            e.field_time(fix::tag::TransactTime, now_);
            if (text) e.field(fix::tag::Text, text);
        });
    }

    template <class Send>
    void cancel_reject(Send& send, std::string_view clordid, std::string_view orig, std::uint64_t order_id,
                       char response_to, const char* text) {
        send('9', [&](fix::Encoder& e) {
            if (order_id) e.field_uint(fix::tag::OrderID, order_id);
            else e.field(fix::tag::OrderID, "NONE");
            e.field(fix::tag::ClOrdID, clordid);
            e.field(fix::tag::OrigClOrdID, orig);
            e.field_char(fix::tag::OrdStatus, '8');
            e.field_char(fix::tag::CxlRejResponseTo, response_to);   // 1 cancel, 2 cancel/replace
            e.field(fix::tag::Text, text);
        });
    }

    void forget(std::uint32_t ref) {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) return;
        by_clordid_.erase(it->second.clordid);
        orders_.erase(it);
    }

    int account_;
    std::uint64_t now_ = 0;
    std::uint32_t next_ref_ = 0;
    std::uint64_t exec_id_ = 0;
    std::unordered_map<std::uint32_t, Order> orders_;              // live orders by UserRefNum
    std::unordered_map<std::string, std::uint32_t> by_clordid_;    // live orders by ClOrdID
    std::unordered_map<std::uint32_t, PendingReplace> replaces_;   // replacement UserRefNum -> request
};

}  // namespace mde::sim
