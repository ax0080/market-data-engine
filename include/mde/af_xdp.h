#pragma once
// AF_XDP receive socket from raw Linux syscalls -- no libbpf, no libxdp, no clang.
//
// Path of a packet:  NIC -> XDP hook -> (our BPF program) -> XSKMAP -> AF_XDP RX ring
//
//  * A 23-instruction XDP program, assembled here as bytecode, redirects only
//    IPv4/UDP packets to the configured destination port into the AF_XDP socket;
//    every other packet continues to the normal network stack untouched.
//  * The program is attached with BPF_LINK_CREATE, so it detaches automatically
//    when the process exits (no stale XDP program left on the interface).
//  * Packets land in a UMEM region shared with the kernel. The receive loop polls
//    the RX ring in shared memory and hands frames back through the fill ring:
//    no syscall per packet or per batch.
//  * Zero-copy mode (XDP_ZEROCOPY, native driver mode) lets the NIC DMA straight
//    into UMEM; it needs driver support. Copy mode works on any interface
//    (including loopback via generic XDP) but the kernel copies each packet once.
//
// Requires root (or CAP_NET_ADMIN + CAP_BPF + CAP_NET_RAW).

#if defined(__linux__)

#include <linux/bpf.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef SOL_XDP
#define SOL_XDP 283
#endif
#ifndef AF_XDP
#define AF_XDP 44
#endif

namespace mde::xdp {

inline long bpf(int cmd, union bpf_attr* attr) { return ::syscall(__NR_bpf, cmd, attr, sizeof(*attr)); }

// ---------------------------------------------------------------- BPF assembly
inline bpf_insn ins(std::uint8_t code, std::uint8_t dst, std::uint8_t src, std::int16_t off, std::int32_t imm) {
    bpf_insn i{};
    i.code = code;
    i.dst_reg = dst & 0xf;
    i.src_reg = src & 0xf;
    i.off = off;
    i.imm = imm;
    return i;
}

// XDP program: if (IPv4 && IHL==5 && UDP && dport==port) redirect to xsks[rx_queue] else PASS.
inline std::vector<bpf_insn> udp_port_filter(int xskmap_fd, std::uint16_t port) {
    const std::int32_t port_le = static_cast<std::int32_t>(((port & 0xff) << 8) | (port >> 8));   // dport as loaded (little-endian host)
    constexpr int kPass = 21;   // index of the PASS block
    auto to_pass = [](int at) { return static_cast<std::int16_t>(kPass - (at + 1)); };
    std::vector<bpf_insn> p = {
        ins(BPF_ALU64 | BPF_MOV | BPF_X, 6, 1, 0, 0),          // 0  r6 = ctx
        ins(BPF_LDX | BPF_MEM | BPF_W, 2, 1, 0, 0),            // 1  r2 = ctx->data
        ins(BPF_LDX | BPF_MEM | BPF_W, 3, 1, 4, 0),            // 2  r3 = ctx->data_end
        ins(BPF_ALU64 | BPF_MOV | BPF_X, 4, 2, 0, 0),          // 3  r4 = data
        ins(BPF_ALU64 | BPF_ADD | BPF_K, 4, 0, 0, 42),         // 4  r4 += eth(14)+ip(20)+udp(8)
        ins(BPF_JMP | BPF_JGT | BPF_X, 4, 3, to_pass(5), 0),   // 5  if r4 > data_end goto PASS
        ins(BPF_LDX | BPF_MEM | BPF_H, 5, 2, 12, 0),           // 6  r5 = eth type
        ins(BPF_JMP | BPF_JNE | BPF_K, 5, 0, to_pass(7), 0x0008),   // 7  != htons(ETH_P_IP)
        ins(BPF_LDX | BPF_MEM | BPF_B, 5, 2, 14, 0),           // 8  r5 = version/IHL
        ins(BPF_ALU64 | BPF_AND | BPF_K, 5, 0, 0, 0x0f),       // 9  r5 &= 0x0f
        ins(BPF_JMP | BPF_JNE | BPF_K, 5, 0, to_pass(10), 5),  // 10 IHL != 5 (options) goto PASS
        ins(BPF_LDX | BPF_MEM | BPF_B, 5, 2, 23, 0),           // 11 r5 = ip proto
        ins(BPF_JMP | BPF_JNE | BPF_K, 5, 0, to_pass(12), 17), // 12 != UDP
        ins(BPF_LDX | BPF_MEM | BPF_H, 5, 2, 36, 0),           // 13 r5 = udp dest port
        ins(BPF_JMP | BPF_JNE | BPF_K, 5, 0, to_pass(14), port_le),   // 14 != port
        ins(BPF_LDX | BPF_MEM | BPF_W, 2, 6, 16, 0),           // 15 r2 = ctx->rx_queue_index
        ins(BPF_LD | BPF_DW | BPF_IMM, 1, BPF_PSEUDO_MAP_FD, 0, xskmap_fd),   // 16 r1 = &xsks (2 slots)
        ins(0, 0, 0, 0, 0),                                    // 17
        ins(BPF_ALU64 | BPF_MOV | BPF_K, 3, 0, 0, XDP_PASS),   // 18 r3 = fallback action
        ins(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_redirect_map),   // 19
        ins(BPF_JMP | BPF_EXIT, 0, 0, 0, 0),                   // 20
        ins(BPF_ALU64 | BPF_MOV | BPF_K, 0, 0, 0, XDP_PASS),   // 21 PASS: r0 = XDP_PASS
        ins(BPF_JMP | BPF_EXIT, 0, 0, 0, 0),                   // 22
    };
    return p;
}

// ---------------------------------------------------------------- rings
struct Ring {
    std::uint32_t* producer = nullptr;
    std::uint32_t* consumer = nullptr;
    std::uint32_t* flags = nullptr;
    void* desc = nullptr;
    std::uint32_t mask = 0;
    void* map = nullptr;
    std::size_t map_len = 0;
};

inline std::uint32_t load_acquire(const std::uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline void store_release(std::uint32_t* p, std::uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

struct Options {
    std::string ifname = "lo";
    std::uint32_t queue = 0;
    std::uint16_t udp_port = 31007;
    bool zero_copy = false;     // XDP_ZEROCOPY + native driver mode
    bool native = false;        // XDP_FLAGS_DRV_MODE (else SKB / generic mode)
    std::uint32_t frames = 4096;
    std::uint32_t frame_size = 2048;
    std::uint32_t ring_size = 2048;
};

class Socket {
public:
    explicit Socket(const Options& o) : o_(o) {
        ifindex_ = static_cast<int>(if_nametoindex(o.ifname.c_str()));
        if (!ifindex_) fail("if_nametoindex " + o.ifname);
        fd_ = ::socket(AF_XDP, SOCK_RAW, 0);
        if (fd_ < 0) fail("socket(AF_XDP)");

        // UMEM: frames shared with the kernel
        umem_len_ = std::size_t{o.frames} * o.frame_size;
        umem_ = ::mmap(nullptr, umem_len_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (umem_ == MAP_FAILED) fail("mmap umem");
        xdp_umem_reg reg{};
        reg.addr = reinterpret_cast<std::uint64_t>(umem_);
        reg.len = umem_len_;
        reg.chunk_size = o.frame_size;
        reg.headroom = 0;
        if (::setsockopt(fd_, SOL_XDP, XDP_UMEM_REG, &reg, sizeof reg)) fail("XDP_UMEM_REG");

        const int n = static_cast<int>(o.ring_size);
        if (::setsockopt(fd_, SOL_XDP, XDP_UMEM_FILL_RING, &n, sizeof n)) fail("XDP_UMEM_FILL_RING");
        if (::setsockopt(fd_, SOL_XDP, XDP_UMEM_COMPLETION_RING, &n, sizeof n)) fail("XDP_UMEM_COMPLETION_RING");
        if (::setsockopt(fd_, SOL_XDP, XDP_RX_RING, &n, sizeof n)) fail("XDP_RX_RING");

        xdp_mmap_offsets off{};
        socklen_t ol = sizeof off;
        if (::getsockopt(fd_, SOL_XDP, XDP_MMAP_OFFSETS, &off, &ol)) fail("XDP_MMAP_OFFSETS");
        map_ring(fill_, off.fr, XDP_UMEM_PGOFF_FILL_RING, sizeof(std::uint64_t));
        map_ring(comp_, off.cr, XDP_UMEM_PGOFF_COMPLETION_RING, sizeof(std::uint64_t));
        map_ring(rx_, off.rx, XDP_PGOFF_RX_RING, sizeof(xdp_desc));

        // Hand every frame to the kernel through the fill ring.
        auto* fr = static_cast<std::uint64_t*>(fill_.desc);
        const std::uint32_t give = std::min(o.ring_size, o.frames);
        for (std::uint32_t i = 0; i < give; ++i) fr[i & fill_.mask] = std::uint64_t{i} * o.frame_size;
        next_frame_ = give;
        store_release(fill_.producer, give);

        sockaddr_xdp sa{};
        sa.sxdp_family = AF_XDP;
        sa.sxdp_ifindex = static_cast<std::uint32_t>(ifindex_);
        sa.sxdp_queue_id = o.queue;
        sa.sxdp_flags = o.zero_copy ? XDP_ZEROCOPY : XDP_COPY;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa)) fail("bind AF_XDP");

        load_and_attach();
    }

    ~Socket() {
        if (link_fd_ >= 0) ::close(link_fd_);   // detaches the XDP program
        if (prog_fd_ >= 0) ::close(prog_fd_);
        if (map_fd_ >= 0) ::close(map_fd_);
        for (Ring* r : {&fill_, &comp_, &rx_})
            if (r->map) ::munmap(r->map, r->map_len);
        if (fd_ >= 0) ::close(fd_);
        if (umem_ && umem_ != MAP_FAILED) ::munmap(umem_, umem_len_);
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Drains the RX ring once: f(udp_payload, len) for every packet, then recycles
    // the frames through the fill ring. Returns the number of packets handled.
    // No syscall is made unless the kernel asks for a wakeup.
    template <class F>
    std::uint32_t poll_once(F&& f) {
        const std::uint32_t prod = load_acquire(rx_.producer);
        std::uint32_t cons = *rx_.consumer;
        const std::uint32_t n = prod - cons;
        if (n == 0) return 0;
        const auto* d = static_cast<const xdp_desc*>(rx_.desc);
        auto* fr = static_cast<std::uint64_t*>(fill_.desc);
        std::uint32_t fprod = *fill_.producer;
        const auto* base = static_cast<const std::uint8_t*>(umem_);
        for (std::uint32_t i = 0; i < n; ++i, ++cons) {
            const xdp_desc& e = d[cons & rx_.mask];
            if (e.len > 42) f(base + e.addr + 42, e.len - 42);   // skip Ethernet + IPv4 + UDP headers
            fr[fprod++ & fill_.mask] = e.addr & ~std::uint64_t{o_.frame_size - 1};
        }
        store_release(rx_.consumer, cons);
        store_release(fill_.producer, fprod);
        if (*fill_.flags & XDP_RING_NEED_WAKEUP) ::recvfrom(fd_, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
        return n;
    }

    int fd() const { return fd_; }

private:
    [[noreturn]] static void fail(const std::string& what) {
        throw std::runtime_error(what + ": " + std::strerror(errno));
    }

    void map_ring(Ring& r, const xdp_ring_offset& ro, off_t pgoff, std::size_t elem) {
        r.map_len = ro.desc + std::size_t{o_.ring_size} * elem;
        r.map = ::mmap(nullptr, r.map_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd_, pgoff);
        if (r.map == MAP_FAILED) fail("mmap ring");
        auto* b = static_cast<std::uint8_t*>(r.map);
        r.producer = reinterpret_cast<std::uint32_t*>(b + ro.producer);
        r.consumer = reinterpret_cast<std::uint32_t*>(b + ro.consumer);
        r.flags = reinterpret_cast<std::uint32_t*>(b + ro.flags);
        r.desc = b + ro.desc;
        r.mask = o_.ring_size - 1;
    }

    void load_and_attach() {
        union bpf_attr a {};
        a.map_type = BPF_MAP_TYPE_XSKMAP;
        a.key_size = 4;
        a.value_size = 4;
        a.max_entries = 64;
        map_fd_ = static_cast<int>(bpf(BPF_MAP_CREATE, &a));
        if (map_fd_ < 0) fail("BPF_MAP_CREATE xskmap");

        const std::uint32_t key = o_.queue;
        const std::uint32_t val = static_cast<std::uint32_t>(fd_);
        std::memset(&a, 0, sizeof a);
        a.map_fd = static_cast<std::uint32_t>(map_fd_);
        a.key = reinterpret_cast<std::uint64_t>(&key);
        a.value = reinterpret_cast<std::uint64_t>(&val);
        if (bpf(BPF_MAP_UPDATE_ELEM, &a)) fail("BPF_MAP_UPDATE_ELEM");

        const std::vector<bpf_insn> prog = udp_port_filter(map_fd_, o_.udp_port);
        static char log[65536];
        static const char license[] = "GPL";
        std::memset(&a, 0, sizeof a);
        a.prog_type = BPF_PROG_TYPE_XDP;
        a.insns = reinterpret_cast<std::uint64_t>(prog.data());
        a.insn_cnt = static_cast<std::uint32_t>(prog.size());
        a.license = reinterpret_cast<std::uint64_t>(license);
        a.log_buf = reinterpret_cast<std::uint64_t>(log);
        a.log_size = sizeof log;
        a.log_level = 1;
        prog_fd_ = static_cast<int>(bpf(BPF_PROG_LOAD, &a));
        if (prog_fd_ < 0) throw std::runtime_error(std::string("BPF_PROG_LOAD: ") + std::strerror(errno) + "\n" + log);

        std::memset(&a, 0, sizeof a);
        a.link_create.prog_fd = static_cast<std::uint32_t>(prog_fd_);
        a.link_create.target_ifindex = static_cast<std::uint32_t>(ifindex_);
        a.link_create.attach_type = BPF_XDP;
        a.link_create.flags = o_.native ? XDP_FLAGS_DRV_MODE : XDP_FLAGS_SKB_MODE;
        link_fd_ = static_cast<int>(bpf(BPF_LINK_CREATE, &a));
        if (link_fd_ < 0) fail("BPF_LINK_CREATE (attach XDP)");
    }

    Options o_;
    int ifindex_ = 0;
    int fd_ = -1;
    void* umem_ = nullptr;
    std::size_t umem_len_ = 0;
    std::uint32_t next_frame_ = 0;
    Ring fill_, comp_, rx_;
    int map_fd_ = -1, prog_fd_ = -1, link_fd_ = -1;
};

}  // namespace mde::xdp

#endif  // __linux__
