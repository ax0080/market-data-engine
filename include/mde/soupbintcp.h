#pragma once
// SoupBinTCP 4.0: NASDAQ's session layer over TCP. It carries OUCH order entry
// and TCP market data (ITCH replay / recovery).
//
//   logical packet: length u16 BE (bytes after this field) | type char | payload
//
// The server numbers its Sequenced Data packets implicitly: Login Accepted says
// which sequence number comes next and both sides count from there. A client
// that loses its connection logs in again with the next number it needs and
// the server resumes from that message, so no sequenced message is ever lost.
// Client -> server messages (Unsequenced Data) carry no such guarantee; OUCH
// makes them safe to resend instead.
//
// TCP is a byte stream, so a recv() may end in the middle of a packet or hold
// several. Framer reassembles logical packets and hands them out in place; the
// only copy is moving an incomplete tail to the front of the buffer.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "mde/wire.h"

namespace mde::soup {

// Packet types
inline constexpr char kDebug = '+';
// server -> client
inline constexpr char kLoginAccepted = 'A';
inline constexpr char kLoginRejected = 'J';
inline constexpr char kSequencedData = 'S';
inline constexpr char kServerHeartbeat = 'H';
inline constexpr char kEndOfSession = 'Z';
// client -> server
inline constexpr char kLoginRequest = 'L';
inline constexpr char kUnsequencedData = 'U';
inline constexpr char kClientHeartbeat = 'R';
inline constexpr char kLogoutRequest = 'O';

// Login Rejected reason codes
inline constexpr char kRejectNotAuthorized = 'A';
inline constexpr char kRejectSessionUnavailable = 'S';

inline constexpr std::size_t kHeaderLen = 3;                    // length + type
inline constexpr std::size_t kLoginRequestLen = kHeaderLen + 46;   // user 6, password 10, session 10, sequence 20
inline constexpr std::size_t kLoginAcceptedLen = kHeaderLen + 30;  // session 10, sequence 20
inline constexpr std::size_t kSessionLen = 10;

// Writes the 3-byte header for a packet whose payload is payload_len bytes.
inline std::size_t put_header(std::uint8_t* out, char type, std::size_t payload_len) {
    wire::put16(out, static_cast<std::uint16_t>(payload_len + 1));
    out[2] = static_cast<std::uint8_t>(type);
    return kHeaderLen;
}

// Header-only packets: heartbeats, End of Session, Logout Request.
inline std::size_t put_empty(std::uint8_t* out, char type) { return put_header(out, type, 0); }

inline std::size_t put_login_request(std::uint8_t* out, std::string_view user, std::string_view password,
                                     std::string_view session, std::uint64_t next_seq) {
    put_header(out, kLoginRequest, kLoginRequestLen - kHeaderLen);
    wire::put_alpha(out + 3, 6, user);
    wire::put_alpha(out + 9, 10, password);
    // Requested Session: left-padded with spaces like the session in Login Accepted;
    // all blanks means "the current session".
    std::memset(out + 19, ' ', kSessionLen);
    if (!session.empty()) {
        const std::size_t n = session.size() < kSessionLen ? session.size() : kSessionLen;
        std::memcpy(out + 19 + (kSessionLen - n), session.data(), n);
    }
    wire::put_ascii_num(out + 29, 20, next_seq);
    return kLoginRequestLen;
}

inline std::size_t put_login_accepted(std::uint8_t* out, std::string_view session, std::uint64_t next_seq) {
    put_header(out, kLoginAccepted, kLoginAcceptedLen - kHeaderLen);
    std::memset(out + 3, ' ', kSessionLen);
    const std::size_t n = session.size() < kSessionLen ? session.size() : kSessionLen;
    if (n) std::memcpy(out + 3 + (kSessionLen - n), session.data(), n);
    wire::put_ascii_num(out + 13, 20, next_seq);
    return kLoginAcceptedLen;
}

inline std::size_t put_login_rejected(std::uint8_t* out, char reason) {
    put_header(out, kLoginRejected, 1);
    out[3] = static_cast<std::uint8_t>(reason);
    return kHeaderLen + 1;
}

// Data packet (Sequenced from the server, Unsequenced from the client) around a
// message the caller already wrote at out + kHeaderLen.
inline std::size_t seal_data(std::uint8_t* out, char type, std::size_t msg_len) {
    put_header(out, type, msg_len);
    return kHeaderLen + msg_len;
}

struct LoginRequest {
    std::string_view user, password, session;   // trailing/leading spaces removed
    std::uint64_t next_seq;                     // 0 = start from the newest message
};
inline LoginRequest parse_login_request(const std::uint8_t* payload) {
    std::string_view session = wire::get_alpha(payload + 16, kSessionLen);
    while (!session.empty() && session.front() == ' ') session.remove_prefix(1);
    return {wire::get_alpha(payload, 6), wire::get_alpha(payload + 6, 10), session,
            wire::get_ascii_num(payload + 26, 20)};
}

struct LoginAccepted {
    std::string_view session;
    std::uint64_t next_seq;
};
inline LoginAccepted parse_login_accepted(const std::uint8_t* payload) {
    std::string_view session(reinterpret_cast<const char*>(payload), kSessionLen);
    while (!session.empty() && session.front() == ' ') session.remove_prefix(1);
    return {session, wire::get_ascii_num(payload + kSessionLen, 20)};
}

// Reassembles logical packets from TCP reads:
//   recv(fd, f.tail(), f.space(), 0) -> f.commit(n) -> f.drain(handler)
class Framer {
public:
    explicit Framer(std::size_t capacity = std::size_t{1} << 17) : buf_(capacity) {}

    std::uint8_t* tail() { return buf_.data() + end_; }
    std::size_t space() const { return buf_.size() - end_; }
    void commit(std::size_t n) { end_ += n; }

    // Calls f(type, payload, payload_len) for every complete packet, then moves
    // any incomplete tail to the front. Returns the number of packets delivered.
    // f may return false to stop early (the rest stays buffered).
    template <class F>
    std::size_t drain(F&& f) {
        std::size_t off = 0, packets = 0;
        while (end_ - off >= 2) {
            const std::size_t len = wire::get16(buf_.data() + off);   // type + payload
            if (len == 0) {                                          // malformed: no type byte
                ++malformed_;
                off += 2;
                continue;
            }
            if (end_ - off < 2 + len) break;
            const std::uint8_t* p = buf_.data() + off;
            off += 2 + len;
            ++packets;
            if (!f(static_cast<char>(p[2]), p + kHeaderLen, len - 1)) break;
        }
        if (off > 0) {
            std::memmove(buf_.data(), buf_.data() + off, end_ - off);
            end_ -= off;
        }
        return packets;
    }

    std::size_t buffered() const { return end_; }
    std::uint64_t malformed() const { return malformed_; }

private:
    std::vector<std::uint8_t> buf_;   // a logical packet is at most 2 + 65535 bytes
    std::size_t end_ = 0;
    std::uint64_t malformed_ = 0;
};

}  // namespace mde::soup
