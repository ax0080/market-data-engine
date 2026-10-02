#pragma once
// MoldUDP64 downstream packet decoder (how NASDAQ disseminates ITCH over UDP
// multicast).
//
//   header (20 bytes): session[10] | sequence u64 BE | message_count u16 BE
//   then message_count blocks of: length u16 BE | message bytes
//
// `sequence` is the number of the first message in the packet, so the next
// packet must start at sequence + message_count. Any jump is a gap (lost
// packet); a lower number is a duplicate (e.g. from an A/B feed arbitration) and
// is dropped. Message blocks reuse the ITCH length-prefix framing, so the
// payload goes straight into itch::Decoder::decode() without copying.

#include <cstddef>
#include <cstdint>

#include "mde/itch50.h"

namespace mde::mold {

inline constexpr std::size_t kHeader = 20;
inline constexpr std::uint16_t kHeartbeat = 0;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;

struct Stats {
    std::uint64_t packets = 0;
    std::uint64_t messages = 0;
    std::uint64_t gaps = 0;            // gap events
    std::uint64_t missing = 0;         // messages lost in gaps
    std::uint64_t duplicates = 0;      // packets already seen
    std::uint64_t malformed = 0;
};

template <class MessageHandler>
class Decoder {
public:
    explicit Decoder(MessageHandler& h) : h_(h) {}

    void on_packet(const std::uint8_t* p, std::size_t n) {
        ++stats_.packets;
        if (n < kHeader) {
            ++stats_.malformed;
            return;
        }
        const std::uint64_t seq = itch::be64(p + 10);
        const std::uint16_t count = itch::be16(p + 18);
        if (count == kHeartbeat || count == kEndOfSession) return;
        if (have_seq_) {
            if (seq < next_) {
                ++stats_.duplicates;
                return;
            }
            if (seq > next_) {
                ++stats_.gaps;
                stats_.missing += seq - next_;
            }
        }
        have_seq_ = true;
        next_ = seq + count;
        std::size_t off = kHeader;
        for (std::uint16_t i = 0; i < count; ++i) {
            if (off + 2 > n) {
                ++stats_.malformed;
                return;
            }
            const std::size_t len = itch::be16(p + off);
            if (off + 2 + len > n) {
                ++stats_.malformed;
                return;
            }
            h_.decode(p + off + 2, len);
            off += 2 + len;
            ++stats_.messages;
        }
    }

    std::uint64_t next_sequence() const { return next_; }
    const Stats& stats() const { return stats_; }

private:
    MessageHandler& h_;
    bool have_seq_ = false;
    std::uint64_t next_ = 0;
    Stats stats_;
};

}  // namespace mde::mold
