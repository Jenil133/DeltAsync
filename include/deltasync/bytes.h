#ifndef DELTASYNC_BYTES_H_
#define DELTASYNC_BYTES_H_

// Fixed-width big-endian integers and unsigned LEB128 varints: the two
// integer encodings used by every serialized format in deltasync.

#include <cstddef>
#include <cstdint>

namespace deltasync {

// A varint stores 7 bits per byte, least significant group first; every byte
// except the last has its high bit set. A 64-bit value needs at most 10 bytes.
constexpr std::size_t kMaxVarintLength = 10;

// Writes the minimal encoding of `value` to `out`, which must have room for
// kMaxVarintLength bytes, and returns the number of bytes written.
std::size_t encode_varint(std::uint64_t value, std::uint8_t* out);

// Number of bytes encode_varint() writes for `value`.
std::size_t varint_length(std::uint64_t value);

// Decodes varints one byte at a time, so a value split across reads needs no
// reassembly by the caller. Only minimal encodings are accepted (0x80 0x00 is
// rejected as a spelling of zero) and values must fit in 64 bits, so every
// value has exactly one valid encoding and corrupted bytes are caught early.
class VarintDecoder {
 public:
  // Consumes one byte. Returns true when `byte` completes a value, which is
  // stored in `*value`; the decoder is then ready for the next value.
  // Throws FormatError on an invalid encoding.
  bool feed(std::uint8_t byte, std::uint64_t* value);

  // True while part of a value has been consumed.
  bool in_progress() const { return shift_ != 0; }

  void reset() {
    accumulator_ = 0;
    shift_ = 0;
  }

 private:
  std::uint64_t accumulator_ = 0;
  unsigned shift_ = 0;
};

inline void store_be32(std::uint8_t* out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>(value >> 24);
  out[1] = static_cast<std::uint8_t>(value >> 16);
  out[2] = static_cast<std::uint8_t>(value >> 8);
  out[3] = static_cast<std::uint8_t>(value);
}

inline std::uint32_t load_be32(const std::uint8_t* in) {
  return (static_cast<std::uint32_t>(in[0]) << 24) |
         (static_cast<std::uint32_t>(in[1]) << 16) |
         (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

inline void store_be64(std::uint8_t* out, std::uint64_t value) {
  store_be32(out, static_cast<std::uint32_t>(value >> 32));
  store_be32(out + 4, static_cast<std::uint32_t>(value));
}

inline std::uint64_t load_be64(const std::uint8_t* in) {
  return (static_cast<std::uint64_t>(load_be32(in)) << 32) | load_be32(in + 4);
}

}  // namespace deltasync

#endif  // DELTASYNC_BYTES_H_
