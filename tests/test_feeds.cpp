#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "mde/binance.h"
#include "mde/coinbase.h"
#include "mde/itch50.h"
#include "mde/json_scan.h"
#include "mde/l2_book.h"
#include "mde/l3_book.h"
#include "mde/moldudp64.h"

using namespace mde;

// ---------------------------------------------------------------- ITCH builders
namespace {
struct Wire {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void be(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void str(const char* s, int n) {
        for (int i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>(s[i] ? s[i] : ' '));
    }
};
// Appends one length-prefixed message with the common header (locate 7, tracking 0, ts 1234).
void msg(std::vector<std::uint8_t>& out, char type, const std::vector<std::uint8_t>& body) {
    Wire w;
    w.u8(static_cast<std::uint8_t>(type));
    w.be(7, 2);
    w.be(0, 2);
    w.be(1234, 6);
    w.b.insert(w.b.end(), body.begin(), body.end());
    const std::size_t len = w.b.size();
    out.push_back(static_cast<std::uint8_t>(len >> 8));
    out.push_back(static_cast<std::uint8_t>(len));
    out.insert(out.end(), w.b.begin(), w.b.end());
}
std::vector<std::uint8_t> add_body(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price4) {
    Wire w;
    w.be(ref, 8);
    w.u8(static_cast<std::uint8_t>(side));
    w.be(shares, 4);
    w.str("MSFT    ", 8);
    w.be(price4, 4);
    return w.b;
}
std::vector<std::uint8_t> ref_qty(std::uint64_t ref, std::uint32_t q, bool match) {
    Wire w;
    w.be(ref, 8);
    w.be(q, 4);
    if (match) w.be(99, 8);
    return w.b;
}
}  // namespace

TEST(itch_stream_builds_l3_book) {
    std::vector<std::uint8_t> s;
    {
        Wire r;   // stock directory body (only the symbol matters here), padded to full length
        r.str("MSFT    ", 8);
        for (int i = 0; i < 20; ++i) r.u8(0);
        msg(s, 'R', r.b);
    }
    msg(s, 'A', add_body(1, 'B', 100, 1'000'000));   // buy 100 @ 100.0000
    msg(s, 'A', add_body(2, 'S', 50, 1'000'100));    // sell 50 @ 100.0100
    msg(s, 'E', ref_qty(1, 40, true));               // 40 executed
    msg(s, 'X', ref_qty(2, 10, false));              // 10 cancelled
    {
        Wire u;   // replace 1 -> 3 : 70 @ 99.9900
        u.be(1, 8);
        u.be(3, 8);
        u.be(70, 4);
        u.be(999'900, 4);
        msg(s, 'U', u.b);
    }
    {
        Wire d;
        d.be(2, 8);
        msg(s, 'D', d.b);
    }

    L3Book book(64);
    itch::Decoder<L3Book> dec(book);
    // Feed in two chunks split mid-message to exercise framing.
    const std::size_t cut = 60;   // R is 41 bytes on the wire, so this lands inside the first A
    CHECK(cut < s.size());
    const std::size_t used = dec.decode_stream(s.data(), cut);
    std::vector<std::uint8_t> rest(s.begin() + static_cast<std::ptrdiff_t>(used), s.end());
    CHECK_EQ(dec.decode_stream(rest.data(), rest.size()), rest.size());

    CHECK_EQ(dec.stats().messages, 7u);
    CHECK_EQ(dec.stats().book_events, 6u);
    CHECK(dec.symbol_name(7) == "MSFT    ");
    const L3Book::Instrument* in = book.instrument(7);
    CHECK(in != nullptr);
    CHECK_EQ(in->bids.best().price, 9'999'000'000LL);   // 99.99 * 1e8
    CHECK_EQ(in->bids.best().qty, 70 * kScale);
    CHECK(in->asks.empty());
    CHECK_EQ(book.live_orders(), 1u);
    CHECK_EQ(book.stats().unknown_order, 0u);
}

// ---------------------------------------------------------------- MoldUDP64
namespace {
// MoldUDP64 packet carrying `count` copies of a Delete message, first sequence `seq`.
std::vector<std::uint8_t> mold_packet(std::uint64_t seq, std::uint16_t count) {
    std::vector<std::uint8_t> p(20, 0);
    std::memcpy(p.data(), "SESSION001", 10);
    for (int i = 0; i < 8; ++i) p[10 + i] = static_cast<std::uint8_t>(seq >> (8 * (7 - i)));
    p[18] = static_cast<std::uint8_t>(count >> 8);
    p[19] = static_cast<std::uint8_t>(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        std::vector<std::uint8_t> one;
        Wire d;
        d.be(1000 + seq + i, 8);
        msg(one, 'D', d.b);
        p.insert(p.end(), one.begin(), one.end());
    }
    return p;
}
}  // namespace

TEST(moldudp64_sequencing) {
    L3Book book(64);
    itch::Decoder<L3Book> itch_dec(book);
    mold::Decoder<itch::Decoder<L3Book>> dec(itch_dec);
    auto feed = [&](std::uint64_t seq, std::uint16_t n) {
        const auto p = mold_packet(seq, n);
        dec.on_packet(p.data(), p.size());
    };
    feed(1, 3);    // messages 1..3
    feed(4, 2);    // 4..5, contiguous
    feed(4, 2);    // duplicate (e.g. the B feed): dropped
    feed(9, 1);    // 6..8 lost
    CHECK_EQ(dec.stats().messages, 6u);
    CHECK_EQ(dec.stats().duplicates, 1u);
    CHECK_EQ(dec.stats().gaps, 1u);
    CHECK_EQ(dec.stats().missing, 3u);
    CHECK_EQ(dec.next_sequence(), 10u);
    CHECK_EQ(itch_dec.stats().messages, 6u);
}

// ---------------------------------------------------------------- JSON
TEST(json_parse_fixed_is_exact) {
    CHECK_EQ(json::parse_fixed("86324.95000000"), 8'632'495'000'000LL);
    CHECK_EQ(json::parse_fixed("0.00000001"), 1);
    CHECK_EQ(json::parse_fixed("0.000013"), 1'300);
    CHECK_EQ(json::parse_fixed("12"), 1'200'000'000);
    CHECK_EQ(json::parse_fixed("1.123456789"), 112'345'678);   // 9th digit truncated
    CHECK_EQ(json::parse_fixed("-2.5"), -250'000'000);
}

TEST(json_keys_and_pairs) {
    const std::string m = R"({"e":"depthUpdate","E":1700,"s":"BTCUSDT","U":10,"u":12,"b":[["1.5","2"],["1.4","0.00000000"]],"a":[]})";
    CHECK_EQ(json::get_uint(m, "U"), 10u);
    CHECK_EQ(json::get_uint(m, "u"), 12u);
    CHECK(json::get_str(m, "s") == "BTCUSDT");
    int n = 0;
    Price last_p = 0;
    json::for_each_pair(m, json::after_key(m, "b"), [&](Price p, Qty) {
        ++n;
        last_p = p;
    });
    CHECK_EQ(n, 2);
    CHECK_EQ(last_p, 140'000'000);
    int na = 0;
    json::for_each_pair(m, json::after_key(m, "a"), [&](Price, Qty) { ++na; });
    CHECK_EQ(na, 0);
}

// ---------------------------------------------------------------- Binance
namespace {
std::string upd(std::uint64_t U, std::uint64_t u, const char* bids, const char* asks = "[]") {
    return std::string(R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":)") + std::to_string(U) + R"(,"u":)" +
           std::to_string(u) + R"(,"b":)" + bids + R"(,"a":)" + asks + "}";
}
}  // namespace

TEST(binance_snapshot_sync_and_gap_detection) {
    L2Book book;
    binance::DepthDecoder<L2Book> dec(book, 0);
    // Stream starts before the snapshot: these are buffered.
    dec.on_update(upd(90, 95, R"([["10","1"]])"));     // fully before snapshot -> stale
    dec.on_update(upd(96, 105, R"([["11","2"]])"));    // straddles lastUpdateId=100 -> applied first
    CHECK(!dec.synced());
    dec.on_snapshot(R"({"lastUpdateId":100,"bids":[["10","5"],["9","1"]],"asks":[["12","3"]]})");
    CHECK(dec.synced());
    CHECK_EQ(dec.stats().stale, 1u);
    CHECK_EQ(dec.last_update_id(), 105u);
    CHECK_EQ(book.bids.best().price, 11 * kScale);
    CHECK_EQ(book.bids.at(1).qty, 5 * kScale);   // snapshot level kept

    dec.on_update(upd(106, 110, R"([["11","0"]])"));   // contiguous; removes level 11
    CHECK_EQ(book.bids.best().price, 10 * kScale);
    dec.on_update(upd(115, 120, R"([["8","1"]])"));    // hole 111..114
    CHECK_EQ(dec.stats().gaps, 1u);
    CHECK(!dec.synced());
    dec.on_snapshot(R"({"lastUpdateId":118,"bids":[["7","4"]],"asks":[["12","3"]]})");
    CHECK(dec.synced());   // resynced; buffered 115..120 straddles 118 and is applied
    CHECK_EQ(dec.last_update_id(), 120u);
    CHECK_EQ(book.bids.best().price, 8 * kScale);
    CHECK_EQ(book.bids.depth(), 2u);
}

// ---------------------------------------------------------------- Coinbase
TEST(coinbase_snapshot_updates_and_sequence) {
    L2Book book;
    coinbase::Level2Decoder<L2Book> dec(book, 0);
    dec.on_message(R"({"channel":"l2_data","sequence_num":0,"events":[{"type":"snapshot","product_id":"BTC-USD","updates":[)"
                   R"({"side":"bid","event_time":"t","price_level":"100.5","new_quantity":"2"},)"
                   R"({"side":"bid","event_time":"t","price_level":"100.4","new_quantity":"1"},)"
                   R"({"side":"offer","event_time":"t","price_level":"100.7","new_quantity":"3"}]}]})");
    dec.on_message(R"({"channel":"subscriptions","sequence_num":1,"events":[{"subscriptions":{}}]})");
    dec.on_message(R"({"channel":"l2_data","sequence_num":2,"events":[{"type":"update","product_id":"BTC-USD","updates":[)"
                   R"({"side":"bid","event_time":"t","price_level":"100.5","new_quantity":"0"}]}]})");
    CHECK_EQ(dec.stats().gaps, 0u);
    CHECK_EQ(dec.stats().snapshots, 1u);
    CHECK_EQ(book.bids.best().price, 10'040'000'000LL);
    CHECK_EQ(book.asks.best().qty, 3 * kScale);
    dec.on_message(R"({"channel":"l2_data","sequence_num":5,"events":[]})");   // 3 and 4 lost
    CHECK_EQ(dec.stats().gaps, 1u);
}
