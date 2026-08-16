#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "config.h"
#include "error.h"
#include "track_sink.h"

struct sp_port;

namespace capture_eye {

// The link to the ESP32-S3, speaking newline-delimited JSON per docs/protocol.md.
//
// capture-eye is the sole writer while it runs: nothing in the protocol arbitrates
// two writers, and interleaved bytes would corrupt whole lines. Close horusctl
// first, or run it with --fake.
class SerialPort {
public:
  [[nodiscard]] static Result<SerialPort> open(const SerialConfig& config);

  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;
  SerialPort(SerialPort&& other) noexcept;
  SerialPort& operator=(SerialPort&& other) noexcept;
  ~SerialPort();

  // Writes one complete line. The caller supplies the trailing newline.
  [[nodiscard]] Result<void> write_line(std::string_view line);

  // Non-blocking drain of whatever the device has said, split into complete
  // lines. Partial trailing input is retained until the rest arrives, so a line
  // is never reported cut in half.
  [[nodiscard]] Result<std::vector<std::string>> read_lines();

private:
  SerialPort() = default;
  void close_port();

  sp_port* port_ = nullptr;
  std::string pending_;  // bytes received but not yet terminated by a newline
};

// A track sink that writes to the device, reopening the port if it goes away
// (unplug, reset, or a reflash mid-run) so the pipeline keeps running headless.
//
// Device output is drained and echoed to stderr: `hello` reveals a reboot, and
// any `err` means we put something malformed on the wire.
[[nodiscard]] Result<TrackSink> make_serial_track_sink(const SerialConfig& config);

} // namespace capture_eye
