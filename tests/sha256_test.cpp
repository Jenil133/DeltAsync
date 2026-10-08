#include "deltasync/sha256.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "deltasync/bytes.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;

std::string hex_of(const std::string& message) {
  return to_hex(Sha256::hash(message.data(), message.size()));
}

std::string hex_of(const Bytes& message) {
  return to_hex(Sha256::hash(message.data(), message.size()));
}

// The digest of a message that reached `state` and then continued with
// `tail`, with the padding built here by hand (FIPS 180-4, section 5.1.1)
// and its 64-bit length field set to `bit_length`. Only whole blocks are
// compressed, starting from a zero byte count, so none of Sha256's own
// length bookkeeping is involved.
Sha256Digest hand_padded_digest(Sha256::Midstate state, const Bytes& tail,
                                std::uint64_t bit_length) {
  Bytes padded = tail;
  padded.push_back(0x80);
  while (padded.size() % Sha256::kBlockLength != 56) padded.push_back(0);
  // Grow the buffer and store into it directly: a range insert from a
  // small array trips a false -Warray-bounds positive in some GCC releases.
  const std::size_t length_at = padded.size();
  padded.resize(length_at + 8);
  store_be64(padded.data() + length_at, bit_length);

  state.length = 0;
  Sha256 hasher = Sha256::from_midstate(state);
  hasher.update(padded.data(), padded.size());
  const Sha256::Midstate end = hasher.midstate();
  Sha256Digest digest;
  for (std::size_t i = 0; i < end.words.size(); ++i) {
    store_be32(digest.data() + 4 * i, end.words[i]);
  }
  return digest;
}

// Examples from FIPS 180-2 Appendix B and the NIST "SHA256ShortMsg" byte
// vectors (the 896-bit message is the one FIPS 180-2 uses for its SHA-384
// and SHA-512 examples; the SHA-256 digest of it is listed here). Each was
// also checked against `shasum -a 256` and OpenSSL.
TEST(Sha256Test, FipsExamples) {
  EXPECT_EQ(hex_of(std::string()),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  // One block.
  EXPECT_EQ(hex_of(std::string("abc")),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 448 bits: the padding no longer fits, so it spills into a second block.
  EXPECT_EQ(hex_of(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // 896 bits (from the SHA-384/512 examples): message and padding fill two blocks.
  EXPECT_EQ(hex_of(std::string("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                               "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
            "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(Sha256Test, CavpShortMessages) {
  EXPECT_EQ(hex_of(Bytes{0xd3}),
            "28969cdfa74a12c82f3bad960b0b000aca2ac329deea5c2328ebc6f2ba9802c1");
  EXPECT_EQ(hex_of(Bytes{0x11, 0xaf}),
            "5ca7133fa735326081558ac312c620eeca9970d1e70a4b95533d956f072d1f98");
  EXPECT_EQ(hex_of(Bytes{0xb4, 0x19, 0x0e}),
            "dff2e73091f6c05e528896c4c831b9448653dc2ff043528f6769437bc7b975c2");
  EXPECT_EQ(hex_of(Bytes{0x74, 0xba, 0x25, 0x21}),
            "b16aa56be3880d18cd41e68384cf1ec8c17680c45a02b1575dc1518923ae8b0e");
}

TEST(Sha256Test, OneMillionA) {
  const std::string expected = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
  EXPECT_EQ(hex_of(std::string(1000000, 'a')), expected);

  // The same message in uneven pieces that straddle block boundaries.
  Sha256 hasher;
  const std::string piece(997, 'a');
  std::size_t remaining = 1000000;
  while (remaining > 0) {
    const std::size_t take = std::min(remaining, piece.size());
    hasher.update(piece.data(), take);
    remaining -= take;
  }
  EXPECT_EQ(hasher.length(), 1000000u);
  EXPECT_EQ(to_hex(hasher.digest()), expected);
}

// Every way of splitting a message into two updates, for lengths around the
// 55/56/64-byte padding boundaries and across several blocks.
TEST(Sha256Test, SplitUpdatesMatchOneShot) {
  const Bytes message = test::random_bytes(200, 21);
  for (std::size_t length = 0; length <= message.size(); ++length) {
    const Sha256Digest expected = Sha256::hash(message.data(), length);
    for (std::size_t split = 0; split <= length; ++split) {
      Sha256 hasher;
      hasher.update(message.data(), split);
      hasher.update(message.data() + split, length - split);
      ASSERT_EQ(hasher.digest(), expected) << "length " << length << ", split " << split;
    }
  }
}

TEST(Sha256Test, ByteAtATimeMatchesOneShot) {
  const Bytes message = test::random_bytes(1000, 22);
  Sha256 hasher;
  for (const std::uint8_t byte : message) hasher.update(&byte, 1);
  EXPECT_EQ(hasher.digest(), Sha256::hash(message.data(), message.size()));
}

TEST(Sha256Test, DigestDoesNotDisturbTheRunningHash) {
  const std::string first = "abcdbcdecdefdefgefghfghighijhijk";
  const std::string second = "ijkljklmklmnlmnomnopnopq";
  Sha256 hasher;
  hasher.update(first.data(), first.size());
  EXPECT_EQ(to_hex(hasher.digest()), hex_of(first));
  hasher.update(second.data(), second.size());
  EXPECT_EQ(to_hex(hasher.digest()),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // Copies carry the state, which is how a prefix hash is handed around.
  Sha256 copy = hasher;
  EXPECT_EQ(copy.digest(), hasher.digest());
  EXPECT_EQ(copy.length(), first.size() + second.size());
}

TEST(Sha256Test, ResetStartsANewMessage) {
  Sha256 hasher;
  hasher.update("garbage", 7);
  hasher.reset();
  EXPECT_EQ(hasher.length(), 0u);
  hasher.update("abc", 3);
  EXPECT_EQ(to_hex(hasher.digest()),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, EmptyUpdatesAreIgnored) {
  Sha256 hasher;
  hasher.update(nullptr, 0);
  hasher.update("abc", 3);
  hasher.update(nullptr, 0);
  EXPECT_EQ(to_hex(hasher.digest()),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, MidstateContinuesTheMessage) {
  const Bytes message = test::random_bytes(1000, 23);
  for (const std::size_t split : {0, 64, 640, 960}) {
    Sha256 first;
    first.update(message.data(), split);
    const Sha256::Midstate state = first.midstate();
    EXPECT_EQ(state.length, split);
    Sha256 resumed = Sha256::from_midstate(state);
    EXPECT_EQ(resumed.length(), split);
    resumed.update(message.data() + split, message.size() - split);
    EXPECT_EQ(resumed.digest(), Sha256::hash(message.data(), message.size())) << split;
  }
}

TEST(Sha256Test, MidstateExistsOnlyAtBlockBoundaries) {
  Sha256 hasher;
  hasher.update("abc", 3);
  EXPECT_THROW(hasher.midstate(), std::logic_error);
  Sha256::Midstate state;
  state.length = 65;
  EXPECT_THROW(Sha256::from_midstate(state), std::invalid_argument);
}

// The padding ends with the message length in bits as a 64-bit big-endian
// number. Rather than hash gigabytes, the hasher is resumed from a midstate
// placed just before each length of interest and compared with hand-built
// padding; the stress suite also hashes a real 4 GiB + 3 byte message.
TEST(Sha256Test, LengthCounterIsSafePast4GiB) {
  const Sha256::Midstate initial = Sha256().midstate();
  // The helper itself reproduces a FIPS example.
  EXPECT_EQ(to_hex(hand_padded_digest(initial, test::bytes_of("abc"), 24)),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  const Bytes tail = test::random_bytes(200, 24);
  const std::uint64_t starts[] = {
      (std::uint64_t{1} << 29) - 64,    // the bit count passes 2^32 within the tail
      (std::uint64_t{1} << 32) - 128,   // the byte count passes 2^32 within the tail
      (std::uint64_t{1} << 32) + 64,    // already past 4 GiB
      std::uint64_t{1} << 40,           // 1 TiB
      (std::uint64_t{1} << 61) - 4096,  // close to the 2^61-byte limit of SHA-256
  };
  for (const std::uint64_t start : starts) {
    Sha256::Midstate state = initial;
    state.length = start;
    Sha256 hasher = Sha256::from_midstate(state);
    hasher.update(tail.data(), tail.size());
    const std::uint64_t total = start + tail.size();
    ASSERT_EQ(hasher.length(), total);
    EXPECT_EQ(hasher.digest(), hand_padded_digest(state, tail, total * 8)) << start;
    // What a 32-bit counter would have produced is distinguishable, so the
    // comparison above really checks the high bits.
    EXPECT_NE(hasher.digest(), hand_padded_digest(state, tail, (total * 8) & 0xffffffffu))
        << start;
  }
}

TEST(Sha256Test, HexRendering) {
  const std::uint8_t data[] = {0x00, 0x0f, 0xa5, 0xff};
  EXPECT_EQ(to_hex(data, sizeof(data)), "000fa5ff");
  EXPECT_EQ(to_hex(data, 0), "");
}

}  // namespace
}  // namespace deltasync
