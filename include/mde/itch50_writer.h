#pragma once
// ITCH 5.0 message encoders and a MoldUDP64 packet builder: the publishing side
// of itch50.h / moldudp64.h, used by the exchange simulator and the tests.
// Each put_* writes one message (no length prefix) and returns its length.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "mde/moldudp64.h"
#include "mde/wire.h"

namespace mde::itch {

inline constexpr std::size_t kSystemEventLen = 12;
inline constexpr std::size_t kStockDirectoryLen = 39;
inline constexpr std::size_t kAddOrderLen = 36;
inline constexpr std::size_t kExecutedLen = 31;
inline constexpr std::size_t kCancelLen = 23;
inline constexpr std::size_t kDeleteLen = 19;
inline constexpr std::size_t kReplaceLen = 35;
inline constexpr std::size_t kMaxWrittenLen = kStockDirectoryLen;

// System event codes
inline constexpr char kStartOfMessages = 'O';
inline constexpr char kEndOfMessages = 'C';

namespace detail {
inline void header(std::uint8_t* m, char type, std::uint16_t locate, std::uint64_t ts_ns) {
    m[0] = static_cast<std::uint8_t>(type);
    wire::put16(m + 1, locate);
    wire::put16(m + 3, 0);   // tracking number
    wire::put48(m + 5, ts_ns);
}
}  // namespace detail

inline std::size_t put_system_event(std::uint8_t* m, std::uint64_t ts_ns, char code) {
    detail::header(m, 'S', 0, ts_ns);
    m[11] = static_cast<std::uint8_t>(code);
    return kSystemEventLen;
}

inline std::size_t put_stock_directory(std::uint8_t* m, std::uint16_t locate, std::uint64_t ts_ns, std::string_view stock) {
    detail::header(m, 'R', locate, ts_ns);
    wire::put_alpha(m + 11, 8, stock);
    m[19] = 'Q';                  // market category: NASDAQ Global Select
    m[20] = 'N';                  // financial status: normal
    wire::put32(m + 21, 100);     // round lot size
    m[25] = 'N';                  // round lots only
    m[26] = 'C';                  // issue classification: common stock
    wire::put_alpha(m + 27, 2, "Z");
    m[29] = 'P';                  // authenticity: production
    m[30] = 'N';                  // short sale threshold
    m[31] = 'N';                  // IPO flag
    m[32] = '1';                  // LULD tier
    m[33] = 'N';                  // ETP flag
    wire::put32(m + 34, 0);       // ETP leverage factor
    m[38] = 'N';                  // inverse indicator
    return kStockDirectoryLen;
}

inline std::size_t put_add_order(std::uint8_t* m, std::uint16_t locate, std::uint64_t ts_ns, std::uint64_t ref, char side,
                                 std::uint32_t shares, std::string_view stock, std::uint32_t price4) {
    detail::header(m, 'A', locate, ts_ns);
    wire::put64(m + 11, ref);
    m[19] = static_cast<std::uint8_t>(side);
    wire::put32(m + 20, shares);
    wire::put_alpha(m + 24, 8, stock);
    wire::put32(m + 32, price4);
    return kAddOrderLen;
}

inline std::size_t put_executed(std::uint8_t* m, std::uint16_t locate, std::uint64_t ts_ns, std::uint64_t ref,
                                std::uint32_t shares, std::uint64_t match) {
    detail::header(m, 'E', locate, ts_ns);
    wire::put64(m + 11, ref);
    wire::put32(m + 19, shares);
    wire::put64(m + 23, match);
    return kExecutedLen;
}

inline std::size_t put_cancel(std::uint8_t* m, std::uint16_t locate, std::uint64_t ts_ns, std::uint64_t ref,
                              std::uint32_t shares) {
    detail::header(m, 'X', locate, ts_ns);
    wire::put64(m + 11, ref);
    wire::put32(m + 19, shares);
    return kCancelLen;
}

inline std::size_t put_delete(std::uint8_t* m, std::uint16_t locate, std::uint64_t ts_ns, std::uint64_t ref) {
    detail::header(m, 'D', locate, ts_ns);
    wire::put64(m + 11, ref);
    return kDeleteLen;
}

inline std::size_t put_replace(std::uint8_t* m, std::uint16_t locate, std::uint64_t ts_ns, std::uint64_t orig_ref,
                               std::uint64_t new_ref, std::uint32_t shares, std::uint32_t price4) {
    detail::header(m, 'U', locate, ts_ns);
    wire::put64(m + 11, orig_ref);
    wire::put64(m + 19, new_ref);
    wire::put32(m + 27, shares);
    wire::put32(m + 31, price4);
    return kReplaceLen;
}

}  // namespace mde::itch

namespace mde::mold {

// Packs messages into MoldUDP64 packets of at most max_bytes.
class PacketBuilder {
public:
    explicit PacketBuilder(std::string_view session, std::size_t max_bytes = 1400)
        : max_(max_bytes < sizeof buf_ ? max_bytes : sizeof buf_) {
        wire::put_alpha(buf_, 10, session);
        reset(1);
    }

    // True if a message of len bytes still fits in the current packet.
    bool fits(std::size_t len) const { return size_ + 2 + len <= max_; }

    void append(const std::uint8_t* msg, std::size_t len) {
        wire::put16(buf_ + size_, static_cast<std::uint16_t>(len));
        std::memcpy(buf_ + size_ + 2, msg, len);
        size_ += 2 + len;
        ++count_;
    }

    bool empty() const { return count_ == 0; }
    std::uint16_t count() const { return count_; }
    std::uint64_t first_sequence() const { return seq_; }

    // Finishes the packet (header filled in); valid until the next reset().
    const std::uint8_t* data() {
        wire::put64(buf_ + 10, seq_);
        wire::put16(buf_ + 18, count_);
        return buf_;
    }
    std::size_t size() const { return size_; }

    // Starts the next packet; its first message has sequence number next_seq.
    void reset(std::uint64_t next_seq) {
        seq_ = next_seq;
        count_ = 0;
        size_ = kHeader;
    }

    // Heartbeat (count 0) or End of Session (count 0xFFFF): header only,
    // carrying the next sequence number so idle receivers can spot a gap.
    std::size_t control(std::uint8_t* out, std::uint64_t next_seq, std::uint16_t count) const {
        std::memcpy(out, buf_, 10);
        wire::put64(out + 10, next_seq);
        wire::put16(out + 18, count);
        return kHeader;
    }

private:
    std::uint8_t buf_[2048];
    std::size_t max_;
    std::size_t size_ = 0;
    std::uint16_t count_ = 0;
    std::uint64_t seq_ = 1;
};

}  // namespace mde::mold
