#include "deltasync/delta_encoder.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "block_index.h"
#include "deltasync/error.h"
#include "deltasync/rolling_checksum.h"

namespace deltasync {
namespace {

// See DeltaEncoder::Impl::false_alarm_count_.
constexpr std::uint32_t kMaxFalseAlarms = 1024;

const Signature& require_complete(const Signature& signature) {
  validate(signature.header());
  if (!signature.complete()) {
    throw std::invalid_argument("DeltaEncoder needs a complete signature");
  }
  return signature;
}

}  // namespace

class DeltaEncoder::Impl {
 public:
  Impl(const Signature& signature, DeltaSink& sink, const DeltaOptions& options,
       const Sha256& prefix);

  void feed(const std::uint8_t* data, std::size_t length);
  void finish();

  const DeltaStats& stats() const { return stats_; }
  std::size_t buffer_capacity() const { return buffer_.size(); }

 private:
  // Input side: finding matches in the buffered target.
  void scan();
  void scan_tail();
  bool try_match_window();
  bool find_match(const std::uint8_t* window, std::size_t length, std::uint32_t weak,
                  std::uint32_t* found);
  void flush_literal(std::size_t end);
  void emit_match(std::uint32_t block, const std::uint8_t* data, std::size_t length);
  void compact();

  // Output side: turning matches and literals into chunked records.
  void emit_literal(const std::uint8_t* data, std::size_t length);
  void emit_copy(std::uint64_t offset, std::uint64_t length, const std::uint8_t* data);
  void flush_copy();
  void end_chunk();
  std::uint64_t chunk_room() const { return chunk_size_ - (output_position_ - chunk_start_); }

  const Signature& signature_;
  const BlockIndex index_;
  DeltaSink& sink_;
  const std::size_t block_size_;
  const std::uint32_t block_count_;
  const std::uint32_t strong_length_;
  const std::uint64_t chunk_size_;
  const std::size_t max_literal_;
  // The final basis block when it is shorter than block_size_, else kNone.
  std::uint32_t short_block_ = BlockIndex::kNone;

  // Holds target bytes [literal_start_, data_end_). Bytes before
  // window_start_ are unmatched and waiting to be sent as a literal; the
  // current window is [window_start_, window_start_ + block_size_).
  std::vector<std::uint8_t> buffer_;
  std::size_t literal_start_ = 0;
  std::size_t window_start_ = 0;
  std::size_t data_end_ = 0;
  RollingChecksum rolling_;
  bool window_valid_ = false;    // rolling_ covers a full window at window_start_
  bool window_checked_ = false;  // ...and that window has already missed
  // The current window is a run of one repeated byte whose weak checksum hit
  // a block but whose strong hash matched nothing. Sliding in another copy of
  // that byte leaves the contents unchanged, so the rest of the run costs no
  // further SHA-256, even if a basis block was crafted to share the run's
  // weak checksum. (Only a constant window qualifies: for any other window,
  // equal outgoing and incoming bytes still shift the contents.)
  bool known_miss_ = false;
  // Weak hits of the same weak value rejected by the strong hash since the
  // last match. Past kMaxFalseAlarms that value is not hashed again until
  // something matches, which bounds the hashing a repeating (but not
  // constant) pattern can force; the price is at most a larger delta.
  std::uint32_t false_alarm_weak_ = 0;
  std::uint32_t false_alarm_count_ = 0;
  std::uint32_t last_match_ = BlockIndex::kNone;

  Sha256 chunk_hash_;
  Sha256 file_hash_;
  std::uint64_t output_position_;
  std::uint64_t chunk_start_;
  std::uint64_t pending_copy_offset_ = 0;
  std::uint64_t pending_copy_length_ = 0;

  bool finished_ = false;
  DeltaStats stats_;
};

DeltaEncoder::Impl::Impl(const Signature& signature, DeltaSink& sink,
                         const DeltaOptions& options, const Sha256& prefix)
    : signature_(require_complete(signature)),
      index_(signature),
      sink_(sink),
      block_size_(signature.block_size()),
      block_count_(signature.block_count()),
      strong_length_(signature.strong_length()),
      chunk_size_(options.chunk_size),
      max_literal_(options.max_literal),
      file_hash_(prefix),
      output_position_(prefix.length()),
      chunk_start_(prefix.length()) {
  if (chunk_size_ == 0 || chunk_size_ > kMaxChunkSize) {
    throw std::invalid_argument("chunk size " + std::to_string(chunk_size_) +
                                " is outside [1, " + std::to_string(kMaxChunkSize) + "]");
  }
  // Chunk boundaries are the points a transfer can resume from, and resuming
  // restores the whole-file hash from a Sha256::Midstate, which only exists
  // at multiples of the SHA-256 block length.
  if (chunk_size_ % Sha256::kBlockLength != 0) {
    throw std::invalid_argument("chunk size " + std::to_string(chunk_size_) +
                                " is not a multiple of " + std::to_string(Sha256::kBlockLength));
  }
  if (output_position_ % chunk_size_ != 0) {
    throw std::invalid_argument("a delta can only start at a multiple of the chunk size");
  }
  if (max_literal_ == 0 || max_literal_ > kMaxChunkSize) {
    throw std::invalid_argument("max_literal " + std::to_string(max_literal_) +
                                " is outside [1, " + std::to_string(kMaxChunkSize) + "]");
  }
  if (block_count_ > 0 && signature.block_length(block_count_ - 1) < block_size_) {
    short_block_ = block_count_ - 1;
  }
  // After every scan() the buffer holds less than max_literal_ pending
  // bytes plus at most one window, so twice that guarantees each
  // compaction frees at least as much space as it moves.
  buffer_.resize(2 * (max_literal_ + block_size_));

  DeltaHeader header;
  header.chunk_size = chunk_size_;
  header.basis_size = signature.header().basis_size;
  header.start_offset = output_position_;
  sink_.on_header(header);
}

void DeltaEncoder::Impl::feed(const std::uint8_t* data, std::size_t length) {
  if (finished_) throw std::logic_error("DeltaEncoder::feed after finish");
  stats_.target_bytes += length;
  while (length > 0) {
    if (data_end_ == buffer_.size()) compact();
    const std::size_t take = std::min(length, buffer_.size() - data_end_);
    std::memcpy(buffer_.data() + data_end_, data, take);
    data_end_ += take;
    data += take;
    length -= take;
    scan();
  }
}

void DeltaEncoder::Impl::finish() {
  if (finished_) throw std::logic_error("DeltaEncoder::finish called twice");
  finished_ = true;
  scan_tail();
  flush_literal(data_end_);
  flush_copy();
  if (output_position_ > chunk_start_) end_chunk();
  sink_.on_end(output_position_, file_hash_.digest());
}

void DeltaEncoder::Impl::scan() {
  if (block_count_ == 0) {
    // An empty basis has nothing to match: everything is literal.
    window_start_ = data_end_;
    while (data_end_ - literal_start_ >= max_literal_) flush_literal(literal_start_ + max_literal_);
    return;
  }
  for (;;) {
    if (!window_valid_) {
      if (data_end_ - window_start_ < block_size_) return;
      rolling_.reset();
      rolling_.update(buffer_.data() + window_start_, block_size_);
      window_valid_ = true;
      window_checked_ = false;
      known_miss_ = false;
    }
    if (!window_checked_) {
      if (try_match_window()) continue;
      window_checked_ = true;
    }
    // Slide one byte, which needs the byte just past the window.
    if (window_start_ + block_size_ >= data_end_) return;
    const std::uint8_t outgoing = buffer_[window_start_];
    const std::uint8_t incoming = buffer_[window_start_ + block_size_];
    if (outgoing != incoming) known_miss_ = false;
    rolling_.rotate(outgoing, incoming);
    ++window_start_;
    window_checked_ = false;
    if (window_start_ - literal_start_ >= max_literal_) flush_literal(window_start_);
  }
}

void DeltaEncoder::Impl::scan_tail() {
  // Every position with a full window has been tried. A shorter window can
  // only match a short final basis block, and only where the window ends at
  // the end of the target, so there is exactly one place to look.
  if (short_block_ == BlockIndex::kNone) return;
  const std::size_t length = signature_.block_length(short_block_);
  const std::size_t first_short_window = window_valid_ ? window_start_ + 1 : window_start_;
  if (data_end_ - first_short_window < length) return;

  const std::size_t start = data_end_ - length;
  const std::uint8_t* window = buffer_.data() + start;
  if (RollingChecksum::compute(window, length) != signature_.weak(short_block_)) return;
  ++stats_.weak_hits;
  ++stats_.strong_hashes;
  const Sha256Digest strong = Sha256::hash(window, length);
  if (std::memcmp(strong.data(), signature_.strong(short_block_), strong_length_) != 0) {
    ++stats_.false_matches;
    return;
  }
  flush_literal(start);
  emit_match(short_block_, window, length);
  literal_start_ = data_end_;
  window_start_ = data_end_;
  window_valid_ = false;
}

bool DeltaEncoder::Impl::try_match_window() {
  const std::uint8_t* window = buffer_.data() + window_start_;
  std::uint32_t block = BlockIndex::kNone;
  if (!find_match(window, block_size_, rolling_.digest(), &block)) return false;
  flush_literal(window_start_);
  emit_match(block, window, block_size_);
  window_start_ += block_size_;
  literal_start_ = window_start_;
  window_valid_ = false;
  return true;
}

bool DeltaEncoder::Impl::find_match(const std::uint8_t* window, std::size_t length,
                                    std::uint32_t weak, std::uint32_t* found) {
  // Every weak checksum present in the basis has a slot in the index, so a
  // miss here (the common case) rules out all blocks, including the
  // preferred one below.
  const std::uint32_t head = index_.find(weak);
  if (head == BlockIndex::kNone) return false;
  if (known_miss_ || (false_alarm_count_ >= kMaxFalseAlarms && weak == false_alarm_weak_)) {
    // Same answer as last time (see known_miss_), or a value that keeps
    // failing; counted as a rejected hit without hashing again.
    ++stats_.weak_hits;
    ++stats_.false_matches;
    return false;
  }

  Sha256Digest strong{};
  bool have_strong = false;
  bool weak_hit = false;
  const auto confirms = [&](std::uint32_t block) {
    if (signature_.block_length(block) != length) return false;
    weak_hit = true;
    if (!have_strong) {
      strong = Sha256::hash(window, length);
      have_strong = true;
      ++stats_.strong_hashes;
    }
    return std::memcmp(strong.data(), signature_.strong(block), strong_length_) == 0;
  };

  // Try the block after the previous match first. When the basis contains
  // repeated blocks this keeps consecutive matches contiguous in the basis,
  // so they coalesce into a single COPY.
  std::uint32_t match = BlockIndex::kNone;
  if (last_match_ != BlockIndex::kNone && last_match_ + 1 < block_count_ &&
      signature_.weak(last_match_ + 1) == weak && confirms(last_match_ + 1)) {
    match = last_match_ + 1;
  }
  for (std::uint32_t block = head; match == BlockIndex::kNone && block != BlockIndex::kNone;
       block = index_.next(block)) {
    if (confirms(block)) match = block;
  }

  if (weak_hit) ++stats_.weak_hits;
  if (match == BlockIndex::kNone) {
    if (weak_hit) {
      ++stats_.false_matches;
      known_miss_ = std::memcmp(window, window + 1, length - 1) == 0;
      if (weak != false_alarm_weak_) {
        false_alarm_weak_ = weak;
        false_alarm_count_ = 0;
      }
      ++false_alarm_count_;
    }
    return false;
  }
  false_alarm_count_ = 0;
  *found = match;
  return true;
}

void DeltaEncoder::Impl::flush_literal(std::size_t end) {
  if (end <= literal_start_) return;
  const std::size_t length = end - literal_start_;
  stats_.literal_bytes += length;
  emit_literal(buffer_.data() + literal_start_, length);
  literal_start_ = end;
}

void DeltaEncoder::Impl::emit_match(std::uint32_t block, const std::uint8_t* data,
                                    std::size_t length) {
  last_match_ = block;
  stats_.matched_bytes += length;
  emit_copy(std::uint64_t{block} * block_size_, length, data);
}

void DeltaEncoder::Impl::compact() {
  // Everything before literal_start_ has already been emitted.
  const std::size_t keep = data_end_ - literal_start_;
  std::memmove(buffer_.data(), buffer_.data() + literal_start_, keep);
  window_start_ -= literal_start_;
  data_end_ = keep;
  literal_start_ = 0;
}

void DeltaEncoder::Impl::emit_literal(const std::uint8_t* data, std::size_t length) {
  flush_copy();
  while (length > 0) {
    const auto take = static_cast<std::size_t>(
        std::min({std::uint64_t{length}, chunk_room(), std::uint64_t{max_literal_}}));
    chunk_hash_.update(data, take);
    file_hash_.update(data, take);
    output_position_ += take;
    ++stats_.literal_ops;
    sink_.on_literal(data, take);
    data += take;
    length -= take;
    if (chunk_room() == 0) end_chunk();
  }
}

void DeltaEncoder::Impl::emit_copy(std::uint64_t offset, std::uint64_t length,
                                   const std::uint8_t* data) {
  while (length > 0) {
    const std::uint64_t take = std::min(length, chunk_room());
    if (pending_copy_length_ > 0 && pending_copy_offset_ + pending_copy_length_ == offset) {
      pending_copy_length_ += take;
    } else {
      flush_copy();
      pending_copy_offset_ = offset;
      pending_copy_length_ = take;
    }
    // The matched target bytes are hashed now, while they are still in the
    // buffer; the COPY record itself may be sent later after coalescing.
    chunk_hash_.update(data, static_cast<std::size_t>(take));
    file_hash_.update(data, static_cast<std::size_t>(take));
    output_position_ += take;
    offset += take;
    data += take;
    length -= take;
    if (chunk_room() == 0) {
      flush_copy();
      end_chunk();
    }
  }
}

void DeltaEncoder::Impl::flush_copy() {
  if (pending_copy_length_ == 0) return;
  const std::uint64_t offset = pending_copy_offset_;
  const std::uint64_t length = pending_copy_length_;
  pending_copy_length_ = 0;
  ++stats_.copy_ops;
  sink_.on_copy(offset, length);
}

void DeltaEncoder::Impl::end_chunk() {
  const Sha256Digest digest = chunk_hash_.digest();
  chunk_hash_.reset();
  chunk_start_ = output_position_;
  ++stats_.chunks;
  sink_.on_chunk(digest);
}

DeltaEncoder::DeltaEncoder(const Signature& signature, DeltaSink& sink,
                           const DeltaOptions& options)
    : impl_(std::make_unique<Impl>(signature, sink, options, Sha256())) {}

DeltaEncoder::DeltaEncoder(const Signature& signature, DeltaSink& sink,
                           const DeltaOptions& options, const Sha256& prefix)
    : impl_(std::make_unique<Impl>(signature, sink, options, prefix)) {}

DeltaEncoder::~DeltaEncoder() = default;

void DeltaEncoder::feed(const std::uint8_t* data, std::size_t length) {
  impl_->feed(data, length);
}

void DeltaEncoder::finish() { impl_->finish(); }

const DeltaStats& DeltaEncoder::stats() const { return impl_->stats(); }

std::size_t DeltaEncoder::buffer_capacity() const { return impl_->buffer_capacity(); }

DeltaStats encode_delta(const Signature& signature, ByteSource& target, DeltaSink& sink,
                        const DeltaOptions& options) {
  DeltaEncoder encoder(signature, sink, options);
  std::vector<std::uint8_t> buffer(kIoBufferSize);
  for (;;) {
    const std::size_t count = target.read(buffer.data(), buffer.size());
    if (count == 0) break;
    encoder.feed(buffer.data(), count);
  }
  encoder.finish();
  return encoder.stats();
}

}  // namespace deltasync
