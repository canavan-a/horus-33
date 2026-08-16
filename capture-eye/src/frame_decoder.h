#pragma once

#include <cstddef>
#include <span>

#include "config.h"
#include "error.h"
#include "frame.h"

namespace capture_eye {

// Turns a raw v4l2 buffer into BGR24 pixels in a pool slab.
//
// MJPG needs a JPEG decode per frame (~6-12ms at 1080p), which is why the
// camera's format choice is a performance decision and not a detail: YUYV needs
// only a colour conversion but caps at 30fps, and 5fps at 1080p.
class FrameDecoder {
public:
  [[nodiscard]] static Result<FrameDecoder> create(std::uint32_t fourcc, int source_width,
                                                   int source_height, int decode_scale);

  // Size of the decoded image, which is the source size divided by the decode
  // scale. The frame pool must be built for these dimensions.
  [[nodiscard]] int output_width() const { return output_width_; }
  [[nodiscard]] int output_height() const { return output_height_; }

  // Decodes into destination's existing buffer; allocates nothing.
  [[nodiscard]] Result<void> decode(std::span<const std::byte> encoded, Frame& destination) const;

private:
  std::uint32_t fourcc_ = 0;
  int source_width_ = 0;
  int source_height_ = 0;
  int decode_scale_ = 1;
  int output_width_ = 0;
  int output_height_ = 0;
};

} // namespace capture_eye
