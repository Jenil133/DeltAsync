#ifndef DELTASYNC_SRC_BLOCK_INDEX_H_
#define DELTASYNC_SRC_BLOCK_INDEX_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "deltasync/signature.h"

namespace deltasync {

// Maps weak checksums to the basis blocks that have them.
//
// Open addressing with linear probing over a power-of-two table at most half
// full: a lookup that misses, which is what happens at almost every byte
// offset of unmatched data, usually ends at the first or second slot.
// Blocks sharing a weak checksum hang off the slot in a chain, in block
// order.
//
// Two rules keep the work per lookup bounded. A block whose length and
// strong checksum equal one already in its chain is left out: it could
// never match anything the earlier block would not (runs of identical
// blocks, such as zero-filled regions, then cost one candidate instead of
// thousands). And a chain stops growing at kMaxChainLength distinct blocks,
// which only matters when many different blocks collide on the weak
// checksum, e.g. in adversarial input; the cost is a possibly larger delta,
// never a wrong one.
class BlockIndex {
 public:
  static constexpr std::uint32_t kNone = 0xffffffff;
  static constexpr unsigned kMaxChainLength = 64;

  explicit BlockIndex(const Signature& signature);

  // First block whose weak checksum is `weak`, or kNone.
  std::uint32_t find(std::uint32_t weak) const {
    std::size_t slot = slot_for(weak);
    for (;;) {
      const Slot& entry = slots_[slot];
      if (entry.head == kNone) return kNone;
      if (entry.weak == weak) return entry.head;
      slot = (slot + 1) & mask_;
    }
  }

  // Next block in the same chain, or kNone.
  std::uint32_t next(std::uint32_t block) const { return next_[block]; }

  // Number of blocks reachable through find() and next().
  std::uint32_t indexed_blocks() const { return indexed_; }

 private:
  struct Slot {
    std::uint32_t weak;
    std::uint32_t head;
  };

  // Fibonacci hashing: the top bits of a multiplication by 2^64/phi mix all
  // bits of the weak checksum, whose low half (a plain byte sum) is poorly
  // distributed on its own.
  std::size_t slot_for(std::uint32_t weak) const {
    return static_cast<std::size_t>((std::uint64_t{weak} * 0x9e3779b97f4a7c15ull) >> shift_);
  }

  std::vector<Slot> slots_;
  std::vector<std::uint32_t> next_;
  std::size_t mask_ = 0;
  unsigned shift_ = 0;
  std::uint32_t indexed_ = 0;
};

}  // namespace deltasync

#endif  // DELTASYNC_SRC_BLOCK_INDEX_H_
