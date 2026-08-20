#include "clip_sink.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include <chrono>
#include <cstdio>
#include <format>
#include <vector>

#include "clip_policy.h"
#include "frame_mat.h"
#include "h264_encoder.h"

namespace capture_eye {
namespace {

using Clock = std::chrono::steady_clock;

// A bounded, preallocated-once ring of recent frames, fed unconditionally on
// every submit() so it is always warm by the time a clip actually needs to
// start. Deep-copies each frame in (AnnotatedFrame's pixels are only valid
// for the duration of submit — frame_sink.h), so this is the only place any
// clip-related memory grows with time, and it is capped at
// pre_roll_seconds * fps slots regardless of how long recording runs.
class PreRollBuffer {
public:
  PreRollBuffer(std::size_t capacity, int width, int height) : capacity_{capacity} {
    slots_.resize(capacity);
    timestamps_.resize(capacity);
    for (auto& slot : slots_) slot.create(height, width, CV_8UC3);
  }

  void push(const cv::Mat& image, Clock::time_point captured_at) {
    if (capacity_ == 0) return;
    image.copyTo(slots_[next_]);
    timestamps_[next_] = captured_at;
    next_ = (next_ + 1) % capacity_;
    if (count_ < capacity_) ++count_;
  }

  // Oldest first. Empty once drained by a clip start; the ring keeps filling
  // for the next one regardless.
  struct Entry {
    const cv::Mat& image;
    Clock::time_point captured_at;
  };
  template <typename Fn>
  void for_each_ordered(Fn&& fn) const {
    if (count_ == 0) return;
    const std::size_t start = (next_ + capacity_ - count_) % capacity_;
    for (std::size_t i = 0; i < count_; ++i) {
      const std::size_t index = (start + i) % capacity_;
      fn(Entry{slots_[index], timestamps_[index]});
    }
  }

private:
  std::size_t capacity_;
  std::vector<cv::Mat> slots_;
  std::vector<Clock::time_point> timestamps_;
  std::size_t next_ = 0;
  std::size_t count_ = 0;
};

[[nodiscard]] Result<std::unique_ptr<Encoder>> open_clip_file(const std::filesystem::path& path,
                                                              int width, int height, int fps,
                                                              int bitrate_kbps,
                                                              bool hardware_encode,
                                                              const std::filesystem::path& vaapi_device) {
  auto encoder = std::make_unique<Encoder>();

  AVFormatContext* format = nullptr;
  const std::string path_str = path.string();
  const int allocated = avformat_alloc_output_context2(&format, nullptr, "mp4", path_str.c_str());
  if (allocated < 0 || format == nullptr) {
    return fail(ErrorCode::sink_failed, std::format("mp4 output: {}", av_error(allocated)));
  }
  encoder->format.reset(format);

  if (const auto configured = configure_codec(*encoder, width, height, fps, bitrate_kbps,
                                              hardware_encode, vaapi_device);
      !configured) {
    return std::unexpected(configured.error());
  }

  // Moves the moov atom to the front so a fully-written clip is playable and
  // seekable without a second remux pass — the standard mitigation, though a
  // clip is not reliably seekable while still being actively written; callers
  // must only expose finished (renamed) files.
  av_opt_set(format->priv_data, "movflags", "faststart", 0);

  if ((format->oformat->flags & AVFMT_NOFILE) == 0) {
    if (const int opened = avio_open(&format->pb, path_str.c_str(), AVIO_FLAG_WRITE); opened < 0) {
      return fail(ErrorCode::sink_failed, std::format("open {}: {}", path_str, av_error(opened)));
    }
    encoder->needs_avio_close = true;
  }

  if (const int header = avformat_write_header(format, nullptr); header < 0) {
    return fail(ErrorCode::sink_failed, std::format("write header {}: {}", path_str, av_error(header)));
  }
  encoder->header_written = true;
  return encoder;
}

// Re-decodes the just-finished clip to grab a frame from the middle, so the
// thumbnail shows probable action rather than whatever was happening at the
// very first or last frame. A one-off cost paid once per finished clip, not
// per video frame, so a fresh cv::VideoCapture for it is fine — no reason to
// keep a decoder open across the whole recording just for this.
void generate_thumbnail(const std::filesystem::path& clip_path) {
  cv::VideoCapture capture(clip_path.string());
  if (!capture.isOpened()) {
    std::fprintf(stderr, "clip: thumbnail: cannot reopen %s\n", clip_path.string().c_str());
    return;
  }

  const double reported_count = capture.get(cv::CAP_PROP_FRAME_COUNT);
  cv::Mat middle;
  if (reported_count > 0) {
    capture.set(cv::CAP_PROP_POS_FRAMES, reported_count / 2.0);
    capture.read(middle);
  }
  if (middle.empty()) {
    // Some containers/backends don't report a usable frame count — fall back
    // to scanning the whole thing and keeping the middle-most frame seen.
    std::vector<cv::Mat> frames;
    cv::Mat frame;
    while (capture.read(frame)) frames.push_back(frame.clone());
    if (!frames.empty()) middle = frames[frames.size() / 2];
  }
  if (middle.empty()) {
    std::fprintf(stderr, "clip: thumbnail: no readable frame in %s\n", clip_path.string().c_str());
    return;
  }

  auto thumbnail_path = clip_path;
  thumbnail_path.replace_extension(".jpg");
  if (!cv::imwrite(thumbnail_path.string(), middle)) {
    std::fprintf(stderr, "clip: thumbnail: failed to write %s\n", thumbnail_path.string().c_str());
  }
}

class ClipSink {
public:
  ClipSink(ClippingConfig config, int width, int height, int fps,
          std::shared_ptr<ClipRuntime> runtime)
      : config_{std::move(config)},
        width_{width},
        height_{height},
        fps_{fps},
        runtime_{std::move(runtime)},
        preroll_{static_cast<std::size_t>(config_.pre_roll_seconds * fps), width, height} {
    policy_.stop_after_ticks = config_.stop_after_ticks;
  }

  [[nodiscard]] Result<void> submit(const AnnotatedFrame& annotated) {
    if (!runtime_->enabled()) {
      // Disabled live while a clip was in flight: finalize it now, from this
      // same overlay thread, rather than leaving it open indefinitely.
      if (active_ != nullptr) finish_clip();
      return {};
    }

    const cv::Mat image = mat_for(annotated.image);
    preroll_.push(image, annotated.captured_at);

    if (annotated.detection_seq != 0 && annotated.detection_seq != last_detection_seq_) {
      last_detection_seq_ = annotated.detection_seq;
      const auto decision = policy_.update(!annotated.detections.empty());
      runtime_->set_recording(decision.recording);

      if (decision.start_clip) {
        if (const auto started = start_clip(); !started) {
          std::fprintf(stderr, "clip: %s\n", to_string(started.error()).c_str());
          // Recording continues to be tracked by the policy even if the file
          // failed to open; the next start attempt is the next detection.
        }
      }
      if (decision.stop_clip) finish_clip();
    }

    if (active_ != nullptr) {
      if (const auto encoded = active_->encode(annotated); !encoded) return encoded;
    }
    return {};
  }

private:
  [[nodiscard]] Result<void> start_clip() {
    std::error_code ec;
    std::filesystem::create_directories(config_.output_dir, ec);

    const auto now = std::chrono::system_clock::now();
    const auto epoch_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    current_path_ = config_.output_dir / std::format("clip-{}-{}.mp4", epoch_ms, ++clip_counter_);
    const auto temp_path = current_path_;
    temp_path_ = current_path_;
    temp_path_ += ".tmp";

    auto encoder = open_clip_file(temp_path_, width_, height_, fps_, config_.bitrate_kbps,
                                  config_.hardware_encode, config_.vaapi_device);
    if (!encoder) return std::unexpected(encoder.error());
    active_ = std::move(*encoder);

    // Drain the pre-roll first — oldest to newest — so the clip includes the
    // moment just before the person was first detected, not just from this
    // instant onward. Goes through the exact same one-frame-at-a-time
    // Encoder::encode path as every live frame afterward.
    preroll_.for_each_ordered([&](const PreRollBuffer::Entry& entry) {
      Frame proxy;
      proxy.width = width_;
      proxy.height = height_;
      proxy.bytes = std::span{reinterpret_cast<std::byte*>(entry.image.data),
                              static_cast<std::size_t>(entry.image.total()) * entry.image.elemSize()};
      const AnnotatedFrame pre{.image = proxy,
                               .detections = {},
                               .selected = std::nullopt,
                               .seq = 0,
                               .captured_at = entry.captured_at,
                               .detection_seq = 0};
      if (const auto encoded = active_->encode(pre); !encoded) {
        std::fprintf(stderr, "clip: pre-roll frame: %s\n", to_string(encoded.error()).c_str());
      }
    });

    std::fprintf(stderr, "clip: recording -> %s\n", current_path_.string().c_str());
    return {};
  }

  void finish_clip() {
    if (active_ == nullptr) return;
    active_.reset();  // destructor writes the trailer and closes the file
    runtime_->set_recording(false);

    std::error_code ec;
    std::filesystem::rename(temp_path_, current_path_, ec);
    if (ec) {
      std::fprintf(stderr, "clip: rename %s -> %s failed: %s\n", temp_path_.string().c_str(),
                   current_path_.string().c_str(), ec.message().c_str());
      return;
    }
    std::fprintf(stderr, "clip: finished %s\n", current_path_.string().c_str());
    generate_thumbnail(current_path_);
  }

  ClippingConfig config_;
  int width_;
  int height_;
  int fps_;
  std::shared_ptr<ClipRuntime> runtime_;
  PreRollBuffer preroll_;
  ClipPolicy policy_;
  std::uint64_t last_detection_seq_ = 0;
  std::unique_ptr<Encoder> active_;
  std::filesystem::path current_path_;
  std::filesystem::path temp_path_;
  std::uint64_t clip_counter_ = 0;
};

} // namespace

Result<ClipSinkHandle> make_clip_sink(const ClippingConfig& config, int width, int height,
                                      int fps) {
  if (config.output_dir.empty()) {
    return fail(ErrorCode::config_invalid, "clip sink needs clipping.output_dir");
  }
  avformat_network_init();

  auto runtime = std::make_shared<ClipRuntime>();
  runtime->set_enabled(config.enabled);

  auto state = std::make_shared<ClipSink>(config, width, height, fps, runtime);

  FrameSink sink;
  sink.name = "clip";
  sink.submit = [state](const AnnotatedFrame& annotated) -> Result<void> {
    return state->submit(annotated);
  };
  return ClipSinkHandle{.sink = std::move(sink), .runtime = std::move(runtime)};
}

} // namespace capture_eye
