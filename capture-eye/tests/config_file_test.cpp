#include "config_file.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <format>
#include <fstream>
#include <string_view>

using namespace capture_eye;

namespace {

// A real temp file, because load_config_file does real filesystem I/O by
// design (config.h stays library-agnostic; config_file.cpp is the one edge).
struct TempFile {
  std::filesystem::path path;

  explicit TempFile(std::string_view contents) {
    path = std::filesystem::temp_directory_path() /
           std::format("capture-eye-config-test-{}.json", static_cast<void*>(this));
    std::ofstream out(path);
    out << contents;
  }

  ~TempFile() { std::filesystem::remove(path); }
};

} // namespace

TEST_CASE("load_config_file: a well-formed file populates the overlay") {
  TempFile file(R"({
    "capture": {"device": "/dev/video2", "fps": 30, "fourcc": "MJPG"},
    "tracking": {"policy": "largest"}
  })");

  const auto overlay = load_config_file(file.path);
  REQUIRE(overlay.has_value());
  REQUIRE(overlay->capture.device.has_value());
  CHECK(*overlay->capture.device == "/dev/video2");
  REQUIRE(overlay->capture.fps.has_value());
  CHECK(*overlay->capture.fps == 30);
  REQUIRE(overlay->tracking.policy.has_value());
  CHECK(*overlay->tracking.policy == TargetPolicy::largest_area);

  // Untouched fields stay absent — merge() must be free to layer flags on top.
  CHECK_FALSE(overlay->capture.width.has_value());
}

TEST_CASE("load_config_file: an unknown top-level key is a hard error") {
  TempFile file(R"({"cammera": {"fps": 30}})");
  const auto overlay = load_config_file(file.path);
  CHECK_FALSE(overlay.has_value());
  CHECK(overlay.error().code == ErrorCode::config_invalid);
}

TEST_CASE("load_config_file: an unknown key inside a section is a hard error") {
  TempFile file(R"({"capture": {"fsp": 30}})");
  const auto overlay = load_config_file(file.path);
  CHECK_FALSE(overlay.has_value());
}

TEST_CASE("load_config_file: wrong JSON type is rejected, never coerced") {
  TempFile file(R"({"capture": {"fps": "30"}})");
  const auto overlay = load_config_file(file.path);
  CHECK_FALSE(overlay.has_value());
}

TEST_CASE("load_config_file: malformed JSON is rejected") {
  TempFile file("{ not json");
  const auto overlay = load_config_file(file.path);
  CHECK_FALSE(overlay.has_value());
}

TEST_CASE("load_config_file: a missing file is rejected, not silently empty") {
  const auto overlay = load_config_file("/nonexistent/capture-eye-config.json");
  CHECK_FALSE(overlay.has_value());
}

TEST_CASE("load_config_file: an empty object is valid and produces an empty overlay") {
  TempFile file("{}");
  const auto overlay = load_config_file(file.path);
  REQUIRE(overlay.has_value());
  CHECK_FALSE(overlay->capture.fps.has_value());
}
