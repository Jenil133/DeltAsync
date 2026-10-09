#include "cli.h"

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "deltasync/delta_encoder.h"
#include "deltasync/delta_format.h"
#include "deltasync/file.h"
#include "deltasync/patch.h"
#include "deltasync/signature.h"
#include "deltasync/sync.h"

namespace deltasync::cli {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

constexpr char kUsage[] = R"(usage: deltasync <command> [options] <files>

Commands:
  signature BASIS SIGNATURE      compute the block signature of BASIS (the old version)
  delta SIGNATURE TARGET DELTA   encode TARGET (the new version) against SIGNATURE
  patch BASIS DELTA OUTPUT       rebuild the target from BASIS and DELTA, verify it,
                                 and atomically replace OUTPUT (which may be BASIS)
  serve --port P --root DIR      receive files pushed over TCP into DIR
  push FILE --host H --port P    send FILE to a server; only the regions the
                                 server's copy lacks cross the network

Options:
  -b, --block-size SIZE   signature: block size; the default is the square root
                          of the basis size rounded up to 1K, within 1K..128K
  -S, --strong-len N      signature: SHA-256 bytes kept per block, 8..32 (default 16)
  -c, --chunk-size SIZE   delta, push: output bytes covered by each verification
                          checksum, a multiple of 64 (default 1M)
  -s, --stats             print statistics to standard error

serve options:
  --port P                TCP port to listen on (0 picks a free one)
  --root DIR              directory holding the received files
  --bind ADDR             address to listen on (default 0.0.0.0)
  --once                  handle one connection, then exit with its status
  --timeout-ms N          give up on a peer silent for N ms (default 30000, 0 = never)

push options:
  --host H, --port P      the server to send to
  --name NAME             file name on the server (default: FILE's own name)
  --whole-file            send every byte (baseline) instead of a delta
  -b, --block-size SIZE   ask the server for this signature block size
  --retries N             reconnect up to N times after a network failure; with
                          N >= 1, "busy" answers are retried for up to
                          --timeout-ms without counting against N
  --stats-json PATH       write transfer statistics as JSON to PATH
  --timeout-ms N          as for serve
  -h, --help              show this help and exit
  -V, --version           show the version and exit

SIZE is a byte count with an optional K, M or G suffix (powers of 1024).
SIGNATURE and DELTA may be "-" for standard input or standard output.
)";

class UsageError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct Options {
  std::string command;
  std::vector<std::string> files;
  std::optional<std::uint64_t> block_size;
  std::optional<std::uint64_t> strong_length;
  std::optional<std::uint64_t> chunk_size;
  std::optional<std::uint64_t> port;
  std::optional<std::string> root;
  std::optional<std::string> bind;
  std::optional<std::string> host;
  std::optional<std::string> name;
  std::optional<std::string> stats_json;
  std::optional<std::uint64_t> retries;
  std::optional<std::uint64_t> timeout_ms;
  bool once = false;
  bool whole_file = false;
  bool stats = false;
  bool help = false;
  bool version = false;
};

std::uint64_t parse_size_option(const std::string& name, const std::string& value,
                                std::uint64_t min, std::uint64_t max) {
  const std::optional<std::uint64_t> size = parse_size(value);
  if (!size) throw UsageError("invalid value '" + value + "' for " + name);
  if (*size < min || *size > max) {
    throw UsageError(name + " must be between " + std::to_string(min) + " and " +
                     std::to_string(max));
  }
  return *size;
}

// A plain decimal number (no size suffix) within [min, max].
std::uint64_t parse_number_option(const std::string& name, const std::string& value,
                                  std::uint64_t min, std::uint64_t max) {
  const bool digits_only =
      !value.empty() && value.find_first_not_of("0123456789") == std::string::npos;
  const std::optional<std::uint64_t> number = digits_only ? parse_size(value) : std::nullopt;
  if (!number) throw UsageError("invalid value '" + value + "' for " + name);
  if (*number < min || *number > max) {
    throw UsageError(name + " must be between " + std::to_string(min) + " and " +
                     std::to_string(max));
  }
  return *number;
}

Options parse_arguments(int argc, char** argv) {
  Options options;
  const std::vector<std::string> args(argv + 1, argv + argc);
  bool only_operands = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (only_operands || arg.size() < 2 || arg[0] != '-') {
      if (options.command.empty()) {
        options.command = arg;
      } else {
        options.files.push_back(arg);
      }
      continue;
    }
    if (arg == "--") {
      only_operands = true;
      continue;
    }

    std::string name = arg;
    std::optional<std::string> inline_value;
    const std::size_t equals = arg.find('=');
    if (arg.compare(0, 2, "--") == 0 && equals != std::string::npos) {
      name = arg.substr(0, equals);
      inline_value = arg.substr(equals + 1);
    }
    const auto value = [&]() -> std::string {
      if (inline_value) return *inline_value;
      if (i + 1 >= args.size()) throw UsageError("option " + name + " needs a value");
      return args[++i];
    };
    const auto no_value = [&]() {
      if (inline_value) throw UsageError("option " + name + " does not take a value");
    };

    if (name == "-h" || name == "--help") {
      no_value();
      options.help = true;
    } else if (name == "-V" || name == "--version") {
      no_value();
      options.version = true;
    } else if (name == "-s" || name == "--stats") {
      no_value();
      options.stats = true;
    } else if (name == "-b" || name == "--block-size") {
      options.block_size = parse_size_option(name, value(), kMinBlockSize, kMaxBlockSize);
    } else if (name == "-S" || name == "--strong-len") {
      options.strong_length =
          parse_size_option(name, value(), kMinStrongLength, kMaxStrongLength);
    } else if (name == "-c" || name == "--chunk-size") {
      options.chunk_size = parse_size_option(name, value(), 1, kMaxChunkSize);
      if (*options.chunk_size % Sha256::kBlockLength != 0) {
        throw UsageError(name + " must be a multiple of " +
                         std::to_string(Sha256::kBlockLength));
      }
    } else if (name == "--port") {
      options.port = parse_number_option(name, value(), 0, 65535);
    } else if (name == "--root") {
      options.root = value();
    } else if (name == "--bind") {
      options.bind = value();
    } else if (name == "--host") {
      options.host = value();
    } else if (name == "--name") {
      options.name = value();
    } else if (name == "--stats-json") {
      options.stats_json = value();
    } else if (name == "--retries") {
      options.retries = parse_number_option(name, value(), 0, 1000);
    } else if (name == "--timeout-ms") {
      options.timeout_ms = parse_number_option(name, value(), 0, 24ull * 3600 * 1000);
    } else if (name == "--once") {
      no_value();
      options.once = true;
    } else if (name == "--whole-file") {
      no_value();
      options.whole_file = true;
    } else {
      throw UsageError("unknown option " + name);
    }
  }
  return options;
}

void require_files(const Options& options, std::size_t count, const char* synopsis) {
  if (options.files.size() != count) {
    throw UsageError(std::string("usage: deltasync ") + synopsis);
  }
}

void reject_option(bool present, const char* option, const std::string& command) {
  if (present) throw UsageError(std::string(option) + " does not apply to '" + command + "'");
}

// Rejects the options that only serve and push understand.
void reject_network_options(const Options& options) {
  reject_option(options.port.has_value(), "--port", options.command);
  reject_option(options.root.has_value(), "--root", options.command);
  reject_option(options.bind.has_value(), "--bind", options.command);
  reject_option(options.host.has_value(), "--host", options.command);
  reject_option(options.name.has_value(), "--name", options.command);
  reject_option(options.stats_json.has_value(), "--stats-json", options.command);
  reject_option(options.retries.has_value(), "--retries", options.command);
  reject_option(options.timeout_ms.has_value(), "--timeout-ms", options.command);
  reject_option(options.once, "--once", options.command);
  reject_option(options.whole_file, "--whole-file", options.command);
}

// Where a serialized stream goes: standard output for "-", otherwise a file
// that only appears (or replaces the old one) once the stream is complete.
class Output {
 public:
  explicit Output(const std::string& path) {
    if (path == "-") {
      stdout_ = std::make_unique<FileSink>(STDOUT_FILENO, "standard output");
    } else {
      file_ = std::make_unique<AtomicFile>(path);
    }
  }

  ByteSink& sink() {
    if (file_) return *file_;
    return *stdout_;
  }

  void commit() {
    if (file_) {
      file_->commit();
    } else {
      stdout_->flush();
    }
  }

 private:
  std::unique_ptr<AtomicFile> file_;
  std::unique_ptr<FileSink> stdout_;
};

std::unique_ptr<FileSource> open_input(const std::string& path) {
  if (path == "-") return std::make_unique<FileSource>(STDIN_FILENO, "standard input");
  return std::make_unique<FileSource>(path);
}

class Stopwatch {
 public:
  double seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// Statistics go to standard error as one line of key=value pairs, which is
// easy to read and easy for scripts to pick apart.
class StatsLine {
 public:
  explicit StatsLine(const char* command) { line_ << command << ':'; }

  StatsLine& add(const char* key, std::uint64_t value) {
    line_ << ' ' << key << '=' << value;
    return *this;
  }

  StatsLine& add_text(const char* key, const char* value) {
    line_ << ' ' << key << '=' << value;
    return *this;
  }

  void print(const Stopwatch& stopwatch) {
    line_ << " seconds=" << std::fixed << std::setprecision(3) << stopwatch.seconds();
    std::cerr << line_.str() << '\n';
  }

  void print_milliseconds(std::uint64_t elapsed_ms) {
    line_ << " elapsed_ms=" << elapsed_ms;
    std::cerr << line_.str() << '\n';
  }

 private:
  std::ostringstream line_;
};

int run_signature(const Options& options) {
  require_files(options, 2, "signature [options] BASIS SIGNATURE");
  reject_network_options(options);
  reject_option(options.chunk_size.has_value(), "--chunk-size", options.command);
  const Stopwatch stopwatch;

  FileSource basis(options.files[0]);
  SignatureHeader header;
  header.basis_size = basis.size();
  header.block_size = options.block_size ? static_cast<std::uint32_t>(*options.block_size)
                                         : default_block_size(header.basis_size);
  if (options.strong_length) header.strong_length = static_cast<std::uint32_t>(*options.strong_length);
  validate(header);

  Output output(options.files[1]);
  SignatureWriter writer(output.sink());
  generate_signature(basis, header, writer);
  output.commit();

  if (options.stats) {
    StatsLine("signature")
        .add("basis_bytes", header.basis_size)
        .add("block_size", header.block_size)
        .add("blocks", header.block_count())
        .add("signature_bytes", writer.bytes_written())
        .print(stopwatch);
  }
  return kExitOk;
}

int run_delta(const Options& options) {
  require_files(options, 3, "delta [options] SIGNATURE TARGET DELTA");
  reject_network_options(options);
  reject_option(options.block_size.has_value(), "--block-size", options.command);
  reject_option(options.strong_length.has_value(), "--strong-len", options.command);
  const Stopwatch stopwatch;

  const Signature signature = read_signature(*open_input(options.files[0]));
  FileSource target(options.files[1]);
  DeltaOptions delta_options;
  if (options.chunk_size) delta_options.chunk_size = *options.chunk_size;

  Output output(options.files[2]);
  DeltaWriter writer(output.sink());
  const DeltaStats stats = encode_delta(signature, target, writer, delta_options);
  output.commit();

  if (options.stats) {
    StatsLine("delta")
        .add("target_bytes", stats.target_bytes)
        .add("matched_bytes", stats.matched_bytes)
        .add("literal_bytes", stats.literal_bytes)
        .add("delta_bytes", writer.bytes_written())
        .add("copy_ops", stats.copy_ops)
        .add("literal_ops", stats.literal_ops)
        .add("chunks", stats.chunks)
        .add("block_size", signature.block_size())
        .add("weak_hits", stats.weak_hits)
        .add("false_matches", stats.false_matches)
        .print(stopwatch);
  }
  return kExitOk;
}

int run_patch(const Options& options) {
  require_files(options, 3, "patch [options] BASIS DELTA OUTPUT");
  reject_network_options(options);
  reject_option(options.block_size.has_value(), "--block-size", options.command);
  reject_option(options.strong_length.has_value(), "--strong-len", options.command);
  reject_option(options.chunk_size.has_value(), "--chunk-size", options.command);
  if (options.files[2] == "-") throw UsageError("patch OUTPUT must be a file");
  const Stopwatch stopwatch;

  const PatchStats stats = patch_file(options.files[0], *open_input(options.files[1]),
                                      options.files[2]);

  if (options.stats) {
    StatsLine("patch")
        .add("output_bytes", stats.output_bytes)
        .add("copied_bytes", stats.copied_bytes)
        .add("literal_bytes", stats.literal_bytes)
        .add("chunks_verified", stats.chunks_verified)
        .print(stopwatch);
  }
  return kExitOk;
}

std::string describe_session(const SessionReport& report) {
  std::ostringstream line;
  line << "serve: peer=" << report.peer << " name=" << (report.name.empty() ? "-" : report.name);
  if (report.ok) {
    line << " ok mode=" << transfer_mode_name(report.mode) << " file_size=" << report.file_size
         << " had_basis=" << (report.had_basis ? 1 : 0)
         << " bytes_received=" << report.bytes_received << " bytes_sent=" << report.bytes_sent
         << " signature_bytes=" << report.signature_bytes
         << " delta_bytes=" << report.delta_bytes << " resumed_from=" << report.resumed_from;
  } else {
    line << " failed: " << report.error;
  }
  return line.str();
}

int run_serve(const Options& options) {
  require_files(options, 0, "serve --port P --root DIR [--bind ADDR] [--once] [--timeout-ms N]");
  if (!options.port) throw UsageError("serve needs --port");
  if (!options.root) throw UsageError("serve needs --root");
  for (const auto& [present, option] :
       {std::pair<bool, const char*>{options.block_size.has_value(), "--block-size"},
        {options.strong_length.has_value(), "--strong-len"},
        {options.chunk_size.has_value(), "--chunk-size"},
        {options.host.has_value(), "--host"},
        {options.name.has_value(), "--name"},
        {options.stats_json.has_value(), "--stats-json"},
        {options.retries.has_value(), "--retries"},
        {options.whole_file, "--whole-file"},
        {options.stats, "--stats"}}) {
    reject_option(present, option, options.command);
  }

  // Termination signals are handled by a dedicated thread through
  // sigwait(), which may then call stop() like any other thread. They are
  // blocked before the server starts threads, so every thread inherits it.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  if (!options.once) pthread_sigmask(SIG_BLOCK, &signals, nullptr);

  ServerOptions server_options;
  server_options.root = *options.root;
  if (options.bind) server_options.bind_address = *options.bind;
  server_options.port = static_cast<std::uint16_t>(*options.port);
  if (options.timeout_ms) server_options.timeout_ms = static_cast<int>(*options.timeout_ms);
  std::mutex log_mutex;
  server_options.on_session = [&log_mutex](const SessionReport& report) {
    const std::string line = describe_session(report);
    const std::lock_guard<std::mutex> guard(log_mutex);
    std::cerr << "deltasync " << line << std::endl;
  };
  SyncServer server(std::move(server_options));
  // Scripts that ask for port 0 read the real port from this line.
  std::cout << "listening on " << server.address() << std::endl;

  if (options.once) return server.serve_one().ok ? kExitOk : kExitFailure;

  std::thread signal_waiter([&server, &signals] {
    int signal_number = 0;
    sigwait(&signals, &signal_number);
    server.stop();
  });
  try {
    server.run();
  } catch (...) {
    pthread_kill(signal_waiter.native_handle(), SIGTERM);
    signal_waiter.join();
    throw;
  }
  // run() only returns after stop(), so the waiter is done or about to be.
  signal_waiter.join();
  return kExitOk;
}

int run_push(const Options& options) {
  require_files(options, 1,
                "push FILE --host H --port P [--name NAME] [--whole-file] [options]");
  if (!options.host) throw UsageError("push needs --host");
  if (!options.port || *options.port == 0) throw UsageError("push needs --port (1..65535)");
  reject_option(options.strong_length.has_value(), "--strong-len", options.command);
  reject_option(options.root.has_value(), "--root", options.command);
  reject_option(options.bind.has_value(), "--bind", options.command);
  reject_option(options.once, "--once", options.command);
  if (options.whole_file && options.block_size) {
    throw UsageError("--block-size does not apply to --whole-file");
  }

  PushOptions push;
  push.local_path = options.files[0];
  push.host = *options.host;
  push.port = static_cast<std::uint16_t>(*options.port);
  if (options.name) {
    try {
      validate_remote_name(*options.name);
    } catch (const ProtocolError& error) {
      throw UsageError(std::string("--name: ") + error.what());
    }
    push.remote_name = *options.name;
  }
  push.mode = options.whole_file ? TransferMode::kWholeFile : TransferMode::kDelta;
  if (options.block_size) push.block_size = static_cast<std::uint32_t>(*options.block_size);
  if (options.chunk_size) push.chunk_size = *options.chunk_size;
  if (options.retries) push.retries = static_cast<unsigned>(*options.retries);
  if (options.timeout_ms) push.timeout_ms = static_cast<int>(*options.timeout_ms);

  const PushStats stats = push_file(push);

  if (options.stats_json) {
    const std::string json = stats_json(stats);
    AtomicFile file(*options.stats_json);
    file.write(reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
    file.commit();
  }
  if (options.stats) {
    StatsLine line("push");
    line.add_text("mode", transfer_mode_name(stats.mode))
        .add("file_size", stats.file_size)
        .add("bytes_sent", stats.bytes_sent)
        .add("bytes_received", stats.bytes_received)
        .add("signature_bytes", stats.signature_bytes)
        .add("delta_bytes", stats.delta_bytes)
        .add("literal_bytes", stats.literal_bytes)
        .add("matched_bytes", stats.matched_bytes)
        .add("resumed_from", stats.resumed_from)
        .add("attempts", stats.attempts);
    line.print_milliseconds(stats.elapsed_ms);
  }
  return kExitOk;
}

}  // namespace

std::optional<std::uint64_t> parse_size(const std::string& text) {
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  std::size_t digits = 0;
  std::uint64_t value = 0;
  while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
    const auto digit = static_cast<std::uint64_t>(text[digits] - '0');
    if (value > (kMax - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
    ++digits;
  }
  if (digits == 0) return std::nullopt;

  std::uint64_t multiplier = 1;
  const std::string suffix = text.substr(digits);
  if (suffix == "K" || suffix == "k") {
    multiplier = std::uint64_t{1} << 10;
  } else if (suffix == "M" || suffix == "m") {
    multiplier = std::uint64_t{1} << 20;
  } else if (suffix == "G" || suffix == "g") {
    multiplier = std::uint64_t{1} << 30;
  } else if (!suffix.empty()) {
    return std::nullopt;
  }
  if (value > kMax / multiplier) return std::nullopt;
  return value * multiplier;
}

int run(int argc, char** argv) {
  try {
    const Options options = parse_arguments(argc, argv);
    if (options.help) {
      std::cout << kUsage;
      return kExitOk;
    }
    if (options.version) {
      std::cout << "deltasync " << DELTASYNC_VERSION << '\n';
      return kExitOk;
    }
    if (options.command.empty()) {
      std::cerr << kUsage;
      return kExitUsage;
    }
    if (options.command == "signature") return run_signature(options);
    if (options.command == "delta") return run_delta(options);
    if (options.command == "patch") return run_patch(options);
    if (options.command == "serve") return run_serve(options);
    if (options.command == "push") return run_push(options);
    throw UsageError("unknown command '" + options.command + "'");
  } catch (const UsageError& error) {
    std::cerr << "deltasync: " << error.what() << "\n"
              << "Try 'deltasync --help' for more information.\n";
    return kExitUsage;
  } catch (const std::exception& error) {
    std::cerr << "deltasync: " << error.what() << '\n';
    return kExitFailure;
  }
}

}  // namespace deltasync::cli
