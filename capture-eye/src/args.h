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
  // Fully resolved: AppConfig defaults with the parsed flags layered on top,
  // already validated. Correct as-is when no --config file is given.
  AppConfig config;
  // The same flags, unmerged. When --config is also given, the caller must
  // redo the merge as merge(AppConfig{}, file_overlay, overlay) — a config
  // file sits *between* defaults and flags, so `config` above (which never
  // saw the file) is not the right value to use in that case.
  ConfigOverlay overlay;
  std::optional<std::filesystem::path> config_file;  // --config PATH
  std::filesystem::path image;  // for detect_image
};

// Pure: no filesystem, no environment, no I/O. Takes the argument list without
// argv[0]. --config's *path* is recorded but never opened here — parse_args
// does not touch the filesystem regardless of what flags are given.
[[nodiscard]] Result<Invocation> parse_args(std::span<const std::string_view> args);

[[nodiscard]] std::string usage();

// "1280x720" -> {1280, 720}. Exposed for its own tests.
[[nodiscard]] Result<std::pair<int, int>> parse_size(std::string_view text);

} // namespace capture_eye
