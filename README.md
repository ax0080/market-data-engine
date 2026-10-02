# market-data-engine

A C++20 market-data feed handler that decodes **NASDAQ TotalView-ITCH 5.0**, **Binance** and **Coinbase** order-book feeds into one venue-neutral event stream and rebuilds the order book: order-by-order (L3) for ITCH, price-level (L2) for the crypto venues.

It also covers the trading side of the NASDAQ protocol stack: **UDP market data with TCP gap recovery** (MoldUDP64 + SoupBinTCP), **OUCH 5.0 order entry**, and an exchange simulator around the [orderbook-engine](https://github.com/ax0080/orderbook-engine) matching engine, so the full tick-to-trade path (packet in, book update, decision, order out) runs and is measured between two cloud VMs.

Companion projects: a [matching engine](https://github.com/ax0080/orderbook-engine) and a [backtest engine](https://github.com/ax0080/backtest-engine).

```
 transport               decoders (one per venue)              one event type        books
 ─────────────          ──────────────────────────            ──────────────        ─────────────────
 file replay      ──►   ITCH 5.0 (binary, zero-copy)   ──┐                          L3: order table +
 UDP multicast    ──►   MoldUDP64 framing + sequencing   ├──►    BookEvent    ──►      price levels
 (recvmmsg/AF_XDP)      Binance diff-depth (JSON)      ──┤   (compile-time sink)    L2: tick-grid levels
 recorded WS      ──►   Coinbase level2 (JSON)         ──┘                          gap / resync checks

 tick to trade (tools/trader.cpp, one core)                    exchange simulator (sim/)
 ──────────────────────────────────────────                    ─────────────────────────────────────
 UDP  ITCH/MoldUDP64 ─► GapFiller ─► ITCH ─► L3 book ─► rule ◄── ITCH publisher ◄─┐
 TCP  SoupBinTCP replay ──┘ (only while a gap is open)        ◄── replay log       ├─ orderbook-engine
 TCP  OUCH 5.0 order ─────────────────────────────────────────►  OUCH gateway ─────┘   (matching)
```

## Results

Intel Core i5-12600K, Windows 10, g++ 15.2 `-O3 -march=native`, one thread. Timings cover decode and book update only; file reads are excluded.

### Correctness

| Feed | Data | Check | Result |
|---|---|---|---|
| NASDAQ ITCH 5.0 | full trading day, 30 Jan 2019: 11.25 GB, **368.4 M messages** | every execute / cancel / delete / replace must find a live order | **0 unknown-order events**, 0 duplicate adds; the book is empty after the close, as it should be |
| Binance BTCUSDT, ETHUSDT | 29 min live diff-depth stream each (~510 k / 410 k level updates) | rebuilt book against **independent REST snapshots**, level by level, top 1,000 levels per side | **10,000 levels compared per symbol, 0 mismatches**, 0 sequence gaps |
| Coinbase BTC-USD, ETH-USD | 29 min live level2 stream each (~30 k messages) | sequence continuity, book never crossed | 0 gaps, 0 crossed states |
| UDP feed with TCP gap recovery | exchange simulator, 1.6 M ITCH messages per 20 s run, every 50th UDP packet withheld | trader's L3 book vs the book the exchange published (hash of every level) | **~6,000 gaps per run, all filled over SoupBinTCP; books identical** (two GCP VMs and CI loopback) |
| Exchange gateway | 20,000 random OUCH enter / replace / cancel and house orders against the real matching engine | after every operation: L3 book rebuilt from the published ITCH vs the engine's book; each client's open quantity from OUCH replies vs the engine | **identical at every step** |

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

"Late" packets in the logs arrived after a higher sequence number (reordered in the network). `udp_bench` uses the counting MoldUDP64 decoder, which drops them, so they count as lost messages even though the packet arrived; the trader below uses `GapFiller`, which holds later packets and fills the hole over TCP instead.

### Tick to trade: UDP market data in, OUCH order out (two GCP VMs)

The same two-VM setup (`n2-standard-4`, gVNIC, one RX queue on the trader, each program pinned to one core). `exchange_sim` on one VM runs the matching engine and publishes ITCH over UDP at about 82 k messages/s across 8 symbols. Every millisecond it adds a 5,000-share bid; `trader` on the other VM reacts by buying 100 shares IOC at the best ask, sending the OUCH order from inside the decode callback of the packet that carried the signal. 20,000 orders per run; ranges are two runs ([`t2t_run1.log`](bench/gcp/t2t_run1.log), [`t2t_run2.log`](bench/gcp/t2t_run2.log)).

| Market-data receive | Tick-to-trade in the trader, p50 / p99 / p99.9 | Order round trip, p50 | Trigger sent -> order received, at the exchange, p50 / p99 |
|---|---|---|---|
| `recvmmsg` | 0.68-0.69 / 1.64-1.85 / 4.6-5.1 µs | 70-85 µs | 74-86 / 135-136 µs |
| **AF_XDP, copy, generic XDP** | **0.35-0.36 / 1.15-1.17 / 3.6 µs** | 70-74 µs | 72-82 / 117-126 µs |
| AF_XDP, zero-copy, driver XDP | 0.75-0.77 / 1.74-1.83 / 5.0-5.5 µs | 87-91 µs | 90-95 / 139-145 µs |
| `recvmmsg`, every 50th packet withheld | 0.85-0.92 / 2.1-5.1 / 4.9-7.0 µs | 71-81 µs | 73-104 / 119-533 µs |

- **Tick-to-trade in the trader** runs from the receive call returning the packet's batch to the OUCH order being handed to `send()`: MoldUDP64 sequencing, ITCH decode, L3 book update, the rule, and encoding the SoupBinTCP + OUCH message. It is stamped once per receive batch on every path, so a packet queued behind others in the same batch carries their processing time.
- **Trigger sent -> order received** is measured by the exchange: the trigger packet is stamped just before `sendto()` and the order just after `recv()`, both on the exchange's clock, so no clock synchronisation is involved. It covers two network hops plus the trader.
- **The network dominates.** The virtual network's round trip (about 70 µs) is about 200 times the trader's own processing, so the receive method moves in-process latency by tenths of a microsecond but barely moves what the exchange sees. On colocated hardware with kernel-bypass NICs the wire time shrinks to single-digit microseconds and the in-process part starts to matter.
- **Copy-mode AF_XDP was the fastest receive path here, ahead of zero-copy.** Our likely explanation (not verified with counters): in zero-copy mode the NIC writes frames into memory the CPU has not touched, so the decoder's first reads miss the cache; in copy mode the kernel's copy has just brought the data into cache. The flood test above favoured zero-copy because there the per-packet kernel work, not cache misses, was the limit.
- **Gap recovery.** With every 50th packet withheld the trader logged about 6,000 gaps per run, filled each from the exchange's SoupBinTCP replay, and still ended with exactly the exchange's book. Signals that arrive while a gap is open are not traded, because the book is behind. The recovery connection is opened per gap, which costs a TCP handshake each time; that shows in the exchange-observed tail of the second run (p99 533 µs).

On WSL2 loopback, where both programs share one machine, tick-to-trade in the trader is 0.29-0.30 µs p50, and trigger -> order at the exchange is 3.6-4.1 µs p50.

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

**UDP with TCP recovery.** `GapFiller` delivers MoldUDP64 messages strictly in sequence. When a packet arrives ahead of the next expected number it stops delivering, holds that packet and every later one, and reports the missing range. The trader then opens a SoupBinTCP session to the exchange's replay port, logs in with the first missing sequence number, feeds the replayed messages in, and logs out as soon as the hole is filled; the held packets are then replayed and live delivery resumes. Packets that partly overlap what was already delivered are trimmed, and MoldUDP64 heartbeats and End of Session carry the next sequence number, so a gap at the very end of the feed is found too. The normal path stays zero-copy; packets are copied only while a gap is open.

**SoupBinTCP.** NASDAQ's session layer over TCP: a 2-byte length, a type byte, and a payload. TCP may split a logical packet across reads or put several in one, so `soup::Framer` reassembles them in place and only moves an incomplete tail to the front of its buffer. Login carries the next sequence number the client wants, so after a reconnect the server resumes exactly where the client stopped; heartbeats in both directions detect a dead link.

**OUCH 5.0.** Field offsets follow NASDAQ's specification (big-endian binary, 8-byte prices with 4 implied decimals, space-padded alpha fields), and the tests check the layout byte by byte. Orders are named by the client's UserRefNum, which must strictly increase, so a resent message is recognised and ignored; Replace re-states the total quantity liable for the whole order chain, and Cancel gives the new intended order size (0 cancels; the simulator treats it as the new open quantity).

**Exchange gateway.** `sim/gateway.h` puts the matching engine behind both protocols. Every book change becomes an ITCH message for everyone and an OUCH reply for the order's owner. ITCH only shows resting orders: an order that trades on arrival produces Executed messages against the orders it hit and an Add Order for whatever rests; a reprice is published as Delete + Add under a new reference, and a size reduction as a Cancel that keeps priority. The engine reports events after each operation finishes, so anything the OUCH reply must state as of a replace (the quantity outstanding at that moment) is captured before the call.

**Tick to trade on one core.** The trader busy-polls the UDP socket (or the AF_XDP ring) and decides inside the decode callback: on a signal it builds the SoupBinTCP + OUCH order in a stack buffer and calls `send()` on a `TCP_NODELAY` socket, with no queue or thread hop. Order-entry and recovery sockets are polled every 16th iteration, since they are not on the hot path.

## Build and run

```sh
cmake -S . -B build -G Ninja && cmake --build build   # fetches orderbook-engine at a pinned commit
                                                      # (or -DMDE_ORDERBOOK_DIR=<local checkout>)
./build/mde_tests && ./build/mde_sim_tests

# tick to trade on one Linux host: exchange_sim + trader, checks the trader's book equals the exchange's
sh tools/t2t_local.sh build
DROP=20 sh tools/t2t_local.sh build                    # withhold every 20th UDP packet: TCP gap recovery
# two hosts: exchange first, then the trader (add --rx xdp [--native --zc] and sudo for AF_XDP)
./build/exchange_sim --md-dest <trader ip> --duration 20
./build/trader --exchange <exchange ip> --rx recvmmsg
# the GCP runs above
sh tools/gcp_t2t_bench.sh <exchange public ip> <exchange internal ip> <trader public ip> <trader internal ip> bench/gcp/t2t.log

# record 30 minutes of public Binance and Coinbase data (no API key needed)
python tools/record_ws.py --minutes 30
./build/replay_crypto binance data/binance/btcusdt_depth.tsv          # tick-grid book + validation
./build/replay_crypto binance data/binance/btcusdt_depth.tsv vector   # sorted-vector book, for comparison
./build/replay_crypto coinbase data/coinbase/btc-usd_level2.tsv

# NASDAQ sample day: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
gzip -dc 01302019.NASDAQ_ITCH50.gz > 01302019.itch
./build/replay_itch 01302019.itch                  # decode + L3 book (mmap on Linux, chunked reads on Windows)
./build/replay_itch 01302019.itch --decode-only    # decode only; --mmap / --fread to override the I/O method

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
               wire.h  soupbintcp.h  ouch50.h  itch50_writer.h  gap_filler.h  book_hash.h  linux_net.h
sim/           gateway.h (matching engine -> ITCH + OUCH)  exchange_sim.cpp
tools/         replay_itch.cpp  replay_crypto.cpp  udp_bench.cpp  trader.cpp  record_ws.py  make_synthetic_itch.py
tests/         check.h (tiny harness)  test_books.cpp  test_feeds.cpp  test_order_entry.cpp  test_gateway.cpp
```
