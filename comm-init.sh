#!/bin/bash
set -euo pipefail
SIDE="$1"
# Set APP_LANG=cpp to run the C++20 implementation; Python remains the default.
APP_LANG="${APP_LANG:-python}"
sysctl -w net.ipv4.ip_forward=1 >/dev/null
# Linux rp_filter can reject packets whose return path intentionally uses tun0.
sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null
sysctl -w net.ipv4.conf.default.rp_filter=0 >/dev/null
mkdir -p /run/comm
rm -f /run/comm/src_to_f.sock /run/comm/f_to_dst.sock /run/comm/dst_to_src.sock

if [ "$SIDE" = 5 ]; then
  LOCAL_LAN=10.5.0.0/24; REMOTE_LAN=10.6.0.0/24
  LOCAL_BB=10.100.0.5; REMOTE_BB=10.100.0.6
else
  LOCAL_LAN=10.6.0.0/24; REMOTE_LAN=10.5.0.0/24
  LOCAL_BB=10.100.0.6; REMOTE_BB=10.100.0.5
fi

# APP_SRC owns tun0. Packets routed to the remote LAN enter APP_SRC through tun0.
if [ "$APP_LANG" = cpp ]; then
  /opt/lab/apps_cpp src tun0 /run/comm/src_to_f.sock /run/comm/dst_to_src.sock &
else
  python3 /opt/lab/app_src.py --tun tun0 --ipc /run/comm/src_to_f.sock --in-ipc /run/comm/dst_to_src.sock &
fi
SRC_PID=$!
for i in {1..50}; do ip link show tun0 >/dev/null 2>&1 && break; sleep .1; done
ip link set tun0 up
ip route replace "$REMOTE_LAN" dev tun0

# APP_DST receives decapsulated IP packets and writes them to tun0.
if [ "$APP_LANG" = cpp ]; then
  /opt/lab/apps_cpp dst /run/comm/f_to_dst.sock /run/comm/dst_to_src.sock &
else
  python3 /opt/lab/app_dst.py --ipc /run/comm/f_to_dst.sock --out-ipc /run/comm/dst_to_src.sock &
fi
DST_PID=$!

# APP_F bridges local Unix IPC to UDP backbone, and UDP back to local APP_DST IPC.
if [ "$APP_LANG" = cpp ]; then
  /opt/lab/apps_cpp f /run/comm/src_to_f.sock /run/comm/f_to_dst.sock "$LOCAL_BB:5000" "$REMOTE_BB:5000" &
else
  python3 /opt/lab/app_f.py \
    --src-ipc /run/comm/src_to_f.sock \
    --dst-ipc /run/comm/f_to_dst.sock \
    --bind "$LOCAL_BB:5000" --peer "$REMOTE_BB:5000" \
    --payload "${APP_F_PAYLOAD:-1600}" &
fi
F_PID=$!

trap 'kill $SRC_PID $DST_PID $F_PID 2>/dev/null || true' TERM INT
wait -n $SRC_PID $DST_PID $F_PID
