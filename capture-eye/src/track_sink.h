#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "error.h"

namespace capture_eye {

// Where encoded track lines go. A value type so the transport is a factory
// choice, not a class hierarchy: stdout for testing without hardware, the
// serial port in production.
//
// write receives one complete NDJSON line including its trailing newline.
struct TrackSink {
  std::function<Result<void>(std::string_view line)> write;
  std::string name;
};

// Prints lines to stdout. Byte-for-byte what the serial port would carry, so
// the whole pipeline is testable with no ESP32 attached.
[[nodiscard]] TrackSink make_stdout_track_sink();

} // namespace capture_eye
