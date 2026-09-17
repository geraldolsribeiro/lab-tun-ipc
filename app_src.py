#!/usr/bin/env python3
import argparse, os, selectors, socket, struct, fcntl, time
TUNSETIFF=0x400454ca; IFF_TUN=0x0001; IFF_NO_PI=0x1000

def open_tun(name):
    fd=os.open('/dev/net/tun', os.O_RDWR)
    fcntl.ioctl(fd, TUNSETIFF, struct.pack('16sH', name.encode(), IFF_TUN|IFF_NO_PI))
    return fd

p=argparse.ArgumentParser(); p.add_argument('--tun', required=True); p.add_argument('--ipc', required=True); p.add_argument('--in-ipc', required=True); a=p.parse_args()
fd=open_tun(a.tun)
for path in (a.ipc, a.in_ipc):
    try: os.unlink(path)
    except FileNotFoundError: pass
out=socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
while True:
    try: out.connect(a.ipc); break
    except FileNotFoundError: time.sleep(.05)
inp=socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); inp.bind(a.in_ipc)
sel=selectors.DefaultSelector(); sel.register(fd, selectors.EVENT_READ, 'tun'); sel.register(inp, selectors.EVENT_READ, 'ipc')
print(f'APP_SRC: {a.tun} -> {a.ipc}; {a.in_ipc} -> {a.tun}', flush=True)
while True:
    for key, _ in sel.select():
        if key.data == 'tun': out.send(os.read(fd, 65535))
        else: os.write(fd, inp.recv(65535))
