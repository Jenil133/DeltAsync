#ifndef DELTASYNC_TESTS_TEST_UTIL_H_
#define DELTASYNC_TESTS_TEST_UTIL_H_

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "deltasync/delta_encoder.h"
#include "deltasync/delta_format.h"
#include "deltasync/io.h"
#include "deltasync/patch.h"
#include "deltasync/sha256.h"
#include "deltasync/signature.h"

namespace deltasync::test {

using Bytes = std::vector<std::uint8_t>;

// SplitMix64: tiny, fast and deterministic, so every randomized test
// reproduces exactly from its seed.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next();
  // Uniform enough for tests in [0, bound); bound must be positive.
  std::uint64_t below(std::uint64_t bound) { return next() % bound; }
  // Inclusive range.
  std::uint64_t between(std::uint64_t low, std::uint64_t high) {
    return low + below(high - low + 1);
  }
  void fill(std::uint8_t* out, std::size_t length);

 private:
  std::uint64_t state_;
};

Bytes random_bytes(std::size_t length, std::uint64_t seed);
Bytes bytes_of(const std::string& text);

// Compares byte strings and reports the first difference instead of
// printing both buffers.
::testing::AssertionResult same_bytes(const Bytes& expected, const Bytes& actual);

Signature make_signature(const Bytes& basis, std::uint32_t block_size,
                         std::uint32_t strong_length = kDefaultStrongLength);
Bytes serialize(const Signature& signature);

struct EncodedDelta {
  Bytes bytes;
  DeltaStats stats;
};

// Encodes `target` against `signature`. With feed_piece > 0 the target is
// fed in pieces of that size, otherwise in one call.
EncodedDelta encode(const Signature& signature, const Bytes& target,
                    const DeltaOptions& options = DeltaOptions(), std::size_t feed_piece = 0);

// Decodes and applies a serialized delta.
Bytes apply(const Bytes& basis, const Bytes& delta, PatchStats* stats = nullptr);

struct RoundTrip {
  Bytes output;
  Bytes delta;
  DeltaStats delta_stats;
  PatchStats patch_stats;
  std::uint64_t signature_bytes = 0;
};

// The whole local pipeline through the serialized formats: signature of the
// basis, written and parsed back, delta of the target, written, then decoded
// and patched onto the basis.
RoundTrip round_trip(const Bytes& basis, const Bytes& target, std::uint32_t block_size,
                     const DeltaOptions& options = DeltaOptions());

// One DeltaSink callback.
struct Record {
  enum class Kind { kHeader, kCopy, kLiteral, kChunk, kEnd };
  Kind kind = Kind::kHeader;
  std::uint64_t offset = 0;  // COPY offset
  std::uint64_t length = 0;  // COPY/LITERAL length, END total length
  Bytes data;                // LITERAL bytes
  Sha256Digest digest{};     // CHUNK/END digest
  DeltaHeader header;        // header fields

  bool operator==(const Record& other) const;
};

std::ostream& operator<<(std::ostream& out, const Record& record);

// Keeps every callback, merging consecutive literal callbacks so the result
// does not depend on how a decoder split them.
class RecordingSink final : public DeltaSink {
 public:
  void on_header(const DeltaHeader& header) override;
  void on_copy(std::uint64_t offset, std::uint64_t length) override;
  void on_literal(const std::uint8_t* data, std::size_t length) override;
  void on_chunk(const Sha256Digest& digest) override;
  void on_end(std::uint64_t total_length, const Sha256Digest& digest) override;

  // Replays the records into another sink.
  void replay(DeltaSink& sink) const;

  std::vector<Record> records;
  bool merge_literals = true;
};

// Counts and hashes what it receives without storing it.
class HashingSink final : public ByteSink {
 public:
  void write(const std::uint8_t* data, std::size_t length) override {
    hash.update(data, length);
    bytes += length;
  }

  Sha256 hash;
  std::uint64_t bytes = 0;
};

// A directory under $TMPDIR that is deleted with its contents when the
// object goes out of scope.
class TempDir {
 public:
  TempDir();
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string file(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

void write_file(const std::string& path, const Bytes& data);
Bytes read_file(const std::string& path);
bool file_exists(const std::string& path);
// Names of the entries in `directory`, sorted.
std::vector<std::string> list_directory(const std::string& directory);

}  // namespace deltasync::test

#endif  // DELTASYNC_TESTS_TEST_UTIL_H_
