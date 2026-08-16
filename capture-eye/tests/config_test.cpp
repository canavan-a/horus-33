#include "config.h"

#include <catch2/catch_test_macros.hpp>

using namespace capture_eye;

TEST_CASE("merge: an absent optional never overwrites") {
  AppConfig base;
  base.capture.fps = 60;
  ConfigOverlay empty;
  const auto merged = merge(base, empty, empty);
  CHECK(merged.capture.fps == 60);
}

TEST_CASE("merge: file layers over defaults") {
  AppConfig base;
  ConfigOverlay file;
  file.capture.fps = 30;
  const auto merged = merge(base, file, {});
  CHECK(merged.capture.fps == 30);
}

TEST_CASE("merge: flags win over the file — the precedence this whole layer exists for") {
  AppConfig base;
  ConfigOverlay file;
  file.capture.fps = 30;
  ConfigOverlay flags;
  flags.capture.fps = 60;
  const auto merged = merge(base, file, flags);
  CHECK(merged.capture.fps == 60);
}

TEST_CASE("merge: a field the flags never touched still comes from the file") {
  AppConfig base;
  ConfigOverlay file;
  file.capture.fps = 30;
  file.capture.width = 1920;
  ConfigOverlay flags;
  flags.capture.fps = 60;  // only fps overridden on the command line
  const auto merged = merge(base, file, flags);
  CHECK(merged.capture.fps == 60);
  CHECK(merged.capture.width == 1920);  // from the file, untouched by flags
}

TEST_CASE("merge: every section merges independently") {
  AppConfig base;
  ConfigOverlay file;
  file.serial.port = std::filesystem::path{"/dev/ttyUSB0"};
  file.tracking.policy = TargetPolicy::largest_area;
  file.sink.bitrate_kbps = 8000;
  const auto merged = merge(base, file, {});
  CHECK(merged.serial.port == "/dev/ttyUSB0");
  CHECK(merged.tracking.policy == TargetPolicy::largest_area);
  CHECK(merged.sink.bitrate_kbps == 8000);
}

TEST_CASE("validate: defaults are valid") {
  CHECK(validate(AppConfig{}).has_value());
}

TEST_CASE("validate: rejects the same bad values the old inline flag checks caught") {
  AppConfig config;

  config.capture.fps = 0;
  CHECK_FALSE(validate(config).has_value());
  config.capture.fps = 60;

  config.capture.decode_scale = 3;
  CHECK_FALSE(validate(config).has_value());
  config.capture.decode_scale = 1;

  config.inference.conf_threshold = 1.5f;
  CHECK_FALSE(validate(config).has_value());
  config.inference.conf_threshold = 0.35f;

  config.inference.intra_op_threads = 0;
  CHECK_FALSE(validate(config).has_value());
  config.inference.intra_op_threads = 2;

  config.sink.bitrate_kbps = -1;
  CHECK_FALSE(validate(config).has_value());
  config.sink.bitrate_kbps = 4000;

  CHECK(validate(config).has_value());
}

TEST_CASE("validate: a bad value from the file is rejected exactly like a bad flag") {
  AppConfig base;
  ConfigOverlay file;
  file.capture.fps = -5;
  const auto merged = merge(base, file, {});
  const auto ok = validate(merged);
  CHECK_FALSE(ok.has_value());
  CHECK(ok.error().code == ErrorCode::config_invalid);
}

TEST_CASE("merge: flip flags default off and layer like any other bool") {
  AppConfig base;
  CHECK_FALSE(base.capture.flip_horizontal);
  CHECK_FALSE(base.capture.flip_vertical);

  ConfigOverlay file;
  file.capture.flip_vertical = true;
  const auto merged = merge(base, file, {});
  CHECK_FALSE(merged.capture.flip_horizontal);
  CHECK(merged.capture.flip_vertical);
}
