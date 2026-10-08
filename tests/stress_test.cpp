// Long-running tests, labelled "stress" and excluded from the default suite:
//   ctest --preset <name>-stress

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "deltasync/sha256.h"
#include "edit_model.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;

// The 64-byte pattern of the widely published "extremely long message"
// SHA-2 test vector.
const std::string kLongMessagePattern =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno";

// Hashes `repeats` copies of the pattern followed by `tail`, feeding 1 MiB
// at a time.
Sha256Digest hash_repeated_pattern(std::uint64_t repeats, const std::string& tail) {
  std::string block;
  for (int i = 0; i < 16384; ++i) block += kLongMessagePattern;  // 1 MiB
  Sha256 hasher;
  const std::uint64_t full_blocks = repeats / 16384;
  for (std::uint64_t i = 0; i < full_blocks; ++i) hasher.update(block.data(), block.size());
  for (std::uint64_t i = 0; i < repeats % 16384; ++i) {
    hasher.update(kLongMessagePattern.data(), kLongMessagePattern.size());
  }
  hasher.update(tail.data(), tail.size());
  EXPECT_EQ(hasher.length(), repeats * 64 + tail.size());
  return hasher.digest();
}

// The pattern repeated 16,777,216 times (1 GiB); its bit length, 2^33, does
// not fit in 32 bits. The published digest was confirmed with both
// `openssl dgst -sha256` (OpenSSL 3.6.3) and `shasum -a 256`.
TEST(Sha256StressTest, ExtremelyLongMessage) {
  EXPECT_EQ(to_hex(hash_repeated_pattern(16777216, "")),
            "50e72a0e26442fe2552dc3938ac58658228c0cbfb1d2ca872ae435266fcd055e");
}

// 4 GiB + 3 bytes: the byte count itself no longer fits in 32 bits. The
// expected digest was computed independently with both
// `openssl dgst -sha256` (OpenSSL 3.6.3) and `shasum -a 256` over the same
// stream (the pattern 67,108,864 times, then "abc").
TEST(Sha256StressTest, MessageLongerThan4GiB) {
  EXPECT_EQ(to_hex(hash_repeated_pattern(67108864, "abc")),
            "90455151a0d960de7cfd735f323bac613f3fbf63e62b000b74ce363790864d18");
}

TEST(RoundTripStressTest, ThousandsOfRandomEditSequences) {
  const test::EditKind kinds[] = {test::EditKind::kInsert, test::EditKind::kDelete,
                                  test::EditKind::kOverwrite, test::EditKind::kMove};
  const test::Where wheres[] = {test::Where::kStart, test::Where::kMiddle, test::Where::kEnd,
                                test::Where::kAnywhere};
  test::Rng rng(801);
  for (int iteration = 0; iteration < 3000; ++iteration) {
    const Bytes basis = test::make_basis(rng.below(200000), rng.below(2) == 0, rng);
    Bytes target = basis;
    const std::uint64_t edits = rng.below(20);
    for (std::uint64_t edit = 0; edit < edits; ++edit) {
      test::apply_edit(target, kinds[rng.below(4)], wheres[rng.below(4)],
                       rng.between(1, 1 + rng.below(20000)), rng);
    }
    const std::uint32_t block_size = static_cast<std::uint32_t>(rng.between(1, 9000));
    DeltaOptions options;
    options.chunk_size = rng.between(1, 300000 / 64) * 64;  // multiples of 64 only
    options.max_literal = rng.between(1, 100000);
    const test::RoundTrip result = test::round_trip(basis, target, block_size, options);
    ASSERT_TRUE(test::same_bytes(target, result.output))
        << "iteration " << iteration << ", block size " << block_size << ", chunk size "
        << options.chunk_size << ", max literal " << options.max_literal;
  }
}

TEST(RoundTripStressTest, ScatteredEditsTo256MiB) {
  const std::size_t size = std::size_t{256} << 20;
  const Bytes basis = test::random_bytes(size, 802);
  Bytes target = basis;
  test::Rng rng(803);
  std::size_t touched = 0;
  for (int edit = 0; edit < 25; ++edit) {
    const auto kind = edit % 3 == 0   ? test::EditKind::kInsert
                      : edit % 3 == 1 ? test::EditKind::kDelete
                                      : test::EditKind::kOverwrite;
    touched += test::apply_edit(target, kind, test::Where::kAnywhere, rng.between(1, 4096), rng);
  }
  const std::uint32_t block_size = default_block_size(size);
  const Signature signature = test::make_signature(basis, block_size);
  const test::EncodedDelta delta = test::encode(signature, target);

  MemoryRandomAccess basis_source(basis);
  MemorySource delta_source(delta.bytes);
  test::HashingSink output;
  apply_delta(basis_source, delta_source, output);
  EXPECT_EQ(output.bytes, target.size());
  EXPECT_EQ(output.hash.digest(), Sha256::hash(target.data(), target.size()));
  // Each edit resends at most about two blocks besides its own bytes.
  EXPECT_LE(delta.stats.literal_bytes, touched + 25 * 2 * std::size_t{block_size});
  EXPECT_LT(delta.bytes.size(), size / 100);
}

}  // namespace
}  // namespace deltasync
