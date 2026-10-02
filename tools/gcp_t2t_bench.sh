#!/bin/sh
# Two-VM tick-to-trade benchmark (run from a machine with SSH access to both).
# exchange_sim runs on one VM and sends ITCH over UDP to the other VM's internal
# address; trader runs there, with each market-data receive method, and sends
# OUCH orders back over TCP. Both programs are pinned to one core.
#
#   sh tools/gcp_t2t_bench.sh <exchange public ip> <exchange internal ip> <trader public ip> <trader internal ip> <log>
#
# The trader NIC should have one RX queue (ethtool -L ens4 rx 1 tx 1) so that
# market data lands on queue 0, where the AF_XDP socket is bound.
EX=$1; EX_INT=$2; TR=$3; TR_INT=$4; LOG=$5
S="sh $(dirname "$0")/gcp_ssh.sh"
DUR=${DUR:-20}
RATE=${RATE:-100000}
PIN=${PIN:-3}
TMP=${TMPDIR:-/tmp}

run_one() {   # $1 label, $2 trader args, $3 drop
  echo "=== $1 | rate $RATE events/s, ${DUR}s, withheld packets: ${3:+every ${3}th}${3:-none}" | tee -a "$LOG"
  $S run "$EX" "timeout 180 taskset -c $PIN ./exchange_sim --md-dest $TR_INT --duration $DUR --rate $RATE --drop ${3:-0}" \
    > "$TMP/t2t_ex.log" 2>&1 &
  epid=$!
  sleep 2
  $S run "$TR" "sudo timeout 180 taskset -c $PIN ./trader --exchange $EX_INT --ifname ens4 $2" > "$TMP/t2t_tr.log" 2>&1
  wait $epid
  cat "$TMP/t2t_ex.log" "$TMP/t2t_tr.log" >> "$LOG"
  echo >> "$LOG"
  grep -E "trigger -> order|tick-to-trade \(rx -> send call|order RTT|book hash|recovered" "$TMP/t2t_ex.log" "$TMP/t2t_tr.log"
}

run_one recvmmsg "--rx recvmmsg"
run_one xdp-copy-skb "--rx xdp"
run_one xdp-zc-drv "--rx xdp --native --zc"
run_one recvmmsg-gap-recovery "--rx recvmmsg" 50
