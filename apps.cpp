// C++20 reference implementation of APP_SRC, APP_F, and APP_DST.
// It transports complete IPv4 packets: TUN <-> Unix datagrams <-> UDP.
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/if_tun.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

static int unix_socket(const std::string &path, bool bind_it) {
  int s = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (s < 0)
    throw std::runtime_error(strerror(errno));
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
  if (bind_it) {
    unlink(path.c_str());
    if (bind(s, (sockaddr *)&a, sizeof(a)) < 0)
      throw std::runtime_error(strerror(errno));
  } else
    while (connect(s, (sockaddr *)&a, sizeof(a)) < 0) {
      if (errno != ENOENT)
        throw std::runtime_error(strerror(errno));
      usleep(50000);
    }
  return s;
}
static int tun(const std::string &name) {
  int f = open("/dev/net/tun", O_RDWR);
  if (f < 0)
    throw std::runtime_error(strerror(errno));
  ifreq r{};
  strncpy(r.ifr_name, name.c_str(), IFNAMSIZ);
  r.ifr_flags = IFF_TUN | IFF_NO_PI;
  if (ioctl(f, TUNSETIFF, &r) < 0)
    throw std::runtime_error(strerror(errno));
  return f;
}
static sockaddr_in endpoint(const std::string &x) {
  auto p = x.rfind(':');
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(stoi(x.substr(p + 1)));
  inet_pton(AF_INET, x.substr(0, p).c_str(), &a.sin_addr);
  return a;
}
static void src(const char *t, const char *outp, const char *inp) {
  int tf = tun(t), out = unix_socket(outp, false), in = unix_socket(inp, true);
  char b[65535];
  pollfd p[2] = {{tf, POLLIN, 0}, {in, POLLIN, 0}};
  for (;;) {
    poll(p, 2, -1);
    if (p[0].revents)
      send(out, b, read(tf, b, sizeof b), 0);
    if (p[1].revents)
      write(tf, b, recv(in, b, sizeof b, 0));
  }
}
static void dst(const char *inpath, const char *outpath) {
  int in = unix_socket(inpath, true), out = unix_socket(outpath, false);
  char b[65535];
  for (;;)
    send(out, b, recv(in, b, sizeof b, 0), 0);
}
static void bridge(const char *sp, const char *dp, const char *bind,
                   const char *peer) {
  int in = unix_socket(sp, true), u = socket(AF_INET, SOCK_DGRAM, 0);
  auto ba = endpoint(bind), pa = endpoint(peer);
  ::bind(u, (sockaddr *)&ba, sizeof ba);
  char b[65535];
  pollfd p[2] = {{in, POLLIN, 0}, {u, POLLIN, 0}};
  for (;;) {
    poll(p, 2, -1);
    if (p[0].revents) {
      auto n = recv(in, b, sizeof b, 0);
      sendto(u, b, n, 0, (sockaddr *)&pa, sizeof pa);
    }
    if (p[1].revents) {
      auto n = recv(u, b, sizeof b, 0);
      auto d = unix_socket(dp, false);
      send(d, b, n, 0);
      close(d);
    }
  }
}
int main(int n, char **v) {
  try {
    std::string m = v[1];
    if (m == "src")
      src(v[2], v[3], v[4]);
    if (m == "dst")
      dst(v[2], v[3]);
    if (m == "f")
      bridge(v[2], v[3], v[4], v[5]);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
