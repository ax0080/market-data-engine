#pragma once
// Coinbase Advanced Trade WebSocket "level2" channel decoder.
//
// Messages look like
//   {"channel":"l2_data","sequence_num":N,"events":[{"type":"snapshot"|"update",
//     "product_id":"BTC-USD","updates":[{"side":"bid"|"offer","event_time":"..",
//     "price_level":"86285.13","new_quantity":"0.00561713"},...]}]}
// sequence_num increases by exactly one per message on a connection (across all
// channels), so any jump is a lost message. A "snapshot" event replaces the book.
// new_quantity is the absolute size at the level, so each update is a SetLevel.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mde/json_scan.h"
#include "mde/types.h"

namespace mde::coinbase {

struct Stats {
    std::uint64_t messages = 0;
    std::uint64_t book_messages = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t gaps = 0;
    std::uint64_t levels = 0;
};

template <EventSink Sink>
class Level2Decoder {
public:
    Level2Decoder(Sink& sink, std::uint16_t symbol) : sink_(sink), symbol_(symbol) {}

    void on_message(std::string_view msg) {
        ++stats_.messages;
        const std::uint64_t seq = json::get_uint(msg, "sequence_num");
        if (have_seq_ && seq != last_seq_ + 1) ++stats_.gaps;
        have_seq_ = true;
        last_seq_ = seq;

        if (json::get_str(msg, "channel") != "l2_data") return;
        ++stats_.book_messages;

        // Walk the events; each starts with "type":"snapshot"|"update".
        std::size_t pos = json::after_key(msg, "events");
        while (pos != std::string_view::npos) {
            std::size_t type_end = 0;
            const std::string_view type = json::get_str(msg, "type", pos, &type_end);
            if (type.empty()) break;
            if (type == "snapshot") {
                ++stats_.snapshots;
                BookEvent e;
                e.type = EventType::ClearBook;
                e.symbol = symbol_;
                e.seq = seq;
                sink_.on_event(e);
            }
            // This event's updates end where the next event's "type" begins.
            const std::size_t next = json::after_key(msg, "type", type_end);
            const std::size_t stop = next == std::string_view::npos ? msg.size() : next;
            std::size_t p = type_end;
            while (true) {
                std::size_t side_end = 0;
                const std::string_view side = json::get_str(msg, "side", p, &side_end);
                if (side.empty() || side_end > stop) break;
                std::size_t px_end = 0, qty_end = 0;
                const std::string_view px = json::get_str(msg, "price_level", side_end, &px_end);
                const std::string_view qty = json::get_str(msg, "new_quantity", px_end, &qty_end);
                if (qty.empty()) break;
                BookEvent e;
                e.type = EventType::SetLevel;
                e.side = side[0] == 'b' ? Side::Buy : Side::Sell;
                e.symbol = symbol_;
                e.price = json::parse_fixed(px);
                e.qty = json::parse_fixed(qty);
                e.seq = seq;
                ++stats_.levels;
                sink_.on_event(e);
                p = qty_end;
            }
            // Rewind to the opening quote of the next "type": key so get_str finds it.
            pos = next == std::string_view::npos ? next : next - (sizeof("\"type\":") - 1);
        }
    }

    const Stats& stats() const { return stats_; }

private:
    Sink& sink_;
    std::uint16_t symbol_;
    bool have_seq_ = false;
    std::uint64_t last_seq_ = 0;
    Stats stats_;
};

}  // namespace mde::coinbase
