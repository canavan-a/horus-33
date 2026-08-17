#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "config.h"
#include "error.h"
#include "frame_sink.h"

namespace capture_eye {

// Live state behind a clip sink: whether recording is on, and whether a clip
// is active right now. clip_admin.cpp toggles/reads this from its own accept
// thread; the pipeline's overlay thread is the only one that ever encodes a
// frame or opens/closes a clip file — see clip_sink.cpp's submit().
class ClipRuntime {
public:
  struct Status {
    bool enabled = false;
    bool recording = false;
  };

  void set_enabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
  [[nodiscard]] bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  [[nodiscard]] Status status() const {
    const std::scoped_lock lock{recording_mutex_};
    return Status{.enabled = enabled(), .recording = recording_};
  }

  // Only ever called from the overlay thread, right before/after a clip
  // opens or closes — recording_mutex_ only exists so status() (called from
  // the admin thread) never observes a half-updated value.
  void set_recording(bool recording) {
    const std::scoped_lock lock{recording_mutex_};
    recording_ = recording;
  }

private:
  std::atomic<bool> enabled_{true};
  mutable std::mutex recording_mutex_;
  bool recording_ = false;
};

struct ClipSinkHandle {
  FrameSink sink;
  std::shared_ptr<ClipRuntime> runtime;
};

// Writes an H.264/MP4 clip to config.output_dir whenever a person is in
// frame, with a short pre-roll of video from just before the detection
// instant. One frame is encoded and written to disk per submit() call — see
// clip_sink.cpp — so memory use stays flat regardless of clip length; the
// only frames ever held in memory are the bounded pre-roll ring buffer.
[[nodiscard]] Result<ClipSinkHandle> make_clip_sink(const ClippingConfig& config, int width,
                                                    int height, int fps);

} // namespace capture_eye
