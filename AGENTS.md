# AGENTS.md

## Objective

Implement a minimal bidirectional Layer-3 tunnel in modern C++20 using Linux TUN interfaces.

The tunnel connects two otherwise independent IPv4 networks:

```text
192.168.105.0/24                              192.168.106.0/24

 PC5                 FPU5                      FPU6                 PC6
 .105.50             .105.5                    .106.6               .106.60
    |                   |                         |                    |
    +------ eno1 --------+                         +------ eno1 --------+
                        |                         |
                    Linux routing             Linux routing
                        |                         |
                       tun0                      tun0
                        |                         |
                      APP5 ===================== APP6
                            custom transport
```

Target test:

```bash
# PC6
iperf -s

# PC5
iperf -c 192.168.106.60
```

Traffic must preserve the original addresses:

```text
source      192.168.105.50
destination 192.168.106.60
```

The reverse direction must work as well.

---

## Critical Architecture Constraint

There MUST NOT be an IP network between FPU5 and FPU6.

Do NOT implement:

```text
FPU5 tun0 = 10.0.0.5
FPU6 tun0 = 10.0.0.6
```

Do NOT create an IP subnet for the TUN endpoints.

Do NOT assume APP5 can reach APP6 using TCP, UDP, IP routing, ARP, or any other IP-based mechanism.

The transport between APP5 and APP6 is an abstract/custom non-IP transport.

The application transports opaque IP packets.

Conceptually:

```text
Linux FPU5
    |
    | route 192.168.106.0/24 dev tun0
    v
  tun0
    |
    | read()
    v
  APP5
    |
    | opaque non-IP transport
    v
  APP6
    |
    | write()
    v
  tun0
    |
    v
Linux FPU6
    |
    | normal routing
    v
  eno1
    |
    v
 PC6
```

The reverse path must be symmetrical.

---

# Linux Networking Model

## FPU5

Physical interface:

```text
eno1
192.168.105.5/24
```

TUN:

```text
tun0
NO IP ADDRESS
MTU initially 1500
```

Route:

```bash
ip route replace 192.168.106.0/24 dev tun0
```

Enable forwarding:

```bash
sysctl -w net.ipv4.ip_forward=1
```

Expected packet flow:

```text
PC5
 |
eno1
 |
Linux IP stack
 |
routing
 |
tun0
 |
APP5
```

---

## FPU6

Physical interface:

```text
eno1
192.168.106.6/24
```

TUN:

```text
tun0
NO IP ADDRESS
MTU initially 1500
```

Route:

```bash
ip route replace 192.168.105.0/24 dev tun0
```

Enable forwarding:

```bash
sysctl -w net.ipv4.ip_forward=1
```

Expected packet flow:

```text
APP6
 |
tun0
 |
Linux IP stack
 |
routing
 |
eno1
 |
PC6
```

---

# PC Configuration

## PC5

```text
IP: 192.168.105.50/24
```

Route:

```bash
ip route replace 192.168.106.0/24 via 192.168.105.5
```

## PC6

```text
IP: 192.168.106.60/24
```

Route:

```bash
ip route replace 192.168.105.0/24 via 192.168.106.6
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
| src = 192.168.105.50        |
| dst = 192.168.106.60        |
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

When the application receives a packet from the remote FPU:

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

Do not configure IP addresses on `tun0`.

Prefer Linux networking configuration in shell scripts rather than embedding `ip` or `sysctl` commands into the application.

---

## Transport

Define an interface independent of the actual FPU5/FPU6 transport.

For example:

```cpp
class Transport {
public:
    virtual ~Transport() = default;

    virtual bool send(std::span<const std::byte> packet) = 0;

    virtual std::optional<std::vector<std::byte>> receive() = 0;
};
```

The tunnel implementation MUST NOT depend on TCP, UDP, IP addresses, or Ethernet.

The real transport will be supplied/replaced independently.

---

# Initial Development Transport

For development and unit/integration testing, implement a transport that can run locally without changing the tunnel architecture.

Suitable choices include:

```text
Unix domain SOCK_SEQPACKET
```

or another message-preserving local IPC mechanism.

Prefer `AF_UNIX + SOCK_SEQPACKET`.

This is only a development/test transport.

Do not design the core tunnel around Unix sockets.

The production transport remains abstract and non-IP.

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

Create:

```text
scripts/setup-pc5.sh
scripts/setup-fpu5.sh
scripts/setup-fpu6.sh
scripts/setup-pc6.sh
```

FPU5 setup:

```bash
#!/bin/bash
set -euo pipefail

ip link set tun0 mtu 1500 up
ip route replace 192.168.106.0/24 dev tun0

sysctl -w net.ipv4.ip_forward=1
```

FPU6:

```bash
#!/bin/bash
set -euo pipefail

ip link set tun0 mtu 1500 up
ip route replace 192.168.105.0/24 dev tun0

sysctl -w net.ipv4.ip_forward=1
```

PC5:

```bash
#!/bin/bash
set -euo pipefail

ip route replace 192.168.106.0/24 via 192.168.105.5
```

PC6:

```bash
#!/bin/bash
set -euo pipefail

ip route replace 192.168.105.0/24 via 192.168.106.6
```

Account for the fact that `tun0` must exist before the FPU setup script configures it.

If useful, separate TUN creation from route configuration.

---

# Diagnostics

Add optional diagnostic logging.

For every packet, debug mode may report:

```text
RX TUN  len=1500 src=192.168.105.50 dst=192.168.106.60
TX LINK len=1500

RX LINK len=52
TX TUN  len=52 src=192.168.106.60 dst=192.168.105.50
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

FPU5:

```bash
ip route get 192.168.106.60
```

must select:

```text
dev tun0
```

FPU6:

```bash
ip route get 192.168.105.50
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

On FPU5, attempting:

```bash
ping 192.168.106.60
```

from PC5 should result in IP packets appearing on FPU5 `tun0`.

---

## Test 4 — End-to-end ping

Once APP5/APP6 transport is connected:

```bash
PC5$ ping 192.168.106.60
```

must succeed.

Verify both FPUs:

```bash
tcpdump -ni tun0 icmp
```

---

## Test 5 — iperf

PC6:

```bash
iperf -s
```

PC5:

```bash
iperf -c 192.168.106.60
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
* TCP tunnel between FPU5/FPU6
* UDP tunnel between FPU5/FPU6
* TAP/Ethernet bridging
* ARP forwarding across the tunnel
* IP addresses on `tun0`
* an IP subnet between FPU5 and FPU6

The purpose of this project is specifically to transport IP packets through an existing/custom non-IP FPU-to-FPU application transport.

---

# Build System

Use CMake.

Minimum:

```text
C++20
Linux
GCC/Clang
```

Keep dependencies minimal.

Prefer Linux/POSIX APIs and the C++ standard library.

Do not add Boost unless a concrete requirement makes it necessary.

Suggested layout:

```text
.
├── AGENTS.md
├── CMakeLists.txt
├── README.md
├── src
│   ├── main.cpp
│   ├── tun_device.cpp
│   ├── tun_device.hpp
│   ├── tunnel.cpp
│   ├── tunnel.hpp
│   ├── transport.hpp
│   ├── unix_transport.cpp
│   └── unix_transport.hpp
├── tests
└── scripts
    ├── setup-pc5.sh
    ├── setup-fpu5.sh
    ├── setup-fpu6.sh
    └── setup-pc6.sh
```

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

Do not implement the production FPU-to-FPU transport until the TUN packet path has been demonstrated using the test transport.

---

# Definition of Done

The implementation is complete when the following path works without NAT:

```text
iperf client
192.168.105.50

       |
       v

FPU5 eno1
192.168.105.5

       |
       v

Linux routing

       |
       v

tun0
       |
       v
APP5
       |
       | opaque/custom transport
       v
APP6
       |
       v
tun0

       |
       v

Linux routing

       |
       v

FPU6 eno1
192.168.106.6

       |
       v

iperf server
192.168.106.60
```

The TCP connection observed by PC6 must retain:

```text
source = 192.168.105.50
```

and the reverse packets must return through the same tunnel architecture.

The application must not require or establish IP connectivity between FPU5 and FPU6.

One design choice I made deliberately is to define the FPU-to-FPU transport as a `Transport` interface and use `AF_UNIX/SOCK_SEQPACKET` only as a test implementation. This should let Codex prove the TUN/routing architecture first, then replace the test transport with your actual FPU transport without touching the packet-routing code.

