#!/bin/sh
# Record the environment a network benchmark ran on (kernel, CPU, NIC driver,
# queues, MTU, XDP features). Usage: sh gcp_env.sh <ifname>
IF=${1:-ens4}
echo "== host";    hostname; uname -r; . /etc/os-release && echo "$PRETTY_NAME"
echo "== cpu";     lscpu | grep -E 'Model name|^CPU\(s\)|Thread|NUMA node\(s\)'
echo "== nic";     ethtool -i "$IF" 2>&1 | grep -E 'driver|version|firmware|bus-info'
echo "== queues";  ethtool -l "$IF" 2>&1
echo "== link";    ip -d link show "$IF" | head -3
echo "== mtu";     cat "/sys/class/net/$IF/mtu"
echo "== xdp features (if exposed)"
ls "/sys/class/net/$IF/" | grep -i xdp || echo "(not in sysfs)"
