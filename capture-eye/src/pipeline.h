#pragma once

#include <memory>
#include <stop_token>
#include <vector>

#include "config.h"
#include "detector.h"
#include "error.h"
#include "frame_sink.h"
#include "track_sink.h"

namespace capture_eye {

// Owns every stage and every thread.
//
// Stages are std::jthread members of a local scope inside run(), so shutdown is
// structural: closing the channels wakes the waiters, and the jthreads join on
// scope exit. There are no detached threads and no way to forget a join.
class Pipeline {
public:
  struct Stages {
    Detector detector;
    std::vector<FrameSink> frame_sinks;
    TrackSink track_sink;
  };

  // Sinks that need the decoded frame size are built by this callback, after
  // the camera has told us what it actually granted. Asking for 1280x720 and
  // encoding whatever the driver decided to give would silently corrupt the
  // video stream.
  using SinkFactory = std::function<Result<std::vector<FrameSink>>(int width, int height, int fps)>;

  [[nodiscard]] static Result<std::unique_ptr<Pipeline>> create(const AppConfig& config,
                                                                Stages stages,
                                                                const SinkFactory& late_sinks);
  ~Pipeline();

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  // Blocks until the token is stopped, then drains and joins.
  [[nodiscard]] Result<void> run(std::stop_token token);

private:
  struct Impl;
  explicit Pipeline(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace capture_eye
