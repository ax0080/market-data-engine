// UDP receive benchmark (Linux): recvfrom vs recvmmsg vs AF_XDP, end to end through
// MoldUDP64 -> ITCH 5.0 decoder -> L3 book.
//
// The first N messages of a raw ITCH file are packed into MoldUDP64 packets
// (<= 1400 bytes) and sent at a target packet rate (0 = as fast as possible).
//
//   udp_bench local <itch file> <messages> <rx> <pps> [--transport] [--unicast]
//       sender thread + receiver in one process over loopback
//       (multicast 239.255.7.7 by default, or 127.0.0.1 with --unicast;
//        rx=xdp needs --unicast because generic XDP sees loopback unicast)
//   udp_bench send <itch file> <messages> <dest ip> <pps>
//   udp_bench recv <rx> [--ifname eth0] [--zc] [--native] [--transport] [--expect <messages>]
//       for two hosts (e.g. two cloud VMs); unicast, port 31007
//
// rx: recvfrom (1 packet/syscall) | recvmmsg (up to 64/syscall)
//     | xdp (AF_XDP: polls a ring in shared memory, no syscall per packet;
//            --zc = zero-copy into UMEM, --native = driver-mode XDP; both need NIC support)
// --transport: MoldUDP64 sequencing only, no ITCH decode (isolates receive cost).

#if !defined(__linux__)
#include <cstdio>
int main() {
    std::puts("udp_bench needs Linux (recvmmsg/sendmmsg/AF_XDP)");
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

#include "mde/af_xdp.h"
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

int udp_recv_socket(bool multicast) {
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
    if (multicast) {
        ip_mreq m{};
        inet_pton(AF_INET, kGroup, &m.imr_multiaddr);
        inet_pton(AF_INET, "127.0.0.1", &m.imr_interface);
        if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) != 0) {
            std::perror("IP_ADD_MEMBERSHIP");
            std::exit(2);
        }
    }
    timeval tv{0, 300'000};   // 300 ms idle -> sender is done
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return s;
}

void send_all(const Packets& P, const char* dst_ip, bool multicast, double pps) {
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (multicast) {
        in_addr ifc{};
        inet_pton(AF_INET, "127.0.0.1", &ifc);
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifc, sizeof ifc);
        unsigned char loop = 1;
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    }
    int sndbuf = 64 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(kPort);
    inet_pton(AF_INET, dst_ip, &dst.sin_addr);
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

struct RxConfig {
    std::string rx = "recvmmsg";
    bool transport_only = false;
    bool multicast = false;
    std::string ifname = "lo";
    bool zero_copy = false;
    bool native = false;
    double idle_stop_s = 0.3;
};

struct RxResult {
    mold::Stats mold;
    std::uint64_t syscalls = 0;
    double secs = 0;
};

// Receives until the stream goes idle, feeding every packet to MoldUDP64 (and,
// unless transport-only, the ITCH decoder and L3 book). ready is set once the
// receiver can accept packets.
RxResult receive(const RxConfig& c, std::atomic<bool>& ready) {
    auto book = std::make_unique<L3Book>(std::size_t{1} << 22);
    auto itch_dec = std::make_unique<itch::Decoder<L3Book>>(*book);
    mold::Decoder<itch::Decoder<L3Book>> mold_dec(*itch_dec);
    struct NullHandler {
        void decode(const std::uint8_t*, std::size_t) {}
    } null_handler;
    mold::Decoder<NullHandler> null_mold(null_handler);
    auto on_packet = [&](const std::uint8_t* p, std::size_t n) {
        if (c.transport_only) {
            null_mold.on_packet(p, n);
        } else {
            mold_dec.on_packet(p, n);
        }
    };

    RxResult r;
    Clock::time_point first{}, last{};
    bool started = false;
    auto mark = [&] {
        const auto now = Clock::now();
        if (!started) {
            first = now;
            started = true;
        }
        last = now;
    };

    if (c.rx == "xdp") {
        xdp::Options o;
        o.ifname = c.ifname;
        o.udp_port = kPort;
        o.zero_copy = c.zero_copy;
        o.native = c.native;
        xdp::Socket xsk(o);
        // keep a plain UDP socket bound so packets that bypass XDP are absorbed, not answered with ICMP
        const int absorb = udp_recv_socket(false);
        ready = true;
        auto idle_since = Clock::now();
        for (;;) {
            const std::uint32_t n = xsk.poll_once(on_packet);
            if (n) {
                mark();
                idle_since = last;
            } else if (std::chrono::duration<double>(Clock::now() - idle_since).count() > (started ? c.idle_stop_s : 30.0)) {
                break;
            }
        }
        close(absorb);
    } else {
        const bool batch = c.rx == "recvmmsg";
        const int s = udp_recv_socket(c.multicast);
        std::vector<std::uint8_t> bufs(kBatch * 2048);
        mmsghdr msgs[kBatch];
        iovec iov[kBatch];
        for (unsigned j = 0; j < kBatch; ++j) {
            iov[j] = {bufs.data() + j * 2048, 2048};
            msgs[j] = {};
            msgs[j].msg_hdr.msg_iov = &iov[j];
            msgs[j].msg_hdr.msg_iovlen = 1;
        }
        ready = true;
        int idle_timeouts = 0;
        for (;;) {
            int got;
            if (batch) {
                got = recvmmsg(s, msgs, kBatch, MSG_WAITFORONE, nullptr);
            } else {
                const ssize_t n = recvfrom(s, bufs.data(), 2048, 0, nullptr, nullptr);
                got = n < 0 ? -1 : 1;
                if (n >= 0) msgs[0].msg_len = static_cast<unsigned>(n);
            }
            ++r.syscalls;
            if (got <= 0) {
                if (started) break;                 // went idle after traffic: done
                if (++idle_timeouts > 100) break;   // nothing for 30 s
                continue;
            }
            mark();
            for (int j = 0; j < got; ++j) on_packet(bufs.data() + j * 2048, msgs[j].msg_len);
        }
        close(s);
    }
    r.mold = c.transport_only ? null_mold.stats() : mold_dec.stats();
    r.secs = std::chrono::duration<double>(last - first).count();
    return r;
}

void report(const RxConfig& c, double pps, std::uint64_t expected_msgs, const RxResult& r) {
    const auto& m = r.mold;
    const double lost_pct = expected_msgs ? 100.0 * double(expected_msgs - m.messages) / double(expected_msgs) : 0.0;
    std::string rx = c.rx;
    if (c.rx == "xdp") rx += std::string(c.zero_copy ? "-zc" : "-copy") + (c.native ? "-drv" : "-skb");
    std::printf("%-9s %-14s target %8.0f pkt/s | received %llu pkts, %llu msgs, %llu gaps | lost %.3f%% | "
                "%.2f M msgs/s, %.0f k pkts/s | %s\n",
                c.transport_only ? "transport" : "full", rx.c_str(), pps, (unsigned long long)m.packets,
                (unsigned long long)m.messages, (unsigned long long)m.gaps, lost_pct, m.messages / r.secs / 1e6,
                m.packets / r.secs / 1e3,
                c.rx == "xdp" ? "0 syscalls/packet (ring polling)"
                              : (std::to_string(double(m.packets) / double(r.syscalls)).substr(0, 5) + " pkts/syscall").c_str());
}

void parse_flags(int argc, char** argv, int from, RxConfig& c, std::uint64_t* expect) {
    for (int i = from; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--transport") c.transport_only = true;
        else if (a == "--zc") c.zero_copy = true;
        else if (a == "--native") c.native = true;
        else if (a == "--ifname" && i + 1 < argc) c.ifname = argv[++i];
        else if (a == "--expect" && i + 1 < argc && expect) *expect = std::strtoull(argv[++i], nullptr, 10);
    }
}

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  udp_bench local <itch file> <messages> recvfrom|recvmmsg|xdp <pps> [--transport] [--unicast]\n"
                 "  udp_bench send  <itch file> <messages> <dest ip> <pps>\n"
                 "  udp_bench recv  recvfrom|recvmmsg|xdp [--ifname eth0] [--zc] [--native] [--transport] [--expect N]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string role = argv[1];
    try {
        if (role == "local") {
            if (argc < 6) return usage();
            const Packets P = build(argv[2], std::strtoull(argv[3], nullptr, 10));
            RxConfig c;
            c.rx = argv[4];
            const double pps = std::atof(argv[5]);
            parse_flags(argc, argv, 6, c, nullptr);
            bool unicast = false;
            for (int i = 6; i < argc; ++i) unicast |= std::string(argv[i]) == "--unicast";
            c.multicast = !unicast;
            if (c.rx == "xdp" && c.multicast) {
                std::fprintf(stderr, "rx=xdp on loopback needs --unicast\n");
                return 2;
            }
            std::atomic<bool> ready{false};
            std::thread tx([&] {
                while (!ready.load()) std::this_thread::yield();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                send_all(P, unicast ? "127.0.0.1" : kGroup, !unicast, pps);
            });
            const RxResult r = receive(c, ready);
            tx.join();
            report(c, pps, P.messages, r);
        } else if (role == "send") {
            if (argc < 6) return usage();
            const Packets P = build(argv[2], std::strtoull(argv[3], nullptr, 10));
            const auto t0 = Clock::now();
            send_all(P, argv[4], false, std::atof(argv[5]));
            const double s = std::chrono::duration<double>(Clock::now() - t0).count();
            std::printf("sent %zu packets, %llu messages in %.2f s (%.0f k pkts/s)\n", P.offset.size(),
                        (unsigned long long)P.messages, s, P.offset.size() / s / 1e3);
        } else if (role == "recv") {
            RxConfig c;
            c.rx = argv[2];
            c.ifname = "eth0";
            c.idle_stop_s = 1.0;
            std::uint64_t expect = 0;
            parse_flags(argc, argv, 3, c, &expect);
            std::atomic<bool> ready{false};
            std::fprintf(stderr, "receiving on port %u (%s)...\n", kPort, c.rx.c_str());
            const RxResult r = receive(c, ready);
            report(c, 0, expect, r);
        } else {
            return usage();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
#endif
