#include "block_index.h"

#include <cstring>

namespace deltasync {

BlockIndex::BlockIndex(const Signature& signature) {
  const std::uint32_t count = signature.block_count();

  // At least twice as many slots as blocks keeps the load factor <= 1/2.
  unsigned bits = 1;
  while ((std::uint64_t{1} << bits) < std::uint64_t{count} * 2) ++bits;
  slots_.assign(std::size_t{1} << bits, Slot{0, kNone});
  mask_ = slots_.size() - 1;
  shift_ = 64 - bits;
  next_.assign(count, kNone);

  const std::uint32_t strong_length = signature.strong_length();
  for (std::uint32_t block = 0; block < count; ++block) {
    const std::uint32_t weak = signature.weak(block);
    std::size_t slot = slot_for(weak);
    while (slots_[slot].head != kNone && slots_[slot].weak != weak) slot = (slot + 1) & mask_;
    if (slots_[slot].head == kNone) {
      slots_[slot] = Slot{weak, block};
      ++indexed_;
      continue;
    }

    std::uint32_t tail = slots_[slot].head;
    unsigned chain_length = 1;
    bool duplicate = false;
    for (;;) {
      if (signature.block_length(tail) == signature.block_length(block) &&
          std::memcmp(signature.strong(tail), signature.strong(block), strong_length) == 0) {
        duplicate = true;
        break;
      }
      if (next_[tail] == kNone) break;
      tail = next_[tail];
      ++chain_length;
    }
    if (!duplicate && chain_length < kMaxChainLength) {
      next_[tail] = block;
      ++indexed_;
    }
  }
}

}  // namespace deltasync
