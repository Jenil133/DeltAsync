#include "deltasync/bytes.h"

#include "deltasync/error.h"

namespace deltasync {

std::size_t encode_varint(std::uint64_t value, std::uint8_t* out) {
  std::size_t length = 0;
  while (value >= 0x80) {
    out[length++] = static_cast<std::uint8_t>(value | 0x80);
    value >>= 7;
  }
  out[length++] = static_cast<std::uint8_t>(value);
  return length;
}

std::size_t varint_length(std::uint64_t value) {
  std::size_t length = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++length;
  }
  return length;
}

bool VarintDecoder::feed(std::uint8_t byte, std::uint64_t* value) {
  // A zero byte after the first carries no bits: the same value has a
  // shorter encoding.
  if (shift_ > 0 && byte == 0) {
    reset();
    throw FormatError("non-minimal varint encoding");
  }
  // The tenth byte holds bit 63 only; anything larger cannot fit in 64 bits
  // (and a continuation bit would make the encoding longer than ten bytes).
  if (shift_ == 63 && byte > 1) {
    reset();
    throw FormatError("varint does not fit in 64 bits");
  }
  accumulator_ |= static_cast<std::uint64_t>(byte & 0x7f) << shift_;
  if ((byte & 0x80) != 0) {
    shift_ += 7;
    return false;
  }
  *value = accumulator_;
  reset();
  return true;
}

}  // namespace deltasync
