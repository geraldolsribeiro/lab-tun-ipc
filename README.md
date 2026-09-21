# PC_5 <-> PC_6 userspace tunnel lab

Topology:

    10.5.0.0/24                 10.100.0.0/24                 10.6.0.0/24
 PC_5 -------- CMM_5 ================================= CMM_6 -------- PC_6
 .2       .1       .5                                      .6       .1     .2
                     backbone / UDP tunnel

Packet path in either direction:

 PC -> CMM kernel -> tun0 -> APP_SRC -> Unix datagram IPC -> APP_F
    -> UDP/5000 over backbone -> APP_F -> Unix datagram IPC -> APP_DST
    -> tun0 -> CMM kernel -> remote PC

The two PC containers are attached only to their local LAN. They have no Docker
interface on `backbone`, so they cannot directly access the CMM backbone NIC/network.
The `lan5`, `lan6`, and `backbone` Docker networks are also marked `internal: true`.
The LAN Docker bridge gateways are `10.5.0.254` and `10.6.0.254`; `.1` is
reserved for `CMM_5` and `CMM_6`, which act as the PCs' routers.

## Run

The default implementation is Python:

```sh
docker compose build
docker compose up -d
```

## Python and C++ implementations

The project includes two interchangeable implementations of `APP_SRC`,
`APP_F`, and `APP_DST`:

- **Python**: `app_src.py`, `app_f.py`, and `app_dst.py`. This version is
  intended to be easy to read and modify while learning the packet path.
- **C++20**: `apps.cpp`. One executable contains all three roles and selects
  the role through its first command-line argument. Docker compiles it as
  `/opt/lab/apps_cpp` with optimization enabled.

Both implementations use the same interfaces and protocol:

```text
TUN <-> Unix datagram IPC <-> UDP/5000 over the backbone <-> Unix IPC <-> TUN
```

### Run the Python implementation

Python is the default when `APP_LANG` is omitted:

```sh
APP_LANG=python docker compose up -d --build
```

Equivalent Makefile commands:

```sh
make build
make up
```

### Run the C++20 implementation

Set `APP_LANG=cpp` for both CMM containers. The PC containers remain unchanged;
they only generate and receive normal IP traffic:

```sh
docker compose down
APP_LANG=cpp docker compose up -d --build
```

The C++ processes are started internally as:

```text
CMM_5: apps_cpp src ... / apps_cpp f ... / apps_cpp dst ...
CMM_6: apps_cpp src ... / apps_cpp f ... / apps_cpp dst ...
```

### Compare the implementations

Run the same test with each implementation and compare throughput and CPU use:

```sh
# Python test
APP_LANG=python docker compose down
APP_LANG=python docker compose up -d --build
make ping
make iperf

# C++20 test
APP_LANG=cpp docker compose down
APP_LANG=cpp docker compose up -d --build
make ping
make iperf
```

To observe the UDP packet transport while testing:

```sh
make tcpdump
```

`APP_LANG` only controls the applications inside `CMM_5` and `CMM_6`.
`PC_5` and `PC_6` always use the same Ubuntu tools and network configuration.

## Fixed-size APP_F payloads and packet framing

`APP_F` can be configured with a fixed UDP payload size from 28 to 11,200
bytes. Set the same value in both CMM containers with `APP_F_PAYLOAD`:

```sh
APP_LANG=python APP_F_PAYLOAD=1600 docker compose up -d --build
```

An IP packet is prefixed with a 4-byte network-order length. APP_F packs one
or more length-prefixed packets into each payload and pads the final payload
with zero bytes. A packet may cross payload boundaries. The receiving APP_F
buffers payloads, removes padding, reads each length prefix, and forwards only
complete packets to APP_DST in their original order. No partial packet is ever
written to TUN. This preserves packet boundaries while meeting the fixed-size
UDP requirement.

## How traffic moves between Ethernet and TUN

Docker creates virtual Ethernet (`veth`) interfaces for each container network.
The PC containers have only their local LAN interface. Their startup script
replaces the default route so that traffic for the opposite LAN is sent to the
local communicator's LAN address (`10.5.0.1` or `10.6.0.1`). This is ordinary
layer-3 routing; the PCs do not have an interface on the backbone network.

The communicator setup then installs a more specific route for the *remote*
LAN through `tun0`:

```text
PC_5:     10.6.0.0/24 via 10.5.0.1
CMM_5:   10.6.0.0/24 dev tun0
CMM_6:   10.5.0.0/24 dev tun0
PC_6:     10.5.0.0/24 via 10.6.0.1
```

When PC_5 sends an IP packet to PC_6, Linux on CMM_5 sees the packet arrive
on its LAN-side virtual Ethernet interface. Its routing table matches the
remote-LAN route and sends the packet to `tun0`, rather than to the backbone
Ethernet interface. `APP_SRC` reads that packet from TUN and hands it to
`APP_F` through Unix IPC. The two APP_F processes carry it as a UDP payload
across the backbone.

On CMM_6, `APP_DST` and `APP_SRC` write the received IP packet into `tun0`.
Writing to TUN injects the packet back into the CMM_6 Linux networking stack;
it is not an Ethernet transmission by itself. Linux examines the destination
(`10.6.0.2`), selects the connected LAN route, and emits a new Ethernet frame
through CMM_6's LAN-side virtual Ethernet interface. Docker's bridge delivers
that frame to PC_6. The reverse direction follows the same process.

The C++ applications cannot perform all of this setup alone. Docker must create
`/dev/net/tun`, grant the CMM containers network administration privileges, and
attach the containers to the three isolated networks. `cmm-init.sh` enables IP
forwarding, disables problematic reverse-path filtering, brings `tun0` up, and
installs the remote-LAN route. `pc-init.sh` changes each PC's default gateway.
These are intentionally visible shell-level networking operations so the
application code can focus on moving packets.

## Event-loop design: why `poll()`?

The C++ implementation uses Linux/POSIX `poll()` to wait for input without
busy-waiting. Each process has only a small number of descriptors:

- `APP_SRC` watches `tun0` and its return Unix socket.
- `APP_F` watches its Unix IPC socket and its UDP backbone socket.
- `APP_DST` waits on one Unix IPC socket.

For example:

```cpp
poll(watched, 2, -1);
```

The `-1` timeout means that the process sleeps until one of the descriptors is
ready. This is more efficient than repeatedly checking sockets in a tight loop.

`poll()` was selected because it is straightforward, widely available on Linux,
and exposes the event-driven networking model clearly. `select()` is older and
has descriptor-count limitations. `epoll()` is a strong choice for a production
server managing thousands of descriptors, but adds complexity that is not
needed when each lab application watches only one or two descriptors.
`io_uring` can provide advanced asynchronous I/O, but requires a more involved
submission/completion-queue design. C++20 coroutines do not provide networking
by themselves and would require an additional asynchronous runtime. Libraries
such as Boost.Asio would also hide some of the socket operations this lab is
intended to demonstrate.

For a production, high-rate tunnel, benchmark `epoll()` and `io_uring` against
`poll()` using realistic packet sizes, rates, CPU measurements, packet loss,
and latency. For this small teaching topology, `poll()` keeps the implementation
understandable while still being event-driven and efficient.

If upgrading from an older compose file that used the default `.1` Docker
network gateways, recreate the networks first:

```sh
docker compose down
docker network rm docker_cmm_tunnel_lan5 docker_cmm_tunnel_lan6 docker_cmm_tunnel_backbone
docker compose up -d --build
```

Check routes/interfaces:

```sh
docker exec PC_5 ip addr
docker exec PC_5 ip route
docker exec CMM_5 ip addr
docker exec CMM_5 ip route
docker exec CMM_6 ip addr
docker exec PC_6 ip route
```

Basic IP and RTT test:

```sh
docker exec PC_5 ping -c 10 -W 2 10.6.0.2
docker exec PC_6 ping -c 10 -W 2 10.5.0.2
```

The `make ping` target runs the first test with ten ICMP echo requests. The
summary reports:

```text
rtt min/avg/max/mdev = MIN/AVERAGE/MAX/STDDEV ms
```

For each successful request, RTT is measured as the elapsed time from sending
the ICMP Echo Request until receiving its Echo Reply. If the successful sample
RTTs are `r_1 ... r_n`:

- `min = min(r_i)` — fastest round trip.
- `avg = (r_1 + ... + r_n) / n` — arithmetic mean RTT.
- `max = max(r_i)` — slowest round trip.
- `mdev` — ping's mean deviation statistic, representing typical variation
  around the average; it is not additional network latency.
- Packet loss is failed requests divided by total requests.

The RTT includes both directions of the complete path: PC -> local CMM -> TUN
and applications -> UDP backbone -> remote applications -> TUN -> remote PC,
plus the return path. It therefore measures end-to-end tunnel latency rather
than only UDP or application processing time.

High-rate ping tests:

Deterministic rate test, without flood mode:

```sh
make ping-rate
```

Stress test, with flood mode:

```sh
make ping-stress
```

Both tests send 1,000 ICMP requests with an 11,000-byte payload and a
requested 2-millisecond interval:

```sh
ping -f -i 0.002 -s 11000 -c 1000 -W 2 10.6.0.2
```

`-s 11000` specifies the ICMP data size. The resulting IP packet is larger
than the normal 1500-byte Ethernet MTU, so Linux may fragment it before APP_SRC
reads the fragments from TUN. This stresses fixed-size APP_F framing,
fragmentation/reassembly, throughput, and packet loss.

`-i 0.002` requests one new echo request every 0.002 seconds, or up to 500
requests per second. It is a pacing interval, not a packet-size option. The
actual rate can be lower because of replies, CPU scheduling, routing, or packet
loss. Privileged Linux ping permits very small intervals; unprivileged ping may
reject them or enforce a larger minimum.

`-f` enables flood mode. It asks ping to transmit as aggressively as possible
and prints a compact progress display instead of one line per reply. When
combined with `-i`, the explicit interval is the intended pacing limit, but
exact precedence and minimum enforcement depend on the installed iputils ping
version. Without `-f`, `-i 0.002` is the clearer deterministic rate test;
with `-f`, the command intentionally stresses the implementation and may
produce more load than the requested interval suggests.

iperf v2, PC_5 -> PC_6:

```sh
docker exec -d PC_6 iperf -s
docker exec PC_5 iperf -c 10.6.0.2 -b 40m -t 10
```

Reverse application direction, PC_6 -> PC_5:

```sh
docker exec PC_6 iperf -c 10.5.0.2 -b 40m -t 10
```

iperf v2 bidirectional/dual test:

```sh
docker exec PC_5 iperf -c 10.6.0.2 -d -t 10
```

Observe the backbone encapsulation:

```sh
docker exec CMM_5 tcpdump -ni any udp port 5000
```

## Important design point

APP_SRC/APP_DST cannot be ordinary TCP/UDP proxies if the requirement is that an
unmodified `iperf` connection addressed to PC_6 traverses all three applications.
The CMM kernel must hand complete IP packets to userspace. A Linux TUN interface
provides that boundary. APP_SRC reads outbound IP packets from TUN; APP_DST writes
received IP packets back to the same TUN. Thus TCP endpoints remain PC_5 and PC_6,
while the CMM applications transport their IP packets.

This is intentionally a functional lab implementation. For production/high-rate
use, replace Python/Unix datagrams with the target C++ IPC mechanism, add framing,
sequence numbers, MTU handling/fragmentation, loss/reordering policy, authentication,
and metrics.


 Created Makefile with automation for:

 ```bash
   make build
   make up
   make down
   make restart
   make ping
   make iperf
   make tcpdump
   make logs
   make ps
   make clean
   make help
 ```

 Default command:

 ```bash
   make
 ```

 runs make up.

