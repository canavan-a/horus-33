#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config.h"
#include "error.h"

namespace capture_eye {

enum class Command {
  run,
  list_formats,   // enumerate what the camera actually supports
  dump_model_io,  // print the model's real input/output tensor shapes
  detect_image,   // run the detector on one still image and print the boxes
  help,
};

struct Invocation {
  Command command = Command::run;
  AppConfig config;
  std::filesystem::path image;  // for detect_image
};

// Pure: no filesystem, no environment, no I/O. Takes the argument list without
// argv[0].
[[nodiscard]] Result<Invocation> parse_args(std::span<const std::string_view> args);

[[nodiscard]] std::string usage();

// "1280x720" -> {1280, 720}. Exposed for its own tests.
[[nodiscard]] Result<std::pair<int, int>> parse_size(std::string_view text);

} // namespace capture_eye
