#pragma once
// FIX 4.4 (tag=value): codec, stream framing and the session layer.
//
//   8=FIX.4.4|9=<body length>|35=<type>|49=..|56=..|34=<seq>|52=<time>| ... |10=<checksum>|
//   ('|' is the SOH byte 0x01)
//
// FIX is the industry-standard order-entry protocol: text instead of OUCH's
// fixed binary layout, so it is slower to build and parse but spoken by almost
// every broker and venue. The codec here keeps the same rules as the binary
// paths: no allocation per message and values read in place.
//
//   Encoder   writes the body first and puts "8=FIX.4.4|9=<n>|" in front of it
//             once the length is known, then appends the checksum.
//   Message   parses a message into (tag, offset, length) triples over the
//             original bytes; get(tag) returns a view.
//   Framer    reassembles messages from TCP reads (BodyLength gives the size)
//             and drops anything whose checksum does not match.
//   Session   MsgSeqNum in both directions. A message that arrives ahead of the
//             next expected number triggers a ResendRequest; the peer resends
//             its application messages with PossDupFlag=Y and replaces admin
//             messages with a SequenceReset-GapFill, so every application
//             message is delivered once and in order. Also Logon, Heartbeat,
//             TestRequest and Logout.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mde::fix {

inline constexpr std::uint8_t kSOH = 0x01;

namespace tag {
enum : std::uint32_t {
    AvgPx = 6, BeginSeqNo = 7, BeginString = 8, BodyLength = 9, CheckSum = 10, ClOrdID = 11, CumQty = 14,
    EndSeqNo = 16, ExecID = 17, LastPx = 31, LastQty = 32, MsgSeqNum = 34, MsgType = 35, NewSeqNo = 36,
    OrderID = 37, OrderQty = 38, OrdStatus = 39, OrdType = 40, OrigClOrdID = 41, PossDupFlag = 43, Price = 44,
    RefSeqNum = 45, SenderCompID = 49, SendingTime = 52, Side = 54, Symbol = 55, TargetCompID = 56, Text = 58,
    TimeInForce = 59, TransactTime = 60, EncryptMethod = 98, CxlRejReason = 102, HeartBtInt = 108,
    TestReqID = 112, OrigSendingTime = 122, GapFillFlag = 123, ResetSeqNumFlag = 141, ExecType = 150,
    LeavesQty = 151, CxlRejResponseTo = 434,
};
}

// Admin (session-level) message types; everything else is application data.
inline bool is_admin(char type) {
    return type == '0' || type == '1' || type == '2' || type == '3' || type == '4' || type == '5' || type == 'A';
}

// ------------------------------------------------------------------ values

// Prices travel as decimal text; internally they are integers with 4 implied
// decimals (the same scale as ITCH and OUCH), so no floating point is involved.
inline char* put_price4(char* p, std::uint64_t v) {
    p = std::to_chars(p, p + 20, v / 10000).ptr;
    const auto f = static_cast<unsigned>(v % 10000);
    p[0] = '.';
    p[1] = static_cast<char>('0' + f / 1000);
    p[2] = static_cast<char>('0' + f / 100 % 10);
    p[3] = static_cast<char>('0' + f / 10 % 10);
    p[4] = static_cast<char>('0' + f % 10);
    return p + 5;
}
// Decimal text -> 4 implied decimals; digits beyond the 4th are truncated.
inline std::uint64_t parse_price4(std::string_view s) {
    std::uint64_t ip = 0, fp = 0;
    int fd = 0;
    bool frac = false;
    for (const char c : s) {
        if (c == '.') {
            frac = true;
        } else if (c >= '0' && c <= '9') {
            if (!frac) ip = ip * 10 + static_cast<unsigned>(c - '0');
            else if (fd < 4) fp = fp * 10 + static_cast<unsigned>(c - '0'), ++fd;
        }
    }
    for (; fd < 4; ++fd) fp *= 10;
    return ip * 10000 + fp;
}
inline std::uint64_t parse_uint(std::string_view s) {
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') break;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    return v;
}

// UTCTimestamp "YYYYMMDD-HH:MM:SS.sss" (21 chars) from nanoseconds since the Unix epoch.
inline constexpr std::size_t kTimeLen = 21;
inline void put_utc(char* out, std::uint64_t epoch_ns) {
    const std::uint64_t ms_total = epoch_ns / 1'000'000;
    const auto ms = static_cast<unsigned>(ms_total % 1000);
    const std::uint64_t secs = ms_total / 1000;
    const auto sod = static_cast<unsigned>(secs % 86'400);
    // days -> civil date (proleptic Gregorian), Howard Hinnant's algorithm
    const std::int64_t z = static_cast<std::int64_t>(secs / 86'400) + 719'468;
    const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
    const auto doe = static_cast<unsigned>(z - era * 146'097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36'524 - doe / 146'096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const auto y = static_cast<unsigned>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2));
    auto two = [](char* p, unsigned v) {
        p[0] = static_cast<char>('0' + v / 10);
        p[1] = static_cast<char>('0' + v % 10);
    };
    two(out, y / 100);
    two(out + 2, y % 100);
    two(out + 4, m);
    two(out + 6, d);
    out[8] = '-';
    two(out + 9, sod / 3600);
    out[11] = ':';
    two(out + 12, sod / 60 % 60);
    out[14] = ':';
    two(out + 15, sod % 60);
    out[17] = '.';
    out[18] = static_cast<char>('0' + ms / 100);
    two(out + 19, ms % 100);
}

inline unsigned checksum(const std::uint8_t* p, std::size_t n) {
    unsigned s = 0;
    for (std::size_t i = 0; i < n; ++i) s += p[i];
    return s % 256;
}

// ----------------------------------------------------------------- encoder

class Encoder {
public:
    // Standard header. possdup adds PossDupFlag=Y and OrigSendingTime (resends).
    void begin(char type, std::string_view sender, std::string_view target, std::uint64_t seq, std::uint64_t epoch_ns,
               bool possdup = false, std::string_view orig_sending_time = {}) {
        pos_ = kPrefix;
        overflow_ = false;
        field_char(tag::MsgType, type);
        field(tag::SenderCompID, sender);
        field(tag::TargetCompID, target);
        field_uint(tag::MsgSeqNum, seq);
        if (possdup) field_char(tag::PossDupFlag, 'Y');
        field_time(tag::SendingTime, epoch_ns);
        if (possdup) field(tag::OrigSendingTime, orig_sending_time);
    }

    Encoder& field(std::uint32_t t, std::string_view v) {
        if (!room(16 + v.size())) return *this;
        put_tag(t);
        if (!v.empty()) std::memcpy(buf_ + pos_, v.data(), v.size());
        pos_ += v.size();
        buf_[pos_++] = kSOH;
        return *this;
    }
    Encoder& field_char(std::uint32_t t, char c) { return field(t, std::string_view(&c, 1)); }
    Encoder& field_uint(std::uint32_t t, std::uint64_t v) {
        if (!room(40)) return *this;
        put_tag(t);
        pos_ = static_cast<std::size_t>(std::to_chars(reinterpret_cast<char*>(buf_ + pos_),
                                                      reinterpret_cast<char*>(buf_ + pos_ + 20), v).ptr -
                                        reinterpret_cast<char*>(buf_));
        buf_[pos_++] = kSOH;
        return *this;
    }
    Encoder& field_price(std::uint32_t t, std::uint64_t price4) {
        if (!room(40)) return *this;
        put_tag(t);
        pos_ = static_cast<std::size_t>(put_price4(reinterpret_cast<char*>(buf_ + pos_), price4) -
                                        reinterpret_cast<char*>(buf_));
        buf_[pos_++] = kSOH;
        return *this;
    }
    Encoder& field_time(std::uint32_t t, std::uint64_t epoch_ns) {
        char ts[kTimeLen];
        put_utc(ts, epoch_ns);
        return field(t, std::string_view(ts, kTimeLen));
    }

    // Completes the message: prefix, BodyLength, CheckSum. The bytes stay valid
    // until the next begin(). Returns {nullptr, 0} if the message overflowed.
    std::pair<const std::uint8_t*, std::size_t> finish() {
        if (overflow_) return {nullptr, 0};
        const std::size_t body = pos_ - kPrefix;
        char hdr[kPrefix];
        std::memcpy(hdr, "8=FIX.4.4\x01" "9=", 12);
        char* e = std::to_chars(hdr + 12, hdr + sizeof hdr, body).ptr;
        *e++ = static_cast<char>(kSOH);
        const auto hl = static_cast<std::size_t>(e - hdr);
        const std::size_t start = kPrefix - hl;
        std::memcpy(buf_ + start, hdr, hl);
        const unsigned cs = checksum(buf_ + start, pos_ - start);
        buf_[pos_++] = '1';
        buf_[pos_++] = '0';
        buf_[pos_++] = '=';
        buf_[pos_++] = static_cast<std::uint8_t>('0' + cs / 100);
        buf_[pos_++] = static_cast<std::uint8_t>('0' + cs / 10 % 10);
        buf_[pos_++] = static_cast<std::uint8_t>('0' + cs % 10);
        buf_[pos_++] = kSOH;
        return {buf_ + start, pos_ - start};
    }

private:
    static constexpr std::size_t kPrefix = 24;   // room for "8=FIX.4.4|9=<up to 10 digits>|"
    static constexpr std::size_t kCap = 4096;

    bool room(std::size_t n) {
        if (pos_ + n + 8 > kCap) overflow_ = true;   // + checksum trailer
        return !overflow_;
    }
    void put_tag(std::uint32_t t) {
        pos_ = static_cast<std::size_t>(std::to_chars(reinterpret_cast<char*>(buf_ + pos_),
                                                      reinterpret_cast<char*>(buf_ + pos_ + 10), t).ptr -
                                        reinterpret_cast<char*>(buf_));
        buf_[pos_++] = '=';
    }

    std::uint8_t buf_[kCap];
    std::size_t pos_ = kPrefix;
    bool overflow_ = false;
};

// ------------------------------------------------------------------ parser

// Single pass over a message: f(tag, value_offset, value_length) for each field
// in wire order. Returns false on a malformed message. This is the fast way to
// read FIX: a switch on the tag inside f picks out the fields that matter.
template <class F>
bool scan(const std::uint8_t* p, std::size_t n, F&& f) {
    std::size_t i = 0;
    while (i < n) {
        std::uint32_t t = 0;
        std::size_t j = i;
        for (; j < n && p[j] != '='; ++j) {
            if (p[j] < '0' || p[j] > '9') return false;
            t = t * 10 + static_cast<std::uint32_t>(p[j] - '0');
        }
        if (j >= n || j == i) return false;
        const std::size_t v = j + 1;
        std::size_t e = v;
        while (e < n && p[e] != kSOH) ++e;
        if (e >= n) return false;
        f(t, v, e - v);
        i = e + 1;
    }
    return true;
}

// Random access by tag: splits a message into fields once, then get(tag)
// returns a view. Views stay valid while the bytes do.
class Message {
public:
    bool parse(const std::uint8_t* p, std::size_t n) {
        base_ = p;
        n_ = 0;
        const bool ok = scan(p, n, [&](std::uint32_t t, std::size_t v, std::size_t len) {
            if (n_ < kMaxFields) f_[n_++] = Field{t, static_cast<std::uint32_t>(v), static_cast<std::uint32_t>(len)};
        });
        return ok && n_ > 0;
    }

    std::string_view get(std::uint32_t t) const {
        for (std::size_t k = 0; k < n_; ++k)
            if (f_[k].tag == t) return {reinterpret_cast<const char*>(base_ + f_[k].off), f_[k].len};
        return {};
    }
    bool has(std::uint32_t t) const {
        for (std::size_t k = 0; k < n_; ++k)
            if (f_[k].tag == t) return true;
        return false;
    }
    std::uint64_t get_uint(std::uint32_t t) const { return parse_uint(get(t)); }
    std::uint64_t get_price(std::uint32_t t) const { return parse_price4(get(t)); }
    char type() const {
        const std::string_view v = get(tag::MsgType);
        return v.empty() ? '\0' : v[0];
    }

    // Visits every field (tag, value) in wire order.
    template <class F>
    void for_each(F&& f) const {
        for (std::size_t k = 0; k < n_; ++k)
            f(f_[k].tag, std::string_view(reinterpret_cast<const char*>(base_ + f_[k].off), f_[k].len));
    }

private:
    struct Field {
        std::uint32_t tag, off, len;
    };
    static constexpr std::size_t kMaxFields = 64;
    const std::uint8_t* base_ = nullptr;
    std::size_t n_ = 0;
    Field f_[kMaxFields];
};

// ------------------------------------------------------------------ framer

// Same interface as soup::Framer: recv into tail()/space(), commit(n), then
// drain(f) calls f(msg, len) for each complete, checksum-valid message.
class Framer {
public:
    explicit Framer(std::size_t capacity = std::size_t{1} << 17) : buf_(capacity) {}

    std::uint8_t* tail() { return buf_.data() + end_; }
    std::size_t space() const { return buf_.size() - end_; }
    void commit(std::size_t n) { end_ += n; }

    template <class F>
    std::size_t drain(F&& f) {
        std::size_t off = 0, delivered = 0;
        const std::uint8_t* b = buf_.data();
        while (end_ - off >= kMinMessage) {
            if (std::memcmp(b + off, "8=FIX", 5) != 0) {   // resynchronise on the next BeginString
                const std::size_t next = find(off + 1, "8=FIX", 5);
                ++skipped_;
                off = next == npos ? end_ - 4 : next;
                continue;
            }
            const std::size_t soh = find(off, "\x01" "9=", 3);
            if (soh == npos || soh > off + 16) {   // BodyLength must follow BeginString
                if (soh == npos && end_ - off < 32) break;
                ++malformed_;
                off += 1;
                continue;
            }
            std::size_t i = soh + 3;
            std::size_t body = 0;
            while (i < end_ && b[i] >= '0' && b[i] <= '9') body = body * 10 + (b[i++] - '0');
            if (i >= end_) break;
            if (b[i] != kSOH || body > buf_.size()) {
                ++malformed_;
                off += 1;
                continue;
            }
            const std::size_t head = i + 1 - off;
            const std::size_t total = head + body + 7;   // + "10=XXX|"
            if (total > buf_.size()) {
                ++malformed_;
                off += 1;
                continue;
            }
            if (end_ - off < total) break;
            const std::uint8_t* t = b + off + head + body;
            const bool trailer_ok = t[0] == '1' && t[1] == '0' && t[2] == '=' && t[6] == kSOH;
            const unsigned want = trailer_ok ? static_cast<unsigned>((t[3] - '0') * 100 + (t[4] - '0') * 10 + (t[5] - '0')) : 999;
            if (!trailer_ok || checksum(b + off, head + body) != want) {
                ++malformed_;
                off += 1;
                continue;
            }
            ++delivered;
            const std::size_t at = off;
            off += total;
            if (!f(b + at, total)) break;
        }
        if (off > 0) {
            std::memmove(buf_.data(), buf_.data() + off, end_ - off);
            end_ -= off;
        }
        return delivered;
    }

    std::size_t buffered() const { return end_; }
    std::uint64_t malformed() const { return malformed_; }
    std::uint64_t skipped() const { return skipped_; }

private:
    static constexpr std::size_t kMinMessage = 20;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    std::size_t find(std::size_t from, const char* pat, std::size_t n) const {
        for (std::size_t i = from; i + n <= end_; ++i)
            if (std::memcmp(buf_.data() + i, pat, n) == 0) return i;
        return npos;
    }

    std::vector<std::uint8_t> buf_;
    std::size_t end_ = 0;
    std::uint64_t malformed_ = 0, skipped_ = 0;
};

// ----------------------------------------------------------------- session

struct SessionStats {
    std::uint64_t received = 0, sent = 0;
    std::uint64_t gaps = 0;                 // ResendRequests we sent
    std::uint64_t resend_requests = 0;      // ResendRequests we answered
    std::uint64_t resent = 0;               // application messages resent with PossDupFlag
    std::uint64_t gap_fills_sent = 0, gap_fills_received = 0;
    std::uint64_t duplicates = 0;           // PossDup messages already seen
    std::uint64_t malformed = 0;
};

// One end of a FIX session. Transport-agnostic: every outbound message is
// handed to out(bytes, len); inbound application messages go to
// app.on_app(const Message&) strictly in sequence.
class Session {
public:
    Session(std::string sender, std::string target, bool acceptor, unsigned heartbeat_s = 30)
        : sender_(std::move(sender)), target_(std::move(target)), acceptor_(acceptor), hb_s_(heartbeat_s) {
        history_.reserve(1 << 16);
        log_.reserve(std::size_t{16} << 20);
    }

    // Builds the next outbound message; fill(Encoder&) adds the body fields.
    // Application messages are kept so they can be resent on request.
    template <class Fill>
    std::pair<const std::uint8_t*, std::size_t> build(char type, std::uint64_t epoch_ns, Fill&& fill) {
        enc_.begin(type, sender_, target_, out_seq_, epoch_ns);
        fill(enc_);
        const auto m = enc_.finish();
        if (!m.first) return m;
        if (!is_admin(type)) {   // one contiguous log: no allocation per message once it has grown
            history_.push_back(Sent{out_seq_, log_.size(), m.second});
            log_.insert(log_.end(), m.first, m.first + m.second);
        }
        ++out_seq_;
        ++stats_.sent;
        return m;
    }

    std::pair<const std::uint8_t*, std::size_t> logon(std::uint64_t epoch_ns) {
        return build('A', epoch_ns, [&](Encoder& e) {
            e.field_char(tag::EncryptMethod, '0');
            e.field_uint(tag::HeartBtInt, hb_s_);
        });
    }
    std::pair<const std::uint8_t*, std::size_t> heartbeat(std::uint64_t epoch_ns, std::string_view test_req = {}) {
        return build('0', epoch_ns, [&](Encoder& e) {
            if (!test_req.empty()) e.field(tag::TestReqID, test_req);
        });
    }
    std::pair<const std::uint8_t*, std::size_t> logout(std::uint64_t epoch_ns, std::string_view text = {}) {
        return build('5', epoch_ns, [&](Encoder& e) {
            if (!text.empty()) e.field(tag::Text, text);
        });
    }

    // Handles one inbound message (already framed and checksum-checked).
    template <class App, class Out>
    void on_message(const std::uint8_t* p, std::size_t n, std::uint64_t epoch_ns, App& app, Out&& out) {
        Message m;
        if (!m.parse(p, n)) {
            ++stats_.malformed;
            return;
        }
        ++stats_.received;
        last_rx_ns_ = epoch_ns;
        const char type = m.type();
        const std::uint64_t seq = m.get_uint(tag::MsgSeqNum);
        const bool possdup = m.get(tag::PossDupFlag) == "Y";

        if (type == '4') {   // SequenceReset: gap fill (or reset) moves the next expected number forward
            if (m.get(tag::GapFillFlag) == "Y") ++stats_.gap_fills_received;
            const std::uint64_t to = m.get_uint(tag::NewSeqNo);
            if (to > in_seq_) in_seq_ = to;
            return;
        }
        if (seq > in_seq_) {   // gap: ask for everything from the first missing message
            if (type == 'A') handle_logon(m, epoch_ns, out);   // a logon is processed even when ahead
            if (!resend_pending_) {
                resend_pending_ = true;
                ++stats_.gaps;
                send(out, build('2', epoch_ns, [&](Encoder& e) {
                    e.field_uint(tag::BeginSeqNo, in_seq_);
                    e.field_uint(tag::EndSeqNo, 0);   // 0 = up to the newest message
                }));
            }
            return;
        }
        if (seq < in_seq_) {
            ++stats_.duplicates;   // resend of something already delivered (or a peer error)
            return;
        }
        ++in_seq_;
        if (!possdup) resend_pending_ = false;

        switch (type) {
            case 'A':
                handle_logon(m, epoch_ns, out);
                break;
            case '0':
                break;
            case '1':   // TestRequest: answer with a Heartbeat echoing the id
                send(out, heartbeat(epoch_ns, m.get(tag::TestReqID)));
                break;
            case '2':
                resend(m.get_uint(tag::BeginSeqNo), m.get_uint(tag::EndSeqNo), epoch_ns, out);
                break;
            case '5':
                if (!logout_sent_) {
                    logout_sent_ = true;
                    send(out, logout(epoch_ns));
                }
                logged_on_ = false;
                break;
            default:
                app.on_app(m);
                break;
        }
    }

    // Sends a Heartbeat if nothing went out for HeartBtInt seconds.
    template <class Out>
    void tick(std::uint64_t epoch_ns, std::uint64_t last_tx_ns, Out&& out) {
        if (logged_on_ && epoch_ns > last_tx_ns + std::uint64_t{hb_s_} * 1'000'000'000) send(out, heartbeat(epoch_ns));
    }

    bool logged_on() const { return logged_on_; }
    std::uint64_t next_out_seq() const { return out_seq_; }
    std::uint64_t next_in_seq() const { return in_seq_; }
    const std::string& sender() const { return sender_; }
    const std::string& target() const { return target_; }
    const SessionStats& stats() const { return stats_; }
    void set_logout_sent() { logout_sent_ = true; }

private:
    struct Sent {
        std::uint64_t seq;
        std::size_t off, len;   // in log_
    };

    template <class Out>
    static void send(Out& out, std::pair<const std::uint8_t*, std::size_t> m) {
        if (m.first) out(m.first, m.second);
    }

    template <class Out>
    void handle_logon(const Message& m, std::uint64_t epoch_ns, Out& out) {
        if (acceptor_ && !logged_on_) send(out, logon(epoch_ns));
        if (const auto hb = m.get_uint(tag::HeartBtInt)) hb_s_ = static_cast<unsigned>(hb);
        logged_on_ = true;
    }

    // Resends [begin, end] (end 0 = newest): application messages again with
    // PossDupFlag=Y and their original SendingTime; runs of admin messages are
    // replaced by one SequenceReset-GapFill.
    template <class Out>
    void resend(std::uint64_t begin, std::uint64_t end, std::uint64_t epoch_ns, Out& out) {
        ++stats_.resend_requests;
        const std::uint64_t last = end == 0 || end >= out_seq_ ? out_seq_ - 1 : end;
        std::size_t h = 0;
        while (h < history_.size() && history_[h].seq < begin) ++h;
        std::uint64_t seq = begin;
        while (seq <= last) {
            if (h < history_.size() && history_[h].seq == seq) {
                resend_one(history_[h], epoch_ns, out);
                ++h;
                ++seq;
                continue;
            }
            const std::uint64_t to = h < history_.size() && history_[h].seq <= last ? history_[h].seq : last + 1;
            enc_.begin('4', sender_, target_, seq, epoch_ns, true, now_text(epoch_ns));
            enc_.field_char(tag::GapFillFlag, 'Y');
            enc_.field_uint(tag::NewSeqNo, to);
            send(out, enc_.finish());
            ++stats_.gap_fills_sent;
            seq = to;
        }
    }

    template <class Out>
    void resend_one(const Sent& s, std::uint64_t epoch_ns, Out& out) {
        Message old;
        if (!old.parse(log_.data() + s.off, s.len)) return;
        enc_.begin(old.type(), sender_, target_, s.seq, epoch_ns, true, old.get(tag::SendingTime));
        old.for_each([&](std::uint32_t t, std::string_view v) {
            switch (t) {
                case tag::BeginString: case tag::BodyLength: case tag::CheckSum: case tag::MsgType:
                case tag::SenderCompID: case tag::TargetCompID: case tag::MsgSeqNum: case tag::SendingTime:
                case tag::PossDupFlag: case tag::OrigSendingTime:
                    break;
                default:
                    enc_.field(t, v);
            }
        });
        send(out, enc_.finish());
        ++stats_.resent;
    }

    std::string_view now_text(std::uint64_t epoch_ns) {
        put_utc(now_buf_, epoch_ns);
        return {now_buf_, kTimeLen};
    }

    std::string sender_, target_;
    bool acceptor_;
    unsigned hb_s_;
    std::uint64_t out_seq_ = 1, in_seq_ = 1;
    bool logged_on_ = false, logout_sent_ = false, resend_pending_ = false;
    std::uint64_t last_rx_ns_ = 0;
    std::vector<Sent> history_;          // application messages sent, for resend
    std::vector<std::uint8_t> log_;      // their bytes, back to back
    Encoder enc_;
    char now_buf_[kTimeLen];
    SessionStats stats_;
};

}  // namespace mde::fix
