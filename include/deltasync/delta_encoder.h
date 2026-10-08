#ifndef DELTASYNC_DELTA_ENCODER_H_
#define DELTASYNC_DELTA_ENCODER_H_

// Computes a delta from a basis signature and the target (new version).
//
// The target is streamed through feed() in pieces of any size and records
// are pushed to a DeltaSink as soon as they are decided, so the caller can
// write them to a file or a socket while the target is still being read.
// Memory use is fixed when the encoder is constructed: the signature index
// plus a buffer of about 2 * (max_literal + block_size) bytes. feed() and
// finish() do not allocate.
//
// Matching works as in rsync. A window of block_size bytes slides over the
// target one byte at a time while the rolling checksum is updated in O(1).
// When the weak checksum equals that of a basis block, the window's SHA-256
// is computed (at most once per position, only on such hits) and compared with the
// block's strong checksum; on a match the encoder emits COPY and jumps a whole
// block ahead, otherwise the byte joins the pending literal. A short final
// basis block can only match the very end of the target.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "deltasync/delta_format.h"
#include "deltasync/io.h"
#include "deltasync/sha256.h"
#include "deltasync/signature.h"

namespace deltasync {

constexpr std::size_t kDefaultMaxLiteral = 64 * 1024;

struct DeltaOptions {
  // Output bytes per CHUNK record. Must be a positive multiple of
  // Sha256::kBlockLength (64), because a transfer resumes at a chunk boundary
  // from the SHA-256 midstate of the bytes before it.
  std::uint64_t chunk_size = kDefaultChunkSize;
  // Longest LITERAL record emitted. Unmatched bytes are flushed whenever this
  // many are pending, which also bounds the encoder's buffer.
  std::size_t max_literal = kDefaultMaxLiteral;
};

struct DeltaStats {
  std::uint64_t target_bytes = 0;   // target bytes passed to feed()
  std::uint64_t matched_bytes = 0;  // output produced by COPY records
  std::uint64_t literal_bytes = 0;  // output carried in LITERAL records
  std::uint64_t copy_ops = 0;       // COPY records after coalescing and chunking
  std::uint64_t literal_ops = 0;    // LITERAL records
  std::uint64_t chunks = 0;         // CHUNK records
  std::uint64_t weak_hits = 0;      // window positions whose weak checksum
                                    // matched a block of the same length
  std::uint64_t strong_hashes = 0;  // SHA-256 computations over target windows
  std::uint64_t false_matches = 0;  // weak hits rejected by the strong hash
};

class DeltaEncoder {
 public:
  // Starts a delta at target offset 0. `signature` must be complete and must
  // outlive the encoder. The header is sent to `sink` immediately.
  DeltaEncoder(const Signature& signature, DeltaSink& sink,
               const DeltaOptions& options = DeltaOptions());

  // Starts a delta at target offset prefix.length(), which must be a multiple
  // of options.chunk_size; this is how an interrupted transfer continues from
  // its last verified chunk. `prefix` must hold the SHA-256 state of exactly
  // the target bytes before that offset, so END still certifies the whole
  // file. Only the bytes after the offset are fed.
  DeltaEncoder(const Signature& signature, DeltaSink& sink, const DeltaOptions& options,
               const Sha256& prefix);

  ~DeltaEncoder();
  DeltaEncoder(const DeltaEncoder&) = delete;
  DeltaEncoder& operator=(const DeltaEncoder&) = delete;

  // Consumes the next `length` bytes of the target. Exceptions thrown by the
  // sink propagate and leave the encoder unusable.
  void feed(const std::uint8_t* data, std::size_t length);

  // Declares the end of the target and emits the remaining records,
  // including the final CHUNK and END.
  void finish();

  const DeltaStats& stats() const;

  // Size of the internal window buffer, fixed at construction.
  std::size_t buffer_capacity() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Reads `target` to the end and streams its delta against `signature` into
// `sink`.
DeltaStats encode_delta(const Signature& signature, ByteSource& target, DeltaSink& sink,
                        const DeltaOptions& options = DeltaOptions());

}  // namespace deltasync

#endif  // DELTASYNC_DELTA_ENCODER_H_
