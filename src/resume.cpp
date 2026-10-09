#include "deltasync/resume.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "deltasync/bytes.h"
#include "deltasync/error.h"

namespace deltasync {
namespace {

constexpr std::uint8_t kJournalMagic[4] = {'D', 'S', 'J', 'R'};
constexpr std::uint8_t kJournalVersion = 1;

// Field offsets within the journal.
constexpr std::size_t kVersionAt = 4;
constexpr std::size_t kTargetSizeAt = 8;
constexpr std::size_t kTargetDigestAt = 16;
constexpr std::size_t kChunkSizeAt = 48;
constexpr std::size_t kVerifiedAt = 56;
constexpr std::size_t kMidstateAt = 64;
constexpr std::size_t kChecksumAt = 96;
constexpr std::size_t kJournalSize = 128;

using JournalBytes = std::array<std::uint8_t, kJournalSize>;

const TargetId& checked(const TargetId& target) {
  if (!valid_chunk_size(target.chunk_size)) {
    throw std::invalid_argument("chunk size " + std::to_string(target.chunk_size) +
                                " is not a positive multiple of " +
                                std::to_string(Sha256::kBlockLength) + " up to " +
                                std::to_string(kMaxChunkSize));
  }
  return target;
}

// "<dir>/<name>" -> "<dir>/.<name><suffix>"
std::string hidden_sibling(const std::string& destination, const char* suffix) {
  const std::size_t slash = destination.find_last_of('/');
  const std::size_t start = slash == std::string::npos ? 0 : slash + 1;
  return destination.substr(0, start) + "." + destination.substr(start) + suffix;
}

bool path_exists(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) == 0) return true;
  if (errno == ENOENT) return false;
  throw IoError("stat", path, errno);
}

UniqueFd open_data_file(const std::string& path, const std::string& journal_path) {
  // Any journal, even a damaged one (which then restarts the transfer),
  // marks the data file as ours. Without one the data file must not exist
  // yet: whatever is there was not made by this code, or not finished
  // being claimed, and is not ours to truncate.
  const bool claimed = path_exists(journal_path);
  // Owner-only until commit(): the partial file may sit around for a long
  // time between attempts.
  const int flags = O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (claimed ? 0 : O_EXCL);
  for (;;) {
    const int fd = ::open(path.c_str(), flags, 0600);
    if (fd >= 0) return UniqueFd(fd);
    if (errno == EINTR) continue;
    if (errno == EEXIST && !claimed) {
      throw UnclaimedPartialFile(path +
                                 " exists but no resume journal claims it; it is left alone "
                                 "(remove it if it is left over from a crash)");
    }
    throw IoError("open", path, errno);
  }
}

void write_all(int fd, const std::uint8_t* data, std::size_t length, const std::string& name) {
  while (length > 0) {
    const ssize_t count = ::write(fd, data, length);
    if (count < 0) {
      if (errno == EINTR) continue;
      throw IoError("write", name, errno);
    }
    data += count;
    length -= static_cast<std::size_t>(count);
  }
}

// Reads up to `capacity` bytes from the start of the file; returns the
// count, or -1 if the file does not exist.
long read_small_file(const std::string& path, std::uint8_t* out, std::size_t capacity) {
  int fd;
  for (;;) {
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) break;
    if (errno == ENOENT) return -1;
    if (errno != EINTR) throw IoError("open", path, errno);
  }
  UniqueFd owned(fd);
  std::size_t used = 0;
  while (used < capacity) {
    const ssize_t count = ::read(fd, out + used, capacity - used);
    if (count < 0) {
      if (errno == EINTR) continue;
      throw IoError("read", path, errno);
    }
    if (count == 0) break;
    used += static_cast<std::size_t>(count);
  }
  return static_cast<long>(used);
}

void truncate_or_throw(int fd, std::uint64_t length, const std::string& name) {
  for (;;) {
    if (::ftruncate(fd, static_cast<off_t>(length)) == 0) return;
    if (errno != EINTR) throw IoError("truncate", name, errno);
  }
}

void seek_or_throw(int fd, std::uint64_t offset, const std::string& name) {
  if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) throw IoError("seek", name, errno);
}

void unlink_if_present(const std::string& path) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) throw IoError("remove", path, errno);
}

bool same_state(const Sha256::Midstate& a, const Sha256::Midstate& b) {
  return a.length == b.length && a.words == b.words;
}

}  // namespace

bool operator==(const TargetId& a, const TargetId& b) {
  return a.size == b.size && a.digest == b.digest && a.chunk_size == b.chunk_size;
}

std::string PartialOutput::data_path_for(const std::string& destination) {
  return hidden_sibling(destination, ".partial");
}

std::string PartialOutput::journal_path_for(const std::string& destination) {
  return hidden_sibling(destination, ".journal");
}

PartialOutput::PartialOutput(std::string destination, const TargetId& target,
                             const CheckProgress& progress)
    : destination_(std::move(destination)),
      data_path_(data_path_for(destination_)),
      journal_path_(journal_path_for(destination_)),
      target_(checked(target)),
      data_fd_(open_data_file(data_path_, journal_path_)),
      sink_(data_fd_.get(), data_path_),
      prefix_state_(Sha256().midstate()) {
  // flock() locks belong to the open file description, so they also keep a
  // second object in this process away, which fcntl() locks would not.
  for (;;) {
    if (::flock(data_fd_.get(), LOCK_EX | LOCK_NB) == 0) break;
    if (errno == EINTR) continue;
    if (errno == EWOULDBLOCK) {
      throw Error(destination_ + ": another transfer is writing this file");
    }
    throw IoError("lock", data_path_, errno);
  }
  if (try_resume(progress)) {
    resumed_ = verified_length_ > 0;
    // Whatever follows the verified prefix was never confirmed; drop it.
    truncate_or_throw(data_fd_.get(), verified_length_, data_path_);
    seek_or_throw(data_fd_.get(), verified_length_, data_path_);
    position_ = verified_length_;
  } else {
    restart();
  }
}

PartialOutput::~PartialOutput() = default;

bool PartialOutput::try_resume(const CheckProgress& progress) {
  JournalBytes journal;
  // Read one byte more than a journal holds so an overlong file is noticed.
  std::uint8_t raw[kJournalSize + 1];
  const long count = read_small_file(journal_path_, raw, sizeof(raw));
  if (count < 0) return false;  // nothing to resume; not worth a reason
  if (static_cast<std::size_t>(count) != kJournalSize) {
    restart_reason_ = "journal has the wrong size";
    return false;
  }
  std::memcpy(journal.data(), raw, kJournalSize);
  if (std::memcmp(journal.data(), kJournalMagic, sizeof(kJournalMagic)) != 0) {
    restart_reason_ = "journal has a bad magic number";
    return false;
  }
  if (journal[kVersionAt] != kJournalVersion) {
    restart_reason_ = "journal version " + std::to_string(journal[kVersionAt]) + " is unsupported";
    return false;
  }
  const Sha256Digest checksum = Sha256::hash(journal.data(), kChecksumAt);
  if (std::memcmp(checksum.data(), journal.data() + kChecksumAt, checksum.size()) != 0) {
    restart_reason_ = "journal checksum mismatch";
    return false;
  }

  TargetId recorded;
  recorded.size = load_be64(journal.data() + kTargetSizeAt);
  std::memcpy(recorded.digest.data(), journal.data() + kTargetDigestAt, recorded.digest.size());
  recorded.chunk_size = load_be64(journal.data() + kChunkSizeAt);
  if (recorded != target_) {
    restart_reason_ = "journal describes a different target";
    return false;
  }
  Sha256::Midstate state;
  state.length = load_be64(journal.data() + kVerifiedAt);
  for (std::size_t i = 0; i < state.words.size(); ++i) {
    state.words[i] = load_be32(journal.data() + kMidstateAt + 4 * i);
  }
  // A checksummed journal from this code always passes these, but a
  // malicious or buggy writer could produce one that does not.
  if (state.length > target_.size || state.length % target_.chunk_size != 0) {
    restart_reason_ = "journal records an impossible verified length";
    return false;
  }

  struct stat info;
  if (::fstat(data_fd_.get(), &info) != 0) throw IoError("stat", data_path_, errno);
  if (static_cast<std::uint64_t>(info.st_size) < state.length) {
    restart_reason_ = "partial file holds " + std::to_string(info.st_size) +
                      " bytes but the journal verified " + std::to_string(state.length);
    return false;
  }

  // The journal vouches for the prefix hash, not for the bytes on disk.
  // Rehash them, so damage to the partial file cannot slip through to a
  // file whose END check (which continues from this state) would pass.
  Sha256 hash;
  std::vector<std::uint8_t> buffer(kIoBufferSize);
  std::uint64_t offset = 0;
  while (offset < state.length) {
    const auto want =
        static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), state.length - offset));
    const ssize_t got = ::pread(data_fd_.get(), buffer.data(), want, static_cast<off_t>(offset));
    if (got < 0) {
      if (errno == EINTR) continue;
      throw IoError("read", data_path_, errno);
    }
    if (got == 0) {
      restart_reason_ = "partial file shrank while it was being checked";
      return false;
    }
    hash.update(buffer.data(), static_cast<std::size_t>(got));
    offset += static_cast<std::uint64_t>(got);
    if (progress) progress(offset);
  }
  if (!same_state(hash.midstate(), state)) {
    restart_reason_ = "partial file does not match the journal";
    return false;
  }
  verified_length_ = state.length;
  prefix_state_ = state;
  return true;
}

void PartialOutput::restart() {
  // One rename replaces the old journal with a claim on the data file that
  // verifies nothing, before the data is touched: a crash in between leaves
  // the file claimed and never claims more than is present. The claim is
  // not synced because it only has to outlive a crash of this process; after
  // a power failure an unclaimed data file is reported, not overwritten, and
  // a stale journal that did survive is checked against the data anyway.
  write_journal(0, Sha256().midstate(), /*durable=*/false);
  truncate_or_throw(data_fd_.get(), 0, data_path_);
  seek_or_throw(data_fd_.get(), 0, data_path_);
  verified_length_ = 0;
  prefix_state_ = Sha256().midstate();
  position_ = 0;
}

void PartialOutput::write(const std::uint8_t* data, std::size_t length) {
  require_open("write to");
  sink_.write(data, length);
  position_ += length;
}

void PartialOutput::flush() {
  require_open("flush");
  sink_.flush();
}

void PartialOutput::rewind() {
  require_open("rewind");
  // FileSink cannot drop its buffer, so the unverified bytes are written and
  // then cut off with the rest of the tail.
  sink_.flush();
  truncate_or_throw(data_fd_.get(), verified_length_, data_path_);
  seek_or_throw(data_fd_.get(), verified_length_, data_path_);
  position_ = verified_length_;
}

void PartialOutput::checkpoint(std::uint64_t verified_length, const Sha256& prefix) {
  require_open("checkpoint");
  if (verified_length > position_) {
    throw std::logic_error("checkpoint at " + std::to_string(verified_length) +
                           " is past the " + std::to_string(position_) + " bytes written");
  }
  if (prefix.length() != verified_length) {
    throw std::logic_error("checkpoint at " + std::to_string(verified_length) +
                           " with the hash state of " + std::to_string(prefix.length()) +
                           " bytes");
  }
  if (verified_length % target_.chunk_size != 0 || verified_length <= verified_length_) return;

  const Sha256::Midstate state = prefix.midstate();
  sink_.flush();
  sync_file(data_fd_.get(), data_path_);
  if (observer_) observer_(CheckpointStage::kDataSynced, verified_length);
  write_journal(verified_length, state);
  verified_length_ = verified_length;
  prefix_state_ = state;
  if (observer_) observer_(CheckpointStage::kJournalReplaced, verified_length);
}

Patcher::ChunkCallback PartialOutput::checkpoint_callback(const Patcher& patcher) {
  return [this, &patcher](std::uint64_t verified_length) {
    checkpoint(verified_length, patcher.file_hash());
  };
}

void PartialOutput::set_mode(mode_t mode) {
  require_open("set the mode of");
  has_mode_ = true;
  mode_ = mode & 07777;
}

void PartialOutput::commit() {
  require_open("commit");
  if (position_ != target_.size) {
    throw std::logic_error("commit after " + std::to_string(position_) + " of " +
                           std::to_string(target_.size) + " bytes");
  }
  sink_.flush();
  const mode_t mode = has_mode_ ? mode_ : new_file_mode(parent_directory(destination_));
  if (::fchmod(data_fd_.get(), mode) != 0) throw IoError("chmod", data_path_, errno);
  sync_file(data_fd_.get(), data_path_);
  if (::rename(data_path_.c_str(), destination_.c_str()) != 0) {
    throw IoError("rename", data_path_ + " to " + destination_, errno);
  }
  closed_ = true;
  sync_directory(parent_directory(destination_));
  // A journal that outlives a crash here is harmless: its data file is gone,
  // so the next attempt finds nothing to resume and starts over.
  unlink_if_present(journal_path_);
  data_fd_.reset();
}

void PartialOutput::abandon() {
  require_open("abandon");
  closed_ = true;
  // Data first: a crash in between leaves a journal without data (harmless),
  // never data that no journal claims.
  unlink_if_present(data_path_);
  remove_journal();
  data_fd_.reset();
}

void PartialOutput::write_journal(std::uint64_t verified_length, const Sha256::Midstate& state,
                                  bool durable) {
  JournalBytes journal{};
  std::memcpy(journal.data(), kJournalMagic, sizeof(kJournalMagic));
  journal[kVersionAt] = kJournalVersion;
  store_be64(journal.data() + kTargetSizeAt, target_.size);
  std::memcpy(journal.data() + kTargetDigestAt, target_.digest.data(), target_.digest.size());
  store_be64(journal.data() + kChunkSizeAt, target_.chunk_size);
  store_be64(journal.data() + kVerifiedAt, verified_length);
  for (std::size_t i = 0; i < state.words.size(); ++i) {
    store_be32(journal.data() + kMidstateAt + 4 * i, state.words[i]);
  }
  const Sha256Digest checksum = Sha256::hash(journal.data(), kChecksumAt);
  std::memcpy(journal.data() + kChecksumAt, checksum.data(), checksum.size());

  // Write a complete new journal beside the old one and rename it into
  // place, so a crash leaves either the old or the new journal, whole.
  const std::string temp_path = journal_path_ + ".tmp";
  {
    UniqueFd fd;
    for (;;) {
      const int raw = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (raw >= 0) {
        fd.reset(raw);
        break;
      }
      if (errno != EINTR) throw IoError("create", temp_path, errno);
    }
    write_all(fd.get(), journal.data(), journal.size(), temp_path);
    if (durable) sync_file(fd.get(), temp_path);
  }
  if (::rename(temp_path.c_str(), journal_path_.c_str()) != 0) {
    throw IoError("rename", temp_path + " to " + journal_path_, errno);
  }
  if (durable) sync_directory(parent_directory(journal_path_));
}

void PartialOutput::remove_journal() {
  bool removed = false;
  for (const std::string& path : {journal_path_, journal_path_ + ".tmp"}) {
    if (::unlink(path.c_str()) == 0) {
      removed = true;
    } else if (errno != ENOENT) {
      throw IoError("remove", path, errno);
    }
  }
  if (removed) sync_directory(parent_directory(journal_path_));
}

void PartialOutput::require_open(const char* what) const {
  if (closed_) {
    throw std::logic_error(std::string("cannot ") + what +
                           " a PartialOutput after commit() or abandon()");
  }
}

}  // namespace deltasync
