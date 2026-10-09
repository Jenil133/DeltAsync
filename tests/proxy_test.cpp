// Tests for deltasync_proxy, the fault-injection TCP forwarder. They run the
// real executable and talk to it with plain sockets. Synchronisation is by
// data only: the proxy announces its port on stdout, servers report what
// they received through futures, and every connection end is observed as
// EOF or an error, never by waiting a fixed time.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "deltasync/error.h"
#include "test_util.h"

extern char** environ;

namespace deltasync::test {
namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  ~Fd() { reset(); }
  int get() const { return fd_; }
  void reset() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_ = -1;
};

void no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}

// Sends everything; returns false if the peer went away part way.
bool send_all(int fd, const std::uint8_t* data, std::size_t length) {
  while (length > 0) {
    const ssize_t sent = ::send(fd, data, length, kSendFlags);
    if (sent < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += sent;
    length -= static_cast<std::size_t>(sent);
  }
  return true;
}

// Reads until EOF or a connection error, returning what arrived.
Bytes read_to_end(int fd) {
  Bytes out;
  std::uint8_t buffer[16384];
  for (;;) {
    const ssize_t got = ::recv(fd, buffer, sizeof buffer, 0);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return out;
    out.insert(out.end(), buffer, buffer + got);
  }
}

// Reads exactly `length` bytes (fewer only if the connection ends).
Bytes read_exactly(int fd, std::size_t length) {
  Bytes out(length);
  std::size_t have = 0;
  while (have < length) {
    const ssize_t got = ::recv(fd, out.data() + have, length - have, 0);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) break;
    have += static_cast<std::size_t>(got);
  }
  out.resize(have);
  return out;
}

Fd listen_loopback(int* port) {
  Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (fd.get() < 0) throw std::runtime_error("socket failed");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
      ::listen(fd.get(), 16) != 0) {
    throw std::runtime_error("bind/listen failed");
  }
  socklen_t length = sizeof address;
  ::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&address), &length);
  *port = ntohs(address.sin_port);
  return fd;
}

Fd connect_loopback(int port) {
  Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (fd.get() < 0) throw std::runtime_error("socket failed");
  no_sigpipe(fd.get());
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    throw std::runtime_error(std::string("connect failed: ") + std::strerror(errno));
  }
  return fd;
}

Fd accept_one(int listener) {
  for (;;) {
    const int fd = ::accept(listener, nullptr, nullptr);
    if (fd >= 0) {
      no_sigpipe(fd);
      return Fd(fd);
    }
    if (errno != EINTR) throw std::runtime_error("accept failed");
  }
}

// The proxy executable as a child process.
class Proxy {
 public:
  explicit Proxy(std::vector<std::string> args) {
    args.insert(args.begin(), DELTASYNC_PROXY_PATH);
    std::vector<char*> argv;
    for (std::string& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) throw std::runtime_error("pipe failed");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    const int rc = ::posix_spawn(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);
    stdout_ = Fd(pipe_fds[0]);
    if (rc != 0) throw std::runtime_error("posix_spawn failed");
  }

  ~Proxy() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
  }

  // Everything the proxy printed until it closed stdout (i.e. exited).
  std::string read_stdout_to_end() {
    std::string text;
    char buffer[256];
    for (;;) {
      const ssize_t got = ::read(stdout_.get(), buffer, sizeof buffer);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) return text;
      text.append(buffer, static_cast<std::size_t>(got));
    }
  }

  // Blocks until the "listening on host:port" line arrives.
  int port() {
    std::string line;
    char c;
    for (;;) {
      const ssize_t got = ::read(stdout_.get(), &c, 1);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) throw std::runtime_error("proxy exited before listening: '" + line + "'");
      if (c == '\n') break;
      line.push_back(c);
    }
    const auto colon = line.rfind(':');
    if (line.rfind("listening on ", 0) != 0 || colon == std::string::npos) {
      throw std::runtime_error("unexpected proxy output: " + line);
    }
    return std::stoi(line.substr(colon + 1));
  }

  // Waits for the proxy to exit on its own and returns its exit code.
  int wait() {
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    pid_ = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }

 private:
  pid_t pid_ = -1;
  Fd stdout_;
};

std::vector<std::string> proxy_args(int target_port, std::vector<std::string> extra = {}) {
  std::vector<std::string> args = {"--listen-port", "0", "--target-host", "127.0.0.1",
                                   "--target-port", std::to_string(target_port)};
  args.insert(args.end(), extra.begin(), extra.end());
  return args;
}

TEST(ProxyTest, ForwardsBothWaysAndPassesHalfClose) {
  int target_port = 0;
  Fd listener = listen_loopback(&target_port);
  // Echo server: returns everything it receives, then closes after EOF.
  auto server = std::async(std::launch::async, [&] {
    Fd peer = accept_one(listener.get());
    Bytes received = read_to_end(peer.get());
    send_all(peer.get(), received.data(), received.size());
    return received.size();
  });

  Proxy proxy(proxy_args(target_port, {"--max-connections", "1"}));
  Fd client = connect_loopback(proxy.port());
  const Bytes payload = random_bytes(1 << 20, 11);
  ASSERT_TRUE(send_all(client.get(), payload.data(), payload.size()));
  ::shutdown(client.get(), SHUT_WR);  // the echo server waits for this EOF
  const Bytes echoed = read_to_end(client.get());

  EXPECT_EQ(server.get(), payload.size());
  EXPECT_TRUE(same_bytes(payload, echoed));
  client.reset();
  EXPECT_EQ(proxy.wait(), 0);
}

TEST(ProxyTest, CutsUpstreamAfterExactlyNBytes) {
  int target_port = 0;
  Fd listener = listen_loopback(&target_port);
  auto server = std::async(std::launch::async, [&] {
    Fd peer = accept_one(listener.get());
    return read_to_end(peer.get());
  });

  Proxy proxy(proxy_args(target_port, {"--drop-after-bytes", "12345", "--drop-direction", "up",
                                       "--max-connections", "1"}));
  Fd client = connect_loopback(proxy.port());
  const Bytes payload = random_bytes(300000, 12);
  // The proxy may cut us off part way, so a failed send is expected.
  send_all(client.get(), payload.data(), payload.size());
  // The client sees the connection end without receiving anything.
  EXPECT_TRUE(read_to_end(client.get()).empty());

  const Bytes received = server.get();
  ASSERT_EQ(received.size(), 12345u);
  EXPECT_TRUE(same_bytes(Bytes(payload.begin(), payload.begin() + 12345), received));
  client.reset();
  EXPECT_EQ(proxy.wait(), 0);
}

TEST(ProxyTest, CutsDownstreamAfterExactlyNBytes) {
  int target_port = 0;
  Fd listener = listen_loopback(&target_port);
  const Bytes payload = random_bytes(200000, 13);
  auto server = std::async(std::launch::async, [&] {
    Fd peer = accept_one(listener.get());
    // Reply only after the request is in, so the cut cannot race it.
    const std::size_t request_size = read_exactly(peer.get(), 5000).size();
    send_all(peer.get(), payload.data(), payload.size());
    read_to_end(peer.get());  // ends when the proxy cuts the connection
    return request_size;
  });

  Proxy proxy(proxy_args(target_port, {"--drop-after-bytes", "7000", "--drop-direction", "down",
                                       "--max-connections", "1"}));
  Fd client = connect_loopback(proxy.port());
  // Upstream bytes do not count in this mode.
  const Bytes request = random_bytes(5000, 14);
  ASSERT_TRUE(send_all(client.get(), request.data(), request.size()));
  const Bytes received = read_to_end(client.get());

  ASSERT_EQ(received.size(), 7000u);
  EXPECT_TRUE(same_bytes(Bytes(payload.begin(), payload.begin() + 7000), received));
  EXPECT_EQ(server.get(), request.size());
  client.reset();
  EXPECT_EQ(proxy.wait(), 0);
}

TEST(ProxyTest, BothDirectionsShareOneBudget) {
  int target_port = 0;
  Fd listener = listen_loopback(&target_port);
  const Bytes reply = random_bytes(10000, 15);
  auto server = std::async(std::launch::async, [&] {
    Fd peer = accept_one(listener.get());
    // Only answer after the whole request arrived, so the order in which
    // the budget is spent is fixed: 100 bytes up, then 900 down.
    const Bytes request = read_exactly(peer.get(), 100);
    send_all(peer.get(), reply.data(), reply.size());
    read_to_end(peer.get());
    return request.size();
  });

  Proxy proxy(proxy_args(target_port, {"--drop-after-bytes", "1000", "--max-connections", "1"}));
  Fd client = connect_loopback(proxy.port());
  const Bytes request = random_bytes(100, 16);
  ASSERT_TRUE(send_all(client.get(), request.data(), request.size()));
  const Bytes received = read_to_end(client.get());

  EXPECT_EQ(server.get(), 100u);
  ASSERT_EQ(received.size(), 900u);
  EXPECT_TRUE(same_bytes(Bytes(reply.begin(), reply.begin() + 900), received));
  client.reset();
  EXPECT_EQ(proxy.wait(), 0);
}

TEST(ProxyTest, ZeroBudgetCutsBeforeForwardingAnything) {
  int target_port = 0;
  Fd listener = listen_loopback(&target_port);
  auto server = std::async(std::launch::async, [&] {
    Fd peer = accept_one(listener.get());
    return read_to_end(peer.get()).size();
  });

  Proxy proxy(proxy_args(target_port, {"--drop-after-bytes", "0", "--max-connections", "1"}));
  Fd client = connect_loopback(proxy.port());
  const Bytes payload = random_bytes(1000, 17);
  send_all(client.get(), payload.data(), payload.size());
  EXPECT_TRUE(read_to_end(client.get()).empty());
  EXPECT_EQ(server.get(), 0u);
  client.reset();
  EXPECT_EQ(proxy.wait(), 0);
}

TEST(ProxyTest, CutsOnlyTheFirstConnectionsWhenAsked) {
  int target_port = 0;
  Fd listener = listen_loopback(&target_port);
  auto server = std::async(std::launch::async, [&] {
    std::vector<std::size_t> sizes;
    for (int i = 0; i < 3; ++i) {
      Fd peer = accept_one(listener.get());
      sizes.push_back(read_to_end(peer.get()).size());
    }
    return sizes;
  });

  Proxy proxy(proxy_args(target_port, {"--drop-after-bytes", "4096", "--drop-direction", "up",
                                       "--drop-connections", "2", "--max-connections", "3"}));
  const int port = proxy.port();
  const Bytes payload = random_bytes(50000, 18);
  for (int i = 0; i < 3; ++i) {
    Fd client = connect_loopback(port);
    send_all(client.get(), payload.data(), payload.size());
    ::shutdown(client.get(), SHUT_WR);
    read_to_end(client.get());
  }

  EXPECT_EQ(server.get(), (std::vector<std::size_t>{4096, 4096, 50000}));
  EXPECT_EQ(proxy.wait(), 0);
}

TEST(ProxyTest, ServesConcurrentConnections) {
  int target_port = 0;
  Fd listener = listen_loopback(&target_port);
  constexpr int kClients = 4;
  // Accepts every connection first, so all of them are open at once, then
  // echoes each.
  auto server = std::async(std::launch::async, [&] {
    std::vector<Fd> peers;
    for (int i = 0; i < kClients; ++i) peers.push_back(accept_one(listener.get()));
    std::vector<std::thread> echoes;
    for (Fd& peer : peers) {
      echoes.emplace_back([&peer] {
        Bytes received = read_to_end(peer.get());
        send_all(peer.get(), received.data(), received.size());
        ::shutdown(peer.get(), SHUT_WR);
      });
    }
    for (std::thread& t : echoes) t.join();
  });

  Proxy proxy(proxy_args(target_port, {"--max-connections", std::to_string(kClients)}));
  const int port = proxy.port();
  std::vector<std::future<bool>> clients;
  for (int i = 0; i < kClients; ++i) {
    clients.push_back(std::async(std::launch::async, [port, i] {
      Fd client = connect_loopback(port);
      const Bytes payload = random_bytes(100000 + static_cast<std::size_t>(i) * 777,
                                         100 + static_cast<std::uint64_t>(i));
      if (!send_all(client.get(), payload.data(), payload.size())) return false;
      ::shutdown(client.get(), SHUT_WR);
      return read_to_end(client.get()) == payload;
    }));
  }
  for (auto& client : clients) EXPECT_TRUE(client.get());
  server.get();
  EXPECT_EQ(proxy.wait(), 0);
}

TEST(ProxyTest, DelayStillForwardsEverything) {
  int target_port = 0;
  Fd listener = listen_loopback(&target_port);
  auto server = std::async(std::launch::async, [&] {
    Fd peer = accept_one(listener.get());
    Bytes received = read_to_end(peer.get());
    send_all(peer.get(), received.data(), received.size());
  });

  Proxy proxy(proxy_args(target_port, {"--delay-ms", "1", "--max-connections", "1"}));
  Fd client = connect_loopback(proxy.port());
  const Bytes payload = random_bytes(20000, 19);
  ASSERT_TRUE(send_all(client.get(), payload.data(), payload.size()));
  ::shutdown(client.get(), SHUT_WR);
  EXPECT_TRUE(same_bytes(payload, read_to_end(client.get())));
  server.get();
  client.reset();
  EXPECT_EQ(proxy.wait(), 0);
}

TEST(ProxyTest, UnreachableTargetClosesTheClient) {
  // Reserve a port, then close it so nothing listens there.
  int dead_port = 0;
  { Fd reserved = listen_loopback(&dead_port); }

  Proxy proxy(proxy_args(dead_port, {"--max-connections", "1"}));
  Fd client = connect_loopback(proxy.port());
  EXPECT_TRUE(read_to_end(client.get()).empty());
  client.reset();
  EXPECT_EQ(proxy.wait(), 0);
}

TEST(ProxyTest, RejectsBadArguments) {
  const std::vector<std::vector<std::string>> cases = {
      {},
      {"--listen-port", "0", "--target-port", "1"},
      {"--listen-port", "0", "--target-host", "127.0.0.1"},
      {"--listen-port", "70000", "--target-host", "127.0.0.1", "--target-port", "1"},
      {"--listen-port", "0", "--target-host", "127.0.0.1", "--target-port", "1",
       "--drop-direction", "sideways"},
      {"--listen-port", "0", "--target-host", "127.0.0.1", "--target-port", "1",
       "--drop-after-bytes", "-5"},
      {"--listen-port", "0", "--target-host", "127.0.0.1", "--target-port", "1", "--bogus"},
      {"--listen-port"},
  };
  for (const auto& args : cases) {
    Proxy proxy(args);
    EXPECT_EQ(proxy.read_stdout_to_end(), "");
    EXPECT_EQ(proxy.wait(), 2) << "args: " << ::testing::PrintToString(args);
  }
}

TEST(ProxyTest, HelpExitsCleanly) {
  Proxy proxy({"--help"});
  EXPECT_NE(proxy.read_stdout_to_end().find("--drop-after-bytes"), std::string::npos);
  EXPECT_EQ(proxy.wait(), 0);
}

// error_message() is the thread-safe errno text the proxy and the core
// library use for diagnostics.
TEST(ErrorMessageTest, MatchesTheSystemDescription) {
  EXPECT_EQ(error_message(ENOENT), std::string(std::strerror(ENOENT)));
  EXPECT_EQ(error_message(ECONNREFUSED), std::string(std::strerror(ECONNREFUSED)));
  EXPECT_FALSE(error_message(987654).empty());
  const IoError error("open", "/no/such/file", ENOENT);
  EXPECT_NE(std::string(error.what()).find(std::strerror(ENOENT)), std::string::npos);
  EXPECT_EQ(error.error_number(), ENOENT);
}

TEST(ErrorMessageTest, IsConsistentAcrossThreads) {
  const std::string expected_noent = error_message(ENOENT);
  const std::string expected_pipe = error_message(EPIPE);
  std::vector<std::future<bool>> workers;
  for (int t = 0; t < 4; ++t) {
    workers.push_back(std::async(std::launch::async, [&, t] {
      for (int i = 0; i < 2000; ++i) {
        const bool noent = (i + t) % 2 == 0;
        if (error_message(noent ? ENOENT : EPIPE) != (noent ? expected_noent : expected_pipe)) {
          return false;
        }
      }
      return true;
    }));
  }
  for (auto& worker : workers) EXPECT_TRUE(worker.get());
}

}  // namespace
}  // namespace deltasync::test
