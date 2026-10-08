#include "deltasync/delta_format.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "deltasync/error.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;
using test::Record;
using test::RecordingSink;

Sha256Digest digest_of(const std::string& text) { return Sha256::hash(text.data(), text.size()); }

// Writes records byte by byte without any validation, so malformed deltas
// can be expressed.
class RawDelta {
 public:
  RawDelta& header(std::uint64_t chunk_size, std::uint64_t basis_size,
                   std::uint64_t start_offset = 0) {
    bytes_ = {'D', 'S', 'D', 'L', kDeltaVersion};
    return varint(0).varint(chunk_size).varint(basis_size).varint(start_offset);
  }
  RawDelta& copy(std::uint64_t offset, std::uint64_t length) {
    return byte(0x01).varint(offset).varint(length);
  }
  RawDelta& literal(const Bytes& data) {
    byte(0x02).varint(data.size());
    bytes_.insert(bytes_.end(), data.begin(), data.end());
    return *this;
  }
  RawDelta& chunk(const Sha256Digest& digest) {
    byte(0x03);
    bytes_.insert(bytes_.end(), digest.begin(), digest.end());
    return *this;
  }
  RawDelta& end(std::uint64_t total, const Sha256Digest& digest) {
    byte(0x04).varint(total);
    bytes_.insert(bytes_.end(), digest.begin(), digest.end());
    return *this;
  }
  RawDelta& byte(std::uint8_t value) {
    bytes_.push_back(value);
    return *this;
  }
  RawDelta& varint(std::uint64_t value) {
    std::uint8_t buffer[kMaxVarintLength];
    bytes_.insert(bytes_.end(), buffer, buffer + encode_varint(value, buffer));
    return *this;
  }

  const Bytes& bytes() const { return bytes_; }

 private:
  Bytes bytes_;
};

// Decodes `bytes` and returns the FormatError message ("" on success).
std::string decode_error(const Bytes& bytes, RecordingSink* sink = nullptr) {
  RecordingSink local;
  DeltaDecoder decoder(sink != nullptr ? *sink : local);
  try {
    decoder.feed(bytes.data(), bytes.size());
    decoder.finish();
  } catch (const FormatError& error) {
    return error.what();
  }
  return "";
}

// A delta with every record type: chunk size 128, output 261 bytes.
RecordingSink sample_records() {
  RecordingSink sink;
  DeltaHeader header;
  header.chunk_size = 128;
  header.basis_size = 1000;
  sink.on_header(header);
  sink.on_copy(0, 60);
  const Bytes literal = test::random_bytes(68, 51);
  sink.on_literal(literal.data(), literal.size());
  sink.on_chunk(digest_of("first chunk"));
  sink.on_copy(500, 128);
  sink.on_chunk(digest_of("second chunk"));
  const Bytes tail = test::random_bytes(5, 52);
  sink.on_literal(tail.data(), tail.size());
  sink.on_chunk(digest_of("final short chunk"));
  sink.on_end(261, digest_of("whole file"));
  return sink;
}

Bytes serialize(const RecordingSink& records) {
  MemorySink out;
  DeltaWriter writer(out);
  records.replay(writer);
  return out.take();
}

TEST(DeltaFormatTest, WriterOutputDecodesToTheSameRecords) {
  const RecordingSink expected = sample_records();
  const Bytes bytes = serialize(expected);
  RecordingSink decoded;
  ASSERT_EQ(decode_error(bytes, &decoded), "");
  EXPECT_EQ(decoded.records, expected.records);
}

TEST(DeltaFormatTest, ExactByteLayout) {
  MemorySink out;
  DeltaWriter writer(out);
  DeltaHeader header;
  header.chunk_size = 128;
  header.basis_size = 300;
  writer.on_header(header);
  writer.on_copy(256, 44);
  const std::uint8_t literal[] = {0xaa, 0xbb};
  writer.on_literal(literal, 2);
  const Bytes expected = {'D',  'S',  'D',  'L',   // magic
                          1,                       // version
                          0,                       // flags
                          0x80, 0x01,              // chunk size 128
                          0xac, 0x02,              // basis size 300
                          0,                       // start offset
                          0x01, 0x80, 0x02, 44,    // COPY(256, 44)
                          0x02, 2,    0xaa, 0xbb}; // LITERAL(2 bytes)
  EXPECT_TRUE(test::same_bytes(expected, out.data()));
  EXPECT_EQ(writer.bytes_written(), expected.size());
}

TEST(DeltaFormatTest, DecoderAcceptsAnySplitOfTheInput) {
  const RecordingSink expected = sample_records();
  const Bytes bytes = serialize(expected);
  test::Rng rng(53);
  for (int round = 0; round < 50; ++round) {
    RecordingSink decoded;
    DeltaDecoder decoder(decoded);
    const std::size_t max_piece = round == 0 ? 1 : rng.between(1, 64);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const std::size_t piece = std::min<std::size_t>(rng.between(0, max_piece), bytes.size() - offset);
      decoder.feed(bytes.data() + offset, piece);
      offset += piece;
    }
    decoder.finish();
    ASSERT_EQ(decoded.records, expected.records) << "round " << round;
  }
}

TEST(DeltaFormatTest, LiteralBytesArePassedOnAsTheyArrive) {
  const Bytes data = test::random_bytes(1000, 54);
  const Bytes bytes = RawDelta().header(4096, 0).literal(data).bytes();
  const std::size_t literal_start = bytes.size() - data.size();

  RecordingSink sink;
  sink.merge_literals = false;
  DeltaDecoder decoder(sink);
  decoder.feed(bytes.data(), literal_start + 10);
  ASSERT_EQ(sink.records.size(), 2u);
  EXPECT_EQ(sink.records[1].data, Bytes(data.begin(), data.begin() + 10));
  decoder.feed(bytes.data() + literal_start + 10, 500);
  ASSERT_EQ(sink.records.size(), 3u);
  EXPECT_EQ(sink.records[2].data.size(), 500u);
  EXPECT_EQ(decoder.position(), 1000u);
}

TEST(DeltaFormatTest, ValidateHeader) {
  DeltaHeader header;
  EXPECT_NO_THROW(validate(header));
  header.chunk_size = 0;
  EXPECT_THROW(validate(header), FormatError);
  header.chunk_size = kMaxChunkSize + 1;
  EXPECT_THROW(validate(header), FormatError);
  header.chunk_size = kMaxChunkSize;
  EXPECT_NO_THROW(validate(header));
  header.chunk_size = 192;
  header.start_offset = 320;
  EXPECT_THROW(validate(header), FormatError);
  header.start_offset = 384;
  EXPECT_NO_THROW(validate(header));
}

// Every chunk boundary is a resume point, and resuming restores the SHA-256
// state of the prefix from a midstate, which exists only at multiples of the
// SHA-256 block length.
TEST(DeltaFormatTest, ChunkSizesAreSha256BlockMultiples) {
  for (const std::uint64_t size : std::vector<std::uint64_t>{1, 7, 63, 65, 100, 1000, 1000004}) {
    EXPECT_FALSE(valid_chunk_size(size)) << size;
    DeltaHeader header;
    header.chunk_size = size;
    EXPECT_THROW(validate(header), FormatError) << size;
  }
  for (const std::uint64_t size : std::vector<std::uint64_t>{64, 128, 4096, 1 << 20, kMaxChunkSize}) {
    EXPECT_TRUE(valid_chunk_size(size)) << size;
    DeltaHeader header;
    header.chunk_size = size;
    EXPECT_NO_THROW(validate(header)) << size;
  }
  EXPECT_FALSE(valid_chunk_size(0));
  EXPECT_FALSE(valid_chunk_size(kMaxChunkSize + 64));
  EXPECT_NE(decode_error(RawDelta().header(100, 0).bytes()).find("multiple of 64"),
            std::string::npos);
}

TEST(DeltaFormatTest, RejectsBadHeaders) {
  const Bytes good = RawDelta().header(128, 0).end(0, digest_of("")).bytes();
  ASSERT_EQ(decode_error(good), "");
  for (std::size_t i = 0; i < 4; ++i) {
    Bytes bad = good;
    bad[i] = static_cast<std::uint8_t>(bad[i] + 1);
    EXPECT_NE(decode_error(bad).find("magic"), std::string::npos) << i;
  }
  Bytes bad_version = good;
  bad_version[4] = 9;
  EXPECT_NE(decode_error(bad_version).find("version"), std::string::npos);
  Bytes bad_flags = good;
  bad_flags[5] = 4;
  EXPECT_NE(decode_error(bad_flags).find("flags"), std::string::npos);

  EXPECT_NE(decode_error(RawDelta().header(0, 0).bytes()).find("chunk size"), std::string::npos);
  EXPECT_NE(decode_error(RawDelta().header(kMaxChunkSize + 1, 0).bytes()).find("chunk size"),
            std::string::npos);
  EXPECT_NE(decode_error(RawDelta().header(128, 0, 192).bytes()).find("multiple"),
            std::string::npos);
}

TEST(DeltaFormatTest, RejectsUnknownRecordTypes) {
  for (const std::uint8_t type : {0x00, 0x05, 0x7f, 0xff}) {
    const Bytes bytes = RawDelta().header(128, 0).byte(type).bytes();
    EXPECT_NE(decode_error(bytes).find("unknown delta record type"), std::string::npos)
        << int{type};
  }
}

TEST(DeltaFormatTest, RejectsEmptyOperations) {
  EXPECT_NE(decode_error(RawDelta().header(128, 50).copy(0, 0).bytes()).find("empty COPY"),
            std::string::npos);
  EXPECT_NE(decode_error(RawDelta().header(128, 50).literal(Bytes()).bytes()).find("empty LITERAL"),
            std::string::npos);
}

TEST(DeltaFormatTest, RejectsCopiesOutsideTheBasis) {
  EXPECT_EQ(decode_error(RawDelta().header(128, 50).copy(0, 50).bytes()).find("outside"),
            std::string::npos);
  EXPECT_NE(decode_error(RawDelta().header(128, 50).copy(1, 50).bytes()).find("outside"),
            std::string::npos);
  EXPECT_NE(decode_error(RawDelta().header(128, 50).copy(51, 1).bytes()).find("outside"),
            std::string::npos);
  // offset + length would wrap around 2^64.
  const std::uint64_t huge = std::numeric_limits<std::uint64_t>::max();
  EXPECT_NE(decode_error(RawDelta().header(128, 50).copy(huge, 2).bytes()).find("outside"),
            std::string::npos);
  EXPECT_NE(decode_error(RawDelta().header(128, 50).copy(10, huge).bytes()).find("outside"),
            std::string::npos);
}

TEST(DeltaFormatTest, RejectsOperationsThatCrossAChunkBoundary) {
  EXPECT_NE(decode_error(RawDelta().header(128, 1000).copy(0, 129).bytes()).find("crosses"),
            std::string::npos);
  const Bytes sixty(60, 1);
  const Bytes sixty_nine(69, 2);
  EXPECT_NE(decode_error(RawDelta().header(128, 0).literal(sixty).literal(sixty_nine).bytes())
                .find("crosses"),
            std::string::npos);
  // Exactly reaching the boundary is fine, but the chunk must then be closed
  // before more output.
  const Bytes sixty_eight(68, 3);
  EXPECT_NE(decode_error(RawDelta().header(128, 0).literal(sixty).literal(sixty_eight).literal(sixty).bytes())
                .find("crosses"),
            std::string::npos);
}

TEST(DeltaFormatTest, RejectsMisplacedChunkRecords) {
  const Sha256Digest any = digest_of("x");
  EXPECT_NE(decode_error(RawDelta().header(128, 0).chunk(any).bytes()).find("covers no output"),
            std::string::npos);
  const Bytes ten(10, 4);
  EXPECT_NE(decode_error(RawDelta().header(128, 0).literal(ten).chunk(any).chunk(any).bytes())
                .find("covers no output"),
            std::string::npos);
  // After a short chunk only END may follow.
  EXPECT_NE(decode_error(RawDelta().header(128, 0).literal(ten).chunk(any).literal(ten).bytes())
                .find("final, short chunk"),
            std::string::npos);
  EXPECT_NE(decode_error(RawDelta().header(128, 50).literal(ten).chunk(any).copy(0, 5).bytes())
                .find("final, short chunk"),
            std::string::npos);
}

TEST(DeltaFormatTest, RejectsInconsistentEnd) {
  const Sha256Digest any = digest_of("x");
  const Bytes ten(10, 5);
  EXPECT_NE(decode_error(RawDelta().header(128, 0).literal(ten).end(10, any).bytes())
                .find("not covered by a CHUNK"),
            std::string::npos);
  EXPECT_NE(decode_error(RawDelta().header(128, 0).literal(ten).chunk(any).end(11, any).bytes())
                .find("END declares 11"),
            std::string::npos);
  EXPECT_EQ(decode_error(RawDelta().header(128, 0).literal(ten).chunk(any).end(10, any).bytes()), "");
}

TEST(DeltaFormatTest, RejectsTrailingData) {
  Bytes bytes = serialize(sample_records());
  bytes.push_back(0x01);
  EXPECT_NE(decode_error(bytes).find("after the end"), std::string::npos);
}

TEST(DeltaFormatTest, EveryTruncationIsDetected) {
  const Bytes bytes = serialize(sample_records());
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    const Bytes truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
    EXPECT_NE(decode_error(truncated).find("truncated"), std::string::npos) << length;
  }
}

TEST(DeltaFormatTest, ResumedStreamsCountFromTheirStartOffset) {
  const Sha256Digest any = digest_of("x");
  const Bytes data(128, 6);
  const Bytes tail(7, 7);
  RecordingSink sink;
  const Bytes bytes = RawDelta()
                          .header(128, 0, 384)
                          .literal(data)
                          .chunk(any)
                          .literal(tail)
                          .chunk(any)
                          .end(519, any)
                          .bytes();
  ASSERT_EQ(decode_error(bytes, &sink), "");
  EXPECT_EQ(sink.records.front().header.start_offset, 384u);
  EXPECT_EQ(sink.records.back().length, 519u);
  // The total counts the bytes before the start offset as well.
  EXPECT_NE(decode_error(RawDelta().header(128, 0, 384).literal(tail).chunk(any).end(7, any).bytes())
                .find("END declares 7"),
            std::string::npos);
}

TEST(DeltaFormatTest, RejectsOutputBeyond64Bits) {
  const std::uint64_t chunk = kMaxChunkSize;
  const std::uint64_t last_chunk_start = std::numeric_limits<std::uint64_t>::max() / chunk * chunk;
  // A literal that fits the chunk but would push the position past 2^64 - 1.
  const Bytes bytes = RawDelta().header(chunk, 0, last_chunk_start).byte(0x02).varint(chunk).bytes();
  EXPECT_NE(decode_error(bytes).find("2^64"), std::string::npos);
}

}  // namespace
}  // namespace deltasync
