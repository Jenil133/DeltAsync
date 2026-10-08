#include "deltasync/patch.h"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "deltasync/error.h"
#include "deltasync/file.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;
using test::Record;
using Kind = test::Record::Kind;

struct Fixture {
  Bytes basis;
  Bytes target;
  test::RecordingSink records;  // the encoder's output, unserialized
};

// A basis and an edited target, encoded with small chunks so a delta has
// many CHUNK records.
Fixture make_fixture(std::uint64_t chunk_size = 1024) {
  Fixture fixture;
  fixture.basis = test::random_bytes(10000, 101);
  fixture.target = fixture.basis;
  fixture.target.insert(fixture.target.begin() + 4321, 64, 0x5a);
  fixture.target.erase(fixture.target.begin() + 8000, fixture.target.begin() + 8100);
  DeltaOptions options;
  options.chunk_size = chunk_size;
  const Signature signature = test::make_signature(fixture.basis, 256);
  DeltaEncoder encoder(signature, fixture.records, options);
  encoder.feed(fixture.target.data(), fixture.target.size());
  encoder.finish();
  return fixture;
}

// Applies records directly (no serialization) and returns the output.
Bytes apply_records(const Bytes& basis, const test::RecordingSink& records,
                    Patcher::ChunkCallback callback = nullptr) {
  MemoryRandomAccess source(basis);
  MemorySink output;
  Patcher patcher(source, output);
  if (callback) patcher.set_chunk_callback(std::move(callback));
  records.replay(patcher);
  EXPECT_TRUE(patcher.finished());
  return output.take();
}

TEST(PatcherTest, AppliesEncoderOutputDirectly) {
  const Fixture fixture = make_fixture();
  EXPECT_TRUE(test::same_bytes(fixture.target, apply_records(fixture.basis, fixture.records)));
}

TEST(PatcherTest, ReportsEachVerifiedChunk) {
  const Fixture fixture = make_fixture(1024);
  std::vector<std::uint64_t> verified;
  apply_records(fixture.basis, fixture.records,
                [&](std::uint64_t length) { verified.push_back(length); });
  std::vector<std::uint64_t> expected;
  for (std::uint64_t end = 1024; end < fixture.target.size(); end += 1024) expected.push_back(end);
  expected.push_back(fixture.target.size());
  EXPECT_EQ(verified, expected);
}

// The state a resume journal would store at a chunk boundary must let a new
// Patcher (and a new encoder) finish the same target as if nothing happened.
TEST(PatcherTest, ExposesThePrefixStateAtEachVerifiedChunk) {
  const Fixture fixture = make_fixture(1024);
  MemoryRandomAccess source(fixture.basis);
  MemorySink output;
  Patcher patcher(source, output);
  std::vector<std::pair<std::uint64_t, Sha256::Midstate>> states;
  patcher.set_chunk_callback([&](std::uint64_t verified) {
    EXPECT_EQ(patcher.file_hash().length(), verified);
    // A digest of the prefix computed independently must agree.
    EXPECT_EQ(patcher.file_hash().digest(), Sha256::hash(fixture.target.data(), verified));
    if (verified % Sha256::kBlockLength == 0) {
      states.emplace_back(verified, patcher.file_hash().midstate());
    }
  });
  fixture.records.replay(patcher);
  ASSERT_TRUE(patcher.finished());
  ASSERT_GE(states.size(), 5u);

  const Signature signature = test::make_signature(fixture.basis, 256);
  DeltaOptions options;
  options.chunk_size = 1024;
  for (const auto& entry : states) {
    const std::uint64_t resume_at = entry.first;
    SCOPED_TRACE("resume at " + std::to_string(resume_at));
    EXPECT_EQ(entry.second.length, resume_at);
    // Both ends rebuild their hashers from the stored midstate alone.
    const Sha256 sender_prefix = Sha256::from_midstate(entry.second);
    const Sha256 receiver_prefix = Sha256::from_midstate(entry.second);

    MemorySink delta;
    DeltaWriter writer(delta);
    DeltaEncoder encoder(signature, writer, options, sender_prefix);
    encoder.feed(fixture.target.data() + resume_at, fixture.target.size() - resume_at);
    encoder.finish();

    MemorySink suffix;
    Patcher resumed(source, suffix, receiver_prefix);
    DeltaDecoder decoder(resumed);
    decoder.feed(delta.data().data(), delta.data().size());
    decoder.finish();
    ASSERT_TRUE(resumed.finished());
    Bytes rebuilt(fixture.target.begin(), fixture.target.begin() + resume_at);
    rebuilt.insert(rebuilt.end(), suffix.data().begin(), suffix.data().end());
    EXPECT_TRUE(test::same_bytes(fixture.target, rebuilt));
    EXPECT_EQ(resumed.file_hash().digest(),
              Sha256::hash(fixture.target.data(), fixture.target.size()));
  }
}

// A wrong prefix state is caught by END even when every chunk after the
// resume point is correct.
TEST(PatcherTest, ResumingFromAWrongPrefixStateFailsAtEnd) {
  const Fixture fixture = make_fixture(1024);
  Sha256 sender_prefix;
  sender_prefix.update(fixture.target.data(), 2048);
  Bytes wrong(fixture.target.begin(), fixture.target.begin() + 2048);
  wrong[100] ^= 1;
  Sha256 receiver_prefix;
  receiver_prefix.update(wrong.data(), wrong.size());

  const Signature signature = test::make_signature(fixture.basis, 256);
  DeltaOptions options;
  options.chunk_size = 1024;
  test::RecordingSink records;
  DeltaEncoder encoder(signature, records, options, sender_prefix);
  encoder.feed(fixture.target.data() + 2048, fixture.target.size() - 2048);
  encoder.finish();

  MemoryRandomAccess source(fixture.basis);
  MemorySink suffix;
  Patcher patcher(source, suffix, Sha256::from_midstate(receiver_prefix.midstate()));
  EXPECT_THROW(records.replay(patcher), IntegrityError);
  EXPECT_FALSE(patcher.finished());
}

TEST(PatcherTest, StatsDescribeTheOutput) {
  const Fixture fixture = make_fixture();
  MemoryRandomAccess source(fixture.basis);
  MemorySink output;
  Patcher patcher(source, output);
  fixture.records.replay(patcher);
  const PatchStats& stats = patcher.stats();
  EXPECT_EQ(stats.output_bytes, fixture.target.size());
  EXPECT_EQ(stats.copied_bytes + stats.literal_bytes, fixture.target.size());
  EXPECT_GT(stats.copied_bytes, 9000u);
  EXPECT_EQ(stats.chunks_verified, (fixture.target.size() + 1023) / 1024);
}

TEST(PatcherTest, RejectsABasisOfTheWrongSize) {
  const Fixture fixture = make_fixture();
  Bytes shorter = fixture.basis;
  shorter.pop_back();
  try {
    apply_records(shorter, fixture.records);
    FAIL() << "expected IntegrityError";
  } catch (const IntegrityError& error) {
    EXPECT_NE(std::string(error.what()).find("10000-byte basis"), std::string::npos);
  }
}

TEST(PatcherTest, WrongBasisOfTheRightSizeFailsAtTheFirstAffectedChunk) {
  const Fixture fixture = make_fixture(1024);
  // Corrupt the basis inside the block that feeds target bytes 2048..3071.
  Bytes damaged = fixture.basis;
  damaged[2500] ^= 1;
  std::vector<std::uint64_t> verified;
  EXPECT_THROW(apply_records(damaged, fixture.records,
                             [&](std::uint64_t length) { verified.push_back(length); }),
               IntegrityError);
  // The two chunks before the damage were verified; nothing after it was.
  EXPECT_EQ(verified, (std::vector<std::uint64_t>{1024, 2048}));
}

TEST(PatcherTest, DetectsAlteredLiteralData) {
  const Fixture fixture = make_fixture();
  test::RecordingSink altered = fixture.records;
  bool changed = false;
  for (Record& record : altered.records) {
    if (record.kind == Kind::kLiteral && !changed) {
      record.data[0] ^= 0x80;
      changed = true;
    }
  }
  ASSERT_TRUE(changed);
  EXPECT_THROW(apply_records(fixture.basis, altered), IntegrityError);
}

TEST(PatcherTest, DetectsACopyOfTheWrongBlock) {
  const Fixture fixture = make_fixture();
  test::RecordingSink altered = fixture.records;
  for (Record& record : altered.records) {
    if (record.kind == Kind::kCopy && record.offset >= 256) {
      record.offset -= 256;
      break;
    }
  }
  EXPECT_THROW(apply_records(fixture.basis, altered), IntegrityError);
}

TEST(PatcherTest, DetectsAlteredDigests) {
  const Fixture fixture = make_fixture();
  for (const Kind kind : {Kind::kChunk, Kind::kEnd}) {
    test::RecordingSink altered = fixture.records;
    for (auto it = altered.records.rbegin(); it != altered.records.rend(); ++it) {
      if (it->kind == kind) {
        it->digest[31] ^= 1;
        break;
      }
    }
    EXPECT_THROW(apply_records(fixture.basis, altered), IntegrityError);
  }
}

TEST(PatcherTest, RejectsRecordsOutOfOrder) {
  const Bytes basis = test::random_bytes(100, 102);
  MemoryRandomAccess source(basis);
  MemorySink output;
  Patcher patcher(source, output);
  EXPECT_THROW(patcher.on_copy(0, 10), FormatError);
  const std::uint8_t byte = 1;
  EXPECT_THROW(patcher.on_literal(&byte, 1), FormatError);

  DeltaHeader header;
  header.basis_size = basis.size();
  patcher.on_header(header);
  EXPECT_THROW(patcher.on_header(header), FormatError);
  EXPECT_THROW(patcher.on_copy(90, 11), FormatError);
  EXPECT_THROW(patcher.on_chunk(Sha256Digest{}), FormatError);  // nothing to cover yet

  patcher.on_end(0, Sha256().digest());
  EXPECT_TRUE(patcher.finished());
  EXPECT_THROW(patcher.on_literal(&byte, 1), FormatError);
}

TEST(PatcherTest, RejectsEndWithUnverifiedOutput) {
  const Bytes basis = test::random_bytes(100, 103);
  MemoryRandomAccess source(basis);
  MemorySink output;
  Patcher patcher(source, output);
  DeltaHeader header;
  header.basis_size = basis.size();
  patcher.on_header(header);
  patcher.on_copy(0, 10);
  EXPECT_THROW(patcher.on_end(10, Sha256::hash(basis.data(), 10)), FormatError);
}

TEST(PatcherTest, ResumeRequiresMatchingStartOffset) {
  const Bytes basis = test::random_bytes(100, 104);
  MemoryRandomAccess source(basis);
  MemorySink output;
  Sha256 prefix;
  prefix.update(basis.data(), 64);
  Patcher patcher(source, output, prefix);
  DeltaHeader header;
  header.chunk_size = 64;
  header.basis_size = basis.size();
  header.start_offset = 0;
  EXPECT_THROW(patcher.on_header(header), IntegrityError);
}

// Flipping any single bit of a serialized delta must never yield wrong
// output silently: either decoding or verification fails, or the output is
// still exactly the target.
TEST(PatcherTest, SingleBitFlipsNeverProduceWrongOutput) {
  const Fixture fixture = make_fixture(512);
  MemorySink serialized;
  DeltaWriter writer(serialized);
  fixture.records.replay(writer);
  const Bytes delta = serialized.take();

  test::Rng rng(105);
  int detected = 0;
  for (std::size_t bit = 0; bit < delta.size() * 8; bit += 1 + rng.below(13)) {
    Bytes damaged = delta;
    damaged[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
    try {
      const Bytes output = test::apply(fixture.basis, damaged);
      EXPECT_TRUE(test::same_bytes(fixture.target, output)) << "bit " << bit;
    } catch (const FormatError&) {
      ++detected;
    } catch (const IntegrityError&) {
      ++detected;
    }
  }
  EXPECT_GT(detected, 0);
}

class PatchFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    basis_ = test::random_bytes(50000, 106);
    target_ = basis_;
    target_.insert(target_.begin() + 25000, 500, 0x11);
    test::write_file(dir_.file("basis"), basis_);
    delta_ = test::encode(test::make_signature(basis_, 1024), target_).bytes;
  }

  PatchStats patch(const Bytes& delta, const std::string& basis_name,
                   const std::string& output_name) {
    MemorySource source(delta);
    return patch_file(dir_.file(basis_name), source, dir_.file(output_name));
  }

  test::TempDir dir_;
  Bytes basis_;
  Bytes target_;
  Bytes delta_;
};

TEST_F(PatchFileTest, WritesTheVerifiedTarget) {
  const PatchStats stats = patch(delta_, "basis", "out");
  EXPECT_TRUE(test::same_bytes(target_, test::read_file(dir_.file("out"))));
  EXPECT_EQ(stats.output_bytes, target_.size());
  EXPECT_EQ(test::list_directory(dir_.path()), (std::vector<std::string>{"basis", "out"}));
}

TEST_F(PatchFileTest, CanUpdateTheBasisInPlace) {
  patch(delta_, "basis", "basis");
  EXPECT_TRUE(test::same_bytes(target_, test::read_file(dir_.file("basis"))));
  EXPECT_EQ(test::list_directory(dir_.path()), (std::vector<std::string>{"basis"}));
}

TEST_F(PatchFileTest, FailureLeavesNoTraceAndKeepsTheOldOutput) {
  test::write_file(dir_.file("out"), test::bytes_of("previous contents"));
  Bytes corrupted = delta_;
  corrupted[corrupted.size() / 2] ^= 0x01;
  EXPECT_ANY_THROW(patch(corrupted, "basis", "out"));
  Bytes truncated(delta_.begin(), delta_.end() - 10);
  EXPECT_THROW(patch(truncated, "basis", "out"), FormatError);
  EXPECT_EQ(test::read_file(dir_.file("out")), test::bytes_of("previous contents"));
  EXPECT_EQ(test::list_directory(dir_.path()), (std::vector<std::string>{"basis", "out"}));
}

TEST_F(PatchFileTest, NewFileTakesTheBasisPermissions) {
  ASSERT_EQ(::chmod(dir_.file("basis").c_str(), 0640), 0);
  patch(delta_, "basis", "out");
  struct stat info;
  ASSERT_EQ(::stat(dir_.file("out").c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 07777, 0640u);
}

TEST_F(PatchFileTest, ReplacedFileKeepsItsPermissions) {
  test::write_file(dir_.file("out"), Bytes{1, 2, 3});
  ASSERT_EQ(::chmod(dir_.file("out").c_str(), 0600), 0);
  patch(delta_, "basis", "out");
  struct stat info;
  ASSERT_EQ(::stat(dir_.file("out").c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 07777, 0600u);
  EXPECT_TRUE(test::same_bytes(target_, test::read_file(dir_.file("out"))));
}

TEST_F(PatchFileTest, MissingBasisIsAnIoError) {
  try {
    patch(delta_, "no-such-basis", "out");
    FAIL() << "expected IoError";
  } catch (const IoError& error) {
    EXPECT_EQ(error.error_number(), ENOENT);
    EXPECT_NE(std::string(error.what()).find("no-such-basis"), std::string::npos);
  }
  EXPECT_FALSE(test::file_exists(dir_.file("out")));
}

}  // namespace
}  // namespace deltasync
