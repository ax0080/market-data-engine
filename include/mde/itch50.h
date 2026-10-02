#pragma once
// NASDAQ TotalView-ITCH 5.0 decoder.
//
// Zero-copy: fields are read in place from the receive buffer at fixed offsets
// (big-endian, byte-swapped on load); nothing is copied into an intermediate
// struct. The message type byte drives a switch that the compiler lowers to a
// jump table. Only the messages that change the book (A F E C X D U) emit
// events; R (stock directory) records symbol names; everything else is counted.
//
// Framing: the NASDAQ sample files and MoldUDP64 payloads both prefix each
// message with a 2-byte big-endian length, handled by decode_stream().

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "mde/types.h"

namespace mde::itch {

inline std::uint16_t be16(const std::uint8_t* p) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}
inline std::uint32_t be32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
inline std::uint64_t be64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}
inline std::uint64_t be48(const std::uint8_t* p) {   // ITCH timestamps: 6-byte ns since midnight
    return (std::uint64_t{be16(p)} << 32) | be32(p + 2);
}

// Prices are 4 decimal places on the wire; shares are whole numbers.
inline constexpr Price kPriceMul = kScale / 10'000;

struct Stats {
    std::uint64_t messages = 0;
    std::uint64_t book_events = 0;
    std::array<std::uint64_t, 256> by_type{};
    std::uint64_t truncated = 0;
};

template <EventSink Sink>
class Decoder {
public:
    explicit Decoder(Sink& sink) : sink_(sink) {}

    // Decode one message (pointer at the type byte, len = message length).
    void decode(const std::uint8_t* m, std::size_t len) {
        ++stats_.messages;
        ++stats_.by_type[m[0]];
        BookEvent e;
        switch (m[0]) {
            case 'A':   // Add Order, no attribution
            case 'F':   // Add Order with MPID attribution
                if (len < 36) return truncated();
                e.type = EventType::AddOrder;
                e.order_id = be64(m + 11);
                e.side = m[19] == 'B' ? Side::Buy : Side::Sell;
                e.qty = Qty{be32(m + 20)} * kScale;
                e.price = Price{be32(m + 32)} * kPriceMul;
                break;
            case 'E':   // Order Executed
                if (len < 31) return truncated();
                e.type = EventType::ExecuteOrder;
                e.order_id = be64(m + 11);
                e.qty = Qty{be32(m + 19)} * kScale;
                break;
            case 'C':   // Order Executed With Price (price does not change the resting order)
                if (len < 36) return truncated();
                e.type = EventType::ExecuteOrder;
                e.order_id = be64(m + 11);
                e.qty = Qty{be32(m + 19)} * kScale;
                break;
            case 'X':   // Order Cancel (partial)
                if (len < 23) return truncated();
                e.type = EventType::CancelOrder;
                e.order_id = be64(m + 11);
                e.qty = Qty{be32(m + 19)} * kScale;
                break;
            case 'D':   // Order Delete
                if (len < 19) return truncated();
                e.type = EventType::DeleteOrder;
                e.order_id = be64(m + 11);
                break;
            case 'U':   // Order Replace
                if (len < 35) return truncated();
                e.type = EventType::ReplaceOrder;
                e.order_id = be64(m + 11);
                e.new_order_id = be64(m + 19);
                e.qty = Qty{be32(m + 27)} * kScale;
                e.price = Price{be32(m + 31)} * kPriceMul;
                break;
            case 'R':   // Stock Directory: locate -> symbol
                if (len >= 19) symbols_[be16(m + 1)].assign(reinterpret_cast<const char*>(m + 11), 8);
                return;
            default:
                return;
        }
        e.symbol = be16(m + 1);
        e.ts_ns = be48(m + 5);
        ++stats_.book_events;
        sink_.on_event(e);
    }

    // Decode as many complete length-prefixed messages as `buf` holds.
    // Returns the number of bytes consumed; the caller keeps the remainder.
    std::size_t decode_stream(const std::uint8_t* buf, std::size_t n) {
        std::size_t off = 0;
        while (off + 2 <= n) {
            const std::size_t len = be16(buf + off);
            if (off + 2 + len > n) break;
            if (len > 0) decode(buf + off + 2, len);
            off += 2 + len;
        }
        return off;
    }

    const Stats& stats() const { return stats_; }
    const std::string& symbol_name(std::uint16_t locate) const { return symbols_[locate]; }

private:
    void truncated() { ++stats_.truncated; }

    Sink& sink_;
    Stats stats_;
    std::vector<std::string> symbols_ = std::vector<std::string>(65536);   // heap: 2 MB
};

}  // namespace mde::itch
