#include "deltasync/protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "deltasync/bytes.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;

struct Pair {
  Socket client;
  Socket server;
};

Pair loopback_pair(int timeout_ms = 10000) {
  Listener listener("127.0.0.1", 0);
  Pair pair;
  pair.client = connect_to("127.0.0.1", listener.port(), timeout_ms);
  pair.server = listener.accept(timeout_ms);
  return pair;
}

FileInfo sample_info() {
  FileInfo info;
  info.name = "data.bin";
  info.size = 0x0102030405060708ull;
  info.digest = Sha256::hash("x", 1);
  info.chunk_size = 1 << 20;
  info.block_size = 4096;
  info.mode = TransferMode::kWholeFile;
  return info;
}

TEST(ProtocolTest, HelloRoundTrip) {
  Hello hello;
  hello.role = PeerRole::kReceiver;
  const Payload payload = encode_hello(hello);
  EXPECT_EQ(payload, (Payload{'D', 'S', 'Y', 'N', kProtocolVersion, 2}));
  const Hello decoded = decode_hello(payload);
  EXPECT_EQ(decoded.version, kProtocolVersion);
  EXPECT_EQ(decoded.role, PeerRole::kReceiver);
}

TEST(ProtocolTest, HelloRejectsStrangers) {
  EXPECT_THROW(decode_hello(test::bytes_of("GET / HTTP/1.1")), ProtocolError);
  EXPECT_THROW(decode_hello(Payload{'D', 'S', 'Y', 'N', 1}), ProtocolError);
  EXPECT_THROW(decode_hello(Payload{'D', 'S', 'Y', 'N', 1, 2, 3}), ProtocolError);
  EXPECT_THROW(decode_hello(Payload{'D', 'S', 'Y', 'N', 1, 9}), ProtocolError);
  // A newer version still decodes; the peer decides whether it can talk.
  EXPECT_EQ(decode_hello(Payload{'D', 'S', 'Y', 'N', 7, 1}).version, 7);
}

TEST(ProtocolTest, FileInfoRoundTripUsesNetworkByteOrder) {
  const FileInfo info = sample_info();
  const Payload payload = encode_file_info(info);
  EXPECT_EQ(payload[0], 1);                         // mode
  EXPECT_EQ(load_be64(payload.data() + 1), 1u << 20);  // chunk size
  EXPECT_EQ(load_be32(payload.data() + 9), 4096u);  // block size
  EXPECT_EQ(load_be64(payload.data() + 13), info.size);
  const FileInfo decoded = decode_file_info(payload);
  EXPECT_EQ(decoded.name, info.name);
  EXPECT_EQ(decoded.size, info.size);
  EXPECT_EQ(decoded.digest, info.digest);
  EXPECT_EQ(decoded.chunk_size, info.chunk_size);
  EXPECT_EQ(decoded.block_size, info.block_size);
  EXPECT_EQ(decoded.mode, info.mode);
}

TEST(ProtocolTest, FileInfoRejectsMalformedPayloads) {
  const Payload good = encode_file_info(sample_info());
  for (std::size_t length = 0; length < good.size(); ++length) {
    EXPECT_THROW(decode_file_info(Payload(good.begin(), good.begin() + length)), ProtocolError)
        << length;
  }
  Payload longer = good;
  longer.push_back('x');
  EXPECT_THROW(decode_file_info(longer), ProtocolError);
  Payload bad_mode = good;
  bad_mode[0] = 2;
  EXPECT_THROW(decode_file_info(bad_mode), ProtocolError);
  FileInfo long_name = sample_info();
  long_name.name.assign(kMaxNameLength + 1, 'a');
  EXPECT_THROW(encode_file_info(long_name), ProtocolError);
}

TEST(ProtocolTest, ResumeOkAndErrorRoundTrip) {
  ResumePoint resume;
  resume.offset = 3u << 20;
  for (std::uint32_t i = 0; i < 8; ++i) resume.prefix.words[i] = 0x01020304u * (i + 1);
  const ResumePoint decoded = decode_resume(encode_resume(resume));
  EXPECT_EQ(decoded.offset, resume.offset);
  EXPECT_EQ(decoded.prefix.words, resume.prefix.words);
  EXPECT_EQ(decoded.prefix.length, resume.offset);
  EXPECT_THROW(decode_resume(Payload(39)), ProtocolError);

  OkInfo ok;
  ok.receiver_bytes_received = 123456789;
  ok.receiver_bytes_sent = 42;
  const OkInfo ok_decoded = decode_ok(encode_ok(ok));
  EXPECT_EQ(ok_decoded.receiver_bytes_received, ok.receiver_bytes_received);
  EXPECT_EQ(ok_decoded.receiver_bytes_sent, ok.receiver_bytes_sent);
  EXPECT_THROW(decode_ok(Payload(15)), ProtocolError);

  ProgressInfo progress;
  progress.position = (5ull << 32) + 7;
  const Payload progress_bytes = encode_progress(progress);
  EXPECT_EQ(progress_bytes, (Payload{0, 0, 0, 5, 0, 0, 0, 7}));
  EXPECT_EQ(decode_progress(progress_bytes).position, progress.position);
  EXPECT_THROW(decode_progress(Payload(9)), ProtocolError);

  ErrorInfo error;
  error.code = ErrorCode::kBusy;
  error.message = "try later";
  const ErrorInfo error_decoded = decode_error(encode_error(error));
  EXPECT_EQ(error_decoded.code, ErrorCode::kBusy);
  EXPECT_EQ(error_decoded.message, "try later");
  EXPECT_THROW(decode_error(Payload()), ProtocolError);
}

TEST(ProtocolTest, ErrorMessagesAreBoundedAndSanitized) {
  ErrorInfo error;
  error.code = ErrorCode::kIo;
  error.message = std::string(5000, 'x');
  EXPECT_EQ(encode_error(error).size(), 1 + kMaxErrorMessage);
  Payload hostile = {static_cast<std::uint8_t>(ErrorCode::kIo), 'a', 0x1b, '[', '2', 'J', '\n'};
  EXPECT_EQ(decode_error(hostile).message, "a?[2J?");
  Payload too_long(2 + kMaxErrorMessage, 'y');
  EXPECT_THROW(decode_error(too_long), ProtocolError);

  const RemoteError remote(ErrorCode::kIntegrity, "chunk 3");
  EXPECT_EQ(remote.code(), ErrorCode::kIntegrity);
  EXPECT_NE(std::string(remote.what()).find("chunk 3"), std::string::npos);
}

TEST(ProtocolTest, RemoteNamesMustBePlainFileNames) {
  // Resumable output lives in hidden files, so names ending in ".partial"
  // or ".journal" cannot collide with it and are fine.
  const std::vector<std::string> good = {"a",         "data.bin",      "with space",
                                         "x..y",      "caf\xc3\xa9",   "partial",
                                         "x.partial", "x.journal",     "x.journal.tmp",
                                         std::string(kMaxNameLength, 'n')};
  for (const std::string& name : good) {
    EXPECT_NO_THROW(validate_remote_name(name)) << name;
  }
  for (const std::string& name : std::vector<std::string>{
           std::string(), std::string("."), std::string(".."), std::string("../etc/passwd"),
        std::string("a/b"), std::string("/abs"), std::string("dir/"), std::string(".hidden"),
        std::string(".a.tmp-123"), std::string("line\nbreak"), std::string("nul\0x", 5),
        std::string("del\x7f"), std::string(kMaxNameLength + 1, 'n')}) {
    EXPECT_THROW(validate_remote_name(name), ProtocolError) << name;
  }
}

TEST(ProtocolTest, ChunkSizesMustBeMultiplesOf64) {
  for (const std::uint64_t size : {64ull, 128ull, 4096ull, 1ull << 20, 1ull << 30}) {
    EXPECT_NO_THROW(validate_chunk_size(size)) << size;
  }
  for (const std::uint64_t size : {0ull, 1ull, 63ull, 100ull, 1000ull, (1ull << 30) + 64}) {
    EXPECT_THROW(validate_chunk_size(size), ProtocolError) << size;
  }
}

TEST(FrameChannelTest, FramesArriveWhole) {
  Pair pair = loopback_pair();
  FrameChannel sender(pair.client);
  FrameChannel receiver(pair.server);
  const Bytes big = test::random_bytes(kMaxFramePayload, 1101);
  sender.send(FrameType::kHello, encode_hello(Hello()));
  sender.send(FrameType::kOk, nullptr, 0);
  std::thread writer([&] { sender.send(FrameType::kData, big); });
  EXPECT_EQ(receiver.receive(), FrameType::kHello);
  EXPECT_EQ(receiver.payload(), encode_hello(Hello()));
  EXPECT_EQ(receiver.receive(), FrameType::kOk);
  EXPECT_TRUE(receiver.payload().empty());
  EXPECT_EQ(receiver.receive(), FrameType::kData);
  writer.join();
  EXPECT_TRUE(test::same_bytes(big, receiver.payload()));
  EXPECT_EQ(pair.client.bytes_sent(), 3 * kFrameHeaderLength + 6 + big.size());
}

TEST(FrameChannelTest, RejectsUnknownTypesAndOversizedFrames) {
  {
    Pair pair = loopback_pair();
    const std::uint8_t unknown[] = {0x42, 0, 0, 0, 0};
    pair.client.write_all(unknown, sizeof unknown);
    FrameChannel channel(pair.server);
    EXPECT_THROW(channel.receive(), ProtocolError);
  }
  {
    Pair pair = loopback_pair();
    std::uint8_t huge[kFrameHeaderLength] = {static_cast<std::uint8_t>(FrameType::kData)};
    store_be32(huge + 1, kMaxFramePayload + 1);
    pair.client.write_all(huge, sizeof huge);
    FrameChannel channel(pair.server);
    EXPECT_THROW(channel.receive(), ProtocolError);
    // Nothing beyond the header was read or buffered.
    EXPECT_EQ(pair.server.bytes_received(), kFrameHeaderLength);
  }
}

TEST(FrameChannelTest, TruncatedFrameIsANetError) {
  Pair pair = loopback_pair();
  std::uint8_t partial[kFrameHeaderLength + 3] = {static_cast<std::uint8_t>(FrameType::kData)};
  store_be32(partial + 1, 10);
  pair.client.write_all(partial, sizeof partial);
  pair.client.shutdown_write();
  FrameChannel channel(pair.server);
  EXPECT_THROW(channel.receive(), NetError);
}

// The receiver consumes DATA payloads in pieces as they arrive; the bytes
// of a frame cut short are still handed out before the NetError.
TEST(FrameChannelTest, PayloadCanBeReadInPieces) {
  Pair pair = loopback_pair();
  FrameChannel sender(pair.client);
  FrameChannel receiver(pair.server);
  const Bytes data = test::random_bytes(300000, 1102);
  sender.send(FrameType::kData, data);
  sender.send(FrameType::kOk, test::bytes_of("ok"));
  std::uint8_t header[kFrameHeaderLength] = {static_cast<std::uint8_t>(FrameType::kData)};
  store_be32(header + 1, 1000);
  pair.client.write_all(header, sizeof header);
  pair.client.write_all(data.data(), 600);
  pair.client.shutdown_write();

  EXPECT_EQ(receiver.receive_header(), FrameType::kData);
  EXPECT_EQ(receiver.payload_remaining(), data.size());
  Bytes got;
  std::uint8_t piece[777];
  while (receiver.payload_remaining() > 0) {
    const std::size_t count = receiver.read_payload_some(piece, sizeof piece);
    got.insert(got.end(), piece, piece + count);
  }
  EXPECT_TRUE(test::same_bytes(data, got));
  EXPECT_EQ(receiver.read_payload_some(piece, sizeof piece), 0u);

  EXPECT_EQ(receiver.receive_header(), FrameType::kOk);
  receiver.read_payload();
  EXPECT_EQ(receiver.payload(), test::bytes_of("ok"));

  EXPECT_EQ(receiver.receive_header(), FrameType::kData);
  std::size_t delivered = 0;
  EXPECT_THROW(
      for (;;) delivered += receiver.read_payload_some(piece, sizeof piece), NetError);
  EXPECT_EQ(delivered, 600u);
}

TEST(FrameChannelTest, ExpectReportsErrorFramesAndSurprises) {
  Pair pair = loopback_pair();
  FrameChannel sender(pair.client);
  FrameChannel receiver(pair.server);
  ErrorInfo error;
  error.code = ErrorCode::kBadRequest;
  error.message = "no";
  sender.send(FrameType::kError, encode_error(error));
  sender.send(FrameType::kData, Payload{1});
  try {
    receiver.expect(FrameType::kOk);
    FAIL() << "expected RemoteError";
  } catch (const RemoteError& remote) {
    EXPECT_EQ(remote.code(), ErrorCode::kBadRequest);
  }
  EXPECT_THROW(receiver.expect(FrameType::kOk), ProtocolError);
}

TEST(FrameChannelTest, ExpectCanSkipProgressFrames) {
  Pair pair = loopback_pair();
  FrameChannel sender(pair.client);
  FrameChannel receiver(pair.server);
  sender.send(FrameType::kProgress, encode_progress(ProgressInfo()));
  sender.send(FrameType::kProgress, encode_progress(ProgressInfo()));
  sender.send(FrameType::kOk, encode_ok(OkInfo()));
  sender.send(FrameType::kProgress, encode_progress(ProgressInfo()));
  receiver.expect_skipping_progress(FrameType::kOk);
  EXPECT_THROW(receiver.expect(FrameType::kOk), ProtocolError);  // plain expect does not skip
}

TEST(FrameSinkTest, PacksStreamsIntoFrames) {
  Pair pair = loopback_pair();
  FrameChannel sender(pair.client);
  FrameChannel receiver(pair.server);
  const Bytes data = test::random_bytes(2 * kStreamFramePayload + 1234, 1102);
  std::thread writer([&] {
    FrameSink sink(sender, FrameType::kSignature);
    // Odd write sizes must not change the framing.
    for (std::size_t offset = 0; offset < data.size(); offset += 1000) {
      sink.write(data.data() + offset, std::min<std::size_t>(1000, data.size() - offset));
    }
    sink.flush();
    sink.flush();  // nothing pending: no empty frame
    EXPECT_EQ(sink.payload_bytes(), data.size());
  });
  Bytes received;
  std::vector<std::size_t> sizes;
  while (received.size() < data.size()) {
    ASSERT_EQ(receiver.receive(), FrameType::kSignature);
    sizes.push_back(receiver.payload().size());
    received.insert(received.end(), receiver.payload().begin(), receiver.payload().end());
  }
  writer.join();
  EXPECT_TRUE(test::same_bytes(data, received));
  EXPECT_EQ(sizes, (std::vector<std::size_t>{kStreamFramePayload, kStreamFramePayload, 1234}));
  EXPECT_FALSE(pair.server.readable(0));
}

TEST(FrameSinkTest, StopsWhenThePeerReportsAnError) {
  Pair pair = loopback_pair();
  FrameChannel sender(pair.client);
  FrameChannel receiver(pair.server);
  ErrorInfo error;
  error.code = ErrorCode::kIntegrity;
  error.message = "chunk mismatch";
  receiver.send(FrameType::kError, encode_error(error));
  ASSERT_TRUE(pair.client.readable(10000));
  FrameSink sink(sender, FrameType::kData);
  const Bytes data(kStreamFramePayload, 1);
  try {
    sink.write(data.data(), data.size());
    FAIL() << "expected RemoteError";
  } catch (const RemoteError& remote) {
    EXPECT_EQ(remote.code(), ErrorCode::kIntegrity);
  }
  EXPECT_EQ(pair.client.bytes_sent(), 0u);
}

// The sender's DATA sink skips PROGRESS frames waiting before a frame (and
// still notices an ERROR behind them); other sinks treat them as surprises.
TEST(FrameSinkTest, SkipsProgressFramesOnlyWhenAsked) {
  Pair pair = loopback_pair();
  FrameChannel sender(pair.client);
  FrameChannel receiver(pair.server);
  // Frames the receiver sends in one write, so they arrive together.
  const auto send_together = [&](const std::vector<std::pair<FrameType, Payload>>& frames) {
    Bytes bytes;
    for (const auto& frame : frames) {
      std::uint8_t header[kFrameHeaderLength] = {static_cast<std::uint8_t>(frame.first)};
      store_be32(header + 1, static_cast<std::uint32_t>(frame.second.size()));
      bytes.insert(bytes.end(), header, header + kFrameHeaderLength);
      bytes.insert(bytes.end(), frame.second.begin(), frame.second.end());
    }
    pair.server.write_all(bytes.data(), bytes.size());
    ASSERT_TRUE(pair.client.readable(10000));
  };
  const Payload progress = encode_progress(ProgressInfo());
  send_together({{FrameType::kProgress, progress}, {FrameType::kProgress, progress}});
  {
    FrameSink sink(sender, FrameType::kData);
    sink.set_skip_progress(true);
    sink.write(test::bytes_of("abc").data(), 3);
    sink.flush();
  }
  ASSERT_EQ(receiver.receive(), FrameType::kData);
  EXPECT_EQ(receiver.payload(), test::bytes_of("abc"));

  ErrorInfo error;
  error.code = ErrorCode::kIo;
  send_together({{FrameType::kProgress, progress}, {FrameType::kError, encode_error(error)}});
  {
    FrameSink sink(sender, FrameType::kData);
    sink.set_skip_progress(true);
    sink.write(test::bytes_of("def").data(), 3);
    EXPECT_THROW(sink.flush(), RemoteError);
  }

  send_together({{FrameType::kProgress, progress}});
  FrameSink strict(sender, FrameType::kSignature);
  strict.write(test::bytes_of("ghi").data(), 3);
  EXPECT_THROW(strict.flush(), ProtocolError);
}

}  // namespace
}  // namespace deltasync
