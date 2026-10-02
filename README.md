# market-data-engine

A C++20 market-data feed handler that decodes **NASDAQ TotalView-ITCH 5.0**, **Binance** and **Coinbase** order-book feeds into one venue-neutral event stream and rebuilds the order book: order-by-order (L3) for ITCH, price-level (L2) for the crypto venues.

Companion projects: a [matching engine](https://github.com/ax0080/orderbook-engine) and a [backtest engine](https://github.com/ax0080/backtest-engine).

```
 transport               decoders (one per venue)              one event type        books
 ─────────────          ──────────────────────────            ──────────────        ─────────────────
 file replay      ──►   ITCH 5.0 (binary, zero-copy)   ──┐                          L3: order table +
 UDP multicast    ──►   MoldUDP64 framing + sequencing   ├──►    BookEvent    ──►      price levels
 (recvmmsg)             Binance diff-depth (JSON)      ──┤   (compile-time sink)    L2: tick-grid levels
 recorded WS      ──►   Coinbase level2 (JSON)         ──┘                          gap / resync checks
```

## Results

Intel Core i5-12600K, Windows 10, g++ 15.2 `-O3 -march=native`, one thread. Timings cover decode and book update only; file reads are excluded.

### Correctness

| Feed | Data | Check | Result |
|---|---|---|---|
| NASDAQ ITCH 5.0 | full trading day, 30 Jan 2019: 11.25 GB, **368.4 M messages** | every execute / cancel / delete / replace must find a live order | **0 unknown-order events**, 0 duplicate adds; the book is empty after the close, as it should be |
| Binance BTCUSDT, ETHUSDT | 29 min live diff-depth stream each (~510 k / 410 k level updates) | rebuilt book against **independent REST snapshots**, level by level, top 1,000 levels per side | **10,000 levels compared per symbol, 0 mismatches**, 0 sequence gaps |
| Coinbase BTC-USD, ETH-USD | 29 min live level2 stream each (~30 k messages) | sequence continuity, book never crossed | 0 gaps, 0 crossed states |

### Performance

| Feed | Book | Decode + book update | Decode only |
|---|---|---|---|
| NASDAQ ITCH 5.0, full day | L3 (order by order) | **63.5 ns/message, 15.7 M messages/s** | 6.8 ns/message (148 M/s) |
| Binance BTCUSDT | L2 tick grid | **55 ns/level** (1.5 µs/message) | 29 ns/level |
| Binance ETHUSDT | L2 tick grid | 53 ns/level | 30 ns/level |
| Coinbase BTC-USD | L2 tick grid | 119 ns/level | 93 ns/level |
| Coinbase ETH-USD | L2 tick grid | 113 ns/level | 93 ns/level |

**Tick grid vs sorted vector** on the same recordings: Binance BTCUSDT 55 vs 919 ns/level (**17x**), Coinbase BTC-USD 119 vs 1,556 ns/level (**13x**), ETH 7x and 6x. Crypto books are thousands of levels deep and updated everywhere, which is exactly where a sorted vector's memmove hurts.

**Where the ITCH time goes.** Decoding is 6.8 ns of the 63.5 ns. With the price-level aggregation switched off the book costs about 15 ns, so most of the rest is the per-instrument level update: each message lands on one of ~8,700 symbols, so its levels are usually not in cache. Two measured changes cut the total from 81 to 63.5 ns: an identity hash for the order table (NASDAQ order references are sequential and short-lived, so recent orders sit in neighbouring, cached slots; 81 to 65 ns), and storing instruments inline instead of behind pointers.

### Network receive: recvfrom vs recvmmsg vs AF_XDP (Linux, WSL2 loopback)

`udp_bench` packs the first 5 M messages of the same ITCH day into MoldUDP64 packets (about 47 messages each), sends them over loopback, and receives them through MoldUDP64, the ITCH decoder and the L3 book.

The AF_XDP path is built from raw syscalls, with no libbpf, libxdp or clang: a 23-instruction XDP program, assembled in `af_xdp.h` as bytecode, redirects only UDP packets for the feed port into an AF_XDP socket and passes all other traffic to the normal stack. It is attached with `BPF_LINK_CREATE`, so it detaches when the process exits. The receiver polls the RX ring in memory shared with the kernel and recycles frames through the fill ring: no syscall per packet.

| Sender rate | recvfrom | recvmmsg | AF_XDP (generic XDP, copy mode) |
|---|---|---|---|
| 100 k packets/s (4.7 M msgs/s) | 0 loss | 0 loss | 0 loss |
| 300 k packets/s (14 M msgs/s) | 0 loss | 0.4% loss | 0 loss |
| flood: packets/s actually received | 371 k | 349 k | **501 k (+35%)** |
| receive syscalls per packet | 1 | 1/13 | **0** |

Under flood AF_XDP took 35% more packets per second than `recvfrom`: packets are taken at the driver hook, skipping the IP/UDP stack and socket layer. This is the least favourable AF_XDP setting (WSL2 has no physical NIC, so XDP runs in generic mode and the kernel copies each packet into the shared memory).

`recvmmsg` cut syscalls by more than 10x but did not raise throughput on loopback, where the sender and the book update, not the syscalls, were the limits.

### Network receive on a real NIC: AF_XDP zero-copy (two GCP VMs)

Two `n2-standard-4` VMs in one zone (asia-east1-b), gVNIC (`gve` driver), Ubuntu 24.04, kernel 7.0.0-gcp; the receiver NIC set to one RX/TX queue and the receiver pinned to a core that does not service the NIC interrupt. One VM sends the first 5 M messages of the ITCH day as fast as it can (about 440 k packets/s from one core); the other receives them and runs MoldUDP64, the ITCH decoder and the L3 book **on the same core**. Raw output and both machines' environment reports are in [`bench/gcp/`](bench/gcp/).

| Receive path, full pipeline on one core | Packets/s | Messages/s | Share of the flood handled |
|---|---|---|---|
| `recvfrom` | 240 k | 11.3 M | 55-60% |
| `recvmmsg` | 255 k | 12.0 M | 63-65% |
| AF_XDP, copy, generic XDP | 333-338 k | 15.6-15.8 M | 78-82% |
| AF_XDP, copy, driver XDP | 329-344 k | 15.0-16.1 M | 85% |
| **AF_XDP, zero-copy, driver XDP** | **329-348 k** | **15.4-16.3 M** | **85-88%** |

Ranges are two runs ([`flood_full_pinned.log`](bench/gcp/flood_full_pinned.log)). In zero-copy mode the NIC DMAs packets straight into the shared UMEM, so the kernel does no per-packet copy, socket work or syscall, and the core spends its time on decoding. The full pipeline then ran at **16.3 M msgs/s, the same as replaying the file from memory (15.7 M msgs/s)**: the cost of receiving from the network all but disappeared, and the L3 book became the only limit. Through the kernel socket path the same core managed 11.3 M msgs/s, **1.4x less**. Zero-copy was also the only path with no reordered packets in either run.

With decoding switched off (`--transport`), every path took all packets: one sending core (about 440 k packets/s) is below the receive limit of all five, so the network path is only the bottleneck when it shares the core with real work ([`flood_transport_pinned.log`](bench/gcp/flood_transport_pinned.log)). The unpinned rate sweep, where the receiver sometimes shared a core with the NIC interrupt, is in [`sweep.log`](bench/gcp/sweep.log).

"Late" packets in the logs arrived after a higher sequence number (reordered in the network). A production feed handler would recover them with a retransmission request; here MoldUDP64 drops them, so they count as lost messages even though the packet arrived.

### File replay: fread vs mmap

| Full ITCH day, decode only, warm page cache | fread (256 MB chunks) | mmap |
|---|---|---|
| Linux (WSL2 ext4), end to end | 3.63 s (101 M msgs/s) | **2.44 s (151 M msgs/s)** |
| I/O overhead on top of decoding | 1.2 s | **0.03 s** |
| Windows 10, end to end | **4.1 s** | 4.95 s |

Memory-mapping removes the kernel-to-user copy, and the file becomes one contiguous range, so no message straddles a buffer edge. On Linux that removes almost all I/O cost. On Windows it is slower: every 4 KB page takes a soft fault on first touch, and `PrefetchVirtualMemory` does not map pages into the working set (it measured slower still, so it is disabled there). Linux maps pages around each fault and honours `MADV_SEQUENTIAL` / `MADV_WILLNEED`.

## Design

**One event model, no virtual calls.** Every decoder emits `BookEvent` (add / execute / cancel / delete / replace for L3, set-level / clear for L2) into a sink that is a template parameter, so the call from decoder to book is resolved at compile time and inlined.

**ITCH 5.0, zero-copy.** Fields are read in place from the receive buffer at fixed offsets with a byte-swapping load; nothing is copied into an intermediate struct. The message type byte drives a `switch` that compiles to a jump table. The same decoder takes length-prefixed file data or MoldUDP64 packets.

**L3 order table.** Resting orders live in one open-addressing hash table keyed by order id. Linear probing keeps a lookup in one or two cache lines; deletion uses backward shift, so there are no tombstones and probe lengths do not degrade over a full day of adds and deletes. The default identity hash exploits NASDAQ's sequential order references (see above); `-DMDE_SCATTER_HASH` switches to a multiplicative hash for feeds with random ids. A randomized test checks the table against `std::unordered_map` over 200,000 operations, with both hashes.

**L2 tick grid.** Crypto venues publish thousands of levels and update them across the whole depth. A sorted vector pays a memmove of thousands of entries on each update; here a price maps straight to an array slot, `(price - anchor) / tick`, so an update is one store. The array covers 131,072 ticks around the market and re-centres when the price drifts; levels far outside it, and any off-grid prices, go to a small ordered map. A randomized test checks it against `std::map` over 300,000 operations with a drifting price.

**JSON without a JSON library.** Exchange messages have a fixed shape, so the decoders scan the raw text for the few keys they need and convert decimal strings straight to 8-decimal fixed point. No allocation, no `double`, no rounding.

**Sequencing.** Binance follows the documented sync: buffer the stream, take a REST snapshot, drop updates already in it, require `U <= lastUpdateId + 1 <= u` for the first one and `U == previous u + 1` afterwards; a hole marks the book unsynced until the next snapshot. Coinbase checks that `sequence_num` advances by one per message. MoldUDP64 detects gaps (and counts lost messages) and drops duplicates from a redundant feed.

## Build and run

```sh
cmake -S . -B build -G Ninja && cmake --build build
./build/mde_tests

# record 30 minutes of public Binance and Coinbase data (no API key needed)
python tools/record_ws.py --minutes 30
./build/replay_crypto binance data/binance/btcusdt_depth.tsv          # tick-grid book + validation
./build/replay_crypto binance data/binance/btcusdt_depth.tsv vector   # sorted-vector book, for comparison
./build/replay_crypto coinbase data/coinbase/btc-usd_level2.tsv

# NASDAQ sample day: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
gzip -dc 01302019.NASDAQ_ITCH50.gz > 01302019.itch
./build/replay_itch 01302019.itch                  # decode + L3 book (mmap)
./build/replay_itch 01302019.itch --decode-only    # decode only; add --fread to compare with chunked reads

# network receive on loopback (Linux; AF_XDP needs root)
sudo sh tools/xdp_local_test.sh ./build/udp_bench 01302019.itch 5000000 100000 300000 0
# two hosts (e.g. cloud VMs): receiver first, then sender
sudo ./build/udp_bench recv xdp --ifname eth0 --expect 5000000      # add --zc --native if the NIC supports it
./build/udp_bench send 01302019.itch 5000000 <receiver ip> 200000
# the GCP runs above: SSH helper, environment report and the full sweep
sh tools/gcp_net_bench.sh <tx public ip> <rx public ip> <rx internal ip> bench/gcp/run.log

# file replay: fread vs mmap on a warm page cache
sh tools/io_compare.sh ./build/replay_itch 01302019.itch
# no NASDAQ file? a self-consistent synthetic stream works too (used in CI):
python3 tools/make_synthetic_itch.py synthetic.itch 2000000
```

## Layout

```
include/mde/   types.h  itch50.h  moldudp64.h  binance.h  coinbase.h  json_scan.h
               order_table.h  l3_book.h  price_levels.h  tick_levels.h  l2_book.h
               mapped_file.h  af_xdp.h
tools/         replay_itch.cpp  replay_crypto.cpp  udp_bench.cpp  record_ws.py  make_synthetic_itch.py
tests/         check.h (tiny harness)  test_books.cpp  test_feeds.cpp
```
