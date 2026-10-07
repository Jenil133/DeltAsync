#ifndef DELTASYNC_ROLLING_CHECKSUM_H_
#define DELTASYNC_ROLLING_CHECKSUM_H_

// The rsync weak checksum (Tridgell & Mackerras, 1996). For a window
// x[0..n) with y[i] = x[i] + kCharOffset:
//
//   a = sum(y[i])               mod 2^16
//   b = sum((n - i) * y[i])     mod 2^16
//   digest = a | (b << 16)
//
// Sliding the window by one byte, adding a byte at the end, or dropping one
// from the front each update a and b in O(1), which is what makes testing
// every byte offset of the target file affordable.
//
// kCharOffset (31, the value librsync uses) keeps zero bytes from
// contributing nothing, so runs of zeros of different lengths still get
// different checksums.
//
// The sums are kept in 32-bit unsigned accumulators that wrap mod 2^32; since
// 2^16 divides 2^32, reducing to 16 bits only in digest() gives the same
// result as reducing after every step.

#include <cstddef>
#include <cstdint>

namespace deltasync {

class RollingChecksum {
 public:
  static constexpr std::uint32_t kCharOffset = 31;

  void reset() {
    a_ = 0;
    b_ = 0;
    count_ = 0;
  }

  // Appends `length` bytes to the end of the window.
  void update(const std::uint8_t* data, std::size_t length) {
    std::uint32_t a = a_;
    std::uint32_t b = b_;
    for (std::size_t i = 0; i < length; ++i) {
      a += data[i] + kCharOffset;
      b += a;
    }
    a_ = a;
    b_ = b;
    count_ += length;
  }

  // Appends one byte: every existing byte gains one unit of weight, which
  // adds the old a to b, and the new byte enters with weight 1.
  void roll_in(std::uint8_t in) {
    a_ += in + kCharOffset;
    b_ += a_;
    ++count_;
  }

  // Removes the oldest byte, which carried weight n.
  void roll_out(std::uint8_t out) {
    const std::uint32_t y = out + kCharOffset;
    a_ -= y;
    b_ -= static_cast<std::uint32_t>(count_) * y;
    --count_;
  }

  // Slides a window of fixed length by one byte: roll_out(out) then
  // roll_in(in) without changing the length.
  void rotate(std::uint8_t out, std::uint8_t in) {
    const std::uint32_t y_out = out + kCharOffset;
    a_ = a_ - y_out + (in + kCharOffset);
    b_ = b_ - static_cast<std::uint32_t>(count_) * y_out + a_;
  }

  std::uint32_t digest() const { return (a_ & 0xffffu) | (b_ << 16); }

  // Number of bytes in the window.
  std::size_t count() const { return count_; }

  static std::uint32_t compute(const std::uint8_t* data, std::size_t length) {
    RollingChecksum sum;
    sum.update(data, length);
    return sum.digest();
  }

 private:
  std::uint32_t a_ = 0;
  std::uint32_t b_ = 0;
  std::size_t count_ = 0;
};

}  // namespace deltasync

#endif  // DELTASYNC_ROLLING_CHECKSUM_H_
