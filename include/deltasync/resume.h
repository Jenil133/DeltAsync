#ifndef DELTASYNC_RESUME_H_
#define DELTASYNC_RESUME_H_

// Durable, resumable output for a transfer that is verified chunk by chunk.
//
// While a target "<dir>/<name>" is being rebuilt, its bytes go to the hidden
// file "<dir>/.<name>.partial" and a small journal "<dir>/.<name>.journal"
// records how much of that file is verified: the verified length (always a
// chunk boundary) and the SHA-256 midstate of those bytes. After every
// verified chunk the data file is synced first and the journal is then
// replaced atomically, so the journal never claims more than is on stable
// storage. A later attempt for the same target reopens both files, cuts off
// the unverified tail and continues from the verified length; anything that
// does not check out exactly starts over from zero.
//
// The names start with '.', which a pushed remote name never does, so the
// state of one transfer can never be mistaken for a file someone pushed. A
// data file is only ever truncated, replaced or removed while a journal
// claims it: fresh state is created with O_EXCL and claimed at once by a
// journal that verifies 0 bytes, and an existing data file without a journal
// is left alone (the constructor fails instead).
//
//   journal (128 bytes, integers big-endian):
//     magic "DSJR" | version (1 byte) | 3 zero bytes |
//     target size (8) | target SHA-256 (32) | chunk size (8) |
//     verified length (8) | SHA-256 midstate words (8 x 4) |
//     SHA-256 of the preceding 96 bytes (32)

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "deltasync/delta_format.h"
#include "deltasync/error.h"
#include "deltasync/file.h"
#include "deltasync/io.h"
#include "deltasync/patch.h"
#include "deltasync/sha256.h"

namespace deltasync {

// A data file for resumable output exists, but no journal claims it, so it
// may not belong to deltasync and is not touched.
class UnclaimedPartialFile : public Error {
 public:
  using Error::Error;
};

// Identifies the target a partial file belongs to. A journal written for a
// different size, digest or chunk size is never resumed.
struct TargetId {
  std::uint64_t size = 0;
  Sha256Digest digest{};  // SHA-256 of the whole target
  std::uint64_t chunk_size = kDefaultChunkSize;
};

bool operator==(const TargetId& a, const TargetId& b);
inline bool operator!=(const TargetId& a, const TargetId& b) { return !(a == b); }

// The ByteSink a Patcher writes the target into, backed by the hidden
// partial and journal files described above. Typical use on the receiving
// side:
//
//   PartialOutput out(dest, target);
//   // tell the sender out.verified_length() and out.prefix_state()
//   Patcher patcher(basis, out, out.prefix_hash());
//   patcher.set_chunk_callback(out.checkpoint_callback(patcher));
//   ... feed the delta (start_offset == verified_length()) ...
//   if (patcher.finished()) out.commit();
//
// If the transfer fails, destroying the object (without commit() or
// abandon()) keeps both files so a later attempt can resume. An object is
// confined to one thread; a lock on the data file keeps two objects (in any
// processes) from writing the same destination at once.
class PartialOutput final : public ByteSink {
 public:
  // Points in checkpoint() reported to a CheckpointObserver.
  enum class CheckpointStage {
    kDataSynced,       // the data is durable; the journal is not yet replaced
    kJournalReplaced,  // the new journal is durable
  };
  using CheckpointObserver = std::function<void(CheckpointStage stage, std::uint64_t length)>;
  // Called while the constructor rehashes a verified prefix, with the bytes
  // checked so far, so a caller can show a peer that it is still working.
  using CheckProgress = std::function<void(std::uint64_t checked)>;

  // Opens the partial state for `target` at `destination`, resuming it when
  // the journal is intact, describes this exact target, and the data file
  // holds at least the verified length and its bytes hash to the recorded
  // midstate; otherwise it starts from zero (restart_reason() says why).
  // Throws std::invalid_argument if target.chunk_size is not a valid chunk
  // size (see valid_chunk_size()), Error if another PartialOutput holds the
  // destination, UnclaimedPartialFile if a data file exists without a
  // journal, and IoError on I/O failures.
  PartialOutput(std::string destination, const TargetId& target,
                const CheckProgress& progress = CheckProgress());

  // The files that hold the state for `destination`.
  static std::string data_path_for(const std::string& destination);
  static std::string journal_path_for(const std::string& destination);

  // Closes the files. Unless commit() or abandon() ran, both files stay for
  // a later attempt; bytes past the last checkpoint are discarded then.
  ~PartialOutput() override;

  PartialOutput(const PartialOutput&) = delete;
  PartialOutput& operator=(const PartialOutput&) = delete;

  // ByteSink: appends target bytes at position(). Bytes are buffered; only
  // checkpoint() and commit() make them durable.
  void write(const std::uint8_t* data, std::size_t length) override;
  void flush() override;

  const TargetId& target() const { return target_; }

  // Length of the verified, durable prefix; a multiple of the chunk size
  // (0 for a fresh start).
  std::uint64_t verified_length() const { return verified_length_; }

  // SHA-256 state of target bytes [0, verified_length()).
  const Sha256::Midstate& prefix_state() const { return prefix_state_; }

  // A hasher continuing from prefix_state(), for Patcher's resume
  // constructor (and for checking a sender's claim).
  Sha256 prefix_hash() const { return Sha256::from_midstate(prefix_state_); }

  // True if the constructor resumed earlier progress (verified_length() > 0).
  bool resumed() const { return resumed_; }

  // Why earlier partial state was discarded; empty if it was resumed or there
  // was none.
  const std::string& restart_reason() const { return restart_reason_; }

  // Target offset of the next byte written.
  std::uint64_t position() const { return position_; }

  // Discards everything written after verified_length(), so that another
  // attempt can continue on this object from the last checkpoint (the state
  // reopening would give). Use it after a failed attempt.
  void rewind();

  // Records that target bytes [0, verified_length) are verified and that
  // `prefix` is their SHA-256 state: syncs the data file, then atomically
  // replaces the journal. Lengths that are not a multiple of the chunk size
  // (only the final, short chunk) are not journaled, since END and commit()
  // follow immediately. Throws std::logic_error if verified_length is past
  // position() or disagrees with prefix.length().
  void checkpoint(std::uint64_t verified_length, const Sha256& prefix);

  // A Patcher chunk callback that checkpoints with the patcher's prefix
  // state. `patcher` must write into this object and outlive the callback.
  Patcher::ChunkCallback checkpoint_callback(const Patcher& patcher);

  // Permission bits for the committed file. Without a call it gets what a
  // new file in its directory would get (see new_file_mode()). The partial
  // file itself stays owner-only (0600) until commit().
  void set_mode(mode_t mode);

  // Moves the complete target into place: flush, set the mode, sync,
  // rename the partial file over the destination, sync the directory, then
  // remove the journal. The caller must have verified the whole target
  // (Patcher::finished()); throws std::logic_error unless exactly
  // target().size bytes were written.
  void commit();

  // Removes both files, e.g. when the sender's file changed and the partial
  // data is useless.
  void abandon();

  // For fault-injection tests: called at each stage of a checkpoint. If it
  // throws, or the process dies inside it, the on-disk state is exactly what
  // a crash at that point would leave.
  void set_checkpoint_observer(CheckpointObserver observer) { observer_ = std::move(observer); }

  const std::string& destination() const { return destination_; }
  const std::string& data_path() const { return data_path_; }
  const std::string& journal_path() const { return journal_path_; }

 private:
  // Returns true and fills verified_length_/prefix_state_ if the existing
  // journal and data can be resumed; otherwise sets restart_reason_.
  bool try_resume(const CheckProgress& progress);
  void restart();
  // `durable` syncs the journal and its directory; the claim written by
  // restart() skips that (see restart()).
  void write_journal(std::uint64_t verified_length, const Sha256::Midstate& state,
                     bool durable = true);
  void remove_journal();
  void require_open(const char* what) const;

  std::string destination_;
  std::string data_path_;
  std::string journal_path_;
  TargetId target_;
  UniqueFd data_fd_;
  FileSink sink_;
  std::uint64_t verified_length_ = 0;
  Sha256::Midstate prefix_state_;
  std::uint64_t position_ = 0;
  bool resumed_ = false;
  std::string restart_reason_;
  bool has_mode_ = false;
  mode_t mode_ = 0;
  bool closed_ = false;  // committed or abandoned
  CheckpointObserver observer_;
};

}  // namespace deltasync

#endif  // DELTASYNC_RESUME_H_
