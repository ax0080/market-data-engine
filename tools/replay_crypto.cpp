// Replay a recorded Binance or Coinbase stream (tools/record_ws.py output),
// rebuild the L2 book, measure decode+update cost, and validate the book.
//
// Binance validation: every REST snapshot recorded after sync is compared with
// the reconstructed book, level by level, at the snapshot's lastUpdateId L.
// Diff messages cover ranges of update ids, so L often falls inside one message;
// the price levels touched by messages that span or follow L are excluded from
// that comparison (their exact size at L is not observable). All other levels
// must match exactly.
//
// usage: replay_crypto binance|coinbase <file.tsv> [tick|vector]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mde/binance.h"
#include "mde/coinbase.h"
#include "mde/json_scan.h"
#include "mde/l2_book.h"

using namespace mde;
using Clock = std::chrono::steady_clock;

namespace {

struct Line {
    char kind;
    std::string_view payload;
};

std::vector<Line> load(const std::string& path, std::string& storage) {
    std::ifstream f(path, std::ios::binary);
    storage.assign(std::istreambuf_iterator<char>(f), {});
    std::vector<Line> out;
    std::size_t p = 0;
    while (p < storage.size()) {
        std::size_t e = storage.find('\n', p);
        if (e == std::string::npos) e = storage.size();
        const std::string_view line(storage.data() + p, e - p);
        const std::size_t t1 = line.find('\t');
        const std::size_t t2 = line.find('\t', t1 + 1);
        if (t1 != std::string_view::npos && t2 != std::string_view::npos)
            out.push_back({line[t1 + 1], line.substr(t2 + 1)});
        p = e + 1;
    }
    return out;
}

using LevelMap = std::unordered_map<Price, Qty>;

void collect_prices(std::string_view msg, std::string_view key, std::unordered_set<Price>& out) {
    const std::size_t open = json::after_key(msg, key);
    if (open != std::string_view::npos) json::for_each_pair(msg, open, [&](Price p, Qty) { out.insert(p); });
}

struct Compare {
    std::uint64_t snapshots = 0, levels = 0, mismatches = 0, excluded = 0;
};

// Compare one side of the book with the snapshot's levels from the best down to
// the snapshot's K-th level (both directions: missing and extra levels count).
template <class Levels>
void compare_side(const Levels& ours, std::string_view snap, std::string_view key, Side side,
                  const std::unordered_set<Price>& touched, Compare& c, std::size_t K) {
    std::vector<std::pair<Price, Qty>> s;
    json::for_each_pair(snap, json::after_key(snap, key), [&](Price p, Qty q) {
        if (s.size() < K) s.emplace_back(p, q);
    });
    if (s.empty()) return;
    const Price worst = s.back().first;
    auto within = [&](Price p) { return side == Side::Buy ? p >= worst : p <= worst; };
    LevelMap mine;
    ours.for_each([&](Price p, Qty q) {
        if (!within(p)) return false;
        mine[p] = q;
        return true;
    });
    LevelMap theirs(s.begin(), s.end());
    for (const auto& [p, q] : theirs) {
        if (touched.count(p)) {
            ++c.excluded;
            continue;
        }
        ++c.levels;
        auto it = mine.find(p);
        if (it == mine.end() || it->second != q) ++c.mismatches;
    }
    for (const auto& [p, q] : mine)
        if (!touched.count(p) && !theirs.count(p)) {
            ++c.levels;
            ++c.mismatches;
        }
}

template <class Book>
int run_binance(const std::vector<Line>& lines, Book& book) {
    binance::DepthDecoder<Book> dec(book, 0);
    Compare cmp;
    struct Applied {
        std::string_view msg;
        std::uint64_t u;
    };
    std::vector<Applied> recent;                 // applied diffs, for touched-level exclusion
    std::string_view pending;                    // snapshot waiting for the message that crosses L
    std::uint64_t pending_L = 0;
    std::uint64_t diffs = 0;
    double ns_diff = 0, ns_validate = 0;

    auto validate = [&](std::string_view snap, const std::unordered_set<Price>& touched) {
        ++cmp.snapshots;
        compare_side(book.bids, snap, "bids", Side::Buy, touched, cmp, 1000);
        compare_side(book.asks, snap, "asks", Side::Sell, touched, cmp, 1000);
    };

    for (const Line& l : lines) {
        if (l.kind == 'S') {
            if (!dec.synced()) {
                dec.on_snapshot(l.payload);
                continue;
            }
            const auto t0 = Clock::now();
            const std::uint64_t L = json::get_uint(l.payload, "lastUpdateId");
            if (dec.last_update_id() >= L) {             // our book is already at or past L
                std::unordered_set<Price> touched;
                for (const Applied& a : recent)
                    if (a.u > L) {
                        collect_prices(a.msg, "b", touched);
                        collect_prices(a.msg, "a", touched);
                    }
                validate(l.payload, touched);
            } else {
                pending = l.payload;
                pending_L = L;
            }
            ns_validate += std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
            continue;
        }
        if (!pending.empty()) {
            const auto t0 = Clock::now();
            const binance::Range r = binance::range_of(l.payload);
            if (r.last > pending_L) {                    // this message reaches past L: compare first
                std::unordered_set<Price> touched;
                if (dec.last_update_id() != pending_L) {
                    collect_prices(l.payload, "b", touched);
                    collect_prices(l.payload, "a", touched);
                }
                validate(pending, touched);
                pending = {};
            }
            ns_validate += std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
        }
        const auto t0 = Clock::now();
        dec.on_update(l.payload);
        ns_diff += std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
        ++diffs;
        recent.push_back({l.payload, binance::range_of(l.payload).last});
        if (recent.size() > 64) recent.erase(recent.begin());
    }

    const auto& s = dec.stats();
    std::printf("binance: %llu diff messages, %llu applied, %llu stale, %llu gaps, %llu level updates\n",
                (unsigned long long)s.updates, (unsigned long long)s.applied, (unsigned long long)s.stale,
                (unsigned long long)s.gaps, (unsigned long long)s.levels);
    std::printf("  decode+update: %.0f ns/message, %.1f ns/level\n", ns_diff / diffs, ns_diff / s.levels);
    std::printf("  book depth: %zu bids, %zu asks; best %.2f / %.2f\n", book.bids.depth(), book.asks.depth(),
                book.bids.empty() ? 0.0 : book.bids.best().price / 1e8,
                book.asks.empty() ? 0.0 : book.asks.best().price / 1e8);
    std::printf("  validation: %llu snapshots, %llu levels compared, %llu mismatches, %llu excluded (in-flight)\n",
                (unsigned long long)cmp.snapshots, (unsigned long long)cmp.levels,
                (unsigned long long)cmp.mismatches, (unsigned long long)cmp.excluded);
    return cmp.mismatches == 0 && s.gaps == 0 ? 0 : 1;
}

template <class Book>
int run_coinbase(const std::vector<Line>& lines, Book& book) {
    coinbase::Level2Decoder<Book> dec(book, 0);
    std::uint64_t msgs = 0, crossed = 0;
    double ns = 0;
    for (const Line& l : lines) {
        if (l.kind != 'D') continue;
        const auto t0 = Clock::now();
        dec.on_message(l.payload);
        ns += std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
        ++msgs;
        if (book.crossed()) ++crossed;
    }
    const auto& s = dec.stats();
    std::printf("coinbase: %llu messages (%llu book), %llu snapshots, %llu gaps, %llu level updates, %llu crossed states\n",
                (unsigned long long)s.messages, (unsigned long long)s.book_messages, (unsigned long long)s.snapshots,
                (unsigned long long)s.gaps, (unsigned long long)s.levels, (unsigned long long)crossed);
    std::printf("  decode+update: %.0f ns/message, %.1f ns/level (incl. the initial snapshot)\n", ns / msgs,
                ns / s.levels);
    std::printf("  book depth: %zu bids, %zu asks; best %.2f / %.2f\n", book.bids.depth(), book.asks.depth(),
                book.bids.empty() ? 0.0 : book.bids.best().price / 1e8,
                book.asks.empty() ? 0.0 : book.asks.best().price / 1e8);
    return s.gaps == 0 && crossed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: replay_crypto binance|coinbase <file.tsv>\n");
        return 2;
    }
    std::string storage;
    const std::vector<Line> lines = load(argv[2], storage);
    const std::string venue = argv[1];
    // "null": decode only, events discarded -- isolates parsing cost from book cost.
    if (argc > 3 && std::string(argv[3]) == "null") {
        struct Null {
            std::uint64_t n = 0;
            void on_event(const BookEvent&) { ++n; }
        } sink;
        double ns = 0;
        std::uint64_t msgs = 0;
        auto timed = [&](auto&& f) {
            const auto t0 = Clock::now();
            f();
            ns += std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
            ++msgs;
        };
        if (venue == "binance") {
            binance::DepthDecoder<Null> dec(sink, 0);
            for (const Line& l : lines) {
                if (l.kind == 'S') {
                    dec.on_snapshot(l.payload);
                } else {
                    timed([&] { dec.on_update(l.payload); });
                }
            }
        } else {
            coinbase::Level2Decoder<Null> dec(sink, 0);
            for (const Line& l : lines)
                if (l.kind == 'D') timed([&] { dec.on_message(l.payload); });
        }
        std::printf("[decode only] %s: %llu events, %.0f ns/message, %.1f ns/event\n", venue.c_str(),
                    (unsigned long long)sink.n, ns / msgs, ns / sink.n);
        return 0;
    }
    // Book layout: "tick" (default, tick-grid array) or "vector" (sorted vector, for comparison).
    const bool use_vector = argc > 3 && std::string(argv[3]) == "vector";
    const Price tick = kScale / 100;   // 0.01 for BTC/ETH on both venues
    std::printf("[%s book]\n", use_vector ? "sorted-vector" : "tick-grid");
    if (use_vector) {
        auto book = std::make_unique<L2Book>();
        if (venue == "binance") return run_binance(lines, *book);
        if (venue == "coinbase") return run_coinbase(lines, *book);
    } else {
        auto book = std::make_unique<L2TickBook>(tick);
        if (venue == "binance") return run_binance(lines, *book);
        if (venue == "coinbase") return run_coinbase(lines, *book);
    }
    std::fprintf(stderr, "unknown venue %s\n", argv[1]);
    return 2;
}
