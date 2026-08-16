#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace capture_eye {

enum class ErrorCode {
  config_invalid,
  camera_open_failed,
  camera_format_rejected,
  camera_stream_failed,
  decode_failed,
  model_fetch_failed,
  model_hash_mismatch,
  model_load_failed,
  model_shape_unexpected,
  inference_failed,
  serial_open_failed,
  serial_write_failed,
  sink_failed,
};

[[nodiscard]] std::string_view describe(ErrorCode code);

// A code alone is not actionable — "camera open failed" without the errno or the
// device path costs more debugging time than the string costs to carry.
struct Error {
  ErrorCode code;
  std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(ErrorCode code, std::string message) {
  return std::unexpected(Error{code, std::move(message)});
}

// Rendered as "camera open failed: /dev/video0: No such file or directory".
[[nodiscard]] std::string to_string(const Error& error);

} // namespace capture_eye
