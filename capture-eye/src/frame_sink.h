#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "config.h"
#include "detection.h"
#include "error.h"
#include "frame.h"

namespace capture_eye {

// One annotated frame leaving the pipeline.
//
// The pixels belong to the overlay stage's scratch buffer and are valid only
// for the duration of the submit call. A sink that needs to keep a frame must
// copy it and bound its own queue.
struct AnnotatedFrame {
  const Frame& image;
  std::span<const Detection> detections;
  std::optional<Detection> selected;
  std::uint64_t seq = 0;
  std::chrono::steady_clock::time_point captured_at{};
  // The inference tick this frame's detections came from (DetectionResult's
  // own frame_seq), not this frame's own seq above. Overlay runs at capture
  // rate and repeats the same detection result across several frames when
  // inference is slower — a sink comparing consecutive detection_seq values
  // can tell a genuinely new detection tick from a stale repeat. 0 before the
  // first inference result ever arrives.
  std::uint64_t detection_seq = 0;
};

// Where annotated video goes. A value type, so adding a transport means writing
// a factory rather than a subclass.
//
// submit is called from the single overlay thread, in seq order, at capture
// rate. It must not block for longer than a frame interval. Returning an error
// is logged and counted, not fatal.
struct FrameSink {
  std::function<Result<void>(const AnnotatedFrame&)> submit;
  std::function<Result<void>()> flush;  // may be empty
  std::string name;
};

// Shows an annotated preview window. Debug aid, not the egress path.
[[nodiscard]] Result<FrameSink> make_preview_sink(const SinkConfig& config);

// Periodically writes the annotated frame to a JPEG, replacing it in place.
//
// Proves the sink seam works for a non-display consumer, and makes the overlay
// inspectable on a headless machine — which is also how it gets checked in CI.
[[nodiscard]] Result<FrameSink> make_snapshot_sink(const SinkConfig& config);

} // namespace capture_eye
