#ifndef DELTASYNC_FILE_H_
#define DELTASYNC_FILE_H_

// POSIX file implementations of the stream interfaces, plus AtomicFile for
// replacing a file so readers never observe a partially written version.

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "deltasync/io.h"

namespace deltasync {

// Owns a file descriptor and closes it on destruction.
class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept;
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  ~UniqueFd() { reset(); }

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

  // Gives up ownership without closing.
  int release();

  // Closes the current descriptor (errors ignored) and takes `fd`.
  void reset(int fd = -1);

 private:
  int fd_ = -1;
};

// Sequential reads from a file.
class FileSource final : public ByteSource {
 public:
  // Opens `path` read-only; throws IoError.
  explicit FileSource(const std::string& path);

  // Reads from a descriptor owned by the caller, e.g. standard input.
  FileSource(int fd, std::string name);

  std::size_t read(std::uint8_t* out, std::size_t capacity) override;

  int fd() const { return fd_; }
  const std::string& name() const { return name_; }

  // Current size of the file. Throws Error if it is not a regular file
  // (a pipe or terminal has no size to report).
  std::uint64_t size() const;

 private:
  UniqueFd owned_;
  int fd_;
  std::string name_;
};

// Positional reads from a file whose size is fixed when it is opened. A file
// that shrinks while it is being read produces an error, never short data.
class RandomAccessFile final : public RandomAccessSource {
 public:
  explicit RandomAccessFile(const std::string& path);

  std::uint64_t size() const override { return size_; }
  void read_at(std::uint64_t offset, std::uint8_t* out, std::size_t length) override;

  // Permission bits of the file when it was opened.
  mode_t mode() const { return mode_; }
  const std::string& path() const { return path_; }

 private:
  UniqueFd fd_;
  std::string path_;
  std::uint64_t size_ = 0;
  mode_t mode_ = 0;
};

// Buffered writes to a descriptor owned by the caller (e.g. standard output).
// Call flush() before discarding it; the destructor does not write, since it
// could not report a failure.
class FileSink final : public ByteSink {
 public:
  FileSink(int fd, std::string name, std::size_t buffer_size = kIoBufferSize);

  void write(const std::uint8_t* data, std::size_t length) override;
  void flush() override;

 private:
  void write_through(const std::uint8_t* data, std::size_t length);

  int fd_;
  std::string name_;
  std::vector<std::uint8_t> buffer_;
  std::size_t used_ = 0;
};

// Creates or replaces the file at `path` atomically. Bytes go to a
// temporary file in the same directory; commit() makes them durable and
// renames the temporary file over `path`, so `path` always holds either its
// old contents or the complete new contents. Without a successful commit(),
// the destructor deletes the temporary file and `path` is untouched.
class AtomicFile final : public ByteSink {
 public:
  explicit AtomicFile(std::string path);
  ~AtomicFile() override;

  AtomicFile(const AtomicFile&) = delete;
  AtomicFile& operator=(const AtomicFile&) = delete;

  void write(const std::uint8_t* data, std::size_t length) override;
  void flush() override;

  // Permission bits for the committed file. Without a call, it gets what a
  // newly created file in its directory would get (0666 minus the process
  // umask, as reported by new_file_mode()). Either way the temporary file is
  // owner-only (0600) while it is being written and receives its final mode
  // during commit(), just before the rename.
  void set_mode(mode_t mode);

  // Flushes, syncs the data to stable storage, renames the temporary file
  // over the destination, and syncs the directory entry.
  void commit();

  const std::string& path() const { return path_; }
  const std::string& temp_path() const { return temp_path_; }
  bool committed() const { return committed_; }

 private:
  std::string path_;
  std::string temp_path_;
  UniqueFd fd_;
  FileSink sink_;
  bool has_mode_ = false;
  mode_t mode_ = 0;
  bool committed_ = false;
};

// Forces written data and the size of the file behind `fd` to stable storage.
void sync_file(int fd, const std::string& name);

// Makes renames and creations in `directory` durable.
void sync_directory(const std::string& directory);

// Permission bits a file newly created with mode 0666 in `directory` would
// get (0666 minus the umask, or as a default ACL dictates). Determined by
// creating and removing a probe file, since reading the umask means changing
// it, which is not thread-safe.
mode_t new_file_mode(const std::string& directory);

// The directory part of `path` ("." when there is none).
std::string parent_directory(const std::string& path);

}  // namespace deltasync

#endif  // DELTASYNC_FILE_H_
