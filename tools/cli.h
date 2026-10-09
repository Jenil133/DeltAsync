#ifndef DELTASYNC_TOOLS_CLI_H_
#define DELTASYNC_TOOLS_CLI_H_

#include <cstdint>
#include <optional>
#include <string>

namespace deltasync::cli {

// Parses a byte count such as "4096", "64K", "8M" or "1G" (multiples of
// 1024). Returns nullopt for anything else, including values that overflow.
std::optional<std::uint64_t> parse_size(const std::string& text);

// Runs the deltasync command line and returns the process exit status:
// 0 on success, 1 when the operation failed, 2 for invalid usage.
int run(int argc, char** argv);

}  // namespace deltasync::cli

#endif  // DELTASYNC_TOOLS_CLI_H_
