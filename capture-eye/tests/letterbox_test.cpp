#include "letterbox.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "yolo_output.h"

using namespace capture_eye;
using Catch::Approx;

TEST_CASE("a portrait frame pads left and right") {
  // bus.jpg, the image the model interface was verified against.
  const auto t = letterbox_transform(810, 1080, 640);
  CHECK(t.scale == Approx(640.0f / 1080.0f));
  CHECK(t.scaled_width == 480);
  CHECK(t.scaled_height == 640);
  CHECK(t.pad_x == 80);
  CHECK(t.pad_y == 0);
}

TEST_CASE("a landscape frame pads top and bottom") {
  const auto t = letterbox_transform(1280, 720, 640);
  CHECK(t.scaled_width == 640);
  CHECK(t.scaled_height == 360);
  CHECK(t.pad_x == 0);
  CHECK(t.pad_y == 140);
}

TEST_CASE("a square frame needs no padding") {
  const auto t = letterbox_transform(640, 640, 640);
  CHECK(t.pad_x == 0);
  CHECK(t.pad_y == 0);
  CHECK(t.scale == Approx(1.0f));
}

TEST_CASE("degenerate sizes do not divide by zero") {
  CHECK(box_from_model(0.5f, 0.5f, 0.1f, 0.1f, letterbox_transform(0, 0, 640)).area() ==
        Approx(0.0f));
}

// A box filling the whole canvas must map back to the whole source frame; this
// catches sign and scale errors in both directions at once.
TEST_CASE("the full canvas maps back to the full frame") {
  const auto t = letterbox_transform(1280, 720, 640);
  // The image occupies the canvas centre; its own extent in normalised canvas
  // coordinates is the scaled height over the canvas height.
  const float h_norm = 360.0f / 640.0f;
  const auto box = box_from_model(0.5f, 0.5f, 1.0f, h_norm, t);
  CHECK(box.x1 == Approx(0.0f).margin(1.0));
  CHECK(box.y1 == Approx(0.0f).margin(1.0));
  CHECK(box.x2 == Approx(1280.0f).margin(1.0));
  CHECK(box.y2 == Approx(720.0f).margin(1.0));
}

TEST_CASE("the canvas centre maps to the frame centre") {
  const auto t = letterbox_transform(1280, 720, 640);
  const auto box = box_from_model(0.5f, 0.5f, 0.1f, 0.1f, t);
  CHECK(box.center_x() == Approx(640.0f).margin(1.0));
  CHECK(box.center_y() == Approx(360.0f).margin(1.0));
}

// Ground truth recorded from running the real model on bus.jpg: the leftmost
// person came back as cx=0.2564 under letterboxing and cx=0.1772 under a plain
// stretch. Those must describe the same person in source pixels.
TEST_CASE("letterboxed output agrees with stretched output on real model values") {
  const auto letterboxed = letterbox_transform(810, 1080, 640);
  const auto box = box_from_model(0.2564f, 0.6018f, 0.1778f, 0.4664f, letterboxed);

  const float stretched_cx = 0.1772f * 810.0f;
  CHECK(box.center_x() == Approx(stretched_cx).margin(4.0));

  // Sanity: a person roughly 1/6 of the frame wide, most of its height.
  CHECK(box.width() == Approx(0.1778f * 640.0f / 480.0f * 810.0f).margin(1.0));
  CHECK(box.center_y() == Approx(0.6018f * 1080.0f).margin(1.0));
}

TEST_CASE("boxes are clamped to the frame") {
  const auto t = letterbox_transform(1280, 720, 640);
  const auto box = box_from_model(0.5f, 0.5f, 4.0f, 4.0f, t);
  CHECK(box.x1 >= 0.0f);
  CHECK(box.y1 >= 0.0f);
  CHECK(box.x2 <= 1280.0f);
  CHECK(box.y2 <= 720.0f);
}
