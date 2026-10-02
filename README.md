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

### UDP multicast (Linux, WSL2 loopback)

`udp_bench` packs the first 5 M messages of the same ITCH day into MoldUDP64 packets (about 47 messages each), multicasts them on loopback, and receives them through MoldUDP64, the ITCH decoder and the L3 book.

| Sender rate | Full pipeline, recvfrom | Full pipeline, recvmmsg |
|---|---|---|
| 100 k packets/s (4.7 M msgs/s) | 0 loss, 1.0 packet/syscall | 0 loss, **11.0 packets/syscall** |
| 200 k packets/s (9.4 M msgs/s) | 0 loss, 1.0 packet/syscall | 0 loss, **11.7 packets/syscall** |

`recvmmsg` cuts receive syscalls by roughly 11x under load. On this loopback setup that did not change throughput or loss: with no NIC in the path the sender (about 380 k packets/s) and the book update, not the syscalls, were the limits. The batching matters on a real NIC at line rate, where per-packet syscall cost dominates.

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
./build/replay_itch 01302019.itch            # decode + L3 book
./build/replay_itch 01302019.itch 24 null    # decode only

# UDP multicast on loopback (Linux): recvfrom vs recvmmsg
sh tools/udp_sweep.sh ./build/udp_bench 01302019.itch 5000000
# no NASDAQ file? a self-consistent synthetic stream works too (used in CI):
python3 tools/make_synthetic_itch.py synthetic.itch 2000000
```

## Layout

```
include/mde/   types.h  itch50.h  moldudp64.h  binance.h  coinbase.h  json_scan.h
               order_table.h  l3_book.h  price_levels.h  tick_levels.h  l2_book.h
tools/         replay_itch.cpp  replay_crypto.cpp  udp_bench.cpp  record_ws.py  make_synthetic_itch.py
tests/         check.h (tiny harness)  test_books.cpp  test_feeds.cpp
```
