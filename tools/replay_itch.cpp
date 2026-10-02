// Replay a raw (decompressed) NASDAQ TotalView-ITCH 5.0 file through the decoder
// and the L3 book.
//
// Input is memory-mapped by default on Linux (zero-copy: the decoder reads
// page-cache pages in place, and the file is one contiguous range, so no message
// straddles a buffer edge) and read in chunks by default on Windows, where that
// measured faster. --mmap / --fread override.
//
// Two times are reported: "decode + book" (the decoder/book work only) and
// "end to end" (wall clock including I/O and page faults).
//
// usage: replay_itch <file> [--mmap|--fread] [--decode-only] [--cap <log2 capacity>]
//        (decompress the NASDAQ sample first: gzip -dc X.gz > X)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mde/itch50.h"
#include "mde/l3_book.h"
#include "mde/mapped_file.h"

using namespace mde;
using Clock = std::chrono::steady_clock;

namespace {

struct Null {
    std::uint64_t n = 0;
    void on_event(const BookEvent&) { ++n; }
};

constexpr std::size_t kChunk = std::size_t{256} << 20;

// Feeds the whole file to dec in kChunk slices; returns decode time in ns.
template <class Dec>
double feed_mmap(const std::string& path, Dec& dec, std::uint64_t& bytes) {
    const MappedFile m(path);
    bytes = m.size();
    double ns = 0;
    std::size_t off = 0;
    m.prefetch(0, kChunk);
    while (off < m.size()) {
        const std::size_t n = std::min(kChunk, m.size() - off);
        m.prefetch(off + kChunk, kChunk);   // the next slice loads while this one decodes
        const auto t0 = Clock::now();
        std::size_t used = dec.decode_stream(m.data() + off, n);
        if (used == 0 && n < m.size() - off) used = dec.decode_stream(m.data() + off, m.size() - off);   // message longer than a slice
        ns += std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
        if (used == 0) break;   // trailing partial message
        off += used;
    }
    return ns;
}

template <class Dec>
double feed_fread(const std::string& path, Dec& dec, std::uint64_t& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::perror(path.c_str());
        std::exit(2);
    }
    std::vector<std::uint8_t> buf(kChunk + 65536);
    std::size_t carry = 0;
    double ns = 0;
    bytes = 0;
    for (;;) {
        const std::size_t got = std::fread(buf.data() + carry, 1, kChunk, f);
        if (got == 0) break;
        bytes += got;
        const std::size_t n = carry + got;
        const auto t0 = Clock::now();
        const std::size_t used = dec.decode_stream(buf.data(), n);
        ns += std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
        carry = n - used;
        std::memmove(buf.data(), buf.data() + used, carry);   // the copy mmap avoids
    }
    std::fclose(f);
    return ns;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: replay_itch <file> [--fread] [--decode-only] [--cap <log2>]\n");
        return 2;
    }
    const std::string path = argv[1];
    // Default I/O per platform, from measurements on the full ITCH day: mmap removes
    // almost all I/O cost on Linux, but on Windows every 4 KB page soft-faults and
    // chunked reads are faster. --mmap / --fread override.
#if defined(_WIN32)
    bool use_fread = true;
#else
    bool use_fread = false;
#endif
    bool decode_only = false;
    // Default 2^24: a full day peaks near 1.7M live orders; lower load means shorter
    // probe runs, which measured faster than a smaller, more cache-resident table.
    int cap_log2 = 24;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fread") use_fread = true;
        else if (a == "--mmap") use_fread = false;
        else if (a == "--decode-only") decode_only = true;
        else if (a == "--cap" && i + 1 < argc) cap_log2 = std::atoi(argv[++i]);
    }
    const char* io = use_fread ? "fread" : "mmap";
    std::uint64_t bytes = 0;
    const auto w0 = Clock::now();

    if (decode_only) {
        Null sink;
        auto d = std::make_unique<itch::Decoder<Null>>(sink);
        const double ns = use_fread ? feed_fread(path, *d, bytes) : feed_mmap(path, *d, bytes);
        const double wall = std::chrono::duration<double>(Clock::now() - w0).count();
        const auto msgs = d->stats().messages;
        std::printf("[%s] decode only: %llu messages, %.1f ns/message (%.1f M/s); end to end %.2f s (%.1f M/s)\n", io,
                    (unsigned long long)msgs, ns / msgs, msgs / ns * 1e3, wall, msgs / wall / 1e6);
        return 0;
    }

    auto book = std::make_unique<L3Book>(std::size_t{1} << cap_log2);
    auto dec = std::make_unique<itch::Decoder<L3Book>>(*book);
    const double ns = use_fread ? feed_fread(path, *dec, bytes) : feed_mmap(path, *dec, bytes);
    const double wall = std::chrono::duration<double>(Clock::now() - w0).count();

    const auto& s = dec->stats();
    std::printf("[%s] ITCH 5.0: %.2f GB, %llu messages, %llu book events, %llu truncated\n", io, bytes / 1e9,
                (unsigned long long)s.messages, (unsigned long long)s.book_events, (unsigned long long)s.truncated);
    std::printf("  decode + L3 book: %.2f s, %.1f M messages/s, %.1f ns/message\n", ns / 1e9, s.messages / ns * 1e3,
                ns / s.messages);
    std::printf("  end to end (incl. I/O): %.2f s, %.1f M messages/s\n", wall, s.messages / wall / 1e6);
    std::printf("  orders live at end: %zu, unknown-order events: %llu, duplicate adds: %llu\n", book->live_orders(),
                (unsigned long long)book->stats().unknown_order, (unsigned long long)book->stats().duplicate_add);
    std::printf("  message mix:");
    for (char c : std::string("AFECXDUPRSQ"))
        std::printf(" %c=%llu", c, (unsigned long long)s.by_type[static_cast<unsigned char>(c)]);
    std::printf("\n");
    return 0;
}
