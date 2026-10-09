#include "deltasync/resume.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "deltasync/delta_encoder.h"
#include "deltasync/delta_format.h"
#include "deltasync/error.h"
#include "deltasync/file.h"
#include "deltasync/patch.h"
#include "test_util.h"

namespace deltasync {
namespace {

using test::Bytes;
using Stage = PartialOutput::CheckpointStage;

constexpr std::uint32_t kBlockSize = 256;

// A basis, an edited target, and what the receiving side knows about the
// target before any data arrives.
struct Scenario {
  Bytes basis;
  Bytes target;
  Signature signature;
  TargetId id;
};

Scenario make_scenario(std::size_t size, std::uint64_t chunk_size, std::uint64_t seed) {
  Scenario scenario;
  scenario.basis = test::random_bytes(size, seed);
  scenario.target = scenario.basis;
  // Edits spread over the file, so the delta mixes COPY and LITERAL records
  // in most chunks.
  test::Rng rng(seed + 1);
  for (int edit = 0; edit < 6; ++edit) {
    const std::size_t at = rng.below(scenario.target.size());
    const Bytes insert = test::random_bytes(1 + rng.below(300), seed + 10 + edit);
    scenario.target.insert(scenario.target.begin() + static_cast<std::ptrdiff_t>(at),
                           insert.begin(), insert.end());
  }
  scenario.signature = test::make_signature(scenario.basis, kBlockSize);
  scenario.id.size = scenario.target.size();
  scenario.id.digest = Sha256::hash(scenario.target.data(), scenario.target.size());
  scenario.id.chunk_size = chunk_size;
  return scenario;
}

Sha256 hash_prefix(const Bytes& data, std::uint64_t length) {
  Sha256 hash;
  hash.update(data.data(), static_cast<std::size_t>(length));
  return hash;
}

// What the sender does: encode the target from `offset`, continuing the
// whole-file hash from the prefix state.
Bytes encode_from(const Scenario& scenario, const Sha256& prefix) {
  DeltaOptions options;
  options.chunk_size = scenario.id.chunk_size;
  MemorySink delta;
  DeltaWriter writer(delta);
  DeltaEncoder encoder(scenario.signature, writer, options, prefix);
  const std::size_t offset = static_cast<std::size_t>(prefix.length());
  if (offset < scenario.target.size()) {
    encoder.feed(scenario.target.data() + offset, scenario.target.size() - offset);
  }
  encoder.finish();
  return delta.take();
}

// Stands in for a dropped connection.
struct Interrupted {};

// Stands in for the process dying (thrown past receive()).
struct Crash {};

// What the receiver does: decode `delta` into a Patcher writing into `out`,
// checkpointing after every verified chunk. The transfer is cut off after
// `byte_limit` delta bytes or once `chunk_limit` chunks were checkpointed.
// Returns true if the target was completed and verified.
bool receive(const Scenario& scenario, PartialOutput& out, const Bytes& delta,
             std::size_t byte_limit = std::numeric_limits<std::size_t>::max(),
             int chunk_limit = -1) {
  MemoryRandomAccess basis(scenario.basis);
  Patcher patcher(basis, out, out.prefix_hash());
  const Patcher::ChunkCallback checkpoint = out.checkpoint_callback(patcher);
  int chunks = 0;
  patcher.set_chunk_callback([&](std::uint64_t verified) {
    checkpoint(verified);
    if (++chunks == chunk_limit) throw Interrupted{};
  });
  DeltaDecoder decoder(patcher);
  try {
    decoder.feed(delta.data(), std::min(byte_limit, delta.size()));
    if (byte_limit >= delta.size()) decoder.finish();
  } catch (const Interrupted&) {
    return false;
  }
  return patcher.finished();
}

// Resumes whatever partial state is on disk and finishes the transfer.
void resume_and_finish(const Scenario& scenario, const std::string& dest) {
  PartialOutput out(dest, scenario.id);
  // The sender checks the receiver's claim against its own file.
  const Sha256 sender_prefix = hash_prefix(scenario.target, out.verified_length());
  const Sha256::Midstate expected = sender_prefix.midstate();
  ASSERT_EQ(out.prefix_state().length, expected.length);
  ASSERT_EQ(out.prefix_state().words, expected.words);
  const Bytes delta = encode_from(scenario, sender_prefix);
  ASSERT_TRUE(receive(scenario, out, delta));
  out.commit();
  EXPECT_TRUE(test::same_bytes(scenario.target, test::read_file(dest)));
}

std::string partial_of(const std::string& dest) { return PartialOutput::data_path_for(dest); }
std::string journal_of(const std::string& dest) { return PartialOutput::journal_path_for(dest); }

std::uint64_t file_size(const std::string& path) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) return 0;
  return static_cast<std::uint64_t>(info.st_size);
}

TEST(PartialOutputTest, FreshTransferCommitsTheTarget) {
  test::TempDir dir;
  const Scenario scenario = make_scenario(50000, 1024, 1);
  const std::string dest = dir.file("target");
  PartialOutput out(dest, scenario.id);
  EXPECT_EQ(out.verified_length(), 0u);
  EXPECT_FALSE(out.resumed());
  EXPECT_EQ(out.restart_reason(), "");
  // Hidden names, which no pushed remote name can collide with.
  EXPECT_EQ(out.data_path(), dir.file(".target.partial"));
  EXPECT_EQ(out.journal_path(), dir.file(".target.journal"));

  std::vector<std::uint64_t> journaled;
  out.set_checkpoint_observer([&](Stage stage, std::uint64_t length) {
    if (stage == Stage::kJournalReplaced) journaled.push_back(length);
  });
  ASSERT_TRUE(receive(scenario, out, encode_from(scenario, Sha256())));
  // Every full chunk was journaled; the short final one is covered by END.
  std::vector<std::uint64_t> expected;
  for (std::uint64_t at = 1024; at <= scenario.target.size(); at += 1024) expected.push_back(at);
  EXPECT_EQ(journaled, expected);
  EXPECT_EQ(out.verified_length(), expected.back());
  EXPECT_FALSE(test::file_exists(dest));  // nothing visible before commit

  out.set_mode(0640);
  out.commit();
  EXPECT_TRUE(test::same_bytes(scenario.target, test::read_file(dest)));
  EXPECT_EQ(test::list_directory(dir.path()), (std::vector<std::string>{"target"}));
  struct stat info;
  ASSERT_EQ(::stat(dest.c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 07777, 0640u);
  EXPECT_THROW(out.commit(), std::logic_error);
  const std::uint8_t byte = 0;
  EXPECT_THROW(out.write(&byte, 1), std::logic_error);
}

TEST(PartialOutputTest, PartialStateIsOwnerOnlyAndCommitAppliesTheDefaultMode) {
  test::TempDir dir;
  const Scenario scenario = make_scenario(5000, 1024, 2);
  const std::string dest = dir.file("target");
  PartialOutput out(dest, scenario.id);
  ASSERT_FALSE(receive(scenario, out, encode_from(scenario, Sha256()),
                       std::numeric_limits<std::size_t>::max(), 2));
  struct stat info;
  ASSERT_EQ(::stat(out.data_path().c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 077, 0u);
  ASSERT_EQ(::stat(out.journal_path().c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 077, 0u);
}

TEST(PartialOutputTest, CommitWithoutAModeUsesTheDefaultPermissions) {
  test::TempDir dir;
  const Scenario scenario = make_scenario(3000, 1024, 3);
  const std::string dest = dir.file("target");
  PartialOutput out(dest, scenario.id);
  ASSERT_TRUE(receive(scenario, out, encode_from(scenario, Sha256())));
  out.commit();
  struct stat info;
  ASSERT_EQ(::stat(dest.c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 07777, new_file_mode(dir.path()));
}

// Interrupt after k verified chunks, reopen, and finish from there: the
// reopened state must sit exactly at k chunks with the right prefix hash,
// and the sender must only send the rest.
TEST(PartialOutputTest, ResumesAfterEachNumberOfVerifiedChunks) {
  const Scenario scenario = make_scenario(20000, 1024, 4);
  const Bytes full_delta = encode_from(scenario, Sha256());
  const int chunks = static_cast<int>(scenario.target.size() / 1024);
  for (int k = 1; k <= chunks; ++k) {
    SCOPED_TRACE("interrupted after " + std::to_string(k) + " chunks");
    test::TempDir dir;
    const std::string dest = dir.file("target");
    {
      PartialOutput out(dest, scenario.id);
      ASSERT_FALSE(receive(scenario, out, full_delta, std::numeric_limits<std::size_t>::max(), k));
    }
    const std::uint64_t resume_at = static_cast<std::uint64_t>(k) * 1024;
    {
      PartialOutput out(dest, scenario.id);
      EXPECT_TRUE(out.resumed());
      EXPECT_EQ(out.restart_reason(), "");
      EXPECT_EQ(out.verified_length(), resume_at);
      EXPECT_EQ(out.position(), resume_at);
      const Sha256::Midstate expected = hash_prefix(scenario.target, resume_at).midstate();
      EXPECT_EQ(out.prefix_state().length, expected.length);
      EXPECT_EQ(out.prefix_state().words, expected.words);

      const Bytes rest = encode_from(scenario, out.prefix_hash());
      EXPECT_LT(rest.size(), full_delta.size());
      ASSERT_TRUE(receive(scenario, out, rest));
      out.commit();
    }
    EXPECT_TRUE(test::same_bytes(scenario.target, test::read_file(dest)));
    EXPECT_EQ(test::list_directory(dir.path()), (std::vector<std::string>{"target"}));
  }
}

// Cut the delta stream at arbitrary bytes, including inside records and in
// the middle of a chunk: the reopened state is the last checkpoint, the
// unverified tail of the partial file is cut off, and finishing from there
// reproduces the target.
TEST(PartialOutputTest, InterruptionAtAnyByteResumesFromTheLastCheckpoint) {
  const Scenario scenario = make_scenario(30000, 2048, 5);
  const Bytes full_delta = encode_from(scenario, Sha256());
  test::Rng rng(6);
  for (int round = 0; round < 40; ++round) {
    const std::size_t cut = rng.below(full_delta.size());
    SCOPED_TRACE("cut after " + std::to_string(cut) + " delta bytes");
    test::TempDir dir;
    const std::string dest = dir.file("target");
    std::uint64_t last_checkpoint = 0;
    {
      PartialOutput out(dest, scenario.id);
      out.set_checkpoint_observer([&](Stage stage, std::uint64_t length) {
        if (stage == Stage::kJournalReplaced) last_checkpoint = length;
      });
      ASSERT_FALSE(receive(scenario, out, full_delta, cut));
      out.flush();  // put the unverified tail on disk, as a real crash might
      EXPECT_GE(file_size(out.data_path()), last_checkpoint);
    }
    {
      PartialOutput out(dest, scenario.id);
      EXPECT_EQ(out.verified_length(), last_checkpoint);
      EXPECT_EQ(file_size(out.data_path()), last_checkpoint);
    }
    resume_and_finish(scenario, dest);
  }
}

// Resuming twice in a row, each time interrupted again, still ends with the
// exact target.
TEST(PartialOutputTest, SurvivesRepeatedInterruptions) {
  const Scenario scenario = make_scenario(40000, 1024, 7);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  std::uint64_t previous = 0;
  for (int attempt = 0; attempt < 5; ++attempt) {
    PartialOutput out(dest, scenario.id);
    EXPECT_EQ(out.verified_length(), previous);
    const Bytes delta = encode_from(scenario, out.prefix_hash());
    ASSERT_FALSE(receive(scenario, out, delta, std::numeric_limits<std::size_t>::max(), 5));
    previous = out.verified_length();
    EXPECT_EQ(previous, static_cast<std::uint64_t>(attempt + 1) * 5 * 1024);
  }
  resume_and_finish(scenario, dest);
}

// Interrupted after the last chunk but before END: everything is verified,
// and the resumed "delta" is just END.
TEST(PartialOutputTest, ResumesWhenOnlyTheEndRecordIsMissing) {
  Scenario scenario = make_scenario(8192, 1024, 8);
  scenario.target.resize(8192);  // a whole number of chunks
  scenario.id.size = scenario.target.size();
  scenario.id.digest = Sha256::hash(scenario.target.data(), scenario.target.size());
  test::TempDir dir;
  const std::string dest = dir.file("target");
  {
    PartialOutput out(dest, scenario.id);
    ASSERT_FALSE(receive(scenario, out, encode_from(scenario, Sha256()),
                         std::numeric_limits<std::size_t>::max(), 8));
  }
  PartialOutput out(dest, scenario.id);
  EXPECT_EQ(out.verified_length(), 8192u);
  const Bytes rest = encode_from(scenario, out.prefix_hash());
  ASSERT_TRUE(receive(scenario, out, rest));
  out.commit();
  EXPECT_TRUE(test::same_bytes(scenario.target, test::read_file(dest)));
}

TEST(PartialOutputTest, EmptyTargetCommits) {
  test::TempDir dir;
  Scenario scenario = make_scenario(1000, 64, 9);
  scenario.target.clear();
  scenario.id.size = 0;
  scenario.id.digest = Sha256().digest();
  const std::string dest = dir.file("empty");
  PartialOutput out(dest, scenario.id);
  ASSERT_TRUE(receive(scenario, out, encode_from(scenario, Sha256())));
  out.commit();
  EXPECT_TRUE(test::read_file(dest).empty());
}

// Leaves a journal and partial file for `scenario` verified up to `chunks`
// chunks, and returns the verified length.
std::uint64_t leave_partial_state(const Scenario& scenario, const std::string& dest, int chunks) {
  PartialOutput out(dest, scenario.id);
  const bool finished = receive(scenario, out, encode_from(scenario, Sha256()),
                                std::numeric_limits<std::size_t>::max(), chunks);
  EXPECT_FALSE(finished);
  return out.verified_length();
}

Bytes read_bytes(const std::string& path) { return test::read_file(path); }

void expect_clean_restart(const Scenario& scenario, const std::string& dest,
                          const std::string& reason_part) {
  {
    PartialOutput out(dest, scenario.id);
    EXPECT_EQ(out.verified_length(), 0u);
    EXPECT_FALSE(out.resumed());
    EXPECT_NE(out.restart_reason().find(reason_part), std::string::npos)
        << "reason: " << out.restart_reason();
    EXPECT_EQ(out.position(), 0u);
    EXPECT_EQ(file_size(out.data_path()), 0u);
    // The old journal was replaced by a claim that verifies nothing.
    EXPECT_EQ(file_size(out.journal_path()), 128u);
  }
  // The restarted transfer completes normally.
  resume_and_finish(scenario, dest);
}

TEST(PartialOutputTest, EveryCorruptedJournalByteRestartsFromZero) {
  const Scenario scenario = make_scenario(10000, 1024, 10);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  ASSERT_EQ(leave_partial_state(scenario, dest, 3), 3u * 1024);
  const Bytes journal = read_bytes(journal_of(dest));
  ASSERT_EQ(journal.size(), 128u);
  const Bytes data = read_bytes(partial_of(dest));
  for (std::size_t i = 0; i < journal.size(); ++i) {
    SCOPED_TRACE("byte " + std::to_string(i));
    Bytes damaged = journal;
    damaged[i] ^= 0x10;
    test::write_file(journal_of(dest), damaged);
    test::write_file(partial_of(dest), data);
    PartialOutput out(dest, scenario.id);
    EXPECT_EQ(out.verified_length(), 0u);
    EXPECT_NE(out.restart_reason(), "");
  }
  // One full restart-and-finish from the last damaged state.
  test::write_file(journal_of(dest), Bytes(journal.begin(), journal.begin() + 50));
  expect_clean_restart(scenario, dest, "wrong size");
}

TEST(PartialOutputTest, OversizedOrForeignJournalRestartsFromZero) {
  const Scenario scenario = make_scenario(10000, 1024, 11);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  leave_partial_state(scenario, dest, 3);
  Bytes journal = read_bytes(journal_of(dest));
  journal.push_back(0);
  test::write_file(journal_of(dest), journal);
  expect_clean_restart(scenario, dest, "wrong size");

  leave_partial_state(scenario, dest + "2", 3);
  test::write_file(journal_of(dest + "2"), test::bytes_of(std::string(128, 'x')));
  expect_clean_restart(scenario, dest + "2", "magic");
}

TEST(PartialOutputTest, UnknownJournalVersionRestartsFromZero) {
  const Scenario scenario = make_scenario(10000, 1024, 12);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  leave_partial_state(scenario, dest, 3);
  Bytes journal = read_bytes(journal_of(dest));
  journal[4] = 2;
  // A well-formed journal of another version: fix up its checksum.
  const Sha256Digest checksum = Sha256::hash(journal.data(), 96);
  std::copy(checksum.begin(), checksum.end(), journal.begin() + 96);
  test::write_file(journal_of(dest), journal);
  expect_clean_restart(scenario, dest, "version 2");
}

TEST(PartialOutputTest, JournalForAnotherTargetRestartsFromZero) {
  const Scenario scenario = make_scenario(10000, 1024, 13);
  test::TempDir dir;
  int index = 0;
  for (const auto& change : std::vector<void (*)(TargetId&)>{
           [](TargetId& id) { id.digest[7] ^= 1; },
           [](TargetId& id) { id.size += 1; },
           [](TargetId& id) { id.chunk_size *= 2; },
       }) {
    const std::string dest = dir.file("target" + std::to_string(index++));
    Scenario other = scenario;
    change(other.id);
    leave_partial_state(other, dest, 2);
    expect_clean_restart(scenario, dest, "different target");
  }
}

TEST(PartialOutputTest, TruncatedDataFileRestartsFromZero) {
  const Scenario scenario = make_scenario(10000, 1024, 14);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  ASSERT_EQ(leave_partial_state(scenario, dest, 4), 4096u);
  Bytes data = read_bytes(partial_of(dest));
  data.resize(4095);
  test::write_file(partial_of(dest), data);
  expect_clean_restart(scenario, dest, "holds 4095 bytes");
}

TEST(PartialOutputTest, MissingDataFileRestartsFromZero) {
  const Scenario scenario = make_scenario(10000, 1024, 15);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  leave_partial_state(scenario, dest, 4);
  ASSERT_EQ(::unlink(partial_of(dest).c_str()), 0);
  expect_clean_restart(scenario, dest, "holds 0 bytes");
}

// The journal can be intact while the data under it is not; resuming that
// would carry the damage into a file whose END check still passes, because
// the patcher continues from the journal's hash state.
TEST(PartialOutputTest, DamagedVerifiedDataRestartsFromZero) {
  const Scenario scenario = make_scenario(10000, 1024, 16);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  leave_partial_state(scenario, dest, 4);
  Bytes data = read_bytes(partial_of(dest));
  data[1500] ^= 0x01;
  test::write_file(partial_of(dest), data);
  expect_clean_restart(scenario, dest, "does not match the journal");
}

// A leftover temporary journal (a crash while replacing the journal) is
// ignored and cleaned up.
TEST(PartialOutputTest, LeftoverTemporaryJournalIsIgnored) {
  const Scenario scenario = make_scenario(10000, 1024, 17);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  leave_partial_state(scenario, dest, 3);
  test::write_file(journal_of(dest) + ".tmp", Bytes(17, 0xee));
  {
    PartialOutput out(dest, scenario.id);
    EXPECT_EQ(out.verified_length(), 3u * 1024);
  }
  resume_and_finish(scenario, dest);
  EXPECT_EQ(test::list_directory(dir.path()), (std::vector<std::string>{"target"}));
}

// A failure between the data sync and the journal replacement (here: the
// observer throws, as a crash would stop the process) leaves the previous
// journal in force, never one that claims the new chunk.
TEST(PartialOutputTest, FailureBeforeTheJournalIsReplacedKeepsThePreviousCheckpoint) {
  const Scenario scenario = make_scenario(10000, 1024, 18);
  for (const Stage stage : {Stage::kDataSynced, Stage::kJournalReplaced}) {
    test::TempDir dir;
    const std::string dest = dir.file("target");
    {
      PartialOutput out(dest, scenario.id);
      out.set_checkpoint_observer([&](Stage at, std::uint64_t length) {
        if (at == stage && length == 3 * 1024) throw Crash{};
      });
      EXPECT_THROW(receive(scenario, out, encode_from(scenario, Sha256())), Crash);
    }
    PartialOutput out(dest, scenario.id);
    EXPECT_EQ(out.verified_length(), stage == Stage::kDataSynced ? 2u * 1024 : 3u * 1024);
  }
}

// The same with a real process death: the child is killed with _exit()
// (no destructors, no buffered writes) at a chosen point of a checkpoint,
// and the parent reopens what it left behind.
TEST(PartialOutputTest, ProcessKilledDuringACheckpointNeverOverclaims) {
  const Scenario scenario = make_scenario(10000, 1024, 19);
  const Bytes delta = encode_from(scenario, Sha256());
  struct Case {
    Stage stage;
    std::uint64_t at;
    std::uint64_t expected;
  };
  for (const Case& kill : {Case{Stage::kDataSynced, 1024, 0}, Case{Stage::kDataSynced, 4096, 3072},
                           Case{Stage::kJournalReplaced, 4096, 4096}}) {
    SCOPED_TRACE("killed at " + std::to_string(kill.at));
    test::TempDir dir;
    const std::string dest = dir.file("target");
    EXPECT_EXIT(
        {
          PartialOutput out(dest, scenario.id);
          out.set_checkpoint_observer([&](Stage at, std::uint64_t length) {
            if (at == kill.stage && length == kill.at) ::_exit(42);
          });
          receive(scenario, out, delta);
        },
        ::testing::ExitedWithCode(42), "");
    {
      PartialOutput out(dest, scenario.id);
      EXPECT_EQ(out.verified_length(), kill.expected);
      EXPECT_EQ(out.restart_reason(), "");
      // The data the child synced beyond the journal is cut off.
      EXPECT_EQ(file_size(out.data_path()), kill.expected);
    }
    resume_and_finish(scenario, dest);
  }
}

TEST(PartialOutputTest, OnlyOneWriterPerDestination) {
  const Scenario scenario = make_scenario(1000, 64, 20);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  PartialOutput first(dest, scenario.id);
  try {
    PartialOutput second(dest, scenario.id);
    FAIL() << "a second writer was allowed";
  } catch (const Error& error) {
    EXPECT_NE(std::string(error.what()).find("another transfer"), std::string::npos);
  }
  // The failed attempt did not disturb the first one.
  ASSERT_TRUE(receive(scenario, first, encode_from(scenario, Sha256())));
  first.commit();
  EXPECT_TRUE(test::same_bytes(scenario.target, test::read_file(dest)));
  // Once the first is done the destination is free again.
  EXPECT_NO_THROW(PartialOutput third(dest, scenario.id));
}

TEST(PartialOutputTest, RejectsChunkSizesThatCannotBeResumed) {
  test::TempDir dir;
  TargetId id;
  id.size = 100;
  for (const std::uint64_t chunk_size :
       std::vector<std::uint64_t>{0, 1, 100, 1000, kMaxChunkSize + 64}) {
    id.chunk_size = chunk_size;
    EXPECT_THROW(PartialOutput(dir.file("target"), id), std::invalid_argument) << chunk_size;
  }
  EXPECT_EQ(test::list_directory(dir.path()), std::vector<std::string>{});
}

TEST(PartialOutputTest, CheckpointsMustMatchWhatWasWritten) {
  const Scenario scenario = make_scenario(5000, 1024, 21);
  test::TempDir dir;
  PartialOutput out(dir.file("target"), scenario.id);
  out.write(scenario.target.data(), 2048);
  // Past the bytes written.
  EXPECT_THROW(out.checkpoint(3072, hash_prefix(scenario.target, 3072)), std::logic_error);
  // Hash state of a different length.
  EXPECT_THROW(out.checkpoint(2048, hash_prefix(scenario.target, 1024)), std::logic_error);
  out.checkpoint(1024, hash_prefix(scenario.target, 1024));
  EXPECT_EQ(out.verified_length(), 1024u);
  // Not a chunk boundary: accepted but not journaled.
  out.checkpoint(1088, hash_prefix(scenario.target, 1088));
  EXPECT_EQ(out.verified_length(), 1024u);
  // Commit needs the whole target.
  EXPECT_THROW(out.commit(), std::logic_error);
}

TEST(PartialOutputTest, AbandonRemovesThePartialState) {
  const Scenario scenario = make_scenario(10000, 1024, 22);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  test::write_file(dest, test::bytes_of("old version"));
  leave_partial_state(scenario, dest, 3);
  {
    PartialOutput out(dest, scenario.id);
    EXPECT_EQ(out.verified_length(), 3u * 1024);
    out.abandon();
    EXPECT_THROW(out.checkpoint(0, Sha256()), std::logic_error);
  }
  EXPECT_EQ(test::list_directory(dir.path()), (std::vector<std::string>{"target"}));
  EXPECT_EQ(test::read_file(dest), test::bytes_of("old version"));
}

// Fresh state is claimed by a journal at once, so a later attempt (or
// abandon()) may reuse or remove the data file even if nothing was verified.
TEST(PartialOutputTest, FreshStateIsClaimedByAJournalAtOnce) {
  const Scenario scenario = make_scenario(10000, 1024, 25);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  {
    PartialOutput out(dest, scenario.id);
    EXPECT_EQ(file_size(out.journal_path()), 128u);
  }
  {
    PartialOutput out(dest, scenario.id);
    EXPECT_EQ(out.verified_length(), 0u);
    EXPECT_EQ(out.restart_reason(), "");
    out.abandon();
  }
  EXPECT_TRUE(test::list_directory(dir.path()).empty());
}

// A data file that no journal claims may be someone else's: it is neither
// truncated nor removed, and the transfer cannot start.
TEST(PartialOutputTest, UnclaimedDataFileIsLeftAlone) {
  const Scenario scenario = make_scenario(10000, 1024, 26);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  const Bytes precious = test::bytes_of("not written by deltasync");
  test::write_file(partial_of(dest), precious);
  EXPECT_THROW(PartialOutput(dest, scenario.id), UnclaimedPartialFile);
  EXPECT_TRUE(test::same_bytes(precious, test::read_file(partial_of(dest))));
  EXPECT_EQ(test::list_directory(dir.path()), (std::vector<std::string>{".target.partial"}));
}

// Visible files that merely look like resume state are not touched.
TEST(PartialOutputTest, VisibleLookalikeFilesAreNotTouched) {
  const Scenario scenario = make_scenario(10000, 1024, 27);
  test::TempDir dir;
  const std::string dest = dir.file("notes");
  test::write_file(dest + ".partial", test::bytes_of("user data A"));
  test::write_file(dest + ".journal", test::bytes_of("user data B"));
  leave_partial_state(scenario, dest, 2);
  resume_and_finish(scenario, dest);
  EXPECT_EQ(test::read_file(dest + ".partial"), test::bytes_of("user data A"));
  EXPECT_EQ(test::read_file(dest + ".journal"), test::bytes_of("user data B"));
  EXPECT_EQ(test::list_directory(dir.path()),
            (std::vector<std::string>{"notes", "notes.journal", "notes.partial"}));
}

// Rehashing a long verified prefix reports its progress, so a receiver can
// keep its peer informed.
TEST(PartialOutputTest, ReportsProgressWhileCheckingTheVerifiedPrefix) {
  const Scenario scenario = make_scenario(3 * kIoBufferSize + 5000, 64 * 1024, 28);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  const std::uint64_t verified = leave_partial_state(scenario, dest, 12);
  ASSERT_EQ(verified, 12u * 64 * 1024);
  std::vector<std::uint64_t> checked;
  PartialOutput out(dest, scenario.id, [&](std::uint64_t bytes) { checked.push_back(bytes); });
  EXPECT_EQ(out.verified_length(), verified);
  ASSERT_GE(checked.size(), 3u);
  EXPECT_TRUE(std::is_sorted(checked.begin(), checked.end()));
  EXPECT_EQ(checked.back(), verified);
}

// A failed attempt can be retried on the same object after rewind().
TEST(PartialOutputTest, RewindReturnsToTheLastCheckpoint) {
  const Scenario scenario = make_scenario(10000, 1024, 24);
  const Bytes full_delta = encode_from(scenario, Sha256());
  test::TempDir dir;
  const std::string dest = dir.file("target");
  PartialOutput out(dest, scenario.id);
  // Cut in the middle of the data: some unverified bytes were written.
  ASSERT_FALSE(receive(scenario, out, full_delta, full_delta.size() / 2));
  ASSERT_GT(out.position(), out.verified_length());
  const std::uint64_t verified = out.verified_length();
  ASSERT_GT(verified, 0u);
  out.rewind();
  EXPECT_EQ(out.position(), verified);
  EXPECT_EQ(file_size(out.data_path()), verified);
  ASSERT_TRUE(receive(scenario, out, encode_from(scenario, out.prefix_hash())));
  out.commit();
  EXPECT_TRUE(test::same_bytes(scenario.target, test::read_file(dest)));
}

// The destination keeps its old contents until commit() replaces it.
TEST(PartialOutputTest, CommitReplacesAnExistingFile) {
  const Scenario scenario = make_scenario(10000, 1024, 23);
  test::TempDir dir;
  const std::string dest = dir.file("target");
  test::write_file(dest, scenario.basis);
  PartialOutput out(dest, scenario.id);
  ASSERT_FALSE(receive(scenario, out, encode_from(scenario, Sha256()),
                       std::numeric_limits<std::size_t>::max(), 2));
  EXPECT_TRUE(test::same_bytes(scenario.basis, test::read_file(dest)));
  const Bytes rest = encode_from(scenario, out.prefix_hash());
  ASSERT_TRUE(receive(scenario, out, rest));
  out.commit();
  EXPECT_TRUE(test::same_bytes(scenario.target, test::read_file(dest)));
}

}  // namespace
}  // namespace deltasync
