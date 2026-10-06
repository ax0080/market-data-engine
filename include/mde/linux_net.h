#pragma once
// Small Linux socket and timing helpers shared by the exchange simulator and
// the trader. Everything is non-blocking and polled from one busy loop.

#if defined(__linux__)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "mde/fix44.h"
#include "mde/soupbintcp.h"

namespace mde::net {

inline std::uint64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::uint64_t(ts.tv_sec) * 1'000'000'000u + std::uint64_t(ts.tv_nsec);
}
inline std::uint64_t epoch_ns() {   // FIX SendingTime / TransactTime
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return std::uint64_t(ts.tv_sec) * 1'000'000'000u + std::uint64_t(ts.tv_nsec);
}
inline std::uint64_t ns_since_midnight() {   // ITCH / OUCH timestamps (UTC midnight)
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (std::uint64_t(ts.tv_sec) % 86'400) * 1'000'000'000u + std::uint64_t(ts.tv_nsec);
}

[[noreturn]] inline void fail(const std::string& what) { throw std::runtime_error(what + ": " + std::strerror(errno)); }

inline void pin_cpu(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) fail("sched_setaffinity");
}

inline void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

inline void tune_tcp(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   // no Nagle delay on small messages
    set_nonblocking(fd);
}

inline sockaddr_in addr(const std::string& ip, std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) throw std::runtime_error("bad address " + ip);
    return a;
}

inline int tcp_listen(std::uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a = addr("0.0.0.0", port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) fail("bind tcp " + std::to_string(port));
    if (listen(fd, 16) != 0) fail("listen");
    set_nonblocking(fd);
    return fd;
}

// Blocking connect (retried until timeout_s), then switched to non-blocking.
inline int tcp_connect(const std::string& ip, std::uint16_t port, double timeout_s = 10) {
    const sockaddr_in a = addr(ip, port);
    const std::uint64_t deadline = mono_ns() + std::uint64_t(timeout_s * 1e9);
    for (;;) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) {
            tune_tcp(fd);
            return fd;
        }
        close(fd);
        if (mono_ns() > deadline) fail("connect " + ip + ":" + std::to_string(port));
        usleep(50'000);
    }
}

inline int udp_socket() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int buf = 64 << 20;   // capped by net.core.{r,w}mem_max
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    return fd;
}

// UDP receiver on port; joins `group` if it is a multicast address.
inline int udp_receiver(std::uint16_t port, const std::string& group = "") {
    const int fd = udp_socket();
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a = addr("0.0.0.0", port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) fail("bind udp " + std::to_string(port));
    if (!group.empty()) {
        ip_mreq m{};
        inet_pton(AF_INET, group.c_str(), &m.imr_multiaddr);
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) != 0) fail("IP_ADD_MEMBERSHIP");
    }
    set_nonblocking(fd);
    return fd;
}

// One TCP connection: reassembles inbound messages with Framer (SoupBinTCP or
// FIX), and queues outbound bytes only when the socket would block (the common
// case is one send()).
template <class Framer>
class BasicConn {
public:
    explicit BasicConn(int fd) : fd_(fd) {}
    BasicConn(BasicConn&& o) noexcept : fd_(o.fd_), in_(std::move(o.in_)), out_(std::move(o.out_)) { o.fd_ = -1; }
    BasicConn& operator=(BasicConn&& o) noexcept {
        std::swap(fd_, o.fd_);
        std::swap(in_, o.in_);
        std::swap(out_, o.out_);
        return *this;
    }
    BasicConn(const BasicConn&) = delete;
    ~BasicConn() { close(); }

    int fd() const { return fd_; }
    bool open() const { return fd_ >= 0; }
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    // Reads what is available. Returns false if the peer closed or errored.
    bool read() {
        if (fd_ < 0) return false;
        for (;;) {
            if (in_.space() == 0) return true;   // caller must drain first
            const ssize_t n = ::recv(fd_, in_.tail(), in_.space(), MSG_DONTWAIT);
            if (n > 0) {
                in_.commit(static_cast<std::size_t>(n));
                last_rx_ns = mono_ns();
                continue;
            }
            if (n == 0) return false;
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        }
    }
    Framer& in() { return in_; }

    // Sends now if nothing is queued, else queues. Returns false on a dead socket.
    bool send(const std::uint8_t* p, std::size_t n) {
        if (fd_ < 0) return false;
        last_tx_ns = mono_ns();
        if (out_.empty()) {
            const ssize_t w = ::send(fd_, p, n, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (w == static_cast<ssize_t>(n)) return true;
            if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
            const std::size_t done = w > 0 ? static_cast<std::size_t>(w) : 0;
            enqueue(p + done, n - done);
            return true;
        }
        enqueue(p, n);
        return flush();
    }
    bool flush() {
        while (!out_.empty() && fd_ >= 0) {
            const ssize_t w = ::send(fd_, out_.data(), out_.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (w > 0) {
                out_.erase(out_.begin(), out_.begin() + w);
                continue;
            }
            return w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        }
        return fd_ >= 0;
    }
    std::size_t queued() const { return out_.size(); }

    std::uint64_t last_rx_ns = 0, last_tx_ns = 0;

private:
    void enqueue(const std::uint8_t* p, std::size_t n) {
        const std::size_t old = out_.size();
        out_.resize(old + n);
        std::memcpy(out_.data() + old, p, n);
    }

    int fd_ = -1;
    Framer in_;
    std::vector<std::uint8_t> out_;
};
using Conn = BasicConn<soup::Framer>;
using FixConn = BasicConn<fix::Framer>;

// Latency samples -> percentiles.
struct Samples {
    std::vector<std::uint64_t> v;
    void add(std::uint64_t ns) { v.push_back(ns); }
    void print(const char* name) {
        if (v.empty()) {
            std::printf("  %-34s no samples\n", name);
            return;
        }
        std::sort(v.begin(), v.end());
        auto q = [&](double p) { return double(v[std::min(v.size() - 1, std::size_t(p * double(v.size())))]) / 1000.0; };
        std::printf("  %-34s n=%-7zu p50 %8.2f  p90 %8.2f  p99 %8.2f  p99.9 %8.2f  max %8.2f us\n", name, v.size(), q(0.5),
                    q(0.9), q(0.99), q(0.999), double(v.back()) / 1000.0);
    }
};

}  // namespace mde::net

#endif
