#include "deltasync/net.h"

#include <gtest/gtest.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;

struct Pair {
  Socket client;
  Socket server;
};

// A connected TCP pair on the loopback interface. connect() completes
// through the listen backlog, so one thread can set up both ends.
Pair loopback_pair(int timeout_ms = 10000) {
  Listener listener("127.0.0.1", 0);
  Pair pair;
  pair.client = connect_to("127.0.0.1", listener.port(), timeout_ms);
  pair.server = listener.accept(timeout_ms);
  return pair;
}

TEST(NetTest, ListenerOnPortZeroReportsTheChosenPort) {
  Listener listener("127.0.0.1", 0);
  EXPECT_NE(listener.port(), 0);
  EXPECT_EQ(listener.address(), "127.0.0.1:" + std::to_string(listener.port()));
}

TEST(NetTest, MovesBytesBothWaysAndCountsThem) {
  Pair pair = loopback_pair();
  const Bytes ping = test::bytes_of("ping");
  const Bytes pong = test::bytes_of("pong!");
  pair.client.write_all(ping.data(), ping.size());
  Bytes received(ping.size());
  pair.server.read_exact(received.data(), received.size());
  EXPECT_EQ(received, ping);
  pair.server.write_all(pong.data(), pong.size());
  received.resize(pong.size());
  pair.client.read_exact(received.data(), received.size());
  EXPECT_EQ(received, pong);

  EXPECT_EQ(pair.client.bytes_sent(), ping.size());
  EXPECT_EQ(pair.client.bytes_received(), pong.size());
  EXPECT_EQ(pair.server.bytes_sent(), pong.size());
  EXPECT_EQ(pair.server.bytes_received(), ping.size());
}

TEST(NetTest, LargeTransfersSurvivePartialReadsAndWrites) {
  // Far more than the socket buffers hold, so writes block and complete in
  // pieces while the reader takes odd-sized bites.
  Pair pair = loopback_pair();
  const Bytes data = test::random_bytes(8 << 20, 1001);
  std::thread writer([&] { pair.client.write_all(data.data(), data.size()); });
  Bytes received;
  std::vector<std::uint8_t> buffer(7777);
  while (received.size() < data.size()) {
    const std::size_t count = pair.server.read_some(buffer.data(), buffer.size());
    if (count == 0) break;
    received.insert(received.end(), buffer.begin(), buffer.begin() + count);
  }
  writer.join();
  EXPECT_TRUE(test::same_bytes(data, received));
  EXPECT_EQ(pair.client.bytes_sent(), data.size());
  EXPECT_EQ(pair.server.bytes_received(), data.size());
}

TEST(NetTest, EndOfStreamAndEarlyClose) {
  Pair pair = loopback_pair();
  const Bytes data = test::bytes_of("abc");
  pair.client.write_all(data.data(), data.size());
  pair.client.shutdown_write();
  Bytes two(2);
  pair.server.read_exact(two.data(), two.size());
  // One byte is left, but three are asked for: the peer closed too early.
  Bytes three(3);
  EXPECT_THROW(pair.server.read_exact(three.data(), three.size()), NetError);
  EXPECT_EQ(pair.server.read_some(three.data(), three.size()), 0u);
}

TEST(NetTest, WritingToAClosedPeerIsAnErrorNotASignal) {
  // Without MSG_NOSIGNAL / SO_NOSIGPIPE the process would die of SIGPIPE.
  Pair pair = loopback_pair();
  pair.server = Socket();  // closes the receiving end
  const Bytes chunk(1 << 20, 7);
  EXPECT_THROW(
      {
        for (int i = 0; i < 1000; ++i) pair.client.write_all(chunk.data(), chunk.size());
      },
      NetError);
}

TEST(NetTest, ReadTimesOutWithoutData) {
  Pair pair = loopback_pair(50);
  std::uint8_t byte = 0;
  EXPECT_THROW(pair.server.read_exact(&byte, 1), TimeoutError);
  EXPECT_FALSE(pair.server.readable(0));
}

TEST(NetTest, ReadableSeesDataAndEndOfStream) {
  Pair pair = loopback_pair();
  EXPECT_FALSE(pair.server.readable(0));
  const std::uint8_t byte = 1;
  pair.client.write_all(&byte, 1);
  EXPECT_TRUE(pair.server.readable(10000));
  std::uint8_t out = 0;
  pair.server.read_exact(&out, 1);
  pair.client.shutdown_write();
  EXPECT_TRUE(pair.server.readable(10000));
  EXPECT_EQ(pair.server.read_some(&out, 1), 0u);
}

TEST(NetTest, ShutdownFromAnotherThreadWakesABlockedReader) {
  Pair pair = loopback_pair(0);  // no timeout: only the shutdown can end the read
  const int fd = pair.server.fd();
  std::thread reader([&] {
    std::uint8_t byte = 0;
    EXPECT_EQ(pair.server.read_some(&byte, 1), 0u);
  });
  ::shutdown(fd, SHUT_RDWR);
  reader.join();
}

TEST(NetTest, ConnectFailuresAreNetErrors) {
  std::uint16_t port = 0;
  {
    Listener listener("127.0.0.1", 0);
    port = listener.port();
  }
  // Nothing listens there any more (barring a race with another process).
  EXPECT_THROW(connect_to("127.0.0.1", port, 2000), NetError);
}

void ignore_signal(int) {}

TEST(NetTest, InterruptedCallsAreRestarted) {
  // A handler installed without SA_RESTART makes blocking calls fail with
  // EINTR. The reader must carry on regardless of when the signals land.
  struct sigaction action {};
  struct sigaction previous {};
  action.sa_handler = ignore_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  ASSERT_EQ(sigaction(SIGUSR1, &action, &previous), 0);

  Pair pair = loopback_pair();
  const Bytes data = test::random_bytes(1 << 20, 1002);
  Bytes received(data.size());
  std::atomic<bool> started{false};
  std::thread reader([&] {
    started = true;
    pair.server.read_exact(received.data(), received.size());
  });
  while (!started) std::this_thread::yield();
  for (int i = 0; i < 20; ++i) {
    pthread_kill(reader.native_handle(), SIGUSR1);
    std::this_thread::yield();
  }
  pair.client.write_all(data.data(), data.size());
  reader.join();
  EXPECT_TRUE(test::same_bytes(data, received));
  sigaction(SIGUSR1, &previous, nullptr);
}

}  // namespace
}  // namespace deltasync
