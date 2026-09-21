/*
 * apps.cpp - C++20 teaching implementation of the three CMM applications.
 *
 * The lab deliberately transports IP packets instead of application data:
 *
 *   PC_5 -> Linux route -> tun0 -> APP_SRC -> Unix IPC -> APP_F
 *        -> UDP/5000 on the private backbone -> APP_F
 *        -> Unix IPC -> APP_DST -> APP_SRC -> tun0 -> Linux route -> PC_6
 *
 * The same path works in reverse.  Therefore iperf remains a normal, unaware
 * TCP program: its TCP endpoints are still PC_5 and PC_6.  None of these
 * programs parses TCP, UDP, or ICMP; they forward opaque IP packet buffers.
 *
 * IMPORTANT SETUP NOTE:
 * This source moves packets, but it does not configure the network topology.
 * Docker must provide /dev/net/tun and the CMM container must have the
 * privilege/CAP_NET_ADMIN needed for TUNSETIFF.  Outside this file,
 * cmm-init.sh enables IPv4 forwarding, disables reverse-path filtering,
 * brings tun0 up, and adds the remote-LAN route via `ip route`.  pc-init.sh
 * changes each PC's default gateway to its local CMM address.  Without those
 * routes, packets never reach TUN; without forwarding/rp_filter setup, Linux
 * may drop packets even when the applications are running.
 *
 * Traffic direction detail:
 * - A packet arriving from a PC on the CMM LAN veth is routed by Linux to
 *   tun0 because the remote-LAN route is more specific than the default route.
 * - Reading the TUN descriptor removes the packet from the kernel-to-userspace
 *   queue; APP_SRC sends it through IPC and APP_F sends it over UDP.
 * - Writing a received packet to TUN injects it into the kernel. Linux then
 *   routes it out the CMM LAN veth, where the Docker bridge turns it into an
 *   Ethernet frame delivered to the destination PC. Writing to TUN is not
 *   itself an Ethernet transmission: Linux routing performs that final step.
 *
 * This single executable contains all three programs to make comparison with
 * the Python reference easy.  The first argument selects the role:
 *   apps_cpp src tun0 src_to_f.sock dst_to_src.sock
 *   apps_cpp dst f_to_dst.sock dst_to_src.sock
 *   apps_cpp f src_to_f.sock f_to_dst.sock 10.100.0.5:5000 10.100.0.6:5000
 */

#include <arpa/inet.h>    // inet_pton(): text IPv4 address -> binary address
#include <cerrno>         // errno and standard POSIX error numbers
#include <cstring>        // strerror(), strncpy()
#include <cstdlib>        // getenv(), strtoul()
#include <fcntl.h>        // open() and O_RDWR for /dev/net/tun
#include <iostream>       // std::cerr
#include <linux/if_tun.h> // TUNSETIFF, IFF_TUN, IFF_NO_PI
#include <net/if.h>       // struct ifreq and IFNAMSIZ
#include <netinet/in.h>   // sockaddr_in, htons()
#include <netinet/ip.h>   // struct iphdr for minimal IPv4 validation
#include <poll.h>         // pollfd and poll(): wait on several descriptors
#include <stdexcept>      // std::runtime_error
#include <string>         // std::string
#include <sys/ioctl.h>    // ioctl(): device control operations
#include <sys/socket.h>   // socket(), bind(), send(), recv()
#include <sys/un.h>       // sockaddr_un: Unix-domain socket addresses
#include <unistd.h> // open(), read(), write(), close(), unlink(), usleep()
#include <vector>   // dynamic framing buffers

// Maximum IPv4 packet size.  The theoretical IPv4 maximum is 65,535 bytes,
// including its header.  This buffer is intentionally large enough for one
// complete packet and avoids splitting a packet across multiple datagrams.
constexpr std::size_t MAX_PACKET_SIZE = 65535;

// APP_SRC retries because APP_F creates its socket after APP_SRC starts.
// Fifty milliseconds keeps startup simple without busy-spinning the CPU.
constexpr useconds_t IPC_RETRY_DELAY_US = 50000;

// The Linux TUN ioctl number asks the kernel to create or attach to a TUN
// interface.  It is an ABI constant supplied by <linux/if_tun.h>.
constexpr unsigned long TUN_CREATE_IOCTL = TUNSETIFF;

// Create a Unix-domain datagram socket.  A Unix socket never leaves the
// container: it is an IPC mechanism, unlike the UDP socket used on backbone.
// bind_it=true means this process owns and listens at 'path'.  false means
// connect to an endpoint owned by another local process.
static int make_unix_socket(const std::string &path, bool bind_it) {
  // AF_UNIX selects local inter-process communication; SOCK_DGRAM preserves
  // message boundaries, which is important because one message is one IP pkt.
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0)
    throw std::runtime_error("socket(AF_UNIX): " +
                             std::string(strerror(errno)));

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

  if (bind_it) {
    // A pathname socket is a filesystem entry.  Remove a stale entry left by
    // a previous crash before binding, otherwise bind() returns EADDRINUSE.
    unlink(path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
      throw std::runtime_error("bind(" + path + "): " + strerror(errno));
  } else {
    // Startup order is intentionally not encoded in Compose dependencies.
    // Wait until the receiving application has created its socket pathname.
    while (connect(fd, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) < 0) {
      if (errno != ENOENT)
        throw std::runtime_error("connect(" + path + "): " + strerror(errno));
      usleep(IPC_RETRY_DELAY_US);
    }
  }
  return fd;
}

// Open the kernel's TUN character device and attach it to a named interface.
// TUN is layer 3: reads and writes contain IP packets without Ethernet headers.
static int open_tun(const std::string &name) {
  // The container needs /dev/net/tun and CAP_NET_ADMIN (provided by privileged
  // CMM containers).  Failure here usually means missing device/permission.
  int fd = open("/dev/net/tun", O_RDWR);
  if (fd < 0)
    throw std::runtime_error("open(/dev/net/tun): " +
                             std::string(strerror(errno)));

  ifreq request{};
  strncpy(request.ifr_name, name.c_str(), IFNAMSIZ - 1);
  // IFF_TUN requests an IP interface rather than an Ethernet TAP interface.
  // IFF_NO_PI omits a four-byte packet-information prefix from every read.
  request.ifr_flags = IFF_TUN | IFF_NO_PI;
  if (ioctl(fd, TUN_CREATE_IOCTL, &request) < 0)
    throw std::runtime_error("TUNSETIFF: " + std::string(strerror(errno)));
  return fd;
}

// Convert "address:port" into the binary sockaddr required by sendto/bind.
// htons() is necessary because network protocols use big-endian port numbers.
static sockaddr_in parse_endpoint(const std::string &text) {
  auto colon = text.rfind(':');
  if (colon == std::string::npos)
    throw std::runtime_error("endpoint needs address:port");
  sockaddr_in result{};
  result.sin_family = AF_INET;
  result.sin_port = htons(std::stoi(text.substr(colon + 1)));
  if (inet_pton(AF_INET, text.substr(0, colon).c_str(), &result.sin_addr) != 1)
    throw std::runtime_error("invalid IPv4 endpoint: " + text);
  return result;
}

// Validate the IPv4 envelope without interpreting the transport payload.
// Addresses, ports, flags, and checksums are deliberately left untouched.
static bool valid_ipv4_packet(const char *packet, std::size_t size) {
  if (size < sizeof(iphdr)) return false;
  const auto *header = reinterpret_cast<const iphdr *>(packet);
  if (header->version != 4 || header->ihl < 5) return false;
  const std::size_t header_bytes = header->ihl * 4u;
  const std::size_t total_bytes = ntohs(header->tot_len);
  return header_bytes <= total_bytes && total_bytes == size && total_bytes <= MAX_PACKET_SIZE;
}

// APP_SRC owns the one TUN descriptor.  poll() waits for either direction:
// TUN readable means kernel -> tunnel; return IPC readable means tunnel -> PC.
static void run_src(const char *tun_name, const char *to_f,
                    const char *from_dst) {
  int tun_fd = open_tun(tun_name);
  int to_f_fd = make_unix_socket(to_f, false);
  int from_dst_fd = make_unix_socket(from_dst, true);
  char packet[MAX_PACKET_SIZE];
  pollfd watched[] = {{tun_fd, POLLIN, 0}, {from_dst_fd, POLLIN, 0}};

  for (;;) {
    if (poll(watched, 2, -1) < 0 && errno == EINTR)
      continue;
    if (watched[0].revents & POLLIN) {
      // The kernel routed a packet for the remote LAN to tun0.
      auto size = read(tun_fd, packet, sizeof(packet));
      if (size > 0)
        send(to_f_fd, packet, size, 0);
    }
    if (watched[1].revents & POLLIN) {
      // A remote packet has completed the tunnel. Inject it into the kernel;
      // Linux then uses its connected LAN route to deliver it to the PC.
      auto size = recv(from_dst_fd, packet, sizeof(packet), 0);
      if (size > 0 && valid_ipv4_packet(packet, static_cast<std::size_t>(size)))
        write(tun_fd, packet, size);
    }
  }
}

// APP_DST is an explicit application stage.  It does not open TUN; it passes
// the packet to APP_SRC, keeping TUN ownership unambiguous and deterministic.
static void run_dst(const char *from_f, const char *to_src) {
  int from_f_fd = make_unix_socket(from_f, true);
  int to_src_fd = make_unix_socket(to_src, false);
  char packet[MAX_PACKET_SIZE];
  for (;;) {
    auto size = recv(from_f_fd, packet, sizeof(packet), 0);
    if (size > 0)
      send(to_src_fd, packet, size, 0);
  }
}

// APP_F bridges local IPC and remote CMM_F over UDP. UDP datagrams must all
// have exactly APP_F_PAYLOAD bytes, so packets are length-prefixed records
// inside a small framing header. A record may cross frame boundaries.
static void run_f(const char *from_src, const char *to_dst, const char *local,
                  const char *remote) {
  constexpr unsigned FRAME_HEADER = 6; // 4-byte magic + 2-byte used length.
  const std::string magic = "AF20";
  unsigned payload = 1600;
  if (const char *value = std::getenv("APP_F_PAYLOAD")) payload = std::stoul(value);
  if (payload < 28 || payload > 11200 || payload <= FRAME_HEADER)
    throw std::runtime_error("APP_F_PAYLOAD must be 28..11200");
  const unsigned capacity = payload - FRAME_HEADER;

  int ipc_fd = make_unix_socket(from_src, true);
  int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (udp_fd < 0) throw std::runtime_error("socket(UDP): " + std::string(strerror(errno)));
  auto local_address = parse_endpoint(local), remote_address = parse_endpoint(remote);
  if (bind(udp_fd, reinterpret_cast<sockaddr *>(&local_address), sizeof(local_address)) < 0)
    throw std::runtime_error("bind(UDP): " + std::string(strerror(errno)));

  std::vector<unsigned char> pending, stream;
  auto send_frame = [&](const unsigned char *data, unsigned used) {
    std::vector<unsigned char> frame(payload, 0);
    std::memcpy(frame.data(), magic.data(), 4);
    frame[4] = static_cast<unsigned char>(used >> 8);
    frame[5] = static_cast<unsigned char>(used);
    std::memcpy(frame.data() + FRAME_HEADER, data, used);
    sendto(udp_fd, frame.data(), frame.size(), 0,
           reinterpret_cast<sockaddr *>(&remote_address), sizeof(remote_address));
  };
  auto flush = [&] {
    while (pending.size() >= capacity) {
      send_frame(pending.data(), capacity);
      pending.erase(pending.begin(), pending.begin() + capacity);
    }
    if (!pending.empty()) { send_frame(pending.data(), pending.size()); pending.clear(); }
  };

  pollfd watched[] = {{ipc_fd, POLLIN, 0}, {udp_fd, POLLIN, 0}};
  for (;;) {
    poll(watched, 2, -1);
    if (watched[0].revents & POLLIN) {
      unsigned char packet[MAX_PACKET_SIZE];
      auto size = recv(ipc_fd, packet, sizeof(packet), 0);
      if (size > 0) {
        pending.push_back(static_cast<unsigned char>(size >> 24));
        pending.push_back(static_cast<unsigned char>(size >> 16));
        pending.push_back(static_cast<unsigned char>(size >> 8));
        pending.push_back(static_cast<unsigned char>(size));
        pending.insert(pending.end(), packet, packet + size);
        flush();
      }
    }
    if (watched[1].revents & POLLIN) {
      std::vector<unsigned char> frame(payload);
      auto size = recv(udp_fd, frame.data(), frame.size(), 0);
      if (size != static_cast<ssize_t>(payload) || std::memcmp(frame.data(), magic.data(), 4) != 0)
        throw std::runtime_error("invalid fixed APP_F frame");
      unsigned used = (frame[4] << 8) | frame[5];
      if (used > capacity) throw std::runtime_error("invalid APP_F used length");
      stream.insert(stream.end(), frame.begin() + FRAME_HEADER, frame.begin() + FRAME_HEADER + used);
      while (stream.size() >= 4) {
        unsigned length = (stream[0] << 24) | (stream[1] << 16) | (stream[2] << 8) | stream[3];
        if (length < 1 || length > MAX_PACKET_SIZE) throw std::runtime_error("invalid packet length");
        if (stream.size() < 4 + length) break;
        int dst_fd = make_unix_socket(to_dst, false);
        send(dst_fd, stream.data() + 4, length, 0); close(dst_fd);
        stream.erase(stream.begin(), stream.begin() + 4 + length);
      }
    }
  }
}

int main(int argc, char **argv) {
  try {
    if (argc < 2)
      throw std::runtime_error("role required: src, dst, or f");
    std::string role = argv[1];
    if (role == "src" && argc == 5)
      run_src(argv[2], argv[3], argv[4]);
    else if (role == "dst" && argc == 4)
      run_dst(argv[2], argv[3]);
    else if (role == "f" && argc == 6)
      run_f(argv[2], argv[3], argv[4], argv[5]);
    else
      throw std::runtime_error("invalid arguments for role " + role);
  } catch (const std::exception &error) {
    std::cerr << "apps_cpp: " << error.what() << '\n';
    return 1;
  }
}
