#pragma once

#include <string>
#include <utility>

namespace lsm {

// A Status carries a success value or a failure code plus a human-readable
// message. Returned by value everywhere; the success case allocates nothing.
class Status {
 public:
  enum class Code {
    kOk = 0,
    kNotFound,
    kCorruption,
    kIOError,
    kInvalidArgument,
    kNotSupported,
  };

  Status() = default;

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg = "") {
    return Status(Code::kNotFound, std::move(msg));
  }
  static Status Corruption(std::string msg) {
    return Status(Code::kCorruption, std::move(msg));
  }
  static Status IOError(std::string msg) {
    return Status(Code::kIOError, std::move(msg));
  }
  static Status InvalidArgument(std::string msg) {
    return Status(Code::kInvalidArgument, std::move(msg));
  }
  static Status NotSupported(std::string msg) {
    return Status(Code::kNotSupported, std::move(msg));
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }

  Code code() const { return code_; }
  const std::string& message() const { return msg_; }

  std::string ToString() const {
    const char* name;
    switch (code_) {
      case Code::kOk: return "OK";
      case Code::kNotFound: name = "NotFound"; break;
      case Code::kCorruption: name = "Corruption"; break;
      case Code::kIOError: name = "IOError"; break;
      case Code::kInvalidArgument: name = "InvalidArgument"; break;
      case Code::kNotSupported: name = "NotSupported"; break;
      default: name = "Unknown"; break;
    }
    std::string out(name);
    if (!msg_.empty()) {
      out += ": ";
      out += msg_;
    }
    return out;
  }

 private:
  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  Code code_ = Code::kOk;
  std::string msg_;
};

}  // namespace lsm
