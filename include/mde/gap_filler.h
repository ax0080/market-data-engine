#pragma once
// MoldUDP64 receiver that never skips a message: the recovering counterpart of
// mold::Decoder (which only counts gaps).
//
// Messages go to the handler strictly in sequence. When a packet arrives ahead
// of the next expected sequence number, the filler stops delivering, keeps that
// packet and every later one, and reports the missing range
// [next_sequence(), recovery_end()). The caller fetches those messages from a
// recovery channel (here a SoupBinTCP replay, as with NASDAQ's TCP ITCH feed)
// and passes each one to on_recovered(). As soon as the hole is filled the
// buffered packets are replayed and live delivery resumes.
//
// Heartbeats (message count 0) and End of Session (0xFFFF) carry the next
// sequence number, so a receiver that lost the last packets before the feed
// went quiet still learns about the gap.
//
// The normal path is the same zero-copy decode as mold::Decoder; packets are
// copied only while a gap is open.

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "mde/moldudp64.h"
#include "mde/wire.h"

namespace mde::mold {

struct GapStats {
    std::uint64_t packets = 0;
    std::uint64_t live_messages = 0;        // delivered straight from a live packet
    std::uint64_t buffered_messages = 0;    // delivered from packets held during a gap
    std::uint64_t recovered_messages = 0;   // delivered from the recovery channel
    std::uint64_t gaps = 0;
    std::uint64_t duplicate_packets = 0;
    std::uint64_t overlap_messages = 0;     // already-delivered messages inside a newer packet
    std::uint64_t malformed = 0;
    std::uint64_t max_buffered_packets = 0;
};

template <class Handler>   // Handler: void decode(const std::uint8_t* msg, std::size_t len)
class GapFiller {
public:
    explicit GapFiller(Handler& h, std::uint64_t first_seq = 1) : h_(h), next_(first_seq), gap_end_(first_seq) {}

    void on_packet(const std::uint8_t* p, std::size_t n) {
        ++stats_.packets;
        if (n < kHeader) {
            ++stats_.malformed;
            return;
        }
        const std::uint64_t seq = wire::get64(p + 10);
        const std::uint16_t count = wire::get16(p + 18);
        if (count == kHeartbeat || count == kEndOfSession) {
            if (count == kEndOfSession) end_of_session_ = true;
            if (seq > gap_end_) gap_end_ = seq;
            if (seq > next_ && !recovering_) open_gap();
            return;
        }
        if (seq + count <= next_) {
            ++stats_.duplicate_packets;
            return;
        }
        if (seq + count > gap_end_) gap_end_ = seq + count;
        if (recovering_ || seq > next_) {
            if (!recovering_) open_gap();
            hold(seq, p, n);
            return;
        }
        live_ = true;
        deliver(p, n);
        live_ = false;
    }

    // The next message of the missing range, fetched from the recovery channel.
    // Anything other than next_sequence() is ignored (late or repeated replay).
    void on_recovered(std::uint64_t seq, const std::uint8_t* msg, std::size_t len) {
        if (!recovering_ || seq != next_) return;
        current_ = seq;
        h_.decode(msg, len);
        ++next_;
        ++stats_.recovered_messages;
        drain();
    }

    bool recovering() const { return recovering_; }
    // While recovering: messages [next_sequence(), recovery_end()) are missing.
    std::uint64_t next_sequence() const { return next_; }
    std::uint64_t recovery_end() const { return held_.empty() ? gap_end_ : held_.begin()->first; }
    // During a decode() call: sequence number of the message being delivered,
    // and whether it came straight off the wire (not from a buffer or a replay).
    std::uint64_t current_sequence() const { return current_; }
    bool live() const { return live_; }
    bool end_of_session() const { return end_of_session_; }
    const GapStats& stats() const { return stats_; }

private:
    void open_gap() {
        recovering_ = true;
        ++stats_.gaps;
    }

    void hold(std::uint64_t seq, const std::uint8_t* p, std::size_t n) {
        held_.try_emplace(seq, p, p + n);
        if (held_.size() > stats_.max_buffered_packets) stats_.max_buffered_packets = held_.size();
    }

    // Delivers the messages of one packet that come at or after next_.
    void deliver(const std::uint8_t* p, std::size_t n) {
        std::uint64_t seq = wire::get64(p + 10);
        const std::uint16_t count = wire::get16(p + 18);
        std::size_t off = kHeader;
        for (std::uint16_t i = 0; i < count; ++i, ++seq) {
            if (off + 2 > n) {
                ++stats_.malformed;
                return;
            }
            const std::size_t len = wire::get16(p + off);
            if (off + 2 + len > n) {
                ++stats_.malformed;
                return;
            }
            if (seq < next_) {
                ++stats_.overlap_messages;
            } else {
                current_ = seq;
                h_.decode(p + off + 2, len);
                ++next_;
                ++(live_ ? stats_.live_messages : stats_.buffered_messages);
            }
            off += 2 + len;
        }
    }

    // Replays held packets that are now contiguous; ends recovery when nothing
    // is missing any more.
    void drain() {
        while (!held_.empty() && held_.begin()->first <= next_) {
            const std::vector<std::uint8_t> pkt = std::move(held_.begin()->second);
            held_.erase(held_.begin());
            deliver(pkt.data(), pkt.size());
        }
        if (held_.empty() && next_ >= gap_end_) recovering_ = false;
    }

    Handler& h_;
    std::uint64_t next_;
    std::uint64_t gap_end_;        // highest sequence number known to exist, + 1
    std::uint64_t current_ = 0;
    bool recovering_ = false;
    bool live_ = false;
    bool end_of_session_ = false;
    std::map<std::uint64_t, std::vector<std::uint8_t>> held_;   // packets after the gap, by first sequence
    GapStats stats_;
};

}  // namespace mde::mold
