#!/usr/bin/env python3
import argparse, os, selectors, socket
p=argparse.ArgumentParser()
p.add_argument('--src-ipc',required=True); p.add_argument('--dst-ipc',required=True)
p.add_argument('--bind',required=True); p.add_argument('--peer',required=True); a=p.parse_args()

def hp(x):
    h,p=x.rsplit(':',1); return h,int(p)
try: os.unlink(a.src_ipc)
except FileNotFoundError: pass
ipc=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM); ipc.bind(a.src_ipc)
udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); udp.bind(hp(a.bind))
sel=selectors.DefaultSelector(); sel.register(ipc,selectors.EVENT_READ,'ipc'); sel.register(udp,selectors.EVENT_READ,'udp')
peer=hp(a.peer)
print(f'APP_F: IPC {a.src_ipc} <-> UDP {a.bind} <-> {a.peer} <-> IPC {a.dst_ipc}',flush=True)
while True:
    for key,_ in sel.select():
        if key.data=='ipc': udp.sendto(ipc.recv(65535),peer)
        else:
            pkt,_=udp.recvfrom(65535)
            try: ipc.sendto(pkt,a.dst_ipc)
            except FileNotFoundError: pass
