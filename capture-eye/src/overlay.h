#pragma once

#include <opencv2/core.hpp>

#include <chrono>
#include <optional>
#include <span>

#include "detection.h"
#include "track_message.h"

namespace capture_eye {

// Draws detections onto an image in place.
//
// The detections are always slightly stale — inference runs slower than capture,
// so the boxes belong to a frame 1-3 back. That is the deliberate trade the
// pipeline makes to keep the video at full rate, and the HUD reports the actual
// age so it is visible rather than merely known.
void draw_detections(cv::Mat& image, std::span<const Detection> people,
                     const std::optional<Detection>& selected);

struct HudInfo {
  std::optional<TrackMessage> track;  // exactly what went to the device
  std::chrono::milliseconds detection_age{0};
  double inference_ms = 0;
  std::uint64_t capture_fps = 0;
  std::uint64_t inference_fps = 0;
};

// Frame-centre reticle plus a corner readout.
//
// The reticle matters more than it looks: the PID loop drives the target's box
// centre toward the frame centre, so the gap between the crosshair and the
// reticle *is* the error signal being sent. Seeing them converge is how you
// confirm the sign conventions are right without instrumenting the firmware.
void draw_hud(cv::Mat& image, const HudInfo& info);

} // namespace capture_eye
