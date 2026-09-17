#!/usr/bin/env python3
"""APP_SRC: the COMM-side IP-packet ingress/egress adapter.

A TUN device is a virtual layer-3 (IP) network interface.  The Linux kernel
routes packets for the remote LAN to tun0, and reading tun0 gives us complete
IP packets (not TCP byte streams).  We send those packets to APP_F over a
Unix-domain datagram socket.  Packets coming back from APP_DST are written to
tun0, where the kernel performs normal IP forwarding to the local PC.

Only this process opens tun0.  Keeping one TUN owner avoids competing readers
and demonstrates the clean separation between the TUN and application layers.
"""
import argparse, os, selectors, socket, struct, fcntl, time

TUNSETIFF = 0x400454ca
IFF_TUN = 0x0001       # Create a layer-3 TUN device (rather than an Ethernet TAP).
IFF_NO_PI = 0x1000    # Do not prepend Linux's extra packet-information header.


def open_tun(name):
    # /dev/net/tun is the kernel interface used to create/access a TUN device.
    fd = os.open('/dev/net/tun', os.O_RDWR)
    request = struct.pack('16sH', name.encode(), IFF_TUN | IFF_NO_PI)
    fcntl.ioctl(fd, TUNSETIFF, request)
    return fd


p = argparse.ArgumentParser()
p.add_argument('--tun', required=True)
p.add_argument('--ipc', required=True, help='APP_F input socket')
p.add_argument('--in-ipc', required=True, help='APP_DST return socket')
a = p.parse_args()

fd = open_tun(a.tun)
# APP_F creates its receiving socket slightly later, so retry the connection.
for path in (a.ipc, a.in_ipc):
    try: os.unlink(path)
    except FileNotFoundError: pass
out = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
while True:
    try:
        out.connect(a.ipc)
        break
    except FileNotFoundError:
        time.sleep(.05)
# This socket is bound by APP_SRC and receives packets returning from APP_DST.
inp = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
inp.bind(a.in_ipc)

# select() lets one event loop handle both kernel packets and IPC packets.
sel = selectors.DefaultSelector()
sel.register(fd, selectors.EVENT_READ, 'tun')
sel.register(inp, selectors.EVENT_READ, 'ipc')
print(f'APP_SRC: {a.tun} -> {a.ipc}; {a.in_ipc} -> {a.tun}', flush=True)
while True:
    for key, _ in sel.select():
        if key.data == 'tun':
            # Outbound packet: kernel -> TUN -> APP_SRC -> APP_F.
            out.send(os.read(fd, 65535))
        else:
            # Inbound packet: APP_DST -> APP_SRC -> TUN -> kernel -> PC.
            os.write(fd, inp.recv(65535))
