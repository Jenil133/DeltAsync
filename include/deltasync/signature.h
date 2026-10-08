#ifndef DELTASYNC_SIGNATURE_H_
#define DELTASYNC_SIGNATURE_H_

// The signature of a basis file: its length, the block size, and for every
// block a weak rolling checksum plus a (possibly truncated) SHA-256. The side
// holding the new version uses it to find which blocks the other side
// already has.
//
// Signatures are produced and consumed as a stream of records through
// SignatureSink, so generation, serialization, parsing, and building the
// in-memory lookup structure are independent pieces that can be chained.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "deltasync/bytes.h"
#include "deltasync/io.h"

namespace deltasync {

constexpr std::uint8_t kSignatureVersion = 1;

// Limits accepted by the signature format.
constexpr std::uint32_t kMinBlockSize = 1;
constexpr std::uint32_t kMaxBlockSize = 16u << 20;
constexpr std::uint32_t kMinStrongLength = 8;
constexpr std::uint32_t kMaxStrongLength = 32;
constexpr std::uint64_t kMaxBlockCount = std::uint64_t{1} << 31;

// Range used by default_block_size().
constexpr std::uint32_t kMinDefaultBlockSize = 1024;
constexpr std::uint32_t kMaxDefaultBlockSize = 128 * 1024;

// 128 bits of SHA-256 per block: accidental collisions are out of reach even
// for very large files, at 20 signature bytes per block including the weak sum.
constexpr std::uint32_t kDefaultStrongLength = 16;

// Block size used when the caller does not choose one: sqrt(basis_size)
// rounded up to a multiple of 1 KiB and clamped to [1 KiB, 128 KiB].
//
// Each changed region costs roughly one block of literal data, while the
// signature costs 20 bytes per block, so the total transfer
// (basis_size / B) * 20 + edits * B is smallest for B near
// sqrt(basis_size * 20 / edits). sqrt(basis_size) is rsync's heuristic for a
// moderate number of edits. The lower clamp keeps signatures of small files
// from being dominated by per-block overhead; the upper clamp bounds the cost
// of a single edit in very large files (one 128 KiB block) and keeps the
// encoder's window buffer small. Examples: 64 MiB -> 8 KiB, 1 GiB -> 32 KiB,
// anything up to 1 MiB -> 1 KiB, 16 GiB and beyond -> 128 KiB.
std::uint32_t default_block_size(std::uint64_t basis_size);

struct SignatureHeader {
  std::uint32_t block_size = 0;
  std::uint32_t strong_length = kDefaultStrongLength;
  std::uint64_t basis_size = 0;

  std::uint64_t block_count() const;

  // Every block is block_size bytes except possibly the last.
  std::uint32_t block_length(std::uint64_t index) const;
};

// Throws FormatError if a field is outside the limits above.
void validate(const SignatureHeader& header);

// Receives a signature as a stream: one on_header(), then exactly
// header.block_count() calls to on_block() in block order, then on_end().
class SignatureSink {
 public:
  virtual ~SignatureSink() = default;
  virtual void on_header(const SignatureHeader& header) = 0;
  // `strong` points at header.strong_length bytes: the leading bytes of the
  // block's SHA-256.
  virtual void on_block(std::uint32_t weak, const std::uint8_t* strong) = 0;
  virtual void on_end() = 0;
};

// Computes a signature incrementally. The basis is fed in pieces of any
// size; memory use is one block regardless of the basis size.
class SignatureGenerator {
 public:
  // Validates `header` and sends it to `sink` immediately.
  SignatureGenerator(const SignatureHeader& header, SignatureSink& sink);

  // Throws Error if more than header.basis_size bytes are fed in total.
  void feed(const std::uint8_t* data, std::size_t length);

  // Emits the final (possibly short) block and on_end(). Throws Error if
  // fewer than header.basis_size bytes were fed.
  void finish();

 private:
  void emit_block(const std::uint8_t* data, std::size_t length);

  SignatureHeader header_;
  SignatureSink& sink_;
  std::vector<std::uint8_t> block_;
  std::size_t buffered_ = 0;
  std::uint64_t fed_ = 0;
  bool finished_ = false;
};

// Reads `basis` to the end and streams its signature into `sink`.
// `header.basis_size` must equal the number of bytes the source yields; a
// mismatch (for example a file that changed while it was read) throws Error.
void generate_signature(ByteSource& basis, const SignatureHeader& header,
                        SignatureSink& sink);

// A complete signature held in memory, as the delta encoder needs it.
class Signature {
 public:
  Signature() = default;
  explicit Signature(const SignatureHeader& header);

  const SignatureHeader& header() const { return header_; }
  std::uint32_t block_size() const { return header_.block_size; }
  std::uint32_t strong_length() const { return header_.strong_length; }

  // Number of blocks added so far; equals header().block_count() once the
  // signature is complete.
  std::uint32_t block_count() const { return static_cast<std::uint32_t>(weak_.size()); }
  bool complete() const { return weak_.size() == header_.block_count(); }

  std::uint32_t weak(std::uint32_t index) const { return weak_[index]; }
  const std::uint8_t* strong(std::uint32_t index) const {
    return strong_.data() + std::size_t{index} * header_.strong_length;
  }
  std::uint32_t block_length(std::uint32_t index) const { return header_.block_length(index); }

  // Throws FormatError if the signature already holds every block.
  void add_block(std::uint32_t weak, const std::uint8_t* strong);

  // Streams the signature into `sink`, e.g. a SignatureWriter.
  void replay(SignatureSink& sink) const;

 private:
  SignatureHeader header_;
  std::vector<std::uint32_t> weak_;
  std::vector<std::uint8_t> strong_;
};

// Collects a streamed signature into a Signature.
class SignatureBuilder final : public SignatureSink {
 public:
  void on_header(const SignatureHeader& header) override;
  void on_block(std::uint32_t weak, const std::uint8_t* strong) override;
  void on_end() override;

  bool done() const { return done_; }

  // The collected signature; only valid after on_end().
  Signature take();

 private:
  Signature signature_;
  bool started_ = false;
  bool done_ = false;
};

// Serialized form (all integers are varints unless noted):
//
//   magic "DSIG" | version (1 byte) | flags (must be 0) | block_size |
//   strong_length | basis_size | block_count x { weak (4 bytes, big-endian),
//   strong (strong_length bytes) }
//
// The block count is implied by basis_size and block_size, so a truncated or
// over-long signature is detected without a trailer.
class SignatureWriter final : public SignatureSink {
 public:
  explicit SignatureWriter(ByteSink& out) : out_(out) {}

  void on_header(const SignatureHeader& header) override;
  void on_block(std::uint32_t weak, const std::uint8_t* strong) override;
  void on_end() override;

  std::uint64_t bytes_written() const { return bytes_written_; }

 private:
  void write(const std::uint8_t* data, std::size_t length);

  ByteSink& out_;
  std::uint32_t strong_length_ = 0;
  std::uint64_t bytes_written_ = 0;
};

// Parses a serialized signature incrementally. Input may be split anywhere.
// All structural problems (bad magic, unsupported version, out-of-range
// header fields, truncation, trailing bytes) throw FormatError.
class SignatureDecoder {
 public:
  explicit SignatureDecoder(SignatureSink& sink) : sink_(sink) {}

  void feed(const std::uint8_t* data, std::size_t length);

  // Declares the end of input; throws FormatError if the signature is incomplete.
  void finish();

  bool done() const { return state_ == State::kDone; }

 private:
  enum class State {
    kMagic,
    kVersion,
    kFlags,
    kBlockSize,
    kStrongLength,
    kBasisSize,
    kBlocks,
    kDone,
  };

  void consume_header_byte(std::uint8_t byte);
  void finish_header();
  void consume_block(const std::uint8_t* entry);

  SignatureSink& sink_;
  State state_ = State::kMagic;
  VarintDecoder varint_;
  SignatureHeader header_;
  std::uint64_t blocks_left_ = 0;
  std::uint8_t staging_[4 + kMaxStrongLength];
  std::size_t staged_ = 0;
};

// Parses a complete serialized signature from `source` into memory.
Signature read_signature(ByteSource& source);

}  // namespace deltasync

#endif  // DELTASYNC_SIGNATURE_H_
