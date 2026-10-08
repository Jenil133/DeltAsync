// Property tests: whatever the edits, basis + delta must rebuild the target
// exactly, and a single edit must cost little more than the bytes it changed.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>

#include "edit_model.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;
using test::EditKind;
using test::Where;

using SingleEditParams = std::tuple<EditKind, Where, std::uint32_t>;

class SingleEditTest : public ::testing::TestWithParam<SingleEditParams> {};

TEST_P(SingleEditTest, RebuildsTheTargetAndResendsOnlyTheEditedRegion) {
  const auto [kind, where, block_size] = GetParam();
  test::Rng rng(1000 + static_cast<std::uint64_t>(kind) * 100 + static_cast<std::uint64_t>(where) * 10 +
                block_size);
  for (int round = 0; round < 4; ++round) {
    const Bytes basis = test::random_bytes(30000 + rng.below(5000), rng.next());
    Bytes target = basis;
    const std::size_t touched = test::apply_edit(target, kind, where, rng.between(1, 3000), rng);
    const test::RoundTrip result = test::round_trip(basis, target, block_size);
    ASSERT_TRUE(test::same_bytes(target, result.output)) << "round " << round;
    EXPECT_EQ(result.delta_stats.matched_bytes + result.delta_stats.literal_bytes, target.size());
    // Blocks untouched by the edit are all found again; what is resent is
    // the edited bytes plus the partly edited blocks around them (a move
    // disturbs two places).
    EXPECT_LE(result.delta_stats.literal_bytes, touched + 4 * block_size) << "round " << round;
  }
}

std::string single_edit_name(const ::testing::TestParamInfo<SingleEditParams>& info) {
  return std::string(test::name_of(std::get<0>(info.param))) + "_" +
         test::name_of(std::get<1>(info.param)) + "_block" +
         std::to_string(std::get<2>(info.param));
}

INSTANTIATE_TEST_SUITE_P(
    AllEdits, SingleEditTest,
    ::testing::Combine(::testing::Values(EditKind::kInsert, EditKind::kDelete, EditKind::kOverwrite,
                                         EditKind::kMove),
                       ::testing::Values(Where::kStart, Where::kMiddle, Where::kEnd, Where::kAnywhere),
                       ::testing::Values(16u, 700u, 4096u)),
    single_edit_name);

TEST(RandomEditsTest, RandomEditSequencesRoundTrip) {
  const std::uint32_t block_sizes[] = {1, 2, 5, 16, 64, 300, 1024, 4096, 65536};
  // Chunk sizes must be multiples of the SHA-256 block (64) so every chunk
  // boundary is a resume point; 64 is the smallest and 192 is not a power of two.
  const std::uint64_t chunk_sizes[] = {64, 192, 512, 4096, kDefaultChunkSize};
  const std::size_t max_literals[] = {1, 100, kDefaultMaxLiteral};
  const EditKind kinds[] = {EditKind::kInsert, EditKind::kDelete, EditKind::kOverwrite,
                            EditKind::kMove};
  const Where wheres[] = {Where::kStart, Where::kMiddle, Where::kEnd, Where::kAnywhere};

  test::Rng rng(301);
  for (int iteration = 0; iteration < 150; ++iteration) {
    const bool text_like = rng.below(2) == 0;
    const Bytes basis = test::make_basis(rng.below(40000), text_like, rng);
    Bytes target = basis;
    const std::uint64_t edits = rng.below(8);
    for (std::uint64_t edit = 0; edit < edits; ++edit) {
      test::apply_edit(target, kinds[rng.below(4)], wheres[rng.below(4)], rng.between(1, 2000), rng);
    }
    const std::uint32_t block_size = rng.below(4) == 0 ? default_block_size(basis.size())
                                                       : block_sizes[rng.below(9)];
    DeltaOptions options;
    options.chunk_size = chunk_sizes[rng.below(5)];
    options.max_literal = max_literals[rng.below(3)];

    const test::RoundTrip result = test::round_trip(basis, target, block_size, options);
    ASSERT_TRUE(test::same_bytes(target, result.output))
        << "iteration " << iteration << ": basis " << basis.size() << " bytes, block size "
        << block_size << ", chunk size " << options.chunk_size;
    ASSERT_EQ(result.delta_stats.matched_bytes + result.delta_stats.literal_bytes, target.size());
    ASSERT_EQ(result.patch_stats.output_bytes, target.size());
  }
}

TEST(RandomEditsTest, TextWithSmallEditsIsMostlyCopied) {
  test::Rng rng(302);
  const Bytes basis = test::make_basis(200000, true, rng);
  Bytes target = basis;
  std::size_t touched = 0;
  for (int edit = 0; edit < 5; ++edit) {
    touched += test::apply_edit(target, EditKind::kOverwrite, Where::kAnywhere, 20, rng);
    touched += test::apply_edit(target, EditKind::kInsert, Where::kAnywhere, 30, rng);
  }
  const test::RoundTrip result = test::round_trip(basis, target, 1024);
  ASSERT_TRUE(test::same_bytes(target, result.output));
  EXPECT_LE(result.delta_stats.literal_bytes, touched + 10 * 2 * 1024);
}

}  // namespace
}  // namespace deltasync
