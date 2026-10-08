#include "deltasync/file.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <ios>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "deltasync/error.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;

TEST(FileSourceTest, ReadsTheWholeFileInPieces) {
  test::TempDir dir;
  const Bytes data = test::random_bytes(100000, 501);
  test::write_file(dir.file("f"), data);
  FileSource source(dir.file("f"));
  EXPECT_EQ(source.size(), data.size());
  Bytes read;
  std::uint8_t buffer[777];
  for (;;) {
    const std::size_t count = source.read(buffer, sizeof(buffer));
    if (count == 0) break;
    read.insert(read.end(), buffer, buffer + count);
  }
  EXPECT_TRUE(test::same_bytes(data, read));
}

TEST(FileSourceTest, MissingFileReportsErrno) {
  test::TempDir dir;
  try {
    FileSource source(dir.file("missing"));
    FAIL() << "expected IoError";
  } catch (const IoError& error) {
    EXPECT_EQ(error.error_number(), ENOENT);
    EXPECT_NE(std::string(error.what()).find("missing"), std::string::npos);
  }
}

TEST(FileSourceTest, PipesHaveNoSize) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  UniqueFd read_end(fds[0]);
  UniqueFd write_end(fds[1]);
  FileSource source(read_end.get(), "pipe");
  EXPECT_THROW(source.size(), Error);
  ASSERT_EQ(::write(write_end.get(), "abc", 3), 3);
  write_end.reset();
  std::uint8_t buffer[8];
  EXPECT_EQ(source.read(buffer, sizeof(buffer)), 3u);
  EXPECT_EQ(source.read(buffer, sizeof(buffer)), 0u);
}

TEST(RandomAccessFileTest, ReadsRangesAndRejectsOutOfBounds) {
  test::TempDir dir;
  const Bytes data = test::random_bytes(5000, 502);
  test::write_file(dir.file("f"), data);
  RandomAccessFile file(dir.file("f"));
  EXPECT_EQ(file.size(), 5000u);
  Bytes out(100);
  file.read_at(4900, out.data(), 100);
  EXPECT_EQ(out, Bytes(data.begin() + 4900, data.end()));
  file.read_at(0, out.data(), 0);
  EXPECT_THROW(file.read_at(4901, out.data(), 100), Error);
  EXPECT_THROW(file.read_at(5001, out.data(), 0), Error);
}

TEST(RandomAccessFileTest, NoticesAFileThatShrankWhileOpen) {
  test::TempDir dir;
  test::write_file(dir.file("f"), test::random_bytes(5000, 503));
  RandomAccessFile file(dir.file("f"));
  ASSERT_EQ(::truncate(dir.file("f").c_str(), 1000), 0);
  Bytes out(100);
  try {
    file.read_at(2000, out.data(), out.size());
    FAIL() << "expected an error";
  } catch (const Error& error) {
    EXPECT_NE(std::string(error.what()).find("shrank"), std::string::npos);
  }
}

TEST(RandomAccessFileTest, RejectsDirectories) {
  test::TempDir dir;
  EXPECT_THROW(RandomAccessFile file(dir.path()), Error);
}

TEST(FileSinkTest, BuffersSmallWritesAndPassesLargeOnes) {
  test::TempDir dir;
  const std::string path = dir.file("out");
  UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
  ASSERT_TRUE(fd.valid());
  const Bytes data = test::random_bytes(300000, 504);
  {
    FileSink sink(fd.get(), path, 1024);
    test::Rng rng(505);
    std::size_t offset = 0;
    while (offset < data.size()) {
      // A mix of tiny writes and writes larger than the buffer.
      const std::size_t piece = std::min<std::size_t>(
          rng.below(4) == 0 ? rng.between(1000, 5000) : rng.between(0, 100), data.size() - offset);
      sink.write(data.data() + offset, piece);
      offset += piece;
    }
    sink.flush();
  }
  EXPECT_TRUE(test::same_bytes(data, test::read_file(path)));
}

TEST(AtomicFileTest, NothingIsVisibleBeforeCommit) {
  test::TempDir dir;
  const std::string path = dir.file("target");
  AtomicFile file(path);
  const Bytes data = test::random_bytes(10000, 506);
  file.write(data.data(), data.size());
  file.flush();
  EXPECT_FALSE(test::file_exists(path));
  // The temporary file lives next to the destination so rename() is atomic.
  EXPECT_EQ(parent_directory(file.temp_path()), dir.path());
  EXPECT_EQ(file.temp_path().rfind(dir.file(".target.tmp-"), 0), 0u);
  file.commit();
  EXPECT_TRUE(file.committed());
  EXPECT_TRUE(test::same_bytes(data, test::read_file(path)));
  EXPECT_EQ(test::list_directory(dir.path()), (std::vector<std::string>{"target"}));
}

TEST(AtomicFileTest, AbandonedFileLeavesTheOldVersion) {
  test::TempDir dir;
  const std::string path = dir.file("target");
  test::write_file(path, test::bytes_of("old"));
  {
    AtomicFile file(path);
    const Bytes data(5000, 1);
    file.write(data.data(), data.size());
    EXPECT_EQ(test::list_directory(dir.path()).size(), 2u);
  }
  EXPECT_EQ(test::read_file(path), test::bytes_of("old"));
  EXPECT_EQ(test::list_directory(dir.path()), (std::vector<std::string>{"target"}));
}

TEST(AtomicFileTest, CommitReplacesAndAppliesTheMode) {
  test::TempDir dir;
  const std::string path = dir.file("target");
  test::write_file(path, test::bytes_of("old"));
  AtomicFile file(path);
  file.write(reinterpret_cast<const std::uint8_t*>("new"), 3);
  file.set_mode(0604);
  file.commit();
  EXPECT_EQ(test::read_file(path), test::bytes_of("new"));
  struct stat info;
  ASSERT_EQ(::stat(path.c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 07777, 0604u);
  EXPECT_THROW(file.commit(), std::logic_error);
  EXPECT_THROW(file.write(reinterpret_cast<const std::uint8_t*>("x"), 1), std::logic_error);
}

// A destination meant to be private must not be readable by others through
// its temporary file while the data is being written.
TEST(AtomicFileTest, TemporaryFileIsOwnerOnlyUntilCommit) {
  for (const mode_t final_mode : {mode_t{0600}, mode_t{0644}, mode_t{0755}}) {
    SCOPED_TRACE(final_mode);
    test::TempDir dir;
    const std::string path = dir.file("target");
    AtomicFile file(path);
    file.set_mode(final_mode);
    const Bytes data = test::random_bytes(100000, 507);
    file.write(data.data(), data.size());
    file.flush();
    struct stat info;
    ASSERT_EQ(::stat(file.temp_path().c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 077, 0u) << std::oct << (info.st_mode & 07777);
    file.commit();
    ASSERT_EQ(::stat(path.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, final_mode);
  }
}

TEST(AtomicFileTest, WithoutAModeTheFileGetsTheDefaultPermissions) {
  test::TempDir dir;
  // What open(..., 0666) gives a new file here, i.e. 0666 minus the umask.
  const std::string reference = dir.file("reference");
  {
    UniqueFd fd(::open(reference.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666));
    ASSERT_TRUE(fd.valid());
  }
  struct stat expected;
  ASSERT_EQ(::stat(reference.c_str(), &expected), 0);
  EXPECT_EQ(new_file_mode(dir.path()), expected.st_mode & 07777);

  const std::string path = dir.file("target");
  AtomicFile file(path);
  file.write(reinterpret_cast<const std::uint8_t*>("abc"), 3);
  file.commit();
  struct stat info;
  ASSERT_EQ(::stat(path.c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 07777, expected.st_mode & 07777);
  // The probe used to learn the default mode is gone.
  EXPECT_EQ(test::list_directory(dir.path()), (std::vector<std::string>{"reference", "target"}));
}

bool is_valid_utf8(const std::string& text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    std::size_t extra = 0;
    if (lead < 0x80) {
      extra = 0;
    } else if ((lead & 0xE0) == 0xC0) {
      extra = 1;
    } else if ((lead & 0xF0) == 0xE0) {
      extra = 2;
    } else if ((lead & 0xF8) == 0xF0) {
      extra = 3;
    } else {
      return false;
    }
    if (i + extra >= text.size() && extra > 0) return false;  // truncated sequence
    for (std::size_t k = 1; k <= extra; ++k) {
      if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
    }
    i += extra + 1;
  }
  return true;
}

// Long names are shortened inside the temporary name; the cut must not split
// a multi-byte character, or filesystems that require UTF-8 names (APFS)
// refuse to create the file.
TEST(AtomicFileTest, LongMultiByteNamesAreShortenedOnCharacterBoundaries) {
  test::TempDir dir;
  std::string name = "a";
  for (int i = 0; i < 120; ++i) name += "\xc3\xa9";  // U+00E9, two bytes
  ASSERT_EQ(name.size(), 241u);
  ASSERT_TRUE(is_valid_utf8(name));
  // A plain 200-byte cut would end in the middle of a character.
  ASSERT_FALSE(is_valid_utf8(name.substr(0, 200)));

  const std::string path = dir.file(name);
  AtomicFile file(path);
  const std::string temp_name = file.temp_path().substr(dir.path().size() + 1);
  EXPECT_TRUE(is_valid_utf8(temp_name));
  EXPECT_LE(temp_name.size(), 255u);
  file.write(reinterpret_cast<const std::uint8_t*>("abc"), 3);
  file.commit();
  EXPECT_EQ(test::read_file(path), test::bytes_of("abc"));
}

TEST(SyncTest, SyncsDirectoriesAndReportsMissingOnes) {
  test::TempDir dir;
  EXPECT_NO_THROW(sync_directory(dir.path()));
  EXPECT_THROW(sync_directory(dir.file("missing")), IoError);
  const std::string path = dir.file("f");
  test::write_file(path, test::bytes_of("data"));
  UniqueFd fd(::open(path.c_str(), O_RDWR));
  ASSERT_TRUE(fd.valid());
  EXPECT_NO_THROW(sync_file(fd.get(), path));
}

TEST(AtomicFileTest, MissingDirectoryIsAnIoError) {
  test::TempDir dir;
  EXPECT_THROW(AtomicFile file(dir.file("no/such/dir/target")), IoError);
}

TEST(FilePathTest, ParentDirectory) {
  EXPECT_EQ(parent_directory("a/b/c"), "a/b");
  EXPECT_EQ(parent_directory("c"), ".");
  EXPECT_EQ(parent_directory("/c"), "/");
  EXPECT_EQ(parent_directory("/a/b"), "/a");
  EXPECT_EQ(parent_directory("dir with space /file"), "dir with space ");
}

TEST(UniqueFdTest, MovesOwnership) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  UniqueFd a(fds[0]);
  UniqueFd b(fds[1]);
  UniqueFd moved(std::move(a));
  EXPECT_FALSE(a.valid());
  EXPECT_EQ(moved.get(), fds[0]);
  moved = std::move(b);
  EXPECT_EQ(moved.get(), fds[1]);
  // The first descriptor was closed by the assignment.
  EXPECT_EQ(::fcntl(fds[0], F_GETFD), -1);
  const int released = moved.release();
  EXPECT_FALSE(moved.valid());
  EXPECT_EQ(::close(released), 0);
}

}  // namespace
}  // namespace deltasync
