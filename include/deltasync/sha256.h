#ifndef DELTASYNC_SHA256_H_
#define DELTASYNC_SHA256_H_

// SHA-256 as specified in FIPS 180-4. Used as the strong checksum for block
// matching and for verifying reconstructed output.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace deltasync {

using Sha256Digest = std::array<std::uint8_t, 32>;

class Sha256 {
 public:
  static constexpr std::size_t kDigestLength = 32;
  static constexpr std::size_t kBlockLength = 64;

  // The chaining value after a whole number of blocks, plus the byte count.
  // That is everything needed to continue a message later without the bytes
  // already hashed, e.g. after reloading a resume journal. It is not a
  // digest: the final padding has not been applied.
  struct Midstate {
    std::array<std::uint32_t, 8> words{};
    std::uint64_t length = 0;  // bytes hashed; a multiple of kBlockLength
  };

  Sha256() { reset(); }

  // A hasher that continues the message `state` was taken from. Throws
  // std::invalid_argument unless state.length is a multiple of kBlockLength.
  static Sha256 from_midstate(const Midstate& state);

  // Starts a new message.
  void reset();

  // Appends `length` bytes to the message. The total message length is kept
  // as a 64-bit byte count, so messages may exceed 4 GiB; the limit is the
  // 2^61 - 1 bytes that fit SHA-256's 64-bit bit-length field.
  void update(const void* data, std::size_t length);

  // Digest of the bytes added so far. The object is unchanged, so more data
  // can follow: digest() of a prefix is available while hashing continues.
  Sha256Digest digest() const;

  // Number of bytes added since the last reset().
  std::uint64_t length() const { return length_; }

  // Throws std::logic_error unless length() is a multiple of kBlockLength,
  // the only points at which no message bytes are buffered.
  Midstate midstate() const;

  // One-shot convenience.
  static Sha256Digest hash(const void* data, std::size_t length);

 private:
  void compress(const std::uint8_t* blocks, std::size_t count);

  std::uint32_t state_[8];
  std::uint64_t length_;
  std::uint8_t buffer_[kBlockLength];
  std::size_t buffered_;
};

// Lower-case hexadecimal rendering, e.g. for logs and test failure messages.
std::string to_hex(const std::uint8_t* data, std::size_t length);

inline std::string to_hex(const Sha256Digest& digest) {
  return to_hex(digest.data(), digest.size());
}

}  // namespace deltasync

#endif  // DELTASYNC_SHA256_H_
