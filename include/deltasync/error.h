#ifndef DELTASYNC_ERROR_H_
#define DELTASYNC_ERROR_H_

#include <stdexcept>
#include <string>

namespace deltasync {

// Base class for every error the library reports.
class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Serialized input (a signature or a delta) is malformed, truncated, or uses
// a version or option this build does not understand.
class FormatError : public Error {
 public:
  using Error::Error;
};

// Data failed verification: reconstructed output does not hash to what the
// sender certified, or the basis is not the file a delta was computed against.
class IntegrityError : public Error {
 public:
  using Error::Error;
};

// Thread-safe description of an errno value (strerror is not thread-safe on
// every platform).
std::string error_message(int error_number);

// A system call failed. what() names the operation and the file involved.
class IoError : public Error {
 public:
  IoError(const std::string& operation, const std::string& path, int error_number);

  int error_number() const noexcept { return error_number_; }

 private:
  int error_number_;
};

}  // namespace deltasync

#endif  // DELTASYNC_ERROR_H_
