#pragma once
// Binance spot diff-depth stream (<symbol>@depth / @depth@100ms) decoder with the
// documented snapshot synchronisation:
//   1. buffer stream events until a REST snapshot (lastUpdateId) is available;
//   2. drop events with u <= lastUpdateId;
//   3. the first applied event must satisfy U <= lastUpdateId + 1 <= u;
//   4. afterwards every event must start at U == previous u + 1. A hole means
//      updates were lost: the book is marked unsynced and waits for a new snapshot.
// Updates carry absolute quantities per price level, so each [price, qty] pair
// becomes one SetLevel event (qty 0 removes the level).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mde/json_scan.h"
#include "mde/types.h"

namespace mde::binance {

struct Stats {
    std::uint64_t updates = 0;        // stream messages seen
    std::uint64_t applied = 0;        // stream messages applied to the book
    std::uint64_t stale = 0;          // dropped: already covered by the snapshot
    std::uint64_t gaps = 0;           // sequence holes detected
    std::uint64_t snapshots_used = 0; // snapshots used to (re)synchronise
    std::uint64_t levels = 0;         // SetLevel events emitted
};

// Update-id range of a diff-depth message, read without decoding the levels.
struct Range {
    std::uint64_t first;   // U
    std::uint64_t last;    // u
};
inline Range range_of(std::string_view msg) { return {json::get_uint(msg, "U"), json::get_uint(msg, "u")}; }

template <EventSink Sink>
class DepthDecoder {
public:
    DepthDecoder(Sink& sink, std::uint16_t symbol) : sink_(sink), symbol_(symbol) {}

    // REST snapshot {"lastUpdateId":N,"bids":[[p,q],...],"asks":[[p,q],...]}.
    // Used only when not synchronised; otherwise ignored (callers may validate with it).
    void on_snapshot(std::string_view snap) {
        if (synced_) return;
        const std::uint64_t last_id = json::get_uint(snap, "lastUpdateId");
        emit_clear();
        emit_side(snap, "bids", Side::Buy, 0);
        emit_side(snap, "asks", Side::Sell, 0);
        last_u_ = last_id;
        synced_ = true;
        ++stats_.snapshots_used;
        std::vector<std::string> pending;
        pending.swap(buffer_);
        for (const std::string& m : pending) on_update(m);
    }

    // Stream message {"e":"depthUpdate","E":..,"s":..,"U":..,"u":..,"b":[..],"a":[..]}.
    void on_update(std::string_view msg) {
        ++stats_.updates;
        if (!synced_) {
            buffer_.emplace_back(msg);   // only while waiting for a snapshot
            return;
        }
        const Range r = range_of(msg);
        if (r.last <= last_u_) {
            ++stats_.stale;
            return;
        }
        if (r.first > last_u_ + 1) {     // hole: lost updates
            ++stats_.gaps;
            synced_ = false;
            buffer_.clear();
            buffer_.emplace_back(msg);
            return;
        }
        const std::uint64_t ts = json::get_uint(msg, "E") * 1'000'000;
        emit_side(msg, "b", Side::Buy, ts, r.last);
        emit_side(msg, "a", Side::Sell, ts, r.last);
        last_u_ = r.last;
        ++stats_.applied;
    }

    bool synced() const { return synced_; }
    std::uint64_t last_update_id() const { return last_u_; }
    const Stats& stats() const { return stats_; }

private:
    void emit_clear() {
        BookEvent e;
        e.type = EventType::ClearBook;
        e.symbol = symbol_;
        sink_.on_event(e);
    }

    void emit_side(std::string_view msg, std::string_view key, Side side, std::uint64_t ts, std::uint64_t seq = 0) {
        const std::size_t open = json::after_key(msg, key);
        if (open == std::string_view::npos) return;
        json::for_each_pair(msg, open, [&](Price p, Qty q) {
            BookEvent e;
            e.type = EventType::SetLevel;
            e.side = side;
            e.symbol = symbol_;
            e.price = p;
            e.qty = q;
            e.seq = seq;
            e.ts_ns = ts;
            ++stats_.levels;
            sink_.on_event(e);
        });
    }

    Sink& sink_;
    std::uint16_t symbol_;
    bool synced_ = false;
    std::uint64_t last_u_ = 0;
    std::vector<std::string> buffer_;
    Stats stats_;
};

}  // namespace mde::binance
