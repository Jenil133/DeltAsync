#include "deltasync/net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace deltasync {
namespace {

// MSG_NOSIGNAL (Linux) makes a send to a closed peer fail with EPIPE instead
// of raising SIGPIPE. macOS lacks it and uses the SO_NOSIGPIPE socket option,
// set once when the socket is created.
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Keeps single system calls well below INT_MAX bytes.
constexpr std::size_t kMaxSystemIo = std::size_t{1} << 30;

void configure_socket(int fd) {
#ifdef SO_NOSIGPIPE
  const int one = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0) {
    throw NetError("setsockopt(SO_NOSIGPIPE): " + error_message(errno));
  }
#else
  (void)fd;
#endif
}

void set_nonblocking(int fd, bool enable) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0) throw NetError("fcntl(F_GETFL): " + error_message(errno));
  const int wanted = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (wanted != flags && ::fcntl(fd, F_SETFL, wanted) != 0) {
    throw NetError("fcntl(F_SETFL): " + error_message(errno));
  }
}

UniqueFd new_socket(int family, int type, int protocol) {
  UniqueFd fd(::socket(family, type, protocol));
  if (!fd.valid()) throw NetError("socket: " + error_message(errno));
  if (::fcntl(fd.get(), F_SETFD, FD_CLOEXEC) != 0) {
    throw NetError("fcntl(FD_CLOEXEC): " + error_message(errno));
  }
  configure_socket(fd.get());
  return fd;
}

std::string describe(const sockaddr* address) {
  char host[NI_MAXHOST] = {};
  char service[NI_MAXSERV] = {};
  const socklen_t length = address->sa_family == AF_INET6 ? sizeof(sockaddr_in6)
                                                          : sizeof(sockaddr_in);
  if (::getnameinfo(address, length, host, sizeof host, service, sizeof service,
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return "?";
  }
  if (address->sa_family == AF_INET6) return std::string("[") + host + "]:" + service;
  return std::string(host) + ":" + service;
}

struct AddrInfoList {
  addrinfo* head = nullptr;
  ~AddrInfoList() {
    if (head != nullptr) ::freeaddrinfo(head);
  }
};

AddrInfoList resolve(const std::string& host, std::uint16_t port, bool passive) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
  AddrInfoList list;
  const std::string service = std::to_string(port);
  const int status = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(),
                                   &hints, &list.head);
  if (status != 0) {
    throw NetError("cannot resolve '" + host + "': " + ::gai_strerror(status));
  }
  return list;
}

// Milliseconds left until `deadline`, for poll(); -1 waits forever.
int remaining_ms(bool bounded, std::chrono::steady_clock::time_point deadline) {
  if (!bounded) return -1;
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  return static_cast<int>(std::max<std::chrono::milliseconds::rep>(0, left.count()));
}

// poll() on one descriptor, restarted after EINTR with the time that is
// left. Returns false on timeout.
bool poll_one(int fd, short events, int timeout_ms, short* revents) {
  const bool bounded = timeout_ms >= 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    pollfd entry{fd, events, 0};
    const int ready = ::poll(&entry, 1, remaining_ms(bounded, deadline));
    if (ready > 0) {
      *revents = entry.revents;
      return true;
    }
    if (ready == 0) return false;
    if (errno != EINTR) throw NetError(std::string("poll: ") + error_message(errno));
  }
}

}  // namespace

Socket::Socket(UniqueFd fd, std::string peer, int timeout_ms)
    : fd_(std::move(fd)), peer_(std::move(peer)), timeout_ms_(timeout_ms) {
  // Frames are written whole, so Nagle's algorithm would only add latency
  // to the small request/response messages.
  const int one = 1;
  ::setsockopt(fd_.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

void Socket::wait_for(short events, const char* operation) {
  short revents = 0;
  if (!poll_one(fd_.get(), events, timeout_ms_ > 0 ? timeout_ms_ : -1, &revents)) {
    throw TimeoutError(std::string(operation) + " " + peer_ + ": no progress for " +
                       std::to_string(timeout_ms_) + " ms");
  }
  // Errors and hang-ups are reported by the read or write that follows.
}

std::size_t Socket::read_some(std::uint8_t* out, std::size_t capacity) {
  if (capacity == 0) return 0;
  for (;;) {
    wait_for(POLLIN, "receive from");
    const ssize_t count = ::recv(fd_.get(), out, std::min(capacity, kMaxSystemIo), 0);
    if (count >= 0) {
      bytes_received_ += static_cast<std::uint64_t>(count);
      return static_cast<std::size_t>(count);
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    throw NetError("receive from " + peer_ + ": " + error_message(errno));
  }
}

void Socket::read_exact(std::uint8_t* out, std::size_t length) {
  while (length > 0) {
    const std::size_t count = read_some(out, length);
    if (count == 0) throw NetError("connection closed by " + peer_);
    out += count;
    length -= count;
  }
}

void Socket::write_all(const std::uint8_t* data, std::size_t length) {
  while (length > 0) {
    wait_for(POLLOUT, "send to");
    const ssize_t count =
        ::send(fd_.get(), data, std::min(length, kMaxSystemIo), kSendFlags);
    if (count >= 0) {
      bytes_sent_ += static_cast<std::uint64_t>(count);
      data += count;
      length -= static_cast<std::size_t>(count);
      continue;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    if (errno == EPIPE || errno == ECONNRESET) {
      throw NetError("connection closed by " + peer_);
    }
    throw NetError("send to " + peer_ + ": " + error_message(errno));
  }
}

bool Socket::readable(int timeout_ms) {
  short revents = 0;
  return poll_one(fd_.get(), POLLIN, timeout_ms, &revents);
}

void Socket::shutdown_write() {
  if (fd_.valid()) ::shutdown(fd_.get(), SHUT_WR);
}

void Socket::shutdown_both() {
  if (fd_.valid()) ::shutdown(fd_.get(), SHUT_RDWR);
}

Socket connect_to(const std::string& host, std::uint16_t port, int timeout_ms) {
  const AddrInfoList addresses = resolve(host, port, false);
  std::string last_error = "no addresses";
  for (const addrinfo* entry = addresses.head; entry != nullptr; entry = entry->ai_next) {
    UniqueFd fd = new_socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    const std::string peer = describe(entry->ai_addr);
    // A non-blocking connect lets poll() bound the wait; the socket goes
    // back to blocking mode afterwards (every operation polls first anyway).
    set_nonblocking(fd.get(), true);
    int status = ::connect(fd.get(), entry->ai_addr, entry->ai_addrlen);
    if (status != 0 && (errno == EINPROGRESS || errno == EINTR)) {
      short revents = 0;
      if (!poll_one(fd.get(), POLLOUT, timeout_ms > 0 ? timeout_ms : -1, &revents)) {
        last_error = peer + ": connection timed out";
        continue;
      }
      int error = 0;
      socklen_t length = sizeof error;
      if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) != 0) error = errno;
      status = error == 0 ? 0 : -1;
      errno = error;
    }
    if (status != 0) {
      last_error = peer + ": " + error_message(errno);
      continue;
    }
    set_nonblocking(fd.get(), false);
    return Socket(std::move(fd), peer, timeout_ms);
  }
  throw NetError("cannot connect to " + host + ":" + std::to_string(port) + " (" +
                 last_error + ")");
}

Listener::Listener(const std::string& bind_address, std::uint16_t port, int backlog) {
  const AddrInfoList addresses = resolve(bind_address, port, true);
  std::string last_error = "no addresses";
  for (const addrinfo* entry = addresses.head; entry != nullptr; entry = entry->ai_next) {
    UniqueFd fd = new_socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    const int one = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    // Non-blocking, so that accepting a connection that was reset after
    // poll() reported it cannot hang; accept() waits in poll() instead.
    set_nonblocking(fd.get(), true);
    if (::bind(fd.get(), entry->ai_addr, entry->ai_addrlen) != 0 ||
        ::listen(fd.get(), backlog) != 0) {
      last_error = describe(entry->ai_addr) + ": " + error_message(errno);
      continue;
    }
    sockaddr_storage bound{};
    socklen_t length = sizeof bound;
    if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
      throw NetError("getsockname: " + error_message(errno));
    }
    const auto* address = reinterpret_cast<const sockaddr*>(&bound);
    port_ = ntohs(address->sa_family == AF_INET6
                      ? reinterpret_cast<const sockaddr_in6*>(address)->sin6_port
                      : reinterpret_cast<const sockaddr_in*>(address)->sin_port);
    address_ = describe(address);
    fd_ = std::move(fd);
    return;
  }
  throw NetError("cannot listen on " + bind_address + ":" + std::to_string(port) + " (" +
                 last_error + ")");
}

Socket Listener::accept(int timeout_ms) {
  for (;;) {
    short revents = 0;
    poll_one(fd_.get(), POLLIN, -1, &revents);
    Socket socket = try_accept(timeout_ms);
    if (socket.valid()) return socket;
  }
}

Socket Listener::try_accept(int timeout_ms) {
  for (;;) {
    sockaddr_storage peer{};
    socklen_t length = sizeof peer;
    const int fd = ::accept(fd_.get(), reinterpret_cast<sockaddr*>(&peer), &length);
    if (fd >= 0) {
      UniqueFd owned(fd);
      if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        throw NetError("fcntl(FD_CLOEXEC): " + error_message(errno));
      }
      configure_socket(fd);
      // Accepted sockets may inherit O_NONBLOCK from the listener on some
      // systems; operations poll first, so blocking mode is what we want.
      set_nonblocking(fd, false);
      return Socket(std::move(owned), describe(reinterpret_cast<sockaddr*>(&peer)), timeout_ms);
    }
    if (errno == EINTR) continue;
    // Nothing pending, or a connection that was reset while queued (the
    // client's problem).
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED || errno == EPROTO) {
      return Socket();
    }
    throw NetError("accept: " + error_message(errno));
  }
}

}  // namespace deltasync
