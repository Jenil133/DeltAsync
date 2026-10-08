#include "deltasync/signature.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include "deltasync/error.h"
#include "deltasync/rolling_checksum.h"
#include "deltasync/sha256.h"

namespace deltasync {
namespace {

constexpr std::uint8_t kSignatureMagic[4] = {'D', 'S', 'I', 'G'};

// The header is untrusted until blocks actually arrive, so memory is
// reserved for at most this many blocks up front and grows from there.
constexpr std::uint64_t kMaxBlocksReservedUpFront = 1 << 20;

}  // namespace

std::uint32_t default_block_size(std::uint64_t basis_size) {
  constexpr std::uint64_t kLargest = kMaxDefaultBlockSize;
  if (basis_size >= kLargest * kLargest) return kMaxDefaultBlockSize;
  // basis_size < 2^34 here, so the root is below 2^17 and the squares below
  // cannot overflow. The loops correct any rounding in the floating-point
  // estimate.
  auto root = static_cast<std::uint64_t>(std::sqrt(static_cast<double>(basis_size)));
  while (root * root > basis_size) --root;
  while ((root + 1) * (root + 1) <= basis_size) ++root;
  const std::uint64_t rounded = (root + 1023) / 1024 * 1024;
  return static_cast<std::uint32_t>(
      std::clamp<std::uint64_t>(rounded, kMinDefaultBlockSize, kMaxDefaultBlockSize));
}

std::uint64_t SignatureHeader::block_count() const {
  if (block_size == 0) return 0;
  return basis_size / block_size + (basis_size % block_size != 0 ? 1 : 0);
}

std::uint32_t SignatureHeader::block_length(std::uint64_t index) const {
  const std::uint64_t start = index * block_size;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size, basis_size - start));
}

void validate(const SignatureHeader& header) {
  if (header.block_size < kMinBlockSize || header.block_size > kMaxBlockSize) {
    throw FormatError("block size " + std::to_string(header.block_size) + " is outside [" +
                      std::to_string(kMinBlockSize) + ", " + std::to_string(kMaxBlockSize) +
                      "]");
  }
  if (header.strong_length < kMinStrongLength || header.strong_length > kMaxStrongLength) {
    throw FormatError("strong checksum length " + std::to_string(header.strong_length) +
                      " is outside [" + std::to_string(kMinStrongLength) + ", " +
                      std::to_string(kMaxStrongLength) + "]");
  }
  if (header.block_count() > kMaxBlockCount) {
    throw FormatError("a " + std::to_string(header.basis_size) + "-byte basis with " +
                      std::to_string(header.block_size) + "-byte blocks has more than " +
                      std::to_string(kMaxBlockCount) + " blocks; use a larger block size");
  }
}

SignatureGenerator::SignatureGenerator(const SignatureHeader& header, SignatureSink& sink)
    : header_(header), sink_(sink) {
  validate(header_);
  block_.resize(static_cast<std::size_t>(
      std::min<std::uint64_t>(header_.block_size, header_.basis_size)));
  sink_.on_header(header_);
}

void SignatureGenerator::feed(const std::uint8_t* data, std::size_t length) {
  if (finished_) throw std::logic_error("SignatureGenerator::feed after finish");
  if (length == 0) return;
  if (length > header_.basis_size - fed_) {
    throw Error("basis has more than the expected " + std::to_string(header_.basis_size) +
                " bytes (did it change while being read?)");
  }
  fed_ += length;

  const std::size_t block_size = header_.block_size;
  if (buffered_ > 0) {
    const std::size_t take = std::min(length, block_size - buffered_);
    std::memcpy(block_.data() + buffered_, data, take);
    buffered_ += take;
    data += take;
    length -= take;
    if (buffered_ < block_size) return;
    emit_block(block_.data(), block_size);
    buffered_ = 0;
  }
  // Whole blocks are hashed in place; only a trailing fragment is copied.
  for (; length >= block_size; data += block_size, length -= block_size) {
    emit_block(data, block_size);
  }
  if (length > 0) {
    std::memcpy(block_.data(), data, length);
    buffered_ = length;
  }
}

void SignatureGenerator::finish() {
  if (finished_) throw std::logic_error("SignatureGenerator::finish called twice");
  if (fed_ != header_.basis_size) {
    throw Error("basis ended after " + std::to_string(fed_) + " of the expected " +
                std::to_string(header_.basis_size) + " bytes (did it change while being read?)");
  }
  if (buffered_ > 0) emit_block(block_.data(), buffered_);
  buffered_ = 0;
  finished_ = true;
  sink_.on_end();
}

void SignatureGenerator::emit_block(const std::uint8_t* data, std::size_t length) {
  const std::uint32_t weak = RollingChecksum::compute(data, length);
  const Sha256Digest strong = Sha256::hash(data, length);
  sink_.on_block(weak, strong.data());
}

void generate_signature(ByteSource& basis, const SignatureHeader& header,
                        SignatureSink& sink) {
  SignatureGenerator generator(header, sink);
  std::vector<std::uint8_t> buffer(kIoBufferSize);
  for (;;) {
    const std::size_t count = basis.read(buffer.data(), buffer.size());
    if (count == 0) break;
    generator.feed(buffer.data(), count);
  }
  generator.finish();
}

Signature::Signature(const SignatureHeader& header) : header_(header) {
  validate(header_);
  const auto reserve = static_cast<std::size_t>(
      std::min(header_.block_count(), kMaxBlocksReservedUpFront));
  weak_.reserve(reserve);
  strong_.reserve(reserve * header_.strong_length);
}

void Signature::add_block(std::uint32_t weak, const std::uint8_t* strong) {
  if (weak_.size() >= header_.block_count()) {
    throw FormatError("signature has more blocks than its header declares");
  }
  weak_.push_back(weak);
  strong_.insert(strong_.end(), strong, strong + header_.strong_length);
}

void Signature::replay(SignatureSink& sink) const {
  if (!complete()) throw std::logic_error("Signature::replay of an incomplete signature");
  sink.on_header(header_);
  for (std::uint32_t i = 0; i < block_count(); ++i) sink.on_block(weak_[i], strong(i));
  sink.on_end();
}

void SignatureBuilder::on_header(const SignatureHeader& header) {
  if (started_) throw FormatError("signature has two headers");
  signature_ = Signature(header);
  started_ = true;
}

void SignatureBuilder::on_block(std::uint32_t weak, const std::uint8_t* strong) {
  if (!started_ || done_) throw FormatError("signature block outside a signature");
  signature_.add_block(weak, strong);
}

void SignatureBuilder::on_end() {
  if (!started_ || done_ || !signature_.complete()) {
    throw FormatError("signature ended after " + std::to_string(signature_.block_count()) +
                      " of " + std::to_string(signature_.header().block_count()) + " blocks");
  }
  done_ = true;
}

Signature SignatureBuilder::take() {
  if (!done_) throw std::logic_error("SignatureBuilder::take before the signature ended");
  return std::move(signature_);
}

void SignatureWriter::on_header(const SignatureHeader& header) {
  validate(header);
  std::uint8_t buffer[sizeof(kSignatureMagic) + 1 + 4 * kMaxVarintLength];
  std::size_t used = 0;
  std::memcpy(buffer, kSignatureMagic, sizeof(kSignatureMagic));
  used += sizeof(kSignatureMagic);
  buffer[used++] = kSignatureVersion;
  used += encode_varint(0, buffer + used);  // flags
  used += encode_varint(header.block_size, buffer + used);
  used += encode_varint(header.strong_length, buffer + used);
  used += encode_varint(header.basis_size, buffer + used);
  write(buffer, used);
  strong_length_ = header.strong_length;
}

void SignatureWriter::on_block(std::uint32_t weak, const std::uint8_t* strong) {
  std::uint8_t entry[4 + kMaxStrongLength];
  store_be32(entry, weak);
  std::memcpy(entry + 4, strong, strong_length_);
  write(entry, 4 + strong_length_);
}

void SignatureWriter::on_end() { out_.flush(); }

void SignatureWriter::write(const std::uint8_t* data, std::size_t length) {
  out_.write(data, length);
  bytes_written_ += length;
}

void SignatureDecoder::feed(const std::uint8_t* data, std::size_t length) {
  while (length > 0) {
    if (state_ == State::kDone) {
      throw FormatError("unexpected data after the end of the signature");
    }
    if (state_ != State::kBlocks) {
      consume_header_byte(*data);
      ++data;
      --length;
      continue;
    }
    const std::size_t entry_size = 4 + header_.strong_length;
    if (staged_ == 0) {
      // Fast path: parse whole entries straight from the input.
      while (state_ == State::kBlocks && length >= entry_size) {
        consume_block(data);
        data += entry_size;
        length -= entry_size;
      }
      if (state_ != State::kBlocks || length == 0) continue;
    }
    const std::size_t take = std::min(length, entry_size - staged_);
    std::memcpy(staging_ + staged_, data, take);
    staged_ += take;
    data += take;
    length -= take;
    if (staged_ == entry_size) {
      staged_ = 0;
      consume_block(staging_);
    }
  }
}

void SignatureDecoder::finish() {
  if (state_ == State::kDone) return;
  if (state_ == State::kBlocks) {
    throw FormatError("signature is truncated: " + std::to_string(blocks_left_) + " of " +
                      std::to_string(header_.block_count()) + " blocks missing");
  }
  throw FormatError("signature is truncated inside its header");
}

void SignatureDecoder::consume_header_byte(std::uint8_t byte) {
  std::uint64_t value = 0;
  switch (state_) {
    case State::kMagic:
      if (byte != kSignatureMagic[staged_]) {
        throw FormatError("not a deltasync signature (bad magic number)");
      }
      if (++staged_ == sizeof(kSignatureMagic)) {
        staged_ = 0;
        state_ = State::kVersion;
      }
      return;
    case State::kVersion:
      if (byte != kSignatureVersion) {
        throw FormatError("unsupported signature version " + std::to_string(byte));
      }
      state_ = State::kFlags;
      return;
    case State::kFlags:
      if (!varint_.feed(byte, &value)) return;
      if (value != 0) throw FormatError("signature uses unsupported flags");
      state_ = State::kBlockSize;
      return;
    case State::kBlockSize:
      if (!varint_.feed(byte, &value)) return;
      if (value < kMinBlockSize || value > kMaxBlockSize) {
        throw FormatError("signature block size " + std::to_string(value) + " is out of range");
      }
      header_.block_size = static_cast<std::uint32_t>(value);
      state_ = State::kStrongLength;
      return;
    case State::kStrongLength:
      if (!varint_.feed(byte, &value)) return;
      if (value < kMinStrongLength || value > kMaxStrongLength) {
        throw FormatError("signature strong checksum length " + std::to_string(value) +
                          " is out of range");
      }
      header_.strong_length = static_cast<std::uint32_t>(value);
      state_ = State::kBasisSize;
      return;
    case State::kBasisSize:
      if (!varint_.feed(byte, &value)) return;
      header_.basis_size = value;
      finish_header();
      return;
    case State::kBlocks:
    case State::kDone:
      break;
  }
  throw std::logic_error("SignatureDecoder: header byte in a non-header state");
}

void SignatureDecoder::finish_header() {
  validate(header_);
  blocks_left_ = header_.block_count();
  state_ = State::kBlocks;
  sink_.on_header(header_);
  if (blocks_left_ == 0) {
    state_ = State::kDone;
    sink_.on_end();
  }
}

void SignatureDecoder::consume_block(const std::uint8_t* entry) {
  sink_.on_block(load_be32(entry), entry + 4);
  if (--blocks_left_ == 0) {
    state_ = State::kDone;
    sink_.on_end();
  }
}

Signature read_signature(ByteSource& source) {
  SignatureBuilder builder;
  SignatureDecoder decoder(builder);
  std::vector<std::uint8_t> buffer(kIoBufferSize);
  for (;;) {
    const std::size_t count = source.read(buffer.data(), buffer.size());
    if (count == 0) break;
    decoder.feed(buffer.data(), count);
  }
  decoder.finish();
  return builder.take();
}

}  // namespace deltasync
