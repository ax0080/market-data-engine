#!/bin/sh
# Loopback comparison of recvfrom / recvmmsg / AF_XDP (generic XDP, copy mode).
# Run as root (AF_XDP needs CAP_NET_ADMIN/CAP_BPF/CAP_NET_RAW).
# usage: sh tools/xdp_local_test.sh <udp_bench binary> <raw itch file> <messages> [pps...]
BIN=$1; FILE=$2; N=$3; shift 3
RATES=${*:-"100000 200000 0"}
for pps in $RATES; do
  for rx in recvfrom recvmmsg xdp; do
    timeout 120 "$BIN" local "$FILE" "$N" "$rx" "$pps" --unicast
  done
done
