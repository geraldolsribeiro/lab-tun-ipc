#!/usr/bin/env python3
import argparse, os, socket, time
p=argparse.ArgumentParser(); p.add_argument('--ipc', required=True); p.add_argument('--out-ipc', required=True); a=p.parse_args()
try: os.unlink(a.ipc)
except FileNotFoundError: pass
s=socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.bind(a.ipc)
out=socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
while True:
    try: out.connect(a.out_ipc); break
    except FileNotFoundError: time.sleep(.05)
print(f'APP_DST: {a.ipc} -> {a.out_ipc}', flush=True)
while True: out.send(s.recv(65535))
