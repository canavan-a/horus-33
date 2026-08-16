#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "config.h"
#include "device_link.h"
#include "error.h"

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

// A device link backed by the real port, reopening it if it goes away (unplug,
// reset, or a reflash mid-run) so the pipeline keeps running headless.
//
// write and read are independent: read must be polled on its own cadence by
// the caller, not only after a write, or replies to control-relay commands
// (which may arrive when no track is being sent) would only surface by luck.
[[nodiscard]] Result<DeviceLink> make_serial_device_link(const SerialConfig& config);

} // namespace capture_eye
