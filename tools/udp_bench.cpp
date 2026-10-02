// UDP multicast receive benchmark (Linux): recvfrom (one packet per syscall)
// versus recvmmsg (up to 64 packets per syscall), end to end through
// MoldUDP64 -> ITCH 5.0 decoder -> L3 book.
//
// A sender thread packs the first N messages of a raw ITCH file into MoldUDP64
// packets (<= 1400 bytes) and multicasts them on the loopback interface at a
// target packet rate (0 = as fast as possible). The receiver reports delivered
// messages, lost messages (MoldUDP64 sequence gaps), throughput and receive-side
// CPU per packet.
//
// usage: udp_bench <raw itch file> <messages> recvfrom|recvmmsg <packets/s, 0=flood>

#if !defined(__linux__)
#include <cstdio>
int main() {
    std::puts("udp_bench needs Linux (recvmmsg/sendmmsg)");
    return 0;
}
#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mde/itch50.h"
#include "mde/l3_book.h"
#include "mde/moldudp64.h"

using namespace mde;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kGroup = "239.255.7.7";
constexpr std::uint16_t kPort = 31007;
constexpr std::size_t kMaxPacket = 1400;
constexpr unsigned kBatch = 64;

struct Packets {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint32_t> offset, size;
    std::uint64_t messages = 0;
};

// Pack the first n messages of a raw ITCH file into MoldUDP64 packets.
Packets build(const char* path, std::uint64_t n) {
    Packets out;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::perror(path);
        std::exit(2);
    }
    std::vector<std::uint8_t> pkt;
    std::uint64_t seq = 1;
    std::uint16_t count = 0;
    auto start_packet = [&] {
        pkt.assign(mold::kHeader, 0);
        std::memcpy(pkt.data(), "SESSION001", 10);
        count = 0;
    };
    auto flush = [&] {
        if (count == 0) return;
        for (int i = 0; i < 8; ++i) pkt[10 + i] = static_cast<std::uint8_t>(seq >> (8 * (7 - i)));
        pkt[18] = static_cast<std::uint8_t>(count >> 8);
        pkt[19] = static_cast<std::uint8_t>(count);
        out.offset.push_back(static_cast<std::uint32_t>(out.bytes.size()));
        out.size.push_back(static_cast<std::uint32_t>(pkt.size()));
        out.bytes.insert(out.bytes.end(), pkt.begin(), pkt.end());
        seq += count;
        start_packet();
    };
    start_packet();
    std::uint8_t lenbuf[2];
    std::uint8_t msg[256];
    while (out.messages < n && std::fread(lenbuf, 1, 2, f) == 2) {
        const std::size_t len = (std::size_t{lenbuf[0]} << 8) | lenbuf[1];
        if (len > sizeof msg || std::fread(msg, 1, len, f) != len) break;
        if (pkt.size() + 2 + len > kMaxPacket) flush();
        pkt.push_back(lenbuf[0]);
        pkt.push_back(lenbuf[1]);
        pkt.insert(pkt.end(), msg, msg + len);
        ++count;
        ++out.messages;
    }
    flush();
    std::fclose(f);
    return out;
}

int recv_socket() {
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int rcvbuf = 64 << 20;   // capped by net.core.rmem_max
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        std::perror("bind");
        std::exit(2);
    }
    ip_mreq m{};
    inet_pton(AF_INET, kGroup, &m.imr_multiaddr);
    inet_pton(AF_INET, "127.0.0.1", &m.imr_interface);
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) != 0) {
        std::perror("IP_ADD_MEMBERSHIP");
        std::exit(2);
    }
    timeval tv{0, 300'000};   // 300 ms idle -> sender is done
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return s;
}

int send_socket() {
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    in_addr ifc{};
    inet_pton(AF_INET, "127.0.0.1", &ifc);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifc, sizeof ifc);
    unsigned char loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    int sndbuf = 64 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    return s;
}

void sender(const Packets& P, double pps, std::atomic<bool>& ready) {
    const int s = send_socket();
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(kPort);
    inet_pton(AF_INET, kGroup, &dst.sin_addr);
    while (!ready.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mmsghdr msgs[kBatch];
    iovec iov[kBatch];
    const std::size_t total = P.offset.size();
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < total; i += kBatch) {
        const unsigned k = static_cast<unsigned>(std::min<std::size_t>(kBatch, total - i));
        for (unsigned j = 0; j < k; ++j) {
            iov[j] = {const_cast<std::uint8_t*>(P.bytes.data() + P.offset[i + j]), P.size[i + j]};
            msgs[j] = {};
            msgs[j].msg_hdr.msg_name = &dst;
            msgs[j].msg_hdr.msg_namelen = sizeof dst;
            msgs[j].msg_hdr.msg_iov = &iov[j];
            msgs[j].msg_hdr.msg_iovlen = 1;
        }
        unsigned sent = 0;
        while (sent < k) {
            const int r = sendmmsg(s, msgs + sent, k - sent, 0);
            if (r > 0) sent += static_cast<unsigned>(r);
        }
        if (pps > 0) {   // pace: packet i+k is due at (i+k)/pps seconds
            const auto due = t0 + std::chrono::duration<double>((i + k) / pps);
            while (Clock::now() < due) {
            }
        }
    }
    close(s);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: udp_bench <raw itch file> <messages> recvfrom|recvmmsg <packets/s, 0=flood>\n");
        return 2;
    }
    const Packets P = build(argv[1], std::strtoull(argv[2], nullptr, 10));
    const bool batch = std::string(argv[3]) == "recvmmsg";
    const double pps = std::atof(argv[4]);
    // "transport" = receive and sequence-check only (MoldUDP64 header), no ITCH decode:
    // isolates the cost of getting packets out of the kernel.
    const bool transport_only = argc > 5 && std::string(argv[5]) == "transport";

    auto book = std::make_unique<L3Book>(std::size_t{1} << 22);
    auto itch_dec = std::make_unique<itch::Decoder<L3Book>>(*book);
    mold::Decoder<itch::Decoder<L3Book>> mold_dec(*itch_dec);
    struct NullHandler {
        void decode(const std::uint8_t*, std::size_t) {}
    } null_handler;
    mold::Decoder<NullHandler> null_mold(null_handler);

    const int s = recv_socket();
    std::atomic<bool> ready{false};
    std::thread tx(sender, std::cref(P), pps, std::ref(ready));

    std::vector<std::uint8_t> bufs(kBatch * 2048);
    mmsghdr msgs[kBatch];
    iovec iov[kBatch];
    for (unsigned j = 0; j < kBatch; ++j) {
        iov[j] = {bufs.data() + j * 2048, 2048};
        msgs[j] = {};
        msgs[j].msg_hdr.msg_iov = &iov[j];
        msgs[j].msg_hdr.msg_iovlen = 1;
    }
    std::uint64_t syscalls = 0;
    Clock::time_point first{}, last{};
    bool started = false;
    ready = true;
    for (;;) {
        int got;
        if (batch) {
            got = recvmmsg(s, msgs, kBatch, MSG_WAITFORONE, nullptr);
        } else {
            const ssize_t r = recvfrom(s, bufs.data(), 2048, 0, nullptr, nullptr);
            got = r < 0 ? -1 : 1;
            if (r >= 0) msgs[0].msg_len = static_cast<unsigned>(r);
        }
        ++syscalls;
        if (got <= 0) {
            if (started) break;   // timed out after traffic: done
            continue;
        }
        const auto now = Clock::now();
        if (!started) {
            first = now;
            started = true;
        }
        last = now;
        for (int j = 0; j < got; ++j) {
            const std::uint8_t* p = bufs.data() + j * 2048;
            if (transport_only) {
                null_mold.on_packet(p, msgs[j].msg_len);
            } else {
                mold_dec.on_packet(p, msgs[j].msg_len);
            }
        }
    }
    const mold::Stats& m = transport_only ? null_mold.stats() : mold_dec.stats();
    tx.join();
    close(s);

    const double secs = std::chrono::duration<double>(last - first).count();
    const std::uint64_t lost = P.messages - m.messages;
    std::printf("%-9s %-8s target %8.0f pkt/s | sent %zu pkts, %llu msgs | received %llu pkts, %llu msgs | "
                "lost %.3f%% | %.2f M msgs/s, %.0f k pkts/s | %.2f pkts/syscall\n",
                transport_only ? "transport" : "full", batch ? "recvmmsg" : "recvfrom", pps, P.offset.size(),
                (unsigned long long)P.messages,
                (unsigned long long)m.packets, (unsigned long long)m.messages, 100.0 * lost / P.messages,
                m.messages / secs / 1e6, m.packets / secs / 1e3, double(m.packets) / syscalls);
    return 0;
}
#endif
