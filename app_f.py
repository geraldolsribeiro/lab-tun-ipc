#!/usr/bin/env python3
"""APP_F packet framer and UDP bridge.

UDP is required to carry exactly one configurable payload size on every send.
IP packets may be smaller, larger, or several packets may fit in one payload,
so a 4-byte network-order length prefixes every packet in a byte stream.
Records may cross payload boundaries; the receiver buffers bytes and emits each
complete record in original order. Zero bytes at the end are padding.
"""
import argparse, os, selectors, socket, struct

MIN_PAYLOAD = 28
MAX_PAYLOAD = 11200
HEADER_SIZE = 4
FRAME_HEADER_SIZE = 6       # Four-byte magic plus two-byte used-byte count.
FRAME_MAGIC = b'AF20'
MAX_PACKET = 65535

p = argparse.ArgumentParser()
p.add_argument('--src-ipc', required=True); p.add_argument('--dst-ipc', required=True)
p.add_argument('--bind', required=True); p.add_argument('--peer', required=True)
p.add_argument('--payload', type=int, default=int(os.getenv('APP_F_PAYLOAD', '1600')))
a = p.parse_args()
if not MIN_PAYLOAD <= a.payload <= MAX_PAYLOAD:
    raise SystemExit(f'payload must be {MIN_PAYLOAD}..{MAX_PAYLOAD} bytes')

def hp(x):
    h, port = x.rsplit(':', 1); return h, int(port)
try: os.unlink(a.src_ipc)
except FileNotFoundError: pass
ipc = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); ipc.bind(a.src_ipc)
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); udp.bind(hp(a.bind))
peer = hp(a.peer)
sel = selectors.DefaultSelector(); sel.register(ipc, selectors.EVENT_READ, 'ipc'); sel.register(udp, selectors.EVENT_READ, 'udp')
pending = bytearray()       # Complete records waiting for a fixed-size UDP payload.
received = bytearray()      # Ordered record stream received from the peer.

def emit_payloads():
    """Send as many exactly-sized payloads as currently possible."""
    capacity = a.payload - FRAME_HEADER_SIZE
    while len(pending) >= capacity:
        chunk = bytes(pending[:capacity]); del pending[:capacity]
        udp.sendto((FRAME_MAGIC + struct.pack('!H', len(chunk)) + chunk).ljust(a.payload, b'\0'), peer)

def consume_payload(data):
    """Decode a fixed frame, then records split across frame boundaries."""
    if len(data) != a.payload or data[:4] != FRAME_MAGIC:
        raise RuntimeError('invalid fixed-size APP_F frame')
    used = struct.unpack('!H', data[4:6])[0]
    if used > a.payload - FRAME_HEADER_SIZE:
        raise RuntimeError('invalid APP_F used length')
    received.extend(data[FRAME_HEADER_SIZE:FRAME_HEADER_SIZE + used])
    while True:
        # Padding is excluded by the frame's `used` count, so it must not be
        # removed here. In particular, packet lengths commonly begin with
        # zero bytes; treating every leading zero as padding would corrupt the
        # four-byte length prefix.
        if len(received) < HEADER_SIZE: return
        size = struct.unpack('!I', received[:HEADER_SIZE])[0]
        if not HEADER_SIZE <= size <= MAX_PACKET:
            raise RuntimeError(f'invalid framed packet length: {size}')
        if len(received) < HEADER_SIZE + size: return
        packet = bytes(received[HEADER_SIZE:HEADER_SIZE + size])
        del received[:HEADER_SIZE + size]
        ipc.sendto(packet, a.dst_ipc)

print(f'APP_F: fixed UDP payload={a.payload} bytes', flush=True)
while True:
    # A short timeout flushes a partial aggregate with zero padding, while
    # allowing packets arriving together to share one payload.
    events = sel.select(.01)
    if not events and pending:
        chunk = bytes(pending)
        udp.sendto((FRAME_MAGIC + struct.pack('!H', len(chunk)) + chunk).ljust(a.payload, b'\0'),
                   peer)
        pending.clear()
    for key, _ in events:
        if key.data == 'ipc':
            packet = ipc.recv(65535)
            if len(packet) > MAX_PACKET: raise RuntimeError('packet too large')
            pending.extend(struct.pack('!I', len(packet))); pending.extend(packet)
            emit_payloads()
            # Flush immediately when a packet does not fill a frame. This
            # avoids introducing a timer-based latency penalty for low-rate
            # traffic such as ping; bursts still aggregate up to one frame.
            if pending:
                chunk = bytes(pending); pending.clear()
                udp.sendto((FRAME_MAGIC + struct.pack('!H', len(chunk)) + chunk).ljust(a.payload, b'\0'), peer)
        else:
            consume_payload(udp.recv(a.payload))
