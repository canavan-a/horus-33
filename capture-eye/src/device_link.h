#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "error.h"

namespace capture_eye {

// The bidirectional link to the device (or a stand-in for one). A value type
// so the transport is a factory choice, not a class hierarchy: stdout for
// testing without hardware, the serial port in production.
//
// write receives one complete NDJSON line including its trailing newline.
// read is non-blocking and returns whatever complete lines have arrived since
// the last call — commonly empty, since most messages (track) get no reply.
struct DeviceLink {
  std::function<Result<void>(std::string_view line)> write;
  std::function<Result<std::vector<std::string>>()> read;
  std::string name;
};

// Prints written lines to stdout and never has anything to read. Byte-for-byte
// what the serial port would carry outbound, so the whole pipeline is
// testable with no ESP32 attached.
[[nodiscard]] DeviceLink make_stdout_device_link();

} // namespace capture_eye
