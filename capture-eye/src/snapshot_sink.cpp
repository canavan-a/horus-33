#include <opencv2/imgcodecs.hpp>

#include <filesystem>
#include <format>
#include <memory>
#include <vector>

#include "frame_mat.h"
#include "frame_sink.h"

namespace capture_eye {

Result<FrameSink> make_snapshot_sink(const SinkConfig& config) {
  if (config.snapshot_path.empty()) {
    return fail(ErrorCode::config_invalid, "snapshot sink needs a path");
  }

  const auto parent = config.snapshot_path.parent_path();
  if (!parent.empty() && !std::filesystem::exists(parent)) {
    return fail(ErrorCode::sink_failed, std::format("{}: no such directory", parent.string()));
  }

  struct State {
    std::filesystem::path path;
    std::filesystem::path temporary;
    int every = 30;
    int countdown = 0;
  };
  auto state = std::make_shared<State>();
  state->path = config.snapshot_path;
  // The temporary keeps the real extension: OpenCV picks its encoder from the
  // filename, and "shot.jpg.part" has no writer at all.
  state->temporary = config.snapshot_path;
  state->temporary.replace_extension(".part" + config.snapshot_path.extension().string());
  state->every = config.snapshot_every;

  FrameSink sink;
  sink.name = "snapshot";
  sink.submit = [state](const AnnotatedFrame& annotated) -> Result<void> {
    if (--state->countdown > 0) return {};
    state->countdown = state->every;

    const cv::Mat image = mat_for(annotated.image);
    // Write then rename, so a reader never opens a half-written JPEG.
    //
    // imwrite throws on an unusable path rather than returning false, and an
    // exception escaping a sink would take down the whole pipeline over a
    // failed debug snapshot. It stops here.
    try {
      if (!cv::imwrite(state->temporary.string(), image)) {
        return fail(ErrorCode::sink_failed,
                    std::format("cannot write {}", state->temporary.string()));
      }
    } catch (const cv::Exception& error) {
      return fail(ErrorCode::sink_failed, error.what());
    }
    std::error_code ec;
    std::filesystem::rename(state->temporary, state->path, ec);
    if (ec) {
      return fail(ErrorCode::sink_failed, std::format("cannot replace {}: {}",
                                                      state->path.string(), ec.message()));
    }
    return {};
  };
  return sink;
}

} // namespace capture_eye
