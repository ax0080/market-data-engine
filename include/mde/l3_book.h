#pragma once
// Order-by-order (L3) book for every instrument of a feed, built from
// AddOrder / ExecuteOrder / CancelOrder / DeleteOrder / ReplaceOrder events.
// Resting orders live in one OrderTable; each instrument keeps aggregated
// bid and ask PriceLevels, created on first use.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "mde/order_table.h"
#include "mde/price_levels.h"
#include "mde/types.h"

namespace mde {

struct L3Stats {
    std::uint64_t events = 0;
    std::uint64_t unknown_order = 0;   // execute/cancel/delete/replace for an id we never saw
    std::uint64_t duplicate_add = 0;
};

class L3Book {
public:
    struct Instrument {
        PriceLevels bids{Side::Buy};
        PriceLevels asks{Side::Sell};
        PriceLevels& side(Side s) { return s == Side::Buy ? bids : asks; }
    };

    // Instruments are stored inline in one array indexed by symbol id: one less
    // pointer to chase (and cache line to miss) on every event than an array of
    // pointers.
    explicit L3Book(std::size_t order_capacity = std::size_t{1} << 22)
        : orders_(order_capacity), instruments_(kMaxSymbols), used_(kMaxSymbols, false) {}

    void on_event(const BookEvent& e) {
        ++stats_.events;
        switch (e.type) {
            case EventType::AddOrder:
                add(e.order_id, e.symbol, e.side, e.price, e.qty);
                break;
            case EventType::ExecuteOrder:
            case EventType::CancelOrder:
                reduce(e.order_id, e.qty);
                break;
            case EventType::DeleteOrder:
                remove(e.order_id);
                break;
            case EventType::ReplaceOrder: {
                RestingOrder* o = orders_.find(e.order_id);
                if (!o) {
                    ++stats_.unknown_order;
                    break;
                }
                const std::uint16_t sym = o->symbol;
                const Side side = o->side;
                remove_found(o);
                add(e.new_order_id, sym, side, e.price, e.qty);
                break;
            }
            default:
                break;   // L2 events do not apply to an L3 book
        }
    }

    // Instrument book, or nullptr if the symbol never had an order.
    const Instrument* instrument(std::uint16_t symbol) const {
        return used_[symbol] ? &instruments_[symbol] : nullptr;
    }
    std::size_t live_orders() const { return orders_.size(); }
    const L3Stats& stats() const { return stats_; }

private:
    static constexpr std::size_t kMaxSymbols = 65536;

    Instrument& inst(std::uint16_t symbol) {
        used_[symbol] = true;
        return instruments_[symbol];
    }

    void add(std::uint64_t id, std::uint16_t symbol, Side side, Price price, Qty qty) {
        if (!orders_.insert(RestingOrder{id, price, qty, symbol, side})) {
            ++stats_.duplicate_add;
            return;
        }
        inst(symbol).side(side).add(price, qty, +1);
    }

    void reduce(std::uint64_t id, Qty qty) {
        RestingOrder* o = orders_.find(id);
        if (!o) {
            ++stats_.unknown_order;
            return;
        }
        const Qty dq = qty < o->qty ? qty : o->qty;
        o->qty -= dq;
        if (o->qty == 0) {
            inst(o->symbol).side(o->side).add(o->price, -dq, -1);
            orders_.erase(o);
        } else {
            inst(o->symbol).side(o->side).add(o->price, -dq, 0);
        }
    }

    void remove(std::uint64_t id) {
        RestingOrder* o = orders_.find(id);
        if (!o) {
            ++stats_.unknown_order;
            return;
        }
        remove_found(o);
    }

    void remove_found(RestingOrder* o) {
        inst(o->symbol).side(o->side).add(o->price, -o->qty, -1);
        orders_.erase(o);
    }

    OrderTable orders_;
    std::vector<Instrument> instruments_;
    std::vector<bool> used_;
    L3Stats stats_;
};

}  // namespace mde
