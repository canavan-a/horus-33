#include "error.h"

#include <catch2/catch_test_macros.hpp>

using namespace capture_eye;

TEST_CASE("exit_code_for: camera failures all map to 10") {
  CHECK(exit_code_for(ErrorCode::camera_open_failed) == 10);
  CHECK(exit_code_for(ErrorCode::camera_format_rejected) == 10);
  CHECK(exit_code_for(ErrorCode::camera_stream_failed) == 10);
}

TEST_CASE("exit_code_for: serial failures both map to 11") {
  CHECK(exit_code_for(ErrorCode::serial_open_failed) == 11);
  CHECK(exit_code_for(ErrorCode::serial_write_failed) == 11);
}

TEST_CASE("exit_code_for: config_invalid maps to 2, matching parse_args' own hardcoded exit") {
  CHECK(exit_code_for(ErrorCode::config_invalid) == 2);
}

TEST_CASE("exit_code_for: everything else keeps the generic 1") {
  CHECK(exit_code_for(ErrorCode::decode_failed) == 1);
  CHECK(exit_code_for(ErrorCode::model_fetch_failed) == 1);
  CHECK(exit_code_for(ErrorCode::model_hash_mismatch) == 1);
  CHECK(exit_code_for(ErrorCode::model_load_failed) == 1);
  CHECK(exit_code_for(ErrorCode::model_shape_unexpected) == 1);
  CHECK(exit_code_for(ErrorCode::inference_failed) == 1);
  CHECK(exit_code_for(ErrorCode::sink_failed) == 1);
}
