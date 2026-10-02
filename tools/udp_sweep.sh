#!/bin/sh
# Rate sweep for udp_bench: lossless capacity of recvfrom vs recvmmsg,
# for transport only and for the full MoldUDP64 -> ITCH -> L3 book pipeline.
# usage: tools/udp_sweep.sh <udp_bench binary> <raw itch file> <messages>
BIN=$1; FILE=$2; N=$3
for mode in transport full; do
  for pps in 0 100000 200000 300000 500000 800000; do
    for rx in recvfrom recvmmsg; do
      timeout 120 "$BIN" "$FILE" "$N" "$rx" "$pps" "$mode"
    done
  done
done
