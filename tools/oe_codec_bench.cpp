// Order-entry codec cost: OUCH 5.0 (binary, fixed offsets) vs FIX 4.4 (tag=value text).
//
// For each protocol: build the client's new order and parse it the way the
// exchange would, then build the exchange's execution report and parse it the
// way the client would. Single thread, no I/O; FIX timestamps are formatted
// from a counter, so the cost of reading a clock is not included.
//
// Decoders run over a ring of 256 different pre-built messages and every
// result goes through a compiler barrier, so nothing is hoisted out of the
// loop or optimised away.
//
// usage: oe_codec_bench [iterations]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "mde/fix44.h"
#include "mde/ouch50.h"
#include "mde/soupbintcp.h"

using namespace mde;
using Clock = std::chrono::steady_clock;

namespace {

// Forces v to be computed and memory to be re-read on the next iteration.
template <class T>
inline void keep(const T& v) {
    asm volatile("" : : "r"(&v) : "memory");
}

constexpr std::size_t kRing = 256;

struct Ring {
    std::vector<std::vector<std::uint8_t>> msg;
    template <class Build>
    explicit Ring(Build&& build) {
        for (std::size_t i = 0; i < kRing; ++i) msg.push_back(build(i));
    }
    const std::vector<std::uint8_t>& at(std::uint64_t i) const { return msg[i & (kRing - 1)]; }
};

struct OuchSink {
    std::uint64_t acc = 0;
    void on_enter(const ouch::EnterOrder& o) { acc += o.qty + o.price + o.symbol.size() + o.clordid.size(); }
    void on_replace(const ouch::ReplaceOrder&) {}
    void on_cancel(const ouch::CancelOrder&) {}
    void on_system_event(std::uint64_t, char) {}
    void on_accepted(const ouch::Accepted&) {}
    void on_replaced(const ouch::Replaced&) {}
    void on_canceled(const ouch::Canceled&) {}
    void on_executed(const ouch::Executed& e) { acc += e.qty + e.price + e.match + e.user_ref; }
    void on_rejected(const ouch::Rejected&) {}
};

// Median of 5 runs, ns per call.
template <class F>
double time_ns(std::uint64_t n, F&& f) {
    std::vector<double> runs;
    for (int r = 0; r < 5; ++r) {
        const auto t0 = Clock::now();
        for (std::uint64_t i = 0; i < n; ++i) f(i);
        runs.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / double(n));
    }
    std::sort(runs.begin(), runs.end());
    return runs[2];
}

const std::uint64_t kT0 = 1'760'000'000'000'000'000ull;   // an epoch timestamp in 2025

ouch::EnterOrder ouch_order(std::uint64_t i) {
    ouch::EnterOrder o{};
    o.user_ref = static_cast<std::uint32_t>(i + 1);
    o.side = 'B';
    o.qty = static_cast<std::uint32_t>(100 + (i & 7) * 100);
    o.symbol = "AAPL";
    o.price = 1'503'500 + (i & 63) * 100;
    o.tif = ouch::kTifIoc;
    o.clordid = "T123456";
    return o;
}
ouch::Executed ouch_exec(std::uint64_t i) {
    return ouch::Executed{kT0 + i, static_cast<std::uint32_t>(i + 1), static_cast<std::uint32_t>(100 + (i & 7) * 100),
                          1'503'500 + (i & 63) * 100, 'R', i};
}
void fix_order(fix::Encoder& e, std::uint64_t i) {
    e.begin('D', "TRADER", "MDESIM", i + 1, kT0 + i * 1000);
    e.field(fix::tag::ClOrdID, "T123456").field(fix::tag::Symbol, "AAPL").field_char(fix::tag::Side, '1');
    e.field_uint(fix::tag::OrderQty, 100 + (i & 7) * 100).field_char(fix::tag::OrdType, '2');
    e.field_price(fix::tag::Price, 1'503'500 + (i & 63) * 100).field_char(fix::tag::TimeInForce, '3');
    e.field_time(fix::tag::TransactTime, kT0 + i * 1000);
}
void fix_exec(fix::Encoder& e, std::uint64_t i) {
    const std::uint64_t qty = 100 + (i & 7) * 100, px = 1'503'500 + (i & 63) * 100;
    e.begin('8', "MDESIM", "TRADER", i + 1, kT0 + i * 1000);
    e.field_uint(fix::tag::OrderID, 900'000 + i).field(fix::tag::ClOrdID, "T123456");
    e.field_uint(fix::tag::ExecID, i + 1).field_char(fix::tag::ExecType, 'F').field_char(fix::tag::OrdStatus, '2');
    e.field(fix::tag::Symbol, "AAPL").field_char(fix::tag::Side, '1').field_uint(fix::tag::OrderQty, qty);
    e.field_price(fix::tag::Price, px).field_uint(fix::tag::LastQty, qty).field_price(fix::tag::LastPx, px);
    e.field_uint(fix::tag::LeavesQty, 0).field_uint(fix::tag::CumQty, qty).field_price(fix::tag::AvgPx, px);
    e.field_time(fix::tag::TransactTime, kT0 + i * 1000);
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    std::uint8_t buf[128];
    fix::Encoder enc;

    // ------------------------------------------------------------- OUCH
    const double ouch_nos_enc = time_ns(n, [&](std::uint64_t i) {
        const std::size_t len = soup::seal_data(buf, soup::kUnsequencedData, ouch::put(buf + soup::kHeaderLen, ouch_order(i)));
        keep(len);
        keep(buf);
    });
    const double ouch_er_enc = time_ns(n, [&](std::uint64_t i) {
        const std::size_t len = soup::seal_data(buf, soup::kSequencedData, ouch::put(buf + soup::kHeaderLen, ouch_exec(i)));
        keep(len);
        keep(buf);
    });
    const Ring ouch_nos([&](std::size_t i) {
        const std::size_t len = ouch::put(buf, ouch_order(i));
        return std::vector<std::uint8_t>(buf, buf + len);
    });
    const Ring ouch_er([&](std::size_t i) {
        const std::size_t len = ouch::put(buf, ouch_exec(i));
        return std::vector<std::uint8_t>(buf, buf + len);
    });
    OuchSink os;
    const double ouch_nos_dec = time_ns(n, [&](std::uint64_t i) {
        const auto& m = ouch_nos.at(i);
        ouch::decode_inbound(m.data(), m.size(), os);
        keep(os.acc);
    });
    const double ouch_er_dec = time_ns(n, [&](std::uint64_t i) {
        const auto& m = ouch_er.at(i);
        ouch::decode_outbound(m.data(), m.size(), os);
        keep(os.acc);
    });

    // -------------------------------------------------------------- FIX
    const double fix_nos_enc = time_ns(n, [&](std::uint64_t i) {
        fix_order(enc, i);
        const auto m = enc.finish();
        keep(m);
    });
    const double fix_er_enc = time_ns(n, [&](std::uint64_t i) {
        fix_exec(enc, i);
        const auto m = enc.finish();
        keep(m);
    });
    const Ring fix_nos([&](std::size_t i) {
        fix_order(enc, i);
        const auto m = enc.finish();
        return std::vector<std::uint8_t>(m.first, m.first + m.second);
    });
    const Ring fix_er([&](std::size_t i) {
        fix_exec(enc, i);
        const auto m = enc.finish();
        return std::vector<std::uint8_t>(m.first, m.first + m.second);
    });
    std::uint64_t acc = 0;
    const double fix_nos_dec = time_ns(n, [&](std::uint64_t i) {
        const auto& raw = fix_nos.at(i);
        fix::Message m;
        m.parse(raw.data(), raw.size());
        acc += m.get_uint(fix::tag::OrderQty) + m.get_price(fix::tag::Price) + m.get(fix::tag::Symbol).size() +
               static_cast<unsigned char>(m.get(fix::tag::Side)[0]) + m.get(fix::tag::ClOrdID).size();
        keep(acc);
    });
    const double fix_er_dec = time_ns(n, [&](std::uint64_t i) {
        const auto& raw = fix_er.at(i);
        fix::Message m;
        m.parse(raw.data(), raw.size());
        acc += m.get_uint(fix::tag::LastQty) + m.get_price(fix::tag::LastPx) + m.get_uint(fix::tag::LeavesQty) +
               m.get_uint(fix::tag::CumQty) + static_cast<unsigned char>(m.get(fix::tag::ExecType)[0]);
        keep(acc);
    });

    // Single-pass decode: one scan, a switch picks the fields (how FIX engines read).
    const double fix_nos_scan = time_ns(n, [&](std::uint64_t i) {
        const auto& raw = fix_nos.at(i);
        const std::uint8_t* b = raw.data();
        std::uint64_t qty = 0, px = 0, idl = 0, syl = 0;
        char side = 0;
        fix::scan(b, raw.size(), [&](std::uint32_t t, std::size_t v, std::size_t len) {
            const std::string_view s(reinterpret_cast<const char*>(b + v), len);
            switch (t) {
                case fix::tag::OrderQty: qty = fix::parse_uint(s); break;
                case fix::tag::Price: px = fix::parse_price4(s); break;
                case fix::tag::Symbol: syl = len; break;
                case fix::tag::Side: side = s[0]; break;
                case fix::tag::ClOrdID: idl = len; break;
                default: break;
            }
        });
        acc += qty + px + syl + idl + static_cast<unsigned char>(side);
        keep(acc);
    });
    const double fix_er_scan = time_ns(n, [&](std::uint64_t i) {
        const auto& raw = fix_er.at(i);
        const std::uint8_t* b = raw.data();
        std::uint64_t lq = 0, lp = 0, leaves = 0, cum = 0;
        char et = 0;
        fix::scan(b, raw.size(), [&](std::uint32_t t, std::size_t v, std::size_t len) {
            const std::string_view s(reinterpret_cast<const char*>(b + v), len);
            switch (t) {
                case fix::tag::LastQty: lq = fix::parse_uint(s); break;
                case fix::tag::LastPx: lp = fix::parse_price4(s); break;
                case fix::tag::LeavesQty: leaves = fix::parse_uint(s); break;
                case fix::tag::CumQty: cum = fix::parse_uint(s); break;
                case fix::tag::ExecType: et = s[0]; break;
                default: break;
            }
        });
        acc += lq + lp + leaves + cum + static_cast<unsigned char>(et);
        keep(acc);
    });

    std::printf("order-entry codec, %llu iterations, median of 5 runs (ns per message)\n", (unsigned long long)n);
    std::printf("%-36s %10s %10s %9s\n", "", "OUCH 5.0", "FIX 4.4", "FIX/OUCH");
    auto row = [](const char* name, double o, double f) { std::printf("%-36s %10.1f %10.1f %8.0fx\n", name, o, f, f / o); };
    row("new order: encode (client)", ouch_nos_enc, fix_nos_enc);
    row("new order: decode (exchange)", ouch_nos_dec, fix_nos_scan);
    row("execution report: encode (exchange)", ouch_er_enc, fix_er_enc);
    row("execution report: decode (client)", ouch_er_dec, fix_er_scan);
    row("all four, codec only", ouch_nos_enc + ouch_nos_dec + ouch_er_enc + ouch_er_dec,
        fix_nos_enc + fix_nos_scan + fix_er_enc + fix_er_scan);
    std::printf("(FIX decode above is a single scan with a switch on the tag; splitting into\n"
                " fields and looking each one up by tag: new order %.1f ns, execution report %.1f ns)\n",
                fix_nos_dec, fix_er_dec);
    std::printf("%-36s %10zu %10zu   bytes\n", "new order on the wire", ouch_nos.at(0).size() + soup::kHeaderLen,
                fix_nos.at(0).size());
    std::printf("%-36s %10zu %10zu   bytes\n", "execution report on the wire", ouch_er.at(0).size() + soup::kHeaderLen,
                fix_er.at(0).size());
    return 0;
}
