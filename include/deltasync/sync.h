#ifndef DELTASYNC_SYNC_H_
#define DELTASYNC_SYNC_H_

// File synchronization over TCP: the receiver (server) holds the basis
// files under a root directory, the sender (client) holds the new version
// of one file and pushes it. Only the regions the receiver lacks cross the
// network; see protocol.h for the conversation.

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "deltasync/delta_format.h"
#include "deltasync/net.h"
#include "deltasync/protocol.h"

namespace deltasync {

// The sender refuses a signature with more blocks than this (about 160 MiB
// of signature at the default strong length), so a hostile receiver cannot
// make it allocate without bound. Real signatures stay far below it: the
// default block size keeps a 1 TiB basis at 8 Mi blocks.
constexpr std::uint64_t kMaxSignatureBlocks = std::uint64_t{1} << 23;

// While the receiver works without anything else to send (checking partial
// output before RESUME, rebuilding the target while the sender waits for
// OK), it sends a PROGRESS frame once this much time has passed since its
// last frame. It must stay well below the sender's inactivity timeout.
constexpr int kDefaultProgressIntervalMs = 1000;

struct PushOptions {
  std::string local_path;
  std::string host;
  std::uint16_t port = 0;
  // Name under the receiver's root; empty means the last component of
  // local_path.
  std::string remote_name;
  TransferMode mode = TransferMode::kDelta;
  // Signature block size to ask the receiver for; 0 lets it choose
  // default_block_size() of its basis.
  std::uint32_t block_size = 0;
  std::uint64_t chunk_size = kDefaultChunkSize;
  // Further attempts after a connection failure (NetError) or a busy
  // receiver. Other errors are not retried. When retries > 0, ERROR busy
  // answers keep being retried (without counting against `retries`) until
  // timeout_ms has passed since the first of them: a session the receiver
  // still holds for a connection that silently died ends by then, at the
  // latest after the receiver's own inactivity timeout (assuming it is no
  // longer than ours).
  unsigned retries = 0;
  // Pause before retry n is n * retry_delay_ms, at most 5 s.
  int retry_delay_ms = 500;
  // Inactivity timeout for every socket operation (0 = wait forever).
  int timeout_ms = kDefaultTimeoutMs;
};

// What a push cost. Byte counts add up all attempts; literal, matched and
// resumed_from describe the attempt that succeeded.
struct PushStats {
  TransferMode mode = TransferMode::kDelta;
  std::uint64_t file_size = 0;
  std::uint64_t bytes_sent = 0;       // every byte written to the socket
  std::uint64_t bytes_received = 0;   // every byte read from the socket
  std::uint64_t signature_bytes = 0;  // serialized signature received
  std::uint64_t delta_bytes = 0;      // serialized delta sent
  std::uint64_t literal_bytes = 0;
  std::uint64_t matched_bytes = 0;
  std::uint64_t resumed_from = 0;     // target offset the delta started at
  std::uint32_t block_size = 0;       // of the receiver's signature (0: none)
  unsigned attempts = 0;
  std::uint64_t elapsed_ms = 0;
  // The receiver's own byte counts, from its OK frame.
  OkInfo receiver;
};

// Pushes options.local_path to the receiver. Returns once the receiver has
// verified, synced and renamed the new file into place; throws otherwise
// (NetError, RemoteError, ProtocolError, IoError, ...).
PushStats push_file(const PushOptions& options);

// One push attempt over a connected socket, without retries. Exposed so
// tests can drive a sender against a hand-written peer. It reads the whole
// file before it sends anything, so the peer waits that long for HELLO;
// push_file() reads the file before it connects instead.
PushStats push_over(Socket& socket, const PushOptions& options);

// The stats as one JSON object with the keys mode, file_size, bytes_sent,
// bytes_received, signature_bytes, delta_bytes, literal_bytes,
// matched_bytes, resumed_from, attempts, elapsed_ms.
std::string stats_json(const PushStats& stats);

// The outcome of one connection on the receiving side.
struct SessionReport {
  std::string peer;
  std::string name;
  bool ok = false;
  std::string error;
  TransferMode mode = TransferMode::kDelta;
  std::uint64_t file_size = 0;
  bool had_basis = false;
  std::uint64_t bytes_sent = 0;
  std::uint64_t bytes_received = 0;
  std::uint64_t signature_bytes = 0;
  std::uint64_t delta_bytes = 0;
  std::uint64_t resumed_from = 0;
};

// Serializes writers of the same name on one server: a second connection
// for a name that is being written is turned away with ERROR(kBusy) instead
// of racing the first one to the final rename.
class NameLocks {
 public:
  bool try_lock(const std::string& name);
  void unlock(const std::string& name);

 private:
  std::mutex mutex_;
  std::set<std::string> held_;
};

// Runs the receiving side of one connection to completion and reports the
// outcome; errors are reported, not thrown. The file appears at
// root/<name> only if every chunk and the whole-file hash verified, and is
// fsynced and atomically renamed into place before OK is sent. `locks` may
// be null when nothing else writes under `root`. PROGRESS frames go out
// at most every progress_interval_ms (0: at every opportunity).
SessionReport receive_session(Socket& socket, const std::string& root, NameLocks* locks,
                              int progress_interval_ms = kDefaultProgressIntervalMs);

struct ServerOptions {
  std::string root;
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 0;  // 0: any free port, see SyncServer::port()
  int timeout_ms = kDefaultTimeoutMs;
  // See receive_session().
  int progress_interval_ms = kDefaultProgressIntervalMs;
  // Connections handled at once by run(); more are refused with kBusy.
  unsigned max_connections = 16;
  // Called after every connection, from the thread that handled it.
  std::function<void(const SessionReport&)> on_session;
};

// Listens for senders and stores what they push under options.root.
// run() handles each connection on its own thread; the core objects are
// thread-confined, so sessions share nothing but the NameLocks.
class SyncServer {
 public:
  // Binds immediately (throws NetError), so port() is valid at once.
  explicit SyncServer(ServerOptions options);
  ~SyncServer();

  SyncServer(const SyncServer&) = delete;
  SyncServer& operator=(const SyncServer&) = delete;

  std::uint16_t port() const { return listener_.port(); }
  const std::string& address() const { return listener_.address(); }

  // Accepts one connection and handles it on the calling thread.
  SessionReport serve_one();

  // Accepts connections until stop(), then waits for open sessions after
  // shutting their sockets down.
  void run();

  // Makes run() return. Safe from any thread, including signal-free use from
  // another thread while run() is blocked; idempotent.
  void stop();

 private:
  struct Worker {
    std::thread thread;
    int fd = -1;
    bool finished = false;
  };

  void session(Socket socket, Worker* worker);
  void reap_finished_locked(std::list<Worker>* done);

  ServerOptions options_;
  Listener listener_;
  NameLocks locks_;
  UniqueFd wake_read_;
  UniqueFd wake_write_;

  std::mutex mutex_;
  std::condition_variable idle_;
  std::list<Worker> workers_;
  unsigned active_ = 0;
  bool stopping_ = false;
};

}  // namespace deltasync

#endif  // DELTASYNC_SYNC_H_
