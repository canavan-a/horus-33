#include "track_message.h"

#include <cmath>
#include <format>

namespace capture_eye {
namespace {

[[nodiscard]] float clamp_unit(float value) { return std::clamp(value, -1.0f, 1.0f); }

[[nodiscard]] bool all_finite(const TrackMessage& m) {
  return std::isfinite(m.x) && std::isfinite(m.y) && std::isfinite(m.w) && std::isfinite(m.h) &&
         std::isfinite(m.c);
}

} // namespace

TrackMessage track_from_box(BoxF box, int frame_width, int frame_height, float confidence) {
  if (frame_width <= 0 || frame_height <= 0) {
    return TrackMessage{.lost = true, .seq = std::nullopt};
  }

  const float width = static_cast<float>(frame_width);
  const float height = static_cast<float>(frame_height);

  const float cx = box.center_x() / width;   // [0, 1], left to right
  const float cy = box.center_y() / height;  // [0, 1], top to bottom

  TrackMessage message;
  message.x = clamp_unit(2.0f * cx - 1.0f);
  message.y = clamp_unit(1.0f - 2.0f * cy);  // flip: protocol's +y is up
  message.w = std::clamp(box.width() / width, 0.0f, 1.0f);
  message.h = std::clamp(box.height() / height, 0.0f, 1.0f);
  message.c = std::clamp(confidence, 0.0f, 1.0f);

  // A NaN box would serialise as `nan`, which the device cannot parse; it would
  // read as a malformed track and retire the target. Say so explicitly instead.
  if (!all_finite(message)) return TrackMessage{.lost = true, .seq = std::nullopt};
  return message;
}

std::string encode_track(const TrackMessage& message) {
  // seq is omitted entirely unless explicitly set. Absent reads as 0 on the
  // device, which is what suppresses the ack.
  const std::string seq =
      message.seq.has_value() ? std::format(R"(,"seq":{})", *message.seq) : std::string{};

  if (message.lost) {
    return std::format(R"({{"t":"track","lost":true{}}})"
                       "\n",
                       seq);
  }
  return std::format(
      R"({{"t":"track","x":{:.4f},"y":{:.4f},"w":{:.4f},"h":{:.4f},"c":{:.4f}{}}})"
      "\n",
      message.x, message.y, message.w, message.h, message.c, seq);
}

} // namespace capture_eye
