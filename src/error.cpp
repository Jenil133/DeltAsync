#include "deltasync/error.h"

#include <cstdio>
#include <cstring>

namespace deltasync {

namespace {

// strerror_r comes in two flavours: XSI (returns int, fills the buffer) and
// GNU (returns char*, which may or may not point into the buffer). Overloading
// on the return type picks the right interpretation at compile time without
// feature-test macros. std::strerror itself is not thread-safe on glibc.
[[maybe_unused]] const char* strerror_result(int status, const char* buffer, int error_number,
                                             char* scratch, std::size_t scratch_size) {
  if (status == 0) return buffer;
  std::snprintf(scratch, scratch_size, "Unknown error %d", error_number);
  return scratch;
}

[[maybe_unused]] const char* strerror_result(const char* result, const char* /*buffer*/,
                                             int /*error_number*/, char* /*scratch*/,
                                             std::size_t /*scratch_size*/) {
  return result;
}

}  // namespace

std::string error_message(int error_number) {
  char buffer[256] = {};
  char scratch[64] = {};
  const char* text = strerror_result(strerror_r(error_number, buffer, sizeof buffer), buffer,
                                     error_number, scratch, sizeof scratch);
  if (text == nullptr || text[0] == '\0') {
    std::snprintf(scratch, sizeof scratch, "Unknown error %d", error_number);
    text = scratch;
  }
  return text;
}

IoError::IoError(const std::string& operation, const std::string& path, int error_number)
    : Error(operation + " " + path + ": " + error_message(error_number)),
      error_number_(error_number) {}

}  // namespace deltasync
