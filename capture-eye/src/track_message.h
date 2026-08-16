#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "geometry.h"

namespace capture_eye {

// One `track` line as docs/protocol.md defines it. Exactly two shapes exist:
// a full target, or `lost`. There is no partial form — the device treats a
// track with a missing or non-numeric x/y as lost anyway
// (firmware/src/main.cpp:102).
struct TrackMessage {
  bool lost = false;
  float x = 0;  // [-1, 1], 0 = frame centre, +1 = right edge
  float y = 0;  // [-1, 1], 0 = frame centre, +1 = TOP edge
  float w = 0;  // [0, 1] fraction of frame width
  float h = 0;  // [0, 1] fraction of frame height
  float c = 0;  // [0, 1] confidence

  // Debugging only, off in normal operation. A non-zero seq makes the device
  // ack every message (firmware/src/main.cpp:113-115), doubling link traffic at
  // frame rate — but it is the only way to prove the device is receiving and
  // parsing exactly the bytes we sent.
  std::optional<std::uint32_t> seq;
};

// Converts a source-frame pixel box into protocol coordinates.
//
// The Y axis is flipped: the protocol's +1 is the TOP of the frame, while pixel
// rows count downward. Getting this wrong drives the gimbal away from the target
// instead of toward it, and the PID loop will happily run to its limit.
[[nodiscard]] TrackMessage track_from_box(BoxF box, int frame_width, int frame_height,
                                          float confidence);

// Serialises to a single NDJSON line including the trailing newline.
//
// Built with std::format rather than a JSON library: the shape is fixed, this is
// the hot path, and fixed 4-decimal output guarantees a ~70 byte line against
// the firmware's 511 byte limit while ruling out scientific notation.
// Confidence is always emitted — omitting it makes the device default to 1.0 and
// silently bypass its own min_conf gate (firmware/src/main.cpp:110).
[[nodiscard]] std::string encode_track(const TrackMessage& message);

} // namespace capture_eye
