#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include "error.h"
#include "serial_queue.h"

namespace capture_eye {

// A transparent multi-client relay for the device link, listening on a Unix
// socket. It moves lines: it does not parse `set`, does not validate fields,
// and does not know what a control is. The one deliberate exception is
// rewriting `seq` (see dispatch_incoming) — required so two clients can both
// use the protocol's request/response correlation without colliding, since
// the device echoes seq verbatim and never validates or dedupes it.
//
// Commands read from clients go into the shared SerialQueue, which the single
// thread that owns the serial port (Pipeline's device_loop) drains — this is
// what keeps "capture-eye is the sole writer" true even with several relay
// clients attached at once.
//
// A slow or dead client is disconnected, never allowed to stall the pipeline:
// each client has its own bounded outbound queue and its own writer thread,
// so one stuck client cannot block delivery to any other, let alone the
// device thread that calls dispatch_incoming.
class ControlRelay {
public:
  [[nodiscard]] static Result<std::unique_ptr<ControlRelay>> create(
      const std::filesystem::path& socket_path, std::size_t max_clients, SerialQueue& to_device);
  ~ControlRelay();

  ControlRelay(const ControlRelay&) = delete;
  ControlRelay& operator=(const ControlRelay&) = delete;

  // Called by the device-owning thread for every line read from the device.
  // Routes acks/errs whose seq the relay itself assigned back to the one
  // client that sent the original command; everything else (hello,
  // descriptor, state, and anything unmatched) is broadcast to every client.
  void dispatch_incoming(const std::string& line);

  [[nodiscard]] std::size_t client_count() const;

private:
  struct Impl;
  explicit ControlRelay(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace capture_eye
