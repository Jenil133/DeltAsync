// The headline property: a small edit to a large file yields a delta of a
// few blocks, not a copy of the file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iostream>

#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;

constexpr std::size_t kFileSize = std::size_t{64} << 20;
constexpr std::size_t kEditSize = 4096;

struct LargeResult {
  std::uint64_t signature_bytes = 0;
  std::uint64_t delta_bytes = 0;
  DeltaStats stats;
};

// Streams the target through the encoder and the decoded delta through the
// patcher; the rebuilt file is only hashed, never stored.
LargeResult sync_in_memory(const Bytes& basis, const Bytes& target) {
  LargeResult result;
  const Signature signature = test::make_signature(basis, default_block_size(basis.size()));
  result.signature_bytes = test::serialize(signature).size();

  const test::EncodedDelta delta = test::encode(signature, target);
  result.delta_bytes = delta.bytes.size();
  result.stats = delta.stats;

  MemoryRandomAccess basis_source(basis);
  MemorySource delta_source(delta.bytes);
  test::HashingSink output;
  apply_delta(basis_source, delta_source, output);
  EXPECT_EQ(output.bytes, target.size());
  EXPECT_EQ(output.hash.digest(), Sha256::hash(target.data(), target.size()));
  return result;
}

void report(const char* name, const LargeResult& result) {
  std::cout << name << ": file_bytes=" << kFileSize << " block_size="
            << default_block_size(kFileSize) << " signature_bytes=" << result.signature_bytes
            << " delta_bytes=" << result.delta_bytes
            << " literal_bytes=" << result.stats.literal_bytes
            << " copy_ops=" << result.stats.copy_ops << " chunks=" << result.stats.chunks
            << "\n";
}

class LargeFileTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { basis_ = new Bytes(test::random_bytes(kFileSize, 401)); }
  static void TearDownTestSuite() {
    delete basis_;
    basis_ = nullptr;
  }

  static Bytes* basis_;
};

Bytes* LargeFileTest::basis_ = nullptr;

TEST_F(LargeFileTest, FourKibOverwriteGivesATinyDelta) {
  Bytes target = *basis_;
  const Bytes edit = test::random_bytes(kEditSize, 402);
  // Starts 6000 bytes into an 8 KiB block, so the edit straddles two blocks.
  std::copy(edit.begin(), edit.end(), target.begin() + (33 << 20) + 6000);
  const LargeResult result = sync_in_memory(*basis_, target);
  report("overwrite", result);

  EXPECT_EQ(default_block_size(kFileSize), 8192u);
  EXPECT_LT(result.delta_bytes, kFileSize / 100);
  // Two damaged blocks are resent; everything else is copied.
  EXPECT_LE(result.stats.literal_bytes, 2u * 8192);
  EXPECT_EQ(result.stats.matched_bytes, kFileSize - result.stats.literal_bytes);
  // 8192 blocks of 20 bytes plus the header.
  EXPECT_LT(result.signature_bytes, 8192u * 20 + 64);
}

TEST_F(LargeFileTest, FourKibInsertionGivesATinyDelta) {
  Bytes target = *basis_;
  const Bytes edit = test::random_bytes(kEditSize, 403);
  target.insert(target.begin() + (17 << 20) + 777, edit.begin(), edit.end());
  const LargeResult result = sync_in_memory(*basis_, target);
  report("insert", result);
  EXPECT_LT(result.delta_bytes, kFileSize / 100);
  EXPECT_LE(result.stats.literal_bytes, kEditSize + 8192u);
}

TEST_F(LargeFileTest, FourKibDeletionGivesATinyDelta) {
  Bytes target = *basis_;
  target.erase(target.begin() + (50 << 20) + 4321, target.begin() + (50 << 20) + 4321 + kEditSize);
  const LargeResult result = sync_in_memory(*basis_, target);
  report("delete", result);
  EXPECT_LT(result.delta_bytes, kFileSize / 100);
  EXPECT_LE(result.stats.literal_bytes, 2u * 8192);
}

}  // namespace
}  // namespace deltasync
