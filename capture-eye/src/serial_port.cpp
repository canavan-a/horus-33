#include "serial_port.h"

#include <libserialport.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <utility>

namespace capture_eye {
namespace {

using Clock = std::chrono::steady_clock;

// A wedged USB CDC endpoint must not stall the pipeline. Losing a track line is
// recoverable; blocking the stage that writes them is not.
constexpr unsigned int kWriteTimeoutMs = 50;
constexpr auto kReopenInterval = std::chrono::seconds{1};

[[nodiscard]] std::string describe_sp(sp_return code) {
  if (code == SP_ERR_FAIL) {
    // libserialport allocates this; it is ours to free.
    char* message = sp_last_error_message();
    std::string owned = message == nullptr ? "unknown error" : message;
    sp_free_error_message(message);
    return owned;
  }
  switch (code) {
    case SP_ERR_ARG:  return "invalid argument";
    case SP_ERR_MEM:  return "out of memory";
    case SP_ERR_SUPP: return "not supported";
    default:          return "unknown error";
  }
}

} // namespace

Result<SerialPort> SerialPort::open(const SerialConfig& config) {
  SerialPort serial;

  const std::string name = config.port.string();
  if (const auto code = sp_get_port_by_name(name.c_str(), &serial.port_); code != SP_OK) {
    return fail(ErrorCode::serial_open_failed,
                std::format("{}: {}", name, describe_sp(code)));
  }

  // Read as well as write: the device's replies are how we find out it rebooted
  // or that we sent something malformed.
  if (const auto code = sp_open(serial.port_, SP_MODE_READ_WRITE); code != SP_OK) {
    return fail(ErrorCode::serial_open_failed,
                std::format("{}: {}", name, describe_sp(code)));
  }

  // Baud is nominal on native USB CDC (docs/protocol.md), but the host still has
  // to pick something, and 8-N-1 with no flow control is what the device expects.
  sp_set_baudrate(serial.port_, config.baud);
  sp_set_bits(serial.port_, 8);
  sp_set_parity(serial.port_, SP_PARITY_NONE);
  sp_set_stopbits(serial.port_, 1);
  sp_set_flowcontrol(serial.port_, SP_FLOWCONTROL_NONE);

  return serial;
}

SerialPort::SerialPort(SerialPort&& other) noexcept
    : port_{std::exchange(other.port_, nullptr)}, pending_{std::move(other.pending_)} {}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
  if (this != &other) {
    close_port();
    port_ = std::exchange(other.port_, nullptr);
    pending_ = std::move(other.pending_);
  }
  return *this;
}

SerialPort::~SerialPort() { close_port(); }

void SerialPort::close_port() {
  if (port_ == nullptr) return;
  sp_close(port_);
  sp_free_port(port_);
  port_ = nullptr;
}

Result<void> SerialPort::write_line(std::string_view line) {
  if (port_ == nullptr) {
    return fail(ErrorCode::serial_write_failed, "port is not open");
  }

  const auto written =
      sp_blocking_write(port_, line.data(), line.size(), kWriteTimeoutMs);
  if (written < 0) {
    return fail(ErrorCode::serial_write_failed, describe_sp(static_cast<sp_return>(written)));
  }
  if (static_cast<std::size_t>(written) != line.size()) {
    // A partial line is worse than no line: the device would try to parse a
    // fragment and answer with an err.
    return fail(ErrorCode::serial_write_failed,
                std::format("wrote {} of {} bytes before timing out",
                            static_cast<int>(written), line.size()));
  }
  return {};
}

Result<std::vector<std::string>> SerialPort::read_lines() {
  std::vector<std::string> lines;
  if (port_ == nullptr) return lines;

  std::array<char, 512> buffer{};
  for (;;) {
    const auto read = sp_nonblocking_read(port_, buffer.data(), buffer.size());
    if (read < 0) {
      return fail(ErrorCode::serial_write_failed,
                  describe_sp(static_cast<sp_return>(read)));
    }
    if (read == 0) break;
    pending_.append(buffer.data(), static_cast<std::size_t>(read));
    if (static_cast<std::size_t>(read) < buffer.size()) break;
  }

  // Split on newlines, keeping any unterminated tail for the next call.
  std::size_t start = 0;
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    if (pending_[i] != '\n') continue;
    auto line = pending_.substr(start, i - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(std::move(line));
    start = i + 1;
  }
  pending_.erase(0, start);

  // A device that never sends a newline must not grow this without bound.
  if (pending_.size() > 4096) pending_.clear();

  return lines;
}

Result<TrackSink> make_serial_track_sink(const SerialConfig& config) {
  auto opened = SerialPort::open(config);
  if (!opened) return std::unexpected(opened.error());

  std::fprintf(stderr, "serial: %s @%d\n", config.port.string().c_str(), config.baud);

  // Owned by the sink lambda. The track stage is the only caller, so this state
  // is single-threaded despite living in a std::function.
  struct State {
    SerialPort port;
    SerialConfig config;
    bool connected = true;
    Clock::time_point next_attempt{};
  };
  auto state = std::make_shared<State>(State{.port = std::move(*opened), .config = config});

  TrackSink sink;
  sink.name = "serial";
  sink.write = [state](std::string_view line) -> Result<void> {
    const auto now = Clock::now();

    if (!state->connected) {
      // Retry on a timer rather than on every line: at track rate that would be
      // 25 open attempts a second against a device that is not there.
      if (now < state->next_attempt) {
        return fail(ErrorCode::serial_write_failed, "port is disconnected");
      }
      state->next_attempt = now + kReopenInterval;

      auto reopened = SerialPort::open(state->config);
      if (!reopened) return std::unexpected(reopened.error());
      state->port = std::move(*reopened);
      state->connected = true;
      std::fprintf(stderr, "serial: reconnected\n");
    }

    if (const auto written = state->port.write_line(line); !written) {
      state->connected = false;
      state->next_attempt = now + kReopenInterval;
      std::fprintf(stderr, "serial: %s\n", to_string(written.error()).c_str());
      return std::unexpected(written.error());
    }

    // Drain whatever the device said. `track` is answered with nothing, so in
    // steady state this is silent; anything here is a reboot or a complaint.
    if (auto replies = state->port.read_lines(); replies.has_value()) {
      for (const auto& reply : *replies) {
        std::fprintf(stderr, "device: %s\n", reply.c_str());
      }
    }
    return {};
  };
  return sink;
}

} // namespace capture_eye
