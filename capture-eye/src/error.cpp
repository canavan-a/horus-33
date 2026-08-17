#include "error.h"

namespace capture_eye {

std::string_view describe(ErrorCode code) {
  switch (code) {
    case ErrorCode::config_invalid:         return "invalid configuration";
    case ErrorCode::camera_open_failed:     return "camera open failed";
    case ErrorCode::camera_format_rejected: return "camera rejected the requested format";
    case ErrorCode::camera_stream_failed:   return "camera streaming failed";
    case ErrorCode::decode_failed:          return "frame decode failed";
    case ErrorCode::model_fetch_failed:     return "model download failed";
    case ErrorCode::model_hash_mismatch:    return "model hash mismatch";
    case ErrorCode::model_load_failed:      return "model load failed";
    case ErrorCode::model_shape_unexpected: return "model has unexpected tensor shapes";
    case ErrorCode::inference_failed:       return "inference failed";
    case ErrorCode::serial_open_failed:     return "serial open failed";
    case ErrorCode::serial_write_failed:    return "serial write failed";
    case ErrorCode::sink_failed:            return "frame sink failed";
  }
  return "unknown error";
}

int exit_code_for(ErrorCode code) {
  switch (code) {
    case ErrorCode::camera_open_failed:
    case ErrorCode::camera_format_rejected:
    case ErrorCode::camera_stream_failed:
      return 10; // no camera / camera rejected the request
    case ErrorCode::serial_open_failed:
    case ErrorCode::serial_write_failed:
      return 11; // no ESP32 / serial link failed
    case ErrorCode::config_invalid:
      return 2; // matches parse_args' existing hardcoded 2 in main.cpp — a
                // config error is a config error regardless of which code
                // path caught it first
    default:
      return 1;
  }
}

std::string to_string(const Error& error) {
  std::string out{describe(error.code)};
  if (!error.message.empty()) {
    out += ": ";
    out += error.message;
  }
  return out;
}

} // namespace capture_eye
