// Interrupted syncs that resume: a SyncServer on loopback, push_file() as
// the client, and an in-process forwarder between them that cuts a
// connection after an exact number of sender-to-receiver bytes.
//
// Synchronization is by data only: the forwarder forwards exactly the bytes
// it is told to, the receiver reads everything it was sent before it sees
// the connection end, and each step waits for the server's session report.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "deltasync/net.h"
#include "deltasync/resume.h"
#include "deltasync/sync.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;

// Only safety nets so a broken build fails instead of hanging; no test
// relies on how long anything takes.
constexpr auto kReportWait = std::chrono::seconds(120);
constexpr int kTimeoutMs = 20000;

constexpr std::uint64_t kNoCut = std::numeric_limits<std::uint64_t>::max();

// Accepts one connection per entry of `cuts` and forwards it to the
// receiver. Connection i is shut down in both directions once cuts[i]
// sender-to-receiver bytes have been forwarded; the receiver still gets
// every one of those bytes, followed by end of stream.
class CuttingForwarder {
 public:
  CuttingForwarder(std::uint16_t target_port, std::vector<std::uint64_t> cuts,
                   std::function<void(std::size_t)> before_connect = {})
      : listener_("127.0.0.1", 0) {
    thread_ = std::thread([this, target_port, cuts, before_connect] {
      for (std::size_t i = 0; i < cuts.size(); ++i) {
        try {
          Socket client = listener_.accept(kTimeoutMs);
          // Lets a test hold a reconnecting sender back until the previous
          // session has ended on the receiver.
          if (before_connect) before_connect(i);
          Socket server = connect_to("127.0.0.1", target_port, kTimeoutMs);
          forward(client, server, cuts[i]);
        } catch (const std::exception& error) {
          const std::lock_guard<std::mutex> guard(mutex_);
          errors_ += error.what();
        }
      }
    });
  }
  ~CuttingForwarder() { thread_.join(); }

  std::uint16_t port() const { return listener_.port(); }

  std::string errors() {
    const std::lock_guard<std::mutex> guard(mutex_);
    return errors_;
  }

 private:
  static void forward(Socket& client, Socket& server, std::uint64_t cut) {
    std::thread down([&client, &server] {
      std::vector<std::uint8_t> buffer(64 * 1024);
      try {
        for (;;) {
          const std::size_t count = server.read_some(buffer.data(), buffer.size());
          if (count == 0) break;
          client.write_all(buffer.data(), count);
        }
        client.shutdown_write();
      } catch (const NetError&) {
        // The connection was cut; the other direction handles it.
      }
    });
    std::vector<std::uint8_t> buffer(64 * 1024);
    std::uint64_t forwarded = 0;
    try {
      for (;;) {
        if (forwarded == cut) {
          client.shutdown_both();
          server.shutdown_both();
          break;
        }
        const auto want = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), cut - forwarded));
        const std::size_t count = client.read_some(buffer.data(), want);
        if (count == 0) {
          server.shutdown_write();
          break;
        }
        server.write_all(buffer.data(), count);
        forwarded += count;
      }
    } catch (const NetError&) {
      client.shutdown_both();
      server.shutdown_both();
    }
    down.join();
  }

  Listener listener_;
  std::thread thread_;
  std::mutex mutex_;
  std::string errors_;
};

class ResumeSyncTest : public ::testing::Test {
 protected:
  static constexpr std::uint64_t kChunk = 64 * 1024;

  void SetUp() override {
    ServerOptions options;
    options.root = root_.path();
    options.bind_address = "127.0.0.1";
    options.timeout_ms = kTimeoutMs;
    options.on_session = [this](const SessionReport& report) {
      const std::lock_guard<std::mutex> guard(mutex_);
      reports_.push_back(report);
      reported_.notify_all();
    };
    server_ = std::make_unique<SyncServer>(options);
    thread_ = std::thread([this] { server_->run(); });
  }

  void TearDown() override {
    server_->stop();
    thread_.join();
  }

  // Waits until the server has reported `count` sessions in total.
  std::vector<SessionReport> wait_for_reports(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool done =
        reported_.wait_for(lock, kReportWait, [&] { return reports_.size() >= count; });
    EXPECT_TRUE(done) << "only " << reports_.size() << " of " << count << " sessions reported";
    return reports_;
  }

  std::size_t report_count() {
    const std::lock_guard<std::mutex> guard(mutex_);
    return reports_.size();
  }

  PushOptions options_for(const std::string& name, const Bytes& data, std::uint16_t port,
                          TransferMode mode) {
    test::write_file(local_.file(name), data);
    PushOptions options;
    options.local_path = local_.file(name);
    options.host = "127.0.0.1";
    options.port = port;
    options.timeout_ms = kTimeoutMs;
    options.retry_delay_ms = 0;
    options.chunk_size = kChunk;
    options.mode = mode;
    return options;
  }

  // Pushes through a forwarder that cuts after `cut` bytes; the push must
  // fail with a connection error. Returns once the receiver has ended the
  // session.
  void interrupted_push(const std::string& name, const Bytes& data, std::uint64_t cut,
                        TransferMode mode) {
    const std::size_t before = report_count();
    {
      CuttingForwarder forwarder(server_->port(), {cut});
      EXPECT_THROW(push_file(options_for(name, data, forwarder.port(), mode)), NetError)
          << "cut after " << cut << " bytes";
    }
    const SessionReport report = wait_for_reports(before + 1).at(before);
    EXPECT_FALSE(report.ok);
  }

  PushStats direct_push(const std::string& name, const Bytes& data, TransferMode mode) {
    const std::size_t before = report_count();
    const PushStats stats = push_file(options_for(name, data, server_->port(), mode));
    EXPECT_TRUE(wait_for_reports(before + 1).at(before).ok);
    return stats;
  }

  std::string remote(const std::string& name) const { return root_.file(name); }

  test::TempDir root_;
  test::TempDir local_;
  std::unique_ptr<SyncServer> server_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable reported_;
  std::vector<SessionReport> reports_;
};

// Interrupts a whole-file push at many points, from inside the handshake to
// the last bytes of the delta, and resumes each with a direct push. The
// receiver resumes from the last chunk it verified before the cut: nothing
// verified is sent again, so the resumed push sends roughly the rest of the
// file instead of all of it.
TEST_F(ResumeSyncTest, InterruptionAtManyPointsResumesFromTheLastVerifiedChunk) {
  const Bytes data = test::random_bytes(2 * 1024 * 1024 + 4321, 5100);
  // What a full transfer costs, measured on a separate name of the same
  // length (the name is part of FILE_INFO).
  const PushStats full = direct_push("ref", data, TransferMode::kWholeFile);
  ASSERT_EQ(full.resumed_from, 0u);

  // Frames, the delta header, literal record headers and CHUNK records:
  // well under this many bytes per run of a few MiB.
  const std::uint64_t kOverhead = 4096;
  const std::uint64_t cuts[] = {0,
                                1,
                                60,  // inside FILE_INFO
                                200,
                                kChunk - 1,
                                kChunk + 100,
                                3 * kChunk + 7,
                                full.bytes_sent / 3,
                                full.bytes_sent / 2,
                                2 * full.bytes_sent / 3,
                                full.bytes_sent - 2 * kChunk,
                                full.bytes_sent - 100,
                                full.bytes_sent - 1};
  int index = 0;
  for (const std::uint64_t cut : cuts) {
    SCOPED_TRACE("cut after " + std::to_string(cut) + " bytes");
    const std::string name = (index < 10 ? "f0" : "f") + std::to_string(index);
    ++index;
    interrupted_push(name, data, cut, TransferMode::kWholeFile);
    EXPECT_FALSE(test::file_exists(remote(name)));

    const PushStats resumed = direct_push(name, data, TransferMode::kWholeFile);
    EXPECT_TRUE(test::same_bytes(data, test::read_file(remote(name))));
    EXPECT_EQ(test::list_directory(root_.path()).size(), static_cast<std::size_t>(index + 1))
        << "partial state left behind";

    // Never more than was delivered, and at most one chunk (plus framing)
    // less than that.
    EXPECT_EQ(resumed.resumed_from % kChunk, 0u);
    EXPECT_LE(resumed.resumed_from, cut);
    if (cut > kChunk + kOverhead) {
      EXPECT_GE(resumed.resumed_from + kChunk + kOverhead, cut);
    }
    // The resumed push sends only what was not yet verified.
    EXPECT_EQ(resumed.literal_bytes, data.size() - resumed.resumed_from);
    EXPECT_LE(resumed.bytes_sent + resumed.resumed_from, full.bytes_sent + kOverhead);
    if (cut >= full.bytes_sent / 2) {
      EXPECT_LT(resumed.bytes_sent, full.bytes_sent / 2 + kChunk + kOverhead);
    }
  }
}

// The same in delta mode, where the target shares only its first half with
// the basis: the resumed attempt continues the delta from the verified
// prefix against the unchanged basis.
TEST_F(ResumeSyncTest, DeltaPushResumesAgainstTheBasis) {
  const Bytes basis = test::random_bytes(1536 * 1024, 5200);
  Bytes target(basis.begin(), basis.begin() + basis.size() / 2);
  const Bytes fresh = test::random_bytes(1024 * 1024, 5201);
  target.insert(target.end(), fresh.begin(), fresh.end());

  test::write_file(remote("doc"), basis);
  const PushStats full = direct_push("reference", target, TransferMode::kWholeFile);
  test::write_file(remote("reference"), basis);
  const PushStats whole_delta = direct_push("reference", target, TransferMode::kDelta);
  ASSERT_GE(whole_delta.matched_bytes + 4096, basis.size() / 2);

  // Cut inside the literal half, after about three quarters of the delta.
  const std::uint64_t cut = whole_delta.bytes_sent * 3 / 4;
  interrupted_push("doc", target, cut, TransferMode::kDelta);
  EXPECT_TRUE(test::same_bytes(basis, test::read_file(remote("doc"))));

  const PushStats resumed = direct_push("doc", target, TransferMode::kDelta);
  EXPECT_TRUE(test::same_bytes(target, test::read_file(remote("doc"))));
  EXPECT_GT(resumed.resumed_from, basis.size() / 2);
  EXPECT_LT(resumed.bytes_sent, whole_delta.bytes_sent / 2);
  EXPECT_LT(resumed.bytes_sent, full.bytes_sent / 4);
  EXPECT_EQ(resumed.literal_bytes, target.size() - resumed.resumed_from);
}

// push --retries: the first connection is cut, the sender reconnects on
// its own and the second attempt resumes.
TEST_F(ResumeSyncTest, RetriesReconnectAndResume) {
  const Bytes data = test::random_bytes(1024 * 1024 + 99, 5300);
  const std::uint64_t cut = 700 * 1024;
  PushStats stats;
  {
    // The second connection is forwarded only after the receiver ended the
    // first session, which still holds the name until then.
    CuttingForwarder forwarder(server_->port(), {cut, kNoCut},
                               [this](std::size_t connection) { wait_for_reports(connection); });
    PushOptions options = options_for("retried", data, forwarder.port(), TransferMode::kWholeFile);
    options.retries = 3;
    stats = push_file(options);
    EXPECT_EQ(forwarder.errors(), "");
  }
  EXPECT_TRUE(test::same_bytes(data, test::read_file(remote("retried"))));
  EXPECT_EQ(stats.attempts, 2u);
  EXPECT_GT(stats.resumed_from, 0u);
  EXPECT_LE(stats.resumed_from, cut);
  EXPECT_EQ(stats.literal_bytes, data.size() - stats.resumed_from);
  // Both attempts together send little more than one full transfer.
  EXPECT_LT(stats.bytes_sent, data.size() + data.size() / 4);

  const std::string json = stats_json(stats);
  EXPECT_NE(json.find("\"attempts\": 2"), std::string::npos) << json;
  EXPECT_NE(json.find("\"resumed_from\": " + std::to_string(stats.resumed_from)),
            std::string::npos)
      << json;
  const std::vector<SessionReport> reports = wait_for_reports(2);
  EXPECT_FALSE(reports[0].ok);
  EXPECT_TRUE(reports[1].ok);
  EXPECT_EQ(reports[1].resumed_from, stats.resumed_from);
}

// Partial state belongs to one exact target. If the sender's file changed
// in between, the receiver starts over and the old partial state is gone.
TEST_F(ResumeSyncTest, ChangedFileStartsOverFromZero) {
  const Bytes first = test::random_bytes(1024 * 1024, 5400);
  interrupted_push("moving", first, 600 * 1024, TransferMode::kWholeFile);
  EXPECT_TRUE(test::file_exists(remote(".moving.journal")));

  Bytes second = first;
  second[10] ^= 0xff;
  const PushStats stats = direct_push("moving", second, TransferMode::kWholeFile);
  EXPECT_EQ(stats.resumed_from, 0u);
  EXPECT_TRUE(test::same_bytes(second, test::read_file(remote("moving"))));
  EXPECT_EQ(test::list_directory(root_.path()), std::vector<std::string>{"moving"});
}

// A receiver whose partial state claims a prefix the sender does not have
// (here: written for the right target id, but with wrong bytes) is turned
// down by the sender's prefix check; the receiver then rebuilds from zero,
// still keeping the replaced file's permissions.
TEST_F(ResumeSyncTest, SenderDeclinesAPrefixItDoesNotHave) {
  const Bytes data = test::random_bytes(512 * 1024, 5500);
  test::write_file(remote("claimed"), test::bytes_of("old version"));
  ASSERT_EQ(::chmod(remote("claimed").c_str(), 0640), 0);
  {
    TargetId target;
    target.size = data.size();
    target.digest = Sha256::hash(data.data(), data.size());
    target.chunk_size = kChunk;
    PartialOutput partial(remote("claimed"), target);
    Bytes wrong(data.begin(), data.begin() + 2 * kChunk);
    wrong[5] ^= 1;
    partial.write(wrong.data(), wrong.size());
    Sha256 prefix;
    prefix.update(wrong.data(), wrong.size());
    partial.checkpoint(2 * kChunk, prefix);
  }  // kept for a later attempt
  const PushStats stats = direct_push("claimed", data, TransferMode::kWholeFile);
  EXPECT_EQ(stats.resumed_from, 0u);
  EXPECT_EQ(stats.literal_bytes, data.size());
  EXPECT_TRUE(test::same_bytes(data, test::read_file(remote("claimed"))));
  EXPECT_EQ(test::list_directory(root_.path()), std::vector<std::string>{"claimed"});
  struct stat info;
  ASSERT_EQ(::stat(remote("claimed").c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 07777, 0640u);
}

// Partial data damaged on disk between attempts is never resumed: the
// receiver rehashes the verified prefix on reopen and starts over.
TEST_F(ResumeSyncTest, DamagedPartialDataStartsOverFromZero) {
  const Bytes data = test::random_bytes(1024 * 1024, 5600);
  interrupted_push("bad", data, 700 * 1024, TransferMode::kWholeFile);
  ASSERT_TRUE(test::file_exists(remote(".bad.journal")));

  Bytes partial = test::read_file(remote(".bad.partial"));
  ASSERT_GT(partial.size(), kChunk);
  partial[kChunk / 2] ^= 0x40;
  test::write_file(remote(".bad.partial"), partial);

  const PushStats stats = direct_push("bad", data, TransferMode::kWholeFile);
  EXPECT_EQ(stats.resumed_from, 0u);
  EXPECT_TRUE(test::same_bytes(data, test::read_file(remote("bad"))));
  EXPECT_EQ(test::list_directory(root_.path()), std::vector<std::string>{"bad"});
}

}  // namespace
}  // namespace deltasync
