#pragma once

#include <filesystem>
#include <memory>

#include "clip_sink.h"
#include "error.h"

namespace capture_eye {

// A second, much simpler Unix socket than ControlRelay: host-only,
// request/response, no seq/ack machinery, because the only client is
// horus-server asking to toggle clipping or poll its status — low frequency,
// single logical concern. Newline-delimited single-line JSON in both
// directions:
//
//   -> {"cmd":"set_enabled","enabled":true}   <- {"ok":true,"enabled":true,"recording":false}
//   -> {"cmd":"status"}                        <- {"ok":true,"enabled":true,"recording":false}
//
// Off unless clipping.admin_socket_path is set, mirroring IngressConfig's
// "off unless configured" precedent.
class ClipAdmin {
public:
  [[nodiscard]] static Result<std::unique_ptr<ClipAdmin>> create(
      const std::filesystem::path& socket_path, std::shared_ptr<ClipRuntime> runtime);
  ~ClipAdmin();

  ClipAdmin(const ClipAdmin&) = delete;
  ClipAdmin& operator=(const ClipAdmin&) = delete;

private:
  struct Impl;
  explicit ClipAdmin(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace capture_eye
