#include "deltasync/file.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

#include "deltasync/error.h"

namespace deltasync {
namespace {

// Some systems reject single read/write calls above INT_MAX bytes.
constexpr std::size_t kMaxSystemIo = std::size_t{1} << 30;

UniqueFd open_or_throw(const std::string& path, int flags, mode_t mode = 0) {
  for (;;) {
    const int fd = ::open(path.c_str(), flags | O_CLOEXEC, mode);
    if (fd >= 0) return UniqueFd(fd);
    if (errno != EINTR) throw IoError("open", path, errno);
  }
}

struct stat stat_or_throw(int fd, const std::string& name) {
  struct stat info;
  if (::fstat(fd, &info) != 0) throw IoError("stat", name, errno);
  return info;
}

std::string base_name(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string random_suffix() {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::random_device device;
  std::uint64_t bits = (static_cast<std::uint64_t>(device()) << 32) ^ device();
  std::string out;
  for (int i = 0; i < 12; ++i, bits >>= 4) out.push_back(kDigits[bits & 0xf]);
  return out;
}

// Cuts `name` to at most `limit` bytes without splitting a UTF-8 sequence:
// some filesystems (APFS among them) reject names that are not valid UTF-8.
std::string truncate_name(std::string name, std::size_t limit) {
  if (name.size() <= limit) return name;
  std::size_t cut = limit;
  // Back up over continuation bytes (10xxxxxx) to the start of a character.
  while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
  name.resize(cut);
  return name;
}

// Creates a new, empty file next to `path`, readable and writable only by
// the owner: the final permissions are applied with fchmod() later, so the
// data being written is never exposed through a more permissive temporary
// file.
UniqueFd create_temp_file(const std::string& path, std::string* temp_path) {
  std::string directory = parent_directory(path);
  if (directory.back() != '/') directory.push_back('/');
  // Leave room for the prefix and suffix within the 255-byte name limit.
  const std::string base = truncate_name(base_name(path), 200);
  for (int attempt = 0; attempt < 64; ++attempt) {
    std::string candidate = directory + "." + base + ".tmp-" + random_suffix();
    const int fd = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) {
      *temp_path = std::move(candidate);
      return UniqueFd(fd);
    }
    if (errno != EEXIST && errno != EINTR) throw IoError("create", candidate, errno);
  }
  throw Error("could not create a temporary file next to " + path);
}

}  // namespace

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
  if (this != &other) reset(other.release());
  return *this;
}

int UniqueFd::release() {
  const int fd = fd_;
  fd_ = -1;
  return fd;
}

void UniqueFd::reset(int fd) {
  if (fd_ >= 0) ::close(fd_);
  fd_ = fd;
}

FileSource::FileSource(const std::string& path)
    : owned_(open_or_throw(path, O_RDONLY)), fd_(owned_.get()), name_(path) {}

FileSource::FileSource(int fd, std::string name) : fd_(fd), name_(std::move(name)) {}

std::size_t FileSource::read(std::uint8_t* out, std::size_t capacity) {
  for (;;) {
    const ssize_t count = ::read(fd_, out, std::min(capacity, kMaxSystemIo));
    if (count >= 0) return static_cast<std::size_t>(count);
    if (errno != EINTR) throw IoError("read", name_, errno);
  }
}

std::uint64_t FileSource::size() const {
  const struct stat info = stat_or_throw(fd_, name_);
  if (!S_ISREG(info.st_mode)) throw Error(name_ + ": not a regular file");
  return static_cast<std::uint64_t>(info.st_size);
}

RandomAccessFile::RandomAccessFile(const std::string& path)
    : fd_(open_or_throw(path, O_RDONLY)), path_(path) {
  const struct stat info = stat_or_throw(fd_.get(), path_);
  if (!S_ISREG(info.st_mode)) throw Error(path_ + ": not a regular file");
  size_ = static_cast<std::uint64_t>(info.st_size);
  mode_ = info.st_mode & 07777;
}

void RandomAccessFile::read_at(std::uint64_t offset, std::uint8_t* out, std::size_t length) {
  if (offset > size_ || length > size_ - offset) {
    throw Error(path_ + ": read of " + std::to_string(length) + " bytes at offset " +
                std::to_string(offset) + " is past the end of the file");
  }
  while (length > 0) {
    const ssize_t count = ::pread(fd_.get(), out, std::min(length, kMaxSystemIo),
                                  static_cast<off_t>(offset));
    if (count < 0) {
      if (errno == EINTR) continue;
      throw IoError("read", path_, errno);
    }
    if (count == 0) throw Error(path_ + ": file shrank while it was being read");
    out += count;
    offset += static_cast<std::uint64_t>(count);
    length -= static_cast<std::size_t>(count);
  }
}

FileSink::FileSink(int fd, std::string name, std::size_t buffer_size)
    : fd_(fd), name_(std::move(name)), buffer_(buffer_size) {}

void FileSink::write(const std::uint8_t* data, std::size_t length) {
  if (length == 0) return;
  if (length <= buffer_.size() - used_) {
    std::memcpy(buffer_.data() + used_, data, length);
    used_ += length;
    return;
  }
  flush();
  if (length >= buffer_.size()) {
    // Large writes bypass the buffer instead of being copied through it.
    write_through(data, length);
    return;
  }
  std::memcpy(buffer_.data(), data, length);
  used_ = length;
}

void FileSink::flush() {
  if (used_ == 0) return;
  write_through(buffer_.data(), used_);
  used_ = 0;
}

void FileSink::write_through(const std::uint8_t* data, std::size_t length) {
  while (length > 0) {
    const ssize_t count = ::write(fd_, data, std::min(length, kMaxSystemIo));
    if (count < 0) {
      if (errno == EINTR) continue;
      throw IoError("write", name_, errno);
    }
    if (count == 0) throw IoError("write", name_, EIO);
    data += count;
    length -= static_cast<std::size_t>(count);
  }
}

AtomicFile::AtomicFile(std::string path)
    : path_(std::move(path)),
      fd_(create_temp_file(path_, &temp_path_)),
      sink_(fd_.get(), temp_path_) {}

AtomicFile::~AtomicFile() {
  if (!committed_) {
    fd_.reset();
    ::unlink(temp_path_.c_str());
  }
}

void AtomicFile::write(const std::uint8_t* data, std::size_t length) {
  if (committed_) throw std::logic_error("write to an AtomicFile after commit");
  sink_.write(data, length);
}

void AtomicFile::flush() { sink_.flush(); }

void AtomicFile::set_mode(mode_t mode) {
  if (committed_) throw std::logic_error("set_mode on an AtomicFile after commit");
  has_mode_ = true;
  mode_ = mode & 07777;
}

void AtomicFile::commit() {
  if (committed_) throw std::logic_error("AtomicFile committed twice");
  sink_.flush();
  // The temporary file was created owner-only; it gets its final
  // permissions just before it becomes visible under its real name.
  const mode_t mode = has_mode_ ? mode_ : new_file_mode(parent_directory(path_));
  if (::fchmod(fd_.get(), mode) != 0) throw IoError("chmod", temp_path_, errno);
  sync_file(fd_.get(), temp_path_);
  // POSIX leaves the descriptor state unspecified after EINTR; on the
  // supported systems it is closed, so only other errors are reported.
  if (::close(fd_.release()) != 0 && errno != EINTR) throw IoError("close", temp_path_, errno);
  if (::rename(temp_path_.c_str(), path_.c_str()) != 0) {
    throw IoError("rename", temp_path_ + " to " + path_, errno);
  }
  committed_ = true;
  sync_directory(parent_directory(path_));
}

void sync_file(int fd, const std::string& name) {
#if defined(__APPLE__)
  // On macOS fsync() only hands the data to the drive; F_FULLFSYNC also
  // flushes the drive's write cache. Filesystems without support for it
  // fall through to fsync().
  if (::fcntl(fd, F_FULLFSYNC) == 0) return;
#endif
  for (;;) {
#if defined(__linux__)
    // The data and the file size are what a reader needs after a crash;
    // fdatasync() skips flushing unrelated metadata such as timestamps.
    const int rc = ::fdatasync(fd);
#else
    const int rc = ::fsync(fd);
#endif
    if (rc == 0) return;
    if (errno != EINTR) throw IoError("sync", name, errno);
  }
}

void sync_directory(const std::string& directory) {
  UniqueFd fd = open_or_throw(directory, O_RDONLY | O_DIRECTORY);
#if defined(__APPLE__)
  // As for files, only F_FULLFSYNC gets the directory update past the
  // drive's write cache on macOS.
  if (::fcntl(fd.get(), F_FULLFSYNC) == 0) return;
#endif
  for (;;) {
    if (::fsync(fd.get()) == 0) return;
    if (errno == EINTR) continue;
    // A few filesystems cannot sync directories and report EINVAL; there is
    // nothing more to do on those.
    if (errno == EINVAL) return;
    throw IoError("sync", directory, errno);
  }
}

mode_t new_file_mode(const std::string& directory) {
  // The process umask cannot be read without changing it, which would race
  // with other threads creating files. Creating a probe file shows the
  // permissions the system actually grants a new file here, including any
  // default ACL of the directory.
  std::string prefix = directory;
  if (prefix.empty()) prefix = ".";
  if (prefix.back() != '/') prefix.push_back('/');
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::string probe = prefix + ".deltasync-mode-" + random_suffix();
    const int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) {
      if (errno == EEXIST || errno == EINTR) continue;
      throw IoError("create", probe, errno);
    }
    UniqueFd owned(fd);
    struct stat info;
    const int rc = ::fstat(fd, &info);
    const int saved_errno = errno;
    ::unlink(probe.c_str());
    if (rc != 0) throw IoError("stat", probe, saved_errno);
    return info.st_mode & 07777;
  }
  throw Error("could not create a probe file in " + directory);
}

std::string parent_directory(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

}  // namespace deltasync
