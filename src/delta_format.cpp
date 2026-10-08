#include "deltasync/delta_format.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#include "deltasync/error.h"

namespace deltasync {
namespace {

constexpr std::uint8_t kDeltaMagic[4] = {'D', 'S', 'D', 'L'};

std::string hex_byte(std::uint8_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  return std::string("0x") + kDigits[value >> 4] + kDigits[value & 0x0f];
}

}  // namespace

void validate(const DeltaHeader& header) {
  if (header.chunk_size == 0 || header.chunk_size > kMaxChunkSize) {
    throw FormatError("chunk size " + std::to_string(header.chunk_size) + " is outside [1, " +
                      std::to_string(kMaxChunkSize) + "]");
  }
  if (!valid_chunk_size(header.chunk_size)) {
    throw FormatError("chunk size " + std::to_string(header.chunk_size) +
                      " is not a multiple of " + std::to_string(Sha256::kBlockLength) +
                      " (the SHA-256 block length)");
  }
  if (header.start_offset % header.chunk_size != 0) {
    throw FormatError("start offset " + std::to_string(header.start_offset) +
                      " is not a multiple of the chunk size " +
                      std::to_string(header.chunk_size));
  }
}

void DeltaWriter::on_header(const DeltaHeader& header) {
  validate(header);
  std::uint8_t buffer[sizeof(kDeltaMagic) + 1 + 4 * kMaxVarintLength];
  std::size_t used = 0;
  std::memcpy(buffer, kDeltaMagic, sizeof(kDeltaMagic));
  used += sizeof(kDeltaMagic);
  buffer[used++] = kDeltaVersion;
  used += encode_varint(0, buffer + used);  // flags
  used += encode_varint(header.chunk_size, buffer + used);
  used += encode_varint(header.basis_size, buffer + used);
  used += encode_varint(header.start_offset, buffer + used);
  write(buffer, used);
}

void DeltaWriter::on_copy(std::uint64_t offset, std::uint64_t length) {
  if (length == 0) return;
  std::uint8_t buffer[1 + 2 * kMaxVarintLength];
  std::size_t used = 0;
  buffer[used++] = static_cast<std::uint8_t>(DeltaRecord::kCopy);
  used += encode_varint(offset, buffer + used);
  used += encode_varint(length, buffer + used);
  write(buffer, used);
}

void DeltaWriter::on_literal(const std::uint8_t* data, std::size_t length) {
  if (length == 0) return;
  std::uint8_t buffer[1 + kMaxVarintLength];
  std::size_t used = 0;
  buffer[used++] = static_cast<std::uint8_t>(DeltaRecord::kLiteral);
  used += encode_varint(length, buffer + used);
  write(buffer, used);
  write(data, length);
}

void DeltaWriter::on_chunk(const Sha256Digest& digest) {
  std::uint8_t buffer[1 + Sha256::kDigestLength];
  buffer[0] = static_cast<std::uint8_t>(DeltaRecord::kChunk);
  std::memcpy(buffer + 1, digest.data(), digest.size());
  write(buffer, sizeof(buffer));
}

void DeltaWriter::on_end(std::uint64_t total_length, const Sha256Digest& digest) {
  std::uint8_t buffer[1 + kMaxVarintLength + Sha256::kDigestLength];
  std::size_t used = 0;
  buffer[used++] = static_cast<std::uint8_t>(DeltaRecord::kEnd);
  used += encode_varint(total_length, buffer + used);
  std::memcpy(buffer + used, digest.data(), digest.size());
  used += digest.size();
  write(buffer, used);
  out_.flush();
}

void DeltaWriter::write(const std::uint8_t* data, std::size_t length) {
  out_.write(data, length);
  bytes_written_ += length;
}

void DeltaDecoder::feed(const std::uint8_t* data, std::size_t length) {
  while (length > 0) {
    if (state_ == State::kLiteralData) {
      // Literal bytes are handed on as they arrive instead of being
      // collected, so a large LITERAL never has to fit in memory.
      const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(length, literal_left_));
      literal_left_ -= take;
      if (literal_left_ == 0) state_ = State::kRecordType;
      sink_.on_literal(data, take);
      data += take;
      length -= take;
      continue;
    }
    if (state_ == State::kDone) throw FormatError("unexpected data after the end of the delta");
    consume_byte(*data);
    ++data;
    --length;
  }
}

void DeltaDecoder::finish() {
  if (state_ == State::kDone) return;
  if (state_ == State::kMagic || state_ == State::kVersion || state_ == State::kFlags ||
      state_ == State::kChunkSize || state_ == State::kBasisSize ||
      state_ == State::kStartOffset) {
    throw FormatError("delta is truncated inside its header");
  }
  throw FormatError("delta is truncated after " + std::to_string(position_) +
                    " bytes of output (no END record)");
}

void DeltaDecoder::consume_byte(std::uint8_t byte) {
  std::uint64_t value = 0;
  switch (state_) {
    case State::kMagic:
      if (byte != kDeltaMagic[staged_]) throw FormatError("not a deltasync delta (bad magic number)");
      if (++staged_ == sizeof(kDeltaMagic)) {
        staged_ = 0;
        state_ = State::kVersion;
      }
      return;
    case State::kVersion:
      if (byte != kDeltaVersion) {
        throw FormatError("unsupported delta version " + std::to_string(byte));
      }
      state_ = State::kFlags;
      return;
    case State::kFlags:
      if (!varint_.feed(byte, &value)) return;
      if (value != 0) throw FormatError("delta uses unsupported flags");
      state_ = State::kChunkSize;
      return;
    case State::kChunkSize:
      if (!varint_.feed(byte, &value)) return;
      header_.chunk_size = value;
      state_ = State::kBasisSize;
      return;
    case State::kBasisSize:
      if (!varint_.feed(byte, &value)) return;
      header_.basis_size = value;
      state_ = State::kStartOffset;
      return;
    case State::kStartOffset:
      if (!varint_.feed(byte, &value)) return;
      header_.start_offset = value;
      validate(header_);
      position_ = header_.start_offset;
      chunk_start_ = header_.start_offset;
      state_ = State::kRecordType;
      sink_.on_header(header_);
      return;
    case State::kRecordType:
      begin_record(byte);
      return;
    case State::kCopyOffset:
      if (!varint_.feed(byte, &value)) return;
      pending_value_ = value;
      state_ = State::kCopyLength;
      return;
    case State::kCopyLength: {
      if (!varint_.feed(byte, &value)) return;
      const std::uint64_t offset = pending_value_;
      if (offset > header_.basis_size || value > header_.basis_size - offset) {
        throw FormatError("COPY of " + std::to_string(value) + " bytes at offset " +
                          std::to_string(offset) + " is outside the " +
                          std::to_string(header_.basis_size) + "-byte basis");
      }
      require_operation_allowed(value, "COPY");
      position_ += value;
      state_ = State::kRecordType;
      sink_.on_copy(offset, value);
      return;
    }
    case State::kLiteralLength:
      if (!varint_.feed(byte, &value)) return;
      require_operation_allowed(value, "LITERAL");
      position_ += value;
      literal_left_ = value;
      state_ = State::kLiteralData;
      return;
    case State::kChunkDigest: {
      staging_[staged_++] = byte;
      if (staged_ < Sha256::kDigestLength) return;
      staged_ = 0;
      Sha256Digest digest;
      std::memcpy(digest.data(), staging_, digest.size());
      if (position_ - chunk_start_ < header_.chunk_size) final_chunk_seen_ = true;
      chunk_start_ = position_;
      state_ = State::kRecordType;
      sink_.on_chunk(digest);
      return;
    }
    case State::kEndLength:
      if (!varint_.feed(byte, &value)) return;
      if (value != position_) {
        throw FormatError("END declares " + std::to_string(value) +
                          " bytes of output but the records produce " +
                          std::to_string(position_));
      }
      pending_value_ = value;
      state_ = State::kEndDigest;
      return;
    case State::kEndDigest: {
      staging_[staged_++] = byte;
      if (staged_ < Sha256::kDigestLength) return;
      staged_ = 0;
      Sha256Digest digest;
      std::memcpy(digest.data(), staging_, digest.size());
      state_ = State::kDone;
      sink_.on_end(pending_value_, digest);
      return;
    }
    case State::kLiteralData:
    case State::kDone:
      break;
  }
  throw std::logic_error("DeltaDecoder: byte consumed in an unexpected state");
}

void DeltaDecoder::begin_record(std::uint8_t type) {
  switch (static_cast<DeltaRecord>(type)) {
    case DeltaRecord::kCopy:
      state_ = State::kCopyOffset;
      return;
    case DeltaRecord::kLiteral:
      state_ = State::kLiteralLength;
      return;
    case DeltaRecord::kChunk:
      if (position_ == chunk_start_) throw FormatError("CHUNK record covers no output");
      state_ = State::kChunkDigest;
      return;
    case DeltaRecord::kEnd:
      if (position_ != chunk_start_) {
        throw FormatError("END follows " + std::to_string(position_ - chunk_start_) +
                          " bytes of output not covered by a CHUNK record");
      }
      state_ = State::kEndLength;
      return;
  }
  throw FormatError("unknown delta record type " + hex_byte(type));
}

void DeltaDecoder::require_operation_allowed(std::uint64_t length, const char* what) const {
  if (length == 0) throw FormatError(std::string("empty ") + what + " record");
  if (final_chunk_seen_) {
    throw FormatError(std::string(what) + " record after the final, short chunk");
  }
  if (length > chunk_room()) {
    throw FormatError(std::string(what) + " record of " + std::to_string(length) +
                      " bytes crosses a chunk boundary");
  }
  if (length > std::numeric_limits<std::uint64_t>::max() - position_) {
    throw FormatError("delta output would exceed 2^64 bytes");
  }
}

std::uint64_t DeltaDecoder::chunk_room() const {
  return header_.chunk_size - (position_ - chunk_start_);
}

}  // namespace deltasync
