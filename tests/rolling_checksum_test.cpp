#include "deltasync/rolling_checksum.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;

// Direct evaluation of the definition, independent of the incremental code.
std::uint32_t reference_checksum(const std::uint8_t* data, std::size_t length) {
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  for (std::size_t i = 0; i < length; ++i) {
    const std::uint64_t y = data[i] + RollingChecksum::kCharOffset;
    a += y;
    b += (length - i) * y;
  }
  return static_cast<std::uint32_t>((a & 0xffff) | ((b & 0xffff) << 16));
}

TEST(RollingChecksumTest, MatchesDefinitionOnSmallInputs) {
  EXPECT_EQ(RollingChecksum::compute(nullptr, 0), 0u);
  const std::uint8_t one[] = {0};
  // a = 31, b = 1 * 31.
  EXPECT_EQ(RollingChecksum::compute(one, 1), 31u | (31u << 16));
  const std::uint8_t abc[] = {'a', 'b', 'c'};
  // y = 128, 129, 130: a = 387, b = 3*128 + 2*129 + 130 = 772.
  EXPECT_EQ(RollingChecksum::compute(abc, 3), 387u | (772u << 16));
  EXPECT_EQ(RollingChecksum::compute(abc, 3), reference_checksum(abc, 3));
}

TEST(RollingChecksumTest, ReducesSumsModulo65536) {
  // 4096 bytes of 0xff: a = 4096 * 286 overflows 16 bits many times over.
  const Bytes data(4096, 0xff);
  EXPECT_EQ(RollingChecksum::compute(data.data(), data.size()),
            reference_checksum(data.data(), data.size()));
}

// The property the encoder depends on: rotating the window one byte at a
// time gives exactly the checksum a fresh computation over the window would.
TEST(RollingChecksumTest, RotationMatchesRecomputationAtEveryOffset) {
  const std::size_t window_sizes[] = {1, 2, 3, 7, 16, 64, 255, 256, 1000, 4096};
  std::uint64_t seed = 1;
  for (const std::size_t window : window_sizes) {
    for (const std::size_t length : {window, window + 1, 3 * window + 17, std::size_t{9000}}) {
      if (length < window) continue;
      const Bytes data = test::random_bytes(length, seed++);
      RollingChecksum rolling;
      rolling.update(data.data(), window);
      ASSERT_EQ(rolling.digest(), reference_checksum(data.data(), window));
      for (std::size_t start = 1; start + window <= length; ++start) {
        rolling.rotate(data[start - 1], data[start + window - 1]);
        ASSERT_EQ(rolling.count(), window);
        ASSERT_EQ(rolling.digest(), reference_checksum(data.data() + start, window))
            << "window " << window << ", offset " << start;
        ASSERT_EQ(rolling.digest(), RollingChecksum::compute(data.data() + start, window));
      }
    }
  }
}

TEST(RollingChecksumTest, RotationMatchesOnLowEntropyData) {
  // Long runs and a tiny alphabet stress the wrap-around arithmetic
  // differently from random bytes.
  Bytes data;
  test::Rng rng(99);
  while (data.size() < 20000) {
    const std::uint8_t value = static_cast<std::uint8_t>(rng.below(3) * 0x7f);
    data.insert(data.end(), rng.between(1, 300), value);
  }
  for (const std::size_t window : {std::size_t{1}, std::size_t{31}, std::size_t{2048}}) {
    RollingChecksum rolling;
    rolling.update(data.data(), window);
    for (std::size_t start = 1; start + window <= data.size(); ++start) {
      rolling.rotate(data[start - 1], data[start + window - 1]);
      ASSERT_EQ(rolling.digest(), reference_checksum(data.data() + start, window))
          << "window " << window << ", offset " << start;
    }
  }
}

TEST(RollingChecksumTest, RollInGrowsTheWindow) {
  const Bytes data = test::random_bytes(3000, 11);
  RollingChecksum rolling;
  for (std::size_t length = 1; length <= data.size(); ++length) {
    rolling.roll_in(data[length - 1]);
    ASSERT_EQ(rolling.count(), length);
    ASSERT_EQ(rolling.digest(), reference_checksum(data.data(), length)) << length;
  }
}

TEST(RollingChecksumTest, RollOutShrinksTheWindow) {
  const Bytes data = test::random_bytes(3000, 12);
  RollingChecksum rolling;
  rolling.update(data.data(), data.size());
  for (std::size_t start = 1; start <= data.size(); ++start) {
    rolling.roll_out(data[start - 1]);
    ASSERT_EQ(rolling.count(), data.size() - start);
    ASSERT_EQ(rolling.digest(), reference_checksum(data.data() + start, data.size() - start))
        << start;
  }
  EXPECT_EQ(rolling.digest(), 0u);
}

TEST(RollingChecksumTest, UpdateInPiecesEqualsOneUpdate) {
  const Bytes data = test::random_bytes(5000, 13);
  test::Rng rng(14);
  RollingChecksum pieces;
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t piece = std::min<std::size_t>(rng.between(0, 700), data.size() - offset);
    pieces.update(data.data() + offset, piece);
    offset += piece;
  }
  EXPECT_EQ(pieces.digest(), RollingChecksum::compute(data.data(), data.size()));
}

TEST(RollingChecksumTest, CharOffsetSeparatesZeroRunsOfDifferentLength) {
  const Bytes zeros(100, 0);
  EXPECT_NE(RollingChecksum::compute(zeros.data(), 10), RollingChecksum::compute(zeros.data(), 11));
}

// The weak checksum is only a filter. Adding (+1, -2, +1) to three
// consecutive bytes leaves both sums unchanged, which the delta tests use
// to build blocks that collide on purpose.
TEST(RollingChecksumTest, KnownCollisionPatternCollides) {
  Bytes block = test::random_bytes(512, 15);
  for (std::size_t i = 0; i < 3; ++i) block[100 + i] = 100;
  Bytes twin = block;
  twin[100] += 1;
  twin[101] -= 2;
  twin[102] += 1;
  ASSERT_NE(block, twin);
  EXPECT_EQ(RollingChecksum::compute(block.data(), block.size()),
            RollingChecksum::compute(twin.data(), twin.size()));
}

}  // namespace
}  // namespace deltasync
