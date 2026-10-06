// FIX 4.4: value formats, encoder framing (BodyLength, CheckSum), stream
// framing with corruption, and session-level gap recovery.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "mde/fix44.h"

using namespace mde;

namespace {
std::string str(std::pair<const std::uint8_t*, std::size_t> m) {
    return std::string(reinterpret_cast<const char*>(m.first), m.second);
}
std::string utc(std::uint64_t epoch_ns) {
    char b[fix::kTimeLen];
    fix::put_utc(b, epoch_ns);
    return std::string(b, fix::kTimeLen);
}
std::string price(std::uint64_t p4) {
    char b[32];
    return std::string(b, fix::put_price4(b, p4));
}
}  // namespace

TEST(fix_values) {
    CHECK(price(1'503'500) == "150.3500");
    CHECK(price(5) == "0.0005");
    CHECK_EQ(fix::parse_price4("150.35"), 1'503'500);
    CHECK_EQ(fix::parse_price4("7"), 70'000);
    CHECK_EQ(fix::parse_price4("0.00015"), 1);   // beyond 4 decimals: truncated
    CHECK(utc(0) == "19700101-00:00:00.000");
    CHECK(utc(1'700'000'000'123'000'000ull) == "20231114-22:13:20.123");
    CHECK(utc(951'782'400'000'000'000ull) == "20000229-00:00:00.000");   // leap day
}

TEST(fix_encoder_length_and_checksum) {
    fix::Encoder e;
    e.begin('D', "TRADER", "EXCH", 42, 1'700'000'000'000'000'000ull);
    e.field(fix::tag::ClOrdID, "T123").field(fix::tag::Symbol, "AAPL").field_char(fix::tag::Side, '1');
    e.field_uint(fix::tag::OrderQty, 100).field_char(fix::tag::OrdType, '2').field_price(fix::tag::Price, 1'503'500);
    const std::string m = str(e.finish());

    // Recompute both framing fields independently of the encoder.
    CHECK(m.rfind("8=FIX.4.4\x01" "9=", 0) == 0);
    const std::size_t len_end = m.find('\x01', 12);
    const std::size_t body_len = std::stoul(m.substr(12, len_end - 12));
    const std::size_t trailer = m.rfind("10=");
    CHECK_EQ(trailer - (len_end + 1), body_len);
    unsigned sum = 0;
    for (std::size_t i = 0; i < trailer; ++i) sum += static_cast<unsigned char>(m[i]);
    CHECK_EQ(std::stoul(m.substr(trailer + 3, 3)), sum % 256);
    CHECK(m.back() == '\x01');

    fix::Message p;
    CHECK(p.parse(reinterpret_cast<const std::uint8_t*>(m.data()), m.size()));
    CHECK(p.type() == 'D');
    CHECK(p.get(fix::tag::Symbol) == "AAPL");
    CHECK_EQ(p.get_uint(fix::tag::MsgSeqNum), 42);
    CHECK_EQ(p.get_price(fix::tag::Price), 1'503'500);
    CHECK(p.get(fix::tag::SendingTime) == "20231114-22:13:20.000");
}

TEST(fix_framer_split_coalesced_and_corrupt) {
    fix::Encoder e;
    std::string stream;
    std::vector<std::string> expect;
    std::mt19937 rng(5);
    for (int k = 0; k < 300; ++k) {
        e.begin('8', "EXCH", "TRADER", static_cast<std::uint64_t>(k + 1), 0);
        e.field(fix::tag::ClOrdID, "C" + std::to_string(k));
        const std::string pad(rng() % 80, 'x');
        e.field(fix::tag::Text, pad);
        std::string m = str(e.finish());
        if (k % 37 == 5) {
            m[m.size() / 2] ^= 0x20;   // corrupted in transit: checksum no longer matches
        } else {
            expect.push_back(m);
        }
        if (k % 53 == 7) stream += "garbage\x01" "bytes";   // noise between messages
        stream += m;
    }
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
        fix::Framer f(8192);
        std::vector<std::string> got;
        std::size_t off = 0;
        while (off < stream.size()) {
            const std::size_t n = std::min({chunk, stream.size() - off, f.space()});
            std::memcpy(f.tail(), stream.data() + off, n);
            f.commit(n);
            off += n;
            f.drain([&](const std::uint8_t* p, std::size_t len) {
                got.push_back(std::string(reinterpret_cast<const char*>(p), len));
                return true;
            });
        }
        CHECK_EQ(got.size(), expect.size());
        CHECK(got == expect);
        CHECK(f.malformed() + f.skipped() > 0);
    }
}

// Two sessions joined by an in-memory link that can lose messages in flight.
namespace {
struct Collector {
    std::vector<std::string> ids;
    void on_app(const fix::Message& m) { ids.emplace_back(m.get(fix::tag::ClOrdID)); }
};
struct Link {
    std::deque<std::string> a_to_b, b_to_a;
    std::vector<std::uint64_t> drop;   // MsgSeqNums from A to lose once
    void push_ab(const std::uint8_t* p, std::size_t n) {
        std::string m(reinterpret_cast<const char*>(p), n);
        fix::Message f;
        f.parse(p, n);
        const std::uint64_t seq = f.get_uint(fix::tag::MsgSeqNum);
        const auto it = std::find(drop.begin(), drop.end(), seq);
        if (it != drop.end() && f.get(fix::tag::PossDupFlag) != "Y") {
            drop.erase(it);
            return;
        }
        a_to_b.push_back(std::move(m));
    }
};
}  // namespace

TEST(fix_session_resend_fills_gaps_in_order) {
    fix::Session a("TRADER", "EXCH", false), b("EXCH", "TRADER", true);
    Link link;
    Collector at_a, at_b;
    auto to_b = [&](const std::uint8_t* p, std::size_t n) { link.push_ab(p, n); };
    auto to_a = [&](const std::uint8_t* p, std::size_t n) { link.b_to_a.emplace_back(reinterpret_cast<const char*>(p), n); };
    auto pump = [&] {
        while (!link.a_to_b.empty() || !link.b_to_a.empty()) {
            while (!link.a_to_b.empty()) {
                const std::string m = std::move(link.a_to_b.front());
                link.a_to_b.pop_front();
                b.on_message(reinterpret_cast<const std::uint8_t*>(m.data()), m.size(), 0, at_b, to_a);
            }
            while (!link.b_to_a.empty()) {
                const std::string m = std::move(link.b_to_a.front());
                link.b_to_a.pop_front();
                a.on_message(reinterpret_cast<const std::uint8_t*>(m.data()), m.size(), 0, at_a, to_b);
            }
        }
    };
    auto order = [&](int i) {
        const auto m = a.build('D', 0, [&](fix::Encoder& e) { e.field(fix::tag::ClOrdID, "C" + std::to_string(i)); });
        to_b(m.first, m.second);
    };

    const auto logon = a.logon(0);
    to_b(logon.first, logon.second);
    pump();
    CHECK(a.logged_on() && b.logged_on());

    // seq 1 logon; orders C1..C12 are 2..14 with a heartbeat (admin) at 7;
    // lose orders at seq 5 and 10 the first time they are sent.
    link.drop = {5, 10};
    for (int i = 1; i <= 12; ++i) {
        order(i);
        if (i == 5) {
            const auto hb = a.heartbeat(0);
            to_b(hb.first, hb.second);
        }
    }
    pump();

    std::vector<std::string> want;
    for (int i = 1; i <= 12; ++i) want.push_back("C" + std::to_string(i));
    CHECK(at_b.ids == want);                         // every order once, in order
    CHECK(b.stats().gaps >= 1);
    CHECK(a.stats().resent >= 1);
    CHECK(a.stats().gap_fills_sent >= 1);             // the heartbeat was replaced by a gap fill
    CHECK_EQ(b.next_in_seq(), a.next_out_seq());
}

TEST(fix_session_answers_test_request) {
    fix::Session a("TRADER", "EXCH", false), b("EXCH", "TRADER", true);
    Collector ca, cb;
    std::vector<std::string> to_a;
    auto out_b = [&](const std::uint8_t* p, std::size_t n) { to_a.emplace_back(reinterpret_cast<const char*>(p), n); };
    auto ignore = [](const std::uint8_t*, std::size_t) {};
    const std::string logon = str(a.logon(0));
    b.on_message(reinterpret_cast<const std::uint8_t*>(logon.data()), logon.size(), 0, cb, out_b);
    const std::string tr = str(a.build('1', 0, [](fix::Encoder& e) { e.field(fix::tag::TestReqID, "PING7"); }));
    b.on_message(reinterpret_cast<const std::uint8_t*>(tr.data()), tr.size(), 0, cb, out_b);
    CHECK_EQ(to_a.size(), 2);   // logon reply, heartbeat
    fix::Message hb;
    CHECK(hb.parse(reinterpret_cast<const std::uint8_t*>(to_a[1].data()), to_a[1].size()));
    CHECK(hb.type() == '0');
    CHECK(hb.get(fix::tag::TestReqID) == "PING7");
    (void)ca;
    (void)ignore;
}
