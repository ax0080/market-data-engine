#!/bin/sh
# fread vs mmap replay of a raw ITCH file on Linux, interleaved so both see the
# same page-cache state. The first pass warms the cache and is not reported.
# usage: sh tools/io_compare.sh <replay_itch binary> <raw itch file> [rounds]
BIN=$1; FILE=$2; N=${3:-3}
"$BIN" "$FILE" --decode-only > /dev/null            # warm the page cache
i=0
while [ $i -lt "$N" ]; do
  "$BIN" "$FILE" --fread --decode-only
  "$BIN" "$FILE" --decode-only
  i=$((i + 1))
done
"$BIN" "$FILE" --fread
"$BIN" "$FILE"
