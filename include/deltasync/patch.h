#ifndef DELTASYNC_PATCH_H_
#define DELTASYNC_PATCH_H_

// Applies a delta to a basis and verifies the result.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "deltasync/delta_format.h"
#include "deltasync/io.h"
#include "deltasync/sha256.h"

namespace deltasync {

struct PatchStats {
  std::uint64_t output_bytes = 0;     // bytes written by this patcher
  std::uint64_t copied_bytes = 0;     // of which copied from the basis
  std::uint64_t literal_bytes = 0;    // of which taken from the delta
  std::uint64_t copy_ops = 0;
  std::uint64_t chunks_verified = 0;
};

// Rebuilds the target from a basis and a stream of delta records, writing
// it to `output` in order. Every chunk is hashed as it is written and
// compared with the sender's CHUNK digest, and END is checked against the
// SHA-256 of the whole target; any mismatch throws IntegrityError. Output is
// written before its chunk is verified, so it must go somewhere disposable
// (such as an AtomicFile) until finished() is true.
class Patcher final : public DeltaSink {
 public:
  // Called after each chunk is verified, with the length of the target
  // prefix that is now verified. During the call, file_hash() holds the
  // SHA-256 state of exactly that prefix.
  using ChunkCallback = std::function<void(std::uint64_t verified_length)>;

  // Rebuilds a target from offset 0.
  Patcher(RandomAccessSource& basis, ByteSink& output);

  // Continues a target at offset prefix.length(): the caller has already
  // written and verified the bytes before it, and `prefix` holds their
  // SHA-256 state. The delta's start_offset must match.
  Patcher(RandomAccessSource& basis, ByteSink& output, const Sha256& prefix);

  void on_header(const DeltaHeader& header) override;
  void on_copy(std::uint64_t offset, std::uint64_t length) override;
  void on_literal(const std::uint8_t* data, std::size_t length) override;
  void on_chunk(const Sha256Digest& digest) override;
  void on_end(std::uint64_t total_length, const Sha256Digest& digest) override;

  void set_chunk_callback(ChunkCallback callback) { chunk_callback_ = std::move(callback); }

  // True once END has been verified.
  bool finished() const { return finished_; }

  // Target offset written so far, including any resumed prefix.
  std::uint64_t position() const { return position_; }

  const PatchStats& stats() const { return stats_; }

  // SHA-256 state of the target bytes [0, position()), including a resumed
  // prefix. Inside the chunk callback it covers exactly the verified prefix,
  // so a resume journal can store file_hash().midstate() (available when the
  // verified length is a multiple of Sha256::kBlockLength, which every chunk
  // boundary is when the chunk size is resumable; see delta_format.h).
  const Sha256& file_hash() const { return file_hash_; }

 private:
  void require_operation(const char* what) const;
  void write_output(const std::uint8_t* data, std::size_t length);

  RandomAccessSource& basis_;
  ByteSink& output_;
  Sha256 chunk_hash_;
  Sha256 file_hash_;
  std::uint64_t position_ = 0;
  std::uint64_t chunk_start_ = 0;
  bool header_seen_ = false;
  bool finished_ = false;
  std::vector<std::uint8_t> copy_buffer_;
  ChunkCallback chunk_callback_;
  PatchStats stats_;
};

// Decodes the serialized delta read from `delta`, applies it to `basis`, and
// writes the verified target to `output`.
PatchStats apply_delta(RandomAccessSource& basis, ByteSource& delta, ByteSink& output);

// Rebuilds the target into a temporary file next to `output_path` and,
// only after the whole file is verified and synced, renames it over
// `output_path`. On any error the temporary file is removed and
// `output_path` is left as it was. `output_path` may be the basis itself.
// The result keeps the permissions of an existing `output_path`, otherwise
// it takes those of the basis.
PatchStats patch_file(const std::string& basis_path, ByteSource& delta,
                      const std::string& output_path);

}  // namespace deltasync

#endif  // DELTASYNC_PATCH_H_
