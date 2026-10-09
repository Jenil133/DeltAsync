// The sending side of a sync: holds the new version and streams a delta
// against the receiver's signature.

#include <errno.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "deltasync/delta_encoder.h"
#include "deltasync/file.h"
#include "deltasync/signature.h"
#include "deltasync/sync.h"

namespace deltasync {
namespace {

using Clock = std::chrono::steady_clock;

std::string last_component(const std::string& path) {
  std::string trimmed = path;
  while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
  const std::size_t slash = trimmed.find_last_of('/');
  return slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
}

// Reads `length` bytes of `source` into `hash` and returns how many bytes
// there were.
std::uint64_t hash_stream(FileSource& source, std::uint64_t length, Sha256* hash) {
  std::vector<std::uint8_t> buffer(kIoBufferSize);
  std::uint64_t total = 0;
  while (total < length) {
    const auto want =
        static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - total));
    const std::size_t count = source.read(buffer.data(), want);
    if (count == 0) break;
    hash->update(buffer.data(), count);
    total += count;
  }
  return total;
}

void seek_to(FileSource& source, std::uint64_t offset) {
  if (::lseek(source.fd(), static_cast<off_t>(offset), SEEK_SET) < 0) {
    throw IoError("seek", source.name(), errno);
  }
}

// At most this many hash states are kept from the first pass over the
// file (40 bytes each), so checking a resume offer rehashes at most
// 1/kMaxSavedStates of the file (or one chunk, if that is more).
constexpr std::uint64_t kMaxSavedStates = 4096;

// What a push needs to know about the local file before it talks to the
// receiver: the size and digest announced in FILE_INFO, and SHA-256 states
// at evenly spaced chunk boundaries for checking a RESUME offer.
struct LocalFile {
  std::uint64_t size = 0;
  Sha256Digest digest{};
  std::uint64_t stride = 0;              // bytes between saved states; a chunk multiple
  std::vector<Sha256::Midstate> states;  // states[i]: after the first i * stride bytes
};

LocalFile read_local_file(FileSource& source, std::uint64_t chunk_size) {
  LocalFile file;
  file.size = source.size();
  const std::uint64_t chunks = file.size / chunk_size;
  file.stride = chunk_size * std::max<std::uint64_t>(
                                 1, (chunks + kMaxSavedStates - 1) / kMaxSavedStates);
  Sha256 hash;
  file.states.push_back(hash.midstate());
  std::vector<std::uint8_t> buffer(kIoBufferSize);
  std::uint64_t total = 0;
  std::uint64_t next_state = file.stride;
  for (;;) {
    const auto want =
        static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), next_state - total));
    const std::size_t count = source.read(buffer.data(), want);
    if (count == 0) break;
    hash.update(buffer.data(), count);
    total += count;
    if (total == next_state) {
      file.states.push_back(hash.midstate());
      next_state += file.stride;
    }
  }
  if (total != file.size) throw Error(source.name() + " changed while it was being read");
  file.digest = hash.digest();
  return file;
}

// Builds the signature in memory while refusing absurd sizes before they
// are allocated.
class BoundedSignatureBuilder final : public SignatureSink {
 public:
  void on_header(const SignatureHeader& header) override {
    if (header.block_count() > kMaxSignatureBlocks) {
      throw ProtocolError("receiver signature has " + std::to_string(header.block_count()) +
                          " blocks, more than the limit of " +
                          std::to_string(kMaxSignatureBlocks));
    }
    builder_.on_header(header);
  }
  void on_block(std::uint32_t weak, const std::uint8_t* strong) override {
    builder_.on_block(weak, strong);
  }
  void on_end() override { builder_.on_end(); }

  Signature take() { return builder_.take(); }

 private:
  SignatureBuilder builder_;
};

// Passes the delta on to the writer and ends the current DATA frame after
// every CHUNK record. A chunk the receiver already has encodes to a few dozen
// bytes, so without this a nearly unchanged file would fill no frame for
// gigabytes and the receiver would hear nothing while the sender hashes.
class ChunkFramedWriter final : public DeltaSink {
 public:
  ChunkFramedWriter(DeltaWriter& writer, ByteSink& frames) : writer_(writer), frames_(frames) {}

  void on_header(const DeltaHeader& header) override { writer_.on_header(header); }
  void on_copy(std::uint64_t offset, std::uint64_t length) override {
    writer_.on_copy(offset, length);
  }
  void on_literal(const std::uint8_t* data, std::size_t length) override {
    writer_.on_literal(data, length);
  }
  void on_chunk(const Sha256Digest& digest) override {
    writer_.on_chunk(digest);
    frames_.flush();
  }
  void on_end(std::uint64_t total_length, const Sha256Digest& digest) override {
    writer_.on_end(total_length, digest);  // flushes the last frame
  }

 private:
  DeltaWriter& writer_;
  ByteSink& frames_;
};

Signature empty_signature() {
  SignatureHeader header;
  header.block_size = kMinDefaultBlockSize;
  header.basis_size = 0;
  return Signature(header);
}

bool same_midstate(const Sha256::Midstate& a, const Sha256::Midstate& b) {
  return a.length == b.length && a.words == b.words;
}

bool is_busy(const std::exception& error) {
  const auto* remote = dynamic_cast<const RemoteError*>(&error);
  return remote != nullptr && remote->code() == ErrorCode::kBusy;
}

bool retryable(const std::exception& error) {
  return dynamic_cast<const NetError*>(&error) != nullptr || is_busy(error);
}

std::string remote_name_of(const PushOptions& options) {
  return options.remote_name.empty() ? last_component(options.local_path) : options.remote_name;
}

// One attempt over a connected socket. `source` is positioned anywhere;
// `file` describes its contents.
PushStats push_session(Socket& socket, const PushOptions& options, FileSource& source,
                       const LocalFile& file) {
  const Clock::time_point start = Clock::now();
  validate_chunk_size(options.chunk_size);
  FileInfo info;
  info.name = remote_name_of(options);
  validate_remote_name(info.name);
  info.chunk_size = options.chunk_size;
  info.block_size = options.block_size;
  info.mode = options.mode;
  info.size = file.size;
  info.digest = file.digest;

  PushStats stats;
  stats.mode = options.mode;
  stats.file_size = info.size;

  FrameChannel channel(socket);
  Hello hello;
  hello.role = PeerRole::kSender;
  channel.send(FrameType::kHello, encode_hello(hello));
  channel.send(FrameType::kFileInfo, encode_file_info(info));

  channel.expect(FrameType::kHello);
  const Hello peer = decode_hello(channel.payload());
  if (peer.role != PeerRole::kReceiver) throw ProtocolError("peer is not a deltasync receiver");
  if (peer.version != kProtocolVersion) {
    throw ProtocolError("receiver speaks protocol version " + std::to_string(peer.version) +
                        ", this build speaks " + std::to_string(kProtocolVersion));
  }

  // The signature arrives in SIGNATURE frames; RESUME ends it. PROGRESS
  // frames only show that the receiver is still working.
  BoundedSignatureBuilder builder;
  SignatureDecoder decoder(builder);
  ResumePoint resume;
  for (;;) {
    const FrameType type = channel.receive();
    if (type == FrameType::kSignature) {
      if (options.mode == TransferMode::kWholeFile) {
        throw ProtocolError("receiver sent a signature for a whole-file transfer");
      }
      stats.signature_bytes += channel.payload().size();
      decoder.feed(channel.payload().data(), channel.payload().size());
    } else if (type == FrameType::kProgress) {
      decode_progress(channel.payload());
    } else if (type == FrameType::kResume) {
      resume = decode_resume(channel.payload());
      break;
    } else if (type == FrameType::kError) {
      throw_remote_error(channel.payload());
    } else {
      throw ProtocolError(std::string("expected SIGNATURE or RESUME, received ") +
                          frame_type_name(type));
    }
  }
  Signature signature;
  if (stats.signature_bytes > 0) {
    decoder.finish();
    signature = builder.take();
    stats.block_size = signature.block_size();
  } else {
    signature = empty_signature();
  }

  // A receiver offering to resume must prove it holds our prefix: its
  // midstate has to equal the hash of our first `offset` bytes. Anything
  // else (a different file, a stale partial) restarts at zero. The check
  // continues from the nearest saved state, so it rereads less than one
  // stride of the file however long the prefix is.
  Sha256 prefix;
  const bool offset_plausible = resume.offset > 0 && resume.offset <= info.size &&
                                resume.offset % info.chunk_size == 0;
  if (offset_plausible) {
    const std::uint64_t index = resume.offset / file.stride;
    const std::uint64_t saved = index * file.stride;
    Sha256 candidate = Sha256::from_midstate(file.states.at(index));
    seek_to(source, saved);
    if (hash_stream(source, resume.offset - saved, &candidate) != resume.offset - saved) {
      throw Error(options.local_path + " changed while it was being sent");
    }
    if (same_midstate(candidate.midstate(), resume.prefix)) prefix = candidate;
  }
  seek_to(source, prefix.length());
  stats.resumed_from = prefix.length();

  FrameSink data(channel, FrameType::kData);
  data.set_skip_progress(true);
  DeltaWriter writer(data);
  ChunkFramedWriter framed(writer, data);
  DeltaOptions delta_options;
  delta_options.chunk_size = info.chunk_size;
  DeltaEncoder encoder(signature, framed, delta_options, prefix);
  std::vector<std::uint8_t> buffer(kIoBufferSize);
  std::uint64_t sent = prefix.length();
  for (;;) {
    const std::size_t count = source.read(buffer.data(), buffer.size());
    if (count == 0) break;
    sent += count;
    if (sent > info.size) break;
    encoder.feed(buffer.data(), count);
  }
  if (sent != info.size) throw Error(options.local_path + " changed while it was being sent");
  encoder.finish();  // the writer flushes the last DATA frame with END

  // The receiver may still be rebuilding and syncing the file; it sends
  // PROGRESS meanwhile.
  channel.expect_skipping_progress(FrameType::kOk);
  stats.receiver = decode_ok(channel.payload());
  stats.delta_bytes = data.payload_bytes();
  stats.literal_bytes = encoder.stats().literal_bytes;
  stats.matched_bytes = encoder.stats().matched_bytes;
  stats.bytes_sent = socket.bytes_sent();
  stats.bytes_received = socket.bytes_received();
  stats.attempts = 1;
  stats.elapsed_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
  return stats;
}

// push_over() for a file that has already been read once.
PushStats push_prepared(Socket& socket, const PushOptions& options, FileSource& source,
                        const LocalFile& file) {
  try {
    return push_session(socket, options, source, file);
  } catch (const NetError&) {
    throw;
  } catch (const RemoteError&) {
    throw;
  } catch (const std::exception& error) {
    // Tell the receiver why we stopped, so it does not just see the
    // connection drop. Best effort: the original error is what matters.
    try {
      FrameChannel channel(socket);
      ErrorInfo info;
      info.code = dynamic_cast<const FormatError*>(&error) != nullptr ? ErrorCode::kProtocol
                                                                       : ErrorCode::kInternal;
      info.message = error.what();
      channel.send(FrameType::kError, encode_error(info));
    } catch (const std::exception&) {
    }
    throw;
  }
}

}  // namespace

PushStats push_over(Socket& socket, const PushOptions& options) {
  const Clock::time_point start = Clock::now();
  validate_chunk_size(options.chunk_size);
  FileSource source(options.local_path);
  const LocalFile file = read_local_file(source, options.chunk_size);
  PushStats stats = push_prepared(socket, options, source, file);
  stats.elapsed_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
  return stats;
}

PushStats push_file(const PushOptions& options) {
  const Clock::time_point start = Clock::now();
  // Mistakes in the options fail before anything touches the network.
  validate_chunk_size(options.chunk_size);
  validate_remote_name(remote_name_of(options));
  PushStats totals;
  unsigned counted_failures = 0;
  bool seen_busy = false;
  Clock::time_point first_busy;
  for (unsigned attempt = 1;; ++attempt) {
    Socket socket;
    try {
      // The file is read (and hashed) before connecting, so the receiver
      // never waits on a silent connection for it; every attempt rereads it
      // in case it changed in between.
      FileSource source(options.local_path);
      const LocalFile file = read_local_file(source, options.chunk_size);
      socket = connect_to(options.host, options.port, options.timeout_ms);
      PushStats stats = push_prepared(socket, options, source, file);
      stats.bytes_sent += totals.bytes_sent;
      stats.bytes_received += totals.bytes_received;
      stats.signature_bytes += totals.signature_bytes;
      stats.delta_bytes += totals.delta_bytes;
      stats.attempts = attempt;
      stats.elapsed_ms = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
      return stats;
    } catch (const std::exception& error) {
      if (!retryable(error) || options.retries == 0) throw;
      // A busy answer may come from a session the receiver still keeps for
      // an earlier attempt whose connection died without a FIN or RST; it
      // ends once the receiver's inactivity timeout expires. Until about
      // then, busy answers do not use up the retries.
      bool waiting_out_busy = false;
      if (is_busy(error)) {
        const Clock::time_point now = Clock::now();
        if (!seen_busy) {
          seen_busy = true;
          first_busy = now;
        }
        waiting_out_busy =
            options.timeout_ms > 0 && now - first_busy < std::chrono::milliseconds(options.timeout_ms);
      }
      if (!waiting_out_busy && ++counted_failures > options.retries) throw;
      // A failed attempt still used the wire; count what it moved. The
      // signature and delta split of a failed attempt is not tracked.
      totals.bytes_sent += socket.bytes_sent();
      totals.bytes_received += socket.bytes_received();
    }
    const int delay = std::min(5000, options.retry_delay_ms * static_cast<int>(attempt));
    if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  }
}

std::string stats_json(const PushStats& stats) {
  std::ostringstream out;
  out << "{\"mode\": \"" << transfer_mode_name(stats.mode) << "\""
      << ", \"file_size\": " << stats.file_size << ", \"bytes_sent\": " << stats.bytes_sent
      << ", \"bytes_received\": " << stats.bytes_received
      << ", \"signature_bytes\": " << stats.signature_bytes
      << ", \"delta_bytes\": " << stats.delta_bytes
      << ", \"literal_bytes\": " << stats.literal_bytes
      << ", \"matched_bytes\": " << stats.matched_bytes
      << ", \"resumed_from\": " << stats.resumed_from << ", \"attempts\": " << stats.attempts
      << ", \"elapsed_ms\": " << stats.elapsed_ms << "}\n";
  return out.str();
}

}  // namespace deltasync
