// deltasync_proxy: a small TCP forwarder for fault injection.
//
// It accepts connections on a local port, connects each one to a fixed
// target and copies bytes both ways. Optionally it cuts a connection after a
// given number of forwarded bytes (to interrupt a sync at a chosen point) and
// delays every forwarded piece (a crude stand-in for latency where tc netem
// is not available).
//
// Once listening it prints one line, "listening on <host>:<port>", to standard
// output, so callers that pass --listen-port 0 can learn the port without
// guessing or polling. Each finished connection is summarised on standard
// error.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "deltasync/error.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

constexpr char kUsage[] = R"(usage: deltasync_proxy --listen-port P --target-host H --target-port T [options]

Forwards every connection accepted on P to H:T, in both directions.

Options:
  --listen-port P          port to listen on (0 picks a free port)
  --listen-host A          address to listen on (default 127.0.0.1)
  --target-host H          host to forward to
  --target-port T          port to forward to
  --drop-after-bytes N     cut each connection once N bytes have been forwarded
                           in the counted direction(s)
  --drop-direction D       which bytes count towards N: up (client to target),
                           down (target to client) or both (default)
  --drop-connections K     cut only the first K connections; later ones are
                           forwarded untouched (default: every connection)
  --delay-ms D             wait D milliseconds before forwarding each piece
  --max-connections K      exit after K connections have been served
                           (default: run until killed)
  -h, --help               show this help and exit

On startup the proxy prints "listening on <host>:<port>" to standard output.
)";

class UsageError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

enum class Direction { kUp, kDown, kBoth };

struct Options {
  std::string listen_host = "127.0.0.1";
  int listen_port = -1;
  std::string target_host;
  int target_port = -1;
  bool drop = false;
  std::uint64_t drop_after_bytes = 0;
  Direction drop_direction = Direction::kBoth;
  std::uint64_t drop_connections = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t delay_ms = 0;
  std::uint64_t max_connections = 0;  // 0 = unlimited
};

std::uint64_t parse_number(const std::string& flag, const std::string& text, std::uint64_t max) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
    throw UsageError(flag + ": expected a non-negative integer, got '" + text + "'");
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (errno == ERANGE || value > max) {
    throw UsageError(flag + ": value out of range: " + text);
  }
  return value;
}

int parse_port(const std::string& flag, const std::string& text) {
  return static_cast<int>(parse_number(flag, text, 65535));
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) throw UsageError(arg + " needs a value");
      return argv[++i];
    };
    if (arg == "--listen-port") {
      options.listen_port = parse_port(arg, value());
    } else if (arg == "--listen-host") {
      options.listen_host = value();
    } else if (arg == "--target-host") {
      options.target_host = value();
    } else if (arg == "--target-port") {
      options.target_port = parse_port(arg, value());
    } else if (arg == "--drop-after-bytes") {
      options.drop = true;
      options.drop_after_bytes =
          parse_number(arg, value(), std::numeric_limits<std::uint64_t>::max());
    } else if (arg == "--drop-direction") {
      const std::string direction = value();
      if (direction == "up") {
        options.drop_direction = Direction::kUp;
      } else if (direction == "down") {
        options.drop_direction = Direction::kDown;
      } else if (direction == "both") {
        options.drop_direction = Direction::kBoth;
      } else {
        throw UsageError("--drop-direction: expected up, down or both, got '" + direction + "'");
      }
    } else if (arg == "--drop-connections") {
      options.drop_connections =
          parse_number(arg, value(), std::numeric_limits<std::uint64_t>::max());
    } else if (arg == "--delay-ms") {
      options.delay_ms = parse_number(arg, value(), 3'600'000);
    } else if (arg == "--max-connections") {
      options.max_connections =
          parse_number(arg, value(), std::numeric_limits<std::uint64_t>::max());
    } else {
      throw UsageError("unknown option: " + arg);
    }
  }
  if (options.listen_port < 0) throw UsageError("--listen-port is required");
  if (options.target_host.empty()) throw UsageError("--target-host is required");
  if (options.target_port <= 0) throw UsageError("--target-port is required (1..65535)");
  return options;
}

std::string errno_text(int error_number) { return deltasync::error_message(error_number); }

class Socket {
 public:
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  ~Socket() {
    if (fd_ >= 0) ::close(fd_);
  }
  int fd() const { return fd_; }

 private:
  int fd_ = -1;
};

void suppress_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}

void set_nodelay(int fd) {
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

Socket connect_to(const std::string& host, int port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  const int status = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
  if (status != 0) {
    throw std::runtime_error("resolve " + host + ": " + ::gai_strerror(status));
  }
  std::unique_ptr<addrinfo, void (*)(addrinfo*)> guard(results, ::freeaddrinfo);
  int last_errno = 0;
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    Socket socket(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (socket.fd() < 0) {
      last_errno = errno;
      continue;
    }
    int rc;
    do {
      rc = ::connect(socket.fd(), ai->ai_addr, ai->ai_addrlen);
    } while (rc != 0 && errno == EINTR);
    if (rc == 0) {
      suppress_sigpipe(socket.fd());
      set_nodelay(socket.fd());
      return socket;
    }
    last_errno = errno;
  }
  throw std::runtime_error("connect " + host + ":" + service + ": " + errno_text(last_errno));
}

Socket listen_on(const std::string& host, int port, int* bound_port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  const int status = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
  if (status != 0) {
    throw std::runtime_error("resolve " + host + ": " + ::gai_strerror(status));
  }
  std::unique_ptr<addrinfo, void (*)(addrinfo*)> guard(results, ::freeaddrinfo);
  int last_errno = 0;
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    Socket socket(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (socket.fd() < 0) {
      last_errno = errno;
      continue;
    }
    int one = 1;
    ::setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (::bind(socket.fd(), ai->ai_addr, ai->ai_addrlen) != 0 || ::listen(socket.fd(), 64) != 0) {
      last_errno = errno;
      continue;
    }
    sockaddr_storage address{};
    socklen_t length = sizeof address;
    if (::getsockname(socket.fd(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      last_errno = errno;
      continue;
    }
    if (address.ss_family == AF_INET6) {
      *bound_port = ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    } else {
      *bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
    }
    return socket;
  }
  throw std::runtime_error("listen on " + host + ":" + service + ": " + errno_text(last_errno));
}

bool send_all(int fd, const char* data, std::size_t length) {
#ifdef MSG_NOSIGNAL
  constexpr int kFlags = MSG_NOSIGNAL;
#else
  constexpr int kFlags = 0;
#endif
  while (length > 0) {
    const ssize_t sent = ::send(fd, data, length, kFlags);
    if (sent < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += sent;
    length -= static_cast<std::size_t>(sent);
  }
  return true;
}

// One proxied connection: two pump threads, one per direction. The byte
// budget is shared when both directions count, so the cut lands on exactly
// the N-th forwarded byte whichever side sends it.
class Connection {
 public:
  Connection(std::uint64_t id, Socket client, Socket target, const Options& options, bool cut)
      : id_(id),
        client_(std::move(client)),
        target_(std::move(target)),
        options_(options),
        cut_enabled_(cut) {}

  void run() {
    if (cut_enabled_ && options_.drop_after_bytes == 0) {
      // A budget of zero cuts the connection before anything is forwarded.
      cut_ = true;
      sever();
    }
    std::thread down([this] { pump(target_.fd(), client_.fd(), /*up=*/false); });
    pump(client_.fd(), target_.fd(), /*up=*/true);
    down.join();
    std::fprintf(stderr, "deltasync_proxy: connection %llu closed: up=%llu down=%llu cut=%s\n",
                 static_cast<unsigned long long>(id_), static_cast<unsigned long long>(up_bytes_),
                 static_cast<unsigned long long>(down_bytes_), cut_ ? "yes" : "no");
  }

 private:
  bool counts(bool up) const {
    if (!cut_enabled_) return false;
    switch (options_.drop_direction) {
      case Direction::kUp:
        return up;
      case Direction::kDown:
        return !up;
      case Direction::kBoth:
        return true;
    }
    return false;
  }

  // Decides how much of a piece of `length` bytes may be forwarded. Returns
  // the allowed count and sets *cut when the budget is exhausted by it.
  std::size_t reserve(bool up, std::size_t length, bool* cut) {
    std::lock_guard<std::mutex> lock(mutex_);
    *cut = false;
    if (cut_) {
      *cut = true;
      return 0;
    }
    std::size_t allowed = length;
    if (counts(up)) {
      const std::uint64_t remaining = options_.drop_after_bytes - counted_;
      if (length >= remaining) {
        allowed = static_cast<std::size_t>(remaining);
        cut_ = true;
        *cut = true;
      }
      counted_ += allowed;
    }
    (up ? up_bytes_ : down_bytes_) += allowed;
    return allowed;
  }

  // Wakes the other pump and makes both peers see the connection end.
  void sever() {
    ::shutdown(client_.fd(), SHUT_RDWR);
    ::shutdown(target_.fd(), SHUT_RDWR);
  }

  void pump(int from, int to, bool up) {
    std::vector<char> buffer(64 * 1024);
    for (;;) {
      const ssize_t received = ::recv(from, buffer.data(), buffer.size(), 0);
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) {
        if (received < 0) {
          sever();
        } else {
          // Orderly end of this direction: pass the half-close on.
          ::shutdown(to, SHUT_WR);
        }
        return;
      }
      if (options_.delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options_.delay_ms));
      }
      bool cut = false;
      const std::size_t allowed = reserve(up, static_cast<std::size_t>(received), &cut);
      if (allowed > 0 && !send_all(to, buffer.data(), allowed)) {
        sever();
        return;
      }
      if (cut) {
        sever();
        return;
      }
    }
  }

  const std::uint64_t id_;
  Socket client_;
  Socket target_;
  const Options& options_;
  const bool cut_enabled_;

  std::mutex mutex_;
  std::uint64_t counted_ = 0;
  std::uint64_t up_bytes_ = 0;
  std::uint64_t down_bytes_ = 0;
  bool cut_ = false;
};

int serve(const Options& options) {
  int port = 0;
  Socket listener = listen_on(options.listen_host, options.listen_port, &port);
  std::printf("listening on %s:%d\n", options.listen_host.c_str(), port);
  std::fflush(stdout);

  std::vector<std::thread> workers;
  std::uint64_t accepted = 0;
  while (options.max_connections == 0 || accepted < options.max_connections) {
    const int fd = ::accept(listener.fd(), nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      std::fprintf(stderr, "deltasync_proxy: accept: %s\n", errno_text(errno).c_str());
      break;
    }
    Socket client(fd);
    suppress_sigpipe(client.fd());
    set_nodelay(client.fd());
    const std::uint64_t id = ++accepted;
    const bool cut = options.drop && id <= options.drop_connections;
    workers.emplace_back([id, cut, &options, client = std::move(client)]() mutable {
      Socket target;
      try {
        target = connect_to(options.target_host, options.target_port);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "deltasync_proxy: connection %llu: %s\n",
                     static_cast<unsigned long long>(id), e.what());
        return;  // closing `client` tells the peer the target is unreachable
      }
      Connection connection(id, std::move(client), std::move(target), options, cut);
      connection.run();
    });
  }
  for (std::thread& worker : workers) worker.join();
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  // Writes to a peer that already went away must fail with EPIPE, not kill
  // the proxy.
  ::signal(SIGPIPE, SIG_IGN);
  try {
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
        std::fputs(kUsage, stdout);
        return kExitOk;
      }
    }
    const Options options = parse_options(argc, argv);
    return serve(options);
  } catch (const UsageError& e) {
    std::fprintf(stderr, "deltasync_proxy: %s\n\n%s", e.what(), kUsage);
    return kExitUsage;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "deltasync_proxy: %s\n", e.what());
    return kExitFailure;
  }
}
