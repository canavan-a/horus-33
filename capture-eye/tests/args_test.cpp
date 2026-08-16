#include "args.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <vector>

using namespace capture_eye;

namespace {

Result<Invocation> parse(std::initializer_list<std::string_view> args) {
  const std::vector<std::string_view> owned{args};
  return parse_args(owned);
}

} // namespace

TEST_CASE("defaults match the documented camera capability") {
  const auto inv = parse({});
  REQUIRE(inv.has_value());
  CHECK(inv->command == Command::run);
  CHECK(inv->config.capture.width == 1280);
  CHECK(inv->config.capture.height == 720);
  CHECK(fourcc_string(inv->config.capture.fourcc) == "MJPG");
  CHECK(inv->config.capture.fps == 60);
  CHECK(inv->config.serial.enabled);
  // Sending seq would make the device ack every frame; it must be opt-in.
  CHECK_FALSE(inv->config.serial.send_seq);
}

TEST_CASE("parse_size accepts WxH and rejects everything else") {
  const auto ok = parse_size("1920x1080");
  REQUIRE(ok.has_value());
  CHECK(ok->first == 1920);
  CHECK(ok->second == 1080);

  CHECK_FALSE(parse_size("1920").has_value());
  CHECK_FALSE(parse_size("1920x").has_value());
  CHECK_FALSE(parse_size("x1080").has_value());
  CHECK_FALSE(parse_size("1920X1080").has_value());  // capital X is not the separator
  CHECK_FALSE(parse_size("0x1080").has_value());
  CHECK_FALSE(parse_size("-4x8").has_value());
  CHECK_FALSE(parse_size("1920x1080p").has_value());
}

TEST_CASE("flags with values are applied") {
  const auto inv = parse({"--device", "/dev/video2", "--size", "640x480", "--fourcc", "YUYV",
                          "--fps", "30", "--conf", "0.5", "--policy", "largest"});
  REQUIRE(inv.has_value());
  CHECK(inv->config.capture.device == "/dev/video2");
  CHECK(inv->config.capture.width == 640);
  CHECK(fourcc_string(inv->config.capture.fourcc) == "YUYV");
  CHECK(inv->config.capture.fps == 30);
  CHECK(inv->config.inference.conf_threshold == 0.5f);
  CHECK(inv->config.tracking.policy == TargetPolicy::largest_area);
}

TEST_CASE("boolean flags take no value") {
  const auto inv = parse({"--no-serial", "--preview", "--offline", "--loose-format"});
  REQUIRE(inv.has_value());
  CHECK_FALSE(inv->config.serial.enabled);
  CHECK(inv->config.sink.preview);
  CHECK(inv->config.model.offline);
  CHECK_FALSE(inv->config.capture.strict_format);
}

TEST_CASE("modes short-circuit to their command") {
  const auto formats = parse({"--list-formats"});
  REQUIRE(formats.has_value());
  CHECK(formats->command == Command::list_formats);

  const auto dump = parse({"--dump-model-io"});
  REQUIRE(dump.has_value());
  CHECK(dump->command == Command::dump_model_io);

  // --help wins immediately, even with a broken flag after it.
  const auto help = parse({"--help", "--nonsense"});
  REQUIRE(help.has_value());
  CHECK(help->command == Command::help);
}

TEST_CASE("bad input is rejected rather than defaulted") {
  CHECK_FALSE(parse({"--nonsense"}).has_value());
  CHECK_FALSE(parse({"--fps"}).has_value());            // missing value
  CHECK_FALSE(parse({"--fps", "abc"}).has_value());
  CHECK_FALSE(parse({"--fps", "0"}).has_value());
  CHECK_FALSE(parse({"--fourcc", "MJPEG"}).has_value());  // must be exactly 4 chars
  CHECK_FALSE(parse({"--conf", "1.5"}).has_value());
  CHECK_FALSE(parse({"--decode-scale", "3"}).has_value());
  CHECK_FALSE(parse({"--policy", "whatever"}).has_value());
  CHECK_FALSE(parse({"--intra-threads", "0"}).has_value());

  const auto err = parse({"--nonsense"});
  CHECK(err.error().code == ErrorCode::config_invalid);
}

TEST_CASE("a flag value that looks like a flag is still consumed as a value") {
  // Otherwise `--device --size` would silently leave device at its default.
  const auto inv = parse({"--device", "--size"});
  REQUIRE(inv.has_value());
  CHECK(inv->config.capture.device == "--size");
}
