#!/usr/bin/env python3
"""APP_F: the transport bridge between the two COMM containers.

Unix-domain datagrams provide local IPC without exposing a listening TCP port.
The backbone uses UDP datagrams: each received IP packet remains one UDP
payload, so packet boundaries are preserved.  UDP is intentionally simple and
has no delivery, ordering, or congestion guarantees; production tunnels need
sequence numbers, loss handling, authentication, and MTU policy.
"""
import argparse, os, selectors, socket

p = argparse.ArgumentParser()
p.add_argument('--src-ipc', required=True)
p.add_argument('--dst-ipc', required=True)
p.add_argument('--bind', required=True)
p.add_argument('--peer', required=True)
a = p.parse_args()


def host_port(value):
    host, port = value.rsplit(':', 1)
    return host, int(port)

try: os.unlink(a.src_ipc)
except FileNotFoundError: pass
# APP_SRC sends complete IP packets to this local Unix datagram endpoint.
ipc = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
ipc.bind(a.src_ipc)
# The COMM backbone is a separate Docker network; UDP crosses only that link.
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.bind(host_port(a.bind))
peer = host_port(a.peer)
sel = selectors.DefaultSelector()
sel.register(ipc, selectors.EVENT_READ, 'ipc')
sel.register(udp, selectors.EVENT_READ, 'udp')
print(f'APP_F: {a.src_ipc} <-> UDP {a.bind} <-> {a.peer} <-> IPC {a.dst_ipc}', flush=True)
while True:
    for key, _ in sel.select():
        if key.data == 'ipc':
            # Local packet -> encapsulate as one UDP datagram to remote APP_F.
            udp.sendto(ipc.recv(65535), peer)
        else:
            # Remote UDP packet -> local APP_DST for injection toward the PC.
            packet, _ = udp.recvfrom(65535)
            try: ipc.sendto(packet, a.dst_ipc)
            except FileNotFoundError: pass  # APP_DST may still be starting.
