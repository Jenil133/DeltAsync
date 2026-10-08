// Verifies that the streaming paths do no heap allocation once set up.
// The global allocation functions are replaced with counting versions, so
// this file is built into its own test binary (and skipped under the
// sanitizers, which install their own allocators).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

#include "deltasync/delta_encoder.h"
#include "deltasync/delta_format.h"
#include "deltasync/patch.h"
#include "deltasync/signature.h"
#include "test_util.h"

namespace {

std::atomic<std::size_t> g_allocations{0};

void* counted_allocation(std::size_t size) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}

}  // namespace

void* operator new(std::size_t size) { return counted_allocation(size); }
void* operator new[](std::size_t size) { return counted_allocation(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace deltasync {
namespace {

using test::Bytes;

std::size_t allocations() { return g_allocations.load(std::memory_order_relaxed); }

struct Inputs {
  Bytes basis;
  Bytes target;
};

// 8 MiB with edits scattered through it, so encoding exercises matches,
// literals, coalescing and chunk boundaries.
Inputs make_inputs() {
  Inputs inputs;
  inputs.basis = test::random_bytes(8 << 20, 701);
  inputs.target = inputs.basis;
  test::Rng rng(702);
  for (int edit = 0; edit < 40; ++edit) {
    const std::size_t at = rng.below(inputs.target.size() - 5000);
    rng.fill(inputs.target.data() + at, rng.between(1, 5000));
  }
  return inputs;
}

TEST(AllocationTest, SignatureGenerationDoesNotAllocatePerBlock) {
  const Inputs inputs = make_inputs();
  test::HashingSink out;
  SignatureWriter writer(out);
  SignatureHeader header;
  header.block_size = 2048;
  header.basis_size = inputs.basis.size();
  SignatureGenerator generator(header, writer);

  const std::size_t before = allocations();
  for (std::size_t offset = 0; offset < inputs.basis.size(); offset += 10000) {
    generator.feed(inputs.basis.data() + offset,
                   std::min<std::size_t>(10000, inputs.basis.size() - offset));
  }
  generator.finish();
  const std::size_t during = allocations() - before;
  EXPECT_EQ(during, 0u);
  EXPECT_EQ(writer.bytes_written(), out.bytes);
}

TEST(AllocationTest, EncodingDoesNotAllocateAfterConstruction) {
  const Inputs inputs = make_inputs();
  const Signature signature = test::make_signature(inputs.basis, 2048);
  test::HashingSink out;
  DeltaWriter writer(out);
  DeltaOptions options;
  options.chunk_size = 256 * 1024;
  DeltaEncoder encoder(signature, writer, options);

  const std::size_t before = allocations();
  for (std::size_t offset = 0; offset < inputs.target.size(); offset += 30000) {
    encoder.feed(inputs.target.data() + offset,
                 std::min<std::size_t>(30000, inputs.target.size() - offset));
  }
  encoder.finish();
  const std::size_t during = allocations() - before;
  EXPECT_EQ(during, 0u);
  EXPECT_GT(encoder.stats().matched_bytes, 0u);
  EXPECT_GT(encoder.stats().literal_bytes, 0u);
  EXPECT_EQ(encoder.stats().chunks, inputs.target.size() / options.chunk_size);
}

TEST(AllocationTest, DecodingAndPatchingDoNotAllocateAfterConstruction) {
  const Inputs inputs = make_inputs();
  DeltaOptions options;
  options.chunk_size = 256 * 1024;
  const Bytes delta =
      test::encode(test::make_signature(inputs.basis, 2048), inputs.target, options).bytes;

  MemoryRandomAccess basis(inputs.basis);
  test::HashingSink output;
  Patcher patcher(basis, output);
  DeltaDecoder decoder(patcher);

  const std::size_t before = allocations();
  for (std::size_t offset = 0; offset < delta.size(); offset += 4093) {
    decoder.feed(delta.data() + offset, std::min<std::size_t>(4093, delta.size() - offset));
  }
  decoder.finish();
  const std::size_t during = allocations() - before;
  EXPECT_EQ(during, 0u);
  EXPECT_TRUE(patcher.finished());
  EXPECT_EQ(output.hash.digest(), Sha256::hash(inputs.target.data(), inputs.target.size()));
}

// Sanity check that the counter sees allocations at all.
TEST(AllocationTest, CounterWorks) {
  const std::size_t before = allocations();
  // A direct call: unlike a new-expression, it cannot be optimized away.
  void* memory = ::operator new(16);
  EXPECT_EQ(allocations() - before, 1u);
  ::operator delete(memory);
}

}  // namespace
}  // namespace deltasync
