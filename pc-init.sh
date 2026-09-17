#!/bin/bash
set -euo pipefail
IP="$1"; GW="$2"
# Docker installs a connected route; replace Docker's default route with COMM_x.
ip route del default 2>/dev/null || true
ip route add default via "$GW"
echo "$(hostname): default gateway -> $GW"
exec sleep infinity
