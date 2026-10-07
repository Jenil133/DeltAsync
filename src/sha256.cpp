#include "deltasync/sha256.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "deltasync/bytes.h"

namespace deltasync {
namespace {

// FIPS 180-4, section 4.2.2: the first 32 bits of the fractional parts of
// the cube roots of the first 64 primes.
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

// FIPS 180-4, section 5.3.3: the first 32 bits of the fractional parts of
// the square roots of the first 8 primes.
constexpr std::uint32_t kInitialState[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

void Sha256::reset() {
  std::memcpy(state_, kInitialState, sizeof(state_));
  length_ = 0;
  buffered_ = 0;
}

Sha256 Sha256::from_midstate(const Midstate& state) {
  if (state.length % kBlockLength != 0) {
    throw std::invalid_argument("SHA-256 midstate length " + std::to_string(state.length) +
                                " is not a whole number of blocks");
  }
  Sha256 hasher;
  std::copy(state.words.begin(), state.words.end(), hasher.state_);
  hasher.length_ = state.length;
  return hasher;
}

Sha256::Midstate Sha256::midstate() const {
  if (buffered_ != 0) {
    throw std::logic_error("SHA-256 midstate requested " + std::to_string(buffered_) +
                           " bytes into a block");
  }
  Midstate state;
  std::copy(state_, state_ + 8, state.words.begin());
  state.length = length_;
  return state;
}

void Sha256::update(const void* data, std::size_t length) {
  if (length == 0) return;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  length_ += length;

  if (buffered_ > 0) {
    const std::size_t take = std::min(length, kBlockLength - buffered_);
    std::memcpy(buffer_ + buffered_, bytes, take);
    buffered_ += take;
    bytes += take;
    length -= take;
    if (buffered_ < kBlockLength) return;
    compress(buffer_, 1);
    buffered_ = 0;
  }

  // Whole blocks are compressed straight from the caller's memory.
  const std::size_t blocks = length / kBlockLength;
  if (blocks > 0) {
    compress(bytes, blocks);
    bytes += blocks * kBlockLength;
    length -= blocks * kBlockLength;
  }
  if (length > 0) {
    std::memcpy(buffer_, bytes, length);
    buffered_ = length;
  }
}

Sha256Digest Sha256::digest() const {
  // Padding (FIPS 180-4, section 5.1.1): a 1 bit, zeros up to 56 bytes mod
  // 64, then the message length in bits as a 64-bit big-endian integer.
  Sha256 tail = *this;
  const std::uint64_t bit_length = length_ * 8;
  std::uint8_t padding[kBlockLength + 8] = {0x80};
  const std::size_t zeros_and_marker =
      buffered_ < 56 ? 56 - buffered_ : kBlockLength + 56 - buffered_;
  store_be64(padding + zeros_and_marker, bit_length);
  tail.update(padding, zeros_and_marker + 8);

  Sha256Digest out;
  for (int i = 0; i < 8; ++i) store_be32(out.data() + 4 * i, tail.state_[i]);
  return out;
}

Sha256Digest Sha256::hash(const void* data, std::size_t length) {
  Sha256 hasher;
  hasher.update(data, length);
  return hasher.digest();
}

void Sha256::compress(const std::uint8_t* blocks, std::size_t count) {
  std::uint32_t w[64];
  for (; count > 0; --count, blocks += kBlockLength) {
    for (int i = 0; i < 16; ++i) w[i] = load_be32(blocks + 4 * i);
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t choose = (e & f) ^ (~e & g);
      const std::uint32_t t1 = h + s1 + choose + kRoundConstants[i] + w[i];
      const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = s0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }
}

std::string to_hex(const std::uint8_t* data, std::size_t length) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(length * 2);
  for (std::size_t i = 0; i < length; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0x0f]);
  }
  return out;
}

}  // namespace deltasync
