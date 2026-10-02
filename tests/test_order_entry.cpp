// SoupBinTCP framing, OUCH 5.0 layouts, and MoldUDP64 gap recovery.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "mde/gap_filler.h"
#include "mde/itch50.h"
#include "mde/itch50_writer.h"
#include "mde/l3_book.h"
#include "mde/ouch50.h"
#include "mde/soupbintcp.h"

using namespace mde;

// --------------------------------------------------------------- SoupBinTCP

TEST(soup_login_round_trip) {
    std::uint8_t b[soup::kLoginRequestLen];
    CHECK_EQ(soup::put_login_request(b, "TRADER", "secret", "", 1234), soup::kLoginRequestLen);
    CHECK_EQ(wire::get16(b), 47);   // type + 46 bytes of payload
    CHECK(b[2] == 'L');
    CHECK(std::memcmp(b + 29, "                1234", 20) == 0);   // ASCII, left-padded with spaces
    const soup::LoginRequest r = soup::parse_login_request(b + 3);
    CHECK(r.user == "TRADER");
    CHECK(r.password == "secret");
    CHECK(r.session.empty());
    CHECK_EQ(r.next_seq, 1234);

    std::uint8_t a[soup::kLoginAcceptedLen];
    soup::put_login_accepted(a, "S1", 98765);
    CHECK(std::memcmp(a + 3, "        S1", 10) == 0);   // session is left-padded too
    const soup::LoginAccepted la = soup::parse_login_accepted(a + 3);
    CHECK(la.session == "S1");
    CHECK_EQ(la.next_seq, 98765);
}

// Feeds a byte stream to a Framer in pieces of the given sizes and returns the
// (type, payload) packets it produced.
static std::vector<std::string> frame(const std::vector<std::uint8_t>& stream, const std::vector<std::size_t>& cuts) {
    soup::Framer f(4096);
    std::vector<std::string> out;
    std::size_t off = 0, i = 0;
    while (off < stream.size()) {
        const std::size_t n = std::min({cuts[i++ % cuts.size()], stream.size() - off, f.space()});
        std::memcpy(f.tail(), stream.data() + off, n);
        f.commit(n);
        off += n;
        f.drain([&](char type, const std::uint8_t* p, std::size_t len) {
            out.push_back(std::string(1, type) + std::string(reinterpret_cast<const char*>(p), len));
            return true;
        });
    }
    return out;
}

TEST(soup_framer_handles_split_and_coalesced_reads) {
    std::vector<std::uint8_t> stream;
    std::vector<std::string> expect;
    std::mt19937 rng(7);
    for (int k = 0; k < 200; ++k) {
        std::uint8_t pkt[300];
        const std::size_t len = rng() % 120;   // includes empty payloads (heartbeats)
        for (std::size_t j = 0; j < len; ++j) pkt[3 + j] = static_cast<std::uint8_t>(rng());
        const char type = "SHUA+"[rng() % 5];
        soup::seal_data(pkt, type, len);
        stream.insert(stream.end(), pkt, pkt + 3 + len);
        expect.push_back(std::string(1, type) + std::string(reinterpret_cast<const char*>(pkt + 3), len));
    }
    // one byte at a time, odd sizes, and everything at once
    for (const auto& cuts : {std::vector<std::size_t>{1}, std::vector<std::size_t>{2, 7, 3, 64, 1},
                             std::vector<std::size_t>{4096}}) {
        const auto got = frame(stream, cuts);
        CHECK_EQ(got.size(), expect.size());
        CHECK(got == expect);
    }
}

// --------------------------------------------------------------------- OUCH

struct OuchSink {
    std::vector<ouch::EnterOrder> enters;
    std::vector<ouch::ReplaceOrder> replaces;
    std::vector<ouch::CancelOrder> cancels;
    std::string text;   // keeps decoded string_views alive in the checks below
    void on_enter(const ouch::EnterOrder& o) { enters.push_back(o); }
    void on_replace(const ouch::ReplaceOrder& o) { replaces.push_back(o); }
    void on_cancel(const ouch::CancelOrder& o) { cancels.push_back(o); }

    std::vector<ouch::Accepted> accepted;
    std::vector<ouch::Replaced> replaced;
    std::vector<ouch::Canceled> canceled;
    std::vector<ouch::Executed> executed;
    std::vector<ouch::Rejected> rejected;
    char event = 0;
    void on_system_event(std::uint64_t, char c) { event = c; }
    void on_accepted(const ouch::Accepted& a) { accepted.push_back(a); }
    void on_replaced(const ouch::Replaced& r) { replaced.push_back(r); }
    void on_canceled(const ouch::Canceled& c) { canceled.push_back(c); }
    void on_executed(const ouch::Executed& e) { executed.push_back(e); }
    void on_rejected(const ouch::Rejected& r) { rejected.push_back(r); }
};

TEST(ouch_enter_order_layout_matches_spec) {
    std::uint8_t m[ouch::kEnterOrderLen];
    ouch::EnterOrder o{};
    o.user_ref = 0x01020304;
    o.side = 'B';
    o.qty = 500;
    o.symbol = "AAPL";
    o.price = 1'503'500;   // $150.35
    o.tif = ouch::kTifIoc;
    o.clordid = "T42";
    CHECK_EQ(ouch::put(m, o), 47);
    CHECK(m[0] == 'O');
    CHECK(m[1] == 1 && m[2] == 2 && m[3] == 3 && m[4] == 4);   // UserRefNum at 1, big-endian
    CHECK(m[5] == 'B');                                        // Side at 5
    CHECK_EQ(wire::get32(m + 6), 500);                         // Quantity at 6
    CHECK(std::memcmp(m + 10, "AAPL    ", 8) == 0);            // Symbol at 10, space padded
    CHECK_EQ(wire::get64(m + 18), 1'503'500);                  // Price at 18, 8 bytes
    CHECK(m[26] == '3');                                       // Time In Force at 26
    CHECK(std::memcmp(m + 31, "T42           ", 14) == 0);     // ClOrdID at 31
    CHECK_EQ(wire::get16(m + 45), 0);                          // Appendage Length at 45

    OuchSink s;
    CHECK(ouch::decode_inbound(m, sizeof m, s));
    CHECK_EQ(s.enters.size(), 1);
    CHECK_EQ(s.enters[0].user_ref, 0x01020304);
    CHECK(s.enters[0].symbol == "AAPL");
    CHECK(s.enters[0].clordid == "T42");
    CHECK_EQ(s.enters[0].price, 1'503'500);
}

TEST(ouch_replace_and_cancel_round_trip) {
    OuchSink s;
    std::uint8_t m[64];
    CHECK_EQ(ouch::put(m, ouch::ReplaceOrder{7, 8, 300, 1'000'100, ouch::kTifDay, 'Y', 'N', "R1"}), 40);
    CHECK_EQ(wire::get16(m + 38), 0);   // Appendage Length at 38
    CHECK(ouch::decode_inbound(m, 40, s));
    CHECK_EQ(s.replaces.at(0).orig_user_ref, 7);
    CHECK_EQ(s.replaces.at(0).user_ref, 8);
    CHECK_EQ(s.replaces.at(0).qty, 300);
    CHECK_EQ(s.replaces.at(0).price, 1'000'100);
    CHECK_EQ(ouch::put(m, ouch::CancelOrder{9, 0}), 11);
    CHECK(ouch::decode_inbound(m, 11, s));
    CHECK(ouch::decode_inbound(m, 9, s));   // Appendage Length is optional on Cancel
    CHECK_EQ(s.cancels.size(), 2);
    CHECK(!ouch::decode_inbound(m, 8, s));   // too short
}

TEST(ouch_outbound_round_trip) {
    OuchSink s;
    std::uint8_t m[ouch::kMaxMessageLen];
    CHECK_EQ(ouch::put(m, ouch::Accepted{123, 5, 'S', 200, "MSFT", 4'000'000, '0', 'Y', 77, 'P', 'N', 'N', 'L', "C1"}), 64);
    CHECK_EQ(wire::get64(m + 36), 77);   // Order Reference Number at 36
    CHECK(m[47] == 'L');                 // Order State at 47
    CHECK(ouch::decode_outbound(m, 64, s));
    CHECK_EQ(s.accepted.at(0).order_ref, 77);
    CHECK(s.accepted.at(0).symbol == "MSFT");
    CHECK(s.accepted.at(0).clordid == "C1");

    CHECK_EQ(ouch::put(m, ouch::Executed{1, 5, 100, 4'000'000, 'R', 999}), 36);
    CHECK_EQ(wire::get64(m + 26), 999);   // Match Number at 26
    CHECK(ouch::decode_outbound(m, 36, s));
    CHECK_EQ(s.executed.at(0).qty, 100);
    CHECK(s.executed.at(0).liquidity == 'R');

    CHECK_EQ(ouch::put(m, ouch::Canceled{1, 5, 100, 'I'}), 20);
    CHECK(ouch::decode_outbound(m, 20, s));
    CHECK(s.canceled.at(0).reason == 'I');

    CHECK_EQ(ouch::put(m, ouch::Rejected{1, 6, ouch::kRejectInvalidSymbol, "X"}), 31);
    CHECK(ouch::decode_outbound(m, 31, s));
    CHECK_EQ(s.rejected.at(0).reason, 0x17);

    CHECK_EQ(ouch::put(m, ouch::Replaced{1, 5, 6, 'S', 150, "MSFT", 4'000'100, '0', 'Y', 77, 'P', 'N', 'N', 'L', ""}), 68);
    CHECK(ouch::decode_outbound(m, 68, s));
    CHECK_EQ(s.replaced.at(0).user_ref, 6);
    CHECK_EQ(s.replaced.at(0).qty, 150);
}

// ------------------------------------------------------- MoldUDP64 recovery

namespace {
// A stream of N ITCH Add Orders (one per order ref) packed into MoldUDP64
// packets of k messages, plus the raw messages for the recovery channel.
struct Feed {
    std::vector<std::vector<std::uint8_t>> packets;
    std::vector<std::vector<std::uint8_t>> messages;   // index = sequence - 1
};
Feed make_feed(int n, int per_packet) {
    Feed f;
    mold::PacketBuilder pb("TEST", 1400);
    for (int i = 1; i <= n; ++i) {
        std::uint8_t m[itch::kAddOrderLen];
        const std::size_t len = itch::put_add_order(m, 1, 0, static_cast<std::uint64_t>(i), 'B', 100, "TEST",
                                                    1'000'000 + static_cast<std::uint32_t>(i % 50));
        f.messages.emplace_back(m, m + len);
        pb.append(m, len);
        if (pb.count() == per_packet || i == n) {
            const std::uint8_t* p = pb.data();
            f.packets.emplace_back(p, p + pb.size());
            pb.reset(static_cast<std::uint64_t>(i) + 1);
        }
    }
    return f;
}

struct SeqCheck {
    std::vector<std::uint64_t> refs;
    void decode(const std::uint8_t* m, std::size_t) { refs.push_back(wire::get64(m + 11)); }
};

// Recovery channel stand-in: serves messages from next_sequence() while recovering.
template <class G>
void recover(G& g, const Feed& f) {
    while (g.recovering() && g.next_sequence() < g.recovery_end()) {
        const auto& m = f.messages[g.next_sequence() - 1];
        g.on_recovered(g.next_sequence(), m.data(), m.size());
    }
}
}  // namespace

TEST(gap_filler_recovers_lost_and_reordered_packets) {
    const Feed f = make_feed(2000, 7);
    std::mt19937 rng(11);
    for (int trial = 0; trial < 50; ++trial) {
        // Lose ~10%, duplicate ~5%, and swap ~5% of neighbouring packets.
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < f.packets.size(); ++i) {
            const unsigned r = rng() % 100;
            if (r < 10) continue;
            order.push_back(i);
            if (r < 15) order.push_back(i);
        }
        for (std::size_t i = 1; i < order.size(); ++i)
            if (rng() % 100 < 5) std::swap(order[i - 1], order[i]);

        SeqCheck sink;
        mold::GapFiller<SeqCheck> g(sink);
        for (const std::size_t i : order) {
            g.on_packet(f.packets[i].data(), f.packets[i].size());
            recover(g, f);
        }
        std::uint8_t hb[mold::kHeader];   // end of session tells us about a lost tail
        mold::PacketBuilder("TEST").control(hb, 2001, mold::kEndOfSession);
        g.on_packet(hb, sizeof hb);
        recover(g, f);

        CHECK(!g.recovering());
        CHECK_EQ(sink.refs.size(), 2000);
        bool in_order = true;
        for (std::size_t i = 0; i < sink.refs.size(); ++i) in_order &= sink.refs[i] == i + 1;
        CHECK(in_order);
        CHECK_EQ(g.stats().live_messages + g.stats().buffered_messages + g.stats().recovered_messages, 2000);
    }
}

TEST(gap_filler_heartbeat_reveals_tail_gap) {
    const Feed f = make_feed(30, 10);
    SeqCheck sink;
    mold::GapFiller<SeqCheck> g(sink);
    g.on_packet(f.packets[0].data(), f.packets[0].size());   // 1..10; 11..30 lost
    CHECK(!g.recovering());
    std::uint8_t hb[mold::kHeader];
    mold::PacketBuilder("TEST").control(hb, 31, mold::kHeartbeat);
    g.on_packet(hb, sizeof hb);
    CHECK(g.recovering());
    CHECK_EQ(g.next_sequence(), 11);
    CHECK_EQ(g.recovery_end(), 31);
    recover(g, f);
    CHECK(!g.recovering());
    CHECK_EQ(sink.refs.size(), 30);
    CHECK_EQ(g.stats().recovered_messages, 20);
}

TEST(gap_filler_delivers_book_identical_to_lossless_feed) {
    // Full ITCH round trip: writer -> MoldUDP64 -> loss -> recovery -> L3 book.
    std::vector<std::uint8_t> msgs;
    std::vector<std::vector<std::uint8_t>> all;
    std::mt19937 rng(3);
    std::vector<std::uint64_t> live;
    std::uint64_t ref = 0;
    for (int i = 0; i < 5000; ++i) {
        std::uint8_t m[itch::kMaxWrittenLen];
        std::size_t len;
        if (live.empty() || rng() % 3) {
            live.push_back(++ref);
            len = itch::put_add_order(m, 1, 0, ref, rng() % 2 ? 'B' : 'S', 100 * (1 + rng() % 5), "TEST",
                                      1'000'000 + 100 * (rng() % 20));
        } else {
            const std::size_t k = rng() % live.size();
            len = rng() % 2 ? itch::put_delete(m, 1, 0, live[k]) : itch::put_executed(m, 1, 0, live[k], 100, 1);
            if (m[0] == 'D') {
                live[k] = live.back();
                live.pop_back();
            }
        }
        all.emplace_back(m, m + len);
    }
    Feed f;
    f.messages = all;
    mold::PacketBuilder pb("TEST");
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (!pb.fits(all[i].size())) {
            f.packets.emplace_back(pb.data(), pb.data() + pb.size());
            pb.reset(i + 1);
        }
        pb.append(all[i].data(), all[i].size());
    }
    f.packets.emplace_back(pb.data(), pb.data() + pb.size());

    auto build = [&](bool lossy) {
        auto book = std::make_unique<L3Book>(1 << 16);
        itch::Decoder<L3Book> dec(*book);
        mold::GapFiller<itch::Decoder<L3Book>> g(dec);
        for (std::size_t i = 0; i < f.packets.size(); ++i) {
            if (lossy && i % 4 == 1) continue;
            g.on_packet(f.packets[i].data(), f.packets[i].size());
            recover(g, f);
        }
        std::uint8_t hb[mold::kHeader];
        pb.control(hb, all.size() + 1, mold::kEndOfSession);
        g.on_packet(hb, sizeof hb);
        recover(g, f);
        return book;
    };
    const auto a = build(false), b = build(true);
    CHECK_EQ(a->live_orders(), b->live_orders());
    const auto* ia = a->instrument(1);
    const auto* ib = b->instrument(1);
    CHECK(ia && ib);
    if (ia && ib) {
        CHECK_EQ(ia->bids.depth(), ib->bids.depth());
        CHECK_EQ(ia->asks.depth(), ib->asks.depth());
        bool same = true;
        for (std::size_t k = 0; k < ia->bids.depth(); ++k)
            same &= ia->bids.at(k).price == ib->bids.at(k).price && ia->bids.at(k).qty == ib->bids.at(k).qty;
        for (std::size_t k = 0; k < ia->asks.depth(); ++k)
            same &= ia->asks.at(k).price == ib->asks.at(k).price && ia->asks.at(k).qty == ib->asks.at(k).qty;
        CHECK(same);
    }
}
