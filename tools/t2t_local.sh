#!/bin/sh
# Tick-to-trade over loopback: exchange_sim and trader on one host.
# Fails unless the trader finished the session, traded, and rebuilt exactly
# the book the exchange published (same L3 book hash).
#
#   tools/t2t_local.sh [build dir]       env: DUR=3 DROP=0 RATE=100000 EX_CPU= TR_CPU=
#   DROP=20 withholds every 20th UDP packet, so the trader must recover over TCP.
set -e
B=${1:-build}
DUR=${DUR:-3}
DROP=${DROP:-0}
RATE=${RATE:-100000}
LOG=${LOG:-$B}
EX_CPU=${EX_CPU:+--cpu $EX_CPU}
TR_CPU=${TR_CPU:+--cpu $TR_CPU}

"$B/exchange_sim" --duration "$DUR" --drop "$DROP" --rate "$RATE" $EX_CPU > "$LOG/exchange.log" 2>&1 &
EX=$!
sleep 0.5
set +e
"$B/trader" --timeout 60 $TR_CPU > "$LOG/trader.log" 2>&1
TR_RC=$?
wait $EX
EX_RC=$?
set -e
cat "$LOG/exchange.log"
echo
cat "$LOG/trader.log"

ex_hash=$(sed -n 's/^book hash \([0-9a-f]*\).*/\1/p' "$LOG/exchange.log")
tr_hash=$(sed -n 's/^book hash \([0-9a-f]*\).*/\1/p' "$LOG/trader.log")
fills=$(sed -n 's/.*fills \([0-9]*\).*/\1/p' "$LOG/trader.log")
echo
if [ "$TR_RC" -ne 0 ] || [ "$EX_RC" -ne 0 ]; then echo "FAIL: exit codes trader=$TR_RC exchange=$EX_RC"; exit 1; fi
if [ -z "$ex_hash" ] || [ "$ex_hash" != "$tr_hash" ]; then echo "FAIL: book hash exchange=$ex_hash trader=$tr_hash"; exit 1; fi
if [ "${fills:-0}" -eq 0 ]; then echo "FAIL: no fills"; exit 1; fi
if [ "$DROP" -gt 0 ] && ! grep -q "recovered over TCP" "$LOG/trader.log"; then echo "FAIL: no recovery"; exit 1; fi
echo "OK: books match ($tr_hash), $fills fills"
