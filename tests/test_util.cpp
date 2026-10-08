#include "test_util.h"

#include <stdlib.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <stdexcept>
#include <system_error>

namespace deltasync::test {

std::uint64_t Rng::next() {
  std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

void Rng::fill(std::uint8_t* out, std::size_t length) {
  while (length >= 8) {
    const std::uint64_t value = next();
    std::memcpy(out, &value, 8);
    out += 8;
    length -= 8;
  }
  if (length > 0) {
    const std::uint64_t value = next();
    std::memcpy(out, &value, length);
  }
}

Bytes random_bytes(std::size_t length, std::uint64_t seed) {
  Bytes out(length);
  Rng(seed).fill(out.data(), out.size());
  return out;
}

Bytes bytes_of(const std::string& text) { return Bytes(text.begin(), text.end()); }

::testing::AssertionResult same_bytes(const Bytes& expected, const Bytes& actual) {
  const auto mismatch = std::mismatch(expected.begin(), expected.end(), actual.begin(), actual.end());
  if (mismatch.first == expected.end() && mismatch.second == actual.end()) {
    return ::testing::AssertionSuccess();
  }
  const auto offset = static_cast<std::size_t>(mismatch.first - expected.begin());
  auto failure = ::testing::AssertionFailure();
  failure << "expected " << expected.size() << " bytes, got " << actual.size()
          << "; first difference at offset " << offset;
  if (offset < expected.size() && offset < actual.size()) {
    failure << " (expected 0x" << std::hex << int{expected[offset]} << ", got 0x"
            << int{actual[offset]} << std::dec << ")";
  }
  return failure;
}

Signature make_signature(const Bytes& basis, std::uint32_t block_size,
                         std::uint32_t strong_length) {
  SignatureHeader header;
  header.block_size = block_size;
  header.strong_length = strong_length;
  header.basis_size = basis.size();
  SignatureBuilder builder;
  SignatureGenerator generator(header, builder);
  generator.feed(basis.data(), basis.size());
  generator.finish();
  return builder.take();
}

Bytes serialize(const Signature& signature) {
  MemorySink sink;
  SignatureWriter writer(sink);
  signature.replay(writer);
  return sink.take();
}

EncodedDelta encode(const Signature& signature, const Bytes& target,
                    const DeltaOptions& options, std::size_t feed_piece) {
  MemorySink sink;
  DeltaWriter writer(sink);
  DeltaEncoder encoder(signature, writer, options);
  if (feed_piece == 0) {
    encoder.feed(target.data(), target.size());
  } else {
    for (std::size_t offset = 0; offset < target.size(); offset += feed_piece) {
      encoder.feed(target.data() + offset, std::min(feed_piece, target.size() - offset));
    }
  }
  encoder.finish();
  return EncodedDelta{sink.take(), encoder.stats()};
}

Bytes apply(const Bytes& basis, const Bytes& delta, PatchStats* stats) {
  MemoryRandomAccess source(basis);
  MemorySource delta_source(delta);
  MemorySink output;
  const PatchStats result = apply_delta(source, delta_source, output);
  if (stats != nullptr) *stats = result;
  return output.take();
}

RoundTrip round_trip(const Bytes& basis, const Bytes& target, std::uint32_t block_size,
                     const DeltaOptions& options) {
  RoundTrip result;
  const Bytes signature_bytes = serialize(make_signature(basis, block_size));
  result.signature_bytes = signature_bytes.size();
  MemorySource signature_source(signature_bytes);
  const Signature signature = read_signature(signature_source);
  EncodedDelta delta = encode(signature, target, options);
  result.delta = std::move(delta.bytes);
  result.delta_stats = delta.stats;
  result.output = apply(basis, result.delta, &result.patch_stats);
  return result;
}

bool Record::operator==(const Record& other) const {
  if (kind != other.kind) return false;
  switch (kind) {
    case Kind::kHeader:
      return header.chunk_size == other.header.chunk_size &&
             header.basis_size == other.header.basis_size &&
             header.start_offset == other.header.start_offset;
    case Kind::kCopy:
      return offset == other.offset && length == other.length;
    case Kind::kLiteral:
      return data == other.data;
    case Kind::kChunk:
      return digest == other.digest;
    case Kind::kEnd:
      return length == other.length && digest == other.digest;
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, const Record& record) {
  switch (record.kind) {
    case Record::Kind::kHeader:
      return out << "HEADER(chunk_size=" << record.header.chunk_size
                 << ", basis_size=" << record.header.basis_size
                 << ", start_offset=" << record.header.start_offset << ")";
    case Record::Kind::kCopy:
      return out << "COPY(" << record.offset << ", " << record.length << ")";
    case Record::Kind::kLiteral:
      return out << "LITERAL(" << record.data.size() << " bytes)";
    case Record::Kind::kChunk:
      return out << "CHUNK(" << to_hex(record.digest).substr(0, 16) << "...)";
    case Record::Kind::kEnd:
      return out << "END(" << record.length << ", " << to_hex(record.digest).substr(0, 16)
                 << "...)";
  }
  return out;
}

void RecordingSink::on_header(const DeltaHeader& header) {
  Record record;
  record.kind = Record::Kind::kHeader;
  record.header = header;
  records.push_back(record);
}

void RecordingSink::on_copy(std::uint64_t offset, std::uint64_t length) {
  Record record;
  record.kind = Record::Kind::kCopy;
  record.offset = offset;
  record.length = length;
  records.push_back(record);
}

void RecordingSink::on_literal(const std::uint8_t* data, std::size_t length) {
  if (merge_literals && !records.empty() && records.back().kind == Record::Kind::kLiteral) {
    records.back().data.insert(records.back().data.end(), data, data + length);
    records.back().length += length;
    return;
  }
  Record record;
  record.kind = Record::Kind::kLiteral;
  record.data.assign(data, data + length);
  record.length = length;
  records.push_back(record);
}

void RecordingSink::on_chunk(const Sha256Digest& digest) {
  Record record;
  record.kind = Record::Kind::kChunk;
  record.digest = digest;
  records.push_back(record);
}

void RecordingSink::on_end(std::uint64_t total_length, const Sha256Digest& digest) {
  Record record;
  record.kind = Record::Kind::kEnd;
  record.length = total_length;
  record.digest = digest;
  records.push_back(record);
}

void RecordingSink::replay(DeltaSink& sink) const {
  for (const Record& record : records) {
    switch (record.kind) {
      case Record::Kind::kHeader:
        sink.on_header(record.header);
        break;
      case Record::Kind::kCopy:
        sink.on_copy(record.offset, record.length);
        break;
      case Record::Kind::kLiteral:
        sink.on_literal(record.data.data(), record.data.size());
        break;
      case Record::Kind::kChunk:
        sink.on_chunk(record.digest);
        break;
      case Record::Kind::kEnd:
        sink.on_end(record.length, record.digest);
        break;
    }
  }
}

TempDir::TempDir() {
  const char* base = std::getenv("TMPDIR");
  std::string pattern = (base != nullptr && *base != '\0') ? base : "/tmp";
  if (pattern.back() != '/') pattern.push_back('/');
  pattern += "deltasync-test-XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  if (::mkdtemp(buffer.data()) == nullptr) {
    throw std::system_error(errno, std::generic_category(), "mkdtemp " + pattern);
  }
  path_ = buffer.data();
}

TempDir::~TempDir() {
  std::error_code ignored;
  std::filesystem::remove_all(path_, ignored);
}

void write_file(const std::string& path, const Bytes& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!out) throw std::runtime_error("cannot write " + path);
}

Bytes read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path);
  return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool file_exists(const std::string& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored);
}

std::vector<std::string> list_directory(const std::string& directory) {
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace deltasync::test
