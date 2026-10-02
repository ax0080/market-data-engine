// Replay a raw (decompressed) NASDAQ TotalView-ITCH 5.0 file through the decoder
// and the L3 book, timing only decode + book update (file reads are excluded).
//
// usage: replay_itch <file>        (decompress the sample first: gzip -dc X.gz > X)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "mde/itch50.h"
#include "mde/l3_book.h"

using namespace mde;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: replay_itch <raw ITCH 5.0 file>\n");
        return 2;
    }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::perror(argv[1]);
        return 2;
    }
    auto book = std::make_unique<L3Book>(std::size_t{1} << 24);
    auto dec = std::make_unique<itch::Decoder<L3Book>>(*book);

    constexpr std::size_t kChunk = std::size_t{256} << 20;
    std::vector<std::uint8_t> buf(kChunk + 65536);
    std::size_t carry = 0;
    double ns = 0;
    std::uint64_t bytes = 0;
    std::size_t peak_orders = 0;
    for (;;) {
        const std::size_t got = std::fread(buf.data() + carry, 1, kChunk, f);
        if (got == 0) break;
        bytes += got;
        const std::size_t n = carry + got;
        const auto t0 = Clock::now();
        const std::size_t used = dec->decode_stream(buf.data(), n);
        ns += std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
        peak_orders = std::max(peak_orders, book->live_orders());
        carry = n - used;
        std::copy(buf.begin() + static_cast<std::ptrdiff_t>(used), buf.begin() + static_cast<std::ptrdiff_t>(n), buf.begin());
        std::fprintf(stderr, "\r%.1f GB", bytes / 1e9);
    }
    std::fclose(f);
    std::fprintf(stderr, "\n");

    const auto& s = dec->stats();
    std::printf("ITCH 5.0: %.2f GB, %llu messages, %llu book events, %llu truncated\n", bytes / 1e9,
                (unsigned long long)s.messages, (unsigned long long)s.book_events, (unsigned long long)s.truncated);
    std::printf("  decode + L3 book: %.2f s, %.1f M messages/s, %.1f ns/message\n", ns / 1e9, s.messages / ns * 1e3,
                ns / s.messages);
    std::printf("  orders live at end: %zu, peak sampled: %zu, unknown-order events: %llu, duplicate adds: %llu\n",
                book->live_orders(), peak_orders, (unsigned long long)book->stats().unknown_order,
                (unsigned long long)book->stats().duplicate_add);
    std::printf("  message mix:");
    for (char c : std::string("AFECXDUPRSQ"))
        std::printf(" %c=%llu", c, (unsigned long long)s.by_type[static_cast<unsigned char>(c)]);
    std::printf("\n");
    // Top of book for a few well-known names at end of day.
    for (std::uint32_t loc = 0; loc < 65536; ++loc) {
        const std::string& name = dec->symbol_name(static_cast<std::uint16_t>(loc));
        if (name.rfind("AAPL ", 0) != 0 && name.rfind("MSFT ", 0) != 0 && name.rfind("NVDA ", 0) != 0) continue;
        const L3Book::Instrument* in = book->instrument(static_cast<std::uint16_t>(loc));
        if (!in) continue;
        std::printf("  %s bid levels %zu, ask levels %zu\n", name.c_str(), in->bids.depth(), in->asks.depth());
    }
    return 0;
}
