# AGENTS.md

## Objective

Implement a minimal bidirectional Layer-3 tunnel in modern C++20 using Linux TUN interfaces.

The tunnel connects two otherwise independent IPv4 networks:

```text
10.5.0.0/24                                      10.6.0.0/24

 PC_5                    CMM_5                 CMM_6                    PC_6
10.5.0.2              LAN 10.5.0.1          LAN 10.6.0.1              10.6.0.2
   |                        |                     |                        |
   +-------- lan5 ----------+                     +---------- lan6 ----------+
                            |                     |
                         tun0                   tun0
                            |                     |
                 APP_SRC / APP_F / APP_DST on both CMM containers
                            |                     |
             backbone/eno1 10.100.0.5 <-> 10.100.0.6 backbone/eno1
                            UDP fixed-size transport
```

`tun0` is the application-facing Layer-3 interface inside each CMM container.
It is not an Ethernet interface. Its IP address is optional and is not used
for CMM-to-CMM transport. The tunnel must work with an unaddressed TUN.
Linux routes remote-LAN
packets from the LAN veth to `tun0`; the applications read and write packets
there. APP_F then carries those packets as fixed-size UDP payloads through the
CMM backbone interface (`eno1` in the conceptual diagram). Thus TUN is between
the Linux routing stack and the applications, while the backbone Ethernet
interface is used by APP_F for inter-CMM transport.

Target test:

```bash
# PC_6
iperf -s

# PC_5
iperf -c 10.6.0.2
```

Traffic must preserve the original addresses:

```text
source      10.5.0.2
destination 10.6.0.2
```

The reverse direction must work as well.

---

## Transport Architecture Constraint

The production tunnel should expose an abstract/custom transport interface. For
this Docker lab, that transport is implemented using UDP over a dedicated,
isolated backbone network. Only CMM containers attach to the backbone; PC
containers have no interface or route on it.

The backbone is an implementation detail of the lab transport, not a network
available to the endpoint PCs. Do not NAT, bridge, or expose it to PC_5 or PC_6.

Each CMM container runs all three applications:

```text
CMM_5: APP_SRC <-> APP_F <-> UDP backbone <-> APP_F <-> APP_DST
CMM_6: APP_SRC <-> APP_F <-> UDP backbone <-> APP_F <-> APP_DST
```

The tunnel is bidirectional. On either side, APP_SRC reads locally routed
packets from TUN and sends them through the local APP_F. The remote APP_F
passes them to APP_DST, which injects them into the local Linux stack through
TUN. Return traffic follows the same path in reverse.

The application transports opaque IP packets. It must not modify addresses,
perform NAT, or parse TCP/UDP application payloads.

Conceptually for PC_5 -> PC_6:

```text
Linux CMM_5 -> tun0 -> APP_SRC -> APP_F
           -> UDP backbone -> APP_F -> APP_DST -> APP_SRC -> tun0 -> Linux CMM_6
```

PC_6 -> PC_5 uses the identical architecture in reverse.

Backbone network:

```text
10.100.0.0/24
CMM_5: 10.100.0.5
CMM_6: 10.100.0.6
UDP port: 5000
```

---

# Linux Networking Model

## CMM_5

Physical interface:

```text
eno1
10.5.0.1/24
```

TUN:

```text
tun0
(unaddressed; optional diagnostic address)
MTU initially 1500
```

Route:

```bash
ip route replace 10.6.0.0/24 dev tun0
```

Enable forwarding:

```bash
sysctl -w net.ipv4.ip_forward=1
```

Expected packet flow:

```text
PC_5
 |
eno1
 |
Linux IP stack
 |
routing
 |
tun0
 |
APP_SRC/APP_F/APP_DST on CMM_5
```

---

## CMM_6

Physical interface:

```text
eno1
10.6.0.1/24
```

TUN:

```text
tun0
(unaddressed; optional diagnostic address)
MTU initially 1500
```

Route:

```bash
ip route replace 10.5.0.0/24 dev tun0
```

Enable forwarding:

```bash
sysctl -w net.ipv4.ip_forward=1
```

Expected packet flow:

```text
APP_SRC/APP_F/APP_DST on CMM_6
 |
tun0
 |
Linux IP stack
 |
routing
 |
eno1
 |
PC_6
```

---

# PC Configuration

## PC_5

```text
IP: 10.5.0.2/24
```

Route:

```bash
ip route replace 10.6.0.0/24 via 10.5.0.1
```

## PC_6

```text
IP: 10.6.0.2/24
```

Route:

```bash
ip route replace 10.5.0.0/24 via 10.6.0.1
```

No NAT should be used.

---

# TUN Semantics

Use `/dev/net/tun` with:

```text
IFF_TUN | IFF_NO_PI
```

`IFF_TUN` is mandatory because the application transports Layer-3 IP packets, not Ethernet frames.

Do NOT use TAP.

With `IFF_NO_PI`, a read from the TUN file descriptor returns an IP packet beginning directly with the IPv4 header.

Example:

```text
+-----------------------------+
| IPv4                        |
| src = 10.5.0.2        |
| dst = 10.6.0.2        |
+-----------------------------+
| TCP                         |
+-----------------------------+
| iperf payload               |
+-----------------------------+
```

There is no Ethernet header.

---

# TUN Direction Semantics

This distinction is important.

When Linux routes a packet to `tun0`:

```text
Linux -> tun0 -> application
```

the application obtains it with:

```cpp
read(tun_fd, buffer, size);
```

When the application receives a packet from the remote CMM container:

```text
remote transport -> application -> tun0 -> Linux
```

the application injects it using:

```cpp
write(tun_fd, buffer, size);
```

Writing to `tun0` means:

> Inject this IP packet into the Linux networking stack as a packet received from this virtual interface.

Do NOT attempt to write the received packet directly to `eno1`.

Linux must remain responsible for:

* IP routing
* ARP
* Ethernet framing
* forwarding
* neighbor discovery
* physical interface management

The tunnel application only transports IP packets.

---

# Application Architecture

Separate TUN handling from transport handling.

Recommended abstractions:

```cpp
class TunDevice;
class Transport;
class Tunnel;
```

## TunDevice

Responsibilities:

* open `/dev/net/tun`
* create/attach `tun0`
* configure `IFF_TUN | IFF_NO_PI`
* expose file descriptor
* read IP packets
* write IP packets

The TUN IP address is optional and must not be required for packet transport.
If configured for diagnostics or a future control plane, do not use it as a
CMM-to-CMM transport endpoint.

Prefer Linux networking configuration in shell scripts rather than embedding `ip` or `sysctl` commands into the application.

---

## Transport

Define an interface independent of the actual CMM_5/CMM_6 transport.

For example:

```cpp
class Transport {
public:
    virtual ~Transport() = default;

    virtual bool send(std::span<const std::byte> packet) = 0;

    virtual std::optional<std::vector<std::byte>> receive() = 0;
};
```

The TUN packet-routing layer must remain independent of the transport
implementation. APP_F may use UDP, IP addresses, and Ethernet because those
belong to the isolated CMM backbone. PC containers must not access the backbone
directly. The tunnel must not perform NAT or modify packet addresses.

The UDP transport can be replaced independently without changing TUN handling.

---

# Application Transport

APP_SRC, APP_F, and APP_DST run inside each CMM container. APP_SRC and APP_F,
and APP_F and APP_DST, communicate through Unix-domain datagram sockets. These
sockets are local IPC only and never connect the two CMM containers.

APP_F communicates between CMM_5 and CMM_6 using UDP over the isolated
10.100.0.0/24 backbone. The UDP payload has a configurable fixed size from
28 bytes through 11,200 bytes. The framing layer must:

* preserve packet order
* support multiple IP packets in one payload
* split a packet across payloads when necessary
* reassemble packets before writing them to TUN
* pad every UDP datagram to the configured size
* reject malformed frames and unreasonable packet lengths

The packet-routing code should remain independent from APP_F so a future custom
transport can replace UDP without changing TUN handling.

---

# Packet Boundaries

Packet boundaries MUST be preserved.

One read from TUN represents one IP packet:

```text
TUN packet N
      |
      v
transport message N
      |
      v
remote TUN packet N
```

Do not concatenate arbitrary IP packets into a byte stream without framing.

If the underlying transport is stream-oriented, introduce explicit framing:

```text
+------------------+
| packet length    |
+------------------+
| IP packet        |
+------------------+
```

Use a fixed-width integer for packet length with a documented byte order.

Reject malformed or unreasonably large lengths.

---

# Packet Size

Support at least:

```text
65535 bytes
```

internally so the code does not depend on Ethernet's 1500-byte MTU.

Initial TUN MTU:

```text
1500
```

Do not arbitrarily reduce it to 1400.

The production transport determines whether fragmentation is required.

If the production transport cannot carry a complete TUN packet atomically, fragmentation/reassembly belongs in the transport layer, not in `TunDevice`.

---

# Main Packet Flow

Conceptually the application needs two independent flows.

## Local -> Remote

```cpp
while (running) {
    auto packet = tun.read_packet();

    if (!packet.empty()) {
        transport.send(packet);
    }
}
```

## Remote -> Local

```cpp
while (running) {
    auto packet = transport.receive();

    if (packet) {
        tun.write_packet(*packet);
    }
}
```

Both directions must operate concurrently.

A simple implementation may use two `std::jthread`s.

Avoid unnecessary complexity initially.

---

# Concurrency

Initial implementation:

```text
Thread 1:
    tun.read()
       ->
    transport.send()

Thread 2:
    transport.receive()
       ->
    tun.write()
```

Use:

```cpp
std::jthread
std::stop_token
```

where practical.

Ensure clean shutdown on:

```text
SIGINT
SIGTERM
```

Do not introduce a large asynchronous framework unless justified.

---

# Packet Validation

The tunnel should primarily treat packets as opaque data.

However, before injecting a packet into TUN, perform minimal sanity checking:

* non-empty
* large enough to contain an IPv4 header
* IPv4 version field is 4
* IPv4 total length is consistent with the received buffer
* packet size does not exceed configured maximum

Do NOT:

* modify IP addresses
* modify TCP/UDP ports
* perform NAT
* recalculate transport checksums unnecessarily
* parse application payloads

The original IP packet should arrive unchanged at the opposite Linux network stack.

---

# Privileges

Opening/configuring TUN generally requires:

```text
CAP_NET_ADMIN
```

Development may run the application with:

```bash
sudo
```

Do not require the application to run permanently as root if capabilities can be used instead.

---

# Setup Scripts

The current Docker implementation uses these scripts:

```text
pc-init.sh   # configures the PC container's default route
cmm-init.sh  # configures CMM forwarding, TUN, routes, and applications
```

Docker Compose passes the side number to `cmm-init.sh`, so the same script
configures both CMM_5 and CMM_6. The PC containers pass their local address and
CMM gateway to `pc-init.sh`.

CMM_5 setup:

```bash
#!/bin/bash
set -euo pipefail

ip link set tun0 mtu 1500 up
ip route replace 10.6.0.0/24 dev tun0

sysctl -w net.ipv4.ip_forward=1
```

CMM_6:

```bash
#!/bin/bash
set -euo pipefail

ip link set tun0 mtu 1500 up
ip route replace 10.5.0.0/24 dev tun0

sysctl -w net.ipv4.ip_forward=1
```

PC_5:

```bash
#!/bin/bash
set -euo pipefail

ip route replace 10.6.0.0/24 via 10.5.0.1
```

PC_6:

```bash
#!/bin/bash
set -euo pipefail

ip route replace 10.5.0.0/24 via 10.6.0.1
```

Account for the fact that `tun0` must exist before the CMM setup script configures it.

If useful, separate TUN creation from route configuration.

---

# Diagnostics

Add optional diagnostic logging.

For every packet, debug mode may report:

```text
RX TUN  len=1500 src=10.5.0.2 dst=10.6.0.2
TX LINK len=1500

RX LINK len=52
TX TUN  len=52 src=10.6.0.2 dst=10.5.0.2
```

Do not log every packet by default because this will significantly affect iperf throughput.

Support at least:

```text
--verbose
```

for diagnostics.

---

# Testing Strategy

Testing must be incremental.

## Test 1 — TUN creation

Verify:

```bash
ip link show tun0
```

Expected:

```text
tun0 UP
```

No IP address should be assigned.

---

## Test 2 — Routing

CMM_5:

```bash
ip route get 10.6.0.2
```

must select:

```text
dev tun0
```

CMM_6:

```bash
ip route get 10.5.0.2
```

must select:

```text
dev tun0
```

---

## Test 3 — Observe packets

Use:

```bash
tcpdump -ni tun0
```

On CMM_5, attempting:

```bash
ping 10.6.0.2
```

from PC_5 should result in IP packets appearing on CMM_5 `tun0`.

---

## Test 4 — End-to-end ping

Once APP_SRC/APP_F/APP_DST on CMM_5/APP_SRC/APP_F/APP_DST on CMM_6 transport is connected:

```bash
PC_5$ ping 10.6.0.2
```

must succeed.

Verify both CMM containers:

```bash
tcpdump -ni tun0 icmp
```

---

## Test 5 — iperf

PC_6:

```bash
iperf -s
```

PC_5:

```bash
iperf -c 10.6.0.2
```

Then test the reverse direction.

---

# Important Non-Goals

Do NOT implement:

* NAT
* IPsec
* GRE
* WireGuard
* OpenVPN
* a TCP application proxy between CMM_5/CMM_6
* exposing the backbone network to PC_5 or PC_6
* TAP/Ethernet bridging
* ARP forwarding across the tunnel
* IP addresses on `tun0`
* an IP subnet between CMM_5 and CMM_6

The purpose of this project is to transport opaque IP packets through APP_SRC, APP_F, and APP_DST over the isolated UDP backbone.

---

# Build System

Use a plain GNU Makefile rather than CMake.

The Makefile should provide targets for:

* building the C++20 implementation
* rebuilding Docker images
* starting and stopping the lab
* running ping and iperf tests
* capturing backbone traffic with tcpdump
* cleaning generated images, containers, and networks

Minimum:

```text
C++20
Linux
GNU Make
GCC/Clang
```

Keep dependencies minimal. Prefer Linux/POSIX APIs and the C++ standard
library. Do not add Boost unless a concrete requirement makes it necessary.
The Docker build may install the compiler and required Linux networking tools.

---

# Implementation Priorities

Implement in this order:

1. `TunDevice`
2. abstract `Transport`
3. Unix-domain test transport
4. bidirectional `Tunnel`
5. graceful shutdown
6. setup scripts
7. packet diagnostics
8. unit tests
9. local integration test
10. iperf end-to-end test documentation

Keep each stage independently testable.

Keep the packet-routing path separate from the UDP backbone implementation so the transport can later be replaced without changing TUN handling.

---

# Definition of Done

The implementation is complete when the following path works without NAT:

```text
iperf client
10.5.0.2

       |
       v

CMM_5 eno1
10.5.0.1

       |
       v

Linux routing

       |
       v

tun0
       |
       v
APP_SRC/APP_F/APP_DST on CMM_5
       |
       | opaque/custom transport
       v
APP_SRC/APP_F/APP_DST on CMM_6
       |
       v
tun0

       |
       v

Linux routing

       |
       v

CMM_6 eno1
10.6.0.1

       |
       v

iperf server
10.6.0.2
```

The TCP connection observed by PC_6 must retain:

```text
source = 10.5.0.2
```

and the reverse packets must return through the same tunnel architecture.

The application uses the dedicated 10.100.0.0/24 backbone between CMM_5 and CMM_6; PC_5 and PC_6 must not be attached to or route through that backbone.

The packet-routing code should remain separated from the APP_F transport code so the UDP backbone transport can later be replaced without changing TUN handling.

