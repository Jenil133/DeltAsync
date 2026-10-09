#ifndef DELTASYNC_PROTOCOL_H_
#define DELTASYNC_PROTOCOL_H_

// The deltasync wire protocol.
//
// Everything on the connection is a frame: type (1 byte) | payload length
// (4 bytes, big-endian) | payload. Fixed-width integers inside payloads are
// big-endian. A conversation, sender (holds the new version) on the left:
//
//   HELLO(role sender)        ->
//   FILE_INFO                 ->
//                             <-  HELLO(role receiver)
//                             <-  SIGNATURE * n   (serialized signature of the
//                                                  basis; n = 0 without one)
//                             <-  RESUME          (also ends the signature)
//   DATA * m                  ->  (serialized delta, start_offset = resume
//                                  offset, or 0 if the sender declined it)
//                             <-  OK | ERROR
//
// Either side may send ERROR instead of its next frame and then close. The
// sender sends HELLO and FILE_INFO without waiting, so a sync costs two
// round trips plus the transfer itself.
//
// Both sides give up on a peer that stays silent for longer than their
// inactivity timeout, so neither may go quiet for long while it works. The
// sender ends a DATA frame at every CHUNK record and the receiver ends a
// SIGNATURE frame every kSignatureFlushBytes of basis. Work that produces no
// frame of its own (checking resumable partial output before RESUME,
// rebuilding and syncing the target while the sender waits for OK) is
// covered by PROGRESS frames, which the receiver may send at any point after
// its HELLO and the sender skips.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "deltasync/error.h"
#include "deltasync/io.h"
#include "deltasync/net.h"
#include "deltasync/sha256.h"

namespace deltasync {

constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::size_t kFrameHeaderLength = 5;
// Largest payload a peer may announce; anything longer is rejected before
// any of it is buffered.
constexpr std::uint32_t kMaxFramePayload = 1u << 20;
// Payload size used for SIGNATURE and DATA frames.
constexpr std::size_t kStreamFramePayload = 256 * 1024;
// The receiver ends a SIGNATURE frame after this many bytes of basis even
// if the frame is not full, so the sender hears from it while it hashes a
// large basis.
constexpr std::uint64_t kSignatureFlushBytes = 8u << 20;
// Longest remote file name: the usual 255-byte limit of one path component,
// less the 13 bytes the receiver adds for resumable output, whose longest
// file is ".<name>.journal.tmp" (see resume.h).
constexpr std::size_t kMaxNameLength = 242;
// Longest ERROR message accepted or sent.
constexpr std::size_t kMaxErrorMessage = 1024;

enum class FrameType : std::uint8_t {
  kHello = 0x01,
  kFileInfo = 0x02,
  kSignature = 0x03,
  kResume = 0x04,
  kData = 0x05,
  kOk = 0x06,
  kProgress = 0x07,
  kError = 0x7f,
};

const char* frame_type_name(FrameType type);

enum class PeerRole : std::uint8_t { kSender = 1, kReceiver = 2 };

enum class TransferMode : std::uint8_t {
  kDelta = 0,      // match blocks against the receiver's basis
  kWholeFile = 1,  // send every byte as literal data (baseline)
};

const char* transfer_mode_name(TransferMode mode);

enum class ErrorCode : std::uint8_t {
  kProtocol = 1,     // malformed or unexpected frame
  kVersion = 2,      // unsupported protocol version
  kBadRequest = 3,   // e.g. an unsafe name or invalid chunk size
  kBusy = 4,         // the name is being written by another connection,
                     // or the server is at its connection limit
  kIntegrity = 5,    // the rebuilt file failed verification
  kIo = 6,           // a file operation failed on the receiver
  kInternal = 7,
};

// A frame or payload is malformed, or arrived where it is not allowed.
class ProtocolError : public FormatError {
 public:
  using FormatError::FormatError;
};

// The peer reported an error in an ERROR frame.
class RemoteError : public Error {
 public:
  RemoteError(ErrorCode code, const std::string& message);
  ErrorCode code() const { return code_; }

 private:
  ErrorCode code_;
};

// HELLO: magic "DSYN" | version (1) | role (1)
struct Hello {
  std::uint8_t version = kProtocolVersion;
  PeerRole role = PeerRole::kSender;
};

// FILE_INFO: mode (1) | chunk_size (8) | block_size (4) | size (8) |
//            sha256 (32) | name length (2) | name
struct FileInfo {
  std::string name;
  std::uint64_t size = 0;
  Sha256Digest digest{};  // SHA-256 of the whole new version
  std::uint64_t chunk_size = 0;
  // Signature block size the sender asks for; 0 lets the receiver choose.
  std::uint32_t block_size = 0;
  TransferMode mode = TransferMode::kDelta;
};

// RESUME: verified_offset (8) | prefix SHA-256 midstate (8 x 4). The
// midstate's byte count is verified_offset itself.
struct ResumePoint {
  std::uint64_t offset = 0;
  Sha256::Midstate prefix;
};

// OK: bytes the receiver read (8) | bytes it wrote before this frame (8)
struct OkInfo {
  std::uint64_t receiver_bytes_received = 0;
  std::uint64_t receiver_bytes_sent = 0;
};

// PROGRESS: position (8). Tells the sender that the receiver is still
// working; position is how many bytes its current step has covered (target
// bytes verified, or partial output checked before RESUME) and is only
// informational.
struct ProgressInfo {
  std::uint64_t position = 0;
};

// ERROR: code (1) | UTF-8 message
struct ErrorInfo {
  ErrorCode code = ErrorCode::kInternal;
  std::string message;
};

using Payload = std::vector<std::uint8_t>;

Payload encode_hello(const Hello& hello);
Hello decode_hello(const Payload& payload);
Payload encode_file_info(const FileInfo& info);
FileInfo decode_file_info(const Payload& payload);
Payload encode_resume(const ResumePoint& resume);
ResumePoint decode_resume(const Payload& payload);
Payload encode_ok(const OkInfo& ok);
OkInfo decode_ok(const Payload& payload);
Payload encode_progress(const ProgressInfo& progress);
ProgressInfo decode_progress(const Payload& payload);
// Messages longer than kMaxErrorMessage are truncated.
Payload encode_error(const ErrorInfo& error);
ErrorInfo decode_error(const Payload& payload);

// Throws ProtocolError unless `name` is one plain path component that is
// safe to create under the receiver's root: 1..kMaxNameLength bytes, no '/',
// no NUL or other control characters, and not starting with '.' (which also
// rules out "." and ".." and keeps every pushed name apart from the
// receiver's hidden temporary and resumable-output files).
void validate_remote_name(const std::string& name);

// Throws ProtocolError unless `chunk_size` is a positive multiple of
// Sha256::kBlockLength no larger than kMaxChunkSize.
void validate_chunk_size(std::uint64_t chunk_size);

// Frames over a Socket. Received payloads are held in one buffer that is
// reused for every frame, so memory stays at kMaxFramePayload at most.
class FrameChannel {
 public:
  explicit FrameChannel(Socket& socket) : socket_(socket) {}

  void send(FrameType type, const std::uint8_t* payload, std::size_t length);
  void send(FrameType type, const Payload& payload) {
    send(type, payload.data(), payload.size());
  }
  // Sends `buffer`, whose first kFrameHeaderLength bytes are reserved for
  // the header and are overwritten here; avoids copying large payloads.
  void send_with_header_room(FrameType type, std::uint8_t* buffer, std::size_t payload_length);

  // Reads the next frame. Throws ProtocolError for an unknown type or an
  // oversized length, NetError if the connection fails.
  FrameType receive();
  const Payload& payload() const { return payload_; }

  // receive() in steps, for consuming a large payload as it arrives instead
  // of after the whole frame: receive_header() checks the header and
  // returns the type, then read_payload_some() hands out up to
  // payload_remaining() bytes, or read_payload() reads the rest into
  // payload(). A connection that ends inside a frame is a NetError.
  FrameType receive_header();
  std::uint32_t payload_remaining() const { return remaining_; }
  std::size_t read_payload_some(std::uint8_t* out, std::size_t capacity);
  void read_payload();

  // Reads the next frame and throws RemoteError if it is ERROR, or
  // ProtocolError if it is not `expected`.
  void expect(FrameType expected);

  // As expect(), but first skips any PROGRESS frames (sender side).
  void expect_skipping_progress(FrameType expected);

  Socket& socket() { return socket_; }

 private:
  Socket& socket_;
  Payload payload_;
  std::uint32_t remaining_ = 0;
};

// Throws RemoteError carrying the ERROR frame in `channel.payload()`.
[[noreturn]] void throw_remote_error(const Payload& payload);

// A ByteSink that packs everything written into frames of one type, each
// carrying up to kStreamFramePayload bytes. flush() sends a partial frame.
// Before each frame it checks, without blocking, whether the peer has sent
// something and throws an ERROR, so a sender learns why the receiver
// stopped instead of just seeing a reset. PROGRESS frames are skipped when
// set_skip_progress(true) (the sender's DATA stream); any other frame is a
// protocol error.
class FrameSink final : public ByteSink {
 public:
  FrameSink(FrameChannel& channel, FrameType type);

  void write(const std::uint8_t* data, std::size_t length) override;
  void flush() override;

  // Payload bytes written so far (excluding frame headers).
  std::uint64_t payload_bytes() const { return payload_bytes_; }
  void set_check_peer(bool check) { check_peer_ = check; }
  void set_skip_progress(bool skip) { skip_progress_ = skip; }

 private:
  void send_frame();

  FrameChannel& channel_;
  FrameType type_;
  std::vector<std::uint8_t> buffer_;
  std::size_t used_ = 0;
  std::uint64_t payload_bytes_ = 0;
  bool check_peer_ = true;
  bool skip_progress_ = false;
};

}  // namespace deltasync

#endif  // DELTASYNC_PROTOCOL_H_
