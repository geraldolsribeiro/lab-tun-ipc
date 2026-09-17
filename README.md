# PC_5 <-> PC_6 userspace tunnel lab

Topology:

    10.5.0.0/24                 10.100.0.0/24                 10.6.0.0/24
 PC_5 -------- COMM_5 ================================= COMM_6 -------- PC_6
 .2       .1       .5                                      .6       .1     .2
                     backbone / UDP tunnel

Packet path in either direction:

 PC -> COMM kernel -> tun0 -> APP_SRC -> Unix datagram IPC -> APP_F
    -> UDP/5000 over backbone -> APP_F -> Unix datagram IPC -> APP_DST
    -> tun0 -> COMM kernel -> remote PC

The two PC containers are attached only to their local LAN. They have no Docker
interface on `backbone`, so they cannot directly access the COMM backbone NIC/network.
The `lan5`, `lan6`, and `backbone` Docker networks are also marked `internal: true`.
The LAN Docker bridge gateways are `10.5.0.254` and `10.6.0.254`; `.1` is
reserved for `COMM_5` and `COMM_6`, which act as the PCs' routers.

## Run

```sh
docker compose build
docker compose up -d
```

If upgrading from an older compose file that used the default `.1` Docker
network gateways, recreate the networks first:

```sh
docker compose down
docker network rm docker_comm_tunnel_lan5 docker_comm_tunnel_lan6 docker_comm_tunnel_backbone
docker compose up -d --build
```

Check routes/interfaces:

```sh
docker exec PC_5 ip addr
docker exec PC_5 ip route
docker exec COMM_5 ip addr
docker exec COMM_5 ip route
docker exec COMM_6 ip addr
docker exec PC_6 ip route
```

Basic IP test:

```sh
docker exec PC_5 ping -c 3 10.6.0.2
docker exec PC_6 ping -c 3 10.5.0.2
```

iperf v2, PC_5 -> PC_6:

```sh
docker exec -d PC_6 iperf -s
docker exec PC_5 iperf -c 10.6.0.2 -t 10
```

Reverse application direction, PC_6 -> PC_5:

```sh
docker exec PC_6 iperf -c 10.5.0.2 -t 10
```

iperf v2 bidirectional/dual test:

```sh
docker exec PC_5 iperf -c 10.6.0.2 -d -t 10
```

Observe the backbone encapsulation:

```sh
docker exec COMM_5 tcpdump -ni any udp port 5000
```

## Important design point

APP_SRC/APP_DST cannot be ordinary TCP/UDP proxies if the requirement is that an
unmodified `iperf` connection addressed to PC_6 traverses all three applications.
The COMM kernel must hand complete IP packets to userspace. A Linux TUN interface
provides that boundary. APP_SRC reads outbound IP packets from TUN; APP_DST writes
received IP packets back to the same TUN. Thus TCP endpoints remain PC_5 and PC_6,
while the COMM applications transport their IP packets.

This is intentionally a functional lab implementation. For production/high-rate
use, replace Python/Unix datagrams with the target C++ IPC mechanism, add framing,
sequence numbers, MTU handling/fragmentation, loss/reordering policy, authentication,
and metrics.
