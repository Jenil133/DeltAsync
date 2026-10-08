#include "deltasync/patch.h"

#include <sys/stat.h>

#include <algorithm>
#include <string>

#include "deltasync/error.h"
#include "deltasync/file.h"

namespace deltasync {

Patcher::Patcher(RandomAccessSource& basis, ByteSink& output)
    : Patcher(basis, output, Sha256()) {}

Patcher::Patcher(RandomAccessSource& basis, ByteSink& output, const Sha256& prefix)
    : basis_(basis),
      output_(output),
      file_hash_(prefix),
      position_(prefix.length()),
      chunk_start_(prefix.length()),
      copy_buffer_(kIoBufferSize) {}

void Patcher::on_header(const DeltaHeader& header) {
  if (header_seen_) throw FormatError("delta has two headers");
  validate(header);
  if (header.basis_size != basis_.size()) {
    throw IntegrityError("delta was computed against a " + std::to_string(header.basis_size) +
                         "-byte basis, but the basis has " + std::to_string(basis_.size()) +
                         " bytes");
  }
  if (header.start_offset != position_) {
    throw IntegrityError("delta starts at target offset " +
                         std::to_string(header.start_offset) + " but the output is at " +
                         std::to_string(position_));
  }
  header_seen_ = true;
}

void Patcher::on_copy(std::uint64_t offset, std::uint64_t length) {
  require_operation("COPY");
  if (offset > basis_.size() || length > basis_.size() - offset) {
    throw FormatError("COPY of " + std::to_string(length) + " bytes at offset " +
                      std::to_string(offset) + " is outside the basis");
  }
  ++stats_.copy_ops;
  stats_.copied_bytes += length;
  while (length > 0) {
    const auto take = static_cast<std::size_t>(
        std::min<std::uint64_t>(length, copy_buffer_.size()));
    basis_.read_at(offset, copy_buffer_.data(), take);
    write_output(copy_buffer_.data(), take);
    offset += take;
    length -= take;
  }
}

void Patcher::on_literal(const std::uint8_t* data, std::size_t length) {
  require_operation("LITERAL");
  stats_.literal_bytes += length;
  write_output(data, length);
}

void Patcher::on_chunk(const Sha256Digest& digest) {
  require_operation("CHUNK");
  if (position_ == chunk_start_) throw FormatError("CHUNK record covers no output");
  if (chunk_hash_.digest() != digest) {
    throw IntegrityError("output bytes " + std::to_string(chunk_start_) + ".." +
                         std::to_string(position_) +
                         " do not match the sender's checksum (wrong basis or corrupted delta)");
  }
  chunk_hash_.reset();
  chunk_start_ = position_;
  ++stats_.chunks_verified;
  if (chunk_callback_) chunk_callback_(position_);
}

void Patcher::on_end(std::uint64_t total_length, const Sha256Digest& digest) {
  require_operation("END");
  if (position_ != chunk_start_) {
    throw FormatError("delta ended with output that no CHUNK record covers");
  }
  if (total_length != position_) {
    throw FormatError("END declares " + std::to_string(total_length) +
                      " bytes but the delta produced " + std::to_string(position_));
  }
  if (file_hash_.digest() != digest) {
    throw IntegrityError("SHA-256 of the rebuilt file does not match the sender's");
  }
  output_.flush();
  finished_ = true;
}

void Patcher::require_operation(const char* what) const {
  if (!header_seen_) throw FormatError(std::string(what) + " record before the delta header");
  if (finished_) throw FormatError(std::string(what) + " record after the end of the delta");
}

void Patcher::write_output(const std::uint8_t* data, std::size_t length) {
  chunk_hash_.update(data, length);
  file_hash_.update(data, length);
  output_.write(data, length);
  position_ += length;
  stats_.output_bytes += length;
}

PatchStats apply_delta(RandomAccessSource& basis, ByteSource& delta, ByteSink& output) {
  Patcher patcher(basis, output);
  DeltaDecoder decoder(patcher);
  std::vector<std::uint8_t> buffer(kIoBufferSize);
  for (;;) {
    const std::size_t count = delta.read(buffer.data(), buffer.size());
    if (count == 0) break;
    decoder.feed(buffer.data(), count);
  }
  decoder.finish();
  return patcher.stats();
}

PatchStats patch_file(const std::string& basis_path, ByteSource& delta,
                      const std::string& output_path) {
  RandomAccessFile basis(basis_path);
  AtomicFile output(output_path);
  struct stat existing;
  if (::stat(output_path.c_str(), &existing) == 0 && S_ISREG(existing.st_mode)) {
    output.set_mode(existing.st_mode);
  } else {
    output.set_mode(basis.mode());
  }
  const PatchStats stats = apply_delta(basis, delta, output);
  output.commit();
  return stats;
}

}  // namespace deltasync
