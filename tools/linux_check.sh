#!/bin/sh
# Build every target with the system g++ (no CMake needed) and run the tests and
# the UDP smoke test -- the same checks CI runs. Usage: sh tools/linux_check.sh
set -e
mkdir -p build-linux
FLAGS="-std=c++20 -O3 -march=native -Wall -Wextra -Wpedantic -Iinclude"
for f in tools/replay_itch.cpp tools/replay_crypto.cpp tools/udp_bench.cpp; do
  g++ $FLAGS "$f" -o "build-linux/$(basename "$f" .cpp)" -pthread
done
g++ $FLAGS tests/test_main.cpp tests/test_books.cpp tests/test_feeds.cpp -o build-linux/mde_tests
./build-linux/mde_tests | tail -1
python3 tools/make_synthetic_itch.py build-linux/synthetic.itch 200000
./build-linux/udp_bench build-linux/synthetic.itch 200000 recvmmsg 100000
