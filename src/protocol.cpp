#include "deltasync/protocol.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include "deltasync/bytes.h"
#include "deltasync/delta_format.h"

namespace deltasync {
namespace {

constexpr std::uint8_t kHelloMagic[4] = {'D', 'S', 'Y', 'N'};
constexpr std::size_t kHelloLength = 6;
constexpr std::size_t kFileInfoFixedLength = 1 + 8 + 4 + 8 + Sha256::kDigestLength + 2;
constexpr std::size_t kResumeLength = 8 + 8 * 4;
constexpr std::size_t kOkLength = 16;
constexpr std::size_t kProgressLength = 8;

bool known_frame_type(std::uint8_t type) {
  switch (static_cast<FrameType>(type)) {
    case FrameType::kHello:
    case FrameType::kFileInfo:
    case FrameType::kSignature:
    case FrameType::kResume:
    case FrameType::kData:
    case FrameType::kOk:
    case FrameType::kProgress:
    case FrameType::kError:
      return true;
  }
  return false;
}

void require_length(const Payload& payload, std::size_t expected, const char* what) {
  if (payload.size() != expected) {
    throw ProtocolError(std::string(what) + " frame has " + std::to_string(payload.size()) +
                        " payload bytes, expected " + std::to_string(expected));
  }
}

std::string error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::kProtocol:
      return "protocol error";
    case ErrorCode::kVersion:
      return "unsupported version";
    case ErrorCode::kBadRequest:
      return "bad request";
    case ErrorCode::kBusy:
      return "busy";
    case ErrorCode::kIntegrity:
      return "verification failed";
    case ErrorCode::kIo:
      return "I/O error";
    case ErrorCode::kInternal:
      return "internal error";
  }
  return "error " + std::to_string(static_cast<unsigned>(code));
}

}  // namespace

const char* frame_type_name(FrameType type) {
  switch (type) {
    case FrameType::kHello:
      return "HELLO";
    case FrameType::kFileInfo:
      return "FILE_INFO";
    case FrameType::kSignature:
      return "SIGNATURE";
    case FrameType::kResume:
      return "RESUME";
    case FrameType::kData:
      return "DATA";
    case FrameType::kOk:
      return "OK";
    case FrameType::kProgress:
      return "PROGRESS";
    case FrameType::kError:
      return "ERROR";
  }
  return "unknown";
}

const char* transfer_mode_name(TransferMode mode) {
  return mode == TransferMode::kWholeFile ? "whole-file" : "delta";
}

RemoteError::RemoteError(ErrorCode code, const std::string& message)
    : Error("peer reported " + error_code_name(code) + ": " + message), code_(code) {}

Payload encode_hello(const Hello& hello) {
  Payload out(kHelloMagic, kHelloMagic + 4);
  out.push_back(hello.version);
  out.push_back(static_cast<std::uint8_t>(hello.role));
  return out;
}

Hello decode_hello(const Payload& payload) {
  // The magic is checked before the length so that a connection from
  // something that is not deltasync at all gets a clear message.
  if (payload.size() < 4 || std::memcmp(payload.data(), kHelloMagic, 4) != 0) {
    throw ProtocolError("peer is not speaking the deltasync protocol");
  }
  require_length(payload, kHelloLength, "HELLO");
  Hello hello;
  hello.version = payload[4];
  const std::uint8_t role = payload[5];
  if (role != static_cast<std::uint8_t>(PeerRole::kSender) &&
      role != static_cast<std::uint8_t>(PeerRole::kReceiver)) {
    throw ProtocolError("HELLO has unknown role " + std::to_string(role));
  }
  hello.role = static_cast<PeerRole>(role);
  return hello;
}

Payload encode_file_info(const FileInfo& info) {
  if (info.name.size() > kMaxNameLength) throw ProtocolError("remote name is too long");
  Payload out(kFileInfoFixedLength + info.name.size());
  std::uint8_t* p = out.data();
  *p++ = static_cast<std::uint8_t>(info.mode);
  store_be64(p, info.chunk_size);
  p += 8;
  store_be32(p, info.block_size);
  p += 4;
  store_be64(p, info.size);
  p += 8;
  std::memcpy(p, info.digest.data(), info.digest.size());
  p += info.digest.size();
  p[0] = static_cast<std::uint8_t>(info.name.size() >> 8);
  p[1] = static_cast<std::uint8_t>(info.name.size());
  p += 2;
  std::memcpy(p, info.name.data(), info.name.size());
  return out;
}

FileInfo decode_file_info(const Payload& payload) {
  if (payload.size() < kFileInfoFixedLength) {
    throw ProtocolError("FILE_INFO frame is too short");
  }
  const std::uint8_t* p = payload.data();
  FileInfo info;
  const std::uint8_t mode = *p++;
  if (mode != static_cast<std::uint8_t>(TransferMode::kDelta) &&
      mode != static_cast<std::uint8_t>(TransferMode::kWholeFile)) {
    throw ProtocolError("FILE_INFO has unknown mode " + std::to_string(mode));
  }
  info.mode = static_cast<TransferMode>(mode);
  info.chunk_size = load_be64(p);
  p += 8;
  info.block_size = load_be32(p);
  p += 4;
  info.size = load_be64(p);
  p += 8;
  std::memcpy(info.digest.data(), p, info.digest.size());
  p += info.digest.size();
  const std::size_t name_length = (std::size_t{p[0]} << 8) | p[1];
  p += 2;
  require_length(payload, kFileInfoFixedLength + name_length, "FILE_INFO");
  info.name.assign(reinterpret_cast<const char*>(p), name_length);
  return info;
}

Payload encode_resume(const ResumePoint& resume) {
  Payload out(kResumeLength);
  store_be64(out.data(), resume.offset);
  for (std::size_t i = 0; i < 8; ++i) store_be32(out.data() + 8 + 4 * i, resume.prefix.words[i]);
  return out;
}

ResumePoint decode_resume(const Payload& payload) {
  require_length(payload, kResumeLength, "RESUME");
  ResumePoint resume;
  resume.offset = load_be64(payload.data());
  for (std::size_t i = 0; i < 8; ++i) {
    resume.prefix.words[i] = load_be32(payload.data() + 8 + 4 * i);
  }
  resume.prefix.length = resume.offset;
  return resume;
}

Payload encode_ok(const OkInfo& ok) {
  Payload out(kOkLength);
  store_be64(out.data(), ok.receiver_bytes_received);
  store_be64(out.data() + 8, ok.receiver_bytes_sent);
  return out;
}

OkInfo decode_ok(const Payload& payload) {
  require_length(payload, kOkLength, "OK");
  OkInfo ok;
  ok.receiver_bytes_received = load_be64(payload.data());
  ok.receiver_bytes_sent = load_be64(payload.data() + 8);
  return ok;
}

Payload encode_progress(const ProgressInfo& progress) {
  Payload out(kProgressLength);
  store_be64(out.data(), progress.position);
  return out;
}

ProgressInfo decode_progress(const Payload& payload) {
  require_length(payload, kProgressLength, "PROGRESS");
  ProgressInfo progress;
  progress.position = load_be64(payload.data());
  return progress;
}

Payload encode_error(const ErrorInfo& error) {
  Payload out;
  out.push_back(static_cast<std::uint8_t>(error.code));
  const std::size_t length = std::min(error.message.size(), kMaxErrorMessage);
  out.insert(out.end(), error.message.begin(),
             error.message.begin() + static_cast<std::ptrdiff_t>(length));
  return out;
}

ErrorInfo decode_error(const Payload& payload) {
  if (payload.empty()) throw ProtocolError("ERROR frame is empty");
  if (payload.size() > 1 + kMaxErrorMessage) throw ProtocolError("ERROR message is too long");
  ErrorInfo error;
  error.code = static_cast<ErrorCode>(payload[0]);
  // The text is only ever displayed; replace bytes that could mess up a
  // terminal rather than trusting the peer.
  for (std::size_t i = 1; i < payload.size(); ++i) {
    const auto c = static_cast<char>(payload[i]);
    error.message.push_back(payload[i] < 0x20 || payload[i] == 0x7f ? '?' : c);
  }
  return error;
}

void validate_remote_name(const std::string& name) {
  if (name.empty()) throw ProtocolError("remote name is empty");
  if (name.size() > kMaxNameLength) {
    throw ProtocolError("remote name is longer than " + std::to_string(kMaxNameLength) +
                        " bytes");
  }
  if (name[0] == '.') throw ProtocolError("remote name must not start with '.'");
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '/') throw ProtocolError("remote name must not contain '/'");
    if (byte < 0x20 || byte == 0x7f) {
      throw ProtocolError("remote name must not contain control characters");
    }
  }
}

void validate_chunk_size(std::uint64_t chunk_size) {
  if (chunk_size == 0 || chunk_size > kMaxChunkSize || chunk_size % Sha256::kBlockLength != 0) {
    throw ProtocolError("chunk size " + std::to_string(chunk_size) +
                        " is not a positive multiple of " +
                        std::to_string(Sha256::kBlockLength) + " up to " +
                        std::to_string(kMaxChunkSize));
  }
}

void FrameChannel::send(FrameType type, const std::uint8_t* payload, std::size_t length) {
  if (length > kMaxFramePayload) throw std::logic_error("frame payload too large");
  // Small frames go out in one write; there is no need to copy big ones.
  std::uint8_t header[kFrameHeaderLength];
  header[0] = static_cast<std::uint8_t>(type);
  store_be32(header + 1, static_cast<std::uint32_t>(length));
  if (length <= 4096) {
    std::uint8_t frame[kFrameHeaderLength + 4096];
    std::memcpy(frame, header, kFrameHeaderLength);
    if (length > 0) std::memcpy(frame + kFrameHeaderLength, payload, length);
    socket_.write_all(frame, kFrameHeaderLength + length);
    return;
  }
  socket_.write_all(header, kFrameHeaderLength);
  socket_.write_all(payload, length);
}

void FrameChannel::send_with_header_room(FrameType type, std::uint8_t* buffer,
                                         std::size_t payload_length) {
  if (payload_length > kMaxFramePayload) throw std::logic_error("frame payload too large");
  buffer[0] = static_cast<std::uint8_t>(type);
  store_be32(buffer + 1, static_cast<std::uint32_t>(payload_length));
  socket_.write_all(buffer, kFrameHeaderLength + payload_length);
}

FrameType FrameChannel::receive_header() {
  if (remaining_ != 0) throw std::logic_error("the previous frame's payload was not read");
  std::uint8_t header[kFrameHeaderLength];
  socket_.read_exact(header, kFrameHeaderLength);
  if (!known_frame_type(header[0])) {
    throw ProtocolError("unknown frame type " + std::to_string(header[0]));
  }
  const std::uint32_t length = load_be32(header + 1);
  if (length > kMaxFramePayload) {
    throw ProtocolError("frame of " + std::to_string(length) + " bytes exceeds the limit of " +
                        std::to_string(kMaxFramePayload));
  }
  remaining_ = length;
  return static_cast<FrameType>(header[0]);
}

std::size_t FrameChannel::read_payload_some(std::uint8_t* out, std::size_t capacity) {
  const std::size_t want = std::min<std::size_t>(capacity, remaining_);
  if (want == 0) return 0;
  const std::size_t count = socket_.read_some(out, want);
  if (count == 0) {
    throw NetError("connection closed by " + socket_.peer() + " in the middle of a frame");
  }
  remaining_ -= static_cast<std::uint32_t>(count);
  return count;
}

void FrameChannel::read_payload() {
  payload_.resize(remaining_);
  socket_.read_exact(payload_.data(), payload_.size());
  remaining_ = 0;
}

FrameType FrameChannel::receive() {
  const FrameType type = receive_header();
  read_payload();
  return type;
}

void throw_remote_error(const Payload& payload) {
  const ErrorInfo error = decode_error(payload);
  throw RemoteError(error.code, error.message);
}

void FrameChannel::expect(FrameType expected) {
  const FrameType type = receive();
  if (type == expected) return;
  if (type == FrameType::kError) throw_remote_error(payload_);
  throw ProtocolError(std::string("expected ") + frame_type_name(expected) + ", received " +
                      frame_type_name(type));
}

void FrameChannel::expect_skipping_progress(FrameType expected) {
  FrameType type = receive();
  while (type == FrameType::kProgress && expected != FrameType::kProgress) {
    decode_progress(payload_);
    type = receive();
  }
  if (type == expected) return;
  if (type == FrameType::kError) throw_remote_error(payload_);
  throw ProtocolError(std::string("expected ") + frame_type_name(expected) + ", received " +
                      frame_type_name(type));
}

FrameSink::FrameSink(FrameChannel& channel, FrameType type)
    : channel_(channel), type_(type), buffer_(kFrameHeaderLength + kStreamFramePayload) {}

void FrameSink::write(const std::uint8_t* data, std::size_t length) {
  while (length > 0) {
    const std::size_t take = std::min(length, kStreamFramePayload - used_);
    std::memcpy(buffer_.data() + kFrameHeaderLength + used_, data, take);
    used_ += take;
    data += take;
    length -= take;
    if (used_ == kStreamFramePayload) send_frame();
  }
}

void FrameSink::flush() {
  if (used_ > 0) send_frame();
}

void FrameSink::send_frame() {
  while (check_peer_ && channel_.socket().readable(0)) {
    // The peer has nothing to say in the middle of a stream except ERROR
    // and, from a receiver, PROGRESS (or closing the connection, which
    // receive() reports).
    const FrameType type = channel_.receive();
    if (type == FrameType::kProgress && skip_progress_) {
      decode_progress(channel_.payload());
      continue;
    }
    if (type == FrameType::kError) throw_remote_error(channel_.payload());
    throw ProtocolError(std::string("unexpected ") + frame_type_name(type) +
                        " frame from the peer");
  }
  channel_.send_with_header_room(type_, buffer_.data(), used_);
  payload_bytes_ += used_;
  used_ = 0;
}

}  // namespace deltasync
