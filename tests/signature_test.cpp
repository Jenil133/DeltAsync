#include "deltasync/signature.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "deltasync/error.h"
#include "deltasync/rolling_checksum.h"
#include "deltasync/sha256.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;

void expect_same_signature(const Signature& expected, const Signature& actual) {
  ASSERT_EQ(expected.header().block_size, actual.header().block_size);
  ASSERT_EQ(expected.header().strong_length, actual.header().strong_length);
  ASSERT_EQ(expected.header().basis_size, actual.header().basis_size);
  ASSERT_EQ(expected.block_count(), actual.block_count());
  for (std::uint32_t i = 0; i < expected.block_count(); ++i) {
    ASSERT_EQ(expected.weak(i), actual.weak(i)) << "block " << i;
    ASSERT_EQ(0, std::memcmp(expected.strong(i), actual.strong(i), expected.strong_length()))
        << "block " << i;
  }
}

Signature decode_in_pieces(const Bytes& serialized, test::Rng& rng, std::size_t max_piece) {
  SignatureBuilder builder;
  SignatureDecoder decoder(builder);
  std::size_t offset = 0;
  while (offset < serialized.size()) {
    const std::size_t piece =
        std::min<std::size_t>(rng.between(0, max_piece), serialized.size() - offset);
    decoder.feed(serialized.data() + offset, piece);
    offset += piece;
  }
  decoder.finish();
  EXPECT_TRUE(decoder.done());
  return builder.take();
}

// Decodes `bytes` as a signature and returns the FormatError message, or an
// empty string if decoding succeeded.
std::string decode_error(const Bytes& bytes) {
  SignatureBuilder builder;
  SignatureDecoder decoder(builder);
  try {
    decoder.feed(bytes.data(), bytes.size());
    decoder.finish();
  } catch (const FormatError& error) {
    return error.what();
  }
  return std::string();
}

TEST(BlockSizePolicyTest, FollowsSquareRootWithinBounds) {
  EXPECT_EQ(default_block_size(0), 1024u);
  EXPECT_EQ(default_block_size(1), 1024u);
  EXPECT_EQ(default_block_size(1 << 20), 1024u);            // sqrt = 1024
  EXPECT_EQ(default_block_size(1025ull * 1025), 2048u);     // rounded up to 1 KiB
  EXPECT_EQ(default_block_size(64ull << 20), 8192u);        // 64 MiB
  EXPECT_EQ(default_block_size(100ull << 20), 10240u);      // 100 MiB
  EXPECT_EQ(default_block_size(1ull << 30), 32768u);        // 1 GiB
  EXPECT_EQ(default_block_size((1ull << 34) - 1), 131072u); // just under 16 GiB
  EXPECT_EQ(default_block_size(1ull << 34), 131072u);
  EXPECT_EQ(default_block_size(std::numeric_limits<std::uint64_t>::max()), 131072u);

  // Monotonic and always a multiple of 1 KiB inside the documented range.
  std::uint32_t previous = 0;
  for (std::uint64_t size = 1; size < (1ull << 40); size = size * 3 / 2 + 1) {
    const std::uint32_t block = default_block_size(size);
    EXPECT_GE(block, previous);
    EXPECT_EQ(block % 1024, 0u);
    EXPECT_GE(block, kMinDefaultBlockSize);
    EXPECT_LE(block, kMaxDefaultBlockSize);
    previous = block;
  }
}

TEST(SignatureHeaderTest, BlockCountAndLengths) {
  SignatureHeader header;
  header.block_size = 100;
  header.basis_size = 0;
  EXPECT_EQ(header.block_count(), 0u);

  header.basis_size = 100;
  EXPECT_EQ(header.block_count(), 1u);
  EXPECT_EQ(header.block_length(0), 100u);

  header.basis_size = 250;
  EXPECT_EQ(header.block_count(), 3u);
  EXPECT_EQ(header.block_length(0), 100u);
  EXPECT_EQ(header.block_length(1), 100u);
  EXPECT_EQ(header.block_length(2), 50u);

  header.basis_size = std::numeric_limits<std::uint64_t>::max();
  header.block_size = kMaxBlockSize;
  EXPECT_EQ(header.block_count(),
            std::numeric_limits<std::uint64_t>::max() / kMaxBlockSize + 1);
}

TEST(SignatureHeaderTest, ValidateRejectsOutOfRangeFields) {
  SignatureHeader header;
  header.block_size = 1024;
  header.basis_size = 1 << 20;
  EXPECT_NO_THROW(validate(header));

  SignatureHeader bad = header;
  bad.block_size = 0;
  EXPECT_THROW(validate(bad), FormatError);
  bad.block_size = kMaxBlockSize + 1;
  EXPECT_THROW(validate(bad), FormatError);

  bad = header;
  bad.strong_length = kMinStrongLength - 1;
  EXPECT_THROW(validate(bad), FormatError);
  bad.strong_length = kMaxStrongLength + 1;
  EXPECT_THROW(validate(bad), FormatError);

  bad = header;
  bad.block_size = 1;
  bad.basis_size = kMaxBlockCount + 1;
  EXPECT_THROW(validate(bad), FormatError);
  bad.basis_size = kMaxBlockCount;
  EXPECT_NO_THROW(validate(bad));
}

TEST(SignatureGeneratorTest, EntriesMatchDirectComputation) {
  const Bytes basis = test::random_bytes(1000, 31);
  const Signature signature = test::make_signature(basis, 128);
  ASSERT_TRUE(signature.complete());
  ASSERT_EQ(signature.block_count(), 8u);  // 7 full blocks and one of 104 bytes
  for (std::uint32_t i = 0; i < signature.block_count(); ++i) {
    const std::size_t start = std::size_t{i} * 128;
    const std::size_t length = std::min<std::size_t>(128, basis.size() - start);
    EXPECT_EQ(signature.block_length(i), length);
    EXPECT_EQ(signature.weak(i), RollingChecksum::compute(basis.data() + start, length));
    const Sha256Digest strong = Sha256::hash(basis.data() + start, length);
    EXPECT_EQ(0, std::memcmp(signature.strong(i), strong.data(), kDefaultStrongLength));
  }
}

TEST(SignatureGeneratorTest, FeedingInPiecesGivesTheSameSignature) {
  const Bytes basis = test::random_bytes(10000, 32);
  const Signature whole = test::make_signature(basis, 333);
  test::Rng rng(33);
  for (int round = 0; round < 20; ++round) {
    SignatureHeader header;
    header.block_size = 333;
    header.basis_size = basis.size();
    SignatureBuilder builder;
    SignatureGenerator generator(header, builder);
    std::size_t offset = 0;
    while (offset < basis.size()) {
      const std::size_t piece = std::min<std::size_t>(rng.between(0, 900), basis.size() - offset);
      generator.feed(basis.data() + offset, piece);
      offset += piece;
    }
    generator.finish();
    expect_same_signature(whole, builder.take());
  }
}

TEST(SignatureGeneratorTest, EmptyBasisHasNoBlocks) {
  const Signature signature = test::make_signature(Bytes(), 1024);
  EXPECT_TRUE(signature.complete());
  EXPECT_EQ(signature.block_count(), 0u);
  EXPECT_EQ(signature.header().basis_size, 0u);
}

TEST(SignatureGeneratorTest, RejectsABasisOfTheWrongLength) {
  SignatureHeader header;
  header.block_size = 16;
  header.basis_size = 100;
  const Bytes data(101, 7);

  SignatureBuilder longer_builder;
  SignatureGenerator longer(header, longer_builder);
  longer.feed(data.data(), 60);
  EXPECT_THROW(longer.feed(data.data(), 41), Error);

  SignatureBuilder shorter_builder;
  SignatureGenerator shorter(header, shorter_builder);
  shorter.feed(data.data(), 99);
  EXPECT_THROW(shorter.finish(), Error);
}

TEST(SignatureGeneratorTest, TruncatesStrongChecksumsToTheRequestedLength) {
  const Bytes basis = test::random_bytes(512, 34);
  for (const std::uint32_t strong_length : {kMinStrongLength, 20u, kMaxStrongLength}) {
    const Signature signature = test::make_signature(basis, 64, strong_length);
    const Bytes serialized = test::serialize(signature);
    // Header plus fixed-size entries.
    EXPECT_EQ(serialized.size(),
              4 + 1 + 1 + 1 + 1 + varint_length(512) + 8 * (4 + strong_length));
    const Sha256Digest full = Sha256::hash(basis.data(), 64);
    EXPECT_EQ(0, std::memcmp(signature.strong(0), full.data(), strong_length));
  }
}

TEST(SignatureFormatTest, RoundTripsThroughSerialization) {
  for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{4095},
                                 std::size_t{4096}, std::size_t{100000}}) {
    const Bytes basis = test::random_bytes(size, 35 + size);
    const Signature original = test::make_signature(basis, 4096);
    const Bytes serialized = test::serialize(original);
    MemorySource source(serialized);
    expect_same_signature(original, read_signature(source));
  }
}

TEST(SignatureFormatTest, DecoderAcceptsAnySplitOfTheInput) {
  const Bytes basis = test::random_bytes(20000, 36);
  const Signature original = test::make_signature(basis, 1000);
  const Bytes serialized = test::serialize(original);
  test::Rng rng(37);
  expect_same_signature(original, decode_in_pieces(serialized, rng, 1));
  for (const std::size_t max_piece : {3, 19, 24, 100, 5000}) {
    for (int round = 0; round < 5; ++round) {
      expect_same_signature(original, decode_in_pieces(serialized, rng, max_piece));
    }
  }
}

TEST(SignatureFormatTest, EmptyBasisSignatureIsJustAHeader) {
  const Bytes serialized = test::serialize(test::make_signature(Bytes(), 2048));
  // magic, version, flags, block size (2 bytes), strong length, basis size.
  EXPECT_EQ(serialized.size(), 4u + 1 + 1 + 2 + 1 + 1);
  EXPECT_EQ(decode_error(serialized), "");
}

TEST(SignatureFormatTest, RejectsBadMagicVersionAndFlags) {
  const Bytes good = test::serialize(test::make_signature(test::random_bytes(300, 38), 100));
  ASSERT_EQ(decode_error(good), "");
  for (std::size_t i = 0; i < 4; ++i) {
    Bytes bad = good;
    bad[i] ^= 0x20;
    EXPECT_NE(decode_error(bad).find("magic"), std::string::npos) << i;
  }
  Bytes bad_version = good;
  bad_version[4] = 2;
  EXPECT_NE(decode_error(bad_version).find("version"), std::string::npos);
  Bytes bad_flags = good;
  bad_flags[5] = 1;
  EXPECT_NE(decode_error(bad_flags).find("flags"), std::string::npos);
}

// Builds a raw header so out-of-range fields can be encoded at all.
Bytes raw_header(std::uint64_t block_size, std::uint64_t strong_length, std::uint64_t basis_size) {
  Bytes out = {'D', 'S', 'I', 'G', kSignatureVersion, 0};
  for (const std::uint64_t value : {block_size, strong_length, basis_size}) {
    std::uint8_t buffer[kMaxVarintLength];
    out.insert(out.end(), buffer, buffer + encode_varint(value, buffer));
  }
  return out;
}

TEST(SignatureFormatTest, RejectsOutOfRangeHeaderFields) {
  EXPECT_EQ(decode_error(raw_header(16, 16, 0)), "");
  EXPECT_NE(decode_error(raw_header(0, 16, 0)), "");
  EXPECT_NE(decode_error(raw_header(kMaxBlockSize + 1ull, 16, 0)), "");
  EXPECT_NE(decode_error(raw_header(std::uint64_t{1} << 40, 16, 0)), "");
  EXPECT_NE(decode_error(raw_header(16, kMinStrongLength - 1, 0)), "");
  EXPECT_NE(decode_error(raw_header(16, kMaxStrongLength + 1, 0)), "");
  // A header claiming billions of blocks is refused before anything is
  // allocated for them.
  EXPECT_NE(decode_error(raw_header(1, 16, std::uint64_t{1} << 40)), "");
}

TEST(SignatureFormatTest, EveryTruncationIsDetected) {
  const Bytes good = test::serialize(test::make_signature(test::random_bytes(1000, 39), 128));
  for (std::size_t length = 0; length < good.size(); ++length) {
    const Bytes truncated(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
    EXPECT_NE(decode_error(truncated).find("truncated"), std::string::npos) << length;
  }
}

TEST(SignatureFormatTest, RejectsTrailingData) {
  Bytes bytes = test::serialize(test::make_signature(test::random_bytes(1000, 40), 128));
  bytes.push_back(0);
  EXPECT_NE(decode_error(bytes).find("after the end"), std::string::npos);

  Bytes empty = test::serialize(test::make_signature(Bytes(), 128));
  empty.push_back(0);
  EXPECT_NE(decode_error(empty).find("after the end"), std::string::npos);
}

TEST(SignatureBuilderTest, RejectsIncompleteOrOverfullSignatures) {
  SignatureHeader header;
  header.block_size = 10;
  header.basis_size = 25;  // three blocks
  const std::uint8_t strong[kMaxStrongLength] = {};

  SignatureBuilder short_builder;
  short_builder.on_header(header);
  short_builder.on_block(1, strong);
  EXPECT_THROW(short_builder.on_end(), FormatError);

  SignatureBuilder long_builder;
  long_builder.on_header(header);
  for (int i = 0; i < 3; ++i) long_builder.on_block(1, strong);
  EXPECT_THROW(long_builder.on_block(1, strong), FormatError);
  long_builder.on_end();
  EXPECT_TRUE(long_builder.done());
  EXPECT_EQ(long_builder.take().block_count(), 3u);
}

}  // namespace
}  // namespace deltasync
