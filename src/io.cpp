#include "deltasync/io.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "deltasync/error.h"

namespace deltasync {

std::size_t MemorySource::read(std::uint8_t* out, std::size_t capacity) {
  const std::size_t count = std::min(capacity, length_ - position_);
  if (count > 0) {
    std::memcpy(out, data_ + position_, count);
    position_ += count;
  }
  return count;
}

void MemorySink::write(const std::uint8_t* data, std::size_t length) {
  if (length > 0) data_.insert(data_.end(), data, data + length);
}

void MemoryRandomAccess::read_at(std::uint64_t offset, std::uint8_t* out, std::size_t length) {
  if (offset > length_ || length > length_ - offset) {
    throw Error("read of " + std::to_string(length) + " bytes at offset " +
                std::to_string(offset) + " is outside a source of " +
                std::to_string(length_) + " bytes");
  }
  if (length > 0) std::memcpy(out, data_ + offset, length);
}

}  // namespace deltasync
