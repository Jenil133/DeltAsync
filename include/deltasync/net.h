#ifndef DELTASYNC_NET_H_
#define DELTASYNC_NET_H_

// Blocking TCP sockets with the details every caller would otherwise get
// wrong: partial reads and writes, EINTR, per-operation timeouts (poll), and
// writes to a closed peer reported as errors instead of SIGPIPE. Both ends
// count every byte that crosses the socket.

#include <cstddef>
#include <cstdint>
#include <string>

#include "deltasync/error.h"
#include "deltasync/file.h"

namespace deltasync {

// The connection failed: refused, reset, closed early by the peer, or timed
// out. Retrying may help, unlike a FormatError or IntegrityError.
class NetError : public Error {
 public:
  using Error::Error;
};

// No progress within the configured timeout.
class TimeoutError : public NetError {
 public:
  using NetError::NetError;
};

// Inactivity timeout used when the caller does not choose one.
constexpr int kDefaultTimeoutMs = 30000;

// A connected stream socket. Every blocking operation waits at most
// timeout_ms for progress (0 waits forever).
class Socket {
 public:
  Socket() = default;
  Socket(UniqueFd fd, std::string peer, int timeout_ms);

  Socket(Socket&&) noexcept = default;
  Socket& operator=(Socket&&) noexcept = default;

  bool valid() const { return fd_.valid(); }
  int fd() const { return fd_.get(); }
  const std::string& peer() const { return peer_; }

  void set_timeout_ms(int timeout_ms) { timeout_ms_ = timeout_ms; }
  int timeout_ms() const { return timeout_ms_; }

  // Reads up to `capacity` bytes; returns 0 only when the peer has closed
  // its side. Throws TimeoutError or NetError.
  std::size_t read_some(std::uint8_t* out, std::size_t capacity);

  // Reads exactly `length` bytes. A peer that closes first is a NetError.
  void read_exact(std::uint8_t* out, std::size_t length);

  // Writes all `length` bytes. Throws NetError if the peer has gone away.
  void write_all(const std::uint8_t* data, std::size_t length);

  // True if a read would not block (data or end of stream is pending),
  // waiting up to `timeout_ms` (0 = just check).
  bool readable(int timeout_ms);

  // Sends FIN: the peer reads end of stream after the data already sent.
  void shutdown_write();

  // Shuts down both directions; any thread blocked on this socket wakes up
  // with an error. Safe to call from another thread.
  void shutdown_both();

  std::uint64_t bytes_sent() const { return bytes_sent_; }
  std::uint64_t bytes_received() const { return bytes_received_; }

 private:
  // Waits until `events` are ready or the timeout passes (TimeoutError).
  void wait_for(short events, const char* operation);

  UniqueFd fd_;
  std::string peer_;
  int timeout_ms_ = kDefaultTimeoutMs;
  std::uint64_t bytes_sent_ = 0;
  std::uint64_t bytes_received_ = 0;
};

// Connects to host:port (any address family the name resolves to), giving
// up after timeout_ms per address. Throws NetError.
Socket connect_to(const std::string& host, std::uint16_t port, int timeout_ms);

// A listening socket. Port 0 picks a free port; port() reports it.
class Listener {
 public:
  Listener(const std::string& bind_address, std::uint16_t port, int backlog = 64);

  std::uint16_t port() const { return port_; }
  int fd() const { return fd_.get(); }
  // "address:port" as bound, for log messages.
  const std::string& address() const { return address_; }

  // Waits for a connection (forever). Accepted sockets get `timeout_ms`.
  Socket accept(int timeout_ms);

  // Accepts a connection if one is pending, else returns an invalid Socket.
  // Never blocks, even if a connection that poll() reported was reset
  // before it could be accepted.
  Socket try_accept(int timeout_ms);

 private:
  UniqueFd fd_;
  std::uint16_t port_ = 0;
  std::string address_;
};

}  // namespace deltasync

#endif  // DELTASYNC_NET_H_
