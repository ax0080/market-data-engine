"""Record raw public market-data streams to disk for deterministic C++ replay.

Each output line is:   <recv_ns>\t<kind>\t<raw payload>\n
    kind D = incremental update (WebSocket message, unmodified)
    kind S = REST order-book snapshot (used for initial sync and for validation)

Binance: diff-depth stream (<symbol>@depth@100ms). Following Binance's documented
procedure, the stream is opened first and a REST snapshot is taken after, so replay
can sync on lastUpdateId. Further snapshots every SNAPSHOT_EVERY seconds let the
replayer check its reconstructed book against the exchange's.

Coinbase: Advanced Trade "level2" channel. The first message is an in-band snapshot;
REST snapshots are recorded for reference.

Usage:  python tools/record_ws.py --minutes 30
Only public endpoints are used; no account or API key is needed.
"""
import argparse
import asyncio
import json
import os
import time

import aiohttp

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
SNAPSHOT_EVERY = 300


def now_ns():
    return time.time_ns()


async def binance(session, symbol, minutes):
    path = os.path.join(OUT, "binance", f"{symbol.lower()}_depth.tsv")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    url = f"wss://stream.binance.com:9443/ws/{symbol.lower()}@depth@100ms"
    rest = f"https://api.binance.com/api/v3/depth?symbol={symbol.upper()}&limit=5000"
    end = time.time() + minutes * 60
    n = 0
    with open(path, "w", encoding="utf8", newline="\n") as f:
        async with session.ws_connect(url, heartbeat=20, max_msg_size=0) as ws:
            next_snap = time.time() + 2.0        # first snapshot shortly after the stream is open
            while time.time() < end:
                if time.time() >= next_snap:
                    async with session.get(rest) as r:
                        body = await r.text()
                    f.write(f"{now_ns()}\tS\t{body}\n")
                    next_snap = time.time() + SNAPSHOT_EVERY
                try:
                    msg = await asyncio.wait_for(ws.receive(), 5)
                except asyncio.TimeoutError:
                    continue
                if msg.type != aiohttp.WSMsgType.TEXT:
                    raise RuntimeError(f"binance {symbol}: unexpected message {msg.type}")
                f.write(f"{now_ns()}\tD\t{msg.data}\n")
                n += 1
    print(f"binance {symbol}: {n} updates -> {path}", flush=True)


async def coinbase(session, product, minutes):
    path = os.path.join(OUT, "coinbase", f"{product.lower()}_level2.tsv")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    rest = f"https://api.exchange.coinbase.com/products/{product}/book?level=2"
    end = time.time() + minutes * 60
    n = 0
    with open(path, "w", encoding="utf8", newline="\n") as f:
        async with session.ws_connect("wss://advanced-trade-ws.coinbase.com", heartbeat=20, max_msg_size=0) as ws:
            await ws.send_str(json.dumps({"type": "subscribe", "product_ids": [product], "channel": "level2"}))
            next_snap = time.time() + 2.0
            while time.time() < end:
                if time.time() >= next_snap:
                    async with session.get(rest, headers={"User-Agent": "market-data-engine"}) as r:
                        body = await r.text()
                    f.write(f"{now_ns()}\tS\t{body}\n")
                    next_snap = time.time() + SNAPSHOT_EVERY
                try:
                    msg = await asyncio.wait_for(ws.receive(), 5)
                except asyncio.TimeoutError:
                    continue
                if msg.type != aiohttp.WSMsgType.TEXT:
                    raise RuntimeError(f"coinbase {product}: unexpected message {msg.type}")
                f.write(f"{now_ns()}\tD\t{msg.data}\n")
                n += 1
    print(f"coinbase {product}: {n} messages -> {path}", flush=True)


async def main(minutes):
    async with aiohttp.ClientSession() as s:
        await asyncio.gather(
            binance(s, "BTCUSDT", minutes), binance(s, "ETHUSDT", minutes),
            coinbase(s, "BTC-USD", minutes), coinbase(s, "ETH-USD", minutes),
        )


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=30)
    asyncio.run(main(ap.parse_args().minutes))
