#ifndef DELTASYNC_IO_H_
#define DELTASYNC_IO_H_

// Minimal stream interfaces. Every producer in the library writes to a
// ByteSink and every consumer reads from a ByteSource or RandomAccessSource,
// so the same code runs against files, memory buffers, or a network stream.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace deltasync {

// Size of the read requests and write buffers used throughout the library.
constexpr std::size_t kIoBufferSize = 256 * 1024;

// A sequential stream of bytes.
class ByteSource {
 public:
  virtual ~ByteSource() = default;

  // Reads up to `capacity` bytes into `out` and returns how many were read.
  // Returns 0 only at the end of the stream; throws on I/O errors.
  virtual std::size_t read(std::uint8_t* out, std::size_t capacity) = 0;
};

// A destination for a sequential stream of bytes.
class ByteSink {
 public:
  virtual ~ByteSink() = default;

  // Writes all `length` bytes or throws.
  virtual void write(const std::uint8_t* data, std::size_t length) = 0;

  // Pushes buffered bytes to the underlying destination.
  virtual void flush() {}
};

// Random-access reads from data of a fixed size (the basis during patching).
class RandomAccessSource {
 public:
  virtual ~RandomAccessSource() = default;

  virtual std::uint64_t size() const = 0;

  // Reads exactly `length` bytes starting at `offset`. Throws if the range is
  // not inside [0, size()) or the read fails.
  virtual void read_at(std::uint64_t offset, std::uint8_t* out, std::size_t length) = 0;
};

// Reads from a caller-owned buffer.
class MemorySource final : public ByteSource {
 public:
  MemorySource(const std::uint8_t* data, std::size_t length)
      : data_(data), length_(length) {}
  explicit MemorySource(const std::vector<std::uint8_t>& data)
      : MemorySource(data.data(), data.size()) {}

  std::size_t read(std::uint8_t* out, std::size_t capacity) override;

 private:
  const std::uint8_t* data_;
  std::size_t length_;
  std::size_t position_ = 0;
};

// Appends everything written to a vector.
class MemorySink final : public ByteSink {
 public:
  void write(const std::uint8_t* data, std::size_t length) override;

  const std::vector<std::uint8_t>& data() const { return data_; }
  std::vector<std::uint8_t> take() { return std::move(data_); }

 private:
  std::vector<std::uint8_t> data_;
};

// Random access over a caller-owned buffer.
class MemoryRandomAccess final : public RandomAccessSource {
 public:
  MemoryRandomAccess(const std::uint8_t* data, std::size_t length)
      : data_(data), length_(length) {}
  explicit MemoryRandomAccess(const std::vector<std::uint8_t>& data)
      : MemoryRandomAccess(data.data(), data.size()) {}

  std::uint64_t size() const override { return length_; }
  void read_at(std::uint64_t offset, std::uint8_t* out, std::size_t length) override;

 private:
  const std::uint8_t* data_;
  std::size_t length_;
};

}  // namespace deltasync

#endif  // DELTASYNC_IO_H_
