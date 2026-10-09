// Runs the real deltasync executable, as a user or script would.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cli.h"
#include "deltasync/resume.h"
#include "deltasync/sync.h"
#include "test_util.h"

extern char** environ;

namespace deltasync {
namespace {

using test::Bytes;

struct CommandResult {
  int exit_code = -1;
  std::string out;
  std::string err;
};

// Runs deltasync with `args`. Standard input comes from `input_path` (or
// /dev/null); standard output and error are captured through files so a
// large output cannot fill a pipe and stall the child.
CommandResult run_deltasync(const std::vector<std::string>& args,
                            const std::string& input_path = "/dev/null") {
  const test::TempDir capture;
  const std::string out_path = capture.file("stdout");
  const std::string err_path = capture.file("stderr");

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, input_path.c_str(), O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);

  std::vector<std::string> argv_storage = {DELTASYNC_CLI_PATH};
  argv_storage.insert(argv_storage.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (std::string& arg : argv_storage) argv.push_back(arg.data());
  argv.push_back(nullptr);

  CommandResult result;
  pid_t pid = 0;
  const int spawn_error =
      posix_spawn(&pid, DELTASYNC_CLI_PATH, &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_error != 0) {
    ADD_FAILURE() << "posix_spawn failed: " << spawn_error;
    return result;
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      ADD_FAILURE() << "waitpid failed";
      return result;
    }
  }
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  const Bytes out = test::read_file(out_path);
  const Bytes err = test::read_file(err_path);
  result.out.assign(out.begin(), out.end());
  result.err.assign(err.begin(), err.end());
  return result;
}

// Parses "command: key=value key=value ..." from a --stats line.
std::map<std::string, std::string> parse_stats(const std::string& text, const std::string& command) {
  std::map<std::string, std::string> values;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind(command + ":", 0) != 0) continue;
    std::istringstream fields(line.substr(command.size() + 1));
    std::string field;
    while (fields >> field) {
      const std::size_t equals = field.find('=');
      if (equals != std::string::npos) values[field.substr(0, equals)] = field.substr(equals + 1);
    }
  }
  return values;
}

class CliTest : public ::testing::Test {
 protected:
  void SetUp() override {
    basis_ = test::random_bytes(300000, 601);
    target_ = basis_;
    target_.insert(target_.begin() + 150000, 2000, 0x42);
    test::write_file(path("basis"), basis_);
    test::write_file(path("target"), target_);
  }

  std::string path(const std::string& name) const { return dir_.file(name); }

  test::TempDir dir_;
  Bytes basis_;
  Bytes target_;
};

TEST_F(CliTest, SignatureDeltaPatchRebuildsTheTarget) {
  ASSERT_EQ(run_deltasync({"signature", path("basis"), path("sig")}).exit_code, 0);
  ASSERT_EQ(run_deltasync({"delta", path("sig"), path("target"), path("delta")}).exit_code, 0);
  const CommandResult patch = run_deltasync({"patch", path("basis"), path("delta"), path("out")});
  ASSERT_EQ(patch.exit_code, 0) << patch.err;
  EXPECT_TRUE(test::same_bytes(target_, test::read_file(path("out"))));
  EXPECT_EQ(patch.out, "");
  EXPECT_EQ(patch.err, "");
  // Without --stats the tool is silent, and it leaves no temporary files.
  EXPECT_EQ(test::list_directory(dir_.path()),
            (std::vector<std::string>{"basis", "delta", "out", "sig", "target"}));
  EXPECT_LT(test::read_file(path("delta")).size(), 2000u + 2 * 1024 + 200);
}

TEST_F(CliTest, StatsReportWhatWasTransferred) {
  const CommandResult signature =
      run_deltasync({"signature", "--stats", path("basis"), path("sig")});
  ASSERT_EQ(signature.exit_code, 0) << signature.err;
  auto stats = parse_stats(signature.err, "signature");
  EXPECT_EQ(stats["basis_bytes"], std::to_string(basis_.size()));
  EXPECT_EQ(stats["block_size"], std::to_string(default_block_size(basis_.size())));
  EXPECT_EQ(stats["signature_bytes"], std::to_string(test::read_file(path("sig")).size()));
  EXPECT_EQ(stats.count("seconds"), 1u);

  const CommandResult delta =
      run_deltasync({"delta", "-s", path("sig"), path("target"), path("delta")});
  ASSERT_EQ(delta.exit_code, 0) << delta.err;
  stats = parse_stats(delta.err, "delta");
  const std::uint64_t matched = std::stoull(stats["matched_bytes"]);
  const std::uint64_t literal = std::stoull(stats["literal_bytes"]);
  EXPECT_EQ(stats["target_bytes"], std::to_string(target_.size()));
  EXPECT_EQ(matched + literal, target_.size());
  EXPECT_LE(literal, 2000u + 1024);
  EXPECT_EQ(stats["delta_bytes"], std::to_string(test::read_file(path("delta")).size()));
  for (const char* key : {"copy_ops", "literal_ops", "chunks", "weak_hits", "false_matches"}) {
    EXPECT_EQ(stats.count(key), 1u) << key;
  }

  const CommandResult patch =
      run_deltasync({"patch", "--stats", path("basis"), path("delta"), path("out")});
  ASSERT_EQ(patch.exit_code, 0) << patch.err;
  stats = parse_stats(patch.err, "patch");
  EXPECT_EQ(stats["output_bytes"], std::to_string(target_.size()));
  EXPECT_EQ(stats["literal_bytes"], std::to_string(literal));
  EXPECT_EQ(stats["chunks_verified"], "1");
}

TEST_F(CliTest, StreamsThroughStandardInputAndOutput) {
  const CommandResult signature = run_deltasync({"signature", path("basis"), "-"});
  ASSERT_EQ(signature.exit_code, 0) << signature.err;
  test::write_file(path("sig"), Bytes(signature.out.begin(), signature.out.end()));

  const CommandResult delta = run_deltasync({"delta", "-", path("target"), "-"}, path("sig"));
  ASSERT_EQ(delta.exit_code, 0) << delta.err;
  test::write_file(path("delta"), Bytes(delta.out.begin(), delta.out.end()));

  const CommandResult patch = run_deltasync({"patch", path("basis"), "-", path("out")}, path("delta"));
  ASSERT_EQ(patch.exit_code, 0) << patch.err;
  EXPECT_TRUE(test::same_bytes(target_, test::read_file(path("out"))));
}

TEST_F(CliTest, PatchesInPlace) {
  ASSERT_EQ(run_deltasync({"signature", path("basis"), path("sig")}).exit_code, 0);
  ASSERT_EQ(run_deltasync({"delta", path("sig"), path("target"), path("delta")}).exit_code, 0);
  ASSERT_EQ(run_deltasync({"patch", path("basis"), path("delta"), path("basis")}).exit_code, 0);
  EXPECT_TRUE(test::same_bytes(target_, test::read_file(path("basis"))));
}

TEST_F(CliTest, HonoursBlockAndChunkSizeOptions) {
  const CommandResult signature =
      run_deltasync({"signature", "--block-size", "4K", "--strong-len=24", "--stats",
                     path("basis"), path("sig")});
  ASSERT_EQ(signature.exit_code, 0) << signature.err;
  auto stats = parse_stats(signature.err, "signature");
  EXPECT_EQ(stats["block_size"], "4096");
  // 74 blocks (73 full, one partial) of 4 + 24 bytes, plus the header.
  EXPECT_EQ(stats["blocks"], "74");
  EXPECT_GT(std::stoull(stats["signature_bytes"]), 74u * 28);

  const CommandResult delta = run_deltasync(
      {"delta", "--chunk-size=64K", "--stats", path("sig"), path("target"), path("delta")});
  ASSERT_EQ(delta.exit_code, 0) << delta.err;
  stats = parse_stats(delta.err, "delta");
  EXPECT_EQ(stats["block_size"], "4096");
  EXPECT_EQ(stats["chunks"], std::to_string((target_.size() + 65535) / 65536));
  ASSERT_EQ(run_deltasync({"patch", path("basis"), path("delta"), path("out")}).exit_code, 0);
  EXPECT_TRUE(test::same_bytes(target_, test::read_file(path("out"))));
}

TEST_F(CliTest, EmptyFilesWork) {
  test::write_file(path("empty"), Bytes());
  ASSERT_EQ(run_deltasync({"signature", path("empty"), path("sig")}).exit_code, 0);
  ASSERT_EQ(run_deltasync({"delta", path("sig"), path("empty"), path("delta")}).exit_code, 0);
  ASSERT_EQ(run_deltasync({"patch", path("empty"), path("delta"), path("out")}).exit_code, 0);
  EXPECT_TRUE(test::read_file(path("out")).empty());

  ASSERT_EQ(run_deltasync({"delta", path("sig"), path("target"), path("delta2")}).exit_code, 0);
  ASSERT_EQ(run_deltasync({"patch", path("empty"), path("delta2"), path("out2")}).exit_code, 0);
  EXPECT_TRUE(test::same_bytes(target_, test::read_file(path("out2"))));
}

TEST_F(CliTest, CorruptDeltaFailsWithoutTouchingTheOutput) {
  ASSERT_EQ(run_deltasync({"signature", path("basis"), path("sig")}).exit_code, 0);
  ASSERT_EQ(run_deltasync({"delta", path("sig"), path("target"), path("delta")}).exit_code, 0);
  Bytes delta = test::read_file(path("delta"));
  delta[delta.size() / 2] ^= 0x10;
  test::write_file(path("bad-delta"), delta);
  test::write_file(path("out"), test::bytes_of("keep me"));

  const CommandResult result = run_deltasync({"patch", path("basis"), path("bad-delta"), path("out")});
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.err.rfind("deltasync: ", 0), 0u) << result.err;
  EXPECT_EQ(test::read_file(path("out")), test::bytes_of("keep me"));
  EXPECT_EQ(test::list_directory(dir_.path()),
            (std::vector<std::string>{"bad-delta", "basis", "delta", "out", "sig", "target"}));
}

TEST_F(CliTest, WrongBasisIsDetected) {
  ASSERT_EQ(run_deltasync({"signature", path("basis"), path("sig")}).exit_code, 0);
  ASSERT_EQ(run_deltasync({"delta", path("sig"), path("target"), path("delta")}).exit_code, 0);
  Bytes other = basis_;
  other[1000] ^= 1;
  test::write_file(path("other"), other);
  const CommandResult result = run_deltasync({"patch", path("other"), path("delta"), path("out")});
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.err.find("checksum"), std::string::npos) << result.err;
  EXPECT_FALSE(test::file_exists(path("out")));
}

TEST_F(CliTest, MissingInputNamesTheFile) {
  const CommandResult result = run_deltasync({"signature", path("nope"), path("sig")});
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.err.find(path("nope")), std::string::npos) << result.err;
  EXPECT_FALSE(test::file_exists(path("sig")));
}

TEST_F(CliTest, GarbageSignatureIsRejected) {
  test::write_file(path("sig"), test::bytes_of("this is not a signature"));
  const CommandResult result = run_deltasync({"delta", path("sig"), path("target"), path("delta")});
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.err.find("magic"), std::string::npos) << result.err;
  EXPECT_FALSE(test::file_exists(path("delta")));
}

TEST(CliUsageTest, HelpAndVersion) {
  const CommandResult help = run_deltasync({"--help"});
  EXPECT_EQ(help.exit_code, 0);
  EXPECT_EQ(help.out.rfind("usage: deltasync", 0), 0u);
  const CommandResult version = run_deltasync({"--version"});
  EXPECT_EQ(version.exit_code, 0);
  EXPECT_EQ(version.out.rfind("deltasync ", 0), 0u);
  const CommandResult nothing = run_deltasync({});
  EXPECT_EQ(nothing.exit_code, 2);
  EXPECT_EQ(nothing.err.rfind("usage: deltasync", 0), 0u);
}

TEST(CliUsageTest, InvalidUsageExitsWithStatus2) {
  const std::vector<std::vector<std::string>> cases = {
      {"frobnicate", "a", "b"},
      {"signature", "only-one"},
      {"delta", "a", "b"},
      {"patch", "a", "b", "c", "d"},
      {"signature", "--bogus", "a", "b"},
      {"signature", "--block-size", "a", "b"},
      {"signature", "--block-size", "0", "a", "b"},
      {"signature", "--block-size", "17M", "a", "b"},
      {"signature", "--strong-len", "7", "a", "b"},
      {"signature", "--strong-len", "33", "a", "b"},
      {"signature", "--chunk-size", "1M", "a", "b"},
      {"delta", "--block-size", "1K", "a", "b", "c"},
      {"delta", "--chunk-size", "0", "a", "b", "c"},
      {"delta", "--chunk-size", "2G", "a", "b", "c"},
      {"patch", "a", "b", "-"},
      {"patch", "--stats=yes", "a", "b", "c"},
      {"signature", "a", "--block-size"},
      {"delta", "--chunk-size", "100", "a", "b", "c"},
      {"signature", "--port", "1", "a", "b"},
      {"patch", "--whole-file", "a", "b", "c"},
      {"push", "file", "--port", "1"},
      {"push", "file", "--host", "h"},
      {"push", "file", "--host", "h", "--port", "0"},
      {"push", "file", "--host", "h", "--port", "65536"},
      {"push", "file", "--host", "h", "--port", "12x"},
      {"push", "file", "--host", "h", "--port", "1", "--chunk-size", "1000"},
      {"push", "file", "--host", "h", "--port", "1", "--once"},
      {"push", "file", "--host", "h", "--port", "1", "--whole-file", "--block-size", "4K"},
      {"push", "file", "--host", "h", "--port", "1", "--name", "../x"},
      {"push", "--host", "h", "--port", "1"},
      {"serve", "--port", "0"},
      {"serve", "--root", "."},
      {"serve", "--port", "0", "--root", ".", "--name", "x"},
      {"serve", "--port", "0", "--root", ".", "extra"},
  };
  for (const auto& args : cases) {
    const CommandResult result = run_deltasync(args);
    std::string joined;
    for (const auto& arg : args) joined += arg + " ";
    EXPECT_EQ(result.exit_code, 2) << joined << "\n" << result.err;
    EXPECT_EQ(result.err.rfind("deltasync: ", 0), 0u) << joined << "\n" << result.err;
  }
}

TEST(CliParseSizeTest, AcceptsSuffixes) {
  EXPECT_EQ(cli::parse_size("0"), 0u);
  EXPECT_EQ(cli::parse_size("4096"), 4096u);
  EXPECT_EQ(cli::parse_size("64K"), 65536u);
  EXPECT_EQ(cli::parse_size("64k"), 65536u);
  EXPECT_EQ(cli::parse_size("8M"), 8u << 20);
  EXPECT_EQ(cli::parse_size("1G"), 1u << 30);
  EXPECT_EQ(cli::parse_size("18446744073709551615"), UINT64_MAX);
}

TEST(CliParseSizeTest, RejectsMalformedValues) {
  for (const char* text : {"", "K", "-1", "1.5K", "12X", "1KB", " 1", "1 ", "0x10",
                           "18446744073709551616", "17179869184G"}) {
    EXPECT_FALSE(cli::parse_size(text).has_value()) << "'" << text << "'";
  }
}

// serve and push against in-process counterparts, so no test has to guess
// when a server process is ready.
class CliSyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ServerOptions options;
    options.root = root_.path();
    options.bind_address = "127.0.0.1";
    server_ = std::make_unique<SyncServer>(options);
    thread_ = std::thread([this] { server_->run(); });
  }

  void TearDown() override {
    server_->stop();
    thread_.join();
  }

  std::string port() const { return std::to_string(server_->port()); }

  test::TempDir root_;
  test::TempDir local_;
  std::unique_ptr<SyncServer> server_;
  std::thread thread_;
};

TEST_F(CliSyncTest, PushWritesStatsJson) {
  const Bytes basis = test::random_bytes(400000, 611);
  Bytes target = basis;
  target.insert(target.begin() + 5000, 1000, 7);
  test::write_file(root_.file("data"), basis);
  test::write_file(local_.file("data"), target);

  const CommandResult result =
      run_deltasync({"push", local_.file("data"), "--host", "127.0.0.1", "--port", port(),
                     "--stats-json", local_.file("stats.json"), "--stats"});
  ASSERT_EQ(result.exit_code, 0) << result.err;
  EXPECT_TRUE(test::same_bytes(target, test::read_file(root_.file("data"))));
  const Bytes json_bytes = test::read_file(local_.file("stats.json"));
  const std::string json(json_bytes.begin(), json_bytes.end());
  for (const char* key : {"mode", "file_size", "bytes_sent", "bytes_received", "signature_bytes",
                          "delta_bytes", "literal_bytes", "matched_bytes", "resumed_from",
                          "attempts", "elapsed_ms"}) {
    EXPECT_NE(json.find(std::string("\"") + key + "\": "), std::string::npos) << key;
  }
  EXPECT_NE(json.find("\"mode\": \"delta\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"file_size\": " + std::to_string(target.size())), std::string::npos);
  EXPECT_NE(json.find("\"attempts\": 1"), std::string::npos);

  auto stats = parse_stats(result.err, "push");
  EXPECT_EQ(stats["mode"], "delta");
  EXPECT_EQ(stats["file_size"], std::to_string(target.size()));
  EXPECT_LT(std::stoull(stats["bytes_sent"]), target.size() / 10);
  EXPECT_NE(json.find("\"bytes_sent\": " + stats["bytes_sent"]), std::string::npos);
}

TEST_F(CliSyncTest, PushWholeFileWithAnotherName) {
  const Bytes data = test::random_bytes(100000, 612);
  test::write_file(local_.file("local"), data);
  const CommandResult result =
      run_deltasync({"push", local_.file("local"), "--host", "127.0.0.1", "--port", port(),
                     "--name", "remote", "--whole-file", "--stats-json", local_.file("s.json")});
  ASSERT_EQ(result.exit_code, 0) << result.err;
  EXPECT_TRUE(test::same_bytes(data, test::read_file(root_.file("remote"))));
  const Bytes json = test::read_file(local_.file("s.json"));
  EXPECT_NE(std::string(json.begin(), json.end()).find("\"mode\": \"whole-file\""),
            std::string::npos);
}

// A push continues from partial state an interrupted push left behind and
// says so in its statistics.
TEST_F(CliSyncTest, PushResumesFromVerifiedPartialState) {
  const std::uint64_t chunk = 64 * 1024;
  const Bytes data = test::random_bytes(5 * chunk + 1000, 613);
  {
    TargetId target;
    target.size = data.size();
    target.digest = Sha256::hash(data.data(), data.size());
    target.chunk_size = chunk;
    PartialOutput partial(root_.file("big"), target);
    partial.write(data.data(), 3 * chunk);
    Sha256 prefix;
    prefix.update(data.data(), 3 * chunk);
    partial.checkpoint(3 * chunk, prefix);
  }
  test::write_file(local_.file("big"), data);
  const CommandResult result =
      run_deltasync({"push", local_.file("big"), "--host", "127.0.0.1", "--port", port(),
                     "--whole-file", "--chunk-size", "64K", "--stats-json",
                     local_.file("s.json"), "--stats"});
  ASSERT_EQ(result.exit_code, 0) << result.err;
  EXPECT_TRUE(test::same_bytes(data, test::read_file(root_.file("big"))));
  EXPECT_EQ(test::list_directory(root_.path()), std::vector<std::string>{"big"});
  auto stats = parse_stats(result.err, "push");
  EXPECT_EQ(stats["resumed_from"], std::to_string(3 * chunk));
  EXPECT_EQ(stats["literal_bytes"], std::to_string(data.size() - 3 * chunk));
  const Bytes json = test::read_file(local_.file("s.json"));
  EXPECT_NE(std::string(json.begin(), json.end())
                .find("\"resumed_from\": " + std::to_string(3 * chunk)),
            std::string::npos);
}

TEST(CliServeTest, ServeOnceReceivesOneFile) {
  const test::TempDir root;
  const test::TempDir local;
  int out_pipe[2];
  ASSERT_EQ(::pipe(out_pipe), 0);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
  const std::string err_path = local.file("stderr");
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  std::vector<std::string> args = {DELTASYNC_CLI_PATH, "serve", "--port", "0", "--root",
                                   root.path(), "--bind", "127.0.0.1", "--once"};
  std::vector<char*> argv;
  for (std::string& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int spawn_error =
      posix_spawn(&pid, DELTASYNC_CLI_PATH, &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(out_pipe[1]);
  ASSERT_EQ(spawn_error, 0);

  // The server prints its address once it is listening; reading the line
  // is the synchronization.
  std::string line;
  char c = 0;
  while (::read(out_pipe[0], &c, 1) == 1 && c != '\n') line.push_back(c);
  ::close(out_pipe[0]);
  const std::string prefix = "listening on 127.0.0.1:";
  ASSERT_EQ(line.rfind(prefix, 0), 0u) << line;
  const int port = std::stoi(line.substr(prefix.size()));

  const Bytes data = test::random_bytes(200000, 613);
  test::write_file(local.file("pushed"), data);
  PushOptions options;
  options.local_path = local.file("pushed");
  options.host = "127.0.0.1";
  options.port = static_cast<std::uint16_t>(port);
  push_file(options);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_TRUE(test::same_bytes(data, test::read_file(root.file("pushed"))));
  const Bytes err = test::read_file(err_path);
  const std::string log(err.begin(), err.end());
  EXPECT_NE(log.find("serve: peer=127.0.0.1:"), std::string::npos) << log;
  EXPECT_NE(log.find("name=pushed ok"), std::string::npos) << log;
}

TEST(CliServeTest, PushWithoutAServerFails) {
  std::uint16_t port = 0;
  {
    Listener listener("127.0.0.1", 0);
    port = listener.port();
  }
  const test::TempDir local;
  test::write_file(local.file("f"), test::bytes_of("x"));
  const CommandResult result = run_deltasync(
      {"push", local.file("f"), "--host", "127.0.0.1", "--port", std::to_string(port)});
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.err.find("cannot connect"), std::string::npos) << result.err;
}

TEST(CliServeTest, ServeRejectsAMissingRoot) {
  const CommandResult result =
      run_deltasync({"serve", "--port", "0", "--root", "/nonexistent/deltasync-root"});
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.err.find("/nonexistent/deltasync-root"), std::string::npos) << result.err;
}

}  // namespace
}  // namespace deltasync
