#!/bin/sh
# Two-VM network receive benchmark (run from a machine with SSH access to both).
# For each receive method and send rate: start the receiver, then the sender,
# and collect the receiver's report. All output is appended to $LOG.
#
#   sh tools/gcp_net_bench.sh <tx public ip> <rx public ip> <rx internal ip> <log file>
#
# Receive methods: recvfrom, recvmmsg, AF_XDP copy (generic), AF_XDP copy (driver),
# AF_XDP zero-copy (driver). The receiver NIC is set to one RX/TX queue so every
# packet lands on queue 0, where the AF_XDP socket is bound.
TX=$1; RX=$2; RX_INT=$3; LOG=$4
S="sh $(dirname "$0")/gcp_ssh.sh"
N=5000000
RATES=${RATES:-"100000 300000 500000 0"}

$S run "$RX" "sudo ethtool -L ens4 rx 1 tx 1 2>&1; ethtool -l ens4 | tail -4" | tee -a "$LOG"

run_one() {   # $1 = method label, $2 = receiver args, $3 = pps
  echo "--- $1 @ $3 pkt/s" | tee -a "$LOG"
  # PIN: CPU for the receiver (keep it off the core that services the NIC interrupt);
  # EXTRA: e.g. --transport to measure the receive path without ITCH decode / book.
  $S run "$RX" "sudo timeout 90 taskset -c ${PIN:-3} ./udp_bench recv $2 --ifname ens4 --expect $N ${EXTRA:-}" >> "$LOG" 2>&1 &
  rpid=$!
  sleep 4                                   # let the receiver bind / attach XDP
  $S run "$TX" "./udp_bench send itch_head200m.bin $N $RX_INT $3" >> "$LOG" 2>&1
  wait $rpid
  tail -1 "$LOG"
}

for pps in $RATES; do
  run_one recvfrom "recvfrom" "$pps"
  run_one recvmmsg "recvmmsg" "$pps"
  run_one xdp-copy-skb "xdp" "$pps"
  run_one xdp-copy-drv "xdp --native" "$pps"
  run_one xdp-zc-drv "xdp --native --zc" "$pps"
done
