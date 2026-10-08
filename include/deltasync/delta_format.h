#ifndef DELTASYNC_DELTA_FORMAT_H_
#define DELTASYNC_DELTA_FORMAT_H_

// A delta is a stream of records that rebuild the target (new version) from
// the basis (old version):
//
//   COPY(offset, length)  output basis bytes [offset, offset + length)
//   LITERAL(bytes)        output bytes carried in the delta
//   CHUNK(sha256)         SHA-256 of the output written since the previous
//                         chunk boundary
//   END(length, sha256)   total target length and SHA-256 of the whole target
//
// The output is divided into chunks of chunk_size bytes, counted from offset
// 0 of the target. No COPY or LITERAL crosses a chunk boundary, and a CHUNK
// record follows the last operation of every chunk, including a final short
// chunk. A receiver can therefore verify and persist the output chunk by
// chunk, and a transfer can continue from any chunk boundary: start_offset in
// the header says where in the target the stream begins.
//
// Records only ever need a few bytes of lookahead, and literal data can be
// passed through as it arrives, so both ends work in bounded memory no
// matter how large the files are.

#include <cstddef>
#include <cstdint>

#include "deltasync/bytes.h"
#include "deltasync/io.h"
#include "deltasync/sha256.h"

namespace deltasync {

constexpr std::uint8_t kDeltaVersion = 1;
constexpr std::uint64_t kDefaultChunkSize = 1 << 20;
constexpr std::uint64_t kMaxChunkSize = std::uint64_t{1} << 30;

struct DeltaHeader {
  std::uint64_t chunk_size = kDefaultChunkSize;
  // Length of the basis the delta was computed against. A patcher checks it
  // against the basis it was given before applying anything.
  std::uint64_t basis_size = 0;
  // Target offset of the first operation; a multiple of chunk_size.
  std::uint64_t start_offset = 0;
};

// True if `chunk_size` is a positive multiple of Sha256::kBlockLength (64)
// no larger than kMaxChunkSize. Every chunk boundary is a point a transfer
// can resume from, and resuming restores the SHA-256 state of the verified
// prefix from a Sha256::Midstate, which only exists at block multiples.
constexpr bool valid_chunk_size(std::uint64_t chunk_size) {
  return chunk_size > 0 && chunk_size <= kMaxChunkSize &&
         chunk_size % Sha256::kBlockLength == 0;
}

// Throws FormatError unless valid_chunk_size(chunk_size) holds and
// start_offset is a multiple of chunk_size.
void validate(const DeltaHeader& header);

// Receives a delta as a stream of records.
class DeltaSink {
 public:
  virtual ~DeltaSink() = default;
  virtual void on_header(const DeltaHeader& header) = 0;
  virtual void on_copy(std::uint64_t offset, std::uint64_t length) = 0;
  // A decoder may deliver one LITERAL record through several calls as its
  // bytes arrive; consumers must not depend on how literals are split.
  virtual void on_literal(const std::uint8_t* data, std::size_t length) = 0;
  virtual void on_chunk(const Sha256Digest& digest) = 0;
  virtual void on_end(std::uint64_t total_length, const Sha256Digest& digest) = 0;
};

// Serialized form (integers are varints):
//
//   header: magic "DSDL" | version (1 byte) | flags (must be 0) |
//           chunk_size (a multiple of 64) | basis_size | start_offset
//   COPY    0x01 | offset | length
//   LITERAL 0x02 | length | length bytes
//   CHUNK   0x03 | 32-byte SHA-256
//   END     0x04 | total_length | 32-byte SHA-256
enum class DeltaRecord : std::uint8_t {
  kCopy = 0x01,
  kLiteral = 0x02,
  kChunk = 0x03,
  kEnd = 0x04,
};

// Serializes records to a ByteSink.
class DeltaWriter final : public DeltaSink {
 public:
  explicit DeltaWriter(ByteSink& out) : out_(out) {}

  void on_header(const DeltaHeader& header) override;
  void on_copy(std::uint64_t offset, std::uint64_t length) override;
  void on_literal(const std::uint8_t* data, std::size_t length) override;
  void on_chunk(const Sha256Digest& digest) override;
  // Writes the END record and flushes the underlying sink.
  void on_end(std::uint64_t total_length, const Sha256Digest& digest) override;

  std::uint64_t bytes_written() const { return bytes_written_; }

 private:
  void write(const std::uint8_t* data, std::size_t length);

  ByteSink& out_;
  std::uint64_t bytes_written_ = 0;
};

// Parses a serialized delta incrementally and forwards the records to a
// sink. Input may be split at any byte. Besides syntax, the decoder enforces
// the structural rules a patcher relies on: operations are non-empty, COPY
// ranges lie inside the declared basis, nothing crosses a chunk boundary,
// every output byte is covered by a CHUNK record before END, END's length
// matches the operations, and nothing follows END. Violations throw
// FormatError; checksums are verified by the sink (see Patcher).
class DeltaDecoder {
 public:
  explicit DeltaDecoder(DeltaSink& sink) : sink_(sink) {}

  void feed(const std::uint8_t* data, std::size_t length);

  // Declares the end of input; throws FormatError unless END was decoded.
  void finish();

  bool done() const { return state_ == State::kDone; }

  // Output offset reached by the operations decoded so far.
  std::uint64_t position() const { return position_; }

 private:
  enum class State {
    kMagic,
    kVersion,
    kFlags,
    kChunkSize,
    kBasisSize,
    kStartOffset,
    kRecordType,
    kCopyOffset,
    kCopyLength,
    kLiteralLength,
    kLiteralData,
    kChunkDigest,
    kEndLength,
    kEndDigest,
    kDone,
  };

  // Handles one byte of every state except kLiteralData.
  void consume_byte(std::uint8_t byte);
  void begin_record(std::uint8_t type);
  void require_operation_allowed(std::uint64_t length, const char* what) const;
  std::uint64_t chunk_room() const;

  DeltaSink& sink_;
  State state_ = State::kMagic;
  VarintDecoder varint_;
  DeltaHeader header_;
  std::uint64_t position_ = 0;
  std::uint64_t chunk_start_ = 0;
  bool final_chunk_seen_ = false;
  std::uint64_t pending_value_ = 0;
  std::uint64_t literal_left_ = 0;
  std::uint8_t staging_[Sha256::kDigestLength];
  std::size_t staged_ = 0;
};

}  // namespace deltasync

#endif  // DELTASYNC_DELTA_FORMAT_H_
