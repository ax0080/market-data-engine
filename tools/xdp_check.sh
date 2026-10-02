#!/bin/sh
# Report whether this Linux kernel / box can run AF_XDP (used by udp_bench --xdp).
echo "kernel: $(uname -r)"
CFG=""
if [ -r /proc/config.gz ]; then CFG=$(zcat /proc/config.gz); elif [ -r "/boot/config-$(uname -r)" ]; then CFG=$(cat "/boot/config-$(uname -r)"); fi
if [ -n "$CFG" ]; then
  echo "$CFG" | grep -E 'CONFIG_XDP_SOCKETS=|CONFIG_BPF_SYSCALL=|CONFIG_BPF_JIT=|CONFIG_XDP_SOCKETS_DIAG='
else
  echo "kernel config not readable"
fi
for h in /usr/include/linux/if_xdp.h /usr/include/linux/bpf.h /usr/include/bpf/libbpf.h /usr/include/xdp/xsk.h; do
  [ -e "$h" ] && echo "header present: $h" || echo "header missing: $h"
done
for t in clang llc bpftool ip; do
  command -v "$t" >/dev/null 2>&1 && echo "tool present: $t" || echo "tool missing: $t"
done
grep -E '^[^#]*xdp' /proc/net/ptype 2>/dev/null | head -2
# Probe: can we open an AF_XDP socket (needs root)?
python3 - <<'EOF'
import socket
try:
    s = socket.socket(44, socket.SOCK_RAW, 0)   # AF_XDP = 44
    print("AF_XDP socket: OK")
    s.close()
except OSError as e:
    print("AF_XDP socket:", e)
EOF
