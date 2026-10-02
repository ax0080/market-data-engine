"""Write a synthetic, self-consistent ITCH 5.0 stream (length-prefixed, like the
NASDAQ sample files) for CI and quick local runs.

Every execute / cancel / delete / replace refers to an order that is live at that
point, so a correct L3 book ends with zero unknown-order events.

usage: python tools/make_synthetic_itch.py <out file> <messages>
"""
import random
import struct
import sys


def header(t, locate, ts):
    return struct.pack(">cHH", t, locate, 0) + ts.to_bytes(6, "big")


def main(path, n):
    rng = random.Random(1)
    live = {}                  # ref -> [locate, side, shares, price]
    refs = []                  # live refs, for O(1) random pick (swap-remove)
    pos = {}                   # ref -> index in refs
    next_ref = 1

    def track(ref):
        pos[ref] = len(refs)
        refs.append(ref)

    def untrack(ref):
        i = pos.pop(ref)
        last = refs.pop()
        if last != ref:
            refs[i] = last
            pos[last] = i
    ts = 34_200_000_000_000    # 09:30:00 in ns since midnight
    out = bytearray()

    def put(body):
        out.extend(struct.pack(">H", len(body)))
        out.extend(body)

    for loc, sym in enumerate([b"AAPL    ", b"MSFT    ", b"NVDA    ", b"AMZN    "], start=1):
        put(header(b"R", loc, ts) + sym + bytes(20))

    written = 4
    while written < n:
        ts += rng.randint(100, 5000)
        r = rng.random()
        # Keep the live book around 20k orders: add more when small, remove more when large.
        add_p = 0.6 if len(live) < 20_000 else 0.4
        if r < add_p or len(live) < 50:
            loc = rng.randint(1, 4)
            side = rng.choice([b"B", b"S"])
            mid = 1_000_000 * loc
            price = mid - rng.randint(1, 200) * 100 if side == b"B" else mid + rng.randint(1, 200) * 100
            shares = rng.randint(1, 50) * 100
            put(header(b"A", loc, ts) + struct.pack(">Q", next_ref) + side + struct.pack(">I", shares)
                + b"SYM     " + struct.pack(">I", price))
            live[next_ref] = [loc, side, shares, price]
            track(next_ref)
            next_ref += 1
        else:
            ref = refs[rng.randrange(len(refs))]
            loc, side, shares, price = live[ref]
            kind = rng.random()
            if kind < 0.3:             # execute part or all
                q = rng.randint(1, shares)
                put(header(b"E", loc, ts) + struct.pack(">QIQ", ref, q, rng.getrandbits(40)))
                live[ref][2] -= q
            elif kind < 0.5:           # partial cancel
                q = rng.randint(1, shares)
                put(header(b"X", loc, ts) + struct.pack(">QI", ref, q))
                live[ref][2] -= q
            elif kind < 0.8:           # delete
                put(header(b"D", loc, ts) + struct.pack(">Q", ref))
                live[ref][2] = 0
            else:                      # replace with a new reference
                new_price = price + rng.choice([-100, 100])
                new_shares = rng.randint(1, 50) * 100
                put(header(b"U", loc, ts) + struct.pack(">QQII", ref, next_ref, new_shares, new_price))
                live[next_ref] = [loc, side, new_shares, new_price]
                track(next_ref)
                next_ref += 1
                live[ref][2] = 0
            if live[ref][2] <= 0:
                del live[ref]
                untrack(ref)
        written += 1

    with open(path, "wb") as f:
        f.write(out)
    print(f"wrote {written} messages ({len(out)} bytes) to {path}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]))
