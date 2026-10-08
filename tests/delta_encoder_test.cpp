#include "deltasync/delta_encoder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "deltasync/error.h"
#include "deltasync/rolling_checksum.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;
using test::Record;
using Kind = test::Record::Kind;

// Runs serialized delta bytes through the validating decoder. The whole
// delta is fed at once, so each LITERAL record arrives as one callback and
// the recorded literals correspond one-to-one to records.
std::vector<Record> decode_records(const Bytes& delta) {
  test::RecordingSink sink;
  sink.merge_literals = false;
  DeltaDecoder decoder(sink);
  decoder.feed(delta.data(), delta.size());
  decoder.finish();
  return sink.records;
}

struct Summary {
  std::size_t copies = 0;
  std::size_t literals = 0;
  std::size_t chunks = 0;
  std::uint64_t copied = 0;
  std::uint64_t literal_bytes = 0;
};

Summary summarize(const std::vector<Record>& records) {
  Summary summary;
  for (const Record& record : records) {
    if (record.kind == Kind::kCopy) {
      ++summary.copies;
      summary.copied += record.length;
    } else if (record.kind == Kind::kLiteral) {
      ++summary.literals;
      summary.literal_bytes += record.data.size();
    } else if (record.kind == Kind::kChunk) {
      ++summary.chunks;
    }
  }
  return summary;
}

// Recomputes every CHUNK and END digest from the target itself.
void expect_digests_match_target(const std::vector<Record>& records, const Bytes& target,
                                 std::uint64_t start = 0) {
  std::uint64_t position = start;
  std::uint64_t chunk_start = start;
  for (const Record& record : records) {
    if (record.kind == Kind::kCopy) position += record.length;
    if (record.kind == Kind::kLiteral) position += record.data.size();
    if (record.kind == Kind::kChunk) {
      EXPECT_EQ(record.digest, Sha256::hash(target.data() + chunk_start, position - chunk_start))
          << "chunk at " << chunk_start;
      chunk_start = position;
    }
    if (record.kind == Kind::kEnd) {
      EXPECT_EQ(record.length, target.size());
      EXPECT_EQ(record.digest, Sha256::hash(target.data(), target.size()));
    }
  }
  EXPECT_EQ(position, target.size());
}

// Encodes, checks the structure and digests, and patches the basis.
struct Checked {
  test::EncodedDelta delta;
  std::vector<Record> records;
  Summary summary;
};

Checked encode_and_check(const Bytes& basis, const Bytes& target, std::uint32_t block_size,
                         const DeltaOptions& options = DeltaOptions()) {
  Checked result;
  result.delta = test::encode(test::make_signature(basis, block_size), target, options);
  result.records = decode_records(result.delta.bytes);
  result.summary = summarize(result.records);
  expect_digests_match_target(result.records, target);
  EXPECT_TRUE(test::same_bytes(target, test::apply(basis, result.delta.bytes)));
  const DeltaStats& stats = result.delta.stats;
  EXPECT_EQ(stats.target_bytes, target.size());
  EXPECT_EQ(stats.matched_bytes + stats.literal_bytes, target.size());
  EXPECT_EQ(stats.matched_bytes, result.summary.copied);
  EXPECT_EQ(stats.literal_bytes, result.summary.literal_bytes);
  EXPECT_EQ(stats.copy_ops, result.summary.copies);
  EXPECT_EQ(stats.literal_ops, result.summary.literals);
  EXPECT_EQ(stats.chunks, result.summary.chunks);
  return result;
}

Bytes concat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

Bytes slice(const Bytes& data, std::size_t start, std::size_t length) {
  return Bytes(data.begin() + static_cast<std::ptrdiff_t>(start),
               data.begin() + static_cast<std::ptrdiff_t>(start + length));
}

TEST(DeltaEncoderTest, IdenticalFilesBecomeOneCopy) {
  const Bytes basis = test::random_bytes(100000, 61);
  const Checked result = encode_and_check(basis, basis, 1024);
  ASSERT_EQ(result.records.size(), 4u);
  EXPECT_EQ(result.records[1].kind, Kind::kCopy);
  EXPECT_EQ(result.records[1].offset, 0u);
  EXPECT_EQ(result.records[1].length, basis.size());
  EXPECT_EQ(result.records[2].kind, Kind::kChunk);
  EXPECT_EQ(result.records[3].kind, Kind::kEnd);
  EXPECT_EQ(result.delta.stats.literal_bytes, 0u);
  EXPECT_LT(result.delta.bytes.size(), 100u);
}

TEST(DeltaEncoderTest, IdenticalFilesMatchFullyAtAnyBlockSize) {
  const Bytes basis = test::random_bytes(20000, 62);
  for (const std::uint32_t block_size : {1u, 2u, 3u, 64u, 1000u, 4096u, 19999u, 20000u, 65536u}) {
    SCOPED_TRACE("block size " + std::to_string(block_size));
    const Checked result = encode_and_check(basis, basis, block_size);
    EXPECT_EQ(result.delta.stats.literal_bytes, 0u);
    // Even with one-byte blocks, where almost every byte value occurs many
    // times in the basis, preferring the block after the previous match keeps
    // the copy contiguous.
    EXPECT_EQ(result.summary.copies, 1u);
  }
}

TEST(DeltaEncoderTest, UnrelatedFilesAreAllLiteral) {
  const Bytes basis = test::random_bytes(50000, 63);
  const Bytes target = test::random_bytes(60000, 64);
  const Checked result = encode_and_check(basis, target, 512);
  EXPECT_EQ(result.delta.stats.matched_bytes, 0u);
  EXPECT_EQ(result.delta.stats.literal_bytes, target.size());
  EXPECT_EQ(result.summary.copies, 0u);
  // The delta is the target plus a small framing overhead.
  EXPECT_LT(result.delta.bytes.size(), target.size() + 200);
}

// SHA-256 is the expensive check, so it runs only at window positions whose
// weak checksum hits a block of the same length, at most once per position.
TEST(DeltaEncoderTest, StrongHashRunsOnlyOnWeakHits) {
  const std::size_t size = 1 << 20;
  const Bytes basis = test::random_bytes(size, 91);

  // Unrelated data: the weak lookup misses at nearly all of the ~1M
  // positions, so the strong hash almost never runs.
  const DeltaStats unrelated = encode_and_check(basis, test::random_bytes(size, 92), 1024).delta.stats;
  EXPECT_EQ(unrelated.strong_hashes, unrelated.weak_hits);
  EXPECT_EQ(unrelated.false_matches, unrelated.weak_hits);
  EXPECT_LT(unrelated.strong_hashes, size / 10000);

  // Identical data: one strong hash per block, each confirming a match.
  const DeltaStats same = encode_and_check(basis, basis, 1024).delta.stats;
  EXPECT_EQ(same.strong_hashes, size / 1024);
  EXPECT_EQ(same.weak_hits, size / 1024);
  EXPECT_EQ(same.false_matches, 0u);
}

TEST(DeltaEncoderTest, EmptyBasisSendsEverythingAsLiteral) {
  const Bytes target = test::random_bytes(5000, 65);
  const Checked result = encode_and_check(Bytes(), target, 1024);
  EXPECT_EQ(result.delta.stats.literal_bytes, target.size());
  EXPECT_EQ(result.delta.stats.weak_hits, 0u);
  EXPECT_EQ(result.records.front().header.basis_size, 0u);
}

TEST(DeltaEncoderTest, EmptyTargetIsHeaderAndEnd) {
  const Checked result = encode_and_check(test::random_bytes(5000, 66), Bytes(), 1024);
  ASSERT_EQ(result.records.size(), 2u);
  EXPECT_EQ(result.records[1].kind, Kind::kEnd);
  EXPECT_EQ(result.records[1].length, 0u);
  EXPECT_EQ(to_hex(result.records[1].digest),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(DeltaEncoderTest, BothEmpty) {
  const Checked result = encode_and_check(Bytes(), Bytes(), 1024);
  EXPECT_EQ(result.records.size(), 2u);
  EXPECT_EQ(result.delta.stats.target_bytes, 0u);
}

TEST(DeltaEncoderTest, TargetSmallerThanOneBlock) {
  const Bytes basis = test::random_bytes(10000, 67);
  // Even bytes copied from the basis cannot match: no block is that short.
  const Checked prefix = encode_and_check(basis, slice(basis, 0, 100), 4096);
  EXPECT_EQ(prefix.delta.stats.literal_bytes, 100u);
  const Checked other = encode_and_check(basis, test::random_bytes(4095, 68), 4096);
  EXPECT_EQ(other.delta.stats.literal_bytes, 4095u);
}

TEST(DeltaEncoderTest, BasisSmallerThanOneBlock) {
  const Bytes basis = test::random_bytes(100, 69);
  const Checked same = encode_and_check(basis, basis, 4096);
  EXPECT_EQ(same.delta.stats.matched_bytes, 100u);
  EXPECT_EQ(same.summary.copies, 1u);

  // The single short block still matches when it ends the target...
  const Checked prepended = encode_and_check(basis, concat({test::random_bytes(3, 70), basis}), 4096);
  EXPECT_EQ(prepended.delta.stats.matched_bytes, 100u);
  EXPECT_EQ(prepended.delta.stats.literal_bytes, 3u);

  // ...but not anywhere else.
  const Checked appended = encode_and_check(basis, concat({basis, test::random_bytes(3, 71)}), 4096);
  EXPECT_EQ(appended.delta.stats.matched_bytes, 0u);
}

TEST(DeltaEncoderTest, LastPartialBlockMatchesAtTheEnd) {
  // 3 full blocks of 1000 plus a 500-byte final block.
  const Bytes basis = test::random_bytes(3500, 72);
  const Bytes target = concat({test::random_bytes(77, 73), basis});
  const Checked result = encode_and_check(basis, target, 1000);
  EXPECT_EQ(result.delta.stats.matched_bytes, 3500u);
  EXPECT_EQ(result.delta.stats.literal_bytes, 77u);
  ASSERT_EQ(result.summary.copies, 1u);

  // Moving the short block away from the end makes it unmatchable, while
  // the full blocks still match after it.
  const Bytes moved = concat({slice(basis, 3000, 500), slice(basis, 0, 3000)});
  const Checked moved_result = encode_and_check(basis, moved, 1000);
  EXPECT_EQ(moved_result.delta.stats.matched_bytes, 3000u);
  EXPECT_EQ(moved_result.delta.stats.literal_bytes, 500u);
}

TEST(DeltaEncoderTest, TailShorterThanABlockAfterAFullWindow) {
  // The target ends with a full window that misses, followed by the final
  // short block: the short block must still be found.
  const Bytes basis = test::random_bytes(2300, 74);  // blocks: 1000, 1000, 300
  const Bytes target = concat({test::random_bytes(1000, 75), slice(basis, 2000, 300)});
  const Checked result = encode_and_check(basis, target, 1000);
  EXPECT_EQ(result.delta.stats.matched_bytes, 300u);
  EXPECT_EQ(result.delta.stats.literal_bytes, 1000u);
}

TEST(DeltaEncoderTest, ZeroFilledDataCollapsesToCopies) {
  const Bytes basis(64 * 1024, 0);
  const Bytes target(100 * 1024, 0);
  const Checked result = encode_and_check(basis, target, 4096);
  EXPECT_EQ(result.delta.stats.literal_bytes, 0u);
  // The basis's 16 identical blocks are copied as one run, then the run
  // restarts from block 0 for the remaining 36 KiB.
  EXPECT_EQ(result.summary.copies, 2u);
  EXPECT_LT(result.delta.bytes.size(), 120u);
}

TEST(DeltaEncoderTest, BinaryDataWithZerosAndRuns) {
  test::Rng rng(76);
  Bytes basis;
  while (basis.size() < 50000) {
    const auto run = rng.between(1, 2000);
    switch (rng.below(3)) {
      case 0:
        basis.insert(basis.end(), run, 0);
        break;
      case 1:
        basis.insert(basis.end(), run, 0xff);
        break;
      default: {
        const Bytes noise = test::random_bytes(run, rng.next());
        basis.insert(basis.end(), noise.begin(), noise.end());
      }
    }
  }
  Bytes target = basis;
  target.erase(target.begin() + 1000, target.begin() + 1500);
  target.insert(target.begin() + 20000, 3000, 0);
  for (std::size_t i = 30000; i < 30100; ++i) target[i] = 0;
  for (const std::uint32_t block_size : {16u, 333u, 2048u}) {
    SCOPED_TRACE("block size " + std::to_string(block_size));
    const Checked result = encode_and_check(basis, target, block_size);
    EXPECT_GT(result.delta.stats.matched_bytes, target.size() / 2);
  }
}

// Builds a block with the same weak checksum but different contents, using
// the (+1, -2, +1) pattern that leaves both rolling sums unchanged.
Bytes weak_twin(Bytes block, std::size_t at) {
  block[at] = static_cast<std::uint8_t>(block[at] + 1);
  block[at + 1] = static_cast<std::uint8_t>(block[at + 1] - 2);
  block[at + 2] = static_cast<std::uint8_t>(block[at + 2] + 1);
  return block;
}

TEST(DeltaEncoderTest, StrongHashRejectsAWeakOnlyMatch) {
  const std::uint32_t block_size = 512;
  Bytes basis = test::random_bytes(4 * block_size, 77);
  // Keep the perturbed bytes away from 0 and 255 so the twin is exact.
  for (std::size_t i = 0; i < 3; ++i) basis[block_size + 100 + i] = 100;
  const Bytes block1 = slice(basis, block_size, block_size);
  const Bytes twin = weak_twin(block1, 100);
  ASSERT_NE(twin, block1);
  ASSERT_EQ(RollingChecksum::compute(twin.data(), twin.size()),
            RollingChecksum::compute(block1.data(), block1.size()));

  const Bytes target = concat({slice(basis, 0, block_size), twin,
                               slice(basis, 2 * block_size, 2 * block_size)});
  const Checked result = encode_and_check(basis, target, block_size);
  // The twin passed the weak check, failed the strong one, and was sent as
  // literal data; everything around it was matched.
  EXPECT_GE(result.delta.stats.false_matches, 1u);
  EXPECT_GE(result.delta.stats.weak_hits, 4u);
  EXPECT_EQ(result.delta.stats.matched_bytes, 3u * block_size);
  EXPECT_EQ(result.delta.stats.literal_bytes, block_size);
}

TEST(DeltaEncoderTest, ConstantRunCostsOneStrongHashAgainstACraftedBlock) {
  // A basis block crafted to share the weak checksum of a run of one
  // repeated byte. Every window inside the run is a weak hit that the strong
  // hash rejects; the contents never change while sliding, so one SHA-256
  // answers for the whole run.
  const std::uint32_t block_size = 1024;
  const Bytes run_block(block_size, 100);
  const Bytes crafted = weak_twin(run_block, 10);
  ASSERT_EQ(RollingChecksum::compute(crafted.data(), crafted.size()),
            RollingChecksum::compute(run_block.data(), run_block.size()));
  const Bytes basis = concat({test::random_bytes(block_size, 91), crafted});
  const Bytes target = Bytes(1 << 20, 100);

  const Checked result = encode_and_check(basis, target, block_size);
  const DeltaStats& stats = result.delta.stats;
  EXPECT_EQ(stats.literal_bytes, target.size());
  EXPECT_EQ(stats.weak_hits, target.size() - block_size + 1);
  EXPECT_EQ(stats.false_matches, stats.weak_hits);
  EXPECT_EQ(stats.strong_hashes, 1u);
}

TEST(DeltaEncoderTest, KnownMissIsForgottenWhenTheWindowChanges) {
  // Runs of the crafted weak value separated by other bytes: each run is
  // hashed once, and the genuine block after them is still found.
  const std::uint32_t block_size = 256;
  const Bytes run_block(block_size, 100);
  const Bytes crafted = weak_twin(run_block, 7);
  const Bytes other = test::random_bytes(4 * block_size, 92);
  const Bytes basis = concat({crafted, other});
  Bytes target;
  for (int run = 0; run < 3; ++run) {
    target.insert(target.end(), 3 * block_size, 100);
    target.push_back(7);
  }
  target.insert(target.end(), other.begin(), other.end());

  const Checked result = encode_and_check(basis, target, block_size);
  EXPECT_EQ(result.delta.stats.strong_hashes, 3u + 4u);
  EXPECT_EQ(result.delta.stats.matched_bytes, other.size());
}

TEST(DeltaEncoderTest, RepeatedFalseAlarmsOnOneWeakValueAreCapped) {
  // A two-byte period changes the window at every slide, so the known-miss
  // shortcut does not apply; the per-value cap bounds the hashing instead.
  const std::uint32_t block_size = 512;
  Bytes period_window(block_size);
  for (std::size_t i = 0; i < block_size; ++i) period_window[i] = i % 2 == 0 ? 100 : 150;
  const Bytes crafted = weak_twin(period_window, 20);
  ASSERT_EQ(RollingChecksum::compute(crafted.data(), crafted.size()),
            RollingChecksum::compute(period_window.data(), period_window.size()));
  Bytes target(1 << 20);
  for (std::size_t i = 0; i < target.size(); ++i) target[i] = i % 2 == 0 ? 100 : 150;

  const Checked result = encode_and_check(crafted, target, block_size);
  const DeltaStats& stats = result.delta.stats;
  EXPECT_EQ(stats.literal_bytes, target.size());
  EXPECT_GT(stats.false_matches, 100000u);
  EXPECT_EQ(stats.strong_hashes, 1024u);
}

TEST(DeltaEncoderTest, ManyBlocksSharingOneWeakChecksum) {
  // 200 distinct blocks with one weak checksum between them: block i is a
  // flat block with the (+k, -2k, +k) pattern at position i % 62, where
  // k = 1 + i / 62. The index keeps only the first 64 of them, so this
  // exercises the capped chains and many weak-only hits.
  const std::uint32_t block_size = 64;
  Bytes basis;
  for (std::size_t i = 0; i < 200; ++i) {
    Bytes block(block_size, 100);
    const std::size_t at = i % 62;
    const auto k = static_cast<std::uint8_t>(1 + i / 62);
    block[at] = static_cast<std::uint8_t>(block[at] + k);
    block[at + 1] = static_cast<std::uint8_t>(block[at + 1] - 2 * k);
    block[at + 2] = static_cast<std::uint8_t>(block[at + 2] + k);
    basis.insert(basis.end(), block.begin(), block.end());
  }
  ASSERT_EQ(RollingChecksum::compute(basis.data(), block_size),
            RollingChecksum::compute(basis.data() + 199 * block_size, block_size));

  // Identical data still matches completely: each match is found through
  // the preferred next block, not the capped chain.
  const Checked same = encode_and_check(basis, basis, block_size);
  EXPECT_EQ(same.delta.stats.literal_bytes, 0u);
  EXPECT_EQ(same.summary.copies, 1u);

  // After an insertion the blocks beyond the cap cannot be found again.
  // That costs delta size only; the output is still exact (checked inside
  // encode_and_check), and everything before the edit is matched.
  Bytes target = basis;
  target.insert(target.begin() + 5000, 10, 7);
  const Checked edited = encode_and_check(basis, target, block_size);
  EXPECT_GE(edited.delta.stats.matched_bytes, 5000u / block_size * block_size);
  EXPECT_GT(edited.delta.stats.false_matches, 0u);
}

TEST(DeltaEncoderTest, ReorderedBlocksBecomeTwoCopies) {
  const Bytes basis = test::random_bytes(4000, 78);
  const Bytes target = concat({slice(basis, 2000, 2000), slice(basis, 0, 2000)});
  const Checked result = encode_and_check(basis, target, 500);
  EXPECT_EQ(result.delta.stats.literal_bytes, 0u);
  ASSERT_EQ(result.summary.copies, 2u);
  EXPECT_EQ(result.records[1].offset, 2000u);
  EXPECT_EQ(result.records[1].length, 2000u);
  EXPECT_EQ(result.records[2].offset, 0u);
}

TEST(DeltaEncoderTest, ChunkBoundariesSplitCopies) {
  const Bytes basis = test::random_bytes(1280, 79);
  DeltaOptions options;
  options.chunk_size = 128;
  const Checked result = encode_and_check(basis, basis, 64, options);
  EXPECT_EQ(result.summary.chunks, 10u);
  EXPECT_EQ(result.summary.copies, 10u);
  for (const Record& record : result.records) {
    if (record.kind == Kind::kCopy) {
      EXPECT_EQ(record.length, 128u);
    }
  }
}

TEST(DeltaEncoderTest, ChunkBoundariesSplitLiterals) {
  DeltaOptions options;
  options.chunk_size = 128;
  const Checked result = encode_and_check(Bytes(), test::random_bytes(320, 80), 64, options);
  std::vector<Kind> kinds;
  std::vector<std::size_t> literal_sizes;
  for (const Record& record : result.records) {
    kinds.push_back(record.kind);
    if (record.kind == Kind::kLiteral) literal_sizes.push_back(record.data.size());
  }
  EXPECT_EQ(kinds, (std::vector<Kind>{Kind::kHeader, Kind::kLiteral, Kind::kChunk, Kind::kLiteral,
                                      Kind::kChunk, Kind::kLiteral, Kind::kChunk, Kind::kEnd}));
  EXPECT_EQ(literal_sizes, (std::vector<std::size_t>{128, 128, 64}));
}

TEST(DeltaEncoderTest, TinyChunksAndBlocksStillRoundTrip) {
  const Bytes basis = test::random_bytes(3000, 81);
  Bytes target = basis;
  target.insert(target.begin() + 1234, 17, 9);
  target.erase(target.begin() + 2000, target.begin() + 2100);
  for (const std::uint64_t chunk_size : {64ull, 128ull, 192ull, 1024ull}) {
    DeltaOptions options;
    options.chunk_size = chunk_size;
    SCOPED_TRACE("chunk size " + std::to_string(chunk_size));
    const Checked result = encode_and_check(basis, target, 50, options);
    EXPECT_EQ(result.summary.chunks, (target.size() + chunk_size - 1) / chunk_size);
  }
}

TEST(DeltaEncoderTest, FeedSplittingDoesNotChangeTheDelta) {
  const Bytes basis = test::random_bytes(40000, 82);
  Bytes target = basis;
  target.insert(target.begin() + 777, 50, 1);
  target.erase(target.begin() + 20000, target.begin() + 20333);
  std::copy_n(test::random_bytes(100, 83).begin(), 100, target.begin() + 30000);
  const Signature signature = test::make_signature(basis, 700);
  DeltaOptions options;
  options.chunk_size = 4096;
  options.max_literal = 300;
  const Bytes reference = test::encode(signature, target, options).bytes;
  for (const std::size_t piece : {1, 2, 7, 699, 700, 701, 4096, 100000}) {
    EXPECT_EQ(test::encode(signature, target, options, piece).bytes, reference) << piece;
  }
}

TEST(DeltaEncoderTest, LiteralRecordsRespectMaxLiteral) {
  const Bytes basis = test::random_bytes(10000, 84);
  const Bytes target = test::random_bytes(300 * 1024, 85);
  for (const std::size_t max_literal : {std::size_t{1000}, kDefaultMaxLiteral}) {
    DeltaOptions options;
    options.max_literal = max_literal;
    const Checked result = encode_and_check(basis, target, 1024, options);
    for (const Record& record : result.records) {
      if (record.kind == Kind::kLiteral) {
        EXPECT_LE(record.data.size(), max_literal);
      }
    }
    EXPECT_EQ(result.summary.literals, (target.size() + max_literal - 1) / max_literal);
  }
}

TEST(DeltaEncoderTest, BufferStaysTheSameSize) {
  const Bytes basis = test::random_bytes(1 << 20, 86);
  Bytes target = basis;
  for (std::size_t i = 0; i < target.size(); i += 100000) target[i] ^= 0x55;
  const Signature signature = test::make_signature(basis, 2048);
  test::HashingSink bytes;
  DeltaWriter writer(bytes);
  DeltaOptions options;
  DeltaEncoder encoder(signature, writer, options);
  const std::size_t capacity = encoder.buffer_capacity();
  EXPECT_EQ(capacity, 2 * (options.max_literal + 2048));
  for (int pass = 0; pass < 4; ++pass) {
    for (std::size_t offset = 0; offset < target.size(); offset += 65536) {
      encoder.feed(target.data() + offset, 65536);
    }
  }
  encoder.finish();
  EXPECT_EQ(encoder.buffer_capacity(), capacity);
  EXPECT_EQ(encoder.stats().target_bytes, 4u * target.size());
}

TEST(DeltaEncoderTest, RejectsInvalidArguments) {
  const Signature signature = test::make_signature(test::random_bytes(100, 87), 10);
  test::RecordingSink sink;
  DeltaOptions zero_chunk;
  zero_chunk.chunk_size = 0;
  EXPECT_THROW({ DeltaEncoder encoder(signature, sink, zero_chunk); }, std::invalid_argument);
  DeltaOptions big_chunk;
  big_chunk.chunk_size = kMaxChunkSize + 1;
  EXPECT_THROW({ DeltaEncoder encoder(signature, sink, big_chunk); }, std::invalid_argument);
  const std::vector<std::uint64_t> unaligned_sizes = {1, 100, 1000, kDefaultChunkSize + 1};
  for (const std::uint64_t unaligned : unaligned_sizes) {
    DeltaOptions options;
    options.chunk_size = unaligned;
    EXPECT_THROW({ DeltaEncoder encoder(signature, sink, options); }, std::invalid_argument)
        << unaligned;
  }
  DeltaOptions zero_literal;
  zero_literal.max_literal = 0;
  EXPECT_THROW({ DeltaEncoder encoder(signature, sink, zero_literal); }, std::invalid_argument);

  SignatureHeader header;
  header.block_size = 10;
  header.basis_size = 100;
  const Signature incomplete(header);
  EXPECT_THROW({ DeltaEncoder encoder(incomplete, sink); }, std::invalid_argument);
  const Signature empty;
  EXPECT_THROW({ DeltaEncoder encoder(empty, sink); }, FormatError);
  // Nothing reaches the sink when construction fails.
  EXPECT_TRUE(sink.records.empty());

  DeltaEncoder encoder(signature, sink);
  encoder.finish();
  const std::uint8_t byte = 0;
  EXPECT_THROW(encoder.feed(&byte, 1), std::logic_error);
  EXPECT_THROW(encoder.finish(), std::logic_error);
}

TEST(DeltaEncoderTest, ResumesFromAChunkBoundary) {
  const Bytes basis = test::random_bytes(10000, 88);
  Bytes target = basis;
  target.insert(target.begin() + 6000, 123, 4);
  const Signature signature = test::make_signature(basis, 256);
  DeltaOptions options;
  options.chunk_size = 1024;
  const std::size_t resume_at = 3072;

  Sha256 prefix;
  prefix.update(target.data(), resume_at);

  MemorySink delta;
  DeltaWriter writer(delta);
  DeltaEncoder encoder(signature, writer, options, prefix);
  encoder.feed(target.data() + resume_at, target.size() - resume_at);
  encoder.finish();

  const std::vector<Record> records = decode_records(delta.data());
  EXPECT_EQ(records.front().header.start_offset, resume_at);
  expect_digests_match_target(records, target, resume_at);

  // The receiving side continues from the same prefix state.
  MemoryRandomAccess basis_source(basis);
  MemorySink suffix;
  Patcher patcher(basis_source, suffix, prefix);
  DeltaDecoder decoder(patcher);
  decoder.feed(delta.data().data(), delta.data().size());
  decoder.finish();
  EXPECT_TRUE(patcher.finished());
  EXPECT_TRUE(test::same_bytes(slice(target, resume_at, target.size() - resume_at), suffix.data()));
  EXPECT_EQ(patcher.position(), target.size());
}

TEST(DeltaEncoderTest, ResumeOffsetMustBeChunkAligned) {
  const Signature signature = test::make_signature(test::random_bytes(100, 89), 10);
  test::RecordingSink sink;
  DeltaOptions options;
  options.chunk_size = 1024;
  Sha256 prefix;
  const Bytes some(2560, 1);
  prefix.update(some.data(), some.size());
  EXPECT_THROW({ DeltaEncoder encoder(signature, sink, options, prefix); },
               std::invalid_argument);
}

TEST(DeltaEncoderTest, StrongLengthVariantsRoundTrip) {
  const Bytes basis = test::random_bytes(20000, 90);
  Bytes target = basis;
  target.erase(target.begin() + 100, target.begin() + 200);
  for (const std::uint32_t strong_length : {kMinStrongLength, 24u, kMaxStrongLength}) {
    const Signature signature = test::make_signature(basis, 512, strong_length);
    const test::EncodedDelta delta = test::encode(signature, target);
    EXPECT_TRUE(test::same_bytes(target, test::apply(basis, delta.bytes)));
    EXPECT_GT(delta.stats.matched_bytes, 19000u);
  }
}

}  // namespace
}  // namespace deltasync
