#pragma once

#include <algorithm>
#include <cmath>

#include "geometry.h"

namespace capture_eye {

// How a source frame maps into the model's square input.
//
// The image is scaled to fit and centred on a padded canvas rather than
// stretched. Aspect ratio matters here beyond image quality: the PID loop steers
// on the box centre, so distorting the geometry distorts the steering signal.
struct LetterboxTransform {
  int src_width = 0;
  int src_height = 0;
  int size = 0;      // canvas is size x size
  float scale = 1;   // source pixels -> canvas pixels
  int scaled_width = 0;
  int scaled_height = 0;
  int pad_x = 0;     // left padding, in canvas pixels
  int pad_y = 0;     // top padding, in canvas pixels
};

[[nodiscard]] constexpr LetterboxTransform letterbox_transform(int src_width, int src_height,
                                                               int size) {
  LetterboxTransform transform;
  transform.src_width = src_width;
  transform.src_height = src_height;
  transform.size = size;
  if (src_width <= 0 || src_height <= 0 || size <= 0) return transform;

  transform.scale = std::min(static_cast<float>(size) / static_cast<float>(src_width),
                             static_cast<float>(size) / static_cast<float>(src_height));
  transform.scaled_width =
      static_cast<int>(std::lround(static_cast<float>(src_width) * transform.scale));
  transform.scaled_height =
      static_cast<int>(std::lround(static_cast<float>(src_height) * transform.scale));
  transform.pad_x = (size - transform.scaled_width) / 2;
  transform.pad_y = (size - transform.scaled_height) / 2;
  return transform;
}

// Maps a model box back to source-frame pixels.
//
// The model emits centre/size normalised to the whole padded canvas, so the
// inverse is: scale up to canvas pixels, remove the padding, then renormalise
// against the scaled image rather than the canvas.
[[nodiscard]] constexpr BoxF box_from_model(float cx, float cy, float w, float h,
                                            const LetterboxTransform& transform) {
  if (transform.scaled_width <= 0 || transform.scaled_height <= 0) return BoxF{};

  const float canvas = static_cast<float>(transform.size);
  const float scaled_w = static_cast<float>(transform.scaled_width);
  const float scaled_h = static_cast<float>(transform.scaled_height);
  const float src_w = static_cast<float>(transform.src_width);
  const float src_h = static_cast<float>(transform.src_height);

  const float cx_src = (cx * canvas - static_cast<float>(transform.pad_x)) / scaled_w * src_w;
  const float cy_src = (cy * canvas - static_cast<float>(transform.pad_y)) / scaled_h * src_h;
  const float w_src = w * canvas / scaled_w * src_w;
  const float h_src = h * canvas / scaled_h * src_h;

  // Boxes routinely extend past the frame edge when a person is half in view;
  // clamping keeps the centre honest without discarding the detection.
  return clamp_box(box_from_center(cx_src, cy_src, w_src, h_src), src_w, src_h);
}

} // namespace capture_eye
