// End-to-end syncs over loopback TCP: a SyncServer on a thread (port 0) and
// push_file() as the client, plus hand-written peers that misbehave.

#include "deltasync/sync.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "deltasync/delta_encoder.h"
#include "deltasync/delta_format.h"
#include "deltasync/signature.h"
#include "edit_model.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;

// Only a safety net so a broken build fails instead of hanging; no test
// relies on how long anything takes.
constexpr auto kReportWait = std::chrono::seconds(120);
constexpr int kTimeoutMs = 20000;

class SyncServerTest : public ::testing::Test {
 protected:
  void SetUp() override { start_server(); }
  void TearDown() override { stop_server(); }

  void start_server(unsigned max_connections = 16,
                    int progress_interval_ms = kDefaultProgressIntervalMs) {
    ServerOptions options;
    options.root = root_.path();
    options.bind_address = "127.0.0.1";
    options.timeout_ms = kTimeoutMs;
    options.progress_interval_ms = progress_interval_ms;
    options.max_connections = max_connections;
    options.on_session = [this](const SessionReport& report) {
      const std::lock_guard<std::mutex> guard(mutex_);
      reports_.push_back(report);
      reported_.notify_all();
    };
    server_ = std::make_unique<SyncServer>(options);
    thread_ = std::thread([this] { server_->run(); });
  }

  void stop_server() {
    if (!server_) return;
    server_->stop();
    thread_.join();
    server_.reset();
  }

  // Waits until the server has reported `count` sessions in total.
  std::vector<SessionReport> wait_for_reports(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool done =
        reported_.wait_for(lock, kReportWait, [&] { return reports_.size() >= count; });
    EXPECT_TRUE(done) << "only " << reports_.size() << " of " << count << " sessions reported";
    return reports_;
  }

  PushOptions push_options(const std::string& name) const {
    PushOptions options;
    options.local_path = local_.file(name);
    options.host = "127.0.0.1";
    options.port = server_->port();
    options.timeout_ms = kTimeoutMs;
    options.retry_delay_ms = 0;
    return options;
  }

  PushStats push(const std::string& name, const Bytes& data,
                 TransferMode mode = TransferMode::kDelta) {
    test::write_file(local_.file(name), data);
    PushOptions options = push_options(name);
    options.mode = mode;
    return push_file(options);
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

TEST_F(SyncServerTest, ManyEditPatternsSyncCorrectly) {
  using test::EditKind;
  using test::Where;
  test::Rng rng(2001);
  int case_number = 0;
  for (const bool text_like : {false, true}) {
    for (const EditKind kind :
         {EditKind::kInsert, EditKind::kDelete, EditKind::kOverwrite, EditKind::kMove}) {
      for (const Where where : {Where::kStart, Where::kMiddle, Where::kEnd, Where::kAnywhere}) {
        const std::string name = "case" + std::to_string(case_number++);
        SCOPED_TRACE(name + " " + test::name_of(kind) + " at " + test::name_of(where) +
                     (text_like ? " text" : " random"));
        const Bytes basis = test::make_basis(200000 + rng.below(50000), text_like, rng);
        Bytes target = basis;
        test::apply_edit(target, kind, where, 1 + rng.below(5000), rng);
        test::apply_edit(target, EditKind::kOverwrite, Where::kAnywhere, 100, rng);
        test::write_file(remote(name), basis);

        const PushStats stats = push(name, target);
        EXPECT_TRUE(test::same_bytes(target, test::read_file(remote(name))));
        EXPECT_EQ(stats.matched_bytes + stats.literal_bytes, target.size());
        EXPECT_GT(stats.matched_bytes, 0u);
        EXPECT_LT(stats.delta_bytes, target.size() / 4);
      }
    }
  }
  const std::vector<SessionReport> reports = wait_for_reports(32);
  for (const SessionReport& report : reports) EXPECT_TRUE(report.ok) << report.error;
  // No temporary files are left behind.
  EXPECT_EQ(test::list_directory(root_.path()).size(), 32u);
}

TEST_F(SyncServerTest, LargeFileWithASmallEditSendsKilobytes) {
  const std::size_t size = 64 << 20;
  const Bytes basis = test::random_bytes(size, 2002);
  Bytes target = basis;
  const Bytes edit = test::random_bytes(4096, 2003);
  std::copy(edit.begin(), edit.end(), target.begin() + size / 2 + 1000);
  test::write_file(remote("big"), basis);

  const PushStats stats = push("big", target);
  EXPECT_TRUE(test::same_bytes(target, test::read_file(remote("big"))));
  // Everything the sender put on the wire (delta plus protocol overhead)
  // stays under 1% of the file; the signature flows the other way.
  EXPECT_LT(stats.bytes_sent, size / 100);
  EXPECT_LT(stats.literal_bytes, 3u * default_block_size(size));
  EXPECT_EQ(stats.block_size, default_block_size(size));
  std::cout << "64 MiB file, 4 KiB edit: sender->receiver " << stats.bytes_sent
            << " bytes (delta " << stats.delta_bytes << ", literal " << stats.literal_bytes
            << "), receiver->sender " << stats.bytes_received << " bytes (signature "
            << stats.signature_bytes << ")\n";
  RecordProperty("bytes_sent", std::to_string(stats.bytes_sent));
  RecordProperty("signature_bytes", std::to_string(stats.signature_bytes));
}

TEST_F(SyncServerTest, ByteAccountingAgreesOnBothSides) {
  const Bytes basis = test::random_bytes(500000, 2004);
  Bytes target = basis;
  target.insert(target.begin() + 1000, 3000, 9);
  test::write_file(remote("f"), basis);
  const PushStats stats = push("f", target);

  const SessionReport report = wait_for_reports(1).at(0);
  ASSERT_TRUE(report.ok) << report.error;
  EXPECT_EQ(report.name, "f");
  EXPECT_TRUE(report.had_basis);
  EXPECT_EQ(stats.bytes_sent, report.bytes_received);
  EXPECT_EQ(stats.bytes_received, report.bytes_sent);
  EXPECT_EQ(stats.receiver.receiver_bytes_received, stats.bytes_sent);
  EXPECT_EQ(stats.signature_bytes, report.signature_bytes);
  EXPECT_EQ(stats.delta_bytes, report.delta_bytes);
  EXPECT_GT(stats.signature_bytes, 0u);
  // Frame headers and the handshake are the only overhead.
  EXPECT_GT(stats.bytes_sent, stats.delta_bytes);
  EXPECT_LT(stats.bytes_sent, stats.delta_bytes + 400);
  EXPECT_GT(stats.bytes_received, stats.signature_bytes);
  EXPECT_LT(stats.bytes_received, stats.signature_bytes + 200);
  EXPECT_EQ(stats.attempts, 1u);
  EXPECT_EQ(stats.resumed_from, 0u);
  EXPECT_EQ(stats.file_size, target.size());
}

TEST_F(SyncServerTest, MissingBasisSendsEverythingThroughTheSamePath) {
  const Bytes target = test::random_bytes(300000, 2005);
  const PushStats stats = push("new", target);
  EXPECT_TRUE(test::same_bytes(target, test::read_file(remote("new"))));
  EXPECT_EQ(stats.signature_bytes, 0u);
  EXPECT_EQ(stats.literal_bytes, target.size());
  EXPECT_EQ(stats.matched_bytes, 0u);
  EXPECT_GT(stats.bytes_sent, target.size());
  const SessionReport report = wait_for_reports(1).at(0);
  EXPECT_TRUE(report.ok);
  EXPECT_FALSE(report.had_basis);
}

TEST_F(SyncServerTest, IdenticalFileSendsAlmostNothing) {
  const Bytes data = test::random_bytes(1 << 20, 2006);
  test::write_file(remote("same"), data);
  const PushStats stats = push("same", data);
  EXPECT_TRUE(test::same_bytes(data, test::read_file(remote("same"))));
  EXPECT_EQ(stats.literal_bytes, 0u);
  EXPECT_EQ(stats.matched_bytes, data.size());
  EXPECT_LT(stats.bytes_sent, 300u);
}

TEST_F(SyncServerTest, WholeFileModeIgnoresTheBasis) {
  const Bytes basis = test::random_bytes(400000, 2007);
  Bytes target = basis;
  target[1234] ^= 1;
  test::write_file(remote("w"), basis);
  const PushStats stats = push("w", target, TransferMode::kWholeFile);
  EXPECT_TRUE(test::same_bytes(target, test::read_file(remote("w"))));
  EXPECT_EQ(stats.mode, TransferMode::kWholeFile);
  EXPECT_EQ(stats.signature_bytes, 0u);
  EXPECT_EQ(stats.literal_bytes, target.size());
  EXPECT_GT(stats.bytes_sent, target.size());
  EXPECT_LT(stats.bytes_sent, target.size() + target.size() / 100);
}

TEST_F(SyncServerTest, EmptyFiles) {
  push("empty-new", Bytes());
  EXPECT_TRUE(test::read_file(remote("empty-new")).empty());
  test::write_file(remote("to-empty"), test::random_bytes(5000, 2008));
  push("to-empty", Bytes());
  EXPECT_TRUE(test::read_file(remote("to-empty")).empty());
  test::write_file(remote("from-empty"), Bytes());
  const Bytes data = test::random_bytes(5000, 2009);
  push("from-empty", data);
  EXPECT_TRUE(test::same_bytes(data, test::read_file(remote("from-empty"))));
}

TEST_F(SyncServerTest, KeepsThePermissionsOfTheReplacedFile) {
  test::write_file(remote("script"), test::bytes_of("old"));
  ASSERT_EQ(::chmod(remote("script").c_str(), 0751), 0);
  push("script", test::bytes_of("new contents"));
  struct stat info;
  ASSERT_EQ(::stat(remote("script").c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 07777, 0751u);
}

TEST_F(SyncServerTest, ChunkAndBlockSizeOptionsAreHonoured) {
  const Bytes basis = test::random_bytes(300000, 2010);
  Bytes target = basis;
  target.erase(target.begin() + 5000, target.begin() + 6000);
  test::write_file(remote("opts"), basis);
  test::write_file(local_.file("opts"), target);
  PushOptions options = push_options("opts");
  options.block_size = 2048;
  options.chunk_size = 64 * 1024;
  const PushStats stats = push_file(options);
  EXPECT_EQ(stats.block_size, 2048u);
  EXPECT_TRUE(test::same_bytes(target, test::read_file(remote("opts"))));

  options.chunk_size = 1000;  // not a multiple of 64
  EXPECT_THROW(push_file(options), ProtocolError);
}

TEST_F(SyncServerTest, RemoteNameDefaultsToTheLocalFileName) {
  const Bytes data = test::bytes_of("named after the file");
  test::write_file(local_.file("local-name.txt"), data);
  PushOptions options = push_options("local-name.txt");
  push_file(options);
  EXPECT_TRUE(test::same_bytes(data, test::read_file(remote("local-name.txt"))));
  options.remote_name = "renamed.txt";
  push_file(options);
  EXPECT_TRUE(test::same_bytes(data, test::read_file(remote("renamed.txt"))));
  options.remote_name = "../escape";
  EXPECT_THROW(push_file(options), ProtocolError);
}

TEST_F(SyncServerTest, ConcurrentClientsSyncDifferentFiles) {
  constexpr int kClients = 6;
  std::vector<Bytes> targets;
  for (int i = 0; i < kClients; ++i) {
    const std::string name = "c" + std::to_string(i);
    Bytes basis = test::random_bytes(400000, 2100 + i);
    Bytes target = basis;
    target.insert(target.begin() + 1000 * (i + 1), 500, static_cast<std::uint8_t>(i));
    if (i % 2 == 0) test::write_file(remote(name), basis);  // half start without a basis
    test::write_file(local_.file(name), target);
    targets.push_back(std::move(target));
  }
  std::vector<std::thread> clients;
  std::vector<std::string> errors(kClients);
  for (int i = 0; i < kClients; ++i) {
    clients.emplace_back([&, i] {
      try {
        push_file(push_options("c" + std::to_string(i)));
      } catch (const std::exception& error) {
        errors[i] = error.what();
      }
    });
  }
  for (std::thread& client : clients) client.join();
  for (int i = 0; i < kClients; ++i) {
    EXPECT_EQ(errors[i], "") << i;
    EXPECT_TRUE(test::same_bytes(targets[i], test::read_file(remote("c" + std::to_string(i)))));
  }
  for (const SessionReport& report : wait_for_reports(kClients)) {
    EXPECT_TRUE(report.ok) << report.error;
  }
}

// A client that speaks the protocol by hand, for driving the server into
// corners push_file() never reaches.
class RawClient {
 public:
  explicit RawClient(std::uint16_t port)
      : socket_(connect_to("127.0.0.1", port, kTimeoutMs)), channel_(socket_) {}

  Socket& socket() { return socket_; }
  FrameChannel& channel() { return channel_; }

  void send_raw(const Bytes& bytes) { socket_.write_all(bytes.data(), bytes.size()); }

  void hello(std::uint8_t version = kProtocolVersion) {
    Hello hello;
    hello.version = version;
    channel_.send(FrameType::kHello, encode_hello(hello));
  }

  // Sends HELLO and FILE_INFO, reads up to RESUME, and returns the
  // serialized delta of `target` against the server's signature.
  Bytes start(const FileInfo& info, const Bytes& target) {
    hello();
    channel_.send(FrameType::kFileInfo, encode_file_info(info));
    channel_.expect(FrameType::kHello);
    SignatureBuilder builder;
    SignatureDecoder decoder(builder);
    bool any = false;
    for (;;) {
      const FrameType type = channel_.receive();
      if (type == FrameType::kResume) {
        resume_ = decode_resume(channel_.payload());
        break;
      }
      if (type == FrameType::kProgress) {
        ++progress_frames_;
        continue;
      }
      EXPECT_EQ(type, FrameType::kSignature);
      decoder.feed(channel_.payload().data(), channel_.payload().size());
      any = true;
    }
    Signature signature;
    if (any) {
      decoder.finish();
      signature = builder.take();
    } else {
      SignatureHeader header;
      header.block_size = 1024;
      signature = Signature(header);
    }
    MemorySink delta;
    DeltaWriter writer(delta);
    DeltaOptions options;
    options.chunk_size = info.chunk_size;
    DeltaEncoder encoder(signature, writer, options);
    encoder.feed(target.data(), target.size());
    encoder.finish();
    return delta.take();
  }

  void send_data(const Bytes& delta, std::size_t begin, std::size_t end) {
    for (std::size_t offset = begin; offset < end; offset += kStreamFramePayload) {
      const std::size_t length = std::min(kStreamFramePayload, end - offset);
      channel_.send(FrameType::kData, delta.data() + offset, length);
    }
  }

  // Reads frames until ERROR and returns its code.
  ErrorCode read_error() {
    for (;;) {
      const FrameType type = channel_.receive();
      if (type == FrameType::kError) return decode_error(channel_.payload()).code;
      EXPECT_TRUE(type == FrameType::kHello || type == FrameType::kSignature ||
                  type == FrameType::kResume || type == FrameType::kProgress)
          << frame_type_name(type);
    }
  }

  // Reads frames until OK, counting the PROGRESS frames before it.
  void read_ok() {
    for (;;) {
      const FrameType type = channel_.receive();
      if (type == FrameType::kOk) return;
      ASSERT_EQ(type, FrameType::kProgress) << frame_type_name(type);
      ++progress_frames_;
    }
  }

  // RESUME as received by start().
  const ResumePoint& resume() const { return resume_; }
  // PROGRESS frames received so far.
  int progress_frames() const { return progress_frames_; }

 private:
  Socket socket_;
  FrameChannel channel_;
  ResumePoint resume_;
  int progress_frames_ = 0;
};

FileInfo info_for(const std::string& name, const Bytes& target,
                  std::uint64_t chunk_size = kDefaultChunkSize) {
  FileInfo info;
  info.name = name;
  info.size = target.size();
  info.digest = Sha256::hash(target.data(), target.size());
  info.chunk_size = chunk_size;
  return info;
}

TEST_F(SyncServerTest, MalformedHandshakesGetAnErrorFrame) {
  {
    RawClient client(server_->port());
    client.send_raw(test::bytes_of("GET / HTTP/1.0\r\n\r\n"));
    EXPECT_EQ(client.read_error(), ErrorCode::kProtocol);
  }
  {
    RawClient client(server_->port());
    client.channel().send(FrameType::kHello, test::bytes_of("NOPE\x01\x01"));
    EXPECT_EQ(client.read_error(), ErrorCode::kProtocol);
  }
  {
    RawClient client(server_->port());
    client.hello(kProtocolVersion + 1);
    EXPECT_EQ(client.read_error(), ErrorCode::kVersion);
  }
  {
    RawClient client(server_->port());
    client.channel().send(FrameType::kFileInfo, encode_file_info(info_for("x", Bytes())));
    EXPECT_EQ(client.read_error(), ErrorCode::kProtocol);
  }
  {
    RawClient client(server_->port());
    client.send_raw(Bytes{static_cast<std::uint8_t>(FrameType::kHello), 0xff, 0xff, 0xff, 0xff});
    EXPECT_EQ(client.read_error(), ErrorCode::kProtocol);
  }
  {
    RawClient client(server_->port());
    client.hello();
    client.channel().send(FrameType::kFileInfo, Bytes(10, 0));
    EXPECT_EQ(client.read_error(), ErrorCode::kProtocol);
  }
  {
    RawClient client(server_->port());
    client.hello();
    client.channel().send(FrameType::kFileInfo,
                          encode_file_info(info_for("x", Bytes(), /*chunk_size=*/1000)));
    EXPECT_EQ(client.read_error(), ErrorCode::kBadRequest);
  }
  const std::vector<SessionReport> reports = wait_for_reports(7);
  for (const SessionReport& report : reports) EXPECT_FALSE(report.ok);
  // The server is still fine.
  push("after", test::bytes_of("still serving"));
  EXPECT_TRUE(test::file_exists(remote("after")));
}

TEST_F(SyncServerTest, UnsafeNamesAreRefused) {
  const std::string outside = root_.path() + "/../deltasync-escape-test";
  for (const std::string name : {"../deltasync-escape-test", "/tmp/deltasync-abs", "a/b",
                                 ".hidden", "..", ".x.partial"}) {
    RawClient client(server_->port());
    client.hello();
    client.channel().send(FrameType::kFileInfo, encode_file_info(info_for(name, Bytes())));
    EXPECT_EQ(client.read_error(), ErrorCode::kBadRequest) << name;
  }
  wait_for_reports(6);
  EXPECT_FALSE(test::file_exists(outside));
  EXPECT_TRUE(test::list_directory(root_.path()).empty());
}

TEST_F(SyncServerTest, DestinationThatIsNotARegularFileIsRefused) {
  const std::string target_outside = local_.file("secret");
  test::write_file(target_outside, test::bytes_of("outside the root"));
  ASSERT_EQ(::symlink(target_outside.c_str(), remote("link").c_str()), 0);
  ASSERT_EQ(::mkdir(remote("dir").c_str(), 0755), 0);
  EXPECT_THROW(push("link", test::bytes_of("replacement")), RemoteError);
  EXPECT_THROW(push("dir", test::bytes_of("replacement")), RemoteError);
  EXPECT_TRUE(
      test::same_bytes(test::bytes_of("outside the root"), test::read_file(target_outside)));
}

class BadDeltaTest : public SyncServerTest {
 protected:
  void SetUp() override {
    SyncServerTest::SetUp();
    basis_ = test::random_bytes(600000, 2200);
    target_ = basis_;
    target_.insert(target_.begin() + 300000, 20000, 0x5a);
    test::write_file(remote("victim"), basis_);
  }

  // The destination still holds the basis and nothing else is in the root.
  void expect_untouched() {
    EXPECT_TRUE(test::same_bytes(basis_, test::read_file(remote("victim"))));
    EXPECT_EQ(test::list_directory(root_.path()), std::vector<std::string>{"victim"});
  }

  Bytes basis_;
  Bytes target_;
};

// Each test closes its client before waiting for the report: after an
// ERROR the server reads until the client hangs up.
TEST_F(BadDeltaTest, GarbageDataIsRejected) {
  {
    RawClient client(server_->port());
    client.start(info_for("victim", target_), target_);
    client.channel().send(FrameType::kData, test::bytes_of("this is not a delta"));
    EXPECT_EQ(client.read_error(), ErrorCode::kProtocol);
  }
  EXPECT_FALSE(wait_for_reports(1).at(0).ok);
  expect_untouched();
}

TEST_F(BadDeltaTest, AlteredLiteralFailsVerification) {
  {
    RawClient client(server_->port());
    Bytes delta = client.start(info_for("victim", target_, 64 * 1024), target_);
    // Inside the inserted literal run; the chunk digest no longer matches.
    delta[delta.size() / 2] ^= 0x01;
    client.send_data(delta, 0, delta.size());
    EXPECT_EQ(client.read_error(), ErrorCode::kIntegrity);
  }
  EXPECT_FALSE(wait_for_reports(1).at(0).ok);
  expect_untouched();
}

TEST_F(BadDeltaTest, DeltaForADifferentFileThanAnnounced) {
  {
    RawClient client(server_->port());
    FileInfo info = info_for("victim", target_);
    info.digest[0] ^= 1;
    const Bytes delta = client.start(info, target_);
    client.send_data(delta, 0, delta.size());
    EXPECT_EQ(client.read_error(), ErrorCode::kIntegrity);
  }
  wait_for_reports(1);
  expect_untouched();
}

TEST_F(BadDeltaTest, MoreDataThanAnnounced) {
  {
    RawClient client(server_->port());
    FileInfo info = info_for("victim", target_);
    info.size -= 1;
    const Bytes delta = client.start(info, target_);
    client.send_data(delta, 0, delta.size());
    EXPECT_EQ(client.read_error(), ErrorCode::kProtocol);
  }
  wait_for_reports(1);
  expect_untouched();
}

TEST_F(BadDeltaTest, UnexpectedFrameMidStream) {
  {
    RawClient client(server_->port());
    const Bytes delta = client.start(info_for("victim", target_), target_);
    client.send_data(delta, 0, delta.size() / 2);
    client.channel().send(FrameType::kOk, Bytes(16, 0));
    EXPECT_EQ(client.read_error(), ErrorCode::kProtocol);
  }
  wait_for_reports(1);
  expect_untouched();
}

// A broken connection keeps the verified part of the new version as
// resumable partial state next to the destination, never in its place.
// (The raw client always restarts its delta at 0, so each attempt also
// exercises a declined resume offer.) A complete push then replaces the
// destination and removes the partial state.
TEST_F(BadDeltaTest, SenderDisconnectingMidStreamLeavesTheDestination) {
  std::size_t sessions = 0;
  for (const double fraction : {0.0, 0.1, 0.5, 0.99}) {
    {
      RawClient client(server_->port());
      const Bytes delta = client.start(info_for("victim", target_, 64 * 1024), target_);
      client.send_data(delta, 0, static_cast<std::size_t>(fraction * delta.size()));
    }  // the client closes its socket here
    // The next client may only start once this session released the name.
    wait_for_reports(++sessions);
  }
  const std::vector<SessionReport> reports = wait_for_reports(4);
  for (const SessionReport& report : reports) {
    EXPECT_FALSE(report.ok);
    EXPECT_NE(report.error.find("closed"), std::string::npos) << report.error;
  }
  EXPECT_TRUE(test::same_bytes(basis_, test::read_file(remote("victim"))));
  for (const std::string& entry : test::list_directory(root_.path())) {
    EXPECT_TRUE(entry == "victim" || entry == ".victim.partial" || entry == ".victim.journal")
        << entry;
  }

  test::write_file(local_.file("victim"), target_);
  PushOptions options = push_options("victim");
  options.chunk_size = 64 * 1024;
  push_file(options);
  EXPECT_TRUE(test::same_bytes(target_, test::read_file(remote("victim"))));
  EXPECT_EQ(test::list_directory(root_.path()), std::vector<std::string>{"victim"});
}

TEST_F(BadDeltaTest, StoppingTheServerAbortsOpenSessions) {
  RawClient client(server_->port());
  const Bytes delta = client.start(info_for("victim", target_), target_);
  client.send_data(delta, 0, delta.size() / 3);
  stop_server();  // returns only after the session has ended
  ASSERT_EQ(reports_.size(), 1u);
  EXPECT_FALSE(reports_[0].ok);
  expect_untouched();
}

TEST_F(SyncServerTest, SecondWriterOfTheSameNameIsTurnedAway) {
  const Bytes target = test::random_bytes(10000, 2300);
  {
    RawClient holder(server_->port());
    holder.start(info_for("contended", target), target);  // holds the name now
    try {
      push("contended", target);
      FAIL() << "expected RemoteError";
    } catch (const RemoteError& error) {
      EXPECT_EQ(error.code(), ErrorCode::kBusy);
    }
  }
  wait_for_reports(2);  // the holder's session has ended and released the name
  push("contended", target);
  EXPECT_TRUE(test::same_bytes(target, test::read_file(remote("contended"))));
}

TEST_F(SyncServerTest, ConnectionLimitTurnsClientsAway) {
  stop_server();
  start_server(/*max_connections=*/1);
  const Bytes target = test::random_bytes(10000, 2301);
  {
    RawClient holder(server_->port());
    holder.start(info_for("one", target), target);
    try {
      push("two", target);
      FAIL() << "expected RemoteError";
    } catch (const RemoteError& error) {
      EXPECT_EQ(error.code(), ErrorCode::kBusy);
    }
  }
  wait_for_reports(2);
  push("two", target);
  EXPECT_TRUE(test::file_exists(remote("two")));
}

// With PROGRESS enabled at every opportunity, the receiver reports each
// verified chunk while it rebuilds the target, and the verified prefix of
// resumable partial state while it checks it before RESUME.
TEST_F(SyncServerTest, ReceiverReportsProgressWhileItWorks) {
  stop_server();
  start_server(/*max_connections=*/16, /*progress_interval_ms=*/0);
  const std::uint64_t chunk = 64 * 1024;
  const Bytes target = test::random_bytes(9 * kIoBufferSize + 100, 2302);
  const std::uint64_t chunks = (target.size() + chunk - 1) / chunk;
  {
    RawClient client(server_->port());
    const Bytes delta = client.start(info_for("watched", target, chunk), target);
    EXPECT_EQ(client.progress_frames(), 0);  // nothing to check yet
    // Stop one byte short: the connection breaks with chunks verified.
    client.send_data(delta, 0, delta.size() - 1);
    for (std::uint64_t i = 0; i + 1 < chunks; ++i) {
      ASSERT_EQ(client.channel().receive(), FrameType::kProgress);
      EXPECT_EQ(decode_progress(client.channel().payload()).position, (i + 1) * chunk);
    }
  }
  wait_for_reports(1);
  {
    RawClient client(server_->port());
    const Bytes delta = client.start(info_for("watched", target, chunk), target);
    EXPECT_EQ(client.resume().offset, (chunks - 1) * chunk);
    // The partial state's verified prefix was read in kIoBufferSize pieces.
    EXPECT_EQ(static_cast<std::uint64_t>(client.progress_frames()),
              ((chunks - 1) * chunk + kIoBufferSize - 1) / kIoBufferSize);
    // The raw client always restarts from 0 (a declined offer); the
    // receiver reports every chunk, then sends OK.
    client.send_data(delta, 0, delta.size());
    const int before = client.progress_frames();
    client.read_ok();
    EXPECT_EQ(static_cast<std::uint64_t>(client.progress_frames() - before), chunks);
  }
  EXPECT_TRUE(test::same_bytes(target, test::read_file(remote("watched"))));
  EXPECT_EQ(test::list_directory(root_.path()), std::vector<std::string>{"watched"});
}

// Files in the root that look like resume state of another name, or of
// this one, are user data; a push neither reads nor removes them.
TEST_F(SyncServerTest, UserFilesNamedLikeResumeStateSurviveAPush) {
  test::write_file(remote("notes"), test::bytes_of("old notes"));
  test::write_file(remote("notes.partial"), test::bytes_of("precious user data A"));
  test::write_file(remote("notes.journal"), test::bytes_of("precious user data B"));
  push("notes", test::bytes_of("new notes"));
  push("notes.partial", test::bytes_of("pushed under a lookalike name"));
  EXPECT_EQ(test::read_file(remote("notes")), test::bytes_of("new notes"));
  EXPECT_EQ(test::read_file(remote("notes.journal")), test::bytes_of("precious user data B"));
  EXPECT_EQ(test::read_file(remote("notes.partial")),
            test::bytes_of("pushed under a lookalike name"));
  EXPECT_EQ(test::list_directory(root_.path()),
            (std::vector<std::string>{"notes", "notes.journal", "notes.partial"}));
}

// A hidden partial file without a journal is not the receiver's to
// overwrite: the push fails and the file stays as it is.
TEST_F(SyncServerTest, UnclaimedPartialFileIsNotOverwritten) {
  test::write_file(remote(".notes.partial"), test::bytes_of("whose is this?"));
  try {
    push("notes", test::bytes_of("new notes"));
    FAIL() << "expected RemoteError";
  } catch (const RemoteError& error) {
    EXPECT_EQ(error.code(), ErrorCode::kIo);
  }
  EXPECT_EQ(test::read_file(remote(".notes.partial")), test::bytes_of("whose is this?"));
  EXPECT_EQ(test::list_directory(root_.path()), std::vector<std::string>{".notes.partial"});
}

// A push with retries keeps retrying ERROR busy while an older session
// still holds the name (here: a client that stopped talking), and gets
// through once that session ends, however many retries it was given.
TEST_F(SyncServerTest, RetriesOutlastASessionThatStillHoldsTheName) {
  const Bytes target = test::random_bytes(10000, 2303);
  test::write_file(local_.file("held"), target);
  auto holder = std::make_unique<RawClient>(server_->port());
  holder->start(info_for("held", target), target);  // holds the name now

  PushOptions options = push_options("held");
  options.retries = 1;
  options.retry_delay_ms = 1;
  std::string error;
  PushStats stats;
  std::thread pusher([&] {
    try {
      stats = push_file(options);
    } catch (const std::exception& failure) {
      error = failure.what();
    }
  });
  // Three busy refusals, more than `retries` allows, then the holder goes.
  const std::vector<SessionReport> refused = wait_for_reports(3);
  for (const SessionReport& report : refused) {
    EXPECT_NE(report.error.find("another connection"), std::string::npos) << report.error;
  }
  holder.reset();
  pusher.join();
  EXPECT_EQ(error, "");
  EXPECT_GE(stats.attempts, 4u);
  EXPECT_TRUE(test::same_bytes(target, test::read_file(remote("held"))));
}

// stop() may land while connections keep arriving and finished sessions
// are waiting to be joined; run() must join them all and return.
TEST(SyncServerStopTest, StopWhileClientsKeepConnecting) {
  test::TempDir root;
  for (int cycle = 0; cycle < 40; ++cycle) {
    SCOPED_TRACE("cycle " + std::to_string(cycle));
    std::mutex mutex;
    std::condition_variable reported;
    int sessions = 0;
    ServerOptions options;
    options.root = root.path();
    options.bind_address = "127.0.0.1";
    options.timeout_ms = kTimeoutMs;
    options.on_session = [&](const SessionReport&) {
      const std::lock_guard<std::mutex> guard(mutex);
      ++sessions;
      reported.notify_all();
    };
    SyncServer server(options);
    std::thread runner([&] { server.run(); });
    std::atomic<bool> done{false};
    std::vector<std::thread> clients;
    for (int i = 0; i < 3; ++i) {
      clients.emplace_back([&] {
        while (!done) {
          try {
            Socket socket = connect_to("127.0.0.1", server.port(), kTimeoutMs);
            socket.shutdown_write();  // the session ends at once
          } catch (const NetError&) {
          }
        }
      });
    }
    {
      std::unique_lock<std::mutex> lock(mutex);
      ASSERT_TRUE(reported.wait_for(lock, kReportWait, [&] { return sessions >= 2 + cycle % 5; }));
    }
    server.stop();
    runner.join();
    done = true;
    for (std::thread& client : clients) client.join();
  }
}

// The receiving end of one connection, scripted by the test.
class FakeReceiver {
 public:
  explicit FakeReceiver(std::function<void(Socket&)> script, int connections = 1)
      : listener_("127.0.0.1", 0) {
    thread_ = std::thread([this, script, connections] {
      for (int i = 0; i < connections; ++i) {
        Socket socket = listener_.accept(kTimeoutMs);
        try {
          script(socket);
        } catch (const std::exception& error) {
          errors_ += error.what();
        }
      }
    });
  }
  ~FakeReceiver() { thread_.join(); }

  std::uint16_t port() const { return listener_.port(); }
  // Only read after the destructor... or after the push returned and the
  // script is known to be done; tests join first via scope.
  const std::string& errors() const { return errors_; }

 private:
  Listener listener_;
  std::thread thread_;
  std::string errors_;
};

// Reads until the sender hangs up. A peer that closes with unread input
// makes the kernel reset the connection, which could destroy what it sent
// last before the other side reads it.
void drain(Socket& socket) {
  std::vector<std::uint8_t> discard(65536);
  while (socket.read_some(discard.data(), discard.size()) > 0) {
  }
}

void receiver_hello(FrameChannel& channel) {
  channel.expect(FrameType::kHello);
  channel.expect(FrameType::kFileInfo);
  Hello hello;
  hello.role = PeerRole::kReceiver;
  channel.send(FrameType::kHello, encode_hello(hello));
}

// Reads DATA frames until the delta is complete, rebuilding the target
// from `basis` (and a verified prefix when the sender resumed).
Bytes receive_delta(FrameChannel& channel, const Bytes& basis, const Bytes& prefix_bytes,
                    DeltaHeader* header_out) {
  test::RecordingSink records;
  DeltaDecoder decoder(records);
  while (!decoder.done()) {
    channel.expect(FrameType::kData);
    decoder.feed(channel.payload().data(), channel.payload().size());
  }
  *header_out = records.records.front().header;
  MemoryRandomAccess basis_source(basis);
  MemorySink suffix;
  Sha256 prefix;
  prefix.update(prefix_bytes.data(), prefix_bytes.size());
  Patcher patcher(basis_source, suffix, prefix);
  records.replay(patcher);
  Bytes result = prefix_bytes;
  result.insert(result.end(), suffix.data().begin(), suffix.data().end());
  return result;
}

class SenderTest : public ::testing::Test {
 protected:
  PushOptions options_for(std::uint16_t port, const Bytes& data) {
    test::write_file(local_.file("file"), data);
    PushOptions options;
    options.local_path = local_.file("file");
    options.host = "127.0.0.1";
    options.port = port;
    options.timeout_ms = kTimeoutMs;
    options.retry_delay_ms = 0;
    return options;
  }

  test::TempDir local_;
};

TEST_F(SenderTest, RejectsAPeerThatIsNotAReceiver) {
  const Bytes data = test::random_bytes(1000, 2400);
  {
    FakeReceiver fake([](Socket& socket) {
      const Bytes junk = test::bytes_of("HTTP/1.1 400 Bad Request\r\n\r\n");
      socket.write_all(junk.data(), junk.size());
      drain(socket);
    });
    EXPECT_THROW(push_file(options_for(fake.port(), data)), ProtocolError);
  }
  {
    FakeReceiver fake([](Socket& socket) {
      FrameChannel channel(socket);
      channel.send(FrameType::kHello, encode_hello(Hello()));  // role: sender
      drain(socket);
    });
    EXPECT_THROW(push_file(options_for(fake.port(), data)), ProtocolError);
  }
  {
    FakeReceiver fake([](Socket& socket) {
      FrameChannel channel(socket);
      Hello hello;
      hello.role = PeerRole::kReceiver;
      hello.version = kProtocolVersion + 1;
      channel.send(FrameType::kHello, encode_hello(hello));
      drain(socket);
    });
    EXPECT_THROW(push_file(options_for(fake.port(), data)), ProtocolError);
  }
}

TEST_F(SenderTest, RefusesAnOversizedSignature) {
  FakeReceiver fake([](Socket& socket) {
    FrameChannel channel(socket);
    receiver_hello(channel);
    SignatureHeader header;
    header.block_size = 1;
    header.basis_size = kMaxSignatureBlocks + 1;
    MemorySink bytes;
    SignatureWriter writer(bytes);
    writer.on_header(header);
    channel.send(FrameType::kSignature, bytes.data());
    channel.expect(FrameType::kError);  // the sender says why it stopped
  });
  EXPECT_THROW(push_file(options_for(fake.port(), test::random_bytes(1000, 2401))), ProtocolError);
}

TEST_F(SenderTest, ReportsTheReceiversError) {
  const Bytes data = test::random_bytes(3 << 20, 2402);
  FakeReceiver fake([](Socket& socket) {
    FrameChannel channel(socket);
    receiver_hello(channel);
    channel.send(FrameType::kResume, encode_resume(ResumePoint()));
    channel.expect(FrameType::kData);
    ErrorInfo error;
    error.code = ErrorCode::kIo;
    error.message = "disk full";
    channel.send(FrameType::kError, encode_error(error));
    socket.shutdown_write();
    drain(socket);
  });
  try {
    push_file(options_for(fake.port(), data));
    FAIL() << "expected RemoteError";
  } catch (const RemoteError& error) {
    EXPECT_EQ(error.code(), ErrorCode::kIo);
    EXPECT_NE(std::string(error.what()).find("disk full"), std::string::npos);
  }
}

TEST_F(SenderTest, ReceiverDisconnectingMidStreamIsANetError) {
  const Bytes data = test::random_bytes(4 << 20, 2403);
  FakeReceiver fake([](Socket& socket) {
    FrameChannel channel(socket);
    receiver_hello(channel);
    channel.send(FrameType::kResume, encode_resume(ResumePoint()));
    channel.expect(FrameType::kData);
  });  // closes after one DATA frame
  EXPECT_THROW(push_file(options_for(fake.port(), data)), NetError);
}

TEST_F(SenderTest, ResumesOnlyFromAProvenPrefix) {
  const Bytes data = test::random_bytes(5 * 65536 + 100, 2404);
  const std::uint64_t chunk = 65536;
  for (const bool honest : {true, false}) {
    SCOPED_TRACE(honest ? "matching prefix" : "stale prefix");
    DeltaHeader header;
    Bytes rebuilt;
    {
      FakeReceiver fake([&](Socket& socket) {
        FrameChannel channel(socket);
        receiver_hello(channel);
        // Claim 2 verified chunks; the dishonest receiver's bytes differ.
        Bytes prefix_bytes(data.begin(), data.begin() + 2 * chunk);
        if (!honest) prefix_bytes[100] ^= 1;
        Sha256 prefix;
        prefix.update(prefix_bytes.data(), prefix_bytes.size());
        ResumePoint resume;
        resume.offset = 2 * chunk;
        resume.prefix = prefix.midstate();
        channel.send(FrameType::kResume, encode_resume(resume));
        DeltaHeader seen;
        const Bytes kept = honest ? Bytes(data.begin(), data.begin() + 2 * chunk) : Bytes();
        rebuilt = receive_delta(channel, Bytes(), kept, &seen);
        header = seen;
        channel.send(FrameType::kOk, encode_ok(OkInfo()));
      });
      PushOptions options = options_for(fake.port(), data);
      options.chunk_size = chunk;
      const PushStats stats = push_file(options);
      EXPECT_EQ(stats.resumed_from, honest ? 2 * chunk : 0u);
      EXPECT_EQ(stats.literal_bytes, data.size() - stats.resumed_from);
    }
    EXPECT_EQ(header.start_offset, honest ? 2 * chunk : 0u);
    EXPECT_TRUE(test::same_bytes(data, rebuilt));
  }
}

TEST_F(SenderTest, RetriesAfterAConnectionFailure) {
  test::TempDir root;
  const Bytes data = test::random_bytes(100000, 2405);
  int connection = 0;
  FakeReceiver fake(
      [&](Socket& socket) {
        // The first connection drops right away; the second is a real
        // receiver.
        if (connection++ == 0) return;
        EXPECT_TRUE(receive_session(socket, root.path(), nullptr).ok);
      },
      2);
  PushOptions options = options_for(fake.port(), data);
  options.retries = 2;
  options.remote_name = "retried";
  const PushStats stats = push_file(options);
  EXPECT_EQ(stats.attempts, 2u);
  EXPECT_TRUE(test::same_bytes(data, test::read_file(root.file("retried"))));
  EXPECT_GT(stats.bytes_sent, data.size());
}

TEST_F(SenderTest, GivesUpAfterTheLastRetry) {
  const Bytes data = test::random_bytes(1000, 2406);
  FakeReceiver fake([](Socket&) {}, 3);  // every connection drops at once
  PushOptions options = options_for(fake.port(), data);
  options.retries = 2;
  EXPECT_THROW(push_file(options), NetError);
}

// The receiver may send PROGRESS before RESUME, between DATA frames and
// before OK; the sender skips all of them.
TEST_F(SenderTest, SkipsProgressFrames) {
  const Bytes data = test::random_bytes(5 * 65536 + 100, 2407);
  Bytes rebuilt;
  {
    FakeReceiver fake([&](Socket& socket) {
      FrameChannel channel(socket);
      receiver_hello(channel);
      const auto progress = [&](std::uint64_t position) {
        ProgressInfo info;
        info.position = position;
        channel.send(FrameType::kProgress, encode_progress(info));
      };
      progress(1);
      progress(2);
      channel.send(FrameType::kResume, encode_resume(ResumePoint()));
      test::RecordingSink records;
      DeltaDecoder decoder(records);
      int frames = 0;
      while (!decoder.done()) {
        channel.expect(FrameType::kData);
        decoder.feed(channel.payload().data(), channel.payload().size());
        if (++frames == 1) progress(3);  // read by the sender mid-stream
      }
      MemoryRandomAccess no_basis(nullptr, 0);
      MemorySink out;
      Patcher patcher(no_basis, out);
      records.replay(patcher);
      rebuilt = out.take();
      for (std::uint64_t i = 0; i < 5; ++i) progress(4 + i);
      channel.send(FrameType::kOk, encode_ok(OkInfo()));
    });
    PushOptions options = options_for(fake.port(), data);
    options.chunk_size = 65536;
    const PushStats stats = push_file(options);
    EXPECT_EQ(stats.literal_bytes, data.size());
  }
  EXPECT_TRUE(test::same_bytes(data, rebuilt));
}

// Every CHUNK record ends a DATA frame, so a receiver hears from the sender
// once per chunk even when the chunks are unchanged and encode to a few
// dozen bytes each.
TEST_F(SenderTest, EndsADataFrameAtEveryChunk) {
  const std::uint64_t chunk = 65536;
  const Bytes data = test::random_bytes(9 * chunk + 100, 2408);
  const std::uint64_t chunks = 10;
  int frames = 0;
  Bytes rebuilt;
  {
    FakeReceiver fake([&](Socket& socket) {
      FrameChannel channel(socket);
      receiver_hello(channel);
      MemorySink signature;
      SignatureWriter writer(signature);
      SignatureHeader header;
      header.block_size = 4096;
      header.basis_size = data.size();
      MemorySource basis(data);
      generate_signature(basis, header, writer);
      channel.send(FrameType::kSignature, signature.data());
      channel.send(FrameType::kResume, encode_resume(ResumePoint()));
      test::RecordingSink records;
      DeltaDecoder decoder(records);
      while (!decoder.done()) {
        channel.expect(FrameType::kData);
        ++frames;
        decoder.feed(channel.payload().data(), channel.payload().size());
      }
      MemoryRandomAccess basis_source(data);
      MemorySink out;
      Patcher patcher(basis_source, out);
      records.replay(patcher);
      rebuilt = out.take();
      channel.send(FrameType::kOk, encode_ok(OkInfo()));
    });
    PushOptions options = options_for(fake.port(), data);
    options.chunk_size = chunk;
    const PushStats stats = push_file(options);
    EXPECT_EQ(stats.matched_bytes, data.size());
  }
  EXPECT_TRUE(test::same_bytes(data, rebuilt));
  // One frame per chunk, plus the last one that carries END.
  EXPECT_EQ(static_cast<std::uint64_t>(frames), chunks + 1);
}

// With more chunks than the sender keeps hash states for, an offer is
// checked from the nearest saved state: honest offers anywhere are taken,
// stale ones are not.
TEST_F(SenderTest, ChecksResumeOffersAtAnyChunk) {
  const std::uint64_t chunk = 64;
  const Bytes data = test::random_bytes(10000 * chunk + 13, 2409);
  struct Offer {
    std::uint64_t offset;
    bool honest;
  };
  for (const Offer offer : {Offer{chunk, true}, Offer{3 * chunk, true}, Offer{4 * chunk, true},
                            Offer{5000 * chunk + chunk, true}, Offer{10000 * chunk, true},
                            Offer{4 * chunk, false}, Offer{9999 * chunk, false}}) {
    SCOPED_TRACE("offset " + std::to_string(offer.offset) + (offer.honest ? "" : " stale"));
    Bytes rebuilt;
    {
      FakeReceiver fake([&](Socket& socket) {
        FrameChannel channel(socket);
        receiver_hello(channel);
        Bytes prefix_bytes(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(offer.offset));
        if (!offer.honest) prefix_bytes[prefix_bytes.size() - 1] ^= 1;
        Sha256 prefix;
        prefix.update(prefix_bytes.data(), prefix_bytes.size());
        ResumePoint resume;
        resume.offset = offer.offset;
        resume.prefix = prefix.midstate();
        channel.send(FrameType::kResume, encode_resume(resume));
        DeltaHeader seen;
        rebuilt = receive_delta(channel, Bytes(), offer.honest ? prefix_bytes : Bytes(), &seen);
        EXPECT_EQ(seen.start_offset, offer.honest ? offer.offset : 0u);
        channel.send(FrameType::kOk, encode_ok(OkInfo()));
      });
      PushOptions options = options_for(fake.port(), data);
      options.chunk_size = chunk;
      const PushStats stats = push_file(options);
      EXPECT_EQ(stats.resumed_from, offer.honest ? offer.offset : 0u);
    }
    EXPECT_TRUE(test::same_bytes(data, rebuilt));
  }
}

TEST(StatsJsonTest, HasEveryKey) {
  PushStats stats;
  stats.mode = TransferMode::kWholeFile;
  stats.file_size = 10;
  stats.bytes_sent = 20;
  stats.attempts = 3;
  const std::string json = stats_json(stats);
  for (const char* key : {"\"mode\": \"whole-file\"", "\"file_size\": 10", "\"bytes_sent\": 20",
                          "\"bytes_received\": 0", "\"signature_bytes\": 0", "\"delta_bytes\": 0",
                          "\"literal_bytes\": 0", "\"matched_bytes\": 0", "\"resumed_from\": 0",
                          "\"attempts\": 3", "\"elapsed_ms\": 0"}) {
    EXPECT_NE(json.find(key), std::string::npos) << key << " in " << json;
  }
  EXPECT_EQ(json.front(), '{');
  EXPECT_EQ(json.substr(json.size() - 2), "}\n");
}

}  // namespace
}  // namespace deltasync
