#!/usr/bin/env python3
"""APP_DST: the final IPC hop on the receiving CMM side.

APP_DST is deliberately an application boundary: it receives a complete IP
packet from APP_F and passes it to APP_SRC, whose single TUN file descriptor
writes it into the Linux kernel.  The kernel then routes the packet onto the
local LAN toward the destination PC.
"""
import argparse, os, socket, time

p = argparse.ArgumentParser()
p.add_argument('--ipc', required=True, help='APP_F output socket')
p.add_argument('--out-ipc', required=True, help='APP_SRC injection socket')
a = p.parse_args()
try: os.unlink(a.ipc)
except FileNotFoundError: pass
incoming = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
incoming.bind(a.ipc)
outgoing = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
while True:
    try:
        outgoing.connect(a.out_ipc)
        break
    except FileNotFoundError:
        time.sleep(.05)
print(f'APP_DST: {a.ipc} -> {a.out_ipc}', flush=True)
while True:
    # APP_F -> APP_DST -> APP_SRC -> TUN -> Linux routing -> destination PC.
    outgoing.send(incoming.recv(65535))
