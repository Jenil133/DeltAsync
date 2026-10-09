// The receiving side of a sync: holds the basis, streams its signature, and
// rebuilds the new version from the sender's delta.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "deltasync/delta_format.h"
#include "deltasync/file.h"
#include "deltasync/io.h"
#include "deltasync/patch.h"
#include "deltasync/resume.h"
#include "deltasync/signature.h"
#include "deltasync/sync.h"

namespace deltasync {
namespace {

using Clock = std::chrono::steady_clock;

// After sending ERROR the receiver reads (and discards) what the sender
// still has in flight before closing. Closing a socket with unread input
// makes the kernel send RST, which can destroy the ERROR frame before the
// sender reads it. Both bounds only limit how long a misbehaving peer can
// hold the connection open: the time is a deadline for the whole drain, not
// a per-read timeout, so a peer trickling bytes cannot stretch it.
constexpr int kDrainTimeoutMs = 2000;
constexpr std::uint64_t kMaxDrainBytes = 64u << 20;

// Sends PROGRESS frames while the receiver works, at most one per interval
// and only when nothing else went out for that long.
class ProgressReporter {
 public:
  ProgressReporter(FrameChannel& channel, int interval_ms)
      : channel_(channel), interval_(std::chrono::milliseconds(interval_ms)) {}

  // Call after the receiver sent any other frame.
  void sent_something() { last_ = Clock::now(); }

  void report(std::uint64_t position) {
    const Clock::time_point now = Clock::now();
    if (now - last_ < interval_) return;
    ProgressInfo progress;
    progress.position = position;
    channel_.send(FrameType::kProgress, encode_progress(progress));
    last_ = now;
  }

 private:
  FrameChannel& channel_;
  Clock::duration interval_;
  Clock::time_point last_ = Clock::now();
};

// Passes a generated signature on to the writer and ends the current
// SIGNATURE frame after every kSignatureFlushBytes of basis, so the sender
// keeps hearing from a receiver that hashes a large basis.
class PacedSignatureSink final : public SignatureSink {
 public:
  PacedSignatureSink(SignatureSink& writer, ByteSink& frames, std::uint32_t block_size)
      : writer_(writer),
        frames_(frames),
        blocks_per_frame_(std::max<std::uint64_t>(1, kSignatureFlushBytes / block_size)) {}

  void on_header(const SignatureHeader& header) override { writer_.on_header(header); }
  void on_block(std::uint32_t weak, const std::uint8_t* strong) override {
    writer_.on_block(weak, strong);
    if (++blocks_ % blocks_per_frame_ == 0) frames_.flush();
  }
  void on_end() override { writer_.on_end(); }

 private:
  SignatureSink& writer_;
  ByteSink& frames_;
  std::uint64_t blocks_per_frame_;
  std::uint64_t blocks_ = 0;
};

// Reads a RandomAccessSource from start to end, so the signature is
// computed from the same open file (and size) the patcher later copies from.
class SequentialReader final : public ByteSource {
 public:
  explicit SequentialReader(RandomAccessSource& source) : source_(source) {}

  std::size_t read(std::uint8_t* out, std::size_t capacity) override {
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(capacity, source_.size() - position_));
    if (count == 0) return 0;
    source_.read_at(position_, out, count);
    position_ += count;
    return count;
  }

 private:
  RandomAccessSource& source_;
  std::uint64_t position_ = 0;
};

// A refusal of the request itself (as opposed to a broken stream).
class BadRequest : public Error {
 public:
  BadRequest(ErrorCode code, const std::string& message) : Error(message), code_(code) {}
  ErrorCode code() const { return code_; }

 private:
  ErrorCode code_;
};

// Rebuilds the target announced in FILE_INFO into resumable partial output
// and holds the delta to that announcement before the patcher sees it: the
// right chunk size, a start offset the receiver offered (or 0), no more
// output than the announced size, and an END that certifies the announced
// hash.
//
// The partial state is opened up front so its verified prefix can be
// offered in RESUME. Whether the sender accepted the offer is only known at
// the delta header: a header that starts at 0 instead means the sender
// could not confirm the prefix, so the partial state is discarded and the
// target is rebuilt from scratch.
class ResumableTarget final : public DeltaSink {
 public:
  // `progress` is called with the bytes checked or verified so far while
  // the partial state is checked and after every verified chunk.
  ResumableTarget(RandomAccessSource& basis, const FileInfo& info, std::string destination,
                  std::function<void(std::uint64_t)> progress)
      : basis_(basis),
        info_(info),
        destination_(std::move(destination)),
        progress_(std::move(progress)) {
    target_.size = info.size;
    target_.digest = info.digest;
    target_.chunk_size = info.chunk_size;
    open_output();
    offered_ = output_->verified_length();
  }

  ResumePoint offer() const {
    ResumePoint resume;
    resume.offset = offered_;
    resume.prefix = output_->prefix_state();
    return resume;
  }

  // Where the accepted delta started; valid once the header has arrived.
  std::uint64_t start_offset() const { return start_offset_; }

  // Permission bits for the committed file (kept across a reopen).
  void set_mode(mode_t mode) {
    has_mode_ = true;
    mode_ = mode;
  }

  bool finished() const { return patcher_ && patcher_->finished(); }

  void commit() {
    if (has_mode_) output_->set_mode(mode_);
    output_->commit();
  }

  // After a failed attempt: keeps verified progress for the next attempt
  // when the connection merely broke, and removes the partial state
  // otherwise (or when there is nothing worth keeping).
  // Never throws, so it cannot hide the error that ended the attempt.
  void keep_or_discard(bool connection_failed) noexcept {
    if (!output_) return;
    patcher_.reset();
    try {
      if (!(connection_failed && output_->verified_length() > 0)) output_->abandon();
    } catch (const std::exception&) {
      // A leftover partial file is checked before it is ever resumed.
    }
    output_.reset();
  }

  void on_header(const DeltaHeader& header) override {
    if (header.chunk_size != info_.chunk_size) {
      throw ProtocolError("delta chunk size " + std::to_string(header.chunk_size) +
                          " differs from FILE_INFO " + std::to_string(info_.chunk_size));
    }
    if (header.start_offset != offered_ && header.start_offset != 0) {
      throw ProtocolError("delta starts at " + std::to_string(header.start_offset) +
                          ", expected " + std::to_string(offered_) + " or 0");
    }
    if (header.start_offset != output_->verified_length()) {
      // The sender declined the offered prefix.
      output_->abandon();
      output_.reset();
      open_output();
      if (output_->verified_length() != 0) {
        throw Error(destination_ + ": partial state reappeared after it was discarded");
      }
    }
    start_offset_ = header.start_offset;
    position_ = header.start_offset;
    patcher_.emplace(basis_, *output_, output_->prefix_hash());
    const Patcher::ChunkCallback checkpoint = output_->checkpoint_callback(*patcher_);
    patcher_->set_chunk_callback([this, checkpoint](std::uint64_t verified) {
      checkpoint(verified);
      progress_(verified);
    });
    patcher_->on_header(header);
  }

  void on_copy(std::uint64_t offset, std::uint64_t length) override {
    advance(length);
    patcher_->on_copy(offset, length);
  }

  void on_literal(const std::uint8_t* data, std::size_t length) override {
    advance(length);
    patcher_->on_literal(data, length);
  }

  void on_chunk(const Sha256Digest& digest) override { patcher_->on_chunk(digest); }

  void on_end(std::uint64_t total_length, const Sha256Digest& digest) override {
    if (total_length != info_.size) {
      throw ProtocolError("delta ends at " + std::to_string(total_length) +
                          " bytes, FILE_INFO announced " + std::to_string(info_.size));
    }
    if (digest != info_.digest) {
      throw IntegrityError("delta certifies a different file than FILE_INFO announced");
    }
    patcher_->on_end(total_length, digest);
  }

 private:
  void open_output() {
    try {
      output_.emplace(destination_, target_, progress_);
    } catch (const IoError&) {
      throw;
    } catch (const UnclaimedPartialFile& error) {
      throw BadRequest(ErrorCode::kIo, error.what());
    } catch (const Error& error) {
      // Another process holds the partial file of this destination.
      throw BadRequest(ErrorCode::kBusy, error.what());
    }
  }

  void advance(std::uint64_t length) {
    if (length > info_.size - position_) {
      throw ProtocolError("delta carries more data than the announced " +
                          std::to_string(info_.size) + " bytes");
    }
    position_ += length;
  }

  RandomAccessSource& basis_;
  const FileInfo& info_;
  const std::string destination_;
  const std::function<void(std::uint64_t)> progress_;
  TargetId target_;
  std::uint64_t offered_ = 0;
  std::uint64_t start_offset_ = 0;
  std::uint64_t position_ = 0;
  bool has_mode_ = false;
  mode_t mode_ = 0;
  // Declared before the patcher, which writes into it.
  std::optional<PartialOutput> output_;
  std::optional<Patcher> patcher_;
};

ErrorCode code_for(const std::exception& error) {
  if (dynamic_cast<const IntegrityError*>(&error) != nullptr) return ErrorCode::kIntegrity;
  if (dynamic_cast<const FormatError*>(&error) != nullptr) return ErrorCode::kProtocol;
  if (dynamic_cast<const IoError*>(&error) != nullptr) return ErrorCode::kIo;
  return ErrorCode::kInternal;
}

void send_error_and_close(Socket& socket, ErrorCode code, const std::string& message) {
  try {
    FrameChannel channel(socket);
    ErrorInfo error;
    error.code = code;
    error.message = message;
    channel.send(FrameType::kError, encode_error(error));
    socket.shutdown_write();
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(kDrainTimeoutMs);
    std::vector<std::uint8_t> discard(64 * 1024);
    std::uint64_t drained = 0;
    while (drained < kMaxDrainBytes) {
      const auto left =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left <= 0) break;
      socket.set_timeout_ms(static_cast<int>(left));
      const std::size_t count = socket.read_some(discard.data(), discard.size());
      if (count == 0) break;
      drained += count;
    }
  } catch (const std::exception&) {
    // The connection is already broken; there is nobody left to tell.
  }
}

// Releases a NameLocks entry on every path out of a session.
class NameLockGuard {
 public:
  NameLockGuard(NameLocks* locks, const std::string& name) : locks_(locks), name_(name) {
    if (locks_ != nullptr && !locks_->try_lock(name_)) {
      throw BadRequest(ErrorCode::kBusy, "'" + name_ + "' is being written by another connection");
    }
  }
  ~NameLockGuard() {
    if (locks_ != nullptr) locks_->unlock(name_);
  }
  NameLockGuard(const NameLockGuard&) = delete;
  NameLockGuard& operator=(const NameLockGuard&) = delete;

 private:
  NameLocks* locks_;
  std::string name_;
};

// Opens the existing destination as the basis, or returns null if there is
// none. Anything other than a regular file (a symlink that might point out
// of the root, a directory) is refused rather than followed or replaced.
std::unique_ptr<RandomAccessFile> open_existing(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    if (errno == ENOENT) return nullptr;
    throw IoError("stat", path, errno);
  }
  if (!S_ISREG(info.st_mode)) {
    throw BadRequest(ErrorCode::kBadRequest, "destination exists and is not a regular file");
  }
  return std::make_unique<RandomAccessFile>(path);
}

void run_session(Socket& socket, const std::string& root, NameLocks* locks,
                 int progress_interval_ms, SessionReport* report) {
  FrameChannel channel(socket);

  channel.expect(FrameType::kHello);
  const Hello hello = decode_hello(channel.payload());
  if (hello.role != PeerRole::kSender) throw ProtocolError("peer is not a deltasync sender");
  if (hello.version != kProtocolVersion) {
    throw BadRequest(ErrorCode::kVersion,
                     "protocol version " + std::to_string(hello.version) +
                         " is not supported (this receiver speaks " +
                         std::to_string(kProtocolVersion) + ")");
  }
  Hello reply;
  reply.role = PeerRole::kReceiver;
  channel.send(FrameType::kHello, encode_hello(reply));

  channel.expect(FrameType::kFileInfo);
  const FileInfo info = decode_file_info(channel.payload());
  report->mode = info.mode;
  report->file_size = info.size;
  try {
    validate_remote_name(info.name);
    validate_chunk_size(info.chunk_size);
    if (info.block_size != 0 &&
        (info.block_size < kMinBlockSize || info.block_size > kMaxBlockSize)) {
      throw ProtocolError("block size " + std::to_string(info.block_size) + " is out of range");
    }
  } catch (const ProtocolError& error) {
    throw BadRequest(ErrorCode::kBadRequest, error.what());
  }
  // Only a validated name goes into reports and logs.
  report->name = info.name;

  const NameLockGuard lock(locks, info.name);
  const std::string destination = root + "/" + info.name;
  std::unique_ptr<RandomAccessFile> existing = open_existing(destination);
  report->had_basis = existing != nullptr;

  // Whole-file mode, a missing basis, and an empty one all mean "no
  // signature": the sender then encodes against an empty basis, so every
  // byte travels as literal data through the same path.
  const bool use_basis = info.mode == TransferMode::kDelta && existing != nullptr &&
                         existing->size() > 0;
  MemoryRandomAccess no_basis(nullptr, 0);
  RandomAccessSource& basis =
      use_basis ? static_cast<RandomAccessSource&>(*existing) : no_basis;
  {
    FrameSink signature_frames(channel, FrameType::kSignature);
    if (use_basis) {
      SignatureHeader header;
      header.basis_size = existing->size();
      header.block_size =
          info.block_size != 0 ? info.block_size : default_block_size(header.basis_size);
      SignatureWriter writer(signature_frames);
      PacedSignatureSink paced(writer, signature_frames, header.block_size);
      SequentialReader reader(*existing);
      generate_signature(reader, header, paced);  // the writer flushes at the end
    }
    report->signature_bytes = signature_frames.payload_bytes();
  }

  // Partial output from an earlier, interrupted attempt for this exact
  // target is offered for resumption; the sender decides whether to use it.
  // Checking that state can take a while, and so can rebuilding the target
  // after the sender has sent its last DATA frame; PROGRESS covers both.
  ProgressReporter progress(channel, progress_interval_ms);
  ResumableTarget target(basis, info, destination,
                         [&progress](std::uint64_t position) { progress.report(position); });
  if (existing != nullptr) target.set_mode(existing->mode());

  try {
    channel.send(FrameType::kResume, encode_resume(target.offer()));
    progress.sent_something();

    // DATA payloads go to the decoder as they arrive rather than frame by
    // frame, so a broken connection loses at most the chunk in progress.
    DeltaDecoder decoder(target);
    std::vector<std::uint8_t> buffer(64 * 1024);
    while (!decoder.done()) {
      const FrameType type = channel.receive_header();
      if (type != FrameType::kData) {
        channel.read_payload();
        if (type == FrameType::kError) throw_remote_error(channel.payload());
        throw ProtocolError(std::string("expected DATA, received ") + frame_type_name(type));
      }
      while (channel.payload_remaining() > 0) {
        const std::size_t count = channel.read_payload_some(buffer.data(), buffer.size());
        report->delta_bytes += count;
        decoder.feed(buffer.data(), count);
      }
    }
    decoder.finish();
    if (!target.finished()) throw ProtocolError("delta ended without END");
    report->resumed_from = target.start_offset();
    target.commit();
  } catch (const NetError&) {
    report->resumed_from = target.start_offset();
    target.keep_or_discard(/*connection_failed=*/true);
    throw;
  } catch (...) {
    report->resumed_from = target.start_offset();
    target.keep_or_discard(/*connection_failed=*/false);
    throw;
  }

  OkInfo ok;
  ok.receiver_bytes_received = socket.bytes_received();
  ok.receiver_bytes_sent = socket.bytes_sent();
  channel.send(FrameType::kOk, encode_ok(ok));
}

}  // namespace

bool NameLocks::try_lock(const std::string& name) {
  const std::lock_guard<std::mutex> guard(mutex_);
  return held_.insert(name).second;
}

void NameLocks::unlock(const std::string& name) {
  const std::lock_guard<std::mutex> guard(mutex_);
  held_.erase(name);
}

SessionReport receive_session(Socket& socket, const std::string& root, NameLocks* locks,
                              int progress_interval_ms) {
  SessionReport report;
  report.peer = socket.peer();
  try {
    run_session(socket, root, locks, progress_interval_ms, &report);
    report.ok = true;
  } catch (const BadRequest& error) {
    report.error = error.what();
    send_error_and_close(socket, error.code(), error.what());
  } catch (const RemoteError& error) {
    report.error = error.what();  // the sender gave up and told us why
  } catch (const NetError& error) {
    report.error = error.what();  // nobody left to tell
  } catch (const std::exception& error) {
    report.error = error.what();
    send_error_and_close(socket, code_for(error), error.what());
  }
  report.bytes_sent = socket.bytes_sent();
  report.bytes_received = socket.bytes_received();
  return report;
}

SyncServer::SyncServer(ServerOptions options)
    : options_(std::move(options)), listener_(options_.bind_address, options_.port) {
  struct stat info;
  if (::stat(options_.root.c_str(), &info) != 0) throw IoError("stat", options_.root, errno);
  if (!S_ISDIR(info.st_mode)) throw Error(options_.root + ": not a directory");
  if (options_.max_connections == 0) options_.max_connections = 1;

  // stop() wakes run() through this pipe, which works from any thread
  // without signals or timeouts.
  int fds[2];
  if (::pipe(fds) != 0) throw IoError("pipe", "wake-up pipe", errno);
  wake_read_.reset(fds[0]);
  wake_write_.reset(fds[1]);
  for (const int fd : fds) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL) | O_NONBLOCK);
}

SyncServer::~SyncServer() {
  stop();
  std::list<Worker> workers;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    workers.swap(workers_);
  }
  for (Worker& worker : workers) {
    if (worker.thread.joinable()) worker.thread.join();
  }
}

SessionReport SyncServer::serve_one() {
  Socket socket = listener_.accept(options_.timeout_ms);
  SessionReport report =
      receive_session(socket, options_.root, &locks_, options_.progress_interval_ms);
  if (options_.on_session) options_.on_session(report);
  return report;
}

void SyncServer::reap_finished_locked(std::list<Worker>* done) {
  for (auto it = workers_.begin(); it != workers_.end();) {
    const auto current = it++;
    if (current->finished) done->splice(done->end(), workers_, current);
  }
}

void SyncServer::session(Socket socket, Worker* worker) {
  const SessionReport report =
      receive_session(socket, options_.root, &locks_, options_.progress_interval_ms);
  if (options_.on_session) {
    try {
      options_.on_session(report);
    } catch (const std::exception&) {
      // A failing observer must not take the server down.
    }
  }
  const std::lock_guard<std::mutex> guard(mutex_);
  // Deregister before the socket closes (at return), so stop() never shuts
  // down a descriptor number that has been reused.
  worker->fd = -1;
  worker->finished = true;
  --active_;
  idle_.notify_all();
}

void SyncServer::run() {
  for (;;) {
    pollfd fds[2] = {{listener_.fd(), POLLIN, 0}, {wake_read_.get(), POLLIN, 0}};
    const int ready = ::poll(fds, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      throw NetError(std::string("poll: ") + error_message(errno));
    }
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      if (stopping_) break;
    }
    if ((fds[0].revents & POLLIN) == 0) continue;

    Socket socket = listener_.try_accept(options_.timeout_ms);
    if (!socket.valid()) continue;
    std::list<Worker> done;
    bool refuse = false;
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      reap_finished_locked(&done);
      if (stopping_) {
        // Their threads are joined with everyone else's below; a joinable
        // std::thread must never be destroyed.
        workers_.splice(workers_.end(), done);
        break;
      }
      if (active_ >= options_.max_connections) {
        refuse = true;
      } else {
        workers_.emplace_back();
        Worker* worker = &workers_.back();
        worker->fd = socket.fd();
        ++active_;
        worker->thread = std::thread(&SyncServer::session, this, std::move(socket), worker);
      }
    }
    for (Worker& worker : done) worker.thread.join();
    if (refuse) {
      SessionReport report;
      report.peer = socket.peer();
      report.error = "connection limit reached";
      send_error_and_close(socket, ErrorCode::kBusy, "the server is at its connection limit");
      if (options_.on_session) options_.on_session(report);
    }
  }

  // Wake every session still running and wait for all of them.
  std::list<Worker> workers;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    for (const Worker& worker : workers_) {
      if (worker.fd >= 0) ::shutdown(worker.fd, SHUT_RDWR);
    }
    idle_.wait(lock, [this] { return active_ == 0; });
    workers.swap(workers_);
  }
  for (Worker& worker : workers) worker.thread.join();
}

void SyncServer::stop() {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (stopping_) return;
    stopping_ = true;
  }
  const std::uint8_t byte = 1;
  // The pipe is non-blocking; if it is somehow full, run() wakes anyway.
  [[maybe_unused]] const ssize_t written = ::write(wake_write_.get(), &byte, 1);
}

}  // namespace deltasync
