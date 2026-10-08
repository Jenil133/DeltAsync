#include "deltasync/bytes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "deltasync/error.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;

Bytes encode(std::uint64_t value) {
  std::uint8_t buffer[kMaxVarintLength];
  const std::size_t length = encode_varint(value, buffer);
  return Bytes(buffer, buffer + length);
}

// Feeds `bytes` and returns the decoded value; fails the test unless the
// value completes exactly on the last byte.
std::uint64_t decode(const Bytes& bytes) {
  VarintDecoder decoder;
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const bool complete = decoder.feed(bytes[i], &value);
    EXPECT_EQ(complete, i + 1 == bytes.size()) << "at byte " << i;
  }
  EXPECT_FALSE(decoder.in_progress());
  return value;
}

TEST(VarintTest, KnownEncodings) {
  EXPECT_EQ(encode(0), (Bytes{0x00}));
  EXPECT_EQ(encode(1), (Bytes{0x01}));
  EXPECT_EQ(encode(127), (Bytes{0x7f}));
  EXPECT_EQ(encode(128), (Bytes{0x80, 0x01}));
  EXPECT_EQ(encode(300), (Bytes{0xac, 0x02}));
  EXPECT_EQ(encode(16383), (Bytes{0xff, 0x7f}));
  EXPECT_EQ(encode(16384), (Bytes{0x80, 0x80, 0x01}));
  EXPECT_EQ(encode(std::numeric_limits<std::uint64_t>::max()),
            (Bytes{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}));
}

TEST(VarintTest, RoundTripsBoundaryValues) {
  std::vector<std::uint64_t> values = {0, 1, 2, 126, 127, 128, 129, 255, 256};
  for (unsigned bits = 7; bits < 64; bits += 7) {
    const std::uint64_t power = std::uint64_t{1} << bits;
    values.push_back(power - 1);
    values.push_back(power);
    values.push_back(power + 1);
  }
  values.push_back(std::numeric_limits<std::uint32_t>::max());
  values.push_back(std::uint64_t{1} << 63);
  values.push_back(std::numeric_limits<std::uint64_t>::max() - 1);
  values.push_back(std::numeric_limits<std::uint64_t>::max());
  for (const std::uint64_t value : values) {
    const Bytes encoded = encode(value);
    EXPECT_EQ(encoded.size(), varint_length(value)) << value;
    EXPECT_EQ(decode(encoded), value);
  }
}

TEST(VarintTest, RoundTripsRandomValues) {
  test::Rng rng(7);
  for (int i = 0; i < 10000; ++i) {
    // Spread values over every encoded length, not just the long ones.
    const std::uint64_t value = rng.next() >> rng.below(64);
    EXPECT_EQ(decode(encode(value)), value);
  }
}

TEST(VarintTest, DecoderHandlesConsecutiveValues) {
  Bytes stream;
  const std::vector<std::uint64_t> values = {5, 300, 0, std::uint64_t{1} << 40, 127};
  for (const std::uint64_t value : values) {
    const Bytes encoded = encode(value);
    stream.insert(stream.end(), encoded.begin(), encoded.end());
  }
  VarintDecoder decoder;
  std::vector<std::uint64_t> decoded;
  for (const std::uint8_t byte : stream) {
    std::uint64_t value = 0;
    if (decoder.feed(byte, &value)) decoded.push_back(value);
  }
  EXPECT_EQ(decoded, values);
}

TEST(VarintTest, RejectsNonMinimalEncodings) {
  // Zero spelled with two bytes, and 1 spelled with three.
  for (const Bytes& bad : {Bytes{0x80, 0x00}, Bytes{0x81, 0x80, 0x00}}) {
    VarintDecoder decoder;
    std::uint64_t value = 0;
    EXPECT_FALSE(decoder.feed(bad[0], &value));
    bool threw = false;
    try {
      for (std::size_t i = 1; i < bad.size(); ++i) decoder.feed(bad[i], &value);
    } catch (const FormatError&) {
      threw = true;
    }
    EXPECT_TRUE(threw);
  }
}

TEST(VarintTest, RejectsValuesAbove64Bits) {
  // Nine continuation bytes followed by a tenth byte carrying more than bit 63.
  VarintDecoder decoder;
  std::uint64_t value = 0;
  for (int i = 0; i < 9; ++i) ASSERT_FALSE(decoder.feed(0xff, &value));
  EXPECT_THROW(decoder.feed(0x02, &value), FormatError);

  // A continuation bit on the tenth byte would ask for an eleventh.
  VarintDecoder second;
  for (int i = 0; i < 9; ++i) ASSERT_FALSE(second.feed(0x80 | (i == 0 ? 1 : 0), &value));
  EXPECT_THROW(second.feed(0x81, &value), FormatError);
}

TEST(BigEndianTest, StoresMostSignificantByteFirst) {
  std::uint8_t buffer[8];
  store_be32(buffer, 0x01020304u);
  EXPECT_EQ(Bytes(buffer, buffer + 4), (Bytes{1, 2, 3, 4}));
  EXPECT_EQ(load_be32(buffer), 0x01020304u);

  store_be64(buffer, 0x0102030405060708ull);
  EXPECT_EQ(Bytes(buffer, buffer + 8), (Bytes{1, 2, 3, 4, 5, 6, 7, 8}));
  EXPECT_EQ(load_be64(buffer), 0x0102030405060708ull);

  store_be32(buffer, 0xffffffffu);
  EXPECT_EQ(load_be32(buffer), 0xffffffffu);
}

}  // namespace
}  // namespace deltasync
