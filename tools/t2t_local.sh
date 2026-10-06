#!/bin/sh
# Tick-to-trade over loopback: exchange_sim and trader on one host.
# Fails unless the trader finished the session, traded, and rebuilt exactly
# the book the exchange published (same L3 book hash).
#
#   tools/t2t_local.sh [build dir]       env: DUR=3 DROP=0 RATE=100000 OE=ouch|fix EX_CPU= TR_CPU=
#   DROP=20 withholds every 20th UDP packet, so the trader must recover over TCP.
set -e
B=${1:-build}
DUR=${DUR:-3}
DROP=${DROP:-0}
RATE=${RATE:-100000}
OE=${OE:-ouch}
LOG=${LOG:-$B}
EX_CPU=${EX_CPU:+--cpu $EX_CPU}
TR_CPU=${TR_CPU:+--cpu $TR_CPU}

"$B/exchange_sim" --duration "$DUR" --drop "$DROP" --rate "$RATE" $EX_CPU > "$LOG/exchange.log" 2>&1 &
EX=$!
sleep 0.5
set +e
"$B/trader" --timeout 60 --oe "$OE" $TR_CPU > "$LOG/trader.log" 2>&1
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

# On failure: say why, and in GitHub Actions also raise an annotation with the
# tail of both logs (annotations are readable without signing in).
fail() {
  echo "FAIL: $1"
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    body=$( { echo "$1 (OE=$OE DROP=$DROP)"; echo "--- trader"; tail -n 25 "$LOG/trader.log";               echo "--- exchange"; tail -n 25 "$LOG/exchange.log"; } | cut -c1-240 | awk '{ printf "%s%%0A", $0 }')
    echo "::error title=t2t_local OE=$OE DROP=$DROP::$body"
  fi
  exit 1
}
if [ "$TR_RC" -ne 0 ] || [ "$EX_RC" -ne 0 ]; then fail "exit codes trader=$TR_RC exchange=$EX_RC"; fi
if [ -z "$ex_hash" ] || [ "$ex_hash" != "$tr_hash" ]; then fail "book hash exchange=$ex_hash trader=$tr_hash"; fi
if [ "${fills:-0}" -eq 0 ]; then fail "no fills"; fi
if [ "$DROP" -gt 0 ] && ! grep -q "recovered over TCP" "$LOG/trader.log"; then fail "no recovery"; fi
echo "OK: books match ($tr_hash), $fills fills"
